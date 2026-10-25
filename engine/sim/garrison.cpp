// Garrisons (docs/garrison-spec.md): the container links, num_inside / count_inside, UnitTypeData::can_garrison, the
// GARRISON order (add_garrison_order, kill_garrison_order, do_garrison), Group::action_garrison / action_eject_all,
// Object::eject_contents / kill_contents, Build::process_ejection and the garrisoned branch of Unit::process.
#include "sim/garrison.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#include "sim/aircraft.hpp"
#include "sim/ai.hpp"
#include "sim/combat_internal.hpp"
#include "sim/compass.hpp"
#include "sim/fog.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/production.hpp"
#include "sim/search.hpp"
#include "sim/spells.hpp"
#include "sim/transport.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace combat_detail;

namespace {

// TypeIndex values this module tests (garrison-spec 3).
constexpr int32_t kTypeBarracks = 0x1ab, kTypeStable = 0x1ac, kTypeAutoPlant = 0x1ad, kTypeSiegeFactory = 0x1ae,
                  kTypeFactory = 0x1af, kTypeDock = 0x1b0, kTypeTower = 0x1b7, kTypeFort = 0x1bb, kTypeVillage = 0x19e,
                  kTypeUniversity = 0x1a4, kTypeAirbase = 0x1bf, kTypeCarrier = 0x15f, kTypeSilo = 0x208,
                  kTypeMilitia = 0x42, kTypeBarge = 0x140, kTypeMerchantFleet = 0x13e;
constexpr int32_t kTribeLakota = 19, kTribeAmerican = 20;

bool is_peasant_t(int32_t t) { return t == ptype::kPeasants || t == ptype::kPeasantsKorean; }
bool is_scholar_t(int32_t t) { return t == ptype::kScholars || t == ptype::kScholarsKorean; }
bool is_decoy(const Unit& u) { return (u.unit_masks & umask::kDecoy) != 0; }

const Unit* chain_unit(const Object& o) { return o.is_unit ? static_cast<const Unit*>(&o) : nullptr; }

// The cell owner (WData.who) of the W cell under (x, y); -1 neutral.
int32_t cell_owner_at(const Game& g, int32_t x, int32_t y) {
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	return g.world.in_w(wx, wy) ? int32_t(g.world.w(wx, wy).who) : -1;
}

// The captain of u's uber chain (u itself when it is one).
Unit* captain_of(Game& g, Unit& u) {
	Unit* c = &u;
	for (int guard = 0; c->o_up >= 0 && guard < 64; guard++) {
		Unit* p = g.unit(c->who, c->o_up);
		if (!p) break;
		c = p;
	}
	return c;
}

// The militia type M of a leader (garrison-spec 2): the militia itself when the tribe has it, else its substitute.
int32_t militia_type(const Game& g, int32_t who) { return get_graft(g, who, kTypeMilitia); }

// The militia line's contribution to the garrison arrows: (attack of the current upgrade / 10 + 2) / 3.
int32_t militia_arrows(const Game& g, int32_t who, int32_t M) {
	const int32_t cur = current_upgrade(g, who, M);
	if (cur < 0) return 0;
	return (g.type(cur).attack / 10 + 2) / 3;
}

}  // namespace

// ---------------------------------------------------------------- the container links (1)

int32_t unit_get_inside(const Game& g, const Unit& u, int32_t* who_out) {
	if (who_out) *who_out = -1;
	if (u.inside_up < 0) return -1;
	int32_t o = u.inside_up, w = u.inside_up_who;
	for (int guard = 0; guard < 64; guard++) {
		const Object* ob = g.object(w, o);
		if (!ob || !ob->is_unit) break;
		const Unit& c = static_cast<const Unit&>(*ob);
		if (c.inside_up < 0) break;
		w = c.inside_up_who;
		o = c.inside_up;
	}
	if (who_out) *who_out = w;
	return o;
}

