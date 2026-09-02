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
#include <unordered_set>

using namespace SDK;

// ======================================================
//  ProcessEventHookClient — client-side dispatch
// ======================================================

void ProcessEventHookClient(UObject *Object, UFunction *Function, void *Parms)
{
    const FCachedProcessEventInfo& EventInfo = GetProcessEventInfo(Function);

    // B.5: trace what OnRep_PartNetworkConfig calls internally (to find the
    // skin-application function). When OnRep fires, log the nested ProcessEvents.
    static thread_local int s_PEDepth = 0;
    static thread_local bool s_TraceOnRep = false;
    if (EventInfo.FullName.find("OnRep_PartNetworkConfig") != std::string::npos)
        s_TraceOnRep = true;

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

    // --- B.5: OnRep_PartNetworkConfig — verify + force visual refresh ---
    APBWeapon* weaponToRefresh = nullptr;
    {
        static int s_OnRepPartCount = 0;
        if (EventInfo.FullName.find("OnRep_PartNetworkConfig") != std::string::npos)
        {
            // Remember the weapon so we can force its visual rebuild after the
            // native OnRep runs. The native handler skips the refresh for the
            // locally-controlled player's own weapon (IsLocallyControlled guard),
            // which is why the player never sees the custom config the server sent.
            if (Object && Object->IsA(APBWeapon::StaticClass()))
                weaponToRefresh = static_cast<APBWeapon*>(Object);

            if (s_OnRepPartCount < 200)
            {
                s_OnRepPartCount++;
                unsigned char* wpn = (unsigned char*)Object;
                void* slotData = *(void**)(wpn + 0x3E8);
                int   slotNum  = *(int*)(wpn + 0x3F0);
                void* partData = *(void**)(wpn + 0x3F8);
                int   partNum  = *(int*)(wpn + 0x400);
                uint32_t ornCI = *(uint32_t*)(wpn + 0x408);
                uint32_t widCI = *(uint32_t*)(wpn + 0x410);
                char buf[256];
                sprintf_s(buf, "[CLIENT][ONREP] wpn=%p slots=%d parts=%d orn=0x%04X wep=0x%04X",
                    Object, slotNum, partNum, ornCI, widCI);
                WriteLog(buf);
                for (int s = 0; s < partNum && s < 5; ++s) {
                    unsigned char* p = (unsigned char*)partData + 32 * s;
                    uint32_t pid  = *(uint32_t*)(p + 0);
                    uint32_t skid = *(uint32_t*)(p + 8);
                    uint32_t spid = *(uint32_t*)(p + 16);
                    uint32_t ptid = *(uint32_t*)(p + 24);
                    int sl = slotNum > s ? *(uint8_t*)((unsigned char*)slotData + s) : -1;
                    sprintf_s(buf, "[CLIENT][ONREP]   Part[%d] slot=%d part=0x%04X skin=0x%04X spec=0x%04X paint=0x%04X",
                        s, sl, pid, skid, spid, ptid);
                    WriteLog(buf);
                }
            }
        }
    }

    // B.5: log the nested ProcessEvents that OnRep_PartNetworkConfig triggers.
    if (s_TraceOnRep && s_PEDepth > 0)
    {
        static int s_OnRepCallLog = 0;
        if (s_OnRepCallLog < 30)
        {
            s_OnRepCallLog++;
            char dbg[256];
            sprintf_s(dbg, "[ONREP-CALL] depth=%d fn=%s", s_PEDepth, EventInfo.FullName.c_str());
            WriteLog(dbg);
        }
    }

    s_PEDepth++;
    ProcessEventClient.call(Object, Function, Parms);
    s_PEDepth--;

    if (s_PEDepth == 0) s_TraceOnRep = false;

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
            // Call the BP override of NotifyRecalculateSpecialPartOffset on the
            // weapon's ACTUAL class, which re-derives AmmoPerClip from the custom
            // attachments (the PBWeapon base impl does NOT).
            {
                // Walk the class hierarchy (CSTM_Base_C parent) to find the override.
                UFunction* recalc = nullptr;
                for (UStruct* c = weaponToRefresh->Class; c && !recalc; c = c->Super)
                {
                    for (UField* f = c->Children; f; f = f->Next)
                    {
                        if (f->HasTypeFlag(EClassCastFlags::Function) && f->GetName() == "NotifyRecalculateSpecialPartOffset")
                        { recalc = static_cast<UFunction*>(f); break; }
                    }
                }
                if (recalc)
                    weaponToRefresh->ProcessEvent(recalc, nullptr);
            }
            // K2_InitSimulatedPartsComplete is what the native OnRep calls to apply
            // the skins (trace shows OnRep → NotifyRecalculateSpecialPartOffset →
            // K2_InitSimulatedPartsComplete, NOT K2_RefreshSkin). Call it every OnRep.
            weaponToRefresh->K2_InitSimulatedPartsComplete();

            *reinterpret_cast<int32*>(wpn + 0x0318) = savedAmmo;
            *reinterpret_cast<int32*>(wpn + 0x031C) = savedClip;
            s_InWeaponRefresh = false;

            static int s_RefreshCount = 0;
            if (s_RefreshCount < 20)
            {
                s_RefreshCount++;
                char dbg[128];
                sprintf_s(dbg, "[CLIENT][B5-REFRESH] InitWeapon(full) capacity-rederive weapon=%p", weaponToRefresh);
                WriteLog(dbg);
            }
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
