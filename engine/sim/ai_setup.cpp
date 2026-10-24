// The AI's setup (Leader::init's AI part, Leader::random_personality), the LeaderData / Leaders / Game helpers the AI
// reads, and the ObjectsData searches it calls (docs/ai-spec.md 0, 2, 6.10).
//
// Evidence (local, gitignored): re/decomp/ai/, re/decomp/ai2/, re/decomp/ai_port/ (Leader_random_personality,
// Leader_init, ObjectsData_find_unit / find_building / find_city with their disassembly in ai_port/asm,
// Search_valid_filter_body.asm), re/decomp/combat_A/Search_valid_search.
#include "sim/ai.hpp"
#include "sim/ai_internal.hpp"

#include <algorithm>

#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/orders.hpp"
#include "sim/production_internal.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using prod::is_t;
using prod::tb;

namespace {
Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }
const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }
}  // namespace

// ---------------------------------------------------------------- personality and Leader::init

void random_personality(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	Personality& p = L.pers;
	auto r3 = [&]() { return g.rng.get(0, 0xffff) % 3 - 1; };
	auto draw = [&]() { return g.rng.get(0, 0xffff); };
	const int32_t t = L.tribe;
	p.early_army = 0;
	p.rush = r3();
	if (t == 14 || t == 2 || t == 7 || t == 4 || t == 5) {
		if (p.rush == 1 && draw() % 5 != 0) p.rush = -1;
	} else if (t == 12 || t == 8) {
		if (p.rush == 1 && draw() % 3 != 0) p.rush = 0;
	} else if (t == 0 || t == 3 || t == 17 || t == 6 || t == 15) {
		if (p.rush < 0 && draw() % 3 != 0) p.rush = g.world.sea_map < 3 ? 1 : 0;
	}
	for (int32_t j = 0; j < 8; j++) {
		const Leader& J = LD(g, j);
		if ((J.flags & 3) != 3 || j == who || is_ally(g, who, j) || (J.flags & 0x10)) continue;
		if (J.tribe == 14 || J.tribe == 13 || J.tribe == 1) {
			if (p.rush == 1 && (draw() & 3) != 0) p.rush = 0;
		} else if ((J.tribe == 0 || J.tribe == 15) && p.rush == -1) {
			if ((draw() & 1) != 0) p.rush = 0;
		}
	}
	if (p.rush == 1 && g.world.sea_map > 2 && draw() % 3 != 0) p.rush = 0;
	p.cities = r3();
	if ((draw() & 1) == 0) p.arms = draw() % 5 - 2;
	else p.arms = r3();
	p.army = r3();
	p.army_size = r3();
	p.raid = r3();
	if ((t == 17 || t == 0) && draw() % 3 != 0) p.raid = 1;
	if (g.info.rush_rules == 8) p.raid = -1;
	p.invade = r3();
	p.target = r3();
	p.raze = 0;
	p.strategy = r3();
	p.spells = r3();
	p.forts = r3();
	if (t == 6) p.forts = 1;
	p.nukes = r3();
	p.air = r3();
	if (t == 12) p.air = 1;
	{
		const int32_t v = draw() % 3;
		p.naval = v - 1;
		if ((t == 15 || t == 11 || t == 9) && p.naval < 1) p.naval = v;
	}
	p.market = r3();
	p.scouts = r3();
	if (t == 9) p.scouts = 1;
	p.civilians = r3();
	p.friendly_human = r3();
	p.alliance_human = r3();
	p.friendly_ai = p.friendly_human;
	p.alliance_ai = p.alliance_human;
}

void leader_init_ai(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	L.prod_step = 1;  // script_step (+0x790)
	L.production_ai = 0;
	L.prod_script_on = 0;
	L.prod_script.clear();
	L.pers = Personality();  // Personality::close
	if ((L.flags & 0xc) != leaderflag::kHuman) {
		L.prod_script_on = 1;
		random_personality(g, who);
		// GameInfo.player[get_player()].flags (lobby): 0x1000 rusher, 0x2000 boomer; team style 3, team 1 rushes.
		const uint32_t pf = g.info.player_flags[size_t(who & 7)];
		if (pf & 0x1000) {
			L.pers.rush = 1;
			L.pers.raid = 1;
		} else if (pf & 0x2000) {
			L.pers.rush = -1;
			L.pers.raid = -1;
			L.pers.civilians = 1;
		} else if (g.info.team_style == 3 && g.info.team[size_t(who & 7)] == 1) {
			L.pers.rush = 1;
			L.pers.raid = 1;
		}
		// Patch version >= 9: the coin is drawn only when rush == 0, before has_tribe_bonus(0x13).
		bool economic = L.pers.rush < 0;
		if (!economic && L.pers.rush == 0) economic = (g.rng.get(0, 0xffff) & 1) != 0;
		if (economic && !has_tribe_bonus(g, who, 0x13)) {
			L.prod_script = "economic";
		} else {
			L.prod_script = "defensive";
			L.pers.early_army = 1;
		}
	}
	L.ages_queued = 0;
	L.epochs_queued = 0;
	L.site_mark = 0;
	L.sites.fill(Site());
	L.make_list.fill(MakeObject());  // MakeList::init
	L.mil_trainers.clear();
	L.new_rares.clear();
	L.oil_patches.clear();
	// Leader::init's per-player reset (agendas, stamps, ... zeroed by the constructor): hire_who[k] = -1.
	L.hire_who.fill(-1);
}

