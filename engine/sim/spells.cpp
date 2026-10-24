// Spell casting (docs/transport-spec.md section 4, docs/spells-spec.md): Unit::add_cast_order, Unit::do_cast (the untargeted
// branch: general powers, TRANSPORT, PACK / UNPACK, CIVILIAN ...; the targeted one: range walk, facing, cloak rules, job
// time), SpellTypeData::is_castable / is_valid_target / get_range / get_job_time, SpellType::pay_cast_costs /
// unpay_cast_costs, the SpellType::cast dispatch, the five general powers (heroes-spec 4) and Group::action_spell.
// The other cast bodies are spells_cast.cpp (spies, special forces, conversions, city casts, the AI's target search) and
// packers.cpp (PACK / UNPACK, the merchant spots, the pack predicates). RON_SPELL_TRACE=1 logs refused / killed casts,
// RON_CAST_LOG=1 (with it) every cast order created, RON_HERO_TRACE=1 every SpellType::cast.
#include "sim/spells.hpp"
#include <cstdio>
#include <cstdlib>

#include "sim/ai.hpp"
#include "sim/aircraft.hpp"
#include "sim/fog.hpp"
#include "sim/movement.hpp"
#include "sim/pathfinder.hpp"
#include "sim/combat_internal.hpp"
#include "sim/compass.hpp"
#include "sim/heroes.hpp"
#include "sim/trig.hpp"
#include "sim/game.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/production.hpp"
#include "sim/production_internal.hpp"
#include "sim/search.hpp"
#include "sim/economy.hpp"
#include "sim/garrison.hpp"
#include "sim/transport.hpp"

