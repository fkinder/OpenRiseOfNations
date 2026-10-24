# Save format (native, version 7)

The engine's own save format for its simulation state: `engine/sim/save.hpp` / `save.cpp` (API and the section walks),
`save_archive.hpp` (the writer / reader), `save_walk.inc` (the member lists, generated). It is **not** the original game's
`.sav` layout: that is a later phase, once real original save files exist to validate against. The design mirrors the
original's single serializer (`docs/state-spec.md` section 1): **one `walk(ar, T&)` per state struct, driven by a `Writer`
and a `Reader`**, so a member can never be written without being read.

The contract: a game saved at frame F and continued M frames gives, frame by frame, exactly the digests of the loaded save
continued the same M frames (and the same full saved state at the end). `ron-sim --roundtrip` checks it.

## 1. How a save loads

A save records the scenario path (relative to the game folder, `/` separators; absolute for a scenario outside the install)
and the `Game::load` seed. `load_game` then:

1. reads the header, `META` and `INFO` (the `GameInfo` the original run had, including options set before loading such as the
   reveal map and difficulty), checks the format version and a hash of the loaded rules tables;
2. runs the normal `Game::load(files, scenario, rules, seed)`, which rebuilds everything derived from the install's files
   (rules, art, terrain heights, regions, mountains, the world grids as a scenario starts, objects, leaders ...), and
   `script_host_load` when the save has scripts (compiles the same `.bhs` files);
3. walks every other section with the `Reader`, **overwriting** the state `Game::load` built. Pointers into the rules
   (`Object::ptype`) are re-derived; the pathfinder's block-list pointer is re-pointed.

**Random games.** When the scenario slot of `META` starts with `random:` it holds the setup text of a random-map game
(`random_setup_string`, sim/newgame.hpp) and the seed slot its seed; step 2 then runs `new_random_game` (the generator, the
objects, the scripts) instead of `Game::load`, and step 3 overwrites the state as usual. No `Game` member was added, so the
layout and the version are unchanged; `scenario_rel_path` passes such a string through.

API (`sim/save.hpp`): `save_game(game, scenario_rel, seed, path)`, `save_game_bytes`, `read_save_info`,
`load_game(files, rules, anims, path, &info)` / `load_game_bytes`, `scenario_rel_path(files, file)`,
`describe_save_difference(bytes, bytes)` and `describe_game_difference(game, game, ...)` (the latter names the member where
two games first differ). Errors throw `std::runtime_error` with the section: `save: section 'WRLD': ...`.
`rules` is the loader's rules (`load_rules` + `start_game(GameSetup())`) and, like `files`, must outlive the returned `Game`.

## 2. File layout

All integers little-endian (the build refuses a big-endian host), floats as their bits, `bool` as one byte (0 / 1 checked),
strings and containers as a `u32` count followed by the elements (`std::u16string` as UTF-16 code units). Structs are walked
member by member in declaration order, so there is no padding in the file and equal states give byte-identical files.

```
"RONS" | u32 version (7) | u32 flags (0) | section*
section = tag (4 ASCII bytes) | u64 payload length | u32 CRC-32 of the payload | payload
```

A reader opens each section by its tag (they come in the order below), checks the CRC, walks the payload with a bounded
reader and **requires that every payload byte was consumed**. A corrupt section (`checksum mismatch`), a truncated file,
a section whose layout differs from this build's (`N bytes were not read`, or running past its end) and a wrong tag are all
reported with the section name. The version is bumped on any layout change; there is no migration (a save loads only into
the build that wrote it, and only against the same rules tables: the hash in `META` is checked).

