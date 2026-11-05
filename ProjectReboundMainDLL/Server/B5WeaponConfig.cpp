// B5WeaponConfig.cpp
// Weapon part config injection (attachments + cosmetics) via the game's own
// OnRep_PartNetworkConfig apply path.
//
// Strategy (simplified):
//   - ReapplyToExistingWeapons() builds the DataTable buffer, converts it to
//     FPBWeaponNetworkConfig, injects attachments + skins + ornament at the
//     OUTPUT config level (ApplyOverridesToConfig), deep-copies into
//     APBWeapon::PartNetworkConfig (+0x3E8), and — once per weapon — calls the
//     native OnRep_PartNetworkConfig to apply everything through the game's
//     own code (attachments + skins + capacity + ornaments).
//   - The DataTable buffer hook is gone: it was redundant (it fired 0x on
//     natural spawn, and ApplyOverridesToConfig already patches every field
//     the buffer hook did, plus the ornament write to buf+0xA0/+0xA8 was dead).

#include "B5WeaponConfig.h"
#include "../Logging/LogManager.h"
#include "../Logging/FileLog.h"
#include <Windows.h>
#include <cstdio>
#include <string>
#include <unordered_set>

using namespace SDK;

#define ODS(msg) WriteLog(msg)

// Native OnRep_PartNetworkConfig — PBWeapon vtable slot 0x908 (vtable off_144981548).
// RVA 0x15FDA40 (imagebase 0x140000000). Signature: void(this, const FPBWeaponNetworkConfig&, char).
static constexpr std::uintptr_t kOnRepPartNetworkConfigRVA = 0x15FDA40;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static std::uintptr_t            s_GameBase = 0;
static FWeaponPartCache          s_PartCache;
static FWeaponOrnamentCache      s_OrnamentCache;
thread_local bool                B5WeaponConfig::g_InB5NativeCall = false;

// Resolve a raw FName (ComparisonIndex | Number<<32) to its display string.
// Used for diagnostics: shows whether an injected skin/ornament FName resolves
// to a real name (i.e. exists in the FName pool / DataTable) or is bogus.
static std::string FNameRawToString(std::uint64_t raw)
{
    if (!raw)
        return std::string("<none>");
    FName fn;
    *reinterpret_cast<std::uint64_t*>(&fn) = raw;
    return fn.ToString();
}

// ---------------------------------------------------------------------------
// ApplyOverridesToConfig — inject attachments + skins into the OUTPUT
// FPBWeaponNetworkConfig (produced by the converter). WeaponPartID /
// WeaponPartSkinID / OrnamentID are plain struct fields here (NOT TMap keys),
// so this is safe — unlike mutating the C1 key in the buffer.
// ---------------------------------------------------------------------------

static void ApplyOverridesToConfig(void* config, const std::string& playerId, std::uint64_t weaponFNameRaw)
{
    std::uint8_t* cfg = static_cast<std::uint8_t*>(config);

    // WeaponPartConfigs TArray at +0x10
    void*  partData = *reinterpret_cast<void**>(cfg + 0x10);
    std::int32_t partNum = *reinterpret_cast<std::int32_t*>(cfg + 0x18);

    auto pIt = s_PartCache.find(playerId);
    if (pIt != s_PartCache.end())
    {
        auto wpnIt = pIt->second.find(weaponFNameRaw);
        if (wpnIt != pIt->second.end() && partData && partNum > 0 && partNum <= 32)
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
                // An ornament id is one of two DIFFERENT things, selected by its `type`
                // (see NEXT_SESSION_PROMPT.md §8):
                //   PartOri / absent ("PT*")  -> painting theme  -> WeaponPartSkinPaintingID (+0x18)
                //   anything else (SMGTiger*) -> part skin mesh  -> WeaponPartSkinID (+0x08)
                // LoadoutSync routes the id into the matching slot; never write one into both.
                if (ov.PaintFName)   *reinterpret_cast<std::uint64_t*>(elem + 0x18) = ov.PaintFName;
                if (ov.SkinFName)    *reinterpret_cast<std::uint64_t*>(elem + 0x08) = ov.SkinFName;
                if (ov.SpecialFName) *reinterpret_cast<std::uint64_t*>(elem + 0x10) = ov.SpecialFName;
                ++modified;
            }

            if (modified > 0)
            {
                char dbg[160];
                sprintf_s(dbg, "[B5-ACTOR] player=%s weapon=0x%04X patched %d parts",
                    playerId.c_str(), static_cast<std::uint32_t>(weaponFNameRaw & 0xFFFFFFFF), modified);
                ODS(dbg);
            }
        }
    }

    // Weapon-level ornament at +0x20
    auto opIt = s_OrnamentCache.find(playerId);
    if (opIt != s_OrnamentCache.end())
    {
        auto ornIt = opIt->second.find(weaponFNameRaw);
        if (ornIt != opIt->second.end() && ornIt->second != 0)
        {
            *reinterpret_cast<std::uint64_t*>(cfg + 0x20) = ornIt->second;
        }
    }
}

