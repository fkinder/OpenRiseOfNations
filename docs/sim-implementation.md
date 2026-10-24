# Simulation core: what is implemented (engine/sim)

Status of the C++ simulation in `engine/sim/` against the specs ([frame-spec.md](frame-spec.md),
[orders-spec.md](orders-spec.md), [movement-spec.md](movement-spec.md), [world-spec.md](world-spec.md)). Target: bit
identity with the original under lockstep. Nothing has been compared with a real replay (none with unit commands
exists), so "ported" means "from the spec and the decompiles", not "verified". See "Resume here" for what is next.

## Layout and API

| File | Contents | State |
|---|---|---|
| `trig.{hpp,cpp}` | `vector_dist`, `find_angle`, `sin_table`/`sin_a`/`cos_a` (sinx/cosx wrappers), `angle_diff` | ported from the asm, incl. 32-bit wraps |
| `objects.hpp` | PDB enums, `Order` (one record for every order class), `PathData`, `Guy` (Guy::clear defaults), `Object`, `Unit`, `Build` | data model |
| `game.{hpp,cpp}` | `Game` (rules, world, regions, heights, game_random, frame/tick, leaders, objects, GameDaemon, groups, pathfinder), `Game::load`, `Game::do_frame`, diplomacy (`is_ally`/`is_enemy`/`is_team`, scenario chunk 41 over Leader::init defaults), goods / items arrays, leaders' tech bits | skeleton per frame-spec §3/§12 |
| `fog.{hpp,cpp}` | fog of war ([fog-spec.md](fog-spec.md)): grid writers, every `WorldData` fog query, LOS (`Unit/Wall::update_los`, `UnitData::los`), `stamp_range` + `Object::update_seen`, `update_local_seen`, `reveal_fog`, `update_seen_ally`, `GameDaemon::update_all_seen`, `Wall::check_ever_seen`, the fog parts of the leader (`ally_mask`, `check_explore`, `new_rare`, `meet`), per-object `is_seen` / `is_now_seen` | ported (see "Fog of war") |
| `objects.cpp` | `find_free`, `init_unit` (followers take back the counted bookkeeping), `unit_init` (Unit::init order, counted `track_unit_type` / control / active), `add_to_world`/`remove_from_world` (wcoord lists + footprints), `objects_process_all` (rotating start player, birds, `Build::process`), `objects_inc_time` (Wall::inc_time, the Ammo and DeathObj passes), `unit_process` | partly ported |
| `production.hpp`, `production_internal.hpp`, `techs.cpp`, `costs.cpp`, `build.cpp`, `build_place.cpp`, `cities.cpp`, `territory.{hpp,cpp}`, `queues.cpp`, `construct.cpp` | production ([production-spec.md](production-spec.md)): availability and `gain_tech`, prices and times, building life cycle, placement, cities / forts / wonders, territory, queues and spawning, construction / repair / deletion, the building pass | ported (see "Production") |
| `combat.hpp`, `combat_*.cpp` | combat ([combat-spec.md](combat-spec.md)): `combat_damage.cpp` (stat getters, `get_damage` core + wrapper, `do_damage`, `take_damage`, kill / score values), `combat_death.cpp` (`Object::die`, `Unit::close`, `Object::close`, `kill_guy`, `add_death`, DeathObj pool), `combat_target.cpp` (range, validity, acquisition, `think` combat part, `target_opportunity`), `combat_orders.cpp` (attack orders, `find_attack_pos`, `check_target_path`, group attacks), `combat_fight.cpp` (`fight`, Ammo, release events, building fire, fire_proj) | ported (see "Combat") |
| `orders.{hpp,cpp}` | order deque (front = current), `add_order`, `add_move_facing_order` (incl. the attack-move re-add), `add_move_order`, `kill_current_order`, `close_orders`, `clear_orders`, `kill_current_path`, `repath`, `update_action`/`get_action`, `work` (incl. `check_target_path`, `set_in_danger`), `do_job` (ATTACK, GROUP_ATTACK, GROUP_ATTACK_TO), `do_idle`, `check_idle`, `think` (combat part) | ported except caravan/packer/economy branches |
| `pathfinder.{hpp,cpp}` | `find_wpath`/`find_tpath`/`find_upath`/`find_upath_restore`, `astar_path` (incl. time slicing into `Unit::openlist`), `calc_cost`, `valid_*`, `kill_lists`, `clear_partial_path` | ported; matches `re/tools/astar-ref.mjs` in a standalone self-test |
| `movement.{hpp,cpp}` | `do_move` (incl. moving fire, the ranged stop / retarget rule, `find_new_target` after slow searches), `do_attack_to` (scan / pause), `do_explore_to`/`do_flee_to`, `find_path`, `go_around_building`, `move_step`, `update_speed`/`unit_speed`/`get_speed` | ported |
| `collision.cpp` | `collide_here` (+fill_slots, fast-path stall quirk), `coll_move_unit`, corners, `detect_unit_collision`, `resolve_unit_collision`, `resolve_block` (stub 0), `detect_boat_collision` (stub) | ported except pushers |
| `guys.{hpp,cpp}` | terrain z (float, as the original), `unit_set_new_location`, `unit_place_guys`, `unit_set_angle`, `unit_set_anim`, `guy_init_real`, turning, `guy_move`, `guy_process`, `guy_set_anim` (all RNG sites), `guy_inc_time`, graphics pieces (`unit_gpiece` = `get_unit_gpiece`, `guy_update_gpiece` incl. crew track offsets, `unit_update_gpiece`) | ported |
| `anims.hpp`, `anim_assets.{hpp,cpp}` | `AnimSource` (piece loaded / packet / has / frames / time_ms / loops / track per graphics piece) and `AssetAnimSource` from `unit_graphics.xml`, `anim_graphics.xml` and the `.bha` first-node key times ([anim-spec.md](anim-spec.md)) | ported |
| `groups.{hpp,cpp}` | Group/Groups slot logic, `push_group`, `get_open_slot`, `copy_group`, `groups_process`; actions `move_near`/`move_to`, `halt`, `form`, `stance`; `get_loc`/`get_loc_to`, `set_up_insert`/`finish_insert`; unit side `add_group_move_order`, `do_group_move`, `ungroup_move_order`, `kill_group_move` | ported (army / city / combat branches TODO) |
| `forms.cpp` | `FormData` (the global `forms`, `Game::forms`), `FormData::type_cat`, `Form::categorize` / `compute_rows_and_columns` / `compute_dests` / `compute`, `Group::compute_form` | ported from the asm |
| `search.{hpp,cpp}` | `project`, `ObjectsData::find_unit_with_radius` / `find_unit_ordered_with_radius`, `Objects::find_collision` / `find_ordered_collision`, `UnitType::find_nearby_spot` | ported (filters other than ALL / NOT_ME, building-footprint test TODO) |
| `commands.{hpp,cpp}` | `CommandManager` (issuer: `issue_group`/`issue_move_to`/`issue_halt`/`issue_form`/`issue_stance`/`issue_attack`, production `issue_build`/`issue_swarm_around`/`issue_queue`/`issue_gather_point`/`issue_disband`/`issue_buildmask`/`issue_unqueue`, `process_turn`), `CommandPackage::process` (GROUP semantics of orders-spec §1.2; ATTACK op 4; SWARM_AROUND 6, DISBAND 21, GATHER_POINT 22, QUEUE_UP 24, BUILD 25, BUILDMASK 33, UNQUEUE 48) | done for these verbs |
| `view.{hpp,cpp}` | `make_view(game)` → `GameView {frame, tick, rng_seed, units[], builds[]}`; `UnitView` has who, o, uid, type, color, x/y/z, angle (guy 0), `AnimState`, alive, on_map, packed, group, order, dest, guys[], fog `seen_by` / `detected_by` / `cloaked`, combat `hit_points` / `max_hit_points` / `target_who` / `target_o` / `attacking` / `recharging`; `BuildView` adds `started`, `seen_by`, `now_seen_by`, `ever_seen(_completed)`, hit points (construction hit points while unfinished), target, attacking, production (`job_counter` / `construct_time`, `city_index`, `queue[]` {type, job_counter, time}, `repeat`, `rally[]`, `rally_inside`); `leader_view(game, who)` → `LeaderView` (stockpile[6], pop / pop_cap / control, age, cities / city limit, units / buildings built); `GameView.corpses` (`CorpseView`: dead figure position / angle / death anim / piece / frame / anim length) and `GameView.projectiles` (`ProjectileView`: ammo piece, ballistic position, launch / aim points, flight time, stuck arrows); `make_fog_view(game, who)` → `FogView` (level 0..3 per fog cell) | done |
| `economy.hpp`, `econ_income.cpp`, `econ_gather.cpp`, `econ_market.cpp`, `econ_trade.cpp` | economy ([economy-spec.md](economy-spec.md)): starting stockpiles, income pipeline, gather rates, gatherer lists, GATHER order and handlers, auto-gather, farms, oil wells, markets, tribute, fishermen / merchants, caravans | ported (see "Economy") |
| `capture.{hpp,cpp}` | capture, elimination, victory ([capture-spec.md](capture-spec.md)): `Build::check_capture`, `Cities::capture_city`, `City::capture`, `Build` / `Wall` / `Unit::swap_team`, `Leader::lost_a_city` / `lost_capital` / `recapture_capital` / `defeat` / `defeat_by` / `victory`, `Game::check_victory` / `defeat_all` / `retake_capital`, `process_elimination`, the victory clocks, `GameDaemon::process_victory`, the city's 64-frame capture poll | ported (see "Capture and victory") |
| `script_host.{hpp,cpp}`, `script_funcs*.cpp`, `script_ui.hpp`, `scenario_data.hpp` | scripts ([script-spec.md](script-spec.md) §6, §7): the game's `bhs::Runtime` (engine/script), scenario / general powers / AI production script loading and call points, `ScriptTimers`, find counters, the builtins on the simulation, `ScenarioData`, the presentation queue / seat-local input view | ported (see "Scripts") |
| `spells.{hpp,cpp}`, `transport.{hpp,cpp}` | spells and transports ([transport-spec.md](transport-spec.md)): `add_cast_order`, `do_cast` (untargeted branch), `spell_is_castable` / `pay_cast_costs` / `spell_job_time`, `spell_cast`; the who-may-be-carried predicates, `cast_transport`, `same_damage`, BOARD_SHIP / AWAIT_BOARD (`do_board`, `do_await_board`, `check_meet_ship`), the TRANSPORT / SET_TRANSPORT / BOARD_SHIP group actions | ported (see "Transports") |
| `apps/sim/main.cpp` | `build/ron-sim` harness | done |

Renderer integration: `#include "sim/game.hpp"`, `"sim/commands.hpp"`, `"sim/view.hpp"`, `"sim/anim_assets.hpp"`.
Build the art once with `auto art = load_anim_source(files, rules)` and set `game.anims = art.get()` **before**
`game.load(files, scenario, rules, seed)` (rules after `load_rules` + `start_game`); guys pick their graphics pieces
and draw idle variants during the load. Each simulation step:
`cm.issue_move_to(game, {o...}, x, y)`, `cm.issue_attack(game, {o...}, target_o, target_who)` etc. (set
`cm.play`/`cm.play_who` to the local leader; attack only targets the local leader can see — `seen_by` in the view —
as the original UI does: an unseen target makes the order fail), then
`cm.process_turn(game)`, then `game.do_frame()`; draw from `make_view(game)`. Never draw RNG or mutate state from
the renderer.

## Harness

