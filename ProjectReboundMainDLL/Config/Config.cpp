// Config.cpp
#include "Config.h"
#include <Windows.h>
#include <iostream>
#include "../Logging/LogManager.h"

// Central server ip
std::string OnlineBackendAddress = "";

// Server registration
std::string RegistrationToken = "";
std::string g_ServerId = "";
std::string g_ServerToken = "";

// Room heartbeat credentials from the desktop browser/match server
std::string HostRoomId = "";
std::string HostToken = "";

// IP from the server browser
std::string MatchIP = "";
std::string MatchPipeName = "";
std::mutex MatchIPMutex;

ServerConfig Config{};
bool amServer = false;

// Set up the dll to get values from the wrapper
std::string GetCmdValue(const std::string &key)
{
    std::string cmd = GetCommandLineA();
    size_t pos = cmd.find(key);
    if (pos == std::string::npos)
        return "";

    pos += key.length();
    size_t end = cmd.find(" ", pos);
    if (end == std::string::npos)
        end = cmd.length();

    return cmd.substr(pos, end - pos);
}

void LoadConfig()
{
    std::string cmd = GetCommandLineA();

    // PvE flag
    Config.IsPvE = cmd.find("-pve") != std::string::npos;

    // Map
    std::string mapArg = GetCmdValue("-map=");
    if (!mapArg.empty())
    {
        Config.MapName = std::wstring(mapArg.begin(), mapArg.end());
    }
    else
    {
        // fallback to something safe
        Config.MapName = L"Warehouse";
    }

    // Mode
    std::string modeArg = GetCmdValue("-mode=");
    if (!modeArg.empty())
    {
        Config.FullModePath = std::wstring(modeArg.begin(), modeArg.end());
    }
    else
    {
        // fallback based on PvE
        Config.FullModePath = Config.IsPvE
                                  ? L"/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Hard.BP_PBGameMode_Rush_PVE_Hard_C"
                                  : L"/Game/Online/GameMode/PBGameMode_Rush_BP.PBGameMode_Rush_BP_C";
    }

    // Port
    std::string portArg = GetCmdValue("-port=");
    if (!portArg.empty())
    {
        Config.Port = std::stoi(portArg);
    }
    else
    {
        Config.Port = 7777;
    }

    // External port
    std::string externalArg = GetCmdValue("-external=");
    if (!externalArg.empty())
    {
        Config.ExternalPort = std::stoi(externalArg);
    }
    else
    {
        Config.ExternalPort = Config.Port; // default same as internal
    }

    ServerLog("[SERVER] External port: " + std::to_string(Config.ExternalPort));

    // Name
    std::string serverNameArg = GetCmdValue("-servername=");
    if (!serverNameArg.empty())
    {
        Config.ServerName = serverNameArg;
        ServerLog("[SERVER] Server name: " + serverNameArg);
    }
    // Region
    std::string serverRegionArg = GetCmdValue("-serverregion=");
    if (!serverRegionArg.empty())
    {
        Config.ServerRegion = serverRegionArg;
        ServerLog("[SERVER] Server region: " + serverRegionArg);
    }
    // Min players (still used in TickFlush)
    Config.MinPlayersToStart = Config.IsPvE ? 1 : 2;

    // Online check if contact central server
    std::string onlineArg = GetCmdValue("-online=");
    if (!onlineArg.empty())
    {
        OnlineBackendAddress = onlineArg;
        ServerLog("[SERVER] Online backend: " + OnlineBackendAddress);
    }

    std::string roomIdArg = GetCmdValue("-roomid=");
    if (!roomIdArg.empty())
    {
        HostRoomId = roomIdArg;
        ServerLog("[SERVER] Host room id: " + HostRoomId);
    }

    std::string hostTokenArg = GetCmdValue("-hosttoken=");
    if (!hostTokenArg.empty())
    {
        HostToken = hostTokenArg;
        ServerLog("[SERVER] Host token received.");
    }

    std::string serverIdArg = GetCmdValue("-serverid=");
    if (!serverIdArg.empty())
    {
        Config.ServerUniqueId = serverIdArg;
        ServerLog("[SERVER] Server ID: " + serverIdArg);
    }

    std::string tokenArg = GetCmdValue("-registrationtoken=");
    if (!tokenArg.empty())
    {
        RegistrationToken = tokenArg;
        ServerLog("[SERVER] Registration token received.");
    }

    MatchPipeName = GetCmdValue("-pipe=");
    if (!MatchPipeName.empty())
    {
        ServerLog("[SERVER] Command pipe name: " + MatchPipeName);
    }
}

void LoadClientConfig()
{
    std::string matchArg = GetCmdValue("-match=");
    if (!matchArg.empty())
    {
        {
            std::lock_guard<std::mutex> lock(MatchIPMutex);
            MatchIP = matchArg;
        }
        ClientDebugLog("[CLIENT] Auto-match target: " + matchArg);
    }

    MatchPipeName = GetCmdValue("-pipe=");
    if (!MatchPipeName.empty())
    {
        ClientDebugLog("[CLIENT] Command pipe name: " + MatchPipeName);
    }

    // Diagnostic log flags — gate non-essential output behind explicit opt-in.
    // -serverdebuglog  enables ServerDebugLog() output (diagnostic server-side logs)
    // -clientdebuglog  enables ClientDebugLog() output (diagnostic client-side logs)
    if (std::string(GetCommandLineA()).find("-serverdebuglog") != std::string::npos)
    {
        ServerDebugLogEnabled = true;
    }
    if (std::string(GetCommandLineA()).find("-clientdebuglog") != std::string::npos)
    {
        ClientDebugLogEnabled = true;
    }
}
