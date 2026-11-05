// CharCosmetics.cpp
// B.5 character cosmetics — operator skin, head ornament, arm badge.
//
// Strategy mirrors B5WeaponConfig: ParseAndPush caches per-player, per-role
// cosmetics (already converted to runtime FNames); a tick-driven reapply then
// finds the spawned APBCharacter for that player, writes CharacterSkinConfig
// (+0x698) and calls the game's own OnRep_CharacterSkinConfig so the change is
// applied and replicated to clients.
//
// The write is idempotent: a character is only touched when its current
// CharacterSkinConfig differs from the cached one, so this can run every few
// ticks without spamming OnReps.

#include "CharCosmetics.h"
#include "B5WeaponConfig.h"
#include "../SDK.hpp"
#include "../Logging/LogManager.h"
#include "../Logging/FileLog.h"

#include <Windows.h>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace SDK;

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

// playerId (Steam ID) -> role ComparisonIndex (FName low 32 bits) -> cosmetics.
// Keying the role by ComparisonIndex only: the JSON role key has Number == 0,
// while the replicated CharacterID FName may carry any Number.
using FRoleCosmeticMap = std::unordered_map<std::uint32_t, FCharCosmeticCfg>;
static std::unordered_map<std::string, FRoleCosmeticMap> s_Cache;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static FName AsFName(std::uint64_t raw)
{
    FName fn;
    *reinterpret_cast<std::uint64_t*>(&fn) = raw;
    return fn;
}

static std::uint64_t FNameRaw(const FName& fn)
{
    return *reinterpret_cast<const std::uint64_t*>(&fn);
}

static std::string FNameRawToString(std::uint64_t raw)
{
    if (!raw)
        return std::string("<none>");
    return AsFName(raw).ToString();
}

// Owner: APBCharacter::PBPlayerState (+0x570) is the reliable pointer on a
// dedicated server; APawn::PlayerState is the fallback while it replicates.
static std::string GetCharacterOwnerId(APBCharacter* chr)
{
    if (!chr)
        return {};

    if (chr->PBPlayerState)
        return chr->PBPlayerState->GetDefaultIDStr().ToString();

    APawn* pawn = static_cast<APawn*>(chr);
    if (pawn->PlayerState)
        return static_cast<APBPlayerState*>(pawn->PlayerState)->GetDefaultIDStr().ToString();

    return {};
}

// Does the character already carry exactly the cached config?
static bool MatchesConfig(const FPBCharacterNetworkConfig& sc, const FCharCosmeticCfg& cfg)
{
    int expected = 0;
    if (cfg.SkinFName)  ++expected;
    if (cfg.HeadFName)  ++expected;
    if (cfg.BadgeFName) ++expected;

    if (sc.SkinClassArray.Num() != expected || sc.SkinIDArray.Num() != expected)
        return false;

    int i = 0;
    auto same = [&](EPBSkinClass cls, std::uint64_t raw) -> bool
    {
        if (!raw)
            return true;
        if (i >= sc.SkinIDArray.Num())
            return false;
        const bool ok = sc.SkinClassArray[i] == cls && FNameRaw(sc.SkinIDArray[i]) == raw;
        ++i;
        return ok;
    };

    // Order is the one we write (Skin, Ornaments, SpecialSlot).
    if (!same(EPBSkinClass::Skin,        cfg.SkinFName))  return false;
    if (!same(EPBSkinClass::Ornaments,   cfg.HeadFName))  return false;
    if (!same(EPBSkinClass::SpecialSlot, cfg.BadgeFName)) return false;

    // The paint is a single scalar, not part of the arrays, so it needs its own
    // comparison: an empty PaintFName means "no paint" and must clear the field.
    if (FNameRaw(sc.SkinPaintingID) != cfg.PaintFName)
        return false;

    return true;
}

// Write the pairs. Clear + Add, never CopyFrom: the SDKHelper CopyFrom is
// broken (memcpy without multiplying by ElementSize).
static void WriteConfig(APBCharacter* chr, const FCharCosmeticCfg& cfg)
{
    FPBCharacterNetworkConfig& sc = chr->CharacterSkinConfig;

    sc.SkinClassArray.Clear();
    sc.SkinIDArray.Clear();

    if (cfg.SkinFName)
    {
        sc.SkinClassArray.Add(EPBSkinClass::Skin);
        sc.SkinIDArray.Add(AsFName(cfg.SkinFName));
    }
    if (cfg.HeadFName)
    {
        sc.SkinClassArray.Add(EPBSkinClass::Ornaments);
        sc.SkinIDArray.Add(AsFName(cfg.HeadFName));
    }
    if (cfg.BadgeFName)
    {
        sc.SkinClassArray.Add(EPBSkinClass::SpecialSlot);
        sc.SkinIDArray.Add(AsFName(cfg.BadgeFName));
    }
    // Always assigned: a loadout with no paint clears the scalar to "None"
    // instead of leaving a previously applied one in place.
    sc.SkinPaintingID = AsFName(cfg.PaintFName);
}

