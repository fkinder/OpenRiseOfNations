# Nuclear explosions (Nuke, TerrainOut::terraform_for_nuke, the Armageddon counter, the nuke embargo)

Decompiled from `riseofnations.exe` (2026-10-25; `re/decomp/nukes/`, `re/decomp/victory/Nuke_*`, jobs `re/jobs/nk_*.txt`). Ported in
`engine/sim/nukes.cpp` / `nukes.hpp`. Everything not marked ❓ was read from the disassembly, not guessed.

## 1. Where a nuke comes from

A nuclear missile (type 315 NUCLEARMISSILE, and the whole `is(0x13b, 0)` line: 316 ICBM) leaves its silo (`Build::do_missile_launch` @622670,
aircraft-spec 8), flies as a plane (`do_air_physics`), and when it is in range fires its warhead: `Object::fire_ammo` creates an `Ammo` whose
`Ammo::init` (@67bbf0) gives it the target of the AIR_ATTACK_GROUND order (`r = 0` for the nuclear line: no scatter; `2 * r` for the other
missiles) and the missile dies on the spot (aircraft-spec 5). The ammo flies (`Ammo::inc_time` @67d380) and, on arrival, `Ammo::do_damage`
@678060 runs its **splash branch**, whose first test is "the shooter has OBJMASK_MISSILE" (the part ported here, `nuke_missile_impact`):

1. **Missile defence.** The territory owner at the impact W-cell (`WData.who`, +0xf). If the cell is ocean (`WorldData::is_ocean`), the first
   land cell with an owner among the spiral cells 1..119 (`move_x/move_y`, the 120-cell search) gives the owner instead (a land cell without
   owner is skipped, no owner found = none). If that owner has the tech **MISSILE_DEFENSE_BONUS** (693 = 0x2b5) and is not the shooter: the
   console messages (`loc_str` +0x690 to the shooter's console seat, +0x6a4 to the defender's) and `S_MISSILE_SHIELD`, then `Ammo::close`.
   **Nobody is hurt.** Applies to cruise missiles and nukes alike.
2. **A missile that is not of the nuclear line**: `S_NUKE_HIT` sound, `LeaderData +0x7c0` (`missiles_used`) `+= 1`, and the ordinary splash goes on
   (the rest of `Ammo::do_damage`, combat-spec 4.11: radius `min(splash_area / 4 + 1, 10)` wcoords, units with `splash_percent`, ...).
3. **The nuclear line** (`is(0x13b, 0)`): the achievement `ACHIEVEEVENT_NUKE_EXPLOSION` (not ported), `S_ATTACK` sound on the missile, `LeaderData +0x7b4
   := game.frame` (the **nuke stamp**, section 6), `+0x7bc` (`nukes_used`) `+= 1`, then

   ```
   Nuke::add_nuke(ex, ey, 100, splash_area * 192, who, o)         // @92ba30, section 3; splash_area from the missile's TYPE (+0x200)
   reveal: for each tile of circle_radius[clamp(splash_area + 1, 0, 64)] around the impact tile (tcoord = x / 192):
           in the map -> World::set_seen2(fog cell = tile >> 1, mask 0xff, explored_only 0)   // everybody sees the ground
   MessageWin::add_event(ex, ey, ...)                              // the "nuclear explosion" message (presentation)
   Nuke::do_damage()                                               // @92bc80, section 4: the first call, same frame
   Ammo::close
   ```

   `splash_area` of the shipped NUCLEARMISSILE is 10, so the final radius is 1920 world units (10 tiles).
   The ICBM's own value is read the same way.

The `ConsoleWin::run_cmd` developer command also reaches `add_nuke` and `do_damage` (not ported).

## 2. The state (`nuke_effect`, global `NukeOut`, 0x118 bytes at @c0a780)

`Nuke::add_nuke` pushes one element onto each of these parallel arrays (`Nuke::clear` @92c730 empties them all, `Nuke::init` @92c960 sets the
constants and is called when a game starts):

