# Oracle corpus: real recordings from Rise of Nations: Extended Edition

Status: ✅ every replay decodes (95/95) · ✅ checksum rows extracted (172,156) · ✅ `rules` checksum reproduced from the rules block
(11 of 13 eligible games, the other 2 run a script) · ❓ no other CHECK_SUMS field computed yet (needs the engine's own state)

The corpus lives in `re/oracle/` (**gitignored, personal data**: it contains Windows profile paths and player names; never copy
names or file contents into committed files, refer to files by name). It is the fidelity oracle for the reimplementation:
multiplayer replays carry per-turn CHECK_SUMS (docs/replay-spec.md §11, docs/state-spec.md §6), saves (`.SVX`) carry full
state (docs/state-spec.md §4-§5), and replays that embed a save tie the two together.

## 1. Organisation

```
re/oracle/
  Recorded Games/*.rcx          36 single-player replays   (file name: "Playback - YYYY.MM.DD HH'MM'SS (Day).rcx")
  Recorded Games/Multi/*.rcx    59 multiplayer replays
  Saves/*.SVX                   10 saves (+ steam_autocloud.vdf, empty AutoSaves/)
  out/                          derived: survey.txt, survey.tsv, checksums.csv (regenerate, see below)
```

| Property | Value |
|---|---|
| Game build | all 95 replays and all 10 saves: `(Version: 00.2024.06.2000)`, `GameInfo.version` 0x06ac012c (the EE Steam build in `re/bin/`) |
| Container | 86 gzip; 9 plain (uncompressed): 8 are crashed games with a truncated last record, 1 ended cleanly |
| Date range | 2025.06.08 to 2026.09.07 |
| Modes | 36 SP (`semaphore` byte0 = 0x08), 59 MP (0x0c) |
| Start | 36 start from scratch (rules block inside, located exactly); 59 start from an embedded save (14 SP, 45 MP), located by scan |
| Truncated tail | 8 (7 SP, 1 MP); decoded up to the cut, reported as `TRUNCATED` |
| Decoded end to end | 31 (located exactly, no truncation) |
| CHECK_SUMS | MP only: 172,156 packages in 58 of the 59 MP files (the 59th, `Multi/2025.06.10 21'38'50 (Tue)`, has 2 packages and ends at frame 1) |

Regenerate the derived files from the repo root:

```
node re/tools/rcxdump.mjs --survey "re/oracle/Recorded Games" --checksums re/oracle/out/checksums.csv --tsv re/oracle/out/survey.tsv
node re/tools/svxhead.mjs re/oracle/Saves --hex
node re/tools/rulesum.mjs "re/oracle/Recorded Games/Multi/<file>.rcx"      # rules-block checksum
```

The survey takes about a minute (the scan through ~4 MB embedded saves dominates). `checksums.csv` has the header
`file,frame,play,units,builds,walls,ammo,deaths,groups,guys,leaders,cities,items,goods,world,rules,scenario_data,script_run_time,all`
and one row per CHECK_SUMS command, in file order, `file` relative to `Recorded Games/`. In every row `all` equals the 32-bit sum
of the other 15 values.

### Reading the CHECK_SUMS rows

- **Frame.** The `frame` of a CHECK_SUMS row is the record frame at which the command was executed. The sums were computed one turn earlier, at the start
  of the previous turn, before that turn's packages ran (replay-spec.md §11). In every MP file the first CHECK_SUMS is at record
  frame **7** and the next ones follow every 6 frames (all 83,949 gaps between consecutive CHECK_SUMS frames in the corpus are exactly 6, game speed 2; frame 1 holds the first packages, LEADER_OPTIONS and TURN_DATA, with no CHECK_SUMS). So the first sums describe the state right after
  initialisation, with the frame-1 packages not yet applied.
- **Agreement.** Each player sends its own sums every turn. The players agree on `all` in 83,932 of 84,007 distinct (file, frame) turns; 75 turns show a disagreement
  (36 differ only in `leaders`, 3 only in `rules` (below), 36 in several of units/builds/guys/leaders/world: out-of-sync events or a player leaving, not investigated). Compare against the
  majority, or against the player whose `flags & 2` is set (the machine that recorded the file).
- **Rules revisions.** `rules` takes three values in the corpus: 3907925208 (replays from 2025.06 to 2025.12), 314192132 (2026) and, for games running the
  `no_nukes` script, 1896493747 / 1196242567 (the script edits rules at runtime). In 2025.06.11 22'24'21 and the two 2025.06.12 games the recording
  player's `rules` value (3907925208) differs from the other players' (314192132): those players ran another data revision, and the `all` field differs accordingly.
- **Player order.** Within a frame the rows are in player slot order; `play` is the sender slot.
- **Header map_style is the lobby choice, not always the generator.** `World+0x30` (last value of the random-game-info block) is the resolved style.
  Over the 34 from-scratch replays with a random map (no scenario file): 21→21, 14→14, 18→18, 6→6, 8→8, but **1→8, 1→11, 1→14 and 4→6**: style 1 is a random pick, and the resolved
  value must be taken from the random-game-info block (or recomputed from the seed by the generator).

## 2. Decoder state (what changed for the corpus)

`re/tools/rcxdump.mjs` now decodes 95 of 95. The 10 failures had three causes (details and evidence in docs/replay-spec.md §4, §5, §7):

| Cause | Files | Fix |
|---|--:|---|
| Crashed game: a plain (uncompressed) file whose last package is cut short (header cut or payload cut, e.g. 14 of 18 header bytes, 75 of 159). Error `read past end at N (+k)`. 5 files were complete up to the cut (4 SP, 1 MP) | 5 | `readStream` stops at the partial record and reports `truncated {at, have, need}` |
| Same truncation in a save-based replay: the scan requires the chain to end exactly at EOF, so every candidate failed. Error `could not locate the command stream after the embedded save` (one of the three also had the frame-bound problem below) | 3 | the scan accepts a truncated tail |
| Random-game-info block has more entries than the header has players (the two `.scx` scenario replays: 2 header players, 8 and 5 entries). Error `section tag "Game Rules" = 0x0 at 954 / 836` | 2 | the entry count is probed until the `0x92` tag follows (`rgiCount`); in the scan, any count 0..8 with plausible team/leader bytes |

Found while fixing: the header `Game.frame` is not always the embedded save's frame (one replay: header 353231, save and first record at 284305/284306), so a lower bound taken from the
header rejected the real start; the bound is now the frame read from the save itself (`embeddedSaveFrame`). Performance: an 18-byte zero run in a save parses as a record, which
made the scan quadratic; no record has `size 0` (minimum observed 2 in SP, 67 in MP), so those are rejected.

Confirmation that the scan lands on the true stream start: in the 7 replays whose save is also in `Saves/` (6 saves), the located embedded-save size equals the
decompressed `.SVX` size to the byte (table in §6).

## 3. Survey table

Columns: `mode` SP/MP; `style`/`size` = header `map_style`/`map_size`; `pl` = header players (`/n` when the random-info block has another count);
`scx/script` = scenario file (`scx`) and/or game script name; `embedded save` = decompressed bytes of the save copied into the replay (`-` = none);
`CHECK_SUMS` = number of packages and the first-last record frame; `frames` = first-last record frame (start frame is the save's frame for save-based replays);
`status` = `exact` (stream located exactly after the rules block), `scan` (located by scan after an embedded save), `truncated tail`, `plain` (uncompressed).
File names: `Playback - <name>.rcx`, prefixed `Multi/` where shown. `style` 18 = the most common MP style; the table has no player names.

| # | file (`[Multi/]Playback - <this>.rcx`) | mode | style | size | pl | scx/script | embedded save (bytes) | packages | CHECK_SUMS (n, frame range) | frames | status |
|--:|---|---|--:|--:|--:|---|--:|--:|---|---|---|
| 1 | `Multi/2025.06.08 22'44'37 (Sun)` | MP | 21 | 6 | 2 | no_nukes | - | 22458 | 22456 (7-63277) | 0-63277 | exact |
| 2 | `Multi/2025.06.09 11'09'09 (Mon)` | MP | 14 | 2 | 2 | - | - | 25172 | 25170 (7-74155) | 0-74155 | exact |
| 3 | `Multi/2025.06.10 21'38'50 (Tue)` | MP | 21 | 6 | 2 | - | - | 2 | 0 | 0-1 | exact |
| 4 | `Multi/2025.06.10 21'42'55 (Tue)` | MP | 14 | 2 | 2 | - | - | 19997 | 19995 (7-59689) | 0-59689 | exact |
| 5 | `Multi/2025.06.11 22'24'21 (Wed)` | MP | 4 | 2 | 2 | - | - | 4 | 2 (7-7) | 0-7 | exact |
| 6 | `Multi/2025.06.12 17'39'28 (Thu)` | MP | 1 | 4 | 4 | - | - | 8 | 4 (7-7) | 0-7 | exact |
| 7 | `Multi/2025.06.12 17'46'22 (Thu)` | MP | 8 | 3 | 4 | - | - | 8 | 4 (7-7) | 0-7 | exact |
| 8 | `Multi/2025.12.19 21'28'29 (Fri)` | MP | 21 | 6 | 2 | - | - | 1007 | 1005 (7-3019) | 0-3019 | exact, truncated tail, plain |
| 9 | `Multi/2025.12.19 21'43'27 (Fri)` | MP | 21 | 6 | 2 | - | - | 25054 | 25052 (7-73669) | 0-73669 | exact |
| 10 | `Multi/2026.07.19 02'49'17 (Sun)` | MP | 1 | 1 | 2 | - | - | 4372 | 4370 (7-12151) | 0-12151 | exact |
| 11 | `Multi/2026.08.30 21'22'28 (Sun)` | MP | 18 | 6 | 2 | no_nukes | - | 4 | 2 (7-7) | 0-7 | exact |
| 12 | `Multi/2026.08.30 21'23'34 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4082922 | 6114 | 6112 (14-18344) | 7-18344 | scan |
| 13 | `Multi/2026.08.30 21'45'58 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4353564 | 926 | 924 (18351-21117) | 18344-21117 | scan |
| 14 | `Multi/2026.08.30 21'49'39 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4389037 | 134 | 132 (21124-21514) | 21117-21514 | scan |
| 15 | `Multi/2026.08.30 21'50'35 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4389739 | 372 | 370 (21521-22625) | 21514-22625 | scan |
| 16 | `Multi/2026.08.30 21'52'11 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4438799 | 424 | 422 (22632-23892) | 22625-23892 | scan |
| 17 | `Multi/2026.08.30 21'53'58 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4442697 | 410 | 408 (23899-24601) | 23892-24601 | scan |
| 18 | `Multi/2026.08.30 21'55'37 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4435140 | 110 | 108 (24608-24926) | 24601-24926 | scan |
| 19 | `Multi/2026.08.30 21'56'12 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4437805 | 106 | 104 (24933-25239) | 24926-25239 | scan |
| 20 | `Multi/2026.08.30 21'56'43 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4442315 | 106 | 104 (25246-25552) | 25239-25552 | scan |
| 21 | `Multi/2026.08.30 21'57'16 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4449280 | 32 | 30 (25559-25643) | 25552-25643 | scan |
| 22 | `Multi/2026.08.30 21'57'36 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4452827 | 188 | 186 (25650-25716) | 25643-25716 | scan |
| 23 | `Multi/2026.08.30 21'58'34 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4450186 | 418 | 416 (25723-25969) | 25716-25969 | scan |
| 24 | `Multi/2026.08.30 22'00'17 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4461296 | 28 | 26 (25976-26048) | 25969-26048 | scan |
| 25 | `Multi/2026.08.30 22'00'36 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4465549 | 262 | 260 (26055-26247) | 26048-26247 | scan |
| 26 | `Multi/2026.08.30 22'01'46 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4459959 | 358 | 356 (26254-27316) | 26247-27316 | scan |
| 27 | `Multi/2026.08.30 22'03'19 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4472800 | 148 | 146 (27323-27755) | 27316-27755 | scan |
| 28 | `Multi/2026.08.30 22'04'05 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4468231 | 584 | 582 (27762-29502) | 27755-29502 | scan |
| 29 | `Multi/2026.08.30 22'06'21 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4502541 | 790 | 788 (29509-31867) | 29502-31867 | scan |
| 30 | `Multi/2026.08.30 22'09'27 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4501130 | 938 | 936 (31874-34676) | 31867-34676 | scan |
| 31 | `Multi/2026.08.30 22'13'07 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4563254 | 366 | 364 (34683-35769) | 34676-35769 | scan |
| 32 | `Multi/2026.08.30 22'14'36 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4536721 | 114 | 112 (35776-36106) | 35769-36106 | scan |
| 33 | `Multi/2026.08.30 22'15'12 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4538658 | 344 | 342 (36113-37133) | 36106-37133 | scan |
| 34 | `Multi/2026.08.30 22'16'35 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4560770 | 24 | 22 (37140-37200) | 37133-37200 | scan |
| 35 | `Multi/2026.08.30 22'16'53 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4564856 | 206 | 204 (37207-37813) | 37200-37813 | scan |
| 36 | `Multi/2026.08.30 22'17'47 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4566562 | 44 | 42 (37820-37940) | 37813-37940 | scan |
| 37 | `Multi/2026.08.30 22'18'06 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4570457 | 864 | 862 (37947-40527) | 37940-40527 | scan |
| 38 | `Multi/2026.08.30 22'21'11 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4664736 | 374 | 372 (40534-41644) | 40527-41644 | scan |
| 39 | `Multi/2026.08.30 22'22'47 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4635532 | 202 | 200 (41651-42245) | 41644-42245 | scan |
| 40 | `Multi/2026.08.30 22'23'40 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4670331 | 1130 | 1128 (42252-45630) | 42245-45630 | scan |
| 41 | `Multi/2026.08.30 22'27'38 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4651785 | 116 | 114 (45637-45973) | 45630-45973 | scan |
| 42 | `Multi/2026.08.30 22'28'15 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4658320 | 12 | 10 (45980-46004) | 45973-46004 | scan |
| 43 | `Multi/2026.08.30 22'28'28 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4659229 | 2564 | 2562 (46011-53691) | 46004-53691 | scan |
| 44 | `Multi/2026.08.30 22'37'15 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4757332 | 2104 | 2102 (53698-59998) | 53691-59998 | scan |
| 45 | `Multi/2026.08.30 22'44'30 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4710762 | 242 | 240 (60005-60719) | 59998-60719 | scan |
| 46 | `Multi/2026.08.30 22'45'30 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4713502 | 674 | 672 (60726-62736) | 60719-62736 | scan |
| 47 | `Multi/2026.08.30 22'47'57 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4714305 | 12 | 10 (62743-62767) | 62736-62767 | scan |
| 48 | `Multi/2026.08.30 22'48'11 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4714288 | 1136 | 1134 (62774-66170) | 62767-66170 | scan |
| 49 | `Multi/2026.08.30 22'52'08 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4738505 | 94 | 92 (66177-66447) | 66170-66447 | scan |
| 50 | `Multi/2026.08.30 22'52'38 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4738196 | 972 | 970 (66454-69358) | 66447-69358 | scan |
| 51 | `Multi/2026.08.30 22'56'06 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4768895 | 396 | 394 (69365-70541) | 69358-70541 | scan |
| 52 | `Multi/2026.08.30 22'57'36 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4776358 | 4030 | 4028 (70548-82626) | 70541-82626 | scan |
| 53 | `Multi/2026.08.30 23'11'20 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4800258 | 174 | 172 (82633-83143) | 82626-83143 | scan |
| 54 | `Multi/2026.08.30 23'12'05 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4809299 | 94 | 92 (83150-83420) | 83143-83420 | scan |
| 55 | `Multi/2026.08.30 23'12'35 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4810225 | 1362 | 1360 (83427-87501) | 83420-87501 | scan |
| 56 | `Multi/2026.08.30 23'17'25 (Sun)` | MP | 18 | 6 | 2 | no_nukes | 4820938 | 788 | 786 (87508-89476) | 87501-89476 | scan |
| 57 | `Multi/2026.08.31 22'39'48 (Mon)` | MP | 18 | 6 | 2 | - | - | 6610 | 6608 (7-19825) | 0-19825 | exact |
| 58 | `Multi/2026.09.05 20'52'07 (Sat)` | MP | 18 | 6 | 2 | - | - | 19338 | 19336 (7-58009) | 0-58009 | exact |
| 59 | `Multi/2026.09.06 21'11'02 (Sun)` | MP | 18 | 6 | 6 | - | - | 17358 | 17356 (7-50809) | 0-50809 | exact |
| 60 | `2025.06.09 10'23'22 (Mon)` | SP | 21 | 6 | 2 | - | - | 17655 | 0 | 0-17559 | exact |
| 61 | `2025.06.09 18'31'01 (Mon)` | SP | 21 | 6 | 2 | - | - | 76 | 0 | 0-75 | exact |
| 62 | `2025.06.09 18'31'40 (Mon)` | SP | 21 | 6 | 2 | - | 4195313 | 59476 | 0 | 17508-76474 | scan, truncated tail, plain |
| 63 | `2025.06.10 12'24'28 (Tue)` | SP | 6 | 6 | 2 | - | - | 30536 | 0 | 0-30313 | exact |
| 64 | `2025.06.28 13'28'33 (Sat)` | SP | 6 | 4 | 2 | - | - | 4860 | 0 | 0-4841 | exact |
| 65 | `2025.09.12 22'21'52 (Fri)` | SP | 6 | 4 | 2 | - | - | 27109 | 0 | 0-26898 | exact |
| 66 | `2025.09.12 22'54'14 (Fri)` | SP | 6 | 4 | 2 | - | - | 53765 | 0 | 0-53352 | exact |
| 67 | `2025.09.13 15'21'52 (Sat)` | SP | 6 | 4 | 2 | - | - | 188133 | 0 | 0-184808 | exact |
| 68 | `2025.09.14 01'09'04 (Sun)` | SP | 6 | 4 | 2 | - | 4835387 | 110 | 0 | 180904-181013 | scan |
| 69 | `2025.09.14 08'51'12 (Sun)` | SP | 6 | 4 | 2 | - | - | 21051 | 0 | 0-20918 | exact |
| 70 | `2025.09.14 09'15'46 (Sun)` | SP | 6 | 4 | 2 | - | - | 30654 | 0 | 0-30450 | exact |
| 71 | `2025.09.14 09'52'46 (Sun)` | SP | 6 | 4 | 2 | - | 4835387 | 5672 | 0 | 180904-186451 | scan |
| 72 | `2025.09.14 13'07'44 (Sun)` | SP | 6 | 4 | 2 | - | - | 162627 | 0 | 0-159928 | exact, truncated tail, plain |
| 73 | `2025.09.15 23'30'59 (Mon)` | SP | 6 | 4 | 2 | - | 4435356 | 98809 | 0 | 159979-256979 | scan |
| 74 | `2025.09.16 01'24'47 (Tue)` | SP | 6 | 4 | 2 | - | 5025034 | 32 | 0 | 256648-256679 | scan |
| 75 | `2025.09.16 20'35'30 (Tue)` | SP | 6 | 4 | 2 | - | - | 3264128 | 0 | 0-3264117 | exact, truncated tail, plain |
| 76 | `2025.12.14 20'52'50 (Sun)` | SP | 6 | 4 | 2 | - | - | 86081 | 0 | 0-84979 | exact |
| 77 | `2025.12.16 21'07'54 (Tue)` | SP | 6 | 4 | 2 | - | - | 129663 | 0 | 0-127587 | exact, plain |
| 78 | `2025.12.17 17'27'40 (Wed)` | SP | 6 | 4 | 2 | - | 4347405 | 49829 | 0 | 113264-161662 | scan, truncated tail, plain |
| 79 | `2025.12.17 23'55'03 (Wed)` | SP | 6 | 4 | 2 | - | 4920375 | 50671 | 0 | 161710-210922 | scan |
| 80 | `2025.12.18 21'47'50 (Thu)` | SP | 14 | 2 | 2/8 | scx  | - | 8830 | 0 | 0-8768 | exact |
| 81 | `2025.12.19 20'22'09 (Fri)` | SP | 6 | 4 | 2 | - | 4818818 | 438 | 0 | 142933-143367 | scan |
| 82 | `2025.12.19 20'24'38 (Fri)` | SP | 6 | 4 | 2 | - | 5142161 | 492 | 0 | 194797-195287 | scan |
| 83 | `2025.12.19 20'31'57 (Fri)` | SP | 18 | 4 | 4 | no_nukes | - | 572 | 0 | 0-565 | exact |
| 84 | `2026.03.07 15'51'39 (Sat)` | SP | 18 | 4 | 3 | - | - | 58365 | 0 | 0-57601 | exact |
| 85 | `2026.03.07 20'02'58 (Sat)` | SP | 18 | 4 | 3 | - | - | 162 | 0 | 0-161 | exact |
| 86 | `2026.03.07 20'04'37 (Sat)` | SP | 18 | 4 | 3 | - | - | 382773 | 0 | 0-376408 | exact |
| 87 | `2026.03.09 18'22'27 (Mon)` | SP | 18 | 6 | 5 | - | - | 187562 | 0 | 0-185749 | exact, truncated tail, plain |
| 88 | `2026.03.12 23'16'39 (Thu)` | SP | 18 | 6 | 5 | - | 7092021 | 99568 | 0 | 185801-284436 | scan |
| 89 | `2026.03.15 22'01'45 (Sun)` | SP | 18 | 4 | 3 | - | 7468184 | 46081 | 0 | 284305-330080 | scan, truncated tail, plain |
| 90 | `2026.04.25 22'41'37 (Sat)` | SP | 18 | 4 | 3 | - | 5825003 | 203 | 0 | 375834-376036 | scan |
| 91 | `2026.04.25 22'42'25 (Sat)` | SP | 18 | 6 | 5 | - | 7701291 | 53643 | 0 | 330219-383396 | scan |
| 92 | `2026.07.11 00'24'00 (Sat)` | SP | 1 | 6 | 5 | - | - | 195 | 0 | 0-189 | exact |
| 93 | `2026.07.19 01'52'12 (Sun)` | SP | 1 | 6 | 5 | - | 4232994 | 62205 | 0 | 189-62097 | scan |
| 94 | `2026.08.31 23'14'13 (Mon)` | SP | 14 | 2 | 2/5 | scx  | - | 2652 | 0 | 0-2627 | exact |
| 95 | `2026.09.07 20'35'54 (Mon)` | SP | 18 | 6 | 3 | - | - | 223787 | 0 | 0-221718 | exact, truncated tail, plain |

## 4. First targets for the replay oracle

Selection: multiplayer, random map (no `.scx`), no game script (scripts such as `no_nukes` rewrite rules at runtime), small `map_size`, few players, CHECK_SUMS from the start,
decoded exactly, no embedded save (the original save loader is not needed: the replay carries the rules block and the setup, and the game is generated from `GameInfo.seed`).
Every MP file's first CHECK_SUMS is at record frame 7 (the state after initialisation, see §1), so the first comparison of any target is the **post-generation state**
of the map and the starting units, before a single command ran.

Recommended order:

1. **`Multi/Playback - 2025.06.11 22'24'21 (Wed).rcx`**: generator and starting units only. 4 packages, one CHECK_SUMS frame (7). `starting_town=0`, so
   `builds`, `walls`, `cities` are empty (`1`) and the oracle reduces to `units`, `guys`, `leaders`, `items`, `goods`, `world`. 2 players, `map_size` 2.
2. **`Multi/Playback - 2026.07.19 02'49'17 (Sun).rcx`**: same empty start (`starting_town=0`), `map_size` 1 (smallest in the corpus), 2 players, then a real game: 4,372 packages,
   2,185 comparison turns, last frame 12151. First simulation target (movement, gathering, production with no cities yet).
3. **`Multi/Playback - 2025.06.10 21'42'55 (Tue).rcx`**: a normal start (`starting_town=2`, towns exist, `builds` and `cities` non-trivial), `map_size` 2, 2 players, map style 14
   (header and resolved style agree), 19,997 packages, 9,948 turns, last frame 59689. Alternatives with the same setup and a longer game:
   `2025.06.09 11'09'09 (Mon)` (25,172 packages). For a 4-player case: `2025.06.12 17'39'28 (Thu)` (`map_size` 4, 8 packages, frame 7 only).

Fields marked "no town": `starting_town=0` and `starting_resources=0` mean units only, no city; `tech_cost=8` is the other non-default there.

Notes that apply to all three: `team 8` = no team; header `tribe 24` = random nation, resolved in the random-game-info block (leader value = nation index; 15/6, 17/16 below);
`map_style` in the setup is the lobby choice and `world+30` the resolved style; nations in the header of the recording player (`flags & 2`) are the resolved ones.

#### `Multi/Playback - 2025.06.11 22'24'21 (Wed).rcx`

Setup line: `team_style=1 map_style=4 map_size=2 players=0 max_observers=0 game_speed=2 game_rules=1 difficulty=2 starting_town=0 starting_resources=0 starting_resources2=1 tech_cost=8 reveal_map=1 pop_limit=5 rush_rules=0 cannon_times=0 starting_technology=0 starting_technology2=1 ending_technology=7 elimination=1 victory=2 wonderwin=5 score_goal=4 popwin=8 time_limit=1 chairs=3 econwin=2 scenario_type=0 script_type=0 mods=0`

- seed (GameInfo.seed): 154746907 (0x939401b); GameInfo.version 0x6ac012c; info.flags 0x5a0; checksum deep/window/threshold 0/256/0; semaphore bytes 0..3 = 0c000000 (multiplayer + recording)
- players: slot 0: flags 0x5, tribe field 24, who 0, team 8 (none), handicap 0, play 0, difficulty 2; slot 1: flags 0x7, tribe field 24, who 2, team 8 (none), handicap 0, play 1, difficulty 2
- random game info (resolved nation per player, World+0x30 = map_style): player 0: team 8, leader value 15; player 1: team 8, leader value 6; world+30 = 6
- packages 4, first records: frame 1 play 0 (LEADER_OPTIONS+TURN_DATA+TURN_DATA+CAMERA); frame 1 play 1 (LEADER_OPTIONS+TURN_DATA+TURN_DATA+CAMERA); frame 7 play 0 (PLAYER_SPEED+CHECK_SUMS+TURN_DATA+CAMERA); frame 7 play 1 (CHECK_SUMS+TURN_DATA+CAMERA)

First CHECK_SUMS at record frame **7** (2 packages, one per player):

| field | play 0 | play 1 |
|---|--:|--:|
| units | 552184010 | 552184010 |
| builds | 1 | 1 |
| walls | 1 | 1 |
| ammo | 1 | 1 |
| deaths | 1 | 1 |
| groups | 477688821 | 477688821 |
| guys | 3123094529 | 3123094529 |
| leaders | 750479421 | 750479421 |
| cities | 1 | 1 |
| items | 3477165731 | 3477165731 |
| goods | 3146469513 | 3146469513 |
| world | 801218185 | 801218185 |
| rules | 314192132 | 3907925208 |
| scenario_data | 160574352 | 160574352 |
| script_run_time | 262145 | 262145 |
| all | 4213394252 | 3512160032 |

#### `Multi/Playback - 2026.07.19 02'49'17 (Sun).rcx`

Setup line: `team_style=1 map_style=1 map_size=1 players=0 max_observers=0 game_speed=2 game_rules=1 difficulty=2 starting_town=0 starting_resources=0 starting_resources2=1 tech_cost=8 reveal_map=1 pop_limit=5 rush_rules=0 cannon_times=0 starting_technology=0 starting_technology2=1 ending_technology=7 elimination=1 victory=0 wonderwin=5 score_goal=4 popwin=8 time_limit=1 chairs=3 econwin=2 scenario_type=0 script_type=0 mods=0`

- seed (GameInfo.seed): 347288870 (0x14b33526); GameInfo.version 0x6ac012c; info.flags 0x1a0; checksum deep/window/threshold 0/256/0; semaphore bytes 0..3 = 0c000000 (multiplayer + recording)
- players: slot 0: flags 0x5, tribe field 24, who 0, team 8 (none), handicap 0, play 0, difficulty 2; slot 1: flags 0x7, tribe field 24, who 1, team 8 (none), handicap 0, play 1, difficulty 2
- random game info (resolved nation per player, World+0x30 = map_style): player 0: team 8, leader value 17; player 1: team 8, leader value 16; world+30 = 8
- packages 4372, first records: frame 1 play 0 (LEADER_OPTIONS+TURN_DATA+TURN_DATA+CAMERA); frame 1 play 1 (LEADER_OPTIONS+TURN_DATA+TURN_DATA+CAMERA); frame 7 play 0 (PLAYER_SPEED+CHECK_SUMS+TURN_DATA+CAMERA); frame 7 play 1 (CHECK_SUMS+TURN_DATA+CAMERA)

First CHECK_SUMS at record frame **7** (2 packages, one per player):

| field | play 0 | play 1 |
|---|--:|--:|
| units | 3971917847 | 3971917847 |
| builds | 1 | 1 |
| walls | 1 | 1 |
| ammo | 1 | 1 |
| deaths | 1 | 1 |
| groups | 477688821 | 477688821 |
| guys | 1037032712 | 1037032712 |
| leaders | 3468257968 | 3468257968 |
| cities | 1 | 1 |
| items | 3534050868 | 3534050868 |
| goods | 3582877758 | 3582877758 |
| world | 2393828152 | 2393828152 |
| rules | 314192132 | 314192132 |
| scenario_data | 160574352 | 160574352 |
| script_run_time | 262145 | 262145 |
| all | 1760813576 | 1760813576 |

#### `Multi/Playback - 2025.06.10 21'42'55 (Tue).rcx`

Setup line: `team_style=1 map_style=14 map_size=2 players=1 max_observers=0 game_speed=2 game_rules=1 difficulty=2 starting_town=2 starting_resources=1 starting_resources2=1 tech_cost=3 reveal_map=3 pop_limit=5 rush_rules=9 cannon_times=0 starting_technology=0 starting_technology2=1 ending_technology=7 elimination=1 victory=2 wonderwin=12 score_goal=4 popwin=12 time_limit=1 chairs=3 econwin=2 scenario_type=0 script_type=0 mods=0`

- seed (GameInfo.seed): 460924 (0x7087c); GameInfo.version 0x6ac012c; info.flags 0x121; checksum deep/window/threshold 0/256/0; semaphore bytes 0..3 = 0c000000 (multiplayer + recording)
- players: slot 0: flags 0x7, tribe field 0, who 0, team 8 (none), handicap 0, play 0, difficulty 2; slot 1: flags 0x5, tribe field 11, who 1, team 8 (none), handicap 0, play 1, difficulty 2
- random game info (resolved nation per player, World+0x30 = map_style): player 0: team 8, leader value 0; player 1: team 8, leader value 11; world+30 = 14
- packages 19997, first records: frame 1 play 0 (LEADER_OPTIONS+TURN_DATA+TURN_DATA+CAMERA); frame 1 play 1 (LEADER_OPTIONS+TURN_DATA+TURN_DATA+CAMERA); frame 7 play 0 (PLAYER_SPEED+CHECK_SUMS+TURN_DATA+CAMERA); frame 7 play 1 (CHECK_SUMS+TURN_DATA+CAMERA)

First CHECK_SUMS at record frame **7** (2 packages, one per player):

| field | play 0 | play 1 |
|---|--:|--:|
| units | 95588589 | 95588589 |
| builds | 4013010720 | 4013010720 |
| walls | 1 | 1 |
| ammo | 1 | 1 |
| deaths | 1 | 1 |
| groups | 477688821 | 477688821 |
| guys | 1086021837 | 1086021837 |
| leaders | 1126924800 | 1126924800 |
| cities | 266996512 | 266996512 |
| items | 2123257942 | 2123257942 |
| goods | 450917222 | 450917222 |
| world | 733587243 | 733587243 |
| rules | 3907925208 | 3907925208 |
| scenario_data | 160574352 | 160574352 |
| script_run_time | 262145 | 262145 |
| all | 1557853506 | 1557853506 |


The three games share the same `GameInfo.version` (0x06ac012c) and `checksum deep/window/threshold` (0 / 256 / 0); `scenario_data` is 160574352 and `script_run_time` 262145 in all
three (their empty values). The remaining fields have no engine counterpart to compare with until the sim exists, but two are computable today:

- `groups` = 477688821 in every starting state of the corpus (an empty group list hashed).
- `rules`: **reproduced from the replay's own rules block** with `node re/tools/rulesum.mjs <file>` (Adler-32, accum starts at 1, over the raw ranges of the
  rules block in file order; section tags and String fields skipped; each SimpleArray<u16> hashed as `u32 n` then, if `n != 0`, `u32 alloc, u16, u8 flags` and the `n` values).
  11 of the 13 MP replays that carry a rules block match the recorded sum of the recording player (the one with `flags & 2`), including the three targets (3907925208 for 2025.06.11 22'24'21
  and 2025.06.10 21'42'55; 314192132 for 2026.07.19 02'49'17). The 2 misses are games with the `no_nukes` script (§1). This confirms the model in state-spec.md §6: Adler-32 over raw
  ranges, tags not hashed, names skipped, arrays hashed with their headers.

## 5. Replays that need more than the replay itself

**Scenario-based (need a scenario loader and the original save loader's world):**

- `Playback - 2025.12.18 21'47'50 (Thu).rcx` (SP, `Custom/italy_mp` scenario, 8 random-info entries) and
  `Playback - 2026.08.31 23'14'13 (Mon).rcx` (SP, a `Chess Exercise` scenario, 5 entries). Both start from frame 0 with the rules block, but the map comes from the `.scx`, not the generator. SP: no CHECK_SUMS.

**Game scripts (`no_nukes`):** 48 replays: the 46-file 2026.08.30 chain below, `Multi/2025.06.08 22'44'37` and the SP `2025.12.19 20'31'57` (style 18, `map_size` 4, from scratch, no CHECK_SUMS).
The script edits rules at runtime; in the two MP games that carry a rules block the `rules` column differs from the block's checksum, and `scenario_data` differs from the script-free games. Not first targets.

**Embedded-save replays (59: 14 SP, 45 MP; status `scan`):** they begin from a save of another game state, so the original save loader (`WalkDataGame::walk_data`, state-spec.md §5) is a prerequisite.
Their value is (a) a full-state starting point for long runs and (b) state checks against `.SVX` files, §6.

- **The 2026.08.30 multiplayer chain (46 files: the frame-0 start `Multi/2026.08.30 21'22'28` plus the 45 save-based `Multi/2026.08.30 21'23'34` ... `23'17'25`):** one 2-player game
  (style 18, `map_size` 6, `no_nukes`) recorded as segments: each segment's embedded save has the frame at which the previous segment ended (7, 18344, 21117, 21514, ...,
  87501) and the last segment ends at frame 89476. Frame 0 to 7 is in `21'22'28` (exact), the rest need the save loader: about 15,400 comparison turns in total.
- **SP save-based replays:** `2025.06.09 18'31'40` (17508 to 76474, truncated), `2025.09.14 01'09'04`, `2025.09.14 09'52'46`, `2025.09.15 23'30'59`, `2025.09.16 01'24'47`,
  `2025.12.17 17'27'40` (truncated), `2025.12.17 23'55'03`, `2025.12.19 20'22'09`, `2025.12.19 20'24'38`, `2026.03.12 23'16'39`, `2026.03.15 22'01'45` (truncated), `2026.04.25 22'41'37`,
  `2026.04.25 22'42'25`, `2026.07.19 01'52'12`. SP replays contain no CHECK_SUMS (replay-spec.md §11), so the only checks are the replay-level end states below.

**Full-state pairs (replay start/end against a save, same seed and frame):**

- **`Playback - 2026.07.11 00'24'00 (Sat).rcx`** (SP, exact from frame 0, 5 players, `map_style` 1, `map_size` 6, seed 18119929, 195 packages, last frame 189) ends at frame 189, and
  **`Saves/current 2.SVX`** is the same game at frame 189 (same seed, 5 players, 4,232,994 bytes). That is a **full-state oracle that needs no save loader on the generator side**: generate the map, run 189 frames
  of the commands, and compare with the loaded `.SVX` (the loader is needed to read the expected side). The next replay, `Playback - 2026.07.19 01'52'12 (Sun).rcx`, embeds that same save
  (byte-identical size) and continues to frame 62097.
- `Playback - 2026.04.25 22'42'25 (Sat).rcx` ends at frame 383396 = **`Saves/east indies 2 (rn.SVX`** (same seed 1309756020); its start save (7,701,291 bytes, frame 330219) is not in `Saves/`.
- `Playback - 2026.04.25 22'41'37 (Sat).rcx` starts at **`Saves/NO MISSILES.SVX`** (375834, same seed 1146631147, byte-identical size).

## 6. Saves (`.SVX`)

Format: gzip of `String "RoNSave"`, `u32 sGameSaveVersion`, then `WalkDataGame::walk_data` (docs/state-spec.md §4, §5). The decoder is `node re/tools/svxhead.mjs <file|dir> [--hex]`
(new): it reads only the leading fields, with the primitives of `rcxdump` (it does **not** walk the body). The embedded-save replays above are byte-identical copies of these files (decompressed).

First 64 bytes of the decompressed stream, **identical in all 10 files** (the files first differ at the seed, byte 83):

| offset | bytes | meaning |
|--:|---|---|
| 0 | `07 00 00 00` | String length 7 (UTF-16 units) |
| 4 | `52 00 6f 00 4e 00 53 00 61 00 76 00 65 00` | `"RoNSave"` (single-player save; `RoNMultiSave` would be a multiplayer one) |
| 18 | `10 00 00 00` | `sGameSaveVersion` = 0x10 |
| 22 | `42` | section tag `GameInfo` (first step of `WalkDataGame::walk_data`) |
| 23 | `1a 00 00 00` | String length 26 |
| 27 | `28 00 56 00 65 00 72 00 73 00 69 00 6f 00 6e 00 3a 00 20 00 30 00 30 00 2e 00 32 00 30 00 32 00 34 00 2e 00 30` | start of `"(Version: 00.2024.06.2000)"` (36 of the 52 string bytes, plus the first byte of the next `0`) |
| 79 | `2c 01 ac 06` | `GameInfo.version` = 0x06ac012c |
| 83 | u32 | `seed` (per file, table below) |

Beyond 64 bytes: `checksum_deep/window/threshold` (0, 256, 0), flags, the 30 setup bytes, 8 players, scenario/mod fields, then **182 raw bytes** (Console and object ranges) and the `Game` tag (0x16)
with `GameInfo` repeated; the first raw field of that block is `Game.frame`, then `tick`, then the semaphore and `graphic_tick`. (A save's `Game::walk_data` has no `save_name`; only the replay header does.)
`svxhead` reads all of this and checks it: in every file the two `GameInfo` copies agree on the seed, `tick` = `frame / 15` (integer division), the semaphore bytes are `08 01 00 00 ...` (byte0 0x08 = the game was being
recorded, byte1 0x01; no multiplayer bit, no CTW bit) and the next byte after the Game block is tag `0xec` (`Tribes`), the next section of `WalkDataGame::walk_data`; that byte sits at offset 1275 to 1857 depending
on the file (checked on all 10 files). Everything after it is the part a save loader has to decode.

| save file | stored (gzip) | decompressed | game frame | tick | seed | players | map_style / size | embedded in replay(s) |
|---|--:|--:|--:|--:|--:|--:|---|---|
| `DO NOT DELETE 2.SVX` | 728028 | 5025034 | 256648 | 17109 | 2618890287 | 2 | 6 / 4 | `2025.09.16 01'24'47 (Tue)` |
| `DO NOT DELETE.SVX` | 705361 | 4835387 | 180904 | 12060 | 2540555904 | 2 | 6 / 4 | `2025.09.14 01'09'04 (Sun)`, `2025.09.14 09'52'46 (Sun)` |
| `East indies final surround.SVX` | 792557 | 5615574 | 353231 | 23548 | 1146631147 | 3 | 18 / 4 | - |
| `NO MISSILES.SVX` | 840642 | 5825003 | 375834 | 25055 | 1146631147 | 3 | 18 / 4 | `2026.04.25 22'41'37 (Sat)` |
| `curent dec 14.SVX` | 624136 | 4347405 | 113264 | 7550 | 69343968 | 2 | 6 / 4 | `2025.12.17 17'27'40 (Wed)` |
| `curent dec 17.SVX` | 739759 | 5142161 | 194797 | 12986 | 69343968 | 2 | 6 / 4 | `2025.12.19 20'24'38 (Fri)` |
| `current 2.SVX` | 649403 | 4232994 | 189 | 12 | 18119929 | 5 | 1 / 6 | `2026.07.19 01'52'12 (Sun)` |
| `current RN.SVX` | 905963 | 6059032 | 221518 | 14767 | 476345469 | 3 | 18 / 6 | - |
| `east indies 2 (rn.SVX` | 1215716 | 7857479 | 383396 | 25559 | 1309756020 | 5 | 18 / 6 | - |
| `show dad point injustice.SVX` | 745575 | 5208116 | 209278 | 13951 | 69343968 | 2 | 6 / 4 | - |

## 7. What `savdump` needs (plan for the original save loader)

`re/tools/savdump.mjs` is the generic DataWalk decoder (docs/state-spec.md §7). It **does not run on this Mac**, for three reasons:

1. **`re/iface/datawalk/*.json` (the per-batch fragments) and their merge `re/iface/datawalk.json` are missing.** `re/iface/` is gitignored (`.gitignore`: `/re/iface/`) and did not come with the checkout. `savdump` reads `FRAG_DIR = re/iface/datawalk` in
   `--build` and the merged file in every decode (`MERGED = re/iface/datawalk.json`, built on demand from the fragments). Fragment names seen in the `source:` lines of `docs/state-walk.md`: `A.json`, `B.json`, `core.json`, ... (one per batch of
   `walk_data` functions). Without them there is no machine-readable description, only the human-readable listing.
2. **`docs/state-walk.md` is the surviving form of the same information** (3,791 lines, one section per `walk_data` function with offsets, tags, conditions and PDB field names, `confidence:` and `source:` per function).
   It is generated from `datawalk.json` (`savdump --doc`), so it is complete, but it is prose-like pseudo-code, not JSON. Either (a) write a parser for its fenced blocks (the grammar is small: `raw [a, b) N bytes`, `tag "X" (0xNN)`, `if (cond):` /
   `else:`, `for k in 0..(N)-1:`, `Class::walk_data on <expr>`, `String x`, `SimpleArray`/`ObjectArray`/`PtrArray<T>::walk_data on`, `u32 name (writer stores: ...)`) and rebuild `datawalk.json`, or (b) re-run the Ghidra batches
   (`re/ghidra_scripts/`, `re/jobs/`) that produced the fragments. (a) is far cheaper and testable: the 10 `.SVX` files and the 59 embedded saves are the test set (a correct description ends exactly at EOF, with every tag byte matching).
3. **Auxiliary inputs.** `savdump` also loads `re/bin/rise.pdb` through `re/tools/pdbtypes.mjs` (present here) for field names, and `re/tools/walklabel.mjs` needs `Data/internal_strings.xml` from the install (`~/Games/RiseOfNations`) to resolve label offsets to tag
   bytes. Both are available on this Mac; only the fragments are missing. The decoded tag bytes in the saves (0x42 `GameInfo`, 0x16 `Game`, 0xec `Tribes`) match `state-walk.md`, which supports using it as the source.

What the loader work can rely on, confirmed here:

- Order and field widths of the first section (GameInfo, 182 raw bytes, Game block) as in §6; the sections after `Tribes` are described in `docs/state-walk.md` but have not been walked on real bytes.
- **Test data with known ends:** 10 saves (§6) and the embedded-save byte ranges of 59 replays (`save` column in §3), each of which must be consumed exactly: the embedded-save replays turn the first 4 MB of every one into a free regression test, and
  the replays' next bytes (random-info block, then records starting at the stated frame) show where the walk must stop.
- Expected body sizes: 4.2 to 7.9 MB decompressed (the stored gzip is 0.62 to 1.2 MB).
- Cross-checks after loading: `current 2.SVX` against replay `2026.07.11 00'24'00` run from scratch (§5), and the header frame/seed/players of any save against `svxhead`.

## 8. The replay table (`re/tools/replay-all.sh`)

`bash re/tools/replay-all.sh [--frames N] [--jobs J] [--only REGEX] [--out FILE] [--net]` runs `build/ron-sav <file> --replay` on every recording of
`re/oracle/Recorded Games` (the 59 that start from an embedded save, the 34 random-map ones that start from the header's setup, and the 2 `.scx` scenario games, which
are reported and not run) and writes one row per recording to `re/oracle/out/replay-table.tsv`, then prints the totals. At most `--jobs` simulations run at a time
(default 4). The columns:

| column | meaning |
|---|---|
| `replay` | the date and time of the file name (`Multi/` for the multiplayer folder); never a player name |
| `start` | `save` (the embedded save is loaded) or `setup` (generated from the header: `new_random_game`, the playback prologue, `prepare_fresh_shadow`) |
| `mode` | `MP` or `SP`; a single-player recording carries no CHECK_SUMS rows (replay-spec.md section 11), its row only says so in `note` |
| `boundaries`, `equal` | turn boundaries compared, and how many of them have all 15 parts equal |
| `first_row_parts_equal` | of the 15 parts, how many are equal at the first CHECK_SUMS row (the boundary after the start frame: record frame 7 of a generated start) |
| 15 part columns | the first boundary frame at which the part differs (`1` for a generated start = already the first row), or `=` when it matches through the last boundary compared |
| `net_equals_sav` | with `--net`: whether `ron-net replay` printed the same 15 lines (multiplayer recordings only) |
| `note` | why a row has no numbers |

The corpus has 59 multiplayer and 36 single-player recordings; 45 + 13 multiplayer ones have rows (one more, `Multi/2025.06.10 21'38'50`, ends at frame 1). The table is
the harness of docs/fidelity-plan.md section 3.5: a fidelity change must not move a part earlier in any row (the 45 saved replays: 14,904 boundaries, 1,656 with all 15 parts equal
before stage C, `first_row_parts_equal` 14.8 on average).

The setup of a generated start and its cross-check: `ron-sav <rcx> --setup-check <save>` prints the lobby setup decoded from the header and compares it with the one read
back from the save of the same game (seven pairs, section 5: `2025.09.13 15'21'52`, `2025.09.14 13'07'44`, `2025.12.16 21'07'54`, `2026.03.07 20'04'37`, `2026.03.09 18'22'27`,
`2026.07.11 00'24'00`, `2026.09.07 20'35'54` against `DO NOT DELETE`, `DO NOT DELETE 2`, `show dad point injustice`, `East indies final surround`, `east indies 2 (rn`,
`current 2`, `current RN`): all seven equal in all 31 fields. docs/original-save.md section 6.21 describes the generated start, its shadow and the findings.
