// The strategy census and sites (docs/ai-spec.md 3, 4, 5): Leaders::strategy_all, Leader::plan_strategy (with the
// fog part of fog-spec 6.6), check_orphaned_buildings, compute_sites, compute_site_stats.
//
// Evidence (local, gitignored): re/decomp/ai/Leader_plan_strategy@006b9620.c, Leader_check_orphaned_buildings,
// Leader_compute_sites, Leader_compute_site_stats (+ re/decomp/ai/asm, re/decomp/ai_port/asm).
#include <algorithm>

#include "sim/ai_internal.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/orders.hpp"
#include "sim/guys.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace ai;

namespace {

bool war_between(const Game& g, int32_t a, int32_t b) {  // either side's diplos is 0
	return LD(g, a).diplos(b) == 0 || LD(g, b).diplos(a) == 0;
}

// The census region of a unit (plan_strategy): its tcoord region on the map; inside a dock, the first water tcoord of
// the dock's footprint whose region is a sea region; inside anything else, the container's region.
int32_t census_region(const Game& g, const Unit& u) {
	if (u.on_map()) return tregion_at(g, u.x, u.y);
	const Object* c = g.object(u.inside_up_who, u.inside_up);
	if (!c) return 0;
	if (c->ptype->domain != 1 || c->is_unit || !type_is(*c->ptype, ptype::kDock, false)) return tregion_at(g, c->x, c->y);
	const World& w = g.world;
	const Type& T = *c->ptype;
	const int32_t cxt = tcoord_of(c->x) * 0xc0 + ((T.x_size & 1) ? 0x60 : 0);
	const int32_t cyt = tcoord_of(c->y) * 0xc0 + ((T.y_size & 1) ? 0x60 : 0);
	const int32_t x0 = tcoord_of(cxt) - (T.x_size >> 1), y0 = tcoord_of(cyt) - (T.y_size >> 1);
	int32_t r = -1;
	for (int32_t x = x0; x < x0 + T.x_size; x++) {
		for (int32_t y = y0; y < y0 + T.y_size; y++) {
			if (!w.in_t(x, y)) continue;
			if ((w.t(x, y) & 0x30) == 0x20) {
				r = w.get_tregion(x, y);
				break;
			}
		}
		if (r > 63) break;
	}
	if (r < 0) return tregion_at(g, c->x, c->y);
	return r;
}

}  // namespace

// ---------------------------------------------------------------- Leaders::strategy_all

void leaders_strategy_all(Game& g) {
	for (int32_t who = 0; who < 8; who++) {
		const uint32_t f = LD(g, who).flags;
		if ((f & (leaderflag::kValid | leaderflag::kActive)) != (leaderflag::kValid | leaderflag::kActive)) continue;
		leader_check_explore(g, who);
		leader_plan_strategy(g, who);
		if (g.frame == 0 || (who + g.frame) % 10 == 0) leader_compute_score(g, who, 0);
		leader_diplomacy(g, who);
	}
	// (Leaders::strategy_all @6ed430 ends with Game::check_victory under semaphore byte1 & 2, balance mode: victory-spec 9.)
}

// ---------------------------------------------------------------- Leader::plan_strategy

