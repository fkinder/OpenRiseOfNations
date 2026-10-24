# Combat: target acquisition, attack orders, firing, damage, death

Status: ✅ the whole chain from an idle unit noticing an enemy to the enemy's corpse expiring is specified from the
decompiles **and** checked against the disassembly where the decompiler was unreliable (argument lists, signedness,
float order, the hidden `| GUN` in `get_damage`) · 🟡 a few branches from the decompile only (marked inline: `fight`'s
chase/give-up, corpse lifetimes) · ❓ open items in §11, chiefly the art-derived data
(release-event frames, animation lengths, muzzle offsets) and the spline math of rockets and missiles. Nothing is
validated against a running game yet (no replay with combat exists on this Mac).

Deliverables: this document; `re/tools/combat-ref.mjs` (damage formula, `do_damage` scaling, `take_damage`
accumulation and the flank/entrenchment angle tests, reading the rules from `~/Games/RiseOfNations`:
`node re/tools/combat-ref.mjs --examples`, `--selftest`, `--dump out.tsv`, or `<attacker> <defender> [situation
flags]`). Decompiled C
and disassembly stay local (gitignored): `re/decomp/combat/` (core, job `re/jobs/dc_combat1.txt`, types
`types_combat*.txt`), `re/decomp/combat_A/` (acquisition, `dc_combat_A_*`), `combat_B/` (orders, `dc_combat_B_*`),
`combat_C/` (firing, `dc_combat_C_*`), `combat_D/` (damage and death, `dc_combat_D*`), plus the older `atk/`,
`atk2/`, `life2/`, `halt/`, `move*/`.

Read first: [orders-spec.md](orders-spec.md) (order list, `work`, `do_job`), [movement-spec.md](movement-spec.md)
(coordinates, `vector_dist`, `find_angle`, `sin`, guys, PathFinder), [frame-spec.md](frame-spec.md) (frame order,
object lifecycle), [rules-spec.md](rules-spec.md) §6–7 (type fields) and §11 (the combat balance table, which this
document builds on). Corrections to those specs found here are listed in §3.13 and §11.

## 0. Conventions

- Addresses are `riseofnations.exe` VAs, names are PDB names. Pseudocode is paraphrased, never the decompiler text.
- Integer arithmetic is int32 with C truncating division unless noted (the binary's magic-number multiplies and
  add-sign-then-shift sequences are exact equivalents). `trunc256(x)` = `x / 256` truncated toward zero (the binary's
  `(x + ((x >> 31) & 0xff)) >> 8`).
- "rnd(n)" = `game_random.get(0, 0xFFFF) % n` (`GameAccess::rnd` @43cca0 is exactly this). There is one simulation
  stream (replay-spec §12); `internal_random` (particles, AmmoOut, underlays, sounds) is presentation only.
- Object virtual slots: `+0x18 is_unit`, `+0x1c is_wall` (all buildings), `+0x20 is_build`, `+0x48 is_seen`,
  `+0x4c is_active`, `+0xb8 is(type, strict)`, `+0xe4 get_captain`, `+0xe8 is_captain`, `+0xf4 get_combat_stance`,
  `+0x11c hits`, `+0x120 attack`, `+0x124 armor`, `+0x12c min_range`, `+0x130 max_range`, `+0x134 recharge`,
  `+0x138 valid_target_const`, `+0x148 has_objmask`, `+0x150 close`, `+0x158 die`, `+0x168 repair_damage`,
  `+0x16c take_damage`; several "cast to building/wall" slots are folded `return this` bodies (0x3c/0x40,
  0xac/0xb0). Type slots: `+0x60 is`, `+0x7c get_score_value`, `+0x80 get_kill_value`, `+0xec get_age`,
  `+0xf0 has_objmask`, `+0x10c is_siege`, `+0x110 is_tank`, `+0x114 is_missile`, `+0x130 is_caravan`,
  `+0x134 has_stance_type`, `+0x138 get_stance_type`. Identical-COMDAT folding: `@41bff0` = `return 0`,
  `@41e0e0` = `return 1`, `@41c000` = `return this`.
- LeaderData offsets are given as LeaderData offsets (the decompiler's `leaders.list[i].field_0xNNNN` is the same
  offset; `Leader` = LeaderData + 8 trailing bytes). Rare resources are `LeaderData.rare` (+0x6d98) /
  `rare_conquest` (+0x6dc0), bit `good − 6` (e.g. Obsidian 32 → bit 26). `num_units[t − 50]` is +0x5762,
  `num_queued[t]` +0x5a22, the XOR-encoded `LeaderDataEncrypt` (`ages ^ 0x62766`, resources `^ 0x8221`) via +0x6eb8.
- Stances (`CombatStanceIndex`): 0 AGGRESSIVE, 1 DEFENSIVE, 2 STAND_GROUND, 3 RAID, 4 RAZE, 5 HOLD_FIRE. Death types
  (`DeathIndex`): 1 STABBED, 2 SHOT, 3 SPLODED, 4 SCORCH (5 CAPTURED).

## 1. The combat chain in one frame

```
Game::do_frame
 ├─ Objects::process_all (units: players (frame+k)%10, slot order; then buildings)
 │   ├─ Unit::process: recharging--, healing--, …; every 16 frames targeted/=4; every 32 process_attrition (§7)
 │   │   └─ Unit::work → do_job
 │   │       ├─ NONE  → do_idle → think → think_attack → find_melee_target → find_nearby_target (§2) → add_attack_order (§3)
 │   │       ├─ ATTACK_TO / GROUP_ATTACK_TO → do_move + every 15 frames find_melee_target (§3.4)
 │   │       └─ ATTACK → do_attack (§3.5) → fight (§4.3)
 │   │             ├─ out of range: find_attack_pos (RNG for building targets) → add_move_order(QUEUE_FIRST)
 │   │             └─ in range, recharged: face, set_anim(ATTACK1..3) (RNG per guy), then
 │   │                   melee: do_damage per guy (§5.3) → take_damage (§5.4) → die/close (§6, RNG)
 │   │                   direct-fire ranged: fire_ammo → add_ammo → Ammo::init (RNG)       recharging = recharge
 │   └─ Build::process → Build::do_attack → find_target (§2.16) → fire_ammo (RNG per arrow) (§4.13)
 └─ Objects::inc_time
     ├─ units: Guy::inc_time (animation clock) then execute_events: RELEASEEVENT at its frame → add_ammo → Ammo::init (§4.5)
     ├─ buildings: Wall::inc_time (air-defence turret events)
     ├─ every live Ammo in slot order: Ammo::inc_time → arrival → hit tests → Ammo::do_damage → Object::do_damage (§4.9–4.11)
     └─ every DeathObj: inc_time (corpse lifetime, holds the dead slot) (§6.3)
```

Most ranged units therefore shoot from the **animation event** (art data), and the projectile lands in a later
`Objects::inc_time` ammo pass, in ammo-slot order; melee damage is applied immediately inside `fight`.

## 2. Target acquisition

Scope: how units and buildings pick what to attack without (or in addition to) a player order: the idle `think`
path, the per-frame/periodic re-acquisition paths (`find_melee_target` from `do_attack_to`, `fight`, `do_move` …),
the "I was hit" response (`target_opportunity`), group target sharing, building auto-fire target choice, and the
spatial queries underneath. Damage numbers (`ObjectData::get_damage`), the stat getters (`max_range`, `attack`, …)
and `fight`/`do_attack` themselves are in §5.1–5.2, §4.3 and §3.5; they are named here with their arguments only.

Confidence: ✅ read from decompile **and** checked against the disassembly; 🟡 from the decompile only or partly
inferred; ❓ unknown. Evidence lives in `re/decomp/combat/`, `re/decomp/atk/` and (new for this section)
`re/decomp/combat_A/` (+ `re/decomp/combat_A/asm/` disassembly), job files `re/jobs/dc_combat_A_*.txt`.

### 2.0 Conventions used in this section

- `S` = the searching object (`this`), `T`/`C` = the candidate target `objects[whom][ox]`. Units live in the same
  per-leader lists as objects (`units.lists[who][o]` aliases `objects.lists[who][o]`).
- Object flags (`SubObject.flags` +8, frame-spec.md §0): VALID 1, STARTED 2, ACTIVE 4, IDLING 8, NEW_THINK 0x10,
  CITY 0x20, DETECTOR 0x40, ATTACKING 0x80.
- Object virtual slots used below (Unit / Build bodies from `re/analysis/vt_objects.txt`):

  | slot | name used here | Unit | Build/Wall |
  |---|---|---|---|
  | +0x08 | `is_live_unit` (PDB alias `is_valid_unit`) | `flags & 1` | `return 0` |
  | +0x0c | `is_live_build` | `return 0` | `flags & 1` |
  | +0x18 / +0x1c / +0x20 | `is_unit` / `is_wall` / `is_build` | 1/0/0 | 0/1/1 |
  | +0x2c | `is_wonder` | 0 | `BuildData::is_wonder` |
  | +0x4c | `is_active` | `flags & 1` | `flags & 4` (finished) |
  | +0x48 | `is_seen(who, p2)` | `UnitData::is_seen` @607a60 | `BuildData::is_seen` @62e1a0 |
  | +0xac / +0xb0 | `get_build` / `get_wall` | 0 | `this` |
  | +0xbc | `is_on_map` | `inside_up < 0` | 1 |
  | +0xc0 | `is_plane` | `UnitData::is_plane` | 0 |
  | +0xcc / +0xd0 / +0xd8 | `is_supply` / `is_caravan` / `is_moving` | flags2&0x40 / flags2&8 / front order `is_move` | 0 |
  | +0xe4 / +0xe8 | `get_captain` / `is_captain` | o_up chain / `o_up < 0` | self / 1 |
  | +0xf4 | `get_combat_stance` | @610a30 | @62e0f0 |
  | +0x100 | `get_packer_stance` | @610970 | – |
  | +0x114 / +0x118 / +0x11c / +0x120 | `hits_left` / `health_level` / `hits` / `attack` | | |
  | +0x12c / +0x130 | `min_range` / `max_range` (tiles) | | |
  | +0x138 | `valid_target_const` @6472c0 | | |
  | +0x148 | `has_objmask(m)` | `ptype.obj_masks & m` | `BuildData::has_objmask` @62e210 (city/age tweaks) |
  | +0x17c | `get_speed(x, y, 0)` | @608720 | – |
  | +0x1b8 | `get_garrison_arrows` | – | @62e3d0 |

- Type virtual slots: +0x60 `is(t, strict)` (rules-spec §7.5), +0xd0 `Type::sum_rules_cost` @6680e0 (no override in
  UnitType/BuildType), +0xf0 `has_objmask`, +0x108 `BuildTypeData::is_dock` (units: 0), +0x10c `UnitTypeData::is_siege`
  (`unit_flags & 0x20000`, buildings 0), +0x110 `UnitTypeData::is_tank` (`unit_flags & 0x80000`), +0x130 `is_caravan`,
  +0x134 `has_stance_type(st)` = `get_stance_type() == st`, +0x138 `get_stance_type` (§2.12).
- Order accessors (vslots, `re/analysis/vt_orders.txt`): +0x10 `get_type`, +0x14 `is_move`, +0x18 `is_attack`,
  +0x1c `is_move_attack`, +0x2c `is_group`, +0x34 `is_patrol`, +0x50 `update_attack_order`, +0xcc `get_attack_order`,
  +0xec `get_guard_order` (guard_x/guard_y at +0x1c/+0x20), +0xf4 `get_cast_order` (spell at +0x20), +0x94
  `update_group_order`. `get_activity` @608370 = the first order (from the front) that is not `is_move_attack`, or an
  ATTACK_TO / GROUP_ATTACK_TO; `update_action`/`get_action` = orders-spec.md §4.4.
- "every N frames, staggered" = `(o + frame) % N == 0` (all such tests here are on non-negative values). "tile" =
  TCoord (192 world units), "W-cell" = WCoord (768), "fog cell" = 384 (`x>>7` then /3), "U-cell" = 48.
- Divisions are C `int32` divisions (truncate toward zero); the binary uses magic-number multiplies that are exactly
  equivalent, checked in the disassembly (`/2`, `/4` via add-sign-then-shift, `/5`, `/10`, `/20`, `/25`, `/400`,
  `/10000`).
- `TypeIndex` names used: 50/51 peasant types (`is_peasant`), 50..53 workers (`is_worker`), 58 SPY, 61 MERCHANT,
  62 MERCHANTDUTCH, 68 PARTISAN, 73 COMMANDO, 123 MACHINEGUN, 132 HOPLITES, 304 BOMBER, 306 STEALTHBOMBER,
  317 FISHERMEN, 323 BARK, 336 FIRESHIP, 340 TRIREME, 351 AIRCRAFTCARRIER, 400 FURTRAPPER, 414 VILLAGE (city),
  439 TOWER, 443 FORTX, 447 AIRBASE, 520 MISSILESILO, 521 LOOKOUT, 522 OBSERVATIONPOST; spells 629 BRIBE,
  640 SABOTAGE, 652 UNPACK.

### 2.1 Entry points (who searches, when) ✅

| Caller | Call | When |
|---|---|---|
| `Unit::do_idle` → `Unit::think` @5f6e40 | `think_attack` @5f5a80 → `find_melee_target(-1, &w, 0, 1, 0)` | idle units, gated (§2.2) |
| `Unit::do_attack_to` @5f2320, `do_group_attack_to` @5e74e0 | `find_melee_target(-1, NULL, 0, 1, 0)` | attack-move, every 15 frames staggered (orders-spec §6) |
| `Unit::fight` @5fd4d0 | `find_melee_target(-1, …, facing, 0/1, 0)`, `find_new_target`, `check_target` | target lost / retarget (§4.3) |
| `Unit::do_move` @5f7b30, `resolve_unit_collision` @5f9d30, `check_target_path` @5e22d0 | `find_new_target(NULL, 0 or 1)`, `find_melee_target(-1, &w, 0, 0, filter)` | path to target failed / blocked |
| `Unit::do_guard` @5e5c70 | `find_melee_target` | guarding |
| `Unit::do_group_attack` @5e75a0 | `find_nearby_target((max_range+4)*192, &w, 0, 0, 0)` (no orders) | group attack, current target unusable |
| `Object::do_damage` @64a480 | victim captain `->target_opportunity(attacker.o, attacker.who, 0)` | each time a **unit** is damaged (unless the do_damage flag arg 8 is set) |
| `Build::do_attack` @6228f0 | `Build::find_target` @622c80 → `find_nearby_target` | every building process frame with arrows (§2.16) |

`find_melee_target(r, *whom, facing, act, filter)` (argument names used throughout): `r` radius in world units
(`-1` = stance default, `0` = periodic far scan), `facing` = "prefer targets in front, no orders" mode, `act` = issue
the attack order, `filter` = mask bits (§2.5.3). It returns `ox` (or −1) and writes `whom`.

### 2.2 `Unit::think` @5f6e40 — the combat parts ✅

Called every frame from `do_idle` while the unit has no order (`check_idle` has already advanced `idle` +0xb0,
orders-spec §6.1).

1. **Formation members copy their captain** (`!is_captain()`, i.e. `o_up ≥ 0`): `cap = get_captain()`;
   `a = units[who][cap].get_action()`; if `a` is null or `a.type ≠ ATTACK` → return. Else `(ox, whom)` = `a`'s
   target; if `valid_target(ox, whom, NULL)` → `add_attack_order(ox, whom, QUEUE_NEW, mandatory = a.mandatory,
   explicit = 0)`. Return (members never run anything else in `think`).
2. Captains only from here. If `ptype.type ∈ {50, 51}` (peasants): `unit_masks &= ~0x78000000` (WITH_STUFF bits),
   `leaders[who].leader_flags |= LEADER_IDLE_PEASANT (0x80000)`.
3. **Stagger**: if `!(flags & NEW_THINK)` and `idle > 2`: continue only when `(o + frame) % 16 == 0`, else return.
4. Return if `leaders[who].leader_flags2 & LEADER_UNIT_AI_OFF (2)` or `unit_masks & UNIT_AI_OFF (0x1000000)`.
5. If `!(unit_masks & UNIT_AUTO_EXPLORE (0x100))`:
   - **Off-tick** (`idle ≠ 1` and `(o + frame) % 32 ≠ 0`): **near-attacker engagement**. If `near_o ≥ 0`,
     `near_who ≥ 0` (ObjectData +0x34/+0x36, §2.5.5) and `get_combat_stance() < 5` and `objects[near_who][near_o]`
     is VALID and `is_in_range(near_o, near_who)` (@648d70, slack 0):
       - if the unit is a packer (`unit_flags2 & 4`) **and** PACKED (`unit_masks & 0x80000`) **and**
         `get_packer_stance() ≠ 0`: clear NEW_THINK, return (no order);
       - else `add_attack_order(near_o, near_who, QUEUE_NEW, 0, 0)`, clear NEW_THINK, return.
     Otherwise skip to step 6 (no `think_attack` on off-ticks).
     Note there is **no** `valid_target` call here (only VALID + in range); `near_o` itself only ever holds a
     candidate that passed `valid_target` in an earlier scan.
   - **On-tick** (`idle == 1` or `(o + frame) % 32 == 0`): evaluate, in this order with C short-circuit,
     `(is(62 MERCHANTDUTCH, strict) and ((PACKED and think_merchant()) or (!is_packing_or_unpacking() and
     think_attack()))) or (ptype.attack ≠ 0 and (ptype.role & ROLE_COMBAT 0x10000) and think_attack())`.
     If true: clear NEW_THINK, return. (A Dutch merchant whose first clause fails can run `think_attack` twice in one
     call if it also has an attack and ROLE_COMBAT.)
6. The rest (ACTIVE-leader peasants `think_peasant`, caravans, rare collectors, human-leader scouts / spellcasters /
   idle notices, AI `think_fish`, `think_merchant`, `think_carry`, heroes/supply `add_to_army`, scouts/spies
   `think_scout`) is economy/AI and out of scope; several of those draw `game_random` (§2.18). Every successful
   branch ends with `flags &= ~NEW_THINK`.

### 2.3 `Unit::think_attack` @5f5a80 ✅

Returns 1 only when it acquired a **unit** target.

1. `player_style = !(unit_masks & UNIT_AI_CONTROL (0x40000))`.
   - player_style: stance gate — if the type's stance type is COMBAT (`ptype.has_stance_type(0)`) and
     `stance (+0xb1) ≥ 5` (HOLD_FIRE) → return 0. (Inlined `get_combat_stance`; types without a COMBAT stance type
     read as STAND_GROUND and pass.)
   - AI-controlled: `player_style = 1` if the leader is not ACTIVE (`leader_flags & 2` clear).
   - Either way: `player_style = 1` if `leader_flags2 & LEADER_COMBAT_AI_OFF (8)`.
2. **Packed packers** (`unit_flags2 & 4` and PACKED), except `is(62, strict)`:
   - if `get_packer_stance() == PACKER_AUTO (0)`: threshold = 3 if `is(123 MACHINEGUN, 0)`, else 7 for human
     leaders, 21 otherwise; if `idle ≥ threshold`: `add_cast_order(-1, -1, -1, -1, 652 UNPACK, QUEUE_FIRST, 0)`,
     return 0;
   - then (AUTO below threshold, or NEVER): player_style → return 0; AI continues.
3. **Radius argument `R`** for `find_melee_target`: player_style → `R = -1`. AI-controlled (still `player_style = 0`):
   - `tregion` = `get_tregion` of the unit's tile.
   - If the unit's tile has TData CITY (0x100) and the W-cell owner (`WData.who`) is the unit's owner: set
     `on_own_city = 1`; call `ObjectsData::find_city(x, y, SEARCH_FRIENDLY, who, who, 0x200, 0, 0, 0)` (result
     unused 🟡 side effects not checked); then if `damage ≠ 0` and (`obj_masks & (FOOT 0x20 | MOUNTED 0x1000)` or
     `healing ≠ 0`) → `R = -1`, `region_flag = 0`; else `region_flag = leaders[who].strategy[tregion] & 4`.
   - `R = -1` if `role & ROLE_SCOUT (0x10)`.
   - If type ∈ {61, 62, 400} or `is_caravan()`: `R = -1`. Else if `R ≥ 0`: **`army = add_to_army()`** @5f7740 (AI
     side effect, out of scope) and `R = -1`.
   - `R = -1` if `region_flag`.
   So `R` is always −1 here … except: if `game.semaphore byte1 & 2` → `R = 0x2d00` (60 tiles) for everyone (❓ which
   game mode sets that bit).
4. **MELEE_AND_RANGED** (`unit_flags & 0x400`): temporarily set the **type's** `max_range` (+0x1fc) to
   `second_max_range` (+0x2d0) for the duration of the search, then back to 0 (a shared mutation; a port should pass
   an override instead — every range/score function below reads `ptype.max_range` through it).
5. `t = find_melee_target(R, &w, facing = 0, act = 1, filter = 0)`.
6. If `t ≥ 0` and `objects[w][t].is_unit()`: if `group ≥ 0` and `GroupData::member(group, o, who, 1)` →
   `Group::target_opportunity(group, t, w, o, from_unit = 1)` (§2.15). Return 1.
   (A building target falls through and returns 0, but the attack order was already issued by step 5.)