namespace ron::sim {

namespace {
// RON_SPELL_TRACE=1 prints why a cast order is refused or dies (and the casts that run) on stderr.
bool spell_trace_on() {
	static const bool on = std::getenv("RON_SPELL_TRACE") != nullptr || std::getenv("RON_HERO_TRACE") != nullptr;
	return on;
}
#define SPT(...)                                  \
	do {                                          \
		if (spell_trace_on()) {                   \
			std::fprintf(stderr, "[spell] " __VA_ARGS__); \
			std::fputc('\n', stderr);             \
		}                                         \
	} while (0)
}  // namespace

bool spell_is_pack(int32_t t) { return t == 0x28b || t == 0x28d || t == 0x28f || t == 0x291; }
bool spell_is_unpack(int32_t t) { return t == 0x28c || t == 0x28e || t == 0x290 || t == 0x292; }

// ---------------------------------------------------------------- the order (4.1)

Order* add_cast_order(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t x, int32_t y, int32_t sp, QueuePos q,
                      bool expl) {
	// @5e4a60. The QUEUE_NEW preamble is add_order's (clear_orders).
	// The pack / unpack requests become the variant of the unit's family (the original's UnitData::is(0x7b), the
	// merchants 0x3d / 0x3e / 400, is(0x13d)).
	if (sp == 0x28b) {
		if (unit_is(g, u, 0x7b)) sp = 0x28d;
		else if (u.type == 0x3d || u.type == 0x3e || u.type == 400) sp = 0x28f;
		else if (unit_is(g, u, 0x13d)) sp = 0x291;
	} else if (sp == 0x28c) {
		if (unit_is(g, u, 0x7b)) sp = 0x28e;
		else if (u.type == 0x3d || u.type == 0x3e || u.type == 400) sp = 0x290;
		else if (unit_is(g, u, 0x13d)) sp = 0x292;
	}
	Order c;
	c.type = kCastSpell;
	c.x = x;
	c.y = y;
	c.ox = ox;
	c.whom = whom;
	const Object* t = (ox < 0 || whom < 0) ? nullptr : g.object(whom, ox);
	c.uid = t ? t->uid : uint16_t(0xffff);
	c.spell = sp;
	c.paid = 0;
	c.flags = expl ? uint8_t(orderflag::kExplicit) : uint8_t(0);
	if (spell_trace_on() && std::getenv("RON_CAST_LOG"))
		std::fprintf(stderr, "[cast-order] f%d %d:%d type %d spell 0x%x target %d:%d queue %d%s\n", g.frame, u.who, u.o, u.type, unsigned(sp), whom, ox, int(q), expl ? " explicit" : "");
	return add_order(g, u, c, q);
}

// ---------------------------------------------------------------- castability (4.2)

namespace {

// terr_owner: the W-cell owner under (x, y), -1 for none / off the map.
int32_t terr_owner_at(const Game& g, int32_t x, int32_t y) {
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	return g.world.in_w(wx, wy) ? int32_t(g.world.w(wx, wy).who) : -1;
}

// LeaderData::is_peace @6e1200.
bool is_peace_lead(const Game& g, int32_t a, int32_t b) {
	if (a < 0 || a >= kNumLeaders || b < 0 || b >= kNumLeaders || a == b) return false;
	const int32_t d1 = g.leaders[size_t(a)].diplos(b), d2 = g.leaders[size_t(b)].diplos(a);
	return !(d1 == 0 || d2 == 0 || (d1 == 2 && d2 == 2));
}

// Game::teams_locked @594880.
bool teams_locked(const Game& g) {
	const uint8_t ts = g.info.team_style;
	return !(ts == 0 || ts == 8 || ts == 11);
}

bool leader_active(const Game& g, int32_t p) { return p >= 0 && p < kNumLeaders && (g.leaders[size_t(p)].flags & leaderflag::kActive) != 0; }

bool is_map_unit(const Object& ob) { return ob.is_unit && ob.alive() && static_cast<const Unit&>(ob).on_map(); }

// CityData::is_disbanding @7380a0: the DISBAND items queued in the city's buildings (finished, not the "no disband" types).
int32_t city_is_disbanding(const Game& g, int32_t who, int32_t c) {
	int32_t n = 0;
	for (int32_t o = g.cities[size_t(who)][size_t(c)].o, guard = 0; o >= 0 && guard < kUnitLimit; guard++) {
		const Build* b = g.build(who, o);
		if (!b) break;
		const int32_t next = b->city_down;
		if (b->alive() && (b->flags & objflag::kActive) && !(b->ptype->build_flags & 0x10)) n += build_count_queue(g, *b, 1, spell::kDisband);
		o = next;
	}
	return n;
}

// CityData::depop_can_queue @738060: the city building has no DEPOPULATE queued.
bool city_depop_can_queue(const Game& g, int32_t who, int32_t c) {
	const Build* b = g.build(who, g.cities[size_t(who)][size_t(c)].o);
	return b && build_count_queue(g, *b, 1, 0x286) == 0;
}

}  // namespace

int32_t spell_is_castable(const Game& g, int32_t sp, int32_t o, int32_t who, int32_t p3) {
	const Object* ob = who >= 0 && who < kNumLeaders && o >= 0 ? g.object(who, o) : nullptr;
	if (!ob || !ob->ptype || sp < 0 || sp >= kNumTypes) return 0;
	const Type& S = g.type(sp);
	// The owner chain (spells-spec 2.1): the object `is` the spell's FROM or FROM2 unit, or the tribe's graft of either.
	// Exempt: DISBAND (0x29a) and TRANSPORT (0x28a).
	auto is = [&](int32_t t) { return t >= 0 && type_is(*ob->ptype, t, false); };
	bool owns = is(S.from) || is(S.from2);
	if (!owns) owns = is(S.from >= 0 ? get_graft(g, who, S.from) : S.from) || is(get_graft(g, who, S.from2));
	if (!owns && sp != spell::kDisband && sp != spell::kTransport) return 0;
	// Decoys cast nothing but pack / unpack (asm: XOR EAX, so 0 although transport-spec 4.2 says 1).
	if (ob->is_unit && (static_cast<const Unit&>(*ob).unit_masks & umask::kDecoy) && !spell_is_pack(sp) && !spell_is_unpack(sp))
		return 0;
	switch (sp) {
	case 0x27e: {  // SUPPLIES: only inside an own city (dead in the shipped data: craft FROM none)
		if (!is_map_unit(*ob)) return 0;
		const int32_t wx = wcoord_of(ob->x), wy = wcoord_of(ob->y), tx = tcoord_of(ob->x), ty = tcoord_of(ob->y);
		if (!g.world.in_t(tx, ty) || !(g.world.t(tx, ty) & tmask::kCity) || !g.world.in_w(wx, wy) || g.world.w(wx, wy).who != who)
			return 2;
		const int32_t c = find_city_at(g, ob->x, ob->y, who, -1, 0);
		if (c < 0 || g.cities[size_t(who)][size_t(c)].race != who) return 2;
		return 3;
	}
	case 0x284: {  // PARADROP: allied land (dead: FROM none)
		if (!is_map_unit(*ob)) return 0;
		const int32_t ow = terr_owner_at(g, ob->x, ob->y);
		return (ow >= 0 && is_ally(g, who, ow)) ? 3 : 2;
	}
	case 0x286: {  // DEPOPULATE: a building queue item
		if (ob->is_unit) return 0;
		const Build& b = static_cast<const Build&>(*ob);
		if (b.city >= 0 && p3 == 0 && size_t(b.city) < g.cities[size_t(who)].size() && !city_depop_can_queue(g, who, b.city)) return 1;
		return spell_is_castable(g, spell::kDisband, o, who, p3);
	}
	case 0x287: {  // MOVE CAPITAL (the scenario editor's only)
		if (ob->is_unit) return 0;
		const Build& b = static_cast<const Build&>(*ob);
		if (is_unassimilated(g, b)) return 0;
		if (p3 == 0 && researching(g, who, 0x287, o, true, false)) return 0;
		if (b.city < 0 || size_t(b.city) >= g.cities[size_t(who)].size()) return 0;
		const City& C = g.cities[size_t(who)][size_t(b.city)];
		const int32_t f = C.founder;
		if (((C.was_capital_flags >> (f & 31)) & 1) && f != who && leader_active(g, f)) return 0;
		return 3;
	}
	case 0x288:
	case 0x289:
		return 0;  // never castable (transport-spec merged them with 0x28a)
	case spell::kTransport:
		return object_is_cargo(g, *ob) ? 3 : 0;
	case 0x28b: case 0x28d: case 0x28f: case 0x291:  // PACK
		if (!is_map_unit(*ob)) return 0;
		return (static_cast<const Unit&>(*ob).unit_masks & umask::kPacked) ? 0 : 3;
	case 0x28c: case 0x28e: case 0x290: case 0x292:  // UNPACK
		if (!is_map_unit(*ob)) return 0;
		return (static_cast<const Unit&>(*ob).unit_masks & umask::kPacked) ? 3 : 0;
	case 0x294: {  // TO ARMS: citizens / scholars with a militia line tech, on own / allied / unowned land
		if (!is_map_unit(*ob)) return 0;
		auto tribe_type = [&](int32_t t) { return tribe_can_type(g, who, t) ? t : get_graft(g, who, t); };
		if (!has_tech(g, who, tribe_type(ptype::kMilitia)) && !has_tech(g, who, tribe_type(ptype::kMinuteman)) &&
		    !has_tech(g, who, tribe_type(ptype::kPartisan)))
			return 0;
		const int32_t ow = terr_owner_at(g, ob->x, ob->y);
		if (ow < 0 || ow == who || is_ally(g, who, ow)) return 3;
		return 0;
	}
	case 0x297:
	case 0x298:
		return is_map_unit(*ob) ? 3 : 0;
	case spell::kDisband: {
		if (p3 == 0 && researching(g, who, spell::kDisband, o, true, false)) return 0;
		if (ob->is_unit) return 0;  // !is_building
		const Build& b = static_cast<const Build&>(*ob);
		if (!(b.ptype->build_flags & 0x10) || (b.flags & objflag::kCity)) {
			const int32_t c = b.city;
			if (c >= 0 && size_t(c) < g.cities[size_t(who)].size()) {
				const City& C = g.cities[size_t(who)][size_t(c)];
				if (!(b.flags & objflag::kCity)) {
					if (city_is_disbanding(g, who, c) > (p3 != 0 ? 1 : 0)) return 0;
				} else {
					if (C.city_flags & cityflag::kCapital) return 0;
					for (int32_t i = 0; i < 8; i++)
						if (i != who && ((C.was_capital_flags >> i) & 1) && leader_active(g, i)) return 0;
				}
			}
		}
		return 3;
	}
	default:
		return 3;  // no case in the switch: castable once the owner chain passed
	}
}

// ---------------------------------------------------------------- targets and range (spells-spec 2.2, 2.3)

namespace {
bool unit_flag_vehicle(const Object& ob) { return (uint32_t(ob.ptype->obj_masks) & 0x200000u) != 0; }
bool obj_is_infiltrated(const Object& ob, int32_t p) {
	// ObjectData::is_infiltrated(p) @script 9f9180: another leader's plant (p < 0: any).
	const uint8_t m = uint8_t(ob.infiltrated);
	return p != ob.who && m != 0 && (p < 0 || (m & (1u << (p & 31))));
}
}  // namespace

int32_t spell_is_valid_target(const Game& g, int32_t sp, int32_t who, int32_t o, int32_t tw, int32_t* reason) {
	if (reason) *reason = 0;
	const Type& S = g.type(sp);
	const uint32_t F = S.spell_flags;
	if ((F & 0xe) == 0 || (F & 8)) return 1;
	const Object* T = (tw >= 0 && tw < kNumLeaders && o >= 0) ? g.object(tw, o) : nullptr;
	if (!T || !T->ptype) return 0;
	auto rs = [&](int32_t r) {
		if (reason) *reason = r;
	};
	if (T->is_unit && !(F & 2) && sp == 0x280 && !unit_flag_vehicle(*T)) {
		rs(1);
		return 0;
	}
	if (!T->is_unit && !(F & 4)) {
		rs(2);
		return 0;
	}
	if (who == tw) {
		if (!(F & 0x10)) {
			rs(3);
			return 0;
		}
	} else if (is_enemy(g, who, tw)) {
		if (!(F & 0x20)) {
			rs(6);
			return 0;
		}
	} else if (is_ally(g, who, tw)) {
		if (!(F & 0x80)) {
			rs(4);
			return 0;
		}
	} else if (is_peace_lead(g, who, tw)) {
		if (!(F & 0x400)) {
			rs(5);
			return 0;
		}
	}
	rs(8);
	if (T->is_unit) {
		if ((F & 0x100) && !unit_flag_vehicle(*T)) return 0;
		if ((F & 0x200) && unit_flag_vehicle(*T)) return 0;
	}
	const Unit* TU = T->is_unit ? static_cast<const Unit*>(T) : nullptr;
	if (sp == 0x276) {
		if (!(!T->is_unit && (T->flags & objflag::kActive)) || !(T->flags & objflag::kCity)) return 0;
	}
	if (sp == 0x27f && obj_is_infiltrated(*T, who)) return 0;
	if (sp == 0x275) {
		if (has_general(g, *T, 0, hero::kTheCeo) >= 0) return 0;
		if (TU && unit_is_gov_hero(*TU)) return 0;
		if (tw == who) return 0;
		if (is_ally(g, who, tw)) {
			if (!teams_locked(g)) return 0;
			if (leader_active(g, tw)) return 0;
		} else {
			if (!is_enemy(g, who, tw)) return 0;
			if (TU) {
				if (type_is(*T->ptype, ptype::kSpy, false)) return 0;
				if (obj_attack(g, *T) == 0 && !unit_is_hero(*TU)) return 0;
				if (unit_is_caravan(*TU)) return 0;
				if (combat_detail::is_worker_type(TU->type)) return 0;
				if (T->ptype->unit_flags2 & 0x40) return 0;  // is_supply
				if (unit_is_merchant(*TU)) return 0;
				if (T->ptype->domain != 0) return 0;
			} else if (terr_owner_at(g, T->x, T->y) != who) {
				return 0;
			}
		}
	}
	if (sp == 0x281) {
		if (has_general(g, *T, 0, hero::kTheCeo) >= 0) return 0;
		if (TU && unit_is_gov_hero(*TU)) return 0;
		if (T->ptype->domain != 0) return 0;
		const bool supply = (T->ptype->unit_flags2 & 0x40) != 0;
		if (obj_attack(g, *T) == 0 && !(TU && unit_is_hero(*TU)) && !supply) {
			if (!type_is(*T->ptype, 0x45, false)) return 0;
		}
		if (TU && unit_is_caravan(*TU)) return 0;
		if (TU && combat_detail::is_worker_type(TU->type)) return 0;
		if (TU && unit_is_merchant(*TU)) return 0;
	}
	if (sp == 0x277) {
		bool ok = false;
		if (!object_is_seen(g, *T, who, false)) ok = false;
		else if (tw == who) ok = obj_is_infiltrated(*T, -1);
		else if (is_ally(g, who, tw)) ok = obj_is_infiltrated(*T, -1) && !obj_is_infiltrated(*T, who);
		else if (is_enemy(g, who, tw)) ok = type_is(*T->ptype, ptype::kSpy, false);
		if (ok) rs(0);
		return ok ? 1 : 0;
	}
	rs(0);
	return 1;
}

int32_t spell_get_range(const Game& g, int32_t sp, int32_t who, int32_t to, int32_t tw) {
	int32_t R = g.type(sp).spell_range;
	if (R == 0) return 0;
	const Object* T = (to >= 0 && tw >= 0 && tw < kNumLeaders) ? g.object(tw, to) : nullptr;
	if (sp == 0x275 || sp == 0x277) {
		R += get_spy_upgrade(g, who) * RON_K(g, "spy_bribe_upgrade_range") * 0xc0;
		if (has_wonder(g, who, 0x211)) R += RON_K(g, "terra_cotta_range") * 0xc0;
		if (T && (type_is_siege(*T->ptype) || (T->ptype->unit_flags2 & 0x40) || !T->is_unit)) R = 0xc0;
	}
	if (sp == 0x27f) {
		R += get_spy_upgrade(g, who) * RON_K(g, "spy_informer_upgrade_range") * 0xc0;
		if (T && !T->is_unit) R = (R + 1) / 2;
	}
	if (sp == 0x281 && T && T->is_unit && unit_flag_vehicle(*T)) R = 0xc0;
	return R;
}

// ---------------------------------------------------------------- costs (4.3)

int32_t pay_cast_costs(Game& g, int32_t sp, int32_t o, int32_t who) {
	// @676c40
	const int32_t q = can_pay_cost(g, sp, who, -1, 0, 1);
	if (q == 0) return 1;
	if (q != 10 || !g.editor) {  // free in the editor (semaphore byte1 & 8)
		pay_cost(g, sp, who, o, -1, 1, nullptr);
		Unit* u = g.unit(who, o);
		if (u) u->mana_burn = int16_t(u->mana_burn + g.type(sp).mana);
	}
	return 0;
}

void unpay_cast_costs(Game& g, int32_t sp, int32_t o, int32_t who) {
	// @676ff0
	unpay_cost(g, sp, who, o, -1);
	Unit* u = g.unit(who, o);
	if (!u) return;
	int32_t take = u->mana_burn;
	if (g.type(sp).mana < take) take = g.type(sp).mana;
	u->mana_burn = int16_t(u->mana_burn - take);
}

// ---------------------------------------------------------------- job time (4.4)

int32_t spell_job_time(const Game& g, int32_t sp, int32_t o, int32_t who) {
	// @675800
	int32_t t = int32_t(g.type(sp).job_time);
	const Unit* u = (o >= 0 && who >= 0 && who < kNumLeaders) ? g.unit(who, o) : nullptr;
	if (sp == 0x27d && who >= 0) {
		if (has_tribe_bonus(g, who, 10)) t = (RON_K(g, "french_entrench") * t) / 100;
		if (o >= 0 && u && u->alive()) {
			// The asm calls is(0x169) and discards the result (@675800; spells-spec 2.4): every caster's entrench is scaled.
			t = RON_K(g, "antipater_entrench_rate") * t;
			t = (t + ((t >> 31) & 0xff)) >> 8;
		}
	}
	if ((sp == 0x275 || sp == 0x27f) && who >= 0) {
		if (has_preq(g, who, 0x304 /*SPIES_CRAFT_FASTER*/)) t = t / 2;
	}
	if (sp == 0x28b || sp == 0x28c) {
		if (who >= 0 && has_tribe_bonus(g, who, 8)) t = (RON_K(g, "turk_pack") * t) / 100;
		// Napoleon: Leader +0x59cc != 0 (❓ taken as "owns a Napoleon", num_units[0x167]); has_general is -1 until heroes exist.
		if (who >= 0 && who < kNumLeaders && g.leaders[size_t(who)].num_units[size_t(0x167 - kUnits)] != 0 && u) {
			if (has_general(g, *u, 0, 0x167) >= 0 && type_is_siege(*u->ptype)) t = (RON_K(g, "napoleon_pack") * t) / 100;
		}
		if (u) {
			if (unit_is(g, *u, 0x10f, true) || unit_is(g, *u, 0x116, true)) {
				t = t / 2;
			} else if (unit_is(g, *u, 0x111, true)) {
				t = (t + ((t >> 31) & 3)) >> 2;
			}
		}
	}
	if ((sp == 0x28d || sp == 0x28e) && u && unit_is(g, *u, 0x7d)) t = t / 2;
	if ((sp == 0x281 || sp == 0x280) && who >= 0) {
		if (has_tribe_bonus(g, who, 9)) t = t / 2;
		if (o >= 0 && u && unit_is(g, *u, 0x177, true)) t = 10;
	}
	return t;
}

// ---------------------------------------------------------------- the handler (4.5)

namespace {

// The untargeted branch of Unit::do_cast (spells-spec 3.2): the general powers, TRANSPORT, PACK / UNPACK, CIVILIAN ...
// Returns false when the order was killed (the caller returns 0).
int32_t do_cast_untargeted(Game& g, Unit& u, Order& ord, int32_t sp, int32_t eff) {
	if (u.spell_time == 0) {  // the first frame of the cast
		int32_t anim = kCharDefault, p3 = 1;
		if (spell_is_pack(sp)) {
			if (u.unit_masks & umask::kPacked) {
				kill_current_order(g, u, false);
				return 0;
			}
			if (unit_is_merchant(u)) unit_update_gpiece(g, u);  // (scene.recalc_builds: presentation)
			unit_set_angle(g, u, u.guys.empty() ? u.angle : u.guys[0].angle, false);
			anim = kCharPack;
		} else if (spell_is_unpack(sp)) {
			if (!(u.unit_masks & umask::kPacked)) {
				kill_current_order(g, u, false);
				return 0;
			}
			if (unit_is_rare_collector(g, u)) {
				if (!good_merchant_spot(g, u, tcoord_of(u.x), tcoord_of(u.y))) {
					kill_current_order(g, u, false);  // (feedback INVALID; the cost is refunded by the kill)
					return 0;
				}
				unit_set_new_location(g, u, ucoord_of(u.x) * 48 + 24, ucoord_of(u.y) * 48 + 24, true, true);
			}
			unit_set_angle(g, u, u.guys.empty() ? u.angle : u.guys[0].angle, false);
			anim = kCharUnpack;
		} else if (u.ptype->unit_flags2 & utflag2::kHero) {  // UnitData::is_hero
			if (sp == 0x27b) anim = kCharAttack1, p3 = 0;
			else if (sp == 0x27c) anim = kCharAttack2, p3 = 0;
			else if (sp == 0x279) anim = kCharAttack3, p3 = 0;
		}
		unit_set_anim(g, u, anim, 0, p3);
		if (sp == spell::kTransport) {
			int32_t sx = 0, sy = 0;
			// No water spot for a barge (the base type 320, not the upgrade): give up.
			if (find_nearby_spot(g, g.type(ptype::kTransportBarge), u.x, u.y, &sx, &sy, 0, K("unit_board_distance"), 0, 0x55555555,
			                     kFilterNotMe, -1, -1, 0, 0, -1, 0, -1) != 0) {
				kill_current_order(g, u, false);
				return 0;
			}
		}
	}
	// A pack / unpack by a siege weapon next to THE MONARCH runs twice as fast (spell_time advances by two; the guys'
	// animation clock +0x74 / +0x78 is presentation).
	if ((spell_is_pack(eff) || spell_is_unpack(eff)) && type_is_siege(*u.ptype) && has_general(g, u, 0, hero::kTheMonarch) >= 0)
		u.spell_time = int16_t(u.spell_time + 1);
	u.spell_time = int16_t(u.spell_time + 1);
	if (u.spell_time < int16_t(spell_job_time(g, eff, u.o, u.who))) return 0;
	if (sp == spell::kTransport && u.o_up >= 0) {  // a follower waits for its captain
		const Unit* cap = g.unit(u.who, obj_captain(g, u));
		if (cap && cap->order_type() == kCastSpell) {
			u.spell_time = int16_t(u.spell_time - 1);
			return 0;
		}
	}
	u.spell_time = 0;
	if (is_spell_type(sp)) {
		spell_cast(g, sp, u.o, u.who, 0, 0);
	} else if (u.o_up < 0) {
		// "Upgrade by casting" (spells-spec 3.3): the captain takes the current upgrade of the unit type. (The walk into a
		// University on the unit's tile, TData 3, is not ported: no command creates such an order.)
		unit_set_type(g, u, current_upgrade(g, u.who, sp), 0);
	}
	if (sp == spell::kTransport) return 0;  // the order now lives on the barge (cast_transport)
	ord.paid = 0;
	kill_current_order(g, u, false);
	return 0;
}

// The targeted branch (spells-spec 3.1).
int32_t do_cast_targeted(Game& g, Unit& u, Order& ord, int32_t sp) {
	const Type& S = g.type(sp);
	const uint32_t F = S.spell_flags;
	int32_t ox = ord.ox, tw = ord.whom;
	if (tw < 0 || tw >= kNumLeaders || ox < 0 || !g.object(tw, ox)) {
		// ❓ spells-spec 11 (13): the original indexes objects[whom][ox] without a guard.
		if (!(F & 8) || tw < 0 || tw >= kNumLeaders) {
			SPT("f%d %d:%d cast 0x%x: order killed (#1)", g.frame, u.who, u.o, unsigned(sp));
			kill_current_order(g, u, false);
			return 0;
		}
	}
	Object* T = (ox >= 0 && tw >= 0 && tw < kNumLeaders) ? g.object(tw, ox) : nullptr;
	if (T && !T->alive()) {  // follow the chain top
		const int32_t c = obj_captain(g, *T);
		if (c != ox) {
			ox = c;
			ord.ox = c;
			if (Object* N = g.object(tw, c)) {
				ord.uid = N->uid;
				T = N;
			}
		}
	}
	if (T && T->is_unit && !static_cast<Unit*>(T)->on_map()) {  // target the container
		int32_t cw = -1;
		const int32_t c = unit_get_inside(g, *static_cast<Unit*>(T), &cw);
		ox = c;
		tw = cw;
		ord.ox = c;
		ord.whom = cw;
		if (c < 0 || cw < 0) {
			SPT("f%d %d:%d cast 0x%x: order killed (#2)", g.frame, u.who, u.o, unsigned(sp));
			kill_current_order(g, u, false);
			return 0;
		}
		T = g.object(cw, c);
		ord.uid = T ? T->uid : uint16_t(0xffff);
	}
	int32_t why = 0;
	if (!spell_is_valid_target(g, sp, u.who, ox, tw, &why)) {
		SPT("f%d %d:%d cast 0x%x: target %d:%d invalid (reason %d)", g.frame, u.who, u.o, unsigned(sp), tw, ox, why);
		SPT("f%d %d:%d cast 0x%x: order killed (#3)", g.frame, u.who, u.o, unsigned(sp));
			kill_current_order(g, u, false);
		return 0;
	}
	if (sp == 0x275 && unit_is(g, u, hero::kTheSenator, true)) {  // the Senator bribes only on own / allied land
		const int32_t ow = T ? terr_owner_at(g, T->x, T->y) : -1;
		if (ow < 0 || !is_ally(g, u.who, ow)) {
			SPT("f%d %d:%d cast 0x%x: order killed (#4)", g.frame, u.who, u.o, unsigned(sp));
			kill_current_order(g, u, false);
			return 0;
		}
	}
	u.cavarch_o = int16_t(ox);  // spell_target_o / who / uid (+0xa2 / +0xa8 / +0xa6)
	u.cavarch_who = int8_t(tw);
	u.cavarch_uid = ox < 0 ? uint16_t(0xffff) : (T ? T->uid : uint16_t(0xffff));
	int32_t tx, ty, radius = 0;
	if (!(F & 8)) {
		if (!T || !T->alive()) {
			SPT("f%d %d:%d cast 0x%x: order killed (#5)", g.frame, u.who, u.o, unsigned(sp));
			kill_current_order(g, u, false);
			return 0;
		}
		tx = T->x;
		ty = T->y;
		radius = T->is_unit ? T->ptype->big_radius : (T->ptype->x_size + T->ptype->y_size) * 0x30;
	} else {
		tx = ord.x;
		ty = ord.y;
	}
	const int32_t R = spell_get_range(g, sp, u.who, ox, tw);
	if (R != 0 && vector_dist(u.x - tx, u.y - ty) > R + radius) {  // walk into range
		const uint32_t a = find_angle(u.x - tx, u.y - ty);
		int32_t nx = 0, ny = 0;
		if (find_nearby_spot(g, *u.ptype, tx, ty, &nx, &ny, R + radius - 0xc0, R + radius - 0x30, 0, a, kFilterNotMe, u.o, u.who, 0, 0,
		                     -1, 0, -1) != 0)
			return 0;  // keeps the order
		if (vector_dist(nx - tx, ny - ty) > R + radius) return 0;
		add_move_order(g, u, nx, ny, 1, false, kQueueFirst, false, -1, -1);
		return 0;
	}
	if (tw != u.who && ox >= 0) {  // reveal and face
		if (!T || !object_is_seen(g, *T, u.who, false)) {
			SPT("f%d %d:%d cast 0x%x: target %d:%d not seen", g.frame, u.who, u.o, unsigned(sp), tw, ox);
			SPT("f%d %d:%d cast 0x%x: order killed (#6)", g.frame, u.who, u.o, unsigned(sp));
			kill_current_order(g, u, false);
			return 0;
		}
		u.flags |= objflag::kAttacking;
		if (!(u.visible & (1 << (tw & 7)))) {
			if (!is_seen(g, fcoord_of(u.x), fcoord_of(u.y), tw)) {
				u.visible = int8_t(u.visible | (1 << (tw & 7)));
				object_update_local_seen(g, u);
			} else {
				u.visible = int8_t(u.visible | (1 << (tw & 7)));
			}
		}
		unit_set_angle(g, u, find_angle(T->x - u.x, T->y - u.y), true);
	}
	// the cast animation
	int32_t anim, p3 = 0;
	const bool target_spy = T && type_is(*T->ptype, ptype::kSpy, false);
	if (unit_is(g, u, ptype::kSpy)) {
		if (sp == 0x275) anim = kCharAttack3;
		else if (sp == 0x27f) anim = kCharAttackWalk;
		else anim = target_spy ? kCharAttack2 : kCharAttack1;
	} else if (unit_is(g, u, 0x45)) {
		if (sp == 0x280) anim = kCharAttackWalk;
		else if (sp == 0x281) anim = kCharAttack3;
		else anim = target_spy ? kCharAttack2 : kCharAttack1;
	} else {
		anim = kCharAttack1;
		p3 = 1;
	}
	unit_set_anim(g, u, anim, 0, p3);
	if (!(u.unit_masks & umask::kCasting)) u.unit_masks |= umask::kCasting;  // (the enemy-sees-you sound and bubble: UI)
	if (unit_is_cloaked(u) && !(F & 0x800) && sp != 0x281 && !unit_is(g, u, ptype::kSpy)) u.unit_masks |= umask::kBlowCover | umask::kCoverBlown;
	if (sp == 0x275) u.unit_masks2 |= 0x60;  // ❓ both bits, every frame (decayed by process_cloak)
	u.spell_time = int16_t(u.spell_time + 1);
	if (u.spell_time < int16_t(spell_job_time(g, sp, u.o, u.who))) return 0;
	if (unit_is_cloaked(u) && !(F & 0x800)) u.unit_masks |= umask::kBlowCover | umask::kCoverBlown;  // at completion: no exemption
	u.spell_time = 0;
	if (!(F & 8)) spell_cast(g, sp, u.o, u.who, ox, tw);
	else spell_cast(g, sp, u.o, u.who, ord.x, ord.y);
	// the order may have been killed (the caster died, or its orders were cleared): only touch it when it is still ours
	if (!u.orders.empty() && &u.orders.front() == &ord) ord.paid = 0;
	SPT("f%d %d:%d cast 0x%x: done", g.frame, u.who, u.o, unsigned(sp));
	if (u.alive()) kill_current_order(g, u, false);
	return 0;
}

}  // namespace

int32_t do_cast(Game& g, Unit& u, Order& ord) {
	// @5ebfe0
	const int32_t sp = ord.spell;
	const bool is_spell = is_spell_type(sp);
	const int32_t eff = is_spell ? sp : spell::kDefault;
	if (ord.paid == 0) {
		if (pay_cast_costs(g, sp, u.o, u.who) != 0) {
			// (console: S_INVALID_ORDER and a red feedback line)
			kill_current_order(g, u, false);
			return 0;
		}
		ord.paid = 1;
	}
	if (eff == spell::kDefault || (g.type(eff).spell_flags & 0xe) == 0) return do_cast_untargeted(g, u, ord, sp, eff);
	return do_cast_targeted(g, u, ord, sp);
}

// ---------------------------------------------------------------- the general powers (heroes-spec 4)

namespace {

// D = spell.duration + general upgrade * spell.duration_upgrade frames.
int32_t power_duration(const Game& g, int32_t sp, int32_t who) {
	const Type& S = g.type(sp);
	return S.duration + get_general_upgrade(g, who) * S.duration_upgrade;
}

// SpellType::cast_supplies @6714c0 (0x27e): the caster's mana refills.
void cast_supplies(Game& g, int32_t o, int32_t who) {
	if (Unit* u = g.unit(who, o)) u->mana_burn = 0;
}

// SpellType::cast_march @671500 (0x27c).
void cast_march(Game& g, int32_t o, int32_t who) {
	Unit* u = g.unit(who, o);
	if (!u) return;
	hero_add_spell(g, *u, 0x27c, power_duration(g, 0x27c, who));
	u->unit_masks |= umask::kMarchRunning;
	g.leaders[size_t(who)].flags |= 0x8000;  // kMarchActive (sound S_GENERAL_MARCH for the console)
}

// SpellType::cast_ambush @671610 (0x27b).
void cast_ambush(Game& g, int32_t o, int32_t who) {
	Unit* u = g.unit(who, o);
	if (!u) return;
	int32_t d = power_duration(g, 0x27b, who);
	if (unit_is(g, *u, hero::kParmenio)) {
		d = RON_K(g, "parmenio_ambush_time") * d;
		d = (d + ((d >> 31) & 0xff)) >> 8;
	}
	hero_add_spell(g, *u, 0x27b, d);
	u->unit_masks |= umask::kCloakRunning | umask::kUnderCover;
	g.leaders[size_t(who)].flags |= 0x4000;  // kAmbushActive
}

// Objects::find_units(x, y, SEARCH_FRIENDLY, who, radius, 0x200, FILTER_ALL, ..., leaders 0..7) @65a620: the live on-map
// units of `who` and its allies in the same region within `radius`, as (o, who).
std::vector<std::pair<int32_t, int32_t>> find_friendly_units(const Game& g, int32_t x, int32_t y, int32_t who, int32_t radius) {
	std::vector<std::pair<int32_t, int32_t>> out;
	const int32_t wx0 = wcoord_of(x), wy0 = wcoord_of(y);
	const int32_t tx0 = tcoord_of(x), ty0 = tcoord_of(y);
	const int32_t reg = g.world.in_t(tx0, ty0) ? g.world.get_tregion(tx0, ty0) : -1;
	auto region_of = [&](const Unit& u) {
		const int32_t tx = tcoord_of(u.x), ty = tcoord_of(u.y);
		return g.world.in_t(tx, ty) ? g.world.get_tregion(tx, ty) : -1;
	};
	auto friendly = [&](int32_t cw) { return is_ally(g, cw, who); };  // Search::valid_search(SEARCH_FRIENDLY)
	const Circles& C = circles();
	const int32_t k = (radius + 0x2ff) / 0x300;
	if (radius >= 0 && k <= 64 && C.radius[size_t(k)] <= g.total_units) {
		for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
			const int32_t cx = wx0 + C.x[size_t(i)], cy = wy0 + C.y[size_t(i)];
			if (!g.world.in_w(cx, cy)) continue;
			const WData& d = g.world.w(cx, cy);
			int32_t co = d.down, cw = d.down_who;
			while (co >= 0) {
				const Object* ob = g.object(cw, co);
				if (!ob) break;
				const int32_t no = ob->down, nw = ob->down_who;
				if (cw < 8 && ob->is_unit && friendly(cw)) {
					const Unit& u = static_cast<const Unit&>(*ob);
					if (u.alive() && u.on_map() && region_of(u) == reg) out.emplace_back(co, cw);
				}
				co = no;
				cw = nw;
			}
		}
		return out;
	}
	for (int32_t p = 0; p < 8; p++) {
		if (!(g.leaders[size_t(p)].flags & leaderflag::kValid) || !friendly(p)) continue;
		for (int32_t co = 0; co < g.objects[size_t(p)].unit_mark; co++) {
			const Unit* u = g.unit(p, co);
			if (!u || !u->alive() || !u->on_map() || region_of(*u) != reg) continue;
			// ❓ the original subtracts the type field +0x244 (+0x2f8 for sea units); big_radius is taken for both.
			const int32_t dd = vector_dist(u->x - x, u->y - y) - u->ptype->big_radius;
			if (radius < 0 || dd <= radius) out.emplace_back(co, p);
		}
	}
	return out;
}

// SpellType::cast_entrench @672820 (0x27d): idle friendly land combat captains in the general's radius dig in.
void cast_entrench(Game& g, int32_t o, int32_t who) {
	Unit* U = g.unit(who, o);
	if (!U || U->cara < 0 || size_t(U->cara) >= g.leaders[size_t(who)].heroes.size()) return;
	const int32_t r = hero_get_radius(g, g.leaders[size_t(who)].heroes[size_t(U->cara)]);
	const bool antipater = unit_is(g, *U, hero::kAntipater);
	const auto found = find_friendly_units(g, U->x, U->y, who, r * 0xc0);
	for (const auto& fw : found) {
		const int32_t fo = fw.first, fwho = fw.second;
		Unit* F = g.unit(fwho, fo);
		if (!F || !F->alive()) continue;
		const Type& T = *F->ptype;
		if (!(T.obj_masks & 0x20) || T.attack == 0 || !(uint32_t(T.role) & role::kCombat)) continue;
		if (F->o_up >= 0) continue;  // is_captain
		if (attack_dist(g, *U, fo, fwho) > r * 0xc0) continue;
		if (!F->orders.empty()) continue;  // UnitData::is_idle
		if (objects_find_unit(g, F->x, F->y, search::kEnemy, fwho, 0x600, 0x2000, filter::kCombat, 0, 0, filter::kAll, 0, 0) >= 0)
			continue;
		if (!g.field_battle && !antipater) {  // only on own / allied land
			const int32_t wx = wcoord_of(F->x), wy = wcoord_of(F->y);
			const int32_t owner = g.world.in_w(wx, wy) ? g.world.w(wx, wy).who : -1;
			if (owner < 0 || !is_ally(g, fwho, owner)) continue;
		}
		for (int32_t co = fo; co >= 0;) {
			Unit* C = g.unit(fwho, co);
			if (!C || !C->alive() || !C->on_map()) break;
			int32_t tx = -1, tw = -1;
			const Order* act = get_action(*C);
			if (act && act->type == kAttack) {
				tx = act->ox;
				tw = act->whom;
			}
			C->unit_masks &= ~umask::kMultimove;
			C->path.clear();
			close_orders(g, *C, false);
			clear_partial_path(*C);
			update_action(g, *C);
			if (tx >= 0) add_attack_order(g, *C, tx, tw, kQueueNew, 0, false);
			// (entrench graphic event when the unit was not entrenched yet)
			C->unit_masks |= umask::kEntrenched;
			if (antipater) C->unit_masks2 |= umask2::kAntipaterEntr;
			C->trench_angle = C->angle;
			co = C->o_down;
		}
	}
}

// SpellType::cast_create_decoy @674370 (0x27a): copies of nearby combat captains, marked kDecoy.
void cast_create_decoy(Game& g, int32_t o, int32_t who) {
	Unit* U = g.unit(who, o);
	if (!U || U->cara < 0 || size_t(U->cara) >= g.leaders[size_t(who)].heroes.size()) return;
	Leader& L = g.leaders[size_t(who)];
	int32_t n = (get_general_upgrade(g, who) + 2) * 5;
	if (unit_is(g, *U, hero::kPorus)) {
		n = RON_K(g, "porus_decoys") * n;
		n = (n + ((n >> 31) & 0xff)) >> 8;
	}
	if (unit_is(g, *U, hero::kKutusov)) n = (RON_K(g, "kutosov_decoys") * n) / 100;
	const int32_t mark = g.objects[size_t(who)].unit_mark;
	int32_t existing = 0;
	for (int32_t i = 0; i < mark; i++) {
		const Unit* u = g.unit(who, i);
		if (u && u->alive() && u->o_up < 0 && (u->unit_masks & umask::kDecoy)) existing++;
	}
	const int32_t army = (U->unit_masks & umask::kAiControl) ? object_get_army(g, *U) : -1;
	const int32_t radius = hero_get_radius(g, L.heroes[size_t(U->cara)]);
	bool made = false;
	for (int32_t o2 = mark - 1; o2 >= 0; o2--) {
		Unit* V = g.unit(who, o2);
		if (!V || !V->alive() || V->o_up >= 0 || (V->unit_masks & umask::kDecoy) || V->ptype->domain != 0) continue;
		if (obj_attack(g, *V) == 0) continue;  // vslot 0x120 (attack)
		if (V->type >= 50 && V->type <= 53) continue;
		if (unit_is_caravan(*V) || unit_is(g, *V, 0x3d)) continue;
		if (o2 == o) continue;
		if (!(vector_dist(U->x - V->x, U->y - V->y) < radius * 0xc0)) continue;
		int32_t sx = 0, sy = 0;
		if (find_nearby_spot(g, *U->ptype, U->x, U->y, &sx, &sy, 0x180, -1, 0, 0, kFilterNotMe, U->o, U->who, 0, 0, -1, 0, -1) != 0) break;
		const int32_t vtype = V->type;
		const int32_t d = init_unit(g, who, vtype, sx, sy, -1, -1, -1);
		if (d < 0) break;
		if (army >= 0) army_add_unit(g, who, army, d);
		made = true;
		Unit* D = g.unit(who, d);
		if (D->ptype->control_cost != 0) {  // a decoy costs no population: undo init_unit's counters
			track_unit_type(g, who, vtype, -1, d);
			L.control -= (D->unit_masks & umask::kDecoy) ? 0 : D->ptype->control_cost;
			L.active--;
			L.units_built--;
		}
		for (int32_t c = d; c >= 0;) {
			Unit* C = g.unit(who, c);
			if (!C) break;
			C->unit_masks |= umask::kDecoy;
			C->mana_burn = 0;
			unit_update_hits(g, *C);
			unit_update_los(g, *C);
			update_speed(g, *C);
			unit_update_armor(g, *C);
			c = C->o_down;
		}
		existing++;
		if (existing >= n) break;
	}
	if (!made) {  // nothing created: the cast is refunded
		unpay_cast_costs(g, 0x27a, o, who);
	}
	// (sound, console only)
}

}  // namespace

