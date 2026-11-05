// CharCosmetics.h
// B.5 character cosmetics — operator skin, head ornament, arm badge.
//
// The character counterpart of B5WeaponConfig. The loadout JSON already carries
// the four values per role; nothing on the server needs to be asked for them:
//
//   _skinToken    EPBItemType::SpaceSuit              -> EPBSkinClass::Skin        (1)
//   cosmetic10    EPBItemType::HeadAccessory          -> EPBSkinClass::Ornaments   (3)
//   cosmetic9     EPBItemType::ArmBadge               -> EPBSkinClass::SpecialSlot (4)
//   _ornamentId   EPBItemType::CharacterSuitePainting -> FPBCharacterNetworkConfig::SkinPaintingID
//
// All four are written into APBCharacter::CharacterSkinConfig (+0x698), a
// Net+RepNotify FPBCharacterNetworkConfig (0x28), then applied with
// OnRep_CharacterSkinConfig() + ForceNetUpdate() so the change reaches clients.
//
// See Docs/Loadout-Persistence.md for the evidence behind the class mapping.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One role's four character cosmetics, already converted to runtime FName raws
// (0 = absent / "None").
struct FCharCosmeticCfg
{
    std::uint64_t SkinFName{};   // EPBSkinClass::Skin        <- _skinToken
    std::uint64_t HeadFName{};   // EPBSkinClass::Ornaments   <- cosmetic10
    std::uint64_t BadgeFName{};  // EPBSkinClass::SpecialSlot <- cosmetic9
    std::uint64_t PaintFName{};  // SkinPaintingID scalar     <- _ornamentId
};

namespace CharCosmetics
{

// Populate the per-player cache for one role. Called from ParseAndPush (game
// thread) after the FName conversion, so every raw is a live runtime FName.
//   roleFNameRaw: raw uint64 of the role FName (the JSON role key)
void CacheRoleCosmetics(const std::string& playerId,
                        std::uint64_t roleFNameRaw,
                        const FCharCosmeticCfg& cfg);

// Walk the current world and write + apply the config on every APBCharacter
// whose owner and role are cached. Cheap when nothing changed: a character is
// only written when its current CharacterSkinConfig differs from the cache.
// Returns the number of characters updated.
int ReapplyToExistingCharacters();

// Called from TickFlush; keeps covering characters that spawn later
// (role selection, respawn). Returns true while a cache exists.
bool DeferredReapplyTick();

// Drop a departed player's cached cosmetics. Called by the slow prune sweep in
// LoadoutSync once the player is gone for two consecutive sweeps.
void PrunePlayer(const std::string& playerId);

// Append the Steam IDs currently present in the cache (for that sweep).
void CachedPlayerIds(std::vector<std::string>& out);

} // namespace CharCosmetics
