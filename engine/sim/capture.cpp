// City capture, ownership transfer, capital loss, defeat and victory (docs/capture-spec.md). Ported from the decompiles
// and the disassembly in re/decomp/capture/ (plus prod_c2*, econ_mkt, combat_C for the callers); every comment names the
// function it ports. Loops keep the original's order: the strength scan walks the circle table and each wcoord's
// object list newest-first, building lists follow city_down, leaders run 0..7.
#include "sim/capture.hpp"

#include <algorithm>
#include <cstdlib>

#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/combat_internal.hpp"
#include "sim/compass.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/garrison.hpp"
#include "sim/heroes.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/pathfinder.hpp"
#include "sim/production_internal.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using prod::tb;

namespace {

Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }
const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }
bool valid_active(const Leader& l) { return (l.flags & 3u) == 3u; }  // LEADER_VALID | LEADER_ACTIVE
bool leader_ok(int32_t w) { return w >= 0 && w < 8; }

// UnitData::get_capture_value @610a70 (vslot 0xec): 0 for decoys and siege, else min(uber_size, guy_mark), at least 1.
// Buildings answer 1 (the folded `return 1` in the Build vtable).
int32_t capture_value(const Object& ob) {
	if (!ob.is_unit) return 1;
	const Unit& u = static_cast<const Unit&>(ob);
	if ((u.unit_masks & umask::kDecoy) || type_is_siege(*u.ptype)) return 0;
	int32_t v = u.ptype->uber_size;
	if (u.guy_mark <= v) v = u.guy_mark;
	return v < 1 ? 1 : v;
}

// The map_sizes entry 3 width (70 in the stock rules) that scales the victory clocks by the map width
// (Game::retake_capital / wonder_timer / popwin_timer: (ms / 2 + xs * k) / ms, at least 1).
int32_t map_scaled(const Game& g, int32_t k) {
	if (k < 1) return 0;
	int32_t ms = 70;
	if (const CategoryList* cl = g.rules->category_list("map_sizes"); cl && cl->items.size() > 3) ms = cl->items[3].data[0];
	if (ms <= 0) ms = 70;
	const int32_t v = (ms / 2 + g.world.xs * k) / ms;
	return v < 1 ? 1 : v;
}

int32_t list_value(const Game& g, const char* list, int32_t idx) {
	const CategoryList* cl = g.rules->category_list(list);
	if (!cl || idx < 0 || size_t(idx) >= cl->items.size()) return 0;
	return cl->items[size_t(idx)].data[0];
}

void log_event(Game& g, const GameEvent& e) { g.events.push_back(e); }

// The poorest stockpile among goods 0..5 that both leaders can use (type_avail), knowledge excluded; strict '<' from
// 999999, so ties keep the lower good (Cities::capture_city's plunder target).
int32_t poorest_good(const Game& g, int32_t of, int32_t a, int32_t b) {
	int32_t best = 0, v = 999999;
	for (int32_t k = 0; k < 6; k++) {
		if (!type_avail(g, a, k, 1) || !type_avail(g, b, k, 1) || k == ptype::kKnowledge) continue;
		if (LD(g, of).bucket[size_t(k)] < v) {
			v = LD(g, of).bucket[size_t(k)];
			best = k;
		}
	}
	return best;
}

}  // namespace

// ---------------------------------------------------------------- swap_team

namespace {
// Wall::swap_team @640c00: Objects::init_build(who, current_upgrade(type), x, y, preplaced 1, -1), then the state of
// the original: build_masks, the ever-seen masks, flags (the copy keeps its own DETECTOR bit), the construction
// counters, damage, infiltrated, visible; Build::new_library on a copied active library (patches after Rise of
// Legends); update_hits(0), update_los.
int32_t wall_swap_team(Game& g, Build& b, int32_t who) {
	const int32_t t = current_upgrade(g, who, b.type);
	const int32_t n = init_build_site(g, who, t, b.x, b.y, true, -1);
	if (n < 0) return n;
	Build& nb = *g.build(who, n);
	nb.build_masks = b.build_masks;
	nb.ever_seen = b.ever_seen;
	nb.ever_seen_completed = b.ever_seen_completed;
	if ((b.flags & objflag::kActive) && type_is(*b.ptype, ptype::kLibrary)) build_new_library(g, nb);
	nb.flags = uint8_t((nb.flags & objflag::kDetector) | (b.flags & ~objflag::kDetector));
	nb.job_counter = b.job_counter;
	nb.job_counter_2 = b.job_counter_2;
	nb.constr_time = b.constr_time;
	nb.construct_hits = b.construct_hits;
	nb.damage = b.damage;
	nb.infiltrated = b.infiltrated;
	nb.visible = b.visible;
	wall_update_hits(g, nb, false);
	build_update_los(g, nb);
	return n;
}
}  // namespace

int32_t build_swap_team(Game& g, Build& b, int32_t who) {
	// @628210. A silo's missiles die; the garrison comes out (EJECTING: the old building lets one chain out per frame,
	// each Unit::come_out drawing game_random @61a1aa for AI units).
	if (type_is(*b.ptype, ptype::kMissileSilo)) kill_contents(g, b);
	eject_contents(g, b, 1, -1, 0, 1);
	if (!type_is(*b.ptype, ptype::kLibrary)) build_clean_queue(g, b, true);
	const int32_t n = wall_swap_team(g, b, who);
	if (n >= 0) {
		Build& nb = *g.build(who, n);
		nb.infiltrate = b.infiltrate;
		nb.recharging = b.recharging;
		nb.founder = b.founder;
		if (nb.max_age < b.max_age) nb.max_age = b.max_age;
		if (is_gather_type(*b.ptype) && !is_flat_gather(*b.ptype)) {
			// The claimed gather tiles move to the copy (appended after the ones its own Build::init found).
			for (const auto& tl : b.gather_from) nb.gather_from.push_back(tl);
			nb.mtn = b.mtn;
			nb.cliff = b.cliff;
			b.mtn = b.cliff = -1;
			b.gather_from.clear();
		}
		if (b.type == ptype::kFarm && b.fort >= 0 && size_t(b.fort) < g.farms.size()) {
			nb.fort = b.fort;
			g.farms[size_t(b.fort)].who = who;
			g.farms[size_t(b.fort)].o = n;
			b.fort = -1;
		}
		build_update_gpiece(g, nb);  // Wall::update_gpiece (the founder and the older max_age carried over)
	}
	if (type_is(*b.ptype, ptype::kSpaceProgram)) LD(g, who).flags |= leaderflag::kRevealEnemy;
	return n;
}

