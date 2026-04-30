// Hooks.cpp
#include "Hooks.h"
#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include "../SDK.hpp"
#include "../GameOffsets.h"
#include "../Network/NetDriverAccess.h"
#include "../SDK/Engine_parameters.hpp"
#include "../SDK/ProjectBoundary_parameters.hpp"
#include "../safetyhook/safetyhook.hpp"
#include "../Libs/json.hpp"
#include "../Replication/libreplicate.h"
#include "../ServerLogic/LateJoinManager.h"
#include "../Config/Config.h"
#include "../Debug/Debug.h"
#include "../ServerLogic/ServerLogic.h"
#include "../ClientLogic/ClientLogic.h"
#include "../Utility/Utility.h"

extern uintptr_t BaseAddress;
extern LibReplicate* libReplicate;

using namespace SDK;

namespace
{
    enum class EServerProcessEventKind
    {
        None,
        QuickRespawn,
        ServerRestartPlayer,
        CanPlayerSelectRole,
        CanSelectRole,
        ServerConfirmRoleSelection,
        ReadyToMatchIntroWaitingToStart,
        ClientBeKilled,
        PlayerCanRestart,
        MatchHasEnded,
        StartMatchEnding,
        StartShowingMatchResult
    };

    enum class EClientProcessEventKind
    {
        None,
        EnterGameConstruct,
        EnterGameActivated,
        MainMenuConstruct,
        ConnectMatchServerTimeout
    };

    struct FCachedProcessEventInfo
    {
        std::string FullName;
        EServerProcessEventKind ServerKind = EServerProcessEventKind::None;
        EClientProcessEventKind ClientKind = EClientProcessEventKind::None;
    };

    struct FTickReplicationBatch
    {
        std::vector<LibReplicate::FActorInfo> ActorInfos;
        std::vector<LibReplicate::FPlayerControllerInfo> PlayerControllerInfos;
        std::vector<void*> Connections;
        std::unordered_map<void*, void*> ConnectionByPlayerController;

        void Reset(int connectionCount)
        {
            ActorInfos.clear();
            PlayerControllerInfos.clear();
            Connections.clear();
            ConnectionByPlayerController.clear();

            const size_t connectionCapacity = connectionCount > 0 ? static_cast<size_t>(connectionCount) : 0;
            Connections.reserve(connectionCapacity);
            PlayerControllerInfos.reserve(connectionCapacity);
            ConnectionByPlayerController.reserve(connectionCapacity);
        }
    };

    struct FInlineHookSpec
    {
        const char* Name;
        SafetyHookInline* Storage;
        uintptr_t Offset;
        void* Detour;
    };

    EServerProcessEventKind ClassifyServerProcessEvent(const std::string& functionName)
    {
        if (functionName.contains("QuickRespawn"))
            return EServerProcessEventKind::QuickRespawn;
        if (functionName.contains("ServerRestartPlayer"))
            return EServerProcessEventKind::ServerRestartPlayer;
        if (functionName.contains("CanPlayerSelectRole"))
            return EServerProcessEventKind::CanPlayerSelectRole;
        if (functionName.contains("CanSelectRole"))
            return EServerProcessEventKind::CanSelectRole;
        if (functionName.contains("ServerConfirmRoleSelection"))
            return EServerProcessEventKind::ServerConfirmRoleSelection;
        if (functionName.contains("ReadyToMatchIntro_WaitingToStart"))
            return EServerProcessEventKind::ReadyToMatchIntroWaitingToStart;
        if (functionName.contains("ClientBeKilled"))
            return EServerProcessEventKind::ClientBeKilled;
        if (functionName.contains("PlayerCanRestart"))
            return EServerProcessEventKind::PlayerCanRestart;
        if (functionName.contains("K2_MatchHasEnded"))
            return EServerProcessEventKind::MatchHasEnded;
        if (functionName.contains("K2_StartMatchEnding"))
            return EServerProcessEventKind::StartMatchEnding;
        if (functionName.contains("K2_StartShowingMatchResult"))
            return EServerProcessEventKind::StartShowingMatchResult;

        return EServerProcessEventKind::None;
    }

