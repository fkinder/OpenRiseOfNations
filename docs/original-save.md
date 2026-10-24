# Loading the original game's saves into the engine

Status (2026-10-24): ✅ `load_original_save` builds a `Game` from a `RoNSave` / `RoNMultiSave` stream (the 10 `.SVX` of the corpus and the 59
saves embedded in replays) · ✅ `EngineProvider` emits the original's byte image of that `Game` and the 15 CHECK_SUMS parts and the whole verify
block recomputed over it equal the save's own, **69 of 69** · ✅ `ron-sav --replay` steps the engine from an embedded save and compares the
15 parts at every turn boundary with the replay's CHECK_SUMS · 🟡 the engine's simulation matches the original for a stretch (leaders, groups,
cities, guys and the parts nothing changes in), then diverges; `ron-sav` names where (section 7).

Code: `engine/sim/original_save.{hpp,cpp}` (decode + load), `original_map.{hpp,cpp}` + `original_map_{objects,leaders,misc,state}.cpp` (the
field maps), `datawalk_engine.{hpp,cpp}` (`EngineProvider`), `apps/sav/main.cpp` (`ron-sav --engine-roundtrip / --replay / --field-report`).
Inputs: `docs/state-spec.md` (the DataWalk), `engine/sim/datawalk.hpp` (interpreter and `Provider`), `docs/replay-spec.md` §11, §14.

## 1. Design: a shadow, field maps and a provider

The DataWalk hashes the in-memory images of the original's structs. Rebuilding every byte of ~5 MB of state from an engine that models only part
of it needs a place for the rest, so:

- **The shadow** (`OriginalShadow`, a `std::shared_ptr` member at the end of `Game`, `Game::original`) is the decoded save itself: the `datawalk::Tree`
  of object images (`Rec`), container headers, locals, buffers and strings, plus the stored verify block. Nothing in it is dropped.
- **Field maps** (`omap::ClassMap`, one per PDB class of the original) list the members the engine models. Each entry is a PDB member name
  (offset and size come from `rise.pdb` through `datawalk::Description`) with one `load` (original bytes -> engine member) and one `emit` (engine
  member -> original bytes), declared once with `Builder<T>::f(name, &Struct::member)` so a member cannot be loaded without being emitted.
  `fx` adds a XOR key (`x/y/z ^ 0x63637`), `fu` forces unsigned, `c` / `raw` take lambdas for unions and nested arrays.
- **Segments** (`omap::Segment`) bind a range of a `Rec` to a map and to the identity of one engine object (`Seg{a, b, c}` = who/o/index). The
  loader creates the binding for every object it builds; `EngineProvider` creates them for objects the engine creates later.
- **`EngineProvider`** is a `datawalk::Provider` wrapping the `TreeProvider` of the shadow. `image(o, off, n)` returns the shadow bytes with every mapped
  entry that overlaps the request **overwritten by the engine's value** (the mapped range never comes from the shadow). The same provider answers
  the structure from the engine where a map says so (`ArraySpec`, `ChildSpec`, `local_fn`, `string_fn`): container lengths and presence of the
  object lists, guys, orders, rally points, mining list, cities / groups / goods / ... lists, the `Stack<PathData>` and queue slots, the writer-side
  locals (`must_walk`, `type`, `has_block`, `order_type`, the 62 `LeaderDataEncrypt` values with their loop index), the World buffers (`tdata`, fog bytes,
  `danger`), a SimpleArray block, the leader's script-name string. Images the engine created after the load have no recorded bytes: zero, as a
  freshly allocated original object, with locals defaulting to 0.
- `emit` receives its bytes pre-filled with the shadow's, so a member the engine models only partly (a stack's allocation size next to its length)
  keeps the rest.

`compute_check_sums(EngineProvider&)` and `compute_verify(EngineProvider&, stored)` are the existing functions, unchanged.

```
load_original_save(files, rules, anims, stream, &report)   ->  std::unique_ptr<Game>   (Game::original = the shadow)
EngineProvider ep(*game);  compute_check_sums(ep);  compute_verify(ep, game->original->verify);
audit_mapping(game)            every mapped entry emitted from the engine against the original bytes
diff_mapped_state(a, b)        which mapped members differ between two states of one save (stepping, or the next save)
```