| offset | what | in the port |
|---|---|---|
| +0x44 / +0x50 | `TexArray` of (x, y) floats (the explosion centre; **int to float**, `cvtdq2ps`) | `NukeBlast::x, y` |
| +0x60 / +0x6c | `TexArray` of (r0, r1) floats: the radius at the start (100) and at the end (`splash_area * 192`) | `r0, r1` |
| +0x7c / +0x88 | start frame (`game.frame` when added) | `frame` |
| +0x98 / +0xa4 | shooter leader (`who`) | `who` |
| +0xb4 / +0xc0 | shooter object (`o`, the missile unit) | `o` |
| +0xd0 / +0xdc | per explosion a `SimpleArray<int>` of the objects already hurt (their `o`) | `hit_o` |
| +0xec / +0xf8 | ... and of their leaders | `hit_who` |
| +0x104 | delay before the fireball grows = **10** frames (`Nuke::init`) | `kDelay` |
| +0x108 | growth time = **30** frames (`Nuke::init`) | `kGrow` |
| +0x10c | 7 (`Nuke::init`: `nuke_effect._268_4_`, only the drawing reads it) | - |
| +0x110 | `max_nuke_frames` = **110** (`NukeOut::init` @92c9a0, `0x6e`) | `kMaxNukeFrames` |
| +0x114 | initialised flag | - |

The terraform dedupe list is `terrain +0x4bd4` (`ArrayBase<int>`, section 5). The per-leader stamp is `LeaderData +0x7b4`. None of it was saved by
the original (a nuke in flight is lost on a load); the native save keeps all of it (section `NUKE`, docs/save-format.md, version 10).

## 3. `Nuke::add_nuke(x, y, r0, r1, who, o)` @92ba30

Arguments on the stack (`ebp+8 ... ebp+0x1c`): x, y, r0, r1, who, o. In order:
1. `SoundGlobal::random` pick of one of the `sound_global.lookups[0x50]` explosion sounds, played at (x, y) (**presentation generator, not
   `game_random`**).
2. Push (x, y) as floats, (r0, r1) as floats, `game.frame`, `who`, `o`, and two fresh empty `SimpleArray<int>`.
3. `TerrainOut::terraform_for_nuke(x, y)` (section 5), then `Terrain::refresh_good_z` (recomputes the z of every good and item with the VALID bit
   from the new heights; the engine keeps no z for goods, so nothing to port).

**No `game_random` draw.**

## 4. `Nuke::do_damage()` @92bc80