int32_t unit_swap_team(Game& g, Unit& u, int32_t who) {
	// @6118d0: a follower delegates to the unit above it (o_up) until the captain does the work.
	if (u.o_up >= 0) {
		Unit* up = g.unit(u.who, u.o_up);
		return up ? unit_swap_team(g, *up, who) : -1;
	}
	const int32_t t = current_upgrade(g, who, u.type);
	const int32_t n = init_unit(g, who, t, u.x, u.y, -1, -1, -1);
	if (n >= 0) {
		// Member by member along o_down (+0x90) of both chains: damage, damage_frac, angle (+0x50), flags (the copy's
		// DETECTOR kept), unit_masks (the copy's AI_CONTROL kept), form (+0xaa), recharging (+0xae), the position,
		// guy_mark and each guy's state.
		int32_t oo = u.o, no = n;
		for (int guard = 0; oo >= 0 && no >= 0 && guard < kUnitLimit; guard++) {
			Unit* src = g.unit(u.who, oo);
			Unit* dst = g.unit(who, no);
			if (!src || !dst) break;
			dst->damage = src->damage;
			dst->damage_frac = src->damage_frac;
			dst->angle = src->angle;
			dst->flags = uint8_t((dst->flags & objflag::kDetector) | (src->flags & ~objflag::kDetector));
			dst->unit_masks = (dst->unit_masks & umask::kAiControl) | (src->unit_masks & ~umask::kAiControl);
			dst->form = src->form;
			dst->recharging = src->recharging;
			if (!(dst->flags & objflag::kValid)) remove_from_world(g, *dst);
			else unit_set_new_location(g, *dst, src->x, src->y, true, true);
			dst->guy_mark = src->guy_mark;
			for (int32_t i = 0; i < dst->guy_mark && size_t(i) < dst->guys.size() && size_t(i) < src->guys.size(); i++) {
				// 🟡 GuyData +0xc..+0x40 and the bytes +0x9d / +0xa2: position, angles and turret angles here.
				Guy& d = dst->guys[size_t(i)];
				const Guy& s = src->guys[size_t(i)];
				d.x = s.x, d.y = s.y, d.z = s.z, d.angle = s.angle, d.last_angle = s.last_angle;
				d.turret_angles = s.turret_angles;
				d.des_turret_angles = s.des_turret_angles;
				d.track_dx = s.track_dx, d.track_dy = s.track_dy;
				guy_update_gpiece(g, *dst, d);
			}
			oo = src->o_down;
			no = dst->o_down;
		}
		// TODO(transports): carried units (inside_down chain) are recreated and put inside the copy (Unit::go_inside).
	}
	// The old chain closes member by member (dt 0: no death animation, no draw): Unit::close re-appends a closed
	// captain behind its followers, so the walk follows get_captain until the last member is closed.
	Unit* m = &u;
	for (int guard = 0; m && guard < kUnitLimit; guard++) {
		unit_close(g, *m, 0, -1, 0.0f);
		if (m->o_up < 0) break;
		m = g.unit(m->who, obj_captain(g, *m));
	}
	return n;
}

// ---------------------------------------------------------------- Build::check_capture

int32_t check_capture(Game& g, Object& t, int32_t o, int32_t who) {
	// @6276a0
	if (t.is_unit || !check_capture_eligible(g, t)) return 0;
	Build& b = static_cast<Build&>(t);
	const int32_t owner = b.who;
	if (b.inside_down >= 0) {  // a garrison must be ejected first
		b.build_masks |= bmask::kEjecting;
		return 0;
	}
	if (!leader_ok(who) || !leader_ok(owner)) return 0;
	if (mutual_ally_ai(g, owner, who) && (LD(g, owner).flags & leaderflag::kActive)) return 0;
	if (!(LD(g, who).flags & leaderflag::kActive)) return 0;
	Object* cu = g.object(who, o);
	if (!cu || cu->ptype->domain != kDomainGround || !cu->is_unit) return 0;
	Unit& U = static_cast<Unit&>(*cu);
	if (U.unit_masks & umask::kDecoy) return 0;
	if (obj_attack(g, U) == 0) return 0;
	if (uint32_t(U.ptype->obj_masks) & objmask::kMissile) return 0;
	if (b.city < 0 || size_t(b.city) >= g.cities[size_t(owner)].size()) return 0;  // (a non-city: never reached)
	City& C = g.cities[size_t(owner)][size_t(b.city)];
	const int32_t since = g.frame - C.capture_stamp;
	if (since <= 0x4a) return 0;
	const int32_t R = (b.flags & objflag::kCity) ? K("city_capture_radius") : K("unit_respond_range");
	const int32_t reach = R * 0xc0;
	const int32_t rw = (R * 0xc0 + 0x2ff) / 0x300;
	const int32_t bwx = wcoord_of(b.x), bwy = wcoord_of(b.y);
	const int16_t reg = g.world.in_w(bwx, bwy) ? g.world.w(bwx, bwy).region : int16_t(-1);
	int32_t def = since < 900 ? C.capture_strength : 2;
	const int32_t av = U.ptype->domain == kDomainGround ? capture_value(U) : 0;
	int32_t att = av;
	std::array<int32_t, 8> sums{};
	sums[size_t(who)] = av;
	sums[size_t(owner)] = def;
	const Circles& CI = circles();
	for (int32_t k = 0; k < CI.radius[size_t(rw)]; k++) {
		const int32_t wx = bwx + CI.x[size_t(k)], wy = bwy + CI.y[size_t(k)];
		if (!g.world.in_w(wx, wy)) continue;
		const WData& wd = g.world.w(wx, wy);
		if (wd.down < 0 || wd.region != reg) continue;
		int32_t lo = wd.down, lw = wd.down_who;
		while (lo >= 0) {
			Object* ob = g.object(lw, lo);
			if (!ob) break;
			const int32_t co = lo, cw = lw;
			lo = ob->down;
			lw = ob->down_who;
			if (co == b.o && cw == owner) continue;
			if (co == o && cw == who) continue;
			if (!(ob->flags & objflag::kValid)) continue;
			if (ob->ptype->domain == kDomainAir || ob->ptype->domain == kDomainSea) continue;
			if (ob->is_unit && (static_cast<Unit*>(ob)->unit_masks & umask::kDecoy)) continue;
			if (!search_valid_filter(g, cw, co, -1, -1, filter::kNotUnderAttack)) continue;
			if (vector_dist(std::abs(ob->x - b.x), std::abs(ob->y - b.y)) > reach) continue;
			int32_t v = capture_value(*ob);
			if (v != 0 && !ob->is_unit && cw == owner) {
				// The defender's own buildings: garrison (num_inside(1)) + 6, forts + 12; the attacker now sees them.
				v = num_inside(g, *ob, 1) + (type_is(*ob->ptype, ptype::kFortX) ? 12 : 6) + v;
				if (!(ob->visible & (1 << who))) {
					ob->visible = int8_t(ob->visible | (1 << who));
					object_update_local_seen(g, *ob);
				}
			}
			if (cw >= 0 && cw < 8) sums[size_t(cw)] += v;
			if (cw == who || (is_ally(g, who, cw) && is_enemy(g, owner, cw))) att += v;
			else if (cw == owner || (is_ally(g, owner, cw) && is_enemy(g, who, cw))) def += v;
		}
	}
	if (!(def < att)) return 0;
	if (b.flags & objflag::kCity) {
		// The new owner: the attacking-side leader with the largest sum (strict '>', so ties keep the lower index),
		// then the founder or, failing that, the race leader if allied to it and active.
		int32_t best = who, bestv = 0;
		for (int32_t i = 0; i < 8; i++) {
			if (!valid_active(LD(g, i))) continue;
			if (i != who && !is_ally(g, i, who)) continue;
			if (bestv < sums[size_t(i)]) {
				bestv = sums[size_t(i)];
				best = i;
			}
		}
		const int32_t F = C.founder, Rc = C.race;
		auto eligible = [&](int32_t p) {
			return p >= 0 && p < 8 && p != owner && p != best && is_ally(g, best, p) && (LD(g, p).flags & leaderflag::kActive);
		};
		int32_t neww = best;
		if (eligible(F)) neww = F;
		else if (eligible(Rc)) neww = Rc;
		const int32_t idx = cities_capture_city(g, neww, b.city, owner);
		if (idx >= 0 && size_t(idx) < g.cities[size_t(neww)].size()) g.cities[size_t(neww)][size_t(idx)].capture_strength = av;
		return 1;
	}
	// A capturable building that is not a city (no stock type: BuildTypeData::is_city is is(414)).
	const int32_t n = build_swap_team(g, b, who);
	if (n >= 0) {
		Build& nb = *g.build(who, n);
		build_activate_full(g, nb, false, true, false);
		build_close(g, b, 0);
		wall_mask_me(g, nb, true, Regen::Total);
		GameEvent e;
		e.frame = g.frame, e.kind = 7, e.who = who, e.other = owner, e.o = b.o, e.o2 = n;
		log_event(g, e);
		return 1;
	}
	object_die(g, b, 0, -1, 0.0f);
	return 0;
}