## 2. Rules

A save carries its own rules block (last item of the walk). The corpus mixes data revisions and scripted games edit the rules while they run, so
the install's tables are often not the game's: of the 69 saves, 3 equal the install's image (`current 2`, `current RN`, one replay), 21 differ in
`constants int[241]` (older data revision, 17 image bytes) and 45 in `type 315 (Nuclear Missile) tribe_mask` (the `no_nukes` script, 16 bytes).
`load_original_save` compares the install's image with the save's, and when they differ **adopts the save's walked fields over a copy of the
install's tables** (`read_image_into`, new in `rules_image.cpp`; `Game::own_rules`), re-encodes to check, and reports the first difference
(`OriginalLoadReport::rules_equal`, `rules_note`). The `rules` part is then computed from the engine's tables (`Constants` is mapped, the types come
from the shadow, which equals the adopted tables).

## 3. What the loader fills (and the missing pieces it rebuilds)

World (size, cells with all 16 walked members, `tdata`, the three fog byte grids, `wcoord_seen`, `danger[8]`, the `CollBlock` pool, the terrain
height field `Terrain::master_land_heights` (4xs+1 by 4ys+1 floats, in the save), mountain ranges from the saved stamps plus the install's
template textures), Game header, GameInfo, `game_random`, `GameDaemon`, `ai_speed/ai_off`, `queue_fail`, market, the 10 leaders (246 members, the
62 encrypted economy values, sites, make list, the three SimpleArrays, production script), every unit slot with its guys, path stack and order
list, every building with queue, mining list and rally points, goods, items, cities (with caravan links), forts, wonders, supplies, caravans,
armies, groups, ammo, corpses, regions with their coordinate lists, farms, unbuilt sites, oil wells, heroes and specials (loaded, not mapped),
`leader_options`. `Object::linked` is recomputed from the per-wcoord lists.

## 4. Field coverage

`ron-sav <any> --field-report` prints this table (walked = PDB members of the class that overlap a raw range of its walk functions; mapped = covered
by an entry; the rest stay in the shadow and are emitted unchanged). Total: **933 entries**, 1006 walked members, **860 mapped, 146 shadow**.
Containers, locals and buffers are handled by the structure hooks, not counted here.

