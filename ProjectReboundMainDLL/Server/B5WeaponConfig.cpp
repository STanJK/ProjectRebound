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
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace SDK;

// Native OnRep_PartNetworkConfig — PBWeapon vtable slot 0x908 (vtable off_144981548).
// RVA 0x15FDA40 (imagebase 0x140000000). Signature: void(this, const FPBWeaponNetworkConfig&, char).
static constexpr std::uintptr_t kOnRepPartNetworkConfigRVA = 0x15FDA40;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static std::uintptr_t            s_GameBase = 0;
static FWeaponPartCache          s_PartCache;
thread_local bool                B5WeaponConfig::g_InB5NativeCall = false;

// ---------------------------------------------------------------------------
// ApplyOverridesToConfig — inject attachments + skins into the OUTPUT
// FPBWeaponNetworkConfig (produced by the converter). WeaponPartID /
// WeaponPartSkinID / OrnamentID are plain struct fields here (NOT TMap keys),
// so this is safe — unlike mutating the C1 key in the buffer.
//
// OrnamentID (+0x20) is deliberately NOT written: it is the `WO-*` weapon
// badge slot (EPBItemType::WeaponOrnament), not the weapon suit. Suits are
// expanded per slot into WeaponPartSkinID by LoadoutSync::ApplySuitExpansion.
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
            for (int i = 0; i < partNum; ++i)
            {
                std::uint8_t* elem = static_cast<std::uint8_t*>(partData) + 32 * i;
                std::uint64_t partIdRaw = *reinterpret_cast<std::uint64_t*>(elem + 0x00);

                auto ovIt = overrides.find(partIdRaw);
                if (ovIt == overrides.end()) continue;

                const FPartOverride& ov = ovIt->second;
                if (ov.PartIdFName)  *reinterpret_cast<std::uint64_t*>(elem + 0x00) = ov.PartIdFName; // WeaponPartID (attachment)
                // An ornament id is one of two DIFFERENT things, selected by its `type`
                // (see Docs/Loadout-Persistence.md):
                //   PartOri / absent ("PT*")  -> painting theme  -> WeaponPartSkinPaintingID (+0x18)
                //   anything else (SMGTiger*) -> part skin mesh  -> WeaponPartSkinID (+0x08)
                // LoadoutSync routes the id into the matching slot; never write one into both.
                if (ov.PaintFName)   *reinterpret_cast<std::uint64_t*>(elem + 0x18) = ov.PaintFName;
                if (ov.SkinFName)    *reinterpret_cast<std::uint64_t*>(elem + 0x08) = ov.SkinFName;
                if (ov.SpecialFName) *reinterpret_cast<std::uint64_t*>(elem + 0x10) = ov.SpecialFName;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Config equality — lets the reapply loop skip weapons whose PartNetworkConfig
// already carries what we would write. The two TArrays hold plain data (FName
// raws, uint8 slot enums), so their contents compare byte-for-byte; the array
// POINTERS do not match (the live config and the freshly converted one own
// different allocations), which is why a whole-struct memcmp would report a
// change on every tick and never converge.
// ---------------------------------------------------------------------------
static bool SameWeaponConfig(const void* liveCfg, const void* freshCfg)
{
    const std::uint8_t* a = static_cast<const std::uint8_t*>(liveCfg);
    const std::uint8_t* b = static_cast<const std::uint8_t*>(freshCfg);

    // +0x00 WeaponPartSlotTypeArray (TArray<EPBPartSlotType>, 1 byte each)
    const std::int32_t aSlots = *reinterpret_cast<const std::int32_t*>(a + 0x08);
    const std::int32_t bSlots = *reinterpret_cast<const std::int32_t*>(b + 0x08);
    if (aSlots != bSlots) return false;
    if (aSlots > 0)
    {
        const void* ap = *reinterpret_cast<void* const*>(a + 0x00);
        const void* bp = *reinterpret_cast<void* const*>(b + 0x00);
        if (!ap || !bp) return false;
        if (memcmp(ap, bp, static_cast<size_t>(aSlots)) != 0) return false;
    }

    // +0x10 WeaponPartConfigs (TArray<FPBWeaponPartNetworkConfig>, 0x20 each)
    const std::int32_t aParts = *reinterpret_cast<const std::int32_t*>(a + 0x18);
    const std::int32_t bParts = *reinterpret_cast<const std::int32_t*>(b + 0x18);
    if (aParts != bParts) return false;
    if (aParts > 0)
    {
        const void* ap = *reinterpret_cast<void* const*>(a + 0x10);
        const void* bp = *reinterpret_cast<void* const*>(b + 0x10);
        if (!ap || !bp) return false;
        if (memcmp(ap, bp, static_cast<size_t>(aParts) * 0x20) != 0) return false;
    }

    // +0x20 OrnamentID, +0x28 WeaponID, +0x30 WeaponClassID (FName = 8 byte raw)
    for (const int off : { 0x20, 0x28, 0x30 })
        if (*reinterpret_cast<const std::uint64_t*>(a + off) !=
            *reinterpret_cast<const std::uint64_t*>(b + off))
            return false;

    return true;
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
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool B5WeaponConfig::Init(std::uintptr_t gameBase)
{
    s_GameBase = gameBase;

    LogF("[B5] OnRep-based weapon config injection ready (OnRep RVA 0x%X)",
         static_cast<unsigned int>(kOnRepPartNetworkConfigRVA));
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
static int ReapplyToExistingWeaponsImpl(bool force)
{
    if (s_PartCache.empty())
        return 0;

    UWorld* world = UWorld::GetWorld();
    if (!world) return 0;

    // Weapons we've already full-applied once. After the first OnRep we only do a
    // cheap DeepCopy, so the reapply can run every frame without the apply cost.
    // Value = UObject index at the time of application, to tell a recycled actor
    // address apart from the same weapon.
    static std::unordered_map<void*, int32> s_InitializedWeapons;

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
            if (!weaponFNameRaw) continue;

            // Resolve the owning player so we only apply THIS player's overrides
            // (the cache is per-player — otherwise the last deployer clobbers
            // everyone else using the same weapon).
            std::string ownerId = GetWeaponOwnerPlayerId(weapon);

            bool hasPartOverride = false;
            if (!ownerId.empty())
            {
                auto pIt = s_PartCache.find(ownerId);
                hasPartOverride = (pIt != s_PartCache.end() && pIt->second.count(weaponFNameRaw) > 0);
            }

            if (!hasPartOverride) continue;

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

            // First time we see this weapon: apply through the game's own
            // OnRep_PartNetworkConfig, which runs the whole chain natively
            // (attachments + skins + capacity via NotifyRecalculateSpecialPartOffset
            // + ornaments). After that, the DeepCopy below keeps the config fresh
            // cheaply, so the reapply can run every frame without the apply cost.
            //
            // Keyed by address *and* UObject index: UE recycles actor memory, and a
            // bare pointer would let a brand-new weapon look already-initialised,
            // skipping its one OnRep and leaving a stale model on the client.
            auto initIt = s_InitializedWeapons.find(weapon);
            const bool firstSight = (initIt == s_InitializedWeapons.end() ||
                                     initIt->second != weapon->Index);

            // Already in effect: the actor carries exactly the config we just
            // produced, so the DeepCopy + the net-forcing block below would only
            // re-send an unchanged PartNetworkConfig (~30x/s per weapon) and make
            // the client re-run its OnRep rebuild for nothing. `force` keeps the
            // join path re-pushing everything once, because a client that just
            // connected still needs state the server already holds.
            if (!firstSight && !force && SameWeaponConfig(wpn + 0x3E8, converted))
            {
                B5WeaponConfig::g_InB5NativeCall = false;
                continue;
            }

            // Deep-copy the PartNetworkConfig (attachments + skins) into +0x3E8.
            using FnDeepCopy = void* (__fastcall*)(void* dst, void* src);
            FnDeepCopy DeepCopy = reinterpret_cast<FnDeepCopy>(s_GameBase + 0x154B3A0);
            DeepCopy(wpn + 0x3E8, converted);

            if (firstSight)
            {
                s_InitializedWeapons[weapon] = weapon->Index;
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
            weapon->SetReplicates(false);
            weapon->SetReplicates(true);

            ++reapplied;
        }
    }
    return reapplied;
}

namespace B5WeaponConfig {

int ReapplyToExistingWeapons()
{
    // Join path: force one full push, so a client that has just connected gets
    // state the server already holds (the tick path skips unchanged weapons).
    int count = ReapplyToExistingWeaponsImpl(/*force=*/true);
    LogF("[B5-REAPPLY] Done: %d weapons updated", count);
    return count;
}

void PrunePlayer(const std::string& playerId)
{
    if (!playerId.empty())
        s_PartCache.erase(playerId);
}

void CachedPlayerIds(std::vector<std::string>& out)
{
    for (const auto& entry : s_PartCache)
        out.push_back(entry.first);
}

bool DeferredReapplyTick()
{
    static int s_TickCounter = 0;

    // Keep scanning while a cache exists, so we catch weapons that spawn after
    // the player actually deploys (not just the connect-time actors). Weapons
    // whose config already matches are skipped, so this converges to no work.
    if (s_PartCache.empty())
        return false;

    ++s_TickCounter;
    if (s_TickCounter % 2 != 0) // ~33ms at 60fps
        return true;

    ReapplyToExistingWeaponsImpl(/*force=*/false);
    return true;
}

} // namespace B5WeaponConfig
