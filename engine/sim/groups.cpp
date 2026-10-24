// Groups and group moves (docs/orders-spec.md sections 1-2, docs/movement-spec.md section 11).
//
// Status (see the TODO markers): the Group / Groups slot logic (clear, add, kill, get_num, get_num_cap, normalize,
// find_leader, find_role, compute_speed, sort, push_group, get_open_slot, copy_group, process), FormData::type_cat,
// update_positions, HALT, the unit side of group moves (add_group_move_order, ungroup_move_order,
// Unit::kill_group_move with the original's iterator behaviour) are ports. Formation layout (compute_form), the full
// action_move_near (split, insert pattern, group path), STANCE, FORM and do_group_move are reduced stubs for now.
#include "sim/groups.hpp"

#include "sim/combat.hpp"

#include <algorithm>

#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/search.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

// ObjectData::has_general @646b00: a hero (general) in range giving `mask`. TODO(heroes): no heroes yet (-1), so the
// generals' bonuses (stats.cpp, attrition.cpp, groups) never fire. Declared in groups.hpp.
int32_t has_general(const Game& g, const Unit& u, int32_t mask, int32_t p) {
	(void)g, (void)u, (void)mask, (void)p;
	return -1;
}

namespace {


Object* obj_at(Game& g, int32_t who, int32_t o) { return g.object(who, o); }
// vtable +8 is_valid_unit: units with flags & 1; buildings 0.
bool is_valid_unit(const Object* ob) { return ob && ob->is_unit && ob->alive(); }
// vtable +0xe8 is_captain: units o_up < 0; buildings return 1 (folded body).
bool is_captain(const Object* ob) { return !ob->is_unit || static_cast<const Unit*>(ob)->o_up < 0; }
// UnitData::is_plane @46ce40: domain 2 and not a helicopter.
bool is_plane(const Unit& u) { return u.ptype && u.ptype->domain == 2 && (u.ptype->unit_flags & 0x20) == 0; }
bool on_map(const Object* ob) { return ob->is_unit ? static_cast<const Unit*>(ob)->on_map() : true; }

// UnitData::get_captain @610ab0.
int32_t get_captain(Game& g, const Unit& u) {
	const Unit* p = &u;
	for (int guard = 0; p->o_up >= 0 && guard < kUnitLimit; guard++) {
		const Unit* q = g.unit(p->who, p->o_up);
		if (!q) break;
		p = q;
	}
	return p->o;
}

// Removes entry i, shifting the six per-member arrays (the stale last entry is kept, as in the original).
void remove_at(Group& gr, int32_t i) {
	for (int32_t j = i; j < gr.num - 1; j++) {
		const size_t a = size_t(j), b = size_t(j + 1);
		gr.list[a] = gr.list[b];
		gr.angles[a] = gr.angles[b];
		gr.off_x[a] = gr.off_x[b];
		gr.off_y[a] = gr.off_y[b];
		gr.curr_x[a] = gr.curr_x[b];
		gr.curr_y[a] = gr.curr_y[b];
	}
	gr.num--;
}

// GroupData::get_num_const @70d000.
int32_t get_num_const(Game& g, const Group& gr) {
	int32_t n = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (ob && ob->alive()) n++;
	}
	return n;
}

// GroupData::member @70f8f0 (with the validity test).
bool member(Game& g, const Group& gr, int32_t o, int32_t who) {
	if (who != gr.who) return false;
	const Object* ob = obj_at(g, gr.who, o);
	if (!ob || !ob->alive()) return false;
	for (int32_t i = 0; i < gr.num; i++)
		if (gr.list[size_t(i)] == o) return true;
	return false;
}

// GroupData::find_leader @70ccb0: returns the leader's o, *idx its list index.
int32_t find_leader_o(Game& g, const Group& gr, int32_t* idx) {
	for (int pass = 0; pass < 2; pass++) {
		int32_t best_cat = kNumFormCat, best_o = -1, best_i = 0;
		for (int32_t i = 0; i < gr.num; i++) {
			const int32_t o = gr.list[size_t(i)];
			const Object* ob = obj_at(g, gr.who, o);
			if (!is_valid_unit(ob) || !is_captain(ob)) continue;
			if (pass == 0 && !on_map(ob)) continue;
			const int32_t c = form_type_cat(g, *static_cast<const Unit*>(ob));
			if (best_o < 0 || c < best_cat) {
				best_cat = c;
				best_i = i;
				best_o = o;
			}
		}
		if (best_o >= 0) {
			if (idx) *idx = best_i;
			return best_o;
		}
	}
	return -1;
}

// Group::equals_group @708000.
bool equals_group(Game& g, Group& a, Group& b) {
	if (a.who != b.who) return false;
	if (a.id != -1) group_normalize(g, a);
	if (b.id != -1) group_normalize(g, b);
	const int32_t na = group_get_num(g, a), nb = group_get_num(g, b);
	if (na != nb || a.buildings != b.buildings) return false;
	for (int32_t i = 0; i < a.num; i++)
		if (a.list[size_t(i)] != b.list[size_t(i)]) return false;
	return true;
}

// GroupData::is_on_map @70c450.
bool group_is_on_map(Game& g, const Group& gr) {
	if (gr.num == 0) return false;
	if (gr.buildings) return true;
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (is_valid_unit(ob) && is_captain(ob) && on_map(ob)) return true;
	}
	return false;
}

// Valid, on the map and not a plane: the member filter of every action.
Unit* action_member(Game& g, const Group& gr, int32_t i) {
	Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
	if (!is_valid_unit(ob) || !on_map(ob)) return nullptr;
	Unit* u = static_cast<Unit*>(ob);
	return is_plane(*u) ? nullptr : u;
}

// GroupData::get_form @70b9f0.
int32_t get_form(Game& g, const Group& gr) {
	if (gr.buildings) return -1;
	int32_t cur = -1;
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (!is_valid_unit(ob) || !on_map(ob)) continue;
		const int32_t f = static_cast<const Unit*>(ob)->form;
		if (f != cur) {
			if (cur >= 0) return -1;
			cur = f;
		}
	}
	return cur;
}

// GroupData::get_form_mod_option @70bd00.
int32_t get_form_mod_option(Game& g, const Group& gr) {
	if (gr.buildings) return 50;
	const int32_t n = get_num_const(g, gr);
	if (n == 0) return 50;
	if (n == 1) {
		if (gr.num > 0) {
			const int32_t lo = find_leader_o(g, gr, nullptr);
			const Unit* u = lo >= 0 ? g.alive_unit(gr.who, lo) : nullptr;
			if (u && u->form_mod != -1) return u->form_mod;
		}
		return 50;
	}
	int32_t cnt = 0, sum = 0;
	for (int32_t i = 0; i < n; i++) {
		const Unit* u = action_member(g, gr, i);
		if (u && u->form_mod != -1) {
			cnt++;
			sum += u->form_mod;
		}
	}
	return cnt > 0 ? sum / cnt : 50;
}

// ---- systems the group code calls that are not simulated yet

// Objects::add_flock @65c0e0 (a flock of animals near a forest wcoord). TODO(gaia, movement-spec 15.8): it is rate
// limited per leader (frame >= leader+0x7b8 + 225), then draws game_random (2 + per animal) and creates gaia units;
// not ported, so a group move that ends on a forest edge consumes only do_group_move's own draw.
void add_flock(Game& g, int32_t tx, int32_t ty, int32_t who, int32_t n) {
	(void)g, (void)tx, (void)ty, (void)who, (void)n;
}
// LeaderData::current_upgrade @6e3140. TODO(techs): leader techs are not simulated; the type itself.
int32_t current_upgrade_type(const Game& g, int32_t who, int32_t t) {
	(void)g, (void)who;
	return t;
}

}  // namespace


// ---------------------------------------------------------------- Groups

Groups::Groups() {
	// Groups::clear @713f20
	for (int32_t i = 0; i < kNumGroups; i++) {
		group_clear(g[size_t(i)], i);
		g[size_t(i)].stamp = 0;
		g[size_t(i)].priority = 0;
	}
	for (int32_t p = 0; p < 8; p++) last_group[size_t(p)] = p * kGroupsPerLeader;
	proc_group = 0;
}

// Group::clear @713e80. The original stamps game.frame; slots pass it through the Game overload below.
void group_clear(Group& gr, int32_t id) {
	if (id >= 0) gr.id = id;
	gr.who = 0;
	gr.army = -1;
	gr.num = 0;
	gr.form = -1;
	gr.stamp = 0;
	gr.ox = gr.oy = gr.o_dist = 0;
	gr.o_angle = 0;
	gr.facing = gr.buildings = 0;
	gr.disband = gr.order_num = gr.priority = gr.role = 0;
	gr.new_speed = gr.speed = gr.form_num = gr.think_frame = 0;
	gr.march = 0;
}
static void group_clear(Game& g, Group& gr, int32_t id) {
	group_clear(gr, id);
	gr.stamp = g.frame;
}

