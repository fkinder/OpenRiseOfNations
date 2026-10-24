// Attack orders and getting into range (docs/combat-spec.md section 3): Unit::add_attack_order, find_def_pos,
// get_activity, do_attack, check_target_path, find_attack_pos (the perimeter walk with its % 192 draw),
// find_melee_pos / find_open_slots, attack-move scans, and the group level (action_attack, distribute_attack,
// do_group_attack(_to), is_attacking_near / is_attacking_to).
#include <algorithm>

#include "sim/aircraft.hpp"
#include "sim/combat_internal.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/search.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace combat_detail;

namespace {

bool order_is_move_attack(OrderType t) { return order_is_move(t) || order_is_attack(t); }

void restrict_xy(const Game& g, int32_t* x, int32_t* y) {
	*x = std::clamp(*x, 0, g.world.xs * 768 - 1);
	*y = std::clamp(*y, 0, g.world.ys * 768 - 1);
}
bool world_is_valid(const Game& g, int32_t x, int32_t y) {
	return x >= 0 && y >= 0 && x < g.world.xs * 768 && y < g.world.ys * 768;
}
int32_t tregion_at(const Game& g, int32_t x, int32_t y) {
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	return g.world.in_t(tx, ty) ? g.world.get_tregion(tx, ty) : -1;
}
// The target's W cell at the unit's offset inside its own W cell (find_attack_pos 3.8.3 / 3.8.6).
void wcell_offset(const Unit& u, const Object& t, int32_t* ox, int32_t* oy) {
	*ox = wcoord_of(t.x) * 768 + 384 + (u.x % 768) - 384;
	*oy = wcoord_of(t.y) * 768 + 384 + (u.y % 768) - 384;
}

bool grp_on_map(Game& g, const Group& gr) {
	if (gr.num == 0) return false;
	if (gr.buildings) return true;
	for (int32_t i = 0; i < gr.num; i++) {
		const Unit* u = g.unit(gr.who, gr.list[size_t(i)]);
		if (u && u->alive() && u->o_up < 0 && u->on_map()) return true;
	}
	return false;
}

// Unit::kill_group_order: drop the group order `id` (moves through kill_group_move, GROUP_ATTACK at the front).
void unit_kill_group_order(Game& g, Unit& u, int32_t id) {
	unit_kill_group_move(g, u, id);
	const Order* f = u.front();
	if (f && order_is_group(f->type) && f->id == id) kill_current_order(g, u);
}

void group_kill_group_order(Game& g, Group& gr, int32_t id) {
	// Group::kill_group_order @710aa0
	group_normalize(g, gr);
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (m && m->alive() && m->on_map()) unit_kill_group_order(g, *m, id);
	}
}

// GroupData::get_max_range @711080 (siege = 1: siege members only; 0: non-siege only).
int32_t group_get_max_range(Game& g, const Group& gr, int32_t siege) {
	int32_t best = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		Object* ob = g.object(gr.who, gr.list[size_t(i)]);
		if (!ob) continue;
		bool take;
		if (!gr.buildings) {
			const Unit* m = as_unit(ob);
			if (!m || !m->alive() || !m->on_map() || unit_is_plane(*m)) continue;
			take = siege ? type_is_siege(*m->ptype) : !type_is_siege(*m->ptype);
		} else {
			take = !ob->is_unit && siege == 0;
		}
		if (take && ob->ptype->attack != 0) best = std::max(best, obj_max_range(g, *ob));
	}
	return best;
}

bool group_is_moving_to(Game& g, Group& gr) {
	// Group::is_moving_to @713bd0: every valid on-map member with orders has a MOVE_TO / GROUP_MOVE front.
	group_normalize(g, gr);
	for (int32_t i = 0; i < gr.num; i++) {
		const Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!m || !m->alive() || !m->on_map() || m->orders.empty()) continue;
		const OrderType t = m->order_type();
		if (t != kMoveTo && t != kGroupMove) return false;
	}
	return true;
}

// Unit::find_open_slots @600e30: free U cells on a square around the target, in visit order.
bool find_open_slots(Game& g, Unit& u, const Object& t, std::vector<std::array<int32_t, 2>>& slots) {
	const int32_t a = u.ptype->new_block_radius, b = t.ptype->new_block_radius;
	const int32_t Rc = a + 1 + b + (type_is(*u.ptype, 132) ? 3 : 0);
	const int32_t step = 2 * a + 1;
	const int32_t tu = ucoord_of(t.x), tv = ucoord_of(t.y);
	int32_t cu = tu - Rc, cv = tv + Rc;
	int32_t dir = 1;
	bool any = false;
	for (int guard = 0; guard < 100000; guard++) {
		if (cu >= 0 && cv >= 0 && cu < g.world.xs * 16 && cv < g.world.ys * 16 &&
		    unit_invalid_loc(g, u, cu >> 2, cv >> 2, false, false, false, false, true) == 0) {
			const int32_t px = cu * 48 + 24, py = cv * 48 + 24;
			if (!find_collision(g, px, py, u.o, u.who, 0) && !find_ordered_collision(g, px, py, u.o, u.who)) {
				slots.push_back({px, py});
				any = true;
			}
		}
		cu += orthog_x[dir] * step;
		cv += orthog_y[dir] * step;
		if (iabs(cu - tu) > Rc) {
			cu = cu <= tu ? tu - Rc : tu + Rc;
			if (++dir > 4) return any;
			cu += orthog_x[dir] * step;
			cv += orthog_y[dir] * step;
		} else if (iabs(cv - tv) > Rc) {
			cv = tv < cv ? tv + Rc : tv - Rc;
			if (++dir > 4) return any;
			cu += orthog_x[dir] * step;
			cv += orthog_y[dir] * step;
		}
	}
	return any;
}

