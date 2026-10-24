# Multiplayer: lockstep session, start, drops, desyncs, saves (roadmap phase 8)

Status: ✅ decompiled from `riseofnations.exe` (+ `CrossplayNetLib.dll`, PDB applied) and checked against the 59 multiplayer
replays in `re/oracle/Recorded Games/Multi` (gitignored; refer to files by date only) · 🟡 inferred from code or data, not
proven · ❓ unknown. Builds on [replay-spec.md](replay-spec.md) §7–§11, §14 (package records, verbs, `TurnControl`, checksums);
that text is not repeated. Decompiles: `re/decomp/multiplayer/` (jobs `re/jobs/dc_mp*.txt`). The corpus is described in
[oracle-corpus.md](oracle-corpus.md). Flag/enum names below come from the PDB (`PlayerFlags`, `NetMsgType`, `MPDropOptions`).

## 0. Summary

- Gameplay is **peer-to-peer lockstep, symmetric**. Every human peer builds one `CommandPackage` per turn and broadcasts it
  (`NetSys::send_all`); each peer executes the packages of all peers in slot order 0..7, one turn later. The host is not a
  sequencer and relays nothing in play. Host duties: lobby owner, `TimeSync` reference, drop-vote arbiter 🟡, source of the
  state for OOS recovery and for loaded games.
- The wire layer is an abstract `NetSys` object loaded at run time from `CrossplayNetLib.dll` (`get_netsys_object_ptr`); it
  forwards to `Crossplay::Service()` (`ICrossPlayService`: Steam / PlayFab Party / Xbox lobbies and P2P, in
  `CrossplayProxy.dll`, `PartyWin.dll`, `PlayFabMultiplayerWin.dll`, not analysed). **A port cannot talk to original clients**
  (proprietary services); only replays and saves are compatible. The sim needs from the transport only: reliable ordered
  delivery per sender, broadcast, peer timeout notification.
- Everything the sim sees is in the packages. Wall-clock data (ping, frame time, wait time) travels in `TURN_DATA` and is
  only ever read back from the stream, so `turn_length` is deterministic. **In this build it is constant** (§2.4).
- The original desynchronises in practice: one 2026.08.30 session is a chain of 46 corpus replays, 45 of which ended in an OOS
  and were followed by a restart from the recovery save (§5.4).

## 1. Architecture

### 1.1 Layers

| Layer | Class (addr) | Role |
|---|---|---|
| Sim glue | `CommandManager` (@93ef10 `process_turn`, @940120 `send_local_package`, @93fcd0 `process_command_package_data`) | per-turn package build / exchange / execute |
| Pump | `NetDaemon` (@950f30 `process`, @951300 `process_all`) | polls `netsys`, dispatches by `NetMsgType` |
| Transport interface | `NetSys` (abstract, 65 virtual slots), `NetPlayer` (21 slots) | implemented by `CrossplayNetLibSys` / `CrossplayNetLibPlayer` in the DLL |
| System messages | `CrossplayNetLibSys::handle_network_message` | `IPT_*` 0x80..0x88, see below |
| Lobby / matchmaking | `GameSpy` (name is legacy), `LobbyManager`, `SteamLobbyMgr`, `MultiplayerManager`, `ConnectionData`, `SetupWin` | key/value lobbies, ready flags, countdown |
| Clock | `TimeSync` (@954b90…) | wall-clock offset to the host, pacing only |
| Barrier | `SyncPoint::sync` (@93a2d0) | startup rendezvous |
| Drops | `DropControl` (@95xxxx) | timeouts, flags, votes, replacement |

`NetSys` slots the exe uses (from the DLL vtable): +04 `init`, +08 `close`, +10 `is_host`, +38 `send_dsync(stamp)`, +3c
`check_pulse`, +44 `get_time_out`, +48/+4c `set/get_allow_timeout`, +54 `send(buf,len,peer)`, +58 `send_all(buf,len,flags)`,
+5c `get(buf,&peer,&len)`, +90 `find_player_from_id`, +9c `process_system_messages`, +a0/a4 `send/drop_player`, +f8
`notify_waiting_on_player`. `NetPlayer`: +04 `is_local`, +08 `is_host`, +10 `is_observer`, +38 `get_ping_time`, +4c `get_sync_counter`.

### 1.2 Message framing

Every game message is `u8 type` (`NetMsgType`, bit 6 = "response" flag, masked off on receipt) + packed body, no length
field (the transport keeps message boundaries; `NetMsg_Generic` is ≤ 0x201 bytes).

| Type | Name | Body | Use |
|---:|---|---|---|
| 7 | COMMANDPACKAGEDATA | `u32 stamp, i8 play, i16 size, u8 data[size ≤ 512]` (8-byte header) | **the lockstep package** |
| 10 | SYNCSIGNAL | `i32 play` | `SyncPoint` barrier |
| 12 | TIMESYNC | `u32 sent, u32 received` | clock alignment (client → host) |
| 13 / 14 | DROPVOTE / DROPDECISION | `u8 play, i8 vote` / `i8 vote` | §3 |
| 29 | DROP_FLAG | `i32 pid` | §3 |
| 5, 6, 30, 9 | CHAT, PING, SPLINE, TAUNT | see §7 | out-of-band presentation (observers) |
| 15–25 | GAMEMODSYNC*, SYNCFILE*, SYNCDIR* | file transfer | mods, scenario files, OOS/loaded save |
| 1–4, 27, 28, 31, 32 | PLAYERCONNECTIONDATA…, PLAYER_STATUS_* | lobby data | legacy; the Crossplay lobby uses key/value keys (§2.1) |
| 8 | PAUSE | — | handled nowhere in `NetDaemon::process`; pause is the PAUSE verb |