int32_t group_get_num(Game& g, Group& gr) {
	// Group::get_num @714700
	if (gr.num < 1) {
		gr.num = 0;
		gr.oy = gr.ox = 0;
		return 0;
	}
	if (gr.num < 4 && gr.id >= 0) {
		group_normalize(g, gr);
		return gr.num;
	}
	for (int32_t i = gr.num - 1; i >= 0; i--) {
		const Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (!ob || !ob->alive()) remove_at(gr, i);
	}
	return gr.num;
}

int32_t group_get_num_cap(Game& g, Group& gr) {
	// Group::get_num_cap @7145c0
	if (gr.num <= 0) {
		gr.num = 0;
		return 0;
	}
	if (gr.num < 4 && gr.id >= 0) group_normalize(g, gr);
	int32_t cnt = 0;
	for (int32_t i = gr.num - 1; i >= 0; i--) {
		const Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (!ob || !ob->alive()) remove_at(gr, i);
		else if (is_captain(ob)) cnt++;
	}
	return cnt;
}

void group_find_role(Game& g, Group& gr) {
	gr.role = 0;
	if (gr.buildings) return;
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (is_valid_unit(ob)) gr.role |= ob->ptype->role;
	}
}

void group_compute_speed(Game& g, Group& gr) {
	if (!gr.buildings && gr.num > 0) {
		const int32_t lo = find_leader_o(g, gr, nullptr);
		if (lo >= 0) {
			gr.speed = gr.new_speed = unit_speed(g, *g.unit(gr.who, lo));
			return;
		}
	}
	gr.speed = gr.new_speed = 0;
}

void group_normalize(Game& g, Group& gr) {
	// Group::normalize @711540: back to front, drop invalid, dead and departed members.
	for (int32_t i = gr.num - 1; i >= 0; i--) {
		const int32_t o = gr.list[size_t(i)];
		bool drop = o < 0;
		if (!drop) {
			const Object* ob = obj_at(g, gr.who, o);
			if (!ob || !ob->alive()) drop = true;
			else if (gr.priority == 0 && gr.id >= 0 && ob->is_unit && static_cast<const Unit*>(ob)->group != gr.id)
				drop = true;
		}
		if (drop) remove_at(gr, i);
	}
	group_find_role(g, gr);
	group_compute_speed(g, gr);
}

int32_t group_find_leader(const Game& g, const Group& gr) {
	int32_t idx = -1;
	if (find_leader_o(const_cast<Game&>(g), gr, &idx) < 0) return -1;
	return idx;
}

bool group_add(Game& g, Group& gr, int32_t o, int32_t who, int32_t down, int32_t konst) {
	// Group::add @714350
	const int32_t cnt = konst ? get_num_const(g, gr) : group_get_num(g, gr);
	if (cnt == 0) gr.who = int8_t(who);
	if (!(gr.num == 0 || who == gr.who)) return false;
	Object* ob = obj_at(g, gr.who, o);
	if (!ob) return false;
	if (ob->is_unit) {
		Unit* u = static_cast<Unit*>(ob);
		if (down == 0) {
			if (!is_captain(u)) return group_add(g, gr, u->o_up, gr.who, 0, konst);
		} else if (is_captain(u)) {
			group_kill(g, gr, o, gr.who, 0, 0);
		}
	}
	gr.disband = 0;
	const int8_t isb = ob->is_unit ? 0 : 1;
	if (!(gr.num == 0 || gr.buildings == isb)) return false;
	if (member(g, gr, o, who) || gr.num >= kGroupMax) return false;
	if (gr.num == 0) gr.who = int8_t(who);
	const size_t n = size_t(gr.num);
	gr.off_x[n] = gr.off_y[n] = gr.curr_x[n] = gr.curr_y[n] = 0;
	gr.angles[n] = 0;
	gr.list[n] = int16_t(o);
	gr.num++;
	gr.buildings = isb;
	gr.stamp = g.frame;
	ob = obj_at(g, gr.who, o);
	if (is_valid_unit(ob)) {
		const Unit* u = static_cast<const Unit*>(ob);
		gr.role |= u->ptype->role;
		const int32_t d = u->o_down;
		const Object* dob = d >= 0 ? obj_at(g, gr.who, d) : nullptr;
		if (dob && dob->alive()) group_add(g, gr, d, gr.who, 1, konst);
	}
	if (gr.id >= 0) group_compute_speed(g, gr);
	return true;
}

void group_kill(Game& g, Group& gr, int32_t o, int32_t who, int32_t down, int32_t konst) {
	// Group::kill @714110
	Object* ob = obj_at(g, who, o);
	if (ob && ob->is_unit) {
		Unit* u = static_cast<Unit*>(ob);
		if (down == 0 && !is_captain(u)) {
			group_kill(g, gr, u->o_up, who, 0, konst);
			return;
		}
		const int32_t d = u->o_down;
		if (d >= 0) {
			const Object* dob = obj_at(g, who, d);
			if (konst != 0 || (dob && dob->alive())) group_kill(g, gr, d, who, 1, konst);
		}
	}
	if (who != gr.who) return;
	int32_t i = 0;
	while (i < gr.num && gr.list[size_t(i)] != o) i++;
	if (i >= gr.num) return;
	if (ob && ob->is_unit && static_cast<Unit*>(ob)->group == gr.id) static_cast<Unit*>(ob)->group = -1;
	gr.disband = 0;
	remove_at(gr, i);
	if (gr.num == 0) group_clear(g, gr, -1);
	else gr.stamp = g.frame;
	if (!gr.buildings && gr.num > 0) {
		const int32_t lo = find_leader_o(g, gr, nullptr);
		if (lo >= 0) {
			gr.speed = gr.new_speed = unit_speed(g, *g.unit(gr.who, lo));
			return;
		}
	}
	gr.speed = gr.new_speed = 0;
}

void group_sort(Game& g, Group& gr) {
	// Group::sort @708090: move a follower that is not behind its captain (with the whole chain) to the end.
	for (int guard = 0; guard < 4 * kGroupMax; guard++) {
		if (gr.buildings) return;
		const int32_t who = gr.who;
		int32_t last_cap = -1, i = 0, bad = -1;
		for (; i < gr.num; i++) {
			const int32_t o = gr.list[size_t(i)];
			const Unit* u = g.unit(who, o);
			if (!u) continue;
			if (u->o_up < 0) {
				last_cap = o;
				continue;
			}
			if (last_cap < 0 || get_captain(g, *u) != last_cap) {
				bad = o;
				break;
			}
		}
		if (bad < 0) return;
		group_kill(g, gr, bad, gr.who, 0, 0);
		group_normalize(g, gr);
		group_add(g, gr, bad, who, 0, 1);
	}
}

void group_update_positions(Game& g, Group& gr, const Unit& leader) {
	// Group::update_positions @713810
	uint32_t b = leader.angle;
	if (const Order* m = leader.front(); m && order_is_move(m->type) && m->dest != 0)
		b = find_angle(m->dest_x - leader.x, m->dest_y - leader.y);
	(void)g;
	for (int32_t i = 0; i < gr.form_num && i < kGroupMax; i++) {
		const int32_t ox = int32_t(uint32_t(gr.off_x[size_t(i)]) * 48u);
		const int32_t oy = int32_t(uint32_t(gr.off_y[size_t(i)]) * 48u);
		gr.curr_x[size_t(i)] = sin_a(b, oy) + cos_a(b, ox);
		gr.curr_y[size_t(i)] = sin_a(b, ox) - cos_a(b, oy);
	}
}

void group_kill_group_move(Game& g, Group& gr, int32_t id) {
	// Group::kill_group_move @7123f0
	group_normalize(g, gr);
	for (int32_t i = 0; i < gr.num; i++) {
		Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (is_valid_unit(ob) && on_map(ob)) unit_kill_group_move(g, *static_cast<Unit*>(ob), id);
	}
}

void group_refresh_group_order(Game& g, Group& gr, Unit& u) {
	// Group::refresh_group_order @713a50 (u becomes the leader of its group order).
	group_normalize(g, gr);
	const Order* f = u.front();
	if (!f || !order_is_group(f->type)) return;
	const int32_t id = f->id, slot = f->form_id;
	const int32_t dx = gr.off_x[size_t(slot & 127)], dy = gr.off_y[size_t(slot & 127)];
	for (int32_t i = 0; i < gr.form_num && i < kGroupMax; i++) {
		gr.off_x[size_t(i)] -= dx;
		gr.off_y[size_t(i)] -= dy;
	}
	for (int32_t i = 0; i < gr.num; i++) {
		Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (!is_valid_unit(ob) || !on_map(ob)) continue;
		for (Order& o : static_cast<Unit*>(ob)->orders)  // Unit::modify_group_order @5e3530
			if (order_is_group(o.type) && o.id == id) {
				o.oxx = u.o;
				o.whose = u.who;
			}
	}
	group_update_positions(g, gr, u);
}

// ---------------------------------------------------------------- Groups slots