    EClientProcessEventKind ClassifyClientProcessEvent(const std::string& functionName)
    {
        if (functionName.contains("UMG_EnterGame_C.Construct"))
            return EClientProcessEventKind::EnterGameConstruct;
        if (functionName.contains("UMG_EnterGame_C.BP_OnActivated"))
            return EClientProcessEventKind::EnterGameActivated;
        if (functionName.contains("UMG_MainMenuBase_C.Construct"))
            return EClientProcessEventKind::MainMenuConstruct;
        if (functionName.contains("OnConnectMatchServerTimeOut"))
            return EClientProcessEventKind::ConnectMatchServerTimeout;

        return EClientProcessEventKind::None;
    }

    const FCachedProcessEventInfo& GetProcessEventInfo(UFunction* Function)
    {
        // ProcessEvent is broad and hot; cache classification per game thread by UFunction pointer.
        thread_local std::unordered_map<UFunction*, FCachedProcessEventInfo> Cache;

        auto existing = Cache.find(Function);
        if (existing != Cache.end())
        {
            return existing->second;
        }

        FCachedProcessEventInfo info{};
        if (Function)
        {
            info.FullName = Function->GetFullName();
            info.ServerKind = ClassifyServerProcessEvent(info.FullName);
            info.ClientKind = ClassifyClientProcessEvent(info.FullName);
        }

        auto insertResult = Cache.emplace(Function, std::move(info));
        return insertResult.first->second;
    }

    bool IsLateJoinRoleQuery(EServerProcessEventKind kind)
    {
        return kind == EServerProcessEventKind::CanPlayerSelectRole ||
               kind == EServerProcessEventKind::CanSelectRole;
    }

    FName* GetActorChannelName()
    {
        static FName ActorName = UKismetStringLibrary::Conv_StringToName(L"Actor");
        return &ActorName;
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
                    std::cout << "Selecting role..." << std::endl;
                    PlayerController->ClientSelectRole();
                }
                else
                {
                    std::cout << "CANT SELECT ROLE WEE WOO WEE WOO" << std::endl;
                }
            }
        }
    }

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

                Batch.ActorInfos.emplace_back(actor, actor->bNetTemporary);
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

    void InstallInlineHook(const FInlineHookSpec& spec)
    {
        *spec.Storage = safetyhook::create_inline(GameOffsets::Resolve(BaseAddress, spec.Offset), spec.Detour);
        if (!static_cast<bool>(*spec.Storage))
        {
            Log("[HOOK] Failed to install " + std::string(spec.Name));
        }
    }

    template <size_t Count>
    void InstallInlineHooks(const FInlineHookSpec (&specs)[Count])
    {
        for (const FInlineHookSpec& spec : specs)
        {
            InstallInlineHook(spec);
        }
    }
}

// ======================================================
//  SECTION 7 — HOOK DETOURS (ENGINE HOOKS)
// ======================================================

static SafetyHookInline TickFlush = {};

