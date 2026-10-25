// Building sites, builders and repair, and the per-frame building pass (docs/production-spec.md sections 3.1-3.4, 4,
// 8): Group::action_build / action_swarm_around, Unit::add_build_order / add_repair_order, Unit::do_build /
// do_repair / check_build_order / build_done / find_build_spot, Wall::do_construct, Wall::process,
// Build::process, Wall::inc_time, Group::action_disband.
#include <algorithm>

#include "sim/aircraft.hpp"
#include "sim/heroes.hpp"
#include "sim/roads.hpp"
#include "sim/capture.hpp"
#include "sim/combat.hpp"
#include "sim/combat_internal.hpp"
#include "sim/economy.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/production_internal.hpp"
#include "sim/search.hpp"
#include "sim/spells.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using prod::is_t;
using prod::tb;

namespace {

Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }

// UnitAnim values of the builders (CHAR_BUILD 0x21, CHAR_REPAIR 0x22, CHAR_SOW 0x23).
constexpr int32_t kCharBuild = 0x21, kCharRepair = 0x22, kCharSow = 0x23;

// GroupData::is_on_map @70c450 (groups.cpp keeps its copy file-local).
bool group_on_map(Game& g, const Group& gr) {
	if (gr.num == 0) return false;
	if (gr.buildings) return true;
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* ob = g.object(gr.who, gr.list[size_t(i)]);
		if (ob && ob->is_unit && ob->alive() && static_cast<const Unit*>(ob)->o_up < 0 &&
		    static_cast<const Unit*>(ob)->on_map())
			return true;
	}
	return false;
}

// UnitData::get_worker_stance (vslot 0xf8): the unit's stance if its type has the WORKER stance type, else 0.
int32_t worker_stance(const Unit& u) { return unit_stance_type(*u.ptype) == kStanceWorker ? u.stance : 0; }

// UnitData::is_busy @60a370: the current order is a PACK / UNPACK cast, or the unit is entering / leaving a building (packers.cpp).
bool unit_is_busy(const Unit& u) { return cast_is_busy(u); }

bool is_peasant_type(int32_t t) { return t == ptype::kPeasants || t == ptype::kPeasantsKorean; }

// GroupData::count(COUNT_PEASANTS 1 / COUNT_GATHER_PEASANTS 0xb / COUNT_BUILD_PEASANTS 0xe).
// TODO(spec): the counting rules are taken from the names (citizen members; with a GATHER / BUILD_AT action).
int32_t group_count_peasants(const Game& g, const Group& gr, int32_t kind) {
	int32_t n = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* ob = g.object(gr.who, gr.list[size_t(i)]);
		if (!ob || !ob->is_unit || !ob->alive() || !is_peasant_type(ob->type)) continue;
		const Unit& u = static_cast<const Unit&>(*ob);
		if (kind == 1) n++;
		else if (kind == 0xb) n += action_type(u) == kGather ? 1 : 0;
		else if (kind == 0xe) n += action_type(u) == kBuildAt ? 1 : 0;
	}
	return n;
}

// Objects::find_units(x, y, SEARCH_FRIENDLY, who, radius, 0x200, FILTER_BUILDREPAIR, 1, 0, _, 1) @65a620
// (combat-spec 2.17.2): cell mode or dense mode, own units only, same tregion as (x, y). The BUILDREPAIR filter is
// not decoded (❓); every caller here only counts units whose action is BUILD_AT, which the filter cannot add.
std::vector<const Unit*> find_units_friendly(const Game& g, int32_t x, int32_t y, int32_t who, int32_t radius) {
	std::vector<const Unit*> out;
	const World& w = g.world;
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	const int32_t reg = w.in_t(tx, ty) ? w.get_tregion(tx, ty) : -1;
	auto keep = [&](const Object* ob) {
		if (!ob || !ob->is_unit || !ob->alive() || ob->who != who) return false;
		const Unit& u = static_cast<const Unit&>(*ob);
		if (!u.on_map()) return false;
		const int32_t ux = tcoord_of(u.x), uy = tcoord_of(u.y);
		return w.in_t(ux, uy) && w.get_tregion(ux, uy) == reg;
	};
	const Circles& C = circles();
	const int32_t n = (radius + 767) / 768;
	if (radius >= 0 && n <= 64 && C.radius[size_t(n)] <= g.total_units) {
		const int32_t cx = x / 768, cy = y / 768;
		for (int32_t i = 0; i < C.radius[size_t(n)]; i++) {
			const int32_t wx = cx + C.x[size_t(i)], wy = cy + C.y[size_t(i)];
			if (!w.in_w(wx, wy)) continue;
			int32_t o = w.w(wx, wy).down, ow = w.w(wx, wy).down_who;
			while (o >= 0) {
				const Object* ob = g.object(ow, o);
				if (!ob) break;
				const int32_t no = ob->down, nw = ob->down_who;
				if (ow < 8 && keep(ob)) out.push_back(static_cast<const Unit*>(ob));
				o = no;
				ow = nw;
			}
		}
		return out;
	}
	for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
		const Unit* u = g.unit(who, o);
		if (!u || !keep(u)) continue;
		const int32_t d = vector_dist(iabs(u->x - x), iabs(u->y - y)) -
		                  (u->ptype->domain == 1 ? u->ptype->push_size : u->ptype->big_radius);
		if (radius < 0 || d <= radius) out.push_back(u);
	}
	return out;
}