// ---------------------------------------------------------------- LeaderData / Leaders / Game helpers

int32_t get_diff(const Game& g, int32_t who) {
	const Leader& L = LD(g, who);
	if (g.multiplayer) return L.difficulty;
	// semaphore byte1 & 0x10 (scripted rules: game_rules 10) and not Conquer the World: the leader's own setting.
	if (g.scripted_rules && !g.ctw && L.difficulty >= 0) return L.difficulty;
	return g.info.difficulty;
}

bool is_ai_seat(const Game& g, int32_t who) { return (LD(g, who).flags & 0xc) != leaderflag::kHuman; }
bool is_rusher(const Game& g, int32_t who) { return (g.info.player_flags[size_t(who & 7)] & 0x1000) != 0; }
bool is_boomer(const Game& g, int32_t who) { return (g.info.player_flags[size_t(who & 7)] & 0x2000) != 0; }

bool mutual_ally_ai(const Game& g, int32_t a, int32_t b) {
	if (a == b) return true;
	return LD(g, a).diplos(b) == 2 && LD(g, b).diplos(a) == 2;
}

int32_t get_team_terr(const Game& g, int32_t who) {
	// LeaderData::get_team_terr @6d62e0 (team style 7 neutral seats: the lobby is not modelled, no neutrals).
	int32_t s = 0;
	for (int32_t j = 0; j < 8; j++) {
		const Leader& J = LD(g, j);
		bool team = j == who;
		if (!team && (J.flags & leaderflag::kValid)) {
			if (g.frame == 0) team = is_team(g, who, j);
			else team = is_ally(g, who, j);
		}
		if (team) s += J.territory;
	}
	return s;
}

int32_t get_mod_resource_cap(const Game& g, int32_t who, int32_t good) {
	if (g.info.starting_resources == 8) return 0;
	const Leader& L = LD(g, who);
	const int32_t cap = L.resource_cap[size_t(good)];
	if (!g.multiplayer && !(L.flags & leaderflag::kHuman)) {
		const int32_t d = get_diff(g, who);
		if (d == 0) return int32_t(float(cap) * 0.5f);
		if (d == 1) return int32_t(float(cap) * 0.75f);
	}
	return int32_t(float(cap) * 1.0f);
}

bool ai_has_market(const Game& g, int32_t who) {
	const Leader& l = LD(g, who);
	if (l.num_buildings[size_t(ptype::kMarket - kBuilds)] == 0) return false;
	const auto& cl = g.cities[size_t(who)];
	for (int32_t c = 0; c < l.city_mark && size_t(c) < cl.size(); c++) {
		const City& ct = cl[size_t(c)];
		if ((ct.city_flags & cityflag::kValid) && ct.race == who && (ct.city_flags & cityflag::kHasMarket)) return true;
	}
	return false;
}

int32_t get_lowest_epoch(const Game& g, int32_t who) {
	const Leader& L = LD(g, who);
	int32_t v = std::min(99, L.epoch[0]);
	for (int32_t c = 1; c < 4; c++) v = std::min(v, L.epoch[size_t(c)]);
	return v;
}
int32_t get_highest_epoch(const Game& g, int32_t who) {
	const Leader& L = LD(g, who);
	int32_t v = std::max(0, L.epoch[0]);
	for (int32_t c = 1; c < 4; c++) v = std::max(v, L.epoch[size_t(c)]);
	return v;
}

int32_t get_farm_limit(const Game& g, int32_t who) {
	int32_t n = tb(g, who, 7) ? K("egyptian_farms_per_city_base") : K("farms_per_city_base");
	if (has_wonder(g, who, ptype::kTajMahal)) n += K("taj_farms");
	if (has_wonder(g, who, ptype::kKremlin)) n += K("kremlin_farms");
	const Leader& L = LD(g, who);
	if ((L.rare[4] & 2) || (L.rare_conquest[4] & 2)) n += K("olive_farms");
	return n;
}

int32_t get_target(const Game& g, int32_t who) {
	// LeaderData::get_target @6da000 over Game.start_list / start_index. TODO(ai-spec 16.2): random maps' start
	// permutation; scenarios keep the identity permutation.
	for (int32_t i = 0; i < 8; i++) {
		const int32_t j = (who + 1 + i) & 7;
		if ((LD(g, j).flags & 3) == 3) return j;
	}
	return who;
}

