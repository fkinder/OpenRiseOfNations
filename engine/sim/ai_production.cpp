// The AI production pipeline (docs/ai-spec.md 3.1, 6.0-6.4, 6.7-6.10): Leader::production_ai and its steps
// (production_ai_setup with market_speculation, found_cities, research_techs, upgrade_units), the make list, purchases
// (make_stuff, use_market, make_this) and the produce_* functions except produce_building (ai_place.cpp).
//
// Evidence (local, gitignored): re/decomp/ai/Leader_production_ai@006c1960.c, Leader_production_ai_setup,
// Leader_found_cities, Leader_research_techs, Leader_upgrade_units, Leader_make_stuff, Leader_use_market,
// Leader_make_this, Leader_produce_*, Leader_check_income, Leader_unit_prod_value, re/decomp/ai2/MakeList_*,
// re/decomp/ai_port/Leader_tech_avail, the disassembly in re/decomp/ai/asm and re/decomp/ai_port/asm.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "script/bhs_vm.hpp"
#include "sim/ai_internal.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/orders.hpp"
#include "sim/script_host.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace ai;
using prod::is_t;
using prod::tb;

namespace {

bool is_human_seat(const Leader& l) { return (l.flags & 0xc) == leaderflag::kHuman; }
bool is_city_t(const Game& g, int32_t t) { return is_build_type(t) && type_is(g.type(t), ptype::kVillage, false); }
int32_t cost_of(const Game& g, int32_t t, int32_t good, int32_t who, int32_t o, int32_t loc) {
	return get_cost(g, t, good, who, o, loc, 0, 1, -1);
}
// The building's danger (max(0, danger[who] at its position)).
int32_t bdanger(const Game& g, int32_t who, const Object& b) { return std::max(0, danger_at(g, who, b.x, b.y)); }

// Leader::tech_avail @6da060.
int32_t tech_avail(const Game& g, int32_t who, int32_t t) {
	const Type& T = g.type(t);
	if (is_tech_type(t) && (T.leader_off & (1u << (who & 31)))) return 0;
	if (has_tech(g, who, t)) return 0;
	if (researching(g, who, t, -1, false, false)) return 0;
	if (is_gov_type(t)) {
		const int32_t pair = ((t - ptype::kGovBase) ^ 1) + ptype::kGovBase;
		if (has_tech(g, who, pair) || researching(g, who, pair, -1, false, false)) return 0;
	}
	return type_avail(g, who, t, 1);
}

// LeaderData::get_gov @6d6a20 (the government bonus held, -1 none).
int32_t get_gov_ai(const Game& g, int32_t who) {
	if (has_preq(g, who, 804)) return 0x273;  // SOCIALISM_1
	if (has_preq(g, who, 805)) return 0x274;  // CAPITALISM_1
	if (has_preq(g, who, 800)) return 0x271;  // MONARCHY_1
	if (has_preq(g, who, 802)) return 0x272;  // DEMOCRACY_1
	if (has_preq(g, who, 794)) return 0x26f;  // DESPOTISM_1
	if (has_preq(g, who, 797)) return 0x270;  // REPUBLIC_1
	return -1;
}

// Type::find_preq: an effective preq slot of `o` equals x.
bool find_preq_live(const Game& g, int32_t o, int32_t x, int32_t who) {
	for (int32_t i = 0; i < num_preq(o); i++) {
		if (get_preq(g, o, i, who) == x) return true;
	}
	return false;
}

int32_t owned_w(const Game& g, int32_t who, int32_t w) {  // num_buildings[w] + get_buildings(w.to, ...)
	if (!is_build_type(w)) return 0;
	const Leader& L = LD(g, who);
	int32_t n = L.num_buildings[size_t(w - kBuilds)];
	const int32_t to = g.type(w).to;
	if (to >= 0 && is_build_type(to)) n += get_buildings(g, who, to, L.num_buildings[size_t(to - kBuilds)]);
	return n;
}

// City::generate_name's string is file-only in cities.cpp, so the cities of a generated game (new_random_game) reach the AI
// without a name; the production scripts address cities by the string find_city_with_num returns and city_index("") is
// always the first city (the second city is then "the capital" to economic.bhs, whose farm count never reaches its goal).
// The original's names come from the tribe's <CITIES> list (not read): "<tribe> City N" stands in. Names are outside the
// digest and the draws.
void name_unnamed_cities(Game& g, int32_t who) {
	const Leader& L = LD(g, who);
	auto& list = g.cities[size_t(who)];
	const int32_t n = std::min<int32_t>(L.city_mark, int32_t(list.size()));
	const auto& tr = g.rules->tribes;
	const std::string tn = L.tribe >= 0 && size_t(L.tribe) < tr.size() ? bhs::to_utf8(tr[size_t(L.tribe)].eng_name) : std::string("Tribe");
	for (int32_t c = 0; c < n; c++) {
		City& ct = list[size_t(c)];
		if (ct.valid() && ct.name.empty()) ct.name = tn + " City " + std::to_string(c + 1);
	}
}

}  // namespace

// ---------------------------------------------------------------- Leader::production_ai

void leader_production_ai(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	if (((L.flags & leaderflag::kHuman) && !(L.flags & leaderflag::kCoopSolo)) || g.ai_off || (L.flags2 & 4)) {
		L.production_ai = 0;
		return;
	}
	name_unnamed_cities(g, who);
	L.effective_pop = queued_units(g, who) + L.control + 1;
	static const bool trace = std::getenv("RON_AI_TRACE") != nullptr;  // developer aid (no effect on the simulation)
	if (trace) {
		std::fprintf(stderr, "ai f%d L%d state %d make0 %d/%d free %d gath %d sites", g.frame, who, L.production_ai,
		             L.make_list[0].t, L.make_list[0].val, L.free_peasants, L.gatherers);
		std::fprintf(stderr, " diff %d rush %d end %d |", get_diff(g, who), g.info.rush_rules, g.info.ending_technology);
		for (const MakeObject& m : L.make_list) std::fprintf(stderr, " %d:%d", m.t, m.val);
		std::fprintf(stderr, " | sites");
		for (const Site& s : L.sites) std::fprintf(stderr, " %d", s.val);
		for (int32_t c = 0; c < L.city_mark; c++) {
			const City& C = g.cities[size_t(who)][size_t(c)];
			std::fprintf(stderr, " | c%d land %d filled %d space %d,%d,%d ocean %d free %d busy %d gath %d ter %d %d %d %d", c,
			             C.land, C.filled, C.space[0], C.space[1], C.space[2], C.ocean, C.free, C.busy, C.gatherers,
			             C.ter[0], C.ter[1], C.ter[2], C.ter[4]);
		}
		std::fprintf(stderr, "\n");
	}
	const bool sr8 = g.info.starting_resources == 8;
	if (L.production_ai == 1 && (L.prod_script_on == 0 || sr8)) L.production_ai = 2;
	const int32_t state = L.production_ai;
	switch (state) {
	case 1: {
		// run_script(name, who + 1, ref step, rush + 2, 5) (script-spec 6.4).
		int st = bhs::Runtime::kNotFound;
		int32_t ret = 0;
		bhs::VP step = bhs::make_int(L.prod_step, bhs::SC_VAR);
		if (g.scripts) {
			st = g.scripts->run(bhs::to_u16(L.prod_script), {bhs::make_int(who + 1), step, bhs::make_int(L.pers.rush + 2),
			                                                 bhs::make_int(5)});
			if (st == bhs::Runtime::kOk) ret = g.scripts->rt.ret_int();
		}
		if (st == bhs::Runtime::kOk) {
			L.prod_step = bhs::value_int(step.get());
			if (ret == 1) {  // BLOCK_ON_THIS
				L.production_ai = 0;
				return;
			}
			if (ret == 3) L.prod_script_on = 0;  // SCRIPT_DONE
		} else {
			L.prod_script_on = 0;
		}
		L.production_ai++;
		return;
	}
	case 2:
		L.production_ai = 3;
		production_ai_setup(g, who);
		make_list_clear(g, who);
		return;
	case 3:
		L.production_ai = 4;
		found_cities(g, who);
		break;
	case 4:
		L.production_ai = 5;
		research_techs(g, who);
		break;
	case 5:
		L.production_ai = 6;
		upgrade_units(g, who);
		break;
	case 6:
	case 9:
		L.production_ai = state + 1;
		create_units(g, who);
		break;
	case 7:
		L.production_ai = 8;
		create_buildings(g, who);
		break;
	case 8:
		L.production_ai = 9;
		if (make_stuff(g, who) != 0) return;
		if (sr8) return;
		L.production_ai = 0;
		return;
	case 10:
		L.production_ai = 11;
		create_buildings(g, who);
		return;
	case 11:
		make_stuff(g, who);
		L.production_ai = 0;
		return;
	default:
		L.production_ai = 0;
		return;
	}
	if (sr8) {
		make_stuff(g, who);
		make_list_clear(g, who);
	}
}

