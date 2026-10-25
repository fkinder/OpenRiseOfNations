# Random maps, part 3: resource placement, goody boxes, fish and herds

Status 2026-10-24: 🟡 decoded from the disassembly (`map.obj`, `objects.obj`, `herds.obj`); the Ghidra decompiles of
`place_player_resource` / `place_region_resource` / `place_resources` drop most `return`s, so every condition below was
read from the `.asm` (Ghidra output kept locally in `re/decomp/rm_res/`). Not compared with a recorded original map: nothing
here is validated against the game. Read `randommap-spec.md` first (coordinates, `rnd(n)`, `WData`, regions, `Map::make`).
Corrections to part 1 found on the way are in §10.

Entry point: `Map::place_resources(n)` @68f4f0, called by `Map::make` (part 1 §3) after the terrain groups, unless map editor
or a Conquer-the-World style that forbids rares. `n` = number of nations. It returns the number of objects placed.

## 0. Conventions used below

- `rnd(k)` = `Random::get(game_random, 0, 0xffff) % k` (non-negative operands). Every draw is on the simulation stream.
- `vdist(dx, dy)` (the `circle_dist` of combat-spec §2.17.1, also the `vector_dist` form): `hi = max(|dx|, |dy|)`, `lo` the
  other; `0` when `hi == 0`, else `hi + lo² / (2·hi)` (unsigned); `lo ≥ 60000` → `(lo + 2·hi) >> 1` (never reached).
- `center_d(x,y) = vdist(x − xs/2, y − ys/2)` (C division); `edge_d(x,y) = min(x, y, xs − x, ys − y)`;
  `corner_d(x,y,c) = vdist(x − cbx[c]·xs, y − cby[c]·ys)` with `cbx = {0,0,1,1}`, `cby = {0,1,0,1}` (@add2a0 / @add2b0);
  "corner_d" without a corner = the minimum over c = 0..3, started at 9999.
- Ring of a start: cell indices `k ∈ [circle_radius[a], circle_radius[b])` of `circle_x/circle_y` (combat-spec §2.17.1: rings in
  raster order, ring r = cells with `circle_dist == r`), so the ring is the annulus `a < dist ≤ b`.
- WData flags used (world-spec §2.2): 4 COAST, 8 ROCK, 0x10 MOUNTAIN, 0x20 FOREST, 0x40 CLIFF, 0x100 HALFLAND, 0x800 OIL,
  0x1000 RIVER; `+0x11 blocked`; `down` / `down_who` (+8 / +0xa). `land` +2: 0 land, 1 shallow, 2 ocean.
- **Land class** `u(cell)` indexes the `Lands` records of `<RESOURCES>` (part 1 §4; record = 0x138 bytes, **rare count at +0x4c,
  rare good keys from +0x50**; part 1 wrote +0x10): `COAST → 3`; else `FOREST → 4`; else `flags & 0x50 → 5`; else `ROCK →
  (OIL ? 7 : 6)`; else `HALFLAND → land`; else `land ∈ {1,2} and OIL → 7`; else `land`. The shipped `default.xml` fills only
  LAND_0 (the 29 pool goods 7..36 except Whales, Salt twice) and LAND_2 (Fish 6, Whales 31); no style overrides them, so
  **a good may only sit on a plain land cell (u = 0), Fish and Whales only where u = 2 (deep ocean, no COAST / FOREST / ROCK / OIL flag)**.
- `Lands[2].rare` is "the water list" (count at record 2 + 0x4c = `lands+0x2bc`, keys at `lands+0x2c0`).

## 1. The `<BONUSES>` / `<FISH>` / `<GOODIES>` elements

`place_resources` runs three passes in this order: **0 BONUSES, 1 FISH, 2 GOODIES**. For each pass the element is taken from the
style XML when the style file exists and has that element (even an empty one: `<FISH/>` in Amazon, Himalayas, Old World, Sahara,
Southwest Mesa means no fish); otherwise from `mapstyles\default.xml` as a whole (no per-entry merge). Styles without `<GOODIES>`
therefore get the 5 default goody entries (Africa, British Isles, East meets West, Great Lakes, Himalayas, Nile Delta, Outback,
Sahara, Southwest Mesa, Conquest maps). Children are `<BONUS …/>` elements, processed in file order. Attributes:

