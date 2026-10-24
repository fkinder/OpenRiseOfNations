# Aircraft: planes, helicopters, bases, carriers and missiles

How air units live in the original simulation, and what the engine still needs. Companion of `docs/garrison-spec.md`
(containers, `go_inside`, `eject_contents`), `docs/transport-spec.md` (Unit/Object vtable slots, `can_carry`),
`docs/orders-spec.md` (order classes), `docs/combat-spec.md` (valid_target, `Ammo::init` AA rolls, `Unit::fight`) and
`docs/production-spec.md` §6.7 (carrier queue). Decoded from the exe (`re/decomp/aircraft/`, disassembly in
`re/decomp/aircraft/dis/`); nothing is checked against a recording. ❓ marks every unresolved point. Distances are world
units (1 tile = 192, "XML tiles" × 192; `0x300` = one map cell). Angles are 2^32 per turn (`0x40000000` = 90 degrees).

## 0. Summary

An aircraft is a `Unit` with `type.domain == 2`. It lives in one of two states. **Based**: `inside_up >= 0`, a member of the
`inside_down` chain of an airbase / carrier / missile silo (it keeps its own order list; hit points and fuel recover). **Airborne**:
on the map, never collides, moves in a straight line toward a goal with a banking turn model, burns one fuel point (`mana_burn += 1`)
per frame and must land (`land_plane`, which re-inserts it into a base) before the fuel runs out, otherwise it crashes.
Everything the player does is a STRAFE (16), AIR_PATROL (17) or AIR_ATTACK_GROUND (24) order; a plane inside a base is *launched*
by `Object::do_launch`, one plane per 15 frames, when it has an order. Missiles are aircraft with OBJMASK `2` that die on impact.
Helicopters (`unit_flags & 0x20`) use the same orders but skip fuel and most of the flight model.

## 1. Data

Type ids used by the code (`is(t, 0)` = family test along the upgrade line): 0x11f (287) Biplane line (Biplane, Fighters, Jet
Fighters, Advanced Fighter), 0x127 (295) Jet Fighter line, 0x130 (304) Bomber line, 0x134 (308) Fighter-Bomber line (carrier planes),
0x136 (310) Helicopter line, 0x139 (313) V2 Rocket, 0x13b (315) Nuclear Missile line (315 and ICBM 316), 0x15f (351) Aircraft Carrier,
0x1bf (447) Airbase, 0x208 (520) Missile Silo, 0x192 / 0x193 (402 / 403) wild / flock birds (gaia `Animal`, domain Air, the same
flight code; vslot +0x30 `is_animal` is 1 only for them, +0x180 is `Animal::think_bird`).

| Type (unitrules) | Flags | HITS | MOVES | ATTACK | RANGE | FLY_HIGH / LOW | RECHARGE | AMMO_PER_ATT | MANA (fuel) | TURN |
|---|---|---|---|---|---|---|---|---|---|---|
| Biplane 287-288 | `w` | 118 | 55 | 39 | 2-7 | 0 / 50% | 30 | 16 | 300 | 5 |
| Fighter 289-294 | `w` | 138 | 75 | 45 | 2-7 | 0 / 25% | 30 | 16 | 400 | 8 |
| Jet Fighter 295-300 | | 140 | 90 | 46 | 2-10 | 0 / 10% | 20 | 2 | 500 | 11 |
| Advanced Fighter 301, Harrier 388 | | 155 | 115 | 50 | 2-12 | 0 / 5% | 20 | 2 | 600 | 14 |
| Bomber 304, Lancaster 307 | | 300 | 60 | 43-45 | 1-3 | 0 / 10% | 30 | 10 | 600 | 3-4 |
| Strategic 305 / Stealth 306 | | 340 / 380 | 80 / 100 | 52 / 60 | 1-3 / 1-4 | 0 / 5% , 0 | 40 | 12 / 4 | 750 / 900 | 5 / 8 |
| Fighter Bomber 308, Jet FB 309 (WHERE None) | `w` | 200 / 240 | 75 / 90 | 43 / 47 | 1-7 / 1-10 | 0 / 25%, 5% | 30 / 20 | 10 / 2 | 400 / ❓ | 6 |
| Helicopters 310-312 | `f` | 95-130 | 75 | 14-20 | 1-7 | 10% / 20% | 40 | 2 | 0 (no fuel) | 65 |
| V2 313, Cruise 314, Nuke 315, ICBM 316 | OBJMASK `2` | 2 | 115 | 150 / 300 / 1200 / 4000 | 0-40 / 99999 | 0 / 0 | 999 | 1 | 250 / ❓ / 650 / ❓ | 9 |

`w` = `unit_flags & 0x400000` ("strafes"), `f` = `0x20` (helicopter). WHERE: planes Airbase, missiles Missile Silo, carrier Dock.
Aircraft Carrier: type CARRY 12 but the real wing limit is `MAX_AIRCRAFT_PER_CARRIER` 7. All other air objmask bits: AIR `3`
(`0x10000000`), MISSILE `2` (`0x8000000`), ANTI_AIR `6` (`0x80000000`: AA Gun 281-283, carrier, Air Defense buildings).

