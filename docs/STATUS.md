# Project status and how to resume

Last updated 2026-10-23 (healing / attrition, leader stat bonuses, garrisons; see docs/roadmap.md for the phases). Read this first when picking the work back up; then `CLAUDE.md`, `docs/systems-map.md`, and
the spec for whatever you touch.

## Where things stand

**Phase 1: draw the battlefield. Done.** `ronmap` renders any shipped scenario natively in Metal with the original
look: terrain + transitions, sea (water lattice, ocean floor), rivers, mountains + snowcaps, doobers, forests, every
building/unit/farm/good from the scenario (nation style, age, team colour, facing), original render states, lighting,
HDR tonemap and bloom. Unit selection (click, drag box, Shift, Esc) with rings. Port decisions that deviate from the
decoded code are listed at the end of `docs/water-spec.md` (floor drawn before land; no off-map floor ring; ocean
coral overlays off unless `RON_CORAL=1`). Selection circles and health bars are drawn after tone mapping, as the original does.

**Phase 2: simulation foundations. Done.**
- Rules: `engine/sim/rules*` (C++), byte-identical to the sample replay's rules block (`ron-rules`).
- World grid: `engine/sim/world*`, `regions`, `mountains`, `placement`, `passability`, `random.hpp`; `ron-world`
  matches `re/tools/worlddump.mjs` on all 156 stock scenarios.
- Specs decoded: replay format (`replay-spec.md`, `re/tools/rcxdump.mjs`), full state model / save format
  (`state-spec.md`, `state-walk.md`, `re/iface/datawalk.json`, `re/tools/savdump.mjs`), frame loop
  (`frame-spec.md`), orders (`orders-spec.md`), movement + pathfinding (`movement-spec.md`, `re/tools/astar-ref.mjs`).

**Phase 3: simulation. In progress.** `engine/sim/` has the frame loop, objects/units with the original slot
allocation, guys with real animation lengths and graphics pieces from the install (`anim-spec.md`), the order machine,
PathFinder at 3 levels, per-frame movement, collision, formations and full group moves (`forms.cpp`, `groups.cpp`,
`search.cpp`: `find_nearby_spot`), diplomacy from the scenario, and the command layer (GROUP, MOVE_TO, HALT, FORM,
STANCE). `build/ron-sim` is the headless harness (deterministic digests). **`ronmap` runs the simulation**: scenario
units are drawn from the sim's view (interpolated, per-guy graphic and animation clock), click/box select, right-click
moves the selection (P pauses); `--move WHO:o..@X,Y --frames N --snapshot` for headless checks. Details and stubs:
`docs/sim-implementation.md`.

## Next steps, in order

Ported since the last checkpoint: fog of war (`fog-spec.md`, `engine/sim/fog.*`, drawn in `ronmap`), combat steps
1-6 (`combat-spec.md`, `engine/sim/combat_*.cpp`: damage matching `combat-ref.mjs` on all 169,312 pairs, death and
corpses, acquisition and auto-attack, attack orders, melee, projectiles from animation release frames, building
arrows; `ronmap`: right-click attack, health bars, corpses, projectiles), market drift (economy-spec 5.2).
Production (production-spec §13 steps 1-11): leader production state, `gain_tech`, prices and times (byte-identical
to `production-ref.mjs --examples` via `ron-prod-check`), placement and BUILD, construction, cities, territory
(`compute_reg_territory` decoded and ported), training queues with rally points, research, repair, DISBAND; scenario
objects created in file order with their queues and stockpiles; `ron-sim --build/--queue/--rally/...`.
HUD (`ui-port.md`): interface readers (`ron-ui-check`), the Metal UI layer with CoreText fonts, and in `ronmap` the
bar, resource panel (leader_view), selection panel with queues, command card (build menu, placement preview + BUILD,
train / research, rally), minimap, cursors, selection / control-group / camera input.
Script UI and audio (`ui-port.md` sections 8-9, 2026-10-13): `ronmap` fills `script_input` from the HUD every frame and
plays `script_presentation` (objectives with check boxes, messages, chat, feedback / scenario message plates, bubbles,
notices, arrows, the card highlight, pings with the announce ring, scenario flags, camera pans / jumps / zooms, locks,
ui_hide, popups in a nested loop, goal dialog, game-over notice); AVFoundation voice / effects / JukeBox music by mood
(`platform/audio`). tut1 and tut2 play through their first objectives from HUD-equivalent harness input
(`--select / --option / --look / --zoom-at ...@FRAME`).
Front end (`ui-port.md` section 10, 2026-10-13): the app opens in its game window on the Main Menu drawn with the
install's TopMenu art (animated backdrops, ribbon, bronze frame, buttons, fonts, menu music) and the project's
menu-logo.png; Tutorials / Skirmish / Scenarios lists, AI difficulty, Quit popup; Play and Return to Main Menu (Esc /
F10 game menu, game over) swap the window's contents in place; `ronmap --menu SCREEN --snapshot`.
Economy (economy-spec §12 steps 1-13): starting stockpiles, the income pipeline (Leader::gather, calc_gather, caps,
do_gather; `ron-econ-check` byte-identical to `gather-ref.mjs --examples`), gather rates, gatherer lists, the GATHER /
CITY_GATHER orders and handlers with every game_random draw, auto-gather, farms (growth, pens, animals), oil wells,
BUY / SELL / TRIBUTE / ACCEPT / REJECT, fishermen, merchants and caravans (roads TODO); economy fields in LeaderView
(rates, caps, prices), UnitView and BuildView; `ron-sim --gather/--buy/--sell/--econ-every/...`.