// Unit::find_melee_pos @6010b0
int32_t find_melee_pos(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t* rx, int32_t* ry, int32_t x, int32_t y) {
	Object& t = *obj_at(g, ox, whom);
	std::vector<std::array<int32_t, 2>> slots;
	if (!find_open_slots(g, u, t, slots) || slots.empty()) {
		*rx = t.x;
		*ry = t.y;
		if (Unit* tu = as_unit(&t)) tu->full = int8_t(std::min(tu->full + 5, 30));
		return 0;
	}
	int32_t best = 99999999;
	*rx = slots[0][0];
	*ry = slots[0][1];
	for (const auto& s : slots) {
		const int32_t d = vector_dist(s[0] - x, s[1] - y);
		if (d < best) {
			best = d;
			*rx = s[0];
			*ry = s[1];
		}
	}
	return 1;
}

}  // namespace

// ---------------------------------------------------------------- activity / defend point

const Order* get_activity(const Unit& u) {
	// @608370: the first order from the front that is neither a move nor an attack (the last order stops the walk),
	// or an ATTACK_TO / GROUP_ATTACK_TO.
	if (u.orders.empty()) return nullptr;
	size_t i = 0;
	const size_t last = u.orders.size() - 1;
	while (order_is_move_attack(u.orders[i].type) && i != last) i++;
	const Order& c = u.orders[i];
	if (!order_is_move_attack(c.type) || c.type == kAttackTo || c.type == kGroupAttackTo) return &c;
	return nullptr;
}

bool find_def_pos(const Game& g, const Unit& u, int32_t* x, int32_t* y) {
	(void)g;
	// @608560
	const Order* a = get_action(u);
	if (a && a->type == kAttack && a->defensive) {
		*x = a->def_x;
		*y = a->def_y;
		return true;
	}
	const Order* f = u.front();
	if (f && f->type != kOrderNone && (f->flags & orderflag::kReturnHome) && order_is_move(f->type)) {
		*x = f->x;
		*y = f->y;
		return true;
	}
	*x = uc(u.x);
	*y = uc(u.y);
	return false;
}

Order* add_attack_order(Game& g, Unit& u, int32_t ox, int32_t whom, QueuePos q, int32_t mandatory, bool expl) {
	// @5e5410
	if (q == kQueueNew) clear_orders(g, u);
	Order a;
	a.type = kAttack;
	a.ox = ox;
	a.whom = whom;
	const Object* t = obj_at(g, ox, whom);
	a.uid = (ox >= 0 && whom >= 0 && t) ? t->uid : 0xffff;
	a.mandatory = int8_t(mandatory);
	a.in_range = a.ever_in_range = 0;
	a.new_ord = 1;
	if (get_combat_stance(g, u) == kDefensive && !expl) {
		a.defensive = 1;
		find_def_pos(g, u, &a.def_x, &a.def_y);
	} else {
		a.defensive = 0;
		a.def_x = a.def_y = -1;
	}
	a.flags = uint8_t((a.flags & ~orderflag::kFlag80) | (expl ? orderflag::kExplicit : 0));
	return add_order(g, u, a, q == kQueueNew ? kQueueLast : q);
}

// ---------------------------------------------------------------- do_attack