void leader_plan_strategy(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	const int32_t period = int32_t(200 / int64_t(g.ai_speed));
	const int32_t c = (who * 25 + g.frame) % period;
	if (L.production_ai != 0) {
		leader_production_ai(g, who);
		return;
	}
	if (g.frame != 0 && c != 0) {
		if (c % 30 != 0) return;
		const int32_t t = L.make_list[0].t;
		if (uint32_t(t - kAges) > 0x54u) return;
		if (!can_pay(g, who, 0) || has_tech(g, who, t) || researching(g, who, t, -1, false, false)) return;
		make_this(g, who, 0);
		return;
	}
	auto& cities = g.cities[size_t(who)];
	// 4.1 resets
	if (L.city_num + L.village_num > 2) L.escrow_rate.fill(40);
	for (int32_t i = 0; i < L.city_mark && size_t(i) < cities.size(); i++) {
		City& C = cities[size_t(i)];
		if (!C.valid()) continue;
		C.gatherers = C.busy = C.free = 0;
		C.peasant_dist = 100;
	}
	L.active = L.combat = L.siege = L.non_siege = L.sea_combat = L.defense = L.attack = L.naval = L.air = L.missile = 0;
	L.transports = L.fishermen = L.idle_fishermen = L.peasants = L.scholars = L.merchants = 0;
	L.fighters = L.bombers = L.cruise = L.nuke = L.caras = 0;
	L.free_peasants = L.xport_peasants = L.gatherers = L.attacked = L.full_cities = 0;
	L.my_team_terr = get_team_terr(g, who);
	L.other_team_terr = L.min_other_team_terr = 0;
	for (int32_t j = 0; j < 8; j++) {
		if (j == who || !(LD(g, j).flags & leaderflag::kActive) || mutual_ally_ai(g, who, j)) continue;
		const int32_t t = get_team_terr(g, j);
		L.other_team_terr = std::max(L.other_team_terr, t);
		L.min_other_team_terr = L.min_other_team_terr == 0 ? t : std::min(t, L.min_other_team_terr);
	}
	L.filled_gather_slots.fill(0);
	// The fog part (fog-spec 6.6): ally_mask and meeting leaders whose buildings are seen.
	const uint8_t old_mask = L.ally_mask;
	L.ally_mask = uint8_t(1u << who);
	if (g.scripted_rules || g.ctw) L.ally_mask = uint8_t(L.ally_mask | g.scen_ally_mask[size_t(who)]);
	if (has_preq(g, who, ptype::kAllyLos) || g.info.reveal_map != 0) {
		for (int32_t j = 0; j < 8; j++) {
			if (!(LD(g, j).flags & leaderflag::kActive) || j == who) continue;
			if (mutual_ally_ai(g, who, j)) L.ally_mask = uint8_t(L.ally_mask | (1u << j));
			if (L.treaty(j) & 1) continue;
			bool met = false;
			const ObjectList& J = g.objects[size_t(j)];
			for (int32_t o = kBuildBase; o < J.build_mark && !met; o++) {
				const Build* b = g.build(j, o);
				if (b && b->alive() && object_is_seen(g, *b, who, false)) met = true;
			}
			if (met) leader_meet(g, who, j);
		}
	}
	if (L.ally_mask != old_mask) g.world.fog_changes++;
	for (int32_t j = 0; j < 8; j++) {
		if (LD(g, j).flags & leaderflag::kValid) LD(g, j).invaders[size_t(who)] = 0;
	}
	for (int32_t r = 0; r < 127; r++) {
		L.reg_active[size_t(r)] = L.reg_combat[size_t(r)] = L.reg_attack[size_t(r)] = 0;
		if (r < 63) L.reg_naval[size_t(r)] = L.reg_transports[size_t(r)] = 0;
		if (r < 64) {
			L.reg_defense[size_t(r)] = 0;
			L.reg_attacked[size_t(r)] = 0;
			L.reg_peasants[size_t(r)] = L.reg_free_peasants[size_t(r)] = L.reg_xport_peasants[size_t(r)] = 0;
			L.reg_gatherers[size_t(r)] = L.reg_gather_slots[size_t(r)] = L.reg_land[size_t(r)] = 0;
			const auto& rb = L.reg_buildings[size_t(r)];
			L.reg_cities[size_t(r)] = int16_t(rb[size_t(ptype::kVillage - kBuilds)] + rb[size_t(ptype::kTown - kBuilds)] +
			                                  rb[size_t(ptype::kMetropolis - kBuilds)] +
			                                  rb[size_t(ptype::kForbiddenCity - kBuilds)]);
			L.reg_known_rares[size_t(r)] = 0;
			L.reg_unpack_merch[size_t(r)] = 0;
		}
	}
	// Rares discovered (new_rares): the known bit and the region count.
	for (size_t i = 0; i < L.new_rares.size(); i++) {
		const int32_t gi = L.new_rares[i];
		if (gi < 0 || size_t(gi) >= g.goods.size()) continue;
		GoodObj& gd = g.goods[size_t(gi)];
		if (who < 8 && g.info.reveal_map != 3) {
			const Leader& W = LD(g, who);
			const bool revealed = (W.flags & leaderflag::kRevealEnemy) || W.num_units[size_t(371 - kUnits)] != 0 ||
			                      (g.world.seen2[size_t(tcoord_to_fcoord(tcoord_of(gd.y))) * size_t(g.world.fog_xs) +
			                                     size_t(tcoord_to_fcoord(tcoord_of(gd.x)))] & W.ally_mask);
			if (!revealed) continue;
		}
		gd.ever_seen = uint8_t(gd.ever_seen | (1u << who));
		const int32_t wx = wcoord_of(gd.x), wy = wcoord_of(gd.y);
		if (!g.world.in_w(wx, wy)) continue;
		const WData& d = g.world.w(wx, wy);
		const int32_t owner = d.who;
		if (owner >= 0 && owner != who && !mutual_ally_ai(g, who, owner)) continue;
		if (d.region >= 0 && d.region < 64) L.reg_known_rares[size_t(d.region)]++;
	}
	for (int32_t i = 0; i < L.city_mark && size_t(i) < cities.size(); i++) {
		if (cities[size_t(i)].valid()) cities[size_t(i)].in_port = 0;
	}
	// 4.2 unit census
	const ObjectList& OL = g.objects[size_t(who)];
	for (int32_t o = 0; o < OL.unit_mark; o++) {
		Unit* up = g.unit(who, o);
		if (!up || !up->alive() || up->o_up >= 0) continue;
		Unit& u = *up;
		if ((u.unit_masks & umask::kDecoy) || u.ptype->control_cost == 0) continue;
		const int32_t R = census_region(g, u);
		const int32_t dom = u.ptype->domain;
		L.active++;
		int32_t s = obj_attack(g, u) / 10;
		if (dom != 0) s = 10;
		const int32_t role = u.ptype->role;
		if (role & 0x10000) {
			L.attack += s;
			L.combat++;
			if (type_is_siege(*u.ptype)) L.siege++;
			else L.non_siege++;
		}
		if (dom == 1) {
			L.naval += s;
			if (s != 0) L.sea_combat++;
			if (u.ptype->carry != 0) L.transports++;
			if (u.ptype->unit_flags2 & 4) {
				L.fishermen++;
				if (u.unit_masks & umask::kPacked) L.idle_fishermen++;
			}
		}
		if (dom == 2) {
			if (!(u.ptype->obj_masks & 0x8000000)) {
				L.air++;
				if (unit_is(g, u, 287, false)) L.fighters++;
				else if (unit_is(g, u, ptype::kBomber, false)) L.bombers++;
			} else {
				L.missile++;
				if (unit_is(g, u, 313, false)) L.cruise++;
				else if (unit_is(g, u, ptype::kNuke, false)) L.nuke++;
			}
		}
		if ((role & 0x10010) == 0x10000) {
			const int32_t wx = wcoord_of(u.x), wy = wcoord_of(u.y);
			if (g.world.in_w(wx, wy)) {
				const int32_t k = g.world.w(wx, wy).who;
				if (k >= 0 && k != who && (LD(g, k).flags & leaderflag::kValid)) LD(g, k).invaders[size_t(who)]++;
			}
		}
		const int32_t t = u.ptype->type;
		if (t == 52 || t == 53) {
			L.scholars++;
			if (!u.on_map()) {
				const Object* in = g.object(u.inside_up_who, u.inside_up);
				if (in && type_is(*in->ptype, ptype::kUniversity, false)) L.filled_gather_slots[3]++;
			}
		} else if (t == 61 || t == 62 || t == 400) {
			L.merchants++;
			if (!(u.unit_masks & umask::kPacked) && R >= 0 && R < 64) L.reg_unpack_merch[size_t(R)]++;
		} else if (unit_is(g, u, ptype::kCara, false)) {
			L.caras++;
		}
		if (t == 50 || t == 51) {
			int32_t cidx = objects_find_city(g, u.x, u.y, search::kFriendly, who, 0x200, filter::kAll, 0, 0);
			const int32_t fdist = g.find_dist;
			L.peasants++;
			if (!u.on_map()) {
				const Object* in = g.object(u.inside_up_who, u.inside_up);
				if (in && type_is(*in->ptype, ptype::kOilPlatform, false)) L.filled_gather_slots[5]++;
				if (in && in->is_unit && in->ptype->domain == 1) {
					const Unit& tr = static_cast<const Unit&>(*in);
					const Order* a = tr.front();
					if (a && order_is_move(a->type)) {
						const int32_t r2 = g.world.in_w(wcoord_of(a->x), wcoord_of(a->y))
						                       ? g.world.w(wcoord_of(a->x), wcoord_of(a->y)).region
						                       : -1;
						if (r2 >= 0) {
							if (r2 < 64) L.reg_xport_peasants[size_t(r2)]++;
							L.xport_peasants++;
						}
					}
				}
			} else {
				if (R < 64) {
					L.reg_peasants[size_t(R)]++;
					if (L.reg_cities[size_t(R)] == 0 && action_kind(u) != kBuildAt) {
						L.reg_xport_peasants[size_t(R)]++;
						L.xport_peasants++;
					}
				}
				const Order* a = get_action(u);
				const int32_t kind = a ? int32_t(a->type) : 0;
				auto dist_min = [&](int32_t ci) {
					City& C = cities[size_t(ci)];
					const int16_t pd = int16_t(fdist / 0x300);
					if (pd < C.peasant_dist) C.peasant_dist = pd;
				};
				if (kind == 0 || kind == kExploreTo) {
					L.free_peasants++;
					if (R < 64) L.reg_free_peasants[size_t(R)]++;
					if (cidx >= 0) {
						cities[size_t(cidx)].free++;
						dist_min(cidx);
					}
				} else {
					if (cidx >= 0) cities[size_t(cidx)].busy++;
					if (kind == kGather) {
						const int32_t to = a->ox, tw = a->whom;
						if (to >= 0) {
							const Object* tb2 = g.object(tw, to);
							const int32_t bt = tb2 ? tb2->type : -1;
							if (bt == ptype::kFarm) L.filled_gather_slots[0]++;
							else if (bt == ptype::kWoodcutter) L.filled_gather_slots[1]++;
							else if (bt == ptype::kMine) L.filled_gather_slots[4]++;
							else if (bt == ptype::kOilWell || bt == ptype::kOilPlatform) L.filled_gather_slots[5]++;
						}
						if (cidx >= 0) cities[size_t(cidx)].busy--;
						if (to >= 0 && tw == who) {
							const Build* tb3 = g.build(tw, to);
							if (tb3 && tb3->city >= 0) {
								cidx = tb3->city;
								cities[size_t(cidx)].busy++;
							}
						}
						L.gatherers++;
						if (cidx >= 0) {
							cities[size_t(cidx)].gatherers++;
							dist_min(cidx);
						}
						if (R < 64) L.reg_gatherers[size_t(R)]++;
					} else if (kind == kBoardShip || kind == kCastSpell) {
						const Object* tg = a->ox >= 0 ? g.object(a->whom, a->ox) : nullptr;
						if (tg) {
							const int32_t wx = wcoord_of(tg->x), wy = wcoord_of(tg->y);
							const int32_t r2 = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : -1;
							if (r2 >= 0 && r2 < 64) L.reg_xport_peasants[size_t(r2)]++;
						}
						L.xport_peasants++;
					}
				}
			}
		}
		if ((dom != 1) == (R < 64) || dom == 2) {
			if (R >= 0 && R < 127) {
				L.reg_active[size_t(R)]++;
				int16_t s2 = int16_t(obj_attack(g, u) / 10);
				if (dom != 0) s2 = 10;
				if (role & 0x10000) {
					L.reg_attack[size_t(R)] = uint16_t(L.reg_attack[size_t(R)] + s2);
					L.reg_combat[size_t(R)]++;
				}
				if (dom == 1 && R > 62) {
					L.reg_naval[size_t(R % 63)] = uint16_t(L.reg_naval[size_t(R % 63)] + s2);
					if (u.ptype->carry != 0) L.reg_transports[size_t(R % 63)]++;
				}
			}
		}
	}
	// 4.3 building census
	std::array<int32_t, 6> slots{};
	for (int32_t o = kBuildBase; o < OL.build_mark; o++) {
		const Build* b = g.build(who, o);
		if (!b || !b->alive()) continue;
		const int32_t R = g.world.w(wcoord_of(b->x), wcoord_of(b->y)).region;
		if ((b->flags & objflag::kActive) && is_gather_type(*b->ptype)) {
			int32_t k = -1;
			switch (b->type) {
			case ptype::kFarm: k = 0; break;
			case ptype::kWoodcutter: k = 1; break;
			case ptype::kMine: k = 4; break;
			case ptype::kUniversity: k = 3; break;
			case ptype::kOilWell: case ptype::kOilPlatform: k = 5; break;
			default: k = -1;
			}
			if (k >= 0) slots[size_t(k)] += b->gather_max;  // (k = -1 writes before the array in the original)
		}
		if (b->ptype->domain != 1 && R < 64 && b->ptype->attack != 0) {
			const int32_t d = type_is(*b->ptype, ptype::kTower, false) ? 1 : (type_is(*b->ptype, ptype::kFortX, false) ? 2 : 0);
			L.defense += d;
			L.reg_defense[size_t(R)] = uint16_t(L.reg_defense[size_t(R)] + d);
		}
	}
	for (int32_t k : {0, 1, 3, 4, 5}) {
		L.gather_slots[size_t(k)] = slots[size_t(k)];
		L.gather_slots_high[size_t(k)] = std::max(slots[size_t(k)], L.gather_slots_high[size_t(k)]);
	}
	L.peasant_high = std::max(L.peasants, L.peasant_high);
	L.scholar_high = std::max(L.scholars, L.scholar_high);
	L.caravan_high = std::max(L.caras, L.caravan_high);
	L.merchant_high = std::max(L.merchants, L.merchant_high);
	L.army_high = std::max(L.combat, L.army_high);
	L.city_high = std::max(L.city_num, L.city_high);
	L.village_high = std::max(L.village_num, L.village_high);
	L.population_high = std::max(L.pop, L.population_high);
	{
		int32_t n = 0;
		for (uint8_t byte : L.rare) n += __builtin_popcount(byte);
		L.resources_controlled = n;
	}
	// 4.4 city evaluation
	const Circles& CI = circles();
	for (int32_t ci = 0; ci < L.city_mark && size_t(ci) < cities.size(); ci++) {
		City& C = cities[size_t(ci)];
		if (!C.valid()) continue;
		L.city_pop_high = std::max<int32_t>(C.pop, L.city_pop_high);
		const Build* cb = g.build(who, C.o);
		if (!cb) continue;
		const int32_t ct = cb->type;
		const int32_t level = ct == ptype::kTown ? 2 : (ct == ptype::kMetropolis || ct == ptype::kForbiddenCity) ? 3 : 1;
		int32_t rr = K("city_center_radius") + (level - 1) * K("city_center_pop_radius");
		if (has_tribe_bonus(g, who, 0x15)) rr += K("indians_city_radius");
		if (rr > 0x40) rr = 0x40;
		const int32_t n = (rr + 2) / 4;
		const int32_t cx = wcoord_of(cb->x), cy = wcoord_of(cb->y);
		C.ocean = 0;
		C.ocean_filled = 0;
		C.land = 0;
		C.filled = 0;
		C.dock_tile = 0;
		C.ter.fill(0);
		C.space = {0, 0, 0};
		L.reg_gather_slots[size_t(C.reg & 63)] =
		    uint16_t(L.reg_gather_slots[size_t(C.reg & 63)] + city_count_gather_slots(g, who, ci, nullptr, nullptr));
		if (C.city_flags & cityflag::kUnderAttack) {
			L.attacked++;
			if (C.reg < 64) L.reg_attacked[size_t(C.reg)]++;
		}
		const int32_t lim_land = CI.radius[size_t(std::min(n, 64))];
		const int32_t lim = CI.radius[size_t(std::min(n + 1, 64))];
		// The tile loop reads circle_x[k + 1] / circle_y[k + 1] (@6bb6f7: [k + 0xcb7e91], the other users index from 0xcb7e90): the centre
		// entry 0 is never visited, tile i = k + 1 runs 1 .. radius[n + 1] - 1, and the land test is `i < radius[n]` (@6bb86c).
		for (int32_t k = 1; k < lim; k++) {
			const int32_t x = cx + CI.x[size_t(k)], y = cy + CI.y[size_t(k)];
			if (!g.world.in_w(x, y)) continue;
			const WData& d = g.world.w(x, y);
			if (d.who >= 0 && d.who != who) continue;
			if (!(d.flags & wflag::kHalfLand) && (d.land == 2 || d.land == 1)) {
				C.ocean++;
				const int32_t rg = d.region;
				if ((num_coasts(g, rg) > 1 || g.world.size / 10 <= g.regions.r[size_t(rg & 127)].size) && C.dock_tile == 0 &&
				    is_dock_tile(g, x, y))
					C.dock_tile++;
				continue;
			}
			if (!(k < lim_land)) continue;
			if (d.region != C.reg) continue;
			if ((d.flags & 0x70) == 0) {
				C.land++;
				const int32_t q = check_building_wcoord(g, x, y, who, 0, 0, 1, 1);
				for (int32_t sz = 2; sz <= q; sz++) {
					if (sz < 5) C.space[size_t(sz - 2)]++;
				}
				if (q < 4) {
					C.filled++;
					continue;
				}
			}
			int32_t gat[6];
			gather_at(g, gat, x, y, 1);
			for (int32_t gd = 0; gd < 6; gd++) C.ter[size_t(gd)] = uint8_t(std::max<int32_t>(gat[gd], C.ter[size_t(gd)]));
		}
		if (int32_t(C.land) - int32_t(C.filled) < 2) L.full_cities++;
		L.reg_land[size_t(C.reg & 63)] = uint16_t(L.reg_land[size_t(C.reg & 63)] + (C.land - C.filled));
	}
	// Diplomacy counts.
	L.allies = L.active_wars_with = L.active_wars = L.wars = 0;
	for (int32_t j = 0; j < 8; j++) {
		if (!(LD(g, j).flags & leaderflag::kActive) || j == who || !(L.treaty(j) & 1)) continue;
		if (war_between(g, who, j)) L.wars++;
		else if (L.diplos(j) == 2 && LD(g, j).diplos(who) == 2) L.allies++;
	}
	// Region strategy.
	for (int32_t r = 0; r < 64; r++) {
		L.reg_wars[size_t(r)] = L.reg_allies[size_t(r)] = L.reg_neutrals[size_t(r)] = 0;
		L.strategy[size_t(r)] = 0;
		int32_t all = 0;
		for (int32_t j = 0; j < 8; j++) {
			if (LD(g, j).flags & leaderflag::kValid) all += uint16_t(LD(g, j).reg_cities[size_t(r)]);
		}
		if (all * 50 < g.regions.r[size_t(r)].size) {
			L.strategy[size_t(r)] = 1;
		} else if (L.reg_cities[size_t(r)] != 0) {
			L.strategy[size_t(r)] = 8;
		} else {
			continue;
		}
		if (L.reg_cities[size_t(r)] == 0) continue;
		uint16_t& S = L.strategy[size_t(r)];
		for (int32_t j = 0; j < 8; j++) {
			const Leader& J = LD(g, j);
			if (!(J.flags & leaderflag::kActive) || j == who || !(L.treaty(j) & 1) || J.reg_cities[size_t(r)] == 0) continue;
			if (war_between(g, who, j)) {
				L.active_wars++;
				L.active_wars_with |= 1 << j;
				L.reg_wars[size_t(r)]++;
				if (L.attack < J.attack && (r == L.home_reg || uint16_t(J.reg_cities[size_t(r)]) < uint16_t(L.reg_cities[size_t(r)])))
					S = uint16_t((S & 0xfff5) | 4);
				else
					S = uint16_t((S & 0xfffb) | 2);
				if (get_diff(g, who) < 2) S = uint16_t((S & 0xfffd) | 4);
				else if (g.info.starting_resources == 8) S = uint16_t((S & 0xfffb) | 2);
			} else if (L.diplos(j) == 2 && J.diplos(who) == 2) {
				L.reg_allies[size_t(r)]++;
			} else {
				L.reg_neutrals[size_t(r)]++;
			}
		}
		const int32_t sm = g.world.sea_map;
		if (sm < 3) {
			if (sm < 1) {
				S = uint16_t(S & 0xfff7);
			} else if (sm == 2 && (S & 6) == 0) {
				for (int32_t r2 = 1; r2 < 64; r2++) {
					const Region& RG = g.regions.r[size_t(r2)];
					if (r2 == r || RG.size == 0 || !(RG.flags & 8) || L.reg_cities[size_t(r2)] != 0) continue;
					bool any = false;
					for (int32_t j = 0; j < 8; j++) {
						if ((LD(g, j).flags & leaderflag::kValid) && LD(g, j).reg_cities[size_t(r2)] != 0) any = true;
					}
					if (!any) S = uint16_t(S | 8);
				}
			}
		} else if ((S & 6) == 0) {
			S = uint16_t(S | 8);
		}
		if ((L.flags & 0xc) != leaderflag::kHuman) {
			int32_t n = 0;
			for (const Army& A : g.armies[size_t(who)]) {
				if (A.valid && (A.status & armystat::kMustering) && A.reg == r) n++;
			}
			for (const Army& A : g.armies[size_t(who)]) {
				if (A.valid && (A.status & armystat::kDefending) && A.reg == r) n++;
			}
			if (n < 2) {
				int32_t best = -1, bv = 0;
				for (int32_t ci = 0; ci < L.city_mark && size_t(ci) < cities.size(); ci++) {
					const City& C = cities[size_t(ci)];
					if (!C.valid() || C.reg != r) continue;
					bool homed = false;
					for (const Army& A : g.armies[size_t(who)]) {
						if (A.valid && A.city == ci && (A.status & armystat::kMustering)) {
							homed = true;
							break;
						}
					}
					if (homed) continue;
					const int32_t lv = city_level(g, who, ci) * 20;
					int32_t sc = ((city_count_buildings(g, who, ci, ptype::kBarracks, 0, 1) +
					               city_count_buildings(g, who, ci, ptype::kSiegeFactory, 0, 1) +
					               city_count_buildings(g, who, ci, ptype::kStable, 0, 1)) *
					                  4 +
					              1 + city_count_buildings(g, who, ci, ptype::kDock, 0, 1)) *
					             lv;
					if (C.city_flags & cityflag::kUnderAttack) sc /= 3;
					const int32_t a = armies_find_army(g, who, C.x, C.y, -1, -1);
					const int32_t f = a < 0 ? 12 : g.armies_find_dist / 4 + 6;
					if (bv <= sc * f) {
						best = ci;
						bv = sc * f;
					}
				}
				if (best >= 0) armies_init_army(g, who, best);
			}
		}
	}
	// 4.5
	check_orphaned_buildings(g, who);
	compute_sites(g, who, 0);
	L.production_ai = 1;
}