`rules.xml` constants: `AIRCRAFT_RESPOND_RANGE` 10 tiles, `BOMBER_RESPOND_RANGE` 12, `AIR_UNIT_MANA_RECHARGE` 2 (fuel regained
per frame in base), `MAX_AIRCRAFT_PER_CARRIER` 7, `MAX_AIRCRAFT_PER_AIRBASE` 10, `AIRCRAFT_HEAL_RATE` 20 / 16 / 12 / 8 frames (by
`get_heal_level`), `BOMBING_MANA_COST` 0 (a bomber's fuel cost per drop: no effect with the shipped value), `PARADROP_RANGE` 64 tiles.
`FRAMES_BETWEEN_LAUNCHES` is not in the XML: exe data @c06248 = **15** (float-looking denormal 2.1E-44 = int 15; ❓ it is initialised
data, not computed). `UnitData::mana()` @609a50: `type.mana` (+0x2ec); for domain 2 `x (100 + space_air_range)/100` when the owner has the
SPACE PROGRAM wonder (0x21e); 0 mana = no fuel use. `mana_left = max(0, mana - mana_burn)` (`mana_burn` = Unit +0x96, i16).

## 2. Containers and capacity

Containers are airbase (447), carrier (351, a *unit*), silo (520). `build_masks` HOLDS_AIR | REPEAT_AIR (0x88) on every `can_carry(AIR)` building
(production-spec 792).

- `ObjectData::can_carry(AIR)` @646c40 (domain version): `is(447, strict) or is(351, strict) or is(520, strict)`. (For `GROUND` the decompile returns 1 only for a unit with `carry != 0`
  that is not a carrier and **0 otherwise**; `garrison-spec.md` §2 says "else the same three strict tests", which the decompile does not show.)
- `ObjectData::can_carry(TypeIndex t)` @645e00 (is *type* t allowed here), t a unit type 50..413: silo: `t.is_missile` and `!has_nuke(this)`
  (❓ `has_nuke` @643d40 counts nuke-line missiles in the silo queue / chain; not decoded); airbase (strict 447): `t.domain == 2`, t not a
  missile (objmask `2`), **not** `t.is(0x134)`; carrier: `t.is(0x134)` only; otherwise a land transport (`carry != 0`, not a carrier).
- `ObjectData::can_carry(o, who)` @6483c0, air cargo `T` (completes transport-spec §2.7): `who == this.who` and not itself, then
  `T` not a missile: if `T.is(0x136 helicopter)` the container must be `is(447)` else return 0; any other plane: container `is(520)` -> 0.
  `T` a missile: container `is(447)` or `is(351)` -> 0. Then (L484e6): `T.is(0x134)` requires the container `is(351)`; any other `T`
  requires it **not** `is(351)`. Finally `home_base(T) == this` -> 1 (already based here) else `num_aircraft_here(this, 0) < num_aircraft_limit(this)`.
  So airbases take planes and helicopters, carriers only fighter-bombers, silos only missiles (and not planes).
- `ObjectData::num_aircraft_limit()` @6454a0: strict `is(351)` -> 7, strict `is(447)` -> 10, strict `is(520)` -> 1, else 0.
- `ObjectData::num_aircraft_here(count_queue)` @645330: count of this leader's units (`units[who]` all slots) that are VALID, `domain == 2` and
  `home_base(u) == (this.o, this.who)`; **carrier** returns `carrier.num_queued (Unit +0xa0) + count`; a **building** with `count_queue != 0`
  returns `count + count_queue(UNITS) - count_queue(TYPE, 310)` (queued units not counting queued helicopters; vslot +0x3c is the BuildData,
  +0x190 `count_queue`); otherwise `count`. Missiles and birds are domain 2, so they count.
- `UnitData::home_base(&who)` @609dc0: inside a container -> `get_inside()` (its who in `*who`). On the map: the *front order*; if it is a
  SPECIAL_ANIM (25) of type 0 (landing) return its `+0x1c`; type 1 (takeoff): use the order before the tail; then, if the order is an air
  order (`is_air`), `(oxx, whose)` of its `AirOrder`; accepted only if that object `is_active` and `can_carry(TypeIndex = unit type)`; else -1.
  (`who` is not written in the landing case: ❓ caller relies on the preset value.)
- Production gates: `Build::queue_up` @620f40 refuses a non-helicopter plane when `num_aircraft_here(1) >= limit` (message "building full", sound
  S_INVALID_ORDER) and also needs `here(0) < limit` unless it is a helicopter; `Build::do_queue` @61e410 prints the "full" feedback every 220 frames
  `(o + frame) % 0xdc == 0`; `Build::finished` @628490 returns -1 (item stays queued) for an airbase non-helicopter when `here(0) >= limit`, else
  `train`. Carrier production: production-spec §6.7 (no limit check there). **New carrier wing**: `Build::train` @62f9b0, after creating a carrier,
  `Unit::action_unqueue(carrier, 1)` then `limit` (7) times `Objects::init_unit(who, current_upgrade(FIGHTERBOMBER), carrier.x, carrier.y, -1, -1, -1)`
  (**game_random per guy**) + `Unit::go_inside(carrier)` for each (the "full wing" note in `engine/sim/queues.cpp:953`).

## 3. Orders

Order classes (orders-spec §3.1): STRAFE 16 (`StrafeOrder` 0x54: TargetOrder ox 8 / whom 0xc / uid 0x10 (u16), `mandatory` 0x1c, **AirOrder at 0x24**:
`oxx` 0x28 / `whose` 0x2c (the *home base*), `cruising_alt` 0x30, `sharp_turn` 0x34 (turn direction -1/0/+1), `old` 0x38, `returning` 0x3c, then `xx` 0x40 /
`yy` 0x44 = last known target position), AIR_PATROL 17 (`PatrolOrder` waypoints `x_pos`/`y_pos`, current index +0x3c, count +8, AirOrder oxx/whose at
+0x44/+0x48, cruising_alt +0x4c, sharp_turn +0x50, returning +0x58), AIR_ATTACK_GROUND 24 (att_x +4, att_y +8, accuracy, AirOrder), GROUP_PATROL 22.
Positions stored in orders are plain world coordinates; object coordinates read from the object are `^ 0x63637` (obfuscated Coord).

- `Unit::add_strafe_order(ox, who, home_o, home_who, explicit, pos, mandatory)` @5e48c0 (the engine's `add_strafe_order(-1,-1,-1,-1,1,QUEUE_NEW,0)` calls):
  if the *unit* has OBJMASK MISSILE and `(ox, who)` is a `valid_target`, it becomes `add_air_attack_ground_order(target.x, target.y, home_o, home_who, pos,
  mandatory)` instead. Otherwise `QUEUE_NEW` first clears: `unit_masks &= ~0x4000000`, `path.length = 0`, `close_orders(0)`, `clear_partial_path`, `update_action`.
  New StrafeOrder: `ox, whom`; target invalid (`ox<0 or who<0`): `returning = 1`, `uid = 0xffff`, `xx = yy = -1`; else `uid = target.uid`, `returning = 0`,
  `xx/yy` = target position; `oxx/whose = home_o/home_who`; `cruising_alt = 0x640`. Argument mapping: `p5` -> the byte at +0x1c (AttackOrder `mandatory`; players' commands pass 1, `land_plane`
  and `Group::action_return` read it back), `p7` -> `UnitOrder.flags & 4` ("group-level" bit; `do_launch` and `land_plane` read it). `QUEUE_FIRST` also `clear_partial_path` + rewinds the cursor.
- `add_air_attack_ground_order(x, y, home_o, home_who, pos, p6)` @5e41c0: AirAttackGroundOrder `x, y`, `accuracy`/state 0, `home = (p3, p4)`, `cruising_alt = 0x640`, flag 4 iff p6.
- `add_air_patrol_order(x, y, home_o, home_who, explicit, pos)` @5e4350: **helicopters** (`unit_flags & 0x20`) get `add_move_facing_order(cell(x), cell(y), find_angle, 2, ...)`
  instead. Planes: always clears like QUEUE_NEW; creates AirPatrolOrder with exactly **1 waypoint** (x, y); if `(home_o, home_who)` is a building the waypoint is stored as
  an *offset from the base* (`x - base.x`, `y - base.y`: patrols follow the airbase); `home = (home_o, home_who)`, waypoint index 0, `returning = 0`, `cruising_alt = 0x640`,
  flag 4 iff explicit. `Group::action_air_patrol` appends waypoints (QUEUE_LAST) by writing into the order arrays.
- `add_patrol_order(a, b, c, d, ...)` @5e4560: GROUP_PATROL with 2 waypoints in cell units (`cell*0x30 + 0x18`), used for ground units (`Group::action_patrol` falls through to
  `action_air_patrol` for planes). `Unit::do_patrol` @5f1910 walks the waypoints with ATTACK_TO moves; a **carrier** doing GROUP_PATROL scrambles its wing each time
  (`Group::action_scramble` on a temp group of itself).
- `Unit::kill_current_order`, `close_orders`: orders-spec.

## 4. Flight model: `Unit::do_air_physics(order, x, y)` @5e86d0

Called by `do_strafe`, `do_air_patrol`, `do_air_attack_ground`; returns 0 when the plane landed / died / the order ended.
1. **Every 8 frames** (`(o + frame) & 7 == 0`) `air.cruising_alt` is rerolled: not a bomber-line plane, not a helicopter, and (not an animal or type 403) ->
   `(game_random(0,0xffff) % 7 + 13) * 100`; otherwise 0x640, animals +200 (402) or -200 (others). The altitude is cosmetic (Guy z) but **the draw is simulation state**.
2. `Unit +0xc0` (path length) = 0 (the path stack is reused as a 1-point scratch). Fuel: unless `is_animal`, `check_fuel` (§6); if it returns 1 the unit is dead -> return 0.
   `x, y` may be replaced by the base approach point. Type 403 (flock bird) also zeroes `+0xae` and, with `order_count == 1`, sets `returning`.
3. Clamp `x, y` into `[0, xs*0x300-1] x [0, ys*0x300-1]`; if clamped the plane may not land this frame.
4. `speed = get_speed(vslot +0x17c)(x, y, 1)` (movement-spec §9; x `ai_speed` when > 1). `pitch_aircraft` (below) may halve it.
5. **Landing test** (`air.returning != 0`): `|dx| + |dy| < speed * 3 / 2`, not clamped, and (non-helicopter, or helicopter guy z - terrain z < 0x96) -> `land_plane()`,
   return 0. A returning plane within `0xc00` of a *carrier* base sets `unit_masks |= 0x100000` on it (read by `has_repeat_air` of that unit: the carrier "expects planes").
6. Push the goal as the only path point. Non-returning, non-helicopter with an attack order whose target is a plane `valid_target`: `facing_target = angle diff < 0x40000000`
   when within `min_range * 0xc0` (feeds `bank_aircraft` as its third argument).
7. **Heading**: `want = find_angle(goal - pos)`. Non-helicopter: if `|want - angle| > 0x20000000` (45 degrees) and the goal is nearer than `(returning ? 0 : min_range*0xc0) + 0x300`
   the plane keeps its current heading (overshoots); `sharp_turn != 0` forces `want = angle + sharp_turn * 0x40000000`. `bank_aircraft` (@5e9520: float bank state in the Guy, snaps the
   angle to `want` when within `0x2222220` / `0x38e38e3`, else adds `air_turn_speed(dir, 0) * dir`) turns the angle; `Guy::set_angle`.
   `air_turn_speed` @5ea390: `t = (type.turn_speed(+0x2c4) >> 8) * unit_turn_speed`; with `p2 == 0` and bank `b` (Guy float +0x44; -5b for helicopters): opposite sign -> `0x5b05b0`,
   else `t / 0x37 * |b|`; non-helicopters at least `0x5b05b0` (~0.5 degrees/frame). ❓ the float banking constants (5.0, 55.0, 0.33, 10.0) are in the decompile, the Guy bank state
   is not modelled anywhere in the engine yet.
8. **Move** (non-helicopter always; helicopter only when `dist > 0xc0` away, or `0x30` when returning, otherwise it stays): `project(angle, speed)` -> next point.
   `UnitData::invalid_loc(tcell)` @607c30 returns 0 for **every in-bounds cell for domain 2** (nothing but the map edge limits planes; no water/cliff/forest/unit tests; it
   returns 1 only if the tcell is outside `[0, world tcells)`). If invalid: `WorldData::restrict` the point and, when `sharp_turn == 0`, draw `game_random(0,0xffff)`:
   `sharp_turn = (r odd ? +1 : -1)`; valid: `sharp_turn = 0`. Then `Unit::set_new_location(x, y, 0, 1)`: stores x, y, `z = terrain z` (not the altitude), re-adds to the world
   grid. Aircraft are never entered in `CollCheck` (movement-spec §10.1) and never block.
9. Helicopter that is already at the goal: `set_anim(WALK)` unless the recharge counter `+0xae` runs; flock bird: `sharp_turn = 0`, `+0xae = 1`, and if `|dx|+|dy| < 0x30` and `order_count > 1`
   -> `kill_current_order`.

`pitch_aircraft(dx, dy, goal_z, &speed)` @5e8de0 only changes sim state through `speed`: for a *returning* fixed-wing plane (not helicopter) closer than 0x600 to the base and not
`facing` an airborne base (`local_2c`), `speed /= 2` (approach). The rest sets the Guy pitch from `cruising_alt` (rendering).

## 5. STRAFE, AIR_PATROL, AIR_ATTACK_GROUND

**`Unit::do_strafe(order)` @5eab00** (16 per frame per plane; helicopters too). `S.returning` (+0x3c) = no target, flying home.
1. **Target check** (`returning == 0`): target `(ox, who)`; if `who >= 0`, the target is not ally of the owner (vslot +0x30 `is_animal` skips this for birds) it must be `valid_target`.
   Invalid target: missile -> `die`; helicopter -> `kill_current_order`, and if nothing is left `add_air_patrol_order(self.x, self.y, S.home...)` (i.e. hover), return;
   fixed wing: if the stored ground position `xx/yy` is a valid world point, the order is **converted to AIR_PATROL** at that point (`add_air_patrol_order(xx, yy, home)`, then
   `kill_current_order`); else `returning = 1`, `ox = who = -1`.
   Valid target: `xx/yy` follow the target; goal = target position; against another plane further than 0xc00 the goal is `project`ed (lead). A helicopter already `is_in_range`
   heads for a point at its range.
2. **Retarget** every 16 frames `((o + frame) & 0xf == 0)` while `ox >= 0`: non-bomber `find_new_air_target(pos, ox, who)`, bomber-line `find_new_bomber_target`; if the result is a
   `valid_target`, `add_strafe_order(new, home, 0, QUEUE_FIRST, 0)` replaces the order.
3. `do_air_physics(goal)`; 0 -> return.
4. **Fire gate** (`+0xae == 0`, target alive and `valid_target`): `angle diff = |angle - find_angle(target)|`; `is_in_range(target, diff, 0)`; for non-missiles the firing cone is
   **15 degrees (0xaaaaaa9)**, or **60 degrees (0x2aaaaaaa) for `is(0x127)` Jet Fighter line**; missiles have no cone. Fire: `unit_flags & 0x400000` (`w`) -> `set_anim(ATTACK2)` (the
   guy's release events launch the ammo, combat-spec §5); a non-`w`, non-bomber plane with a type ammo piece (+0x2cc) calls `Object::fire_ammo(target)` directly; bomber-line ->
   `set_anim(ATTACK2)`. A missile then `die`s (impact). Others: `+0xae = recharge(0) + 1`; bomber-line `mana_burn += BOMBING_MANA_COST` (0). A plane never stops: the next frames
   `do_air_physics` carries it past the target (rule 7: it keeps heading until farther than `min_range*0xc0 + 0x300`, then turns back for the next pass).
   RECHARGE = `UnitData::recharge` @60fdf0: the type value for non-siege units (planes: 30 / 20 / 40); the siege-only clause (x1.5 / x2 slow-fire) does not apply to planes.
5. **Wrap-up** every 32 frames `((frame + 2*o) & 0x1f == 0)` when the strafe is not the explicit order and (type has fuel `mana != 0` and `mana_left == 0`, or the target is gone, or `returning`):
   the order is killed and `set_anim(WALK)`; with the strafe being the last order, `find_new_air_target` around the *previous order's* waypoint (AIR_PATROL index) re-aims
   without killing (`aircraft_respond_range`). A plane out of fuel is handled by `check_fuel` (§6), not here.
   ❓ Several arms of this tail (the "previous order is a patrol / group move" branch and its `GROUP/AIR_PATROL` waypoint arithmetic) are in `Unit_do_strafe@5eab00.c` lines 230-330.

**`Unit::find_new_air_target(x, y, type_o, type_who, &who)`** @5ebc70 (fighters, helicopters): first `Objects::find_units(own pos, SEARCH_ENEMY, radius respond*192, FILTER_DOMAIN, 2)` (enemy
**planes**): best `compare_target` score among `valid_target` within range; if none and `dist(unit, (x, y)) <= respond*0x3c0` and `type_o >= 0`: `find_units(x, y, FILTER_TARGET, type)`;
if still none `find_units(x, y, FILTER_ALL)` within range. `respond = AIRCRAFT_RESPOND_RANGE` (10 tiles); when `unit_masks & 0x40000` (hold position) the search centre is the unit itself.
**`find_new_bomber_target`** @5eb960: only buildings (`find_builds`, SEARCH_ENEMY, radius `BOMBER_RESPOND_RANGE*0x3c0` of the point, FILTER_ALL), score `compare_target / (dist/192 + 1)`; if
`type_o >= 0` it first tries a build of the same type; returns -1 / `*who = -1` when nothing.

**`Unit::do_air_patrol(order)` @5ea620**: waypoint `i` (wrap to 0 when `i >= count`); goal = waypoint (+ base position when `oxx/whose` is a building, planes follow the base); `do_air_physics`;
within `0x240` of the goal advance `i`, and when it is the last waypoint and the unit has >1 orders the patrol ends (`kill_current_order`). Every 16 frames `((o + frame) & 0xf == 0)`
and not returning: look for a target (`find_new_air_target` / bomber; fallback to the other kind when the `game.semaphore[1] & 2` option is set): if valid and (we are on the last waypoint or the
target is a plane) -> `add_strafe_order(target, home, 0, QUEUE_FIRST, 0)` (the patrol stays below and resumes). Every 32 frames: `find_building_at(waypoint, SEARCH_ENEMY)` with an enemy
building whose `build_masks +0x62` bit for our leader is set (it is *visible/armed against us*) -> `add_strafe_order(building, home, 1, QUEUE_FIRST, 0)`. Animals: no targeting; `spell_time = 1`.
**`Unit::do_air_attack_ground(order)` @5ea420**: `do_air_physics(order.x, order.y)`; fire when `+0xae == 0`, `order+0x2c == 0`, `is_in_range(x, y)`, the same cone as §5.4;
`set_attack(-1, -1)`; ammo/animation as in §5.4 (`fire_ammo(-1, -1)` for non-bomber type-ammo planes, which spreads `AMMO_PER_ATT`; see §7); missile dies; `+0xae = recharge + 1`.
Used by AI bombers and by missiles (add_strafe_order converts a building-targeted missile).

## 6. Fuel, returning and landing

- `Unit::process` @610bc0 (domain 2, `unit_masks & 1 == 0`, not a caster): **airborne** (`inside_up < 0`): if `mana_left != 0` then `mana_burn += 1` (so `mana_left` hits 0 after `type.mana` frames);
  **based**: `mana_burn -= AIR_UNIT_MANA_RECHARGE` (clamped at 0), and every `aircraft_heal_rate[get_heal_level]` frames, `(o + frame) % rate == 0` (rate `(2*rate+2)/3` for `is(0x136)`... ❓ the
  type id is lost in the decompile), `repair_damage` vslot +0x168 and `spell_time (+0x38) = 1`. Birds: `Animal::work`.
- `Unit::check_fuel(order, &x, &y, &z)` @5e9be0: `air.returning` (AirOrder +0x18) is the "go home" flag. No fuel type and not returning -> 0. Not returning and `mana_left == 0` and not a missile -> `returning = 1`.
  Returning: if `home` is invalid or `!home.can_carry(this)` search a new base: all of the leader's active buildings (object slots 2000..) and then all units (carriers) with `can_carry(this)`, keeping
  the nearest by `vector_dist` (❓ the compare operands were swapped between the two loops, tie rule unverified). None found: helicopter -> hold position; plane -> console message
  "plane out of fuel" (loc string 0x12020) + S_PLANE_OUT_OF_FUEL (human console only) and `die(2, -1, 0)` (crash, `Ammo::init_crash`); return 1. Found: store `home`, goal = base position; for a
  *building* base the runway heading is 0 when `base.y <= ys*0x240` else 0x80000000, for a carrier its angle; a non-helicopter plane approaches along that heading from 0x300 + ... behind (`project`)
  unless `|runway - want| < 0x15555556` (30 degrees) when it flies straight at the base; the base must not touch the map edge (cell 0 / xs-1 / ys-1 -> return 0 with a base unchanged).
  `z` = base z + `dist/2`. ❓ exact approach geometry (the compare uses decompiled vector_dist calls whose arguments are lost; disassemble `check_fuel` @5e9be0 before porting).
- `Unit::land_plane()` @5e9950: animals die. Else `air.returning = 0`; if the front order is a STRAFE with a valid target the order is kept, with no target it is killed (`mandatory == 0`) or all
  orders are cleared (`mandatory != 0`). If the base `has_repeat_air` (vslot +0xf0: `build_masks & REPEAT_AIR` / unit `unit_masks & 0x200000`) the plane keeps its other orders and the order's flag 4
  is cleared, else all orders are dropped (`unit_masks &= ~0x4000000`, `close_orders`, `update_action`). Then `add_spec_anim_order(0, base.vslot_0x34, 1 (3 helicopter), base)`: a SPECIAL_ANIM
  type 0 (landing, data `+0x1c = base.o`, `+0x20 = base.who`) after which `Unit::do_garrison` / `go_inside` seats the plane (garrison-spec; the engine's TODO at `garrison.cpp:618` is
  `add_strafe_order(-1, -1, -1, -1, 1, QUEUE_NEW, 0)` for planes that came out of an airbase). Sound S_CARRIER_LANDING when seen by the console player.
- A **helicopter** ordered to GARRISON a carrier: `Unit::do_garrison` @5e6b80: `kill_current_order`, `add_strafe_order(-1, -1, carrier.o, carrier.who, 1, QUEUE_FIRST, 1)` then `strafe.returning = 1`.
- `Group::action_recall` @6fa7e0 / `action_return` @6fad40 (RECALL / RETURN commands): if the first member is air, `action_return`: helicopters (and other non-`is_plane` members) are cleared and, if on the map,
  get `add_strafe_order(-1, -1, -1, -1, 1, QUEUE_NEW, 0)` (no home: they search the nearest base); fixed-wing non-missile planes keep their current home: `add_strafe_order(-1, -1, home_o, home_who, 0,
  QUEUE_NEW, 0)` (the home is read from the air order or `get_inside`). ❓ the inside-plane arm. A non-air group first `clear_gather` on its buildings, then recalls the planes based on them.
- ❓ What a plane does when its order list becomes empty while airborne (e.g. final `kill_current_order` of `do_strafe`): no `add_strafe_order` caller exists in `Unit::work` / `do_idle`; `check_fuel`
  only runs inside the three air orders. The decompile shows no fallback; confirm with a recording before porting `do_idle` for planes.

## 7. Combat specifics

- Targeting rules (who may be hit): combat-spec §2.9.1 (air branch). **`UnitData::is_flying_low()` @60a140** (planes only: domain 2, not helicopter, not MISSILE objmask, `is_on_map`): front order
  STRAFE with a live target (`ox, who` valid + object VALID) and `|dx|,|dy|` octagon `vector_dist(self, target) < 0x900` (12 tiles) -> 1; AIR_ATTACK_GROUND and `vector_dist(self, (x, y)) < 0x900` -> 1;
  otherwise via the order's `get_attack_order` (`+0x18 != 0`, target `(+4, +8)` alive, `vector_dist < 0x900`) -> 1; NONE order -> 0. **`is_flying_high()` @60a310** = plane, on map and `!is_flying_low`.
  (Both feed `valid_target_const` and the `Ammo::init` rolls #7-#10 of combat-spec's RNG table; today both are stubbed in `combat_target.cpp:33`.)
- AA: a shooter with OBJMASK ANTI_AIR hits an air target only if `rnd%100 < target.fly_low` (target flying low) or `< target.fly_high` (high), then again `rnd%100 < shooter.fly_low|high` for the same band;
  `is_flying_low/high` choose the band. Stealth Bombers (0 / 0) are never hit by AA; draw order: combat-spec RNG table #8-#10. Helicopters and missiles skip the rolls.
- `AMMO_PER_ATT` (`type +0x208`): a *building* shooter creates that many Ammo per `fire_ammo` with scatter `% (x_size*96)` / `% (y_size*96)` (**2 draws per ammo**, only when the dimension is > 1).
  Units create one Ammo per live guy (`+0xb5`), z = guy z; planes have 1 guy. Bombers' bombs come from the `ATTACK2` animation events (`Guy::execute_events`), one release event per bomb.
- **Carrier attack** `Unit::do_attack` @5f1b80: a carrier (`is(0x15f)`) with an ATTACK order on a live target: every 16 frames (`frame & 0xf == 0`; skipped if the target is a building and the attack is not
  `mandatory`) each plane inside its chain gets `add_strafe_order(target, carrier.o, carrier.who, 1, QUEUE_NEW, mandatory)`; the carrier does not shoot itself while the planes fly.
- **Attack-triggered launch**: `Object::do_damage` @64a480: when `this` (victim) is not a missile and still VALID, the *attacker's* `attempt_launch(victim.x, victim.y)` runs. `Object::attempt_launch(x, y)`
  @643a10: needs `launch_timer (+0x41) >= 15`, `can_carry(AIR)`, a non-empty chain and a plane of the Biplane line (`count_inside(TYPE, 0x11f) != 0`); picks the **last** such plane in the chain with
  `mana_burn == 0` (a plane with orders and burnt fuel aborts the whole attempt) and gives it `add_air_patrol_order(x, y, container, 1, ...)`. (A carrier that fires at something scrambles one fighter.)
- Anti-air buildings: Air Defense Gun / Radar Air Defense (unitrules AA units are separate): upgrade by craft **0x299** ("Upgrade", GRAPH GORADAR, COST2 6t/8m, JOB_TIME 300, prereq Industrial Age):
  `SpellType::cast` @676ce0: `cast_target.set_type(current_upgrade(AIRDEFENSE 523))` (production-spec §RADARSPELL 665 also resolves to it).

## 8. Launch: `Object::do_launch()` @64f3b0 and the base commands

Called every frame from `Build::process` (airbases, silos) and `Unit::work` (carriers) when the container has a chain.
1. **Silo with a pending countdown** (`Build +0x7a != 0`): `Build::do_missile_launch` @622670: `--countdown`; at 0 take the head of the launch list (`+0x44`), and if its order is AIR_ATTACK_GROUND (or an
   STRAFE against a non-ally) announce (own: S_MISSILE_LAUNCHED; nuke-line `is(0x13b)`: S_NUKE_LAUNCHED + message / `+0x40 = 0xff` + `update_seen`; others see S_MISSILE_DETECTED / S_NUKE_DETECTED and the
   "nuke detected" notice), then `Unit::come_out(missile, 0)` and `missile.process()` once. `countdown` is set to the *missile type's recharge* when it is chosen (below).
2. `launch_timer` increments (saturating at 15). Every time it is `>= 15` walk the chain: skip units without orders (`order_count == 0`) or with `mana_burn != 0`. A unit with orders qualifies if
   `container.has_repeat_air` (vslot +0xf0) or its current order flag 4 is set; otherwise `kill_current_order` and drop it from the launch list. A qualifying STRAFE with **no target** whose home is a
   *different* container that is full (`num_aircraft_here(0) >= limit`) is turned into an AIR_PATROL over that container (it will rebase on arrival); a STRAFE whose target is invalid *and*
   whose stored position is not a valid point is killed. AIR_PATROL resets its waypoint to 0. The qualifier is added to the launch list `+0x44` (set semantics) and **at most one unit per call**
   leaves: a plane in a **building** -> `Unit::come_out(0)`; a missile -> `countdown = missile type.recharge (+0x1f4)` (it leaves in `do_missile_launch`); `launch_timer = 0`. In a **carrier**
   (`is_unit`) nothing leaves here: the plane stays at the head of the launch list `+0x44` and is released by the carrier's launch animation event (`GraphicEvents::execute_game_events` @8e48e0,
   event type 5, time window of the carrier's animation: `Unit::come_out(plane, 0)` + remove from `+0x44`). ❓ the event frame/anim comes from `unit_graphics.xml`, not traced.
3. **AI auto-launch** (only for AI leaders: `(leader_flags & 4) == 0` and `leader_flags2 & 8 == 0`): airbase / carrier every 32 frames `((frame + o) & 0x1f == 0)`, silo every 128 frames and only
   with `num_inside(1) != 0` (a nuke-line missile at the head waits while `game.armageddon >= Game::get_armageddon() - 2`, ❓ meaning of the counter). It scores enemy cities, forts and wonders
   (not Forbidden City 0x213) within `plane speed x mana` range: city `value/100 + 1000` (x2 allies, `/ (dist/0x300 + 1)`), forts `.../100 + 1000`, wonders `/1000 + 1000` (x10 if enemies lead, x100 if
   the leader can win by wonders), skipping targets guarded by enemy Air Defense (`find_building(FILTER_TYPE 0x20b)` within 0x900) unless the owner has the STEALTHBOMBER tech. Silo: cities with
   `build_masks +0x62` visible, score `num_buildings * (value+1000)` or `(value+1000)`; **one `game_random(0,0xffff) % 10 == 0` draw** when the city is not owned-visible (`/ 50`). The best target gets:
   airbase / carrier -> `clear_orders` + `add_air_patrol_order(target.x, target.y, container, 0)` on every plane (allies only fighters); silo -> `add_air_attack_ground_order` on the first missile.
   ❓ numeric details of the wonder/diff weights are in `Object_do_launch@64f3b0.c` lines 280-560; `Game::wonder_winning`, `get_diff` are unresolved externally.
- `Build::clear_gather` @623180 / `Build::add_gather_point` @622e70 on an airbase (gather action 3): every plane with `home_base == this`: inside -> `clear_orders` + dropped from `+0x44`; airborne -> STRAFE
  home (no target, `add_strafe_order(-1, -1, this.o, this.who, 0, QUEUE_NEW, 0)`) on clear; on add, planes re-issue an AIR_PATROL through the gather list (`action == 3` points are map positions). The engine's
  `build_clear_gather` / `build_add_gather_point` (queues.cpp:455-492) mark these. ❓ the add-gather arm builds the patrol waypoints from the whole gather list (`Build_add_gather_point@622e70.c`, lines 59-200).
- `Object::eject_contents` (garrison-spec; transport-spec §7.1) from an **airbase**: each ejected unit gets `add_strafe_order(-1, -1, -1, -1, 1, QUEUE_NEW, 0)` (so it flies off, and `check_fuel` finds
  the nearest base). Missile silos use `kill_contents` (`die_uber`). `Build::process_ejection` @6201e0 has **no air branch** (it only calls `Unit::come_out(captain, 1)` once per frame).
- **Commands** (CommandPackage -> Group): `FLIGHT` `Group::action_flight(ox, who, order{ATTACK|MOVE_TO}, ...)` @6fb260: attack: non-planes are split off to `action_attack` / `action_guard`; if the group holds nuke-line
  missiles only missiles act; per plane: existing STRAFE is retargeted in place (fuel `mana_left != 0` else "out of fuel" message), else `add_strafe_order(target, base = get_inside / air home, ...)`; nuke targets require
  `can_nuke` (message), target owner without MISSILE_DEFENSE_BONUS (else "defended"), `dist <= mana * speed` (else "out of range"), `war_allowed`. MOVE_TO onto an airbase / carrier that `can_carry(AIR)` and the plane not
  already based there: `add_strafe_order(-1, -1, base, ...)` (rebase). A group that contains buildings routes to `action_launch_flight` @6fbfb0: for each container pick **one** plane (the cheapest by octagonal
  distance `max + min^2 / (2 max)` for attacks (x200 if it already has an order, /4 for helicopters/`is(...)`), the farthest for moves) among planes with `mana_burn == 0`, within `mana * speed` range, honouring
  RushRules ages, then `action_flight` on the temporary group; messages when nothing qualifies. `AIR_PATROL` `action_air_patrol` @7029d0 (planes; others `action_move_to`): per plane `add_air_patrol_order`
  or append a waypoint (QUEUE_LAST); `launch_patrol` @703580 does the same from buildings; `AIR_ATTACK_GROUND` @703d80 (bombers/missiles at a map point: `can_nuke`, range, MISSILE_DEFENSE_BONUS, `war_allowed`);
  `action_scramble` @7111c0 (carriers): every plane in the chain with `mana_burn == 0` and not busy gets `add_air_patrol_order(carrier pos)` (fixed wing) or a MOVE_TO (helicopter).

## 9. Paradrop (craft 0x284)

`SpellType::cast_paradrop(o, who, x, y)` @671f80 (**Special Forces** ability, craftrules "Paradrop": FLAGS `dlm` (area, doesn't blow cloak, group), MANA 500, JOB_TIME 25, RES_TIME 300; range `PARADROP_RANGE` 64
tiles is checked by `is_castable`, not here). It involves no aircraft: `UnitType::find_nearby_spot(caster type, x, y, &nx, &ny, r0 = 0, r1 = 0x600, step = 0xc0, 0x55555555, FILTER_NOT_ME, ...)` around the target point
(✅ the radii order read off the disassembly 2026-10-24: 0, 0x600, 0xc0; ported in `spells_cast.cpp`), found -> sound S_SPECIAL_FORCES_COMPLETE (own) and `Unit::set_new_location(caster, nx, ny, 1, 1)` (**teleport**, the unit reappears there with no flight). Not found -> feedback string 0x10ab8 +
S_UNIT_PLACEMENT_BLOCKED (own) and `vslot +0xd8` (refund), `mana_burn -= min(mana_burn, spell.mana)`. No `game_random` draw.

## 10. Randomness (in call order of one frame's air logic)

1. `do_air_physics` altitude reroll: `game_random(0,0xffff) % 7` per eligible plane every 8th frame of `(o + frame)` (fixed-wing, non-bomber, non-heli, non-animal) at the very start of the physics (before `check_fuel`).
2. `do_air_physics` invalid next point: one `game_random(0,0xffff)` (bit 0 = turn direction) only when `sharp_turn == 0`.
3. `Unit::come_out` tail (add_to_army, engine-ported, generic): `game_random(0,0xffff)`; missiles use `% 3`.
4. Carrier creation: `Objects::init_unit` x7 (`Guy::init_real`, one draw per guy) in `Build::train`.
5. `Object::do_launch` silo AI scoring: `game_random(0,0xffff) % 10` per candidate city (see §8.3); airbase AI and the rest draw nothing.
6. `Ammo::init` AA rolls and building-shooter scatter (combat-spec RNG table #5-#10).
`find_new_*_target` and `check_fuel` draw nothing; Animals (`think_bird`) have their own draws (movement-spec §2 bird spawn).

## 11. What the port needs

> Ported 2026-10-24 (engine/sim/aircraft*.cpp); what the port found, where the exe differs from this spec (launch cadence 16 frames,
> `queue_up` does not refuse, `vslot 8` = is_live_unit, the missile `is_in_range` sign, gather action 3) and what is left is in
> docs/sim-implementation.md, section "Aircraft". The list below is the original work plan.

Code with `TODO(air)` markers (all under `engine/sim/`): `combat_target.cpp:33` (`is_flying_high/low`, §7), `combat_fight.cpp:262,335` (AA bands; strafing impact slide ❓ not decoded: Guy anim phase),
`combat_orders.cpp:223` (carrier strafes, §7) and `:905` (retarget a STRAFE in place), `queues.cpp:330` (`can_carry` silo filter), `:455/:463/:492` (gather: §8), `:708` (aircraft z at THECEO), `:930`
(aircraft-housing buildings), `:953` (carrier wing, §2), `:966` (airbase limit, §2), `garrison.cpp:383` (helicopter on carrier, §6) and `:618` (`add_strafe_order` after eject), `build.cpp:468`
(`can_carry(AIR)`), `construct.cpp:716` (HOLDS_AIR `do_launch`), `commands.cpp:167` (`Unit::action_unqueue`), `ai_diplo.cpp:70` (ejected aircraft strafe), `guys.cpp:434`,
`script_funcs_order.cpp:214,259,313,324`, `script_funcs_mutate.cpp:485-489`, `script_funcs_query.cpp:394,400`.
Order of work: (1) `StrafeOrder` / `AirPatrolOrder` / `AirAttackGroundOrder` structs with `AirOrder` fields + save walk (state-spec); (2) §2 `can_carry` / `home_base` / `num_aircraft_*`;
(3) `Unit::process` fuel/heal and `do_air_physics` (+ Guy bank float state, which also decides the heading: needs the bank constants of `bank_aircraft`); (4) `do_launch` + `attempt_launch`;
(5) `do_strafe` / `do_air_patrol` / `do_air_attack_ground`, `find_new_*_target`; (6) commands (§8); (7) missile silo; (8) carrier wing + carrier `do_attack`.

Acceptance (all objective):
1. `./build-mac.sh` with zero warnings; `ron-check` 0 failures; no new `TODO(air)` left in the files listed above unless marked ❓ here.
2. `re/tools/sim-all.sh 600 --ai-every 8` on the 156 stock scenarios: self-check OK; digests of scenarios with no air units or bases unchanged; list those that changed (each must contain an air object).
3. Capacity: in a scenario with an airbase, queue 11 non-helicopter planes: the 11th is refused ("building full"), `num_aircraft_here(0) <= 10` at all times; a carrier created through `Build::train`
   starts with exactly 7 `current_upgrade(FIGHTERBOMBER)` units inside (dump the chain) and the RNG state after creation matches 7 `init_unit` draws.
4. Launch cadence: a STRAFE ordered to a plane in an airbase leaves via `come_out` exactly 15 frames after the previous launch; at most one plane per container per 15 frames (dump `launch_timer`).
5. Fuel: a Biplane (`MANA 300`) leaving its base with a never-ending AIR_PATROL turns back (`returning = 1`) when `mana_burn == 300` (frame count verifiable), lands via `land_plane` and is `inside_up >= 0`
   again with `mana_burn` decreasing by 2 per frame; with no base left it `die`s with the out-of-fuel message.
6. Physics: positions of a plane over 600 frames follow `do_air_physics` (speed `get_speed`, 45 degree overshoot rule); `invalid_loc` never rejects an in-map cell; RNG draws per §10 (compare the
   `game_random` call count over a 300-frame replay with planes; use the `.rcx` replays that contain aircraft, `re/tools/rcxdump.mjs`, as the oracle when one is available).
7. Combat: AA rolls use the real `is_flying_low/high`; a plane strafing at < 12 tiles from its target is "low".
8. Save / load round trip (`--roundtrip`) across a frame with planes in the air and in a base, including `launch_timer`, `+0x44` launch list, `mana_burn` and the three order types.

## 12. Evidence

| Function | Address | Used for |
|---|---|---|
| `ObjectData::can_carry(AIR)` / `(TypeIndex)` / `(o, who)` | 646c40 / 645e00 / 6483c0 | §2 capacity and eligibility |
| `ObjectData::num_aircraft_here`, `num_aircraft_limit`, `UnitData::home_base`, `has_nuke` | 645330, 6454a0, 609dc0, 643d40 | §2 |
| `Unit::add_strafe_order`, `add_air_attack_ground_order`, `add_air_patrol_order`, `add_patrol_order` | 5e48c0, 5e41c0, 5e4350, 5e4560 | §3 |
| `Unit::do_air_physics`, `bank_aircraft`, `pitch_aircraft`, `air_turn_speed`, `air_alt_speed` | 5e86d0, 5e9520, 5e8de0, 5ea390, 5ea360 | §4 |
| `Unit::do_strafe`, `do_air_patrol`, `do_air_attack_ground`, `do_patrol` | 5eab00, 5ea620, 5ea420, 5f1910 | §3, §5 |
| `Unit::find_new_air_target`, `find_new_bomber_target` | 5ebc70, 5eb960 | §5 |
| `Unit::check_fuel`, `land_plane`, `UnitData::mana`, `mana_left`, `Unit::process` (air block) | 5e9be0, 5e9950, 609a50, 609a30, 610bc0 | §1, §6 |
| `UnitData::is_flying_low` / `is_flying_high` / `is_plane` / `has_repeat_air` | 60a140 (disasm) / 60a310 / 46ce40 / 46cec0 | §7 |
| `Object::do_launch`, `attempt_launch`, `Build::do_missile_launch` | 64f3b0, 643a10, 622670 | §8 (callers: `Build::process` 61edf0, `Unit::work` 60d180, `Object::do_damage` 64a480) |
| `Build::queue_up`, `do_queue`, `finished`, `train`, `clear_gather`, `add_gather_point` | 620f40, 61e410, 628490, 62f9b0, 623180, 622e70 | §2, §8 |
| `Object::eject_contents` (air arm), `Build::process_ejection` | 64cd20 (transport dir), 6201e0 | §8 |
| `Unit::come_out` (planes: z, orders kept for AI planes), `Unit::do_garrison`, `Unit::do_attack`, `Unit::do_idle` | 617c10, 5e6b80, 5f1b80, 60dcd0 | §6, §7 |
| `Object::fire_ammo`, `Unit::set_new_location`, `UnitData::invalid_loc` | 64c8b0, 5f8d20, 607c30 | §4, §7 |
| `Group::action_flight`, `launch_flight`, `air_patrol`, `launch_patrol`, `air_attack_ground`, `patrol`, `scramble`, `recall`, `return` | 6fb260, 6fbfb0, 7029d0, 703580, 703d80, 7030c0, 7111c0, 6fa7e0, 6fad40 | §8 |
| `SpellType::cast_paradrop`, `SpellType::cast` (0x299) | 671f80, 676ce0 | §7, §9 |
| `FRAMES_BETWEEN_LAUNCHES` | c06248 (`re/decomp/aircraft/dis/Object_attempt_launch@00643a10.asm`) | §1 |
| Animal vtable (+0x30 `is_animal`, +0x180 `think_bird`), Build vtable (+0x3c, +0x190 `count_queue`) | `re/decomp/aircraft/vt_animal_build.txt` | §0, §2 |