`IPT_*` (DLL-internal, first byte): 0x80 PLAYERLIST, 0x81 DROPREQUEST, 0x82 CANCELDROPREQUEST, 0x83 PULSEPACKET (heartbeat,
broadcast by `send_pulse`), 0x84 ADDPLAYER, 0x85 DESTROYPLAYER, 0x86 MIGRATEHOST, 0x87 DSYNCMSG (`u8 type, i32 stamp`), 0x88
READYFLAG. `check_pulse` runs every `NetDaemon::process`; a peer silent longer than `time_out` raises `on_player_timed_out`.
❓ default `time_out` and pulse period (set by the lobby layer; not traced).

Transport requirements the sim code relies on: **reliable and ordered per sender** (a package whose stamp ≠ last+1 is
dropped, §1.3); **`send_all` also delivers to the sender** 🟡 (the local package, drop votes and decisions are only ever
sent, never applied locally, yet every peer including the sender consumes them from its own FIFO). Packages are
≤ 8 + 512 bytes, so no fragmentation is needed. Package bodies are obfuscated (replay-spec §8); that is not security.
`NetDaemon::process` case 7 has a branch for `play < 0` that re-broadcasts the message (relay?) ❓ no issuer known.

### 1.3 The turn loop (completes replay-spec §10)

`TurnControl::do_frame_multi` @955440, once per OS-loop iteration, under a re-entrancy guard:
1. `check_new_frame_multi` @9560c0 says a sim tick is due when `target_frame_time × frame_counter + frame_base_time ≤ now`
   (`now = timeGetTime + time_sync.time − base_time`) or no render is pending. Pacing only.
2. `turn_counter++`. If `turn_counter == turn_length`: `CommandManager::process_turn`, then `compute_next_turn_multi`, then
   `turn_counter = 0`.
3. Return `~paused & 1`: **when paused (semaphore bit0) the turn machinery keeps running but `Game::do_frame` is skipped.**
   Corpus: during PAUSE the same `frame` repeats for many stamps (e.g. 11 turns × 2 players at one frame; the 2025.06.08 22'44
   file has 14–184 records at frames 7147, 7249, 10249, 31435). Boundaries are therefore **not** every `turn_length` frames:
   key any MP logic on stamps. `GameInfo` pause quota: 10 pauses per player (`Player.pauses`) unless `info.flags & 0x40`.

`process_turn` @93ef10, multiplayer branch (semaphore 0x04, not 0x10), in order:
1. Zero `CommandPackage::checksums[8]`, `random_seeds[8]`.
2. `issue_check_sums` (into the local package), `DropControl::process_time_outs`, `DropControl::process_drops` (§3).
3. `send_local_package` @940120: if the local player is active (`Player::is_active_connection`: VALID, not WON 0x100, none of
   DROPPED 0x10 / RESIGNED 0x40 / DEFEATED 0x80, has a connection) → `issue_turn_data`, `issue_camera`, `local_package_stamp++`,
   `play`, `stamp`, `CommandPackage::send` (obfuscate + broadcast), clear the package, reseed its padding RNG from `info.seed`.
   If not active the stamp still increments and nothing is sent. `CommandManager::start` sends stamp 1 before the first
   boundary (`issue_turn_data` + `send_local_package`), so stamp 1 = the first package, **executed at frame `turn_length₀ − 1`**
   (`turn_length₀ = 200 / timings[speed]` = 2 at speed 2): corpus first boundary frame 1, then 7, 13…
4. For slot `p` = 0..7, **expected sender** = VALID ∧ ¬WON ∧ ¬(DROPPED|RESIGNED|DEFEATED) ∧ `is_connection` (has `net_player`, or
   is in `player_pendings` / `player_drops`) ∧ ¬OBSERVER (0x08) (MP-playback variant tests HUMAN 0x04 instead). Loop until
   `package_fifos[p]` holds a package: execute it; or `check_for_drop(p)` (pending / dropped) → skip p. While waiting:
   `process_time_outs`, `process_need_syncs`, `NetDaemon::process_all`, `Game::loop_render` every 150 ms.
5. A package is accepted only if `valid ∧ stamp == local_package_stamp − 1`. Otherwise it is still recorded (§6), the log says
   "Invalid command package. Exiting game...", a popup shows, `game->playing = 0`. There is no skip, ghosting or rollback.
6. On accept: `PackageFifo::get_local`, write to the record file (if recording), `CommandPackage::process_all`.
   Stats: `wait_time_per_player[p]`, `wait_time_accum` (ms spent waiting, fed to the next TURN_DATA).
7. `CheckSums::check_all`, `CommandPackage::end_process` (§5). If semaphore byte3 bit 3 (OOS recovery) → `recover_from_oos`.