// ---------------------------------------------------------------------------
// Cache population
// ---------------------------------------------------------------------------
void B5WeaponConfig::CachePartOverride(
    const std::string& playerId,
    std::uint64_t weaponFNameRaw,
    std::uint64_t partIdRaw,
    std::uint64_t skinFNameRaw,
    std::uint64_t paintFNameRaw)
{
    if (playerId.empty() || !weaponFNameRaw || !partIdRaw)
        return;

    FPartOverride& ov = s_PartCache[playerId][weaponFNameRaw][partIdRaw];
    if (skinFNameRaw)  ov.SkinFName = skinFNameRaw;
    if (paintFNameRaw) ov.PaintFName = paintFNameRaw;

    std::string skinStr = FNameRawToString(skinFNameRaw);
    char dbg[384];
    sprintf_s(dbg, "[B5-CACHE] player=%s weapon=0x%04X part=0x%04X skin=0x%04X (%s) paint=0x%04X",
        playerId.c_str(),
        static_cast<std::uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(partIdRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(skinFNameRaw & 0xFFFFFFFF),
        skinStr.c_str(),
        static_cast<std::uint32_t>(paintFNameRaw & 0xFFFFFFFF));
    ODS(dbg);
}

void B5WeaponConfig::CachePartAttachment(
    const std::string& playerId,
    std::uint64_t weaponFNameRaw,
    std::uint64_t partIdRaw,
    std::uint64_t attachmentFNameRaw)
{
    if (playerId.empty() || !weaponFNameRaw || !partIdRaw || !attachmentFNameRaw)
        return;

    s_PartCache[playerId][weaponFNameRaw][partIdRaw].PartIdFName = attachmentFNameRaw;

    char dbg[256];
    sprintf_s(dbg, "[B5-CACHE] player=%s weapon=0x%04X part=0x%04X attach=0x%04X",
        playerId.c_str(),
        static_cast<std::uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(partIdRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(attachmentFNameRaw & 0xFFFFFFFF));
    ODS(dbg);
}

void B5WeaponConfig::CacheWeaponOrnament(
    const std::string& playerId,
    std::uint64_t weaponFNameRaw,
    std::uint64_t ornamentFNameRaw)
{
    if (playerId.empty() || !weaponFNameRaw || !ornamentFNameRaw)
        return;

    s_OrnamentCache[playerId][weaponFNameRaw] = ornamentFNameRaw;

    std::string ornStr = FNameRawToString(ornamentFNameRaw);
    char dbg[320];
    sprintf_s(dbg, "[B5-CACHE] weapon ornament: player=%s weapon=0x%04X ornament=0x%04X (%s)",
        playerId.c_str(),
        static_cast<std::uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
        static_cast<std::uint32_t>(ornamentFNameRaw & 0xFFFFFFFF),
        ornStr.c_str());
    ODS(dbg);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool B5WeaponConfig::Init(std::uintptr_t gameBase)
{
    s_GameBase = gameBase;

    char buf[128];
    sprintf_s(buf,
        "[B5] OnRep-based weapon config injection ready (OnRep RVA 0x%X)",
        static_cast<unsigned int>(kOnRepPartNetworkConfigRVA));
    ODS(buf);
    return true;
}

// ---------------------------------------------------------------------------
// Owner resolution — map a weapon actor back to its owning player's Steam ID
// ---------------------------------------------------------------------------
//
// The override cache is per-player, so before applying we must know which
// player owns each APBWeapon. Dedicated server: APBWeapon::TopOwner
// (AController) -> PlayerState; fall back through MyPBCharacter (APawn::PlayerState)
// for weapons whose TopOwner hasn't replicated yet.
static std::string GetWeaponOwnerPlayerId(APBWeapon* weapon)
{
    if (!weapon)
        return {};

    AController* top = weapon->TopOwner;
    if (top && top->PlayerState)
    {
        return static_cast<APBPlayerState*>(top->PlayerState)->GetDefaultIDStr().ToString();
    }

    APBCharacter* chr = weapon->MyPBCharacter;
    if (chr)
    {
        APawn* pawn = static_cast<APawn*>(chr);
        if (pawn && pawn->PlayerState)
            return static_cast<APBPlayerState*>(pawn->PlayerState)->GetDefaultIDStr().ToString();
    }

    return {};
}

// ---------------------------------------------------------------------------
// ReapplyToExistingWeapons — post-cache fixup
//
// Called after cache is populated. Iterates all APBWeapon actors in the
// current world, checks if their WeaponID matches a cached weapon (for their
// OWNER's cache entry), and if so: builds the DataTable buffer, converts it,
// injects the overrides, deep-copies into the actor (+0x3E8), and — once per
// weapon — applies via the native OnRep_PartNetworkConfig.
// ---------------------------------------------------------------------------
static int ReapplyToExistingWeaponsImpl()
{
    if (s_PartCache.empty() && s_OrnamentCache.empty())
        return 0;

    UWorld* world = UWorld::GetWorld();
    if (!world) return 0;

    // Weapons we've already full-applied once. After the first OnRep we only do a
    // cheap DeepCopy, so the reapply can run every frame without the apply cost.
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

            // Resolve the owning player so we only apply THIS player's overrides
            // (the cache is per-player — otherwise the last deployer clobbers
            // everyone else using the same weapon).
            std::string ownerId = GetWeaponOwnerPlayerId(weapon);

            bool hasPartOverride = false;
            bool hasOrnOverride  = false;
            if (!ownerId.empty())
            {
                auto pIt = s_PartCache.find(ownerId);
                hasPartOverride = (pIt != s_PartCache.end() && pIt->second.count(weaponFNameRaw) > 0);
                auto oIt = s_OrnamentCache.find(ownerId);
                hasOrnOverride  = (oIt != s_OrnamentCache.end() && oIt->second.count(weaponFNameRaw) > 0);
            }

            // --- FName diagnostic: log each weapon ID ONCE (first time seen),
            // plus a HIT whenever one matches the cache. This catches weapons that
            // spawn later (e.g. after the player deploys), not just at connect.
            {
                static std::unordered_set<uint64_t> s_SeenWeapons;
                static bool s_DumpedCache = false;

                bool isNew = s_SeenWeapons.insert(weaponFNameRaw).second;
                if (isNew || (hasPartOverride || hasOrnOverride))
                {
                    char dbg[384];
                    sprintf_s(dbg, "[B5-FNAME] owner=%s getid=0x%04X (%s) partcfg=0x%04X actor=%p %s",
                        ownerId.empty() ? "<none>" : ownerId.c_str(),
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
                    ODS("[B5-FNAME] cached weapon FNames (per player):");
                    for (const auto& [pid, wmap] : s_PartCache)
                    {
                        for (const auto& [wkey, _] : wmap)
                        {
                            char cb[192];
                            sprintf_s(cb, "[B5-FNAME]   player=%s cached=0x%04X",
                                pid.c_str(), static_cast<uint32_t>(wkey & 0xFFFFFFFF));
                            ODS(cb);
                        }
                    }
                }
            }

            if (!hasPartOverride && !hasOrnOverride) continue;

            char buf[256];
            sprintf_s(buf, "[B5-REAPPLY] Found cached weapon=0x%04X owner=%s at actor=%p",
                static_cast<uint32_t>(weaponFNameRaw & 0xFFFFFFFF),
                ownerId.c_str(),
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
            ApplyOverridesToConfig(converted, ownerId, weaponFNameRaw);

            // Deep-copy the PartNetworkConfig (attachments + skins) into +0x3E8.
            using FnDeepCopy = void* (__fastcall*)(void* dst, void* src);
            FnDeepCopy DeepCopy = reinterpret_cast<FnDeepCopy>(s_GameBase + 0x154B3A0);
            DeepCopy(wpn + 0x3E8, converted);

            // First time we see this weapon: apply through the game's own
            // OnRep_PartNetworkConfig, which runs the whole chain natively
            // (attachments + skins + capacity via NotifyRecalculateSpecialPartOffset
            // + ornaments). After that, the DeepCopy above keeps the config fresh
            // cheaply, so the reapply can run every frame without the apply cost.
            if (s_InitializedWeapons.insert(weapon).second)
            {
                using FnOnRep = void (__fastcall*)(void* self, void* config, char flag);
                FnOnRep OnRep = reinterpret_cast<FnOnRep>(s_GameBase + kOnRepPartNetworkConfigRVA);
                OnRep(weapon, converted, 0);
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
            uint64_t weaponClassId = *reinterpret_cast<uint64_t*>(wpn + 0x03E8 + 0x30); // WeaponClassID (FName)
            std::string wclassStr = FNameRawToString(weaponClassId);
            sprintf_s(buf, "[B5-REAPPLY] Wrote: ornament=0x%04X parts=%d skin0=0x%04X class=%s",
                ornCI, partNum, skin0, wclassStr.c_str());
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
