// Gathering (docs/economy-spec.md sections 3.5, 3.6, 4; docs/farms-spec.md 2-3.3): gatherer lists, the GATHER order
// (Unit::add_gather_order, Group::action_gather / action_city_gather), the per-frame handlers Unit::do_gather and
// do_non_flat_gather with their game_random draws (G3-G7), auto-gather (think_peasant, find_gather_spot), farms
// (Farms::add / inc_time / grow / snip / add_animals; G9 and the pen / ambience draws) and the oil-well records.
#include <algorithm>

#include "sim/ai.hpp"
#include "sim/spells.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/production_internal.hpp"
#include "sim/search.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using prod::tb;

// construct.cpp (exported wrappers of its file-local helpers)
bool unit_adjacent_to(const Game& g, const Unit& u, int32_t ox, int32_t whom);
bool build_covers_tile(const Build& b, int32_t tx, int32_t ty);
int32_t unit_worker_stance(const Unit& u);
int32_t unit_find_build_spot(Game& g, Unit& u);
int32_t unit_find_repair_spot(Game& g, Unit& u);
// econ_income.cpp
bool econ_has_gather_access(const Game& g, int32_t tx, int32_t ty, int32_t who, int32_t p4);

namespace {

Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }

bool is_worker_t(int32_t t) { return t >= 50 && t <= 53; }
bool is_scholar_t(int32_t t) { return t == 52 || t == 53; }
bool is_peasant_t(int32_t t) { return t == 50 || t == 51; }

// ObjectData::count_inside(COUNT_TYPE / COUNT_NON_DECOY_TYPE, t) over the inside_down chain (garrison.cpp).
int32_t count_inside_type(const Game& g, const Object& ob, int32_t t, bool no_decoys) {
	return count_inside(g, ob, no_decoys ? kCountNonDecoyType : kCountType, t);
}


// Doober::add_hold_doober / remove_hold_doobers: the hold slot index stored in the unit (+0x86). The doober itself is
// presentation; the slot is the first free one (the original frees a slot after its fade-out, a render-side timer:
// here at once, TODO(doobers)).
void add_hold_doober(Game& g, Unit& u) {
	size_t i = 0;
	while (i < g.hold_doobers.size() && g.hold_doobers[i]) i++;
	if (i == g.hold_doobers.size()) g.hold_doobers.push_back(0);
	g.hold_doobers[i] = 1;
	u.doober = int16_t(i);
}

// The "drop the order and re-think" tails of Unit::do_gather.
void drop_new_think(Game& g, Unit& u) {
	kill_current_order(g, u, false);
	u.flags |= objflag::kNewThink;
}

int32_t min_size_radius(const Type& t) { return std::min(t.x_size, t.y_size) * 0x60 + 0x30; }

}  // namespace

void remove_hold_doober(Game& g, Unit& u) {
	if (u.doober >= 0 && size_t(u.doober) < g.hold_doobers.size()) g.hold_doobers[size_t(u.doober)] = 0;
	u.doober = -1;
}

// ---------------------------------------------------------------- gatherer lists (3.5)

bool build_is_gathered_by(const Game& g, const Build& b, int32_t o) {
	for (int32_t i = b.gather_down, guard = 0; i >= 0 && guard < kUnitLimit; guard++) {
		if (i == o) return true;
		const Unit* u = g.unit(b.who, i);
		if (!u) break;
		i = u->gather_down;
	}
	return false;
}

bool unit_is_gathering_at(const Game& g, const Unit& u, int32_t o, int32_t who, bool been_there) {
	(void)g;
	// UnitData::is_gathering_at @608880
	if (is_scholar_t(u.type) && !u.on_map()) {
		if (o == u.inside_up && who == u.inside_up_who) return true;
		if (been_there) return false;
	}
	if (!u.on_map() || !is_worker_t(u.type)) return false;
	const Order* a = get_action(u);
	if (!a || a->type != kGather) return false;
	if (a->whom != who || a->ox != o) return false;
	return been_there ? a->been_there != 0 : true;
}

bool unit_is_gathering(const Game& g, const Unit& u) {
	(void)g;
	if (!is_worker_t(u.type)) return false;
	const Order* a = get_action(u);
	return a && a->type == kGather && a->whom == u.who;
}

int32_t build_num_gatherers(const Game& g, const Build& b, bool been_there, bool no_decoys) {
	// BuildData::num_gatherers @630450
	int32_t n = 0;
	if (type_is(*b.ptype, ptype::kOilPlatform)) n = count_inside_type(g, b, ptype::kPeasants, no_decoys);
	if (type_is(*b.ptype, ptype::kUniversity)) n += count_inside_type(g, b, ptype::kScholars, no_decoys);
	for (int32_t i = b.gather_down, guard = 0; i >= 0 && guard < kUnitLimit; guard++) {
		const Unit* u = g.unit(b.who, i);
		if (!u) break;
		if (!(no_decoys && (u->unit_masks & umask::kDecoy)) && unit_is_gathering_at(g, *u, b.o, b.who, been_there)) n++;
		i = u->gather_down;
	}
	return n;
}

int32_t build_check_gatherers(Game& g, Build& b) {
	// Build::check_gatherers @62f710: pop dead / off-map / no-longer-gathering heads, then unlink the same from the
	// rest (an entry unlinked from the middle keeps its own gather_down: quirk kept).
	auto ok = [&](const Unit* u) {
		return u && u->alive() && unit_is_gathering_at(g, *u, b.o, b.who, false) && u->alive() && u->on_map();
	};
	for (int guard = 0; b.gather_down >= 0 && guard < kUnitLimit; guard++) {
		Unit* u = g.unit(b.who, b.gather_down);
		if (ok(u)) break;
		const int16_t next = u ? u->gather_down : int16_t(-1);
		if (u) u->gather_down = -1;
		b.gather_down = next;
	}
	if (b.gather_down < 0) return b.gather_down;
	int32_t prev = b.gather_down;
	for (int guard = 0; guard < kUnitLimit; guard++) {
		Unit* p = g.unit(b.who, prev);
		if (!p) break;
		const int32_t cur = p->gather_down;
		if (cur < 0) break;
		Unit* c = g.unit(b.who, cur);
		if (ok(c)) {
			prev = cur;
		} else {
			p->gather_down = c ? c->gather_down : int16_t(-1);
		}
	}
	return b.gather_down;
}

void build_add_gatherer(Game& g, Build& b, int32_t o, int32_t who) {
	// Build::add_gatherer @62f640
	if (who != b.who) return;
	if (build_num_gatherers(g, b, false, false) >= b.gather_max) return;
	Unit* u = g.unit(who, o);
	if (!u || !u->alive() || !u->on_map() || !is_worker_t(u->type)) return;
	if (build_is_gathered_by(g, b, o)) return;
	build_check_gatherers(g, b);
	u->gather_down = b.gather_down;
	b.gather_down = int16_t(o);
}

