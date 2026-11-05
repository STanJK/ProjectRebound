// ServerLogicHooks.cpp
// Tick-driven server hooks: TickFlushHook, PostLogin, replication batching,
// round state machine, role-selection timer, late-join driver.

#include "ServerLogicHooks.h"
#include "HookCore.h"
#include "../Core/GameOffsets.h"
#include "../Logging/LogManager.h"
#include "../Config/Config.h"
#include "../Server/Replication.h"
#include "../Server/NetDriverAccess.h"
#include "../Server/LateJoin.h"
#include "../Server/RoundManager.h"
#include "../Server/PlayerNaming.h"
#include "../Server/PvECamera.h"
#include "../Server/LoadoutSync.h"
#include "../Server/B5WeaponConfig.h"

#include <Windows.h>

extern uintptr_t BaseAddress;
extern LibReplicate* libReplicate;

using namespace SDK;

// ======================================================
//  Shared helpers
// ======================================================

FName* GetActorChannelName()
{
    static FName ActorName = UKismetStringLibrary::Conv_StringToName(L"Actor");
    return &ActorName;
}

// ======================================================
//  FTickReplicationBatch helpers
// ======================================================

void CollectTickReplicationBatch(UNetDriver* NetDriver, UWorld* World, FTickReplicationBatch& Batch)
{
    const int connectionCount = NetDriver ? NetDriver->ClientConnections.Num() : 0;
    Batch.Reset(connectionCount);

    if (!NetDriver || !World)
        return;

    for (UNetConnection* Connection : NetDriver->ClientConnections)
    {
        if (!Connection || !Connection->OwningActor)
            continue;

        Connection->ViewTarget = Connection->PlayerController
            ? Connection->PlayerController->GetViewTarget()
            : Connection->OwningActor;

        Batch.Connections.push_back(Connection);

        if (Connection->PlayerController)
        {
            // Preserve the original first-connection match while avoiding a nested scan later.
            Batch.ConnectionByPlayerController.emplace(Connection->PlayerController, Connection);
        }
    }

    for (int i = 0; i < World->Levels.Num(); ++i)
    {
        ULevel* Level = World->Levels[i];
        if (!Level)
            continue;

        for (int j = 0; j < Level->Actors.Num(); ++j)
        {
            AActor* actor = Level->Actors[j];
            if (!actor)
                continue;
            if (actor->RemoteRole == ENetRole::ROLE_None)
                continue;
            if (!actor->bReplicates)
                continue;
            if (actor->bActorIsBeingDestroyed)
                continue;

            if (actor->Class == APlayerController_BP_C::StaticClass())
            {
                auto* PlayerController = static_cast<APlayerController*>(actor);
                auto connectionIt = Batch.ConnectionByPlayerController.find(PlayerController);
                if (connectionIt != Batch.ConnectionByPlayerController.end())
                {
                    Batch.PlayerControllerInfos.emplace_back(connectionIt->second, PlayerController);
                }

                if (PlayerController->Character)
                {
                    auto* Movement = static_cast<UCharacterMovementComponent*>(
                        PlayerController->Character->GetComponentByClass(UCharacterMovementComponent::StaticClass()));
                    if (Movement)
                    {
                        Movement->bIgnoreClientMovementErrorChecksAndCorrection = true;
                        Movement->bServerAcceptClientAuthoritativePosition = true;
                    }
                }

                continue;
            }

            const bool isNetTemporary = actor->bNetTemporary != 0;
            Batch.ActorInfos.emplace_back(actor, isNetTemporary);
        }
    }
}

void SelectRoleForQueuedPlayers()
{
    if (!SDK::UObject::GObjects)
        return;

    for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; --i)
    {
        SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

        if (!Obj || Obj->IsDefaultObject())
            continue;

        if (Obj->IsA(APBPlayerController::StaticClass()))
        {
            auto* PlayerController = static_cast<APBPlayerController*>(Obj);
            if (PlayerController->CanSelectRole())
            {
                ServerDebugLog("Selecting role...");
                PlayerController->ClientSelectRole();
            }
            else
            {
                ServerDebugLog("CANT SELECT ROLE WEE WOO WEE WOO");
            }
        }
    }
}

void ForceServerSuicideForAllPlayers()
{
    if (!SDK::UObject::GObjects)
        return;

    for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; --i)
    {
        SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

        if (!Obj || Obj->IsDefaultObject())
            continue;

        if (Obj->IsA(APBPlayerController::StaticClass()))
        {
            static_cast<APBPlayerController*>(Obj)->ServerSuicide(0);
        }
    }
}

// ======================================================
//  TickFlushHook
// ======================================================

