# Replays (`.rcx`), the command stream, turn timing, checksums and RNG

Status: ✅ container, header, rules block, package records, every verb's byte layout (lengths cross-checked against all 82
`CommandPackage::process_*` return values), **validated on 95 real recordings** (`re/oracle/`, see [oracle-corpus.md](oracle-corpus.md)) · 🟡 embedded-save case (the save body
is located by scan, then confirmed byte-for-byte against the user's own `.SVX` files, but not decoded here) · ❓ Conquer
the World header.

Tool: `node re/tools/rcxdump.mjs <file.rcx|decompressed.bin> [--summary] [--hex] [--json out.json] [--checksums out.csv]`, or
`--survey <dir> [--checksums out.csv] [--tsv out.tsv]`.

**Replays available:** besides the corpus below, the original sample is `re/analysis/sample_replay.bin` (already decompressed; a 1-player-vs-7-AI
random map, 6 sim frames long). It decodes exactly: header → random-game-info → rules (1,024,221 bytes, 806 types) →
6 packages (LEADER_OPTIONS, CAMERA ×5, PLAYER_SPEED), ending exactly at EOF. The install ships no replay or save.
**Real corpus (added 2026-10):** `re/oracle/Recorded Games/` (36 single-player) and `.../Multi/` (59 multiplayer) `.rcx`, and
`re/oracle/Saves/*.SVX`, copied from the user's Windows profile (gitignored, personal data). All 95 replays decode
(`--survey`); the multiplayer packages (XOR, padding, CHECK_SUMS, TURN_DATA) are now confirmed on real data: every
CHECK_SUMS row has `all` equal to the 32-bit sum of the other 15 values (172,156 rows), and the two players of a turn agree.

---

## 1. File layout

A `.rcx` is the plain byte stream below, **gzip-compressed once, when the game closes the recording**:
`RecordGame::finalize` maps the plain file, writes it through `File::open(…, 3)` (write + gz) to `recordgame.tmp` and renames
it over the original. During the game the file is written uncompressed (`File::open(name, 2)` = plain `"wb"`), so a replay
from a crashed game is left uncompressed. Readers open with mode 1 (gz read), which accepts both.

```
Header        Game::walk_data + String save_name                         (RecordGame::write_header, from init_record)
[CTW header]  ConquestGame::walk_data          only if semaphore byte2 & 0x02 (Conquer the World)   ❓ not decoded
[Save]        verbatim copy of the save file    only if save_name != ""  (RecordGame::write_save_game)
RandomInfo    5 bytes per active player + 4     only if GameInfo.version > 03.02.03.2905 (always for EE)
[Rules]       Game::walk_rules_data            only if save_name == ""   (RecordGame::write_rules)
Packages      { record } ... until EOF          (RecordGame::write_package)
```

Order of the writes in `Game::run` @584590: `init_record` (header) is called before `Game::init` for a new game, or after
the save is loaded when the game starts from a save; after `CommandManager::start` come `write_save_game`,
`write_random_game_info` and `write_rules`, then the frame loop appends packages.

Playback (`Game::run_playback` @586ea0) mirrors this: `read_header`, then `Game::run` with the playback bit set
(`semaphore byte0 |= 0x10`). `LoadGame::load_game` reads the embedded save straight from the replay stream,
`read_random_game_info` reads the 5n+4 bytes **and discards them**, and `read_rules` loads the recorded rules over the
live ones (when `save_name == ""` or the patch version is < 4). **A replay carries its own rules data**, so a reimplementation should
take rules from the replay, not from the install's XML, when it plays back.

## 2. Serialization primitives (DataWalk, `SaveGame` / `LoadGame`)

`DataWalk` vtable: slot 0 `walk_function(start, end)`, slot 1 `walk_test(label)`. Fields `input` (load), `checksum`
(checksum walker), `flags`.

| Primitive | Bytes on disk |
|---|---|
| `walk_function(a, b)` | the raw memory `[a, b)`; every field is little-endian x86 layout (struct offsets from `rise.pdb`) |
| `walk_test(label)` (section tag) | **1 byte**: the low byte of the label String's hash. On load a mismatch reports "Error loading section". The checksum walker's `walk_test` is a folded no-op |
| `String::walk_data` | `u32 n` (UTF-16 units), then `n × u16`. No terminator |
| `SimpleArray<u16>::walk_data` | `u32 n`; if `n ≠ 0`: `u32 alloc`, `u16`, `u8 flags`, `n × u16` |
| `BitMask<N>` (Game.semaphore) | `{i32 bits, i32 size}` then `size` bytes |

Tag bytes in this build: `Game` 0x16, `GameInfo` 0x42, `Player` 0x50, `Game Rules` 0x92, `Tribe rules` 0x8f.
`rcxdump` checks them.

## 3. Header (`Game::walk_data` @589600, `GameInfo::walk_data` @5d6570, `Player::walk_data` @6ee2d0)

| Field | Encoding | Notes |
|---|---|---|
| tag `Game` | u8 | 0x16 |
| tag `GameInfo` | u8 | 0x42 |
| version string | String | `"(Version: 00.2024.06.2000)"` (prefix + `Version::get_string` + suffix) |
| `GameInfo.version` | u32 | packed `Version` (compared byte-wise by `Game::get_patch_version`) |
| `seed` | u32 | **the master seed**: `game_random`, the package XOR key, and the padding RNG all derive from it |
| `checksum_deep`, `checksum_window_size`, `checksum_failure_threshold` | 3 × i32 | sample: 0, 256, 0 |
| `GameInfo.flags` | u32 | |
| setup | 30 × u8 | `team_style map_style map_size players max_observers game_speed game_rules difficulty starting_town starting_resources starting_resources2 tech_cost reveal_map pop_limit rush_rules cannon_times starting_technology starting_technology2 ending_technology elimination victory wonderwin score_goal popwin time_limit chairs econwin scenario_type script_type mods` |
| 8 × Player | tag 0x50, `u16 flags`; if `flags & 1`: 0x39 raw bytes + String name | raw = `synced_*[12]` (i32), `flags`, `tribe`@0x32, `who`@0x33, `team`@0x34 (s8), `handicap`, `play`, `pauses`, `diff`@0x38 |
| scenario workshop id | u64 | `sGameSaveVersion ≥ 0x10` layout (playback forces 0x10) |
| `scenario_script`, `scenario_path`, scenario dir | 3 × String | |
| mod workshop id | u64 | |
| mod install path | String | non-empty → playback loads or subscribes the mod |
| `Game[0x550, 0x6e4)` | 0x194 raw bytes | `frame, frame_to_break, playing, loading, tick, market_tick, market[6], market_flux[6], next_flux[6], delta_flux[6], flux_length[6], on_team[8], starting[6], wonders[17], start_list[8], start_index[8], num_players … armageddon` |
| `Game.semaphore` | `i32 bits, i32 size` + `size` bytes | byte0: 0x04 **multiplayer**, 0x08 recording, 0x10 playback; byte2: 0x02 Conquer the World |
| `graphic_tick` | i32 | presentation only |
| `save_name` | String | `""` for a fresh game |

The header is written before `TurnControl`/`CommandManager` start, so it holds the initial `frame` (0 for a new game, the
save's frame otherwise).

## 4. Embedded save (🟡)

When `save_name ≠ ""`, the save file's bytes are copied verbatim (`write_save_game` reads the save byte-by-byte with
`File::open(save, 1)`, so a gzip save is copied **decompressed**). The copy starts with `String` magic `"RoNSave"`,
`"RoNMultiSave"` or `"RoNCTWSave"` and a `u32 sGameSaveVersion`, then `WalkDataGame::walk_data` (the full DataWalk of the game,
263 classes). There is no length prefix: the original finds the end by parsing the whole save. `rcxdump` stops at the
magic and version, then **locates the command stream by scanning**: it takes the earliest offset whose records chain exactly to
EOF and decode cleanly, whose first frame is at least the **embedded save's own frame**, and which is preceded by a random-info block
(§5). A full save decoder is a separate task (it is the save-game format).

**Corrections found on the real corpus (2026-10):**
- *Lower bound of the first record's frame.* The header's `Game.frame` is **not always** the embedded save's frame. Example:
  a replay whose `save_name` points at one of the user's saves had header frame 353231 but its first record is at frame
  284306 and the embedded save (read with `embeddedSaveFrame`: GameInfo, 182 raw bytes, `Game` tag, GameInfo, frame) is at 284305; the file had been
  overwritten with a later game between recording and the header snapshot. The scan therefore bounds the first record by the frame read from the
  save itself, not by the header.
- *Save bytes are the SVX bytes.* In all 7 replays that embed a save whose `.SVX` is in the corpus (6 saves), the located save size
  equals the decompressed `.SVX` size exactly, and the frame matches (e.g. 4835387 bytes / frame 180904). This confirms
  `write_save_game` copies the decompressed file verbatim and that the scan finds the true end.
- *A save-based multiplayer game is chained.* The `Multi/2026.08.30 *` files are 45 consecutive segments of one game that was saved and
  reloaded again and again: segment *k+1* embeds a save whose frame equals segment *k*'s last record frame (7, 18344, 21117, 21514, ...).
  The first segment (frame 0..7) has no save; the next embeds an MP save made at frame 7.
- *Zero runs.* A 18-byte all-zero stretch inside a save parses as a valid record (`frame 0, play 0, size 0`), which made the scan quadratic. No real
  record has `size 0` (minimum observed: 2 in SP, 67 in MP), so the scan and the strict chain reject `size < 1`.

## 5. Random game info (`write_random_game_info` @9534e0)

For each player of the **running game**, in slot order: `i8 team` (Player+0x34) and
`i32 leaders[who]+0x0c`. Then `i32 World+0x30`. **The entry count is not always the number of header players with `flags & 1`:** the header is
written from `GameInfo` before players are added in some setups. Evidence: the two `.scx` scenario replays (the only two with a scenario path in the corpus) have 2 header players (flags & 1) but 8 and 5 entries; the 8-entry block of
the `italy_mp` scenario is `00 06000000 01 06000000 ... 07 06000000` (team = slot index, leader value 6 for all), then `0e000000` (World+0x30 = 14), then the `0x92` rules tag. The decoder now finds the count by probing 0..8 entries until the byte after `5n + 4` is the `0x92`
rules tag (or, after an embedded save, by team/leader plausibility during the scan). In the sample, the leader values are the tribes the random choice resolved
to (17, 8, 19, …), and World+0x30 = 19 = `map_style`. Playback reads and discards the block (`read_random_game_info`), so it is
informational only.

## 6. Rules block (`Game::walk_rules_data` @589550)

```
tag 0x92
806 types in TypeIndex order, each class's walk_rules_data (vtable +0xc4):
  Type part (all):       0x5a raw (Type+0x04..0x5e) + String name (Type+0x74)
  ObjectType part:       + 0x98 raw (+0x1e4..0x27c) + SimpleArray<u16> (+0x27c) + SimpleArray<u16> (+0x298)
    GoodType   0..49     + 0x44  raw (+0x2b4..0x2f8)
    UnitType   50..413   + 0x18 (+0x2b4) + 8 (+0x2d4) + 4 (+0x2dc) + 0x2f4 (+0x2e0..0x5d4)      (352 units + 12 gaia)
    BuildType  414..542  + 0x31  raw (+0x2b4..0x2e5)
    ItemType   543       (ObjectType part only)
  TechType     544..628  Type part + 0x1b raw (+0x1c8..0x1e3) + 8 × String (+0x1e4)
  SpellType    629..683  Type part + 0x30 raw (+0x1c8..0x1f8)
  BonusType    684..805  Type part only
Constants[0x000, 0xd40) + Constants[0x804, 0x808)       (0xd40 + 4 bytes)
Balance table: 493 × 493 × u16                           (Balance::walk_rules_data, 486,098 bytes)
24 × Tribe: tag 0x8f, Tribe[0x54, 0x6c), Tribe[0x70, 0x5f0)
```

The type ranges come from the `TypeIndex` enum (`BASE_*`/`END_*`; units 50..413 include the 12 gaia types, and
`Types::init` asserts 364). Validated on the sample: the walk ends exactly where the first package record starts.

## 7. Package records (`RecordGame::write_package` @952fb0, `read_package` @952d90)

```
i32  frame      Game.frame when the package was processed (written, then CommandPackage::process_all)
i32  play       player slot that sent it (CommandPackage.play)
i32  valid      CommandPackage.valid; not used on playback
u32  stamp      CommandPackage.stamp, per-sender sequence number (local_package_stamp)
i16  size       payload bytes, ≤ 512
u8   data[size]
```

No alignment and no per-record checksum. **A game that crashed leaves a plain file whose last record is cut short** (8 of the 95 corpus files, all
uncompressed): the final record has its 18-byte header, or part of the payload, missing (examples: 14 of 18 header bytes, 75 of 159). `rcxdump` reports
it as `truncated {at, have, need}` and decodes everything before it. Records are in processing order: frames never decrease, and within a multiplayer
turn the order is player slot 0..7.

**Writers:**
- Single player (`CommandManager::process_turn` @93ef10, solo branch): when the local package is non-empty, the game
  increments the stamp, sets `play = console.play`, writes the record, then processes it. Empty turns are not recorded.
- Multiplayer: at each turn boundary, every connected player's package for that turn (stamp = local stamp − 1, from that player's
  `PackageFifo`) is written **as received, still obfuscated** (§8), then processed. A package that fails the stamp check
  is still written, then the game stops.
