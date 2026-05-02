# Launcher (Side-Mounted Pod) Fixing Documentation

## Overview

This document chronicles the complete journey of debugging and fixing the LeftLauncher/RightLauncher (side-mounted pod) system on a Dedicated Server built from a client-only game binary. The game (Project Boundary) was originally designed for ListenServer/P2P, and we forced it into Dedicated Server mode via DLL injection and hooks.

**Core file modified**: `Payload/Hooks/Hooks.cpp`

**Total lines added**: ~120 (across server and client ProcessEvent hooks)

---

## Phase 0: Problem Description

### Symptoms (Before Any Fix)
- LeftLauncher and RightLauncher fire the first shot correctly (server processes it)
- After the first shot, a reload/cooldown bar appears on the client UI and completes
- After the bar completes, no ammo is refilled, and subsequent shots are NOT processed by the server
- The client renders projectile visuals locally, but the server never acknowledges them
- Grappling hook has the EXACT same symptom (first use works, subsequent uses don't)
- Regular handheld weapons work perfectly (reload, fire, everything)

### Ammo Display
- Initial ammo shows as 24/48 (clip/total) or 1/3 (single-shot launchers)
- Firing correctly decrements the clip count
- Reload bar appears when clip hits 0
- After reload bar completes: ammo display does NOT update

---

## Phase 1: First Theories (All Wrong)

### Theory 1: bReplicates=false breaks RPC delivery
**Assumption**: Launcher actors have `bReplicates = false` from the Blueprint CDO, so they never get a `UActorChannel`. Without a channel, `ServerFiring` RPC cannot reach the server.

**Attempt**: Set `bReplicates = true` and `RemoteRole = ROLE_SimulatedProxy` on launcher actors in `TickFlushHook`.

**Result**: **Regression**. Even the FIRST shot stopped working. Setting `bReplicates` on the launcher interfered with the existing RPC path. Reverted immediately.

### Theory 2: Manual ammo refill
**Attempt**: In `TickFlushHook`, detect `Magazine.AmmoInClip == 0 && TotalAmmo > 0` and call `APBLauncher::OnRefill()` or directly manipulate the Magazine struct.

**Result**: `[LAUNCHER-RELOAD]` log NEVER appeared. The Tick-based approach couldn't detect the ammo state because the Tick iterates `CurrentLeftLauncher`/`CurrentRightLauncher` pointers which may point to different objects than the ones actually firing.

### Theory 3: Failed reload RPCs corrupt networking state
**Attempt**: Block `ServerOnStartReload`/`ServerOnRefill`/`ServerOnReoload` RPCs on the client side to prevent them from corrupting the launcher's networking state.

**Result**: **Broke the cooldown UI**. The reload RPCs are needed for the client's visual cooldown bar. Blocking them prevented the UI from functioning. Reverted.

---

## Phase 2: Log-Driven Discovery

### Adding Comprehensive ProcessEvent Logging

Server-side (`ProcessEventHook`):
```cpp
if (Object && Object->IsA(APBLauncher::StaticClass())) {
    std::cout << "[POD] " << EventInfo.FullName << std::endl;
    if (EventInfo.FullName.contains("ServerFiring")) {
        // Log AmmoInClip, TotalAmmo, bIsFiring, bPendingFiring, etc.
    }
}
```

Client-side (`ProcessEventHookClient`):
```cpp
if (Object && Object->IsA(APBLauncher::StaticClass())) {
    ClientLog("[POD-CLIENT] " + EventInfo.FullName);
}
```

### Critical Discovery #1: ServerFiring DOES reach the server

Server log showed the COMPLETE fire sequence arriving:
```
[POD] ServerOnPressed
[POD] K2_Deploying
[POD] ServerOnReleased
[POD] ServerFiring          ← RPC IS working!
[POD] K2_Fired
[POD] K2_SimuilateFire
[POD] K2_Ready
[POD] K2_Undeploying
[POD] K2_Standby
```

**Conclusion**: RPC delivery is NOT the problem. `ServerFiring`, `ServerOnPressed`, `ServerOnReleased` all reach the server reliably. The launcher has a functioning RPC channel.

### Critical Discovery #2: ServerFiring stops after 2 shots (one per launcher)

After the first shot on each launcher (left=smoke, right=impulse), ServerFiring is no longer called by the client. The server processes K2_Fired/K2_SimuilateFire (BP events triggered by the server's own state machine), but ServerFiring is absent.

### Critical Discovery #3: Client-side flags stuck at 1

With extended logging at `K2_Ready` time (the last checkpoint before ServerFiring should be called):

| Variable | Fire 1 & 2 (works) | Fire 3+ (broken) |
|----------|:---:|:---:|
| `AmmoInClip` | 1 | 1 |
| `HasAmmoInClip()` | 1 | 1 |
| `CurrentState` | 3 (Ready) | 3 (Ready) |
| `bIsFiring` | **0** | **1** |
| `bPendingFiring` | **0** | **1** |
| `BurstCtr` | **0** | **1** |
| `bFireCtrl` | **0** | **1** |

**Root cause identified**: `bIsFiring`, `bPendingFiring`, `BurstCtr`, `bFireCtrl` are set to 1 by the client's local fire flow during the first shot, but NEVER cleared back to 0. The Native code checks these flags before calling `ServerFiring` and sees them stuck at 1 → refuses to fire.

### Critical Discovery #4: Server-side flags are always 0

Server-side `[POD-STANDBY]` logs showed that the server always has these flags at 0. The serviceNEVER sets them. These are PURELY client-side flags — the client BP code sets them during fire and the client BP code is supposed to clear them when the fire completes. But the clearing code is gated behind `IsLocallyControlled()`.

---

## Phase 3: Blueprint Decompilation Analysis

### FModel Export

Exported the entire game's Blueprint folder from FModel with "Deserialize" option enabled, producing pseudo-C++ files showing CDO (Class Default Object) properties and Ubergraph function bytecode.

Key files analyzed:
- `PBLauncher_Delay_BP.cpp` — base Delay launcher BP
- `PBLauncher_Deploy_BP.cpp` — base Deploy launcher BP
- `PBLauncher_Direct_BP.cpp` — base Direct launcher BP
- `PBCharacter_BP.cpp` — Character BP (8189 lines)
- `APBSmokeGrenade_BP_squid.cpp` — Smoke grenade projectile
- `PBEMPGrenade_BP.cpp` — EMP grenade projectile
- `PBGrenade_CQB_BP.cpp` — HE grenade projectile
- `BP_PBImpulseGrenade.cpp` — Impulse grenade projectile
- `UMG_InGameHUD_LeftPod.cpp` — Left pod HUD widget

### Confirmed Pattern

Every `K2_` state transition function in the Delay BP has this pattern:

```cpp
// K2_Undeploying (PBLauncher_Delay_BP.cpp line 80-83)
IsLocallyControlled = GetOwnerPawn->IsLocallyControlled();
if (!IsLocallyControlled)
    return;  // ← Skips ALL animation/sound/cleanup logic!
```

Same pattern found in: `K2_Deploying`, `K2_Ready`, `K2_Fired`, `K2_SimuilateFire`, `K2_Standby`.

### The `Fired()` Function Logic

```cpp
public void Fired() {
    IsLocallyControlled = OwningCharacter->IsLocallyControlled();
    if (!IsLocallyControlled) goto Label_407;

    // Label_215: Plays 1P fire sound + self voice
    // (only when IsLocallyControlled = true)

    Label_407:
    HasAuthority = UActor::HasAuthority();
    if (!HasAuthority) goto Label_442;
    return;  // Server does nothing here!

    Label_442:
    // Remote client: plays 3P fire sound
}
```

### The Core Mechanism

In a ListenServer:
1. Client BP sets `bIsFiring=1` during fire flow
2. Client calls `ServerFiring` RPC → server processes
3. Server replicates state back via `OnRep_PendingState=0` (Standby)
4. Native `OnRep_PendingState` handler: `IsLocallyControlled()=true` → calls `K2_Standby` → BP `K2_Standby` clears `bIsFiring=0`
5. Next fire: `bIsFiring=0` → passes check → `ServerFiring` called

In Dedicated Server:
1. Client BP sets `bIsFiring=1` during fire flow
2. Client calls `ServerFiring` RPC → server processes
3. Server replicates state back via `OnRep_PendingState=0`
4. Native `OnRep_PendingState` handler: `IsLocallyControlled()=false` → **does nothing**
5. `bIsFiring` stays 1 forever
6. Next fire: `bIsFiring=1` → check fails → `ServerFiring` never called

---

## Phase 4: State Machine Fix

### The Fix (Client-side ProcessEventHook)

When `OnRep_PendingState` fires and the launcher state changes, we:
1. Force `CurrentState = PendingState` (unstuck the state machine)
2. Call the corresponding `K2_` BP function for the new state
3. Clear fire flags at Standby(0) and Ready(3)

```cpp
if (EventInfo.FullName.contains("OnRep_PendingState")) {
    uint8 pending = static_cast<uint8>(Pod->PendingState);
    uint8 current = static_cast<uint8>(Pod->CurrentState);

    if (pending != current) {
        Pod->CurrentState = static_cast<EPBLauncherState>(pending);
    }

    switch (pending) {
    case 0: // Standby — fire cycle complete, clear all fire flags
        Pod->bIsFiring = false;
        Pod->bPendingFiring = false;
        Pod->BurstCounter = 0;
        Pod->bIsFireControlEnabled = false;
        Pod->K2_Standby();
        break;
    case 1: Pod->K2_Deploying();   break;
    case 2: Pod->K2_Undeploying(); break;
    case 3: // Ready — ensure clean state before allowing fire
        Pod->bIsFiring = false;
        Pod->bPendingFiring = false;
        Pod->K2_Ready();
        break;
    case 4: Pod->K2_Reloading();   break;
    case 5: Pod->K2_Handup();      break;
    }
}
```

**Effect**: All launcher animations now play correctly (deploy, fire, undeploy, reload progress bar). The reload progress bar that was previously missing now appears because `K2_Reloading()` is properly called.

---

## Phase 5: ServerFiring Force-Call (Later Removed)

Initially, we added a force-call for `ServerFiring` when `K2_Ready` fired with ammo available, computing Origin/ShootDir from the PlayerController's control rotation (because `GetAdjustedAim` returns garbage values when `IsLocallyControlled=false`).

This was later **removed** because after the state machine fix, the BP's own async `ServerFiring` call works correctly for all shots. The force-call was creating duplicate projectiles.

---

## Phase 6: The Async BP Timer Duplicate (Dud Projectile)

### Problem

After the state machine fix, ALL shots fired correctly. But the server log showed two `ServerFiring` calls per shot:
```
[POD-FIRE] AmmoInClip=1 TotalAmmo=3 State=1  ← First call (correct)
[POD-FIRE] AmmoInClip=0 TotalAmmo=2 State=3  ← Second call (dud!)
```

The second call came from the BP's async timer (`FireConfig.TimeCanRetriggerFire=0.25s`). The native fire code starts a timer after the first `ServerFiring`, and the timer calls `ServerFiring` again after 0.25 seconds. By then, the ammo is already consumed → the second call creates a dud projectile with `AmmoInClip=0`.

In ListenServer, the async call is harmless (same process, ammo state is consistent). In Dedicated Server, the dud arrives after the real projectile, and depending on network timing, the dud's replication data (with `bExploded=1`, `Vel=0`, `LifeSpan=0`) can overwrite the real projectile's data on the client.

This caused **~1/3 of shots to have no visible projectile** on the client.

### The Fix

Block `ServerFiring` when `AmmoInClip == 0` on both client and server:

**Client side** (prevents sending):
```cpp
else if (EventInfo.FullName.contains("ServerFiring")) {
    if (!Pod->HasInfiniteAmmo() && Pod->Magazine.AmmoInClip == 0) {
        ClientLog("[POD-CLIENT-FIRE] BLOCKED — empty clip");
        return;
    }
    // ... normal fire log
}
```

**Server side** (prevents processing):
```cpp
if (EventInfo.FullName.contains("ServerFiring")) {
    // ... log
    if (!Pod->HasInfiniteAmmo() && Pod->Magazine.AmmoInClip == 0) {
        std::cout << "[POD-FIRE] BLOCKED — empty clip" << std::endl;
        return;
    }
}
```

---

## Phase 7: Projectile Visual Effects Fix

### Problem

After fixing the launcher state machine and dud projectiles, projectiles were visible and traveled correctly, but explosion effects (particles, impact templates, sounds) did NOT appear on the client for EMP, Impulse, and Smoke grenades. HE grenade effects worked inconsistently (~sometimes appeared).

### Root Cause

`MulticastExplode` — the multicast RPC that triggers visual effects on all clients when a projectile explodes — was NOT being called. The native code that calls `MulticastExplode` on the server has an `IsLocallyControlled()` check that prevents it from executing on Dedicated Server.

Even though the projectile state replicated to the client (`bExploded=1` via `OnRep_Exploded`), the BP `K2_Explode` function (which spawns particles, impact templates, and sounds) was never triggered because:
1. Native explosion code: `if (IsLocallyControlled()) { MulticastExplode(...); }` → NOT called on server
2. `OnRep_Exploded` fires but BP handler has `IsLocallyControlled` check → does nothing on client

### BP Analysis Results

**Smoke Grenade** (`APBSmokeGrenade_BP_squid.cpp`):
```cpp
SmokeGrenadeData = {
    Speed: 2000,
    ExplosionCountDown: 0.1,   // 0.1 second fuse!
    EffectRadius: 400,
    MaxBoundCount: 0,          // No bouncing
};
```
- 0.1s fuse → detonates almost immediately
- Spawns `SmokeVolumeClass` actor for smoke visual
- Has `Sphere` component visibility controlled by `AmIOwner?` check in `ReceiveTick`
- `AmIOwner?` compares `OwnerPlayerState` (Net+RepNotify) with local PlayerState

**EMP Grenade** (`PBEMPGrenade_BP.cpp`):
```cpp
EMPGrenadeData = {
    ImpactTemplate: PBImpactEMP_BP_C,  // Explosion effect
    DetonatorRadius: 350,
    MaxBoundCount: 1,
    ExplosionCountDownAfterBounce: 0.6
};
```
- Detonates on proximity (350 units) or 0.6s after bounce
- `ImpactTemplate` spawns explosion visual
- `ExplodeParticleFX = EMP_Explode` particle

**HE Grenade** (`PBGrenade_CQB_BP.cpp`):
```cpp
GrenadeData = {
    LifeTime: 4,              // 4 second fuse
    ImpactTemplate: PBImpactGrenade_BP_C,
    DetonatorRadius: 350
};
```
- 4s fuse — plenty of time for replication to complete
- This is why HE "sometimes" worked — the long fuse gives the client more chances to receive proper state

**Impulse Grenade** (`BP_PBImpulseGrenade.cpp`):
```cpp
ImpulseGrenadeData = {
    LifeTime: 0.1,            // 0.1s fuse!
    InnerRadius: 500,
    OuterRadius: 1000,
    InnerImpulseValue: 2500   // Extremely high impulse
};
```
- K2_Explode override spawns `NS_Smoke_ImpulseGrenade` Niagara effect
- `NotifyOverlapCharacter` forces characters into `EnterRagdollState(0.5)` — this is the push effect
- Push effect works on server-side, but Niagara smoke is server-only (no rendering on DS)

### The Fix

When `OnRep_Exploded` fires on the client and `bExploded == 1`, force-call `MulticastExplode` to trigger the full visual effects locally:

```cpp
if (Object && Object->IsA(APBProjectile::StaticClass())) {
    auto* P = static_cast<APBProjectile*>(Object);
    // ... diagnostic logging ...

    if (EventInfo.FullName.contains("OnRep_Exploded") && P->bExploded) {
        ClientLog("[PROJ-CLIENT-FIX] Forcing MulticastExplode");
        FHitResult DummyHit{};
        P->MulticastExplode(DummyHit);
    }
}
```

**Effect**: All projectile types now display their explosion effects correctly on the client — HE explosions, EMP blasts, Impulse smoke, and Smoke volume clouds.

---

## Appendix A: Key SDK Classes and Offsets

### APBLauncher (base class for all side-mounted pods)

| Member | Offset | Type | Notes |
|--------|--------|------|-------|
| `CurrentState` | 0x02C1 | `EPBLauncherState` | Local state |
| `PendingState` | 0x02D4 | `EPBLauncherState` | **Net, RepNotify** |
| `ReloadingDelay` | 0x0338 | float | |
| `ReloadingDuration` | 0x033C | float | |
| `FireConfig` | 0x0370 | `FPBFiringConfig` | Burst/fire timing |
| `BurstCounter` | 0x03B8 | int32 | **Net, RepNotify** |
| `bIsFiring` | 0x03BC bit 0 | bool | **Net, RepNotify** |
| `bPendingFiring` | 0x03BC bit 1 | bool | **Net, RepNotify** |
| `bIsFireControlEnabled` | 0x0410 bit 0 | bool | |
| `bInProjectileControlMode` | 0x0428 bit 0 | bool | **Net, RepNotify** |
| `bInSpecialMode` | 0x0440 bit 0 | bool | |
| `MagazineConfig` | 0x0454 | `FPBMagazineConfig` | |
| `Magazine` | 0x0468 | `FPBMagazine` | **Net, RepNotify** |
| `bInForceState` | 0x0498 bit 0 | bool | **Net** |
| `FireComponent` | 0x0278 | `UPBFireComponent*` | |

### APBCharacter

| Member | Offset | Notes |
|--------|--------|-------|
| `CurrentLeftLauncher` | 0x1F50 | **Net, RepNotify** |
| `CurrentRightLauncher` | 0x1F58 | **Net, RepNotify** |

### EPBLauncherState

| Value | Name |
|:-----:|------|
| 0 | Standby |
| 1 | Deploying |
| 2 | Undeploying |
| 3 | Ready |
| 4 | Reloading |
| 5 | Handup |

### APBProjectile

| Member | Offset | Type | Notes |
|--------|--------|------|-------|
| `MovementComp` | 0x0250 | `UProjectileMovementComponent*` | |
| `CollisionComp` | 0x0258 | `USphereComponent*` | |
| `ParticleComp` | 0x0260 | `UParticleSystemComponent*` | |
| `bExploded` | 0x0620 | bool | **Net, RepNotify** |
| `bIsDisabled` | 0x0244 bit 0 | bool | **Net, RepNotify** |
| `OwnerPlayerState` | 0x0638 | `APBPlayerState*` | **Net, RepNotify** |
| `ImpulseScale` | 0x062C | float | |

### UProjectileMovementComponent

| Member | Offset | Notes |
|--------|--------|-------|
| `Velocity` | 0x00C4 | Inherited from UMovementComponent |
| `InitialSpeed` | 0x00F0 | |
| `MaxSpeed` | 0x00F4 | |
| `bInitialVelocityInLocalSpace` | 0x00F8 bit 3 | |

### FPBMagazine

| Member | Offset |
|--------|--------|
| `Config` | 0x00 |
| `AmmoInClip` | 0x0C |
| `TotalAmmo` | 0x10 |
| `AmmoInMagazine` | 0x14 |
| `MagazineCapacity` | 0x18 |

### FPBFiringConfig

| Member | Offset |
|--------|--------|
| `TimeBetweenFire` | 0x00 |
| `TimeBetweenBurst` | 0x04 |
| `TimeCanRetriggerFire` | 0x08 |
| `PostFireDuration` | 0x0C |
| `BurstCount` | 0x10 |
| `bEnableBurst` | 0x14 bit 0 |
| `bEnableAutoFire` | 0x14 bit 1 |

---

## Appendix B: All Launcher BP Configurations

| Launcher | InitAmmo | InitClips | BurstCount | bEnableBurst | bEnableAuto | Reload(s) |
|----------|:--:|:--:|:--:|:----:|:----:|:--:|
| AutoCanon | 15 | ? | 5 | true | — | 15 |
| Auto Missile | 25 | 1 | 5 | true | — | — |
| MiniGun | 100 | 3 | — | — | true | 4.2 |
| MiniGun+Shield | 200 | 1 | — | — | — | — |
| EMP Auto | 6 | ? | 3 | true | — | 6 |
| EMP Delay INF | 3 | 11 | — | — | — | — |
| Grenade Auto | 6 | ? | 3 | — | true | 18 |
| CQB (HE) | 1 | 3 | 3 | — | — | 6 |
| Smoke Squid | 1 | 3 | 3 | — | — | 12 |
| Impulse | 1 | 2 | 3 | — | — | 9 |
| Deployable ADS | 1 | 2 | 3 | — | — | 15 |

---

## Appendix C: All Projectile BP Configurations

| Projectile | Speed | Fuse | Damage | Special |
|------------|:-----:|------|:------:|--------|
| HE (CQB) | 1000 | 4s LifeTime | 120 base | Proximity detonator 350 |
| EMP | 2000 | 0.6s after bounce | 200 base | MaxBound=1, Detonator=350 |
| Impulse | 1500 | 0.1s LifeTime | — | Impulse 2500, MaxBounce=2 |
| Smoke (squid) | 2000 | 0.1s ExplosionCountDown | — | MaxBound=0, EffectRadius=400 |
| Smoke (normal) | 2000 | 2s ExplosionCountDown | — | EffectRadius varies |
| Auto Grenade | ? | ? | 50 base | |
| Longrange | ? | ? | 70 base | |
| Trap | ? | ? | 500 base | |
| Missile | ? | 30s EngineOnLifeTime | 35-50 base | |

---

## Appendix D: Lesson Learned — The IsLocallyControlled Pattern

The fundamental issue across ALL fixes was the same pattern:

**In ListenServer**, `IsLocallyControlled()` returns `true` for the hosting player. All BP state transitions, flag clearing, visual effects, and RPC calls execute normally.

**In Dedicated Server**, `IsLocallyControlled()` returns `false` for ALL clients (the server is not a locally controlled player). Every BP function gated behind this check is silently skipped. This includes:

- State transitions (K2_Standby, K2_Undeploying, etc.)
- Flag clearing (bIsFiring=0, etc.)
- Visual effects (MulticastExplode, K2_Explode)
- HUD updates (K2_RefreshPodReloadInfo)
- Sound effects (Fired function's 1P branch)

The fix pattern in every case:
1. Identify the ProcessEvent that fires but doesn't execute its intended effect
2. Add a hook that detects the event and manually triggers the skipped logic
3. If native code has the IsLocallyControlled check, call the BP function directly from the hook

This pattern likely applies to ANY other system in the game that doesn't work correctly on Dedicated Server (weapon residue on pickup, grappling hook, etc.).

---

## Appendix E: Final Code Structure (Hooks.cpp)

### Server-Side ProcessEventHook Additions

1. Launcher diagnostics logging (`[POD]`, `[POD-FIRE]`, `[POD-STANDBY]`)
2. Dud ServerFiring blocking (`AmmoInClip == 0` check)

### Client-Side ProcessEventHookClient Additions

1. **OnRep_PendingState handler**: CurrentState fix + K2_ function calls + flag clearing
2. **ServerFiring handler**: Dud blocking (`AmmoInClip == 0`) + diagnostic logging
3. **K2_Ready handler**: Diagnostic logging with full state dump
4. **OnRep_Magazine/OnRep_SavedData handlers**: Diagnostic logging
5. **APBProjectile handler**: Diagnostic logging + `OnRep_Exploded` → `MulticastExplode` force-call

### Infrastructure

- `int DefaultMatchStartCountdown = 2;` — debug countdown override
- `Batch.ActorInfos.push_back(...)` — VS2026 bit-field compilation fix (later superseded by upstream)

---

## Appendix F: Test Verification Checklist

- [x] First launcher shot works on server
- [x] Subsequent shots work (no ServerFiring dropout)
- [x] Launcher deploy/fire/undeploy animations play on client
- [x] Reload progress bar appears between shots
- [x] No dud projectiles (empty-clip ServerFiring blocked)
- [x] HE grenade: projectile visible, travels, explodes with effects
- [x] EMP grenade: projectile visible, travels, explodes with effects
- [x] Impulse grenade: projectile visible, travels, ragdoll push works, smoke appears
- [x] Smoke grenade: deploys smoke volume on detonation
- [x] No server crash on last shot (ammo=0 handled gracefully)
- [x] All ammo types deplete correctly (no double-decrement from manual refill)
- [ ] Grappling hook (same pattern, not yet fixed)
- [ ] Weapon residue on pickup (ActorChannel close commented out in LibReplicate)
