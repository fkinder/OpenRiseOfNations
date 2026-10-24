# Random maps, part 4: terrain groups and the per-style continent recipes

Status 2026-10-24: 🟡 decoded from the decompiles (`terraingroups.obj`, `map.obj`, `mountains.obj` for two helpers) with the
lossy places checked in the disassembly (`Map::point`, `stamp`, `large_stamp`, `land_dist`, `invert_land`, `fix_lakes`,
`grow_region`'s return paths, `check_player_forest`, `fix_transitions`, the `place_all` switch, `World::wipe`, `project`,
the Outback counters). Nothing is validated against a running game (no recording of an original random map exists on this
Mac). Where the decompiler dropped a loop counter or a branch and the disassembly was not read, the text says ❓.
Part 1 is `randommap-spec.md` (coordinates, `rnd(n)`, WData / Region layouts, `Map::make`, the base generator,
`grow_region`, the Mediterranean recipe). This file does not repeat it; "§P1 n" points there. Rivers and resources
(`randommap-rivers.md`, `randommap-resources.md`) run after the terrain groups.

Notation as in part 1: `rnd(n)` = `Random::get(game_random, 0, 0xffff) % n`; `rnd&k` = the same draw masked (the
sign-fix idiom `& 0x80000001` is just `& 1`). `vd(dx, dy)` is the game's distance (`vector_dist`, same code in `circle_init`,
`grow_region`, the placers): `a = |dx|`, `b = |dy|`; if `b < a`: `a == 0 ? 0 : (b < 60000 ? a + b²/(2a) : (b + 2a) >> 1)`;
else `b == 0 ? 0 : (a < 60000 ? b + a²/(2b) : (a + 2b) >> 1)` (unsigned integer arithmetic). **Every draw below is on
`game_random` and must be reproduced in order.**

## 0. Tables and flags the passes share

- `move_x/move_y[0..8]` (static data): index 0 = (0,0), then the eight neighbours clockwise from the north-west: (−1,−1),
  (0,−1), (1,−1), (1,0), (1,1), (0,1), (−1,1), (−1,0). `orthog_x/y[1..4]` = the four orthogonal neighbours, `corner_x/y[1..4]` =
  the four diagonals (`TerrainGroup` keeps rotatable circular lists `orthog_xlist/ylist`, `corner_xlist/ylist` of these, built
  once in the `TerrainGroup` constructor @6a5df0), `corner_box_x/y[0..3]` = (0 or 1, 0 or 1): the map's four corners are
  `(bx·xs, by·ys)` (not `xs−1`).
- `circle_init` @6817f0 builds the offset tables once: for radius r = 0, 1, 2 … 64, in the order x = −r..r (outer), y = −r..r
  (inner), append `(x, y)` when `vd(x, y) == r`; `circle_radius[r]` = number of offsets so far. So entry 0 is (0,0), ring r (r ≥ 1)
  is indices `[circle_radius[r−1], circle_radius[r])`, "within radius r" is `[0, circle_radius[r])`. At most 0x3248 points.
- `project(cx, cy, angle, r, &x, &y)` @92d000: the angle is a 32-bit fraction of a turn (2³² = 360°, 0 = north, clockwise).
  `x = cx + sin_table(angle, r)`, `y = cy − sin_table(angle + 0x40000000, r)` (= `r·sin`, `−r·cos`). `sin_table` @a46a00 is a 256-entry
  table with linear interpolation, result `(table·r) >> 16` (arithmetic shift; the `r ≥ 0xffff` branches scale by 8 bits first).
  Angle steps for n players are `0xffffffff / n` (unsigned division).
- `WData.flags` bits used here (u16): 0x04 coastline (set by `make_coastlines`), 0x08 rock group tile, 0x10 mountain tile, 0x20
  forest tile (part 1 called 0x30 "blocked": it is mountain | forest), 0x40 ❓ (neighbours of it keep forest groups away: the river / bed
  bit written by the river pass), 0x100 half-land (river / coast cell), 0x400 set together with 0x04, 0x800 oil on this cell
  (`World::set_oil_at(x, y, 1)`), 0x1000 ❓ (blocks doobers and group placement). Byte +0x11 of a WData must be 0 for a group
  tile ❓ (a resource mark; always 0 at this stage). "Water" = not 0x100 and `land` ∈ {1, 2}; `land_sub` (+3) is the land subtype
  0..3 (0 fertile … 3 arid; §A1).
