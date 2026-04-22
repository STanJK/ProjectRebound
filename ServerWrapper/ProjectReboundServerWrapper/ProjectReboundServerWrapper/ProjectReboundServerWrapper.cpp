#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

enum class ServerState
{
    Stopped,
    Starting,
    Running,
    Stopping,
    Restarting
};

HANDLE g_ServerProcess = NULL;
DWORD g_ServerPid = 0;

const std::string DEFAULT_BACKEND = "ax48735790k.vicp.fun:3000";
std::string CurrentMap = "Warehouse";
std::string CurrentMode = "pvp";
std::string LastMap = "";
std::string CurrentDifficulty = "normal";
std::string OnlineBackend = DEFAULT_BACKEND;
std::string ServerName = "DefaultServer";
std::string ServerRegion = "CN";
int g_ServerPort = 7777;
int g_ExternalPort = g_ServerPort;

bool OfflineMode = false;

// Serializes all lifecycle transitions so input commands and background
// watcher threads cannot restart/kill/start the process at the same time.
std::mutex g_ServerMutex;
std::mutex g_LogMutex;
std::atomic<bool> ServerRunning = false;
std::atomic<bool> g_WrapperShuttingDown = false;
std::atomic<ServerState> g_ServerState{ ServerState::Stopped };
// Each launched process gets a generation number. Background threads only act
// if their captured generation still matches the current server instance.
std::atomic<uint64_t> g_ServerGeneration{ 0 };
std::atomic<int> g_ConsecutiveFailures{ 0 };
std::atomic<uint64_t> g_LastHeartbeatTickMs{ 0 };

std::chrono::steady_clock::time_point g_LastFailureTime;
const int MAX_FAILURES = 3;
const auto FAILURE_RESET_WINDOW = std::chrono::minutes(1);

//Forward Declaration
void LauncherLog(const std::string& msg);
void LaunchServer();
void RestartServer();
void KillServer();

bool LaunchServerLocked();
bool StopServerLocked();
void RequestRestart(bool rotateMap, const std::string& reason);
void PipeReader(HANDLE pipe, uint64_t generation);
void StartWatchdog(HANDLE processHandle, uint64_t generation);
void StartExitWatcher(HANDLE processHandle, uint64_t generation);

uint64_t SteadyNowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void ResetHeartbeatClock()
{
    g_LastHeartbeatTickMs.store(SteadyNowMs());
}

bool HasHeartbeatTimedOut(std::chrono::seconds timeout)
{
    const uint64_t last = g_LastHeartbeatTickMs.load();
    const uint64_t now = SteadyNowMs();
    return now > last + static_cast<uint64_t>(timeout.count()) * 1000ULL;
}

HANDLE DuplicateProcessHandle(HANDLE source)
{
    HANDLE duplicated = NULL;
    if (!DuplicateHandle(
        GetCurrentProcess(),
        source,
        GetCurrentProcess(),
        &duplicated,
        0,
        FALSE,
        DUPLICATE_SAME_ACCESS))
    {
        LauncherLog("DuplicateHandle failed. GetLastError=" + std::to_string(GetLastError()));
        return NULL;
    }

    return duplicated;
}

std::string CurrentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
    localtime_s(&tm, &t);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

std::ofstream logFile;

//Set the maplist for PVP and PVE
std::vector<std::string> Maps = {
    "CircularX", "DataCenter", "Dusty", "GangesRiver", "Oriolus",
    "RelayStation", "Warehouse", "MiniFarm", "Museum", "OSS"
};

std::vector<std::string> PvEMaps = {
    "CircularX", "DataCenter", "Warehouse", "MiniFarm", "OSS"
};

struct MapInfo {
    std::string name;
    bool pveBug;
};

std::vector<MapInfo> MapList = {
    {"OSS", false},
    {"MiniFarm", false},
    {"Warehouse", false},
    {"Dusty", true},
    {"DataCenter", false},
    {"CircularX", false},
    {"Interior_C", true},
    {"Museum_art", true},
    {"RelayStation", true},
    {"Oriolus", true},
    {"GangesRiver", true}
};

std::string PickRandomMapAvoidingLast()
{
    std::vector<std::string> candidates;

    // Build list of allowed maps (no PVEbug)
    for (const auto& m : MapList)
    {
        if (!m.pveBug && m.name != LastMap)
            candidates.push_back(m.name);
    }

    if (candidates.empty())
    {
        // fallback: if everything is forbidden or only 1 map exists
        return LastMap;
    }

    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, candidates.size() - 1);

    return candidates[dist(rng)];
}

