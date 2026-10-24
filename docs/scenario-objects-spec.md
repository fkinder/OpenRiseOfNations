# Scenario objects spec: populating a still battlefield

Phase 1b of the port: after the terrain ([terrain-spec.md](terrain-spec.md)), place everything that stands on it. This file
covers the scenario chunks 3–11: players (nations, team colours, ages), units, buildings, cities, goods, items, plus the
editor-only data. It also gives the name chain from a record to a graphic in `unit_graphics.xml` / `building_graphics.xml` /
`effects_graphics.xml`. Status key as in [formats.md](formats.md): ✅ verified against the stock files · 🟡 read from code,
not verified by rendering · ❓ open.

Tool: `re/tools/scxdump.mjs` (decoder, survey, byte round trip, name resolution) and `re/tools/objects-preview.mjs`
(top-down plot over the terrain preview).

```
node re/tools/scxdump.mjs --objects <file.scx> [--json out.json]   players + every object with its resolved graphic
node re/tools/scxdump.mjs --objects-survey ~/Games/RiseOfNations   all 311 files: strict sizes, round trip, name resolution
node re/tools/objects-preview.mjs <file.scx> [--px 20]             re/previews/<name>_objects.png + placement checks
```

`resolveBattlefield(file, install)` in scxdump.mjs returns the renderer-ready list (world position, owner, colour slot,
facing, graphic name, hidden flag) and is the reference implementation of this spec.

---

## 1. Container recap ✅

The file is gzip → BHG chunk tree; root chunk 0; first child is the header (its id is the header version), then the top-level
chunks in this order in every stock file: `11 leaders, 2 terrain, 3 items, 4 goods, 7 cities, 5 objects, 6 orders,
8 triggers, 9 signposts, 10 types`. `ScenarioRead::import` dispatches on the id, so order does not matter to the reader, but
**leaders must be loaded before objects** (object creation reads the owner's tribe and age).

Chunk header: `u32 size (incl. 8-byte header), u16 id, u16 aux`. For containers `aux` = number of children. On all 311 files
every container's children exactly fill it and `aux` equals the child count (94,491 containers checked).

Conventions used below:

- All integers little-endian. `wchar[n]` is a fixed UTF-16 buffer; read up to the first NUL. **Bytes after the NUL and
  struct padding are uninitialised junk** (stack garbage at save time); ignore them, don't zero-check them.
- A "counted list" is a container with two children: a header child `{i32 count}` and a data child with `count`
  fixed-size records (data child is 0 bytes when count is 0).
- "Type names" are resolved through `Types::get_type_index`, a linear, case-sensitive, first-match search over every rule
  type comparing `TypeData.type_name` (§6). If a name does not resolve, the game skips the record (and pops an ignorable
  error).

## 2. Header additions ✅

The terrain spec lists the fields. Struct `ScenHeaderChunk6` (0x180, header id `0x6c`), offsets in bytes:

| Off | Field | Off | Field |
|---|---|---|---|
| 0x000 | `num_chunks` (top-level chunks after the header; always 10) | 0x0f4 | `reveal_map` |
| 0x004 | `file_id` (magic) | 0x0f8 | `elimination` |
| 0x008 | `wchar name[100]` | 0x0fc | `victory` |
| 0x0d0 | `team_style` | 0x100 | `score_goal` |
| 0x0d4 | `game_rules` | 0x104 | `time_limit` |
| 0x0d8 | `map_style` | 0x108 | `wonderwin` |
| 0x0dc | `map_size` | 0x10c | `popwin` |
| 0x0e0 | `map_seed` | 0x110 | `econwin` |
| 0x0e4 | `scenario_flags` (`ScenarioFlag`: 1 LOAD_SCRIPT, 2 ALLOW_SETUP_OVERRIDE, 4 DISABLE_UNIQUE_UNITS, 8 DISABLE_NATION_POWERS, 0x10 LOAD_XML) | 0x114 | `rush_rules` |
| 0x0e8 | `game_speed` | 0x118 | `camera_init_x[8]` (world units, −1 = unset) |
| 0x0ec | `start_tech` | 0x138 | `camera_init_y[8]` |
| 0x0f0 | `end_tech` | 0x158 | `camera_init_zoom[8]` |
|  |  | 0x178 | `ai_sea_map` |
|  |  | 0x17c | `pop_limit` |

Older headers: `0x52` (0x124 bytes, 26 files) and `0x66` (also 0x124 in the 2 stock files) store one camera
`x, y, zoom` at 0x118/0x11c/0x120 that the loader copies to all eight players; `0x67` (0x12c, 8 files) adds
`ai_sea_map` 0x124 and `pop_limit` 0x128. Missing fields default to camera −1, zoom 5.
Shipped magics: 0x12345678 (v0x52/0x66), 0x87654321 (v0x67), 0xf00dcafe (v0x6c). The three other accepted magics
(0x49248753, 0x39823792, 0x00700732) trigger object-index remaps (§4.4) but no stock file uses them.

Check: the per-player camera lands next to that player's own objects (median distance to the nearest own object
1.6 % of the map width over 92 scenarios). Camera and objects share the same world frame.

