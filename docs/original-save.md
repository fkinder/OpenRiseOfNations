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
5. prints per part the first boundary that differs, or "matches through frame N", and the verbs the engine cannot execute (since the second pass none of the corpus: GUARD and FOLLOW are `Verb::Guard` / `Verb::Follow`, see the Fidelity log;
   UNITMASK has no `Verb` but `apps/sav` applies it itself, `Group::action_unitmask` @6fcb90: a toggle of the
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
| builds | 8 -> 50 -> **200** | 18,387 | 25,571 |
| walls | ok | ok | ok |
| ammo | 6,896 | ok | 25,559 |
| deaths | 7,094 | 20,415 | 25,559 |
| groups | 422 (UNITMASK now applied, still 422) | 19,047 | 25,595 |
| guys | 14 -> **200** | 18,345 (first) | 25,553 (first) |
| leaders | 200 -> **566** | 18,369 | 25,553 (first) |
| cities | 206 | 18,381 | 25,577 |
| items | 6,752 | ok | ok |
| goods | 8 -> **2,318** | ok | ok |
| world | 14 -> **308** | 18,351 | 25,553 (first) |
| rules, scenario_data, script_run_time | ok | ok | ok |

**Second pass (2026-10-24): re-measured, the frontier did not move** - `21'23'34`: units 8, builds 50, ammo 6,896, deaths 7,094, groups 422, guys 14, leaders 566, cities 206, items 6,752, goods 2,318, world 14; `21'45'58`: units / guys 18,345, world 18,351, builds 18,387, cities 18,381, leaders 18,405 (the table above says 18,369; the same value before and after this round, moved by another owner's change), groups 19,047, deaths 20,415; `21'57'16`: units / guys / leaders / world 25,553, ammo / deaths 25,559, builds 25,571, cities 25,577, groups 25,595. The round's work is in the diagnostics (section 6.1), the negative results (section 7), GUARD / FOLLOW and the `--astar-check` oracle.

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

Second pass (round of 2026-10-24, GUARD / FOLLOW executed; the "before" column is the tree at the start of that round, which is not the table above: other
owners' engine changes moved e.g. `21'49'39 -> 21'50'35` from -644 to -560 and `22'22'47 -> 22'23'40` from -50 to -618). Engine draws minus original draws over the
44 pairs (6 are more than 4,000,000 draws apart and not listed: `22'06'21 -> 22'09'27`, `22'09'27 -> 22'13'07`, `22'28'28 -> 22'37'15`, `22'56'06 -> 22'57'36`, `22'57'36 -> 23'11'20`, `23'12'35 -> 23'17'25`); sum of |d| over the 38 others
541,732 -> 530,860. Changed by this round (all because a GUARD / FOLLOW that used to be dropped now runs; 10 closer, 5 further, 23 unchanged):
`21'50'35 -> 21'52'11` -5,370 -> -6,249; `21'58'34 -> 22'00'17` -1,251 -> -1,239; `22'03'19 -> 22'04'05` -818 -> -824; `22'04'05 -> 22'06'21` -31,318 -> -34,230;
`22'13'07 -> 22'14'36` -10,988 -> -9,658; `22'14'36 -> 22'15'12` -4,717 -> -4,495; `22'15'12 -> 22'16'35` -2,604 -> -2,242; `22'16'35 -> 22'16'53` -2,764 -> -2,175;
`22'16'53 -> 22'17'47` -11,489 -> -4,927; `22'17'47 -> 22'18'06` -3,538 -> -2,321; `22'18'06 -> 22'21'11` -54,257 -> -34,598; `22'37'15 -> 22'44'30` -41,601 -> -57,297;
`22'44'30 -> 22'45'30` -194 -> -882 (one unit, The CEO: it now walks to its guard slot and stops thinking, which removes about one draw per frame the engine had been
making and the original, with a guard order, would not have made either: the deficit it uncovers is not the guard's); `23'11'20 -> 23'12'05` -4,464 -> -3,608; `23'12'05 -> 23'12'35` -3,102 -> -2,858.
The big pairs (`21'23'34 -> 21'45'58` -227,833, `21'45'58 -> 21'49'39` -45,201) did not move. The engine draws 230,745 numbers in the 18,337 frames of the first
segment (by site: 64,802 animation picks of `Guy::inc_time`, 60,831 `Animal::think_bird`, 31,320 `Farms::inc_time`, 26,666 idle-animation picks, 16,519 + 5,509 air physics)
and the original 458,578: half the original's draws are missing from the very first segment (12 per frame), in a game with no fights.

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
| `RON_DUMP_MOVERS=1` (with `--step-diff N`) | every unit whose position, path, orders or collision fields changed: before -> after, with the full path stack |
| `RON_TAIL=N`, `RON_OBJ_LAYOUT=who,o` (with `--solve-at`) | the labelled hashed ranges in the last N bytes of a differing part's stream; the hashed ranges of one object (step kind, offset / size constants, bytes). The `units` hash is, per unit, `Object` 0x20..0x42, `SubObject` 9..24, `Unit` 0x48..0xb6, the path stack, the order list and the guys (`GuyData` 8..163): the Adler delta (a, b) of a part is `sum e_i` and `sum e_i (N - i)` over its stream |
| `RON_TRY_PATH=who,o`, `RON_PAIR_SEARCH=who/o,.. (+RON_PAIR_W, RON_PAIR_PART)`, `RON_BIT_SEARCH=R`, `RON_GUY_TRY=B (+RON_GUY_D)`, `RON_RNG_TRY=B` | experiments against the expected hash (all at `RON_SEARCH_AT`, default the first boundary): every subset of one unit's path stack; every small-integer member of the listed units (their `Unit.*`, order bodies and path entries) off by +-W, one or two together; single / three collision-bit flips around the movers; one guy's angle / position / z / speed / clock groups moved by +-D; the generator k draws ahead / behind (no part includes `game_random`). Expect spurious pair hits: 150,000 effects give about 3 chance pairs per run, and members sharing a stream position give many more |
| `RON_COLL_MAP=x,y@B`, `RON_FOG_DIFF=B`, `RON_LIST_UNIT_DIFF=member` | the engine's collision bits against the union of the guys' footprints around a ucoord; the fog bytes changed since the save; with `--compare-next`, every unit whose `los_x` / `idle` / `tolerance` / `group` / `uid` / `collide` / `unit_masks` / ... differs |
| `RON_NO_GUARD=1` | the replay driver drops GUARD / FOLLOW again (the state before they were executed) |
| `--astar-check [--verbose]` (+ `RON_ASTAR_REWIND=1`, `RON_ASTAR_DETAIL=1/2/3`, `RON_ASTAR_ANY=1`) | **the one oracle for the path finder**: for every unit of the save that repathed in the frame just before it (`collide_frame == frame - 1`, standing on a cell centre, a coarse goal entry under >= 2 flag-2 entries) re-run `find_upath` from the loaded state and compare the stack with the original's, entry by entry. The save holds the counter `repaths` after the frame, so it also tries smaller values (the search limit is `500 / repaths^2`, halved when `fast`); `RON_ASTAR_REWIND` first puts back the units processed after the searcher in that frame (guy 0's `last_x/last_y`). Over the 105 saves: 10 distinct candidates (13 counting the SVX copies of the same games), 8 reproduced exactly (paths of 4 to 42 entries, fast and slow, radius 1 - 7), 2 not: an Armored Cavalry whose route the engine sees blocked by a unit standing 4 cells away that was probably created later in that frame (a building trains after the unit loop), and a Light Tank whose original path has a flag-4 (transport) node the engine's search does not produce (section 7) |

A hit is only believable when it is **consistent across parts** (the same object in `units` and `guys`: independent equations) or when the pool is
small (a few hundred changed members): the unrestricted single-byte search over 90,000 bytes finds a spurious candidate about once per run. Cases it
settled: the `Build.recharging` bytes of the two woodcutters (boundaries 8 / 14: +1 then +2 for both players, constant afterwards), the gaia goods
(`ever_seen`), the leader flag NEW_UNITS (one byte of `leader_flags`), a `LeaderData.explored` +4 (boundary 566), and a single guy
(`1/22`, Crossbowmen, `des_x` +1024 at the first step of `21'45'58`; patching it makes `units` and `guys` equal at 18,345 and exposes the
next difference at 18,351). `units` at boundary 8 of `21'23'34` has no single member, no small delta of any mapped member of any object, no
one- to three-object time shift that explains it (see section 7).

### 6.2 What loading does besides restoring bytes (third and fourth fidelity pass, 2026-10-24)

`WalkDataGame::walk_data` @5a2360 restores `game_random` (@5a2ddb) in the middle of the walk and then, in its `input` tail (after `Terrain::init(.., 1, 0)`, which draws only from
`internal_random`), runs two things that touch the generator: `Leader::init(&leaders[9], 9, tribe -1, 9)` @5a30eb and `Farms::add_animals(i)` @8d8f30 for every farm record whose
byte +0xbc is set. The static closure (depth 10) of the walk reaches `game_random` through nothing else (`Objects::reload_data` only updates graphic pieces, `Types::finalize_grafting`,
`GraphicPieces::*`, `WorldMap::*`, `TurnControl::init`, `Doober::walk_data` have no draw); `LoadGame::do_load` / `load_game` / `Game::run` add none before the first `Game::do_frame`.

| step | address | draws |
|---|---|---|
| `Leader::init(leaders[9], 9, -1, 9)` | @6e3930, test `param_2 < 0` | **0**. The negative tribe is the *reset* branch: it clears the arrays, diplomacy, tech masks, personality and returns before `gain_tech` / `random_personality` / the economic coin. It leaves `flags = 0x2000000`; the walk then ORs 7 and sets tribe 0. (The third pass ran the whole AI init here: 21 draws, wrong.) |
| `Farms::add_animals(i)` per pen (`+0xbd == 1`, building flags & 4) | @8d8f30 | 5 animals x (coin `r & 1`: even -> chicken, odd -> pig; two position draws `% 0x180`; `Objects::init_unit(9, ..)` -> `Unit::init` -> `Guy::init_real` @5db6b0: one variant draw `% 100`) = **20 per pen** |
| first `Game::do_frame` | list 9 (processed when `(frame + k) % 10 == 9`) | 5 per pen: `Animal::do_idle` -> `Guy::set_anim(DEFAULT, 0, 1)` with `cur_time >= end_time == 0` picks the idle variant |

So every segment of a chain starts with the generator `LCG^(20 P)(saved seed)` (P = number of finished pens: 2 pens -> 40, 6 -> 120, 7 -> 140). The coin of all five animals of a pen is the
same: the low bit of the generator alternates, and the five coins are four draws apart. Evidence that this is exact: the **fish clock** (6.3) places a 71-fish burst in the very first frame of
`21'57'16` (7 pens) at generator position 40 after the load effects, the engine's own burst is at 40 too; with the old 21 extra draws the original's burst sits 22 draws away.
`21'23'34`: `builds` 50 -> 200 (matched through 194), `guys` 14 -> 20, `world` 14 -> 26 with this fix alone; `RON_NO_LOAD_EFFECTS=1` skips the effects, `RON_LOAD_STAGES=1` prints them.

### 6.3 The fish clock: the original's draw count from the saves (fourth pass)

The 213 fish, 17 whales and 4 sheep of list 8 re-pick their idle variant in lock step: `cur_time` of a gaia animal is the frame number, and every animal of one end time picks in the same
frame, in object order, in `Objects::inc_time` (nothing else draws between two animals of one list). A pick is `r = game_random % 100`, class r < 70 -> 0, < 83 -> 1, < 96 -> 2, else 3. The
variants stored in a save are therefore the classes of **consecutive generator outputs**: a burst of 71 fish (the stock map has three: end times 101 / 116 / 170) is a 71-symbol pattern
that occurs once per 65,536 positions (the low 16 bits of the LCG have period 65,536) and, with the save's generator state, once in reach. List 9 (birds, chickens) is not saved, list 8 is.

`ron-sav A.rcx --replay --compare-next B.rcx` with `RON_FISH_TIMELINE=1 [RON_FISH_J=n]` runs A to the frame of B's embedded save, finds in B the bursts of the last ~400 frames before the save
(`frame = save_frame - 1 - cur_time`), locates each in the generator stream that starts at the engine's seed right after the load effects, and prints for every burst: the original's
draws before it counted from the load, the engine's own first pick of that frame (a hook in `Guy::set_anim`, `g_idle_pick_hook`), and the original's draw rate since the previous burst against
the engine's. It is the first object-level-free *time* axis for the generator; the chain's end-of-segment totals (`--compare-next`) were the only other number. `RON_INSERT="F:k,.."` draws k
extra numbers (k < 0: puts the generator back) before frame F, a probe for "which frame is short"; `RON_ANIMCHK=1` compares the saved `end_time` of every guy with the engine's animation lengths
(488 of 489 agree at 18,345).

What it showed: (a) at the first bursts of quiet segments the engine's draws since the load are within a few hundred of the original's once the road searches (6.4) are in (it was 1,000 - 3,000 short); (b) near the
end of segment 1 the original draws 22 - 28 per frame against the engine's 14 - 15 (fights absent); (c) later segments agree to a few draws per frame, positive or negative.

### 6.4 Road searches: the largest missing draw source (fourth pass)

`BuildType::place_roads` @63c580 ends, for a building that is not a city, not a 'g' (gather) type and belongs to a city (Build +0x72 >= 0), with `REGEN_TOTAL` (mark the building MAKE_ROAD,
Build +0x60 |= 0x100) or `REGEN_FORCE`: **`PathFinder::find_road` from the building to the city centre and `World::set_road_at` along the path**. REGEN_FORCE comes from `Wall::start`
(`mask_me(1, REGEN_FORCE)`, every building the moment it is started) and from `Build::process` for MAKE_ROAD buildings every 16 frames (`(frame + o) % 16 == 0`); `City::regen_roads`
(`Build::activate`, `Build::remove_from_city`) marks every building of the city. `astar_caravan_road` @685990 draws **one number per valid neighbour** (`calc_road_cost` @686300:
`rnd(20)` first), up to 3200 expansions: 100 - 1,700 draws per search, 40 searches (17,000 draws) in the 2,773 frames of `21'45'58`. The six searches of the first three frames of
`21'23'34` (frames 7 - 9: the pending MAKE_ROAD flags of the save) cost 250 - 820 draws each and precede every later fish burst; with them `guys` matches through boundary 194 (was 20) and
`world` through 302 (was 26). `engine/sim/roads.{hpp,cpp}` implements the building variant (4-connected, `cara < 0`); `placement.cpp` / `build.cpp` / `cities.cpp` / `construct.cpp` hook it
(`wall_start` runs the road between `mask_me` and the fog marks, because `calc_road_cost` reads `was_seen`). `RON_NO_ROAD_SEARCH=1` switches it off for A/B digests.
Checked against the binary: neighbours' heuristic is `vector_dist(|N - G|) * 60 / 384` (asm @686017: `SAR 6`; the start node uses `/ 192`), the goal test is the exact position, the 3200 cut-off
is tested after the goal test, a closed node discards, an open duplicate with `length <=` discards, the emission skips the goal and the start and every tile with `TData & 0x4000`.
Not ported: `Caravan::build_road` (8-connected, resumable slices stored in the caravan, `find_road_restore`), `verify_road`, `Caravans::new_danger`; a one-shot probe of the caravan search adds 4,200
draws to segment 1 and none to segment 2, so they are not the rest of the deficit.

**Draw distance engine - original** over the 44 pairs of the 2026.08.30 chain (pair named by its first save; start of the fourth pass = old load, no roads, -> after the load fix and the road
search): 30 pairs closer, 14 further; the sum of |d| over all 44 is dominated by the fights (1.12M -> 1.20M). Quiet pairs: `21'23'34` -224,525 -> -194,661; `21'45'58` -42,142 -> -26,425;
`21'49'39` -600 -> +216; `21'50'35` -4,535 -> -1,284; `21'53'58` -644 -> +251; `22'13'07` -9,617 -> +781; `22'14'36` -3,606 -> +34; `22'16'35` -1,893 -> -50; `22'17'47` -1,768 -> +9;
`22'21'11` -7,066 -> +1,757; `22'22'47` -348 -> +135; `22'28'15` -307 -> +51; `22'48'11` -15,637 -> -1,315; `22'52'08` -5,936 -> +2,356. Further: `22'52'38` -4,770 -> +12,899,
`22'45'30` +2,762 -> +8,713, `22'01'46` -703 -> -1,423, `22'03'19` -85 -> -323, `21'57'16` +177 -> +292. At the first burst after a load the gap
(original - engine draws before the burst) went: `22'14'36` +270 frames 3,246 -> -110; `22'17'47` +61 frames 595 -> 0 and +118 frames 1,661 -> -29; `21'49'39` +302 frames 1,003 -> 63.

**What remains of the early deficit.** (1) After the road searches `21'23'34` is still -194,661 draws (10.6 per frame): the engine's economy is behind the original's (24 / 23 active buildings
at 18,338 against 49 / 47, no Large Cities, no wonders): fewer activations, fewer MAKE_ROAD events, and every one of them costs a search. `21'45'58` started from the true state ends with equal
building counts and is still -26,425; its 40 engine searches look like the original's events (the MAKE_ROAD flags pending in the save at 21,118 are the ones the engine then processes), so the rest is probably not roads. (2) Timing: a search whose
trigger (a wonder / city activation) is a few frames earlier or later in the engine moves 300 - 600 draws across a window. (3) `Unit::think_scout` for auto-exploring scouts in a fully seen
region falls through to `think_civilian_transport(0)` / `think_scout(1)` (random cell of the own region); the engine returns 0 there (TODO in ai_unit.cpp), a few draws per 16 frames per scout.
(4) `units` at boundary 8 of `21'23'34` still differs (no RNG involved any more: guys, world, builds match to 194): a plain member, see section 7.

## 7. What blocks exact checksums

State of the fidelity work (2026-10-24; the log of each fix is in docs/sim-implementation.md "Fidelity log"). **Fixed** since the first table:
the gaia animals (`Animal::process` / `work` / `do_idle`, the `Herd` records and `Herds::process`, the herd goods' `ever_seen`, bird flight,
pen animals), the `LEADER_NEW_UNITS` flag, the walking gatherers' `Build.recharging`, the idle flag of moving units, the complete
`Unit::update_speed`, the herd branch of `AnimalData::is_seen`, the loaded game's leader slot 9, the game script of the replay header.

What still **separates the engine from the original**, in the order the replays meet it:
- **Movement in the first frames.** `21'23'34` matches `guys` and `world` at boundary 8 (the first step from the save: ten citizens walk, the first
  collision is resolved identically) and differs at 14; `units` differs at 8. The second pass (2026-10-24) narrowed it and ruled out a lot, none of it found the cause:
  - *Where.* The `units` hash is per unit `Object` 0x20..0x42, `SubObject` 9..24, `Unit` 0x48..0xb6, path stack, order list, guys; its delta at 8 is (a = 25, b = 46,052),
    `guys` at 14 (a = -1,055, b = 5,723), `world` at 14 (a = -48, b = 15,458). `guys` / `world` match at 8, so positions, angles, animations and collision bits are right; the
    mismatch is Unit / Order / path state of the ten walkers (or of the 256 animals) after one frame. Every one of the 45 replays differs in `units` and `guys` at its first boundary, so the
    cause is systematic (and `guys` in 39 of the 42 mid-game ones, with small Adler deltas: |a| < 200 in 20 of those 39, i.e. a few small byte errors, not a wholesale drift).
  - *What happens in frames 7 -> 8* (`RON_DUMP_MOVERS`): units 1/2, 1/4 and 1/5 (a clump of three) collide: 1/2 finds both sidestep cells blocked, snaps to its cell and repaths (path 2 -> 7 entries,
    the A* expands 146 nodes, limit 500), draws `pause`; 1/5 waits (`unit_masks` 0x40); 1/4 reaches its sidestep point. `guys` at 8 confirms 1/2's snap (forcing a sidestep for it
    changes `guys` at 8) and the three collisions of frame 7 (turning each collision off changes `guys` at 8).
  - *Ruled out* (each by the full `units` / `guys` / `world` hash, which has no false positives): any single member of any object, or one or two members of the ten walkers' objects
    off by up to +-70 (`RON_PAIR_SEARCH`); every subset / flag variant of 1/2's path stack; the `pause` of 1/2 (all 9 values, alone and with each sidestep choice of 1/5); a sidestep chosen
    differently for 1/2, 1/5 (frame 7) or 1/4, 0/5, 0/1 (later); every single collision test at frames 7 - 13 skipped for each walker (`guys` at 14); an angle / position / z / speed / clock group
    of one walker off by +-300 (`guys` at 14); the generator off by +-300 draws; single and triple collision-bit flips; all fog bytes (the engine changes none in these frames); `Unit.visible`,
    `los_x/y` (written only by `Unit::init`), `trench_angle`, `dest_angle`/`orders_x/y` (`update_action` matches the asm).
  - *Checked against the decompilation and found equal*: `Unit::process`, `Unit::work` (both halves), `Unit::move_step`, `Unit::resolve_unit_collision`, `Unit::detect_unit_collision`,
    `Unit::set_new_location`, `Unit::set_angle`, `Unit::update_action`, `Unit::do_idle`, `Unit::check_idle`, `Unit::think` (the economy / AI half is ported), `Animal::process / work / do_idle`,
    `Guy::move`, `Guy::process`, `Farms::inc_time`, `vector_dist`, `find_upath`'s straightening, the A* loop (neighbour order, start direction, tie-breaking, cost, closed / open rules) and
    `Objects::process_all`'s order.
  - *The path finder is right where it can be tested*: `--astar-check` reproduces 8 of the 10 distinct fresh U-level paths of the corpus exactly (section 6.1). So the A* result for 1/2 is almost
    certainly what the original computed. The two it does not reproduce: a unit another unit (created later in the frame) blocks, and a Light Tank whose original path crosses water with a flag-4
    node (`needs_transport`; the transport owner's file).
  - *Still open*: what else in the Unit-level state of those ten walkers or of the 256 animals differs. The candidates left are the members no search can reach because they are not in the
    engine's change set and have no small delta (a different animal state, an order body field the original rewrites that the engine leaves, the RNG-dependent `pause` with a different
    draw order) and a second, independent difference; a ground truth of the original's frame 8 (a second save a few frames later in a quiet game) is what it needs. The segment pairs of the chain are all
    noisy (74 - 443 members differ), the shortest, `22'28'15 -> 22'28'28` (31 frames) and `22'47'57 -> 22'48'11` (31 frames), are fights (Ammo, 27 draws per frame missing).
  The later first mismatches of that replay look like consequences of such drift, not separate bugs: builds from 50 (the `Object.up` link of the player-1 woodcutter camp is unit 3 in the
  engine, 2 in the original: the order in which the workers entered the camp's cell), cities 206 (the granary's flag is set a frame earlier), leaders 566 (`LeaderData.explored` +4 for
  player 1: scouts), goods 2,318 / items 6,752 (a rare resource / ruin that player 1 has discovered in the original at that boundary and not yet in the engine).
- **The first step of a mid-game save** (`21'45'58`: one guy, `des_x` +1024 on a Crossbowmen that starts an idle animation, a quirk not found in
  the code; `21'57'16`: no single cause, a fight is running).
- **Random draws.** The engine under-draws by 1 - 2 per 10 frames in quiet segments (`--compare-next`, section 6) and over-draws in the long
  fights; the sources not yet ported are listed by the call-site profile (`RON_RNG_PROFILE`) against `re/decomp/fidelity/rng_callers.txt` (every
  call of `Random::get` in the binary): `Unit::think_scout`, `Objects::add_flock`, the rest of `Animal`/`Unit::do_air_physics` for birds, `Army::find_target`.
- **Tech effects on stats.** Over a long segment `Unit.mylos`, `Unit.myhits`, `Build.myhits` / `max_age` differ for most units (the original has
  more hit points and sight: a tech the engine had not researched, or applies differently); not checked at a first mismatch yet.
- The command stream: `GUARD` and `FOLLOW` are executed since the second pass (`Verb::Guard` / `Verb::Follow`, `group_action_guard` / `group_action_follow`, `do_guard` / `do_follow` in
  `orders_guard.cpp`, aliasing the GuardOrder members onto unused `Order` fields so the save layout is unchanged); nothing in the corpus can check them yet (their effect on the draw distance is in section 6).
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
