// B5WeaponConfig.h
// B.5 Weapon part config injection — OnRep-based approach.
//
// Architecture:
//   The override cache is populated during ParseAndPush (ApplyCustomWeaponConfigs),
//   after FName conversion so all values are live runtime FNames.
//   ReapplyToExistingWeapons() then, for each matching APBWeapon actor:
//     - builds the DataTable buffer (sub_141654C80), converts it to
//       FPBWeaponNetworkConfig (sub_141546780),
//     - injects attachments/skins/ornament at the OUTPUT config level
//       (ApplyOverridesToConfig),
//     - deep-copies into PartNetworkConfig (+0x3E8) via sub_14154B3A0,
//     - once per weapon, applies everything by calling the game's own native
//       OnRep_PartNetworkConfig (vtable[0x908]) — no manual InitWeapon/
//       ApplyPartModification/hierarchy-walk.

#pragma once

#include "../SDK.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Cache structures
// ---------------------------------------------------------------------------
struct FPartOverride
{
    std::uint64_t PartIdFName{};  // FName raw for WeaponPartID (the attachment)
    std::uint64_t SkinFName{};    // FName raw for WeaponPartSkinID (+0x08, part skin MESH)
    std::uint64_t SpecialFName{}; // FName raw for WeaponPartSpecialSkinID
    std::uint64_t PaintFName{};   // FName raw for WeaponPartSkinPaintingID (+0x18, paint/theme)
};
// SkinFName and PaintFName are mutually exclusive: an ornament id is one or the
// other, decided by its `type`. For skin-typed ornaments SkinFName holds the ornament
// TYPE (e.g. "SMGTigerMZL", an EPBItemType::WeaponPartSkin), NOT its id — the id is a
// painting-shaped name with no item entry and never resolves.

// Per-player cache: steamId → weaponFName_raw → partID_raw → override.
// Keying by player prevents the last player who deployed with a given weapon
// from clobbering every other player using that same weapon.
using FWeaponPartCache = std::unordered_map<std::string,
    std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, FPartOverride>>>;

// Per-player cache: steamId → weaponFName_raw → weapon-level ornament FName_raw.
using FWeaponOrnamentCache = std::unordered_map<std::string,
    std::unordered_map<std::uint64_t, std::uint64_t>>;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace B5WeaponConfig
{

// Call once during DLL init.
// Stores the game base address (no hook is installed anymore).
bool Init(std::uintptr_t gameBase);

// Populate the override cache with a single weapon's part configs.
// Called from ApplyCustomWeaponConfigs after FName conversion.
//   weaponFNameRaw: raw uint64 of the weapon FName
//   partIdRaw:      raw uint64 of the part FName (C1 key / C2 PartID)
//   skinFNameRaw:   raw uint64 of the skin ornament FName (0 if none)
//   paintFNameRaw:  raw uint64 of the paint FName (0 if none)
void CachePartOverride(const std::string& playerId,
                       std::uint64_t weaponFNameRaw,
                       std::uint64_t partIdRaw,
                       std::uint64_t skinFNameRaw,
                       std::uint64_t paintFNameRaw);

// Cache the custom attachment (part id) for a part slot keyed by the default
// part id. The hook uses this to replace the default part id with the custom one.
void CachePartAttachment(const std::string& playerId,
                         std::uint64_t weaponFNameRaw,
                         std::uint64_t partIdRaw,
                         std::uint64_t attachmentFNameRaw);

void CacheWeaponOrnament(const std::string& playerId,
                         std::uint64_t weaponFNameRaw,
                         std::uint64_t ornamentFNameRaw);

// Accessor for the g_InB5NativeCall flag (shared with ProcessEvent hook).
extern thread_local bool g_InB5NativeCall;

// After cache is populated, re-apply custom configs to all existing
// APBWeapon actors that match cached weapons. Returns count of weapons updated.
// Call from ParseAndPush after ApplyCustomWeaponConfigs.
int ReapplyToExistingWeapons();

// Called from TickFlush to continue reapplying for frames after cache init.
// Returns true while still actively reapplying (limits to ~5 seconds).
bool DeferredReapplyTick();

} // namespace B5WeaponConfig
