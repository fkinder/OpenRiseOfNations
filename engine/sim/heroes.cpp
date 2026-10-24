// Heroes (docs/heroes-spec.md): hero records, find_hero / has_general, the general powers' expiry, cloaking and cover,
// government heroes and plunder.
//
// Evidence (local, gitignored): re/decomp/heroes/*.c (Heroes::init_hero, close_hero, HeroesData::find_hero,
// HeroData::get_radius, Caster::process_spells, ObjectData::has_general, Unit::plunder, Build::plunder),
// re/decomp/heroes2/*.c (Unit::init, Unit::close, Unit::process, Unit::process_cloak, Build::activate, Build::finished,
// City::assimilate, Leader::verify_spell_flags) and the disassembly of Unit::process @610bc0 (the mana regeneration).
#include "sim/heroes.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "sim/combat_internal.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/production.hpp"
#include "sim/transport.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

namespace {

bool valid_leader(int32_t who) { return who >= 0 && who < 8; }

std::vector<Caster>& list_of(Leader& l, bool specials) { return specials ? l.specials : l.heroes; }

// Heroes::init_hero / Specials::init_special (same algorithm, separate list and high mark).
int32_t init_record(Game& g, int32_t who, int32_t o, bool specials) {
	Leader& l = g.leaders[size_t(who)];
	std::vector<Caster>& v = list_of(l, specials);
	int32_t& n = specials ? l.num_specials : l.num_heroes;
	int32_t slot = -1;
	for (int32_t i = 0; i < n && size_t(i) < v.size(); i++) {
		if (!(v[size_t(i)].hero_flags & 1)) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		if (size_t(n) < v.size()) {
			slot = n;
			n++;
		} else {
			v.emplace_back();  // grows by one
			slot = int32_t(v.size()) - 1;
		}
	}
	n = std::max(n, slot + 1);
	Caster& c = v[size_t(slot)];
	c.hero = int16_t(slot);
	c.o = int16_t(o);
	c.who = int8_t(who);
	c.active_spells.clear();
	c.hero_flags = 1;
	return slot;
}

// Heroes::close_hero / Specials::close_special.
void close_record(Game& g, int32_t who, int32_t slot, bool specials) {
	Leader& l = g.leaders[size_t(who)];
	std::vector<Caster>& v = list_of(l, specials);
	if (slot < 0 || size_t(slot) >= v.size()) return;
	Caster& c = v[size_t(slot)];
	if (c.o >= 0 && c.who >= 0) caster_process_spells(g, c, c.o, c.who, true);
	c.hero_flags = uint8_t(c.hero_flags & ~1u);
	c.who = -1;
	c.o = -1;
	int32_t& n = specials ? l.num_specials : l.num_heroes;
	while (n > 0 && !(v[size_t(n - 1)].hero_flags & 1)) n--;
}

}  // namespace

bool unit_is_hero(const Unit& u) { return u.ptype && (u.ptype->unit_flags2 & 0x20) != 0; }
bool unit_is_special(const Unit& u) { return u.ptype && (u.ptype->unit_flags2 & 0x10) != 0; }
bool unit_is_gov_hero(const Unit& u) { return u.ptype && (u.ptype->unit_flags & 0x4000000) != 0; }

void heroes_init(Game& g) {
	for (int32_t who = 0; who < 8; who++) {
		Leader& l = g.leaders[size_t(who)];
		l.heroes.assign(20, Caster{});
		l.specials.assign(20, Caster{});
		l.num_heroes = 0;
		l.num_specials = 0;
	}
}

int32_t heroes_init_hero(Game& g, int32_t who, int32_t o) {
	if (!valid_leader(who)) return -1;
	return init_record(g, who, o, false);
}
void heroes_close_hero(Game& g, int32_t who, int32_t slot) {
	if (valid_leader(who)) close_record(g, who, slot, false);
}
int32_t specials_init_special(Game& g, int32_t who, int32_t o) {
	if (!valid_leader(who)) return -1;
	return init_record(g, who, o, true);
}
void specials_close_special(Game& g, int32_t who, int32_t slot) {
	if (valid_leader(who)) close_record(g, who, slot, true);
}

// ---------------------------------------------------------------- radius and search (3)