**Receive side** `process_command_package_data` @93fcd0: ignore unless MP; sender slot must be VALID ∧ ¬WON ∧ ¬0xd0; require
`stamp == package_stamps[play] + 1` (else log "wrong stamp", ignore; this is what makes resends idempotent); copy into
`PackageFifo::add`; a full FIFO (20 entries) is fatal (`playing = 0`).
`PackageFifo` @29f0 bytes = 20 × `CommandPackage` (0x218) with two cursors: `front/length` (history, trimmed to ≤ 3 packages by
`get_local`, used by `resend`) and `front_local/length_local` (not yet executed).
`last_objects_received[play][128]`, `last_uids_received[play][128]`, `last_num_received[play]` are the GROUP-repeat cache of
replay-spec §8, per sender, reset by `CommandPackage::init_class` at game start; they have nothing to do with timing.

**Late packages**: the sim of every peer stalls inside `process_turn`. After 1000 ms the status line shows "Waiting on
$PLAYERNAME0..." with `elapsed / netsys->get_time_out()` as a progress fraction (or "Lost Connection..." with
`local_player_disconnect_pct` when the local transport is `LOCAL_PLAYER_DISCONNECTING`), `notify_waiting_on_player(p)` is called,
and the transport timeout eventually starts a drop (§3). `SyncPoint` disables timeouts during its waits (`set_allow_timeout`).

### 1.4 Turn length

`compute_next_turn_multi` @955880 (after every boundary): `T = clamp(max(2 × max last_ping_times[p] over TURN_DATAs processed
this turn, min_turn_time), ≤ 1000)`, `turn_length = ceil(T / timings[speed])`; `turn_flags == 0` (no TURN_DATA) → 1. It also
shifts `frame_base_time` by the mean lag (machines slow down to the slowest; pacing only) and feeds `MPStatWin`.
`min_turn_time` = 80 × (slots with flags & 5 == 5), clamped to [400, 600] (@957e20).
**New finding: `ping_time` is not measured.** `issue_turn_data` fills it with the maximum `NetPlayer::get_ping_time` over the
other connected players, and `CrossplayNetLibPlayer::get_ping_time` @10027710 returns the constant **200** for a remote
peer and 0 for the local one. The field is masked `& 0xff7f` (bit 7 is the "view all" flag below), so 200 is stored as **72**:
corpus: all 172,400 TURN_DATA have `ping_time = 72`. Hence `T = max(144, min_turn_time)`: with 2–5 humans `T = 400` →
`turn_length = 6` at speed 2 (67 ms); by formula (🟡, no corpus file): 6 humans 480 → 8, 7 humans 560 → 9, 8 humans 600 → 9. Corpus: every gap between consecutive
frames-with-records is exactly 6 (84,007 gaps, 59 files, 2/4/6-player slots, 2–4 humans). `frame_average`, `wait_time`,
`game_lag`, `forced_loads` are logged and fed to pacing/statistics only.
Speed changes (SPEED_UP/DOWN/SET, cannon time) change `timings[speed]` before `compute_next_turn_multi`, so the new
`turn_length` is identical on all peers.
TURN_DATA bit 7 of `ping_time`: set when the sender cannot act (game over, or WON/dropped/resigned/defeated and
(`!MP` or `info.flags & 0x80`)); `process_turn_data` then calls `Game::action_cheat_view_all(sender.who)` so spectating losers
see the whole map (deterministic: it is in the package).

### 1.5 Pause and cannon time

PAUSE verb (state 0/1) → `TurnControl::process_pause` → `toggle_pause(play)`; counts `Player.pauses`; over quota: feedback "no
pauses left" to the issuer only. Cannon time (CANNON_TIME verb, issuer needs `LeaderData.+0x48 > 0` uses left): speed forced
to 0 (200 ms), `cannon_time_stamp = game.frame`; `check_cannon_time` (each 15-frame tick) ends it when `frame − stamp > 0x4a`
(74 frames) and restores `cannon_saved_speed`. Both are frame-based, so deterministic. Neither stops the turn machinery.

## 2. Game start

### 2.1 Lobby → agreement
1. **Lobby state is a key/value store**, not packages: `ConnectionData` (@94xxxx: `player_data_server[8]`, `game_data`
   = `GameConnectionDataFull` 0x7ef bytes) is mirrored through `MultiplayerManager::LobbySetKey` into the Crossplay lobby
   (Steam/PlayFab/Xbox). Keys (strings in the exe): game: `map_size map_style teamstyle gamerules gamespeed game_seed
   lobby_flags elorank cannontime chairs mods scenario_type script_type time_limit victory elimination wonderwin popwin
   score_goal echowin pop_limit rush_rules tech_cost starting_* ending_technology reveal_map scenario_{data,size,checksum,
   num_files,workshop_id} mod_{name,desc,size,checksum,workshop_id} script_name desc hostid`; per slot `n`: `slot_type_n
   tribe_n who_n team_n handicap_n diff_n elo_n steam_ready_n`, plus `player_id`, `platform_player_id`, `name`.
   The 30 setup bytes of `GameConnectionData` are exactly `GameInfo.setup` (replay-spec §3).