// ---------------------------------------------------------------- check_orphaned_buildings

void check_orphaned_buildings(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	if ((L.flags & 0xc) == leaderflag::kHuman) return;
	auto one = [&](int32_t o) {
		Group gr;
		group_clear(gr, -1);
		gr.stamp = g.frame;
		group_add(g, gr, o, who, 0, 0);
		return gr;
	};
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		Build* b = g.build(who, o);
		if (!b || !b->alive()) continue;
		if (b->build_masks & bmask::kPathing) {
			b->build_masks &= ~bmask::kPathing;
			if (!(b->flags & objflag::kCity)) {
				const Type& T = *b->ptype;
				if (type_is(T, ptype::kTower) || type_is(T, ptype::kFortX) || type_is(T, ptype::kAirbase) || T.attack != 0 ||
				    L.num_buildings[size_t(b->type - kBuilds)] > 1) {
					Group gr = one(o);
					group_action_disband(g, gr, 0);
					continue;
				}
			}
		}
		if (b->flags & objflag::kActive) continue;
		const int32_t R = g.world.w(wcoord_of(b->x), wcoord_of(b->y)).region;
		bool being_built = false, cleared = false;
		for (int32_t u = 0; u < g.objects[size_t(who)].unit_mark; u++) {
			Unit* up = g.unit(who, u);
			if (!up || !up->alive() || !up->on_map() || (up->type != 50 && up->type != 51)) continue;
			const Order* a = update_action(g, *up);
			if (!a || a->type != kBuildAt || a->ox != o || a->whom != who) continue;
			if (g.world.w(wcoord_of(up->x), wcoord_of(up->y)).region == R || b->ptype->domain != 0) {
				being_built = true;
				break;
			}
			clear_orders(g, *up);
			cleared = true;
		}
		if (being_built || cleared) continue;
		const int32_t dz = danger_at(g, who, b->x, b->y);
		if (b->job_counter == 0 && dz > 0) {
			Group gr = one(o);
			group_action_disband(g, gr, 0);
			continue;
		}
		bool sent = false;
		if (dz < 1) {
			for (int32_t u = 0; u < g.objects[size_t(who)].unit_mark && !sent; u++) {
				Unit* up = g.unit(who, u);
				if (!up || !up->alive() || !up->on_map() || (up->type != 50 && up->type != 51)) continue;
				const int32_t k = action_kind(*up);
				if (!(k == 0 || k == kGather)) continue;
				if (g.world.w(wcoord_of(up->x), wcoord_of(up->y)).region != R) continue;
				Group gr;
				group_clear(gr, -1);
				group_add(g, gr, u, who, 0, 0);
				group_action_swarm_around(g, gr, o, who, kQueueNew, kBuildAt, true);
				sent = true;
			}
		}
		if (!sent) b->build_masks |= bmask::kOrphan;
	}
}