//Console commands
void PrintMapList()
{
    LauncherLog("=== Available Maps ===");

    for (const auto& m : MapList)
    {
        if (m.pveBug)
            std::cout << m.name << "  [FORBIDDEN: PVE BUG]" << std::endl;
        else
            std::cout << m.name << std::endl;
    }

    LauncherLog("======================");
}

void SetMap(const std::string& name)
{
    std::lock_guard<std::mutex> lock(g_ServerMutex);

    for (const auto& m : MapList)
    {
        if (_stricmp(m.name.c_str(), name.c_str()) == 0)
        {
            if (m.pveBug)
            {
                LauncherLog("Map '" + name + "' is forbidden due to PVE bug.");
                return;
            }

            CurrentMap = m.name;
            LauncherLog("Map set to: " + CurrentMap);
            return;
        }
    }

    LauncherLog("Unknown map: " + name);
}

void SetMode(const std::string& mode)
{
    std::lock_guard<std::mutex> lock(g_ServerMutex);

    if (_stricmp(mode.c_str(), "pvp") == 0)
    {
        CurrentMode = "pvp";
        LauncherLog("Mode set to PvP.");
    }
    else if (_stricmp(mode.c_str(), "pve") == 0)
    {
        CurrentMode = "pve";
        LauncherLog("Mode set to PvE.");
    }
    else
    {
        LauncherLog("Invalid mode. Use: pvp or pve");
    }
}

void SetDifficulty(const std::string& diff)
{
    std::lock_guard<std::mutex> lock(g_ServerMutex);

    if (_stricmp(diff.c_str(), "easy") == 0)
    {
        CurrentDifficulty = "easy";
        LauncherLog("Difficulty set to EASY.");
    }
    else if (_stricmp(diff.c_str(), "normal") == 0)
    {
        CurrentDifficulty = "normal";
        LauncherLog("Difficulty set to NORMAL.");
    }
    else if (_stricmp(diff.c_str(), "hard") == 0)
    {
        CurrentDifficulty = "hard";
        LauncherLog("Difficulty set to HARD.");
    }
    else
    {
        LauncherLog("Invalid difficulty. Use: easy, normal, hard");
    }
}

bool StopServerLocked()
{
    if (!g_ServerProcess)
    {
        g_ServerPid = 0;
        ServerRunning.store(false);
        g_ServerState.store(ServerState::Stopped);
        return true;
    }

    LauncherLog("Stopping server...");
    g_ServerState.store(ServerState::Stopping);
    ServerRunning.store(false);

    // Retire the current instance before we touch the process so older watcher
    // threads cannot restart or inspect a server that is already being replaced.
    g_ServerGeneration.fetch_add(1);

    HANDLE process = g_ServerProcess;
    DWORD exitCode = 0;

    if (GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE)
    {
        if (!TerminateProcess(process, 0))
        {
            LauncherLog("TerminateProcess failed. GetLastError=" + std::to_string(GetLastError()));
            return false;
        }
    }

    const DWORD waitResult = WaitForSingleObject(process, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LauncherLog("ERROR: timed out waiting for server process to exit.");
        return false;
    }

    CloseHandle(process);
    g_ServerProcess = NULL;
    g_ServerPid = 0;
    g_ServerState.store(ServerState::Stopped);
    return true;
}

void KillServer()
{
    std::lock_guard<std::mutex> lock(g_ServerMutex);
    StopServerLocked();
}

void RestartServer()
{
    RequestRestart(false, "manual restart");
}