Called (a) first thing in every `Objects::inc_time` @65db70 (frame-spec 7), and (b) by the impact itself, so a nuke's first call is on the frame it
lands (the later calls run in the following frames' `inc_time`). Each call:

**4.1 Pruning.** For every explosion `i` in list order: if `game.frame < start` or `start + max_nuke_frames + 0x140 <= game.frame` (110 + 320 = 430
frames after the start) the explosion is removed by **moving the last element into slot `i`** (all the parallel arrays; the hit lists are freed
and the last list moved) and slot `i` is examined again.

**4.2** `NukeOut::graph_do_damage` @925970: the smouldering fire particles (12 per explosion around `frame - start == 66`, radius 30..310,
a local `Random(x + y)`), presentation only.

**4.3** Then for every remaining explosion `i`, with `d = game.frame - start`:

1. **Armageddon.** `(float)(d - 1) / 110.0f < 1.0f && 1.0f <= (float)d / 110.0f` (i.e. exactly `d == 110`), and `info.starting_resources != 8`, and
   (`semaphore[0] & 4` multiplayer **or** `game.num_nations > 1`): `game.armageddon += 1` (@92bf09). **This is 110 frames after the detonation, once per
   explosion**, not at the detonation. The check runs on every call, so when a second nuke lands in the very frame some explosion is 110 frames old,
   its `do_damage` call counts the first explosion a second time (the port does the same because it calls `nuke_do_damage` the same way).
   The port calls `armageddon_nuke_detonated` (victory.cpp), which holds the two conditions and logs the event 26.
2. **The damage window.** Only while `start <= frame < start + 10 + 30` (40 frames). `f = 0.0f` before `start + 10`, else
   `f = (float)(frame - start - 10) / 30.0f`. The current radius is
   `R = (int)(r1 * f + (1.0f - f) * r0)` (single precision, truncation): 100 for the first 10 frames, 1859 on the last frame (`d = 39`, `f = 29/30`, r1 = 1920).
3. If `who == -1` or `o == -1` there is no shooter and nothing more happens. The shooter (`this` of every `Object::do_damage` below) is the
   object `objects[who][o]` (the dead missile's slot).
4. **Units.** `Objects::find_units(x, y, SEARCH_ALL, -1, R, 0, FILTER_ALL, 0, 0, R, 1)` @65a620: the on-map live units of leaders 0..7
   (parameter 11 = 1 excludes gaia). Order: with `circle_radius[ceil(R / 768)] <= game.total_units` the circle cells of the world grid in table
   order (each wcoord's object list from its head), else every leader's unit slots in order (kept ported as in aircraft.cpp). For each
   found unit `(idx, vw)`, still live and on the map, **not hurt before by this explosion** (`been_damaged_before(vw, idx, i)`), and `!is(0x13b, 0)`
   (a nuclear missile is immune):
   `dist = vector_dist(x - u.x, y - u.y)` (octagonal), must be `<= R`; `pct = 0x100 - (dist << 8) / (int)r1` (**r1, not the current
   R**), must be `> 0`;
   `Object::do_damage(shooter; idx, vw, find_angle(u.x - x, u.y - y), guys 1, ammo -1, mult pct, splash 1, no_react 0)`; append `(idx, vw)` to the hit lists;
   then, if `Game::war_allowed()` and `vw != who`: the victim's leader, if its flag 4 is clear and the territory owner of the wcoord of **the
   explosion centre** is the victim and `!is_enemy(victim, shooter)`, runs `Leader::action_declare(shooter, 0, free = 1, force = 0)` (declares war on the shooter).
5. **Everything else in the wcoords around** (buildings; the cell lists also hold units, which are skipped here). `rr = min(R / 8 + 1, 10)`; the cells
   `(wcx + move_x[k], wcy + move_y[k])` for `k = 0 ... radius[rr]` **inclusive** (`radius[r] = (2r+1)^2`; at `rr = 10` the 442nd entry reads the
   two ints that follow the tables in `.data` - `move_x[441] = 0x610076`, `move_y[441] = 120` - so that cell is far off the map and is skipped; the port stops at k = 440).
   Cells off the map are skipped. For each object of the cell list (`down` / `down_who` chain; **the next link is read after the damage**),
   not hurt before, `is_live_build` (vslot +0xc: a live building):
   `dx = max(0, |x - b.x| - 96 * x_size)`, `dy = max(0, |y - b.y| - 96 * y_size)`, `dist = vector_dist(dx, dy)` (the footprint's half size, tiles of 192),
   must be `< R` (strict), `pct = 0x100 - (dist << 8) / (int)r1` must be `>= 0` (zero allowed);
   `Object::do_damage(shooter; idx, vw, find_angle(b.x - sh.x, b.y - sh.y), guys 0, ammo -1, mult pct, splash 1, no_react 0)`; append to the hit lists; then the
   same war declaration as for units, but skipped when `!Game::war_allowed()` (the rush-rules test is inlined) and the two diplomacy tests are
   written out (`leaders[victim].diplos[shooter] != 0 && leaders[shooter].diplos[victim] != 0`, i.e. `!is_enemy`).

**Every `game_random` draw of an explosion is inside `Object::do_damage`** (combat-spec 5; `take_damage`, deaths, `Unit::react`): `nukes.cpp`
itself draws none. The order of the calls (units in `find_units` order, then cells in spiral order) fixes those draws.

## 5. `TerrainOut::terraform_for_nuke(x, y)` @869ca0 (the scorch)

`nuke_crater_size @c06290 = 3` and `nuke_crater_depth @c0628c = 30.0f`. Tile coordinates `tx0 = x / 192`, `ty0 = y / 192` (`div_3_table[x >> 6]`).
For `k = 0 ... radius[3] - 1` (the 49 tiles of `move_x/move_y`, a 7 x 7 square):

* skip when off the map (`tx < 0`, `ty < 0`, `tx >= tile_xs`, `ty >= tile_ys`), or `tx == 0`, `ty == 0`, **`tx == xs - 1`, `ty == ys - 1`** (the
  original compares the *tile* coordinate with the *wcoord* dimension, a quirk kept);
* skip when `tdata & 3` is 3 (building) or 2 (mountain), or `WorldData::is_cliff_at` (`tdata & 3 == 1`);
* skip when the wcoord `(tx >> 2, ty >> 2)` has `land != 0` (only plain `LAND` is scorched);
* skip when `tx + xs * ty * 4` is already in the list at `terrain + 0x4bd4` (a tile sinks once, however many nukes fall on it); else append it, and for
  the four corner heights `(tx, ty)`, `(tx+1, ty)`, `(tx, ty+1)`, `(tx+1, ty+1)` of the grid `(4 xs + 1) x (4 ys + 1)`:
  `v = h - 30.0f; if (0.0f < v) h = v`.

A vertex shared by neighbouring tiles of the square sinks once per tile (up to 4 x 30), and never below zero (a vertex that would reach 0 stays).
The second loop (`calculate_norms`, `calculate_tangents`, `TerrainVis::invalidate_wcoord`, `camera_changed`) is rendering; `TerrainOut::rebuild_nuke_craters`
@875850 rebuilds the visual craters from the list after a load. The heights are saved in section `HGHT` since version 5, the dedupe list in `NUKE`.

## 6. The nuke embargo (the stamp's only reader)

`LeaderData::get_my_nuke_embargo` @6d5350: `0` when `+0x7b4` (stamp) is 0 or the leader owns the **Space Program** wonder (0x21e); else
`max(0, nuke_embargo_world * game.armageddon + nuke_embargo_nation * nukes_used - game.frame + nuke_embargo_base + stamp)`
(rules constants @0xd20 base 900, 0xd24 nation 900, 0xd28 world 0). `get_nuke_embargo` @6d52c0: 0 with the wonder, else the maximum over the VALID leaders
that are the leader itself or its allies of `get_my_nuke_embargo`. `is_nuke_embargo` @6d52b0: `!= 0`. It blocks `Leader::action_buy` / `action_sell`
(`econ_market.cpp` `nuke_embargo`, which now asks `is_nuke_embargo`), `Leader::use_market` and `market_speculation` (the AI; `ai_production.cpp`
does not test it yet), and the market panel. A detonation therefore costs the leader (and its allies) at least 900 frames of trading.

## 7. Not in the original / not ported

* Sounds (`S_NUKE_HIT`, `S_ATTACK`, the explosion lookups, `S_MISSILE_SHIELD`), the achievement, the console messages and `MessageWin::add_event`,
  `NukeOut::draw*`, `graph_do_damage`'s particles, `TerrainOut` normals and visibility: presentation. The messages are logged as `GameEvent`s.
* **The AI's nuke targeting is not in `Leader::*nuke*`** (there is nothing but `get_nukes`, `can_nuke`, `is_nuke_embargo`): the AI launches through
  `Group::action_flight` / the silo logic of aircraft.cpp (aircraft-spec 8, which already holds the `armageddon - 2` waits). `LeaderData::nukes_in_flight`
  (+0xa3c) is only ever written by `Leader::init`.
* `ObjectsData::count_nukes_in_flight` @658490 (UI): ammo with the nuclear line as shooter.
* The ConquestGame (Conquer the World) nukes are the strategic layer's (ctw-spec).
* Spline flight of the missile (`Spline::calc_nuke_spline`, combat-spec 4.18.1) is still the plain flight time; the explosion happens when the ammo
  lands, so the *when* is approximate and the *what* exact.

## 8. Port map, events, tests

* `nukes.cpp`: `nuke_add` (add_nuke), `nuke_do_damage` (do_damage), `nuke_detonate` (the nuclear half of `Ammo::do_damage`), `nuke_missile_impact`
  (the missile branch with the defence test; called from `ammo_do_damage` in `combat_fight.cpp`), `get_my_nuke_embargo` / `get_nuke_embargo`.
  `objects_inc_time` calls `nuke_do_damage` first of all. State: `Game::nukes` (`NukeState`), saved in section `NUKE` (save version 10).
* `GameEvent` kinds 27 (nuclear missile launched: `build_do_missile_launch`, who / o = missile / o2 = silo / x, y = target), 28 (explosion began),
  29 (stopped by missile defence), plus the existing 26 (the Armageddon count, 110 frames later). Never read by the simulation.
* `ron-sim <scenario> --nuke WHO:X,Y@FRAME` creates a NUCLEARMISSILE of the leader at the point, detonates it (`nuke_detonate`) and checks at the end:
  everything within 60 % of the final radius is hurt or dead, nothing beyond the radius is touched, the height grid changes only in the 7 x 7 tiles by
  multiples of 30, `nukes_used` / the stamp are set, Armageddon is counted exactly once 110 frames later. `--victory armageddon` also drives a detonation and
  its 110th frame through `nuke_do_damage` and ends the game (defeat type 8).