void groups_copy_group(Game& g, Group& dst, const Group& src) {
	// Groups::copy_group @6fa690: only these fields; the others keep the previous occupant's values.
	dst.who = src.who;
	dst.num = src.num;
	dst.ox = src.ox;
	dst.oy = src.oy;
	dst.o_dist = src.o_dist;
	dst.o_angle = src.o_angle;
	dst.buildings = src.buildings;
	dst.speed = src.speed;
	dst.stamp = g.frame;
	for (int32_t i = 0; i < src.num; i++) {
		const size_t k = size_t(i);
		dst.list[k] = src.list[k];
		dst.angles[k] = src.angles[k];
		dst.off_x[k] = src.off_x[k];
		dst.off_y[k] = src.off_y[k];
		dst.curr_x[k] = src.curr_x[k];
		dst.curr_y[k] = src.curr_y[k];
	}
}

int32_t groups_get_open_slot(Game& g, int32_t who) {
	// Groups::get_open_slot @6fa460
	Groups& G = g.groups;
	const int32_t base = who * kGroupsPerLeader, last = G.last_group[size_t(who)];
	int32_t best = g.frame, cand = -1, result = -1;
	for (int32_t s = base; s < base + kGroupOpenSlots; s++) {
		Group& gr = G.g[size_t(s)];
		const int32_t n = group_get_num(g, gr);
		if ((n == 0 || gr.buildings) && s != last) {
			result = s;
			break;
		}
		if (gr.stamp <= best && group_get_num_cap(g, gr) == 1 && s != last) {
			best = gr.stamp;
			cand = s;
		}
	}
	if (result < 0) result = cand;
	if (result < 0) {  // "UH OH, NEED MORE GROUPS!"
		for (int32_t s = base; s < base + kGroupOpenSlots; s++)
			if (G.g[size_t(s)].stamp <= g.frame && s != last) result = s;
	}
	if (result < 0) result = base;
	if (!G.g[size_t(result)].buildings) {
		const int32_t mark = g.objects[size_t(who)].unit_mark;
		for (int32_t o = 0; o < mark; o++)
			if (Unit* u = g.unit(who, o); u && u->group == result) u->group = -1;
	}
	return result;
}

int32_t groups_push_group(Game& g, int32_t who, Group& tmp, int32_t force) {
	// Groups::push_group @70f9e0
	Groups& G = g.groups;
	if (force == 0 && tmp.num < 2) {
		for (int32_t i = 0; i < tmp.num; i++) {
			Object* ob = obj_at(g, who, tmp.list[size_t(i)]);
			if (is_valid_unit(ob)) static_cast<Unit*>(ob)->group = -1;
		}
		return -1;
	}
	int32_t slot = G.last_group[size_t(who)];
	if (!equals_group(g, tmp, G.g[size_t(slot)])) {
		slot = groups_get_open_slot(g, who);
		groups_copy_group(g, G.g[size_t(slot)], tmp);
		G.last_group[size_t(who)] = slot;
	}
	for (int32_t i = 0; i < tmp.num; i++) {
		Object* ob = obj_at(g, who, tmp.list[size_t(i)]);
		if (!is_valid_unit(ob)) continue;
		Unit* u = static_cast<Unit*>(ob);
		if (u->group >= 0 && u->group != slot) group_kill(g, G.g[size_t(u->group)], u->o, u->who, 0, 0);
		u->group = int16_t(slot);
	}
	return slot;
}

void groups_process(Game& g) {
	// Groups::process @6fa210: one slot per valid leader 0..7 per frame.
	Groups& G = g.groups;
	for (int32_t p = 0; p < 8; p++)
		if (g.leaders[size_t(p)].flags & leaderflag::kValid)
			group_normalize(g, G.g[size_t(p * kGroupsPerLeader + G.proc_group)]);
	G.proc_group++;
	if (G.proc_group > 63) G.proc_group = 0;
}

// ---------------------------------------------------------------- actions

void group_action_halt(Game& g, Group& gr, int32_t p) {
	// Group::action_halt @70d0c0. TODO: ScenarioData::ignore_orders prelude.
	gr.disband = 0;
	if (gr.buildings) return;
	gr.form = -1;
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* u = action_member(g, gr, i);
		if (!u) continue;
		// UnitData::is_entering_or_exiting: front SPECIAL_ANIM of type 0/1 (TODO: the anim type is not modelled).
		if (u->order_type() == kSpecialAnim) continue;
		if ((p & 4) && (u->ptype->unit_flags & 0x20000)) continue;  // is_siege (assumed for type vfunc +0x10c)
		if ((p & 2) && (u->ptype->unit_flags2 & 0x10)) continue;    // is_special
		if ((p & 1) && unit_is(g, *u, 0x3a)) continue;
		clear_orders(g, *u);
		u->unit_masks &= ~umask::kAutoExplore;
	}
}

// ---- stance (Group::action_stance @70d440)

namespace {

// UnitTypeData::get_stance_type @61d350 (StanceTypes).
int32_t unit_type_stance_type(const Type& t) {
	if (t.role & 0x10000) return (t.unit_flags2 & 4) ? kStancePacker : kStanceCombat;
	if (t.type == 0x32 || t.type == 0x33 || t.type == 0x34 || t.type == 0x35) return kStanceWorker;
	return (t.unit_flags2 & 6) == 2 ? kStanceCaster : kStanceNone;
}
// ObjectData vtable +0x108 get_stance_type. Buildings: BuildTypeData::get_stance_type @6396c0 needs
// is_training_building / is_city / is_fort (TODO(buildings)); NONE for now.
int32_t object_stance_type(const Object* ob) {
	if (!ob || !ob->ptype) return kStanceNone;
	return ob->is_unit ? unit_type_stance_type(*ob->ptype) : kStanceNone;
}

// GroupData::get_stance_type @70d370.
int32_t group_stance_type(Game& g, const Group& gr) {
	if (gr.num <= 0) return kStanceNone;
	int32_t lo = gr.buildings ? gr.list[0] : find_leader_o(g, gr, nullptr);
	if (lo < 0) return kStanceNone;
	int32_t st = object_stance_type(obj_at(g, gr.who, lo));
	if (st == kStanceNone || st == kStanceCaster) {
		bool caster = false;
		for (int32_t i = 0; i < gr.num; i++) {
			st = object_stance_type(obj_at(g, gr.who, gr.list[size_t(i)]));  // no validity test in the original
			if (st != kStanceNone) {
				if (st != kStanceCaster) return st;
				caster = true;
			}
		}
		if (caster) st = kStanceCaster;
	}
	return st;
}

int32_t stance_options(int32_t st) {
	switch (st) {
	case kStanceCombat: return 6;
	case kStanceWorker: return 4;
	case kStanceCaster:
	case kStancePacker: return 2;
	default: return 0;
	}
}

// GroupData::get_stance_option @70bab0: the most common option among the members of the group's stance type.
int32_t group_stance_option(Game& g, const Group& gr) {
	if (gr.num == 0) return 0;
	const int32_t st = group_stance_type(g, gr), n = stance_options(st);
	if (n == 0) return 0;
	std::array<int32_t, 8> cnt{};
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (!ob || !ob->alive() || object_stance_type(ob) != st) continue;
		const int32_t v = gr.buildings ? static_cast<const Build*>(ob)->stance : static_cast<const Unit*>(ob)->stance;
		if (v >= 0 && v < int32_t(cnt.size())) cnt[size_t(v)]++;  // (the original indexes unchecked)
	}
	int32_t best = 0;
	for (int32_t k = 0; k < n; k++)
		if (cnt[size_t(k)] > 0 && cnt[size_t(best)] < cnt[size_t(k)]) best = k;
	return best;
}

// Unit::clear_mandatory @5e3890: every ATTACK order loses its mandatory flag.
void clear_mandatory(Unit& u) {
	for (Order& o : u.orders)
		if (o.type == kAttack) o.mandatory = 0;
}

// GroupData::get_form_option @70beb0 (the formation shown for the selection; FORM next / previous cycle from it).
int32_t group_form_option(Game& g, const Group& gr) {
	const int32_t n = get_num_const(g, gr);
	if (n == 0) return 0;
	if (n == 1) {
		if (gr.num > 0) {
			const int32_t lo = gr.buildings ? gr.list[0] : find_leader_o(g, gr, nullptr);
			if (lo >= 0) {
				const Object* ob = obj_at(g, gr.who, lo);
				if (ob && ob->alive()) return ob->is_unit ? static_cast<const Unit*>(ob)->form : 0;
			}
		}
		return 0;
	}
	// The original loops over the first `n` (= live count) list entries, not `num`.
	std::array<int32_t, 6> cnt{};
	for (int32_t i = 0; i < n; i++) {
		const Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (!ob || !ob->is_unit) continue;
		const int32_t f = static_cast<const Unit*>(ob)->form;
		if (f >= 0 && f < 6) cnt[size_t(f)]++;
	}
	int32_t best = 0;
	for (int32_t k = 1; k <= 4; k++)
		if (cnt[size_t(k)] > 0 && cnt[size_t(best)] < cnt[size_t(k)]) best = k;
	return best;
}