void build_remove_gatherer(Game& g, Build& b, int32_t o) {
	// Build::remove_gatherer @62f8d0
	if (b.gather_down < 0) return;
	int32_t i = b.gather_down;
	if (i == o) {
		Unit* u = g.unit(b.who, i);
		if (!u) return;
		b.gather_down = u->gather_down;
		u->gather_down = -1;
		return;
	}
	for (int guard = 0; guard < kUnitLimit; guard++) {
		Unit* p = g.unit(b.who, i);
		if (!p || !p->alive()) return;
		const int32_t n = p->gather_down;
		if (n < 0) return;
		if (n == o) {
			Unit* c = g.unit(b.who, n);
			if (!c) return;
			p->gather_down = c->gather_down;
			c->gather_down = -1;
			return;
		}
		i = n;
	}
}

bool build_all_gathering(Game& g, Build& b) {
	// Build::all_gathering @62f570: every registered gatherer's current (front) order is a target order that is at
	// its tile (goto_build 0) with wait >= 0.
	build_check_gatherers(g, b);
	for (int32_t i = b.gather_down, guard = 0; i >= 0 && guard < kUnitLimit; guard++) {
		const Unit* u = g.unit(b.who, i);
		if (!u) return true;
		i = u->gather_down;
		const Order* f = u->front();
		if (!f || !order_is_targeted(f->type)) return false;
		if (f->goto_build != 0) return false;
		if (f->wait < 0) return false;
	}
	return true;
}

void build_update_max_gatherers(Game& g, Build& b) {
	const Footprint f = footprint(*b.ptype, b.x, b.y);
	b.gather_max = int8_t(max_gatherers(g, b.type, b.o, b.who, f.x0, f.y0));
}

void build_verify_gather_tiles(Game& g, Build& b) {
	// Build::verify_gather_tiles @623570: tiles whose W-cell owner is neither own nor mutually allied are dropped.
	for (size_t i = 0; i < b.gather_from.size();) {
		const auto t = b.gather_from[i];
		const int32_t own = g.world.in_t(t[0], t[1]) ? g.world.w(t[0] >> 2, t[1] >> 2).who : -1;
		if (own >= 0 && own != b.who && !is_ally(g, b.who, own)) {
			g.world.set_mask_bit(t[0], t[1], tmask::kGathered, false);
			b.gather_from.erase(b.gather_from.begin() + std::ptrdiff_t(i));
		} else {
			i++;
		}
	}
}

// ---------------------------------------------------------------- the GATHER order (4.1)

void add_gather_order(Game& g, Unit& u, int32_t ox, QueuePos q, bool expl) {
	// Unit::add_gather_order @61a5c0
	if (q == kQueueNew) {
		u.unit_masks &= ~umask::kMultimove;
		u.queue_time = 0;  // (+0xc0)
		close_orders(g, u, false);
		clear_partial_path(u);
		update_action(g, u);
	}
	u.unit_masks &= ~umask::kBuilder;
	Order o;
	o.type = kGather;
	o.ox = ox;
	o.whom = u.who;
	Build* b = ox >= 0 ? g.build(u.who, ox) : nullptr;
	o.uid = b ? b->uid : 0xffff;
	o.build_type = b ? b->type : -1;
	o.g_tx = o.g_ty = -1;
	o.goto_build = 0;
	o.non_flat = 0;
	if (b && is_gather_type(*b->ptype)) {
		build_add_gatherer(g, *b, u.o, u.who);
		if (!is_flat_gather(*b->ptype) && !type_is(*b->ptype, ptype::kUniversity)) {
			o.non_flat = 1;
			o.dist_mod = type_is(*b->ptype, ptype::kMine) ? 10 : 4;
		}
	}
	if (expl) o.flags |= orderflag::kExplicit;
	if (b) {
		const int32_t r1 = g.world.get_tregion(tcoord_of(b->x), tcoord_of(b->y));
		const int32_t r2 = g.world.get_tregion(tcoord_of(u.x), tcoord_of(u.y));
		if (r1 != r2) {
			// The leader's transport level (CAN_TRANSPORT_CIV 3 / MIL 2 / SCT 1) against the unit's transport type.
			// TODO(transports): UnitData::transport_type / can_ever_transport are not ported; land citizens count as
			// civilians (1) that can be transported.
			const uint32_t lf = LD(g, u.who).flags;
			const int32_t lvl = (lf & 0x100) ? 3 : (lf & 0x200) ? 2 : int32_t((lf >> 10) & 1);
			if (1 <= lvl && u.ptype->domain == 0) u.unit_masks |= umask::kCanTransport;
		}
	}
	u.orders.push_back(o);
	update_action(g, u);
}

void gather_order_killed(Game& g, Unit& u, const Order& o) {
	// The GATHER part of Unit::kill_current_order @5e2cb0.
	LD(g, u.who).flags |= leaderflag::kRecompute;
	Unit* gu = &u;
	if ((u.ptype->unit_flags & 0x10) && u.inside_down >= 0) {
		Unit* c = g.unit(u.inside_down_who, u.inside_down);
		if (c) gu = c;
	}
	if (o.ox >= 0 && o.whom >= 0 && o.whom < kNumLeaders) {
		Build* b = g.build(o.whom, o.ox);
		if (b && b->alive() && gu->who == b->who) build_remove_gatherer(g, *b, gu->o);
	}
	gu->unit_masks &= ~umask_econ::kWithStuff;
	if (is_worker_t(gu->type) && gu->doober >= 0) remove_hold_doober(g, *gu);
}

// ---------------------------------------------------------------- Unit::do_gather (4.2)

