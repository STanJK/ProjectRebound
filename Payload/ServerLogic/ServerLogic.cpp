// ServerLogic.cpp
#include "ServerLogic.h"
#include "../Config/Config.h"
#include "../Debug/Debug.h"
#include "../Network/Network.h"
#include "../Network/NetDriverAccess.h"
#include "../ServerLogic/LateJoinManager.h"
#include "../Replication/libreplicate.h"
#include "../SDK/Engine_parameters.hpp"
#include "../SDK/ProjectBoundary_parameters.hpp"
#include <Windows.h>
#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>

using namespace SDK;

extern LibReplicate *libReplicate;
extern uintptr_t BaseAddress;

namespace
{
    constexpr DWORD HeartbeatFreshnessMs = 15000;
    constexpr DWORD MatchEndExitDelayMs = 12000;

    std::atomic<bool> gServerShutdownRequested{false};
    std::atomic<bool> gRoomStartReported{false};
    std::atomic<bool> gRoomEndReported{false};
    std::atomic<ULONGLONG> gLastServerGameTickMs{0};

    void DelayedExitAfterMatchEnd(std::string reason)
    {
        Sleep(MatchEndExitDelayMs);

        listening = false;

        if (!gRoomEndReported.exchange(true) &&
            !OnlineBackendAddress.empty() &&
            !HostRoomId.empty() &&
            !HostToken.empty())
        {
            SendRoomLifecycleEvent(OnlineBackendAddress, "end");
        }

        std::cout << "[SERVER_EXIT] reason=" << reason << std::endl;
        Sleep(250);
        ExitProcess(0);
    }
}

// ======================================================
//  SECTION 6 — REPLICATION SYSTEM GLOBALS (moved to ServerLogic)
// ======================================================

std::vector<APlayerController *> playerControllersPossessed = std::vector<APlayerController *>();

int NumPlayersJoined = 0;
float PlayerJoinTimerSelectFuck = -1.0f;
bool DidProcFlow = false;
float StartMatchTimer = -1.0f;
int NumPlayersSelectedRole = 0;
bool DidProcStartMatch = false;
bool canStartMatch = false;
int NumExpectedPlayers = -1;
float MatchStartCountdown = -1.0f;

std::unordered_map<APBPlayerController *, bool> PlayerRespawnAllowedMap{};
std::unordered_set<APBPlayerController *> PlayersConfirmedRole{};
std::unordered_set<APBPlayerController *> PendingNameUpdatePlayers{};
std::unordered_set<APBPlayerController *> AppliedNameUpdatePlayers{};

// LateJoinManager instance (constructed later in MainThread after dependencies are ready)
LateJoinManager *gLateJoinManager = nullptr;

bool listening = false;

// ======================================================
//  Helpers used by TickFlushHook and LateJoinManager
// ======================================================

APBGameState *GetPBGameState()
{
    UWorld *World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode || !World->AuthorityGameMode->GameState)
        return nullptr;

    return (APBGameState *)World->AuthorityGameMode->GameState;
}

APBGameMode *GetPBGameMode()
{
    UWorld *World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode)
        return nullptr;

    return (APBGameMode *)World->AuthorityGameMode;
}

bool IsRoundCurrentlyInProgress()
{
    APBGameState *GameState = GetPBGameState();
    return GameState && GameState->IsRoundInProgress();
}

// Get PlayerCount helper
int GetCurrentPlayerCount()
{
    UWorld *World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode)
        return -1;

    APBGameState *GS = (APBGameState *)World->AuthorityGameMode->GameState;
    if (!GS)
        return -1;

    return GS->PlayerArray.Num();
}

void NoteServerGameTick()
{
    gLastServerGameTickMs.store(GetTickCount64(), std::memory_order_release);
}

bool IsServerHeartbeatHealthy()
{
    if (gServerShutdownRequested.load(std::memory_order_acquire) || !listening)
        return false;

    const ULONGLONG lastTick = gLastServerGameTickMs.load(std::memory_order_acquire);
    return lastTick != 0 && GetTickCount64() <= lastTick + HeartbeatFreshnessMs;
}

bool IsServerShutdownRequested()
{
    return gServerShutdownRequested.load(std::memory_order_acquire);
}

bool IsTerminalRoundState(const std::string &roundState)
{
    return roundState.contains("ShowingMatchResult") ||
           roundState.contains("MatchEnding") ||
           roundState.contains("WaitingToEndGame");
}

void HandleServerMatchStarted()
{
    if (gRoomStartReported.exchange(true))
        return;

    std::cout << "[SERVER_LIFECYCLE] match_started" << std::endl;

    if (!OnlineBackendAddress.empty() && !HostRoomId.empty() && !HostToken.empty())
        SendRoomLifecycleEvent(OnlineBackendAddress, "start");
}

