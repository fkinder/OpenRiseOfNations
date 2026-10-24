# Unit movement and pathfinding

Status: ✅ per-frame call chain, MoveOrder life cycle, path stack representation, `astar_path` (neighbour order, costs
plumbing, heuristic, tie-break, limits, time slicing, path emission), `find_upath` pre/post passes, `do_move` /
`move_step` / `find_path` decision trees, road and river searches · 🟡 grid predicates (named here, specified in
docs/world-spec.md when it exists), collision resolution and group movement details (see the per-section notes) ·
❓ nothing validated against a running game (no replay with unit movement exists on this Mac).

Deliverables: this document; `re/tools/astar-ref.mjs` (a JavaScript model of `astar_path` + the `find_upath`
straightening pass, `node re/tools/astar-ref.mjs --selftest`). Decompiled sources (local only, `.gitignore`d):
`re/decomp/move/` (job `re/jobs/mv_dc_core.txt`, `mv_dc_core2.txt`), `re/decomp/move_asm/` (`mv_asm_core.txt`),
`re/decomp/move_pf/`, `move_road/`, `move_guy/`, `move_coll/`, `move_group/` (jobs `re/jobs/mv_*.txt`).

Addresses are `riseofnations.exe` VAs, names are PDB names. "rnd(n)" below means `game_random.get(0, 0xFFFF) % n`
(signed `idiv`; the draw is never negative). Integer division is C division (truncates toward zero) unless noted.

## 1. Units, coordinates and primitives

| Name | Size (world units) | Grid | Conversion |
|---|---|---|---|
| `Coord` | 1 | — | positions, path points |
| `UCoord` ("unit coord") | 48 (0x30) | `16·xs × 16·ys` | `div_3_table[c >> 4]` |
| `TCoord` (terrain tile) | 192 (0xc0) | `4·xs × 4·ys` (`world+0x18`/`+0x1c`) | `div_3_table[c >> 6]` |
| `WCoord` (gameplay tile) | 768 (0x300) | `xs × ys` (`world+0`/`+4`) | `div_3_table[c >> 8]` |

- `div_3_table[v]` (pointer @cae5fc, built by `init_coord_lookup_array` @681db0) is exactly `floor(v / 3)`, also for
  negative `v` (the table is centred). With arithmetic shifts, `div_3_table[c >> k]` = `floor(c / (3·2^k))`.
- Unit positions are stored XOR-obfuscated: `SubObjectData.x_internal/y_internal/z_internal` (+0x10/+0x14/+0xc) hold
  `value ^ 0x63637`. All formulas below use the plain value. (The XOR matters only for the checksum image, which
  hashes the stored bytes; state-spec.md §6.)
- Cell centres: UCoord `u·48 + 24`, TCoord `t·192 + 96`, WCoord `w·768 + 384`.

**`vector_dist`** @46cff0 (ECX, EDX; the `(Coord&×4)` @46d060 and `(int×4)` @46f020 overloads subtract and jump here).
With `M = max(|a|, |b|)`, `m = min(|a|, |b|)` (when `|a| == |b|` the second branch runs; the result is the same):
`M == 0 → 0`; `m ≥ 60000` (unsigned) → `(m + 2M) >> 1` (logical); else `M + (m·m) / (2M)` (32-bit unsigned multiply and
divide). This is the "max + min²/2max" approximation of the Euclidean length (vector_dist(300,400) = 512).

**Angles** are 32-bit binary angles, full circle = 2^32. Angle 0 points to −y, 0x40000000 to +x, 0x80000000 to +y,
0xC0000000 to −x (clockwise with +y down-screen). A displacement of length `r` at angle `a` is
`(dx, dy) = (sin(a, r), −cos(a, r))`.
- `find_angle(dx, dy)` @92d130 (ECX, EDX) is the inverse. With `ny = −dy`: `dx == 0 → (ny > 0 ? 0 : 0x80000000)`
  (so (0,0) → 0x80000000); `ny == 0 → (dx > 0 ? 0x40000000 : 0xC0000000)`; otherwise `a = |dx|`, `b = |ny|`,
  `q = (min(a,b) << 14) / max(a,b)` (signed), `E = (((0x2800 − ((|0x1333 − q|·0xb00) >> 14))·q) & 0xffffc000) << 2`,
  `xdom = a > b`, and the octant table:

  | | `ny > 0` | `ny ≤ 0` |
  |---|---|---|
  | `dx > 0` | `xdom ? 0x40000000 − E : E` | `xdom ? 0x40000000 + E : 0x80000000 − E` |
  | `dx ≤ 0` | `xdom ? 0xC0000000 + E : −E` | `xdom ? 0xC0000000 − E : 0x80000000 + E` |

  (`min << 14` overflows for |d| ≥ 131072; map coordinates never get there.)
- `sin(a, r)`, the inlined wrapper every caller uses around `sin_table` @a46a00: `r == 0 → 0`; if `a < 0` (signed)
  `r = −r`, `a &= 0x7fffffff`; if `a & 0x40000000` then `a = 0x7fffffff − a`; then `sin_table(a, r)`.
  `cos(a, r)` (`cosx` @92d0c0) = `sin(a + 0x40000000, r)`.
- `sin_table(a, R)` @a46a00 (ECX, EDX) on the folded first-quadrant angle: table `t[256]` @e32f40 with
  `t[i] = trunc(sin(i·1.570796327/255)·65535)` (`trig_init` @a46980; the constant is the double 0x3ff921fb54524550,
  `t[255] = 65535`; libm `sin` reproduces it). `i = (a & 0x3fffffff) >> 22`, `f = a & 0x3fffff`,
  `v = t[i] + (((t[(i+1) & 0xff] − t[i])·f) >> 22)` (int32 wrapping multiply, arithmetic shift); result
  `R < 0xffff ? (v·R) >> 16 : R < 0xffffff ? (v·(R >> 8)) >> 8 : v·(R >> 16)` (signed compares, arithmetic shifts:
  negative results round toward −∞, so `sin(−a, r)` is not exactly `−sin(a, r)`; magnitudes below −32768 overflow the
  product and must wrap the same way). Against true sine the error is at most ≈ 0.3 %.
- `angle_diff(a, b)` @92d0b0: `d = a − b` (u32); `d > 0x80000000 ? ~d : d` (note `~d`, not `−d`). The movement code
  inlines the same idiom (`move_step`).

**RNG.** `game_random` (docs/replay-spec.md §12). All movement draws are `get(0, 0xFFFF) % n`.

## 2. Where movement runs in a simulation frame

`Game::do_frame` order (systems-map.md; the full frame skeleton is in frame-spec.md, the order machine in
orders-spec.md), as far as movement is concerned:

1. **Step 3 `GameDaemon::process_all`** @732700: `busy--`; for each player `repaths[p] = repaths[p]/2`, then 0 if
   `< 3`; victory, danger (every 200 frames), markets, regions; `process_coll_blocks` (garbage-collects empty collision
   blocks, ≥ 5 W cells per frame round-robin, §10); **`Groups::process`** (group movement, §11). Then
   `Armies::process_all` (AI armies, §11.4).
2. **Step 4 `Objects::process_all`** @65dce0. Unit order: for `k = 0..9`, player `p = (frame + k) % 10` (**the
   starting player rotates every frame**); if `leaders[p].flags & 1`, for unit index `0 .. unit_mark[p]−1`: alive
   (`flags & 1`) → vtable +0x9c (`Unit::process` / `Animal::process`), else `hold_frames--` if non-zero. Buildings
   (2000..) and walls (3000..) follow per player in fixed order 0..9. Every 32 frames gaia air units (birds) are spawned
   with 2 draws each (`rnd(xs)`, `rnd(ys)` per spawn; skipped draws when the dimension is 1), every 64 frames one herd is
   processed. Then `Objects::inc_time` (vtable +0xa0 → `Unit::inc_time` → `Guy::inc_time` per guy, then +0x154
   `execute_events`).
3. **`Unit::process`** @610bc0 (unit on the map, `inside_up < 0`): decrement `recharging` +0xae, `full` +0xac,
   `waiting` +0xad, `healing`; spells/mana; production; `process_healing`; every 16/32 frames (staggered by `o`) cloak,
   attrition, supply; then **vtable +0x188 `Unit::work`** @60d180; then **`Guy::process`** for guys `0 .. guy_mark−1`
   and `squad_size .. guys.length−1` (§9). Garrisoned units only copy `guy.last_z = guy.z`.
4. **`Unit::work`**: housekeeping on the current order, periodic checks (every 16 frames staggered by `o`:
   `check_target_path` for targeted actions (§3.6), `repath` for GUARD every 64), `clear_partial_path` when a saved
   search exists but the current order is not a move, boat collision (`detect_boat_collision`) for non-moving boats
   within 4 frames of a collision, then **`do_job(order_type, order)`** @617a10: MOVE_TO and FLEE_TO → `do_move`;
   ATTACK_TO / EXPLORE_TO → `do_move` + a target scan every 15 frames; GROUP_MOVE → `do_group_move` (§11) which itself
   ends in `do_move`; FOLLOW, GUARD, … build their own MoveOrders.

Everything below happens inside one `Unit::process` call, so units move **sequentially**: a unit sees the positions
other units already reached this frame (earlier in the rotation) and last frame's positions of the rest.

## 3. Orders and the path stack

### 3.1 MoveOrder

`MoveOrder` (0x5c bytes, virtual base `UnitOrder` with the `flags` byte; PDB layout) fields used by movement:
`x, y` (target), `angle` (final facing), `dest` (1 = a sub-destination is latched in `dest_x/dest_y`), `tolerance`,
`pause`, `retry` (frames to wait after a failed search), `attempts`, `timer` (order lifetime; 1 → expire), `facing`,
`dest_x/dest_y`, `last_x/last_y`, `coll_x/coll_y` (where a collision happened), `orig_x/orig_y`, `off_x/off_y`.
Order flags (`UnitOrder.flags`): bit 0 = **pathed** (the long path is on the stack; `MoveOrder::is_pathed` @488980
returns `flags & 1`; `do_move` sets it after planning, arrival clears it), bit 2 =
the `f7` argument (❓ meaning; orders-spec.md §3.2), bit 5 = the `f11` argument (with unit flag 0x20 it ejects passengers on
arrival).

`Unit::add_move_facing_order(ux, uy, angle, kind, pathed, queue, f7, facing, ox, oy, f11)` @5e55c0 (all move-type
orders are created here; `add_move_order` @616ed0 converts a Coord target with `div_3_table[c >> 4]`):
- `QUEUE_NEW`: clears bit 0x4000000 of `unit_masks`, **empties the path stack** (`path.length = 0`), closes all
  orders, `clear_partial_path`, `update_action`. (kind 2 = ATTACK_TO while attacking a valid target re-adds that attack as the head order afterwards.)