// ObjectData::get_inside @651a80: the outermost container (o, *who) of a carried unit, -1 when on the map.
int32_t get_inside(Game& g, const Unit& u, int32_t* who_out) {
	if (who_out) *who_out = -1;
	int32_t o = u.inside_up, who = u.inside_up_who;
	if (o < 0) return -1;
	for (int guard = 0; guard < kUnitLimit; guard++) {
		const Object* ob = obj_at(g, who, o);
		if (!ob || !ob->is_unit) break;
		const Unit* c = static_cast<const Unit*>(ob);
		if (c->inside_up < 0) break;
		who = c->inside_up_who;
		o = c->inside_up;
	}
	if (who_out) *who_out = who;
	return o;
}

// UnitData::is_packing @60aa60: the current order is a SPECIAL_ANIM of a packing animation. TODO(packers): not
// simulated (no SPECIAL_ANIM orders are created yet).
bool is_packing(const Unit& u) {
	(void)u;
	return false;
}

// Type vtable +0x10c (taken as is_siege: UnitTypeData unit_flags 0x20000; also assumed by action_halt).
bool is_siege(const Unit& u) { return (u.ptype->unit_flags & 0x20000) != 0; }

// LeaderData::can_transport @6e0c60.
int32_t leader_can_transport(const Game& g, int32_t who) {
	const uint32_t f = g.leaders[size_t(who)].flags;
	if (f & leaderflag::kCanTransportCiv) return 1;
	if (f & leaderflag::kCanTransportMil) return 2;
	return int32_t((f >> 10) & 3);
}

// UnitData::get_final_loc @608040: the target of the newest move or targeted order (newest first, up to `length`
// orders), or the unit's position when that tcoord is invalid for it.
void get_final_loc(Game& g, const Unit& u, int32_t* x, int32_t* y) {
	int32_t fx = -1, fy = -1;
	for (size_t k = u.orders.size(); k-- > 0;) {
		const Order& o = u.orders[k];
		if (order_is_move(o.type)) {
			fx = o.x;
			fy = o.y;
			break;
		}
		if (order_is_targeted(o.type)) {
			const Object* t = (o.ox >= 0 && o.whom >= 0) ? obj_at(g, o.whom, o.ox) : nullptr;
			if (t && t->alive()) {
				fx = t->x;
				fy = t->y;
				break;
			}
		}
	}
	if (unit_invalid_loc(g, u, tcoord_of(fx), tcoord_of(fy), true, false, false, false, false) == 0) {
		*x = fx;
		*y = fy;
	} else {
		*x = u.x;
		*y = u.y;
	}
}

// The member filter of the move loops: valid, on the map, not a plane.
Unit* move_member(Game& g, const Group& gr, int32_t i) { return action_member(g, gr, i); }

}  // namespace

int32_t group_get_loc(Game& g, const Group& gr, int32_t* x, int32_t* y) {
	// GroupData::get_loc @70e030
	int32_t lo = gr.num < 1 ? -1 : gr.buildings ? gr.list[0] : find_leader_o(g, gr, nullptr);
	if (lo < 0) return 1;
	int32_t who = gr.who;
	const Object* ob = obj_at(g, who, lo);
	if (!on_map(ob)) {
		int32_t w2 = -1;
		lo = get_inside(g, *static_cast<const Unit*>(ob), &w2);
		who = w2;
		ob = obj_at(g, who, lo);
	}
	*x = ob->x;
	*y = ob->y;
	if (gr.ox < 0 || gr.oy < 0) return 0;
	if (vector_dist(*x - gr.ox, *y - gr.oy) <= 0x180) {
		*x = gr.ox;
		*y = gr.oy;
		return 0;
	}
	if (!ob->is_unit) return 0;
	const Unit& L = *static_cast<const Unit*>(ob);
	const Order* m = L.front();  // vtable +0xd8 is_moving, get_order, +0xb8 get_move_order
	if (!m || !order_is_move(m->type)) return 0;
	if (vector_dist(m->x - gr.ox, m->y - gr.oy) > 0x180) return 0;
	*x = L.x - m->x + gr.ox;
	*y = L.y - m->y + gr.oy;
	return 0;
}

int32_t group_get_loc_to(Game& g, const Group& gr, int32_t* x, int32_t* y) {
	// GroupData::get_loc_to @70c5d0
	int32_t lo = gr.num < 1 ? -1 : gr.buildings ? gr.list[0] : find_leader_o(g, gr, nullptr);
	if (lo < 0) return 1;
	int32_t who = gr.who;
	const Object* ob = obj_at(g, who, lo);
	if (ob->is_unit && !static_cast<const Unit*>(ob)->on_map()) {
		int32_t w2 = -1;
		lo = get_inside(g, *static_cast<const Unit*>(ob), &w2);
		who = w2;
		ob = obj_at(g, who, lo);
	}
	if (!ob->is_unit) {
		*x = ob->x;
		*y = ob->y;
		return 0;
	}
	get_final_loc(g, *static_cast<const Unit*>(ob), x, y);
	if (gr.ox >= 0 && gr.oy >= 0 && vector_dist(*x - gr.ox, *y - gr.oy) <= 0x180) {
		*x = gr.ox;
		*y = gr.oy;
	}
	return 0;
}

std::vector<Order> group_set_up_insert(Game& g, Group& gr) {
	// Group::set_up_insert @70e520: copies of the leader's explicit (flag 4) orders, in execution order.
	std::vector<Order> saved;
	const int32_t lo = find_leader_o(g, gr, nullptr);
	if (lo < 0) return saved;
	const Unit& L = *g.unit(gr.who, lo);
	for (const Order& o : L.orders)
		if (o.flags & orderflag::kExplicit) saved.push_back(o);
	return saved;
}

void group_finish_insert(Game& g, Group& gr, std::vector<Order>& saved) {
	// Group::finish_insert @70e620: re-issue the saved orders as QUEUE_LAST group actions (oldest first).
	for (const Order& o : saved) {
		const bool has_orig = o.orig_x >= 0 && o.orig_y >= 0;
		switch (o.type) {
		case kMoveTo:
		case kAttackTo:
		case kExploreTo:
		case kFleeTo:
			if (has_orig)
				group_action_move_near(g, gr, o.orig_x, o.orig_y, 0, kQueueLast, true, o.angle,
				                       o.type, 1, -1, -1, 0);
			else group_action_move_near(g, gr, o.x, o.y, 0, kQueueLast, true, o.angle, o.type, 1, -1, -1, 0);
			break;
		case kChangeForm:
			group_action_form(g, gr, o.newform, 0, kQueueLast, 1);
			break;
		case kGroupMove:
			if (has_orig) group_action_move_near(g, gr, o.orig_x, o.orig_y, o.tolerance, kQueueLast, true, o.angle, kMoveTo, 1, -1, -1, 0);
			else group_action_move_near(g, gr, o.x, o.y, o.tolerance, kQueueLast, true, o.angle, kMoveTo, 1, -1, -1, 0);
			break;
		case kGroupAttackTo:
			if (has_orig) group_action_move_near(g, gr, o.orig_x, o.orig_y, o.tolerance, kQueueLast, true, o.angle, kAttackTo, 1, -1, -1, 0);
			else group_action_move_near(g, gr, o.x, o.y, o.tolerance, kQueueLast, true, o.angle, kAttackTo, 1, -1, -1, 0);
			break;
		case kAttack:
			// 🟡 re-issued as a QUEUE_LAST group attack on the same target (mandatory as saved).
			group_action_attack(g, gr, o.ox, o.whom, o.mandatory, kQueueLast, 0);
			break;
		case kGarrison:
			group_action_garrison(g, gr, o.ox, o.whom, kQueueLast, o.find);
			break;
		default:
			// TODO(orders-spec 2.1): BUILD_AT/REPAIR (action_swarm_around), GATHER, BOARD_SHIP, FOLLOW, GUARD,
			// CAST_SPELL, TRADE_ROUTE, GROUP_PATROL (redo_patrol_order), ATTACK_GROUND: those group actions
			// are not ported (and their orders are not created yet).
			break;
		}
	}
	saved.clear();
}

void group_action_stance(Game& g, Group& gr, int32_t s) {
	// Group::action_stance @70d440
	if (!group_is_on_map(g, gr)) return;
	gr.disband = 0;
	const int32_t st = group_stance_type(g, gr), n = stance_options(st);
	if (n == 0) return;
	if (s < 0) {
		const int32_t cur = group_stance_option(g, gr);
		if (s == -2) s = cur - 1 < 0 ? n - 1 : cur - 1;
		else s = (cur + 1) % n;
	}
	// (GroupData::count(COUNT_PEASANTS / COUNT_MILITARY) is called and its result discarded.)
	for (int32_t i = 0; i < gr.num; i++) {
		Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (!ob || !ob->alive()) continue;
		if (!ob->is_unit) {
			if (object_stance_type(ob) != st) continue;
			static_cast<Build*>(ob)->stance = int8_t(s);
			continue;
		}
		Unit& u = *static_cast<Unit*>(ob);
		if (object_stance_type(ob) != st || is_plane(u)) continue;
		u.stance = int8_t(s);
		u.flags |= objflag::kNewThink;
		if (st != kStanceCombat) continue;
		switch (s) {
		case 0:
		case 3:
		case 4:
			clear_mandatory(u);
			break;
		case 1:
		case 2:
		case 5: {
			if (!(g.leaders[size_t(gr.who)].flags & leaderflag::kHuman)) break;
			const Order* f = update_order(u);
			if (!f || (f->flags & orderflag::kExplicit)) break;
			const Order* a = update_action(g, u);
			if (a && (update_action(g, u)->flags & orderflag::kExplicit)) break;
			repath(g, u);
			kill_current_order(g, u, false);
			break;
		}
		default:
			clear_orders(g, u);
			break;
		}
	}
}