2. **The seed is chosen by the host** (`SetupWin::exec` @5c67f0: `info.seed = timeGetTime()` when MP, `game_data.seed =
   info.seed`, published as `game_seed`); clients overwrite from the lobby key. `setup_game` @5b3860 then sets
   `info.seed = connection_data->game_data.seed` on every peer.
3. Host presses start: `GameSpy::send_start_game`, ready flags, 10 s countdown (`SetupWin::countdown` @5bc400, ticks
   chat "n seconds"), `start_game_success`. `validate_start` @5bac20 enforces team/slot rules (team-size limits for team
   styles 4/5/6, at least 2 sides, ranked counts 4/6/8). Version gate: `Game::run_gamespy` hands
   `adler32(exe_version, current_version)` to `netsys->init`; peers with a different build are refused 🟡.
4. `SetupWin::setup_game`: copies combo boxes to `info.setup` and per-slot `tribe/who/team/handicap/diff`, `play = slot`,
   `flags |= VALID` for slot types 0–3, and binds `info.player[i].net_player = netsys->find_player_from_id(player_id[i])`
   (missing → logged "Could not find a netplayer for player id"). The 11 `info.flags` bits are the setup check boxes.
5. **No map or state is sent.** Every peer runs `Game::run` (@584590) with `game_random.seed = info.seed` and generates the
   map locally; rules come from the local XML (their hash is the `rules` checksum; mismatched installs desync at stamp 2: three
   corpus files end at stamp 2 with a differing `rules` sum, 🟡 attributed to different installs). Mods and scenario files are
   verified by size/checksum and transferred with `SyncFile`/`SyncDir` before the start (`validate_file_transfer`,
   `do_scen_script_sync`). Loaded games: §6.
6. `Game::run` rendezvous points: 4 × `SyncPoint::sync` (before init, after load, after interface init, before
   `TurnControl::init`). Each: `set_allow_timeout`, broadcast SYNCSIGNAL, wait until every non-dropped peer's sync counter
   ≥ own counter (`NetPlayer::get_sync_counter`), `NetDaemon::process_all` + `Game::loop_render` meanwhile.
7. `CommandManager::init` (@943310: stamps 0, all FIFOs reset, `local_package.valid = 1`), `TurnControl::init`,
   `TimeSync::sync`, `CommandManager::start` (§1.3.3). Initial per-player packages carry LEADER_OPTIONS + TURN_DATA + CAMERA
   (corpus: all 59 first records `LEADER_OPTIONS,TURN_DATA,TURN_DATA,CAMERA`).

### 2.2 Slots, observers
`play` = lobby slot 0..7 (also the package order). Slot `Player.flags`: VALID 1, LOCAL 2, HUMAN 4, OBSERVER 8, DROPPED 0x10,
RESIGNED 0x40, DEFEATED 0x80, WON 0x100, plus setup bits 0x400/0x800 (warnings) 0x1000 RUSH 0x2000 BOOM. AI slots are VALID
only and send nothing (corpus: 6-slot games have 2 records per turn). Observers: slots above the player count
(`max_observers`) 🟡. Facts: they are never waited for (§1.3.4) and never become drop-pending; their chat/pings go
out-of-band (§7). ❓ whether an observer broadcasts packages at all: `send_local_package` has no observer test, receivers
accept them, but nobody drains an observer FIFO, which would overflow after 20 turns; no observer in the corpus.

## 3. Drops (`DropControl`, UNGRACEFUL_PLAYER_DROP verb 80 = `'P'`)

State (@98 bytes): `votes[8]`, `vote_start_time`, `drop_flags[8]` (per dropped slot, bit = which peers flagged it),
`player_need_syncs`, `player_pendings`, `player_drops[]` of `DropPacket{play, frame, action}`.
1. **Detect**: transport timeout → `NetDaemon::on_player_timed_out` → `on_player_timed_out(p)` (@959110): set own bit in
   `drop_flags[p]`, broadcast DROP_FLAG(pid). A remote flag sets that sender's bit (and triggers an own flag for the local slot).
   `on_player_deleted` (peer left cleanly) forces `drop_flags = 0xff`. If the timed-out peer is the local player, the local
   player drops itself.
2. **Consensus** (`process_time_outs` @959e40, every pump): when every remaining peer has flagged `p`: log, add `p` to
   `need_syncs` and `pendings` (unless `p` is an observer), clear `p.net_player`, start the vote.
3. **Vote** (MPDropWin: 4 options + "Save & exit" + "Exit"): each peer broadcasts DROPVOTE(play, option). The host 🟡
   (decompile hides one test) sends DROPDECISION(option) once all remaining peers voted the same; after **90 s**
   (`check_vote_time_out`) any peer broadcasts the plurality, REPLACE_WITH_AI winning ties (+1), default 3. Options
   (`MPDropOptions`): 0 DROP_PLAYER, 1 DIPLOMACY, 2 SURVIVAL, 3 REPLACE_WITH_AI, 4 SAVE_AND_EXIT, 5 EXIT.
4. **Decision** (`process_drop_decision`): every pending slot moves to `player_drops{play, frame = 0x0fffffff, action}`.
5. **Resync** (`process_need_syncs`, inside the wait loop): `PackageFifo::resend` rebroadcasts the dropped player's last ≤ 3 stored
   packages so peers that missed the final ones converge (idempotent by the stamp rule), and writes the max stamp into
   `DropPacket.frame`. ⚠ Race: a peer that already skips the pending slot (`check_for_drop`) before the resend arrives processes
   fewer packages of that player than a peer that has them. ❓ whether this can occur (needs the dropped peer's last
   broadcast to reach only some peers); the port should not copy it (§8.2).