// Objects::find_builds(x, y, SEARCH_FRIENDLY, who, radius, 0x200, FILTER_CONSTRUCT, 0, _, 0) @65a120. The 0x200 flag restricts to
// the centre wcoord's WData.region (not the tregion). Cell mode (radius >= 0, circle index <= num_def_builds = 200): the circle's
// wcoords in table order, each cell's down-chain, no distance test. Dense mode: the leader's buildings in slot order, within
// vector_dist of the centre. FILTER_CONSTRUCT is taken as an unfinished (not active) own site.
std::vector<Build*> find_builds_construct(Game& g, int32_t x, int32_t y, int32_t who, int32_t radius) {
	std::vector<Build*> out;
	const World& w = g.world;
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	const int32_t reg = w.in_w(wx, wy) ? w.w(wx, wy).region : -1;
	auto ok = [&](Build* b) { return b && b->alive() && !(b->flags & objflag::kActive); };
	const Circles& C = circles();
	const int32_t k = (radius + 0x2ff) / 0x300;
	if (radius >= 0 && k <= 64 && C.radius[size_t(k)] <= 200) {
		for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
			const int32_t cx = wx + C.x[size_t(i)], cy = wy + C.y[size_t(i)];
			if (!w.in_w(cx, cy)) continue;
			const WData& d = w.w(cx, cy);
			if (d.down < 0 || d.region != reg) continue;
			int32_t co = d.down, cw = d.down_who;
			while (co >= 0) {
				const Object* ob = (cw >= 0 && cw < 8) ? g.object(cw, co) : nullptr;
				const int32_t no = ob ? ob->down : -1, nw = ob ? ob->down_who : 0;
				if (cw == who && co >= kBuildBase) {
					Build* b = g.build(cw, co);
					if (ok(b)) out.push_back(b);
				}
				co = no;
				cw = nw;
			}
		}
		return out;
	}
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		Build* b = g.build(who, o);
		if (!ok(b)) continue;
		const int32_t bx = wcoord_of(b->x), by = wcoord_of(b->y);
		if (!w.in_w(bx, by) || w.w(bx, by).region != reg) continue;
		if (radius < 0 || vector_dist(iabs(b->x - x), iabs(b->y - y)) <= radius) out.push_back(b);
	}
	return out;
}

// Units of the list whose action is BUILD_AT on (o, who).
int32_t count_builders(const std::vector<const Unit*>& units, int32_t self_o, int32_t site_o, int32_t who) {
	int32_t n = 0;
	for (const Unit* u : units) {
		if (u->o == self_o) continue;
		const Order* a = get_action(*u);
		if (a && a->type == kBuildAt && a->whom == who && a->ox == site_o) n++;
	}
	return n;
}

// Object::adjacent_to(ox, whom) @651f40 (garrison.cpp).
bool adjacent_to(const Game& g, const Unit& u, int32_t ox, int32_t whom) { return object_adjacent_to(g, u, ox, whom); }

// WallData::covers_tile(tx, ty) @6439b0: the footprint from the corner tile.
bool covers_tile(const Build& b, int32_t tx, int32_t ty) {
	const Footprint f = footprint(*b.ptype, b.x, b.y);
	return tx >= f.x0 && tx < f.x0 + f.w && ty >= f.y0 && ty < f.y0 + f.h;
}

Order* add_target_order(Game& g, Unit& u, OrderType t, int32_t ox, int32_t whom, QueuePos q, bool expl) {
	if (q == kQueueNew) clear_orders(g, u);
	u.unit_masks |= umask::kBuilder;
	Order o;
	o.type = t;
	o.ox = ox;
	o.whom = whom;
	const Object* tg = g.object(whom, ox);
	o.uid = tg ? tg->uid : 0xffff;
	if (expl) o.flags |= orderflag::kExplicit;
	// TODO(transports): UNIT_CAN_TRANSPORT when the target's tregion differs and the leader's transport level allows
	// (REPAIR also needs a land target tile).
	return add_order(g, u, o, q == kQueueNew ? kQueueLast : q);
}

void swarm_one(Game& g, Unit& u, int32_t ox, int32_t whom, OrderType order, bool expl) {
	Group tmp;
	group_clear(tmp, -1);
	group_add(g, tmp, u.o, u.who, 0, 0);
	group_action_swarm_around(g, tmp, ox, u.who, kQueueFirst, order, expl);
	(void)whom;
}

int32_t find_build_spot(Game& g, Unit& u);
int32_t find_repair_spot(Game& g, Unit& u);

// Unit::build_done(ox, whom, range) @603bf0.
void build_done(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t range) {
	(void)range;
	if (u.orders.size() > 1) return;
	if (u.unit_masks & umask::kAiControl) {
		if (find_build_spot(g, u)) return;
		if (find_repair_spot(g, u)) return;
		if (g.info.starting_resources != 8) find_gather_spot(g, u, range);
		return;
	}
	const int32_t st = worker_stance(u);
	if ((st == 1 || st == 2) && find_build_spot(g, u)) return;
	if (st == 1 || st == 0) {
		if (find_gather_spot(g, u, K("unit_gather_respond_range") * 0xc0)) {
			u.group = -1;  // (and the local selection drops it)
			return;
		}
	}
	if ((st == 1 || st == 2) && find_repair_spot(g, u)) return;
	if (ox >= 0 && whom == u.who) {
		Build* b = g.build(whom, ox);
		if (b && b->alive() && is_gather_type(*b->ptype) && !type_is(*b->ptype, ptype::kUniversity, true) &&
		    !type_is(*b->ptype, ptype::kOilPlatform, true) && build_num_gatherers(g, *b, false, false) < b->gather_max)
			add_gather_order(g, u, ox, kQueueNew, false);
	}
}