// ---------------------------------------------------------------------------
// Cache population
// ---------------------------------------------------------------------------
void CharCosmetics::CacheRoleCosmetics(const std::string& playerId,
                                       std::uint64_t roleFNameRaw,
                                       const FCharCosmeticCfg& cfg)
{
    if (playerId.empty() || !roleFNameRaw)
        return;

    const std::uint32_t roleCI = static_cast<std::uint32_t>(roleFNameRaw & 0xFFFFFFFFu);
    if (!roleCI)
        return;

    s_Cache[playerId][roleCI] = cfg;

    LogF("[CHAR-SKIN] cache player=%s role=%s skin=%s paint=%s head=%s badge=%s",
        playerId.c_str(),
        FNameRawToString(roleFNameRaw).c_str(),
        FNameRawToString(cfg.SkinFName).c_str(),
        FNameRawToString(cfg.PaintFName).c_str(),
        FNameRawToString(cfg.HeadFName).c_str(),
        FNameRawToString(cfg.BadgeFName).c_str());
}

// ---------------------------------------------------------------------------
// Reapply
// ---------------------------------------------------------------------------
static int ReapplyToExistingCharactersImpl()
{
    if (s_Cache.empty())
        return 0;

    UWorld* world = UWorld::GetWorld();
    if (!world)
        return 0;

    int applied = 0;

    // One-shot diagnostics: a character we recognised but could not match to a
    // cached role is the likely failure mode (CharacterID not replicated yet, or
    // the role FName differs). Log each distinct (owner, role) once, capped.
    static std::unordered_set<std::string> s_MissLogged;

    for (int li = 0; li < world->Levels.Num(); ++li)
    {
        ULevel* level = world->Levels[li];
        if (!level)
            continue;

        for (int ai = 0; ai < level->Actors.Num(); ++ai)
        {
            AActor* actor = level->Actors[ai];
            if (!actor || !actor->IsA(APBCharacter::StaticClass()))
                continue;

            APBCharacter* chr = static_cast<APBCharacter*>(actor);

            const std::string ownerId = GetCharacterOwnerId(chr);
            if (ownerId.empty())
                continue;

            auto pIt = s_Cache.find(ownerId);
            if (pIt == s_Cache.end())
                continue;

            const FName roleFn = chr->CharacterID;
            const std::uint64_t roleRaw = FNameRaw(roleFn);
            const std::uint32_t roleCI = static_cast<std::uint32_t>(roleRaw & 0xFFFFFFFFu);

            auto rIt = (roleCI ? pIt->second.find(roleCI) : pIt->second.end());
            if (roleCI == 0 || rIt == pIt->second.end())
            {
                if (s_MissLogged.size() < 32)
                {
                    const std::string key = ownerId + "/" + std::to_string(roleCI);
                    if (s_MissLogged.insert(key).second)
                    {
                        LogF("[CHAR-SKIN] MISS owner=%s role=%s cachedRoles=%d",
                            ownerId.c_str(),
                            roleCI ? roleFn.ToString().c_str() : "<none>",
                            (int)pIt->second.size());
                    }
                }
                continue;
            }

            const FCharCosmeticCfg& cfg = rIt->second;

            // Already correct: skip so we don't re-fire OnRep every tick.
            if (MatchesConfig(chr->CharacterSkinConfig, cfg))
                continue;

            WriteConfig(chr, cfg);

            // Apply through the game's own RepNotify. Resolve it ourselves rather
            // than using the SDK wrapper: the wrapper dereferences a null UFunction
            // if the class-name lookup ever fails.
            UFunction* onRep = chr->Class->GetFunction("PBCharacter", "OnRep_CharacterSkinConfig");
            if (onRep)
            {
                B5WeaponConfig::g_InB5NativeCall = true;
                chr->ProcessEvent(onRep, nullptr);
                B5WeaponConfig::g_InB5NativeCall = false;
            }
            else
            {
                WriteLog("[CHAR-SKIN] OnRep_CharacterSkinConfig not found on class");
            }

            // Make sure the changed struct is re-sent to the owning client.
            UNetPushModelHelpers::MarkPropertyDirty(
                chr, UKismetStringLibrary::Conv_StringToName(L"CharacterSkinConfig"));
            chr->ForceNetUpdate();

            ++applied;

            LogF("[CHAR-SKIN] apply owner=%s role=%s skin=%s paint=%s head=%s badge=%s",
                ownerId.c_str(),
                roleFn.ToString().c_str(),
                FNameRawToString(cfg.SkinFName).c_str(),
                FNameRawToString(cfg.PaintFName).c_str(),
                FNameRawToString(cfg.HeadFName).c_str(),
                FNameRawToString(cfg.BadgeFName).c_str());
        }
    }

    return applied;
}

namespace CharCosmetics
{

int ReapplyToExistingCharacters()
{
    const int count = ReapplyToExistingCharactersImpl();
    LogF("[CHAR-SKIN] Done: %d characters updated", count);
    return count;
}

void PrunePlayer(const std::string& playerId)
{
    if (!playerId.empty())
        s_Cache.erase(playerId);
}

void CachedPlayerIds(std::vector<std::string>& out)
{
    for (const auto& entry : s_Cache)
        out.push_back(entry.first);
}

bool DeferredReapplyTick()
{
    static int s_TickCounter = 0;

    // Keep scanning while a cache exists: characters spawn after role selection,
    // not at PostLogin, and respawn again after every death.
    if (s_Cache.empty())
        return false;

    ++s_TickCounter;
    if (s_TickCounter % 2 != 0) // ~33ms at 60fps; no work once every character matches
        return true;

    const int count = ReapplyToExistingCharactersImpl();
    if (count > 0)
        LogF("[CHAR-SKIN] tick %d: %d characters updated", s_TickCounter, count);

    return true;
}

} // namespace CharCosmetics
