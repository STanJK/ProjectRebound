// ClientHooks.cpp
// Client-side hooks: ProcessEventHookClient, ClientDeathCrashHook.

#include "ClientHooks.h"
#include "HookCore.h"
#include "../Logging/LogManager.h"
#include "../Logging/FileLog.h"
#include "../Client/SideMountFixClient.h"
#include "../Client/UIShake.h"
#include "../Client/AutoConnect.h"
#include "../API/APIInternal.h"
#include "../Loadout/LoadoutFix.h"
#include <Windows.h>
#include <cstdio>
#include <thread>

using namespace SDK;

extern uintptr_t BaseAddress;

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

    // --- B.5: OnRep_PartNetworkConfig — force visual refresh ---
    APBWeapon* weaponToRefresh = nullptr;
    if (EventInfo.FullName.find("OnRep_PartNetworkConfig") != std::string::npos)
    {
        // Remember the weapon so we can force its visual rebuild after the
        // native OnRep runs. The native handler skips the refresh for the
        // locally-controlled player's own weapon (IsLocallyControlled guard),
        // which is why the player never sees the custom config the server sent.
        if (Object && Object->IsA(APBWeapon::StaticClass()))
            weaponToRefresh = static_cast<APBWeapon*>(Object);
    }

    ProcessEventClient.call(Object, Function, Parms);

    // Force the visual model + derived stats to rebuild from the freshly-replicated
    // config. The native OnRep skips this for the locally-controlled player's own
    // weapon. InitWeapon(full) re-derives the capacity/rate-of-fire and rebuilds the
    // model, but also resets ammo — so we save/restore the ammo around it, and only
    // apply the part models once (re-applying them every tick would flicker).
    if (weaponToRefresh)
    {
        static thread_local bool s_InWeaponRefresh = false;
        if (!s_InWeaponRefresh)
        {
            s_InWeaponRefresh = true;
            unsigned char* wpn = reinterpret_cast<unsigned char*>(weaponToRefresh);
            const FPBWeaponNetworkConfig* cfg = reinterpret_cast<const FPBWeaponNetworkConfig*>(wpn + 0x3E8);
            int32 savedAmmo = *reinterpret_cast<int32*>(wpn + 0x0318); // CurrentAmmo
            int32 savedClip = *reinterpret_cast<int32*>(wpn + 0x031C); // CurrentAmmoInClip

            weaponToRefresh->InitWeapon(*cfg, /*bIsOnlyModel=*/false);
            weaponToRefresh->ApplyPartModification();
            // The weapon's part/skin BP events are BlueprintImplementableEvents: the SDK
            // wrapper resolves to the PBWeapon BASE declaration (no script → no-op), so we
            // walk the hierarchy to find the CSTM_Base_C / WP_*_C override that actually has
            // the BP script. (The SDK wrapper for K2_InitSimulatedPartsComplete was a silent
            // no-op for exactly this reason.)
            auto findAndCall = [&](const char* name) {
                UFunction* fn = nullptr;
                for (UStruct* c = weaponToRefresh->Class; c && !fn; c = c->Super)
                {
                    for (UField* f = c->Children; f; f = f->Next)
                    {
                        if (f->HasTypeFlag(EClassCastFlags::Function) && f->GetName() == name)
                        { fn = static_cast<UFunction*>(f); break; }
                    }
                }
                if (fn)
                    weaponToRefresh->ProcessEvent(fn, nullptr);
            };

            // Re-derives AmmoPerClip (capacity) from the custom attachments.
            findAndCall("NotifyRecalculateSpecialPartOffset");
            // Finishes the "simulated parts" setup the native OnRep normally triggers.
            findAndCall("K2_InitSimulatedPartsComplete");
            // Skin/display refresh candidates; whichever exists is harmless when its
            // BP script does not handle per-part skins.
            findAndCall("K2_RefreshSkin");
            findAndCall("K2_RefreshWeaponData");
            findAndCall("RefreshWeaponData");

            *reinterpret_cast<int32*>(wpn + 0x0318) = savedAmmo;
            *reinterpret_cast<int32*>(wpn + 0x031C) = savedClip;
            s_InWeaponRefresh = false;
        }
    }

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