int32_t hero_get_radius(const Game& g, const Caster& h) {
	const int32_t who = h.who;
	const Unit* U = g.unit(who, h.o);
	int32_t r = (RON_K(g, "general_radius") * (get_general_upgrade(g, who) + 3)) / 2;
	if (!U) return r;
	if (unit_is(g, *U, hero::kParmenio, false)) {
		r = RON_K(g, "parmenio_radius_adjust") * r;
		r = (r + ((r >> 31) & 0xff)) >> 8;
	}
	if (unit_is(g, *U, hero::kWellington, false)) r = (RON_K(g, "wellington_radius") * r) / 100;
	if (unit_is(g, *U, hero::kKutusov, false)) r = (RON_K(g, "kutosov_radius") * r) / 100;
	if (has_wonder(g, who, 0x211)) r += RON_K(g, "terra_cotta_range");
	if (unit_is(g, *U, hero::kTheDespot, true) || unit_is(g, *U, hero::kTheMonarch, true) ||
	    unit_is(g, *U, hero::kTheComrade, true))
		r += RON_K(g, "mil_patriot_radius_bonus");
	if (unit_is(g, *U, hero::kTheSenator, true) || unit_is(g, *U, hero::kThePresident, true) ||
	    unit_is(g, *U, hero::kTheCeo, true))
		r += RON_K(g, "econ_patriot_radius_bonus");
	return r;
}

int32_t find_hero(const Game& g, int32_t x, int32_t y, int32_t who, uint32_t mask, int32_t type, int32_t extra) {
	if (!valid_leader(who)) return -1;
	const Leader& l = g.leaders[size_t(who)];
	for (int32_t i = 0; i < l.num_heroes && size_t(i) < l.heroes.size(); i++) {
		const Caster& h = l.heroes[size_t(i)];
		if (!(h.hero_flags & 1)) continue;
		const Unit* U = g.unit(who, h.o);
		if (!U || !U->alive() || !U->on_map()) continue;
		if (type >= 0 && !unit_is(g, *U, type, false)) continue;
		const int32_t r = hero_get_radius(g, h);
		const int32_t d = vector_dist(x - U->x, y - U->y);
		if (d - extra <= r * 0xc0 && (mask == 0 || (U->unit_masks & mask) != 0)) return h.o;
	}
	return -1;
}

int32_t has_general(const Game& g, const Object& ob, uint32_t mask, int32_t type) {
	// (vslot 0x18 "active": a live unit is always active in this port, units never carry objflag::kActive)
	if (ob.is_unit && ob.alive()) {
		const Unit& u = static_cast<const Unit&>(ob);
		if (unit_is_hero(u) && (mask == 0 || (u.unit_masks & mask)) && (type < 0 || unit_is(g, u, type, false))) return u.o;
	}
	const int32_t extra = ob.is_unit || !ob.ptype ? 0 : (ob.ptype->y_size + ob.ptype->x_size) * 0x60;
	if (type >= kUnits && type < kEndUnits && valid_leader(ob.who) &&
	    g.leaders[size_t(ob.who)].num_units[size_t(type - kUnits)] == 0)
		return -1;
	if (!valid_leader(ob.who)) return -1;
	return find_hero(g, ob.x, ob.y, ob.who, mask, type, extra);
}

int32_t leader_has_general(const Game& g, int32_t who, int32_t x, int32_t y, uint32_t mask, int32_t type, int32_t extra) {
	if (!valid_leader(who)) return -1;
	if (type >= kUnits && type < kEndUnits && g.leaders[size_t(who)].num_units[size_t(type - kUnits)] == 0) return -1;
	return find_hero(g, x, y, who, mask, type, extra);
}

// ---------------------------------------------------------------- powers (4.4)

void leader_verify_spell_flags(Game& g, int32_t who) {
	Leader& l = g.leaders[size_t(who)];
	auto any = [&](uint32_t m) {
		for (int32_t i = 0; i < l.num_heroes && size_t(i) < l.heroes.size(); i++) {
			const Caster& h = l.heroes[size_t(i)];
			if (!(h.hero_flags & 1)) continue;
			const Unit* U = g.unit(who, h.o);
			if (U && (U->unit_masks & m)) return true;
		}
		return false;
	};
	if ((l.flags & 0x4000) && !any(0x2000)) l.flags &= ~0x4000u;
	if ((l.flags & 0x8000) && !any(0x8000)) l.flags &= ~0x8000u;
}