## 3. Coordinates and angles ✅

- **Positions are world units**, the same frame as the terrain heights: x along the row (`wx = x div 768`,
  `tx = x div 192`), y down the rows. Map size = `xs·768 × ys·768`. Z is not stored; the object stands on the terrain
  (`TerrainOut::find_data_z`) 🟡 (ships presumably at sea level, not checked).
- **Buildings, cities and goods store their footprint centre, already snapped** (`BuildTypeData::snap_center`):
  `c = (c div 192)·192 + (size odd ? 96 : 0)` per axis, where size is the rules `X_SIZE`/`Y_SIZE` **in TCoords (192 units)**.
  Footprint = `[cx − xs·96, cx + xs·96) × [cy − ys·96, cy + ys·96)`. All 13,371 building/city records and all goods
  satisfy this (cx mod 96 = 0). Use (cx, cy) directly as the building's X, Y in buildings-spec §3 (`L = (X − 96h, Y − 96h, Z)`).
- **Units store their exact position** (in practice cx mod 96 ∈ {24, 72}: a 48-unit placement grid in the editor).
- **Unit facing is a 32-bit binary angle** (BAM): `deg = angle · 360 / 2³²` (0x40000000 = 90°). Direction in the map plane:
  `(sin a, −cos a)` — 0 faces −y (up the rows), 90° faces +x. From `find_angle` (0 for dx = 0, dy < 0; 0x40000000 for
  dy = 0, dx > 0) and verified on the data: over 1,197 stock units with an enemy unit within 3,000 units, this
  hypothesis gives mean cos 0.46 towards the nearest enemy (67 % within 45°); the five other axis conventions score
  ≤ 0.21. The draw path uses `chad.angle = deg(angle) + 180` (`GuyOut::draw`), then the `Rz` of attachments-spec §M_unit.
  The unit matrix `T(L) · Rz(angle) · S(s, −s, s)` is a mirror (det < 0) in the same basis as the terrain, so the
  port binds each render state's own cull faces unflipped for units and projectiles (as for the terrain and the
  scenario flags); flipping them culled the faces towards the camera and drew closed bodies (trucks, wagons, siege
  engines) hollow. BH3 normals are unit length after `parse_bh3` (the shipped files carry unnormalised and, in tail
  vertices, garbage ones: denormals, zeros, NaNs) and the combiner shader guards `normalize` so that a NaN pixel cannot
  black the frame through the exposure average.
- Axis check: with x/y swapped, 27 % of land units would stand in water and only 50 % of building centres would sit on
  `land_sub > 0`; with the correct axes it is 0 % and 91 % (towns are painted with the arid/dirt land sub-type; the
  map-wide share is 35 %).

## 4. Objects chunk (id 5) ✅

Children, in any mix: **25** = one unit, **26** = one building. 30,372 units and 24,969 buildings in the 311 files.

### 4.1 Unit: chunk 25 → single child 83, `UnitPropertiesChunk4` (0x244 bytes)