- kind 1 MOVE_TO, 2 ATTACK_TO, 3 EXPLORE_TO, 4 FLEE_TO.
- `x = ux·48 + 24`, `y = uy·48 + 24` (**targets snap to UCoord centres**), `dest = 0`, `dest_x/dest_y = x/y`,
  `last = (−1, −1)`, `off_x = (short)(x − (x/768)·768)`, `off_y` likewise (the target's offset inside its W cell),
  `orig = (ox, oy)`, `facing`, `pause = retry = timer = 0`; flags bit 0 = `pathed`, bit 2 = `f7`, bit 5 = `f11`.
- `QUEUE_FIRST` makes it the head (`clear_partial_path` first).

### 3.2 The path stack

`UnitData.path` (+0xb8) is a `Stack<PathData>`; `PathData = {to_x, to_y, tolerance, flags}` (16 bytes). The **top**
(last element) is the next point to walk to. It is walked by `Unit::walk_data` (flag 2 group, state-spec.md §5), so it is
saved and checksummed.

| Flag | Meaning | Set by |
|---|---|---|
| 0x01 | final point of an order (the order target) | `do_move` (pushes `{order.x, order.y, 0, 1}` before planning) |
| 0x02 | fine (UCoord-level / anti-unit) waypoint | `astar_path` when `anti_unit` or step 0x30 |
| 0x04 | transport/region change ahead: walk it with tolerance 0, may turn in place | `astar_path` transport nodes, closest-node fallback, `do_move` region check, `find_path` |
| 0x08 | midpoint of a `go_around_building` detour; `detect_unit_collision` ignores units while it is the top | `go_around_building` |
| 0x10 | waypoint is inside a building footprint (`resolve_block` on arrival) | `astar_path` at step 0xc0 |
| 0x20 | caravan road segment | trade code |

Typical stack for a long move, bottom → top: `{target, tol 0, flags 1}`, W-level waypoints (tol 0x180), T-level
waypoints (tol 0x60, or 0 for transports), U-level waypoints (flags 2, tol 0) near a blocker.

`Unit::kill_current_path` @5e31d0 pops entries up to and including the first one with flag 1 (the current order's
segment) and calls `clear_partial_path`. `Unit::kill_current_order` @5e2cb0 removes the head order (for a move-type order
that `is_pathed`, also `kill_current_path`), then `clear_partial_path` and `update_action`.

### 3.3 Saved (time-sliced) searches

A UCoord search that runs out of its per-call budget stores its open list, open refs, closed list, valid-cache tree and
collision-block tree in `UnitData` +0x104…+0x114, plus `tol` +0x128, `offset` (= start direction) +0x12c, `start_dist`
+0x130, `valid_hit` +0x134, `avoid_land/avoid_sea` +0x138/+0x13c, `endx/endy` +0x140/+0x144, `traversed` +0x148
(§7.5). **None of these are walked**: a save (or a CHECK_SUMS) does not see an in-progress search, and a loaded unit
restarts without it. `clear_partial_path` @5e3920 frees them (recycles all nodes and trees).

### 3.4 `Unit::repath` @5e29b0

Not a search: it drops movement orders. While the head order is a move type (1–4, 0x12 FORM unless its flag bit 2,
0x13 GROUP_MOVE, 0x15 GROUP_ATTACK_TO): a GROUP_MOVE of a unit in a group calls `Group::kill_group_move(order.id)`;
anything else `kill_current_order`. Callers then re-plan on their next think.

### 3.5 `Unit::do_move` @5f7b30 (MOVE_TO, FLEE_TO; the tail of every move-type behaviour)

`m` = `order.get_move_order()`. In order:

1. **Fires-while-moving units** (type flag `v` 0x200000 with an attack): if the combat stance is HOLD_FIRE, or the
   action is ATTACK and the type is not MELEE_AND_RANGED (`k` 0x400) → `cavarch_o = −1` (no moving fire while walking
   to an attack); else `cavarch_fight` (if `cavarch_o < −1`, only when `(o + frame) % 32 == 0`). combat-spec 3.10.
2. **Resume a saved search** (`openlist != 0`), `t = frame − collide_frame`:
   - action ATTACK (10): if `t > 0`, `t % 4 == 0`, `repaths[who] != 0` and the attack order's byte +0x1c is 0 →
     `find_new_target`, `collide_frame = frame`, return 0.
   - action GATHER (7): if `t > 0`, `(t + 2) % 8 == 0` and `vector_dist(|m.x − x|, |m.y − y|) < 0x120` → remember
     `avoid = (m.x, m.y)`, reset the gather order's target fields (+0x14, +0x18, +0x20 = −1, +0x24 = 1), release a
     worker's held doobers, `kill_current_order`, return 0.
   - if `t > 3`, `t` odd, and `detect_unit_collision(m.coll_x, m.coll_y, 1, 1, 0, 0, 1) == 0` (the blocker left):
     `m.dest = 0`, `clear_partial_path`, set order bit 0, `collide = 0`, return 0.
   - if `(o + frame) % 4 == 0`: `repaths[who]++`.
   - `collide++`; `m.dest = 0`; `find_upath_restore(path, who, o)` (§6.3).
   - return 0 (if the search finished and `frame − 5 > collide_frame`, `collide = 0` first).
3. **Timer**: if `m.timer > 0`: `== 1` → `kill_current_order` and run `work()` again, return; else `timer--`.
4. **Action checks** while an action rides on the move:
   - ATTACK target still valid (alive, same uid), **ranged units only** (raw `type.max_range != 0`; @5f7d1c):
     - unit target: when it faces the attacker (`T.angle − angle + 0x80000000` within ±60°), or `flanking == 0`, or it
       is not moving, and it is in range (4-arg `is_in_range`, slack = `!mandatory`) and `Objects::find_collision(x, y,
       o, who, 1)` is clear → `kill_current_order`, return 0 (stop and shoot). Otherwise, for a non-mandatory attack by
       a captain: every 16 frames (staggered) `find_melee_target(−1, &w, 0, 0, filter = siege ? 2 : 1)`; the candidate
       is that result when in range, else the scan's `near_o/near_who`; if the candidate is alive, in range, not a
       `poor_target`, a unit and not "civilian" (the `fight` re-evaluation rule), the action's `in_range = 1` and
       `change_target(old → candidate)` (the unit and its followers; drops the approach moves), return 0.
     - building target: unless an AI siege unit, in range, `valid_target` and `find_collision` clear →
       `kill_current_order`, return 0.
     (The earlier text said "melee captains" retarget; the decompile keeps both rules inside the ranged branch.)
   - ATTACK target gone: if `vector_dist(|x − m.x|, |y − m.y|) ≤ 0x480` and not unit flag `e` (0x10) → `repath`,
     return 0.
   - TRADE (15) with no valid partner → `kill_current_order`, return 1.
5. **Retry wait**: if `m.retry != 0`: `retry--`; when it reaches 0, `attempts += 3`; return 0.
6. Modern infantry without a general (`(o·17 + frame) & 0x7f == 0`, unit not flagged 4): snap to guy 0's position and
   angle, `set_anim(CHAR_PACK)`, `retry = guy0.end_time`, `attempts = −3`, return 0.
7. `attempts--` if non-zero.
8. **Plan** if the order is not pathed or the path is empty: push `{m.x, m.y, 0, 1}`; `r = find_wpath(path, x, y)`
   (§6.1). If `r < 1`: make sure the top is that final entry (push it again otherwise), `r = find_tpath(path, x, y)`
   (§6.2), same fix-up. Set order bit 0. **If the path now has more than 10 entries, return 1** (the unit does not move
   this frame).
9. **Latch the next sub-destination** if `m.dest == 0`: `top = path.top`; `m.dest = 1`; clear `unit_masks` bit 8
   ("straight line verified"); `m.dest_x/y = top.x/y`; `unit.tolerance (+0x60) = top.tolerance`.
   - trade + flag 0x20: caravan road bookkeeping (`TerrainOut::caravan_step`, `Caravan::verify_road`, clearing 0x20 on
     every entry when the road is broken).
   - final entries (and trade): if `get_tregion(unit tile) != get_tregion(top tile)` → rewrite the top with tolerance 0
     and flag 4.
   - `detect_unit_collision(dest, 0,0,0,0,0)` ≠ 0 (someone stands on the point): for a final point of a TRADE, GATHER,
     ATTACK or BUILD action → `kill_current_order`, return 0. Otherwise, unless the occupant has a move order,
     `tolerance = max(tolerance, 3·occupant.type.big_radius)` and write it back into the top entry.
   - **Arrival test**: `vector_dist(|x − dest_x|, |y − dest_y|) ≤ tolerance` → `m.dest = 0`, pop; a non-final entry
     returns 1; a final one: if the unit has exactly one order, `set_angle(m.angle)`; clear order bit 0;
     `kill_current_order`; return 0.
   - flag 0x10 and `resolve_block()` → return 1.
10. **Speed**: `s = get_speed(x, y, 0)` (vtable +0x17c, §9.1); `if ai_speed > 1: s *= ai_speed`; modern infantry:
    `s = (5s + (5s < 0 ? 3 : 0)) >> 2`.
11. If `unit_masks & 8` is **clear** (straight line not yet verified): `path_recursion = 0`,
    `r = find_path(m.dest_x, m.dest_y)` (§4):
    - `r == 0` (clear line): if the (possibly new) top is the unit's own position, clear bit 8; else set bit 8,
      `m.dest = 1`, `m.dest_x/y = top`, `tolerance = top.tolerance`. If bit 8 is now set → step 13.
    - `r == 2` → return 1.
    - otherwise (blocked): if the target tile `invalid_loc(m.x/192, m.y/192, 1,0,0,0,1)` and the unit has more than one
      order → `kill_current_order`, return 1. Otherwise **local re-plan**:
      - `R = rnd(5)`: `far = R == 2 ? 0x600 : R == 0 ? 0x1800 : 0xf00` (**one game_random draw per blocked frame**).
      - if `|m.dest_x − x| + |m.dest_y − y| > far`: `r = find_wpath(path)` (4-arg wrapper, from the unit's position);
        "used W" = 1.
      - else: if the top is not final and equals `(m.last_x, m.last_y)`, pop it. If `collide == 0`: pop entries while
        `!(flags & 0x21)` and `1 ≤ tolerance ≤ 0x60` (stale T-level waypoints), then `r = find_tpath(path)`; else
        `r = find_upath(path, who, o, fast = 0)`. "used W" = 0.
      - a positive `r` with an unchanged path length counts as 0.
      - `r == 0`, path empty → **retarget** (below). `r == 0`, top not final: pop it (if it had flag 4 and the path is
        not empty, give flag 4 to the new top), `m.dest = 0`, return 1. `r == 0`, top final: pop, clear order bit 0,
        `kill_current_order`; if the new head order is ATTACK or BUILD, kill that too; return 1.
      - `r == −1` (no path, **or a UCoord search that was time-sliced**: `find_upath` returns −1 after saving): handled
        like the final case — the order is killed and its saved search freed. Reproduce as is.
      - `r > 0` → **retarget**: `m.last = (−1, −1)`, `m.dest = 1`; if the top has flag 0x10 and `resolve_block` → return
        1; `path_recursion = 0`; `find_path(top)`: 0 → set bit 8, 2 → return 1; if bit 8 is still clear → (unless
        "used W") `resolve_block`; return 1. Else `m.dest_x/y = top`, `tolerance = top.tolerance` → step 13.
12. If bit 8 was **already set** on entry: **pause** — if `m.pause != 0`: `pause--`; for orders other than ATTACK_TO (2)
    and GROUP_ATTACK_TO (21) return 1; for those, units without an attack value `set_anim(CHAR_DEFAULT)`; return 1.
13. `avoid = (−1, −1)`; return **`move_step(m, s)`** (§5).

### 3.6 `Unit::check_target_path` @5e22d0 (attack chase)

Run by `work` every 16 frames for targeted actions; specified in full in combat-spec 3.7 (it replaces this outline).
It does **not** repath for an invisible target (it returns 0 and keeps the plan), and the flank cone does not drop
the move: it selects the range test — a unit target in front (±60°) or flanked but standing is tested from the
current position (in range → `repath`, else keep walking), buildings and fleeing units from the planned end point
(`orders_x/orders_y`). Out of range, plain attacks `repath` and call `find_attack_pos` (which draws `game_random` for
building/wall targets only) and `add_move_order(pos, kind 1, QUEUE_FIRST)`; group orders go through
`Group::action_attack`; DEFENSIVE attackers past their leash call `find_new_target`.

## 4. `Unit::find_path(dx, dy)` @5fb910: the straight-line check

Despite its name this is not a graph search. It decides whether the unit can walk straight to `(dx, dy)` (the latched
sub-destination) and, when a building is in the way, inserts detour points. Returns **0** = line clear (walk), **1** =
blocked or nothing to do (re-plan), **2** = a nested call returned 2.

Skipped (returns 0) for helicopters (unit flag `f` 0x20). Otherwise:

1. If the unit's own tile is invalid (`invalid_loc(tile, 1,0,0,0,0)`), `UnitType::find_nearby_spot` and
   `set_new_location(spot, 1, 0)` (teleport out), return 1.
2. If the target equals the position, return 0.
3. If the W-cell Manhattan distance `|wx − twx| + |wy − twy| > 4` and the target tile is valid
   (`invalid_loc(t, 1,1,0,0,0)`), return 1 (too far for a straight walk: re-plan at T level).
4. `a = find_angle(dx − x, dy − y)`; `s = max(get_speed(x, y, 0), 3)`; `sx = sin(a, s)`, `sy = cos(a, s)`;
   `path_recursion++` (saturating at 255).
5. **Pull the target out of an obstacle**: while the target tile is invalid (`invalid_loc(t, 0,0,0,0,0)`): clamp `sx`
   to the remaining `dx − x` (and `sy` to `−(dy − y)`) when smaller in magnitude; if the order's `dest` equals the target,
   move it too; rewrite the top entry the same way when it equals the target; `target −= (sx, −sy)`; stop when both
   steps are 0.
6. Air units (domain 2) are done here (return 0).
7. If the target is now the unit's position: `avoid = target`, return 1.
8. **March** from the unit toward the target in steps of `s` along the re-computed angle (`x += sin(a', s)`,
   `y −= cos(a', s)`, clamped to the remainder), re-aiming each step, testing each **new tile** once with
   `invalid_loc(t, 0,0,0,0,0)`. The march ends with 0 when the remaining distance is within one step on both axes, or
   with 0 when the Manhattan remainder stops shrinking (`return 0` if it grew).
9. On a blocked tile: if `path_recursion < 10`, `go_around_building(prev point, a', s)` (§10.5), then
   (if it did not fail) re-order the newly pushed detour points so that points equal to the unit position are dropped,
   and recurse `find_path(top)`; a 0 result keeps the detour (re-flagging transport (4) by comparing water/land of
   consecutive points), sets `MoveOrder.last = position` and `unit_masks |= 8`; 2 propagates. Otherwise return 1.

Notes: `find_path` reads `get_speed` but never moves the unit; its only side effects are the detour pushes, the target
pull-in, `avoid_x/avoid_y`, `MoveOrder.last_x/y` and `path_recursion`.

## 5. `Unit::move_step(m, speed)` @5faf30: one frame of motion

1. Entrenched units lose the entrenchment (`unit_masks` 0x2000000, `unit_masks2` 0x10000; graphic event removed).
2. Animation `CHAR_WALK`, or `CHAR_ATTACKWALK` for fires-while-moving units that have a target (`cavarch_o ≥ 0`; their
   guys get turret targets; when guy 0 has no target node the cavarch target is cleared).
3. `(Dx, Dy) = (m.dest_x − x, m.dest_y − y)`; `wp = path.top` (its tolerance and flags are used below);
   `a = find_angle(Dx, Dy)`; **`set_angle(a, 0)`** (unit `angle`/`dest_angle`, §9.2).
4. **Heading**: `g = guys[0]`, `h = g.angle`, `δ = a − h` (u32), `|δ| = δ > 0x80000000 ? ~δ : δ`.
   - `|δ| < 0x2222220` (3°) or `|δ| ≤ turn_speed(g, 0)` → `rem = 0`, `h' = a`.
   - else `rem = |δ| − ts`, `h' = h + ts` if `δ ≤ 0x80000000` else `h − ts`.
5. `L = |Dx| + |Dy|` (Manhattan).
6. **Ground and sea units** (not helicopters, or a helicopter with `rem > 0x40000000`):
   `k = (type.turn_speed < 0xe38e38c) + 1` (1 = fast turner, 2 = slow turner; 0xe38e38c = 20°).
   - `L < 192k` or `wp.flags & 4`: if `rem != 0`, **turn in place** (`Guy::do_turn(g, a, h', 0, 1)`), return 1.
   - else: limit = `rem < 0x20000000` (45°), except for non-land units or units with object mask 0x200000 that are at
     least `384k` away, where the limit is `rem < 0x38e38e3a` (80°). Over the limit → turn in place, return 1.
   - Speed halving: if `rem ≥ 0x20000000 / k` → `speed /= 2`; else if `unit_masks & 0x100000` (soft collision last
     frame, §10.3) → `speed /= 2` and clear the bit.
   - `Guy::do_turn(g, a, h', 0, 0)` (turn while moving).
7. **Helicopters** (`rem ≤ 0x40000000`): `Guy::do_turn(g, a, h', 0, 0)`; then recompute from the guy's new angle:
   `h = g.angle`, `δ = a − h`; `|δ| < 0x2222220` → `h' = a`; else `ts = turn_speed(g, 1)`; `h' = |δ| > ts ? h ± ts : a`.
8. **Partial step** (`speed < L`):
   - `sx = sin(h', speed)`, `sy = cos(h', speed)` — **the unit moves along its current heading `h'`, not along `a`**.
   - if `L < 2·speed`: if `|Dx| < |sx|` then `sx = Dx`; if `|Dy| < |sy|` then `sy = −Dy`.
   - `(nx, ny) = (x + sx, y − sy)`; off the map (`< 0` or `≥ tiles·192`) → return 1.
   - `blocked = detect_unit_collision(nx, ny, 0,1,0,0,0) != 0` (§10.3). If blocked, `wp.flags & 2`, the waypoint itself
     is free (`detect_unit_collision(wp, 1,1,0,0,0) == 0`), and `|Dx| ≤ 0x60`, `|Dy| ≤ 0x60`: step straight onto
     `(m.dest_x, m.dest_y)` instead, not blocked.
   - blocked: `set_anim(CHAR_DEFAULT)`; if `rem != 0` return 0; if `L ≥ 3·(B.big_radius + my.big_radius)` or
     `collide < 26` or (`wp.tolerance == 0` or `wp.flags & 2`) and not `wp.flags & 1` → `resolve_unit_collision(nx, ny)`
     (§10.4), return 0; otherwise `tolerance = 2L` (a unit stuck close to a toleranced point counts as arrived) and fall
     through to the arrival test.
   - not blocked: if the step enters a new tile and that tile is `invalid_loc(·, 0,0,0,0,0)` → clear `unit_masks` bit 8
     (forces a new straight-line check), return 0. Else `set_anim(walk anim)`, `set_new_location(nx, ny, 0, 0)` (§9.3);
     a 0 result returns 1.
   - **Arrival**: if `tolerance < |m.dest_x − x| + |m.dest_y − y|` (new position, Manhattan) → return 1. Else
     `m.dest = 0`, pop the top; non-final → return 1; final → if the unit has exactly one order `set_angle(m.angle)`;
     clear order bit 0; if order bit 5 and unit flag 0x20 and passengers inside → `kill_current_order` + eject all
     (`Group::action_eject_all` on a one-unit group), return 1.
9. **Last step** (`speed ≥ L`): target = `(m.dest_x, m.dest_y)` exactly.
   - free (`detect_unit_collision(dest, 0,1,0,0,0) == 0`): unless the step changes tile into an invalid tile, set the
     anim (`CHAR_DEFAULT` when `Dx = Dy = 0` and the unit has < 2 orders, else the walk anim) and
     `set_new_location(dest, 0, 0)` (0 → return 1). `m.dest = 0`, pop.
   - blocked: `collide_o = collide_who = −1`, `m.dest = 0`, pop, `set_anim(CHAR_DEFAULT)` (**a blocked last step still
     counts as reaching the point**).
   - popped entry not final → return 1. Final: clear order bit 0; passenger eject case as above; if the unit has one
     order or its action is GATHER, `set_angle(m.angle)`.
10. `kill_current_order`, return 1.

Arrival tolerances therefore differ by stage: `do_move` tests `vector_dist ≤ tolerance` before moving, `move_step` tests
Manhattan `≤ tolerance` after moving, and the last step lands exactly. Tolerances come from the waypoint (0x180 W level,
0x60 T level, 0 U level and final points, raised to `3·big_radius` of a unit standing on the point).

## 6. The path searches: entry points

All searches use the single global `PathFinder pathfinder` @e85e40 (fields in the PDB; `PathFinderData` at +0x40:
`openlist` +0x40, `openlistrefs` +0x44, `closedlist` +0x48, `blocklist` +0x4c, `validlist` +0x50, `pathing_unit` +0x54,
`sx/sy` +0x58, `anti_unit` +0x64, `offx/offy`, `army` +0x70, `iroquois` +0x74, `worker` +0x78, `no_danger` +0x7c,
`limit` +0x80, `saving` +0x84, `avoid_land/avoid_sea` +0x88/+0x8c, `valid_hit` +0x90, `scouting` +0x94,
`can_transport` +0x98, road parameters +0xa0…+0xc0). `PathFinder::walk_data` saves `[+0x58, +0xc4)`; no checksum part
covers it. Searches always leave the trees empty (`kill_lists` @687ae0) except a time-sliced one, whose trees move to the
unit (§3.3).

Three levels, each a call to the same A* (`astar_path(stack, step, fast)`, §7) with a different step:

| Level | Entry | Step | Typical use |
|---|---|---|---|
| W | `find_wpath(stack, x, y, who, o)` @688fc0 | 0x300 | long moves; group moves (`Group::action_move_near`, `find_wpath_army` sets `army = 1`) |
| T | `find_tpath(stack, x, y, who, o)` @6897d0 | 0xc0 | from the unit to the next W waypoint, and blocked re-plans |
| U | `find_upath(stack, x, y, who, o, fast)` @682f30 | 0x30 | around units (only after a collision), `resolve_unit_collision` |

The 4-argument wrappers @688e10 / @688e60 / @688eb0 take the unit's own position. `find_upath` @688eb0 also sets
`limit = 500 / max(1, repaths[who]²)` (halved when `fast`); `find_upath_restore` @688f40 sets `saving = 1` and
`limit = 300 / max(1, repaths[who]²)`, and both clear `saving` afterwards.

### 6.1 `find_wpath` @688fc0

1. `pathing_unit`, `scouting = 0`, `sx/sy` = the unit's TCoord, `offx/offy = 0`.
2. Pop the goal. Off the map → `length = 0`, return −1. Same W cell as the unit, or a helicopter → push it back and
   return the length.
3. Ground and sea units (domain < 2 and not transport-capable), unless player flag 4 (fog branch, below):
   **march the goal toward the unit** while the goal's tregion differs from the unit's (ships additionally need a valid
   naval spot): step 0x180, or 0x30 when the Manhattan remainder is below 0x300; `a = find_angle(unit − goal)`,
   `goal += (sin(a, s), −cos(a, s))`. When both axes are within one step, or the march enters the unit's W cell, push the
   moved goal and return (no search).
4. Push the goal. If the W Manhattan distance is below 3, return the length (no search).
5. `scouting = 1` for explorer/EXPLORE_TO cases; push `{goalW·768 + 384, tol 0x180, 0}` and `{unitW·768 + 384, tol 0, 0}`;
   `army = 1` for soldiers/supply that are not workers, not attacking, not on half-water; `worker = 1` for workers.
6. `astar_path(stack, 0x300, 0)`. On 0: pop one entry and return −1 if it was a final entry, else 0. Reset `army`,
   `worker`, `iroquois`; `kill_lists`.

The player-flag-4 branch walks the goal with `was_seen` tests on the fog grid; it was not traced in full (❓).

### 6.2 `find_tpath` @6897d0

As 6.1 with: the setup is skipped when `saving != 0` (resumption); the march step is 96 and "close enough" is
`< 96` on both axes (returns 0 unless the goal is final), the other stop is `valid_tcoord`; T Manhattan distance < 2 →
push the goal with tolerance 0 and return; otherwise push the goal, then `{goalT·192 + 96, tol (goal.tol == 0x180 ?
0x180 : 0x60), 0}`, then the unit's T centre with tol 0; `astar_path(stack, 0xc0, 0)`; a result < 1 pops the top unless
it is final.

### 6.3 `find_upath` @682f30

1. `pathing_unit`, `scouting = 0`, `sx/sy` = the unit's TCoord, `offx/offy = 0`.
2. Unless `saving` (resume): pop the target `G`. Off the map (UCoord outside `16xs × 16ys`) → `length = 0`, return −1.
   Same UCoord as the unit → push `G` with tolerance 0, return the length.
3. **Pull-in** for ground and sea units that are not transport-capable: while `G`'s tregion differs from the unit's or
   `!valid_ucoord(G, key = (Gy/48)·16xs + Gx/48)`: if `|ux − Gx| < 24` and `|uy − Gy| < 24` → return 0 if `G` is not
   final, else stop pulling; otherwise `a = find_angle(x − Gx, y − Gy)`, `G += (sin(a, 24), −cos(a, 24))`; when `G`
   reaches the unit's UCoord, push `G` and return.
4. UCoord Manhattan distance < 2 → push `G` with tolerance 0, return.
5. Push `G`; push `{G_U·48 + 24, tol G.tol, 0}`; push `{unit_U·48 + 24, 0, 0}`.
6. `anti_unit = 1`, `r = astar_path(stack, 0x30, fast)`, `iroquois = anti_unit = 0`.
7. Failure (`r < 1`) and not saving: pop the top unless final; if the current order is a move whose `retry` is
   non-zero keep it, else `kill_current_order`. A time-sliced search returns −1 with the trees saved.
8. Success: **straightening** when the stack holds more than 3 entries (exact rule; `findUpathPost` in
   `re/tools/astar-ref.mjs`): pop `W0`; if it is exactly the unit's position and not final, pop the next instead; keep
   `W0`. If `W0.flags & 2` and more than 2 entries remain: `A = W0`, `B = pop`, `C = peek`; while `B.flags & 2`,
   `C.flags & 2` and at least 2 entries remain: keep `B` if `B.flags & 4`; else drop `B` and set `A = B` if
   `A − B == B − C` in both axes (collinear); else drop `B` keeping `A` if `|A.x − C.x| == 48` and `|A.y − C.y| == 48`
   (corner between diagonal neighbours); else keep `B`. A kept `B` sets `A = B`. Then `B = pop`, `C = peek`. Push the
   last `B` back, then the kept points in their original order (`W0` on top).
9. `kill_lists`, return the stack length.

## 7. `PathFinder::astar_path(stack, step, fast)` @683770

`re/tools/astar-ref.mjs` implements this section; grid predicates are injected.

### 7.1 Setup

- `u` = `pathing_unit`. `iroquois = u.unit_masks2 & 0x4000` (passes forests).
- `cap = 50`, `dirinc = 1`, `mult = 1`; step 0x30: `cap = 500`, `mult = max(1, (type.new_block_radius + 1) / 2)`,
  `W = 16·xs`, `dirinc = fast ? 2 : 1`; step 0xc0: `W = 4·xs` (tile width); step 0x300: `W = xs`. `stride = mult·step`.
- `no_danger = 0` when the unit's action is not ATTACK, `who < 8`, and its order is neither ATTACK_TO nor
  GROUP_ATTACK_TO; else 1.
- Offset `(ox, oy)` = the current order's `MoveOrder.off_x/off_y` if it is a move order, else (0, 0).
- **Resume** (`saving`): free the pathfinder's own trees, adopt the unit's (§3.3), restore `valid_hit`, `dir0 =
  offset`, goal `= (endx, endy)`, `tol`, `avoid_land/sea`, previous expansions `T0 = traversed`, `start_dist`; go to 7.3.
- **Fresh**: `valid_hit = 0`; pop the **start** `S` (top), then the **goal** `G`; `tol` = tolerance of the entry now on top
  (the original target), or 0 if the stack is empty. Start node: `x, y = S`, `length = timeout = 0`, `parent = null`,
  `metric` = cell index (`x/768 + (y/768)·xs`, `x/192 + (y/192)·W`, or `x/48 + (y/48)·16xs`),
  `estimate = value = H(S)`; insert into the open list.

**Heuristic** `H(p) = vector_dist(|p.x − G.x|, |p.y − G.y|) · 10` at step 0x30, else `· 60 / step` (signed). With
`calc_cost` at 32 per orthogonal and 40 per diagonal step (before penalties), the UCoord heuristic (480 per cell)
dominates the cost, so the UCoord search is effectively greedy best-first; at T and W level the heuristic is 60 per cell
against a base cost of 32–40, an inadmissible (weighted) A*. Paths are therefore not shortest paths; reproduce the
formulas, not the intent.

**Avoid flags.** If start and goal are in the same region (W level: `WData.region` +4 equal; else `get_tregion` of both
TCoords equal): `water = start is ocean` (W: `is_ocean(wx, wy)`; else `TData & 0x30 == 0x20`); unless the unit is
transport-capable with `unit_masks & 0x40000` (both 0): water → `avoid_sea = 0, avoid_land = 1`; land → `avoid_sea = 1`
(2 if attacking), `avoid_land = 0`. Different regions: both 0, and `avoid_land = 1` if the order has flag 0x20.

`start_dist = |S.x − G.x| + |S.y − G.y|`. **Start direction** (fixed for the whole search):
`|S.y − G.y| < |S.x − G.x|` → `dir0 = G.x < S.x ? 7 : 3`; else `dir0 = S.y ≤ G.y ? 5 : 1`.

### 7.2 Neighbours

`move_x/move_y` @adcaf0/@adc400 (the first 9 entries of the compass spiral):

| d | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| dx | −1 | 0 | +1 | +1 | +1 | 0 | −1 | −1 |
| dy | −1 | −1 | −1 | 0 | +1 | +1 | +1 | 0 |

Odd `d` are diagonals. Expansion visits `d = dir0+1, dir0+1+dirinc, …` (8 or 4 directions, wrapping 9→1): the first
one is the orthogonal step toward the goal, then clockwise. Fast mode (`dirinc = 2`) is 4-connected.

### 7.3 Main loop

```
goal_tol = tol/2 + stride
while open not empty:
    n = first_open_node()                      // removes the lowest value; newest first among equal values
    if manhattan(n, G) <= goal_tol  or  T0 + E >= cap*64  or  (anti_unit and limit < E):
        → termination (7.5)
    for each direction d (7.2):
        p = new node; p.x = n.x + dx*stride; p.y = n.y + dy*stride
        p.metric = n.metric + dx + dy*W                    // NOT scaled by mult
        validity:
          0x300: valid_wcoord(n.timeout < 2 ? p + (ox,oy) − 0x180 : p, n.timeout, G)
          0xc0:  !invalid_loc(p/192, 0,1,1,1,0)
          0x30:  mult < 2 or d even: valid_ucoord(p, p.metric)
                 else for k = 1..mult: valid_ucoord(n + k*step*(dx,dy), key = n.metric + k*step*dx + k*step*dy*W)
        invalid → discard p
        E += (anti_unit and step == 0xc0) ? 5 : 1
        c = calc_cost(n.x, n.y, p.x, p.y, d, step, n.timeout + 1, &p.transport)
        c == 0x7fffffff → discard
        if p == G exactly: c = c/2
        p.length = n.length + c
        if closed contains p.metric → discard
        if open contains q with q.metric == p.metric: if q.length <= p.length → discard; else remove q
        p.estimate = H(p); p.value = p.length + p.estimate; p.timeout = n.timeout + 1; p.parent = n
        if step == 0xc0 and H(p)/32 + p.timeout > 120 → discard
        insert p into open
    insert n into closed (by metric)
open empty → failure (7.6)
```

`E` counts expansions of this call; `T0` is the count carried over from earlier slices of a resumed search. The large-unit
diagonal check uses a cache key that mixes world units into the cell index (`k·step` instead of `k`); it can alias
another cell's cached answer and must be reproduced exactly.

### 7.4 Containers and tie-breaking

- **Open list** `Tree<PathNode*, int>`: an unbalanced binary search tree keyed by `value` (signed);
  `ordered_insert` @4796f0 goes right when `key > node.key` and left otherwise, so a new node lands, in order,
  **before every existing node with the same value**. `first_open_node` @687970 removes the leftmost node, and
  `remove_current` @479770 preserves in-order position. Model: a priority queue ordered by
  `(value ascending, insertion sequence descending)` — lowest f first, **most recent first on ties**.
- `openlistrefs` (BRTree metric → tree node), `closedlist` (BRTree metric → node), `validlist` (BRTree key → bool):
  red-black trees with unsigned keys, unique keys, lazy deletion (a `removed` byte); a re-insert of an existing key
  overwrites it. Behaviour depends only on the key sets: model them as maps.
- No ordering depends on pointer values. Node recycling (LIFO pools, zeroed nodes) has no effect on results.

### 7.5 Termination and path emission

On the termination test:

1. **Time slice**: `anti_unit` and `limit < E`, still farther than `goal_tol`, and not `fast` → re-insert `n`, set
   `saving = 1`, move the trees and `tol`, `dir0`, `start_dist`, `T0 + E`, `G`, `avoid_*`, `valid_hit` into the unit
   (§3.3), give the pathfinder fresh trees, return −1.
2. `saving = 0`. **Node cap** reached (`T0 + E ≥ cap·64`, i.e. 32000 at U level, 3200 at T and W level) and not
   `fast`:
   - step 0xc0 with `anti_unit`: return 0.
   - step 0x30: if the current order is a move, `retry = rnd(3) + 6` (draw after `update_order`, before
     `get_move_order`); `u.safe (+0xb2) += 30` (char, wraps); return 0.
   - otherwise (T level without `anti_unit`, W level): **closest-node fallback** — pop every remaining open node in open
     order; the best is replaced only by a node with a strictly smaller `vector_dist` to `G` (start: `n`). If
     `needs_transport(best tile, G tile)` and the unit can transport (`unit_masks & 0x800000` and not
     `unit_masks2 & 0x2000`, or unit flag 0x10), set flag 4 on the stack top. Emit from `best` with `reached = 1`.
   - else (`fast`, or goal reached, or `limit` hit in fast mode): emit from `n` with `reached = 0` (a fast search that
     runs out emits a partial path to the best node).
3. **Emission**: if the emit node has no parent, return 1 (start = goal).
   - Start at the emit node `e`, except at W level when `!reached` and `!e.transport`: start at `e.parent` (the goal cell
     itself is already on the stack).
   - T level: walk from `e` to the start's child and mark nodes whose tile has `TData & 0x4000` and `(TData & 3) == 3`
     (building footprint) with `building = 1`.
   - For each node from there toward the start (**the start node itself is emitted only at U level**):
     position `(x, y)`; W level: `tol = 0x180`, and with a move order `(x, y) += (ox, oy) − 0x180`; U level: `tol = 0`;
     T level: with a move order `(x, y) += (ox % 0xc0, oy % 0xc0) − 0x60`, `tol = 0` when not `anti_unit` and the unit
     can transport, else 0x60. `flags = 2` if `anti_unit` or step 0x30, else 0; `| 0x10` if the node is a building node.
     If `(x, y)` differs from the stack top: a transport node gets `flags |= 4`, `tol = 0`; push.
   - Return 1.

The W-level offset keeps each unit's position **inside** its W cell equal to its target's offset (`off_x/off_y`,
§3.1), so a group's members keep their relative layout at W level; at T level the same offset modulo 192 recentres the
waypoint within the tile.

### 7.6 Failure (open list exhausted)

`saving = 0`. At step 0x30: if the current order is a move with `attempts ≤ 12`, `retry = rnd(3) + 6`; `u.safe += 30`.
Return 0.

## 8. Costs and grid predicates

### 8.1 `calc_cost(x1, y1, x2, y2, d, step, depth, &transport)` @684e50

Returns `((base·32) >> 8, rounded toward zero) + extra + (d & 1)·8`, or 0x7fffffff (blocked). `transport = 0`,
`extra = 0`, `base = 0x100`; `P` = the unit's player; cell indices are of the **destination** `(x2, y2)` unless noted.

- **0x30**: only the transport section below (so 32/40 per step plus transport penalties).
- **0xc0**: `m = TData`; `base = m & 0x2000 ? 0x400 : 0x100`; `k = no_danger ? 0 : danger[P][region]` (World +0x13c,
  regions of 1536 units: `(y2/1536)·reg_xs + x2/1536`); `+4000` if the tile is a building footprint (`(m & 3) == 3`
  and `m & 0x4000`; `building_danger` @685900); `who = WData.who`: `−4` if own, `+4` if enemy; `extra = max(k, 0)`;
  then the shared terms.
- **0x300, cell not `was_really_seen(x2/384, y2/384, P)`**: `base = 0x124` (`0x2480` if `army` and `WData.flags &
  0x200`); `scouting` → `base = 8`, `extra = 0`, else `extra = 8`; then team style.
- **0x300, seen**: `k = no_danger ? 0 : danger/8`; ocean: `+200` if `avoid_sea`; land: own `−4` / enemy `+4`, `+200` if
  `avoid_land`; `v = iroquois ? WData.solid : WData.blocked`; `k += 20v`, `+100000` if `v > 12`; `base = scouting ?
  0x400 : 0x100`; `army`: `+10000` if `v > 4`, `base <<= 5` if `WData.flags & 0x200`; `extra = max(k, 0)`;
  **corridor test** when `depth < 10` and (`v != 0` or `iroquois`): with `(tx, ty)` the **source** tile and
  `B(x, y) = is_blocked_at(x, y, iroquois)`, blocked (0x7fffffff) when, for the step direction:
  d1 `B(tx−2,ty−2) || B(tx−3,ty−3)`; d2 all of `B(tx−2..tx+1, ty−3)`; d3 `B(tx+1,ty−2) || B(tx+2,ty−3)`;
  d4 all of `B(tx+2, ty−2..ty+1)`; d5 `B(tx+1,ty+1) || B(tx+2,ty+2)`; d6 all of `B(tx−2..tx+1, ty+2)`;
  d7 `B(tx−2,ty+1) || B(tx−3,ty+2)`; d8 all of `B(tx−3, ty−2..ty+1)`.
- **Shared (0xc0 and seen 0x300)**: fleeing → `extra *= 3`; rush rules (`r = game.info.rush_rules ≠ 0`,
  `current_age() < r`, and `r < 9` or `frame < rush_rules[r].+0x3c·900`) and `who ≥ 0` not allied → `+500`.
- **Team style (0x300 with `army` or `worker`)**: `ts ∈ {0, 8, 11}` and at peace with `who` → `+5000`; `ts == 2` and
  `who` is neither the player's target nor itself → `+5000`.
- **Transport (all steps)**: `nt = needs_transport(src tile, dst tile)` (0 same medium, 2 land→water, 1 water→land);
  `C` = transport-capable. If `nt > 0`, `C`, `depth > 1`: blocked if `avoid_sea == 2`; if `nt != 1`, `extra += (avoid_sea ||
  avoid_land) ? 2000 : 500` (`<< 2` at step 0x30); `transport = 1`. Else at `depth == 1` the same test from the unit's
  start tile with `+1000`/`+250` (no shift) and `transport = 1` when `C`. If the governing `nt ≤ 0`, W level and
  `WData.flags & 0x100` (half water) → `base *= 3`.
- No `game_random` in `calc_cost`.

### 8.2 Validity

- `valid_ucoord(x, y, key)` @687c80: inside `tiles·192` bounds; cache hit in `validlist` → `valid_hit++`, cached
  answer; else `!invalid_loc(x/192, y/192, 0,1,0,1,0) && !detect_unit_collision(x, y, 1,1,_,1,0)` (the pathing variant
  of §10.2, using the search's `blocklist` snapshot of collision blocks), cached under `key`.
- `valid_tcoord` @687d60 = `!invalid_loc(t, 0,1,1,1,0)`.
- `valid_wcoord(x, y, depth, G)` @687da0: false at `(avoid_x, avoid_y)`; `r = invalid_loc(x/192, y/192, 1, depth > 1,
  0, 1, 0)`; `r == 3` is accepted inside the goal's W cell; valid iff `r == 0`.
- `UnitData::invalid_loc(tx, ty, wlevel, fog, bldg, transp, p7)` @607c30 (0 = passable): the top path entry's flag 4
  forces `transp = 1`; off the tile grid → 1; with `fog` and player flag 4, a tile whose four 384-unit fog cells are all
  unexplored → 0; then by domain — **sea**: water tile → 3 if `TData & 0x2400` for attacking ships; land tile → 2 unless
  (`transp || p7`), transport-capable and the W cell lacks flags 0x70; **land**: 2 if the W cell has 0x70 (except forest
  0x20 for `unit_masks2 & 0x4000`) with `wlevel && transp`; tree (`TData & 0x30 == 0x30`, except iroquois), mountain
  (`(TData & 3) == 2`) or cliff → 2; water → 2 unless (`transp || p7`) and `unit_masks & 0x800000`; **air**: 0. Tail:
  `wlevel` → 0; not `TData & 0x4000` (or iroquois tree, or the unit's own tile has 0x4000) → 0; attacking unit with
  `bldg` on a built tile → 4 or 0 by owner (`objects+0x200 == P`, ❓); else 4.
- `needs_transport(tx1, ty1, tx2, ty2)` @609920: 0 if same tile or same medium; 2 if the source is land, 1 if water.
- `is_blocked_at(x, y, iroq)` @461340: `TData & 0x4000` (iroquois: and not a tree).

Grid bits used (to be reconciled with docs/world-spec.md): TData `0x30` medium (0x20 water, 0x30 tree, 0x10 road),
`& 3` (2 mountain, 3 + 0x4000 building), `0x4000` blocked, `0x2000` near-blocked (T-level cost ×4, naval "shallow"),
`0x400` coastal, `0x800` river crossing; WData `who` +0xf, `blocked` +0x11, `solid` +0x13, `region` +4, `region2` +6,
`block` +0x18, flags `0x70` impassable at W level, `0x100` half water, `0x200` near impassable (army penalty).

## 9. Speed, turning and guys

### 9.1 Speed (world units per simulation frame)

Rules `MOVES` (`UnitTypeData.moves` +0x2c0) is already in world units per frame (`C.unit_move_speed` = 1): a citizen with
MOVES 25 covers 25 units (≈ 0.13 tile) per frame.

- **`Unit::update_speed`** @6055c0 computes the cached `myspeed` (short +0x9a). It runs only on `Unit::init`,
  `set_type`, `Leader::calc_unit_stats` (tech/bonus changes) and two spells — not per frame. On the captain (`o_up < 0`
  head of the chain): `base = moves`; sea units with unit flag 0x10 `+= leader transport level (XOR 0x63187 field)
  · military_transport_bonus`; `unit_masks2 & 0x200` `+= same · americans_marine_speed_bonus`;
  `s = unit_move_speed · base`; objmask 0x2000 with a whales leader bit → `s = (whales_ships_move + 100)·s/100`; first
  matching type (non-strict `is`): 104 `s·36/32`, 102 `s·34/32`, 100 `s·32/27`, 98 `s·30/24`; Bantu (tribe bonus 3,
  where 0x1ab or types 50/51 or militia 66) `(bantu_units_move + 100)·s/100`; siege where 0x1ae/0x1af: French (tribe
  bonus 10) and Versailles (wonder 0x218) `(x + 100)·s/100` each; air with the aluminum bit
  `(aluminum_air_speed + 100)·s/100`; spies, heroes and supply `+= (upgrade·s)/4`. The result is copied down the
  `o_down` chain.
- **`UnitData::speed`** @60aae0: non-ground → `myspeed`. Ground: Iroquois spearmen (`is(166)`) in allied territory add
  `iroquois_spear_speed[strict type 166..169]`; forced march (leader flag 0x8000 and a forced-march hero on the unit or
  within `(x_size + y_size)·0x60`): `v = forced_march·unit_move_speed` (Alexander/Napoleon types 358/359:
  `forced_march·alexander_forced_march·unit_move_speed/256`), `s = max(v, myspeed)` (this replaces the Iroquois
  value); then Conquer-the-World hero multipliers (Spitamenes /256, Blücher /100, Porus /256, Charles /100, Napoleon on
  siege /100, in that order).
- **`UnitData::get_speed(x, y, nocap)`** @608720 (vtable +0x17c; @6086f0 calls it at the unit's position):
  `s = speed()`; action ATTACK → `s·9/8`; action GUARD → `s·10/8` if `unit_masks & 0x40000` else `s·9/8`; ground units:
  `unit_masks & 0x10` → `s/2`; `z ≤ 0` on a tile with `TData & 0x800` (ford/river crossing) → `s/2`; general 0x162
  on a siege-like type → `s·2`; **group cap**: if `nocap == 0`, the unit is in a group, has no action, and
  `0 < group.speed (+0x40) < s` → `s = group.speed`; return `max(s, 3)`.
  There is **no terrain cost on speed** (no road, forest, hill or damage modifier).
- Per frame: `do_move` uses `get_speed(x, y, 0)` (× `ai_speed` when > 1; modern infantry `·5/4`), `move_step` may
  halve it (§5), `do_group_move` uses its own rule (§11.3).

### 9.2 Facing and turning

- **`Unit::set_angle(a, now)`** @605400: if the change is more than 90° (`0x3fffffff < a − old < 0xc0000001`, u32),
  toggle `unit_masks ^= 2` (mirrors the guy grid, below) and, for its group's leader, the group's `facing` byte (+0x48);
  `angle = a`; `Guy::set_angle(guy0, a, now)`.
- **`GuyData::turn_speed(full)`** @5de340: squad guys (`guy_num < squad_size`):
  `base = (type.turn_speed >> 8)·unit_turn_speed` (u32; `unit_turn_speed` = 256, so the low byte is cleared), ×
  `unit_pack_turn_bonus` (2) when packed (`unit_masks & 0x80000`). Crew guys: 0x40000000 if they have a track, else
  `base = 0x40000000`. `last_speed == 0 && guy_flags & 0x10` → 0x80000000 (instant). `full` → `base`; else
  `max(0xb60b·unit_turn_speed (1°), base / (avg_speed/4 + 1))` (u32) — turning slows as the guy moves faster.
  `TURN_SPEED` is degrees per frame (`degrees_to_angle`; 45 → 0x20000000).
- **`Guy::turn_towards(target, _, anim)`** @5d9720: `d = angle_diff`; `< 0x2222220` (3°) → snap; `> turn_speed(1)` →
  step by the rate (clockwise when `target − angle ≤ 0x80000000`); else snap; `do_turn`; returns the remainder.
- **`Guy::do_turn(target, new, _, anim)`** @5d97a0: if `new != angle`: `guy_flags |= 2`; with `anim` and
  `guy_flags & 8`, `set_anim(TURN_RIGHT 22 / TURN_LEFT 21)`; `angle = new`, `Guy::set_angle(new, 0)`, restore
  `des_angle`; guy 0 recurses into crew guys without a track.
- **`Guy::set_angle(a, now)`** @5d9010: `des_angle = a` (and `angle = last_angle = a` if `now`); guy 0 retargets crew
  guys (`index ≥ squad_size`) to `lead + (cos(a, tdx) − sin(a, tdy), sin(a, tdx) + cos(a, tdy))` computed as
  `sin(a, tdx)`, `sin(a + 90°, tdx)`, `sin(a + 90°, tdy)`, `sin(a + 180°, tdy)` and clamped to the map when a track is
  non-zero; `now` teleports them.

### 9.3 Guys

A unit owns `squad_size + crew_size` guys (UnitType +0x304/+0x30c); `guy_num` = index. `guy_mark` (+0xb5, initially
`squad_size`) counts the living squad guys. Guys `≥ squad_size` are crew/trailing figures whose `track_dx/dy` come from
**render data** (`Guy::update_gpiece` @5d8530: `trunc(a.x·4.8f·gp.scale)`, single-precision floats from the graphics
piece) — simulation state derived from art files.

**Guy slots** (`Unit::set_new_location`, every time the unit moves): `n = guy_mark`,
`sp = guy_spacing·(form == 5 ? 2 : 1)`, `A = sin(angle − 0x40000000, sp)`, `B = sin(angle, sp)` (0 if `sp == 0`);
`cols = form == 8 ? (n ≠ 1) + 1 : min(n, 3)`, `R = (n − 1)/cols`; if `unit_masks & 2`: `A = −A`, `B = −B`;
`ox = (R·B)/2 − ((cols − 1)·A)/2`, `oy = (R·A)/2 + ((cols − 1)·B)/2`; guy `i` (`row = i/cols`, `col = i % cols`):
`des = (x + col·A − row·B + ox, y − col·B − row·A + oy)`, clamped to `[0, xs·768 − 1]` (upper bound first). With
`n == 1` guy 0's destination is the unit position. `p4` also calls `Guy::set_angle(unit.angle, p3)`; `p3` teleports.

**`Unit::set_new_location(x, y, p3, p4)`** @5f8d20, in order: W-cell change `wc` (unit on the map) and tile change `tc`;
on a tile-only change with tribe bonus 0x12 (ground): `unit_masks` 0x800 = "on a tree tile"; amphibious units
(transport-capable): a ground unit entering water gets a cast order (spell 0x28a, the boat transform) and the function
**returns 0 without moving**, a boat leaving water ejects/dies; `wc` → `remove_from_world` (§10.1); store x, y and
`z = find_tcoord_z(tx, ty, 1)` (XOR-obfuscated); `wc` → `add_to_world`, ground units in unowned territory
`explore_goody`; `tc` with a new 384-unit fog cell → `update_seen(p3 == 0)` (vtable +0x174) and `update_ceo_position`
when `unit_masks2 & 0x10000`; then the guy slots above.

**`Guy::process`** @5e0230 (after `Unit::work`, §2): `Guy::move`; clear `guy_flags` bit 2; turrets (flag 0x100) step
15° (0xaaaaaaa) toward their targets, setting `node_flags` bits on arrival; every 64 frames (`(frame + o) % 64 == 0`)
with `avg_speed == 0`, non-air squad guys re-stamp their collision footprint (§10.1) and clear `guy_flags` 0x20.

**`Guy::move`** @5d9240: `last_x/y/z = x/y/z` (z unless flag 0x40), `last_angle = angle`.
- At `des`: `last_speed = 0`; facing done → idle animation handling (`set_anim(DEFAULT)` one frame after the walk
  stops), `stopped = 1`; facing not done → turn in place (`turn_towards(des_angle, 1)` unless `guy_flags & 2`).
- Not at `des`: walk/attack-walk/carry animation; **squad guys** (and crew without a track) **snap**:
  `last_speed = vector_dist(des − pos)`, `Guy::set_new_location(des)`; **crew with a track** turn first
  (`rem = turn_towards(find_angle(des − pos), 0)`, return without the `avg_speed` update if `rem > 2·turn_speed(1)`),
  then step `last_speed = get_speed·11/8` (× `ai_speed`) along their own heading with the `move_step` clamps (snap if
  within reach), `x −= off_x`, `y −= off_y` (❓ `off_x/off_y` only written by `Unit::init`), and
  `set_new_location` if `WorldData::is_valid`. `stopped = 0`.
- `avg_speed = (avg_speed·3 + last_speed)/4`.

**`Guy::set_new_location(x, y, now)`** @5d86f0: non-air squad guys whose UCoord changes call
`CollCheck::move_unit(old U, new U, new_block_radius)` (§10.1); store x, y; z: sea 0, ground `find_data_z(x, y, 0)`,
helicopter `z += clamp(terrain − z + 1000, −30, 30)`, other air unchanged; `now` → `last = current`; guy 0 retargets the
crew. `Guy::update_z` @5d9950 (only from `Unit::come_out`) and `Unit::update_z` @606590 (from `add_flock`,
`Wall::init`) re-read terrain z.

**Terrain z is float.** `find_tcoord_z = trunc((H[tx, ty+1] + H[tx+1, ty])·0.5f)`; `find_data_z` interpolates the
vertex grid (TerrainOut +0x4a4, `4xs+1` per row) on one of two triangles with `k = 1/192` (0x3baaaaab) in single
precision and truncates (`cvttss2si`); with flag 1 water or negative results become 0; without a height grid
(TerrainOut +0x498 == 0) it returns `(int)land_height`. z is walked (unit z, guy z), so the terrain height grid and these
float steps are simulation-relevant (terrain-spec.md: `TerrainOut::find_data_z`).

**Simulation vs render.** The walked 0x9b bytes (+0x08…+0xa2) are simulation; +0xa4 on (`last_norm`, bbox, river
cache) and the `GuyData::get_render_*` getters (x87 interpolation by `graphic_percent`) are render only. `last_x/y/z`
and `last_angle` exist for that interpolation but are walked and checksummed.

**Animation timing is simulation state.** `end_time` comes from the animation manager's frame counts (`.bha` assets;
3 if missing) and walk-cycle switches rescale `cur_time = cur_time·len_old/len_new` (u32). The walk variant is chosen
with a **float** ratio `(float)avg_speed / (float)(moves·unit_move_speed)` (`divss`): `< 0.6f` SLOG, `> 1.1f` JOG,
else WALK. A bit-exact simulation therefore needs the shipped animation lengths.

## 10. Collision and avoidance

### 10.1 Collision blocks

- Each W cell may own a **CollBlock** (`WData.block` +0x18: null, a block, or 0xFFFFFFFF = never): a `BitMask<768>`
  of which 256 bits are used, one per UCoord of the cell (16×16). Bit index `= (ux mod 16)·16 + (uy mod 16)` (C
  remainder), byte `idx >> 3`, bit `1 << (idx & 7)`. `flags`: 1 = known empty, 0 = set since the last test, 2 = dirty.
  `set(on)` sets the bit and `flags = 0`; `set(off)` clears it and turns 0 into 2. `BitMask<768>::empty` @479150:
  `flags & 1` → empty; `flags == 0` → non-empty; else OR all 96 bytes and write `flags = 0/1` (a read with a side effect).
  `World::new_coll_block` @46d250 allocates a zeroed block with `flags = 1`. Blocks are walked by `World::walk_data`
  part 9 (`flags = 2` after load).
- `WorldData::get_coll_block(wx, wy, region)` @6b5350: −1 if `region ≥ 0` and the cell's region differs, else the block.
- **Garbage collection** `GameDaemon::process_coll_blocks` @731f90, once per frame: `n = max(5, xs/4)` cells from the
  cursor `empty_colls` (row-major, wrapping at `xs·ys`); a block with `flags == 0` is kept; `flags & 1` → freed, set null;
  else OR the bytes: non-zero → `flags = 0`, kept; zero → freed. This matters: `move_unit`'s fast path writes nothing
  into a null block.
- **Footprint**: a `(2r+1)²` square of UCoords around the guy's UCoord, `r = type.new_block_radius` (+0x248, stock 0..7),
  enumerated in the compass spiral order (`move_x/move_y`, first `radius[r] = (2r+1)²` entries, table @add1e0). The
  spiral table has two typos at indices 288 and 360 (reachable only with r ≥ 8); copy it verbatim. Writes skip
  off-map UCoords and cells whose region differs from the tile's `get_tregion`, and cells with block −1; setting a bit
  creates the block if null, clearing needs an existing block. **No reference counting**: overlapping footprints share
  bits.
- Writers are **guys** (squad guys only, `guy_num < squad_size`, non-air): `Guy::set_new_location` →
  `CollCheck::move_unit(old, new, r)` @682ad0 (r == 0 → nothing). Fast path when `|Δx| + 2r < 16`, `|Δy| + 2r < 16`, both
  boxes in one valid W cell and its block (for the old tile's tregion) is a real block: update in place; **if that block
  is null, nothing is written**. Slow path: clear the old square's bits not within Chebyshev `r` of the new centre
  (old tile's tregion), then set the new square's bits not within `r` of the old centre (new tile's tregion).
  `Object::add_to_world` @64d8c0 / `remove_from_world` @647970 (from `Unit::set_new_location` on a W-cell change,
  before the guys move) stamp/clear the whole footprint of every guy `0..guy_mark−1` and link/unlink the unit in the
  cell's object list (insertion at the head). `Guy::process` re-stamps idle guys every 64 frames.
- Timing: a unit's footprint is current at the end of its own `Unit::process` (guys move after `work`).

### 10.2 `CollCheck::collide_here(o, who, ux, uy, r, &hx, &hy, pathing)` @682540

`r == 0` → 0. `fill_slots` @6820e0 loads up to four blocks (the cells of the box corners `(ux ± r, uy ± r) >> 4`); a slot
is ignored unless its region matches the centre's `get_tregion`-style region. With `pathing` and a search `blocklist`,
blocks are read from / copied into that snapshot (keyed `wy·xs + wx`), so a search sees a consistent snapshot.
All slots empty → 0. **Fast path** (`pathing == 0`, the probe is one UCoord orthogonally from the unit's current UCoord):
test the leading edge, `i = 0..2r`, even `i` only, advancing on odd `i` and on even `i` only when a test ran and was
clear (an empty-slot even step does not advance; asm-verified quirk). **General path**: spiral entries `0..radius[r]−1`
where `move_x[i] + r` and `move_y[i] + r` are both even (a 2-UCoord lattice, edges included), skipping cells within
Chebyshev `r` of the unit's own UCoord; the first set bit is the hit `(hx, hy)`.

### 10.3 `Unit::detect_unit_collision(x, y, test_only, allow_push, _, pathing, no_push)` @617060

"Reset" = `collide_who = collide_o = −1`, `collide = 0` if `collide_frame < frame − 5`, `unit_masks &= ~0x40`,
return 0 (only when `test_only == 0`; with `test_only` it just returns 0).

1. Air units: reset.
2. Pushers (sea units, siege 0x20000, heroes, supply), unless `no_push` or `pathing`: return 0 if `test_only` or not
   `allow_push`; `detect_boat_collision(x, y, 1)` returns 1 → return 0 (everything was pushed); else continue.
3. Reset if the top waypoint has flag 8 (a `go_around_building` midpoint) or `safe` (+0xb2) ≠ 0 (set by failed searches,
   §7.5; counts down in `Unit::work`).
4. `U = (x/48, y/48)`; the unit's own UCoord, or `collide_here(…, pathing) == 0` → reset.
5. `corner = will_be_corner(h, U)` @609fa0 (diagonal corner code 1/3/5/7 when `|h − U| == r` on both axes).
6. `test_only` → return 1.
7. Owner search: the 9 W cells around `(x/768, y/768)` in spiral order, each object list from its head; skip self,
   non-units and air; the owner must `is_here(h)` (its own footprint covers `h`). It is **ignored** (soft, keep
   scanning) when: both are trading and moving; it guards me; both attack, same player, both radius 1, both moving,
   and my target is more than 0x300 beyond range; same group, I am not attacking, it has no saved search and it is
   idle / guarding / moving (not attacking) / casting certain spells. It is passed over without effect when the corners
   just touch (`|corner − is_corner(other, h)| == 4`). Otherwise: `collide_o/who/guy = (o, who, 0)`; with `pathing`
   return 1; a move order records `coll_x/coll_y = (x, y)`; return 1.
8. No owner found: if any soft owner, `unit_masks |= 0x100000` (move_step halves the next step); reset.

### 10.4 `Unit::resolve_unit_collision(x, y)` @5f9d30 (from `move_step`)

With `B` = the collider:
1. Combat cases: units that die when attacking (`n`) hit a valid target; a foreign `B` that is my target, in range, or an
   enemy → attack/retarget (`add_attack_order` QUEUE_FIRST etc.); gather on a wall that covers me → stop.
2. **Sidestep**: `B` is moving (order in MOVE_TO, ATTACK_TO, EXPLORE_TO, FLEE_TO, CHANGE_FORM, GROUP_MOVE,
   GROUP_ATTACK_TO), I am a land unit and the top waypoint lacks flag 2: `c` = UCoord of `coll_x/coll_y`,
   `(dx, dy) = c − myU`; diagonal (`|dx| == |dy|`): try `(cx, cy − dy)` then `(cx − dx, cy)`; else `(cx − dy, cy − dx)`
   then `(cx + dy, cy + dx)`; the first candidate with a valid tile and no collision (`detect(·, 1,1,0,0,0)`) is pushed
   as `{u·48 + 24, 0, flags 2}` and becomes `dest`; return.
3. `collide++`, `collide_frame = frame`.
4. **Wait**: `B`'s order is a move (not FLEE_TO) or the boat-transform cast: `lim = repaths[who] > 3 ? 0x80 : 0x20`; if
   both `collide` counters are below `lim`, `B` is not an enemy, `B` is not colliding with me, and `B` is not waiting
   (or waits on someone else): `unit_masks |= 0x40` (waiting), return — retried next frame.
5. **Repath** (path non-empty): `rp = repaths[who]`; if `rp ≥ 4` continue only when `(o + collide) & 3 == 0`, with
   `c = collide/4`, else `c = collide`; proceed only if `rp < 16` and (`rp < 8` or `(o + c) & 15 == 0`); `repaths[who]++`;
   pop waypoints until a final one, or one with `tol ≥ 0x60`, not flag 2 and not blocked (tile `& 0x6000`, else
   `collide_here(pathing = 1)`), push it back; `set_new_location(myU·48 + 24, snap)`;
   `find_upath(path, who, o, fast = my action and B's are both ATTACK)`; `dest = 0`; if a path was found, `B` collides
   with me and is not waiting: `update_order()`, `v = rnd(9)`, `order.pause = v + 1` (**the only RNG draw in
   collision**, @5fa86e).

### 10.5 Other pieces

- **`detect_boat_collision(x, y, mutual)`** @5fa8b0 (pushers): `who ≥ 8` or `push_size == 0` → 1. Candidates from
  `Objects::find_units(x, y, …, push_size, same tregion, not me, push_circles, sea)` @65a620 (cell-ordered or
  player/index-ordered depending on density). Each side is modelled as `push_circles` circles of radius
  `q = push_size/push_circles` laid along guy 0's heading; `d` = min `vector_dist` between centres;
  `ov = (q_other − d + q)/2`; for `ov > 0`: blocked (return 0) for armed others vs flag-0x10 pushers, non-allies,
  unpacked packables, entrenched units, tanks; else push the other by `min(ov, 48)` along `find_angle(other − me)`
  (clamped to ≥ 45° off my heading when it is idle; blocked if it is moving within ±45°), `set_new_location` if the tile
  is valid, mark `collide_frame`. `Unit::work` also runs it for idle pushers.
- **`resolve_block`** @5fccc0 (waypoint flag 0x10): own building → mark it; friendly foreign → diplomacy flag, 0; enemy
  → `add_attack_order` QUEUE_FIRST, 1.
- **`go_around_building(x, y, angle, step)`** @5fc350 (from `find_path`, tile level): probe along the blocked tile's row
  or column in both directions for the first valid tile (recursing with `step − 1` on a doubly blocked diagonal),
  choose the exit nearer `MoveOrder.dest` (Manhattan), require it within 0x300 on each axis, and push the exit
  (tile centre + `(off % 192)/2 − 48`), a flag-8 midpoint and the corner point. Failure: `dest = 0`, clear `unit_masks`
  bit 8, `path_recursion = 10`.
- `collide` returns to 0 only through a non-colliding test more than 5 frames after `collide_frame`, `Unit::do_idle`,
  or the saved-search abort in `do_move`.

## 11. Groups, formations and armies

### 11.1 From a command to orders

- A MOVE_TO command builds a temporary group in command order (`Group::add` @714350: non-captains are replaced by their
  captain, followers via `o_down` follow their captain; max 128) and `Groups::push_group` @70f9e0 stores it
  (`get_open_slot` @6fa460: slots `who·64 … who·64 + 45`, first empty/building slot other than `last_group[who]`,
  else the oldest single-member group).
- `Group::action_move_to` → **`action_move_near(x, y, tol, queue, set_angle, angle, orders, …, form, width, disembark)`**
  @704990: clamp the target to the map; split land/sea mixes into two groups (recursing); skip building groups;
  `QUEUE_FIRST` wraps a `QUEUE_NEW` call in `set_up_insert`/`finish_insert`; leader `L` = `GroupData::find_leader`
  @70ccb0 (lowest `type_cat`, first in list order); formation = command byte (9 or −1 → the members' common form, default
  0), width = `form_mod` (−1 → mean of members' `form_mod`, default 50); `QUEUE_NEW` clears members' orders;
  `compute_form` (§11.2); then per member (list order): set `unit.form/form_mod`, clamp the slot, relocate slots
  (members `i > 0`): a slot in another tregion than slot 0 goes to `find_nearby_spot` around slot 0 (radius 0, for
  the unit's type, the leader's barge or the carried unit's type, `group` temporarily −1), else a captain whose slot
  is invalid while slot 0 is valid (not AI-controlled) goes to a spot 0xc0..0x480 around slot 0, and the followers
  of a relocated captain to a spot `guy_spacing..4·guy_spacing` beside the captain's slot; then issue either a **GroupMoveOrder** (GROUP_ATTACK_TO for ATTACK_TO) when
  orders ∈ {MOVE_TO, ATTACK_TO}, not modern infantry, not a role-0x10 unit without `unit_masks & 0x40000`, `num ≥ 2`,
  not `unit_masks & 4`, not a ship (the unit's domain, not the target tile), and form ≠ 9 (MOB) — else an individual `add_move_facing_order`.
  GroupMoveOrder: `x, y = slot` snapped to its UCoord centre, `angle = (angles[i] << 24) + A`,
  `id = (group.id + frame·10)·100 + order_num`, `form_id = i`, `oxx/whose` = the leader, `off_x/off_y = x mod 768`.
- `update_positions(L)`; then the **shared path** in the static `grouppath`: start = top of `L.path` or the group
  location (`get_loc`, `get_loc_to` for QUEUE_LAST); push `{to[L], tol, 1}`; if `vector_dist(start − to[L]) ≥ 0x900`, `find_wpath(&grouppath, start, who, L)`
  (`find_wpath_army` sets `army = 1` for AI armies). Empty result: every member plans alone (`find_wpath` to its own
  slot, starting from the top of its own path or the **group location**, not its position). Otherwise each W waypoint
  `W` becomes, per member, `P = W + (to[i] − to[L])` (clamped; when the region differs and `P` is invalid: snapped
  into W's wcoord, or `P = W` when the leader was already within 0x900 of its slot); the leader and every member get
  final points, intermediate points go to the leader and to individual movers within 0x600 of it (computer players'
  ships other than the leader only get final points); COLUMN intermediate points read `to[cols[i]]` from a static
  array that is never written (uninitialised). At the end every member's path is inverted into stack order,
  `order_num++` and the FormData scratch is zeroed (`memset(F + 0x30, 0, 0xe60)`: the walked `forms` are zero past
  `density` between commands).
  These orders are created **pathed**, so `do_move` skips its own `find_wpath`.

### 11.2 Formation slots

`Group::compute_form` @707c80 / `Form::compute` @72e8e0 / `categorize` @72e250 / `compute_rows_and_columns` @72d910 /
`compute_dests` @72cba0 (all integer except one SSE term):

- Angle `A = find_angle(dest − loc)` (or the commanded angle, with a `flip` point reflection when it differs by 90–270°;
  dest == loc reuses the previous group angle when loc == `(ox, oy)`, else `L.angle − (angles[idx] << 24)`); the
  group `facing` byte is toggled around `Form::compute` when the leader's base angle is 90–270° from `A`, and
  passed as `reverse` (mirrors every lateral offset).
- `categorize`: `Group::sort` (followers made contiguous behind their captain; not a key sort); per captain the
  effective type (land units on water use the transport-barge type), category `type_cat` @72dfc0 (0 MECH, 2 MOUNTED,
  3 FOOT, 4 FOOT_RANGED, 5 mounted ranged, 6 ARTILLERY, 8 COMMAND, 10 CIVILIAN by `obj_masks`/flags/`max_range`),
  rank `cat_id` by list order, per-category spacing `xs = max(type.x_spacing·k)`, `ys = max(type.y_spacing·((uber−1)/k
  + 1))`, `k = min(uber_size, 3)` (2 for COLUMN); COMMAND units (second pass) join the first populated category after
  the most populous one, else the one right after it (SQUARE keeps them in COMMAND). The leader `F.idx` is the first
  captain with the lowest category over both passes.
- Rows/columns: width `W` = max over categories of `n·xs` (`n·xs/2` when `n ≥ 6`);
  `cols[c] = clamp(((W·form_mod)/50)/xs, 1, n)`, `rows = ceil(n/cols)` (COLUMN `(n + 2)/3`, `cols` untouched).
  WEDGE: the wedge category is the first populated of 3, 4, 2, 5, 0, 1, 6, 7 (else any); its rows are triangular
  (seeded from an uninitialised stack slot); the other categories get `cols = max(n/2, 10)`. SQUARE works around
  category 7 and sets `wedge = −1`, so `compute_dests` gives its captains no slot at all (they keep (0, 0)).
- Slot of a captain with rank `n` (`C = cols`, `q = n/C`, `r = n % C`): `lat = ((r + 1) >> 1)·xs`, negated when `r` is
  odd, `+ xs/2` when `C` is even (unless guarding), `+ xs/2` stagger on odd rows for categories ≤ 2, 6 and ≥ 12;
  `D = −q·ys` (the density factor is always 1: `compute_dests` takes the density only for a negative category, so
  SPARSE lays out like LINE); `dep = D`
  (REFUSED `D − |lat|`, ENVELOP `D + |lat|`, echelons `D ± lat`); COLUMN `lat = ((n + 1) % 3 − 1)·xs`,
  `dep = −(n/3)·ys`; WEDGE triangular rows (depth `−row·xs`); MOB a ring spiral (5 points on the first ring, +5 per
  ring, from angle 0x55555555). COLUMN and MOB leave `Group.angles[i]` unchanged. Categories stack front to back:
  `dep −= xs[wedge]·rows[wedge]` (WEDGE), `dep −= ys[k]/2` per present category after the first and
  `dep −= cvttss2si(((float)rows[k] − 0.5f)·(float)ys[k])` for each earlier one (the only float). Followers: the first
  sits `+guy_spacing` beside its captain, every later one `−guy_spacing` (overlapping beyond uber 3), with the
  REFUSED / ENVELOP / echelon depth rules from the captain's `off_x` (modern infantry gets an integer jitter from the
  destination and object index). World slot `to[i] = dest + (cos(A, lat) + sin(A, dep), sin(A, lat) − cos(A, dep))`,
  `off[i] = (lat, dep)`; then everything is recentred on rank 0 of the front category: `off −= (lat0, dep0)`, but the
  destinations only by the depth (`to += (sin(A, −dep0), −cos(A, −dep0))`; the lateral cos/sin terms are called with
  radius 0), and `Group.off_x/off_y = floor(off/48)`. Guarding groups shift `F.off_y` after `Group.off_*` is written.

### 11.3 Per frame

- **`Groups::process`** @6fa210 (in `GameDaemon::process_all`): for each live player, **one** slot per frame
  (`p·64 + proc_group`, `proc_group` cycles mod 64): `normalize` (drop dead/departed members, compacting the per-member
  arrays in lockstep), `find_role`, `speed = new_speed = UnitData::speed(L)`.
- **`Group::update_positions(L)`** @713810: `B = find_angle(dest − L.pos)` while the leader's move has `dest ≠ 0`, else
  `L.angle`; `curr[i] = rot(B, off_x·48, off_y·48)`.
- **`Unit::do_group_move`** @5e79a0:
  - leader: every 32 frames an attacking leader near its target (`< 0x900`) ends the group move; `do_move`; while
    moving: `group.speed (+0x40) = new_speed`, `new_speed (+0x3c) = get_speed(x, y, 1)`, `update_positions`; at the
    end: attack the collider (GROUP_ATTACK_TO) or `kill_group_move` + `distribute_attack`, else `ungroup_move_order`.
  - member: leader lost → `refresh_group_order` (≥ 0x600 away) or `ungroup_move_order`; slot `T = L.pos + curr[slot]`;
    `Wp` = its final waypoint; `ds = vector_dist(T − u)`, `dw = vector_dist(Wp − u)`; `dw − ds ≤ 0x60` or `dw ≤ 0x180`
    → ungroup (plain move to `Wp`). (`Wp`: the member pops its path down to the first final entry and pushes it back;
    an empty path reads a stale entry.) If the slot is ahead (my captain is `L`, or the angles to `T` and `Wp` differ by
    less than 120°): push `T`, `dest = T`, `in_group = 1`, normal speed. Else (I am ahead of my slot): `ds < 0x30` →
    wait; else, if the leader has no path or `in_group == 0`, head for the midpoint `(T + Wp)/2` at reduced speed;
    otherwise push the leader's top waypoint + my `curr` offset, reduced speed only when my facing is more than 120°
    off the slot direction, `in_group = 0` within 0x60 of it.
  - speed: normal = `g = get_speed(x, y, 1)`, `s = g + min(g/3, 9)` (catch-up; members also lower `group.speed` to
    their own `UnitData::speed`); reduced = `get_speed(x, y, 0)/2`. Net effect: the leader moves at the slowest
    member's speed with a one-frame lag that depends on processing order (§2).
  - **RNG**: when `dest` is invalid: if it is within 0x300 (Manhattan), its (clamped) W cell has WData flag 0x20
    (forest) and the unit is in a group: `v = get(0, 0xffff)`, `Objects::add_flock(wx·4 + 2, wy·4 + 2, whose,
    2 + (v & 1))`; in every case `ungroup_move_order`. (`add_flock` @65c0e0 returns at once for a leader whose last
    flock is less than 225 frames old, otherwise it draws `game_random` itself and creates gaia animals.)
  - then `move_step(order, s)`.
- `Unit::ungroup_move_order` @5fd140 converts GroupMove → MOVE_TO and GroupAttackTo → ATTACK_TO (copying the
  MoveOrder base, `dest = 0`, `orig = position`) for the captain chain; `Group::kill_group_move` @7123f0 kills orders
  with the group's id on every member.

### 11.4 Armies

`Armies::process_all` @6f3b00 (after `Groups::process`) runs `Army::process` @6f93d0 for AI players when the army's
status bit 0x80 is set or `(frame + 2·(army + 2·who)) & 255 == 0`. `Army::normalize` stable-sorts its groups by the
leader's unit category. `do_forming` and `march_to_target` issue `action_move_to(P, QUEUE_NEW, …, ATTACK_TO, …)` (or
siege variants) per group, stepping `P` 384 units back along `muster_angle + 0x80000000` per group.
`Army::find_target` uses `game_random` (not traced here).

## 12. Road, caravan and river searches

All three use the global pathfinder. The road searches run **inside the simulation** (`BuildType::place_roads` on every
building activation/capture/close, `Caravan::build_road` from trade code); the river search runs at map generation but
draws from the same `game_random` stream. Tables: `orthog_x/orthog_y` @add250/@add210 = N, E, S, W (index 1..4).

### 12.1 `find_road(stack, srcObj, srcPl, dstObj, dstPl, unit, unitPl)` @688a40

1. Endpoint tiles: a city → its centre tile `(pos/192)`; otherwise `WallData::tile_corner + (x_size, y_size) − 1`
   (the far footprint corner).
2. Unless resuming: off-map endpoints or equal tiles → `length = 0`, return −1; push goal, then start, as
   `{t·192 + 96, 96, 0}`.
3. `avoid_land = 0`; `avoid_sea = (WData.region of start == of goal)`; `can_transport` = unit given and
   transport-capable.
4. `astar_caravan_road(…, cara = unit ≥ 0 ? unit.cara : −1, unitPl)`; `kill_lists`; return −1 for a negative result,
   else the stack length.

`find_road_restore` @685950 (only `Caravan::build_road`) wraps it with `saving = 1`.

### 12.2 `astar_caravan_road` @685990

- `avoid_land = 0`, `iroquois = 0`. **Resume** when `cara ≥ 0` and the caravan's `making_road` is set: adopt the trees
  stored in the `Caravan` (+0x28/+0x2c/+0x30), `dir0 = offset` (+0x34), goal = `endx/endy` (+0x38/+0x3c), clear
  `making_road`.
- Fresh: pop start, pop goal; `orthoOnly = cara < 0` (building roads 4-connected, caravans 8-connected); start
  `metric = tw·ty + tx`, `length = 0`, `timeout = 1`, `z_val = (short)find_tcoord_z(tile)`,
  `estimate = value = vector_dist(|S − G|)·60/192`; `dir0`: `|dx| > |dy|` (dx = start − goal) → `(sx > gx)·4 + 3`, else
  `sy > gy ? 1 : 5`.
- Loop: pop `P`. `P == G` exactly → emit. If the expansion count `≥ 3200` → slice (below). Else for `k = 1..8`,
  `d = dir0 + k` (wrap): skip odd `d` when `orthoOnly`; `N = P + move[d]·192`, `N.metric = P.metric + dy·tw + dx`;
  discard unless `valid_roadcoord`; `count++`; `N.parent = P`; `c = calc_road_cost(N, srcPl, dstPl, d)` (**one RNG draw,
  before the closed/open checks**); `N == G` → `c /= 2`; `N.length = P.length + c`; discard if closed; open duplicate
  with `length ≤` → discard, else replace; heuristic `vector_dist(|N − G|)·60/384` for `cara < 0` (asm @686017: `SAR 6`; only the start node divides by 192), and for caravans
  `vector_dist(N.x − gx, −gy)·40/16` (**the y argument is `−gy`, not `N.y − gy`**: asm-verified original bug, keep it);
  `value = length + h`, `timeout = P.timeout + 1`, insert. Then close `P`. Empty open list → 0.
- Emission: from `G.parent` toward the start, excluding the start, push `{x, y, 96, 0}` for every node whose tile lacks
  `TData & 0x4000`. Return 1.
- Slice (3200 expansions): building roads (`cara < 0`) fail with −1; caravans re-insert `P`, store the trees, `dir0`
  and the goal in the caravan, set `making_road`, return −1 (each resume has a fresh 3200 budget; `traversed` is
  written but never read).

### 12.3 `calc_road_cost(N, srcPl, dstPl, d)` @686300

`td` = TData, `wd` = WData of the W cell. `r = rnd(20)`, `cost = r + road_base_val`, `rnd = r`.
- Water (`td & 0x30 == 0x20`): `+road_avoid_sea` if `avoid_sea`, else `+road_cross_coast` when the parent is on land;
  `N.z_val = 0`.
- Land: `wd.who < 0` → `+road_noone`; else `friendly = (owner is srcPl, or mutual alliance state 2) and owner allied with
  dstPl`; not friendly → `+road_enemy`. `(td & 0x6000) == 0x2000` and not a road → `+road_bad_path`. `wd.flags & 8` →
  `+2·road_river` (also added to `rnd`). `td & 0x800` → `+road_river` (and `rnd`). Height: `z = max(find_tcoord_z, 0)`,
  `N.z_val = z`, `m = min(|z − |P.z_val||, road_z_max)`, `+3m` (and `rnd`). Road tile (`td & 0x30 == 0x10`): friendly
  and `P.z_val < 0` → `cost = (cost − rnd)/2`; friendly otherwise → `+road_bad_path/8`; unfriendly and `P.z_val < 0` →
  `cost −= rnd`; unfriendly otherwise → `+4·road_bad_path`; a road tile also sets `N.z_val = −z` ("on road"). Non-road:
  friendly `+road_bad_path/8`, else `+4·road_bad_path`.
- Not `was_seen(tx/2, ty/2, srcPl)` → `cost ·= 2`. Diagonal `d` → `cost·7/5` (`road_diag_penalty` is never read).
- Defaults (`PathFinder::init` @689ec0; debug tunables, saved by `PathFinder::walk_data`): base 55, avoid_sea 100,
  cross_coast 100, enemy 540, noone 240, bad_path 200, river 60, z_max 600, diag_penalty 8.

`valid_roadcoord(x, y, px, py, srcObj, srcPl, dstObj, dstPl)` @688740: in bounds; water only with `can_transport`; a
tile with `TData & 0x4000` or occupancy (`(td & 3) == 3` or `td & 0x80`) is allowed only when it is a road without
0x4000, or lies in the source/destination city's footprint (`WallData::covers_tile`); a diagonal step needs one of the
two corner tiles valid.

### 12.4 `astar_river(sx, sy, out_x[], out_y[], windiness, min_len)` @686690 (map generation)

From `Map::make_riverlist` @693820 (← `Map::make_rivers` ← `Terrain::init`). W-cell grid, best-first on a per-step
score (not g + h), 4-connected N, E, S, W:
- Start at `(sx, sy)` (value 0); all four neighbours must be in bounds, not river (`WData 0x1000`), not ocean, not on the
  border; else return 0.
- Pop `P`; if `P` is ocean, river or border: when `min_len < 1` or `P.length > min_len` write the path into
  `out_*[P.length..0]` (index 0 = start) and return `length + 1`; else drop `P` (not closed).
- Expand `P` over N, E, S, W: skip out of bounds, already open (no improvement test), closed, a non-root ancestor of `P`,
  or `WData.flags & 2`; count the candidate's neighbours (other than `P`) that are river/ocean/border when the candidate
  is inland — more than 1 → skip (and skip if a neighbour is an ancestor). An **end candidate** (ocean, border, river):
  if `vector_dist` to the start `< min_len` or `P` touches another river cell → discard it and all siblings, close `P`,
  stop; else insert it with value 0, `length + 1`, discard the siblings, close `P`, stop. A **land candidate**: skip
  near start cities (`start_city_rad_wcoord`), `WData.blocked != 0` or `WData.flags & 8`;
  `r = get_land(N, 1) ≥ 0 ? rnd(20) + 10 : 0`; with `L = P.length ≥ 1`: straight (same row/column as `P.parent`)
  `r ·= windiness + 1`, turn: `if windiness == 0 r ·= 10; r ·= 101 − windiness`; value: `L > min_len` → `1 + (L −
  min_len) + r`; `L < min_len/2` → `d = min_len − L`, `10·(d/2 + 2d) + 1 + r`; else `10·(d/4 + 2·(d/2)) + 1 + r`; negative
  → skip. Accepted candidates are inserted in slot order N, E, S, W after the loop, then `P` is closed.
- The RNG is drawn only for land candidates reaching `get_land`, in N, E, S, W order; an end candidate stops further
  draws for that expansion. (`calc_river_cost` @687930 has the same formula but no callers.)

## 13. `game_random` in movement (call-order contract)

All draws are `get(0, 0xFFFF)` followed by the operation shown.

| Site | Draw | When |
|---|---|---|
| `Unit::do_move` @5f899d | `% 5` → far threshold 0x600/0xf00/0x1800 | every frame a moving unit's straight line is blocked (`find_path` = 1) and it re-plans |
| `PathFinder::astar_path` @6848b5 | `% 3 + 6` → `MoveOrder.retry` | U-level search hits the 32000-node cap (current order is a move) |
| `PathFinder::astar_path` @684df5 | `% 3 + 6` → `retry` | U-level open list exhausted, move order with `attempts ≤ 12` |
| `Unit::resolve_unit_collision` @5fa86e | `% 9 + 1` → `MoveOrder.pause` | after a successful collision repath when the other unit is blocked by me and not waiting |
| `Unit::do_group_move` @5e8491 | `& 1` → flock size 2/3 | group member whose slot is invalid on a special W cell |
| `PathFinder::calc_road_cost` @686328 | `% 20` | every valid road neighbour |
| `PathFinder::astar_river` @6870a6 | `% 20 + 10` | every eligible river land candidate (map generation) |
| `Guy::init_real` @5db6e9 | `% 100` → idle variant | each guy at unit creation / type change |
| `Guy::set_anim` @5dac68, @5db21d, @5db339 | `% 100` | DEFAULT restart (skipped while a saved search exists), ATTACK2 variants, gaia bird walk/jog |
| `Objects::process_all` @65dfad, @65dfd9 | `% xs`, `% ys` | every 32 frames per gaia bird spawn |

Movement-adjacent draws specified elsewhere: `Unit::find_attack_pos` (combat-spec 3.8.4, 3.11: `% 192` per valid
perimeter candidate of a building/wall target), `fight` (combat-spec 4.15). Not specified: `do_guard`, `think_scout`,
`Army::find_target`, `Group::action_queue_up`, `Objects::add_flock` (draws: `% 3` twice for the flock centre, then
per animal; rate-limited per leader, see §11.3).

## 14. Determinism notes for a port

- **Processing order**: players rotate per frame (`(frame + k) % 10`), units in index order; every unit sees the
  positions and collision bits of units processed before it in the same frame.
- **Integer everywhere** in pathing and stepping, with these exceptions: terrain z (`find_tcoord_z`, `find_data_z`,
  SSE single precision + truncation), the walk-variant ratio in `Guy::set_anim`, the crew track offsets from graphics
  pieces, and one formation term (`cvttss2si(((float)rows − 0.5f)·(float)ys)`). Use IEEE single precision with the
  same operation order and truncation.
- **Asset data in the simulation**: animation frame counts (`end_time`) and graphics-piece offsets (crew tracks) feed
  walked guy state.
- **Bug-compatible behaviours to keep**: the large-unit diagonal cache key (§7.3), the caravan heuristic's `−gy`,
  `angle_diff`'s `~d`, the empty-slot fast-path stall in `collide_here`, the spiral-table typos, non-refcounted collision
  bits, null blocks swallowing fast-path writes, uninitialised `cols` (COLUMN intermediate waypoints) and wedge row seed
  (sub-report ❓ U1/U2), `do_move` killing the order when a U-level search is time-sliced.
- **Per-call budgets depend on other units**: `limit = 500/max(1, repaths[who]²)`, where `repaths` is incremented by
  blocked units and halved every frame; the 4-frame staggering uses `(o + frame) % 4`.
- **Not saved**: the in-progress search trees (§3.3), the `PathFinder` scratch fields are saved but not checksummed.

## 15. Open questions

1. **No movement data to validate against.** Everything here is from code. The first multiplayer replay with
   CHECK_SUMS (part 1 hashes unit paths and guys every turn) will show divergence frame by frame.
2. **`do_move` kills the order when a U-level search is time-sliced** (`find_upath` returns −1 after saving, §3.5 step
   11). The code is unambiguous, but the behaviour (a long U search ends the move) is surprising; confirm in-game.
3. Grid bit meanings are partly unknown: TData 0x2000 (near-blocked; T-level cost ×4, naval "shallow" 3), 0x800 (river
   crossing / ford: half speed, road river cost), WData flags 0x200 and 0x40, player flag 4 (fog-limited pathing; the
   `find_wpath` fog branch is untraced), `objects+0x200` in `invalid_loc`. Reconcile with docs/world-spec.md when it
   exists.
4. Group movement: uninitialised `cols` (COLUMN intermediate waypoints) and the WEDGE row seed read stale memory in the
   original; forms 6–9 may be unreachable from the UI. `Group::get_loc`'s moving-leader branch, `modify_group_order`,
   `distribute_attack`, `refresh_group_order` were not fully traced.
5. `GuyData::get_speed` adds 9 when the order's vtable slot +0x2c is true (unidentified); `Guy` `off_x/off_y` initial
   values; which code sets `unit_masks` 0x10 (half speed) — not traced.
6. Rivers: whether loading a save re-runs `Map::make_rivers` (it would consume `game_random` on load).
7. The collision `circle_x/circle_y` tables used by `Objects::find_units` are built at runtime (`circle_init`) and were
   not dumped.
8. ~~`Objects::add_flock` may draw from `game_random`~~ it does (@65c0e0: two `% 3` draws for the centre, more per
   animal), after a per-leader 225-frame rate limit; not ported yet.

## Evidence

| Function | Address | Used for |
|---|---|---|
| `Objects::process_all`, `Objects::inc_time`, `Unit::process`, `Unit::work`, `Unit::do_job`, `Unit::inc_time` | 65dce0, 65db70, 610bc0, 60d180, 617a10, 610b40 | §2 frame order |
| `GameDaemon::process_all`, `process_coll_blocks` | 732700, 731f90 | `repaths` decay, block GC |
| `Unit::add_move_facing_order`, `add_move_order`, `kill_current_order`, `kill_current_path`, `clear_partial_path`, `repath`, `go_to` | 5e55c0, 616ed0, 5e2cb0, 5e31d0, 5e3920, 5e29b0, 5f7a50 | §3 orders and path stack |
| `Unit::do_move`, `do_flee_to`, `do_explore_to`, `do_attack_to`, `check_target_path` | 5f7b30, 5f2480, 5f24a0, 5f2320, 5e22d0 | §3.5–3.6 |
| `Unit::find_path`, `go_around_building` | 5fb910, 5fc350 | §4 |
| `Unit::move_step`, `Unit::set_angle`, `Unit::set_new_location`, `Unit::update_z` | 5faf30, 605400, 5f8d20, 606590 | §5, §9 |
| `PathFinder::find_wpath` / `find_tpath` / `find_upath` / `find_upath_restore` / `find_wpath_army` (+ wrappers) | 688fc0, 6897d0, 682f30, 688f40, 683730 (688e10, 688e60, 688eb0) | §6 |
| `PathFinder::astar_path`, `first_open_node`, `add_to_openlist`, `kill_lists`, `clear`, `init`, `walk_data` | 683770, 687970, 687aa0, 687ae0, 688150, 689ec0, 689cb0 | §7 |
| `Tree<PathNode*,int>::ordered_insert / remove_current`, `BRTree<…>::ordered_insert / seek`, `Recycler<PathNode>::pop` | 4796f0 / 479770, 479920 / 479c00 / 479ef0 / 479b90, 479540 | §7.4 containers |
| `PathFinder::calc_cost`, `building_danger`, `valid_ucoord`, `valid_tcoord`, `valid_wcoord`, `UnitData::invalid_loc`, `UnitData::needs_transport`, `WorldData::is_blocked_at` | 684e50, 685900, 687c80, 687d60, 687da0, 607c30, 609920, 461340 | §8 |
| `move_x`, `move_y`, `radius` (compass spiral), `div_3_table`, `init_coord_lookup_array` | adcaf0, adc400, add1e0, cae5fc, 681db0 | tables |
| `vector_dist` (3 overloads), `find_angle`, `angle_diff`, `cosx`, `sinx`, `sin_table`, `cos_table`, `trig_init` | 46cff0 / 46d060 / 46f020, 92d130, 92d0b0, 92d0c0, 92d100, a46a00, a469f0, a46980 | §1 |
| `Unit::update_speed`, `UnitData::speed`, `UnitData::get_speed` (2), `UnitData::turn_speed`, `GuyData::turn_speed`, `GuyData::get_speed` | 6055c0, 60aae0, 608720 / 6086f0, 60a600, 5de340, 5de410 | §9.1–9.2 |
| `Guy::process`, `move`, `set_new_location`, `set_angle`, `do_turn`, `turn_towards`, `turn_angles`, `set_anim`, `init_real`, `update_gpiece`, `update_z` | 5e0230, 5d9240, 5d86f0, 5d9010, 5d97a0, 5d9720, 5d98c0, 5da300, 5db6b0, 5d8530, 5d9950 | §9.3 |
| `CollCheck::collide_here`, `fill_slots`, `move_unit`, `World::new_coll_block`, `WorldData::get_coll_block`, `BitMask<768>::empty`, `Object::add_to_world / remove_from_world` | 682540, 6820e0, 682ad0, 46d250, 6b5350, 479150, 64d8c0 / 647970 | §10.1–10.2 |
| `Unit::detect_unit_collision`, `will_be_corner`, `is_corner`, `is_here`, `detect_boat_collision`, `resolve_unit_collision`, `resolve_block`, `Objects::find_units` | 617060, 609fa0, 60a040, 60a0c0, 5fa8b0, 5f9d30, 5fccc0, 65a620 | §10.3–10.5 |
| `Group::add`, `Groups::push_group`, `get_open_slot`, `Group::action_move_near`, `GroupData::find_leader`, `Group::compute_form`, `Form::compute`, `categorize`, `type_cat`, `compute_rows_and_columns`, `compute_dests`, `Unit::add_group_move_order` | 714350, 70f9e0, 6fa460, 704990, 70ccb0, 707c80, 72e8e0, 72e250, 72dfc0, 72d910, 72cba0, 5e4710 | §11.1–11.2 |
| `UnitType::find_nearby_spot`, `project`, `ObjectsData::find_unit_with_radius` / `find_unit_ordered_with_radius`, `Objects::find_collision` / `find_ordered_collision`, `Search::valid_search` / `valid_filter`, `GroupData::get_loc` / `get_loc_to`, `UnitData::get_final_loc`, `Group::set_up_insert` / `finish_insert`, `Objects::add_flock` | 61de70, 92cf40, 659890 / 658ef0, 65b1b0 / 65b440, 67daa0 / 67dbb0, 70e030 / 70c5d0, 608040, 70e520 / 70e620, 65c0e0 | §11.1–11.3 |
| `Groups::process`, `Group::update_positions`, `Unit::do_group_move`, `ungroup_move_order`, `Group::kill_group_move`, `Armies::process_all`, `Army::process` | 6fa210, 713810, 5e79a0, 5fd140, 7123f0, 6f3b00, 6f93d0 | §11.3–11.4 |
| `PathFinder::find_road`, `find_road_restore`, `astar_caravan_road`, `calc_road_cost`, `valid_roadcoord`, `astar_river` | 688a40, 685950, 685990, 686300, 688740, 686690 | §12 |
| `game_random` reads | `re/analysis/xrefs_random.txt` | §13 |

Decompiled sources and disassembly (local): `re/decomp/move/` (core, `vtables.txt`, `callers_pathfinder.txt`),
`re/decomp/move_asm/`, `re/decomp/move_pf/` (containers, trig, costs, validity), `re/decomp/move_road/`,
`re/decomp/move_guy/`, `re/decomp/move_coll/` (`compass_tables.txt`: all compass tables), `re/decomp/move_group/`.