// ---------------------------------------------------------------- production_ai_setup, market_speculation

void production_ai_setup(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	int32_t lo_rate = 99999999, hi_rate = -99999999;
	L.shortages = 0;
	L.worst_good = 0;
	L.best_good = 0;
	if (g.info.starting_resources == 8) {
		for (int32_t& e : L.econ) e |= 8;
		return;
	}
	if (get_diff(g, who) < 3) {
		const int32_t A = kAges + L.age;
		int32_t cap = 0;
		bool clamp = true;
		if (is_age_type(A)) {
			for (int32_t k = 0; k < 6; k++) {
				if (type_avail(g, who, k, 1) == 0) continue;
				cap = std::max(cap, cost_of(g, A, k, who, -1, -1));
			}
			if (cap < 300) cap = 300;
			const int32_t d = get_diff(g, who);
			if (d == 0) cap = (cap * 3) / 2;
			else if (d == 1) cap = cap * 2;
			else if (d == 2) cap = (cap * 5) / 2;
			if (cap == 0) clamp = false;
		} else {
			cap = 11000;
		}
		if (clamp) {
			for (int32_t k = 0; k < 6; k++) {
				const int32_t b = L.bucket[size_t(k)];
				if (cap < b) {
					L.escrow[size_t(k)] = (L.escrow[size_t(k)] * cap) / b;
					L.bucket[size_t(k)] = cap;
				}
			}
		}
	}
	for (int32_t k = 0; k < 6; k++) {
		L.econ[size_t(k)] = 0;
		const int32_t cap = get_mod_resource_cap(g, who, k);
		L.rate[size_t(k)] = std::min(cap, L.income[size_t(k)]) / 16;
		if (type_avail(g, who, k, 1) == 0) continue;
		if (L.rate[size_t(k)] < lo_rate) {
			L.worst_good = k;
			lo_rate = L.rate[size_t(k)];
		}
		if (hi_rate < L.rate[size_t(k)]) {
			L.best_good = k;
			hi_rate = L.rate[size_t(k)];
		}
		if (L.rate[size_t(k)] < 30) {
			L.econ[size_t(k)] |= 1;
			L.shortages++;
		}
	}
	for (int32_t k = 0; k < 6; k++) {
		int32_t& e = L.econ[size_t(k)];
		if (type_avail(g, who, k, 1) == 0) {
			e = 0;
			continue;
		}
		const int32_t cap = get_mod_resource_cap(g, who, k);
		int32_t lo = std::min(L.city_num * 15, cap / 32);
		int32_t hi = std::min(L.city_num * 30, ((cap / 16) * 4) / 5);
		if (L.city_num > 4) {
			lo = cap / 32;
			hi = std::min(175, ((cap / 16) * 3) / 4);
		}
		lo = std::min(lo, 250);
		hi = std::min(hi, 350);
		if (k != 0 && k != 1 && L.city_num < 3) {
			lo = 20;
			hi = L.city_num * 20;
			if (e & 1) {
				e &= ~1;
				L.shortages--;
			}
		}
		if (L.income[size_t(k)] < cap) {
			if (L.rate[size_t(k)] < lo) e |= 2;
			if (L.rate[size_t(k)] < hi) e |= 4;
			if (L.rate[size_t(k)] < hi) continue;
		}
		e |= 8;
	}
	if (L.city_num <= 2) {
		auto& E = L.econ;
		const bool lakota = has_tribe_bonus(g, who, 0x13);
		if ((E[0] & 4) && !lakota) {
			E[1] &= ~4;
			for (int32_t k = 2; k < 6; k++) E[size_t(k)] &= ~4;
		} else if (E[1] & 4) {
			E[0] &= ~4;
			for (int32_t k = 2; k < 6; k++) E[size_t(k)] &= ~4;
		}
		bool go = true;
		if ((E[0] & 2) && !lakota) {
			if (E[1] & 2) E[1] = (E[1] & ~2) | 4;
		} else {
			if (!(E[1] & 2)) go = false;
			else if (E[0] & 2) E[0] = (E[0] & ~2) | 4;
		}
		if (go) {
			for (int32_t k = 2; k < 6; k++) {
				if (E[size_t(k)] & 2) E[size_t(k)] = (E[size_t(k)] & ~2) | 4;
			}
		}
	}
	market_speculation(g, who);
}

void market_speculation(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	if (!(has_tribe_bonus(g, who, 4) || has_preq(g, who, ptype::kBuySell))) return;
	if (!ai_has_market(g, who) || g.info.starting_resources == 8) return;  // (no nuke embargo: nukes not simulated)
	for (int32_t k = 0; k < 6; k++) {
		if (type_avail(g, who, k, 1) != 0 && L.escrow[size_t(k)] > 4000) L.escrow[size_t(k)] = 2000;
	}
	int32_t p = 0;
	for (int32_t k = 0; k < 6; k++) {
		if (type_avail(g, who, k, 1) == 0) continue;
		if (L.bucket[size_t(k)] < 100) p = 2;
		else if (L.bucket[size_t(k)] < 200 && p < 1) p = 1;
	}
	for (int32_t k = 0; k < 6; k++) {
		if (k == 2 || k == 3 || type_avail(g, who, k, 1) == 0) continue;
		int32_t buy = 0, sell = 0;
		calc_market_prices(g, who, k, &buy, &sell);
		if (L.bucket[size_t(k)] - L.escrow[size_t(k)] >= 2000 / (1 << p) && sell >= 75 / (p + 1)) do_sell(g, who, k);
	}
	for (int32_t k = 0; k < 6; k++) {
		if (k == 2 || k == 3 || type_avail(g, who, k, 1) == 0) continue;
		int32_t buy = 0, sell = 0;
		calc_market_prices(g, who, k, &buy, &sell);
		const int32_t b = L.bucket[size_t(k)];
		if (L.bucket[2] - L.escrow[2] >= buy && b < 2000 && buy <= 200 && (b < 500 || buy <= 25) && (b < 200 || buy <= 50) &&
		    (b < 100 || buy <= 100))
			do_buy(g, who, k);
	}
}