void TickFlushHook(UNetDriver *NetDriver, float DeltaTime)
{
    NoteServerGameTick();

    UserNameFix_DrainPending();
    PVECamFix_Tick(NetDriver, DeltaTime);
    LoadoutSync::SelfTestStart();
    LoadoutSync::SelfTestDrain();
    LoadoutSync::DrainPending();
    B5WeaponConfig::DeferredReapplyTick();

    if (IsServerShutdownRequested())
    {
        return TickFlush.call(NetDriver, DeltaTime);
    }

    UWorld* World = UWorld::GetWorld();

    if (listening && NetDriver && World)
    {
        NetDriverAccess::Observe(NetDriver, World, NetDriverAccess::Source::HookArgument);

        if (PlayerJoinTimerSelectFuck > 0.0f)
        {
            PlayerJoinTimerSelectFuck -= DeltaTime;

            if (PlayerJoinTimerSelectFuck <= 0.0f)
            {
                SelectRoleForQueuedPlayers();
            }
        }

        thread_local FTickReplicationBatch ReplicationBatch;
        CollectTickReplicationBatch(NetDriver, World, ReplicationBatch);

        if (!ReplicationBatch.ActorInfos.empty() && !ReplicationBatch.Connections.empty() && libReplicate)
        {
            libReplicate->CallFromTickFlushHook(
                ReplicationBatch.ActorInfos,
                ReplicationBatch.PlayerControllerInfos,
                ReplicationBatch.Connections,
                GetActorChannelName(),
                NetDriver);

            int *counter = reinterpret_cast<int *>(reinterpret_cast<char *>(NetDriver) + 0x420);
            *counter = *counter + 1;
        }

        // Drive LateJoin state machine
        if (gLateJoinManager)
            gLateJoinManager->Tick(DeltaTime);
    }

    APBGameState *CurrentGameState = GetPBGameState();
    if (CurrentGameState && !CurrentGameState->IsRoundInProgress())
    {
        const std::string RoundState = CurrentGameState->RoundState.ToString();

        if (DidProcStartMatch && IsTerminalRoundState(RoundState))
        {
            std::string reason = "round_state_" + RoundState;
            HandleServerMatchEndSignal(reason.c_str());
            return TickFlush.call(NetDriver, DeltaTime);
        }

        if (RoundState.contains("InvalidState"))
        {

            if (NumPlayersJoined >= Config.MinPlayersToStart)
            {
                if (!DidProcFlow)
                {
                    if (MatchStartCountdown == -1.0f)
                    {
                        MatchStartCountdown = 30.0f;

                        NumExpectedPlayers = NumPlayersJoined;
                    }
                    else
                    {
                        MatchStartCountdown -= DeltaTime;

                        if (NumExpectedPlayers > NumPlayersJoined)
                        {
                            NumExpectedPlayers = NumPlayersJoined;

                            MatchStartCountdown += 15.0f;
                        }

                        if (MatchStartCountdown <= 0.0f)
                        {
                            DidProcFlow = true;

                            ServerDebugLog("All players connected, beginning role selection flow!");

                            PlayerJoinTimerSelectFuck = 5.0f;

                            NumExpectedPlayers = NumPlayersJoined;
                        }
                    }
                }
            }
        }

        if (RoundState.contains("CountdownToStart") && NetDriver)
        {

            for (UNetConnection *pc : NetDriver->ClientConnections)
            {
                if (pc->PlayerController && pc->PlayerController->Pawn)
                    pc->PlayerController->Possess(pc->PlayerController->Pawn);
            }
        }
    }

    if (canStartMatch && !DidProcStartMatch)
    {
        DidProcStartMatch = true;

        if (UWorld* CurrentWorld = UWorld::GetWorld())
        {
            if (CurrentWorld->AuthorityGameMode)
            {
                ((APBGameMode *)CurrentWorld->AuthorityGameMode)->StartMatch();
                HandleServerMatchStarted();
            }
        }
    }

    if ((GetAsyncKeyState(VK_F8) & 0x8000) && amServer)
    {
        ForceServerSuicideForAllPlayers();

        while (GetAsyncKeyState(VK_F8) & 0x8000)
        {
            Sleep(10);
        }
    }

    return TickFlush.call(NetDriver, DeltaTime);
}

// ======================================================
//  PostLogin
// ======================================================

void *PostLogin(AGameMode *GameMode, APBPlayerController *PC)
{
    void *Ret = PostLoginHook.call<void *>(GameMode, PC);

    NumPlayersJoined++;

    ServerDebugLog("[POST-LOGIN] Player #" + std::to_string(NumPlayersJoined) + " connected");

    UserNameFix_OnPostLogin(GameMode, PC);
    LoadoutSync::OnPlayerJoined(GameMode, PC);

    // LateJoin detection
    if (gLateJoinManager && gLateJoinManager->OnPostLogin(GameMode, PC))
    {
        // Handled as LateJoin player; skip normal first-life flow
        return Ret;
    }

    // Force first-life respawn fix
    if (PC && PC->Pawn)
    {
        PC->ServerSuicide(0); // triggers respawn
    }

    return Ret;
}