1. **Combat leftovers**: heroes / generals (every hero term in the stats and healing
   waits on `has_general` / `find_hero`), packing, aircraft. Ported 2026-10-23: healing, regeneration, supply and
   attrition (combat-spec §7), the leader stat bonuses (combat-spec §5.1, `engine/sim/stats.cpp`) and garrisons
   (docs/garrison-spec.md, `engine/sim/garrison.cpp`; transports and aircraft still TODO). The phase
   plan is `docs/roadmap.md`.
2. **Production leftovers** (sim-implementation "Production"): garrisons, aircraft, spells, capture, AI paths,
   terraforming.
3. **Economy leftovers** (sim-implementation "Economy"): caravan roads (PathFinder::find_road), packers (merchants
   unpack), diplomacy branches of tribute deals, the AI economy (strategy_all), Build::do_bonus.
4. **HUD / UI** leftovers (`ui-port.md` §6): tooltips, messages, library / mini library, market, streamlined mode,
   minimap territory and pings, the key map, market prices (income rates and the commerce cap are wired).
5. **AI and scenarios**: BigHugeScript is ported (`engine/script`: compiler matching `bhs-parse.mjs` on all 365
   scripts, VM, the 873-entry builtin table; `engine/sim/script_host.cpp`: scenario / general powers / AI production
   scripts run in `Game::do_frame` and `plan_strategy`; 472 of 813 builtin names on the sim incl. the scenario
   builtins, a presentation queue / seat-local input view for the renderer in `sim/script_ui.hpp`; sim-implementation
   "Scripts"). The computer opponent (ai-spec 15 steps 1-11: personality draws, census, sites, production states,
   make list, creators incl. navy / air, placement, city founding, armies with find_target and the state machine,
   the danger map, per-unit AI (think_scout, goody boxes), diplomacy D1-D9 and compute_score) is ported
   (`engine/sim/ai_*.cpp`, sim-implementation "Computer opponent"; `ron-sim --ai-every K --difficulty N`): AI armies
   muster, form up and assault enemy cities. City capture, elimination and victory are ported (`capture-spec.md`,
   `engine/sim/capture.cpp`, sim-implementation "Capture and victory": the strength scan, ownership transfer, plunder,
   the capital clock, defeat, the game-over test and the victory clocks). Next: transports, spells, heroes, garrisons,
   script save / load, CTW. (The renderer side of the scripts and audio are done: ui-port.md 8-9; left there: unit
   acknowledgement sounds, the profile volumes, the rare-goods / mini-library offsets of MessageWin.)
6. **Saves**: port `re/tools/savdump.mjs` to C++ (load a save into the sim and render it).

## Wanted: recorded games

No saves or replays from the original game are in the project yet (only a 6-frame sample replay used locally). Single-
player replays (skirmish and scenario), saves and especially multiplayer replays from the Windows game are the oracle for
bit-exactness (`.rcx` files carry checksums every turn in multiplayer; see `replay-spec.md`). Contributions welcome.

## How work was organised

Reverse engineering ran as background agents, each producing a spec in `docs/` (evidence cites functions/addresses,
paraphrased, no decompiled code committed) plus a JS reference tool in `re/tools/`; C++ ports were then checked
byte-for-byte against those references (`ron-check`, `ron-rules`, `ron-world`, `world-compare.sh`). Rendering was
checked by reading `--snapshot` PNGs. Ghidra runs go through `re/tools/ghidra-run.sh` (serialised by a lock).
