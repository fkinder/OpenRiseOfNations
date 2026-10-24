# Garrisons: entering, leaving and counting units inside buildings

Status 2026-10-23: ported in `engine/sim/garrison.cpp` (see sim-implementation "Garrisons"); ✅ decoded from the decompiles with the lossy functions checked in the disassembly
(`UnitTypeData::can_garrison`, `num_inside`, `find_garrison_build`, `get_garrison_arrows`, `gather_inside`); 🟡 the
decompiler dropped two branches of `Unit::do_garrison` (marked ❓). Not compared with a running game. Transports (units
carried by units), aircraft on airbases / carriers and missiles in silos share the same container links but are
outside this document (their `come_out` / `can_carry` paths are named where they appear).

Related: frame-spec §9 step 6 (the garrisoned branch of `Unit::process`) and §10 (`Build::process_ejection`),
production-spec §6.4 (`Build::train` → `go_inside` / `come_out`, gather points), combat-spec §7.2 (healing inside),
capture-spec (ejection on capture), orders-spec §2.6 (GARRISON command) and §4 (`GarrisonOrder`), replay-spec
(commands 20 GARRISON and 26 EJECTALL).

## 1. The container links

`Object`: `inside_down` (+0x28, i16) / `inside_down_who` (+0x3e, i8): the first object inside this one. `Unit`:
`inside_up` (+0x82, i16) / `inside_up_who` (+0xb4, i8): the object this unit is directly inside (−1 on the map;
`is_on_map` = `inside_up < 0`). Contents form a singly linked chain through the contained units' own `inside_down`:
building → unit A → unit B → …, so `inside_up` of B is A, not the building.

- `ObjectData::inside_bottom(&who)` @644000: the last object of the chain (this object itself when empty).
- `ObjectData::get_inside(&who)` @651a80 (units only): the outermost container, following `inside_up` while the
  container is a unit; −1 on the map.
- `Object::insert_inside(o, who)` @647e90: valid objects only; `b = inside_bottom(objects[who][o])`;
  `remove_from_world(this)`; `b.inside_down = this`, `this.inside_up = b`. Then, for a captain that is not a decoy
  and is now `in_a_building`: the leader's "inside" counters go up — `peasants_inside` (+0xa28, `is_peasant`), else
  `stable_inside` (+0xa2c, `where == 0x1ac`), else `special_inside` (+0xa30, `is_special`), else `barracks_inside`
  (+0xa34, `where == 0x1ab`) — and the Lakota (tribe bonus 19, `lakota_food ≠ 0`) / Americans (tribe bonus 20,
  `americans_barracks_gather ≠ 0`, barracks only) set LEADER_RECOMPUTE (0x2000000). *(Economy: these counters feed
  the Lakota and American gather bonuses; keep them when the economy reads them.)*
- `Object::remove_from_inside` @6480f0: `in_bld = is_building(get_inside())` first; unlink (`up.inside_down =
  this.inside_down`, `down.inside_up = this.inside_up`), `inside_up = inside_down = −1`, `add_to_world(this)`; the
  same counters go down (with `in_bld`).
- `Unit::go_inside(o, who, p3)` @61a2e0: THECEO units update their position (UI); with `p3 == 0` a follower defers
  to its captain; `insert_inside(o, who)`; (console selection: UI); if the container is a unit (transport) and
  `uber_size > 1`: `clear_orders`; then **the live `o_down` follower is put inside too** (`go_inside(o, who, 1)`,
  recursively down the chain); types 0x34 / 0x35 (scholars) set LEADER_RECOMPUTE and snap to the container's
  position, angle 0, CHAR_DEFAULT; the guys' render clocks (+0xd0) are zeroed.
- `Unit::come_out(p)` @617c10 (production-spec §6.4.2, ported): leaves the container; returns 0 on success.

## 2. Counting

`ObjectData::num_inside(p)` @646d50: 0 unless the object is on the map; for every unit in the chain that is a
**captain** (`is_captain`): `p ≠ 0` → +1; `p == 0` → +1 for a government hero, else `+ control_cost` (type +0x2f0;
0 for a decoy). So `num_inside(0)` is the population inside, `num_inside(1)` the number of chains.

