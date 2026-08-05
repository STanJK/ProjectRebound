// HookCore.cpp
// Shared hook infrastructure — SafetyHookInline variables, classification cache,
// inline-hook helpers, and all three init entry points.

#include "HookCore.h"
#include "../Core/GameOffsets.h"
#include "../Logging/LogManager.h"
#include <Windows.h>

extern uintptr_t BaseAddress;

// ======================================================
//  SafetyHookInline variable definitions
// ======================================================

SafetyHookInline TickFlush = {};
SafetyHookInline ProcessEvent;
SafetyHookInline ProcessEventClient;
SafetyHookInline PostLoginHook;
SafetyHookInline NotifyActorDestroyed = {};
SafetyHookInline NotifyAcceptingConnection = {};
SafetyHookInline NotifyControlMessage = {};
SafetyHookInline OnFireWeaponHook;
SafetyHookInline ClientDeathCrash;
SafetyHookInline ObjectNeedsLoad;
SafetyHookInline ActorNeedsLoad;
SafetyHookInline IsDedicatedServerHook;
SafetyHookInline IsServerHook;
SafetyHookInline IsStandaloneHook;
SafetyHookInline FixEquipErrorHook;
SafetyHookInline FixSkinErrorHook;
SafetyHookInline FixBadgeOrnamentErrorHook;

// ======================================================
//  Equip error resolver hooks
// ======================================================
//
// Intercept the terminal error resolvers before ProcessEvent.
// Known success codes (0, 200, 9001-9003) pass through;
// unrecognized values (incl. the async task's default 404)
// are forced to 0 (NoError).

void __fastcall FixEquipErrorHookFn(__int64 a1, int a2, __int64 a3, __int64 a4, int a5)
{
    if (a2 != 0 && a2 != 200 && a2 != 9001 && a2 != 9002 && a2 != 9003)
        a2 = 0;
    FixEquipErrorHook.call<void>(a1, a2, a3, a4, a5);
}

void __fastcall FixSkinErrorHookFn(__int64 a1, int a2, __int64 a3, __int64 a4, __int64 a5)
{
    if (a2 != 0 && a2 != 200 && a2 != 9001 && a2 != 9002 && a2 != 9003)
        a2 = 0;
    FixSkinErrorHook.call<void>(a1, a2, a3, a4, a5);
}

void __fastcall FixBadgeOrnamentErrorHookFn(__int64 a1, int a2, __int64 a3, __int64 a4, int a5)
{
    if (a2 != 0 && a2 != 200 && a2 != 9001 && a2 != 9002 && a2 != 9003)
        a2 = 0;
    FixBadgeOrnamentErrorHook.call<void>(a1, a2, a3, a4, a5);
}

// ======================================================
//  Inline-hook helpers
// ======================================================

void InstallInlineHook(const FInlineHookSpec& spec)
{
    *spec.Storage = safetyhook::create_inline(GameOffsets::Resolve(BaseAddress, spec.Offset), spec.Detour);
    if (!static_cast<bool>(*spec.Storage))
    {
        ServerLog("[HOOK] Failed to install " + std::string(spec.Name));
    }
}

// ======================================================
//  ProcessEvent classification
// ======================================================

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

const FCachedProcessEventInfo& GetProcessEventInfo(SDK::UFunction* Function)
{
    // ProcessEvent is broad and hot; cache classification per game thread by UFunction pointer.
    thread_local std::unordered_map<SDK::UFunction*, FCachedProcessEventInfo> Cache;

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

// ======================================================
//  MessageBox hook (Roboto font popup suppressor)
// ======================================================

static SafetyHookInline MessageBoxWHook;

static int WINAPI MessageBoxW_Detour(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType)
{
    if (lpText && wcsstr(lpText, L"Roboto"))
    {
        return IDOK;
    }
    return MessageBoxWHook.call<int>(hWnd, lpText, lpCaption, uType);
}

// ======================================================
//  Hook initialization
// ======================================================

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

// ---- Server hook detour forward declarations (defined in their respective files) ----

void TickFlushHook(SDK::UNetDriver* NetDriver, float DeltaTime);
void ProcessEventHook(SDK::UObject* Object, SDK::UFunction* Function, void* Parms);
void* PostLogin(SDK::AGameMode* GameMode, SDK::APBPlayerController* PC);
bool NotifyActorDestroyedHook(SDK::UWorld* World, SDK::AActor* Actor, bool SomeShit, bool SomeShit2);
__int64 NotifyAcceptingConnectionHook(SDK::UObject* obj);
char NotifyControlMessageHook(unsigned __int64 ScuffedShit, __int64 a2, uint8_t a3, __int64 a4);
void* OnFireWeapon(SDK::APBWeapon* Weapon);
bool IsDedicatedServer(void* WorldContextOrSomething);
bool IsServer(void* WorldContextOrSomething);
bool IsStandalone(void* WorldContextOrSomething);
char ObjectNeedsLoadHook(SDK::UObject* a1);
char ActorNeedsLoadHook(SDK::UObject* a1);

// ---- Client hook detour forward declarations ----

void ProcessEventHookClient(SDK::UObject* Object, SDK::UFunction* Function, void* Parms);
__int64 ClientDeathCrashHook(__int64 a1);

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
        { "FixEquipError",       &FixEquipErrorHook,          GameOffsets::Hook::FixEquipErrorCode,          reinterpret_cast<void*>(FixEquipErrorHookFn) },
        { "FixSkinError",        &FixSkinErrorHook,           GameOffsets::Hook::FixSkinErrorCode,           reinterpret_cast<void*>(FixSkinErrorHookFn) },
        { "FixBadgeOrnament",    &FixBadgeOrnamentErrorHook,  GameOffsets::Hook::FixBadgeOrnamentErrorCode,  reinterpret_cast<void*>(FixBadgeOrnamentErrorHookFn) },
    };

    InstallInlineHooks(ClientHooks);
}