// ---------------------------------------------------------------- sites

void compute_site_stats(Game& g, int32_t who, int32_t wx, int32_t wy, int32_t city, int32_t o, int32_t reg,
                        int32_t* val, int32_t* dist, int32_t scan, int32_t* bx, int32_t* by) {
	Leader& L = LD(g, who);
	const World& w = g.world;
	*val = 0;
	*dist = 0;
	if (w.t(4 * wx + 2, 4 * wy + 2) & tmask::kCity) return;
	if (!was_seen(g, 2 * wx + 1, 2 * wy + 1, who)) return;
	bool local = false;
	{
		const int32_t owner = w.w(wx, wy).who;
		if (owner >= 0 && owner != who) {
			if (!g.ctw || L.city_num != 0 || !is_ally(g, owner, who)) return;
			local = true;
		}
	}
	if (World::is_ocean(w.w(wx, wy))) return;
	int32_t best = w.w(wx, wy).val;
	int32_t enemy = 1;
	if (blocked_site(g, ptype::kVillage, wx * 0x300, wy * 0x300, who, -1, nullptr) != 0) best = 0;
	const int32_t ox = wx, oy = wy;  // the original cell (the ocean test below)
	int32_t px = wx, py = wy;        // param_1 / param_2: moved to the best cell
	int32_t bad = 0, water = 0, forts = 0;
	for (int32_t k = 0; k < 25; k++) {
		const int32_t x = ox + move_x[k], y = oy + move_y[k];
		if (!w.in_w(x, y) || !was_seen(g, 2 * x + 1, 2 * y + 1, who)) continue;
		const WData& d = w.w(x, y);
		const int32_t owner = d.who;
		if (!World::is_ocean(d) && !(d.flags & wflag::kHalfLand)) {
			if (owner != who) {
				if (owner < 0) bad += L.city_num > 2 ? 2 : 1;
				else if (g.info.team_style == 2) {
					if (get_target(g, who) == owner) {
						enemy++;
						bad += 2;
					}
				} else if (!is_ally(g, who, owner)) {
					bad += 2;
				}
			}
		} else {
			water++;
			bad = bad == 0 ? 0 : bad - 1;
		}
		const bool mine = (owner >= 0 && owner == who) || (local && owner >= 0 && is_ally(g, who, owner));
		if (!mine || scan == 0 || (w.t(4 * x + 2, 4 * y + 2) & tmask::kCity)) continue;
		if (find_fort(g, x, y, who) >= 0) forts++;
		uint32_t v = d.val;
		if (w.in_w(x + 1, y + 1)) v = v + w.w(x + 1, y).val + w.w(x + 1, y + 1).val + w.w(x, y + 1).val;
		v = (v >> 2) + uint32_t(find_tcoord_z_at(g, x * 0x300 + 0x180, y * 0x300 + 0x180, 0) / 25);
		if (best < int32_t(v) && blocked_site(g, ptype::kVillage, x * 0x300, y * 0x300, who, -1, nullptr) == 0) {
			if (bx) *bx = x;
			if (by) *by = y;
			px = x;
			py = y;
			best = int32_t(v);
		}
	}
	if (best < 1) return;
	const int32_t best_lo = best & 3;
	int32_t b2 = best;
	if (w.sea_map > 2) {
		int32_t docks = L.num_buildings[size_t(ptype::kDock - kBuilds)];
		const int32_t to = g.type(ptype::kDock).to;
		if (to >= 0) docks += get_buildings(g, who, to, L.num_buildings[size_t(to - kBuilds)]);
		if (docks == 0) {
			for (int32_t k = 81; k < 121; k++) {
				const int32_t x = ox + move_x[k], y = oy + move_y[k];
				if (w.in_w(x, y) && World::is_ocean(w.w(x, y))) {
					b2 = best * 30;
					break;
				}
			}
		}
	}
	int32_t v = (b2 * 250) / (water + 1);
	if (L.city_num == 1) {
		if (bad != 0) v = ((std::min(bad, 9) + 7) * v) / 8;
	} else if (L.city_num == 2) {
		if (bad != 0) v = ((std::min(bad, 4) + 7) * v) / 8;
	} else if (forts != 0) {
		v = (v * 3) / 2;
	}
	v = ((enemy + 1) * v) / 2;
	const int32_t ms = g.info.map_style;
	if (ms != 0xc && ms != 0x11) {
		const int32_t xs = w.xs, ys = w.ys;
		bool edge;
		if (L.city_num < 3) {
			if (px < xs / 5 || (xs * 4) / 5 < px) v /= 4;
			edge = !(ys / 5 <= py && py <= (ys * 4) / 5);
		} else {
			if (px < xs / 10 || (xs * 9) / 10 < px) v /= 4;
			edge = !(ys / 10 <= py && py <= (ys * 9) / 10);
		}
		if (edge) v /= 4;
	}
	v /= std::max(1, danger_at_w(g, who, px, py));
	{
		const int32_t o2 = w.w(px, py).who2;
		if (o2 != who) {
			if (o2 < 0) v *= 2;
			else if (!is_ally(g, o2, who)) v <<= 2;
			else v /= 2;
		}
	}
	if (ms != 0x14) {
		const uint32_t rc = uint16_t(L.reg_cities[size_t(reg & 63)]);
		if (rc == 0) v *= 2;
		if (rc == 1) v *= 2;
		const uint32_t rl = L.reg_land[size_t(reg & 63)];
		if (rl == 0) v <<= 2;
		if (rl < 3) v <<= 2;
		if (rl < rc) v *= 2;
		if (rl < rc * 2) v *= 2;
	}
	const uint8_t goods_bits = w.w(px, py).goods;
	if (L.city_num == 0) {
		if (objects_find_unit(g, px * 0x300 + 0x180, py * 0x300 + 0x180, search::kFriendly, who, -1, 0, filter::kType, 50,
		                      0, filter::kAll, 0, 0) >= 0)
			v /= g.find_dist / 0xc00 + 1;
		if (g.info.starting_resources != 8) {
			bool any = false;
			for (int32_t k = 0; k < 5 && !any; k++) {
				const int32_t x = px + corner_x5[k], y = py + corner_y5[k];
				if (w.in_w(x, y) && (w.w(x, y).goods & 2)) any = true;
			}
			if (!any) return;
		}
	}
	int32_t has = 0, miss = 0;
	for (int32_t gd = 0; gd < 6; gd++) {
		if (type_avail(g, who, gd, 1) == 0) continue;
		if (goods_bits & (1u << gd)) {
			const int32_t e = L.econ[size_t(gd)];
			has |= e & 6;
			if (e & 2) v = (v * 3) / 2;
			if (e & 4) v = (v * 3) / 2;
		} else {
			miss |= L.econ[size_t(gd)] & 6;
		}
	}
	if (has == 0) v = (v * 2) / 3;
	if (miss & ~has) v = best_lo == 0 ? v / 2 : (v * 3) / 4;
	int32_t f, d_out;
	if (o < 0) {
		f = 3;
		d_out = 9;
	} else {
		f = 1;
		if (city >= 0) {
			int32_t acc = 1;
			const auto& cl = g.cities[size_t(who)];
			for (int32_t i = 0; i < L.city_mark; i++) {
				const City& C = cl[size_t(i)];
				if (!C.valid() || reg != C.reg) return;  // (val and dist stay 0: original behaviour)
				const int32_t d = vector_dist(px - wcoord_of(C.x), py - wcoord_of(C.y));
				if (d < 5) v /= 2;
				if (d < 8) v /= 2;
				acc += L.city_num > 2 ? std::min(d, 10) : d;
			}
			f = acc;
		}
		d_out = f * f * 2;
	}
	v = f * v;
	if (ms != 0xc && ms != 0x11 && (L.city_num == 1 || g.info.team_style == 2)) {
		int32_t s = 0;
		for (int32_t j = 0; j < 8; j++) {
			const Leader& J = LD(g, j);
			if (!(J.flags & leaderflag::kActive) || j == who || mutual_ally_ai(g, who, j)) continue;
			if (g.info.team_style == 2 && L.city_num >= 2 && get_target(g, who) != j) continue;
			const auto& cl = g.cities[size_t(j)];
			for (int32_t i = 0; i < J.city_mark && size_t(i) < cl.size(); i++) {
				const City& C = cl[size_t(i)];
				if ((C.city_flags & 0x11) != 0x11) continue;
				const int32_t d = vector_dist(px - wcoord_of(C.x), py - wcoord_of(C.y));
				d_out = d / std::max(1, g.info.num_nations);  // the dist output is overwritten (local reuse)
				s += d_out;
			}
		}
		if (g.info.team_style == 2) s *= 100;
		if (s != 0) v /= s;
	}
	if (local) v /= 10;
	*val = v;
	*dist = d_out;
}