- A forest tile always gets `land = 2 if (flags & 4) else land`, `flags = (flags & 0xffc3) | 0x20` (this clears 0x04 0x08 0x10 0x20);
  a rock tile the same with `| 0x08`. (A coastline-flagged cell becomes `land 2`: the coastline cells are water in the
  decompile's reading; ❓ whether `make_coastlines` already did that, §P1 7.2.)

# A. Terrain groups

## A1. The data: `GROUPENTRY` → `TerrainGroup`, `TILESET_DATA`, tileset group data

`TerrainGroups::init_terrain_data(n)` @6a6540 clears the group list, then reads every `<GROUPENTRY>` of `<TERRAIN_GROUPS>` (the
style's, else `default.xml`'s). Per entry a `TerrainGroup` (0x9c bytes; ctor @6a5df0 zeroes +0x4..+0x63 and builds the shared
orthog / corner lists) is filled. `scalevalue(attr)` is `Map::scale_number` (§P1 4): the attribute text "N [AREA|SCALE|…]",
any other word after the number (`mindist`, `forestspace`, `min`, `max`, …) is a comment and ignored; a **negative** N means
`|N| × n` (n = `init_terrain_data`'s argument, the player count ❓ passed in ECX).

| attribute | field (offset) | notes |
|---|---|---|
| `type` | `type` (+4) | a land name (`Lands::find_key`) → that index (gather land groups, unused by the shipped files); `trees` 4, `mountains` 5, `rocks` 6, `oil` / `sea oil` 7, `cliffs` 8. A leading `if mapsize >N` / `<N` (`mapsize` compared with the game's map size) drops the entry when false; no shipped file uses it ❓ (the decompile's control flow around it is confused) |
| `chance` | `chance` (+8) | integer percent, default −1 |
| `grouping` | `grouping` (+0xc) | §A2 step 2 |
| `min_clumps` / `max_clumps` | +0x10 / +0x14 | scaled |
| `pattern` | `pattern` (+0x18) | `player` 0 (also the default), `continent` 1, `world` 2, `nonplayer` 3, `corner` 4 (**never placed**: the `place_all` switch only handles 0..3, §A2) |
| `min_size` / `max_size` | +0x1c / +0x20 | scaled, except for `mountains` where they are the raw 1 / 2 / 3 = small / medium / large |
| `cliff_face` | +0x5c | only for type 8; −1 default |
| `city_keep_away` / `city_stay_near` | `start_min` (+0x24) / `start_max` (+0x28) | scaled; `start_max` clamped to ≤ 0x40 |
| `forest_space` / `mount_space` / `rock_space` / `coast_space` | +0x2c / +0x30 / +0x34 / +0x38 | scaled, clamped to 0..0x40 |
| `cent_keep_away` / `cent_stay_near` | `cent_min` / `cent_max` (+0x3c / +0x40) | `cent_max` ≤ 0x40 |
| `edge_keep_away` / `edge_stay_near` | `edge_min` / `edge_max` (+0x44 / +0x48) | `edge_max` ≤ 0x40 |
| `corner_keep_away` / `corner_stay_near` | `corner_min` / `corner_max` (+0x4c / +0x50) | `corner_max` ≤ 0x40 |
| `min_oil` / `max_oil` | +0x54 / +0x58 | only read for type 6 |
| (constructor) | `touching_mountain` (+0x60) = −1, `placed` (+0x64, SimpleArray<int>), `tiles` (+0x80, WCoordList) | |

`TerrainGroups::init_tileset_data(is_default)` @6a61f0: on the default pass the per-land `subtype_freqs[land]` arrays are sized
to `tileset.num_tiles[land]` (lands: 0 fertile / baseland, 1 coastal, 2 ocean; `eTILE_FERTILE/COASTAL/OCEAN`). For each
`<LANDKEY name frequency_0..k>` the frequencies (percent) fill `subtype_freqs[Lands::find_key(name)][k]` (k < `num_tiles`; the
shipped files only give baseland four values and ocean one). Then `Fractal::init(xs, ys, smooth = tileset group_data
.clump_factor, p4 = world, flags 0, seed = world.seed)` (own `Random`, §P1 5: no `game_random` draw) and the **partitions**
(`Array<byte>`): for i = 0 .. `subtype_freqs[0].count − 2`: `acc += (int)(freq[0][i] / 100.0f · 255.0f) & 0xff; push(acc)`.
The tileset's `group_data` (`TileSetGroupData`, 0x40 bytes) is **not** in the map style: it comes from `Data/tilesets.xml`
`<TERRAINGROUP>` per tileset: `clump_factor` (smoothing 0..5), `forest_base`, `forest_growth_prob`, `mountain_base`,
`mountain_growth_prob`, `mnt_fringe_tree_prob` (`MOUNTAIN_FRINGE_TREE_PROB`), `coast_base`, `coast_growth_prob`, `bush_clump_prob`,
`bush_spacing`, `bush_min`, `bush_max`, `mnt_rock_clump_prob`, `mnt_rock_spacing`, `mnt_rock_min`, `mnt_rock_max` (e.g. Snowy:
clump 3, forest base 0, mountain base 3, coast base 3, probs 30 / 30 / 40 / 30, bush 60 / 50 / 3 / 5, rock 50 / 100 / 1 / 4).

`TerrainGroups::fill_fertile` @6a6f90 (called by `Map::make` after the regions are rebuilt, §P1 3): for every cell with `land == 0`
(x outer, y inner): `k` = the first index with `fractal.raw[x][y] < partitions[k]` (the raw height byte, not `get_height`),
`k = partitions.count` when none; `land_sub = k` (written when `k ≥ 0`). So the baseland subtype is a smooth noise field cut at
the cumulative percentages of `frequency_0..3`.

## A2. `TerrainGroups::place_all(splash, 1)` @6a70d0

State: `num_players = world.num_starts (+0x84)`; per-type tables indexed `t = type − 4` (0 trees, 1 mountains, 2 rocks, 3 oil,
4 cliffs): `help_lowest_index[t]`, `player_scores[player][t]`, `lowest_player[t]`; per group `clumps[g]` and `active[g]`.
1. `Mountains::randomize_mountains` @89ca70: three circular lists of mountain shapes (small / medium / large); for each in that
   order, if it has more than one entry **draw** `rnd(count)` and rotate its cursor there. `Mountains::get_range(size)` @89cb50 (size
   1..3) returns the list's current element and advances the cursor (an out-of-range size is an error).
2. **Which groups are on, and how many clumps** (checked in the asm) — state `roll`, `taken`, `last_grouping`. For `g` in file order: `active[g] = 1`.
   If `grouping == 0` or `grouping ≠ last_grouping`: **draw** `roll = rnd(100)`, `last_grouping = grouping`, `taken = 0`. Then:
   `roll < 0` (an earlier member of this grouping took it): `chance ≠ 0` → inactive, `taken = 0`; `chance == 0` → active only if `taken` (a tag-along that
   follows a taken member) else inactive. `roll ≥ 0`: `roll −= chance`; still ≥ 0 → inactive, `taken = 0`; else active, `taken = 1`. So entries with the same
   non-zero `grouping` are **alternatives** weighted by `chance` (like the `TILECHANCE` walk, §P1 4), `chance 0` members tag along with the one before them, and
   grouping 0 entries roll independently. An active entry: `n = min_clumps`; when `max_clumps > min_clumps`: **draw** `n = min + rnd(max − min + 1)`;
   `clumps[g] = n`; `help_lowest_index[t] += n` (the array is zeroed before the loop).
3. After the loop each `help_lowest_index[t]` becomes `0x7fffffff` when it is < 2, else `h >> 1` made even (`h −= h & 1`); the player
   scores are zeroed ❓. ("Fairness": a clump whose index within its group is ≥ `help_lowest_index[t]` only aids the worst-off player, §A4; the threshold is half of the **type's** total clump count, so only groups with many clumps ever reach it.)
4. For each active group in order: `size_lo/size_hi = min/max(min_size, max_size)`, `oil_lo/hi = min/max(min_oil, max_oil)`; mountains
   clamp `size_hi ≤ 3`, cliffs `size_hi ≤ 5`; for rocks `oil_hi = min(oil_hi, size_hi)`. For every clump `i < clumps[g]`:
   - **draw** (only when `size_lo < size_hi`) `size[i] = size_lo + rnd(size_hi − size_lo + 1)`; trees with `pattern 0` (per player) then get
     `size[i]` rounded up to even and capped at 9 (so the copied shapes of §A3 fit);
   - rocks (type 6): **draw** (only when `oil_lo < oil_hi`) `oil[i] = oil_lo + rnd(oil_hi − oil_lo + 1)`; other types store `size[i]`.
5. Dispatch on `pattern` (a jump table for 0..3; anything above is skipped):
   - **0 player** (skipped for oil, type 7): `for i in clumps: for p in 0..num_players−1:` `place_player_group(size, p, …)` (§A3), result
     (1 / 0) appended to `placed`. Trees pass the two shape arrays (an earlier forest's shape is copied, §A3) and `param_8 = 1`; when that fails they retry once with
     `param_8 = 0`. The decompile shows no reset of the shape arrays between clumps or groups, so (if that is real) the first forest tile dropped
     anywhere fixes the shape of **every** later per-player forest clump ❓ (check the `SimpleArray` count in the disassembly before porting).
     Mountains: `range = get_range(size)`; on failure retry with the next `get_range` shape until the cursor comes back to the first one, then
     `randomize_mountains` again (3 more draws) and one size smaller (`get_range(size − 1)`), and for size 2 once more with `MOUNTAIN_SMALL`;
     ❓ exact retry ladder (loop exits lost).
   - **1 continent**: regions `r = 1..63` (oil: sea regions 64..126) with more than 5 cells; for each such region and each clump
     `i`: `is_helping = (help_lowest_index[t] <= i)`; `place_region_group(size, r, …)` (§A4); failed clump indices go to a retry list that is
     replayed over all regions with `is_helping = 1`. `placed[i]` = the result.
   - **2 world / 3 nonplayer**: candidate regions = those in the same id range with more than 5 cells; for pattern 3 additionally skip any
     region containing a start location. If there are more than 6 clumps and fewer than 6 candidate regions, regions are repeated in the
     circular list proportionally to their size (`floor(clumps · size_r / total)` copies, remainder added to the largest). If the list is
     not empty: **draw** (when its length > 1) `k = rnd(length)` and rotate the cursor by `k`; then for each clump the current region is
     tried with `place_region_group`; success advances the cursor, failure tries the following regions (mountains fall back to smaller
     `get_range(size − 1)` shapes after a `randomize_mountains`; a bit array remembers regions that already failed) ❓ (the fallback
     ladder is mangled). `placed[i]` = the result.
6. After all groups: `add_doobers` (§A6), then, **unless the map style is 9 (HIMALAYAS)**, `treeify_mountains(group_data.mnt_fringe_tree_prob)`,
   then log lines "Fairness totals" with `player_scores`.
Order matters for the RNG: groups are processed in file order, trees / mountains / rocks / oil / cliffs interleaved as in the XML.

## A3. `TerrainGroup::place_player_group(size, p, ?, oilcount, shapeX&, shapeY&, ?, copy)` @6a4190

One clump of `size` tiles for player `p` (a mountain range, a forest, a rock group with oil, a cliff). Returns 1 on success.
1. `(sx, sy)` = the player's start, `reg0` = its region. `start_max` 0 → 0x40, > 0x40 → 0x40. `lo = circle_radius[start_min]`, `hi =
   circle_radius[start_max]`; for cliffs `size −= 1`. When `lo < hi − 1`: **draw** `i0 = lo + rnd(hi − lo)`, else `i0 = lo`.
2. Walk the ring indices `i = i0 + 1, i0 + 2, …` (wrapping `hi → lo`) until `i` comes back to `i0 + 1`; `c = (sx, sy) + circle[i]`. A candidate `c`
   is rejected unless: in bounds; `region(c) > 63` or `== reg0`; **no other player's start is closer to `c`** than `p`'s own is
   (`vd`); `flags & 0x10`, `0x68`, `0x1800` clear and byte +0x11 == 0; with `cent_min > 0`: `vd(c, centre) ≥ cent_min`, with
   `cent_max > 0`: `≤ cent_max`, where the centre is `(xs/2 + (x > xs/2), ys/2 + (y > ys/2))` (the nearest of the four middle cells);
   with `edge_min` / `edge_max`: `e = min(x, y, xs − x, ys − y)` between them; `c` not on the map border (x, y = 0 or `xs − 1`; the decompile
   compares both to `xs − 1` ❓); `corner_min` / `corner_max`: `min over the 4 corners vd(c, corner)` between them; trees: none of the 8
   neighbours has flag 0x40 (checked twice in the decompile), and when `param_8 ≠ 0` none of the 8 neighbours has flag 0x20 (the loop body was dropped; read as a
   reject ❓: forests keep apart from other forests).
3. By type at the first accepted `c`:
   - mountains (5): `Mountains::add_mountain(size = shape, c.x, c.y, 5, mount_space, forest_space, rock_space, coast_space, start_min)` (shape = the
     `get_range` value passed in; ❓ add_mountain not decoded), done after one success.
   - cliffs (8): when the size ≠ 4, `Cliffs::verify_defensive_position(c, start, cliff_face)` must pass; `Cliffs::position_cliff(c, size, start, …,
     cliff_face)` ❓ (not decoded).
   - trees (4) with a recorded shape (`shapeX.count ≠ 0`, i.e. an earlier call already grew one): **copy**: **draw** `d = rnd&3`, `(mx, my) =
     corner_x/y[d + 1]` (a mirror / rotation), then for each recorded offset `(ox, oy)`: tile `(c.x + mx·ox, c.y + my·oy)` must pass
     `available_rough_tile(…, 0x20, spaces)`; all must pass, then every tile is `drop_tile`d; a failing copy falls back to growing.
   - otherwise (trees without a shape, rocks, oil): **grow** a clump: `drop_tile(c, type, …, stay_near = size/4 + 1, …)` for the seed, then
     repeat: pick a source tile (**draw** `rnd(count)` when `count > 1`, +1), `randomize_orthogs` (**2 draws**, §A5), and for each of the four
     orthogonal directions of the rotated list: with fewer than 3 tiles take the orthogonal neighbour, otherwise **draw** `rnd&1`: 0 =
     orthogonal neighbour, 1 = the matching diagonal from the corner list; skip a neighbour that is already a tile; accept it when in
     bounds, `vd(neighbour, start)` between `start_min` and `start_max`, and the cent / edge / corner tests of step 2 pass; then `drop_tile`.
     A forest records its offsets relative to the first tile in the two shape arrays. Stops at `size` tiles. If every source tile is exhausted
     with fewer than `size`: "Growth Stuck! Removing group" → `clear_group`, return 0. ❓ the exact retry / index bookkeeping of this
     loop (the decompile's `local_cc` / `local_c8` indices) was not reconstructed.
4. Rocks: after success `place_oil_deposits(oilcount)` (§A5).
Counters ("validity", "playerdist", "centermin", … "drop success") are only logged.

## A4. `TerrainGroup::place_region_group(size, region, mountainshape, oilcount, cent_min, cent_max, edge_min, edge_max, corner_min, corner_max, fertile_only, ?, rock_space)` @6a2f60

One clump somewhere in a region. The candidate cells are walked in a **pseudo-random full-period order**: `n = region.coords.count`,
`idx0 = 1 + rnd(n)` (**draw**, when `n > 1`); then the sequence `idx → (idx & 1 ? idx ^ 0x16800 : idx) >> 1` (a 17-bit Galois LFSR step) repeated
while the value is `≤ n` (values above `n` are skipped by stepping again), ending when the start value comes round; cell = `coords[idx − 1]`.
A candidate cell passes when:
1. in bounds; for oil (type 7) the cell is water (`land` read as 2 when the coastline bit is set); for the other types it is not water; with
   `fertile_only == 0`: the tile-level flags (`world +0x138`, u16 per tcoord: bit 7 clear and `(flags & 3) ≠ 3`), else the cell is not in the forest bit set;
2. for every player: `vd(c, start) ≥ start_min` (if > 0) and `≤ start_max` (if > 0, a violation ends the region);
3. `flags & 0x10`, `0x68`, `0x1800` clear, byte +0x11 == 0; oil: some cell within radius 3 (or `rock_space` for large values) must be water without
   flag 0x800 ❓; the centre / edge / corner tests of §A3 with the six numeric arguments; trees: no neighbour has 0x40;
4. fairness, when `is_helping`: the player nearest to `c` must be `lowest_player[t]`, otherwise log "Aborting drop type t. Will not aid player w" and
   reject. (`is_helping` comes from `place_all`: clump index ≥ `help_lowest_index[t]`.)
On the first passing cell the clump is built exactly like §A3 step 3 (seed `drop_tile`, growth with `randomize_orthogs`, the same 3-tile coin flip),
with the same stuck handling (`clear_group` and continue with the next cell). On success: `player_scores[p][t] += vd(start_p, first tile)` for every
player, `lowest_player[t]` = the player with the **largest** score ("worst off"), and rocks call `place_oil_deposits(oilcount)`. Returns 1 on success, 0
when the whole region was walked.

## A5. The small placers

- `drop_tile(x, y, type, ?, stay_near, shape, ?)` @6a2ab0 → 1 on success, and the tile is appended to `tiles`.
  - trees (4): `available_rough_tile(x, y, 0x20, mount, forest, rock, coast space)`; when the group already has tiles, `vd(tile0, c) > stay_near` fails;
    sets the tile (§0 forest rule), then `World::set_blocked_at` on the 16 sub-tiles (4×4) of the wcell.
  - mountains (5): `Mountains::add_mountain(shape, x, y, 4, spaces, start_min)` ❓.
  - rocks (6): `available_rough_tile(x, y, 8, …)`, tile set with `| 8`.
  - oil (7): `World::set_oil_at(x, y, 1)`, success (the sea checks were done by the caller).
  - cliffs (8): `Cliffs::position_cliff(x, y, shape − 1, −1, −1, 0, FACE_WORLD_CENTER)` ❓.
- `available_rough_tile(x, y, kind, mount_sp, forest_sp, rock_sp, coast_sp)` @6a24d0: the cell must be in bounds, **not water**, `flags & 0x28 == 0`, not
  `WorldData::is_edge`, `flags & 0x10 == 0`, not a start city cell (`start_city_wcoord`), `flags & 0x44 == 0` (the decompiler dropped the `return 0`
  of each failed test; read as such ❓). Then keep-away rings (offsets `circle[circle_radius[0] .. circle_radius[space])`, i.e. all within radius
  `space` except the centre): a mountain (0x10) in `mount_sp`, a forest (0x20) in `forest_sp`, a rock (0x08) in `rock_sp` that is not one of **this group's own
  tiles** fails; any water cell within `coast_sp` fails; for `kind == 8` (rocks) additionally none of the eight neighbours may have mountain tcoords or a rock
  flag that is not an own tile. No draws.
- `randomize_orthogs` @6a2320: rewind the four lists; **draw** `a = rnd(4)` (the orthogonal lists' length) and rotate both orthogonal lists together by `a`; then
  **draw** `b = rnd(4)` and rotate the two corner lists together by `b`. (The decompile's first draw is `% orthog length`, guarded by `length − 1 > 0`.)
- `place_oil_deposits(count)` @6a2d90 (only rock groups, `count > 0`): `count = min(count, tiles)`; for the group's tiles in order: tile `t` with flag 0x800
  clear qualifies when some neighbour (`move_x/y[1..8]`) is in bounds, has `flags & 0x28 == 0` and no mountain tcoords; then `World::set_oil_at(t, 1)`; stop
  after `count`. Returns whether `count` were set. No draws.
- `clear_group` @6a28e0 (a stuck clump): every recorded tile that is still a valid group tile loses its group bits (`flags &= 0xffd7`), rock tiles lose their oil
  (`set_oil_at(…, 0)`) and the 16 sub-tile blocks are cleared (except `(blocked & 3) == 2` ones); tiles that are not group tiles get `land_sub = num_tiles[0] − 1` ❓.
  The tile list is not emptied in the decompile ❓.

## A6. The passes after the groups

`Map::make` order after `place_all`: `check_player_forest` (§A7), `nubify_forest`, then (when the tileset's group data base ≥ 0) `change_forest_base`,
`change_mountain_base`, `change_coast_base`, then `fix_transitions` (§P1 3).

- `add_doobers` @6a1540 (inside `place_all`, cosmetic but **consumes draws**): *bushes* — for every interior cell (x from 1 to `xs−2`, y from 1 to `ys−2`, x outer) with
  the forest flag, not on the border, `flags & 0x1000 == 0`: `s = (south neighbour (x, y+1) has no 0x20 flag)`, `e = (east neighbour (x+1, y) has none)`.
  If `s`: **draw** `rnd(100) < bush_clump_prob` → **draw** `q = rnd&3` and place `bush_min + h % (|bush_max − bush_min| + 1)` bushes, `h` a deterministic
  hash of the sub-cell (`X·Y·X` with `X = 4x + ((q+12)&3)`, `Y = 4y + ((q+12)>>2)`), each at `Doober::add_doober("bush", …)` with `bush_spacing`
  offsets from the `move_x/y` tables. If `e`: the same with `(q·4+3)`. *Rocks* — for every interior non-water cell with `flags & 0x38 == 0`, no doobers yet and
  `flags & 0x1000 == 0`: look at the west neighbour then the north neighbour; the first that has the mountain flag 0x10: **draw** `rnd(100) < mnt_rock_clump_prob`
  → **draw** `q = rnd&15` and place `mnt_rock_min + h % (|max − min| + 1)` rocks (`"rock"`, `mnt_rock_spacing`). `Doober::add_doober` / `has_doobers` ❓ not decoded.
- `treeify_mountains(prob)` @6a1cc0: for every cell (x outer, y inner): if it has mountain tcoords and no mountain flag and a south or east neighbour carries 0x10 (and
  `flags & 0x404 == 0`): **draw** `rnd(100) < prob` → `flags = (flags & 0xffc3) | 0x20` (a fringe tree next to the range). If the cell has flag 0x10 and one of its
  orthogonal neighbours (indices 2 and 3 of the orthog list, i.e. not the first and fourth) in bounds has no mountain tcoords (and `flags & 0x404 == 0`): **draw**
  `rnd(100) < prob` → `flags = (flags & 0xffc3) | 0x30` (mountain + forest). The two branches draw only where their conditions hold, in cell order.
- `nubify_forest` @6a93b0 (the argument is unused) roughens straight forest edges; state `run`, `dir` (both reset to 0 at the start of each outer step).
  Pass 1: x = 1..xs−2 outer, y = 1..ys−2 inner (so the run counts consecutive **y** whose cell has a forest neighbour in x). At a cell: not forest → `run = 0`;
  forest with forest on **both** sides (x−1 and x+1) → `run = 0`; the neighbour on the side `dir` is forest → `run++` (a continued run); otherwise a new run:
  `run = 1`, `dir = +1` if the east neighbour is forest, else `−1` if the west one is, else 0. When `run ≥ 4`: **draw** `k = rnd&3`, nub cell
  `(x + dir, y − k)`; it is used only if it is not in the forest bit set, not water, and `World::is_edge_of_region` is false → `land` = 2 if the coastline
  bit else unchanged, `flags = (flags & 0xffc3) | 0x20`, the cell goes into two scratch lists (unused afterwards); a rejected nub resets `run = 0`, an accepted one
  does not. Pass 2 is the same with the axes swapped (y = 1..ys−2 outer, x inner, neighbours at y±1, nub at `(x − (rnd&3), y + dir)`); its "forest on both sides, draw
  `rnd&1` for the direction" branch is dead code (both sides forest resets first). ❓ only the pass-1 resets were read in the asm; the nub rejection reset of pass 2 is by analogy.
- `change_forest_base` @6a9a60 (when `forest_base ≥ 0`): every forest-flag cell gets `land_sub = forest_base`; each of its four orthogonal neighbours that is not forest and
  not `land == 2`: **draw** `rnd(100) < forest_growth_prob` → `land_sub = forest_base`. `change_mountain_base` @6a9c10: same for cells with mountain tcoords
  (`mountain_base`, `mountain_growth_prob`, neighbours without mountain tcoords). `change_coast_base` @6a9dc0: cells with flag 0x04 (`coast_base`,
  `coast_growth_prob`, neighbours without 0x04). The loops run x outer, y inner; draws happen per qualifying neighbour in that order.
- `fix_transitions` @6a9f70 keeps subtype steps gradual: `nubify_transitions(3)`; every land cell (`land == 0`) with `land_sub == 3`: each of its 8 neighbours that is not a
  plain water cell without the coastline bit and is not (`land 0` with `land_sub` ∈ {3, 2}) gets `(land, land_sub) = (0, 2)`; `nubify_transitions(2)`; the same for
  `land_sub == 2` cells with allowed neighbours {3, 2, 1} → `(0, 1)`; `nubify_transitions(1)`.
  `nubify_transitions(s)` @6a1f30 (asm checked): pass 1, x outer / y inner: `run` counts consecutive y of cells with `land == 0`, no 0x04 and `land_sub == s` (any other cell
  sets `run = 0`); at `run ≥ 5`: **draw** `k = rnd(3)`, `y' = y − 1 − k`; first try `(x − 1, y')`, then `(x + 1, y')`: the first one in bounds with no 0x04, `land 0` and
  `land_sub < s` gets `land_sub = s` (and `run = 0`); if neither qualifies `run` stays (so the next matching cell draws again). Pass 2, y outer / x inner:
  `run ≥ 4`, **draw** `k = rnd&3`, cells `(x − k, y − 1)` then `(x − k, y + 1)`, same test and reset.

## A7. `Map::check_player_forest` @68e8f0 (corrections to §P1 7.3)

For each start location: `R = city_center_radius / 4 − 1`; count the cells with flag 0x20 over circle indices `[circle_radius[0], circle_radius[R])`. At 3 or more: done. Otherwise
`target = 3`, and up to three passes (`target = 3, 2, 1`): scan ring indices `i = circle_radius[2] .. circle_radius[R]−1` around the start; a **free** cell is in bounds, `flags & 0x30 == 0`, not water,
`flags & 0x48 == 0`, not in the forest bit set, `flags & 4 == 0`. A free cell starts a clump `[c]`; its four orthogonal neighbours in orthog order are added when free;
the moment the clump has exactly `target` cells, every cell gets `land = 2 if flags & 4 else land` (`land_sub` is **unchanged**: part 1's "copied from the first" is wrong), `flags =
(flags & 0xffc3) | 0x20`. A free cell whose neighbours do not reach `target` is skipped. After the first conversion a `done` flag stays set for the remaining passes: they only try the
first ring cell (any non-free ring cell ends the pass). So a start with fewer than 3 forest cells gets one clump of 3 (or 2, or 1) beside it. No draws.

# B. The continent recipes

## B1. Primitives

- `Map::wipe` (`World::wipe` @6b2c00): clears the start / oil lists and the forest bit set, `sea_map = −1`, `land_size = 0`, territory limits +0x50..+0x74 = 0, and every cell becomes
  `flags 0, land 2 (water), land_sub 0, region 64`, `region2 0`, `down −1`, `val 0`, `goods 0`, `light 0`, `who 0xff`. **A fresh map is all ocean (region 64).** `Regions::clear_all` follows.
- `Map::make_region(r, x, y, goal)` @69d3f0 (§P1 6): the seed becomes land 0 of region `r`, size 1, flags / common / goody from the `Map` fields (`region_flags +0x18`, `common_factor +0x1c`,
  `goody_factor +0x20`); `regions.land = max(regions.land, r)`. The recipes then override `region.flags` (+8), `common_factor` (+0), `goody_factor` (+4) and `climate` (+0xc) per region.
- `Map::point(r, x, y)` @69d5a0 returns **1** when `(x, y)` is out of bounds or already has a region (nothing done), **0** when `grow_valid(r, x, y)` fails, otherwise sets `land = 0`, `land_sub = 0`,
  `region = r`, appends the cell to `region[r].coords`, `size++`, returns 1. When `region[r].flags & 1` it also updates the **touching masks**: for each of the 8 neighbours (`move_x/y[0..8]`) in bounds with
  another non-zero region `q`: `m = mask[r] | mask[q] | (1 << r) | (1 << q)`; `mask[r] |= m; mask[q] |= m;` then every region whose mask intersects `m` gets `|= m` (a transitive
  closure; `Region +0x24`, 32 bits, so only regions 0..31 take part).
- `Map::stamp(r, x, y)` @69cf80: `point` at (x, y), (x+1, y), (x−1, y), (x, y−1), (x, y+1), stopping at the first 0; returns 1 only if all five returned 1.
- `Map::large_stamp(r, x, y, left)` @69cb20 (`left` unused): `stamp(x, y)` (0 → return 0); **draw** `v = rnd&15`; the pattern, each `stamp` offset `(dx, dy)` stopping at the first 0 (return 0), else 1:
  0: (−1,−1) (−2,−2) (−4,−1) (−4,0) (−5,−1) (−5,−2) · 1: (1,1) (2,2) (4,1) (4,0) (5,1) (5,2) · 2: (1,0) (1,−2) (1,2) (2,−4) (3,−4) (4,1) · 3: (−1,0) (−1,−2) (−1,2) (−2,4) (−3,4) (−4,−1) ·
  4: (0,−1) (0,1), then (−2,0) (−1,0) (0,0) (1,0) (2,0) (a failure ends the call), then **draw** `rnd&1` → `stamp(x + (bit·2 − 1), y − 1)`, and if it succeeds **draw** `rnd&1` → `stamp(x + (bit·2 − 1), y + 1)` ·
  5: (1,−2) (2,−4) · 6: (−1,−2) (−2,−4) · 7: (−1,2) (−2,4) · 8: (1,2) (2,4) · 9..15: nothing (return 1). (Cases 5..8 return the result of their last stamp.)
- `Map::land_dist(x, y, outside_is_land)` @69d970: 0 unless `(x, y)` is a plain water cell; else the smallest ring `k = 1..64` with a land or half-land cell in ring `k` (indices `[circle_radius[k−1], circle_radius[k])`);
  out-of-bounds cells count as land when `outside_is_land ≠ 0`, else are skipped; 65 when none. The ring is scanned in table order, the first hit returns.
- `Map::invert_land` @69d880: every plain water cell (not 0x100, land 1 or 2) becomes `land 0`; **every other cell (land, half-land) becomes `land 2`**; `land_sub` is cleared, `region` set to 0; then `Regions::clear_all`.
  Together with `wipe` this is how "grow a blob of land, then invert" gives lakes and seas.
- `Map::fix_lakes()` @69c470 (static, called from `make_coastlines`, §P1 7.2): repeat until nothing changes: for every interior cell (1..xs−2, 1..ys−2, y outer) that is plain water, count the eight
  neighbours that are *not* plain water (land or half-land); more than 5 → the cell becomes `land 0` (land_sub 0). **`MapGreatSahara::fix_lakes` @41c150 is an empty `RET`**: an override of a
  virtual that `make_coastlines` never calls (it calls the static one directly).
- `Map::grow_region` @69c600, details beyond §P1 6: the *return value* is **0 on success when the 7th argument is 0** (the callers loop "until `grow_region(…, 0) == 0`"), and the region's final size
  (≠ 0) when it gave up; with the 7th argument ≠ 0 it **always returns the final size**. Give-up: a pass that adds no cell increments a total stall counter and a consecutive stall counter; the
  total ≥ `8·goal` or the consecutive ≥ `goal` ends it (the consecutive counter resets when a pass adds a cell). Pass: pick a cell (§P1 6, `scale_by_area(512)` is the size threshold below which the pick is
  always uniform), `goal − size < 0x40` → `stamp` (five `point`s), else `large_stamp`. `k` (ring size for the first fill) as in §P1 6. `grow_valid` @69d000 returns 1 for valid.
- `Map::scale_land_area(v, n)` @68b1e0 (corrects §P1 6): if `n == 0` or `n ≤ base` (`base = {2, 2, 3, 4, 6, 8}[map_size]`, 8 above) return `v`; else `(int)((float)v / ((float)n / (float)base))` (32-bit float).
  `Map::get_player_overage(n)` = `0` if `n == 0 || n ≤ base`, else `n − base`.
- `Map::fill_cont(count[8], team_of[8], extra)` and the pools / spacing helpers: §P1 6, 7.

## B2. Parameter table

`h = min(xs, ys) / 2`. Radii are integers (truncating). "ring" = starts on a circle around `(xs/2 [+ rnd&1], ys/2 [+ rnd&1])` via `project`. `Gs` = `scale_land_area`.

| style (id) | land method | start placement | per-player regions | after | `sea_map` XML | notes |
|---|---|---|---|---|---|---|
| Old World (6) / Great Sahara (7) / Amazon Basin (8) / Himalayas (9) / Southwest Mesa (10) | `wipe`, `invert_land`: **the whole map is land** | ring `R = min(0.8·h, h − 4)`, step `0xffffffff/n` | none | `start_dist = max(xs/16, 4)` | 0 | the five functions are identical (§B3.1); Himalayas also disables `treeify_mountains` |
| African Watering Hole (11) | one water region of `5%·cells` grown at the centre, inverted | same ring | none | `avoid_continent = max(4, Gs(avoid_continent))`, retry until grown | 0 | §B3.2 |
| Mediterranean (12) | §P1 6 | coast ring | island region 32 | | 1 | |
| Australian Outback (13) | player regions + central region 9, merged | ring `0.66·h` | regions 1..n flags 1 | | 1 | §B3.3 |
| Great Lakes (14) | `n_lakes` water regions scattered, inverted | ring at `1.5·xs` shrinking by 3 | lake regions | `check_player_land(range)` | 1 | §B3.4 |
| Warring States (15) | a chain of region seeds ("states") + fairness blobs | chain order | seeds flags 1 / 6 | `check_player_land(−1)` | 2 | §B3.5 |
| New World (16) | player islands on a ring + central region 9 | ring `7/8·h` | flags 2, common = goody = 12; centre 6 | `check_player_land(−1)`, `eliminate_pools(ENTIRE)` | 3 | §B3.6 |
| Colonial Powers (17) | two big regions; players on one or two of them | ring `3/4·h`, clamped 5..dim−5 | | `eliminate_pools(EDGES)` | 4 | §B3.7 |
| East Indies (18) | player islands + scattered land blobs | ring `7/8·h` | flags 2, common = goody = 8; blobs 5 | | 3 | §B3.8 |
| East Meets West (19) | one continent per team | per continent, near the shore | | `eliminate_pools(ENTIRE)`, `eliminate_edge_canals` | 3 | §B3.9 |
| Sea Power (20) | one island per player | ring `7/8·h` | | `check_player_land(−1)`, `eliminate_pools(EDGES)`, `eliminate_edge_canals` | 3 | §B3.10 |
| Nile Delta (21) | one island per player, grown in stages | ring `3/4·h` | | `eliminate_pools(ENTIRE)`, `eliminate_edge_canals`, `check_player_land(−1)` | 3 | §B3.11 |
| British Isles (22) | `k` islands holding the players | `place_start_in_region` | | `check_player_land(−1)`, `eliminate_pools(ENTIRE)` | 3 | §B3.12 |
| Conquest map / sea (24 / 25) | a rendered map image imported as a scenario | from the conquest nodes | | `check_sea_map` | – | §B3.13 |
| Scenario (26) | `ScenarioRead::import` | from the game style | | | – | §B3.14 |

The `sea_map` numbers are only stored (`Map::make`: `world.sea_map = sea_map` when ≥ 0; `check_sea_map` writes 2 / 3 for the conquest maps); their meaning (naval maps) is for the AI / start-resource code.

## B3. The recipes

All of them run inside `Map::make` after `load_map_data` (so `xs`, `ys`, `world.seed`, the XML `Map` fields exist) and `base_edge`'s draw, §P1 3. "retry" means: the loop body starts again with
`wipe` and **repeats every draw**.

### B3.1 Old World, Great Sahara, Amazon Basin, Himalayas, Southwest Mesa (`@69c0e0 / 69bf70 / 69be00 / 69bc90 / 69bb20`)
```
wipe; clear_all; invert_land                                  // all land, region 0
R = min(0.8·h, h − 4); step = 0xffffffff / n
a = rnd(0xffff) % 0xffff; b = rnd(0xffff) % 0xffff; angle = (a << 16) + b          // 2 draws
cx = xs/2 + (rnd&1)                                                                   // draw 3
cy = ys/2 + (rnd&1)                                                                   // draw 4
start_dist = max(xs/16, 4)                                                            // Map +0x28
repeat n: angle += step; project(cx, cy, angle, R) → add_starting_location(x, y)
```
The five functions differ only in the class. No region exists, `Regions::find_all` later builds one land region. The tilesets and terrain groups (XML) give the character.

### B3.2 African Watering Hole (`@69b650`)
```
avoid_continent = max(4, scale_land_area(avoid_continent, n)); step = 0xffffffff / n
loop:
  wipe; clear_all; R = min(0.8·h, h − 4)
  angle = (rnd % 0xffff) << 16 + rnd % 0xffff            // 2 draws
  goal = cells · 5 / 100
  cx = xs/2 + (rnd&1); cy = ys/2 + (rnd&1)               // 2 draws
  make_region(1, cx, cy, goal)
  if grow_region(1, goal, R, -1, -1, 0) != 0: continue   // retry (everything redrawn)
invert_land                                              // the 5% blob becomes the central lake
cx = xs/2 + (rnd&1); cy = ys/2 + (rnd&1)                 // 2 draws (a second centre jitter)
start_dist = max(xs/16, 4)
repeat n: angle += step; project(cx, cy, angle, R) → add_starting_location
```
The lake is water region 64 (found later); `place_all` puts `oil` groups (type 7, world pattern) into it.

### B3.3 Australian Outback (`@69a650`)
```
step = 0xffffffff / n
retry loop:
  wipe; clear_all; h = min(xs,ys)/2; Rg = h·55/100 ; Rs = h·66/100
  angle = (rnd % 0xffff) << 16 + rnd % 0xffff            // 2 draws
  goal0 = xs·ys · 5 / 9
  cx = xs/2 + (rnd&1); cy = ys/2 + (rnd&1)               // 2 draws
  start_dist = max(xs/16, 2)
  mask = 0
  for i in 1..n:                                          // player seeds on the ring (radius Rs)
      project(cx, cy, angle, Rs) → (px, py)                    // the jittered centre (checked in the asm)
      add_starting_location(px, py); make_region(i, px, py, goal0)
      region[i].flags |= 1; region[i].climate = 1; mask |= 1 << i; angle += step
  mask |= 1 << 9; make_region(9, cx, cy, cells); region[9].climate = 0
  for j in 0..n-1:  grow_region(((j & 7) + 1), (goal0 / n) / 4, Rg, cx, cy, 0)          // pass 1: quarter
  for j in 0..n-1:  grow_region(((j & 7) + 1), (goal0 / n) / 2, Rg, cx, cy, 0)          // pass 2: half
  region[9].flags |= 1; grow_region(9, goal0 / 3, Rg / 3, -1, -1, 0)
  step_sz = goal0 / 64
  loop:
     if (region[9].mask & mask) == mask:                  // the outback touches every player region
         for j in 0..n-1: move every cell of region ((j & 7) + 1) into region 9 (append to its coords, set cell region 9, free the list)
         grow_region(9, goal0, Rg, -1, -1, 0); eliminate_pools(ENTIRE_WORLD); clear_all; return
     for k in 0..7: r = k + 1                              // k restarts at 0 each round (the decompiler reuses a counter; otherwise the loop could not terminate)
         if (region[r].mask & 0x200) == 0:                 // r not yet touching region 9
             if goal0 < region[r].size + step_sz:
                 grow_region(9, region[9].size + step_sz, Rg/4, -1, -1, 0)          // r is big enough: grow the outback towards it
             else if grow_region(r, region[r].size + step_sz, Rg, cx, cy, 0) != 0:   // r stalled
                 grow_region(9, region[9].size + step_sz, Rg/4, -1, -1, 0)
```
The result is a large central landmass joined to all player blobs (≈ 5/9 of the map is land after the final grow). The `(j & 7)` pass loops use the angle variable as a counter in the decompile; the asm shows it is reset to 0 (checked), so with more than 8 players regions 1..8 are grown more than once.

### B3.4 Great Lakes (`@699e40`, asm checked)
```
R_ck = ceil(max_range(unit type 0x15d) / 4)                  // type 0x15d = the ships' type (same as Mediterranean); passed to check_player_land
avoid_continent = max(6, scale_land_area(avoid_continent, n)); step = 0xffffffff / n
wipe; clear_all                                               // no retry loop
angle = (rnd % 0xffff) << 16 + rnd % 0xffff                   // 2 draws
Smax = max(cells / 20, 90); Smin = max(cells / 40, 24); Smax_cur = size = Smax
lakes_left = xs / 12 + (rnd & 1)                              // 1 draw
r = 0; iter = 0
while lakes_left != 0 and ++iter < 100:                       // iter is reset to 0 after each lake that was made
    rho = isqrt(size · 7 / 22)                                // Newton: x = v/2; loop y = (x + v/x) >> 1 while y < x
    need = max(3, (avoid_continent + 1)/2 + rho)
    tries = 0
    do { x = rnd(xs) [if xs > 1 else 0]; y = rnd(ys) [if ys > 1 else 0] }          // 2 draws per try
    while land_dist(x, y, 1) < need and ++tries < 1000
    if tries < 1000:
        if grow_valid(r + 1, x, y): r++; make_region(r, x, y, size); grow_region(r, size, xs, -1, -1, 0); lakes_left--; iter = 0
        size = Smin + (Smax_cur - Smin > 1 ? rnd(Smax_cur - Smin) : 0)               // 1 draw when the range is > 1
    else:                                                                            // no spot at this size
        if size > Smin: Smax_cur = size = Smin
        else: Smin = Smin · 13 / 16; Smax_cur = size = Smin; if Smin <= 19: stop
eliminate_pools(ENTIRE_WORLD); invert_land                    // the lakes become water, everything else land
for each of n players:
    angle += step; R = xs · 3 / 2
    repeat: (x, y) = project(xs/2, ys/2, angle, R); R −= 3
            until the cell and its four orthogonal neighbours are in bounds and none of them is on the map border, and (x, y) is not water
    add_starting_location(x, y)
check_player_land(R_ck)
```
`land_dist(x, y, 1)` on a fresh map (all water) is the distance to the nearest lake region so far (out-of-bounds counts as land, so lakes keep off the edge by `need`). The start ring begins outside the map and is pulled in by 3 per try, so each player's start is the first land cell along its ray from the outside.

### B3.5 Warring States (`@698b60`)
```
avoid_continent = max(4, scale_land_area(avoid_continent, n))
wipe; clear_all
ρ = h − avoid_edge[0] − avoid_continent
angle0 = (rnd % 0xffff) << 16 + rnd % 0xffff               // 2 draws
goal = cells / 7
seed0 = project(angle0, ρ)                                 // first state, on the ring
k = n rounded up to even, doubled until >= 6; stepR = 2ρ / k; start_dist = stepR / 4
repeat:                                                    // a chain of state seeds
    s++ ; make_region(s, x, y, goal); region[s].flags |= 1; mask |= 1 << s; record (x, y)
    heading = angle0 + ((rnd&0x3fff) + 0x6000) << 16       // 1 draw: 135..225 degrees off angle0
    (x, y) = project(prev, heading, stepR)
until vd((x,y), centre) > ρ + avoid_continent or > min(xs,ys) − avoid_continent
fill_cont(team counts, teams …)  → the seed index of every leader's start (members of a team share a state, team 8 = solo)
add_starting_location for every valid leader (from the recorded seeds)
grow loop: for r in 1..s: grow_region(r, size(r) + 0x30, xs, -1, -1, 0)  until total land >= goal·? and the touching mask holds all seeds (or `s` rounds); ❓ exit rule
merge: regions 2..s are folded into region 1 (cells appended, region id set to 1)
Regions::clear_all; find_all
extra "states" to even out resources (MapFairness, 5 rounds): m = xs/15 + (rnd&1) extra blobs;
   for each: ρ' = isqrt(193 + rnd(0x37)) ... pick a land cell (rnd(count) start, walk) with land_dist >= max(5, (avoid_continent + ρ')/2)
   whose MapFairness.calc_distances keeps the teams balanced; make_region(s', x, y, 220); grow_region(s', 220, ρ' + 1, -1, -1, 0); region flags 6
check_player_land(-1)
```
❓ This recipe is the least certain: the decompile of the grow / merge loops and the MapFairness bookkeeping (`calc_distances`, `lowest_dist`, `highest_player`, `fairness[team] += dists[team]`) is partial. A port must read the disassembly before claiming bit-exactness. Known for sure: the chain of seeds with random headings in `[0x6000, 0x9fff]·2¹⁶`, the cut-off by `ρ + avoid_continent`, `goal = cells / 7`, the fixed blob size 220 and `m ≈ xs/15` extra blobs (+ 0 or 1).

### B3.6 New World (`@6984d0`, asm checked)
```
avoid_continent = max(4, Gs(avoid_continent)); avoid_edge[0..3] = Gs(avoid_edge[k]) each; step = 0xffffffff / n
retry loop (any island failure restarts here):
   wipe; clear_all; m = min(xs, ys)
   P = max(60, Gs(250));  C = Gs((cells − P·n) · 4 / 10)
   d1 = rnd(0xffff)                                                       // draw (both branches)
   rho = (m/2) · 7 / 8
   if n < 4: angle = (d1 & 3) · 0x3fffffff + 0x1fffffff; if n != 3: C = (cells − P·n) · 2 / 3      // quarter-turn offsets, no more draws
   else:     d2 = rnd(0xffff); angle = (d1 % 0xffff) << 16 + d2 % 0xffff                              // 1 more draw
   for i in 1..n: angle += step; (x, y) = project(xs/2, ys/2, angle, rho); add_starting_location; make_region(i, x, y, P)
                  region[i].flags |= 2; region[i].common = region[i].goody = 12
   make_region(9, xs/2, ys/2, C); region[9].common = region[9].goody = 6
   avoid_center = Gs((xs/2) · 70 / 100); K = max(4, Gs(4))
   for pass in 0, 1:  div = (pass == 0) ? 2 : 1
        for i in 1..n: sz = grow_region(i, P/div, (rho / K) / div, -1, -1, 1)       // 7th arg 1: returns the size
                       if sz < (P/div)/2: log "Failed to grow region for player"; restart
   avoid_center = 0; grow_region(9, C, rho, -1, -1, 0)
   check_player_land(-1); eliminate_pools(ENTIRE_WORLD); return
```
Players sit on small islands at 7/8 of the half-size; the central landmass (region 9) is the "new world". The two island passes use the pass-0 half goal then the full goal; with `avoid_center` set, other regions keep that distance from the cells being grown (§P1 6 `grow_valid`).

### B3.7 Colonial Powers (`@697dd0`)
```
avoid_continent = max(4, Gs(avoid_continent))
retry loop:
   wipe; clear_all
   (A, B) = (players of the "home" side, the rest):
        team_style == 7 and not the editor: A = count of valid leaders with (flags & 5) == 1 whose team does not occur among the leaders with (flags & 5) == 5; B = n − A
        else: A = 0, B = n
   ρ = (h·3)/4; angle0 = (rnd % 0xffff) << 16 + rnd % 0xffff     // 2 draws
   goal = cells · 2 / 3
   for g in 0, 1:                                                 // two regions
       (x, y) = project(angle, ρ)
       for each player of group g (A or B): (px, py) = project(angle', ρ) clamped to [5, dim − 5]; add_starting_location; angle' += 0x2aaaaaaa/(A−1) or 0x55555555/(B−1)
       make_region(g + 1, x, y, goal); angle += 0x7fffffff      // the second region sits opposite
   avoid_center_saved = avoid_center
   for g in 0, 1: if g > 0: avoid_center = ρ·7/9;  fail = grow_region(g + 1, goal/2, ρ·3/4, -1, -1, 0); on failure retry the whole loop
   avoid_center = avoid_center_saved; check_player_land(-1); eliminate_pools(EDGES_ONLY)
```
Group 0's region (empty unless team style 7) is the colony continent; the other side's players start on region 2. ❓ which of A / B is region 1 and the use of the `0x2aaaaaaa` / `0x55555555` constants (arcs of one sixth / third of a turn) were not confirmed.

### B3.8 East Indies (`@697540`)
```
avoid_continent = max(4, Gs(avoid_continent)); step = 0xffffffff / n
retry loop:
   wipe; clear_all; m = min(xs, ys); G = Gs(250)
   d1 = rnd(0xffff)
   if n < 4: angle = (d1 & 3) · 0x3fffffff + 0x1fffffff; rho = (n == 3) ? ((m/2) · 7 / 8) : m / 2
   else:     d2 = rnd(0xffff); angle = (d1 % 0xffff) << 16 + d2 % 0xffff; rho = (m/2) · 7 / 8
   edge = max(avoid_edge[0..3]) + 4
   for i in 1..n: angle += step
        loop: (x, y) = project(xs/2, ys/2, angle, rho); if x > edge and xs − x > edge and y > edge and ys − y > edge: break; rho −= 1
        add_starting_location(x, y); make_region(i, x, y, G); region[i].common = region[i].goody = 8; region[i].flags |= 2
   pass A: for i in 1..n: grow_region(i, G/3, rho/2, -1, -1, 0)                   // failures only logged
   pass B: for i in 1..n: grow_region(i, G,   rho/2, -1, -1, 0) != 0 → log and RESTART the whole loop
   scatter of extra islands (Great Lakes' loop, B3.4, with LAND regions):
        Smax = max(cells/20, 90), Smin = max(cells/40, 24), size = Smax; count = xs/12 − n + 8 + (rnd&1)      // 1 draw; 0 ⇒ done
        same position search (land_dist ≥ need, 1000 tries, 2 draws per try), r = n + k;
        on success: make_region(r, x, y, size); grow_region(r, size, xs, -1, -1, 0); region[r].common = goody = 5, climate = 1; count−−
        size / shrink rules as in B3.4
   return                                                                      // no check_player_land here
```
Players sit on islands at 7/8 (n ≥ 3; for 1 or 2 players the ring starts at the map edge and is pulled inward until `edge` is cleared). ❓ the 1-2 player ring and the (unchecked) scatter loop's exact exit follow B3.4 by analogy.

### B3.9 East Meets West (`@696640`)
```
avoid_continent = clamp(Gs(avoid_continent), 4, 16)   // the decompile shows a clamp to [4, 16] and a retry counter that later lowers it (12/8/4)
retry loop:
   wipe; clear_all; sides = max(1, game.num_sides)
   fill_cont(count[8], team_of[8], null)                       // continent of every team (§P1 6; may flip with rnd&1)
   ρ = (h·3)/4; angle = (rnd % 0xffff) << 16 + rnd % 0xffff  // 2 draws
   U = Gs((cells / n)·5 / 9)
   for c in 0..sides-1: (x, y) = project(angle, ρ); make_region(c + 1, x, y, U);
        angle += (count[c]·step >> 1) + (count[(c+1) % sides]·step >> 1)        // arcs ∝ players on each continent
   pass A: for c: grow_region(c + 1, (U/2)·count[c], ρ·3/4, -1, -1, 0)
   pass B: for c: grow_region(c + 1, U·count[c], ρ·3/4, -1, -1, 0)
   any failure: counter++ and, at counters 8 / 12 / …, avoid_continent = 8 / 4 ❓; retry
   centroid[c] = find_region_centroid(c + 1); eliminate_pools(ENTIRE_WORLD); eliminate_edge_canals
   for each valid leader slot 0..7 (team 8 = solo, own continent): continent c = team_of[…] (or the slot itself when −1)
        baseAngle = find_angle(map centre, centroid[c]) ± team spread (60° steps for 2..3 members, 0x6aaaaaaa/count otherwise, random offset `0xaaaaaaa − rnd(0x15555555)` for a single member);
        try place_start_in_region(c + 1, …, min_dist = Gs(scale_by_area(…)), …) first ❓; else walk project(baseAngle, ρ·3/4 ⇒ shrinking by 2 per try, then angle += 0x71c71c6 after 3 turns) up to 30 tries:
              accept the cell when it is in continent c, ≥ 8 (vd) from every start already placed and is_near_ocean(…, 3, 9);
              then up to 30 more tries with a relaxed test (≥ 6, is_near_ocean(…, 2, 7)); last resort: any cell of the continent ≥ 8 from all starts
        add_starting_location
check_player_land(-1)
```
❓ The start search is the least certain part (the decompile's loop exits were reconstructed from the asm shape only); the continent / ring construction is solid.

### B3.10 Sea Power (`@696080`, asm checked)
```
G0 = max(80, Gs((cells / n) · 5 / 8, n)); avoid_continent = max(4, Gs(avoid_continent)); step = 0xffffffff / n
retry loop:
   wipe; clear_all; rho = ((min(xs,ys)/2) · 7) / 8
   angle = (rnd % 0xffff) << 16 + rnd % 0xffff                          // 2 draws
   for i in 1..n: angle += step; (x, y) = project(xs/2, ys/2, angle, rho); add_starting_location; make_region(i, x, y, G0)
   pass A: for i in 1..n: if grow_region(i, G0/4, (rho·3)/4, -1, -1, 0) != 0: log; RESTART (G0 unchanged)
   pass B: for i in 1..n: if grow_region(i, G0,   (rho·3)/4, -1, -1, 0) != 0: log; G0 = max(G0 · 9 / 10, (cells / n) / 4); RESTART
check_player_land(-1); eliminate_pools(EDGES_ONLY); eliminate_edge_canals
```
### B3.11 Nile Delta (`@695bf0`, asm checked)
```
Gs0 = max(100, Gs((cells / n) · 6 / 8, n)); avoid_continent = max(4, Gs(avoid_continent)); step = 0xffffffff / n
retry loop:
   wipe; clear_all; rho = ((min(xs,ys)/2) · 3) / 4
   angle = (rnd % 0xffff) << 16 + rnd % 0xffff                          // 2 draws
   for i in 1..n: angle += step; (x, y) = project(xs/2, ys/2, angle, rho); add_starting_location; make_region(i, x, y, Gs0)
   K = Gs(4, n); acc = Gs0                                              // K stages
   for stage in 0..K−1:
       for i in 1..n: if grow_region(i, acc / K, rho / 2, -1, -1, 0) != 0:
                         log; Gs0 = max(Gs0 · 4 / 5, (cells / n) / 4); RESTART
       acc += Gs0                                                       // the last stage reaches Gs0
eliminate_pools(ENTIRE_WORLD); eliminate_edge_canals; check_player_land(-1)
```
(With more players than the map's nominal count `K` can be 0, in which case no growth happens and the loop ends at once ❓ `Gs(4, n)` with `n > base` floors to `4·base/n`.)

### B3.12 British Isles (`@695130`, the growth part checked in the asm)
```
avoid_continent = max(4, Gs(avoid_continent))
retry loop:
   wipe; clear_all
   k = n if n < 3 else min(n, rnd(n) + 2)                                  // 1 draw (n ≥ 3 only); k = number of islands
   share[i] = (n + i % k) / k  for i < k; share[i] = 1 when it is 0 and i < n       // players per island
   rho = ((min(xs,ys)/2) · 6) / 8; angle = (rnd % 0xffff) << 16 + rnd % 0xffff       // 2 draws
   U = max(120, Gs((cells / n) · 5 / 9, k))                                 // NB the second argument is k, not n
   for i in 0..k−1: (x, y) = project(xs/2, ys/2, angle, rho); remember (x, y)
        make_region(i + 1, x, y, share[i]·U); grow_region(i + 1, share[i]·U / 4, rho / 6, -1, -1, 0)
        angle += (share[i]·step >> 1) + (share[(i + 1) % k]·step >> 1)        // arcs proportional to the players on the two islands
   stage = (get_player_overage(n) != 0 and U == 120) ? 1 : 0
   for i in 0..k−1:
        goal = stage ? U : share[i]·U
        size = grow_region(i + 1, goal, rho / 3, -1, -1, 1)                  // 7th arg 1: returns the size
        if size < goal/2 or size < 120: log; k −= 1; if i == k: the failing (last) island is dropped, else RESTART the loop     ❓ the share bookkeeping of the drop (an `INC` of a share slot) was not resolved
   for i in 0..k−1: repeat share[i] times: place_start_in_region(i + 1, &x, &y, 10, ?, …) and append (x, y) to the candidate lists     // 1 draw each (§P1 8)
   repeat n times: do { j = rnd(n) (0 when n == 1) } while used[j]; used[j] = 1; add_starting_location(cand[j])               // ≥ n draws
check_player_land(-1); eliminate_pools(ENTIRE_WORLD)
```
The islands' players get candidate starts from their own island but are handed to the start slots in a random order (the start order of §P1 1 step 7 then maps slots to leaders). `Setup::build_game` skips the team-mates-together rule for style 0x16 (§P1 1 step 9).

### B3.13 `MapConquest` (styles 24 / 25) — the conquest campaign map
- `make_continents(n)` @68dba0: `wipe`; frees the conquest start lists (`start_who / capital / node / add_armies`); requires `conquest_game.conquest_win`
  ("No conquest map data available."); picks the two nations (the player's tribe and the link's other node: `ConquestCountries::draw_scenario_area` renders the
  involved countries' area to an image, saved under the profile's app directory and re-loaded), `ScenarioEditor::import_image` turns the image into terrain
  (error "Failed to build conquest scenario from map image."). `Regions::clear_all / find_all`. The two start locations (centroid-like coordinates returned by the importer)
  are `add_starting_location`ed and pushed to the conquest lists (`start_capital = 1`, `start_node`, `start_who` = 0 for the player (or 1 when the map style is
  6 or 7 and the sides swap), `start_add_armies = 0`). Then `add_additional_players(…)` and `check_sea_map`.
- `add_additional_players(link_a, color_a, color_b)` @68d260: `m = game_style.additional_opponents[difficulty] (+ the link's extra count)`; `r_far = city_spacing·2/4`, `r_near = first_city_near_coast / 4`. For
  each of the `m` extra seats: for a "barbarian" city: try the region(s) of the two main nations (`rnd(region size)` start index, walk the region's cells in order) with
  `check_starting_loc(x, y, num_sea, 0, −1, 0, …)` until one passes, then `add_starting_location` and push (capital 1, node −1, `who = (a ≠ b) + 1 + index`, armies 0); error
  "unable to place barbarian city" otherwise. For a supporting nation: scan circle indices `[circle_radius[r_far], circle_radius[0x40])` around the main nation's start, same test; error
  "Unable to place supporting nation". The one `rnd` is the region-walk start.
- `check_starting_loc(x, y, r, want_land, skip_player, list, …)` @68d7e0 → bool: in bounds; `r ≤ 0x40`; when `want_land`, no water within the ring `1..r` ❓ (empty bodies); the cell at least `r` from the map edge; every
  other start (or just `skip_player`) and every entry of `list` at `vd ≥ r`; no mountain / forest / water / `flags & 0x48` cells within radius `r` when the 4th argument is non-zero.
- `check_sea_map` @68d1a0: if the map has sea regions (`Regions::get_num_sea ≠ 0`): `sea_map = 3` when the start locations are not all in the same region, else 2 (no sea: unchanged).

Third pass (2026-10-24), the pipeline read against the decompile (`re/decomp/mapgen_fid3/cq/`), NOT ported (no oracle and the inputs live in the CtW layer):
- `ConquestCountries::draw_scenario_area(nodeA, nodeB, &image, &ids, &xs, &ys)` @761310: `get_scenario_map_rect` gives the rectangle of the strategic map around the two
  countries; the image is a square of side `max(w, h)` filled with `0xffffffff` (water); for every country whose rectangle (its `left / right / top / bottom` divided by
  the map texture scale, shifted by `-map_tex_width` when the map wraps) overlaps the rectangle, every pixel of the country's own silhouette image
  (`conquest/RenderMaps/MapPieces/<map>/xpack_temp_country<N><map>.png`, alpha != 0) is drawn as `0xff000000` (land); the countries whose centre
  (`starting_coords_ws`) lies inside the rectangle are returned with their centre relative to the rectangle. The image is written to
  `<profile>/conquest/<file>` and read back unchanged.
- `ScenarioEditor::import_image(image, percent, ids, xs, ys, &out_x, &out_y)` @9a3f30: for every wcoord `(x, y)` the pixels of the box
  `[x / xs * side, (x + 1) / xs * side] x [y / ys * side, (y + 1) / ys * side]` (both ends inclusive, truncated) are averaged as
  `sum(rgb24 / 16777215) / count` (white = 1, land = 0); `flags &= 0xffc3`; `land = 2` (water) when the average is at least `percent / 100`, else 0.
  The returned start positions are the countries' centres scaled to the world (`int(pos / side * xs)`). Then `Regions::clear_all / find_all`,
  `Map::force_continent_spacing` and `Map::make_coastlines`.
- `make_continents` then adds the two starts (`add_starting_location`, the conquest lists as in B3.13 above), `add_additional_players` @68d260 (the
  barbarian / supporting seats: one `rnd(region size)` as the walk start, `check_starting_loc` @68d7e0 per candidate) and `check_sea_map`. The percent and the
  hidden register arguments of `check_starting_loc` / `add_additional_players` need an asm pass.
What `ctw_battle.cpp` would have to supply: the two countries' silhouette images and rectangles (the PNG pieces ship in `conquest/RenderMaps/MapPieces/`; the engine has no PNG
reader), the map-wrap shift and the scale, and `get_scenario_map_rect`'s rectangle rule (not decompiled).

### B3.14 `MapScenario::make(a, b, c)` @68c8f0 — style 26 and the campaign's custom maps
Frees the conquest start lists, then `ScenarioRead::import(game_style.custom_map)` (error "Unable to load custom scenario."); only when `semaphore.ptr[2] & 2` (a conquest game) it continues:
`game_style.player_start_x/y` (tcoords, `>> 2` = wcoords) and `ai_start_x/y` become start locations 0 and 1 (with the conquest lists `start_capital = 1`, `start_node` = the link's nodes picked by which one is
the player's tribe, `start_who` 0 / 1, `start_add_armies` 0), the player slots take the conquest leaders (`info.player[k].tribe`, name, team, `Leader::init`), `ConquestGame::setup_scenario_wonders`,
`add_additional_players` for the extra seats, then the age setup (`starting_technology = ending_technology = cur_age`, one more when `cur_age < ending_age`; every leader gains the techs `0x220 ..` up to the age and
marks the unit types that become available). That belongs to the conquest campaign (not to random maps).

### B3.15 `Map::run_map_script(a, b)` @68c870 / `MapScript::run_map_script` @68c860
`Map::run_map_script` builds a log string when `b ≠ 0` and calls the virtual slot `+0x40` with `a`; `MapScript::run_map_script` is an empty `RET`. No shipped style overrides it: **nothing happens** (it is a hook for scripted maps).

## C. What the port needs

1. `circle_init`, `vd`, `project` (with the sine table: read it from the exe, 256 entries), `move_*`, `orthog_*`, `corner_*` — bit-exact tables.
2. The primitives of §B1 (`point` returns, `stamp`, `large_stamp`'s 16 patterns and its 1–3 draws, `grow_region`'s return and give-up rule, `land_dist`, `invert_land`, `fix_lakes`, `scale_land_area`'s float division).
3. One function per recipe following §B3 with the draws in the order listed; the ones checked in the asm (B3.1, B3.2, B3.4, B3.6, B3.10, B3.11) can be ported now; B3.3, B3.5, B3.7–B3.9 and B3.12 need a further disassembly pass for the ❓ counters before a bit-exact claim. B3.1 and B3.2 are enough for a first playable land map (Old World family).
4. `TerrainGroups`: the `GROUPENTRY` reader (with `scale_number`), `fill_fertile` (own noise: `Fractal`), `place_all` with the grouping roll, clump draws, the `help_lowest_index` fairness and the four patterns; the two placers (the LFSR cell walk is exact), `drop_tile`, `available_rough_tile`, `place_oil_deposits`; then `nubify_forest`, the three base changers, `fix_transitions` / `nubify_transitions`, `add_doobers` and `treeify_mountains` **including their draws** (they shift every later `game_random` value).
5. Not decoded here and needed for exactness: `Mountains::add_mountain` (+ the mountain range data), `Cliffs::position_cliff / verify_defensive_position`, `World::set_oil_at / set_blocked_at / is_edge_of_region`, `WorldData::is_edge / start_city_wcoord / has_mountain_tcoords`, `Doober::add_doober / has_doobers`, `MapFairness` (Warring States).
6. Validation: record a random game's seed, style, size and players on the Windows build and compare the land mask, start locations and group tiles; until then only plausibility checks apply.

## D. Evidence

| what | function @ address |
|---|---|
| terrain groups | `TerrainGroups::init_terrain_data` @6a6540, `init_tileset_data` @6a61f0, `fill_fertile` @6a6f90, `place_all` @6a70d0 (switch @6a79a6), `TerrainGroup::TerrainGroup` @6a5df0, `place_player_group` @6a4190, `place_region_group` @6a2f60, `drop_tile` @6a2ab0, `available_rough_tile` @6a24d0, `place_oil_deposits` @6a2d90, `randomize_orthogs` @6a2320, `clear_group` @6a28e0 |
| passes | `nubify_forest` @6a93b0, `nubify_transitions` @6a1f30, `fix_transitions` @6a9f70, `treeify_mountains` @6a1cc0, `add_doobers` @6a1540, `change_forest_base` @6a9a60, `change_mountain_base` @6a9c10, `change_coast_base` @6a9dc0, `Map::check_player_forest` @68e8f0 |
| helpers | `Mountains::randomize_mountains` @89ca70, `get_range` @89cb50, `circle_init` @6817f0, `project` @92d000, `sin_table` @a46a00, `Map::scale_number` @6a0320, `scale_by_area` @6a0910, `scale_land_area` @68b1e0, `get_player_overage` @68b280 |
| primitives | `World::wipe` @6b2c00, `Map::make_region` @69d3f0, `point` @69d5a0, `stamp` @69cf80, `large_stamp` @69cb20, `grow_region` @69c600, `grow_valid` @69d000, `land_dist` @69d970, `invert_land` @69d880, `fix_lakes` @69c470, `MapGreatSahara::fix_lakes` @41c150 |
| recipes | OldWorld @69c0e0, Sahara @69bf70, Amazon @69be00, Himalayas @69bc90, SouthwestMesa @69bb20, AfricanWateringHole @69b650, AustralianOutback @69a650, GreatLakes @699e40, WarringStates @698b60, NewWorld @6984d0, ColonialPowers @697dd0, EastIndies @697540, EastMeetsWest @696640, SeaPower @696080, NileDelta @695bf0, BritishIsles @695130 |
| conquest / scenario | `MapConquest::make_continents` @68dba0, `check_sea_map` @68d1a0, `add_additional_players` @68d260, `check_starting_loc` @68d7e0, `MapScenario::make` @68c8f0, `Map::run_map_script` @68c870, `MapScript::run_map_script` @68c860 |
| data | `mapstyles/*.xml` (`default.xml` groups and `TILESET_DATA`), `Data/tilesets.xml` (`TERRAINGROUP`), PDB types `Map` (0x2f0: `start_mode` +0x24, `start_dist` +0x28, `avoid_center` +0x2c, `avoid_continent` +0x30, `avoid_edge` +0x34, `base_edge` +0x44, `sea_map` +0x48, `avoid_modify` +0x64), `TerrainGroup` (0x9c), `TileSetGroupData` (0x40) |

## Oracle: current 2.SVX (2026-10-24)

Confirmed against the save (cells placed that are also in the save): home trees groups 0 and 1 30/30 each, home mountains group 2 (ranges 9,8,7,14,13) 72/73, home rocks group 3 10/10.

- Pattern-0 tree sizes are rounded up to ODD (even +1), cap 9. The tree shape array is stale after a stuck growth and is reset only per clump.
- Direction and mountain-range lists are built by head insertion: forward walks read in reverse of file order (medium list 14..7). `randomize_mountains` draws medium (`%8`) then large (`%7`).
- `Mountains::add_mountain` @0089c2e0: mode 4 `excluding_verify` (drop_tile, region groups), mode 5 `sliding_excluding_verify` (player groups); `quick_verify_template` @00898ab0. Rings: mountain 0x10|0x40, coast 0x4, forest 0x20, rock 0x8.
- `place_region_group`: LFSR 0xB400 walk from `idx0 = rnd % n`, orthogonal growth only, returns the last seed-drop result.
- (2026-10-24, second pass) The world-pattern mediums (group 4) now reproduce exactly: all 27 mountains of the save, in order. The earlier
  "open" item was not a draw schedule: the region-1 list had 7231 cells where the original has 7236, which moved the LFSR start of
  every clump (`idx0 = draw % n`). The cause is in `Map::check_player_land` (below). The draw per clump is exactly one
  (`Random::get(0,0xffff) % n`), the walk is the raster-ordered list, and the 19 placements after the five home mountains were
  found by fitting `n` (7236 reproduced 19/19; 7231 reproduced 3/19).

### Corrections found against the save (second pass)

All verified on `current 2.SVX` (a fresh game, frame 189) and on the other saves of the oracle set (see the table in
`randommap-spec.md`).

| Function | Address | Fix |
|---|---|---|
| `Map::check_player_land` | 68ef00 | The circle loop walks the four **footprint cells** that `World::add_starting_location` @6b2de0 pushes for every start (lists `+0xc8` / `+0xe4`, count `+0xbc`: `(x,y) (x-1,y) (x,y-1) (x-1,y-1)`), not the start itself. Land is made under the union of four radius-`R` discs, `R = ceil(Battleship max_range / 4)`. This adds 5 lake-shore cells to region 1 in Great Lakes and fixes `n`. |
| `TerrainGroups::treeify_mountains` | 6a1cc0 | The fringe test was inverted. A wcoord with mountain tcoords that is not itself a mountain wcoord becomes forest when **neither** its (x, y+1) **nor** its (x+1, y) neighbour is a mountain wcoord (`flags & 0x404 == 0`, one draw); a mountain wcoord with an open +x or +y orthogonal neighbour becomes `0x30` (draw, `land = 0`). Forest cells now match 520/520. |
| `TerrainGroups::nubify_forest` | 6a93b0 | A run is tested for **continuation first** (`dir != 0` and forest at `x + dir`): a cell with forest on both sides still continues a run. The run is reset to 0 after **every** draw, whether the nub was placed or not. Both changes alter the number of draws (the original draws more). |
| `TerrainGroups::change_forest_base / mountain_base / coast_base` | 6a9a60 / 6a9c10 / 6a9dc0 | `Map::make` calls each only when the tileset base is `>= 0`. The growth neighbour must also have `land != 2` (mountain and coast variants lacked it, adding draws); source cells and grown neighbours get `land = 0`. |
| `Fractal::init` | 6aa2d0 | Rewritten from the decompile: grid `(xs+1) x (ys+1)` bytes, level `L = S..0`, cells `i << L, j << L` with `i < xs >> L`, `j < ys >> L` visited **x outer / y inner**, so horizontal, vertical and centre cells interleave their draws; neighbours past `ys` stay 0; column `xs` is a copy of column 0 at the start of every level and at the end. This is also the field of `fill_fertile`; the per-tileset check is the share of agreeing `land_sub` cells on plain land (81 percent before the other fixes, the remaining differences are `Terrain::init` and late-game changes in the old saves). |
| `TerrainGroups::add_doobers` | 6a1540 | Draw order was right. Doober positions are `(px, py)` in world units (768 per wcoord); `Doober::add_doober` draws nothing. `Doober::has_doobers` @8466d0 (disassembled, third pass) can never return 1: for a match on the x wcoord it builds `(float)(loc.y == (float)param_2)` (0.0 or 1.0), takes `div_3_table[(int)that >> 8]` = `div_3_table[0]` = 0 and tests that against 0, so the function returns 0 for every call; the rock pass therefore has no skip. The doober count is kept for `res.doobers`. |

The draw alignment after these fixes was verified with a statistical fit: for every `change_forest_base` growth draw a hit must put
`land_sub == 3` in the save. The best alignment moved from "+9 draws" (before the `nubify_forest` fix) to "+0" (after).

### What still differs on `current 2.SVX` (third pass)

- `land_sub`: 76 of 10000 cells (158 before this pass). They sit at class boundaries and next to the 30 half-land wcoords that
  `Terrain::init` still gets wrong at lake corners (shelf heights, see `randommap-spec.md`).
- One rock flag (16,36) and the derricks at (1,4) / (29,80) in the newer worlds: the moved derrick follows the lake-shore half-land cells.
  `Doober::has_doobers` is settled (always 0), so the earlier diagonal growth difference of the rock clump is not a doober skip.
- Goods 80/80 and goodies 50/50 now match (they were moved by `move_goody` / `move_ocean_goody`, `randommap-resources.md` section 13).
