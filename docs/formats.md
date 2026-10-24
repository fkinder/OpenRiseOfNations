# File formats

Status key: ✅ verified against real files · 🟡 structure known, fields partly decoded · ❓ not started

## BHG chunk container (`.BH3`, `.BHa`) ✅ container / 🟡 payloads

Read by `ChunkRead` in `riseofnations.exe`. Little-endian. Every chunk:

```
u32 size   // includes this 8-byte header
u16 id
u16 aux    // child count for tree nodes (BH3 id 6, BHa id 8); otherwise version/flags
u8  payload[size - 8]
```

A chunk is a container when its payload is a whole sequence of child chunks. Tool: `re/tools/chunkdump.mjs <file> [--stats]`.
**Validated:** all 3,909 `.BH3` and `.BHa` files in the install parse with no leftover bytes.

### `.BH3` mesh ✅ (`BH3Method::load_bh3`, `load_verts`, `load_faces`, `load_texs`, `load_hier`)

```
id 0  root
  id 1  mesh group (aux=4)
    id 2  positions : u32 n; float4[n]   (x, y, z, 1.0), in the owning node's LOCAL space
    id 3  normals   : u32 n; float4[n]
    id 4  texcoords : u32 n; float2[n]   (top-left texture origin)
    id 5  faces     : u32 n; u16[n]      (triangle list; n/3 triangles)
  id 6  hierarchy node (aux = number of child nodes)
    id 7  node record:
            u32 first_vertex, u32 vertex_count   // node rigidly owns this vertex range
            u32 name_len, char name[name_len]    // NUL-terminated, e.g. "Bip01 L Thigh", "gun pivot", "ammo node 0"
            f32 quat[4] (x,y,z,w), f32 trans[3], f32 unused   // parent-to-local ("rest") transform
    id 6  child node ... (recursive, in file order; HierObj::add_child appends)
```

Skinning is **rigid**: each vertex belongs to exactly one node, and node vertex ranges cover the mesh (verified on the
machine gunner, where they sum to 346 of 346 vertices). Skeletons are 3ds Max Biped exports. Some nodes are attach
points (`ammo node N`, `pivot node N`, `smoke node N`) that `unit_graphics.xml` EXISTEVENT/PARTICLEEVENT/RELEASEEVENT entries reference
by slot index (name → slot rules in `docs/attachments-spec.md`).

### `.BHa` animation ✅ (`AnimObj::load_hier`; playback `HierObj::set_to_time`)

```
id 0  root (aux=1)
  id 8  node (aux = number of child nodes); the tree mirrors the BH3 hierarchy exactly
    id 7  key track: u32 n; n x { f32 dt_seconds, f32 quat[4] (x,y,z,w), f32 trans[3], f32 unused }  (36 bytes/key)
    id 8  child node ... (recursive)
  id 10 (3 files only) ❓
```

- Key time = running sum of `int(dt * 1000) & 0xffff` in milliseconds, and clip length = the last key's time. Playback loops.
- Sampling: binary search for the surrounding keys, **slerp** rotation and **lerp** translation. Keys flagged constant are dropped at load.
- BHa nodes bind to BH3 nodes **by tree position** (child i ↔ child i), not by name, so there are no names in BHa.
- Node world transform: `world = parent.world × rest(BH3) × anim(BHa)` (`HierObj::update_transforms`).
- `MOTMethod` (chunk ids 11/12) is a separate per-vertex morph-animation path, not used by `.BHa`.

### Rotation convention ✅

Quaternions in both files are the **conjugate** of the standard (column-vector, `q·v·q⁻¹`) convention. Negate x, y, z
on load. Verified visually: with conjugated quaternions the machine gunner's idle is a correct kneeling pose
(`re/previews/quat_convention_check.png`); with the raw values his limbs fold into his body and the A-10 points nose-up.

### Graphics catalogs ✅

- `Data/unit_graphics.xml`: `<UNIT name model= texture= scale= skeleton= ...>` containing `<ANIM name="CHAR_IDLE1" file="MachinegunGU Idle1"/>`,
  `BOUNDINGVOLUMES`, `PARTICLEEVENT`/`RELEASEEVENT` (timed effects at attach nodes), and `EXISTEVENT` (weapon/prop models attached
  to a node during time windows; see `docs/attachments-spec.md`). 1,375 unit graphics in total.
- `Data/anim_graphics.xml`: animation name → `.bha` path, in `<LOOPING>` and other sections.
- `Data/building_graphics.xml`: **buildings are pre-rendered 2D sprites** (`texture=`, `spritexoff/spriteyoff`) plus a
  low-poly `zbuffer=` .BH3 used only for depth. They are not 3D models. Animated parts (EXISTEVENT add-ons) are separate 3D
  AMMO models. Sprite alpha is a code: 0 empty, 255 opaque, 145 baked black shadow, 248 team-colour mask. Full drawing rules
  (anchor, camera scale, hull transform, team colour, construction, variants): [buildings-spec.md](buildings-spec.md).
