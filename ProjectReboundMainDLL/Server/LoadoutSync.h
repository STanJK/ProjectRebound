// LoadoutSync.h
// One-way metaserver-to-DS loadout push.
// Currently in SELF-TEST MODE: auto-fetches on server start, no client needed.

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
    SDK::APBPlayerController* PC;
    std::string json;
};

extern std::vector<PendingLoadout> g_Pending;
extern std::mutex g_Mutex;

// Player-join path (disabled during self-test)
void OnPlayerJoined(SDK::AGameMode* GameMode, SDK::APBPlayerController* PC);
void DrainPending();

// B.5: set current spawn PlayerState for getter hook
void SetSpawnPlayerState(SDK::APBPlayerState* PS);

// Self-test: called from TickFlushHook
void SelfTestStart();   // spawns HTTP worker thread (once)
void SelfTestDrain();   // processes result on game thread (every frame until done)

} // namespace LoadoutSync

#endif // LOADOUTSYNC_H
