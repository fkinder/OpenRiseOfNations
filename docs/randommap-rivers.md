# Random maps, part 2: rivers (`Terrain::init` → `Map::make_rivers`)

Status 2026-10-24: 🟡 decoded from the decompiles and checked against the disassembly (`Map::make_rivers`,
`make_riverlist`, `make_riverbeds`, `PathFinder::astar_river`, `RiversOut::start_river`, `RiverOut::add_spline_point`;
`re/decomp/rivers/`, local). Not compared with a running game: no recording of an original random map exists on this
Mac. Part 1 is `randommap-spec.md` (coordinate conventions, `rnd(n)`, `WData`, `Map::make`).

**Surprise: rivers are not made inside `Map::make`.** `Map::make_rivers` (vtable slot +0x4c of `Map`) has one caller,
`Terrain::init` @850f70 (called by `Setup::build_game` step 4 right after `Map::make`, `randomize_starts` kept, see
part 1 §1): it runs after `TerrainOut::generate_land` and `convert_beach_to_sandy`, only when `GameAccess::game_map` is
alive and `param_2 == 0`, then `RiversOut::calculate_all` builds the spline geometry and `game_map` is deleted. So
the river draws come **after** all of `Map::make` (terrain groups, resources, `compute_all_vals`) and after the start
order draws of build_game step 3. Loaded games and scenarios have no `game_map`: nothing is regenerated.

What the simulation keeps of a river is one `WData.flags` bit (§5): `0x1000` on every cell of the path. The `River`
objects (splines, sections, textures) are visual and are outside this document.

## 1. Inputs

- `Map +0x14` `river_thresh` (`<RIVER_THRESHOLD value="8">`, default.xml; no style overrides it).
- `Map +0x110` `stash_nations_here` (player count, set by `Map::make`); `Map +0x258` `river_beds`, an array of `(x, y)`
  int pairs (count +0x25c, capacity +0x260, data +0x268).
- `World`: `xs` +0, `ys` +4, `cells` +8, `WData` +0x134, tcoord bytes +0x138 (row length `+0x18` = 4·xs), `seed` +0x7c
  (build_game draw 1), start lists data `+0x90` (x) / `+0xac` (y), and the list read by `start_city_rad_wcoord`
  (count `+0xbc`, x `+0xc8`, y `+0xe4`, the one part 1 calls the oil list ❓ the function name says start city).
- `R = map_sizes.list[+0x144]` (same reference width as part 1 §4; ❓ which map-size entry, the rules `mapsizes`
  categories are 40, 50, 60, 70, 80...). `Map::scale_by_axis(v) = max(1, (R/2 + xs·v) / R)` (integer division).
- `RIVERLIST` of the style XML. `rules.xml` has **no** `river_*` / `riverbed_*` constants: everything numeric comes from
  the map style XML. The only rules constant read is `city_center_radius` (shipped `20 tiles`).
- Tables: `move_x/move_y` (49 offsets, index 0 = self, 1..8 the ring around), `circle_x/circle_y` (offset lists) and
  `circle_radius[k]` (number of offsets within radius k), `orthog_x/orthog_y` (index 1..4 = the four neighbours; in
  the A* in this order), `div_3_table[pos >> 8]` (position → wcoord).
- Position units in the A*: a wcoord `w` is the position `w·0x300 + 0x180` (768 per wcoord, centre of the cell);
  `div_3_table[pos >> 8]` converts back.

## 2. `Map::make_rivers` @692fb0