int32_t do_attack(Game& g, Unit& u, Order& a) {
	// @5f1b80
	const int32_t ox = a.ox, whom = a.whom, mand = a.mandatory;
	Object* T = obj_at(g, ox, whom);
	if (type_is(*u.ptype, 351, true)) {
		// Aircraft carrier (@5f1b80): every 16 frames each plane inside gets a STRAFE at the target (not at a building
		// unless the attack is mandatory); the carrier itself does not fire while the planes fly.
		if (!T || !T->alive()) {
			kill_current_order(g, u);
			return 0;
		}
		if ((g.frame & 0xf) != 0) return 0;
		if (!T->is_unit && !mand) return 0;
		int32_t po = u.inside_down, pw = u.inside_down_who;
		for (int guard = 0; po >= 0 && pw >= 0 && guard < kUnitLimit; guard++) {
			Unit* pl = g.unit(pw, po);
			if (!pl) break;
			add_strafe_order(g, *pl, ox, whom, u.o, u.who, 1, kQueueNew, mand != 0);
			po = pl->inside_down;
			pw = pl->inside_down_who;
		}
		return 0;
	}
	if (u.ptype->attack == 0) {
		if (type_is(*u.ptype, 320)) {
			// Transport barge carrying an attacker. TODO(transports): find_attack_pos for the cargo.
			kill_current_order(g, u);
			return 0;
		}
		if (u.group >= 0) {
			Group& gr = g.groups.g[size_t(u.group)];
			const bool worker = is_worker_type(u.type);
			const int32_t r = group_get_max_range(g, gr, worker ? 1 : 0);
			const bool supply = (u.ptype->unit_flags2 & utflag2::kSupply) != 0;
			int32_t D;
			if (r != 0) D = (r + 2) * 192 + (supply ? 192 : 0) + (worker ? 192 : 0);
			else D = (group_get_max_range(g, gr, 1) + 1) * 192 + (supply ? 384 : 0) + (worker ? 384 : 0);
			if (!T || !T->alive() || !valid_target(g, u, ox, whom, nullptr)) {
				kill_current_order(g, u);
				return 0;
			}
			if (!T->is_unit) D += (T->ptype->x_size + T->ptype->y_size) * 48;
			if (vector_dist(T->x - u.x, T->y - u.y) <= D + 0x240) {
				kill_current_order(g, u);
				return 0;
			}
			int32_t px = 0, py = 0;
			project(T->x, T->y, find_angle(u.x - T->x, u.y - T->y), D + 192, &px, &py);
			const int16_t sg = u.group;
			u.group = -1;
			int32_t sx = 0, sy = 0;
			const int32_t nf = find_nearby_spot(g, *u.ptype, px, py, &sx, &sy, 0, -1, 0, 0x55555555u, kFilterNotMe, u.o,
			                                    u.who, 0, 0, -1, 0, -1);
			u.group = sg;
			if (nf == 0 && world_is_valid(g, sx, sy) && tregion_at(g, T->x, T->y) == tregion_at(g, sx, sy)) {
				kill_current_order(g, u);
				add_move_order(g, u, sx, sy, 1, false, kQueueFirst, false, -1, -1);
				return 0;
			}
			kill_current_order(g, u);
			return 0;
		}
	}
	const bool mar = (u.ptype->unit_flags & utflag::kMeleeAndRanged) != 0;
	if (mand) {
		if (u.ptype->unit_flags & utflag::kFireWhileMoving) {
			u.cavarch_o = int16_t(ox);
			u.cavarch_who = int8_t(whom);
			if (ox >= 0 && whom >= 0 && T && T->alive()) {
				u.unit_masks2 |= umask2::kKeepTarget;
				u.cavarch_uid = T->uid;
			} else {
				u.unit_masks2 &= ~umask2::kKeepTarget;
			}
		}
	} else if (mar && get_combat_stance(g, u) == kStandGround) {
		const int32_t st = g.range_swap_type, sv = g.range_swap_value;
		g.range_swap_type = u.ptype->type;
		g.range_swap_value = u.ptype->second_max_range;
		fight(g, u, ox, whom, mand, 0, 0);
		g.range_swap_type = st;
		g.range_swap_value = sv;
		return 0;
	}
	const int32_t r = fight(g, u, ox, whom, mand, 0, 0);
	if (r != 0 && mar && u.alive()) {
		const uint8_t rc = u.recharging;
		u.recharging = 0;
		cavarch_fight(g, u);
		u.recharging = rc;
	}
	return 0;
}

// ---------------------------------------------------------------- check_target_path

int32_t check_target_path(Game& g, Unit& u, Order& act) {
	// @5e22d0. `act` is the targeted action behind the front move.
	const int32_t ox = act.ox, whom = act.whom;
	const OrderType t = act.type;
	if (ox < 0) return 0;
	Object* T = obj_at(g, ox, whom);
	if (!T || !obj_is_seen(g, *T, u.who, 0)) return 0;
	if (t == kCastSpell) {
		// TODO(spells): SpellTypeData::is_valid_target.
		if (!T->alive()) {
			repath(g, u);
			kill_current_order(g, u);
			return 1;
		}
		if (T->is_unit && unit_is_moving(static_cast<Unit&>(*T))) {
			repath(g, u);
			return 1;
		}
		return 0;
	}
	if (t == kBoardShip) return 0;
	if (t == kRepair && !T->is_unit) return 0;
	if (T->is_unit) {
		const Unit& tu = static_cast<const Unit&>(*T);
		const uint32_t a = find_angle(T->x - u.x, T->y - u.y);
		const uint32_t d = tu.angle - (a - 0x80000000u);
		const bool front = d < 0x2aaaaaaau || flanking(d) == 0;
		if (front || !unit_is_moving(tu)) {
			if (is_in_range(g, u, ox, whom, 0)) {
				repath(g, u);
				return 1;
			}
			return 0;
		}
	}
	if (T->is_unit && !static_cast<Unit&>(*T).on_map()) {
		repath(g, u);
		return 1;
	}
	if (t == kGarrison) {
		if (object_adjacent_to(g, u, ox, whom)) {
			repath(g, u);
			return 1;
		}
		return 0;
	}
	if (is_in_range(g, u, ox, whom, u.orders_x, u.orders_y, 0, nullptr)) return 0;
	if (!order_is_attack(t)) {
		if (t == kFollow && T->is_unit) {
			const Order* f = u.front();
			if (f && order_is_move(f->type) && vector_dist(T->x - f->x, T->y - f->y) > 0x900) {
				repath(g, u);
				return 1;
			}
		}
		return 0;
	}
	if (raw_max_range(g, *u.ptype) == 0 && u.path.size() == 1) return 0;
	if (is_wall(*T)) return 0;
	const Order* f = u.front();
	if (f && order_is_group(f->type) && u.group >= 0) {
		Group& gr = g.groups.g[size_t(u.group)];
		const int32_t mand = act.mandatory;
		if (group_is_moving_to(g, gr)) {
			repath(g, u);
			if (mand >= 1 && T->is_unit && static_cast<Unit&>(*T).on_map()) {
				// Group::kill_next_order: every member drops its front order.
				for (int32_t i = 0; i < gr.num; i++) {
					Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
					if (m && m->alive() && m->on_map()) kill_current_order(g, *m);
				}
				group_action_attack(g, gr, ox, whom, 1, kQueueFirst, 0);
			}
		} else {
			while (!u.orders.empty() && order_is_move(u.order_type())) kill_current_order(g, u);
		}
		return 1;
	}
	const int32_t mand = act.mandatory, defensive = act.defensive, dx = act.def_x, dy = act.def_y;
	const bool expl = (act.flags & orderflag::kExplicit) != 0;
	repath(g, u);
	int32_t px = 0, py = 0;
	if (!find_attack_pos(g, u, ox, whom, 1, &px, &py, 0)) return 1;
	if (get_combat_stance(g, u) == kDefensive && !mand && !expl && defensive && dx >= 0 && dy >= 0) {
		const int32_t L = std::max(obj_max_range(g, u), RON_K(g, "unit_defensive_respond_range"));
		if (vector_dist(u.x - dx, u.y - dy) >= L * 192) {
			find_new_target(g, u, nullptr, false);
			return 1;
		}
	}
	add_move_order(g, u, px, py, 1, false, kQueueFirst, false, -1, -1);
	return 1;
}