6. **Apply** (`process_drops` @9598b0, start of the next `process_turn`; entry ready when `frame(=max stamp) < game.frame`):
   build a 3-byte `'P'` package `[0x50, dropped play, action]` (clear text, `play = min(dropped, first connected)`), write it to the
   record, then call `process_drop(play, action)` directly. **On playback** the `'P'` record is executed by
   `process_ungraceful_player_drop` → the same `process_drop` (only when semaphore 0x10). Not seen in the corpus (0 files).
7. **`process_drop` @959500**: option 3 REPLACE_WITH_AI: unless a co-op partner shares the leader (`who`), message
   " converted to computer-player.", clear `Player.flags` 0x14 (HUMAN, DROPPED), clear `LEADER_HUMAN` on the leader, set
   `LeaderData.+0x50 = 3`; the player stays VALID and the AI plays it (difficulty = `multi_diff`, replay-spec/ai-spec).
   Options 1/2: `team_style = 0/1`, every VALID player `team = 8`, `Leader::action_declare(i, j, 0, 1, 1)` for all pairs
   (diplomacy reset), then `Player::drop`. Option 0: `Player::drop` ("$PLAYERNAME0 dropped.", sound, then `leave_game(7)`).
   `Player::leave_game(reason)`: set DROPPED; if another VALID non-dropped HUMAN shares `who` return; otherwise
   `Leader::defeat(leader, reason, …)` (capital variants when `elimination == 1`). Reasons: 6 resign, 7 drop.
8. **Voluntary leave**: RESIGN (`Player::resign(0)`: flag 0x40, `leave_game(6)`) and QUIT (`resign(1)`, local `playing = 0`,
   optional system quit) are normal package verbs, so every peer applies them at the same boundary; the leaver's sender slot
   leaves the expected-sender set (§1.3.4). Corpus: 6 RESIGN, always the last records of the file.
9. **Reconnection: none found** ✅ in play. `net_player` is never restored and DROPPED is never cleared after start. Re-joining
   is via lobby: SAVE_AND_EXIT then load (§6). Host migration exists at transport/lobby level (`IPT_MIGRATEHOST`,
   `NetDaemon/SetupWin::on_host_migrate`); ❓ what in-game state follows a changed `host_player`.

## 4. Package content in multiplayer (additions to replay-spec §8–§9)

Each turn's package, in order: LEADER_OPTIONS (first only), PLAYER_SPEED (every 8th frame for `frame & 7 == play`), the
player's GROUP/verb commands, then appended by `send_local_package`: CHECK_SUMS (via `issue_check_sums`, put first at the
boundary), TURN_DATA, CAMERA. TURN_DATA appears twice in the first package (start + first boundary). Max 512 payload bytes.

## 5. Desync handling

### 5.1 Compare (`CommandPackage::end_process` @94c800, every turn)
`process_check_sums` stores `checksums[play] = all`. `end_process`: reference = first non-zero entry; **mismatch** if any
non-zero entry differs, **or** any peer's `dsync_frame` (`NetPlayer+0xa8`, set by an IPT_DSYNCMSG) is in `[0, local_stamp]`
(a peer already reported OOS). Players absent from the turn (zero) are ignored. Only the aggregate `all` is compared on the
wire; the 15 components exist for the report. `checksum_failure_threshold` (`GameInfo`, lobby key, **0 in all 59 headers**; deep 0,
window 256): `fail_count++`; `< threshold` → "recheck" (`checksum_recheck = 1`, `check_sums.mark = last_mark[1]`, deeper
logging) and continue; else the OOS flow. With threshold 0 the first mismatch is fatal.

### 5.2 OOS flow
1. `netsys->send_dsync(stamp)` broadcasts to the others; popup "Game is Out of Sync in …" + "Click to create local report";
   telemetry `DesyncGame`; `SyncLogger::writeToFileAndReset` (§5.5) and `GameLog::create_report`; all `dsync_frame` reset to −1.
2. Second popup: "Would you like to reload the game and continue playing?" (No/Yes). Branch A: `playing = 0`, semaphore byte3
   bit 3 (OOS recovery pending). Branch B: clear the MP bit and `Leader::defeat(…, 7)` every other leader (continue alone).
   ❓ which button is which (labels are assigned to `exec` results 0/1); the corpus shows branch A being taken.
3. `process_turn` then calls `recover_from_oos` @93e9e0: authority = host if ≤ 2 peers; otherwise the peer whose 8-entry
   checksum vector agrees with the most others (lowest slot wins ties). The authority `SaveGame::save_game(<profile dir>/
   <oos name>, 1)` and `SyncFile::send`s it to everybody; the others `SyncFile::receive`. On success `info.save_name` = that
   file and byte3 bit 3 is set again; `Game::loop` ends; `Game::run` (@584590 line ~1109) sees the bit, clears it and calls
   `Game::run_oos_recovery` @5869e0: tear down the whole game (renderer, objects, leaders, script state), keep `save_name`,
   `TimeSync::sync`, `Game::run` again (loads the save, new `CommandManager::init`: **stamps restart at 1, frame continues**).
   The recorder opens a new `.rcx` that embeds the save (header.frame = frame of the save).

