# Random maps: `Setup::build_game`, `Map::make` and the empire placement

Status 2026-10-24: 🟡 decoded from the decompiles (`map.obj`, `setup.obj`, `terraingroups.obj`, `fractal.obj`); the
decompiler lost many return paths and conditions in this code (marked ❓ where a port must read the disassembly
before claiming bit-exactness). No recording of an original random map exists on this Mac, so nothing here is
validated against the game. Part 1 (this file) covers the driver, the map style data, the base continent
generators, land shaping and the starting empires. The other parts: `randommap-rivers.md`, `randommap-resources.md`,
`randommap-terrain-groups.md` (terrain groups, forests, mountains, doobers, the per-style continent recipes).

Coordinates: wcoords (`WData`, `World +0x134`, stride 0x1c: `flags` u16 +0, `land` i8 +2, `land_sub` +3, `region`
i16 +4, `region2` +6, `down` +8, `val` +0xc, `goods` +0xd, `light` +0xe, `who` +0xf). `World`: `xs` +0, `ys` +4,
`cells` (xs·ys) +8, `map_style` +0x30, `sea_map` +0x34, territory limits +0x38..+0x4c, `land_size` +0x78, `seed`
+0x7c, the start lists `start_x` / `start_y` (`SimpleArray<int>` at +0x80 / +0x9c: count +4, data +0x10 → +0x90 /
+0xac), the oil lists +0xb8 / +0xd4 (count +0xbc, data +0xc8 / +0xe4), the forest bit set +0xf8. `land` values seen:
0 grass, 1 / 2 water (2 = ocean, 1 = coast water); `flags` bit 0x100 marks a half-land (river / coast) cell, bit 4
the coastline, bits 0x30 blocked, bit 0x400, bit 0x20 forest-reserved. Regions: `Regions.list` entries are 0x88
bytes: `+0x0 common_factor, +0x4 goody_factor, +0x8 flags, +0x14 size, +0x6c Array<TCoordData> cells (count +0x70,
capacity +0x74, data +0x7c)`; land regions are 1..63, sea regions 64..126; `regions.land` / `regions.sea` are the
counts in use.

`rnd(n)` is `Random::get(game_random, 0, 0xffff) % n`; `rnd&k` is the same draw masked (sign-fixed). Every draw is
on the simulation stream and must be reproduced in order.

## 1. `Setup::build_game(splash, from_scenario, p3)` @5ac190 — the game start

1. CtW: `info.seed = conquest_game.seed`. `game_random.seed = info.seed`. `semaphore byte0 &= ~0x40` (game over
   off); `semaphore.flags = 2` if 0. Zero `world_pop`, `world_cities`, `world_villages`, `total_units`, `frame`,
   `musical_chairs`, `tick`, `market_tick`, `graphic_tick`, `armageddon`, `wonders[17]`. `Objects::clear`,
   `Groups::clear`; per leader 0..9: +0x408, +0x420 (oil well / caravan marks), +0x434 (supply_mark), +0x43c = 0.
2. Editor or map editor (`semaphore byte1 & 10`): `world.map_style = 0x17` (BLANK_MAP).
3. **Draw 1:** `world.seed = rnd(0xffff) + 1`.
4. Random map (`from_scenario == 0`): `game_map = Map::new_map(info.map_style)` (§2); `world.map_style =
   map.type`; `map->make(num_nations, −1, splash)` (§3); keep `map->randomize_starts`; `Terrain::init` (renderer
   heights and surfaces). Scenario: `ScenarioRead::import` (world-spec), the console seat, `reset_obs_flags`.
   CtW: `ConquestGame::build_conquest_game`.
5. Market: `market[k] = 50, flux = 15, next_flux = 15, flux_length = 1, delta = 0` for the 6 goods.
6. `Terrain::mark_forest` (the forest bit set from the FOREST land cells).
7. Random map only — **start order**: `start_order[8]` (GameInfo +0x638): with `randomize_starts` draw `rnd & 7`
   repeatedly until an unused slot comes up, eight times (one draw per attempt, duplicates re-drawn); without it
   `start_order[i] = i`. Then for `i = 0..7`, `who = start_order[i]`, if leader `who` is VALID: `Leader::init(who,
   tribe, who)` (ai-spec §2: `random_personality` draws, in this order). `active = count of those`.
8. `p3 == 0`: graphics pre-load per leader (no simulation effect). `Leader::reset_score` for every valid leader.
9. Not the editor — **empires**: `placed[10] = 0`, `next_start = 0`; passes while `placed count < active`: for `i =
   0..7`: `who = start_order[i]`; `game.start_index[who] = i`; if VALID and not placed: (team style 7, the "team 8"
   seats: special-cased ❓, rarely used) `start = randomize_starts ? next_start : order_index[who]` where
   `order_index` numbers the VALID leaders 0.. in slot order; `build_empire(who, start)`; `placed[who] = 1`;
   `next_start++`; unless `world.map_style == 0x16` (British Isles): every other VALID, unplaced leader `j` with
   `is_team(who, j)` is placed right after it (`build_empire(j, next_start++)`), so team mates take consecutive
   start locations.
10. `World::analyze_map`, `World::compute_all_territory`; every valid `Herd::create_units` (animals from the
    resource placement, `randommap-resources.md`); the scenario / general-powers scripts compile; CtW setup script.

The scenario path of this function is what `Game::load` already follows (sim-implementation). A random game is
steps 3-4-7-9-10 added before the per-frame simulation.

## 2. `Map::new_map(style)` @6a0950 and `Map::set_map` @6a1340

`MapStyleIndex`: 0..5 are the random pickers (0 / 3 any land map, 1 / 4 "land" maps, 2 / 5 "sea" maps; 3..5 ❓ the
same lists under another name), 6 OLD_WORLD (`NUM_RANDOM_MAP_STYLES` in the enum), 7 GREAT_SAHARA, 8 AMAZON_BASIN,
9 HIMALAYAS, 10 SOUTHWEST_MESA, 11 AFRICAN_WATERING_HOLE, 12 MEDITERRANEAN, 13 AUSTRALIAN_OUTBACK, 14 GREAT_LAKES,
15 WARRING_STATES, 16 NEW_WORLD, 17 COLONIAL_POWERS, 18 EAST_INDIES, 19 EAST_MEETS_WEST, 20 SEA_POWER, 21
NILE_DELTA, 22 BRITISH_ISLES, 23 BLANK_MAP, 24 CONQUEST_MAP, 25 CONQUEST_MAP_SEA, 26 SCENARIO_MAP. Each gets its own
`Map` subclass (`MapOldWorld`, …; 6..22 share one vtable family with a per-style `make_continents`). The file names
(`Map::init_map_file_names`, `mapstyles\<name>.xml`): 6 OLDWORLD, 7 SAHARA, 8 AMAZON, 9 HIMALAYAS, 10
SOUTHWESTMESA, 11 AFRICA, 12 MEDITERRANEAN, 13 OUTBACK, 14 GREATLAKES, 15 WARRINGSTATES, 16 NEWWORLD, 17
COLONIALPOWERS, 18 EASTINDIES, 19 EASTWEST, 20 SEAPOWER, 21 NILEDELTA, 22 BRITISHISLES, 23 DEFAULT, 24 CONQUESTMAP,
25 CONQUESTMAPSEA.

`set_map` picks a concrete style for 0..5 from a 17-entry class table over styles 6..22: `{0,0,2,2,2,2,2,2,1,2,2,
1,1,1,1,1,1}` ❓(the bytes `00 00 02 02 | 02 02 02 02 | 01 02 02 01 | 01 01 01 01 01` read from the stack image: 2 =
land map, 1 = sea map, 0 = neither). Draw `rnd(17)` until the class matches: style 1 / 4 want class 2 (land), not
style 15 (WARRING_STATES) in team style 2; style 2 / 5 want class 1 (sea), never 17 (COLONIAL_POWERS) and, in team
style 2, not 15; style 0 / 3 want class ≠ 0, never 16 / 17 (NEW_WORLD, COLONIAL_POWERS) and, in team style 2, not
15 either. Each rejected draw is a draw.

## 3. `Map::make(n_players, seed, splash)` @68bc90 — the map pipeline

```
stash_nations_here = n; if seed >= 0: world.seed = seed; game_random.seed = seed
file = "mapstyles\" + name[type]; load_map_data(n, splash)                                   (§4)
if base_edge < 0: base_edge = rnd & 3                                                        (draw)
make_continents(n)   (virtual: §5 base, §6 per style)      → WData.land, regions, start locations
Regions::clear_all / find_all; world territory limits (+0x38..+0x4c) = the map's six values
if type != BLANK_MAP:
    fix_diag_land (§7.1); make_coastlines(n) (§7.2); Regions clear / find_all
    TerrainGroups::fill_fertile                                                              (terrain-groups part)
    unless map editor (semaphore byte1 & 2): TerrainGroups::place_all(splash, 1); check_player_forest (§7.3);
        TerrainGroups::nubify_forest
    tileset group_data forest_base / mountain_base / coast_base >= 0 → change_*_base; fix_transitions
    (map-make log lines: style, size, xs × ys, percentage of the normal size, cells, each start location)
    unless map editor, and (not CtW or the conquest style allows rares): place_resources(n)  (resources part)
    World::adjust_for_blocking; Regions clear / find_all; World::compute_all_vals
    if sea_map >= 0: world.sea_map = sea_map
```
`MapScenario::make` (style 26) and the CtW maps have their own `make`; `BLANK_MAP` stops after the regions.