bool is_tribute_period(const Game& g, int32_t a, int32_t b) {
	// LeaderData::is_tribute_period @6d5b90: a tribute from a (stamped on a's tribute_stamp[k]) to an ally of b within
	// 3000 frames. The loop reads tribute_stamp[k] of every other VALID leader k that is b or allied with b.
	const Leader& A = LD(g, a);
	for (int32_t k = 0; k < 8; k++) {
		if (k == a || !(LD(g, k).flags & leaderflag::kValid)) continue;
		bool hit = b == k;
		if (!hit) hit = g.frame == 0 ? is_team(g, k, b) : is_ally(g, k, b);
		if (!hit) continue;
		const int32_t s = A.tribute_stamp[size_t(k)];
		if (s == 0) return false;
		if (g.frame - s < 3000) return true;
	}
	return false;
}

int32_t leaders_max_age(const Game& g) {
	int32_t m = -1;
	for (int32_t j = 0; j < 8; j++) {
		if ((LD(g, j).flags & leaderflag::kActive) && LD(g, j).age >= m) m = LD(g, j).age;
	}
	return m;
}
int32_t leaders_max_enemy_age(const Game& g, int32_t who) {
	int32_t m = -1;
	for (int32_t j = 0; j < 8; j++) {
		if (!(LD(g, j).flags & leaderflag::kActive) || j == who || mutual_ally_ai(g, who, j)) continue;
		if (LD(g, j).age >= m) m = LD(g, j).age;
	}
	return m;
}
int32_t leaders_max_human_age(const Game& g) {
	int32_t m = -1;
	for (int32_t j = 0; j < 8; j++) {
		if ((LD(g, j).flags & 6) == 6 && LD(g, j).age >= m) m = LD(g, j).age;
	}
	return m;
}
int32_t leaders_max_human_cities(const Game& g) {
	int32_t m = -1;
	for (int32_t j = 0; j < 8; j++) {
		if ((LD(g, j).flags & 6) != 6) continue;
		const int32_t n = get_total_cities(g, j);
		if (n >= m) m = n;
	}
	return m;
}
int32_t leaders_best_human_age_stamp(const Game& g, int32_t t) {
	if (!is_age_type(t)) return -1;
	int32_t best = -1;
	for (int32_t j = 0; j < 8; j++) {
		if ((LD(g, j).flags & 7) != 7) continue;
		const int32_t s = LD(g, j).age_stamp[size_t(t - kAges)];
		if (s < 0) continue;
		if (best < 0 || s <= best) best = s;
	}
	return best;
}

int32_t queued_units(const Game& g, int32_t who) {
	// Leader::queued_units @6ce000: control cost of the units queued in the leader's active buildings.
	int32_t n = 0;
	const ObjectList& ol = g.objects[size_t(who)];
	for (int32_t o = kBuildBase; o < ol.build_mark; o++) {
		const Build* b = g.build(who, o);
		if (!b || !b->alive() || !(b->flags & objflag::kActive)) continue;
		for (int32_t i = 0; i < b->queued && i < b->build_queue.queue_size; i++) {
			const int32_t t = b->build_queue.queue[size_t(i)].type;
			if (is_unit_type(t) && type_avail(g, who, t, 1) >= 4) n += g.type(t).control_cost;
		}
	}
	return n;
}

int32_t get_armageddon(const Game& g) {
	// Game::get_armageddon @594020: the nukes allowed before Armageddon.
	int32_t v = K("armageddon_per_team") * g.info.num_sides + K("armageddon_per_nation") * g.info.num_nations +
	            K("armageddon");
	const uint8_t s = g.info.starting_resources;
	if (s == 7) v *= 3;
	else if (s == 5 || s == 6 || s == 11 || s == 12) v *= 2;
	if (s == 8 && v < 100) v = 100;
	return v;
}

int32_t get_nukes(const Game& g, int32_t who) {
	// LeaderData::get_nukes: 0 without the NUCLEARMISSILE tech bit, else queued + built minus those in flight.
	if (!has_tech(g, who, ptype::kNuke)) return 0;
	const Leader& L = LD(g, who);
	return get_queued(g, who, ptype::kNuke, L.num_queued[size_t(ptype::kNuke)]) + get_units(g, who, ptype::kNuke, 0) -
	       L.nukes_in_flight;
}

bool war_allowed(const Game& g) {
	const int32_t rr = g.info.rush_rules;
	if (rr == 0) return true;
	int32_t cur = -1;  // Game::current_age: max ages over VALID leaders
	for (int32_t j = 0; j < 8; j++) {
		if ((LD(g, j).flags & leaderflag::kValid) && LD(g, j).age > cur) cur = LD(g, j).age;
	}
	if (cur >= rr) return true;
	if (rr < 9) return false;
	const CategoryList* cl = g.rules->category_list("rush_rules");
	const int32_t mins = cl && size_t(rr) < cl->items.size() ? cl->items[size_t(rr)].data[0] : 0;
	return g.frame >= mins * 900;
}