| Off | Type | Field | Meaning / use |
|---|---|---|---|
| 0x000 | i32 | `who` | owner player 0–7 |
| 0x004 | i32 | `o` | object slot (0–1999 for units); kept on load (`find_free` honours it). Odd `o` → female graphic (§6.3) |
| 0x008 | i32 | `o_up` | captain unit's `o`, −1 = none. Batch units (Musketeers, Fusiliers, Phalanx, …, `UnitType+0x308` objects per training) form an up/down chain; each object is still its own record and drawn at its own position |
| 0x00c | i32 | `o_down` | next in the chain, −1 = none |
| 0x010 | u32 | `angle` | facing, BAM (§3) |
| 0x014 | u32 | `dest_angle` | turn target (equal to `angle` in 97 % of records) |
| 0x018 | i32 | `tolerance` | always 0 |
| 0x01c | u32 | `unit_masks` | file bits → `UnitData.unit_masks`: 1 → UNIT_AI_OFF (0x1000000), 2 → UNIT_DECOY (1), 4 → UNIT_UNDER_COVER (0x800), **8 → crew slot** (no unit created, only the slot reserved; position −1,−1), **0x10 → UNIT_PACKED** (0x80000; otherwise the loader calls `cast_unpack`) |
| 0x020 | i16 | `mana_burn` | |
| 0x022 | i8 | `form` | `FormIndex` (0 LINE … 9 MOB; 9 on civilians) |
| 0x023 | i8 | `stance` | index into the unit's stance table (`CombatStanceIndex` / `WorkerStanceIndex` / packer) |
| 0x024 | i8 | `move_mode` | always 0 |
| 0x025 | — | pad 1 | |
| 0x026 | wchar[256] | `type` | unit type name (§6) |
| 0x226 | — | pad 2 | |
| 0x228 | i32 | `cx` | world x |
| 0x22c | i32 | `cy` | world y |
| 0x230 | i32 | `damage` | hit points lost (`Object+0x24`) |
| 0x234 | i32 | `inside` | container's `o` (unit or building ≥ 2000), −1 = not inside |
| 0x238 | i32 | `inside_who` | container's owner |
| 0x23c | i32 | `group` | editor group index, −1 = none |
| 0x240 | i32 | `group_scan` | slot in that group |

**Drawing rule:** skip `unit_masks & 8` (crew slots: 4 Bazooka records in Korea) and `inside ≥ 0` (2,120 records;
garrisoned in forts, castles, towers, cities, airbases, or carried by transports and carriers). Everything else is a visible
unit at (cx, cy) facing `angle`.

Older layouts still accepted by the reader, absent from stock files: 72 and 79 (`UnitPropertiesChunk2/3`, `char type[256]`
at 0x25, cx/cy at 0x128) and 28 (rejected: "outdated units").

### 4.2 Building: chunk 26 → 84 + 85

84, `BuildPropertiesChunk2` (0x224 bytes):

| Off | Type | Field | Meaning / use |
|---|---|---|---|
| 0x000 | i32 | `who` | owner |
| 0x004 | i32 | `o` | object slot, 2000–2999 |
| 0x008 | i32 | `damage` | hit points lost; applied only when `is_active` |
| 0x00c | i32 | `gather_x` | rally/gather point, world units |
| 0x010 | i32 | `gather_y` | |
| 0x014 | i32 | `cx` | footprint centre, snapped (§3) |
| 0x018 | i32 | `cy` | |
| 0x01c | wchar[256] | `type` | building type name (§6) |
| 0x21c | i32 | `is_active` | 1 → `Build::activate` (finished building); 0 → construction site at progress 0 (buildings-spec §6). **All stock buildings are 1** |
| 0x220 | u8 | `ever_seen` | per-player "seen" bits (fog memory) |
| 0x221 | — | pad 3 | |

85, the production queue: counted-list-like pair **33** `BuildQueueHeaderChunk {i32 count, i32 first_in_progress}` and
**35** `count × wchar[256]` unit type names, each fed to `Build::queue_up`. Not needed for a still image (12,570 of
24,969 queues are empty in .scx).

