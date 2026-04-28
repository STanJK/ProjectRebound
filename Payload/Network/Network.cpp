// Network.cpp
#include "Network.h"
#include "../Config/Config.h"
#include "../ServerLogic/ServerLogic.h"
#include "../Debug/Debug.h"
#include "../SDK.hpp"
#include "../SDK/Engine_parameters.hpp"
#include "../Libs/json.hpp"
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <Windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

using namespace SDK;

// ======================================================
//  SECTION 4 — UTILITY HELPERS (network related)
// ======================================================

std::string StripHttpScheme(const std::string &backend)
{
    const std::string http = "http://";
    const std::string https = "https://";

    if (backend.rfind(http, 0) == 0)
        return backend.substr(http.length());

    if (backend.rfind(https, 0) == 0)
        return backend.substr(https.length());

    return backend;
}

nlohmann::json BuildServerStatusPayload()
{
    int playerCount = GetCurrentPlayerCount();

    std::string map = std::string(Config.MapName.begin(), Config.MapName.end());
    std::string mode = std::string(Config.FullModePath.begin(), Config.FullModePath.end());

    std::string state = "Unknown";

    // FIXED: Add proper null checks before dereferencing
    UWorld *World = UWorld::GetWorld();
    if (World && World->AuthorityGameMode && World->AuthorityGameMode->GameState)
    {
        APBGameState *GS = (APBGameState *)World->AuthorityGameMode->GameState;
        state = GS->RoundState.ToString();
    }

    nlohmann::json payload = {
        {"name", Config.ServerName},
        {"region", Config.ServerRegion},
        {"mode", mode},
        {"map", map},
        {"port", Config.ExternalPort},
        {"playerCount", playerCount},
        {"serverState", state}};

    return payload;
}

nlohmann::json BuildRoomHeartbeatPayload()
{
    int playerCount = GetCurrentPlayerCount();
    std::string state = "Unknown";

    UWorld *World = UWorld::GetWorld();
    if (World && World->AuthorityGameMode && World->AuthorityGameMode->GameState)
    {
        APBGameState *GS = (APBGameState *)World->AuthorityGameMode->GameState;
        state = GS->RoundState.ToString();
    }

    nlohmann::json payload = {
        {"hostToken", HostToken},
        {"playerCount", playerCount},
        {"serverState", state}};

    return payload;
}

// Send Message to Backend HTTP Helper
void SendServerStatus(const std::string &backend)
{
    bool useRoomHeartbeat = !HostRoomId.empty() && !HostToken.empty();
    nlohmann::json payload = useRoomHeartbeat ? BuildRoomHeartbeatPayload() : BuildServerStatusPayload();
    if (!useRoomHeartbeat && !HostRoomId.empty())
    {
        payload["roomId"] = HostRoomId;
        payload["hostToken"] = HostToken;
    }

    std::string body = payload.dump();
    std::string cleanBackend = StripHttpScheme(backend);

    size_t slash = cleanBackend.find('/');
    if (slash != std::string::npos)
        cleanBackend = cleanBackend.substr(0, slash);

    size_t colon = cleanBackend.find(':');
    if (colon == std::string::npos)
    {
        std::cout << "[ONLINE] Invalid backend address format." << std::endl;
        return;
    }

    std::string host = cleanBackend.substr(0, colon);
    std::string port = cleanBackend.substr(colon + 1);
    std::string path = useRoomHeartbeat
                           ? "/v1/rooms/" + HostRoomId + "/heartbeat"
                           : "/server/status";

    HINTERNET hSession = WinHttpOpen(L"BoundaryDLL/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession)
        return;

    std::wstring whost(host.begin(), host.end());
    INTERNET_PORT wport = (INTERNET_PORT)std::stoi(port);

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), wport, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        std::wstring(path.begin(), path.end()).c_str(),
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);

    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    BOOL bResults = WinHttpSendRequest(
        hRequest,
        L"Content-Type: application/json",
        -1,
        (LPVOID)body.c_str(),
        (DWORD)body.size(),
        (DWORD)body.size(),
        0);

    if (bResults)
        WinHttpReceiveResponse(hRequest, NULL);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    std::cout << "[ONLINE] Sent " << path << ": " << body << std::endl;
}

// 心跳线程（原本在 MainThread 中启动）
void StartHeartbeatThread()
{
    std::thread([]()
                {
        // Wait until Gamestate is Valid
        while (!UWorld::GetWorld() ||
            !UWorld::GetWorld()->AuthorityGameMode ||
            !UWorld::GetWorld()->AuthorityGameMode->GameState)
        {
            Sleep(100);
        }
        while (true)
        {
            int pc = GetCurrentPlayerCount();
            std::cout << "[HEARTBEAT] PlayerCount = " << pc << std::endl;

            if (!OnlineBackendAddress.empty())
            {
                SendServerStatus(OnlineBackendAddress);
            }

            Sleep(5000);
        } })
        .detach();
}