// ---------------------------------------------------------------- find_attack_pos

int32_t find_attack_pos(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t p3, int32_t* rx, int32_t* ry, int32_t p8) {
	return find_attack_pos(g, u, ox, whom, p3, rx, ry, u.x, u.y, p8);
}

int32_t find_attack_pos(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t p3, int32_t* rx, int32_t* ry, int32_t fx,
                        int32_t fy, int32_t p8) {
	// @601280
	Object* Tp = obj_at(g, ox, whom);
	if (!Tp) {
		*rx = fx;
		*ry = fy;
		return 0;
	}
	Object& T = *Tp;
	if (u.inside_down >= 0 && u.ptype->domain == kDomainSea) {
		if (Unit* cargo = g.unit(u.inside_down_who, u.inside_down)) return find_attack_pos(g, *cargo, ox, whom, p3, rx, ry, fx, fy, p8);
	}
	bool restore = false, far = false, projected = false;
	int32_t mand = 0, region = -1;
	const Order* actv = get_activity(u);
	const OrderType act_type = actv ? actv->type : kOrderNone;
	const bool siege_bld = type_is_siege(*u.ptype) && unit_ai(u) && !T.is_unit;
	if (u.on_map() && !siege_bld && is_in_range(g, u, ox, whom, fx, fy, 0, nullptr)) {
		*rx = fx;
		*ry = fy;
		return 1;
	}
	const int32_t mr = obj_max_range(g, u);
	const int32_t far_dist = std::max(0x600, (mr + 4) * 192);
	if (!u.orders.empty() && u.order_type() == kAttack) {
		mand = u.front()->mandatory;
		if (u.ptype->domain != kDomainAir && u.ptype->domain != T.ptype->domain) region = tregion_at(g, u.x, u.y);
	}
	if (u.unit_masks & umask::kCanTransport) {
		u.unit_masks &= ~umask::kCanTransport;
		restore = true;
	}
	auto do_restore = [&] {
		if (restore) u.unit_masks |= umask::kCanTransport;
	};
	const uint32_t ang_from = find_angle(fx - T.x, fy - T.y);
	const int32_t d = attack_dist(g, u, ox, whom, u.x, u.y);
	int32_t R;
	bool melee_standing = false;
	if (T.is_unit && d > (mr + 8) * 192) {
		far = true;
		R = (mr + 2) * 192;
	} else if (u.ptype->role & role::kRanged) {
		const int32_t mn = obj_min_range(g, u);
		if (d < mn * 192 - 6 && !siege_bld) {
			project(fx, fy, u.angle, (mn + 1) * 192 - d, &fx, &fy);
			R = mn * 192 + 0x90;
		} else if (d > mr * 192 - 6 || siege_bld) {
			R = mr * 192 - 0x60;
			if (mr < 10) R += u.ptype->big_radius - 0x30;
			if (T.is_unit) R -= T.ptype->big_radius;
			R = std::max(R, 0xc0);
		} else {
			R = d;
		}
	} else {
		R = 0x30;
		melee_standing = T.is_unit && !unit_is_moving(static_cast<Unit&>(T));
	}
	const int32_t filter = p3 ? kFilterCanCollide : kFilterNotMe;

	auto fallback = [&]() -> int32_t {
		// 3.8.6
		*rx = T.x;
		*ry = T.y;
		if (!mand || vector_dist(T.x - u.x, T.y - u.y) < far_dist) {
			do_restore();
			return 0;
		}
		const int32_t xs = T.ptype->x_size;
		if (find_nearby_spot(g, *u.ptype, T.x, T.y, rx, ry, xs * 192, xs * 768, 0, 0x55555555u, kFilterNotMe, u.o, u.who,
		                     0, 0, -1, 0, region) != 0) {
			wcell_offset(u, T, rx, ry);
			restrict_xy(g, rx, ry);
			const int32_t tx = tcoord_of(*rx), ty = tcoord_of(*ry);
			if (u.ptype->domain == kDomainGround && g.world.in_t(tx, ty) &&
			    (g.world.t(tx, ty) & tmask::kMedium) == tmask::kWater)
				return 0;
		}
		do_restore();
		if (u.ptype->domain == kDomainSea) return 0;
		if (u.ptype->domain == kDomainGround && T.ptype->domain == kDomainSea) return 0;
		return 1;
	};

	if (is_wall(T)) {
		if (act_type == kGuard) {
			*rx = fx;
			*ry = fy;
			do_restore();
			return 0;
		}
		// ---- 3.8.4: the perimeter walk (the game_random site @602107)
		const int32_t xs = T.ptype->x_size, ys = T.ptype->y_size, hx = xs * 96, hy = ys * 96, tx = T.x, ty = T.y;
		const int32_t ex = iabs(fx - tx) - hx, ey = iabs(fy - ty) - hy;
		const bool diag = ex > 0 && ey > 0;
		int32_t side0;
		if (ex > ey) {
			if (diag) side0 = fx > tx ? (fy > ty ? 5 : 3) : (fy > ty ? 7 : 1);
			else side0 = fx > tx ? 4 : 8;
		} else {
			if (diag) side0 = fy > ty ? (fx <= tx ? 7 : 5) : (fx > tx ? 3 : 1);
			else side0 = fy > ty ? 6 : 2;
		}
		int32_t step, astep;
		if (raw_max_range(g, *u.ptype) == 0) {
			step = 0x20;
			astep = 0x20000000;
		} else {
			const int32_t br = u.ptype->big_radius;
			if (br > 0x30 && xs > 2 && ys > 2) step = 0xc0;
			else if (br > 0x18 && xs > 1 && ys > 1) step = 0x40;
			else step = 0x20;
			const uint32_t q = uint32_t(int32_t(0x40000000) / std::max(mr, 1));
			astep = int32_t(std::min<uint32_t>(q / uint32_t(0xc0 / step), 0x20000000u));
		}
		const uint32_t nseg = 0x40000000u / uint32_t(astep);
		const int32_t nmax = int32_t(nseg) - 1;
		const int32_t ainc = int32_t(0x40000000) / int32_t(nseg);
		auto corner_base = [](int32_t side) -> uint32_t {
			switch (side) {
			case 1: return 0xc0000000u;
			case 3: return 0u;
			case 5: return 0x40000000u;
			default: return 0x80000000u;  // 7
			}
		};
		auto corner_pt = [&](int32_t side, int32_t* x, int32_t* y) {
			switch (side) {
			case 1: *x = tx - hx, *y = ty - hy; break;
			case 3: *x = tx + hx, *y = ty - hy; break;
			case 5: *x = tx + hx, *y = ty + hy; break;
			default: *x = tx - hx, *y = ty + hy; break;
			}
		};
		int32_t side[2], wx[2], wy[2], k[2] = {0, 0};
		uint32_t base[2] = {0, 0};
		for (int w = 0; w < 2; w++) {
			side[w] = side0;
			if (side0 & 1) {
				base[w] = corner_base(side0);
				corner_pt(side0, &wx[w], &wy[w]);
				k[w] = nmax / 2 + 1;
			} else {
				switch (side0) {
				case 2: wx[w] = tx, wy[w] = ty - hy - R; break;
				case 4: wx[w] = tx + hx + R, wy[w] = ty; break;
				case 6: wx[w] = tx, wy[w] = ty + hy + R; break;
				default: wx[w] = tx - hx - R, wy[w] = ty; break;
				}
			}
		}
		int32_t best = -1, bx = 0, by = 0, limit = 100, cnt = 4, uu = 0;
		do {
			int32_t cx, cy;
			if (side[uu] & 1) project(wx[uu], wy[uu], uint32_t(k[uu] * ainc) + base[uu], R, &cx, &cy);
			else cx = wx[uu], cy = wy[uu];
			cx = uc(cx);
			cy = uc(cy);
			const int32_t ctx = tcoord_of(cx), cty = tcoord_of(cy);
			bool valid = unit_invalid_loc(g, u, ctx, cty, true, false, false, false, true) == 0;
			if (valid) valid = g.world.in_t(ctx, cty) && !(g.world.t(ctx, cty) & tmask::kBlocked);
			if (valid) valid = !find_collision(g, cx, cy, u.o, u.who, 0) && !find_ordered_collision(g, cx, cy, u.o, u.who);
			if (valid) {
				const int32_t rr = g.rng.get(0, 0xffff) % 192;  // @602107
				const int32_t score = vector_dist(cx - fx, cy - fy) + rr;
				if (best < 0 || score < best) {
					best = score;
					bx = cx;
					by = cy;
					limit = std::min(limit, R > 0x300 ? cnt + 11 : cnt);
				}
			}
			uu ^= 1;
			// advance walker uu
			if (side[uu] & 1) {
				k[uu] += uu ? 1 : -1;
				if (k[uu] > nmax || k[uu] < 1) {
					side[uu] += uu ? 1 : -1;
					if (side[uu] == 0) side[uu] = 8;
					if (side[uu] == 9) side[uu] = 1;
					switch (side[uu]) {
					case 2: wx[uu] = tx + (uu ? -hx : hx), wy[uu] = ty - hy - R; break;
					case 4: wx[uu] = tx + hx + R, wy[uu] = ty + (uu ? -hy : hy); break;
					case 6: wx[uu] = tx + (uu ? hx : -hx), wy[uu] = ty + hy + R; break;
					default: wx[uu] = tx - hx - R, wy[uu] = ty + (uu ? hy : -hy); break;
					}
				}
			} else {
				bool over = false;
				switch (side[uu]) {
				case 2: wx[uu] += uu ? step : -step, over = iabs(wx[uu] - tx) > hx; break;
				case 4: wy[uu] += uu ? step : -step, over = iabs(wy[uu] - ty) > hy; break;
				case 6: wx[uu] += uu ? -step : step, over = iabs(wx[uu] - tx) > hx; break;
				default: wy[uu] += uu ? -step : step, over = iabs(wy[uu] - ty) > hy; break;
				}
				if (over) {
					side[uu] += uu ? 1 : -1;
					if (side[uu] == 9) side[uu] = 1;
					if (side[uu] == 0) side[uu] = 8;
					base[uu] = corner_base(side[uu]);
					corner_pt(side[uu], &wx[uu], &wy[uu]);
					k[uu] = uu ? 1 : nmax;
				}
			}
			cnt++;
		} while (cnt - 4 < limit);
		do_restore();
		if (best >= 0) {
			*rx = bx;
			*ry = by;
			return 1;
		}
		return fallback();
	}
	if (melee_standing) {
		// ---- 3.8.3
		if (find_melee_pos(g, u, ox, whom, rx, ry, u.x, u.y)) {
			do_restore();
			return 1;
		}
		*rx = T.x;
		*ry = T.y;
		if (mand && vector_dist(T.x - u.x, T.y - u.y) >= far_dist) {
			wcell_offset(u, T, rx, ry);
			restrict_xy(g, rx, ry);
			return 1;  // (UNIT_CAN_TRANSPORT is not restored on this exit: bug-compatible)
		}
		if (u.order_type() == kAttack) u.front()->mandatory = 0;
		do_restore();
		return 0;
	}
	// ---- 3.8.5: units (lead, hold or ring)
	int32_t cx = T.x, cy = T.y;
	const Unit& tu = static_cast<const Unit&>(T);
	if (!unit_ai(u) && (u.ptype->domain == kDomainAir || u.ptype->domain == T.ptype->domain) && !tu.orders.empty() &&
	    order_is_move(tu.order_type()) && raw_max_range(g, *u.ptype) != 0) {
		const uint32_t dd = tu.angle - find_angle(T.x - u.x, T.y - u.y) + 0x80000000u;
		if (dd < 0x2aaaaaaau || dd > 0xd5555555u) {
			if ((obj_max_range(g, T) < mr || (u.unit_masks2 & umask2::kMarineEntrench)) &&
			    vector_dist(T.x - u.x, T.y - u.y) < (mr + 4) * 192) {
				*rx = u.x;
				*ry = u.y;
				do_restore();
				return 1;
			}
		} else if (flanking(dd) == 1) {
			const int32_t s1 = get_speed(g, u, u.x, u.y, false), s2 = get_speed(g, tu, T.x, T.y, false);
			if (s1 != 0) {
				const int32_t lead = ((vector_dist(T.x - u.x, T.y - u.y) / s1) * s2 * 3) / 2;
				int32_t px = 0, py = 0;
				project(T.x, T.y, tu.angle, lead, &px, &py);
				if (world_is_valid(g, px, py)) {
					cx = px;
					cy = py;
					projected = true;
				}
			}
		}
	}
	int32_t rin = T.ptype->big_radius + u.ptype->big_radius + R, rmax = 0, rstep = 0;
	if (rin > 0x240) {
		rmax = rin;
		rstep = 0x60;
		rin -= 0xc0;
	}
	int32_t sx = 0, sy = 0;
	if (find_nearby_spot(g, *u.ptype, cx, cy, &sx, &sy, rin, rmax, rstep, ang_from, filter, u.o, u.who, p8, 0, -1, 0,
	                     region) != 0)
		return fallback();
	do_restore();
	if (projected || far || is_in_range(g, u, ox, whom, sx, sy, 0, nullptr)) {
		*rx = sx;
		*ry = sy;
		return 1;
	}
	return fallback();
}