| attribute | read as | meaning |
|---|---|---|
| `type` | `Types::good_key` → good index; else (case-insensitive) `Random` → kind 1, good −1; else the first 5 chars of item type 0x21f's name (`Goody`) → good 0x21f; `Early` → kind 2, good −1; `Late` → kind 3, good −1; else **the entry is skipped before any draw** | what to place |
| `chance`, `group` | `get_attrib_num`, default −1 | selection (§2.1) |
| `numrare` | `scale_number` | count (per player for `player`, else in total) |
| `pattern` | `player`=0, `continent`=1, `world`=2, `nonplayer`=3, `corner`=4 | where (§5, §6) |
| `saturate` | number, <0 → 0 | `≠ 0` disables the balancing rule of §6.4 |
| `player_keep_away` | `get_attrib_num` (**not scaled**: the `SCALE` word in the files is ignored) | `pka`, min `vdist` to every start |
| `player_stay_near` | `scale_number`; for `player` a negative becomes 0 | `psn`, max distance (0 = no limit / 64) |
| `spacing`, `group_spacing` | `scale_number`, < 1 → 1 | `sp`: to existing objects; `gsp`: to objects placed by this entry |
| `center_keep_away` / `_stay_near`, `edge_…`, `corner_…` | `scale_number` | `ck cs ek es cok cos` |

`scale_number` (part 1 §4) reads the attribute named in `global_cur_element2_string`; **a negative number N becomes `|N|·n`
(n = number of nations) before the `AREA / SCALE / SCALEWATER / SCALELAND` word is applied** (`-2 SCALE` = `scale_by_axis(2n)`).
Unknown words after the number (`far`, `num`, `space`, `grspace`) are ignored. An unknown `pattern` string leaves the previous
value in a register ❓ (no shipped file has one; shipped patterns are only `player`, `world`, `nonplayer`, `corner`).

`default.xml` (the table most styles inherit): BONUSES = Early / player / `n=1`, pka 6, psn 10, sp 6 SCALE, ck 20 SCALE; Early /
player / 1, pka 12, psn 18 SCALE, sp 8 SCALE, ck 16 SCALE; Early / world / `4 SCALE`, pka 16, sp 8 SCALE, cs 20 SCALE; Late /
world / `4 SCALE`, pka 16, sp 6 SCALE, gsp 12 SCALE, cs 20 SCALE (pka is never scaled). FISH = Whales / world / `8 SCALE`, pka 6, sp 3, gsp 8, group
6; Fish / world / `24 SCALE`, **saturate 1**, pka 4, sp 3, gsp 3, group 5. GOODIES = 4 `player` entries (n 1 / 2 SCALE / 4 SCALE /
4 SCALE; pka 8 / 8 / 15 / 15; psn 12 / 15 SCALE / 30 SCALE / 60 SCALE) and 1 `corner` (n 1, pka 8, cok 3, cos 8 SCALE).
Other styles use `nonplayer` (Mediterranean, Colonial Powers, Warring States, New World), `Random` (Colonial Powers, Warring
States) and negative `numrare` (`-1`, `-2`, `-8 SCALE`, meaning ×n); no shipped file uses `continent`.

## 2. `Map::place_resources(n)` @68f4f0

1. Pool: for good `g = 6..49` ascending: add to `resource_pool` (`ResourceDivvyPool` at `Map +0x274`, §3) when
   `GoodType.random_rare_drop (+0x2f8) ≠ 0` and not (CtW (`semaphore byte2 & 2`) and `exclude_ctw (+0x2f4) ≠ 0`). (`Map::make` also skips the whole `place_resources` call in a CtW battle whose style has `no_rare`: `mapgen::ConquestInput::no_rare`, fourth pass.)
   In `resourcerules.xml` that is goods **6..36** (Fish 6, Relics 7 … Titanium 36), minus Marble 20 under CtW. Then
   `done_adding_goods`.
2. `prev_group = −1`, `total = 0`. For `pass = 0..2`: pick the element (§1); for each `<BONUS>`:
   1. Resolve `type` (§1 table). Unresolvable → next entry (no draw).
   2. **Chance** (`chance` c, `group` g): if `g ≠ prev_group or g == 0`: `prev_group = g`, **draw `roll = rnd(100)`**, `hit = 0`; else
      reuse the group's remaining `roll` and the `hit` of the previous entry. If `roll < 0` (an earlier member of the group already
      won): continue only when `c == 0 and hit` (a zero-chance entry rides along with the preceding winner); else `hit = 0`, next
      entry. Otherwise `roll −= c`; `roll < 0` → `hit = 1` (this entry is placed) else `hit = 0`, next entry. So a group is a
      weighted pick in file order, and every `group = 0` entry draws its own `rnd(100)` (`chance = 100` always wins).
   3. `count = scale_number("numrare")`; 0 → next entry (the result is never negative: `-2 SCALE` = `scale_by_axis(2n)`).
   4. Read `pattern`, `saturate`, `pka`, `psn`, `sp`, `gsp`, `ck`, `cs`, `ek`, `es`, `cok`, `cos` (§1).
   5. `pattern == player` → `total += place_player_resource(n, good, count, pka, psn, sp, gsp, ck, cs, ek, es, cok, cos, kind)`;
      else `total += place_region_resource(n, good, pattern, saturate, count, pka, psn, sp, gsp, ck, cs, ek, es, cok, cos, kind)`.
