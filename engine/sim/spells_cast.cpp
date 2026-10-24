// The cast bodies of docs/spells-spec.md sections 4 and 6: the spies' and special forces' casts (BRIBE, COUNTERINTEL,
// INFORMER, SABOTAGE / SNIPER), the conversions (CIVILIAN, TO ARMS, the immortals), the city casts (DEPOPULATE, DISBAND,
// MOVE CAPITAL) and the war-price helpers (Leader::pay_dow / LeaderData::afford_dow). The cast machinery
// (add_cast_order, do_cast, is_castable ...) is spells.cpp; PACK / UNPACK are packers.cpp.
//
// No cast draws from game_random itself (spells-spec 8): the draws come from the callees (Unit::swap_team ->
// Guy::init_real, Unit::set_type, the deaths).
#include <algorithm>

#include "sim/ai.hpp"
#include "sim/aircraft.hpp"
#include "sim/capture.hpp"
#include "sim/combat.hpp"
#include "sim/combat_internal.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/heroes.hpp"
#include "sim/compass.hpp"
#include "sim/production.hpp"
#include "sim/spells.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

namespace {

bool locked_teams(const Game& g) {
	const uint8_t ts = g.info.team_style;
	return !(ts == 0 || ts == 8 || ts == 11);  // Game::teams_locked @594880
}
bool active_leader(const Game& g, int32_t p) {
	return p >= 0 && p < kNumLeaders && (g.leaders[size_t(p)].flags & leaderflag::kActive) != 0;
}
int32_t owner_at(const Game& g, int32_t x, int32_t y) {
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	return g.world.in_w(wx, wy) ? int32_t(g.world.w(wx, wy).who) : -1;
}

// "refund": SpellType vslot +0xd8 (unpay_cost) and mana_burn -= min(mana_burn, MANA).
void refund(Game& g, int32_t sp, int32_t o, int32_t who) { unpay_cast_costs(g, sp, o, who); }

// The captain of an uber chain (units); a building is its own.
Object* captain_of(Game& g, Object* t, int32_t tw, int32_t* to) {
	if (!t) return nullptr;
	if (t->is_unit && static_cast<Unit*>(t)->o_up >= 0) {
		const int32_t c = obj_captain(g, *t);
		if (Object* n = g.object(tw, c)) {
			*to = c;
			return n;
		}
	}
	return t;
}

// hits(0) of the unit: the myhits term every ratio below keeps.
int32_t hits_of(const Game& g, const Unit& u) { return obj_hits(g, u); }

// The damage ratio kept across a type change (cast_civilian / cast_to_arms / cast_immortal): (damage << 8) / old hits,
// applied to the new hits with the signed >> 8 rounding of the asm.
void keep_damage_ratio(const Game& g, Unit& u, int32_t old_hits, int32_t old_damage) {
	const int32_t ratio = old_hits != 0 ? (old_damage << 8) / old_hits : 0;
	const int32_t v = hits_of(g, u) * ratio;
	u.damage = (v + ((v >> 31) & 0xff)) >> 8;
}

}  // namespace

// ---------------------------------------------------------------- BRIBE (spells-spec 4.1)

void cast_bribe(Game& g, int32_t sp, int32_t o, int32_t who, int32_t to, int32_t tw) {
	// @672d00
	if (tw == who) {
		refund(g, sp, o, who);
		return;
	}
	if (is_ally(g, who, tw) && (!locked_teams(g) || active_leader(g, tw))) {  // the gate repeats is_valid_target for allies
		refund(g, sp, o, who);
		return;
	}
	Object* T = (tw >= 0 && tw < kNumLeaders) ? g.object(tw, to) : nullptr;
	if (!T) {
		refund(g, sp, o, who);
		return;
	}
	if (T->is_unit) {
		if (has_general(g, *T, 0, hero::kTheCeo) >= 0) {  // the CEO's radius protects
			refund(g, sp, o, who);
			return;
		}
		g.leaders[size_t(who)].units_bribed++;
		unit_swap_team(g, static_cast<Unit&>(*T), who);  // the whole chain is recreated for `who` (Guy::init_real draws)
		return;
	}
	Build& b = static_cast<Build&>(*T);
	if (b.flags & objflag::kCity) {  // a city centre: only a dead teammate's, in locked teams
		if (!is_ally(g, who, tw) || active_leader(g, tw) || !locked_teams(g)) {
			refund(g, sp, o, who);
			return;
		}
		if (b.city >= 0) cities_capture_city(g, who, b.city, tw);
		else refund(g, sp, o, who);
		return;
	}
	if (owner_at(g, b.x, b.y) != who) {  // other buildings: own land only
		refund(g, sp, o, who);
		return;
	}
	if (!(is_enemy(g, who, tw) || (is_ally(g, who, tw) && !active_leader(g, tw) && locked_teams(g)))) {
		refund(g, sp, o, who);
		return;
	}
	const bool was_active = (b.flags & objflag::kActive) != 0;
	const int32_t n = build_swap_team(g, b, who);
	if (n < 0) {
		refund(g, sp, o, who);
		return;
	}
	Build& nb = *g.build(who, n);
	build_close(g, b, 0);
	if (!was_active) {  // (patch version > 3) an unfinished site is only masked
		wall_mask_me(g, nb, true, Regen::Force);
		mark_behind_tiles(g.world, *nb.ptype, tcoord_of(nb.x), tcoord_of(nb.y), true);
		return;
	}
	build_activate_full(g, nb, false, true, false);  // vslot 0x1a8
	wall_mask_me(g, nb, true, Regen::Total);
}

