# IDA Decode Library

This directory is the durable decode library for native Boundary functions recovered in IDA.

The library exists because reverse-engineering results previously lived across chat history, temporary notes, raw documentation, and implementation comments. That preserved many conclusions but did not reliably preserve the native function identities that produced them.

## Rule: one native function, one file

A confirmed IDA function gets one file named after the IDA identity:

```text
Docs/IDA/
  sub_141234560.md
  sub_141ABCDEF.md
```

Do not create a `sub_xxx.md` until the native identity is actually recovered from IDA or from a historical artifact that explicitly recorded it. Never infer an address from behavior, an SDK symbol, or a later implementation.

The filename remains the stable native identity even after the function's semantic role becomes known.

## What belongs in a sub file

Each `sub_xxx.md` should preserve the recovered native material itself and enough human explanation to make it usable:

```markdown
# sub_141234560

## Identity

- VA: `0x141234560`
- RVA: `0x1234560`
- Binary/build: ...
- Recovery status: confirmed | partial

## What this function does

Human explanation of the recovered native behavior.

## IDA decompilation

\`\`\`c
<actual recovered IDA pseudocode>
\`\`\`

## Important observations

Human explanation of branches, state changes, invariants, or unusual behavior that matter to Project Rebound.

## Related code

- SDK symbol / field / generated declaration
- Project Rebound implementation that compensates for or relies on this behavior
- Caller/callee `sub_xxx` entries when confirmed

## Provenance

Where this decode came from: IDA database/build, original RE note, preserved conversation excerpt, or historical repository document.
```

The IDA pseudocode section is evidence, not a rewritten summary. If the original decompilation is unavailable, say that explicitly rather than reconstructing it from memory.

## Relationship to Vibefactory knowledge

Vibefactory's `.vf.md` pages are cognitive navigation. They explain a system at a useful abstraction level.

Fact/explanation pages may explain why a VF claim is justified and link here.

This directory is different: it preserves the actual recovered native functions that those explanations ultimately point to. There is no separate synthetic "FactStore" beneath it. The chain should terminate in real artifacts:

```text
VF-KB
  -> human explanation
      -> IDA decode / SDK declaration / actual Project Rebound source
```

## SideMountFix bootstrap recovery

The first recovery target is the SideMountFix / LauncherFix work.

### Historical material recovered

The following Project Rebound documents preserve the SideMountFix reasoning and verified behavior:

- [Launcher Fixing Docs](../Raw/Launcher%20Fixing%20Docs.md) — full research history, launcher state-machine diagnosis, dud-fire diagnosis, projectile VFX diagnosis, SDK offsets, and verification notes.
- [03 — Game Fixes](../ForAI/03-Game-Fixes.md) — compressed explanation of the `IsLocallyControlled()` failure pattern and the implemented repairs.
- [04 — Reverse Engineering Data](../ForAI/04-RE-Data.md) — relevant SDK field/structure offsets such as `APBLauncher`, `APBProjectile`, and `FPBFiringConfig::TimeCanRetriggerFire`.
- [Session 2026-05-15 to 18](../Raw/Session_2026-05-15_to_18_Compact.md) — later project-session consolidation of the same SideMountFix findings.

The repository history also contains the original documentation commit:

- `a373694f229944f90b37e3ced6ebc83da572882c` — `SideMounted Launcher fixing docs` (2026-05-02).

The archived Project Rebound conversation material supplied to Vibefactory was also searched for `PBLauncher`, `OnRep_PendingState`, `ServerFiring`, `TimeCanRetriggerFire`, `OnRep_Exploded`, `MulticastExplode`, and nearby `sub_14...` identities.

### What survived

The historical material preserves these reverse-engineering conclusions:

1. The launcher client can retain firing flags after a shot because the required state-transition/cleanup path is skipped when local-control checks fail on the dedicated-server client.
2. `OnRep_PendingState` is the replication point used by the fix to restore `CurrentState` and replay the missing state-specific behavior.
3. The native firing flow schedules a retrigger using `FPBFiringConfig::TimeCanRetriggerFire`; the observed value in the documented launcher configuration is 0.25 s. The retrigger can reach `ServerFiring` after the finite-ammo clip is already empty.
4. Projectile explosion visuals can be skipped by local-control-gated behavior; the implemented client repair responds to `OnRep_Exploded` and forces `MulticastExplode`.

These conclusions remain documented in the linked raw research and current implementation.

### What did not survive

No recovered SideMountFix artifact currently available here contains a trustworthy native `sub_14...` identity or RVA for:

- the native `APBLauncher` pending-state / `OnRep_PendingState` handling path;
- the native launcher firing/retrigger path responsible for `TimeCanRetriggerFire` and the later `ServerFiring`;
- the native projectile explosion path that gates `MulticastExplode`.

The original 2026-05-02 launcher document also does not contain those native addresses.

Some archived RE documents contain both launcher SDK offsets and unrelated `sub_1409...` identifiers. Inspection shows those subroutines belong to loadout/protobuf handling, not SideMountFix. They must not be imported into the SideMountFix evidence chain.

Therefore no SideMountFix `sub_xxx.md` file is created yet. Creating one now would fabricate an IDA identity.

### Native functions to recover next

When the relevant game build is opened in IDA again, recover these first:

1. `APBLauncher` native replicated pending-state handler / the native path corresponding to `OnRep_PendingState`.
2. Launcher native firing path that checks the firing flags and schedules the `TimeCanRetriggerFire` retrigger.
3. Native projectile explosion path that conditionally calls `MulticastExplode`.

As soon as an exact `sub_xxx` is recovered, add its own file here and move all function-specific IDA pseudocode into that file instead of another general-purpose document.
