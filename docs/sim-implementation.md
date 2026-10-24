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
| `collision.cpp` | `collide_here` (+fill_slots, fast-path stall quirk), `coll_move_unit`, corners, `detect_unit_collision`, `resolve_unit_collision`, `resolve_block` (stub 0), `detect_boat_collision` (ported from the asm, see "Orders and commands, second pass") | ported |
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
| `spells.{hpp,cpp}`, `spells_cast.cpp`, `packers.cpp`, `transport.{hpp,cpp}` | spells and transports ([transport-spec.md](transport-spec.md), [spells-spec.md](spells-spec.md)): `add_cast_order`, `do_cast` (both branches), `spell_is_castable` / `spell_is_valid_target` / `spell_get_range` / `spell_job_time` / `pay_cast_costs`, `spell_cast`, `group_action_spell`; the spies' and special forces' casts, conversions and city casts (`spells_cast.cpp`); PACK / UNPACK and the merchant spots (`packers.cpp`); the who-may-be-carried predicates, `cast_transport`, `same_damage`, BOARD_SHIP / AWAIT_BOARD, the TRANSPORT / SET_TRANSPORT / BOARD_SHIP group actions | ported (see "Transports", "Spells") |
| `replay.{hpp,cpp}` | the `.rcx` reader ([replay-spec.md](replay-spec.md)): gzip / plain, header, embedded save, random info, rules block, package records with the multiplayer de-XOR and padding, the 82-verb table, `replay_to_engine` / `engine_to_wire` between wire commands and `Command` | done (see "Multiplayer") |
| `engine/net/` | the lockstep layer ([multiplayer-spec.md](multiplayer-spec.md) §8): `Session`, `Transport` (loopback, TCP), `Lobby`, `GameHooks` over `Game` | done (see "Multiplayer") |
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

Order / command options added in the second orders pass (see "Orders and commands, second pass"): `--patrol` (an alias of `--air-patrol`: planes fly it, ground groups walk it; `/last` queues a waypoint), `--attack-ground` for ground groups too, `--cast WHO:o@PARADROP@TO:TWHO`, `--hotkey`, `--come-out`, `--speed`, `--pause`, `--chat`, `--rename-city`, `--cheat`; `RON_OLD_BOAT=1` runs the pre-port `detect_boat_collision` stub.

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

Stubbed (TODO in code): aircraft (helicopters on carriers, airbase scramble), the militia disarm spell on leaving, the scenario ignore-orders
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

Stubbed (TODO in code): (the targeted branch of `do_cast` and the other crafts are done, see "Spells"), the group machinery
for chains landing from a barge (`TODO(groups)`: `reset_move_orders` + the temporary group; the orders and path go to the
captain instead), `is_entering_or_exiting` / `unpay_cast_costs` in `kill_current_order`, aircraft cargo in `can_carry`,
the AI's ferrying (`think_civilian_transport`, `do_transporting`), sounds, the console selection and `replace_hotunit`, the
`ron-sim` options and the right-click verbs. ❓ `check_meet_ship` follows the disassembly but is exercised only by
BOARD_SHIP (no stock scenario issues it); `action_board_ship`'s QUEUE_FIRST re-insert runs `finish_insert` on the group (the
decompile uses a temporary group holding the ship).

## Aircraft (docs/aircraft-spec.md; engine/sim/aircraft.{hpp,cpp}, aircraft_cmd.cpp)

Planes, helicopters, airbases, carriers and missile silos (2026-10-24). `aircraft.cpp` holds the containers and capacity
(`can_carry` air branches, `home_base`, `num_aircraft_here / limit`, `has_nuke`), the three air orders and their adders, the
flight (`do_air_physics` with the guy's bank / pitch floats, `check_fuel`, `land_plane`), `do_strafe / do_air_patrol /
do_air_attack_ground`, the target searches, the fuel / recharge / healing block of `Unit::process`, `do_launch`,
`attempt_launch`, `Build::do_missile_launch`, the carrier's PLANERELEASE events, the aircraft-housing branch of `Build::train`,
the airbase gather-point hooks and `air_describe`; `aircraft_cmd.cpp` the command layer. The file is compiled with
`#pragma clang fp contract(off)`: the float bank / pitch math must not be fused.
- **State** (save version 3): `Order` air fields, `Guy` bank / pitch, `Object::launch_timer / launch_list`, `Unit::mana_burn`
  (fuel used). The two simulation RNG draws of the flight (the altitude reroll every 8th frame of `(o + frame)` and the edge-turn
  draw) are in call order; a new carrier takes exactly 7 draws (one `init_unit` per plane; `RON_AIR_TRACE=1` prints the count).
- **Commands**: FLIGHT 28 (`Command::orders` 10 attack / 1 rebase, shift / ctrl / alt), PATROL 10, LAUNCH_PATROL 11,
  ATTACK_GROUND 9, SCRAMBLE 36, RECALL 35, UNQUEUE of a unit (`Unit::action_unqueue`), the airbase rally point (gather action 3),
  and the scenario builtins `unit_attack_order`, `unit_clear_orders`, `air_unit_transfer_order`, `create_missile_and_target_build`,
  `create_unit` (planes fly at once, a carrier gets its wing), `num_garrisoned_in`, `building_full`. The console messages and
  sounds of the original are not modelled (marked "(message)" in the code).
- **Hooks in older files**: `can_carry` (transport.cpp), the Strafe / AirPatrol / AirAttackGround / SpecialAnim cases of
  `Unit::work` (orders.cpp), `unit_process` (objects.cpp), `Build::process / finished / train / queue_up` gates (queues.cpp, build.cpp,
  construct.cpp: HOLDS_AIR | REPEAT_AIR on air containers, the 10-plane airbase limit in `Build::finished`), AA bands
  (`is_flying_low / high`, combat_target.cpp), the carrier's strafes (combat_orders.cpp), eject / garrison (garrison.cpp,
  ai_diplo.cpp), the carrier closing (combat_death.cpp `carrier_closed`), guys.cpp (aircraft z) and the animation lookups.
- **Where the exe contradicts the spec** (the exe wins): (1) the launch cadence is **16** frames, not 15: `do_launch` @64f46a
  compares the timer **before** the increment (`CMP ECX,15; JL`), so after a launch (timer 0) the next walk is 16 calls later;
  (2) `Build::queue_up` @620f40 does **not** refuse a plane over the airbase limit (it only prints "building full"): the item
  waits in the queue because `Build::finished` @628490 returns -1, so `num_aircraft_here(0) <= 10` holds and the queue keeps the
  rest; (3) `vslot 8` of the object is `is_live_unit`: the relative patrol waypoints (`add_air_patrol_order` @5e4350,
  `do_air_patrol` @5ea620, `Unit::close` @60ee50) apply to a **carrier** only, never to an airbase; (4) `ObjectData::is_in_range`
  @64e4a0 returns 1 for a MISSILE that is **not** inside a container (`get_inside(0) < 0`; combat-spec and the old engine code had
  the sign reversed), and a missile type's range is 99999, so a missile fires its ground ammo and dies the first frame it
  runs after leaving the silo; (5) `Group::action_gather_point` @6ff1b0 (action 3) calls `action_flight(x, y, ATTACK, ...)` with
  the point's coordinates in the `(ox, whom)` slots: it never finds a target and does nothing; (6) `Group::action_flight`
  @6fb260 MOVE_TO on a no-fuel STRAFE refuses a base farther than the plane's current one (the two `vector_dist` calls compare
  the new base against the old home); (7) `ObjectData::has_nuke` @643d40 is "the silo holds, queues or expects any missile".
- **Harness**: `--flight`, `--air-patrol`, `--launch-patrol`, `--attack-ground`, `--scramble`, `--recall`, `--air-every K`,
  `--list` (airbases / carriers / silos with their chain, planes with fuel, base, order, low / high band), the test aids `--kill`
  and `--stock`, `RON_AIR_TRACE=1`. The digest hashes the aircraft block only when some aircraft state is non-default
  (`RON_NO_AIR_DIGEST=1` leaves it out; `RON_DIGEST_PARTS=1` prints the running hash per section).
- **Validated** (2026-10-24, scratch build with the build-mac.sh flags, zero warnings): `ron-check` 0 failures; `sim-all`
  600 frames `--ai-every 8`: 156 / 156 self-check OK, `--roundtrip` OK with planes in the air, landing and based
  (korea.scx at frames 20, 300, 1085, 1095); digests against a build of the previous tree: 134 scenarios identical, 22 changed,
  every one of them with an airbase, carrier or silo (13: `build_masks` 0x88 on the air containers from the start or when the
  AI's silo completes (central, f9); 5: planes inside bases, only the new aircraft digest block; 4 real divergences from AI
  launches: korea3 / korea4 (units f21, RNG f31), suez2 (f36 / f42), Tutorial6 (f587, RNG unchanged)).
- **Not ported / ❓**: `Group::action_guard` (an ally target of FLIGHT sends the non-planes there), vslot 0x10 of the home object in
  the FLIGHT no-fuel check (read as "alive"), the strafing impact slide (combat_fight.cpp) and the carrier deck node position (render
  data). The ground halves of PATROL / ATTACK_GROUND, the paradrop, the carrier's BUILDAIR queue with the counters and refund of
  `Unit::action_unqueue` were ported in the second pass (below).

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

Not done (TODO in code): garrisons (the strength scan's num_inside,
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
   victory"). Next: ~~transports~~ (see "Transports"), the other spells, garrisons (ejection, the strength
   scan's num_inside, the AI alarm); ~~the most-called scenario builtins~~ done (see "Scripts": 472 names, the
   tutorials call no unported builtin); then save / load of the script state, the renderer side of
   `sim/script_ui.hpp` (apps/map-mac), the CTW strategic layer.


## Heroes (docs/heroes-spec.md; engine/sim/heroes.cpp)