`ObjectData::count_inside(kind, t)` @64dda0 walks the chain over **valid** objects (`flags & 1`). `CountIndex` (PDB):
WORKERS 0, PEASANTS 1, ATTACK 2, MILITARY 3, SIEGE 4, CASTERS 5, DECOYS 6, TANKS 7, MISSILES 8, DOMAIN 9, ON_MAP 10,
GATHER_PEASANTS 11, GATHER_WORKERS 12, GATHER_SCHOLARS 13, BUILD_PEASANTS 14, CAN_GATHER 15, STRICT_TYPE 16, TYPE 17,
TYPE_SAME_WHO 18, NON_DECOY_TYPE 19, CATEGORY 20, REGION 21, TRADE 22, GATHER_POINT 23, CAN_GARRISON_AT 24,
CAN_AIR_GARRISON 25, GARRISON_ARROWS 26, GARRISON 27, GARRISON_WITH_TYPE 28, TYPE_INSIDE 29, WHO_INSIDE 30,
CAN_BE_SCHOLARS 31. The kinds decoded:
- WORKERS: `is_worker` (types 50..53). PEASANTS: types 50 / 51. ATTACK: alive units with `type.attack ≠ 0` that are
  captains. MILITARY: alive with objmask 4. DOMAIN: alive with `domain == t`. ON_MAP: `is_on_map`. GATHER_PEASANTS /
  WORKERS / SCHOLARS: units that are peasants / workers / scholars with current action GATHER. BUILD_PEASANTS: types
  50 / 51 with action BUILD_AT. TYPE / STRICT_TYPE: `is(t, 0)` / `is(t, 1)`. TYPE_SAME_WHO: `is(t)` and the unit's
  leader is this object's. NON_DECOY_TYPE: alive units that are not decoys and `is(t)`. CATEGORY: `type.cat == t`.
  REGION: map units whose tregion is t. TRADE: `is_caravan` or `is(0x13e)`. WHO_INSIDE: units of leader t.
- GARRISON_ARROWS: alive unit, captain, not a decoy, objmask 0x20 (OBJMASK_SHOOTS_FROM_BUILDINGS),
  not siege (type vslot 0x10c): a worker whose leader has the tech of the militia type (`M = tribe_can_type(types
  [0x42]) ? 0x42 : the tribe's substitute`; `has_tech(M)`) counts the **militia line**: `(types[current_upgrade(M)].
  attack / 10 + 2) / 3`; other units: not `is(0x42)` → `+ attack() / 10`, `(v + 1) / 2` when `type.max_range == 0`;
  `is(0x42)` (a militia unit) → the militia-line value above.

`BuildTypeData::get_garrison_limit(who)` @633f50 (ported, `build.cpp`): `garrison_max`, towers `+ (k − 1) ×
tower_garrison_upgrade` and forts (vslot 0xfc) `+ (k − 1) × fort_garrison_upgrade` with k = 4 / 3 / 2 / 1 for
FORTGARRISON4 / 3 / 2 (0x2d9 / 0x2d8 / 0x2d7, `has_preq`) / none (`LeaderData::get_fort_garrison` @6db2f0 is that
k). `WallData::get_garrison_limit` (vslot 0x178) = the type's with the building's leader.

`BuildData::get_base_garrison_arrows` @62e340: `type.base_arrows` (+0x2cc), `+ maya_garrison_arrows` for the Maya
(tribe bonus 1) unless the building is a city building (`flags & 0x20`) that is active and whose city has
`city[+0x5f] ≠ city[+0x5e]` (an unassimilated city: no bonus). `BuildData::get_garrison_arrows` @62e3d0:
`div = (attack() + 5) / 10`; 0 → 0; `base = get_base_garrison_arrows()`; `S = count_inside(GARRISON_ARROWS)`; if
`base == 0`: `S += div / 2`; `S /= div`; return `base + min(S, most_shots)` (type +0x2c4).

`BuildData::gather_inside` @46f180 and `GatherPoint::is_inside` @730400: the building's first gather point exists,
its action is **not** 3, and `x < 0 or y < 0` → trained units stay inside ("rally inside").

`UnitData::is_entering_or_exiting` @60a6f0: the current order's transport state is 0 or 1 (boarding / unboarding a
ship); transports are not simulated: false.

