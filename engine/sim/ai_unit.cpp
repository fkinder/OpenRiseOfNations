// Per-unit AI (docs/ai-spec.md 12.6): the AI branches of Unit::think after the economy part, Unit::think_scout
// (T1-T6), Unit::think_spellcaster (the S1 draw and the spy / special casts), Unit::find_goody_box and
// Unit::explore_goody (G1 draws).
//
// Evidence (local, gitignored): re/decomp/ai/Unit_think@005f6e40.c, Unit_think_scout@005f6010.c,
// Unit_think_spellcaster@005f27a0.c, Unit_explore_goody@005f9780.c, re/decomp/ai_port/Unit_find_goody_box@005f2540.c,
// ObjectsData_find_unit_ordered (+ asm: the distance is to the unit's orders_x / orders_y).
#include <algorithm>

#include "sim/ai_internal.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/heroes.hpp"
#include "sim/orders.hpp"
#include "sim/spells.hpp"
#include "sim/transport.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace ai;

namespace {

bool is_special(const Unit& u) { return (u.ptype->unit_flags2 & 0x10) != 0; }
bool is_hero(const Unit& u) { return (u.ptype->unit_flags2 & 0x20) != 0; }
bool is_supply(const Unit& u) { return (u.ptype->unit_flags2 & 0x40) != 0; }

// ObjectsData::find_unit_ordered(x, y, FRIENDLY, who, r, 0x200, BASE_TYPE bt, NOT_ME o) @65bc40: an own unit of base
// type bt (not o) whose current order is a move-family order (1-4, 18, 19, 21) heading within r of (x, y).
int32_t find_unit_ordered(const Game& g, int32_t x, int32_t y, int32_t who, int32_t r, int32_t bt, int32_t o) {
	if (who < 0 || who >= 8) return -1;
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	const int16_t reg = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : int16_t(-1);
	g.find_who = who;
	g.find_dist = 99999999;
	int32_t best = -1;
	for (int32_t k = 0; k < g.objects[size_t(who)].unit_mark; k++) {
		const Unit* u = g.unit(who, k);
		if (!u || !u->alive() || k == o || !type_is(*u->ptype, bt, false)) continue;
		const Order* a = u->front();
		if (!a) continue;
		const int32_t t = a->type;
		if (!(t == 1 || t == 2 || t == 3 || t == 4 || t == 18 || t == 19 || t == 21)) continue;
		if (g.world.w(wcoord_of(u->x), wcoord_of(u->y)).region != reg) continue;
		const int32_t d = vector_dist(u->orders_x - x, u->orders_y - y);
		if (d <= g.find_dist && d <= r) {
			g.find_dist = d;
			best = k;
		}
	}
	return best;
}

int32_t base_type_of(const Game& g, const Unit& u) {  // UnitType vslot 0xe4 (basic_type)
	int32_t t = u.ptype->type;
	for (int guard = 0; guard < 32; guard++) {
		const int32_t f = g.type(t).from;
		if (f < kUnits || f >= 402) break;
		t = f;
	}
	return t;
}

// Unit::think_spellcaster @5f27a0 (ai-spec 12.6.1, heroes-spec 4.7, spells-spec 8): `ok(s)` = SpellTypeData::is_castable(s)
// non-zero and mana() >= mana_burn + the craft's mana; `cast` = add_cast_order(QUEUE_FIRST, not explicit). The only
// game_random draw is the special units' `% 3` one (S1).
int32_t think_spellcaster(Game& g, Unit& u) {
	const Leader& L = LD(g, u.who);
	auto ok = [&](int32_t sp) {
		return spell_is_castable(g, sp, u.o, u.who, 0) != 0 && unit_mana(g, u) >= u.mana_burn + g.type(sp).mana;
	};
	auto cast = [&](int32_t sp, int32_t t, int32_t tw) {
		add_cast_order(g, u, t, tw, u.x, u.y, sp, kQueueFirst, false);
		return 1;
	};
	if (L.flags & leaderflag::kHuman) {
		// A human's special forces counter-spy by themselves: COUNTERINTEL on the nearest planted / enemy-spy object.
		if (!is_special(u) || !ok(0x277)) return 0;
		const int32_t t = spell_find_target(g, 0x277, u.who, u.x, u.y, search::kAll, spell_get_range(g, 0x277, u.who, -1, -1), false, false);
		if (t < 0) return 0;
		return cast(0x277, t, g.find_who);
	}
	if (get_diff(g, u.who) < 2 || (u.unit_masks & umask::kDecoy)) return 0;
	if (type_is(*u.ptype, ptype::kSpy)) {
		if (!unit_is_cloaked(u)) return 0;
		if (ok(0x277)) {  // find a plant on a friendly object within 5 x the range
			const int32_t t = spell_find_target(g, 0x277, u.who, u.x, u.y, search::kFriendly, 5 * spell_get_range(g, 0x277, u.who, -1, -1), false, false);
			if (t >= 0) return cast(0x277, t, g.find_who);
		}
		if (ok(0x275)) {  // bribe the nearest enemy combat unit
			const int32_t t = spell_find_target(g, 0x275, u.who, u.x, u.y, search::kEnemy, 5 * spell_get_range(g, 0x275, u.who, -1, -1), true, true);
			if (t >= 0) return cast(0x275, t, g.find_who);
		}
		return 0;
	}
	if (!is_special(u)) {
		// A general (heroes-spec 4.7, decompile @5f27a0; the spec's "two draws" belongs to the special path below): idle ->
		// DECOY unless a decoy already exists; moving (guy 0 with a non-zero avg_speed) -> FORCED MARCH on a coin
		// (one game_random draw, only while no march runs) or else AMBUSH in foreign territory.
		if (!is_hero(u)) return 0;
		auto affordable = [&](int32_t sp) { return unit_mana(g, u) >= u.mana_burn + g.type(sp).mana; };
		int32_t sp;
		if (!unit_is_moving(u)) {
			for (int32_t o = 0; o < g.objects[size_t(u.who)].unit_mark; o++) {
				const Unit* d = g.unit(u.who, o);
				if (d && d->alive() && d->o_up < 0 && (d->unit_masks & umask::kDecoy)) return 0;
			}
			if (!affordable(0x27a)) return 0;
			sp = 0x27a;
		} else {
			if (u.guys.empty() || u.guys[0].avg_speed == 0) return 0;
			if (!(L.flags & 0x8000) && (rnd_raw(g) & 1) != 0) {
				if (!affordable(0x27c)) return 0;
				sp = 0x27c;
			} else {
				const int32_t wx = wcoord_of(u.x), wy = wcoord_of(u.y);
				const int32_t terr = g.world.in_w(wx, wy) ? g.world.w(wx, wy).who : -1;
				if (terr < 0 || terr == u.who) return 0;
				if (!affordable(0x27b)) return 0;
				sp = 0x27b;
			}
		}
		add_cast_order(g, u, -1, -1, u.x, u.y, sp, kQueueFirst, false);
		return 1;
	}
	// Special units (commandos, scouts ...): SNIPER an enemy combat unit in range; else the S1 draw (always made here), then
	// COUNTERINTEL.
	if (ok(0x281)) {
		const int32_t t = spell_find_target(g, 0x281, u.who, u.x, u.y, search::kEnemy, spell_get_range(g, 0x281, u.who, -1, -1), true, true);
		if (t >= 0) return cast(0x281, t, g.find_who);
	}
	const int32_t r = rnd_raw(g);
	if (r % 3 != 0 && object_get_army(g, u) < 0) return 0;
	if (!ok(0x277)) return 0;
	const int32_t t = spell_find_target(g, 0x277, u.who, u.x, u.y, search::kAll, spell_get_range(g, 0x277, u.who, -1, -1), false, false);
	if (t < 0) return 0;
	return cast(0x277, t, g.find_who);
}

int32_t find_goody_box(Game& g, Unit& u) {  // Unit::find_goody_box @5f2540
	if (u.ptype->domain != 0) return 0;
	const World& w = g.world;
	const int32_t ux = wcoord_of(u.x), uy = wcoord_of(u.y);
	if (!w.in_w(ux, uy)) return 0;
	const int16_t reg = w.w(ux, uy).region;
	for (int32_t k = 0; k < 49; k++) {
		const int32_t x = ux + move_x[k], y = uy + move_y[k];
		if (!w.in_w(x, y)) continue;
		const WData& d = w.w(x, y);
		if (d.region != reg || !(d.flags & wflag::kGoody)) continue;
		if (!(was_seen(g, 2 * x + 1, 2 * y + 1, u.who) || was_seen(g, 2 * x, 2 * y + 1, u.who) ||
		      was_seen(g, 2 * x + 1, 2 * y, u.who) || was_seen(g, 2 * x, 2 * y, u.who)))
			continue;
		bool take = World::is_ocean(d);
		if (!take) {
			const int32_t it = find_goody_at_wcoord(g, x, y);
			take = it < 0 || item_is_seen(g, g.items[size_t(it)], u.who);
		}
		if (!take) continue;
		if (x == wcoord_of(u.orders_x) && y == wcoord_of(u.orders_y)) return 0;
		unit_get_goody_box(g, u, x, y);
		return 1;
	}
	return 0;
}

}  // namespace

