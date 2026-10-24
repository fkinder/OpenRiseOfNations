# World grid spec: the simulation grid units move on and buildings occupy

Implementation spec for the **simulation side** of the map: `World` / `WorldData` (`world.obj`), connectivity regions
(`regions.obj`), building footprints (`BuildType::mask_me`), unit occupancy (`CollBlock`, `collcheck.obj`) and the
passability queries. Rendering of the same map is in [terrain-spec.md](terrain-spec.md); the file format of the
terrain chunk is in terrain-spec §A, objects in [scenario-objects-spec.md](scenario-objects-spec.md), building masks in
[rules-spec.md](rules-spec.md) §8.6. Status key: ✅ verified on real data · 🟡 read from code · ❓ open.

**Reference tool:** `re/tools/worlddump.mjs` builds the grid from a scenario exactly as the load path below does
(terrain chunk → mountains → `compute_all_vals` → `adjust_for_blocking` → regions → goods → buildings → unit footprints)
and renders tcoord-resolution PNGs.

```
node re/tools/worlddump.mjs scenario/Tutorial1/tut1.scx --png re/previews/world_tut1     # summary + _pass/_sea/_regions.png
node re/tools/worlddump.mjs conquest/Alexander/granicus.scx --json out.json               # WData/TData arrays as JSON
```

Everything below is integer arithmetic unless stated. "Trunc" is C division (toward zero); `>>` on negative numbers is
arithmetic (floor).

---

## 1. Coordinate types ✅

All positions are `int` **world units** (`Coord`). Grids are derived by floor division:

| Type | Cell (world units) | From a Coord | Grid size | Used for |
|---|---|---|---|---|
| `Coord` | 1 | — | `xs·768 × ys·768` | object positions |
| `UCoord` | 48 | `div_3_table[x >> 4]` | `16·xs` | unit footprints, `CollBlock` bits |
| `TCoord` | 192 | `div_3_table[x >> 6]` | `tile_xs = 4·xs` | `TData`, blocking, buildings |
| `FCoord` | 384 | `x >> 7` / `tx >> 1` | `fog_xs = 2·xs` | fog of war `seen*` bytes |
| `WCoord` | 768 | `div_3_table[x >> 8]` | `xs` | `WData`, regions, territory |
| region cell | 1536 | `div_3_table[x >> 9]` | `reg_xs = (4·xs) >> 3` | per-player `danger` arrays |

- `div_3_table` (`init_coord_lookup_array` @00681db0) is a pointer into the middle of a table: `t[i] = i/3` for i ≥ 0
  and `t[i] = trunc((i−2)/3)` for i < 0, i.e. **`floor(i/3)` everywhere**. So `TCoord = floor(x/192)` etc., including
  negative x. Range ±24·n with n = 400 at startup, then `4 × largest map size` (`Game::init_rules_data`). A port can just
  floor-divide.
- Conversions between grid types are shifts: `tcoord >> 2 = wcoord`, `ucoord >> 2 = tcoord`, `ucoord >> 4 = wcoord`.
  `WCoord::get_tcorner` = `4w` (NW tcoord), `WCoord → TCoord` operator = `4w + 2` (the centre tcoord), wcoord centre in
  Coords = `768w + 384`.
- `WCoord::traverse_x/y(i)` (i = 0..15) give the i-th tcoord of a wcoord: x = `4w + (i & 3)`, y = `4w + (i >> 2)`.
  **Every "16 tcoords of a wcoord" loop in the game uses this order** (row-major, x fastest).
- Bounds: `WorldData::is_valid(Coord)` is `0 ≤ x < tile_xs·192`; `restrict(Coord)` clamps to `[0, 768·xs − 1]`.

**XOR obfuscation (irrelevant to a port).** Every object's position is stored in memory XORed with a constant:
`SubObjectData.x_internal` (+0x10), `y_internal` (+0x14) and `z_internal` (+0xc) are `value ^ 0x63637`. Other keys
(`0x63187`, `0x62766`, `0x8221`, `0x1281`, `0x69587`, …) obfuscate leader economy fields (see state-spec). The save
stream and the checksum walk hold plain values; nothing in the grid itself is obfuscated. Guys (`GuyData.x/y` +0xc/+0x10)
are not XORed.

## 2. Data layout ✅ (PDB)

### 2.1 `World` (global `world`, 0x174 bytes; `WorldData` is the first 0x16c)

`World::init(xs, ys)` @006b76f0 (after `close`):

| Field | Value |
|---|---|
| `xs, ys`, `size` | args, `xs·ys` |
| `tile_xs, tile_ys, tile_size` | `4xs`, `4ys`, product |
| `fog_xs, fog_ys, fog_size` | `(8xs) >> 2`, `(8ys) >> 2`, product |
| `reg_xs, reg_ys, reg_size` | `(4xs) >> 3`, `(4ys) >> 3`, product |
| `sea_map` | −1 |
| `player_territory_limit{,_civic,_city}`, `colonized_territory_limit{…}` | `constants.territory_limit_base / _civic / _city` (both triples) |
| `player_reg, resource_reg, land_size, seed` | 0 |
| `wdata` | `size` × `WData` (constructor defaults below) |
| `tdata` | `tile_size` × u16, zero |
| `seen, seen2, seen3` | `fog_size` bytes each, zero (per-player bit masks) |
| `wcoord_seen` | `size` bytes, zero |
| `danger[8]` | 8 × `reg_size` i32, zero |
| start/oil `SimpleArray<WCoord>`s | emptied; `start_city_locs` = `DynamicBitMask(size)` |

It also closes the doobers and re-inits `Borders` and `GameDaemon`.

### 2.2 `WData` (0x1c bytes per wcoord, row-major `y·xs + x`)

| Off | Field | Default (`WData::WData` @006b2310) | Meaning |
|---|---|---|---|
| 0x00 | u16 `flags` | 0 | `FLAG_*` below |
| 0x02 | i8 `land` | **2** (ocean) | `LandIndex`: 0 land, 1 sandy (shallow), 2 ocean |
| 0x03 | u8 `land_sub` | 0 | base-land subtype (texture) |
| 0x04 | i16 `region` | 0 | connectivity region (§5) |
| 0x06 | i16 `region2` | 0 | sea region of a half-water wcoord's water tcoords |
| 0x08 | i16 `down`, 0x0a `down_who` | −1, 0 | head of the per-wcoord object list (§7.4) |
| 0x0c | u8 `val` | 0 | AI site value (§3.3) |
| 0x0d | u8 `goods` | 0 | nearby-resource bits (§3.3) |
| 0x0e | u8 `light` | 0 | |
| 0x0f | i8 `who`, 0x10 `who2` | −1, −1 | territory owner (§9) |
| 0x11 | u8 `blocked` | 0 | number of BLOCKED tcoords in this wcoord |
| 0x12 | u8 `bad` | 0 | number of BAD_PATH tcoords |
| 0x13 | i8 `solid` | 0 | number of BLOCKED tcoords that are not trees |
| 0x14 | u8 `was_seen` | 0 | per-player bits |
| 0x18 | `CollBlock*` `block` | null | unit occupancy bitmap (§7) |

**WData flags** (PDB unnamed enum `FLAG_GOODY`):