// ---------------------------------------------------------------- COUNTERINTEL (spells-spec 4.2)

void cast_counterintel(Game& g, int32_t o, int32_t who, int32_t to, int32_t tw) {
	// @673740. The caster object is not passed.
	(void)o;
	Object* T = (tw >= 0 && tw < kNumLeaders) ? g.object(tw, to) : nullptr;
	if (!T) return;
	T = captain_of(g, T, tw, &to);
	if (tw != who && !is_ally(g, who, tw)) {  // an enemy spy: kill it
		if (T->is_unit) {
			Unit& U = static_cast<Unit&>(*T);
			if (U.o_up < 0 && unit_curr_uber_size(g, U, false) == 1 && !(U.unit_masks & umask::kDecoy)) {
				Leader& L = g.leaders[size_t(who)];
				L.units_killed++;
				L.score_combat += get_kill_value(g, U.type, tw) / 5;
			}
		}
		object_die(g, *T, 2, -1, 0.0f);  // SHOT; no deaths_current_frame / units_lost bookkeeping here
		return;
	}
	T->infiltrated = 0;  // clears every plant (console messages to the planters: UI)
}

// ---------------------------------------------------------------- INFORMER / DOUBLE AGENT (spells-spec 4.3)

void cast_double_agent(Game& g, int32_t o, int32_t who, int32_t to, int32_t tw) {
	// @673a80
	(void)o;
	Object* T = (tw >= 0 && tw < kNumLeaders) ? g.object(tw, to) : nullptr;
	if (!T) return;
	T = captain_of(g, T, tw, &to);
	T->infiltrated = int8_t(uint8_t(T->infiltrated) | uint8_t(1u << (who & 7)));
	object_update_seen(g, *T, false);
}

// ---------------------------------------------------------------- SABOTAGE / SNIPER (spells-spec 4.4)

void cast_sabotage(Game& g, int32_t sp, int32_t o, int32_t who, int32_t to, int32_t tw) {
	// @6720e0
	Object* T = (tw >= 0 && tw < kNumLeaders) ? g.object(tw, to) : nullptr;
	if (!T || !T->alive()) {
		refund(g, sp, o, who);
		return;
	}
	const int32_t ttype = T->type;
	Leader& L = g.leaders[size_t(who)];
	if (!T->is_unit) {
		// The Elite (0x4d) / Special (0x4b) damage floors test the SPELL's own index (SpellType::is(0x4d, 1)): never true,
		// so only COMMANDO_MIN_DAMAGE applies (spells-spec 4.4).
		int32_t d = obj_hits_left(g, *T) / 2;
		d = std::max(d, RON_K(g, "commando_min_damage"));
		if (take_damage(g, *T, d, 0, 3, -1, true, 0x55555555u, -1, -1, false) != 0) {
			L.buildings_razed++;
			L.score_combat += get_kill_value(g, ttype, tw) / 5;
		}
		return;
	}
	Unit& U = static_cast<Unit&>(*T);
	if (U.o_up < 0 && unit_curr_uber_size(g, U, false) == 1 && !(U.unit_masks & umask::kDecoy)) {
		g.leaders[size_t(tw)].deaths_current_frame = int16_t(g.leaders[size_t(tw)].deaths_current_frame + 1);
		L.kills_current_frame = int16_t(L.kills_current_frame + 1);
		L.units_killed++;
		L.score_combat += get_kill_value(g, ttype, tw) / 5;
	}
	const bool vehicle = (uint32_t(U.ptype->obj_masks) & 0x200000u) != 0;
	object_die(g, U, vehicle ? 3 : 2, -1, 0.0f);
}

