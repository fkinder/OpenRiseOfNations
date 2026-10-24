# Simulation state model: the DataWalk and the save format

Status: ✅ visitor mechanics, primitives, tag bytes (hash reimplemented, all observed tags reproduced), save container,
top-level walk order (`WalkDataGame::walk_data`), checksum-part mapping · ✅ per-class walks (machine-readable in
`re/iface/datawalk.json`), **validated on real data**: the 10 `.SVX` saves of the original game and the saves embedded in 59
`.rcx` replays all decode to the last byte with every section tag matching (§8). The description was reconstructed from this
listing's own generated copy (`docs/state-walk.md`, via `re/tools/statewalk-to-json.mjs`) and corrected in four places by the
real data (§8). · 🟡 reimplementation-side questions in §9.

Deliverables:
- `re/iface/datawalk.json`: every walk function as a list of steps (machine-readable; built by `node re/tools/savdump.mjs --build`
  from the fragments in `re/iface/datawalk/*.json`). The step language is in §3.
- `docs/state-walk.md`: the same, rendered as readable pseudo-code with PDB field names (`node re/tools/savdump.mjs --doc docs/state-walk.md`).
  It is **lossless**: `node re/tools/statewalk-to-json.mjs` rebuilds the fragments from it (`re/iface` is gitignored, so on a fresh
  checkout the listing is the only committed copy of the description), and `--doc` of the rebuilt description gives the listing back byte for byte.
- `re/tools/savdump.mjs`: a generic decoder that interprets the JSON (saves, the save embedded in a replay, the replay header,
  the rules block, or any single walk function at an offset).
- Helpers: `re/tools/pdbtypes.mjs` (class layouts straight from `rise.pdb`'s type stream, base classes included; Ghidra's
  import drops base classes for most game classes), `re/tools/walklabel.mjs` (section labels and tag bytes),
  `re/ghidra_scripts/DecompileWalk.java` (decompiles with a typed DataWalk vtable so the visitor calls keep their arguments).

Addresses are `riseofnations.exe` VAs. Names are the PDB names.

## 1. Why this matters

`DataWalk` is the single serializer of simulation state. The same `walk_data` methods drive four things:

| Visitor | `input` | `checksum` | `flags` | `walk_function(a, b)` | `walk_test(label)` |
|---|---|---|---|---|---|
| `SaveGame` (inlined ctor, e.g. `RecordGame::write_header`) | 0 | 0 | −1 | write `[a, b)` (`fwrite`/`gzwrite` @43d730) | write 1 byte: low byte of the label's case-insensitive hash (@43d840) |
| `LoadGame` (ctor @43d8a0) | 1 | 0 | −1 | read into `[a, b)` (@43d950) | read 1 byte, report "Error loading section, probably in <last label>" on mismatch (@43da60) |
| `CheckSum` (ctor @45dec0) | 0 | 1 | −1 | `accum = adler32(accum, a, b−a)`, `size += b−a` (@936ff0) | no-op (@41bfe0, folded) |

There are no other implementations (the vtable has exactly these two slots; `SyncLogger` only receives checksum values).
So the list of bytes a class walks **is** its persistent simulation state, and the subset walked when `checksum = 1`
is what multiplayer peers compare. Anything not walked is either derived on load (rebuilt by `init`/`reload_data`
functions, listed as notes in the JSON) or presentation.

The walk is not self-describing: there are no lengths except the ones the code walks, and no field names. A wrong
size anywhere desynchronises everything after it, which the 1-byte section tags catch at the next section.

## 2. Primitives

| C++ | Bytes | Notes |
|---|---|---|
| `walk_function(&x, &y)` | the raw memory `[&x, &y)`, little-endian x86 layout, **including struct padding** | Ranges usually span many consecutive members (e.g. `Unit` +0x48..+0xb7); the struct layouts are in `rise.pdb` (`node re/tools/pdbtypes.mjs <Class>`) |
| temporaries | 1, 2 or 4 bytes | Many walks write a computed value through a stack temporary: a "present" byte before an optional pointer, a count, a type id. The writer's value is given as the `save` expression of a `local` step |
| `walk_test(label)` | 1 byte | `hash_value_insensitive & 0xff` of the label String. Labels come from `Data/internal_strings.xml`: code passes `int_str_array->list + off`, i.e. the `(off / 0x14)`-th `<STRING>`. Hash: `String::generate_hash` @a1b6b0, see below |
| `String::walk_data` @a1b2d0 | `u32 n`, then `n` UTF-16 units | no terminator; `n` = `curr_len` |
| `BitMask<N>` members | `i32 bits, i32 size` (raw `[&bm, &bm.flags)`), then `size` bytes (the inline `ptr` array) | on load `flags := 2` |
| `Array<T>`, `ObjectArray<T>`, `NamedObjectArray<T>` | `i32 length`; if ≠ 0: `i32 size`, `i16 increment`, `u8 flags & ~0x40`, then each element's walk | element walk inlined per instantiation |
| `SimpleArray<T>` | same header, then one raw block `length × sizeof(T)` | |
| `PtrArray<T>` | header; `length × u8` present flags; raw `size, increment` again (6 bytes); then each present element's (virtual) walk | the loader allocates the concrete class |
| `MultiPtrArray<Object>` (the per-player object lists) | header; present flags; for each present element `i32` data type (`ObjectsArray::get_data_type`, recreated by `get_new_data`); raw `size, increment`; each present element's vtable `+0x7c` walk | dispatch table in the JSON |
| `Stack<T>` (unit path) | raw `size, length, increment` (9 bytes), then `length` raw elements | |

