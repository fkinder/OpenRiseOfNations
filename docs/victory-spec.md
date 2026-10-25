# Victory conditions, teams, resignation (decompile of `riseofnations.exe`, 2026-10-24)

Scope: everything the lobby / scenario setup offers as a way to end a game, and what each one reads. `docs/capture-spec.md`
section 7 already documents `Game::check_victory`, `Leader::victory` / `defeat` and the conquest / wonder / territory / score /
time-limit part of `GameDaemon::process_victory`; this spec adds what was missing there and records the corrections the second
decompile pass made. Port: `engine/sim/victory.{hpp,cpp}` plus hooks (section 11). Decompiles: `re/decomp/victory/`; argument files
`re/jobs/victory*.txt`. Nothing here draws `game_random` during play (the sounds use `SoundGlobal::random`); the only draws are the
lobby's team assignment (section 3.4). Marks: ✅ decompile read and ported, 🟡 ported with a port decision, ❓ open.

## 1. State, flags, tables

| Original | Port | Meaning |
|---|---|---|
| `Game.semaphore` byte0 `& 4` | `Game::multiplayer` | multiplayer game |
| byte0 `& 0x40` (bit 6) | `Game::game_over` | `Game::check_victory` ended the game; `TurnControl` stops stepping, `Game::do_frame` returns |
| byte0 `& 0x80` (bit 7) | (none) | team-score game: `get_team_score` averages the team. **No writer in the executable** (`InsnGrep` on every store to the byte), so the port returns the leader's own score |
| byte1 `& 2` | (not modelled) | balance mode, the developers' `UnitBalance` harness (section 9) |
| byte1 `& 8` | `Game::editor` | editor / free mode |
| byte1 `& 0x10` | `Game::scripted_rules` | scripted rules (`game_rules` 10) |
| byte2 `& 2` | `Game::ctw` | Conquer the World battle |
| **byte2 `& 0x40` = `BitMask<256>` bit 0x16** | `Game::victory_checked` | **end-game screen pending**: set by `check_victory` (end) and by the allied victory of `Leader::set_diplo`; `Game::do_frame` @591ef0 calls `Game::process_end_game` @591ce0 at the end of the frame, which shows `EndGameWin` and clears the bit again. The port has no window: the front end reads it and clears it (`game_ended()` is `game_over || victory_checked`) |
| `Leader.flags` `0x20` / `0x40` / `0x80` | `kVictory` / `kDefeated` / `kGameOver` | won / lost / "the game is over for me" |
| `Leader.flags2` `0x100` / `0x200` | `kVictoryFlag` / `kDefeatFlag` | the `flag` argument of `victory` / `defeat` (1: by World Government) |

Victory types (`Leader::victory(type, flag)`, `LeaderData +0x7d8`): 0 conquest (also the allied victory), 1 wonders, 2 territory,
3 **tech race**, 4 score, 5 economic, 6 time limit. Defeat types (`+0x7dc`): 0 conquest, 1 capital, 2 capital sudden death,
3 sudden death, 4 musical chairs, 5 another leader's victory, 6 resigned, 7 disconnected, 8 Armageddon (9: the scripts' `defeat()`).
`GameInfo.victory` modes (`ScenarioFuncSet::is_victory_*` @9e5250..9e52e0): 0 standard (wonder and territory clocks + conquest), 1 sudden death
(the wonder / territory leader wins at once, no clock), 2 conquest (no extra win), 3 score, 4 time limit, 5 musical chairs, 6 wonders (instant),
7 territory (instant), 8 economic, 9 tech race.
Lists read: `wonderwins`, `popwins`, `scores`, `time_limits`, `chairs`, `econwins`, `map_sizes` (entry 3), constants `wonder_timer`
(+0xd08), `popwin_timer` (+0xd10), `retake_capital` (+0xd04), `timer_refresh_ratio` (+0xd00), `armageddon*` (+0xd14..0xd1c).

## 2. `Game::check_victory` @5926b0 ✅ — corrections to capture-spec 7.1

