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
5. prints per part the first boundary that differs, or "matches through frame N", and the verbs the engine cannot execute (GUARD and FOLLOW:
   no `Verb` in `engine/sim/commands.hpp`; UNITMASK has none either but `apps/sav` applies it itself, `Group::action_unitmask` @6fcb90: a toggle of the
   mask over the current group, the first member decides; PATROL, LAUNCH_PATROL, ATTACK_GROUND, FLIGHT, RECALL, SCRAMBLE map to the aircraft verbs).

Before the loop the driver attaches the **game script** the replay header names (`scenarioScript`, `no_nukes` in the whole 2026.08.30 chain; the
embedded save only says `script_type 1`): `original_attach_game_script` (`engine/sim/original_save.cpp`) creates `Game::scripts` and compiles
`scenario/Scripts/<name>/<name>.bhs`, so `Game::do_frame` runs it again (its `run_once` block repeats; the rules it edits are already in the loaded
rules image: `rules`, `scenario_data` and `script_run_time` stay equal). `RON_NO_SCRIPTS=1` turns it off.

All 45 save-based multiplayer replays run to their last CHECK_SUMS without a crash (about 2 minutes together). Results for three of them
(engine frames -> compared turn boundaries; numbers = first differing boundary frame, "before -> after" is the state at the start of the fidelity
work -> now; "ok" = equal through the end):

| part | `21'23'34` (frames 7 - 18,338, 3,056 turns, 818 commands run) | `21'45'58` (18,344 - 21,111, 462 turns) | `21'57'16` (25,552 - 25,637, 15 turns) |
|---|---|---|---|
| units | 8 -> 8 (first) | 18,345 (first) | 25,553 (first) |
| builds | 8 -> **50** | 18,387 | 25,571 |
| walls | ok | ok | ok |
| ammo | 6,896 | ok | 25,559 |
| deaths | 7,094 | 20,415 | 25,559 |
| groups | 422 (UNITMASK now applied, still 422) | 19,047 | 25,595 |
| guys | 14 (matched at 8) | 18,345 (first) | 25,553 (first) |
| leaders | 200 -> **566** | 18,369 | 25,553 (first) |
| cities | 206 | 18,381 | 25,577 |
| items | 6,752 | ok | ok |
| goods | 8 -> **2,318** | ok | ok |
| world | 14 (matched at 8) | 18,351 | 25,553 (first) |
| rules, scenario_data, script_run_time | ok | ok | ok |

(`ron-sav` prints the same as "matches through frame N" / "first differs at boundary frame B (matched through M)".) The second and third replay did
not move: they start in the middle of a game (700 units, fights) and already differ in the first step by many members at once. Over the 45
replays, `walls`, `rules` and `script_run_time` stay equal in all 45, `scenario_data` in 35, `items` in 38, `goods` in 38 (was 36); `units`
and `world` differ at the first boundary in all (guys in 42, leaders in 19).

**Draw distance.** `--compare-next` also prints how many `game_random` draws the engine made against the original over the segment
(`game_random: engine 0x.. original 0x..: the engine made -644 draws against the original`): the two generator states are an LCG apart, so the
step count is found by walking one state to the other (up to 4,000,000 draws). It is a one-number check of the whole RNG consumption of a
segment that needs no object-level truth. Over the 44 consecutive pairs of the 2026.08.30 chain (the 45 saves; `21'22'28` has none) it went (engine draws minus original draws):
`21'49'39 -> 21'50'35` (397 frames) -839 -> -644; `21'57'16 -> 21'57'36` (91 frames) -73 -> -30; `22'22'47 -> 22'23'40` (601 frames) -579 -> -50;
`22'52'38 -> 22'56'06` -16,428 -> -14,766; `21'23'34 -> 21'45'58` (18,337 frames) -323,775 -> -218,465; 31 pairs came closer, 8 went further
(mostly the long fights `22'06'21` .. `22'37'15` and `22'57'36 -> 23'11'20`, where the engine already over-draws), 5 are unchanged. The remaining deficit in peaceful segments is about
1 - 2 draws per 10 frames; a segment with the engine within a few draws of the original has the animals, birds, farms, anim picks and
markets in step.