Version 7: `Build::gpiece` (WallData +0x58, the graphics piece of the building's art: `Wall::update_gpiece` / `GraphicPieces::get_build_gpiece`, part of the `builds` checksum; sim-implementation.md fidelity log, eighth pass, item 36).

Version 6: `Leader::flock_stamp` (LeaderData +0x7b8, the frame of the leader's last bird flock; `Objects::add_flock` allows one per 225 frames, docs/aircraft-spec.md 15). The debris of crashed
aircraft is ordinary `Ammo` state (`gpiece = -2`, `bank_dx` / `bank_dy` were already walked).

Version 3 (aircraft, docs/aircraft-spec.md): `Order` gains the air-order fields (cruising altitude, sharp turn, returning, the
last target position, the patrol waypoints and index, the SPECIAL_ANIM fields), `Guy` the bank / pitch floats and `Object` the
`launch_timer` and `launch_list` of airbases, carriers and silos.

A path ending in `.gz` is written gzipped (a save compresses to about 2 % of its size, 40 KB for a Tutorial 2 game); the loader
reads gzip and plain files alike (`gzopen` on a plain file). Writing goes to `<path>.tmp` and is renamed over the target.

## 3. Sections

| Tag | Content |
|---|---|
| `META` | scenario path, seed, frame, tick, has-scripts, has-own-rules, rules hash |
| `INFO` | `Game::info` (`GameInfo`: reveal map, teams, victory settings, tech span, AI settings ...) |
| `RULE` | the rules tables when a scenario script rewrote them (`Game::own_rules`, see 5) |
| `CORE` | `rng` (the whole generator: its 32-bit state), `frame`, `tick`, `seed`, `total_units`, `everyone_mask`, `GameDaemon` (`busy`, `repaths`), `Market`, `good_mark`, `world_total_land`, the budget / counter members (`queue_fail`, `check_borders_budget`, `armageddon`, `musical_chairs`), the mode flags (`editor`, `scripted_rules`, `ctw`, `multiplayer`, `field_battle`, `ai_speed`, `ai_off`, `scenario_mode`, `building_*_bonus`, `buildings_gather`), game over / victory-checked |
| `WRLD` | `World`: dimensions (checked against the loaded scenario), `sea_map`, `wdata`, `tdata`, `seen` / `seen2` / `seen3` / `wcoord_seen` (fog), `danger`, the collision block pool and its free list, `empty_colls` |
| `REGN` | `Regions`: every `Region` member except the coordinate list, `sea`, `land` |
| `LEAD` | the 10 `Leader`s (all ~270 members, the hero / special records included: economy, production, census, AI personality / sites / make list, diplomacy, victory clocks, stamps ...) |
| `OBJS` | per leader: `unit_mark`, `build_mark`, `obj_ctr`, then every `unique_ptr<Unit>` and `unique_ptr<Build>` slot **including dead and empty ones** (a presence byte, the `ptype` index, the object; a unit also its time-sliced search) |
| `GOOD` | `goods`, `items` (`GoodObj`) |
| `HGHT` | `heights.h` (the terrain height grid: `TerrainOut::terraform_for_building` flattens it under every new building, sim-implementation.md fidelity log item 32; version 5) |
| `CITY` | `cities`, `forts`, `wonder_records`, `unbuilt_*`, `wonders`, `terra_cotta_total`, `world_pop`, `world_cities`, `starting`, `supplies`, `caravans`, `farms`, `hold_doobers` |
| `GRUP` | the 512 `Group`s, `last_group`, `proc_group`, the global `forms`, `grouppath`, `group_cols` |
| `PATH` | `PathFinder`: tunables, request state and the pathfinder's own search trees (the valid-answer cache and block snapshot a returning `find_upath` leaves behind are behaviour) |
| `COMB` | `ammo` pool, `ammo_counter`, the 100 `deaths` (corpses), `range_swap_type` / `range_swap_value` |
| `AIST` | `armies` (8 x 16 `Army`), `armies_find_dist`, `find_dist`, `find_who` |
| `SCEN` | `ScenarioData` (goals, scenario groups, kill tallies, overrides ...), `scen_ally_mask`, `scen_diplomacy_setting`, `reveal_points`, `attrition_free_points` |
| `SCRP` | (only when the game has a `ScriptHost`) the BigHugeScript run time and host, see 6 |
| `END!` | empty |

## 4. Derived state that is not saved

Everything here is rebuilt by `Game::load` / the loaders from the install's files and is never written afterwards (each
claim was checked by grepping the writers):

| State | Why it is skipped |
|---|---|
| `Game::rules` / `base_rules` | the loader's rules tables (hash-checked); only the script-rewritten delta is saved (`RULE`) |
| `Game::anims` | the art source the app passes in |
| `Game::heights` | the terrain height grid copied from the scenario at load (`game.cpp`); no writer afterwards (farm terraforming is a TODO that leaves it alone) |
| `Game::mountains` (`PlacedRange`) | built from the scenario's mountain stamps at load; read only (`mountains_find_nearest`, `mountain_gather_size`) |
| `Region::coords` | the raster coordinate lists, rebuilt by `Regions::find_all` at load; regions only change at map creation. The mutable scalars (`borders`, `flags`, `size`, `rank`, `coastal` ...) **are** saved |
| `Game::fire_proj` | per unit type, decided from the art by `init_combat_data` at load; its only run-time write (`combat_fight.cpp`, the melee-and-ranged swap) restores the value inside the same call |
| `World` size members (`xs`, `ys`, `size`, `tile_*`, `fog_*`, `reg_*`) | derived from the scenario; saved only to verify they match |
| `World::fog_changes` | a renderer change counter; bumped after a load so renderers refresh |
| `Game::events` | the tool log (`GameEvent`), never read by the simulation |
| `Game::local_play` | the seat-local console leader, chosen by `Game::load` |
| `Game::scenario_loading` | a flag that is only set inside `Game::load` |
| `PathFinder::blocklist` | always `&lists->blocks` (`own_lists`), re-derived |
| `ScriptHost::game`, `files`, `rt.files[*].code` and the other compiled script data, `rt.natives`, the builtin registry | rebuilt by `script_host_load` from the `.bhs` files |
| `ScriptHost` statistics (`runs`, `run_errors`, `run_failures`, `messages`, `todo_calls`, `verbose`), `rt.return_value`, `bytecodes_executed`, `err_count`, the VM's frame and operand stacks | tool counters and per-run scratch (reset at the start of every `run_script`) |
| `ScriptInput` (all but the selection and armed command) | renderer-filled between frames: screen, interface rectangles, camera, keys, callbacks |

Not part of `Game` and therefore not saved: the `CommandManager` / `CommandPackage` of the app (a load starts a fresh one, as
a fresh session does; every `issue_*` re-sends its group) and the renderer.

## 5. Rules rewritten by scripts

Scenario builtins such as `set_type_build_time` copy the rules into `Game::own_rules` (`Game::mut_rules`). A save then
holds, per vector of the `Rules` (`types`, `tribes`, `categories`, `lands`) the **elements that differ** from the loaded
tables (by comparing their serialized bytes) and, for `constants` and `balance`, a changed flag and the whole value. Loading
copies the loader's tables, applies the differences and repoints `Game::rules` at the copy. Tutorial 2, which rewrites the
tables, gives a 154 KB `RULE` section.

`Object::ptype` is stored as the index into the *current* tables (bit 30 set: into the loaded base tables, which a closed
object can still point at after the rewrite), so a pointer is restored to the table it pointed into.

## 6. Scripts

`SCRP` holds, in this order: the scenario and general-powers script names, the timers, `find_counters`, the whole
`ScriptPresentation` (the pending event queue with its sequence counter, objectives, arrows, flags, highlight, camera
request, locks, `unit_scale` ...), `scenario_dir`, the scenario sound, `ctw_ints` / `ctw_strs`, the XML-profile cursor and
documents, the saved part of `ScriptInput`, and the VM: for every compiled file (name, script count and constant pool size are
checked against the freshly compiled files) the constant pool, and per script the trigger bits and the **static variables**
(created by a script's first run, so their number is the file's, not the load's). Script values (`bhs::Value`) are written as
a graph with identity: the first sight of a value writes it, later sights write its id, so shared and cyclic structure comes
back as it was. The XML documents (`open_file` ...) are written as a node table with ids for the same reason (the
current / parent / root / element pointers share nodes with the documents).

A renderer consuming the presentation queue keeps its own cursor on `ScriptEvent::seq`; after loading it should skip to
`next_seq - 1` (ronmap does, `View::reset`).

## 7. The pathfinder

The original's global A* keeps its trees between calls (a `find_upath` that returns early leaves the valid-answer cache and
the block snapshot for the next search) and a unit's time-sliced search lives in `Unit::openlist`. Both are state:
leaving them out makes loaded games diverge (the round trip fails in 6 of the 156 stock scenarios without them). A search
is written as its node arena (`PathNode` fields and a parent index) followed by the open tree (key, node index), `refs`,
`closed` (metric, node index), the valid cache, the block snapshot and the sequence counter; node pointers become indices
into the arena and a pointer outside the arena is an error. The definitions moved to `sim/pathfinder_search.hpp` for this.

## 8. Tools

`ron-sim` (`apps/sim/main.cpp`):

- `--save-at FRAME:path` (repeatable): writes the state at the *start* of FRAME, before that frame's scripted commands.
- `--load path`: continues a save. The scenario argument may be left out (`ron-sim <game dir> --load path`); the seed comes
  from the save. `--frames N` runs N more frames from the saved frame; scripted commands still apply by absolute frame.
- `--roundtrip FRAME`: the self-check, printed after the determinism self-check. The run saves at FRAME to
  `ron-roundtrip-<pid>.ronsave` in the current directory and continues recording digests; the file is then loaded (its
  re-save must equal the file byte for byte), the same remaining frames are run, and the digests are compared one by one,
  followed by the whole saved state of the last frame (`describe_game_difference` names the member of a difference).
  It prints `save round-trip: OK (...)` or the first differing frame. A game that stops early (game over) never reaches FRAME:
  the check then uses the frame before its end.

`ronmap` (`apps/map-mac`): **F5** quick-saves and **F9** quick-loads one slot,
`~/Library/Application Support/OpenRiseOfNations/Saves/quicksave.ronsave`; the game menu (Esc / F10) has **Save Game** and
**Load Game** for the same slot (the front end has no text entry, so there are no named saves yet). A save loads only into
the scenario it was made in (the terrain, trees and art the renderer built stay); a load replaces the simulation, resets the
selection, the command manager and the script view, and says "Game loaded" in the message window. Headless:
`--load-game FILE` before the first frame and `--save-game FILE` after `--frames`. One value differs between a continued
session and a loaded one: `real_time_millis()` reports the renderer's clock, which restarts with the session; a script that
stores it in a variable carries that difference (checked: two bytes of one static variable of Tutorial 2).

## 9. Adding state

When a struct gains a member the build stops at a `static_assert(sizeof(...))` (the generated walks end with one per struct,
`save.cpp` has them for the hand-walked aggregates and for `Game` itself). Then either regenerate `save_walk.inc` with the
script below (it lists the non-static members of each struct from clang's AST dump, in declaration order, so nothing is
typed by hand) or, for a new `Game` / `ScriptHost` member, add it to `walk_core` / `walk_body` / `walk_scripts` in
`save.cpp`, update the size in the assertion, and bump `kSaveVersion`. If the member is rebuilt by `Game::load` and never
written afterwards, say so in section 4.

```python
#!/usr/bin/env python3
# Regenerates engine/sim/save_walk.inc from the struct declarations (see docs/save-format.md, "Adding state").
# Usage: python3 regen.py <repo root> > engine/sim/save_walk.inc      (needs clang++ and the repo's headers)
import json, os, re, subprocess, sys, tempfile

ROOT = sys.argv[1]
STRUCTS = ["Leader", "Leader::Deal", "Personality", "Site", "MakeObject", "City", "Fort", "Wonder", "Unbuilt", "GameInfo",
           "GameDaemon", "Market", "FarmRec", "CaravanRec", "GoodObj", "Game::Supply", "Game::RevealPoint", "Army", "Ammo",
           "DeathObj", "Group", "FormData", "PathData", "Order", "Guy", "Object", "Unit", "Build", "BuildQueue", "QueueItem",
           "GatherPoint", "WData", "CollBlock", "Region", "ScenarioData", "ScenarioData::Goal", "ScenarioData::Group", "Type",
           "U16Array", "TexInfo", "BuildMask", "DooberMask", "Tribe", "Category", "CategoryList", "Constants", "PathFinder",
           "SnapBlock", "ScriptEvent", "ScriptObjective", "ScriptArrow", "CameraRequest", "ScriptPresentation",
           "ScriptTimers::Timer", "ScriptHost::Sound", "LandMake"]
SKIP = {("Object", "ptype"), ("Region", "coords"), ("PathFinder", "lists"), ("PathFinder", "blocklist"), ("Unit", "openlist")}
SRC = '#include "sim/game.hpp"\n#include "sim/script_host.hpp"\n#include "sim/pathfinder_search.hpp"\n#include <cstdio>\n'
tmp = tempfile.mkdtemp()
src = os.path.join(tmp, "t.cpp")
fields, bases = {}, {}
for name in STRUCTS:
    open(src, "w").write(SRC)
    out = subprocess.run(["clang++", "-std=c++17", "-I", ROOT + "/engine", "-fsyntax-only", "-Xclang", "-ast-dump",
                          "-Xclang", "-ast-dump-filter=ron::sim::" + name, src], capture_output=True, text=True).stdout
    parts = re.split(r"^Dumping (.*?):$", out, flags=re.M)
    for i in range(1, len(parts), 2):
        if parts[i] != "ron::sim::" + name:
            continue
        body = parts[i + 1].splitlines()
        f = [m.group(1) for l in body for m in [re.match(r"^[|`]-FieldDecl \S+ <[^>]*> (?:col|line):\S+ (?:implicit |referenced |used )*(\w+) '", l)] if m]
        b = [m.group(1) for l in body[:12] for m in [re.match(r"^\s*[|`]?-?(?:public|private|protected) '(.*?)'", l)] if m]
        if name not in fields or len(f) > len(fields[name]):
            fields[name], bases[name] = f, b
    if name not in fields:
        sys.exit("struct not found: " + name)