| Bit | Name | Set by |
|---|---|---|
| 0x0001 | RESOURCE | `Good::init` (centre wcoord of a good) |
| 0x0002 | DEAD_BUILD (alias RIVER_VALLEY) | `Build::close` (building destroyed) |
| 0x0004 | COAST | file flag 0x08 |
| 0x0008 | ROCK | file flag 0x01 |
| 0x0010 | MOUNTAIN | `Mountains::add_mountain`, `eliminate_dead_mount_spots` (file flag 0x02 is ignored) |
| 0x0020 | FOREST | file flag 0x04 |
| 0x0040 | CLIFF | file flag 0x40 |
| 0x0080 | ROAD | `set_road_at` (any road tcoord in the wcoord) |
| 0x0100 | HALFLAND (half water, "shoreline") | file flag 0x80 |
| 0x0200 | NEAR_BLOCKING | `adjust_for_blocking` (3×3 around mountain/forest/cliff wcoords) |
| 0x0400 | ORIGINAL_COAST | file flag 0x08 |
| 0x0800 | OIL | file flag 0x10 (answers terrain-spec open question 8) |
| 0x1000 | RIVER | file flag 0x20; `set_river_at` |
| 0x2000 | RIVERBED (alias TEST_FOG) | |
| 0x4000 | BUILDING | `mask_me` (centre wcoord of a building) |
| 0x8000 | GOODY | `Item::init` (ruins / goody box) |

`(flags & 0x70) != 0` ("impassable wcoord": mountain, forest or cliff) is used by `is_passable`, `is_impassable`,
`map_is_passable` and the wcoord-level path queries; `(flags & 0x78) == 0` is `buildings_allowed`.

### 2.3 `TData` (u16 `mask` per tcoord, row-major `ty·tile_xs + tx`)

PDB unnamed enum `MASK_GATHER_EDGE`. Bits 0–1 and 4–5 are **two-bit fields**:

| Bits | Value | Name | Meaning |
|---|---|---|---|
| `mask & 3` | 1 / 2 / 3 | CLIFF / MOUNTAIN / BUILDING | 3 is also "MERGED" |
| 0x0004 | | TEMP_BEHIND | NW of a building (occlusion; `mark_behind_tiles`) |
| 0x0008 | | PERM_BEHIND | tree tcoord, or NW/N/NE of a mountain tcoord (occlusion) |
| `mask & 0x30` | 0x10 / 0x20 / 0x30 | ROAD / WATER / TREE | |
| 0x0040 | | STARTED2 | second overlapping building foundation |
| 0x0080 | | STARTED | building foundation (placed, not yet started) |
| 0x0100 | | CITY | inside a city radius (`Wall::mask_city`) |
| 0x0200 | | RESOURCE | under a good |
| 0x0400 | | COASTAL | file tcoord bit 0x20 |
| 0x0800 | | RIVER_B | file tcoord bit 0x02 |
| 0x1000 | | GATHERED | resource tcoord being gathered |
| 0x2000 | | BAD_PATH | next to a BLOCKED tcoord, map-edge water, water near a dock |
| 0x4000 | | BLOCKED | impassable for ground units |
| 0x8000 | | GATHER_EDGE | tree/mountain/cliff tcoord with an open orthogonal side |

Predicates (`WorldData::*`): `is_tocean` = `(m & 0x30) == 0x20`; `is_tree_at` = `== 0x30`; `is_mountain_at` = `(m&3) == 2`;
`is_cliff_at` = `== 1`; `is_built_at` = `(m&3) == 3 || (m & 0x80)`; `is_blocked_at(…, 0)` = `m & 0x4000`,
`is_blocked_at(…, 1)` = blocked and not a tree; `is_river` = `m & 0x800`; `is_gathered_from` = `m & 0x1000`.
`is_ocean(wcoord)` = `!(flags & HALFLAND) && land ∈ {1, 2}`.

### 2.4 Neighbour tables (`compass.obj`) ✅ (read from the exe)

- **`move_x/move_y`** @00adcaf0/@00adc400, a square spiral of 441 entries (rings 0–10; `radius[r] = (2r+1)²`).
  Index 0 = (0,0); **1–8 = NW, N, NE, E, SE, S, SW, W** (y grows southwards); 9–24 = ring 2 in the irregular order
  (−1,−2) (0,−2) (1,−2) (2,−1) (2,0) (2,1) (1,2) (0,2) (−1,2) (−2,1) (−2,0) (−2,−1) (−2,−2) (2,−2) (2,2) (−2,2);
  25–48 = ring 3 starting at (−3,−3) and going clockwise. Rings 8–10 contain data typos; ship the table verbatim
  (only rings ≤ 7 are used by collision, more by the pathfinder and map generator).
- **`orthog_x/orthog_y`** @00add250/@00add210: index 0 = (0,0), **1–4 = N, E, S, W**.
- **`circle_x/circle_y/circle_radius`** (`circle_init` @006817f0): integer "circles" r = 0…64. Ring r holds the
  offsets (x outer from −r to r, y inner from −r to r) whose approximate distance equals r, with
  `d(ax, ay) = ay < ax ? ay²/(2ax) + ax : (ay == 0 ? 0 : ax²/(2ay) + ay)` (unsigned integer division; ax = |x|, ay = |y|).
  `circle_radius[r]` = cumulative count after ring r, so ring r is `[circle_radius[r−1], circle_radius[r])` and
  "everything within r" is `[0, circle_radius[r])` (ring 0 = the single point (0,0)). Capacity 0x3249 points.
- `even_circle_*` (`even_circle_init` @006816d0): the same idea with `round(sqrtf(x² + y²))` (round half down) on a
  half-tile lattice; used by `Wall::mask_city` (§6.4).

## 3. Building the grid from a scenario 🟡 (order from `ScenarioRead::import` @009b1430)

| # | Step | Function | Grid effect |
|---|---|---|---|
| 1 | `Objects::clear`, `Regions::clear_all` | | all regions empty, `land = 0`, `sea = 0x40` |
| 2 | terrain chunk children in file order | `load_terrain_chunk` @009aedf0 | §3.1, §3.2 |
| 3 | end of the terrain chunk | `World::compute_all_vals` @006b3790 | `val`, `goods` (§3.3) |
| 4 | | `World::adjust_for_blocking` @006b3230 | trees, NEAR_BLOCKING, regions, dead mountain spots, edge BAD_PATH, GATHER_EDGE (§3.4) |
| 5 | only if no heights stored | `Map::make_coastlines` | (never for stock files) |
| 6 | | `ScenarioEditor::init_terrain` @009a19d0 | **`game_random := GameInfo.seed`**; `Regions::clear_all` + `find_all`; `World::compute_all_territory`; `Terrain::init`; rivers, cliffs, mountains overlay (render) |
| 7 | items (chunk 3), goods (4), objects (5), orders (6), cities (7) | `load_*_chunk` | goods, buildings, units (§3.5) |
| 8 | | `World::compute_all_territory` @006b5700 | territory with the buildings in place (§9) |

`init_terrain` is called with CL = 1 when heights are stored (all stock files): the fractal "land 1 → 2" pass and the
half-land region patch in `Terrain::init` are skipped. Steps 2–4 run with whatever `game_random` holds at that point;
step 6 resets it, so draws made in step 3 do not shift the post-load stream (they do decide `WData.val`).