namespace {

// The "inside" counters of insert_inside / remove_from_inside (garrison-spec 1): a captain that is not a decoy, in
// a building. d = +1 / -1. Lakota (tribe bonus 19) and Americans (20, barracks only) recompute their gather rates.
void inside_counters(Game& g, const Unit& u, int32_t d) {
	if (u.o_up >= 0 || is_decoy(u) || u.who < 0 || u.who >= kNumLeaders) return;
	Leader& l = g.leaders[size_t(u.who)];
	const Type& t = *u.ptype;
	bool lakota = false, american = false;
	if (is_peasant_t(u.type)) {
		l.peasants_inside += d;
		lakota = true;
	} else if (t.where == kTypeStable) {
		l.stable_inside += d;
		lakota = true;
	} else if (t.unit_flags2 & utflag2::kSpecial) {
		l.special_inside += d;
		lakota = true;
	} else if (t.where == kTypeBarracks) {
		l.barracks_inside += d;
		american = true;
	}
	if (u.who >= 8) return;
	if (lakota && has_tribe_bonus(g, u.who, kTribeLakota) && RON_K(g, "lakota_food") != 0) l.flags |= leaderflag::kRecompute;
	if (american && has_tribe_bonus(g, u.who, kTribeAmerican) && RON_K(g, "americans_barracks_gather") != 0)
		l.flags |= leaderflag::kRecompute;
}

bool container_is_building(const Game& g, const Unit& u) {
	int32_t xw;
	const int32_t x = unit_get_inside(g, u, &xw);
	const Object* c = x >= 0 ? g.object(xw, x) : nullptr;
	return c && !c->is_unit;
}

}  // namespace

void unit_insert_inside(Game& g, Unit& u, int32_t o, int32_t who) {
	Object* c = g.object(who, o);
	if (!c || !c->alive() || !u.alive()) return;
	// ObjectData::inside_bottom: the last object of the container's chain (the container itself when empty).
	Object* bottom = c;
	for (int guard = 0; bottom->inside_down >= 0 && guard < 1000; guard++) {
		Object* n = g.object(bottom->inside_down_who, bottom->inside_down);
		if (!n) break;
		bottom = n;
	}
	remove_from_world(g, u);
	bottom->inside_down = u.o;
	bottom->inside_down_who = u.who;
	u.inside_up = bottom->o;
	u.inside_up_who = bottom->who;
	if (u.o_up < 0 && !is_decoy(u) && container_is_building(g, u)) inside_counters(g, u, 1);
}

void unit_remove_from_inside(Game& g, Unit& u) {
	if (u.inside_up < 0) return;
	const bool in_bld = container_is_building(g, u);
	Object* up = g.object(u.inside_up_who, u.inside_up);
	if (up) {
		up->inside_down = u.inside_down;
		up->inside_down_who = u.inside_down_who;
	}
	if (u.inside_down >= 0) {
		if (Unit* d = g.unit(u.inside_down_who, u.inside_down)) {
			d->inside_up = u.inside_up;
			d->inside_up_who = u.inside_up_who;
		}
	}
	u.inside_up = -1;
	u.inside_down = -1;
	add_to_world(g, u);
	if (in_bld) inside_counters(g, u, -1);
}

// ---------------------------------------------------------------- counting (2)

int32_t unit_control_cost(const Unit& u) { return is_decoy(u) ? 0 : u.ptype->control_cost; }

int32_t num_inside(const Game& g, const Object& ob, int32_t p) {
	if (!on_map(ob)) return 0;
	int32_t n = 0;
	int32_t o = ob.inside_down, w = ob.inside_down_who;
	for (int guard = 0; o >= 0 && guard < kUnitLimit; guard++) {
		const Object* c = g.object(w, o);
		if (!c) break;
		const Unit* u = chain_unit(*c);
		if (u && u->alive() && u->o_up < 0) {
			if (p != 0) n += 1;
			else if (is_gov_hero_type(u->type)) n += 1;
			else n += unit_control_cost(*u);
		}
		o = c->inside_down;
		w = c->inside_down_who;
	}
	return n;
}