namespace {
int32_t wonder_value(const Game& g, int32_t who) {  // LeaderData::get_wonder_value @6ebb90
	int32_t v = 0;
	const auto& W = g.wonder_records[size_t(who)];
	for (int32_t i = 0; i < LD(g, who).wonder_mark && size_t(i) < W.size(); i++) {
		if (!(W[size_t(i)].wonder_flags & 1)) continue;
		const Build* b = g.build(W[size_t(i)].who, W[size_t(i)].o);
		if (b && is_wonder_type(b->type)) v += b->ptype->wonder_val;
	}
	return v;
}
int32_t unbuilt_wonder_value(const Game& g, int32_t who) {  // @6d5ee0
	int32_t v = 0;
	for (const Unbuilt& u : g.unbuilt_wonders[size_t(who)]) {
		if (u.o < 0) continue;
		const Build* b = g.build(who, u.o);
		if (b && is_wonder_type(b->type)) v += b->ptype->wonder_val;
	}
	return v;
}
int32_t wonder_net(const Game& g, int32_t who) {  // @6ebb10
	const int32_t mine = get_team_wonder_value(g, who);
	int32_t best = 0;
	for (int32_t j = 0; j < 8; j++) {
		if (!(LD(g, j).flags & leaderflag::kValid) || j == who || mutual_ally_ai(g, j, who)) continue;
		best = std::max(best, get_team_wonder_value(g, j));
	}
	return std::max(0, mine - best);
}
}  // namespace

int32_t get_team_wonder_value(const Game& g, int32_t who) {
	int32_t v = 0;
	for (int32_t j = 0; j < 8; j++) {
		if ((LD(g, j).flags & leaderflag::kValid) && (j == who || mutual_ally_ai(g, who, j))) v += wonder_value(g, j);
	}
	return v;
}
int32_t get_enemy_wonder_value(const Game& g, int32_t who) {
	int32_t v = 0;
	for (int32_t j = 0; j < 8; j++) {
		if ((LD(g, j).flags & leaderflag::kValid) && j != who && !mutual_ally_ai(g, who, j)) v += wonder_value(g, j);
	}
	return v;
}
int32_t get_enemy_unbuilt_wonder_value(const Game& g, int32_t who) {
	int32_t v = 0;
	for (int32_t j = 0; j < 8; j++) {
		if ((LD(g, j).flags & leaderflag::kValid) && j != who && !mutual_ally_ai(g, who, j))
			v += unbuilt_wonder_value(g, j);
	}
	return v;
}
int32_t wonder_winning(const Game& g) {
	const CategoryList* cl = g.rules->category_list("wonderwins");
	const int32_t need = cl && size_t(g.info.wonderwin) < cl->items.size() ? cl->items[size_t(g.info.wonderwin)].data[0] : 0;
	int32_t best = 0, win = -1;
	for (int32_t j = 0; j < 8; j++) {
		if ((LD(g, j).flags & 3) != 3) continue;
		const int32_t n = wonder_net(g, j);
		if (n < best) continue;
		if (n == best && win >= 0) {
			if (!is_ally(g, j, win)) {
				win = -1;
				if (need <= n) {
					best = n;
					win = j;
				}
				continue;
			}
			if (wonder_value(g, j) > wonder_value(g, win)) {
				best = n;
				win = j;
			}
			continue;
		}
		if (need <= n) {
			best = n;
			win = j;
		}
	}
	return win;
}

int32_t city_level(const Game& g, int32_t who, int32_t c) {
	const City& C = g.cities[size_t(who)][size_t(c)];
	const Build* b = g.build(who, C.o);
	return b ? city_get_level(b->type) : 1;
}

int32_t city_num_buildings(const Game& g, int32_t who, int32_t c) {
	int32_t n = 0;
	for (int32_t o = g.cities[size_t(who)][size_t(c)].o; o >= 0;) {
		const Build* b = g.build(who, o);
		if (!b) break;
		o = b->city_down;
		if (b->alive() && (b->flags & objflag::kActive)) n++;
	}
	return n;
}

int32_t city_get_building(const Game& g, int32_t who, int32_t c, int32_t t) {
	for (int32_t o = g.cities[size_t(who)][size_t(c)].o; o >= 0;) {
		const Build* b = g.build(who, o);
		if (!b) break;
		const int32_t cur = o;
		o = b->city_down;
		if (b->alive() && (b->flags & objflag::kActive) && type_is(*b->ptype, t, false)) return cur;
	}
	return -1;
}