void group_action_move_to(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q, bool set_angle, uint32_t angle,
                          int32_t orders, int32_t p7, int32_t form, int32_t width, int32_t disembark) {
	// Group::action_move_to @70fba0
	group_action_move_near(g, gr, x, y, 0, q, set_angle, angle, orders, p7, form, width, disembark);
}

void group_action_move_near(Game& g, Group& gr, int32_t x, int32_t y, int32_t tol, QueuePos q, bool set_angle,
                            uint32_t angle, int32_t orders, int32_t p8, int32_t form, int32_t width, int32_t disembark) {
	// Group::action_move_near @704990
	if (!group_is_on_map(g, gr)) return;
	// TODO(orders-spec 2.1): ScenarioData::ignore_orders prelude (scenario scripts).
	if (gr.num <= 0) return;
	g.grouppath.clear();
	gr.disband = 0;  // action_begin
	World& w = g.world;
	x = std::clamp(x, 0, w.xs * 0x300 - 1);
	y = std::clamp(y, 0, w.ys * 0x300 - 1);
	const int32_t who = gr.who;

	// ---- land / sea split
	{
		Group A, B;  // temporary groups (clear(-1), id -1): A land units, B ships and the containers of carried units
		group_clear(A, -1);
		group_clear(B, -1);
		A.id = B.id = -1;
		for (int32_t i = 0; i < gr.num; i++) {
			const int32_t o = gr.list[size_t(i)];
			Object* ob = obj_at(g, who, o);
			if (!is_valid_unit(ob)) continue;
			const Unit& u = *static_cast<Unit*>(ob);
			if (u.ptype->domain == 1) {
				group_add(g, B, o, who, 0, 0);
			} else {
				int32_t w2 = -1;
				const int32_t in = get_inside(g, u, &w2);
				if (in < 0) group_add(g, A, o, who, 0, 0);
				else group_add(g, B, in, w2, 0, 0);
			}
		}
		bool split = false;
		if (B.num != 0) {
			if (A.num != 0 && gr.army < 0) {
				split = true;
			} else if ((w.t(tcoord_of(x), tcoord_of(y)) & tmask::kMedium) != tmask::kWater) {
				// Target on land: transports go to A, every other unit (or its container) to B.
				group_clear(A, -1);
				group_clear(B, -1);
				for (int32_t i = 0; i < gr.num; i++) {
					const int32_t o = gr.list[size_t(i)];
					Object* ob = obj_at(g, who, o);
					if (!is_valid_unit(ob)) continue;
					const Unit& u = *static_cast<Unit*>(ob);
					if (u.ptype->unit_flags & 0x10) {
						group_add(g, A, o, who, 0, 0);
					} else {
						int32_t w2 = -1;
						const int32_t in = get_inside(g, u, &w2);
						if (in < 0) group_add(g, B, o, who, 0, 0);
						else group_add(g, A, in, w2, 0, 0);
					}
				}
				split = B.num != 0 && A.num != 0 && gr.army < 0;
			}
		}
		if (split) {
			const int32_t sa = groups_push_group(g, who, A, 1);
			g.groups.g[size_t(sa)].facing = gr.facing;
			group_action_move_near(g, g.groups.g[size_t(sa)], x, y, tol, q, set_angle, angle, orders, p8, form, width,
			                       disembark);
			const int32_t sb = groups_push_group(g, who, B, 1);
			g.groups.g[size_t(sb)].facing = gr.facing;
			group_action_move_near(g, g.groups.g[size_t(sb)], x, y, tol, q, set_angle, angle, orders, p8, form, width,
			                       disembark);
			group_clear(g, gr, -1);
			return;
		}
	}
	if (gr.buildings) return;

	// ---- QUEUE_FIRST: the insert pattern
	if (q == kQueueFirst) {
		const int32_t lo = gr.num < 1 ? -1 : find_leader_o(g, gr, nullptr);
		std::vector<Order> saved = group_set_up_insert(g, gr);
		uint32_t explore = 0;
		if (group_get_num_cap(g, gr) == 1 && lo >= 0) {
			const Unit* L = g.unit(who, lo);
			if (L && L->alive()) explore = L->unit_masks & umask::kAutoExplore;
		}
		group_action_halt(g, gr, 0);
		group_action_move_near(g, gr, x, y, tol, kQueueNew, set_angle, angle, orders, p8, form, width, disembark);
		if (explore) g.unit(who, lo)->unit_masks |= umask::kAutoExplore;
		group_finish_insert(g, gr, saved);
		return;
	}

	// The function-static `cols` (SimpleArray<int>) is resized to num and never written: COLUMN intermediate
	// waypoints read it (uninitialised heap memory in the original, movement-spec 15.4). Zero here: every member
	// then uses slot 0's offset.
	g.group_cols.assign(size_t(gr.num), 0);

	const int32_t lo = find_leader_o(g, gr, nullptr);
	if (lo < 0) return;
	if (is_plane(*g.unit(who, lo))) return;
	int32_t form2 = form;
	if (form == 9 || form == -1) {
		form2 = get_form(g, gr);
		if (form2 < 0) form2 = 0;
	}
	gr.form = form2;
	const int32_t w2 = width == -1 ? get_form_mod_option(g, gr) : width;
	int32_t locx = 0, locy = 0;
	if (q == kQueueLast) group_get_loc_to(g, gr, &locx, &locy);
	else group_get_loc(g, gr, &locx, &locy);
	// AI armies (computer leader, army >= 0): local_40 = 1; armies whose +0x2c flag is set also pick a friendly city
	// (find_city) and garrison / gather the members there (local_44). TODO(armies, movement-spec 11.4): Army state is
	// not simulated, so the +0x2c branch is never taken.
	const bool ai_army = !(g.leaders[size_t(who)].flags & leaderflag::kHuman) && gr.army >= 0;
	auto army_skip = [&](const Unit& u) { return ai_army && is_siege(u) && u.order_type() == kAttack; };

	if (q == kQueueNew) {
		for (int32_t i = 0; i < gr.num; i++) {
			Unit* u = move_member(g, gr, i);
			if (!u || army_skip(*u)) continue;
			if ((u->ptype->unit_flags2 & 4) && is_packing(*u) && !u->orders.empty()) {
				// A packing packer keeps its current order.
				const Order keep = u->orders.front();
				u->orders.pop_front();
				clear_orders(g, *u);
				u->orders.push_back(keep);  // orderlist.add
				unit_update_gpiece(g, *u);
			} else {
				clear_orders(g, *u);
			}
		}
	}

	FormData* Fp = group_compute_form(g, gr, x, y, form2, w2, set_angle, &angle, locx, locy, 0);
	if (!Fp) return;
	FormData& F = *Fp;
	gr.disband = 0;
	if (q == kQueueNew || q == kQueueLast) {
		gr.o_angle = angle;
		gr.ox = x;
		gr.oy = y;
	}

	// ---- per member orders
	const int32_t xmax = w.xs * 0x300 - 1, ymax = w.ys * 0x300 - 1;
	int32_t last_cap = 0, reloc = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* up = move_member(g, gr, i);
		if (!up || army_skip(*up)) continue;
		Unit& u = *up;
		if (u.type < 50 || u.type > 53) u.form = int8_t(form2);
		u.form_mod = int8_t(w2);
		if (q == kQueueLast) std::reverse(u.path.begin(), u.path.end());  // Stack::invert
		const bool captain = u.o_up < 0;
		if (captain) last_cap = i;
		const int32_t flag = captain ? 0 : reloc;
		int32_t& tx = F.to_x[size_t(i)];
		int32_t& ty = F.to_y[size_t(i)];
		if (tx < 0) tx = 0;
		if (ty < 0) ty = 0;
		if (tx >= w.xs * 0x300) tx = xmax;
		if (ty >= w.ys * 0x300) ty = ymax;
		reloc = flag;
		if (i > 0) {
			const int32_t ri = w.get_tregion(tcoord_of(tx), tcoord_of(ty));
			const int32_t r0 = w.get_tregion(tcoord_of(F.to_x[0]), tcoord_of(F.to_y[0]));
			if (r0 != ri) {
				// Slot in another region than slot 0: look for a spot next to slot 0 (for the unit's type, or the
				// barge it would board, or the unit a transport carries).
				u.group = -1;
				int32_t t = u.type;
				if (unit_invalid_loc(g, u, tcoord_of(F.to_x[0]), tcoord_of(F.to_y[0]), true, true, false, false,
				                     false) != 0) {
					if (u.ptype->domain == 0 && leader_can_transport(g, who) != 0)
						t = current_upgrade_type(g, who, 0x140);  // TRANSPORTBARGE (no fallback here)
					if (u.ptype->domain == 1 && num_inside(g, u, 0) != 0) {
						const Object* c = obj_at(g, u.inside_down_who, u.inside_down);
						if (c) t = c->type;
					}
				}
				int32_t sx = 0, sy = 0;
				const int32_t r = find_nearby_spot(g, g.type(t), F.to_x[0], F.to_y[0], &sx, &sy, 0, -1, 0, 0x55555555u,
				                                   kFilterNotMe, -1, -1, 0, 0, -1, 0, -1);
				if (r == 0) {
					tx = sx;
					ty = sy;
				} else {
					tx = F.to_x[0];
					ty = F.to_y[0];
				}
				reloc = flag;
				u.group = int16_t(gr.id);
			} else {
				bool relocate = flag != 0;
				if (!relocate) {
					reloc = 0;
					if (!(u.unit_masks & umask::kAiControl) &&
					    unit_invalid_loc(g, u, tcoord_of(tx), tcoord_of(ty), false, false, false, false, false) != 0 &&
					    unit_invalid_loc(g, u, tcoord_of(F.to_x[0]), tcoord_of(F.to_y[0]), false, false, false, false,
					                     false) == 0)
						relocate = true;
				}
				if (relocate) {
					int32_t sx, sy;
					if (captain) {
						// Captain slot invalid while slot 0 is fine: a spot around slot 0.
						sx = F.to_x[0];
						sy = F.to_y[0];
						find_nearby_spot(g, *u.ptype, sx, sy, &sx, &sy, 0xc0, 0x480, -1, angle, kFilterNotMe, u.o,
						                 u.who, 0, 1, -1, 0, -1);
						reloc = 1;
					} else {
						// Follower of a relocated captain: beside the captain's slot.
						const int32_t cap_o = get_captain(g, u);
						sx = F.to_x[size_t(last_cap)];
						sy = F.to_y[size_t(last_cap)];
						const uint32_t a = u.o_up == cap_o ? angle + 0x40000000u : angle + 0xc0000000u;
						const int32_t gs = u.ptype->guy_spacing;
						find_nearby_spot(g, *u.ptype, sx, sy, &sx, &sy, gs, gs * 4, -1, a, kFilterNotMe, u.o, u.who, 0,
						                 0, -1, 0, -1);
						reloc = flag;
					}
					tx = sx;
					ty = sy;
				}
			}
		}
		QueuePos qq = q;
		if (q == kQueueNew && (u.ptype->unit_flags2 & 4) && is_packing(u) && !u.orders.empty()) {
			const Order keep = u.orders.front();
			u.orders.pop_front();
			clear_orders(g, u);
			u.orders.push_back(keep);
			unit_update_gpiece(g, u);
			qq = kQueueLast;
		}
		const uint32_t a = (uint32_t(int32_t(gr.angles[size_t(i)])) << 24) + angle;
		const bool group_move = (orders == kMoveTo || orders == kAttackTo) && !unit_is_modern_infantry(g, u) &&
		                        !((u.ptype->role & 0x10) && !(u.unit_masks & umask::kAiControl)) && gr.num > 1 &&
		                        !(u.unit_masks & umask::kInCombat) && u.ptype->domain != 1 && form2 != 9;
		if (group_move) {
			const int32_t move_id = int32_t((uint32_t(gr.id) + uint32_t(g.frame) * 10u) * 100u + uint32_t(gr.order_num));
			add_group_move_order(g, u, ucoord_of(tx), ucoord_of(ty), a, move_id, i, gr.list[size_t(F.idx)], who, orders,
			                     qq, p8 != 0, F.reverse, x, y, disembark);
		} else {
			add_move_facing_order(g, u, ucoord_of(tx), ucoord_of(ty), a, orders, true, qq, p8 != 0, F.reverse, x, y,
			                      disembark != 0);
		}
		u.unit_masks &= ~umask::kBuilder;
	}
	group_update_positions(g, gr, *g.unit(who, gr.list[size_t(F.idx)]));

	// ---- the shared group path (W level, from the leader to its slot), copied to every member with its offset
	const int32_t L1 = gr.num < 1 ? -1 : find_leader_o(g, gr, nullptr);
	std::vector<PathData>& gp = g.grouppath;
	int32_t near_slot = 0;  // local_50: leader already within 0x900 of its slot (no search)
	const Object* lob = L1 >= 0 ? obj_at(g, who, L1) : nullptr;
	if (is_valid_unit(lob) && on_map(lob)) {
		const Unit& L = *static_cast<const Unit*>(lob);
		int32_t sx = locx, sy = locy;
		if (!L.path.empty()) {
			sx = L.path.back().x;
			sy = L.path.back().y;
		}
		gp.clear();
		const int32_t lx = F.to_x[size_t(F.idx)], ly = F.to_y[size_t(F.idx)];
		gp.push_back({lx, ly, tol, pathflag::kFinal});
		if (vector_dist(sx - lx, sy - ly) < 0x900) near_slot = 1;
		else if (gr.army < 0) find_wpath(g, gp, sx, sy, who, L1);
		else find_wpath_army(g, gp, sx, sy, who, L1);
	}
	if (gp.empty()) {
		// No group path: every member plans its own W path to its slot.
		for (int32_t i = 0; i < gr.num; i++) {
			Unit* up = move_member(g, gr, i);
			if (!up || army_skip(*up)) continue;
			Unit& u = *up;
			gp.clear();
			const PathData fin{F.to_x[size_t(i)], F.to_y[size_t(i)], tol, pathflag::kFinal};
			gp.push_back(fin);
			int32_t sx = locx, sy = locy;
			if (!u.path.empty()) {
				sx = u.path.back().x;
				sy = u.path.back().y;
			}
			find_wpath(g, gp, sx, sy, who, u.o);
			if (gp.empty()) {
				u.path.push_back(fin);
			} else {
				while (!gp.empty()) {
					u.path.push_back(gp.back());
					gp.pop_back();
				}
			}
			std::reverse(u.path.begin(), u.path.end());
		}
	} else {
		while (!gp.empty()) {
			const PathData W = gp.back();
			gp.pop_back();
			const int32_t L2 = gr.num < 1 ? -1 : find_leader_o(g, gr, nullptr);
			const Object* l2 = obj_at(g, who, L2);
			const int32_t Lx = l2->x, Ly = l2->y;
			for (int32_t i = 0; i < gr.num; i++) {
				Unit* up = move_member(g, gr, i);
				if (!up || army_skip(*up)) continue;
				Unit& u = *up;
				if (!(g.leaders[size_t(who)].flags & leaderflag::kHuman) && u.ptype->domain == 1 &&
				    !(W.flags & pathflag::kFinal) && u.o != find_leader_o(g, gr, nullptr))
					continue;
				int32_t sx, sy;
				if (form2 == 8 && !(W.flags & pathflag::kFinal) && near_slot == 0) {
					const int32_t j = g.group_cols[size_t(i)];
					sx = F.to_x[size_t(j)];
					sy = F.to_y[size_t(j)];
				} else {
					sx = F.to_x[size_t(i)];
					sy = F.to_y[size_t(i)];
				}
				int32_t dx = sx - F.to_x[size_t(F.idx)], dy = sy - F.to_y[size_t(F.idx)];
				int32_t px = W.x + dx, py = W.y + dy;
				if (px < 0 || py < 0 || px >= w.tile_xs * 0xc0 || py >= w.tile_ys * 0xc0) {
					px = std::clamp(px, 0, xmax);
					py = std::clamp(py, 0, ymax);
					dx = px - W.x;
					dy = py - W.y;
				}
				px = dx + W.x;
				py = dy + W.y;
				if (w.get_tregion(tcoord_of(px), tcoord_of(py)) != w.get_tregion(tcoord_of(W.x), tcoord_of(W.y)) &&
				    unit_invalid_loc(g, u, tcoord_of(px), tcoord_of(py), true, true, false, false, false) != 0) {
					if (near_slot == 0) {
						// Snap into W's wcoord.
						py = (W.y / 0x300 - py / 0x300) * 0x300 + py;
						px = px + (W.x / 0x300 - px / 0x300) * 0x300;
					} else {
						px = W.x;
						py = W.y;
					}
				}
				bool push = u.o == L2 || (W.flags & pathflag::kFinal);
				if (!push) {
					// Intermediate points go to the leader and, unless they move as a group, to members within 0x600
					// of it.
					if (orders != kMoveTo || unit_is_modern_infantry(g, u) || u.ptype->domain == 1 || form2 == 9)
						push = vector_dist(u.x - Lx, u.y - Ly) < 0x600;
				}
				if (push) u.path.push_back({px, py, W.tol, W.flags});
			}
		}
		for (int32_t i = 0; i < gr.num; i++) {
			Unit* up = move_member(g, gr, i);
			if (up) std::reverse(up->path.begin(), up->path.end());
		}
	}
	gr.order_num++;
	F.clear_scratch();
}