- `DropControl::process_drops` @9598b0 builds a one-command `UNGRACEFUL_PLAYER_DROP` package locally (not obfuscated, size 3
  or 3 + 1 padding byte) and writes it directly.

**Reader (playback):** each `process_turn` call does `read_package(frame = game.frame)` in a loop. The loop reads every record
whose `frame ≤ game.frame` (it seeks back over the first later one), runs `CheckSums::check_all` (the result goes to SyncLogger only),
then runs `process_all`. **Equivalent rule for a reimplementation:** before simulation step *F* (the `Game::do_frame`
that turns `frame` from F into F+1), execute every record whose `frame == F`, in file order.

## 8. Package payload

`data` is a sequence of commands. Each command is `u8 opcode` + fixed-layout arguments (packed, no alignment) (§9).

**Multiplayer obfuscation** (semaphore byte0 & 0x04; `CommandPackage::send` @94c1e0 applies it, `process_all` @94c500 undoes it):
1. **XOR:** each little-endian u16 of the payload is XORed with `(seed >> 8) & 0xffff`. An odd trailing byte is left
   as is. `process_all` skips the XOR when replaying a multiplayer game if the package is a drop package (`data[0] == 'P'`, `size ≤ 4`),
   because those are recorded in the clear.
2. **Random padding:** after every command, `add_command`/`add_group`/`add_spline`/`add_chat` add
   `Random(seed).get(0, 2)` ∈ {0, 1} garbage bytes to the size. The Random is reset to `GameInfo.seed` for every
   package (`CommandPackage::clear`/`init`, and a fresh local in `process_all`). The reader skips `get(0,2)` bytes after each
   command. The garbage bytes are uninitialised buffer contents.

