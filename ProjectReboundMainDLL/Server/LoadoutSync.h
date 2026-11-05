// LoadoutSync.h
// One-way metaserver-to-DS loadout push: PostLogin queues an HTTP fetch, and
// TickFlush parses it on the game thread and pushes it into the player's
// inventories, weapon caches and character cosmetics.

#ifndef LOADOUTSYNC_H
#define LOADOUTSYNC_H
#pragma once

#include "../SDK.hpp"
#include <mutex>
#include <string>
#include <vector>

// Set before calling native functions that internally trigger ProcessEvent
// (e.g. GetWeaponNetworkConfig) to prevent server-side PE hook reentry.
extern thread_local bool g_InNativeCall;

namespace LoadoutSync {

struct PendingLoadout {
    // Steam ID, not a controller pointer: the fetch runs on a detached thread and
    // may finish after the player has left, so the controller is re-resolved on
    // the game thread at drain time instead of being held across the wait.
    std::string playerId;
    std::string json;
};

extern std::vector<PendingLoadout> g_Pending;
extern std::mutex g_Mutex;

// Player-join path
void OnPlayerJoined(SDK::AGameMode* GameMode, SDK::APBPlayerController* PC);
void DrainPending();

// Slow sweep (called from TickFlush): drops the per-player caches of players
// who are gone, so a long-lived server does not accumulate one entry per player
// that has ever joined. Requires two consecutive misses before dropping.
void PruneStaleCaches();

} // namespace LoadoutSync

#endif // LOADOUTSYNC_H
