#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <random>

#define NOMINMAX

HANDLE g_ServerProcess = NULL;

std::string CurrentMap = "Warehouse";
std::string CurrentMode = "pve";
std::string LastMap = "";
std::atomic<bool> ServerRunning = false;

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
        if (msg.find("[HEARTBEAT]") != std::string::npos)
        {
            lastHeartbeatTime = std::chrono::steady_clock::now();
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

void LaunchServer()
{
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
    std::string modePath =
        (CurrentMode == "pve")
        ? "/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Hard.BP_PBGameMode_Rush_PVE_Hard_C"
        : "/Game/Online/GameMode/PBGameMode_Rush_BP.PBGameMode_Rush_BP_C";

    // Build command line
    std::wstring cmd =
        L".\\ProjectBoundarySteam-Win64-Shipping.exe "
        L"-log -server -nullrhi "
        L"-map=" + std::wstring(CurrentMap.begin(), CurrentMap.end()) + L" "
        L"-mode=" + std::wstring(modePath.begin(), modePath.end()) + L" "
        L"-port=7777 "
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