Single-player packages use neither the XOR nor the padding.

**GROUP (opcode 0) and the current group.** Every unit order is issued as `GROUP` followed by the verb
(`CommandManager::issue_*` calls `add_group` then `add_command`; if `add_group` fails, neither is added).

```
u8 0x00, u8 num, i8 who, num × u16 object index   (objects of player `who`, index into Objects' per-player array)
num == 0  → "same group as last time": the sender emits it when (who, object list, object uids) equal its last sent group
```

`process_group` @94a0c0 builds a `Group` from the listed objects that are alive (flag bit 0). Each listed unit's `+0xb6` is set to
the sender's `play`, and the same is done for its linked chain at `+0x90`. The group is pushed with `Groups::push_group`, and the
index goes into `CommandPackage.group`. Following verbs in the package act on `groups[package.group]` (−1 means no group).
The receiver keeps per-sender `last_objects_received[play][128]` and `last_uids_received[play][128]` (u16), which hold only
the objects that were alive (`last_num_received` ends up as that count). A repeat rebuilds the group from that list, keeping each object only if its uid (`Object+0x30`)
still matches. These arrays and the sender's `last_*_sent` are reset by `CommandPackage::init_class` at game start. A
`who` other than the sender's own leader is ignored, teammates included (`LeaderData::is_team` is called but its
result discarded), and `package.group` keeps its previous value (orders-spec.md §1.2).