### 3.1 Terrain chunk → WData / TData ✅

`World::init(xs, ys)` from the wcoord header, then per wcoord (`load_wcoord_chunk3` @009ae370): `flags = 0`, then map
the file flags (`FILE_FLAG_*` enum): 0x04 → FOREST 0x20, 0x01 → ROCK 0x08, 0x08 → COAST|ORIGINAL_COAST 0x404,
0x20 → RIVER 0x1000, 0x10 → OIL 0x800, 0x40 → CLIFF 0x40, 0x80 → HALFLAND 0x100; FILE_FLAG_MOUNTAIN 0x02 is ignored.
`land`, `land_sub` copied; `land == 1 && land_sub != 0` is fixed to `land_sub = 0` ("Bad WCoord found and fixed").
Per tcoord (`load_tcoord_chunk2` @009ae1f0): `mask = 0`, then file 0x10 → WATER (`(m & ~0x30) | 0x20`),
0x20 → COASTAL 0x400, 0x02 → RIVER_B 0x800. The tcoord chunk precedes the mountains chunk in every file, which matters
because it overwrites masks.

Rivers (chunk 92), cliffs (96), heights (89), doobers (73) and coord flags (99) are only stored for later; none of them
touches `tdata`/`wdata` here (rivers are already in the RIVER/RIVER_B bits; no stock file has cliffs).

### 3.2 Mountains ✅ (`Mountains::add_mountain` @0089c2e0, `MountainRange::init` @008998b0)

Each chunk-69 stamp `{wx, wy, range_index, num_coords}` is checked (`num_coords` must equal the template's face-wcoord
count, terrain-dressing-spec §1.1) and placed. With the loader's arguments all validation radii are 0, so no other
check applies. A template (`TEMPLATE_TEX`, W×H pixels, 48 world units per pixel, `ox = (W/2) mod 16`, `oy` likewise,
`hw = W/2 − ox`, `hh = H/2 − oy`) yields two lists besides its render mesh:

- **Mountain tcoords:** one per *complete cell* of the 4-pixel vertex lattice (all four corner samples opaque), offset
  `(trunc(x/4) − trunc(hw/4), trunc(y/4) − trunc(hh/4))` for the cell's top-left sample (x, y), in face order (rows,
  then columns). This is `buildRange().cells` in `re/tools/dressing.mjs`.
- **Mountain wcoords:** scan 16×16-pixel blocks with top-left (x0, y0), `y0 = oy, oy+16, …` while `y0 + 16 < H`, x0
  likewise (rows outer). Sample alpha at the 5×5 points `(x0 + 4i, y0 + 4j)`, i, j = 0…4. The block counts when all 25
  are opaque **or at least 16** are; its offset is `(trunc(x0/16) − trunc(hw/16), trunc(y0/16) − trunc(hh/16))`. (This is
  a different list from the face-wcoord list used for `num_coords` and rendering.)

Placement, in order:
1. For each mountain wcoord `(wx+dx, wy+dy)`: `flags = (flags & ~0x3c) | (flags & 0x404) | MOUNTAIN` (clears FOREST,
   ROCK; keeps COAST).
2. For each mountain tcoord `(4wx+dx, 4wy+dy)` in list order: `set_blocked_at(t, on)` (§4), then `mask = (mask & ~1) | 2`.
3. `Mountains::set_behind_flags` @00898050: for each mountain tcoord, its NW, N and NE neighbours (move 1–3) that are
   in the map and not mountain get PERM_BEHIND 0x08.
4. The stamp is appended to the mountains list (used again in §3.4 step 5).

### 3.3 `compute_all_vals`: WData.val and WData.goods 🟡

`World::compute_all_vals` @006b3790 runs `compute_val` @006b0350 for **x outer, y inner** and stores `val` (clamped
0…255) and `goods`. For an open-water wcoord (`is_ocean`) both are 0 and nothing else happens. Otherwise:

1. Over the circle rings 3 and 4 (`[circle_radius[2], circle_radius[4])`), every in-map cell first adds 1 to `count`.
   If its centre tcoord `(4x+2, 4y+2)` is GATHERED, nothing else happens (it stays counted as land; asm @006b0451
   increments before the test). Otherwise: add the cell's MAKE goods to per-good counters `make[g]` (g = 0…5 except
   3), using the gather-land index `get_land(w, 1)` @006b4730 (COAST → 3; FOREST → 4; MOUNTAIN|CLIFF → 5; ROCK → 6, or
   7 with OIL; open water with OIL → 7; else `land`) and the rules `<LANDS>` table (`Lands::init` @0067e730: land 0
   makes Knowledge and Food, 4 Timber, 5 Metal, 7 Oil, 8 Metal, the rest nothing); a HALFLAND or open-water cell
   then moves from `count` to **water**. (No tcoord is GATHERED at scenario load, so this detail only matters for
   later calls.)
2. Over the 25 cells of `move[0..24]` (5×5): off-map → `count −= 4`; a cell whose gather-land index is 4 or 5 (i.e.
   non-COAST with `flags & 0x70`) → `count −= 2`; a HALFLAND or open-water cell → `count −= 1` (both can apply).
3. `v = trunc(count/4) + Random::get(game_random, 0, 0xffff) % 10` (**one sim-RNG draw per non-water wcoord**).
4. If `trunc(count/5) < water < trunc(count/3)`: `v = trunc(3v/2)`. If `water ≥ trunc(count/3)`: `v = trunc(v/10)`.
5. If `make[Timber] > 4`: `v *= min(trunc(make[Timber]/4), 5) + 1`. If `make[Metal] != 0`: `v = trunc(3v/2)`.
6. With `rs` = size of the wcoord's region: `rs < trunc(size/128)` → `v = trunc(v/2)`; `rs < trunc(size/64)` →
   `v = trunc(3v/4)`; `rs < trunc(size/16)` → `v = trunc(3v/4)`. **During a scenario load every `WData.region` is 0
   and region 0 is empty** (step 1 of §3), so `rs = 0` and all three apply on maps with ≥ 16 wcoords.
7. `goods = 0x20 | (make[Food] > 4) | (make[Timber] > 4) << 1 | (make[Wealth] > 4) << 2 | (make[Metal] > 4) << 4`.

`val` is AI-only (city site selection) but it is part of the saved and checksummed `WData`. ❓ The value of
`game_random` at `load_terrain_chunk` time (seed plus any draws made earlier in `Game::run`/`Game::init`) is not pinned
down; `worlddump.mjs --seed` takes it as a parameter.

### 3.4 `World::adjust_for_blocking` @006b3230 ✅ (counters self-consistent on all 156 stock `.scx`)

Five steps, in this order:

1. **Forests and halo** (y outer, x inner): for every wcoord with `flags & 0x70`: OR NEAR_BLOCKING into itself and its
   8 neighbours (move 0–8, in-map). If it is FOREST: for its 16 tcoords (traverse order) `set_blocked_at(t, on)`,
   then `solid −= 1` if the tcoord was not yet a tree, then `mask |= 0x38` (TREE + PERM_BEHIND).
2. **Regions**: release every land region (0–63) whose flags have RECALC 0x40 (no writer found: in practice none),
   then `Regions::find_all` (§5).