```
./build/ron-sim <game> <scx> --list
./build/ron-sim <game> <scx> --frames N [--seed S] [--move WHO:o1,o2@X,Y[@FRAME][/form][%queue]]...
                [--halt WHO:o..@FRAME] [--form WHO:o..@FRAME/form[,rotate]] [--dump-every K] [--digest-every K]
                [--no-selfcheck] [--anims] [--no-art] [--reveal N] [--fog WHO] [--fog-check]
                [--attack WHO:o1,o2@TWHO:TO[@FRAME][%queue]]... [--combat-every K]
                [--build WHO:o..@TYPE@X,Y[@FRAME][%queue]]... [--queue|--research WHO:b..@TYPE[*N][@FRAME]]...
                [--rally WHO:b..@X,Y[@FRAME][/action]]... [--swarm WHO:o..@TWHO:TO[@FRAME][/order]]...
                [--disband WHO:o..[@FRAME]]... [--unqueue WHO:b@INDEX[@FRAME]]... [--prod-every K]
./build/ron-sim --dump circle|ring|stamps
re/tools/sim-all.sh <game> [frames] [ron-sim args]    # every stock scenario (156) with the self-check
./build/ron-combat-check <game> --dump out.tsv | --situations out.tsv [--compare ref.tsv]
./build/ron-prod-check <game>    # == node re/tools/production-ref.mjs --examples, byte for byte
./build/ron-econ-check <game>    # == node re/tools/gather-ref.mjs --examples, byte for byte
```