## 9. Verb table

Lengths include the opcode byte and equal the `process_*` return values. `ox` is an object index, and `whom`/`who`/`whose` are
leader (`Player.who`) indices. `play` is a player slot. Coordinates (`x`, `y`, `to_x`, …) are world units, 768 per tile
(`docs/terrain-spec.md`). `queued` is `QueuePos` (0 FIRST, 1 LAST, 2 NEW): i32 unless marked `i8`. `orders` is `OrderIndex`.
**G** = the issuer sends a GROUP first and the verb acts on the current group.

| Op | Verb | Len | Arguments (after the opcode) | G | Effect (`CommandPackage::process_*` →) |
|---:|---|---:|---|:-:|---|
| 0 | GROUP | 3+2n | u8 num, i8 who, u16 objects[num] | | Group::add, Groups::push_group (sets package.group) |
| 1 | BEGIN | 1 | — | | groups[group] vfunc +0x14 (no issuer found) |
| 2 | STANCE | 5 | i32 option within the group's stance type (`CombatStanceIndex` 0–5, `WorkerStanceIndex` 0–3, caster/packer 0–1; −1 next, −2 previous) | G | Group::action_stance (orders-spec.md §2.3) |
| 3 | FORM | 13 | i32 form, i32 rotate, queued | G | Group::action_form |
| 4 | ATTACK | 17 | i32 ox, i32 whom, i32 ignore, queued | G | Group::action_attack |
| 5 | SIEGE_ATTACK | 13 | i32 ox, i32 whom, queued | G | Group::action_siege_attack |
| 6 | SWARM_AROUND | 17 | i32 ox, i32 whom, queued, i32 orders | G | Group::action_swarm_around |
| 7 | MOVE_TO | 22 | i32 to_x, i32 to_y, i32 set_angle, i32 angle, i8 orders, i8 queued, i8 form, i8 width, i8 disembark | G | Group::action_move_to |
| 8 | MOVE_NEAR | 26 | i32 to_x, i32 to_y, i32 tolerance, i32 set_angle, i32 angle, i8 orders, i8 queued, i8 form, i8 width, i8 disembark | G | Group::action_move_near (no issuer found) |
| 9 | ATTACK_GROUND | 10 | i32 to_x, i32 to_y, i8 queued | G | Group::action_attack_ground |
| 10 | PATROL | 10 | i32 to_x, i32 to_y, i8 queued | G | Group::action_patrol |
| 11 | LAUNCH_PATROL | 25 | i32 to_x, i32 to_y, queued, i32 shift, i32 ctrl, i32 alt | G | Group::action_launch_patrol |
| 12 | HALT | 1 | — | G | Group::action_halt |
| 13 | TRANSPORT | 1 | — | | Group::action_transport (no issuer found) |
| 14 | SET_TRANSPORT | 5 | i32 flag | G | Group::action_set_transport |
| 15 | BOARD_SHIP | 9 | i32 ox, queued | | Group::action_board_ship (no issuer found) |
| 16 | REPAIR | 13 | i32 ox, i32 whom, queued | | Group::action_repair (no issuer found) |
| 17 | TRADE | 21 | i32 ox, i32 whom, i32 oxx, i32 whose, queued | G | Group::action_trade |
| 18 | CITY_GATHER | 9 | i32 t, queued | G | Group::action_city_gather |
| 19 | GATHER | 9 | i32 ox, queued | G | Group::action_gather |
| 20 | GARRISON | 13 | i32 ox, i32 whom, queued | G | Group::action_garrison |
| 21 | DISBAND | 5 | i32 all | G | Group::action_disband |
| 22 | GATHER_POINT | 17 | i32 x, i32 y, i32 action, i32 add_to_end | G | Group::action_gather_point |
| 23 | SPELL | 21 | i32 ox, i32 whom, i32 type, i32 x, i32 y | G | Group::action_spell |
| 24 | QUEUE_UP | 9 | i32 type (TypeIndex), i32 num | G | Group::action_queue_up |
| 25 | BUILD | 25 | i32 x, i32 y, i32 x2, i32 y2, i32 type, queued | G | Group::action_build |
| 26 | EJECTALL | 17 | i32 back_to_work, i32 who, i32 eject_o, i32 eject_who | G | Group::action_eject_all |
| 27 | ALARM | 1 | — | G | Group::action_alarm |
| 28 | FLIGHT | 25 | i32 ox, i32 whom, i32 shift, i32 ctrl, i32 alt, i32 orders | G | Group::action_flight |
| 29 | STOP_SPELL | 1 | — | G | Group::action_stop_spell |
| 30 | FOLLOW | 13 | i32 ox, i32 whom, queued | G | Group::action_follow |
| 31 | GUARD | 13 | i32 ox, i32 whom, queued | G | Group::action_guard |
| 32 | UNITMASK | 9 | i32 unitmask, i32 set | G | Group::action_unitmask |
| 33 | BUILDMASK | 9 | i32 buildmask, i32 set | G | Group::action_buildmask |
| 34 | HOTKEY | 25 | i32 group, i32 clear, i32 valid, f32 x, f32 y, i32 zoom | G | HotKeyGroups::copy_group (control groups) |
| 35 | RECALL | 1 | — | G | Group::action_recall |
| 36 | SCRAMBLE | 1 | — | G | Group::action_scramble |
| 37 | TREATY | 13 | i32 who, i32 whom, i32 treaty | | Leader::action_treaty |
| 38 | DECLARE | 13 | i32 who, i32 whom, i32 treaty | | Leader::action_declare |
| 39 | CLEAR_TRIBUTES | 9 | i32 who, i32 whom | | Leader::action_clear_tributes |
| 40 | CLEAR_ALL | 9 | i32 who, i32 whom | | Leader::action_clear_all |
| 41 | ACCEPT | 9 | i32 who, i32 whom | | Leader::action_respond |
| 42 | REJECT | 9 | i32 who, i32 whom | | Leader::action_respond |
| 43 | TRIBUTE | 17 | i32 who, i32 whom, i32 good, i32 amount | | Leader::action_offer |
| 44 | DEMAND_TRIBUTE | 17 | i32 who, i32 whom, i32 good, i32 amount | | Leader::action_offer |
| 45 | PROPOSE_ATTACK | 17 | i32 who, i32 whom, i32 whose, i32 onoff | | Leader::action_attack |
| 46 | BUY | 13 | i32 who, i32 good, i32 flags | | Leader::action_buy |
| 47 | SELL | 13 | i32 who, i32 good, i32 flags | | Leader::do_sell (market, embargo and prerequisite checks inline) |
| 48 | UNQUEUE | 15 | i32 who, i32 o, i32 type, i16 uid | | Build::action_unqueue / Unit::action_unqueue |
| 49 | COME_OUT | 11 | i32 who, i32 o, i16 uid | | Unit::action_come_out |
| 50 | PING | 9 | i32 x, i32 y | | Leader::action_ping (minimap ping) |
| 51 | SPLINE | 6+8n | u8 type, u8 flags, u8 cmd, u16 n, n × {f32 x, f32 y} | | ArrowSpline::receive (drawn arrows; presentation) |
| 52 | SPEED_SET | 5 | i32 speed | | TurnControl::set_new_target_time |
| 53 | SPEED_UP | 1 | — | | TurnControl::speed_set |
| 54 | SPEED_DOWN | 1 | — | | TurnControl::speed_set |
| 55 | MP_LOG | 1 | — | | toggles Game.semaphore byte0 bit 0x20 (logging) |
| 56 | CHECK_RANDOM | 5 | u32 seed | | log only (no issuer in this build) |
| 57 | CHECK_SUMS | 65 | 16 × u32: units, builds, walls, ammo, deaths, groups, guys, leaders, cities, items, goods, world, rules, scenario_data, script_run_time, all | | `checksums[play] = all` (§11) |
| 58 | NEXT_CHECK_SUM | 6 | u8 checksum_type, u32 checksum | | `checksums[play] = checksum` unless rechecking (no issuer found) |
| 59 | CHEAT_VIEW_ALL | 5 | i32 who | | Game::action_cheat_view_all |
| 60 | CHEAT_GIVE_TECHS | 5 | i32 who | | Game::action_cheat_give_techs |
| 61 | CHEAT_ZERO_TECHS | 5 | i32 who | | Game::action_cheat_zero_techs |
| 62 | CHEAT_AI_SPEED_INCREASE | 1 | — | | GameAccess::ai_speed |
| 63 | CHEAT_AI_SPEED_NORMAL | 1 | — | | GameAccess::ai_speed |
| 64 | CHEAT_AI_TOGGLE | 1 | — | | GameAccess::ai_off |
| 65 | CHEAT_INCREASE_BUCKETS | 5 | i32 who | | Game::action_cheat_increase_buckets |
| 66 | CHEAT_ZERO_BUCKETS | 5 | i32 who | | Game::action_cheat_zero_buckets |
| 67 | CHEAT_INIT_UNIT | 17 | i32 who, i32 t, i32 x, i32 y | | Game::action_cheat_init_unit |
| 68 | CHAT | 19+2n | i32 bits, i32 taunt, i32 taunt_num, i32 n, n × u16 text, u16 0 | | Leader::receive_chat / receive_taunt, Taunts::play, ConsoleWin::parse_cmd (chat cheats) |
| 69 | CHAT_SET | 9 | u8 status[8] | | chat recipients (presentation) |
| 70 | RESIGN | 5 | i32 play | | Player::resign |
| 71 | QUIT | 7 | i32 play, u8 replay, u8 system_quit | | Player::quit |
| 72 | CAMERA | 10 | u8 zoom, i32 x_loc, i32 y_loc | | Camera::zoom_to_level / pan_to, followed player only (presentation) |
| 73 | LEADER_OPTIONS | 33 | i32 who, i32 peasants, i32 peasants_wait, i32 buildings, BitMask<32> flags {i32 bits=32, i32 size=4, i32 flags, u8[4]} | | leader options (autonomy settings) + Unit::set_stance |
| 74 | TURN_DATA | 11 | u16 ping_time (bit 0x80 = observer/cheat flag), u16 frame_average, u16 wait_time, u16 game_lag, u16 forced_loads | | TurnControl.last_*[play], turn_flags bit (§10); bit 0x80 → action_cheat_view_all |
| 75 | RENAME_CITY | 53 | i32 who, i32 o, u16 name[22] | | city rename |
| 76 | PAUSE | 2 | u8 state | | TurnControl::process_pause |
| 77 | CANNON_TIME | 2 | u8 state | | TurnControl::start_cannon_time |
| 78 | CONSOLE_CMD | 521 | i32 mouse_x, i32 mouse_y, u16 cmd[256] | | ConsoleWin::parse_cmd |
| 79 | PLAYER_SPEED | 9 | u8 × 8: frames_zoomed_in, frames_zoomed_out, clicks, hotkeys, minimap_clicks, mainmap_clicks, control_groups_formed, control_groups_activated | | adds to `Player.synced_*` (statistics; part of the save state) |
| 80 | UNGRACEFUL_PLAYER_DROP | 3 | u8 play, u8 state | | DropControl::process_drop |
| 81 | MARWAN | 2 | u8 start | | log only |