// ---------------------------------------------------------------- City::capture

void city_capture(Game& g, int32_t new_who, int32_t new_idx, int32_t old_who, int32_t old_idx, int32_t new_o) {
	// @736c40
	City& N = g.cities[size_t(new_who)][size_t(new_idx)];
	const City O = g.cities[size_t(old_who)][size_t(old_idx)];  // (a copy: the old list is not touched below)
	N.city = int16_t(new_idx);
	N.who = int8_t(new_who);
	N.o = int16_t(new_o);
	const Build* nb = g.build(new_who, new_o);
	N.x = nb->x;
	N.y = nb->y;
	if (g.scripted_rules || g.ctw) {  // ScenarioData::city_lost / city_lost_to (city_captured)
		g.scen.city_lost = uint8_t(g.scen.city_lost | (1u << (old_who & 7)));
		g.scen.city_lost_to[size_t(old_who & 7)] = int16_t(new_who);
	}
	Leader& NL = LD(g, new_who);
	const int32_t pv = city_get_pop_value(nb->type);
	NL.pop -= pv;
	g.world_pop -= pv;
	N.pop = O.pop;
	NL.pop += pv;
	g.world_pop += pv;
	N.attack_stamp = N.raid_stamp = g.frame;
	N.reg = O.reg;
	N.scouted = O.scouted;
	N.founder = O.founder;
	const bool friendly = mutual_ally_ai(g, old_who, new_who);
	if (friendly || (NL.flags & leaderflag::kAssimilateImmed) || has_preq(g, new_who, ptype::kGlobalGovernmentBonus))
		N.race = int8_t(new_who);
	else
		N.race = O.race;
	N.plundered = O.plundered;
	N.was_capital_flags = O.was_capital_flags;
	N.traded_with = O.traded_with;
	N.conquest_node = O.conquest_node;
	N.city_flags = uint16_t(O.city_flags & 0xf13f);
	if (!friendly) N.city_flags |= cityflag::kGotCaptured;
	N.name = O.name;
	if (N.city_flags & cityflag::kCapital) {
		N.was_capital_flags = uint8_t(N.was_capital_flags | (1u << (old_who & 0x1f)));
		if (N.city_flags & cityflag::kOriginalCapital) N.city_flags |= cityflag::kWasOrigCapital;
		if (!tb(g, old_who, 0x17) || !(N.city_flags & cityflag::kOriginalCapital)) {
			// Every unfinished Senate of the old owner is disbanded with a full refund (Object::disband(1)).
			for (int32_t o = kBuildBase; o < g.objects[size_t(old_who)].build_mark; o++) {
				Build* s = g.build(old_who, o);
				if (!s || !s->alive() || (s->flags & objflag::kActive) || !type_is(*s->ptype, ptype::kSenate)) continue;
				build_disband(g, *s, true);
			}
		}
	}
	N.city_flags &= 0xbfef;  // CAPITAL and ORIGINAL_CAPITAL cleared
	const uint16_t f = N.city_flags;
	if (N.was_capital_flags & (1u << (new_who & 0x1f))) {
		// Recapturing one's own capital.
		N.city_flags = uint16_t(f | cityflag::kCapital);
		N.was_capital_flags = uint8_t(N.was_capital_flags & ~(1u << (new_who & 0x1f)));
		if ((N.city_flags & cityflag::kWasOrigCapital) && N.who == N.founder) N.city_flags = uint16_t((f & 0x3fff) | 0x4010);
		N.race = N.who;
	}
	Build* cb = g.build(new_who, new_o);
	cb->damage = obj_hits(g, *cb) - 10;
}

// ---------------------------------------------------------------- Cities::capture_city