void group_action_form(Game& g, Group& gr, int32_t form, int32_t rotate, QueuePos q, int32_t p5) {
	// Group::action_form @707220
	if (!group_is_on_map(g, gr)) return;
	gr.disband = 0;
	if (gr.buildings || gr.num < 1) return;
	const int32_t lo = find_leader_o(g, gr, nullptr);
	if (lo < 0 || is_plane(*g.unit(gr.who, lo))) return;
	bool insert_check = true;
	if (form == -3) {
		form = group_form_option(g, gr) - 1;
		if (form < 0) form = 4;
	} else if (form == -1) {
		form = (group_form_option(g, gr) + 1) % 5;
	}
	if (form == -2) {
		gr.form = -2;
		const int32_t u0 = (gr.buildings || gr.num <= 0) ? -1 : find_leader_o(g, gr, nullptr);  // get_unit
		if (u0 < 0) return;
		form = g.unit(gr.who, u0)->form;
		insert_check = false;
	}
	if (insert_check && (q == kQueueNew || q == kQueueFirst) && p5 == 0) {
		gr.form = -1;
		std::vector<Order> saved = group_set_up_insert(g, gr);
		group_action_halt(g, gr, 0);
		group_action_form(g, gr, form, rotate, saved.empty() ? kQueueNew : kQueueFirst, 1);
		group_finish_insert(g, gr, saved);
		return;
	}
	for (int32_t i = 0; i < gr.num; i++) {
		Object* ob = obj_at(g, gr.who, gr.list[size_t(i)]);
		if (ob && ob->is_unit && !is_plane(*static_cast<Unit*>(ob))) static_cast<Unit*>(ob)->form = int8_t(form);
	}
	if (q != kQueueLast && q != kQueueNew) return;
	int32_t lx = 0, ly = 0;
	group_get_loc_to(g, gr, &lx, &ly);
	bool set_angle = false;
	uint32_t a = 0;
	if (rotate != 0) {
		if (lx == gr.ox && ly == gr.oy) {
			a = gr.o_angle;
		} else {
			const int32_t l2 = gr.num > 0 ? (gr.buildings ? gr.list[0] : find_leader_o(g, gr, nullptr)) : -1;
			a = l2 >= 0 ? g.unit(gr.who, l2)->angle : 0;
		}
		a += uint32_t(rotate);
		set_angle = true;
	}
	group_action_move_to(g, gr, lx, ly, kQueueLast, set_angle, a, kMoveTo, 0, -1, -1, 0);
}


