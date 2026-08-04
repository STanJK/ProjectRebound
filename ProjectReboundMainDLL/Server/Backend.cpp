// Backend.cpp
#include "Backend.h"
#include "../Config/Config.h"
#include "RoundManager.h"
#include "../Logging/LogManager.h"
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

constexpr const char* BACKEND_PRIMARY  = "https://api.project-rebound.space";
constexpr const char* BACKEND_FALLBACK = "https://cnapi.project-rebound.space";
constexpr const char* REG_TOKEN_PLACEHOLDER = "YOUR_TOKEN_HERE";

// Track which backend is currently working
static std::string g_CurrentBackend = BACKEND_PRIMARY;

namespace
{
    bool BuildHttpTarget(const std::string &backend, std::string &host, INTERNET_PORT &port)
    {
        std::string cleanBackend = StripHttpScheme(backend);

        size_t slash = cleanBackend.find('/');
        if (slash != std::string::npos)
            cleanBackend = cleanBackend.substr(0, slash);

        size_t colon = cleanBackend.find(':');
        if (colon == std::string::npos)
        {
            // No port — use default based on scheme
            host = cleanBackend;
            bool isHttps = backend.rfind("https://", 0) == 0;
            port = isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
            return true;
        }

        host = cleanBackend.substr(0, colon);
        std::string portText = cleanBackend.substr(colon + 1);

        try
        {
            int parsedPort = std::stoi(portText);
            if (parsedPort <= 0 || parsedPort > 65535)
                return false;

            port = static_cast<INTERNET_PORT>(parsedPort);
            return true;
        }
        catch (...)
        {
            std::cout << "[ONLINE] Invalid backend port." << std::endl;
            return false;
        }
    }

    bool SendJsonPost(const std::string &backend, const std::string &path,
                      const nlohmann::json &payload, const char *logPrefix,
                      const std::string &extraHeader = "")
    {
        if (IsServerShutdownRequested()) {
            OutputDebugStringA("[EXIT-GUARD] SendJsonPost skipped (shutdown)\n");
            return false;
        }

        std::string host;
        INTERNET_PORT port = 0;
        if (!BuildHttpTarget(backend, host, port))
            return false;

        const std::string body = payload.dump();
        bool isHttps = backend.rfind("https://", 0) == 0;

        HINTERNET hSession = WinHttpOpen(L"BoundaryDLL/0.7.0",
                                         WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession)
            return false;

        WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);

        std::wstring whost(host.begin(), host.end());
        HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), port, 0);
        if (!hConnect)
        {
            WinHttpCloseHandle(hSession);
            return false;
        }

        std::wstring wpath(path.begin(), path.end());
        DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"POST", wpath.c_str(),
            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

        if (!hRequest)
        {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        // Build headers: Content-Type + optional extra header
        std::wstring headers = L"Content-Type: application/json\r\n";
        if (!extraHeader.empty())
        {
            std::wstring wExtra(extraHeader.begin(), extraHeader.end());
            headers += wExtra + L"\r\n";
        }

        BOOL ok = WinHttpSendRequest(
            hRequest, headers.c_str(), -1,
            (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);

        if (ok)
            ok = WinHttpReceiveResponse(hRequest, NULL);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        std::cout << logPrefix << (ok ? " Sent " : " Failed ") << path << ": " << body << std::endl;
        return ok == TRUE;
    }
}