int32_t count_inside(const Game& g, const Object& ob, int32_t kind, int32_t t) {
	int32_t n = 0;
	int32_t o = ob.inside_down, w = ob.inside_down_who;
	for (int guard = 0; o >= 0 && guard < kUnitLimit; guard++) {
		const Object* c = g.object(w, o);
		if (!c) break;
		o = c->inside_down;
		w = c->inside_down_who;
		if (!c->alive() || !c->ptype) continue;
		const Unit* u = chain_unit(*c);
		const Type& ty = *c->ptype;
		const int32_t ct = c->type;
		switch (kind) {
		case kCountWorkers:
			if (is_worker_type(ct)) n++;
			break;
		case kCountPeasants:
			if (is_peasant_t(ct)) n++;
			break;
		case kCountAttack:
			if (u && ty.attack != 0 && u->o_up < 0) n++;
			break;
		case kCountMilitary:
			// "alive with objmask 4" (garrison-spec 2): taken literally (OBJMASK_CIVILIAN); no caller uses it yet.
			if (u && (uint32_t(ty.obj_masks) & 4u)) n++;
			break;
		case kCountDomain:
			if (ty.domain == t) n++;
			break;
		case kCountOnMap:
			if (u && u->on_map()) n++;
			break;
		case kCountGatherPeasants:
			if (u && is_peasant_t(ct) && action_type(*u) == kGather) n++;
			break;
		case kCountGatherWorkers:
			if (u && is_worker_type(ct) && action_type(*u) == kGather) n++;
			break;
		case kCountGatherScholars:
			if (u && is_scholar_t(ct) && action_type(*u) == kGather) n++;
			break;
		case kCountBuildPeasants:
			if (u && is_peasant_t(ct) && action_type(*u) == kBuildAt) n++;
			break;
		case kCountStrictType:
			if (type_is(ty, t, true)) n++;
			break;
		case kCountType:
			if (type_is(ty, t, false)) n++;
			break;
		case kCountTypeSameWho:
			if (type_is(ty, t, false) && c->who == ob.who) n++;
			break;
		case kCountNonDecoyType:
			if (u && !is_decoy(*u) && type_is(ty, t, false)) n++;
			break;
		case kCountCategory:
			if (ty.cat == t) n++;
			break;
		case kCountRegion:
			if (u && u->on_map()) {
				const int32_t tx = tcoord_of(u->x), ty2 = tcoord_of(u->y);
				if (g.world.in_t(tx, ty2) && g.world.get_tregion(tx, ty2) == t) n++;
			}
			break;
		case kCountTrade:
			if ((ty.unit_flags2 & utflag2::kCaravan) || type_is(ty, kTypeMerchantFleet)) n++;
			break;
		case kCountWhoInside:
			if (c->who == t) n++;
			break;
		case kCountGarrisonArrows: {
			// Captains that are not decoys, with OBJMASK_FOOT, not siege.
			if (!u || u->o_up >= 0 || is_decoy(*u) || !(uint32_t(ty.obj_masks) & objmask::kFoot) || type_is_siege(ty)) break;
			const int32_t who = u->who;
			const int32_t M = who >= 0 && who < kNumLeaders ? militia_type(g, who) : -1;
			if (is_worker_type(ct) && M >= 0 && has_tech(g, who, M)) {
				n += militia_arrows(g, who, M);
			} else if (!type_is(ty, kTypeMilitia)) {
				int32_t v = obj_attack(g, *u) / 10;
				if (ty.max_range == 0) v = (v + 1) / 2;
				n += v;
			} else if (M >= 0) {
				n += militia_arrows(g, who, M);
			}
			break;
		}
		default:
			break;
		}
	}
	return n;
}

bool can_carry_domain(const Game& g, const Object& ob, int32_t domain) {
	(void)g;
	if (!ob.ptype) return false;
	const Type& t = *ob.ptype;
	if (domain == kDomainGround && ob.is_unit && t.carry != 0 && !type_is(t, kTypeCarrier)) return true;
	return type_is(t, kTypeAirbase, true) || type_is(t, kTypeCarrier, true) || type_is(t, kTypeSilo, true);
}

// ---------------------------------------------------------------- eligibility (3)

bool type_can_garrison(const Game& g, const Type& ut, int32_t bt) {
	if (ut.domain == kDomainSea && ((ut.unit_flags & 0x10) || type_is(ut, kTypeCarrier, true))) return false;
	const Type* B = bt >= 0 ? &g.type(bt) : nullptr;
	if (ut.unit_flags & 0x1800) {  // 'l' garrison town / tower, 'm' garrison fort
		if (bt < 0) return true;
		if (type_is(*B, kTypeFort) || type_is(*B, kTypeTower) || type_is(*B, kTypeVillage)) return true;
	}
	if (bt < 0) return false;
	if (ut.domain == kDomainSea) return type_is(*B, kTypeDock);
	const int32_t where = ut.where;
	if (bt == where && bt != kTypeUniversity) return true;
	auto trainer = [&](int32_t b) { return b >= kTypeBarracks && b <= kTypeFactory; };
	if (where == kTypeBarracks) return trainer(bt);
	if (where == kTypeStable || where == kTypeAutoPlant) return trainer(bt);
	if (where == kTypeSiegeFactory || where == kTypeFactory)
		return trainer(bt) || type_is(*B, kTypeFort) || type_is(*B, kTypeVillage);
	return false;
}

bool unit_can_garrison(const Game& g, const Unit& u, int32_t o, int32_t who) {
	const Object* t = g.object(who, o);
	if (!t || !t->alive() || t->is_unit || !(t->flags & objflag::kActive)) return false;
	return type_can_garrison(g, *u.ptype, t->type);
}

