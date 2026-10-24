# Multiplayer: lockstep session, start, drops, desyncs, saves (roadmap phase 8)

Status: ✅ lockstep layer, replay reader and tests built (section 8) · ✅ decompiled from `riseofnations.exe` (+ `CrossplayNetLib.dll`, PDB applied) and checked against the 59 multiplayer
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
(`max_observers`; rules.xml has the category `maxobservers` 0..6; the port: §8.6.1) 🟡. Facts: they are never waited for (§1.3.4) and never become drop-pending; their chat/pings go
out-of-band (§7). ❓ whether an observer broadcasts packages at all: `send_local_package` has no observer test, receivers
accept them, but nobody drains an observer FIFO, which would overflow after 20 turns; no observer in the corpus. (The port: observers send no package, nobody queues theirs.)

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
4. **Port:** built, see §8.5.1 (authority vote, chunked snapshot, ready barrier, stamps from 1, new recording segment).

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
- **Port:** the native-save variants of all four are built (§8.5): the OOS recovery chain with one recording segment per recovery, a live-session
  recorder in the original's layout, a synchronized save at an agreed turn and a loaded game that the host hands to the re-joining peers.
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

**CHAT_SET (69, 9 bytes `u8 status[8]`; decompile `CommandPackage::process_chat_set @9458e0`, `CommandManager::issue_chat_set @941490`).** `process_chat_set` copies the 8 bytes into the *sender's* leader
(`LeaderData +0x54 + 4 i`); `issue_chat_set(i, v)` rebuilds the sender's row, sets `status[i] = v` and sends it. `process_chat @9454f0` (sender S, recipient mask `bits`) walks the valid leaders L: for a broadcast
(`bits == -1`) the line reaches L only if `L.status[S] == 0` (or `semaphore & 0x40`), for a mask it reaches the leaders whose bit is set and **ignores** the status. So `status[i] != 0` means "L does not want to hear
leader i's broadcast chat" (an ignore list), and team / whispered chat is the CHAT verb's mask. The rows are package-synchronized state (the same on every peer).

## 8. The port: what was built (roadmap phase 8)

✅ built and checked against the corpus as stated in §8.4. **Second pass (2026-10-24, §8.5): OOS recovery from the authority's snapshot, `.rcx` recording
of a live session, the Multiplayer lobby and in-game session in `ronmap`, synchronized saves with re-joining.** **Third pass (2026-10-24, §8.6): the lobby's seat rules (colours, handicaps,
observers, guests' choices with the host's approval, kick, chat targets, countdown), scenario games and Conquer the World battles in multiplayer, the LAN browser, chat in a game, `-ffp-contract=off`.** Files: `engine/sim/replay.{hpp,cpp}` (the `.rcx` reader and the package codec),
`engine/net/` (the lockstep layer and its transports), `apps/net/` (`ron-net`, the tests; `rcxcompare.mjs` checks the reader against
`rcxdump.mjs`). The layer has no dependency on the engine's `Game`: it knows the verb table (`sim/replay.hpp`) and talks to the simulation
through `SimHooks`.

### 8.1 Architecture

| File | What |
|---|---|
| `engine/sim/replay.{hpp,cpp}` | `read_replay`: gzip or plain `.rcx` -> header (`GameInfo`, 8 players, setup, semaphore, `save_name`), the embedded save (offset, size, frame, its own `GameInfo`), the random-game info, the rules block (located, not decoded), the package records with the multiplayer de-XOR and padding (replay-spec §7-§8), the stream located after a save by the same scan as `rcxdump.mjs`. The verb table (82 verbs: names, lengths), `replay_command_length`, `replay_to_engine` / `engine_to_wire` (the engine's `Command` for the 36 verbs it has one for, raw bytes for the rest), `wire_check_sums` / `wire_turn_data` / `wire_camera` / `wire_pause` / `wire_resign` / `wire_drop`, `replay_obfuscate` / `replay_decode_package`. `ron-net survey --codec` checks the codec on the whole corpus: 104,206 engine commands re-encode byte for byte (2 LEADER_OPTIONS differ in the BitMask's middle word, 0 in all the others, which `Command` does not keep), and the XOR + padding of 172,278 multiplayer packages undo themselves |
| `engine/net/lockstep.{hpp,cpp}` | `Transport` (abstract: `peers`, `send`, `broadcast(msg, include_self)`, `poll`, `take_timeout`, `now_ms`), `LoopbackHub` / `LoopbackTransport` (N endpoints in one process; deterministic per-link delay, `hold`, `kill`, `kill_after_sends` = death in the middle of a broadcast), `SimHooks`, `Session` |
| `engine/net/tcp.{hpp,cpp}` | `TcpTransport`: plain sockets, a star (host = slot 0, clients get 1..n, the host relays client-to-client traffic; per-sender order is kept). Connection loss -> `take_timeout` (the host also tells the other clients) |
| `engine/net/lobby.{hpp,cpp}` | `Lobby`: the host's key/value store mirrored to the peers (SET), ready flags, START (the whole table + the seed the host picked, key `game_seed`), a rendezvous barrier (`SyncPoint`). Game messages that arrive early are given back to the transport (`push_front`) |
| `engine/net/game_link.{hpp,cpp}` | `GameHooks` (`SimHooks` over `sim::Game`: runs `CommandPackage::process`, `do_frame`, the 15 sums), `ExecPolicy`, `setup_to_lobby` / `lobby_to_setup` (the keys of §2.1 for `RandomGameSetup`) |
| `apps/net/main.cpp` ... | `ron-net survey | timing | replay | selftest | host | join | loopback` |
| `engine/net/recovery.cpp` | the OOS recovery of `Session` (§8.5.1): authority vote, snapshot transfer, epoch gate, restart |
| `engine/net/record.{hpp,cpp}` | `Recorder` / `RecordInfo`: the live-session `.rcx` writer (§8.5.2) |
| `engine/net/seat_transport.{hpp,cpp}`, `engine/net/discovery.{hpp,cpp}` | third pass (§8.6): the machine <-> game slot mapping under `Session`, the LAN announcements and browser |
| `apps/net/{node,loopback_cmd,native_replay}.*` | `ron-net loopback`, `replay` of recordings of this port, `host` / `join --recover --record` |
| `apps/map-mac/lobby.{h,mm}` | `mp::Room`, `mp::Runtime`, `mp::CommandSink`: the lobby network side and the in-game session of `ronmap` (§8.5.3) |