int32_t do_gather(Game& g, Unit& u, Order& ord) {
	const int32_t ox = ord.ox, whom = ord.whom;
	if (whom != u.who) {
		kill_current_order(g, u, false);
		return 0;
	}
	Build* B = g.build(whom, ox);
	if (!B || !B->alive() || !(B->flags & objflag::kActive)) {
		kill_current_order(g, u, false);
		return 0;
	}
	Build& b = *B;
	// The "re-think" tail: after kill + THINK, flat targets return if the unit stands on the footprint, else it walks
	// to the building (QUEUE_FIRST, in front of the THINK order).
	auto rethink_tail = [&]() {
		if (!is_flat_gather(*b.ptype)) return;
		if (build_covers_tile(b, tcoord_of(u.x), tcoord_of(u.y))) return;
		add_move_order(g, u, b.x, b.y, 1, false, kQueueFirst, false, -1, -1);
	};
	if (ord.been_there == 0) {
		if (!build_is_gathered_by(g, b, u.o)) {
			if (build_num_gatherers(g, b, false, false) < b.gather_max) {
				if (ord.build_type != ptype::kOilPlatform && u.ptype->domain == 0) {
					build_add_gatherer(g, b, u.o, u.who);
					if (!build_is_gathered_by(g, b, u.o)) {
						kill_current_order(g, u, false);
						add_think_order(g, u);
						rethink_tail();
						return 0;
					}
				}
			} else {
				kill_current_order(g, u, false);
				add_think_order(g, u);
				rethink_tail();
				return 0;
			}
		}
	}
	int32_t farm = -1;
	if (ord.build_type == ptype::kFarm) {
		if (b.city < 0) {
			kill_current_order(g, u, false);
			add_think_order(g, u);
			rethink_tail();
			return 0;
		}
		farm = b.fort;  // the farm slot (+0x78)
	}
	if (ord.build_type == ptype::kMine || ord.build_type == ptype::kWoodcutter) {
		if ((g.frame + u.o * 4) % 128 != 0 || build_num_gatherers(g, b, false, false) <= b.gather_max) {
			do_non_flat_gather(g, u, ord);
			return 0;
		}
		kill_current_order(g, u, false);
		add_think_order(g, u);
		rethink_tail();
		return 0;
	}
	bool arrived = false;
	if (ord.build_type != ptype::kFarm) {
		if (!unit_adjacent_to(g, u, ox, whom)) {
			const int32_t R = min_size_radius(*b.ptype);
			int32_t sx = b.x, sy = b.y;
			if (b.ptype->domain == u.ptype->domain || !unit_can_transport(u)) {
				const uint32_t a = find_angle(u.x - b.x, u.y - b.y);
				if (find_nearby_spot(g, *u.ptype, b.x, b.y, &sx, &sy, R, -1, 0, a, kFilterNotMe, u.o, u.who, 0, 0, -1, 0,
				                     -1) != 0) {
					drop_new_think(g, u);
					return 0;
				}
			}
			if (!build_is_gathered_by(g, b, u.o) && !type_is(*b.ptype, ptype::kOilPlatform)) {
				ord.been_there = 0;
				return 0;
			}
			add_move_order(g, u, sx, sy, 1, false, kQueueFirst, false, -1, -1);
			return 0;
		}
		arrived = true;
	} else {
		if (build_covers_tile(b, tcoord_of(u.x), tcoord_of(u.y))) {
			arrived = true;
		} else {
			const int32_t r1 = g.world.get_tregion(tcoord_of(b.x), tcoord_of(b.y));
			const int32_t r2 = g.world.get_tregion(tcoord_of(u.x), tcoord_of(u.y));
			if (r2 != r1 && !unit_can_transport(u)) {
				drop_new_think(g, u);
				return 0;
			}
			if (b.gather_max < build_num_gatherers(g, b, false, false)) {
				drop_new_think(g, u);
				return 0;
			}
			if (u.ptype->domain == 0 && !build_is_gathered_by(g, b, u.o)) {
				ord.been_there = 0;
				return 0;
			}
			add_move_order(g, u, b.x, b.y, 1, false, kQueueFirst, false, -1, -1);
			return 0;
		}
	}
	(void)arrived;
	// Adjacent / on the footprint (LAB_005ef939).
	if (ord.been_there == 0) {
		if (b.gather_max < build_num_gatherers(g, b, false, false)) {
			drop_new_think(g, u);
			return 0;
		}
		ord.been_there = 1;
		LD(g, u.who).flags |= leaderflag::kRecompute;
		if (is_scholar_t(u.type)) {
			unit_go_inside(g, u, ox, whom);
			build_check_gatherers(g, b);
			kill_current_order(g, u, false);
			return 0;
		}
		if (type_is(*b.ptype, ptype::kOilPlatform)) {
			// TODO(garrisons): ObjectData::can_carry(GROUND) of the platform; the citizen goes inside and, when the
			// platform can carry it, dies as a unit (same_damage, go_inside, kill_current_order, die).
			unit_go_inside(g, u, ox, whom);
			build_check_gatherers(g, b);
			kill_current_order(g, u, false);
			return 0;
		}
		build_check_gatherers(g, b);
	}
	// AI repair of the city's damaged building (LAB_005efb20).
	if ((u.unit_masks & umask::kAiControl) && (u.o + g.frame + u.who) % 256 == 0 && get_diff(g, u.who) > 1 &&
	    b.damage != 0 && !(b.build_masks & bmask::kUnderAttack) && b.city >= 0 &&
	    !(g.cities[size_t(u.who)][size_t(b.city)].city_flags & cityflag::kUnderAttack)) {
		add_repair_order(g, u, ox, whom, kQueueNew, false);
		return 0;
	}
	if (ord.build_type != ptype::kFarm) {
		if (!(b.build_masks & bmask::kInUse)) {
			b.recharging++;
			b.build_masks |= bmask::kInUse;
		}
		if (!u.guys.empty() && u.guys[0].cur_anim == '%') return 0;  // (CHAR_FARM 37 already playing)
		unit_set_anim(g, u, eanim::kFarm, 0, 1);
		unit_set_angle(g, u, 0x40000000u, false);
		unit_set_new_location(g, u, b.x + 0x120, b.y, true, true);
		return 0;
	}
	// Farm work (farms-spec 3.3).
	const int32_t ftype = farms_get_farm_type(g, farm);
	const Footprint f = footprint(*b.ptype, b.x, b.y);
	const int32_t xs = b.ptype->x_size, ys = b.ptype->y_size;
	int32_t tx, ty;
	if (ftype == 1) {
		unit_set_anim(g, u, eanim::kSow, 0, 1);
		if ((u.o * 7 + g.frame + u.who) % 256 != 0) return 0;
		const int32_t dx = g.rnd(xs / 2);  // G3 (GameAccess::rnd: no draw for <= 1)
		const int32_t dy = g.rnd(ys / 2);
		tx = (f.x0 + 1 + dx) * 0xc0 + 0x60;
		ty = (dy + f.y0 + 1) * 0xc0 + 0x60;
	} else {
		int32_t cx = tcoord_of(u.x) - f.x0, cy = tcoord_of(u.y) - f.y0;
		if (cy < 0 || cx < 0 || cy >= ys || cx >= xs) {
			cx = 1;  // ("Invalid farming location?" assert)
			cy = 1;
		}
		const uint8_t st = (farm >= 0 && size_t(farm) < g.farms.size() && cx < 4 && cy < 4)
		                       ? g.farms[size_t(farm)].status[size_t(cx)][size_t(cy)]
		                       : 0;
		const int8_t ga = u.guys.empty() ? 0 : u.guys[0].cur_anim;
		switch (st) {
		case 0:
			if (ga == '$') break;  // (CHAR_REAP 36 playing)
			[[fallthrough]];
		case 1:
			unit_set_anim(g, u, eanim::kSow, 0, 1);
			farms_grow(g, farm, cy, cx);
			return 0;
		case 2:
			if (ga != '#') {  // (CHAR_SOW 35 playing)
				unit_set_anim(g, u, eanim::kReap, 0, 1);
				farms_snip(g, farm, cy, cx);
				return 0;
			}
			break;
		case 3:
			unit_set_anim(g, u, eanim::kReap, 0, 1);
			return 0;
		default:
			break;
		}
		const int32_t dx = g.rnd(xs);  // G4
		const int32_t dy = g.rnd(ys);
		tx = (f.x0 + dx) * 0xc0 + 0x60;
		ty = (f.y0 + dy) * 0xc0 + 0x60;
	}
	add_move_order(g, u, tx, ty, 1, false, kQueueFirst, false, -1, -1);
	return 0;
}