int32_t find_garrison_build(const Game& g, const Unit& u, int32_t city, int32_t who) {
	if (who < 0 || who >= 8 || city < 0 || size_t(city) >= g.cities[size_t(who)].size()) return -1;
	const City& C = g.cities[size_t(who)][size_t(city)];
	if (!C.valid()) return -1;
	int32_t best = -1, best_d = 0x7fffffff;
	int32_t o = C.o;
	for (int guard = 0; o >= 0 && guard < kBuildLimit; guard++) {
		const Build* b = g.build(who, o);
		if (!b) break;
		const int32_t next = b->city_down;
		if (b->alive() && (b->flags & objflag::kActive) &&
		    num_inside(g, *b, 0) + unit_control_cost(u) <= get_garrison_limit(g, b->type, who) &&
		    type_can_garrison(g, *u.ptype, b->type)) {
			const int32_t d = vector_dist(b->x - u.x, b->y - u.y);
			if (best < 0 || d < best_d) {
				best = o;
				best_d = d;
			}
		}
		o = next;
	}
	return best;
}

bool object_adjacent_to(const Game& g, const Object& s, int32_t ox, int32_t whom) {
	if (s.ptype->domain == kDomainSea && !(s.ptype->unit_flags & 0x10)) {
		int32_t lim = RON_K(g, "boat_garrison_max_distance");
		if (s.ptype->new_block_radius < 4) lim += 0x180;
		return attack_dist(g, s, ox, whom) < lim;
	}
	return attack_dist(g, s, ox, whom, s.x, s.y) < 0x60;
}

// ---------------------------------------------------------------- the order (4)

Order* add_garrison_order(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t find, QueuePos q, bool expl) {
	if (q == kQueueNew) clear_orders(g, u);
	Order o;
	o.type = kGarrison;
	o.ox = ox;
	o.whom = whom;
	const Object* tg = g.object(whom, ox);
	o.uid = tg ? tg->uid : 0xffff;
	o.find = int8_t(find);
	if (expl) o.flags |= orderflag::kExplicit;
	return add_order(g, u, o, q == kQueueNew ? kQueueLast : q);
}

void kill_garrison_order(Game& g, Unit& u, int32_t p) {
	(void)p;
	Unit* m = &u;
	for (int guard = 0; m && guard < kUnitLimit; guard++) {
		if (action_type(*m) == kGarrison) {
			repath(g, *m);
			kill_current_order(g, *m, false);
		}
		if (m->o_down < 0) break;
		Unit* d = g.unit(m->who, m->o_down);
		if (!d || !d->alive()) break;
		m = d;
	}
}

