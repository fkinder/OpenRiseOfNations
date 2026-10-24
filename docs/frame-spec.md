# Frame skeleton: the OS loop, turn pacing, `Game::do_frame` and the per-object step

Status: ✅ loop/turn/frame order, `Objects::process_all` / `inc_time` iteration, `Unit::process` structure, order-memory
cycle (all from decompilation, PDB names) · 🟡 object lifecycle (slot allocation, death, removal: §8, from a sub-pass over
the decompiles, not cross-checked against data) · ❓ nothing here is validated against a running game yet (no real
replay with commands exists on this Mac, see replay-spec.md §open questions).

Companion specs: [replay-spec.md](replay-spec.md) (command bytes, turn timing, RNG), [orders-spec.md](orders-spec.md)
(command → group → order → `Unit::work`), [state-spec.md](state-spec.md) (what is persistent state).
Addresses are `riseofnations.exe` VAs. Decompiles: `re/decomp/core/`, `re/decomp/frame/`, `re/decomp/cmds/`,
`re/decomp/orders/`, `re/decomp/orders2/` (jobs `re/jobs/dc_frame.txt`, `dc_orders.txt`, `dc_orders2.txt`, `dc_cmds.txt`).
Virtual-slot names come from the PDB's method lists (see "Tools" under Evidence).

Conventions used below:
- `frame` = `Game.frame` (Game +0x550), the number of simulation steps completed. `tick` = Game +0x560.
- `who` = leader index 0..9. Leaders 0..7 are players, **9 is gaia** (animals, birds, herds); slot 8 exists in every
  per-leader array but its role was not traced (❓, probably scenario/neutral). `leaders.list[who]` stride 0x6eec
  (@e3a390). `leader_flags` bits are the PDB enum `LeaderFlagIndex`: 0x1 VALID, 0x2 ACTIVE, 0x4 HUMAN, 0x40 DEFEATED, …
- Objects live in per-leader lists `objects.lists[who].list[o]`; units use `o ∈ [0, unit_mark[who])`, buildings
  `[2000, build_mark[who])`, walls `[3000, wall_mark[who])` (never used: Rise of Nations has no player-built walls and `Objects::init_wall` has no callers; production-spec §14) (`ObjectsData`: `lists[10]` +0x4, `unit_mark` +0x15c,
  `build_mark` +0x184, `wall_mark` +0x1ac). An object is **alive** when `SubObject.flags & OBJECT_VALID (1)`.
- `SubObject.flags` (+0x8) is the PDB enum: 0x01 VALID, 0x02 STARTED, 0x04 ACTIVE, 0x08 IDLING, 0x10 NEW_THINK, 0x20 CITY,
  0x40 DETECTOR, 0x80 ATTACKING.
- **In-memory coordinates are XOR-obfuscated**: `SubObject.x_internal/y_internal/z_internal` (+0x10/+0x14/+0x0c) hold
  `coord ^ 0x63637` (cleared objects hold `0xfff9c9c8` = −1 ^ 0x63637, `SubObject::clear` @662420). The DataWalk writes
  the raw member bytes, so saves and checksums contain the XORed values; a reimplementation that stores plain coordinates
  must XOR on walk. Every read in the code is `x_internal ^ 0x63637`.
- "every N frames, staggered" means `(frame + o) % N == 0` with `o` the object's own index (+0xa). The code computes it
  as `(frame + o) & (N−1)` with a sign fix-up; frame and o are never negative, so plain `%` is equivalent.

## 1. The OS loop and where commands run

`Game::run` @584590 sets `game_random.seed = GameInfo.seed` before `Game::init` (replay-spec.md §12), then, after
`CommandManager::init`, `TurnControl::init`, `CommandManager::start`, it runs three simulation-affecting calls once before
the first frame: `Leaders::init_production_script`, `GameDaemon::calc_danger`, `GameDaemon::update_all_seen`. Then:

```
do { } while (Game::loop());            // one iteration per OS frame
```

`Game::loop` @591570, one iteration:

1. `time_cached = timeGetTime()` (wall clock; presentation only).
2. If a modal window is open in single player (not the diplomacy screens), or `GameMod::changing_assets`, or re-entrancy:
   render only (`loop_render(1, 2)`) and return.
3. If `semaphore byte1 & 1` is clear: `NetDaemon::process_all` (network pump; no sim state).
4. `TurnControl::do_frame` @957dd0 → returns 1 if a simulation step is due. If so: **`Game::do_frame`** (§3), then
   `graphic_last_frame = frame`, `graphic_dt = 0`, `graphic_percent = 0`.
5. Render (`loop_render`), with up to 10 skipped renders during playback catch-up. Rendering never touches sim state.

`TurnControl::do_frame` @957dd0: `DropControl::process_time_outs`, `TimeSync::update`; returns 0 if
`Game.semaphore byte0 & 0x40`; otherwise `do_frame_solo` (byte0 & 4 clear) or `do_frame_multi`.

### 1.1 Single player: `TurnControl::do_frame_solo` @9556a0

```
if frame_guard: error "re-entrant do_frame_solo!", return 0
CommandManager::process_turn()                 // EVERY loop iteration, frame due or not (§2)
if ScenarioData::load_scenario_script & 0x80 and TurnControl.semaphore byte0 & 1 (paused):
    run the scenario script                    // scenario scripts keep running while paused
due = playback ? check_new_frame_playback() : check_new_frame_solo()
if due:
    turn_counter++
    if turn_counter == turn_length (always 1 in single player):
        CommandManager::issue_camera()         // queues a CAMERA command for the NEXT process_turn
        if target_frame_time != timings[speed]: frame_base_time = 0, frame_counter = 1, target_frame_time = timings[speed]
        turn_counter = 0
    return 1                                   // → Game::do_frame
return 0
```