`--compare-next B.rcx` runs to the frame of the *next* replay's embedded save (the same game, saved when a player rejoined: `21'57'16` ->
`21'57'36`, 91 frames), loads that save as the truth and lists the mapped members that differ, by count of objects. `--step-diff N` lists what
the engine changes in N frames from a save. `--revert Unit.flags,..` puts members back after the first step (an experiment to find which change
spoils a part). These three turn "the hash differs" into the member that is wrong.

### 6.1 Finding the wrong bytes from an Adler-32 (the diagnostics of `apps/sav`)

A part's hash is an Adler-32 over a known byte stream (`ChecksumWalker`), so the *difference* between the engine's hash and the original's
(da, db) is a linear function of the bytes that differ: da = sum e_i, db = sum e_i * (N - i) (mod 65521, N = stream length, e_i = original -
engine byte). `ron-sav` turns that into searches over the engine's own stream, labelled with the object and the walk step of every byte
(`trace_part`: units, ammo, deaths, groups, guys, leaders, cities, items, goods, world):

| option | what it tests |
|---|---|
| `--solve-at B` | at boundary B, for each differing part: one changed byte; two bytes of the same member of two objects with the same delta; a whole column off by e; one integer member (1/2/4 bytes) off by up to 600 |
| `--shift-search PART B` | one object of PART as the engine has it at another frame (+-10) |
| `--pshift-search B` | one to three guys with a member set (positions, + z, + angles/speeds, + anim clock) as at another frame |
| `RON_REVERT_SEARCH=PART` (+ `RON_SEARCH_AT`, `RON_REVERT_INCLUDE/EXCLUDE`) | which changed (object, member) put back to the saved value, alone or 2-4 together (additive effects, hash maps) makes the part equal; `RON_DELTA_SEARCH=W`, `RON_MULTI_DELTA=W` (+`RON_MULTI3`), `RON_ALL_DELTA=W` (+`RON_ALL_PREFIX="Unit.*"`), `RON_CLUSTER_SEARCH=R` add integer deltas, several members at once, whole classes, and one guy at another place |
| `RON_PATCH="Guy.des_x:1,22,0:+1024@18345"` | add a delta to one member at a boundary (to see how far a single quirk carries) |
| `RON_DUMP_UNIT=who,o`, `RON_DUMP_CHAIN=who,o`, `RON_DUMP_GOODS`, `RON_SCAN_UNITS=who`, `RON_SCAN_GUYS`, `RON_ANIM_TABLE`, `RON_ANIM_HIST`, `RON_HDR_HIST=<walk fn>` | state dumps used by the searches |
| `RON_RNG_PROFILE=1` | the draws of the run by call site (needs `-DRON_RNG_PROFILE_HOOK`, see `engine/sim/random.hpp`) |

A hit is only believable when it is **consistent across parts** (the same object in `units` and `guys`: independent equations) or when the pool is
small (a few hundred changed members): the unrestricted single-byte search over 90,000 bytes finds a spurious candidate about once per run. Cases it
settled: the `Build.recharging` bytes of the two woodcutters (boundaries 8 / 14: +1 then +2 for both players, constant afterwards), the gaia goods
(`ever_seen`), the leader flag NEW_UNITS (one byte of `leader_flags`), a `LeaderData.explored` +4 (boundary 566), and a single guy
(`1/22`, Crossbowmen, `des_x` +1024 at the first step of `21'45'58`; patching it makes `units` and `guys` equal at 18,345 and exposes the
next difference at 18,351). `units` at boundary 8 of `21'23'34` has no single member, no small delta of any mapped member of any object, no
one- to three-object time shift that explains it (see section 7).

## 7. What blocks exact checksums