3. **`Terrain::eliminate_dead_mount_spots`** @0084fd90: two passes, each y outer / x inner over MOUNTAIN wcoords, with
   one visited bitmap shared by both passes (sized `xs·xs`, indexed `y·xs + x`). For each of the 9 cells `c` of
   move 0–8 around the mountain wcoord that is in the map and not yet visited: if **no** in-map orthogonal neighbour of
   `c` lacks both FOREST and MOUNTAIN (`flags & 0x30`), then each of `c`'s 16 tcoords that is not BLOCKED gets
   `set_blocked_at(t, on)` and `c.flags = (flags & ~0x3c) | MOUNTAIN` (so enclosed land becomes a mountain). Mark `c`
   visited either way. On tut1 this fills 65 wcoords, on granicus 280 (most are mountain interiors).
4. **Edges and forest borders** (y outer, x inner), per wcoord:
   - If it is on the map border (x = 0, y = 0, x = xs−1 or y = ys−1), not HALFLAND and `land ∈ {1,2}`: each of its
     tcoords with `tx < 2 || ty < 2 || tx > tile_xs−3 || ty > tile_ys−3` gets BAD_PATH (counted in `bad`). Ships keep
     two tcoords off the map edge.
   - If it is FOREST and its east neighbour exists and has neither FOREST nor MOUNTAIN: for the east tcoord column
     (tx = 4x+3, ty = 4y…4y+3): if tree, clear the tree bits and `solid += 1`; clear PERM_BEHIND; unless the tcoord is
     mountain, `set_blocked_at(t, off)`. Then the same for the south neighbour and the south row (ty = 4y+3).
     Forests therefore have a walkable seam along their open east and south sides.
5. **`World::set_edge_flags`** @006b0240: GATHER_EDGE 0x8000 on every tcoord that `has_gather_access(t, −1, 0, 1)`:
   first the tcoords of every placed mountain (`Mountains::set_edge_flags` @0089c1a0, stamp order, template order), then
   cliffs (`Cliffs::set_edge_flags`), then the 16 tcoords of every FOREST wcoord (y outer, x inner).
   `WorldData::has_gather_access` @006b4e50 in this mode: the tcoord is tree, mountain or cliff, and some in-map
   orthogonal neighbour is neither water nor BLOCKED.

### 3.5 Objects ✅/🟡

**Items** (`Item::init` @006770e0): `FLAG_GOODY` on the item's wcoord. Nothing blocks.

**Goods** (`Objects::init_good` → `Good::init` @0066da20): skip Oil (it is the wcoord OIL flag). Otherwise, with
`x0 = tcoord(cx) − trunc(x_size/2)`, `y0` likewise (resource rules X_SIZE/Y_SIZE), for x in `[x0, x0+x_size)` outer,
y inner (no bounds check): `mask |= RESOURCE`, and if the good type's `block` (+0x2f0, rules) is set,
`set_blocked_at(t, on)`. Then `FLAG_RESOURCE` on `wcoord(cx, cy)`. Goods are pre-snapped
(`GoodTypeData::snap_center` @0066fcb0: `768w + 288`, +96 when x_size is even, on both axes).

**Buildings and cities** (objects chunk 26 and cities chunk 27, file order): §6.

**Units**: `Object::add_to_world` marks the unit footprint in `CollBlock`s (§7). Units do not touch `tdata`.

## 4. Primitive writers ✅

All counters are per wcoord of the tcoord: `w = wdata[(ty>>2)·xs + (tx>>2)]`.

**`World::set_blocked_at(tx, ty, on)`** @006b4900:
- on: if not BLOCKED, `blocked += 1`, `solid += 1`; set BLOCKED; if BAD_PATH was set, clear it and `bad −= 1`;
  `set_road_at(t, off)`; then each in-map neighbour (move 1–8) gets BAD_PATH (`bad += 1` if new), **even if it is
  itself BLOCKED**.
- off: if BLOCKED, `blocked −= 1`, `solid −= 1`; `mask &= 0x3fff` (**also clears GATHER_EDGE**); each in-map neighbour
  without a BLOCKED neighbour of its own (`has_blocked_neighbors` @006b2990, move 1–8) loses BAD_PATH; finally the
  tcoord itself gets BAD_PATH if it has a BLOCKED neighbour.
- The result is **order dependent**: inside a solid block each newly blocked tcoord clears its own BAD_PATH and sets its
  neighbours', so the last-blocked corner of a block ends up without BAD_PATH while the others keep it (visible in
  `worlddump` output: `0x4003` vs `0x6003` inside a city footprint). Keep every loop order exactly.

**`set_tree_at(t, on)`** @006b2060: on → if not tree `solid −= 1`; `mask |= 0x38`. off → if tree, clear 0x30 and
`solid += 1`; clear PERM_BEHIND. Blocking is separate.

**`set_road_at(t, on, a4, quiet)`** @006b43b0: on → `mask = (mask & ~0x20) | 0x10` (replaces water/tree bits), WData
ROAD, and `Roads::road_added` unless it was a road already or `quiet`. off → if road, clear 0x30 (and
`Roads::road_cleared` unless quiet); then clear WData ROAD if none of the wcoord's 16 tcoords is a road (this runs even
when the tcoord was not a road).

**Others**: `set_bad_path` @006b4610 (bit + `bad` counter), `set_mountain_at` @006b1f00 (blocked + `(m & ~1) | 2`;
off clears low bits only if 2), `set_cliff_at` @006b1eb0 (blocked + `(m & ~2) | 1`), `set_building_at` @006b45c0
(`|= 3` / clear if 3), `set_city_at` (0x100), `set_started_at` (0x80), `set_started2_at` (0x40), `set_resource_at` (0x200),
`set_gathered_at` (0x1000), `set_gather_edge` (0x8000), `set_behind(t, on, perm)` (0x8 or 0x4), `set_tocean` /
`set_waterhalf` (WATER; the latter also sets HALFLAND on the wcoord), `clear_waterhalf`, `set_coastal` (0x400),
`set_river_at` (0x800 on the tcoord, RIVER on the wcoord; off clears RIVER when no tcoord of the wcoord keeps 0x800),
`set_land(w, land, sub, feature, keep_coast)` @006b2b50 (`flags = (flags & ~0x3c) | feature`, feature 4 also sets
ORIGINAL_COAST), `set_oil_at`, `set_down`.

Writers of BLOCKED (callers of `set_blocked_at`): `mask_me`, `Good::init/close`, `Mountains::add_mountain` / `remove` /
`clear`, `Cliffs` (assess/delete), `adjust_for_blocking`, `eliminate_dead_mount_spots`, `TerrainGroup` (map
generator), the scenario editor, and **`Unit::close`, `SpellType::cast_pack` / `cast_unpack`** (units that deploy,
❓ not decoded).

## 5. Connectivity regions ✅ (`regions.obj`)

Regions are **landmasses and water bodies**, not obstacle-aware connectivity: a wcoord is *water* when
`is_ocean` (land 1/2 and not HALFLAND), otherwise *land* (half-water wcoords count as land). Trees, mountains and
buildings play no part. Regions only change during map creation and scenario import; in a game they are fixed (saved
by the `Regions` walk, hashed by `CheckSums::check_regions` @00936f70).