void caster_process_spells(Game& g, Caster& c, int32_t o, int32_t who, bool force) {
	bool changed = false;
	for (int32_t i = int32_t(c.active_spells.size()) - 1; i >= 0; i--) {
		const ActiveSpell S = c.active_spells[size_t(i)];
		if (!force && S.frame >= g.frame) {
			// (craft 0x283 JAM RADAR shockwave graphics: presentation; the craft is dead in the shipped data)
			continue;
		}
		Unit* U = g.unit(who, o);
		if (S.t == 0x27b) {
			if (U) U->unit_masks &= ~0x2800u;
			changed = true;  // (sound AMBUSH_OFF for the console)
		} else if (S.t == 0x27c) {
			if (U) U->unit_masks &= ~0x8000u;
			changed = true;  // (sound MARCH_OFF)
		}
		c.active_spells.erase(c.active_spells.begin() + i);
	}
	if (changed && valid_leader(who)) leader_verify_spell_flags(g, who);
}

void hero_add_spell(Game& g, const Unit& u, int32_t t, int32_t duration) {
	if (!valid_leader(u.who)) return;
	Leader& l = g.leaders[size_t(u.who)];
	if (u.cara < 0 || size_t(u.cara) >= l.heroes.size()) return;
	l.heroes[size_t(u.cara)].active_spells.push_back({t, g.frame, g.frame + duration});
}

// ---------------------------------------------------------------- Unit::process pieces (frame-spec 9)

int32_t unit_mana(const Game& g, const Unit& u) {
	// UnitData::mana @609a50
	const Type& t = *u.ptype;
	int32_t m = t.mana;
	if (m == 0) return 0;
	const int32_t who = u.who;
	if (t.domain == 2) {
		if (!has_wonder(g, who, 0x21e)) return m;
		return ((RON_K(g, "space_air_range") + 100) * m) / 100;
	}
	if (t.unit_flags2 & 0x40) m = m * (get_supply_upgrade(g, who) + 1);
	if (!has_tribe_bonus(g, who, 10)) return m;
	if (!unit_is(g, u, 0x36, true)) return m;
	return ((RON_K(g, "french_special_craft") + 100) * m) / 100;
}

bool unit_process_casters(Game& g, Unit& u) {
	const Type& t = *u.ptype;
	if (u.unit_masks & umask::kDecoy) {
		// A decoy ages in mana_burn and closes itself after decoy_time * (general upgrade + 2) / 2 frames.
		u.mana_burn = int16_t(u.mana_burn + 1);
		if (((get_general_upgrade(g, u.who) + 2) * RON_K(g, "decoy_time")) / 2 <= u.mana_burn) {
			unit_close(g, u, 0, -1, 0.0f);
			return false;
		}
		if (u.on_map()) {
			const int32_t fx = fcoord_of(u.x), fy = fcoord_of(u.y);
			if (is_detected_by_enemy(g, fx, fy, u.who) && suffer_attrition(g, u, g.frame % 7 == 0) != 0) return false;
		}
		return true;
	}
	if (!(t.unit_flags2 & 2)) {
		// TODO(aircraft, frame-spec 9): the air unit mana / fuel bookkeeping of this step.
		return true;
	}
	if (valid_leader(u.who)) {
		Leader& l = g.leaders[size_t(u.who)];
		if (unit_is_hero(u)) {
			if (u.cara >= 0 && size_t(u.cara) < l.heroes.size()) caster_process_spells(g, l.heroes[size_t(u.cara)], u.o, u.who, false);
		} else if (unit_is_special(u)) {
			if (u.hero >= 0 && size_t(u.hero) < l.specials.size())
				caster_process_spells(g, l.specials[size_t(u.hero)], u.o, u.who, false);
		}
	}
	// TODO(spells): every 32 frames the jamming / shockwave toggle (unit_masks bit 31, unit_masks2 0x8; craft 0x283 is dead).
	if (u.mana_burn != 0) {
		if ((t.unit_flags2 & 0x40) && !unit_is_gov_hero(u)) return true;  // supply units regenerate only as governors
		if ((u.unit_masks & 0x2a000) == 0) {  // not ambushing, marching or casting
			int32_t rate = 2;
			if (has_tribe_bonus(g, u.who, 10) && unit_is(g, u, 0x36, true)) rate = 4;
			if (has_preq(g, u.who, 0x301 /*SPIES_GENERALS_RECOVER_CRAFT*/) &&
			    (unit_is(g, u, 0x36, true) || unit_is(g, u, 0x3a, true)))
				rate *= 2;
			if (unit_is(g, u, hero::kMemnon, false)) {
				rate = RON_K(g, "memnon_regen_rate") * rate;
				rate = (rate + ((rate >> 31) & 0xff)) >> 8;
			}
			int32_t dec = (rate + (g.frame & 1)) / 2;
			dec = std::min<int32_t>(dec, u.mana_burn);
			u.mana_burn = int16_t(u.mana_burn - dec);
		}
	}
	return true;
}