void TickFlushHook(UNetDriver *NetDriver, float DeltaTime)
{
    NoteServerGameTick();

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

                            std::cout << "All players connected, beginning role selection flow!" << std::endl;

                            PlayerJoinTimerSelectFuck = 5.0f;

                            NumExpectedPlayers = NumPlayersJoined;
                            NumPlayersSelectedRole = 0;
                            canStartMatch = false;
                            StartMatchTimer = -1.0f;
                            PlayersConfirmedRole.clear();
                            PendingNameUpdatePlayers.clear();
                            AppliedNameUpdatePlayers.clear();
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
//  SECTION 8 — HOOK DETOURS (GAMEPLAY HOOKS)
// ======================================================

static SafetyHookInline NotifyActorDestroyed = {};

bool NotifyActorDestroyedHook(UWorld *World, AActor *Actor, bool SomeShit, bool SomeShit2)
{
    bool ret = NotifyActorDestroyed.call<bool>(World, Actor, SomeShit, SomeShit2);

    if (listening)
    {
        LibReplicate::FActorInfo ActorInfo = LibReplicate::FActorInfo((void *)Actor, Actor->bNetTemporary);

        libReplicate->CallWhenActorDestroyed(ActorInfo);
    }

    return ret;
}

static SafetyHookInline NotifyAcceptingConnection = {};

__int64 NotifyAcceptingConnectionHook(UObject *obj)
{
    return 1;
}

static SafetyHookInline NotifyControlMessage = {};

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

static SafetyHookInline ProcessEvent;

void ProcessEventHook(UObject *Object, UFunction *Function, void *Parms)
{
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
            std::cout << "Denied restart!" << std::endl;
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

        QueuePendingPlayerNameUpdate(PBPlayerController);

        if (gLateJoinManager && gLateJoinManager->IsLateJoinPlayer(PBPlayerController))
        {
            // Execute original function first
            ProcessEvent.call(Object, Function, Parms);
            // Advance LateJoin state to RoleConfirmed
            gLateJoinManager->OnRoleConfirmed(PBPlayerController);
            return;
        }

        if (gLateJoinManager && gLateJoinManager->IsInitialJoinPlayer(PBPlayerController))
        {
            ProcessEvent.call(Object, Function, Parms);
            gLateJoinManager->OnRoleConfirmed(PBPlayerController);

            if (PBPlayerController)
            {
                const auto [_, inserted] = PlayersConfirmedRole.insert(PBPlayerController);
                if (inserted)
                {
                    NumPlayersSelectedRole = static_cast<int>(PlayersConfirmedRole.size());
                    std::cout << "[MATCH] Role confirmed by (initial-join) "
                        << PBPlayerController->GetFullName()
                        << " (" << NumPlayersSelectedRole << "/" << NumExpectedPlayers << ")" << std::endl;
                }

                if (!canStartMatch && NumExpectedPlayers > 0 && NumPlayersSelectedRole >= NumExpectedPlayers)
                {
                    canStartMatch = true;
                }
            }
            return;
        }

        if (PBPlayerController)
        {
            const auto [_, inserted] = PlayersConfirmedRole.insert(PBPlayerController);
            if (inserted)
            {
                NumPlayersSelectedRole = static_cast<int>(PlayersConfirmedRole.size());
                std::cout << "[MATCH] Role confirmed by "
                    << PBPlayerController->GetFullName()
                    << " (" << NumPlayersSelectedRole << "/" << NumExpectedPlayers << ")" << std::endl;
            }
            else
            {
                std::cout << "[MATCH] Ignoring duplicate role confirmation from "
                    << PBPlayerController->GetFullName() << std::endl;
            }

            if (!canStartMatch && NumExpectedPlayers > 0 && NumPlayersSelectedRole >= NumExpectedPlayers)
            {
                canStartMatch = true;
            }
        }
    }

    if (EventInfo.ServerKind == EServerProcessEventKind::ReadyToMatchIntroWaitingToStart)
    {
        ApplyPendingPlayerNameUpdates("ReadyToMatchIntro_WaitingToStart");
        if (!canStartMatch)
        {
            return;
        }
    }

    if (EventInfo.ServerKind == EServerProcessEventKind::ClientBeKilled)
    {
        std::cout << "Intercepted Player Kill!" << std::endl;

        APBPlayerController *PBPlayerController = (APBPlayerController *)Object;

        PlayerRespawnAllowedMap[PBPlayerController] = false;
    }

    if (EventInfo.ServerKind == EServerProcessEventKind::PlayerCanRestart)
    {
        ((Params::GameModeBase_PlayerCanRestart *)Parms)->ReturnValue =
            ((AGameModeBase *)Object)->HasMatchStarted();
        return;
    }

    return ProcessEvent.call(Object, Function, Parms);
}