```
make_riverbeds()                                     (§3)
for every cell: flags &= ~0x1000                     (river path bit reset)
if river_beds.count == 0: return                     (nothing else, no draws)
path = this->file minus its extension, + ".xml";     (the style file; default is ".\mapstyles\default.xml")
list = RIVERLIST of the style file if `file` is non-empty and the element exists, else of default.xml
    (an empty <RIVERLIST/> in the style counts as present: that style has NO rivers.)
for each <RIVER> child, in file order:
    start  = attr start_range   (default -1)
    range  = max(0, attr end_range (default -1) - start)
    scaled(a) = a <= 0 ? 0 : max(1, (R/2 + xs·a) / R)          // the "SCALE" suffix text is ignored by the reader
    length    = scaled(river_length);  minlen = scaled(river_min_length);  valley = scaled(river_valley)
    wide, med, twist, flags = attrs river_wide, river_med, river_twist, river_flags   (default -1, unscaled)
    make_riverlist(start, range, min(length, 399), minlen, valley, wide, med, twist, flags)
for every cell: flags &= ~(0x2000 | 0x0002)          (bed marks and valley marks go away)
Regions::find_all
```
`get_attrib_num` parses the leading number of `"50 SCALE"`. Style data: default.xml (used by every style without a
`<RIVERLIST>`: WARRINGSTATES, the CONQUEST maps) has one entry `start 1 end 3 length 24 min 12 valley 8 wide 0 med 100
twist 60 flags 11`; OLDWORLD `2..4, 50/12/6`; AMAZON `4..6, 50/12/6`; GREATLAKES `1..2, 12/6/8`; NEWWORLD
`0..0, length 0, min 0, valley 8`; all others `<RIVERLIST/>` (no rivers: AFRICA, BRITISHISLES, COLONIALPOWERS,
EASTINDIES, EASTWEST, HIMALAYAS, MEDITERRANEAN, NILEDELTA, OUTBACK, SAHARA, SEAPOWER, SOUTHWESTMESA). All entries:
`wide 0, med 100, twist 60, flags 11`.

## 3. `Map::make_riverbeds` @694530 — the candidate sources

```
for every cell: flags &= ~(0x2000 | 0x0002)
for y in 0..ys, x in 0..xs (row-major, y outer):
    f = flags(x, y)
    require (f & 0x21) == 0x20                      // bit 0x20 set (forest, part 1 §7.3), bit 0x01 clear
    skip if has_mountain_tcoords(x, y)              // any of the 16 tcoords has (byte & 3) == 2
    skip if start_city_rad_wcoord(x, y)             // dist(x,y; entry) · 4 < city_center_radius - 1 for an entry of the +0xc8/+0xe4 list
                                                    // dist = max + min²/(2·max) of |dx|, |dy| (60000 guard: (min + 2max) >> 1)
    count = 0
    for i in 1..48:                                 // the 7×7 square around (x,y) in move_x/move_y order
        (cx, cy) = (x + move_x[i], y + move_y[i])
        if outside the map or flags(cx,cy) & 0x2000: count = 0; break      // an earlier bed within 3, or the edge
        if get_land(cx, cy, mode 1) == 2: count = 0; break                 // open ocean (a coast-flagged cell returns 3, no abort)
        if i < 9 and not has_mountain_tcoords(cx, cy) and not (flags & 8): count++   // only the 8 neighbours count
    if count >= river_thresh:  river_beds.push(x, y); flags(x, y) |= 0x2000
```
The first `if ((f & 0x74) == 0)` ladder of the decompile is dead code: `0x20` is inside `0x74`. No `game_random` draw. Beds
are therefore inland forest cells at least 4 apart (Chebyshev) whose 7x7 surroundings hold no open ocean, in row-major
order; with the shipped threshold 8 all eight neighbours must be free of mountains and of flag 8. ❓ The meaning of
flag 8 and of bit 0x01 was not traced (part 1 calls 0x100 the half-land bit and 0x04 the coast).

## 4. `Map::make_riverlist` @693820 — one `<RIVER>` entry

Parameters (stack order after `this`): `start, range, length, minlen, valley, wide, med, twist, flags`. `flags`: bit 1 =
may end in water, bit 2 = may end on the map edge, bit 4 = may end on an existing river (cell bit 0x1000), bit 8 = the
A* must make the river longer than `length` (otherwise the A* gets length target 0). Every shipped entry has `flags = 11`
(1 + 2 + 8: mouths in water or at the edge, never a confluence).