`ObjectData::can_carry(domain)` @646c40: AIR → `is(0x1bf airbase, strict) or is(0x15f carrier, strict) or is(0x208
missile silo, strict)`; GROUND → a unit whose type has `carry ≠ 0` (+0x2d4) and is not `is(0x15f)` → true, else the
same three strict tests.

## 3. Who may garrison where: `UnitTypeData::can_garrison(bt)` @61d7d0

`bt` is the building's TypeIndex (or −1 = "any"). From the disassembly (patch version ≥ 9 for the Extended Edition):

```
if domain == 1 (sea) and (unit_flags & 0x10 ("e": barges / merchant fleets) or is(0x15f carrier, strict)): return 0
if unit_flags & 0x1800 ("l" garrison town/tower, "m" garrison fort):
    if bt < 0: return 1
    if bt is(0x1bb fort) or is(0x1b7 tower) or is(0x19e city): return 1
if bt < 0: return 0
if domain == 1: return bt is(0x1b0 dock)
where = type.where (the trainer)
if bt == where and bt != 0x1a4: return 1
if where == 0x1ab (barracks):          return bt in {0x1ab, 0x1ac, 0x1ad, 0x1ae, 0x1af}
if where in {0x1ac, 0x1ad}:            return bt in {0x1ab .. 0x1af}
if where in {0x1ae, 0x1af}:            return bt in {0x1ab .. 0x1af} or bt is(0x1bb) or bt is(0x19e)
return 0
```
(0x1ab barracks, 0x1ac stable, 0x1ad auto plant, 0x1ae siege factory, 0x1af factory.) Before patch 4 only the own
trainer family counted; the Extended Edition is past that.

`UnitData::can_garrison(o, who)` @6099a0: the target is valid (`flags & 1`), a building (vslot 0x20), active (vslot
0x4c), and `UnitTypeData::can_garrison(target.type)`.

`Unit::find_garrison_build(city, who)` @605040: −1 unless `cities[who][city]` is valid; walk the city's buildings
(`city.o`, then `city_down` +0x74): valid, active, `num_inside(0) + control_cost(0) <= get_garrison_limit`, and
`can_garrison(type)`: the nearest by `vector_dist`, else −1.

## 4. The GARRISON order

`Unit::add_garrison_order(ox, whom, find, q, explicit)` @5e4080: QUEUE_NEW → the clear-orders preamble (orders-spec
§2.1); a `GarrisonOrder` (TargetOrder: `ox`, `whom`, `uid = objects[whom][ox].uid` or 0xffff) with `find` (+0x14);
order flag 4 = explicit; appended; QUEUE_FIRST → `clear_partial_path` and the new order becomes current;
`update_action`.

`Unit::kill_garrison_order(p)` @5e2bd0 (called on the captain with 0): for the captain and every live `o_down`
member: if its current action is GARRISON (0x1a): `repath`, `kill_current_order(0)`.

`Unit::do_garrison(order)` @5e6b80, the handler (every frame the order is current; `T = objects[whom][ox]`):

1. A helicopter (`unit_flags & 0x20`) ordered onto a valid carrier ❓(`is(0x15f)`): `can_carry(o, who)` → kill the
   order, `add_strafe_order(−1, −1, ox, whom, 1, QUEUE_FIRST, 1)` (landing); else kill, `add_move_order` to the
   carrier (QUEUE_FIRST) and a "no room" feedback. *(Aircraft: not simulated.)*
2. `T` must be a valid, active building; `whom` must be the unit's leader or a mutual ally (diplos 2 both ways); the
   building must have a garrison limit > 0 and `UnitTypeData::can_garrison(T.type)`; else **kill the order**.
3. Not adjacent (`Object::adjacent_to(ox, whom)` @651f40: `attack_dist(ox, whom, x, y) < 0x60`; a non-"e" ship
   instead compares with `boat_garrison_max_distance` (+0x180 when `type.+0x248 < 4`)): `r = max(x_size, y_size) ×
   96`; `r0 = r + 0x30`, `r1 = −1`, or `r0 = r + 0x1b0`, `r1 = r + 0x330` for ❓`is(0x1a4)`; `angle = find_angle(T −
   u)`; `find_nearby_spot(type, T.x, T.y, &sx, &sy, r0, r1, 0, angle, FILTER_NOT_ME, o, who, 0, 0, −1, 0, −1)`,
   retried with `nocheck = 1`; both failing → kill; else `add_move_order(sx, sy, 1, 0, QUEUE_FIRST, 0, …, −1, −1)`
   and return (the GARRISON order stays underneath).