int32_t do_garrison(Game& g, Unit& u, Order& ord) {
	// @5e6b80. The order is read first: killing it invalidates `ord`.
	const int32_t ox = ord.ox, whom = ord.whom;
	const int32_t find = ord.find;
	const uint8_t oflags = ord.flags;
	Object* T = obj_at(g, ox, whom);
	// 1. A helicopter ordered onto an airbase (the spec says carrier; the disassembly of @5e6b80 tests is(0x1bf)): it lands
	// there (a STRAFE home with `returning` set) or, when the base cannot take it, walks up to it.
	if ((u.ptype->unit_flags & utflag::kHelicopter) && ox >= 0 && whom >= 0 && T && T->ptype && type_is(*T->ptype, kTypeAirbase)) {
		if (!can_carry_unit(g, *T, u.o, u.who)) {  // ("the airbase is full")
			kill_current_order(g, u, false);
			add_move_order(g, u, T->x, T->y, 1, false, kQueueFirst, false, -1, -1);
			return 0;
		}
		kill_current_order(g, u, false);
		add_strafe_order(g, u, -1, -1, ox, whom, 1, kQueueFirst, true);
		if (Order* s = update_order(u)) s->returning = 1;
		return 0;
	}
	// 2. a valid, active building of the unit's leader or a mutual ally, with a garrison limit and a type that fits.
	if (!T || !T->alive() || T->is_unit || !(T->flags & objflag::kActive) || !is_ally(g, u.who, whom) ||
	    get_garrison_limit(g, T->type, whom) <= 0 || !type_can_garrison(g, *u.ptype, T->type)) {
		kill_current_order(g, u, false);
		return 0;
	}
	Build& B = static_cast<Build&>(*T);
	// 3. walk next to the building.
	if (!object_adjacent_to(g, u, ox, whom)) {
		const Type& BT = *B.ptype;
		const int32_t r = std::max(BT.x_size, BT.y_size) * 96;
		int32_t r0 = r + 0x30, r1 = -1;
		if (type_is(BT, kTypeUniversity)) {
			r0 = r + 0x1b0;
			r1 = r + 0x330;
		}
		const uint32_t angle = find_angle(B.x - u.x, B.y - u.y);
		int32_t sx = 0, sy = 0;
		if (find_nearby_spot(g, *u.ptype, B.x, B.y, &sx, &sy, r0, r1, 0, angle, kFilterNotMe, u.o, u.who, 0, 0, -1, 0, -1) != 0 &&
		    find_nearby_spot(g, *u.ptype, B.x, B.y, &sx, &sy, r0, r1, 0, angle, kFilterNotMe, u.o, u.who, 1, 0, -1, 0, -1) != 0) {
			kill_current_order(g, u, false);
			return 0;
		}
		add_move_order(g, u, sx, sy, 1, false, kQueueFirst, false, -1, -1);
		return 0;
	}
	// 4. a city building must be assimilated to its owner and not be falling (❓ the sense of the decompile's test is
	// the one the spec gives: not assimilated, or reduced below a tenth of its hit points, refuses).
	if (B.flags & objflag::kCity) {
		bool refuse = false;
		if (whom >= 0 && whom < 8 && B.city >= 0 && size_t(B.city) < g.cities[size_t(whom)].size()) {
			const City& C = g.cities[size_t(whom)][size_t(B.city)];
			if (C.race != whom) refuse = true;
		}
		if (obj_hits(g, B) / 10 > obj_hits_left(g, B)) refuse = true;
		if (refuse) {  // (console: "cannot garrison")
			kill_current_order(g, u, false);
			return 0;
		}
	}
	// 5. room: go inside, unless the ground under the building belongs to a stranger.
	const int32_t lim = get_garrison_limit(g, B.type, whom);
	const int32_t cost = unit_control_cost(u);
	if (num_inside(g, B, 0) + cost <= lim) {
		const int32_t owner = cell_owner_at(g, B.x, B.y);
		if (owner >= 0 && owner != whom && !is_ally(g, whom, owner)) {  // (console: "enemy territory")
			kill_current_order(g, u, false);
			return 0;
		}
		Unit* cap = captain_of(g, u);
		unit_go_inside(g, *cap, ox, whom);
		kill_garrison_order(g, *cap, 0);
		return 0;
	}
	// 6. full: another building of the same city, else give up.
	if (find != 0 && B.city >= 0) {
		const int32_t b = find_garrison_build(g, u, B.city, whom);
		if (b >= 0) {
			kill_current_order(g, u, false);
			add_garrison_order(g, u, b, whom, 1, kQueueFirst, (oflags & orderflag::kExplicit) != 0);
			return 0;
		}
	}
	kill_current_order(g, u, false);  // (console: "%s is full")
	return 0;
}

// ---------------------------------------------------------------- the group actions (5)