### 5.3 What the first mismatching sum means
`check_strings[15]` (units, builds, walls, ammo, deaths, groups, guys, leaders, cities, items, goods, world, rules,
scenario_data, script_run_time) name the component in the report; component definitions: replay-spec §11, state-spec §6.

### 5.4 Corpus: 48 mismatching turns in 86,087
Components that differed (first two senders compared; 46 of the 48 turns classified): `leaders`+`all` only: **36**;
units+builds+guys+leaders+world (+ammo, groups): 8; `rules`+`all`: 1; leaders+rules+all: 1. The 2026.08.30 session is a chain of
46 files: one fresh start (ends at stamp 2) then 45 files, each starting from the save of the previous one (header.frame =
previous last frame; first boundary = header.frame + 1). 45 of the 46 end exactly at their first mismatching CHECK_SUMS turn
(the last ends with PAUSE + RESIGN); segment lengths 12 … 4,030 frames. Three early 2025.06 files also end at stamp 2.
So (a) OOS recovery works and is exercised a lot; (b) **after loading a save in MP the original diverges again soon, nearly always in `leaders` only**, i.e. some `LeaderData`
field is not round-tripped by save/load or depends on peer-local data ❓ (candidate for the save-port / state-spec work: compare
the `leaders` walk of a segment's first CHECK_SUMS with a fresh-game peer). The port must not assume original saves reproduce the
original continuation bit for bit.

### 5.5 SyncLogger
`SyncLogger` (Skybox, `synclogger.ini`, `SyncLog <x> <y>.txt`): in-memory ring of `logToMemory` lines tagged
(`cCommandManagerSync`, `cNoteOnlySync`…), `newTurn(turn_index)` per boundary, `mDesyncOnTurn = turn_counter`, written on OOS.
Logged per package: stamp, play, each CHECK_SUMS component, TURN_DATA fields, pause, resign, drop. Diagnostic only.

## 6. Saving, loading and recording

- **Save in MP**: any peer may save its own copy (drop window "Save & exit" → `SaveGame::save_game_screen`, then leave);
  autosave hooks are per-peer debug (`Game::auto_save_load`). The OOS save (§5.2) is the only synchronised save.
- **Loaded MP game**: lobby `team_style == 13`, `info.save_name` = the host's file. `Game::run`, `(semaphore & 0x14) == 4`:
  `SyncFile::sync` (NETMSG 17–21: BEGIN, RESPONSE, DATA (256-byte chunks 🟡), VERIFY, END with checksums) copies it from the
  host, then every peer `LoadGame::load_game`s it. The save fixes players/leaders; humans are re-bound to slots in the lobby.
  After the load `CommandManager::init` and `TurnControl::init` run again (stamps from 1).
- **Recording** (`RecordGame`): every peer writes its own file under `…\Recorded Games\Multi\`, header semaphore 0x0c
  (corpus: 59/59). Mechanism as replay-spec §7: packages are written as received (obfuscated) just before `process_all`,
  in slot order, `'P'` packages in clear. Differences from SP: no SP-style "empty turns skipped"; `play`/LOCAL flag identify
  the recorder (so two peers' files differ only in header player flags and name fields); stamps restart at 1 in each segment;
  the embedded save is present when the segment starts mid-game (45/59 corpus files); a crashed game leaves an uncompressed tail.
- **MP playback** (`use_mp_playback`, `check_mp_playback`/`set_mp_playback`, pref key): a recorded MP file can be fed to a live
  session: `send_local_package` reads each human's package from the record and broadcasts it as that player (`valid`
  semantics differ: expected senders are tested with HUMAN). Used for shared replay viewing 🟡; ❓ not traced further.

## 7. Chat, pings, taunts, splines

For an **active** local player (VALID, ¬WON, ¬0xd0, connection) these are package verbs processed in lockstep: CHAT (68;
`Leader::receive_chat`, `Taunts::play`, chat cheats via `ConsoleWin::parse_cmd`), PING (50, `Scene::add_ping` / `Leader::action_ping`),
SPLINE (51). Taunt scheduling is **sim state** (`LeaderData.+0x3b4`, due frame at +0x3d4, `Leader::process_taunt`), so CHAT
must be executed deterministically even though it looks cosmetic. For observers / defeated / dropped local players
`issue_chat` calls `NetDaemon::send_chat` (NETMSG_CHAT, 256 wchar, `observer_to_all`), pings use NETMSG_PING (6) and splines
NETMSG_SPLINE (30); receivers show out-of-band chat only if they are not active players, unless `info.flags & 0x20`
(observer chat to all). NETMSG_TAUNT is a no-op. Lobby chat is lobby-layer.

## 8. What the port needs

### 8.1 Architecture (`engine/net`, new; no sim dependency)

```
Transport (abstract)   reliable ordered per peer; send(peer,bytes) / broadcast(bytes, include_self);
                       poll() -> (peer, bytes); peers(); local_peer(); is_host(); on_timeout(peer); now_ms()
  LoopbackTransport    N endpoints in one process, deterministic delay/jitter, loss=0 (tests; also unit-test stalls)
  ... real one later   (UDP/ENet or platform P2P); star-relay via host is allowed: semantics below do not change
Lockstep::Session      owns: package_stamps[8], local_package_stamp, FIFO[8] (cap ≥ 20, history 3), TurnClock,
                       DropState, ChecksumState, Recorder
  submit(bytes)        append verbs to the local package (≤ 512)          [UI thread]
  pump()               transport -> accept package iff stamp == last+1    [any time]
  step()               -> Advance | Stalled(slot) | Desync(report) | Ended; at most one sim tick per call
Sim contract           execute_package(play, bytes);  step_frame();  checksums() -> u32[16];  snapshot() / restore()
```

Algorithm (must stay observably identical to §1.3): per tick, `turn_counter++`; at `turn_length`: compute 16 sums;
queue sums + TURN_DATA + CAMERA into the local package; broadcast stamp *s*; for slot 0..7 in the expected-sender set,
block until stamp *s−1* is present; execute in slot order; compare `all`; recompute `turn_length` from the
executed TURN_DATAs; then step frames. Paused → keep counting turns, skip `step_frame`. **The only wall-clock use is deciding when
a tick happens**; ticks never change which commands run. Keep the original package bytes (verbs of replay-spec §9, group
state `last_*_received`) so recorded games and `rcxdump` stay usable; XOR/padding on a live wire is unnecessary (strip it, keep it
in the `.rcx` reader). Record each peer's executed packages as `.rcx` segments (same layout, stamps from 1, embedded save).

### 8.2 Decisions
1. Keep the **constant** ping (200 → 72 after masking) so `turn_length` matches the corpus (6 at speed 2 for 2–5 humans). An
   adaptive scheme is a port extension and must travel inside TURN_DATA, never be derived locally.
2. Dropped player: agree on the dropped slot's **last executed stamp** through the host (or lowest live slot) and only then
   apply the `'P'` command at a boundary; do not skip a pending slot locally as the original does (§3.5).
3. Transport must be reliable-ordered; treat stamp gaps as a protocol error (original drops silently, then deadlocks).
4. OOS: keep the aggregate compare + per-component report; threshold 0; on mismatch freeze, send a dsync notice,
   offer reload from the authority's snapshot (needs the save-port); until `leaders`-only divergence (§5.4) is understood, a
   failed post-load run is not by itself a port bug.
5. Observers: decide explicitly (their packages must be either not sent or drained); never let a FIFO overflow end a game.
6. Cross-play with original clients is out of scope; lobby = port-defined key/value set mirroring §2.1.

### 8.3 Determinism risks the current engine must close (cf. [sim-implementation.md](sim-implementation.md))
- **Iteration order**: players, leaders, objects, groups by index only; no pointer- or hash-ordered containers in the sim;
  `qsort`/`std::sort` ties (original: MSVC `qsort`, insertion sorts in `queues.cpp`/`normalize`) need stable explicit comparators.
- **Floating point**: the original is x86 SSE2 single precision, no FMA. On arm64 compile `engine/sim` with
  `-ffp-contract=off` globally (today only some files use `#pragma clang fp contract(off)`), no fast-math, no libm
  transcendentals (the original uses its own `sin_table`/`cos` tables), and mirror x86 `cvttss2si` for out-of-range / NaN
  float→int (returns 0x80000000; arm64 `fcvtzs` saturates / returns 0) wherever a float is truncated in the sim.
- **RNG**: one global `game_random` seeded from `info.seed` (replay-spec §12); UI/sound RNGs must never touch it; draw sites
  must be exhaustive (the RNG is not in any checksum, so a missed draw shows up only through its effects).
- **Peer-local state**: nothing derived from the camera, window, console `play`, audio, locale or wall time may reach the sim or
  the 15 sums (the original's `leaders` divergences hint at exactly this kind of leak, §5.4).
- **Uninitialised memory** and `memcmp`-style hashing of padded structs: hash fields, not bytes (state-spec §6 already does).
- **Package order and re-entrancy**: execute in slot order 0..7 inside the turn boundary, before the frame step; single sim thread.

### 8.4 Acceptance criteria (no original needed)
1. **Timing oracle**: from a corpus replay's stamps + PAUSE/speed verbs alone, `TurnClock` must reproduce the `frame` column of
   every record (e.g. 84,007 gaps all 6; boundaries 1, 7, 13…; repeated frames during pauses; mid-game segments start at
   header.frame + 1). No wall clock input.
2. **Loopback lockstep**: two (and four, using 4-human files) `Session`s over `LoopbackTransport` with random delivery delay,
   each fed only its own human's recorded packages, advance in lockstep; their 16-sum vectors are equal to each other at every
   turn; staged equality with the recorded `all` as sim components land (first `rules`, already reproducible; then the rest:
   oracle-corpus.md). Replay the 14 fresh MP files, then the 45 save-embedded ones.
3. **Stall**: withholding one package freezes both instances at the same boundary (no frame advance, no divergence) and resumes
   when delivered; stamp gap / duplicate is rejected.
4. **Resign**: the 6 corpus RESIGN endings: leaver leaves the expected set, survivors continue to match.
5. **Drop** (no corpus oracle): kill a peer at a random turn and at a random point of its broadcast; survivors must apply the same
   drop at the same boundary and keep equal sums, for each of the 4 options.
6. **OOS**: flip one bit of one instance's state; both report OOS within one turn (`all` or dsync); recovery from the authority
   snapshot resumes with equal sums and stamps restarting at 1.
7. **Cross-platform soak**: the same replay on x86-64 and arm64 gives identical sums turn by turn (floating-point criteria §8.3).

## Evidence

| Item | Address / source | Used for |
|---|---|---|
| `CommandManager::process_turn / send_local_package / process_command_package_data / init / start / recover_from_oos` | 93ef10 / 940120 / 93fcd0 / 943310 / 942e10 / 93e9e0 | §1.3, §2, §5.2 |
| `CommandPackage::send / end_process / process_check_sums / process_turn_data / process_resign / process_quit / process_ungraceful_player_drop` | 94c1e0 / 94c800 / 9459d0 / 943d20 / 9438c0 / 9439a0 / 943ea0 | framing, §5, §3 |
| `PackageFifo::add / get_local / resend / init` | 952880 / 9528d0 / 952380 / 952590 | FIFO and history |
| `TurnControl::do_frame_multi / compute_next_turn_multi / check_new_frame_multi / init / toggle_pause / start_cannon_time / check_cannon_time` | 955440 / 955880 / 9560c0 / 957e20 / 957ab0 / 956500 / 9579e0 | §1.3–§1.5 |
| `DropControl::*` (process_time_outs / process_need_syncs / process_drops / process_drop / on_player_timed_out…) | 959e40 / 959ab0 / 9598b0 / 959500 / 959110 | §3 |
| `NetDaemon::process / process_all / send_drop_vote / send_drop_decision / process_chat / process_ping` | 950f30 / 951300 / 950510 / 9505c0 / 950740 / 950be0 | §1.2, §3, §7 |
| `SyncPoint::sync`, `TimeSync::sync / update / analyze_response`, `SyncFile::send / receive / sync` | 93a2d0, 955060 / 954cf0 / 954b90, 9396e0 / 939cc0 / 939fe0 | barrier, clock, transfers |
| `SetupWin::exec / setup_game / countdown / validate_start`, `ConnectionData::send_game / send_player` | 5c67f0 / 5b3860 / 5bc400 / 5bac20, 94e200 / 94ec40 | §2 |
| `Game::run / run_gamespy / run_oos_recovery / do_frame`, `Player::drop / resign / quit / leave_game / is_connection` | 584590 / 587060 / 5869e0 / 591ef0, 6ede80 / 6edcb0 / 6edc00 / 6ee010 / 6edb90 | §2, §3, §5.2 |
| `RecordGame::init_record / check_mp_playback / set_mp_playback` | 9537a0 / 943220 / 93ee70 | §6 |
| `CrossplayNetLibSys::send / send_all / handle_network_message / send_pulse / check_pulse / send_dsync / process_dsync`, `CrossplayNetLibPlayer::get_ping_time` | `CrossplayNetLib.dll` (PDB in `sbl/`) @100177f0 / 10017910 / 10019ae0 / 10017db0 / 1001a2e0 / 10017cf0 / 10019f70 / 10027710 | transport, constant ping |
| Types `NetMsgType`, `NetMsg_*`, `MPDropOptions`, `PlayerFlags`, `DropPacket`, `GameConnectionData(Full)`, `NetSys`, vtables | `rise.pdb`, `CrossplayNetLib.pdb` | §1.2, §3 |
| Strings: `loc 0x16a8 0x17e8 0x1824 0xf0f0 0x56f4 0x13358 0x11a58…` | `re/tools/locstrings.mjs` | popup / status texts |
| Corpus statistics (59 MP replays) | `rcxdump --json`; this session's scripts | gaps = 6, ping = 72, 45 saves, 48 mismatches |

## Open questions (❓)

1. Default `time_out` and pulse period of `CrossplayNetLibSys` (set by the lobby layer).
2. Whether `send_all` loops back in `ICrossPlayService` (inferred from usage, 🟡) and the `play < 0` relay branch of `NetDaemon::process`.
3. Whether observers broadcast packages (FIFO overflow hazard, §2.2); lobby rule that makes a slot an observer.
4. Which popup button maps to OOS branch A / B (§5.2).
5. Cause of the post-load `leaders`-only divergence (§5.4).
6. In-game effect of host migration (`host_player` change): vote arbitration, `TimeSync` reference, SyncFile source.
7. Whether the drop-resync race (§3.5) can actually produce different packages per peer.
8. Who evaluates DROPVOTE (host test hidden by the decompile) and what happens when several peers send DROPDECISION after the 90 s timeout.
9. `GameInfo.flags` bit names (only 0x20 observer chat, 0x40 unlimited pauses, 0x80 defeated see all, 0x800 matchmaking are inferred from use).
10. `Version`/handshake details inside `ICrossPlayService` (build gate), and lobby ranked / Elo flow (out of scope for the sim).
11. MP playback feature (live replay sharing) not traced beyond `send_local_package`.
12. The 15 per-component sums as a function of the engine state: see state-spec §6 / oracle-corpus (not multiplayer-specific).