// ---------------------------------------------------------------- unit side

void add_group_move_order(Game& g, Unit& u, int32_t ux, int32_t uy, uint32_t angle, int32_t move_id, int32_t slot,
                          int32_t leader_o, int32_t leader_who, int32_t orders, QueuePos q, bool expl, int32_t reverse,
                          int32_t orig_x, int32_t orig_y, int32_t disembark) {
	// Unit::add_group_move_order @5e4710
	Order o;
	o.type = orders == 2 ? kGroupAttackTo : kGroupMove;
	o.x = ux * 48 + 24;
	o.y = uy * 48 + 24;
	o.angle = angle;
	o.dest = 0;
	o.dest_x = o.x;
	o.dest_y = o.y;
	o.last_x = o.last_y = -1;
	o.flags |= orderflag::kPathed;
	o.off_x = int16_t(o.x - (o.x / 0x300) * 0x300);
	o.off_y = int16_t(o.y - (o.y / 0x300) * 0x300);
	o.orig_x = orig_x;
	o.orig_y = orig_y;
	o.facing = reverse;
	o.oxx = leader_o;
	o.whose = leader_who;
	o.id = move_id;
	o.form_id = slot;
	o.timer = o.pause = o.retry = o.tolerance = 0;
	o.group_angle = angle;
	if (expl) o.flags |= orderflag::kExplicit;
	else o.flags &= uint8_t(~orderflag::kExplicit);
	if (disembark) o.flags |= orderflag::kDisembark;
	else o.flags &= uint8_t(~orderflag::kDisembark);
	add_order(g, u, o, q);
}