// ---------------------------------------------------------------- the dispatch (4.5)

void spell_cast(Game& g, int32_t sp, int32_t o, int32_t who, int32_t p3, int32_t p4) {
	// @676ce0
	if (spell_is_castable(g, sp, o, who, 1) != 3) {
		if (getenv("RON_HERO_TRACE")) fprintf(stderr, "[spell] f%d cast %x by %d:%d not castable (%d)\n", g.frame, sp, who, o, spell_is_castable(g, sp, o, who, 1));
		return;
	}
	if (getenv("RON_HERO_TRACE")) fprintf(stderr, "[spell] f%d cast %x by %d:%d\n", g.frame, sp, who, o);
	switch (sp) {
	case 0x27a:
		cast_create_decoy(g, o, who);
		return;
	case 0x27b:
		cast_ambush(g, o, who);
		return;
	case 0x27c:
		cast_march(g, o, who);
		return;
	case 0x27d:
		cast_entrench(g, o, who);
		return;
	case 0x27e:
		cast_supplies(g, o, who);
		return;
	case spell::kTransport:
		cast_transport(g, o, who);
		return;
	case 0x275:
		cast_bribe(g, sp, o, who, p3, p4);
		return;
	case 0x277:
		if (spell_is_valid_target(g, sp, who, p3, p4, nullptr)) cast_counterintel(g, o, who, p3, p4);
		return;
	case 0x27f:
		cast_double_agent(g, o, who, p3, p4);
		return;
	case 0x280:
	case 0x281:
		cast_sabotage(g, sp, o, who, p3, p4);
		return;
	case 0x284:  // PARADROP (aircraft-spec 9): the target point is (p3, p4)
		cast_paradrop(g, o, who, p3, p4);
		return;
	case 0x285:  // RESEARCH PROJECT (dead): +50 knowledge
		g.leaders[size_t(who)].bucket[ptype::kKnowledge] += 50;
		return;
	case 0x286:
		cast_depopulate(g, sp, o, who);
		return;
	case 0x287:
		cast_move_capital(g, o, who);
		return;
	case 0x28b: case 0x28d: case 0x28f: case 0x291:
		cast_pack(g, o, who);
		return;
	case 0x28c: case 0x28e: case 0x290: case 0x292:
		cast_unpack(g, o, who);
		return;
	case 0x293:
		cast_civilian(g, o, who);
		return;
	case 0x294:
		cast_to_arms(g, o, who);
		return;
	case 0x297:
	case 0x298:
		cast_immortal(g, sp, o, who);
		return;
	case 0x299:  // radar UPGRADE (dead): the building takes the current AIRDEFENSE type
		if (Build* b = g.build(who, o)) wall_set_type(g, *b, current_upgrade(g, who, ptype::kAirDefense), 0);
		return;
	case 0x29a:
		cast_disband(g, o, who);
		return;
	case 0x282:  // BLOW TREAD (dead): a spared unit takes one hit point of damage
		if (Unit* t = g.unit(p4, p3)) {
			if (t->damage == 0) t->damage = 1;
		}
		return;
	default:
		// Dead crafts without a cast body: PILFER 0x276, ASSIMILATE 0x278, RALLY 0x279, JAM RADAR 0x283, LOCK / UNLOCK GATE 0x288 / 9 (never castable). TODO(spells): their bodies (spells-spec 4.6).
		return;
	}
}