void compute_sites(Game& g, int32_t who, int32_t force) {
	Leader& L = LD(g, who);
	auto& S = L.sites;
	auto locate = [&](int32_t reg, int32_t x, int32_t y, int32_t* c_out) {
		int32_t c = -1, o = -1;
		if (L.reg_cities[size_t(reg & 63)] == 0) {
			o = objects_find_unit(g, x, y, search::kFriendly, who, -1, 0x200, filter::kBaseType, 50, 0, filter::kAll, 0, 0);
		} else {
			c = objects_find_city(g, x, y, search::kFriendly, who, 0x200, filter::kAll, 0, 0);
			o = c < 0 ? -1 : g.cities[size_t(who)][size_t(c)].o;
		}
		*c_out = c;
		return o;
	};
	if (g.frame == 0) {
		for (Site& s : S) s = Site();
	} else {
		for (Site& s : S) {
			if (s.rank < 6 || s.val < 1) {
				s = Site();
				continue;
			}
			int32_t c;
			const int32_t o = locate(s.reg, s.wx * 0x300 + 0x180, s.wy * 0x300 + 0x180, &c);
			int32_t v, d, bx = s.wx, by = s.wy;
			compute_site_stats(g, who, s.wx, s.wy, c, o, s.reg, &v, &d, 0, &bx, &by);
			s.val = v;
			s.dist = d;
			s.wx = bx;
			s.wy = by;
		}
	}
	if (!(force != 0 || (L.flags & 0xc) != leaderflag::kHuman)) return;
	for (int32_t r = 0; r < 64; r++) {
		if (L.reg_peasants[size_t(r)] == 0) continue;
		const Region& RG = g.regions.r[size_t(r)];
		if (RG.coords.empty()) continue;
		const int32_t cxw = RG.coords[0].x * 0x300 + 0x180, cyw = RG.coords[0].y * 0x300 + 0x180;
		int32_t c;
		const int32_t o = locate(r, cxw, cyw, &c);
		int32_t step = 2;
		uint32_t start = 0;
		const int32_t size = RG.size;
		if (size > 199) {
			const int32_t xs = g.world.xs;
			const int32_t k = (xs == 1 || xs - 1 < 0) ? 0 : rnd_raw(g) % xs;  // draw A
			const int32_t q0 = size / (xs - k / 2);
			step = q0 > 10 ? q0 : 10;
			start = L.site_mark % uint32_t(step);
			const int32_t q = step / 4;
			const int32_t add = q - 1 < 1 ? 0 : rnd_raw(g) % q;  // draw B
			L.site_mark += uint32_t(add + 3);
		}
		for (int32_t i = int32_t(start); i < RG.size; i += step) {
			if (size_t(i) >= RG.coords.size()) break;
			int32_t bx = RG.coords[size_t(i)].x, by = RG.coords[size_t(i)].y;
			int32_t v, d;
			compute_site_stats(g, who, bx, by, c, o, r, &v, &d, 1, &bx, &by);
			bool dup = false;
			int32_t slot = -1, cur = 0;
			for (int32_t j = 0; j < 10; j++) {
				if (S[size_t(j)].wx == bx && S[size_t(j)].wy == by && S[size_t(j)].val > 0) {
					dup = true;
					break;
				}
				const int32_t sv = S[size_t(j)].val;
				if (!(cur < sv && (cur > 0 || v <= sv))) {
					slot = j;
					cur = sv;
				}
			}
			if (dup || slot < 0) continue;
			Site& s = S[size_t(slot)];
			s.wx = bx;
			s.wy = by;
			s.val = v;
			s.reg = r;
			s.dist = d;
		}
	}
	for (Site& s : S) {
		int32_t rank = 0;
		for (const Site& t : S) rank += t.val <= s.val ? 1 : 0;
		s.rank = rank;
	}
}

}  // namespace ron::sim