int32_t cities_capture_city(Game& g, int32_t new_who, int32_t idx, int32_t old_who) {
	// @733380
	Leader& OL = LD(g, old_who);
	Leader& NL = LD(g, new_who);
	GameEvent ev;
	ev.frame = g.frame, ev.kind = 1, ev.who = new_who, ev.other = old_who;
	if (OL.dip[size_t(new_who)].agree == 1) leader_action_clear_all(g, old_who, new_who);
	const uint16_t old_flags = g.cities[size_t(old_who)][size_t(idx)].city_flags;
	const bool was_capital = (old_flags & cityflag::kCapital) != 0;
	bool persian_other_cap = false;
	if (tb(g, old_who, 0x17) && was_capital) {
		int32_t c = -1, w = -1;
		find_capital(g, old_who, &c, &w, idx, old_who);
		if (c >= 0 && w >= 0) persian_other_cap = w == old_who;
	}
	OL.cities_lost++;
	NL.cities_captured++;
	// (Steam statistic for the console player; Achieve events: presentation)
	NL.flags |= leaderflag::kRecompute;
	OL.flags |= leaderflag::kRecompute;
	const int32_t old_o = g.cities[size_t(old_who)][size_t(idx)].o;
	Build* ocb = g.build(old_who, old_o);
	const int32_t rr = std::min(get_radius(g, old_who, ocb->type), 64);
	const int32_t rw = (rr + 3) / 4;
	ev.o = old_o;
	std::vector<int32_t> list_a{old_o}, list_b;  // A: old objects to dispose of, B: new copies to re-mask
	const int32_t n = build_swap_team(g, *ocb, new_who);
	int32_t new_idx = -1, plunder = 0;
	if (n >= 0) {
		Build& ncb = *g.build(new_who, n);
		build_activate_full(g, ncb, true, true, false);  // activate(1, 1, 0): City::init stamps every timer with frame
		list_b.push_back(n);
		new_idx = ncb.city;
		if (new_idx >= 0) plunder = K("city_plunder_per_level") * (city_get_level(ncb.type) - 1);
	}
	ev.o2 = n;
	ev.value = new_idx;
	if (new_idx == -1) {
		for (int32_t o = ocb->city_down; o >= 0;) {
			Build* b = g.build(old_who, o);
			if (!b) break;
			if (!(b->ptype->build_flags & 0x10)) {
				list_a.push_back(o);
				plunder += 25;
			}
			o = b->city_down;
		}
	} else {
		city_capture(g, new_who, new_idx, old_who, idx, n);
		armies_update_city(g, old_o, old_who, n, new_who);
		for (int32_t o = ocb->city_down; o >= 0;) {
			Build* b = g.build(old_who, o);
			if (!b) break;
			const int32_t next = b->city_down;
			if (!(b->ptype->build_flags & 0x2000)) {  // 'n': never captured
				list_a.push_back(o);
				const bool lakota_skip = (type_is(*b->ptype, ptype::kFarm) || type_is(*b->ptype, ptype::kGranary)) && tb(g, new_who, 0x13);
				if (!lakota_skip && (b->flags & objflag::kActive)) {
					const int32_t m = build_swap_team(g, *b, new_who);
					if (m >= 0) {
						build_activate_full(g, *g.build(new_who, m), true, true, false);
						list_b.push_back(m);
						plunder += 25;
					}
				}
			}
			o = next;
		}
		city_find_buildings(g, new_who, new_idx);
		if (mutual_ally_ai(g, new_who, old_who)) {
			// Old owner's units around the city change sides (same leader or mutual allies only; with more than one
			// old city, only those inside the radius whose nearest friendly city is this one).
			const City& NC = g.cities[size_t(new_who)][size_t(new_idx)];
			const int32_t cwx = wcoord_of(NC.x), cwy = wcoord_of(NC.y);
			const Circles& CI = circles();
			for (int32_t k = 0; k < CI.radius[size_t(rw)]; k++) {
				const int32_t wx = cwx + CI.x[size_t(k)], wy = cwy + CI.y[size_t(k)];
				if (!g.world.in_w(wx, wy)) continue;
				int32_t lo = g.world.w(wx, wy).down, lw = g.world.w(wx, wy).down_who;
				while (lo >= 0) {
					Object* ob = g.object(lw, lo);
					if (!ob) break;
					const int32_t co = lo, cw = lw;
					lo = ob->down;
					lw = ob->down_who;
					if (cw != old_who || !ob->is_unit || !ob->alive()) continue;
					Unit& u = static_cast<Unit&>(*ob);
					if (!u.on_map()) continue;
					if (OL.city_num > 1) {
						const City& C2 = g.cities[size_t(new_who)][size_t(new_idx)];
						const Build* c2b = g.build(new_who, C2.o);
						const int32_t r2 = std::min(get_radius(g, new_who, c2b->type), 64);
						if (vector_dist(std::abs(u.x - C2.x), std::abs(u.y - C2.y)) > r2 * 0xc0) continue;
						if (objects_find_city(g, u.x, u.y, search::kFriendly, new_who, 0, filter::kAll, 0, 0) != new_idx) continue;
					}
					(void)co;
					if (unit_swap_team(g, u, new_who) >= 0) ev.n_units++;
				}
			}
		}
	}
	// ---- plunder: THE DESPOT / Spitamenes in range of the new city building (HeroesData::find_hero with the building's
	// extra radius, ObjectData::has_general; the owns(type) gate is inside has_general)
	bool despot = false;
	if (new_idx >= 0) {
		const Build* ncb = g.build(new_who, g.cities[size_t(new_who)][size_t(new_idx)].o);
		if (ncb && has_general(g, *ncb, 0, hero::kTheDespot) >= 0) despot = true;
		if (ncb && has_general(g, *ncb, 0, hero::kSpitamenes) >= 0) despot = true;
	}
	int32_t russian = 0;  // the victim keeps the plunder (Russians; after Rise of Legends only for non-capitals)
	if (tb(g, old_who, 0xd) && K("russian_plunder_steal") != 0) russian = was_capital ? 0 : 1;
	const City& OC = g.cities[size_t(old_who)][size_t(idx)];
	const bool may_plunder = !(plunder == 0 && !was_capital);
	const bool recent = (OC.city_flags & cityflag::kGotCaptured) || (OC.capture_stamp != 0 && g.frame - OC.capture_stamp < 0x1195) ||
	                    new_who < 0;
	if (may_plunder && !recent) {
		int32_t to_new = 0, to_old = 0;
		if ((OL.flags & leaderflag::kCapitalPlundered) || !was_capital || persian_other_cap || g.info.team_style == 3) {
			if (!russian) to_new = despot ? (K("thedespot_plunder") * plunder) / 100 : plunder;
			else to_old = plunder, to_new = despot ? plunder : 0;
			if (to_new) {
				const int32_t k = poorest_good(g, new_who, new_who, old_who);
				NL.bucket[size_t(k)] += to_new;
				ev.goods[size_t(k)] += to_new;
			}
			if (to_old) {
				const int32_t k = poorest_good(g, old_who, new_who, old_who);
				OL.bucket[size_t(k)] += to_old;
				ev.goods2[size_t(k)] += to_old;
			}
		} else {
			// The first capture of this leader's capital: a lump sum of every good (bucket_add for the captor).
			OL.flags |= leaderflag::kCapitalPlundered;
			const uint8_t ts = g.info.team_style;
			if (ts == 2 || ts == 1 || ts == 9) {
				int32_t remaining = g.info.num_nations;
				for (int32_t i = 0; i < 8; i++)
					if ((LD(g, i).flags & 3u) == 1u) remaining--;
				int32_t gone = g.info.num_nations - remaining;
				if (gone < 1) gone = 1;
				plunder = K("capital_plunder_assassin") * gone;
			} else if (plunder <= K("capital_plunder")) {
				plunder = K("capital_plunder");
			}
			if (!russian) to_new = despot ? (K("thedespot_plunder") * plunder) / 100 : plunder;
			else to_old = plunder, to_new = despot ? plunder : 0;
			for (int32_t k = 0; to_new && k < 6; k++) {
				if (!type_avail(g, new_who, k, 1) || !type_avail(g, old_who, k, 1) || k == ptype::kKnowledge) continue;
				bucket_add(g, new_who, k, to_new);
				ev.goods[size_t(k)] += to_new;
			}
			for (int32_t k = 0; to_old && k < 6; k++) {
				if (!type_avail(g, new_who, k, 1) || !type_avail(g, old_who, k, 1) || k == ptype::kKnowledge) continue;
				OL.bucket[size_t(k)] += to_old;
				ev.goods2[size_t(k)] += to_old;
			}
		}
	}
	// (messages and sounds: presentation)
	// ---- list A: unfinished buildings are disbanded (partial refund), finished ones close without a ruin (dt 5); the
	// Lakota captor's farms and granaries close as destroyed (dt 0). The city object itself closes below.
	for (const int32_t o : list_a) {
		Build* b = g.build(old_who, o);
		if (!b || !b->alive() || (b->flags & objflag::kCity)) continue;  // (every entry is still alive here)
		if (!(b->flags & objflag::kActive)) {
			build_disband(g, *b, false);
			ev.n_disbanded++;
		} else {
			const bool lakota = (type_is(*b->ptype, ptype::kFarm) || type_is(*b->ptype, ptype::kGranary)) && tb(g, new_who, 0x13);
			build_close(g, *b, lakota ? 0 : 5);
			ev.n_closed++;
		}
	}
	for (const int32_t o : list_b) {
		if (Build* b = g.build(new_who, o)) wall_mask_me(g, *b, true, Regen::Simple);
	}
	ev.n_swapped = int32_t(list_b.size());
	// ---- capitals
	if (was_capital) {
		const int32_t fc = has_wonder_bits(g, old_who, ptype::kForbiddenCity);
		if (fc == 0 || fc == 2) {
			int32_t c = -1, w = -1;
			find_capital(g, old_who, &c, &w, idx, old_who);
			if (c < 0 || w != old_who) leader_lost_capital(g, old_who, new_who);
		}
	}
	if (new_idx >= 0 && (g.cities[size_t(new_who)][size_t(new_idx)].city_flags & cityflag::kCapital)) {
		int32_t c = -1, w = -1;
		find_capital(g, new_who, &c, &w, new_idx, new_who);
		if (c < 0 || w != new_who) leader_recapture_capital(g, new_who);
	}
	// ---- the old record and the old city object
	const int32_t oo = g.cities[size_t(old_who)][size_t(idx)].o;
	if (old_who >= 0) {
		city_close(g, old_who, idx, new_who);
		while (OL.city_mark > 0 && !(g.cities[size_t(old_who)][size_t(OL.city_mark - 1)].city_flags & cityflag::kValid))
			OL.city_mark--;
	}
	if (Build* ob = g.build(old_who, oo)) {
		ob->city = -1;
		build_close(g, *ob, 5);
	}
	if (new_idx >= 0) {
		Build* ncb = g.build(new_who, g.cities[size_t(new_who)][size_t(new_idx)].o);
		if (ncb) {
			wall_update_hits(g, *ncb, false);
			ncb->damage = obj_hits(g, *ncb) - 10;
			build_update_los(g, *ncb);
		}
	}
	calc_pop_cap(g, new_who);
	log_event(g, ev);
	return new_idx;
}