| Class (engine type) | walked | mapped | shadow-only members |
|---|--:|--:|---|
| Game (frame / tick / market / counters) | 35 | 20 | frame_to_break playing loading on_team start_list start_index num_teams num_def_* num_goods num_items num_objects world_villages graphic_tick |
| GameInfo | 37 | 24 | version checksum_* data map_size players max_observers game_speed cannon_times scenario_type script_type mods (the engine's `GameInfo` has no member for them) |
| World | 32 | 14 | map, territory limits, player_reg, resource_reg, forest/mountain/rock sizes, metal/oil/goodies totals, resources, seed |
| WData | 17 | 16 | block (`CollBlock` index: mapped as the has_block local + the block image) |
| Objects header | 8 | 5 | rare_mark wall_mark valid |
| Unit / Animal | 83 | 82 | launch_frames |
| Build | 56 | 52 | launch_frames gpiece infiltrate2 (+ `gather_from` mtn/cliff are mapped, the list by `ArraySpec`) |
| Guy | 42 | 37 | turret_inc bank last_bank pitch last_pitch (floats) |
| Order classes (19, one map each; the flags byte of every class is mapped) | 203 | 170 | Follow, Guard, GroupPatrol, AttackGround: no engine member (flags only); GroupAttack temporary/oxxx/whosoever; AirAttackGround accuracy/attack_unit/total_time/sx/sy |
| PathData, QueueItem, TCoordData, GatherPoint | 13 | 13 | none |
| LeaderData | 279 | 239 | who cannon_time capital_stamp ally_stamp ctw_hero_stamp ctw_hero_retreat_stamp last_spoke last_taunt taunt_frame incoming_taunt* incoming dock_mark oil_well_mark support_stamp nuke_stamp flock_stamp goody_box_techs goody_box_units supported trade_profit units_bribed move_orders_issued column_orders_issued best_pop best_attack best_armor best_move frame_battle average_{death,kill,damage,hit}_rate damage_current_frame {deaths,kills,hits,damage}_fifteen_seconds reg_docks |
| Site, MakeObject, City, CaravanLink, Fort, Wonder, Supply, Army, Group, Ammo | | all | none (Caravan: making_road reset_road; Good/Item: who o z; DeathObj: turret_angles node_flags; Region: resource site goodies) |

**Dropped:** nothing from the save. Every byte the DataWalk reads is in the shadow, and the members above are emitted from there unchanged. What the
engine has and the original image has no place for (never emitted): `Object::ptype`, `is_unit`, `linked` (derived), `Unit::temp_x .. announce_frame`
and `openlist` (pathfinder scratch, outside the walked range), `Leader::combat_stance / worker_stance / science / color` (`color` maps to the u8
`team_color`; `science` is `epoch[3]`; the stances are not in LeaderData), `Game::scen / scripts / events / local_play`, the render-side members.
Five engine members split what the original keeps in two slots: `Unit::rare` / `air_alt` (+0x54: emitted by `ptype->domain`) and `cara` / `doober` /
`hero` (+0x86: emitted by unit type).

## 5. Round trip: 69 of 69

`ron-sav <file> --engine-roundtrip [--quiet] [--no-diag] [--mutate N]`: load, audit every mapped member against the original bytes, recompute the
**whole verify block** (134,499 - 210,209 Adler-32 entries: every subsystem, every world cell, tile and fog cell) and the 15 parts with
`EngineProvider`, compare with the stored values / `TreeProvider`. A mismatch names the walk function, step, object and member, with the first
differing byte (`LogProvider` runs both providers and diffs every provider call). `--mutate N` changes one engine member (frame, a cell, a unit's x,
a building's hits, a guy, a path point, an order, a queue slot, a city, a group, a good, a leader score or encrypted value, a guy count, an
order type ...) and the check fails at that member: the mapped ranges really come from the engine.

| Save | frame | world | verify entries | rules vs install | result |
|---|--:|--:|--:|---|---|
| DO NOT DELETE 2 | 256,648 | 80x80 | 134,499 | differ (constants) | PASS |
| DO NOT DELETE | 180,904 | 80x80 | 134,499 | differ (constants) | PASS |
| East indies final surround | 353,231 | 80x80 | 134,585 | differ (constants) | PASS |
| NO MISSILES | 375,834 | 80x80 | 134,585 | differ (constants) | PASS |
| curent dec 14 | 113,264 | 80x80 | 134,499 | differ (constants) | PASS |
| curent dec 17 | 194,797 | 80x80 | 134,499 | differ (constants) | PASS |
| current 2 | 189 | 100x100 | 210,153 | equal | PASS |
| current RN | 221,518 | 100x100 | 210,209 | equal | PASS |
| east indies 2 (rn | 383,396 | 100x100 | 210,180 | differ (constants) | PASS |
| show dad point injustice | 209,278 | 80x80 | 134,499 | differ (constants) | PASS |
| 59 embedded saves (45 `RoNMultiSave`, 14 `RoNSave`) | 7 .. 400k | | 134,499 - 210,204 | 45 differ (type 315), 13 (constants), 1 equal | 59 PASS |

0 verify mismatches, 0 of 15 parts differ, 0 of 681 - 789 audited members differ in every case; about 0.3 - 0.5 s per save (the whole corpus in 22 s).

## 6. Replay stepping

`ron-sav <file.rcx> --replay [--frames N] [--verbose] [--no-commands] [--json F] [--csv F] [--compare-next B.rcx] [--revert Member,..]`:

1. loads the embedded save (refuses a replay without one: the 36 random-map starts "need the map generator path");
2. reads the command stream through `node re/tools/rcxdump.mjs --json` (or `--json F`) and the CHECK_SUMS rows from `re/oracle/out/checksums.csv`;
3. builds the art (`AssetAnimSource`: animation lengths and release frames come from the install; without it every guy animation is 3 frames);
4. loop: at each turn boundary B (the state **before** the packages of record frame B; the row at record frame B+6 describes it, the first row
   describes save frame + 1) it computes the 15 parts with `EngineProvider` and compares; then executes the records with `frame == F` through
   `CommandPackage::process` (GROUP, MOVE_TO, ATTACK, BUILD, GATHER, SWARM_AROUND, QUEUE_UP, GATHER_POINT, UNQUEUE, LEADER_OPTIONS, HALT, STANCE,
   FORM, DISBAND, BUILDMASK, TRANSPORT, SET_TRANSPORT, BOARD_SHIP, TRADE, CITY_GATHER, GARRISON, SPELL, EJECTALL, BUY, SELL, tribute verbs;
   TURN_DATA, CAMERA, CHECK_SUMS, PLAYER_SPEED, PAUSE, HOTKEY are not simulation); then `Game::do_frame()`;
5. prints per part the first boundary that differs, or "matches through frame N", and the verbs the engine cannot execute (UNITMASK, GUARD,
   FLIGHT, PATROL, FOLLOW: no `Verb` in `engine/sim/commands.hpp`).

All 45 save-based multiplayer replays run to their last CHECK_SUMS without a crash (about 2 minutes together). Results for three of them
(engine frames -> compared turn boundaries; `=` = equal at the first boundary; numbers = first differing boundary frame; "ok" = equal through the end):

| part | `21'23'34` (frames 7 - 18,338, 3,056 turns, 816 commands run) | `21'45'58` (18,344 - 21,111, 462 turns) | `21'57'16` (25,552 - 25,637, 15 turns) |
|---|---|---|---|
| units | 8 (first) | 18,345 (first) | 25,553 (first) |
| builds | 8 (first) | 18,387 | 25,571 |
| walls | ok | ok | ok |
| ammo | 6,896 | ok | 25,559 |
| deaths | 7,094 | 20,415 | 25,559 |
| groups | 422 | 19,047 | 25,595 |
| guys | 14 (matched at 8) | 18,345 (first) | 25,553 (first) |
| leaders | 200 | 18,369 | 25,553 (first) |
| cities | 206 | 18,381 | 25,577 |
| items | 6,752 | ok | ok |
| goods | 8 (first) | ok | ok |
| world | 14 (matched at 8) | 18,351 | 25,553 (first) |
| rules, scenario_data, script_run_time | ok | ok | ok |

(`ron-sav` prints the same as "matches through frame N" / "first differs at boundary frame B (matched through M)".) Over the 45 replays,
`walls`, `rules` and `script_run_time` stay equal in all 45, `scenario_data` in 35, `items` in 38, `goods` in 36; `units`, `builds`, `guys`, `leaders`
and `world` differ from the start or soon after in all.

`--compare-next B.rcx` runs to the frame of the *next* replay's embedded save (the same game, saved when a player rejoined: `21'57'16` ->
`21'57'36`, 91 frames), loads that save as the truth and lists the mapped members that differ, by count of objects. `--step-diff N` lists what
the engine changes in N frames from a save. `--revert Unit.flags,..` puts members back after the first step (an experiment to find which change
spoils a part). These three turn "the hash differs" into the member that is wrong.

## 7. What blocks exact checksums

Engine state that is **missing** (frozen at the save's values in the shadow; the hash of those parts cannot follow the original):
- `LeaderData` combat statistics (`average_*_rate`, `*_fifteen_seconds`, `damage_current_frame`, `frame_battle`), command counters
  (`move_orders_issued`, `column_orders_issued`), `best_pop/attack/armor/move`, taunt state, `capital_stamp`, `ally_stamp`, CTW stamps,
  `support/nuke/flock_stamp`, `trade_profit`, `units_bribed`, `goody_box_*`, `supported`, `dock_mark`, `reg_docks`, `last_spoke`, `cannon_time`.
  They change on every battle and every move order, so `leaders` cannot stay equal past the first fight.
- `Guy` `turret_inc`, `bank`, `last_bank`, `pitch`, `last_pitch` (floats of ships and aircraft); `launch_frames` and the `launching` list
  (carriers); the `Spline` of a lobbed projectile (`Ammo::ammo_path`: a new `Ammo` has `has_path = 0`).
- `Follow`, `Guard`, `GroupPatrol`, `AttackGround` order bodies, GroupAttack's `temporary/oxxx/whosoever`, AirAttackGround's `accuracy/attack_unit/total_time/sx/sy`.
- `Unit` `launch_frames`; `Region` `resource/site/goodies`; `Good/Item` `z`; `Caravan` `making_road/reset_road` and the road stack.
- Capacities: the original's containers carry an allocation `size` / `increment` (a unit's path stack, the guy array, SimpleArray lists, the object
  lists); the engine has none. `EngineProvider` keeps the recorded size and grows it by the rule it assumes (doubling from 10 for SimpleArray, size =
  length for Array families, `+increment` for a path stack): **unverified**, only a game that outgrows a recorded capacity tests it.
- Scripts: `Game::scripts` is not created for a loaded save, `ScenarioData`, `RunTimeEnv` stay in the shadow (equal in the corpus: the only script,
  `no_nukes`, changes nothing after its first frame).
- The command stream: `UNITMASK`, `GUARD`, `FLIGHT`, `PATROL`, `FOLLOW` have no `Verb`; the first one executed late makes `groups` / `units` diverge
  (replay `21'23'34`: UNITMASK at record frame 416, `groups` differs from boundary 422).

Laid out **differently**:
- **The gaia leader is slot 8 in these games, the engine says 9** (`kGaiaWho`): the animals of a random-map multiplayer game are `objects[8]` with
  `who = 8` (`Objects::walk_data` saves 9 of the 10 lists; `Leaders` 0..8). The loader binds slot 8 (so the round trip is exact), but engine code that tests
  `kGaiaWho` (`guys.cpp`, `objects.cpp` birds, `build.cpp`, `newgame.cpp`) treats those animals as an ordinary leader's units. (Setting the constant
  to 8 in a scratch copy did not change the first-frame result below, so it is not the whole story.)