int32_t ai_think_spellcaster(Game& g, Unit& u) { return think_spellcaster(g, u); }

// ---------------------------------------------------------------- explore_goody (G1)

void explore_goody(Game& g, Unit& u) {  // Unit::explore_goody @5f9780
	World& w = g.world;
	const int32_t wx = wcoord_of(u.x), wy = wcoord_of(u.y);
	if (!w.in_w(wx, wy)) return;
	const int32_t it = find_goody_at_wcoord(g, wx, wy);
	if (it >= 0) g.items[size_t(it)].flags = uint8_t(g.items[size_t(it)].flags & ~objflag::kValid);  // Item close
	WData& d = w.w(wx, wy);
	if (d.down >= 0) {  // the item terminal at the end of the object chain is cut
		int32_t co = d.down, cw = d.down_who;
		while (co >= 0) {
			Object* ob = g.object(cw, co);
			if (!ob) break;
			if (ob->down == -3) ob->down = -1;
			co = ob->down;
			cw = ob->down_who;
		}
	}
	d.flags = uint16_t(d.flags & 0x7fff);
	if (g.frame == 0) return;
	Leader& L = LD(g, u.who);
	int32_t best_g = -1, best_v = 99999999;
	for (int32_t gd = 0; gd < 6; gd++) {
		if (type_avail(g, u.who, gd, 1) == 0 || gd == 3) continue;
		const int32_t v = L.bucket[size_t(gd)] + rnd_raw(g) % 25;
		if (v < best_v) {
			best_v = v;
			best_g = gd;
		}
	}
	// Unit::explore_goody @5f9780 reads LeaderDataEncrypt +0xf4 (epoch[3]) itself; the `science` mirror is 0 after a load.
	int32_t amount = has_tribe_bonus(g, u.who, 9) ? L.epoch[3] * K("spanish_ruins") + K("spanish_ruins_base")
	                                              : L.epoch[3] * K("goody_box_age") + K("goody_box");
	if (best_g < 0) best_g = 2;
	L.bucket[size_t(best_g)] += amount;
	L.goody_box_resources += amount;
}