- All unit art (models, animations and textures) is 3,237 files, about 99 MB. All textures are 32-bit TGA (type 10 RLE or type 2 raw).
  The alpha channel is mostly 255, with small patches around 64; it is probably a team-color mask (🟡 to confirm in the texture-stage setup).

## Rules and content XML ✅ (plain text)

`Data\*.xml` with `.dtd`/`.xsd` schemas. The designers documented the fields in comments at the top of each file.
Localized variants are `name.xml.<langid>`. File names are resolved through the internal string table
(`internal_strings.xml`), not hard-coded. Load order (from `Game::init_common_data`):

1. `Game::init_rules_data`: `Constants::init`, `Categories::init`, `Tribes::init`, map file names
2. `Game::init_non_rules_data`: `Scene`, `Nuke`, doober names, **`Types::init`** (units, buildings, techs, goods, items,
   objects, spells, bonuses, governments; up to 806 of each), `Particle`, `ShockWave`, `Lands`, `Forms`, `ScriptGameInterface`
3. Per game, `Game::init`: `World`, `TileSet`, `RiversOut`, `WorldMap`, `Setup::build_game`, `GraphicPieces::fill_doobers`, `Farms`, ...

## Replays `.rcx` ✅ (embedded-save body 🟡, Conquer the World header ❓)

Full spec: [replay-spec.md](replay-spec.md). Tool: `node re/tools/rcxdump.mjs <file>`.
The plain stream is written while the game runs and gzip-compressed once, when the recording is closed (`RecordGame::finalize`):

1. **Header**: `Game::walk_data` (GameInfo with seed, setup and players; the `Game[frame..balance)` block; the semaphore) + `String save_name`
2. **Embedded save** (only if `save_name` is set): the save file copied verbatim (`"RoNSave"` magic, `WalkDataGame` body)
3. **Random game info**: 5 bytes per player of the running game + 4 (informational; playback discards it). The count can exceed the header's players (scenario games), so it is probed
4. **Rules** (only for a fresh game): `Game::walk_rules_data`, i.e. all 806 types + constants + 493×493 balance table + 24 tribes. Playback loads these over the XML rules (field-level layout and the XML loader that reproduces it: [rules-spec.md](rules-spec.md))
5. **Command stream** until EOF: `i32 frame, i32 play, i32 valid, u32 stamp, i16 size, u8 data[size]`. `data` = commands
   (`u8 opcode` + packed args, 82 verbs). In multiplayer, data is XORed with `(seed>>8)&0xffff` per u16, and each
   command is followed by 0–1 random padding bytes (LCG seeded with `seed` per package)

A game that crashed leaves a plain file whose last record is cut short; the decoder reports it. All 95 real recordings in `re/oracle/` decode ([oracle-corpus.md](oracle-corpus.md)).

Sample: `re/analysis/sample_replay.bin` (decompressed). It starts with tag bytes `16 42`, then UTF-16 `"(Version: 00.2024.06.2000)"`, and decodes exactly to EOF.

## Saves 🟡

`SaveGame` / `LoadGame` → `WalkDataGame::walk_data`, the same DataWalk visitor used for replays and checksums.

## DataWalk: the universal state serializer

263 classes implement `walk_data(DataWalk*)`. One visitor implementation per purpose:
save (`SaveGame` vtable), load (`LoadGame`), checksum (`CheckSums`, about 50 `check_*` per subsystem), and sync
logging (`SyncLogger`). `walk_data` lists each class's persistent simulation fields in serialization order, and flags select
optional groups (for example, for `Unit`: path stack = flag 2, orders = flag 4, guys = flag 8).
This gives us the full simulation state model and the save/replay byte layout from one source.
Fully described in [state-spec.md](state-spec.md) (mechanics, save container, checksum parts), with every walk function
as machine-readable steps in `re/iface/datawalk.json` and readable pseudo-code in [state-walk.md](state-walk.md); decoder:
`re/tools/savdump.mjs`.

## Scripts `.bhs` ✅ (plain text)

BigHugeScript: C-like, with `trigger name() { ... }`, `enable_trigger("...")` and typed functions (`int ai city_placement(int who)`).
Compiled at runtime by `lexer` → `SyntaxTree` → `compiler` → `opcodes`, and run in `VirtualMachine`, with 843 `ScenarioFuncSet` builtins.

## Scenarios `.scx` / `.sce` ✅ terrain chunk / 🟡 header / ❓ other chunks

gzip around a BHG chunk tree (same container as `.BH3`). Root id 0; first child is the header (its id is the header
version: 0x6c in 275 of 311 stock files); then 2 terrain, 3 items, 4 goods, 5 objects, 6 orders, 7 cities, 8 triggers,
9 signposts, 10 types, 11 leaders. The terrain chunk (tileset name, per-wcoord land, per-tcoord masks, per-vertex heights,
rivers, mountains, doobers) is fully decoded and round-trips byte-exactly: see [terrain-spec.md](terrain-spec.md).
Tool: `re/tools/scxdump.mjs`.

## Not started ❓

`.tga`/`.png` usage conventions, `soundtypes.xml`, `.wmv` videos, fonts.