int32_t city_count_gather_slots(const Game& g, int32_t who, int32_t c, int32_t* gs, int32_t* fs) {
	for (int32_t k = 0; k < 6; k++) {
		if (gs) gs[k] = 0;
		if (fs) fs[k] = 0;
	}
	int32_t total = 0;
	for (int32_t o = g.cities[size_t(who)][size_t(c)].o; o >= 0;) {
		const Build* b = g.build(who, o);
		if (!b) break;
		o = b->city_down;
		if (!is_gather_type(*b->ptype)) continue;
		const int32_t k = building_good(b->type);
		if (k < 0) continue;
		const int32_t m = b->gather_max;
		if (gs) gs[k] += m;
		if (k != 3) total += m;
		if (fs) fs[k] += m - build_num_gatherers(g, *b, false, false);
	}
	return total;
}

bool city_could_queue(Game& g, int32_t who, int32_t c, int32_t t) {
	// City::could_queue @737fa0: some building of the city chain can queue t.
	for (int32_t o = g.cities[size_t(who)][size_t(c)].o; o >= 0;) {
		const Build* b = g.build(who, o);
		if (!b) break;
		o = b->city_down;
		if (b->alive() && build_could_queue(g, *b, t)) return true;
	}
	return false;
}

// ---------------------------------------------------------------- ObjectsData searches

namespace {

// Search::valid_search @67daa0 (cand = the candidate's leader).
bool valid_search(const Game& g, int32_t cand, int32_t s, int32_t who) {
	if (who < 0) return true;
	switch (s) {
	case 1: return cand == who;
	case 2: return !is_ally(g, cand, who) && !is_enemy(g, cand, who);
	case 3: return is_enemy(g, cand, who);
	case 4: return is_ally(g, cand, who);
	case 5: return !is_ally(g, cand, who);
	case 6: return cand != who;
	case 7: return !is_enemy(g, cand, who);
	default: return true;
	}
}

// Search::valid_filter @67dbb0 (jump table @67e57c; cases from the disassembly). Filters the AI does not pass are
// treated as passing (TODO(search): the rest of the table).
bool valid_filter(const Game& g, int32_t cw, int32_t co, int32_t f1, int32_t f2, int32_t filter) {
	if (filter == filter::kAll || uint32_t(filter - 1) > 0x16u) return true;
	const Object* ob = g.object(cw, co);
	if (!ob) return false;
	switch (filter) {
	case 1: return type_is(*ob->ptype, f1, true);   // FILTER_TYPE
	case 2: return type_is(*ob->ptype, f1, false);  // FILTER_BASE_TYPE
	case 3: return !(co == f1 && cw == f2);         // FILTER_NOT_ME
	case 8:                                          // FILTER_NOT_UNDER_ATTACK
		return ob->is_unit ? true : !(static_cast<const Build*>(ob)->build_masks & bmask::kUnderAttack);
	case 9:  // FILTER_COMBAT
		if (!ob->is_unit) {
			if (ob->flags & objflag::kCity) return false;
			if (!(ob->flags & objflag::kActive) || ob->ptype->attack == 0) return false;
			return ob->myhits - ob->damage > 0;  // ObjectData::hits_left
		}
		return ob->ptype->attack != 0 && (ob->ptype->role & 0x10000) != 0;
	case 11: {  // FILTER_TARGET: the unit's current action targets (f1, f2)
		if (!ob->is_unit) return false;
		const Order* a = get_action(*static_cast<const Unit*>(ob));
		if (!a || !order_is_targeted(a->type)) return false;
		return a->ox == f1 && a->whom == f2;
	}
	case 12:  // FILTER_NOT_BUSY (jump-table entry @67e019): a unit with no action, a move or a gather
		if (!ob->is_unit) return false;
		{
			const OrderType a = action_type(*static_cast<const Unit*>(ob));
			return a == kOrderNone || a == kGather || a == kMoveTo;
		}
	case 13: return ob->ptype->domain == f1;  // FILTER_DOMAIN
	case 14: return ob->ptype->domain != f1;  // FILTER_NOT_DOMAIN
	default: return true;
	}
}

int16_t region_at(const Game& g, int32_t x, int32_t y) {
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	return g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : int16_t(-1);
}

}  // namespace

bool search_valid_filter(const Game& g, int32_t cw, int32_t co, int32_t f1, int32_t f2, int32_t filter) {
	return valid_filter(g, cw, co, f1, f2, filter);
}

bool search_valid_search(const Game& g, int32_t cand, int32_t s, int32_t who) { return valid_search(g, cand, s, who); }

int32_t objects_find_city(const Game& g, int32_t x, int32_t y, int32_t s, int32_t who, int32_t flags, int32_t filter,
                          int32_t f1, int32_t f2) {
	const int16_t reg = (flags & 0x200) ? region_at(g, x, y) : int16_t(-1);
	g.find_who = who;
	g.find_dist = 99999999;
	int32_t best = -1;
	for (int32_t k = 0; k < 8; k++) {
		const Leader& K = LD(g, k);
		if (!(K.flags & leaderflag::kValid) || !valid_search(g, k, s, who)) continue;
		const auto& cl = g.cities[size_t(k)];
		for (int32_t i = 0; i < K.city_mark && size_t(i) < cl.size(); i++) {
			const City& C = cl[size_t(i)];
			if (!C.valid()) continue;
			if ((flags & 0x200) && reg != C.reg) continue;
			const Build* b = g.build(k, C.o);
			const int32_t bx = b ? b->x : C.x, by = b ? b->y : C.y;
			const int32_t d = vector_dist(bx - x, by - y);
			if (d > g.find_dist) continue;
			if (!valid_filter(g, k, C.o, f1, f2, filter)) continue;
			g.find_dist = d;
			g.find_who = k;
			best = i;
		}
	}
	return best;
}