Legacy: 29 (`BuildPropertiesChunk`, `char type[]`, `is_active` at 0x11c) and 32 (queue of `char[256]`) are accepted, unused.

### 4.3 Load-time effects that matter for drawing 🟡

`Objects::init_build(who, type, cx, cy, 0, o)` snaps again (idempotent on stock data), calls `Build::init`, and emits
a "build" particle puff (not for Farm, Mine, Woodcutter or docks). `Objects::init_unit(who, type, cx, cy, o, o_up, o_down)`
creates exactly one object in slot `o`; `Unit::set_angle(angle)` sets the guys' facing.

### 4.4 Containment and index remaps

After all objects exist, `insert_inside(unit, in_o, in_who)` runs for every `inside ≥ 0`. For files with magic
0x49248753 / 0x39823792 / 0x00700732 the container index (and, for 0x00700732, building `o` and order targets) is shifted
(+100 above 299, +50 above 399, +0x60e above 0x1c1) to the current numbering. Not exercised by stock files.

## 5. Cities (7), goods (4), items (3), signposts (9) ✅

**Cities**: chunk 7 → n × 27, each = 30 + 85 (queue as §4.2). 30 is `CityPropertiesChunk` (0x2f0):

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0x000 | i32 | `who`, `o` (0x004), `damage` (0x008), `gather_x/y` (0x00c/0x010), `cx/cy` (0x014/0x018) | as buildings |
| 0x01c | i32 | `city_flags` | bit 1 → `CityData.city_flags |= 0x10` (capital), bit 2 (only with bit 1) → `|= 0x4000` ❓ |
| 0x020 | i32 | `founder` | original founding player (`CityData.founder`) |
| 0x024 | wchar[100] | `name` | city name, localised through the scenario's string XML |
| 0x0ec | wchar[256] | `type` | building type: "Small City", "Large City", "Major City" (→ graphics VILLAGE, CITY, METROPOLIS) |
| 0x2ec | i32 | `is_active` | as buildings |

A city is a building (`init_build`, `o` in 2000–2999): draw it exactly like a building (all three city types are 7×7).
`is_active` also increments a leader counter (`Leader+0x820`) and sets the city's name, id and flags.

**Goods** (resources on the map: Fish, Wine, Metal, Oil, Whales, …): counted list **23** `{count}` + **24**
`count × GoodsDataChunk {wchar type[256], i32 cx, i32 cy}` (0x208). → `Objects::init_good(type, cx, cy)`. Positions follow
the building snap with resourcerules `X_SIZE/Y_SIZE` (1×1 goods at tile centres). All 2,778 Fish/Whales/Salmon/Seals
goods in .scx files sit on water.

**Items** (ruins, "Goody Box"): counted list **21** + **22** of the same 0x208 record → `Objects::init_item`.

**Signposts** (map labels): counted list **54** + **55**, `SignpostsDataChunk {wchar name[256], cx, cy}`. Empty in every
stock file.

## 6. Name resolution: record → rules type → graphic ✅

### 6.1 Rules lookup

`Types::get_type_index(name)` compares against `TypeData.type_name` (+0xb0). For units and buildings that is the
**`<TYPENAME>`** element of `unitrules.xml` / `buildingrules.xml`, not `<NAME>`: e.g. unit "Boudicea" is the record whose
`<NAME>` is "Boadicea", and the Korean citizen is `TYPENAME` "Citizen Korean" (`NAME` "Citizen"). Techs, resources (goods)
and items have no `TYPENAME`; their `<NAME>` is the key. First match wins.

Over all 311 files every unit, building, city and good name resolves, except 7 conquest unit records ("Pioneer",
"Pathfinder") whose types no longer exist in EE: the game drops them. Leader "known types" lists (§7) also contain
"Ranger", "Pioneer", "Pathfinder" and "Boadicea" (a `NAME`, not a `TYPENAME`); `gain_tech` stops the list at the first
miss, which is harmless for drawing.

### 6.2 Buildings