int32_t do_group_move(Game& g, Unit& u, Order& m) {
	// Unit::do_group_move @5e79a0 (GROUP_MOVE, and the movement of GROUP_ATTACK_TO)
	if (u.group == -1) {
		ungroup_move_order(g, u);
		return 0;
	}
	Group& gr = g.groups.g[size_t(u.group)];
	const int32_t lead_o = m.oxx, lead_who = m.whose;
	if (lead_o == u.o && lead_who == u.who) {
		// ---- the leader
		if (const Order* act = get_action(u); act && act->type == kAttack && (g.frame + u.o) % 32 == 0 &&
		                                       update_action(g, u)) {
			// An attacking leader close to its target ends the group move.
			const Object* t = (act->ox >= 0 && act->whom >= 0) ? g.object(act->whom, act->ox) : nullptr;
			if (t && vector_dist(u.x - t->x, u.y - t->y) < 0x900) {
				group_kill_group_move(g, gr, m.id);
				return 0;
			}
		}
		if (do_move(g, u, m) != 0) {
			if (update_order(u) != &m) return 0;
			const int32_t s = get_speed(g, u, u.x, u.y, true);
			const int32_t old = gr.new_speed;
			gr.new_speed = s;
			gr.speed = old;
			gr.march = 0;
			if ((g.leaders[size_t(u.who)].flags & 0x8000) && has_general(g, u, 0x8000, -1) >= 0) gr.march = 1;
			group_update_positions(g, gr, u);
			return 0;
		}
		if (update_order(u) == &m) {
			const OrderType at = action_type(u);
			if ((at == kAttack || at == kGroupAttack || m.type == kGroupAttackTo) && u.collide != 0 && u.group >= 0) {
				// combat-spec 3.3.1 / 3.3.2: an enemy collider is attacked by the group.
				const int32_t co = u.collide_o, cw = u.collide_who;
				if (valid_target(g, u, co, cw, nullptr)) {
					if (m.type == kGroupAttackTo) {
						group_action_attack(g, gr, co, cw, 0, kQueueFirst, 0);
					} else if (const Order* ac = get_action(u)) {
						const int32_t aox = ac->ox, aw = ac->whom;
						group_kill_group_move(g, gr, m.id);
						group_distribute_attack(g, gr, aox, aw);
					}
					return 0;
				}
			}
		}
		if (u.order_type() != kGroupMove && u.order_type() != kGroupAttackTo) return 0;
		if (update_order(u) == &m) ungroup_move_order(g, u);
		return 0;
	}

	// ---- a member: is the leader still leading this move?
	bool lost = false;
	{
		const Object* lob = g.object(lead_who, lead_o);
		const Unit* L = (lob && lob->is_unit && lob->alive() && static_cast<const Unit*>(lob)->on_map())
		                    ? static_cast<const Unit*>(lob) : nullptr;
		if (!L || L->group != u.group || L->orders.empty()) {
			lost = true;
		} else {
			const Order& lf = L->orders.front();
			if (!order_is_group(lf.type) && lf.type != kChangeForm) lost = true;
			else if (order_is_group(lf.type) && lf.id != m.id) lost = true;
		}
	}
	int32_t slot = -1;
	for (int32_t i = 0; i < gr.num; i++)
		if (gr.list[size_t(i)] == u.o) {
			slot = i;
			break;
		}
	if (slot < 0) u.group = -1;
	else m.form_id = slot;
	if (slot < 0 || lost) {
		if (u.group >= 0 && vector_dist(u.x - m.x, u.y - m.y) >= 0x600) {
			group_refresh_group_order(g, gr, u);
			return 0;
		}
		ungroup_move_order(g, u);
		return 0;
	}
	if (const Order* act = get_action(u); act && act->type == kAttack) {
		if (act->ox >= 0 && act->whom >= 0 && g.object(act->whom, act->ox) && is_in_range(g, u, act->ox, act->whom, 0)) {
			kill_current_order(g, u);
			return 0;
		}
	}
	const Unit& L = *g.unit(lead_who, lead_o);
	// My slot: the leader's position plus my rotated offset (Group::update_positions).
	const PathData T{L.x + gr.curr_x[size_t(m.form_id & 127)], L.y + gr.curr_y[size_t(m.form_id & 127)], 0, 0};
	// Drop my intermediate waypoints down to the final one (an empty stack yields a zeroed entry, see below).
	PathData Wp{};
	for (;;) {
		if (u.path.empty()) {
			// The original sets length = 1 and reads the stale entry 0 of its array; a zeroed entry here.
			Wp = PathData{};
			break;
		}
		Wp = u.path.back();
		u.path.pop_back();
		if (Wp.flags & pathflag::kFinal) break;
		if (u.path.empty()) break;
	}
	if (!(Wp.flags & pathflag::kFinal)) Wp.flags |= pathflag::kFinal;
	u.path.push_back(Wp);
	const int32_t ds = vector_dist(T.x - u.x, T.y - u.y);
	const int32_t dw = vector_dist(Wp.x - u.x, Wp.y - u.y);
	if (dw - ds <= 0x60 || dw <= 0x180) {
		// Close to the final point: continue as a plain move.
		m.in_group = 0;
		m.dest_x = Wp.x;
		m.dest_y = Wp.y;
		m.dest = 1;
		ungroup_move_order(g, u);
		return 0;
	}
	const uint32_t at = find_angle(T.x - u.x, T.y - u.y);
	int32_t reduced = 0;
	auto follow_slot = [&]() {
		u.path.push_back(T);
		m.dest_x = T.x;
		m.dest_y = T.y;
		m.in_group = 1;
		reduced = 0;
	};
	if (lead_o == get_captain(g, u)) {
		follow_slot();
	} else {
		uint32_t d = at - find_angle(Wp.x - u.x, Wp.y - u.y);
		if (d > 0x80000000u) d = ~d;
		if (d < 0x55555555u) {
			follow_slot();  // the slot is ahead (within 120 degrees of the way to the final point)
		} else {
			if (ds < 0x30) return 0;  // I am ahead of my slot and on it: wait
			const Unit& Lu = *g.unit(gr.who, lead_o);
			if (Lu.path.empty() || m.in_group == 0) {
				m.dest_x = (T.x + Wp.x) / 2;
				m.dest_y = (T.y + Wp.y) / 2;
				m.dest = 1;
				reduced = 1;
			} else {
				const PathData& lt = Lu.path.back();
				const PathData P{gr.curr_x[size_t(m.form_id & 127)] + lt.x, gr.curr_y[size_t(m.form_id & 127)] + lt.y, 0, 0};
				u.path.push_back(P);
				m.dest_x = P.x;
				m.dest_y = P.y;
				m.dest = 1;
				uint32_t d2 = at - u.angle;
				if (d2 > 0x80000000u) d2 = ~d2;
				reduced = 0x55555555u < d2 ? 1 : 0;
				if (vector_dist(P.x - u.x, P.y - u.y) < 0x60) m.in_group = 0;
			}
		}
	}
	m.dest = 1;
	if (m.dest_x == u.x && m.dest_y == u.y) return 0;
	u.tolerance = 0;
	int32_t s;
	if (reduced == 0) {
		if (gr.march == 0 || has_general(g, u, 0x8000, -1) >= 0) {
			const int32_t mine = unit_speed(g, u);
			if (mine < gr.speed) {
				gr.speed = mine;
				gr.new_speed = mine;
			}
		}
		s = get_speed(g, u, u.x, u.y, true);
		s += std::min(s / 3, 9);  // catch-up
	} else {
		s = get_speed(g, u, u.x, u.y, false) / 2;
	}
	if (unit_invalid_loc(g, u, tcoord_of(m.dest_x), tcoord_of(m.dest_y), false, false, false, false, false) != 0) {
		const int32_t ddx = m.dest_x - u.x, ddy = m.dest_y - u.y;
		if ((ddy < 0 ? -ddy : ddy) + (ddx < 0 ? -ddx : ddx) < 0x300) {
			const int32_t wx = std::clamp(wcoord_of(m.dest_x), 0, g.world.xs - 1);  // WorldData::restrict
			const int32_t wy = std::clamp(wcoord_of(m.dest_y), 0, g.world.ys - 1);
			if ((g.world.w(wx, wy).flags & wflag::kForest) && u.group >= 0) {
				const int32_t v = int32_t(g.rng.get(0, 0xffff)) % 2;  // game_random @5e8491
				add_flock(g, wx * 4 + 2, wy * 4 + 2, lead_who, v + 2);
			}
		}
		ungroup_move_order(g, u);
		return 0;
	}
	// Units that fire while moving (combat-spec 3.10): cavarch_fight before the step (no reset here).
	if ((u.ptype->unit_flags & 0x200000) && obj_attack(g, u) != 0 && get_combat_stance(g, u) != kHoldFire) {
		const Order* ca = get_action(u);
		if ((!ca || ca->type != kAttack || (u.ptype->unit_flags & 0x400)) && (u.cavarch_o >= -1 || (u.o + g.frame) % 32 == 0)) {
			cavarch_fight(g, u);
			if (!u.alive() || update_order(u) != &m) return 0;
		}
	}
	if (move_step(g, u, m, s) != 0) return 0;
	if (update_order(u) != &m) return 0;
	{
		const OrderType at2 = action_type(u);
		if ((at2 == kAttack || at2 == kGroupAttack || m.type == kGroupAttackTo) && u.collide != 0) {
			const int32_t co = u.collide_o, cw = u.collide_who;
			if (valid_target(g, u, co, cw, nullptr)) {
				if (m.type == kGroupAttackTo && u.group >= 0) group_action_attack(g, g.groups.g[size_t(u.group)], co, cw, 0, kQueueFirst, 0);
				else kill_current_order(g, u);
				return 0;
			}
		}
	}
	if (update_order(u) == &m) ungroup_move_order(g, u);
	return 0;
}


void ungroup_move_order(Game& g, Unit& u) {
	// Unit::ungroup_move_order @5fd140 with the id of the unit's front group move and follower = 0: GROUP_MOVE ->
	// MOVE_TO, GROUP_ATTACK_TO -> ATTACK_TO (MoveOrder copied except timer, dest = 0, orig = target) at the front,
	// down the captain chain.
	const Order* f = u.front();
	if (!f || !(f->type == kGroupMove || f->type == kGroupAttackTo)) return;
	const int32_t id = f->id;
	Unit* p = &u;
	while (p->o_up >= 0) {
		Unit* q = g.unit(p->who, p->o_up);
		if (!q) break;
		p = q;
	}
	for (int guard = 0; p && guard < kUnitLimit; guard++) {
		ptrdiff_t found = -1;
		for (size_t k = p->orders.size(); k-- > 0;) {
			const Order& o = p->orders[k];
			if ((o.type == kGroupMove || o.type == kGroupAttackTo) && o.id == id) {
				found = ptrdiff_t(k);
				break;
			}
		}
		if (found >= 0) {
			const Order gm = p->orders[size_t(found)];
			const bool leader = gm.oxx == p->o && gm.whose == p->who;
			Order n;
			n.type = gm.type == kGroupAttackTo ? kAttackTo : kMoveTo;
			n.x = gm.x, n.y = gm.y, n.angle = gm.angle, n.dest = gm.dest, n.tolerance = gm.tolerance;
			n.pause = gm.pause, n.retry = gm.retry, n.attempts = gm.attempts, n.facing = gm.facing;
			n.dest_x = gm.dest_x, n.dest_y = gm.dest_y, n.last_x = gm.last_x, n.last_y = gm.last_y;
			n.coll_x = gm.coll_x, n.coll_y = gm.coll_y, n.off_x = gm.off_x, n.off_y = gm.off_y;
			n.flags = gm.flags;
			if (!leader) n.flags &= uint8_t(~orderflag::kPathed);
			n.dest = 0;
			n.orig_x = n.x;
			n.orig_y = n.y;
			p->orders.erase(p->orders.begin() + found);
			p->orders.push_front(n);
			if (!leader) kill_current_path(g, *p);
		}
		if (p->o_down < 0) return;
		p = g.alive_unit(p->who, p->o_down);
	}
}

void unit_kill_group_move(Game& g, Unit& u, int32_t id) {
	// Unit::kill_group_move @5e3400, with the original's list iterator: walk front -> newest; a pathed move clears
	// its pathed bit and kills the CURRENT path; a matching group move kills the CURRENT (front) order, after which
	// the walk resumes from where update_action left the iterator.
	if (u.orders.empty()) return;
	size_t cur = 0;
	for (int guard = 0; guard < 4096; guard++) {
		size_t at = cur;
		{
			Order& o = u.orders[cur];
			if (order_is_move(o.type) && o.type != kGroupAttackTo && order_is_pathed(o)) {
				o.flags &= uint8_t(~orderflag::kPathed);
				kill_current_path(g, u);
			}
		}
		const Order& o = u.orders[cur];
		if (order_is_group(o.type) && o.id == id && order_is_move(o.type) && o.type != kGroupAttackTo) {
			kill_current_order(g, u, false);
			if (u.orders.empty()) return;
			// update_action's stop node
			size_t i = 0;
			while (i + 1 < u.orders.size()) {
				const Order& q = u.orders[i];
				if (!((order_is_move(q.type) && !(q.flags & orderflag::kExplicit)) || q.type == kChangeForm)) break;
				i++;
			}
			at = i;
		}
		if (u.orders.empty() || at + 1 >= u.orders.size()) return;
		cur = at + 1;
	}
}

}  // namespace ron::sim