void unit_change_target(Game& g, Unit& u0, int32_t oox, int32_t owhom, int32_t nox, int32_t nwhom) {
	// @5e36c0
	Unit* u = &u0;
	const Object* nt = obj_at(g, nox, nwhom);
	for (int guard = 0; u && guard < kUnitLimit; guard++) {
		Order* a = update_action(g, *u);
		if (a && order_is_targeted(a->type) && a->ox == oox && a->whom == owhom) {
			a->ox = nox;
			a->whom = nwhom;
			a->uid = nt ? nt->uid : 0xffff;
			while (!u->orders.empty() && !order_is_targeted(u->order_type())) kill_current_order(g, *u);
		}
		if (u->o_down < 0) break;
		Unit* d = g.unit(u->who, u->o_down);
		if (!d || !d->alive()) break;
		u = d;
	}
}

// ---------------------------------------------------------------- attack-move

void do_attack_to_pause(Game& g, Unit& u, Order& m) {
	// @5f22a0
	if (u.group < 0) return;
	Group& gr = g.groups.g[size_t(u.group)];
	if (group_member(g, gr, u.o, u.who) && group_is_attacking_near(g, gr, u.x, u.y)) m.pause = 15;
}

void do_attack_to_scan(Game& g, Unit& u, Order& m) {
	// The tail of Unit::do_attack_to @5f2320 (do_move ran first; the caller checks that m is still the front order).
	if ((u.o + g.frame) % 15 != 0) return;
	if (u.ptype->attack != 0 && !(u.ptype->unit_flags2 & utflag2::kSupply)) {
		// TODO(AI armies): an AI army keeping the unit in formation returns here.
		find_melee_target(g, u, -1, nullptr, false, true, 0);
	} else {
		do_attack_to_pause(g, u, m);
	}
}