```
n = river_beds.count; if n == 0 return
d = range > 0 ? rnd(range + 1) : 0                    // DRAW A: Random::get(0, 0xffff) % (range + 1), once per entry
goal = max(1, d + start > 0 ? scale_by_axis(d + start) : 0)        // rivers wanted by this entry (>= 1 even for 0..0)
seed, taps = n > 4000 ? (world.seed & 0xffff, 0xb400) : n > 250 ? (world.seed & 0xfff, 0xca0) : (world.seed & 0xff, 0xb8)
s0 = seed % n + 1; x = s0; pass = 0; counters[8] = 0; mincount = 0; again = true
loop:
    x = (x & 1) ? (x >> 1) ^ taps : x >> 1             // Galois LFSR step, taps as above
    if x == s0: pass++; if pass >= 3: return
    if x > n: continue                                 // skip values past the bed count
    (sx, sy) = river_beds[x - 1]
    <§4.1 player fairness filter>  <§4.2 search>  <§4.3 acceptance>  <§4.4 emission>
    if not again: return                               // `again` is set by §4.3 (initially true)
```
The bed order is a pure function of `world.seed` and `n` (no `game_random` draw). The walk visits the beds up to three
times (`pass` 0, 1, 2), and a river is accepted only until `goal` is reached (`again` becomes false at `goal == 0` or
once `pass >= 2`). The GameLog lines (`river_range`, `river_base`, `river_goal`, `make_river_list`, `Rivergen=`) and
`do_all_non_input` (UI pump) have no simulation effect.

### 4.1 Fairness filter (only if `stash_nations_here > 0`)
`best` = the start location `i` in `0 .. stash_nations_here-1` (start lists `+0x90/+0xac`, not the list count) with the
smallest `dist((sx, sy), start_i)` (same `max + min²/2max` metric, first minimum wins). If `best >= 0` and
`counters[best] > mincount`, this bed is skipped (the loop goes on with the next LFSR value, nothing else changes).

### 4.2 The search
`P = PathFinder::astar_river(sx, sy, out_x[], out_y[], twist, (flags & 8) ? length : 0)` (§6) returns the number of
path points (0 = failure). Skip the bed when `P <= 0` or `P >= 400`. `(ex, ey)` = last point, `water` = the end cell is
not half-land (`flags & 0x100 == 0`) and `land` is 1 or 2, `edge` = `ex` or `ey` on the map border.

### 4.3 Acceptance (all must hold, else the bed is skipped without effect)
```
water        → needs (flags & 1)
edge, !water → needs (flags & 2)
!edge, !water→ needs (flags & 4)          // an existing river cell; never with flags 11
P >= minlen    and    P >= length / (pass + 1)   // signed integer division; the requirement relaxes on later passes
goal = max(goal - 1, 0);  again = goal != 0 and pass < 2
if stash_nations_here > 0:   // fairness counters, now by the END point
    k = nearest start location to (ex, ey);  counters[k]++;  mincount = min(counters[0 .. stash_nations_here-1])
```
No draw. (`maxcount`, kept by the binary, is never read.)

### 4.4 Emission: up to three `River` objects
```
if P <= 6:  a = b = c = P
else if water or edge or flags(ex, ey) & 0x1000:
    a = (100 - wide - med) · P / 100;   b = P·med / 100 + a;   c = P            // signed, truncating
else a = b = c = 0                                                          // nothing is drawn (unreachable with flags 11)
seg 1 : if a != 0:           r = RiversOut::start_river();  points i = 0 .. a-1
seg 2 : if b - a != 0:       r = start_river();             points i = max(a,1)-1 .. b-1       // one point of overlap
seg 3 : if c - b != 0:       r = start_river();             points i = max(b,1)-1 .. P-1
for each point (x, y):
    flags(x, y) |= 0x1000                                     // river cell
    if valley > 0:  reg = region(x, y);  for k in 0 .. circle_radius[valley]-1:
        (cx, cy) = (x + circle_x[k], y + circle_y[k]);  if inside the map and region(cx, cy) == reg: flags(cx, cy) |= 0x0002
    r.add_spline_point(x, y)          // control point (x·768+384, y·768+384, 0); length += distance/768
if edge: flags(ex, ey) |= 0x1000;  last river .add_spline_point(ex, ey);   then one point outside the map:
         (-1, ey) if ex == 0, else (ex, -1) if ey == 0, else (xs, ey) if ex == xs-1, else (ex, ys)
```
`wide 0, med 100` (every shipped entry) gives `a = 0, b = c = P`: a single river from the source to the mouth. Seg 1 and
seg 3 never exist in the shipped data. Flag `0x0002` marks the valley around the river: the A* refuses to enter such a
cell, which keeps later rivers (of the same `make_rivers` call, same region) `valley` cells away; it is cleared at the
end of `make_rivers`. `RiversOut::start_river` @888f70 allocates a `River` (0x2b8 bytes, `River::init`) and appends
it to the global `rivers` array (count `+0x44`, data `+0x50`).

## 5. What is left in the world after `make_rivers`