**Storage.** 128 `Region` records (stride 0x88): 0–63 land (0 never assigned, 0x3f overflow), 0x40–0x7f sea (0x40
never assigned, 0x7e overflow). Fields used here: `region` (own index), `size` (wcoords), `rank`, `flags`
(`REGION_MERGE 1, NO_RARE 2, PLAYER 4, RESOURCE 8, GATHER_RECOMPUTE 0x10, GATHER_VERIFY 0x20, RECALC 0x40,
ANYBODY_HERE 0x80`), `coastal` (`BitMask<64>` over land regions), `coast` (`BitMask<64>`, bit k = sea region 0x40+k),
`coords` (list of wcoords). `Regions.sea` is the sea-id counter, `Regions.land` is always 0.

**`Regions::clear_all`** @00680060: all regions `flags = climate = fertile = 0`, `common_factor = goody_factor = 8`;
non-empty ones also lose size, coords, `borders` and `border_id`; every `WData.region = 0` (**`region2` is not
cleared**); `land = 0`, `sea = 0x40`.

**`Regions::find_all`** @0067eff0:
1. Raster scan, y outer, x inner, over wcoords with `region == 0`. Let `t` = water-ness.
   - Join: the first of move 1–8 that is in the map, has the same `t` and a region ∉ {0, 0x7f} gives its region
     (`size += 1`). Never fires in a full rebuild.
   - Otherwise a new id: land → the first `i` in 1…63 with `size == 0`, clamped to ≤ 0x3e; water → `++sea`, clamped
     to 0x40…0x7d (so the first sea is **0x41**). Flood-fill it (`find` below), `climate = 0`.
   - Overflow (id reached 0x3e / 0x7d): fold the smallest region (running minimum below the overflowing one's size,
     scanning from 0 / 0x40) into the overflow bucket and relabel. Because region 0 / 0x40 are always empty, the first
     overflow always picks them; for land this goes badly wrong (relabels unvisited cells), but needs 62 land bodies.
     Stock maps reach at most 16 land and 10 sea regions.
2. `region2` for HALFLAND wcoords with `region2 == 0` (x outer, y inner): the region of the first of move 1–8 that is
   in the map, not HALFLAND and `land ∈ {1,2}`; stays 0 if there is none.
3. `set_coastals`, `sort_regions`, `rebuild_coords`.

**`Regions::find(x, y, id)`** @00680180: breadth-first fill, a FIFO of wcoords (circular, `size + 1` slots), neighbours
move 1–8 with the same water-ness and `region == 0`; `size` = cells popped. **8-connected**: diagonal contact joins two
bodies, so two lakes touching at a corner are one sea, and two land masses touching diagonally are one region.

**`set_coastals`** @0067fd70: clears `scouted`, `coast`, `coastal` for regions 0–126. Then raster scan (y outer):
for each land cell (region < 0x40, region 0 included) and each in-map 8-neighbour in a sea region s: `coast |= s`,
and for every cell at move 9–48 (Chebyshev ring 2 and 3 around the **land cell**) in another land region L:
`coastal` gets L on both sides. Then `Region::finalize_coastals` @006805b0 for regions 0–63: region j joins `me.coastal`
when `me` is coastal to i, i is coastal to j and all three touch a common sea; a newly added j ≤ i restarts the scan at
j. Exact loop in the regions decompile notes; reproduced in `worlddump.mjs`.

**`sort_regions`** @0067fb90: rank = position in a stable sort by descending size (ties by ascending index), separately
for land 0–63 and sea 0x40–0x7e. **`rebuild_coords`** @0067f800: each region's `coords` = its cells in raster order
(y outer); a count mismatch asserts "Region Size mismatch".

**Queries.**
- `WorldData::get_tregion(tx, ty)` @006b52e0: `region2` when the wcoord is HALFLAND and the tcoord is WATER, else
  `region`. This is the region of a *tcoord*: on a shoreline wcoord, land tcoords are in the land region and water
  tcoords in the adjacent sea. The pathfinder treats "same `get_tregion`" as a precondition for a path; collision filters
  by it (§7).
- `Region::is_coast(r)` @00680f90: same region → 1; land/sea pair → the land region's `coast` bit for the sea; else 0.
  Transport logic looks for a sea S with `is_coast(S, src) && is_coast(S, dst)`.
- `Region::coast_here(r, x, y)` @00681020: direction (1–8) from wcoord (x, y) to an adjacent tcoord region matching the
  other side of a land/sea pair, or 0. `num_coasts` counts adjacent seas. `World::is_edge_of_region` @006b3180: any
  8-neighbour in a different region. `Regions::get_num_sea` @0067eed0: non-empty sea regions.

Regions do not include rivers (river tcoords are not water) and are computed before the river splines are rebuilt.

## 6. Buildings on the grid 🟡

### 6.1 Snap (`BuildTypeData::snap_center(cx, cy, &ox, &oy, who)` @00636190)

1. Oil wells / platforms (`is(OILWELL 421)` / `is(OILPLATFORM 422)`): if `wcoord(cx, cy)` has OIL, the centre becomes
   `(768wx + 384, 768wy + 384)`; otherwise the first of move 1–8 with OIL that `was_really_seen` by `who`.