`check_new_frame_solo` @956320 (wall-clock pacing only; it never changes sim state):
- TurnControl paused (`TurnControl.semaphore byte0 & 1`): due only if the single-step bit (byte0 & 2) is set; it is
  cleared when consumed.
- `Game.fast_forward_frame ≠ 0`: always due.
- Otherwise `now = timeGetTime() + time_sync offset`, `due_at = frame_counter × target_frame_time + frame_base_time`.
  If the scene is rendering (`scene->render_phase ≠ 0`) and `now < due_at`: not due. If more than
  `target_frame_time + 2000` ms behind, the clock is re-based (`frame_counter = 1`, `frame_base_time = now`). Otherwise
  `frame_counter++`. Due.
- `timings[speed]` = {200, 125, 67, 50, 1} ms per frame (@afc4a4); `speed` starts at the setup's `game_speed`
  (`TurnControl::init` @957e20) and changes through SPEED_* commands.

**For a reimplementation the pacing is irrelevant to determinism**: only the sequence "execute all commands recorded for
frame F, then step" matters (replay-spec.md §7, §14). Multiplayer turn grouping is in replay-spec.md §10.

## 2. Command execution relative to the step

`CommandManager::process_turn` @93ef10 (solo branch; multiplayer and playback in replay-spec.md §7, §10):

1. Reset `CommandPackage::checksums[8]` / `random_seeds[8]` (MP bookkeeping).
2. If the local package is non-empty: `local_package_stamp++`, `play = console.play`, stamp it, `RecordGame::write_package`
   if recording (byte0 & 8), then **`CommandPackage::process_all(&local_package)`**, which executes every command in order
   and consumes the package (its `size` is decremented to 0 as commands are processed; `valid = 0` at the end).
3. `CheckSums::check_all` (playback only logs to SyncLogger), `CommandPackage::end_process` (MP checksum compare),
   OOS recovery if `semaphore byte3 & 8`.

`CommandPackage::process_all` @94c500 → `CommandPackage::process` @94a700 (switch on the opcode → `process_<verb>`; for
unit verbs from the local console player it also sets `options->rebuild = 1`, a UI refresh). The package's `group`
field (the "current group", set by GROUP) persists across commands within the package; see orders-spec.md §1.

Consequences:
- All commands issued while frame `F` is the current frame (UI input during rendering, `issue_player_speed` inside
  `do_frame`, `issue_camera` at the turn end) are executed at the **start of the next `Game::loop` iteration**, before the
  `Game::do_frame` that turns `F` into `F+1`. They are recorded with `frame = F`.
- Several `process_turn` calls can happen between two steps (when the frame is not due yet); each executes whatever has
  been queued since. Their records share the same frame number. Execution order = issue order.
- During playback the same position in the loop executes every record with `record.frame ≤ frame`.

**Reimplementation rule:** `for each frame F: execute all commands with record frame F (file order); Game::do_frame()`.

## 3. `Game::do_frame` @591ef0 in full

Steps that only touch presentation or wall-clock state are marked (P). Everything else mutates simulation state and must
run in this order.