3. Return `total`. `kind` = 0 for a named good, 1 `Random`, 2 `Early`, 3 `Late`. **In both placers `kind ≠ 0` draws the good from the
   pool (`kind == 2` → `get_early`, any other non-zero → `get_late`), so `Random` behaves exactly like `Late`.**
   The water pool (`get_water`) is only reachable for a named water good with `kind ≠ 0`, which no file produces: dead in practice.

## 3. `ResourceDivvyPool` (a shuffle bag per list)

Three int lists (`early`, `late`, `water`) and a bit mask per list. `add_good(g)` @68a830, in call order:
1. if `g ∈ Lands[2].rare` (the water list): append `g` to `water`; Fish (6) is appended **10 times** in total (the extra 9 are
   added right after), Whales (31) once; return.
2. else `age = GoodType.age (+0x278)` (`get_age_slow` when −1: the age of the preq tech, +1 for an age tech, 0 without preq):
   `age < 3` → append to `early`, else to `late`. With the shipped rules: **early = 7..14, 17..29, 32, 33 (23 goods), late =
   15 Rubber, 16 Sulphur (preq Gunpowder Age), 30, 34, 35, 36 (preq Industrial Age)**. ❓ the age of a good goes through the
   generic `ObjectTypeData::get_age_slow` (rules-spec §7.6), not re-read for goods; Sulphur = late assumes Gunpowder Age gives 3.
`add_early/late/water(n)` exist but have no caller. `done_adding_goods` sizes each mask to its list and clears it.
`get_early / get_late / get_water` @68a640 / 68a570 / 68a4a0:
```
loop: i = (len − 1 > 0) ? rnd(len) : 0             // one draw per attempt, only when len > 1
      if bit[i] clear: set it; result = list[i]; break
after a pick: if every bit is set, clear all bits   // the bag refills
```
So goods come out in random order without repeats until the list is exhausted. `refresh_*` clear a mask (no caller seen).

## 4. `place_player_resource` @691f70 (`pattern == player`)

Arguments `(n, good, count, pka, psn, sp, gsp, ck, cs, ek, es, cok, cos, kind)`. `water_class = good ∈ {6, 31}` (fixed from the
*argument*, so pool-picked goods are always land class). `placed[]` = a local list of the cells filled by this call (all players).
```
S = world start count
for i in 0 .. n−1 (skip i ≥ S, or a start outside the map):               // (sx, sy) = start i
  for c in 0 .. count−1:
    if kind ≠ 0: good = (kind == 2) ? get_early() : get_late()
    psn = (psn > 64 or psn == 0) ? 64 : psn ; pka = min(pka, psn)         // stored back: later iterations see the clamped values
    w = circle_radius[psn] − circle_radius[pka]          // pka < 0 (attribute missing) would index before the table; no shipped player entry omits it
    if w ≤ 0: continue                                                      // no draw
    off = rnd(w + 1)                                                        // draw (w ≥ 1)
    for j in 0 .. w−1:  k = (off + j) mod w + circle_radius[pka]; (x,y) = (sx + circle_x[k], sy + circle_y[k])
        if accept(x, y): place; append (x,y) to placed[]; break            // else next ring cell; ring exhausted = nothing placed
```
`accept(x, y)` (all must hold; no draws; order irrelevant): in map; not in the forest bit set (`world +0xf8`); `flags & 0x1000` clear;
`bad = flags & 0x1878 or blocked ≠ 0` (MOUNTAIN, ROCK, FOREST, CLIFF, OIL, RIVER) false;
1. `pka > 0`: `vdist` to **every** start `≥ pka`;
2. `vdist` to start i `≤ psn`; and **strictly closer to start i than to every other start** (`vdist(i) < vdist(j)` for j ≠ i);
3. `ck ≠ 0`: `center_d ≥ ck`; `cs > 0`: `center_d ≤ cs`; `ek > 0`: `edge_d ≥ ek`; `es > 0`: `edge_d ≤ es`;
   `cok > 0`: min `corner_d ≥ cok`; `cos > 0`: min `corner_d ≤ cos`;
4. `Terrain::has_south_forest` false (FOREST on (x+1,y+1), (x,y+1) or (x+1,y));
5. `gsp > 0`: `vdist` to every cell of `placed[]` `≥ gsp`; then `x ∉ {0, xs−1}` and `y ∉ {0, ys−1}` (**the original compares `y`
   with `xs − 1`**, a bug a port must keep on non-square maps);
