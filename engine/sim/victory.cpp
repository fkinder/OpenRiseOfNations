// Victory modes, teams and resignation (docs/victory-spec.md).
#include "sim/victory.hpp"

#include <algorithm>

#include "sim/ai.hpp"
#include "sim/capture.hpp"
#include "sim/game.hpp"
#include "sim/production.hpp"
#include "sim/production_internal.hpp"

namespace ron::sim {

namespace {

Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }
const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }
bool valid_active(const Leader& l) { return (l.flags & 3u) == 3u; }  // LEADER_VALID | LEADER_ACTIVE

int32_t list_value(const Game& g, const char* list, int32_t idx) {
	const CategoryList* cl = g.rules->category_list(list);
	if (!cl || idx < 0 || size_t(idx) >= cl->items.size()) return 0;
	return cl->items[size_t(idx)].data[0];
}

// GameInfo.player[].team of a leader's seat. The port keeps the lobby team of the leader (-1: none).
int32_t seat_team(const Game& g, int32_t who) { return g.info.team[size_t(who & 7)]; }

}  // namespace

// ---------------------------------------------------------------- teams

TeamCounts game_team_counts(const Game& g) {
	// Game::init_teams @58ae70, first loop: for every VALID leader the seat that plays it (flags & 1, not an observer or a
	// dropped seat): team 5 (random) waits for the lobby's assignment, 0..3 count into on_team, anything else is a team of
	// one ("loose": num_sides = num_teams + loose).
	TeamCounts c;
	for (int32_t who = 0; who < 8; who++) {
		if (!(LD(g, who).flags & leaderflag::kValid)) continue;
		const int32_t t = seat_team(g, who);
		if (t == 5) continue;
		if (t >= 0 && t < 4) c.on_team[size_t(t)]++;
		else c.loose++;
	}
	for (int32_t k = 0; k < 4; k++)
		if (c.on_team[size_t(k)] != 0) c.num_teams++;
	return c;
}

void game_init_teams(Game& g) { g.info.num_sides = game_team_counts(g).num_sides(); }

void game_init_players(Game& g) {
	// Game::init_rules_and_teams @589bb0 (single player): num_players = num_nations = GameInfo.players + 1, one per seat.
	int32_t n = 0;
	for (int32_t who = 0; who < 8; who++)
		if (LD(g, who).flags & leaderflag::kValid) n++;
	g.info.num_players = n;
	g.info.num_nations = n;
}

int32_t leader_get_team(const Game& g, int32_t who) {
	// LeaderData::get_team @6ec040: the seat's team, 8 without a seat. (Team 8 with the team-game bit semaphore byte0 & 0x80
	// would answer the first team mate's index: the bit has no writer.)
	if (who < 0 || who >= 8 || !(LD(g, who).flags & leaderflag::kValid)) return 8;
	return seat_team(g, who);
}

// ---------------------------------------------------------------- scores

int32_t leader_get_economic(const Game& g, int32_t who) {
	// @6d6490: the mean of the income rates (x16 stored, so /16) of the goods the leader can use, over the five goods other
	// than Knowledge, rounded up.
	int32_t n = 0, sum = 0;
	for (int32_t k = 0; k < 6; k++) {
		if (k == ptype::kKnowledge) continue;
		n++;
		if (type_avail(g, who, k, 1) != 0) sum += LD(g, who).income[size_t(k)] / 16;
	}
	if (n == 0) return 0;
	return (sum - 1 + n) / n;
}

int32_t leader_get_team_economic(const Game& g, int32_t who) {
	// @6d6400: the mean over the leader and its team mates (VALID, is_team(i, 0)).
	int32_t n = 0, sum = 0;
	for (int32_t i = 0; i < 8; i++) {
		if (i == who || ((LD(g, i).flags & leaderflag::kValid) && is_team_p(g, who, i, 0))) {
			n++;
			sum += leader_get_economic(g, i);
		}
	}
	return sum / std::max(1, n);
}

int32_t leader_get_team_score(const Game& g, int32_t who) {
	// @6d6520: semaphore byte0 bit 7 (a team-score game) averages the team; the bit has no writer in the executable, so the
	// leader's own score.
	return LD(g, who).score;
}