// ---------------------------------------------------------------- Unit::do_non_flat_gather (4.3)

void do_non_flat_gather(Game& g, Unit& u, Order& ord) {
	const int32_t ox = ord.ox, whom = ord.whom;
	Build& b = *g.build(whom, ox);
	const bool woodcutter = type_is(*b.ptype, ptype::kWoodcutter);
	if (ord.been_there != 0 && !(b.build_masks & bmask::kInUse)) {
		b.recharging++;
		b.build_masks |= bmask::kInUse;
	}
	u.group = -1;
	auto give_up_tile = [&]() {
		ord.g_tx = ord.g_ty = -1;
		ord.goto_build = 1;
		ord.wait = -1;
		if (ord.dist_mod != 0) ord.dist_mod--;
		if (is_worker_t(u.type) && u.doober >= 0) remove_hold_doober(g, u);
	};
	if (ord.goto_build == 0) {
		// At / going to the tile.
		u.unit_masks &= ~umask_econ::kWithStuff;
		if (ord.been_there == 0) {
			ord.been_there = 1;
			LD(g, u.who).flags |= leaderflag::kRecompute;
		}
		if (ord.wait < 0) {
			const int32_t R = min_size_radius(*b.ptype);
			int32_t sx = 0, sy = 0;
			const uint32_t a = find_angle(u.x - b.x, u.y - b.y);
			if (find_nearby_spot(g, *u.ptype, b.x, b.y, &sx, &sy, R, -1, 0, a, kFilterNotMe, u.o, u.who, 0, 0, -1, 0,
			                     -1) != 0) {
				unit_set_anim(g, u, eanim::kChopWood, 0, 1);
				ord.wait = 0x14;
				return;
			}
			unit_set_anim(g, u, kCharDefault, 0, 1);
			Order* keep = &ord;
			add_move_order(g, u, sx, sy, 1, false, kQueueFirst, false, -1, -1);
			keep->goto_build = 1;
			keep->wait = 0x20;
			u.unit_masks |= woodcutter ? umask_econ::kHasWood : umask_econ::kHasMetal;
			return;
		}
		const int8_t ga = u.guys.empty() ? 0 : u.guys[0].cur_anim;
		if (ga == 0x1d) return;  // CHAR_MINE_ORE playing
		if (ga == 0x19) {        // CHAR_CHOP_WOOD playing
			if (--ord.wait != 0) return;
			if (build_all_gathering(g, b)) {
				ord.wait = -1;
				return;
			}
			ord.wait = int32_t(g.rng.get(0, 0xffff) % 100) + 300;  // G6
			return;
		}
		const int32_t tcx = ord.g_tx * 0xc0 + 0x60, tcy = ord.g_ty * 0xc0 + 0x60;
		if (vector_dist(iabs(tcx - u.x), iabs(tcy - u.y)) < 0x140) {
			if (--ord.wait == 0) {
				if (build_all_gathering(g, b)) {
					ord.wait = -1;
					return;
				}
				ord.wait = int32_t(g.rng.get(0, 0xffff) % 50) + 100;  // G7
			}
			const uint32_t a = find_angle(tcx - u.x, tcy - u.y);
			if (a != u.angle) unit_set_angle(g, u, a, false);
			if (u.doober < 0) add_hold_doober(g, u);  // wood at the tile centre, ore halfway (presentation)
			unit_set_anim(g, u, woodcutter ? eanim::kChopWood : eanim::kMineOre, 0, 1);
			return;
		}
		unit_set_anim(g, u, kCharDefault, 0, 1);
		int32_t sx = 0, sy = 0;
		const uint32_t a = find_angle(u.x - tcx, u.y - tcy);
		if (find_nearby_spot(g, *u.ptype, tcx, tcy, &sx, &sy, 0xc0, 0x100, 2, a, kFilterNotMe, u.o, u.who, 0, 0, -1, 0,
		                     -1) != 0) {
			give_up_tile();
			return;
		}
		if ((sx == u.x && sy == u.y) || (sx == u.avoid_x && sy == u.avoid_y)) {
			give_up_tile();
			return;
		}
		if (!build_is_gathered_by(g, b, u.o)) {
			ord.been_there = 0;
			return;
		}
		add_move_order(g, u, sx, sy, 1, false, kQueueFirst, false, -1, -1);
		u.unit_masks |= woodcutter ? umask_econ::kGettingWood : umask_econ::kGettingMetal;
		return;
	}
	if (ord.wait >= 0) {
		// Returning to the building.
		if (unit_adjacent_to(g, u, ox, whom)) {
			ord.wait--;
			if (ord.been_there == 0) {
				ord.been_there = 1;
				LD(g, u.who).flags |= leaderflag::kRecompute;
			}
			if (ord.wait < 0) return;
			unit_set_angle(g, u, find_angle(b.x - u.x, b.y - u.y), false);
			unit_set_anim(g, u, woodcutter ? eanim::kDumpWood : eanim::kDumpOre, 0, 1);
			return;
		}
		if (!build_is_gathered_by(g, b, u.o) && u.ptype->domain == 0) {
			ord.been_there = 0;
			return;
		}
		const int32_t R = min_size_radius(*b.ptype);
		const uint32_t a = find_angle(u.x - b.x, u.y - b.y);
		const int32_t r1 = g.world.get_tregion(tcoord_of(b.x), tcoord_of(b.y));
		const int32_t r2 = g.world.get_tregion(tcoord_of(u.x), tcoord_of(u.y));
		if (r2 != r1 && unit_can_transport(u)) {
			add_move_order(g, u, b.x, b.y, 1, false, kQueueFirst, false, -1, -1);
			return;
		}
		int32_t sx = 0, sy = 0;
		if (find_nearby_spot(g, *u.ptype, b.x, b.y, &sx, &sy, R, -1, 0, a, kFilterNotMe, u.o, u.who, 0, 0, -1, 0, -1) == 0) {
			add_move_order(g, u, sx, sy, 1, false, kQueueFirst, false, -1, -1);
			return;
		}
		if (ord.been_there == 0 && vector_dist(iabs(u.x - b.x), iabs(u.y - b.y)) > 0x600) {
			if (find_nearby_spot(g, *u.ptype, b.x, b.y, &sx, &sy, 0x600, -1, 0, a, kFilterNotMe, u.o, u.who, 0, 0, -1, 0,
			                     -1) != 0) {
				kill_current_order(g, u, false);
				return;
			}
			add_move_order(g, u, sx, sy, 1, false, kQueueFirst, false, -1, -1);
			return;
		}
		ord.wait = -1;
		return;
	}
	// Choose the next tile (goto_build 1, wait < 0).
	if (ord.been_there == 0) {
		ord.been_there = 1;
		LD(g, u.who).flags |= leaderflag::kRecompute;
	}
	unit_set_anim(g, u, kCharDefault, 0, 1);
	const World& w = g.world;
	bool keep = ord.g_tx >= 0 && ord.g_ty >= 0;
	if (keep) {
		keep = econ_has_gather_access(g, ord.g_tx, ord.g_ty, u.who, 1);
	}
	if (!keep) {
		if (is_worker_t(u.type) && u.doober >= 0) remove_hold_doober(g, u);
		int32_t dm = ord.dist_mod;
		const int32_t count = int32_t(b.gather_from.size());
		if (b.mtn >= 0 && count < K("mtn_tiny_size") && dm > 3) dm = 3;
		if (count < 1) return;
		int32_t best = 9999999, bx = -1, by = -1;
		const int32_t btx = tcoord_of(b.x), bty = tcoord_of(b.y);
		for (int32_t i = 0; i < count; i++) {
			const int32_t tx = b.gather_from[size_t(i)][0], ty = b.gather_from[size_t(i)][1];
			if (!w.in_t(tx, ty) || !(w.t(tx, ty) & tmask::kBlocked)) continue;
			if (!econ_has_gather_access(g, tx, ty, u.who, 1)) continue;
			int32_t d = vector_dist(iabs(tx - btx), iabs(ty - bty));
			if (d < 3) d = 3;
			const int32_t s = d * dm + (i >> 2);
			if (s < best) {
				best = s;
				bx = tx;
				by = ty;
			}
		}
		if (best == 9999999) return;
		// Array<TCoordData>::remove (the first equal entry), then append: round robin.
		for (size_t j = 0; j < b.gather_from.size(); j++) {
			if (b.gather_from[j][0] == bx && b.gather_from[j][1] == by) {
				b.gather_from.erase(b.gather_from.begin() + std::ptrdiff_t(j));
				break;
			}
		}
		b.gather_from.push_back({bx, by});
		ord.g_tx = bx;
		ord.g_ty = by;
	}
	ord.wait = int32_t(g.rng.get(0, 0xffff) % 200) + 400;  // G5
	ord.goto_build = 0;
	if (!woodcutter) {
		u.unit_masks |= umask_econ::kGettingMetal;
		ord.wait = 1000000;  // miners never run out of work
		return;
	}
	u.unit_masks |= umask_econ::kGettingWood;
}