int32_t get_need(const Game& g, int32_t who, int32_t good) {
	const int32_t e = LD(g, who).econ[size_t(good)];
	if (e & 8) return 0;
	if (e & 1) return 4;
	if (e & 2) return 3;
	if (e & 4) return 2;
	return 1;
}

// ---------------------------------------------------------------- make list

void make_list_clear(Game& g, int32_t who) { LD(g, who).make_list.fill(MakeObject()); }

void make_me(Game& g, int32_t who, int32_t t, int32_t val, int32_t escrow, int32_t cat, int32_t city, int32_t up,
             int32_t num, int32_t wx, int32_t wy) {
	auto& M = LD(g, who).make_list;
	MakeObject it;
	it.t = t;
	it.val = val;
	it.escrow = escrow;
	it.cat = cat;
	it.city = city;
	it.up = up;
	it.o = -1;
	it.num = num;
	it.wx = wx;
	it.wy = wy;
	if (M[0].val < val) {
		M[0] = it;
		for (int32_t i = 1; i < 4; i++) {
			if (M[size_t(i)].t == t) M[size_t(i)].t = -1;
		}
	} else {
		for (int32_t i = 1; i < 4; i++) {
			if (M[size_t(i)].val <= val) {
				for (int32_t k = 3; k > i; k--) M[size_t(k)] = M[size_t(k - 1)];
				for (int32_t k = i; k < 4; k++) {
					if (M[size_t(k)].t == t) M[size_t(k)].t = -1;
				}
				M[size_t(i)] = it;
				break;
			}
			if (M[size_t(i)].t == t) break;
		}
	}
	if (cat >= 0 && cat < 11 && M[size_t(cat)].val < val) M[size_t(cat)] = it;
}

bool can_pay(Game& g, int32_t who, int32_t i) {
	const MakeObject& it = LD(g, who).make_list[size_t(i)];
	if (it.t < 0) return false;
	return it.num <= can_pay_cost(g, it.t, who, it.city, it.o, it.escrow);
}

int32_t check_income(Game& g, int32_t who, int32_t t, int32_t k, int32_t o, int32_t total, int32_t city,
                     int32_t need, int32_t* n_out) {
	const int32_t n = can_pay_cost(g, t, who, city, o, total);
	if (n_out) *n_out = n;
	int32_t m;
	if (n < need) {
		if (total == 0) return 0;
		m = 64;
	} else {
		m = 256;
	}
	if (total == 0 && g.info.starting_resources != 8) {
		const Leader& L = LD(g, who);
		for (int32_t gd = 0; gd < 6; gd++) {
			if (type_avail(g, who, gd, 1) == 0) continue;
			const int32_t c = cost_of(g, t, gd, who, o, city);
			if (c == 0) continue;
			const int32_t inc = L.income[size_t(gd)] / 16;
			if (inc < 1) {
				m /= 4;
			} else {
				const int32_t q = std::clamp(c / inc, 0, 20);
				m = ((100 - (q * k) / 256) * m) / 100;
			}
		}
	}
	return m;
}

// ---------------------------------------------------------------- found_cities

void found_cities(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	if (L.flags2 & 0x10) return;  // NO_EXPANSION
	const int32_t t = ptype::kVillage;
	bool queued = false;
	if (get_city_limit(g, who) > get_total_cities(g, who) && type_avail(g, who, t, 1) == 4 &&
	    (L.free_peasants != 0 || L.gatherers != 0)) {
		bool stop = false;
		if (!is_human_seat(L)) {
			const int32_t h = leaders_max_human_cities(g);
			if (h > 0) {
				const int32_t d = get_diff(g, who), tc = get_total_cities(g, who);
				if ((d == 0 || d == 1) && tc >= std::max(h, 2)) stop = true;
				if (d == 2 && tc >= std::max(h + 1, 2)) stop = true;
			}
		}
		if (!stop) {
			for (int32_t i = 0; i < 10; i++) {
				const Site& s = L.sites[size_t(i)];
				if (s.val <= 0) continue;
				const int32_t r = s.reg & 63;
				if (L.reg_free_peasants[size_t(r)] == 0 && L.reg_gatherers[size_t(r)] == 0) continue;
				if (g.world.sea_map >= 4 && g.info.map_style != 0x14 && L.city_num >= 2 && L.city_num <= 3 &&
				    r == L.home_reg)
					continue;
				if (blocked_site(g, t, s.wx * 0x300, s.wy * 0x300, who, -1, nullptr) != 0) continue;
				int32_t v = s.val;
				const int32_t n = uint16_t(L.reg_cities[size_t(r)]);
				if (n == 0) {
					v *= 4;
				} else {
					v = ((L.reg_pop[size_t(r)] + 1) * v) / (2 * n);
					if (!(L.strategy[size_t(r)] & 1)) v /= 2;
				}
				if (L.num_queued[size_t(t)] == 0) {
					if (L.reg_pop[size_t(r)] <= L.reg_peasants[size_t(r)]) v *= 2;
					if (n <= L.reg_free_peasants[size_t(r)]) v *= 2;
				}
				bool veto = v == 0;
				for (int32_t j = 0; j < 8 && !veto; j++) {
					const Leader& J = LD(g, j);
					if (!(J.flags & leaderflag::kValid) || (j != who && J.reg_cities[size_t(r)] == 0)) continue;
					for (int32_t o = kBuildBase; o < g.objects[size_t(j)].build_mark && !veto; o++) {
						const Build* b = g.build(j, o);
						if (!b || !b->alive() || !(b->flags & objflag::kCity)) continue;
						int32_t dx = std::abs(wcoord_of(b->x) - s.wx), dy = std::abs(wcoord_of(b->y) - s.wy);
						const int32_t mx = std::max(dx, dy), mn = std::min(dx, dy);
						int32_t d = 0;
						if (mx != 0) d = mn < 60000 ? (mn * mn) / (2 * mx) + mx : (mx + 2 * mn) / 2;
						if (d < 5) veto = true;
					}
				}
				if (veto) continue;
				if (L.reg_land[size_t(r)] < n) v *= 2;
				else if (L.reg_land[size_t(r)] < 2 * n) v = (v * 3) / 2;
				int32_t lim = L.epoch[1] + 1;
				if (tb(g, who, 3) && L.epoch[1] != 0) lim += K("bantu_city_limit");
				if (has_wonder(g, who, ptype::kPyramids)) lim += K("pyramids_city_limit");
				if (get_total_cities(g, who) < lim) {
					queued = true;
					v *= 30;
				}
				if (L.pop_cap < 200) {
					const int32_t n5 = (L.city_num + L.village_num) / 5;
					if (L.effective_pop > (3 * L.pop_cap) / 4) v = ((n5 + 2) * v) / (n5 + 1);
					if (L.effective_pop > L.pop_cap - 4) v = (v * 3) / 2;
				}
				if ((L.strategy[size_t(r)] & 5) == 1) {
					const int32_t C = L.city_num + L.village_num, P = L.peasants;
					if (P >= 5 * C) v *= 2;
					else if (P >= 3 * C) v = (v * 3) / 2;
					if (P >= 2 * C) {
						if (C < 4) v *= 2;
						else if (C < 8) v = (v * 3) / 2;
					}
				}
				int32_t w = v * 10;
				if (w < 0) w = 999999;
				if (danger_at_w(g, who, s.wx, s.wy) > 0) w /= 3;
				if (g.world.w(s.wx, s.wy).who != who) w /= 3;
				const int32_t m = can_pay_cost(g, t, who, -1, -1, 1) < 1 ? 64 : 256;
				make_me(g, who, t, ((w / 256) * m / 256) / (L.num_queued[size_t(t)] + 1), 1, 9, -1, 0, 1, s.wx, s.wy);
			}
		}
	}
	if (queued && L.make_list[0].t != -1 && make_stuff(g, who) != 0) make_list_clear(g, who);
}