// ---------------------------------------------------------------- capital loss

void leader_lost_capital(Game& g, int32_t who, int32_t who_new) {
	// @6dc870
	Leader& L = LD(g, who);
	if (L.lost_capital_timer == 0) {
		L.lost_capital_timer = 1;
		const int32_t T = get_team_terr(g, who), E = leader_ok(who_new) ? get_team_terr(g, who_new) : 0;
		const int32_t a = g.info.num_nations == 0 ? 0 : g.world_total_land / g.info.num_nations;
		if (T + a == 0 || E + a == 0) {
			L.lost_capital_modifier = 0x100;
		} else {
			L.lost_capital_modifier = std::clamp(((T + a) * 0x100) / (E + a), 0x80, 0x200);
		}
		const int32_t mine = L.city_num, theirs = leader_ok(who_new) ? LD(g, who_new).city_num : 0;
		if (mine != 0 && theirs != 0 && theirs < mine)
			L.lost_capital_modifier = std::clamp((L.lost_capital_modifier * (mine + 2)) / (theirs + 2), 0x80, 0x300);
		L.lost_capital_stamp = int32_t(uint32_t(L.lost_capital_stamp) << 8) / L.lost_capital_modifier + g.frame;
	}
	GameEvent e;
	e.frame = g.frame, e.kind = 5, e.who = who, e.other = who_new;
	log_event(g, e);
}

void leader_recapture_capital(Game& g, int32_t who) {
	// @6d1ba0
	Leader& L = LD(g, who);
	if (L.lost_capital_timer != 0) {
		L.lost_capital_timer = 0;
		const int32_t v = (L.lost_capital_stamp - g.frame) * L.lost_capital_modifier;
		L.lost_capital_stamp = v / 256;  // (v + (v >> 31 & 0xff)) >> 8
		GameEvent e;
		e.frame = g.frame, e.kind = 6, e.who = who;
		log_event(g, e);
	}
}

int32_t game_retake_capital(const Game& g, int32_t who) {
	// @594530 (CTW adjustments for the Forbidden City: TODO(ctw))
	int32_t v = map_scaled(g, K("retake_capital"));
	if (who >= 0) v = (LD(g, who).lost_capital_modifier * v) / 256;
	return v;
}

// ---------------------------------------------------------------- Leader::lost_a_city