- `Leader::last_unit_finished` is `int16_t` for 352 values, the original's are `int32`; the round trip passes because the corpus values fit.
- Object lists keep **preallocated empty slots above the high mark** (602 in one save: build slots 2026 - 2199); the engine allocates up to the mark. They
  are emitted from the shadow, so the verify block matches, but an engine object created in such a slot reuses a Rec with zero bytes.
- The path stack of the original is a `Stack` (bottom = `list[0]`), the order list is written tail first (file node i = `orders[i]`, current order first),
  the rally-point list is written tail first (file node i = `gather[n-1-i]`).
- Terrain: the height field is in the save and loaded; `Game::mountains` is rebuilt from the saved stamps and the install's templates (so a modded
  install breaks it); the 36 replays that start from setup need the generator (`mapgen`), which produces no heights yet.

Engine behaviour that differs (visible in the replay tables, not caused by the loader): in the first frame from the save of `21'23'34` the engine
changes `Unit.flags` (0x01 -> 0x09, IDLING), `idle`, `orders_x/y`, `dest_angle` of 234 of the 256 gaia animals (`ron-sav --step-diff 1`), while
`guys` still match the original at that boundary; `goods` (`ever_seen`) and `world` differ from the original in that frame; `ammo`, `deaths`,
`items`, `builds` diverge when battles start. Which of these the original really does not do is not decidable from one aggregated hash; the
next-save comparison (`--compare-next`) lists them against the true state 91 frames later.

## 8. Files touched outside the new ones

`engine/sim/game.hpp`: one member `std::shared_ptr<OriginalShadow> original` at the end of `Game` plus a forward declaration. `engine/sim/save.cpp`:
the `sizeof(Game)` guard (1647392 -> 1647408; the native save ignores the member). `engine/sim/rules.hpp` / `rules_image.cpp`: `read_image_into`
(no behaviour change for existing callers). Nothing else; a 164-scenario A/B of `ron-sim` (200 frames, `--ai-every 8`) with and without the change gives
identical digests.