int32_t objects_find_unit(const Game& g, int32_t x, int32_t y, int32_t s, int32_t who, int32_t radius, int32_t flags,
                          int32_t filter, int32_t f1, int32_t f2, int32_t filter2, int32_t f3, int32_t f4) {
	const int16_t reg = (flags & 0x200) ? region_at(g, x, y) : int16_t(-1);
	g.find_who = who;
	g.find_dist = 99999999;
	int32_t best = -1;
	const Circles& C = circles();
	const int32_t k = (radius + 0x2ff) / 0x300;
	auto consider = [&](int32_t cw, int32_t co, bool inside_ok) {
		const Unit* u = g.unit(cw, co);
		if (!u || !u->alive()) return;
		if (!inside_ok && !u->on_map()) return;
		if (!valid_filter(g, cw, co, f1, f2, filter) || !valid_filter(g, cw, co, f3, f4, filter2)) return;
		if ((flags & 0x200) && region_at(g, u->x, u->y) != reg) return;
		if ((flags & 0x10000) && u->o_up >= 0) return;  // captains only (UnitData::is_captain)
		const int32_t d = vector_dist(u->x - x, u->y - y);
		if (d <= g.find_dist && (radius < 0 || d <= radius)) {
			g.find_dist = d;
			g.find_who = cw;
			best = co;
		}
	};
	if (radius < 0 || k > 64 || g.total_units < C.radius[size_t(k)]) {
		for (int32_t p = 0; p < 8; p++) {
			if (!(LD(g, p).flags & leaderflag::kValid) || !valid_search(g, p, s, who)) continue;
			for (int32_t o = 0; o < g.objects[size_t(p)].unit_mark; o++) consider(p, o, (flags & 0x20000) != 0);
		}
		return best;
	}
	const World& w = g.world;
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
		const int32_t cx = wx + C.x[size_t(i)], cy = wy + C.y[size_t(i)];
		if (!w.in_w(cx, cy)) continue;
		const WData& d = w.w(cx, cy);
		if ((flags & 0x200) && d.region != reg) continue;
		int32_t co = d.down, cw = d.down_who;
		while (co >= 0) {
			const Object* ob = g.object(cw, co);
			if (!ob) break;
			const int32_t no = ob->down, nw = ob->down_who;
			if (cw < 8 && valid_search(g, cw, s, who) && ob->is_unit) consider(cw, co, false);
			co = no;
			cw = nw;
		}
	}
	return best;
}

int32_t objects_find_building(const Game& g, int32_t x, int32_t y, int32_t s, int32_t who, int32_t radius,
                              int32_t flags, int32_t filter, int32_t f1, int32_t f2) {
	const int16_t reg = (flags & 0x200) ? region_at(g, x, y) : int16_t(-1);
	g.find_who = who;
	g.find_dist = 99999999;
	int32_t best = -1;
	// The distance is taken between tcoords (div_3_table[c >> 6]) in both modes (disassembly).
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	auto consider = [&](int32_t cw, int32_t co) {
		const Build* b = g.build(cw, co);
		if (!b || !b->alive() || !(b->flags & objflag::kActive)) return;
		if (!valid_filter(g, cw, co, f1, f2, filter)) return;
		if ((flags & 0x200) && region_at(g, b->x, b->y) != reg) return;
		const int32_t d = vector_dist(tcoord_of(b->x) - tx, tcoord_of(b->y) - ty);
		if (d <= g.find_dist && (radius < 0 || d <= radius)) {
			g.find_dist = d;
			g.find_who = cw;
			best = co;
		}
	};
	const int32_t k = (radius + 0x2ff) / 0x300;
	if (radius < 0 || k > 9) {
		for (int32_t p = 0; p < 8; p++) {
			if (!(LD(g, p).flags & leaderflag::kValid) || !valid_search(g, p, s, who)) continue;
			for (int32_t o = kBuildBase; o < g.objects[size_t(p)].build_mark; o++) consider(p, o);
		}
		return best;
	}
	const Circles& C = circles();
	const World& w = g.world;
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
		const int32_t cx = wx + C.x[size_t(i)], cy = wy + C.y[size_t(i)];
		if (!w.in_w(cx, cy)) continue;
		const WData& d = w.w(cx, cy);
		if ((flags & 0x200) && d.region != reg) continue;
		int32_t co = d.down, cw = d.down_who;
		while (co >= 0) {
			const Object* ob = g.object(cw, co);
			if (!ob) break;
			const int32_t no = ob->down, nw = ob->down_who;
			if (cw < 8 && valid_search(g, cw, s, who) && !ob->is_unit) consider(cw, co);
			co = no;
			cw = nw;
		}
	}
	return best;
}

