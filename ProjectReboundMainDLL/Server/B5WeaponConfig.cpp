// B5WeaponConfig.cpp
// Hook-based weapon part config injection (attachments + cosmetics).
//
// Strategy:
//   - Hook sub_141654C80 (DataTable buffer builder) and patch Container#1 (C1)
//     skin FNames in-place after it builds the 0xB0 buffer. The converter
//     (sub_141546780) reads C1, so skins are custom from the source.
//   - ReapplyToExistingWeapons() re-builds the buffer, converts it, injects
//     attachments + skins into the OUTPUT FPBWeaponNetworkConfig
//     (ApplyOverridesToConfig), deep-copies into APBWeapon::PartNetworkConfig
//     (+0x3E8), and forces re-replication (dormancy wake + ForceNetUpdate +
//     SetReplicates toggle). Attachments (WeaponPartID) are injected at the
//     output level because mutating the C1 key corrupts the lookup.

#include "B5WeaponConfig.h"
#include "../Logging/LogManager.h"
#include "../Logging/FileLog.h"
#include "../Libs/safetyhook.hpp"
#include <Windows.h>
#include <cstdio>
#include <string>
#include <unordered_set>

using namespace SDK;

#define ODS(msg) WriteLog(msg)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static std::uintptr_t            s_GameBase = 0;
static SafetyHookInline          s_HkBuildBuffer;
static FWeaponPartCache          s_PartCache;
static FWeaponOrnamentCache      s_OrnamentCache;
thread_local bool                B5WeaponConfig::g_InB5NativeCall = false;

// ---------------------------------------------------------------------------
// DataTable buffer layout (sub_141654C80 output, 0xB0 bytes)
//
//   +0x00: Container#1 TSparseArray header (0x40 bytes)
//            +0x00: Data* (8)
//            +0x08: Num (4), Max (4)
//            +0x10..+0x2F: AllocationFlags inline (32 bytes)
//            +0x30: ?? (4)
//            +0x34: 128 (inline alloc magic, 4)
//            +0x38: FirstFreeIndex = -1 (4)
//            +0x3C: NumFree = 0 (4)
//   +0x40: Pointer field (8)
//   +0x48: DWORD field (4)
//   +0x50: Container#2 TSparseArray header (0x40 bytes) — PART CONFIGS
//            +0x00: Data* → FPBWeaponPartNetworkConfig[Num] (32B each)
//            +0x08: Num (4)
//            +0x0C: Max (4)
//   +0x90: (end of Container#2 header area)
//   +0xA0: FName (8)
//   +0xA8: FName (8)
//   Total: 0xB0
//
// FPBWeaponPartNetworkConfig element (32 bytes):
//   +0x00: WeaponPartID (FName, 8)
//   +0x08: WeaponPartSkinID (FName, 8)       ← we modify this
//   +0x10: WeaponPartSpecialSkinID (FName, 8) ← we modify this
//   +0x18: WeaponPartSkinPaintingID (FName, 8) ← we modify this
// ---------------------------------------------------------------------------