void leader_lost_a_city(Game& g, int32_t who, bool was_capital, bool own, int32_t attacker) {
	// @6dc6d0 (asm). Balance mode (semaphore byte1 & 2) and a paused editor return at once.
	if (g.editor) return;  // TODO(editor): only while TurnControl is paused
	Leader& L = LD(g, who);
	if (was_capital && attacker >= 0 && g.info.elimination == 1 && has_wonder_bits(g, who, ptype::kForbiddenCity) == 0) {
		for (int32_t w = 0; w < 8; w++) {
			if (!valid_active(LD(g, w))) continue;
			if (w != attacker && !is_ally(g, attacker, w)) continue;
			if (has_preq(g, w, ptype::kGlobalGovernmentBonus)) {
				leader_defeat(g, who, 1, attacker, 1);
				return;
			}
		}
	}
	switch (g.info.elimination) {
	case 2:
		if (was_capital) {
			if (tb(g, who, 0x17)) {
				int32_t c = -1, w = -1;
				find_capital(g, who, &c, &w, -1, -1);
				if (c >= 0 && w == who) return;
			}
			leader_defeat(g, who, 2, attacker, 0);
			return;
		}
		break;
	case 3:
		if (own) leader_defeat(g, who, 3, attacker, 0);
		return;
	default: break;
	}
	if (!(L.flags2 & leaderflag2::kNoCityDefeat) && L.city_num == 0) leader_defeat(g, who, 0, attacker, 0);
}

// ---------------------------------------------------------------- defeat

namespace {
// Leader::blow_up_towers(attacker) @6d2310 (multiplayer): armed buildings die, and military trainers standing on
// territory of a third leader not allied to the conqueror.
void blow_up_towers(Game& g, int32_t who, int32_t attacker) {
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		Build* b = g.build(who, o);
		if (!b || !b->alive() || (b->flags & objflag::kCity)) continue;
		bool die = b->ptype->attack != 0;
		if (!die && !is_wonder_type(b->type) && is_military_trainer(g, b->type)) {
			const int32_t wx = wcoord_of(b->x), wy = wcoord_of(b->y);
			const int32_t tw = g.world.in_w(wx, wy) ? g.world.w(wx, wy).who : -1;
			die = tw >= 0 && tw != who && tw != attacker && (attacker < 0 || !is_ally(g, attacker, tw));
		}
		if (die) object_die(g, *b, 3, -1, 0.0f);
	}
}
// Leader::blow_up_units @6d2170 (multiplayer): every unit dies (dt 3: death animations draw game_random); garrisoned
// units come out first (Unit::come_out(0), which draws game_random @61a1aa).
void blow_up_units(Game& g, int32_t who) {
	for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
		Unit* u = g.unit(who, o);
		if (!u || !u->alive()) continue;
		if (!u->on_map()) unit_come_out(g, *u, 0);
		if (u->alive()) object_die(g, *u, 3, -1, 0.0f);
	}
}
void blow_up_buildings(Game& g, int32_t who) {  // @6d22b0
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		Build* b = g.build(who, o);
		if (b && b->alive()) object_die(g, *b, 3, -1, 0.0f);
	}
}
}  // namespace

void leader_defeat_by(Game& g, int32_t who, int32_t attacker, int32_t type) {
	// @6d1c80
	Leader& L = LD(g, who);
	if (L.defeated_by >= 0) return;
	if (type != 5) {
		int32_t humans = 0, live_humans = 0;
		for (int32_t i = 0; i < 8; i++) {
			const Leader& I = LD(g, i);
			if (!(I.flags & leaderflag::kValid) || !(I.flags & leaderflag::kHuman)) continue;  // any_humans
			humans++;
			if (i != who && (I.flags & 99u) == 3u) live_humans++;
		}
		bool contest = false;
		for (int32_t i = 0, first = -1; i < 8; i++) {
			const Leader& I = LD(g, i);
			if (i == who || !valid_active(I)) continue;
			if (I.flags & leaderflag::kVictory) break;
			if (I.flags & leaderflag::kDefeated) continue;
			if (first < 0) {
				first = i;
			} else if (!is_ally(g, i, first)) {
				contest = true;
				break;
			}
		}
		if (g.multiplayer && contest && (live_humans != 0 || humans == 0)) {
			if (g.info.victory == 5) {
				blow_up_buildings(g, who);
				blow_up_units(g, who);
			} else {
				blow_up_towers(g, who, attacker);
				blow_up_units(g, who);
			}
		}
	}
	L.defeat_stamp = g.frame;
	if (attacker < 0) return;
	L.defeated_by = attacker;
	if (g.info.elimination == 3) return;
	// The remaining cities go to the conqueror (or to their founder / race leader when allied to the conqueror while
	// the conqueror is active); a capture that leaves the city undamaged assimilates it.
	for (int32_t i = 0; i < L.city_mark && size_t(i) < g.cities[size_t(who)].size(); i++) {
		const City& C = g.cities[size_t(who)][size_t(i)];
		if (!C.valid()) continue;
		const int32_t F = C.founder, Rc = C.race;
		const bool att_active = (LD(g, attacker).flags & leaderflag::kActive) != 0;
		if (F >= 0 && F != who && F != attacker && is_ally(g, attacker, F) && att_active) {
			cities_capture_city(g, F, i, who);
			continue;
		}
		if (Rc >= 0 && Rc != who && Rc != attacker && is_ally(g, attacker, Rc) && att_active) {
			cities_capture_city(g, Rc, i, who);
			continue;
		}
		if (who != attacker) {
			const int32_t n = cities_capture_city(g, attacker, i, who);
			if (n >= 0) {
				const Build* nb = g.build(attacker, g.cities[size_t(attacker)][size_t(n)].o);
				if (nb && nb->damage == 0) city_assimilate(g, attacker, n);
			}
		}
	}
	// Buildings: unfinished ones die; forts, towers, lookouts and cities stay; military trainers outside the
	// conqueror's cities die; the rest change sides.
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		Build* b = g.build(who, o);
		if (!b || !b->alive()) continue;
		if (!(b->flags & objflag::kActive)) {
			object_die(g, *b, 3, -1, 0.0f);
			continue;
		}
		if (type_is(*b->ptype, ptype::kFortX) || type_is(*b->ptype, ptype::kTower) || type_is(*b->ptype, ptype::kLookout) ||
		    (b->flags & objflag::kCity))
			continue;
		if (is_military_trainer(g, b->type) && find_city_at(g, b->x, b->y, attacker, -1, 1) < 0) {
			object_die(g, *b, 3, -1, 0.0f);
			continue;
		}
		if (who == attacker) continue;
		const int32_t n = build_swap_team(g, *b, attacker);
		if (n < 0) {
			object_die(g, *b, 3, -1, 0.0f);
			continue;
		}
		Build& nb = *g.build(attacker, n);
		build_activate_full(g, nb, false, true, false);  // activate(0, 1, 0)
		build_close(g, *b, 0);
		wall_mask_me(g, nb, true, Regen::Simple);
	}
}