void do_group_attack_to_scan(Game& g, Unit& u, Order& m) {
	// The tail of Unit::do_group_attack_to @5e74e0.
	if ((u.o + g.frame) % 15 != 0) return;
	if (u.ptype->attack != 0 && !(u.ptype->unit_flags2 & utflag2::kSupply)) find_melee_target(g, u, -1, nullptr, false, true, 0);
	else do_attack_to_pause(g, u, m);
}

int32_t do_group_attack(Game& g, Unit& u, Order& a) {
	// @5e75a0. GROUP_ATTACK orders are created by the AI (Army) and scenario triggers only; the conversion and the
	// leader / follower logic follow the spec, the movement step of a temporary attack is TODO.
	if (u.recharging != 0) return 0;
	unit_set_angle(g, u, a.group_angle, false);
	if (u.group == -1) {
		Order c = a;
		c.type = kAttack;
		u.orders.pop_front();
		add_order(g, u, c, kQueueFirst);
		return 0;
	}
	const int32_t ox = a.ox, whom = a.whom, mand = a.mandatory, id = a.id, temp = a.in_group;
	Group& gr = g.groups.g[size_t(u.group)];
	if (a.oxx == u.o && a.whose == u.who) {
		if (!alive_at(g, ox, whom)) {
			group_kill_group_order(g, gr, id);
			return 0;
		}
		if (is_in_range(g, u, ox, whom, 0)) {
			fight(g, u, ox, whom, mand, temp, 0);
			return 0;
		}
		if (temp == 0) {
			group_kill_group_move(g, gr, id);
			group_distribute_attack(g, gr, ox, whom);
		} else {
			group_kill_group_order(g, gr, id);
		}
		return 0;
	}
	const Unit* L = g.unit(a.whose, a.oxx);
	if (!L || !L->alive() || !L->on_map() || L->group != u.group) {
		group_refresh_group_order(g, gr, u);
		return 0;
	}
	int32_t t = -1, w = -1;
	if (mand >= 2 && is_in_range(g, u, ox, whom, 0)) {
		t = ox;
		w = whom;
	} else {
		t = find_nearby_target(g, u, (obj_max_range(g, u) + 4) * 192, &w, false, false, 0);
	}
	if (t >= 0 && valid_target(g, u, t, w, nullptr) && is_in_range(g, u, t, w, 0)) {
		fight(g, u, t, w, 1, 1, 0);
		return 0;
	}
	if (u.orders.size() >= 2 && u.orders[1].type == kGroupMove && temp != 0 && !u.path.empty()) {
		// TODO(combat-spec 3.3.3 step 6): unlink the attack, move_step the group move at 9/8 speed, re-add.
		return 0;
	}
	if (u.angle != a.group_angle) unit_set_angle(g, u, a.group_angle, false);
	else unit_set_anim(g, u, kCharDefault, 0, 1);
	return 0;
}

