// Markets, buying and selling, tribute (docs/economy-spec.md 5.4-5.6). No game_random draws, no floats.
#include <algorithm>

#include "sim/ai.hpp"
#include "sim/economy.hpp"
#include "sim/production_internal.hpp"

namespace ron::sim {

using prod::tb;

namespace {

Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }
const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }
bool valid_good(int32_t k) { return k >= 0 && k < 6; }
bool valid_leader(int32_t w) { return w >= 0 && w < 8; }

// LeaderData::has_market @6d5410: a MARKET exists and a live own-founded city has CITY_HAS_MARKET.
bool has_market(const Game& g, int32_t who) {
	const Leader& l = LD(g, who);
	if (l.num_buildings[size_t(ptype::kMarket - kBuilds)] == 0) return false;
	const auto& cl = g.cities[size_t(who)];
	for (int32_t c = 0; c < l.city_mark && size_t(c) < cl.size(); c++) {
		const City& ct = cl[size_t(c)];
		if ((ct.city_flags & cityflag::kValid) && ct.race == who && (ct.city_flags & cityflag::kHasMarket)) return true;
	}
	return false;
}

// LeaderData::get_nuke_embargo @6d52c0 (waived by SPACEPROGRAM). TODO(nukes): no nukes are simulated.
bool nuke_embargo(const Game& g, int32_t who) {
	(void)g;
	(void)who;
	return false;
}

}  // namespace

// ---------------------------------------------------------------- prices (5.4)

void calc_market_prices(const Game& g, int32_t who, int32_t k, int32_t* buy, int32_t* sell) {
	// LeaderData::calc_market_prices @6dc2a0
	const Market& m = g.market;
	int32_t s = m.level[size_t(k)] + m.flux[size_t(k)];
	int32_t b = 2 * m.level[size_t(k)] + m.flux[size_t(k)];
	const Leader& l = LD(g, who);
	const int32_t nub = K("nubian_market_prices");
	if (nub != 0 && tb(g, who, 4)) {
		s += nub;
		b -= nub;
	} else if (((l.rare[1] >> 3) & 1) || ((l.rare_conquest[1] >> 3) & 1)) {  // AMBER (good 17)
		s += K("amber_market");
		b -= K("amber_market");
	}
	if (g.ctw) {
		const int32_t n = l.num_bonus_cards[8];
		if (n != 0) {
			for (int32_t i = 0; i < n; i++) b = (b * 100) / (K("ctw_market_bonus_buy") + 100);
			s += ((K("ctw_market_bonus_sell") * s) / 100) * n;
		}
	}
	s = std::max(s, 1);
	b = std::max(b, K("market_basement") * 2);
	b = std::max(b, s + 10);
	if (has_wonder(g, who, ptype::kSupercollider)) {
		const int32_t sb = K("super_buy"), ss = K("super_sell");
		b = std::min(b, sb);
		s = std::min(s, sb - 1);
		b = std::max(b, ss + 1);
		s = std::max(s, ss);
	}
	if (K("russian_communism") != 0 && tb(g, who, 0xd) && l.age > 4) {
		b = 100;
		s = 100;
	}
	*buy = b;
	*sell = s;
}

bool can_buy_sell(const Game& g, int32_t who) {
	// LeaderData::can_buy_sell @6d53e0: has_bonus(BUY_SELL) (Nubians, or the tech) and has_market.
	if (!valid_leader(who)) return false;
	if (!(tb(g, who, 4) || has_preq(g, who, etype::kBuySell))) return false;
	return has_market(g, who);
}

// ---------------------------------------------------------------- buy / sell (5.5)

bool do_buy(Game& g, int32_t who, int32_t k) {
	// Leader::do_buy @6cfbd0: 1 = failed.
	int32_t buy = 0, sell = 0;
	calc_market_prices(g, who, k, &buy, &sell);
	Leader& l = LD(g, who);
	if (l.bucket[2] < buy) return true;
	l.bucket[2] -= buy;
	l.escrow[2] = std::max(0, l.escrow[2] - 100);  // -100, not -buy (sic)
	l.bucket[size_t(k)] += 100;
	g.market.level[size_t(k)] += K("market_supply_demand");
	return false;
}

bool do_sell(Game& g, int32_t who, int32_t k) {
	// Leader::do_sell @6cfc60: 1 = failed.
	int32_t buy = 0, sell = 0;
	calc_market_prices(g, who, k, &buy, &sell);
	Leader& l = LD(g, who);
	if (l.bucket[size_t(k)] < 100) return true;
	l.bucket[size_t(k)] -= 100;
	l.escrow[size_t(k)] = std::max(0, l.escrow[size_t(k)] - 100);
	l.bucket[2] += sell;
	g.market.level[size_t(k)] = std::max(0, g.market.level[size_t(k)] - K("market_supply_demand"));
	return false;
}