void leader_defeat(Game& g, int32_t who, int32_t type, int32_t attacker, int32_t flag) {
	// @6ecb00
	leader_defeat_by(g, who, attacker, type);
	Leader& L = LD(g, who);
	if (L.flags & leaderflag::kDefeated) return;
	if (flag == 0) L.flags2 &= ~leaderflag2::kDefeatFlag;
	else L.flags2 |= leaderflag2::kDefeatFlag;
	L.flags = (L.flags & ~leaderflag::kActive) | leaderflag::kDefeated;
	armies_leader_defeated(g, who);
	L.flags2 |= leaderflag2::kUnitAiOff;
	g.musical_chairs = g.frame;
	L.defeat_type = type;
	GameEvent e;
	e.frame = g.frame, e.kind = 2, e.who = who, e.other = attacker, e.value = type;
	log_event(g, e);
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		Build* b = g.build(who, o);
		if (b && b->alive()) build_clean_queue(g, *b, false);
	}
	// Units keep standing with no orders; planes die (dt 0: no death animation).
	for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
		Unit* u = g.unit(who, o);
		if (!u || !u->alive()) continue;
		if (!unit_is_air(*u)) clear_orders(g, *u);
		else object_die(g, *u, 0, -1, 0.0f);
		u->unit_masks &= ~umask::kAiControl;
	}
	// (Player::lose for the seats, notices: presentation)
	if (!g.game_over) {
		game_check_victory(g);
		if (g.game_over) L.flags |= leaderflag::kGameOver;
	}
	if (g.multiplayer && !g.game_over) {
		// The defeated leader's stockpile is shared among its live allies (scale_tribute of an equal share each).
		int32_t allies = 0;
		for (int32_t i = 0; i < 8; i++)
			if (valid_active(LD(g, i)) && is_ally(g, i, who)) allies++;
		if (allies != 0) {
			for (int32_t i = 0; i < 8; i++) {
				if (!valid_active(LD(g, i)) || !is_ally(g, i, who)) continue;
				for (int32_t k = 0; k < 6; k++) {
					if (!type_avail(g, i, k, 1) || !type_avail(g, who, k, 1) || k == ptype::kKnowledge) continue;
					bucket_add(g, i, k, scale_tribute(g, who, L.bucket[size_t(k)] / allies));
				}
			}
			L.bucket.fill(0);
		}
	}
}

// ---------------------------------------------------------------- victory

void leader_victory(Game& g, int32_t who, int32_t type, int32_t flag) {
	// @6ec9b0
	Leader& L = LD(g, who);
	if (L.flags & (leaderflag::kVictory | leaderflag::kDefeated)) return;
	L.flags |= leaderflag::kVictory;
	if (flag == 0) L.flags2 &= ~leaderflag2::kVictoryFlag;
	else L.flags2 |= leaderflag2::kVictoryFlag;
	L.victory_type = type;
	GameEvent e;
	e.frame = g.frame, e.kind = 3, e.who = who, e.value = type;
	log_event(g, e);
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		Build* b = g.build(who, o);
		if (b && b->alive()) build_clean_queue(g, *b, false);
	}
	// (Player::win: presentation)
	for (int32_t i = 0; i < 8; i++) {
		Leader& I = LD(g, i);
		if (i == who || !valid_active(I)) continue;
		I.flags |= leaderflag::kGameOver;
		if (!is_ally(g, i, who)) leader_defeat(g, i, 5, -1, 0);
		else leader_victory(g, i, type, flag);
	}
}

void game_check_victory(Game& g) {
	// @5926b0 (asm; balance mode is not modelled). The game ends when every human seat is gone (with at least one
	// human seat), or when all live leaders are mutual allies of the first one.
	if (g.game_over) return;
	int32_t humans = 0, live_humans = 0;
	for (int32_t i = 0; i < 8; i++) {
		const Leader& I = LD(g, i);
		if (!(I.flags & leaderflag::kValid) || !(I.flags & leaderflag::kHuman)) continue;
		humans++;
		if ((I.flags & 0x43u) == 3u) live_humans++;
	}
	if (live_humans != 0 || humans == 0) {
		for (int32_t i = 0, first = -1; i < 8; i++) {
			if ((LD(g, i).flags & 0x43u) != 3u) continue;
			if (first < 0) first = i;
			else if (first != i && !is_ally(g, first, i)) return;
		}
	}
	g.game_over = true;
	GameEvent e;
	e.frame = g.frame, e.kind = 4;
	log_event(g, e);
	int32_t lim = K("armageddon_per_team") * g.info.num_sides + K("armageddon_per_nation") * g.info.num_nations + K("armageddon");
	const uint8_t sr = g.info.starting_resources;
	if (sr == 7) lim *= 3;
	else if (sr == 5 || sr == 6 || sr == 11 || sr == 12) lim *= 2;
	if (sr == 8 && lim < 100) lim = 100;
	if (g.armageddon < lim) {
		int32_t winner = -1;
		for (int32_t i = 0; i < 8; i++) {
			Leader& I = LD(g, i);
			if (!valid_active(I)) continue;
			const uint32_t f = I.flags;
			I.flags |= leaderflag::kGameOver;
			if (f & leaderflag::kDefeated) continue;
			if (winner < 0) winner = i;
			else if (!(f & leaderflag::kVictory) && i != winner && !is_ally(g, i, winner)) {
				g.victory_checked = true;
				return;
			}
		}
		if (winner >= 0) leader_victory(g, winner, 0, 0);
	}
	g.victory_checked = true;
}

void game_defeat_all(Game& g) {
	// @592aa0: Armageddon. Every live, undefeated leader is defeated (type 8) and scored.
	for (int32_t i = 0; i < 8; i++) {
		Leader& I = LD(g, i);
		if (!(I.flags & leaderflag::kValid)) continue;
		if ((I.flags & 0x42u) == 2u) {
			I.flags |= leaderflag::kGameOver;
			leader_defeat(g, i, 8, -1, 0);
		}
		leader_compute_score(g, i, 1);
	}
}

void leader_process_elimination(Game& g, int32_t who) {
	// @6b8a20: CAPITAL elimination: the capital lost longer than the retake window ago defeats the leader in favour of
	// whoever holds the capital now.
	Leader& L = LD(g, who);
	if (g.info.elimination != 1 || L.lost_capital_timer == 0) return;
	const int32_t elapsed = g.frame - L.lost_capital_stamp;
	if (game_retake_capital(g, who) <= elapsed) {
		int32_t c = -1, w = -1;
		find_capital(g, who, &c, &w, -1, -1);
		leader_defeat(g, who, 1, w, 0);
	}
}

void leader_step_timers(Game& g, int32_t who) {
	// Leaders::process_all step 5: every timer_refresh_ratio frames each stopped clock recovers one frame.
	const int32_t r = K("timer_refresh_ratio");
	if (r == 0 || g.frame % r != 0) return;
	Leader& L = LD(g, who);
	if (L.lost_capital_stamp < 0 && L.lost_capital_timer == 0) L.lost_capital_stamp++;
	if (L.popwin_stamp < 0 && L.popwin_timer == 0) L.popwin_stamp++;
	if (L.wonderwin_stamp < 0 && L.wonderwin_timer == 0) L.wonderwin_stamp++;
}