// Unit::check_build_order @603470: walks the queue from the front over MOVE_TO / BUILD_AT orders.
void check_build_order(Game& g, Unit& u) {
	if (u.orders.empty()) return;
	std::vector<int32_t> sites;
	size_t it = 0;
	while (it < u.orders.size()) {
		const Order& o = u.orders[it];
		if (o.type != kBuildAt && o.type != kMoveTo) break;
		if (o.type == kMoveTo) {
			if ((o.flags & orderflag::kExplicit) || it + 1 == u.orders.size()) break;
			it++;
			continue;
		}
		const Object* t = g.object(o.whom, o.ox);
		bool stop = false;
		if (o.ox >= 0 && o.whom >= 0 && t && t->alive() && t->uid == o.uid && !(t->flags & objflag::kActive)) {
			sites.push_back(t->o);
			if ((t->flags & objflag::kCity) || type_is(*t->ptype, ptype::kFortX) || type_is(*t->ptype, ptype::kTower)) stop = true;
		}
		if (stop) break;
		// (the front order is killed, whatever `it` points at; the walk restarts at the front)
		repath(g, u);
		kill_current_order(g, u, false);
		if (u.orders.empty()) break;
		it = 0;
	}
	if (sites.empty()) return;
	if (sites.size() == 1) {
		add_build_order(g, u, sites[0], u.who, kQueueFirst, true);
		return;
	}
	const auto units = find_units_friendly(g, u.x, u.y, u.who, K("unit_build_respond_range") * 0xc0);
	int32_t best = 999999, bi = 0;
	for (size_t i = 0; i < sites.size(); i++) {
		const int32_t n = count_builders(units, u.o, sites[i], u.who);
		if (n < best) {
			best = n;
			bi = int32_t(i);
		}
	}
	std::swap(sites[0], sites[size_t(bi)]);
	for (int32_t i = int32_t(sites.size()) - 1; i >= 0; i--) add_build_order(g, u, sites[size_t(i)], u.who, kQueueFirst, true);
}

// Unit::find_build_spot @603e20.
int32_t find_build_spot(Game& g, Unit& u) {
	int32_t R = K("unit_build_respond_range") * 0xc0;
	const int32_t st = worker_stance(u);
	if (st == 1 || st == 2) R = K("unit_build_respond_range") * 0x180;
	std::vector<int32_t> cand;
	for (Build* b : find_builds_construct(g, u.x, u.y, u.who, R)) {
		if (!(b->build_masks & bmask::kUnderAttack) && b->who == u.who) cand.push_back(b->o);
	}
	if (cand.empty()) return 0;
	const auto units = find_units_friendly(g, u.x, u.y, u.who, R);
	int32_t best = 999999, bi = 0;
	for (size_t i = 0; i < cand.size(); i++) {
		const int32_t n = count_builders(units, -1, cand[i], u.who);
		if (n < best) {
			best = n;
			bi = int32_t(i);
		}
	}
	Group tmp;
	group_clear(tmp, -1);
	group_add(g, tmp, u.o, u.who, 0, 0);
	group_action_swarm_around(g, tmp, cand[size_t(bi)], u.who, kQueueLast, kBuildAt, false);
	return 1;
}

// Unit::find_repair_spot @604320 🟡. TODO(spec 4.7): ObjectsData::find_any_building with FILTER_DAMAGED and the
// territory test are not ported; nothing is auto-repaired.
int32_t find_repair_spot(Game& g, Unit& u) {
	(void)g;
	(void)u;
	return 0;
}

}  // namespace

// ---------------------------------------------------------------- orders

Order* add_build_order(Game& g, Unit& u, int32_t ox, int32_t whom, QueuePos q, bool expl) {
	return add_target_order(g, u, kBuildAt, ox, whom, q, expl);
}

Order* add_repair_order(Game& g, Unit& u, int32_t ox, int32_t whom, QueuePos q, bool expl) {
	return add_target_order(g, u, kRepair, ox, whom, q, expl);
}

// ---------------------------------------------------------------- BUILD (3.1) and SWARM_AROUND (3.4)

void group_action_build(Game& g, Group& gr, int32_t x, int32_t y, int32_t type, QueuePos q) {
	if (!group_on_map(g, gr)) return;
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	const int32_t who = gr.who;
	if (!is_build_type(type)) return;
	const Type& T = g.type(type);
	// GroupData::validate_build @708620.
	{
		int32_t sx = x, sy = y;
		snap_center_who(g, type, x, y, &sx, &sy, who);
		if (blocked_site(g, type, sx, sy, who, -1, nullptr) != 0) return;  // (console: invalid location)
		const int32_t c = (T.build_flags & (0x2000 | 0x10)) ? -1 : find_city_at(g, sx, sy, who, -1, 0);
		if (!can_pay_cost(g, type, who, c, -1, 1)) return;  // (console: warn_resources)
	}
	int32_t valid = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* ob = g.object(who, gr.list[size_t(i)]);
		if (ob && ob->alive()) valid++;
	}
	if (valid <= 0) return;
	if (is_city_type(T) && type != ptype::kForbiddenCity && get_total_cities(g, who) >= get_city_limit(g, who)) return;
	gr.disband = 0;  // action_begin
	if (type_avail(g, who, type, 1) != 4) return;
	int32_t sx = x, sy = y;
	snap_center_who(g, type, x, y, &sx, &sy, who);
	if (blocked_site(g, type, sx, sy, who, -1, nullptr) != 0) return;
	const int32_t city = (T.build_flags & (0x2000 | 0x10)) ? -1 : find_city_at(g, sx, sy, who, -1, 0);
	if (!g.editor) {  // (ScenarioData::buildings_free: TODO(scenarios))
		if (!can_pay_cost(g, type, who, city, -1, 1)) return;
		pay_cost(g, type, who, -1, city, 1, nullptr);
	}
	const int32_t o = init_build_site(g, who, type, sx, sy, false, -1);
	if (o < 0) return;  // no free slot: the cost stays paid
	group_action_swarm_around(g, gr, o, who, q, kBuildAt, true);
}