4. Adjacent, `T` a city building (`flags & 0x20`): `C = cities[whom][T.city]`; ❓(decompile lost the sense) if the
   city is not assimilated to `whom` (`C[+0x5f] ≠ whom`), or it is being taken (`T.hits() / 10 > T.hits_left()`):
   feedback ("cannot garrison") and kill. Otherwise continue as 5.
5. `lim = get_garrison_limit`; `cost = decoy ? 0 : control_cost`; if `num_inside(0) + cost <= lim`: the W-cell owner
   under `T`, when ≥ 0 and not `whom` and not `is_ally(whom, owner)`, blocks it (kill, feedback "enemy territory");
   else `captain.go_inside(ox, whom, 0)` and `captain.kill_garrison_order(0)` (the whole chain is inside; the
   order is gone). Return.
6. Full: if `order.find ≠ 0` and `T.city ≥ 0` is a valid city: `b = find_garrison_build(T.city, whom)`; `b ≥ 0` →
   `kill_current_order`, `add_garrison_order(b, whom, 1, QUEUE_FIRST, order.flags & 4)`. Else feedback "%s is full"
   (console) and kill.

Order upkeep (`Unit::check_target_path`, combat_orders.cpp): a GARRISON order whose target is a building is kept
while the unit walks (the "adjacent_to" test above decides when the handler acts).

## 5. The commands

**GARRISON (20, 13 bytes: i32 ox, i32 whom, i8 queued)** → `CommandPackage::process_garrison`: with a group, when
`ox < 0 or whom < 0` or the target is valid: `Group::action_garrison(ox, whom, queued, find = 0)` @700490:
1. The scenario's ignore-orders list drops its members; `action_begin` (vslot 0x14).
2. QUEUE_FIRST: `set_up_insert`, `action_halt(0)`, `action_garrison(ox, whom, QUEUE_NEW, 0)`, `finish_insert`.
3. `group.+0x10 = −1`. `whom` must be the group's leader or a mutual ally (else console feedback, return). The
   target must be a valid building (vslot 0x10) with `get_garrison_limit ≠ 0` or ❓`is(0x1a4)`, and not
   `is_unassimilated` (else feedback, return).
4. For each member: alive, on the map, `domain ≠ 2` or helicopter, not `is_entering_or_exiting`; a member whose
   current action is GARRISON: `repath`, `kill_current_order(0)`; then if `UnitTypeData::can_garrison(T.type)`:
   `b = find ? find_garrison_build(T.city, whom) : −1`, `b < 0 → b = ox`; with QUEUE_NEW in the scenario editor
   (semaphore byte1 & 8) the unit `go_inside`s directly when there is room; `q' = QUEUE_FIRST` for workers,
   QUEUE_LAST for a packer that is packing (its last order is kept first), else `q`;
   `add_garrison_order(b, whom, find, q', explicit = 1)`.

**EJECTALL (26, 17 bytes: i32 back_to_work, i32 who, i32 eject_o, i32 eject_who)** → `Group::action_eject_all` @710b40:
- `(eject_who < 0 and who == group leader) or who < 0`: for each member in **reverse** order: alive with
  `num_inside(1) > 0` and not `can_carry(AIR)`: `eject_contents(0, back_to_work ? 0x32 : −1, 0, back_to_work ==
  0)`; a member that `is(0x140)` or `is(0x13e)` (transports) left with `num_inside(0) == 0` dies.
- else with `eject_o ≥ 0` a building: repeatedly take the first unit of leader `who` in its chain and `come_out(0)`.

`ScenarioFuncSet::unit_garrison_order(who, o, bwho, bo)` @9f8380 (and the group form): a temporary group with the
unit, `ignore_orders` suspended, `action_garrison(bo, bwho, QUEUE_NEW, 0)`; returns −1.

## 6. Ejection: `Object::eject_contents(kill_if_stuck, type, carry_orders, no_orders)` @64cd20

Nothing unless the object is on the map. A **building** called with `type < 0` outside the editor that cannot carry
aircraft does not eject now: it sets `build_masks |= EJECTING` (0x4000) and `Build::process_ejection` @6201e0 lets
**one chain out per frame** (`come_out(1)` on the captain of the first contained unit; a dead building also adds 1
to its `hold_frames`; the flag clears when the chain is empty). frame-spec §10 / §6 run it for active and for dead
buildings.

