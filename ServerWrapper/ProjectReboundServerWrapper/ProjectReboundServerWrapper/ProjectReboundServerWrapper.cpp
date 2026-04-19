#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <random>
#pragma comment(lib, "ws2_32.lib")


#define NOMINMAX

HANDLE g_ServerProcess = NULL;

std::string CurrentMap = "Warehouse";
std::string CurrentMode = "pve";
std::string LastMap = "";
std::string CurrentDifficulty = "normal";
std::atomic<bool> ServerRunning = false;
int g_ConsecutiveFailures = 0;
std::chrono::steady_clock::time_point g_LastFailureTime;
const int MAX_FAILURES = 3;
const auto FAILURE_RESET_WINDOW = std::chrono::minutes(1);

//Forward Declaration
void LauncherLog(const std::string& msg);
void LaunchServer();

std::chrono::steady_clock::time_point lastHeartbeatTime;

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

void KillServer()
{
    if (g_ServerProcess)
    {
        LauncherLog("Killing server...");
        TerminateProcess(g_ServerProcess, 0);
        CloseHandle(g_ServerProcess);
        g_ServerProcess = NULL;
        ServerRunning = false;
    }
    else
    {
        LauncherLog("No server to kill.");
        ServerRunning = false;
    }
}

void RestartServer()
{
	//Check if mutiple failures happen within the reset window
    auto now = std::chrono::steady_clock::now();
    if (now - g_LastFailureTime < FAILURE_RESET_WINDOW) {
        g_ConsecutiveFailures++;
    }
    else {
        g_ConsecutiveFailures = 1;
    }
    g_LastFailureTime = now;

	// Exceed max failures, stop auto-restart and alert user
    if (g_ConsecutiveFailures >= MAX_FAILURES) {
        LauncherLog("CRITICAL: Server failed to start 3 times in 1 minute. Stopping auto-restart.");
        MessageBoxA(NULL,
            "Server failed to start repeatedly.\n"
            "Possible reasons:\n"
            "- Port 7777 is occupied by another program.\n"
            "- Map file missing or corrupt.\n"
            "- Antivirus blocking the executable.\n\n"
            "Please check the logs and restart the launcher manually.",
            "Project Boundary Server Wrapper",
            MB_OK | MB_ICONERROR);
        ServerRunning = false;
        g_ConsecutiveFailures = 0; //reset counter
        return;
    }

    LauncherLog("Restarting server...");
    KillServer();
    Sleep(500);
    LaunchServer();
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
            std::string diff = cmd.substr(11);
            SetDifficulty(diff);
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

    // Write to log file
    logFile << line << std::endl;
    logFile.flush();

    // Write to console
    std::cout << line << std::endl;
}

void PipeReader(HANDLE pipe)
{
    char buffer[4096];
    DWORD bytesRead;

    while (true)
    {
        if (!ReadFile(pipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) || bytesRead == 0)
            break;

        buffer[bytesRead] = '\0';

        std::string msg(buffer);

        // Detect heartbeat
        if (msg.find("[HEARTBEAT]") != std::string::npos) {
            lastHeartbeatTime = std::chrono::steady_clock::now();
			g_ConsecutiveFailures = 0;  //Clear counter on successful heartbeat
            LauncherLog("Heartbeat received");
        }

        // Write raw game output
        logFile << msg;
        logFile.flush();

        // Also print to wrapper console
        std::cout << msg;
    }
    LauncherLog("PipeReader thread ended.");
    ServerRunning = false;
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
    // Kill the server if wrapper is closing
    if (g_ServerProcess)
    {
        TerminateProcess(g_ServerProcess, 0);
    }

    return FALSE; // allow normal Ctrl+C behavior
}

void StartWatchdog()
{
    std::thread([]() {
        const auto timeout = std::chrono::seconds(10);

        while (ServerRunning)
        {
            DWORD code = 0;
            GetExitCodeProcess(g_ServerProcess, &code);

            // If process is dead, exit watcher will restart it
            if (code != STILL_ACTIVE)
            {
                LauncherLog("Watchdog: server exited, skipping timeout restart.");
                return;
            }

            auto now = std::chrono::steady_clock::now();

            if (now - lastHeartbeatTime > timeout)
            {
                LauncherLog("Heartbeat timeout — server frozen.");

                LastMap = CurrentMap;
                CurrentMap = PickRandomMapAvoidingLast();

                LauncherLog("Auto-rotating map to: " + CurrentMap);

                RestartServer();
                return;
            }

            Sleep(1000);
        }
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

void LaunchServer()
{
    int serverPort = 7777;
    if (!IsPortAvailable(serverPort)) {
        LauncherLog("ERROR: UDP Port " + std::to_string(serverPort) + " is already in use!");
        return;
    }
    LastMap = CurrentMap;
    LauncherLog("Launching server process...");

    // Create pipes
    SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE readPipe, writePipe;

    CreatePipe(&readPipe, &writePipe, &sa, 0);
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};

    // Build mode path
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

    // Build command line
    std::wstring cmd =
        L".\\ProjectBoundarySteam-Win64-Shipping.exe "
        L"-log -server -nullrhi "
        L"-map=" + std::wstring(CurrentMap.begin(), CurrentMap.end()) + L" "
        L"-mode=" + std::wstring(modePath.begin(), modePath.end()) + L" "
        L"-port=" + std::to_wstring(serverPort) + L" "
        + (CurrentMode == "pve" ? L"-pve" : L"");

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
        LauncherLog("Failed to launch server!");
        return;
    }

    g_ServerProcess = pi.hProcess;
    CloseHandle(writePipe);

    // Pipe reader
    std::thread reader(PipeReader, readPipe);
    reader.detach();

    LauncherLog("Server launched. PID = " + std::to_string(pi.dwProcessId));

    Sleep(500);


    // Exit watcher
    std::thread([=]() {
        while (true)
        {
            DWORD code = 0;
            if (!GetExitCodeProcess(g_ServerProcess, &code))
                break;

            if (code != STILL_ACTIVE)
            {
                LauncherLog("Server exited — rotating map.");

                ServerRunning = false;

                LastMap = CurrentMap;
                CurrentMap = PickRandomMapAvoidingLast();

                RestartServer();
                return;
            }

            Sleep(1000);
        }
        }).detach();

    HideGameWindow(pi.dwProcessId);
    LauncherLog("Server window hidden.");
    ServerRunning = true;
    StartWatchdog();
}

int main()
{
    //set port, this part should belongs to a outside function later
    int g_ServerPort = 7777;
    std::string cmdLine = GetCommandLineA();
    size_t pos = cmdLine.find("-port=");
    if (pos != std::string::npos) {
        pos += 6;
        size_t end = cmdLine.find(" ", pos);
        std::string portStr = cmdLine.substr(pos, end - pos);
        g_ServerPort = std::stoi(portStr);
    }
    lastHeartbeatTime = std::chrono::steady_clock::now();
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