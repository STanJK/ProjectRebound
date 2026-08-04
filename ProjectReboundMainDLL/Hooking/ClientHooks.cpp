// ClientHooks.cpp
// Client-side hooks: ProcessEventHookClient, ClientDeathCrashHook.

#include "ClientHooks.h"
#include "HookCore.h"
#include "../Logging/LogManager.h"
#include "../Client/SideMountFixClient.h"
#include "../Client/UIShake.h"
#include "../Client/AutoConnect.h"
#include "../API/APIInternal.h"
#include "../Loadout/LoadoutFix.h"
#include <Windows.h>
#include <thread>

using namespace SDK;

// ======================================================
//  ProcessEventHookClient — client-side dispatch
// ======================================================

void ProcessEventHookClient(UObject *Object, UFunction *Function, void *Parms)
{
    const FCachedProcessEventInfo& EventInfo = GetProcessEventInfo(Function);

    // Froce space to login
    if (EventInfo.ClientKind == EClientProcessEventKind::EnterGameConstruct)
    {
        ClientDebugLog("[LOGIN] EnterGame Construct forcing SPACE");

        std::thread([]()
                    {
                Sleep(1000); // small delay so widget is fully active
                PressSpace(); })
            .detach();
    }

    if (EventInfo.ClientKind == EClientProcessEventKind::EnterGameActivated)
    {
        ClientDebugLog("[LOGIN] EnterGame Activated forcing SPACE");

        std::thread([]()
                    {
                Sleep(1000);
                PressSpace(); })
            .detach();
    }

    // Detect login complete via MainMenuBase Construct
    if (EventInfo.ClientKind == EClientProcessEventKind::MainMenuConstruct)
    {
        if (NotifyClientLoginCompleted())
            LoadoutFix_FetchAndLog();
    }

    if (EventInfo.ClientKind == EClientProcessEventKind::ConnectMatchServerTimeout)
    {
        const std::string objectName = Object ? std::string(Object->GetFullName()) : "NULL";
        ClientDebugLog("[PE] " + objectName + " - " + EventInfo.FullName);

        ConnectToMatch();
    }

    // --- Launcher event handling (delegated to SideMountFixClient) ---
    if (Object && Object->IsA(APBLauncher::StaticClass()))
    {
        if (HandleLauncherClientEvent(Object, Function, Parms, EventInfo.FullName))
            return;
    }

    // --- Projectile event handling (delegated to SideMountFixClient) ---
    if (Object && Object->IsA(APBProjectile::StaticClass()))
    {
        HandleProjectileClientEvent(Object, EventInfo.FullName);
    }

    // --- Sprint shake diagnostic (UIShake) ---
    if (Object && Object->IsA(APBCharacter::StaticClass()))
    {
        HandleUICharacterClientEvent(Object, Function, Parms, EventInfo.FullName);
    }

    // --- Equip error swallow (LoadoutFix) ---
    HandleEquipErrorSwallow(Object, Function, Parms, EventInfo.FullName);

    ProcessEventClient.call(Object, Function, Parms);

    // Execute pipe-originated Unreal work only after the original callback,
    // on the game thread captured by the login-complete event.
    PumpPendingClientCommands();

    // After BP callback ran, flush any pending equip display refresh
    LoadoutFix_FlushRefresh();
}

// ======================================================
//  ClientDeathCrash — prevent crash on client death
// ======================================================

__int64 ClientDeathCrashHook(__int64 a1)
{
    return 0;
}
