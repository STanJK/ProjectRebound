// ServerEventHooks.cpp
// Event-driven server hooks: ProcessEventHook dispatch, Notify hooks, OnFireWeapon.

#include "ServerEventHooks.h"
#include "HookCore.h"
#include "../Core/GameOffsets.h"
#include "../Logging/LogManager.h"
#include "../Server/NetDriverAccess.h"
#include "../Server/Replication.h"
#include "../Server/LateJoin.h"
#include "../Server/RoundManager.h"
#include "../Server/SideMountFixServer.h"
#include "../Server/B5WeaponConfig.h"
#include "../SDK/Engine_parameters.hpp"

extern uintptr_t BaseAddress;
extern LibReplicate* libReplicate;

using namespace SDK;

// ======================================================
//  ProcessEventHook — server-side dispatch
// ======================================================

extern thread_local bool g_InNativeCall;

void ProcessEventHook(UObject *Object, UFunction *Function, void *Parms)
{
    // B.5: skip hook during native calls (FName construction via Conv_StringToName,
    // converter calls, etc.) to prevent ProcessEvent reentry
    if (B5WeaponConfig::g_InB5NativeCall || g_InNativeCall)
        return ProcessEvent.call(Object, Function, Parms);

    const FCachedProcessEventInfo& EventInfo = GetProcessEventInfo(Function);

    if (EventInfo.ServerKind == EServerProcessEventKind::MatchHasEnded)
    {
        HandleServerMatchEndSignal("process_event_match_has_ended");
    }
    else if (EventInfo.ServerKind == EServerProcessEventKind::StartMatchEnding)
    {
        HandleServerMatchEndSignal("process_event_start_match_ending");
    }
    else if (EventInfo.ServerKind == EServerProcessEventKind::StartShowingMatchResult)
    {
        HandleServerMatchEndSignal("process_event_start_showing_match_result");
    }

    if (EventInfo.ServerKind == EServerProcessEventKind::QuickRespawn)
    {
        APBPlayerController *PBPlayerController = (APBPlayerController *)Object;

        PlayerRespawnAllowedMap[PBPlayerController] = true;
    }

    if (EventInfo.ServerKind == EServerProcessEventKind::ServerRestartPlayer)
    {
        APBPlayerController *PBPlayerController = (APBPlayerController *)Object;
        auto respawnAllowed = PlayerRespawnAllowedMap.find(PBPlayerController);

        if (respawnAllowed != PlayerRespawnAllowedMap.end() && !respawnAllowed->second)
        {
            ServerDebugLog("Denied restart!");
            return;
        }
    }

    // LateJoin: role-selection interception (CanPlayerSelectRole / CanSelectRole)
    if (gLateJoinManager && IsLateJoinRoleQuery(EventInfo.ServerKind) &&
        gLateJoinManager->OnProcessEvent(Object, EventInfo.FullName, Parms))
    {
        // Already handled by LateJoinManager
        return;
    }

    // LateJoin: ServerConfirmRoleSelection
    // Must call original ProcessEvent first, then advance LateJoin state
    if (EventInfo.ServerKind == EServerProcessEventKind::ServerConfirmRoleSelection)
    {
        APBPlayerController *PBPlayerController = Object && Object->IsA(APBPlayerController::StaticClass())
                                                      ? (APBPlayerController *)Object
                                                      : nullptr;

        if (gLateJoinManager && gLateJoinManager->IsLateJoinPlayer(PBPlayerController))
        {
            // Execute original function first
            ProcessEvent.call(Object, Function, Parms);
            // Advance LateJoin state to RoleConfirmed
            gLateJoinManager->OnRoleConfirmed(PBPlayerController);
            return;
        }

        NumPlayersSelectedRole++;

        if (!canStartMatch && NumPlayersSelectedRole >= NumExpectedPlayers)
        {
            canStartMatch = true;
        }
    }

    if (EventInfo.ServerKind == EServerProcessEventKind::ReadyToMatchIntroWaitingToStart)
    {
        if (!canStartMatch)
        {
            return;
        }
    }

    if (EventInfo.ServerKind == EServerProcessEventKind::ClientBeKilled)
    {
        ServerDebugLog("Intercepted Player Kill!");

        APBPlayerController *PBPlayerController = (APBPlayerController *)Object;

        PlayerRespawnAllowedMap[PBPlayerController] = false;
    }

    if (EventInfo.ServerKind == EServerProcessEventKind::PlayerCanRestart)
    {
        ((Params::GameModeBase_PlayerCanRestart *)Parms)->ReturnValue =
            ((AGameModeBase *)Object)->HasMatchStarted();
        return;
    }

    // --- Launcher event handling (delegated to SideMountFixServer) ---
    if (Object && Object->IsA(APBLauncher::StaticClass()))
    {
        if (HandleLauncherServerEvent(Object, Function, Parms, EventInfo.FullName))
            return;
    }

    return ProcessEvent.call(Object, Function, Parms);
}

// ======================================================
//  NotifyActorDestroyed
// ======================================================

bool NotifyActorDestroyedHook(UWorld *World, AActor *Actor, bool SomeShit, bool SomeShit2)
{
    bool ret = NotifyActorDestroyed.call<bool>(World, Actor, SomeShit, SomeShit2);

    if (listening)
    {
        const bool isNetTemporary = Actor->bNetTemporary != 0;
        LibReplicate::FActorInfo ActorInfo = LibReplicate::FActorInfo((void *)Actor, isNetTemporary);

        libReplicate->CallWhenActorDestroyed(ActorInfo);
    }

    return ret;
}

// ======================================================
//  NotifyAcceptingConnection — always accept
// ======================================================

__int64 NotifyAcceptingConnectionHook(UObject *obj)
{
    return 1;
}

// ======================================================
//  NotifyControlMessage — observe NetDriver
// ======================================================

char NotifyControlMessageHook(unsigned __int64 ScuffedShit, __int64 a2, uint8_t a3, __int64 a4)
{
    if (UWorld *World = UWorld::GetWorld())
    {
        if (UNetDriver *ActiveNetDriver = NetDriverAccess::Resolve())
        {
            NetDriverAccess::Observe(ActiveNetDriver, World, NetDriverAccess::Source::Cached);
        }
    }

    return NotifyControlMessage.call<char>(ScuffedShit, a2, a3, a4);
}

// ======================================================
//  OnFireWeapon — return-address guard
// ======================================================

void *OnFireWeapon(APBWeapon *Weapon)
{
    if ((uintptr_t)_ReturnAddress() - BaseAddress != GameOffsets::ReturnAddress::OnFireWeaponAllowedCaller)
    {
        return nullptr;
    }
    else
    {
        return OnFireWeaponHook.call<void *>(Weapon);
    }
}