void unit_process_cloak(Game& g, Unit& u) {
	if (((g.frame + u.o) & 0x1ff) == 0) {
		if (!(u.unit_masks & umask::kCoverBlown)) {
			u.unit_masks &= ~umask::kBlowCover;
			// (cloak particles when cloaked: presentation)
			// ❓ heroes-spec 6: unit_masks2 bits 0x20 / 0x40 are unnamed; copied as the decompile has them.
			u.unit_masks2 = (u.unit_masks2 & 0x40) ? (u.unit_masks2 & ~0x40u) : (u.unit_masks2 & ~0x20u);
		} else {
			u.unit_masks2 &= ~0x40u;
			u.unit_masks &= ~umask::kCoverBlown;
		}
	}
	u.unit_masks2 &= ~umask2::kPassiveCloak;
	const int32_t wx = wcoord_of(u.x), wy = wcoord_of(u.y);
	if ((u.unit_masks2 & 0x80) && g.world.in_w(wx, wy)) {
		const int32_t owner = g.world.w(wx, wy).who;
		if (owner >= 0 && owner < kNumLeaders) {
			if (owner == u.who ||
			    (g.leaders[size_t(u.who)].diplos(owner) == 2 && g.leaders[size_t(owner)].diplos(u.who) == 2))
				u.unit_masks2 |= umask2::kPassiveCloak;
		}
	}
	if (!(u.unit_masks & umask::kCloakRunning)) {
		Leader& l = g.leaders[size_t(u.who)];
		if (!(l.flags & 0x4000)) {
			u.unit_masks &= ~umask::kUnderCover;
		} else {
			bool found = false;
			for (int32_t i = 0; i < l.num_heroes && size_t(i) < l.heroes.size(); i++) {
				const Caster& h = l.heroes[size_t(i)];
				if (!(h.hero_flags & 1) || h.o == u.o) continue;
				const Unit* H = g.unit(u.who, h.o);
				if (!H || (H->unit_masks & 0x3000) != 0x2000) continue;
				if (!H->alive() || !H->on_map()) continue;  // is_map_unit (units are always "active" here)
				const int32_t r = hero_get_radius(g, h);
				if (vector_dist(u.x - H->x, u.y - H->y) <= r * 0xc0) {
					found = true;
					break;
				}
			}
			if (found) u.unit_masks |= umask::kUnderCover;
			else u.unit_masks &= ~umask::kUnderCover;
		}
	}
	if (has_tribe_bonus(g, u.who, 0x12) && u.ptype->domain == 0) {  // Iroquois: infantry hide in forest
		const int32_t tx = tcoord_of(u.x), ty = tcoord_of(u.y);
		if (g.world.in_t(tx, ty) && (g.world.t(tx, ty) & 0x30) == 0x30) u.unit_masks |= umask::kUnderCover;
		else u.unit_masks &= ~umask::kUnderCover;
	}
}

// ---------------------------------------------------------------- government heroes (5)

int32_t find_gov_hero_unit(const Game& g, int32_t who, int32_t x, int32_t y) {
	// ObjectsData::find_unit(x, y, FRIENDLY, who, -1, 0x20000, FILTER_GOV_HERO): the nearest unit of the leader (garrisoned
	// ones too; ties go to the later slot) whose type is a government hero. Allies' heroes are not looked at here (the
	// SEARCH_FRIENDLY relation of a hero search never reached beyond the leader in the shipped scenarios: ❓ heroes-spec 5.2).
	if (!valid_leader(who)) return -1;
	const ObjectList& L = g.objects[size_t(who)];
	int32_t best = -1, bd = 99999999;
	for (int32_t o = 0; o < L.unit_mark; o++) {
		const Unit* u = g.unit(who, o);
		if (!u || !u->alive() || !unit_is_gov_hero(*u)) continue;
		const int32_t d = vector_dist(u->x - x, u->y - y);
		if (d <= bd) {
			bd = d;
			best = o;
		}
	}
	return best;
}