// ---------------------------------------------------------------- research_techs

void research_techs(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	if (g.info.elimination == 1) {
		int32_t c = -1, w = -1;
		find_capital(g, who, &c, &w, -1, -1);
		if (w != who) return;
	}
	const int32_t lowest = get_lowest_epoch(g, who);
	int32_t over = 0;
	for (int32_t k = 0; k < 6; k++) over += (type_avail(g, who, k, 1) != 0 && L.over_cap[size_t(k)] != 0) ? 1 : 0;
	const int32_t diff = get_diff(g, who);
	const bool human_pacing = !is_human_seat(L) && !(L.flags & leaderflag::kCoopSolo);
	for (int32_t t = kAges; t < 629; t++) {
		if (tech_avail(g, who, t) <= 3) continue;
		const Type& T = g.type(t);
		const int32_t w = T.where;
		if (w < 0 || !is_build_type(w) || owned_w(g, who, w) == 0) continue;
		int32_t B = ((L.pop * 200) / std::max(1, L.city_num + L.village_num)) * L.infra_mod / 256;
		const int32_t cat = T.cat;
		const int32_t ck = cost_of(g, t, 3, who, -1, -1);
		const bool kn_avail = type_avail(g, who, 3, 1) != 0;
		if (is_age_type(t)) {
			if (kn_avail && ck > (4 * L.bucket[3]) / 3) continue;
			if (human_pacing) {
				const int32_t h = leaders_max_human_age(g);
				if (h >= 0) {
					const int32_t a = t - kAges;
					if (diff == 0 || diff == 1) {
						if (a > h - 1) continue;
						const int32_t s = leaders_best_human_age_stamp(g, t);
						if (s >= 0 && g.frame - s < (who + (diff == 0 ? 16 : 8)) * 225) continue;
					} else if (diff == 2) {
						if (a > h) continue;
						const int32_t s = leaders_best_human_age_stamp(g, t - 1);
						if (s >= 0 && g.frame - s < (who + 20) * 225) continue;
					}
				}
			}
			B *= lowest > t - kAges ? 600 : 10;
		} else if (is_epoch_type(t)) {
			if (kn_avail && ck > (4 * L.bucket[3]) / 3) continue;
			if (L.age < int32_t(g.info.ending_technology) && t != ptype::kMilitaryBase && !(cat == 3 && L.tribe == 9)) {
				const int32_t next = kAges + L.age;
				if (is_age_type(next)) {
					int32_t allow = techs_per_age(g, who, next);
					if (cat == 0 && L.effective_pop >= (7 * L.pop_cap) / 8) allow += 4;
					if (cat == 2 && over > 1) allow += 4;
					if (diff == 0 || diff == 1) {
						if (!(L.epochs < allow + 2 && L.epoch[size_t(cat & 3)] < allow)) continue;
					} else if (diff == 2) {
						if (!(L.epochs < allow + 6 && L.epoch[size_t(cat & 3)] < allow + 1)) continue;
					}
				}
			}
			if (L.epoch[size_t(cat & 3)] == lowest) B *= 10;
			else if (L.epoch[size_t(cat & 3)] > lowest + 1) continue;
			if ((cat == 1 || cat == 2) && (g.info.victory == 8 || g.info.victory == 9)) B *= 30;
			if (g.world.sea_map > 1 && find_preq_live(g, 686, t, who)) B *= 30;
			if ((t == 559 || t == 566) && has_tech(g, who, ptype::kClassicalAge)) B *= 20;
		}
		if (ck < L.bucket[3] - 100) B *= 10;
		const auto& ai = T.ai;
		int32_t S = 1 + ai[0] / 10;
		for (int32_t i = 1; i <= 10; i++) S += ai[size_t(i)];
		const int32_t ts = g.info.team_style;
		S += (ts == 0 || ts == 8 || ts == 11) ? ai[10] : -ai[10];
		if (L.full_cities > (L.city_num + L.village_num) / 2) S += 2 * ai[5];
		else if (L.full_cities != 0) S += ai[5];
		if (L.my_team_terr < L.other_team_terr) {
			S += ai[5];
			if (L.my_team_terr < L.min_other_team_terr) S += ai[5];
		}
		const int32_t P = L.active_wars == 0 ? ai[5] + ai[1] : ai[0] / 3;
		const int32_t sm = g.world.sea_map;
		const int32_t Q = sm == 0 ? -ai[2] : ai[2] * sm * sm * sm;
		int32_t V = (P + Q + S) * B;
		if (cat == 0) {
			V /= 10;
			if (L.pop_cap < 200) {
				if (L.effective_pop > (5 * L.pop_cap) / 6) V *= 20;
				else if (L.epoch[0] > 1) V /= 10;
			}
		} else if (cat == 2) {
			if (L.epoch[2] == 0) V *= 3;
			if (is_epoch_type(t) || is_age_type(t)) {
				int32_t n = 0;
				for (int32_t k = 0; k < 6; k++) {
					if (k == 3 || type_avail(g, who, k, 1) == 0) continue;
					if (L.income[size_t(k)] >= (9 * get_mod_resource_cap(g, who, k)) / 10) n++;
				}
				V = n == 0 ? V / 10 : V * n * n;
			}
		} else if (diff >= 4 && cat == 3) {
			int32_t gd = -1;
			if (t >= 612 && t <= 614) gd = 0;
			else if (t >= 609 && t <= 611) gd = 1;
			else if (t >= 620 && t <= 622) gd = 4;
			else if (t >= 615 && t <= 618) gd = 3;
			if (gd >= 0) {
				const int32_t e = L.econ[size_t(gd)];
				if (e & 4) V = 2 * V;
				if (e & 2) V *= 4;
				if (e & 1) V *= 8;
			}
			const int32_t d = L.epoch[3] - T.age;
			if (d > 0) V *= (d + 1) * d;
		}
		if (g.info.victory == 9) V *= is_age_type(t) ? 9 : 3;
		int32_t slot = cat == 0 ? 10 : cat == 1 ? 9 : cat == 2 ? 4 : 8;
		if (w == ptype::kTower) {
			if (diff < 2) continue;
			V *= 100;
			slot = 6;
		}
		if (w == ptype::kTemple) {
			V *= 4;
			if (t >= 589 && t <= 592) V *= 100;
		}
		if (w == ptype::kLibrary) {
			const int32_t mx = std::max({0, L.epoch[0], L.epoch[1], L.epoch[2], L.epoch[3]});
			V = (mx * V + 1) / (L.epoch[size_t(cat & 3)] + 1);
		}
		if (w == ptype::kUniversity) V *= 1000;
		if (w == ptype::kSenate) {
			const int32_t raid = L.pers.raid;
			const bool civil = raid < 0 || (raid == 0 && (rnd_raw(g) & 1) != 0);
			// GOV_EMPIRE 623, MONARCHY 625, SOCIALISM 627 are martial; REPUBLIC 624, DEMOCRACY 626, CAPITALISM 628 civil.
			if (civil && (t == 623 || t == 625 || t == 627)) continue;
			if (!civil && (t == 624 || t == 626 || t == 628)) continue;
			V *= rnd_raw(g) % 100;
			if (get_gov_ai(g, who) < 0) V *= 20;
		}
		{
			const int32_t a = 4, b = 3;  // the time factors (constant: the stamps are past frames)
			V = (b * ((a * V) / 2)) / 2;
		}
		if (L.shortages == 0) V *= 2;
		int32_t C = 0, M = 0;
		for (int32_t k = 0; k < 6; k++) {
			const int32_t c = T.costs[size_t(k)];
			C += c;
			if (type_avail(g, who, k, 1) == 0 && !has_preq(g, who, k)) M += 2 * c;
		}
		if (M != 0) {
			V = (2 * C - M) * V / (2 * C);
			if (V < 0) V = 9999999;
		}
		if (L.wonder_mod != 0) V /= 2;
		V = check_income(g, who, t, 0x400, -1, L.wonder_mod == 0 ? 1 : 0, -1, 1, nullptr) * V / 256;
		if (V < 0) V = 9999999;
		make_me(g, who, t, V, 1, slot, -1, 0, 1, 0, 0);
	}
}