void group_action_swarm_around(Game& g, Group& gr, int32_t ox, int32_t whom, QueuePos q, int32_t order, bool expl) {
	Group tmp;
	group_clear(tmp, -1);
	if (!group_on_map(g, gr)) return;
	Object* T = g.object(whom, ox);
	if (!T || !T->alive()) return;
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	gr.disband = 0;  // action_begin
	const int32_t who = gr.who;
	if (q == kQueueFirst) {
		std::vector<Order> saved = group_set_up_insert(g, gr);
		group_action_halt(g, gr, 0);
		group_action_swarm_around(g, gr, ox, whom, kQueueNew, order, expl);
		group_finish_insert(g, gr, saved);
		return;
	}
	gr.form = -1;
	group_clear(tmp, -1);
	int32_t kind = 1;
	int32_t skip_gatherers = 0;
	if (order == kBuildAt) {
		kind = (LD(g, who).flags & leaderflag::kHuman) ? 1 : 3;
		if (!T->is_unit && is_gather_type(*T->ptype))
			skip_gatherers = group_count_peasants(g, gr, 1) - group_count_peasants(g, gr, 0xb) - group_count_peasants(g, gr, 0xe);
	}
	const int32_t cx = T->x, cy = T->y;
	int32_t R;
	if (T->is_unit) R = T->ptype->big_radius;
	else R = std::min(T->ptype->x_size, T->ptype->y_size) * 0x60 + 0x30;
	if (type_is(*T->ptype, ptype::kFarm) && order == kBuildAt) R /= 2;
	for (int32_t pass = 0; pass < 2; pass++) {
		for (int32_t i = 0; i < gr.num; i++) {
			const int32_t mo = gr.list[size_t(i)];
			Unit* u = g.unit(who, mo);
			if (!u || !u->alive() || !u->on_map()) continue;
			const int32_t dom = u->ptype->domain;
			if (dom != pass || dom == 2) continue;
			if (skip_gatherers != 0 && action_type(*u) == kGather) continue;
			if (unit_is_busy(*u)) continue;
			// An armed citizen (a militia) casts CIVILIAN first (SpellTypeData::is_castable(CIVILIANSPELL 659)).
			const bool can_civ = spell_is_castable(g, ptype::kCivilianSpell, mo, who, 0) != 0;
			if (order == kBuildAt || order == kRepair) {
				if (!is_peasant_type(u->type) && count_inside(g, *u, kCountPeasants, 0) == 0 && !can_civ) {
					group_add(g, tmp, mo, who, 0, 0);
					continue;
				}
			} else if (order == kGather) {
				const int32_t t = u->type;
				bool worker = false;
				if (t == 52 || t == 53) worker = type_is(*T->ptype, ptype::kUniversity);
				else if (t == 50 || t == 51) worker = !type_is(*T->ptype, ptype::kUniversity);
				else worker = can_civ;
				if (!worker) {
					group_add(g, tmp, mo, who, 0, 0);
					continue;
				}
			}
			const uint32_t a1 = find_angle(u->x - cx, u->y - cy);
			int32_t ft = u->type;
			if (!T->is_unit && T->ptype->domain == 1 && !type_is(*T->ptype, ptype::kDock) && is_peasant_type(u->type) &&
			    0 >= 3) {
				ft = current_upgrade(g, who, ptype::kTransportBarge);  // TODO(transports): LeaderData::can_transport
			} else if (!T->is_unit && T->ptype->domain == 0 && count_inside(g, *u, kCountPeasants, 0) != 0 && u->inside_down >= 0) {
				const Object* p = g.object(u->inside_down_who, u->inside_down);
				if (p) ft = p->type;
			}
			const Type& FT = g.type(ft);
			int32_t sx = 0, sy = 0;
			if (find_nearby_spot(g, FT, cx, cy, &sx, &sy, R, 0, -1, a1, kFilterNotMe, mo, who, 0, 0, ox, whom, -1) != 0) continue;
			const uint32_t a2 = find_angle(cx - sx, cy - sy);
			QueuePos q2 = q;
			if (order == kBuildAt) {
				int32_t nx = sx > cx ? sx + 0x30 : sx - 0x30;
				int32_t ny = sy > cy ? sy + 0x30 : sy - 0x30;
				int32_t rx = nx, ry = ny;
				if (find_nearby_spot(g, FT, nx, ny, &rx, &ry, 0, 0, 0, a1, kFilterNotMe, mo, who, 0, 0, ox, whom, -1) == 0) {
					sx = rx;
					sy = ry;
				}
			}
			if ((order == kBuildAt || order == kRepair) && q == kQueueLast && action_type(*u) == kGather && expl) q2 = kQueueNew;
			add_move_facing_order(g, *u, sx / 48, sy / 48, a2, kind, false, q2, false, -1, -1, -1, false);
			if (order == kBuildAt) {
				if (can_civ) add_cast_order(g, *u, -1, -1, u->x, u->y, ptype::kCivilianSpell, kQueueLast, expl);
				add_build_order(g, *u, ox, whom, kQueueLast, expl);
				if (!T->is_unit) static_cast<Build*>(T)->build_masks &= ~bmask::kOrphan;
			} else if (order == kRepair) {
				if (can_civ) add_cast_order(g, *u, -1, -1, u->x, u->y, ptype::kCivilianSpell, kQueueLast, expl);
				add_repair_order(g, *u, ox, whom, kQueueLast, expl);
			}
			// (GATHER: the move only)
		}
	}
	if (group_get_num(g, tmp) != 0) group_action_move_to(g, tmp, cx, cy, kQueueNew, false, 0, 1, 0, -1, -1, 0);
}

// ---------------------------------------------------------------- construction (4.1, 4.2)