## 4. Map style data (`Map::load_map_data` @69dad0, `init_map_data` @69f250)

Defaults: the six territory limits from constants (`territory_limit_base / _civic / _city`, used for both the
player and the colonized set). `mapstyles\default.xml` is read first, then the style's file; every value the file
has overrides the default. XML (element → `Map` field):
- `<MAP>`: `RIVER_THRESHOLD value`, `GOODY_BOXES value` (goody_factor), `COMMON_RESOURCES value` (common_factor),
  `AVOID_CENTER scalevalue`, `AVOID_CONTINENT scalevalue` (at least `constants.min_continent_spacing`),
  `AVOID_EDGE_0..3 scalevalue`, `BASE_EDGE value` (−1 = random, §3), `SEA_MAP value` (−1 = unchanged),
  `PLAYER_TERRITORY_BASE/CIVIC/CITY_HARD_LIMIT value`, `COLONIZED_TERRITORY_BASE/CIVIC/CITY_HARD_LIMIT value`.
  Outside CtW `<TILESET>` holds `<TILECHANCE chance type>` children: **draw** `rnd(sum of chances)` when the sum
  > 1, walk the children accumulating chances, the first child whose running sum reaches the draw names the
  tileset (it must be one of `TileSet::available_tilesets`, matched case-insensitively); no match or no element →
  **draw** `rnd(number of available tilesets)` when there are more than one.