7. Else, AI only (`player_style == 0`), not `on_own_city`, and `army < 0`: **retreat to the best own city**: over the
   leader's cities `i = 0 .. city_mark−1` (LeaderData +0x408; `cities.lists[who][i]`) with `city.flags & 1`, whose
   W-cell region (`WData.region` of the city's (x, y)) equals `tregion`: `w8 = world.danger[who][regcell(city)]`
   (1536-unit cells), clamped to ≥ 1; `d = vector_dist(|unit.x − city.x|, |unit.y − city.y|) / w8`, halved if
   `city.flags & 2`; keep the smallest `d` (strict `<`, initial 99,999,999, so the first city wins ties). If one was
   found: `go_to_city(i, who, 2)` @5f79c0. Return 0.

### 2.4 `Unit::find_melee_target` @5ff9c0 ✅

1. `*whom = -1`.
2. **Formation member and `!facing`**: copy the captain's ATTACK action (as in §2.2 step 1) when
   `valid_target(ox, whom)` and either no range requirement applies, or `is_in_range(ox, whom)` (@648d70). The range
   requirement applies when `stance == STAND_GROUND` **or** both this unit and its captain are entrenched
   (`unit_masks & UNIT_ENTRENCHED 0x2000000` without `unit_masks2 & UNIT2_MARINE_ENTRENCH 0x20000`).
   On success: if `act`: `add_attack_order(ox, whom, q, captain.mandatory, 0)` with `q = QUEUE_FIRST` if this unit's
   `order_type()` is ATTACK_TO, GROUP_ATTACK_TO or GUARD, else `QUEUE_NEW`. Return `ox` (and `whom`). A missing or
   non-ATTACK captain action returns −1 immediately.
   On a failed copy, fall through to step 3 with the "on duty" shortcut below.
3. **Gates**. With `!facing` (a captain, or a member whose copy failed) the gates are skipped when
   `Unit::on_duty()` @5fff70 is true (`role & ROLE_COMBAT` and the activity type ∈ {ATTACK_TO 2, GUARD 12,
   GROUP_PATROL 22, PATROL 5, GROUP_ATTACK_TO 21}); with `facing` they always run. Gates:
   - `get_combat_stance() ≥ 5` → return −1;
   - human leader, `is_cloaked()` (@60a6a0: UNDER_COVER 0x800, type CLOAK 0x4000, PASSIVE_CLOAK 0x8000 in masks2,
     or STEALTH 0x40000 with no order) and not BLOW_COVER (0x1000): if UNDER_COVER → −1; else unless
     PASSIVE_CLOAK, `unit_flags & SUBMARINE (0x100000)` or `is(68 PARTISAN, 0)` → −1.
4. **Filter override** when the front order is ATTACK_TO (type 2) or `order_type() == GROUP_ATTACK_TO`:
   - siege type (`is_siege`): `filter = 0x20002`, but a human leader keeps the passed filter;
   - else non-tank: `filter = 0x20010`; tank (`is_tank`): keep the passed filter;
   - then if `stance == RAZE (4)`: `filter = 0x20`.
5. **Radius** (`mr` = `max_range()` in tiles when `r < 1`, else 0):
   - `r < 0`: DEFENSIVE → `r = mr ? mr·192 : 288`, then `r = max(r, unit_defensive_respond_range·192)`.
     Other stances → `r = mr ? (mr+1)·192 (+384 if AGGRESSIVE) : 288`, `r = max(r, unit_respond_range·192)`, and if
     AI-controlled `r = max(r, unit_respond_range·384)`.
   - `r == 0`: DEFENSIVE → `r = unit_defensive_respond_range·192`, raised to `mr·192` if `mr` and `r ≤ mr·192`.
     Otherwise: `(o + frame) % 1024 == 0` → `r = 0x6000` (128 tiles); else `(o + frame) % 256 == 0` → `0x3000`;
     else `unit_respond_range·384`; in each case replaced by `(mr+1)·192` when `mr ≠ 0` and `(mr+1)·192 ≥` the value.
   - `r > 0`: used as given.
   - The binary tests `(r+0x2ff) < 0xc300 && circle_radius[(r+0x2ff)/0x300] < 250` and calls the same function on
     both sides: no observable difference.
6. `t = find_nearby_target(r, whom, act, facing, filter)` (note the argument order: `act` is find_nearby_target's
   3rd, `facing` its 4th). If `t < 0` and `unit_flags & FIRE_WHILE_MOVING (0x200000)`: `Guy::reset_pivots(guy, 0)`
   for guys `0 .. guy_mark−1` (turret pivots; 🟡 whether guy pivot state is checksummed). Return `t`.

### 2.5 `Object::find_nearby_target(r, *whom, act, facing, filter)` @648da0 ✅

The core scan. `S` may be a unit or a building.

#### 2.5.1 Setup

1. `melee_and_ranged = S.is_unit() && unit_flags & 0x400`. `*whom = -1`. If `ObjectData::attack() == 0` → −1.
2. If `S` is a unit: `minr = min_range()·192` (vslot), `maxr = max_range()·192`, `ai = unit_masks & AI_CONTROL`;
   if `!facing` and `update_order()` exists, `is_attack()`, and its attack order is mandatory: `cur = (ox, whom)` of
   that order (else `cur = (−1, −1)`). Buildings: `minr = maxr = 0`, `ai = 0`.
3. **Easy AI**: if the owner is not human, `LeaderData::get_diff() == 0` (easiest) and `!(semaphore byte1 & 2)`:
   `easy = 1` and `filter = (filter == 1) ? 0 : filter`. Else `easy = 0`.
4. **Ring count** `R`: `r == 0` → 32. Else `R = (r + 767) / 768` (+1 if `S.is_build()`, +1 if
   `S.has_objmask(ANTI_AIR 0x80000000)`, +1 if `S` is a unit with stance RAID), capped at 32.
5. **Per-unit state** (unit `S` and `!facing`; else `on_duty = 0`, `must_range = 1`, `guarding = 0`):
   `on_duty = Unit::on_duty()`; `guarding = (get_activity().type == GUARD)`;
   `must_range = stance == STAND_GROUND || (ENTRENCHED && !MARINE_ENTRENCH)`;
   packers (`unit_flags2 & 4`): not PACKED → `must_range = 1`; PACKED, owner not human, `!easy`, `is_siege` →
   `must_range = 1`, `ai_siege = 1`, and the **army fast path**: `a = get_army()`; if `a ≥ 0`, `(ao, aw)` = the
   army's target (+0x30/+0x34), `valid_target(ao, aw)` and `attack_dist(ao, aw) < unit_respond_range·384` →
   `add_attack_order(ao, aw, QUEUE_FIRST, 1, 0)`, `*whom = aw`, return `ao` (no scan, no `near_o`/`targeted` update).
6. **Centre W-cell**: if guarding and the guard order exists: `(guard_x/768, guard_y/768)` (floor via `div_3_table`);
   else `S`'s W-cell.

#### 2.5.2 Scan order (determines tie-breaks)

```
for i in 0 .. circle_radius[R]-1:                       // spiral of W-cells, §2.17.1
    cx = centre.x + circle_x[i]; cy = centre.y + circle_y[i]
    skip unless 0 <= cx < world.xs and 0 <= cy < world.ys
    for each object in WData[cy*xs+cx].down/down_who list:   // head first = newest first; next read before use
        evaluate candidate (2.5.3)
        if units_seen > 10 and best_o >= 0: stop the whole scan
```

Gaia (who 9) is never linked into W-cell lists (frame-spec §8), so it is never a candidate. A building is linked in
one W-cell (its centre) 🟡.

#### 2.5.3 Per-candidate pipeline (in order; any "reject" moves to the next object)

1. `T.flags & VALID` and `valid_target(ox, whom, NULL)` (§2.9) else reject.
2. **Filter bits** (`filter` after step 2.5.1.3):
   - bit 1: `T.is_live_unit()` required (units only);
   - else bit 2: `T.is_wall()` required (buildings only); additionally with bit 0x20000 the building must have
     `attack() ≠ 0`, or be a wonder, or `BuildTypeData::is_military_trainer(type)`;
   - else bit 0x20000 alone: a building needs `attack() ≠ 0` (units pass).
3. `melee_and_ranged && !facing && T.ptype.domain == SEA` → reject.
4. `check_target(ox, whom, on_duty, &dist, guarding, use_poor = 1, facing)` (§2.6) must be true; `dist` =
   `attack_dist(ox, whom)` (§2.11).
5. **Nearest-enemy memo**: if `dist < near_best` (initial 9,999,999, strict) and (`S` is a unit or `T` is a unit):
   `near_best = dist`, `S.near_o = ox`, `S.near_who = whom`. (Done before the radius test.)
6. If `r > 0` and `dist > r` → reject.
7. **Range verification** → flag `unchecked`:
   - `unchecked = 1` (no range test) iff `!must_range` and (`!guarding` or (`T.ptype.attack ≠ 0` and (`!ai` or
     `!T.has_objmask(ANTI_AIR)`))).
   - otherwise `is_in_range(ox, whom, S.x, S.y, _, slack 0)` (@6486b0): in range → `unchecked = 0`; out of range →
     reject, unless `ai_siege` and `T` is not a unit and (`T.attack() ≠ 0` or `T.is_wonder()`), which gives
     `unchecked = 0`.
   (Guarding units therefore must verify range only against armed targets that are anti-air (AI) or unarmed targets.)
8. **Distance shaping** (unit `S` only): if `minr ≠ 0`: when `dist < minr`, `dist = minr + (maxr − dist)`;
   else if `maxr ≠ 0` and `dist + 384 < maxr`: `dist = dist / 2`.
9. `dd = dist + (T.targeted + 8)·48` (`targeted` = signed char ObjectData +0x3d, the "already targeted" counter).
10. `score = compare_target(ox, whom, unchecked, easy)` (§2.8).
11. `s2 = score / (dd/192 + 1)`; if `(ox, whom) == cur` (the current mandatory attack target) `s2 /= 2`.
12. If `T.is_unit()`: `units_seen++`.
13. Filter bit 0x10: `s2 /= 2` unless `T.is_unit()`; else filter bit 0x20: `s2 /= 2` unless `T.is_build()`.
14. **Facing mode** (`facing ≠ 0`): `a = find_angle(T.x − S.x, T.y − S.y)`, `d = a − S.angle` (u32), folded
    `d = ~d` if `d > 0x80000000`. `d < 0x15555555` (30°) → `s2 <<= 2`; `< 0x2aaaaaaa` (60°) → `s2 *= 2`;
    `> 0x60000000` (135°) → reject; `> 0x40000000` (90°) → `s2 /= 10`.
15. If `s2 == 0` and `score ≠ 0` → `s2 = 1`.
16. **Selection**: `if s2 > best_score` (initial 0, **strict**): `best_score = s2`, `best = (ox, whom)`. Equal scores
    keep the earlier candidate in scan order; a candidate scoring 0 is never chosen.

#### 2.5.4 After the scan

1. If `near_best > 0xf00` (20 tiles): `near_o = near_who = −1`.
2. No best → return −1 (`*whom` stays −1).
3. `T.targeted = min(T.targeted + 1, 100)` (also when `act == 0`; decays elsewhere: `Unit::process` every 16 frames
   staggered and `Wall::process` when `(frame & 7) == who`, `targeted /= 4`, frame-spec §9).
4. If `act && !facing`:
   - **Building `S`**: `build_masks &= ~4`, `attack_ox = ox`, `attack_whom = whom`.
   - **Unit `S`**: `g = group`; if `g ≥ 0`: `Group::normalize(g)`; `g = −1` unless `GroupData::member(g, o, who, 1)`.
     If still a member and `is_siege` and AI-controlled and `T.is_wall()` and `T.flags & CITY`: `mandatory = 1`;
     `a = get_army()`; if `a ≥ 0`: army target = `(ox, whom)`, army +0x48/+0x4c = `S`'s W-cell, army +0x50 =
     `find_angle(T.x − S.x, T.y − S.y)`.
     Then with `ot = order_type()`:
       - guarding: if `ot == ATTACK_TO` → `kill_current_order(0)`; `add_attack_order(ox, whom, QUEUE_FIRST,
         mandatory, 0)`;
       - `ot ∈ {ATTACK_TO, GROUP_ATTACK_TO}`: if `T.domain == AIR` and `!S.has_objmask(ANTI_AIR)` → **return −1**
         (after `targeted++`, without `*whom`). If `ot == GROUP_ATTACK_TO`, `g ≥ 0`, `Group::is_attacking_to(g)` and
         `!is_siege`: `Group::action_attack(g, ox, whom, 0, QUEUE_FIRST, 4)` (orders-spec §2.5; **draws RNG via
         `find_attack_pos`**). Otherwise `add_attack_order(ox, whom, QUEUE_FIRST, mandatory, 0)`;
       - else: DEFENSIVE → `add_attack_order(…, QUEUE_FIRST, mandatory, 0)`, other stances → `QUEUE_NEW`.
5. `*whom = whom`; return `ox`.

#### 2.5.5 `near_o` / `near_who`

Written only here (and set to −1 by `Object::init`; checked with an instruction scan, `re/decomp/combat_A/
insn_near_who.txt`). They hold the nearest checked enemy of the last scan (within 0xf00), consumed by `think`'s
off-tick engagement (§2.2).

### 2.6 `Object::check_target(ox, whom, on_duty, *dist, guarding, use_poor, facing)` @649e00 ✅

1. If `dist`: `*dist = attack_dist(ox, whom, S.x, S.y)`.
2. Unit `S` and `!facing`:
   - `diff = tregion(T's tile) ≠ tregion(S's tile)`; cleared when `S` is AI-controlled, has objmask SIEGE (0x40000)
     and `S.domain == SEA` (bombard ships shoot ashore).
   - Range requirement: if (`!on_duty` and stance DEFENSIVE and `update_order() ≠ null`) or `diff`: require
     `is_in_range(ox, whom, S.x, S.y, _, 0)`, else return 0.
   - Guarding (`guarding` and the guard order's target `ox, whom` are both ≥ 0, i.e. guarding an object):
     `mult = 2`; if `T` is a unit, not a worker, and `S` is AI-controlled: `mult = 3`. If `S` is not a captain and its
     captain's `action_type() == ATTACK` with the same target `(ox, whom)` → **return 1 immediately** (skips steps 3–4).
     `Rg = unit_guard_respond_range · mult · 96`; return 0 if `vector_dist(|T − guard|) > Rg` or
     `vector_dist(|S − guard|) > Rg` (guard point = guard order +0x1c/+0x20).
3. Buildings in no-man's land: if `T.is_wall()` and its type lacks `build_flags & BUILDTYPE_OUTSIDE (0x10)` and the
   W-cell owner `WData.who` of `T`'s position is −1 → return 0.
4. If `use_poor && !facing && poor_target(ox, whom)` → return 0.
5. Return 1.

### 2.7 `Object::poor_target(ox, whom)` @64a270 ✅

Only for unit `S` and unit `T` (else 0).
- If `T.guys[0].guy_flags & 0x40` (🟡 meaning: airborne/flying guy): `S` without ANTI_AIR → poor (1); else poor iff
  `attack_dist > S.max_range()·192`.
- Else if `T.order_type()` is a move (types 1, 2, 3, 4, 18, 19, 21): `ts = T.get_speed(T.x, T.y, 0)`,
  `ms = S.get_speed(S.x, S.y, 0)`; if `ts > ms`: `a = find_angle(T.x − S.x, T.y − S.y)`;
  `d = T.angle − a − 0x80000000` (u32). Not poor if `d < 0x2aaaaaaa` (a one-sided window: `T` heading roughly toward
  `S`). Else `flanking(d)` @92cfe0 (= 0 if `d > 0xd5555555`, else 2 unless `d ∈ [0x60000000, 0xa0000000]` → 1); if
  `≥ 2`: poor iff `attack_dist > (role & ROLE_RANGED 0x400 ? S.max_range()·192 : 192)`.
- Otherwise 0.
(So: a faster target running across `S`'s front and already out of reach is skipped.)

### 2.8 `Object::compare_target(ox, whom, unchecked, easy)` @64e5c0 ✅

Returns the base priority `p ≥ 1` (after scaling). Let `isB = T.is_build()`, `act = T.is_active()`,
`aaB = isB && T.has_objmask(ANTI_AIR)`.

**Raid state** (unit `S` with combat stance RAID only; inlined stance read): `raid = 1`, `aiS = S AI-controlled`,
`seaS = S.domain == SEA`; if `aiS && seaS`: `raid = !S.has_objmask(SIEGE)`. Otherwise `raid = aiS = seaS = 0`.

1. `v = 4 · sum_rules_cost(T)`, with `sum_rules_cost` = `(Σ costs[0..5]) · unit_cost_factor` for units (50..413),
   `· build_cost_factor` for buildings (414..542) (`Type::sum_rules_cost` @6680e0 on `types.list[T.ptype.type]`, i.e.
   the base rules type, not a per-leader copy). If `T.is_wonder()`: `v /= 25`.
2. **Finished buildings** (`act && isB`):
   - if `aiS && seaS`: `S.unit_flags & FIRE_WHILE_MOVING` → return 0; `T` is a dock (`type.is_dock`) and `S` lacks
     SIEGE → return 0.
   - `easy` → `v *= 10`.
   - if `!raid` and `S`'s owner not human, with `W = T.get_wall()`:
     - `W.flags & CITY`: `easy` → **return 100**; else `v *= 5`;
     - else `WallData::is_defensive(W)` and `!aaB`: `v *= 4`; `easy` → further `v *= 5`, `v *= 2`;
     - else `is_military_trainer(W.type)`: `v *= 3`; if `!easy` and `W.is(520 MISSILESILO, 0)` and (`build.queued
       (+0x82) ≠ 0` or `build.inside_down (+0x28) ≥ 0`): `v *= 5`; then if `seaS && aiS && S.has_objmask(SIEGE)
       && W.type.is_dock`: `v *= 5000`;
     - else `is_training_building(W.type)`: `v *= 2`.
3. **Armed targets** (`T.ptype.attack ≠ 0`): `hl = T.hits_left()`; if `hl ≠ 0` and `!aaB`: if `act && raid && isB`:
   `v /= 20`; else `v = (T.attack() · v · 100) / hl` (left-to-right int32 products), then `isB` → `v *= 10`.
4. **Building `S`** (`S.is_build()`): if `S.attack_ox == ox` and `S.attack_whom == whom`: `v /= 2` if
   `get_garrison_arrows() > 1`, else `v *= 2`. Then `T.damage ≠ 0` → `v = v·3/2`; `T.is_moving()` → `v /= 4`;
   `T.is_supply()` → `v *= 5000`.
5. `dmg = S.get_damage(ox, whom, find_angle(T.x − S.x, T.y − S.y), 0, 0, 0)` (ObjectData::get_damage @644130; spec
   elsewhere). `easy`: `dmg == 0` → return 0, else `v = v / dmg`. Not easy: `v = dmg · v`.
6. **Building targets** (`isB`), skipped when `raid`: `a = T.ptype.attack`, zeroed if `aaB` and `S.domain ≠ AIR`.
   Unless `T` is a CITY with `num_inside(0) == 0`: if `a ≠ 0` and `S`'s owner not human: `S.obj_masks & SIEGE` →
   `v += 1,000,000`, else `v *= 5`. Then `S.obj_masks & SIEGE` and `a ≠ 0` → `v += 100,000`. Go to step 8.
7. **Unit targets** (`U = T`):
   - `U.role & ROLE_COMBAT` → `v *= 20`.
   - spellcaster (`U.unit_flags2 & 2`): if `U.get_action()` is CAST_SPELL: `v *= 20`, and `+10,000,000` if the
     spell's target is `S`; else (not casting) `U.is(58 SPY, 0)` → `v += 6,000,000`.
   - Detected decoy (`U.unit_masks & UNIT_DECOY 1` and `U.is_detected(S.who)`): `v /= 4`, skip the next item.
   - Priority bonus:
     - not raid: ROLE_COMBAT `+1,000,000`; else supply `+4,000,000` if `S.is_build()` else `+100,000`; else `+100,000`.
     - raid, human-style (`!aiS`): type 50/51 or caravan `+9,000,000`; else `v /= 10`.
     - raid, AI land (`aiS && !seaS`): peasant (50/51) or caravan `+900,000`; else ROLE_COMBAT `+10,000`; else `v /= 10`.
     - raid, AI naval (`aiS && seaS && raid`): `S.is(336 FIRESHIP)`: `U.is(317 FISHERMEN)` → return 0;
       `U.unit_flags & FIRE_WHILE_MOVING` → `v /= 10`. Else: `S` fires-while-moving and `U.is(336)` → `+1,000,000`;
       else `U.unit_flags & TRANSPORT (0x10)` `+900,000`; else `U.is(317)` `+100,000`; else ROLE_COMBAT `+10,000`.
   - Ground siege vs non-siege land unit: `S.domain == GROUND`, `S.obj_masks & SIEGE`, `U.domain ≠ SEA`,
     `!(U.obj_masks & SIEGE)` → `v /= 10000`.
   - Naval: `U.domain == SEA` and `S.domain == SEA` and `S.is(323 BARK, 0)`: `U.is(340 TRIREME)` → `v /= 400`; else
     `U.unit_flags & TRANSPORT` → `v *= 40`.
   - `v /= (U.full + 1)` (UnitData +0xac, signed char).
8. Capturable city: if `T.flags & CITY` and `T.is_active()` and `T.hits_left() == 0` → `v = 99,999`; else if
   `v < 0` (overflow) → `v = 9,999,999`.
9. If `unchecked && !raid && !is_in_range(ox, whom, S.x, S.y, _, 0)` → `v /= 5`.
10. `p = (v + 99) / 100`; if `p > 100,000`: `p = (p − 99,996) / 5 + 100,000`. `p = max(p, 15)`.
11. Detected decoy unit → `p = 1`. `T.domain == AIR` and `!S.has_objmask(ANTI_AIR)` → `p = 2`. Return `p`.

### 2.9 Validity

#### 2.9.1 `ObjectData::valid_target_const(ox, whom)` @6472c0 (vslot 0x138) ✅

Pure predicate. Return 0 when any holds, else 1:
1. `ox < 0`, `whom < 0`, `whom > 7`, `whom == S.who`, or `!leaders[S.who].is_enemy(whom)`. `is_enemy(a, b)` =
   `b ≠ a.who && (a.diplos[b] == 0 || leaders[b].diplos[a.who] == 0)` (diplos: 0 war, 1 peace, 2 allied;
   either side at war is enough).
2. `T` not VALID; `!T.is_seen(S.who, 0)` (§2.10); `T` is a unit and not on the map.
3. `S` is a unit with `unit_flags & (FIRESHIP 0x2000 | SUBMARINE 0x100000)`: `T.domain ≠ SEA` or `T.is_wall()`.
4. `S.max_range() == 0` (melee) and `T` stands on a TREE tile (`TData & 0x30 == 0x30`).
5. By `T.domain`:
   - SEA: `T` is a submarine unit and `S.domain == GROUND`. Then the final test.
   - GROUND (and any other value except AIR): the final test.
   - AIR: `S.ptype.max_range` (raw type field) `== 0`. If `T` is a helicopter (`unit_flags & 0x20`): invalid when
     `S.is_siege`, `S.is_tank` or `S.obj_masks & MISSILE`; else valid iff `!S.is(304 BOMBER, 0)` (return here).
     Planes: `T.is(306 STEALTHBOMBER, 0)` and `S.domain ≠ AIR` and `!S.can_carry(AIR)` (AIRBASE/CARRIER/SILO strict)
     → invalid. Altitude: if `S` lacks ANTI_AIR: `S.fly_high == 0 && S.fly_low == 0` → invalid; `T.fly_high == 0`:
     invalid if `T.fly_low == 0` or `T.is_flying_high()`; `T.fly_high ≠ 0 && T.fly_low == 0 && T.is_flying_low()` →
     invalid; then `S.fly_high ≠ 0` → (A) else invalid if `T.is_flying_high()`. If `S` has ANTI_AIR: `S.domain == AIR`
     → skip to the missile test; `S.fly_high == 0`: invalid if `S.fly_low == 0`, else invalid if `T.is_flying_high()`;
     (A): if `S.fly_low == 0` and `T.is_flying_low()` → invalid. Missile test: invalid if `T.obj_masks & MISSILE` or
     `S.obj_masks & MISSILE`; else valid. (`is_flying_high/low` @60a310/@60a140: air section.)
   - Final test (non-air targets): invalid if (`S.is_build()` or `S.domain == GROUND`) and `S.obj_masks` bit 31
     (ANTI_AIR) is set (anti-air-only shooters ignore surface targets).

#### 2.9.2 `Object::valid_target(ox, whom, *capturable)` @648ba0 ✅ (has side effects)

1. `*capturable = 0`; `!valid_target_const(ox, whom)` → 0.
2. If `T` is a VALID, finished (`is_active`) building and `BuildData::check_capture_eligible(T)` (@62d1d0: city
   type `is(414)`, active, and `health_level() > 5` for CITY-flagged / `> 4` otherwise): `*capturable = 1`; unless
   `S.obj_masks & MISSILE`:
   - **`Build::check_capture(T, S.o, S.who)`** @6276a0 (may capture the city: `Cities::capture_city` →
     `City::capture`, … **RNG** from the functions it calls; specified in capture-spec.md); if it returns non-zero
     `*capturable = 0`;
   - return 1 only if `S` is a map unit with objmasks VEHICLE (0x200000) and WAR_MACHINE (0x80000), its
     `order_type() == ATTACK` and that order targets `(ox, whom)` with `mandatory`; else return 0.
   Missiles skip this and fall through.
3. Return 1.

#### 2.9.3 `GroupData::valid_target(ox, whom)` @708570 ✅

True if any VALID member (in list order) that `is_captain()` has `valid_target_const(ox, whom)`.

### 2.10 Fog dependence ✅

- **Units** — `UnitData::is_seen(who, p2)` @607a60: if `p2 == 0` and the unit is "cloaked" (UNDER_COVER, type CLOAK,
  PASSIVE_CLOAK, or type STEALTH with no order) and not BLOW_COVER: require `is_detected(who)` (@60a630:
  `who == owner` or `leaders[who].ally_mask & world.seen3[fog cell]`). Then if `who ≠ owner` and `reveal_map ≠ 3`:
  if `!WorldData::is_seen(fog cell, who)` → return `ObjectData.visible (+0x40) bit who`. Else 1.
- **`WorldData::is_seen(fx, fy, who)`** @6b55c0: `who > 7` or `reveal_map == 3` → 1; leader flag REVEAL_ENEMY
  (0x800) or `num_units[321] ≠ 0` (🟡 = type 371 FOUCHE if the array is `type − 50`) → 1; ACTIVE_PING (0x2000) and the
  W-cell owner is an ally (`is_ally`) → 1; else `world.seen[fx + fog_xs·fy] & ally_mask`.
- **Buildings** — `BuildData::is_seen(who, p2)` @62e1a0: `visible` bit → 1; infiltrated (+0x3a) bit for `who`
  (when `who ≠ owner`) → 1; else `WallData::is_seen` @642bd0: owner → 1; requires (STARTED or `ally_mask` has the
  owner's bit) **and** one of: `reveal_map == 3`, `ever_seen (+0x62) & leaders[who].ally_mask`, REVEAL_ENEMY,
  `num_units[321]`, or (CITY, not multiplayer `semaphore byte0 & 4`, both leaders human). So buildings stay
  auto-targetable after being seen once (**`ever_seen`, not current fog**).
- `BuildData::is_now_seen` @62e130 / `WallData::is_now_seen` @642ca0 (vslot 0x17c) use current fog
  (`WorldData::is_seen` at the building's fog cell) but are not used by acquisition.
- `visible` (+0x40) is per-object "seen by" bits: `Build::do_attack` sets the target owner's bit when firing and
  clears the byte every 32 frames (staggered) unless ATTACKING (§2.16); units: combat-core / `update_seen` 🟡.

### 2.11 Ranges ✅

- **Units**: `min_range`/`max_range` (type +0x1f8/+0x1fc, through the vslots that add bonuses) are in **tiles**;
  every use multiplies by 192. Scan radii are world units. LOS is **not** used by acquisition (only fog bits, §2.10).
- **`attack_dist(ox, whom, x, y)`** @6488f0 (2-arg @64c880 uses `S`'s position): snap `(x, y)` and `T`'s position to
  U-cell centres (`div3(c>>4)·48 + 24`); `dx, dy` = absolute differences. If `S.is_plane()` and `S` is not a MISSILE
  → `vector_dist(dx, dy)`. Else shrink by the target's size — building `T`: `dx -= x_size·96`, `dy -= y_size·96`
  (each clamped at 0); unit `T`: `dx, dy −= block_radius + 24` (clamped) — and then by `S`'s own size the same way
  (building `S`: footprint; unit `S`: `block_radius + 24`); return `vector_dist(dx, dy)` (`vector_dist` @46cff0 =
  movement-spec §1).
- **`is_in_range(ox, whom, x, y, _, slack, *dist)`** @6486b0 ✅: 0 if `T` not VALID or (unit and off the map); 0 if
  the **shooter's** tile at `(x, y)` is TREE; `d = attack_dist(ox, whom, snapped x, snapped y)`, `*dist = d`.
  If the **type's raw** `max_range` (+0x1fc) is 0 (melee): in range iff `d ≤ 102` (`≤ 246` for `is(132 HOPLITES, 0)`).
  Else: if `d < min_range()·192 − 6`, also compute `d + big_radius(S) + big_radius(T)` (units only, type +0x244) and
  fail when that is still `< min_range()·192 − 6`; finally fail if `d + (slack ? 144 : 0) > max_range()·192 + 6`.
  Wrapper @648d70 `(ox, whom, _, slack)` passes `S`'s position. Callers here always use slack 0.
- `is_in_range(x, y)` @64e470 / `(x, y, fx, fy)` @64e4a0 (ground point, e.g. attack-ground): if `S` is not a
  MISSILE type or is inside a container: distance = `vector_dist` minus own size (building: `(x_size+y_size)·48`;
  unit: `big_radius`), melee limit 102, ranged `[min·192−6, max·192+6]`; missiles inside a launcher → 1 (🟡
  decompile only; not used by acquisition).

### 2.12 Stances ✅

`UnitData::get_combat_stance` @610a30 / `BuildData::get_combat_stance` @62e0f0 return the stored `stance`
(UnitData +0xb1 / BuildData +0x7e, signed char) only if `ptype.get_stance_type() == STANCE_COMBAT (0)`, else
**STAND_GROUND (2)**. Stance types: `UnitTypeData::get_stance_type` @61d350 — ROLE_COMBAT units: PACKER (3) if
`unit_flags2 & 4` else COMBAT; types 50..53 WORKER; `(unit_flags2 & 6) == 2` CASTER; else NONE.
`BuildTypeData::get_stance_type` @6396c0 — training buildings only: city `is(414)` WORKER, fort `is(443)` CASTER,
430/431 PACKER, 436/420/447 NONE, 520 NONE, other training buildings COMBAT; non-training buildings NONE. So packer
combat units (catapult/machine-gun lines …) always act as STAND_GROUND, and a building's stance only matters as the
rally stance it hands to produced units.

| Stance | Acquisition / response effects (sections) |
|---|---|
| 0 AGGRESSIVE | default radius `(mr+1)·192 + 384` (§2.4.5); no range requirement |
| 1 DEFENSIVE | default radius `max(mr·192 or 288, unit_defensive_respond_range·192)`; `r == 0` scans use the defensive range; `check_target` requires in-range when it has an order and is not on duty; `find_nearby_target` issues QUEUE_FIRST instead of QUEUE_NEW; `add_attack_order` (non-explicit) stores `defensive = 1` + `def_x/def_y` (orders-spec §4.2); `find_new_target` / `target_opportunity` keep the defence point and walk back to it (§2.13–14) |
| 2 STAND_GROUND | `must_range = 1` (only targets already in range); captain copy needs range; `find_new_target(…, 1)` uses it temporarily; MELEE_AND_RANGED units use `second_max_range` in `do_attack` (orders-spec §6.3) |
| 3 RAID | +1 scan ring; raid priorities in `compare_target` (§2.8); `target_opportunity` will not switch away from a current attack target |
| 4 RAZE | in attack-move `filter = 0x20` (non-buildings scored /2); `target_opportunity` will not switch |
| 5 HOLD_FIRE | `think_attack` returns 0 (player-style units), `find_melee_target` returns −1 unless on duty, `think` off-tick engagement skipped, `target_opportunity` returns, no QUEUE_FIRST counter-attack |

Constants (Constants offsets; values from `~/Games/RiseOfNations/Data/rules.xml`, raw integers, ×192 applied in code):
`unit_respond_range` +0x18 = **12**, `unit_defensive_respond_range` +0x1c = **4**, `unit_guard_respond_range`
+0x20 = **8**. Also read: `unit_cost_factor` +0x354 = 10, `build_cost_factor` +0x358 = 10 (via `sum_rules_cost`).
Not read by acquisition: `ship_defensive_respond_range` (8), `aircraft_respond_range` (10), `bomber_respond_range`
(12), `unit_build/gather_respond_range` (aircraft and worker code). Derived radii: AI default ≥ 12·384 = 4608
(24 tiles); defensive ≥ 768; guard leash 8·96·2 = 1536 (·3 = 2304 for AI vs combat units); periodic scans 0x6000 /
0x3000 / 4608; group sharing cap 12·576 = 6912.

### 2.13 `Unit::find_new_target(*whom, stand_ground)` @5ff6a0 ✅

Called when the current target/path failed.
1. `defpos = (stance == DEFENSIVE) && find_def_pos(&dx, &dy)` — `UnitData::find_def_pos` @608560: the ATTACK
   action's `def_x/def_y` when `defensive`; else the front move order's `(x, y)` if that order has flag 8; else the
   unit's U-cell centre (returns 0).
2. `repath()`; if the front order `is_group()` and `group ≥ 0`: `Group::kill_group_order(group, id)` (id from the
   group order), else `kill_current_order(0)`.
3. If `stand_ground`: temporarily `stance = 2`. `t = find_melee_target(-1, whom, 0, 1, 0)`. Restore the stance.
4. If `defpos`: if `t ≥ 0` and the new `order_type() == ATTACK`: mark it `defensive = 1`, `def_x/def_y = dx, dy`.
   Else if `order_type() == NONE`: replace the queue (as QUEUE_NEW: clear MULTIMOVE, path length 0, `close_orders`,
   `clear_partial_path`, `update_action`) with a MOVE_TO to `(dx, dy)` snapped to its U-cell centre, angle
   `find_angle(dx − x, dy − y)`, `dest = 0`, `last = orig = −1`, `facing = −1`, `off = coord % 768`, then
   `front.flags |= 8` (the "return to defence point" marker read by `find_def_pos`).
5. Return `t`.

### 2.14 `Unit::target_opportunity(ox, whom, from_group)` @5fffc0 ✅

"`whom`'s object `ox` attacked me / my group mate" (from `Object::do_damage` with `from_group = 0`, or from
`Group::target_opportunity` with 1). Steps:

1. Return if `whom == leaders[who].who` or both sides' `diplos` are non-zero (not enemies).
2. Group forwarding: `!from_group`, `group ≥ 0`, `!(role & ROLE_COMBAT)` and a member → `Group::target_opportunity
   (group, ox, whom, o, 0)`, return. Then walk up to the captain (non-captains re-dispatch to `objects[who][o_up]`
   with `from_group = 0`) and continue on the captain.
3. `!valid_target(ox, whom)` → return; stance HOLD_FIRE → return.
4. `pk = packer && get_packer_stance() == PACKER_NEVER (1) && (PACKED || is_packing())`.
5. `ot0 = order_type()`; return if `ot0 ∈ {STRAFE, AIR_PATROL, AIR_ATTACK_GROUND}`.
6. **Moving** (`is_move(ot0)`): `moving = 1`; if `T` is a unit and `vector_dist(|S − T|) ≤ S.big_radius + T.big_radius`
   (`= sz`): unless (`action_type() == ATTACK` and `vector_dist(|S − actiontarget|) ≤ 2·sz`), set
   `unit_masks |= UNIT_ZOC (0x10)`.
7. If `is_fleeing()` (front order FLEE_TO) → return.
8. `a = update_action()`:
   - **`a.is_attack()`**: same target → return. If `a`'s target is valid (`ox, whom ≥ 0`): return if `a.mandatory`;
     return if `S.has_objmask(0x840002)` (EXPLOSIVE|MELEE|BOMBARD) and the attacker is a non-siege, non-building
     while the current target is a siege unit; return if the current target is a ROLE_COMBAT unit; return if the
     attacker is a building; return if stance is RAID or RAZE; then if the current target is a unit, or `S` has
     objmask 0x840002: when the attacker is VALID and armed: return if the current target is in range, or if the
     attacker is **not** in range. (Otherwise switch.) Switching: if `a.defensive` → capture `find_def_pos` (flag
     `keepdef`). `repath()`, `kill_current_order(0)`, go to step 10.
   - **CAST_SPELL** action: only for SABOTAGE (640) casts: if the attacker is neither the cast's `ox` nor its `whom`
     (both tested separately): `spell_time /= 2`. Return in all cast cases.
   - other actions: fall through.
9. If not `moving`:
   - **No action** (`a == null`): spies `is(58)` cast BRIBE (629), commandos `is(73)` cast SABOTAGE (640) on a ground
     unit attacker when `TypeData::can_pay_cost(who, −1, o, 1)` (spell type vslot 0x84) is true,
     `SpellTypeData::is_valid_target(who, ox, whom)` and `mana_left() ≥ spell.mana_cost (+0x1d0)` (or cost 0) hold:
     `add_cast_order(ox, whom, S.x, S.y, spell, QUEUE_NEW, 1)`, return.
   - **Flee**: if (`S.has_objmask(CIVILIAN 4)` and type `max_range == 0`, or type `attack == 0`, or `pk`) and
     (`is_worker()` or `is_idle()`) and (not a packer, or not packing and PACKED), and not (a hero with
     `unit_masks & (CLOAK_RUNNING 0x2000 | MARCH_RUNNING 0x8000)`): flee distance `0x600`, except heroes and supply
     units: `0x300` if the attacker is a building or melee, `0x180` if it is ranged.
     `UnitType::find_nearby_spot(S.type, S.x, S.y, &fx, &fy, dist, −1, 0, find_angle(S.x − T.x, S.y − T.y),
     FILTER_NOT_ME, o, who, 0, 0, −1, 0, −1)` then `add_move_order(fx, fy, FLEE_TO (4), 0, QUEUE_FIRST, 0, …, −1, −1)`,
     return.
10. If `!on_duty()` and `ot0 ≠ NONE` → return (unless arriving from the switch in step 8).
11. **Counter-attack** if `ptype.attack ≠ 0` and (`a == null` or `!(a.flags & 4)` (not explicit) or AI-controlled or
    `a.type ∈ {ATTACK_TO, GROUP_ATTACK_TO}` or `a.is_patrol()`):
    - if `order_type() == NONE`:
      - out-ranged by a ship: `S.max_range() < T.max_range()`, `T.domain == SEA`, `S.domain == GROUND` and
        `find_attack_pos(ox, whom, 1, &x, &y, 1)` @602e60 fails (**RNG**: it wraps @601280): move away —
        `find_nearby_spot(…, 0x600, …, FILTER_NOT_ME, …)` and a one-unit temporary group
        `Group::action_move_to(x, y, QUEUE_FIRST, …, MOVE_TO, …)`; return.
      - DEFENSIVE: `add_attack_order(ox, whom, QUEUE_NEW, 0, 0)`; with `keepdef` restore `def_x/def_y`; return.
    - if stance ≠ HOLD_FIRE: `add_attack_order(ox, whom, QUEUE_FIRST, 0, 0)`.

### 2.15 Groups ✅

**`Group::target_opportunity(ox, whom, member_o, from_unit)`** @7107d0: throttled — runs only if
`frame − last (+0x38) > 14`, then `last = frame`. `cap` = captain of `member_o`. For each member `m` in list order
(+0x8cc, count +0xc) that is a live, on-map captain with ROLE_COMBAT:
- `!from_unit && m == cap` → `m.target_opportunity(ox, whom, 1)`;
- else if `m.get_action()` is non-null with a type ≠ NONE, or `m.order_type() ∉ {NONE, ATTACK_TO, GROUP_ATTACK_TO}`
  → `m.target_opportunity(ox, whom, 1)`;
- else `m.find_melee_target(min(vector_dist(|T − m|) + 192, unit_respond_range·576), NULL, 0, 1, 0)`.

**`Group::distribute_attack(ox, whom)`** @713390 (from `do_group_attack` / `do_group_move`): `normalize()`.
Pass 1 over members (live, on map, not a fixed-wing plane — `domain ≠ AIR` or helicopter): if the front order
`is_attack()`: remember the first such member's `mandatory` and `flags & 4`; if `!is_in_range(ox, whom)` set that
member's attack target to `(−1, −1)` (whatever it was), else count it. Pass 2 if fewer than 2 were in range and one
attacker existed: every such member with an attack front order gets `kill_current_order(0)` +
`add_attack_order(ox, whom, QUEUE_FIRST, mandatory₀, explicit₀)`.

### 2.16 Buildings ✅

**`Build::find_target`** @622c80: `r = (max(x_size, y_size) + 2·max_range()) · 96` (half the footprint plus the
range, world units; `BuildData::max_range` @62e470 = building range incl. general/temple/wonder bonuses);
`ox = find_nearby_target(r, &whom, act = 1, facing = 0, filter = 0)`; then `build_masks &= ~4`, `attack_ox = ox`,
`attack_whom = whom` (also when −1). For a building `S` the scan always verifies range (`must_range = 1`), adds one
ring, and the score uses §2.8 with `S.is_build()` (step 4).

**Acquisition inside `Build::do_attack`** @6228f0 (per process frame):
1. Recharge: unless the building has ANTI_AIR objmask and is not LOOKOUT/OBSERVATIONPOST (strict), a non-zero
   `recharging` (+0x7a) is decremented and the frame ends.
2. Every 32 frames staggered (`(frame + o) & 31 == 0`) and not ATTACKING: `visible = 0`. Clear ATTACKING.
3. `hits_left() == 0` → return. Jammed (`ObjectData::is_jammed` ≥ 0): mark the jammer `unit_masks2 |= NEW_JAM`,
   `build_masks |= 0x8000`, return; else clear 0x8000. `get_garrison_arrows() == 0` → return.
4. If `!(build_masks & 4)` (4 = target set explicitly): `find_target()`.
5. No target → clear (`build_masks &= ~4`, target −1), return. `!valid_target(target)` → `find_target()`, return.
6. Re-evaluation (non-explicit only), when `frame == 0` or `(frame + o + 14) % 32 == 0`: if the target is not a unit,
   or is moving, or (is not ROLE_COMBAT and its action is not CAST_SPELL) → `find_target()`, return.
7. In range (`is_in_range`, slack 0): AA buildings other than LOOKOUT/OP return without firing; otherwise
   `fire_ammo(target)` (§4.6), `flags |= ATTACKING`; if `visible` lacks the target owner's bit: set it, and
   if `ever_seen` (+0x62) also lacks it call `update_local_seen` (vslot 0x164); `recharging = (short)ptype.recharge /
   arrows`.
   Out of range → clear the target.

### 2.17 Spatial tables and `Objects::find_units`

#### 2.17.1 `circle_x / circle_y / circle_radius` (`circle_init` @006817f0) ✅

Built at start-up (world-spec §2.4; implemented in `engine/sim/compass.cpp::make_circles`): for `r = 0..64`, for
`x = −r..r` (outer), `y = −r..r` (inner), append `(x, y)` when `circle_dist(|x|, |y|) == r` with
`circle_dist(ax, ay) = ay < ax ? ay²/(2ax) + ax : (ay == 0 ? 0 : ax²/(2ay) + ay)` (unsigned; the ≥ 60000 branches never
trigger); `circle_radius[r]` = running count after ring `r`. First values 1, 9, 21, 45, 69, 105, 145, 185, 237 …;
`circle_radius[32] = 3325`.
**Capacity cap** ✅ (decompile, re-run in Python): the arrays hold 0x3249 = 12873 points. Ring 64 would reach 12965,
so the original stops at the 12873rd point (ring 64 at `(x, y) = (46, −41)`), sets `circle_radius[64] = 12873` and
returns. **`engine/sim/compass.cpp` does not apply this cap** (it yields 12965); only ring 64 differs, which
`find_nearby_target` never reaches (cap 32) but `find_units` can (radius > 62·768).

#### 2.17.2 `Objects::find_units(x, y, search, who, radius, flags, filter, f1, f2, _, players8)` @65a620 ✅

Not used by target acquisition (callers: `detect_boat_collision`, `fight`, `find_new_air_target`, `find_build_spot`,
`check_build_order`, `Nuke::do_damage`, `cast_entrench`, scenario `find_objects`). Output: the shared list
`objects.found` (pointer array +0x214, count +0x208 reset to 0, capacity +0x20c, grown by the callback +0x204 with
the short at +0x210); also stores `who` at +0x200 and 99,999,999 at +0x1fc (unused here). Returns the count; no cap.

- `flags & 0x200` → region filter: `reg = get_tregion(tile of (x, y))`; candidates must be in the same tregion.
- `n = (radius + 767) / 768`. **Cell mode** when `radius ≥ 0` and `circle_radius[n] ≤ game.total_units`:
  for `i = 0 .. circle_radius[n]−1`, W-cell `(x/768 + circle_x[i], y/768 + circle_y[i])` (in bounds), each object of
  its list (head first; next read first): require `cand.who < 9` (`< 8` if `players8`), `Search::valid_search
  (cand.who, search, who)`, `is_live_unit()` (units only), `is_on_map()`, the region filter, then
  `Search::valid_filter(cand.who, cand.o, f1, f2, filter−1)` for `filter ∈ 1..23`; append. **No distance test**
  (everything in the covered cells).
- **Dense mode** otherwise: for `who' = 0..8` with `leader_flags & VALID` (skipping `who' ≥ 8` if `players8`) and
  `valid_search(who', search, who)`: for `o = 0 .. unit_mark[who']−1`: live unit, on map, filter, region filter; then
  `d = vector_dist(|cx − x|, |cy − y|) − (domain == SEA ? push_size : big_radius)`; append if `radius < 0` or
  `d ≤ radius`.
- `Search::valid_search(cand, kind, who)` @67daa0: `who < 0` → 1; 1 FRIENDLY `who == cand`; 2 PEACE neither ally nor
  enemy; 3 ENEMY `leaders[cand].is_enemy(who)`; 4 ALLIED `is_ally`; 5 NON_ALLIED; 6 NON_FRIENDLY `who ≠ cand`;
  7 NON_ENEMY; 0 all.
- `valid_filter` is a jump table @67e57c indexed by `filter − 1`; decoded: TYPE `is(f1, 1)`, BASE_TYPE `is(f1, 0)`,
  NOT_ME `!(o == f1 && who == f2)`, NOT_MY_UBER (different captain), NOT_DOMAIN `domain ≠ f1`, REGION `on map &&
  tregion == f1`; the rest ❓ (not needed here).
- The two modes return **different sets and orders** (cell/newest-first vs player/index order, with vs without the
  distance test), and the switch depends on `game.total_units`: a port must reproduce both exactly.

### 2.18 RNG draws

None of the functions specified in this section reads `game_random` directly (none appears in
`re/analysis/xrefs_random.txt`; confirmed with a depth-9 call tree, `re/decomp/combat_A/calltree_acq8.txt`).
Draws reachable through callees, in the order they can occur within one acquisition:

| Order | Path | Draw site(s) | Condition |
|---|---|---|---|
| 1 | `valid_target` → `Build::check_capture` → `Cities::capture_city` → `Build::swap_team` (Objects::init_build's `find_gather_tiles` shuffle, `Unit::come_out` of a garrison @61a1aa), `Build::activate` (Farms::add_animals), `Unit::swap_team` (Guy::init_real, allied transfers only), `City::close` → `Leader::defeat` (multiplayer `blow_up_units`: death animation draws) | see capture-spec §9 | target is a capture-eligible **city** (forts are never captured) and the capture actually happens. Can run for **every candidate** in a scan. Corrected: a capture disbands only buildings (no draw) and plunders no unit, so @6459c3 / @605fca / @606159 are not reached |
| 2 | `find_nearby_target` → `Group::action_attack(…, QUEUE_FIRST, 4)` → `Unit::find_attack_pos` @601280 (+ `finish_insert` → pathfinding `astar_path`) | @602107 (per candidate cell); @6848b5, @684df5 | acting unit is on GROUP_ATTACK_TO, group `is_attacking_to`, not siege |
| 3 | `target_opportunity` → `find_attack_pos` @602e60 → @601280 | @602107 | idle ground unit out-ranged by a ship |
| 4 | `think` non-combat branches: `think_scout`, `think_fish`, `think_spellcaster`, `think_merchant`… | think_scout @5f6123 …, think_fish @5f4ec6/@5f4f17, think_spellcaster @5f2ba1/@5f2d17 | see their own sections |

(`SoundGlobal::play` / `SoundType::play` appear in the call tree but use their own RNGs — presentation only.)

### 2.19 Rules / data read

Rules (loaded by `engine/sim/rules*.hpp`): type `obj_masks` 0x1e4, `attack` 0x1e8, `recharge` 0x1f4, `min_range`
0x1f8, `max_range` 0x1fc, `domain` 0x218, `x_size/y_size` 0x234/0x238, `block_radius` 0x240, `big_radius` 0x244,
`fly_high/fly_low` 0x250/0x254, `unit_flags/unit_flags2` 0x2b4/0x2b8, `role` 0x2c8, `second_max_range` 0x2d0 (loaded,
not walked), `carry` 0x2d4, `push_size` 0x2f8, `build_flags` 0x2c0, `costs[6]` 0x18, `is_list`/`is_strict_list`,
spell `mana_cost` +0x1d0; constants `unit_respond_range`, `unit_defensive_respond_range`, `unit_guard_respond_range`,
`unit/build_cost_factor` (and inside the stat getters: `general_building_range`, siege/range bonuses …).

Non-rules state that must exist (flag for the port):
- per object: `flags`, `who/o`, position, `targeted` (+0x3d), `near_o/near_who`, `visible` (+0x40), `damage`,
  `healing`, `infiltrated` (+0x3a); building `ever_seen` (+0x62), `build_masks` bit 4/0x8000, `attack_ox/whom`,
  `recharging`, `queued`, `inside_down`; unit `stance`, `idle`, `unit_masks/2`, `o_up` (captain chain), `group`,
  `angle`, `full` (+0xac), guys (`guy_flags`), order list + accessors (`get_activity`, `update_action` …);
- leaders: `leader_flags/2` (HUMAN, ACTIVE, REVEAL_ENEMY, ACTIVE_PING, UNIT_AI_OFF, COMBAT_AI_OFF), `diplos[8]`,
  `ally_mask`, `num_units[]` (index 321), `strategy[64]`, `city_mark` + cities, `multi_diff` / game difficulty
  (`get_diff`);
- world: `wdata` (`down` lists, `who`, `region`), `tdata` (TREE 0x30, CITY 0x100), `seen`, `seen3`, `danger[8]`,
  `get_tregion`; `game.frame`, `game.total_units`, `game.semaphore` bytes, `reveal_map`;
- groups (members, captain flags, `last` +0x38, `is_attacking_to`) and armies (target +0x30/+0x34, +0x48..+0x50);
- `circle_*` tables with the 12873 cap (§2.17.1).

### 2.20 Open questions / uncertain

1. ❓ `game.semaphore byte1 & 2`: forces `think_attack`'s radius to 0x2d00 and disables "easy AI"; which mode sets it.
2. 🟡 `guy_flags & 0x40` in `poor_target` (assumed "airborne"); `num_units[321]` indexing (type 371 if `type − 50`).
3. ❓ Whether `Guy::reset_pivots` (fire-while-moving units with no target) touches checksummed state.
4. 🟡 `ObjectsData::find_city` in `think_attack` is called for its side effects only (return ignored); not checked.
5. 🟡 `Unit::add_to_army` (AI) runs inside `think_attack` before the search for most AI units; its effects (army
   bookkeeping, possible orders) belong to the AI spec but are part of the acquisition frame.
6. ❓ Remaining `Search::valid_filter` cases (CAN_COLLIDE, DAMAGED, COMBAT, TARGET, SEEN …) — needed by `find_units`
   users (fight, collision), not by acquisition.
7. 🟡 `UnitData::is_seen`'s `visible` fallback: which code sets unit `visible` bits (update_seen / fight) — fog section.
8. ✅ `Build::check_capture` is called from `valid_target`, i.e. potentially many times per scan (once per
   capture-eligible candidate); a capture inside a scan is safe for the W-cell iteration (the new city object is
   linked at the list head, behind the cursor; capture-spec §2.5).
9. ~~Engine divergence~~: fixed by the fog port (`compass.cpp` kCircleCap).

### Evidence (section 2)

| Function | Address | File |
|---|---|---|
| `Unit::think`, `think_attack` | 5f6e40, 5f5a80 | `re/decomp/combat/`, asm in `re/decomp/combat_A/asm/` |
| `Unit::find_melee_target`, `find_new_target`, `target_opportunity` | 5ff9c0, 5ff6a0, 5fffc0 | same |
| `Object::find_nearby_target`, `check_target`, `poor_target`, `compare_target`, `valid_target` | 648da0, 649e00, 64a270, 64e5c0, 648ba0 | same |
| `ObjectData::valid_target_const`, `is_in_range` ×4, `attack_dist` ×2 | 6472c0, 6486b0/648d70/64e470/64e4a0, 6488f0/64c880 | `re/decomp/combat/` (+asm) |
| `UnitData::is_seen`, `BuildData::is_seen/is_now_seen`, `WallData::is_seen/is_now_seen`, `WorldData::is_seen`, `UnitData::is_detected/is_cloaked` | 607a60, 62e1a0/62e130, 642bd0/642ca0, 6b55c0, 60a630/60a6a0 | `re/decomp/combat*/` |
| `UnitData/BuildData::get_combat_stance`, `get_packer_stance`, `Unit/BuildTypeData::get_stance_type`, `has_stance_type` | 610a30/62e0f0, 610970, 61d350/6396c0, 61d3b0/639770 | `re/decomp/combat*/` |
| `Build::find_target`, `Build::do_attack`, `BuildData::check_capture_eligible`, `Build::check_capture` | 622c80, 6228f0, 62d1d0, 6276a0 | `re/decomp/combat/`, `re/decomp/atk/` |
| `Group::target_opportunity`, `distribute_attack`, `GroupData::valid_target` | 7107d0, 713390, 708570 | `re/decomp/atk/` (+asm) |
| `Objects::find_units`, `Search::valid_search/valid_filter`, `circle_init` | 65a620, 67daa0/67dbb0, 6817f0 | `re/decomp/combat*/`, `re/decomp/grid_misc/` |
| `Unit::on_duty`, `UnitData::get_activity/find_def_pos`, `Object::get_army`, `LeaderData::is_enemy/get_diff`, `Type::sum_rules_cost`, `flanking`, `find_angle`, `vector_dist` | 5fff70, 608370/608560, 649d70, 6ebaa0/6ec000, 6680e0, 92cfe0, 92d130, 46cff0 | `re/decomp/combat_A/`, `re/decomp/diplo/` |
| Callers / call trees / instruction scans | – | `re/decomp/combat_A/callers_find_units.txt`, `calltree_acq*.txt`, `insn_near_who.txt`, `insn_targeted.txt` |

## 3. The attack order and getting into range

Scope: how an ATTACK order is created (unit and group level), how the
order handler `Unit::do_attack` dispatches, how a unit that is out of range picks a position to shoot from
(`find_attack_pos`), how the approach is re-checked while walking (`check_target_path`), attack-move, the group attack
orders, and the per-unit combat bookkeeping (ATTACKING flag, IN_COMBAT, `visible`, `targeted`, cavarch). The inside of
`Unit::fight` (facing, animation, firing, damage, target re-acquisition) is §4.3; here it is only a
callee with a contract.

Conventions: positions are plain world coordinates (the stored `x/y_internal` are `^ 0x63637`). `ucentre(c)` =
`div_3_table[c >> 4]·48 + 24` (centre of the 48-unit cell), `tcell(c)` = `div_3_table[c >> 6]` (192-unit T cell),
`wcell(c)` = `div_3_table[c >> 8]` (768-unit W cell). `vd(a, b)` = `vector_dist` @46cff0 (takes |a|, |b| itself;
movement-spec §1). `project(a, r, x, y)` @92cf40 = `(x + sin(a, r), y − cos(a, r))` (the inlined `sin` wrapper of
movement-spec §1; `r == 0` gives `(x, y)`). `find_angle(dx, dy)` as movement-spec §1. "Type" is the unit's `ptype`;
`T` is the target object `objects[whom][ox]`. Virtual calls are named by the slots in §0; several are
devirtualised in the binary (the code compares the vtable entry with the base implementation and inlines it), which
does not change behaviour. Confidence: ✅ read from disassembly, 🟡 partly inferred / decompiler only, ❓ unknown.

All new decompiles and disassembly for this section are in `re/decomp/combat_B/` (job files
`re/jobs/dc_combat_B_1..7.txt`).

### 3.1 Data

#### 3.1.1 `AttackOrder` (type 10, 0x30 bytes; also the base of STRAFE 16 and GROUP_ATTACK 20)

| Off | Field | Written by | Read by |
|---|---|---|---|
| 0x08/0x0c/0x10 | `ox`, `whom`, `uid` (TargetOrder) | `add_attack_order`; `change_target`; `fight` (retargets in place); `Group::distribute_attack` (sets −1/−1); `work` (−1/−1 when the uid no longer matches) | everyone |
| 0x14/0x18 | `def_x`, `def_y` (defend point) | `add_attack_order` | `find_def_pos`, `check_target_path` leash, `fight` leash / return-home |
| 0x1c | `mandatory` (u8) | `add_attack_order`; `find_attack_pos` **clears it** in one melee case (3.8.3) | `do_attack`, `check_target_path`, `find_attack_pos`, `fight`, `Group::action_attack`, `distribute_attack` |
| 0x1d | `defensive` (u8) | `add_attack_order` | `find_def_pos`, `check_target_path`, `fight` |
| 0x1e | `in_range` (u8) | `add_attack_order` = 0; `fight` (non-cavarch calls) = `is_in_range` from the current position | 🟡 fight-internal |
| 0x1f | `ever_in_range` (u8) | `add_attack_order` = 0; `fight` = 1 once in range | `fight`: an unpacked PACKER that has been in range drops the order when the target leaves range |
| 0x20 | `new_ord` (u8) | `add_attack_order` = 1; `fight` = 0 after the first attack animation (`set_attacking`) | `fight`: a recharging non-MELEE_AND_RANGED unit only proceeds while `new_ord` is set |

`GroupAttackOrder` adds `GroupOrder` at 0x28 (`oxx` 0x28 / `whose` 0x2c = group leader, `id` 0x30, `form_id` 0x34,
`group_angle` 0x38), `temporary` 0x3c, `oxxx` 0x40, `whosoever` 0x44 (the target actually being fought).
`UnitOrder.flags` bits used here: 0x04 explicit, 0x08 "return-to-defend-point move" (set by `fight` on the move it
queues when a defensive attacker gives up; read by `find_def_pos`, 3.2.1), 0x10 toggled by `fight` on the action after it
queues an approach move (🟡), 0x80 cleared by `add_attack_order` (❓ no reader found).

#### 3.1.2 Unit / object fields

| Field | Meaning in this section |
|---|---|
| SubObject `flags` 0x80 **ATTACKING** | set by `Unit::set_attacking` (from `fight`) and `Unit::do_cast` @5ec5a6; cleared by `Unit::work` @60d1d4 whenever the front order is not ATTACK (✅ instruction scan `re/decomp/combat_B/insn_attacking_flag.txt`). `Build::do_attack` sets/clears it for buildings |
| ObjectData `visible` +0x40 (s8 bitmask over players) | bit p set by `set_attacking(p)`; cleared to 0 by `work` every 32 frames (`(o + frame) & 31 == 0`) **only if ATTACKING is clear**. `UnitData::is_seen(p)` returns true for a unit in p's fog when bit p is set — an attacker is revealed to its victim's owner |
| ObjectData `targeted` +0x3d (s8) | `Object::find_nearby_target` increments it on the object it picks (cap 100) and uses `(targeted + 8)·48` as a distance penalty; `Unit::process` divides it by 4 (signed, toward 0) every 16 frames (`(o + frame) & 15 == 0`); `Wall::process` likewise; `Object::init` = 0 |
| UnitData `unit_masks` 0x4 **UNIT_IN_COMBAT** | set by `set_in_danger` (from `work` for ATTACK actions, and `Object::take_damage`); cleared by `work` every 32 frames (same stagger) **unconditionally** |
| `unit_masks` 0x40000 UNIT_AI_CONTROL, 0x800000 UNIT_CAN_TRANSPORT, 0x2000000 UNIT_ENTRENCHED, 0x80000 UNIT_PACKED, 0x4000000 UNIT_MULTIMOVE | read as noted below |
| `unit_masks2` 0x100 UNIT2_KEEP_TARGET, 0x20000 UNIT2_MARINE_ENTRENCH, 0x10 UNIT2_HOLD_ANIM | cavarch cache validity; "hold position" for marines; set by `fight` on give-up |
| `cavarch_o` +0xa2 (s16), `cavarch_who` +0xa8 (s8), `cavarch_uid` +0xa6 | moving-fire target cache. −1 = search now, −2 = nothing found last time (search only every 32 frames) |
| `full` +0xac (s8) | crowding counter of a melee target: `find_melee_pos` adds 5 (cap 30) when no free slot exists around it |
| `recharging` +0xae | 0 = ready |
| `stance` +0xb1 | `CombatStanceIndex`: 0 AGGRESSIVE, 1 DEFENSIVE, 2 STAND_GROUND, 3 RAID, 4 RAZE, 5 HOLD_FIRE |
| guy flags +0x9a bit 0x20 | set on every guy by `set_in_danger` |

### 3.2 Creating the order

#### 3.2.1 `Unit::add_attack_order(ox, whom, QueuePos q, mandatory, explicit)` @5e5410 ✅

1. `q == QUEUE_NEW`: the standard preamble of orders-spec §4.2 (clear UNIT_MULTIMOVE, `path.length = 0`,
   `close_orders(0)`, `clear_partial_path`, `update_action`).
2. New AttackOrder from the pool: `ox`, `whom`; `uid = objects[whom][ox].uid` if both ≥ 0, else 0xffff;
   `mandatory = mandatory`; `in_range = ever_in_range = 0` (one 16-bit store); `new_ord = 1`.
3. Stance: `s = get_combat_stance()` (devirtualised: if the type has stances (`has_stance_type` → type vslot 0x134)
   `s = stance`, else STAND_GROUND). If `s == DEFENSIVE && explicit == 0`: `defensive = 1`;
   `find_def_pos(&def_x, &def_y)`; if it returns 0, `def = (ucentre(x), ucentre(y))` of the unit (the same value
   `find_def_pos` already wrote). Otherwise `defensive = 0`, `def = (−1, −1)`.
4. `flags &= ~0x80`; flag 0x04 = `explicit != 0`.
5. `orderlist.add`; `q == QUEUE_FIRST` → `clear_partial_path` and rotate head (new order becomes the front).
   `update_action()`.

`UnitData::find_def_pos(&x, &y)` @608560 ✅ (called **before** the new order is added, so with QUEUE_NEW the list is
already empty):
1. If `get_action()` is an ATTACK with `defensive` set: return its `(def_x, def_y)`, result 1 (a chained
   non-explicit attack inherits the existing defend point).
2. Else, if the front order has flag 0x08 and is a move: return that move's `(x, y)`, result 1 (a unit walking back to
   its defend point keeps that point).
3. Else `(ucentre(x), ucentre(y))`, result 0.

Consequence: explicit (player-issued) attacks are never leashed; every automatic attack (`think`, `target_opportunity`,
`find_melee_target`, `find_nearby_target`, collision retaliation, attack-move re-add, `distribute_attack` with the
copied flags) by a DEFENSIVE unit is.

Callers (✅ `re/decomp/combat_B/callers.txt`): `Group::action_attack` (explicit 1), `Group::distribute_attack` (copies
`mandatory`/flag 4 of the first member's attack), `add_move_facing_order` (attack-move re-add: QUEUE_FIRST, mandatory 0,
explicit 0), `Unit::come_out`, `target_opportunity`, `find_melee_target`, `Object::find_nearby_target`,
`resolve_unit_collision`, `resolve_block`, `think`, `SpellType::cast_entrench`.

#### 3.2.2 `Build::add_attack_order(ox, whom, mandatory)` @622ce0 ✅

`attack_ox` (+0x7c, s16) = ox, `attack_whom` (+0x81, s8) = whom, `build_masks` (+0x60) bit 4 = `mandatory != 0`.
**No callers** in the binary: `Group::action_attack` inlines the same three stores for building groups (with bit 4
always set). Buildings have no order list; `Build::do_attack` reads these fields.

### 3.3 Group level

#### 3.3.1 `Group::action_attack(ox, whom, mandatory, QueuePos q, ignore)` @712490 ✅

Called by `process_attack` (mandatory 1), `check_target_path` (mandatory 1, QUEUE_FIRST, ignore 0), `do_group_move`
(collision with an enemy during GROUP_ATTACK_TO: `(collide_o, collide_who, 0, QUEUE_FIRST, 0)`), `Army::engagement`,
`Army::find_target`, `Object::find_nearby_target`, `Group::action_flight`, `action_siege_attack`, scenario triggers.

1. Return unless `GroupData::is_on_map()` and `ox ≥ 0`. Scenario `ignore_orders` prelude, `action_begin()`
   (orders-spec §2.1). Return if the group is empty.
2. **Building group** (`buildings` byte +0x49): for each member in list order, if it is a Build: `build_masks |= 4`,
   `attack_ox = ox`, `attack_whom = whom` (a non-Build member only raises a debug assert). Presentation only: the first
   member out of range of the target gives the console player a "cannot reach" message. Return (no `order_num++`).
3. `q == QUEUE_FIRST`: `set_up_insert`, `action_halt(ignore)`, recurse with QUEUE_NEW, `finish_insert`; return.
4. `L = find_leader()`; return if < 0. `P = L.position`; with QUEUE_LAST, `GroupData::get_loc_to(&P)` (where the group
   will be after its queued orders).
5. If `!is_in_range(ox, whom, P, slack 0)` (6-arg form, 3.6.2) evaluated **for the leader**:
   `Unit::find_attack_pos(L, ox, whom, 1, &rx, &ry, P.x, P.y, 1)` (3.8). The result position is not used by the
   simulation (only a console-player "invalid order" message and sound when it fails and `T` vslot 0x10 is true), but
   the call has side effects: `game_random` draws (3.8.4) and possibly `mandatory = 0` on L's front attack (3.8.3).
6. `T` vslot 0x18 is called and ignored. If `T.is_build && is_active_build(T) && BuildData::check_capture_eligible(T)`:
   `Build::check_capture(T, L.o, who)`, then `action_move_to(T.x, T.y, q, 0, 0, MOVE_TO, 1, −1, −1, 0)`; return (no
   `order_num++`).
7. `unseen = !T.is_seen(who, 0)` (vslot 0x48).
8. Three passes `dom = 0, 1, 2`; in each, members in list order. For member M (`o = list[i]`):
   - skip unless M is a valid unit (vslot 0x08) and on the map;
   - skip if `ignore & 4` and M's type is siege (`unit_flags & UNITTYPE_SIEGE` 0x20000);
     if `ignore & 2` and M is special (`unit_flags2 & 0x10`); if `ignore & 1` and `M.is(58, 0)` (spy class 🟡);
   - **planes** (`is_plane`: domain 2 and not helicopter): if `mana − mana_burn > 0` (clamped at 0) and the front
     order is STRAFE, retarget the strafe in place (`ox`, `whom`, `uid`, +0x3c = 0); skip in all cases;
   - skip unless `M.type.domain == dom`;
   - **already on it**: if `get_action()` is ATTACK on `(ox, whom)` with `mandatory != 0`, `orderlist.length ≤ 2` and
     `!is_in_range(ox, whom)` from M's current position: skip if `length == 1`; else if the front order is a move and
     `is_in_range(ox, whom, move.x, move.y)` (6-arg, slack 0), skip;
   - `pk = is_packing_or_unpacking(M)`.
   - **`!pk || q != QUEUE_NEW`**:
     - `T` not a Build (units **and walls**): if `unseen`, `add_move_order(T.x, T.y, kind 1, 0, q, explicit 0)`.
       Target `(tox, tw) = (ox, whom)`. If `mandatory == 0`: `r = min(vd(T.x − M.x, T.y − M.y) + 192,
       constants.unit_respond_range · 576)`; `(o2, w2) = M.find_melee_target(r, &w2 (init whom), 0, 0,
       T.is_wall ? 2 : 1)`; if `o2 ≥ 0 && w2 ≥ 0`, `(tox, tw) = (o2, w2)`. Then
       `add_attack_order(tox, tw, q, mandatory, explicit 1)`.
     - `T` a Build, seen: `add_attack_order(ox, whom, q, mandatory, 1)`.
     - `T` a Build, unseen: if `is_in_range(ox, whom)` from M → `Build::check_capture(T, M.o, who)`; then
       `add_move_order(T.x, T.y, 1, 0, q, 0)` (no attack order).
   - **`pk && q == QUEUE_NEW`**:
     - seen: `clear_orders()`; if M is **unpacking** (not `is_packing`) and `is_in_range(T from M)`:
       `add_cast_order(−1, −1, −1, −1, 652 UNPACK, QUEUE_NEW, 0)`; if M is **packing** and *not* in range:
       `add_cast_order(…, 651 PACK, QUEUE_NEW, 0)`; then `add_attack_order(ox, whom, QUEUE_LAST, mandatory, 1)`.
     - unseen: if `T.is_build` and in range → `check_capture`; if `is_packing` → cast PACK (QUEUE_NEW);
       `add_move_order(T.x, T.y, 1, 0, QUEUE_LAST, 0)`.
9. `order_num++` (only on this path).

With QUEUE_NEW the unseen-target move is removed again by the attack's own `close_orders` (orders-spec §2.5 — keep it,
it is what the original does).

#### 3.3.2 `Group::distribute_attack(ox, whom)` @713390 ✅

Called by `do_group_attack` (leader out of range, non-temporary) and `do_group_move` (a member of a group moving with an
ATTACK/GROUP_ATTACK action collides with an enemy: `kill_group_move` then `distribute_attack(action target)`).

1. `normalize()`; `n_in = 0`, `first = −1`.
2. Pass 1, list order, members that are valid units, on the map and not planes, with a non-empty order list whose
   front `is_attack`: `a` = front AttackOrder. The first such member fixes `mand = a.mandatory`,
   `expl = front.flags & 4`. If `is_in_range(ox, whom)` from that member's position (6-arg, slack 0) `n_in++`, else
   `a.ox = a.whom = −1` (the member forgets its target; `work` skips the uid check for ox < 0 and the attack handler
   re-acquires).
3. If `n_in < 2` and `first ≥ 0`: pass 2 over the same members (same filter): `kill_current_order(0)`, then
   `add_attack_order(ox, whom, QUEUE_FIRST, mand, expl)` — everybody switches to the new target.

#### 3.3.3 `Unit::do_group_attack(order)` @5e75a0 (GROUP_ATTACK) ✅

`g` = the GroupAttackOrder.
1. `recharging != 0` → return.
2. `set_angle(g.group_angle, _, 0)` unconditionally.
3. **Not in a group** (`group == −1`): convert in place — a new ATTACK order is copied from `g` with
   `AttackOrder::operator=` @7304c0 (all AttackOrder fields, `ox/whom/uid`, and the flags byte), the front node is
   unlinked (`remove_current`; the GroupAttackOrder object is **not** returned to the pool), the copy is added and
   rotated to the front. Return.
4. **Leader** (`g.oxx == o && g.whose == who`):
   - `T` dead → `Group::kill_group_order(g.id)`; return.
   - in range (4-arg `is_in_range`, slack 0) → `g.oxxx/whosoever = (ox, whom)`;
     `fight(ox, whom, g.mandatory, g.temporary, 0)`; return.
   - out of range: `temporary == 0` → `Group::kill_group_move(g.id)`, `Group::distribute_attack(ox, whom)`;
     else `Group::kill_group_order(g.id)`. Return.
5. **Follower**: if the leader object is not a valid unit, not on the map, or `leader.group != group` →
   `Group::refresh_group_order(g.id, o, who)`; return. Pick a target: if `g.mandatory ≥ 2` and the group target is in
   range, use it; else `Object::find_nearby_target((max_range() + 4)·192, &w, 0, 0, 0)`. If that target is valid and in
   range: `g.oxxx/whosoever` = it; `fight(t, w, mandatory 1, temporary 1, 0)`; return.
6. Otherwise look at the **second** order (the one after the front): if it is GROUP_MOVE, `g.temporary != 0`,
   `path.length != 0` and that move's `dest_x/dest_y` (+0x2c/+0x30) differ from the unit's position: unlink the front
   (the group attack), `s = get_speed(x, y, 1)`, `move_step(front move, (9·s)/8)` (signed division), then re-add the
   group attack and rotate it to the front. Else: if `angle != g.group_angle` → `set_angle(g.group_angle, _, 0)`, else
   `set_anim(CHAR_DEFAULT, 0, 1)`.

#### 3.3.4 `Unit::do_group_attack_to(order)` @5e74e0 (GROUP_ATTACK_TO) ✅

`do_group_move(order)`; then, if the front order is still `order` and `(o + frame) % 15 == 0`: units with
`type.attack != 0` that are not supply (`unit_flags2 & 0x40`) call `find_melee_target(−1, null, 0, 1, 0)`; the others
call `do_attack_to_pause(order)`. (Same as `do_attack_to` without the army test.)

### 3.4 Attack-move (ATTACK_TO)

`Unit::do_attack_to(order)` @5f2320 ✅: `do_move(order)` (movement-spec §3.5); then, only if the front order is still
`order` and `(o + frame) % 15 == 0`:
- `type.attack != 0` and not supply: if `UNIT_AI_CONTROL`, `get_army() ≥ 0`, the army's word +0x2c is non-zero and
  `vd(wcell(x) − army.x(+0x48), wcell(y) − army.y(+0x4c)) > 6` (W cells) → return (the army keeps it in formation).
  Otherwise `find_melee_target(−1, null, 0, 1, 0)` (acquisition, §2.4).
- otherwise `do_attack_to_pause(order)`.

`Unit::do_attack_to_pause(order)` @5f22a0 ✅: if the unit is in a group, `GroupData::member(o, who, 1)` holds, and
`Group::is_attacking_near(x, y)`: `order.pause (+0x18) = 15` (do_move then idles it, movement-spec §3.5 step 12).

`Group::is_attacking_near(x, y)` @710e40 ✅: `normalize()`; over members that are valid, on the map, captains,
`type.attack != 0`, not supply, not heroes (`unit_flags2 & 0x20`) and within `vd(x − m.x, y − m.y) ≤ 0x600`:
`near++`, and `busy++` if their `order_type()` is not a move (`is_move` @46f050: 1, 2, 3, 4, 18, 19, 21), not NONE and
not GUARD. True iff `near ≠ 0 && near/2 ≤ busy`.

Attack-move start: `add_move_facing_order` with kind 2 and QUEUE_NEW re-adds a current ATTACK (non-air target, or an
air target the unit can hit: obj mask ANTI_AIR) in front with `add_attack_order(QUEUE_FIRST, mandatory 0, explicit 0)`
(orders-spec §4.2, ✅).

### 3.5 `Unit::do_attack(order)` @5f1b80 ✅

`a` = the AttackOrder (vslot 0x50).

1. **Aircraft carrier** (`is(AIRCRAFTCARRIER = 351, strict 1)`): `T` dead → `kill_current_order(0)`; return. Only when
   `frame & 15 == 0` (global, not staggered): if `T.is_build` and `!a.mandatory` → return. Walk the carried chain
   starting at `(inside_down +0x28, inside_down_who +0x3e)` of the carrier and following the same two fields of each
   carried object: `add_strafe_order(plane, ox, whom, carrier.o, carrier.who, 1, QUEUE_NEW, a.mandatory)`. The carrier
   itself never fights. Return.
2. **No attack value** (`type.attack (+0x1e8) == 0`):
   - **Transport** (`is(TRANSPORTBARGE = 320, 0)`): `C` = `objects[inside_down_who][inside_down]` (no check that the
     transport carries anything ❓). If `!Object::valid_target(C, ox, whom, null)` → kill the order. Else
     `find_attack_pos(C, ox, whom, 0, &px, &py, transport.x, transport.y, 0)`; if 0, again with last argument 1; if
     still 0, `(px, py) = T.position`. `add_move_order(px, py, kind 1, 0, QUEUE_FIRST, explicit 1)` (the attack stays
     queued behind). Return.
   - **Not in a group** (`group < 0`): fall through to step 3 (`fight` handles it).
   - **Group member** (supply wagons, workers, …): stand-off distance
     `r = GroupData::get_max_range(is_worker ? 1 : 0)` (max of `max_range()` over members with an attack, siege
     members only for argument 1, non-siege only for 0; non-building groups skip invalid/off-map/plane members):
     - `r ≠ 0`: `D = (r + 2)·192`, `+192` if supply (`unit_flags2 & 0x40`), `+192` if worker (types 50–53);
     - `r == 0`: `D = (get_max_range(1) + 1)·192`, `+384` if supply, `+384` if worker.
     Target must be valid and alive (else kill). If `T.is_build`: `D += (T.type.x_size + T.type.y_size)·48`.
     If `vd(T.x − x, T.y − y) ≤ D + 0x240` → kill the order (close enough, nothing to do). Else
     `P = project(find_angle(x − T.x, y − T.y), D + 192, T.x, T.y)` (on the line target→unit); save `group`, set it to
     −1, `UnitType::find_nearby_spot(type, P, &sx, &sy, 0, −1, 0, 0x55555555, FILTER_NOT_ME, o, who, 0, 0, −1, 0, −1)`,
     restore `group`. If found (returns 0), `WorldData::is_valid(sx, sy)` and
     `get_tregion(tcell(T.x), tcell(T.y)) == get_tregion(tcell(sx), tcell(sy))`: `kill_current_order(0)`,
     `add_move_order(sx, sy, 1, 0, QUEUE_FIRST, explicit 0)`; return. Otherwise kill the order.
3. **Mandatory attacks**: if the type is FIRE_WHILE_MOVING (`unit_flags & 0x200000`): `cavarch_o = ox`,
   `cavarch_who = whom`; if both ≥ 0 and `T` alive: `unit_masks2 |= UNIT2_KEEP_TARGET`, `cavarch_uid = T.uid`; else
   clear UNIT2_KEEP_TARGET. Mandatory attacks then go straight to the plain `fight` call of step 5 (they never use the
   stand-ground swap of step 4).
4. **Non-mandatory MELEE_AND_RANGED in STAND_GROUND** (`unit_flags & 0x400`, `get_combat_stance() == 2`):
   `type.max_range (+0x1fc) = type.second_max_range (+0x2d0)`; `fight(ox, whom, mandatory, 0, 0)`;
   `type.max_range = 0`; return. (All `k`-flag types have `max_range == 0` after loading — rules-spec §7 RANGE — so the
   restore is exact; the three such units are the Mahout line. The write is to the **shared type**: during the call
   every reader of this type sees a ranged unit, in particular `max_range()`, `is_in_range` and `find_attack_pos`. A
   port should pass the range as a parameter but must make every callee inside `fight` see the swapped value.)
5. `r = fight(ox, whom, a.mandatory, temporary 0, cavarch 0)`. If `r ≠ 0` and the type is MELEE_AND_RANGED:
   save `recharging`, set it to 0, `cavarch_fight()`, restore `recharging` (a Mahout that just landed a melee blow also
   fires its ranged attack at the cavarch target in the same frame).

**`fight` contract as seen from here** (🟡 for the internals, ✅ for the return): returns **1 only when an attack was
executed this call** (shot fired or melee damage dealt; `recharging` has just been set from `recharge()`), else 0.
Arguments: `(ox, whom, mandatory, temporary, cavarch)`; with `cavarch ≠ 0` it is the moving-fire mode used by
`cavarch_fight` (target from the cavarch cache, no order manipulation). Out-of-range handling is summarised in 3.9.

### 3.6 Range geometry

#### 3.6.1 `ObjectData::attack_dist(ox, whom, x, y)` @6488f0 ✅ (and `attack_dist(ox, whom)` @64c880 = with own position)

Shooter = `this` placed at `(x, y)`; target `T`.
1. `dx = |ucentre(x) − ucentre(T.x)|`, `dy = |ucentre(y) − ucentre(T.y)|`.
2. If the shooter object is a plane (vslot 0xc0) and its type lacks OBJMASK_MISSILE (0x8000000 in `obj_masks` +0x1e4):
   return `vd(dx, dy)` (centre to centre).
3. Target size: if `T.is_wall` (vslot 0x1c, true for buildings and walls): `dx = max(dx − T.x_size·96, 0)`,
   `dy = max(dy − T.y_size·96, 0)` (strict `>` test; footprint half-sizes in world units); else
   `r = T.type.block_radius (+0x240) + 24`, `dx = max(dx − r, 0)`, `dy = max(dy − r, 0)`.
4. Shooter size: if `this.is_wall`: subtract `this.x_size·96` / `this.y_size·96` the same way; else subtract
   `units[who][o].type.block_radius + 24` from both.
5. Return `vd(dx, dy)`.

So it is the gap between two axis-aligned squares (units: half-side `block_radius + 24`; buildings: their footprint),
measured between 48-cell centres, combined with the max+min²/2max norm.

#### 3.6.2 `ObjectData::is_in_range(ox, whom, x, y, _unused, slack, int* dist_out)` @6486b0 ✅

1. `T` dead → 0. If `T` is a unit (vslot 0x08) and not on the map → 0.
2. If the shooter's T cell at `(x, y)` is a TREE cell (`tdata[tcell(y)·tw + tcell(x)] & 0x30 == 0x30`) → 0.
3. `d = attack_dist(ox, whom, ucentre(x), ucentre(y))`; `*dist_out = d` if given.
4. **Melee** (raw `type.max_range == 0`): limit 246 (0xf6) if `is(HOPLITES = 132, 0)` (pike line 🟡) else 102 (0x66);
   in range iff `d ≤ limit`. `slack` is ignored.
5. **Ranged**: `mn = min_range()`. If `d < mn·192 − 6`: `a = this is a unit ? own type.big_radius (+0x244) : 0`,
   `b = T is a unit ? T.type.big_radius : 0`; if `a + d + b < mn·192 − 6` → 0 (too close). Then in range iff
   `d + (slack ? 144 : 0) ≤ max_range()·192 + 6`.

The 5th argument is never read (callers pass leftover registers). Wrappers:
- `is_in_range(ox, whom, _unused, slack)` @648d70 = 6486b0 with the object's own position, `dist_out = null`.
- `is_in_range(tx, ty)` @64e470 = 64e4a0 with the own position. `is_in_range(tx, ty, sx, sy)` @64e4a0 (attack-ground):
  types with OBJMASK_MISSILE that are inside something (`get_inside(0) ≥ 0`) → 1. `d = vd(tx − sx, ty − sy)` (no
  snapping); units: `d = max(d − own big_radius, 0)`; buildings/walls: `d = max(d − (x_size + y_size)·48, 0)`.
  Ranged: 0 if `d < min_range()·192 − 6` or `d > max_range()·192 + 6`, else 1. Melee: `d ≤ 102`.

`min_range()`/`max_range()` are the virtual getters with leader/tech bonuses (UnitData @6100b0/@60fe90; `max_range()`
returns 0 whenever the raw type value is 0). The melee/ranged decision always uses the raw `type.max_range`.

### 3.7 `Unit::check_target_path(TargetOrder* act)` @5e22d0 ✅

Called only from `work` (orders-spec §5): every 16 frames (staggered) while the front order is a move-family order and
the action behind it is targeted and not AWAIT_BOARD/GUARD, and only for the group order's leader if the front is a
group order. Returns 1 when it changed the order list. `t = act.type`, `T = objects[act.whom][act.ox]`.

1. `act.ox < 0` → 0. `!T.is_seen(who, 0)` → 0 (an invisible target keeps the old plan; nothing is dropped).
2. **CAST_SPELL (14)**: if `T` dead or `!SpellTypeData::is_valid_target(spell(act.spell), who, ox, whom, 0)` →
   `repath`, `kill_current_order(0)`, 1. Else if `T` is a unit and `T.is_moving()` → `repath`, 1. Else 0.
3. BOARD_SHIP (8) → 0. REPAIR (13) of a wall/building → 0.
4. **Flank shortcut (unit targets)**: if `T` is a unit: `a = find_angle(T.x − x, T.y − y)`;
   `d = T.angle − (a − 0x80000000)` (u32: how far T's facing is from "looking at me"). Front cone: `d < 0x2aaaaaaa`
   or `flanking(d) == 0` (i.e. `d > 0xd5555555`) — ±60°. If `T` is in front, or flanked (`flanking` @92cfe0: 1 =
   rear ±45°, 2 = side) but **not moving**: in range from the current position (4-arg, slack 0) → `repath`, 1;
   otherwise **0** (keep walking; the planned end point is not examined). Only a flanked *and* moving unit target
   (one running away or across) continues to step 5. Buildings/walls always continue.
5. `T` not on the map → `repath`, 1.
6. GARRISON (26): `adjacent_to(ox, whom)` (vslot 0x170) → `repath`, 1.
7. If `is_in_range(ox, whom, orders_x, orders_y, _, 0)` (from the planned end point, 3.6.2) → 0.
8. Not reachable from the plan:
   - **non-attack actions**: FOLLOW (11) of a unit: if `vd(T.x − m.x, T.y − m.y) > 0x900` where `m` is the front move
     (`get_order()->get_move_order()`, its `x/y`) → `repath`, 1. Else 0.
   - **attack actions** (`is_attack`):
     - raw `type.max_range == 0 && path.length == 1` → 0 (a melee unit on its last waypoint is not re-planned);
     - `T.is_wall` → 0;
     - **group order with group** (front `is_group` and `group ≥ 0`): if `Group::is_moving_to()` (every valid on-map
       member has an empty list or a MOVE_TO/GROUP_MOVE front): `repath`; if `act.mandatory ≥ 1` and `T` is a map unit:
       `Group::kill_next_order()` (each member `kill_current_order(0)`), then
       `Group::action_attack(act.ox, act.whom, 1, QUEUE_FIRST, 0)`. Else (not moving-to): kill front orders while the
       front is a move (`is_move`), stopping when the list is empty. Return 1 in all cases.
     - **otherwise**: `repath`; `find_attack_pos(ox, whom, 1, &px, &py, 0)` (from the current position). Result 0 → 1
       (no move added). Else, **defensive leash**: if `get_combat_stance() == DEFENSIVE`, `!act.mandatory`,
       `!(act.flags & 4)`, `act.defensive`, and `def_x, def_y ≥ 0`: `L = max(max_range(), unit_defensive_respond_range)`
       (compared as ×192); if `vd(x − def_x, y − def_y) ≥ L·192` (the **unit's** distance from its defend point) →
       `find_new_target(0, 0)`, 1. Otherwise `add_move_order(px, py, kind 1, 0, QUEUE_FIRST, explicit 0)`, 1.

`Unit::repath` (movement-spec §3.4) only removes move orders from the front; the attack behind stays.

### 3.8 `Unit::find_attack_pos`

`find_attack_pos(ox, whom, p3, Coord* ox_out, Coord* oy_out, fx, fy, p8)` @601280 ✅;
`find_attack_pos(ox, whom, p3, out_x, out_y, p8)` @602e60 = the same with `(fx, fy)` = the unit's position.
`(fx, fy)` is the point the search is made *from* (the unit, a transport, or the group's future position); `p3 ≠ 0`
selects FILTER_CAN_COLLIDE (5) instead of FILTER_NOT_ME (3) for `find_nearby_spot` (with `o, who ≥ 0` both take the
same collision path there, so `p3` has no effect on results 🟡); `p8 ≠ 0` makes `find_nearby_spot` accept spots
without unit-collision tests (its 12th argument). Returns 1 with a position, or 0 (the outputs are still written, with
the target position, in most failure paths).

Callers and arguments: `Group::action_attack` (leader, `p3 = 1`, `(fx, fy)` = leader position or queued location,
`p8 = 1`); `do_attack` transports (carried unit, `p3 = 0`, transport position, `p8 = 0` then 1);
`check_target_path` (`p3 = 1`, own position, `p8 = 0`); `fight` and `target_opportunity` (`p3 = 0`, own position,
`p8 = 0`).

#### 3.8.1 Prelude

1. **Water unit with cargo**: if `inside_down ≥ 0` and `type.domain == 1`, return
   `find_attack_pos(cargo = units[inside_down_who][inside_down], same arguments)`.
2. Locals: `restore = 0`, `far = 0`, `mand = 0`, `region = −1`, `projected = 0`.
   `act_type = get_activity()` type, or 0 (`UnitData::get_activity` @608370: the first order from the front that is not
   `is_move_attack`, or an ATTACK_TO/GROUP_ATTACK_TO; only used as "== GUARD").
3. `siege_bld = type.is_siege() && (unit_masks & UNIT_AI_CONTROL) && T.is_build`.
4. If the unit is on the map, `!siege_bld`, and `is_in_range(ox, whom, fx, fy, _, 0)` → `out = (fx, fy)`, return 1.
5. `far_dist = max(0x600, (max_range() + 4)·192)`.
6. If the order list is non-empty and the front order is ATTACK (10): `mand = front.mandatory`; and if
   `type.domain ≠ 2` and `type.domain ≠ T.type.domain`: `region = get_tregion(tcell(x), tcell(y))` (own region, passed
   to `find_nearby_spot` to keep the spot reachable).
7. If `unit_masks & UNIT_CAN_TRANSPORT`: clear it, `restore = 1`. **Every exit below sets it back when `restore`**,
   except the one marked ⚠ in 3.8.3.
8. `ang_from = find_angle(fx − T.x, fy − T.y)` (from the target towards the search origin).
9. `d = attack_dist(ox, whom, x, y)` — from the unit's **current** position, not from `(fx, fy)`.

#### 3.8.2 Ring radius `R`

- **Far** — `T` is a unit and `d > (max_range() + 8)·192`: `far = 1`, `R = (max_range() + 2)·192`.
- **Ranged** — otherwise, if `type.role & ROLE_RANGED (0x400)`: `mn` = min range (raw `type.min_range` +0x1f8 when
  the getter is not overridden, else `min_range()`).
  - `d < mn·192 − 6 && !siege_bld` (too close): `(fx, fy) = project(angle (+0x50, the unit's facing),
    (mn + 1)·192 − d, fx, fy)` — the origin is moved along the **unit's own facing**, not away from the target (🟡
    intent); `R = mn·192 + 0x90`.
  - else if `d > max_range()·192 − 6 || siege_bld`: `R = max_range()·192 − 0x60`; if `max_range() < 10`:
    `R += type.big_radius − 0x30`; if `T` is a unit: `R −= T.type.big_radius`; `R = max(R, 0xc0)`.
  - else `R = d`.
- **Melee** — not ROLE_RANGED: `R = 0x30`. If `T` is a unit and not moving, the ring is not used at all: see 3.8.3.

Then `filter = p3 ? 5 : 3` and the branch on the target kind: `T.is_wall` (buildings and walls) → 3.8.4 (but if
`act_type == GUARD`: `out = (fx, fy)`, return 0); units → 3.8.5.

#### 3.8.3 Melee against a standing unit

`r = find_melee_pos(ox, whom, _, &out_x, &out_y, x, y, _)` (3.8.7). `r ≠ 0` → return 1 (restore applied).
`r == 0`: `out = T.position`. If `mand` and `vd(T.x − x, T.y − y) ≥ far_dist`: `out = (wcell(T.x)·768 + 384 +
(x mod 768) − 384, wcell(T.y)·768 + 384 + (y mod 768) − 384)` (the target's W cell, at the unit's offset inside its
own W cell; `mod` is C truncation), `WorldData::restrict(out)` (clamp to the map), return 1 — ⚠ **without** restoring
UNIT_CAN_TRANSPORT (bug-compatible). Otherwise: if `order_type() == ATTACK`, **the front attack's `mandatory` is set to
0**; return 0.

#### 3.8.4 Buildings and walls: perimeter walk (the `game_random` site)

Let `xs, ys` = `T.type.x_size, y_size` (tiles), `hx = xs·96`, `hy = ys·96` (half-sizes), `(tx, ty)` = `T` position.

**Start side.** `ex = |fx − tx| − hx`, `ey = |fy − ty| − hy`, `diag = ex > 0 && ey > 0`. Sides are numbered
clockwise from the NW corner: 1 NW, 2 N edge, 3 NE, 4 E edge, 5 SE, 6 S edge, 7 SW, 8 W edge (+x east, +y south).
- `ex > ey`: diag → `fx > tx` ? (`fy > ty` ? 5 : 3) : (`fy > ty` ? 7 : 1); else `fx > tx` ? 4 : 8.
- `ex ≤ ey`: diag → `fy > ty` ? (`fx ≤ tx` ? 7 : 5) : (`fx > tx` ? 3 : 1); else `fy > ty` ? 6 : 2.

**Step sizes.** Melee (raw `type.max_range == 0`): `step = 0x20`, `astep = 0x20000000`. Ranged: `step = 0xc0` if
own `big_radius > 0x30` and `xs > 2` and `ys > 2`; else `0x40` if own `big_radius > 0x18` and `xs > 1` and `ys > 1`;
else `0x20`. `astep = min(u32(s32(0x40000000) / max_range()) / (0xc0 / step), 0x20000000)` (first division signed,
the other two unsigned). Then `n = 0x40000000 / astep` (unsigned), `nmax = n − 1`, `ainc = 0x40000000 / n` (signed).
A corner arc has the corner indices `k = 1 … nmax`, angle `base + k·ainc`, with `base` 0xc0000000 (NW: W→N),
0 (NE: N→E), 0x40000000 (SE: E→S), 0x80000000 (SW: S→W).

**Two walkers** `u = 0` (counter-clockwise, decreasing side numbers) and `u = 1` (clockwise), each with `side[u]`,
`wx[u], wy[u]`, `k[u]`, `base[u]`. Both start on the start side:
- corner start: `base` of that corner, `(wx, wy)` = the corner point (`tx ∓ hx`, `ty ∓ hy`: NW (−,−), NE (+,−),
  SE (+,+), SW (−,+)), `k[0] = k[1] = nmax/2 + 1` (C division);
- edge start: N `(tx, ty − hy − R)`, E `(tx + hx + R, ty)`, S `(tx, ty + hy + R)`, W `(tx − hx − R, ty)`; `k[]` is
  left uninitialised (never read before it is set).

Search state: `best = −1`, `limit = 100`, `cnt = 4`, `u = 0`.

**Loop** (✅ @601fb3–@60278f):
1. Candidate of walker `u`: on a corner, `(cx, cy) = project(k[u]·ainc + base[u], R, wx[u], wy[u])`; on an edge,
   `(wx[u], wy[u])`. Snap: `cx = ucentre(cx)`, `cy = ucentre(cy)`.
2. Valid iff `!invalid_loc(tcell(cx), tcell(cy), 1, 0, 0, 0, 1)`, the T cell is not BLOCKED (`tdata & 0x4000`),
   `!Objects::find_collision(cx, cy, o, who, 0)` and `!Objects::find_ordered_collision(cx, cy, o, who)`.
3. If valid: **`r = game_random.get(0, 0xffff) % 192`** (site @602107), `score = vd(cx − fx, cy − fy) + r`. If
   `best < 0 || score < best`: `best = score`, `bx, by = cx, cy`, and `limit = min(limit, R > 0x300 ? cnt + 11 : cnt)`.
4. `u ^= 1`; advance walker `u`:
   - on a corner: `k[u] += u ? +1 : −1`; if `k[u] > nmax || k[u] < 1`: `side[u] += u ? +1 : −1` (wrap 0 → 8) and the
     new edge starts at its end next to the corner just left: N `(tx + (u ? −hx : +hx), ty − hy − R)`,
     E `(tx + hx + R, ty + (u ? −hy : +hy))`, S `(tx + (u ? +hx : −hx), ty + hy + R)`,
     W `(tx − hx − R, ty + (u ? +hy : −hy))`.
   - on an edge: move by `step` (N: `wx += u ? +step : −step`; E: `wy += u ? +step : −step`; S: `wx += u ? −step :
     +step`; W: `wy += u ? −step : +step`); if it overshoots the edge (`|wx − tx| > hx` for N/S, `|wy − ty| > hy` for
     E/W): `side[u] += u ? +1 : −1` (wrap 9 → 1), `base[u]` and `(wx, wy)` of the new corner, `k[u] = u ? 1 : nmax`.
     The overshooting point itself is never evaluated.
5. `cnt++`; continue while `cnt − 4 < limit`.

So at most 100 positions are evaluated, alternating between the two walkers outward from the start point; after the
first valid one (at evaluation `j`, 1-based, `cnt = j + 3`) the search stops after evaluation `j + 3`
(`R ≤ 0x300`) or `j + 14` (`R > 0x300`). The surrounding `do … while (--pass)` runs exactly once (its counter is 1 and
is only ever re-set to 1).

**Result**: restore; `best ≥ 0` → `out = (bx, by)`, return 1. Else the fallback 3.8.6.

#### 3.8.5 Units: lead, hold, or ring

`(cx, cy) = T.position`. Unless `unit_masks & UNIT_AI_CONTROL`, and if (`type.domain == 2` or the domains match),
`T` has a non-empty order list whose front `is_move`, and the unit is ranged (raw `type.max_range ≠ 0`):
`d = T.angle − find_angle(T.x − x, T.y − y) + 0x80000000` (u32).
- `d < 0x2aaaaaaa` or `d > 0xd5555555` (T moving towards us ±60°) → **hold check**: if `T.max_range() <
  max_range()` or `unit_masks2 & UNIT2_MARINE_ENTRENCH`, and `vd(T.x − x, T.y − y) < (max_range() + 4)·192` →
  `out = own position`, restore, return 1 (an out-ranging unit lets the target walk into range).
- `flanking(d) == 1` (T moving away, rear ±45°) → **lead**: `s1 = get_speed(x, y, 0)`, `s2 = T.get_speed(T.x, T.y, 0)`,
  `lead = ((vd(T.x − x, T.y − y) / s1)·s2·3) / 2` (signed, truncating); `P = project(T.angle, lead, T.x, T.y)`;
  if `WorldData::is_valid(P)`: `(cx, cy) = P`, `projected = 1`.
- side (`flanking == 2`) → no adjustment.

Ring: `rin = T.type.big_radius + type.big_radius + R`, `rmax = 0`, `rstep = 0`; if `rin > 0x240`: `rmax = rin`,
`rstep = 0x60`, `rin −= 0xc0`. `find_nearby_spot(type, cx, cy, &sx, &sy, rin, rmax, rstep, ang_from, filter, o, who,
p8, 0, −1, 0, region)`. Not found → 3.8.6. Found: restore; if `projected || far` → `out = (sx, sy)`, return 1;
else if `is_in_range(ox, whom, sx, sy, _, 0)` → `out = (sx, sy)`, return 1; else 3.8.6.

(`find_nearby_spot` @61de70 is a shared primitive — rings of 16 directions from `rin` to `rmax` in steps of `rstep`,
default `rmax = rin + 4·big_radius` (or `rin + 0x240`) and `rstep = (rmax − rin)/8`, terrain, region and collision
tests. It draws no `game_random`. It is ported in engine/sim/search.cpp (movement-spec §11.1, sim-implementation).)

#### 3.8.6 Fallback

`out = T.position`. If `!mand` or `vd(T.x − x, T.y − y) < far_dist` → return 0 (restore). Otherwise (a mandatory attack
on a distant target): `find_nearby_spot(type, T.position, &out, xs_T·192, xs_T·768, 0, 0x55555555, FILTER_NOT_ME, o,
who, 0, 0, −1, 0, region)` with `xs_T = T.type.x_size`. If that fails (`out` is then `T.position`): `out` = the target's
W cell at the unit's offset (as in 3.8.3), `restrict`; if `type.domain == 0` and the T cell of `out` is water
(`tdata & 0x30 == 0x20`) → return 0. Then (found or not): restore; return 0 if `type.domain == 1`, 0 if
`type.domain == 0 && T.type.domain == 1`, else 1.

#### 3.8.7 `Unit::find_melee_pos(ox, whom, _, &ox, &oy, x, y, _)` @6010b0 and `find_open_slots` @600e30 ✅

`find_open_slots(ox, whom, &slots)`: `a = type.new_block_radius (+0x248)`, `b = T.type.new_block_radius`,
`Rc = a + 1 + b` (+3 if `is(HOPLITES, 0)`), `step = 2a + 1` (U cells). Around the target's U cell `(tu, tv)` walk the
square of half-side `Rc`: start at `(tu − Rc, tv + Rc)` (SW corner), then directions N, E, S, W (`orthog_x/y[1..4]`),
`step` cells at a time. After each move, if `|u − tu| > Rc` clamp `u` to `tu ± Rc` (sign of the overshoot), turn to the
next direction and immediately move one step in it; same for `v`. Stop when the fourth direction overshoots. Each
visited cell `(u, v)` inside the map (`0 ≤ u < 16·world.xs`, `0 ≤ v < 16·world.ys`) is a slot if
`!invalid_loc(u >> 2, v >> 2, 0, 0, 0, 0, 1)` and no `find_collision` / `find_ordered_collision` at
`(48u + 24, 48v + 24)`; slots are appended in visit order. Returns 1 if any slot was found.

`find_melee_pos`: no slot → `out = T.position`, `T.full += 5` (cap 30), return 0. Else the slot with the smallest
`vd(slot − (x, y))` (first minimum, strict `<`, initial 99 999 999), return 1. No `game_random`.

### 3.9 Chase versus hold, per stance

Summary of how an attacker reacts when its target is out of range; the decisions are in `fight` (§4.3,
🟡 from the decompile, leash constant ✅ from disassembly @5fe212) and `check_target_path` (3.7, ✅).

- **Approach move.** `fight`, when out of range and allowed to chase, calls `find_attack_pos(ox, whom, 0, &p, 0)` and
  queues `add_move_order(p, kind 1, 0, QUEUE_FIRST, 0)` in front of the attack (toggling flag 0x10 on the action, and
  re-running `work` the first time). `check_target_path` re-plans that move every 16 frames.
- **Give-up conditions** in `fight` (non-mandatory attacks only, `temporary == 0`): STAND_GROUND without `on_duty()`
  (`on_duty` @5fff70: role COMBAT and activity ATTACK_TO, GUARD, GROUP_PATROL, PATROL or GROUP_ATTACK_TO), or
  UNIT_ENTRENCHED without UNIT2_MARINE_ENTRENCH, or `find_attack_pos` failing. Give-up = `find_new_target(0, 0)`; if the
  front is still the ATTACK on the same target: remember `def` if `defensive` and valid, `kill_current_order(0)`, and if
  the list is now empty and `def` was valid, `add_move_order(def, QUEUE_NEW)` with flag 0x08 (walk home). Then, if the
  front is an ATTACK and the unit is not recharging, `unit_masks2 |= UNIT2_HOLD_ANIM`. For a non-mandatory attack by a
  PACKER, `fight` first replaces the target by `find_new_target(&w, 0)` and skips the whole out-of-range block when that
  finds nothing. Temporary group attacks (`temporary ≠ 0`) never chase.
- **DEFENSIVE leash**: non-mandatory, non-explicit attacks with `defensive` set give up (as above) when the unit is
  `≥ max(max_range(), unit_defensive_respond_range = 4)·192` from its defend point — checked in `fight` before queueing
  a chase move and in `check_target_path` during the approach. Note it uses UNIT_DEFENSIVE_RESPOND_RANGE even for ships
  (SHIP_DEFENSIVE_RESPOND_RANGE = 8 is not used here).
- **AGGRESSIVE (0)**: no limit in these functions; chases while `find_attack_pos` succeeds.
- **STAND_GROUND (2)**: holds (non-mandatory, off duty); MELEE_AND_RANGED units use their ranged range (3.5 step 4).
  Mandatory (player) attacks still chase.
- **RAID (3), RAZE (4)**: no special case in any function of this section (❓ target choice only).
- **HOLD_FIRE (5)**: `do_move` resets `cavarch_o = −1` and `do_group_move` skips moving fire; acquisition is elsewhere.
- **Artillery**: an unpacked PACKER with `ever_in_range` drops the order when the target leaves range (`fight`).
- **Out-ranging units** facing an approaching target hold position (3.8.5).

### 3.10 Per-unit combat state helpers

#### `Unit::set_attacking(whom)` @5ff5b0 ✅ (from `fight`)
`leaders[who].treaties[whom] |= 1`; `leaders[whom].treaties[leaders[who].who] |= 1` (Leader +0x94 `int[8]`; bit 0
marks that the two players have fought 🟡); `flags |= ATTACKING`; if bit `whom` of `visible` is clear: set it, and if
`WorldData::is_seen(x/384, y/384 (div_3_table[c >> 7]), whom)` is false also call `update_local_seen()` (vslot 0x164).

#### `Unit::set_attack(ox, whom)` @5fce70 ✅ (from `fight`, `resolve_unit_collision`, `do_attack_ground`, `do_air_attack_ground`)
For each guy `i < guy_mark (+0xb5)`: guy +0x8e = ox (s16), guy +0x9f = whom (s8). If the unit's graphics piece type
(`GraphicPieces::get_type(get_gpiece())`) has pivot restrictions, raw `type.max_range ≠ 0`, `ox, whom ≥ 0`, and the
target is (a unit on the map) or a wall/building: `Guy::set_all_pivots` on every guy; returns the last result, else 0.
Note: reads asset data (graphic pieces) in the simulation.

#### `Unit::set_in_danger(flag)` @5fcfb0 ✅
With `flag == 0` on a non-captain (`o_up ≥ 0`), restart on its captain. Then for the unit and down the follower chain
(`o_down`, stopping at a dead follower): every guy's flags +0x9a `|= 0x20`, `unit_masks |= UNIT_IN_COMBAT`. Called by
`work` each frame for an ATTACK action (attacker, and the target if it is a unit) and by `Object::take_damage`.

#### `Unit::change_target(oox, owhom, nox, nwhom)` @5e36c0 ✅ (only from `do_move`'s melee-captain retarget)
For the unit and down its `o_down` chain (via vslot 0xa8, stopping at a dead one): if `update_action()` is a targeted
order on `(oox, owhom)`, set it to `(nox, nwhom)` and its `uid` to the new target's, then kill front orders until the
front is targeted (drops the approach moves).

#### `Unit::cavarch_fight()` @5ff4b0 ✅
`(o0, w0) = (cavarch_o, cavarch_who)`. MELEE_AND_RANGED: `type.max_range = second_max_range`. `fight(cavarch_o,
cavarch_who, mandatory = unit_masks2 & UNIT2_KEEP_TARGET (the value 0x100, not 1), 0, cavarch 1)`. MELEE_AND_RANGED:
`type.max_range = 0`. If `o0, w0 ≥ 0` and `fight` changed the cache: keep UNIT2_KEEP_TARGET only if the player is the
same and `objects[w0][o0].get_captain()` (vslot 0xe4) is the new `cavarch_o`; else clear it. If the cache is now < 0 and
`o0` was < 0 too: `cavarch_o = −2`.

Callers: `do_attack` (3.5 step 5); `do_move` step 1: FIRE_WHILE_MOVING types with `attack() ≠ 0`: if stance is
HOLD_FIRE, or the action is ATTACK and the type is not MELEE_AND_RANGED → `cavarch_o = −1` (no moving fire while
walking to an attack); else, if `cavarch_o < −1` only when `(o + frame) % 32 == 0`, `cavarch_fight()`;
`do_group_move`: same type/attack test, stance ≠ HOLD_FIRE and (action ≠ ATTACK or MELEE_AND_RANGED), same 32-frame
gate, before `move_step` (no reset).

### 3.11 RNG draws

All `game_random` draws in this section's call graph, in call order. Only one site is direct.

| Site | Function | Draw / use | Condition and count |
|---|---|---|---|
| @602107 ✅ | `Unit::find_attack_pos` @601280 | `get(0, 0xffff) % 192`, added to `vd(candidate − (fx, fy))` as a tie-break jitter | target is a building or wall (3.8.4); one draw per **valid** candidate (passes the 4 tests), in walker order; at most 100 positions evaluated, and the search stops 3 (or 14 when `R > 0x300`) evaluations after the first valid one. Zero draws when the unit is already in range from `(fx, fy)`, for unit targets, for GUARD activity, and for melee units against standing units |

Reached indirectly (§4, §8): `fight` @5fdcdb/@5fde6e; `do_move` @5f899d (via the approach moves);
`find_attack_pos` is reached from `Group::action_attack` (every attack command whose leader is out of range of a
building target), `check_target_path`, `do_attack` (transports; up to two calls), `fight`, `target_opportunity`.
No draws in: `add_attack_order`, `do_attack` itself, `check_target_path` itself, `find_melee_pos`/`find_open_slots`,
`find_nearby_spot`, `attack_dist`, `is_in_range`, `distribute_attack`, `do_group_attack(_to)`, `do_attack_to(_pause)`,
`is_attacking_near`, the 3.10 helpers (✅ against `re/analysis/xrefs_random.txt`).

### 3.12 Rules / data read

Type fields (offsets: rules-spec §6.4): `attack` 0x1e8; `min_range` 0x1f8; `max_range` 0x1fc (raw, decides
melee/ranged; **written** temporarily by `do_attack`/`cavarch_fight`); `second_max_range` 0x2d0; `domain` 0x218;
`obj_masks` 0x1e4 (OBJMASK_MISSILE 0x8000000); `unit_flags` 0x2b4 (MELEE_AND_RANGED 0x400, FIRE_WHILE_MOVING 0x200000,
HELICOPTER 0x20, SIEGE 0x20000); `unit_flags2` 0x2b8 (SUPPLY 0x40, SPECIAL 0x10, HERO 0x20, PACKER 4, RETARGETS 1);
`role` 0x2c8 (ROLE_RANGED 0x400, ROLE_COMBAT 0x10000); `x_size`/`y_size` 0x234/0x238; `block_radius` 0x240;
`big_radius` 0x244; `new_block_radius` 0x248; type index +4 (workers 50–53). Type classes via `is()`: AIRCRAFTCARRIER
351 (strict), TRANSPORTBARGE 320, HOPLITES 132, 58 (spy 🟡). Has-stance via type vslot 0x134.

Constants (`rules.xml`, struct Constants): `unit_respond_range` (+0x18) = **12**, `unit_defensive_respond_range`
(+0x1c) = **4** (tiles of 192).

Hard-coded: melee reach 102 (246 hoplites); range slack ±6, `slack` margin 144; front cone ±0x2aaaaaaa (60°), rear
cone ±0x20000000 around 0x80000000; `far_dist ≥ 0x600`; ring radius thresholds 0x240/0xc0/0x60/0x90/0x300; candidate
jitter `% 192`; perimeter limit 100 (+3 / +11); attack-move scan every 15 frames; group-pause 15 frames, radius 0x600;
FOLLOW re-plan 0x900; leash multiplier 192; `unit_respond_range · 576` cap; non-combatant stand-off `(r + 2)·192`
(+192/+384) and `+0x240` / `+192`.

World data: TData mask (TREE 0x30, WATER 0x20, BLOCKED 0x4000), `get_tregion`, `WorldData::is_valid` / `restrict`
(world-spec §1). Asset data: graphic-piece pivot restrictions (`set_attack`).

### 3.13 Corrections to existing specs

orders-spec:
1. §2.5 step 2: buildings are not given orders through `Build::add_attack_order` (it has no callers); the same three
   stores are inlined, always with `build_masks |= 4`.
2. §2.5 step 4: `find_attack_pos` draws `game_random` only for building/wall targets, once per *valid* perimeter
   candidate, capped (3.8.4, 3.11) — not "once per candidate cell" in general. It can also clear `mandatory` on the
   leader's front attack (3.8.3). The capture branch and the building-group branch return without `order_num++`.
3. §2.5 step 5 is incomplete: the "left alone" test requires `mandatory`, `length ≤ 2` and also skips when the front move
   already ends in range; non-mandatory group attacks retarget each member through `find_melee_target` (radius
   `min(dist + 192, unit_respond_range·576)`); unseen *building* targets get only a move (plus `check_capture` when in
   range); the packer branch casts UNPACK only when already in range and PACK only when out of range, and only when the
   target is seen (3.3.1 step 8).
4. §4.2 `add_attack_order`: also `in_range = ever_in_range = 0`, `flags &= ~0x80`; the defend point comes from the
   existing defensive attack or a flag-0x08 move before falling back to the unit's cell (3.2.1).
5. §5 `work`: every 32 frames, `visible = 0` only when ATTACKING is clear, but UNIT_IN_COMBAT is cleared
   unconditionally.
6. §5 `check_target_path` paragraph: the defensive leash measures the **unit's** distance from its defend point (not the
   target's), applies only to non-mandatory, non-explicit attacks with `defensive` set, and ends with
   `find_new_target` (no move added). An unseen target returns 0 (no repath). The "in range from the current position"
   shortcut applies to unit targets in front (±60°) or flanked-but-standing; buildings and fleeing units are checked
   against `orders_x/orders_y` only. Group orders are re-issued through `Group::action_attack` rather than
   `find_attack_pos` (3.7).
7. §6.3 step 2: transports try `find_attack_pos` twice (second with "ignore units"), the move is explicit; group
   non-combatants: exact distance formula in 3.5; a non-attacker that is not in a group goes on to `fight`.
8. §6.3 step 4: mandatory attacks skip the stand-ground range swap; the `cavarch_fight` follow-up is only for
   MELEE_AND_RANGED types. Open question 4 is resolved: the loader sets `max_range = 0` for every `k` type
   (rules-spec §7), so the reset is exact.

movement-spec:
9. §3.5 step 1: before the 32-frame gate, `do_move` resets `cavarch_o = −1` (and does not call `cavarch_fight`) when the
   stance is HOLD_FIRE or the action is ATTACK for a non-MELEE_AND_RANGED type.
10. §3.6: replace the outline by 3.7 (it does not "repath when invisible / out of a flank cone"; the flank cone selects
    which range test is used).
11. §13: `Unit::find_attack_pos` is now specified (3.8.4, 3.11).

### 3.14 Open questions

1. (Ported from the disassembly in engine/sim/search.cpp; see movement-spec §11.1.) `UnitType::find_nearby_spot`
   @61de70 needs its own full spec (direction order inside a ring, the meaning of the
   start-angle argument 0x55555555 / `ang_from`, region test, collision filters); 3.5, 3.8.5 and 3.8.6 depend on it.
2. `fight` internals (target re-acquisition, `poor_target`, follower adoption of the captain's target, the flag 0x10
   toggle and why `work` is re-run) — §4.3; 3.9 is from its decompile only.
3. The perimeter walk's "too close" origin shift follows the unit's facing, which moves a unit that faces its target
   *towards* it. Confirm in game whether ranged units with a minimum range back off or close in.
4. `do_attack` transport branch reads `inside_down` without checking it is ≥ 0; can a TRANSPORTBARGE without cargo
   hold an ATTACK order?
5. `do_group_attack` and `find_attack_pos` leave order objects out of the pool (`remove_current` without `give_obj`);
   check whether the pool is walked/checksummed (state-spec) before relying on it.
6. Leader `treaties[]` bit 0 meaning (set by `set_attacking`) and who reads it.
7. `UnitOrder.flags` 0x80 (cleared by `add_attack_order`) still has no known reader; 0x08 and 0x10 should be added to
   orders-spec §3.2.
8. `is(58)` in `ignore & 1` (spy?) and `is(132)` (HOPLITES list membership) — confirm which types the class lists
   contain.
9. Army word +0x2c and position +0x48/+0x4c used by `do_attack_to` (formation hold) are not yet named.

### Evidence (section 3)

| Function | Address | Files |
|---|---|---|
| `Unit::add_attack_order`, `Build::add_attack_order` | 5e5410, 622ce0 | `re/decomp/atk/`, `re/decomp/combat_B/asm/` |
| `Unit::do_attack` | 5f1b80 | `re/decomp/combat/`, `combat_B/asm/` |
| `Unit::check_target_path`, `flanking` | 5e22d0, 92cfe0 | `combat_B/asm/`, `move_guy/asm/` |
| `Unit::find_attack_pos` (2), `find_melee_pos`, `find_open_slots` | 601280, 602e60, 6010b0, 600e30 | `combat_B/asm/`, `combat_B/` |
| `ObjectData::attack_dist` (2), `is_in_range` (4) | 6488f0, 64c880, 6486b0, 648d70, 64e470, 64e4a0 | `combat_B/asm/` |
| `Unit::do_attack_to`, `do_attack_to_pause`, `Group::is_attacking_near`, `is_move` | 5f2320, 5f22a0, 710e40, 46f050 | `combat_B/asm/`, `combat_B/` |
| `Unit::do_group_attack`, `do_group_attack_to`, `Group::action_attack`, `distribute_attack` | 5e75a0, 5e74e0, 712490, 713390 | `combat_B/asm/`, `atk/`, `move_group/` |
| `UnitData::find_def_pos`, `get_activity`, `GroupData::get_max_range`, `Group::is_moving_to`, `kill_next_order`, `AttackOrder::operator=` | 608560, 608370, 711080, 713bd0, 713d90, 7304c0 | `combat_B/` |
| `Unit::set_attack`, `set_attacking`, `set_in_danger`, `change_target`, `cavarch_fight`, `on_duty` | 5fce70, 5ff5b0, 5fcfb0, 5e36c0, 5ff4b0, 5fff70 | `combat/`, `combat_B/asm/` |
| `Unit::fight` (leash, give-up, return 1) | 5fd4d0 (@5fe212) | `combat/`, `combat_B/asm/` |
| Callers of the above | — | `re/decomp/combat_B/callers.txt` |
| ATTACKING / `targeted` writers | — | `re/decomp/combat_B/insn_attacking_flag.txt` |
| Constants | rules.xml lines 26–28 | `UNIT_RESPOND_RANGE` 12, `UNIT_DEFENSIVE_RESPOND_RANGE` 4 |

## 4. Firing: from "in range" to damage being applied

Paraphrased pseudocode, not decompiled text. Field names are PDB names
(`UnitData`, `GuyData`, `AmmoData`, `BuildData`, `GameDataPackage`, `GraphicEvent`). In the decompiles `Unit` fields
show up as `field_0xNN`, and the offsets are the same as the `UnitData` offsets (types.txt).
Confidence: ✅ checked against decompile and asm, 🟡 decompile only or partly inferred, ❓ open.

New decompiles and listings: `re/decomp/combat_C/` (jobs `re/jobs/dc_combat_C_*.txt`): `asm/` (Ammo::init, inc_time,
do_damage, check_hit, hit_target, init_crash, Object::fire_ammo, Unit::fight, Build::do_attack, UnitData::recharge),
Unit::set_anim, Unit::inc_time / execute_events, Wall::inc_time, Guy::move, GraphicEvents::verify_load /
get_event_group, GraphicPieces::get_position / get_gpiece_type / verify_ammo_flags, AnimationPacket::get_game_frames /
get_anim_time, Objects::find_builds, Vector<int>::length / cross, BuildData::get_shot / most_shots,
ObjectData::count_inside, UnitData::in_supply, the type predicates; plus `callers1.txt`, `xrefs_gravz.txt`,
`insn_guyflags*.txt`.

### 4.0 Summary of the non-obvious findings

1. **Most ranged units do not fire from `Unit::fight` at all.** Their projectiles come from **`RELEASEEVENT`s in
   `Data/unit_graphics.xml`**. Each event is placed at a frame of the attack animation, and the guy's animation clock
   triggers it (`Guy::execute_events` → `GraphicEvents::execute_game_events` → `Objects::add_ammo`). `Unit::fight`
   only picks the animation and starts the recharge. Whether a type fires "directly" is decided **at graphics load**:
   `GraphicEvents::init_unit_events` rewrites `UnitTypeData.fire_proj` (+0x2cc). The rules loader sets it to 1, and
   it ends up 0 if the art has release events, an ammo gpiece otherwise (§4.2). In the shipped data 233 attacking types
   use events, 61 fire directly (nearly all melee units, plus Armed Merchant, Fur Trapper, fire rafts and fireships,
   Aircraft Carrier) and 4 use `INSTANTHIT`. No type mixes the two across its civ/age variants.
2. **Melee damage is applied immediately inside `Unit::fight`** (`Object::do_damage` once per living squad guy), on
   the frame the recharge allows it. It does not wait for an animation frame. The 3 melee types with release events
   (Mahouts) deal their damage through ammo instead.
3. Simulation-relevant data that comes from the art and not from the rules: release-event frame times
   (`starttime`/67 ms), animation frame counts (`end_time`), which attack animations exist (variant fallback), and
   the muzzle offset (a precomputed, animated node position added to the ammo start point, which changes the flight
   time). See §4.17.
4. Projectile hit/miss: shots at **air** units roll against `fly_high`/`fly_low` at launch. Shots at **ground units**
   scatter (radius from `to_hit`/`attenuate`/`target_radius`) and then test the actual landing point against
   `target_size` (`Ammo::hit_target` / `check_hit`). A miss keeps flying on its arc until it hits the terrain, then
   checks again. Splash (`splash_area`) damages every non-allied object (players 0–7 only) in a W-cell spiral, with a
   linear falloff.
5. **Building fire rate scales with garrison**: one `fire_ammo` per volley (`ammo_per_att` arrows, each from a random
   point of the footprint), then `recharging = recharge / garrison_arrows`. Air-defence buildings (types 523–525) never
   fire through `fire_ammo`. `Wall::inc_time` runs a turret animation clock in `BuildData.recharging`, and their
   events fire.
6. `Guy::set_anim(CHAR_ATTACKWALK)` writes simulation state: it zeroes the unit's `recharging` when it is below 6
   (cavalry archers).

### 4.1 Data used

**Unit** (`UnitData`): `angle` +0x50, `unit_masks` +0x68, `unit_masks2` +0x6c, `cavarch_o` +0xa2 (short),
`cavarch_uid` +0xa6, `cavarch_who` +0xa8, `waiting` +0xad, `recharging` +0xae (u8, decremented once per frame at the
top of `Unit::process`, frame-spec §9), `guy_mark` +0xb5, `guys` +0xe4 (`count` +0xe8, `data` +0xf4),
`collide_frame` +0x48. Squad guys are `[0, guy_mark)`, crew guys `[squad_size, count)` (movement-spec §9.3).

**Guy** (`GuyData`): `x/y/z` +0xc..0x14 (raw, not XORed), `angle` +0x18, `turret_angles[4]` +0x20, `des_x/des_y` +0x5c/+0x60,
`des_angle` +0x64, `cur_time` +0x74, `end_time` +0x78, `last_time` +0x7c, `avg_speed` +0x84, `gpiece` +0x88,
`ox` +0x8e, `off_x/off_y` +0x92/+0x94, `node_flags` +0x96, `guy_flags` +0x9a, `cur_anim` +0x9c, `stopped` +0x9d,
**`hold_attack` +0x9e**, `whom` +0x9f, **`queued_attack` +0xa0**, `guy_num` +0xa2.
`hold_attack`/`queued_attack` encoding: 0 = none, 1 = "random attack variant", ≥ 2 = that exact `UnitAnim`.

**UnitAnimCat** @af4370 (.rdata, 38 ints, read from the exe): anims 0–6 → 0 (DEFAULT), 7–9 → 8 (WALK), 10 ATTACKWALK →
10 (its own category), **11–14 (ATTACK1/2/3/ATTACKSPECIAL) → 12**, 15–20 → 15, 21..37 → themselves except 26/28/30/32 → 8.
"Attack category" below means `UnitAnimCat[a] == 12`. ✅

**Ammo** (`AmmoData`, 0x6c; pool `Objects.ammo_objs`: count +0x120, data +0x12c; running counter +0x1f8):
`flags` +4: bits 0x01|0x02 = slot in use (0x02 = in flight, 0x01 alone = stuck in the ground, §4.9). The other bits are
not the graphics `AmmoFlags`: **0x04** = ground-unit target, verify the hit on arrival; **0x08** = missed, still
falling; **0x10** = deals no damage (dodged, or graphics flag DO_NO_DAMAGE). `rolling` +5, `accuracy` +6 (short),
`gpiece` +8, `sx,sy,sz` +0xc.., `ex,ey,ez` +0x18.., `cur_time` +0x24, `total_time` +0x28, `angle` +0x2c,
`splash_area` +0x30, `index` +0x34 (own slot), `graph_index` +0x38, `who/o` +0x3c/+0x40 (shooter), `num_guys` +0x44,
`whom/ox` +0x48/+0x4c (target), `traj` +0x50 (1 ballistic, 2 spline), `v1z` +0x54, `dx` +0x58, `bank_dx/dy` +0x5c/+0x60,
`start_roll_angle` +0x64, `ammo_path` +0x68 (Spline*). AmmoOut `shooter_vis` +0x70 is presentation.

**Graphics ammo flags** (`GraphicPieces.ammo_flags[gpiece − first_ammo_piece]`, from the `<AMMO>` attributes in
`effects_graphics.xml`): PUNCTURE 1 (`puncture`), PUFF 2, EXPLODE 4 (`explosive`), MISSILE 8 (`missile`), FLAME 0x10,
PULSEWAVE 0x20, FLAK 0x40, DO_NO_DAMAGE 0x80 (`do_damage="0"`: "NoDamage ArcherArrow", "ThrowingDagger",
"NoDamage Tracer"). Ammo piece *k* = `first_ammo_piece + k` in AMMO load order (attachments-spec §2).

**Constants**: `GRAV_Z` (global float @cab378) = **−10.4875f** (0xc127cccd). It is BSS, written once in
`GraphicPieces::init` @8ffce3 and registered as a tunable by `GraphicPieces::registration`. ✅

### 4.2 How a unit type fires: `fire_proj` is decided by the art (✅ code, ✅ data scan)

- Rules load (rules-spec §7.2): `fire_proj = 1` if `attack ≠ 0` (or flag v). Not walked, not saved.
- `GraphicEvents::init_unit_events(gpiece)` @8e2520 runs once per unit graphic piece (lazily:
  `GraphicEvents::verify_load` @8e4780 is called from `Guy::update_gpiece`, `Unit::init/set_type`, `Build::queue_up`,
  `Guy::execute_events`, …; before `GraphicEvents::init` the pieces are queued and loaded by `start_verify`). For unit
  types 50..413:
  1. `fire_proj == 1` → `fire_proj = first_ammo_piece`.
  2. Each `<RELEASEEVENT starttime anim type node>` becomes a `GraphicEvent{event_type 1, animation = anim index
     (UnitAnimNames, or the SecondUnitAnimNames aliases), start_time = end_time = max(1, (u16)starttime / 67),
     subject = ammo gpiece of `type`, node_num = node, axis 3}`. If the ammo name resolves → **`fire_proj = 0`**.
     (Unknown names error and fall back to ammo piece 0.)
  3. `<NORELEASE>` present → `fire_proj = 0` (unused in the shipped data).
  4. `<INSTANTHIT ammo=…>` → `fire_proj = that ammo's gpiece` (4 types: cruise/nuclear missiles, V2).
  5. `<PLANERELEASE>` → event_type 5 (carrier launch). `<SOUNDEVENT>` → event_type 7 (presentation).
  6. Validation (only for `gpiece / num_unit_pieces_per_crew == 0` ❓): if `attack ≠ 0`, `fire_proj == 0`, no
     NORELEASE, not OBJMASK_MISSILE, not `is(62 Armed Merchant)`: at least one release event in anims 10..14 must
     have `start_time < get_game_frames(anim)`. Otherwise an error is reported and **`fire_proj = first_ammo_piece`**.
  7. Positions for nodes 4..7 and each release node are precomputed for every frame in
     `[start_time, end_time]` (`GraphicPieces::init_position`) (§4.6.4).
- Result for the port: per type, `fire_proj = 0` ⇔ some `unit_graphics.xml` variant of the type's GRAPH has a
  RELEASEEVENT (variants agree in the shipped data). `fire_proj ≠ 0` means fire directly. The value is an ammo gpiece:
  usually `first_ammo_piece`, the INSTANTHIT ammo for missiles.
  **Port note:** compute it at load from the XML. Events are stored per gpiece, and civ/age variants are linked groups:
  `get_event_group` @8e23c0 picks the last group in the chain whose `civ` is −1 or the owner's tribe civ and whose
  `age` < the owner's age.

### 4.3 `Unit::fight(ox, whom, p3, p4, p5)` @5fd4d0

Callers: `do_attack` @5f1b80 `fight(ord.ox, ord.whom, ord.mandatory, 0, 0)` (orders-spec §6.3), `do_group_attack`
(`(…, mandatory, group-arg, 0)` and `(…, 1, 1, 0)`), `cavarch_fight` `(cavarch_o, cavarch_who, unit_masks2 &
UNIT2_KEEP_TARGET, 0, 1)`. `p3` = mandatory/keep-target, `p4` = "do not retarget", `p5` = **cavalry-archer mode**
(no order bookkeeping, no turning, no animation). Returns **1 only when an in-range attack ran to the end (recharge
set)**, 0 otherwise (§4.3.12). The two RNG sites named in the task, plus a third one through `GameAccess::rnd`, are in
§4.3.5 and §4.3.9.

#### 4.3.1 Prologue ✅
1. If `ox, whom ≥ 0` and `!p5`: the current order's target object (`update_order()` vslot +0x50: `+8 ox, +0xc whom,
   +0x10 uid`) must have `uid == objects[whom][ox].uid`. If not: `ox = whom = −1`.
2. `retarget = (type.unit_flags2 & UNITTYPE2_RETARGETS) && !(ox, whom ≥ 0 && valid_target(ox, whom) &&
   !(get_order().flags & 0x10))`. Keep it as `rt` (1/0).
3. **Recharge gate:** if `recharging ≠ 0 && !retarget`:
   - `p5` → return 0.
   - Not `UNITTYPE_MELEE_AND_RANGED`: if `get_order()` (vslot +0xcc, the AttackOrder) has `new_ord` (+0x20) == 0 →
     return 0. If the order's target == guy 0's `ox/whom` → return 0. If guy 0's `cur_anim` ∈ [0, 3] → return 0.
     Otherwise `Unit::set_anim(DEFAULT, 1, 1)`, return 0.
   - MELEE_AND_RANGED: carry on (and do the snap below).

   Otherwise (not recharging, or retargeting) with `p5 == 0`: carry on.
4. **Snap** (only on the paths that carry on with `p5 == 0`): `set_new_location(x/48·48+24, y/48·48+24, 1, 0)`
   (U-cell centre, `div_3_table[c>>4]·48+24`). With p3 = 1 the guys **teleport** to their formation slots
   (movement-spec §9.3).

#### 4.3.2 Followers copy the captain's target ✅
If the unit is not the head of its uber chain (`is_captain` = `o_up < 0`), `!p5` and `collide_frame < frame − 1`: let
C = `get_captain()`. If C's action is ATTACK with target (o', w') ≠ ours, `valid_target(o', w')` and
(`get_combat_stance() < 2` or `is_in_range(o', w')` from our position): take (o', w') as the target, and if our order
type is ATTACK, write o', w' and uid into our order.

#### 4.3.3 Invalid target (`valid_target(ox, whom, &reason) == 0`) ✅
- **`p5`**: `cavarch_o = −1`. If `p3 && ox ≥ 0` and the target's captain c ≠ ox and is valid: `cavarch_* = c`
  (with uid), return 0. Otherwise go to **R** below.
- **`!p5`**: if `reason ≠ 0` and (`p3` or `UNIT_AI_CONTROL`): let `s = AI_CONTROL && type.is_siege`. If the type is
  ground (domain 0) and (`get_capture_value() ≠ 0` or `s`), and `vector_dist(self, target) > city_capture_radius·192`:
  `find_nearby_spot` around the midpoint (radius 0x180, FILTER_NOT_ME). If one is found →
  `add_move_order(spot, QUEUE_FIRST)`, plus `Army::charge(target)` when `s` and the unit is in an army. Return 0.
- Then: no action, or action ≠ ATTACK → return 0. `!p3 || ox < 0` → **R**. If the target's captain c ≠ ox and is
  valid → rewrite the order target to c and toggle order flag 0x10: if it was clear, set it and run `work()` (vslot
  +0x188) again. Return 0. Otherwise, if `next_action_type()` ∈ {NONE, ATTACK_TO, GROUP_ATTACK_TO} → **R**, else
  `kill_current_order(0)`, return 0.
- **R** (retarget, rate-limited per player): if `!p4`:
  - If `waiting < 5` and `leaders[who].retarget_budget` (LeaderData +0x9f4) > 10: set `UNIT2_HOLD_ANIM` (when
    `!p5`), `waiting += 2`, return 0.
  - Otherwise `find_new_target(NULL, 0)`. With `p5`: `cavarch_o/who/uid = find_melee_target(−1, &who, p5, 0, 0)`.

  Then `retarget_budget += 1` (reset to 0 every frame in `Leader::process`, also used by
  `resolve_unit_collision`). Return 0. Before returning, with `!p5`: if the order type is ATTACK and
  (`recharging == 0 || rt`): `unit_masks2 |= UNIT2_HOLD_ANIM`.

#### 4.3.4 Valid target: AI guard drop (draw **@5fdcdb**) ✅
If `!p5` and the activity is GUARD (12): `local = 0`. If `is_captain && UNIT_AI_CONTROL`: `local = 1` and
**`r = game_random.get(0, 0xffff)`. If r is odd, drop** (skip `check_target`). Otherwise, if
`check_target(ox, whom, 1, NULL, 1, local, 0) == 0`, drop. **Drop** = `kill_current_order(0)`,
`find_melee_target(−1, NULL, 0, 1, 0)`, then set `UNIT2_HOLD_ANIM` if the order type is ATTACK and
`recharging == 0`. Return 0.

#### 4.3.5 Valid target: re-evaluation (draw **@5fde6e**) ✅
Runs only if `!p3 && !p4 && recharging == 0`, and only for captains. For non-captains go straight to the range test.
- With `!p5`: if the order's vslot +0x2c is non-zero, and its vslot +0x94 record (+4 o, +8 who) is not this unit, skip
  to the range test (group-attack leader check ❓).
- **If the target is a unit**: let `civ = (unit_masks2 & UNIT2_ATTACK_CIVIES) ? (tt.role & ROLE_COMBAT &&
  !(tt.unit_flags & FIRESHIP)) : !(tt.role & ROLE_COMBAT)` (tt = target type; ROLE_COMBAT 0x10000). Then **draw
  `r = game_random.get(0,0xffff)`; if `r % 5 == 0` or `update_order().flags & 0x10`, `civ = 0`.** The draw happens
  whenever the target is a unit and this point is reached, which includes every frame of an out-of-range chase with
  recharging = 0.
- If the target is not a unit, or `civ` → look for a better target: `t = p5 ? find_melee_target(−1, &w, p5, 0, 0) :
  find_new_target(&w, 0)`.
  - (t, w) differs and t ≥ 0, w ≥ 0: with `p5`, `cavarch_* = (t, w, uid)`, return 0. Otherwise, if the order type
    is ATTACK, `recharging == 0` and (t, w) ≠ the original arguments: write (t, w, uid) into the order (vslot +0x3c)
    and set `UNIT2_HOLD_ANIM`. Return 0.
  - t < 0 or w < 0: with `p5`, `cavarch_o/who = t/w`. Return 0.
  - Same target: carry on.
- Otherwise (unit target, not `civ`), with `!p5`: if `poor_target(ox, whom)` → the same `find_new_target` step as
  above.

There is also a `get_combat_stance()` vcall at 5fe021 whose result is discarded (no effect).

#### 4.3.6 Range test ✅
`in = ObjectData::is_in_range(ox, whom, x, y, …, &d)` @6486b0 (range rules: §2.11, §3.6). `d` = `attack_dist`.
- `!p5`: `ord = update_order()` vslot +0x50 (null → return 0). `ord.in_range = in`. If `in`: `ord.ever_in_range = 1`.
  Else, if the type is a PACKER (flags2 & 4), not `UNIT_PACKED`, and `ord.ever_in_range` → `kill_current_order(0)`,
  return 0. Else go to the out-of-range branch §4.3.11.
- `p5` and not `in`: `cavarch_* = find_melee_target(−1, &w, p5, 0, 0)` (uid if found). Return 0.

#### 4.3.7 In range: packers and siege ✅
For `!p5`, PACKER types other than `is(62, strict)`:
- **Packed** (`UNIT_PACKED`): if AI-controlled siege, the target is a building with `attack ≠ 0`,
  `max_range() > target.max_range() + 1`, the target can reach us, and `find_attack_pos` gives a different spot →
  `add_move_order(spot, QUEUE_NEW)`, return 0. Otherwise `add_cast_order(−1, −1, −1, −1, spell 0x28c (unpack),
  QUEUE_FIRST, 0)`, return 0.
- **Unpacked**, target is a unit, type `is_siege`: `set_attacking(whom)`. Push an **ATTACK_GROUND** order at the
  target's current x, y (`[3] = target.domain == SEA`, `[4] = 2`, flags 0x80 and 0x04 cleared) onto the order list as
  current, `clear_partial_path`, `update_action`, `work()`, return 0. **Packable siege units always shoot units by
  ground attack** (`do_attack_ground` → `fire_ammo`; ground-order scatter in Ammo::init §4.8.6).

#### 4.3.8 In range: facing ✅
1. `unit_masks |= 0x11000` (UNIT_BLOW_COVER | UNIT_COVER_BLOWN). With `p5`: `cavarch_* = (ox, whom, uid)`.
   `turret = Unit::set_attack(ox, whom)` @5fce70: every squad guy gets `ox/whom`. If the gpiece has pivot restrictions,
   `max_range ≠ 0` and the target is an alive on-map unit or a building, the return value is
   `Guy::set_all_pivots` of the last guy. Otherwise 0.
2. Target domain AIR and `has_objmask(OBJMASK_ANTI_AIR)` → `turret = 1`.
3. If `turret`: `a = angle` (no turning). Otherwise `a = find_angle(target − self)`, with two corrections:
   - **Target is an alive building** (target vslot +0xc): T = target WallData. If T does not cover our own tile
     (tx = x/192, ty = y/192), check in this order: (tx−1, ty), (tx, ty−1), (tx+1, ty), (tx, ty+1). The first one that
     T covers and whose tile word (`world+0x138`, u16 per tile) has bit 0x4000 gives `a` = 0xC0000000, 0, 0x40000000,
     0x80000000 respectively (face the wall square-on).
   - **`UNITTYPE_ATTACK_SIDEWAYS`**, unless the type is 389 (Patrol Boat) and the target is SEA: `a` becomes `a − 90°`
     or `a + 90°`, whichever is nearer to `angle` (|Δ| folded by `~` when > 0x80000000). The minus side wins only
     when strictly nearer.
4. With `p5`: skip to §4.3.10 (no turn, no animation).
5. If `a ≠ angle`: `Unit::set_angle(a, 0)` (movement-spec §9.2).
6. Guy facing: if `squad_size < guys.count` (has crew) or domain SEA or `squad_size == 1` → every guy (squad and crew)
   gets `des_angle = a`. Otherwise each squad guy gets `des_angle = find_angle(target.x − guy.des_x,
   target.y − guy.des_y)`.
7. Snap again: `set_new_location(cell centre, 1, 0)`.

#### 4.3.9 In range: animation choice ✅ (asm 5fecef–5feec1)
`Unit::set_anim(anim, 0, rnd)` @616f40. For each guy in `[0, guy_mark)` then `[squad_size, count)`: if **guy 0's**
current animation is not attack-category, that guy's `hold_attack = 0`. Then `Guy::set_anim(anim, 0, rnd)`.

| Condition (first match) | anim, rnd |
|---|---|
| `UNITTYPE_ROCKING_ATTACK` | `(int)(find_angle(target − self) − a) < 0` → (ATTACK1, 0), else (ATTACK2, 0) |
| `UNITTYPE_MELEE_AND_RANGED` | `type.max_range ≠ 0` (ranged mode, see §4.4) → (ATTACKSPECIAL, 0); else (ATTACK1, 1) |
| `is(162 Immortals, 0)` | `d < 192` → (ATTACKSPECIAL, 0) **plus immediate melee damage**: `guy_mark` × `Object::do_damage(ox, whom, a, guy_mark, −1, 256, 0, 0)` (balance stats when semaphore bit). Skip `set_anim`; go to §4.3.10. Otherwise (ATTACK1, 1) |
| type id == 389 (Patrol Boat) | target domain SEA → (ATTACK1, 0); else (ATTACK2, 0) |
| `has_objmask(OBJMASK_ANTI_AIR)` | **`GameAccess::rnd(100)` (call @5fee89, game_random read @43ccae)**. If `< jam_unit_radar_prob` and `is_jammed(0) ≥ 0` → (DEFAULT, 1): the jammed unit plays no attack animation. Otherwise (ATTACK1, 1) |
| otherwise | (ATTACK1, 1) |

`rnd = 1` makes `Guy::set_anim` draw the variant (§4.5.1).

#### 4.3.10 Bookkeeping, then the recharge gate ✅
`set_attacking(whom)` @5ff5b0 sets the "at war" bits `LeaderData[who].+0x94[whom]` and `leaders[whom].+0x94[team]`,
`flags |= 0x80`, `visible |= 1<<whom` (`update_local_seen` if it was not yet seen). Then, if `!p5`:
`ord.new_ord = 0`. **If `recharging ≠ 0` → return 0** (MELEE_AND_RANGED units get here while recharging and only
re-animate).

#### 4.3.11 Delivering the attack ✅
Let `pt` = ptype.
- **`pt.max_range == 0` (melee)**: if `pt.fire_proj ≠ 0` or `p5` → `guy_mark` × `Object::do_damage(ox, whom, a,
  guy_mark, −1, 256, 0, 0)` (a = the facing from §4.3.8, i.e. the old `angle` with `p5`), plus balance stats. With
  `fire_proj == 0` (Mahouts in melee mode) nothing happens here: the release events deliver it.
- **ranged, not `UNITTYPE_BROADSIDE`**: `am = is(62, strict) && !UNIT_PACKED` (armed merchant). If `pt.fire_proj ≠ 0`
  or `am`: when `am`, temporarily set `pt.fire_proj = first_ammo_piece + 7` (the old value is restored afterwards);
  **`Object::fire_ammo(ox, whom)`** (§4.6). If `pt.unit_flags & UNITTYPE_FIRESHIP` → `die(3 SPLODED, −1, 0.0f)` (vslot
  +0x158), return 0. With `fire_proj == 0` (the usual case) nothing is fired here.
- **`UNITTYPE_BROADSIDE`** (no shipped type has this flag, so it is dead in practice 🟡):
  `Objects::find_units(x, y, SEARCH_ENEMY, −1, (max_range·3 + 6)·64, 0, FILTER_NOT_DOMAIN, 2, 0, _, 1)` then
  `find_builds(…, (max_range·3 + 3)·64, …, append = 1)` into the found list. `a0 = find_angle(target − self)`,
  `v = (tdx, tdy, 0)`. For each found object k in list order, skipping self: needs `k.domain ≠ 0` or k is the primary
  target; `attack_dist(k) ≤ max_range·192 + 6`; `|a0 − find_angle(k − self)| ≤ 0x40000000`; perpendicular distance
  `p = Vector<int>::length((0, 0, cross(w, v).z / length(v)))` with `w = k − self`. `length` is `(int)sqrt(double)`.
  The limit is `k.big_radius + self.big_radius` for units and `(x_size + y_size)·48` for buildings. If `p ≤ limit` →
  `guy_mark` × `do_damage(k, a0, guy_mark, −1, 256, k ≠ primary, 0)`.

#### 4.3.12 Recharge and end ✅
`recharging = (u8) recharge(&sup)` (vslot +0x134):
- **`UnitData::recharge(&sup)`** @60fdf0: `R = type.recharge` (+0x1f4), `sup = −1`. Not `is_siege` → R. Siege: if
  (`!artillery_under_attack_fires_slowly || !(unit_masks2 & UNIT2_UNDER_ATTACK)`) and `in_supply(&sup)` → R.
  Otherwise `is(267 Bombard, 0)` → `R·2`, else `R·3/2` (int).
- `UnitData::in_supply` @609ef0: non-ground → 1. Ground: 1 if the W-cell owner (world cell +0xf) is the unit's
  player. Otherwise `Supplies::find_supply(x, y, who)` ≥ 0 → 1 with `sup` = that supply. Else 0.

AI only: if `sup ≥ 0`, the owner is not HUMAN, the unit is in an army, and the target has flags & 0x20 or type vslot
+0xfc: `army.+0x30/+0x34 = ox/whom`. Then every guy gets `guy_flags &= ~4`. **Return 1.**

`(u8)`: a recharge above 255 wraps.
Period: `recharging` is decremented before `work()` each frame, so a unit that fires on frame N with `recharging = R`
can fire again on frame N + R.

#### 4.3.13 Out of range (`!in`, `!p5`) 🟡 (chase; see also §3.9)
If not (PACKER && !p3, where `find_new_target(&w)` fails) and `!p4`:
- If `!p3` and ((stance STAND_GROUND && `!on_duty()`) or (`UNIT_ENTRENCHED` && `!UNIT2_MARINE_ENTRENCH`)), or
  `find_attack_pos(ox, whom, 0, &px, &py, 0)` fails: `find_new_target(NULL, 0)`. If the order is still ATTACK on the
  same target: remember the defend point (AttackOrder `defensive` +0x1d, `def_x/def_y` +0x14/+0x18, both ≥ 0),
  `kill_current_order(0)`, and if a defend point exists and no order is left: `add_move_order(def, QUEUE_NEW)` with
  order flag 0x08. Then, if the order type is ATTACK and `recharging == 0` → `UNIT2_HOLD_ANIM`.
- Else if stance DEFENSIVE (1), `!p3`, AttackOrder `defensive` with a valid def point, and the distance ≥
  `max(unit_defensive_respond_range·192, max_range·192)`: same give-up and return.
- Else `add_move_order(px, py, 1, 0, QUEUE_FIRST)`. If `px ≠ x` **and** `py ≠ y`: toggle `update_action().flags &
  0x10`; if it was clear, set it and run `work()`.

Return 0.

#### 4.3.14 Return value ✅
1 only at the end of §4.3.12, i.e. the unit was in range, not recharging, finished the attack step (damage, fire,
broadside, or only an animation for event-firing units) and set `recharging`. Every other path returns 0. `do_attack`
uses it to trigger the extra `cavarch_fight` for MELEE_AND_RANGED units (§4.4).

### 4.4 Fire-while-moving and melee-and-ranged units ✅

- `Unit::do_move` @5f7b30 (orders-spec §6.2) calls `cavarch_fight` for types with `UNITTYPE_FIRE_WHILE_MOVING` and an
  attack. If the action is ATTACK and the type is not MELEE_AND_RANGED, it instead sets `cavarch_o = −1`. When
  `cavarch_o < −1` (−2 = "nothing found"), it only calls on `(o + frame) % 32 == 0`. `do_group_move` calls it too.
- `do_attack` caches mandatory targets of FIRE_WHILE_MOVING units in `cavarch_*` (sets/clears UNIT2_KEEP_TARGET).
  For MELEE_AND_RANGED units in stance STAND_GROUND it sets `ptype.max_range = ptype.second_max_range` (+0x2d0)
  around the `fight` call and back to 0 afterwards (**the shared type struct is changed temporarily**). In other
  stances, after `fight(...)` returns 1, it saves `recharging`, sets it to 0, calls `cavarch_fight()`, then restores it.
- **`Unit::cavarch_fight`** @5ff4b0: save `(o0, w0) = (cavarch_o, cavarch_who)`. MELEE_AND_RANGED →
  `ptype.max_range = second_max_range`. `fight(cavarch_o, cavarch_who, unit_masks2 & KEEP_TARGET, 0, 1)`, then restore
  `max_range = 0`. If the old target was valid (o0, w0 ≥ 0) and the cached target changed, clear `UNIT2_KEEP_TARGET`,
  unless the who is the same and the new o is o0's captain. If `cavarch_o < 0` and o0 was already negative →
  `cavarch_o = −2` (throttle).
- In `p5` mode `fight` neither turns nor animates. It deals immediate melee damage when `max_range == 0`, and calls
  `fire_ammo` only if `fire_proj ≠ 0`. **Event-firing horse archers shoot while moving through their `CHAR_ATTACKWALK`
  release events.** `Guy::move` plays ATTACKWALK instead of WALK when `cavarch_o ≥ 0` and the type has
  FIRE_WHILE_MOVING. `Guy::execute_events` falls back to `cavarch_o/who/uid` as the target when the current order is
  not an attack order (§4.5.4). ATTACKWALK is category 10, so there is no uid/validity gate in `execute_events`. The
  ammo is still dropped by `Ammo::init` if the target is invalid.
- **`Guy::set_anim(ATTACKWALK)`** @5da300 entry: if the guy is not already in ATTACKWALK and unit `recharging < 6`
  → **`recharging = 0`** and play ATTACKWALK. Otherwise play WALK. So the moving-fire cadence is the ATTACKWALK cycle,
  gated by the recharge (≥ 6 frames left → plain walk). ✅

### 4.5 Attack animation timing ✅ (data values ❓, see §4.17)

#### 4.5.1 `Guy::set_anim(anim, p2, rnd)` @5da300, attack path
On entry `end_old = end_time`. ATTACKWALK special case above. Gaia (who ≥ 8) WALK special case: only restart when
the walk has finished. For an attack-category `anim` on a non-plane guy (`guy_flags & 0x40 == 0`):
1. **Not in place**: if `des_x ≠ x − off_x || des_y ≠ y − off_y || des_angle ≠ angle` → `hold_attack = rnd ? 1 :
   anim`. Return (nothing else changes).
2. **Already playing an attack** (category 12) → `queued_attack = rnd ? 1 : anim`. Return.

Otherwise the generic part runs: `last_time = −1`, and `anim_index_hints = 0` (GuyOut) for non-walk categories.
For category 12: if `rnd`, **draw `r = game_random.get(0,0xffff) % 100` (@5db21d)**: `r < 30` → ATTACK1, `r > 70` →
ATTACK3, else ATTACK2. Without `rnd`, use `anim`. If the AnimationPacket has no AnimObj for the chosen action →
ATTACK2 (12). If that differs from `cur_anim`: set `cur_anim`, `cur_time = 0`. Finally
`end_time = AnimationPacket::get_game_frames(cur_anim)` = `animmgr.frames[action_ids[cur_anim]]`, or **3** if the
packet has no such action. GuyOut `cur/last_graph_time = cur_time` (presentation).
Note: same category with `cur_time < end_time` returns early only when `p2 == 0` and not via the attack branches.
The other two draws in `Guy::set_anim`, @5dac68 (idle variant, `% 100`, units without `openlist`) and @5db339 (gaia
birds 402–404 walk/jog), are not part of firing.

#### 4.5.2 Releasing a held attack: `Guy::move` @5d9240
At the destination with facing done (`des_angle == angle`): if `hold_attack`: `set_anim(hold < 2 ? ATTACK1 : hold,
0, hold < 2)`, `set_all_pivots`, `hold_attack = 0`, `stopped = 1`. At the destination but still turning: sea units
and SPECIAL_ANIM orders only set `stopped = 1`. Otherwise a held attack is re-requested, which re-holds because
facing is not done, then `turn_towards(des_angle, 1)` unless `guy_flags & 2`. Away from the destination: walk /
ATTACKWALK / carry animation, and the hold persists.
Because `fight` teleports the guys into their slots (§4.3.1), the effective delay before an attack starts is **the
guy's turn time** to `des_angle` (movement-spec §9.2).

#### 4.5.3 The clock: `Unit::inc_time` @610b40 → `Guy::inc_time` @5d9e10
Called from `Objects::inc_time` for units on the map (or types 52/53) after the whole `process_all` pass
(frame-spec §7), for guys `[0, guy_mark)` and `[squad_size, count)`.
- `step = 1`, or 2 if `guy_flags & 4` and the anim is attack-category. **No writer of guy_flags bit 4 exists**: only
  `Unit::fight` and the uncalled `Unit::set_rally` clear it (`insn_guyflags2.txt`), so step 2 is dead 🟡.
  `step = 0` while `unit_masks2 & UNIT2_HOLD_ANIM`, which `Unit::process` clears every frame before `work()` and
  `fight` sets on deferred retargets: the clock freezes for that frame.
- Squad guys (`guy_num < squad_size`) and walk-category anims advance; crew guys copy guy 0's `cur_anim` and
  `cur_time`. Advance: `last_time = cur_time; cur_time += step`.
- While `cur_time ≥ end_time`: if the action loops (`animmgr.loopings`) → `set_anim(same, 0, 1)` (restart). Else, if
  not attack-category → `set_anim(cur == ATTACKWALK ? WALK : DEFAULT, 0, 1)`. Else (an attack finished) →
  `set_anim(DEFAULT, 0, 0)`, then if the unit is not a hero and `queued_attack`: `q = queued_attack;
  queued_attack = 0; set_anim(q < 2 ? ATTACK2 : q, 0, q < 2)`. A random variant gets a fresh draw (@5db21d).
- After the loop: a queued attack while not in an attack anim and not walking (cur ≠ 8) → `set_anim(q < 2 ? ATTACK1
  : q, 0, q < 2)`, clear it.
- Guy 0 in an attack anim with crew: crew guys teleport to their `des` and copy `cur_anim`/`cur_time`.
- `GuyOut::graph_inc_frame` for who < 8 (presentation).

#### 4.5.4 Events: `Unit::execute_events` @60edc0 → `Guy::execute_events` @5d99c0 → `GraphicEvents::execute_game_events` @8e48e0
Right after `inc_time` for the same unit. Only alive, on-map units without `UNIT2_HOLD_ANIM`, guys `[0, guy_mark)`
(crew guys never fire); otherwise just `verify_load`.
- Package: `x/y/z` = guy position, `cur_anim`, `cur_time`, `last_time` (−1 when `cur_time == 0`), `angle` = guy
  angle, `pivot_angles[i] = fast_angle_to_degrees(turret_angles[i])` (all 0.0 if every turret angle is 0),
  `node_flags`, `who/o`, `gpiece` = guy gpiece.
- **Target**: refresh the unit's current order. If it is an attack-type order (vslot +0x18): if vslot +0x2c is 0 →
  target from vslot +0x3c (`+8 ox, +0xc whom, +0x10 uid`), and "skip the uid check" when vslot +0x54 ≠ 0. Else from
  vslot +0x9c (+0x40, +0x44, +0x10). SPECIAL_ANIM → vslot +0xa8 (+0x14, +0x18), no uid. ATTACK_GROUND /
  AIR_ATTACK_GROUND → no target, skip-check set. Any other order or none → **`cavarch_o/who/uid`** if
  `cavarch_o ≥ 0`, else none.
- Aircraft carriers (sea, `is(351)`) run a launch sequence that writes unit +0x5c and +0x44 (PLANERELEASE/come_out).
  Simulation state, out of scope ❓.
- Non-unit gpieces return. `verify_load`. **Attack-category anim without the skip flag** → requires `ox ≥ 0`,
  `whom ≥ 0`, target alive and `uid` match. Otherwise no events at all this frame.
- `execute_game_events`: for every event of the chosen group for `cur_anim`, **fire when `last_time < start_time ≤
  cur_time`**:
  - **type 1 (release)**: also requires `whom ≠ −1 && ox ≠ −1`, or the unit's order is (AIR_)ATTACK_GROUND. Pivot
    restriction `r = has_restrictions(gpiece)`: needs `r == 0`, or `node & 3 ≥ r`, or `node_flags` bit `node & 3`
    (the turret is aligned, set by `Guy::process` turret stepping).
    Then `off = GraphicPieces::get_position(unit gpiece, node, anim, start_time, degrees(angle), pivot_angles, r)`
    (float vector, §4.17). `pkg.x/y/z += (int)off` (truncation), `pkg.angle = node`, `pkg.gpiece = event.subject`
    (the ammo gpiece). **`Objects::add_ammo(pkg)`**, then restore the package.
  - type 5 (PLANERELEASE): pops `launching` and `come_out`s an aircraft (simulation, carrier only).
  - type 7: sound (presentation).
- **Consequences**: one ammo per guy per release event in the anim (e.g. ADVMACHINEGUN has 5 releases in ATTACK1).
  Time from `fight` to the shot = turn time + `start_time` frames. Release times sit at a fixed frame of the anim, so
  the recharge mainly limits how often `fight` restarts the anim. If `recharge` is shorter than the anim, the next
  request is queued (`queued_attack`) and starts when the anim ends. Example: Archers release at ATTACK1 833 ms →
  frame 12, ATTACK2 666 → 9, ATTACK3 1066 → 15 (`/67`, integer).
- Events whose `start_time ≥ end_time` never fire (the anim restarts before `execute_events` sees the frame).
  Validation (§4.2) guarantees at least one usable release.

#### 4.5.5 Ordering inside a frame (for a ranged event-firing unit)
`Objects::process_all` (unit pass) → `Unit::process` → `work()` → `do_attack` → `fight` (draws §4.3.4/§4.3.5,
`rnd(100)` for AA, then one variant draw per guy that starts an anim, guys in order) → … end of pass, buildings →
`Objects::inc_time`: units in player/slot order: `Guy::inc_time` then `execute_events` (release → `add_ammo` →
`Ammo::init` draws). Then buildings (`Wall::inc_time`: AA turret events), then **all ammo in slot order**
(`Ammo::inc_time`; the count is re-read, so ammo created this frame, including during this loop, also flies 1 tick
this frame).

### 4.6 `Object::fire_ammo(ox, whom)` @64c8b0 ✅

1. Unit: `gpiece = ptype.fire_proj`. Types 61/62/400 (Merchant, Armed Merchant, Fur Trapper) when not
   `UNIT_PACKED`: `gpiece = first_ammo_piece (+1 if the owner's age > 2)`, attack sound.
   Building (`is_build`, vslot +0x20; anything else returns): attack sound,
   `gpiece = first_ammo_piece + BuildData::get_shot()`.
   `get_shot` @62dd90: 441/445 → 1; 442/523/524 → 2; 446 → 4; 521/522 → 0; 525 → 5; any other type: owner age
   (LeaderData+0x6eb8 → +0xdc XOR 0x62766) ≤ 2 → 0, 3–4 → 1, ≥ 5 → 2.
2. `pkg.angle` = unit `angle` (buildings 0). Z lift `h` = 100 (unit) / 250 (building). `pkg.who/o` = shooter,
   `pkg.whom/ox` = target.
3. Nothing happens unless `ox ≥ 0 && whom ≥ 0`, or the shooter is a unit whose order is ATTACK_GROUND /
   AIR_ATTACK_GROUND.
4. **Unit**: for each squad guy i in `[0, guy_mark)`: `pkg.x/y = guy.x/y`, `pkg.z = guy.z + 100`, `add_ammo(pkg)`. One
   ammo per living soldier, no RNG.
   **Building**: `W = type.x_size·96`, `H = type.y_size·96`. Repeat `ammo_per_att` times:
   `pkg.x = bx − W/2 + (W−1 ≥ 1 ? rnd(W) : 0)` **(draw @64cc73)**, `pkg.y = by − H/2 + (H−1 ≥ 1 ? rnd(H) : 0)`
   **(draw @64cca4)**, `pkg.z = bz + 250`, `add_ammo(pkg)`. Here `rnd(n) = get(0,0xffff) % n`, and `W/2` and `H/2`
   are int divisions.

Other callers: `do_attack_ground`, `do_air_attack_ground`, `do_strafe` (aircraft), `resolve_unit_collision`
(fireships ramming ❓), `Build::do_attack`.

### 4.7 `Objects::add_ammo(pkg)` @658b10 ✅
Slot = the lowest index i in `ammo_objs` with `(flags & 3) == 0`. If there is none, allocate a new `Ammo` at the end
(the array grows). `Ammo::init(slot, pkg, index = i, graph_index = Objects.+0x1f8)`, then `Objects.+0x1f8 += 1`
(always, even if init bails out). Returns i. **Slot choice is simulation-relevant**: ammo is processed in slot order,
and stuck arrows hold their slot for 200 frames.

### 4.8 `Ammo::init(pkg, index, graph_index)` @67bbf0 ✅ (asm)

In execution order:
1. `flags &= 0xe3` (clear 0x04/0x08/0x10). `who/o` from pkg. `shooter_vis = shooter.is_seen(console)`
   (presentation). `whom/ox` from pkg. Free any old spline.
2. `bomber = shooter.is(304 Bomber, 0)`.
3. `gpiece < first_ammo_piece` → error, return (slot stays free). `miss_gfx = gfx_flags & MISSILE(8)` (valid ammo
   pieces only). `gfx_flags & DO_NO_DAMAGE` → `flags |= 0x10`. `index`, `graph_index` stored.
4. **Ground order**: if the shooter is a unit with order ATTACK_GROUND / AIR_ATTACK_GROUND → `G = get_order()`
   vslot +0xd0 (`+4 gx, +8 gy, +0xc g_exact`). Skip to step 6.
5. **Target**: if `ox < 0`, `whom < 0` or the target is dead → **return without activating** (the slot stays free).
   **Anti-air roll** (target is a unit, domain AIR, not HELICOPTER, not OBJMASK_MISSILE):
   - shooter has OBJMASK_ANTI_AIR and domain AIR → no roll.
   - shooter has OBJMASK_ANTI_AIR, not air: `low = target.is_flying_low()`. `r1 = get % 100` **(@67c079)**. If
     `r1 ≥ T` → miss. Else `r2 = get % 100` (**@67c0ba** if low, **@67c126** if high); miss unless `r2 < S`.
     T/S = target/shooter `fly_low` (+0x254) when low, `fly_high` (+0x250) when high.
   - shooter without ANTI_AIR: `low = is_flying_low()`. `r = get % 100` **(@67c00c)**. Miss unless `r < S`
     (shooter fly_low/high).

   Miss → `flags |= 0x10`: the shot flies but does no damage.
   `UnitData::is_flying_low` @60a140: planes on the map whose STRAFE target or AIR_ATTACK_GROUND point (or order
   vslot +0xfc record with flag +0x18) is within 0x900 🟡.
6. `gpiece` stored, `start_roll_angle = 0`. `num_guys` = shooter `guy_mark` (units) or 0. Start point: if the
   shooter has OBJMASK_MISSILE → shooter x/y/z + the global vector at `graphic_events`+0xc8/+0xcc/+0xd0 ❓.
   Otherwise `sx,sy,sz` = pkg x/y/z.
7. **Bomber** (`bomber ≠ 0`): `(ex, ey) = project(sx, sy, shooter.angle, 192)`, `WorldData::restrict`,
   `ez = find_data_z(ex, ey, 0)`, `angle = find_angle(ex − sx, ey − sy)`. Go to step 11. **`accuracy` is not written**
   (it keeps the slot's previous value, deterministic because ammo is walked state 🟡).
8. **Accuracy**: `d = G ? vector_dist(gx − sx, gy − sy) : attack_dist(ox, whom)`.
   `acc = to_hit − trunc(d / 192)·attenuate`, then `acc = max(acc, 5)` (`·0xd5555555`/`sar 5`/sign fix = exact
   division by −192). Shooter type fields.
9. **Scatter radius r**:
   - shooter is a unit with UNITTYPE_STRAFING → 0.
   - G: `g_exact ≠ 0` → 0, else F.
   - target is a unit with domain GROUND → F, and `flags |= 0x04`, `zb = 75` unless `miss_gfx`.
   - target is anything else (building, ship, air) → 192.

   `F = (target_radius·100) / ((100 − acc)/5 + acc)` (trunc divisions). If `acc > 100`: `F /= 4`.
   `accuracy = (short)acc`. Shooter OBJMASK_MISSILE: `r = is(315 Nuclear Missile, 0) ? 0 : 2r`.
10. **Impact point.** Every scatter offset is `off(r) = (r − 1 ≥ 1 ? get % r : 0) − r/2`.
    - **G**: `ex = gx + off` **(@67c6c6)**, `ey = gy + off` **(@67c703)**,
      `ez = max(0, find_data_z(gx, gy, 0))` (terrain at the *unscattered* point),
      `angle = find_angle(ex − sx, ey − sy)`.
    - **target**: `angle = find_angle(tx − sx, ty − sy)`, `(ex, ey) = (tx, ty)`. If `r ≠ 0`, the target is a building
      and the shooter type is neither `is_siege` nor `is_tank`: `(ex, ey) = project(ex, ey, angle − 0x80000000,
      target.x_size·48)` (pull back to the near face). Then `ex += off` **(@67c8b7)**, `ey += off` **(@67c8e9)**.
      `ez = tz (+75 if flags & 4)`. If the target is an AIR unit, `ez = guy0.z`. `ez = max(ez, 0)`.
    - **Strafing** shooters (unit, STRAFING), target not air, guy 0 in an attack anim:
      `(ex, ey) = project(ex, ey, shooter.angle, trunc((f(cur)/f(end) − 0.3f)·6.0f·192.0f))`, where f(u) =
      `(float)(double)u` and the guy's `cur_time`/`end_time` are used. Then `project(…, shooter.angle ± 90°, 48)`:
      −90° if `pkg.angle & 1` (the release node is odd), else +90°.
11. Clamp: `ex, ey` into `[0, 768·size − 1]` (each axis, low first). `splash_area = shooter.type.splash_area`.
    `flags |= 2` (active), `cur_time = 0`.
12. **Flight time** `total_time`:
    - `miss_gfx` (rockets, torpedoes, SAM/AA rockets, cruise missiles, Stinger) → a Spline from
      `Spline::calc_from_dir(start, start + dir, end, 0)`. `dir` is `(0, −m, 0)` rotated by a pitch and then by a
      heading:
      - ground shooters (domain 0): pitch 45°, `m = attack_dist/2`;
      - other shooters: pitch = guy 0's `pitch` (+0x4c), heading = guy 0's `angle` (also stored in `angle`),
        `m = proj_speed·ums·5` for non-missile units, else 0x480.

      Speed argument: `UnitData::speed()` for OBJMASK_MISSILE units, `proj_speed·ums` for other units, `ums·150` for
      buildings. `total_time = spline.count` (+0xd8). `traj = 2` ❓ (float-heavy, needs its own spec with the asm at
      67cb1f–67cddd).
    - building: `D = sqrtf((float)((ex − sx)² + (ey − sy)²))` (int squares), `total = trunc(D / (float)(ums·90))`
      if `get_shot() == 0`, else `trunc(D / (float)(type.proj_speed·ums))`.
    - unit with OBJMASK_MISSILE: `Spline::calc_nuke_spline(sx, sy, sz, ex, ey, ez, is(314 Cruise Missile, 1))`,
      `total = count − 1`, `traj = 2`. Nuclear missile: message event (UI).
    - bomber: `total = trunc(sqrtf((float)(2·(ez − sz)) / GRAV_Z))` (fall time).
    - `is_siege`: `total = max_range()·192 / (proj_speed·ums)` (int). **Independent of the actual distance.**
    - other units: `total = trunc(sqrtf((float)((ex−sx)² + (ey−sy)²)) / (float)(proj_speed·ums))`.

    Every float→int conversion goes through double (`cvtss2sd`; `cvttsd2si`), which equals truncating the float.
    Then `if (total < 1) total = 1` (unsigned).
13. **Lead** (not bomber, no G, not a spline): if the target is a unit whose order type satisfies `is_move()` @46f050
    or `is_air()` @46f000: `a = target.angle`, `s = target.guy0.avg_speed`,
    `ex += sin(a, s)·total`, `ey −= cos(a, s)·total` (int32), `WorldData::restrict(&ex, &ey)`. No re-clamp, and ez is
    unchanged.
14. `rolling = 0`. `traj = spline ? 2 : 1`. Let `T = (float)(double)total`:
    - `v1z = ((float)(ez − sz) − GRAV_Z·0.5f·T·T) / T`, evaluated as `((GRAV_Z * 0.5f) * T) * T`, then subtract, then
      divide.
    - `dx = sqrtf((float)(ey − sy)·(float)(ey − sy) + (float)(ex − sx)·(float)(ex − sx)) / T`: the squares are taken
      in float, and the y term is added first.
    - `bank_dx = bank_dy = 0`.

    Balance stats `attacks[who]++` when the semaphore bit is set.

### 4.9 `Ammo::inc_time` @67d380 (from `Objects::inc_time`, slots with flags & 3) ✅
Loop:
1. `cur_time += 1`. If the shooter slot is dead → `shooter.hold_frames += 1` (keeps the dead slot reserved while its
   ammo flies, frame-spec §8.2).
2. `!(flags & 2)` (stuck arrow, flags == 1): close at `cur_time ≥ 200` (u32), else return.
3. `cur < total`:
   - **spline** (`ammo_path`): target invalid or dead → return. `cur + 1 ≥ count` → return.
     `p = trunc(pts[cur])` (x, y, z). `tz` = building z, or guy 0's z for units. `R` = unit `target_size` (+0x300), or
     192 for others. If `vector_dist(|px − tx|, |py − ty|) < R` and `|pz − tz| < 192` → `(ex, ey, ez) = p`,
     `total = cur − 1`, loop again (impact this call). Else, if `cur ≠ 0`, `cur % 4 == 0` and the target is a unit →
     re-home: `ex, ey = target x, y`, `ez = tz`, `Spline::calc_from_dir(p, p + 2·(pts[cur+1] − p), target, 0)`,
     `total = count`, `cur = 0`. Return. ❓ (Spline math)
   - `traj ≠ 1` → return.
   - If `bank_dx` or `bank_dy ≠ 0` (crash debris only): `ex += (int)bank_dx`, `ey += (int)bank_dy`, restrict,
     `dx = sqrtf((float)((ex−sx)² + (ey−sy)²)) / (float)(double)total` (int squares here).
   - gpiece is not a unit piece (`get_gpiece_type ≠ 0`, every real projectile) → return.
   - **Crash debris terrain test** (gpiece is a unit piece): `t = f(cur)`, `k = t / f(total)`,
     `px = trunc((float)(ex − sx)·k + (float)sx)`, same for py. `h = find_data_z(px, py, 0)`.
     `z = (v1z·t + (float)sz) + ((GRAV_Z·0.5f)·t)·t`. If `(float)h > z` → `(ex, ey, ez) = (px, py, h)`,
     `total = cur + 1`, loop (impact this call). Else return.
4. `cur ≥ total`: **arrival**.
   - `flags & 4` (ground-unit target) and not `& 8`: `hit_target()`, or else `check_hit(GROUND)`. If both fail →
     `flags |= 8`.
   - `flags & 8` (missed, falling on): if `cur > 3·total` (u32) → close. Else extrapolate with `t = f(cur)`,
     `k = t / f(total)`: `z = (v1z·t + (float)sz) + ((GRAV_Z·0.5f)·t)·t`, `px/py` as in step 3. Not `is_valid` → close.
     `h = find_data_z(px, py, 0)`. If `z > (float)h` → return (still falling). Else `(ex, ey) = (px, py)`,
     `ez = trunc(z)`, `total = cur`.
   - `WorldData::restrict(ex, ey)`, `AmmoOut::graphic_finish` (presentation). `flags & 0x10` → close, else
     **`do_damage()`**.

### 4.10 Hit tests ✅
- **`Ammo::hit_target`** @678f90: the target must be valid, alive and on the map. `d = vector_dist(|tx − ex|, |ty −
  ey|)`, halved if `accuracy > 100`. Unit: hit iff `d ≤ type.target_size`. Building: hit iff `|ex − bx| ≤
  x_size·96` and `|ey − by| ≤ y_size·96` (inclusive bounds). On failure `whom = ox = −1`, return 0.
- **`Ammo::check_hit(domain)`** @678d90: search = `SEARCH_NON_FRIENDLY` if the shooter type is AIR and not
  OBJMASK_MISSILE, else `SEARCH_ALL`. Filter: AIR → `FILTER_DOMAIN 2`; GROUND/SEA → `FILTER_NOT_DOMAIN 2`; else all.
  `o = ObjectsData::find_unit(ex, ey, search, shooter.who, 384, 0, filter, 2, 0, FILTER_ALL, 0, 0)` (nearest:
  `Objects.+0x1fc` = distance, `+0x200` = who). Reject if `type.target_size < distance`. If none: when (ex, ey) is
  inside the map, `find_building_at(tile(ex, ey), SEARCH_ALL, −1)` → hit. Else a miss sound (water/land, UI),
  `whom = ox = −1`, return 0.

### 4.11 `Ammo::do_damage` @678060 ✅ (asm)
1. If the recorded target is dead → `whom = ox = −1`. If shooter `who/o < 0` or the gpiece is a bird piece
   (`get_gpiece_type == 9`) → close.
2. **No splash** (`splash_area == 0`): `hit_target()`, or else `check_hit(original target domain, GROUND if none)`.
   - Hit on an alive object that is not the shooter: **`Object::do_damage(shooter; ox, whom, angle = find_angle(ex −
     sx, ey − sy), num_guys, index, 256, 0, 0)`**. The 5th argument is the ammo slot index (−1 for direct melee),
     the 6th the percent (256 = full). Then balance stats, close.
   - No target, or the target is the shooter: **two draws**: `jx = ex + get % 41 − 20` **(@678ca7)**,
     `jy = ey + get % 41 − 20` **(@678cc4)**, restrict. If the gfx ammo has PUNCTURE (`verify_ammo_flags(gpiece,
     1)`): `AmmoOut::puncture_ground` (presentation, internal_random). If (jx, jy) is valid and its tile is not water
     (`tile & 0x30 ≠ 0x20`) → `flags = 1`, `cur_time = 0`, **return without closing**: a stuck arrow holds the slot
     200 frames. Otherwise close.
3. **Splash** (`splash_area > 0`):
   - Shooter OBJMASK_MISSILE (cruise/nuclear missiles): territory owner at the impact W-cell (world cell +0xf). On
     ocean, search the first 120 spiral cells for a land owner. If that owner has `MISSILE_DEFENSE_BONUS` and is
     not the shooter → messages/sounds (UI), close, **no damage**. Nuclear (`is(315)`): achievement, leader counters
     (+0x7b4 frame, +0x7bc), `Nuke::add_nuke(ex, ey, 100, splash_area·192, who, o)`, reveal a circle of radius
     `clamp(splash + 1, 0, 64)` (`World::set_seen2`), message, `Nuke::do_damage`, close. Others: nuke-hit sound,
     counter +0x7c0, then fall through.
   - `hit_target()`, or else `check_hit(domain D of the original target, GROUND if none)`. **Primary = (ox, whom)
     after this.**
   - Centre `(cx, cy) = (ex, ey)`. FIRESHIP shooters use `((sx + ex)/2, (sy + ey)/2)`.
     `angle = find_angle(cx − sx, cy − sy)`. `rr = min(splash_area/4 + 1, 10)`. Visit W-cells `(cx/768 + move_x[k],
     cy/768 + move_y[k])` for `k = 0 … radius[rr]` **inclusive** (`radius[r] = (2r+1)²`, so one cell of the next ring
     is included too, and at rr = 10 that reads past the 441-entry table ❓). Skip cells off the map. In each cell,
     walk the W-cell object list (head = world cell +8/+10, next = object `down`/`down_who` +0x2c/+0x2e; order per
     frame-spec §8.1 / movement-spec §10.1). For each object X (the ammo's `whom/ox` is overwritten with X):
     - skip the shooter itself; **skip `X.who ≥ 8`** (gaia and animals are never splash-damaged, even as primary);
     - not primary: skip if `X.who == L.self` (LeaderData +8, L = shooter's leader), or if `L.+0x74[X.who] &&
       leaders[X.who].+0x74[L.self]` (mutually allied);
     - **building** (vslot +0xc): skip when D == AIR. `ddx = max(0, |cx − bx| − x_size·192)`,
       `ddy = max(0, |cy − by| − y_size·192)`, `pct = 256 − (vector_dist(ddx, ddy)·256) / (splash_area·192)`.
       If `pct ≥ 0` → `do_damage(X, angle, num_guys, index, pct, X ≠ primary, 0)`;
     - **unit** (alive, on map): requires `(D ≠ AIR) == (X.domain ≠ AIR)` and X not OBJMASK_MISSILE.
       `dd = max(0, vector_dist(|cx − ux|, |cy − uy|) − 192 − X.guy_radius)`, `pct = 256 − (dd·256) /
       (splash_area·192)`. If `pct > 0` → same `do_damage` call.

     Then balance stats, close.

Damage amounts: `ObjectData::get_damage` / `Object::do_damage` (§5). The only per-ammo scaling here is
`pct` (256 = 100 %), `num_guys` and the secondary flag.

### 4.12 `Ammo::init_crash(guy, index, graph_index)` @67b800 (aircraft dying, from `Unit::close`) ✅
`flags &= 0xe3`. `who/o` = the guy's unit, `num_guys = 1`, `gpiece = unit.get_gpiece()` (a unit piece, so the terrain
test of §4.9 applies). `start_roll_angle = −(int)guy0.bank` if the action is 16/17/24, else 0. `s` = guy x/y/z.
`v = type.moves·ums`. `t = sqrtf((float)(−2·sz) / GRAV_Z)`. `ex = unit.x + trunc((float)sin(a, v)·t)`,
`ey = unit.y + trunc((float)sin(a − 90°, v)·t)` with `a` = guy angle (the unit position, not the guy's). Clamp to the
map, `ez = find_data_z(ex, ey, 0)`. `angle = a`, `splash_area = 2`, `flags |= 2`, `cur = 0`.
`total = max(1, trunc(sqrtf((float)((ex−sx)² + (ey−sy)²)) / (float)v))`.
**Draw @67baf7: `rolling = get % 7 − 3`.** `traj = 1`, `v1z = 0`, `dx = sqrtf(…)/T`. Bank: a **local**
`Random` seeded with `sx + sy + sz` gives `bank_dx = (float)get(−30, 30)` and `bank_dy = (float)get(−30, 30)` (not
game_random, but deterministic and simulation-relevant: the impact point drifts by `(int)bank` every frame, §4.9).
`whom = ox = −1`. On impact it splashes (radius 2) like any ammo.

### 4.13 Buildings that fire ✅

#### 4.13.1 Call site (`Build::process` @61edf0, frame-spec §10)
For ACTIVE buildings with `type.attack ≠ 0`:
- no target (`attack_ox < 0 || attack_whom < 0`): `do_attack()` on `(o + frame) % 32 == 0`. Otherwise only if
  `near_o` (+0x34) ≥ 0 and `is_in_range(near_o, near_who)`.
- target set: `do_attack()` every frame.
- AA buildings (OBJMASK_ANTI_AIR) with an alive target: the turret angle `WallData.job_counter` (+0x48) turns toward
  `find_angle(target − self)` by at most 0x38e38e3 (5°) per frame.

The recharge only counts down inside `do_attack`, i.e. on the frames it is called.

#### 4.13.2 `Build::do_attack` @6228f0
1. Unless (OBJMASK_ANTI_AIR and not `is(521 Lookout, 1)` and not `is(522 Observation Post, 1)`): if `recharging`
   (+0x7a, short) ≠ 0 → `recharging -= 1`, return.
2. On `(frame + o) % 32 == 0` with `flags & 0x80` clear → `visible = 0`. Then `flags &= ~0x80`.
3. `hits_left == 0` → return. `is_jammed(&jw) ≥ 0` → mark the jammer (`unit_masks2 |= UNIT2_NEW_JAM`), sound,
   `build_masks |= 0x8000`, return. Else clear 0x8000.
4. `arrows = get_garrison_arrows()` (§4.13.3). 0 → return (**a building without arrows does not shoot**).
5. If `build_masks & 4` (mandatory target, from `add_attack_order`) is clear → `find_target()`.
6. No target → step 9. `!valid_target` → `find_target()`, return. If not mandatory and (frame == 0 or
   `(o + 14 + frame) % 32 == 0`): if the target is not a unit, or `is_moving()`, or (not ROLE_COMBAT and action ≠
   CAST_SPELL) → `find_target()`, return (re-pick, no shot this frame).
7. `is_in_range(attack_ox, attack_whom)`: if in range: AA buildings other than Lookout/OP → return (they fire through
   §4.13.4). **`fire_ammo(attack_ox, attack_whom)`**, `flags |= 0x80`, visibility bit for `whom` (with
   `update_local_seen` if `ever_seen` lacks it), and **`recharging = (short)type.recharge / arrows`** (int division
   of the short-truncated value), return.
8. Not in range → step 9.
9. Clear: `build_masks &= ~4`, `attack_whom = −1`, `attack_ox = −1`.

Volley = **one target, `ammo_per_att` arrows** from random footprint points (§4.6), each with its own Ammo::init
rolls. Period = `recharge/arrows + 1` frames while `do_attack` runs every frame.

**`Build::find_target`** @622c80: `attack_ox/whom = find_nearby_target((max(x_size, y_size) + 2·max_range)·96, &who,
1, 0, 0)`, clear `build_masks & 4` (§2.16).

#### 4.13.3 Garrison arrows, range, attack
- **`BuildData::get_garrison_arrows`** @62e3d0 (vslot +0x1b8): `div = (attack() + 5)/10`; 0 → 0.
  `base = get_base_garrison_arrows()`. `S = count_inside(COUNT_GARRISON_ARROWS)`. If `base == 0`: `S += div/2`.
  `g = min(S / div, most_shots())`, return `base + g`.
- **`get_base_garrison_arrows`** @62e340: `type.base_arrows` (+0x2cc), `+ maya_garrison_arrows` for tribe bonus 1
  (Maya), except for city buildings that are active and whose city has +0x5f ≠ +0x5e. `most_shots` @62e2f0 =
  `type.most_shots` (+0x2c4).
- **`ObjectData::count_inside(COUNT_GARRISON_ARROWS)`** @64dda0: over the `inside_down` chain, for captains that are
  not decoys, have OBJMASK_FOOT and are not siege: militia (`is(66)`), and workers when the owner has the militia tech,
  add `(current militia upgrade.attack/10 + 2)/3`. Everyone else adds `attack()/10`, halved rounding up
  (`(v + 1)/2`) for melee (`max_range == 0`).
- **`BuildData::attack`** @62e610: `ObjectData::attack` + `general_building_attack·10` if a General (type 54 or the
  tribe's replacement) is inside a Tower (`is(439)`), a type with vslot +0xfc, or a city building. Plus
  `obsidian_archers_attack·10` (leader bits +0x6da7/+0x6dcf & 4). Plus `antipater_garrison_attack_bonus·10` if
  Antipater (361) is inside and leader +0x59d0 ≠ 0.
- **`BuildData::max_range`** @62e470: `BuildTypeData::get_building_range(who)` + `general_building_range` (General
  inside a Tower or similar). City buildings: + temple range (`LeaderData::get_temple_range`, ×(100 +
  tikal_temple_hp)/100 rounded up with Tikal, wonder 532) when the city flag +4 & 0x80, + general range again.

#### 4.13.4 Air-defence turrets: `Wall::inc_time` @63fb60 (types 523–525; OBJMASK_ANTI_AIR but not 521/522) ✅
Called in `Objects::inc_time` for buildings. Here `BuildData.recharging` (+0x7a, signed) is an animation clock.
`F8 = get_game_frames(8)`, `F12 = get_game_frames(12)` on the building's AnimationPacket.
- `near_o < 0` or jammed (`build_masks & 0x8000`): if `rc < 0` → `rc = F8 − 1`. If `rc ≥ 1` → `rc -= 1` (lower).
- Otherwise: if `0 ≤ rc < F8 − 1` → `rc += 1` (raise). Else if `attack_ox < 0` → `rc = F8 − 1`. Else: if `rc ≥ 0` →
  `rc = 0`. If `rc ≤ −F12` → `rc = −1`. Else `rc -= 1`.
- If `rc < 0`: build a package {anim 12, `cur_time = −rc`, `last_time = cur_time − 1` (−1 at 0), `ox/whom =
  attack_ox/whom`, angle 0x20000000, gpiece = building piece, pivot angles = degrees of the turret angle} and, if not
  jammed, `execute_game_events`: the building's RELEASEEVENTs fire `add_ammo`.
- The rest of `Wall::inc_time` (queue particles, damage smoke) is presentation.

### 4.14 Melee vs ranged and the special classes (summary)
- **Melee** (`max_range 0`, no release events): damage on the `fight` frame, `guy_mark` hits of 256 %, no Ammo, no
  RNG in the delivery itself. Rate = `recharge`.
- **Ranged with events** (most): `fight` animates; ammo is created at the release frames, one per guy per event.
  Every ammo rolls (§4.8). Flight time is `distance/proj_speed`, except siege (fixed `max_range·192/proj_speed`).
- **Direct-fire ranged** (`fire_proj ≠ 0`): one ammo per guy immediately from `fight` (Armed Merchant uses ammo piece
  +7, merchants' own pieces in `fire_ammo`). **Fireships** (UNITTYPE_FIRESHIP) fire and die (SPLODED); their splash is
  centred on the midpoint.
- **MELEE_AND_RANGED** (Mahouts): melee mode in `fight`, a ranged shot via `cavarch_fight`; ATTACKSPECIAL is the
  ranged animation.
- **Siege**: packers unpack first (spell 0x28c). Packable siege units attack units by ATTACK_GROUND at the unit's
  position. Recharge ×1.5 (Bombard ×2) when unsupplied or under fire.
- **Ships**: ATTACK_SIDEWAYS ships face ±90°. ROCKING_ATTACK picks ATTACK1/2 by side. Broadside code exists but no
  type uses it. Patrol Boat has its own animation rule.
- **Anti-air**: ground AA units roll target evasion and then their own `fly_*`. Non-AA shooters roll only their own
  `fly_*`. A jammed AA unit may skip the attack animation (`rnd(100) < jam_unit_radar_prob`).
- **Strafing aircraft**: `do_strafe` → `fire_ammo`, no scatter, the impact point slides along the heading with the
  animation phase (§4.8 step 10).
- **Bombers**: the bomb lands 192 ahead, no accuracy, fall time from `GRAV_Z`.
- **Immortals**: ATTACKSPECIAL melee within 192, plus arrows from ATTACK2/3 releases only. An ATTACK1 variant (30 %)
  fires nothing, unless the art lacks ATTACK1 (then it falls back to ATTACK2) ❓.

### 4.15 RNG draws (call order within the systems; all `game_random.get(0, 0xffff)` unless noted)

| # | Site | Function | Use | Condition |
|---|---|---|---|---|
| 1 | 5fdcdb | `Unit::fight` | odd → AI guard drops the attack | `!p5`, activity GUARD, captain, UNIT_AI_CONTROL |
| 2 | 5fde6e | `Unit::fight` | `% 5 == 0` → keep the current (civilian/non-combat) target | `!p3 !p4`, `recharging == 0`, captain (group check), target is a unit |
| 3 | 5fee89 → 43ccae | `Unit::fight` via `GameAccess::rnd(100)` | `< jam_unit_radar_prob` → jammed AA plays DEFAULT | in range, not rocking / M&R / Immortals / 389, shooter has OBJMASK_ANTI_AIR |
| 4 | 5db21d | `Guy::set_anim` | `% 100`: < 30 ATTACK1, > 70 ATTACK3, else ATTACK2 | per guy starting an attack anim with `rnd = 1` (from fight, hold release, queued attack) |
| 5 | 64cc73 | `Object::fire_ammo` | x within the building footprint | building shooter, per ammo, `x_size·96 − 1 ≥ 1` |
| 6 | 64cca4 | `Object::fire_ammo` | y within the footprint | as 5 (after 5, per ammo) |
| 7 | 67c00c | `Ammo::init` | `% 100 < shooter.fly_low/high` | air target (not heli/missile), shooter not ANTI_AIR |
| 8 | 67c079 | `Ammo::init` | `% 100 ≥ target.fly_low/high` → miss | air target, ground ANTI_AIR shooter |
| 9 | 67c0ba | `Ammo::init` | `% 100 < shooter.fly_low` | after 8 passed, target flying low |
| 10 | 67c126 | `Ammo::init` | `% 100 < shooter.fly_high` | after 8 passed, target flying high |
| 11 | 67c6c6 | `Ammo::init` | ground-order scatter x `% r` | ground order, `r ≥ 2` |
| 12 | 67c703 | `Ammo::init` | ground-order scatter y | as 11 |
| 13 | 67c8b7 | `Ammo::init` | target scatter x `% r` | target shot, `r ≥ 2` |
| 14 | 67c8e9 | `Ammo::init` | target scatter y | as 13 |
| 15 | 678ca7 | `Ammo::do_damage` | miss jitter x `% 41 − 20` (stuck-arrow decision) | no splash, no valid target after the hit tests |
| 16 | 678cc4 | `Ammo::do_damage` | miss jitter y | as 15 |
| 17 | 67baf7 | `Ammo::init_crash` | `rolling = % 7 − 3` | aircraft death |

Order per shot: Ammo::init draws 7–10 (air rolls) precede 11–14 (scatter). At most one of {7} or {8, 9|10}, and one
pair of {11, 12} or {13, 14}. Flight and impact draws (15–16, plus `Object::do_damage`'s own) happen in a later
`Objects::inc_time` ammo loop, in **ammo slot order**. `Ammo::init_crash` also draws 2 values from a **local**
`Random(sx + sy + sz)` (not the game stream). Presentation draws use `internal_random` (`AmmoOut::puncture_ground`,
`dirt_splash`, `explode`, `Particle::*`) and are excluded. ✅

### 4.16 Rules/data read
Type fields (all loaded in engine/sim/rules.hpp; offsets per rules-spec §6.4):
`obj_masks` 0x1e4 (ANTI_AIR 0x80000000, MISSILE 0x8000000, FOOT 0x20), `attack` 0x1e8, `to_hit` 0x1ec,
`attenuate` 0x1f0, `recharge` 0x1f4, `max_range` 0x1fc (**written temporarily** by do_attack/cavarch_fight),
`splash_area` 0x200, `ammo_per_att` 0x208, `proj_speed` 0x20c, `domain` 0x218, `guy_spacing` 0x224, `x_size/y_size`
0x234/0x238, `guy_radius` 0x23c, `big_radius` 0x244, `fly_high/fly_low` 0x250/0x254, `unit_flags` 0x2b4 (ATTACK_SIDEWAYS,
MELEE_AND_RANGED, FIRESHIP, BROADSIDE, SIEGE, TANK, FIRE_WHILE_MOVING, STRAFING, ROCKING_ATTACK, HELICOPTER),
`unit_flags2` 0x2b8 (RETARGETS 1, PACKER 4, HERO 0x20), `role` 0x2c8 (COMBAT 0x10000), **`fire_proj` 0x2cc
(rules = 1; the art rewrites it, §4.2)**, `second_max_range` 0x2d0, `target_size` 0x300, `squad_size` 0x304, `moves`
0x2c0. Building fields: `most_shots` 0x2c4, `base_arrows` 0x2cc (BuildType), building range via
`BuildTypeData::get_building_range`.
Constants (rules.xml value; offset): `target_radius` 96 (Fraction ×192; 0x38), `jam_unit_radar_prob` 50 (0x21c),
`unit_move_speed` 1 (0x4), `city_capture_radius` 10 (0x134), `unit_defensive_respond_range` 4 (0x1c),
`artillery_under_attack_fires_slowly` 1 (0xc20), `maya_garrison_arrows` 1 (0x588), `general_building_range` 1 (0xc5c),
`general_building_attack` 1 (0xc60), `obsidian_archers_attack` 1 (0x944), `antipater_garrison_attack_bonus` 3 (0xb94),
`tikal_temple_hp` 50 (0x4a0).
Type ids hard-coded: 52/53 scholars (inc_time), 54 General, 61/62/400 merchants, 66 militia, 162 Immortals, 267
Bombard, 304 Bomber, 314 Cruise Missile, 315 Nuclear Missile, 351 Aircraft Carrier, 361 Antipater, 389 Patrol Boat,
439 Tower, 441/442/445/446 Stockade/Bunker/Fortress/Redoubt, 521–525 Lookout/OP/ADG/Radar AD/SAM, 532 Tikal.
**Not rules data**: `GRAV_Z` −10.4875f (code), `UnitAnimCat` (.rdata), `move_x/move_y/radius` (compass tables),
`sin_table`, `circle_*`, the graphics-derived items in §4.17, and the ammo piece order and flags (`effects_graphics.xml`).

### 4.17 Animation data dependencies (what the simulation reads from the art)
1. **`unit_graphics.xml` RELEASEEVENT** per GRAPH × civ × age: `anim`, `starttime` → `max(1, ms/67)` frames, `type`
   → ammo gpiece (AMMO name order in `effects_graphics.xml`), `node`. **These alone decide `fire_proj`** and the shot
   frame. Plus INSTANTHIT `ammo`, PLANERELEASE (`starttime`, `anim`, `node`, `angle`), NORELEASE.
2. **Animation frame counts** `animmgr.frames[action]` = `AnimationPacket::get_game_frames(anim)` (3 if the action is
   missing): the guy's `end_time` for every animation, the release validation in §4.2, the AA turret clock (anims 8
   and 12 of building pieces), and the strafing phase. `get_anim_time` (ms, default 200) is used only for the walk
   rescale (movement-spec). **Looping flags** `animmgr.loopings[action]`.
3. **Which actions exist** in a piece's AnimationPacket (`get_animobj`): the attack variant falls back to ATTACK2, and
   TURN_LEFT/RIGHT to DEFAULT.
4. **Muzzle offsets**: `GraphicPieces::get_position(gpiece, node, anim, frame, angle°, pivot°, restrictions)` @90b750
   returns a float vector from a table precomputed by `GraphicPieces::init_position` (pivot nodes 4–7 and each release
   node at every event frame). It is rotated about z by the guy angle in degrees (and by the pivot angle when
   restricted) and scaled by `guy_scale·piece scale` (+0x88) or `build_scale`. Truncated and added to the ammo start
   point, so it changes the flight time and trajectory. Needs the BH3 node hierarchy + BHA pose at that frame ❓
   (asset/animation work; §11).
5. **Pivot restrictions** (`GraphicPieces.pivot_restrictions[type − 50]` +0x14): gate releases on turret alignment;
   `node_flags` from `Guy::process` turret stepping.
6. Crew guy track offsets (movement-spec §9.3) and z from the terrain mesh (start/impact z).

### 4.18 Open questions
1. ❓ `Spline::calc_from_dir` / `calc_nuke_spline` (rockets, torpedoes, AA missiles, cruise/nuclear missiles): flight
   time, homing re-plans every 4 frames, catch radius. Float-heavy, needs its own pass with the asm.
2. ❓ `GraphicPieces::init_position` / `get_position`: the exact float pipeline for muzzle offsets (rotation helper
   `Transform<float>::rotate` taking degrees; scale).
3. ❓ The global start offset for OBJMASK_MISSILE shooters (`graphic_events`+0xc8..+0xd0): its value and writer.
4. 🟡 `guy_flags & 4` (double-speed attack anim) has no setter in the binary. Confirm by scanning `walk_data` loads
   too.
5. ❓ Order vslots used by `fight`/`execute_events` (+0x2c, +0x54, +0x94, +0x9c, +0xa8, +0xd0, flag 0x10): match them
   to the order classes (orders-spec §3.3).
6. ❓ `is_move()` @46f050 / `is_air()` @46f000 order sets (lead targeting).
7. 🟡 Bomber ammo inherits `accuracy` from the slot's previous ammo. Check that walked/saved state keeps this
   deterministic across save/load.
8. ❓ The splash at `rr = 10` reads `move_x/move_y[441]` (past the table). Only reachable with `splash_area ≥ 36`.
9. ❓ Validation step 6 runs only for `gpiece / num_unit_pieces_per_crew == 0`: what this selects.
10. ❓ The aircraft carrier launch sequence inside `Guy::execute_events` (writes unit +0x5c/+0x44).
11. 🟡 The `game.balance` attacks/hits counters (semaphore bit) are assumed to be stats only.
12. ❓ attachments-spec names ammo piece 0 "BuildingTraceBullet", but the first `<AMMO>` in the file is
    "BuildingArrow " (trailing space). Confirm the load order (it matters for `first_ammo_piece + k` in `fire_ammo` /
    `get_shot` and the Armed Merchant's +7).
13. 🟡 The shooter `hold_frames += 1` per frame in flight (§4.9) on top of `Object::die`'s estimate: check against
    frame-spec §8.2.

### Evidence (section 4)
| Function | Address | Section |
|---|---|---|
| `Unit::fight` | 5fd4d0 (asm `re/decomp/combat_C/asm/`) | 4.3 |
| `Unit::cavarch_fight`, `Unit::do_attack`, `Unit::do_move` | 5ff4b0, 5f1b80, 5f7b30 | 4.4 |
| `UnitData::recharge`, `UnitData::in_supply` | 60fdf0, 609ef0 | 4.3.12 |
| `Unit::set_attack`, `Unit::set_attacking`, `Unit::set_anim` | 5fce70, 5ff5b0, 616f40 | 4.3 |
| `Guy::set_anim`, `Guy::inc_time`, `Guy::move`, `Guy::execute_events` | 5da300, 5d9e10, 5d9240, 5d99c0 | 4.5 |
| `Unit::inc_time`, `Unit::execute_events`, `Unit::process` (HOLD_ANIM clear) | 610b40, 60edc0, 610bc0 | 4.5.3 |
| `GraphicEvents::init_unit_events / verify_load / get_event_group / execute_game_events` | 8e2520, 8e4780, 8e23c0, 8e48e0 | 4.2, 4.5.4 |
| `AnimationPacket::get_game_frames / get_anim_time`, `GraphicPieces::get_position / has_restrictions / get_gpiece_type / verify_ammo_flags` | 918cc0, 918c40, 90b750, 90b640, 9096e0, 9072e0 | 4.5, 4.17 |
| `Object::fire_ammo`, `BuildData::get_shot`, `Objects::add_ammo` | 64c8b0, 62dd90, 658b10 | 4.6, 4.7 |
| `Ammo::init`, `inc_time`, `check_hit`, `hit_target`, `do_damage`, `init_crash`, `close`, `process` (empty) | 67bbf0, 67d380, 678d90, 678f90, 678060, 67b800, 6791a0, 67d370 | 4.8–4.12 |
| `Objects::find_units / find_builds`, `Vector<int>::length / cross` | 65a620, 65a120, 46e700, 46e730 | 4.3.11 |
| `Build::process`, `Build::do_attack`, `Build::find_target`, `Wall::inc_time` | 61edf0, 6228f0, 622c80, 63fb60 | 4.13 |
| `BuildData::get_garrison_arrows / get_base_garrison_arrows / most_shots / attack / max_range`, `ObjectData::count_inside` | 62e3d0, 62e340, 62e2f0, 62e610, 62e470, 64dda0 | 4.13.3 |
| `UnitTypeData::is_siege / is_tank / is_missile`, `UnitData::is_flying_low` | 470460, 470450, 61d430, 60a140 | 4.8 |
| `GameAccess::rnd` | 43cca0 | 4.3.9 |
| `GRAV_Z` writer | 8ffce3 (`GraphicPieces::init`) | 4.1 |

## 5. Damage

All arithmetic below is int32 with C truncating division unless stated; `trunc256(x)` is the binary's
`(x + ((x >> 31) & 0xff)) >> 8`, i.e. `x / 256` truncated toward zero. "Attacker" is the object whose
`do_damage` runs (`this`), "target" is `objects[whom][ox]`. Type offsets are rules-spec §6.4 / §7.

### 5.1 The stat getters (virtual, per object) ✅

Re-read from the decompiles and the disassembly on 2026-10-23 (the decompiler had dropped the return paths of
`ObjectData::attack` / `armor`, `BuildData::attack` / `hits` and `UnitData::max_range`); ported in
`engine/sim/stats.cpp`. Conventions: `ages` = `LeaderDataEncrypt.ages ^ 0x62766` (the leader's age count);
"owns(t)" = `LeaderData.num_units[t − 50] ≠ 0`; "general(t)" = owns(t) and `ObjectData::has_general(0, t) ≥ 0`
(a hero of type t in range: not simulated yet, −1); "rare(g)" = bit `g − 6` of `rare` (+0x6d98) or
`rare_conquest` (+0x6dc0); "dutch set" = types 0x3d, 0x3e, 400, `is_caravan` or `is_supply`, and not a government
hero (`is_gov_hero`, vslot 0xc8: `unit_flags & 0x4000000`; the hero exclusion only from patch version 9, which
every Extended Edition game is); `pct(k, v) = (k + 100)·v / 100`. Hero constants are read behind the stubs.

**`ObjectData::attack` @6469f0** (units and buildings; `type.attack` is already ×10):
```
a = type.attack
if is_caravan and has_preq(CARAVAN_ATTACK_BONUS 0x2ba): a = C.caravan_attack_bonus       // replaces
if tribe bonus 22 (Dutch) and C.dutch_attack_bonus ≠ 0 and is_active and the unit is in the dutch set:
    a += ages × C.dutch_attack_bonus × 10
return a
```

**`UnitData::attack` @6103c0** = `ObjectData::attack`, then **if that is 0 return 0**, else in order:
Terra Cotta (wonder 0x211) `+ C.terra_cotta_attack×10`; general(0x164 THECITIZEN) `+ thecitizen_attack_bonus×10`;
HORSE_ARCHER (obj_masks 0x4000) with rare(32 Obsidian) `+ obsidian_archers_attack×10`, and again for FOOT_ARCHER
(0x400); aircraft (domain 2) with Space Program (wonder 0x21e) `a = pct(space_air_attack, a)`; then the hero
terms, each `+ constant×10`: general(0x166 ALEXANDER) `alexander_attack` (+ `alexander_heavy_inf_attack` for
`is(0x84)`); general(0x167 NAPOLEON) for siege (type vslot 0x10c) `napoleon_siege_attack`; `is(0xe3)` and
general(0x168 PARMENIO) `parmenio_heavy_cav_attack`; `is(0x109)` and general(0x16a PTOLEMY) `ptolemy_siege_attack`;
`is(0xbb)` and general(0x16d SPITAMENES) `spitamenes_horse_archer_attack`; `type.where == 0x1ac` (stable) and
general(0x175 BLUCHER) `blucher_cav_attack`; general(0x172 DJEZZAR) `djezzar_attack`; general(0x16c MEMNON) and
`is(0x18a)` `memnon_merc_attack`; `where == 0x1ac` and general(0x162 THEMONARCH) `themonarch_cav_attack`.

**`BuildData::attack` @62e610** = `ObjectData::attack`; then only for `is(0x1b7 tower)`, forts (type vslot 0xfc)
or city buildings (`flags & 0x20` with `city ≥ 0`): a general garrisoned (`count_inside(COUNT_TYPE, the leader's
general type)`: `tribe_can_type(types[0x36]) ? 0x36 : its tribe substitute`) `+ general_building_attack×10`;
rare(32 Obsidian) `+ obsidian_archers_attack×10`; owns(0x169 ANTIPATER) and `count_inside(COUNT_STRICT_TYPE,
0x169)` `+ antipater_garrison_attack_bonus×10`. (Garrisons are not simulated: `count_inside` is 0.)

**`ObjectData::armor` @647db0**: `type.armor` (+0x214), plus `ages × C.dutch_attack_bonus` (no ×10) for the Dutch
set under the same conditions as `attack`. **`Unit::update_armor` @6054c0** (from the captain, copied down the
`o_down` chain): `myarmor = ObjectData::armor`, `+ C.cattle_citizen_armor` for types 0x32 / 0x33 with rare(37
Cattle). **`UnitData::armor` @610160** (the live value): `a = myarmor`; then `h = o` (the unit itself) when
`is_active and is_hero`, else `h = HeroesData::find_hero(x, y, leader, 0, −1, r)` with `r = (x_size + y_size)·96`
for `is_building` else 0; if `h ≥ 0`: `+ general_rally_armor × (get_general_upgrade + 1)`, owns(0x16a) and
general(0x16a) `+ ptolemy_armor`, owns(0x16b) and general(0x16b) `+ darius_armor`, owns(0x170) and general(0x170)
`+ wellington_defense`, owns(0x175) and `where == 0x1ac` and general(0x175) `+ blucher_cav_armor`, owns(0x16c) and
`is(0x18a)` and general(0x16c) `+ memnon_merc_armor`, general(0x164) `+ thecitizen_armor_bonus`, `where == 0x1ac`
and general(0x162) `+ themonarch_cav_armor`. **`WallData::armor` @63fa60** (buildings): `ObjectData::armor`, `+
thesenator_build_armor` when the building is an active hero that `is(0x161)`, or owns(0x161) and
`find_hero(..., 0x161, r)` finds one; then **halved while not active** (under construction).

**`Object::update_hits` @647010**: `myhits = types[t].hits` with t = PARTISAN 68 / MINUTEMAN 67 / MILITIA 66 for
citizens 0x32 / 0x33 when the leader's `tech` bit of that type is set (highest first). **`Unit::update_hits(p)`
@60e930** (a follower defers to its captain; the captain's value is copied down the chain):
```
h = Object::update_hits
if unit_masks2 & 0x200 (marine transport): h += C.americans_marine_hp_bonus
if rare(24 Copper) and (domain 1 or where in {0x1ae, 0x1af}): h = pct(copper_factory_hp, h)
if rare(45 Bananas): h = pct(bananas_hp_bonus, h)
if tribe bonus 18 (Iroquois) and C.iroquois_extra_hits ≠ 0 and where == 0x1ab (barracks) and
   (type +0x1e8 (obj attack) ≠ 0 or is(0x45)): h = pct(iroquois_extra_hits, h)
if tribe bonus 22 (Dutch) and C.dutch_hp_bonus ≠ 0 and the unit is in the dutch set: h += ages × dutch_hp_bonus
if is(0x3a spy): h = C.spy_upgrade_hp[clamp(get_spy_upgrade, 0, 3)]                 // replaces
if is_hero: g = get_general_upgrade; h += g·g·h / 2
if the unit is a merchant (0x3d, 0x3e, 400) or is_caravan, and tribe bonus 4 (Nubian) and C.nubian_hit_points ≠ 0:
   h = pct(nubian_hit_points, h)
if is_supply: h += C.supply_hp_upgrade[get_supply_upgrade]
myhits = h
```
`UnitData::hits` @610890 = `myhits`. **`BuildData::hits(p)`** @62e740: `p ? myhits : construct_hits`, scaled while
an active building's queue head is DISBAND (0x29a) or DEPOPULATE (0x286): `max(1, (int)(((float)(T − j) ×
(float)h) / (float)T))` with `T = train_time(head type)` and `j` the head's job counter (SSE single).
`WallData::hits(p)` @642bb0 = `p ? myhits : construct_hits`.

**`UnitData::max_range` @60fe90**: `r = type.max_range`; **if 0 return 0**. Siege (vslot 0x10c): tribe bonus 8
(Turks) `+ turk_siege_range`, Statue of Liberty (0x219) `+ liberty_siege_range`, Eiffel (0x21c) `+
eiffel_siege_range`; not siege: owns(0x165 THECEO) and `r ≠ 0` and general(0x165) `+ theceo_unit_range`. Then
owns(0x16a) and general(0x16a) `+ ptolemy_range_bonus`; owns(0x167) and siege and general(0x167) `+
napoleon_siege_range`; domain 1 with rare(43 Rum) `+ rum_naval_range`; FOOT_ARCHER (0x400) or HORSE_ARCHER
(0x4000) with rare(32 Obsidian) `+ obsidian_archers_range`. `UnitData::min_range` @6100b0 = `type.min_range`.

**`BuildTypeData::get_building_range(who)` @639990**: `r = type.max_range`; for towers (`is(0x1b7)`) and forts
(vslot 0xfc, `is(0x1bb)`): Colosseum (0x212) `+ colosseum_fort_range`, tribe bonus 11 (British) `+
british_tower_range`. Forts: tribe bonus 6 (Romans) `+ roman_fort_range`, then `+ fort_upgrade_range[k]` with k =
3 / 2 / 1 / 0 for FORTRANGE4 / 3 / 2 (0x2d6 / 0x2d5 / 0x2d4, `has_preq`) / none. Towers: Romans `+
roman_fort_range`, then `+ tower_fort_range[k]` with k from the militia line **tech bits** (PARTISAN 68 → 3,
MINUTEMAN 67 → 2, MILITIA 66 → 1, else 0). **`BuildData::max_range` @62e470** = that, `+ general_building_range`
with a garrisoned general (as in `attack`) for towers, forts and city buildings; city buildings whose city has a
temple (`city_flags & 0x80`): `+ get_temple_range` (`temple_upgrade_range[k]`, k = 3 / 2 / 1 / 0 for TEMPLERANGE4
/ 3 / 2 = 0x2d0 / 0x2cf / 0x2ce), `((tikal_temple_hp + 100)·v + 99)/100` with Tikal (0x214).

**`LeaderData::get_general_upgrade` @6e0830** counts GENERALS_UPGRADE_1..3 (0x305..0x307) and
**`get_spy_upgrade`** @6e1090 SPIES_UPGRADE_1..3 (0x308..0x30a), with `has_preq` (the BUY_SELL / Nubian rule does
not apply to those ranges). **`Leader::calc_unit_stats` @6cf970**: `calc_attrition`, `calc_anti_attrition`, then
for every valid unit slot: `update_los`; captains also `update_hits(0)`, `update_speed`, `update_armor`.
`Unit::init` and `Unit::set_type` call the same four. **`Leader::calc_wall_stats` @6cf7c0**: every valid building
and wall: `update_construct_time` when not active, `update_hits(0)`, `update_los`.

### 5.2 `ObjectData::get_damage(ox, whom, angle, splash, overkill, &death_type)` @644130 ✅

Callers: `Object::do_damage` (with `overkill = 1`) and `Object::compare_target` (AI/auto target scoring). Pure
except for the `death_type` out parameter. Let `A` = attacker obj_masks (`type+0x1e4`), `D` = target
obj_masks, `bal = combat_table[att.type−50][tgt.type−50]` (rules-spec §11, int16).

```
death_type = 2 (DEATH_SHOT)
armor  = target.armor()                       // vslot 0x124
attack = attacker.attack()                    // vslot 0x120
(att.get_age() and tgt.get_age() are called; results unused)
if att.type in {CASTLE 444, FORTX 443} or att.flags & CITY:
    if leader(att).has_preq(GUNPOWDER_AGE 546): A = (A & ~ARCHERY) | GUN     // see note 1
d = attack * bal / 100
if A & MUSKET_INF:            armor = armor * 133 / 100
if att.is_build and D & SIEGE and tgt.is_unit and tgt.type.unit_flags2 & PACKER and !(tgt.unit_masks & PACKED):
                              d /= 3
if att.is_build and tgt.is_unit and tgt.attack() != 0 and tgt.is_moving():
    if !(D & FOOT): if tgt.type.is_siege or tgt.is_supply or tgt.is_caravan: d /= 2
    else:           d = d*3/4                  // (3d + sign fix) >> 2
if att.is_build and tgt.is_build and !tgt.get_build().is_active():   d <<= 2
if tgt.is_build and (att.type in 50..53 or att.is(MILITIA 66)):
    if world.wdata(tgt).owner (WData+0xf) != att.who:                   d /= 2
if att.is(V2ROCKET 313) and tgt.is_build and !tgt.get_build().is_active(): d /= 2
if att.is_unit and att.domain == AIR and !(A_type & MISSILE) and tgt.is(REDFORT 534, strict):
                              d = (100 − C.red_fort_air_defense) * d / 100
if tgt.is_unit:
    if tgt.unit_masks & PACKED: d *= 2
    if tgt.type.unit_flags2 & PACKER and !(tgt.unit_masks & PACKED): armor += 1
if A & EXPLOSIVE: death_type = 3;  if A & BOMBARD: death_type = 3
if att.is_wall (buildings): death_type = (att.get_build().get_shot() == 4) ? 3 : 2
if att.is_unit and leader(att).num_units[WELLINGTON 368] != 0 and att.has_general(0, 368) >= 0
   and tgt.type.where >= 0 and types[tgt.type.where].is(FACTORY 431):     d += C.wellington_siege_attack
if att.is_unit and att.type.where == BARRACKS 427 and leader(att).has_tribe_bonus(15):
    k = C.japanese_damage; a = min(ages, epoch[0]) (both XOR-decoded from LeaderDataEncrypt)
    if k < 0: k = −(k * a)
    d = (k + 100) * d / 100
if (att.is_build or (att.domain == GROUND and tgt.domain != GROUND)) and tgt.is_unit and
   (tgt.unit_masks & UNIT_SEVERE_ATTRITION or
    (tgt.domain == SEA and game.team_style == 2 and leader(whom).get_target() != att.who)):  d *= 2
if splash:
    if tgt.is_unit: d /= tgt.type.uber_size
    d = att.type.splash_percent * d / 100
    if tgt.is_unit:
        if tgt.type.is_siege and tgt.type.unit_flags2 & PACKER and !(tgt.unit_masks & PACKED): d *= 3
        if tgt.is(BARK 323): d = d * 25 / 100
if tgt.is_unit and tgt.z < 0:   d = trunc256(C.river_modifier * d)        // standing in a river
if tgt.is_unit and tgt.unit_masks & UNIT_DECOY:  armor = 0; d = max(att.attack(), d) * 1000
if tgt.is_wall:
    w = tgt.get_wall()
    if w.in_unfriendly_territory() (Build vslot 0x184) and !(game.semaphore[1] & 2):
        armor = 0
        if !w.is_build or w.get_build().type.attack == 0: d <<= 2
if att.is_unit and tgt.is_unit and !(A & CIVILIAN) and !(D & CIVILIAN) and !(A & AIR) and !(D & AIR)
   and (A & NAVAL) == (D & NAVAL) == 0:
    f = flank_level(tgt.angle, angle)        // §5.2.1
    if f: b = C.flank_bonus
          if D & VEHICLE: b = trunc256(C.vehicle_flank_bonus * b)
          elif D & MOUNTED: b = trunc256(C.cavalry_flank_bonus * b)
          d = (b * f + 100) * d / 100
hp = (d + 5) / 10 − armor                     // attack values are ×10: round to whole hit points
if overkill and att.max_range() != 0 and att.is_unit and tgt.is_unit and tgt.damage_frame != 0
   and frame − tgt.damage_frame < C.overkill_frames and att.get_captain() != tgt.damage_o:
    hp = trunc256(C.overkill_damage * hp)
    if tgt.is(CATAPULT 265) and !att.type.is_siege: hp /= 2
if D & (LIGHT_INF | MODERN_INF | MUSKET_INF) and world.wdata(tgt).flags & 8 (rocky):
    hp = trunc256(C.rocky_modifier * hp)
if tgt.domain != AIR and att.domain != AIR and !att.type.is_siege and tgt.z < att.z (signed):
    hp += (int)((att.z − tgt.z) * C.height_bonus * hp) / (C.height_increment * 100)
if tgt.is_unit and tgt.unit_masks & UNIT_ENTRENCHED and !att.is(FLAMETHROWER 131):
    s = entrench_sector(tgt.trench_angle, angle)        // §5.2.1
    if splash or s == 0:
        hp = trunc256(C.entrenchment_modifier * hp)
        if tgt.unit_masks2 & UNIT2_ANTIPATER_ENTR: hp = trunc256(C.antipater_entrench_bonus * hp)
if C.russian_cossack_damage and leader(att).has_tribe_bonus(13) and att.is_unit and att.type.where == STABLE 428
   and (tgt.is_supply or tgt.type.is_siege):  hp = (C.russian_cossack_damage + 100) * hp / 100
if hp < 1 and !(att.domain == GROUND and tgt.domain == SEA) and (D & AIR) == ((A >> 3) & AIR) and !splash:
    hp = 1                                    // (A >> 3) & AIR is the attacker's ANTI_AIR bit
if tgt.type == SUPERCOLLIDER 541 and att.domain == AIR and C.super_immune: hp = 0
if tgt.is_build and tgt.get_build().is_active() and tgt.flags & CITY and tgt.city >= 0
   and cities[whom][tgt.city].race (+0x5f) == att.who:  hp = trunc256(C.recapture_city_modifier * hp)
return hp
```

Notes: 1) the decompiler hides the `| GUN` (asm @6442ab); within get_damage neither bit is read afterwards, so the
change is dead here (it only matters if another reader shares the copy, which none does). 2) `tgt.z < 0` is the
"in river" test (river beds are below 0). 3) `is_build`/`is_wall`/`is_unit` are object vslots 0x20/0x1c/0x18;
`get_build`/`get_wall` are folded `return this` casts (0x3c/0x40, 0xac/0xb0); `Build::is_active` is `flags & ACTIVE` (inlined when the vtable is Build's).
4) `BuildData::get_shot` @62dd90 is rules-only: STOCKADE 441 / FORTRESS 445 → 1; BUNKER 442, AIRDEFENSE 523,
RADAR 524 → 2; REDOUBT 446 → 4 (explosive: DEATH_SPLODED); LOOKOUT 521 / OBSERVATIONPOST 522 → 0; SAM 525 → 5;
any other building by the owner's age count `a` (LeaderDataEncrypt.ages ^ 0x62766): `a < 3` → 0, `3 ≤ a < 5` → 1,
else 2. `WallData::in_unfriendly_territory` @63eca0: the W tile's owner (WData+0xf) is ≥ 0, not the building's
owner, and the two are not mutually allied (`diplos[a][b] == 2` both ways).
5) Whether a unit-type modifier is tested on the attacker's mask `A` (the local copy, possibly modified) or the
target's `D` is as written; the flank multiplier tests the **target's** VEHICLE/MOUNTED bits.

#### 5.2.1 Angles

`flank_level(target_angle, attack_angle)` (`flanking` @92cfe0 plus the cone test inlined before it): with
`d = target_angle − attack_angle − 0x80000000` (u32): `d < 0x2aaaaaaa` or `d > 0xd5555555` → 0 (the attack
comes within ±60° of the target's front); otherwise `1 + ((d + 0xa0000000) u> 0x40000000)`: 1 when
`0x60000000 ≤ d ≤ 0xa0000000` (attacker behind the target, ±45°), 2 on the flanks. So with stock constants
(flank_bonus 50) a side hit on an infantry target is +100 % and a rear hit +50 % (see open question 3). Against
MOUNTED targets the bonus is `trunc256(cavalry_flank_bonus × 50) = trunc256(40 × 50) = 7` %, against VEHICLE
targets `trunc256(33 × 50) = 6` %: CAVALRY/VEHICLE_FLANK_BONUS are parsed as plain integers ("40% (of base flank
bonus)") but used as 8.8 fractions, so flanking cavalry barely matters. Keep the quirk. `attack_angle` is the `angle` argument of `do_damage` (the shot/blow direction; see §4).

`entrench_sector(trench_angle, angle)`: `x = trench_angle − angle` (u32); if `x + 0x55555556 < 0xaaaaaaac`
(unsigned) → `1 + ((x + 0x20000000) u> 0x40000000)`, else 0. Entrenchment reduces damage when the sector is 0
(the hit comes from the entrenched front, ±60° of `trench_angle + 180°`) or for splash.

### 5.3 `Object::do_damage(ox, whom, angle, guys, ammo, mult, splash, no_react)` @64a480 ✅

Signature from the mangled name (`?do_damage@Object@@QAEXHHKHHHHH@Z`); `guys` is only forwarded to the recursive
building-collateral calls, `ammo` is the Ammo slot index (−1 for melee/direct), `mult` an 8.8 multiplier
(0x100 = 1.0), `splash` 0/1, `no_react` 1 for collateral calls. Callers: `Unit::fight` (melee: one call per
guy with `mult = 0x100`, `ammo = −1`), `Ammo::do_damage` (projectile hits and splash), `Nuke::do_damage`, and
itself.

```
if mult < 1: return
if att.is_unit and att.unit_masks & UNIT_DECOY: return                    // decoys deal no damage
hp = get_damage(ox, whom, angle, splash, overkill=1, &dt)
death = (ammo >= 0 and GraphicPieces::verify_ammo_flags(ammo.gpiece, 0x10 FLAME)) ? 4 (SCORCH) : dt
(balance-test statistics when game.semaphore[1] & 2: not simulation)
if tgt.is_unit and !no_react:
    Unit::target_opportunity(units[whom][tgt.get_captain()], att.o, att.who, 0)   // §2: the victim reacts
    if tgt.unit_masks & UNIT_ZOC: hp *= 2
    if tgt.damage_frame == 0 or frame − tgt.damage_frame >= C.overkill_frames:
        tgt.damage_frame = frame; tgt.damage_o = att.get_captain(); tgt.damage_who = att.who
if !(att.type.obj_masks & MISSILE) and att alive: Object::attempt_launch(tgt, att.x, att.y)  // carriers/airbases scramble fighters (AIR_PATROL order to the first idle BIPLANE-line (287) fighter inside)
(sounds when !splash: presentation)
if att.is_unit:
    v = hp * mult; if v <= 256: v = 256          // at least 1 hp before the divisions
    if ammo >= 0: v /= att.type.ammo_per_att
    v /= att.type.uber_size
elif att.is_build:
    v = hp * mult / att.type.ammo_per_att
q = v / 16;  hp_int = q / 16;  frac = q % 16       // C truncation; 1/16-hp remainder
```

Then leader bookkeeping (all walked LeaderData, so part of the checksum): `hits_current_frame` (+0xa5c) of
attacker and target += 1, or 5 when the attacker is siege and the target a building; if owners differ:
`leaders[att.who].treaties[whom] |= 2` and `leaders[whom].treaties[att.who] |= 2` (LeaderData +0x94, the
"has attacked" bit), and the target owner's stamps `attack_stamp` / `raid_stamp` / `capital_stamp` / `ally_stamp`
(+0xf4 / +0x114 / +0x134 / +0x154, indexed by the attacker's player) and the city +0x14/+0x18 stamps are set with
300-frame throttles (the messages and sounds they gate are UI; the stamps are walked state). A
capital or city under attack can also set a local flag that makes `Armies::emergency(whom)` run after a
non-lethal hit for AI owners.

*Port status (2026-10-24):* the whole stamp block is ported in `combat_damage.cpp` (`damage_warnings`, before `take_damage`, with the
`Armies::emergency` call after a non-lethal hit); the leader stamps are `Leader::attack_stamp / raid_stamp / capital_stamp / ally_stamp`
(the last two new, walked, `kSaveVersion` 4). The branch structure (decompile order): an active building: "heavy" when the attacker is
siege or `damage + hit >= hits / 4` (`/ 10` for a city; the type vslot 0x104 test is read as clear), a city centre or a building of a
city: heavy and not capital sets the leader's raid + attack stamps and the city's attack + raid stamps (city stamp > 300 old), heavy
capital the leader's attack + raid + capital stamps (capital_stamp > 300 old), light the raid stamps (city raid stamp > 300 old);
then the ally stamp; a worker whose action targets an own active building of a city counts as a light hit on that city; in the target's
own territory a light building hit, a worker, a caravan or a merchant stamps `raid_stamp[attacker]`, everything else
`attack_stamp[attacker]`. Each stamp that passes its throttle also logs a `GameEvent` 8 with the alert class (ui-port.md 11.2); a plain
`GameEvent` 8 per victim and frame carries the damage.

```
killed = tgt.take_damage(hp_int, frac, death, ammo, 0, angle, att.o, att.who, splash)   // vslot 0x16c
if killed:
    if !tgt.is_wall: return
    leader(att).buildings_razed++ (+0x818)                       (Steam stats: UI)
    kv = types[tgt_type].get_kill_value(whom)                    // §6.4
    leader(att).score_combat (+0x44) += kv/5 if not allies, else −= kv/5 clamped at 0
    if att.domain != AIR and !splash and !allies(whom, att.who) and tgt.is_build: Build::plunder(tgt, att.who)
    kills_current_frame (+0xa5a) of both += (tgt was an active building ? 5 : 2); return
if att.is(FLAMETHROWER 131):
    if tgt is not a unit and cannot carry aircraft: n = tgt.num_inside(1); eject_contents(tgt, 1, −1, 0, 1);
        if n: spawn up to x_size/2 citizens (type 50 or its tribe graft) at the target, go_inside + come_out,
        then close(4) each (burned) — only if fewer than x_size/2 DeathObjs are already on build pieces
    elif tgt.unit_masks & ENTRENCHED: clear ENTRENCHED, ANTIPATER_ENTR, MARINE_ENTRENCH; remove the trench graphic
if !tgt.is_build: return
if att.is(SUNKAWAKAN 205):  resource-gather building hit (good 0, 1 or 4): leader(att).leftover[g] +=
    trunc256(lakota_cav_dmg_bonus × gather_rate×16 × hp_int); whole units carried into bucket[g]   // XOR-encoded LeaderDataEncrypt
if tgt.type is a garrison type (type vslot 0x108) and (att.type in {FIRERAFT 334, HEAVYFIRERAFT 335} or att.domain == AIR):
    eject_contents(tgt, 1, −1, 0, 1)
building collateral (§5.5)
if tgt is a city (BuildType::is_city) and whom != att.who and !Build::check_capture(tgt, att.o, att.who)
   and tgt.flags & CITY and active and tgt.damage >= tgt.hits():
    siege attacker of a non-human AI army: Army::charge(army, ox, whom)
    city +0x1c "last attacked" stamp (300-frame throttle) and UI event
```

### 5.4 `Object::take_damage(hp, frac, death, ammo, quiet, angle, att_o, att_who, splash)` @652020 ✅

Returns 0 (survived), 1 (died) or 2 (a decoy died and passed the overflow on). `quiet` (param 5) is 1 for
attrition and scripted damage: it suppresses the flock roll, war declaration, peasant alarm and danger flags.

```
if !(flags & VALID): return 0
if hp < 1 and frac < 1: frac = 1                           // every hit does at least 1/16 hp
if !quiet:
    if !(game.semaphore[0] & 4) and difficulty < 2: leader(this).frame_attacked (+0xa40) = frame
    if damage == 0 and this.is_wall and rnd(100) < 5:                         // RNG @6520ef
        if this.type is a fort (type vslot 0xfc) or is(TEMPLE 437) or is(TOWN 415):
            if att_o >= 0 and types[att].is_siege:
                n = (game_random.get(0,0xffff) % 2) + 3                         // RNG @652199 (signed %2)
                Objects::add_flock(this tcoord x, y, −1, n)                      // birds; draws more RNG
    if att_o >= 0 and attacker is a MISSILE (not is(NUCLEARMISSILE 315)) and game.war_allowed() and att_who != this.who
       and this leader is not human and the target tile's owner is this.who and !is_enemy(att_who):
        Leader::action_declare(this.who, att_who, 0, !human, 0)                // AI declares war
t = damage_frac + frac (as signed char); damage += hp + t/16; damage_frac = t % 16
(hits() is called and discarded)
if this.is_wall and !quiet:
    if !this.is_active() and damage>0 and the attacker is not an aircraft:
        job_counter (WallData+0x48) −= 50·hp_added, clamped at 0                 // unfinished sites lose progress
if !this.is_build:                                                            // units
    AI peasant alarm: owner not human/co-op, difficulty > 1, this is a peasant inside one of its cities'
    areas (find_city_at) whose city is active and not flag 0x40: build a temporary Group with the city
    building and Group::action_alarm (garrison the citizens)
else (buildings):
    if !quiet and att_who != this.who: build_masks |= 0x30 unless the attacker is a citizen/militia on a tile
        owned by att_who; the building's city gets flags |= 0xe and the same AI alarm for forts/cities
    if damage >= hits() and !can_carry(AIR): eject_contents(this, 1, −1, 0, 1)
    if flags & CITY and is_active(): if damage >= hits(): flags |= NEW_THINK; damage = hits()   // cities never die here
                                     return 0
thr = hits()
if this.is_unit:
    if !quiet: Unit::set_in_danger(this, 0)
    u = type.uber_size
    if u > 1: thr = (this is the captain and curr_uber_size == 1) ? thr − (u−1)·thr/u : thr/u
    if att is valid and leader(att_who).has_tribe_bonus(0) and attacker.type.where in {427, 428, 432}:
        captain.unit_masks |= UNIT_AZTEC_DAMAGE
if damage < thr:
    captain.flags |= NEW_THINK
    if this.is_wall and !is_active() and owner not human and hits() <= 2·damage and build_masks & 0x2000:
        Object::disband(this, 0); return 1                                    // AI abandons a damaged site (no RNG: a building)
    return 0
-- death --
if this.is_unit and !decoy and death == 3 (SPLODED) and (has_objmask(FOOT) or has_objmask(MOUNTED)):
    every guy in [0, guy_mark) and [squad_size, guys.len): Guy::set_angle(ammo < 0 ? angle + 0x80000000 : find_angle(...), 1)
gpiece = ammo >= 0 ? ammo.gpiece : −1; deg = ammo >= 0 ? (float) angle_to_degrees(find_angle(ammo dir)) : 0.0
decoy = this.is_unit ? unit_masks & DECOY : 0
caravan danger: a land-domain captain caravan on a hostile tile (or hit by a building) → Caravans::new_danger (economy-spec §6)
if this.is_wall: leader.buildings_lost++ (+0x81c); ScenarioData::last_razed[who] = o
elif captain, curr_uber_size(0) == 1 (the last member of the chain) and !decoy:
    leader.units_lost++ (+0x810)
    if att_o >= 0:
        leader(this).deaths_current_frame (+0xa58)++; leader(att).kills_current_frame (+0xa5a)++; leader(att).units_killed (+0x80c)++
        victim = this, or for a TRANSPORT-flag type its first passenger (inside_down/inside_down_who); skip if none or not a unit
        kv = types[victim.type].get_kill_value(victim.who); leader(att).score_combat ± kv/5 (as in 5.3)
        plunder: if !splash and not allies: Aztec (tribe bonus 0) barracks/stable/dock attackers → Unit::plunder(this, att, 1, 0);
            else unless the victim is(CARA 59)/is(GENERAL 54)/is(SUPPLYWAGON 63): THEDESPOT (352) or SPITAMENES-type hero nearby with
            thedespot_unit_plunder → plunder(…, 0, 1); then Inca (tribe bonus 2) with inca_refund: plunder(this, this.o, this.who, 0, 0)
overflow = damage − thr
die(death, gpiece, deg)                                                        // vslot 0x158
if !decoy: return 1
if overflow > 0 and captain >= 0 and captain != o: captain.take_damage(overflow, 0, death, ammo, quiet, angle, captain, who, 0)
return 2
```

Uber chains ("one unit = several figures"): Hoplites have UBER_SIZE 3, i.e. three `Unit` objects linked by
`o_up/o_down`, each with its own `damage`; a member dies at `hits/uber` (the last one at the remainder). Melee and
missile damage from a chain is divided by the attacker's `uber_size` per member, so a full chain deals the
listed damage per volley.

### 5.5 Building collateral (siege splash between buildings) ✅

At the end of `do_damage` on a building target (the target survived): for each cell `k` in `[0, circle_radius[1])`
of the `circle_x/circle_y` W-cell spiral around the target's W tile (the same tables `Objects::find_units` uses,
§2), walk the tile's object list (`WData.down/down_who`, newest first) and for every object that passes
`Search::valid_search(who, 3, att.who)`, is alive and on the map, `valid_filter(FILTER_ALL, 9)` and
`valid_filter(ox, whom, 10)` (i.e. not the target itself), and is not suppressed by the Korean "build under fire"
power: if `vector_dist(object, target) <= 192 × max(target.type.x_size, target.type.y_size)`, the attacker is not
an aircraft and not a siege ship:
- non-siege attackers or non-ground siege: radius `r = max(att.max_range()×192, 0x180)`; skip the object when
  `vector_dist(object, attacker) > r`; else `m = mult / 8`;
- ground siege: `m = mult / 4`;
- `do_damage(object.o, object.who, angle, guys, ammo, m, splash = 0, no_react = 1)` (recursive; collateral of
  collateral is possible because the recursive call runs this loop again for its own target).

### 5.6 Worked examples (`node re/tools/combat-ref.mjs --examples`)

Stock data, no leader bonuses, open ground, frontal hits. "Per call" is what one member of the attacking chain
deals per blow or arrow (after `/uber_size`), as hp + sixteenths.

| Attacker → target | bal | attack×bal/100 | − armor | get_damage | per call | target member threshold | calls to kill one member |
|---|---|---|---|---|---|---|---|
| Hoplites → Light Cavalry | 166 | 130·166/100 = 215 | (215+5)/10 − 2 | 20 | 6 + 10/16 | 91 (uber 1) | 14 |
| Light Cavalry → Hoplites | 120 | 156 | 16 − 4 | 12 | 12 | 120/3 = 40 | 4 |
| Bowmen → Hoplites | 253 | 303 | 30 − 4 | 26 | 8 + 10/16 | 40 | 5 |
| Hoplites → Bowmen | 86 | 111 | 11 − 0 | 11 | 3 + 10/16 | 70/3 = 23 (last 24) | 7 |
| Light Cavalry → Bowmen | 136 | 176 | 18 − 0 | 18 | 18 | 23 | 2 |
| Slingers → Light Cavalry | 100 | 100 | 10 − 2 | 8 | 2 + 10/16 | 91 | 35 |
| Catapult → Barracks | 430 | 731 | 73 − 3 | 70 | 70 | 1200 | 18 |

E.g. Hoplites vs Light Cavalry: three hoplite figures each hit for 6 10/16 per 32-frame recharge (≈20 hp per
volley), so a fresh Light Cavalry (91 hp) dies on the 14th hoplite blow; one Light Cavalry blow takes 12 hp off a
40-hp hoplite figure, killing it in 4 blows (3 × 4 = 12 blows for the chain). A side hit
(`--flank 0,0x40000000`) by Light Cavalry on Bowmen becomes (176·200/100 + 5)/10 = 35 (doubled), but the hoplite
blow on (mounted) Light Cavalry only (215·114/100 + 5)/10 − 2 = 23 (flank bonus 7 %, §5.2.1).

## 6. Death, corpses, kill credit

### 6.1 `Object::die(death, gpiece, deg)` @647080 and `close`

`Unit::die` @60eda0 and `Wall::die` @63f6c0 just call `Object::die`, which calls the virtual `close(death, gpiece,
deg)` (+0x150) and then holds the slot: `hold_frames = max(1, hold_frames, max over this shooter's live Ammo of
(G + 1 + total_time − cur_time))` where G is the global int at @c0a888 (labelled `nuke_effect+0x108` by Ghidra;
❓ value). The shooter scan only runs when `type.max_range != 0`. Other killers: `Unit::die_uber` @606460 (kills
the whole chain: the captain first walks up, then every live `o_down` member, then itself, all with the same
death type), `Object::disband` @6455c0 (units: `die(rnd(3) + 1)` — one draw @6459c3; buildings: `close(0)` or `die(1)` without
a draw, production-spec §14), scripts, transports sinking (`eject_contents`), fuel, collisions.

### 6.2 `Unit::close(death, gpiece, deg)` @60ee50 (death animation part) ✅

The bookkeeping is in frame-spec §8.2. The combat-relevant part, only when `death != 0`:
- skipped (no draws) when the unit is an **unpacked** merchant/fur trader (61, 62, 400) — those leave a
  `World::set_blocked_at` 2×2 footprint and set `local_1c`, so no death animation is chosen; also skipped for
  decoys and units not on the map;
- `anim = death·2 + 13 + r1` with `r1 = game_random.get(0,0xffff) & 1` (signed `% 2`, never negative here)
  (**RNG @60fae6**) → CHAR_DEATH_STAB1/2 (15/16), SHOT1/2 (17/18), SPLODED1/2 (19/20);
- `death == 4` (SCORCH, flame ammo): `anim = 17 + r2` (**RNG @60fb24**, `& 1`) and `alt = 14 − r3` with
  `r3 = get & 3` (**RNG @60fb42**); guys whose graphics piece has a burn skeleton
  (`data_pieces[guy.gpiece] + 0x94 → +0x24 >= 0`) use `alt` (11..14) instead of `anim`;
- the values pass through `float` and back (`(int)(float)n`, exact);
- for every guy in `[0, guy_mark)` then `[type.squad_size, guys.len)`: `Objects::kill_guy(who, o, g, anim,
  gpiece, deg, 0)`.

So a death costs exactly **one** draw (three for flame deaths), independent of the number of guys, made inside
`take_damage`'s `die` call, i.e. at the moment of the killing hit.

`Objects::kill_guy` @659410: aircraft guys (unit type `cat` field == 8 Air) that are not missiles get a crash
**Ammo** (`Ammo::init_crash`, **RNG @67baf7**; the Ammo slot is the first free one in `objects.ammo` (+0x12c),
growing the array) instead of a corpse; otherwise if `11 <= anim <= 20` a corpse `Objects::add_death`; for
non-air units whose type does **not** block while dead (type vslot 0x120 = `UnitTypeData::blocks_while_dead`
@470440 = `unit_flags & 0x800000`, rules flag `x`; corrected from "without an attack") the guy's collision footprint
(radius `radius[type.new_block_radius]` spiral of UCoords, filtered by the guy's W-cell `WData.region`) is cleared. `param_7 == 0` queues the guy for the renderer's removal list (UI).

### 6.3 Corpses (`DeathObj`, 0xa4 bytes; walked in saves) 🟡

`Objects::add_death` @653b60: the pool is **100 slots** allocated by `Objects::init` @65ea80 (never grown). It takes
the first invalid DeathObj (+0x14c, count +0x140), or else the valid one with the smallest `first_frame` (strict,
starting from `frame + 1`) among those whose unit type does not block while dead — slot 0 when every corpse blocks.
A reused blocking corpse calls `clear_blocking` (`remove_from_world` of the dead unit plus the footprint around the
corpse, filtered by its tregion). `DeathObj::init` @8d4c60 stores who/o, the guy position/angle, the anim, the guy
graphics piece (`get_gpiece(guy, packed)`), the ammo piece if it is a "puncture" ammo, `deg`, `first_frame =
frame`, `cur_frame = 0`, the crew index (`guy − squad_size`, 0xff for squad guys), z = 0 for ships else
`find_data_z`, and `skeleton = UnitRDataStruct.skeleton` (+0x94 → +0x20) of the piece. A piece whose model is not
loaded, or without an AnimationPacket, gives an invalid corpse at once (a blocking one clears its block).

`DeathObj::inc_time` @8d5240 (from `Objects::inc_time`, after the Ammo pass): `objects[who][o].hold_frames++`
every frame (so the dead unit's slot stays held for the corpse's whole life — `Objects::process_all` decrements it
once per frame, net 0); `cur_frame++`; the corpse expires when `cur_frame >= L` with `L = anim_frames` for ships,
`anim_frames + 135` for other units, `anim_frames + 627` when a skeleton piece is set (the piece's `skeleton`
attribute names a `<SKELETON>` of unit_graphics.xml: most shipped units have one — Biped, Knight, Machinegun, Siege,
… — so most corpses last about 45 seconds); `anim_frames =
AnimationPacket::get_game_frames(anim)` of the guy's graphics piece (**animation data dependency**). Expired
(or piece-less) corpses of dead-blocking types clear their block; live blocking corpses keep `hold_frames >= 30`.

**Port consequence:** slot reuse order (and therefore every later `o`) depends on corpse lifetimes, which depend on
the death animation lengths from the .bha files (same dependency as the guy animation clock, movement-spec §9.3).

### 6.4 Score and kill value

`UnitTypeData::get_kill_value(who)` @61d3d0 = `get_score_value(0) + (support_cost[0] + support_cost[1]) ×
(num_units[type−50] + num_queued[type])` of leader `who` (the victim's owner; `who < 0` → score value only).
`TypeData::get_kill_value` @663a00 (buildings) = `get_score_value(0)`. `TypeData::get_score_value(0)` @6673b0 =
`sum(costs[0..5]) × C.unit_cost_factor` for units (MILITIA/MINUTEMAN/PARTISAN priced as type 50),
`× C.build_cost_factor` for buildings. Credit: `score_combat += kv/5` (or `−= kv/5`, floored at 0, when the victim
is an ally).

## 7. Healing, repair, regeneration, attrition

Ported in `engine/sim/attrition.cpp` (2026-10-23). Everything below was re-read from the decompiles, with the short
supply functions and the float code checked in the disassembly (the decompiler dropped their return values).
Not yet compared with a recorded game.

### 7.1 Per-frame counters and the attrition tick (`Unit::process` @610bc0) ✅

At the top of `Unit::process`: `recharging` (+0xae), `full` (+0xac), `waiting` (+0xad) and `healing` (+0x38) are
each decremented if non-zero; then `process_healing` runs (on or off the map). For units on the map:
`unit_masks2 &= ~HOLD_ANIM`; every 16 frames (`(frame + o) % 16 == 0`) `targeted` (+0x3d) = `targeted / 4` and
`process_cloak`, and inside that, every 32 frames: clear `unit_masks2` ATTRITION_SAFE (0x40000), `process_attrition`,
then the detected-mark toggle (`m2 & 2 ? m2 &= ~2 : m2 &= ~1`). After the 16-frame block:

```
if attrition (+0x9e) != 0 and (frame + o) % attrition == 0:
    if process_supply(): unit_masks2 |= ATTRITION_SAFE
    else if suffer_attrition(show = 1) != 0: return            // the unit died
```

Decoys visible to an enemy (`fog cell & ~ally_mask`) also call `suffer_attrition(frame % 7 == 0)` earlier in the
function (decoys are not simulated).

### 7.2 `Unit::process_healing` @5e0670 ✅ (garrisons and generals pending)

Only captains (`o_up < 0`) that are not aircraft and have damage somewhere in the chain (`has_damage(1)` @609b50: a
dead member, `damage` or `damage_frac` from this unit down `o_down`). "Heal" below is `repair_damage(1, show, 1)`
(vslot 0x168) followed by `healing = max(healing, rate)`; the branches marked **A** also drop UNIT_AZTEC_DAMAGE
(0x4000) when the captain has no damage left. `tick(r)` is `r != 0 && (frame + o) % r == 0`; `rate0` is
`unit_heal_rate[0]`; "allied cell" is a W-cell owner `>= 0` that `is_ally`.

**Garrisoned** (`inside_up >= 0`): the building it is in must be valid; `rate = unit_heal_rate[get_heal_level()]`,
`rate·100/(red_fort_heal + 100)` in a Red Fort (type 0x216) or with `has_wonder(0x216) == 2`, `(2·rate + 2)/3` for a
ship inside a DOCK-class building (`is(0x1b0)`), at least 1. On `tick(rate)`, if the building produces this unit
(`is(type.where)`), is a city or fort (building `flags & 0x20` or type vslot 0xfc) or `is(0x1b7)`: heal, drop
UNIT_AZTEC_DAMAGE, `healing = max(healing, rate)`. *Not ported: garrisons are not simulated.*

**Ships** (domain 1): Citrus (rare good 26, own or conquest) heals every `rate0` unless the type has
`unit_flags & 0x10`; then, only for an aircraft carrier itself (`is(0x15f, strict)`), heal every `rate0`. Return.

**Land units**: supply units never heal. `rate = supply_heal_rate`. Then, in order:
1. Antipater-type generals (leader owns type 0x169 or 0x170): every `antipater_heal_rate`, if `has_general(0, 0x169)`
   or `(0, 0x170)`: heal **A**. `rate` is reset to `supply_heal_rate`.
2. CtW hero type (`conquest_game.hero_type`, CtW only): every `rate0`, heal.
3. Iroquois (tribe bonus 18): not BLOW_COVER (0x1000), not moving (vslot 0xd8 `is_moving`), ground; every
   `iroquois_heal_rate[get_heal_level()]` on an allied cell: heal **A**.
4. Every `french_supply_heal_rate` (`fr`): general 0x161 on an allied cell; general 0x163 on a neutral or allied cell;
   general 0x165 unless the unit is suffering unsupplied attrition (`ATTRITION && !ATTRITION_SAFE && !is_supply`):
   each heals **A** at `fr`.
5. Supply: French (tribe bonus 10) `rate += fr`; Versailles (0x218) with `versailles_supply_heal_rate = v != 0`:
   `rate = rate == 0 ? v : (v + rate)/4`. On `tick(rate)` with `Supplies::find_supply` in range: heal **A**.
6. Civilians: on `tick(civilian_heal_rate)`, not UNDER_ATTACK (`unit_masks2 & 1`), workers, caravans, merchants or
   FISHERMEN (0x13d), at sea or on an allied cell: `repair_damage(1, 0, 1)` and `healing = max(healing, rate)`.

With the shipped rules `supply_heal_rate` is 0, so supply wagons heal only French units (20 frames) or with Versailles.
*Not ported: the general branches (`has_general` returns -1 until heroes exist) and the CtW hero.*

### 7.3 `Unit::repair_damage(hp, show, conquest)` @60de10 — healing and member regeneration ✅

```
if conquest and CtW and the leader's heal cards (LeaderData +0x68ff) != 0: hp = ((cards·100 + ctw_heal_bonus)/100)·hp
left = hp − damage                                       // computed before the clamp
(heal underlay on the map for the console's side or allies: internal_random, UI only)
if damage != 0: damage = min(damage, hits()); damage = damage > hp ? damage − hp : 0; damage_frac = 0
if left > 0:
    if o_down member alive: o_down.repair_damage(left, show, 0); return           // cascade down the chain
    if curr_uber_size(from the captain) < type.uber_size and (garrisoned or !(unit_masks & IN_COMBAT)):
        the dead member is the chain tail o_down (an error report if o_down < 0)
        s = find_free(who, 0, 2000, forced = o_down); Unit::init(who, type, s, x, y) at this unit's position
            (vslot 0x8c: one Guy::init_real game_random draw per guy, and the counted-unit bookkeeping of
            set_type(INIT), which is **not** undone here, unlike Objects::init_unit for a new follower ❓)
        restore the slot's o_up / o_down; damage = hits()/uber_size (the new member starts nearly dead);
        stance and unit_masks copied from this unit
        outside: UnitType::find_nearby_spot(r0 0, r1 −1, step 0, angle 0x55555555, FILTER_NOT_ME, the new
                 member) + add_move_order(kind 1, QUEUE_NEW, orig −1, −1); inside: Object::insert_inside
        the console's selection, this unit's Group and the hotkey groups that hold this unit: remove it and add
        it again (Group vslots 0x10 then 0x0c), so the group picks up the whole chain
        new.repair_damage(left, show, 0)
```

`Object::repair_damage` @646f90 (buildings, base): clamp damage to hits, `damage_frac = 0`, subtract.
*Ported, except insert_inside (garrisons) and the selection / hotkey groups (console UI state).*

### 7.4 Attrition, unit side ✅

`Unit::process_attrition` @5e11a0 (every 32 frames, on the map). `me` = the unit's leader, `owner` = the W-cell
owner under it.

```
unit_masks &= ~(ATTRITION | SEVERE_ATTRITION); attrition = 0
inside one of me's scenario attrition-free points (tile distance vector_dist(dy, dx) <= radius): return
if owner < 0:
    if neutral_attrition[me] == 0: return
    if not a ship: attrition = neutral_attrition[me]          // the period
    (the original goes on reading leaders[-1], memory before the array; taken as "not a live leader": return)
if owner == me, owner not VALID|ACTIVE, both sides allied (diplos 2 both ways),
   take_att_disabled[me] or give_att_disabled[owner]: return
exemptions: gathering workers whose gather action has byte +0x25 set ❓(not traced);
   for units that are not workers, merchants, heroes or supply: unarmed (attack 0), special, spy (is 0x3a),
   caravan; ships; neutral_attrition[me] != 0; CtW conquest bonus 9 (CtW, team style != 2)
if not enemies, or CtW with war not yet allowed:                       // peace
    broke = an alliance broken (broke_alliance +0x2b0, either side) less than ally_to_war_delay frames ago
    grace = broke and that was less than ally_to_war_grace frames ago,
            or (not grace) a peace made (made_peace +0x2d0, either side) less than ally_to_war_delay ago
    every 900 frames at most (leader stamp +0x1f4): the border messages and S_ATTRITION_WARNING (UI); the stamp is
        set to `max(1, ally_to_war_grace + frame − 900)` during a broken-alliance grace, else to `frame`
    Leader::meet(owner) if not met (treaty bit 1); unit_masks |= ATTRITION | SEVERE_ATTRITION
    if grace: return
    attrition = peace_attrition (halved for aircraft)
else if team style 2 and owner is not my leader's target:               // assassin
    the same warning, meet and masks; attrition = assassin_attrition (halved for aircraft)
ground units only (domain 0):
    if !SEVERE_ATTRITION and the unit is a supply unit: return
    a = get_attrition(owner); if a != 0:
        a = max(1, (attrition_constant · a) / 256 (signed))            // constants.attrition
        attrition = attrition == 0 ? a : min(attrition, a); unit_masks |= ATTRITION; meet(owner) if not met
```

So at war a unit suffers only in the territory of a leader with attrition techs (`att != 0`, §7.6); at peace any
foreign, non-allied territory gives the fast border attrition (`peace_attrition` = 8 frames) regardless of techs,
and SEVERE_ATTRITION means no supply can stop it.

`UnitData::get_attrition(owner)` @608fd0 (the period from the owner's `att` and my `anti_att`):

```
if att[owner] == 0 or constants.attrition == 0: return 0
base = 0x100; siege (type vslot 0x10c): siege_attrition > 99 → 0, else base = 0x6400 / (100 − siege_attrition)
if not militia (is 0x42):
    v = (int)((float)base × anti_att[me] × 0.00390625f)       // SSE single: MULSS, MULSS, CVTTSS2SI
    if me has FORAGING_1 and the unit is idle (no current order): return 0
else: v = base·100 / (militia_attrition + 100)
merchants (0x3d, 0x3e, 400): v /= 2; aircraft: v /= 2
gap = ages[owner] − ages[me]; if gap >= 0: att = ((attrition_aged_up·gap + 100)·att + 99)/100
return v / att
```

`Unit::process_supply` @5e0560 (supplied this tick): never with SEVERE_ATTRITION, never for supply units or militia;
true when `Supplies::find_supply` finds one in range, or the leader owns a supplying general (types 0x16b, 0x176,
0x16e, `num_units` > 0) and `has_general` finds it.

`Unit::suffer_attrition(show)` @5e1a10: the first time a leader's unit suffers, stamp +0x1f8 (and, for the console's
side outside allied land, the "attrition" / "build supply" messages and sound: UI). Then `take_damage(hp, frac,
death 1, ammo −1, quiet 1, angle 0x55555555, −1, −1, 0)` with `(hp, frac) = (1, 0)` when `type.uber_size == 1`, else
`(0, 6)` for a 3-member chain or `(0, 16/n)` sixteenths for n members (from the captain); `show` plays
`UnitOut::suffer_graphic_attrition`. Returns take_damage's result (non-zero: died).

Buildings: `Wall::process` @640450 applies its own territory attrition (`construct.cpp`, not in this section).

### 7.5 Supplies (`supplies.obj`) ✅

One `Supply` record (0x10 bytes: slot, unit slot, flags bit 0 live, who) per supply unit (`is_supply`:
`unit_flags2 & 0x40`), per leader, with `supply_mark` (LeaderData +0x434) one past the highest live record.
`Unit::init` registers (`init_supply`: the first dead record below the mark, else a new one), `Unit::close` releases
(`close_supply`: clear it, then lower the mark past dead records), and `Unit::set_type` (normal mode, games newer than
the expansion's release build) releases or registers when the type changes supply-ness. The record index is kept in
the unit (+0x84). `find_supply(x, y, who)` returns the first live record whose unit is alive and on the map within
`get_radius() × 192` (`vector_dist`), else −1; `SupplyData::get_radius` = `supply_radius + supply_radius_upgrade ×
get_supply_upgrade() (+ terra_cotta_range with wonder 0x211)` tiles. `UnitData::in_supply(&s)` @609ef0 (siege reload,
the HUD): true for non-ground units and on the unit's own cell, else `find_supply`.

### 7.6 Attrition, leader side ✅

Both run in `Leader::init` and `Leader::calc_unit_stats` (LEADER_CALC_UNIT_STATS; `gain_tech` sets it).
`Leader::calc_attrition` @6cdea0 → `att` (+0x7f0): 0 with `give_att_disabled` (+0x7f8); else n = the number of
consecutive ATTRITION1..4 techs (TypeIndex 0x2dd..0x2e0) held, `att = n ? attrition_upgrade[n + 3] : 0`, then the
Colosseum (0x212), Russians (tribe bonus 13), the CtW attrition card (+0x6900) and the Kremlin (0x21a) each apply
`att = (k + 100)·att/100` with a floor of 1. `Leader::calc_anti_attrition` @6cdcc0 → `anti_att` (float, +0x7f4):
256.0f, or 0 with `take_att_disabled` (+0x7fc); then the Foraging level (FORAGING_3/2/1 = 0x300/0x2ff/0x2fe →
`attrition_upgrade[2/1/0]`), the Statue of Liberty (0x219), Mongols (tribe bonus 17) and Titanium (rare good 36),
in that order: `u > 99` → 0 and stop, else `anti = (anti × 100.0f) / (float)(100 − u)` (SSE single).
`get_heal_level` @6e12e0 counts HEAL_FASTER_1..3 (0x2ef..0x2f1) and `get_supply_upgrade` @6e0880 counts
SUPPLY_WAGONS_1..3 (0x2fb..0x2fd), each with `has_preq`.

Scenario switches (`ScenarioFuncSet`, all return −1 except `set_attrition_free_point`): enable / disable
`take_attrition` (+0x7fc) and `give_attrition` (+0x7f8) set the flag only (the values change at the next
calc_unit_stats); `set_attrition_free_point(who, tx, ty, r)` (1 on success, −1 for a bad leader, tile or r < 1);
`clear_attrition_free_points(who)`; `set_neutral_attrition` (+0x800, 0..1000).

## 8. Every `game_random` draw in combat, in call order

All draws are `game_random.get(0, 0xFFFF)` then the operation shown. Sites are the instruction addresses of the
`MOV ECX,[GameAccess::game_random]` before the call (`re/analysis/xrefs_random.txt`). The per-section tables
(§2.18, §3.11, §4.15) give the full conditions; this is the merged contract in frame order.

**Unit pass (`Objects::process_all`), per unit in processing order:**

| # | Site | Function | Draw → use | Condition / count |
|---|---|---|---|---|
| 1 | @602107 | `Unit::find_attack_pos` @601280 | `% 192` jitter added to each candidate's distance | target is a building/wall and the unit is out of range; one per *valid* perimeter candidate (≤ 100 evaluated, stops 3 or 14 after the first valid). Reached from `Group::action_attack` (ATTACK command, at command time), `check_target_path`, `do_attack` (transports), `fight` (out of range), `target_opportunity` (ship out-ranging), `find_nearby_target` (GROUP_ATTACK_TO) |
| 2 | @5fdcdb | `Unit::fight` | odd → an AI guard drops the attack | activity GUARD, captain, AI-controlled, not `p5` |
| 3 | @5fde6e | `Unit::fight` | `% 5 == 0` → keep the current non-combat target | recharged captain re-evaluating a unit target |
| 4 | @5fee89 (→ `rnd` @43ccae) | `Unit::fight` | `% 100 < jam_unit_radar_prob` → jammed AA plays DEFAULT | in range, shooter has OBJMASK_ANTI_AIR, not rocking/M&R/Immortals/389 |
| 5 | @5db21d | `Guy::set_anim` | `% 100`: < 30 ATTACK1, > 70 ATTACK3, else ATTACK2 | once per guy that starts an attack animation (guys in index order; also on hold release and queued attacks) |
| 6 | @64cc73, @64cca4 | `Object::fire_ammo` | `% W`, `% H` arrow start point in the footprint | building shooters, per arrow (`ammo_per_att`), x then y |
| 7 | @67c00c / @67c079, @67c0ba \| @67c126 | `Ammo::init` | `% 100` air hit rolls against `fly_low`/`fly_high` | air targets only (one of: shooter roll; or target roll then low/high roll) |
| 8 | @67c6c6, @67c703 or @67c8b7, @67c8e9 | `Ammo::init` | `% r` scatter x then y | ground-order or target shot with scatter radius `r ≥ 2` |
| 9 | @6520ef | `Object::take_damage` | `% 100 < 5` | first damage on an undamaged building (`damage == 0`), non-quiet hits |
| 10 | @652199 | `Object::take_damage` | `% 2` (signed) → flock of 3–4 | after 9 succeeds, target is a fort/temple/town and the attacker's type is siege; then `Objects::add_flock` draws more (its own sites @65c1d6 …) |
| 11 | @6459c3 | `Object::disband` | `% 3 + 1` → death type | **units only** (disbanding a building draws nothing, production-spec §14): the DISBAND command; city capture disbands only buildings (capture-spec §9) |
| 12 | @605fca / @606159 | `Unit::plunder` | `% 25` per available non-knowledge resource (pick the scarcest) | kill credit plunder (Aztec/Despot/Inca branches; two code paths by patch version) |
| 13 | @60fae6 | `Unit::close` | `& 1` → death anim variant | every unit death with a death type, except unpacked merchants/fur traders, decoys and units off the map |
| 14 | @60fb24, @60fb42 | `Unit::close` | `& 1`, `& 3` → flame death variants | death type 4 (SCORCH) only, after 13 |
| 15 | @67baf7 | `Ammo::init_crash` (from `Objects::kill_guy`) | `% 7 − 3` → roll | each guy of a dying aircraft (not missiles) |
| 16 | @5db6e9 | `Guy::init_real` (via `Unit::init`) | `% 100` idle variant | each guy of an uber member regenerated by `repair_damage` (§7.3) |

**Building pass:** `Build::do_attack` → #6 then #7/#8 per arrow; kills inside it as #9–#15.

**`Objects::inc_time`:** release events (`Guy::execute_events` → `add_ammo` → #7/#8) in unit order; then every live
Ammo in **slot order**:

| # | Site | Function | Draw → use | Condition |
|---|---|---|---|---|
| 17 | @678ca7, @678cc4 | `Ammo::do_damage` | `% 41 − 20` miss jitter x, y (stuck arrow position) | no splash and no valid target after the hit tests |
| — | #9–#16 | via `Object::do_damage` → `take_damage` → `die` | as above | every projectile hit / kill |

Not combat draws but adjacent: `do_move` @5f899d (approach moves), `astar_path` @6848b5/@684df5 (approach paths),
`think_scout` / `think_fish` / `think_spellcaster` (other `think` branches), `Unit::come_out` @61a1aa (city capture
ejecting garrisons). `Object::attempt_launch` (fighters scrambling) only queues an AIR_PATROL order; the carrier
launch itself (`Object::do_launch` @650071) runs later from `work`. `Ammo::init_crash` also uses a local
`Random(sx + sy + sz)` for the impact point (not the game stream). ❓ Whether `Leader::action_declare` (AI war
declaration from `take_damage`) and `Group::action_alarm` (AI peasant alarm) draw: not traced.

## 9. Data the combat code reads

**Rules (all loaded by `engine/sim/rules*.hpp`; nothing type- or constant-related is missing):** type fields
`obj_masks`, `attack` (×10), `to_hit`, `attenuate`, `recharge`, `min_range`/`max_range` (tiles), `second_max_range`,
`splash_area`/`splash_percent`, `ammo_per_att`, `proj_speed`, `hits`, `armor`, `domain`, `x_size`/`y_size`,
`guy_radius`/`block_radius`/`big_radius`/`new_block_radius`, `fly_high`/`fly_low`, `costs[6]`, `support_cost[2]`,
`where`, `unit_flags`/`unit_flags2`, `role`, `target_size`, `squad_size`, `uber_size`, `is_list`, `build_flags`,
`most_shots`, `base_arrows`; the balance table (rules-spec §11); constants listed in §2.12, §3.12, §4.16 and §5.2
(flank_bonus 50, cavalry_flank_bonus 40 and vehicle_flank_bonus 33 used as /256 fractions, overkill_frames 30, overkill_damage 85/256,
river_modifier 512/256, rocky_modifier 170/256, entrenchment_modifier 170/256, height_bonus 10, height_increment
200, recapture_city_modifier 512/256, unit_respond_range 12, unit_defensive_respond_range 4, unit_guard_respond_range
8, unit/build_cost_factor, target_radius, jam_unit_radar_prob, unit_heal_rate[], supply_heal_rate, … and the nation /
hero / wonder constants of §5.1). `combat-ref.mjs` reads them through `rulesdump.mjs`.

**One rules field is rewritten from the art:** `UnitTypeData.fire_proj` (+0x2cc) is 1 after the rules load, then
`GraphicEvents::init_unit_events` sets it to 0 for types whose `unit_graphics.xml` entry has RELEASEEVENTs (they fire
from animation events) or to an ammo piece (§4.2). The port must compute it at load from the graphics catalog.

**Art data the simulation depends on (§4.17):** RELEASEEVENT frames per GRAPH × civ × age (`starttime / 67 ms`),
INSTANTHIT/PLANERELEASE entries, the AMMO piece order and flags in `effects_graphics.xml` (`first_ammo_piece + k`,
FLAME → death type 4, PUNCTURE), animation frame counts `AnimationPacket::get_game_frames` (3 when missing; guy
`end_time`, corpse lifetime §6.3, turret clocks), which attack variants exist, muzzle offsets (animated node
positions, floats), pivot restrictions. This is the same `.bha`/catalog dependency as movement-spec §9.3; the agent
decoding animation lengths should also export release-event frames and death-animation lengths.

**Non-rules simulation state the port must add** (all walked in saves): per object `targeted` (+0x3d), `near_o/
near_who`, `visible` (+0x40), `damage`, `damage_frac` (+0x3b), `healing`, `hold_frames`; per unit `recharging`,
`damage_frame/damage_o/damage_who`, `cavarch_o/uid/who`, `myarmor`, `myhits`, `attrition`, `stance`, `idle`,
`full`, `o_up/o_down` chains, guy `hold_attack/queued_attack`; per building `attack_ox/attack_whom`, `build_masks`
(4, 0x30, 0x2000, 0x8000), `recharging` (+0x7a), `ever_seen`, `infiltrate`; the **Ammo pool** (`objects.ammo`,
slot reuse = lowest free, `graph_index` counter +0x1f8) and the **DeathObj pool** (+0x14c/+0x140); per leader
`diplos`/`treaties`, `ally_mask`, `tech` bitmask, `rare`/`rare_conquest`, `num_units`/`num_queued`, the encrypted
ages/epochs/resources, the score and statistics fields written by `do_damage`/`take_damage` (`score_combat`,
`units_killed/lost`, `buildings_razed/lost`, `*_current_frame`, `frame_attacked`, attack/raid/capital/ally stamps),
`strategy[]`, cities; world `WData.owner` (+0xf), rocky flag (8), fog `seen`/`seen3`, `danger[]`; groups and
armies (target fields); heroes (`HeroesData::find_hero`) and supplies (`Supplies::find_supply`).

## 10. Port plan (ordered)

Each step: C++ in `engine/sim/`, hooked into `ron-sim`, deterministic self-check, then compared with the JS
references where one exists. Keep the original's evaluation order; never let the renderer draw RNG.

1. **Damage core** — new `combat_damage.{hpp,cpp}`: the stat getters with leader-dependent terms behind one
   `LeaderBonuses` hook returning 0 at first (§5.1); `get_damage` (§5.2, all situational branches; the ones needing
   absent systems — cities, heroes, rares, territory — read stubs that return "not applicable"); `do_damage` scaling
   and the leader statistics writes (§5.3); `take_damage` accumulation, uber thresholds, death dispatch (§5.4).
   Verify: a `ron-combat-check` CLI dumping `get_damage` + per-call (hp, frac) for every unit pair (352 × 493) in the
   plain situation and a few flagged situations, diffed against `node re/tools/combat-ref.mjs --dump out.tsv` (169,312 rows: attacker, target, balance,
   get_damage, death type, per-call hp and sixteenths).
2. **Death** — `Unit::close` death-animation draws (§6.2) in the existing `unit_die`; `Objects::kill_guy`,
   `add_death`, a `DeathObj` pool with `inc_time` holding `hold_frames` (§6.3; animation lengths from the
   `AnimSource` once the .bha lengths land, 3 frames until then — matching the original's missing-animation value);
   `Object::die`'s ammo hold; kill value and `score_combat` (§6.4). Stub plunder (but keep its draws once leaders
   have resources: it is RNG-relevant), flocks (`add_flock` draws — port with the gaia bird code), disband.
3. **Range and validity** — `attack_dist`, the four `is_in_range` overloads (§2.11/§3.6), `valid_target_const`
   (needs `is_enemy`, fog `is_seen`: keep the current "always seen" placeholder behind the same API),
   `valid_target` with `check_capture` (ported with capture-spec.md).
4. **Acquisition** — `compare_target`, `check_target`, `poor_target`, `find_nearby_target` (circle spiral, newest
   first, early stop, strict `>`), `find_melee_target`, `think_attack`, the combat part of `think` (replace the stub in
   `orders.cpp`), `target_opportunity` (called from `do_damage`), `find_new_target`, `Group::target_opportunity`.
   Fix `compass.cpp`'s circle table cap (12873 points, §2.17.1) at the same time. Stub `add_to_army`, AI retreat,
   spies/commandos.
5. **Attack orders** — `add_attack_order` (+ `find_def_pos`), `do_attack`, `check_target_path` (replace the TODO in
   `work`), `find_attack_pos` with its draw (needs `UnitType::find_nearby_spot`, already on the movement resume list),
   `find_melee_pos`/`find_open_slots`, `do_attack_to` scan, `Group::action_attack` + `distribute_attack` +
   `do_group_attack(_to)`, the ATTACK command (`CommandPackage::process_attack`) in `commands.cpp`, and the
   attack-move re-add TODO in `orders.cpp`.
6. **Firing** — `fight` (§4.3; melee first: immediate `do_damage`), `set_attack`/`set_attacking`/`set_in_danger`,
   the attack path of `Guy::set_anim` (variant draw) and hold/queue release in `Guy::move`, `recharge`;
   then `fire_ammo`, `add_ammo`, `Ammo::init/inc_time/check_hit/hit_target/do_damage` with the float trajectory
   (single precision, operation order as §4.8–4.9), release events in `execute_events` (needs `fire_proj` from the
   art and the event frames), `cavarch_fight`; then `Build::do_attack` + garrison arrows. Stub at first: splines
   (rockets, torpedoes, missiles: ❓), muzzle offsets (use 0 — a known deviation until the art pipeline provides node
   positions), broadside (unused by the data), aircraft (strafe, bombers, carriers), nukes, air-defence turrets.
7. **Healing, regeneration, attrition** — `process_healing` (garrison healing first), `repair_damage` with member
   regeneration (it calls `Unit::init`: RNG), `process_attrition`/`suffer_attrition` (needs territory and diplomacy;
   check `get_attrition`'s float op in the asm before porting).
8. **Leader-dependent terms** — fill `LeaderBonuses` (nation powers, generals via `find_hero`, rares, wonders) when
   leaders, heroes and the economy exist; each term is listed with its constant in §5.1/§5.2.

### 10.1 Port status (engine/sim, 2026-10-12)

Steps 1–6 are ported (combat_damage.cpp, combat_death.cpp, combat_target.cpp, combat_orders.cpp, combat_fight.cpp;
docs/sim-implementation.md "Combat" lists what is stubbed). Step 7 is not (healing / regeneration / attrition need
garrisons, territory and leader state), step 8 is not. Findings made while porting, folded into the sections above:
`blocks_while_dead` and the corpse pool (6.2–6.3); `do_move`'s ranged stop / retarget rule (movement-spec §3.5 step 4);
`get_activity` skips attack orders too (it walks `is_move || is_attack`, `UnitOrder::is_move_attack` @47ff00), so a unit
whose only order is ATTACK has no activity and is not on duty; `Group::is_attacking_to` @70ea70 = every valid on-map
member with orders has an ATTACK_TO / GROUP_ATTACK_TO front; `find_open_slots` visits the start corner first;
`fight`'s retarget budget (+0x9f4) is zeroed for ACTIVE leaders in `Leaders::process_all` and incremented once per
retarget attempt (also when `p4` skips the search).

## 11. Open questions

The section lists (§2.20, §3.14, §4.18) hold the details; the ones that block bit-exactness:

1. **Art-derived simulation data** (§4.17): release-event frames, animation frame counts (guy clock, corpse
   lifetime, slot reuse), muzzle offsets (float node positions changing projectile flight time), the AMMO piece order
   (attachments-spec and `effects_graphics.xml` disagree on piece 0). Owner: the animation/asset agent.
2. **Spline projectiles** (rockets, torpedoes, AA and cruise/nuclear missiles): `Spline::calc_from_dir`,
   `calc_nuke_spline`, homing every 4 frames — float-heavy, not specified.
3. **Flank multiplier**: the code gives level 2 (+100 %) for side hits and level 1 (+50 %) for hits from behind
   (§5.2.1). Confirm the `angle` passed by every caller (melee: the attacker's facing; ammo: shooter → impact
   direction) and the in-game behaviour; a port must copy the code either way.
4. `UnitType::find_nearby_spot` @61de70 (used by `find_attack_pos`, flee, regeneration): ported in
   engine/sim/search.cpp (movement-spec §11.1); a written spec is still missing.
5. Globals of unknown value: the int at @c0a888 in `Object::die`'s ammo hold (labelled `nuke_effect+0x108`); the
   missile start offset (`graphic_events+0xc8..0xd0`); `game.semaphore` byte 1 bit 2 (balance-test mode?) which
   changes `think_attack`'s radius, disables "easy AI" and gates the 4× unfriendly-territory building damage.
6. `Game::get_patch_version` gates Dutch armor/hp terms and `Unit::plunder`'s path: the port needs the version a
   given replay was recorded with (replay-spec).
7. `get_attrition`'s float multiply (`× anti_att × 0.00390625`): x87 or SSE — check the asm.
8. `Leader::action_declare`, `Group::action_alarm`, `Armies::emergency`, `Army::charge`, `add_to_army` are AI side
   effects reached from combat; their RNG use is not traced.
9. ✅ `Build::check_capture` can run from `valid_target` for every capture-eligible candidate of a scan (§2.9.2);
   the W-cell iteration survives a capture mid-scan (capture-spec §2.5).
10. The splash cell loop at `splash_area ≥ 36` reads past the 441-entry compass table (§4.11); no stock unit gets there.
11. ~~`engine/sim/compass.cpp` circle cap~~: fixed (fog port).

## Evidence (sections 5–7)

| Function | Address | Used for |
|---|---|---|
| `ObjectData::get_damage`, `flanking` | 644130, 92cfe0 | §5.2 (asm `re/decomp/combat_D/asm/`) |
| `Object::do_damage`, `Object::take_damage`, `Object::attempt_launch` | 64a480, 652020, 643a10 | §5.3–5.5 |
| `UnitData::attack / armor / hits / max_range / recharge`, `ObjectData::attack / armor / hits / max_range`, `Unit::update_hits / update_armor`, `Object::update_hits`, `BuildData::get_shot`, `WallData::in_unfriendly_territory` | 6103c0 / 610160 / 610890 / 60fe90 / 60fdf0, 6469f0 / 647db0 / 6450c0 / 646c00, 60e930 / 6054c0, 647010, 62dd90, 63eca0 | §5.1–5.2 |
| `ObjectData::has_general`, `HeroesData::find_hero`, `LeaderData::has_tribe_bonus / is_ally / is_enemy / get_general_upgrade` | 646b00, 73a1b0, 6e1370 / 6edb50 / 6ebaa0 / 6e0830 | §5.1 |
| `Object::die`, `Unit::die`, `Wall::die`, `Unit::die_uber`, `Object::disband`, `Unit::close`, `Objects::kill_guy / add_death`, `DeathObj::init / inc_time / clear_blocking`, `AnimationPacket::get_game_frames` | 647080, 60eda0, 63f6c0, 606460, 6455c0, 60ee50, 659410 / 653b60, 8d4c60 / 8d5240 / 8d4ac0, 918cc0 | §6 |
| `UnitTypeData::get_kill_value`, `TypeData::get_kill_value / get_score_value`, `Unit::plunder`, `Build::plunder` | 61d3d0, 663a00 / 6673b0, 605b00, 623660 | §6.4 |
| `Unit::process`, `process_healing`, `repair_damage`, `Object::repair_damage`, `UnitData::has_damage / curr_uber_size`, `Unit::same_damage` | 610bc0, 5e0670, 60de10, 646f90, 609b50 / 60a760, 5f9400 | §7.1–7.3 |
| `Unit::process_attrition / suffer_attrition`, `UnitData::get_attrition` | 5e11a0 / 5e1a10, 608fd0 | §7.4 |
| Callers and virtual call sites | — | `re/decomp/combat_D/callers.txt`, `insn_vcalls.txt` |
| PDB layouts (ObjectData, UnitData, AmmoData, DeathObjData, LeaderData, Constants, enums) | — | `re/decomp/combat/types.txt`, `types2.txt` |