// ======================================================
//  Utility helpers (network related)
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
        {"serverState", state},
        {"serverId", Config.ServerUniqueId}};

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
    // Token placeholder check — skip registration, print banner
    if (RegistrationToken.empty() || RegistrationToken == REG_TOKEN_PLACEHOLDER)
    {
        static bool bannerShown = false;
        if (!bannerShown)
        {
            bannerShown = true;
            ServerLog("========================================");
            ServerLog("  Registration token not configured.");
            ServerLog("  Server running offline (not listed).");
            ServerLog("  Contact admin for a valid token,");
            ServerLog("  paste into serverconfig.json.");
            ServerLog("========================================");
        }
        return;
    }

    bool useRoomHeartbeat = !HostRoomId.empty() && !HostToken.empty();

    // If already registered, send heartbeat
    if (!g_ServerId.empty() && !g_ServerToken.empty())
    {
        nlohmann::json payload;
        payload["state"]          = "RUNNING";
        payload["current_players"] = GetCurrentPlayerCount();

        std::string path = "/v1/game-servers/" + g_ServerId + "/heartbeat";
        std::string header = "X-Server-Token: " + g_ServerToken;

        bool ok = SendJsonPost(backend, path, payload, "[ONLINE]", header);
        if (!ok)
        {
            // Try fallback
            std::string fb = (backend == BACKEND_PRIMARY) ? BACKEND_FALLBACK : BACKEND_PRIMARY;
            ServerLog("[ONLINE] Primary failed, trying fallback: " + fb);
            ok = SendJsonPost(fb, path, payload, "[ONLINE-FB]", header);
        }
        return;
    }

    // Not registered yet — register first
    {
        nlohmann::json payload;
        payload["instance_id"] = Config.ServerUniqueId.empty() ? "server-unknown" : Config.ServerUniqueId;
        payload["region"]      = Config.ServerRegion;
        payload["version"]     = "0.7.0";
        payload["endpoint"]    = { {"host", "0.0.0.0"}, {"port", Config.ExternalPort} };
        payload["capacity"]    = { {"max_players", 10} };
        payload["metadata"]    = { {"mode", std::string(Config.FullModePath.begin(), Config.FullModePath.end())},
                                   {"map",  std::string(Config.MapName.begin(), Config.MapName.end())} };

        std::string header = "Authorization: Bearer " + RegistrationToken;

        bool ok = SendJsonPost(backend, "/v1/game-servers", payload, "[REGISTER]", header);
        // Simple fallback is handled inside SendJsonPost now — we can't parse response there.
        // For MVP: just try primary, then fallback.
        if (!ok)
        {
            std::string fb = (backend == BACKEND_PRIMARY) ? BACKEND_FALLBACK : BACKEND_PRIMARY;
            ok = SendJsonPost(fb, "/v1/game-servers", payload, "[REGISTER-FB]", header);
        }
        // Note: full JSON response parsing for server_id / server_token
        // will be added once the endpoint is confirmed working.
    }
}

bool SendRoomLifecycleEvent(const std::string &backend, const std::string &lifecycleAction)
{
    if (HostRoomId.empty() || HostToken.empty())
        return false;

    nlohmann::json payload = {
        {"hostToken", HostToken}};

    std::string path = "/v1/rooms/" + HostRoomId + "/" + lifecycleAction;
    return SendJsonPost(backend, path, payload, "[LIFECYCLE]");
}

// 心跳线程（原本在 MainThread 中启动）
void StartHeartbeatThread()
{
    std::thread([]()
                {
        // Wait until Gamestate is Valid
        while (!IsServerShutdownRequested() &&
            (!UWorld::GetWorld() ||
            !UWorld::GetWorld()->AuthorityGameMode ||
            !UWorld::GetWorld()->AuthorityGameMode->GameState))
        {
            Sleep(100);
        }
        while (!IsServerShutdownRequested())
        {
            if (IsServerHeartbeatHealthy())
            {
                int pc = GetCurrentPlayerCount();
                std::cout << "[HEARTBEAT] PlayerCount = " << pc << std::endl;
                // HTTP heartbeat is now handled by ToolBox registration worker.
                // DLL only reports state via the named pipe (server_status callback).
            }
            else
            {
                std::cout << "[HEALTH] Heartbeat suppressed: game tick is stale or shutdown is pending." << std::endl;
            }

            Sleep(5000);

        }

        std::cout << "[HEALTH] Heartbeat thread stopped." << std::endl; })
        .detach();
}