void RequestRestart(bool rotateMap, const std::string& reason)
{
    if (g_WrapperShuttingDown.load())
        return;

    std::lock_guard<std::mutex> lock(g_ServerMutex);

    const ServerState state = g_ServerState.load();
    if (state == ServerState::Starting ||
        state == ServerState::Stopping ||
        state == ServerState::Restarting)
    {
        LauncherLog("Restart ignored: server lifecycle transition already in progress.");
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - g_LastFailureTime < FAILURE_RESET_WINDOW)
        ++g_ConsecutiveFailures;
    else
        g_ConsecutiveFailures = 1;
    g_LastFailureTime = now;

    if (g_ConsecutiveFailures.load() >= MAX_FAILURES)
    {
        LauncherLog("CRITICAL: Server failed to restart 3 times in 1 minute. Stopping auto-restart.");
        MessageBoxA(NULL,
            "Server failed to restart repeatedly.\n"
            "Possible reasons:\n"
            "- Current port is occupied by another program.\n"
            "- Map file missing or corrupt.\n"
            "- Antivirus blocking the executable.\n\n"
            "Please check the logs and restart the launcher manually.",
            "Project Boundary Server Wrapper",
            MB_OK | MB_ICONERROR);
        g_ServerState.store(ServerState::Stopped);
        ServerRunning.store(false);
        g_ConsecutiveFailures = 0;
        return;
    }

    g_ServerState.store(ServerState::Restarting);
    LauncherLog("Restarting server (" + reason + ")...");

    if (rotateMap)
    {
        LastMap = CurrentMap;
        CurrentMap = PickRandomMapAvoidingLast();
        LauncherLog("Auto-rotating map to: " + CurrentMap);
    }

    if (!StopServerLocked())
        return;

    if (!LaunchServerLocked())
        LauncherLog("ERROR: restart failed.");
}

void InputThread()
{
    while (true)
    {
        std::string cmd;
        std::getline(std::cin, cmd);

        if (cmd == "maplist")
        {
            PrintMapList();
        }
        else if (cmd.rfind("setmap ", 0) == 0)
        {
            SetMap(cmd.substr(7));
        }
        else if (cmd.rfind("setmode ", 0) == 0)
        {
            SetMode(cmd.substr(8));
        }
        else if (cmd == "killserver")
        {
            KillServer();
        }
        else if (cmd == "restart")
        {
            RestartServer();
        }
        else if (cmd.rfind("difficulty ", 0) == 0)
        {
            SetDifficulty(cmd.substr(11));
        }
        else if (cmd == "online")
        {
            std::lock_guard<std::mutex> lock(g_ServerMutex);
            OnlineBackend = DEFAULT_BACKEND;
            OfflineMode = false;
            LauncherLog("Online mode enabled. Backend = " + OnlineBackend);
        }
        else if (cmd.rfind("online ", 0) == 0)
        {
            std::lock_guard<std::mutex> lock(g_ServerMutex);
            OnlineBackend = cmd.substr(7);
            OfflineMode = false;
            LauncherLog("Online mode enabled. Backend = " + OnlineBackend);
        }
        else if (cmd == "offline")
        {
            std::lock_guard<std::mutex> lock(g_ServerMutex);
            OfflineMode = true;
            LauncherLog("Offline mode enabled. Server will not contact backend.");
        }
        else if (cmd.rfind("servername ", 0) == 0)
        {
            std::lock_guard<std::mutex> lock(g_ServerMutex);
            ServerName = cmd.substr(11);
            LauncherLog("Server name set to: " + ServerName);
        }
        else if (cmd.rfind("serverregion ", 0) == 0)
        {
            std::lock_guard<std::mutex> lock(g_ServerMutex);
            ServerRegion = cmd.substr(13);
            LauncherLog("Server region set to: " + ServerRegion);
        }
        else if (cmd.rfind("setport ", 0) == 0)
        {
            std::string portStr = cmd.substr(8);
            try {
                int newPort = std::stoi(portStr);
                if (newPort < 1 || newPort > 65535) {
                    LauncherLog("Invalid port. Must be 1-65535.");
                }
                else {
                    std::lock_guard<std::mutex> lock(g_ServerMutex);
                    g_ServerPort = newPort;
                    LauncherLog("Server port set to: " + std::to_string(g_ServerPort));
                }
            }
            catch (...) {
                LauncherLog("Invalid port format.");
            }
        }
        else if (cmd.rfind("setexternal ", 0) == 0)
        {
            std::string portStr = cmd.substr(12);
            try {
                int newPort = std::stoi(portStr);
                if (newPort < 1 || newPort > 65535) {
                    LauncherLog("Invalid external port. Must be 1-65535.");
                }
                else {
                    std::lock_guard<std::mutex> lock(g_ServerMutex);
                    g_ExternalPort = newPort;
                    LauncherLog("External port set to: " + std::to_string(g_ExternalPort));
                }
            }
            catch (...) {
                LauncherLog("Invalid external port format.");
            }
        }
        else
        {
            LauncherLog("Unknown command.");
        }
    }
}

//Random map picker as default
std::string PickRandom(const std::vector<std::string>& list)
{
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, list.size() - 1);
    return list[dist(rng)];
}

void LauncherLog(const std::string& msg)
{
    std::string line = "[Launcher] " + msg;
    std::lock_guard<std::mutex> lock(g_LogMutex);

    logFile << line << std::endl;
    logFile.flush();
    std::cout << line << std::endl;
}