void leader_reset_score(Leader& l) {
	// @6e37f0: +0x18..+0x44 the score terms, +0x808..+0x870 / +0x8ec..+0x8f4 / +0x934 / +0x938 the statistics, +0x8f8 the seven
	// age stamps (-1).
	l.score = l.score_explored = l.score_territory = l.score_units = l.score_attack_units = l.score_buildings = 0;
	l.score_economy = l.score_pop = l.score_upgrades = l.score_research = l.score_wonders = l.score_combat = 0;
	l.units_killed = l.units_lost = l.buildings_razed = l.buildings_lost = 0;
	l.units_bribed = 0;
	l.age_stamp.fill(-1);
}

// ---------------------------------------------------------------- musical chairs and economic victory

void victory_process_chairs(Game& g) {
	// GameDaemon::process_victory @730ef0, victory 5: every chairs[] minutes since the last defeat or round the weakest
	// leader (or the weakest of each team) is defeated.
	if (g.info.victory != 5) return;
	if (g.frame == 0 || list_value(g, "chairs", g.info.chairs) * 900 > g.frame - g.musical_chairs) return;
	g.musical_chairs = g.frame;
	const TeamCounts tc = game_team_counts(g);
	// The size of the smallest live team: start from the largest team (at least 1), then the live members of every
	// non-empty team. (on_team[4..7] are never filled; the original still tests them.)
	int32_t v = 1;
	for (int32_t k = 0; k < 4; k++) v = std::max(v, tc.on_team[size_t(k)]);
	int32_t smallest = v;
	if (tc.on_team[4] != 0) smallest = 1;
	if (tc.on_team[5] != 0) smallest = 1;
	if (tc.on_team[6] != 0) smallest = 1;
	if (tc.on_team[7] == 0) {
		if (smallest > 1) {
			for (int32_t k = 0; k < 4; k++) {
				if (tc.on_team[size_t(k)] == 0) continue;
				int32_t live = 0;
				for (int32_t i = 0; i < 8; i++)
					if (valid_active(LD(g, i)) && leader_get_team(g, i) == k) live++;
				smallest = std::min(smallest, live);
			}
		}
	} else {
		smallest = 1;
	}
	const uint8_t ts = g.info.team_style;
	int32_t defeated = 0;
	if (ts == 0 || ts == 8 || ts == 11 || smallest < 2) {
		// Free for all: the lowest score among the live leaders; a tie with the lowest so far spares it.
		int32_t victim = -1, best = 99999999;
		bool tie = true;
		for (int32_t i = 0; i < 8; i++) {
			if (!valid_active(LD(g, i))) continue;
			const int32_t s = LD(g, i).score;
			if (s <= best) {
				tie = (s == best && victim != i);
				best = s;
				victim = i;
			}
		}
		if (!tie) {
			leader_defeat(g, victim, 4, -1, 0);
			defeated++;
		}
	} else {
		// Teams: the weakest member of each of the teams 0..3.
		for (int32_t team = 0; team < 4; team++) {
			int32_t victim = -1, best = 99999999;
			bool tie = true;
			for (int32_t i = 0; i < 8; i++) {
				if (!valid_active(LD(g, i)) || leader_get_team(g, i) != team) continue;
				const int32_t s = LD(g, i).score;
				if (s <= best) {
					tie = (s == best && victim != i);
					best = s;
					victim = i;
				}
			}
			if (!tie && victim >= 0) {
				leader_defeat(g, victim, 4, -1, 0);
				defeated++;
			}
		}
	}
	GameEvent e;
	e.frame = g.frame, e.kind = victoryevent::kChairsRound, e.value = list_value(g, "chairs", g.info.chairs), e.other = defeated;
	g.events.push_back(e);
}

void victory_process_economic(Game& g) {
	// victory 8: the highest team economy reaching econwins[econwin] wins (type 5). Strict '>' from 0: the lower index keeps
	// a tie, and with nobody above 0 leader 0 stands for the best.
	if (g.info.victory != 8) return;
	int32_t best = 0, bv = 0;
	for (int32_t i = 0; i < 8; i++) {
		if (!valid_active(LD(g, i))) continue;
		const int32_t e = leader_get_team_economic(g, i);
		if (bv < e) bv = e, best = i;
	}
	if (list_value(g, "econwins", g.info.econwin) <= bv) leader_victory(g, best, 5, 0);
}