static SafetyHookInline PostLoginHook;

void *PostLogin(AGameMode *GameMode, APBPlayerController *PC)
{
    void *Ret = PostLoginHook.call<void *>(GameMode, PC);

    NumPlayersJoined++;

    std::cout << "Player Connected!" << std::endl;

    // LateJoin detection
    if (gLateJoinManager && gLateJoinManager->OnPostLogin(GameMode, PC))
    {
        // Handled as LateJoin player; skip normal first-life flow
        return Ret;
    }

    // Initial join: defer Pawn creation to LateJoinManager's delayed-spawn flow.
    // This ensures weapons are created after role confirmation instead of being
    // locked to the engine's default first-spawn timing.
    if (gLateJoinManager)
    {
        gLateJoinManager->QueueInitialJoinPlayer(GameMode, PC);
        return Ret;
    }

    // Fallback: if LateJoinManager is not available, use the old immediate-respawn path
    if (PC && PC->Pawn)
    {
        PC->ServerSuicide(0); // triggers respawn
    }

    return Ret;
}

static SafetyHookInline OnFireWeaponHook;

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

// ======================================================
//  SECTION 9 — HOOK DETOURS (CLIENT HOOKS)
// ======================================================

static SafetyHookInline ProcessEventClient;

void ProcessEventHookClient(UObject *Object, UFunction *Function, void *Parms)
{
    // TEMP LOGIN DEBUG DUMP (GameInstance only)
    // if (Object && Object->IsA(UPBGameInstance::StaticClass()))
    //{
    //    std::string fn = Function->GetFullName();
    //        std::cout << "[LOGIN-DUMP] GI :: " << fn << std::endl;
    //}
    const FCachedProcessEventInfo& EventInfo = GetProcessEventInfo(Function);

    // Froce space to login
    if (EventInfo.ClientKind == EClientProcessEventKind::EnterGameConstruct)
    {
        ClientLog("[LOGIN] EnterGame Construct forcing SPACE");

        std::thread([]()
                    {
                Sleep(1000); // small delay so widget is fully active
                PressSpace(); })
            .detach();
    }

    if (EventInfo.ClientKind == EClientProcessEventKind::EnterGameActivated)
    {
        ClientLog("[LOGIN] EnterGame Activated forcing SPACE");

        std::thread([]()
                    {
                Sleep(1000);
                PressSpace(); })
            .detach();
    }

    // Detect login complete via MainMenuBase Construct
    if (EventInfo.ClientKind == EClientProcessEventKind::MainMenuConstruct)
    {
        LoginCompleted = true;
    }

    if (EventInfo.ClientKind == EClientProcessEventKind::ConnectMatchServerTimeout)
    {
        const std::string objectName = Object ? std::string(Object->GetFullName()) : "NULL";
        ClientLog("[PE] " + objectName + " - " + EventInfo.FullName);

        ConnectToMatch();
    }

    return ProcessEventClient.call(Object, Function, Parms);
}

static SafetyHookInline ClientDeathCrash;

__int64 ClientDeathCrashHook(__int64 a1)
{
    return 0;
}

// ======================================================
//  SECTION 10 — HOOK DETOURS (MISC HOOKS)
// ======================================================

static SafetyHookInline ObjectNeedsLoad;

char ObjectNeedsLoadHook(UObject *a1)
{
    return 1;
}

static SafetyHookInline ActorNeedsLoad;

char ActorNeedsLoadHook(UObject *a1)
{
    return 1;
}

static SafetyHookInline MessageBoxWHook;

int WINAPI MessageBoxW_Detour(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType)
{
    if (lpText && wcsstr(lpText, L"Roboto"))
    {
        return IDOK;
    }
    return MessageBoxWHook.call<int>(hWnd, lpText, lpCaption, uType);
}

static SafetyHookInline HudFunctionThatCrashesTheGame;

__int64 HudFunctionThatCrashesTheGameHook(__int64 a1, __int64 a2)
{
    return 0;
}