- `scalevalue="N [AREA|SCALE|SCALEWATER|SCALELAND]"` (`Map::scale_number` @6a0320): N alone; `AREA` →
  `scale_by_area(N) = max(1, (R² / 2 + cells · N) / R²)`; `SCALE` → `scale_by_axis(N) = max(1, (R / 2 + xs · N) /
  R)`; `SCALEWATER` → `(int)(scale_by_axis(N) · 2 · sea / cells)` and `SCALELAND` → `(int)(scale_by_axis(N) · 2 ·
  (cells − sea) / cells)` in float, where `sea` is the summed size of the sea regions (64..`regions.sea`) at the
  time of the read. `R` is the reference map width `map_sizes.list[+0x144]` (the rules `mapsizes` category, the
  "normal" size's dimension ❓ which entry).
- `<RESOURCES>` (`init_resources` @69e990): per `LAND_0..LAND_8` element (one per gather land; `Lands` records of
  0x138 bytes): children `FOOD TIMBER WEALTH KNOWLEDGE METAL OIL` with `value` go to +0x24..+0x38 and sum into +0x3c;
  `<RARE_RESOURCES><RARE name>` list the rare goods of that land (good keys, into +0x50.., count +0x10; the entry
  at +0x14 ≠ −3 gates the count ❓).
- `<RIVERS><RIVER name mask riverbed cost>` (`init_rivers` @69e520): per named land, three booleans (+0x40 `mask`,
  +0x44 `riverbed`, +0x48 `cost` present or not).
- `<TILESET_DATA><TILESETNAME>` → `TerrainGroups::init_tileset_data` (default first with flag 1, then the file's
  with 0); `<TERRAIN_GROUPS><GROUPENTRY …>` → `TerrainGroups::init_terrain_data(n)` (the file's, else the
  default's). Both in the terrain-groups part.
- `MapStyleIndex` also selects `map_file_mapping`; `tileset` changes trigger a tileset reload (renderer).

Other `Map` fields the generators read: `avoid_center`, `avoid_continent`, `avoid_edge[4]`, `base_edge`,
`avoid_modify` (a drifting edge margin, §7.4), `region_flags`, `common_factor`, `goody_factor`, `river_thresh`,
`sea_map`, `randomize_starts`, `stash_nations_here`, `terrain_groups`.

## 5. The base continent generator (`Map::make_continents` @694ab0) and `Fractal`

`Fractal::init(xs, ys, smooth, p4, flags, seed)` @6aa2d0: a (xs + 1) × (ys + 1) byte height field built by midpoint
displacement on its own `Random` (`seed`), independent of `game_random`. `smooth` (clamped to 5) is the number of
subdivision levels; `flags & 1` = wrap in x; `flags & 2` = scale heights to 0..99 on read. Pass 0 (level `smooth`)
seeds the corner lattice `random.get(0, 255)`; each finer level fills the midpoints: between two lattice points `(a +
b + 1) / 2`, at a square centre `(a + b + c + d + 2) / 4`, each plus `random.get(0, 2^(8 − smooth + level) − 1) −
2^(7 − smooth + level)`, clamped to 0..255 (the low clamp is lost in the decompile: ❓ 0). Without x-wrap the last
column copies the first before and after. `get_height(x, y)` @6aa870: bilinear sample at `((x + 0.5) · x_inc, (y +
0.5) · y_inc)` with `x_inc = xs / (xs − (flags & 1) + 1)`, `y_inc = 1`, clamped to 255; with partitions set
(`parts[16]`, `clear_partitions` resets them to −1) the result is the index of the first partition bound above the
value ❓.

`Map::make_continents(n)` (the default, used by Old World ❓ and as the base of several styles): `World::wipe`,
`Regions::clear_all`; four fractals over the map: `F1(xs, ys, 3, xs?, 3, seed · 0x13)`, `F2(…, 4, 0, seed · 0x1d)`,
`F3(…, 2, …, 0, seed · 0x1f)`, `F4(…, 4, 0, seed · 0x25)` (`seed = world.seed`); then per lattice point `F2 = ((F3 ·
(F2 − 128)) >> 8) − 128` ❓ and `F1 = clamp(F1 + (F4 − 128), 0, 255)`; then per cell `d = WData.val − 14` (kept
from the previous cell when negative ❓: `val` here is a pre-pass edge distance), `land = (F1.get_height(x, y) < 0x37
− 4 d²) ? 2 (water) : 0 (land)`. No start locations are placed here (the styles do it).

`MapGrass::make_continents(n)` (the editor's blank land): wipe, every cell land, then `n` start locations on a circle
of radius `min(xs, ys) / 3` around the centre (`sin_table` steps), `Regions::find_all`.

## 6. A style recipe: `MapMediterranean::make_continents(n)` @69add0

The styles are variations on: wipe, seed a region, grow it to a land goal, carve, place starts, then islands.
```
goal0 = max(4, scale_land_area(this+0x30 ❓, n))            // scale_land_area: v / ((players_for_map_size) / 8)
loop:                                                        // until the main region grows
    World::wipe; Regions::clear_all; m = (min(xs, ys) & ~1) / 3; land_goal = 3 m²
    draws: a = rnd(0xffff), b = rnd(0xffff) → angle0 = (a << 16) + b  (a fixed-point angle)
    cx = xs / 2 + (rnd & 1); cy = ys / 2 + (rnd & 1)
    make_region(1, cx, cy, land_goal); if grow_region(1, land_goal, m, −1, −1, 0) fails: retry the loop
Regions clear / find_all; eliminate_pools(ENTIRE_WORLD) (§7.5); Regions::clear_all; invert_land (land ↔ water);
Regions::find_all; eliminate_pools(ENTIRE_WORLD); Regions::clear_all
for each player i (n times): angle += 0x100000000 / n (wraps); radius r from 3 xs / 2 down to m:
    project(angle, r) → (px, py) from the centre; the five orthogonal neighbours must lie inside the map and off its
    border (else "edge" flag); the cell must be water-free land (not half-land, land not 1 / 2) and
    is_near_ocean(px, py, 3, 9) ("coast" flag); accept the first (px, py) in bounds with coast and not edge:
    World::add_starting_location(px, py); otherwise r −= 1 and, below m, angle += 0x71c71c6 and r restarts
check_player_land(ships' range ❓ (unit type 0x15d's max_range / 4), …)                             (§7.6)
goal1 = max(0x4b, scale_land_area(0x96, n)); cx = xs/2 + (rnd & 1); cy = ys/2 + (rnd & 1)
make_region(0x20, cx, cy, goal1); grow_region(0x20, goal1, m, −1, −1, 0) (an island region, 32) → on failure the
    whole loop restarts; on success eliminate_pools(EDGES_ONLY)
```
`scale_land_area(v, n)` @68b1e0: `players_for_size = {2, 2, 3, 4, 6, 8…}[map_size]` (0..4, else 8); `(int)(v /
(n / players_for_size))` in float (0 when the divisor is 0) ❓ the decompile shows `get_player_overage` called first
and its result unused. `get_player_overage(n)`: `n − players_for_size` (0 when ≤); `get_player_underage(n)`:
`min(n, {2,2,2,4,5,7,8}[map_size] − n)`.

Region building blocks:
- `make_region(r, x, y, goal)` @69d3f0: cell (x, y) becomes land of region r; the region list gets the cell, size 1,
  `flags = region_flags`, `common_factor`, `goody_factor`; `regions.land = max(regions.land, r)`.
- `point(r, x, y)` @69d8f0 ❓(not read): make (x, y) land of region r if `grow_valid`; `stamp` = the point and its
  four neighbours; `large_stamp(r, x, y, left)` = a stamp plus a random (rnd & 15) one of 16 preset offset
  patterns of further stamps.
- `grow_region(r, goal, m, ox, oy, p6)` @69c600: `k = 2`, or the smallest `k ≤ 64` with `3 k² ≥ goal / 4` when
  `goal / 4 > 12`; (ox, oy) default to the region's first cell. If the region has < 2 cells: every cell of
  `circle_radius[k]` offsets 1.. around the first cell that `grow_valid`s is `point`ed. Then until `size ≥ goal`:
  pick a cell of the region — when `size < scale_by_area(?)` ❓ or `rnd(size / 8) == 0`: a uniform `rnd(size)`
  cell (or among the last `size − first_ring` cells when the ring is known); else a cell `rnd(size / 8 + 1) + 7
  size / 8` (one of the newest eighth) and, if its distance to (ox, oy) exceeds `m`, the ring index resets; `left
  = goal − size`: `left < 64` → a `stamp` (five `point`s, each conditional on the previous), else `large_stamp`.
  A pass that adds nothing counts; `8 goal` idle passes or `goal` stalls give up (return ≠ 0 ❓ sense). Returns
  the final size (0 = failure in the callers' reading ❓).
- `grow_valid(r, x, y)` @69d000: false when another non-empty region `q` (whose flags share bit 0 with r when r ≥ 0
  ❓) has a cell within `avoid_center` (`vector_dist`), when within `avoid_continent + 1` of the map border the
  circle test finds a foreign region cell within radius `avoid_continent`, or when the drifting edge margin says
  so: for `e = 0..3` with `avoid_edge[e] ≠ 0`: **draw** `avoid_modify += rnd(3) − 1`, clamped to 0..4 ❓(low clamp
  lost), `margin = avoid_edge[e] + avoid_modify`, side `(base_edge + e) & 3`: 0 → `x < margin`, 1 → `y < margin`, 2
  → `xs − x ≤ margin`, 3 → `ys − y ≤ margin` fails.
- `invert_land` @69d? : every land cell becomes water (2) and every water cell land (0), regions cleared ❓.
- `fill_cont(count[8], team_of[8], extra)` @68a960: teams → continent assignment for the continent maps: counts
  the VALID leaders per team (team 8 = solo), numbers the teams, then for pairs of teams with equal counts flips a
  coin (`rnd & 1` ❓ bit) to swap their continents.

## 7. Land shaping

1. `fix_diag_land` @69c250: for every water cell (land 0 ❓ sense inverted in the decompile: "cell == 0") whose
   diagonal neighbour in direction k (corner_x/y, k = 0..3) is also 0 while the two orthogonal cells between them
   are non-zero → the cell becomes 2 (breaks diagonal land bridges / canals).
2. `make_coastlines(n)` @6947a0: `fix_lakes` first ❓(not read: MapGreatSahara overrides it). Pass 1: every
   non-water cell (not half-land, land not 1/2) without bit 4 that has a water neighbour (8 directions,
   `move_x/y`) gets `flags = (flags & ~0x3c) | 4 | 0x400` (coast). Pass 2: every coast cell (bit 4) with no
   land-or-coast neighbour... ❓ (a coast cell whose 8 neighbours are all water or coast) becomes water: `flags &=
   0xfac3`, `land = 2`.
3. `check_player_forest` @68e8f0: per start location: count the cells with bit 0x20 within `circle_radius[R]`, `R =
   city_center_radius / 4 − 1`; fewer than 3 → up to 3 attempts: walk the ring cells from `circle_radius[2]`
   outward, take the first free cell (flags & 0x30 == 0, land, flags & 0x48 == 0, not in the forest bit set, not
   coast) and grow a 3-cell clump with its orthogonal neighbours; when the clump reaches `local_24` (3, then 2, 1)
   cells each gets `land_sub` copied from the first, `land = 2 if coast else its land`, `flags = (flags & ~0x3c) |
   0x20` ❓ (a guaranteed starting forest).
4. `check_player_land(R, …)` @68ef00: per start location: a start on region 0 (no land) adopts the neighbouring
   land region (its cells move over ❓); then every cell within `circle_radius[R]` (default 5) joins the start's
   region as land (`land = 0`), unless a nearby (radius `radius[min(p, 10)]`) cell of another region with > 4
   cells exists ❓ — guarantees land under and around each start.
5. `eliminate_pools(mode)` @68b890: collect the sea regions (> 64) touching the cells selected by `mode`
   (ENTIRE_WORLD every cell; EDGES_ONLY the border cells; CORNERS_ONLY the corners); while more than one remains:
   the smallest (by size) is merged into the first neighbouring region ≠ itself found scanning its cells' 8
   neighbours (`move_x/y`): every cell gets that region id and the region lists / sizes move; the merged id leaves
   the list. `eliminate_edge_canals` @68b360: on each of the four borders, a run of three water cells on the border
   row whose inner-row cell is land makes that inner cell water (2) and, with three such inner cells, the next
   row's cell too.
6. `force_continent_spacing(p)` @68e6a0: regions ordered by size; for each land region's edge cells
   (`World::is_edge_of_region`), any cell within `move_x/y[0..0x30)` (radius 3 ring) that is land of **another**
   region becomes water (2) and leaves its region (size −1) — keeps continents apart.

## 8. Starting locations and empires

- `find_region_centroid(r, &x, &y)` @68ae50: the mean of the region's cells (integer division).
- `place_start_in_region(r, &x, &y, min_dist, p5, starts_x, starts_y)` @68ac00: two tries (`t = 1, 2`), margins
  `5 / 6` then `3 / 4` from the map border; **draw** `rnd(size)` picks a starting index into the region's cells,
  then walks them cyclically: a cell inside the margins whose `vector_dist`-like distance (the `max + min² / 2max`
  form) to every existing start location is `≥ min_dist` (`≥ 6` on the second try when `min_dist < 6`) and
  `is_near_ocean(x, y, t ≠ 2 ? 3 : 2, t ≠ 2 ? 9 : p5)` → it is the start. Returns ❓ (lost).
- `is_near_ocean(x, y, a, b)` @68b0a0: true when some cell of the ring between `circle_radius[b − 1]` and
  `circle_radius[b + 1]` is land ❓(lost) and/or some cell within `circle_radius[a]` is water — the decompile keeps
  only the loops; the Mediterranean call `(3, 9)` reads as "water within radius 3 and land around radius 9".
- `find_avail_regions(list, sea, p3)` @68f380: the regions (land 1..63, or sea 64..126 when `sea`) with more than
  5 cells (for `p3 == 3` skipping regions that already hold a start) in a list; **draw** `rnd(count)` when count >
  1 and rotate the list's current element to that index.
- `World::add_starting_location(&x, &y)`: appends to `start_x / start_y`.

`Setup::build_empire(who, start)` @5abb80: `info.flags & 4` (no nation powers): type FARM 0x1a1 and GRANARY 0x1a7
`build_flags |= 0x80000` ❓. Not the editor: `c = build_cities(who, start, info.starting_town)` then
`build_units(who, start, c, starting_town)` (CtW: `ConquestGame::build_cities / build_units`); Spanish (tribe
bonus 9) `leader_flags |= EXPLORE_MAP 0x1000`. In the editor, `who == 0` runs the unit balance test instead.

`Setup::build_cities(who, start, town)` @5ab910: `wx = start_x[start]`, `wy = start_y[start]`; `leader.home_reg
(+0x9c8) = WData(wx, wy).region`. `town == 0` (nomad) or `leader +0x4 & 0x80` → −1. `x = wx · 768`, `y = wy ·
768`; `o = find_free(who, 2000, 3000, −1)`; `BuildTypeData::snap_center(CITY 0x19e, x, y, &x, &y, who)`;
`Build::init(who, 0x19e, o, x, y, 0)`; Chinese (tribe bonus 14) with `chinese_large_city` or
`chinese_large_cities`: `set_type(0x19f LARGE_CITY, 0)`; `Build::activate(0, 0, 0)`; `leader.cities_built (+0x820)
++`; `update_seen(0)`, `update_seen_ally`; `city = b.city`; if ≥ 0: `World::compute_all_territory`; `town == 3`:
`large_city_buildings`, `build_civ_specific`; `town == 2`: `small_city_buildings`; then `build_civ_specific`
❓(the decompile calls it twice for town 3: read it as once). Returns `o`.
- `large_city_buildings(who, city)` @5aac80: `Leader::produce_building(t, city.o, 0)` (ai-spec: the AI's site
  search and `init_build`) for 0x1a2 (WOODCUTTER 418), 5 × FARM 0x1a1 unless Lakota (tribe bonus 19), 4 × TOWER
  0x1b7, LIBRARY 0x1b3. `small_city_buildings` @5aae10: 0x1a2; Americans (20) with `americans_starting_farms > 0`:
  that many farms; else non-Lakota: 3 farms; LIBRARY 0x1b3.
- `build_civ_specific(who, city)` @5ab760: Nubians (4) or Dutch (22): MARKET 0x1b4; Greeks (5) with
  `greek_university_early > 1`: UNIVERSITY 0x1a4; Koreans (16) with `korean_temple_upgrades > 1`: TEMPLE 0x1b5;
  Egyptians (7) with `egyptian_granary_early > 1`: GRANARY 0x1a7; French (10) with `french_lumbermill_early > 1`:
  LUMBER MILL 0x1a8; Iroquois (18) with `iroquois_senate`: SENATE 0x1b6.

`Setup::build_units(who, start, city_o, town)` @5aafc0: `(n, n_farm) = get_starting_citizens(town)` @5aaf50 (a
jump table on `town` 0..3 ❓ values not read: dump the table); `starting_resources == 7`: `n += WINE` ❓(an enum
value used as a number), `== 8`: `n += NUM_GAIATYPES` ❓; Americans with `americans_starting_farms > 3` and
`town == 2`: `n += americans_starting_farms − 3`; Lakota: `town == 2` → `n −= 3`, `town == 3` → `n −= 5`. Then with
`town ≠ 0`: `S = graft(SCOUT 0x45)`; `place_unit(who, city_o, current_upgrade(S), wx·768 + 384, wy·768 + 384)`;
Spanish: `spanish_extra_scout` more scouts, and one more when `reveal_map > 1`; Dutch: two
`current_upgrade(MERCHANTDUTCH)`. Greeks with `greek_start_scholars` and a city: that many `graft(SCHOLAR 0x34)`
placed and `go_inside` the UNIVERSITY found by `find_building(city pos, SEARCH_FRIENDLY, who, FILTER_TYPE 0x1a4)`.
Koreans: `n += korean_citizens[0]`. Then `n` citizens (`graft(CITIZEN 0x32)`): `town < 2` → `place_unit` each;
else the first `n_farm` are created by `Objects::init_unit` at building slot 2001 (the first farm), `come_out(0)`
and, unless `starting_resources == 8`, `add_gather_order(2001, QUEUE_NEW, 0)`; the rest walk the later building
slots for the next active FARM (`is(0x1a1)`, gather building, not a university) with room (`num_gatherers <
gather_max`) ❓ and are `place_unit`ed near the city (the decompile mixes the two paths; a port should follow the
disassembly here).

`Setup::place_unit(who, city_o, type, x, y)` @5abca0: `type = current_upgrade(type)`; with a city, (x, y) = the
city's position. Up to 60 attempts (500 for nomads): `ring = town ? circle_radius[2] : (leader.active ≠ 0 ?
circle_radius[24] : circle_radius[8])`; **draw** `k = rnd(ring)` (when ring > 1); `(cx, cy) = (wcoord(x) +
circle_x[k], wcoord(y) + circle_y[k])`; valid when inside the map, same region as the origin, `flags & 0x30 ==
0`, land (not half-land with land 1 / 2), `flags & 0x100 == 0`, `down < 0`, the centre tcoord's flags not bit 14
and `(flags & 3) ≠ 3`, `WData.flags` bit 15 clear → `Objects::init_unit(who, type, cx·768 + 384, cy·768 + 384, −1,
−1, −1)` and return it. After the attempts: with a city `Build::train(city, type)`, else `init_unit` at (x, y) +
`come_out(0)`.

## 9. Open items for the port

1. The ❓ marks above: `grow_region`'s give-up rule and return value, `is_near_ocean`'s two tests,
   `point` / `grow_valid` return paths, the starting-citizen table, the `fix_diag_land` sense, `eliminate_pools`'s
   merge target. All are in `re/decomp/map*` (local) and need the disassembly (`DisasmFunctions.java`).
2. `WData.val` before `compute_all_vals` (the `d` of the base generator): what writes it ❓.
3. Nothing is validated against the original: a recorded random game (seed, style, size, players) from the Windows
   build is the oracle; until then only plausibility (land fraction, start spacing, resource counts) can be checked.

## Oracle: current 2.SVX (2026-10-24)

Checked against `re/oracle/Saves/current 2.SVX` (seed 18119929, 100x100, lobby style 1 resolved to 14, Great Lakes) with `ron-mapgen --compare`.

- `Map::set_map` class table (stack image from byte 2): `{2,2,2,2,2,2,1,2,2,1,1,1,1,1,1,1,1}`, index i is style 6+i, 2 = land map, 1 = sea map. The earlier table was two bytes misaligned (picked 15, the save has 14).
- The style-pick draws are rewound: `game_random` returns to its state after `world.seed`, so the tileset draws and base_edge reuse them; the Great Lakes start angle is draws (d4, d5).
- Confirmed exact: `world.seed`, the five start positions (`angle = (d4%0xffff)<<16 + d5%0xffff`, step `0xffffffff/5`), the lake shapes.
- (second pass, 2026-10-24) `Terrain::init` @850f70 is ported to `engine/sim/mapgen_terrain_init.cpp` (`Gen::terrain_init`, called between
  `place_resources` and `make_rivers` in `build_game`). See the section below. `fix_water_tcoords` is gone from `newgame.cpp`: the generator
  sets the water tcoords itself.

### `Terrain::init` as far as it touches `WData` (ported)

Order: `TerrainOut::generate_land` (`determine_land_height_color` and `smooth_tcoord` on three `Fractal`s: seeds `world.seed`,
`world.seed * 2` and the texture fractal; then `build_coastal_data` = `mark_valid_coastal_influencers` @869070,
`mark_coast_vert_influence` @868ca0 (Bresenham `LineState` @a48000 / @a48110 with the jitter `int(raw * 0.16f)`, at most 15),
`adjust_aquatic_height` @868820, `relax_aquatic_verts` @868470 x5), then `check_and_set_halfland` @84e610 (`check_and_set_halfland_wcoord`
@84dec0, `mark_halfwater` @84ece0, `clear_halfland_wcoord` @84e500, `check_and_set_coastal` @84dd70), `convert_beach_to_sandy` @84b8e0,
`Map::make_rivers`, `RiversOut::calculate_all`, `adjust_for_mountains` @8703c0.

- Heights: land `30 + 7.5 F1 + 1.875 F2` blended by the distance to the rim; open sea -273; the shelf `30 + (d/3)^2 (-273)` from the nearest
  coast-line influence tcoord. All float arithmetic is done operation by operation (`#pragma clang fp contract(off)` in the new file).
- A wcoord whose `CoordInfo.flags` has 0x84 gets its 16 tcoords tested: a water tcoord = all four corner heights `< -30`. Any water tcoord
  makes it half-land (`mark_halfwater`: land 0, `spot_update_land` -> (1, 0)); sixteen of sixteen make it ocean (2, 0); none makes an ocean
  cell land with `land_sub = coast_base`. `verify_land` @84d8a0 / `TileSet::is_combo_acceptable` @87abe0 smooth the `land_sub` of the
  neighbours (the cascade is why a wrong half-land cell moves a few `land_sub` cells).
- Region ids are rebuilt after the pass (`clear_all` + `find_all`); the original keeps the lists incrementally (same ids, same raster order).
- `WData.flags` 0x80 (ROAD) and 0x4000 (BUILDING) in the save come from the start cities, not from the generator.

### Result on the oracle saves (`ron-mapgen --compare`, cells of 10000 / 6400 that differ: before -> after the second pass)

| layer / world | current 2 (Great Lakes, 100x100, frame 189) | current RN (East Indies 100) | east indies 2 (East Indies 100) | East indies final surround (80) | DO NOT DELETE (Old World 80) | DO NOT DELETE 2 (Old World 80) | curent dec 14 (Old World 80) |
|---|---|---|---|---|---|---|---|
| land | 1652 -> 10 | 3563 -> 32 | 3572 -> 49 | 2383 -> 20 | 0 -> 0 | 0 -> 0 | 0 -> 0 |
| land_sub | 3756 -> 158 | 2141 -> 720 | 2538 -> 1230 | 1427 -> 636 | 4097 -> 3509 | 4182 -> 3643 | 3595 -> 2655 |
| WData.flags | 4445 -> 368 | 3596 -> 1703 | 3807 -> 2412 | 2358 -> 1323 | 2437 -> 2220 | 2454 -> 2333 | 2138 -> 1732 |
| forest 0x20 | 806 -> 0 | 120 -> 0 | 113 -> 0 | 57 -> 0 | 74 -> 0 | 55 -> 0 | 68 -> 0 |
| mountain 0x10 | 643 -> 0 | 9 -> 0 | 19 -> 1 | 26 -> 1 | 0 -> 0 | 1 -> 0 | 0 -> 0 |
| doober/rock 0x200 | 2053 -> 0 | 135 -> 0 | 101 -> 0 | 71 -> 0 | 78 -> 0 | 66 -> 0 | 91 -> 0 |
| half-land 0x100 | 575 -> 30 | 1000 -> 63 | 989 -> 85 | 619 -> 40 | - | - | - |
| river 0x1000 | 339 -> 175 | - | - | - | 1163 -> 1074 | 1192 -> 1202 | 1067 -> 768 |
| goods exact / total | 12 -> 78 / 80 | 34 -> 134 / 136 | 39 -> 107 / 107 | 35 -> 111 / 112 | 11 -> 22 / 26 | 13 -> 24 / 26 | 12 -> 23 / 25 |
| goodies exact / total | 1 -> 33 / 50 | 0 -> 3 / 3 | 0 -> 1 / 1 | - | 0 -> 9 / 9 | 0 -> 10 / 10 | 0 -> 14 / 15 |

Only `current 2.SVX` is a fresh game (frame 189). The other saves were taken far into a game, so `val`, `land_sub` (farms, roads), the river
flag and city flags carry game-time changes the generator cannot reproduce. Everything the generator decides before the game starts
(land, half-land, mountains, forests, doobers/rocks, goods) agrees on all of them.

### Third pass (2026-10-24): rivers, moved goodies, the draw check

- **Rivers are done** (`randommap-rivers.md` sections 6 and 9): the A* tie order and dedup were the cause of the shorter paths, and what the saves hold
  beyond the path (the ±1 band, `tdata 0x800`) is the river trough, `RiversOut::calculate_all`. Results: river flag cells exact on three of four worlds
  (953 / 845 / 688), river tiles (`tdata 0x800`) exact (8462 / 7510 / 6049), `current 2.SVX` 201 of 202 cells (+11 extra next to lake shores) and
  1714 of 1730 river tiles. The saved river control splines (`River::walk_data`, `SplineData`) match ours point for point, including the points
  `ensure_ocean_end` appends, and `total_spline_length` / `depth` match to the printed digits (29729.1 / 78, 24397.44 / 66, 23747.65 / 62).
- **Goodies / goods / oil are exact** (`randommap-resources.md` section 13): `Terrain::move_ocean_goody` / `move_goody` move what stands on a cell that
  becomes half water or lies in a river trough. `current 2.SVX`: goodies 50 of 50 (33 before), goods 80 of 80, oil flags exact, rock flags 1 off
  ((16,36): a lake-shore cell the save has as plain shallow water with its derrick, see the open item below). Goods are exact on all seven worlds.
- `Doober::has_doobers` always returns 0 (`randommap-terrain-groups.md`); the rock clump "diagonal growth" was the river moving a derrick.
- The saves carry `CoordInfo.flags` for every wcoord (`TerrainList::walk_data`, `state-walk.md`): ours equal the save's in the bits that drive the
  coast generators (0x8, 0x20, 0x1000, 0x8000) on all 9999 cells; bit 4 differs on 19 cells (the half-land differences).
- `game_random` after 189 frames: `ron-mapgen --play189 <rcx> [--save svx] [--frames N]` builds the replay's game with `new_random_game` and feeds the
  commands. `Playback 2026.07.11 00'24'00` (seed 18119929): the generator makes 53769 draws and `new_random_game` 9 more; the engine then draws 19480
  numbers in 189 frames (19404 after the goody / slot changes; frames 5 to 9 alone 7000: the first AI turn), the save's `random_seed` is 11350 draws after the end of generation. So the
  engine is 8054 draws ahead of the original after the 189 frames, and the generator cannot be separated from the frame code with this oracle: the draw
  schedule of the first frames of a NEW game (not a loaded one) is the open item. No generator step the river geometry added draws.

Result of the third pass on the seven worlds (`ron-mapgen --compare`, differing cells / vertices before -> after this pass; goods and goodies are exact / save):

| | current 2 | current RN | east indies 2 | East indies final surround | DO NOT DELETE | DO NOT DELETE 2 | curent dec 14 |
|---|---|---|---|---|---|---|---|
| land_sub | 158 -> 76 | 720 -> 720 | 1230 -> 1230 | 636 -> 636 | 3509 -> 2012 | 3643 -> 1938 | 2655 -> 1461 |
| WData.flags (all bits) | 368 -> 169 | 1703 -> 1700 | 2412 -> 2410 | 1323 -> 1321 | 2220 -> 1357 | 2333 -> 1337 | 1732 -> 1079 |
| half-land 0x100 | 30 -> 30 | 63 -> 63 | 85 -> 85 | 40 -> 40 | - | - | - |
| rock 0x8 | 2 -> 1 | 0 -> 2 | 0 -> 0 | 0 -> 1 | 2 -> 0 | 0 -> 0 | 0 -> 0 |
| river cells 0x1000 | 175 -> 12 | - | - | - | 1074 -> 0 | 1202 -> 0 | 768 -> 0 |
| river tiles tdata 0x800 | 1730 -> 99 | - | - | - | 7510 -> 0 | 8462 -> 0 | 6049 -> 0 |
| goods | 78 -> 80 / 80 | 134 -> 136 / 136 | 107 -> 107 / 107 | 111 -> 112 / 112 | 22 -> 26 / 26 | 24 -> 26 / 26 | 23 -> 25 / 25 |
| goodies | 33 -> 50 / 50 | 3 / 3 | 1 / 1 | 0 / 0 | 9 / 9 | 10 / 10 | 14 -> 15 / 15 |
| shelf vertices bit-exact | 2615 -> 3145 | 6168 | 6380 | 5531 | 2648 / 2661 | - | - |
| land vertices >= 60 bit-exact | 68991 -> 70999 | 15621 | 9418 | 6003 | 71904 / 92897 | - | - |

(The old saves are late games: their goodies were picked up and their `land_sub` carries farms and roads, so only the shown cells are comparable. The "rock
0 -> 2 / 1" cells of the East Indies worlds are moved derricks whose rock flag the save lacks; same cause as the open item below.)

### Still different on `current 2.SVX`

(The shelf / half-land part of this list is solved: see the fourth pass below. The text is kept as the record of what was verified.)

- `land` 10 and half-land 30, `land_sub` 76, `val` 756, `blocked` / `bad` / `solid` 51 / 87 / 51, one rock flag: all follow from the lake-shore heights.
  Shelf: 7592 of 15381 vertices bit-exact (49 percent), mean error 2.4; sea floor 30397 of 30701. What was verified and is NOT the cause:
  `mark_valid_coastal_influencers`, `mark_coast_vert_influence`, `adjust_aquatic_height`, `relax_aquatic_verts` (5 passes: 3, 4, 6, 8 are all
  worse), `LineState`, `Fractal::init` / `get_height` (read against the asm), the 25-cell window, the generator set (CoordInfo flags equal the save's),
  the line owners, the whole sequence of `Terrain::init` between `refresh_data` and `build_coastal_data`. About one generator in five (24 of 179
  where a change is visible; they all have a diagonal first link) puts its line source at another jitter than ours: in the saved heights the
  nearest influence point sits at the generator cell plus the jitter of its LAST neighbour (`jn`) for the two cases examined closely ((42,36), (37,34))
  while the majority (155) is right with the generator's own jitter, and forcing `jn` for all, the first neighbour, the target cell's own jitter or
  a shared jitter are each much worse. The rule that selects them is not found.
- `Cliffs::init` and `TerrainOut::adjust_for_mountains` were not ported at the time (mountain heights); done in the fifth pass below, which
  also corrects the attribution of `val` / `blocked` / `bad` / `solid` to them: those are the start cities.

### Fourth pass (2026-10-24): the conquest map styles (24 / 25), the PNG reader, the lake-corner heights

**Conquest styles.** `MapConquest` (24, `CONQUESTMAP.xml`) and `MapConquestSea` (25, `CONQUESTMAPSEA.xml`) share one `make_continents` @68dba0;
the sea variant only has another style file and `world.map_style` 0x19 (`MapConquestSea::MapConquestSea` @47b300 sets the type and nothing else). Style 26
(`MapScenario`) is the campaign's custom `.scx`, loaded as a scenario by `ctw_battle.cpp` (`Game::load`), not generated. Ported in
`engine/sim/mapgen_conquest.cpp` (`Gen::mc_conquest`); the CtW layer (`ctw_battle.cpp: conquest_battle_input`) supplies the silhouettes:

- **Inputs** (`ConquestInput`, `mapgen.hpp`): the CtW layer draws the square (`conquest_draw_scenario_area`), the link's two nodes, `start_who`, the extra
  opponents and the style's `no_rare` / tileset, and keeps them alive in a `mapgen::ConquestScope` around `new_random_game`. `RandomGameSetup` clamps
  `map_style` to the shipped styles (0..23), so the battle setup keeps the pickers 1 / 2 and the scope upgrades them to 24 / 25 (`Gen::Gen`);
  `ron-mapgen --ctw` sets the style itself. Without a scope styles 24 / 25 fail with "No conquest map data available." (the original's own error).
- `get_scenario_map_rect` @75c3b0 (texture-pixel branch, read from the asm, all single precision): `dx = x1 - x2`, `dy = y1 - y2`, `wrap = (tex_width - max(x1, x2)) +
  min(x1, x2)` negated when `dx > 0`; styles 0-3 and 8 centre the square on the midpoint (`x1 - dx / 2`, or `x1 - wrap / 2` (minus `tex_width` when `x1 > x2`) when
  `|dx| > |wrap|`) with side `max(|dx or wrap|, |dy|) * 1.3f`; styles 4, 6, 7 centre on node 2 (the defended node) and 5 on node 1, side `max(...)` without the 1.3.
  The call is `get_scenario_map_rect(param_1 = link.from (the attacker's node), param_2 = link.to (the defended node))`.
- `draw_scenario_area` @761310: each COUNTRY's rectangle is `(float(attr) * map_scale) / map_scale` (init_countries stores `attr * scale` as a float in `+0x178 left /
  +0x17c right / +0x180 top / +0x184 bottom`; the `x` / `y` are `starting_coords_x / y` ints). The square's side is the larger of `(int)(bottom - top)` and
  `(int)(right - left)` (the smaller extent gets `+1.0` on its far edge). A country overlaps when none of `L > right`, `left > R`, `T > bottom`, `top > B`
  holds; otherwise it is tried again shifted by `-tex_width` (the wrap) and skipped when it still does not. Every pixel of the piece with alpha != 0
  inside the square becomes land. A country is returned (id, centre minus the square's origin) when its centre lies inside `[clip_left, clip_right) x
  [clip_top, clip_bottom)`. The pieces are 8-bit RGBA PNGs of exactly `right - left + 1` x `bottom - top + 1` pixels (all 215 files agree).
- `import_image` @9a3f30 with **percent 50** (`MOV EDX,0x32` at the call): the box of every wcoord is `[(int)(x / xs * side), (int)((x + 1) / xs * side)]` (both ends inclusive,
  float arithmetic), a pixel adds `rgb24 / 16777215`, the cell is water (`land` 2) when `sum / count >= 0.5`, else land 0, `flags &= 0xffc3`. `Buffer::get_pixel` leaves its output untouched
  outside the image, so the last box row / column re-reads the previous pixel. The start of a country is `(int)(float(cx) / side * xs)` (the world is square). Then `Regions::clear_all /
  find_all`, **`Map::force_continent_spacing` @68e6a0** and `Map::make_coastlines` (it was not in the port: land regions in ascending size, a new entry before the first of at least
  its size; every edge cell (`is_edge_of_region`: a neighbour in another region) with a land / half-land cell of another region among `move_x / y[1..48]` becomes water and leaves its
  region's list; the size drops once per offending neighbour and the next list index is skipped after a removal).
- `make_continents` then `clear_all / find_all` again, the starts, `add_additional_players`, `check_sea_map`:
  - starts: the country of `link.from` (b) first, then of `link.to` (a) (only when they are in the square and a != b): `start_who(b) = (seat 0's nation == link.opponents[0] = the defender) ? 1 : 0`,
    `start_who(a)` the other one, both swapped for conquest map styles 6 and 7; `start_capital` 1, `start_node` the node, `start_add_armies` 0. Styles 4-7 centre the square on one country, so
    the other country has **no start** (its seat gets no city; its armies enter by reinforcement).
  - `add_additional_players(link @ECX, region_b @EDX, region_a [ebp+8], a [ebp+0xc], b [ebp+0x10])` @68d260: `m = additional_opponents[difficulty] + link.supporters.count`;
    `r_near = first_city_near_coast / 4`, `r_far = city_spacing * 2 / 4`. Seat `index` (start_who = `(a != b) + 1 + index`): an index below the supporters' count is a **supporting nation**: it surrounds the
    start of the side it is allied to (start 1 = a's when its stance toward a's owner is ally, else start 0), taking the first point of circle indices `[circle_radius[r_far], circle_radius[64])` that passes
    `check_starting_loc(x, y, mindist = r_far, r = r_near, want_land, skip -1)`, `start_add_armies = link.add_supporter_armies[index]`; the others are **barbarians**: the regions under the starts
    (`[region_b, region_a]`, or `[region_a, 0]` when `region_b == 0`; -1 skipped), one `rnd(size)` draw (`Random::get(0, 0xffff) % size`, none for a region of one cell), then the
    region's cells from that index cyclically, `check_starting_loc(x, y, mindist = city_spacing, r = r_near, ...)`, armies 0. Failure only reports an error ("Unable to place ...").
  - `check_starting_loc(x, y, p3, p4, p5, p6)` @68d7e0 with the register arguments **ECX = the minimum start distance, EDX = the radius** (capped at 0x40): `want_land` (`p3` = number of sea regions != 0) rejects water
    (land 1 / 2 without 0x100) on circle indices `1 .. circle_radius[r] - 1`; `x, y >= r` and `< size - r`; `vector_dist` to every start (`p5 < 0`) or one start `>= mindist`. `p4` (rough terrain) and `p6` (a list) are 0.
  - `check_sea_map` @68d1a0: `sea_map` 0 without sea regions, else 3 when the starts are not all in one region, else 2.
- `Map::make` afterwards is the generic pipeline: `no_rare` styles skip `place_resources`, the tileset is the defended node's (the attacker's for style 5), `Setup::build_game` places seat `who` at the start with `start_who == who`.
- Only `game_random` draw of the whole path: the barbarian walk start (the tileset / `base_edge` draws of `Map::make` are the generic ones). No original recording of a conquest battle exists here: the map is
  checked by reading (`ron-mapgen --ctw CAMPAIGN:LINK --png / --image`, `re/previews/mapgen4_*.png`), not against a save.

**PNG reader** (`engine/assets/png.hpp / .cpp`): zlib `inflate`, the five filters, 1-16 bit grey / grey+alpha / RGB / RGBA / palette (+ tRNS), Adam7; output RGBA8. `ron-mapgen --png-check [DIR]` decodes every
file under `conquest/RenderMaps/MapPieces` (1564 files, 218 distinct sizes, all 8-bit RGBA non-interlaced) and checks the 215 country silhouettes against their COUNTRY rectangles (0 mismatches); two
files were also decoded independently in Python (identical RGBA hashes).

**Lake-corner shelf heights: solved.** The residue of the third pass ("a subset of the generators with a diagonal first link behaves as if it took the neighbour's
jitter") was a register effect in `TerrainOut::mark_coast_vert_influence` @868ca0, not a jitter rule. The function keeps the generator's `CoastInfo` pointer in **EBX**; the loop over the
line's points reloads EBX with the *current owner* cell (`MOV EBX,[EBP-0xc]` at 68fdf, `LEA EBX,[EAX+ECX*8]` at 68fd7), and the head of the next iteration of the two-line loop (68e80)
reads `connected_out` and `to[0] / to[1]` through EBX again. So the **second line is selected by the owner cell the first line ended in**, not by the generator:

- `src = (line == 0) ? generator : owner`; `src.connected_out == 1 && line == 1` ends the loop; `src.connected_out == 0` makes the target the generator's own cell (a G -> G line from the jitter `jc` to
  `jn`), `line == 0` takes `src.to[0]`, `line == 1` takes `src.to[1]`; the start and end offsets stay `jc` / `jn` of the generator and the cell position is the generator's (read from `[EBP-8]`).
- The owner switches only on a step that changes both `wx` and `wy`, i.e. on diagonal links. After an orthogonal first link the owner is still the generator and everything is as before; after a diagonal one it is
  the neighbour's cell, whose `connected_out` is 0 when it is a later cell of the scan (so the generator also draws a short line from `jc` to `jn` inside its own cell: the "extra point at the generator cell plus `jn`"
  of the previous pass) and its own link data when it is an earlier cell (so the second line goes to THAT cell's second neighbour).

How it was found (kept here because the method is reusable): single-generator experiments with the oracle (5656 variants of the start / end jitter source over the 707 generators of `current 2`, then a scan of one extra
influence point per generator: the best point was `generator * 4 + off(jn)` for 79 of 92 diagonal-first generators and never for orthogonal ones), then a re-read of the asm for what could add a point there.
`ron-mapgen --compare` got `--hfull FILE` (every height, save and generated), `--wfull FILE` (the differing cells) and a line "aquatic |error| sum" (sum of |generated - save| over the vertices below 60) for such searches.

Result (`ron-mapgen --compare`; differing cells / bit-exact vertices, before -> after; the three old Old World saves have no coast generators that changed):

| | current 2 | current RN | east indies 2 | East indies final surround | DO NOT DELETE | DO NOT DELETE 2 | curent dec 14 |
|---|---|---|---|---|---|---|---|
| land | 10 -> 0 | 32 -> 0 | 49 -> 0 | 20 -> 0 | 0 | 0 | 0 |
| half-land 0x100 | 30 -> 0 | 63 -> 0 | 85 -> 0 | 40 -> 0 | - | - | - |
| land_sub | 76 -> 59 | 720 -> 668 | 1230 -> 1189 | 636 -> 606 | 2012 | 1938 | 1461 |
| WData.flags (all bits) | 169 -> 132 | 1700 -> 1628 | 2410 -> 2320 | 1321 -> 1281 | 1357 | 1337 | 1079 |
| river tiles `tdata 0x800` (saved ones matched) | 1714 -> 1730 of 1730 | - | - | - | - | - | - |
| sea floor vertices | 30397 -> 30701 / 30701 | 63646 -> 64504 / 64504 | 67358 -> 68292 / 68292 | 45318 -> 45846 / 45846 | - | - | - |
| shelf vertices | 3145 -> 15384 / 15386 | 6168 -> 29790 / 29797 | 6380 -> 29482 / 29492 | 5531 -> 19710 / 19720 | 2648 / 2661 | 2952 / 2966 | 2138 / 2138 |
| shore vertices (-30 .. 60) | 9218 -> 32183 / 32610 | 4501 -> 33878 / 36554 | 3828 -> 29701 / 34660 | 3206 -> 18839 / 21883 | 7366 / 7483 | 7810 / 7927 | 6313 / 6375 |
| land vertices >= 60 | 70999 -> 72103 / 82104 | 15621 -> 15920 / 29946 | 9418 -> 9645 / 28357 | 6003 -> 6218 / 15592 | 71904 / 92897 | 70621 / 92148 | 78715 / 94528 |

Open on the height grid: the 2 shelf vertices of `current 2` that are 0.01 off and the shore vertices under river troughs (max 29, e.g. (283, 91)). (The mountains and the
cliffs were ported in the fifth pass below, which also shows that `val` / `blocked` / `bad` / `solid` come from the start cities; the two rock-flag cells were `TerrainOil::create_doobers`.)

What remains of the conquest part: no oracle (no recording of a conquest battle on this Mac), so the maps are checked by reading, not against a save; a native battle save (`ronmap` F5) of a CtW battle
carries the conquest input in its setup text since the fifth pass; style 4-7 battles leave the other
seat without a city (as the original's start lists do); `Map::force_continent_spacing` is now in the port but only the conquest path calls it (the recipes of the shipped styles do not).

### Fifth pass (2026-10-24): mountain heights, the oil terraform, cliffs; the `val` / `blocked` premise corrected; conquest battle saves

**The `val` / `blocked` / `bad` / `solid` differences are not mountains or cliffs.** The seven saves hold no cliff at all (no WData flag 0x40, no
cliff tcoord), and in `current 2.SVX` (the only fresh game, frame 189) every one of the 756 `val` differences lies 8 to 11 wcoords from a start
(`ron-mapgen --compare` compares the generator's world, which has no starting cities): `City::fix_world_vals` @735aa0 (`>>= 2` inside the city
radius, `>>= 1` on the ring beyond it, called by `Cities::init_city`; ported in `cities.cpp`) lowers `val` around every start city, and the town's
buildings, roads and cleared trees are the `blocked` / `bad` / `solid` / flag 0x80 / flag 0x4000 cells (all within 5 wcoords of a start). `ron-mapgen
--compare ... --game` (new) compares the world of `new_random_game` instead (generator + start cities):

| | current 2 | current RN | east indies 2 | East indies final surround | DO NOT DELETE | DO NOT DELETE 2 | curent dec 14 |
|---|---|---|---|---|---|---|---|
| val | 756 -> 92 | 1572 -> 1497 | 1544 -> 1513 | 825 -> 762 | 4041 -> 4000 | 3719 -> 3691 | 3023 -> 2815 |
| blocked | 51 -> 48 | 813 -> 810 | 1066 -> 1051 | 869 -> 857 | 460 -> 453 | 607 -> 598 | 555 -> 551 |
| bad | 87 -> 103 | 1498 -> 1493 | 1934 -> 1927 | 1461 -> 1447 | 767 -> 755 | 867 -> 862 | 854 -> 847 |
| solid | 51 -> 48 | 813 -> 810 | 1066 -> 1051 | 869 -> 857 | 460 -> 453 | 607 -> 598 | 555 -> 551 |
| WData.flags (all bits) | 132 -> 137 | 1628 -> 1614 | 2320 -> 2288 | 1281 -> 1259 | 1357 -> 1340 | 1337 -> 1322 | 1079 -> 1068 |

(`current 2`: 756 -> 92 `val` cells; the late-game saves keep their farms, roads, AI buildings and felled trees. The 92 left in `current 2` are the
ring of the Indians' seat: the save's city radius is 20 where `get_radius` gives 24 (`indians_city_radius`, tribe bonus 0x15), i.e. the bonus is not
active yet when the original founds the start city; forcing 20 there leaves 41 cells. `cities.cpp` / `techs.cpp` are not part of this pass. `bad`
in `current 2` is 87 -> 103: our town has other buildings than the save's after 189 frames of AI building.)

**Terrain::init's order** (`Terrain::init` @850f70), the part after the half-land check: `TerrainOil::create_doobers` @8a28e0 (oil patches) ->
`generate_skirts` -> ... -> `convert_beach_to_sandy` -> `Map::make_rivers` @692fb0 -> `RiversOut::calculate_all` -> **`TerrainOut::adjust_for_mountains(0)`
@8703c0** -> **`Cliffs::init` @8a8020** (`param_2 == 0`).

**Mountain heights** (`engine/sim/mapgen_mountains.cpp`, `Gen::mountain_heights`, called after `make_rivers`). `adjust_for_mountains(0)` walks the placed
mountains in placement order (`Mountains::add_mountain` @89c2e0 stores the stamp location `(768 wx, 768 wy)` as floats, the range index in the list
at +0xc4/+0xd0); for every face wcoord of the range (the distinct wcoords of its complete cells, list +0x50/+0x6c) it calls `TerrainOut::fill_mountain_data`
@869380 twice (the 8-pixel LOD set with `param_6 = 1`, then the 4-pixel set with `param_6 = 0`); only the second writes the heights: for each of the 5 x 5
grid vertices `(tx, ty)` of the wcoord, the template vertex with `|x + loc.x - 192 tx| < 0.01` and `|y + loc.y - 192 ty| < 0.01` that has not been
emitted for this mountain yet adds its z: `height[ty * (4 xs + 1) + tx] = height + z` (single precision; overlapping mountains add one after the other).
The template z is `MountainRange::init` @8998b0: `((float)blue + 0.0) * (height * 3.0) / 255.0` on the lattice `x = ox + 4 i`, `y = oy + 4 j`
(`ox = (W/2) mod 16`), alpha != 0, position `((x - W/2) * 192 * 0.25, (y - H/2) * 192 * 0.25)`; `height` is the `<MOUNTAIN height>` of
`effects_graphics.xml`. The same function marks the covered CoordInfo (flag 2, peak flag 0x200 with the snowcap offsets, `mountain_number`) and builds
the render face lists: renderer data, not ported.

**Oil sites are terraformed** (`TerrainOil::create_doobers` @8a28e0 -> `add_rocky_patch` @8a2b30 -> `TerrainOut::terraform_for_building` @875210; the same
function also runs from `Build::init` / `Wall::init`, so buildings level the ground in a game). `create_doobers` walks the wcoords y outer / x inner: a
wcoord that stands on open water (land 1 / 2 without 0x100) loses its rock flag 0x8 (this was the "moved derrick" rock flag of `(16, 36)`); a rock
wcoord that stays rock, or a wcoord with a good that the console leader cannot use yet (`type_avail == 0`, assumed for every good) and is not open water,
gets a rocky patch, and the patch of a wcoord with the OIL flag 0x800 calls `terraform_for_building(4 wx, 4 wy, 4, 4)`: the heights of the vertices
`[tx - 1, tx + 6] x [ty - 1, ty + 6]` except the far corner (a vertex of height <= 0 aborts), summed in the order column x1 / row y1 / block, give an average;
each tcoord `[tx - 1, tx + 5] x [ty - 1, ty + 5]` whose wcoord is not open water, with no mountain / cliff tcoord (`mask & 3` = 2 / 1) among itself and its
8 neighbours, `land == 0`, and no good on a resource tcoord (0x200), gets the average on its NW vertex, or the mean of the old height and the average on
the outer ring of the block. Result: a flat 5 x 5 plateau under every oil site (`current 2` (2, 1): 276.3 everywhere). Ported in
`mapgen_terrain_init.cpp` (`TerrainInit::create_doobers`, between the half-land check and `convert_beach_to_sandy`). The doober geometry itself uses
`internal_random`, not `game_random`: no draw.

Result (`ron-mapgen --compare`, generator only, before -> after; bit-exact vertices / total):

| | current 2 | current RN | east indies 2 | East indies final surround | DO NOT DELETE | DO NOT DELETE 2 | curent dec 14 |
|---|---|---|---|---|---|---|---|
| land vertices >= 60 bit-exact | 72103 -> 80636 / 82104 | 15920 -> 19329 / 29946 | 9645 -> 13531 / 28357 | 6218 -> 8458 / 15592 | 71904 -> 76676 / 92897 | 70621 -> 74038 / 92148 | 78715 -> 83691 / 94528 |
| shore vertices (-30 .. 60) bit-exact | 32183 -> 32504 / 32610 | 33878 -> 34175 / 36554 | 29701 -> 30216 / 34660 | 18839 -> 19281 / 21883 | 7366 -> 7432 / 7483 | 7810 -> 7862 / 7927 | 6313 -> 6375 / 6375 |
| shelf vertices (-250 .. -30) bit-exact | 15384 -> 15384 / 15386 | 29790 -> 29797 / 29797 | 29482 -> 29492 / 29492 | 19710 -> 19720 / 19720 | 2648 -> 2653 / 2661 | 2952 -> 2954 / 2966 | 2138 -> 2138 / 2138 |
| rock flag 0x8 differing cells | 1 -> 0 | 2 -> 0 | 0 -> 0 | 1 -> 0 | 0 -> 0 | 0 -> 0 | 0 -> 0 |
| WData.flags (all bits) differing cells | 132 -> 131 | 1628 -> 1627 | 2320 -> 2320 | 1281 -> 1281 | 1357 -> 1357 | 1337 -> 1337 | 1079 -> 1079 |
| game_random draws of the generator | 53769 -> 53769 | 58209 -> 58209 | 53854 -> 53854 | 42487 -> 42487 | 15661 -> 15661 | 14089 -> 14089 | 12625 -> 12625 |

What is left on the land vertices of `current 2` (1468 of 82104): 1257 lie within 2 wcoords of the save's town (buildings and roads: `terraform_for_building` of
`Build::init`, a game-time step the generator does not run), 211 up to 14 wcoords away (roads); the rest of the table is the same kind (the late games'
towns). Shelf: every vertex is exact on three of the East Indies worlds and `curent dec 14` (the ten off in the final surround and the seven in current RN
were mountain vertices over the shelf); the 2 shelf vertices of `current 2` that are 0.01 off and the shore vertices under river troughs (max 22) remain.
The generator's draw counts are unchanged on all seven worlds (mountains, the oil terraform and `Cliffs::init` draw nothing; the styles of the seven worlds
have no cliff group), so the 189-frame check of `current 2` (`--play189`) is unaffected.

**Cliffs** (`engine/sim/mapgen_cliffs.cpp`; only `SOUTHWESTMESA.xml` has cliff groups, `default.xml`'s two entries are replaced by every style's own
`<TERRAIN_GROUPS>`; there is no oracle). Data: `CliffsOut::init_templates` @8a71a0 reads `Data/Cliffs.xml` (124 `<CLIFF path orientation>`: 36 each of 8 / 16 / 32
pixels with orientation 1, 4 each of 64 pixels with orientations 1..4), the TGA of each is `D x D`, **size index** 4 / 3 / 2 / 1 for D = 64 / 32 / 16 / 8
(0 otherwise); per pixel `height = blue * 0.003921569 * 480`; the template's **cliff wcoords** (a `WCoordList`, row by row) are the 4 x 4 pixel blocks that
contain an alpha-0 pixel, or, when the image has none, the blocks whose 6 x 5 height window (x `4bx - 1 .. 4bx + 4`, y `4by - 1 .. 4by + 3`, outside = 0)
spans more than 192. Tables: `offtable` @af2fb0 = {0, -1, -2, -4, -8} (the template origin in wcoords), `index_to_size` @af2fd0 = {4, 8, 16, 32, 64} (the
side in tcoords). The `Cliffs` object keeps, per size index, three arrays (stride 0x1c, bases 0xe894b0 heights, 0xe8953c orientations, 0xe895c8
WCoordLists) and the placed list (16-byte records `x, y, size, template`, 0xe89654).
- `place_player_group` @6a4190 (cliff branch): size index `s = shape - 1` (`local_54`); `mode = (s == 4)`; unless `s == 4` the candidate must pass
  `verify_defensive_position(c, start, cliff_face)` @8a8680: facing 2 (`FACE_ANY`) passes; facing 0 compares the direction start -> `(xs/2, ys/2)` with
  start -> c: normalised in single precision (`1 / sqrtf`), `acos` in double, `|angle| < 22.5` degrees; any other facing (1, and the default -1) uses the
  integer mean of the starts and `17.5`. Then `position_cliff(c, s, start, mode, cliff_face)`.
- `drop_tile` @6a2ab0 case 8 (region groups): `position_cliff(x, y, shape - 1, -1, -1, 0, FACE_WORLD_CENTER)`; `place_all` @6a70d0 passes
  `shape = size` for the continent pattern and `max(size - 1, 1)` for the world patterns, so the size index is `size - 1` there and `size - 2` in the
  world patterns (size index 0 has no template: such a cliff always fails).
- `position_cliff` @8a8bc0: wanted orientation: mode 0 with a start: NW 1 / NE 2 / SE 3 / SW 4 by the sign of `y - sy` and `x - sx` (the table {1, 2, 4, 3} is
  indexed by `(dy < 0 ? -2 : 0) + (dx >= 0 ? 1 : 0)` from -2); mode 0 without a start: any; mode 1 with `facing < 2` (unsigned compare): the same
  quadrant rule on `(xs/2 - sx, xs/2 - sy)`; mode 1 otherwise: orientation 1 only. Every template of the size with that orientation that passes
  `verify_placement` @8a6310 is a candidate; **one `game_random` draw `Random::get(0, 0xffff) % n` when there are several**, none for one; no candidate
  returns 0; `add_cliff` @8a6690 places it.
- `verify_placement(x, y, s, i, mode)`: for each cliff wcoord `(x + off + lx, y + off + ly)`: off the map fails in mode 0 and is skipped in mode 1; the first
  on-map cell passes at once in mode 1; else none of the 48 cells around may have a cliff flag 0x40 (the first 8 not a forest 0x20 either), and the cell
  itself no mountain / forest / rock / oil, not open water, no river 0x1000, no cliff, `blocked == 0`.
- `add_cliff`: records the placement and sets the cliff flag 0x40 on every template wcoord that `TerrainOut::is_clear_land` @874b60 accepts with mask 2
  (mode 0) or 0x22 (mode 1) (no mountain / forest / rock / oil / open water / river / cliff (0x22 lets a cliff stand) and `blocked == 0` unless the mask has
  one of 0x165). Bit 7 of the mask turns the goody / good tests ON in that function (`TEST BL,BL; JNS`): `TerrainInit::is_clear_land_oil` in
  `mapgen_terrain_init.cpp` applies them when the bit is CLEAR; its oil results are exact on the seven worlds, so it was left alone.
- `Cliffs::init` @8a8020 (end of `Terrain::init`): `place_cliff` @8a7ea0 adds the template heights (index running over the `D x D` pixels of every placed
  cliff in order, even when off the map) on mountain-free, non-ocean, non-river, non-coastal (CoordInfo bit 4) wcoords; then `assess_wcoord` @8a6a40 for every
  wcoord with flag 0x40 (y outer): each of its 16 tcoords on a wcoord that is not coast (0x4), not land 1 / 2 and not a mountain tcoord, and whose four corner
  heights differ by more than `tan(25 deg) * 192` (`TerrainOut::has_plus_slope` @869790), becomes a blocked cliff tcoord (`mask = (mask & ~2) | 1`) and so do
  its 8 neighbours (whose wcoords' goods go through `Terrain::move_goody`); a flagged wcoord with no such tcoord loses 0x40.
  Not ported: `Cliff::init` (render pieces, `CliffMiningData`) and `Cliffs::set_edge_flags` (TData 0x8000 on the mining tcoords).
- Southwest Mesa, size 3, 4 nations: seed 3: 19 cliffs, 500 cliff wcoords, 7157 cliff tcoords; seed 5: 14 / 370 / 5340. `ron-mapgen --all`: ok 882, error 0,
  nondeterministic 0. Unverified against an original recording (none exists on this Mac).

**Conquest battle saves.** A native save (`ronmap` F5) of a Conquer the World battle on a generated map regenerates the map from the scenario slot, as
every random game does. The slot now carries what the battle builder gave `new_random_game`: `RandomGameSetup` has `ctw`, `conquest` (the encoded
`mapgen::ConquestInput`: square side, land bitmap as run lengths, the countries' centres, nodes, `start_who`, supporters, barbarians, tileset,
`no_rare`; about 2.7 kB for a 315 x 315 square) and `battle_runtime` / `battle_setup` (the style's scripts, compiled after the base scripts exactly as
`build_battle` does, so the loaded game has the same compiled script files), text keys `ctw=1;conq=...;btrt=...;btsu=...`. `new_random_game` opens a
`ConquestScope` from `conquest` when none is open. The ronmap battle keeps the placeholder scenario path `ctw:battle`; `scenario_rel_path` (save.cpp)
swaps it for the text `build_battle` registered (`battle_setup_text()`, newgame.hpp). The binary save layout is unchanged (no `kSaveVersion`
bump); older builds refuse the new keys. `ronmap --ctw-selftest` checks the text round trip, a battle save -> load -> save byte identity, the equal map
and that the same text without the conquest input regenerates a different map. Still not carried: the campaign link (`ScriptHost::ctw` is a run-time
link a native save never writes), so a resumed battle reports its outcome to no campaign; custom-map (`.scx`) battles. `ronmap`'s F9 compares the
save's scenario slot with `_scenarioPath` (`ctw:battle`) before loading and would refuse such a save: `main.mm` should store
`random_setup_string(extras.setup)`-style text (or `battle_setup_text()`) in `_scenarioPath` for a battle.