static void ApplyOverridesToBuffer(void* buffer, std::uint64_t weaponFNameRaw)
{
    std::uint8_t* buf = static_cast<std::uint8_t*>(buffer);

    // The converter reads Container#1 (C1 entries, 48B each), NOT Container#2.
    // C1 entry layout: +0x00 PartID, +0x08 SkinID, +0x10 SpecialSkinID,
    //                  +0x18 (unused), +0x20 PaintingID.
    void*  c1Data = *reinterpret_cast<void**>(buf + 0x00);
    std::int32_t c1Num = *reinterpret_cast<std::int32_t*>(buf + 0x08);

    if (s_PartCache.count(weaponFNameRaw) || s_OrnamentCache.count(weaponFNameRaw))
    {
        char dbg[192];
        sprintf_s(dbg, "[B5-BUF] weapon=0x%04X c1=%d cache=HIT",
            static_cast<std::uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
            c1Num);
        ODS(dbg);
    }

    auto wpnIt = s_PartCache.find(weaponFNameRaw);
    if (wpnIt != s_PartCache.end() && c1Data && c1Num > 0 && c1Num <= 64)
    {
        const auto& overrides = wpnIt->second;
        int modified = 0;

        for (int i = 0; i < c1Num; ++i)
        {
            std::uint8_t* elem = static_cast<std::uint8_t*>(c1Data) + 48 * i;
            std::uint64_t partIdRaw = *reinterpret_cast<std::uint64_t*>(elem + 0x00);

            auto ovIt = overrides.find(partIdRaw);
            if (ovIt == overrides.end())
                continue;

            const FPartOverride& ov = ovIt->second;

            // NOTE: PartID (elem+0x00) is the C1 lookup key — do NOT mutate it here.
            // The custom attachment is injected at the OUTPUT config level
            // (WeaponPartID) in ApplyOverridesToConfig, where it is a plain field.
            if (ov.SkinFName != 0)
                *reinterpret_cast<std::uint64_t*>(elem + 0x08) = ov.SkinFName;
            if (ov.SpecialFName != 0)
                *reinterpret_cast<std::uint64_t*>(elem + 0x10) = ov.SpecialFName;
            if (ov.PaintFName != 0)
                *reinterpret_cast<std::uint64_t*>(elem + 0x20) = ov.PaintFName;

            ++modified;
        }

        if (modified > 0)
        {
            char dbg[128];
            sprintf_s(dbg, "[B5-BUF] weapon=0x%04X c1overrides=%d",
                static_cast<std::uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
                modified);
            ODS(dbg);
        }
    }

    // Weapon-level ornament (OrnamentID) — also patch the ornament part's
    // SpecialSkinID in the C1 entries (the converter reads the ornament from
    // there, not from a fixed buffer offset).
    auto ornIt = s_OrnamentCache.find(weaponFNameRaw);
    if (ornIt != s_OrnamentCache.end() && ornIt->second != 0)
    {
        *reinterpret_cast<std::uint64_t*>(buf + 0xA8) = ornIt->second;
        // Also patch any C1 entry whose SpecialSkinID (f1) looks like the
        // weapon-body ornament (slot 12). We detect it by its key landing in
        // the "ornament" FName range; simplest: patch f1 on every C1 entry is
        // too broad, so just patch the buffer's two FName slots.
        *reinterpret_cast<std::uint64_t*>(buf + 0xA0) = ornIt->second;
    }
}

// ---------------------------------------------------------------------------
// Hook on sub_141654C80(void* output, const void* weaponFName)
// RCX = output buffer, RDX = weaponFName
// ---------------------------------------------------------------------------
static void* HkBuildDataTableBuffer(void* output, const void* weaponFName)
{
    void* ret = s_HkBuildBuffer.call<void*>(output, weaponFName);

    if (output && weaponFName)
    {
        B5WeaponConfig::g_InB5NativeCall = true;
        std::uint64_t wpnRaw = *reinterpret_cast<const std::uint64_t*>(weaponFName);
        ApplyOverridesToBuffer(output, wpnRaw);
        B5WeaponConfig::g_InB5NativeCall = false;
    }

    return ret;
}

// ---------------------------------------------------------------------------
// ApplyOverridesToConfig — inject attachments + skins into the OUTPUT
// FPBWeaponNetworkConfig (produced by the converter). WeaponPartID /
// WeaponPartSkinID / OrnamentID are plain struct fields here (NOT TMap keys),
// so this is safe — unlike mutating the C1 key in the buffer.
// ---------------------------------------------------------------------------

static void ApplyOverridesToConfig(void* config, std::uint64_t weaponFNameRaw)
{
    std::uint8_t* cfg = static_cast<std::uint8_t*>(config);

    // WeaponPartConfigs TArray at +0x10
    void*  partData = *reinterpret_cast<void**>(cfg + 0x10);
    std::int32_t partNum = *reinterpret_cast<std::int32_t*>(cfg + 0x18);

    auto wpnIt = s_PartCache.find(weaponFNameRaw);
    if (wpnIt != s_PartCache.end() && partData && partNum > 0 && partNum <= 32)
    {
        const auto& overrides = wpnIt->second;
        int modified = 0;
        for (int i = 0; i < partNum; ++i)
        {
            std::uint8_t* elem = static_cast<std::uint8_t*>(partData) + 32 * i;
            std::uint64_t partIdRaw = *reinterpret_cast<std::uint64_t*>(elem + 0x00);

            auto ovIt = overrides.find(partIdRaw);
            if (ovIt == overrides.end()) continue;

            const FPartOverride& ov = ovIt->second;
            if (ov.PartIdFName)  *reinterpret_cast<std::uint64_t*>(elem + 0x00) = ov.PartIdFName; // WeaponPartID (attachment)
            if (ov.SkinFName)    *reinterpret_cast<std::uint64_t*>(elem + 0x08) = ov.SkinFName;
            if (ov.SpecialFName) *reinterpret_cast<std::uint64_t*>(elem + 0x10) = ov.SpecialFName;
            if (ov.PaintFName)   *reinterpret_cast<std::uint64_t*>(elem + 0x18) = ov.PaintFName;
            ++modified;
        }

        if (modified > 0)
        {
            char dbg[128];
            sprintf_s(dbg, "[B5-ACTOR] weapon=0x%04X patched %d parts",
                static_cast<std::uint32_t>(weaponFNameRaw & 0xFFFFFFFF), modified);
            ODS(dbg);
        }
    }

    // Weapon-level ornament at +0x20
    auto ornIt = s_OrnamentCache.find(weaponFNameRaw);
    if (ornIt != s_OrnamentCache.end() && ornIt->second != 0)
    {
        *reinterpret_cast<std::uint64_t*>(cfg + 0x20) = ornIt->second;
    }
}