// ---------------------------------------------------------------- world helpers (ai_internal.hpp)

namespace ai {

namespace {
// grid_index_x / grid_index_y @adecf0 / @aded30: the 16 tcoords of a 4x4 block, centre four first; grid_threes
// @adeca0: four 5-cell L-shaped clearances (compass.obj tables).
constexpr int8_t kGridX[16] = {1, 2, 1, 2, 0, 1, 2, 3, 0, 1, 2, 3, 0, 3, 0, 3};
constexpr int8_t kGridY[16] = {1, 1, 2, 2, 0, 0, 0, 0, 3, 3, 3, 3, 1, 1, 2, 2};
constexpr int8_t kGridThrees[4][5] = {{9, 10, 11, 13, 15}, {12, 14, 8, 9, 10}, {14, 12, 4, 5, 6}, {5, 6, 7, 13, 15}};
}  // namespace

int32_t space_at_corner(const Game& g, int32_t tx, int32_t ty, int32_t who, int32_t city_only) {
	const World& w = g.world;
	std::array<int32_t, 16> blocked{};
	int32_t n = 0;
	for (int32_t i = 0; i < 16; i++) {
		const int32_t x = tx + kGridX[i], y = ty + kGridY[i];
		bool bad = !w.in_t(x, y);
		if (!bad) {
			const uint16_t m = w.t(x, y);
			const int32_t owner = w.w(x >> 2, y >> 2).who;
			bad = (!(m & tmask::kCity) && city_only != 0) || (m & 3) == 3 || (m & tmask::kStarted) ||
			      (owner >= 0 && owner != who) || (m & tmask::kBlocked);
		}
		if (bad) {
			if (i < 4) return 0;
			blocked[size_t(i)] = 1;
			n++;
		}
	}
	if (n == 0) return 4;
	if (n >= 8) return 2;
	for (const auto& row : kGridThrees) {
		bool all = true;
		for (int8_t k : row) all &= blocked[size_t(k)] == 0;
		if (all) return 3;
	}
	return 2;
}

int32_t check_building_wcoord(const Game& g, int32_t wx, int32_t wy, int32_t who, int32_t sx, int32_t sy, int32_t sum,
                              int32_t city_only) {
	const WData& d = g.world.w(wx, wy);
	if ((d.who >= 0 && d.who != who) || (d.flags & 0x70) || d.blocked == 0x10) return 0;
	int32_t best = 0;
	for (int32_t dx = -sx; dx <= sx; dx++) {
		for (int32_t dy = -sy; dy <= sy; dy++) {
			if (!(dx == 0 || dy == 0 || std::abs(dx) + std::abs(dy) <= sum)) continue;
			best = std::max(best, space_at_corner(g, 4 * wx + dx, 4 * wy + dy, who, city_only));
			if (best == 4) return 4;
		}
	}
	return best;
}

namespace {
// The goods test of World::gather_at: timber / metal / oil (type_is 1, 4 or 5) versus the others; non-basic types
// would ask GoodType::is_rare (vslot 0x94), unreachable for goods 0..5.
bool tmo_good(const Game& g, int32_t k) {
	const Type& t = g.type(k);
	return type_is(t, 1, false) || type_is(t, 4, false) || type_is(t, 5, false);
}
}  // namespace

void gather_at(const Game& g, int32_t* out, int32_t wx, int32_t wy, int32_t single) {
	for (int32_t k = 0; k < 6; k++) out[k] = 0;
	const World& w = g.world;
	const auto& lands = g.rules->lands;
	const int32_t li = w.gather_land(wx, wy);
	const bool gathered_c = (w.t(4 * wx + 2, 4 * wy + 2) & tmask::kGathered) != 0;
	if (li >= 0 && size_t(li) < lands.size()) {
		for (const LandMake& m : lands[size_t(li)]) {
			if (m.num == 0 || uint32_t(m.good) >= 6u) continue;
			if (!tmo_good(g, m.good)) {
				out[m.good] += m.num;
			} else if (single != 0 && !gathered_c) {
				out[m.good] += m.num * 2;
			}
		}
	}
	if (single != 0) return;
	for (int32_t k = 0; k < 9; k++) {
		const int32_t x = wx + move_x[k], y = wy + move_y[k];
		if (!w.in_w(x, y) || (w.t(4 * x + 2, 4 * y + 2) & tmask::kGathered)) continue;
		const WData& d = w.w(x, y);
		int32_t l = d.land;
		if (d.flags & wflag::kCoast) l = 3;
		else if (d.flags & wflag::kForest) l = 4;
		else if (d.flags & (wflag::kMountain | wflag::kCliff)) l = 5;
		else if (d.flags & wflag::kRock) l = ((d.flags & wflag::kOil) | 0x3000) >> 11;
		else if (World::is_ocean(d) && (d.flags & wflag::kOil)) l = 7;
		if (l < 0 || size_t(l) >= lands.size()) continue;
		for (int32_t gd = 0; gd < 6; gd++) {
			if (!tmo_good(g, gd)) continue;
			for (const LandMake& m : lands[size_t(l)]) {
				if (m.good == gd) out[gd] += m.num;
			}
		}
	}
	for (int32_t gd = 0; gd < 6; gd++) {
		if (tmo_good(g, gd)) out[gd] *= 2;
	}
}

bool is_dock_tile(const Game& g, int32_t wx, int32_t wy) {
	const World& w = g.world;
	if (!w.in_w(wx, wy)) return false;
	if (!World::is_ocean(w.w(wx, wy))) return false;
	const int32_t tx0 = 4 * wx, ty0 = 4 * wy;
	for (int32_t k = 1; k <= 4; k++) {
		int32_t ox = orthog_x[k], oy = orthog_y[k];
		if (ox > 0) ox <<= 2;
		if (oy > 0) oy <<= 2;
		int32_t x = tx0 + ox, y = ty0 + oy;
		if (!w.in_t(x, y)) continue;
		if (World::is_ocean(w.w(x >> 2, y >> 2))) continue;
		const uint16_t m = w.t(x, y);
		if ((m & 3) == 2 || (m & 0x30) == 0x30) continue;
		bool ok = true;
		if (ox == 0) {
			for (int32_t step = 0; step < 2 && ok; step++) {
				if (step != 0) y += oy < 1 ? -1 : 1;  // (oy >> 31) for oy < 1 (oy is -1 here)
				for (int32_t i = 0; i < 4; i++) {
					if (!w.in_t(tx0 + i, y)) {
						ok = false;
						break;
					}
					const uint16_t q = w.t(tx0 + i, y);
					if ((q & 0x80) || (q & 3) == 3) {
						ok = false;
						break;
					}
				}
			}
		} else {
			for (int32_t step = 0; step < 2 && ok; step++) {
				if (step != 0) x += ox < 1 ? -1 : 1;
				for (int32_t i = 0; i < 4; i++) {
					if (!w.in_t(x, ty0 + i)) {
						ok = false;
						break;
					}
					const uint16_t q = w.t(x, ty0 + i);
					if ((q & 0x80) || (q & 3) == 3) {
						ok = false;
						break;
					}
				}
			}
		}
		if (ok) return true;
	}
	return false;
}

int32_t num_coasts(const Game& g, int32_t r) {
	// Region::num_coasts @680760: sea regions 0x41..0x7e coastal to r (itself included).
	int32_t n = 0;
	for (int32_t s = 0x41; s <= 0x7e; s++) {
		bool c = false;
		if (r == s) c = true;
		else if (r < 0x40) c = (g.regions.r[size_t(r)].coast >> (s - 0x40)) & 1;
		n += c ? 1 : 0;
	}
	return n;
}

int32_t find_fort(const Game& g, int32_t wx, int32_t wy, int32_t who) {
	const auto& F = g.forts[size_t(who)];
	for (int32_t i = 0; i < LD(g, who).fort_mark && size_t(i) < F.size(); i++) {
		if (!(F[size_t(i)].fort_flags & 1)) continue;
		const Build* b = g.build(who, F[size_t(i)].o);
		if (b && wcoord_of(b->x) == wx && wcoord_of(b->y) == wy) return i;
	}
	return -1;
}

int32_t danger_at_w(const Game& g, int32_t who, int32_t wx, int32_t wy) {
	const World& w = g.world;
	if (who < 0 || who >= 8 || w.reg_size <= 0) return 0;
	const int32_t cx = wx >> 1, cy = wy >> 1;
	if (cx < 0 || cy < 0 || cx >= w.reg_xs || cy >= w.reg_ys) return 0;
	const size_t i = size_t(who) * size_t(w.reg_size) + size_t(cy) * size_t(w.reg_xs) + size_t(cx);
	return i < w.danger.size() ? w.danger[i] : 0;
}
int32_t danger_at(const Game& g, int32_t who, int32_t x, int32_t y) {
	return danger_at_w(g, who, regcell_of(x) * 2, regcell_of(y) * 2);
}

int32_t action_kind(const Unit& u) {
	const Order* a = get_action(u);
	return a ? int32_t(a->type) : 0;
}

int32_t tregion_at(const Game& g, int32_t x, int32_t y) {
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	if (!g.world.in_t(tx, ty)) return 0;
	return g.world.get_tregion(tx, ty);
}

}  // namespace ai

}  // namespace ron::sim
