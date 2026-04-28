// Hooks.cpp
#include "Hooks.h"
#include <Windows.h>
#include <iostream>
#include <thread>
#include "../SDK.hpp"
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

// ======================================================
//  SECTION 7 — HOOK DETOURS (ENGINE HOOKS)
// ======================================================

static SafetyHookInline TickFlush = {};

void TickFlushHook(UNetDriver *NetDriver, float DeltaTime)
{
    if (listening && NetDriver && UWorld::GetWorld())
    {
        NetDriverAccess::Observe(NetDriver, UWorld::GetWorld(), NetDriverAccess::Source::HookArgument);

        if (PlayerJoinTimerSelectFuck > 0.0f)
        {
            PlayerJoinTimerSelectFuck -= DeltaTime;

            if (PlayerJoinTimerSelectFuck <= 0.0f)
            {

                for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; i--)
                {
                    SDK::UObject *Obj = SDK::UObject::GObjects->GetByIndex(i);

                    if (!Obj)
                        continue;

                    if (Obj->IsDefaultObject())
                        continue;

                    if (Obj->IsA(APBPlayerController::StaticClass()))
                    {
                        if (((APBPlayerController *)Obj)->CanSelectRole())
                        {
                            std::cout << "Selecting role..." << std::endl;
                            ((APBPlayerController *)Obj)->ClientSelectRole();
                        }
                        else
                        {
                            std::cout << "CANT SELECT ROLE WEE WOO WEE WOO" << std::endl;
                        }
                    }
                }
            }
        }

        std::vector<LibReplicate::FActorInfo> ActorInfos = std::vector<LibReplicate::FActorInfo>();
        std::vector<UNetConnection *> Connections = std::vector<UNetConnection *>();
        std::vector<void *> PlayerControllers = std::vector<void *>();

        for (UNetConnection *Connection : NetDriver->ClientConnections)
        {
            if (Connection->OwningActor)
            {
                Connection->ViewTarget = Connection->PlayerController ? Connection->PlayerController->GetViewTarget() : Connection->OwningActor;
                Connections.push_back(Connection);
            }
        }

        for (int i = 0; i < UWorld::GetWorld()->Levels.Num(); i++)
        {
            ULevel *Level = UWorld::GetWorld()->Levels[i];

            if (Level)
            {
                for (int j = 0; j < Level->Actors.Num(); j++)
                {
                    AActor *actor = Level->Actors[j];

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
                        PlayerControllers.push_back((void *)actor);
                        if (((APlayerController *)actor)->Character && ((APlayerController *)actor)->Character->GetComponentByClass(UCharacterMovementComponent::StaticClass()))
                        {
                            ((UCharacterMovementComponent *)(((APlayerController *)actor)->Character->GetComponentByClass(UCharacterMovementComponent::StaticClass())))->bIgnoreClientMovementErrorChecksAndCorrection = true;
                            ((UCharacterMovementComponent *)(((APlayerController *)actor)->Character->GetComponentByClass(UCharacterMovementComponent::StaticClass())))->bServerAcceptClientAuthoritativePosition = true;
                        }
                        continue;
                    }

                    ActorInfos.push_back(LibReplicate::FActorInfo(actor, actor->bNetTemporary));
                }
            }
        }

        std::vector<LibReplicate::FPlayerControllerInfo> PlayerControllerInfos = std::vector<LibReplicate::FPlayerControllerInfo>();

        for (void *PlayerController : PlayerControllers)
        {
            for (UNetConnection *Connection : Connections)
            {
                if (Connection->PlayerController == PlayerController)
                {
                    PlayerControllerInfos.push_back(LibReplicate::FPlayerControllerInfo(Connection, PlayerController));
                    break;
                }
            }
        }

        std::vector<void *> CastConnections = std::vector<void *>();

        for (UNetConnection *Connection : Connections)
        {
            CastConnections.push_back((void *)Connection);
        }

        static FName *ActorName = nullptr;

        if (!ActorName)
        {
            ActorName = new FName();
            ActorName->ComparisonIndex = UKismetStringLibrary::Conv_StringToName(L"Actor").ComparisonIndex;
            ActorName->Number = UKismetStringLibrary::Conv_StringToName(L"Actor").Number;
        }

        if (ActorInfos.size() > 0 && CastConnections.size() > 0)
        {
            if (NetDriver)
            {
                libReplicate->CallFromTickFlushHook(ActorInfos, PlayerControllerInfos, CastConnections, ActorName, NetDriver);

                int *counter = reinterpret_cast<int *>(reinterpret_cast<char *>(NetDriver) + 0x420);
                *counter = *counter + 1;
            }
        }

        // Drive LateJoin state machine
        if (gLateJoinManager)
            gLateJoinManager->Tick(DeltaTime);
    }

    APBGameState *CurrentGameState = GetPBGameState();
    if (CurrentGameState && !CurrentGameState->IsRoundInProgress())
    {
        if (CurrentGameState->RoundState.ToString().contains("InvalidState"))
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
                        }
                    }
                }
            }
        }

        if (CurrentGameState->RoundState.ToString().contains("CountdownToStart"))
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

        ((APBGameMode *)UWorld::GetWorld()->AuthorityGameMode)->StartMatch();
    }

    if (GetAsyncKeyState(VK_F8) && amServer)
    {
        for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; i--)
        {
            SDK::UObject *Obj = SDK::UObject::GObjects->GetByIndex(i);

            if (!Obj)
                continue;

            if (Obj->IsDefaultObject())
                continue;

            if (Obj->IsA(APBPlayerController::StaticClass()))
            {
                ((APBPlayerController *)Obj)->ServerSuicide(0);
            }
        }

        while (GetAsyncKeyState(VK_F8))
        {
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
    const std::string functionName = Function ? std::string(Function->GetFullName()) : "";

    if (functionName.contains("QuickRespawn"))
    {
        APBPlayerController *PBPlayerController = (APBPlayerController *)Object;

        PlayerRespawnAllowedMap[PBPlayerController] = true;
    }

    if (functionName.contains("ServerRestartPlayer"))
    {
        APBPlayerController *PBPlayerController = (APBPlayerController *)Object;

        if (PlayerRespawnAllowedMap.contains(PBPlayerController) && PlayerRespawnAllowedMap[PBPlayerController] == false)
        {
            std::cout << "Denied restart!" << std::endl;
            return;
        }
    }

    // LateJoin: role-selection interception (CanPlayerSelectRole / CanSelectRole)
    if (gLateJoinManager && gLateJoinManager->OnProcessEvent(Object, functionName, Parms))
    {
        // Already handled by LateJoinManager
        return;
    }

    // LateJoin: ServerConfirmRoleSelection
    // Must call original ProcessEvent first, then advance LateJoin state
    if (functionName.contains("ServerConfirmRoleSelection"))
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

    if (functionName.contains("ReadyToMatchIntro_WaitingToStart"))
    {
        if (!canStartMatch)
        {
            return;
        }
    }

    if (functionName.contains("ClientBeKilled"))
    {
        std::cout << "Intercepted Player Kill!" << std::endl;

        APBPlayerController *PBPlayerController = (APBPlayerController *)Object;

        PlayerRespawnAllowedMap[PBPlayerController] = false;
    }

    if (functionName.contains("PlayerCanRestart"))
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

    // Force first-life respawn fix
    if (PC && PC->Pawn)
    {
        PC->ServerSuicide(0); // triggers respawn
    }

    return Ret;
}