**Tag hash** (`String::generate_hash`, insensitive result via EDX): for `i = n−1 … 0`, with `c` the UTF-16 unit and
`l = towlower(c)`, `k` counting down from `n`:
`h += P[i % 50]·c + P[c % 50]·k` and `hi += P[l % 50]·k + l·P[i % 50]` (32-bit wrap), where `P` is the 50-entry
`primes` table @b14500 (127, 811, 1597, … 89009; in `re/tools/walklabel.mjs`). The tag is `hi & 0xff`. This
reproduces the hash attributes in `internal_strings.xml` and every tag seen in replays (Game 0x16, GameInfo 0x42,
Player 0x50, Game Rules 0x92, Tribe rules 0x8f).

## 3. The description language (`re/iface/datawalk.json`)

```
{ "format": "ron-datawalk/1",
  "walkers": {...}, "containers": {...},           // §1, §2
  "functions": { "<Class>::walk_data": { "addr", "this": <PDB class>, "params": [...], "body": [steps], "note", "confidence", "issues" } },
  "globals":   { "<name>": { "class", "symbol" } },  // contexts walked by name (GameAccess::world, leaders, ...)
  "checksums": {...},                               // §6
  "labels": { "<label>": tagByte }, "coverage": {...}, "problems": [...] }
```