Already ported as capture-spec 7.1 describes. Details this pass confirmed:
- humans are counted through the player seats (`player[p].flags & 1`, `who == leader`, `flags & 4`), the port counts `kHuman` leaders
  (seats are one per leader);
- the early-out loop compares **both** diplomacy directions with the first live leader (`diplos[first] == 2` and the reverse);
- the end sets semaphore byte0 `0x40` first, then (Armageddon not reached) `Leader::victory(first live, 0, 0)` unless a live
  leader is neither the first, nor already victorious, nor allied with it, and finishes with byte2 `0x40` (bit 0x16) whether or
  not a winner was named. GameSpy room bookkeeping afterwards is online-service code, not ported.
- with every human out and `humans != 0` the game ends **without a winner** (a single-player game whose player was defeated).

`Game::get_armageddon` @594020: `armageddon_per_team * num_sides + armageddon_per_nation * num_nations + armageddon`, x3 for starting
resources 7, x2 for 5 / 6 / 11 / 12, at least 100 for 8. `Game::defeat_all` @592aa0: see section 8.

## 3. Teams: `Game.on_team`, `Game::init_teams` @58ae70, `num_sides`, `num_nations`

### 3.1 `Game::init_rules_and_teams` @589bb0 (the part that matters here)
Order: `init_random_colors`, the rules defaults (`GameInfo::default_setting` for fields 9..29 unless `game_rules` is non-zero; a team style
of 4 / 7 / 10 sets `wonderwin` 6, style 5 sets 7, 6 sets 8, others 5), a random starting technology (`starting_technology == 9`: one
`game_random` draw `% (ending_technology + 1)`), `Map::set_map`, `init_starting_resources`, `Leaders::init`, `LeaderOptions::init`, then
the seats. **Single player: `num_players = num_nations = GameInfo.players + 1`** (one per seat; a scenario's seats are its player
chunks), each seat's leader gets flags `VALID | ACTIVE` (+ `HUMAN` for the human, + `COOP_SOLO` with the info flag 0x10). Multiplayer
(`semaphore byte0 & 4`): the counts are rebuilt from the lobby's player slots (`num_nations` counts distinct leaders, `num_players`
the slots). Leaders 8 and 9 (Gaia and the extra slots) get `flags |= 7`. Then `init_teams`, `init_handicaps`, `init_tribes`.
🟡 The port sets `GameInfo.num_players` / `num_nations` in `Game::load` from the scenario's leader chunks, before the leaders are
initialised (`game_init_players`). Before this pass they stayed at the default 2: this changed the price scaling of research
(`TypeData::get_cost`), the `Game::get_armageddon` limit, the "contest" test of the wonder and territory clocks (a single-nation
scenario can no longer be won by territory) and the AI's distance scaling in every scenario with a seat count other than 2.
❓ whether the scenario launcher passes `players = seats - 1` exactly (random-map saves do: `current 2.SVX` has 5 leaders,
`players` 4); no oracle save comes from a stock scenario.