int32_t do_construct(Game& g, Build& b, int32_t amount) {
	if (g.ai_speed > 1) amount *= g.ai_speed;
	if (!(b.flags & objflag::kStarted)) {
		const int32_t r = blocked_site(g, b.type, b.x, b.y, b.who, b.o, nullptr);
		const int32_t bonus = tb(g, b.who, 7) ? 1 : 0;
		if (r == block::kWonder && b.city >= 0 && city_num_wonders(g, b.who, b.city, 1) <= 1 + bonus) {
			build_start(g, b, true);
		} else if (r == 0 || r == block::kOne || r == block::kOneOther || r == block::kFarm || r == block::kNeedWall) {
			build_start(g, b, true);
		} else {
			build_disband(g, b, true);  // console: "construction blocked"
			return 0;
		}
	}
	if (b.flags & objflag::kActive) return 0;
	int32_t inc = amount / (int32_t(b.helpers) + 1);
	if (!(b.build_masks & bmask::kInUse)) {
		b.recharging++;
		b.build_masks |= bmask::kInUse;
	}
	b.helpers++;
	inc = std::max(inc, 1);
	b.job_counter_2 = int32_t(uint32_t(b.job_counter_2) + uint32_t(inc));
	b.job_counter = int32_t(uint32_t(b.job_counter) + uint32_t(inc));
	if (uint32_t(construct_time(g, b, 0)) <= uint32_t(b.job_counter)) {
		build_activate_full(g, b, false, true, true);
		return 1;
	}
	return 0;
}

int32_t do_build(Game& g, Unit& u, Order& ord) {
	const int32_t ox = ord.ox, whom = ord.whom;
	const int32_t range = K("unit_gather_respond_range") * 0xc0;
	Build* B = (ox >= 0 && whom >= 0) ? g.build(whom, ox) : nullptr;
	if (!B || !B->alive()) {
		kill_current_order(g, u, false);
		if (!u.orders.empty() && u.orders.front().type != kOrderNone) return 0;
		build_done(g, u, ox, whom, range);
		return 0;
	}
	auto gather_rule = [&]() {
		// (OILPLATFORM or (not AI and worker stance GATHER / BUILD_AND_GATHER)), own gather building, not a university.
		const bool cond = (type_is(*B->ptype, ptype::kOilPlatform) ||
		                   (!(u.unit_masks & umask::kAiControl) && (worker_stance(u) == 1 || worker_stance(u) == 0))) &&
		                  whom == u.who && is_gather_type(*B->ptype) && !type_is(*B->ptype, ptype::kUniversity);
		return cond;
	};
	if (B->flags & objflag::kActive) {
		kill_current_order(g, u, false);
		if (u.order_type() != kOrderNone) {
			check_build_order(g, u);
			return 0;
		}
		if (gather_rule()) {
			add_gather_order(g, u, ox, kQueueNew, false);
			u.group = -1;  // (selection drop)
			return 0;
		}
		build_done(g, u, ox, whom, range);
		return 0;
	}
	if (!adjacent_to(g, u, ox, whom) ||
	    (covers_tile(*B, tcoord_of(u.x), tcoord_of(u.y)) && !type_is(*B->ptype, ptype::kFarm))) {
		const bool expl = (ord.flags & orderflag::kExplicit) != 0;
		kill_current_order(g, u, false);
		swarm_one(g, u, ox, whom, kBuildAt, expl);
		return 0;
	}
	unit_set_anim(g, u, type_is(*B->ptype, ptype::kFarm) ? kCharSow : kCharBuild, 0, 1);
	const uint32_t a = find_angle(B->x - u.x, B->y - u.y);
	if (u.angle != a) unit_set_angle(g, u, a, false);
	if (u.unit_masks & umask::kDecoy) return 0;
	int32_t amount = K("accel_construct");
	if (!(tb(g, u.who, 0x10) && K("korean_build_under_fire") != 0)) {
		if (B->build_masks & bmask::kUnderAttack) amount /= 4;
	}
	if (!do_construct(g, *B, amount)) return 0;
	// Finished by this call.
	kill_current_order(g, u, false);
	const OrderType ot = u.order_type();
	if (ot != kOrderNone) {
		bool keep_gather = false;
		if (is_gather_type(*B->ptype) && !type_is(*B->ptype, ptype::kUniversity) && u.group >= 0) {
			Group& gr = g.groups.g[size_t(u.group)];
			group_normalize(g, gr);
			keep_gather = group_count_peasants(g, gr, 1) >= 2;
		}
		if (!keep_gather) {
			check_build_order(g, u);
			return 0;
		}
	}
	if (gather_rule()) {
		add_gather_order(g, u, ox, kQueueNew, false);
		u.group = -1;
		return 0;
	}
	if (ot == kOrderNone) build_done(g, u, ox, whom, range);
	else check_build_order(g, u);
	return 0;
}

// ---------------------------------------------------------------- repair (4.8)