`Session` (one per peer): `submit(cmds)` queues verbs into the local package (<= 512 bytes, room kept for the automatic ones), `start()` sends stamp 1,
`step()` runs **at most one simulation tick** and returns `Advanced | Stalled(slot) | Desync | Ended | Error`; `pump()` is part of it.
Callbacks: `on_boundary` (the 16 sums of each boundary, before its packages run), `on_package` (every executed package: this is where a `.rcx`
segment writer would hook in), `on_waiting`, `on_drop`, `on_resign`. The only wall clock is `Transport::now_ms` for the late-package timeout
(`SessionConfig::timeout_ms`, 0 = only the transport's notices decide) and the caller's loop, which decides when a tick happens.

**The tick, as implemented** (observably the original's, §1.3):
1. `turn_counter++`. If it reached `turn_length`: boundary.
2. Boundary: zero the per-turn sums; `SimHooks::checksums` (15 parts + `all`) over the state **before** any package runs; append CHECK_SUMS to the
   local package; `on_boundary`; append TURN_DATA (`ping_time` = 72, the constant of §1.4, bit 7 when `game_over`) and CAMERA; stamp++; broadcast
   stamp *s* (`send_all`: the sender receives its own copy through the transport, as the original's).
3. Gate (see "differences"): apply the drops whose agreed final stamp is behind this boundary, then wait until every expected sender has a package queued.
4. Execute stamp *s*-1 of every expected sender in slot order 0..7 (`begin_package`, each command, `end_package`). The lockstep verbs are handled by the
   layer and also given to the sim: CHECK_SUMS (`sums[play] = all`), TURN_DATA (`last_ping[play]`, bit 7 -> `view_all`), PAUSE (state 1/0 sets the flag), SPEED_SET/UP/DOWN,
   CANNON_TIME (speed 0 for 0x4a frames, `can_cannon_time` hook), RESIGN / QUIT (flag 0x40, the sender leaves the expected set), UNGRACEFUL_PLAYER_DROP.
5. Compare: all non-zero `all` equal and no peer sent DSYNC; else `fail_count++` against `checksum_failure_threshold` (0: the first is fatal) -> `Desync`.
6. `turn_length = ceil(clamp(max(2 x max ping of the TURN_DATA executed, min_turn_time), <= 1000) / timings[speed])`, 1 if there was none;
   `min_turn_time = clamp(80 x humans, 400, 600)`. `turn_counter = 0`.
7. Unless paused: `step_frame`. Pause keeps counting turns and stamps (frames stand still, stamps go on).

**Messages** (first byte): 7 package `u32 stamp, i8 play, i16 size, data` (commands back to back, **no XOR, no padding** on the wire: the `.rcx` reader strips
them), 29 DROP_FLAG, 13 DROP_VOTE, 14 DROP_DECISION, 0x87 DSYNC (`i32 stamp`), and the port's 0x91 DROP_STATE `(slot, last accepted stamp)`, 0x92 DROP_NEED
`(slot, stamp)`; 0xA0-0xA3 the lobby.

**Differences from the original, on purpose:**
- The late-package wait finishes *before the first package of the boundary runs* (the original executes in slot order while waiting). Without
  drops it changes nothing observable; with drops it is what lets every peer apply the drop at the same point (before any package of the boundary).
- Package acceptance: `stamp == last + 1`; an older stamp is a harmless duplicate (counted), a gap is a protocol error (`Error`), a full FIFO (20) is an error.
- A DSYNC from any peer stops this peer at once (the original notices at its next `end_process`), so all peers report within one turn.
- Observers (`kPlayerObserver`) send no package, are never waited for, and execute everybody's (tested with 2 observers).

### 8.2 Decisions (all taken as proposed; status)

1. ✅ Constant ping: `ping_time` 72 -> `turn_length` 6 at speed 2 for 2-5 humans (9 for 8 humans, 8 for 6, by formula). Speed changes (`SPEED_*`, cannon time)
   change `timings[speed]` before the next `turn_length`, identically on every peer. 🟡 `SPEED_*` and `CANNON_TIME` do not occur in the corpus; the clamps (0..4) and the
   cannon-time end test are from the decompile, checked only in `ron-net selftest`.
2. ✅ Dropped player (`Session::report_timeout`, called by the transport's notices): every peer flags the slot (DROP_FLAG); when all remaining peers have flagged it, each
   freezes the slot (direct packages from it are ignored, relays accepted), broadcasts DROP_STATE (its last accepted stamp of the slot) and DROP_VOTE
   (`set_drop_preference`, default REPLACE_WITH_AI); once everybody's state and vote are in, the arbiter (lowest remaining slot) sends DROP_DECISION
   `(slot, option, final_stamp = max of the states)`. Whoever has packages another peer lacks sends them (DROP_STATE round, plus DROP_NEED when a peer is still short
   at the boundary: the dying peer's last broadcast reached only some peers). The drop is applied at the start of the boundary that would execute stamp
   `final_stamp + 1`: the clear-text `'P'` command `[0x50, slot, option]` goes through the same path as a recorded one (`lockstep_verb` + `SimHooks::execute`).
   REPLACE_WITH_AI clears HUMAN|DROPPED (the slot stays valid, the AI plays it), the other options set DROPPED. Votes: unanimous -> that option, else the plurality with
   REPLACE_WITH_AI breaking ties; there is no 90 s timer because every peer votes automatically. Engine side (`GameHooks`, `ExecPolicy::player_exit`): REPLACE_WITH_AI clears
   `leaderflag::kHuman`; the other options and RESIGN / QUIT call `leader_defeat(who, 7 or 6)`. 🟡 DIPLOMACY / SURVIVAL do not reset teams and diplomacy yet; co-op seats sharing a leader are not modelled.
3. ✅ A stamp gap is `Error`; duplicates are ignored (resends rely on it).
4. ✅ OOS: detection and the per-component report (`DesyncReport`: the 16 sums of every sender, the first of the 15 parts that differs; DSYNC broadcast) and, with
   `SessionConfig::recover`, the recovery of §8.5.1 (authority snapshot through the transport, every peer reloads, stamps restart at 1). Without `recover` a desync still freezes
   the session (`StepKind::Desync`), which is what `ron-net replay` and the old tests use.
5. ✅ Observers: they send nothing, nobody waits for them, they execute every package; a FIFO never fills for them (packages of non-expected slots are not queued).
6. ✅ Lobby = port-defined key/value set mirroring §2.1 (`map_style map_size game_seed gamerules gamespeed pop_limit tech_cost rush_rules reveal_map victory elimination
   score_goal time_limit wonderwin popwin econwin starting_town starting_resources starting_technology ending_technology randomize_starts no_tribe_powers num_nations`, per slot
   `slot_type_n tribe_n team_n diff_n color_n`; `scenario`, `humans` for a scenario game). No cross-play with original clients.

### 8.3 Determinism risks the current engine must close (cf. [sim-implementation.md](sim-implementation.md))
- **Iteration order**: players, leaders, objects, groups by index only; no pointer- or hash-ordered containers in the sim;
  `qsort`/`std::sort` ties (original: MSVC `qsort`, insertion sorts in `queues.cpp`/`normalize`) need stable explicit comparators.
- **Floating point**: the original is x86 SSE2 single precision, no FMA. On arm64 compile the engine with
  `-ffp-contract=off` globally (**`build-mac.sh` passes it since the third pass: §8.6.8**; before it only some files used `#pragma clang fp contract(off)` and clang's default is `on`), no fast-math, no libm
  transcendentals (the original uses its own `sin_table`/`cos` tables), and mirror x86 `cvttss2si` for out-of-range / NaN
  float->int (returns 0x80000000; arm64 `fcvtzs` saturates / returns 0) wherever a float is truncated in the sim.
- **RNG**: one global `game_random` seeded from `info.seed` (replay-spec §12); UI/sound RNGs must never touch it; draw sites
  must be exhaustive (the RNG is not in any checksum, so a missed draw shows up only through its effects).
- **Peer-local state**: nothing derived from the camera, window, console `play`, audio, locale or wall time may reach the sim or
  the 15 sums (the original's `leaders` divergences hint at exactly this kind of leak, §5.4).
- **Uninitialised memory** and `memcmp`-style hashing of padded structs: hash fields, not bytes (state-spec §6 already does).
- **Package order and re-entrancy**: execute in slot order 0..7 inside the turn boundary, before the frame step; single sim thread.
Evidence so far: several engine instances in one process (so any shared mutable static would show) stay byte-identical for 15,334 turns of the 45 save-based replays, with
delivery delays up to 25 ms; two processes over TCP end 600 / 1500 / 6000 frames of random games (and a scenario) with identical native saves. Not run: x86-64 against arm64.

### 8.4 Acceptance criteria and results (no original needed)
1. ✅ **Timing oracle** (`ron-net timing "re/oracle/Recorded Games/Multi"`): the `Session` over loopback, fed only each player's stamps, PAUSE and RESIGN (everything else is
   ignored, the sums and TURN_DATA are its own), reproduces the `frame` column of **all 172,276 packages of the 59 files** (86,144 turns; 84,007 gaps of 6, boundaries at header.frame + 1, 7, 13..., the repeated
   frames of every pause, the RESIGN endings), 0 differences, also with random delivery delays up to 40 ms. The 14 files that start from scratch are covered too (the sim is a frame counter there).
2. ✅ **Loopback lockstep** (`ron-net replay <file>`): 2 engine instances (`--observers K` adds instances that only watch), each fed only its own player's packages through the layer, the sums computed by each
   instance's `EngineProvider` at every boundary. On the **45 save-based replays: the instances agree on all 15 parts at every turn (15,334 turns) and end in identical states, 45/45**, at delivery delays 0..3 ms and 0..25 ms.
   The fidelity numbers (first turn each part differs from the replay's CHECK_SUMS) are **identical to `ron-sav --replay` for 45/45 files** (the output lines are `diff`-equal; the aircraft verbs
   ATTACK_GROUND / PATROL / LAUNCH_PATROL / FLIGHT / RECALL / SCRAMBLE are left out by default exactly as `ron-sav` leaves them out, `--all-verbs` runs them). The 14 files without an embedded save need the
   random-map path from the header setup plus the replay's own rules block and are only covered by criterion 1.
3. ✅ **Stall** (`ron-net selftest`): withholding one peer's packages freezes the others at the same boundary (no frame, no divergence) and they resume when it is delivered; a duplicate stamp is ignored, a gap is `Error`.
4. ✅ **Resign**: the 6 RESIGN endings of the corpus run through criterion 1 (the leaver leaves the expected set); `selftest` keeps playing with the survivors: equal sums, nothing queues up for the leaver.
5. ✅ **Drop**: for each of the 4 options, random victim, random turn and a broadcast cut at a random link (`kill_after_sends`), 3-4 peers: the survivors apply the same drop at the same boundary and keep equal sums
   (30/30 per option in 10 seeds). On real sockets: three `ron-net` processes, one killed with `kill -9`: the two survivors apply REPLACE_WITH_AI at the same turn and end 20,000 frames with identical saves.
6. ✅ **OOS**: flipping one bit of one instance makes every peer report within 2 turns (threshold 0) / `threshold + 2` (tested with 3); the simulation stays frozen without `recover`.
   With `recover`: 7 toy cases (2-4 peers, divergent peer = guest / host / the majority's minority, 300 KB snapshots, an observer), two recoveries in a row and two authority deaths
   (mid-transfer, before sending) all end with equal sums; two engine instances repair a divergence injected at frame 150 and end with identical native saves (§8.5.1).
7. ❓ **Cross-platform soak** (x86-64 against arm64): not run; what could be done on one machine is in §8.6.8 (`-ffp-contract=off`, checked not to move any digest).

`ron-net host <port> --random STYLE,SIZE,SEED,NATIONS [--humans N] [--frames N] [--save F]` / `ron-net join <port>`: the host's lobby fixes the setup and the seed, every process builds the same
`new_random_game`, a small bot plays each seat (so there are commands), the game stops at frame N and the processes write their native save: two processes, 600 frames, 100 turns, 80 commands:
about 1.8 MB each, byte-identical. `--scenario FILE --seed S` starts the same scenario instead (tested with the first tutorial).

### 8.5 Second pass: recovery, recording, lobby and in-game session, saves (2026-10-24)

Files added: `engine/net/recovery.cpp` (the recovery half of `Session`), `engine/net/record.{hpp,cpp}` (the `.rcx` writer), `apps/net/{node,loopback_cmd,native_replay}.*` and
`apps/net/demobot.hpp` (test harness), `apps/map-mac/lobby.{h,mm}` (the room and the runtime of `ronmap`), the lobby screens of `frontend.mm`. Changed: `lockstep.*` (recovery, epoch
gate, synchronized save, pause / speed callbacks, DSYNC no longer freezes at once), `game_link.*` (restore, frame wrapper, `game_over`), `tcp.*` (lobby mode, host names), `lobby.*`
(names, ready flags in the table, chat, snapshot for late joiners, file transfer), `sim/replay.*` (the native embedded save). `engine/sim` otherwise untouched.

#### 8.5.1 OOS recovery (`SessionConfig::recover`)

1. **Freeze on the same row.** A peer acts on another peer's DSYNC only at the end of the boundary that reaches its stamp (`end_turn`, as the original's `end_process`), not
   at once. Every peer therefore freezes on the *same* row of check sums (the packages it executed carry all of them), after the packages and before the frame step.
2. **Authority** (`Session::pick_authority`, a pure function of that row): the lowest slot when at most two senders took part (the host), else the sender whose `all` sum agrees with
   the most others, lowest slot on a tie. No message is needed to agree on it.
3. **Transfer** (`SyncFile`-like): the authority takes `SimHooks::snapshot` (the native save of the game: `GameHooks::snapshot` = `save_game_bytes`), zlib-compresses it and sends
   `RecBegin` (epoch, raw / compressed size, CRC-32 of the raw bytes, chunk count and size, frame) and `RecData` chunks (16,000 bytes) to every other peer, observers included.
   A peer assembles, inflates, verifies the CRC and calls `SimHooks::restore` (`load_game_bytes`; the application swaps its `Game`). **The authority reloads its own snapshot too**,
   so all instances restart from the same bytes (the live state of a game is not guaranteed to equal what its save rebuilds).
4. **Epoch gate.** After its restore a peer broadcasts `RecReady(epoch)` (also to itself). Everything a peer sent before its RecReady belongs to the old epoch (per-sender order):
   packages, DSYNC and the drop agreement are dropped while this peer recovers and has not seen that sender's RecReady. When every peer's RecReady is in, `Session` restarts:
   stamps from 1 (`local_stamp`, `last_accepted`, FIFOs, `hist`), `turn_length` from its initial value, `fail_count` 0, drop controls reset (a peer that died meanwhile is given up on
   again), `step()` returns `Recovered` once (callers reset what they count per stamp: `Session::epoch()` changes), the frame goes on. Pause state, speed, flags and `boundaries()` are kept.
5. **The authority dies in transit.** A peer that lacks the snapshot broadcasts `RecNeed`; any peer that already loaded it answers (answered as soon as it has loaded it when it asks early).
   Only if nobody answers within 300 ms of the transport clock does the next candidate of the same report take a fresh snapshot (nobody holds the old one then).
6. **Messages** 0xB0 `RecBegin`, 0xB1 `RecData`, 0xB2 `RecReady`, 0xB3 `RecNeed`; callbacks `on_desync(report, authority)`, `on_recover(snapshot, frame, epoch)`.

Differences from the original: the snapshot is the authority's state at the freeze row (the original saves its game when `process_turn` reaches `recover_from_oos`, same place), a CRC-checked
chunk stream instead of NETMSG 17-21, no popup (`StepKind::Recovering` / `Recovered` and the callbacks let the front end say what is happening), and branch B of the popup ("continue
alone") is not offered. The original diverges again soon after loading its own saves (§5.4); the port's loop round-trips bit for bit (`ron-net selftest`: the instances agree after 80+ turns).

#### 8.5.2 Recording a live session (`net::Recorder`)

`Session::on_package` (frame, slot, stamp, wire commands) feeds `Recorder::package`. The file is the original's layout (`replay-spec.md` §1-§8): the header written by `RecordGame`
(`Game` 0x16, `GameInfo` 0x42 with the version string `(Version: 00.2024.06.2000)`, seed, checksum window 256, flags 0x121, the 30 setup bytes from the `RandomGameSetup`, 8 `Player` records
with the 0x39-byte raw part, `Game[0x550,0x6e4)` with frame / tick / player count, semaphore `0x0c` = multiplayer + recording, `save_name`), then the embedded save when the segment starts from one,
the random-game info (`team`, tribe per seat, `world+30` = map style) and, without a save, the exact rules block (`sim::write_image` of the game's rules, 1,024,221 bytes), then one record per
package: `i32 frame, i32 play, i32 valid (1), u32 stamp, i16 size`, the payload with the MP XOR and the 0-1 padding bytes per command from the seed (`replay_obfuscate`); the clear-text `'P'` drop
package (stamp 0) is written in the clear as `DropControl::process_drops` does. The file is plain while the game runs and gzipped when it closes (a crash leaves the plain file with a short
last record, which both readers report as truncated).

Port extensions inside that layout: (a) the embedded save of a native game is `String "RoNNativeSave"`, `u32` save version, `u32` length, the native save bytes (the original's saves carry no
length; `read_replay` takes the exact end, `rcxdump.mjs` scans for it as for any save and finds the same stream: `rcxcompare.mjs` reports identical packages, commands, fingerprints on the
recordings); (b) the header's `scenario_path` of a random game holds the setup text (`random_setup_string`), the slot a native save uses, so `ron-net replay` rebuilds the game without lobby
data. A recovery closes the segment and opens `<name>_2.rcx` with the snapshot embedded (`header.frame` = the frame of the snapshot), the chain of `docs/oracle-corpus.md` (each segment
starts at stamp 1). Segment 1 ends at the freeze row, exactly as 45 of the corpus' 46 segments end at their first mismatching turn.

`ron-net replay <file.rcx>` recognises these files (embedded `RoNNativeSave`, or a random setup in the header): one engine instance per human, each fed only its own player's packages, the instances must
agree at every turn, and `--save F` writes the final native save. Test: record a loopback game, replay each recording, compare final native saves byte for byte (§8.5.5).

#### 8.5.3 The Multiplayer lobby and the in-game session of `ronmap`

`mp::Room` (a `TcpTransport` in lobby mode: the host listens, guests are accepted whenever the transport is pumped and take the lowest free slot; `Lobby` mirrors names, ready flags and the
setup to every peer and sends the whole table to a peer that arrives late; chat). The Multiplayer screens (`ui-port.md` §10.3) set the game up; `Lobby::start(seed)` carries the host's seed; each
machine builds the same random game (`new_random_game`), joins a barrier (`SyncPoint`), and `mp::Runtime` builds `GameHooks` + `Session` (recovery on, 20 s silence = a drop).
`mp::CommandSink` replaces the local `CommandManager`: `process_turn` hands what `issue_*` queued to the session as one batch (GROUP + verb stay together) instead of executing it; commands run
one turn later, on every machine, in slot order. Pause (`P`, the bar button), speed (`-` / `=`: SPEED_DOWN / UP) and Resign (game menu) are lockstep commands; the HUD's pause and speed words follow the
session. Pause / resume / speed / resign / drop / out-of-sync / recovered / save messages come from the session callbacks and show as game messages; a late peer shows "Waiting for X..." and a recovery
"repairing it from another player's game...". A game that ends on a connection error returns to the menu after 5 s. Every game is recorded to `~/Library/Application Support/OpenRiseOfNations/
Recorded Games/Multi/<date>.rcx`.

Headless (`ronmap`): `--host PORT --humans N --random STYLE,SIZE,SEED,NATIONS [...]` and `--join HOST:PORT`, with `--frames N --snapshot f.png --save-game f`; the game runs to the first turn
boundary at or after frame N (or the end of the game). Test hooks: `--mp-bot` (a demo player), `--mp-record FILE`, `--mp-diverge FRAME` (corrupt this machine's state), `--mp-act pause|resume|faster|slower|resign|save@TURN,...`,
`--mp-save FILE` (where the synchronized save goes), `--mp-load FILE` (host: continue a saved game).

#### 8.5.4 Synchronized save and re-joining (spec §6, "if time allows")

`Session::request_save()` broadcasts `kSaveAt (0x93, boundary)`; every peer snapshots its state at the *start* of that boundary (identical on all peers) and `on_save` writes its own file: the copies are
byte-identical (tested). A saved game of a random map continues from the lobby: the host picks the file (**Load Saved Game...**, or `--mp-load`), the table carries `from_save`, the host sends the file
through `Lobby::send_file` (zlib chunks, CRC) before START, every machine builds the saved game's map from its setup text and loads the save (`load_game_bytes`), stamps start at 1 and the recording begins with the save
embedded. The connected peers must sit in exactly the seats that were human in the save (slots are assigned lowest-free, so rejoining in the same order gives the same seats); the lobby says so when they do not.

#### 8.5.5 Results (scratch build with the `build-mac.sh` flags, zero warnings)

- `ron-net selftest`: **38 / 38** (23 old + 7 recovery cases, two recoveries in one game, 2 authority deaths, the engine part: 2 instances with a divergence injected at frame 150 repaired and identical,
  segment chain written by both, last segment of each replays to the live final native save byte for byte, the second segment starts where the first stopped; 3 instances without recovery, the
  recording of one replays to the final state); seeds 1..12 pass the toy part. `ron-net replay` on the 45 save-based corpus replays: **45 / 45** instances agree at every turn and end identical (the
  other 14 have no embedded save, `ron-net timing` covers them). `ron-check` 0 failures; `sim-all` unchanged.
- Real sockets (`ron-net host / join --recover --record`): three processes, one corrupted at frame 150: majority vote picks slot 0 (slots 0 and 2 agree), all three end with identical native saves.
- `ronmap` LAN, two processes: `--host` / `--join` 300 frames with demo players: identical saves, the host's recording replays to the same save; with a divergence at frame 150: recovered at frame 157, identical
  saves, both segments replay; three processes with pause / resume / faster / resign: identical saves; a synchronized save at turn 29: identical files; the host's file continued by two processes to frame 405:
  identical saves and the recording (embedded save) replays to the same save.
- Snapshots read: `re/previews/mp_connect.png`, `mp_lobby.png`, `mp_game_3p_messages.png`, `mp_game_recovered.png`.

#### 8.5.6 Left (after the second pass; the third pass, 8.6, did the lobby choices, scenario and campaign games, the LAN browser and chat in a game)

NAT traversal, the x86-64 / arm64 soak (§8.4.7), per-peer `Player` colours in the recording header, the original-client interoperability (none by design), a recording of a game that *joined* a running one (none: the room closes at START), and the
diplomacy / team reset of the DIPLOMACY and SURVIVAL drops.

### 8.6 Third pass: lobby rules, scenario and campaign games, LAN browser, chat, floating point (2026-10-24)

Files added: `engine/net/{seat_transport,discovery}.{hpp,cpp}`. Changed: `net/lobby.*` (seat plan, requests, kick, countdown, chat targets), `net/tcp.*` (`disconnect`, `reopen_door`), `net/game_link.*` (`MpSeats`, `apply_mp_seats`,
the CHAT fix of 8.6.7), `net/record.*` (the handicap byte of the header), `sim/replay.*` (CHAT_SET codec), `apps/net/*` (loopback with observers / handicaps / scenarios / chat test, `native_replay` reads the header's seats, `selftest` 38 -> 91),
`apps/map-mac/{lobby,frontend}.{h,mm}` and small hunks of `main.mm`, `build-mac.sh` (one flag). `engine/sim` otherwise untouched.

Facts the original's data gave (rules.xml categories): `handicaps` = 21 entries "Standard", "Skill +1" ... "Skill +20" with DATA 0, 5 ... 100 (`LeaderData::get_handicap`, the gather / territory handicap, read only when `semaphore & 4` = multiplayer);
`maxobservers` = 0..6; `playerslots` = Open, Closed, None, Computer (+ Rush and Raid / Boom and Build), Human, **Boot Player**, Human / Computer Control All, **Block Player**, Unblock Player (the kick and block of the host).
`SetupWin::countdown` ticks "n seconds" in the lobby chat before START (10 s).

#### 8.6.1 Seats: game slots are not connection numbers

The Session addresses peers by game slot (= `Player.play` = package order). In a room the machines connect in any order and some only watch, so the lobby's `SeatPlan` (`make_seat_plan`) maps them:
players take the game slots 0.. in the order of their connection numbers (nobody missing: slot = connection number, as before), seats above them up to `num_nations` are computer players, **observers take the slots
from `nations` up** (spec 2.2 "slots above the player count"; `nations + observers <= 8`, otherwise `overflow` and the host cannot start). A saved game (`identity`) keeps every player on its connection number (they must sit where they
sat), a Conquer the World battle forces the seats of the sides (`force_N`). `Lobby::start(seed)` fixes the plan in the table (`seat_N` = the game slot of machine N, `nations_final`) so every machine reads the same one.
`net::SeatTransport` wraps the raw transport and renames peers <-> game slots in both directions (`send`, `poll`, timeouts, `local_peer`, `peers`), so `Session` is unchanged; lobby messages (0xA0..0xAF) that arrive during the game
are kept (an observer's out-of-band chat, a finishing machine's barrier) and given back at the end. Observers: `kPlayerObserver`, no package, never waited for, they run everybody's packages (tested: 2 observers, drops, recovery).

#### 8.6.2 Lobby rules (`Lobby`, `mp::Room`, the lobby screen)

- **Per seat:** nation, team 1-4, **colour** (`NationSetup.color` = the team colour slot 0..7: red, blue, pink, green, yellow, cyan, white, orange; default = the seat number; unique among the seats), **handicap**
  (`handicap_N`, index into the 21 entries), difficulty for computer seats. The host edits every seat (a nation or colour another seat holds changes places with it).
- **Guests choose their own seat** (`Lobby::request(SeatField, value)`): nation, team, colour, handicap, watch-only. The host validates against the table (`Lobby::check`): `kInvalid` (outside the rules' lists), `kTaken` (a nation or colour
  another seat holds), `kNoRoom` (observer limit `max_observers`, the last player cannot leave the game, `nations + observers > 8`), `kLocked` (a scenario fixes nation / team / colour, a saved game fixes everything), `kDenied`.
  Approval mode **Auto** applies a valid request at once and tells the application (`take_request`: the lobby screen mirrors the three seat fields into its `RandomGameSetup`); **Ask** queues it (`pending()`) until the host presses
  Allow / Deny (`answer`). The answer reaches the guest as a notice; the table is the truth, so a refused combo snaps back.
- **Observers:** the host's "Observers" combo (0..6, default 2) is the limit, a guest (or the host) presses **Watch only** / **Play instead**; the plan moves the machine above the nations. An observer's game: fog off, "Observing: ..." line, orders refused
  (`Runtime::submit_wire`), chat out of band (`Runtime::say`: lobby-layer message, shown as "Name (observer): ...").
- **Kick / block:** the host's **Kick** under a guest's name (`Lobby::kick` sends KICK, `TcpTransport::disconnect` ends the connection: the guest is told "The host removed you", the others see "X was removed"); `Room::kick(peer, block)` also refuses
  the name when it comes back (API; the screen has Kick only). Names are made unique by the host ("Marcus (2)").
- **Chat** (`Lobby::say_to`): to everybody, **`/t text` to the team** (machines whose seat has the same team; observers talk to observers), **`/w name text` private**; lines are drawn with the sender's name in the seat's colour, system lines in yellow
  (join, leave, removal, request outcomes, the countdown); joins are announced once the name is known.
- **Countdown:** Start begins a 10 s countdown (`Room::begin_countdown`), one "The game starts in n seconds..." line per second for everybody; the button turns into Cancel; a guest leaving or un-readying stops it. (`RON_MP_NO_COUNTDOWN`, the headless
  paths and the demo start skip it.)
- After a game the same room is a lobby again (`Room::leave_game`: `started`, barriers, ready flags reset; the door reopens).

#### 8.6.3 The game is a multiplayer game

`net::apply_mp_seats(Game&, who_of, MpSeats)` (every peer, on its fresh Game, before the barrier; the table at START is the same everywhere): `Game::multiplayer = true` (the semaphore bit the original's lobby games have: the lobby handicap
becomes `LeaderData::get_gather_handicap` / the territory handicap, the AI uses its own difficulty, the multiplayer capture rules of `capture.cpp`, the speed lock flag), the human seats' `Leader::name` = the lobby name (so a CHAT shows the
player, not "Player"), `Leader::handicap`. Colours are `Leader::color` already. A game continued from a save carries all of it. Recordings carry the names and the handicap byte of the header; `ron-net replay` of a recording without an embedded
save rebuilds the game and applies the header's seats.

#### 8.6.4 Scenario games

The host's **Scenario...** picks a `.scx` (`mp::load_scenario_info`: the scenario's players, the human-flagged ones first, so game slot k plays leader `who[k]`; table keys `scenario_game`, `scenario`, `scen_*`). The seats of the lobby follow the
scenario's players (nation, colour, difficulty are the scenario's and locked; handicap, observers and approval work), `num_nations` = the number of players (a room with more players than seats must let some watch). START carries the seed; every
machine calls `MapRenderer scenarioMultiplayerWithDevice:` = `Game::load(files, scenario, rules, seed)` + `script_host_load` with `ScenarioPlayer::is_human` set from the plan (the AI of each seat is created from that flag, so it must be set before the load,
identically everywhere). `Runtime::set_who` maps game slots to leaders (`GameHooks`' `seat_who`, `local_play`, the demo bot). The start state is embedded in the recording as a native save (a scenario cannot be rebuilt from a header);
recovery and saves work (`load_game_bytes` reloads the scenario). Headless: `ronmap GAMEDIR --host PORT --humans N --scenario FILE.scx ...` / `--join`; `ron-net host --scenario` and `ron-net loopback --scenario` exist too.

#### 8.6.5 Conquer the World battles

The host's campaign plays the battle (`ctw_host_start`, `ctw_battle_session`, `ctw_host_result`, `ctw_guest_result` in `lobby.mm`; `ctw_battle.cpp` untouched): in the lobby the host presses **Conquer the World...**, which opens the campaign screens with the room kept;
**Play** on the map hands the prepared `BattleDef` to the room instead of to a single machine. Seats: the host sits on the campaign player's side, the guests take the other seats (the host's opponents first), the remaining seats are computer players,
observers watch (`force_N` in the plan). Every machine gets (1) the `BattleDef` as a blob in the table (`encode_battle` / `decode_battle`: every field `build_battle` reads, the setup as its text form; `make_battle` uses the campaign's RNG for some styles, so a
guest must not re-derive it) and (2) the campaign (`ConquestGame::save_bytes`) as the lobby file, loads it as its copy (`ctwui::load_session`) and builds the same Game (`build_battle`, `new_random_game` / `Game::load` of the custom map). The Game of a battle cannot be rebuilt
from a save (`"ctw:battle"`), so these games have **no out-of-sync repair and no recording** (a desync ends the game with the error). When the battle is decided the host's side's outcome (1 won, 0 lost, 2 retreated) is applied with `Session::battle_finished`
and the new campaign is sent to the guests (`ctw_done` = the battle's sequence number), who replace their copy (`frontEndCtwReceived`); the host returns to the map with the room kept (the next battle can be fought by the same players), the guests to the lobby.
Headless: `ronmap GAMEDIR --ctw-mp --ctw-battle CAMPAIGN:LINK --host PORT --humans N --frames F --mp-bot` / `--ctw-mp --join HOST:PORT ...`: the host prints the campaign's hash after the outcome, the guest the hash of the copy it received: equal.

#### 8.6.6 LAN browser (`net/discovery.*`)

A host's room announces itself once a second as a UDP datagram `"RONL" u8 protocol u16 tcp-port u8 players u8 seats u8 flags (1 scenario, 2 saved game, 4 campaign) str name str map` to the limited broadcast, every interface's subnet broadcast,
the multicast group 239.255.77.78 and the loopback, port 7778 (`RON_LAN_PORT` overrides it for tests, `RON_LAN_TARGET` adds a unicast target). The connect screen binds the same port (`SO_REUSEADDR` + `SO_REUSEPORT`, joins the group on every
interface), lists the games with host name, map, players / seats and address (one row per source address and port, a host on this machine heard on two addresses shows once, entries expire 4.5 s after the last datagram), a game of another
protocol value is grey ("another version of the game"). A click selects (fills the address field), a double click or Return joins. Announcing stops when the game starts. No third-party library.

#### 8.6.7 Chat in a game

- A line a player types in the HUD's chat box is the CHAT verb through the `CommandSink` (the HUD pass); an observer's line goes out of band; CHAT_SET (69) is the ignore list (`/ignore NAME`, `/hear NAME` in the chat box; codec `sim::wire_chat_set`,
  `Runtime` keeps the table from the executed packages: identical on every machine). A received line shows "Name (Nation): text" with the sender's name and `Leader::color` (the same code as single player, now fed by 8.6.3).
- **A real desync this pass found:** the engine pushes the presentation event of a CHAT (saved state, so part of the SCRP check sum) only when the line is for the local leader. With a recipient mask (team chat, whispers) the machines of the players outside the mask
  differed from the others: an out-of-sync report on the first team line. `GameHooks::execute` now processes a CHAT as if the local leader were its sender (always in its own mask), so every machine queues the same event, and the application shows it only to
  the recipients (`main.mm` `mpFilteredPresentation`: the mask is read from the console's chat list; a broadcast is hidden from a player who ignores the speaker). Test: `ron-net loopback --chat-test` (3 players + an observer: no desync, identical saves);
  `ronmap --mp-act chat=TEXT@TURN,team=TEXT@TURN,ignore=LEADER@TURN`: a screenshot of the player who ignores Bob and is outside his team shows only its own line.

#### 8.6.8 Floating point (arm64 against x86-64)

The simulation uses single-precision floats in many places (grep: attrition `anti_att`, combat flight, aircraft bank / pitch, cities, construction progress, AI ratios ...). On arm64 clang's default `-ffp-contract=on` fuses `a * b + c`; the original is x86 SSE2 without FMA, and an Intel
Mac would not fuse, so an Apple Silicon and an Intel `ronmap` could disagree in lockstep. `build-mac.sh` now passes **`-ffp-contract=off`** for every target. It changes nothing on this machine, checked on one tree built twice: `sim-all 600 --ai-every 8` final digests
**156 / 156 identical** (every stock scenario, `ron-sim --frames 600 --ai-every 8`: 141 run the 600 frames, 15 end at frame 1 by their own victory conditions, as before; the outputs of the two builds `diff` to 0 lines), the 105 `ron-sav --engine-roundtrip` runs over `re/oracle` (69 PASS, 36 FAIL / empty in this tree, **the same lines** with and
without the flag), `ron-net replay` on the corpus: same output; so the flag is kept. (Whether the arm64 and x86-64 builds then agree is the soak of 8.4.7: not run.)

#### 8.6.9 Results (scratch build with the `build-mac.sh` flags, zero warnings in every target)

- `ron-net selftest`: **91 / 91** (38 before). New: the seat plan (4 cases), the lobby rules over loopback (22: free / taken / invalid / default colours, teams, handicaps, Ask approval refused and allowed, observer limit, plan with a watcher, scenario and saved-game locks,
  START fixes the plan, the room after a game, team and whisper chat targets, countdown, kick), the seat transport (2 layouts x 3: players and an observer agree on every turn although game slots differ from machine numbers, the observer ran the commands, a dead machine is
  dropped under its game slot at the same boundary), the drop with an observer in the room (8 / 8), a room over real sockets (join order, requests, whisper, kick, START), discovery (wire format, foreign datagrams, a browser hears the host over UDP, update, expiry), CHAT_SET,
  the engine: 3 players + 1 observer with handicaps and a chat to a mask (identical saves, handicap in the state), scenario games (italy_mp 2 players, Tutorial 1 2 players + 1 observer).
- LAN, real processes: `ronmap` 2 players (identical saves); 2 players + 1 observer whose connection number is *between* the players' (the observer got game slot 3, the second guest slot 1: all three saves identical, one asked for colour / team / nation / handicap);
  a scenario game (`italy_mp`, 8 seats) identical; a Conquer the World battle (`alexander:auto`, 2 players) identical saves and equal campaign hashes; chat with names and colours; `ron-net host` / `join`: 3 processes (handicaps `0,4,2`) 900 frames and a scenario game
  (2 processes, Tutorial 1, 600 frames): identical saves.
- `ron-check` 0 failures; `sim-all` digests and the 105 round trips as in 8.6.8; `ron-net replay` on the corpus: 45 / 45 instances agree, final states identical.
- Snapshots read: `re/previews/mp3_lobby.png` (two guests with their choices, an observer, Kick), `mp3_lobby_ask.png` (a request waiting for the host, Allow / Deny), `mp3_lobby_scenario.png` (a scenario's seats, locked fields), `mp3_connect.png` (the LAN list),
  `mp3_chat_alice.png` / `mp3_chat_bob.png` (names in colour, the filter), `mp3_game_observer.png`.

#### 8.6.10 Left

NAT traversal and internet play (LAN only); the x86-64 / arm64 soak; original-client interoperability (none by design); recording and out-of-sync repair of a Conquer the World battle (its Game cannot be rebuilt from a save), a CTW battle with the campaign's AI nations
as co-op partners of several guests (the guests fill the opposing seats first); the host as the room's only observer-less player is not forced (a host may watch); an observer cannot yet switch the seat it looks from; Block has no screen button; the lobby rules have no
"Closed" / "Open" seat types (the number of nations does that); `DIPLOMACY` / `SURVIVAL` drops still do not reset teams; ranked / matchmaking rules.

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
| The port: reader, lockstep, tests | `engine/sim/replay.*`, `engine/net/*`, `apps/net/*` (`ron-net`) | section 8 |

## Open questions (❓)

1. Default `time_out` and pulse period of `CrossplayNetLibSys` (set by the lobby layer).
2. Whether `send_all` loops back in `ICrossPlayService` (inferred from usage, 🟡) and the `play < 0` relay branch of `NetDaemon::process`.
3. Whether observers broadcast packages (FIFO overflow hazard, §2.2); lobby rule that makes a slot an observer. (The port: they send none and sit above the nations, §8.6.1; the original's rule is `maxobservers` + the "slots above the player count".)
4. Which popup button maps to OOS branch A / B (§5.2).
5. Cause of the post-load `leaders`-only divergence (§5.4).
6. In-game effect of host migration (`host_player` change): vote arbitration, `TimeSync` reference, SyncFile source.
7. Whether the drop-resync race (§3.5) can actually produce different packages per peer.
8. Who evaluates DROPVOTE (host test hidden by the decompile) and what happens when several peers send DROPDECISION after the 90 s timeout.
9. `GameInfo.flags` bit names (only 0x20 observer chat, 0x40 unlimited pauses, 0x80 defeated see all, 0x800 matchmaking are inferred from use).
10. `Version`/handshake details inside `ICrossPlayService` (build gate), and lobby ranked / Elo flow (out of scope for the sim).
11. MP playback feature (live replay sharing) not traced beyond `send_local_package`.
12. The 15 per-component sums as a function of the engine state: see state-spec §6 / oracle-corpus (not multiplayer-specific).
13. (port) The pause quota: whether `Player.pauses` counts used or remaining pauses, and whether a second PAUSE(1) while paused counts. The layer accepts every recorded pause (`max_pauses` 0 = unlimited) and sets the flag from `state`.
14. (port, answered 2026-10-24) The OOS recovery path and `.rcx` recording of a live session are built (§8.5). Still open: whether the original's recovery save is the
    authority's state at the freeze row (the port's) or one frame later; the per-peer chat in a running game (CHAT verb 68 executes, nothing in `ronmap` sends it).