bool leader_has_gov_hero_unit(const Game& g, int32_t who) {
	if (!valid_leader(who)) return false;
	return find_gov_hero_unit(g, who, 0, 0) >= 0;
}

void build_try_train_gov_hero(Game& g, Build& b) {
	Leader& l = g.leaders[size_t(b.who)];
	if (l.gov_hero_frame >= 0 && !leader_has_gov_hero_unit(g, b.who) && l.gov_hero_frame <= g.frame) {
		const int32_t h = get_gov_hero(g, b.who);
		if (h >= 0) {
			if (std::getenv("RON_HERO_TRACE")) std::fprintf(stderr, "[hero] f%d L%d building %d trains gov hero %d\n", g.frame, b.who, b.o, h);
			build_train(g, b, h);
		}
	}
}

// ---------------------------------------------------------------- plunder (7)

void unit_plunder(Game& g, Unit& u, int32_t o_killer, int32_t who_killer, int32_t aztec, int32_t despot) {
	if (u.unit_masks & umask::kDecoy) return;
	if (!valid_leader(who_killer)) return;
	if (who_killer != u.who && RON_K(g, "thedespot_unit_plunder") != 0 && despot == 0) {
		const Leader& KL = g.leaders[size_t(who_killer)];
		bool d = KL.num_units[size_t(hero::kTheDespot - kUnits)] != 0 &&
		         find_hero(g, u.x, u.y, who_killer, 0, hero::kTheDespot, 0) >= 0;
		if (KL.num_units[size_t(hero::kSpitamenes - kUnits)] != 0 && find_hero(g, u.x, u.y, who_killer, 0, hero::kSpitamenes, 0) >= 0)
			d = true;
		despot = d ? 1 : 0;
	}
	// A transport barge: the plunder looks at its first passenger.
	int32_t vw = u.who, vo = u.o;
	if (u.ptype->unit_flags & 0x10) {
		vw = u.inside_down_who;
		vo = u.inside_down;
	}
	if (vo < 0) {
		vw = u.who;
		vo = u.o;
	}
	Unit* V = g.unit(vw, vo);
	if (!V) V = &u;
	Leader& KL = g.leaders[size_t(who_killer)];
	if (o_killer == u.o && who_killer == u.who) {
		// Self kill (the Inca refund of take_damage / a disband): the victim's own price comes back.
		const Type* T = V->ptype;
		if (unit_is(g, *V, 0x42, false)) {
			if (V->rare == 0) V->rare = get_graft(g, u.who, 0x32);
			if (V->rare >= kUnits && V->rare < kEndUnits) T = &g.type(V->rare);
		}
		for (int32_t good = 0; good < 6; good++) {
			if (!type_avail(g, who_killer, good, 1)) continue;
			const int32_t r = (RON_K(g, "inca_refund") * get_cost(g, T->type, good, u.who, -1, -1, 1, 1, -1)) / 100;
			if (r != 0) KL.bucket[size_t(good)] += r;  // (console message)
		}
		return;
	}
	int32_t amount = 0, good = ptype::kWealth;
	if (unit_is_hero(*V)) amount = RON_K(g, "general_plunder");
	else if ((V->ptype->unit_flags2 & 0x40) || unit_is_caravan(*V)) amount = RON_K(g, "caravan_plunder");
	if (aztec || despot) {
		int32_t poorest = 2, best = 99999999;
		for (int32_t k = 0; k < 6; k++) {
			if (!type_avail(g, who_killer, k, 1) || k == ptype::kKnowledge) continue;
			const int32_t v = KL.bucket[size_t(k)] + g.rnd_draw(0x19);  // Random::get(0, 0xffff) % 25
			if (v < best) {
				best = v;
				poorest = k;
			}
		}
		int32_t bonus = 0;
		if (aztec) bonus = std::min((KL.units_killed + 1) * RON_K(g, "aztec_slay_bonus"), RON_K(g, "aztec_slay_cap"));
		if (despot) bonus += RON_K(g, "thedespot_unit_plunder");
		if (amount < bonus) {
			amount = bonus;
			good = poorest;
		}
	}
	if (!aztec) {
		const int32_t ap = RON_K(g, "aztec_plunder");
		if (ap != 0 && has_tribe_bonus(g, who_killer, 0)) amount = (ap * amount) / 100;
	}
	if ((g.scripted_rules || g.ctw) && !g.scen.plunder) return;
	if (KL.plunder_scale >= 0.0f) amount = int32_t(float(amount) * KL.plunder_scale);
	KL.bucket[size_t(good)] += amount;  // (console message and sound)
}