- `Leader::heroes` / `specials` (20 preallocated `Caster` records, grown by one) with `num_heroes` / `num_specials`; the slot is
  `Unit.cara` (`unit_init` registers every `unit_flags2 & 0x20` hero and every spy / commando, `unit_close` frees it and expires the
  hero's powers). `find_hero(x, y, who, mask, type, extra)` returns the hero unit's `o`; `has_general(obj, mask, type)` is the single
  entry for every general term (stats, healing, attrition, speed, construction / training time, assimilation).
- Powers: `cast_ambush` / `cast_march` push an `ActiveSpell`; `caster_process_spells` (step 2 of `unit_process`) expires them;
  `process_cloak` (every 16 frames) hides friendly units in an ambushing general's radius (`kUnderCover`). `cast_entrench`,
  `cast_create_decoy` (decoys age in `mana_burn`, `decoy_time`), `cast_supplies`. Mana regenerates in `unit_process_casters`.
- Government heroes: `build_try_train_gov_hero` (Senate activation, `City::assimilate`), Senate `finished` (train or convert), the
  respawn clock written by `unit_init` / `unit_close`, the Senate / capital train in `build_process`.
- Plunder: `unit_plunder` (Aztec / THE DESPOT / Spitamenes draws `rnd(25)` per available good, Inca refund) from `take_damage` and
  `Object::disband`; `build_plunder` from `do_damage` kills and the DISBAND queue.
- Harness: `ron-sim --cast WHO:o@SPELL[@FRAME]`, `--heroes-every K`, `--list` (hero radius and powers); `RON_HERO_TRACE=1` logs casts.


## Spells (docs/spells-spec.md; engine/sim/spells.cpp, spells_cast.cpp, packers.cpp)

Everything the original does with `SpellType::cast_*` that is not TRANSPORT or a general's power (2026-10-24).
- **Predicates** (`spells.cpp`): `spell_is_castable` follows the jump table of @675bc0 case by case (owner chain with the tribe
  graft, decoys, SUPPLIES / PARADROP, DEPOPULATE -> the DISBAND rule, MOVE CAPITAL, gates 0, TRANSPORT, the eight pack spells by
  `kPacked`, TO ARMS' three militia techs on own / allied / unowned land, the immortals, DISBAND with the capital, former-capital
  and `is_disbanding` rules; everything else 3). `spell_is_valid_target` is @6763c0 (reason codes included), `spell_get_range` @676a80
  (spy upgrades, Terra Cotta, adjacent for siege / supply / buildings, INFORMER halved on buildings, SNIPER 1 tile on vehicles),
  `spell_job_time` as before.
- **`do_cast`**: the cost is paid before either branch (a failing target check refunds through `kill_current_order`, which now
  does `unpay_cast_costs` for a paid CAST_SPELL order and refreshes a packing merchant's gpiece). Untargeted: the first-frame
  PACK / UNPACK work (`kPacked` test, the merchant spot test and the snap to the 48-cell centre, CHAR_PACK / CHAR_UNPACK), the
  Monarch's extra `spell_time` tick for siege, TRANSPORT as before. Targeted (flags & 0xe): follow the dead target's chain top,
  target the container of a unit that is inside, `is_valid_target`, the Senator's own-or-allied-land rule for BRIBE, the
  spell-target fields (`cavarch_o / who / uid`), the range walk (`find_nearby_spot` + a MOVE_TO in front), `is_seen`, facing, the
  per-craft animation, `kCasting`, the cloak rules (blown cover every frame except spies and SNIPER, at completion for all),
  `unit_masks2 |= 0x60` for BRIBE, then `SpellType::cast` with the target (or the area's x / y for flag 8).
- **Casts** (`spells_cast.cpp`): BRIBE (`units_bribed`, `unit_swap_team`; a building: `build_swap_team`, close the old one, activate
  the copy, `mask_me`; a dead teammate's city: `cities_capture_city`), COUNTERINTEL (`die(2)` + counters on an enemy spy,
  or `infiltrated = 0`), INFORMER (`infiltrated |= 1 << who`, `update_seen`), SABOTAGE (half the hit points, at least
  `COMMANDO_MIN_DAMAGE`, `take_damage(.., SPLODED, quiet)`; a vehicle dies with `die(3)`) / SNIPER (`die(2)`), DEPOPULATE (a finished
  city centre -> DISBAND, else refund), DISBAND (`build_plunder` + `Object::disband`), MOVE CAPITAL, CIVILIAN / TO ARMS / the
  immortals (`Unit::rare` is `made_from`; hits ratio kept at 1/256), RESEARCH PROJECT, BLOW TREAD, radar UPGRADE. Dead: PILFER,
  ASSIMILATE, RALLY, JAM RADAR, SUPPLIES (the old port), PARADROP (aircraft-spec 9), the gates. War prices are `ai_diplo.cpp`'s
  `pay_dow` / `afford_dow`.
- **Packers** (`packers.cpp`): `cast_pack` / `cast_unpack` (the merchants' 2x2 market block, `kRecompute`, LOS / seen / gpiece /
  relocation), `good_merchant_spot` (the four tiles + `unit_collect` at the corner, with `unit_collect`'s new `px / py` override and the
  "unpacking neighbours count" flag, without its writes into the unit), `find_merchant_spot` (square spiral, `invalid_loc`,
  `detect_unit_collision`), `unpack_merchant_spot` (UNPACK cast + a MOVE_TO in front; `econ_trade.cpp:unpack_merchant` calls it),
  `cast_is_packing / unpacking / packing_or_unpacking / entering_or_exiting / busy`, `unit_is_casting_spell` (@60a530: the March /
  Ambush mask bits, not the order), `unit_packer_stance`, `unit_mana_available`.
- **Issuing**: `group_action_spell` (@6fe1a0) = the "one caster" pick (flag 0x1000), the TRANSPORT branch, the per-member loop
  (castable != 0 unless DISBAND / DEPOPULATE, mana incl. the running cast, `can_pay_cost`, `is_valid_target`, `is_busy`, the
  target fields, flag 0x40 replaces running casts, QUEUE_NEW / FIRST) and, for buildings, `pay_cast_costs` + `spell_cast` (job time
  0) or `build_queue_up`. `validate_spell` is only the UI pre-flight (every rejection is repeated per member): not ported.
- **AI**: `think_spellcaster` now has the human COUNTERINTEL branch (also called from `think_economy` for a human's special units),
  the spy branch (COUNTERINTEL on a plant within 5 x range, else BRIBE on the nearest undamaged enemy combat unit), the special units'
  SNIPER, the S1 `% 3` draw (still the only draw) and COUNTERINTEL; `spell_find_target` is `ObjectsData::find` / `find_unit` with
  FILTER_SPELL_TARGET (@67e385: `is_valid_target` + `damage == 0` for BRIBE / SNIPER). `use_casters` is `Army::use_generals /
  use_spies / use_scouts` (walk the army's groups; the first hero / spy / special unit whose `think_spellcaster` returns non-zero
  ends the walk): this also switches on the AI generals' DECOY / FORCED MARCH / AMBUSH (heroes-spec 4.7) and gives every special unit
  of an army one more S1 draw every 128 frames.
- **Reactive casts** (`target_opportunity`): an idle spy BRIBEs, an idle commando SABOTAGEs the ground unit that attacks it; a running
  SABOTAGE halves `spell_time` when the attacker is not its target.
- **Packers in combat / orders**: `think_attack` unpacks a packed PACKER_AUTO packer after 3 (machine guns) / 7 (human) / 21 idle
  frames; a packed packer with a non-AUTO stance ignores near attackers; `Unit::fight` unpacks a packed packer in range (an AI siege
  weapon that out-ranges a shooting building looks for a better spot first); `work()` packs an unpacked packer before it moves
  (a lone move ending inside its block radius is rewritten to end where it stands, a move to its own 48-cell is dropped);
  `Group::action_attack` / `action_garrison` have their packing / unpacking branches; the flee test of `target_opportunity`,
  `combat_death` (a packing merchant keeps its market block), `fog` (LOS while packing), `unit_collect` use `cast_is_packing`.
- **Civilians**: an armed citizen (`CIVILIAN` castable = it `is(MILITIA)`) is a worker for `Group::action_swarm_around` /
  `action_gather` (a CIVILIAN cast QUEUE_LAST / FIRST before the work) and casts CIVILIAN when it leaves a garrison;
  `scholar_militia` counts militia made from scholars (`track_unit_type`).
- **Queues**: `build_can_make` asks `spell_is_castable` for owned crafts, `build_finished` calls `spell_cast` (DISBAND / DEPOPULATE).
- **Harness**: `--cast WHO:o1,o2@SPELL[@TO:TWHO | @X,Y][@FRAME]` (SPELL: BRIBE, COUNTERINTEL, INFORMER, SABOTAGE, SNIPER, DEPOPULATE,
  TRANSPORT, PACK, UNPACK (the artillery craft is mapped to the machine gun / merchant / fisherman variant), CIVILIAN, TOARMS, DISBAND or
  a craft index; `TO:TWHO` is object o of leader TWHO), `--teleport WHO:o@X,Y[@FRAME]` (a test aid), `--spells-every K`
  [`--spells-all WHO`] (specials, packers, merchants, casters, planted buildings, the bribe / razing counters), `--spell-fixtures`
  (acceptance 1-5: 59 checks on units and buildings made for the first two leaders: `is_castable` / `get_job_time` / `get_range`, the
  BRIBE refunds and conversion, SABOTAGE 500 / 200, SNIPER and COUNTERINTEL counters, INFORMER; `--verbose` lists them; run it on a scenario
  where both have a city, e.g. `conquest/ColdWar/korea.scx`). `RON_SPELL_TRACE=1` logs every refused
  / killed cast, `RON_CAST_LOG=1` (with it) every cast order created, `RON_HERO_TRACE=1` every `SpellType::cast`, `RON_DIGEST_UNITS=1`
  the running hash per unit.
- **Runs**: BRIBE converts a unit (`conquest/ColdWar/bribe.scx --teleport 0:6@19600,20300@500 --cast 0:6@BRIBE@12:1@502`: spy 0:6 casts
  100 frames, `L0 units_bribed 1`, 1:12 is gone from leader 1); SABOTAGE on a building (`conquest/ColdWar/infiltrate.scx --cast
  0:7@SABOTAGE@2009:1@600`: the smelter 1:2009 drops 1350 -> 675 after the 150-frame cast); SNIPER kills a machine gun
  (`--teleport 0:6@14300,24500@600 --cast 0:6@SNIPER@8:1@602`); a merchant packs and unpacks on a rare (`conquest/ColdWar/colombia.scx
  --cast 6:0@PACK@200 --cast 6:0@UNPACK@500`: the PACK takes 148 frames, the human merchant then moves to a rare spot and unpacks there
  by itself, the corner position is `tile * 192`); DISBAND through the cast (`infiltrate.scx --cast 1:2009@DISBAND@10`: the smelter is
  gone at f20); TO ARMS then CIVILIAN (`conquest/Napoleon/transylvania.scx --cast 1:0@TOARMS@100 --cast 1:0@CIVILIAN@300`: Citizen ->
  Minuteman -> Citizen, damage 0). `--roundtrip` passes across an active BRIBE (f540) and SABOTAGE (f1180) cast.
- **Validated** (2026-10-24, scratch build with the build-mac.sh flags, zero warnings): `ron-check` 0 failures; `sim-all`
  600 frames `--ai-every 8`: 156 / 156 self-check OK; against a build of the tree before the port 91 scenarios are identical and
  65 changed, each by a new mechanism (in the 63 with cast orders the first diverging frame is the first cast order's frame + 1).
  By the first cause: 33 packers (the AI's packed machine guns / howitzers / catapults unpack in range and pack before moving:
  0x28b..0x28e), 25 merchants / fishermen unpacking on a rare or fishing spot (0x290 / 0x292, many at frame 0 for human leaders),
  5 AI generals casting DECOY / FORCED MARCH through `use_generals` (0x27a / 0x27c; 12 scenarios cast a DECOY at some point), 2 with
  no cast order whose RNG moves at frame 282 / 410 (montenotte4, ulm: `use_scouts` calls `think_spellcaster`, one more S1 draw).
  Also in the set: one BRIBE (transylvania, f589) and one SNIPER (cemicrowars, f415) by the AI's spies / commandos.
- **Differences from docs/spells-spec.md** (the exe wins): (1) `Group::action_spell` (@6fe1a0) does not skip a member that
  `is_casting(spell)` or has the wrong packed state, those tests only bump the UI counters (the wrong packed state is already
  `is_castable == 0`); (2) `UnitData::mana_left` is aircraft.hpp's `mana - mana_burn`; the action's inline formula (adding the running
  cast's mana) is `unit_mana_available`; (3) `Search::valid_filter` FILTER_SPELL_TARGET (index 0x14 @67e385) also requires `damage == 0`
  for BRIBE / SNIPER and sends 0x276 and every other craft to 0; (4) the byte at order +0x1c read by `resolve_unit_collision` @5f9d30 is
  `CastOrder::paid` for a CAST_SPELL action (collision.cpp used `mandatory`): a paid cast is no longer dropped for a colliding enemy;
  (5) `Unit::work` @60d180: a move to the packer's own 48-cell is killed and `work` returns; (6) the building BRIBE closes the old
  building before it activates the copy (@672d00; `check_capture` activates first); (7) `kill_current_order` refunds only while
  `game.playing && !loading`; (8) the spec's "two draws" of `think_spellcaster`: the spy path draws nothing, the special path draws once.
- **Not ported / ❓**: `Unit::do_guard` (the GUARD order is not simulated: its idle auto-unpack and `Unit::think`'s guard use);
  `GroupOut::validate_spell` beyond the UI messages (❓ 6); the "upgrade by casting" University walk (`do_cast` 3.3: set_type only); Unit `+0x14c` (-1 on pack / unpack, no reader) and the guys'
  `+0x74 / +0x78` Monarch clock (only the extra `spell_time` tick is simulated); `FILTER_SPELL_TARGET` for INFORMER (a building that is
  active with hits left, ❓ vslot +0x178); `unit_masks2` 0x60 decay (heroes.cpp); `units_bribed` fills the padding of `Leader` (a new
  `int32` would change the size the save walk asserts) and is not saved; the radar UPGRADE and the dead crafts have no harness tests
  (PARADROP does: `--cast WHO:o@PARADROP@TO:TWHO`, second pass; `detect_boat_collision`'s packer / `is_unpacking` tests are in the second pass too).


## Orders and commands, second pass (2026-10-24; orders_patrol.cpp, commands_extra.cpp, control.hpp, collision.cpp)

Closes the order / command gaps the aircraft, spells, transport, heroes and multiplayer rounds listed. Sources: the decompiles in
`re/decomp/gaps/` (and `re/decomp/ui_air/`), re-checked against the disassembly where the C was garbled (`re/decomp/gaps/dis/`).

**Ground PATROL** (`orders_patrol.cpp`; `Group::action_patrol` @7030c0 dispatches in `aircraft_cmd.cpp`). A group with no plane (`COUNT_DOMAIN 4 == 0`,
leader not a plane) gives every member that is on the map, not a plane and not busy a GROUP_PATROL order (`Unit::add_patrol_order` @5e4560): two waypoints at
cell centres (ucoord * 0x30 + 0x18) from the group's `get_loc` (`get_loc_to` with QUEUE_LAST) to the target, `oxx / whose` = the leader, `id` = `(group.id +
frame * 10) * 100 + group.order_num`, `form_id` = the member's list index, explicit. A QUEUE_LAST command on a leader that already patrols appends the
point to every member's `wp` (waypoints stay shared by position in the list); helicopters inside a container get a MOVE_TO instead. `Unit::do_patrol` @5f1910:
the leader (`order.oxx == o`) advances `wp_index` and runs `Group::action_move_to(next waypoint, QUEUE_FIRST, ATTACK_TO)` for its whole group; the QUEUE_FIRST insert
pattern (`set_up_insert` copies the explicit orders, `halt`, the move, `finish_insert`) gives every member its patrol back through `Group::redo_patrol_order`
@706d90 (same id / leader / waypoints / `wp_index`, wired into `group_finish_insert`); a follower idles; a unit without a group walks alone with an ATTACK_TO
move in front; a carrier on patrol scrambles its wing every call. The units attack what they meet because ATTACK_TO / GROUP_PATROL are "on duty" orders
(combat-spec 2). Verified: `--patrol 0:2,10,13,5,0@9000,5352@5 --patrol ...@9000,9000@60/last` (5 units, a queued second waypoint), a lone Hoplite goes out and
back every ~450 frames, `--roundtrip` mid-patrol identical, replays: 242 recorded PATROL commands now run (`ron-net replay --all-verbs`: 45 / 45 agree).
**ATTACK_GROUND** (`Group::action_attack_ground` @704520, ground branch): every member on the map that is not a plane and whose type
`can_attack_ground` (@61db20: `!war_allowed` rush rule, `max_range != 0`, not a fireship / submarine (`unit_flags & 0x102000`), OBJMASK explosive / siege / naval /
bombard or type 0x7b) gets ATTACK_GROUND (`x / y` = the point, `Order::tolerance` = accuracy, `Order::attempts` = attack_unit; the DataWalk image maps them as
`accuracy` / `attack_unit`). `Unit::do_attack_ground` @5f1410: peace test on the point's owner (attack_unit 0), `find_nearby_spot(point, ring = big_radius-corrected distance
or max_range * 192 - 0x30 / min_range * 192 + 0x90, angle + 0x80000000, NOT_ME)` and a MOVE_TO in front when out of range; unpack a packed packer (cast 0x28c) first;
`set_attack(-1,-1)`, the ATTACK1 / ATTACK2 animation (rocking attackers pick by side), `fire_ammo(-1, -1)` when the type fires directly, `recharging = recharge + 1`. The flag-0x80 bit marks
"fired". Verified on Tyre's catapults (`--attack-ground 0:46,47,48@49000,31000@5`: unpack, the shots land at the clamped map edge). The scenario builtins `group_patrol_order`,
`unit_patrol_order`, `unit_attack_ground_order` (script_funcs_order.cpp) now run the real actions (the air halves too: Tutorial 6's Luftwaffe).

**PARADROP** (`cast_paradrop` @671f80, spells_cast.cpp). ❓ resolved by the disassembly: `find_nearby_spot(caster type, x, y, ..., r0 = 0, r1 = 0x600, step = 0xc0, angle
0x55555555, NOT_ME, o, who, 0, 0, -1, 0, -1)` around the *target point* (not the caster); found: `unit_set_new_location(spot, true, true)` (a teleport, no draw); not found: the
`unpay_cast_costs` refund (vslot +0xd8 and `mana_burn -= min(mana_burn, MANA)`). The craft is dead in the shipped data (`FROM none`), and an area cast with `ox = whom = -1` is killed by the unguarded
`objects[whom][ox]` read (spells-spec 11 (13)), so `ron-sim --cast WHO:o@PARADROP@TO:TWHO` gives the craft the Special Forces unit as its FROM (test aid) and
the target object's position as the point: central.scx `0:5@PARADROP@2000:0@5` teleports the Special Forces next to the Large City.

**Carrier BUILDAIR queue** (`aircraft_cmd.cpp`, hook `unit_process_air_queue` in `unit_process`). `Group::action_spell` with an aircraft type (domain 2) queues the
leader's current FIGHTERBOMBER on every carrier of the group that may cast 0x295 (`num_queued < 10`, room in the carrier, `can_pay_cost` else the
whole command stops, `pay_cost`, `num_queued++`, `track_queued(f, +1)`); `Unit::process` step 3 @611158 builds one every `train_time(f)` frames of `accel_train`
(* `ai_speed`), holds the clock (`pop_issues++`, `queue_time -= acc`) when the population is full, then `init_unit` (RNG draws per guy) + `go_inside`. `Unit::action_unqueue` @5e1f20 (the UNQUEUE
verb on a unit): `num_queued--`, `queue_time = 0` when empty, `track_queued(f, -1)`, `unpay_cost(f, who, -1, -1)` (the current price); `Unit::close`
drops the counters (`carrier_closed`). Verified on korea.scx: two planes killed, `--cast 0:113@0x134` twice, `--unqueue 0:113@-1@60`: one plane is built and goes inside at ~f300.

**Console / turn-control verbs** (`commands_extra.cpp`, state in `sim/control.hpp` = `Game::ctl`, not in the checksums or the native save: the original saves its hotkey groups, the
port does not; `save.cpp`'s `sizeof(Game)` guard grew by 9192). Verbs now in `Command` / `CommandPackage::process` / `replay_to_engine` / `engine_to_wire` / `GameHooks::execute` (so they are recorded, replayed and
networked): HOTKEY 34 (18 groups per leader, `Game::ctl.hotkeys[who * 18 + n]`: a GROUP + `copy_group`, or a camera bookmark x / y / zoom, or clear; `hotkey_groups_kill` from `Unit::close` / `Build::close`
= `HotKeyGroups::kill`), COME_OUT 49 (`Unit::action_come_out` @5e20b0: clear orders, the University scholar animation pass-along, `come_out(0)`), SPEED_SET / UP / DOWN 52-54
(`TurnControl::speed_set` @9567d0: up only below Fast (3), down above 0, not when the multiplayer game locks the speed (`GameInfo.flags & 0x100`); `ctl.speed`, `ctl.target_frame_time` = timings[speed]), PAUSE 76
(`ctl.paused`, per-seat count), CHAT 68 (the line is kept in `ctl.chat` and shown through a `ScriptEvent::kChat` for the local seat when it is a recipient; the cheat-string parse of the single-player chat
is not modelled), RENAME_CITY 75 (the city's `name`; the wire keeps the sender's stack garbage after the NUL so the command re-encodes byte for byte), CHEAT_VIEW_ALL / GIVE_TECHS / ZERO_TECHS /
AI_SPEED_INCREASE / AI_SPEED_NORMAL / AI_TOGGLE / INCREASE_BUCKETS / ZERO_BUCKETS / INIT_UNIT 59-67 (`Game::action_cheat_*`; each counts `accum_cheated`). `ron-sim`: `--hotkey WHO:o..@N[@FRAME][/clear|/mark:X,Y,Z]`,
`--come-out`, `--speed N|up|down`, `--pause 0|1`, `--chat FRAME:WHO:text`, `--rename-city`, `--cheat NAME[:WHO]`, and a `console:` line at the end of a run that touched them.
The HUD: `-` / `=` issue SPEED_DOWN / UP (single player; multiplayer already sent the same bytes through the session), the pause key issues PAUSE, the control-group keys go through `hotkeys::Groups::issue` (CommandManager) and read
`Game::ctl.hotkeys`, a click in the garrison grid issues COME_OUT for the first own unit of the type (a right click: every unit of the type), Enter / Shift+Enter / Ctrl+Enter / Alt+Enter open the chat line
(everybody / everybody / allies / enemies; `ronmap --chat "TEXT@FRAME"` / `--chat-draft`) and Return sends CHAT. `GameHooks`' sav-compatible policy leaves the console verbs out so `ron-net replay` keeps the numbers of `ron-sav --replay`; `--all-verbs` runs them.
❓ `Session::lockstep_verb` (net/lockstep.cpp) steps the speed without the `speed < 3` / `speed != 0` guards of `process_speed_*`, so after a SPEED_UP at Fast the session says Hyper Fast and `Game::ctl.speed` stays Fast (pacing in multiplayer follows the session;
a front end should read `Session::speed()`). The same holds for PAUSE: the session applies its per-player quota, `ctl.paused` does not.

**`Unit::detect_boat_collision` @5fa8b0** (collision.cpp, replaces the stub that returned 0 for every pusher with a `push_size`). Ships, siege weapons, heroes and supply wagons shove allied blockers instead of waiting:
candidates = `Objects::find_units` (dense: the wcoord circle of `ceil(push_size / 768)` rings when the game has at least that many units, else leaders 0..7 / 0..8 in slot order; on map, same tregion, not me),
each side modelled as `push_circles` circles of radius `push_size / push_circles` along guy 0's heading, `d` = the smallest pair distance, overlap = `(qOther - d + qMe) / 2`, `> 0`: blocked (return 0) for armed units against a flag-0x10 barge, non-allies below leader 8, unpacked / unpacking packers, entrenched units and tanks;
else the other is pushed `min(overlap, 0x30)` along `find_angle(other - query)` (an idle one at least 45 degrees off my heading, a moving one within 45 degrees blocks me), `invalid_loc` permitting, with `collide_o / collide_who / collide_frame` set and, for idle ones, a turn (`set_angle`, `Guy::turn_angles` + `do_turn`).
`RON_OLD_BOAT=1` restores the old stub (A/B digest runs). Evidence: `ron-net replay` default mode: the first `units` boundary now matches in 3 replays that did not match before (21'50'35, 21'52'11, 22'06'..); the `groups` match-through frame improves in 7 replays and drops in 1, `builds` improves in 2 and drops in 2 (45 replays).
**UnitData::is_busy / is_entering_or_exiting**: one implementation (`cast_is_busy` / `cast_is_entering_or_exiting`, packers.cpp: a PACK / UNPACK cast or a SPECIAL_ANIM of type 0 / 1); the stubs in construct.cpp, the group halt and the garrison member loop now use it.

**Validation** (2026-10-24, scratch build with the build-mac.sh flags, zero warnings, all apps): `ron-check` 0 failures; `sim-all 600 --ai-every 8` 156 / 156 self-check OK and 156 / 156 `--roundtrip 200` identical;
`ron-net selftest` 38 / 38; `ron-net replay` 45 / 45 agree (default and `--all-verbs`, final states identical, default outputs equal to the pre-change run except the 16 files whose fidelity lines moved with the boat port); `ron-net survey --codec`: engine commands re-encoded
104,567 -> 105,256 (+689: HOTKEY 45, SPEED_SET 1, CHAT 93, RENAME_CITY 8, PAUSE 542), commands left raw 6,691,770 -> 6,691,081 (CAMERA, TURN_DATA, CHECK_SUMS, PLAYER_SPEED ...), the 2 LEADER_OPTIONS that differ are unchanged.
Digests against the previous tree: with `RON_OLD_BOAT=1` the whole set matches the tree before the round except Tutorial 6 (its script now issues real air patrol / attack-ground orders); the boat port moves 59 digests: every one of them reaches the pusher
candidate search (88 scenarios do; the 29 that never overlap are unchanged), ships / siege / heroes / supply wagons in the Alexander, Cold War, Napoleon, New World, Custom and Tutorial 2 scenarios. (Other agents' economy edits moved further digests in the same tree; they were separated with `RON_OLD_BOAT`.)
**Add-ons (same pass)**: (a) gaia birds in `do_air_physics` / `do_air_patrol` (aircraft.cpp): the altitude reroll draws only for non-bomber, non-helicopter units that are not animals (or type 0x193); other animals take 0x640 (+200 for 0x192, -200 else); a bird's patrol skips the carrier-offset waypoint and, once `do_air_physics` returns, sets `spell_time = 1` and returns without targeting. Changes the RNG stream: 138 of 156 stock digests move (every scenario with birds); the 18 unchanged: barbariantactics, capturedarius, greece2, jhelum, pillage, sogdiana, thrace, thrace2, tyre, yemen, militia, porkchop, lowrationsruss, lowrationsturk, malta, newmontenotte, cegeneral3, capture. (b) `process_leader_options` @9441d0 (commands.cpp): a changed citizens' stance (+4) goes to the citizens (types 0x32 / 0x33, `Unit::set_stance` down the chain) and the buildings of stance type 1, the military stance (+0xc) to stance-type-0 units and buildings, the option bits 1 / 3 / 4 to marines, packers and casters; no stock digest changes (nothing sends it within 600 frames).

❓ list of this pass: (1) `detect_boat_collision` reads the type vtable's slot +0x110 as `is_tank` (movement-spec 10.5 lists tanks; the UnitType vtable would not dump); (2) the diplomacy test compares with `leaders[me] +8`, taken as the leader's own index; (3) the sparse
`find_units` distance is `vector_dist(unit - query)` minus the unit's own radius (push_size for ships, big_radius), the register arguments of the call were not visible; (4) `Unit::update_patrol_order(-1)` takes the first GROUP_PATROL in execution order (the original walks the circular
list from `head->next`; a unit never holds two); (5) the `ScenarioData::ignore_orders` prelude of the patrol / attack-ground group actions is not modelled (as in the other group actions); (6) CHAT: the per-leader mute flags (`CHAT_SET`) and the single-player cheat-string parse are not modelled,
the line is delivered to every recipient in the mask; (7) the control groups are not in the native save (the original saves them); (8) the GROUP_PATROL / ATTACK_GROUND orders reuse `Order` members (`wp`, `oxx / whose / id / form_id`, `tolerance`, `attempts`) so the save layout did not change.
Not done: the air garrison panel (IFaceAirGarrison: 3 rows x 2 columns of ready / fuelling counts; the click options FIGHTERS / BOMBERS / HELIS / ALLAIR need `Options::exec` decoded) and the library's final-techs view.

## Random games (docs/randommap-spec.md section 1; engine/sim/newgame.{hpp,cpp}, mapgen*.cpp)

`new_random_game(files, rules, anims, setup, &extras)` is `Setup::build_game` for a random map on top of the generator
(`mapgen::generate_into`) and leaves a `Game` in the same shape `Game::load` does: rules and `GameInfo`, world / regions /
goods / items / mountains, terrain heights, leaders initialised, the empires and herds created as objects, territory,
danger map and first fog refresh done, the scripts (AI production scripts) attached. AI, save / load and the renderer need
nothing else. `RandomGameSetup` carries what the lobby's `GameInfo` does (map style 0..22 incl. the random pickers 0..5, map
size 0..6, seed, 1..8 seats each with tribe (-1 random) / team (-1 or 0..3) / human or AI / difficulty / colour, start town,
resources, start and end age, victory / elimination and their option lists, population, tech cost, rush rules, reveal map,
game rules, game speed, randomize starts, no tribe powers). `RandomExtras` returns the generator `Result`, the resolved setup
and a `Scenario` snapshot (terrain + the finished objects as scenario records) that the renderer draws from.

**Steps, in the order of `Setup::build_game` (section 1) and `Game::run`:**

| Step | Here |
|---|---|
| `Game::run`: `game_random := seed`, `init_starting_resources` (the S1 / S2 draws) | `new_random_game`, before the generator reseeds, as in `Game::load` |
| 1, 3: reseed, **draw 1** `world.seed = rnd(0xffff) + 1` | first thing `generate_into` does |
| 4: `Map::new_map` (random pickers), `Map::make`, `Terrain::init`'s rivers | the generator (`mapgen.cpp`, `make_rivers`) |
| 7: start order (`rnd & 7` with `randomize_starts`) | the generator (`res.start_order`) |
| 7: `Leader::init(who, tribe, who)` in start order | `new_random_game`: diplomacy (team style 12 "Custom Teams" with teams, else 1 "Survival of the Fittest": locked, everyone else at war), starting techs, economy (starting stockpile), attrition, `leader_init_ai` (personality draws, production script name), then difficulty / name / flags |
| 9: `build_empire` per seat (team mates consecutively) | the generator draws `place_unit`'s ring draws and lists the placements; `create_empires` creates them in list order: the city (`init_build_site`, `build_activate_full`, `update_seen_ally`, `compute_all_territory`), its buildings through the AI's `produce_building(t, city, 0)`, the units with `init_unit` (+ `unit_come_out` / `add_gather_order` at the first farm for the town-start citizens, `unit_go_inside` the university for Greek scholars, `build_train` when no ring spot was found) |
| 10: `Herd::create_units` | `create_herds`: `init_unit` for gaia, spread with `find_nearby_spot`, angle from the generator |
| `World::analyze_map`, `compute_all_territory`, `calc_danger`, `update_all_seen` | end of `new_random_game` |
| scripts (`Leaders::init_production_script`) | `script_host_load` with an empty scenario: the AI production scripts compile, there is no scenario script |

**Approximate or missing (nothing here is bit-exact with the original yet):**
- The generator's terrain-group placement is not bit-exact (see `randommap-terrain-groups.md`); the whole map is therefore only
  plausible, and a seed does not give the original's map.
- `Leader::init`'s personality draws and the objects' `init_unit` draws (`Guy::init_real`) come **after** all the generator's
  draws; the original interleaves them (start order, then `Leader::init`, then each `build_empire`'s `place_unit` draws with
  the units created in between). Same draws, other order: the game is deterministic, not the original's stream.
- **The generator leaves `TData`'s water tcoords unset** (it works on `WData.land`; 638 deep wcoords gave 0 water tcoords on
  Great Lakes / size 2). `newgame.cpp` `fix_water_tcoords` makes every tcoord of an open-water wcoord water (`mask & 0x30 == 0x20`)
  right after generation, which the passability, the terrain and the AI read; it runs after the generator's `compute_all_vals` /
  `adjust_for_blocking`, which therefore saw dry tcoords (their map-edge-water BAD_PATH rim is missing), and it is idempotent, so it
  can move into the generator. `TData` COASTAL (0x400) is not set either (on stock files it marks water tcoords, by a rule not
  decoded) and the generator makes no shelf (land 1) or half-water cells.
- Terrain heights are synthesised (`synthesize_terrain`): land 250 +- noise, rivers carved, the template z of each mountain
  added (adjust_for_mountains(0) rule), and the shoreline made from a blurred water indicator because the generator's coastlines
  follow wcoord edges (no shelf / half-water cells yet). `TerrainOut` heights were never decoded. The land doobers are
  placed by a hash (the generator only counts them). No cliffs. The sim reads these heights for z only.
- Not ported: `Herds::process` (herd animals stand still), `Setup::mark_forest`, `Leader::reset_score`, `game.start_index`, the
  Spanish `EXPLORE_MAP` flag and the `info.flags & 4` farm / granary build flags of `build_empire`, the 8-seat "team 8" special
  case of team style 7, CtW maps, `BLANK_MAP` / `SCENARIO_MAP` / `CONQUEST` styles (`new_random_game` takes 0..23).
- The starting citizens of a town start (`n_farm`) are created at the *first farm* of the seat (spec: slot 2001), the others near
  the city; the spec marks this branch as unread.
- Per-seat difficulty: the simulation's `get_diff` reads one `GameInfo.difficulty` outside multiplayer, so the setup's per-seat
  value is stored in `Leader::difficulty` and `GameInfo.difficulty` takes the first AI seat's.
- Random picks (a seat's tribe set to Random, start age "Random Age!") are made with their own generator seeded from the setup
  seed, so `game_random` never sees them; the saved setup text holds the resolved values.

**Save / load.** A save of a random game stores the setup text (`random:style=14;size=2;seed=123;n=2;...;p0=10,-1,1,2,0;...`,
`random_setup_string`) in the scenario-path slot of `META` and the seed in the seed slot; `load_game` regenerates the game with
`new_random_game` (0.1 - 0.3 s) and then overwrites every saved section, exactly as for a scenario. No `Game` member was added
(`sizeof(Game)` is unchanged) and the format version stays 2. `scenario_rel_path` leaves a `random:` string alone.

**Commands.**
```
./build/ron-mapgen <game> --style 14 --size 2 --seed 123 --nations 2 --png map.png     # the generator alone
./build/ronmap <game> --random 14,2,123,2 --frames 600 --census --save-game a.ronsave  # headless random game
./build/ronmap <game> --random 12,3,7,4,r,r,5,9 --all-ai --difficulty 2 --frames 1200 --census --snapshot out.png
./build/ronmap <game> --menu skirmish-setup --snapshot setup.png                       # the setup screen
```
`--random STYLE,SIZE,SEED,NATIONS[,TRIBES...]`: STYLE a MapStyleIndex or a name ("Great Lakes"), SIZE 0..6, TRIBES per seat
(`r` or -1 = random). `--all-ai` makes every seat a computer player, `--difficulty` sets theirs, `--census` prints the per-seat
objects, queues, stockpiles and buildings after the frames. Seat 0 is the human otherwise.

**For `ron-sim`** (not edited by this port; the grammar to add): `--random STYLE,SIZE,SEED,NATIONS[,TRIBES...]` replaces the scenario
argument: `RandomGameSetup s = default_random_setup(); std::string e = parse_random_option(rules, spec, s);` then
`auto g = new_random_game(files, rules, art.get(), s, &x);` instead of `Game::load` + `script_host_load`; `--all-ai` clears
`s.nation[i].human`; `--town N`, `--resources N`, `--age N`, `--end-age N`, `--teams a,b,..`, `--randomize-starts`, `--reveal N`
set the matching fields; save with `save_game(*g, random_setup_string(x.setup), x.setup.seed, path)`. `--frames`, `--ai-every`, the
digests and `--load` of such a save (which regenerates by itself) work unchanged.


## Multiplayer (docs/multiplayer-spec.md section 8; engine/net/, engine/sim/replay.{hpp,cpp}, apps/net/)

Lockstep as the original does it, in four pieces. Nothing in `engine/sim` changed: the layer sits around `CommandPackage::process`, `Game::do_frame` and
the DataWalk check sums.

- **Replay reader** (`sim/replay.hpp`). `read_replay(path, Replay&, error)` decodes any `.rcx` of the corpus (95 / 95 identical to `rcxdump.mjs`: packages, commands, CHECK_SUMS rows, frames,
  where the stream starts, per-verb counts and a fingerprint of every record, `node apps/net/rcxcompare.mjs <dir> <ron-net survey --json file>`); 1.1 s instead of 28 s for the 59 multiplayer files. A package's `plain` bytes are
  what `process_all` sees after the de-XOR; `wire()` gives the commands without the padding. `replay_to_engine` / `engine_to_wire` convert the 36 verbs the engine has a `Command` for (the field mapping of
  `apps/sav`'s `--replay`), the others stay raw. Each wire command is `opcode + packed arguments`; `replay_command_length` walks a package without decoding. `ron-net survey --codec`: 104,206 engine commands round-trip byte for byte (2 LEADER_OPTIONS differ in a word `Command` does not keep), 172,278 obfuscated packages round-trip.
- **Session** (`net/lockstep.hpp`). One per peer; `submit` / `step`. Turns and stamps, `turn_length` from the TURN_DATA rule (6 at speed 2, 2-5 humans), speed changes, CHECK_SUMS / TURN_DATA / CAMERA written by the layer,
  the late-package wait, pause with stamps going on, drops with a final-stamp agreement (REPLACE_WITH_AI / DROP_PLAYER / DIPLOMACY / SURVIVAL), resign and quit, desync detection with the failure threshold, observers.
  `SimHooks` is the whole contract with the simulation: `frame`, `step_frame`, `checksums` (15 + `all`), `begin_package` / `execute(slot, cmd, len)` / `end_package`.
- **Transports**. `LoopbackHub` (N endpoints in one process, deterministic delay and jitter, `hold` / `kill` / `kill_after_sends` for stall and drop tests), `TcpTransport` (sockets only, host = slot 0 relays),
  both reliable and ordered per sender; `Lobby` is the host's key/value table, ready flags, START with the host's seed, a barrier.
- **OOS recovery** (`net/recovery.cpp`, `SessionConfig::recover`; multiplayer-spec §8.5.1). A desync freezes every peer on the same row of sums; the authority (the host for two peers, else the peer agreeing with
  the most others) sends its `save_game_bytes` snapshot in zlib chunks, every peer (the authority too) restores it (`GameHooks::restore` -> `load_game_bytes`, the application swaps its `Game`), a `RecReady`
  barrier separates the epochs, stamps restart at 1, `step()` returns `Recovering` then `Recovered`. An authority that dies in transit is replaced (`RecNeed`: whoever loaded the snapshot serves it).
- **Recording** (`net/record.hpp`, §8.5.2). `Recorder` writes the original's `.rcx` layout from `Session::on_package`: header, random-game info, the exact rules image (or an embedded native save,
  `String "RoNNativeSave"` + length, for a segment that starts from a save), records with the MP XOR and padding, gzip on close; a recovery starts the next segment. `read_replay` reads them (embedded native save
  located exactly), `rcxdump.mjs` reads them too (identical numbers), `ron-net replay` replays them (`apps/net/native_replay.cpp`).
- **Lobby and session in `ronmap`** (`apps/map-mac/lobby.{h,mm}`, `frontend.mm`; ui-port.md §10.3). `mp::Room` (TCP lobby mode, names, ready flags, chat, table for late joiners), `mp::Runtime` (`Session` +
  `GameHooks` over the renderer's `Game`), `mp::CommandSink` (the local `CommandManager` hands its queue to the session through an explicit instantiation of its private member instead of executing it), pause / speed /
  resign / save as lockstep commands, a synchronized save (`Session::request_save`, `kSaveAt`) and a saved game continued by re-joining peers (`Lobby::send_file`).
- **Third pass** (multiplayer-spec §8.6). `net/lobby.*` gained the seat rules (`SeatPlan`: players in joining order, computer seats, observers above the nations; guests' requests for nation / team / colour / handicap / watching with the host's rules and approval; kick; countdown; team and
  private chat; names), `net/seat_transport.*` maps machines to game slots under the unchanged `Session`, `net/discovery.*` is the LAN browser's UDP announcements, `apply_mp_seats` makes a Game a multiplayer game (`Game::multiplayer`, lobby names, handicaps), scenario games and Conquer the World
  battles are played by a room (`mp::ctw_*`). Engine-side finding: a CHAT with a recipient mask desynchronised the machines outside the mask (the presentation event is saved state); `GameHooks::execute` queues it identically everywhere and the application filters at display time.
  `build-mac.sh` passes `-ffp-contract=off` (digests of all 156 stock scenarios, the 105 `ron-sav --engine-roundtrip` runs and the corpus replays are unchanged by it).
- **Engine binding** (`net/game_link.hpp`). `GameHooks(Game&, seat_who, ChecksumMode, ExecPolicy)`: commands run through `CommandPackage::process(g, cmd, play, who)` (the group state resets per package); `ChecksumMode::Original`
  = the 15 DataWalk parts of a game loaded from an original save (`EngineProvider`), `Native` = the CRC-32s of the sections of the engine's own save (any game, e.g. a random game); `ExecPolicy::sav_compatible()` leaves out the
  aircraft verbs `ron-sav` does not feed, `all_engine()` also runs RESIGN / QUIT / drops on the leaders (`leader_defeat`, `kHuman` cleared for an AI takeover).

Harness (`build/ron-net`, `build-mac.sh` builds it last):

```
ron-net survey <file.rcx|dir> [--json out.jsonl]      reader numbers (compare with rcxdump: apps/net/rcxcompare.mjs)
ron-net timing <file.rcx|dir> [--delay a,b]           turn clock only: Session over loopback fed the stamps / PAUSE / RESIGN of the replay must reproduce the frame of every record
ron-net replay <file.rcx> [--game DIR] [--frames N] [--delay a,b] [--observers K] [--all-verbs] [--no-art] [--verbose]
                                                      engine instances in lockstep, one per human, each fed its own player's packages; prints the lines of ron-sav --replay and
                                                      "lockstep: ... the instances agree on all 15 parts at every turn" + "final state ... identical" (exit 1 if not)
ron-net selftest [--seed N] [--trials K]              toy simulation: agreement, stall, duplicate / gap, pause, speed, resign, drops x 4 options, desync + threshold, delay independence; the lobby's seat rules, the seat transport, a room over TCP,
                                                      LAN discovery, CHAT_SET; engine instances with an observer / handicaps / chat, scenario games
ron-net host <port> --random STYLE,SIZE,SEED,NATIONS | --scenario FILE [--seed S]  [--humans N] [--frames N] [--save F] [--recover] [--record F] [--diverge-at FRAME]
ron-net join <port> [--address A] [--frames N] [--save F] [--recover] [--record F] [--diverge-at FRAME]
                                                      a LAN game of N processes (127.0.0.1 by default); a bot plays each seat; the saves must be byte-identical
ron-net loopback (--random STYLE,SIZE,SEED,NATIONS | --scenario FILE) [--humans N] [--observers K] [--handicap a,b,c] [--chat-test] [--frames F] [--recover] [--diverge FRAME:PEER]... [--record BASE] [--save F] [--delay a,b]
                                                      N engine instances of one game in this process (loopback transport); the recovery and the recording without sockets
ron-net replay <file.rcx> [--save F]                  also replays recordings of this port (embedded native save, or the random game of the header) and writes the final native save
ronmap --host PORT --humans N --random ... [--frames N --snapshot f --save-game f] | --join HOST:PORT [...]     (also --mp-bot --mp-record F --mp-diverge FRAME --mp-act what@TURN --mp-save F --mp-load F)
```

Results (this session): `ron-net timing` on the 59 multiplayer replays: 172,276 packages, 86,144 turns, 0 frame differences. `ron-net replay` on the 45 replays with an embedded save: instances agree 45 / 45 (15,334
turns, delivery delays 0..3 and 0..25 ms), final states identical 45 / 45, the fidelity lines `diff`-equal to `ron-sav --replay` for 45 / 45 (still 0 turns with all 15 parts equal: the engine's fidelity work is the other task).
`ron-net selftest` 91 / 91 with the third pass (38 before; seeds 1..12, 30 drop trials per option; recovery: 7 cases, repeated, authority deaths; the engine part: a divergence repaired in two instances, recording chain, replay of the recordings
byte for byte). LAN: 2 processes 600 frames, 3 processes 1500 frames, scenario, and a `kill -9` of one of three processes (20,000 frames): saves identical; 3 processes with one corrupted at frame 150: majority vote,
recovered, identical saves; `ronmap` 2 and 3 processes (`--host` / `--join`): identical saves, recordings replay to them, recovery, synchronized save and continuation of a saved game.

Not done: the x86-64 / arm64 soak, the 14 corpus replays that start without an embedded save (only the turn clock covers them; recordings *of this port* without a save replay: the random setup is in the header),
diplomacy / team reset of the DIPLOMACY and SURVIVAL drops in the engine, saved games other than random maps. (Chat in a running game, the speed / pause / control-group verbs and the other console verbs became engine commands in the second orders pass: see "Orders and commands, second pass".) Build note: `build-mac.sh` passes `-ffp-contract=off` since the multiplayer third pass (multiplayer-spec §8.6.8).


## Conquer the World (docs/ctw-spec.md; engine/sim/ctw*.cpp, script_funcs_ctw.cpp; 2026-10-24)

Roadmap phase 7. The strategic layer is **not lockstep** (spec section 0): it runs on one machine between battles with its own
`Random`; only the battle it hands off (`new_random_game` / `Game::load`) is the bit-exact simulation. Nothing here touches
`Game::do_frame`: `sim-all 600 --ai-every 8` gives the same final digest on all 156 stock scenarios with and without the CTW
builtins installed (checked by building `ron-sim` twice from one tree, the second without the `install_ctw_funcs` call).

| File | What |
|---|---|
| `ctw.hpp` | the whole API: rules / campaign data, `ConquestGame` (spec 2.1), `LinkChoice`, `StyleChoice`, `BattleDef`, `DiploOffer` |
| `ctw_data.cpp` | `load_conquest_rules` (Data/conquestrules.xml: 38 card types with their six DATA, stances, attitude bands, 18 default styles, ARMIES, BUILDINGS_*, RESOURCEBONUS_*, MIN_NO_ATTACK), `load_campaign` (ROOT attributes, COUNTRY / LINK, COLONY / RARE, CONTINENTS, BONUSCARDDECK, VALID_NATION, TRIBES with the nation files' names / LEADERS / CITIES, CONQUESTSTYLES), `validate_campaign` |
| `ctw.cpp` | setup (2.2), the round (2.3: `end_turn_player`, `begin_ai_round`, `ai_step`, `finish_round`, `advance_round`), armies and supply (2.4: `army_cap`, `place_new_armies`, `move_army`, `can_drop_army`), `find_available_links` (5.1), `transfer_to` / `defeat` / continents / allied victory, `check_income`, `assign_bonuses` (2.5) |
| `ctw_diplo.cpp` | stance matrix and setters, attitudes (`get_attitude`, names by band), the cost functions (3), `buy_war / peace / ally / vassal / territory / fortification`, `demand_tribute`, `offer_bribe` (the spec's accept probabilities), the AI's diplomacy |
| `ctw_cards.cpp` | the 38 cards (4): names and variants (Oath 24 per tribe, Eureka 5, Economic Boom 5), the shuffled deck (`make_card_list`), `buy_card`, `card_targets`, `play_strategic_card` (Wonder, Sabotage, Treachery, Commandeer Transport, Brigands, Memnon, Spitamenes, Colony Blockade, Continental System, Letter of Marque, Fouche, Talleyrand, Paoli), tactical selection, the AI's card use |
| `ctw_ai.cpp` | `plan_strategy` (heuristic), AI-vs-AI attack resolution (6: 95 / 98 / 80 / 20 %, overruns), army repositioning |
| `ctw_battle.cpp` | `pick_style` (5.1, own seeded Random per link), `make_battle` (5.2: `GameInfo`-shaped `BattleDef` + `RandomGameSetup`), `build_battle` (5.3), `process_results` (6), `resolve_player_defense` |
| `ctw_script.cpp` | the campaign's own script host (`ScriptRuntime`: a never-simulated `Game` + a `ScriptHost` whose `ctw` points at the campaign) and the hooks, deal structs, script statics for the save |
| `script_funcs_ctw.cpp` | ~110 builtins (below) on the campaign state, installed last by `install_script_funcs` so they replace the stand-alone answers |
| `ctw_save.cpp` | the campaign save (7) |

Hooks in existing files: `RandomGameSetup::ctw` (+ `g.ctw = S.ctw` in `new_random_game`; not part of the text form), `ScriptHost::ctw`
(`script_host.hpp`; `save.cpp`'s size assertion became 1616 for the extra pointer, the pointer is run-time only), one call in
`script_funcs.cpp`, one declaration in `script_funcs_internal.hpp`.

**Campaign state.** `ConquestGame` keeps the spec's saved subset: scalars (`cur_round`, `cur_age`, `difficulty`, `seed`,
`turns_no_attack`, `game_end`, `style_index1/2`, `defcon_level`, ...), `leaders[24]` indexed by slot (stances / `stance_time` /
`attitude_base` / `no_attack_time` / `bribe_paid` matrices, `vassal_of`, hand and selected cards, rare resources, wonders, no-attack
lists), `nodes` (owner, level 0..7, `turns_held`, flags 1 fortified / 2 capital / 4 supply depot / 8 visible / 0x10 priority,
links with their `sea` flag **per direction**, bonus card, tileset, tech category), `colonies`, `pieces[24]` (armies, at most one
per node), `reinforcements`, `valid_tribes` (the AI order of the round in progress), `stored_ints / stored_strs / deals`, news,
the deck and its shuffled draw pile, the style table's `times_played`. `random` is the campaign stream: `rget(lo, hi)` is
**inclusive** (the spec's note; `sim::Random::get` is half-open, `rget` adds one). Not saved: the files (re-read on load), the
available links (recomputed), the script runtime (rebuilt on demand).

**Setup** (`setup`): nodes in file order, `tech_cat = (i + rget(0,5000)) & 3`; the card pool shuffled by a key `rget(0,5000)` per
variant; `tribute` / `rare` / `wonder` / `card` of a country resolve `random` as the spec says (rare 6..49 re-rolled on 20, and on
6 / 31 with random bonuses; wonder = a building type 526..542; tribute `(rget(0,5)+1) x 10`); owners are remapped by a permutation
keyed by `rget(0,1000)` when random nations are asked and the campaign allows them; every slot owning a country or colony gets a
leader (stances peace, `attitude_base = rget(0,5) + 10 - 2`, tribute 25, a name from the nation file's LEADERS, the nation's
start cards, `inherent_skill` from SKILL or `rget(1,100)`); capitals take level `size` (0 -> `ctw_capital_starting_infra` 2),
other owned nodes `size`; the first army on the capital (`base_armies > 0`), supply depots add armies; the player slot is the
chosen one or random among `VALID_NATION`; `turns_no_attack = MIN_NO_ATTACK[difficulty]`; the initialization script runs.
`cur_round` starts at the campaign's `starting_round` (Alexander 1, World 2), so a campaign plays `length() - starting_round` rounds
(`length() = (ending_age - starting_age + 1) x turns_per_age` = 19 / 21 / 18 / 14 / 24 as in the spec).

**The round.** The player moves (`move_army` along links, attacks through `find_available_links`, cards, diplomacy), then
`end_turn_player` (the skip bonus `min((turns_skipped+1) x 25, 100)`, doubled by an Antipater card; `check_income`), `begin_ai_round`
(`valid_tribes` shuffled by `rget(0,0x7fff)`), and `ai_step()` until it reports `kDone`: per tribe the AI plays cards, does its
diplomacy, runs the campaign's `strategy` script **once**, then `plan_strategy` until it finds no attack. An attack on the player
returns `kPlayerAttacked` (the UI plays the battle or auto-resolves, then `resolve_player_defense(won)` and `ai_step()` continues
the same tribe); AI-vs-AI attacks resolve at once (`resolve_ai_vs_ai`). `finish_round` = `assign_bonuses`, `adjust_attitudes`,
`advance_round` (age step every `scen_per_age` rounds: `+1` level on every owned node that is not in its owner's `new_nodes`;
`turns_held`; stance clocks; `no_attack_time` countdown; Talleyrand's reversal; all nodes visible from age 6), `place_new_armies`
for everyone, the `post_ai` and `post_turn` hooks, `check_allied_victory`, the time-over check.

**AI (the spec's "?").** What is spec: the move order, the legal attacks, the player's immunity window (`turns_no_attack`,
`no_attack_time`), the AI-vs-AI win probabilities, overruns (attacker side outnumbers by 2), the army cap, the 98 % rule against
client states, tribute demands (35 % / 50 %), bribes, alliance breaking (`attitude x 5 %` after 4 rounds). What is a heuristic
(`ctw_ai.cpp`, `ctw_diplo.cpp do_ai_diplomacy`, `ctw_cards.cpp play_ai_cards`): which legal attack (score = value + tribute/2 + card +
capital 25 + priority 15 - 6 x level + supporter terms, taken with 75 %), army repositioning toward the nearest enemy, when an AI
declares war (35 % when at war with no neighbour, on the most hated neighbour; free of charge), when it makes peace (15 % after 3
rounds), its card use (50 % per strategic card, no Commandeer Transport), where a new army appears (capital, depots, border). The
campaign scripts' `strategy` / `post_ai` / `diplo_offer` replace parts of this exactly as in the original.

**Battle hand-off** (`pick_style` -> `make_battle` -> `build_battle`):
- *Style.* Category = Offensive (player attacks), Barbarian (against barbarians), Defensive (player defends); candidates per 5.1;
  `Random(seed + to x 0x25 + from x 0x11)` keys `get(0,101)`; a campaign's `battle_script` may name a CUSTOM style
  (`battle_override(from, to, player, enemy, turn, age, strength, reinf, enemy_reinf, allies, enemy_allies, ref info, ref goal1,
  ref goal2)` is called through the script host). A campaign without `CONQUESTSTYLES` (World) uses the 18 defaults of conquestrules.xml.
- *Seats* (`BattleDef.seats`): player first (human, team 0), the other main nation (team 1), in default styles the link's supporters
  (up to 3 per side, allies of the player on team 0), `add_opponents[difficulty]` barbarians; barbarian seats take the first free tribe
  index; at most 8. Special styles drop the supporters (`allied_punks`).
- *Parameters* (`RandomGameSetup`): `starting_technology = cur_age`, `ending_technology = cur_age + 1` below the campaign's last age,
  `starting_town` 2 when the player attacks (0 for the No City / Custom map styles), `map_size` 3 (2-3 seats) or 4 (>= 4), +1 for a
  sea link, or the style's `mapsizes`; the style's `eliminations / victories / revealmaps / rushrules / startingresources / gamerules`
  by category name; `time_limit = 8` (custom) with minutes `min_victory + get(0, max-min+1)` in `scen.custom_time_limit`.
  The battle `seed` is derived per link and round (the original reuses `conquest_game.seed` for every battle; documented deviation).
- *Map.* Custom styles load their `.scx` with `Game::load` (the scenario's player records get the campaign tribes / names), the rest use
  `new_random_game` with the generator style **1 (Random Land) / 2 (Random Sea)**. **TODO(ctw 24-26):** `MapConquest::make` /
  `MapConquestSea` / `MapScenario` (the node-shaped terrain, `check_starting_loc`, `check_sea_map`) are not in the generator.
- *After the build:* the campaign stances become team diplomacy, base rates / rare bits from `assign_bonuses`, the defender's
  start-city build-out from `BUILDINGS_<difficulty>[level]` (`alt` substitutes, age-gated, the player defends with
  `DEFAULT_BUILD_DIFF`) and the resource multiplier, one `<ARMIES>` composition per army (upgraded with `current_upgrade`), the hero of
  `hero_unit` (default styles, land links), the style's `setup` script once and its `runtime` as the scenario script, `g.scripts->ctw`.
- *Tactical cards applied:* Oath, Propaganda (`conquest_racial_powers`), Eureka (techs of a library line; variant 4 = all four),
  Economic Boom (`num_ctw_rate_bonuses`), Mercenaries (+1 army), Skill Bonus (+100 of each good: **?** the real amount is the style's
  `bonus` script), Spy Network (leader flag 0x1000), Cultural Dominance / Political Dissidents (`flags2` 0x40 / 0x800 on the other side),
  Warpath (2 military techs, Barracks, Stable, Siege Factory). `num_bonus_cards[type]++` is set for all, so the simulation's readers see
  them; **? not wired:** the rate readers of Great Thinker / Merchant Guild / Logistics / Partisans / Trade Embargo / Missionaries,
  the card units (elephants, generals, spies, citizens, commandos, Traitors' informers).
- *Reinforcements* are computed (2..4 minute steps per side) into `reinforcements` but **not spawned** in the battle (TODO).
- *Outcomes* (`process_results(kind, won, link, style)`): kind 0 played, 1 attacker wins without a battle, 2 forced loss; selected cards are
  consumed (Parmenio stays after its first play); player attacks + wins -> `transfer_to` (tribute once, card), the army moves in, a client
  state becomes a vassal, `has_fought_barbarian`; loses -> the AI defender gets 50 tribute; player defends + wins -> `need_defensive_bonus`
  and the failed army is lost; loses -> the node goes to the attacker (the capital: elimination, `game_end = 2`).

**Scripts** (`ScriptRuntime`). Hooks and entry points: `init_script()`, `links(string tribe)` (true = only the links added with
`ctw_add_link`), `strategy(string tribe)` (true = the script played the turn; `ctw_ai_set_attack` / `ctw_ai_move_army`), `post_ai()`,
`post_turn(int turn)`, `battle_override(...)`, `diplo_response(ref ConquestDiploOffer)` (the deal evaluator: `Peace / Ally /
Vassal / Exchange`, sets `accepted / response / counter_offer`), `diplo_offer(ref ConquestDiploOffer)` (story offers of the AI nations,
stored in `proposals`). The struct values are built from the compiled script's parameter type id and `type_hash("string[]")`. `rand_int`
inside a hook draws from the campaign stream (the hook's `Game::rng` is seeded from it and written back), `popup_dialog` / notices
become `ConquestGame::messages`, the scripts' `static` variables are saved (`script_statics`). Builtins implemented: stored values
(`get/set_ctw_int / str`, `clear_ctw_val(s)`), clock (`get_turn / get_age / get_ctw_campaign / ctw_get_num_campaign_turns /
get_ctw_seed / get_difficulty`), ownership (`get_country_owner`, `get_territory_owner` x2, `get_num_territories`,
`ctw_territory_list`, `ctw_get_nation_capital`, `ctw_give_territory`, `ctw_territory_captured`), tribes and stances (`ctw_is_war / peace /
ally / vassal / defeated`, `ctw_get_master`, `ctw_set_war / peace / ally / vassal`, `ctw_buy_*`, `ctw_get_num_allies`,
`ctw_ai_no_declare_war`, `set_no_attack` x4 overloads, `ctw_set_leader_name`), tribute and cards (`get_num_tribute`,
`give_player_ctw_tribute`, `give_ctw_tribute`, `give_player_bonus_card`, `ctw_bonus_card_list`, values), armies and links
(`ctw_give_army`, `ctw_remove_army`, `ctw_remove_all_armies`, `ctw_territory_has_army`, `ctw_add_link`, `ctw_territory_link`, strength get / set,
`set_territory_display_name`), colonies, deals (`ctw_deal_accepted`, `ctw_set_deal_accepted`, `ctw_clear_offer`, `ctw_reject_bribes`,
`ctw_clear_bribes`, `set_player_no_negotiate`), the end (`ctw_game_over` x2, `ctw_custom_end_screen`, `ctw_set_sub_goals`,
`ctw_center_camera`), UI switches, Cold War numbers (defcon, nukes, `ctw_build_nuke`), battle reads (`get_attack_to / from /
terr_str`, `get_starting_loc_x / y`, `enable_type_by_tribe_with_type_name`). **Not implemented / ?:** the Cold War launch rules
(`ctw_ai_launch_nukes` returns 0), espionage, `ctw_reinforcements_arrived`, `get_conquest_invasion_dir`.

**Save** (`ctw_save.cpp`): the engine's archive (`save_archive.hpp`: header string `RonCTWMapSave` + version 0x10, then CRC-checked
sections `CTWG` scalars + RNG state, `CTWL` leaders, `CTWN` nodes, `CTWC` colonies, `CTWP` armies / reinforcements / AI order,
`CTWD` deck, `CTWV` the script statics, `CTWS` script storage / news / style play counts / proposals), one `walk` per struct for both
directions. Not the original's layout (state-walk.md). Load re-attaches the files and recomputes the links; a save/load/save is
byte-identical and the loaded game plays on identically.

**Tools and results** (`ronmap`, no window): `--ctw-check` loads the five campaigns (countries 26 / 51 / 32 / 47 / 59 in list order
Alexander, Napoleon, New World, Cold War, World; links 106 / 232 / 128 / 182 / 218, all resolving; colonies 0 / 14 / 0 / 0 / 0; styles
44 / 52 / 55 / 60 / 0; rounds 19 / 21 / 18 / 14 / 24) and sets each up; `--ctw-sim CAMPAIGN [--seed S] [--rounds N] [--ctw-out FILE]`
plays AI rounds (same seed -> identical save bytes for all five, 3 rounds); `--ctw-saveload CAMPAIGN`; `--ctw-selftest` (21 checks:
costs, determinism, `pick_style` purity, lengths / income / age steps, the six outcome cases, battle description, save round trip);
`--ctw-battle CAMPAIGN:auto|auto:N|FROM>TO --frames N [--all-ai] [--snapshot x.png]` builds the hand-off battle in the real renderer
(`CTW_CARDS="Warpath,..."` gives and selects cards). 60 hand-off battles (12 links x 5 campaigns, all style kinds incl. 2-8 seats,
custom maps, sea links) ran 600 frames each; the style scripts run (about 1200 script runs per battle, 0 failures). Builtins the
battle scripts still hit unported: `group_guard_order`, `set_difficulty`, `num_good_collected`, `resource_cap_warning`,
`citizen_to_militia` (not CTW-specific: the scripts task).

**Spec notes / contradictions found.** (1) `<LINK from="0|1">` in the Cold War file is a flag, not an origin territory; ignored (?).
(2) `Random::get` inclusive vs the engine's half-open `get`: `rget`. (3) `starting_round` means a campaign plays fewer rounds than
`length()`. (4) the fortification price is spec "20 + 10 x level" but the constant is named `CTW_BUILD_COST_TIME_HELD_X`: the spec's
level is used. (5) `Elephant Army` is the card type's name but `Elephant Brigade` its key (campaign files use the key); keys are used
for lookup. (6) a country's `rare="random"` range 6..50: goods end at 49, so 6..49. (7) World has no `CONQUESTSTYLES`: the
conquestrules.xml defaults are used (the spec's "styles 0"). (8) the `diplo_offer` default script is `return false`; only
Napoleon / New World / Cold War scripts make offers.

### Conquer the World, second pass (2026-10-24)

New files: `ctw_reinf.cpp` (reinforcement geometry, schedule, arrival), `ctw_tactical.cpp` (AI card picks, `apply_battle_cards`, `run_bonus_script`),
`ctw_cold.cpp` (nukes, DEFCON strikes, espionage). Rewritten: `ctw_ai.cpp` (`plan_strategy`, overruns), `ctw_diplo.cpp` (`set_war` / `set_peace` /
`set_ally`, costs, AI diplomacy, `propose_peace`). The campaign save is version 0x11 (reinforcements, nuke state, deals tri-state, saved names).
Hooks in core files are minimal: `costs.cpp`, `attrition.cpp`, `build.cpp`, `script_host.cpp`, `script_funcs_order.cpp` (`group_guard_order`).

Deviations: `group_guard_order` approximates Group::action_guard; the Dissidents card's `lose_tech` is reduced; a player retaliation uses the AI's
target heuristic; the original's popups are dropped; `set_plunder_scale` and `rename_city` are still unported. Debug hooks (environment): `CTW_STYLE`,
`CTW_FORCE_WIN`, `CTW_VARY_SEED`, `CTW_CARDS` in `apps/map-mac/ctw_cli.mm`. Campaign state is non-lockstep; only the battle is bit-exact, and
`sim-all` digests are unchanged. Checks: `ronmap --ctw-selftest` (46 checks), `--ctw-sim` Cold War over 14 rounds (the AI launches nukes).

## Fidelity log (Phase 0: the engine against the original's frames; docs/original-save.md sections 6 - 7; 2026-10-24)

Method: `ron-sav <rcx> --replay` steps a save-based multiplayer replay from the embedded save and compares the 15 CHECK_SUMS parts at every turn
boundary; `--step-diff N` (what the engine changes), `--compare-next` (full-state and RNG-distance comparison with the next segment's save) and the
Adler-32 searches of docs/original-save.md 6.1 (`--solve-at B`, `RON_REVERT_SEARCH`, ...) turn a differing hash into an object and a member. The
originals are decompiled with `re/tools/ghidra-run.sh DecompileFunctions.java re/jobs/fid_*.txt` into `re/decomp/fidelity/` (jobs in `re/jobs/fid_*.txt`).
"Before -> after" is the first differing boundary of `21'23'34` (frames 7 - 18,338) unless said otherwise.

1. **Gaia animals and herds** (`engine/sim/animals.{hpp,cpp}`, `Game::herds`). Diff: `--step-diff 1` listed `Unit.flags` 0x01 -> 0x09, `idle`,
   `orders_x/y`, `dest_angle` on 234 of 256 gaia animals; `goods` differed at the first boundary although no engine member of a Good changed. The original runs
   `Animal::process` @5d72c0 (not `Unit::process`): `Animal::work` @5d7330 = the order machine's `do_job` (no `Unit::work` preamble), then for a herd animal every
   16 frames (`(o + frame) % 16`) `goods[herd.good].ever_seen |= bit(p)` for the players that `is_seen` the animal's fog cell; `Animal::do_idle` @5d7460 (vslot
   +0x184) instead of `Unit::do_idle`: `set_anim(DEFAULT)`, `collide = 0`, nothing for naval animals (fish, whales: `domain != 0`); a land herd animal whose guy
   is on the last frame of its cycle draws `% 10 < 3` and then either walks back to a free spot near the herd point (more than 0x180 away:
   `find_nearby_spot(0xc0, -1, 0, 0x55555555, NOT_ME)`) or takes a 1 - 4 ucoord step in a random direction (three draws: direction, x length, y length,
   `detect_unit_collision` then `add_move_order(QUEUE_NEW)`); `Herds::process` @741cc0 (inside the `frame % 32` block of `Objects::process_all`) moves the
   herd's target one wcoord with two draws every 64 frames. The engine had no `Herd` records at all: `Game::Herd` (cx, cy, wx, wy, t, good_o, herd, flags) is
   loaded from the save (`map_herds_array`), emitted again by `EngineProvider` (the `herds` verify entry), saved natively (section GOOD) and walked
   by `herds_process`. `is_animal_who(who >= 8)` selects the Animal path in `objects_process_all` and `do_job`. Result: `goods` 8 -> 2,318 (38 of 45
   replays now keep `goods` equal, was 36), the animals' flags/idle/orders_x/y no longer move. Native games: animals created by `newgame.cpp`
   (`create_herds`, forbidden file) are list 9 with no `Herd` record, so they stand still; the generator owner should call a `Herds::init_herd`
   equivalent (the record layout is `Game::Herd`) and create the herd animals in list 8 with `cara = herd id`.
2. **Bird flight, pen animals, leader slot 9.** `objects_process_all` now gives the new birds their air patrol (`add_air_patrol_order(x, y, -1, -1, 0)`) and
   `animal_think_bird` (@5d79e0, vslot +0x180, the first call of `do_air_patrol` for gaia: patrol point jitter every 8 frames, a new region cell every ~800 frames, 2 - 62
   draws) runs from `do_job(kAirPatrol)`; the loader makes `leaders[9]` valid (not in the save; without it the birds were never processed);
   `think_farm_animal` @5d7700 is ported (pen animals pick a pen cell every `128 / (aid + 1)` frames; `clear_orders` + `add_move_facing_order`). Diff: the
   draw distance of `21'49'39 -> 21'50'35` was -881 after item 1, -644 after this (the birds draw about 1 per 3 frames).
3. **`LEADER_NEW_UNITS` (0x800000)** set by `Unit::init` @612d83 (leaders 0 - 7; `Unit::close` already did) and cleared by `compute_score`. Diff: `--solve-at 200` named one byte
   of `leader_flags` of leader 0 (engine 0x08, original 0x88). `leaders` 200 -> 566 (then `LeaderData.explored` +4 for player 1: scouts).
4. **Walking gatherers mark their building in use** (`Guy::move` @5d9541, the TODO in `guy_move`): a unit with a wood / ore carry mask whose action is a GATHER order
   with `been_there` and an active gather building does `recharging++` and sets `build_masks` 0x800 once a frame. Diff: `--solve-at 8/14` named
   `BuildData` byte +10 (`recharging`) of both players' woodcutter camps, +1 at boundary 8 and +2 from 14 on, constant to 62. `builds` 8 -> 50 (the
   rest at 50 is the order of the camp's cell, item "open").
5. **Idling of moving units** (`Unit::work` LAB_0060d733): the IDLING flag is cleared for move orders too (the engine cleared it only for the other orders).
   Found by reading, no effect in the three replays.
6. **`Unit::update_speed` @6055c0 complete**: whales (objmask 0x2000 with the rare bit), Bantu, French and Versailles siege, aluminum air speed, spy, hero, supply and
   Aztec terms in the original's order (the engine had the type tweaks and the hero term only). The compare-next of a segment showed `Unit.myspeed` 26 vs 25 on 59 units.
7. **`AnimalData::is_seen` @5d8160**: the herd branch (the herd good's `ever_seen`, fish / whales / Spanish / Nubian rules) and the pen animals ask their farm;
   `explore_goody` is skipped for both gaia lists.
8. **The replay driver** (`apps/sav`): the game script of the header (`no_nukes`) is compiled for the loaded game (`original_attach_game_script`), `UNITMASK` is
   applied (`Group::action_unitmask`), the aircraft verbs are mapped. New diagnostics: `--solve-at`, `--shift-search`, `--pshift-search`, the `RON_*`
   searches, the draw distance and the call-site profile (docs/original-save.md 6.1). `engine/sim/random.hpp` has a profiling hook compiled only with
   `-DRON_RNG_PROFILE_HOOK`.

**Checks** (scratch build of HEAD + these files with the build-mac.sh flags, zero warnings): `ron-check` 0 failures; `ron-sim ... --frames 600 --ai-every 8` on
the 156 stock scenarios 156/156 self-check OK; `--roundtrip 200` 156/156 OK; `ron-sav --engine-roundtrip` 69/69 PASS (10 saves + 59 embedded; 0 verify mismatches).
**Digests**: 152 of the 156 final digests changed (`porkchop`, `newmontenotte`, `cegeneral3`, `capture` did not; 15 scenarios end at frame 1). Reasons, by switching
the files back one at a time (measured before items 2 and 7 were added): item 4 (`guys.cpp`) alone accounts for 120 (`Build.recharging` is in the digest), the
`orders.cpp` changes (animals' `do_idle`, items 1 and 5) for 31 more, `LEADER_NEW_UNITS` for one, `update_speed` (item 6) for none (no stock scenario has a tribe or tech term
on a moving unit at frame 600); the bird flight and pen animals add the scenarios with gaia birds. None of the changes touches determinism (the self-check passes).

**Open / blocked** (see docs/original-save.md section 7): the first mismatch of `21'23'34` is the movement of ten walkers in frames 8 - 14 (`units` at 8 has no single-member
explanation; `guys` / `world` match at 8 and differ at 14), and the other two replays start in a fight with many differences in the first step; the engine
under-draws by 1 - 2 random draws per 10 frames in quiet segments (not-yet-ported call sites, e.g. `Unit::think_scout`, `Objects::add_flock`); `GUARD` / `FOLLOW` have no verb
(`commands.*` belongs to another owner); the leader statistics, command counters and Guy pitch / bank are still frozen (no first mismatch points at them: the first
MOVE_TO of `21'23'34` is at frame 3,320, the first fight later than the first mismatch of `leaders`); the capacity rules of the containers are in docs/original-save.md section 7.

### Fidelity log, second pass (2026-10-24, movement / `units` at the first boundary)

Target: `Multi/Playback - 2026.08.30 21'23'34 (Sun).rcx`, `units` differs at 8, `guys` / `world` at 14. Decompiled into `re/decomp/fidelity2/` (jobs `re/jobs/fid2_*.txt`; disassembly where
the decompiler lost `sinx` / `cosx` / `vector_dist` / `project` / `find_nearby_spot` arguments). **No first mismatch moved** (docs/original-save.md section 6); what the round produced:

9. **GUARD and FOLLOW** (`engine/sim/orders_guard.cpp`, `groups.cpp`, `commands.*`, `replay.cpp`, `orders.cpp` `do_job`, `original_map_objects.cpp`, `apps/sav`). `Unit::add_follow_order` @5e3f60
   (the object really followed: the first unit riding in the target, else the target; `oxx / whose / uid2`), `Unit::add_guard_order` @5e3e40 (`dx, dy`, the guard point = the target's position, `idle`, `retry`),
   `Unit::update_guard_order` @5e3220, `Unit::do_follow` @5e65d0 (the container fallback, the follow distance `clamp(los*0x180 - (los*0x60 | los*0x300/5, doubled when the target moves), 0x180, 0x600) + 0xc0`,
   a spot on the line to the target / behind it / on a ring, then `add_move_facing_order(MOVE_TO)` + `do_move`) and `Unit::do_guard` @5e5c70 (retry countdown, a `find_melee_target` scan every 16 frames
   while the target stands, a building: walk within 0x600 as a one-unit group `action_move_near`; a unit: stand at `T + rot(angle, dx, dy)` (`sinx / cosx` order read from the asm), the
   `find_nearby_spot` fallbacks, `ATTACK_TO` move with `timer = max(1, W-cell distance) * 30`, `retry = rnd % 3 + 6`, packers unpack after 0x46 / 0x1e idle frames);
   `Group::action_follow` @6fd510 and `Group::action_guard` @6fcd30 (the QUEUE_FIRST insert pattern, `is_ally`, a building target: every member guards; a unit target: the target's captain, a human
   player's other units already guarding it join, a target guarding one of ours stops, `compute_form(guarding = 1)` lays out the slots); `CommandPackage::process_follow` @9479c0 / `process_guard`
   @9478a0 (opcodes 30 / 31: `i32 ox, i32 whom, queued`). The GuardOrder members live in unused `Order` fields (`dx / dy` = `def_x / def_y`, guard point = `x / y`, `idle` = `timer`, `retry` =
   `pause`) so `sizeof(Order)` and the native save do not change; `original_map_objects.cpp` maps the original's GuardOrder / FollowOrder bodies onto them (round trip 69 / 69).
   Not ported: the `ScenarioData::ignore_orders` prelude; `Game::get_patch_version() > 3` is taken as true. **Unverified**: no hash in the corpus depends on a guard yet; the draw distance of
   the 15 pairs that contain a GUARD / FOLLOW moved both ways (docs/original-save.md section 6). The ground-truth the corpus has for them is the order bodies of later saves (`Unit` with a flag-12 order):
   compare `dx / dy / guard_x / guard_y` once a segment with a guard reproduces its start.
10. **`ron-sav --astar-check`** (the path-finder oracle, docs/original-save.md 6.1): 8 of the 10 distinct fresh U-level paths of the 105 saves reproduced exactly by `find_upath` (A* loop, start direction,
    tie-breaking, cost, closed / open rules, the limit / `fast` cut-off, the straightening), so those parts of `pathfinder.cpp` are right; each variant of neighbour order, start direction, tie order,
    goal halving, strict / non-strict open replacement, closed-before-open, goal tolerance and heuristic made it worse or changed nothing. The two misses are state (a unit created later in the frame blocks
    the engine's route) and a flag-4 transport node.
11. **Diagnostics** (`apps/sav`, docs/original-save.md 6.1): `RON_DUMP_MOVERS`, `RON_TAIL`, `RON_OBJ_LAYOUT`, `RON_TRY_PATH`, `RON_PAIR_SEARCH`, `RON_BIT_SEARCH`, `RON_GUY_TRY`, `RON_RNG_TRY`,
    `RON_COLL_MAP`, `RON_FOG_DIFF`, `RON_LIST_UNIT_DIFF`, `RON_NO_GUARD`, `--astar-check`. They turned the ranges of the `units` hash into labelled bytes and made it possible to test a hypothesis against the full hash
    in about 10 ms (the Adler stream of `units` is 1.5 MB).
12. **Looked at, no change**: `Objects::add_flock` (stub, never called in the sampled segments of the chain: 0 - 3 calls each, not the draw deficit); `Unit::think` (economy / AI half already ported); the
    sites of `Random::get` in the binary against the engine (combat, ammo, farms, markets, birds, flocks, `do_move`, `resolve_unit_collision` all present; GuyOut / Particle / Surf / RiverDoobers use a separate `internal_random`).
    The engine draws half of what the original does over the 18,337 frames of the first segment (230,745 against 458,578, no fights), which is the largest single discrepancy left.

**Checks** (scratch build of the working tree with the build-mac.sh flags, zero warnings in the files of this round): `ron-check` 0 failures; `ron-sim ... --frames 600 --ai-every 8` on the 156 stock scenarios
156/156 self-check OK; `--roundtrip 200` 156/156 OK; `ron-sav --engine-roundtrip` 69/69 PASS (10 SVX + 59 embedded). **Digests**: none of the 156 final digests changed against a build of the same tree without the
GUARD / FOLLOW execution and the Guard / Follow order mapping (the only sim-side change: nothing in the stock scenarios issues a guard or follow).

### Fidelity log, third pass (2026-10-24, the draw deficit of the quiet segments)

Method: callers of `Random::get` (re/decomp/fidelity3/xr_game_random.txt: every reader of `GameAccess::game_random` @c06184, `internal_random` @eb697c apart), per-site engine profile
(`RON_RNG_PROFILE`, `RON_RNG_TRACE=N` prints every draw with frame and seed), 44-pair chain. Caller table: setup / editor / mapgen / Conquest / UI sites are not frame code;
frame-time sites and where they stand: Guy::set_anim (idle 5dac68, attack 5db21d, bird walk 5db339) ported; Guy::init_real ported; Farms::inc_time ported; Animal::do_idle / think_bird /
think_farm_animal / Herd::process ported; Unit::do_air_physics ported **but without the animal branch (see 15)**; Unit::think_scout, do_move, resolve_unit_collision, do_non_flat_gather, do_gather (rnd),
find_gather_tiles, calc_market, Ammo::*, take_damage ported; Objects::add_flock stub (only group moves ending on forest edges and wall hits by siege); GameAccess::rnd only from fight / do_gather;
Leader::* / Army::find_target are AI-only (no AI seat in the chain); GuyOut / Particle / Surf / GraphicEvents / Scene / Game::do_frame's test delay use internal_random.
13. **Load side effects** (`original_post_load_effects`, docs/original-save.md 6.2): leader 9 init + pen animals. 14. **think_peasant reads leader_options +8** (peasants_wait), as think_caravan; the engine used
`opt_peasants` (+4). Per `CommandPackage::process_leader_options` @9441d0 `peasants` (+4) is the citizens' *stance* (Unit::set_stance on change, not ported: commands.cpp), `peasants_wait` (+8) the idle delay (default 2 -> 12 frames).
15. **Not fixed (aircraft.cpp is another owner's), sent to aircraft-port:** gaia birds run the aircraft code; the original returns right after do_air_physics for animals and does not draw the altitude for them.
Scratch measurement of 13 + 15 (draw distance engine - original): `21'57'36` -307 -> +7, `21'53'58` -1698 -> -30, `22'01'46` -2357 -> -150, `21'52'11` -1214 -> -540, `22'00'17` -144 -> +156.
**Not found:** the deficit of `21'23'34` (-228k, 12/frame) and `21'45'58` (-45k, 15/frame), and `21'50'35` (-6k). Ruled out by measurement: auto-explore scouts (none), farms (5 of 29 bare>=5 in both), idle caravan crews
(the 0/3 crews are inside cities, clocks frozen in both), find_gather_tiles reshuffles (gather lists identical in the saves 18,345 / 21,118), pens, market, collisions (0.03/frame in the saves), Leader AI / diplomacy
(human seats return first), per-frame engine rate from the truth state (14/frame at 18,344 vs the original's 33/frame average, but 17 vs 19 at 21,117).

### Fidelity log, fourth pass (2026-10-24, the load path, the fish clock and the road searches)

16. **`Leader::init(leaders[9], 9, -1, 9)` draws nothing** (it is the reset branch, @6e3930 returns at `param_2 < 0`): `original_post_load_effects` no longer runs `leader_init_ai`; it sets
`flags = 0x2000000 | 7`, tribe 0, then the pens' animals (20 draws per pen). The full sequence is the table in docs/original-save.md 6.2. `21'23'34`: `builds` 50 -> 200, `guys` 14 -> 20, `world` 14 -> 26.
17. **The fish clock** (`RON_FISH_TIMELINE`, `RON_INSERT`, `RON_ANIMCHK`; `g_idle_pick_hook` in guys.hpp): the saved idle variants of the 71-fish bursts give the original's generator position, in draws
since the load, at the burst frames of the last ~400 frames before each save (docs/original-save.md 6.3). It validated the load path (first-frame burst of `21'57'16`: 40 draws in the original, 40
in the engine, 7 pens) and located the deficit in time.
18. **Road searches** (`engine/sim/roads.{hpp,cpp}`): `PathFinder::find_road` / `astar_caravan_road` (building variant) / `calc_road_cost` / `valid_roadcoord`, called from the REGEN_FORCE tail of
`BuildType::place_roads` (`wall_mask_me`, `build_start` through `wall_start`'s `after_mask` callback, `Build::process`'s MAKE_ROAD) and the REGEN_TOTAL tail (MAKE_ROAD flag). One game_random draw per
valid neighbour: 17,000 draws in the 2,773 frames of `21'45'58`, 21,000 + 2,300 in the 18,337 of `21'23'34`. The first six searches (frames 7 - 9 of `21'23'34`) are exact: `guys` 20 -> 200, `world` 26 -> 308.
Also fixed: `construct.cpp` called `place_roads(world, type, x, y, who)`, i.e. `add = (who != 0)` and no city ring, for every MAKE_ROAD building. `RON_NO_ROAD_SEARCH=1` restores the old behaviour (A/B).
**Digests**: 142 of the 156 scenario digests (`sim-all 600 --ai-every 8`) change: every scenario in which a building is started or a city re-flags its buildings during the run now lays roads
and draws; the 14 unchanged ones have no construction. Self-check and `--roundtrip 200` stay 156/156.
19. **Looked at, no change**: wild-bird top-up (`have` counts every type-0x192 animal: the `+0x30` slot is a `return 1` stub), `Objects::add_flock` (no call in the 18,337 frames of the first
segment), `Farms::inc_time` (decompile identical), `Guy::set_anim` (three draw sites, all ported), `GameDaemon::process_all` / `calc_markets`, `Leaders::strategy_all` (human seats: `Leader::diplomacy` returns on
flag 4), think_fish (AI-controlled boats only), caravan roads (one-shot probe: +4.2k draws in segment 1, 0 in segment 2).
**Checks** (scratch build with the build-mac.sh flags, zero warnings): `ron-check` 0 failures; `sim-all 600 --ai-every 8` 156/156 self-check OK, `--roundtrip 200` 156/156; `ron-sav --engine-roundtrip` 69/69 PASS;
`ron-net selftest` 38/38 and `ron-net replay` on `21'57'16`: the two instances agree.

### Fidelity log, fifth pass (2026-10-24, `units` at boundary 8, the walker, Object defaults; docs/original-save.md 6.5)

20. **`Stack<PathData>` allocation** (`Unit.path_size` / `path_inc`; `pathfinder.cpp` `emit_path`, `objects.cpp` `unit_init`, `original_map_objects.cpp` path map): `Stack<PathData>::init` @46d720 /
`push` @46d820 / `increase_size` @46e8c0, `Unit::init` @612100 (`if (size < 10)` init). A search whose emission exceeds 10 entries leaves a 20-big stack after the straightening; the engine
emitted `max(recorded, length)`. Fixes `units` at boundary 8 of `21'23'34`. `Unit` grows by 8 bytes (`save_walk.inc` guard; not saved natively).
21. **The checksum walker lost the objects the engine created** (`datawalk.cpp` `each_object`, `Provider::hold_containers`, `EngineProvider`): each element is a top-level walk and `begin_walk()` dropped
the engine-owned object list, so only the first created object of a list (and none after) was in `units` / `guys` / `builds`. `guys` / `builds` 200 -> 308 / 338 on `21'23'34`, `21'57'16` builds 25,571 -> 25,601.
`each_element` (items / goods) keeps the old behaviour: re-asking the base provider there changes the presence of the recorded items.
22. **`Object::init` @647750**: `inside_down_who = who` for units (`unit_init`); a never-used slot has `up_who` = 0xff (`Object::up_who` default; `add_to_world` @64d8c0 does not write it).
23. **Stance in `Unit::init` / `Build::init`**: the options switch (`opt_buildings` / `opt_peasants` / the option bits) instead of the never-loaded `combat_stance` / `worker_stance`;
`set_auto_peasant_level` writes `opt_peasants`. **Digests**: 8 of the 156 `sim-all 600 --ai-every 8` digests change (casters / packers now start with `~(options >> 4) & 1` / `~(options >> 3) & 1`
instead of 0): ColdWar `alaska`, `central`, `hostages`, `mexico`, `poland`, `poland_cz`, `poland_hun` and `Custom/skirmish/skirmish.scx`. Self-check 156/156, `--roundtrip` 156/156 unchanged.
24. **Mapped members**: `Object +0x41 launch_frames` (`Object::launch_timer`) and `Guy +0x44..+0x50 bank / last_bank / pitch / last_pitch` (loaded and emitted; `22'23'40` and `22'52'08` differed in exactly this byte
at their first boundary). Not mapped: `Object +0x44 launching` (`launch_list`), `Guy +0x40 turret_inc`.
**Open** (docs/original-save.md 6.5): one missing `game_random` draw in frames 293 - 295 of `21'23'34` (units / guys / world at 308), `City.land` / `City.filled` -1 at the frame-200 strategy pass, then the unit
drift and the idle citizens of the engine's economy.
**Checks** (scratch build with the build-mac.sh flags, zero warnings in a clean full build): `ron-check` 0 failures; `sim-all 600 --ai-every 8` 156/156 self-check OK, `--roundtrip 200` 156/156; `ron-sav --engine-roundtrip` 69/69 PASS;
`ron-net selftest` 86/86 and `ron-net replay` on `21'57'16`: the two instances agree.

### Fidelity log, sixth pass (2026-10-24, `cities` at 206, the transport masks, the scout fall-through; docs/original-save.md 6.6)

25. **`Leader::plan_strategy` tile loop** (`ai_strategy.cpp`): the original reads `circle_x[k + 1]` / `circle_y[k + 1]` (`MOVSX EDI, byte ptr [ECX + 0xcb7e91]` @6bb6f7, `[ECX + 0xcbb0e1]` @6bb700; every other user indexes
from `0xcb7e90`): tile index `i = 1 .. circle_radius[n + 1] - 1`, land test `i < circle_radius[n]` (@6bb86c). The centre entry is never visited; the engine counted the city building's own tile (+1 `City.land`,
+1 `City.filled`) and cut the land ring one tile early. `cities` first differs at 206 -> 422 (`21'23'34`), 18,381 -> 18,915 (`21'45'58`), 25,577 -> 25,601 (`21'57'16`); boundaries with all 15 parts equal on
`21'23'34` 33 -> 50. **Digests**: 2 of 156 (two `conquest/Alexander` scenarios; the AI's `land - filled` drives its building choices).
26. **`Unit::init` @612100 / `Unit::add_gather_order` @61a5c0 transport masks** (`objects.cpp` `unit_init`, `econ_gather.cpp`): UNIT_CAN_TRANSPORT (0x800000) when `UnitData::transport_type() <= LeaderData::can_transport()` and
`can_ever_transport()` (a scout type `is(0x45)` only with the options' bit 1), plus the tribe-bonus-0x12 masks2 bits (0x80 non-civilian land, 0x4080 scouts) and the Marines bit (`is(0x77)`); `add_gather_order`
used `level >= 1 && land` for its cross-region case. `21'49'39`: `units` first differs at 21,118 -> 21,154 (a citizen created in the first frame). **Digests**: 121 of 156 change (with 25: 121 in all, the two of 25
are among them): `unit_masks` is hashed in the digest and every scenario that creates a unit for a leader with a CAN_TRANSPORT flag now sets the bit; the 35 unchanged are the Chess Exercise / Tutorial /
Custom capture / missile scenarios and 18 conquest maps without such a flag.
27. **`Unit::think_scout` @5f6010 tail -> `Unit::think_civilian_transport(0)` @5f40d0** (`ai_unit.cpp`): with nothing to explore, a land unit whose leader cannot carry it (`transport_type > can_transport`) or that
is not cargo returns 0 unless it auto-explores, in which case `think_scout(1)` picks a random passable cell of its region (T1 draws, 16 re-draws at most). The ferry search of the rest of the function is not ported.
No digest changes (the AI scouts never reach it in 600 frames).
28. **Diagnostics** (`apps/sav`): `RON_DUMP_FARMS`, `RON_DUMP_GAIA` (+ `_WHO`, `_TURRET`), `RON_PERTURB_WDATA`, `RON_INSERT_N=F:n:k` (profile build); the scratch scripts of this pass are not kept.
**Looked at, equal to the decompile / asm, no change**: `GameDaemon::calc_market(s)` (3 draws per redraw; +1 breaks `guys` at 104), `Farms::inc_time` (asm @8d8600 read line by line; no roll of frames 293 - 301 is within
20 of its threshold), `Animal::think_farm_animal` schedule (`o` / `aid` / `frame` off by one break at 62 - 104), chicken and bird animation lengths (+-1 breaks at 74 - 236), `Guy::set_anim` bird / walk / idle
draws and the `cur_time * T / T` of an animation change (asm @5db2b8..5db561), `Guy::inc_time`, `Unit::set_anim`, `Animal::think_bird` / `work` / `process` / `do_idle`, `Objects::process_all` / `inc_time`,
`Unit::find_gather_spot` @5f5170, `Unit::think_peasant` @5f5760, `CommandPackage::process_leader_options` @9441d0, the unit census of `plan_strategy` (`City.free` / `busy` / `gatherers`), `Unit::do_gather`'s farm part,
`Roads::scan_and_kill_stray_roads` (no `game_random`).
**Open** (docs/original-save.md 6.6): the one `game_random` draw the original makes between frames 292 and 302 of `21'23'34` (then a second +1 in frames 306 - 320 and a -1 at 333 - 334: net zero), a mature economy that
draws ~10 numbers per frame more than the engine from a true state, the road-art driven `world` differences of the mid-game replays, the `leaders` `explored` of the scouts at 566.
**Checks** (scratch build with the build-mac.sh flags, zero warnings in a clean full build): `ron-check` 0 failures; `sim-all 600 --ai-every 8` 156/156 self-check OK, native `--roundtrip 200` 156/156;
`ron-sav --engine-roundtrip` 69/69 PASS (45 replays + 24 saves).
