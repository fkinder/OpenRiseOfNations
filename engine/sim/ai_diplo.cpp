// The diplomacy AI (docs/ai-spec.md 10.1), the diplomacy actions it calls (10.1.9: set_diplo, action_declare,
// action_treaty, action_attack, action_clear_all, consider_tribute; action_offer / action_respond are in
// econ_market.cpp) and Leader::compute_score with its compute_*_score parts.
#include <algorithm>

#include "sim/aircraft.hpp"
#include "sim/ai_internal.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/victory.hpp"

namespace ron::sim {

using namespace ai;

namespace {

constexpr int32_t kCancelAlliance = 676;  // CANCEL_ALLIANCE (676) + t: the cost of breaking an alliance
constexpr int32_t kGlobalGovernment = 580, kGlobalGovernmentBonus = 697;
constexpr int32_t kWealth = 2, kKnowledge = 3;

bool va(const Game& g, int32_t k) { return (LD(g, k).flags & 3) == 3; }
bool human(const Game& g, int32_t k) { return (LD(g, k).flags & leaderflag::kHuman) != 0; }
bool peace(const Game& g, int32_t a, int32_t b) { return !is_ally(g, a, b) && !is_enemy(g, a, b); }
int32_t stamp_age(const Game& g, int32_t s) { return g.frame - s; }

int32_t owned_w(const Game& g, int32_t who, int32_t w) {  // num_buildings[w] + its `to` chain
	if (!is_build_type(w)) return 0;
	const Leader& L = LD(g, who);
	int32_t n = L.num_buildings[size_t(w - kBuilds)];
	const int32_t to = g.type(w).to;
	if (to >= 0 && is_build_type(to)) n += get_buildings(g, who, to, L.num_buildings[size_t(to - kBuilds)]);
	return n;
}

// LeaderData::any_proposals(j) @6d5ad0.
bool any_proposals(const Game& g, int32_t who, int32_t j) {
	const Leader::Deal& d = LD(g, who).dip[size_t(j)];
	if (d.treaty != -1) return true;
	for (int32_t k = 0; k < 6; k++)
		if (d.offers[size_t(k)] != 0) return true;
	for (int32_t k = 0; k < 8; k++)
		if (d.attacks[size_t(k)] != 0) return true;
	return false;
}

// LeaderData::resource_value(g) @6c18d0.
int32_t resource_value(const Game& g, int32_t who, int32_t k) {
	const Leader& L = LD(g, who);
	const int32_t e = L.econ[size_t(k)], b = L.bucket[size_t(k)];
	if ((e & 2) && b < 1000) return 512;
	if ((e & 4) && b < 500) return 384;
	if ((e & 8) || b > 1000) return 170;
	return 256;
}

// Game::is_victory_timer @594950: standard victory and a VA leader with a wonder or population victory countdown.
bool is_victory_timer(const Game& g) {
	if (g.info.victory != 0) return false;
	for (int32_t k = 0; k < 8; k++)
		if (va(g, k) && LD(g, k).wonderwin_timer != 0) return true;
	for (int32_t k = 0; k < 8; k++)
		if (va(g, k) && LD(g, k).popwin_timer != 0) return true;
	return false;
}

// LeaderData::get_team_score @6d6520. TODO(teams): the team-game test is semaphore byte0 bit 7, which nothing in
// the port sets (no writer found); the own score is returned.
int32_t get_team_score(const Game& g, int32_t who) { return LD(g, who).score; }

// Leader::eject_my_shit_from_his_ass(whom) @6d0220: every unit of who inside a container of whom comes out; an
// aircraft that came out (not the helicopter line) gets a strafe order (it flies off and looks for a base); a unit that
// cannot come out dies.
void eject_units(Game& g, int32_t who, int32_t whom) {
	const int32_t n = int32_t(g.objects[size_t(who)].units.size());
	for (int32_t i = 0; i < n; i++) {
		Unit* u = g.unit(who, i);
		if (!u || !u->alive() || u->inside_up < 0) continue;
		int32_t o = u->inside_up, w = u->inside_up_who;
		for (int guard = 0; guard < 64; guard++) {  // ObjectData::get_inside: the outermost container
			const Object* ob = g.object(w, o);
			if (!ob || !ob->is_unit) break;
			const Unit& c = static_cast<const Unit&>(*ob);
			if (c.inside_up < 0) break;
			w = c.inside_up_who;
			o = c.inside_up;
		}
		if (w != whom) continue;
		if (unit_come_out(g, *u, 0) != 0) unit_die(g, *u, 0);
		else if (u->ptype->domain == kDomainAir && !unit_is(g, *u, 0x136)) add_strafe_order(g, *u, -1, -1, -1, -1, 1, kQueueNew, false);
	}
}

}  // namespace

// ---------------------------------------------------------------- team helpers

bool is_neutral(const Game& g, int32_t who) {
	// LeaderData::is_neutral @6ebae0: team style 7 and the lobby slot's team 8.
	if (g.info.team_style != 7 || who < 0 || who >= 8) return false;
	return (g.info.player_flags[size_t(who)] & 1) && g.info.team[size_t(who)] == 8;
}

bool is_team_p(const Game& g, int32_t a, int32_t b, int32_t p) {
	// LeaderData::is_team(b, p) @6ebd30 on a.
	if (a == b) return true;
	if (a < 0 || a >= 8 || b < 0 || b >= 8) return false;
	if (g.info.team_style == 7 && (is_neutral(g, a) || is_neutral(g, b))) return false;
	if (g.frame != 0 && p == 0) return is_ally(g, a, b);
	const int8_t t = g.info.team[size_t(a)];
	if (t < 0 || t >= 4 || t != g.info.team[size_t(b)]) return false;
	const uint8_t ts = g.info.team_style;
	if (g.frame != 0 && p != 0 && (ts == 0 || ts == 11 || ts == 8)) return is_ally(g, a, b);
	return true;
}

int32_t num_allies(const Game& g, int32_t who) {
	// LeaderData::num_allies @6d6250: VA leaders other than who allied both ways.
	int32_t n = 0;
	for (int32_t k = 0; k < 8; k++) {
		if (k == who || !va(g, k)) continue;
		if (LD(g, who).diplos(k) == 2 && LD(g, k).diplos(who) == 2) n++;
	}
	return n;
}

int32_t num_team_members(const Game& g, int32_t who, int32_t p) {
	// LeaderData::num_team_members(p) @6ebf90: self plus VALID (and ACTIVE when p) team members.
	int32_t n = 0;
	for (int32_t k = 0; k < 8; k++) {
		if (k == who) {
			n++;
			continue;
		}
		const uint32_t f = LD(g, k).flags;
		if (!(f & 1) || (p != 0 && !(f & 2))) continue;
		if (is_team_p(g, who, k, p)) n++;
	}
	return n;
}

// ---------------------------------------------------------------- diplomacy actions (ai-spec 10.1.9)

void leader_set_diplo(Game& g, int32_t who, int32_t t, int32_t d) {
	// Leader::set_diplo(t, d) @6ec6a0 (the console messages are presentation).
	Leader& L = LD(g, who);
	Leader& T = LD(g, t);
	if (L.diplo[size_t(t)] == d) return;
	if (L.diplo[size_t(t)] == 2) {
		L.ally_mask &= uint8_t(~(1u << (uint32_t(t) & 31)));
		T.ally_mask &= uint8_t(~(1u << (uint32_t(who) & 31)));
		eject_units(g, who, t);
		eject_units(g, t, who);
	}
	{  // the tool log (ui-port 11): 15 diplomacy changed
		GameEvent e;
		e.frame = g.frame, e.kind = 15, e.who = who, e.other = t, e.value = d, e.alert = L.diplo[size_t(t)];
		g.events.push_back(e);
	}
	L.diplo[size_t(t)] = d;
	T.diplo[size_t(who)] = d;
	if (d == 2) {
		if (has_preq(g, who, fogtype::kAllyLos) || g.info.reveal_map != 0) L.ally_mask |= uint8_t(1u << (uint32_t(t) & 31));
		if (has_preq(g, t, fogtype::kAllyLos) || g.info.reveal_map != 0) T.ally_mask |= uint8_t(1u << (uint32_t(who) & 31));
		int32_t outside = 0;
		for (int32_t k = 0; k < 8; k++)
			if (k != who && k != t && va(g, k) && !is_ally(g, k, who) && !is_ally(g, k, t)) outside++;
		// Nobody left outside the new alliance: Leader::victory(who, 0, 0) and semaphore bit 0x16 (the end-game screen),
		// victory-spec 6.
		if (outside == 0) leader_allied_victory(g, who);
	}
	armies_diplo_change(g, who);
	// (scene.recalc_borders: presentation)
}

namespace {

bool afford_dow(const Game& g, int32_t who, int32_t t, int32_t d) {
	// LeaderData::afford_dow(t, d, &good) @6d5ce0.
	const Leader& L = LD(g, who);
	if (t != who && (L.diplos(t) == 0 || LD(g, t).diplos(who) == 0)) return true;
	int32_t base = kDeclareWar;
	if (d != 0) {
		if (t != who && (L.diplos(t) != 2 || LD(g, t).diplos(who) != 2)) return true;
		base = kCancelAlliance;
	}
	for (int32_t k = 0; k < 6; k++) {
		if (!type_avail(g, who, k, 1)) continue;
		if (L.bucket[size_t(k)] < get_cost(g, base + t, k, who, -1, -1, 0, 1, -1)) return false;
	}
	return true;
}

void pay_dow(Game& g, int32_t who, int32_t t, int32_t d) {
	// Leader::pay_dow(t, d) @6d2b10.
	Leader& L = LD(g, who);
	const int32_t base = d == 0 ? kDeclareWar : kCancelAlliance;
	for (int32_t k = 0; k < 6; k++) {
		if (!type_avail(g, who, k, 1)) continue;
		const int32_t c = get_cost(g, base + t, k, who, -1, -1, 0, 1, -1);
		L.bucket[size_t(k)] = std::max(0, L.bucket[size_t(k)] - c);
		L.escrow[size_t(k)] = std::max(0, L.escrow[size_t(k)] - get_cost(g, base + t, k, who, -1, -1, 0, 1, -1));
	}
}

}  // namespace

int32_t leader_action_declare(Game& g, int32_t who, int32_t t, int32_t d, int32_t free, int32_t force) {
	// Leader::action_declare(t, d, free, force) @6dab50: d 0 war, 1 peace (break an alliance). Returns 0 when done or
	// already at war, 1 when refused.
	Leader& L = LD(g, who);
	for (;;) {
		if (is_enemy(g, who, t)) return 0;
		bool check = true;
		if (d == 0 && force == 0) {
			if (is_ally(g, who, t)) {
				d = 1;  // war on an ally: only break the alliance
				force = 0;
				continue;
			}
			const int32_t s = std::max(LD(g, t).broke_alliance[size_t(who)], L.broke_alliance[size_t(t)]);
			if (s != 0 && stamp_age(g, s) < K("ally_to_war_delay")) return 1;
		}
		if (check && free == 0 && !afford_dow(g, who, t, d)) {  // (LeaderOut::warn_resources)
			GameEvent e;  // the tool log (ui-port 11): 16 resource flash, goods[k] = the shortfall of good k
			e.frame = g.frame, e.kind = 16, e.who = who, e.other = t;
			const int32_t base = d == 0 ? kDeclareWar : kCancelAlliance;
			for (int32_t k = 0; k < 6; k++) {
				if (!type_avail(g, who, k, 1)) continue;
				const int32_t c = get_cost(g, base + t, k, who, -1, -1, 0, 1, -1);
				if (L.bucket[size_t(k)] < c) e.goods[size_t(k)] = c - L.bucket[size_t(k)];
			}
			g.events.push_back(e);
			return 1;
		}
		if (d != 0 || war_allowed(g)) {
			if (free == 0) {
				pay_dow(g, who, t, d);
				if (is_ally(g, who, t) && d == 0) pay_dow(g, who, t, 0);
				if (d == 0 && (is_ally(g, who, t) || L.invaders[size_t(t)] < 5)) {
					if (L.dow[size_t(t)] != 0) L.blacken++;
					L.dow[size_t(t)]++;
				}
				if (is_ally(g, who, t)) {
					if (L.dow[size_t(t)] != 0) L.blacken++;
					L.dow[size_t(t)]++;
				}
			}
			if (is_ally(g, who, t)) L.broke_alliance[size_t(t)] = g.frame;
			leader_set_diplo(g, who, t, d);
			for (int32_t k = 0; k < 8; k++) {  // team mates follow (Leader::ally_diplo @6d0120 = set_diplo + chat)
				if (k == who || k == t || !(LD(g, k).flags & leaderflag::kValid)) continue;
				const bool tw = is_team_p(g, k, who, 0), tt = is_team_p(g, k, t, 0);
				if (tw && !tt) {
					if (d < LD(g, k).diplos(t)) leader_set_diplo(g, k, t, d);
				} else if (!tw && tt) {
					if (d < LD(g, k).diplos(who)) leader_set_diplo(g, k, who, d);
				}
			}
			return 0;
		}
		if (force == 0) return 1;
		d = 1;
	}
}

void leader_consider_tribute(Game& g, int32_t who, int32_t from, int32_t amount, int32_t good) {
	// Leader::consider_tribute(from, amount, good) @6d1360 (ai-spec 10.2), on the receiver.
	if (amount <= 0) return;
	Leader& L = LD(g, who);
	const Leader& F = LD(g, from);
	LD(g, from).gift_stamp[size_t(who)] = g.frame;
	if ((L.flags & leaderflag::kHuman) ||
	    (F.blacken * 100 <= amount && L.bucket[size_t(good)] < 3000 && (good == kWealth || (L.econ[size_t(good)] & 4))))
		L.tribute_stamp[size_t(from)] = g.frame;
}

namespace {

void deal_clear_all(Leader::Deal& d) {  // Diplomacy::clear_all @47e030
	d.treaty = -1;
	d.any_offer = 0;
	d.agree = 0;
	d.offers.fill(0);
	d.attacks.fill(0);
	d.dows.fill(0);
}

}  // namespace

void leader_action_clear_all(Game& g, int32_t who, int32_t b) {
	// Leader::action_clear_all(b) @6d15e0.
	clear_agree(g, who, b);
	clear_agree(g, b, who);
	deal_clear_all(LD(g, who).dip[size_t(b)]);
	deal_clear_all(LD(g, b).dip[size_t(who)]);
}

void leader_action_treaty(Game& g, int32_t who, int32_t b, int32_t t) {
	// Leader::action_treaty(b, t) @6d1570: 1 peace, 2 alliance.
	LD(g, who).dip[size_t(b)].any_offer = 1;
	clear_agree(g, who, b);
	clear_agree(g, b, who);
	LD(g, who).dip[size_t(b)].treaty = t;
	LD(g, b).dip[size_t(who)].treaty = t;
}

void leader_action_attack(Game& g, int32_t who, int32_t b, int32_t k, int32_t v) {
	// Leader::action_attack(b, k, v) @6d14e0 (= action_propose_attack @6d14d0).
	LD(g, who).dip[size_t(b)].any_offer = 1;
	if (k < 0) return;
	clear_agree(g, who, b);
	clear_agree(g, b, who);
	LD(g, who).dip[size_t(b)].attacks[size_t(k)] = v;
	LD(g, b).dip[size_t(who)].attacks[size_t(k)] = v;
}

// ---------------------------------------------------------------- Leader::diplomacy (ai-spec 10.1)

void leader_diplomacy(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	// 10.1.1: co-op seats (flags & 4 | 8) included. (Balance mode, semaphore byte1 & 2: the developers' UnitBalance
	// test harness, not reachable from the game: victory-spec 9.)
	if ((L.flags & leaderflag::kHuman) || g.ai_off) return;
	int32_t topScorer = -1, best = 0, nAbove = 0, nNonAllied = 0;
	for (int32_t k = 0; k < 8; k++) {
		if (!va(g, k)) continue;
		const Leader& K = LD(g, k);
		if (!is_ally(g, k, who)) nNonAllied++;
		if (K.score > L.score) nAbove++;
		if (K.score >= best) {
			best = K.score;
			topScorer = k;
		}
	}
	const int32_t diff = get_diff(g, who);
	const uint32_t seed = g.seed;
	const int32_t sseed = int32_t(seed);
	const bool scripted = g.scripted_rules || g.ctw;
	const int32_t dset = g.scen_diplomacy_setting[size_t(who)];
	const bool mp = g.multiplayer;
	const uint8_t ts = g.info.team_style;
	const bool unlocked = ts == 0 || ts == 8 || ts == 11;
	const bool rusher = is_rusher(g, who), boomer = is_boomer(g, who);
	auto withdraw_or_reject = [&](int32_t j) {
		action_respond(g, who, j, L.dip[size_t(j)].agree == 1 ? 0 : 2);
	};

	for (int32_t j = 0; j < 8; j++) {
		Leader& J = LD(g, j);
		// 10.1.2 gate and cadence
		if (!(J.flags & leaderflag::kActive) || j == who || !(L.treaty(j) & 1) || (L.treaty(j) & 4)) continue;
		bool urgent;
		{
			const int32_t a = L.agendas[size_t(j)];
			if (a & 4) {
				urgent = true;
			} else {
				uint32_t P = 0x800;
				if (L.dip[size_t(j)].agree != 1) {
					if (L.tribute_demanded[size_t(j)] != 0) P = 0x2000;
					if (L.counteroffer[size_t(j)] != 0) P *= 2;
				}
				if (!(a & 8)) P >>= 2;
				if (((uint32_t(j + 8 * who) * (P >> 6) + uint32_t(g.frame)) & (P - 1)) != 0) continue;
				urgent = false;
			}
			L.agendas[size_t(j)] = a & ~4;
		}
		const bool open = unlocked || (ts == 7 && (is_neutral(g, who) || is_neutral(g, j)));

		// 10.1.3 per-j quantities
		const int32_t delta = L.age - J.age;  // C1
		const int32_t kk = rusher ? int32_t(seed % 3u) + 4 : boomer ? int32_t(seed % 6u) + 8 : int32_t(seed % 5u) + 6;
		const int32_t T = ((int32_t((seed + uint32_t(who)) & 7u)) + 3 * kk) * 300;  // C2
		int32_t mineHeldByJ = 0, jHeldByMe = 0;  // C3
		for (int32_t c = 0; c < J.city_mark && size_t(c) < g.cities[size_t(j)].size(); c++) {
			const City& ct = g.cities[size_t(j)][size_t(c)];
			if ((ct.city_flags & 1) && ct.race == who) mineHeldByJ++;
		}
		for (int32_t c = 0; c < L.city_mark && size_t(c) < g.cities[size_t(who)].size(); c++) {
			const City& ct = g.cities[size_t(who)][size_t(c)];
			if ((ct.city_flags & 1) && ct.race == j) jHeldByMe++;
		}
		int32_t lostFounded = 0, invadedBy = 0;  // C4
		for (int32_t k = 0; k < 8; k++) {
			if (!va(g, k) || k == who || k == j || is_ally(g, k, j) || is_ally(g, k, who)) continue;
			for (int32_t c = 0; c < LD(g, k).city_mark && size_t(c) < g.cities[size_t(k)].size(); c++) {
				const City& ct = g.cities[size_t(k)][size_t(c)];
				if ((ct.city_flags & 1) && ct.founder == who) lostFounded++;
			}
			if (L.invaders[size_t(k)] >= 15) invadedBy++;
		}
		// C5 region pass
		int32_t& aggr = L.aggression[size_t(j)];
		int32_t& weak = L.weak[size_t(j)];
		int32_t& strong = L.strong[size_t(j)];
		aggr = weak = strong = 0;
		const int32_t half = (L.city_num + L.village_num) / 2;
		for (int32_t r = 1; r < 64; r++) {
			const int32_t lc = L.reg_cities[size_t(r)], lm = L.reg_combat[size_t(r)];
			const int32_t jc = J.reg_cities[size_t(r)], jm = J.reg_combat[size_t(r)];
			if (lc == 0 && lm == 0) continue;
			if (jc == 0 && jm == 0) continue;
			aggr = 1;
			if (lc >= half) {
				int32_t v = lm + 3 * int32_t(L.reg_defense[size_t(r)]);
				if (jc != 0) v += lm >> 1;
				if (delta > 0) v = 4 * v / 3;
				else if (delta < 0) v = 3 * v / 4;
				if (jm > v) {
					weak += (jm << 5) / (v + 1);
					continue;
				}
			}
			if (jc != 0) {
				int32_t x = lm;
				if (lc != 0 && is_ally(g, who, j)) x -= jm;
				strong += x;
			} else if (lc != 0) {
				strong += (lm >> 1) - jm + (int32_t(L.reg_defense[size_t(r)]) >> 1);
			}
		}
		weak /= std::max(1, L.dow[size_t(who)]);  // C6
		bool freeK = false;  // C7
		for (int32_t k = 0; k < 8 && !freeK; k++)
			if (va(g, k) && k != j && is_enemy(g, k, j) && LD(g, k).weak[size_t(j)] == 0) freeK = true;
		if ((weak != 0 || J.attack > 3 * L.attack) && !freeK) aggr = 0;
		if (delta > 0) strong = 4 * strong / 3 + 1;  // C8
		else if (delta < 0) strong = 3 * strong / 4;
		bool patience = false;  // C9
		int32_t capTarget = -1, victTarget = -1;
		if ((L.agendas[size_t(j)] & 0x10) && !is_enemy(g, who, j)) strong += L.invaders[size_t(j)];
		if (human(g, j)) {  // C10
			strong += diff >= 4 ? 2 : diff == 3 ? 1 : 0;
			for (int32_t bt : {ptype::kBarracks, ptype::kStable, ptype::kSiegeFactory, ptype::kAirbase})
				if (owned_w(g, who, bt) != 0 && owned_w(g, j, bt) == 0) strong++;
			if (diff >= 3)
				for (int32_t k = 0; k < 8; k++)
					if (va(g, k) && k != who && k != j && is_enemy(g, k, j)) strong++;
		}
		strong <<= 5;  // C11
		if (strong <= 0) aggr = 0;

		// 10.1.4 hostility
		bool b = false;  // H1
		if (L.agendas[size_t(j)] & 2) {
			L.agendas[size_t(j)] &= ~2;
			b = true;
		}
		if ((L.agendas[size_t(j)] & 0x10) && !is_enemy(g, who, j) && L.invaders[size_t(j)] > (boomer ? 4 : 1)) b = true;
		const int32_t e = is_enemy(g, who, j) ? 1 : 0;
		const int32_t c = e + (L.pers.friendly_human < 0 ? 1 : 0);
		bool busy = false;
		if (L.wars - (b ? 1 : 0) > c) {
			aggr = 0;
			busy = true;
		}
		if (L.wars != 0 && (invadedBy != 0 || lostFounded != 0)) {
			aggr = 0;
			busy = true;
		}
		const int32_t myTS = get_team_score(g, who);  // H2
		int32_t maxTS = 0, nEnemies = 0, nFriendly = 0;
		for (int32_t k = 0; k < 8; k++)
			if (va(g, k)) maxTS = std::max(maxTS, get_team_score(g, k));
		for (int32_t k = 0; k < 8; k++) {
			if (k == who || !va(g, k)) continue;
			if (is_enemy(g, who, k)) nEnemies++;
			else if (!is_neutral(g, k)) nFriendly++;
		}
		if (is_ally(g, who, j)) {  // H3
			aggr = 0;
			if (is_victory_timer(g)) continue;
			if (open) {
				int32_t n = 0, last = -1;
				for (int32_t k = 0; k < 8; k++)
					if (k != who && k != j && va(g, k)) {
						n++;
						last = k;
					}
				if (n == 1) {
					if (diff >= 4 && human(g, j)) aggr = 1;
					if (get_team_score(g, last) < myTS && L.score > J.score) aggr = 1;
				}
			}
		}
		if (ts == 7 && is_neutral(g, who)) {  // H4
			if (myTS == maxTS) aggr = 1;
			else if (myTS <= 2 * maxTS / 3) aggr = 0;
			if (!is_ally(g, who, j) && nEnemies == 0) aggr = 1;
			if (nFriendly == 0) aggr = 0;
			if (is_neutral(g, j)) aggr = 0;
		} else {  // H5
			if (is_ally(g, who, j)) {
				if (diff <= 2 && human(g, j)) aggr = 0;
			} else if (peace(g, who, j) && human(g, j)) {
				if (J.dow[size_t(who)] < 2 && J.blacken < 4 && J.wars > 0) {
					if (diff <= 1) aggr = 0;
					else if (diff == 2 && J.wars > 1) aggr = 0;
					else if (diff == 3 && J.wars > 2) aggr = 0;
				}
				if (!busy && J.wars == 0) {
					if (diff >= 4) {
						patience = !(boomer && L.epoch[0] < 4);
					} else if (diff == 3) {
						const int32_t d1 = rnd_raw(g) & 1;  // D1
						patience = (d1 == 0 && !(boomer && L.epoch[0] < 4)) || rusher;
					} else if (diff == 2) {
						patience = rusher;
					}
				}
			}
		}
		if (diff >= 2) {  // H6
			if (topScorer == j && (!is_ally(g, who, j) || nAbove < 2) && g.frame >= (diff >= 3 ? 30 : 45) * 900 &&
			    nAbove < (human(g, j) ? 4 : 2))
				aggr = 1;
			if (!is_ally(g, who, j) && nNonAllied == 1) aggr = 1;
		}
		if (human(g, j) && mp) {  // H7
			if (is_ally(g, who, j)) aggr = 0;
			else if (is_enemy(g, who, j) && J.wars > 1) aggr = 0;
			else if (peace(g, who, j) && J.wars != 0) aggr = 0;
		}
		if (mineHeldByJ != 0 && J.blacken < 2 && !rusher) {  // H8
			if (rnd_raw(g) % (boomer ? 2 : 4) == 0) aggr = 0;  // D2
		}
		const int32_t hs = L.hire_stamp[size_t(j)];
		if (hs != 0 && stamp_age(g, hs) < 13500) aggr = 1;  // H9
		if (jHeldByMe != 0 && mineHeldByJ == 0) {  // H10
			const int32_t d3 = rnd_raw(g) % (boomer ? 2 : 3);  // D3
			if (d3 == 0 && (hs == 0 || stamp_age(g, hs) > 4500)) aggr = 0;
		}
		for (int32_t k = 0; k < 8; k++) {  // H11
			if (!va(g, k) || k == who || k == j) continue;
			const Leader& K = LD(g, k);
			if (K.hire_who[size_t(who)] == k && stamp_age(g, K.hire_stamp[size_t(who)]) < 9000) {
				aggr = 1;
				break;
			}
			if (!is_enemy(g, j, k) && is_enemy(g, who, k) && L.hire_who[size_t(k)] == j &&
			    stamp_age(g, L.hire_stamp[size_t(k)]) <= 9000 && is_ally(g, who, j))
				aggr = 1;
		}
		if (L.agendas[size_t(j)] & 0x20) aggr = 1;  // H12
		if (patience && g.frame >= T) aggr = 1;

		// 10.1.5 team styles, elimination, victory races
		bool declare_cap = false;
		if (!open) {  // H13
			aggr = is_ally(g, who, j) ? 0 : 1;
		} else {
			if (g.info.elimination == 1) {  // E1
				int32_t oc = -1, owner = who;
				find_capital(g, who, &oc, &owner, -1, -1);
				if (owner != who) {
					if (owner == j || (owner >= 0 && is_ally(g, j, owner))) declare_cap = true;
					else {
						capTarget = owner;
						aggr = 0;
					}
				}
			}
			if (!declare_cap) {  // E2
				bool lone = false;
				if (human(g, j) && diff >= 3 && topScorer == j && g.frame >= (sseed % 10 + 25) * 900) lone = nAbove < 3;
				if (!(g.info.victory != 0 && !lone)) {
					if (!is_ally(g, who, j) &&
					    (lone || J.wonderwin_timer != 0 || J.popwin_timer != 0 || has_preq(g, j, kGlobalGovernmentBonus) ||
					     researching(g, j, kGlobalGovernment, -1, false, false))) {
						declare_cap = true;
					} else if (g.info.victory == 0) {
						for (int32_t k = 0; k < 8; k++) {
							if (!va(g, k) || k == who || k == j || is_ally(g, k, who) || is_ally(g, k, j)) continue;
							if (LD(g, k).wonderwin_timer != 0 || LD(g, k).popwin_timer != 0) {
								victTarget = k;
								if (human(g, k)) break;
							}
						}
					}
				}
			}
		}
		if (declare_cap) {  // DECLARE_CAP
			if (J.dip[size_t(who)].agree == 1) withdraw_or_reject(j);
			if (!is_enemy(g, who, j) && war_allowed(g)) leader_action_declare(g, who, j, 0, 1, 0);
			L.agendas[size_t(j)] |= 0x20;
			continue;
		}

		// 10.1.6 action selection (F)
		if (L.dip[size_t(j)].agree != 1 && !urgent && aggr == 0 && (L.agendas[size_t(j)] & 8)) {  // F1
			if (rnd_raw(g) % 3 != 0) continue;  // D4
		}
		bool cleared = false;  // F2
		if (L.dip[size_t(j)].agree == 1 || J.dip[size_t(who)].agree == 2) {
			leader_action_clear_all(g, who, j);
			cleared = true;
		}
		enum class Next { R, Hostile, Done } next = Next::R;
		if (aggr != 0 && !(scripted && dset != 0)) {
			next = is_enemy(g, who, j) ? Next::R : Next::Hostile;
		} else {
			if (!urgent && L.invaders[size_t(j)] != 0 && peace(g, who, j)) {  // "get your army out"
				L.agendas[size_t(j)] |= 0x18;
				J.got_diplo_message = g.frame;
			}
			if (cleared && aggr == 0) next = Next::Done;
		}
		if (next == Next::Done) continue;
		if (next == Next::Hostile) {
			enum class H { Warn, Declare, Demand } h;
			const int32_t D = (L.epoch[0] + 1) * (J.blacken + 1) * 100;
			if (!war_allowed(g) || !open || is_tribute_period(g, who, j) || g.frame < T) h = H::Warn;
			else if (cleared || J.blacken >= 4 || patience || (hs != 0 && stamp_age(g, hs) <= 13500)) h = H::Declare;
			else if (J.bucket[kWealth] < D) h = H::Declare;
			else h = H::Demand;
			if (h == H::Demand) {  // TRIBUTE DEMAND
				leader_action_clear_all(g, who, j);
				int32_t W = D, n = 0;
				const int32_t d5 = rnd_raw(g) % 6;  // D5
				for (int32_t i = 0; i < 6; i++) {
					const int32_t gd = (d5 + i) % 6;
					if (gd == kWealth || gd == kKnowledge) continue;
					if (!type_avail(g, who, gd, 1) || !type_avail(g, j, gd, 1)) continue;
					if ((L.econ[size_t(gd)] & 0xc) != 4 || L.bucket[size_t(gd)] >= 1000) continue;
					n++;
					if (n == 1 || rnd_raw(g) % n == 0) {  // D6 (2nd+ eligible good only)
						if (J.bucket[size_t(gd)] >= 50) {
							action_offer(g, who, j, gd, -std::min(J.bucket[size_t(gd)], D));
							continue;
						}
					}
					W += (25 * L.epoch[0] + 25) * (J.blacken + 1);
				}
				action_offer(g, who, j, kWealth, -std::min(W, J.bucket[kWealth]));
				action_respond(g, who, j, 1);  // L agrees to its own demand
				L.agendas[size_t(j)] |= 8;
				L.tribute_demanded[size_t(j)] = 1;
				continue;
			}
			if (h == H::Declare) {
				if (J.dip[size_t(who)].agree == 1) withdraw_or_reject(j);
				if (war_allowed(g)) {
					J.got_diplo_message = g.frame;
					L.agendas[size_t(j)] |= 8;
					leader_action_declare(g, who, j, is_ally(g, who, j) ? 1 : 0, 1, 0);
				}
				continue;
			}
			// WARN
			if (J.dip[size_t(who)].agree == 1) withdraw_or_reject(j);
			if ((L.agendas[size_t(j)] & 0x48) || (J.got_diplo_message != 0 && stamp_age(g, J.got_diplo_message) < 450))
				continue;
			J.got_diplo_message = g.frame;
			L.agendas[size_t(j)] |= 0x40;
			continue;
		}

		// 10.1.7 answering j's proposal (R)
		if (J.dip[size_t(who)].agree == 1) {
			if (scripted) {
				if (dset == 1) continue;
				if (dset == 2) {
					action_respond(g, who, j, 1);
					continue;
				}
				if (dset == 3) {
					withdraw_or_reject(j);
					continue;
				}
			}
			int32_t G = L.good_deeds[size_t(j)];
			if (L.dip[size_t(j)].treaty == 2) {  // R1 alliance proposal
				const bool vt = is_victory_timer(g);
				bool refuse;
				if (unlocked) refuse = num_allies(g, who) != 0;
				else if (ts == 7 && (is_neutral(g, who) || is_neutral(g, j)))
					refuse = 1 - num_team_members(g, who, 1) + num_allies(g, who) != 0;
				else refuse = true;
				if (refuse || vt) {
					withdraw_or_reject(j);
					continue;
				}
				if (!human(g, j) && aggr == 0) {
					action_respond(g, who, j, 1);
					L.counteroffer[size_t(j)] = 0;
					continue;
				}
				int32_t base = (is_enemy(g, who, j) ? 400 : 200) * (L.epoch[0] + 1);
				if (mp) base /= 2;
				if (rusher) base += 100;
				else if (boomer && L.epoch[0] < 4) base -= 100;
				if (capTarget >= 0) base -= 400;
				if (victTarget >= 0) base -= 200;
				G -= base;
			}
			bool gives = false, shortp = false;  // R2 goods
			for (int32_t gd = 0; gd < 6; gd++) {
				int32_t x = L.dip[size_t(j)].offers[size_t(gd)];
				if (x == 0) continue;
				if (!type_avail(g, who, gd, 1) || !type_avail(g, j, gd, 1)) {
					withdraw_or_reject(j);
					break;
				}
				if (x > L.bucket[size_t(gd)]) shortp = true;
				if (x > 0) gives = true;
				if (x > 0 || gives) {
					if (g.frame < 4500) x *= 4;
					else if (g.frame < 9000 && weak == 0) x = 3 * x / 2;
				}
				if (x > 0 && is_ally(g, who, j) && x <= L.bucket[size_t(gd)] / 3 &&
				    (unlocked || L.gift_stamp[size_t(j)] == 0 || stamp_age(g, L.gift_stamp[size_t(j)]) >= 4500))
					continue;  // a free gift to an ally
				G -= resource_value(g, who, gd) * x / 256;
			}
			if (!any_proposals(g, who, j)) continue;
			for (int32_t k = 0; k < 8; k++) {  // R3 attacks
				if (k == who || k == j || L.dip[size_t(j)].attacks[size_t(k)] == 0) continue;
				if (is_ally(g, who, k) || is_ally(g, j, k) || (is_enemy(g, who, j) && L.dip[size_t(j)].treaty < 1)) {
					withdraw_or_reject(j);
					break;
				}
				const Leader& K = LD(g, k);
				int32_t v;
				if (is_enemy(g, who, k)) {
					v = is_enemy(g, j, k) ? 0 : (L.weak[size_t(k)] != 0 ? 120 : 20) * (J.epoch[1] + 1);
				} else {
					v = (25 * K.blacken - (mp ? 50 : 100) * (J.blacken != 0 ? 2 : 1)) * (K.epoch[0] + 1) +
					    (L.weak[size_t(k)] != 0 ? 20 : 0) * (J.epoch[1] + 1);
				}
				if (k == capTarget) v += 100 * K.epoch[1];
				if (k == victTarget) v += 200;
				if (is_ally(g, who, j) && v < 0) v /= 2;
				G += v;
			}
			if (!any_proposals(g, who, j)) continue;  // R4
			const int32_t cw = weak != 0 ? (diff < 2 ? 100 : 50) : 0;
			int32_t X;
			if (aggr != 0) {
				int32_t H = G + J.epoch[0] * cw;
				if (!mp) H -= (L.epoch[0] + 1) * strong;
				if (rusher) H -= 50;
				else if (boomer && L.epoch[0] < 4) H += diff < 3 ? 100 : 50;
				if (diff < 2) H += 100;
				X = (L.epoch[0] + J.blacken + 1) * (mp ? 50 : 100) * (strong != 0 ? 2 : 1) - H;
			} else {
				if (!is_tribute_period(g, j, who)) {
					G += J.epoch[0] * cw;
					if (L.dip[size_t(j)].treaty == 1) {
						if (strong != 0 && weak == 0 && J.wars == 1) {
							G -= (L.epoch[0] + 1) * strong;
						} else {
							if (J.dow[size_t(who)] < 2) G += diff < 2 ? 200 : diff == 2 ? 100 : 0;
							if (J.dow[size_t(who)] < 3 && weak != 0) G += 100 * J.epoch[0] + 100;
						}
					}
				}
				X = -G;
			}
			if (L.counteroffer[size_t(j)] != 0) X -= 99;
			if (shortp && !(X > 0 && gives)) {  // R5
				withdraw_or_reject(j);
				continue;
			}
			if (X <= 0) {  // ACCEPT
				L.counteroffer[size_t(j)] = 0;
				if (any_proposals(g, who, j)) {
					action_respond(g, who, j, 1);
					if (!is_tribute_period(g, j, who) && weak != 0) X += 50;
					L.good_deeds[size_t(j)] -= X;
				}
				continue;
			}
			if (!gives) {  // COUNTER (the asked amount is chat only)
				L.counteroffer[size_t(j)] = 1;
				clear_agree(g, j, who);
				clear_agree(g, who, j);
				continue;
			}
			if (L.dip[size_t(j)].treaty != 2 && aggr != 0 && peace(g, who, j) && war_allowed(g) && (rnd_raw(g) & 1) != 0) {
				if (J.dip[size_t(who)].agree == 1) withdraw_or_reject(j);  // D7: an insulting offer means war
				leader_action_declare(g, who, j, 0, 1, 0);
				continue;
			}
			J.got_diplo_message = g.frame;  // REJECT
			L.counteroffer[size_t(j)] = 0;
			withdraw_or_reject(j);
			continue;
		}

		// 10.1.8 the AI's own proposals (P)
		if (scripted && dset == 1) continue;
		if (any_proposals(g, who, j)) continue;
		if (!is_ally(g, who, j) && topScorer == j && nAbove >= 4 && J.dow[size_t(j)] < 2 && (rnd_raw(g) & 1) != 0 &&  // D8
		    human(g, j) && num_allies(g, who) == 0 && num_allies(g, j) == 0 && unlocked) {
			J.got_diplo_message = g.frame;  // P1: alliance with the runaway human leader
			leader_action_clear_all(g, who, j);
			leader_action_treaty(g, who, j, 2);
			L.agendas[size_t(j)] |= 8;
			action_respond(g, who, j, 1);
			continue;
		}
		if (aggr != 0) continue;  // P2
		if (peace(g, who, j) && !(scripted && dset != 0) && war_allowed(g) &&
		    g.frame >= ((7 * who + sseed) % 15 + 15) * 900 && !(human(g, j) && sseed % 8 == 0)) {
			if (!((J.blacken >= 2 && capTarget < 0 && victTarget < 0) || L.score >= J.score || num_allies(g, who) != 0 ||
			      num_allies(g, j) != 0 || !unlocked)) {
				int32_t bk = -1, bv = 0;
				for (int32_t k = 0; k < 8; k++) {
					if (!va(g, k) || k == who || k == j || is_ally(g, k, who) || is_ally(g, k, j)) continue;
					if (k == capTarget) {
						bk = k;
						break;
					}
					const Leader& K = LD(g, k);
					int32_t v;
					if (k == victTarget) {
						v = 9999;
					} else {
						if (K.score < L.score || K.score < J.score) continue;
						v = K.score == topScorer ? 200 : 100;  // (a score compared with a leader index: quirk)
						if (K.score >= 3 * L.score / 2) v *= 2;
						if (K.score >= 2 * L.score) v *= 2;
						if (is_enemy(g, who, k)) v *= 3;
						if (!is_enemy(g, j, k)) v = 3 * v / 2;
						if (human(g, k)) v *= 2;
					}
					if (v > bv) {
						bv = v;
						bk = k;
					}
				}
				if (bk >= 0) {  // alliance and a joint attack on bk
					J.got_diplo_message = g.frame;
					leader_action_clear_all(g, who, j);
					leader_action_treaty(g, who, j, 2);
					leader_action_attack(g, who, j, bk, 1);
					L.agendas[size_t(j)] |= 8;
					action_respond(g, who, j, 1);
					continue;
				}
			}
		}
		if (is_enemy(g, who, j) && !(scripted && dset != 0)) {  // P3 peace offer
			leader_action_clear_all(g, who, j);
			leader_action_treaty(g, who, j, 1);
			if (weak != 0 && g.frame >= 9000) {  // with tribute
				const int32_t a = J.epoch[2] + 1 + (delta < 0 ? -2 * delta : 0);
				int32_t rem = std::max(100, 100 * a);
				const int32_t d9 = rnd_raw(g) % 6;  // D9
				for (int32_t i = 0; i < 6; i++) {
					const int32_t gd = (d9 + i) % 6;
					if (gd == kKnowledge || !type_avail(g, who, gd, 1) || !type_avail(g, j, gd, 1)) continue;
					if (L.bucket[size_t(gd)] < 100) continue;
					if (L.bucket[size_t(gd)] >= rem) {
						action_offer(g, who, j, gd, rem);
						break;
					}
					const int32_t y = (L.bucket[size_t(gd)] / 100) * 100;
					action_offer(g, who, j, gd, y);
					rem -= y;
					if (rem < 100) L.tribe = 100;  // (writes this->tribe, not a local: quirk, ai-spec 10.1.8)
				}
			}
			J.got_diplo_message = g.frame;
			L.agendas[size_t(j)] |= 8;
			action_respond(g, who, j, 1);
		}
		if (!(scripted && dset != 0) && peace(g, who, j) && aggr == 0 && !(L.agendas[size_t(j)] & 0x48) &&  // P4
		    (J.got_diplo_message == 0 || stamp_age(g, J.got_diplo_message) >= 675)) {
			L.agendas[size_t(j)] |= 0x40;
			J.got_diplo_message = g.frame;
		}
	}
}

// ---------------------------------------------------------------- Leader::compute_score @6ec560

namespace {

// TypeData::get_score_value(p) @6673b0 (vslot 0x7c): the summed costs times the cost factor of the type's kind; for
// p == 0 militia types (66..68) count as the first unit type; for p != 0 a unit's value has the research premium.
int32_t get_score_value(const Game& g, int32_t t, int32_t p) {
	int32_t t2 = t;
	if (p == 0 && (t == ptype::kMilitia || t == ptype::kMinuteman || t == ptype::kPartisan)) t2 = kUnits;
	int32_t v = 0;
	for (int32_t c : g.type(t2).costs) v += c;
	if (is_unit_type(t)) {
		v = K("unit_cost_factor") * v;
		if (p == 0) return v;
		v = K("research_premium") * v;
		v = (v / 256) * g.type(t2).research_premium_cost;
		return v / 256;
	}
	if (is_build_type(t)) return K("build_cost_factor") * v;
	if (t >= kSpells && t < kEndSpells) return K("spell_cost_factor") * v;  // (techs 544..628 and the rest: tech)
	return K("tech_cost_factor") * v;
}

bool tech_at_start(const Leader& L, int32_t t) { return (L.tech_at_start[size_t(t >> 3)] >> (t & 7)) & 1; }

void compute_unit_score(Game& g, int32_t who) {  // @6bc500
	Leader& L = LD(g, who);
	L.score_units = L.score_attack_units = 0;
	if (g.armageddon >= get_armageddon(g)) return;
	for (int32_t t = 50; t < 402; t++) {
		const int32_t n = L.num_queued[size_t(t)] + L.num_units[size_t(t - kUnits)];
		if (n == 0) continue;
		const Type& T = g.type(t);
		const int32_t v = (((n - 1) * (T.support_cost[1] + T.support_cost[0]) * n) / 2 + get_score_value(g, t, 0) * n) / 5;
		L.score_units += v;
		if (T.attack != 0) L.score_attack_units += v;
	}
}

void compute_build_score(Game& g, int32_t who) {  // @6bc3f0
	Leader& L = LD(g, who);
	L.score_buildings = L.score_wonders = 0;
	if (g.armageddon >= get_armageddon(g)) return;
	const int32_t f = K("build_support_factor");
	for (int32_t t = 414; t < 543; t++) {
		const int32_t n = L.num_buildings[size_t(t - kBuilds)] + L.num_queued[size_t(t)];
		if (n == 0) continue;
		const Type& T = g.type(t);
		const int32_t v = get_score_value(g, t, 0) * n + ((n - 1) * (T.support_cost[1] + T.support_cost[0]) * f * n) / 2;
		if (is_wonder_type(t)) L.score_wonders += v / 3;
		else L.score_buildings += v / 5;
	}
}

void compute_economy_score(Game& g, int32_t who) {  // @6bc270
	Leader& L = LD(g, who);
	L.score_economy = 0;
	if (g.armageddon >= get_armageddon(g)) return;
	for (int32_t k = 0; k < 6; k++) {
		if (!type_avail(g, who, k, 1)) continue;
		int32_t v = (L.bucket[size_t(k)] - g.starting[size_t(k)]) / 20;
		v = std::clamp(v, 0, 100);
		L.score_economy += v;
		L.score_economy += std::min(200, L.income[size_t(k)] / 0x50);
	}
	int32_t rares = 0;
	for (int32_t b = 0; b < 44; b++) rares += (L.rare[size_t(b >> 3)] >> (b & 7)) & 1;
	L.score_economy += rares * 25;
}

void compute_unit_upgrades_score(Game& g, int32_t who) {  // @6bc1a0
	Leader& L = LD(g, who);
	L.score_upgrades = 0;
	if (g.armageddon >= get_armageddon(g)) return;
	for (int32_t t = 50; t < 402; t++) {
		if (!has_tech(g, who, t) && !researching(g, who, t, -1, false, true)) continue;
		if (tech_at_start(L, t) || (g.type(t).unit_flags & 0x80)) continue;
		L.score_upgrades += get_score_value(g, t, 1) / 5;
	}
}

void compute_research_score(Game& g, int32_t who) {  // @6bc360
	Leader& L = LD(g, who);
	L.score_research = 0;
	if (g.armageddon >= get_armageddon(g)) return;
	for (int32_t t = 544; t < 629; t++) {
		if (!has_tech(g, who, t) && !researching(g, who, t, -1, false, false)) continue;
		if (tech_at_start(L, t)) continue;
		L.score_research += get_score_value(g, t, 0) / 5;
	}
}

}  // namespace

void leader_compute_score(Game& g, int32_t who, int32_t p) {
	Leader& L = LD(g, who);
	// (semaphore byte0 & 0x40, frame stepping suspended, is never set during the simulation)
	if (!(g.frame == 0 || p != 0 || (who + g.frame) % 10 == 0)) return;
	L.score_explored = 0;
	if (L.flags & 0x1800000u) {
		compute_unit_score(g, who);
		L.flags &= ~0x800000u;
	}
	compute_build_score(g, who);
	compute_economy_score(g, who);
	L.score_pop = 0;
	if (L.flags & leaderflag::kNewTech) {
		compute_unit_upgrades_score(g, who);
		compute_research_score(g, who);
		L.flags &= ~leaderflag::kNewTech;
	}
	const int32_t terr = g.world_total_land != 0 ? L.territory * 1000 / g.world_total_land : 0;
	L.score_territory = terr;
	if (get_armageddon(g) <= g.armageddon) {
		L.score_economy = L.score_buildings = L.score_territory = L.score_units = L.score_explored = 0;
		L.score_combat = L.score_wonders = L.score_research = L.score_upgrades = L.score_pop = L.score = 0;
		return;
	}
	L.score = L.score_combat + L.score_wonders + L.score_research + L.score_upgrades + L.score_economy +
	          L.score_buildings + L.score_units + terr + L.score_pop + L.score_explored;
}

}  // namespace ron::sim