// ---------------------------------------------------------------- groups

bool group_member(const Game& g, const Group& gr, int32_t o, int32_t who) {
	// GroupData::member @70f8f0 (p3 = 1: the object must be VALID)
	if (who != gr.who) return false;
	const Object* ob = g.object(who, o);
	if (!ob || !ob->alive()) return false;
	for (int32_t i = 0; i < gr.num; i++)
		if (gr.list[size_t(i)] == o) return true;
	return false;
}

bool group_is_attacking_to(Game& g, Group& gr) {
	// Group::is_attacking_to @70ea70: every valid on-map member with orders has an ATTACK_TO / GROUP_ATTACK_TO front.
	group_normalize(g, gr);
	for (int32_t i = 0; i < gr.num; i++) {
		const Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!m || !m->alive() || !m->on_map() || m->orders.empty()) continue;
		const OrderType t = m->order_type();
		if (t != kAttackTo && t != kGroupAttackTo) return false;
	}
	return true;
}

bool group_is_attacking_near(Game& g, Group& gr, int32_t x, int32_t y) {
	// @710e40
	group_normalize(g, gr);
	int32_t near = 0, busy = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		const Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!m || !m->alive() || !m->on_map() || m->o_up >= 0 || m->ptype->attack == 0) continue;
		if ((m->ptype->unit_flags2 & utflag2::kSupply) || (m->ptype->unit_flags2 & utflag2::kHero)) continue;
		if (vector_dist(x - m->x, y - m->y) > 0x600) continue;
		near++;
		const OrderType t = m->order_type();
		if (!order_is_move(t) && t != kOrderNone && t != kGuard) busy++;
	}
	return near != 0 && near / 2 <= busy;
}