6. `sp > 0`: for every valid object in `objects.goods` (slots `< objects+0x154`, type index ≠ 5 Oil; a Fish / Whales good only
   counts when `water_class`, any other only when not) the `vdist` between its stored position (x/y fields +0x10 / +0x14 are
   XOR 0x63637, then `div_3_table[c >> 8]`) and (x, y) must be `≥ sp`; for a goody box the same over all valid `items`;
7. goods: none of the four orthogonal neighbours (N, E, S, W of `move_x/y[1..8]`) in the map has COAST; `has_mountain_tcoords`
   (any of the 16 tcoords of the wcoord has `mask & 3 == 2`) false; `Lands[u].rare` contains `good`.
   Goody box: not `(HALFLAND clear and land ∈ {1,2})`, `flags & 0x70 == 0`, `has_mountain_tcoords` false;
8. the cell is not occupied: walk `down`: `down < −1` (−2 = a good, −3 = an item, set by §7) rejects; a chain of objects
   ending at −1 is accepted.
(The original also has a "same region as the start" test when `psn == 0`; it is dead because `psn` was clamped to ≥ 1.)

## 5. `find_avail_regions(list, sea, pattern)` @68f380

`list` is a circular doubly linked list whose `add` @47afa0 inserts **before the head and becomes the new head**, so walking `next`
from the head visits the added ids in **descending** order. Candidates: land regions 1..63 (`sea == 0`) or sea regions 64..126,
with `size (+0x14) > 5`; for `pattern == 3` (nonplayer) skip a region that contains the wcoord of any start. Then
`idx = (len > 1) ? rnd(len) : 0` (**draw**) and the "current" element is moved `idx` hops along `next` from the head (stops at
`len − 1`). The list persists through §6 (`current` is what the algorithm advances).

## 6. `place_region_resource` @690480 (`continent`, `world`, `nonplayer`, `corner`)

Arguments `(n, good, pattern, saturate, count, pka, psn, sp, gsp, ck, cs, ek, es, cok, cos, kind)`.

### 6.1 Control flow
```
water = good ∈ Lands[2].rare   (good ≥ 0 and ≠ 0x21f)
find_avail_regions(list, water, pattern); empty → return 0
th = ∞ ; if count > 2 and saturate == 0 and (pattern ∉ {1,3} or list.len == 1): th = count >> 1       // §6.4
rounds = (pattern == 1) ? list.len : (pattern == 4 ? 4 : 1);  score[0..7] = 0; worst = 0; placed[] = {}; last_region = list.current
for r in 0 .. rounds−1:                                  // r is also the corner index for pattern 4
  k = 0
  while k < count:
    if kind ≠ 0: good = water ? get_water() : (kind == 2 ? get_early() : get_late())
    R = list.current ; cells = R.cells (count m = R +0x70)
    start = rnd(m) + 1  (m > 1, else 1)                                               // draw
    x = start
    do { cell = cells[x − 1]; if accept(cell) → place, break;
         step: if x & 1: x ^= 0x16800 ; x >>= 1;  repeat while x > m ;  } until x == start     // §6.2
    if pattern ∈ {2,3}:                                   // round robin over the regions
        list.current = list.current.next
        if nothing was placed this attempt:
            if list.current ≠ last_region: k −= 1             // retry in the next region (no unit consumed)
        else: last_region = R                                  // remember the region that just succeeded
    k += 1
  if pattern == 1: list.current = list.current.next
```
So `continent` fills `count` per region in `list` order; `world` / `nonplayer` spread resource after resource over the regions in
descending-id order starting at a random one, giving up on one unit only after a full cycle of failures; `corner` tries `count` per
corner **always in the same (current) region**, which only works when that region reaches the corner (otherwise it fails silently
apart from log lines). Returns the number of objects placed.

### 6.2 Cell order
The cells of a region are visited in a pseudo-random order produced by a 16-bit maximal Galois LFSR (taps 0xB400): `start =
rnd(m) + 1` is tested first, then `x ← (x >> 1) ^ (x & 1 ? 0xB400 : 0)` until it returns to `start`; values `> m` are skipped
(regions have < 65 536 cells). Every cell is tested at most once; the first accepted cell wins.

