#include "../framework.h"

#include <atomic>
#include <cstring>

#include "NetDriverAccess.h"
#include "../SDK.hpp"

namespace {
std::atomic<SDK::UNetDriver*> g_cachedNetDriver{ nullptr };
std::atomic<SDK::UWorld*> g_cachedWorld{ nullptr };
std::atomic<int> g_lastSource{ static_cast<int>(NetDriverAccess::Source::None) };

SDK::UNetDriver* ScanForNetDriver()
{
    if (!SDK::UObject::GObjects) {
        return nullptr;
    }

    for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; --i) {
        SDK::UObject* object = SDK::UObject::GObjects->GetByIndex(i);
        if (!object || object->IsDefaultObject()) {
            continue;
        }

        if (object->IsA(SDK::UIpNetDriver::StaticClass())) {
            return static_cast<SDK::UNetDriver*>(object);
        }
    }

    return nullptr;
}
}

void NetDriverAccess::Observe(SDK::UNetDriver* netDriver, SDK::UWorld* world, Source source)
{
    if (!netDriver) {
        return;
    }

    if (!world) {
        world = SDK::UWorld::GetWorld();
    }

    if (world) {
        world->NetDriver = netDriver;
        netDriver->World = world;
        g_cachedWorld.store(world, std::memory_order_release);
    }

    g_cachedNetDriver.store(netDriver, std::memory_order_release);
    g_lastSource.store(static_cast<int>(source), std::memory_order_release);
}

SDK::UNetDriver* NetDriverAccess::Resolve(bool allowObjectScan)
{
    SDK::UWorld* world = SDK::UWorld::GetWorld();
    if (world && world->NetDriver) {
        Observe(world->NetDriver, world, Source::World);
        return world->NetDriver;
    }

    SDK::UNetDriver* cachedNetDriver = g_cachedNetDriver.load(std::memory_order_acquire);
    if (cachedNetDriver) {
        Observe(cachedNetDriver, world ? world : g_cachedWorld.load(std::memory_order_acquire), Source::Cached);
        return cachedNetDriver;
    }

    if (!allowObjectScan) {
        return nullptr;
    }

    SDK::UNetDriver* scannedNetDriver = ScanForNetDriver();
    if (scannedNetDriver) {
        Observe(scannedNetDriver, world, Source::ObjectScan);
    }

    return scannedNetDriver;
}

bool NetDriverAccess::TryGetSnapshot(Snapshot& snapshot, bool allowObjectScan)
{
    snapshot = {};

    SDK::UNetDriver* netDriver = Resolve(allowObjectScan);
    if (!netDriver) {
        return false;
    }

    SDK::UWorld* world = SDK::UWorld::GetWorld();
    if (!world) {
        world = netDriver->World;
    }

    snapshot.NetDriver = netDriver;
    snapshot.World = world;
    snapshot.ServerConnection = netDriver->ServerConnection;
    snapshot.ClientConnectionCount = netDriver->ClientConnections.Num();
    snapshot.MaxClientRate = netDriver->MaxClientRate;
    snapshot.MaxInternetClientRate = netDriver->MaxInternetClientRate;
    snapshot.NetServerMaxTickRate = netDriver->NetServerMaxTickRate;
    snapshot.TimeSeconds = netDriver->Time;
    snapshot.WorldMatches = world && world->NetDriver == netDriver;
    snapshot.HasReplicationDriver = netDriver->ReplicationDriver != nullptr;
    snapshot.NetDriverNameComparisonIndex = netDriver->NetDriverName.ComparisonIndex;
    snapshot.NetDriverNameNumber = netDriver->NetDriverName.Number;
    snapshot.LastSource = static_cast<Source>(g_lastSource.load(std::memory_order_acquire));
    return true;
}

const char* NetDriverAccess::ToString(Source source)
{
    switch (source) {
    case Source::HookArgument:
        return "hook";
    case Source::World:
        return "world";
    case Source::ObjectScan:
        return "object-scan";
    case Source::Cached:
        return "cached";
    default:
        return "none";
    }
}

extern "C" __declspec(dllexport) void* PR_GetActiveNetDriver()
{
    return NetDriverAccess::Resolve();
}

extern "C" __declspec(dllexport) void* PR_GetActiveWorld()
{
    SDK::UWorld* world = SDK::UWorld::GetWorld();
    if (!world) {
        world = g_cachedWorld.load(std::memory_order_acquire);
    }

    return world;
}

extern "C" __declspec(dllexport) BOOL PR_GetNetDriverSnapshot(ProjectReboundNetDriverSnapshot* snapshot)
{
    if (!snapshot) {
        return FALSE;
    }

    std::memset(snapshot, 0, sizeof(*snapshot));

    NetDriverAccess::Snapshot internalSnapshot{};
    if (!NetDriverAccess::TryGetSnapshot(internalSnapshot)) {
        return FALSE;
    }

    snapshot->NetDriver = internalSnapshot.NetDriver;
    snapshot->World = internalSnapshot.World;
    snapshot->ServerConnection = internalSnapshot.ServerConnection;
    snapshot->ClientConnectionCount = internalSnapshot.ClientConnectionCount;
    snapshot->MaxClientRate = internalSnapshot.MaxClientRate;
    snapshot->MaxInternetClientRate = internalSnapshot.MaxInternetClientRate;
    snapshot->NetServerMaxTickRate = internalSnapshot.NetServerMaxTickRate;
    snapshot->TimeSeconds = internalSnapshot.TimeSeconds;
    snapshot->NetDriverNameComparisonIndex = internalSnapshot.NetDriverNameComparisonIndex;
    snapshot->NetDriverNameNumber = internalSnapshot.NetDriverNameNumber;
    snapshot->WorldMatches = internalSnapshot.WorldMatches ? TRUE : FALSE;
    snapshot->HasReplicationDriver = internalSnapshot.HasReplicationDriver ? TRUE : FALSE;
    snapshot->Source = static_cast<int32_t>(internalSnapshot.LastSource);
    return TRUE;
}