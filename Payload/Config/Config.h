// Config.h
#pragma once
#include <string>
#include <mutex>

struct ServerConfig
{
    std::wstring MapName;
    std::wstring FullModePath;
    unsigned int ExternalPort;
    unsigned int Port;
    bool IsPvE;
    int MinPlayersToStart;
    std::string ServerName;
    std::string ServerRegion;
};

// Central server ip
extern std::string OnlineBackendAddress;

// Room heartbeat credentials from the desktop browser/match server
extern std::string HostRoomId;
extern std::string HostToken;

// IP from the server browser
extern std::string MatchIP;
extern std::string MatchPipeName;
extern std::mutex MatchIPMutex;

extern ServerConfig Config;
extern bool amServer;

std::string GetCmdValue(const std::string &key);
void LoadConfig();
void LoadClientConfig();
