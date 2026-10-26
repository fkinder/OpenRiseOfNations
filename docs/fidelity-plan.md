# Plan: 100 % simulation fidelity to the original

Written 2026-10-26 after fidelity pass 18. Companion to `docs/roadmap.md` (phase 0 "ongoing") and the fidelity logs in
`docs/sim-implementation.md`; the oracle and its tools are in `docs/original-save.md` §6 and `docs/oracle-corpus.md`.

## 1. Definition of done

The engine is "identical to the original" when, for every recorded game of the corpus, `ron-sav <rcx> --replay` and
`ron-net replay <rcx>` report all 15 CHECK_SUMS parts equal at every turn boundary from the first row to the end of the
recording, and the final state hash equals the original's. Measured per stage:

| Stage | Criterion | Today (pass 18) |
|---|---|---|
| A | the target replay `21'23'34` (18,338 frames, 3,056 boundaries) whole | 1,083 / 3,056 boundaries; `ammo` / `goods` / `walls` / rules / scripts to the end |
| B | the 44 other replays with an embedded save, whole (except the rows the oracle cannot carry, §4) | 1,291 / ~14,900 boundaries; 2 replays whole |
| C | the 34 random-map replays (no embedded save: the generator must reproduce the world; the 2 `.scx` games are scenario-based) | run (pass 1, 2026-10-26): 13 multiplayer ones have rows; first rows 5 to 10 of 15 parts equal (7.5 on average; table in docs/original-save.md 6.21, regenerate with re/tools/replay-all.sh), the rest wait on the AI's building sites and the start units (docs/randommap-spec.md "Replay first rows") |
| D | the 10 standalone saves stepped to the next save of their chain with `--compare-next`: 0 mapped members differ | load exact (69/69); stepping leaves the leads of §3.4 |

Presentation (rendering, audio, HUD) is outside this plan: the checksums hash the simulation only.

## 2. Method (what worked, keep doing it)

One fidelity agent per round, every round, with this loop:

1. `--replay` table of the 45 replays before and after; the earliest differing part of the target replay is the task.
2. Name the bytes: `--solve-at` over a stretch of boundaries (a constant delta = static bytes, a moving one = the
   simulation), `b - a*N` bracketing between named objects, `RON_OBJ_LAYOUT` / `RON_DUMP_MEMBERS` (hashed bytes vs
   mapped members: three walls were the hash provider, not the game), the cell-exact CollBlock filter for `world`.
3. Name the code: a coverage diff (`-fprofile-instr-generate`) of the frame window names the branches that run for the
   first time; `--compare-next` to the next replay's embedded save lists the members that differ after a short
   segment; a target scan with additive Adler deltas names a position.
4. Read the asm (`DisasmFunctions.java`), not only the decompile: five walls were decompiler artefacts (loop bounds,
   stack-slot pairs, a byte-offset loop read as 36 iterations, `[esp+0x40]` pairs).
5. Per-behaviour switch builds to attribute every digest change; nothing earlier in any replay without a named cause.
6. Acceptance unchanged: zero warnings, `ron-check` 0, sim-all 156/156, native and engine round trips, `ron-net`
   selftest, the 20,000-frame 8-AI random game, the spell / air / victory / nuke fixtures.

## 3. Work items, in order

### 3.1 Stage A: finish `21'23'34`

Known walls (pass 18): `cities` 6,506 alone, `deaths` 7,094, `units` 7,220, `guys` 8,336 (the lone type-324 unit's
MOVE_TO with `set_angle` at 8,330), `world` 8,342, `builds` 8,354, `groups` 8,828, `leaders` 9,134, `items` 12,782.
Mechanics known to be unvalidated by any matched stretch so far: formation followers behind their slot (BE), follower
relocation, slot loss, the ferry search in a long game, `market_speculation` under an embargo, the `is_jammed` branch of
the air-defence buildings, the missile spline flight. Expect each to appear as a wall once the replay reaches it.
Estimate: 3 to 5 passes (each pass has moved the frontier 150 to 2,300 frames since the tools of passes 13 to 18).

### 3.2 Stage B: the other 44 saved replays

Order them by their earliest differing frame (pass 18: `22'03'19` units 27,527, `22'15'12` builds 36,341, `22'18'06`,
`22'37'15`, the mature saves at +7 to +25) and take the earliest; most fixes there also pay on the target replay's
late game (economy at 25,000+ frames: caravan routes, trade, the AI at war). Fight replays: after the oracle limit of
§4 is applied, their first differing row is also a real wall. Estimate: 10 to 20 passes; the count falls as the
engine's late game matures, because the same mechanics recur across replays.