void group_action_garrison(Game& g, Group& gr, int32_t ox, int32_t whom, QueuePos q, int32_t find) {
	// @700490.
	if (g.scen.ignore_orders != 0 && uint32_t(gr.who) < 8) for (size_t k = 0; k < g.scen.objects_ignoring_orders[size_t(gr.who)].size(); k++) if (g.scen.objects_ignoring_orders[size_t(gr.who)][k] >= 0) group_kill(g, gr, g.scen.objects_ignoring_orders[size_t(gr.who)][k], gr.who, 0, 0);  // ScenarioData::ignore_orders prelude
	gr.disband = 0;  // action_begin
	if (q == kQueueFirst) {
		std::vector<Order> saved = group_set_up_insert(g, gr);
		group_action_halt(g, gr, 0);
		group_action_garrison(g, gr, ox, whom, kQueueNew, 0);
		group_finish_insert(g, gr, saved);
		return;
	}
	gr.form = -1;  // group +0x10
	if (!is_ally(g, gr.who, whom)) return;  // (console feedback)
	Object* T = obj_at(g, ox, whom);
	if (!T || !T->alive() || T->is_unit) return;
	if (get_garrison_limit(g, T->type, whom) == 0 && !type_is(*T->ptype, kTypeUniversity)) return;  // ❓ is(0x1a4)
	if (is_unassimilated(g, static_cast<const Build&>(*T))) return;
	const int32_t city = static_cast<const Build&>(*T).city;
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!m || !m->alive() || !m->on_map()) continue;
		if (m->ptype->domain == kDomainAir && !unit_is_helicopter(*m)) continue;
		if (cast_is_entering_or_exiting(*m)) continue;  // UnitData::is_entering_or_exiting @60a6f0 (garrison-spec 5)
		if (action_type(*m) == kGarrison) {
			repath(g, *m);
			kill_current_order(g, *m, false);
		}
		if (!type_can_garrison(g, *m->ptype, T->type)) continue;
		int32_t b = find != 0 ? find_garrison_build(g, *m, city, whom) : -1;
		if (b < 0) b = ox;
		if (g.editor && q == kQueueNew) {
			// The scenario editor puts the unit straight in when there is room.
			if (num_inside(g, *T, 0) + unit_control_cost(*m) <= get_garrison_limit(g, T->type, whom)) {
				unit_go_inside(g, *m, ox, whom);
				continue;
			}
		}
		QueuePos q2 = q;
		if (is_worker_type(m->type)) {
			q2 = kQueueFirst;  // a worker keeps its work behind the garrison order
		} else if (q == kQueueNew && (m->ptype->unit_flags2 & 4) && cast_is_packing_or_unpacking(*m)) {
			// A packer that is packing keeps its last order (the pack cast) and garrisons behind it; one that is unpacking is
			// simply re-ordered (spells-spec 5, @700a00..700b60).
			if (!cast_is_unpacking(*m)) {
				if (!m->orders.empty()) {
					const Order keep = m->orders.front();  // LinkListBase::tail() is the current order (head->prev)
					m->orders.pop_front();
					clear_orders(g, *m);
					m->orders.push_back(keep);
					unit_update_gpiece(g, *m);
				}
				q2 = kQueueLast;
			} else {
				q2 = kQueueNew;
			}
		}
		add_garrison_order(g, *m, b, whom, find, q2, true);
	}
}

void group_action_eject_all(Game& g, Group& gr, int32_t back_to_work, int32_t who, int32_t eject_o, int32_t eject_who) {
	// @710b40.
	if (g.scen.ignore_orders != 0 && uint32_t(gr.who) < 8) for (size_t k = 0; k < g.scen.objects_ignoring_orders[size_t(gr.who)].size(); k++) if (g.scen.objects_ignoring_orders[size_t(gr.who)][k] >= 0) group_kill(g, gr, g.scen.objects_ignoring_orders[size_t(gr.who)][k], gr.who, 0, 0);  // ScenarioData::ignore_orders prelude
	gr.disband = 0;  // action_begin
	if ((eject_who < 0 && who == gr.who) || who < 0) {
		for (int32_t i = gr.num - 1; i >= 0; i--) {
			Object* ob = g.object(gr.who, gr.list[size_t(i)]);
			if (!ob || !ob->alive()) continue;
			if (num_inside(g, *ob, 1) <= 0 || can_carry_domain(g, *ob, kDomainAir)) continue;
			eject_contents(g, *ob, 0, back_to_work != 0 ? 0x32 : -1, 0, back_to_work == 0 ? 1 : 0);
			if (ob->alive() && ob->ptype && (type_is(*ob->ptype, kTypeBarge) || type_is(*ob->ptype, kTypeMerchantFleet)) &&
			    num_inside(g, *ob, 0) == 0)
				object_die(g, *ob, 0, -1, 0.0f);
		}
		return;
	}
	Object* B = eject_o >= 0 ? obj_at(g, eject_o, eject_who) : nullptr;
	if (!B || B->is_unit) return;
	// Repeatedly the first unit of leader `who` in the chain comes out; one that cannot leave is passed over.
	std::vector<int32_t> stuck;
	for (int guard = 0; guard < kUnitLimit; guard++) {
		Unit* pick = nullptr;
		int32_t o = B->inside_down, w = B->inside_down_who;
		for (int g2 = 0; o >= 0 && g2 < kUnitLimit; g2++) {
			Unit* c = g.unit(w, o);
			if (!c) break;
			if (c->who == who && std::find(stuck.begin(), stuck.end(), int32_t(c->o)) == stuck.end()) {
				pick = c;
				break;
			}
			o = c->inside_down;
			w = c->inside_down_who;
		}
		if (!pick) break;
		if (unit_come_out(g, *pick, 0) != 0) stuck.push_back(pick->o);
	}
}

// ---------------------------------------------------------------- ejection (6)