| Where | Content |
|---|---|
| `WData.flags & 0x1000` | every path cell (and the mouth cell); the only gameplay-visible trace of a river |
| `WData.flags` 0x2000 / 0x0002 | cleared again (bed mark, valley mark) |
| `Map.river_beds` | the bed list (map is deleted by `Terrain::init`) |
| `rivers` (`RiversOut`) | one `River` per emitted segment, control splines only; `calculate_all` then fits `Spline`s (`ensure_ocean_end`, `create_sections`, `Terrain::spot_update_land_batch(riverbase)`) |
| `Lands` records | `mask` (+0x40), `riverbed` (+0x44), `cost` (+0x48) from `<RIVERS>` (`init_rivers` @69e520, part 1 §4) |

`Map::init_rivers` @69e520 (vtable slot +0xc, called from `load_map_data`) only fills the three booleans per land from the
`<RIVER name=… mask=… riverbed=… cost=…>` attributes (true when the attribute is non-empty; default.xml: `BaseLand`
mask 3, riverbed 1, cost 20 → all three true). `Lands::find_key` resolves the name; unknown names are ignored. **None of
the three is read by `make_rivers`, `make_riverbeds`, `make_riverlist` or `astar_river`**; they belong to the terrain
renderer (river texture masks) ❓ not traced. ❓ whether `Regions::find_all` (run at the end of `make_rivers`) treats
`0x1000` as a barrier was not checked; the pathing of units over rivers (`0x1000` in `Terrain`/pathfinder code) is outside
this document.

## 6. `PathFinder::astar_river` @686690 — the river search

Signature `(sx, sy, out_x*, out_y*, twist, minlen)`; `minlen` is `length` when river flag 8 is set, else 0. It is a
**randomised best-first search whose key is not a path cost**: nodes are ordered by `value` (smallest first,
`first_open_node` @687970 takes the leftmost tree node and closes it), `value` is computed once per node from its own
length and one random roll.
```
start check: the four orthogonal neighbours of (sx, sy) must be inside the map, not 0x1000, not water (a half-land cell is not water)
             and not on the border, else return 0
open = { start (length 0, metric = cell index, value 0) };  closed = {}
loop until open is empty (return 0):
    node = pop smallest value; mark closed
    c = cell of node
    if c is half-land or not water:                       // expandable
        if flags(c) & 0x1000 or c on the border:  → finish(node)
        else expand(node)
    else:  → finish(node)                                  // water (land 1 or 2, not half-land)
finish(node): L = node.length
    if minlen < 1 or minlen < L:  copy the parent chain into out_x/out_y (index 0 = start .. L = node); return L + 1
    else: drop the node and go on with the search
expand(node): for dir in orthog 1..4 (this order):
    child = node moved one wcoord; child.metric = node.metric + xs·dy + dx (= cell index)
    drop if outside the map, if its metric is already in the open or the closed set, or if it is on the node's ancestor chain
    drop if flags(child cell) & 0x0002                      // valley of an earlier river
    neighbour test @686d58 (settled in the third pass, asm): for each of the child's 4 orthogonal neighbours q, skipping the node itself
    and positions outside the map:
        count (only when the CHILD cell is not water (not half-land, land 1/2), not on the border and not 0x1000): +1 when q is a 0x1000
        cell, or q is water (not half-land and land 1/2), or q is on the border (x or y = 0 / size-1; the border test also holds for a
        half-land q);
        if q lies on the node's ancestor chain (walked from the node up, the ROOT is not compared) the CHILD is dropped at once
    drop if count > 1
    if the child cell is water (not half-land) or a border cell or has 0x1000:           // terminal child
        D = dist(child, start)  (max + min²/2max metric)
        if D < minlen, or any orthogonal neighbour of the PARENT other than the child has 0x1000:
            discard this child AND the siblings already generated for this node; stop expanding this node (dead end)
        else: child.value = 0, child.length = L + 1, child.parent = node; discard the siblings generated so far and
              stop expanding: the child is the only new open node (popped next, ends through finish())
    else:
        drop if start_city_rad_wcoord(child) or WData byte +0x11 (`blocked`) != 0 or flags & 8
        r = rnd(20) + 10     // DRAW B: `PathFinder::calc_river_cost`: Random::get(0, 0xffff) % 20 + 10, once per surviving child
        child.length = L + 1
        if child.length > 1:                                                             // grandparent = node.parent
            straight (child.x == grandparent.x or child.y == grandparent.y): r *= twist + 1
            turn:  if twist == 0: r *= 10;   r *= 101 - twist
        l = node.length
        if minlen < l:          pen_a = pen_b = 0;           pen_c = l - minlen
        else if l < minlen/2:   pen_a = minlen - l;          pen_b = pen_a/2;           pen_c = 0
        else:                   t = minlen - l;              pen_a = t/2;  pen_b = t/4;  pen_c = 0     // t/4 rounds toward zero
        child.value = (pen_b + 2·pen_a)·10 + 1 + pen_c + r
        keep the child (open set, keyed by value); `value < 0` drops it
    then all kept children are inserted in the open tree in dir order, the node goes to the closed set
```
Consequences for a port: with twist 60 a straight step costs ×61 and a turn ×41 (rivers meander); with a long `minlen`
the search prefers deep nodes (the penalties fall as `length` grows) and only ends when it touches water, the border
or an existing river at least `minlen` away from the source with `length > minlen`.