// ---------------------------------------------------------------- upgrade_units

int32_t unit_prod_value(const Game& g, int32_t who, int32_t t) {
	const Type& T = g.type(t);
	if (!(T.role & 0x10000) || (T.unit_flags & 0x20000)) return 0;
	const int32_t diff = get_diff(g, who);
	int64_t sum = 0;
	int32_t n = 0;
	for (int32_t j = 0; j < 8; j++) {
		if (j == who || mutual_ally_ai(g, who, j)) continue;
		const Leader& J = LD(g, j);
		if (!(J.flags & leaderflag::kValid)) continue;
		if (diff < 2 && !(J.flags & leaderflag::kHuman)) continue;
		for (int32_t u = kUnits; u < 402; u++) {
			const int32_t cnt = J.num_units[size_t(u - kUnits)];
			if (cnt <= 0) continue;
			const Type& U = g.type(u);
			if (!(U.role & 0x10000)) continue;
			const bool t_aa = (T.obj_masks & int32_t(0x80000000)) != 0;
			if (U.domain == 2) {
				if (!t_aa) continue;
			} else {
				if (U.domain != T.domain) continue;
				if (t_aa && T.domain == 0) continue;  // a land anti-air type counts only air enemies
			}
			const int32_t c = std::min(cnt, 10);
			const int32_t tv = size_t(u - kUnits) < T.relative_value.size() ? T.relative_value[size_t(u - kUnits)] : 0;
			const int32_t uv = size_t(t - kUnits) < U.relative_value.size() ? U.relative_value[size_t(t - kUnits)] : 0;
			sum += int64_t(tv - uv) * c;
			n += c;
		}
	}
	return n == 0 ? 0 : int32_t(sum / n);
}

void upgrade_units(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	if (g.info.elimination == 1) {
		int32_t c = -1, w = -1;
		find_capital(g, who, &c, &w, -1, -1);
		if (w != who) return;
	}
	int32_t e_war = 0, e_any = 0;
	for (int32_t j = 0; j < 8; j++) {
		if (j == who || !(LD(g, j).flags & leaderflag::kValid)) continue;
		if (!(L.diplos(j) == 0 || LD(g, j).diplos(who) == 0)) continue;
		if (L.active_wars_with & (1 << j)) e_war = std::max(e_war, LD(g, j).combat);
		else e_any = std::max(e_any, LD(g, j).combat);
	}
	const int32_t E = e_war != 0 ? e_war : e_any;
	const int32_t diff = get_diff(g, who);
	for (int32_t t = kUnits; t < 402; t++) {
		if (type_avail(g, who, t, 1) != 2) continue;
		const Type& T = g.type(t);
		const int32_t w = T.where;
		if (w < 0 || owned_w(g, who, w) == 0 || researching(g, who, t, -1, false, false)) continue;
		int32_t age_t = T.age;
		int32_t owned = 0, buildable = 0, age_b = 0;
		bool have_b = false;
		for (int32_t f = T.from; f >= kUnits && f < 402;) {
			int32_t ff = f;
			if (!tribe_can_type(g, who, ff)) ff = get_graft(g, who, ff);
			if (ff < kUnits || ff >= 402) break;
			owned += L.num_units[size_t(ff - kUnits)];
			if (type_avail(g, who, ff, 1) == 4) {
				buildable = 1;
				if (!have_b) {
					age_b = g.type(ff).age;
					have_b = true;
				}
			}
			f = g.type(ff).from;
		}
		const bool special = (T.unit_flags & 0x20000) != 0;
		if (!((owned != 0 || !buildable || special) && !type_is(T, 318, false))) continue;
		const int32_t P = (L.pop * 1000) / std::max(1, L.city_num + L.village_num);
		const int32_t d = age_t - age_b;
		const int32_t m = std::max(1, 5 * d + 2);
		int32_t V = m * ((((4 * P) / 2) * (owned + 2)) / 2);
		if (L.wonder_mod != 0) V /= 100;
		if (T.domain == 0) V = V * L.ground_mod / 256;
		else if (T.domain == 1) V = V * L.sea_mod / 256;
		else if (T.domain == 2) {
			V = V * L.air_mod / 256;
			if (type_is(T, ptype::kNuke, false)) {
				V *= (g.world.sea_map / 2 + 1) * (L.pers.nukes + 2);
				if (L.pers.nukes > 0) V *= 2;
				if (L.pers.rush < 0 && L.pers.army_size > 0) V *= 2;
				bool shield = false;
				for (int32_t j = 0; j < 8; j++) {
					if ((LD(g, j).flags & 3) != 3 || is_ally(g, who, j)) continue;
					if (has_preq(g, j, 693) || researching(g, j, 579, -1, false, false)) shield = true;
				}
				if (shield) continue;
				if (diff > 2 && leaders_max_enemy_age(g, who) < L.age) V *= 100;
			}
		}
		const int32_t C = L.combat;
		if (C >= 2 * E) V *= 4;
		else if (C >= (3 * E) / 2) V *= 3;
		else if (C > E) V *= 2;
		if (C >= 24) V *= (special || owned) ? 4 : 2;
		else if (C >= 16) V = (special || owned) ? V * 3 : (V * 3) / 2;
		else if (C >= 8 && !special && owned == 0) V = (V * 5) / 4;
		if (V < 0 || V > 9000000) V = 9000000;
		if (diff != 2) {
			int32_t u = unit_prod_value(g, who, t);
			if (diff == 0 || diff >= 3) u = (3 * u) / 2;
			u = ((rnd_raw(g) % 3 + 4) * u) / 5;
			if (diff < 2) u = -u;
			u = std::clamp(u, -256, 256);
			V = (V / 256) * (u + 256);
		}
		int32_t slot;
		if (!(T.role & 0x10000)) {
			slot = 8;
			if (L.active_wars == 0) V = (5 * V) / 4;
		} else {
			if (g.info.rush_rules == 8) continue;
			slot = 7;
			if (L.active_wars != 0) V *= owned / 3 + 1;
			else if (L.wars != 0) V *= owned / 6 + 6;
			if (special) V *= d + 1;
			else V = (d + 2) * V / 2;
		}
		if (L.shortages == 0) V *= 2;
		if (T.domain == 1) V *= g.world.sea_map * g.world.sea_map;
		if (L.effective_pop > L.pop_cap - 4) V *= 2;
		const int32_t m2 = can_pay_cost(g, t, who, -1, -1, 1) < 1 ? 64 : 256;
		make_me(g, who, t, m2 * (V / 256), 1, slot, -1, 0, 1, 0, 0);
	}
}