// ---------------------------------------------------------------- auto-gather (4.6)

int32_t find_gather_spot(Game& g, Unit& u, int32_t range) {
	// Unit::find_gather_spot @5f5170
	const bool scholar = is_scholar_t(u.type);
	const int32_t r = scholar ? -1 : range;
	const int32_t region = g.world.get_tregion(tcoord_of(u.x), tcoord_of(u.y));
	const int32_t ucity = find_city_at(g, u.x, u.y, u.who, -1, 0);
	const Leader& l = g.leaders[size_t(u.who)];
	int32_t best = -1, bestscore = 0;
	for (int32_t o = kBuildBase; o < g.objects[size_t(u.who)].build_mark; o++) {
		Build* b = g.build(u.who, o);
		if (!b || !b->alive() || !(b->flags & objflag::kActive) || !is_gather_type(*b->ptype)) continue;
		if (b->build_masks & bmask::kNeutralized) continue;
		if (type_is(*b->ptype, ptype::kUniversity) != scholar) continue;
		if (!(build_num_gatherers(g, *b, false, false) < b->gather_max || unit_is_gathering_at(g, u, o, u.who, false)))
			continue;
		if (g.world.get_tregion(tcoord_of(b->x), tcoord_of(b->y)) != region) continue;
		if (!scholar && ucity >= 0 && ucity != b->city) {
			const City& uc = g.cities[size_t(u.who)][size_t(ucity)];
			const uint32_t uu = uint32_t(uc.gatherers) + uint32_t(uc.free);
			if (uu < 2) continue;
			if (b->city >= 0) {
				const City& bc = g.cities[size_t(u.who)][size_t(b->city)];
				if (uu <= uint32_t(bc.free) + 2 + uint32_t(bc.gatherers)) continue;
			}
		}
		const int32_t d = vector_dist(iabs(u.x - b->x), iabs(u.y - b->y));
		if (r >= 1 && d > r) continue;
		int32_t room = 0;
		for (int32_t k = 0; k < 6; k++) {
			if (!type_avail(g, u.who, k, 1)) continue;
			if (!type_is(*b->ptype, best_gather_type(k))) continue;
			if (l.over_cap[size_t(k)] != 0) continue;
			room += l.resource_cap[size_t(k)] - l.income[size_t(k)];
			if (tb(g, u.who, 0x16) && k != 3) room += 0x640;
		}
		const int32_t score = (room * 500) / (d / 0xc0 + 2);
		if (bestscore < score) {
			best = o;
			bestscore = score;
		}
	}
	if (best >= 0 && !unit_is_gathering_at(g, u, best, u.who, false)) {
		add_gather_order(g, u, best, kQueueLast, false);
		return 1;
	}
	return 0;
}

int32_t think_peasant(Game& g, Unit& u, bool force) {
	// Unit::think_peasant @5f5760
	if (!force) {
		int32_t T;
		if (u.unit_masks & umask::kAiControl) {
			T = 1;
		} else {
			switch (g.leaders[size_t(u.who)].opt_peasants) {
			case 1: T = 7; break;
			case 2: T = 0xc; break;
			case 3: T = 0x11; break;
			case 4: T = 0x20; break;
			case 5: T = 0x3e; break;
			default: T = 2; break;
			}
		}
		const int32_t idle = u.idle;
		if (idle < T) return 0;
		if (idle != T && (idle - 2) % 5 != 0) return 0;
	}
	// TODO(AI): think_civilian_transport(1) for AI-controlled citizens.
	if (!is_scholar_t(u.type) &&
	    ((u.unit_masks & umask::kBuilder) || unit_worker_stance(u) == 2 || unit_worker_stance(u) == 1) &&
	    unit_find_build_spot(g, u))
		return 1;
	const int32_t st = unit_worker_stance(u);
	if (st == 1 || st == 0) {
		const int32_t range = (u.unit_masks & umask::kAiControl) ? -1 : K("unit_gather_respond_range") * 0xc0;
		if (find_gather_spot(g, u, range)) {
			// (the local selection drops the unit: presentation)
			if (!(u.unit_masks & umask::kAiControl)) u.group = -1;
			return 1;
		}
	}
	if (is_scholar_t(u.type)) return 0;
	if (!(u.unit_masks & umask::kAiControl) &&
	    ((u.unit_masks & umask::kBuilder) || unit_worker_stance(u) == 2 || unit_worker_stance(u) == 1) &&
	    unit_find_repair_spot(g, u))
		return 1;
	u.unit_masks &= ~umask::kBuilder;
	if (u.unit_masks & umask::kAiControl) {
		// TODO(AI): think_scout when the region has no own city and no site; then find_repair_spot.
		if (unit_find_repair_spot(g, u)) return 1;
	}
	return 0;
}