void group_distribute_attack(Game& g, Group& gr, int32_t ox, int32_t whom) {
	// @713390
	group_normalize(g, gr);
	int32_t n_in = 0, first = -1, mand = 0;
	bool expl = false;
	auto eligible = [&](Unit* m) {
		return m && m->alive() && m->on_map() && !unit_is_plane(*m) && !m->orders.empty() &&
		       order_is_attack(m->order_type());
	};
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!eligible(m)) continue;
		Order& a = *m->front();
		if (first < 0) {
			first = i;
			mand = a.mandatory;
			expl = (a.flags & orderflag::kExplicit) != 0;
		}
		if (is_in_range(g, *m, ox, whom, 0)) {
			n_in++;
		} else {
			a.ox = -1;
			a.whom = -1;
		}
	}
	if (n_in < 2 && first >= 0) {
		for (int32_t i = 0; i < gr.num; i++) {
			Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
			if (!eligible(m)) continue;
			kill_current_order(g, *m);
			add_attack_order(g, *m, ox, whom, kQueueFirst, mand, expl);
		}
	}
}

void group_action_attack(Game& g, Group& gr, int32_t ox, int32_t whom, int32_t mandatory, QueuePos q, int32_t ignore) {
	// @712490
	if (!grp_on_map(g, gr) || ox < 0) return;
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	gr.disband = 0;  // action_begin
	if (gr.num <= 0) return;
	Object* Tp = obj_at(g, ox, whom);
	if (!Tp) return;
	if (gr.buildings) {
		for (int32_t i = 0; i < gr.num; i++) {
			Build* b = g.build(gr.who, gr.list[size_t(i)]);
			if (!b) continue;
			b->build_masks |= 4;
			b->attack_ox = int16_t(ox);
			b->attack_whom = int8_t(whom);
		}
		return;
	}
	if (q == kQueueFirst) {
		std::vector<Order> saved = group_set_up_insert(g, gr);
		group_action_halt(g, gr, ignore);
		group_action_attack(g, gr, ox, whom, mandatory, kQueueNew, ignore);
		group_finish_insert(g, gr, saved);
		return;
	}
	const int32_t li = group_find_leader(g, gr);
	if (li < 0) return;
	Unit& L = *g.unit(gr.who, gr.list[size_t(li)]);
	int32_t px = L.x, py = L.y;
	if (q == kQueueLast) group_get_loc_to(g, gr, &px, &py);
	if (!is_in_range(g, L, ox, whom, px, py, 0, nullptr)) {
		int32_t rx = 0, ry = 0;
		find_attack_pos(g, L, ox, whom, 1, &rx, &ry, px, py, 1);  // result unused (draws, may clear mandatory)
	}
	Object& T = *Tp;
	if (!T.is_unit && is_build_active(T) && check_capture_eligible(g, T)) {
		check_capture(g, T, L.o, gr.who);
		group_action_move_to(g, gr, T.x, T.y, q, false, 0, kMoveTo, 1, -1, -1, 0);
		return;
	}
	const bool unseen = !obj_is_seen(g, T, gr.who, 0);
	for (int32_t dom = 0; dom < 3; dom++) {
		for (int32_t i = 0; i < gr.num; i++) {
			Unit* M = g.unit(gr.who, gr.list[size_t(i)]);
			if (!M || !M->alive() || !M->on_map()) continue;
			if ((ignore & 4) && type_is_siege(*M->ptype)) continue;
			if ((ignore & 2) && (M->ptype->unit_flags2 & utflag2::kSpecial)) continue;
			if ((ignore & 1) && type_is(*M->ptype, 58)) continue;
			if (unit_is_plane(*M)) {
				// A plane with fuel left and a current STRAFE retargets it in place (Group::action_attack @712490).
				if (unit_mana_left(g, *M) != 0 && M->order_type() == kStrafe) {
					Order& S = M->orders.front();
					S.whom = whom;
					S.ox = ox;
					S.uid = T.uid;
					S.returning = 0;
				}
				continue;
			}
			if (M->ptype->domain != dom) continue;
			const Order* aa = get_action(*M);
			if (aa && aa->type == kAttack && aa->ox == ox && aa->whom == whom && aa->mandatory && M->orders.size() <= 2 &&
			    !is_in_range(g, *M, ox, whom, 0)) {
				if (M->orders.size() == 1) continue;
				const Order* f = M->front();
				if (f && order_is_move(f->type) && is_in_range(g, *M, ox, whom, f->x, f->y, 0, nullptr)) continue;
			}
			const bool pk = false;  // TODO(packers): is_packing_or_unpacking
			if (!pk || q != kQueueNew) {
				if (T.is_unit || is_wall(T)) {
					if (T.is_unit) {
						if (unseen) add_move_order(g, *M, T.x, T.y, 1, false, q, false, -1, -1);
						int32_t tox = ox, tw = whom;
						if (mandatory == 0) {
							const int32_t r = std::min(vector_dist(T.x - M->x, T.y - M->y) + 192,
							                           RON_K(g, "unit_respond_range") * 576);
							int32_t w2 = whom;
							const int32_t o2 = find_melee_target(g, *M, r, &w2, false, false, 1);
							if (o2 >= 0 && w2 >= 0) {
								tox = o2;
								tw = w2;
							}
						}
						add_attack_order(g, *M, tox, tw, q, mandatory, true);
					} else if (!unseen) {
						add_attack_order(g, *M, ox, whom, q, mandatory, true);
					} else {
						if (is_in_range(g, *M, ox, whom, 0)) check_capture(g, T, M->o, gr.who);
						add_move_order(g, *M, T.x, T.y, 1, false, q, false, -1, -1);
					}
				}
			}
		}
	}
	gr.order_num++;
}

}  // namespace ron::sim