void build_plunder(Game& g, Build& b, int32_t who_razer) {
	if (!valid_leader(who_razer)) return;
	const int32_t who = b.who;
	if (who_razer == who && has_tribe_bonus(g, who, 0x13) && RON_K(g, "lakota_raze_price") != 0) return;
	const Type& T = *b.ptype;
	if (T.plunder_value == 0) return;
	const int32_t good = T.plunder_good;
	if (good < 0 || !type_avail(g, who_razer, good, 1)) return;
	int32_t v = T.plunder_value;
	if (has_tribe_bonus(g, who, 0xc) && b.type == 0x1a3) {
		v = ((100 - RON_K(g, "german_mine_cost")) * v) / 100;
		if (v == 0) return;
	}
	const bool complete = (b.build_masks & bmask::kActivated) != 0;
	if (who_razer == who) {
		if (!complete) return;
		if (b.type == 0x1b5 && has_wonder(g, who, 0x214)) v = ((100 - RON_K(g, "tikal_temple_cost")) * v) / 100;
	}
	bool full = false;  // skip the plunder percentage (LAB_006238da)
	if (!complete) {
		const int32_t ct = construct_time(g, b, 0);
		float f = float(double(b.job_counter_2)) / float(double(ct));
		if (f > 1.0f) f = 1.0f;
		v = int32_t(float(v) * f);
		full = true;
	} else if (who_razer == who) {
		// The city building of an own town keeps its full value; the town's absence does too (the original's fall-through).
		const int32_t town = get_town(g, b.type, tcoord_of(b.x), tcoord_of(b.y), who_razer);
		full = true;
		if (town >= 0) {
			const Build* tb = g.build(who_razer, town);
			if (tb && tb->city >= 0) {
				if (g.cities[size_t(who_razer)][size_t(tb->city)].race != who) full = false;
			}
		}
	}
	if (!full) v = (RON_K(g, "plunder") * v) / 100;
	if (v == 0) return;
	const int32_t ap = RON_K(g, "aztec_plunder");
	if (ap != 0 && who_razer != who && has_tribe_bonus(g, who_razer, 0)) v = ((ap + 100) * v) / 100;
	int32_t despot = 0;
	if (who_razer != who) {
		const Leader& RL = g.leaders[size_t(who_razer)];
		const int32_t extra = (T.y_size / 2 + T.x_size) * 0xc0;
		if (RL.num_units[size_t(hero::kTheDespot - kUnits)] != 0 &&
		    find_hero(g, b.x, b.y, who_razer, 0, hero::kTheDespot, extra) >= 0)
			despot = (RON_K(g, "thedespot_plunder") * v) / 100;
		if (RL.num_units[size_t(hero::kSpitamenes - kUnits)] != 0 &&
		    find_hero(g, b.x, b.y, who_razer, 0, hero::kSpitamenes, extra) >= 0)
			despot = (RON_K(g, "thedespot_plunder") * v) / 100;
	}
	if (g.scenario_mode && !g.scen.plunder) return;
	const float sc = g.leaders[size_t(who_razer)].plunder_scale;
	if (sc >= 0.0f) {
		v = int32_t(float(v) * sc);
		despot = int32_t(float(despot) * sc);
	}
	if (has_tribe_bonus(g, who, 0xd) && RON_K(g, "russian_plunder_steal") != 0 && who_razer != who) {
		bucket_add(g, who, good, v);
		bucket_add(g, who_razer, good, despot);
	} else {
		bucket_add(g, who_razer, good, v + despot);
	}
}

}  // namespace ron::sim