### 3.2 `Game::init_teams`
First loop: for each VALID leader, the seat (`GameInfo.player[p]` with `flags & 1`, `who == leader`, not an observer / dropped
seat `flags & 0x50 == 0`) is examined: with `team_style == 3` outside multiplayer `team = (p != console.play)` (a "me against the
rest" style); team **5** (random) is counted later; team `0..3` increments `on_team[team]` (and tracks the largest team);
anything else is a side of its own (`loose`, local_58). Then
`num_teams` = teams with members, **`num_sides = num_teams + loose`** (@58b4d6), `on_team[0..7]` stays the walked field (saves keep
it at the game start; the port derives it from `GameInfo.team` and the VALID flags, `game_team_counts`: no saved state, no
`kSaveVersion` bump).
### 3.3 `Game.on_team` consumers
Musical chairs (`on_team[0..7]`, section 5.6) and nothing else; `num_sides` feeds `get_armageddon` only. `LeaderData::get_team` @6ec040:
the seat's `team` (8 when the leader has no seat; with semaphore byte0 bit 7 and team 8 the index of the first VALID leader that
`is_team`, never reached). `LeaderData::get_player` @6ec0f0 is the seat index (the last non-observer seat for the leader, else the
last seat found). `is_team` @6ebd30: same leader true; team style 7 and either seat on team 8 false; **after frame 0 (and `p == 0`)
it answers `is_ally`**; at frame 0 equal teams 0..3; with `p != 0` and style 0 / 8 / 11 also `is_ally`. (`is_team_p` in `ai_diplo.cpp`
is that function.) `num_team_members(p)` @6ebf90 counts the leader plus VALID leaders with `is_team(p)` (only ACTIVE ones for `p != 0`).
### 3.4 Random team assignment (not ported)
Seats with team 5 are assigned in the lobby only: multiplayer draws one `Random::get(0, 0xffff)` per seat (key `handicap * 100 +
draw % 500`) and sorts the seats by it; each team-5 seat then draws `% number_of_teams` (or, once the teams are full, `draw & 3`) until
a team below the size bound (`(num_nations + 1) / 2`, 2 / 3 / 4 for team styles 4 / 5 / 6) comes up. The port's lobby (net agents)
hands over finished teams; `game_team_counts` ignores team 5. The final loop of `init_teams` also rewrites `Leader +0x54[k]` (the
"met" treaty bits) to 1 for invalid `k` and for non-team `k` in team games and 0 otherwise; the port's `leader_init_diplomacy`
already produces the single-player values.

## 4. Score ✅ (`Leader::compute_score` @6ec560, `reset_score` @6e37f0)

`compute_score(p)`: runs when `frame == 0`, `p != 0`, the game is over (byte0 `0x40`), or `(who + frame) % 10 == 0`: `explored = 0`;
units (if flags `0x1800000`: `compute_unit_score` @6bc500, which clears `0x800000`), buildings + wonders (@6bc3f0), economy (@6bc270),
`pop = 0`, and with `NEW_TECH` (0x1000000) upgrades (@6bc1a0) + research (@6bc360) then clear it; `territory = land share * 1000 / World.land_size`;
under Armageddon every term and the total is zero; else `score = combat + wonders + research + upgrades + economy + buildings + units
+ territory + pop + explored`. `compute_combat_score` @6bc250, `compute_wonder_score` @6bc260 are empty, `compute_pop_score` @6bc190 and
`compute_explore_score` @6bc5e0 store 0. The port (`ai_diplo.cpp`) matches every term. `Leaders::strategy_all` @6ed430 calls it for each
VALID|ACTIVE leader each frame, then `Leader::diplomacy`.
`reset_score` (called by `Leader::init` and `Setup::build_game`): every score term and combat statistic to 0 and **the seven age
stamps (`+0x8f8`) to -1**. The port's `Leader::age_stamp` defaulted to 0, so `leaders_best_human_age_stamp` saw "age reached at frame 0"
for every age nobody had; now `-1` as a default member initialiser plus `leader_reset_score`. This changes the AI's age pacing in
games with a human (digest note in section 12).
`LeaderData::get_team_score` @6d6520 = own score (the team average needs byte0 bit 7); `get_team_economic` @6d6400 = mean of
`get_economic` over the leader and its VALID `is_team(0)` leaders; `get_economic` @6d6490 = `ceil(sum of (income / 16) over the
goods the leader can use, five goods (Knowledge excluded) / 5)`; `get_team_terr` @6d62e0 (ported, `ai_setup.cpp`).

## 5. `GameDaemon::process_victory` @730ef0 (frame step 11.2)

Order: Armageddon (5.1), wonders (5.2), territory (5.3), score (5.4), time limit (5.5), musical chairs (5.6), economic (5.7). Everything
below that is not marked "new" is capture-spec 7.3; the corrections:

### 5.1 Armageddon
`get_armageddon() <= game.armageddon` -> `defeat_all` (every VALID|ACTIVE leader: flags 0x80, `defeat(8, -1, 0)`; every VALID leader
`compute_score(1)`). **Who counts nukes:** `Game +0x6e0` is written only by `Nuke::do_damage` @92bc80 (`INC`), by `Setup::build_game` (0),
`Game::clear` and `Leader::gain_tech` step 20 (`max(0, armageddon - 2)`, ARMAGEDDON_BONUS = missile shield). `Nuke::do_damage` runs every
frame from `Objects::inc_time`: for each live nuclear effect (`nuke_effect`, added by `Nuke::add_nuke` @92ba30 from `Ammo::do_damage` @678060
when a nuke-line warhead lands, with its start frame `t0` and duration `d`), **at the frame where `(frame - 1 - t0) / d < 1 <= (frame - t0) / d`
(i.e. `frame == t0 + d`) and `starting_resources != 8` and (multiplayer or `num_nations > 1`): `armageddon += 1`**; the same
function then applies the blast damage (`Objects::find_units` in the radius, every frame of the effect). **Ported** (`sim/nukes.cpp`, docs/nukes-spec.md:
`d` = `max_nuke_frames` = 110, the blast itself lasts 40 frames; `nuke_do_damage` calls `armageddon_nuke_detonated(g)` at `t0 + 110`, `kind 26` event;
`ron-sim --nuke` and `--victory armageddon` drive it). The nuke line waits in `Object::do_launch` while `armageddon >= get_armageddon() - 2`
(aircraft-spec).
### 5.2 Wonder victory (victory 0 / 1 / 6) — events new
Clock start (victory 0, multiplayer or `num_nations > 1`, no World Government on the winner's team, first leader of the pass): message
"X is building wonders: victory in N" (`wonder_net`, the countdown) + `S_WONDER_VICTORY_START`; **warning** at exactly 4500 / 1800 / 900
frames left unless the console player is an ally: the pair of messages + `S_WONDER_WARNING`; stop: `Achieve` event + `S_WONDER_VICTORY_STOP`
once per pass; victory when `left == 0` exactly (a clock that skips frame 0 would never fire; the stamp arithmetic makes that impossible
while the loop runs every frame). Events: `kWonderStart` (value = frames until victory, other = `wonder_net`), `kWonderWarning` (value = left),
`kWonderStop`.
### 5.3 Territory victory (victory 7 / 0 / 1) — events new
Gate: not balance mode (byte1 `& 2`), multiplayer or `num_nations > 1`. Start message (value = `popwins[popwin]` percent) and the same
4500 / 1800 / 900 warnings (unless the console is on the team); a halted clock prints "territory victory halted" once per pass.
Victory when `left < 1`. Events `kPopStart`, `kPopWarning`, `kPopStop`.
### 5.4 Score victory (victory 3)
Strict `>` from 0 over `get_team_score`, so with nobody above 0 leader 0 stands for "best"; wins when `scores[score_goal] <= best`.
### 5.5 Time limit (victory 4)
`limit = time_limits[time_limit]` minutes (index >= 8: `ScenarioData::custom_time_limit`, **0 -> 300**, only when `semaphore byte1 & 0x10`
or `byte2 & 2`; otherwise **-1, which ends the game at frame 0**: the port keeps its decision of using the custom slot without that test,
🟡 capture-spec 7.3). While `tick < limit * 60` (`tick` = frame / 15): on `frame % 100 == 0` the warnings at `limit * 60 - tick` = 120 (a random
sound of `SoundGlobal`'s own `Random`), 300 and 600, and for `game_rules == 8` the team message that follows; at `tick >= limit * 60`:
`game_rules == 8`: the first VALID|ACTIVE leader on team 0 wins; else the best `get_team_score` wins; type 6. Event `kTimeWarning`.
### 5.6 Musical chairs (victory 5, `frame != 0`) — new ✅
Every `chairs[chairs].data[0] * 900` frames since `game.musical_chairs` (also moved by every `Leader::defeat`): the stamp moves to now and
```
m = max(1, on_team[0..3]);  if on_team[4] != 0 or [5] != 0 or [6] != 0: m = 1
if on_team[7] != 0: m = 1
elif m > 1: for k in 0..3 with on_team[k] != 0: m = min(m, live members of team k)    // VALID|ACTIVE leaders with get_team == k
if team_style in {0, 8, 11} or m < 2:                // free for all
    victim = the VALID|ACTIVE leader with the lowest score (<=: the later of equal scores replaces it, but an
             equal score marks a tie); no victim when the lowest score is shared
else: for team 0..3: the lowest-scored live member of that team, unless tied
defeat(victim, 4, -1, 0)
```
Tie rule exactly: scanning leaders in order, a score `<=` the best so far replaces it and sets `tie = (score == best && it is a different leader)`.
Event `kChairsRound` (value = the round length in minutes, other = leaders defeated by it) plus the usual `Leader::defeat` event.
### 5.7 Economic victory (victory 8) — new ✅
Strict `>` from 0 over `get_team_economic`; victory (type 5) when `econwins[econwin].data[0] <= best` (with nobody above 0 leader 0 is "best").

## 6. Allied victory: `Leader::set_diplo` @6ec6a0 ✅

After the diplomacy of `(self, t)` becomes **allied (2)**: the ally masks (`ALLY_LOS` / `reveal_map`) are set, then the loop counts the
VALID|ACTIVE leaders other than the two that are allied with **neither**; when there are none, `victory(self, 0, 0)` and
**`BitMask<256>::set(semaphore, 0x16)`** (end-game pending, section 1). The simulation keeps running (`game_over` stays clear): the
original shows `EndGameWin` and leaves the game. Port: `leader_allied_victory` (`victory.cpp`), event `kind 4` with `value 1`.
Consequence worth knowing (verified by the fixture): `Leader::victory(self)` makes every **ally of self** run `victory` itself, which
defeats (type 5) everyone that ally is not allied with. The test uses only the two parties' allies, so an incomplete alliance graph
(a chain A-B, B-C) ends with C defeated by A's victory; a clique ends with everyone victorious. 

## 7. Elimination, resignation

- Elimination modes (capture-spec 6.1) are ported. Port notes this pass: a **defeat type is recorded by the first `Leader::defeat` that finishes**:
  `defeat(2 | 3 ...)` starts with `defeat_by`, which hands the loser's other cities over (`capture_city` -> `City::close` -> `lost_a_city`); the
  nested `lost_a_city` of the last of them defeats by conquest (type 0) before the outer call returns. A capital-sudden-death defeat of a leader
  with other cities therefore reads type 0 (a lone capital: 2). The fixture asserts both.
- **`Unit::close` @60ee50** defeats a leader by conquest when its last counted unit dies: `who < 8`, `Leader.active (+0x93c) == 0`,
  `city_num (+0x3f8) == 0`, not the editor (byte1 `& 8`), `flags2 & 1` (NO_CITY_DEFEAT) clear. Inside the counted-type branch, after the
  decoy-guarded counters. Ported in `combat_death.cpp`. A seat that starts with nothing (no unit, no city) is never defeated by itself.
- **Resign**: `CommandManager::issue_resign` @941170 / `issue_quit` @941360 -> `CommandPackage::process_resign` @9438c0 (op 70, `i32 play`) /
  `process_quit` @9439a0 (op 71, `i32 play`, 2 bytes) -> `Player::resign(quit)` @6edcb0: `flags |= 0x40`, messages / sounds
  (`S_YOU_RESIGN` / `S_YOU_QUIT`, `S_ALLY_RESIGNED` / `S_ENEMY_RESIGNED`), then **`Player::leave_game(6)`** @6ee010, and for the local seat
  semaphore byte1 `0x80` ("I resigned", the UI). `Player::drop` @6ede80 (a disconnected player) is the same with `leave_game(7)`.
  `leave_game(why)`: `flags |= 0x10`; multiplayer and another human seat still plays the same leader: return (co-op, not modelled);
  a leader that is not VALID|ACTIVE or whose **capital clock is not running** (`+0x418 == 0`) -> `defeat(why, -1, 0)`; with the clock
  running -> `defeat(1, holder of the capital, 0)` in CAPITAL elimination (`find_capital`) else `defeat(1, self, 0)`. `Player::win` @6ee290 /
  `lose` @6ee250 / `end_game` @6ee110 only mark the seat (flags `0x100` / `0x80`), set the console's game-over time and the byte1 `0x80` UI bit.
  Port: `player_resign` / `player_drop` / `player_leave_game`, `Verb::Resign` / `Verb::Quit` (`CommandManager::issue_resign`, encoded as ops 70 / 71 by
  `engine_to_replay`; `replay_to_engine` does not decode them: `engine/net` handles the exits with its own policy and calls `leader_defeat(6 | 7)`).
- **World Government** (`gain_tech` step 19): after assimilating the researcher's captured cities, every other leader for which
  `!has_capital(p, &owner)` and (`owner == self` or `is_ally(self, owner)`) is defeated `defeat(1, self, 1)`. `LeaderData::has_capital` @6d54c0 is
  always true unless the elimination mode is CAPITAL (`holds_capital` @6d54f0: `find_capital` finds the capital with the leader). Ported
  (`leader_global_government_defeats`).

## 8. Tech race (victory 9) ✅

`Leader::gain_tech` step 32 (an age tech): `victory == 9`, not Conquer the World (byte2 `& 2`) and `ages == ending_technology` -> `victory(3, 0)`
(after the notification block; independent of `notify`). Step 33 (Conquer the World, an epoch tech, `victory == 9`): message, and when the
raw epoch count is `0x6959b` (28 epoch techs) `victory(3, 0)`. `Build::queue_up` @620f40 (a library, `t` an age, `victory == 9`):
**`ages + 1 == ending_technology` and `!Game::war_allowed()`** -> `gain_tech(t, -1, -1, 1, 1)` at once and return 0 (no queue entry, nothing paid).
Ports: `techs.cpp` (steps) and `queues.cpp` (the shortcut).

## 9. Balance mode (semaphore byte1 & 2) — not ported

The byte is set by the `UnitBalance` command-line harness (`Game::balance`, `check_for_timeout`, `write_results`, `next`); it makes
`Leaders::strategy_all` call `check_victory` every frame, `lost_a_city` / `check_victory`'s counting use live units (`+0x93c` / `+0x954`),
the territory clock and `retake_capital` skip, and `TurnControl::toggle_pause` (byte1 `& 6 == 6`) force `victory(0, 0)` for every live leader.
Nothing in the shipped front ends sets it; `ConsoleWin::run_cmd` cheats (`victory`, `defeat`) are the other callers of
`Leader::victory` / `defeat` (script builtins `victory()` / `defeat()` are in `script_funcs_mutate.cpp`).

## 10. RNG

No `game_random` draw in `check_victory`, `Leader::victory` / `defeat`, `process_victory` (the sounds use `SoundGlobal::random`), `get_*_score`,
`Player::resign` / `leave_game`, `set_diplo`. `Leader::defeat` -> `defeat_by` -> `capture_city` and the multiplayer `blow_up_*` keep the draws of
capture-spec 9. `init_teams` (lobby only): section 3.4. `init_rules_and_teams`: one draw for a random starting technology.

## 11. Port map

| Original | Port |
|---|---|
| `Game::check_victory`, `defeat_all`, `Leader::victory` / `defeat` / `defeat_by`, clocks, `process_victory` | `capture.cpp` (events for the clocks added) |
| `Game::init_teams` (counts), `init_rules_and_teams` (players / nations), `get_team`, `get_economic`, `get_team_economic`, `get_team_score`, `reset_score` | `victory.cpp` (`game_team_counts`, `game_init_teams`, `game_init_players`, `leader_get_*`, `leader_reset_score`); `Game::load` |
| musical chairs, economic victory | `victory.cpp` (`victory_process_chairs`, `victory_process_economic`), called at the end of `game_daemon_process_victory` |
| `set_diplo` allied victory | `ai_diplo.cpp` -> `leader_allied_victory` |
| `Unit::close` defeat | `combat_death.cpp` |
| tech race | `techs.cpp` (steps 32 / 33), `queues.cpp` |
| World Government defeats | `techs.cpp` -> `leader_global_government_defeats` |
| `Player::resign` / `leave_game` / `drop`, RESIGN / QUIT | `victory.cpp`, `commands*.cpp` (`Verb::Resign`, `Verb::Quit`, `issue_resign`) |
| Armageddon count | `victory.cpp` `armageddon_nuke_detonated`, called by `nukes.cpp` `nuke_do_damage` 110 frames after a detonation |

GameEvent kinds 17-26 (`game.hpp`, `victory.hpp`) carry the clocks to the HUD; the HUD text is a follow-up. `Game::victory_checked` is the end-game
flag the front end must clear after showing the screen.

## 12. Acceptance: fixtures and digests

`ron-sim <game> <scenario.scx> --victory MODE [--verbose]` with MODE `all`, `teams`, `conquest`, `capital`, `sudden`, `time`, `score`, `wonder`,
`territory`, `economic`, `chairs`, `armageddon`, `allied`, `tech`, `resign`, `reset` (a mode that needs more leaders than the scenario has prints
`skip`). Results and the digest changes are in `docs/sim-implementation.md` "Victory": every mode passes on five scenarios (96-131 checks each);
31 of the 156 stock digests change, all because `num_nations` / `num_sides` now follow the seats; with the two assignments removed the digests equal HEAD's.

## 13. Open

1. ~~Nuclear explosions~~ ported (docs/nukes-spec.md): the Armageddon counter's producer is `nuke_do_damage`.
2. ❓ `GameInfo.players` of a scenario launch (section 3.1).
3. 🟡 The time-limit port decision for the custom slot outside scripted rules (5.5).
4. Co-op seats in `leave_game`, balance mode, GameSpy room state, the ranked-game ELO bookkeeping of `init_teams`: online / developer features.
5. Wiring RESIGN / QUIT from replays and the lockstep layer into `player_resign` (the net policy defeats with `leader_defeat` directly and skips the
   capital-clock rule of `leave_game`).

## 14. Evidence

| Function | Address | File |
|---|---|---|
| Game::check_victory / defeat_all / get_armageddon / is_victory_timer | 5926b0 / 592aa0 / 594020 / 594950 | `re/decomp/victory/` |
| Game::wonder_winning / wonder_timer / popwin_timer | 5948a0 / 5944f0 / 5944b0 | same |
| Game::init_teams / init_rules_and_teams | 58ae70 / 589bb0 | same |
| Game::process_end_game / Game::do_frame | 591ce0 / 591ef0 | same |
| GameDaemon::process_victory | 730ef0 | same |
| Leader::victory / defeat / set_diplo | 6ec9b0 / 6ecb00 / 6ec6a0 | same |
| Leader::compute_score, compute_*_score, reset_score | 6ec560, 6bc190..6bc5e0, 6e37f0 | same |
| LeaderData::get_team / get_player / is_team / num_team_members | 6ec040 / 6ec0f0 / 6ebd30 / 6ebf90 | same |
| LeaderData::get_economic / get_team_economic / get_team_score / get_team_terr | 6d6490 / 6d6400 / 6d6520 / 6d62e0 | same |
| LeaderData::has_capital / holds_capital | 6d54c0 / 6d54f0 | same |
| Player::resign / leave_game / quit / drop / win / lose / end_game | 6edcb0 / 6ee010 / 6edc00 / 6ede80 / 6ee290 / 6ee250 / 6ee110 | same |
| CommandPackage::process_resign / process_quit, CommandManager::issue_resign / issue_quit | 9438c0 / 9439a0 / 941170 / 941360 | same |
| Unit::close (defeat branch) | 60ee50 | `re/decomp/heroes2/` |
| Leader::gain_tech (steps 19, 20, 32, 33) | 6dcb60 | `re/decomp/heroes2/` |
| Build::queue_up (tech-race shortcut) | 620f40 | `re/decomp/aircraft/` |
| Nuke::do_damage / add_nuke | 92bc80 / 92ba30 | `re/decomp/victory/` |
| Leaders::strategy_all, TurnControl::toggle_pause | 6ed430, 957ab0 | `re/decomp/victory/` |