// ---------------------------------------------------------------- tech race

void leader_tech_race_age(Game& g, int32_t who) {
	// gain_tech step 32: victory 9, not Conquer the World, and the leader's age count reached the game's last age.
	if (g.info.victory == 9 && !g.ctw && LD(g, who).age == int32_t(g.info.ending_technology)) leader_victory(g, who, 3, 0);
}

void leader_tech_race_epoch(Game& g, int32_t who) {
	// gain_tech step 33: under Conquer the World the race is to all 28 epoch techs (raw epochs 0x6959b).
	if (g.ctw && g.info.victory == 9 && LD(g, who).epochs == 28) leader_victory(g, who, 3, 0);
}

void armageddon_nuke_detonated(Game& g) {
	if (g.info.starting_resources == 8 || !(g.multiplayer || g.info.num_nations > 1)) return;
	g.armageddon++;
	GameEvent e;
	e.frame = g.frame, e.kind = victoryevent::kNukeCount, e.value = g.armageddon;
	g.events.push_back(e);
}

// ---------------------------------------------------------------- capitals and World Government

bool leader_has_capital(const Game& g, int32_t who, int32_t* owner) {
	if (g.info.elimination != 1) {
		if (owner) *owner = who;
		return true;
	}
	int32_t c = -1, w = who;
	find_capital(g, who, &c, &w, -1, -1);
	if (owner) *owner = w;
	return w == who;
}

void leader_global_government_defeats(Game& g, int32_t who) {
	for (int32_t p = 0; p < 8; p++) {
		if (p == who) continue;
		int32_t owner = p;
		if (leader_has_capital(g, p, &owner)) continue;
		if (owner == who || is_ally(g, who, owner)) leader_defeat(g, p, 1, who, 1);
	}
}

// ---------------------------------------------------------------- allied victory

void leader_allied_victory(Game& g, int32_t who) {
	leader_victory(g, who, 0, 0);
	g.victory_checked = true;  // BitMask<256>::set(semaphore, 0x16): Game::do_frame runs process_end_game
	GameEvent e;
	e.frame = g.frame, e.kind = 4, e.who = who, e.value = 1;
	g.events.push_back(e);
}

// ---------------------------------------------------------------- resign

void player_leave_game(Game& g, int32_t who, int32_t why) {
	if (who < 0 || who >= 8 || !(LD(g, who).flags & leaderflag::kValid)) return;
	// (Multiplayer: another human seat still playing the same leader keeps it alive; seats are one per leader here.)
	const Leader& L = LD(g, who);
	if (!valid_active(L) || L.lost_capital_timer == 0) {
		leader_defeat(g, who, why, -1, 0);
		return;
	}
	if (g.info.elimination == 1) {
		int32_t c = -1, w = who;
		find_capital(g, who, &c, &w, -1, -1);
		leader_defeat(g, who, 1, w, 0);
		return;
	}
	leader_defeat(g, who, 1, who, 0);
}

void player_resign(Game& g, int32_t who, bool quit) {
	GameEvent e;
	e.frame = g.frame, e.kind = victoryevent::kResigned, e.who = who, e.value = quit ? 1 : 0;
	g.events.push_back(e);
	player_leave_game(g, who, 6);
}

void player_drop(Game& g, int32_t who) {
	GameEvent e;
	e.frame = g.frame, e.kind = victoryevent::kResigned, e.who = who, e.value = 2;
	g.events.push_back(e);
	player_leave_game(g, who, 7);
}

bool game_is_victory_timer(const Game& g) {
	if (g.info.victory != 0) return false;
	for (int32_t i = 0; i < 8; i++)
		if (valid_active(LD(g, i)) && (LD(g, i).wonderwin_timer != 0 || LD(g, i).popwin_timer != 0)) return true;
	return false;
}

bool game_ended(const Game& g) { return g.game_over || g.victory_checked; }

}  // namespace ron::sim