void PipeReader(HANDLE pipe, uint64_t generation)
{
    char buffer[4096];
    DWORD bytesRead = 0;

    while (true)
    {
        if (!ReadFile(pipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) || bytesRead == 0)
            break;

        buffer[bytesRead] = '\0';
        std::string msg(buffer);

        // Ignore heartbeats from stale process generations that may still be draining output during a restart.
        if (msg.find("[HEARTBEAT]") != std::string::npos && generation == g_ServerGeneration.load())
        {
            ResetHeartbeatClock();
            g_ConsecutiveFailures = 0;
            LauncherLog("Heartbeat received");
        }

        std::lock_guard<std::mutex> lock(g_LogMutex);
        logFile << msg;
        logFile.flush();
        std::cout << msg;
    }

    CloseHandle(pipe);
    LauncherLog("PipeReader thread ended.");
}

void HideGameWindow(DWORD pid)
{
    HWND hwnd = NULL;

    // Find the window belonging to the server process
    while ((hwnd = FindWindowExW(NULL, hwnd, NULL, NULL)) != NULL)
    {
        DWORD windowPID = 0;
        GetWindowThreadProcessId(hwnd, &windowPID);

        if (windowPID == pid)
        {
            ShowWindow(hwnd, SW_HIDE);
        }
    }
}

BOOL WINAPI ConsoleHandler(DWORD ctrlType)
{
    (void)ctrlType;
    g_WrapperShuttingDown.store(true);

    std::lock_guard<std::mutex> lock(g_ServerMutex);
    StopServerLocked();

    return FALSE; // allow normal Ctrl+C behavior
}

void StartWatchdog(HANDLE processHandle, uint64_t generation)
{
    std::thread([processHandle, generation]() {
        const auto timeout = std::chrono::seconds(60);

        while (true)
        {
            if (g_WrapperShuttingDown.load())
                break;

            if (generation != g_ServerGeneration.load())
                break;

            if (g_ServerState.load() != ServerState::Running)
                break;

            DWORD code = 0;
            if (!GetExitCodeProcess(processHandle, &code) || code != STILL_ACTIVE)
                break;

            if (HasHeartbeatTimedOut(timeout))
            {
                LauncherLog("Heartbeat timeout - server frozen.");
                RequestRestart(true, "heartbeat timeout");
                break;
            }

            Sleep(1000);
        }

        CloseHandle(processHandle);
        }).detach();
}

void StartExitWatcher(HANDLE processHandle, uint64_t generation)
{
    std::thread([processHandle, generation]() {
        WaitForSingleObject(processHandle, INFINITE);
        CloseHandle(processHandle);

        if (g_WrapperShuttingDown.load())
            return;

        if (generation != g_ServerGeneration.load())
            return;

        if (g_ServerState.load() != ServerState::Running)
            return;

        LauncherLog("Server exited unexpectedly.");
        RequestRestart(true, "process exit");
        }).detach();
}

bool IsPortAvailable(int port, bool useTCP = false)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;

    int sockType = useTCP ? SOCK_STREAM : SOCK_DGRAM;
    int protocol = useTCP ? IPPROTO_TCP : IPPROTO_UDP;
    SOCKET sock = socket(AF_INET, sockType, protocol);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    int result = bind(sock, (sockaddr*)&addr, sizeof(addr));
    closesocket(sock);
    WSACleanup();

    return result != SOCKET_ERROR;
}