// ---------------------------------------------------------------- make_stuff, use_market

int32_t make_stuff(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	auto& M = L.make_list;
	const int32_t t0 = M[0].t;
	const int32_t o0 = M[0].o;
	if (t0 == -1 || M[0].val == 0) return 0;
	const int32_t city0 = M[0].city;
	use_market(g, who);
	bool ok = can_pay(g, who, 0);
	int32_t cost0[6];
	for (int32_t k = 0; k < 6; k++) cost0[k] = cost_of(g, t0, k, who, o0, city0);
	static const bool mtrace = std::getenv("RON_AI_MAKE") != nullptr;  // developer aid
	if (mtrace && std::atoi(std::getenv("RON_AI_MAKE")) == who) {
		std::fprintf(stderr, "make f%d L%d t0 %d val %d ok %d npay %d need %d cost", g.frame, who, t0, M[0].val, int(ok),
		             can_pay_cost(g, t0, who, city0, o0, M[0].escrow), M[0].num);
		for (int32_t k = 0; k < 6; k++) std::fprintf(stderr, " %d", cost0[k]);
		std::fprintf(stderr, " bucket");
		for (int32_t k = 0; k < 6; k++) std::fprintf(stderr, " %d", L.bucket[size_t(k)]);
		std::fprintf(stderr, " esc");
		for (int32_t k = 0; k < 6; k++) std::fprintf(stderr, " %d", L.escrow[size_t(k)]);
		std::fprintf(stderr, " | M");
		for (int32_t j = 0; j < 11; j++) std::fprintf(stderr, " %d:%d:c%d", M[size_t(j)].t, M[size_t(j)].val, M[size_t(j)].city);
		std::fprintf(stderr, "\n");
	}
	int32_t bought = 0;
	bool keep = false, skip_extra = ok;
	if (!ok) {
		if (L.bucket[3] < cost0[3]) skip_extra = true;
	} else {
		bought = make_this(g, who, 0) == 0 ? 1 : 0;
		if (!is_city_t(g, t0)) keep = true;
	}
	if (is_city_t(g, t0)) keep = true;
	const Type& T0 = g.type(t0);
	bool drop_all = type_is(T0, ptype::kTower) || type_is(T0, ptype::kFortX) || is_city_t(g, t0) || is_wonder_type(t0) ||
	                type_is(T0, ptype::kUniversity, true) || (is_unit_type(t0) && t0 != 50 && t0 != 51);
	for (int32_t j = 0; j < 11; j++) {
		if (M[size_t(j)].t == t0 && (rnd_raw(g) % 3 == 0 || drop_all)) M[size_t(j)].t = -1;
	}
	int32_t extra = 0;
	if (!skip_extra) {
		int32_t n = 0;
		for (int32_t k = 0; k < 6; k++) {
			if (type_avail(g, who, k, 1) != 0 && cost0[k] != 0) {
				n++;
				extra += cost0[k] - L.bucket[size_t(k)];
			}
		}
		if (n != 0) extra /= n;
		if (extra < 0) extra = 0;
	}
	for (int32_t i = 1; i <= 10; i++) {
		const int32_t t = M[size_t(i)].t, o = M[size_t(i)].o, city = M[size_t(i)].city;
		if (t < 0 || M[size_t(i)].val == 0) continue;
		if (i > 3) {
			bool any = false;
			for (int32_t s = 0; s < 4; s++) any |= M[size_t(s)].t == t && M[size_t(s)].city == city;
			if (any) continue;
		}
		bool skip = false;
		for (int32_t k = 0; k < 6; k++) {
			if (type_avail(g, who, k, 1) == 0 || cost0[k] == 0) continue;
			const int32_t c = cost_of(g, t, k, who, o, city);
			if (c == 0 || !(L.bucket[size_t(k)] < cost0[k] + c + extra)) continue;
			if (i == 5) {
				if (L.free_peasants != 0 || L.gatherers != 0) skip = true;
			} else if (i == 4) {
				skip = true;
				if (is_build_type(t) && (g.type(t).build_flags & 0x40)) {
					const int32_t gd = building_good(t);
					for (int32_t k2 = 0; k2 < 6; k2++) {
						if (type_avail(g, who, k2, 1) != 0 && k2 == gd && (L.econ[size_t(k2)] & 4)) skip = false;
					}
				}
			} else {
				skip = true;
			}
			break;
		}
		if (skip) continue;
		if (can_pay_cost(g, t, who, city, o, M[size_t(i)].escrow) < M[size_t(i)].num) continue;
		if (is_city_t(g, t)) keep = true;
		make_this(g, who, i);
		const Type& T = g.type(t);
		const bool drop = type_is(T, ptype::kTower) || type_is(T, ptype::kFortX) || is_city_t(g, t) || is_wonder_type(t);
		for (int32_t j = i; j < 11; j++) {
			if (M[size_t(j)].t == t && (rnd_raw(g) % 3 == 0 || drop)) M[size_t(j)].t = -1;
		}
	}
	if (!keep) L.site_mark++;
	return bought;
}

