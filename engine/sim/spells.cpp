// Spell casting (docs/transport-spec.md section 4): Unit::add_cast_order, Unit::do_cast (untargeted branch),
// SpellTypeData::is_castable / get_job_time, SpellType::pay_cast_costs / unpay_cast_costs and the SpellType::cast
// dispatch. Everything but the TRANSPORT spell (0x28a) is TODO(spells): bribe, pilfer, assimilate, decoys, entrench,
// pack / unpack, sabotage, depopulate, move capital, gates, civilian / to arms, disband, ...
#include "sim/spells.hpp"

#include "sim/combat_internal.hpp"
#include "sim/game.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/production.hpp"
#include "sim/production_internal.hpp"
#include "sim/search.hpp"
#include "sim/transport.hpp"

namespace ron::sim {

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
	return add_order(g, u, c, q);
}

// ---------------------------------------------------------------- castability (4.2)

int32_t spell_is_castable(const Game& g, int32_t sp, int32_t o, int32_t who, int32_t p3) {
	(void)p3;
	const Object* ob = who >= 0 && who < kNumLeaders && o >= 0 ? g.object(who, o) : nullptr;
	if (!ob) return 0;
	// The "does the leader own the spell" chain (is(spell), is(from2), tribe_can_type / graft) is skipped for TRANSPORT
	// and DISBAND. TODO(spells): for every other spell it decides, and each spell has its own switch case below.
	if (sp != spell::kTransport && sp != spell::kDisband) return 0;
	// Decoys cast nothing but pack / unpack. (The spec text says 1; the decompile returns 0. Both are != 3.)
	if (ob->is_unit && (static_cast<const Unit&>(*ob).unit_masks & umask::kDecoy) && !spell_is_pack(sp) && !spell_is_unpack(sp))
		return 0;
	switch (sp) {
	case spell::kTransport:
		return object_is_cargo(g, *ob) ? 3 : 0;
	default:
		// TODO(spells): DISBAND (researching, is_building, city disbanding checks) and the other spells.
		return 0;
	}
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
			// ❓ the decompile drops the result of is(0x169) (Antipater); the rate is a /256 fraction (256 = unchanged),
			// so it can only be meant for Antipater's own units.
			if (unit_is(g, *u, 0x169)) {
				t = RON_K(g, "antipater_entrench_rate") * t;
				t = (t + ((t >> 31) & 0xff)) >> 8;
			}
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

int32_t do_cast(Game& g, Unit& u, Order& ord) {
	// @5ebfe0
	const int32_t sp = ord.spell;
	const bool is_spell = is_spell_type(sp);
	const int32_t eff = is_spell ? sp : spell::kDefault;
	if (!is_spell) {
		// TODO(spells): an order whose "spell" is a unit type (upgrade by casting: set_type(current_upgrade) and a walk
		// into a building at the unit's tile). Never created.
		kill_current_order(g, u, false);
		return 0;
	}
	if (eff != spell::kDefault && (g.type(eff).spell_flags & 0xe) != 0) {
		// TODO(spells): the targeted branch (range walk, facing, animations, cloak, job time, SpellType::cast with the
		// target). No targeted spell is created yet.
		kill_current_order(g, u, false);
		return 0;
	}
	if (ord.paid == 0) {
		if (pay_cast_costs(g, sp, u.o, u.who) != 0) {
			// (console: S_INVALID_ORDER and a red feedback line)
			kill_current_order(g, u, false);
			return 0;
		}
		ord.paid = 1;
	}
	if (u.spell_time == 0) {  // the first frame of the cast
		if (spell_is_pack(sp) || spell_is_unpack(sp)) {
			// TODO(spells): CHAR_PACK / CHAR_UNPACK, the merchants' gpiece, the rare collector's merchant spot.
			kill_current_order(g, u, false);
			return 0;
		}
		int32_t anim = kCharDefault, p3 = 1;
		if (u.ptype->unit_flags2 & utflag2::kHero) {  // UnitData::is_hero
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
	// (pack: a general with the right power nearby speeds the pack up: TODO(spells))
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
	spell_cast(g, sp, u.o, u.who, 0, 0);
	if (sp == spell::kTransport) return 0;  // the order now lives on the barge (cast_transport)
	ord.paid = 0;
	kill_current_order(g, u, false);
	return 0;
}

// ---------------------------------------------------------------- the dispatch (4.5)

void spell_cast(Game& g, int32_t sp, int32_t o, int32_t who, int32_t p3, int32_t p4) {
	// @676ce0
	(void)p3;
	(void)p4;
	if (spell_is_castable(g, sp, o, who, 1) != 3) return;
	switch (sp) {
	case spell::kTransport:
		cast_transport(g, o, who);
		return;
	default:
		// TODO(spells): cast_bribe .. cast_disband.
		return;
	}
}

}  // namespace ron::sim
