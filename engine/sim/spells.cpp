// Spell casting (docs/transport-spec.md section 4): Unit::add_cast_order, Unit::do_cast (untargeted branch),
// SpellTypeData::is_castable / get_job_time, SpellType::pay_cast_costs / unpay_cast_costs and the SpellType::cast
// dispatch. The TRANSPORT spell (0x28a) and the five general powers (heroes-spec 4: DECOY 0x27a, AMBUSH 0x27b, FORCED
// MARCH 0x27c, ENTRENCH 0x27d, SUPPLIES 0x27e) are simulated; the rest is TODO(spells): bribe, pilfer, assimilate,
// pack / unpack, sabotage, depopulate, move capital, gates, civilian / to arms, disband, ...
#include "sim/spells.hpp"
#include <cstdio>
#include <cstdlib>

#include "sim/ai.hpp"
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
		if (!ob->is_unit || !ob->alive() || !static_cast<const Unit&>(*ob).on_map()) return 0;  // is_map_unit
		const int32_t wx = wcoord_of(ob->x), wy = wcoord_of(ob->y), tx = tcoord_of(ob->x), ty = tcoord_of(ob->y);
		if (!g.world.in_t(tx, ty) || !(g.world.t(tx, ty) & tmask::kCity) || !g.world.in_w(wx, wy) || g.world.w(wx, wy).who != who)
			return 2;
		const int32_t c = find_city_at(g, ob->x, ob->y, who, -1, 0);
		if (c < 0 || g.cities[size_t(who)][size_t(c)].race != who) return 2;
		return 3;
	}
	case spell::kTransport:
		return object_is_cargo(g, *ob) ? 3 : 0;
	case 0x27a:
	case 0x27b:
	case 0x27c:
	case 0x27d:
		return 3;  // no case in the switch: castable once the owner chain passed
	default:
		// TODO(spells): DISBAND (researching, is_building, city disbanding checks), pack / unpack, the spies' spells ...
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
	(void)p3;
	(void)p4;
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
	default:
		// TODO(spells): cast_bribe .. cast_disband.
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

// UnitData::is_casting(spell): the current order is a CAST_SPELL for it.
bool unit_is_casting(const Unit& u, int32_t sp) { return u.order_type() == kCastSpell && u.orders.front().spell == sp; }

// UnitData::mana_left: the unmet mana of the unit, with the mana of the cast it is running given back.
int32_t unit_mana_left(const Game& g, const Unit& u) {
	int32_t running = 0;
	if (u.order_type() == kCastSpell) running = g.type(u.orders.front().spell).mana;
	const int32_t m = unit_mana(g, u);
	int32_t left = std::max(0, m - u.mana_burn);
	left += running;
	return left < m ? left : m;
}

}  // namespace

void group_action_spell(Game& g, Group& gr, int32_t sp, int32_t ox, int32_t whom, int32_t x, int32_t y) {
	if (sp < 0 || sp >= kNumTypes) return;
	const bool is_spell = is_spell_type(sp);
	const int32_t eff = is_spell ? sp : spell::kDefault;
	if (!is_spell || sp == spell::kTransport) return;  // TODO(spells): upgrade-by-cast, BUILDAIR, TRANSPORT
	const Type& S = g.type(eff);
	if (S.spell_flags & 0xe) return;  // TODO(spells): targeted spells (is_valid_target, range)
	const int32_t who = gr.who;
	// "Only one caster" (spell flag 0x1000): the member with the most mana left casts.
	int32_t best = -1, best_mana = 0;
	if ((S.spell_flags & 0x1000) && !(g.leaders[size_t(who)].options_flags & 1)) {
		for (int32_t i = 0; i < gr.num; i++) {
			const Unit* m = g.unit(who, gr.list[size_t(i)]);
			if (!m || !m->alive() || !m->on_map() || unit_is_casting(*m, sp)) continue;
			if (spell_is_castable(g, sp, m->o, who, 0) != 3) continue;
			const int32_t left = unit_mana_left(g, *m);
			if (best_mana < left) {
				best_mana = left;
				best = i;
			}
		}
	}
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* m = g.unit(who, gr.list[size_t(i)]);
		if (!m || !m->alive() || !m->on_map()) continue;
		if (eff != spell::kDisband && eff != 0x286 && spell_is_castable(g, eff, m->o, who, 0) == 0) continue;
		if (best >= 0 && best != i) continue;
		if (S.mana != 0 && unit_mana_left(g, *m) < S.mana) continue;  // "not enough craft"
		if (can_pay_cost(g, eff, who, -1, m->o, 1) == 0) continue;
		if (unit_is_casting(*m, sp)) continue;
		// (busy test: UnitData::is_busy, not ported)
		if (ox != m->cavarch_o || whom != m->cavarch_who) {
			m->spell_time = 0;
			m->cavarch_o = int16_t(ox);
			m->cavarch_who = int8_t(whom);
			if (ox >= 0) {
				if (const Object* t = g.object(whom, ox)) m->cavarch_uid = t->uid;
			}
		}
		QueuePos q = kQueueNew;
		if (S.spell_flags & 0x40) {
			while (m->order_type() == kCastSpell) kill_current_order(g, *m, false);
			q = kQueueFirst;
		}
		add_cast_order(g, *m, ox, whom, x, y, sp, q, true);
	}
}

}  // namespace ron::sim
