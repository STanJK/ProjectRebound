# Loadout persistence (B.5)

Applies each player's saved loadout on a dedicated server instead of the stock one:
which weapon sits in each role slot, attachments, magazine capacity, weapon suits /
paints / part skins, and the three character cosmetics (operator skin, head ornament,
arm badge).

The loadout is fetched from the metaserver's `GET /api/loadout/<steamId>` endpoint and
written through the game's own replication paths, so the client needs no changes for the
server-side half.

Target binary: Boundary, UE 4.27.2 Shipping, stripped, imagebase `0x140000000`. Offsets
and RVAs below belong to that build and are used as constants (no signature scan).

## Components

| File | Role |
|---|---|
| `Server/LoadoutSync.cpp/.h` | join hook, HTTP fetch, JSON parse, per-role inventory build, 192 B weapon-entry injection, per-player caches, cache pruning |
| `Server/B5WeaponConfig.cpp/.h` | converts a weapon's DataTable row to `FPBWeaponNetworkConfig`, patches the overrides in, writes it into the actor and forces replication |
| `Server/CharCosmetics.cpp/.h` | writes `APBCharacter::CharacterSkinConfig` and applies it |
| `Logging/FileLog.h` | `LogF()` — one bounded, truncating log helper (`_vsnprintf_s`) |
| `Loadout/LoadoutFix.cpp` (existing) | client-side equip-widget fix; only its error handling and logging changed |

## Flow

1. `PostLogin` → `LoadoutSync::OnPlayerJoined` takes the Steam ID off the PlayerState and
   starts a detached fetch: `GET http://127.0.0.1:8000/api/loadout/<steamId>`, three
   attempts two seconds apart, 256 KB cap.
2. The response goes into a pending queue; `TickFlush` drains it on the game thread
   (`DrainPending`), re-resolving the PlayerState from the Steam ID so a player who left
   in the meantime is simply dropped instead of being dereferenced.
3. `ParseAndPush` builds the role list and, per role, the ordered inventory the client
   expects, then:
   - `APBPlayerState::ClientInitFieldMod(...)` — field-mod / role config,
   - `ClientRefreshRolePreOrderingInventory(...)` — per-role inventory,
   - `RunPhase2Injection` — inserts a 192 B entry per weapon into the PlayerState's
     per-role map (see below),
   - `B5WeaponConfig::ReapplyToExistingWeapons()` — pushes the cached overrides onto
     weapons that already exist.
4. From then on `TickFlush` drives `DeferredReapplyTick()` for both cosmetics systems,
   which catch weapons and characters that spawn later (deploy, respawn).

## The per-role weapon map

`APBPlayerState +0x700` is a `TMap<FName, role-entry>` with 272-byte values. Inside each
role entry, `+0xB8` is a second `TMap<FName, 192-byte weapon entry>`. The vanilla server
fills that map while it builds the match; the loadout code inserts the entries for the
weapons the player actually owns, and leaves existing entries alone (idempotent, and
deliberately per-PlayerState — every joining player needs their own).

| Purpose | RVA |
|---|---|
| `Find` on the 272-byte role map | `0x15AEB20` |
| `Find` / `Insert` on the 192-byte weapon map | `0x164F740` / `0x164ED60` |
| Build the 192 B value from a weapon FName | `0x1654C80` |
| Build the role's weapon list (used for matching) | `0x1544D00` |
| FName hash used by that TMap | reimplemented (see `HashFName`) |

## Weapon config: `APBWeapon +0x3E8`

`FPBWeaponNetworkConfig` (0x38), replicated with a RepNotify:

| Offset | Field |
|---|---|
| `+0x00` | `TArray<EPBPartSlotType>` (1-byte elements) |
| `+0x10` | `TArray<FPBWeaponPartNetworkConfig>` (0x20 each) |
| `+0x20` | `OrnamentID` — the `WO-*` weapon **badge** slot (not implemented, see below) |
| `+0x28` | `WeaponID` |
| `+0x30` | `WeaponClassID` |