print("""// GENERATED by regen.py (docs/save-format.md) from the struct declarations: every non-static data member in declaration
// order, one walk() per state struct. The same function is the writer and the reader (sim/save_archive.hpp). Included by
// save.cpp inside namespace ron::sim::save. Members left out on purpose: Object::ptype (re-derived from the type index),
// Region::coords (rebuilt by Game::load), PathFinder::lists / blocklist and Unit::openlist (saved by hand in save.cpp).
// When a struct gains a member the sizeof assertion at the end of this file fails: add the member (rerun regen.py).
""")
for name in STRUCTS:
    mine = [f for f in fields[name] if (name.split("::")[-1], f) not in SKIP]
    print("template <class Ar> void walk(Ar& ar, %s& v) {" % name)
    for b in bases[name]:
        print("\twalk(ar, static_cast<%s&>(v));" % b)
    chunk = []
    for f in mine + [None]:
        if f is not None:
            chunk.append(f)
        if chunk and (f is None or len(", ".join(chunk)) > 90):
            print("\tFIELDS(ar, " + ", ".join("v." + c for c in chunk) + ");")
            chunk = []
    print("}\n")
# sizeof drift guard
prog = SRC + "using namespace ron::sim;\nint main() {\n" + "".join('\tstd::printf("%s %%zu\\n", sizeof(%s));\n' % (n, n) for n in STRUCTS) + "}\n"
open(src, "w").write(prog)
exe = os.path.join(tmp, "sz")
subprocess.run(["clang++", "-std=c++17", "-I", ROOT + "/engine", src, "-o", exe], check=True)
sizes = subprocess.run([exe], capture_output=True, text=True).stdout.splitlines()
print("// Drift guard: the sizes these walks were written for (LP64, libc++). A member added to a struct changes its size and\n"
      "// stops the build here; add the member to the walk above (and to docs/save-format.md if it is a new kind of state).\n"
      "#if defined(__LP64__) && defined(_LIBCPP_VERSION)")
for line in sizes:
    n, sz = line.split()
    print('static_assert(sizeof(%s) == %s, "%s changed: update its walk() in save_walk.inc");' % (n, sz, n))
print("#endif")
```

Usage: `python3 regen.py "$PWD" > engine/sim/save_walk.inc` (save it as `regen.py` first). The members it skips on purpose are
listed in its `SKIP` table; `PathNode` is walked by hand.