### 6.3 `accept(x, y)` for regions (no draws; first failing test rejects)
1. `flags & 0x1000` (RIVER) clear; `pka > 0`: `vdist` to every start `≥ pka`;
2. `flags & 0x1878 == 0` and `blocked == 0` (same set as §4);
3. `psn > 0`: **`vdist` to every start `≤ psn`** (the cell must be near all players, unlike §4); `ck > 0`: `center_d ≥ ck`;
   `cs > 0`: `center_d ≤ cs`; `ek > 0`: `edge_d ≥ ek`; `es > 0`: `edge_d ≤ es`; `cok > 0`: `corner_d ≥ cok`; `cos > 0`: `corner_d ≤ cos`
   (for `corner`, `corner_d` is the distance to corner `r` only, else the minimum over the four);
4. `gsp > 0`: `vdist` to every cell in `placed[]` (all earlier placements of this call) `≥ gsp`;
5. `sp > 0`: the object test of §4.6 (`water` replaces `water_class`; goody → all valid items);
6. not water: none of the 8 neighbours `move_x/y[1..8]` in the map is a non-HALFLAND water cell (`land ∈ {1,2}`); water good: none of
   the 24 neighbours `[1..24]` has `land == 0`;
7. `has_mountain_tcoords` false; `x ∉ {0, xs−1}`, `y ∉ {0, ys−1}` (same `xs − 1` bug for y); `has_south_forest` false;
8. goods: `Lands[u].rare` contains `good` (water good: also COAST clear); goody: `flags & 0x70 == 0`;
9. occupancy: `down < −1` rejects (as §4.8);
10. balancing (§6.4) when `k ≥ th`.
Placement (§7); then `score[p] += vdist((x,y), start p)` for every start p and `worst = argmax score` (first maximum, strictly
greater than 0; stays 0 while all are 0); `placed[] += (x,y)`.

### 6.4 The balancing rule ("saturate")
When `th < ∞` and `k ≥ th` (the second half of the units of this pass), a candidate additionally needs its **nearest start**
(minimum `vdist`, first index on ties) to be `worst`: the player with the largest accumulated distance to the resources
placed so far in this call gets the next one. `saturate = 1` (Fish) disables it. Also logged (no effect): per-reason reject
counters, `Region Resource placed: … scores`, `FAILED TO PLACE RESOURCE!` when an attempt places nothing.

## 7. What a placement does

- Good: `Objects::init_good(good, X, Y)` @653f30 with `X = x·768 + off`, `off = 0x120` if the good's `x_size (+0x234)` is odd,
  else `0x180` (goody `0x180`, `Objects::init_item(0x21f, …)` @653e00). Takes the first free slot of `objects.goods` (or appends a
  0x34-byte Good, raising the high-water count `objects +0x154`), calls `Good::init` @66da20 (virtual +0x8c): marks `TData`
  `|= 0x200` (RESOURCE) over the good's `x_size × y_size` tcoord footprint, applies `block (+0x2f0)` when set, sets the wcoord's
  WData flag 1 RESOURCE. Unless the good is Oil (5), the wcoord gets `down = −2, down_who = slot`. `Item::init` @6770e0 sets WData
  flag 0x8000 GOODY and `Objects::init_item` writes `down = −3, down_who = slot` (what §4.8 / §6.3.9 test).
- Herd goods (`GoodTypeData::is_herd_type`) also register a herd: `Herds::init_herd(x, y, unit_type, good_slot)` @741bb0 with
  Fish 6 → unit 411 (0x19b Herd Fish), Horses 14 → 407 (Herd Horse), Furs 21 → 410 (Herd Bear), Bison 23 → 409 (Herd Bison),
  Peacocks 25 → 413 (Herd Peacock), Whales 31 → 412 (Herd Whales), Wool 33 → 408 (Herd Sheep). Other goods: no herd.
- Oil is not placed here (`TerrainGroup::place_oil_deposits`, terrain-groups part); `OIL`-flagged cells are only excluded.
  `World::add_oil_location` is not called by any function in this document.

## 8. Herds (`herds.obj`)

`Herds::init_herd` reuses the first Herd whose valid byte (+0x1a) is clear, else appends a 0x24-byte `Herd` (array `herds`, grown
in blocks of 10 pointers at `Herds::init` @741e20). Fields: +0 x, +4 y (wcoords), +8 / +0xc target (x, y), +0x10 unit type, +0x14
good slot, +0x18 i16 herd id, +0x1a valid. `Setup::init_wild_life` @5aac70 is `return 0` (an empty stub).

`Herd::create_units` @7417f0 (called for every valid herd by `Setup::build_game` step 10 / `Herds::start_herds`, in herd slot order):
`units = 4`, except type 412 (Whales) 1 and type 411 (Fish) 3. For each unit: `Objects::init_unit(8 (gaia), type, x·768+384,
y·768+384, −1, −1, −1)`; `UnitType::find_nearby_spot(…, radius 0xc0, …)` (no draw seen) → `(sx, sy)`; angle: Fish → 0, Whales →
0x80000000, every other herd type → **draw `degrees_to_angle(rnd(360))`** (the draw happens only in that case); `Unit::set_angle`,
`Unit::set_new_location(sx, sy, 1, 1)`; unit +0x86 = herd id. Fails (returns 0) if `init_unit` fails. ❓ draws inside
`Objects::init_unit` were not checked.

