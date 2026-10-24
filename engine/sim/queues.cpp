// Training and research queues, spawning and rally points (docs/production-spec.md section 6): BuildQueue,
// Build::queue_up / could_queue / can_make, Group::action_queue_up, Build::do_queue / finished / train,
// Unit::come_out, unqueue and refunds, gather points, BUILDMASK, libraries, Unit::set_type, containment.
#include <algorithm>

#include "sim/aircraft.hpp"
#include "sim/heroes.hpp"
#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/production_internal.hpp"
#include "sim/search.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using prod::is_t;
using prod::tb;

namespace {
Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }
const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }
}  // namespace

// ---------------------------------------------------------------- BuildQueue

void BuildQueue::set_queue(int32_t i, int32_t type, const int32_t* cost6, bool keep_jc) {
	if (i < 0 || i >= queue_size) return;
	QueueItem& it = queue[size_t(i)];
	it.type = int16_t(type);
	if (!keep_jc) it.job_counter = 0;
	if (!cost6) return;
	int32_t k = 0;
	for (int32_t gd = 0; gd < 6 && k < 3; gd++) {
		if (cost6[gd] == 0) continue;
		it.good[size_t(k)] = int16_t(gd);
		it.cost[size_t(k)] = int16_t(cost6[gd]);
		k++;
	}
	for (; k < 3; k++) {
		it.good[size_t(k)] = -1;
		it.cost[size_t(k)] = 0;
	}
}

void BuildQueue::un_queue(int32_t i, int32_t n) {
	if (i < 0 || n - i - 1 <= 0) return;
	for (int32_t k = i; k < n - 1 && k + 1 < queue_size; k++) queue[size_t(k)] = queue[size_t(k + 1)];
}

// ---------------------------------------------------------------- containment

namespace {

void unit_set_stance(Game& g, Unit& u, int32_t s) {
	Unit* c = &u;
	for (int guard = 0; c->o_up >= 0 && guard < 64; guard++) {
		Unit* p = g.unit(c->who, c->o_up);
		if (!p) break;
		c = p;
	}
	for (int guard = 0; c && guard < 64; guard++) {
		c->stance = int8_t(s);
		if (c->o_down < 0) break;
		Unit* d = g.unit(c->who, c->o_down);
		if (!d || !d->alive()) break;
		c = d;
	}
}

}  // namespace

void unit_go_inside(Game& g, Unit& u, int32_t o, int32_t who) {
	Unit* c = &u;
	for (int guard = 0; c->o_up >= 0 && guard < 64; guard++) {  // followers go inside through their captain
		Unit* p = g.unit(c->who, c->o_up);
		if (!p) break;
		c = p;
	}
	unit_insert_inside(g, *c, o, who);
	// (UI: the console selection drops it.)
	const Object* cont = g.object(who, o);
	// Into a unit (a transport): every member of an uber chain forgets its orders (the original's recursive
	// go_inside(follower, o, who, 1) runs this block for each of them).
	auto drop_orders = [&](Unit& m) {
		if (cont && cont->is_unit && m.ptype->uber_size > 1) {
			m.unit_masks &= ~umask::kMultimove;
			m.path.clear();
			close_orders(g, m, false);
			clear_partial_path(m);
			update_action(g, m);
		}
	};
	drop_orders(*c);
	if (c->o_down >= 0) {
		Unit* d = g.unit(c->who, c->o_down);
		if (d && d->alive()) {
			unit_insert_inside(g, *d, o, who);  // go_inside(follower, o, who, 1)
			drop_orders(*d);
			for (Unit* q = d; q->o_down >= 0;) {
				Unit* n = g.unit(q->who, q->o_down);
				if (!n || !n->alive()) break;
				unit_insert_inside(g, *n, o, who);
				drop_orders(*n);
				q = n;
			}
		}
	}
	if (c->type == ptype::kScholars || c->type == ptype::kScholarsKorean) {
		LD(g, c->who).flags |= leaderflag::kRecompute;
		if (cont) unit_set_new_location(g, *c, cont->x, cont->y, true, true);
		unit_set_angle(g, *c, 0, true);
		unit_set_anim(g, *c, kCharDefault, 1, 1);
	}
}

// ---------------------------------------------------------------- Unit::set_type (6.8.1)

void unit_set_type(Game& g, Unit& u, int32_t t, int32_t mode) {
	const Type& old = *u.ptype;
	const Type& T = g.type(t);
	auto counted = [&](const Unit& v) {
		if (v.o_up >= 0) return false;
		return v.ptype->control_cost != 0 || unit_is(g, v, ptype::kFighterBomber) || (v.ptype->unit_flags & 0x4000000);
	};
	if (mode == 0) {
		if (counted(u) && !(u.unit_masks & umask::kDecoy)) {
			track_unit_type(g, u.who, u.type, -1, u.o);
			LD(g, u.who).control -= old.control_cost;
			LD(g, u.who).active--;
		}
		if (old.unit_flags2 & 8) LD(g, u.who).caras--;
		const int32_t nguys0 = int32_t(u.guys.size());
		for (int32_t gi = T.squad_size; gi < u.guy_mark; gi++) kill_guy(g, u.who, u.o, gi, 0, -1, 0.0f);
		for (int32_t gi = old.squad_size; gi < nguys0; gi++) kill_guy(g, u.who, u.o, gi, 0, -1, 0.0f);
		const int32_t n = T.squad_size + T.crew_size;
		u.guys.resize(size_t(std::max(n, nguys0)));
		for (int32_t gi = nguys0; gi < T.squad_size; gi++) u.guys[size_t(gi)] = Guy();  // (null: not refilled)
		u.guys.resize(size_t(n));
		for (int32_t gi = T.squad_size; gi < n; gi++) u.guys[size_t(gi)] = Guy();
		u.type = t;
		u.ptype = &T;
		u.guy_mark = int8_t(std::min<int32_t>(u.guy_mark, T.squad_size));
		for (int32_t gi = 0; gi < u.guy_mark; gi++) {
			Guy& gy = u.guys[size_t(gi)];
			gy.type = t;
			guy_init_real(g, u, gy, 1);
			gy.guy_num = int8_t(gi);
		}
		for (int32_t gi = T.squad_size; gi < n; gi++) {
			Guy& gy = u.guys[size_t(gi)];
			gy.type = t;
			gy.guy_num = int8_t(gi);
			gy.who = u.who;
			gy.o = u.o;
			guy_init_real(g, u, gy, 1);
		}
	}
	u.type = t;
	u.ptype = &T;
	if (mode != 2) {
		if (unit_is(g, u, 0x165, true)) u.unit_masks2 |= umask2::kTheCeo;
		else u.unit_masks2 &= ~umask2::kTheCeo;
		if (unit_is(g, u, 0x77)) u.unit_masks2 |= umask2::kMarines;
		unit_update_hits(g, u);  // Unit::update_hits @60e930 (combat-spec 5.1)
		unit_update_los(g, u);
		unit_update_armor(g, u);  // Unit::update_armor @6054c0
		update_speed(g, u);
	}
	if (mode == 0) unit_update_gpiece(g, u);
	if (mode != 2) {
		if (counted(u) && !(u.unit_masks & umask::kDecoy)) {
			track_unit_type(g, u.who, t, 1, u.o);
			LD(g, u.who).control += T.control_cost;
			LD(g, u.who).active++;
		}
		if (T.unit_flags2 & 8) LD(g, u.who).caras++;
	}
	if (mode != 0) return;
	for (int32_t gi = 0; gi < u.guy_mark; gi++) guy_set_new_location(g, u, u.guys[size_t(gi)], u.x, u.y, true);
	// Supplies follow the new type (games newer than the expansion's release build, i.e. every Extended Edition
	// game; the version gate is not modelled).
	if (u.who >= 8) return;
	const bool supply = (T.unit_flags2 & 0x40) != 0;
	if (!supply && u.supply >= 0) {
		supplies_close_supply(g, u.who, u.supply);
		u.supply = -1;
	}
	if (supply && u.supply < 0) u.supply = int16_t(supplies_init_supply(g, u.who, u.o));
}