// ---------------------------------------------------------------- DISBAND / DEPOPULATE (spells-spec 6.1)

void cast_disband(Game& g, int32_t o, int32_t who) {
	// @671ed0
	Object* ob = g.object(who, o);
	if (!ob) return;
	if (ob->alive() && !ob->is_unit && (ob->flags & objflag::kActive)) build_plunder(g, static_cast<Build&>(*ob), who);
	if (!ob->is_unit) {
		build_disband(g, static_cast<Build&>(*ob), false);  // Object::disband(0)
		return;
	}
	// Object::disband(0) of a unit (not reachable through is_castable: DISBAND needs a building): die(rnd(3) + 1) on the map.
	Unit& u = static_cast<Unit&>(*ob);
	if (u.on_map()) {
		const int32_t d = int32_t(g.rng.get(0, 0xffff) % 3) + 1;
		object_die(g, u, d, -1, 0.0f);
	} else {
		object_die(g, u, 1, -1, 0.0f);
	}
}

void cast_depopulate(Game& g, int32_t sp, int32_t o, int32_t who) {
	// @671e50: only a finished city centre disbands; anything else refunds.
	const Object* ob = g.object(who, o);
	if (ob && !ob->is_unit && ob->alive() && (ob->flags & objflag::kActive) && (ob->flags & objflag::kCity)) {
		cast_disband(g, o, who);
		return;
	}
	refund(g, sp, o, who);
}

// ---------------------------------------------------------------- MOVE CAPITAL (spells-spec 6.2; editor / scripts only)

void cast_move_capital(Game& g, int32_t o, int32_t who) {
	// @671760
	const Build* b = g.build(who, o);
	if (!b || b->city < 0) return;
	auto& cl = g.cities[size_t(who)];
	const Leader& L = g.leaders[size_t(who)];
	for (int32_t i = 0; i < L.city_mark && size_t(i) < cl.size(); i++) {
		if (cl[size_t(i)].city_flags & cityflag::kValid) cl[size_t(i)].city_flags = uint16_t(cl[size_t(i)].city_flags & ~cityflag::kCapital);
	}
	if (size_t(b->city) >= cl.size()) return;
	cl[size_t(b->city)].city_flags = uint16_t(cl[size_t(b->city)].city_flags | cityflag::kCapital);
	cl[size_t(b->city)].founder = int8_t(who);
}

// ---------------------------------------------------------------- CIVILIAN / TO ARMS / the immortals (spells-spec 6.5, 6.6)

void cast_civilian(Game& g, int32_t o, int32_t who) {
	// @6704a0. Unit +0x54 is `rare` in the port (the union holds made_from for scholars / militia).
	Unit* u = g.unit(who, o);
	if (!u) return;
	const int32_t old = hits_of(g, *u), dmg = u->damage;
	if (u->rare == 0) u->rare = tribe_can_type(g, who, ptype::kPeasants) ? ptype::kPeasants : get_graft(g, who, ptype::kPeasants);
	unit_set_type(g, *u, current_upgrade(g, who, u->rare), 0);
	keep_damage_ratio(g, *u, old, dmg);
	u->form = 9;
}

void cast_to_arms(Game& g, int32_t o, int32_t who) {
	// @670880
	Unit* u = g.unit(who, o);
	if (!u) return;
	const int32_t old = hits_of(g, *u), dmg = u->damage;
	u->rare = u->type;
	unit_set_type(g, *u, current_upgrade(g, who, ptype::kMilitia), 0);
	keep_damage_ratio(g, *u, old, dmg);
	u->form = 0;
}

void cast_immortal(Game& g, int32_t sp, int32_t o, int32_t who) {
	// @670740 (0x297 IMMORTAL ARCHERS) / @670600 (0x298 IMMORTALS): dead in the shipped data (FROM none).
	Unit* u = g.unit(who, o);
	if (!u) return;
	constexpr int32_t kImmortals = 0xa2, kImmortalArchers = 0xb8;
	if (!unit_is(g, *u, sp == 0x297 ? kImmortals : kImmortalArchers)) return;
	const int32_t old = hits_of(g, *u), dmg = u->damage;
	u->rare = u->type;
	unit_set_type(g, *u, current_upgrade(g, who, sp == 0x297 ? kImmortalArchers : kImmortals), 0);
	keep_damage_ratio(g, *u, old, dmg);
}

// War prices (spells-spec 6.8): Leader::pay_dow / LeaderData::afford_dow are ai_diplo.cpp's (afford_dow / pay_dow).

// ---------------------------------------------------------------- the AI's target search (spells-spec 8, ai-spec 12.6.1)