Other issuers: `GroupOut::issue_*` (duplicates of the CommandManager versions), `Options::do_stance / do_alarm /
do_back_to_work / scramble_aircraft / recall_aircraft / set_air_repeat / picked_spot` (FLIGHT), `Console::on_key_down`
(HOTKEY), `IFaceDiploBase::on_key_click` (PAUSE), `TurnControl::issue_speed_* / issue_toggle_pause`, `UnitOut::issue_come_out`,
`BuildOut::issue_unqueue`, `PingSplineGenerator::issue_spline`, and `Game::do_frame` itself (`issue_player_speed` when
`(frame & 7) == console.play`).

Automatic per-turn commands: single player gets CAMERA every frame (`do_frame_solo` at each turn end), PLAYER_SPEED every 8
frames, and LEADER_OPTIONS at start. Multiplayer gets CHECK_SUMS, TURN_DATA and CAMERA in every player's package every turn
(`issue_check_sums`, then `send_local_package` → `issue_turn_data`, `issue_camera`).

## 10. Turn timing (`TurnControl`)

- `timings[speed]` = {200, 125, 67, 50, 1} ms per sim frame (@afc4a4). `speed` starts at the setup's `game_speed`.
- `Game::do_frame` advances `frame` by 1. `tick` increments every 15 frames. `TurnControl::check_cannon_time` runs on each tick.
- `Game::loop` → `TurnControl::do_frame` → solo or multiplayer:
  - **Single player** (`do_frame_solo` @9556a0): `turn_length = 1`. Every OS-loop iteration runs `process_turn` (record and execute
    the local package if it is non-empty). If `check_new_frame_solo` says a frame is due, `Game::do_frame` follows. So commands
    run before the step of the frame they were recorded with. Several records can share a frame.
  - **Multiplayer** (`do_frame_multi` @955440): when a frame is due, `turn_counter++`. At `turn_counter == turn_length`, the game runs
    `process_turn` (checksums → send own package stamp *s* → execute every player's package stamp *s−1*) and
    `compute_next_turn_multi`, then the frame. A command therefore runs **one turn after it was issued**.
  - Initial `turn_length` = `200 / timings[speed]` (MP). After each turn, `compute_next_turn_multi` @955880 recomputes it
    from the recorded TURN_DATA:
    `T = clamp(max(2 × max ping_time among players whose TURN_DATA arrived this turn (raw u16, default 200), min_turn_time), ≤ 1000)`,
    `turn_length = ceil(T / timings[speed])`. If no TURN_DATA arrived, `turn_length = 1`.
    `min_turn_time` = 80 × (players with `flags & 5 == 5`), clamped to [400, 600] (`TurnControl::init`).
    The wall-clock lag adjustments in the same function change only real-time pacing, never frames.
- Playback uses `check_new_frame_playback` (frame rate from `playback_speed`) and the same turn logic, and reads records
  with `frame ≤ game.frame`. Turn boundaries can be recomputed deterministically, but a reimplementation does not need
  them: the record frames are authoritative.

## 11. Checksums (the per-turn oracle)

`CheckSum` is a DataWalk with `checksum = 1`, `flags = −1`. `walk_function(a, b)` does
`accum = adler32(accum, a, b − a)` (zlib Adler-32), and section tags are not hashed. Each component starts with
`accum = 1`. With `checksum ≠ 0`, the `walk_data` methods skip non-simulation parts (strings, presentation state).

`CommandManager::issue_check_sums` @940770, called in multiplayer only, at the start of each turn's `process_turn`, before any package
of that turn runs. It fills the CHECK_SUMS fields in this order:

| Field | Walk |
|---|---|
| units | every live unit (`Object` flag bit 0) of each active leader: unit vfunc +0x7c (walk_data) |
| builds | every building: walk_data of its object |
| walls, ammo | `check_walls`, `check_ammo` |
| deaths | Objects death list (0xa4-byte entries): tag, first dword, and if non-zero bytes 4..0x4b |
| groups | `Group::walk_data` for every group, then 8 dwords of the array that `groups+0x3c` points to, folded in with adler32 |
| guys | each live unit's `PtrArray<Guy>` (unit+0xe4) |
| leaders | `LeaderData::walk_data` for each leader |
| cities, items, goods | `check_cities`, `check_items`, `check_goods` |
| world | `World::walk_data(…, −1)` if World+0x134 ≠ 0, else stays 1 |
| rules | `Game::walk_rules_data` (names skipped) |
| scenario_data, script_run_time | `ScenarioData::walk_data`, `RunTimeEnv::walk_data` |
| all | 32-bit sum of the 15 values above |

`process_check_sums` stores `all` per sender. `CommandPackage::end_process` compares the senders' values after each turn and
counts failures against `checksum_failure_threshold`. Past the threshold it reports the desync: an OOS popup, a SyncLogger dump and a telemetry event.

**Using it as an oracle:** in a multiplayer replay, a CHECK_SUMS command executed at turn boundary *F_k* (record frame
*F_k*) describes the state at the **previous** boundary *F_(k−1)*, before that boundary's packages ran. Compute the 16
sums at every boundary before executing commands and compare one turn later. Single-player replays contain **no
checksums**. During any playback, `CheckSums::check_all` @936560 runs before each package and logs the 15 components and the
total to SyncLogger (the Skybox sync log, configured by `.\synclogger.ini` `[Settings]`, written as `SyncLog <x> <y>.txt`),
so running the original game on Windows with sync logging enabled could give per-package oracles for single-player
replays too (❓ untested). The game RNG is **not** in any checksum (CHECK_RANDOM has no issuer), so an RNG divergence shows
up only through its effects.

## 12. Random numbers

`Random` (basic:random.obj) is a single-dword LCG, shared by every instance:

```
next():          seed = seed * 0x19660D + 0x3C6EF35F             (mod 2^32)
get(lo, hi):     if lo == hi return lo; swap so lo < hi; next(); return lo + (((seed & 0xFFFF) * (hi - lo)) >> 16)
                 // hi is exclusive; asserts lo, hi ≤ 0xFFFF
get():           next(); return float(bits(0x3F800000 | (seed & 0x7FFFFF))) - 1.0      // [0,1), computed in double, rounded to float
reseed(s):       seed = s; returns the old seed
```

| Instance | Seeded | Used by |
|---|---|---|
| **`game_random`** (`GameAccess::game_random` → `::game_random` @e37a8c) | `= GameInfo.seed` in `Game::run` before `Game::init` (so map generation and simulation share **one** stream). Saved and restored by `WalkDataGame::walk_data`, zeroed by `Game::close`. Scripts can set it with `rand_seed(n)` (`n < 0` → `timeGetTime()`) | **all simulation randomness**: random-map generation (`Map*::make_continents`, `TerrainGroups`, `Mountains`, `Cliffs`, `ResourceDivvyPool`), `Setup::build_game`/`place_unit`, `Game::init_teams/init_tribes/init_starting_resources/init_rules_and_teams`, `Objects::process_all`, `Unit::*` (move, fight, plunder, guard, scout, collision, attack position, goody, come_out, spec anim, air physics, fish, spellcaster, gather), **`Guy::set_anim` / `init_real`** (animation variant choice consumes the sim RNG), `Ammo::init/do_damage/init_crash`, `Object::take_damage/fire_ammo/do_launch/disband`, `Animal`, `Herd`, `Farms`, `Army::find_target`, `PathFinder::astar_path` and road/river costs, `GameDaemon::calc_market`, `SpellType::cast_pilfer`, `Leader::*` AI (random_personality, diplomacy, compute_sites, produce/create, research, use_market), BHS `rand_int`/`rand_real`, `GameAccess::rnd(n)` = `get(0, 0xFFFF) % n` |
| `internal_random` (`::random()`, @eb697c) | never seeded from the game | presentation only: particles, debris, surf, river doobers, grass, underlays, AI chat-line choice (`Leader::diplomacy`, `process_taunt`, verified to choose message strings only), `Game::do_frame`'s test delay |
| `SoundGlobal::random`, `SoundType::random` | — | sound variant choice (inlined `SoundGlobal::play` in sim functions such as `Group::action_queue_up`, `Wall::kill_at_tile`, `Nuke::add_nuke`) |
| package padding Random | `= GameInfo.seed` per package | MP padding only (§8) |

Because there is a single sim stream, **call order is the contract**. Every `game_random` call in `Game::do_frame` order (§13),
including AI and per-unit iteration order, must be reproduced exactly. Presentation code must never draw from it.
Scenario import: `World::compute_val` draws once per non-water wcoord (into `WData.val`), then
`ScenarioEditor::init_terrain` **resets `game_random` to `GameInfo.seed`** before the objects are loaded
(world-spec.md §3).

## 13. `Game::do_frame` @591ef0: per-frame order (additions to systems-map.md)

1. Frame-time stats (wall clock, not sim). Optional test delay (`internal_random`).
2. If `(frame & 7) == console.play`: `issue_player_speed` (queues a PLAYER_SPEED for the **next** `process_turn`).
3. Keyboard snapshot. Scenario script, general-powers script (`frame > 0`), Conquer the World reinforcements, tutorial prompt.
4. Ranked ELO penalty upload at tick 60 (not sim).
5. `Leaders::process_all`. Rush-rules war declaration (setup `rush_rules` > 8) at `frame == rush_rules[r]+0x3c × 900`
   (900 frames = 60 ticks).
6. `Leaders::strategy_all` (skipped when semaphore byte1 & 0x08), `GameDaemon::process_all`, `Armies::process_all`.
7. `Objects::process_all`, `Objects::inc_time`, `GraphicEvents::process`.
8. `Leaders::end_process_all`, `Achieve::capture_data`, `Leader::process_event_frame` for each active leader.
9. `frame++`, `OrdersMemManager::cycle`, `Roads::scan_and_kill_stray_roads`. If `frame % 15 == 0`: `tick++` and `check_cannon_time`.
10. Auto save/load test hook, zoom statistics (`Player.accum_*`, folded into PLAYER_SPEED), end-game check
    (semaphore byte2 & 0x40 → `process_end_game`).

`NetDaemon::process_all` calls in between only move network messages.

## 14. Playback algorithm for the reimplementation

```
read header → GameInfo (seed, setup, players), Game block, semaphore (MP?), save_name
if save_name: load the embedded save (needs the save decoder)  else: build the game from setup with game_random = seed
skip random-info; if no save: load the recorded rules block and use it instead of the XML rules
frame = header.frame
loop:
  while next record.frame <= frame: decode (de-XOR + padding if MP), execute its commands in order, keeping the
                                     current-group state (package.group, last_*_received[play])
  (MP oracle) at turn boundaries: compute CHECK_SUMS before executing, compare with the next turn's CHECK_SUMS
  Game::do_frame()   (frame++)
```

## Evidence

| Function | Address | Used for |
|---|---|---|
| `RecordGame::init_record / write_header / write_ctw_header` | 9537a0 / 952a50 / 952990 | header, CTW header, file naming |
| `RecordGame::write_save_game / write_random_game_info / write_rules / write_package` | 953290 / 9534e0 / 953630 / 952fb0 | sections 4–7 |
| `RecordGame::read_header / read_package / read_random_game_info / read_rules / finalize` | 953710 / 952d90 / 9533e0 / 953610 / 952b40 | playback reader, gzip on close |
| `File::open` | a2dcd0 | mode bits: 1 = gz, 2 = write, 4 = append, 8 = text |
| `Game::run / run_playback / walk_data / walk_rules_data / do_frame / get_patch_version` | 584590 / 586ea0 / 589600 / 589550 / 591ef0 / 595260 | order of sections, header, rules, frame step |
| `GameInfo::walk_data`, `Player::walk_data`, `String::walk_data`, `SimpleArray<u16>::walk_data` | 5d6570, 6ee2d0, a1b2d0, 476610 | primitives and header layout |
| `SaveGame::walk_function / walk_test`, `LoadGame::walk_test / load_game` | 43d730 / 43d840, 43da60 / 5a72f0 | raw ranges, 1-byte tags, save magic |
| `Types::walk_rules_data`, `Type/ObjectType/UnitType/BuildType/GoodType/TechType/SpellType::walk_rules_data`, `Balance::walk_rules_data` | 669800, 663190 / 65fba0 / 61d190 / 631f50 / 66fab0 / 66d5c0 / 675400, 582cc0 | rules block |
| `CommandManager::process_turn / send_local_package / process_command_package_data / issue_*` | 93ef10 / 940120 / 93fcd0 / 940390… | writers, turn flow, verb encodings |
| `CommandPackage::process / process_all / process_group / process_* / add_command / add_group / add_chat / add_spline / send / clear / init / init_class / end_process` | 94a700 / 94c500 / 94a0c0 / 943660… / 94bae0 / 94bb60 / 94c080 / 94bfb0 / 94c1e0 / 94c1c0 / 94b790 / 94c440 / 94c800 | dispatch, XOR, padding, group state, checksum compare |
| `DropControl::process_drops` | 9598b0 | clear-text drop packages |
| `TurnControl::init / do_frame / do_frame_solo / do_frame_multi / compute_next_turn_multi / check_new_frame_*`, `timings` | 957e20 / 957dd0 / 9556a0 / 955440 / 955880 / 9560c0… , afc4a4 | turn timing |
| `CommandManager::issue_check_sums`, `CheckSums::check_all / check_*`, `CheckSum::walk_function` | 940770, 936560 / 936bb0…, 936ff0 | checksums |
| `Random::get / get(lo,hi) / reseed`, `GameAccess::rnd`, `MathUtilFuncSet::rand_*`; xrefs of `game_random`, `internal_random`, `SoundGlobal::random` | a39cf0 / a39d70 / a39d30, 43cca0, 9e1890… | RNG (xref lists in `re/analysis/xrefs_random.txt`, `callers_random.txt`) |
| Struct layouts (`*Command`, `CommandPackage`, `GameInfo`, `Player`, `Game`, `TurnControl`) | `rise.pdb` | `re/types/commands.txt`, `re/types/replay.txt`, `re/types/replay_player.txt` |

Decompiled sources: `re/decomp/replay/`, `replay2/`, `replay3/`, `replay_rules/`, `replay_rng/`, `replay_sync/`
(jobs `re/jobs/dc_replay*.txt`).

## Open questions

1. **Real replays.** ✅ Done for decoding: 95/95 of the user's recordings decode (`--survey`), including XOR, padding, CHECK_SUMS, TURN_DATA;
   see [oracle-corpus.md](oracle-corpus.md). Open: the *values* (compute the sums from our own state and compare).
2. **Embedded save body** (`WalkDataGame::walk_data`): now described in [state-spec.md](state-spec.md) and decoded by
   `re/tools/savdump.mjs --replay`; 59 corpus replays embed a save (14 single-player, 45 multiplayer); `rcxdump` still locates the stream past it by scan (cross-checked against the `.SVX` sizes).
   `savdump` itself needs the missing `re/iface/datawalk/*.json` fragments (oracle-corpus.md §7).
3. **Conquer the World replays**: `ConquestGame::walk_data` header not decoded. They play back through `run_solo(…, 4)`.
4. Tag bytes: solved. `walk_test` writes the low byte of `String::generate_hash`'s case-insensitive result; reimplemented in
   `re/tools/walklabel.mjs` (state-spec.md §2).
5. `GameInfo.version` packing vs `"03.02.03.2905"` (`Version::init(String)` not traced). The random-info presence test
   assumes every EE replay is newer than the expansion's release version.
6. `process_group` edge cases: resolved, see §8 and orders-spec.md §1.2.
7. SyncLogger as a single-player oracle: confirm the `synclogger.ini` keys (`StartLoggingOnWorldTimeMS`, desync tracking, …)
   and the log format.
8. Exact checksum coverage per class: done, see [state-spec.md](state-spec.md) §6 and the `checksum` conditions in
   `re/iface/datawalk.json`.
