#pragma once

#include <Windows.h>
#include <cstdint>

namespace SDK {
class UNetDriver;
class UWorld;
}

namespace NetDriverAccess {
enum class Source : int {
    None = 0,
    HookArgument = 1,
    World = 2,
    ObjectScan = 3,
    Cached = 4,
};

struct Snapshot {
    SDK::UNetDriver* NetDriver{};
    SDK::UWorld* World{};
    void* ServerConnection{};
    int ClientConnectionCount{};
    int MaxClientRate{};
    int MaxInternetClientRate{};
    int NetServerMaxTickRate{};
    float TimeSeconds{};
    bool WorldMatches{};
    bool HasReplicationDriver{};
    unsigned int NetDriverNameComparisonIndex{};
    unsigned int NetDriverNameNumber{};
    Source LastSource{ Source::None };
};

void Observe(SDK::UNetDriver* netDriver, SDK::UWorld* world, Source source);
SDK::UNetDriver* Resolve(bool allowObjectScan = true);
bool TryGetSnapshot(Snapshot& snapshot, bool allowObjectScan = true);
const char* ToString(Source source);
}

struct ProjectReboundNetDriverSnapshot {
    void* NetDriver;
    void* World;
    void* ServerConnection;
    int32_t ClientConnectionCount;
    int32_t MaxClientRate;
    int32_t MaxInternetClientRate;
    int32_t NetServerMaxTickRate;
    float TimeSeconds;
    uint32_t NetDriverNameComparisonIndex;
    uint32_t NetDriverNameNumber;
    BOOL WorldMatches;
    BOOL HasReplicationDriver;
    int32_t Source;
};

extern "C" __declspec(dllexport) void* PR_GetActiveNetDriver();
extern "C" __declspec(dllexport) void* PR_GetActiveWorld();
extern "C" __declspec(dllexport) BOOL PR_GetNetDriverSnapshot(ProjectReboundNetDriverSnapshot* snapshot);