### 3.3 Stage C: random-map replays

1. **Done (pass 1, 2026-10-26).** The generator path of `ron-sav --replay` / `ron-net replay`: a replay without an embedded save builds its
   world with `new_random_game` from the header's setup (`original_random_setup`, equal to the setup of the seven saved worlds in all fields, `--setup-check`); the
   `original_post_load_effects` prologue and the game script; a fresh-state checksum shadow (`prepare_fresh_shadow`); then the same loop. The table: `re/tools/replay-all.sh`.
2. The first row of every such replay is the oracle of the generator plus the start placement: `world`, `units`,
   `builds` at frame 7 must match. The generator matches the seven saved worlds in land, flags, groups, goods, rivers,
   heights and start towns, but 36 replays x other styles will expose style-specific recipes (the 25 styles each have a
   script); fix per style with `ron-mapgen --compare`-style tables against the first row.
3. Then the replays proceed as in stages A and B.
Estimate: 2 passes for the driver and the first rows, then the stage-B rate.

Status after pass 1: items 1 and 2 are in place for every style the corpus uses (6, 8, 14, 18, 21 and the lobby picks 1 and 4); the generator work it caused is listed in
docs/randommap-spec.md "Replay first rows" (randomize_starts in multiplayer, herds in list 8, `analyze_map`, the build_units farm rule, the order of `Leader::init` and of the empire's objects,
incremental region labels, the `halfland` log). Open, in the order the first rows meet them: `Leader::produce_building`'s sites (22 of 25 buildings of the chain are off), the scouts' ring cell
(a draw is still unaccounted: +958 at frame 7), the fog of reveal-map 3, the game script's first edits to the rules (`no_nukes`: the first row's `rules` is 1196242567, the stream's block hashes to
314192132, the later rows 1896493747). The fully generated single-player `2026.07.11` against `current 2.SVX` (frame 189) is a second full-state oracle for the next pass.

### 3.4 Stage D: the load path (`--compare-next` leads)

The adjacent pairs of the chains list members the engine changes in the first frames after a load that the original
does not: `Region.flags 0x80` (AnybodyHere) in 13 to 14 regions, `Build.frame_started` (35 buildings), herd positions,
`Build.up`, `WData who / val / was_seen`, the idle-variant draw count. Pass 17 judged most of them load artefacts of the
original (its own embedded save is written after a load). Decide each one with the asm of the writer; the ones that
are real go to stage A/B as fixes, the rest are documented as oracle limits.

### 3.5 Keep the harness honest

- **Done:** `bash re/tools/replay-all.sh` runs every recording of `re/oracle/Recorded Games` (the 45 + 13 multiplayer ones with rows, the single-player ones as a note) and writes
  `re/oracle/out/replay-table.tsv` (replay, start, boundaries, fully equal, first-row parts equal, the first differing boundary of each of the 15 parts; `--net` adds the `ron-net` agreement); the STATUS row
  quotes it. Add it to the acceptance of every fidelity commit so no part moves earlier unnoticed (docs/oracle-corpus.md section 8).
- `ron-net replay` must keep agreeing with `ron-sav --replay` (it did from pass 12 on); keep the check in the table.
- The digest of `sim-all` is the regression net for behaviour changes outside the replays (random maps, AI); every
  change is attributed by a switch build before it lands.

## 4. Oracle limits (not engine bugs)

- A save carries no projectiles in flight: a replay that starts from a save taken during a fight cannot match `ammo`
  and the units / guys that the missing arrows hit in its first rows. Count these rows out of the stage-B criterion
  once the next rows match.
- The recorded CHECK_SUMS of a recovery segment come from the recording session; a segment that starts at a snapshot
  frame is compared from that frame.
- `Region.borders` is not walked by the original either: territory is recomputed after a load on both sides.
- No corpus recording contains a flak shell in flight, a cliff map or a conquest battle: those are checked by reading
  the decompile only. New recordings from the Windows game would extend the oracle (any `.rcx` from
  `Documents\My Games\Rise of Nations` of a current install; never commit them, `re/oracle/` is gitignored).

## 5. Rules

The constraints of `CLAUDE.md` hold: no decompiled code in git, never the Steam install, `git add` by file name, player
names never copied out of `re/oracle/`, one validated commit per subsystem, STATUS updated at the end of every round.