State of the fidelity work (2026-10-24; the log of each fix is in docs/sim-implementation.md "Fidelity log"). **Fixed** since the first table:
the gaia animals (`Animal::process` / `work` / `do_idle`, the `Herd` records and `Herds::process`, the herd goods' `ever_seen`, bird flight,
pen animals), the `LEADER_NEW_UNITS` flag, the walking gatherers' `Build.recharging`, the idle flag of moving units, the complete
`Unit::update_speed`, the herd branch of `AnimalData::is_seen`, the loaded game's leader slot 9, the game script of the replay header.

What still **separates the engine from the original**, in the order the replays meet it:
- **Movement in the first frames.** `21'23'34` matches `guys` and `world` at boundary 8 (the first step from the save: ten citizens walk, the first
  collision is resolved identically) and differs at 14; `units` differs at 8 with no single member, no small delta of any member of any object and no
  one- to three-object time shift that explains it. The guys of the 25 player units and the 256 animals are bit-identical after one frame, so the
  difference is in Unit-level state of the moving units (path stack, collision bookkeeping, order bodies) or in the second frame of a walker (suspect, not
  shown: the engine keeps a walker for a frame at a path node, `x` equal in two states, e.g. unit 1/2 at boundaries 10 and 14). The later
  first mismatches of that replay look like consequences of such drift, not separate bugs: builds from 50 (the `Object.up` link of the player-1
  woodcutter camp is unit 3 in the engine, 2 in the original: the order in which the workers entered the camp's cell), cities 206 (the granary's
  flag is set a frame earlier), leaders 566 (`LeaderData.explored` +4 for player 1: scouts), goods 2,318 / items 6,752 (a rare resource / ruin that
  player 1 has discovered in the original at that boundary and not yet in the engine).
- **The first step of a mid-game save** (`21'45'58`: one guy, `des_x` +1024 on a Crossbowmen that starts an idle animation, a quirk not found in
  the code; `21'57'16`: no single cause, a fight is running).
- **Random draws.** The engine under-draws by 1 - 2 per 10 frames in quiet segments (`--compare-next`, section 6) and over-draws in the long
  fights; the sources not yet ported are listed by the call-site profile (`RON_RNG_PROFILE`) against `re/decomp/fidelity/rng_callers.txt` (every
  call of `Random::get` in the binary): `Unit::think_scout`, `Objects::add_flock`, the rest of `Animal`/`Unit::do_air_physics` for birds, `Army::find_target`.
- **Tech effects on stats.** Over a long segment `Unit.mylos`, `Unit.myhits`, `Build.myhits` / `max_age` differ for most units (the original has
  more hit points and sight: a tech the engine had not researched, or applies differently); not checked at a first mismatch yet.