static SafetyHookInline GameEngineTick;

__int64 GameEngineTickHook(APlayerController *a1,
                           float a2,
                           __int64 a3,
                           __int64 a4)
{

    static bool flip = true;

    flip = !flip;

    if (flip)
    {
        std::cout << "NO TICKY" << std::endl;
        return 0;
    }

    return GameEngineTick.call<__int64>(a1, a2, a3, a4);
}

static SafetyHookInline IsDedicatedServerHook;

bool IsDedicatedServer(void *WorldContextOrSomething)
{
    return true;
}

static SafetyHookInline IsServerHook;

bool IsServer(void *WorldContextOrSomething)
{
    return true;
}

static SafetyHookInline IsStandaloneHook;

bool IsStandalone(void *WorldContextOrSomething)
{
    return false;
}

// ======================================================
//  SECTION 11 — HOOK INITIALIZATION
// ======================================================

extern uintptr_t BaseAddress;
extern LibReplicate *libReplicate;

void InitMessageBoxHook()
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32)
        return;

    void *addr = GetProcAddress(user32, "MessageBoxW");
    if (!addr)
        return;

    MessageBoxWHook = safetyhook::create_inline(addr, MessageBoxW_Detour);
}

void InitServerHooks()
{
    const FInlineHookSpec ServerHooks[] = {
        { "NotifyActorDestroyed", &NotifyActorDestroyed, GameOffsets::Hook::NotifyActorDestroyed, reinterpret_cast<void*>(NotifyActorDestroyedHook) },
        { "NotifyAcceptingConnection", &NotifyAcceptingConnection, GameOffsets::Hook::NotifyAcceptingConnection, reinterpret_cast<void*>(NotifyAcceptingConnectionHook) },
        { "NotifyControlMessage", &NotifyControlMessage, GameOffsets::Hook::NotifyControlMessage, reinterpret_cast<void*>(NotifyControlMessageHook) },
        { "TickFlush", &TickFlush, GameOffsets::Hook::TickFlush, reinterpret_cast<void*>(TickFlushHook) },
        { "ProcessEvent", &ProcessEvent, GameOffsets::Hook::ProcessEvent, reinterpret_cast<void*>(ProcessEventHook) },
        { "ObjectNeedsLoad", &ObjectNeedsLoad, GameOffsets::Hook::ObjectNeedsLoad, reinterpret_cast<void*>(ObjectNeedsLoadHook) },
        { "ActorNeedsLoad", &ActorNeedsLoad, GameOffsets::Hook::ActorNeedsLoad, reinterpret_cast<void*>(ActorNeedsLoadHook) },
        { "OnFireWeapon", &OnFireWeaponHook, GameOffsets::Hook::OnFireWeapon, reinterpret_cast<void*>(OnFireWeapon) },
        { "PostLogin", &PostLoginHook, GameOffsets::Hook::PostLogin, reinterpret_cast<void*>(PostLogin) },
        { "IsDedicatedServer", &IsDedicatedServerHook, GameOffsets::Hook::IsDedicatedServer, reinterpret_cast<void*>(IsDedicatedServer) },
        { "IsServer", &IsServerHook, GameOffsets::Hook::IsServer, reinterpret_cast<void*>(IsServer) },
        { "IsStandalone", &IsStandaloneHook, GameOffsets::Hook::IsStandalone, reinterpret_cast<void*>(IsStandalone) },
    };

    InstallInlineHooks(ServerHooks);
}

void InitClientHook()
{
    const FInlineHookSpec ClientHooks[] = {
        { "ProcessEventClient", &ProcessEventClient, GameOffsets::Hook::ProcessEvent, reinterpret_cast<void*>(ProcessEventHookClient) },
        { "ClientDeathCrash", &ClientDeathCrash, GameOffsets::Hook::ClientDeathCrash, reinterpret_cast<void*>(ClientDeathCrashHook) },
    };

    InstallInlineHooks(ClientHooks);
}