Settled in the third pass (the two things that made the first port's paths shorter and its third source different):
- **Ties.** `Tree<PathNode*,int>::ordered_insert` @4796f0 is an unbalanced binary tree that sends an equal key to the LEFT
  (`cmp edi,[ecx+0x10]; jle`), and `first_open_node` @687970 takes the leftmost node: among equal `value`s the node inserted LAST is
  popped first (not the oldest).
- **Open / closed.** The open set is a red-black tree keyed by the cell index (`BRTree<TreeNode*,ulong>`, node +0x15 = "removed"):
  `first_open_node` only soft-deletes the entry. The closed set is a second tree that receives a node only when it was EXPANDED (also
  a node that ended as a dead end through the "discard the siblings" rule); a node that finishes (water / border / river) and is
  rejected by the `minlen` test is in neither set, so its cell can be reached again by another route. A child is dropped when its
  cell index is in the open set (not removed) or in the closed set; the ancestor test never compares the root, which is in the
  closed set anyway.
- **Pinch.** See the neighbour test above (the count only applies when the child is an ordinary land cell, a terminal child never
  has a count; the "ancestor neighbour kills the child" rule applies to every child).

## 7. Draw order of the whole step

1. `make_riverbeds`: none. 2. For each `<RIVER>` entry of the list (only when `river_beds.count > 0`): DRAW A once
(`rnd(range+1)` if `range > 0`, else none), then for each visited bed (LFSR order, §4) that passes the fairness filter:
DRAW B once for every child the A* keeps, in the order the A* expands (§6). Nothing else touches `game_random`.
Rejected candidates (acceptance fails, or `P >= 400`) still consumed their draws.

## 8. What the river path changes in the world, and what does not draw

`Map::make_rivers` leaves only WData `0x1000` on the path cells; everything below (section 9) runs in `RiversOut::calculate_all` right after it.
Neither the geometry nor `RiverSectionOut::create_doobers` touches `game_random`: the River fractal is seeded with 0 and the river doobers use
the separate `internal_random`. The total draw count of a map is therefore the one of sections 4 and 7.

## 9. `RiversOut::calculate_all` @888820: sections, trough, tiles (ported in the third pass)

Called by `Terrain::init` @850f70 with `(param_2, param_3) = (0, 0)` for a new game (`ensure_ocean_end` and the terrain edits run only then), once
per `River` object in creation order (`make_riverlist` makes one River per path for the shipped data: `wide 0, med 100` gives `a = 0, b = c = P`).

1. **Control points** (`RiverOut::add_spline_point` @885a40): `(wx * 768 + 384, wy * 768 + 384, 0)` for every path point; a path that ends on the
   map border (`edge`) gets `(ex, ey)` again and one point outside the map (`(-1, ey)` if `ex == 0`, else `(ex, -1)` if `ey == 0`, else
   `(xs, ey)`, else `(ex, ys)`); `length += dist / 768` in float after every point.
2. **`RiverOut::ensure_ocean_end` @885400** (more than two points): direction `d` = last - previous (normalised; the one before when it is zero);
   over the 49 cells `move[0..48]` around the last point's wcoord (cells outside the map count as water) every water cell (`land == 2`) whose vector
   from the last point has a non-negative dot product with `d` is a candidate and the nearest wins (float `sqrt`, `1e6` start); its centre is
   appended; then (quirk kept: the vector of the LAST water cell looked at, not of the winner) `(v - d)` normalised times 768 is added to the winner's
   centre and, when that wcoord is `land == 2`, appended too. A mouth that already sits in a lake finds itself (distance 0) and adds one point.
3. **Spline** (`Spline::calc_spline` type 3): clamped uniform cubic B-spline with `segs = (short)(length * 1.7)` (`rivers._256`) segments,
   normals = side vector `(dy, -dx, 0)` averaged with the previous segment's, walked by arc length (`Spline::next_at_dist`) in steps of
   `768 / 1.7`. Single precision throughout (`engine/render/rivers.cpp` is the same code, checked against the original's meshes).
4. **Sections** (`RiverOut::create_sections` @884e00): the width of section k is `(fractal(a0, a4) * 0.005 + 0.5) * 0.75` (`rivers._92` = 0.75
   wcoord; `Fractal(50, 50, 2, flags 2, seed 0)`, counters `a0` 0..49 then `a4`, both start at 0 for each river); the first previous width is 0.75;
   when the middle of a section lies on a water tile (`tdata & 0x30 == 0x20`) the river is in its delta: that section's width x1.5 and the
   following ones `min(1.5, 1.5 * previous)`. Each section calls `create_trough_edge` three times: start (width = previous width), middle
   (point and normal averaged, width averaged), end (width).
5. **`RiverSection::create_trough_edge` @889660**, nine samples across the river (`k = 0..8`, step = `normal * width * 192`, first sample 4 steps
   before the point): depth `z = (-60 - prof[k] * -60) + 15` with `prof = {1, .75, .26, 0, 0, 0, .26, .75, 1}`; on a water tile and `z < 0`
   and `|k - 4| < 2` it becomes `((z - 30) + z) / 2`. For a sample inside the map:
   - `RiverSectionOut::terraform_from_river_vert` @88b360: skipped when any of the 8 neighbours of the tile's wcoord is water (`land` 1 or 2) or
     half-land, or the tile is a water tile / the wcoord is `land == 1` / the tile is a mountain tile (`tdata & 3 == 2`); otherwise the wcoord is queued
     for the land update, the four corner heights of the tile become `(h + z) / 2` and the 121 tcoords `move[0..120]` around it are queued once
     for smoothing;
   - **marking**: for the 36 tcoords `move[0..35]` around the sample's tile: if any of its four corner heights is `< 0` and its wcoord has no mountain,
     forest, rock or oil flag (`0x10 / 0x28 / 0x800`), is not water (unless half-land), has no cliff flag `0x40`, `blocked == 0` and the wcoord's
     CoordInfo flags have no coastal bit (`& 4`), then `World::set_river_at` @6b1f80 sets `tdata |= 0x800` and `WData.flags |= 0x1000`;
   - `Terrain::move_goody` (land variant, resources section 9) on the sample's wcoord.
6. **Tail of `create_sections`**: `TerrainOut::ensure_45_deg_slope` @869890 on the queue until it is empty (a vertex whose neighbour tile (not mountain,
   not water) is higher than `h + 79.68` is lowered to `h + 79.68 - 4` when that is positive and queued again), then two `smooth_tcoord` passes
   over the ORIGINAL queue (`TerrainOut::smooth_tcoord` @86c3a0), and in `calculate_all` `Terrain::spot_update_land_batch` @84d540 sets
   `land_sub = riverbase` (`<TILESET riverbase=...>`, 2 for `early_autumn`) and `land = 0` on every queued wcoord and then runs `verify_land` on each.

The heights live in the same `master_land_heights` grid that `Terrain::init`'s coast pass left (`Result::heights`); `adjust_for_mountains` and `Cliffs::init`
(next in `Terrain::init`) are ported (randommap-spec.md fifth pass).

**Samples off the map** (sixth pass): in `create_trough_edge` @889660 a sample whose tile is outside the grid is not terraformed or marked, but the branch
clamps the running position (y < 0 -> 0, x < 0 -> 0, then both to `xs * 768`) and the next sample of the nine starts from the clamped point, so a river
that ends on the map border digs its edge row / column (the saved mouth vertices are at the trough bottom). `trough_edge` in `mapgen_terrain_init.cpp`.

## What the port needs

- A call of `make_rivers` from the equivalent of `Terrain::init` (after land generation, before the spline step), with
  the `Map` still alive for `river_beds`, `river_thresh`, `stash_nations_here`; nothing at load time.
- `RIVERLIST` reading with the default-file fallback (absent element → default.xml; empty element → no rivers),
  `scale_by_axis` with the reference width `R` ❓ (needed for `length`, `min_length`, `valley` and the goal).
- `make_riverbeds` as in §3 (needs forest bit 0x20, tcoord mountain flags, the list of `start_city_rad_wcoord`).
- The LFSR walk (§4) driven by `world.seed`, the fairness counters, the three-pass limit and the `goal` countdown.
- `astar_river` (§6) with the 0x1000 / 0x0002 / 0x2000 flags; DRAW A / B on the simulation stream in the order above.
- Output for the sim: `flags |= 0x1000` on the path cells. The spline / section geometry is a renderer matter and can be
  built from the path point lists (control points at `wcoord·768 + 384`, mouth on the border extended one cell outside).
- Open items: the ❓ above (R, bit 0x01 / flag 8 / byte +0x11, the start-city list identity, A* ties and dedup, the
  neighbour pinch test, `Regions::find_all` and 0x1000), and a recorded game from the Windows build as the oracle.

## Evidence

| Function | Address | Used for |
|---|---|---|
| `Terrain::init` | 850f70 (call site 852693) | where rivers are made (only caller, found with `InsnGrep`) |
| `Setup::build_game` | 5ac190 | calls `Terrain::init(0, param_3)` after `Map::make` |
| `Map::make_rivers` | 692fb0 | §2 (disassembled: attribute defaults −1, scaling, argument order) |
| `Map::make_riverlist` | 693820 | §4 (disassembled: LFSR, filters, acceptance, segments, counters) |
| `Map::make_riverbeds` | 694530 | §3 (disassembled) |
| `Map::init_rivers` | 69e520 | the three `Lands` booleans |
| `PathFinder::astar_river` | 686690 | §6 (decompile only, ❓ parts as marked) |
| `PathFinder::calc_river_cost` | 687930 | DRAW B form (`% 0x14 + 10`) |
| `PathFinder::first_open_node` | 687970 | smallest-`value` node first |
| `RiversOut::start_river` / `RiverOut::add_spline_point` | 888f70 / 885a40 | River objects and control points |
| `RiversOut::calculate_all` | 888820 | consumer of the `rivers` array |
| `WorldData::has_mountain_tcoords` / `start_city_rad_wcoord` / `get_land` | 6b3050 / 6b3850 / 6b4730 | bed tests |
| `Map` vtable | b45d68 | slots: `init_rivers` +0xc, `make_riverbeds` +0x44, `make_riverlist` +0x48, `make_rivers` +0x4c |
| Style XML | `mapstyles/*.xml` | `RIVERLIST`, `RIVER_THRESHOLD`; rules.xml has no river constants |

## Oracle: current 2.SVX and the three river worlds (third pass)

After the third pass the A* paths are exact: every generated river cell is in each save, and `DO NOT DELETE`, `DO NOT DELETE 2` and
`curent dec 14` match on all 0x1000 cells (1074 / 1202 / 768 differing before, 0 now) and on all tdata 0x800 trough tiles
(7510 / 8462 / 6049 before, 0 now). On `current 2.SVX` the cells go 202 save / 129 ours (78 common) -> 212 ours (201 common) and the trough
tiles 1730 -> 99 differing. The remaining river difference there sat on the lake shore, where the half-land and corner heights still differed;
the fourth pass fixed those heights (`randommap-spec.md`, "Fourth pass": the second influence line is chosen through the owner cell), after which
the saved river cells are all matched (202 of 202, 11 extra on lake shores) and so are all 1730 saved trough tiles (82 extra).

What closed it: the `astar_river` tie order (`ordered_insert` @4796f0 sends equal keys left, so the newest equal node pops first), the
open/closed dedup (closed receives only expanded nodes and dead ends), the ancestor-chain test, and the orthogonal neighbour pinch test
(@686d58); then the geometry of sections 8 and 9 (`create_trough_edge`, `set_river_at`, `ensure_45_deg_slope`). The saved control splines
and `ensure_ocean_end` points equal ours point for point; the spline lengths match (29729.10, 24397.44, 23747.65).