void action_buy(Game& g, int32_t who, int32_t k, int32_t flags) {
	// CommandPackage::process_buy @946a20 -> Leader::action_buy @6cfa20 (the leader and good are not validated by
	// the original; here an out-of-range command is dropped).
	if (!valid_leader(who) || !valid_good(k)) return;
	const int32_t n1 = (flags & 1) * 4 + 1;
	int32_t n = n1;
	if (flags & 4) n = n1 == 1 ? 10 : 100;
	if (flags & 2) n = 99999;
	if (!can_buy_sell(g, who)) return;
	if (nuke_embargo(g, who)) return;  // (tell_embargo: console)
	for (int32_t i = 0; i < n; i++)
		if (do_buy(g, who, k)) return;
}

void action_sell(Game& g, int32_t who, int32_t k, int32_t flags) {
	// CommandPackage::process_sell @9468a0 (the loop is inlined; no x10 case).
	if (!valid_leader(who) || !valid_good(k)) return;
	int32_t n = (flags & 1) * 4 + 1;
	if (flags & 2) n = 99999;
	if (!(tb(g, who, 4) || has_preq(g, who, etype::kBuySell))) return;
	if (!has_market(g, who)) return;
	if (nuke_embargo(g, who)) return;
	for (int32_t i = 0; i < n; i++)
		if (do_sell(g, who, k)) return;
}

// ---------------------------------------------------------------- tribute (5.6)

int32_t scale_tribute(const Game& g, int32_t who, int32_t amount) {
	// LeaderData::scale_tribute @6d5240 (on the giver)
	int32_t p = LD(g, who).epoch[2] * K("commerce_tribute") + K("base_tribute");
	if (p < 1) p = 1;
	else if (p > 100) return amount;
	if (p != 100) {
		int32_t v;
		if (amount < 20) v = amount * p;
		else if (amount < 100) v = amount * p + 50;
		else v = amount * p + 99;
		amount = v / 100;
	}
	return amount;
}

void clear_agree(Game& g, int32_t a, int32_t b) {
	// Leader::clear_agree(b) @6d1af0 on leader a
	Leader& A = LD(g, a);
	Leader::Deal& d = A.dip[size_t(b)];
	if (d.agree == 1) {
		for (int32_t k = 0; k < 6; k++) {
			if (d.dows[size_t(k)] != 0) {
				const int32_t r = std::min(d.dows[size_t(k)], A.tributes[size_t(k)]);
				A.tributes[size_t(k)] -= r;
				bucket_add(g, a, k, r);
			}
			if (d.offers[size_t(k)] > 0) {
				const int32_t r = std::min(d.offers[size_t(k)], A.tributes[size_t(k)]);
				A.tributes[size_t(k)] -= r;
				bucket_add(g, a, k, r);
			}
		}
		d.dows.fill(0);
	}
	d.agree = 0;
	d.any_offer = 0;
}

void action_offer(Game& g, int32_t who, int32_t whom, int32_t k, int32_t n) {
	// Leader::action_offer(whom, good, amount) @6d1780 (TRIBUTE: +amount, DEMAND_TRIBUTE: -amount)
	if (!valid_leader(who) || !valid_leader(whom) || !valid_good(k)) return;
	Leader& A = LD(g, who);
	// (IFaceDiploNeg::check_click_stamp: UI)
	if (n > 0 && A.bucket[size_t(k)] < A.dip[size_t(whom)].offers[size_t(k)] + n) return;  // (invalid-order sound)
	A.dip[size_t(whom)].any_offer = 1;
	clear_agree(g, who, whom);  // (resets any_offer again: quirk)
	clear_agree(g, whom, who);
	A.dip[size_t(whom)].offers[size_t(k)] += n;
	LD(g, whom).dip[size_t(who)].offers[size_t(k)] -= n;
	{  // the tool log (ui-port 11): 13 tribute offered, 14 tribute demanded
		GameEvent e;
		e.frame = g.frame, e.kind = n > 0 ? 13 : 14, e.who = who, e.other = whom, e.value = k;
		e.goods[size_t(k)] = n > 0 ? n : -n;
		g.events.push_back(e);
	}
}