void do_think_order(Game& g, Unit& u) {
	// Unit::do_think_order @5e5bf0
	kill_current_order(g, u, false);
	if (!u.orders.empty()) return;
	if (is_worker_t(u.type)) think_peasant(g, u, true);
}

void think_economy(Game& g, Unit& u) {
	// Unit::think @5f6e40 from LAB_005f7179 (after the combat part). Every "goto LAB_005f761a" clears NEW_THINK.
	const Leader& l = g.leaders[size_t(u.who)];
	auto done = [&]() { u.flags &= uint8_t(~objflag::kNewThink); };
	if (!(l.flags & leaderflag::kActive)) return;
	if (is_worker_t(u.type) && think_peasant(g, u, false)) return done();
	if ((u.ptype->unit_flags2 & 8) && think_caravan(g, u, false)) return done();
	if (unit_is_rare_collector(g, u) && (l.flags & leaderflag::kHuman) && (u.unit_masks & umask::kPacked)) {
		if (u.idle == 1 || (u.o + g.frame) % 32 == 0) {
			if (!(u.unit_masks & umask::kAutoExplore) || unit_is_merchant(u)) {
				int32_t tmp[6];
				if (unit_collect(g, u, tmp, nullptr, nullptr, true)) {
					if (unpack_merchant(g, u, 4)) return done();
					// (the "rare collector idle" message for the local player on the first idle frame)
				}
				u.idle = uint8_t(u.idle + 1);
			}
		}
	}
	// LAB_005f730d (GameAccess::ai_off is 0 in a normal game)
	if (l.flags & leaderflag::kHuman) {
		if (!(u.unit_masks & umask::kAutoExplore)) {
			if (u.ptype->unit_flags2 & 0x10) {
				// A human's special forces counter-spy on the first idle frame or every 32 frames (ai-spec 12.6 step 8).
				if ((u.idle == 1 || (u.o + g.frame) % 32 == 0) && ai_think_spellcaster(g, u)) return done();
				if (u.idle == 1 && (u.group < 0 || g.groups.g[size_t(u.group)].num == 1) &&
				    (u.unit_masks & umask::kMultimove))
					u.unit_masks &= ~umask::kMultimove;  // (the "scout idle" message: presentation)
			}
		} else {
			think_scout(g, u, 0);  // Unit::think_scout (ai-spec 12.6.2)
		}
		if (!(u.unit_masks & umask::kAiControl)) return done();
	}
	if (u.idle != 1 && (u.o + g.frame) % 32 != 0) return done();
	if (type_is(*u.ptype, etype::kFishermen) && think_fish(g, u)) return done();
	if ((u.idle == 1 || (u.o + g.frame) % 128 == 0) && unit_is_merchant(u) && think_merchant(g, u)) return done();
	if (l.flags & leaderflag::kCoopSolo) return done();
	think_ai(g, u, true);  // steps 12-16: carriers, supply wagons / heroes, scouts and spies (ai-spec 12.6)
	done();
}

// ---------------------------------------------------------------- group commands (4.1)

int32_t group_count(const Game& g, const Group& gr, int32_t kind, int32_t p2) {
	// GroupData::count @711720 for COUNT_TYPE (17), COUNT_GATHER_WORKERS (12), COUNT_GATHER_SCHOLARS (13),
	// COUNT_CAN_GATHER (15), COUNT_CAN_BE_SCHOLARS (31). TODO(spells, transports): the civilian spell
	// (SpellTypeData::is_castable 659) and carried citizens of transports.
	int32_t n = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* ob = g.object(gr.who, gr.list[size_t(i)]);
		if (!ob || !ob->alive()) continue;
		switch (kind) {
		case 17:
			n += type_is(*ob->ptype, p2) ? 1 : 0;
			break;
		case 12:
			if (ob->is_unit && is_worker_t(ob->type) && action_type(*static_cast<const Unit*>(ob)) == kGather) n++;
			break;
		case 13:
			if (ob->is_unit && is_scholar_t(ob->type) && action_type(*static_cast<const Unit*>(ob)) == kGather) n++;
			break;
		case 15:
			n += is_peasant_t(ob->type) ? 1 : 0;
			break;
		case 31:
			if (is_scholar_t(ob->type)) n++;
			if (ob->is_unit && type_is(*ob->ptype, ptype::kMilitia) && is_scholar_t(static_cast<const Unit*>(ob)->rare)) n++;
			break;
		default:
			break;
		}
	}
	return n;
}

void group_action_gather(Game& g, Group& gr, int32_t ox, QueuePos q) {
	// Group::action_gather @700b90
	Group tmp;
	group_clear(tmp, -1);
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	gr.disband = 0;  // action_begin (vslot +0x14)
	gr.form = -1;
	const QueuePos q2 = q == kQueueFirst ? kQueueNew : q;
	const int32_t who = gr.who;
	Object* T = g.object(who, ox);
	if (!T) return;
	const int32_t cx = T->x, cy = T->y;
	const World& w = g.world;
	const int32_t region = w.get_tregion(tcoord_of(cx), tcoord_of(cy));
	group_clear(tmp, -1);
	const bool university = type_is(*T->ptype, ptype::kUniversity);
	int32_t idle_workers;
	if (!university) {
		idle_workers = group_count(g, gr, 15, 0) - group_count(g, gr, 12, 0) - group_count(g, gr, 31, 0);
	} else {
		idle_workers = group_count(g, gr, 17, ptype::kScholars) - group_count(g, gr, 13, 0);
	}
	if (!T->is_unit) {
		const int32_t gd = building_good(T->type);
		if (gd >= 0 && !type_avail(g, who, gd, 1)) return;
	}
	const uint32_t lflags = LD(g, who).flags;
	const bool transport = (lflags & 0x700) != 0;
	int32_t spiral = 0;
	int32_t i = gr.num - 1;
	while (i >= 0) {
		const int32_t mo = gr.list[size_t(i)];
		Unit* u = g.unit(who, mo);
		if (!u || !u->alive() || !u->on_map()) {
			i--;
			continue;
		}
		const int32_t ut = u->rare < 0 ? u->type : u->rare;  // (+0x54: a scholar militia's previous type)
		const bool sch = is_scholar_t(u->type) || is_scholar_t(ut);
		bool worker;
		if (!university) {
			if (sch) {
				group_add(g, tmp, mo, who, 0, 0);
				i--;
				continue;
			}
			worker = is_peasant_t(u->type) || spell_is_castable(g, ptype::kCivilianSpell, mo, who, 0) != 0 ||
			         (transport && count_inside_type(g, *u, ptype::kPeasants, false) != 0);
		} else {
			worker = sch;
		}
		if (!worker) {
			group_add(g, tmp, mo, who, 0, 0);
			i--;
			continue;
		}
		if (idle_workers != 0 && action_type(*u) == kGather) {
			i--;
			continue;
		}
		const int32_t ureg = w.get_tregion(tcoord_of(u->x), tcoord_of(u->y));
		if (!transport && ureg != region) {
			i--;
			continue;
		}
		// The spiral over wcoords around the target: the first own active gather building linked at a probe.
		bool assigned = false;
		for (;;) {
			const int32_t px = move_x[size_t(spiral)] * 0x300 + cx, py = move_y[size_t(spiral)] * 0x300 + cy;
			spiral++;
			if (spiral > 0x78) {
				for (; i >= 0; i--) {
					const int32_t o2 = gr.list[size_t(i)];
					Unit* u2 = g.unit(who, o2);
					if (!u2 || !u2->alive() || !u2->on_map()) continue;
					if (idle_workers != 0 && action_type(*u2) == kGather) continue;
					group_add(g, tmp, o2, who, 0, 0);
				}
				goto done;
			}
			if (px < 0 || py < 0 || px >= w.tile_xs * 0xc0 || py >= w.tile_ys * 0xc0) continue;
			if (!transport && w.get_tregion(tcoord_of(px), tcoord_of(py)) != region) continue;
			int32_t lo = w.w(wcoord_of(px), wcoord_of(py)).down, lw = w.w(wcoord_of(px), wcoord_of(py)).down_who;
			for (int guard = 0; lo >= 0 && guard < 10000; guard++) {
				const Object* ob = g.object(lw, lo);
				if (!ob) break;
				const int32_t no = ob->down, nw = ob->down_who;
				if (lw == who && !ob->is_unit && (ob->flags & objflag::kActive) && is_gather_type(*ob->ptype)) {
					u->group = -1;
					const bool can_civ = spell_is_castable(g, ptype::kCivilianSpell, mo, who, 0) != 0;
					if (q2 == kQueueLast) {
						// an armed citizen disarms first (CIVILIAN, QUEUE_LAST), then gathers
						if (can_civ) add_cast_order(g, *u, -1, -1, u->x, u->y, ptype::kCivilianSpell, kQueueLast, true);
						add_gather_order(g, *u, lo, kQueueLast, true);
					} else {
						add_gather_order(g, *u, lo, kQueueNew, true);
						if (can_civ) add_cast_order(g, *u, -1, -1, u->x, u->y, ptype::kCivilianSpell, kQueueFirst, true);
						// (else: editor-only oil-platform boarding)
					}
					group_kill(g, gr, mo, who, 0, 0);
					assigned = true;
					break;
				}
				lo = no;
				lw = nw;
			}
			if (assigned) break;
		}
		i--;
		spiral = 0;
	}
done:
	if (group_get_num(g, tmp) != 0)
		group_action_move_near(g, tmp, cx, cy, 0xc0, kQueueNew, false, 0, kMoveTo, 0, -1, -1, 0);
}