bool LaunchServerLocked()
{
    if (g_WrapperShuttingDown.load())
        return false;

    const ServerState state = g_ServerState.load();
    if (state == ServerState::Starting || state == ServerState::Running)
    {
        LauncherLog("Launch ignored: server is already starting or running.");
        return false;
    }

    int serverPort = g_ServerPort;
    if (!IsPortAvailable(serverPort)) {
        LauncherLog("ERROR: UDP Port " + std::to_string(serverPort) + " is already in use!");
        g_ServerState.store(ServerState::Stopped);
        ServerRunning.store(false);
        return false;
    }

    g_ServerState.store(ServerState::Starting);
    ServerRunning.store(false);
    LauncherLog("Launching server process...");

    SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE readPipe = NULL;
    HANDLE writePipe = NULL;

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        LauncherLog("CreatePipe failed. GetLastError=" + std::to_string(GetLastError()));
        g_ServerState.store(ServerState::Stopped);
        return false;
    }

    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0))
    {
        LauncherLog("SetHandleInformation failed. GetLastError=" + std::to_string(GetLastError()));
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        g_ServerState.store(ServerState::Stopped);
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};

    std::string modePath;

    if (CurrentMode == "pve")
    {
        if (CurrentDifficulty == "easy")
            modePath = "/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Easy.BP_PBGameMode_Rush_PVE_Easy_C";
        else if (CurrentDifficulty == "hard")
            modePath = "/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Hard.BP_PBGameMode_Rush_PVE_Hard_C";
        else
            modePath = "/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Normal.BP_PBGameMode_Rush_PVE_Normal_C";
    }
    else
    {
        modePath = "/Game/Online/GameMode/PBGameMode_Rush_BP.PBGameMode_Rush_BP_C";
    }

    std::wstring cmd =
        L".\\ProjectBoundarySteam-Win64-Shipping.exe "
        L"-log -server -nullrhi "
        L"-map=" + std::wstring(CurrentMap.begin(), CurrentMap.end()) + L" "
        L"-mode=" + std::wstring(modePath.begin(), modePath.end()) + L" "
        L"-port=" + std::to_wstring(serverPort) + L" "
        L"-external=" + std::to_wstring(g_ExternalPort) + L" "
        + (CurrentMode == "pve" ? L"-pve " : L"");

    std::wstring wName(ServerName.begin(), ServerName.end());
    cmd += L"-servername=" + wName + L" ";

    std::wstring wRegion(ServerRegion.begin(), ServerRegion.end());
    cmd += L"-serverregion=" + wRegion + L" ";

    if (!OfflineMode && !OnlineBackend.empty())
    {
        std::wstring wOnline(OnlineBackend.begin(), OnlineBackend.end());
        cmd += L"-online=" + wOnline + L" ";
    }

    if (!CreateProcessW(
        NULL,
        cmd.data(),
        NULL,
        NULL,
        TRUE,
        0,
        NULL,
        NULL,
        &si,
        &pi))
    {
        LauncherLog("Failed to launch server! GetLastError=" + std::to_string(GetLastError()));
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        g_ServerState.store(ServerState::Stopped);
        return false;
    }

    CloseHandle(writePipe);
    CloseHandle(pi.hThread);

    g_ServerProcess = pi.hProcess;
    g_ServerPid = pi.dwProcessId;

    // Publish the new generation before creating watcher threads so stale
    // threads can detect they no longer belong to the active process.
    const uint64_t generation = g_ServerGeneration.fetch_add(1) + 1;
    ResetHeartbeatClock();

    ServerRunning.store(true);
    g_ServerState.store(ServerState::Running);

    std::thread(PipeReader, readPipe, generation).detach();

    HANDLE watchdogHandle = DuplicateProcessHandle(pi.hProcess);
    if (watchdogHandle)
        StartWatchdog(watchdogHandle, generation);

    HANDLE exitWatcherHandle = DuplicateProcessHandle(pi.hProcess);
    if (exitWatcherHandle)
        StartExitWatcher(exitWatcherHandle, generation);

    LauncherLog("Server launched. PID = " + std::to_string(pi.dwProcessId));

    HideGameWindow(pi.dwProcessId);
    LauncherLog("Server window hidden.");
    return true;
}

void LaunchServer()
{
    std::lock_guard<std::mutex> lock(g_ServerMutex);
    LaunchServerLocked();
}

int main()
{
    std::string cmdLine = GetCommandLineA();
    size_t pos = cmdLine.find("-port=");
    if (pos != std::string::npos) {
        pos += 6;
        size_t end = cmdLine.find(" ", pos);
        std::string portStr = cmdLine.substr(pos, end - pos);
        g_ServerPort = std::stoi(portStr);
    }
    // default external = server port
    g_ExternalPort = g_ServerPort;
    ResetHeartbeatClock();
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // Create logs folder
    std::filesystem::create_directory("logs");

    // Build timestamped log file
    std::string logPath = "logs/log-" + CurrentTimestamp() + ".txt";
    logFile.open(logPath, std::ios::app);

    LauncherLog("Logging to: " + logPath);
    LauncherLog("Wrapper started.");

    // Start input thread (setmap, setmode, restart, killserver, etc.)
    std::thread(InputThread).detach();

    // Launch the server using CURRENT map + mode
    LaunchServer();

    // Main thread just idles forever
    while (true)
    {
        Sleep(1000);
    }
}