Economy options: `--gather WHO:o..@B[@FRAME][%queue]` (GROUP + GATHER), `--city-gather WHO:c..@TYPE[@FRAME]`
(CITY_GATHER, TYPE -1 = farm / woodcutter / mine), `--buy|--sell WHO:GOOD[*FLAGS][@FRAME]` (lots of 100; FLAGS 1 ×5, 4
×10, 2 all), `--tribute WHO:WHOM@GOOD@AMOUNT[@FRAME]` (AMOUNT < 0: DEMAND_TRIBUTE), `--respond WHO:WHOM@1|0[@FRAME]`
(ACCEPT / REJECT), `--peasants WHO:N[@FRAME]` (LEADER_OPTIONS auto-gather delay), `--econ-every K` (per leader the
stockpile with income / cap and buy / sell prices, every gather building's gatherers, every worker's gather state),
`--starting-resources N` (GameInfo setting before loading). The digest includes the economy fields (leaders'
LeaderDataEncrypt values, deals, oil wells, units' / orders' gather state, buildings' gatherer heads and farm slots,
cities' trade, caravans, farms).

Production options: TYPE is a type name (TYPENAME, else NAME, case-insensitive) or a TypeIndex. `--build` issues
GROUP + BUILD (the citizens place, pay for and build a site), `--queue` / `--research` GROUP + QUEUE_UP (`*N` = num),
`--rally` GROUP + GATHER_POINT (`%1` appends a waypoint), `--swarm` SWARM_AROUND (order 6 build, 13 repair),
`--disband` DISBAND, `--unqueue` UNQUEUE. `--prod-every K` prints each leader's stockpile / population / cities,
every building under construction or with a queue or rally point (progress, queue items with progress, rally
points) and the units created during the run; `--list` prints the production state after loading.

`--attack` issues GROUP + ATTACK on object TO of leader TWHO; `--combat-every K` prints alive units and hit points per
leader, kills, live / stuck ammo and corpses; `RON_COMBAT_TRACE=1` traces every projectile and damage call on stderr
(developer aid, no effect on the simulation). `ron-combat-check` writes the damage table in the format of
`node re/tools/combat-ref.mjs --dump` / `--dump-situations` and compares it.

`--reveal N` sets `GameInfo.reveal_map` (default: the scenario header's value, else 1 = Normal). `--fog WHO` prints the
fog map of that seat at the end (`o` visible, `.` seen before, `-` explored only, `#` unexplored, `@`/`B` own units
and buildings) with its discovery counters; `--fog-check` verifies every refresh (see "Fog of war"). `--dump` prints
the circle / ring tables or every stamp range in the format of `node re/tools/los-ref.mjs dump circle|ring|stamps`.

`--anims` prints the frames per animation of every graphics piece the loaded units use (`*` looping, `!` file
missing) and the gaia animal pieces; `--no-art` runs without animation data (3-frame animations, the old
behaviour).

Per-frame digest = FNV-1a 64 over units (position, angles, masks, orders, path stack, guys incl. animation clock,
`mylos` / `visible` / `infiltrated`), groups, repaths, CollBlock bits, the RNG seed and the fog state (`GameDaemon.busy`,
buildings' flags / LOS / memory masks, leaders' flags / `ally_mask` / explored count / diplos and treaties / oil
patches / new rares, goods and items `ever_seen`, the `seen`, `seen2`, `seen3`, `wcoord_seen` grids, `WData.was_seen`
and the wcoord list heads). The run is repeated from a fresh load and compared (determinism
self-check). Results at this checkpoint (seed 12345):

- tut1, `--frames 600 --move 0:1@8000,12000 --move 0:2,10,13@9000,7000@5 --move 0:20@14000,4000`: Boudicea arrives
  (32 units from the target, frame 151), the three Hoplite batches arrive (frames 201–277). Light Horse 0:20 stops
  3688 short at frame 192 (order dropped; not yet investigated) and has a 125-unit max step at speed 41 (suspicious).
  Self-check OK.
- ceraiding1, `--frames 800 --move 0:0,1,2,3,4,5@38000,26000`: all six Steppe Nomads walk ~9000 units and finish
  around frame 570–620, but **all at the identical position** (38982, 26190) — formation slots are stubbed (every
  slot = the target) and units end up stacked, so unit collision is evidently not separating them. Self-check OK.
- With animation data (the runs above predate it): tut1 `--move 0:1@8000,12000 --move 0:2,10,13@9000,7000@5`
  gives Boudicea at frame 151 and the Hoplites at 203–265 (the RNG stream differs from the 3-frame runs because
  idle variants are re-drawn at the real cycle ends). All 156 stock scenarios load and run 300 frames with
  self-check OK; every unit resolves to an existing graphics piece.
- **Formations and group moves** (with animation data, seed 12345): ceraiding1 `--frames 900 --move
  0:0,1,2,3,4,5@36000,24000` — the six Steppe Nomads (category 5, mounted ranged) form a 3 + 3 line facing the
  travel direction, move in formation (members follow their slots behind the leader, catch-up up to 9 units/frame
  faster) and arrive at frames 205–221 on six distinct slots: leader (36000, 24000), front row ±(216, −312), back
  row (35688, 23784), (35880, 23496), (35496, 24120); all end facing 0x57600000 (the formation angle = travel
  direction). Self-check OK. Forms 1–5, 7, 8 give their shapes (REFUSED / ENVELOP bend the line, echelons slant it,
  WEDGE puts the wedge category in triangular rows, COLUMN three abreast); 9 (MOB) and −1 resolve to the members'
  common form. SQUARE (6) sends every captain to (0, 0): `compute_dests` skips SQUARE captains outside the wedge
  category and the wedge is −1 for SQUARE (faithful; SQUARE is not reachable from the UI, which cycles 0–4).
  QUEUE_FIRST (`%0`) and QUEUE_LAST (`%1`) moves and `--form …/-1` run with self-check OK.
- tut1 with animation data and group moves: Boudicea frame 151, the three Hoplite captains frames 190–205 on
  distinct slots, Light Horse 0:20 stops 3690 short (its target tcoord is impassable, expected).
- `ron-check` still `failures: 0`.
- **Fog of war** (seed 12345, reveal_map 1): tut1 `--frames 600 --move 0:1@8000,12000 --move 0:2,10,13@9000,7000@5
  --move 0:20@14000,4000 --fog-check --fog 0`: Boudicea arrives at frame 151, the Hoplite captains at 190–205 as
  before; Light Horse 0:20 now ends at (13212, 5193), 1981 short, arriving at frame 262 (before: 3690 short at frame
  187): `was_seen` is real now, so `find_wpath`'s fog branch for human owners and `invalid_loc`'s unexplored-tile rule
  apply (expected). Refreshes at frames 33/133/233 clear 49/130/45 stale trail cells, no violations; leader 0 ends with
  491 visible, 382 seen-before and 1431 unexplored fog cells (48 × 48). ceraiding1 `--frames 900 --move
  0:0,1,2,3,4,5@36000,24000`: unchanged arrivals (205–221, same slots), 78 trail cells cleared, fog check OK.
  europe (7 leaders, two alliances): ally masks become 0x3e for the allied five at frame 0; the AI leaders 0 and 6
  collect 7 new rares each by frame 300. All 156 stock scenarios run 300 frames with `--fog-check` and self-check OK
  (no violations once started wonders, visible to everyone, are counted as coverage). Tables and all 1128 stamp ranges
  (LOS 0..140 × move × projected × detector) are identical to `re/tools/los-ref.mjs`. `re/tools/world-compare.sh`:
  156 scenarios, 0 differences.

## What is faithful (from code), what is not

Faithful ports (bugs included where the spec lists them): trig, RNG use, slot allocation (`find_free` incl. forced
slots and crew slots), uid counter, wcoord object lists, footprint stamping, the order list and queue positions,
`update_action`, `work`'s structure, PathFinder at all three levels (cost, tie-break, caps, slicing, emission), find_path,
move_step, collision probes and resolution, guy slots / turning / animation RNG draws, Groups slot management, GROUP
command semantics, GameDaemon repaths decay, region flag shift, coll-block GC, `frame % 15` tick.

RNG draw sites implemented: `Guy::init_real` (1 per guy at creation), `Guy::set_anim` (DEFAULT idle variant,
ATTACK2 variant, gaia bird jog), `do_move` far threshold `% 5`, `astar_path` retry `% 3 + 6` (U-level cap/failure),
`resolve_unit_collision` pause `% 9 + 1`, gaia bird spawn `rnd(xs)`/`rnd(ys)` every 32 frames, `do_group_move`
flock size `& 1` (member whose next point is invalid, within 0x300 Manhattan, on a forest wcoord; the
`Objects::add_flock` it feeds is a stub, see below).

Formations and group moves (`forms.cpp`, `groups.cpp`, `search.cpp`), ported from the disassembly: categories and
spacing (effective type for transports / barges), rows and columns for every form (SQUARE, WEDGE, COLUMN, MOB
included), slot destinations with the single-precision stacking term (`#pragma clang fp contract(off)`), the
recentring (whose lateral term the original computes with radius 0, so only depth moves the destinations), the
formation angle / facing toggle / flip of `Group::compute_form`, the global `forms` scratch (zeroed after every
`action_move_near`), `action_move_near` end to end (split, insert pattern, packer keep-front with `update_gpiece`,
slot clamping and region relocation through `find_nearby_spot`, GROUP_MOVE / GROUP_ATTACK_TO with
`move_id = (id + frame·10)·100 + order_num`, the shared W path `grouppath` and its per-member copy with the
region snap, path inversion), `do_group_move` (leader / member, slot following, catch-up and reduced speeds,
`group.speed` lowering, the flock draw), `action_form` (next / previous / keep, insert pattern, rotation),
`action_stance` (stance types, option cycling, COMBAT side effects), `get_loc` / `get_loc_to` / `get_final_loc`,
`set_up_insert` / `finish_insert`, `find_nearby_spot` (ring and 32-direction probe order, terrain tests,
`find_collision` / `find_ordered_collision` / `find_unit_with_radius` / `find_unit_ordered_with_radius` with the
circle tables and `Game::total_units`), followers placed by `init_unit`, `find_path`'s escape from an invalid
tile.

Stubbed / missing (TODO comments in code, with spec references):
- **Groups**: AI armies (`action_move_near`'s `local_40/44/48` city / garrison branch and `Army` state;
  movement-spec 11.4), combat inside group moves (attacking leader near target, collider → `action_attack` /
  `distribute_attack`, member in range of its target, `cavarch_fight`), `Objects::add_flock` (rate limit, its own
  RNG draws and the gaia units), `LeaderData::current_upgrade` (techs; returns the type), `is_caravan`,
  `is_packing`, `num_inside` (transport), `ObjectData::has_general` (heroes; −1), building stance types,
  `ScenarioData::ignore_orders`, `finish_insert` for non-move orders (build, gather, attack, patrol, ...),
  `Search::valid_filter` for filters other than ALL / NOT_ME, the building-footprint test of `find_nearby_spot`.
  Uninitialised reads of the original are replaced by zeros: the WEDGE row seed, the COLUMN `cols` static
  (members take slot 0's offset for intermediate waypoints), `rows`/`cols` of SQUARE, an empty member path in
  `do_group_move` (reads a stale entry).
- Economy, AI (`think` economy branches, strategy, armies), scenario scripts, building production, garrison /
  transport / ships / air (birds are
  spawned but get no air patrol), packers, caravans, lobby teams / rush rules in the diplomacy defaults,
  territory, leader techs/tribe bonuses in speeds, `Objects::find_units`,
  `find_any_building_at`, calc_markets (draws RNG in the original), herds.
- Animation ([anim-spec.md](anim-spec.md)): lengths, looping and "has" tests come from the guy's graphics piece
  (`Game::anims`, loaded by ron-sim). Open: `has_packet` assumed for every loaded piece, tileset model overrides,
  merchant `is_packing`, and the `update_gpiece` calls of `set_type`, `gain_tech` and `kill_current_order`
  (callers not ported; `Group::action_move_near`'s two calls are in). Leader age: Leader::init's starting age plus the
age techs gained through `gain_tech` (see "Production").

Known deviations: terrain-load RNG seed = GameInfo seed (open question world-spec §13.1);
`unit_is_modern_infantry` always false; `ai_speed` = 1; empty-stack pops return zeroed entries (original reads stale
data); writes through orders killed mid-call are dropped (original writes freed memory).

Spec discrepancies found by the module ports (fix the specs when resuming): movement-spec §7.5 T-level emission
starts at the building node nearest the start (asm @684aa8); §6.3 pull-in "close and final" pushes the goal and
returns; find_upath early returns leave the pathfinder lists (valid cache, block snapshot) for the next search; the
find_wpath "fog branch" is decompiled and implemented (human players); §5 entrenchment clears masks2 0x21000;
§10.4 sidestep sets dest_x/dest_y only; §9.1 group cap tests `!= 0`; §3.5 region rewrite leaves `u.tolerance`;
`GroupData::find_leader` returns `o` in the original (here a list index, consistently); `ungroup_move_order` searches
newest→oldest; Guy::set_anim walk-variant rescale reads the old animation twice; DEFAULT draw also skipped on
group-idle frames.

## Fog of war (engine/sim/fog.cpp, docs/fog-spec.md)

Ported from the decompiles (`re/decomp/fog/`, `re/decomp/fog2/`), in the original's order and integer semantics:
- Tables: `circle_init` with the 0x3249-point cap (ring 64 truncated to 300 points, `circle_radius[64] = 12873`),
  `ring_init` (33 radii, 4781 entries); `stamp_range(los, move, projected, detector)` is the table selection of
  `Object::update_seen` as a pure function, checked against `los-ref.mjs` for every LOS.
- Grids and writers (`World::set_seen / set_seen2 / set_was_seen / clear_seen`, the seen3 memset), every query of
  fog-spec §7.1 with the reveal-map, REVEAL_ENEMY / FOUCHE, ACTIVE_PING and allied-city/fort prefixes. They replace the
  old always-true `was_seen` / `was_really_seen` stubs (so the pathfinder's fog branch is live). `World::fog_changes`
  counts grid changes for renderers (not simulation state).
- LOS: `Unit::update_los` (all 13 steps; rares, tribe bonuses and techs come from the leader state below),
  `UnitData::los` (Ptolemy / CEO live terms behind the `has_general` stub), `Wall::update_los`, Animal = 0.
- Stamping: `Object::update_seen` (visible-wonder test, projection 384 units ahead for small-LOS ground units, rim
  updates on the ring tables, detector bound incl. the r = 32 quirk, infiltration), both `update_local_seen`s,
  `World::reveal_fog` (rare goods → `Leader::new_rare` + `ever_seen`, the first live good of an OIL wcoord →
  `oil_patches`, goody items `ever_seen`, AUTO_EXPLORE units → `Unit::get_goody_box` = one-unit group,
  `action_move_to(QUEUE_FIRST, EXPLORE_TO)`), `update_seen_ally` (starting cities, city radius from
  `LeaderData::get_radius`), `GameDaemon::update_all_seen` (buildings, units, reveal points, frame-0 allied explored
  share; `busy = 4`), `Wall::check_ever_seen` + `Leader::meet` (treaty bit 1 both ways).
- Hooks: `Unit::init` (update_los after hits, update_seen(0) after update_speed, before UNIT_AI_CONTROL), the fog-cell
  change inside `Unit::set_new_location` (rim, or the full disc when teleporting), `Wall::init` (update_los,
  check_ever_seen) and `Build::activate` (ever_seen_completed |= ally_mask, Wall::start's check_ever_seen, update_los,
  update_seen(0); the Space Program sets REVEAL_ENEMY), `wall_start` marks seen2 with was_seen, scenario cities get
  `update_seen_ally`, `Wall::process` every 8 frames (frame & 7 == owner: targeted /= 4, check_ever_seen), step 10
  `Leaders::strategy_all` (check_explore + plan_strategy's ally_mask / meeting part, frame 0 and every 200 / ai_speed
  frames staggered by who·25), the refresh in `GameDaemon::process_all` at frame % 100 == 33 (after the repaths,
  before calc_markets) and once at the end of `Game::load` (Game::run's pre-frame-0 call, frame 0).
- Leader state: `ally_mask` (Leader::init rule with the default diplomacy, gain_tech's ALLY_LOS bits as techs load,
  plan_strategy at frame 0), the scenario's tech list as the tech bitmask, the science epoch (= techs 551..557 held),
  the fog flags from held bonuses, `oil_patches`, `new_rares`, `explored`, `num_units` (per type, for FOUCHE / PTOLEMY /
  THECEO), `city_num`; `ScenarioData::ally_mask` and `reveal_points` data (no script sets them yet).
- Goods and items: global arrays in scenario order (unknown goods skipped), each the terminal of its wcoord object list
  (`WData.down = -2 / -3`, `down_who` = index; a good overwrites an item), as `Objects::init_good / init_item`;
  `find_good_at` / `find_goody_at` walk the list like the original.
- Per object: `is_seen(who, ignore_cloak)` for units (cloak / detection / `visible`), gaia animals (non-herd rule),
  buildings (`visible`, infiltration, `ever_seen`, the single-player city rule), `is_now_seen`, `is_cloaked`,
  `is_detected`, goods and items `is_seen`. Not wired into combat yet (`valid_target_const` arrives with combat).

Stubbed / approximated (TODO in code):
- `LeaderData::has_preq` = every preq passes `has_tech` (TYPE_NONE / goods held, "disable" never; no tribe early
  access, no `TypeData::get_preq` overrides); `type_avail` in `new_rare` = `has_preq`; `has_wonder` = an alive active
  building of that type (no city membership, no CTW wonders); `has_general` and `is_packing` = none; rares
  (`L.rare`) are empty until the economy exists, so furs / dogs / obsidian never apply.
- Territory (`WData.who`, `reg_cities` / `reg_forts`) is not computed: the ACTIVE_PING and allied-territory rules and
  the ACTIVE_PING building reveal in `Wall::process` are dormant. `Leader::set_diplo` / `fix_tech_flags` /
  `gain_tech` (as an event) are not ported, nor scripts (`add_visibility`, reveal points, `set_explored`), nukes,
  herds' `ever_seen` (`Animal::work`), `Build::do_attack`'s 32-frame `visible` clear, `set_attacking` / `do_cast`
  (they set `visible`), `Unit::process_cloak`, the decoy attrition test (`is_detected_by_enemy`) and
  `GraphicEvents::process` (presentation).
- Scenario load order: cities, then units and buildings interleaved, in file order (since the production port).
  Units created inside buildings stamp at their position, like the original (fog-spec §11.6).

## Combat (engine/sim/combat*.cpp, docs/combat-spec.md)

Ported from the spec and the decompiles (`re/decomp/combat*/`, plus `re/decomp/combat_port/` for the helpers decoded
during the port), steps 1–6 of combat-spec §10, every `game_random` site of §8 that the ported paths reach in its
place:
- **Damage** (§5): `get_damage` as a pure core over a `DamageSituation` plus the live-object wrapper; `do_damage`
  (scaling, ZOC, overkill stamp, `target_opportunity`, leader statistics and treaty bits, building collateral);
  `take_damage` (sixteenths, uber thresholds, the 5 % first-hit draw @6520ef and the flock draw @652199, kill credit
  with `get_kill_value` = score value + support costs × `num_units`, decoy overflow). **Verified**: `ron-combat-check`
  equals `combat-ref.mjs --dump` (169,312 pairs) and the new `--dump-situations` (14 flagged situations: splash,
  overkill, side / rear flank, packed, moving, river, rocky, entrenched, decoy, height, unfinished, unfriendly, own
  land; 2,370,368 rows), byte for byte.
- **Death** (§6): `Object::die` (ammo hold), `Unit::close` (uber-chain relink with the dead member re-appended at the
  tail, unpacked merchants' 2×2 block release, the death-animation draws @60fae6 / @60fb24 / @60fb42, `kill_guy` per
  guy), `Object::close` (blocks-while-dead types stay linked; `damage = 0`), `kill_guy` (footprint clear),
  `add_death` (100-slot pool, oldest non-blocking reuse), `DeathObj::inc_time` (hold_frames per corpse, lifetime
  anim + 135 / + 627 with a skeleton, ships anim only). Skeleton / scorch / release events / the AMMO table come from
  `AnimSource` (`AssetAnimSource` reads them from unit_graphics.xml and effects_graphics.xml).
- **Range, validity, acquisition** (§2, §3.6): `attack_dist`, `is_in_range` (3 forms), `valid_target_const` /
  `valid_target` on the fog module's `object_is_seen`, `check_target`, `poor_target`, `compare_target`,
  `find_nearby_target` (circle spiral, newest first, near memo, early stop after 10 units, strict `>`),
  `find_melee_target`, `think_attack`, the combat part of `think`, `find_new_target`, `target_opportunity`,
  `Group::target_opportunity`.
- **Orders** (§3): `add_attack_order` / `find_def_pos` / `get_activity`, `do_attack` (non-combatant group members,
  fire-while-moving cache, the MELEE_AND_RANGED range swap through `raw_max_range()`), `check_target_path`,
  `find_attack_pos` (perimeter walk with the `% 192` draw @602107, melee slots, lead / hold / ring, fallback),
  `change_target`, attack-move scans and pause, `Group::action_attack` / `distribute_attack` / `is_attacking_near` /
  `is_attacking_to`, `do_group_attack`, ATTACK in `finish_insert` (🟡 re-issued as a QUEUE_LAST group attack).
- **Firing** (§4): `fight` (all branches incl. the guard draw @5fdcdb, re-evaluation draw @5fde6e, the AA
  `rnd(100)`, wall facing, sideways ships, animation choice → `Guy::set_anim` variant draws, melee damage, direct
  fire, Immortals, fireships, recharge, chase / give-up / leash with the approach-flag `work()` re-run),
  `cavarch_fight` (called from `do_move` / `do_group_move`), `set_attack` / `set_attacking` (with fog
  `update_local_seen`) / `set_in_danger`, `fire_ammo` (building footprint draws @64cc73 / @64cca4), `add_ammo`
  (lowest free slot, 200 at start, counter), `Ammo::init` (air rolls, scatter draws, flight time, lead, single
  precision), `inc_time` (miss extrapolation, stuck arrows for 200 frames), `hit_target` / `check_hit`, `do_damage`
  (miss jitter draws @678ca7 / @678cc4, splash), `Guy::execute_events` release events (event-firing units shoot at
  `max(1, starttime / 67)` of their attack animation), `Build::process` attack part / `Build::do_attack` /
  `find_target` / garrison arrows, `fire_proj` decided from the art at load (`init_combat_data`).
- **Hooks**: collision (fireships ramming, the attacker's beyond-range soft rule, retarget on an enemy collider),
  `do_group_move` (enemy colliders → `action_attack` / `distribute_attack`, members in range, moving fire),
  `Leaders::process_all`'s per-frame `retarget_budget = 0`.

Checks (seed 12345): ceraiding1 (Chess Exercise), six Steppe Nomads moved within sight of the Inca Impi
(`--move 0:0,1,2,3,4,5@25200,34200 --attack 0:0,1,2,3,4,5@1:54@560`): the nomads shoot (release events, ArcherArrowWOtip
at frames 16–19 of their 25-frame attacks), the Impi and the nearby Inca building answer, overkill and flank terms show
in the damage trace, all six nomads die by frame ~1000 after killing 3 Impi members, the corpses (13) expire between
frames ~1300 and 1650 (with main's market draws in the stream); self-check OK. `--attack 1:72,77@0:0` (melee Light Cavalry) deals 12 per blow every 25 frames.
With the map revealed (`--reveal 3`) the attack works from frame 0. All 156 stock scenarios run 300 frames with the
self-check and `--fog-check` OK (6 show combat activity from idle units engaging enemies in sight).
`ron-check` failures 0, `ron-rules` still byte-identical.

Stubbed / approximated (TODO in code, with spec references):
- Leader-dependent stat terms (nation powers, heroes / generals, rares, wonders, techs; combat-spec 5.1, step 8) are
  ported in `engine/sim/stats.cpp` (combat-spec 5.1, 2026-10-23): attack, armor, hits and range from techs, wonders,
  rares and nation powers; the hero and garrison terms are written behind the `has_general` / `find_hero` /
  `count_inside` stubs. `ron-sim --list` prints hits / attack / armor / range per unit. Healing, member
  regeneration, supply and attrition (step 7): see "Healing and attrition".
- Territory (`WData.who`) is not computed, so: citizens / militia deal half damage to every building, buildings never
  count as being in unfriendly territory, and `check_target` rejects every building without BUILDTYPE_OUTSIDE (no
  man's land) — idle units never auto-acquire buildings (explicit attacks work). Unsupplied siege fires 1.5× slower.
- Garrisons (`count_inside` = 0, no
  ejection; city capture is ported, see "Capture and victory"), packers (no PACK / UNPACK casts; packed units fire as they are), ATTACK_GROUND, AI armies
  (`add_to_army`, army targets, emergency), plunder (draws), flocks (`add_flock` draws after the 5 % roll), AI war
  declaration and peasant alarm, jamming, the carrier launch sequence, aircraft (crash ammo draw @67baf7 not made),
  splines (rockets, torpedoes, missiles fly with the plain flight time), muzzle offsets and pivot restrictions (shots
  start at the guy), nukes, broadside (unused by the data), air-defence turrets, strafing / bomber impact rules,
  spells (bribe / sabotage reactions), the global of `Object::die`'s ammo hold (taken as 0).
- `ObjectsData::find_unit` (Ammo::check_hit) scans leaders 0..8 in slot order for the strict nearest unit
  (TODO: the original's table walk); `find_building_at` scans every building.
- fire_proj is taken from the first leader's piece of each type (variants agree in the shipped data); release events
  come from the guy's own piece (the original's civ / age event groups are not modelled separately).
- Building death leaves the footprint's grid tiles as they were (Build / Wall close are not ported beyond
  `Object::close`).


## Garrisons

`engine/sim/garrison.cpp` / `.hpp` (docs/garrison-spec.md, 2026-10-23): the container links (`unit_get_inside`,
`unit_insert_inside`, `unit_remove_from_inside`, with the leader's "inside" counters), `num_inside` / `count_inside`
(the PDB's CountIndex), `can_carry_domain`, `type_can_garrison` (the 'l' / 'm' flags and the trainer families),
`unit_can_garrison`, `find_garrison_build`, `object_adjacent_to`, the GARRISON order (`add_garrison_order`,
`kill_garrison_order`, `do_garrison`), `group_action_garrison` / `group_action_eject_all` behind the GARRISON (20) and
EJECTALL (26) commands (`CommandManager::issue_garrison` / `issue_eject_all`), `eject_contents` / `kill_contents`,
`build_process_ejection` (active and dead buildings), the garrisoned branch of `unit_process` and the garrison branch
of `process_healing`. Hooked: `take_damage` / `wall_update_hits` (a destroyed building ejects), `object_close`,
`capture.cpp` (`swap_team`, `blow_up_units`, the strength scan), `build_train` / `come_out`'s rally branches, the
scenario loader (contained units join the chain), the scenario builtins `unit_garrison_order` /
`group_garrison_order`, `ronmap`'s right-click on an allied building. `ron-sim --garrison WHO:o1,o2@BO:BWHO@FRAME`
and `--eject-all WHO:o@FRAME[/1]`; `--list` prints `inside N (pop M) arrows A` per building.

Checked: europe.scx, three Riflemen into a Fortress (arrows 2 → 5, `inside 3`), EJECTALL brings them out next to it;
capture.scx Light Horse into a Stable and out with cleared orders; a full Fortress (limit 10) refuses the 11th and
12th; citizens of a Major City keep their gather orders and resume after `/1`; a destroyed Fortress ejects one chain
per frame; a unit heals inside; all 156 stock scenarios 600 frames with the AI and the self-check OK.

Stubbed (TODO in code): `is_entering_or_exiting`, aircraft (helicopters on carriers, airbase scramble), the militia disarm spell on leaving, the scenario ignore-orders
prelude, the Lakota / American gather bonuses from the "inside" counters, fire rafts / aircraft ejecting a garrison.

## Transports

`engine/sim/spells.{hpp,cpp}` and `engine/sim/transport.{hpp,cpp}` (docs/transport-spec.md, 2026-10-24): a land unit
whose owner may transport it (`UNIT_CAN_TRANSPORT`, from `Leader::check_transport`) walks a path with flag 4 across
water; there is no ship to build.
- `Unit::set_new_location` (guys.cpp): on entering a water tile the unit does **not** move: `add_cast_order(0x28a,
  QUEUE_FIRST)` and a return of 0 (`do_move` treats 0 as "did not move"). A sea unit with `unit_flags & 0x10` (the barge)
  placed on a non-water tile ejects its contents (`eject_contents(0, -1, carry_orders 1, no_orders 1)`), dies when
  empty and does not move either.
- `do_cast` (untargeted branch): pays the cost (`pay_cast_costs`, free for 0x28a), first frame `set_anim` and the
  "is there a water spot for a barge within `unit_board_distance`" test, `spell_job_time` (0), followers wait while their
  captain is casting, then `spell_cast` -> `cast_transport`: a Transport Barge (the current upgrade; Merchant Fleet for
  caravans; marines mark it `kMarineTransport`) is created on the unit, takes its damage ratio (`same_damage`), is teleported
  to the water spot, receives the unit's whole order list and path (the next point loses flag 4 when it is in the barge's
  region) and `UNIT_CAN_TRANSPORT`, and the unit (with its `o_down` chain) goes inside (`unit_go_inside`; into a unit
  every member of an uber chain also drops its orders).
- The barge walks the path (passability already lets a sea unit with the flag onto land on a flag-4 step; its speed gets
  `military_transport_bonus` x the leader's military epoch). Colliders wait for a barge that is casting (collision.cpp).
- Landing: `eject_contents`, unit-source branch (garrison.cpp): `same_damage` (the cargo takes the carrier's ratio when
  higher), caravan slot hand-over, `carry_orders` (the order list and the path go to the unit, the next path point loses
  flag 4), the transport flag is re-granted from the leader's level. The caravan (`cara`) record follows the unit.
- Commands (commands.hpp): TRANSPORT (13, `Group::action_transport`: barge now, unit inside), SET_TRANSPORT (14,
  `Group::action_set_transport`: sets / clears the flag when the leader has a transport level), BOARD_SHIP (15,
  `Group::action_board_ship` -> BOARD_SHIP / AWAIT_BOARD orders, `check_meet_ship`). `CommandManager::issue_transport /
  issue_set_transport / issue_board_ship`. `RON_TRANSPORT_TRACE=1` prints every cast order, barge, landing on stderr.

Crossing test: unit 1:54 (a citizen of the Persian AI, owner with TRANSPORT_BONUS and docks) walks to a land tile across the
strait of persia.scx:
`RON_TRANSPORT_TRACE=1 build/ron-sim <game dir> conquest/Alexander/persia.scx --frames 700 --no-scripts --no-selfcheck
--digest-every 0 --move 1:54@5200,3500@1 --dump-every 20`. The trace shows `f352 L1 unit 54 enters water at 10544,3438 ->
cast order`, `f353 cast_transport ... barge 7 (type 320)`, `barge 7 takes 1 orders (front 1), path 29`, `f601 barge 7 leaves
the water at 5375,3480`, `f601 landing L1 unit 54 ... 1 orders (front 1), path 2`; the dump shows 1:54 `INSIDE(on_map=0)` from
f360 to f600, then at (5200,3525) (its target) on the far shore at f620 (the AI then gives it new orders). A driver that
damages the unit first (damage 30/50, moves queued: MOVE_TO(5200,3500) then MOVE_TO(5200,4300)) shows the ratio and the
order list surviving: unit 23/50 at the cast, barge 22/50, unit 23/50 and both MOVE_TO orders after the landing.
`--roundtrip 400` on the first line prints `save round-trip: OK` with the unit inside the barge.

What is simulated: everything above for single units and uber chains (a chain crosses as one cargo, the captain casts), the
three commands, the damage hand-over both ways, caravans (Merchant Fleet) as far as the record hand-over goes. All 156
stock scenarios run 600 frames with `--ai-every 8` and the self-check OK; five digests moved against the build before the
port, all of them an AI unit now crossing water in a barge instead of walking on it (persia, persia2, carib, carib2,
italy_mp).

Differences from docs/transport-spec.md (the exe wins): `set_new_location` returns 0 right after `add_cast_order` (the
disassembly at 5f8f77 xors EAX and returns; the barge-leaving branch also returns 0 when contents remain);
`can_ever_transport` is true for a non-land unit with `carry != 0` that is **not** a carrier (5f2f5 returns 1 on `is(0x15f)`
== 0); `is_castable` returns 0 (not 1) for decoys; `do_cast` kills the order when `pay_cast_costs` fails; the military
speed bonus multiplies `LeaderDataEncrypt.epoch[0]` (military library level), not the transport level;
`collision.cpp:382` is the pack-spell soft collider, not the 0x28a wait (that is `:535`).

Stubbed (TODO in code): the targeted branch of `do_cast` and every spell but TRANSPORT (`TODO(spells)`), the group machinery
for chains landing from a barge (`TODO(groups)`: `reset_move_orders` + the temporary group; the orders and path go to the
captain instead), `is_entering_or_exiting` / `unpay_cast_costs` in `kill_current_order`, aircraft cargo in `can_carry`,
the AI's ferrying (`think_civilian_transport`, `do_transporting`), sounds, the console selection and `replace_hotunit`, the
`ron-sim` options and the right-click verbs. ❓ `check_meet_ship` follows the disassembly but is exercised only by
BOARD_SHIP (no stock scenario issues it); `action_board_ship`'s QUEUE_FIRST re-insert runs `finish_insert` on the group (the
decompile uses a temporary group holding the ship).

## Healing and attrition

`engine/sim/attrition.cpp` (combat-spec 7, economy-spec 7.2-7.3, 2026-10-23):
- Leader side: `leader_calc_attrition` / `leader_calc_anti_attrition` (`att`, the float `anti_att`) run in
  `Leader::init` (game load) and `calc_unit_stats`; `get_heal_level`, `get_supply_upgrade`.
- Supplies: `supplies_init_supply` / `close_supply` / `find_supply` (`Game::supplies`, `Leader::supply_mark`),
  registered from `unit_init`, `unit_close` and `unit_set_type`; `unit_in_supply` now drives the siege reload.
- Unit side: `process_attrition` every 32 frames, the attrition tick in `unit_process` (`process_supply` or
  `suffer_attrition`, ATTRITION_SAFE), `unit_get_attrition` (SSE single-precision multiply).
- `process_healing` (ships with Citrus, carriers, Iroquois, supply wagons with the French bonus / Versailles,
  civilians) and `unit_repair_damage` with dead-member regeneration (`find_free` on the dead tail slot, `unit_init`,
  links restored, a walk-out move, the unit's Group refreshed). The scripted heals (`set_object_health`,
  `heal_object`) now go through it too.
- Scenario switches: enable / disable take and give attrition, set / clear attrition-free points.
- `RON_ATTRITION_TRACE=1` prints every attrition decision on stderr.

Checks: `europe.scx --no-scripts --move 0:42@33000,33000@1` (a Soviet tank crossing German land at peace): period 8,
ATTRITION | SEVERE_ATTRITION, exactly 1 hp every 8 frames while inside, nothing on neutral land. All 156 stock
scenarios run 600 frames with `--ai-every 8` and the self-check OK.

Stubbed (TODO in code): garrison healing and `insert_inside` (no garrisons), generals and the CtW hero (`has_general`
is -1), CtW heal and attrition cards, the assassin target (team style 2 counts as plain war), the gathering-worker
exemption byte, the console selection / hotkey group refresh after a regeneration.
## Production (engine/sim production files, docs/production-spec.md)

Ported from the spec, the decompiles and the disassembly (production-spec §13 steps 1-11). Entry points:

- **Leader state** (`game.hpp` `Leader`): stockpile `bucket[6]` / `escrow[6]` (plain values; the ^0x8221 is a save
  detail), ages / epochs / discovered / `epoch[4]`, `tech` / `tech_at_start` / `obs_flags`, `num_queued`,
  `num_buildings` / `high_buildings` / `reg_buildings`, per-class unit and queue counters, pop / pop_cap / control /
  active / caras / pop_issues, city / fort / wonder marks, gather slots, `reg_terr`, `territory(_high)`. `City`,
  `Fort`, `Wonder`, `Unbuilt` records in `Game` (`cities[8]`, `forts[8]`, `wonder_records[8]`, `unbuilt_*[8]`).
- **techs.cpp**: `has_preq` / `special_preq` / runtime `get_preq` / `has_tech` / `type_eligible` / `type_avail` /
  `tribe_can_type` / `get_graft` / `current_upgrade`, Leader::init's tech part (`leader_init_techs`, incl. the tribe
  free epochs), `gain_tech` (§7.1 steps in order, free-upgrade loops, unit-line conversion via `Unit::set_type` with
  its Guy::init_real draws, building upgrades, ages), `calc_pop_cap`, `track_unit_type` / `track_queued`,
  `calc_unit_stats` / `calc_wall_stats` (consumed in `Leaders::process_all`).
- **costs.cpp**: `get_cost` (all branches incl. unusable-resource conversion), `can_pay_cost`, `pay_cost`, `unpay_cost`,
  `train_time`, `type_time` / `type_research_time`, `update_construct_time`, `construct_time`. `ron-prod-check` equals
  `production-ref.mjs --examples` on all 38 cases.
- **build.cpp / build_place.cpp**: `init_build_site` (Objects::init_build → Build::init → Wall::init, incl. the
  `find_gather_tiles` shuffle), `build_start` (kill_competing_buildings), `build_activate_full` (Build::activate +
  Wall::activate: stats, cities, forts, wonders and the wonder race, free units of a new high-water mark),
  `build_close_production` (Build::close + Wall::close), `build_disband` (single-precision partial refunds),
  `wall_update_hits` (construction hit points), `blocked_site` / `blocked_tcoord` / `blocked_location`,
  `snap_center_who`, `find_building_placed_at`, `max_gatherers`.
- **cities.cpp / territory.cpp**: city records, membership (`find_city`, `add_to_city`, `remove_from_city`,
  `find_buildings`), radius / level / pop value, `check_upgrade`, `City::close`, capital rules, `mask_city`, forts,
  wonder records; `check_borders` (256-wcoord budget per frame) driving `compute_reg_territory` (decoded in this
  port, production-spec §5.10.1) and `compute_all_territory` (twice at load).
- **queues.cpp**: `BuildQueue`, `group_action_queue_up` (sort by queued, rounds / research passes), `build_queue_up`,
  `can_make` / `could_queue` / `queue_here`, `build_do_queue` (libraries in parallel, pop-capped heads, repeat),
  `build_finished`, `build_train`, `unit_come_out` (spawn rings, rally points and waypoints, final-point attack /
  build / repair, AI army draw), `build_unqueue` / `build_action_unqueue` / `build_clean_queue` (refunds), gather points,
  BUILDMASK, `build_new_library`, `unit_set_type`, `wall_set_type`, `unit_go_inside`.
- **construct.cpp**: `group_action_build`, `group_action_swarm_around`, `add_build_order` / `add_repair_order`,
  `do_build`, `do_construct` (helpers / IN_USE, start-or-disband), `do_repair` (rate and float pay steps),
  `check_build_order`, `build_done`, `find_build_spot`, `build_process` (Wall::process: decays, AI wonder top-up
  count, helpers latch, territory attrition; Build::process: ejection, attack, do_queue, gather tiles on region flag
  0x10, Kremlin / gov hero / Persian caravans / Terracotta, city steps, roads), `wall_inc_time`,
  `group_action_disband`.
- **Scenario load** (`Game::load`): GameInfo from the header; leaders via Leader::init + record + child chunks in file
  order (techs and types through `gain_tech`, diplomacy, stockpile); cities and objects interleaved in file order
  (`Scenario::seq`; cities chunk first) with `scenario_loading` (the world loader already applied the grid effects);
  active buildings `activate(0, 0, 0)`; queues via `queue_up` + `unpay_cost`; `compute_all_territory` after the
  objects.

Not ported (TODOs with spec references in the code): gathering and everything economy-side (gather orders, gather
slots' do_bonus, farms, oil wells, docks' transport records, caravans), garrisons (`num_inside`, eject / kill
contents, gather-inside rally points), aircraft (airbases, carriers, BUILDAIR), spells (owned-spell costs, casting from
queues), assimilation by THECITIZEN (heroes), AI-only paths (orphaned sites, recruiting via `find_unit`, auto
repair), terraforming at placement, `verify_gather_tiles`, `find_repair_spot`, Objects::find_builds (approximated by
the leader's unfinished sites in slot order), FILTER_BUILDREPAIR (unfiltered), government-hero healing,
rush-rule `war_allowed` in territory attrition, ScenarioData flags (`buildings_free`, `building_unit_bonus`).

Results (seed 12345, tut2 = `scenario/Tutorial2/tut2.scx`, leader 0 British, Classical, 3000 of each resource):
- `--build '0:9@Barracks@13056,17280'`: timber 3000 → 2880 at frame 0 (120, the 0-owned Barracks price), the citizen
  walks there and starts building at frame 182; +100 per frame (one builder), construction hit points grow from 1 to
  1200; the Barracks completes at frame 602 (42000 / 100 = 420 frames) and the builder's order ends.
- `--queue '0:2000@Phalanx@650' --rally '0:2000@12000,17000@650'` on that Barracks: 48 food + 28 metal paid; the
  3-man batch (uber_size 3) appears at frame ~877 (train time 22560 at accel 100), walks to the rally point with
  ATTACK_TO and engages the Vikings.
- `--queue '0:2010@Citizen*2' --rally '0:2010@12000,17000'`: two citizens (27 + 28 food), trained by frames ~106 and
  ~224 (the second's time drops when a citizen dies), walk to the rally point. `--build '0:0,1,2@Library@10848,22752'` builds a Library in ~280 frames (three
  builders: 100 + 50 + 33 per frame); `--research '0:2000@Mathematics@701'` costs 120 wealth + 80 knowledge and
  completes after 27500 / 100 frames; Medieval Age is refused (the scenario's end tech is 1).
- `--disband '0:2002' --disband '0:5@5'`: the Farm demolishes after its DISBAND item (7500), the Light Horse dies.
- All 156 stock scenarios: 300 frames, self-check and `--fog-check` OK; `world-compare.sh` 0 differences;
  `ron-combat-check` identical to `combat-ref.mjs`; `ron-prod-check` identical to `production-ref.mjs --examples`.

## Economy (engine/sim econ_*.cpp, docs/economy-spec.md)

Ported from the spec, the decompiles (`re/decomp/econ*`, plus `re/decomp/econ_port/` for the helpers decoded during
the port) and the disassembly where the decompile lost arguments (economy-spec §12 steps 1-13). Entry points in
`sim/economy.hpp`:

- **State** (`game.hpp`): the LeaderDataEncrypt fields as plain values (`leftover`, `resource_cap[7]`, `over_cap`,
  `resources` = the rate, `support`, `income`, `rate`, `bonus`), `escrow_rate`, `tributes`, `base_rate`, `collected`,
  `bonus_cap[7]`, `rares_collected[44]`, `known_rares`, `gather_stamp`, `rare_owned`, leader options (`opt_peasants`,
  `opt_peasants_wait`, `opt_buildings`; LeaderOptions::init defaults 0 / 2 / 0), diplomacy deals (`dip[8]`: agree,
  any_offer, treaty, offers, dows, attacks), tribute statistics, the OilWell records; `Game.farms` (FarmStruct),
  `Game.caravans`, the Doober hold slots, `world_total_land` (World::analyze_map's land_size), `buildings_gather`;
  GatherOrder / TradeOrder fields in `Order`; Unit +0x54 `rare`, +0x86 `doober` / `cara`, AnimalData ox / whom / aid.
- **Starting stockpiles** (econ_income.cpp): `init_starting_resources` (S1 / S2 draws; Game::load runs it after
  seeding and before the load-time reseed, like Game::run → init_rules_and_teams → Setup::build_game, so the draws
  never reach the simulation's stream), `leader_init_economy` (Leader::init: every slot resets the arrays with caps
  at commerce_cap[0]; scenario leaders then get escrow_rate 40 with more than two cities and bucket_add of the
  starting amount per available good, the Greek / Persian rules, Infinite). The scenario's chunk 44 then overwrites
  the listed goods, as before. GameInfo.starting_resources stays 0 (Low: half of starting_goods) unless
  `--starting-resources` says otherwise (TODO: the lobby / scenario setup value).
- **Income pipeline**: `Leaders::process_all` per ACTIVE leader in order: counters, `leader_gather` (Leader::gather:
  calc_gather with the cadence of §3.1 and LEADER_RECOMPUTE as the dirty flag, the rare masks, support = 0,
  `leader_calc_resource_caps`, `leader_do_gather`), then CALC_WALL_STATS / CALC_UNIT_STATS, then IDLE_PEASANT cleared.
  `gather_frame` is do_gather's arithmetic (cap, Dutch interest, handicap, tech_cost, ×1.5 rules, ai_speed, the
  /7200 carry, AI escrow dither); `get_gather_handicap` as decoded.
- **Rates**: `build_type_calc_gather` (BuildTypeData::calc_gather: the flat / university / mine / radius scans over the
  live world, then the formula layer `gather_flat` / `gather_university` / `gather_mine` / `gather_radius_path`),
  `build_calc_gather` (BuildData::calc_gather with num_gatherers(1, 1)), `max_gatherers` (production's entry point now
  runs the real preview); cities: `city_calc_gather` (enhancer bytes) → calc_city_resources → `city_income_tail`;
  leader: `leader_calc_gather` (Porcelain rares, basic gather, Lakota / Americans, cities, oil wells, out-of-city
  woodcutters / mines, idle fishermen / merchants, `leader_income_tail`, rare-change bookkeeping, calc_resource_bonuses,
  obsolete goods), `calc_rare`. **Verified**: `ron-econ-check` equals `node re/tools/gather-ref.mjs --examples` byte for
  byte (E1-E12).
- **Gather tiles**: production's `find_gather_tiles` (shuffle) unchanged; `build_verify_gather_tiles` on region flag
  0x20 and `find_gather_tiles` on 0x10 in Build::process.
- **Gatherers and the GATHER order** (econ_gather.cpp): `build_add/remove/check_gatherers`, `build_all_gathering`,
  `build_num_gatherers` (scholars / platform citizens inside), `unit_is_gathering_at`, `add_gather_order`, the
  kill_current_order hook (`gather_order_killed`), `group_action_gather` (the 121-step spiral, members in reverse,
  idle-worker rule, the move of the rest), `group_action_city_gather`, commands GATHER (19) / CITY_GATHER (18).
- **Handlers**: `do_gather` (registration, re-think tails, farm / oil / university paths, G3 / G4, AI repair, CHAR_*),
  `do_non_flat_gather` (tile choice with the round robin, G5 / G6 / G7, chop / dump cycles, held doobers, give-up
  rules), the do_move GATHER reset (tile given up when the time-sliced search keeps failing near it).
- **Auto-gather**: `think_economy` (Unit::think after the combat part: think_peasant, think_caravan, human rare
  collectors' re-check, AI fishermen / merchants), `think_peasant`, `find_gather_spot`, `do_think_order`; production's
  build_done / do_build / do_repair / come_out gather rules now give the orders.
- **Farms**: `farms_add` (pen / wheat choice with F1, ambience F2), `farms_inc_time` (G9, after the corpses in
  Objects::inc_time), `farms_grow` / `farms_snip`, `farms_add_animals` (F3; gaia pigs / chickens through init_unit),
  removal and the animals' death in Build::close; oil-well records on activation / close.
- **Markets and tribute** (econ_market.cpp): `calc_market_prices`, `can_buy_sell`, `do_buy` / `do_sell` (escrow −100
  sic), `action_buy` / `action_sell` (BUY 46 / SELL 47), `scale_tribute`, `action_offer` (TRIBUTE 43 / DEMAND 44, the
  any_offer quirk), `clear_agree`, `action_clear_tributes` (39), `action_respond` (ACCEPT 41 / REJECT 42: the resource
  paths incl. the DoW cost "last third party wins" and the B.dip[B].dows clear quirks).
- **Fishermen, merchants, caravans** (econ_trade.cpp): `unit_collect` (UnitData::calc_gather over tiles with the
  upgrade_level radius, crowding), `think_fish` (G8), `think_merchant`, caravan records, `trade_value`,
  `city_compute_trade`, `city_new_caravan`, `city_get_empty_trade_routes`, `add_trade_order`, `do_trade`,
  `think_caravan`, `end_trade_route`, `group_action_trade` (TRADE 17), LEADER_OPTIONS (73).
- **Views**: LeaderView gather_rate / income / cap / over_cap / buy / sell / can_trade; UnitView gather_who /
  gather_o / gather_arrived / carrying / collecting; BuildView gatherers / gatherers_working / gatherers_max.

Checks (seed 12345): tut4 (British, 3 woodcutters, 9 miners, 5 farmers): metal +1440 per 450 frames (9 × 160, +90
per 450 frames observed), timber +640 (3 × 160 + the city's 160), food +1120; `--gather '0:0,8@2012@10'` sends two
farmers to the second mine (6/10 gatherers, metal +1760). Miners stay at their ore (wait 1000000), woodcutters chop
and walk back when every gatherer is busy. tut2: idle citizens auto-gather onto free farms within frames; the AI
leader's food +720 pays 46 per 500 frames (handicap −7 %). caribbean: `--buy '0:1*1@1'` buys three lots of timber
(114 + 120 + 126 wealth, the price rising by 2 × market_supply_demand per lot) until wealth runs short, `--sell
'0:0@2'` sells 100 food for 65. All 156 stock scenarios run 300 frames with the self-check OK; world-compare 0
differences; ron-combat-check, ron-prod-check, ron-econ-check identical to their references; ron-rules
byte-identical.

Stubbed / approximated (TODO in code, with spec references):
- Caravan roads (Caravan::build_road, PathFinder::find_road, verify_road, Caravans::new_danger): without a road the
  original's do_trade heads to the building it is not testing, so routes never complete; `Unit::work`'s road resume.
- Packers (merchants' UNPACK casts, unpack_merchant, is_unpacking): merchants stay packed and produce nothing.
- AI: random_personality / the coin flip in Leader::init, think_civilian_transport, think_scout, think_carry,
  armies, use_market / market_speculation, consider_tribute, City.gatherers / free (AI sites; 0 for humans).
- Diplomacy branches of action_respond (treaties, alliances, declare war), set_diplo; the leaders[whom] +0xb4 flag.
- Transports: UnitData::transport_type / can_ever_transport (land citizens count as transportable civilians),
  can_carry for oil platforms, citizens carried by transports in GroupData::count; the civilian spell 659.
- ObjectsData::find_any_building (Farms' nearest farm type) and find_unit / find_unit_ordered filters (think_merchant)
  are plain scans; CityData::is_seen taken as true in FILTER_CAN_TRADE; the danger map is zero (calc_danger TODO).
- Build::do_bonus resource bonuses outside scenarios; garrisoned unit counters of the Lakota / American terms;
  cliffs (no stock map has them); Doober hold slots free at once (the original frees them after a render-side fade).
- Leaders::process_all's flag 0x40000 and taunts; scripts (buildings_gather defaults on).

## Scripts (engine/script, engine/sim/script_host.cpp, docs/script-spec.md)

BigHugeScript is ported in `engine/script/` (no game data in the repo; see the file headers):

- **Compiler** (`bhs_lexer.cpp`, `bhs_grammar.cpp`, `bhs_parser.cpp`, `bhs_codegen.cpp`, `bhs_symbols.cpp`): the
  scanner by hand (CRLF line breaks, case-insensitive identifiers, the literal rules of script-spec §1); the bison
  LALR(1) tables are **generated at start-up from the spec grammar** (`kGrammar`, rule numbers = the original's; 392
  states, the dangling-else shift/reduce as the only conflict, bison's default reductions and lookahead timing), so
  no table from the exe is embedded; the parser actions build the symbol tables and generate code bottom-up per
  reduction exactly as the original's `SyntaxNode::eval` does; `write_file` produces the chunk image (links, line
  table, symbol tables, code). `bhs_image.cpp` reads images back and disassembles them.
- **VM** (`bhs_vm.cpp`): values with TEMP / CONST / VAR scopes, run-to-completion `run_script` (0x1000000 instruction
  and 0x10000 frame limits, run-time errors keep side effects), case-insensitive strings, f32 arithmetic with
  `fp contract(off)`.
- **Builtins** (`bhs_builtins.cpp`, `bhs_builtin_table.inc`): all 873 registrations in order (global index = CALL_GAME
  operand) with signatures; `bhs_stdlib.cpp` has MathUtil / TriggerUtil / StringUtil / ArrayUtil.
- **Simulation hooks** (`sim/script_host.cpp`): `script_host_load(game, files, scenario_path, scenario)` after
  `Game::load` creates `Game::scripts` and compiles the scenario script (scenario_flags & 1: `<dir>/<base>.bhs`, run
  as `<base>`), `general_powers.bhs` (scripted rules / CTW) and `ai/scripts/<name>.bhs` per AI leader.
  `Game::do_frame` step 5-6 runs the scenario script every frame and general powers when frame > 0.
  `Leader.production_ai` is the +0x788 state machine: plan_strategy's full pass ends with state 1, and while it is
  non-zero each frame's plan_strategy runs one `leader_production_ai` step: state 1 calls
  `run_script(name, who + 1, ref step, boom_vs_rush + 2, 5)` (return 1 → state 0, 3 or failure → script retired),
  states 2-11 are the C++ production steps (TODO(AI): `production_ai_setup`, `found_cities`, `research_techs`,
  `upgrade_units`, `create_units`, `create_buildings`, `make_stuff` do nothing yet). `leader_init_production` picks
  the script (`economic` / `defensive`) without random_personality and without the boom_vs_rush == 0 coin flip
  (TODO(AI), see the header). Without `Game::scripts` (the check tools) state 1 counts as a failed run and nothing
  else changes.
- **Builtins on the simulation** (`sim/script_funcs*.cpp`, 2026-10-13): 472 of the 813 builtin names (873
  registrations with overloads) have a body, about 99 of them from the AI-script work. By the table's categories:
  162 queries, 144 mutations, 125 presentation, 40 script environment, 1 command. The files:

  | File | What |
  |---|---|
  | `script_funcs.cpp` | game_random `rand_*`, timers, script UnitGroup structs, settings, the AI production builtins, the `find_*` searches with `ScenarioData::find_counters`, city queries; the installers below are called from `install_script_funcs` |
  | `script_funcs_query.cpp` | per-frame queries: counts (`num_units`, `num_unit_category`, `num_buildings`, kills / losses, `num_type_upgrade`), health, the `*_killed` / `*_destroyed` tallies (read and clear), order state (`is_idle`, `has_*_order`, rally points), `object_near*` / `any_object_near*` / `object_type_near*` (`Objects::find_units` / `find_builds` port), `find_military*` / `find_all_*` / `find_idle_military` / `find_inside_unit` / `find_capital` / `find_city_name` (round-robin counters), city capture / alarm / assimilation, diplomacy (`have_war`, `have_contact`, offers), map (`map_is_land` / `passable` / `buildable`, `territory_owner`), score, rates, CTW stored values (`get_/set_ctw_int`, `_str`, `clear_ctw_val`) and the CTW reads with their no-campaign values |
  | `script_funcs_mutate.cpp` | goods, rates, pop cap, techs (`gain_tech` / `lose_tech` by name or category, `gain_next_age` = `Leader::set_age`, `set_preq`), type edits (copy-on-write `Game::mut_rules()`: build / job time, rename, disable / enable by tribe, max health / attack / range / armor / speed), victory / defeat, `create_unit*` / `create_building*`, kill / damage / heal, fog (`set_explored`, reveal points, `show_all_map_*`), AI switches, diplomacy (`declare_war`, `make_peace`, `make_alliance`, offers), named groups, the `ScenarioData` switches (`*_costs_disable`, `buildings_gather_*`, `building_*_bonus_*`) |
  | `script_funcs_order.cpp` | orders through a pushed Group as the original: `group_move / attack / attack_to / stance / jump_move / patrol_order` (int-who, named and UnitGroup overloads, each returning the member count), `unit_*_order`, `unit_clear_orders`, `create_game_group` |
  | `script_funcs_ui.cpp` | presentation and seat-local input (below), objectives, goals, messages, sounds, pings, camera, highlights, arrows, flags, iface rectangles, `key_down`, the in-memory XML file builtins |

  New state: `ScenarioData` (`sim/scenario_data.hpp`, `Game::scen`: goals, named groups, kill / destroy tallies,
  last razed / built, city-lost bits, pop cap override, cost switches, time limit), `Leader::chat_status`,
  `take_att_disabled`, `neutral_attrition`, `Game::field_battle`; `Game::scenario_mode` is now
  `scripted_rules || ctw` (`Game::is_scenario`), so the scenario-only paths act only under scripted rules / CTW;
  `building_unit_bonus`, `building_resource_bonus` and `buildings_gather` default to 1 (`ScenarioFuncSet::init`).
  Sim hooks: `Unit::close` / `Build::close` (tallies), `take_damage` / `disband` (last razed), `City::capture`
  (city lost), `Build::train` (last unit built), `calc_pop_cap` (pop cap override), the cost functions (free units /
  buildings / techs), the capture time limit (`custom_time_limit`, else the port's 300 s).
- **Presentation and input API** (`sim/script_ui.hpp`, the renderer's read-only view): `script_presentation(game)`
  returns the `ScriptPresentation` (or null without scripts): `events` (a bounded deque of `ScriptEvent` with a
  monotonically increasing `seq`, so a renderer remembers the last seq it consumed: game / scenario messages,
  instructions, feedback, chat, notices, bubbles, popups, sounds and stop-sound (by file name, with the resolved
  scenario path), music moods, pings, camera requests, clear-messages, goal dialog, screenshots, pause, victory
  screen), the persistent state (`objectives`, `arrows`, `flags`, highlights of a build / train / tech option or an
  object, scroll / zoom locks, `ui_hidden`, basic menu, mouse / hotkey selection and speed / pause switches,
  victory / defeat messages, the latest `CameraRequest`, the music mood, unit scale). `script_input(game)` returns
  the mutable `ScriptInput` the renderer fills each frame for the local seat: `attached` (when false the headless
  models answer: `sound_finished` after the WAV length at 15 frames / s), the local selection (`select_who`,
  `selected`), the armed option (`option` = OptionIndex, -2 contact panel, -3 diplomacy panel; `option_object`,
  `mode_data`, `build_options`, `selecting_spot`), cursor tile and `cursor_over_iface`, screen size, interface
  rectangles by name (`get_button_*`), the objectives box, camera position / zoom / panning, an `on_screen` callback,
  `keystate[256]`, `user_warnings`, `sound_state`, `graphic_clock`, the profile name, a popup handler.
  **Multiplayer**: the builtins that read or drive one machine's interface carry the original's multiplayer guard
  (`semaphore byte0 & 4`) builtin by builtin (selection, armed option and build menu, camera, mouse / key,
  `sound_finished`, warnings, highlights, flags, goal dialog, pause: -1 / 0 or no effect in MP); the port keeps
  each guard (`sf::mp`), so a lockstep game never branches on seat-local input. Objectives, messages and pings are
  not guarded: every machine produces the same events.
- **Logged stubs / TODOs**: 341 names have no body (logged, returning the error value); of the 137 used by the
  shipped scripts (3043 call sites) all but 5 call sites are in the CTW campaign scripts (`set_no_attack`,
  `get_country_owner`, `ctw_set_leader_name`, `get_num_territories`, `group_guard_order`, `get_territory_owner`,
  `ctw_is_war`, ...: the strategic layer, `ConquestGame`). The non-CTW ones: `get_territory_owner` /
  `get_territory_strength` (CTW territories) and `get_starting_loc_x/y` (World start locations, not ported).
  Bodies that call unported systems count in `ScriptHost::todo_calls` (ron-sim prints them): `general_forced_march`
  / `general_entrench` (spells), `unit_stop_effect` (`Caster::process_spells`), `unit_clear_orders` on planes
  (strafe), `unit_attack_ground_order`, `group_patrol_order` / `unit_patrol_order`, `unit_garrison_order`,
  `unit_eject_order` (COME_OUT command), `place_*_with_cost` (AI production), `air_unit_transfer_order`,
  `create_missile_and_target_build`.
- **Not done**: save / load of the RunTimeEnv and ScenarioData script state (state-walk.md; TODO), the paused-game
  runs of `TurnControl::do_frame_solo`, CTW setup / runtime scripts (the conquest battle scenarios with
  LOAD_SCRIPT and no sibling .bhs get their script from the CTW layer) and the CTW strategic-layer builtins.

Harness: `ron-sim` loads scripts unless `--no-scripts`; `--script-log` prints the presentation queue as it fills
(`SCRIPT sound "intro.wav" [path] ms`, `SCRIPT objective added 'id': "text"` / `complete`, camera, pings,
messages) and the run-time messages; `--select WHO:o1,..@FRAME` and `--option N[:MENU[:TYPE]]@FRAME` set the
seat-local input a renderer would report. The summary line `scripts: ...` lists what was compiled, the runs and
failures, the unported builtins called and the unported game functions. The digest covers `ScenarioData`, the find
counters and the timers.

Results (2026-10-13): every stock scenario and conquest battle runs 8000 frames with 0 script failures; the only
unported builtin still called (scenario/ + conquest/) is `group_guard_order` (3 calls, one conquest battle); the
game-function TODOs hit are the spells (`general_forced_march` / `unit_stop_effect` 253 calls each,
`general_entrench` 1), plane strafe in `unit_clear_orders` (10), `unit_attack_ground_order` (2) and
`group_patrol_order` (1). The tutorials run their triggers until they wait for the player:
tut1 to `Select_Boadicea`, tut2 `select_citizen`, tut3 `select_army`, tut4 `alarm_select_city`, tut5 `Farms`, tut6
`click_diplomacy`; with input they advance, e.g. `tut2 --select 0:0@900 --option 1:1@1500` completes
`select_citizen` at 900 (sound, arrow), adds `military_menu`, completes it at 1500 and adds `select_lookout`; tut1
with `--select 0:1@300 --option 9@700 --move 0:1@11000,14700@900` runs through the scroll objectives. Scripts
change the simulation where the tutorials intend it: tut2's script orders the Viking attack and damages Ethandun
(`damage_object 98`), so the city falls at frame 324 instead of 711; tut5 at `--difficulty 4` still ends in L1's
capture victory at frame 11793 (the script only adds messages / objectives there).
`ron-script` (apps/script) is the compiler / VM workbench: `--parse-all`, `--compile-all`, `--disasm`, `--run`,
`--selftest`.

## Computer opponent (engine/sim/ai_*.cpp, docs/ai-spec.md)

Ported from the spec and the decompiles (`re/decomp/ai/`, `ai2/`, `ai_create/`, plus `re/decomp/ai_port/` for the
helpers decoded during the port: ObjectsData::find_unit / find_building / find_city with their disassembly,
Search::valid_filter's jump table, random_personality, Leader::init, SimpleArray<int>::remove, tech_avail, get_gov,
the wonder-value helpers; `re/decomp/ai_diplo/`, `re/decomp/ai3/` for the diplomacy actions and the score),
ai-spec 15 steps 1-11:

- **Setup** (`ai_setup.cpp`): `leader_init_ai` = Leader::init's AI part (prod_script, `random_personality` with every
  draw of ai-spec 2.3, the lobby player-flag overrides, the script coin when rush == 0, sites / make list / lists
  reset). Game::load now follows Setup::build_game's stream for scenarios: seed, one `world.seed` draw, the leaders
  chunk (every scenario seat's VALID | ACTIVE | HUMAN flags and tribe set first, as init_rules_and_teams does; the
  personality draws happen inside each leader's init, before its child chunks), then the terrain reseed — so the
  personality values come from the stream while the simulation stream after the reseed is unchanged. `world.sea_map`
  = the header's `ai_sea_map`, `info.map_style` from the header. The helpers the AI reads (get_diff, get_team_terr,
  get_mod_resource_cap, has_market, epochs, farm limit, get_target, is_tribute_period, Leaders::max_*, queued_units,
  war_allowed, wonder_winning and the wonder values, city counts, count_gather_slots, could_queue) and the
  ObjectsData searches (`objects_find_city / _unit / _building`, writing `Game::find_dist / find_who`; valid_search and
  the valid_filter cases TYPE, BASE_TYPE, NOT_ME, NOT_UNDER_ATTACK, COMBAT, TARGET, DOMAIN, NOT_DOMAIN), and the world
  helpers (space_at_corner with the grid_index / grid_threes tables, check_building_wcoord, World::gather_at,
  is_dock_tile, num_coasts, find_fort, danger cells).
- **Census** (`ai_strategy.cpp`): `leaders_strategy_all` (check_explore, plan_strategy, compute_score, diplomacy);
  `leader_plan_strategy` = the whole plan_strategy (cadence, the 30-frame tech retry, resets, team
  territory, the fog part moved from fog.cpp, invaders, region arrays, rares, the unit and building census, highs,
  the city walk (land / filled / space / ter / dock tile / ocean), wars / allies, the region strategy bits and the
  army creation calls), `check_orphaned_buildings`, `compute_sites` (draws A and B) and `compute_site_stats`.
- **Production** (`ai_production.cpp`): `leader_production_ai` (states 0-11, the starting_resources 8 variant;
  state 1 runs the production script as before), production_ai_setup + market_speculation, found_cities,
  research_techs (government coin / rnd(100)), upgrade_units (rnd(3)), unit_prod_value, check_income, make_me,
  can_pay, make_stuff (the rnd(3) duplicate draws), use_market (rnd(6)), make_this and produce_tech / upgrade / spell /
  unit / city (pay before the site test, no refund).
- **Creators** (`ai_create.cpp`): create_units (land army, explorers, citizens, scholars, caravans, merchants, spies,
  heroes, the navy branch (dock water-region scan, warships against the enemies' sea strength with `init_navy`,
  fishing boats) and the air branch (nuclear / cruise missiles with get_nukes / get_armageddon, aircraft); the rnd(3)
  draw for every combat type before the domain branches) and create_buildings (all of 6.6 incl. the wonder
  rnd(1000) / rnd(300) draws).
- **Placement** (`ai_place.cpp`): produce_building (oil patches, the ring scan with find_friends, rnd(500) for farms /
  mines, builder choice, the rnd(100) fine-position draws, pay, init_build, swarm or activate at frame 0, the city
  bookkeeping) and find_friends.
- **Builtins**: place_[orphan_]building[_upgrade][_with_cost] call produce_building, place_city_with_cost runs
  compute_sites + found_cities (script_funcs.cpp).
- `Wall::increment_stats` / `decrement_stats` keep the leader's `mil_trainers` list (append / first-match remove).
- **Armies** (`ai_army.cpp`, ai-spec 12.1-12.5): the 16 slots per leader, `init_army` (reuse rule), `init_navy`,
  the Armies helpers (find_army, find_local_army, find_aggressive_army, num_armies, emergency, diplo_change,
  send_navy); `Armies::process_all` (frame step 12) and `Army::process` (the 128-frame tick, the 256-frame body,
  disband, merge, creep, the state handlers); normalize (with the category insertion sort), add_group / add_unit /
  remove_group / get_unit, center_of_gravity, is_engaged, is_moving, engagement, set_stance, close, send_here;
  do_mustering + release_mustering, do_defending + find_besieged_city, do_marching, do_forming, march_to_target,
  do_transporting + Region::go_here; find_target (both passes, the F1 coin, F2 rnd(200) per city, F3 per fort, the
  EASIEST token raid) and find_muster_spot; `Unit::add_to_army` + go_to_unit (from think_attack, come_out and
  think_scout), `Object::get_army`, `Army::charge`. `Group::action_siege_attack_to`'s escort (action_guard) is
  approximated by an ATTACK_TO move of the rest of the group. **Danger map**: `GameDaemon::calc_danger` every 200
  frames (and once at load) with do_danger.
- **Per-unit AI** (`ai_unit.cpp`, 12.6): Unit::think steps 12-16 for AI units (supply wagons / heroes join armies,
  scouts and spies explore), `think_scout` (region mode T1, the enemy-city ring scan T2-T4 with its 0x600 budget, the
  region scan T5-T6), `find_goody_box` / `explore_goody` (G1), `find_unit_ordered`; `think_spellcaster` makes the S1
  draw only (spells are not simulated).
- **Diplomacy** (`ai_diplo.cpp`, ai-spec 10): `Leader::diplomacy` with the gate / cadence, C1-C11, H1-H13, E1 / E2,
  the action selection (tribute demand, declare, warn), answering proposals (R1-R5) and the AI's own proposals
  (P1-P4), draws D1-D9 in order; `set_diplo` (ally mask, garrison ejection, Armies::diplo_change), `action_declare`
  (team mates follow; human declarations pay and count dow / blacken), action_treaty / attack / clear_all,
  consider_tribute; `action_respond` (econ_market.cpp) now has the AI stamps, the alliance rule, the treaty upgrade
  with made_peace and neutral-seat propagation, and the joint-attack hire stamps and declarations. `hire_who` starts
  at −1 (Leader::init).
- **Score** (`ai_diplo.cpp`): `Leader::compute_score` and compute_unit / build / economy / unit_upgrades / research
  score with `TypeData::get_score_value` and `Game::get_armageddon`.

Harness: `ron-sim --ai-every K` prints per AI leader the production state, age, population, units (citizens, free,
gathering, combat), buildings / sites, techs, armies, the stockpile and make list item 0, then each army (status,
units / captains / standard / attacking, target with its damage, position, muster spot), the combat totals with the
land military units in armies, difficulty and personality, the score parts and the diplomacy state per opponent
(diplos, met, agendas, aggression / strong / weak). `--difficulty N` sets the GameInfo difficulty (default 2).
`RON_AI_TRACE=1` traces each production step on stderr (make list, sites, city census). The digest includes
personality, sites, make list, census counters, trainer list and armies.

Result (seed 12345, `scenario/Tutorial5_Trial/tut5.scx`, AI leader 1 at difficulty 2, 12000 frames, self-check OK):
5 citizens / 1 city / 6 buildings / Ancient at the start; a second city by frame 2000; 29 citizens (28 gathering),
25 buildings, 10 techs and 6 combat units by frame 12000, Classical Age at frame ~7700. conquest/Alexander/
magnagraecia.scx (7 AI leaders): 9 citizens each at the start, 25-35 citizens, 2-4 cities, military units by frame
9000 (no age: the scenario's ending technology is 1). All 156 stock scenarios run 300 frames with self-check OK.

Armies in action (`tut5.scx`, `--difficulty 3`, 20000 frames, self-check OK; recorded before the capture port, while the
scenario header's `wonderwin` was not read — see "Capture and victory" for the current runs): the first army (9 units, MUSTERING at
city 1) is released at frame ~8000 and idles at its own city while it has fewer than 7 standard units (find_target's
difficulty rule); with 18 units it takes leader 0's Small City (0:2006) as target at frame ~12000, forms up at the
muster spot, assaults (15-16 of 18 units attacking), reduces the city to 1199/1200 damage, kills 5 units and razes 5
buildings by frame 20000 while a second army (19 units) musters at home. The diplomacy line shows L1 at war with
the human seat (aggression 1, strong 832). At difficulty 2 (the default) the army stays under the 7-standard bound
and only guards its own cities. `Custom/italy_mp.scx` (8 leaders): leader 6 at war with everyone kills 6 units by
frame 20000; leader 5 sends friendly-chat agendas (0x40) to its neighbours.

Not done (TODO in code, with spec references): transports (think_carry family, do_transporting only picks a region), the hero S2 coin and
every spell cast (spells are not simulated; needs the guy field +0x84), use_generals / use_spies / use_scouts, the AI
retreat at the end of think_attack, the team-game score average
(semaphore byte0 bit 7), ScenarioData::diplomacy_setting (scripts not ported), Game.num_sides (init_teams),
eject's aircraft strafe order, the starting_resources 7 / starting_technology 8 missile-shield shortcut, random-map
setup (stock scenarios carry ai_sea_map −1, so no warships), the lobby player flags and start order (get_target uses
the identity permutation).

## Capture and victory (engine/sim/capture.cpp, docs/capture-spec.md)

Ported from the decompiles and the disassembly in `re/decomp/capture/` (capture-spec §15):
- **Reducing a city**: `take_damage` sets the building's city flags 0xe (UNDER_ATTACK / ATTACKING / EVER_ATTACKED: the
  city stops healing while assaulted) and honours the citizen / militia exception for `build_masks` 0x30; the
  `do_damage` city tail calls `check_capture`, then `Army::charge` for a computer siege attacker and the 300-frame
  `reduce_stamp`; the recapture damage modifier (city `race` == attacker) feeds `get_damage`.
- **`check_capture`** (§3): gates, the circle strength scan (units' `get_capture_value`, the owner's buildings 7 / forts
  13, FILTER_NOT_UNDER_ATTACK, the attacker-sees-it `visible` bit), the new owner (largest attacking-side sum, founder /
  race redirect), `capture_strength`. Callers: `valid_target`, the group attack code, the `do_damage` tail and the city's
  64-frame poll in `Build::process` (with the AI's repair call through `find_unit` FILTER_NOT_BUSY).
- **`Cities::capture_city`** (§4): the city object's copy (`Build::swap_team` + `activate(1, 1, 0)`), `City::capture`
  (flags, race, capital bookkeeping, unfinished Senates disbanded, damage = hits − 10), `Armies::update_city`, the city
  list's buildings copied (Lakota farms / granaries and 'n' buildings excepted), `City::find_buildings` capturing
  foreign orphans, the allied unit transfer (`Unit::swap_team`), plunder (ordinary, capital lump sum, Russian),
  disposal (disband / close dt 5), `lost_capital` / `recapture_capital`, `City::close` of the old record (armies, then
  `Leader::lost_a_city`, the library queue hand-over) and the old city object.
- **Elimination** (§6): `lost_a_city` (elimination modes 0-3, the GLOBAL_GOVERNMENT capital rule), `defeat` (units keep
  standing, planes die, queues dropped, `Armies::leader_defeated` → `Army::stop`), `defeat_by` (cities to the
  conqueror / founder / race, buildings swapped or destroyed; `blow_up_*` in multiplayer), `process_elimination` with
  `Game::retake_capital`.
- **Victory** (§7): `Game::check_victory` (`Game::game_over` stops `Game::do_frame`), `Leader::victory`,
  `Game::defeat_all`, the three clocks in `Leaders::process_all`, `GameDaemon::process_victory` (Armageddon, wonder,
  territory, score, time limit). The scenario header's victory settings (`score_goal`, `time_limit`, `wonderwin`,
  `popwin`, `econwin`) now reach GameInfo as ScenarioRead::import does (this changes the AI's wonder decisions in
  scenarios with a non-zero `wonderwin`, e.g. tut5).
- `ron-sim` prints `CITY CAPTURED`, `LEADER DEFEATED`, `CAPITAL LOST / RECAPTURED`, `VICTORY` and `GAME OVER` lines
  (from `Game::events`, a tool log outside the simulation state) and, with `--ai-every`, each AI leader's victory clocks;
  `--list` prints the game settings. The digest includes the city capture fields, the leaders' flags, capture counters
  and clocks, and the game-over flag.

Checks (seed 12345):
- `tut3.scx --reveal 3 --attack '0:0,1,4,7,8,9,30,33,36,39,45,46,48@1:2005' --frames 6000` (the Duke of Bedford's army
  against Joan of Arc's Small City): 312 hits (1404 hp + 1942/16) reduce the city; the hit at frame 385 captures it:
  `L1:2005 -> L0:2004 (Small City, damage 1490/1500)`, 3 buildings copied, 2 closed, plunder 50 timber (2 × 25, paid
  to L0's poorest good); L0 now has its first city, which assimilates after 2000 frames and heals. Self-check OK.
- `tut5.scx --difficulty 4 --frames 20000 --ai-every 2500`: L1's army reduces L0's capital (Small City 0:2006) by frame
  ~10000 and captures it at frame 11793: `CAPITAL LOST L0`, `LEADER DEFEATED L0 by 1 (type 0)` (its last city),
  `GAME OVER`, `VICTORY L1 (type 0)`, capital plunder 500 food / timber / wealth to L1. At `--difficulty 3` the AI now
  keeps its army mustering through frame 20000 (the header's `wonderwin` 5 changes its build path, so it reaches the
  later military ages with fewer than the release threshold of standard units; it wins by territory at frame 35142).
- `tut2.scx --frames 3000` (default difficulty): the Vikings (L1) capture the British Small City 0:2010 at frame 711
  (`-> L1:2011`, 6 buildings copied, 5 closed, plunder 125 metal = 5 × 25); L0 keeps its other city. Self-check OK.
- `Custom/italy_mp.scx --frames 20000`: L5 captures L6's Small City at frame 8148 (3 buildings copied, plunder 50
  wealth); L6's armies retarget the captured city through `Armies::update_city`.
- At `--difficulty 3` tut5's army stays MUSTERING through frame 20000 (self-check OK): it has 3 standard units when it
  forms at ~7500 (release needs 5) and by ~10000 the military epoch is 2, which raises `release_mustering`'s bound to
  more than 15 standard units (ai-spec 12.4.1); the `--ai-every` flags line now shows the four epochs.
- All 156 stock scenarios: 300 frames self-check OK, no capture / defeat / victory event in the first 300 frames
  (the conquest maps' custom time limit counts as scripted: capture-spec §7.3 port decision). Over 3000 frames:
  conquest/Alexander/barbariantactics and conquest/Napoleon/lowrationsruss end with L1's territory victory at frame
  2571 (L1 holds over the `popwin` share from the start; these maps are meant for Conquer the World), the tut2 capture
  above, and Chess Exercise/cehotkey1 gives L0 a territory victory at frame 2057 although it has a single nation:
  `GameInfo.num_nations` is still the default 2 (TODO: Game::init_rules_and_teams sets it from GameInfo.players + 1 in
  single player; the scenario value of `players` is not known).

Not done (TODO in code): heroes (despot / Spitamenes plunder, THECITIZEN), garrisons (the strength scan's num_inside,
ejection by swap_team, the AI alarm), Unit::swap_team's cargo and the exact GuyData copy, musical-chairs and economic
victory (Game.on_team, get_economic), CTW retake adjustments, balance mode, scripted `city_lost` bookkeeping, the
caravan repath of City::close.

## Resume here (in order)

1. ~~Collision stacking bug~~: not a bug. Same-group movers are soft colliders by design (detect_unit_collision);
   the stacking came from the stubbed formation slots, fixed by item 2. tut1 0:20's "short stop" is an impassable
   target (tdata & 0x6000); its 125-unit step is the movement-spec §5 step 8 snap onto a free waypoint within 0x60.
2. ~~Formations, `action_move_near`, `do_group_move`, `action_form`, `action_stance`, `find_nearby_spot`~~ done
   (remaining TODOs are army, combat and transport branches, `Objects::add_flock`). A JS reference for
   `compute_dests` (re/tools/) would let the formation layout be checked independently of the C++.
3. ~~AnimSource from `unit_graphics.xml` + `.bha`~~ done (anim-spec.md), incl. `action_move_near`'s `update_gpiece`.
4. ~~Diplomacy from scenario chunk 41~~ done (scenario-objects-spec §7).
5. ~~Fog of war~~ done (see "Fog of war"); the renderer draws the shroud from `make_fog_view` and the view's
   `seen_by` / `now_seen_by` masks. Next fog work comes with combat (`valid_target_const`, `set_attacking`), territory
   and scripts.
6. ~~Combat~~ steps 1–8 done (see "Combat", "Healing and attrition", stats.cpp). Next: packers and ATTACK_GROUND,
   then compare with a real replay that contains fighting.
7. ~~Production~~ done (see "Production"), territory included.
8. ~~Economy~~ done (see "Economy"). Next: caravan roads (PathFinder::find_road), packers (merchants), the AI parts
   (strategy_all), then compare with replays that contain gathering, trading and market commands.
9. ~~AI~~ (ai-spec 15 steps 1-11) done (see "Computer opponent"). ~~City capture~~ done (see "Capture and
   victory"). Next: ~~transports~~ (see "Transports"), the other spells, heroes (the despot plunder, THECITIZEN), garrisons (ejection, the strength
   scan's num_inside, the AI alarm); ~~the most-called scenario builtins~~ done (see "Scripts": 472 names, the
   tutorials call no unported builtin); then save / load of the script state, the renderer side of
   `sim/script_ui.hpp` (apps/map-mac), the CTW strategic layer.