Steps (offsets are relative to the step's context: `this`, or the global named by `"obj"`):

| op | meaning |
|---|---|
| `tag {label}` | 1 tag byte; `text`/`byte` are filled in by the build |
| `raw {at, end}` | `walk_function(ctx+at, ctx+end)`; `end` may be an expression |
| `local {name, type, save}` | a temporary; `save` = what the writer stores; afterwards `L.name` holds it |
| `string {at}` | `String::walk_data` |
| `bitmask {at}` | `BitMask` pattern above |
| `call {fn, at, args}` | another walk function on `ctx+at` (callee's locals become visible to the caller) |
| `if {cond, then, else}`, `loop {var, count, body}` | control flow; conditions are JavaScript over the context |
| `deref {ptr, index, stride, class, body}` | switch to a heap object reached through a pointer member (the pointer itself is not stored) |
| `buf {ptr, len, elem}` | a heap buffer of `len` bytes |
| `container {kind, class, stride, elem / dispatch, count, header, node}` | the template families of §2. Linked lists (`LinkList`, `PtrLinkListAbstract`, `OrderList`) take their node `count` from a `count:` expression (or a `count`/`length` local walked before), per-node `node:` steps and `element:` steps |
| `note {text}` | a load-time side effect worth knowing; no bytes |

Expressions see `input`, `checksum`, `flags`, the function's params, loop variables, `L.<local>`,
`i8/u8/i16/u16/i32/u32/f32(off)` (the current context's already-walked bytes), `up.*(off)` (enclosing context),
`G("global").*(off)`, `LEN("global")` (length of the last container walked on that global), `save_version`, and
`rt("name")` for the few values the loader takes from game data instead of the stream (`RUNTIME_VALUES` in `savdump.mjs`:
`GraphicEvents.events.length` = 60415). **Conditions are written in the loader's view**: every branch that changes the
stream layout depends only on bytes already in the stream, walker fields, params or earlier globals. Where save and load
code differ but produce the same bytes, the JSON has one sequence. `checksum` and `flags` conditions are kept: they
select what a checksum hashes versus what a file contains.

## 4. Save files

`SaveGame::save_game` @5a8220 / `LoadGame::load_game` @5a72f0, `do_save` @5a81f0, `do_load` @5a6e30:

```
gzip stream (File::open mode 3 = write + gz; loaders accept plain files too)
  String magic        u32 n + UTF-16: "RoNSave" (single player), "RoNMultiSave" (semaphore byte0 & 4), "RoNCTWSave" (Conquer the World)
  u32 sGameSaveVersion   0x10 in this build (LoadGame::check_version accepts > 0xe; also "RonCTWMapSave")
  WalkDataGame::walk_data   (§5)  — no length
  SaveGame::verify_save     one u32 Adler-32 per subsystem and per world cell / tile / fog cell (§5, item 19); 4 x (size + tile_size
                            + fog_size + 73 + goods.length) bytes; the file ends with it
```

`LoadGame::get_header` reads only the magic. A playback reads the embedded save from the replay stream itself
(`load_game` uses `record_game.file` when semaphore byte0 & 0x10). `SaveGame::verify_save` / `LoadGame::verify_load`
are a debug round-trip that compares per-subsystem checksums.

**In a replay** (docs/replay-spec.md §1, §4): after the header (`Game::walk_data` + `String save_name`) and an optional
CTW header, a non-empty `save_name` is followed by the save file **decompressed**, byte for byte (magic, version, walk).
`savdump --replay` decodes it with the generic engine. The embedded save is complete, verify block included, and is followed by the
random-game-info block (`5 x N + 4` bytes: per entry `i8 team, i32 leader value`, then `World+0x30`; N = 2, 3 or 5 in the corpus) and the
command stream; in all 59 replays that embed a save the walk ends exactly there (docs/replay-spec.md §4). A replay without a save
(`save_name = ""`) has the random-game-info block and then the rules block; N can exceed the header's player count in lobby and
scenario games, so `savdump` probes for the rules tag like `rcxdump`.

The file is a gzip of about 15 % the size: the 10 saves of the corpus are 4.2 - 7.9 MB decompressed (0.6 - 1.2 MB stored).

## 5. Top level: `WalkDataGame::walk_data` @5a2360

The order of the whole save (each line is a call or raw range; full detail in `docs/state-walk.md`):

1. `GameInfo::walk_data` (game settings, players; also inside `Game::walk_data` below, so it appears twice)
2. Console: `who … target_color`, `target_color` (10 bytes), `target_cx … sys_quit_popup` (selection, camera target, flash state)
3. `obj_base[3]`, `obj_end[3]` (object index ranges per kind: units, buildings, walls)
4. `Game::walk_data` (tag Game; GameInfo; the 0x194-byte frame/tick/market block; semaphore; `graphic_tick`) — same bytes as the replay header
5. tag Tribes + `ObjectArray<Tribe>`; `Leaders`; `Types` (per-type runtime state); `TileSet`; `Mountains`
6. `Constants[0, 0xd40)` + `Constants[0x804, 0x808)`; `ai_speed`; `ai_off`
7. `Armies`; `Cities`; tag Forms + `ObjectArray<Form>`; `PtrArray<Good>` (goods on the map); `PtrArray<Item>`; `Heroes`;
   tag Herds + `PtrArray<Herd>`; `Specials`; `Wonders`; `Forts`; `Docks`; `OilWells`; `Supplies`; `Caravans`;
   tag Lands + `ObjectArray<Land>`; `LeaderOptions`; `OptionInfo`
8. `pathfinder[0x58, 0xc4)`; `Array<Group>`; tag Groups + `last_group[8]`, `proc_group`
9. **`Objects::walk_data`** (every unit, building and wall of every player, plus ammo and death objects)
10. tag HotKeyGroups + `Array<HotKeyGroup>`; `World::walk_data(part = −1)`; `game_daemon[0, 0x28)`; **`game_random` (4 bytes: the simulation RNG seed)**
11. `GraphicEvents`; `Scene`; tag Farms + farm colour/heights + `Array<FarmStruct>`; `UnbuiltWonders`; `UnbuiltCities`; `UnbuiltForts`; `ConquestGame`
12. `detail_threshold` (float); tag Camera + Camera ranges; tag SelectGroups + 2 × `Array<SelectGroup>`; `Array<Option>` + tag + Options state
13. `CommandManager`; `PtrArray<River>`; `Terrain::walk_coord_data`; `MessageWin`; `Terrain::walk_data`; `CliffsData`; `Doober`
14. `Regions` (`sea`, `land`, `ObjectArray<Region>`, `Array<WCoordData>`); `Terrain::walk_roads`
15. `System::command_wait_hold`, `ArmiesData::find_dist`, `BuildData::queue_fail` (4 bytes each; statics)
16. `Achieve`; `ScenarioData`; `RunTimeEnv` (scenario script VM state)
17. tag TurnControl + `semaphore` BitMask + `TurnControl[0x14, 0x169)`
18. **`Game::walk_rules_data`**: the complete rules image (docs/replay-spec.md §6, docs/rules-spec.md). A save therefore
    carries its own rules, like a replay.

19. **`SaveGame::verify_save`** (not part of `WalkDataGame::walk_data`; `SaveGame::do_save` calls it through vtable +0x10 after the
    walk, `LoadGame::do_load` checks it through +0x18): the debug round-trip block, 61 named entries, one `u32` each, the Adler-32
    (start value 1) of the same walk in checksum mode: Game, TileSet, Mountains, [empty], Constants, game_daemon, Armies, Cities, Forms,
    **one per Good**, the goods and items arrays, Regions, Heroes, Herds, Specials, Wonders, Forts, Docks, OilWells, Supplies, Caravans,
    Lands, **one per Leader 0..8**, all leaders, techtypes, LeaderOptions, Tribes, OptionInfo, pathfinder, Groups, HotKeyGroups, **one
    per object list** (9), Objects, **one per world cell** (`WData[0, 0x15)`), **one per tile** (`tdata` u16), **one per fog cell**
    (`seen`, `seen2`, `seen3` bytes), World, roads, GraphicEvents, [empty], Scene, MessageWin, Doober, Farms, the three Unbuilt lists,
    TurnControl, then the CHECK_SUMS-style parts units, builds, walls, guys, ammo, groups, leaders, world, cities, goods, items and
    the script run time. An independent oracle: the bytes of the per-cell families are the stream bytes, and all of them match
    (§8).

Three facts about the stream that the byte ranges alone do not show:
- `GraphicEvents::walk_data` writes **one presence byte per `EventGroup` slot** (`events.length` = `first_ammo_piece + ammo_names.length`,
  60415 in this build; not in the stream) and the group chains of the present slots; about 70 KB per save.
- `Terrain::walk_coord_data` writes `xs*ys - 1` u16 in total: list 0 is empty and the corner cell (0,0) sits in the last list, which is
  never walked (§8, correction 3).
- A `PtrLinkListAbstract` node is `i32 data type, u8/i32 metric, data`; for `GatherPoint` lists that is 5 + 9 bytes (§8, correction 1).

On load the walk is interleaved with rebuilds that are not stored: Objects are closed first, the per-player `units`
index is rebuilt after `Objects`, `Terrain::init`, crater and doober regeneration, per-type flag lists, the gaia leader
(9) is re-initialised, farm animals are re-added, multiplayer seats are re-matched to network players by name, and
`Objects::reload_data` runs (see the `note` steps).

### Per-subsystem summaries

The full step lists are in `docs/state-walk.md`; these are the facts a reimplementation needs first.

**Map objects** (`SubObject` → `Object` → `Unit`/`Animal`, `Wall` → `Build`; `Good`, `Item`):
- `SubObject::walk_data`: tag, `flags` (bit 0 = alive), a **must_walk byte** (writer: 1 if the object has a type / is
  alive / is held / for units, has `o_up ≥ 0`), then if set: `who, o, z, x, y` (15 bytes) and the type as `u32 TypeIndex`
  (the `ptype` pointer is re-derived on load through `set_type`). Dead slots therefore cost 2–3 bytes.
- Every subclass level re-walks its own must_walk byte (a Unit has 3, a Build 4) and gates its block on it.
  `Object`: `[0x20, 0x42)` (hits, damage, links, uid, …) and an optional `SimpleArray<int>` of launched objects.
- `Unit::walk_data`: three groups selected by **walker flags**: bit 2 = `UnitData[0x48, 0xb7)` (111 bytes: angles,
  orders target, unit_masks, group, garrison links, stance, …) + the path `Stack<PathData>`; bit 4 = the `OrderList`;
  bit 8 = the `PtrArray<Guy>` (each `Guy` = 0x9b raw bytes: per-soldier animation and position). Save, load and
  CHECK_SUMS all use flags = −1. `AnimalData` adds its herd fields.
- Orders: each node is `i32 OrderIndex`, `u8 metric`, then the order's walk; the list is written **tail first** and
  rebuilt by head insertion. `get_new_order` map: 1–4 Move/AttackTo/ExploreTo/FleeTo, 6/8/9/13 Build/Board/AwaitBoard/
  Repair (TargetOrder), 7 Gather, 10 Attack, 11 Follow, 12 Guard, 14 Cast, 15 Trade, 16 Strafe, 17 AirPatrol, 18 Form,
  19 GroupMove, 20 GroupAttack, 21 GroupAttackTo, 22 GroupPatrol, 23 AttackGround, 24 AirAttackGround, 25 SpecialAnim,
  26 Garrison, 27 Think; 0, 5 and ≥ 28 produce no order. Orders with two virtual-base paths write their `UnitOrder.flags`
  byte twice.
- `BuildData`: writes `founder` and `max_age` **before** the SubObject tag, then (gated) `[0x70, 0x86)`, the
  `BuildQueue`, the mining list and the gather-point list, and `orig_type`.
- `Objects::walk_data`: header (valid, ammo_index, good/rare marks, unit/build/wall marks and obj_ctr for **9 of the 10**
  player slots), then 9 `MultiPtrArray<Object>` lists (type 0 Unit 0x158, 1 Build 0xdc, 2 Wall 0x74, 3 Animal 0x160),
  ammo (`PtrArray<Ammo>`: only live ammo walk their body) and death objects.

**Players and AI:** `Leaders::walk_data` = String `prod_script_path` + `LeaderData` for leaders 0..8 (not 9). An
inactive leader stores 8 bytes. An active one stores `[who, …)` (≈27 KB of raw per-leader state: diplomacy, AI
personality, sites, make list, tech/age bitmasks, rares), several `SimpleArray`s and `LeaderDataEncrypt`: 62 u32
economy values (per resource: bucket, leftover, cap, over_cap, resources, support, income, rate, bonus; then caps,
epochs, ages). In memory these are XOR-obfuscated with fixed per-field keys; **the stream holds plain values**.
`Armies`: 8 per-player `PtrArray<Army>`; `Army` = `i16 valid` + 150 bytes if valid. `Group`: 72 bytes + the first `num`
entries of each per-member array (list, offsets, current positions, angles).

**Game and settings:** `GameInfo` (version string and number, seed, checksum settings, flags, 30 setup bytes, 8 Players,
scenario/mod block; the version-0x10 layout is the one in docs/replay-spec.md §3). With `checksum = 1` only seed …
threshold, `flags & ~1` and the setup bytes are hashed. `Game` adds the 0x194-byte frame/tick/market block, the
semaphore and `graphic_tick`. `CommandManager` stores the local package and 8 × 20 queued packages, but **the loader
discards them** (it re-initialises every FIFO); `TurnControl` stores the semaphore and its timing block.

**World and terrain** (`World::walk_data(part)`; every caller passes −1 = all parts): 1 `xs, ys`; 2 start positions; 3 oil
positions; 4 scalars `[0x8, 0x80)`; 5 per-cell `WData` (0x15 of 0x1c bytes each); 6 `tdata`/`seen`/`seen2`/`seen3`
(fog of war per team); 7 `wcoord_seen`; 8 `danger[8]`; 9 per-cell collision-block flag + `BitMask<768>`; 10–13 Terrain
`halfland_*` and `nuke_hits`. `Terrain::walk_coord_data` writes one `u16 CoordInfo.flags` per world cell with **no count**,
in the order of the land lists that `TerrainOut::generate_land_lists` rebuilds (see open questions). `Terrain::walk_data`,
`Cliff` (position only), `River` (creation spline only), `TileSet` (name only) and `Mountains` store inputs, and the
loader regenerates geometry from them.

**Cities and sites:** `Cities` and the per-player families (`Caravans`, `Docks`, `Forts`, `Wonders`, `OilWells`,
`Supplies`, `Heroes`, `Specials`) are 8 per-player `PtrArray`s with different element bodies; `City` strings are
file-only. `UnbuiltCities` has no tag (UnbuiltForts/Wonders do).

**Scripts and scenario:** `ScenarioData` walks scenario statics (find counters, kill/destroy tables 8×352 and 8×129
i16, triggers, objectives (tail first), messages, groups, reveal points). `RunTimeEnv` = `i32 count` + each `ScriptFile`
(debug info only when `checksum = 0`) with the VM's global values. A script value slot is tag + `i32 data_type`
(0 = null), `u16 scope` (0x80 = array), `u16 ref_count`, then by type: 0x57bad ScriptInt (i32), 0x12f35f ScriptFloat
(f32), 0x168174 ScriptString (String), otherwise ScriptArray or ScriptObject (nested slot lists). Shared values are
written once per reference, so a load un-shares them.

**Not simulation** (stored for continuity, never checksummed): Console, Camera, Scene, GraphicEvents (presence bytes and
event-group chains), HotKeyGroups, SelectGroups, Options, MessageWin, `detail_threshold`, Achieve statistics.
Pure render helpers named `walk_*` (`Border::walk_regular_strips`, `walk_clump_strips`, `StripBuilder::walk_strip`) and
`Search::walk_data` (empty) produce no bytes.

## 6. Checksums (the multiplayer oracle)

`CommandManager::issue_check_sums` @940770 (multiplayer, start of every turn, before that turn's packages) and
`CheckSums::check_all` @936560 (during playback, logged to SyncLogger) compute 15 Adler-32 parts with a `CheckSum` walker
(`accum` starts at 1 for each part) and send their 32-bit sum as `all`. Each part is a walk over the structures below,
so with `datawalk.json` evaluated in checksum mode (`checksum = 1`, `input = 0`) a reimplementation computes the
same values from its own state, provided its in-memory layout of each walked range is byte-identical (including
padding bytes, which are hashed; the original allocates most objects zeroed).

| # | Part | What is hashed, in order |
|---|---|---|
| 1 | units | if `objects.valid`: for leaders 0..8 (gaia included) with `leader_flags & 1`, for object index `o` in `[obj_base[0], unit_mark[who])`: if the unit is alive (`flags & 1`), its `walk_data` (vtable +0x7c: `Unit::walk_data` / `AnimalData::walk_data`) with `flags = −1` (path, orders and guys included) |
| 2 | builds | leaders 0..7 active, `o` in `[obj_base[1], build_mark[who])`, alive: `BuildData::walk_data` |
| 3 | walls | leaders 0..7 active, `o` in `[obj_base[2], wall_mark[who])`, alive: `WallData::walk_data` |
| 4 | ammo | each `Ammo` in `objects.ammo_objs` with `flags & 3`: `AmmoData::walk_data` |
| 5 | deaths | each `DeathObj` (0xa4 bytes) with `valid ≠ 0`: bytes `[0, 4)` then `[4, 0x4b)` |
| 6 | groups | `Group::walk_data` for every group, then `groups.last_group[0..7]` (32 bytes) |
| 7 | guys | for the same units as part 1: `PtrArray<Guy>::walk_data` (unit +0xe4) — guys are hashed twice in total |
| 8 | leaders | `LeaderData::walk_data` for leaders 0..7 |
| 9 | cities | leaders 0..7 active, each `City` with `city_flags & 1`: `City::walk_data` inlined, strings skipped |
| 10 | items | each alive `Item`: tag (no-op), `[0x20, 0x21)`, `SubObject::walk_data` |
| 11 | goods | each alive `Good`: same pattern |
| 12 | world | if `world.cells` (World +0x134) ≠ 0: `World::walk_data(part = −1)`, else stays 1 |
| 13 | rules | `Game::walk_rules_data` |
| 14 | scenario_data | `ScenarioData::walk_data` |
| 15 | script_run_time | `RunTimeEnv::walk_data` |

Leader stride is 0x6eec (`leaders.list[0]` @e3a390). **Not in any part:** `game_random` (the RNG is never checksummed),
the Game frame/tick block, Console, Camera, TurnControl, CommandManager, Armies, Forms, Heroes, Herds, Specials, Wonders,
Forts, Docks, OilWells, Supplies, Caravans, Lands, Regions, Terrain (other than via World), Farms, GraphicEvents, Scene,
Achieve, MessageWin. The other `CheckSums::check_*` functions (`check_armies`, `check_forms`, … `check_wdata`,
`check_tdata`, `check_seen`) serve the debug save-verification (`SaveGame::verify_save` @5a76b0, `LoadGame::verify_load`
@5a39f0) and never feed CHECK_SUMS. The mapping is also in `datawalk.json` → `checksums`.

## 7. Tools

```
node re/tools/statewalk-to-json.mjs                    docs/state-walk.md → re/iface/datawalk/*.json (A,B,C,D,core,fix_verify,globals)
node re/tools/savdump.mjs --build                      merge fragments → re/iface/datawalk.json; prints coverage and problems
node re/tools/savdump.mjs --check                      validate the description only (exit code 1 on any problem)
node re/tools/savdump.mjs --doc docs/state-walk.md     readable listing (lossless, see the head of this file)
node re/tools/savdump.mjs <save.SVX>                   decode a save (gzip or plain): walk + verify block, to the last byte
node re/tools/savdump.mjs <save.SVX> --summary         the same plus a summary of the decoded state (header, world, leaders,
                                                       objects, world-grid completeness); never prints player names
node re/tools/savdump.mjs <replay> --replay            header walk; then the embedded save, or the random-game-info and rules blocks
node re/tools/savdump.mjs <file> --fn "X::walk_data" --at N [--obj global]   one function at an offset
   options: --json out.json  --trace  --max-depth N  --mode load|checksum  --version V
node re/tools/pdbtypes.mjs <Class> [--tree]            layout with base classes from rise.pdb
node re/tools/walklabel.mjs 0x4f24 | --hash "Game"     label offset → string and tag byte
```

On a fresh checkout: `node re/tools/statewalk-to-json.mjs && node re/tools/savdump.mjs --build && node re/tools/savdump.mjs --check`.

`savdump` decodes in the loader's view and verifies every tag byte; it names raw bytes with the PDB layout of the
context class. A tag mismatch or an implausible container length stops the decode and reports the byte offset and the
function, which is how a wrong step shows up. `--trace` prints every step with its stream offset; locating the failing
object in a trace and comparing the bytes with the walk is the whole debugging method used in §8.

## 8. Validation

Corpus (gitignored, personal, never committed): 10 `.SVX` saves in `re/oracle/Saves` and 95 `.rcx` replays in
`re/oracle/Recorded Games`, all from build `(Version: 00.2024.06.2000)`, `GameInfo.version` 0x06ac012c, save version 0x10, magic
`RoNSave`, `GameInfo.flags` 0x121.

| Check | Result |
|---|---|
| Description consistency (`savdump --build`, `--check`) | **285 functions** (268 distinct walk-function addresses in the symbol index, all described, 16 entries for code that exists only inlined, plus `SaveGame::verify_save`), 123 globals, 0 problems: every call and dispatch target resolves, every tag label resolves to a string and byte, every expression compiles |
| Listing round trip | `statewalk-to-json` rebuilds the 285 functions from `docs/state-walk.md` with no problem; `savdump --doc` on the result reproduces the listing byte for byte |
| **10 original saves** (`savdump <file>`) | all 10 decode to the **last byte** (table below), every section tag matching (10,268 tags in the smallest save); the walk plus `SaveGame::verify_save` consume exactly the decompressed size |
| **Saves embedded in replays** (`savdump --replay`) | all 59 replays with a non-empty `save_name` decode to the last byte; the end of every embedded save equals the position `rcxdump` finds for the command stream minus the random-game-info block (14, 19 or 29 bytes for 2, 3 or 5 entries: `5 x N + 4`); the other 36 replays give header + rules block of exactly 1,024,221 bytes |
| Verify block as an oracle | the number of entries of every save is `size + tile_size + fog_size + 73 + goods.length` (table below); the per-cell families were recomputed from the decoded stream bytes and match in every save: **all** `WData[0, 0x15)` Adler-32 (6400 or 10000 per save), **all** `tdata` u16 (102400 / 160000), **all** fog cells as `adler32(seen[i], seen2[i], seen3[i])` (25600 / 40000) |
| Terrain coordinate flags | the cell mapping derived from `TerrainOut::generate_land_lists` (see `TerrainList::walk_data`) puts CoordInfo.flags bit 2 equal to WData.flags bit 2 for every node of every save (0 mismatches in 6399 / 9999 nodes x 10 saves) |
| Replay header and rules block (sample replay) | `Game::walk_data` consumes bytes 0..1110 with tags Game/GameInfo/Player all matching; decoded fields equal `rcxdump`; `Game::walk_rules_data` consumes exactly 1,024,221 bytes (806 types, constants, balance, 24 tribes) and ends where rcxdump finds the command stream |
| Whole-save walk on synthetic streams (`--synth`, `--synth-fill`) | still runs to the end (1,015,364 bytes on the empty stream) in load and checksum modes: executability of the description |

The 10 saves, ordered by frame (all random maps: `scenario_type` 0, no script, no mod):

| Frame | World | Decompressed | Walk bytes | Verify block (entries) | Goods | Result |
|---|---|---|---|---|---|---|
| 189 | 100x100 | 4,232,994 | 3,392,360 | 840,612 (210,153) | 80 | EOF |
| 113,264 | 80x80 | 4,347,405 | 3,809,387 | 537,996 (134,499) | 26 | EOF |
| 180,904 | 80x80 | 4,835,387 | 4,297,369 | 537,996 (134,499) | 26 | EOF |
| 194,797 | 80x80 | 5,142,161 | 4,604,143 | 537,996 (134,499) | 26 | EOF |
| 209,278 | 80x80 | 5,208,116 | 4,670,098 | 537,996 (134,499) | 26 | EOF |
| 221,518 | 100x100 | 6,059,032 | 5,218,174 | 840,836 (210,209) | 136 | EOF |
| 256,648 | 80x80 | 5,025,034 | 4,487,016 | 537,996 (134,499) | 26 | EOF |
| 353,231 | 80x80 | 5,615,574 | 5,077,212 | 538,340 (134,585) | 112 | EOF |
| 375,834 | 80x80 | 5,825,003 | 5,286,641 | 538,340 (134,585) | 112 | EOF |
| 383,396 | 100x100 | 7,857,479 | 7,016,737 | 840,720 (210,180) | 107 | EOF |

(Walk bytes exclude the 22-byte file header: `String "RoNSave"` 4 + 14, `u32` version 4.)

### Correction log

The description was rebuilt from the listing (no source fragment survived), so the first decode of the real saves is also a test of
the rebuild. Four things were wrong or missing; each is visible as a tag mismatch or an implausible length at a precise offset.
All other ~280 functions decoded without change.

| # | Where | Evidence (save, offset, symptom) | Cause | Fix |
|---|---|---|---|---|
| 1 | `PtrLinkListAbstract<GatherPoint,unsigned char>::walk_data` (the `gather` list of a `Build`, `BuildData::walk_data`) | 9 of the 10 saves stopped inside `Objects::walk_data` with `tag "SubObject" mismatch ... 0x1 != 0x5e` at the first `Build` after one with a non-empty gather list (decompressed offsets 476,141 - 988,513; e.g. 953,535 in the frame-180,904 save, where the bytes were `01 bf 01 00 00 00 07 5e`: the expected tag sits 5 bytes later). Only the frame-189 save, which has no gather point yet, passed | the listing showed only the `GatherPoint` body | per node `i32 data type`, `u8 metric`, then 9 bytes `x, y, action` (decompile @4708a0: `walk_function(&dtype, ..)`, `walk_function(node+0xc, node+0xd)`, then the data) |
| 2 | `GraphicEvents::walk_data` | after fix 1 all 10 stopped at the first array after the GraphicEvents tag (`implausible SimpleArray length 1 size 0` at 1,310,989 in the frame-189 save). The loop bound `i32(0x8)` read the never-walked `events.length` as 0, so the per-slot presence bytes and the EventGroup chains were skipped | the slot count is not in the stream (`GraphicEvents::init` @8e5390 sets it to `first_ammo_piece + ammo_names.length`) | `rt("GraphicEvents.events.length")` = **60415**, found by trying every count until the `Scene` tag (0xb7, `bits` 32, `size` 4) followed the last array (frame-189 save: Scene tag at 1,383,952 = 1,310,978 + 72,974); the same value decodes all 10 saves. It is a constant of this build's game data (`RUNTIME_VALUES` in `savdump.mjs`) |
| 3 | `TerrainList::walk_data` / `Terrain::walk_coord_data` | the node count per list was an assumption of the old `savdump`; with anti-diagonal sizes all 10 saves ran 2 bytes too far (`implausible PtrLinkListAbstract count 25886720` in `MessageWin`, offset 1,459,796 in the frame-189 save) | `generate_land_lists` @85f6a0 fills list `xs-c` (c = 1..xs-1) with `xs-c` cells and list `xs+ys-1-r` (r = 0..ys-1) with `r+1`; list 0 stays empty and the last list (cell (0,0)) is not walked | count of list d = `0` for d = 0, `d` for d < xs, `xs+ys-d` otherwise: **`xs*ys - 1` u16** in total; cell order inside a list fixed (see the function note) |
| 4 | end of the file: `SaveGame::verify_save` (new function, @5a76b0) | with 1-3 fixed all 10 saves ended with `note: N bytes after the walk`, N = 537,996 (80x80), 538,340, 840,612, 840,720 or 840,836 (100x100); the bytes are high-entropy `u32` | `SaveGame::do_save` appends the debug round-trip block of `verify_save` (vtable +0x10 after the walk) | 61 named `u32` entries as listed in §5 item 19; decoded on the same decoder (`LEN("goods")`, `G("world")`); EOF is then exact in all 10 saves, and the entry count equals `4 x (size + tile_size + fog_size + 73 + goods)` |

Tooling changes made along the way (all in `re/tools/savdump.mjs`): `rt()` values, `LEN()`, `count:`/`header:`/`node:` in the
listing, `then:` chaining of `verify_save` after the walk, `pos` on `buf` items, `--summary`, and probing of the random-game-info
entry count for scenario replays (the old header-player count failed on 2 of the 95 replays).

### What the saves are

All ten are single-player-slot random-map games against AI (no scenario, no script, no mod). Setup block, values common to all ten:
team_style 1, game_rules 1, starting_resources 1, tech_cost 3, reveal_map 3, pop_limit 5, victory 2 (game_speed 2 or 3, difficulty,
elimination and ending_technology vary). World grids: 80x80 (map size 4) and 100x100 (map size 6); map styles 6, 18 (both sizes) and 1 (shown as style 14 in
`World.map` after the random pick; `sea_map` 1). The world grid of every save is **complete**: `size` WData cells (21 stored bytes
each), `tile_size = 16 x size` tdata u16, `fog_size = 4 x size` fog cells (`seen`, `seen2`, `seen3` byte arrays), a `region` on every
cell, a `Region` list of 128 slots of which only the used ones carry data, and `Region.size` equals the cell count of that region in
every used region (e.g. frame 189: land region 1 with 7573 cells, sea regions 65..73). `World.land_size` = cells with `land == 0` plus
cells with `WData.flags & 0x100` (shore shallows), verified in all ten. The frame-189 save (5 players, 12 + 12 objects each) is a freshly
generated map and therefore the cleanest oracle for the random-map generator; the other nine keep the same generated terrain (saves of one game share `World.seed` and
`GameInfo.seed`: three of the style-6 saves, two of the style-18 80x80 saves), so they validate it too.

## 9. Open questions

1. ~~Real data.~~ **Closed** (§8): 10 saves and 59 embedded saves decode to the last byte. Still untested: Conquer-the-World saves
   (`RoNCTWSave`), multiplayer saves (`RoNMultiSave`) and saves of versions below 0x10: none in the corpus.
2. ~~`Terrain::walk_coord_data` has no count.~~ **Closed** (§8 correction 3): counts and cell order follow from
   `generate_land_lists`; nodes are the cells in insertion order; the (0,0) corner is not stored.
3. **Checksum bit-exactness needs memory-identical layouts.** CHECK_SUMS hash raw struct ranges including padding and
   members that are presentation or pointers inside the walked ranges (e.g. `Guy` 0x9b bytes, `LeaderData` raw
   blocks). A reimplementation must either keep these exact layouts (and zeroed padding) as a "checksum image", or
   accept that only the per-turn command replay, not CHECK_SUMS, is the oracle. **New:** every save carries 134,499 - 210,209
   reference Adler-32 values (§5 item 19) for the engine's state at the save frame: three families (world cell, tile, fog cell, 99.9 %
   of the entries) are plain stream bytes and match; the ~60 subsystem entries (Game, Constants, Leaders, Objects, ...) are the real test
   of a checksum image and were *not* recomputed yet. The stream bytes of a raw range equal the hashed bytes whenever the range has no
   `checksum == 0` / `flags` condition around it, so a loader that keeps the loaded `[a, b)` images can be compared directly.
4. **Abstract order bases** (`AirOrder`, `GroupOrder`, `PatrolOrder`) are described with offsets as if most-derived;
   only the derived orders are ever created, so this should not matter (all order types present in the corpus decode).
5. **Conquer the World**: `WalkDataConquestGame::walk_data` (tag ConquestWalk, used for CTW campaign saves) and the CTW
   replay header are described but untested; `ConquestGame` saves `invade_win` from UI state.
6. **Load-time rebuilds** (notes in the JSON) define state that is *not* stored: unit index tables, Terrain::init
   products, per-type lists, gaia leader 9, farm animals, `Objects::reload_data`. A reimplementation that loads a save must
   recompute them identically; their exact effects were not traced.
7. **`GameInfo` older versions** (save version < 0x10) are summarised only; `LoadGame::check_version` accepts ≥ 0xf.
8. Partial-struct walks (`Color` 10 of 12 bytes, `Option` 0x12 of 0x14, `Constants` +0x804 twice, Camera blocks out of
   order) are faithful to the code; nothing suggests they are bugs to fix, but a writer must reproduce them.
9. **`GraphicEvents.events.length` = 60415** is a game-data constant found by decoding, not derived: a loader needs
   `graphic_pieces->first_ammo_piece + ammo_names.length` from the install's graphics XML (`GraphicEvents::init` @8e5390), or can keep the constant
   for this build (`GameInfo.version` 0x06ac012c). It is the only value of the walk that comes from outside the stream.
10. **The two `empty` entries of the verify block** (between Mountains and Constants, and after GraphicEvents) are 1 (Adler-32 of no
    bytes) in every save: the decompile shows no walk there (the walk was compiled out of this build or dropped by the decompiler).
    `walls` is 1 in all ten saves (no walls), `ammo` in the two saves without ammo in flight, and `world` equals the later
    `check_world` entry in every save.
11. **Region-grid and tile mapping** of `World` (`reg_xs`, `tile_xs` = 4 x `xs`, `fog_xs` = 2 x `xs`) is stored as plain scalars; the
    meaning of each `tdata` u16 (a 16-bit terrain mask per tile) and of `WData.land_sub` is not spec'd here (docs/terrain-spec.md).

## Evidence

| Function | Address | Used for |
|---|---|---|
| `DataWalk` (vtable: `walk_function`, `walk_test`; fields `input`, `checksum`, `flags`) | `rise.pdb`, dtor @43c450 | §1 |
| `SaveGame::walk_function / walk_test` | 43d730 / 43d840 | write semantics, tag byte = `hash_value_insensitive & 0xff` |
| `LoadGame::LoadGame / walk_function / walk_test` | 43d8a0 / 43d950 / 43da60 | load semantics, tag check |
| `CheckSum::CheckSum / walk_function / walk_test` | 45dec0 / 936ff0 / 41bfe0 | checksum walker (input 0, checksum 1, flags −1) |
| `RecordGame::write_header` (inlined SaveGame ctor: input 0, checksum 0, flags −1) | 952a50 | SaveGame field values |
| `String::generate_hash`, `primes` | a1b6b0, b14500 | tag hash |
| `String::walk_data / write / read` | a1b2d0 / a1c1e0 / a1c290 | strings in walks and in the file header |
| `SaveGame::save_game / do_save`, `LoadGame::load_game / do_load / check_version / get_header` | 5a8220 / 5a81f0, 5a72f0 / 5a6e30 / 5a7150 / 5a6fb0 | save container (§4) |
| `WalkDataGame::walk_data` | 5a2360 | top-level order (§5) |
| `Array<T> / ObjectArray<T> / SimpleArray<T> / PtrArray<T> / MultiPtrArray<Object> / Stack<PathData>::walk_data` | e.g. 47ea30, 481190, 473120, 46df30, 45d550, 46d8b0 | containers (§2) |
| `ObjectsArray::get_data_type / get_new_data` | 475640 / 6450d0 | object type dispatch |
| `Unit::walk_data`, `Unit / Object / SubObject::must_walk`, `Object::walk_data`, `SubObject::walk_data` | 60cf40, 60d040 / 647930 / 6623a0, 647830, 6621d0 | object walks; flag groups 2/4/8 |
| `CommandManager::issue_check_sums`, `CheckSums::check_all / check_units / check_builds / check_walls / check_ammo / check_deaths / check_groups / check_guys / check_leaders / check_cities / check_items / check_goods / check_world` | 940770, 936560 / 9371d0 / 937290 / 937360 / 9374e0 / 936bb0 / 937530 / 937430 / 9375a0 / 937600 / 937790 / 937710 / 9375d0 | §6 |
| every `*::walk_data` / `walk_rules_data` / `walk_*` | addresses in `re/iface/datawalk.json` | per-class walks |

Decompiled sources: `re/decomp/state/` (all walk functions, typed DataWalk vtable; job `re/jobs/st_dc_all.txt`),
`re/decomp/state_core/` (disassembly of `WalkDataGame::walk_data`, `ObjectsArray::get_*`, `CheckSums::check_*`),
`re/decomp/state_A..D/` (helpers decompiled while translating).