static SafetyHookInline OnFireWeaponHook;

void *OnFireWeapon(APBWeapon *Weapon)
{
    if ((uintptr_t)_ReturnAddress() - BaseAddress != 0x1608B31)
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
    // Froce space to login
    if (Function->GetFullName().contains("UMG_EnterGame_C.Construct"))
    {
        ClientLog("[LOGIN] EnterGame Construct forcing SPACE");

        std::thread([]()
                    {
                Sleep(1000); // small delay so widget is fully active
                PressSpace(); })
            .detach();
    }
    if (Function->GetFullName().contains("UMG_EnterGame_C.BP_OnActivated"))
    {
        ClientLog("[LOGIN] EnterGame Activated forcing SPACE");

        std::thread([]()
                    {
                Sleep(1000);
                PressSpace(); })
            .detach();
    }
    // Detect login complete via MainMenuBase Construct
    if (Function->GetFullName().contains("UMG_MainMenuBase_C.Construct"))
    {
        LoginCompleted = true;
    }
    if (Function->GetFullName().contains("OnConnectMatchServerTimeOut"))
    {
        ClientLog("[PE] " + std::string(Object->GetFullName()) + " - " + std::string(Function->GetFullName()));

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
    NotifyActorDestroyed = safetyhook::create_inline((void *)(BaseAddress + 0x33403E0), NotifyActorDestroyedHook);
    NotifyAcceptingConnection = safetyhook::create_inline((void *)(BaseAddress + 0x36CDC90), NotifyAcceptingConnectionHook);
    NotifyControlMessage = safetyhook::create_inline((void *)(BaseAddress + 0x36CDCE0), NotifyControlMessageHook);
    TickFlush = safetyhook::create_inline((void *)(BaseAddress + 0x33E05F0), TickFlushHook);
    ProcessEvent = safetyhook::create_inline((void *)(BaseAddress + 0x1BCBE40), ProcessEventHook);
    ObjectNeedsLoad = safetyhook::create_inline((void *)(BaseAddress + 0x1B7B710), ObjectNeedsLoadHook);
    ActorNeedsLoad = safetyhook::create_inline((void *)(BaseAddress + 0x3124E70), ActorNeedsLoadHook);
    OnFireWeaponHook = safetyhook::create_inline((void *)(BaseAddress + 0x1610500), OnFireWeapon);
    PostLoginHook = safetyhook::create_inline((void *)(BaseAddress + 0x32903B0), PostLogin);
    IsDedicatedServerHook = safetyhook::create_inline((void *)(BaseAddress + 0x33266F0), IsDedicatedServer);
    IsServerHook = safetyhook::create_inline((void *)(BaseAddress + 0x3326C60), IsServer);
    IsStandaloneHook = safetyhook::create_inline((void *)(BaseAddress + 0x3326CE0), IsStandalone);
}

void InitClientHook()
{
    ProcessEventClient = safetyhook::create_inline((void *)(BaseAddress + 0x1BCBE40), ProcessEventHookClient);
    ClientDeathCrash = safetyhook::create_inline((void *)(BaseAddress + 0x16abe10), ClientDeathCrashHook);
}