| # | Step | Notes |
|---|---|---|
| 1 | (P) `netsys` frame notification; debug break at `frame_to_break`; `AutoSave::restore` if `semaphore byte1 & 0x20` | |
| 2 | (P) frame-time statistics (`game_frame_time`, 30-entry FPS ring); optional test delay using `internal_random` | never `game_random` |
| 3 | If `(frame & 7) == console.play`: `CommandManager::issue_player_speed` | queues PLAYER_SPEED; runs before the next step (§2). Depends on the local seat, so it differs per machine; the command itself is replicated |
| 4 | (P) keyboard snapshot (`prevFrameKeystate` ← `keystate`, `GetKeyboardState`) | scripts can read keys; UI only |
| 5 | If `info.scenario_script` non-empty: `RunTimeEnv::run_script(scenario_script)` | scenario trigger VM |
| 6 | If `semaphore byte1 & 0x10` or `byte2 & 2` (Conquer the World): if `frame > 0` and a general-powers script exists: run it; if CTW: `ConquestGame::place_reinforcements`; tutorial prompt (UI) when `byte2 & 8` and `tick > 300` | byte1 0x10 is set by `Game::run` when `setup.game_rules == 10` |
| 7 | (P) ranked ELO penalty upload at `tick > 59` | |
| 8 | **`Leaders::process_all`** @6ed2a0 (§4) | |
| 9 | Rush-rules war declaration: if `setup.rush_rules > 8` and `frame == rush_rules_table[rush_rules].+0x3c × 900`: for every VALID, non-neutral leader pair that is neither allied nor enemy, `Leader::set_diplo(…, 0)` (war) + chat message | team styles 0, 8, 11 skip it |
| 10 | If not editor (`semaphore byte1 & 8` clear): **`Leaders::strategy_all`** @6ed430 | AI |
| 11 | **`GameDaemon::process_all`** @732700 (§5) | includes `Groups::process` |
| 12 | If not editor: **`Armies::process_all`** @6f3b00 | AI army objects |
| 13 | **`Objects::process_all`** @65dce0 (§6) | every unit, building, wall |
| 14 | **`Objects::inc_time`** @65db70 (§7) | animation/time advance, ammo, deaths, farms |
| 15 | `GraphicEvents::process` @8e50a0 | every 32 frames ORs the owners' "seen" bits into each graphic event; fog bookkeeping for effects, not checksummed (P-ish, but it is saved) |
| 16 | **`Leaders::end_process_all`** @6ed070 | per ACTIVE leader 0..7: clears / raises the pop-cap warning flag `Player.flags & 0x800` (UI feedback for the local player) |
| 17 | If not editor: `Achieve::capture_data` @7af980 | statistics graph sampling every `achieve.rate` frames (rate doubles when the buffer fills). Saved, not checksummed |
| 18 | For each VALID leader 0..7: `Leader::process_event_frame` @6ec180 | every 50 frames: rolls per-leader activity counters (LeaderData +0xa50..+0xa66) and picks the music mood for the local player. LeaderData is checksummed, so this must be reproduced |
| 19 | **`frame++`** | |
| 20 | **`OrdersMemManager::cycle`** @730e20 (§11) | |
| 21 | **`Roads::scan_and_kill_stray_roads`** @8956a0 | incremental scan: `world.size / 500` cursor steps per frame over the tile grid (cursor at Roads +0x5fc/+0x600), each step checks a 4×4 sub-cell block for road pieces without support |
| 22 | If `frame % 15 == 0`: **`tick++`**, `TurnControl::check_cannon_time` @9579e0 | cannon time (a timed pause mechanic) ends 75 frames after it started (`frame − cannon_time_stamp > 0x4a`) |
| 23 | (P) debug auto save/load every `auto_save_load` frames | test hook |
| 24 | Zoom statistics: `Game.player[console.play]` accumulators +5 (zoomed in, levels 5–6) or +6 | folded into PLAYER_SPEED; local-only counters, replicated through the command |
| 25 | (P) frame-time prediction for rendering, MP lag check | |
| 26 | If `semaphore byte2 & 0x40`: `Game::process_end_game` (end-of-game UI, ranked result) | |
| 27 | (P) `Scene::process_capture_sequence` | |

`NetDaemon::process_all` is called four times between the steps above; it only moves network messages.

**Semaphore bits seen here** (`Game.semaphore`, BitMask<256>; byte n bit b): byte0 0x04 multiplayer, 0x08 recording,
0x10 playback, 0x40 frame stepping suspended (TurnControl returns 0); byte1 0x01 set by `CommandManager::start` (game
started), 0x08 **scenario editor** (no AI strategy, armies or achievements; `Game::run` posts the `ScenarioEditorStart`
telemetry event and pauses TurnControl in this mode), 0x10 game_rules 10 (scripted rules), 0x20 auto-save restore /
stop-processing-package flag; byte2 0x02 Conquer the World, 0x08 tutorial/scenario splash, 0x40 game over; byte3 0x08 OOS recovery
pending. Bits not listed were not needed.

## 4. Leaders

All three loops run over leaders **0..7** in index order (the loop bound is `&leaders.list[8]`, @e71af0).

`Leaders::process_all` @6ed2a0, for each leader with `LEADER_ACTIVE`:
1. Clear `LEADER_BEING_JAMMED`; zero two per-frame counters (LeaderData +0x7e8, +0x9f4).
2. Jamming: if any VALID leader with `LEADER_JAMMING` is not mutually allied (diplomacy table value 2 both ways) with this
   one, set `LEADER_BEING_JAMMED`.
3. `Leader::gather` (economy: resource income), then `Leader::calc_wall_stats` if `LEADER_CALC_WALL_STATS` (flag cleared
   first) and `Leader::calc_unit_stats` if `LEADER_CALC_UNIT_STATS` (likewise).
4. `Leader::process_elimination`.
5. Every `constants.timer_refresh_ratio` frames: three negative countdown timers at LeaderData +0x414/+0x440/+0x448 step
   toward 0 unless their paired flag (+0x418/+0x444/+0x44c) is set.
6. If `frame ≠ 0`: for 8 scheduled taunts (+0x3b4 table, due frame at +0x3d4) whose due frame == `frame`:
   `Leader::process_taunt`.
7. Clear `LEADER_IDLE_PEASANT`.

`Leaders::strategy_all` @6ed430, for each leader with VALID|ACTIVE: `Leader::check_explore`, `plan_strategy`,
`compute_score(0)`, `diplomacy`. Then, if `semaphore byte1 & 2`, `Game::check_victory`. (Whether a leader is computer
controlled is checked inside these functions.)

These functions are the economy and AI; their internals are out of scope here and get their own specs.

## 5. `GameDaemon::process_all` @732700

1. `busy` countdown; `repaths[0..7] = repaths/2`, values below 3 become 0 (a per-player path-request throttle that
   `Unit::do_move` increments).
2. `process_victory`.
3. Every 200 frames (`frame % 200 == 0`): `calc_danger`. When `frame % 100 == 33`: `update_all_seen`.
4. `calc_markets` (market prices; uses `game_random`, replay-spec.md §12).
5. Regions: for all 64 regions (stride 0x88): clear flag 0x10, then move flag 0x20 into 0x10.
6. `check_borders`, `process_coll_blocks`.
7. **`Groups::process`** @6fa210: for each VALID leader 0..7, process **one** of its 64 group slots,
   `groups[who × 64 + proc_group]`: drop dead or departed members (back to front), `Group::find_role`, recompute the group
   speed from its leader unit; then `proc_group = (proc_group + 1) % 64`. Details in orders-spec.md §1.