void group_action_city_gather(Game& g, Group& gr, int32_t t, QueuePos q) {
	// Group::action_city_gather @701780 (the queue position is not used).
	(void)q;
	gr.disband = 0;
	const int32_t who = gr.who;
	for (int32_t i = 0; i < gr.num; i++) {
		const int32_t mo = gr.list[size_t(i)];
		Build* c = g.build(who, mo);
		if (!c || !c->alive() || !(c->flags & objflag::kCity) || !(c->flags & objflag::kActive)) continue;
		build_clear_gather(g, *c);
		for (int32_t o = mo, guard = 0; o >= 0 && guard < 1000; guard++) {
			Build* b = g.build(who, o);
			if (!b) break;
			const int32_t next = b->city_down;
			if (b->alive()) {
				const bool match = t < 0 ? (type_is(*b->ptype, ptype::kFarm) || type_is(*b->ptype, ptype::kWoodcutter) ||
				                            type_is(*b->ptype, ptype::kMine))
				                         : type_is(*b->ptype, t);
				if (match) build_add_gather_point(g, *c, b->x, b->y, 1, kQueueLast);
			}
			o = next;
		}
	}
	// (When no point was set for the local player, the original clears the last member's gather points again: a
	// seat-local write, economy-spec 4.1. Not reproduced.)
}

// ---------------------------------------------------------------- farms (farms-spec)

int32_t farms_get_farm_type(const Game& g, int32_t farm) {
	if (farm < 0 || size_t(farm) >= g.farms.size()) return 0;
	return g.farms[size_t(farm)].farm_type & 1;
}

void farms_grow(Game& g, int32_t farm, int32_t y, int32_t x) {
	// Farms::grow(farm, y, x) @8d91c0
	if (farm < 0 || size_t(farm) >= g.farms.size() || y >= 4 || x >= 4 || x < 0 || y < 0) return;
	FarmRec& f = g.farms[size_t(farm)];
	f.status[size_t(x)][size_t(y)] = 1;
	float p = f.percent[size_t(x)][size_t(y)] + 0.005f;
	if (1.0f < p) {
		p = 1.0f;
		f.status[size_t(x)][size_t(y)] = 2;
	}
	f.percent[size_t(x)][size_t(y)] = p;
}

void farms_snip(Game& g, int32_t farm, int32_t y, int32_t x) {
	// Farms::snip(farm, y, x) @8d9240
	if (farm < 0 || size_t(farm) >= g.farms.size() || y >= 4 || x >= 4 || x < 0 || y < 0) return;
	FarmRec& f = g.farms[size_t(farm)];
	if (f.status[size_t(x)][size_t(y)] == 2) f.status[size_t(x)][size_t(y)] = 3;
}

void farms_inc_time(Game& g) {
	// Farms::inc_time @8d8600 (from Objects::inc_time): growth, decay and the random sprouting draws (G9).
	for (size_t i = 0; i < g.farms.size(); i++) {
		FarmRec& f = g.farms[i];
		if (f.farm_type == 1) continue;  // (an animal pen with the ambience bit would pass: no pen gets it)
		if (f.who >= 0 && f.o >= 0) {
			const Build* b = g.build(f.who, f.o);
			if (!b || !(b->flags & objflag::kActive)) continue;
		}
		int32_t bare = 0;
		for (int32_t y = 0; y < 4; y++) {
			for (int32_t x = 0; x < 4; x++) {
				float p = f.percent[size_t(x)][size_t(y)];
				uint8_t& st = f.status[size_t(x)][size_t(y)];
				if (st == 1) p = p + 0.005f;
				if (st == 3) p = p - 0.01f;
				if (0.0f < p) {
					if (1.0f <= p) {
						st = 2;
						p = 1.0f;
					}
				} else {
					st = 0;
					p = 0.0f;
					bare++;
				}
				f.percent[size_t(x)][size_t(y)] = p;
			}
		}
		int32_t chance;
		if (bare < 12) {
			if (bare < 5) continue;
			chance = ((bare - 4) * 20) / 8;
			if (chance < 1) continue;
		} else {
			chance = 20;
		}
		if (int32_t(g.rng.get(0, 0xffff) % 1000) < chance) {
			int32_t pick = (bare == 1 || bare - 1 < 0) ? 0 : int32_t(g.rng.get(0, 0xffff) % uint32_t(bare));
			bool done = false;
			for (int32_t y = 0; y < 4 && !done; y++) {
				for (int32_t x = 0; x < 4 && !done; x++) {
					if (f.percent[size_t(x)][size_t(y)] == 0.0f) {
						if (pick == 0) {
							f.status[size_t(x)][size_t(y)] = 1;
							done = true;
						}
						pick--;
					}
				}
			}
		}
	}
}

