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

1. loads the embedded save; a replay without one (the 34 random-map starts of the corpus) is generated from the header's setup instead, section 6.21 (`ron-sav` prints "start: random map generated by new_random_game from the header ...");
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

### 6.5 Fifth pass (2026-10-24): the `units` hash at boundary 8, objects the walker dropped, the Object defaults

First differing boundary, `21'23'34` / `21'45'58` / `21'57'16` (start of the pass -> now; "ok" = equal through the end):

| part | `21'23'34` | `21'45'58` | `21'57'16` |
|---|---|---|---|
| units | 8 -> **308** | 18,345 -> **18,351** | 25,553 -> 25,553 |
| builds | 200 -> **338** | 18,387 -> 18,387 | 25,571 -> **25,601** |
| guys | 200 -> **308** | 18,351 -> 18,351 | 25,553 |
| world | 308 -> 308 | 18,351 | 25,553 |
| cities | 206 -> 206 (a count, see below) | 18,381 | 25,577 |
| groups / leaders | 422 / 566 | 19,047 / 18,405 | 25,595 / 25,553 |
| goods / items / ammo / deaths | 2,318 / 6,752 / 6,896 / 7,094 | ok / ok / ok / 20,415 | ok / ok / 25,559 / 25,559 |

(`21'45'58`: units, guys, world were wrong at its very first boundary - the Crossbowmen `des_x` +1024 quirk of the earlier passes is gone - and now
match through 18,345 with items / goods / ammo equal through the end.) What the pass found, in the order it was found:

1. **`Stack<PathData>` keeps its allocation** (`Unit.path_size` / `path_inc`, `emit_path`, `unit_init`, the path map). `Stack<PathData>::init(10, increment 10)` @46d720
   (called by `Unit::init` @612100 only `if (size < 10)`), `push` @46d820 grows by `increment` whenever `size <= length`, `increase_size` @46e8c0; nothing shrinks it.
   The A* emission of unit 1/2's repath in frame 7 (146 expansions) pushes more than 10 entries before `find_upath`'s straightening cuts the stack to 7, so the
   original's stack is 20 big; the engine emitted `max(recorded size, length)`. This was the whole `units` delta at boundary 8 (a = 20 in the Adler difference).
   Recycled slots keep the grown size, a stack that was never initialised (size 0 in a save) stays 0.
2. **The checksum walker dropped every object the engine created after the load but the first of each list.** `each_object` (`datawalk.cpp`) asks the provider for the
   list once and then walks every element as its own top-level walk; `EngineProvider::begin_walk()` cleared its engine-owned containers at each of them, so
   from the second element on `present()` fell back to the loaded save's presence bytes. Fix: `Provider::hold_containers(bool)` held for the loop. The first citizen
   the engine trains (frame 197, unit 0/11) had been missing from `units`, `guys` and `builds` since: that was `guys` / `builds` at 200 (a = 16,849 = the sum of its bytes).
   It also moved `21'57'16`'s builds from 25,571 to 25,601 and `21'45'58`'s units from 18,345 to 18,351 (in the engine the objects created in the step were invisible).
3. **Object defaults.** `Object::init` @647750 writes `+0x3e` (`inside_down_who`) = `who` for units too (the engine left -1 / the old slot's value; `unit_remove_from_inside`
   copies it into the container, which is the byte that differed at building 2000 at boundary 200); `add_to_world` @64d8c0 never writes `+0x3f` (`up_who`): a never-used slot holds 0xff.