int32_t do_repair(Game& g, Unit& u, Order& ord) {
	unit_set_anim(g, u, kCharRepair, 0, 1);
	const int32_t ox = ord.ox, whom = ord.whom;
	Object* T = g.object(whom, ox);
	if (!T || !T->alive()) {
		kill_current_order(g, u, false);
		return 0;
	}
	const bool mutual = whom == u.who || (is_ally(g, u.who, whom) && is_ally(g, whom, u.who));
	if (T->damage == 0 || !mutual) {
		kill_current_order(g, u, false);
		if (u.unit_masks & umask::kAiControl) {
			find_repair_spot(g, u);
		} else if (whom == u.who && u.orders.empty() && worker_stance(u) < 2 && !T->is_unit &&
		           is_gather_type(*T->ptype) && !type_is(*T->ptype, ptype::kUniversity)) {
			add_gather_order(g, u, ox, kQueueNew, false);
		}
		return 0;
	}
	if (T->is_unit || !(T->flags & objflag::kActive)) {
		kill_current_order(g, u, false);
		return 0;
	}
	Build& B = static_cast<Build&>(*T);
	if ((B.build_masks & bmask::kUnderAttack) && !(ord.flags & orderflag::kExplicit)) {
		kill_current_order(g, u, false);
		return 0;
	}
	{
		const int32_t wx = wcoord_of(B.x), wy = wcoord_of(B.y);
		const int32_t own = g.world.in_w(wx, wy) ? g.world.w(wx, wy).who : -1;
		if (own >= 0 && own != whom && !is_ally(g, own, whom)) {
			kill_current_order(g, u, false);
			return 0;
		}
	}
	if (!adjacent_to(g, u, ox, whom)) {
		const bool expl = (ord.flags & orderflag::kExplicit) != 0;
		kill_current_order(g, u, false);
		swarm_one(g, u, ox, whom, kRepair, expl);
		return 0;
	}
	if (u.unit_masks & umask::kDecoy) return 0;
	const int32_t hits0 = obj_hits(g, B);
	const uint32_t den = uint32_t(hits0) * uint32_t(K("accel_construct"));
	int32_t P = int32_t(B.helpers + 1) * int32_t(den ? (uint32_t(construct_time(g, B, 1)) << 9) / den : 0);
	if (B.flags & objflag::kCity) {
		P *= 2;
		if (B.city >= 0) {
			const City& c = g.cities[size_t(B.who)][size_t(B.city)];
			if (c.race != B.who) P *= 2;  // the city's founder byte (+0x5f) differs from the owner
		}
	}
	const bool korean = tb(g, u.who, 0x10);
	if (korean && K("korean_repair") != 0) P = ((100 - K("korean_repair")) * P) / 100;
	if (!(korean && K("korean_build_under_fire") != 0)) {
		if (B.build_masks & bmask::kUnderAttack) P <<= 2;
		if (B.build_masks & bmask::kAttacking) P <<= 2;
	}
	B.helpers++;
	const int32_t f = g.frame;
	const int32_t amount = P <= 0 ? B.damage : (256 * f) / P - (256 * f - 256) / P;
	if (amount == 0) return 0;
	const int32_t hl = obj_hits_left(g, B);
	Leader& l = LD(g, u.who);
	int32_t pay[6] = {0, 0, 0, 0, 0, 0};
	for (int32_t k = 0; k < 6; k++) {
		if (!type_avail(g, u.who, k, 1)) continue;
		const int32_t c = get_cost(g, B.type, k, whom, -1, -1, 0, 1, -1);
		if (c == 0) continue;
		const float r = float(hits0) / float(c);
		pay[k] = int32_t(float(amount + hl) / r) > int32_t(float(hl) / r) ? 1 : 0;
		if (l.bucket[size_t(k)] < pay[k]) {
			kill_current_order(g, u, false);
			if (l.repair_stamp + 150 < g.frame) l.repair_stamp = g.frame;  // (message throttle)
			return 0;
		}
	}
	for (int32_t k = 0; k < 6; k++) {
		if (!type_avail(g, u.who, k, 1)) continue;
		l.bucket[size_t(k)] -= pay[k];
		l.escrow[size_t(k)] = std::max(0, l.escrow[size_t(k)] - pay[k]);
	}
	// Build::repair_damage(amount, 0, 1) = Object::repair_damage.
	B.damage = std::min(B.damage, hits0);
	B.damage_frac = 0;
	B.damage = std::max(0, B.damage - amount);
	return 0;
}

// ---------------------------------------------------------------- the building pass (8.1, 8.2)

namespace {

// Wall::process @640450. Returns false when the building was removed.
bool wall_process(Game& g, Build& b) {
	const int32_t who = b.who;
	Leader& l = LD(g, who);
	if (g.frame != 0 && (g.frame & 7) == who) {
		b.targeted = int8_t(b.targeted / 4);
		check_ever_seen(g, b, false);
	}
	if ((g.frame + b.o) % 32 == 0) {
		if (b.build_masks & bmask::kAttacking) b.build_masks &= ~bmask::kAttacking;
		else b.build_masks &= ~bmask::kUnderAttack;
		if (!(b.flags & objflag::kActive) && !(l.flags & leaderflag::kHuman)) {
			if ((g.frame + b.o) % 128 == 0 && type_is(*b.ptype, ptype::kOilPlatform)) {
				// TODO(ai): find_unit(FILTER_TARGET) for an own unit targeting the site; taken as none.
				build_disband(g, b, false);
				return false;
			}
			if (is_wonder_type(b.type) || (type_is(*b.ptype, ptype::kFortX) && b.damage == 0)) {
				int32_t want = std::max<int32_t>(4, b.helpers);
				if (is_wonder_type(b.type)) {
					const float f = float(double(uint32_t(b.job_counter))) / float(double(uint32_t(construct_time(g, b, 0))));
					for (int32_t L = 0; L < 8; L++) {
						const Leader& ll = g.leaders[size_t(L)];
						if ((ll.flags & 3) != 3 || is_ally(g, who, L)) continue;
						if (ll.wonderwin_timer != 0) {
							want++;
							continue;
						}
						for (const Unbuilt& e : g.unbuilt_wonders[size_t(L)]) {
							const Build* ob = g.build(e.who, e.o);
							if (!ob || !type_is(*ob->ptype, b.type, true)) continue;
							const float f2 = float(double(uint32_t(ob->job_counter))) / float(double(uint32_t(construct_time(g, *ob, 0))));
							if (f < f2 && (f2 < f + f || f2 < 0.35f)) want++;
						}
					}
				}
				if (b.helpers < want) {
					// TODO(ai): find_unit(SEARCH_FRIENDLY, 0xf00, FILTER_TYPE PEASANTS, FILTER_NOT_BUSY) gets
					// add_build_order(o, who, QUEUE_NEW, 0). ObjectsData::find_unit is not ported.
				}
			}
		}
	}
	if (b.helpers != 0) {
		b.helpers = 0;
		b.build_masks |= bmask::kConstructing;
	} else {
		b.build_masks &= ~bmask::kConstructing;
	}
	b.build_masks &= ~bmask::kInUse;
	// Territory attrition (period 16 under rush rules before war is allowed, else 32; asm order kept).
	if ((g.frame + b.o) % 16 == 0 && l.disable_building_attrition == 0) {
		const bool peace = g.info.rush_rules != 0 && false;  // TODO(frame-spec): Game::war_allowed (rush rules)
		if (peace || (g.frame + b.o) % 32 == 0) {
			int32_t owner;
			if (type_is(*b.ptype, ptype::kDock)) {
				owner = -1;  // TODO(territory): BuildTypeData::check_enemy_adjacent
			} else {
				const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
				owner = g.world.in_w(wx, wy) ? g.world.w(wx, wy).who : -1;
			}
			if (owner >= 0 && owner != who && !is_ally(g, who, owner)) {
				if (!(b.ever_seen & (1u << owner)) && (g.leaders[size_t(owner)].flags & 0x2000)) {
					b.ever_seen = uint8_t(b.ever_seen | (1u << owner));
					check_ever_seen(g, b, true);
				}
				if (!(b.flags & objflag::kStarted)) {
					build_disband(g, b, false);
					l.attrition_stamp3 = g.frame;
					return false;
				}
				if (take_damage(g, b, 8, 0, 1, -1, true, 0x55555555u, -1, -1, false)) return false;
				if (l.attrition_stamp3 != 0) return true;
				l.attrition_stamp3 = g.frame;
			}
		}
	}
	return true;
}

}  // namespace