// ---------------------------------------------------------------- gates (6.2.4)

bool queue_here(const Game& g, int32_t bt, int32_t t) {
	if (t == ptype::kDisband) return true;
	const Type& ty = g.type(t);
	if (ty.preq[0] == -2 || ty.where < 0) return false;
	const Type& b = g.type(bt);
	if (is_unit_type(t)) {
		if (type_is(b, ty.where)) return true;
		const int32_t up = g.type(ty.where).upgrade;
		return up >= 0 && type_is(b, up);
	}
	if (is_build_type(t)) return type_is(b, ty.where, true);
	return type_is(b, ty.where);
}

int32_t build_count_queue(const Game& g, const Build& b, int32_t kind, int32_t t) {
	int32_t n = 0;
	for (int32_t i = 0; i < b.queued; i++) {
		const int32_t v = b.build_queue.get_queue(i);
		if (kind == 0) n += (v == 50 || v == 51) ? 1 : 0;
		else if (kind == 1) n += (v >= 0 && type_is(g.type(v), t, true)) ? 1 : 0;
		else n += (is_unit_type(v) && LD(g, b.who).has_tech_bit(v)) ? 1 : 0;
	}
	return n;
}

int32_t build_can_make(const Game& g, const Build& b, int32_t t, int32_t p) {
	if (!(b.flags & objflag::kActive)) return 0;
	if (!g.editor && t != ptype::kDisband && t != ptype::kDepopulate) {
		if (is_unassimilated(g, b)) return 0;  // (is_neutralized is always 0 for buildings)
	}
	if (!queue_here(g, b.type, t)) return 0;
	const int32_t a = type_avail(g, b.who, t, p);
	if (a == 0) return 0;
	if (is_tech_type(t)) return (has_tech(g, b.who, t) || researching(g, b.who, t, b.o, false, false)) ? 0 : a;
	if (is_unit_type(t)) {
		if (LD(g, b.who).has_tech_bit(t) && !(b.ptype->build_flags & 0x10)) return (is_unassimilated(g, b) && a >= 2) ? 3 : a;
		return a;
	}
	if (is_spell_type(t)) {
		// TODO(spells): SpellTypeData::is_castable for owned spells.
		if (LD(g, b.who).has_tech_bit(t)) return t == ptype::kDisband ? a : 0;
		return type_is(*b.ptype, g.type(t).where) ? a : 0;
	}
	if (is_build_type(t) && b.city >= 0 && is_t(g, t, ptype::kTown)) {
		// CityData::can_upgrade(city, t)
		(void)0;  // the city-upgrade path is normally pre-empted by City::check_upgrade (5.4)
	}
	return a;
}