void action_clear_tributes(Game& g, int32_t who, int32_t whom) {
	// Leader::action_clear_tributes @6d1690
	if (!valid_leader(who) || !valid_leader(whom)) return;
	clear_agree(g, who, whom);
	clear_agree(g, whom, who);
	// (the "tributes cleared" chat line for the local player: presentation)
	LD(g, who).dip[size_t(whom)].offers.fill(0);
	LD(g, whom).dip[size_t(who)].offers.fill(0);
}

namespace {

void deal_clear_all(Leader::Deal& d) {
	d.treaty = -1;
	d.any_offer = 0;
	d.agree = 0;
	d.offers.fill(0);
	d.attacks.fill(0);
	d.dows.fill(0);
}

}  // namespace

void action_respond(Game& g, int32_t who, int32_t whom, int32_t mode) {
	// Leader::action_respond(whom, mode) @6d03c0 (ai-spec 10.1.9): 0 withdraw an agreement, 1 accept, else reject.
	// (notify_deal and the chat lines are presentation.)
	if (!valid_leader(who) || !valid_leader(whom)) return;
	Leader& A = LD(g, who);
	Leader& B = LD(g, whom);
	A.tribute_demanded[size_t(whom)] = 0;
	A.counteroffer[size_t(whom)] = 0;
	if (mode == 0) {
		clear_agree(g, who, whom);
		return;
	}
	if (mode != 1) {
		// Reject. (A human rejecting an AI ally's attack request runs a retaliation loop that tests B.is_enemy(B):
		// never true, so it is not ported.)
		clear_agree(g, who, whom);
		clear_agree(g, whom, who);
		deal_clear_all(B.dip[size_t(who)]);
		deal_clear_all(A.dip[size_t(whom)]);
		return;
	}
	Leader::Deal& d = A.dip[size_t(whom)];
	if (d.agree != 1) {
		std::array<int32_t, 6> dow{}, give{};
		for (int32_t k = 0; k < 6; k++) {
			dow[size_t(k)] = 0;
			if (!type_avail(g, who, k, 1) || !type_avail(g, whom, k, 1)) continue;
			give[size_t(k)] = std::max(0, d.offers[size_t(k)]);
			bool any = false;
			for (int32_t p = 0; p < 8; p++) {
				if (!(g.leaders[size_t(p)].flags & leaderflag::kValid) || d.attacks[size_t(p)] == 0) continue;
				if (is_enemy(g, who, p)) continue;
				// The declare-war cost: type vslot +0x78 of DECLARE_WAR + p (get_cost), x2 against an ally, humans only
				// (the cost is assigned per third party: the last one wins, quirk).
				const int32_t c = get_cost(g, kDeclareWar + p, k, who, -1, -1, 0, 1, -1);
				dow[size_t(k)] = c * ((is_ally(g, who, p) ? 1 : 0) + 1) * ((A.flags & leaderflag::kHuman) ? 1 : 0);
				any = true;
			}
			(void)any;
			if (A.bucket[size_t(k)] < dow[size_t(k)] + give[size_t(k)]) return;  // (console message)
		}
		for (int32_t k = 0; k < 6; k++) {
			if (!type_avail(g, who, k, 1) || !type_avail(g, whom, k, 1) || d.offers[size_t(k)] <= 0) continue;
			const int32_t v = give[size_t(k)];
			A.bucket[size_t(k)] = std::max(0, A.bucket[size_t(k)] - v);
			A.escrow[size_t(k)] = std::max(0, A.escrow[size_t(k)] - v);
			A.tributes[size_t(k)] += v;
		}
		for (int32_t k = 0; k < 6; k++) {
			if (dow[size_t(k)] == 0) continue;
			const int32_t v = dow[size_t(k)];
			A.bucket[size_t(k)] = std::max(0, A.bucket[size_t(k)] - v);
			A.escrow[size_t(k)] = std::max(0, A.escrow[size_t(k)] - v);
			A.tributes[size_t(k)] += v;
			d.dows[size_t(k)] = v;
		}
		d.agree = 1;
		B.agendas[size_t(who)] |= 4;  // the partner evaluates us urgently (Leader::diplomacy 10.1.2)
	}
	for (int32_t p = 0; p < 8; p++) {
		if (p == who || p == whom || A.dip[size_t(whom)].attacks[size_t(p)] == 0) continue;
		if (is_ally(g, who, p) || is_ally(g, whom, p)) {
			clear_agree(g, who, whom);
			clear_agree(g, whom, who);
			deal_clear_all(B.dip[size_t(who)]);
			deal_clear_all(A.dip[size_t(whom)]);
			return;
		}
	}
	if (B.dip[size_t(who)].agree != 1) {
		// Not agreed by the partner yet: only a pure gift (no treaty, no attacks, nothing asked) executes now.
		bool pending = d.treaty != -1;
		for (int32_t p = 0; p < 8 && !pending; p++) pending = d.attacks[size_t(p)] != 0;
		for (int32_t k = 0; k < 6 && !pending; k++) pending = B.dip[size_t(who)].offers[size_t(k)] > 0;
		if (pending) return;
	}
	if (d.treaty == 2 && !(1 - num_team_members(g, who, 1) + num_allies(g, who) == 0 &&
	                       1 - num_team_members(g, whom, 1) + num_allies(g, whom) == 0)) {
		clear_agree(g, who, whom);
		clear_agree(g, whom, who);
		deal_clear_all(B.dip[size_t(who)]);
		deal_clear_all(A.dip[size_t(whom)]);
		return;
	}
	for (int32_t k = 0; k < 6; k++) {
		bucket_add(g, who, k, A.dip[size_t(whom)].dows[size_t(k)]);
		bucket_add(g, whom, k, B.dip[size_t(who)].dows[size_t(k)]);
		A.dip[size_t(whom)].dows[size_t(k)] = 0;
		B.dip[size_t(whom)].dows[size_t(k)] = 0;  // B.dip[B], not B.dip[A] (sic)
		if (!type_avail(g, who, k, 1) || !type_avail(g, whom, k, 1)) continue;
		const int32_t a_gives = std::max(0, A.dip[size_t(whom)].offers[size_t(k)]);
		const int32_t b_gives = std::max(0, B.dip[size_t(who)].offers[size_t(k)]);
		bucket_add(g, who, k, scale_tribute(g, whom, b_gives));
		B.tributes[size_t(k)] -= b_gives;
		B.tribute_sent += b_gives;
		A.tribute_received += scale_tribute(g, whom, b_gives);
		leader_consider_tribute(g, who, whom, b_gives, k);
		bucket_add(g, whom, k, scale_tribute(g, who, a_gives));
		A.tributes[size_t(k)] -= a_gives;
		A.tribute_sent += a_gives;
		B.tribute_received += scale_tribute(g, who, a_gives);
		leader_consider_tribute(g, whom, who, a_gives, k);
		if (A.tributes[size_t(k)] < 0) A.tributes[size_t(k)] = 0;
		if (B.tributes[size_t(k)] < 0) B.tributes[size_t(k)] = 0;
	}
	const int32_t t = A.dip[size_t(whom)].treaty;
	if (t != -1) {  // treaties only upgrade
		leader_set_diplo(g, who, whom, std::max(t, A.diplos(whom)));
		leader_set_diplo(g, whom, who, std::max(t, B.diplos(who)));
		if (t == 1) {
			A.made_peace[size_t(whom)] = g.frame;
			B.made_peace[size_t(who)] = g.frame;
		}
		if (is_neutral(g, who) || is_neutral(g, whom)) {  // neutral (team 8) seats: team members follow
			for (int32_t k = 0; k < 8; k++) {
				if (k == who || k == whom || !(LD(g, k).flags & leaderflag::kValid)) continue;
				if (!is_team_p(g, k, who, 0)) {
					if (is_team_p(g, k, whom, 0) && LD(g, k).diplos(who) < B.diplos(who))
						leader_set_diplo(g, k, who, B.diplos(who));
				} else if (!is_team_p(g, k, whom, 0) && LD(g, k).diplos(whom) < A.diplos(whom)) {
					leader_set_diplo(g, k, whom, A.diplos(whom));
				}
			}
		}
	}
	if (war_allowed(g)) {  // joint attacks: hire stamps both ways, then each side not yet at war declares
		for (int32_t k = 0; k < 8; k++) {
			if (k == who || k == whom || A.dip[size_t(whom)].attacks[size_t(k)] == 0) continue;
			A.hire_stamp[size_t(k)] = g.frame;
			B.hire_stamp[size_t(k)] = g.frame;
			A.hire_who[size_t(k)] = whom;
			B.hire_who[size_t(k)] = who;
			if (!is_enemy(g, who, k)) leader_action_declare(g, who, k, 0, (A.flags & leaderflag::kHuman) ? 0 : 1, 0);
			if (!is_enemy(g, whom, k)) leader_action_declare(g, whom, k, 0, (B.flags & leaderflag::kHuman) ? 0 : 1, 0);
		}
	}
	deal_clear_all(B.dip[size_t(who)]);
	deal_clear_all(A.dip[size_t(whom)]);
}

}  // namespace ron::sim