4. **`Unit::init` / `Build::init` stance from the leader options** (`@612100` / `@629740`, the switch on `get_stance_type`): COMBAT -> `leader_options +0xc`, WORKER -> options `+4`
   (a computer leader: `(starting_resources == 8) + 1`), CASTER -> `~(flags >> 4) & 1`, PACKER -> `~(flags >> 3) & 1`. The engine read `Leader::combat_stance` /
   `worker_stance`, which nothing loads (the replay's `LEADER_OPTIONS` fill `opt_peasants` / `opt_buildings`); the script `set_auto_peasant_level` now writes `opt_peasants`.
5. **Two members that were never mapped** (`original_map_objects.cpp`): `Object +0x41 launch_frames` (airbase / carrier / silo: the engine counted it, the hash read the
   recorded byte: 22'23'40 and 22'52'08 had exactly this single byte at their first boundary) and `Guy +0x44..+0x50 bank / last_bank / pitch / last_pitch` (the aircraft floats).

*Looked at, equal*: the whole of `Unit::init` @612100 member by member (angle, dest_angle, form / form_mod / full, mana_burn, spell_time, attrition, healing, idle, damage_*, waiting,
tolerance, queue_time, orders_x/y, los_x/y, recharging, num_queued, safe, supply, hero, inside_up(_who), group, gather_down, good_obj, rare, doober/cara, collide_*, announce_frame,
o_up/o_down, cavarch_*, play, avoid_*, start_dist), `Object::init` and `Wall::init`; the CEO bit (`is(0x165)`) and the Steam statistics are not simulated. `Unit::clear` @611890 only resets flags,
x/y/z and o_up/o_down.

*Diagnostics added* (`apps/sav`): `RON_DUMP_UNIT_AT=who,o` / `RON_DUMP_BUILD_AT=who,o` (+ `RON_SEARCH_AT`: every member of the engine's unit or building at that boundary),
`RON_OBJ_LIST=1` (with `--solve-at`: which objects the part's stream walked), `RON_CITIZENS=1` (with `--compare-next`: citizens per stance / first order / idle, original then engine),
`RON_CITIZENS_AT=B` (+ `RON_CITIZENS_LIST`), `RON_LIST_BUILD_DIFF=member`, `RON_DIFF_ALL=1` / `RON_DIFF_FILTER=text` (the whole compare-next member list).

**What is left on `21'23'34`.**
- *`units` / `guys` / `world` at 308 are one missing game_random draw.* `RON_INSERT="F:1"` (one extra draw before frame F) makes `guys` match through 326 for every F in 293 - 302 (292 and
  earlier fail): the original draws once more than the engine somewhere in frames 293 - 295 (frame 294 holds the only unattributed site, a market redraw of three numbers; the farms draw 8
  per frame, the birds 9 at 296; none of them is hashed, so the exact site cannot be told from the hashes: only the 71-fish burst of frame 302 shows it). Even with the extra draw the next
  difference is at 332 (a unit, no small delta). Not found in: the BUILD / GATHER / MOVE commands (none in 280 - 330), `Build::process` roads (no MAKE_ROAD pending at 2011), `Unit::think_peasant`.
- *`cities` at 206*: `City.land` and `City.filled` of city (0,0) are both one lower in the original after the frame-200 strategy pass; the same pass is exact for player 1's city at 175 and for
  every other field. Every condition of `Leader::plan_strategy` @6b9620's tile loop was compared with `leader_plan_strategy` (owner `WData +0xf`, flags & 0x100, land, `k + 1 < circle_radius[n]`, region `+4`,
  `flags & 0x70`, `check_building_wcoord < 4`) and they agree; skipping any one filled tile of the loop reproduces the hash, so the cause is a tile the original excludes that the engine counts (the
  `WData`s of the loop are equal in the `world` hash at 200). Persists at 406. Not found.
- *`builds` at 338*: `Object.up` of building 2007 is 7 in the original (a unit that entered the wcoord list first): a consequence of the unit drift above.
- *Early economy.* No late construction or activation was found in the first 560 frames (`leaders` and `builds` match there). Player 0's citizens are at stance 2 (auto-build, no auto-gather) until his
  `LEADER_OPTIONS` of frame 6,806 sets 1; the engine then has 14 idle citizens that `find_gather_spot` cannot place (a citizen of city 1 is not sent to city 0's woodcutter: `uu <= free + 2 + gatherers`
  of city 0), the original has none (`RON_CITIZENS`, at 18,338: engine 34 idle of 50 citizens, original 0 of 55). The `cities` / `leaders` censuses that feed this (`City.free`, `gatherers`) are the engine's,
  so the divergence is upstream of it: the unit drift from 308 on (positions, who stands in which city) is the first suspect, then the idle-citizen counters.

### 6.6 Sixth pass (2026-10-24): `cities` at 206, the transport masks, the scout fall-through, the draw drift narrowed

First differing boundary, before -> after this pass (`ron-sav --replay`, the three replays of the chain; "ok" = equal through the end):

| part | `21'23'34` | `21'45'58` | `21'57'16` |
|---|---|---|---|
| units / guys / world | 308 / 308 / 308 -> same | 18,351 -> same | 25,553 -> same |
| builds | 338 -> same | 18,387 -> same | 25,601 -> same |
| cities | 206 -> **422** | 18,381 -> **18,915** | 25,577 -> **25,601** |
| groups / leaders | 422 / 566 -> same | 19,047 / 18,405 -> same | 25,595 / 25,553 -> same |
| goods / items / ammo / deaths | 2,318 / 6,752 / 6,896 / 7,094 -> same | ok / ok / ok / 20,415 -> same | ok / ok / 25,559 / 25,559 -> same |

Boundaries with all 15 parts equal on `21'23'34`: 33 -> 50. `21'49'39` (first row of a mid-game save): `units` 21,118 -> 21,154.

1. **`cities` at 206 = the centre tile (found).** `Leader::plan_strategy` @6b9620 reads its tile table as `circle_x[k + 1]` / `circle_y[k + 1]` (`MOVSX EDI, byte ptr [ECX + 0xcb7e91]` @6bb6f7, `[ECX + 0xcbb0e1]` @6bb700; every
   other user of the tables indexes from `0xcb7e90` / `0xcbb0e0`): the loop is `for (i = 1; i < circle_radius[n + 1]; i++)` and the land test is `i < circle_radius[n]` (`CMP ECX, [EBP - 0x30]` @6bb86c with
   ECX = k + 1). The centre entry (the city building's own tile, flags 0x4080, `check_building_wcoord` 0) is never visited; the engine visited it (+1 `land`, +1 `filled`) and cut the land ring one
   tile early (`k + 1 < radius[n]` on a 0-based k). City (0,0) of `21'23'34`: 93 land tiles in the engine, 92 in the original (the tile at index 104, the one the engine cut, is not a land tile). Player 1's city at frame
   175 agreed before presumably because the two errors cancelled there. `ai_strategy.cpp`.
2. **`Unit::init` @612100 sets UNIT_CAN_TRANSPORT** (0x800000) for every new unit whose `transport_type <= LeaderData::can_transport()` and that `can_ever_transport` (a scout type, `is(0x45)`, only with the
   options' auto-transport bit), the tribe-bonus-0x12 masks2 bits (0x80 for non-civilian land units, 0x4080 for scouts) and the Marines bit (`is(0x77)`); the engine had a TODO. `Unit::add_gather_order`
   @61a5c0 tests the same predicate when the building is in another region (the engine tested `level >= 1 && land`). `21'49'39` has a citizen created in its first frame whose mask byte was the whole
   `units` delta (a = 128): `units` now matches 36 frames longer. `objects.cpp`, `econ_gather.cpp`.
3. **`Unit::think_scout` falls through** @5f6c4c to `Unit::think_civilian_transport(0)` @5f40d0 when there is nothing left to explore: a land unit whose leader cannot carry it (`transport_type >
   can_transport`) or that is not cargo goes back to the explorer's random walk when it auto-explores, `think_scout(1)` = a random passable cell of its own region (T1, up to 16 re-draws on blocked
   cells; the engine's region mode was already exact against the asm @5f6094..5f61f9). The ferry search of the rest of `think_civilian_transport` is still not ported (returns 0). The corpus scouts
   start idle with masks 0; player 1's UNITMASK at frame 416 (and player 0's at 11,522) turns auto-explore on, so this only matters from there (`leaders` `explored` +4 at 566 is the scout).
4. **The draw drift of `21'23'34`, narrowed but not closed.** Measured with `RON_INSERT` (frame granularity) and the new `RON_INSERT_N=F:n:k` (profile build: k extra draws just before the n-th draw of
   frame F): the original draws **exactly one number more** between the end of frame 292 and the 71-fish burst of frame 302 (k = 1 is the only shift that matches the burst; k = 2, 3, 4, 6, -8 fail at
   302 through the picks of frames 296 - 301, k = -1..-4, 5, 8, 12 pass 302 and fail at the burst, 308). Every (frame, n) position in that window gives the same result (the picks there are blind to a
   shift of 1), so the site cannot be told from the hashes. With it `units` / `guys` / `world` match through 326.
   The next wall (332: the farmers' four target draws of frames 326 - 330) needs a second +1 between the pick of frame 306 and frame ~320, then a -1 at frame 333 / 334 (the chicken pick of 333 or the
   scout pick of 334): net zero, i.e. a transient. With the three corrections (`RON_INSERT="293:1,307:1,333:-1"`) `units` / `guys` match through 398 (wall 404: a one-byte delta of -1 in `units` that no insertion of +-1 in the preceding 40 frames removes),
   `builds` through 452, `groups` / `cities` through 416, `leaders` through 560 (`explored`, the scout), `items` / `goods` / `ammo` / `deaths` / `walls` through the end of the 700-frame run. The transients are
   consistent with pen animals' hidden state (list 9 is in no hash: a chicken that picks another animation picks again 60 frames later) following a draw that is off by one, so the single missing draw is
   the thing to find.
   *Ruled out, each by a run*: the market redraw (`calc_market` is the decompile, 3 draws; +1 per redraw breaks at 104), farm sprouting (the 8 wheat farms roll every frame; no roll in 293 - 302 is within 20 of
   its threshold, so no sprout can differ), the pen animals' wander schedule (`(o * (aid + 1) + frame) % 128`; o / aid / frame off by one breaks at 62 - 104), the chicken and bird animation lengths (+-1 breaks
   at 74 - 236), the bird walk-animation draws (`Guy::set_anim` @5db2b8..5db361 and `Guy::inc_time` read against the asm, including the `cur_time * T / T` of an animation change), `Animal::think_bird`,
   `Unit::do_air_physics` for type 0x192, `Objects::process_all` (flock top-up every 32 frames, none in the window), `Roads::scan_and_kill_stray_roads` / `Wall::kill_at_tile` (no `game_random`; sound / internal),
   `Leaders::strategy_all` (the original calls `compute_score` every frame; no draw), the group idle branch (never taken in the window), `Unit::come_out`, `Unit::repair_damage` (internal_random).
5. **A mature economy draws ~10 numbers per frame more in the original, and it is not the citizens.** From the true state of `21'45'58` the engine's census after 2,773 frames is nearly the original's
   (player 0: 63 against 64 citizens, 5 against 9 idle; player 1: 43 / 43, identical distribution), yet it made 26,425 draws fewer (29,842 after item 2). In segment 1, between the last two fish bursts (frames
   18,280 - 18,327) the original draws 27.7 per frame, the engine 15.9: events of several hundred draws, i.e. searches. The earlier "idle citizens" of the long run (engine 34 idle of 50) are therefore a
   consequence of the early drift (the engine's players own 24 / 23 active buildings and an earlier age: the same QUEUE_UPs fail when the resources arrive later), not a `find_gather_spot` bug:
   `find_gather_spot` @5f5170 and `think_peasant` @5f5760 were read against the decompile and are equal; at 9,002 the idle citizens find every gather building full (1 woodcutter camp, 7 farms).
6. **First-row differences of the mid-game replays** (units / guys / world at the first or second boundary in most of the 45): a part of them is the road art: `Roads::scan_and_kill_stray_roads` @8956a0 runs every frame (`world.size / 500`
   wcoords of 16 tcoords) and clears road bits of tcoords without a matching `RoadElementCandidate` (`TerrainOut` presentation data, `scan_and_kill_bad_tcoord` @88e100, `World::set_road_at`); the engine
   keeps no road elements. Not ported. The `world` hash is sensitive to every WData field (`RON_PERTURB_WDATA`).

*Diagnostics added* (`apps/sav`): `RON_DUMP_FARMS=F0:F1` (per farm: owner, gate, bare cells), `RON_DUMP_GAIA=F0:F1` (+ `RON_DUMP_GAIA_WHO`, `RON_DUMP_GAIA_TURRET`: a gaia list's units with anim / clock),
`RON_PERTURB_WDATA=F:x:y:field:delta` (which parts a WData field feeds), `RON_INSERT_N=F:n:k` (profile build only, with `RON_RNG_PROFILE=1`).

### 6.7 Seventh pass (2026-10-24): the animals' speed, the collision and scout rules, terraforming

First differing boundary of `21'23'34`, before -> after: units / guys / world 308 -> **1,412**, builds 338 -> 1,352, groups 422 -> 1,520, leaders 566 -> 1,628, cities 422 -> 1,604; all 15 parts equal at 224 boundaries (was 50).

1. **The extra draw of 292-302 was a pen chicken.** List 9 (chickens, birds) is in no hash, so its only trace is the generator. `RON_PHASE` (adds a delta to one animal's animation clock) over the ten chickens showed that chicken 6 two frames
   earlier makes `units` / `guys` match through 367. Its walk from (64344,9240) to (63960,8664) took 28 frames; `AnimalData::get_speed` @5d8380 runs an animal at 3/2 speed while more than 0x180 from its move order's target, so 25 frames
   and its arrival pick (a draw) falls before the fish burst of 302. The +1 / +1 / -1 transients were the same chicken's later cycles. `movement.cpp`.
2. **`Unit::think_scout` @5f6180**: AUTO_EXPLORE forces the city scouting (the engine skipped it); `City.scouted` was the `cities` byte at 422 and the scouts' `explored` of 566. `ai_unit.cpp`.
3. **Animals and collisions**: Unit vtable +0x30 = 0, Animal's = 1 (`resolve_unit_collision` clears an animal's orders and returns). The vtable diff of Animal against Unit is 16 slots: log_data, +0x30, +0x44 / +0xb4 (return `this`),
   is_seen, walk_data, init, process, valid_target_const, close, update_los (los = 0), get_speed, think_bird, do_idle, work. `collision.cpp`.
4. **Terraforming and the builder's spot** (`build.cpp`, `search.cpp`): see sim-implementation "seventh pass", items 32-33. The saved height grid is the terraformed one: after the same BUILD the engine's grid equals the next save's float for float
   around the first Barracks; `--compare-next` now reports `terrain heights: N of 160801 vertices differ` (1,146 for the whole first segment: the buildings the engine never built or built elsewhere).
5. **What is left at 1,352 / 1,412** (a `builds` constant on the new Barracks record, and the first building road search) is in the sim-implementation log; the tools that found the rest: `RON_DUMP_PART` + `RON_SKIP_CMD` (hashed
   stream with and without a command, byte by byte), `RON_TILES_WINDOW`, `RON_CMP_BUILD`.

### 6.8 Eighth pass (2026-10-24): the ring road, `WallData::gpiece`, the rally spot

First differing boundary of `21'23'34`, before -> after: units 1,412 -> **1,514**, guys 1,412 -> **1,628**, world 1,412 -> **1,592**, builds 1,352 -> **1,628**, groups 1,520 -> **1,760**, cities 1,604 -> **1,802**, leaders 1,628 (unchanged); all 15 parts equal at 252 boundaries (was 224).
The detail is in sim-implementation "eighth pass", items 35-38; what the hashes taught:

1. **Equations are only as good as the stream length.** `--solve-at B` assumes the engine's stream has the original's length. `world` includes one 96-byte `CollBlock` per wcoord that ever held a unit: its length varies by hundreds of bytes, so the solver's
   "single byte at the end of the stream" for `world` at 1,592 is an artefact (a swap or a different block count). `a = 0` with `b` small means two equal and opposite changes (list heads swapping, a footprint moved by whole rows: 3 rows of 0x38 moved by 2 rows = b 672).
2. **A constant delta over time is a static field.** `builds` kept (a = 346, b = 10,721) for 60 boundaries, then (679, 3,255) when the next building appeared: each new building record was short by a constant. `a` = the byte sum of the missing word
   (0x94 + 0xc6 = 346, 0x87 + 0xc6 = 333), and the walker comment "job_counter ... construct_hits, gpiece, frame_started" named it. Checking the shadow image of every saved building (`RON_RAW_BUILDS=-1`) gave `kFirstBuild + 129 * style + 1161 * age + type - 414`.
   Unmapped bytes of a new object are 0 in the engine (the shadow of an image the engine created is zeroed): look for them whenever a new record differs by a constant; the `(a, b)` of a record is cheap to compare with the byte sums of the walker's raw ranges.
3. **`--compare-next` and `RON_TILES_WINDOW` show the later state.** The "straight road" at the first Barracks was the road of a later search; the ring road of `place_roads` (tiles outside the footprint, laid before the search) explained the whole difference at 1,407:
   with the ring the search lays the same 18 tiles and `world` matches. Test a hypothesis at the boundary with the hash itself (`RON_FRAME_DIFF` to see what the engine changes, then the replay for the first differing boundary).
4. **Placement sweep.** For a unit whose position is in doubt apply a shift of the final spot (`RON_EXP_RELOC` was a throw-away switch: +-96 in 48 steps) and read the three parts: `guys` and `world` jumped from 1,448 to 1,466 / 1,472 at exactly (-48, 0), which led to the stack-slot reading
   of `Unit::come_out` (the latest spot, not the node). The decompile hides the argument registers of `find_angle` / `add_move_facing_order`: the disassembly (`DisasmFunctions.java`) shows `[esp+0x40]` / `[esp+0x44]` as the outputs of the last `find_nearby_spot`.
5. **Still open** (details in the sim-implementation log): `units` at 1,514 (a static `a = -2,146` that stays for 54 boundaries while `guys` match), the collision-block count of `world` at 1,592, a single byte of leader 1's `Diplomacy` at `leaders` 1,628.
   The mature-economy draw gap of the brief is not an economy site: the segment `21'57'16 -> 21'57'36` (+334 draws) is one MAKE_ROAD search whose flag the original sets again after a city changes hands later than the engine does.

*Diagnostics added* (`apps/sav`): `RON_FRAME_DIFF`, `RON_CMD_TRACE`, `RON_RAW_BUILDS`, `RON_POS_SEARCH` / `RON_POS_W`, `RON_TARGET_SEARCH` / `RON_TARGET_W` / `RON_TARGET_STEP`, `RON_PAIR_VALUES` / `RON_PAIR_STEP` / `RON_PAIR_DEBUG`, `RON_TRY_PATH` with allocations;
`RON_PAIR_SEARCH` now runs at the `--solve-at` point (after the boundary's commands).

### 6.9 Ninth pass (2026-10-24): a hash-provider bug, the combat statistics

First differing boundary of `21'23'34`, before -> after: units 1,514 -> **1,622**; the others unchanged; all 15 parts equal at 264 boundaries (was 252). Detail: sim-implementation "ninth pass", items 39-42.

1. **A wrong hash is not always a wrong simulation.** `units` 1,514 (a static delta for 54 boundaries, equal again for 6, different again) was the engine's *hash provider*: a shadow image keeps the segment of every class ever bound to it, so a unit whose order at index 0
   changed from a gather order (load) to a move order carried the gather order's members over the move order's `y` / `angle` / `dest`. Sign of such a bug: a static delta that vanishes exactly when the object's state is replaced. Print the object's hashed bytes (`RON_OBJ_LAYOUT` with
   `RON_LAYOUT_LEN`) and its members (`RON_DUMP_MEMBERS`) and compare field by field before touching the simulation.
2. **A single-byte solution is nearly free** when the stream is shorter than 65,521 bytes (one candidate position per delta): `leaders` 1,628 "Diplomacy offers[4]" was such a reading and was not the same byte one boundary later. Only believe a reading that survives a second boundary or a second part.
3. **Missing state that mature saves need**: `Leader::process_event_frame` (the `*_current_frame` counters, the 15-second sums and the smoothed rates every 50 frames) is ported; the one-frame mismatch of every mature save (`units` / `guys` / `leaders` / `world` at the first row, even though nothing is
   in the first 50) has another cause, not found.

### 6.10 Tenth pass (2026-10-25): road candidates, goto_build, find_builds

- Road candidates are saved by `Terrain::walk_roads` as a top event per cell (value = y*xs+x) whose child `CoordInfo` record holds `num_roads` (byte 0x66) and a 256-byte record of 16 x 16-byte `RoadElementCandidate`; the engine loads them in `Loader::load_roads` into `World::roads`.
- `GatherOrder::clear` sets `goto_build = 1`; `SimpleArray` capacity grows 4, 8, 16 ...
- `Objects::find_builds` @65a120 compares `WData.region` and has a cell mode without distance test. See sim-implementation "tenth pass", items 43 - 47, for the effect on `21'23'34`.

### 6.11 Eleventh pass (2026-10-25): what a playback runs before its first frame, the completion bonus

- **`Game::run` @584590 calls `Leaders::init_production_script`, `GameDaemon::calc_danger` and `GameDaemon::update_all_seen` just before the first frame, for a playback as for a new game.** The recording session writes its embedded save right after those calls (`RecordGame::write_save_game`), so the file's `World::danger` is made from the
  fog of the moment before the refresh and the playback's first frame sees it changed (`is_seen` halves a contribution, `do_danger`). The replay driver now ends `original_post_load_effects` with `calc_danger; update_all_seen`: `world` stops differing at the first row of 40 of the 41 mature replays that had it (first difference +7 .. +127 now; `22'21'11`, a fight, stays at +1), 300 -> 332 fully
  equal boundaries over the 59 replays, nothing earlier anywhere. `ron-net replay` (`apps/net/replay_cmd.cpp`) loads the save itself and does not call `original_post_load_effects`: it needs the same call.
- **The danger delta signature**: a world delta (a, b) that is *static* while the stream length changes means the wrong bytes are before the CollBlocks (`b` moves by `a * dN`, N = the stream length change), i.e. in WData / tdata / the fog arrays / danger; a delta that changes shape with the block count is a unit footprint.
- **`--compare-next` now audits the World buffers** (`seen`, `seen2`, `seen3`, `wcoord_seen`, `danger`, `tdata`, WData members): over a short calm pair they are exact (`21'49'39 -> 21'50'35` has none differing); a few danger cells differ after a fight. `Ammo` is not part of what a save carries over: arrows in flight differ between the engine and the next save by construction.
- **`Build::do_bonus` @627140** is `bucket_add` of 30 (first Dock / Market / Temple), the food / timber / knowledge / metal / oil completion bonuses (docs/sim-implementation.md "eleventh pass" item 49); it moved the `leaders` delta of boundary 1,928 of `21'23'34` (a Market completes in frame 1,923) from (44, 44921) to the static (65520, 34096).
- Open: the static `leaders` (-1) / `world` (679) deltas from boundary 1,802 of `21'23'34` (evidence and dead ends: item 52), `units` / `guys` 1,928 (the two builders of the finished Market), the first-row `leaders` of the fights (the territory recompute transient).

### 6.12 Twelfth pass (2026-10-25): the rush rule's war, the dock records, the playback of ron-net

- **A timed rush rule (RUSHRULES >= 9, `Category.data[0]` minutes) declares war between every pair of non-allied leaders at frame minutes * 900** (`Game::do_frame` @591ef0, before `GameDaemon::process_all`, so the danger map of frame 1,800 already sees it). It was the "static" `leaders` (a = -1) / `world` (a = 679) delta of boundary 1,802 of `21'23'34`: the game's rule is 2 minutes = 1,800 frames. With it `leaders` first differs at
  2,054 (was 1,802) and `world` at 1,928 (the unit divergence). The lesson for the next static delta: look at the periodic events of `Game::do_frame` itself (frame % N or frame == N * 900) before the census.
- **Dock records**: `Build::activate` of a dock creates the record, `Leader` +0x42c `dock_mark`, +0x135e `reg_docks[region]` and a seagull (gaia slot 9, not saved, not hashed) with one `game_random` draw. The `leaders` delta (a = +2, b = 45,119) of boundary 2,054 is exactly `dock_mark` and `reg_docks[4]` going 0 -> 1. `leaders` first differs at 2,438 after it.
- `Leaders::process_all` recomputes leader flag 0x40000 every frame; `PathFinder::calc_cost` has the rush-rule (+500) and team-style (+5000) terms; the caravan call sites in `Unit::do_move` / `Object::take_damage` / `City::close` are wired (sim-implementation "twelfth pass", items 55 - 57).
- **`Region.borders`** is not part of what the original saves (`Region::walk_data` stops at `&borders`) and is 0 after a load, so the territory is recomputed from scratch in the original too: the first-row `leaders` mismatches of the fights are not that. They come with `units` (15 of 19) and `ammo` (9 of 19) differing at the same boundary: the recorded checksums were made by the recording session, whose arrows in
  flight the embedded save does not carry (the playback has none). Those replays cannot match the first frames.
- `ron-net replay` now runs `original_post_load_effects` and attaches the header's game script, and the network layer may execute UNITMASK: its numbers equal `ron-sav --replay`'s (320 fully equal turns on `21'23'34`).
- Open: `units` / `guys` / `world` at 1,928 of `21'23'34` (the two builders after the Market completes; target choice, order construction, `do_build` / `do_construct`, the RNG and a hold of 1 - 6 frames are all excluded, sim-implementation item 60), then `builds` 2,036, `groups` 2,042, `cities` 2,402, `goods` 4,346.

### 6.13 Thirteenth pass (2026-10-25): three missing pieces behind the 1,928 wall of 21'23'34

- Reading a hash delta over a stretch of boundaries (`--solve-at B` at B, B + 6, ...): a constant `guys` delta means static wrong bytes and a right simulation; for the `units` part, `b - a*N` (N = the engine's stream length) is constant while only objects *after* the wrong bytes change length and jumps by `-a * L` when an object before them changes by L (`RON_LEN_CHANGES=part:F0:F1` lists the length changes with their stream offsets): the wrong bytes sit between two named objects.
- A coverage-instrumented `ron-sav` (clang `-fprofile-instr-generate -fcoverage-mapping`, `llvm-cov export`) compared between the run to the last equal boundary and the run past the first divergent one lists the code regions executed only in the window; none for 1,928 / 1,934 / 2,084 means a data-dependent branch of exercised code (a missing special case), not an unported function.
- The three fixes: `Unit::resolve_unit_collision`'s GATHER-on-a-farm early return (units 1,928 -> 2,084), `Stack<PathData>` allocation growth on every push (-> 2,138), `Terrain::object_placed` -> `land_sub` (world 1,928 -> 2,204; the tileset name is the walked `TileSet` String of the save). See sim-implementation "thirteenth pass", items 61 - 65.
- Open: `units` / `guys` 2,138 (unit 1/14's idle animation choice, item 64c), `world` 2,204, `builds` 2,252, `groups` 2,330, `leaders` 2,438, `cities` 2,606.

### 6.14 Fourteenth pass (2026-10-26): the gull draws, the counters of Unit::init, the TCoord land, the scouted bits

- A `game_random` offset behind a visible mismatch is found with `RON_INSERT=F:k` scans; the unhashed leader list 9 (birds, gulls, pen animals) can only be seen through its later visible consumers (fish bursts, idle picks). The 2,138 "idle animation" of 21'23'34 was three gull fire draws.
- `--solve-at B` single-byte hits are often spurious; a delta that stays constant over several boundaries is static state, one that grows is a simulation difference.
- The `Region` +0x3c / +0x40 pair (scout mark, scouted bits) and the Array increment defaults are not in the original's walk: a loaded game starts them clear / at the size the recorded length implies (`ArraySpec::first_size`: 4 for `SimpleArray<int>`, 5 for the mining list of `BuildData`).
- The pass's fixes: sim-implementation "fourteenth pass", items 66 - 76. 21'23'34 now simulates 869 of 3,056 boundaries with all 15 parts equal (355 before); first differing boundaries: units 4,466, builds 5,264, groups / guys / world 5,246, leaders 5,396, cities 5,402, goods 7,082, ammo 6,896, deaths 7,094, items matches to the end.
- Open: the 5,246 squad spawn (a Barracks' three-unit squad, ungroup of the follower), `Docks::remask_docks`, the caravan 1/56, the first fight.

### 6.15 Fifteenth pass (2026-10-26): the squad spawn of 5,246, the spot slots of `Unit::come_out`

- The 5,246 wall (a three-unit squad leaving Barracks 0/2012 next to the squad that already stands on the rally point) was two stack slots of `Unit::come_out`: the relocation of the captain next to the unit on the rally point updates the "previous spot" the group move's facing angle is taken from, and the squad captain's final `find_nearby_spot` runs in place on the latest spot (the rally spot), not on the gather node. Finding it: the `groups` delta was static from 5,246 to 5,258, i.e. a field set once at creation; the group record (`RON_DUMP_PART=5:B:prefix`) showed `o_angle`, `curr_x/y` and the target as the only fields that depend on the spots; `guys` / `world` did not move when the follower was or was not ungrouped, so that branch was not the cause; reading the asm for what `[esp+0x2c]`, `[esp+0x54]`, `[esp+0x40]`, `[esp+0x44]` hold at each call site gave the two rules (sim-implementation "fifteenth pass", items 77 - 78).
- A second finding of the pass came from the chain itself: `--compare-next` of `22'16'35` against the save that starts the next replay (frame 37,200) differs in one mapped member, `Build.mylos` of the cities (the LOS code read a `science` mirror that is 0 after a load instead of `epoch[3]`); with it `22'16'35` matches all 15 parts at every boundary (the first fully matching replay). Aggregating the differing members over the 44 adjacent pairs gives the list of the next leads (sim-implementation item 81 (e)).
- After it `21'23'34` simulates 928 of 3,056 boundaries with all 15 parts equal (869 before); first differing boundaries: units 4,466 (transient), guys 5,600, world 5,606, builds 5,750, leaders 5,912, groups 5,954, cities 6,404, ammo 6,896, deaths 7,088; items / goods / walls / rules / scripts equal to the end.
- A boundary-by-boundary read of a part delta stays the tool: the `groups` delta of a squad created and then left alone is constant (a field of the creation), a `guys` delta that grows is a simulation difference, and a delta that is present only in `units` (not in `guys`) is order / path / mask state. A shell warning from the pass: `env $vars cmd` in zsh does not split `$vars`.
- Open: guys 5,600 (player 1's 15-unit group move at 5,594: two ungroups, two stalls), the packer order nodes of the transient `units` boundaries, then the first fight; the `--compare-next` leads (`Region.flags` AnybodyHere, gaia animals' `cur_anim`, `frame_started`, `Herd` positions).

### 6.16 Sixteenth pass (2026-10-26): combat counters, absolute ammo pieces, the muzzle offset

`Ammo::gpiece` in a save is absolute (first_ammo_piece 0xeaf5 + index into the AMMO list; 0x100 for the aircraft wreck). `Leader` `damage_current_frame` (+0xa5e) counts the damage taken, `hits_current_frame` (+0xa5c) the attacker's hits (5 for siege against a building), a building kill adds to the owner's `deaths_current_frame` (+0xa58). A new shot starts at the guy plus the rotated, scaled muzzle offset of the release node (see sim-implementation.md item 89). After a load the `fire_proj` table has to be rebuilt (`init_fire_proj`). Details: docs/sim-implementation.md, sixteenth pass.

### 6.17 Seventeenth pass (2026-10-26): turrets, pivot restrictions, BH3 rest quantization, DeathObj pivots

The turret units (catapults, cannons, ships, AA guns, tanks) carry `<RESTRICTION node=.. minangle=.. maxangle=..>` pivots in unit_graphics.xml; the pivot of
restriction k is model node 4+k and `GraphicPieces::has_restrictions` @90b640 is the count. A saved Guy holds `turret_angles` (+0x20), `des_turret_angles`
(+0x30), `node_flags` (+0x96), `des_node_flags` (+0x98) and `guy_flags` bit 0x100 (has pivots); a `DeathObj` holds the four angles (+0x30), the skeleton
(+0x44), `node_flags` (short, +0x48, 0xffff after `init`) and the crew byte (+0x4a). The `deaths` checksum covers the DeathObj pivots.

New diagnostics (all env-gated, `apps/sav`, in the style of 6.9 - 6.16):
- `RON_MUZZLE_CHECK=1|2|3`: compares the start point of every in-flight ammo of the save with the engine's muzzle offset of its shooter (1: counts, 2: lists the misses, 3: lists all).
- `RON_TURRETS`: lists the turret units of a save with their pivot state (angles, desired angles, node flags).
- `RON_AA`: lists the type 523 - 525 buildings (air-defence guns) with `recharging`, near/attack objects, masks and flags.
- `RON_REGIONS` (with `--compare-next`): lists the regions whose flags or cursor differ; `RON_WLISTS=N` (with `--compare-next`): lists the WData object-chain differences.
- `RON_ZCHECK`: the saved guy z against `find_data_z` (0 mismatches in the checked saves: z is exact).
- `RON_PAIR_VERIFY=p,q`: a pair-search hit (`RON_PAIR_SEARCH`) counts only if it also makes parts p and q equal.

Findings: the `Build.recharging` diff (24 of 44 compare-next pairs) is the air-defence turret clock of `Wall::inc_time` @63fb60 (85 of 101 AA buildings sit at 0, the
rest 9 - 91 or negative); it needs `F8`/`F12` of the AA graphics events, which building_graphics.xml does not carry (the BUILD entries are stubs): **closed in 6.18b** (the art is the `<UNIT>` of the same name).
The `Region.flags 0x80`, `Build.frame_started`, herd-position and WData `who/val/was_seen` leads of 6.16 are load artifacts: the replay's embedded save is written at
`Game::run` start after a load, and region cursors and several lists are not saved, so the next-boundary compare shows them without a simulation cause.

### 6.18 Eighteenth pass, part A (2026-10-26): the wall of the 15-unit line-formation move at 5,594 (21'23'34)

Three passes had deferred it: at 5,594 player 1 issues a MOVE_TO to a 15-unit group (5 squads of three, line formation, `form = 0`, `width = 50`); `guys` / `units` differed at 5,600, `world` at 5,606.
**Closed.** The cause is one line of `ObjectsData::find_unit_ordered_with_radius` @658ef0 (`search.cpp`): the asm measures the distance to the unit's *order target*
(`Unit +0x70 / +0x74` = `orders_x / orders_y`, `MOV EAX,[ESI+0x74] / [ESI+0x70]` @658fdc / @658fe5), the engine used the unit's *position*. `UnitType::find_nearby_spot` @61de70 calls it for every candidate spot when
its leader argument is >= 0, `nocheck` is 0 and the `use_coll` shortcut does not apply (`uber != 0`, or a filter other than NOT_ME / CAN_COLLIDE): the captain of a relocated squad (`Group::action_move_near` @704990,
`find_nearby_spot(slot0, 0xc0, 0x480, -1, angle, NOT_ME, ..., uber = 1)`) must find room for the whole squad *and* keep clear of the slots already handed to the other captains. Captain 1/38's slot (11852,59413)
sat on the Barracks, so it was relocated around slot 0 (12697,59602): the original rejects all 31 directions of the first ring (r0 = 0xc0; each lies within the exclusion radius `block_radius + (uber_size - 1) * guy_spacing / 2 + 0x30`
of the order target of 1/17 or its neighbours), the engine (positions: nobody stood there yet) accepted the first, (12744,59400). The original's spot is the first of the second ring (r0 = 0xc0 + (0x480 - 0xc0) / 8 = 300): **(12744,59304)**,
and the followers 1/39, 1/40 are placed beside it ((12888,59352), (12600,59256) instead of (12888,59448), (12600,59352)). That is all: `guys`, `units` and `world` match through 8,330 / 8,336 afterwards.

How it was found (the method is worth keeping):
- `RON_TRACE_FORM=1` (`groups.cpp`): one line per member of every `Group::action_move_near` with its slot, the validity test of the slot and of slot 0 (`UnitData::invalid_loc`), the relocation flag and the final target. The 15 units are listed in
  sim-implementation "eighteenth pass, part A".
- The `world` part is a precise filter: it holds the 16x16-bit `CollBlock` footprint of every guy (48-unit cells) and the object-list heads, so a variant that changes any position by a cell or a speed by one breaks it at 5,600. With the formation code
  as is, `world` matched at 5,600 and `guys` did not: every error was below a cell. Variants of the three branches that no earlier segment had ever run (the "behind" branch of `Unit::do_group_move` with its half speed, the relocation, the ungroup on an invalid
  slot position) were tried; those that kept `world` were adjusted one unit at a time (`RON_TGT`-style offsets of one final target) and the **additive** Adler deltas (a, b) of (`units`, `guys`) of all single-unit variants combined (pairs, triples); one variant
  (1/38's target 2 cells north) made all four residues equal, which named the ring (r0 = 300, first direction) and from it the `find_unit_ordered_with_radius` distance. Branch coverage (`clang -fprofile-instr-generate`, lines run between frame 5,587 and 5,600 and never before) showed
  that the only new `engine/sim` lines were those three branches; a corpus count (45 replays, `GMTAG` lines of `do_group_move`) showed that the "behind" branch, the relocation and the lost-slot ungroup never run before a replay's first divergence anywhere else.
- `RON_FOOT_SEARCH=1` (with `--solve-at B`, `apps/sav`): the CollBlock stream positions of the world trace, then every single guy moved by up to +-3 cells (and pairs of such moves) against the world delta. (Negative on this wall: the world delta at 5,606 is not a footprint move.)
- `RON_NEAR_AT=F,x0,y0,x1,y1` (`apps/sav`, at a boundary): every unit inside a box with its orders.

Result on `21'23'34`: fully equal boundaries 932 -> 1,083 of 3,056; first differences `units` 5,600 -> 7,220, `guys` 5,600 -> 8,336, `world` 5,606 -> 8,342, `builds` 5,750 -> 8,354, `groups` 5,954 -> 8,828, `leaders` 5,912 -> 9,134, `cities` 6,404 -> 6,506,
`ammo` 6,896 -> **matches to the end**, `deaths` 7,094 unchanged; `items` end -> 12,782 (earlier, explained below). Over the 45 replays 1,140 -> 1,291 fully equal boundaries.
The new frontier of `21'23'34`: `cities` 6,506 (alone: every other part matches through 6,517), `deaths` 7,094, `units` 7,220, `guys` 8,336 (the boundary after player 0's MOVE_TO of the lone type-324 unit 0/18 with `set_angle` at 8,330; not verified to be its cause).
`items` 12,782: the `ever_seen` byte (`Item +0x20`) of ruin item 4 has player 0's bit set in the engine four frames earlier than in the original (player 0's villager 0/64 reaches the ruin's line of sight at 12,778); it follows `units` 7,220, the part had matched before only by luck.

### 6.18b Eighteenth pass, part B (2026-10-26): the air-defence building clock, the unpack facing

- **Air-defence clock closed.** The `Build.recharging` (+0x7a) of the AA buildings (523 Air Defense Gun, 524 Radar Air Defense, 525 SAM Installation) is the animation clock of `Wall::inc_time` @63fb60 (combat-spec 4.13.4). Its `F8` / `F12`
  are not the 3 of the stub `<BUILD>` entries: `GraphicPieces::verify_load` @906550 loads the DEFAULT / age 0 build pieces of types 0x20b / 0x20c / 0x20d with `init_unit_data`, i.e. from the `<UNIT name="AIRDEFENSEGUN-DEFAULT-AGE0">` of
  unit_graphics.xml: F8 = frames of CHAR_WALK (the "Unpack" animation) = 20, F12 = frames of CHAR_ATTACK2 = 10 (SAM 20 / 20). The saved values in 23'12'35 (0/2110: 13 counting down without a target; 0/2111: 19 = F8 - 1 held with a target near)
  are what fixed F8 = 20. The first difference of `builds` in 23'12'35 moved from boundary 83,421 (S+1) to 83,445 (S+25, downstream of `units` 83,427). Over the 44 compare-next pairs the Build.recharging differences of AA buildings drop from 14 (six pairs)
  to 4 (three pairs), all of them in pairs where the aircraft / target state diverged first; the other Build.recharging differences (gather counters of woodcutters / mines / wells, the Terra Cotta timer) are not this clock.
- **The 21'23'34 packer transients are not timing.** The four boundaries 4,466 / 4,826 / 4,874 / 4,886 are exactly the boundaries where a packed fishing boat (type 317, owner 1: 1/28, 1/25, 1/24, 1/20) sits in the [MOVE_TO][CAST UNPACK] state `Unit::unpack_merchant` @6038e0
  creates (the state lasts 3 - 7 frames, so every period contains one boundary): the facing of that MOVE_TO is the angle to the tile CORNER (`tx * 192 - x`, `ty * 192 - y`), not to the order's own target point (+0x18), and `update_action` copies it into `Unit.dest_angle`.
  Fixed (`engine/sim/packers.cpp`); `units` of 21'23'34 now matches through 5,594 (first difference 5,600, the 15-unit line formation).
- **Corpse `cur_frame`**: `DeathObj::inc_time` @8d5240 (called by `Objects::inc_time` for every valid DeathObj) does `cur_frame (+0x40) += 1`; the only other writers are `DeathObj::init` (0) and `clear` (-1); the presentation functions
  (`DeathObjOut::draw`, `draw_underlay`, `inc_bleed`, `clear_anim_hints`) never write it. It is simulation state and part of `deaths`; nothing in it is presentation-driven.

New diagnostics (env-gated, `apps/sav`; **note**: in zsh an unquoted `env $v prog` passes a variable list as one variable, set several variables in the command line instead):
- `RON_UNIT_TRACE=who:o:F0:F1`: one line per frame (before its `do_frame`) of one unit: position, masks, spell_time, idle, rare / good_obj, orders_x/y, dest_angle, the order list and guy 0; with `o = -1` every unit of `who` that has a pack / unpack CAST order.
- `RON_UEXP=who:o@B:ops`: edit one unit before the sums of boundary B (`fl<i>=hex`, `m^=hex`, `m2^=hex`, `idle=`, `wait=`, `spell_time=`, `paid<i>=`, `pop<i>`, `dang=`, `ang=`, `ox=`, `oy=`, `tol=`, `qt=`, `rare=`, `good_obj=`); `--verbose` lists the
  mismatching boundaries of each part (`grep "part units "`), which is how a single transient is tested.
- `RON_UBF=who,o,B` (with `--replay`): ~1,500 single-member perturbations of one unit and its orders at boundary B, each hashed against `units`; then every pair and triple whose Adler delta (a, b) adds up to the target delta.
- `RON_SOLVE_OBJ=who,o` (with `--solve-at`): two changed bytes inside one object, plus the copy / constant search; `RON_SOLVE_OBJ_DUMP=1` lists the hashed ranges of that object with their byte sums.
- `RON_SCAN_PACKERS=1`, `RON_AMMO=1`: the units with a pack / unpack cast and the in-flight ammo of the loaded save; `RON_AA` also prints the gpiece, the turret angle (`job_counter`), the founder and the age.

### 6.19 Nineteenth pass (2026-10-26): the caravan-link allocation, the DeathObj numbering, `check_idle`, the worker raid, the AA rolls, the wreck piece, the kill values

First differences of `21'23'34` before -> after: `cities` 6,506 -> 8,606, `deaths` 7,094 -> **matches to the end**, `units` 7,220 -> 8,336 (`guys` 8,336, `world` 8,342, `builds` 8,354, `groups` 8,828, `leaders` 9,134, `items` 12,782 unchanged: all downstream of the lone boat 0/18 that starts a MOVE_TO with an explicit facing at 8,330, still open); fully equal boundaries 1,083 -> 1,388 of 3,056, over the 45 replays 1,291 -> 1,656.
Every fix is a small rule found from a single-part delta (details and addresses: docs/sim-implementation.md, nineteenth pass, items 116 - 127):

- `cities` 6,506: the first trade route of the game adds a link to two cities and the hashed `Array<CaravanLink>` header holds the **allocation** (`size`): 4 for a capital that came from a load (an empty array frees its list on load), 10 for a city founded in the game (`City::init` allocates 10). `City::vans_cap`, 6,506 was a 6-byte-sum delta on city 0/1 (`size` 4 vs 10).
- `cities` 8,030: `Build::activate` stores the granary / lumber-mill byte at once. `cities` 27,323 (22'03'19): a repairing peasant is not a raid on its city (only GATHER orders are: vslot +0xe4 of the order).
- `deaths` 7,094: three fields of `DeathObj`: the skeleton piece (0xead1 + index), the ammo piece in the original numbering, the shot's angle relative to the victim's heading.
- `units` 7,220: `Unit::check_idle` drops BLOW_COVER after 10 idle frames (two squad members at 7,220, a third at 7,226: `a = -32`, then `-48`, i.e. a byte of `unit_masks` lower by 16 in three units).
- `ammo` 83,150 (23'12'05): the anti-air roll branches; `ammo` 83,421 (23'12'35): the wreck's own piece in Ammo +8; `leaders` 27,768 / 82,651: `Leader::score_combat` (+0x44) gets `kv / 5` with the queued units / buildings in the kill value.

How the cluster was found (new): `w = b * a^-1 (mod 65521)` of a part's Adler delta is the stream position of a single cluster of changes (`RON_AT=off,..` names the range holding an offset, `RON_RANGE_OF=who,o` lists one object's offsets); `RON_SOLVE_DUMP=1` prints the stream and the original's hash so a hypothesis family (every dword +-R, pairs by meet-in-the-middle) is a few lines of Python;
a **single byte of `e = -16` in three units** (the same member, three objects) is the signature of a flag bit that the engine keeps and the original drops. `RON_TRACE_DIFF` must align by object (a stream whose objects grow cannot be compared by position). The generator-offset experiments (`RON_RNG_SKIP`, `RON_DROP_N`) are only meaningful when the dependent value has a wide range:
a scatter of radius 5 matches by chance at about one shift in 25.

New diagnostics (env-gated, `apps/sav`): `RON_CITY_DUMP=F0:F1`, `RON_DEATH_DUMP=F0:F1`, `RON_AMMO_DUMP=F0:F1`, `RON_LDR_DUMP=who:F0:F1` (every valid city / corpse / ammo / a leader's score fields before each frame, cities only when changed), `RON_VANS_SCAN=1` (+ `RON_SEARCH_AT`: the `Array<CaravanLink>` headers as hashed),
`RON_TRACE_DIFF=part:B0:B1`, `RON_AT=off,..`, `RON_RANGE_OF=who,o`, `RON_SOLVE_DUMP=1` (with `--solve-at`; `--solve-at` now also traces `rules`, `scenario_data`, `script_run_time`), `RON_UT_FULL=1` (with `RON_UNIT_TRACE`: angles, order dest, path, guy 0), `RON_UEXP2/3` and the op `oflags^=hex`, `RON_RNG_SKIP=F:k`, `RON_DROP_N="F:n:m,.."` (+ `RON_RNG_QUICK=1`, profiling build),
and `re/ghidra_scripts/ReadWords.java` (words of memory with the function they point to: vtable slots).

**Not engine-driven**: the `scenario_data` part is the loaded shadow only (`last_razed`, `units_killed`, `builds_destroyed` are updated in `g.scen` but not hashed); `23'11'20` differs at 82,651 for that reason (the original's `last_razed[0]` = the razed Fortress 2,116). (Twentieth pass, 6.20: `last_razed`, `units_killed`, `builds_destroyed` and `city_lost_to` are engine-driven now.)

### 6.20 Twentieth pass (2026-10-26): ScenarioData in the hash, the missile ammo's B-spline, the twin and ship tools

First differences of `21'23'34` before -> after: unchanged (`units` / `guys` 8,336 still open); fully equal boundaries over the 45 replays with a save 1,656 -> 1,666. Details and addresses: docs/sim-implementation.md, twentieth pass, items 128 - 134.

- **`scenario_data` follows the simulation**: `last_razed[8]`, `units_killed[8][352]`, `builds_destroyed[8][129]`, `city_lost_to[8]` are loaded from the save and emitted from the engine's own values (the statics are separate globals of `ScenarioData::walk_data` @997ad0). The only writers of `last_razed` are `Object::take_damage` @652f69 and `Object::disband` @64592c (valid and active). `23'11'20` scenario_data (the razed Fortress 2,116) now matches to the end.
- **Missile ammo have a path** (`AMMO` flag MISSILE: rockets, torpedoes, missile artillery): `Ammo::ammo_path` is a degree-2 B-spline through (start, launch control point, end) with one point per frame; `ammo` hashes the whole `SplineData`. The saves carry four of them (`22'45'30`, `22'28'15`), and the port regenerates all four bit for bit (points, tangents, length, depth, allocation 4 / 16 / 16 / 8). The rocket shot of `22'45'30` at 60,726 that the plain flight time made 109 frames long has 13 points.
- **The corpus is made of OOS recoveries**: all 45 embedded saves are `OOS_RECOVERY.SVX`. A segment's first frames come from a state every player loaded; `RON_TWIN_STEPS` shows that a continued engine and a loaded one (with the prologue) differ at once and that the loaded one matches the rows.
- New diagnostics (`apps/sav`, env-gated): `RON_SHIPS`, `RON_SPLINES`, `RON_SPLINE_CHECK`, `RON_FIELDS`, `RON_STREAM_OUT`, `RON_NEW_OBJECTS`, `RON_TWIN_STEPS` (+ `RON_TWIN_NOSTOP`, `RON_TWIN_SKIP_FIRST`).
- Method note: a `--solve-at` single-byte candidate `e` is realisable only if the engine's byte can reach the original's (`e = -253` needs an engine byte >= 253); candidates that print `engine 0x00 -> original 0x03` for `e = -253` are aliases of the modular equation, not hits.

### 6.21 Random-map replays: the generated start (stage C, 2026-10-26)

A recording without an embedded save starts from the lobby: `ron-sav <rcx> --replay` and `ron-net replay <rcx>` (and `Playback::open` / `make_start`, kind `OriginalRandom`)
build the game the way `Setup::build_game` does from the header, then run the same loop as for a save. The path, in order (`engine/net/playback.cpp`):

1. **`original_random_setup`** turns the header (setup bytes, `GameInfo.flags`, `seed`, the Player records) and the random-game-info block into a `RandomGameSetup`: map style (the
   lobby choice), size, seed, one seat per present player in `who` order, the resolved nation (`leaders[who]+0x0c`, a header tribe of 24 is "random"), the resolved team (a header
   team of 5 is "random team"), human (`flags & 4`), AI difficulty (`GameInfo.difficulty`), colour = seat, every GameInfo setting `new_random_game` takes, and
   `randomize_starts` = multiplayer. **Cross-check**: `ron-sav <rcx> --setup-check <save>` against the saves of the same games (the seven pairs of oracle-corpus.md section 8): equal in all 31
   fields. `apply_original_lobby` sets what the setup text does not carry (`starting_resources2`, `starting_technology2`, `chairs`, the raw `flags`, the Player flags and team bytes).
2. **`new_random_game`** on the install's rules for the header's `GameSetup` (`start_game` with the second starting technology and the game rules of the header), then
   `original_post_load_effects` (the playback prologue) and the game script of the header.
3. **The checksum shadow.** The 15 parts are hashed by `EngineProvider`, which lays the engine's state over the decoded save it was loaded from (`Game::original`); a generated
   game has none. `original_random_game` loads a template: the embedded save of `2026.08.30 21'23'34` (a two-player game at frame 7, so its scenario data, lists and headers are those of a fresh
   game; `RON_SHADOW_SAVE=file.SVX|file.rcx` names another, the fallback is `Saves/current 2.SVX`) and hands it to `new_random_game` through `RandomExtras::original` (so the start towns'
   `Terrain::object_placed` land runs as in a loaded game). After the playback prologue `prepare_fresh_shadow(game)` makes the shadow fit: every container of the walk that the engine owns
   (an `ArraySpec`) gets the engine's element count, template elements the engine has no object for are dropped, elements beyond the template get a copy of a template element; goods and items
   get `SubObject` +9 `who` = -1, +0xa `o` = the index, +0xc `z_internal` = `find_data_z(x, y, 1)` (all 141 of the chain's goods and items agree); the World's unmodelled members are written from
   the generator (`patch_world_shadow`: map, the six territory limits, the ten `analyze_map` counts, the seed, `start_x/y`, `start_city_x/y` as the four wcoords of each start's site, the
   `Terrain.halfland_*` log); the recording's own rules block replaces the template's (`transplant_rules`: the `rules` part hashes the data revision of the game, 314192132 in 2026 and
   3907925208 in 2025); a game without script has `load_scenario_script` 0 and an empty script runtime (the 160574352 / 262145 of every scriptless multiplayer row).
   Check: `ron-sav <save> --shadow-test` (the 15 parts of a loaded save, then of the same engine state over an emptied or another save's shadow once prepared; `RON_SHADOW_TEMPLATE=other.SVX`).
4. The loop is the one of section 6; the first row is the boundary after one frame (`start + 1`), `ron-sav --compare-next` against the embedded save of the next segment is a full-state oracle of a
   fresh start for the chain (`21'22'28` -> `21'23'34`, frame 7) and, for single-player, `Playback - 2026.07.11 00'24'00` -> `Playback - 2026.07.19 01'52'12` (frame 189, `current 2.SVX`: a recording without rows
   is compared at the next save's frame only).

What the first rows showed about the generator is in docs/randommap-spec.md "Replay first rows". Diagnostics added to `apps/sav`: `--setup-check`, `--extract-save F`, `--shadow-test`, `--analysis-check`
(the World members the engine does not model against a save's own: `RON_REGION_DIFF`, `RON_WINDOW=x0,y0,x1,y1`, `RON_WORLD_ARRAYS`, `RON_HALFLAND=n`, `RON_GOOD_Z`, `RON_ANALYZE_TRACE`), `RON_DUMP_PART_START=k` (the labelled stream
of part k of the start state), `RON_STREAM_DIFF=n` (with `--compare-next`: the first n differing bytes of every differing part, labelled), `RON_CMP_BUILDS_ALL`, `RON_DUMP_CONTAINERS`,
`RON_DUMP_STREAM=13|14` (+`RON_DUMP_STREAM_FILE`), `RON_PLACE_TRACE=1` (every `Setup::place_unit` attempt of the generator).

First rows (`ron-sav --replay`, boundary 1; "=" parts of 15; `re/oracle/out/replay-table.tsv` has the whole corpus):

| replay | first row: parts equal | differing at the first row | first later difference of the equal ones | boundaries compared / all equal |
|---|--:|---|---|---|
| `Multi/2025.06.08 22'44'37 (Sun)` | 6 / 15 | units builds guys leaders cities items goods world rules | ammo 34495 deaths 30979 groups 73 scenario_data 39865 | 10546 / 0 |
| `Multi/2025.06.09 11'09'09 (Mon)` | 8 / 15 | units builds guys leaders cities items world | ammo 14899 deaths 15439 groups 103 goods 1207 scenario_data 46585 | 12359 / 0 |
| `Multi/2025.06.10 21'42'55 (Tue)` | 7 / 15 | units builds guys leaders cities items goods world | ammo 21397 deaths 319 groups 157 scenario_data 39211 | 9948 / 0 |
| `Multi/2025.06.11 22'24'21 (Wed)` | 9 / 15 | units guys leaders items world rules | through the end | 1 / 0 |
| `Multi/2025.06.12 17'39'28 (Thu)` | 9 / 15 | units guys leaders items world rules | through the end | 1 / 0 |
| `Multi/2025.06.12 17'46'22 (Thu)` | 8 / 15 | units guys leaders items goods world rules | through the end | 1 / 0 |
| `Multi/2025.12.19 21'28'29 (Fri)` | 7 / 15 | units builds guys leaders cities items goods world | groups 73 | 503 / 0 |
| `Multi/2025.12.19 21'43'27 (Fri)` | 7 / 15 | units builds guys leaders cities items goods world | ammo 24595 deaths 25243 groups 325 scenario_data 34879 | 12278 / 0 |
| `Multi/2026.07.19 02'49'17 (Sun)` | 10 / 15 | units guys leaders items world | builds 175 ammo 7057 deaths 7123 groups 43 cities 1201 goods 19 | 2025 / 0 |
| `Multi/2026.08.30 21'22'28 (Sun)` | 6 / 15 | units builds guys leaders cities items goods world rules | through the end | 1 / 0 |
| `Multi/2026.08.31 22'39'48 (Mon)` | 8 / 15 | units builds guys leaders cities goods world | ammo 9121 deaths 9187 groups 145 items 3499 scenario_data 15793 | 3304 / 0 |
| `Multi/2026.09.05 20'52'07 (Sat)` | 8 / 15 | units builds guys leaders cities goods world | ammo 19729 deaths 23929 groups 655 items 26437 scenario_data 54397 | 9668 / 0 |
| `Multi/2026.09.06 21'11'02 (Sun)` | 5 / 15 | units builds groups guys leaders cities items goods world script_run_time | ammo 4639 deaths 4735 scenario_data 7 | 8468 / 0 |

Totals (re/tools/replay-all.sh, 2026-10-26): 13 generated recordings with rows, 69103 boundaries, none with all 15 parts equal, first rows 7.5 of 15 on average; the 45 saved-replay rows are unchanged (14904 boundaries, 1656 fully equal, first row 14.8 of 15).

## 7. What blocks exact checksums

State of the fidelity work (2026-10-24; the log of each fix is in docs/sim-implementation.md "Fidelity log"). **Fixed** since the first table:
the gaia animals (`Animal::process` / `work` / `do_idle`, the `Herd` records and `Herds::process`, the herd goods' `ever_seen`, bird flight,
pen animals), the `LEADER_NEW_UNITS` flag, the walking gatherers' `Build.recharging`, the idle flag of moving units, the complete
`Unit::update_speed`, the herd branch of `AnimalData::is_seen`, the loaded game's leader slot 9, the game script of the replay header.

What still **separates the engine from the original**, in the order the replays meet it:
- **Movement in the first frames.** (Fixed in the fifth pass, section 6.5: the `units` delta at boundary 8 was the path stack's allocation size.) `21'23'34` matches `guys` and `world` at boundary 8 (the first step from the save: ten citizens walk, the first
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