- The command stream: `GUARD` and `FOLLOW` have no `Verb`.
- Engine state that is **missing** (frozen at the save's values in the shadow; no first mismatch has pointed at them yet):
  `LeaderData` combat statistics (`average_*_rate`, `*_fifteen_seconds`, `damage_current_frame`, `frame_battle`), command counters
  (`move_orders_issued`, `column_orders_issued`; the first MOVE_TO of `21'23'34` is at record frame 3,320), `best_pop/attack/armor/move`, taunt state,
  `capital_stamp`, `ally_stamp`, CTW stamps, `support/nuke/flock_stamp`, `trade_profit`, `goody_box_*`, `supported`, `dock_mark`, `reg_docks`,
  `last_spoke`, `cannon_time`; `Guy` `turret_inc`, `bank`, `last_bank`, `pitch`, `last_pitch` (ships and aircraft; nothing in `Guy::set_new_location`
  computes them for land units); `launch_frames` and the `launching` list (carriers); the `Spline` of a lobbed projectile (`Ammo::ammo_path`);
  `Follow`, `Guard`, `GroupPatrol`, `AttackGround` order bodies, GroupAttack's `temporary/oxxx/whosoever`, AirAttackGround's
  `accuracy/attack_unit/total_time/sx/sy`; `Region` `resource/site/goodies`; `Good/Item` `z`; `Caravan` `making_road/reset_road` and the road stack.
- **Capacities.** The original's containers carry an allocation `size` / `increment`. Rules read from the binary (`re/decomp/fidelity/arr*`):
  `X::increase_size(n)` (SimpleArray, ArrayBase, ObjectArray): `n > 0`: `size += n`; `n < 0` (the default increment -1): `size += size`, or `+= 4`
  from an empty array (ArrayBase: `n == 0` re-allocates the same size); `Stack<PathData>::push` grows when `size <= length` by its `increment` (10 for a path
  stack, whose `size` starts at 10), or doubles (from 1) with increment -1; `SimpleArray::make_valid(i)` grows to cover `i + 1` by at least one
  increment (or `size` for increment -1). `EngineProvider` still doubles from 10 for SimpleArray and keeps `size = length` for the Array families:
  right for every recorded capacity (the 69 round trips), unverified for a growth after the load; no first mismatch involves one (path stacks
  stay at size 10 in these games, the guy arrays at their `squad_size + crew_size`).
- Scripts: the game script is attached by the replay driver (section 6); `ScenarioData`, `RunTimeEnv` stay in the shadow (equal in the corpus).

Laid out **differently**:
- **Gaia is slot 8 for the herds, 9 for everything else.** `Herd::create_units` creates the herd animals as `objects[8]` with `who = 8` (what the
  saves hold: `Objects::walk_data` saves 9 of the 10 lists, `Leaders` 0..8); the bird top-up of `Objects::process_all` and `Farms::add_animals`
  use list 9, which is **not saved**: after a save (so at the start of every segment of the chain) the birds and the pen animals are gone and the
  next `frame % 32 == 0` makes ten new birds. `kGaiaWho` stays 9 (birds, pen animals, `Guy::set_anim`'s bird branch: the guy's `who == 9`); the
  predicate `is_animal_who(who >= 8)` (`sim/animals.hpp`) selects `Animal::process` for both lists. The loader makes `leaders[9]` valid. `newgame.cpp`'s
  `create_herds` still puts the herd animals in list 9 without a `Herd` record (they stand still there), see the log.
- `Leader::last_unit_finished` is `int16_t` for 352 values, the original's are `int32`; the round trip passes because the corpus values fit.
- Object lists keep **preallocated empty slots above the high mark** (602 in one save: build slots 2026 - 2199); the engine allocates up to the mark. They
  are emitted from the shadow, so the verify block matches, but an engine object created in such a slot reuses a Rec with zero bytes.
- The path stack of the original is a `Stack` (bottom = `list[0]`), the order list is written tail first (file node i = `orders[i]`, current order first),
  the rally-point list is written tail first (file node i = `gather[n-1-i]`).
- Terrain: the height field is in the save and loaded; `Game::mountains` is rebuilt from the saved stamps and the install's templates (so a modded
  install breaks it); the 36 replays that start from setup need the generator (`mapgen`), which produces no heights yet.
- `Game::herds` (the `PtrArray<Herd>`, 0x24-byte records) is mapped (`omap::map_herds_array`) and saved natively (section GOOD of the native save).

## 8. Files touched outside the new ones

`engine/sim/game.hpp`: one member `std::shared_ptr<OriginalShadow> original` at the end of `Game` plus a forward declaration (and, since the fidelity work,
`Game::Herd` / `Game::herds`). `engine/sim/save.cpp`: the `sizeof(Game)` guard (1647392 -> 1647408 -> 1647440 with the herds; the native save writes the herds in
section GOOD, `save_walk.inc` has the `Game::Herd` walk). `engine/sim/rules.hpp` / `rules_image.cpp`: `read_image_into`
(no behaviour change for existing callers). Nothing else; a 164-scenario A/B of `ron-sim` (200 frames, `--ai-every 8`) with and without the change gives
identical digests.