bool spell_target_filter(const Game& g, int32_t sp, int32_t who, int32_t co, int32_t cw) {
	// Search::valid_filter, FILTER_SPELL_TARGET @67e385: is_valid_target, then a per-craft test.
	if (!spell_is_valid_target(g, sp, who, co, cw, nullptr)) return false;
	const Object* ob = cw >= 0 && cw < kNumLeaders ? g.object(cw, co) : nullptr;
	if (!ob) return false;
	switch (sp) {
	case 0x275:
	case 0x281:
		return ob->damage == 0;  // [obj + 0x24] == 0: only undamaged targets
	case 0x277:
		return true;
	case 0x27f:  // ❓ a building with vslot +0x4c (active) and vslot +0x178 > 0 (not decoded: taken as hits left)
		return !ob->is_unit && (ob->flags & objflag::kActive) && obj_hits_left(g, *ob) > 0;
	default:
		return false;  // 0x276 and everything else: the jump table sends them to `return 0`
	}
}

int32_t spell_find_target(const Game& g, int32_t sp, int32_t who, int32_t x, int32_t y, int32_t search, int32_t radius,
                          bool units_only, bool combat_only) {
	// ObjectsData::find @65c6b0 (units and buildings) / find_unit @65ca80 (units only, + FILTER_COMBAT) with
	// FILTER_SPELL_TARGET: the nearest object (later wins ties) of the search relation that passes the filter(s).
	g.find_who = who;
	g.find_dist = 99999999;
	int32_t best = -1;
	auto consider = [&](int32_t cw, int32_t co) {
		const Object* ob = g.object(cw, co);
		if (!ob || !ob->alive()) return;
		if (units_only && (!ob->is_unit || !static_cast<const Unit*>(ob)->on_map())) return;
		if (!spell_target_filter(g, sp, who, co, cw)) return;
		if (combat_only && !search_valid_filter(g, cw, co, -1, -1, filter::kCombat)) return;
		const int32_t d = vector_dist(ob->x - x, ob->y - y);
		if (d <= g.find_dist && (radius < 0 || d <= radius)) {
			g.find_dist = d;
			g.find_who = cw;
			best = co;
		}
	};
	const int32_t k = (radius + 0x2ff) / 0x300;
	if (radius >= 0 && k < 10) {
		const Circles& C = circles();
		const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
		for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
			const int32_t cx = wx + C.x[size_t(i)], cy = wy + C.y[size_t(i)];
			if (!g.world.in_w(cx, cy)) continue;
			const WData& d = g.world.w(cx, cy);
			int32_t co = d.down, cw = d.down_who;
			while (co >= 0) {
				const Object* ob = g.object(cw, co);
				if (!ob) break;
				const int32_t no = ob->down, nw = ob->down_who;
				if (cw < 8 && search_valid_search(g, cw, search, who)) consider(cw, co);
				co = no;
				cw = nw;
			}
		}
		return best;
	}
	for (int32_t p = 0; p < 8; p++) {
		if (!(g.leaders[size_t(p)].flags & leaderflag::kValid) || !search_valid_search(g, p, search, who)) continue;
		for (int32_t o = 0; o < g.objects[size_t(p)].unit_mark; o++) consider(p, o);
		if (!units_only)
			for (int32_t o = kBuildBase; o < g.objects[size_t(p)].build_mark; o++) consider(p, o);
	}
	return best;
}

// ---------------------------------------------------------------- the reactive casts (spells-spec 4.5)

bool spell_reactive_cast(Game& g, Unit& u, int32_t atk_o, int32_t atk_who) {
	// Unit::target_opportunity @5fffc0, the no-action branch: a spy bribes (0x275), a commando sabotages (0x280) the ground
	// unit that attacks it.
	const Object* A = (atk_who >= 0 && atk_who < kNumLeaders && atk_o >= 0) ? g.object(atk_who, atk_o) : nullptr;
	if (!A || !A->is_unit || A->ptype->domain != 0) return false;
	auto try_cast = [&](int32_t sp) {
		if (can_pay_cost(g, sp, u.who, -1, u.o, 1) == 0) return false;
		if (!spell_is_valid_target(g, sp, u.who, atk_o, atk_who, nullptr)) return false;
		const int32_t mana = g.type(sp).mana;
		if (mana != 0 && mana > unit_mana_left(g, u)) return false;
		add_cast_order(g, u, atk_o, atk_who, u.x, u.y, sp, kQueueNew, true);
		return true;
	};
	if (unit_is(g, u, ptype::kSpy) && try_cast(0x275)) return true;
	if (unit_is(g, u, 0x49) && try_cast(0x280)) return true;
	return false;
}

}  // namespace ron::sim