void use_market(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	const int32_t n = std::max(1, L.epoch[2]);
	int32_t need[6] = {0, 0, 0, 0, 0, 0};
	for (int32_t i = 0; i < n && i < 11; i++) {
		const MakeObject& it = L.make_list[size_t(i)];
		if (it.t <= 0 || it.val <= 0) continue;
		for (int32_t k = 0; k < 6; k++) need[k] += cost_of(g, it.t, k, who, it.o, it.city);
	}
	if (!(has_tribe_bonus(g, who, 4) || has_preq(g, who, ptype::kBuySell)) || !ai_has_market(g, who)) return;
	for (int32_t k = 0; k < 6; k++) {
		if (k == 3 || type_avail(g, who, k, 1) == 0) continue;
		bool traded = true;
		while (L.bucket[size_t(k)] < need[k] && traded) {
			traded = false;
			int32_t buy = 0, sell = 0;
			calc_market_prices(g, who, k, &buy, &sell);
			if (k != 2 && L.bucket[2] - buy >= need[2]) {
				do_buy(g, who, k);
				traded = true;
			} else {
				const int32_t s = rnd_raw(g) % 6;
				for (int32_t j = 0; j < 6; j++) {
					const int32_t h = (s + j) % 6;
					if (h == 3 || h == 2 || h == 5 || h == k || type_avail(g, who, h, 1) == 0) continue;
					if (L.bucket[size_t(h)] - 100 >= need[h] &&
					    (L.income[size_t(h)] >= L.resource_cap[size_t(h)] / 2 || L.bucket[size_t(h)] >= 200)) {
						do_sell(g, who, h);
						traded = true;
					}
				}
			}
		}
	}
}

// ---------------------------------------------------------------- make_this and the produce_* functions

int32_t make_this(Game& g, int32_t who, int32_t i) {
	Leader& L = LD(g, who);
	MakeObject& it = L.make_list[size_t(i)];
	const int32_t t = it.t;
	int32_t r = 1;
	if (t < 0) return 1;
	if (is_build_type(t)) {
		if (type_avail(g, who, t, 1) != 4) {
			r = produce_tech(g, who, t, it.escrow);
		} else if (it.up != 0) {
			r = produce_upgrade(g, who, t, it.city, it.escrow);
		} else if (type_is(g.type(t), ptype::kTown, false)) {
			r = produce_upgrade(g, who, t, it.city, it.escrow);
		} else if (is_city_t(g, t)) {
			const int32_t reg = g.world.in_w(it.wx, it.wy) ? g.world.w(it.wx, it.wy).region & 63 : 0;
			if (L.reg_free_peasants[size_t(reg)] == 0 && L.reg_gatherers[size_t(reg)] == 0) return 1;
			r = produce_city(g, who, t, it.wx, it.wy, it.escrow);
		} else {
			if (it.city < 0 || size_t(it.city) >= g.cities[size_t(who)].size()) return 1;
			const City& C = g.cities[size_t(who)][size_t(it.city)];
			if (!C.valid()) return 1;
			const Build* cb = g.build(who, C.o);
			const int32_t reg = cb ? g.world.w(wcoord_of(cb->x), wcoord_of(cb->y)).region & 63 : 0;
			if (L.reg_free_peasants[size_t(reg)] == 0 && L.reg_gatherers[size_t(reg)] == 0) return 1;
			const Type& T = g.type(t);
			if (T.domain == 0 && !(T.build_flags & 0x10) && int32_t(C.land) - int32_t(C.filled) < 2) {
				const int32_t idx = std::clamp(std::max(T.x_size, T.y_size) - 2, 0, 3);
				const int32_t sp = idx == 3 ? C.ter[0] : C.space[size_t(idx)];  // space[3] reads ter[0] (original)
				if (sp < 2) return 1;
			}
			r = produce_building(g, who, t, C.o, it.escrow);
		}
	} else if (is_tech_type(t)) {
		r = produce_tech(g, who, t, it.escrow);
	} else if (is_unit_type(t)) {
		if (type_avail(g, who, t, 1) == 4) r = produce_unit(g, who, t, it.city, it.num, it.escrow);
		else r = produce_tech(g, who, t, it.escrow);
	} else if (is_spell_type(t)) {
		r = produce_spell(g, who, t, it.city, it.escrow);
	}
	if (r == 0) {
		L.make_list[size_t(i)].val /= 100;
		return 0;
	}
	return r;
}

int32_t produce_tech(Game& g, int32_t who, int32_t t, int32_t escrow) {
	if (has_tech(g, who, t) || researching(g, who, t, -1, false, false)) return 0;
	const Leader& L = LD(g, who);
	const int32_t w = g.type(t).where;
	int32_t best = -1, bs = -1;
	auto consider = [&](Build& b, int32_t s) {
		if (build_count_queue(g, b, 1, t) != 0) return;
		if (is_unassimilated(g, b)) s /= 2;
		s /= bdanger(g, who, b) + 1;
		if (s > bs) {
			bs = s;
			best = b.o;
		}
	};
	if (w >= 0 && is_military_trainer(g, w)) {
		for (int32_t o : L.mil_trainers) {
			Build* b = g.build(who, o);
			if (!b || !b->alive() || !(b->flags & objflag::kActive) || !type_is(*b->ptype, w, is_build_type(t))) continue;
			consider(*b, 1000000 / (b->queued + 1) / (b->damage + 1));
		}
	} else if (w >= 0) {
		const auto& cl = g.cities[size_t(who)];
		for (int32_t c = 0; c < L.city_mark && size_t(c) < cl.size(); c++) {
			if (!cl[size_t(c)].valid()) continue;
			if (city_count_buildings(g, who, c, w, is_build_type(t) ? 1 : 0, 1) == 0) continue;
			const int32_t lv = city_level(g, who, c);
			for (int32_t o = cl[size_t(c)].o; o >= 0;) {
				Build* b = g.build(who, o);
				if (!b) break;
				o = b->city_down;
				if (!b->alive() || !(b->flags & objflag::kActive) || !type_is(*b->ptype, w, is_build_type(t))) continue;
				consider(*b, lv * 1000000 / (b->queued + 1));
			}
		}
	}
	if (best < 0) return 1;
	return build_queue_up(g, *g.build(who, best), t, escrow);
}

int32_t produce_upgrade(Game& g, int32_t who, int32_t t, int32_t city, int32_t escrow) {
	if (city < 0 || size_t(city) >= g.cities[size_t(who)].size()) return 1;
	const City& C = g.cities[size_t(who)][size_t(city)];
	const int32_t lv = city_level(g, who, city);
	const int32_t w = g.type(t).where;
	int32_t best = -1, bs = -1;
	for (int32_t o = C.o; o >= 0;) {
		Build* b = g.build(who, o);
		if (!b) break;
		o = b->city_down;
		if (!b->alive() || !(b->flags & objflag::kActive)) continue;
		if (w >= 0 && !type_is(*b->ptype, w, true) && !(is_t(g, t, ptype::kTown) && type_is(*b->ptype, ptype::kVillage)))
			continue;
		if (build_count_queue(g, *b, 1, t) != 0) continue;
		int32_t s = lv * 1000000 / (b->queued + 1);
		if (is_unassimilated(g, *b)) s /= 2;
		s /= bdanger(g, who, *b) + 1;
		if (s > bs) {
			bs = s;
			best = b->o;
		}
	}
	if (best < 0) return 1;
	return build_queue_up(g, *g.build(who, best), t, escrow);
}