bool leader_has_spell(const Game& g, int32_t who, int32_t sp) {
	if (sp < 0 || sp >= kNumTypes) return false;
	if (is_spell_type(sp) && (g.type(sp).spell_flags & 1)) return g.leaders[size_t(who)].has_tech_bit(sp) && type_avail(g, who, sp, 1) != 0;
	return type_avail(g, who, sp, 1) != 0;
}

int32_t scenario_cast_spell(Game& g, int32_t who, int32_t unit_o, int32_t ox, int32_t whom, int32_t x, int32_t y, int32_t sp) {
	if (who < 0 || who >= 8 || !(g.leaders[size_t(who)].flags & leaderflag::kValid)) return -1;
	Unit* u = g.unit(who, unit_o);
	if (!u || !u->alive()) return -1;
	if (!is_spell_type(sp) || !leader_has_spell(g, who, sp)) return -1;
	if (spell_is_castable(g, sp, unit_o, who, 0) != 3) return -1;
	add_cast_order(g, *u, ox, whom, x, y, sp, kQueueNew, false);
	return 1;
}

// ---------------------------------------------------------------- issuing (spells-spec 3.4)

namespace {

// GroupOut::validate_spell @7095e0, simulation side. The function is the UI's pre-flight: every rejection it makes either
// repeats a test the per-member loop of Group::action_spell makes again (is_valid_target: own object, allies, the
// vehicle / building flags, the infiltrated test of INFORMER) or only picks the feedback message (the "no one can cast"
// counters). ❓ spells-spec 11 (6): the ENTRENCH / PARADROP per-member land checks are not reproduced.
bool validate_spell(const Game& g, const Group& gr, int32_t sp, int32_t ox, int32_t whom) {
	(void)g;
	(void)gr;
	(void)sp;
	(void)ox;
	(void)whom;
	return false;
}

}  // namespace

