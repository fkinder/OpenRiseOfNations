// The victory modes the lobby / scenario setup offers and the rules around them (docs/victory-spec.md).
//
// capture.cpp owns Game::check_victory, Leader::victory / defeat and GameDaemon::process_victory (conquest, wonders,
// territory, score, time limit; docs/capture-spec.md section 7). This module adds what was missing there: Game.on_team /
// Game::init_teams / Game.num_sides, the economic and musical-chairs victories, the tech-race victory, the allied
// victory of Leader::set_diplo, Player::resign / leave_game, the World Government defeats, Leader::reset_score and the
// Armageddon counter. None of it adds saved state (on_team is derived from GameInfo.team and the leaders' VALID flag).
#pragma once

#include <array>
#include <cstdint>

namespace ron::sim {

class Game;
struct Leader;

// GameEvent kinds written by the victory code (the tool log; never read by the simulation, see game.hpp):
// 17 wonder-victory clock started (who = the first leader of the winning side, value = frames until victory,
//    other = its wonder_net), 18 wonder-victory warning (value = frames left: 4500 / 1800 / 900), 19 wonder clock stopped
//    (who), 20 territory clock started (who, value = frames until victory, other = the popwin percent), 21 territory
//    warning (value = frames left: 4500 / 1800 / 900), 22 territory clock stopped (who), 23 time-limit warning
//    (value = ticks left: 120 / 300 / 600, other = 1 under game_rules 8), 24 musical chairs round (value = the round
//    length in minutes, other = the number of leaders defeated by it), 25 resigned (who, value 0 resign / 1 quit /
//    2 drop), 26 Armageddon: a nuke detonated (value = the new count), kind 4 (game over) value 1 = the allied victory of
//    Leader::set_diplo (the end-game screen is pending; the simulation keeps running as in the original).
namespace victoryevent {
constexpr int32_t kWonderStart = 17, kWonderWarning = 18, kWonderStop = 19, kPopStart = 20, kPopWarning = 21, kPopStop = 22,
                  kTimeWarning = 23, kChairsRound = 24, kResigned = 25, kNukeCount = 26;
}

// ---------------------------------------------------------------- teams (Game::init_teams @58ae70)

struct TeamCounts {
	std::array<int32_t, 8> on_team{};  // Game.on_team: seats per team 0..3 (the walked field is not loaded from saves)
	int32_t num_teams = 0;             // teams with members
	int32_t loose = 0;                 // seats on no team (0..3 excluded; team 5 = "random" is resolved by the lobby)
	int32_t num_sides() const { return num_teams + loose; }
};
TeamCounts game_team_counts(const Game& g);
// The counting half of Game::init_teams: Game.num_sides = num_teams + loose. (The team-5 random assignment, the ELO
// bookkeeping and the log lines are lobby code, victory-spec 3.)
void game_init_teams(Game& g);
// Game::init_rules_and_teams @589bb0, single player: num_players = num_nations = the seats (GameInfo.players + 1).
void game_init_players(Game& g);

int32_t leader_get_team(const Game& g, int32_t who);  // LeaderData::get_team @6ec040 (8 without a seat)

// ---------------------------------------------------------------- scores

int32_t leader_get_economic(const Game& g, int32_t who);       // LeaderData::get_economic @6d6490
int32_t leader_get_team_economic(const Game& g, int32_t who);  // LeaderData::get_team_economic @6d6400
int32_t leader_get_team_score(const Game& g, int32_t who);     // LeaderData::get_team_score @6d6520
// Leader::reset_score @6e37f0 (Leader::init and Setup::build_game): every score term and combat statistic to 0, the age
// stamps to -1 (an age nobody reached yet).
void leader_reset_score(Leader& l);

// ---------------------------------------------------------------- the modes

void victory_process_chairs(Game& g);    // GameDaemon::process_victory, musical chairs (victory 5)
void victory_process_economic(Game& g);  // GameDaemon::process_victory, economic victory (victory 8)
// Leader::gain_tech steps 32 / 33: tech-race victory (victory 9) after an age / an epoch tech.
void leader_tech_race_age(Game& g, int32_t who);
void leader_tech_race_epoch(Game& g, int32_t who);
// The Armageddon counter: Nuke::do_damage @92bc80 adds one the frame a nuclear explosion starts (not under starting
// resources 8, not in a game with a single nation). Called by the nuke effect when it is ported (victory-spec 8).
void armageddon_nuke_detonated(Game& g);

// LeaderData::holds_capital @6d54c0 / has_capital: with elimination CAPITAL, whether `who`'s capital is held by `who`
// (`*owner` = the holder); other elimination modes always answer true with `*owner = who`.
bool leader_has_capital(const Game& g, int32_t who, int32_t* owner);
// Leader::gain_tech step 19 second half (GLOBAL_GOVERNMENT_BONUS): leaders whose capital sits with `who` or an ally of
// `who` are defeated (type 1, attacker `who`, flag 1).
void leader_global_government_defeats(Game& g, int32_t who);

// Leader::set_diplo @6ec6a0 tail: the last alliance that leaves nobody outside gives `who` the (allied) victory and sets
// the end-game flag (semaphore bit 0x16 = byte2 & 0x40 = Game::victory_checked).
void leader_allied_victory(Game& g, int32_t who);

// ---------------------------------------------------------------- Player::resign / leave_game

// Player::leave_game(why) @6ee010 for the leader of a seat: 6 resigned, 7 dropped. A leader whose capital clock runs is
// defeated by capital (type 1) instead.
void player_leave_game(Game& g, int32_t who, int32_t why);
// Player::resign(quit) @6edcb0 (CommandPackage::process_resign, Player::quit) and Player::drop @6ede80.
void player_resign(Game& g, int32_t who, bool quit);
void player_drop(Game& g, int32_t who);

// Game::is_victory_timer @594950 (the HUD's victory-clock predicate): standard victory and some VALID|ACTIVE leader's wonder or
// territory clock is running.
bool game_is_victory_timer(const Game& g);

// True once the game is over for the front end: Game::game_over (the simulation stopped) or the end-game flag
// (semaphore byte2 & 0x40) the allied victory raises without stopping it.
bool game_ended(const Game& g);

}  // namespace ron::sim