`Herds::process` (per frame, frame-spec): when `frame & 63 == 0`, `i = (frame >> 6) mod max(5, herd count)`; if herd i exists and is
valid, `Herd::process` @741760: **draw `a = rnd(3)`, draw `b = rnd(3)`**; `(tx, ty) = (x − 1 + a, y − 1 + b)`; if inside the map, WData
`flags & 0x70 == 0` and `blocked < 8`, the herd's target (+8 / +0xc) becomes `(tx, ty)` (the draws are consumed either way). The
herd's units wander towards it (unit AI, outside this document). `Herd::close` frees the herd when its good is removed.

## 9. Start helpers (corrections of part 1 §8) and `MapFairness`

- `find_region_centroid(r, &x, &y)` @68ae50: `x = Σ cell.x / size`, `y = Σ cell.y / size` over the `size (+0x14)` cells, signed C
  division (a zero-size region faults). No draws.
- `is_near_ocean(&x, &y, a, b)` @68b0a0 (the sense of part 1 was inverted): `false` if (x, y) is off the map. If `b ≠ 0`: `b ± 1 > 64`
  → false; some cell with `land ≠ 0` must exist for `k ∈ [circle_radius[b−1], circle_radius[b+1])` (in-map cells only), else false.
  If `a ≠ 0`: `a > 64` → false; any in-map cell with `land ≠ 0` for `k ∈ [circle_radius[0], circle_radius[a])` → false. Else true.
  I.e. **no water within radius `a` but water in the band around radius `b`** (`b = 0` skips the band test).
- `place_start_in_region(r, &ox, &oy, min_dist, p5, ax, ay)` @68ac00 (`ax/ay` = start arrays, default `world.start_x/y`; **`p5` is unused**):
  for `t = 1, 2` (margins `lo/hi = 5/6`, then `3/4`): `s = size > 1 ? rnd(size) : 0` (**draw**, `size = +0x14`); then for `size`
  candidates `idx = s+1, s+2, …` (wrapping `idx+1 < size ? idx+1 : 0`, ending with `s`): cell `(x, y) = cells[idx]` needs `x ≥ lo`, `y ≥ lo`,
  `x ≤ xs − hi`, `y ≤ ys − hi`; `md = (t == 2) ? min(min_dist, 6) : min_dist`; `vdist` to every existing start `≥ md`; then
  `is_near_ocean(x, y, a, b)` with `(a, b) = (3, 9)` for `t = 1` and `(2, 0)` for `t = 2`. First candidate that passes is written to
  `ox, oy` and the function returns 1; after both tries returns 0.
- `MapFairness` @68a100.. (a helper of `MapWarringStates::make_continents` only; scale argument is the constant 1.0 @b69514).
  Layout: `fairness[8]` +4, `dists[8]` +0x24, `lowest_player` +0x44, `highest_player` +0x48, `lowest_dist` +0x4c, `highest_dist` +0x50,
  `teams[8]` +0x54 (constructor: 0..7), `num_players` +0x74. `clear`: zero both float arrays. `init(n)`: clear, `num_players = n`.
  `calc_distances(&x, &y, s)`: for i in 0..num_players−1, `p = teams[i]`: `dists[p] = float(vdist(start_x[p] − x, start_y[p] − y)) · s`;
  `lowest_dist` = the index p with the smallest `dists` (strict `<`, init 1e12), `highest_dist` = the index with the largest (strict
  `>`, init 0) (the fields hold *player indices*). `apply_distances` / `unapply_distances`: `fairness[p] ±= dists[p]` over `teams`.
  `recompute`: `lowest_player` / `highest_player` = the p with the smallest / largest `fairness[p]` (same strictness).
  `point_lowers_highest` = `lowest_dist == highest_player` (the candidate is nearest to the player who is currently farthest from
  everything). `enable_team_mode(k)`: if `k < num_players`: `m = max(num_players / k, (num_players + 1) / k)`, `teams[i] = i / m`.
  Warring States uses it the same way as §6.4: try a candidate point (`land_dist` clearance first), accept only when the nearest
  start is the neediest, then `apply_distances` + `recompute`.