void game_daemon_process_victory(Game& g) {
	// @730ef0 (messages and achievements omitted).
	if (get_armageddon(g) <= g.armageddon) game_defeat_all(g);
	const uint8_t vic = g.info.victory;
	const bool contest = g.multiplayer || g.info.num_nations > 1;
	auto gg_team = [&](int32_t i) {  // a VALID team mate (or i itself) holds GLOBAL_GOVERNMENT_BONUS
		for (int32_t k = 0; k < 8; k++) {
			if (!(LD(g, k).flags & leaderflag::kValid)) continue;
			if ((k == i || mutual_ally_ai(g, k, i)) && has_preq(g, k, ptype::kGlobalGovernmentBonus)) return true;
		}
		return false;
	};
	// ---- wonder victory (standard, 1, 6)
	if (vic == 0 || vic == 1 || vic == 6) {
		const int32_t ww = wonder_winning(g);
		bool handled = false;
		for (int32_t i = 0; i < 8; i++) {
			Leader& I = LD(g, i);
			if (!valid_active(I)) continue;
			if (ww < 0 || (i != ww && !mutual_ally_ai(g, i, ww))) {
				if (I.wonderwin_timer != 0) {
					I.wonderwin_timer = 0;
					I.wonderwin_stamp -= g.frame;
				}
				continue;
			}
			const bool gg = gg_team(i);
			if (vic == 0 && contest && !gg) {
				if (I.wonderwin_timer == 0) {
					I.wonderwin_timer = 1;
					I.wonderwin_stamp += g.frame;
				} else if (!handled) {
					const int32_t left = map_scaled(g, K("wonder_timer")) - (g.frame - I.wonderwin_stamp);
					if (left == 0) leader_victory(g, i, 1, 0);
				}
			} else if (!handled) {
				leader_victory(g, i, 1, gg ? 1 : 0);
			}
			handled = true;
		}
	}
	// ---- territory victory (7, standard, 1)
	if ((vic == 7 || vic == 0 || vic == 1) && contest) {
		const int32_t need = list_value(g, "popwins", g.info.popwin);
		const int32_t land = g.world_total_land;
		for (int32_t i = 0; i < 8; i++) {
			Leader& I = LD(g, i);
			if (!valid_active(I) || land == 0) continue;
			int32_t rivals = 0;
			for (int32_t k = 0; k < 8; k++) {
				if (!valid_active(LD(g, k)) || k == i || mutual_ally_ai(g, i, k)) continue;
				if (need <= (get_team_terr(g, k) * 100) / land) rivals++;
			}
			if ((get_team_terr(g, i) * 100) / land < need || rivals != 0) {
				if (I.popwin_timer != 0) {
					I.popwin_timer = 0;
					I.popwin_stamp -= g.frame;
				}
				continue;
			}
			const bool gg = gg_team(i);
			if (vic == 0 && !gg) {
				if (I.popwin_timer == 0) {
					I.popwin_timer = 1;
					I.popwin_stamp += g.frame;
				} else {
					const int32_t left = map_scaled(g, K("popwin_timer")) - (g.frame - I.popwin_stamp);
					if (left < 1) leader_victory(g, i, 2, 0);
				}
			} else {
				leader_victory(g, i, 2, gg ? 1 : 0);
			}
		}
	}
	// ---- score victory (3): the best (team) score reaching the goal (LeaderData::get_team_score: the team average
	// needs semaphore byte0 & 0x80, not modelled; the leader's own score).
	if (vic == 3) {
		int32_t best = 0, bv = 0;
		for (int32_t i = 0; i < 8; i++) {
			if (!valid_active(LD(g, i))) continue;
			if (bv < LD(g, i).score) bv = LD(g, i).score, best = i;
		}
		if (list_value(g, "scores", g.info.score_goal) <= bv) leader_victory(g, best, 4, 0);
	}
	// ---- time limit (4)
	if (vic == 4) {
		int32_t lim;
		if (g.info.time_limit < 8) lim = list_value(g, "time_limits", g.info.time_limit);
		// ScenarioData::custom_time_limit (set_time_limit; 0 -> 300 minutes) under scripted rules / CTW, else -1.
		// Port decision: the custom slot of a scenario file counts as scripted even without game_rules 10 / CTW (the
		// conquest maps carry it and are only played inside Conquer the World, which the port does not model; -1
		// would end them at once).
		else lim = g.scen.custom_time_limit != 0 ? g.scen.custom_time_limit : 300;
		if (g.tick >= lim * 0x3c) {
			if (g.info.game_rules == 8) {
				for (int32_t i = 0; i < 8; i++) {
					if (valid_active(LD(g, i)) && g.info.team[size_t(i)] == 0) {
						leader_victory(g, i, 6, 0);
						break;
					}
				}
			} else {
				int32_t best = 0, bv = 0;
				for (int32_t i = 0; i < 8; i++) {
					if (!valid_active(LD(g, i))) continue;
					if (bv < LD(g, i).score) bv = LD(g, i).score, best = i;
				}
				leader_victory(g, best, 6, 0);
			}
		}
	}
	// ---- musical chairs (5) and economic victory (8): TODO(victory): Game.on_team (Game::init_teams) and
	// LeaderData::get_economic are not ported.
}

// ---------------------------------------------------------------- Build::process step 4

bool build_capture_poll(Game& g, Build& b) {
	// Build::process @61edf0 (the city object's 64-frame poll).
	if ((g.frame + b.o) % 64 != 0) return false;
	const int32_t who = b.who;
	int32_t u = -1;
	if (check_capture_eligible(g, b)) {
		u = objects_find_unit(g, b.x, b.y, search::kEnemy, who, 0xc00, 0, filter::kDomain, 0, 0, filter::kAll, 0, 0);
		if (u >= 0 && g.find_who >= 0 && check_capture(g, b, u, g.find_who)) return true;
	}
	if (!(LD(g, who).flags & leaderflag::kHuman) && u < 0 && is_city_type(*b.ptype) && obj_hits(g, b) / 2 < b.damage &&
	    b.city >= 0 && !(g.cities[size_t(who)][size_t(b.city)].city_flags & cityflag::kUnderAttack) && get_diff(g, who) > 1) {
		const int32_t p = objects_find_unit(g, b.x, b.y, search::kFriendly, who, 0x1e00, 0x200, filter::kType,
		                                    ptype::kPeasants, 0, filter::kNotBusy, 0, 0);
		if (p >= 0) {
			if (Unit* pu = g.unit(g.find_who, p)) add_repair_order(g, *pu, b.o, who, kQueueNew, false);
		}
	}
	return false;
}

}  // namespace ron::sim