void HandleServerMatchEndSignal(const char *reason)
{
    if (gServerShutdownRequested.exchange(true))
        return;

    listening = false;

    const std::string shutdownReason = reason && reason[0] ? reason : "match_end";
    std::cout << "[SERVER_LIFECYCLE] match_end_detected reason=" << shutdownReason << std::endl;

    // Keep the hook path lightweight: let the engine finish the current event,
    // then end the backend room and terminate from a detached worker.
    std::thread(DelayedExitAfterMatchEnd, shutdownReason).detach();
}

void QueuePendingPlayerNameUpdate(APBPlayerController *PlayerController)
{
    if (!PlayerController)
        return;

    if (AppliedNameUpdatePlayers.find(PlayerController) == AppliedNameUpdatePlayers.end())
    {
        PendingNameUpdatePlayers.insert(PlayerController);
    }
}

void ApplyPendingPlayerNameUpdates(const char *reason)
{
    if (PendingNameUpdatePlayers.empty())
        return;

    UWorld *World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode)
        return;

    AGameMode *GameMode = (AGameMode *)World->AuthorityGameMode;
    if (!GameMode)
        return;

    std::vector<APBPlayerController *> toApply;
    toApply.reserve(PendingNameUpdatePlayers.size());
    for (APBPlayerController *playerController : PendingNameUpdatePlayers)
    {
        if (playerController)
            toApply.push_back(playerController);
    }

    for (APBPlayerController *playerController : toApply)
    {
        if (!playerController || !playerController->PlayerState)
            continue;

        FString playerName = playerController->PlayerState->GetPlayerName();
        std::string nameStr = playerName.ToString();

        if (nameStr.empty() || nameStr == "UserName")
            continue;

        GameMode->ChangeName(playerController, playerName, true);

        if (playerController->PBPlayerState)
        {
            playerController->PBPlayerState->OnCustomPlayerNameChanged();
        }

        PendingNameUpdatePlayers.erase(playerController);
        AppliedNameUpdatePlayers.insert(playerController);

        std::cout << "[NAME] Applied delayed name update(" << reason << "): " << nameStr << std::endl;
    }
}

// ======================================================
//  SECTION 13 — SERVER STARTUP AND COMMAND RELATED LOGIC
// ======================================================

void StartServer()
{
    Log("[SERVER] Starting server...");

    LoadConfig();

    Log("[SERVER] Map loaded: " + std::string(Config.MapName.begin(), Config.MapName.end()));
    Log("[SERVER] Mode: " + std::string(Config.FullModePath.begin(), Config.FullModePath.end()));
    Log("[SERVER] Port: " + std::to_string(Config.Port));

    std::wstring openCmd = L"open " + Config.MapName + L"?game=" + Config.FullModePath;
    Log("[SERVER] Executing open command");

    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), openCmd.c_str(), nullptr);

    Log("[SERVER] Waiting for world to load...");
    Sleep(8000);

    UEngine *Engine = UEngine::GetEngine();
    UWorld *World = UWorld::GetWorld();

    if (!World)
    {
        Log("[ERROR] World is NULL after map load!");
        return;
    }

    Log("[SERVER] Forcing streaming levels to load...");

    for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; i--)
    {
        SDK::UObject *Obj = SDK::UObject::GObjects->GetByIndex(i);

        if (!Obj)
            continue;

        if (Obj->IsDefaultObject())
            continue;

        if (Obj->IsA(ULevelStreaming::StaticClass()))
        {
            ULevelStreaming *LS = (ULevelStreaming *)Obj;

            LS->SetShouldBeLoaded(true);
            LS->SetShouldBeVisible(true);

            Log("[SERVER] Streaming level loaded: " + std::string(Obj->GetFullName()));
        }
    }

    if (!libReplicate)
    {
        Log("[ERROR] libReplicate is null before CreateNetDriver!");
        return;
    }

    Log("[SERVER] Creating NetDriver...");
    FName name = UKismetStringLibrary::Conv_StringToName(L"GameNetDriver");
    libReplicate->CreateNetDriver(Engine, World, &name);

    UIpNetDriver *NetDriver = reinterpret_cast<UIpNetDriver *>(NetDriverAccess::Resolve());

    if (!NetDriver)
    {
        Log("[ERROR] NetDriver not found after CreateNetDriver!");
        return;
    }

    NetDriverAccess::Observe(NetDriver, World, NetDriverAccess::Source::ObjectScan);
    Log("[SERVER] NetDriver created successfully.");

    Log("[SERVER] Calling Listen()...");
    libReplicate->Listen(NetDriver, World, LibReplicate::EJoinMode::Open, Config.Port);
    NetDriverAccess::Observe(NetDriver, World, NetDriverAccess::Source::World);

    NetDriverAccess::Snapshot snapshot{};
    if (NetDriverAccess::TryGetSnapshot(snapshot, false))
    {
        Log("[SERVER] NetDriver exposed via source: " + std::string(NetDriverAccess::ToString(snapshot.LastSource)));
    }

    listening = true;

    Log("[SERVER] Server is now listening.");
}