namespace {

// Object::die_uber(0): every member of the chain of u dies.
void die_chain(Game& g, Unit& u) {
	std::vector<int32_t> members;
	Unit* c = captain_of(g, u);
	for (int guard = 0; c && guard < kUnitLimit; guard++) {
		members.push_back(c->o);
		c = c->o_down >= 0 ? g.unit(c->who, c->o_down) : nullptr;
	}
	for (int32_t o : members) {
		Unit* m = g.unit(u.who, o);
		if (m && m->alive()) unit_die(g, *m, 0);
	}
}

}  // namespace

void eject_contents(Game& g, Object& ob, int32_t kill_if_stuck, int32_t type, int32_t carry_orders, int32_t no_orders) {
	if (!on_map(ob)) return;
	if (!ob.is_unit && type < 0 && !g.editor && !can_carry_domain(g, ob, kDomainAir)) {
		static_cast<Build&>(ob).build_masks |= bmask::kEjecting;
		return;
	}
	// The chain is read first: a captain takes its followers out with it.
	std::vector<std::array<int32_t, 2>> chain;
	{
		int32_t o = ob.inside_down, w = ob.inside_down_who;
		for (int guard = 0; o >= 0 && guard < kUnitLimit; guard++) {
			const Object* c = g.object(w, o);
			if (!c) break;
			chain.push_back({o, w});
			o = c->inside_down;
			w = c->inside_down_who;
		}
	}
	for (const auto& r : chain) {
		Unit* u = g.unit(r[1], r[0]);
		if (!u || !u->alive() || u->inside_up < 0) continue;
		if (type >= 0 && !unit_is(g, *u, type)) {
			const int32_t base = g.type(type).type;  // types[type].type: the citizen base of a citizen type
			const bool arm = is_peasant_t(base) && unit_is(g, *u, kTypeMilitia) && no_orders == 0;
			if (!arm) continue;
		}
		if (no_orders == 0) {
			if (get_action(*u)) repath(g, *u);
			// An armed citizen (a militia) casts CIVILIAN (0x293, spell 0xa4c) at the building's position, QUEUE_FIRST.
			if (unit_is(g, *u, kTypeMilitia) && spell_is_castable(g, ptype::kCivilianSpell, u->o, u->who, 0) != 0)
				add_cast_order(g, *u, -1, -1, ob.x, ob.y, ptype::kCivilianSpell, kQueueFirst, true);
		} else {
			clear_orders(g, *u);
		}
		if (unit_come_out(g, *u, 0) != 0) {
			if (kill_if_stuck) unit_die(g, *u, 0);
			continue;
		}
		if (!ob.is_unit) {
			const Build& bld = static_cast<const Build&>(ob);
			if (kill_if_stuck && u->type >= 50 && u->type <= 53) {
				bool armed = false;
				if (is_peasant_t(u->type)) {
					const int32_t M = militia_type(g, u->who);
					if (M >= 0 && has_tech(g, u->who, M)) {
						unit_set_type(g, *u, current_upgrade(g, u->who, M), 0);
						clear_orders(g, *u);
						armed = true;
					}
				}
				if (!armed && bld.city >= 0 && bld.who >= 0 && bld.who < 8 && size_t(bld.city) < g.cities[size_t(bld.who)].size()) {
					const City& own = g.cities[size_t(bld.who)][size_t(bld.city)];
					const int32_t c = objects_find_city(g, u->x, u->y, search::kFriendly, u->who, 0x200, filter::kNotMe, own.o, bld.who);
					if (c >= 0 && g.find_who >= 0 && g.find_who < 8 && size_t(c) < g.cities[size_t(g.find_who)].size()) {
						const City& C = g.cities[size_t(g.find_who)][size_t(c)];
						const Build* cb = g.build(g.find_who, C.o);
						add_move_order(g, *u, cb ? cb->x : C.x, cb ? cb->y : C.y, 4, false, kQueueNew, false, -1, -1);
					}
				}
			}
			if (type_is(*bld.ptype, kTypeAirbase)) add_strafe_order(g, *u, -1, -1, -1, -1, 1, kQueueNew, false);  // it flies off
		}
		if (ob.is_unit) {
			// From a unit (a transport barge; docs/transport-spec.md 7.1). (sound S_TRANSPORTUNLOAD when the carrier is on
			// screen and seen by the console)
			Unit& carrier = static_cast<Unit&>(ob);
			same_damage(g, *u, carrier.o, carrier.who);  // the cargo takes the carrier's damage ratio
			if (unit_is_caravan(carrier)) {
				u->cara = carrier.cara;
				if (carrier.cara >= 0 && size_t(carrier.cara) < g.caravans[size_t(carrier.who)].size())
					g.caravans[size_t(carrier.who)][size_t(carrier.cara)].o = int16_t(u->o);
				carrier.cara = -1;
				if (carrier.unit_masks & 0x200) u->unit_masks |= 0x200;  // kTradeLoaded
				else u->unit_masks &= ~0x200u;
			}
			if (carry_orders != 0) {
				if (u->ptype->uber_size != 1) {
					// TODO(groups): Unit::reset_move_orders(carrier); for a temporary group (not an army member) the original
					// re-issues the carrier's orders to the chain through Group::set_up_insert / kill / add / push_group /
					// finish_insert. ❓ approximated as the single-unit hand-over below (orders and path to the captain).
				}
				// The carrier's whole order list goes to the unit, front first, and so does its path (same stack order).
				while (!carrier.orders.empty()) {
					u->orders.push_back(carrier.orders.front());
					carrier.orders.pop_front();
				}
				for (const PathData& p : carrier.path) u->path.push_back(p);
				carrier.path.clear();
				if (!u->path.empty()) {  // the next point: no transport flag
					PathData p = u->path.back();
					u->path.pop_back();
					p.flags &= ~pathflag::kTransport;
					u->path.push_back(p);
				}
			}
			if (transport_trace())
				std::fprintf(stderr, "[transport] f%d landing L%d unit %d (type %d) from %d at %d,%d: damage %d, %zu orders (front %d), path %zu\n",
				             g.frame, u->who, u->o, u->type, carrier.o, u->x, u->y, u->damage, u->orders.size(),
				             u->orders.empty() ? 0 : int(u->orders.front().type), u->path.size());
			// The leader's transport level (re)grants the flag; scouts only with the leader option.
			const int32_t level = leader_can_transport(g, carrier.who);
			if (unit_transport_type(g, *u) <= level && (!unit_is(g, *u, 0x45) || (g.leaders[size_t(carrier.who)].options_flags & 2)))
				u->unit_masks |= umask::kCanTransport;
			// (console selection: the carrier out, the unit in; Object::replace_hotunit: presentation)
		}
	}
}