int32_t farms_add(Game& g, Build& b) {
	// Farms::add(who, o) @8d8a40: the wheat / pen choice (farms-spec 2.1) with its game_random draws, the wheat
	// ambience bit (two draws), and the slot (first invalid one, else appended).
	FarmRec f;
	f.valid = true;
	f.who = b.who;
	f.o = b.o;
	const int32_t city = b.city;
	int32_t wheat_others = 0;
	bool pen = false, skip_heights = false;
	if (city < 0) {
		if (b.o & 1) {
			pen = true;
			skip_heights = true;
		}
	} else {
		const int32_t n = city_count_buildings(g, b.who, city, ptype::kFarm, 1, 0) - 1;
		// CityData::count_farms @7368c0: live farms of the city with a farm slot that is not a pen.
		int32_t wcount = 0;
		for (int32_t o = g.cities[size_t(b.who)][size_t(city)].o, guard = 0; o >= 0 && guard < 1000; guard++) {
			const Build* c = g.build(b.who, o);
			if (!c) break;
			o = c->city_down;
			if (!c->alive() || !type_is(*c->ptype, ptype::kFarm) || c->fort < 0) continue;
			if (size_t(c->fort) >= g.farms.size() || !(g.farms[size_t(c->fort)].farm_type & 1)) wcount++;
		}
		wheat_others = wcount;
		bool decided = false;
		if (n == wcount) {
			if (n == 4 && wcount == 4) {
				pen = true;
				decided = true;
				skip_heights = true;
			}
			if (!decided) {
				// FarmsData::get_nearest_farm_type: ObjectsData::find_any_building(SEARCH_ALL, radius 0x480,
				// FILTER_TYPE FARM, not me). TODO(spec): find_any_building is not decoded; the nearest farm of
				// leaders 0..7 (slot order, strict) by vector_dist.
				int32_t bestd = 0x7fffffff, found_who = -1, found_o = -1;
				for (int32_t p = 0; p < 8; p++) {
					for (int32_t o = kBuildBase; o < g.objects[size_t(p)].build_mark; o++) {
						const Build* c = g.build(p, o);
						if (!c || !c->alive() || c == &b || !type_is(*c->ptype, ptype::kFarm, true)) continue;
						const int32_t d = vector_dist(iabs(c->x - b.x), iabs(c->y - b.y));
						if (d <= 0x480 && d < bestd) {
							bestd = d;
							found_who = p;
							found_o = o;
						}
					}
				}
				if (found_o >= 0) {
					const Build* c = g.build(found_who, found_o);
					if (c->fort >= 0) {
						pen = farms_get_farm_type(g, c->fort) != 0;
						decided = true;
						if (pen) skip_heights = true;
					}
				}
			}
			if (!decided && n == 3) {
				if (wcount == 0) decided = true;  // wheat
				else if (wcount == 3) {
					pen = true;
					decided = true;
					skip_heights = true;
				}
			}
			if (!decided) {
				const uint32_t r = g.rng.get(0, 0xffff) & 3;
				if (r > 2) {
					pen = true;
					skip_heights = true;
				}
			}
		}
	}
	f.farm_type = pen ? 1 : 0;
	if (!skip_heights && wheat_others > 1) {
		// The ambience sound: unless a farm of the city already has it, two draws for its position.
		bool has = false;
		for (int32_t o = g.cities[size_t(b.who)][size_t(city)].o, guard = 0; o >= 0 && guard < 1000; guard++) {
			const Build* c = g.build(b.who, o);
			if (!c) break;
			o = c->city_down;
			if (type_is(*c->ptype, ptype::kFarm) && c->fort >= 0 && size_t(c->fort) < g.farms.size() &&
			    (g.farms[size_t(c->fort)].farm_type & 4)) {
				has = true;
				break;
			}
		}
		if (!has) {
			(void)g.rng.get(0, 0xffff);  // (GraphicEvents::add_ambience position: x % 3)
			(void)g.rng.get(0, 0xffff);  // (y % 3)
			f.farm_type |= 4;
		}
	}
	// (terrain_height[5][5] from TerrainOut::find_data_z: presentation, not kept)
	for (size_t i = 0; i < g.farms.size(); i++) {
		if (!g.farms[i].valid) {
			g.farms[i] = f;
			return int32_t(i);
		}
	}
	g.farms.push_back(f);
	return int32_t(g.farms.size()) - 1;
}

void farms_remove(Game& g, int32_t farm) {
	if (farm >= 0 && size_t(farm) < g.farms.size()) g.farms[size_t(farm)].valid = false;  // (ambience: presentation)
}

void farms_add_animals(Game& g, int32_t farm) {
	// Farms::add_animals(farm) @8d8f30: five gaia pigs or chickens around a finished pen (three draws each).
	if (farm < 0 || size_t(farm) >= g.farms.size()) return;
	const FarmRec f = g.farms[size_t(farm)];
	if (f.farm_type != 1) return;
	const Build* b = g.build(f.who, f.o);
	if (!b || !(b->flags & objflag::kActive)) return;
	for (int32_t i = 0; i < 5; i++) {
		const uint32_t r = g.rng.get(0, 0xffff);
		const int32_t bx = b->x, by = b->y;
		const int32_t y = int32_t(g.rng.get(0, 0xffff) % 0x180) - 0xc0 + by;
		const int32_t x = (bx - 0xc0) + int32_t(g.rng.get(0, 0xffff) % 0x180);
		const int32_t t = (r & 1) == 0 ? etype::kFarmChicken : etype::kFarmPig;
		const int32_t o = init_unit(g, kGaiaWho, t, x, y, -1, -1, -1);
		if (o < 0) continue;
		Unit* a = g.unit(kGaiaWho, o);
		a->animal_ox = int16_t(f.o);
		a->animal_whom = int16_t(f.who);
		a->animal_aid = int8_t(i);
		// TODO(animals): Animal::think_farm_animal (farm animals wander in their pen).
	}
}

// ---------------------------------------------------------------- oil wells (7.4)

int32_t oil_wells_init(Game& g, Build& b) {
	// OilWells::init_oil_well(who, o) @73f6f0: the first unused slot below the mark, else a new one.
	auto& v = LD(g, b.who).oil_wells;
	for (size_t i = 0; i < v.size(); i++) {
		if (v[i] < 0) {
			v[i] = b.o;
			return int32_t(i);
		}
	}
	v.push_back(b.o);
	return int32_t(v.size()) - 1;
}

void oil_wells_close(Game& g, int32_t who, int32_t slot) {
	auto& v = LD(g, who).oil_wells;
	if (slot < 0 || size_t(slot) >= v.size()) return;
	v[size_t(slot)] = -1;
	while (!v.empty() && v.back() < 0) v.pop_back();  // the mark shrinks over trailing unused records
}

}  // namespace ron::sim