// ---------------------------------------------------------------------------
// Cache population
// ---------------------------------------------------------------------------
void B5WeaponConfig::CachePartOverride(
    std::uint64_t weaponFNameRaw,
    std::uint64_t partIdRaw,
    std::uint64_t skinFNameRaw,
    std::uint64_t paintFNameRaw)
{
    if (!weaponFNameRaw || !partIdRaw)
        return;

    FPartOverride& ov = s_PartCache[weaponFNameRaw][partIdRaw];
    if (skinFNameRaw)  ov.SkinFName = skinFNameRaw;
    if (paintFNameRaw) ov.PaintFName = paintFNameRaw;

    char dbg[160];
    sprintf_s(dbg, "[B5-CACHE] weapon=0x%04X part=0x%04X skin=0x%04X paint=0x%04X total_weapons=%zu",
        static_cast<std::uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(partIdRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(skinFNameRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(paintFNameRaw & 0xFFFFFFFF),
        s_PartCache.size());
    ODS(dbg);
}

void B5WeaponConfig::CachePartAttachment(
    std::uint64_t weaponFNameRaw,
    std::uint64_t partIdRaw,
    std::uint64_t attachmentFNameRaw)
{
    if (!weaponFNameRaw || !partIdRaw || !attachmentFNameRaw)
        return;

    s_PartCache[weaponFNameRaw][partIdRaw].PartIdFName = attachmentFNameRaw;

    char dbg[160];
    sprintf_s(dbg, "[B5-CACHE] weapon=0x%04X part=0x%04X attach=0x%04X",
        static_cast<std::uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(partIdRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(attachmentFNameRaw & 0xFFFFFFFF));
    ODS(dbg);
}

void B5WeaponConfig::CacheWeaponOrnament(
    std::uint64_t weaponFNameRaw,
    std::uint64_t ornamentFNameRaw)
{
    if (!weaponFNameRaw || !ornamentFNameRaw)
        return;

    s_OrnamentCache[weaponFNameRaw] = ornamentFNameRaw;

    char dbg[128];
    sprintf_s(dbg, "[B5-CACHE] weapon ornament: weapon=0x%04X ornament=0x%04X",
        static_cast<std::uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(ornamentFNameRaw & 0xFFFFFFFF));
    ODS(dbg);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool B5WeaponConfig::Init(std::uintptr_t gameBase)
{
    s_GameBase = gameBase;

    // Install hook on sub_141654C80 (RVA 0x1654C80)
    s_HkBuildBuffer = safetyhook::create_inline(
        reinterpret_cast<void*>(gameBase + 0x1654C80),
        reinterpret_cast<void*>(HkBuildDataTableBuffer));

    char buf[128];
    sprintf_s(buf,
        "[B5] sub_141654C80 hooked at %p valid=%d",
        reinterpret_cast<void*>(gameBase + 0x1654C80),
        static_cast<bool>(s_HkBuildBuffer));
    ODS(buf);

    ODS("[B5] Hook-based weapon config injection initialized");
    return true;
}

// ---------------------------------------------------------------------------
// ReapplyToExistingWeapons — post-cache fixup
//
// Called after cache is populated. Iterates all APBWeapon actors in the
// current world, checks if their WeaponID matches a cached weapon, and
// if so: calls sub_141654C80 to build a new buffer (which our hook
// will now modify with cached data), converts it, and deep-copies
// into the actor via sub_14154B3A0.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Shared reapply logic
// ---------------------------------------------------------------------------
static int ReapplyToExistingWeaponsImpl()
{
    if (s_PartCache.empty() && s_OrnamentCache.empty())
        return 0;

    UWorld* world = UWorld::GetWorld();
    if (!world) return 0;

    // Weapons we've already full-initialized once. After the first InitWeapon we
    // only cheap DeepCopy, so the reapply can run every frame without the cost.
    static std::unordered_set<void*> s_InitializedWeapons;

    int reapplied = 0;

    for (int li = 0; li < world->Levels.Num(); ++li)
    {
        ULevel* level = world->Levels[li];
        if (!level) continue;

        for (int ai = 0; ai < level->Actors.Num(); ++ai)
        {
            AActor* actor = level->Actors[ai];
            if (!actor || !actor->IsA(APBWeapon::StaticClass()))
                continue;

            APBWeapon* weapon = static_cast<APBWeapon*>(actor);
            uint8_t* wpn = reinterpret_cast<uint8_t*>(weapon);
            // Get the weapon's real identity via the game's own GetWeaponId() UFunction
            // (BlueprintPure) instead of guessing an offset. FName raw key = ComparisonIndex | (Number<<32).
            FName widName = weapon->GetWeaponId();
            uint64_t weaponFNameRaw = *(reinterpret_cast<uint64_t*>(&widName));
            uint64_t partCfgWid = *reinterpret_cast<uint64_t*>(wpn + 0x03E8 + 0x28); // old +0x410 read, for diag
            std::string widStr = widName.ToString(); // actual weapon name string
            if (!weaponFNameRaw && !partCfgWid) continue;

            bool hasPartOverride = s_PartCache.count(weaponFNameRaw) > 0;
            bool hasOrnOverride  = s_OrnamentCache.count(weaponFNameRaw) > 0;

            // --- FName diagnostic: log each weapon ID ONCE (first time seen),
            // plus a HIT whenever one matches the cache. This catches weapons that
            // spawn later (e.g. after the player deploys), not just at connect.
            {
                static std::unordered_set<uint64_t> s_SeenWeapons;
                static bool s_DumpedCache = false;

                bool isNew = s_SeenWeapons.insert(weaponFNameRaw).second;
                if (isNew || (hasPartOverride || hasOrnOverride))
                {
                    char dbg[256];
                    sprintf_s(dbg, "[B5-FNAME] getid=0x%04X (%s) partcfg=0x%04X actor=%p %s",
                        static_cast<uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
                        widStr.c_str(),
                        static_cast<uint32_t>(partCfgWid & 0xFFFFFFFF),
                        static_cast<void*>(weapon),
                        (hasPartOverride || hasOrnOverride) ? "HIT" : "MISS");
                    ODS(dbg);
                }

                if (isNew && !(hasPartOverride || hasOrnOverride) && !s_DumpedCache)
                {
                    s_DumpedCache = true;
                    ODS("[B5-FNAME] cached weapon FNames:");
                    for (const auto& [key, _] : s_PartCache)
                    {
                        char cb[128];
                        sprintf_s(cb, "[B5-FNAME]   cached=0x%04X",
                            static_cast<uint32_t>(key & 0xFFFFFFFF));
                        ODS(cb);
                    }
                }
            }

            if (!hasPartOverride && !hasOrnOverride) continue;

            char buf[128];
            sprintf_s(buf, "[B5-REAPPLY] Found cached weapon=0x%04X at actor=%p",
                static_cast<uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
                static_cast<void*>(weapon));
            ODS(buf);

            B5WeaponConfig::g_InB5NativeCall = true;

            // Build DataTable buffer
            using FnBuildBuf = void* (__fastcall*)(void* output, const void* weaponFName);
            FnBuildBuf BuildBuf = reinterpret_cast<FnBuildBuf>(s_GameBase + 0x1654C80);
            uint8_t dataTableBuf[0xC0] = {};
            BuildBuf(dataTableBuf, &weaponFNameRaw);

            // Convert
            using FnConvert = void* (__fastcall*)(void* output, const void* weaponFName, const void* buffer);
            FnConvert Convert = reinterpret_cast<FnConvert>(s_GameBase + 0x1546780);
            alignas(FPBWeaponNetworkConfig) uint8_t converted[sizeof(FPBWeaponNetworkConfig)] = {};
            Convert(converted, &weaponFNameRaw, dataTableBuf);

            // Inject attachments + skins + ornament into the OUTPUT config.
            // WeaponPartID / WeaponPartSkinID / OrnamentID are plain struct fields
            // here (NOT TMap keys), so this is safe — unlike mutating the C1 key.
            ApplyOverridesToConfig(converted, weaponFNameRaw);

            // Deep-copy the PartNetworkConfig (attachments + skins) into +0x3E8.
            using FnDeepCopy = void* (__fastcall*)(void* dst, void* src);
            FnDeepCopy DeepCopy = reinterpret_cast<FnDeepCopy>(s_GameBase + 0x154B3A0);
            DeepCopy(wpn + 0x3E8, converted);

            // First time we see this weapon: full re-derive (InitWeapon + ApplyPartModification
            // + NotifyRecalculateSpecialPartOffset) to set the derived stats (capacity,
            // rate-of-fire) and full ammo. After that, the DeepCopy above keeps the config
            // fresh cheaply, so the reapply can run every frame without InitWeapon cost.
            if (s_InitializedWeapons.insert(weapon).second)
            {
                const FPBWeaponNetworkConfig* cfg = reinterpret_cast<const FPBWeaponNetworkConfig*>(converted);
                weapon->InitWeapon(*cfg, /*bIsOnlyModel=*/false);
                weapon->ApplyPartModification();

                // NotifyRecalculateSpecialPartOffset lives on the CSTM_Base_C parent,
                // not the weapon's concrete class (WP_*_C), so walk the hierarchy.
                UFunction* recalc = nullptr;
                for (UStruct* c = weapon->Class; c && !recalc; c = c->Super)
                {
                    for (UField* f = c->Children; f; f = f->Next)
                    {
                        if (f->HasTypeFlag(EClassCastFlags::Function) && f->GetName() == "NotifyRecalculateSpecialPartOffset")
                        { recalc = static_cast<UFunction*>(f); break; }
                    }
                }
                if (recalc)
                    weapon->ProcessEvent(recalc, nullptr);
            }

            B5WeaponConfig::g_InB5NativeCall = false;

            // Wake the weapon — it was DORM_DormantAll, which blocks replication.
            weapon->NetDormancy = ENetDormancy::DORM_Awake;
            weapon->FlushNetDormancy();

            // Mark the replicated property dirty so UE4 re-sends it to the client.
            UNetPushModelHelpers::MarkPropertyDirty(
                weapon, UKismetStringLibrary::Conv_StringToName(L"PartNetworkConfig"));

            weapon->ForceNetUpdate();

            // Force a full re-replication by toggling the replicate flag.
            {
                char dbg[160];
                sprintf_s(dbg, "[B5-REAPPLY] rep=%d role=%d dorm=%d",
                    weapon->bReplicates ? 1 : 0,
                    (int)weapon->RemoteRole,
                    (int)weapon->NetDormancy);
                ODS(dbg);
                weapon->SetReplicates(false);
                weapon->SetReplicates(true);
            }

            // Read back the replicated field (+0x3E8) to confirm the copy.
            uint32_t ornCI = *reinterpret_cast<uint32_t*>(wpn + 0x03E8 + 0x20);
            int partNum = *reinterpret_cast<int*>(wpn + 0x03E8 + 0x18);
            void* partData = *reinterpret_cast<void**>(wpn + 0x03E8 + 0x10);
            uint32_t skin0 = (partNum > 0 && partData) ? *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(partData) + 8) : 0;
            sprintf_s(buf, "[B5-REAPPLY] Wrote: ornament=0x%04X parts=%d skin0=0x%04X",
                ornCI, partNum, skin0);
            ODS(buf);

            ++reapplied;
        }
    }
    return reapplied;
}

namespace B5WeaponConfig {

int ReapplyToExistingWeapons()
{
    int count = ReapplyToExistingWeaponsImpl();
    char buf[64];
    sprintf_s(buf, "[B5-REAPPLY] Done: %d weapons updated", count);
    ODS(buf);
    return count;
}

bool DeferredReapplyTick()
{
    static int s_TickCounter = 0;

    // Keep scanning indefinitely while a cache exists, so we catch weapons that
    // spawn after the player actually deploys (not just the connect-time actors).
    if (s_PartCache.empty() && s_OrnamentCache.empty())
        return false;

    ++s_TickCounter;
    if (s_TickCounter % 2 != 0) // ~33ms at 60fps (once-per-weapon: cheap after first)
        return true;

    int count = ReapplyToExistingWeaponsImpl();
    if (count > 0)
    {
        char buf[64];
        sprintf_s(buf, "[B5-REAPPLY] tick %d: %d weapons updated", s_TickCounter, count);
        ODS(buf);
    }

    return true;
}

} // namespace B5WeaponConfig