- `Map::calc_bonus(x, y)` @692f10 / `calc_bonus2` @692e70 (vtable slots 0x50 / 0x54; **no caller exists in the exe**, a leftover):
  `f = region(x,y).common_factor` (`goody_factor` for calc_bonus2; the map's own value when the cell's region is 0);
  `calc_bonus = ((x % f)·f + y % f) == ((y / f)·0x11 + (x / f)·0x3d + world.seed) % f²`, `calc_bonus2 = … == ((x / f)·0xd +
  (y / f)·0xb + world.seed) % f²`: one hash-picked cell per f×f block. Division by zero when f = 0.

## 10. Corrections to part 1

`scale_number`: a negative number means `|N|·n` (§1). `<RESOURCES>` records: count at +0x4c, keys at +0x50 (§0); an unknown `RARE`
name (key −3) is skipped. `is_near_ocean` and `place_start_in_region` as in §9 (the "p5" ring and the mid-loop return are not
there). `find_avail_regions`' list is in descending id order (§5). `find_region_centroid` is a plain mean over `size`.

## 11. What the port needs

1. `scale_number` with the negative-×-n rule; the XML pass logic (§1, §2: element fallback per pass, the chance/group state machine
   with `prev_group`, `roll`, `hit`, the skip before any draw for unknown types, and the `rnd(100)` per group-0 entry).
2. `ResourceDivvyPool` exactly as §3 (goods 6..36, early/late/water lists, Fish ×10, bag refill); the draw order inside `get_*`
   (one `rnd(len)` per attempt, re-drawn on a used slot).
3. Shared predicates: `vdist`, ring tables from `compass.cpp`, the land class `u`, the `xs − 1` quirk, the object-spacing test with
   the obfuscated positions replaced by the engine's own coordinates, the `down = −2 / −3` occupancy marks.
4. `place_player_resource` and `place_region_resource` with the draw order of §4 / §6.1 (pool pick → `rnd(w+1)` / `rnd(m)+1`;
   `find_avail_regions` draw first), the LFSR walk, the round-robin retry rule and the balancing rule with its `score[]` / `worst`.
5. `Objects::init_good` / `init_item` effects and the herd table of §7; `Herd::create_units` (counts, angle draw) and the
   `Herds::process` wander draws (2 per processed herd, every 64 frames) for the simulation to stay in step.
6. `find_avail_regions`, `place_start_in_region`, `is_near_ocean`, `find_region_centroid` and `MapFairness` as in §5 / §9 for the
   per-style recipes (part 1 §6, terrain-groups part).
7. A recorded random game is the only oracle; until then check plausibility: pool counts, start distances, no resource within
   `sp` of another, Fish only in deep ocean.

## 12. Evidence

| Function | Address | Used for |
|---|---|---|
| `Map::place_resources` | 68f4f0 | passes, `type` / `chance` / `group` / `pattern` parsing, pool fill, dispatch |
| `Map::place_player_resource` | 691f70 | §4 (asm read in full; the decompile lost all conditions) |
| `Map::place_region_resource` | 690480 | §6 (asm read in full) |
| `Map::scale_number` | 6a0320 | negative = `|N|·n` rule |
| `Map::find_avail_regions` / `LinkListBase::add` | 68f380 / 47afa0 | §5 (list order) |
| `ResourceDivvyPool::add_good / get_early / get_late / get_water / done_adding_goods` | 68a830 / 68a640 / 68a570 / 68a4a0 / 68a700 | §3 |
| `Objects::init_good` / `init_item` / `Good::init` / `Item::init` | 653f30 / 653e00 / 66da20 / 6770e0 | §7 |
| `Herds::init_herd` / `Herd::create_units` / `Herd::process` / `Herds::process` / `Setup::init_wild_life` | 741bb0 / 7417f0 / 741760 / 741cc0 / 5aac70 | §8 |
| `Terrain::has_south_forest` / `WorldData::has_mountain_tcoords` | 850060 / 6b3050 | accept tests |
| `Map::is_near_ocean` / `place_start_in_region` / `find_region_centroid` | 68b0a0 / 68ac00 / 68ae50 | §9 |
| `MapFairness::*` | 68a100..68a3f0 | §9; user `MapWarringStates::make_continents` @698b60 (scale 1.0 @b69514) |
| `Map::calc_bonus` / `calc_bonus2` | 692f10 / 692e70 | §9 (vtable only, no callers) |
| `Map::init_resources` | 69e990 | `Lands` record layout (+0x4c / +0x50) |
| Data | `mapstyles\*.xml`, `Data\resourcerules.xml`, `unitrules.xml` | §1 table, goods 6..36, units 407..413 |