2. Docks, and mines / woodcutters of a real player (`who ≥ 0`): if `blocked_site` at the tcoord centre fails, try move
   1–80 for the first candidate tcoord with `blocked_site == 0` (only the candidate's y is bounds-checked).
3. Final snap per axis: `ox = tcoord(cx)·192 + (x_size odd ? 96 : 0)`, same for y. No clamping.

The footprint is `x0 = tcoord(ox) − (x_size >> 1)`, `y0` likewise, size `x_size × y_size` tcoords
(`tile_corner` @00636440; inverse `corner_tile` @006364c0: `cx = (w + 2x0)·96`). All 13,371 stock building and city
records are already snapped (`worlddump` re-snaps none of them). At scenario load nothing is explored, so step 2 cannot
succeed for mines and woodcutters (blocked_site returns 36 for > half unseen tiles); ❓ docks skip that rule.

### 6.2 Life cycle

1. `Objects::init_build` → `Build::init` @00629740 → `Wall::init` → **`Wall::start_me(1)`** @0063d790: each footprint
   tcoord (x outer, y inner, in-map) gets STARTED, or STARTED2 if STARTED was already set. No blocking.
2. Construction start (`Wall::start` @0063e810, reached through the first builder, or `Build::activate` → `Wall::activate`
   for scenario buildings with `is_active = 1`): set object flag 2; `kill_competing_buildings` (disbands unstarted
   buildings overlapping on STARTED2 tiles); `Terrain::object_placed`; **`mask_me(1, REGEN_FORCE)`**; OR `1 << who` into
   `WData.was_seen` and `seen2` for every footprint tcoord; `check_ever_seen`; `mark_behind_tiles(1)` @0063d230
   (TEMP_BEHIND 0x4 for k = 1…`behind_height`: row `y0−k`, x in `[x0−k, x0−k+w)`, and column `x0−k`, y in
   `(y0−k, y0−k+h)`).
3. Death / removal (`Build::close` @00628980 → `Wall::close` @0063f6e0): DEAD_BUILD on the centre wcoord unless
   captured (reason 5); `mark_behind_tiles(0)`, `start_me(0)`, and if started and not captured `mask_me(0, REGEN_SIMPLE)`.
   Closing a city re-runs `mask_me(1, REGEN_NONE)` on every live active city (restores overlapping CITY bits). Capture
   and upgrades re-mask without unmasking first.

`MaskMeType` (COULDBE / TOBE / TRUE) is not used by this build: the "to be" state is STARTED/STARTED2.

### 6.3 `BuildType::mask_me(cx, cy, radius, add, …, regen)` @006312a0

1. No mask → error, nothing else.
2. Centre wcoord: FLAG_BUILDING set (add) or cleared (remove, unconditionally).
3. Footprint, x = 0…w−1 outer, y = 0…h−1 inner, in-map tcoords only, `v = mask[w·y + x]` (masks.txt, row-major):
   - clear STARTED and STARTED2;
   - add: `mask |= 3`; if the type is neither a city (`is(VILLAGE 414)`) nor `connects_to_roads`, `set_road_at(off)`;
     remove: clear the low bits if they are 3;
   - `v == 1` → `set_blocked_at(t, add)`; otherwise, on add only: `set_blocked_at(t, off)` and, for cities and
     road-connecting types, `set_road_at(t, on)`.
4. Docks (`is(DOCK 432)`): every in-map WATER tcoord in `[x0−3, x0+w+3) × [y0−3, y0+h+3)` (x outer) gets BAD_PATH
   (`set_bad_path(add)`); on removal `Docks::remask_docks` re-applies the other docks.
5. Cities: `Wall::mask_city(tcoord(cx), tcoord(cy), add, radius)` @0063e310 sets/clears CITY 0x100 on the first
   `even_circle_radius[radius]` points of `even_circle_x/y`, with `radius` from `CityData::get_radius` /
   `LeaderData::get_radius`: 20 / 24 / 28 tcoords by city size, Indians +4, **not** tech dependent (production-spec
   §5.1; not reproduced in the tool).
6. `BuildType::place_roads` @0063c580 (skipped on add with REGEN_NONE): for cities and road-connecting types the ring of
   the box `[x0−1, x0+w−1] × [y0−1, y0+h−1]` — its right and bottom sides are the footprint's own last column and row —
   gets `set_road_at(on)` on every in-map tcoord that is not BLOCKED, not water, and either is outside a building/
   foundation or is not on the box's left/top side. `'e'`-flag buildings that belong to a city get the ring just outside
   the footprint; with REGEN_FORCE a road is pathed to the city (`PathFinder::find_road`, reproduced in engine/sim/roads.cpp: movement-spec 12, original-save 6.4); `'g'`
   types get none.

`connects_to_roads` @006398c0 = not (`'g'` 0x40 or `'e'` 0x10 in BUILD_FLAGS) unless `is(UNIVERSITY 420)`.
Mask cell values in `masks.txt` are only 0 and 1: "solid" masks block everything; "extra space" masks (cities, economic
buildings, wonders) leave the last column and row unblocked, which carries the building's own road; farms
(`4x4 gather S`, all 0) and woodcutters (`2x2 gather`) block nothing; `4x4 oil` blocks the corners and the centre 2×2.

### 6.4 Placement validity (`BlockIndex`)

`BuildTypeData::blocked_site(cx, cy, who, o_self, *gather)` @00636a50 returns the first failing `BLOCKED_*` code
(0 = ok). Per-tcoord rules (`blocked_tcoord` @00636db0), first hit wins: off map 34; WData GOODY → 4 RUINS;
`(m&3) == 3` → 1 BUILDING; land building on WATER → 14, sea building off water → 15; BLOCKED tree → 6, BLOCKED
mountain (or MOUNTAIN wcoord) → 2, other BLOCKED → 7; RESOURCE → 7; oil types need OIL (5), others refuse ROCK (3);
an own STARTED foundation → 1; outside any CITY radius without `'e'` → 31; city/fort territory rules → 23/26;
`'k'` → 43; RIVER_B → 44; gather types on a land without their good → 9. Each code is masked to 36 (never seen) or
35 (seen, not "really seen") when the player has not explored the tile. Then the corner-level `blocked_location`
@006375b0 (territory, city/fort spacing in tregion with the octagonal distance `max + min²/(2·max)` in tcoords,
town radius, one-per-city, farm limit, wonders, water counts for docks via `count_water` / `check_land_adjacent`,
adjacent mountains/trees for cities and forts, gather amounts). `ObjectTypeData::blocked_site` @00661d70 is a stub
returning 0. Placement validity only gates commands (the UI and the AI); it does not change grid state.

## 7. Unit occupancy (`CollBlock`) 🟡

There is **no per-tcoord unit grid**. Units occupy a per-wcoord bitmap of ucoords, allocated on demand:

- `WData.block` → `CollBlock` = `BitMask<768>` (bits, size 0x60, flags, 96 bytes). **Only bits 0…255 are used**:
  bit `i = (ux mod 16)·16 + (uy mod 16)` (x-major), byte `i >> 3`, mask `1 << (i & 7)` (`CollBlock::get/set`
  @00681e30/@00682070). The other 64 bytes stay zero but are saved (World walk part 9).
- `flags` caches emptiness (1 = known empty, 0 = has bits, 2 = unknown); it has no observable effect.
- `World::new_coll_block` @0046d250 allocates a zeroed block. **`GameDaemon::process_coll_blocks`** @00731f90 runs every
  frame: `n = max(trunc(xs/4), 5)` wcoords starting at `game_daemon.empty_colls` (saved), freeing empty blocks, wrapping
  at `size`. Block existence is observable (below), so this schedule must be exact.
- **Footprint:** a unit with `new_block_radius` r > 0 (rules `<BLOCK_RADIUS>`, ucoords; 0–7 in stock data), not AIR,
  occupies the `(2r+1)²` ucoords around the ucoord of its guy 0, **filtered per cell** to wcoords whose `region` equals
  the guy's `get_tregion` (so a boat on a shoreline does not mark land cells). Writers: `Object::add_to_world` @0064d8c0 /
  `remove_from_world` @00647970 (all guys up to `guy_mark`; in practice 1), `Guy::process` (re-stamps every 64 frames
  when idle), `Objects::kill_guy` (clears unless the type blocks while dead), `DeathObj::clear_blocking` @008d4ac0, and
  `CollCheck::move_unit` @00682ad0 (on each ucoord change: clear old-only cells, set new-only cells; fast path inside one
  block does nothing when that block does not exist).
- **Queries:** `CollCheck::fill_slots` / `collide_here` @006820e0 / @00682540 test a unit's footprint against the up to
  4 blocks it overlaps (stride-2 lattice sampling, own footprint excluded, a leading-edge fast path with a reproduced
  original bug); the pathfinder snapshots blocks into a per-search overlay tree. Used by `Unit::detect_unit_collision`,
  `resolve_unit_collision`, `Objects::find_collision`, `PathFinder::valid_ucoord`. Details belong with unit movement.
- Object lookup per wcoord: the doubly linked list `WData.down/down_who` → `ObjectData.up/down` (by XORed position
  `>> 8`), used to find *which* unit a hit belongs to.

## 8. Passability 🟡

The core predicate is **`UnitData::invalid_loc(tx, ty, p3, p4, p5, p6, p7)`** @00607c30 (0 = valid, 1 = off map,
2 = terrain, 3 = water hazard, 4 = blocked). With `t = tdata`, `tt = t & 0x30`, `wf = wdata.flags`,
`forest = unit_masks2 & UNIT2_MOVE_IN_FOREST (0x4000)`:

| Domain | Rule |
|---|---|
| any | off the tcoord map → 1. If `p4` and the owner is human and none of the wcoord's 4 fog cells was ever seen → 0 (unexplored counts as passable) |
| SEA (1) | water tcoord: warships (attack ≠ 0, or the aircraft carrier) get 3 on BAD_PATH or COASTAL; non-water: 2 unless (`p6`‖`p7`) and the ship can transport and the wcoord is not mountain/forest/cliff |
| GROUND (0) | `p3 && p6` and `wf & 0x70` (forest only excused for forest walkers) → 2; tree (unless forest walker), mountain or cliff tcoord → 2; water tcoord → 2 unless boarding a transport |
| AIR (2) | always 0 |
| then | not `p3`, BLOCKED (trees excused for forest walkers), and the unit's own tcoord is not BLOCKED (lets units walk out of buildings): 4, except own buildings for attackers with `p5` (garrison/attack logic) |

Building footprints are not tested as such: the BLOCKED bit carries them (mask value 1); open building tiles
(value 0) and roads are walkable. BAD_PATH is a hint for ground units (pathfinder cost, `resolve_unit_collision`)
and a hazard only for warships.

Wrappers: `PathFinder::valid_tcoord` @00687d60 (`p3=0, p4=1, p5=1, p6=1`), `valid_ucoord` @00687c80 (tcoord rule plus
`detect_unit_collision`, cached per search), `valid_wcoord` @00687da0 (`p3=1`: wcoord-level, so whole forest/mountain
wcoords are out; hazard 3 forgiven at the goal wcoord), `PathFinderData::valid_roadcoord` @00688740 (caravans: roads,
own city walls). Scripts: `map_is_passable(tx, ty)` @009e4de0 = `(wdata.flags & 0x70) == 0` (−1 off map).
`WorldData::is_passable` / `is_impassable` / `is_pass_land` are wcoord flag tests with no callers.

## 9. Territory, fog, danger (summary) 🟡

- **Territory**: `WData.who` (owner) and `who2`, computed per land region by `World::compute_reg_territory(region, 1)`
  @006b0bb0 from cities and forts (influence with `constants.territory_*`, city level, capital and wonder/tribe
  bonuses, border techs), driven by `compute_all_territory` @006b5700, which first zeroes each active leader's per-region
  territory counters, sets `who = who2 = −1` on every sea-region wcoord, and finally totals each leader's territory.
  `GameDaemon::check_borders` recomputes regions whose `borders < size` during play. The influence algorithm is
  specified in production-spec §5.10.1 (C++ `engine/sim/territory.cpp`). `compute_all_territory` runs twice at load:
  in `ScenarioEditor::init_terrain` (no objects yet) and after the objects (§3 steps 6 and 8).
- **Fog**: `seen`, `seen2`, `seen3` (per fog cell, bit per player: really seen / was seen / detected),
  `wcoord_seen` per wcoord, `WData.was_seen`. `is_seen(fx, fy, who)` honours reveal-map and allied vision.
- **Danger**: `danger[player][reg cell]` i32 per 1536-unit cell (AI threat map).

## 10. What is saved and hashed

`World::walk_data` (state-walk.md): `xs, ys`; start and oil lists; the scalars `[0x8, 0x80)`; per wcoord the first 0x15
bytes of `WData` (flags … was_seen; not `block`); `tdata`, `seen`, `seen2`, `seen3`; `wcoord_seen`; `danger[8]`; per
wcoord a has-block flag and the `CollBlock` (all 96 bytes); `Terrain` half-land lists and nuke hits. This is checksum
part 12 (world). `Regions` (sea, land, 128 records, coords) are saved and checked by `check_regions`, but not in the
15 CHECK_SUMS parts. So every bit in §2 matters for multiplayer sync, including the occlusion bits (TEMP/PERM_BEHIND),
`val`/`goods` and the counters.

## 11. Verification

- `worlddump.mjs` builds all 156 stock `.scx` without errors; on every file `Σ WData.blocked` equals the number of
  BLOCKED tcoords and `Σ WData.bad` the number of BAD_PATH tcoords (the counters and the bit updates agree).
- tut1: 4 mountain stamps, 967 mountain and 1942 tree tcoords, 3121 blocked; regions L1 (474 wcoords, coasts on seas
  0x41 and 0x42), S41 (69), S42 (33); 8 buildings, 17 units on the map. granicus: 11 stamps, 6 land and 2 sea regions,
  86 buildings, 20 goods (3 blocking).
- `re/previews/world_tut1_pass.png` / `world_granicus_pass.png` (ground: water blue, trees dark green, mountains brown,
  blocked building tiles red, open building tiles pink, roads brown, bad path tan, unit footprints magenta),
  `*_sea.png` (warship hazards: map-edge band, coastal tcoords), `*_regions.png` (`get_tregion` per tcoord: shoreline
  wcoords split between land and sea). They line up with the terrain renders (`*_terrain.png`).
- Not yet checked against the game's own state: there is no save of a stock scenario in the repo. ❓ Decode the World
  walk of a frame-0 save (`savdump.mjs`) of tut1 and diff it with `worlddump --json`; that would confirm everything in
  §3 bit for bit.

## 12. C++ implementation

`engine/sim/` (namespace `ron::sim`) ports this spec and `worlddump.mjs`:

| File | Contents |
|---|---|
| `compass.{hpp,cpp}`, `compass_table.inc` | coordinate conversions (floor division), `WCoord`/`TCoord`/`UCoord`, `move_x/y` (441 entries verbatim), `orthog_*`, `circles()` |
| `random.hpp` | `Random`: the game's LCG (`get(lo, hi)`, `get_float`, `reseed`) |
| `world.{hpp,cpp}` | `World` (`init`, WData/TData, fog/danger arrays), predicates, the primitive writers (§4), `CollBlock` occupancy: `coll_get/coll_set`, `stamp_footprint`, `process_coll_blocks` (§7; blocks live in a pool, `WData.block` is an index) |
| `regions.{hpp,cpp}` | `Regions` (`clear_all`, `find_all` with overflow, coastals, ranks, coords, `is_coast`) |
| `mountains.{hpp,cpp}` | template tcoord/wcoord lists from `TEMPLATE_TEX` (§3.2) |
| `placement.{hpp,cpp}` | goods, items, `snap_center`, `start_me`, `mask_me`, `place_roads`, `mark_behind_tiles`, `wall_start`, unit footprints |
| `passability.{hpp,cpp}` | `invalid_loc` (§8) with a `Mover` and callbacks for fog and building owners; `valid_tcoord`, `valid_wcoord`, `map_is_passable` |
| `world_load.{hpp,cpp}` | the load path of §3: `load_terrain`, `add_mountains`, `compute_all_vals`, `adjust_for_blocking`, `place_objects`, `build_world` |

`build/ron-world <game dir> <scx> [--seed n] [--no-objects] [--json out]` prints worlddump's summary and writes its
JSON. `re/tools/world-compare.sh [game dir] [--seed n]` diffs both (JSON byte for byte, summary text) over the 156 stock
`.scx`: **identical for all 156**, with seed 0 and seed 12345. `ron-check` builds every grid and checks the counters
and the block collector. Deliberate differences from the JS (none reachable with stock data at load):
`compute_val` counts GATHERED ring cells as land (§3.3, from the asm); `mask_me` does nothing for a type without a mask
(the original reports an error and returns); `eliminate_dead_mount_spots` sizes its visited map for the whole grid (the
original's `xs·xs` bitmap overruns when `ys > xs`); mountain and good writes outside the map are skipped and counted
(the original writes out of bounds). Not ported: `Wall::mask_city` (CITY bits), `Docks::remask_docks`,
`PathFinder::find_road` (REGEN_FORCE), the `Roads` notifications of `set_road_at`, territory (§9), dock/mine snapping.

## 13. Open questions

1. `game_random` at `load_terrain_chunk` (decides `WData.val`): which draws precede scenario import in `Game::run` /
   `Game::init`?
2. The template wcoord list rule (≥ 16 of 25 opaque samples) is read from code only; no file stores FLAG_MOUNTAIN to
   compare with (file flag 0x02 is always 0 in stock scenarios).
3. Dock snapping at load (§6.1) and whether `game.semaphore[1] & 8` (editor) is ever set while importing for play.
4. City radius for CITY bits (`LeaderData::get_radius` / `CityData::get_radius`), `place_roads` with REGEN_FORCE
   (`PathFinder::find_road`), `Roads::road_added`, `Terrain::object_placed` / `terraform_for_building` (heights).
5. `Unit::close`, `SpellType::cast_pack/unpack`: which tcoords deploying units block.
6. `REGION_RECALC` has no writer; the release loop in `adjust_for_blocking` is probably dead.
7. Territory influence (`compute_reg_territory`) and `GameDaemon::check_borders` timing.
8. Guy 0 vs unit position for CollBlock stamping, and whether the pathfinder overlay trees (`UnitData.blocklist`,
   `validlist`) survive save/load.

## Evidence

All addresses in `riseofnations.exe`. Decompiles: `re/decomp/grid/` (World, WorldData, Regions, CollBlock/CollCheck,
BuildTypeData, PathFinder valid_*), `re/decomp/grid_misc/` (compute_all_vals, WData ctor, has_gather_access,
mountains, eliminate_dead_mount_spots, circle_init, init_coord_lookup_array, Good/Item init, asm of `ScenarioRead::import`
and `ScenarioEditor::init_terrain`), `re/decomp/grid_regions/`, `re/decomp/grid_build/`, `re/decomp/grid_coll/`;
job files `re/jobs/grid_*.txt`.

| Topic | Functions |
|---|---|
| Layout, init | `World::init` 006b76f0, `WData::WData` 006b2310, `World::close` 006b7b50, `World::walk_data` 006b5cf0; PDB enums `MASK_GATHER_EDGE` (TData), `FLAG_GOODY` (WData), `FILE_FLAG_RIVER` (file bits), `LandIndex`, `DomainIndex`, `BlockIndex`, `REGION_MERGE` |
| Coordinates | `init_coord_lookup_array` 00681db0 (callers `Main::init_menus` 00595be0, `Game::init_rules_data` 0058ec60), `Coord::operator TCoord/WCoord` 0046cd40/00461460, `WCoord::get_tcorner` 0046f080, `traverse_x/y` 0046eef0/0046eed0, `WorldData::is_valid` 0043f360/0046f760/00471170, `restrict` 006b50a0/006b53a0 |
| Tables | `move_x` 00adcaf0, `move_y` 00adc400, `orthog_x` 00add250, `orthog_y` 00add210, `circle_init` 006817f0, `even_circle_init` 006816d0 |
| Load order | `ScenarioRead::import` 009b1430 (Regions::clear_all @009b21b4, load_terrain_chunk @009b2265, adjust_for_blocking @009b2314, init_terrain @009b2355, compute_all_territory), `load_terrain_chunk` 009aedf0, `load_wcoord_chunk3` 009ae370, `load_tcoord_chunk2` 009ae1f0, `load_mountains_chunk` 009adf40, `ScenarioEditor::init_terrain` 009a19d0 (game_random reseed at 009a1a48) |
| Mountains | `Mountains::add_mountain` 0089c2e0, `set_behind_flags` 00898050, `set_edge_flags` 0089c1a0, `MountainRange::init` 008998b0 |
| Values | `World::compute_all_vals` 006b3790, `compute_val` 006b0350, `WorldData::get_land` 006b4730/006b4c70/0046f9d0, `Lands::init` 0067e730, `Types::good_key` 006691c0 |
| Blocking | `World::adjust_for_blocking` 006b3230, `set_blocked_at` 006b4900, `WorldData::has_blocked_neighbors` 006b2990, `set_tree_at` 006b2060, `set_road_at` 006b43b0, `set_bad_path` 006b4610, `set_edge_flags` 006b0240, `WorldData::has_gather_access` 006b4e50, `Terrain::eliminate_dead_mount_spots` 0084fd90, `Cliffs::set_edge_flags` 008a85e0 |
| Regions | `Regions::clear_all` 00680060, `find_all` 0067eff0, `find` 00680180, `set_coastals` 0067fd70, `Region::finalize_coastals` 006805b0, `sort_regions` 0067fb90, `rebuild_coords` 0067f800, `Region::release` 00680560, `is_coast` 00680f90, `coast_here` 00681020, `WorldData::get_tregion` 006b52e0, `World::is_edge_of_region` 006b3180 |
| Goods, items | `Good::init` 0066da20, `Good::close` 0066d860, `GoodTypeData::snap_center` 0066fcb0, `Item::init` 006770e0 |
| Buildings | `BuildTypeData::snap_center` 00636190, `tile_corner` 00636440, `corner_tile` 006364c0, `BuildType::mask_me` 006312a0, `place_roads` 0063c580, `connects_to_roads` 006398c0, `Build::init` 00629740, `Wall::start_me` 0063d790, `Wall::start` 0063e810, `Wall::mask_me` 00642fc0, `mark_behind_tiles` 0063d230, `Wall::mask_city` 0063e310, `Build::close` 00628980, `Wall::close` 0063f6e0, `blocked_site` 00636a50, `blocked_tcoord` 00636db0, `blocked_location` 006375b0 |
| Units | `CollBlock::get/set` 00681e30/00682070, `World::new_coll_block` 0046d250, `GameDaemon::process_coll_blocks` 00731f90, `CollCheck::fill_slots` 006820e0, `collide_here` 00682540, `move_unit` 00682ad0, `Object::add_to_world` 0064d8c0, `remove_from_world` 00647970, `DeathObj::clear_blocking` 008d4ac0 |
| Passability | `UnitData::invalid_loc` 00607c30, `PathFinder::valid_tcoord` 00687d60, `valid_ucoord` 00687c80, `valid_wcoord` 00687da0, `PathFinderData::valid_roadcoord` 00688740, `ScenarioFuncSet::map_is_passable` 009e4de0, `WorldData::is_passable` 006b23c0 |
| Territory | `World::compute_all_territory` 006b5700, `compute_reg_territory` 006b0bb0 |