`Armies::process_all` @6f3b00: for each leader 0..7 that is VALID, not (`flags & 0xc == 4`, i.e. human without co-op AI)
and whose `leader_flags2 & 0xa` (UNIT_AI_OFF | COMBAT_AI_OFF) is clear: every `Army` with `valid ≠ 0` → `Army::process`
(with a "new" flag taken from and cleared in the army's flags bit 7).

## 6. `Objects::process_all` @65dce0: the per-object step

```
// 1. units, rotating the starting player every frame
for k in 0..9:
    who = (frame + k) % 10
    if !(leaders[who].flags & VALID): continue
    for o = 0; o < unit_mark[who]; o++:            // unit_mark re-read every iteration
        u = lists[who][o]
        if u.flags & VALID: u->process()           // vtable +0x9c: Unit::process / Animal::process
        else if u.hold_frames: u.hold_frames--     // Object +0x32

// 2. buildings then walls, players 0..7 in index order
for who in 0..7:
    if !(leaders[who].flags & VALID): continue
    for o = 2000; o < build_mark[who]; o++:
        b = lists[who][o]
        if b.flags & VALID: b->process()           // Build::process @61edf0
        else if b.hold_frames:
            if b->get_build()->build_masks(+0x60) & 0x4000: Build::process_ejection(b)   // eject the garrison of a dead building (bit name ❓)
            b.hold_frames--
    for o = 3000; o < wall_mark[who]; o++:
        same as units                              // Wall::process @640450

// 3. every 32 frames (frame % 32 == 0): gaia birds and herds
if frame % 32 == 0:
    target = min(10, world.xs * world.ys / 100)
    have = number of alive gaia (who 9) units for which is_animal() and ptype.type == BIRD (402)
    repeat (target - have) times:
        tx = rnd(world.xs); ty = rnd(world.ys)    // rnd(n) = n <= 1 ? 0 (no RNG call) : game_random.get(0,0xffff) % n
        if world.cells[ty*xs + tx].flags & 0x20:
            o = Objects::init_unit(9, BIRD, tx*768+384, ty*768+384, -1, -1, -1)
            if o >= 0: units[9][o]->add_air_patrol_order(x, y, -1, -1, 0)
    if frame % 64 == 0:
        h = (frame / 64) % max(5, herds.length)
        if h < herds.length and herds[h] alive (+0x1a & 1): Herd::process(herds[h])
```

Points that matter for determinism:
- **The unit pass starts at player `frame % 10`** and wraps; buildings and walls always go 0..7. Gaia (9) and slot 8 units
  are processed in the unit pass; buildings/walls of slots 8 and 9 never are. Slots 8 (the herd animals of `Herd::create_units`) and 9 (the birds
  of the top-up below and the pen animals of `Farms::add_animals`; the list is not saved) are `Animal`s: their `process` is `Animal::process` @5d72c0
  (`Animal::work` + the guys: none of §9's countdowns, healing or attrition), see docs/sim-implementation.md "Fidelity log".
- The loop bounds are re-read, so objects appended at the end of a list during the pass (e.g. a unit created by
  `Objects::init_unit` in a slot ≥ the current index) are processed in the same pass; objects placed in a reused slot
  below the cursor are not (slot reuse rules: §8).
- `hold_frames` keeps a dead slot reserved for a while (§8).

## 7. `Objects::inc_time` @65db70

```
Nuke::do_damage()
for who in 0..9 (VALID leaders only):
    for o in [0, unit_mark[who]) alive:  obj->inc_time() (+0xa0); obj->execute_events() (+0x154)
    for o in [2000, build_mark[who]) alive: obj->inc_time()          // Wall::inc_time @63fb60 (Build inherits it); walls are not visited
for each good g in [0, good_mark) alive and not a plain Good: g->inc_time()
for each ammo in ammo_objs with flags & 3: Ammo::inc_time            // projectiles fly and hit here (Ammo::do_damage uses game_random)
for each DeathObj (0xa4 bytes) with valid ≠ 0: DeathObj::inc_time
Farms::inc_time(); Doober::inc_time(); Surf::inc_time()
```

`Unit::inc_time` @610b40: if the unit is on the map (`inside_up < 0`) or its type is 0x34/0x35, `Guy::inc_time` for guys
`[0, guy_mark)` and `[ptype.squad_size, guys.length)` (the per-soldier animation clock). `Unit::execute_events` @60edc0: for
active, on-map units without `unit_masks2 & UNIT2_HOLD_ANIM`, `Guy::execute_events` for each guy in `[0, guy_mark)`
(animation events: attack hits, sounds); otherwise only `GraphicEvents::verify_load`. Guy animation choice consumes
`game_random` (replay-spec.md §12), so these are simulation.

## 8. Object lifecycle (🟡)

The class chain is `SubObject` → `Object` → `Unit` (→ `Animal`) and `SubObject` → `Object` → `Wall` → `Build`. Init,
process and death are virtual (slots: `+0x88/+0x8c init`, `+0x90 close (SubObject)`, `+0x98 clear`, `+0x9c process`,
`+0xa0 inc_time`, `+0x150 close (Object)`, `+0x154 execute_events`, `+0x158 die`; Build adds `+0x19c/+0x1a0 init`,
`+0x1a4 start`, `+0x1a8 activate`, `+0x1b4 do_queue`).

`SubObject::init` @662300 (the base of every init): `who`, `o`, `flags = VALID` (| CITY if the type says so, type vfunc
+0x64), `set_type(type, 1)` (vfunc +0x84), x/y stored XORed, z from `TerrainOut::find_tcoord_z` at the tile.
`SubObject::close` @662660 sets `flags = 0`; `SubObject::clear` @662420 zeroes flags and ptype and sets x/y/z to −1.

### 8.1 Slots, uids and creation

**Slot choice: `Objects::find_free(who, base, limit, &mark, forced)` @65ad60** (no RNG):
- normal (`forced < 0`): return the **lowest** `o` in `[base, mark)` whose object is dead (`flags & VALID == 0`), has
  `hold_frames == 0`, and, for units, `o_up < 0` (not still linked in an uber chain). Otherwise, if `mark < limit`,
  return `mark++` (allocating the object if the pointer is null). Otherwise −1.
- forced (load / scenario placement): allocate every missing object in `[mark, forced]`, set `mark = forced + 1`,
  return `forced`.
- Units use `[0, 2000)`, buildings `[2000, 3000)`. Leaders 0..7 get `Unit` objects (0x158 bytes), 8 and 9 `Animal`
  (0x160). `Objects::init_wall` @658c50 just calls `init_build(…, preplaced = 1)`, so walls are `Build` objects in the
  2000 range; nothing seen allocates in the 3000 range (🟡, the loops still visit it).

**uid:** `Object::init` @647750 sets `uid = obj_ctr[who]++` (u16, wraps), `hold_frames = 0`, links/targets to −1,
`damage = 0`, `flags |= DETECTOR` if `has_objmask(0x2000000)`, then `Object::add_to_world` @64d8c0, which pushes the
object at the **head** of its tile's object list (tile iteration is newest first; gaia objects (who 9) are never
linked).

**`Objects::init_unit(who, type, x, y, forced_o, o_up, o_down)` @65e0c0** creates `type.uber_size` units (1 when
forced) as a chain: for each, `find_free`, `Unit::init` (vslot +0x8c), `o_up` = previous member, previous member's
`o_down` = this. Followers undo the leader counters that `init` added (`control`, `active`, `units_built`, type
tracking) and are moved to `UnitType::find_nearby_spot` around the captain. Returns the captain's `o`.

**`Unit::init` @612100**, in order: `close(0, −1, 0)` (clears the dead slot's path and orders); snap x/y to the centre
of the 48-unit cell (`x/48·48 + 24`); `Object::init` (keeps `guy_mark`); `units_built++`; `angle = 0x55555555`; field
resets; `close_orders`, `clear_partial_path`, `update_action`; stance from the leader options; register with
Supplies/Heroes/Caravans/Specials; **guys**: resize to `squad_size + crew_size` and `Guy::init_real` each (**at least
one `game_random` draw per guy**), `guy_mark = squad_size`; `set_new_location`; `update_hits/los/speed/armor`,
`update_seen(0)`; `unit_masks |= UNIT_AI_CONTROL` unless the owner is human without co-op AI (`leader_flags & 0xc == 4`).

**Buildings:** `Objects::init_build(who, type, x, y, preplaced, forced)` @65d190 → `find_free(…, 2000, 3000)`,
`snap_center`, `Build::init` @629740 (→ `Wall::init` @63e9b0: `Object::init`, clear STARTED|ACTIVE, zero
`job_counter`s and `build_masks`, `frame_started = −1`; then queue capacity 20 / 10 (tech-researching buildings) / 2
by building class (production-spec §6), gather
tiles via `find_gather_tiles` (**RNG**)). The building is neither started nor active yet.
Construction: a builder's `do_build` calls `Wall::do_construct(amount)` @6434d0: an unstarted site is started
(`Build::start` → `Wall::start` @63e810: `flags |= STARTED`, `frame_started = frame`, competing buildings on the
footprint are disbanded (no RNG for buildings, see §8.2)) or, if blocked, disbanded; a started site gets
`max(1, amount / (helpers + 1))` (`amount = accel_construct / 4` while the site is under attack; helpers =
builders already counted this frame, reset in `Wall::process`, which runs after the unit pass) added to `job_counter`;
when `job_counter ≥ construct_time`, **`Build::activate(0, 1, 1)`** @623e20 (`flags |= ACTIVE`, city/border/pop-cap
updates, tech gains, free tribe-bonus units via `train`, which create units and draw RNG). After activation
`job_counter` is reused as the turret angle.

### 8.2 Death

- `Object::take_damage` @652020 accumulates `damage` (+ 1/16 fractions in `damage_frac`); at `damage ≥ hits` it calls
  `die` (vslot +0x158) and passes overflow to the captain. A first hit on a building may spawn a bird flock (5 %,
  **RNG**). `Object::disband` = `die(rnd(3) + 1)` (**RNG**) **for units only**; a building is closed (`close(0)`)
  if unstarted or killed with `die(1)` if started, without a draw (production-spec §14). `Unit::die_uber` kills a
  whole chain.
- **`Object::die(dt, …)` @647080** (used by `Unit::die` and `Wall::die`): `close(dt, …)` (virtual), then
  `hold_frames = max(1, current, frames until this shooter's in-flight ammo lands)`.
- **`Unit::close` @60ee50** (alive unit): population/leader bookkeeping; uber-chain relink (the next live follower
  becomes captain; the dead unit is re-appended at the chain tail, so its slot stays unusable because `find_free`
  requires `o_up < 0`; when the last live member dies, the leader counters drop, `Leader::defeat` is checked, and every
  member's `o_up/o_down` is reset, releasing the slots); **death animation**: `anim = dt·2 + 13 + (rnd & 1)` (two more
  draws when `dt == 4`) and `Objects::kill_guy` → `add_death` (a `DeathObj`) per guy, `Ammo::init_crash` for aircraft
  (**RNG**); free the path; **`close_orders`**; `Object::close`.
- **`Object::close` @647160** (alive object): leave the container or eject/kill contents; `remove_from_world` (kept in
  the tile list while dying if the type blocks while dead); **`Objects::remove(who, o)`**; `hold_frames = 30`; finally
  **`flags = 0`**: the object stops being alive *inside* the `die` call, so the rest of the frame (including the rest of
  `Objects::process_all`) already sees it dead.
- `Objects::remove` @658980 lowers `unit_mark`/`build_mark` while the top slot is dead with `hold_frames == 0` (and
  `o_up < 0`). It runs before `flags` is cleared, so marks shrink lazily on later deaths.
- `Build::close` @628980: refunds the queue (`clean_queue`, no refund in the editor), gatherer counts, gather list,
  `Wall::close` → `Object::close`, then city/fort/dock/wonder closing and `BuildQueue::close`.
- A dead building with garrison (`build_masks & 0x4000`) ejects one occupant per frame in `process_all` and adds 1 to
  `hold_frames` each time, so the slot stays held until it is empty.

### 8.3 What this means for the frame

- An object killed during the pass is skipped if its slot is still ahead of the cursor, and its `hold_frames` (30) is
  decremented in the same pass.
- Units trained by buildings are created during the building pass, after the unit pass, so they first act next frame.
- A slot is reused 30 frames (or longer, see in-flight ammo and uber chains) after death, always the lowest free one.
  Commands address objects by `o` and check `uid`, so slot reuse order is part of the replay contract.


## 9. `Unit::process` @610bc0

Called from §6 for every alive unit (gaia animals use `Animal::process` @5d72c0, which ends in `Animal::work`). Field
names are `UnitData` members (`node re/tools/pdbtypes.mjs Unit`); `unit_masks` bits are the PDB enum `UnitMask`
(`UnitMask2` for `unit_masks2`).

1. **Countdowns:** `recharging` (+0xae), `full` (+0xac), `waiting` (+0xad), `healing` (+0x38) each decrement if non-zero.
2. **Special unit kinds** (exclusive):
   - Decoy (`unit_masks & UNIT_DECOY`): `mana_burn++`; when it reaches `decoy_time × (general_upgrade + 2) / 2` the unit
     closes itself (vfunc +0x150) and **returns**. If on the map and its fog cell is detected by an enemy
     (`WorldData::is_detected_by_enemy`: `seen3 & ~L[owner].ally_mask`, fog-spec.md §8), `suffer_attrition(frame % 7 ==
     0)`; return if that killed it.
   - Spell casters / generals (`ptype.unit_flags2 & UNITTYPE2_SPELLCASTER`): heroes and specials run `Caster::process_spells`; every 32
     frames (staggered) the jamming / shockwave toggle; mana regeneration (`mana_burn` decreases by
     `(rate + (frame & 1)) / 2` with tribe/tech multipliers, skipped while `UNIT_CLOAK_RUNNING | UNIT_MARCH_RUNNING | UNIT_CASTING` is set). Supply units
     regenerate only if they are governor heroes.
   - Aircraft (`ptype.domain == 2`, air): fuel/mana bookkeeping while airborne; on the ground (`inside_up ≥ 0`, i.e. in an
     airport/carrier) recover fuel and heal every `aircraft_heal_rate[heal_level]` frames (staggered by `o`).
3. **Carried production** (`num_queued` +0xa0 ≠ 0 and the owner ACTIVE): units that train other units (carriers, …)
   accumulate `queue_time += accel_train × ai_speed` until `train_time`; pop-cap check (feedback message if blocked);
   then `Objects::init_unit` at the unit's position and `go_inside` the producer.
4. `process_healing`.
5. If the unit is **on the map** (`inside_up < 0`):
   - every 16 frames (staggered): `targeted /= 4`; `process_cloak`; every 32 frames: `process_attrition` (clears
     `UNIT2_ATTRITION_SAFE` first); refresh of "detected" marks; merchant pop-up text (UI).
   - if `attrition` (+0x9e) ≠ 0 and `(frame + o) % attrition == 0`: `process_supply`; if not supplied,
     `suffer_attrition(1)` and return if killed.
   - **`work()`** (vfunc +0x188, `Unit::work` @60d180): the order machine, orders-spec.md §5.
   - `Guy::process` for guys `[0, guy_mark)` and `[ptype.squad_size, guys.length)`.
6. Otherwise (garrisoned / carried): clear several transient masks; every 32 frames (staggered by **who**, not `o`)
   check whether an AI-controlled unit (`UNIT_AI_CONTROL`) should `come_out` of its building; copy guy positions.

## 10. `Build::process` @61edf0

Called from §6 for every alive building. It starts with **`Wall::process` @640450**:
- `frame ≠ 0 and (frame & 7) == who`: `targeted /= 4`, `check_ever_seen`.
- every 32 frames (staggered): mask bookkeeping; for AI owners (`!(leader_flags & HUMAN)`) unfinished buildings:
  assign idle workers to wonders/forts (`add_build_order(QUEUE_NEW)`); the `Wall::process` disband applies only to AI oil
  platforms nobody targets (every 128 frames; no RNG). General AI abandonment is `Leader::check_orphaned_buildings` +
  `take_damage` on ORPHAN sites (production-spec §8.1).
- `helpers` → `build_masks & 0x400`, then `helpers = 0`, clear 0x800 (the per-frame builder count of §8.1).
- territory attrition every 32 frames, or 16 while rush rules forbid war (staggered): in enemy territory an unstarted
  site is disbanded (no RNG),
  otherwise `take_damage(8, …)`.

Then, only for **ACTIVE** buildings:
1. `healing` countdown; garrison ejection (mask 0x4000); `do_launch` (mask 8, **RNG**).
2. Attack (if `type.attack ≠ 0`): `Build::do_attack` @6228f0 every frame while it has a target, otherwise every 32
   frames (staggered) or when an attacker is near; turrets rotate `job_counter` toward the target by ≤ 5° per frame.
   `do_attack`: recharge countdown, `find_target` (`find_nearby_target`), `fire_ammo` (**RNG**),
   `recharging = recharge / garrison_arrows`. (🟡 turret buildings seem to fire through animation events in
   `Wall::inc_time` instead.)
3. French supply heal every `french_supply_heal_rate` frames (staggered).
4. **`do_queue(0)`** (vslot +0x1b4, `Build::do_queue` @61e410): the head item's `job_counter` advances by
   `accel_construct` / `accel_train` / `accel_research` (× `ai_speed`), clamped to `train_time`; the "done" test
   (`train_time ≤ job_counter`) is made **before** adding, so an item completes on the frame after it reaches its time.
   On completion `Build::finished(type)` @628490: units → pop-cap and caravan/aircraft limits, then `Build::train`
   (returns 0 = pop-capped, −1 = limit, 1 = trained); spells → cast; upgrades → `set_type`; techs → `gain_tech`. A
   trained item is unqueued (repeat mode re-queues it); a blocked head lets the next non-unit item run.
5. Gather tiles maintenance (region flags; `find_gather_tiles`, **RNG**).
6. Special buildings: Kremlin spies, Senate governor hero, market caravans, Terracotta army (timers and `train`).
7. City centres: capture check every 64 frames, AI auto-repair, assimilation, healing every `city_heal_rate` frames,
   alarm reset.
8. Road placement every 16 frames (mask 0x100).

`Build::train(type)` @62f9b0: `Objects::init_unit` at the building centre, inherit stance, `go_inside(building)`, then
stay inside (garrison buildings, universities) or `Unit::come_out` @617c10, which places the unit on a ring around the
building and applies the **gather (rally) points**: every point but the last becomes a `QUEUE_LAST` move; the last one
resolves to build/repair/gather/garrison/trade/attack on whatever is there, or a move (ATTACK_TO for units from
barracks, stables and docks). AI units may join an army (one **RNG** draw). Rally points are stored per building by
`Build::add_gather_point` @622e70 (GATHER_POINT command; orders-spec.md §2).

`Build::queue_up(type)` @620f40 (QUEUE_UP command): `can_pay_cost` (else `queue_fail = QUEUE_COST`), `pay_cost`
(skipped in the editor), append a `QueueItem {job_counter, type, good[3], cost[3]}`, `num_queued[type]++`.
`Build::unqueue(i, cancel)` @6207c0 refunds when cancelling and keeps the head's progress by removing the last of a run
of identical items.

## 11. Order memory: `OrdersMemManager` (ordmemmgr.obj)

`ordmgr` holds one `SafeRecycler<UnitOrder>` per order type (28 = `NUM_UNIT_ORDERS`), each a `clean_pool` and a
`dirty_pool` stack.
- `get_obj(type)` @730ac0: pop from `clean_pool[type]`, call its `clear()` (vfunc +0x4), return it; if empty, allocate
  with `::get_new_order(type)` @730550.
- `give_obj(order)` @730bb0: `clear()`, push to `dirty_pool[order->get_type()]`.
- `cycle()` @730e20 (step 20 of `do_frame`): move every dirty order to its clean pool.
- `init` @730cd0 pre-allocates 30 orders per type; `clear` @730db0 frees everything.

So an order released during a frame is not reused until the next frame, which keeps pointers held by other units valid
for the rest of the frame. **No simulation value depends on this** (orders are compared by content and owner, never by
address), so a reimplementation may use any allocator, as long as a released order is never read again in the same frame
in a way that would see a reused object. Order nodes (`RecycledOrderNode`) come from a plain `Recycler`.

## 12. What the reimplementation's frame looks like

```
apply_commands(frame)                       // replay records / local input for this frame, in order (§2)
// Game::do_frame
if (frame & 7) == local_play: queue PLAYER_SPEED   // only matters for producing commands, not for playback
run scenario script; [general powers; CTW reinforcements]
Leaders::process_all; rush-rules war check
if !editor: Leaders::strategy_all
GameDaemon::process_all (→ Groups::process)
if !editor: Armies::process_all
Objects::process_all; Objects::inc_time
GraphicEvents::process
Leaders::end_process_all; if !editor: Achieve::capture_data; Leader::process_event_frame ×8
frame++; OrdersMemManager::cycle; Roads::scan_and_kill_stray_roads
if frame % 15 == 0: tick++; check_cannon_time
```

Every `game_random` draw inside these calls happens in this order; nothing outside this list (rendering, UI, sound)
may draw from it.

## Evidence

| Function | Address | Used for |
|---|---|---|
| `Game::run`, `Game::loop`, `Game::do_frame` | 584590, 591570, 591ef0 | §1, §3 |
| `TurnControl::do_frame / do_frame_solo / check_new_frame_solo / compute_next_turn_solo / init / check_cannon_time` | 957dd0 / 9556a0 / 956320 / 955fe0 / 957e20 / 9579e0 | §1, §3 |
| `CommandManager::process_turn / start`, `CommandPackage::process_all / process` | 93ef10 / 942e10, 94c500 / 94a700 | §2 |
| `Leaders::process_all / strategy_all / end_process_all`, `Leader::process_event_frame` | 6ed2a0 / 6ed430 / 6ed070, 6ec180 | §4 |
| `GameDaemon::process_all`, `Groups::process`, `Armies::process_all` | 732700, 6fa210, 6f3b00 | §5 |
| `Objects::process_all / inc_time` | 65dce0 / 65db70 | §6, §7 |
| `Unit::process / inc_time / execute_events / work` | 610bc0 / 610b40 / 60edc0 / 60d180 | §7, §9 |
| `SubObject::init / close / clear` | 662300 / 662660 / 662420 | §8 |
| `Objects::find_free / init_unit / init_build / init_wall / remove`, `Object::init / add_to_world / die / close / take_damage / disband` | 65ad60 / 65e0c0 / 65d190 / 658c50 / 658980, 647750 / 64d8c0 / 647080 / 647160 / 652020 / 6455c0 | §8 |
| `Unit::init / close / die_uber`, `Build::init / start / activate / close`, `Wall::init / start / do_construct / process` | 612100 / 60ee50 / 606460, 629740 / 6273a0 / 623e20 / 628980, 63e9b0 / 63e810 / 6434d0 / 640450 | §8, §10 |
| `Build::process / do_attack / do_queue / finished / train / queue_up / unqueue / add_gather_point`, `Unit::come_out` | 61edf0 / 6228f0 / 61e410 / 628490 / 62f9b0 / 620f40 / 6207c0 / 622e70, 617c10 | §10 |
| `GraphicEvents::process`, `Achieve::capture_data`, `Roads::scan_and_kill_stray_roads` | 8e50a0, 7af980, 8956a0 | §3 |
| `OrdersMemManager::get_obj / give_obj / cycle / init / clear`, `::get_new_order` | 730ac0 / 730bb0 / 730e20 / 730cd0 / 730db0, 730550 | §11 |
| Vtables (Unit @b417d0, Build @b42174, Wall @b42cf8, Animal @b4145c, UnitOrder @b474f0, …) | `re/analysis/vt_orders.txt` (job `re/jobs/vt_orders.txt`) | slot targets |
| Enums `LeaderFlagIndex`, `LeaderFlag2Index`, `UnitMask`, `UnitMask2`, object flags (`OBJECT_VALID` …), `OrderIndex`, `QueuePos` | `rise.pdb` | flag names |

Tools: `node re/tools/pdbtypes.mjs <Class>` for layouts. Virtual slot names were read from the PDB method lists
(LF_ONEMETHOD / LF_METHODLIST `vbaseoff` of introducing virtuals): e.g. `SubObject` +0x9c `process`, +0xa0 `inc_time`,
`Object` +0x150 `close`, +0x158 `die`, `Unit` +0x184 `do_idle`, +0x188 `work`; `UnitOrder` slots in orders-spec.md §3.

## Open questions

1. **Leader slot 8**: processed in the unit pass and in `inc_time`, never in the building pass or the leader loops. Who
   owns it (scenario "neutral"?) was not traced.
2. **Slot reuse order** (§8.1) is code-derived only: lowest free slot, 30 frames of `hold_frames`, longer for shooters
   with ammo in flight and for uber-chain members. A replay with deaths followed by training is the first real test.
   The 3000+ wall range appears unused (walls are created as buildings).
3. `semaphore` bits byte1 0x01 / 0x20 and byte0 0x40 are named from one or two uses each; confirm before relying on them.
4. `Leader::gather`, `plan_strategy`, `Army::process`, `GameDaemon::calc_markets`, `Herd::process`, `Guy::process`,
   `Ammo::inc_time`, `DeathObj::inc_time` are listed, not specified; each consumes `game_random` and needs its own spec.
5. **Turret buildings** appear to fire from animation events in `Wall::inc_time` @63fb60
   (`AnimationPacket::get_game_frames`, `GraphicEvents::execute_game_events`) rather than through `fire_ammo` in
   `Build::do_attack`. If so, simulation timing depends on animation frame counts from the art data, as it does for
   units through `Guy` events (§7). Needs a dedicated pass.
6. **Local-player-only writes into saved state**: hint timers at LeaderData +0x6ebc..+0x6ec4 (`Build::process`, only
   for `console.who`) and `Player.flags & 0x800` / `pop_cap_frame` (pop-cap warnings in `Unit::process`,
   `Leaders::end_process_all`, `do_queue`). Checked against `re/iface/datawalk.json`: +0x6ebc..+0x6ec7 lies between the
   `prod_script` String (ends +0x6eb8) and `make_list` (+0x6ec8), so it is **not walked** (neither saved nor
   checksummed), and `Player` is not part of any CHECK_SUMS part. These writes can stay seat-local in a
   reimplementation; they must just never feed back into simulation decisions (not verified).