int32_t produce_spell(Game& g, int32_t who, int32_t t, int32_t city, int32_t escrow) {
	const Leader& L = LD(g, who);
	const int32_t w = g.type(t).where;
	int32_t best = -1, bs = -1;
	const auto& cl = g.cities[size_t(who)];
	for (int32_t c = 0; c < L.city_mark && size_t(c) < cl.size(); c++) {
		if (!cl[size_t(c)].valid() || (city >= 0 && c != city)) continue;
		if (w < 0 || city_count_buildings(g, who, c, w, 0, 1) == 0) continue;
		const int32_t lv = city_level(g, who, c);
		for (int32_t o = cl[size_t(c)].o; o >= 0;) {
			Build* b = g.build(who, o);
			if (!b) break;
			o = b->city_down;
			if (!b->alive() || !build_could_queue(g, *b, t)) continue;
			const int32_t s = lv * 1000000 / (b->queued + 1) / (bdanger(g, who, *b) + 1);
			if (s > bs) {
				bs = s;
				best = b->o;
			}
		}
	}
	if (best < 0) return 1;
	return build_queue_up(g, *g.build(who, best), t, escrow);
}

int32_t produce_unit(Game& g, int32_t who, int32_t t, int32_t city, int32_t num, int32_t escrow) {
	Leader& L = LD(g, who);
	const Type& T = g.type(t);
	const int32_t w = T.where;
	if (w < 0) return 1;
	const bool fort = type_is(g.type(w), ptype::kFortX, false);
	if (!is_military_trainer(g, w) && !fort) {
		if (city < 0 || size_t(city) >= g.cities[size_t(who)].size()) return 1;
		const City& C = g.cities[size_t(who)][size_t(city)];
		const int32_t lv = city_level(g, who, city);
		int32_t best = -1, bs = -1;
		for (int32_t o = C.o; o >= 0;) {
			Build* b = g.build(who, o);
			if (!b) break;
			o = b->city_down;
			bool match = type_is(*b->ptype, w, is_build_type(t));
			if (!match && w == ptype::kTown && type_is(*b->ptype, ptype::kVillage)) match = true;
			if (!match || !(b->flags & objflag::kActive) || (b->build_masks & bmask::kNeutralized)) continue;
			int32_t s = lv * 1000000;
			if (is_unassimilated(g, *b)) s /= 2;
			s = ((s / (b->queued + 1)) / (b->damage / 50 + 1)) / (bdanger(g, who, *b) + 1);
			if (bs < s) {
				bs = s;
				best = b->o;
			}
		}
		if (best < 0) return 1;
		Build& b = *g.build(who, best);
		const int32_t r = build_queue_up(g, b, t, escrow);
		for (int32_t k = 1; k < num; k++) build_queue_up(g, b, t, escrow);
		return r;
	}
	if (fort) {
		int32_t best = -1, bs = -1;
		const auto& F = g.forts[size_t(who)];
		for (int32_t i = 0; i < L.fort_mark && size_t(i) < F.size(); i++) {
			if (!(F[size_t(i)].fort_flags & 1)) continue;
			Build* b = g.build(who, F[size_t(i)].o);
			if (!b || !b->alive() || !(b->flags & objflag::kActive) || !type_is(*b->ptype, w, false)) continue;
			int32_t s = int32_t(int64_t(1000000 / uint32_t(b->queued + 1)) / (b->damage + 1));
			if (is_unassimilated(g, *b)) s /= 5;
			s /= bdanger(g, who, *b) + 1;
			if (bs < s) {
				bs = s;
				best = b->o;
			}
		}
		if (best < 0) return 1;
		Build& b = *g.build(who, best);
		const int32_t r = build_queue_up(g, b, t, escrow);
		for (int32_t k = 1; k < num; k++) build_queue_up(g, b, t, escrow);
		return r;
	}
	int32_t n = num;
	if (T.attack != 0 && !(T.obj_masks & 0x8000000) && get_diff(g, who) >= 2)
		n = std::min(7, can_pay_cost(g, t, who, -1, -1, escrow));
	if (n < 1) return -1;
	int32_t first = 1;
	for (int32_t k = 0; k < n; k++) {
		int32_t best = -1, bs = -1;
		for (int32_t o : L.mil_trainers) {
			if (o < 0) continue;
			Build* b = g.build(who, o);
			if (!b || !b->alive() || !(b->flags & objflag::kActive)) continue;
			if (!type_is(*b->ptype, w, is_build_type(t)) || b->queued >= 4) continue;
			int32_t s = int32_t(int64_t(1000000 / uint32_t(b->queued + 1)) / (b->damage + 1));
			if (is_unassimilated(g, *b)) s /= 5;
			s /= bdanger(g, who, *b) + 1;
			if (bs < s) {
				bs = s;
				best = o;
			}
		}
		if (best < 0) {
			if (k == 0) return 1;
			continue;
		}
		const int32_t r = build_queue_up(g, *g.build(who, best), t, escrow);
		if (k == 0) first = r;
	}
	return first;
}

int32_t produce_city(Game& g, int32_t who, int32_t t, int32_t wx, int32_t wy, int32_t escrow) {
	Leader& L = LD(g, who);
	const int32_t R = g.world.w(wx, wy).region;
	int32_t best = -1, bd = 99999999, last_kind = 0;
	for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
		Unit* u = g.unit(who, o);
		if (!u || !u->alive() || !u->on_map() || (u->type != 50 && u->type != 51)) continue;
		const int32_t k = action_kind(*u);
		last_kind = k;  // (the original reads the last examined citizen's kind below)
		if (k != 0 && k != kGather && k != kExploreTo) continue;
		if (g.world.w(wcoord_of(u->x), wcoord_of(u->y)).region != R) continue;
		int32_t d = vector_dist(wx - wcoord_of(u->x), wy - wcoord_of(u->y));
		if (k == kGather) d += 0x18;
		if (bd <= d) continue;
		best = o;
		bd = d;
	}
	if (best < 0) return 1;
	pay_cost(g, t, who, -1, -1, escrow, nullptr);  // paid before the site test (no refund when blocked)
	if (blocked_site(g, t, wx * 0x300, wy * 0x300, who, -1, nullptr) != 0) return 1;
	const int32_t b = init_build_site(g, who, t, wx * 0x300, wy * 0x300, false, -1);
	if (b < 0) return 1;
	const Unit& bu = *g.unit(who, best);
	const int32_t bx = wcoord_of(bu.x), by = wcoord_of(bu.y);
	Group gr;
	group_clear(gr, -1);
	if (L.city_num == 0) {
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (u && u->alive() && (u->type == 50 || u->type == 51)) group_add(g, gr, o, who, 0, 0);
		}
	} else {
		group_add(g, gr, best, who, 0, 0);
	}
	group_action_swarm_around(g, gr, b, who, kQueueNew, kBuildAt, true);
	const int32_t c = objects_find_city(g, bx * 0x300 + 0x180, by * 0x300 + 0x180, search::kFriendly, who, 0x200,
	                                    filter::kAll, 0, 0);
	auto& cl = g.cities[size_t(who)];
	if (last_kind != kGather) {
		if (c >= 0) cl[size_t(c)].free--;
		L.free_peasants--;
		L.reg_free_peasants[size_t(R & 63)]--;
	} else {
		if (c >= 0) cl[size_t(c)].gatherers--;
		L.gatherers--;
		L.reg_gatherers[size_t(R & 63)]--;
	}
	return 0;
}

}  // namespace ron::sim