Otherwise walk the chain; a unit `u` is skipped when `type ≥ 0` and not `u.is(type)`, unless `types[type]` is a
citizen (50 / 51), `u.is(0x42 militia)` and `no_orders == 0`. For each taken unit:
- `no_orders == 0`: `repath` if it has an action; a militia unit (`is(0x42)`) with spell 0xa4c castable casts 0x293
  (disarm) at the building, QUEUE_FIRST. `no_orders ≠ 0`: `clear_orders`.
- `r = u.come_out(0)`; `r ≠ 0`: `kill_if_stuck` → `u.die()`; next.
- From a **building** with `kill_if_stuck` and `u` a civilian (50..53): citizens 50 / 51 whose leader has the militia
  tech bit (`M` as in §2) `set_type(current_upgrade(M))` and `clear_orders` (they arm); otherwise, with a city, the
  nearest other friendly city (`find_city(x, y, SEARCH_FRIENDLY, who, city, 0x200, FILTER_NOT_ME, …)`) gets a FLEE_TO
  (`add_move_order(c.x, c.y, 4, 0, QUEUE_NEW, …)`).
- From an airbase (`is(0x1bf)`): `add_strafe_order(−1, −1, −1, −1, 1, QUEUE_NEW, 0)`.
- From a **unit** (transport): sound; `same_damage` (the cargo takes the carrier's damage fraction); caravan slot
  hand-over; `carry_orders`: a single unit takes the transport's order list and inverted path, a chain joins a group
  with the transport's orders; the transport bit; selection / hot unit (UI). *(Transports: not simulated.)*

`Object::kill_contents` @645550: every contained unit `come_out(0)` then `die_uber(0)` (missile silos).

Callers: capture (`Build::swap_team`: `eject_contents(1, −1, 0, 1)` after `kill_contents` for a silo, capture-spec
§5.2; `check_capture` sets EJECTING when a garrison is present), a destroyed building (`take_damage`: `construct_hits
<= damage` with a garrison and no AIR carry → `eject_contents(1, −1, 0, 1)`, production-spec 6.11), `Object::close`
of a building with `num_inside(1)` (AIR carriers `kill_contents`, others `eject_contents(1, −1, 0, 1)`), alliances
ending (`Leader::eject_my_shit_from_his_ass`, ported in ai_diplo.cpp), the AI's city alarm (ai-spec 15).

## 7. Garrisoned units each frame (`Unit::process`, frame-spec §9 step 6)

Off the map: `unit_masks2 &= ~(0x20 | 0x40 | 0x8000)`; `unit_masks &= ~(BLOW_COVER 0x1000 | COVER_BLOWN 0x10000)`,
and also `~UNDER_COVER 0x800` unless CLOAK_RUNNING (0x2000). Every 32 frames staggered by **who** (`(frame + who) %
32 == 0`), an AI-controlled unit (`UNIT_AI_CONTROL`) whose outermost container is a city building (`flags & 0x20`)
of a city without UNDER_ATTACK | ALARM (`city_flags & 0x42`) leaves: `come_out(0)`. Then every guy copies `last_z =
z` (+0x70 ← +0x14). `process_healing` ran before (combat-spec §7.2, the garrison branch); `work()` does not run.
`Unit::inc_time` (frame-spec §9) still ticks the guys of garrisoned 0x34 / 0x35 types only.

`Unit::action_come_out` @5e20b0 (a single unit's "leave" button): clear-orders preamble; scholars (0x34 / 0x35) of
the same leader swap their graphic pieces with the others inside (UI); `come_out(0)`.

## 8. What the port needs beyond this document

- `Build::train` already `go_inside`s trained units and lets `come_out` place them (production-spec §6.4.2);
  `gather_inside` keeps them in. Healing inside: combat-spec §7.2. Buildings fire garrison arrows: combat-spec §4
  (`get_garrison_arrows` above). Capture ejection: capture-spec §5.2. The AI's garrison / alarm logic: ai-spec §15.
- Not here: transports and boarding (BOARD_SHIP / AWAIT_BOARD orders), aircraft on airbases and carriers, missiles.
