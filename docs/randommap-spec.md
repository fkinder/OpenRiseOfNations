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

- `land` 10 and half-land 30, `land_sub` 76, `val` 756, `blocked` / `bad` / `solid` 51 / 87 / 51, one rock flag: all follow from the lake-shore heights.
  Shelf: 7592 of 15381 vertices bit-exact (49 percent), mean error 2.4; sea floor 30397 of 30701. What was verified and is NOT the cause:
  `mark_valid_coastal_influencers`, `mark_coast_vert_influence`, `adjust_aquatic_height`, `relax_aquatic_verts` (5 passes: 3, 4, 6, 8 are all
  worse), `LineState`, `Fractal::init` / `get_height` (read against the asm), the 25-cell window, the generator set (CoordInfo flags equal the save's),
  the line owners, the whole sequence of `Terrain::init` between `refresh_data` and `build_coastal_data`. About one generator in five (24 of 179
  where a change is visible; they all have a diagonal first link) puts its line source at another jitter than ours: in the saved heights the
  nearest influence point sits at the generator cell plus the jitter of its LAST neighbour (`jn`) for the two cases examined closely ((42,36), (37,34))
  while the majority (155) is right with the generator's own jitter, and forcing `jn` for all, the first neighbour, the target cell's own jitter or
  a shared jitter are each much worse. The rule that selects them is not found.
- `Cliffs::init` and `TerrainOut::adjust_for_mountains` are not ported (mountain height faces, `has_plus_slope`): the remaining land-height
  differences (11105 of 82104 vertices) and the 51 `blocked` cells.
