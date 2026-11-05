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

extern uintptr_t BaseAddress;

// ======================================================
//  B.5: skin FName resolution diagnostic
// ======================================================

// Recursively look up a row in a (possibly composite) DataTable, walking parent tables.
static uint8* FindRowInTable(UDataTable* dt, const FName& key)
{
    for (int i = 0; i < dt->RowMap.Num(); ++i) {
        const auto& pair = dt->RowMap[i];
        if (pair.Key() == key) return pair.Value();
    }
    if (dt->IsA(UCompositeDataTable::StaticClass())) {
        auto* comp = static_cast<UCompositeDataTable*>(dt);
        for (int p = 0; p < comp->ParentTables.Num(); ++p) {
            uint8* r = FindRowInTable(comp->ParentTables[p], key);
            if (r) return r;
        }
    }
    return nullptr;
}

// Resolve an injected skin/ornament FName through the skin-definition DataTable
// (including composite parent tables) and log the row + skin asset chain.
static void DumpSkinResolution(unsigned char* wpn)
{
    static int s_Limit = 0;
    if (s_Limit >= 6) return;
    ++s_Limit;

    // Get the DataTable manager (engine subsystem).
    auto* mgr = static_cast<UPBDataTableManager*>(
        USubsystemBlueprintLibrary::GetEngineSubsystem(
            TSubclassOf<UEngineSubsystem>(UPBDataTableManager::StaticClass())));
    if (!mgr) { WriteLog("[SKIN-DT] no DataTableManager"); return; }

    // Probe every skin-related DataTable to find which one holds the injected FName.
    struct TableProbe { const char* name; UDataTable* table; };
    TableProbe tables[] = {
        { "WeaponSuit",        mgr->WeaponSuitDefinitionDataTable.Get() },
        { "WeaponSuitPainting", mgr->WeaponSuitPaintingDefinitionDataTable.Get() },
        { "PartSkinDef",       mgr->WeaponPartSkinDefinitionDataTable.Get() },
        { "SkinPainting",      mgr->SkinPaintingDefinitionDataTable.Get() },
        { "SkinTheme",         mgr->SkinThemeDataTable.Get() },
        { "WeaponOrnament",    mgr->WeaponOrnamentDefinitionDataTable.Get() },
    };

    unsigned char* cfg = wpn + 0x3E8;                          // PartNetworkConfig
    void*  partData = *reinterpret_cast<void**>(cfg + 0x10);   // WeaponPartConfigs data
    int    partNum  = *reinterpret_cast<int*>(cfg + 0x18);

    auto dumpOne = [&](const char* tag, uint64_t raw) {
        if (!raw) return;
        FName skinName;
        *reinterpret_cast<uint64_t*>(&skinName) = raw;
        std::string nameStr = skinName.ToString();

        for (auto& probe : tables) {
            if (!probe.table) continue;
            uint8* row = FindRowInTable(probe.table, skinName);
            if (row) {
                char b[300];
                sprintf_s(b, "[SKIN-DT] %s fname=%s -> in %s (row=%p)",
                    tag, nameStr.c_str(), probe.name, (void*)row);
                WriteLog(b);
                return;
            }
        }
        char b[260];
        sprintf_s(b, "[SKIN-DT] %s fname=%s -> NOT IN ANY SKIN TABLE", tag, nameStr.c_str());
        WriteLog(b);
    };

    // Weapon-level fields (OrnamentID @ +0x20, WeaponID @ +0x28, WeaponClassID @ +0x30).
    dumpOne("ornament", *reinterpret_cast<uint64_t*>(cfg + 0x20));
    dumpOne("weaponid", *reinterpret_cast<uint64_t*>(cfg + 0x28));
    dumpOne("classid",  *reinterpret_cast<uint64_t*>(cfg + 0x30));

    for (int i = 0; i < partNum && i < 6; ++i) {
        unsigned char* p = reinterpret_cast<unsigned char*>(partData) + 32 * i;
        dumpOne("skin", *reinterpret_cast<uint64_t*>(p + 0x08));      // WeaponPartSkinID (mesh, "PartOri")
        dumpOne("paint", *reinterpret_cast<uint64_t*>(p + 0x18));     // WeaponPartSkinPaintingID (color)
    }
}

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

    // --- B.5: OnRep_CharacterSkinConfig — verify character cosmetics arrived ---
    if (EventInfo.FullName.find("OnRep_CharacterSkinConfig") != std::string::npos)
    {
        static int s_OnRepCharCount = 0;
        if (Object && Object->IsA(APBCharacter::StaticClass()) && s_OnRepCharCount < 60)
        {
            s_OnRepCharCount++;
            unsigned char* chr = (unsigned char*)Object;
            // APBCharacter::CharacterSkinConfig (+0x698): SkinClassArray, SkinIDArray, SkinPaintingID
            void*    clsData  = *(void**)(chr + 0x698);
            int      clsNum   = *(int*)(chr + 0x6A0);
            void*    idData   = *(void**)(chr + 0x6A8);
            int      idNum    = *(int*)(chr + 0x6B0);
            uint32_t paintCI  = *(uint32_t*)(chr + 0x6B8);
            char buf[256];
            sprintf_s(buf, "[CLIENT][CHAR-SKIN] chr=%p classes=%d ids=%d paint=0x%04X",
                Object, clsNum, idNum, paintCI);
            WriteLog(buf);
            for (int i = 0; i < clsNum && i < 8; ++i) {
                uint8_t  cls  = clsData ? *(uint8_t*)((unsigned char*)clsData + i) : 0xFF;
                uint32_t idCI = (idData && i < idNum) ? *(uint32_t*)((unsigned char*)idData + 8 * i) : 0;
                sprintf_s(buf, "[CLIENT][CHAR-SKIN]   [%d] class=%d id=0x%04X", i, (int)cls, idCI);
                WriteLog(buf);
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
            // Finishes "simulated parts" setup the native OnRep normally triggers.
            findAndCall("K2_InitSimulatedPartsComplete");
            // Skin-material candidates. K2_RefreshSkin was a wrong earlier guess; try the
            // "refresh weapon data" events next (native OnRep doesn't reach the per-part skin).
            findAndCall("K2_RefreshSkin");
            findAndCall("K2_RefreshWeaponData");
            findAndCall("RefreshWeaponData");

            // B.5: dump how the game resolves the injected skin/ornament FNames.
            DumpSkinResolution(wpn);

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