Rules `<GRAPH>` (e.g. "Small City" → `VILLAGE`, "House B" → `HOUSE6`) then buildings-spec §6:
`<GRAPH>-<STYLE>-AGE<n>`, style order = tribe BUILD style at age a…0, BACKUP style a…0, DEFAULT a…0. `FARM` is the special
`<FARM>` element, which has no sprite: farms are wheat boxes and a ground underlay ([farms-spec.md](farms-spec.md)). Style names by id: 0 DEFAULT, 1 NEUROPE, 2 ASIAN, 3 KOREAN, 4 AMERICAN, 5 ARAB, 6 IROQUOIS, 7 COLONIAL,
8 EINDIAN. Graphic names compare case-insensitively ("ArmedCaravan" → `ARMEDCARAVAN-…`).

### 6.3 Units (`GraphicPieces::get_unit_gpiece`)

`<GRAPH>-<USTYLE>-AGE{0|3|5}[-FEMALE][-PACKED][-CREWn]`:

- Age bucket from the owner's age a (0–7): `a < 5 ? a div 3 : 2` → AGE0 (Ancient–Medieval), AGE3 (Gunpowder,
  Enlightenment), AGE5 (Industrial+). Fallback: lower buckets, then the DEFAULT style.
- Unit style = the leading number of the tribe's `<UNIT_CONTINENT>`: 0 DEFAULT (European), 1 ARAB, 2 AMERICAN, 3 ASIAN,
  4 NA (Iroquois/Lakota), 5 INDIA (EIndian). (The number is what counts: `get_text_num`; one Alexander file says "4 Arab".)
  The comment table in rules.xml ("0 = American …") is stale. 🟡 The id→suffix order is inferred from the data (each
  tribe's number and text agree, and every resolved name exists), not traced in `init_unit_data`.
- Female: `Guy::update_gpiece` passes the unit's `o`; odd `o` selects `-FEMALE` for types that have it (citizens, scholars),
  unless the unit type has flag 4 at `UnitType+0x2b8`.
- Packed: `unit_masks & 0x10` (HMG, cannons, howitzers, fishermen …) → `-PACKED` variant.
- Crew: crewed units (cannons, caravans) draw extra `-CREWn` guys; crew count is `UnitType+0x304/+0x30c` (❓ not decoded).
- Merchants: a special branch for three type indices (0x3d, 0x3e, 400; 🟡 taken to be the merchant family) picks one of six
  extra pieces by the **build** style — `MERCHANT-KOREAN-AGE3`, `-NEUROPE-AGE3`, `-IROQUOIS-AGE0/AGE3`, `-COLONIAL-AGE3`,
  `-EINDIAN-AGE3` (build styles 3, 1, 6, 7, 8; AGE3 bucket, Iroquois also AGE0) — and a build style 5 (ARAB) forces unit
  style ARAB. Otherwise the normal rule applies. (The Korean citizen is its own type, "Citizen Korean" → `CITIZENSKOREAN`.)

All 15,006 .scx unit records and 13,371 building/city records resolve to an existing graphic with these rules.

### 6.4 Goods and items

Goods: `effects_graphics.xml <GOOD name="<type>" model=… texture=… offset underlay>` (51 entries, all goods resolve).
Items: one `<ITEM texture=".\art\stonehenge.tga" offset underlay>` (no name) 🟡.

## 7. Players: leaders chunk (11) ✅

Chunk 11 → one **36** per player (only active players are present; `who` need not be contiguous, e.g. 0, 1, 6). Children:

**107** `LeaderPropertiesChunk3` (0xec; 819 records) or **87** `LeaderPropertiesChunk2` (0xe8, no `difficulty`; 64 records):

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0x000 | i32 | `who` | player slot 0–7 |
| 0x004 | i32 | `tribe` | nation index (below) → `Leader+0xc` |
| 0x008 | u32 | `leader_flags` | file bits → Leader flags: 8/0x80/0x100 → CAN_TRANSPORT_CIV/MIL/SCT, 0x10 → IGNORE_POPCAP, 0x20 → ASSIMILATE_IMMED, 0x400 → COOP_SOLO; 1, 2, 4, 0x40, 0x200 → second flag word |
| 0x00c | i32 | `gov` | government type (−1 none) → `Leader+0x14` |
| 0x010 | i32 | `city_name_index` | next name in the tribe's `<CITIES>` list |
| 0x014 | wchar[100] | `name` | leader name (localised; conquest scripts may override) |
| 0x0dc | i32 | `is_human` | 0 = AI, non-zero (4) = human |
| 0x0e0 | i32 | `handicap` | → `Leader+0x4c` |
| 0x0e4 | u8 | **`team_color`** | **colour slot** → `Leader::init(who, tribe, team_color)` → `Leader+0x6928` |
| 0x0e8 | i32 | `difficulty` | (chunk 107 only) → `Leader+0x50` |

Then four counted lists: **38** {39 count, 86 `count × wchar[256]`} = techs known (fed to `gain_tech`; includes the age
techs); **76** {77, 78 `wchar[256]`} = unit/building types made available; **44** {45, 46 `count × {i32 resource, i32 amount}`}
= stockpile (resource 0 Food, 1 Timber, 2 Wealth, 3 Knowledge, 4 Metal, 5 Oil; the file value is plain, the game stores it
`^ 0x8221`); **41** {42, 43 `count × {i32 who, i32 diplo, i32 treaty}`} → `Leader+0x74[who]` (`diplos`), `+0x94[who]`
(`treaties`), written unchecked over the defaults of `Leader::init`. `diplos`: 0 war, 1 peace, 2 allied
(`LeaderData::is_enemy` @6ebaa0: not self and either side 0; `is_ally` @6edb50: self, or both sides 2). Stock files list
the leader itself with 0 (both predicates short-circuit on self). `treaties` bits (values seen 0/1/4/5): bit 0 is set for
team members and by `reveal_map == 3`; the rest ❓. The arrays are adjacent `int[8]`s, so `diplos[8]`/`diplos[9]` (slot 8,
gaia) read `treaties[0]`/`[1]`. Defaults (`Leader::init` @6e3930): `Leaders::init` inits every slot with tribe −1 (allied
with itself, war with all, gaia included); a scenario leader is re-inited with its tribe: team members (`is_team`: self or
the same lobby team 0..3) 2, others 0 when `Game::teams_locked` (team_style not 0/8/11) else 1, with rush rules forcing 1
below the rush age; in the editor (semaphore byte1 & 2) others are 0. Port: `engine/sim/game.cpp` (`leader_init_diplomacy`). Legacy 37 (`LeaderPropertiesChunk`, colour slot = who) and 40 (tech names as `char[256]`
inside 38) are accepted, unused.

**Colour slot.** `team_color` indexes the `TEAM_COLOR_*` tables (buildings-spec §4, slots 0–8). In stock files it equals
`who` except in 6 records (0→4, 1→6, 2→4).

**Nation → style chain.**

1. `tribe` indexes the 24-entry `<TRIBES>` list of `Data/rules.xml` (`Tribes::init`, 0x5f0-byte records):
   0 aztecs, 1 maya, 2 inca, 3 bantu, 4 nubians, 5 greeks, 6 romans, 7 egyptians, 8 turks, 9 spanish, 10 french,
   11 british, 12 germans, 13 russians, 14 chinese, 15 japanese, 16 koreans, 17 mongols, 18 iroquois, 19 lakota,
   20 americans, 21 indians (EINDIANS.XML), 22 dutch, 23 persians.
2. **Conquest campaigns replace the file list** with the `<TRIBES>` block of `conquest/CTW_<Campaign>_Map_01.xml` (same
   24 keys, other files: e.g. Alexander maps greeks → `conquest/alexander/MACEDONIANS.XML`, turks → PHOENICIANS; Cold War
   maps russians → Soviets, japanese → North Koreans). `Tribes::init(override)` takes the override file for each entry.
   🟡 The tool picks the override by folder (`conquest/Alexander`, `ColdWar`, `Napoleon`, `NewWorld`); the game applies it
   when the campaign starts, and loading a conquest .scx stand-alone would use the base list. Note `<TRIBE>` elements in
   these files carry attributes.
3. The tribe file gives `<BUILD_CONTINENT>`, `<BACKUP_BUILD_CONTINENT>` (building style ids, §6.2) and `<UNIT_CONTINENT>`
   (unit style id, §6.3); only the leading integer is read. Paths are case-insensitive.

**Age.** `Leader::gain_tech` adds 1 to the leader age for every age tech gained (Classical Age … Information Age; techrules
`<AGE>` 0–6), so player age = number of age techs in list 38: 0 Ancient … 7 Information. On all 883 stock leaders this equals
1 + the highest age tech. The same call raises each finished building's `max_age` byte (`BuildData+0x83`) to the leader age,
which is what the building graphic uses (buildings-spec §6). Stock distribution of ages: 0:27, 1:111, 2:17, 3:62, 4:89,
5:9, 6:122, 7:6.

## 8. Editor-only chunks ✅

- **6 orders** (editor groups): n × **47** = **48** `GroupPropertiesChunk {who, num_units, i32 units[128]}` (only the first
  `num_units` entries are meaningful) + **49** counted list {50, 51 `count × OrderDataChunk {on_what (target o), on_who, cx, cy}`}
  → `EditorGroups::make_editor_group` + `EditorOrder` list. 2,293 groups, nearly all with one order.
- **8 triggers**: always empty (scenario logic lives in `.bhs` scripts).
- **10 types** (rule overrides): n × **56**, each holding **58** (unit type backup, 804 bytes) or **59** (building type
  backup, 816 bytes); the type name is a `wchar` string at +0x0c, the rest is an `ObjectTypeBak`/build-type snapshot restored
  over the rules (`ObjectType::restore`, vtable +0x150 for buildings). 13 records in 7 files (e.g. the hero "Darius").
  Ignore for drawing; needed for gameplay fidelity.

## 9. Recipe: still battlefield

1. Parse header, terrain (terrain-spec), then leaders: per player keep `who → {tribe, colour slot, age}`; resolve the tribe
   file (base or conquest list) → build style, backup style, unit style.
2. Buildings (5/26) and cities (7/27): position (cx, cy, terrain z), graphic `<GRAPH>-<STYLE>-AGE<n>` per §6.2 with the
   owner's age, team colour from the slot, `is_active` 0 → construction site. Draw per buildings-spec.
3. Units (5/25): skip crew slots and contained units; graphic per §6.3 (`o` parity → female, packed bit); facing
   `deg = angle·360/2³²`, chad angle `deg + 180`; team colour from the owner's slot.
4. Goods (4) and items (3) at their centres with the effects_graphics models.
5. Painter order is world `x + y` (buildings-spec §1).

## Verification

- `--objects-survey` over all 311 .scx/.sce: every record has the exact PDB size, every container is fully consumed, no
  unexpected child ids; 30,372 units, 24,969 buildings, 1,573 cities, 12,274 goods, 2,536 items, 883 leaders, 2,293 groups,
  0 signposts, 13 type overrides. Re-encoding every record from the decoded fields reproduces all 6,945,352 defined bytes
  (fields plus strings up to their NUL) with 0 differences; the remaining 86.9 M bytes are string tails and padding (junk).
- Placement over the 156 .scx files: 0 of 11,956 visible land units on water, 0 of 893 ships on land, 2,778 of 2,778
  fish-type goods on water; building footprint cells on water are docks, shipyards and anchorages (all 231 mostly-wet
  footprints).
- Graphic resolution: 100 % of .scx units, buildings, cities and goods resolve to a graphic name.
- Visual: `re/previews/tut1_objects.png`, `granicus_objects.png`, `tyre2_objects.png`, `ceraiding1_objects.png`
  (`node re/tools/objects-preview.mjs`): towns sit on their dirt patches, docks on the shore, fleets at sea, armies
  face each other at Granicus.

## Evidence

All addresses in `riseofnations.exe` (base 0x00400000). Struct layouts are the PDB types (`re/types/scenario_objects.txt`,
`re/types/scenario_enums.txt`). Decompiled output (local only): `re/decomp/scenario/`, `re/decomp/scenario_objects/`.

| Topic | Functions |
|---|---|
| Dispatch, header, load order | `ScenarioRead::import` @009b1430 (header versions and defaults, `num_chunks` loop, case 3–11) |
| Units | `load_objects_chunk` @009ab9c0 (25 → unit, 26 → build; inside list and magic remaps; transport flags), `load_unit_chunk` @009aadc0 (72/79/83 layouts, mask bit mapping, crew slot path, `cast_unpack`, groups), `Objects::init_unit` @0065e0c0, `Unit::set_angle` @00605400 |
| Buildings | `load_build_chunk` @009aa6e0 (29/32/84/85), `Objects::init_build` @0065d190, `BuildTypeData::snap_center` @00636190, vtable `Build::init` +0x1a0 @00629740, `Build::activate` +0x1a8 @00623e20, `Build::queue_up` @00620f40 |
| Cities | `load_cities_chunk` @009aa5d0, `load_city_chunk` @009a9e50 (30 layout, flags 0x10/0x4000, name localisation) |
| Goods, items, signposts | `load_goods_chunk` @009ad360, `load_items_chunk` @009ad510, `load_signposts_chunk` @009a9900, `Objects::init_good` @00653f30, `init_item` @00653e00 |
| Leaders | `load_leaders_chunk` @009acde0, `load_leader_chunk` @009ac080 (37/87/107, 38/76 tech lists, 41 diplomacy, 44 resources `^ 0x8221`, flag mapping), `Leader::init` @006e3930 (+0xc tribe, +0x6928 colour slot), `Leader::gain_tech` @006dcb60 (age +1, building `max_age`), `ScenarioWrite::save_leaders_chunk` @009a77c0 |
| Types, orders | `load_types_chunk` @009a94d0 (56/58/59), `load_orders_chunk` @009a9b40 (47/48/49), `load_triggers_chunk` @009a9b30 |
| Name lookup | `Types::get_type_index` @00668f30 (`TypeData.type_name` +0xb0, first match) |
| Tribes | `Tribes::init` @006ef430 (24 entries × 0x5f0 from rules.xml `<TRIBES>`, optional override list, `get_text_num` of the continent elements) |
| Unit graphics | `GraphicPieces::get_unit_gpiece` @0090c030 (age bucket, style, gender, crew, fallbacks), `Guy::update_gpiece` @005d8530 (passes `o`), `ScenarioRead::load_gpieces` @009a9390 (preload per leader) |
| Angles | `find_angle` @0092d130 (BAM convention), `GuyOut::draw` @005dc490 (`chad.angle = deg + 180`), `GuyData::get_render_angle` @005de570 |
| Writers (field order cross-check) | `ScenarioWrite::save_objects_chunk` @009a6680, `save_cities_chunk` @009a5f90, `save_goods_chunk` @009a7380, `save_items_chunk` @009a75c0, `save_orders_chunk` @009a5b60, `save_types_chunk` @009a56b0 |

## Open questions

1. Object z: assumed terrain height at (cx, cy) for land objects; check where ships and docks sit vertically
   (`Unit::init` / `Build::init` z source not traced).
2. Unit guy layout: how many figures a unit draws (`UnitType+0x304/+0x30c`, `GUY_SPACING`, `CREW_SIZE`) and the crew offsets.
3. Unit style id → suffix table (§6.3) is from the data; confirm in `GraphicPieces::init_unit_data`. Same for the special
   citizen/scholar branch keyed on the build style.
4. Conquest tribe override: confirm the game uses the campaign list when a conquest scenario is loaded outside the
   campaign (the tool assumes the override by folder).
5. Exact age passed to graphics: `scale_gpiece` / `get_unit_gpiece` add a per-call offset (`param_7`) to the leader age;
   the still-image assumption is offset 0.
6. Meanings of city flag bit 2 (`0x4000`), the `treaty` bits other than bit 0, `unit_masks` bit 1 (AI_OFF) in practice,
   and the queue header's second word.
7. Leader names in some Cold War files do not match their nation (korea3: "President Eisenhower" is tribe 13, Soviets under
   the campaign list); presumably scripts rename leaders at start.