void kill_contents(Game& g, Object& ob) {
	std::vector<std::array<int32_t, 2>> chain;
	int32_t o = ob.inside_down, w = ob.inside_down_who;
	for (int guard = 0; o >= 0 && guard < kUnitLimit; guard++) {
		const Object* c = g.object(w, o);
		if (!c) break;
		chain.push_back({o, w});
		o = c->inside_down;
		w = c->inside_down_who;
	}
	for (const auto& r : chain) {
		Unit* u = g.unit(r[1], r[0]);
		if (!u || !u->alive() || u->inside_up < 0) continue;
		unit_come_out(g, *u, 0);
		die_chain(g, *u);
	}
}

void build_process_ejection(Game& g, Build& b) {
	// @6201e0
	if (b.inside_down < 0) {
		b.build_masks &= ~bmask::kEjecting;
		return;
	}
	Unit* u = g.unit(b.inside_down_who, b.inside_down);
	if (!u) return;
	// The captain of the first chain comes out; a chain that cannot (come_out != 0) is tried again next frame, as in
	// the original. A dead building keeps its slot one more frame per attempt.
	unit_come_out(g, *captain_of(g, *u), 1);
	if (!b.alive()) b.hold_frames = uint16_t(b.hold_frames + 1);
}

// ---------------------------------------------------------------- the frame (7)

void unit_process_inside(Game& g, Unit& u) {
	u.unit_masks2 &= ~(0x20u | 0x40u | 0x8000u);
	u.unit_masks &= ~(umask::kBlowCover | umask::kCoverBlown);
	if (!(u.unit_masks & umask::kCloakRunning)) u.unit_masks &= ~umask::kUnderCover;
	if ((g.frame + u.who) % 32 == 0 && (u.unit_masks & umask::kAiControl)) {
		int32_t xw;
		const int32_t x = unit_get_inside(g, u, &xw);
		const Object* c = x >= 0 ? g.object(xw, x) : nullptr;
		if (c && !c->is_unit && (c->flags & objflag::kCity)) {
			const Build& cb = static_cast<const Build&>(*c);
			if (cb.city >= 0 && c->who >= 0 && c->who < 8 && size_t(cb.city) < g.cities[size_t(c->who)].size() &&
			    !(g.cities[size_t(c->who)][size_t(cb.city)].city_flags & (cityflag::kUnderAttack | cityflag::kAlarm)))
				unit_come_out(g, u, 0);
		}
	}
	for (Guy& gy : u.guys) gy.last_z = gy.z;
}

}  // namespace ron::sim
