// ServerLogic.cpp
#include "ServerLogic.h"
#include "../Config/Config.h"
#include "../Debug/Debug.h"
#include "../Network/NetDriverAccess.h"
#include "../ServerLogic/LateJoinManager.h"
#include "../Replication/libreplicate.h"
#include "../SDK/Engine_parameters.hpp"
#include "../SDK/ProjectBoundary_parameters.hpp"
#include <Windows.h>
#include <iostream>
#include <thread>
#include <chrono>

using namespace SDK;

extern LibReplicate *libReplicate;
extern uintptr_t BaseAddress;

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