void build_process(Game& g, Build& b) {
	if (!wall_process(g, b) || !b.alive()) return;
	if (!(b.flags & objflag::kActive)) return;
	const int32_t who = b.who;
	Leader& l = LD(g, who);
	if (b.healing) b.healing--;
	if (b.build_masks & bmask::kEjecting) build_process_ejection(g, b);  // Build::process_ejection @6201e0
	if (b.build_masks & bmask::kHoldsAir) do_launch(g, b);  // Object::do_launch @64f3b0 (sim/aircraft.cpp)
	if (b.ptype->attack != 0) build_process_attack(g, b);
	if (!b.alive()) return;
	if (b.build_masks & bmask::kNeutralized) {
		b.build_masks &= ~bmask::kNeutralized;
		if (b.city >= 0) {
			City& c = g.cities[size_t(who)][size_t(b.city)];
			if (type_is(*b.ptype, ptype::kTemple)) c.city_flags |= 0x80;
			else if (type_is(*b.ptype, ptype::kLumberMill)) c.city_flags |= 0x400;
			else if (type_is(*b.ptype, ptype::kGranary)) c.city_flags |= 0x200;
			else if (type_is(*b.ptype, ptype::kMarket)) c.city_flags |= 0x800;
		}
	}
	// Government heroes mend damaged buildings every french_supply_heal_rate frames: THE PRESIDENT (0x163) on neutral or
	// allied land, THE SENATOR (0x161) / THE CEO (0x165) on allied land (Build::process @61edf0).
	if ((l.num_units[size_t(0x161 - kUnits)] != 0 || l.num_units[size_t(0x165 - kUnits)] != 0 ||
	     l.num_units[size_t(0x163 - kUnits)] != 0) &&
	    (b.damage != 0 || b.damage_frac != 0) && K("french_supply_heal_rate") != 0 &&
	    (b.o + g.frame) % K("french_supply_heal_rate") == 0) {
		const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
		const int32_t terr = g.world.in_w(wx, wy) ? g.world.w(wx, wy).who : -1;
		bool heal;
		if (has_general(g, b, 0, 0x163) < 0)
			heal = terr >= 0 && is_ally(g, who, terr) && (has_general(g, b, 0, 0x161) >= 0 || has_general(g, b, 0, 0x165) >= 0);
		else
			heal = terr < 0 || is_ally(g, who, terr);
		if (heal) {
			b.damage = std::min(b.damage, obj_hits(g, b));  // Object::repair_damage(1, 0, 1)
			b.damage_frac = 0;
			b.damage = std::max(0, b.damage - 1);
			b.healing = int16_t(std::max<int32_t>(K("french_supply_heal_rate"), b.healing));
		}
	}
	build_do_queue(g, b, 0);
	if (!b.alive()) return;
	if (is_gather_type(*b.ptype) && !is_flat_gather(*b.ptype)) {
		const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
		const int32_t r = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : -1;
		if (r >= 0 && r < Regions::kNum) {
			if (g.regions.r[size_t(r)].flags & 0x20) build_verify_gather_tiles(g, b);  // (economy-spec 3.6)
			if (g.regions.r[size_t(r)].flags & 0x10) find_gather_tiles(g, b);
		}
	}
	if (b.type == ptype::kKremlin && (b.o + g.frame) % 15 == 0 && (l.flags & leaderflag::kKremlinSpy)) {
		if (--b.recharging < 1) {
			l.flags &= ~leaderflag::kKremlinSpy;
			b.recharging = int16_t(K("kremlin_spy"));
			const int32_t uo = build_train(g, b, ptype::kSpy);
			if (uo >= 0) {
				Unit& s = *g.unit(who, uo);
				s.unit_masks2 |= 0x400;
				l.control -= (s.unit_masks & umask::kDecoy) ? 0 : s.ptype->control_cost;
			}
		}
	}
	if (l.gov_hero_frame == g.frame + 1) {
		if (b.type == ptype::kSenate) {
			const int32_t h = get_gov_hero(g, who);
			if (h >= 0) build_train(g, b, h);
		}
		if (b.flags & objflag::kCity) {
			const int32_t to = g.type(ptype::kSenate).to;
			int32_t n = l.num_buildings[size_t(ptype::kSenate - kBuilds)];
			if (to >= 0) n += get_buildings(g, who, to, l.num_buildings[size_t(to - kBuilds)]);
			if (n < 1) {
				const int32_t h = get_gov_hero(g, who);
				if (h >= 0) build_train(g, b, h);
			}
		}
	}
	if (b.type == ptype::kMarket && tb(g, who, 0x17) && (b.o + g.frame) % 15 == 0) {
		if (l.caras < get_caravan_limit(g, who, 1) && check_population(g, who, ptype::kCara) == 0) build_train(g, b, ptype::kCara);
	}
	if (b.type == ptype::kTerraCotta) {
		if (--b.recharging < 1) {
			const int32_t d = K("terra_cotta_delay");
			b.recharging = int16_t(K("terra_cotta_free") * 15 + (d == 7 ? l.barracks_units * 15 / 2 : l.barracks_units * d));
			int32_t t = tribe_can_type(g, who, ptype::kSlingers) ? ptype::kSlingers : get_graft(g, who, ptype::kSlingers);
			t = current_upgrade(g, who, t);
			if (t >= 0 && l.control < l.pop_cap) {
				build_train(g, b, t);
				g.terra_cotta_total++;
			}
		}
	}
	if ((b.flags & objflag::kCity) && b.city >= 0) {
		City& C = g.cities[size_t(who)][size_t(b.city)];
		if ((g.frame + b.o) % 200 == 0) {
			if (C.city_flags & 4) C.city_flags &= uint16_t(~4u);
			else C.city_flags &= uint16_t(~2u);
			if (C.plundered) C.plundered--;
		}
		// TODO(diplomacy): team_style 2 targets (LeaderData::get_target) see capitals.
		// (d) the 64-frame capture poll and the AI's repair call (sim/capture.cpp); a capture ends Build::process.
		if (build_capture_poll(g, b)) return;
		if (C.race != who) {
			// THE COMRADE (0x164, "the citizen") in range slows the assimilation clock.
			if (has_general(g, b, 0, 0x164) >= 0) C.assimilation_timer += 1 - K("thecitizen_assimilation_speed");
			int32_t t = g.frame - C.assimilation_timer;
			if (tb(g, who, 8)) t = (K("turk_assimilate") + 100) * t / 100;
			if (who == C.founder) t = (K("reassimilation") + 100) * t / 100;
			if (t >= K("assimilation_timer")) city_assimilate(g, who, b.city);
		}
		if (!(C.city_flags & 2)) {
			const int32_t rate = K("city_heal_rate");
			if (b.damage != 0 && C.race == who && rate > 0 && (g.frame + b.o) % rate == 0) {
				const int32_t lv = city_get_level(b.type);
				b.damage = std::min(b.damage, obj_hits(g, b));
				b.damage_frac = 0;
				b.damage = std::max(0, b.damage - lv);
			}
			// TODO(economy): the city alarm (CITY_ALARM 0x40, Group::action_alarm).
		}
	}
	if ((b.build_masks & bmask::kMakeRoad) && (g.frame + b.o) % 16 == 0) {
		b.build_masks &= ~bmask::kMakeRoad;
		place_roads(g.world, *b.ptype, b.x, b.y, true, b.city >= 0);  // BuildType::place_roads(.., 1, REGEN_FORCE): the ring
		place_roads_regen(g, b, true, Regen::Force);                   // and the road to the city centre
	}
}