// ---------------------------------------------------------------- think_scout (T1-T6)

// Unit::think_civilian_transport(flag) @5f40d0, the part every unit without a transport option reaches (the corpus: the start
// scouts of a game before the Transport tech). A land unit whose leader cannot (yet) carry it, that is not flagged CAN_TRANSPORT or
// is not cargo goes back to the explorer's random walk (think_scout(1): a random passable cell of its own region) when it is
// auto-exploring, and does nothing otherwise. TODO(transports): the ferry search of the rest of the function (the coast scan with
// its group_action_move_near) is not ported; the branch returns 0 as before.
static int32_t think_civilian_transport(Game& g, Unit& u, int32_t flag) { return unit_think_ferry(g, u, flag); }

int32_t think_scout(Game& g, Unit& u, int32_t region_mode) {  // Unit::think_scout @5f6010
	const int32_t who = u.who;
	Leader& L = LD(g, who);
	const World& w = g.world;
	if (u.ptype->domain == 0 && find_goody_box(g, u)) return 1;
	if (!(L.flags & leaderflag::kHuman) && u.alive() && (u.ptype->unit_flags2 & 2) && think_spellcaster(g, u)) return 1;
	const int32_t ux = wcoord_of(u.x), uy = wcoord_of(u.y);
	const int32_t R = w.get_tregion(tcoord_of(u.x), tcoord_of(u.y));
	const Region& RG = g.regions.r[size_t(R & 127)];
	int32_t tx = region_mode, ty = region_mode;
	auto issue = [&](int32_t x, int32_t y) {
		Group tmp;
		group_clear(tmp, -1);
		group_add(g, tmp, u.o, who, 0, 0);
		const int32_t id = groups_push_group(g, who, tmp, 1);
		if (id >= 0) group_action_move_to(g, g.groups.g[size_t(id)], x * 0xc0 + 0x60, y * 0xc0 + 0x60, kQueueNew, false, 0,
		                                  kExploreTo, 0, -1, -1, 0);
		return 1;
	};
	// (A) region mode: a random passable cell of the own region.
	if (region_mode != 0 && RG.size > 5) {
		const int32_t n = RG.size;
		int32_t i = (n == 1 || n - 1 < 0) ? 0 : rnd_raw(g) % n;  // T1
		int32_t tries = 0;
		while (w.w(RG.coords[size_t(i)].x, RG.coords[size_t(i)].y).flags & 0x70) {
			i = (n == 1 || n - 1 < 0) ? 0 : rnd_raw(g) % n;  // T1'
			if (++tries > 15) return 1;
		}
		return issue(RG.coords[size_t(i)].x * 4 + 2, RG.coords[size_t(i)].y * 4 + 2);
	}
	int32_t best = 99999999;
	int32_t home = objects_find_city(g, u.x, u.y, search::kFriendly, who, 0x200, filter::kAll, 0, 0);
	if (home < 0) home = 0;
	const int32_t dom = u.ptype->domain;
	const bool auto_explore = (u.unit_masks & umask::kAutoExplore) != 0;
	// @5f6180: with AUTO_EXPLORE (0x100) the city scouting runs for every unit; without it citizens, spies and ships go straight to (C).
	bool skipB = !auto_explore && (u.type == 50 || u.type == 51 || type_is(*u.ptype, ptype::kSpy) || dom == 1);
	int32_t evals = 0;
	bool doC = skipB || L.city_num == 0;
	if (!doC) {
		// (B) scout the cities (own first, from the home city; one foreign city per leader).
		int32_t rings = 12;
		const Circles& CI = circles();
		bool stop_all = false;
		for (int32_t jj = 0; jj < 8 && !stop_all; jj++) {
			const int32_t j = (who + jj) & 7;
			const Leader& J = LD(g, j);
			if (!(J.flags & leaderflag::kActive) || J.reg_cities[size_t(R & 63)] == 0) continue;
			if (j != who && is_ally(g, j, who)) continue;
			const int32_t cm = J.city_mark;
			auto& cl = g.cities[size_t(j)];
			for (int32_t ci = 0; ci < cm && !stop_all; ci++) {
				const int32_t idx = ((j == who ? home : 0) + ci) % cm;
				if (size_t(idx) >= cl.size()) continue;
				City& C = cl[size_t(idx)];
				if (!C.valid() || (dom == 0 && C.reg != R) || (C.scouted & (1 << who))) continue;
				int32_t step = 1, lim = rings;
				if (u.unit_masks & umask::kAiControl) {
					if (j == who) step = 2;
					else rings = lim = 3;
				}
				const int32_t cwx = wcoord_of(C.x), cwy = wcoord_of(C.y);
				int32_t r = 1;
				while (r < rings) {
					const int32_t m = g.frame % 8;
					const int32_t stride = r / 4 + m + 1;
					const int32_t cr = CI.radius[size_t(r)], cr0 = CI.radius[size_t(r - 1)];
					const int32_t s0 = (cr == 1 || cr - 1 < 0) ? 0 : rnd_raw(g) % cr;  // T2
					const int32_t jit = (r / 4 + m >= 1) ? rnd_raw(g) % stride : 0;  // T3
					for (int32_t p = cr0 + jit; p < cr; p += stride) {
						const int32_t q = cr0 + (s0 + p) % (cr - cr0);
						const int32_t cx = cwx + CI.x[size_t(q)], cy = cwy + CI.y[size_t(q)];
						if (lim + 1 < r) {  // two rings past the best ring: stop this city
							r = rings + 1;
							break;
						}
						if (!w.in_w(cx, cy)) continue;
						bool eligible = dom == 2;
						if (!eligible && w.w(cx, cy).region == R) {
							const bool water = (w.t(4 * cx + 2, 4 * cy + 2) & 0x30) == 0x20;
							eligible = dom == 0 ? !water : water;
						}
						if (!eligible) continue;
						if (++evals > 0x600) {
							stop_all = true;
							break;
						}
						int32_t q2 = w.w(cx, cy).who;
						if (q2 < 0) q2 = who;
						const bool cand = !was_really_seen(g, 2 * cx + 1, 2 * cy + 1, who) ||
						                  (q2 == who && (w.w(cx, cy).flags & wflag::kGoody));
						if (!cand || unit_invalid_loc(g, u, 4 * cx + 2, 4 * cy + 2, false, false, false, false, false)) continue;
						int32_t s = 8 * vector_dist(ux - cx, uy - cy) + (rnd_raw(g) & 7);  // T4
						if (j != who && (L.treaty(j) & 3) == 0) s *= (J.flags & leaderflag::kHuman) ? 2 : 4;
						s += (q2 != who ? 4 : 0) + danger_at_w(g, who, cx, cy);
						if (L.city_num == 0 && (w.w(cx, cy).goods & 2)) s /= 2;
						else if (L.city_num == 1 && (w.w(cx, cy).goods & 0x10)) s /= 2;
						if (s < best && find_unit_ordered(g, cx * 0x300 + 0x180, cy * 0x300 + 0x180, who, 0x600,
						                                  base_type_of(g, u), u.o) < 0) {
							best = s;
							tx = 4 * cx + 2;
							ty = 4 * cy + 2;
							lim = std::min(lim, r);
						}
					}
					if (stop_all) break;
					r += step;
				}
				if (stop_all) break;
				if (r == 8 + 4 * (j == who ? 1 : 0)) C.scouted = int16_t(C.scouted | (1 << who));
				rings = 8;
				if (j != who) break;
			}
		}
		doC = best > 199;
	}
	if (doC) {
		// (C) unexplored cells of the region.
		const bool spyish = !(L.flags & leaderflag::kHuman) && (type_is(*u.ptype, ptype::kSpy) || is_special(u) || dom == 1);
		const int32_t n = std::max(1, (RG.size + 99) / 100);
		const int32_t stride = n + g.frame % 8;
		int32_t i = (stride == 1 || stride - 1 < 0) ? 0 : rnd_raw(g) % stride;  // T5
		for (; i < RG.size && size_t(i) < RG.coords.size(); i += stride) {
			const int32_t cx = RG.coords[size_t(i)].x, cy = RG.coords[size_t(i)].y;
			if (!(who < 8 && g.info.reveal_map != 3 && !(L.flags & leaderflag::kRevealEnemy) &&
			      L.num_units[size_t(371 - kUnits)] == 0 &&
			      !(w.seen2[size_t(2 * cy + 1) * size_t(w.fog_xs) + size_t(2 * cx + 1)] & L.ally_mask)))
				continue;
			if (unit_invalid_loc(g, u, 4 * cx + 2, 4 * cy + 2, false, false, false, false, false)) continue;
			bool eligible;
			if (dom == 2) eligible = true;
			else if (dom == 0 || !u.on_map()) eligible = (w.t(4 * cx + 2, 4 * cy + 2) & 0x30) != 0x20;
			else eligible = World::is_ocean(w.w(cx, cy));
			if (!eligible) continue;
			int32_t s = 16 * vector_dist(ux - cx, uy - cy) + (rnd_raw(g) & 7);  // T6
			int32_t q2 = w.w(cx, cy).who;
			if (q2 < 0) q2 = who;
			if (!spyish) {
				s += danger_at_w(g, who, cx, cy) + (q2 != who ? 4 : 0);
			} else {
				if (q2 >= 0 && !is_ally(g, who, q2)) s /= 4;
				if (w.t(4 * cx + 2, 4 * cy + 2) & 0x100) s /= 2;
			}
			if (2 * s < best &&
			    find_unit_ordered(g, cx * 0x300 + 0x180, cy * 0x300 + 0x180, who, 0x600, base_type_of(g, u), u.o) < 0) {
				best = 2 * s;
				tx = 4 * cx + 2;
				ty = 4 * cy + 2;
			}
		}
	}
	if (best > 99999998) {
		// Nothing to explore: the region counts as scouted by this leader (@5f6c10: bit `who` of Region +0x40, +0x3c cleared; not for aircraft).
		if (dom != 2) {
			Region& RM = g.regions.r[size_t(R & 127)];
			RM.scouted = uint8_t(RM.scouted | (1u << (who & 7)));
			RM.scout_mark = 0;
		}
		if (dom == 1) return unit_add_to_army(g, u) >= 0 ? 1 : 0;
		if ((u.type == 50 || u.type == 51) && !auto_explore && L.reg_cities[size_t(R & 63)] == 0) return 0;
		return think_civilian_transport(g, u, 0);  // @5f6c4c (the last statement of the function)
	}
	return issue(tx, ty);
}

// ---------------------------------------------------------------- Unit::think, AI part (12.6 steps 12-16)

void think_ai(Game& g, Unit& u, bool tick) {
	(void)tick;
	const Leader& L = LD(g, u.who);
	if (L.flags & leaderflag::kCoopSolo) return;  // step 12
	if (u.ptype->carry != 0 && !type_is(*u.ptype, ptype::kCarrier, true)) {
		return;  // step 13: TODO(ai-spec 12.6.3) think_carry (auto-transports)
	}
	if (is_supply(u) || is_hero(u)) {  // step 14
		unit_add_to_army(g, u);
		return;
	}
	if (!(u.ptype->role & 0x10) && !type_is(*u.ptype, ptype::kSpy)) return;  // step 15
	if (object_get_army(g, u) < 0) think_scout(g, u, 0);                     // step 16
}

int32_t ai_find_goody_box(Game& g, Unit& u) { return find_goody_box(g, u); }

}  // namespace ron::sim