Each 0x20-byte part config: `+0x00` `WeaponPartID` (attachment), `+0x08`
`WeaponPartSkinID` (part skin mesh), `+0x10` special part, `+0x18`
`WeaponPartSkinPaintingID` (paint).

Apply path per weapon (`B5WeaponConfig.cpp`):

1. build the DataTable row (`0x1654C80`) and convert it (`0x1546780`) into a config,
2. patch attachment / skin / paint values into that config,
3. `DeepCopy` (`0x154B3A0`) into `APBWeapon +0x3E8`,
4. on first sight, call the native `OnRep_PartNetworkConfig` (`0x15FDA40`) so the game
   rebuilds the weapon through its own code,
5. wake the actor (`NetDormancy`, `FlushNetDormancy`), `MarkPropertyDirty`,
   `ForceNetUpdate`, and toggle `SetReplicates` to force a full re-send.

Step 3–5 only run when the config actually differs from what the actor already carries:
the two `TArray`s own different allocations, so the comparison is element-wise over the
slot array, the 0x20-byte part array and the three FName fields — a struct `memcmp` would
never converge. The join path forces one full push regardless.

An ornament `id` is one of two different things, chosen by its `type` field:
`PartOri` (or absent) is a paint theme written to `+0x18`; anything else
(e.g. `SMGTigerMZL`) is a part-skin mesh, and the **type** — not the id — is what
`WeaponPartSkinID` has to hold.

Weapon **suits** (the receiver/body cosmetic) are not a single field: the loadout's
`{WeaponSuit, WeaponSuitePainting}` pair is expanded per slot through
`WeaponSuitDefinitionDataTable` and `WeaponSuitPaintingDefinitionDataTable`, and the
results are fed into the per-part skin and paint fields above.

## Character cosmetics: `APBCharacter +0x698`

`FPBCharacterNetworkConfig` (0x28) holds `SkinClassArray` / `SkinIDArray` plus a
`SkinPaintingID` scalar. The arrays are cleared and rebuilt (not `CopyFrom`) and the
scalar is written unconditionally — `0` means "none", which is how a removed paint is
cleared. The config is then applied with `OnRep_CharacterSkinConfig()` (via
`ProcessEvent`) plus `MarkPropertyDirty` and `ForceNetUpdate`.

Role JSON → `EPBSkinClass`:

| Field | Class |
|---|---|
| `_skinToken` | `Skin` (1) — operator suit |
| `cosmetic10` | `Ornaments` (3) — head ornament |
| `cosmetic9` | `SpecialSlot` (4) — arm badge |
| `_ornamentId` | `SkinPaintingID` — suit paint |

## Caches and lifetime

Both cosmetics systems keep a `steamId → config` cache populated at join. `TickFlush`
also runs a sweep every ~2 s: it collects the Steam IDs of the live PlayerControllers and
drops cache entries that have been missing for two consecutive sweeps. That is
deliberately two sweeps — a player is briefly absent from the world during connect and
respawn, and a single-sweep rule would throw away a loadout that is still needed.

## Configuration

The metaserver address is the literal `127.0.0.1:8000` in `Server/LoadoutSync.cpp` and
in `Loadout/LoadoutFix.cpp` (two call sites). Making it configurable is the obvious next
step; it is left hard-coded here to keep this change reviewable.

## Known limitations

- `WO-*` weapon badges (`OrnamentID +0x20`) are not applied. The metaserver's loadout
  response does not currently expose `WeaponSkin.WeaponOrnament`, so there is nothing to
  write. Everything else cosmetic is implemented.
- Attachments are matched by part slot and replaced by the saved part id. The saved
  loadout is assumed to be internally consistent (a part that does not exist for that
  weapon resolves to no row and is skipped).
- Offsets are build-specific. A game update invalidates them.

## Testing

Run a dedicated server (`-server`), join with a Steam ID that has a saved loadout, and
check the log for `[LSYNC] ParseAndPush`, `[LSYNC] 192B: N inserted` and
`[B5-REAPPLY] Done: N weapons updated`. Two clients are worth testing: the 192 B map is
per-PlayerState, so the second player's weapons are the case that catches a missing
per-player insert. Joining and leaving before the fetch returns exercises the drop path.