void wall_inc_time(Game& g, Build& b) {
	// Wall::inc_time @63fb60: hit points grow with construction progress.
	if (!(b.flags & objflag::kActive)) wall_update_hits(g, b, false);
	// TODO(combat-spec 4.13.4): air-defence turret events.
}

// ---------------------------------------------------------------- DISBAND (4.9)

void group_action_disband(Game& g, Group& gr, int32_t all) {
	// TODO(scenarios): ignore_orders prelude. GroupOut::validate_disband @70a4a0 🟡: building groups of more than one
	// member are refused; the city rules for finished buildings and the castable test of DISBAND are not ported.
	if (gr.buildings && gr.num > 1) return;
	const int32_t who = gr.who;
	int32_t count = 0;
	for (int32_t i = gr.num - 1; i >= 0; i--) {
		Object* ob = g.object(who, gr.list[size_t(i)]);
		if (!ob || !ob->alive()) continue;
		if (ob->is_unit || !(ob->flags & objflag::kActive)) {
			if (ob->is_unit) {
				Unit& u = static_cast<Unit&>(*ob);
				// Object::disband(0) for a unit: carried units are disbanded first (TODO(garrison)), then die(rnd(3) + 1)
				// on the map, die(1) off it.
				// An Aztec-damaged captain (UNIT_AZTEC_DAMAGE) disbanded: the damaging active Aztec leader plunders it.
				if (u.o_up < 0 && (u.unit_masks & umask::kAztecDamage) && u.damage_who >= 0 && u.damage_who < 8 &&
				    (LD(g, u.damage_who).flags & 3) == 3 && has_tribe_bonus(g, u.damage_who, 0))
					unit_plunder(g, u, u.damage_o, u.damage_who, 1, 0);
				if (u.on_map()) {
					const int32_t d = int32_t(g.rng.get(0, 0xffff) % 3) + 1;  // @6459c3
					object_die(g, u, d, -1, 0.0f);
				} else {
					object_die(g, u, 1, -1, 0.0f);
				}
			} else {
				build_disband(g, static_cast<Build&>(*ob), false);
			}
			count++;
			if (!all) break;
		} else {
			Build& b = static_cast<Build&>(*ob);
			if (build_count_queue(g, b, 1, ptype::kDisband) == 0) build_queue_up(g, b, ptype::kDisband, 0);
		}
	}
	(void)count;
}

// Exported wrappers of the file-local helpers above (economy, sim/econ_gather.cpp).
bool unit_adjacent_to(const Game& g, const Unit& u, int32_t ox, int32_t whom) { return adjacent_to(g, u, ox, whom); }
bool build_covers_tile(const Build& b, int32_t tx, int32_t ty) { return covers_tile(b, tx, ty); }
int32_t unit_worker_stance(const Unit& u) { return worker_stance(u); }
int32_t unit_find_build_spot(Game& g, Unit& u) { return find_build_spot(g, u); }
int32_t unit_find_repair_spot(Game& g, Unit& u) { return find_repair_spot(g, u); }

}  // namespace ron::sim
