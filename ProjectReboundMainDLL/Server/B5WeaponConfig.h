// B5WeaponConfig.h
// B.5 Weapon part config injection — hook-based approach.
//
// Architecture:
//   sub_141654C80 (DataTable buffer builder) is hooked.
//   After it builds the default buffer, we modify Container#2 elements
//   in-place with custom ornament/skin FNames from a pre-populated cache.
//   The converter (sub_141546780) then picks up our modified FNames
//   and the official spawn pipeline handles everything else.
//
// The cache is populated during ParseAndPush (ApplyCustomWeaponConfigs),
// after FName conversion so all values are live runtime FNames.

#pragma once

#include "../SDK.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Cache structures
// ---------------------------------------------------------------------------
struct FPartOverride
{
    std::uint64_t PartIdFName{};  // FName raw for WeaponPartID (the attachment)
    std::uint64_t SkinFName{};    // FName raw for WeaponPartSkinID
    std::uint64_t SpecialFName{}; // FName raw for WeaponPartSpecialSkinID
    std::uint64_t PaintFName{};   // FName raw for WeaponPartSkinPaintingID
};

// weaponFName_raw → partID_raw → override
using FWeaponPartCache = std::unordered_map<std::uint64_t,
    std::unordered_map<std::uint64_t, FPartOverride>>;

// weaponFName_raw → weapon-level ornament FName_raw (slot 12 / weapon body skin)
using FWeaponOrnamentCache = std::unordered_map<std::uint64_t, std::uint64_t>;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace B5WeaponConfig
{

// Call once during DLL init.
// Installs the sub_141654C80 hook.
bool Init(std::uintptr_t gameBase);

// Populate the override cache with a single weapon's part configs.
// Called from ApplyCustomWeaponConfigs after FName conversion.
//   weaponFNameRaw: raw uint64 of the weapon FName
//   partIdRaw:      raw uint64 of the part FName (C1 key / C2 PartID)
//   skinFNameRaw:   raw uint64 of the skin ornament FName (0 if none)
//   paintFNameRaw:  raw uint64 of the paint FName (0 if none)
void CachePartOverride(std::uint64_t weaponFNameRaw,
                       std::uint64_t partIdRaw,
                       std::uint64_t skinFNameRaw,
                       std::uint64_t paintFNameRaw);

// Cache the custom attachment (part id) for a part slot keyed by the default
// part id. The hook uses this to replace the default part id with the custom one.
void CachePartAttachment(std::uint64_t weaponFNameRaw,
                         std::uint64_t partIdRaw,
                         std::uint64_t attachmentFNameRaw);

void CacheWeaponOrnament(std::uint64_t weaponFNameRaw,
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