Open ❓: (1) the `pattern` fallback for an unknown name; (2) `get_age_slow` for goods (Sulphur early/late); (3) draws inside
`Objects::init_unit` / `Unit::set_new_location` during `Herd::create_units` (none seen in `find_nearby_spot`).

## 13. `Terrain::move_goody` @84a3d0 and `Terrain::move_ocean_goody` @84aec0 (third pass, ported)

Placement (sections 4 / 6) happens inside `Map::make`; `Terrain::init` runs afterwards and changes cells under the placed objects. Whatever stands on
a wcoord that stops being plain land is moved by one of two near-twin functions (`engine/sim/mapgen_terrain_init.cpp`, `TerrainInit::move_goody`):

- **`move_ocean_goody`** from `Terrain::mark_halfwater` @84ece0 (before the cell becomes half water; `flags & 0x100 == 0`) and from the end of
  `Terrain::check_and_set_halfland_wcoord` @84dec0 (@84e4a5: a cell that was re-evaluated and joined a neighbour's region);
- **`move_goody`** from `RiverSection::create_trough_edge` (every sample's wcoord, randommap-rivers section 9) and `Cliffs::assess_wcoord` @8a6a40
  (steep cells; not ported yet, it needs `has_plus_slope` on the mountain heights).

Both read the object on the cell first: the goody box (`down == -3`, valid item; skipped when the cell is water and not half-land), the resource
(`down == -2`, valid, not oil), the derrick (`WData.flags & 0x800`); nothing there ends the call. The 48 `land` values of `move[1..48]`
are read (-1 outside the map). Then, each in this order:

1. **Goody** (item slot kept): `Item::close` (clears `0x8000` and the down list of the old cell), then the first of `move[9..24]` (ring 2, 16 cells) that is
   inside the map, not on the border (`x` or `y` equal 0 or `xs - 1`, the original compares both with `xs - 1`), `land == 0`, not half-land, no
   `0x10 / 0x28 / 0x800 / 0x1000 / 0x40 / 0x8000`, no good, `blocked == 0`, CoordInfo coastal bit (`& 4`) clear and not `has_south_forest`
   takes it (`Item::init` at the cell centre, `set_down(-3)`). No acceptable cell: the goody stays closed (lost).
2. **Resource**: `Good::close` (resource tcoords, blocking, `flags & 1`, down list, its herd), then the same ring; land variant: the goody's tests with
   `land == 0`; ocean variant: `land` 1 or 2, not half-land, none of `0x10 / 0x68 / 0x1800 / 0x8000`, no good, `blocked == 0`, no south forest
   (no coastal-bit test); `Objects::init_good` (first free slot, centre from `snap_center`) and `Herds::init_herd` (first free herd slot).
3. **Derrick**: `World::set_oil_at(x, y, 0)` and rock flag `0x8` off; the first of `move[9..48]` whose `land` equals the source's (a water derrick looks for
   `land` 2 or 1 with mask 8), not half-land, `is_clear_land(mask | 0x80)` @874b60 (no mountain / forest / rock / oil / river / cliff flag unless the mask
   allows it, `blocked == 0`, CoordInfo coastal bit clear) and not south forest gets `set_oil_at(..., 1)`; the rock flag `0x8` is set there (the ocean variant
   always, the land variant only for a derrick that was on land). The LAND variant (`Terrain::move_goody` @84a3d0, the river's and the cliffs') then runs
   `TerrainOil::add_rocky_patch` on the new cell (@84ad29; the ocean variant does not): the oil plateau `terraform_for_building` at the new place, at that
   moment of the generation (`Gen::move_goody_at` for the cliffs works on `Result::heights`).

`Objects::init_good` / `init_item` / `World::set_oil_at` reuse the first free slot of their array (also `put_good`, `put_goody`, `Gen::set_oil_at` now).

## Oracle: current 2.SVX (2026-10-24)

Second pass (2026-10-24, after the group fixes of `randommap-terrain-groups.md` and the `Terrain::init` port): goods 78 of 80 exact (type and
position; the 2 others are oil sites on one rock clump), goodies 33 of 50 exact, `WData.goods` near bits exact. Fish, relics, wine and the
helping fairness (the lagging player gets extra oil) all follow once the groups are exact: the earlier count differences were consequences of
group placement, not of this file.

- (third pass) The 17 goodies that sat one or two cells off were not an acceptance rule at placement time: the original moves them afterwards,
  section 13 (`move_ocean_goody` for the cells that become half water, `move_goody` for the river trough). With both ported goodies are 50 of 50
  exact and goods 80 of 80 (the two oil sites on the rock clump too: the river moved the derrick), on every world with a comparable goody list.
- On the other worlds the match is complete for goods and goodies where the group state is (East Indies 100: 107/107 goods, 1/1 goody).