void group_action_spell(Game& g, Group& gr, int32_t sp, int32_t ox, int32_t whom, int32_t x, int32_t y) {
	// @6fe1a0
	gr.disband = 0;  // action_begin (vslot 0x14)
	if (validate_spell(g, gr, sp, ox, whom)) return;
	gr.form = -1;  // group +0x10
	if (sp < 0 || sp >= kNumTypes) return;
	const bool is_spell = is_spell_type(sp);
	const int32_t who = gr.who;
	const Type& PT = g.type(sp);
	// "Only one caster" (spell flag 0x1000, not with the "use craft as group" option): the member with the most mana left.
	int32_t best = -1, best_mana = 0;
	if (is_spell && !(g.leaders[size_t(who)].options_flags & 1) && (PT.spell_flags & 0x1000)) {
		for (int32_t i = 0; i < gr.num; i++) {
			const Object* ob = g.object(who, gr.list[size_t(i)]);
			if (!ob || !ob->alive() || !ob->is_unit || !static_cast<const Unit*>(ob)->on_map()) continue;
			const Unit& m = *static_cast<const Unit*>(ob);
			if (unit_is_casting_spell(m, sp)) continue;
			if (spell_is_castable(g, sp, m.o, who, 0) != 3) continue;
			const int32_t left = unit_mana_left(g, m);  // UnitData::mana_left (aircraft.hpp)
			if (best_mana < left) {
				best_mana = left;
				best = i;
			}
		}
	}
	if (is_unit_type(sp) && g.type(sp).domain == 2) {  // an aircraft type as the "spell": BUILDAIR (aircraft-spec 7)
		group_action_buildair(g, gr);
		return;
	}
	if (sp == spell::kTransport) {
		for (int32_t i = 0; i < gr.num; i++) {
			Unit* m = g.unit(who, gr.list[size_t(i)]);
			if (!m || !m->alive() || !m->on_map()) continue;
			if (m->ptype->domain == kDomainSea || m->ptype->domain == kDomainAir || cast_is_busy(*m)) continue;
			if (spell_is_castable(g, spell::kTransport, m->o, who, 0) != 3 || !leader_has_spell(g, who, spell::kTransport)) continue;
			const Type& T = g.type(spell::kTransport);
			if (T.mana != 0 && unit_mana_available(g, *m) < T.mana) continue;
			if (can_pay_cost(g, spell::kTransport, who, -1, m->o, 1) == 0) continue;
			int32_t sx = 0, sy = 0;
			if (find_nearby_spot(g, g.type(ptype::kTransportBarge), m->x, m->y, &sx, &sy, 0, K("unit_board_distance"), 0, 0x55555555,
			                     kFilterNotMe, -1, -1, 0, 0, -1, 0, -1) != 0)
				continue;  // no water spot
			if (m->order_type() == kCastSpell && m->orders.front().spell == spell::kTransport) continue;  // already casting it
			m->spell_time = 0;
			add_cast_order(g, *m, ox, whom, x, y, spell::kTransport, kQueueNew, true);
		}
		return;
	}
	const int32_t eff = is_spell ? sp : spell::kDefault;  // local_34
	const Type& S = g.type(eff);
	for (int32_t i = 0; i < gr.num; i++) {
		const int32_t mo = gr.list[size_t(i)];
		Object* ob = g.object(who, mo);
		if (!ob || !ob->alive()) continue;
		if (ob->is_unit && !static_cast<Unit*>(ob)->on_map()) continue;
		if (eff != spell::kDisband && eff != 0x286 && spell_is_castable(g, eff, mo, who, 0) == 0) {
			SPT("f%d %d:%d not castable 0x%x", g.frame, who, mo, unsigned(eff));
			continue;
		}
		if (best >= 0 && best != i) continue;
		if (ob->is_unit) {
			Unit& m = *static_cast<Unit*>(ob);
			if (S.mana != 0 && unit_mana_available(g, m) < S.mana) {  // "not enough craft"
				SPT("f%d %d:%d mana %d < %d", g.frame, who, mo, unit_mana_available(g, m), S.mana);
				continue;
			}
		}
		if (can_pay_cost(g, sp, who, -1, mo, 1) == 0) {
			SPT("f%d %d:%d cannot pay 0x%x", g.frame, who, mo, unsigned(sp));
			continue;
		}
		int32_t why = 0;
		if (!spell_is_valid_target(g, eff, who, ox, whom, &why)) {
			SPT("f%d %d:%d invalid target %d:%d (reason %d)", g.frame, who, mo, whom, ox, why);
			continue;
		}
		// (the wrong packed state and "already casting" tests of the original only count for the feedback message)
		if (ob->is_unit) {
			Unit& m = *static_cast<Unit*>(ob);
			if (cast_is_busy(m)) continue;
			if (ox != m.cavarch_o || whom != m.cavarch_who) {
				m.spell_time = 0;
				m.cavarch_o = int16_t(ox);
				m.cavarch_who = int8_t(whom);
				if (ox >= 0) {
					if (const Object* t = g.object(whom, ox)) m.cavarch_uid = t->uid;
				}
			}
			QueuePos q = kQueueNew;
			if (S.spell_flags & 0x40) {
				while (m.order_type() == kCastSpell) kill_current_order(g, m, false);
				q = kQueueFirst;
			}
			add_cast_order(g, m, ox, whom, x, y, sp, q, true);
		} else {
			Build& b = static_cast<Build&>(*ob);
			if (spell_job_time(g, sp, mo, who) == 0) {
				pay_cast_costs(g, sp, mo, who);
				spell_cast(g, sp, mo, who, 0, 0);
			} else {
				build_queue_up(g, b, sp, 1);
			}
		}
	}
}

}  // namespace ron::sim