bool build_could_queue(Game& g, const Build& b, int32_t t) {
	g.queue_fail = 0;
	if (build_can_make(g, b, t, 1) == 0) {
		g.queue_fail = 1;
		return false;
	}
	if (b.queued >= b.build_queue.queue_size) {
		g.queue_fail = 2;
		return false;
	}
	if (type_is(*b.ptype, ptype::kUniversity) && (t == 52 || t == 53)) {
		// count_queue(TYPE, SCHOLARS) + num_gatherers(0, 0) > 6 (scholars inside: TODO(economy), 0).
		if (build_count_queue(g, b, 1, ptype::kScholars) + 0 > 6) {
			g.queue_fail = 2;
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------- QUEUE_UP (6.2)

int32_t build_queue_up(Game& g, Build& b0, int32_t t, int32_t use_escrow) {
	Build* bp = &b0;
	const int32_t who = b0.who;
	if (type_is(*b0.ptype, ptype::kLibrary) && t != ptype::kDisband) {
		const int32_t lib = get_first_library(g, who);
		if (lib < 0) return 1;
		if (lib != b0.o) return build_queue_up(g, *g.build(who, lib), t, use_escrow);
		// (tech-race shortcut: victory 9 is not modelled, TODO(victory))
	}
	Build& b = *bp;
	if (!can_pay_cost(g, t, who, -1, b.o, 1)) {
		g.queue_fail = 5;
		return 1;
	}
	if (!build_could_queue(g, b, t)) return 1;
	Leader& l = LD(g, who);
	if (is_tech_type(t) || type_avail(g, who, t, 1) == 2) l.tech_frame = g.frame;
	if (is_tech_type(t)) l.tech_cat_frame[size_t(g.type(t).cat & 3)] = g.frame;
	int32_t cost6[6] = {0, 0, 0, 0, 0, 0};
	if (!g.editor) pay_cost(g, t, who, b.o, -1, use_escrow, cost6);
	const int32_t slot = b.queued;
	b.queued++;
	b.build_queue.set_queue(slot, t, cost6, false);
	if (b.queued == 1) b.build_queue.queue[0].job_counter = 0;
	if (t >= 0) {
		l.num_queued[size_t(t)]++;
		if (is_unit_type(t) && g.type(t).attack != 0) {
			const Type& ty = g.type(t);
			switch (ty.where) {
			case ptype::kBarracks: l.barracks_queued++; l.combat_queued++; break;
			case ptype::kStable: l.stable_queued++; l.combat_queued++; break;
			case ptype::kSiegeFactory: l.factory_queued++; break;
			case ptype::kDock: l.dock_queued++; break;
			default:
				if (ty.domain == 2) l.air_queued++;
				break;
			}
		}
	}
	if (is_age_type(t)) l.ages_queued++;
	if (is_epoch_type(t)) l.epochs_queued++;
	return 0;
}

void group_action_queue_up(Game& g, Group& gr, int32_t t, int32_t num) {
	// prelude (orders-spec 2.1): scenario ignore_orders TODO; action_begin.
	gr.disband = 0;
	if (!gr.buildings) return;
	const int32_t who = gr.who;
	std::vector<int16_t> S(gr.list.begin(), gr.list.begin() + gr.num);
	auto bld = [&](int16_t o) { return g.build(who, o); };
	for (int32_t i = 0; i + 1 < int32_t(S.size()); i++) {
		Build* bi = bld(S[size_t(i)]);
		if (!bi || !bi->alive() || !(bi->flags & objflag::kActive)) continue;
		for (int32_t j = i + 1; j < int32_t(S.size()); j++) {
			Build* bj = bld(S[size_t(j)]);
			Build* bi2 = bld(S[size_t(i)]);
			if (bj && bi2 && bj->queued < bi2->queued) std::swap(S[size_t(i)], S[size_t(j)]);
		}
	}
	gr.disband = 0;
	const bool trains = (is_unit_type(t) && LD(g, who).has_tech_bit(t)) || (is_build_type(t) && !(g.type(t).build_flags & 4));
	if (trains) {
		for (int32_t r = 0; r < num; r++) {
			for (int16_t o : S) {
				Build* b = bld(o);
				if (!b || !b->alive() || !(b->flags & objflag::kActive)) continue;
				// Missile silos skip unit types they cannot carry (ObjectData::can_carry(TypeIndex)).
				if (is_unit_type(t) && type_is(*b->ptype, airtype::kSilo, false) && !object_can_carry_type(g, *b, t)) continue;
				build_queue_up(g, *b, t, 1);
			}
		}
		return;
	}
	if (researching(g, who, t, -1, false, false)) return;  // (console-only feedback)
	for (int pass = 0; pass < 2; pass++) {
		for (int16_t o : S) {
			Build* b = bld(o);
			if (!b || !b->alive() || !(b->flags & objflag::kActive)) continue;
			if (pass == 0 && b->queued != 0) continue;
			if (build_queue_up(g, *b, t, 1) == 0) return;
		}
	}
}

// ---------------------------------------------------------------- unqueue (6.5)

namespace {
void build_unpay_cost(Game& g, Build& b, int32_t i) {
	if (g.editor || i < 0 || i >= b.build_queue.queue_size) return;
	const QueueItem& it = b.build_queue.queue[size_t(i)];
	for (int32_t k = 0; k < 3; k++) {
		if (it.good[size_t(k)] >= 0) LD(g, b.who).bucket[size_t(it.good[size_t(k)])] += it.cost[size_t(k)];
	}
}
}  // namespace

void build_unqueue(Game& g, Build& b, int32_t i, bool cancel) {
	const bool disband = b.queued && i < b.queued && i < b.build_queue.queue_size &&
	                     b.build_queue.queue[size_t(i)].type == ptype::kDisband;
	if (type_is(*b.ptype, ptype::kLibrary) && !disband) {
		const int32_t lib = get_first_library(g, b.who);
		if (lib != b.o && lib >= 0) {
			build_unqueue(g, *g.build(b.who, lib), i - b.queued, cancel);
			return;
		}
	}
	if (b.queued == 0 || i > b.queued - 1 || i < 0) return;
	if (cancel) {
		while (i + 1 < b.queued && b.build_queue.get_queue(i) == b.build_queue.get_queue(i + 1)) i++;
	}
	b.build_queue.queue[size_t(i)].job_counter = 0;
	const int32_t t = b.build_queue.queue[size_t(i)].type;
	Leader& l = LD(g, b.who);
	if (t >= 0) {
		if (l.num_queued[size_t(t)] != 0) l.num_queued[size_t(t)]--;
		if (is_unit_type(t) && g.type(t).attack != 0) {
			const Type& ty = g.type(t);
			auto dec = [](int32_t& c) {
				if (c != 0) c--;
			};
			switch (ty.where) {
			case ptype::kBarracks: dec(l.barracks_queued); dec(l.combat_queued); break;
			case ptype::kStable: dec(l.stable_queued); dec(l.combat_queued); break;
			case ptype::kSiegeFactory: dec(l.factory_queued); break;
			case ptype::kDock: dec(l.dock_queued); break;
			default:
				if (ty.domain == 2) dec(l.air_queued);
				break;
			}
		}
	}
	if (is_age_type(t)) l.ages_queued--;
	if (is_epoch_type(t)) l.epochs_queued--;
	if (cancel) build_unpay_cost(g, b, i);
	if (i < b.queued - 1) b.build_queue.un_queue(i, b.queued);
	b.queued--;
	if (b.queued == 0) b.build_masks &= ~bmask::kInfinite;
}

void build_action_unqueue(Game& g, Build& b0, int32_t i) {
	Build* b = &b0;
	for (int guard = 0; guard < 8; guard++) {
		if (!type_is(*b->ptype, ptype::kLibrary)) break;
		const int32_t idx = std::max(i, 0);
		if (idx < b->build_queue.queue_size && b->build_queue.get_queue(idx) == ptype::kDisband) break;
		const int32_t lib = get_first_library(g, b->who);
		if (lib == b->o || lib < 0) break;
		b = g.build(b->who, lib);
	}
	if (b->queued == 0) return;
	if (b->build_masks & bmask::kInfinite) {
		b->build_masks &= ~bmask::kInfinite;
		if (i >= -1) return;
	}
	if (i <= -10) {
		while (b->queued) build_unqueue(g, *b, b->queued - 1, true);
	} else if (i <= -5) {
		for (int32_t k = std::min<int32_t>(b->queued, 5); k > 0; k--) build_unqueue(g, *b, b->queued - 1, true);
	} else {
		build_unqueue(g, *b, i < 0 ? b->queued - 1 : i, true);
	}
}

void build_clean_queue(Game& g, Build& b, bool refund) {
	for (int guard = 0; b.queued && guard < 100; guard++) build_unqueue(g, b, b.queued - 1, refund);
	b.build_masks &= ~bmask::kInfinite;
}

// ---------------------------------------------------------------- libraries (7.10)

void build_new_library(Game& g, Build& b) {
	if (!type_is(*b.ptype, ptype::kLibrary)) return;
	const int32_t first = get_first_library(g, b.who);
	if (first < 0 || b.o >= first) return;
	Build& f = *g.build(b.who, first);
	b.queued = f.queued;
	for (int32_t i = 0; i < f.queued && i < b.build_queue.queue_size && i < f.build_queue.queue_size; i++)
		b.build_queue.queue[size_t(i)] = f.build_queue.queue[size_t(i)];
	f.queued = 0;
}

// ---------------------------------------------------------------- gather points (6.6)

bool build_gather_inside(const Build& b) {
	if (b.gather.empty()) return false;
	const GatherPoint& p = b.gather.front();
	return p.action != 3 && (p.x < 0 || p.y < 0);
}

void build_clear_gather(Game& g, Build& b) {
	b.gather.clear();
	if (type_is(*b.ptype, ptype::kAirbase)) airbase_gather_cleared(g, b);  // its planes go home / drop their orders
}

void build_add_gather_point(Game& g, Build& b, int32_t x, int32_t y, int32_t action, QueuePos q) {
	if (q == kQueueNew) build_clear_gather(g, b);
	GatherPoint p{x, y, uint8_t(action)};
	if (q == kQueueLast) b.gather.push_back(p);  // add before the head, then head = head.next: the new tail
	else b.gather.insert(b.gather.begin(), p);   // the new head
	if (type_is(*b.ptype, ptype::kAirbase)) airbase_gather_added(g, b);  // its planes re-issue their patrols
}

void group_action_gather_point(Game& g, Group& gr, int32_t x, int32_t y, int32_t action, int32_t add_to_end) {
	gr.disband = 0;
	const int32_t who = gr.who;
	auto eligible = [&](const Build* b) {
		return b && b->alive() && !type_is(*b->ptype, ptype::kUniversity) && !type_is(*b->ptype, ptype::kMissileSilo);
	};
	std::vector<Build*> members;
	for (int32_t i = 0; i < gr.num; i++) {
		Object* ob = g.object(who, gr.list[size_t(i)]);
		members.push_back(ob && !ob->is_unit ? static_cast<Build*>(ob) : nullptr);
	}
	if (x < 0 || y < 0) {
		for (Build* b : members)
			if (eligible(b)) build_clear_gather(g, *b);
		return;
	}
	x = std::min(x, g.world.xs * 0x300 - 1);
	y = std::min(y, g.world.ys * 0x300 - 1);
	bool q_last = add_to_end != 0;
	for (Build* b : members)
		if (b && b->alive() && eligible(b) && build_gather_inside(*b)) q_last = false;
	if (action == 3) {
		Group airbases;  // the airbases that took the point
		group_clear(airbases, -1);
		for (size_t i = 0; i < members.size(); i++) {
			Build* b = members[i];
			if (b && b->alive() && type_is(*b->ptype, ptype::kAirbase)) {
				build_add_gather_point(g, *b, x, y, 3, q_last ? kQueueLast : kQueueNew);
				group_add(g, airbases, b->o, gr.who, 0, 0);
			}
		}
		// action_flight(x, y, ATTACK, 0, 0, 0): the original passes the point's coordinates where the object (ox, whom)
		// belongs, so it never finds a live target and does nothing. ❓ (aircraft-spec 8; the exe is what it is.)
		if (airbases.num != 0) group_action_flight(g, airbases, x, y, kAttack, 0, 0, 0);
	} else {
		// (a group of cities pings a nearby woodcutter / mine: TODO(economy) find_building with FILTER_TYPE)
	}
	bool inside = false;
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	if (action != 3 && g.world.in_t(tx, ty) && (g.world.t(tx, ty) & 3) == 3 && !q_last) {
		for (Build* b : members) {
			if (!eligible(b)) continue;
			const Footprint f = footprint(*b->ptype, b->x, b->y);
			if (tx >= f.x0 && tx < f.x0 + f.w && ty >= f.y0 && ty < f.y0 + f.h) {
				inside = true;
				if (b->type == ptype::kTerraCotta || b->type == ptype::kKremlin || b->type == ptype::kSenate) {
					for (Build* e : members) {
						if (eligible(e)) {
							build_clear_gather(g, *e);
							break;
						}
					}
					return;
				}
				break;
			}
		}
	}
	for (Build* b : members) {
		if (!eligible(b) || (type_is(*b->ptype, ptype::kAirbase) && action == 3)) continue;
		const int32_t basic = basic_type(g, b->type);
		if (!(g.type(basic).build_flags & 0x80000000u) && get_garrison_limit(g, b->type, who) == 0 &&
		    b->type != ptype::kTerraCotta && b->type != ptype::kKremlin && b->type != ptype::kSenate)
			continue;
		if (inside) build_add_gather_point(g, *b, -1, -1, 0, kQueueNew);
		else build_add_gather_point(g, *b, x, y, action, q_last ? kQueueLast : kQueueNew);
	}
}

void group_action_buildmask(Game& g, Group& gr, int32_t mask, int32_t set) {
	(void)set;  // ignored: the verb toggles
	if (!gr.buildings) return;
	bool all_on = true;
	for (int32_t i = 0; i < gr.num; i++) {
		Build* b = g.build(gr.who, gr.list[size_t(i)]);
		if (!b || !b->alive()) continue;
		bool valid = false;
		if (mask & bmask::kInfinite) {
			// BuildData::can_infinite: a training building with a trainable item queued.
			valid = is_training_building(g, b->type) && build_count_queue(g, *b, 2, 0) != 0;
		}
		if (mask & bmask::kRepeatAir) valid = valid || (b->build_masks & bmask::kHoldsAir);
		if (!valid) continue;
		if (!(b->build_masks & uint32_t(mask)) && all_on) {
			b->build_masks |= uint32_t(mask);
		} else {
			b->build_masks &= ~uint32_t(mask);
			all_on = false;
		}
	}
}

// ---------------------------------------------------------------- spawning (6.4)

namespace {

// ObjectsData::find_any_building_at(tx, ty, search, who, filter, ...) @659180, with search = any leader and the
// filter (0x11, production-spec 6.15) not applied: a started building, or any building of `who`, covering the tcoord.
int32_t find_any_building_at(const Game& g, int32_t tx, int32_t ty, int32_t who, int32_t* who_out) {
	if (who_out) *who_out = -1;
	const World& w = g.world;
	if (!w.in_t(tx, ty)) return -1;
	const uint16_t m = w.t(tx, ty);
	if ((m & 3) != 3 && !(m & tmask::kStarted)) return -1;
	for (int32_t k = 0; k < 9; k++) {
		const int32_t wx = move_x[k] + (tx >> 2), wy = move_y[k] + (ty >> 2);
		if (!w.in_w(wx, wy)) continue;
		int32_t o = w.w(wx, wy).down, ow = w.w(wx, wy).down_who;
		while (o >= 0) {
			const Object* ob = g.object(ow, o);
			if (!ob) break;
			const int32_t no = ob->down, nw = ob->down_who;
			if (ow < 8 && !ob->is_unit && ob->alive() && ((ob->flags & objflag::kStarted) || ow == who)) {
				const Footprint f = footprint(*ob->ptype, ob->x, ob->y);
				if (f.x0 <= tx && tx < f.x0 + f.w && f.y0 <= ty && ty < f.y0 + f.h) {
					if (who_out) *who_out = ow;
					return o;
				}
			}
			o = no;
			ow = nw;
		}
	}
	return -1;
}

}  // namespace

int32_t unit_come_out(Game& g, Unit& u0, int32_t param) {
	// ---- A
	for (Guy& gy : u0.guys) (void)gy;  // guys[g].+0xd0 = 0 (a render-side field)
	if (param == 0 && u0.o_up >= 0) {
		Unit* c = &u0;
		for (int guard = 0; c->o_up >= 0 && guard < 64; guard++) {
			Unit* p = g.unit(c->who, c->o_up);
			if (!p) break;
			c = p;
		}
		return unit_come_out(g, *c, 0);
	}
	Unit& u = u0;
	const int32_t who = u.who;
	const Type& T = *u.ptype;
	int32_t Xwho = -1;
	int32_t X = unit_get_inside(g, u, &Xwho);
	uint32_t ang = 0x80000000u;
	int32_t sx = 0, sy = 0, gp_x = 0, gp_y = 0;
	bool from_bld = false;
	int32_t gpiece = 0;
	int32_t slot = -1;
	int32_t S = 0;
	const bool captain = u.o_up < 0;
	if (X < 0) {
		// ---- B: not inside anything.
		const int32_t r0 = T.block_radius;
		if (find_nearby_spot(g, T, u.x, u.y, &sx, &sy, r0, r0 + K("unit_disembark_distance"), 0, 0x80000000u, kFilterNotMe, u.o,
		                     who, 0, 0, -1, 0, -1) != 0 &&
		    find_nearby_spot(g, T, u.x, u.y, &sx, &sy, r0, r0 + K("unit_disembark_distance"), 0, 0x80000000u, kFilterNotMe, u.o,
		                     who, 1, 0, -1, 0, -1) != 0) {
			sx = u.x;
			sy = u.y;
		}
	} else {
		Object* Xo = g.object(Xwho, X);
		if (Xo && !Xo->is_unit && type_is(*Xo->ptype, ptype::kOilPlatform) && T.domain == 0) {
			// TODO(oil platforms): the transport barge that ferries land units off an oil platform.
			return 1;
		}
		if (T.obj_masks & 0x8000000) {
			sx = Xo->x;
			sy = Xo->y;
		} else {
			if (!captain) {
				// followers are placed around their captain
				const Unit* c = &u;
				for (int guard = 0; c->o_up >= 0 && guard < 64; guard++) c = g.unit(c->who, c->o_up);
				X = c->o;
				Xwho = who;
				Xo = g.object(Xwho, X);
			}
			if (Xo && !Xo->is_unit) {
				from_bld = true;
				if (type_is(*Xo->ptype, ptype::kUniversity) || type_is(*Xo->ptype, ptype::kOilPlatform)) LD(g, who).flags |= leaderflag::kRecompute;
			}
			gpiece = 0;  // X.get_gpiece(): presentation
			const Build* XB = Xo && !Xo->is_unit ? static_cast<const Build*>(Xo) : nullptr;
			bool have_gp = false;
			if (XB && !XB->gather.empty() && !build_gather_inside(*XB)) {
				const GatherPoint& p = XB->gather.front();
				int32_t tx = p.x, ty = p.y;
				if (p.action == 3) {
					// (an object rally point: HotKeyGroups::copy is UI state, production-spec 6.15)
					const Object* t2 = (p.x >= 0 && p.y < 8) ? g.object(p.y, p.x) : nullptr;
					if (t2) {
						tx = t2->x;
						ty = t2->y;
					}
				}
				const bool br0 = T.block_radius == 0;
				gp_x = tx;
				gp_y = ty;
				if (find_nearby_spot(g, T, tx, ty, &gp_x, &gp_y, 0, 0x600, 0, 0x55555555u, br0 ? kFilterAll : kFilterNotMe, u.o, who, 0,
				                     br0 ? 1 : 0, -1, 0, -1) == 0) {
					ang = find_angle(gp_x - XB->x, gp_y - XB->y);
					u.angle = ang;
				}
				have_gp = true;
			}
			(void)have_gp;
			int32_t r0, r1;
			if (XB) {
				S = (XB->ptype->x_size + XB->ptype->y_size) * 48;
				int32_t d, m;
				if (T.domain == 1) {
					d = K("boat_train_distance");
					m = K("boat_train_max_distance");
					S += T.big_radius;
				} else {
					d = K("unit_train_distance");
					m = K("unit_train_max_distance");
				}
				r0 = (XB->flags & objflag::kValid) ? S + d : 0;
				r1 = S + m;
			} else {
				const Unit* XU = static_cast<const Unit*>(Xo);
				ang = XU ? XU->angle : 0;
				r0 = XU ? XU->ptype->block_radius : 0;
				r1 = r0 + K("unit_disembark_distance");
			}
			const int32_t px = Xo ? Xo->x : u.x, py = Xo ? Xo->y : u.y;
			if (T.block_radius == 0) {
				if (find_nearby_spot(g, T, px, py, &sx, &sy, r0, r1, 0, ang, kFilterAll, u.o, who, 0, 0, -1, 0, -1) != 0 &&
				    find_nearby_spot(g, T, px, py, &sx, &sy, 2 * r0, 2 * r1, 0, ang, kFilterAll, u.o, who, 1, 0, -1, 0, -1) != 0) {
					sx = px;
					sy = py;
				}
			} else if (find_nearby_spot(g, T, px, py, &sx, &sy, r0, r1, 0, ang, kFilterNotMe, u.o, who, 0, 0, -1, 0, -1) != 0 &&
			           find_nearby_spot(g, T, px, py, &sx, &sy, r0, r1, 0, ang, kFilterNotMe, u.o, who, 1, 0, -1, 0, -1) != 0) {
				return 1;  // stays inside
			}
		}
		// ---- D
		u.x = sx;
		u.y = sy;
		u.z = find_tcoord_z(g, tcoord_of(sx), tcoord_of(sy), 1);
		unit_remove_from_inside(g, u);
	}
	// ---- E
	unit_set_new_location(g, u, sx, sy, true, true);
	// (THECEO position: TODO.) An aircraft starts at the ground; a helicopter 500 above it.
	if (T.domain == kDomainAir && !u.guys.empty()) {
		guy_update_z(g, u, u.guys[0]);
		if (T.unit_flags & utflag::kHelicopter) {
			u.guys[0].z += 500;
			u.guys[0].last_z = u.guys[0].z;
		}
	}
	if (u.o_down >= 0) {
		Unit* f = g.unit(who, u.o_down);
		if (f && f->alive()) unit_come_out(g, *f, 1);
	}
	if (u.type == 52 || u.type == 53) unit_set_anim(g, u, kCharDefault, 1, 1);
	if (!(LD(g, who).flags & leaderflag::kHuman) && !(unit_is_plane(u) && T.domain == 2 && !(T.unit_flags & 0x20)) &&
	    !(u.type >= 50 && u.type <= 53)) {
		u.unit_masks &= ~umask::kMultimove;
		u.path.clear();
		close_orders(g, u, false);
		clear_partial_path(u);
		update_action(g, u);
	}
	// ---- F: rally points (captain only; followers take the captain's angle)
	Object* Xo = X >= 0 ? g.object(Xwho, X) : nullptr;
	if (Xo && Xo->is_unit) unit_set_angle(g, u, static_cast<Unit*>(Xo)->angle, true);
	if (captain && Xo && !Xo->is_unit) {
		Build& XB = *static_cast<Build*>(Xo);
		if (T.uber_size > 1) {
			Group tmp;
			group_clear(tmp, -1);
			group_add(g, tmp, u.o, who, 0, 0);
			slot = groups_push_group(g, who, tmp, 1);
		}
		// TODO(economy): the city alarm flag reset (CITY_ALARM with no worker garrisoned).
		bool final_pt = false;
		int32_t fx = 0, fy = 0, fact = 0, prevx = sx, prevy = sy;
		if (!XB.gather.empty() && !build_gather_inside(XB) && u.order_type() == kOrderNone) {
			const int32_t n = int32_t(XB.gather.size());
			for (int32_t i = 0; i < n; i++) {
				const GatherPoint& p = XB.gather[size_t(i)];
				int32_t qx, qy;
				if (i >= 1) {
					if (find_nearby_spot(g, T, p.x, p.y, &qx, &qy, 0, 0x600, 0, 0x55555555u, kFilterNotMe, u.o, who, 0, 0, -1, 0, -1) != 0)
						continue;
				} else {
					qx = gp_x;
					qy = gp_y;
				}
				if (i == n - 1) {
					final_pt = true;
					fx = p.x;
					fy = p.y;
					fact = p.action;
					break;
				}
				bool special = false, skip = false;
				if (p.action != 0) {
					int32_t bw = -1;
					const int32_t bo = find_any_building_at(g, tcoord_of(p.x), tcoord_of(p.y), who, &bw);
					if (bo >= 0) {
						const Build* bb = g.build(bw, bo);
						const bool peasant = u.type == 50 || u.type == 51;
						if (peasant && p.action == 1 && bw == who) {
							if (!(bb->flags & objflag::kActive)) special = true;
							else if (bb->damage != 0) special = true;
							else if (is_gather_type(*bb->ptype) && !type_is(*bb->ptype, ptype::kUniversity)) special = true;  // TODO: num_gatherers < gather_max
							else skip = true;
						}
						if ((bb->flags & objflag::kActive) && is_ally(g, who, bw) && get_garrison_limit(g, bb->type, bw) != 0 &&
						    type_can_garrison(g, T, bb->type) && p.action == 1) {
							if (num_inside(g, *bb, 0) + unit_control_cost(u) <= get_garrison_limit(g, bb->type, bw)) special = true;
							else skip = true;
						}
						if (T.attack != 0 && !(u.type >= 50 && u.type <= 53) && is_enemy(g, who, bw) && p.action == 2) special = true;
						if (skip && !special) continue;
						if (special) {
							final_pt = true;
							fx = p.x;
							fy = p.y;
							fact = p.action;
							break;
						}
					} else if (T.attack != 0 && !(u.type >= 50 && u.type <= 53) && p.action == 2) {
						continue;
					}
				}
				if (T.uber_size > 1 && slot >= 0) {
					int32_t q2x = qx, q2y = qy;
					find_nearby_spot(g, T, qx, qy, &q2x, &q2y, 0, -1, 0, 0x55555555u, kFilterNotMe, u.o, who, 0, 1, -1, 0, -1);
					group_action_move_to(g, g.groups.g[size_t(slot)], q2x, q2y, kQueueLast, true,
					                     find_angle(q2x - prevx, q2y - prevy), 1, 1, -1, -1, 0);
				} else {
					add_move_facing_order(g, u, qx / 48, qy / 48, find_angle(qx - prevx, qy - prevy), 1, false, kQueueLast, true, -1, -1,
					                      -1, false);
				}
				prevx = qx;
				prevy = qy;
			}
		}
		// ---- G
		if (final_pt) {
			int32_t bw = -1;
			const int32_t bo = find_any_building_at(g, tcoord_of(fx), tcoord_of(fy), who, &bw);
			auto relocate = [&](int32_t tx, int32_t ty) {
				int32_t rx, ry;
				if (find_nearby_spot(g, T, XB.x, XB.y, &rx, &ry, S + K("unit_train_distance"), S + K("unit_train_max_distance"), 0,
				                     find_angle(tx - XB.x, ty - XB.y), kFilterAll, u.o, who, 0, 0, -1, 0, -1) == 0)
					unit_set_new_location(g, u, rx, ry, true, true);
			};
			bool done = false;
			if (bo >= 0) {
				Build& bb = *g.build(bw, bo);
				relocate(bb.x, bb.y);
				if (u.type >= 50 && u.type <= 53) {
					if (u.type <= 51) {
						if (!(bb.flags & objflag::kActive) && bw == who) {
							add_build_order(g, u, bo, bw, kQueueLast, false);
							final_pt = false;
						} else if (bb.damage != 0) {
							add_repair_order(g, u, bo, bw, kQueueLast, false);
							final_pt = false;
						} else if (fact && bb.alive() && is_gather_type(*bb.ptype) && !type_is(*bb.ptype, ptype::kUniversity) &&
						           bw == who) {
							add_gather_order(g, u, bo, kQueueLast, true);
							final_pt = false;
						}
					} else if (fact && bb.alive() && is_gather_type(*bb.ptype) && type_is(*bb.ptype, ptype::kUniversity) &&
					           bw == who) {
						add_gather_order(g, u, bo, kQueueLast, true);
						final_pt = false;
					}
				}
				if ((T.unit_flags2 & 8) && (bb.flags & objflag::kCity) && !is_enemy(g, who, bw)) {
					add_trade_order(g, u, bo, bw, -1, -1, kQueueLast, false);  // (vslot +0x24 is_trade: a city centre)
					final_pt = false;
				} else if (final_pt && bb.alive() && !bb.is_unit && (bb.flags & objflag::kActive) && is_ally(g, who, bw) &&
				         get_garrison_limit(g, bb.type, bw) != 0 && type_can_garrison(g, T, bb.type) && fact != 0) {
					for (Unit* m = &u; m; m = m->o_down >= 0 ? g.unit(who, m->o_down) : nullptr) {
						add_garrison_order(g, *m, bo, bw, 0, kQueueLast, false);
						if (m->o_down == m->o) break;
					}
					final_pt = false;
				}
				if (T.attack != 0 && is_enemy(g, who, bw)) {
					if (fact != 0) {
						for (Unit* m = &u; m; m = m->o_down >= 0 ? g.unit(who, m->o_down) : nullptr) {
							add_attack_order(g, *m, bo, bw, kQueueLast, 1, true);
							if (m->o_down == m->o) break;
						}
					} else if (get_combat_stance(g, u) == kHoldFire) {
						if (slot >= 0) group_action_move_to(g, g.groups.g[size_t(slot)], fx, fy, kQueueNew, false, 0, 1, 0, -1, -1, 0);
						else add_move_order(g, u, fx, fy, 1, false, kQueueNew, false, -1, -1);
					} else if (!type_is_siege(T)) {
						if (slot >= 0) group_action_move_to(g, g.groups.g[size_t(slot)], fx, fy, kQueueNew, false, 0, 2, 0, -1, -1, 0);
						else if (T.uber_size <= 1) add_move_order(g, u, fx, fy, 2, false, kQueueNew, false, -1, -1);
						else
							for (Unit* m = &u; m; m = m->o_down >= 0 ? g.unit(who, m->o_down) : nullptr) add_attack_order(g, *m, bo, bw, kQueueLast, 1, false);
					} else {
						for (Unit* m = &u; m; m = m->o_down >= 0 ? g.unit(who, m->o_down) : nullptr) add_attack_order(g, *m, bo, bw, kQueueLast, 1, false);
					}
					done = true;
				}
			}
			if (!done && final_pt) {
				int32_t uw = -1;
				const int32_t uo = find_unit_with_radius(g, fx, fy, -1, 1, 0, u.o, who, &uw);
				bool attacked = false;
				if (uo >= 0) {
					const Unit* tu = g.unit(uw, uo);
					if (tu) relocate(tu->x, tu->y);
					if (T.attack != 0 && is_enemy(g, who, uw)) {
						for (Unit* m = &u; m; m = m->o_down >= 0 ? g.unit(who, m->o_down) : nullptr)
							add_attack_order(g, *m, uo, uw, kQueueLast, fact ? 1 : 0, fact != 0);
						attacked = true;
					}
				}
				if (!attacked) {
					int32_t orders = 1;
					if (T.attack != 0 && get_combat_stance(g, u) != kHoldFire && T.where >= 0 &&
					    (is_t(g, T.where, ptype::kBarracks) || is_t(g, T.where, ptype::kStable) || is_t(g, T.where, ptype::kDock)))
						orders = 2;
					if (T.uber_size > 1 && slot >= 0) {
						int32_t qx = fx, qy = fy;
						find_nearby_spot(g, T, fx, fy, &qx, &qy, 0, -1, 0, 0x55555555u, kFilterNotMe, u.o, who, 0, 1, -1, 0, -1);
						group_action_move_to(g, g.groups.g[size_t(slot)], qx, qy, kQueueLast, true, find_angle(qx - prevx, qy - prevy), orders,
						                     1, -1, -1, 0);
					} else {
						relocate(fx, fy);
						add_move_facing_order(g, u, fx / 48, fy / 48, find_angle(fx - u.x, fy - u.y), orders, false, kQueueLast, true, -1,
						                      -1, -1, false);
					}
				}
			}
		}
	}
	// ---- H
	if (from_bld && g.frame != 0) {
		// Unit::add_spec_anim_order(1, gpiece, 0) with the container's position and (o, who), then do_spec_anim: the walk-out
		// order is killed in the same call, except that a plane leaving an airbase appears 0xc0 west of it (aircraft.cpp).
		const Object* Xc = g.object(Xwho, X);
		Order* sa = add_spec_anim_order(g, u, 1, Xwho, 0);
		if (Xc) {
			sa->sa_data3 = Xc->x;
			sa->sa_data4 = Xc->y;
			sa->ox = Xc->o;
			sa->whom = Xc->who;
		}
		do_spec_anim(g, u, *sa);
	}
	(void)gpiece;
	if ((u.unit_masks & umask::kAiControl) && !(T.unit_flags2 & 8) && u.type != ptype::kMerchant &&
	    u.type != ptype::kMerchantDutch && u.type != ptype::kFurTrapper) {
		bool join = false;
		if ((T.unit_flags2 & 0x10) || unit_is(g, u, ptype::kBark)) {
			const uint32_t v = g.rng.get(0, 0xffff);  // @61a1bb / @61a1d5
			join = unit_is(g, u, ptype::kBark) ? (g.frame & int32_t(v % 3)) != 0 : (g.frame & int32_t(v % 2)) != 0;
		} else {
			join = unit_is(g, u, ptype::kSpy);
		}
		if (join) unit_add_to_army(g, u);  // Unit::add_to_army @5f7740 (ai-spec 12.3)
	}
	return 0;
}

int32_t build_train(Game& g, Build& b, int32_t t) {
	const int32_t who = b.who;
	const int32_t uo = init_unit(g, who, t, b.x, b.y, -1, -1, -1);
	if (uo < 0) return uo;
	if (who < 8) g.scen.last_unit_built_o[size_t(who)] = uo;  // ScenarioData::last_unit_built_o
	LD(g, who).last_unit_finished[size_t(t - kUnits)] = int16_t(uo);
	Unit& U = *g.unit(who, uo);
	if (unit_stance_type(*U.ptype) == build_stance_type(g, b.type)) unit_set_stance(g, U, b.stance);
	unit_go_inside(g, U, b.o, who);
	if (b.ptype->obj_masks & 0x200) {
		// Aircraft-housing buildings: the airbase's gather points become patrols / strafes / rebasing (aircraft.cpp).
		const int32_t r = build_train_air(g, b, U);
		if (r == 1) return uo;
		if (r == 2) return -1;
	} else {
		int32_t lim = get_garrison_limit(g, b.type, who);
		if (lim == 0) lim = 10;
		if (type_is(*b.ptype, ptype::kUniversity)) {
			// Scholars stay inside up to gather_max (Build::check_gatherers): TODO(economy). Everything else leaves.
			if (!(t == 52 || t == 53)) unit_come_out(g, U, 0);
		} else {
			// BuildData::gather_inside with room keeps the unit garrisoned; a unit that cannot leave stays, unless the
			// building is over its garrison limit, which kills it.
			if (build_gather_inside(b) && num_inside(g, b, 0) <= lim && !unit_is(g, U, ptype::kCarrier, true)) {
				// stays inside
			} else if (unit_come_out(g, U, 0) != 0 && num_inside(g, b, 0) > lim) {
				unit_die(g, U, 0);
				return -1;
			}
		}
	}
	U.unit_masks &= ~umask::kMultimove;
	if (unit_is(g, U, ptype::kCarrier, true)) carrier_new_wing(g, U);  // a new carrier comes with a full wing
	return uo;
}

// ---------------------------------------------------------------- per frame (6.3)

int32_t build_finished(Game& g, Build& b, int32_t t) {
	const int32_t who = b.who;
	Leader& l = LD(g, who);
	const bool trainable = is_unit_type(t) && l.has_tech_bit(t);
	if (trainable) {
		if (l.pop_cap < g.type(t).control_cost + l.control) return 0;
		if ((g.type(t).unit_flags2 & 8) && get_units(g, who, t, 0) >= get_caravan_limit(g, who, 0)) return -1;
		// An airbase holds MAX_AIRCRAFT_PER_AIRBASE planes (helicopters are exempt): the item waits in the queue.
		if (type_is(*b.ptype, ptype::kAirbase) && !(g.type(t).unit_flags & 0x20) &&
		    num_aircraft_here(g, b, 0) >= num_aircraft_limit(g, b))
			return -1;
		build_train(g, b, t);
		return 1;
	}
	if (is_spell_type(t) && l.has_tech_bit(t)) {
		// SpellType::cast(o, who): TODO(spells). DISBAND demolishes the building.
		if (t == ptype::kDisband) {
			if (b.alive() && (b.flags & objflag::kActive)) build_plunder(g, b, who);  // SpellType::cast_disband @671ed0
			build_disband(g, b, false);
		}
		return 1;
	}
	if (is_build_type(t) && !(g.type(t).build_flags & 4)) {
		const int32_t old = b.city >= 0 ? city_get_pop_value(g.build(who, g.cities[size_t(who)][size_t(b.city)].o)->type) : 0;
		wall_set_type(g, b, t, 0);
		wall_mask_me(g, b, true, Regen::None);
		l.flags |= leaderflag::kCalcWallStats;
		if (!(b.flags & objflag::kCity)) return 1;
		city_find_buildings(g, who, b.city);
		const int32_t d = city_get_pop_value(b.type) - old;
		l.pop += d;
		g.world_pop += d;
		const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
		const int32_t reg = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : 64;
		if (reg < 64) l.reg_pop[size_t(reg)] = int16_t(l.reg_pop[size_t(reg)] + d);
		calc_pop_cap(g, who);
		regions_fix_all_borders(g);
		return 1;
	}
	gain_tech(g, who, t, b.x, b.y, true, true);
	// Build::finished @628490: a finished Senate trains the new government's hero, or converts the living one.
	if (type_is(*b.ptype, ptype::kSenate)) {
		const int32_t h = get_gov_hero(g, who);
		if (h >= 0) {
			if (l.gov_hero_frame < 0) {
				build_train(g, b, h);
				return 1;
			}
			const int32_t uo = find_gov_hero_unit(g, who, b.x, b.y);
			if (uo >= 0) unit_set_type(g, *g.unit(who, uo), h, 0);
		}
	}
	return 1;
}

void build_do_queue(Game& g, Build& b, int32_t slot) {
	if (b.queued == 0) return;
	const int32_t who = b.who;
	Leader& l = LD(g, who);
	const int32_t t = slot < b.build_queue.queue_size ? b.build_queue.queue[size_t(slot)].type : -1;
	if (type_is(*b.ptype, ptype::kLibrary) && get_first_library(g, who) != b.o && t != ptype::kDisband) return;
	if (type_is(*b.ptype, ptype::kMissileSilo, true) && b.inside_down >= 0 && l.has_tech_bit(t) && t != ptype::kDisband) return;
	const int32_t tt = train_time(g, b.o, who, t);
	int32_t acc;
	const bool trainable = is_unit_type(t) && l.has_tech_bit(t);
	if (is_build_type(t) || t == ptype::kDisband) {
		acc = K("accel_construct");
	} else if (trainable) {
		acc = K("accel_train");
		if (check_population(g, who, t)) l.pop_issues++;
	} else {
		acc = K("accel_research");
	}
	if (g.ai_speed > 1) acc *= g.ai_speed;
	const int32_t jc = slot < b.build_queue.queue_size ? b.build_queue.queue[size_t(slot)].job_counter : -1;
	const int32_t eff = tt == 1 ? 1 : jc;
	const bool done = eff >= tt;
	const int32_t nv = std::min(eff + acc, tt);
	if (type_is(*b.ptype, ptype::kLibrary)) {
		if (slot + 1 < get_building_cities(g, who) && slot + 1 < b.queued) build_do_queue(g, b, slot + 1);
		if (done) {
			if (build_finished(g, b, t) && (b.flags & objflag::kValid)) build_unqueue(g, b, slot, false);
			return;
		}
		b.build_queue.queue[size_t(slot)].job_counter = nv;
		return;
	}
	if (!done) {
		b.build_queue.queue[size_t(slot)].job_counter = nv;
		return;
	}
	b.build_queue.queue[size_t(slot)].job_counter = tt;
	const int32_t r = build_finished(g, b, t);
	if (r > 0) {
		if (!(b.flags & objflag::kValid)) return;  // (a DISBAND demolished the building)
		const bool repeat = (b.build_masks & bmask::kInfinite) != 0;
		build_unqueue(g, b, slot, false);
		if (trainable && repeat) {
			b.build_masks &= ~bmask::kInfinite;
			if (build_queue_up(g, b, t, 0) == 0) b.build_masks |= bmask::kInfinite;
		}
		return;
	}
	if (slot != 0) return;
	auto non_unit = [&]() {
		for (int32_t i = 1; i < std::min<int32_t>(b.queued, b.build_queue.queue_size); i++) {
			const int32_t v = b.build_queue.queue[size_t(i)].type;
			if (!is_unit_type(v) || !has_tech(g, who, v)) return i;
		}
		return -1;
	};
	int32_t i = non_unit();
	if (i > 0) {
		build_do_queue(g, b, i);
		return;
	}
	if (b.queued < 2 || r >= 0) return;
	i = -1;
	for (int32_t k = 1; k < std::min<int32_t>(b.queued, b.build_queue.queue_size); k++) {
		const int32_t v = b.build_queue.queue[size_t(k)].type;
		const bool hit = type_is(*b.ptype, ptype::kAirbase) ? (v >= 0 && type_is(g.type(v), ptype::kHelicopter)) : v != ptype::kCara;
		if (hit) {
			i = k;
			break;
		}
	}
	if (i > 0) build_do_queue(g, b, i);
}

}  // namespace ron::sim
