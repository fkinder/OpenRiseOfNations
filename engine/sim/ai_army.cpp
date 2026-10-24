// AI armies (docs/ai-spec.md 12.1-12.5): Armies / Army — creation and membership, Armies::process_all and the
// Army::process state machine (muster, defend, march, form up, engage, transport), target selection (find_target
// with its F1-F3 draws), muster spots, send_here / charge, and the helpers (find_army & co., emergency,
// diplo_change, send_navy), plus Unit::add_to_army / go_to_unit / Object::get_army and GameDaemon::calc_danger.
//
// Evidence (local, gitignored): re/decomp/ai/Army_*.c, Armies_*.c, Unit_add_to_army@005f7740.c, the disassembly in
// re/decomp/ai_mil_asm and re/decomp/ai_port/asm (Army::process, find_target, normalize), re/decomp/ai_port/
// GameDaemon_calc_danger / do_danger, re/decomp/bld2/GroupData_count, re/decomp/move/Unit_go_to.
#include <algorithm>

#include "sim/ai_internal.hpp"
#include "sim/combat.hpp"
#include "sim/orders.hpp"
#include "sim/search.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace ai;

namespace {

// CountIndex values (GroupData::count @711720).
constexpr int32_t kCountAttack = 2, kCountSiege = 4, kCountCasters = 5, kCountDecoys = 6, kCountType = 17,
                  kCountNonDecoyType = 19;
constexpr int32_t kGeneral = 54, kSupplyWagon = 63, kCataphract = 227, kHoplites = 132;

Group& GR(Game& g, int32_t id) { return g.groups.g[size_t(id)]; }
int32_t wc(int32_t c) { return wcoord_of(c); }
int32_t centre(int32_t w) { return w * 768 + 384; }
bool war(const Game& g, int32_t a, int32_t b) {
	return b != a && (LD(g, a).diplos(b) == 0 || LD(g, b).diplos(a) == 0);
}
bool mutual(const Game& g, int32_t a, int32_t b) { return mutual_ally_ai(g, a, b); }
int32_t age_mil(const Game& g, int32_t p) { return LD(g, p).epoch[0]; }  // the military library level
int32_t ctype(const Game& g, int32_t who, const City& C) {
	const Build* b = g.build(who, C.o);
	const int32_t t = b ? b->type : -1;
	return t == ptype::kTown ? 2 : (t == ptype::kMetropolis || t == ptype::kForbiddenCity) ? 3 : 1;
}
bool is_city_obj(const Object* o) { return o && !o->is_unit && (o->flags & objflag::kCity); }
int32_t army_hits(const Game& g, const Object& o) { return obj_hits(g, o); }  // vslot 0x11c hits(0)
void clamp_map(const Game& g, int32_t* x, int32_t* y) {
	*x = std::clamp(*x, 0, g.world.xs * 768 - 1);
	*y = std::clamp(*y, 0, g.world.ys * 768 - 1);
}

// GroupData::count(kind, p, 0) for the kinds the armies use.
int32_t gcount(const Game& g, const Group& gr, int32_t kind, int32_t p) {
	int32_t n = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* ob = g.object(gr.who, gr.list[size_t(i)]);
		if (!ob || !ob->alive()) continue;
		const Unit* u = ob->is_unit ? static_cast<const Unit*>(ob) : nullptr;
		switch (kind) {
		case kCountAttack:
			// TODO(transports): a map barge (320) carrying an attack captain also counts.
			if (ob->ptype->attack != 0 && (!u || u->o_up < 0)) n++;
			break;
		case kCountSiege:
			if (u && (u->ptype->unit_flags & 0x20000) && (u->unit_masks & 0x40001) != 0x40001) n++;
			break;
		case kCountCasters:
			if (u && (u->ptype->unit_flags2 & 2) && !(u->unit_masks & umask::kDecoy)) n++;
			break;
		case kCountDecoys:
			if (u && (u->unit_masks & umask::kDecoy) && u->o_up < 0) n++;
			break;
		case kCountType:
			if (type_is(*ob->ptype, p, false)) n++;
			break;
		case kCountNonDecoyType:
			if (u && (u->unit_masks & umask::kDecoy)) break;
			if (type_is(*ob->ptype, p, false)) n++;
			break;
		default: break;
		}
	}
	return n;
}

int32_t army_count(const Game& g, const Army& A, int32_t kind, int32_t p) {  // Army::count @6f9120
	int32_t n = 0;
	for (int32_t i = 0; i < A.num_groups; i++) {
		const int32_t id = A.list[size_t(i)];
		if (id >= 0) n += gcount(g, g.groups.g[size_t(id)], kind, p);
	}
	return n;
}

int32_t leader_o_of(Game& g, int32_t id) {  // the group's leader unit o (buildings group: list[0]), -1
	const Group& gr = GR(g, id);
	if (gr.num < 1) return -1;
	if (gr.buildings) return gr.list[0];
	const int32_t li = group_find_leader(g, gr);
	return li < 0 ? -1 : gr.list[size_t(li)];
}

void army_normalize(Game& g, Army& A);

void remove_group(Game& g, Army& A, int32_t id) {  // Army::remove_group @6f8b50
	if (GR(g, id).army != A.army || A.num_groups == 0) return;
	for (int32_t i = 0; i < A.num_groups; i++) {
		if (A.list[size_t(i)] != id) continue;
		for (int32_t k = i; k + 1 < A.num_groups; k++) A.list[size_t(k)] = A.list[size_t(k + 1)];
		A.num_groups--;
		break;
	}
	GR(g, id).army = -1;
	army_normalize(g, A);
}

void army_normalize(Game& g, Army& A) {  // Army::normalize @6f9b50
	A.role = A.num_units = A.num_captains = A.num_standard = A.num_decoys = 0;
	for (int32_t i = A.num_groups - 1; i >= 0; i--) {
		const int32_t id = A.list[size_t(i)];
		if (id < 0) continue;
		Group& gr = GR(g, id);
		group_normalize(g, gr);
		if (gr.num == 0 || gr.buildings) {
			remove_group(g, A, id);
			return;
		}
		A.num_units += gr.num;
		const int32_t c = group_get_num_cap(g, gr);
		A.num_captains += c;
		const int32_t dec = gcount(g, gr, kCountDecoys, 0);
		A.num_decoys += dec;
		A.num_standard += c - gcount(g, gr, kCountCasters, 0) - gcount(g, gr, kCountNonDecoyType, kSupplyWagon) - dec -
		                  gcount(g, gr, kCountNonDecoyType, ptype::kAaGun);
		A.role |= gr.role;
	}
	// The insertion sort of list[1..] by the leader unit's category (TypeData.cat), ascending (stable).
	// TODO(ai-spec 12.11.4): the exact tie / empty-group behaviour of the original's sort.
	for (int32_t i = 1; i < A.num_groups; i++) {
		if (A.list[size_t(i)] < 0 || GR(g, A.list[size_t(i)]).num == 0) continue;
		const int32_t li = leader_o_of(g, A.list[size_t(i)]);
		const Object* lo = li >= 0 ? g.object(A.who, li) : nullptr;
		const int32_t ci = lo ? lo->ptype->cat : 0;
		int32_t k = i;
		while (k > 0) {
			const int32_t pj = A.list[size_t(k - 1)];
			if (pj < 0 || GR(g, pj).num == 0) break;
			const int32_t lj = leader_o_of(g, pj);
			const Object* jo = lj >= 0 ? g.object(A.who, lj) : nullptr;
			if ((jo ? jo->ptype->cat : 0) <= ci) break;
			std::swap(A.list[size_t(k - 1)], A.list[size_t(k)]);
			k--;
		}
	}
}

void add_group(Game& g, Army& A, int32_t id) {  // Army::add_group @6f8c00
	if (A.num_groups >= 16) return;
	Group& gr = GR(g, id);
	if (gr.who != A.who) return;
	A.list[size_t(A.num_groups++)] = id;
	gr.army = A.army;
	A.num_units += gr.num;
	A.num_captains += group_get_num_cap(g, gr);
	A.role |= gr.role;
}

bool army_member_o(const Game& g, const Army& A, int32_t o) {  // Army::member(o, who) @6f8de0
	for (int32_t i = 0; i < A.num_groups; i++) {
		const int32_t id = A.list[size_t(i)];
		if (id >= 0 && group_member(g, g.groups.g[size_t(id)], o, A.who)) return true;
	}
	return false;
}

void set_group_chain(Game& g, int32_t who, int32_t o, int32_t id) {  // Unit::set_group @605220
	for (Unit* u = g.unit(who, o); u;) {
		u->group = int16_t(id);
		u = u->o_down >= 0 ? g.unit(who, u->o_down) : nullptr;
	}
}

void add_unit(Game& g, Army& A, int32_t o) {  // Army::add_unit @6f9f40
	const Unit* u = g.unit(A.who, o);
	if (!u || !u->alive()) return;
	if (army_member_o(g, A, o)) return;
	int32_t id;
	if (A.num_groups < 1 || A.list[0] < 0) {
		Group tmp;
		group_clear(tmp, -1);
		group_add(g, tmp, o, A.who, 0, 0);
		id = groups_push_group(g, A.who, tmp, 1);
		if (id < 0) return;
		add_group(g, A, id);
	} else {
		id = A.list[0];
		const int32_t n0 = GR(g, id).num;
		group_add(g, GR(g, id), o, A.who, 0, 0);
		A.num_units += GR(g, id).num - n0;
		A.num_captains += 1;
	}
	set_group_chain(g, A.who, o, id);
}

int32_t get_unit(const Game& g, const Army& A, int32_t i) {  // ArmyData::get_unit @6f9df0
	for (int32_t k = 0; k < A.num_groups; k++) {
		const int32_t id = A.list[size_t(k)];
		if (id < 0) continue;
		const Group& gr = g.groups.g[size_t(id)];
		if (gr.buildings) continue;
		if (i < gr.num) return gr.list[size_t(i)];
		i -= gr.num;
	}
	return -1;
}

void center_of_gravity(const Game& g, const Army& A, int32_t* wx, int32_t* wy) {
	int64_t sx = 0, sy = 0;
	int32_t n = 0;
	for (int32_t i = 0; i < A.num_units; i++) {
		const int32_t o = get_unit(g, A, i);
		const Unit* u = o >= 0 ? g.unit(A.who, o) : nullptr;
		if (!u || !u->alive() || !u->on_map()) continue;
		sx += wc(u->x);
		sy += wc(u->y);
		n++;
	}
	if (n == 0) {
		*wx = wc(A.x);
		*wy = wc(A.y);
		return;
	}
	*wx = int32_t(sx / n);
	*wy = int32_t(sy / n);
}

bool is_engaged(Game& g, Army& A) {  // Army::is_engaged @6f56d0
	army_normalize(g, A);
	int32_t count = 0, cx = 0, cy = 0;
	bool cog = false;
	for (int32_t i = 0; i < A.num_units; i++) {
		const int32_t o = get_unit(g, A, i);
		const Unit* u = o >= 0 ? g.unit(A.who, o) : nullptr;
		if (!u || !u->alive() || !u->on_map()) continue;
		const Order* a = get_action(*u);
		if (!a || a->type != kAttack) continue;
		if (vector_dist(u->x - A.x, u->y - A.y) >= 0xc00) continue;
		if (!cog) {
			center_of_gravity(g, A, &cx, &cy);
			cog = true;
		}
		if (vector_dist(centre(cx) - u->x, centre(cy) - u->y) <= 0xc00) count++;
	}
	return count != 0 && count > A.num_units / 4;
}

bool is_moving(Game& g, const Army& A) {  // Army::is_moving @6f5470 (no normalize)
	int32_t count = 0, cx = 0, cy = 0;
	bool cog = false;
	for (int32_t i = 0; i < A.num_units; i++) {
		const int32_t o = get_unit(g, A, i);
		if (o < 0) continue;
		const Unit* u = g.unit(A.who, o);
		if (!u || !u->alive() || !u->on_map()) continue;
		const Order* a = u->front();
		if (!a) continue;
		if (!order_is_move(a->type)) {
			if (g.world.w(wc(u->x), wc(u->y)).region != A.reg) count++;
		} else {
			if (!cog) {
				center_of_gravity(g, A, &cx, &cy);
				cog = true;
			}
			if (vector_dist(centre(cx) - u->x, centre(cy) - u->y) < 0xf01) count++;
		}
	}
	return A.num_units / 6 < count;
}

void set_stance(Game& g, Army& A, int32_t s) {  // Army::set_stance @6f8750
	for (int32_t i = 0; i < A.num_groups; i++) {
		const int32_t id = A.list[size_t(i)];
		if (id < 0 || GR(g, id).num == 0) continue;
		const int32_t lo = leader_o_of(g, id);
		const Object* l = lo >= 0 ? g.object(A.who, lo) : nullptr;
		if (!l || !l->is_unit || unit_stance_type(*l->ptype) != kStanceCombat) continue;
		group_action_stance(g, GR(g, id), s);
	}
}

void army_close(Game& g, Army& A) {  // Army::close @6f8ea0
	if (!A.valid) return;
	for (int32_t i = 0; i < A.num_groups; i++) {
		const int32_t id = A.list[size_t(i)];
		if (id < 0 || GR(g, id).army != A.army) continue;
		GR(g, id).army = -1;
		group_action_halt(g, GR(g, id), 0);
	}
	A.valid = 0;
	A.status = 0;
	A.human_frame = 0;
	A.num_groups = 0;
}

void move_group_to(Game& g, int32_t id, int32_t x, int32_t y, QueuePos q, bool set_angle, uint32_t angle,
                   int32_t order, int32_t expl = 1) {
	group_action_move_to(g, GR(g, id), x, y, q, set_angle, angle, order, expl, -1, -1, 0);
}

void send_here(Game& g, Army& A, int32_t x, int32_t y, int32_t order) {  // Army::send_here @6f98a0
	clamp_map(g, &x, &y);
	if (x != A.x && y != A.y) A.muster_angle = find_angle(x - A.x, y - A.y);
	A.x = x;
	A.y = y;
	A.muster_x = wc(x);
	A.muster_y = wc(y);
	int32_t px = x, py = y;
	for (int32_t i = 0; i < A.num_groups; i++) {
		const int32_t id = A.list[size_t(i)];
		if (id < 0 || GR(g, id).num == 0) continue;
		GR(g, id).army = A.army;
		move_group_to(g, id, px, py, kQueueNew, true, A.muster_angle, order);
		project(px, py, A.muster_angle + 0x80000000u, 384, &px, &py);
	}
}

// Group::action_siege_attack_to @70d830: the siege members (or a supply wagon, else a hero, for computer owners) move
// to the point; TODO(ai-spec 12.11.7): the rest should escort them with Group::action_guard (not ported: the guard
// order is not simulated), approximated by an ATTACK_TO move of the whole group behind them.
void siege_attack_to(Game& g, int32_t id, int32_t x, int32_t y, uint32_t angle) {
	Group& gr = GR(g, id);
	if (gr.buildings || gr.num < 1) return;
	const int32_t who = gr.who;
	Group S;
	group_clear(S, -1);
	S.army = gr.army;
	for (int32_t i = 0; i < gr.num; i++) {
		const Unit* u = g.unit(who, gr.list[size_t(i)]);
		if (u && u->alive() && u->on_map() && (u->ptype->unit_flags & 0x20000)) group_add(g, S, u->o, who, 0, 0);
	}
	const bool human = (LD(g, who).flags & leaderflag::kHuman) != 0;
	if (!human && S.num == 0) {
		for (int32_t pass = 0; pass < 2 && S.num == 0; pass++) {
			for (int32_t i = 0; i < gr.num; i++) {
				const Unit* u = g.unit(who, gr.list[size_t(i)]);
				if (!u || !u->alive() || !u->on_map()) continue;
				if ((u->ptype->unit_flags2 & (pass == 0 ? 0x40 : 0x20)) != 0) {
					group_add(g, S, u->o, who, 0, 0);
					break;
				}
			}
		}
	}
	int32_t L = -1;
	if (S.num > 0) {
		L = S.list[size_t(std::max(0, group_find_leader(g, S)))];
		if (!human) {
			int32_t best = 0x0fffffff;
			for (int32_t k = 0; k < S.num; k++) {
				const Unit* m = g.unit(who, S.list[size_t(k)]);
				if (!m) continue;
				int32_t s = 0;
				for (int32_t i = 0; i < gr.num; i++) {
					const Unit* u = g.unit(who, gr.list[size_t(i)]);
					if (!u || !u->alive() || !u->on_map()) continue;
					s += std::abs(u->x - m->x) / 1024 + std::abs(u->y - m->y) / 1024;
				}
				if (s >= 0 && s < best) {
					best = s;
					L = m->o;
				}
			}
		}
	}
	if (S.num == 0 || L < 0) {
		move_group_to(g, id, x, y, kQueueNew, true, angle, kAttackTo);
		return;
	}
	const Unit* lu = g.unit(who, L);
	if (!human && lu && g.world.w(wc(x), wc(y)).region != g.world.get_tregion(tcoord_of(lu->x), tcoord_of(lu->y))) {
		move_group_to(g, id, x, y, kQueueNew, true, angle, kAttackTo);
		return;
	}
	const int32_t sid = groups_push_group(g, who, S, 1);
	if (sid < 0) return;
	group_action_move_to(g, GR(g, sid), x, y, kQueueNew, true, angle, kAttackTo, 1, -1, -1, 0);
	// (escort: see the TODO above)
	Group R;
	group_clear(R, -1);
	for (int32_t i = 0; i < GR(g, id).num; i++) {
		const int32_t o = GR(g, id).list[size_t(i)];
		if (!group_member(g, GR(g, sid), o, who)) group_add(g, R, o, who, 0, 0);
	}
	if (R.num > 0) {
		const int32_t rid = groups_push_group(g, who, R, 1);
		if (rid >= 0) {
			int32_t px = x, py = y;
			project(x, y, angle + 0x80000000u, 384, &px, &py);
			group_action_move_to(g, GR(g, rid), px, py, kQueueNew, true, angle, kAttackTo, 1, -1, -1, 0);
		}
	}
	for (int32_t i = 0; i < GR(g, sid).num; i++) set_group_chain(g, who, GR(g, sid).list[size_t(i)], id);
}

int32_t find_aggressive_army(const Game& g, int32_t who) {
	for (int32_t k = 0; k < 16; k++) {
		const Army& A = g.armies[size_t(who)][size_t(k)];
		if (!A.valid || A.num_captains < 2 || (A.status & armystat::kMustering)) continue;
		const int32_t wx = wc(A.x), wy = wc(A.y);
		if (!g.world.in_w(wx, wy) || g.world.w(wx, wy).who != who) return k;
	}
	return -1;
}

int32_t num_armies(const Game& g, int32_t who, int32_t mask, int32_t reg) {
	int32_t n = 0;
	for (const Army& A : g.armies[size_t(who)]) {
		if (A.valid && (A.status & mask) && (reg < 0 || A.reg == reg)) n++;
	}
	return n;
}

int32_t find_local_army(Game& g, int32_t who, int32_t x, int32_t y, int32_t u) {  // @6f32e0
	const int32_t R = tregion_at(g, x, y);
	g.armies_find_dist = 99999999;
	int32_t best = -1;
	const Unit* uu = u >= 0 ? g.unit(who, u) : nullptr;
	for (int32_t k = 0; k < 16; k++) {
		const Army& A = g.armies[size_t(who)][size_t(k)];
		if (!A.valid || A.reg != R) continue;
		if (!(A.target_who < 0 || A.target_who == who)) continue;
		if (!(get_diff(g, who) > 2 || (A.status & armystat::kMustering))) continue;
		if (uu) {
			if ((uu->ptype->unit_flags2 & 0x40) && army_count(g, A, kCountType, kSupplyWagon) >= 3) continue;
			if ((uu->ptype->unit_flags2 & 0x20) && army_count(g, A, kCountType, kGeneral) >= 2) continue;
		}
		const int32_t d = A.num_units == 0 ? 90000000 : vector_dist(x - A.x, y - A.y);
		if (d <= g.armies_find_dist) {
			g.armies_find_dist = d;
			best = k;
		}
	}
	return best;
}

void army_process(Game& g, Army& A, int32_t force);
bool find_muster_spot(Game& g, Army& A, int32_t o, int32_t w, int32_t wide);
void find_target(Game& g, Army& A);

void send_navy(Game& g, int32_t who, int32_t o, int32_t w, int32_t ra, int32_t rb) {  // Armies::send_navy @6f2c90
	const Leader& L = LD(g, who);
	if ((L.flags & 5) != 1 || (L.flags2 & 0xa)) return;
	for (int32_t k = 0; k < 16; k++) {
		Army& A = g.armies[size_t(who)][size_t(k)];
		if (!A.valid || !A.navy || A.num_captains <= 2 || A.reg <= 63) continue;
		if (!g.regions.is_coast(A.reg, ra) || !g.regions.is_coast(A.reg, rb)) continue;
		A.target_o = o;
		A.target_who = w;
		army_process(g, A, 0);
	}
}

// ---------------------------------------------------------------- find_muster_spot (12.5.2)

bool find_muster_spot(Game& g, Army& A, int32_t o, int32_t w, int32_t wide) {
	const int32_t me = A.who;
	const Leader& Lme = LD(g, me);
	A.hurry = 0;
	bool ret = false;
	int32_t best = 0, bi = 999999, bx = 0, by = 0;
	bool found = false;
	const Object* T = g.object(w, o);
	if (!T) return false;
	const int32_t ys = g.world.ys, xs = g.world.xs;
	if ((w == me || mutual(g, me, w)) && A.navy == 0) {
		A.muster_x = A.muster_y = -1;
		for (int32_t b = o; b >= 0;) {
			const Build* bb = g.build(w, b);
			if (!bb) break;
			const int32_t next = bb->city_down;
			if (bb->damage != 0 && (bb->build_masks & bmask::kUnderAttack)) {
				int32_t e = objects_find_unit(g, bb->x, bb->y, search::kEnemy, me, 0x900, 0x200, filter::kCombat, 0, 0,
				                              filter::kDomain, 0, 0);
				if (e < 0)
					e = objects_find_unit(g, bb->x, bb->y, search::kEnemy, me, 0x1200, 0x200, filter::kTarget, b, w,
					                      filter::kDomain, 0, 0);
				if (e < 0 && army_count(g, A, kCountSiege, 0) != 0)
					e = objects_find_building(g, bb->x, bb->y, search::kEnemy, me, 0x1200, 0x200, filter::kCombat, 0, 0);
				if (e >= 0 && g.find_who >= 0) {
					const Object* E = g.object(g.find_who, e);
					if (E) {
						A.muster_x = wc(E->x);
						A.muster_y = wc(E->y);
						A.muster_angle = find_angle(A.muster_x - wc(T->x), A.muster_y - wc(T->y));
						A.hurry = 1;
						return true;
					}
				}
				if (A.muster_x < 0 || A.muster_y < 0) {
					A.muster_x = wc(bb->x);
					A.muster_y = wc(bb->y);
				}
			}
			b = next;
		}
		if (A.muster_x < 0 || A.muster_y < 0) {
			A.muster_x = wc(T->x);
			A.muster_y = wc(T->y);
		}
		A.muster_y += A.muster_y < ys - 1 ? 1 : -1;
		A.muster_angle = find_angle(A.muster_x - wc(T->x), A.muster_y - wc(T->y));
	}
	const int32_t cx = wc(A.x), cy = wc(A.y);
	const City* TC = nullptr;
	const Build* TB = T->is_unit ? nullptr : static_cast<const Build*>(T);
	if (TB && TB->city >= 0 && size_t(TB->city) < g.cities[size_t(w)].size()) TC = &g.cities[size_t(w)][size_t(TB->city)];
	int32_t r0;
	if (!is_city_obj(T) || A.navy) r0 = 5;
	else if (TB && is_unassimilated(g, *TB)) r0 = 4;
	else {
		r0 = (TC ? city_get_radius(g, w, TB->city) : 4) / 4 + 1;
		if (army_count(g, A, kCountSiege, 0) == 0 && me != w) r0 /= 2;
	}
	const int32_t kmax = wide ? 49 : 9;
	const int32_t r1 = std::min(64, r0 + 2 * (3 * A.navy + 1));
	const Circles& CI = circles();
	r0 = std::clamp(r0, 0, 64);
	for (int32_t i = CI.radius[size_t(r0)]; i < CI.radius[size_t(r1)]; i++) {
		const int32_t c_x = cx + CI.x[size_t(i)], c_y = cy + CI.y[size_t(i)];
		bool done_cell = false;
		if (!g.world.in_w(c_x, c_y)) continue;
		int32_t score = 0;
		for (int32_t k = 0; k < 16 && !done_cell; k++) {
			if (k == A.army) continue;
			const Army& B = g.armies[size_t(me)][size_t(k)];
			if (!B.valid) continue;
			const int32_t lim = (me == w || mutual(g, w, me)) ? 4 : 2;
			if (vector_dist(c_x - B.muster_x, c_y - B.muster_y) <= lim) done_cell = true;
		}
		if (!done_cell) {
			for (int32_t k = 0; k < kmax; k++) {
				const int32_t nx = c_x + move_x[k], ny = c_y + move_y[k];
				if (!g.world.in_w(nx, ny)) {
					ret = false;
					done_cell = true;
					break;
				}
				if (A.navy == 0 && g.world.in_w(c_x + 2 * move_x[k], c_y + 2 * move_y[k])) {
					const int32_t q = g.world.w(c_x + 2 * move_x[k], c_y + 2 * move_y[k]).who;
					if (q != -1 && q != me && !mutual(g, me, q) && q != w) {
						ret = false;
						done_cell = true;
						break;
					}
				}
				if (k >= (A.navy ? 25 : 9)) continue;
				const WData& d = g.world.w(nx, ny);
				const int32_t R = d.region;
				if (R != A.reg && (A.navy || !(Lme.flags & 0x700) || R > 64)) {
					ret = false;
					done_cell = true;
					break;
				}
				const uint16_t f = d.flags;
				int32_t t;
				if (f & wflag::kCoast) t = 3;
				else if (f & wflag::kForest) t = 4;
				else if (f & (wflag::kMountain | wflag::kCliff)) t = 5;
				else if (f & wflag::kRock) t = (f & wflag::kOil) ? 7 : 6;
				else if (World::is_ocean(d) && (f & wflag::kOil)) t = 7;
				else t = d.land;
				const bool water = !(f & wflag::kHalfLand) && (d.land == 1 || d.land == 2);
				if (A.navy == 0) {
					if (water || t == 3 || t == 4 || t == 5) {
						ret = false;
						done_cell = true;
						break;
					}
				} else if (!water) {
					ret = false;
					done_cell = true;
					break;
				}
				if (f & wflag::kBuilding) {
					if (A.navy && k >= 9) score += 10;
				} else {
					// Land record +0x100: LandData.move_rate (TODO(ai-spec 12.11.6): confirm the field). Taken as 1.
					score += 1;
				}
				ret = true;
			}
			if (!done_cell && ret && score > best) {
				best = score;
				bx = c_x;
				by = c_y;
				bi = i;
				found = true;
			}
		}
		if (i - bi > 40) {
			ret = true;
			break;
		}
	}
	if (found) {
		A.muster_x = bx;
		A.muster_y = by;
		A.muster_angle = find_angle(bx - cx, by - cy);
		if (is_city_obj(T) && me == w && TC && TC->valid())
			g.cities[size_t(w)][size_t(TB->city)].city_flags &= uint16_t(~cityflag::kCrowded);
		return ret;
	}
	if (A.navy == 0 && is_city_obj(T) && (!mutual(g, me, w) || T->damage != 0) && TC) {
		A.muster_x = wc(TC->x);
		A.muster_y = wc(TC->y) + (wc(TC->y) < ys - 1 ? 1 : -1);
		A.muster_angle = find_angle(A.muster_x - cx, A.muster_y - cy);
		return true;
	}
	if (is_city_obj(T) && me == w && TC && (TC->city_flags & 3) == 1)
		g.cities[size_t(w)][size_t(TB->city)].city_flags |= cityflag::kCrowded;
	(void)xs;
	return ret;
}

// ---------------------------------------------------------------- find_target (12.5.1)

void find_target(Game& g, Army& A) {
	const int32_t me = A.who;
	Leader& Lme = LD(g, me);
	const int32_t wagons = army_count(g, A, kCountNonDecoyType, kSupplyWagon);
	if (Lme.flags & 8) return;
	const int32_t siege_n = army_count(g, A, kCountSiege, 0);
	const bool siege_ok = A.navy != 0 ||
	                      (2 * siege_n + army_count(g, A, kCountNonDecoyType, kCataphract) +
	                               army_count(g, A, kCountNonDecoyType, kHoplites) >=
	                           4 &&
	                       (age_mil(g, me) < 3 || type_avail(g, me, kSupplyWagon, 1) == 0 || wagons != 0));
	int32_t cc = -1, cw = -1;
	find_capital(g, me, &cc, &cw, -1, -1);
	const bool have_cap = cc >= 0 && cw == me;
	int32_t capx = 0, capy = 0;
	if (have_cap) {
		capx = wc(g.cities[size_t(me)][size_t(cc)].x);
		capy = wc(g.cities[size_t(me)][size_t(cc)].y);
	}
	int32_t fsum = 0, fn = 0, esum = 0, en = 0;
	for (int32_t p = 0; p < 8; p++) {
		if (!(LD(g, p).flags & leaderflag::kValid)) continue;
		if (p == me || mutual(g, me, p)) {
			fsum += LD(g, p).sea_combat;
			fn++;
		} else if (war(g, me, p)) {
			esum += LD(g, p).sea_combat;
			en++;
		}
	}
	const int32_t friendly = fn ? fsum / fn : 0, enemy = en ? esum / en : 0;
	int32_t best = -99999999, best_o = -1, best_who = -1;
	const int32_t d = get_diff(g, me);
	for (int32_t pass = 0; pass < 2; pass++) {
		for (int32_t j = 0; j < 8; j++) {
			const Leader& J = LD(g, j);
			if (!(J.flags & leaderflag::kActive)) continue;
			bool win = false;
			if (j != me) {
				if (war(g, me, j)) {
					if (Lme.defense_mod > 0x100) continue;
					win = Lme.wonderwin_timer != 0 || Lme.popwin_timer != 0;
				} else if (mutual(g, me, j)) {
					if (Lme.weak[size_t(j)] || !Lme.strong[size_t(j)] || is_tribute_period(g, me, j) ||
					    Lme.defense_mod < 0x100)
						continue;
				} else {
					continue;
				}
				if (d <= 1) {
					if (d == 0 && !(J.flags & leaderflag::kHuman)) continue;
					if (J.frame_attacked + 7200 > g.frame) continue;
					const int32_t a = find_aggressive_army(g, me);
					if (a >= 0) {
						if (a != A.army) continue;
					} else if (rnd_raw(g) & 1) {  // F1
						continue;
					}
				}
				if (d == 1) {
					if (A.num_captains >= Lme.combat / 2) continue;
					const int32_t a = find_aggressive_army(g, me);
					if (a >= 0 && a != A.army) continue;
				}
				if (d >= 2 && !mutual(g, me, j)) {
					if ((age_mil(g, me) > 2 || Lme.pers.early_army == 0 || get_diff(g, me) == 2) &&
					    A.num_standard < 7 - 2 * Lme.pers.raid)
						continue;
				}
				if (d == 2) {
					const int32_t fa = J.frame_attacked;
					if (fa + 1800 > g.frame) continue;
					if (g.info.team_style != 3 && fa + 3600 > g.frame && J.attacked_by != me && Lme.attacked_by != j) continue;
				}
			}
			if (g.info.team_style == 2) {
				const int32_t t = get_target(g, me);
				if (t != j && j != me && !mutual(g, me, j)) continue;
			}
			const auto& cl = g.cities[size_t(j)];
			for (int32_t i = 0; i < J.city_mark && size_t(i) < cl.size(); i++) {
				const City& C = cl[size_t(i)];
				if (!C.valid()) continue;
				if (A.navy && !(g.regions.is_coast(A.reg, C.reg) && C.ocean)) continue;
				if (d <= 1 && mutual(g, me, j) && C.founder != me) continue;
				int32_t S = 900 + rnd_raw(g) % 200;  // F2
				const Build* B = g.build(j, C.o);
				if (have_cap && war(g, me, j)) {
					if (!siege_ok) {
						if (C.founder != me) continue;
						if (!B || !is_unassimilated(g, *B)) continue;
					}
					S += (vector_dist(wc(C.x) - capx, wc(C.y) - capy) * -50) / g.world.xs;
				}
				if (Lme.pop_issues && war(g, me, j)) S <<= 2;
				if (J.wonderwin_timer && city_num_wonders(g, j, i, 0) && wonder_winning(g) == j) S *= 10;
				if (war(g, me, j)) {
					if (g.info.team_style == 2 || g.info.team_style == 3) S *= 5;
					if (A.navy) S *= 10;
					const int32_t t = ctype(g, j, C), raid = Lme.pers.raid;
					if (raid == -1) S *= t;
					else if (raid == 1) {
						if (A.num_captains >= 20 && siege_n >= 3 && Lme.score > 4 * J.score / 3) S *= city_level(g, j, i);
						else S *= 4 - t;
					} else {
						if (A.num_captains >= 20 && siege_n >= 2) S *= city_level(g, j, i);
						else if (A.num_captains >= 10 && siege_n != 0) {
						} else S *= 4 - t;
					}
				} else {
					if (C.city_flags & cityflag::kUnderAttack) {
						if (g.info.team_style == 2) S *= 5;
					} else {
						if (A.num_captains >= 20 && siege_n >= 2) S = (4 - city_level(g, j, i)) * S / 4;
						else if (A.num_captains >= 10 && siege_n != 0) {
						} else S *= ctype(g, j, C);
						if (C.city_flags & cityflag::kCrowded) S /= 20;
					}
				}
				if (C.reg != A.reg && A.navy == 0) {
					S /= 2;
					if (!(Lme.flags & 0x700)) continue;
					if (friendly < enemy || friendly < 5) {
						S /= 2;
						if (friendly < enemy - 2 || friendly == 0) S /= 5;
					}
				}
				if ((pass == 0 || get_diff(g, me) >= 3) && mutual(g, me, j) && !(C.city_flags & cityflag::kUnderAttack)) S /= 3;
				if (mutual(g, me, j) && (C.city_flags & cityflag::kUnderAttack)) {
					if (j == me) S *= 10;
					if (C.city_flags & cityflag::kCapital) S *= 10;
				} else if (war(g, me, j) && (C.city_flags & cityflag::kCapital)) {
					if (has_preq(g, me, ptype::kGlobalGovernmentBonus)) S <<= 2;
					else if (Lme.pers.raid < 0) S = 3 * S / 2;
				}
				if (C.bordering & (1 << me)) {
					S *= Lme.pers.raid <= 0 ? 4 : 2;
					if (war(g, me, j) && get_diff(g, me) >= 3) {
						if (J.frame_attacked + 9000 < g.frame) S *= 3;
						if (C.attack_stamp + 9000 < g.frame) S *= 3;
					}
				}
				if (j != me && mutual(g, me, j)) {
					S = 3 * S / 4;
					if (!(C.city_flags & cityflag::kUnderAttack)) S /= 2;
				}
				if (win) S *= 100;
				if (j == me) {
					if (B && B->damage) S *= 2;
				} else if (B && B->damage) {
					S = (C.bordering & (1 << me)) ? 3 * S / 2 : 5 * S / 4;
				}
				if (C.was_capital_flags & (1 << me)) {
					S = 1000000;
				} else if (war(g, me, j) && A.navy == 0) {
					if (age_mil(g, me) > 2 && get_diff(g, me) >= 3 && !(B && is_unassimilated(g, *B))) {
						if (siege_n == 0 && age_mil(g, me) >= 4) S /= 5;
						if (wagons < 2) {
							if (age_mil(g, me) > 4) S /= 5;
							S /= 5;
							if (wagons < 1) S /= 5;
						}
					}
					if (J.flags & leaderflag::kHuman) {
						if (get_diff(g, me) == 4) S = 5 * S / 4;
						else if (get_diff(g, me) == 5) S = 4 * S / 3;
					}
				}
				for (int32_t k = 0; k < 16; k++) {
					const Army& B2 = g.armies[size_t(me)][size_t(k)];
					if (!B2.valid || (B2.status & armystat::kMustering) || B2.target_o != C.o || B2.target_who != j) continue;
					if (k == A.army) {
						S *= 2;
						if (war(g, me, j) || (C.city_flags & cityflag::kUnderAttack)) S <<= 2;
						if (j == me && B && is_unassimilated(g, *B)) S <<= 3;
						if (j == me && B && 2 * B->damage > army_hits(g, *B)) S <<= 3;
					} else if (C.city_flags & cityflag::kUnderAttack) {
						if (mutual(g, me, j) || get_diff(g, me) >= 3 || A.navy) S <<= 2;
						else S /= 4;
					} else {
						S /= 4;
					}
				}
				if (S < 0) S = 1000000;
				if (S >= best) {
					best = S;
					best_who = j;
					best_o = C.o;
				}
			}
			if (siege_n >= 2 && wagons != 0 && war(g, me, j)) {
				const auto& F = g.forts[size_t(j)];
				for (int32_t f = 0; f < J.fort_mark && size_t(f) < F.size(); f++) {
					if (!(F[size_t(f)].fort_flags & 1) || F[size_t(f)].reg != A.reg) continue;
					const Build* fb = g.build(j, F[size_t(f)].o);
					if (!fb) continue;
					int32_t S = (900 + rnd_raw(g) % 200) * (J.age + 1);  // F3
					if (fb->city < 0) S /= 2;
					for (int32_t k = 0; k < 16; k++) {
						const Army& B2 = g.armies[size_t(me)][size_t(k)];
						if (!B2.valid || (B2.status & armystat::kMustering) || B2.target_o != F[size_t(f)].o || B2.target_who != j)
							continue;
						if (k == A.army) S *= 2;
						else S = (fb->build_masks & bmask::kUnderAttack) ? S << 2 : S / 4;
					}
					S = S / (vector_dist(A.x - fb->x, A.y - fb->y) / 768 + 1);
					if (S >= best) {
						best = S;
						best_o = F[size_t(f)].o;
						best_who = j;
					}
				}
			}
		}
		if (best_o >= 0) break;
	}
	if (best_o < 0) return;
	if (best_who != me) A.hurry = 0;
	const Object* T = g.object(best_who, best_o);
	if (!T) return;
	if (get_diff(g, me) == 0 && (LD(g, best_who).flags & leaderflag::kHuman)) {
		// EASIEST: a two-unit token raid.
		const int32_t u0 = get_unit(g, A, 0);
		if (u0 < 0) return;
		Group tmp;
		group_clear(tmp, -1);
		tmp.stamp = g.frame;
		group_add(g, tmp, u0, me, 0, 0);
		const int32_t u1 = get_unit(g, A, A.num_units - 1);
		if (u1 >= 0) group_add(g, tmp, u1, me, 0, 0);
		if (tmp.num <= 0) return;
		const int32_t li = tmp.buildings ? 0 : group_find_leader(g, tmp);
		if (li < 0) return;
		const Unit* Lu = g.unit(me, tmp.list[size_t(li)]);
		const int32_t gid = groups_push_group(g, me, tmp, 1);
		if (gid < 0 || !Lu) return;
		if (LD(g, best_who).combat == 0) {
			const int32_t b = objects_find_building(g, T->x, T->y, search::kFriendly, best_who, -1, 0x200, filter::kType,
			                                        ptype::kFarm, 0);
			if (b >= 0) {
				Build* fb = g.build(best_who, b);
				(void)fb;  // TODO(ai-spec 12.11.8): the per-player byte +0x62 of the farm (ever_seen?) gets bit(me).
				group_action_stance(g, GR(g, gid), 5);
				group_action_attack(g, GR(g, gid), b, best_who, 1, kQueueNew, 0);
				group_action_move_to(g, GR(g, gid), Lu->x, Lu->y, kQueueLast, false, 0, kMoveTo, 0, -1, -1, 0);
			}
		} else {
			group_action_move_to(g, GR(g, gid), T->x, T->y, kQueueNew, false, 0, kAttackTo, 0, -1, -1, 0);
		}
		army_normalize(g, A);
		LD(g, best_who).frame_attacked = g.frame;
		LD(g, best_who).attacked_by = me;
		return;
	}
	if (A.target_o != best_o || A.target_who != best_who) A.rally_dist = 0x1200;
	A.target_o = best_o;
	A.target_who = best_who;
	if (war(g, me, best_who)) {
		LD(g, best_who).frame_attacked = g.frame;
		LD(g, best_who).attacked_by = me;
		if (A.navy == 0) {
			const int32_t r1 = g.world.w(wc(A.x), wc(A.y)).region, r2 = g.world.w(wc(T->x), wc(T->y)).region;
			if (r1 != r2) send_navy(g, me, best_o, best_who, r1, r2);
		}
	}
	A.x = T->x;
	A.y = T->y + 0x300;
	clamp_map(g, &A.x, &A.y);
	if (!find_muster_spot(g, A, best_o, best_who, 0)) {
		army_close(g, A);
		return;
	}
	A.muster_angle += 0x80000000u;
}

// ---------------------------------------------------------------- states (12.4)

bool release_mustering(Game& g, Army& A) {  // @6f87c0
	const int32_t me = A.who;
	const Leader& Lme = LD(g, me);
	const int32_t n = A.num_standard;
	if (A.city < 0 || size_t(A.city) >= g.cities[size_t(me)].size()) return true;
	const City& C = g.cities[size_t(me)][size_t(A.city)];
	if (C.who != me || !C.valid()) return true;
	if (g.info.rush_rules && !war_allowed(g)) return false;
	if (Lme.flags & 8) return false;
	if (n < 2) return false;
	for (int32_t p = 0; p < 8; p++) {
		if (!(LD(g, p).flags & leaderflag::kValid) || !is_ally(g, me, p)) continue;
		const auto& cl = g.cities[size_t(p)];
		for (int32_t i = 0; i < LD(g, p).city_mark && size_t(i) < cl.size(); i++) {
			if ((cl[size_t(i)].city_flags & 3) == 3) {
				if (p == me) return true;
				if (n > 4) return true;
			}
		}
	}
	if (n < Lme.city_num) return false;
	if (n < 5) return false;
	if (A.navy) return true;
	const int32_t cap = Lme.pop_cap;
	if (Lme.effective_pop >= 7 * cap / 8) return true;
	if (n >= cap / 8) return true;
	const int32_t na = std::max(1, num_armies(g, me, armystat::kMustering, A.reg));
	if (n >= cap / (2 * na)) return true;
	const int32_t d = get_diff(g, me);
	if (d == 0 && n > 6) return true;
	if (d == 1 && n > 10) return true;
	if (d == 2 && n > 12) return true;
	if (Lme.pers.rush == -1) return n > 32;
	if (Lme.pers.rush == 1) {
		if (age_mil(g, me) < 2) return true;
		if (age_mil(g, me) < 3) return n > 15;
		if (age_mil(g, me) < 5) return n > 20;
	} else if (Lme.pers.early_army != 0) {
		if (age_mil(g, me) < 2) return true;
		if (age_mil(g, me) < 3) return n > 15;
	}
	return n > 25;
}

void do_mustering(Game& g, Army& A) {  // @6f4260
	const Leader& Lme = LD(g, A.who);
	const bool r = release_mustering(g, A);
	if (!r) {
		if (A.city >= 0 && size_t(A.city) < g.cities[size_t(A.who)].size()) {
			const City& C = g.cities[size_t(A.who)][size_t(A.city)];
			if (C.valid() && find_muster_spot(g, A, C.o, A.who, 1)) {
				A.status |= armystat::kFormup;
				return;
			}
		}
		if ((Lme.strategy[size_t(A.reg & 63)] & 8) && A.num_captains > 7 && (Lme.flags & 0x300))
			A.status = armystat::kNeedTransport;
		else
			A.status = armystat::kMarching;
	} else if (A.navy == 0) {
		const int32_t s = Lme.strategy[size_t(A.reg & 63)];
		if ((s & 4) && get_diff(g, A.who) < 3) A.status = armystat::kDefending;
		else if ((s & 8) && (Lme.flags & 0x300)) A.status = armystat::kNeedTransport;
		else A.status = armystat::kMarching;
	} else {
		A.status = armystat::kMarching;
	}
	A.city = -1;
	A.x = centre(A.muster_x);
	A.y = centre(A.muster_y);
	A.angle = int32_t(A.muster_angle);
}

bool find_besieged_city(Game& g, Army& A) {  // @6f58b0 (city_mark bound of the army owner: original bug)
	const int32_t me = A.who;
	const Leader& Lme = LD(g, me);
	int32_t best = -99999999, bp = -1, bc = -1;
	for (int32_t p = 0; p < 8; p++) {
		if (!(LD(g, p).flags & leaderflag::kValid) || !(p == me || mutual(g, me, p))) continue;
		const auto& cl = g.cities[size_t(p)];
		for (int32_t i = 0; i < Lme.city_mark; i++) {
			if (size_t(i) >= cl.size()) break;  // TODO(ai-spec 12.11.9): the original reads past p's list
			const City& C = cl[size_t(i)];
			if (!C.valid()) continue;
			if (A.navy == 0) {
				if (C.reg != A.reg) continue;
			} else if (!g.regions.is_coast(A.reg, C.reg)) {
				continue;
			}
			if (!(C.city_flags & cityflag::kUnderAttack)) continue;
			int32_t s = 1000 * ctype(g, p, C);
			if (A.navy && objects_find_unit(g, C.x, C.y, search::kEnemy, me, 0x1200, 0, filter::kDomain, 1, 0, filter::kAll,
			                                0, 0) < 0)
				s >>= 2;
			if (p == me) s *= 10;
			for (int32_t k = 0; k < 16; k++) {
				const Army& B = g.armies[size_t(p)][size_t(k)];
				if (B.valid && (k != A.army || p != me) && B.city == i && B.num_captains > 4) s >>= 2;
			}
			s = s / (vector_dist(A.x - C.x, A.y - C.y) / 768 + 1);
			if (s >= best) {
				best = s;
				bp = p;
				bc = i;
			}
		}
	}
	if (bc < 0) return false;
	const City& C = g.cities[size_t(bp)][size_t(bc)];
	if (A.navy == 0) {
		A.muster_angle = find_angle(C.x - centre(A.muster_x), C.y - centre(A.muster_y));
		A.muster_x = wc(A.x);
		A.muster_y = wc(A.y);
		A.x = C.x;
		A.y = C.y + 0x300;
		clamp_map(g, &A.x, &A.y);
		A.target_o = C.o;
		A.target_who = bp;
		return true;
	}
	find_muster_spot(g, A, C.o, bp, 0);
	A.x = centre(A.muster_x);
	A.y = centre(A.muster_y);
	return true;
}

void do_defending(Game& g, Army& A) {  // @6f4070
	if (army_count(g, A, kCountAttack, 0) < 5) {
		army_close(g, A);
		return;
	}
	if (is_engaged(g, A)) return;
	const int32_t old = A.status;
	A.status &= ~armystat::kOffense;
	if ((old & armystat::kMarching) && A.target_o >= 0 && A.target_who >= 0) {
		const Object* T = g.object(A.target_who, A.target_o);
		if (T && T->alive()) {
			const Build* B = T->is_unit ? nullptr : static_cast<const Build*>(T);
			if (is_city_obj(T) && B && B->city >= 0 && (g.cities[size_t(A.target_who)][size_t(B->city)].city_flags & 3) == 3)
				return;
			A.target_o = A.target_who = -1;
			A.muster_x = wc(A.x);
			A.muster_y = wc(A.y);
			A.status = (A.status & ~0x22) | armystat::kFormup;
			return;
		}
	}
	if (find_besieged_city(g, A)) {
		A.status |= armystat::kMarching;
		return;
	}
	const int32_t c = objects_find_city(g, A.x, A.y, search::kFriendly, A.who, 0x200, filter::kAll, 0, 0);
	A.target_o = A.target_who = -1;
	A.city = c;
	if (c >= 0) {
		const City& C = g.cities[size_t(A.who)][size_t(c)];
		A.status &= ~0x22;
		A.x = C.x;
		A.y = C.y + 0x300;
		clamp_map(g, &A.x, &A.y);  // WorldData::restrict
		if (find_muster_spot(g, A, C.o, A.who, 1)) A.status |= armystat::kFormup;
		else army_close(g, A);
		return;
	}
	A.muster_x = wc(A.x);
	A.muster_y = wc(A.y);
	A.status = (A.status & ~0x22) | armystat::kFormup;
}

bool siege_needed_plain(Game& g, Army& A, int32_t px, int32_t py, bool forming) {
	const int32_t me = A.who, tw = A.target_who;
	const bool siege = objects_find_building(g, px, py, search::kEnemy, me, 0x300, 0x200, filter::kCombat, 0, 0) >= 0 &&
	                   army_count(g, A, kCountSiege, 0) != 0;
	if (siege) return false;
	if (A.hurry != 0) return true;
	if (forming && tw != me && !is_ally(g, me, tw)) return false;
	if (!forming && !(tw == me || is_ally(g, me, tw))) return false;
	if (A.target_o < 0 || tw < 0) return false;
	const Object* T = g.object(tw, A.target_o);
	const Build* B = T && !T->is_unit ? static_cast<const Build*>(T) : nullptr;
	return B && B->city >= 0 && (g.cities[size_t(tw)][size_t(B->city)].city_flags & 3) == 3;
}

bool do_forming(Game& g, Army& A) {  // @6f43c0
	if (is_engaged(g, A)) return false;
	const int32_t me = A.who;
	int32_t px, py;
	project(centre(A.muster_x), centre(A.muster_y), A.muster_angle, 192 * A.num_groups, &px, &py);
	for (int32_t i = 0; i < A.num_groups; i++) {
		const int32_t id = A.list[size_t(i)];
		if (id < 0 || GR(g, id).num == 0) continue;
		GR(g, id).army = A.army;
		if (A.target_who >= 0 && war(g, me, A.target_who) && A.hurry == 0 && age_mil(g, me) <= 3 &&
		    army_count(g, A, kCountSiege, 0) == 0)
			set_stance(g, A, 3);
		if (siege_needed_plain(g, A, px, py, true)) {
			set_stance(g, A, 0);
			move_group_to(g, id, px, py, kQueueNew, true, A.muster_angle, kAttackTo);
		} else {
			siege_attack_to(g, id, px, py, A.muster_angle);
		}
		project(px, py, A.muster_angle + 0x80000000u, 384, &px, &py);
	}
	return true;
}

void march_to_target(Game& g, Army& A) {  // @6f4d80
	if (A.target_o < 0) return;
	if (!is_engaged(g, A)) {
		A.status = (A.status & ~armystat::kOffense) | armystat::kMarching | armystat::kFormup;
		return;
	}
	if (A.status & armystat::kOffense) return;
	const int32_t me = A.who, tw = A.target_who;
	const Object* T = g.object(tw, A.target_o);
	if (!T) return;
	int32_t px, py;
	project(A.x, A.y, A.muster_angle + 0x80000000u, 768, &px, &py);
	const uint32_t ang = find_angle(px - T->x, py - T->y);
	if (tw >= 0 && war(g, me, tw) && A.hurry == 0 && age_mil(g, me) <= 3 && army_count(g, A, kCountSiege, 0) == 0)
		set_stance(g, A, 3);
	const bool plain = siege_needed_plain(g, A, px, py, false);
	for (int32_t i = 0; i < A.num_groups; i++) {
		const int32_t id = A.list[size_t(i)];
		if (id < 0 || GR(g, id).num == 0) continue;
		GR(g, id).army = A.army;
		if (is_city_obj(T) && T->damage >= 9 * army_hits(g, *T) / 10) move_group_to(g, id, T->x, T->y, kQueueNew, true, ang, kAttackTo);
		else if (plain) move_group_to(g, id, px, py, kQueueNew, true, ang, kAttackTo);
		else siege_attack_to(g, id, px, py, ang);
		project(px, py, A.muster_angle + 0x80000000u, 384, &px, &py);
	}
	A.status |= armystat::kOffense;
}

void do_marching(Game& g, Army& A) {  // @6f3df0
	const int32_t me = A.who;
	const Leader& Lme = LD(g, me);
	if (army_count(g, A, kCountAttack, 0) < 1) {
		army_close(g, A);
		return;
	}
	const int32_t o = A.target_o, w = A.target_who;
	bool retarget = o < 0;
	if (!retarget) {
		const Object* T = g.object(w, o);
		const Build* B = T && !T->is_unit ? static_cast<const Build*>(T) : nullptr;
		if (B && B->alive()) {
			if (!is_city_obj(B)) {
				if (!war(g, me, w) && !(B->build_masks & bmask::kUnderAttack)) retarget = true;
			} else if (B->city >= 0) {
				const City& C = g.cities[size_t(w)][size_t(B->city)];
				const uint16_t f = C.city_flags;
				if (!(f & cityflag::kValid)) retarget = true;
				else if (A.navy && !war(g, me, w) && !(f & cityflag::kUnderAttack)) retarget = true;
				else if (!war(g, me, w) && !(f & cityflag::kUnderAttack) && !is_unassimilated(g, *B)) {
					if (B->damage < 3 * army_hits(g, *B) / 4 &&
					    (is_ally(g, me, w) || Lme.weak[size_t(w)] || !Lme.strong[size_t(w)] || is_tribute_period(g, me, w)))
						retarget = true;
				}
			}
		}
	}
	if (!retarget) {
		if (A.status & armystat::kMarching) march_to_target(g, A);
		return;
	}
	A.target_o = A.target_who = -1;
	if (A.status & armystat::kDefending) {
		A.status = armystat::kDefending | armystat::kFormup;
		return;
	}
	find_target(g, A);
	if (!A.valid) return;
	if (A.target_o >= 0) {
		A.status |= armystat::kFormup;
		do_forming(g, A);
	} else {
		A.status = armystat::kIdle;
	}
}

void engagement(Game& g, Army& A) {  // @6f5160
	int32_t o = -1, w = -1, cx = 0, cy = 0;
	bool cog = false;
	for (int32_t i = 0; i < A.num_units; i++) {
		const int32_t uo = get_unit(g, A, i);
		Unit* u = uo >= 0 ? g.unit(A.who, uo) : nullptr;
		if (!u || !u->alive() || !u->on_map()) continue;
		const Order* a = get_action(*u);
		if (!a || a->type != kAttack) continue;
		if (vector_dist(u->x - A.x, u->y - A.y) >= 0xc00) continue;
		if (!cog) {
			center_of_gravity(g, A, &cx, &cy);
			cog = true;
		}
		if (vector_dist(centre(cx) - u->x, centre(cy) - u->y) > 0xc00) continue;
		const Order* ua = update_action(g, *u);
		if (!ua) continue;
		o = ua->ox;
		w = ua->whom;
		if (o >= 0 && w >= 0) {
			const Object* t = g.object(w, o);
			if (t && t->is_unit && t->alive() && static_cast<const Unit*>(t)->on_map()) break;
		}
	}
	if (o < 0 || w < 0) return;
	if (A.target_who >= 0 && war(g, A.who, A.target_who) && A.hurry == 0 && age_mil(g, A.who) < 4 &&
	    army_count(g, A, kCountSiege, 0) == 0)
		set_stance(g, A, 3);
	for (int32_t i = 0; i < A.num_groups; i++) {
		const int32_t id = A.list[size_t(i)];
		if (id < 0) continue;
		bool any = false;
		for (int32_t k = 0; k < GR(g, id).num; k++) {
			const Object* m = g.object(A.who, GR(g, id).list[size_t(k)]);
			if (m && m->alive()) any = true;
		}
		if (any) group_action_attack(g, GR(g, id), o, w, 0, kQueueNew, 0);
	}
}

// Region::go_here @6810f0.
int32_t region_go_here(const Game& g, int32_t r, int32_t who) {
	const Region& R = g.regions.r[size_t(r)];
	const Leader& L = LD(g, who);
	int32_t v = 0;
	if ((R.flags & 8) && L.reg_cities[size_t(r & 63)] == 0 && L.reg_peasants[size_t(r & 63)] == 0) {
		bool crowded = false;
		for (int32_t p = 0; p < 8; p++) {
			if (p == who || !(LD(g, p).flags & leaderflag::kActive)) continue;
			const int32_t c = uint16_t(LD(g, p).reg_cities[size_t(r & 63)]);
			if (c != 0 && R.size < 200 * c) crowded = true;
		}
		if (!crowded) v |= 1;
	}
	for (int32_t p = 0; p < 8; p++) {
		if (p == who || !(LD(g, p).flags & leaderflag::kActive) || !war(g, who, p)) continue;
		if (LD(g, p).reg_cities[size_t(r & 63)] == 0) continue;
		v |= (L.reg_cities[size_t(r & 63)] == 0 || (L.strategy[size_t(r & 63)] & 4)) ? 2 : 4;
	}
	return v;
}

bool do_transporting(Game& g, Army& A) {  // @6f4690
	const Leader& Lme = LD(g, A.who);
	if (g.world.w(wc(A.x), wc(A.y)).region != A.reg) return false;
	int32_t best = -1, br = -1, bc = -1;
	for (int32_t r = 1; r < 64; r++) {
		if (r == A.reg || g.regions.r[size_t(r)].size <= 0) continue;
		const int32_t s = Lme.strategy[size_t(r)], gh = region_go_here(g, r, A.who);
		if (!((s & 2) || (gh & 2) || (gh & 4))) continue;
		int32_t v = g.regions.r[size_t(r)].size * ((gh & 4) ? 3 : (s & 2) ? 2 : 1);
		const auto& ca = g.regions.r[size_t(A.reg & 127)].coords;
		const auto& cb = g.regions.r[size_t(r)].coords;
		if (!ca.empty() && !cb.empty()) v = v / (vector_dist(ca[0].x - cb[0].x, ca[0].y - cb[0].y) / 4 + 1);
		int32_t c = -1;
		const auto& cl = g.cities[size_t(A.who)];
		for (int32_t i = 0; i < Lme.city_mark && size_t(i) < cl.size(); i++) {
			const City& C = cl[size_t(i)];
			if (C.valid() && C.reg == r && (C.city_flags & cityflag::kUnderAttack)) {
				v *= city_level(g, A.who, i);
				c = i;
			}
		}
		if (v > best) {
			best = v;
			br = r;
			bc = c;
		}
	}
	if (best <= 0) return false;
	A.reg = br;
	if (bc < 0) {
		find_target(g, A);
		if (!A.valid) return true;
		if (A.target_o < 0) {
			A.reg = g.world.w(wc(A.x), wc(A.y)).region;
			return true;
		}
	} else {
		const City& C = g.cities[size_t(A.who)][size_t(bc)];
		A.target_o = C.o;
		A.target_who = A.who;
		A.city = bc;
		if (!find_muster_spot(g, A, C.o, A.who, 1)) {
			A.muster_x = wc(C.x);
			A.muster_y = wc(C.y);
			A.muster_y += A.muster_y >= g.world.ys - 1 ? -1 : 1;
		}
	}
	A.status = armystat::kMarching;
	return true;
}

// use_generals / use_spies / use_scouts @6f4c30 / @6f4af0 / @6f49a0: the first active member of the kind whose
// think_spellcaster returns non-zero ends the walk. TODO(spells): Unit::think_spellcaster (casting is not simulated).
void use_casters(Game& g, Army& A, int32_t kind) {
	(void)g;
	(void)A;
	(void)kind;
}

// ---------------------------------------------------------------- Army::process (12.2)

void army_process(Game& g, Army& A, int32_t force) {
	const int32_t me = A.who;
	Leader& Lme = LD(g, me);
	if (A.human_frame != 0) A.human_frame -= 1;
	if (!force) {
		const int32_t k = 2 * A.army + 4 * A.who;
		if (((g.frame - 30 + k) & 127) == 0) {
			if (Lme.flags & leaderflag::kDefeated) return;
			army_normalize(g, A);
			if (A.num_captains == 0) return;
			if (A.num_standard != 0 && army_count(g, A, kCountNonDecoyType, kGeneral) != 0) use_casters(g, A, 0);
			if (army_count(g, A, kCountNonDecoyType, ptype::kSpy) != 0) use_casters(g, A, 1);
			if (army_count(g, A, kCountNonDecoyType, ptype::kScout) != 0) use_casters(g, A, 2);
		}
		if (((g.frame + k) & 255) != 0) return;
	}
	if (Lme.flags & leaderflag::kDefeated) return;
	army_normalize(g, A);
	if (A.num_standard < 1 && A.valid && !(A.status & armystat::kMustering)) {
		A.city = 0;
		if (Lme.city_num > 0) {
			while (!(g.cities[size_t(me)][size_t(A.city)].city_flags & cityflag::kValid)) {
				A.city += 1;
				if (A.city >= Lme.city_num || size_t(A.city) >= g.cities[size_t(me)].size()) {
					army_close(g, A);
					return;
				}
			}
			const City& C = g.cities[size_t(me)][size_t(A.city)];
			send_here(g, A, C.x, C.y, kMoveTo);
		}
		army_close(g, A);
		return;
	}
	if (A.human_frame != 0) {
		send_here(g, A, A.x, A.y, kAttackTo);
		return;
	}
	if (A.num_standard < (A.num_captains - A.num_decoys) / 2) {
		for (int32_t k = 0; k < 16; k++) {
			Army& B = g.armies[size_t(me)][size_t(k)];
			if (B.valid && B.navy == 0 && B.reg == A.reg && B.num_standard > 4 &&
			    (B.num_captains - B.num_decoys) / 2 <= B.num_standard) {
				for (int32_t i = A.num_groups - 1; i >= 0; i--) {
					const int32_t id = A.list[size_t(i)];
					if (id < 0) continue;
					for (int32_t m = GR(g, id).num - 1; m >= 0; m--) {
						const int32_t o = GR(g, id).list[size_t(m)];
						const Unit* u = g.unit(me, o);
						if (u && u->alive()) add_unit(g, B, o);
					}
				}
				army_close(g, A);
				return;
			}
		}
	}
	if (!is_moving(g, A) && !is_engaged(g, A)) {
		A.status &= ~(armystat::kMarching | armystat::kFormup);
		if (A.target_o >= 0 && A.target_who >= 0) {
			if (!war(g, me, A.target_who) || A.navy) {
				find_muster_spot(g, A, A.target_o, A.target_who, 0);
			} else {
				const Object* T = g.object(A.target_who, A.target_o);
				if (T) {
					A.muster_x = (wc(T->x) + A.muster_x) / 2;
					A.muster_y = (wc(T->y) + A.muster_y) / 2;
				}
			}
			A.status |= armystat::kMarching | armystat::kFormup;
		}
	}
	if ((A.status & 0x18) == A.status) A.status = armystat::kMarching;
	if (A.status & armystat::kMustering) {
		set_stance(g, A, 1);
		do_mustering(g, A);
	}
	if (!A.valid) return;
	if (A.status & armystat::kDefending) {
		set_stance(g, A, 1);
		do_defending(g, A);
	}
	if (!A.valid) return;
	if (A.status & armystat::kMarching) {
		set_stance(g, A, 0);
		do_marching(g, A);
	}
	if (!A.valid) return;
	if (A.status & armystat::kFormup) do_forming(g, A);
	else if (is_engaged(g, A)) engagement(g, A);
	if (A.status & armystat::kNeedTransport) do_transporting(g, A);
	if (A.navy) set_stance(g, A, 3);
}

}  // namespace

// ---------------------------------------------------------------- public API

// Army::stop @6f9180: every listed group (with members) gets Group vslot 0x14 (action_begin); a unit group also resets
// its form (+0x10 = -1) and each live member on the map that is not a plane (helicopters included) loses its orders: MULTIMOVE and
// AUTO_EXPLORE cleared, queue_time 0, close_orders, clear_partial_path, update_action. (ScenarioData::ignore_orders:
// TODO(scripts); transports' is_entering_or_exiting: never, transports are not simulated.)
void army_stop(Game& g, Army& A) {
	for (int32_t i = 0; i < A.num_groups; i++) {
		const int32_t id = A.list[size_t(i)];
		if (id < 0) continue;
		Group& gr = GR(g, id);
		if (gr.num == 0) continue;
		gr.disband = 0;  // action_begin
		if (gr.buildings) continue;
		gr.form = -1;  // (Group +0x10)
		for (int32_t m = 0; m < gr.num; m++) {
			Unit* u = g.unit(gr.who, gr.list[size_t(m)]);
			if (!u || !u->alive() || !u->on_map()) continue;
			if (unit_is_air(*u) && !unit_is_helicopter(*u)) continue;
			u->unit_masks &= ~umask::kMultimove;
			u->queue_time = 0;
			close_orders(g, *u, false);
			clear_partial_path(*u);
			update_action(g, *u);
			u->unit_masks &= ~umask::kAutoExplore;
		}
	}
}

// Armies::leader_defeated @6f2f90: Army::stop for every valid army of the defeated leader.
void armies_leader_defeated(Game& g, int32_t who) {
	if (who < 0 || who >= 8) return;
	for (Army& A : g.armies[size_t(who)])
		if (A.valid) army_stop(g, A);
}

// Armies::update_city @6f2d70 (from Cities::capture_city): armies of computer-run leaders aiming at the old city object
// now aim at the new one; the new owner's own armies get FORCE_PROCESS.
void armies_update_city(Game& g, int32_t old_o, int32_t old_w, int32_t new_o, int32_t new_w) {
	for (int32_t p = 0; p < 8; p++) {
		const Leader& L = LD(g, p);
		if (!(L.flags & leaderflag::kValid) || (L.flags & 0xc) == leaderflag::kHuman) continue;
		for (Army& A : g.armies[size_t(p)]) {
			if (!A.valid || A.target_o != old_o || A.target_who != old_w) continue;
			A.target_o = new_o;
			A.target_who = new_w;
			if (new_w == p) A.status |= armystat::kForceProcess;
		}
	}
}

// City::close's army part (@737550): the owner's MUSTERING armies homed in the city close; every other army aiming
// at the city object forgets its target (target_o = -1).
void armies_city_closed(Game& g, int32_t who, int32_t city, int32_t city_o) {
	for (int32_t p = 0; p < 8; p++) {
		if (!(LD(g, p).flags & leaderflag::kValid)) continue;
		for (Army& A : g.armies[size_t(p)]) {
			if (!A.valid) continue;
			if (p == who && A.city == city && (A.status & armystat::kMustering)) army_close(g, A);
			else if (A.target_o == city_o && A.target_who == who) A.target_o = -1;
		}
	}
}

void armies_init(Game& g) {
	for (int32_t who = 0; who < 8; who++) {
		for (int32_t k = 0; k < 16; k++) {
			Army& A = g.armies[size_t(who)][size_t(k)];
			A = Army();
			A.army = int16_t(k);
			A.who = int16_t(who);
			A.list.fill(-1);
		}
	}
	g.armies_find_dist = 0;
}

int32_t armies_init_army(Game& g, int32_t who, int32_t city) {  // @6f36a0
	auto& AL = g.armies[size_t(who)];
	int32_t k = -1;
	for (int32_t i = 0; i < 16; i++) {
		if (!AL[size_t(i)].valid) {
			k = i;
			break;
		}
	}
	if (k < 0) {  // all valid: the smallest army (ties -> later slot) is re-initialised in place, without close
		int32_t best = 0x7fffffff;
		for (int32_t i = 0; i < 16; i++) {
			if (AL[size_t(i)].num_units <= best) {
				best = AL[size_t(i)].num_units;
				k = i;
			}
		}
	}
	Army& A = AL[size_t(k)];  // Army::init @6f9000
	A.army = int16_t(k);
	A.who = int16_t(who);
	A.city = city;
	if (city >= 0) {
		const City& C = g.cities[size_t(who)][size_t(city)];
		A.reg = C.reg;
		A.x = C.x;
		A.y = C.y + 0x300;
		A.muster_x = wc(A.x);
		A.muster_y = wc(A.y);
	} else {
		A.reg = A.x = A.y = A.muster_x = A.muster_y = -1;
	}
	A.num_groups = 0;
	A.num_units = A.num_captains = A.num_standard = A.num_decoys = 0;
	A.human_frame = A.role = 0;
	A.valid = 1;
	A.status = armystat::kMustering;
	A.angle = 0;
	A.muster_angle = 0;
	A.rally_dist = 0;
	A.target_o = A.target_who = -1;
	A.navy = A.hurry = 0;
	A.list.fill(-1);
	return k;
}

int32_t armies_init_navy(Game& g, int32_t who, int32_t city, int32_t reg) {
	const int32_t k = armies_init_army(g, who, city);
	g.armies[size_t(who)][size_t(k)].navy = 1;
	g.armies[size_t(who)][size_t(k)].reg = reg;
	return k;
}

int32_t armies_find_army(Game& g, int32_t who, int32_t x, int32_t y, int32_t maxd, int32_t u) {  // @6f34f0
	const int32_t R = tregion_at(g, x, y);
	g.armies_find_dist = 99999999;
	int32_t best = -1;
	const Unit* uu = u >= 0 ? g.unit(who, u) : nullptr;
	for (int32_t k = 0; k < 16; k++) {
		const Army& A = g.armies[size_t(who)][size_t(k)];
		if (!A.valid || A.reg != R) continue;
		const int32_t d = vector_dist(x - A.x, y - A.y);
		if (!(d <= g.armies_find_dist && (maxd < 0 || d <= maxd))) continue;
		if (uu) {
			if ((uu->ptype->unit_flags2 & 0x40) && army_count(g, A, kCountType, kSupplyWagon) >= 3) continue;
			if ((uu->ptype->unit_flags2 & 0x20) && army_count(g, A, kCountType, kGeneral) >= 2) continue;
		}
		g.armies_find_dist = d;
		best = k;
	}
	return best;
}

void armies_process_all(Game& g) {  // @6f3b00 (frame step 12)
	for (int32_t who = 0; who < 8; who++) {
		const Leader& L = LD(g, who);
		if (!(L.flags & leaderflag::kValid) || (L.flags & 0xc) == leaderflag::kHuman || (L.flags2 & 0xa)) continue;
		for (int32_t k = 0; k < 16; k++) {
			Army& A = g.armies[size_t(who)][size_t(k)];
			if (!A.valid) continue;
			const int32_t force = A.status & armystat::kForceProcess;
			A.status &= ~armystat::kForceProcess;
			army_process(g, A, force);
		}
	}
}

void armies_emergency(Game& g, int32_t who) {  // @6f3250
	const Leader& L = LD(g, who);
	if ((L.flags & 0xc) == leaderflag::kHuman || (L.flags2 & 0xa)) return;
	for (Army& A : g.armies[size_t(who)]) {
		if (!A.valid) continue;
		A.target_o = A.target_who = -1;
		army_process(g, A, 1);
	}
}

void armies_diplo_change(Game& g, int32_t who) {  // @6f30f0
	if (who < 0 || who >= 8) return;
	const Leader& L = LD(g, who);
	if ((L.flags & 0xc) == leaderflag::kHuman || (L.flags2 & 0xa)) return;
	for (Army& A : g.armies[size_t(who)]) {
		if (A.valid) army_process(g, A, 1);
	}
}

int32_t object_get_army(const Game& g, const Unit& u) {  // @649d70
	if (u.group < 0) return -1;
	const Group& gr = g.groups.g[size_t(u.group)];
	if (!group_member(g, gr, u.o, u.who) || gr.army < 0 || u.who >= 8) return -1;
	const Army& A = g.armies[size_t(u.who)][size_t(gr.army)];
	for (int32_t i = 0; i < A.num_groups; i++) {
		if (A.list[size_t(i)] == u.group) return gr.army;
	}
	return -1;
}

void army_add_unit(Game& g, int32_t who, int32_t army, int32_t o) {  // Army::add_unit @6f9f40 (cast_create_decoy)
	if (who < 0 || who >= 8 || army < 0 || army >= int32_t(g.armies[size_t(who)].size())) return;
	add_unit(g, g.armies[size_t(who)][size_t(army)], o);
}

int32_t unit_add_to_army(Game& g, Unit& u) {  // Unit::add_to_army @5f7740
	int32_t a = object_get_army(g, u);
	if (a >= 0) return a;
	const int32_t who = u.who;
	if (who >= 8) return -1;
	if (u.ptype->domain == 1) {
		a = armies_find_army(g, who, u.x, u.y, -1, -1);
		if (a < 0) return a;
	} else {
		a = u.damage == 0 ? find_local_army(g, who, u.x, u.y, u.o) : armies_find_army(g, who, u.x, u.y, 0x2400, u.o);
		if (a < 0) {
			const int32_t c = objects_find_city(g, u.x, u.y, search::kFriendly, who, 0x200, filter::kAll, 0, 0);
			if (c < 0) return a;
			a = armies_init_army(g, who, c);
			add_unit(g, g.armies[size_t(who)][size_t(a)], u.o);
			return a;
		}
	}
	Army& A = g.armies[size_t(who)][size_t(a)];
	army_normalize(g, A);
	if (A.num_units != 0) {
		const int32_t f = get_unit(g, A, 0);
		Unit* lead = f >= 0 ? g.unit(who, f) : nullptr;
		if (lead && lead->alive()) {
			// Unit::go_to_unit @5f78c0 (on this unit, towards the army's first unit).
			for (Unit* m = &u; m;) {
				m->unit_masks |= umask::kInCombat;
				m = m->o_down >= 0 ? g.unit(who, m->o_down) : nullptr;
			}
			if (vector_dist(lead->x - u.x, lead->y - u.y) > 0x480) {
				int32_t x = lead->x, y = lead->y;  // Unit::go_to(x, y, ATTACK_TO, 0, 0x300) @5f7a50
				if (find_nearby_spot(g, *u.ptype, x, y, &x, &y, 0, 0x300, 0, 0x55555555u, kFilterNotMe, u.o, who, 0, 1, -1, 0,
				                     -1) == 0) {
					Group tmp;
					group_clear(tmp, -1);
					group_add(g, tmp, u.o, who, 0, 0);
					const int32_t id = groups_push_group(g, who, tmp, 1);
					if (id >= 0) group_action_move_to(g, g.groups.g[size_t(id)], x, y, kQueueNew, false, 0, kAttackTo, 0, -1, -1, 0);
				}
			}
		}
	}
	add_unit(g, A, u.o);
	return a;
}

void army_charge(Game& g, Army& A, int32_t o, int32_t w) {  // @6f9a90
	const Object* T = g.object(w, o);
	if (!T) return;
	for (int32_t i = 0; i < A.num_groups; i++) {
		const int32_t id = A.list[size_t(i)];
		if (id < 0 || GR(g, id).num == 0) continue;
		GR(g, id).army = A.army;
		move_group_to(g, id, T->x, T->y, kQueueFirst, false, 0, kAttackTo);
		A.target_o = o;
		A.target_who = w;
	}
}

// ---------------------------------------------------------------- GameDaemon::calc_danger @732d10

namespace {
void do_danger(Game& g, int32_t o, int32_t who, int32_t p, int32_t cell, int32_t v) {  // @732390
	auto& D = g.world.danger;
	const size_t i = size_t(p) * size_t(g.world.reg_size) + size_t(cell);
	if (i >= D.size()) return;
	if (p == who) {
		D[i] -= v;
		return;
	}
	if (LD(g, who).diplos(p) != 2) {
		if (o >= 0) {
			const Object* ob = g.object(who, o);
			if (ob && !object_is_seen(g, *ob, p, false)) v /= 2;
		}
		if (LD(g, who).diplos(p) == 1) v /= 2;
		D[i] += v;
		return;
	}
	D[i] -= v / 2;
}
}  // namespace

void calc_danger(Game& g) {
	World& w = g.world;
	if (w.reg_size <= 0) return;
	if (w.danger.size() < size_t(8) * size_t(w.reg_size)) w.danger.assign(size_t(8) * size_t(w.reg_size), 0);
	for (int32_t p = 0; p < 8; p++) {
		if (LD(g, p).flags & leaderflag::kActive)
			std::fill(w.danger.begin() + long(p) * w.reg_size, w.danger.begin() + long(p + 1) * w.reg_size, 0);
	}
	for (int32_t who = 0; who < 8; who++) {
		if (!(LD(g, who).flags & leaderflag::kValid)) continue;
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (!u || !u->alive() || !u->on_map() || !(u->ptype->role & 0x10000)) continue;
			const int32_t cell = regcell_of(u->y) * w.reg_xs + regcell_of(u->x);
			for (int32_t p = 0; p < 8; p++) {
				if (!(LD(g, p).flags & leaderflag::kActive) || p == who) continue;
				if (!(LD(g, who).diplos(p) == 0 || LD(g, p).diplos(who) == 0)) continue;
				do_danger(g, o, who, p, cell, (obj_attack(g, *u) * 5) / 10);
			}
		}
	}
	for (int32_t who = 0; who < 8; who++) {
		if (!(LD(g, who).flags & leaderflag::kValid)) continue;
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			const Build* b = g.build(who, o);
			if (!b || !b->alive() || !(b->flags & objflag::kActive)) continue;
			if (!(b->city >= 0 || (b->ptype->build_flags & 0x10))) continue;
			const int32_t bx = regcell_of(b->x), by = regcell_of(b->y);
			int32_t v;
			if (type_is(*b->ptype, ptype::kFortX) || type_is(*b->ptype, ptype::kTower)) {
				v = obj_hits_left(g, *b) / 2;  // ObjectData::hits_left
			} else if (!(b->flags & objflag::kCity)) {
				if (type_is(*b->ptype, ptype::kAirbase) || type_is(*b->ptype, ptype::kDock)) v = 100;
				else v = (g.type(basic_type(g, b->type)).build_flags & 0x40000000) ? 50 : 10;
			} else {
				v = 100;
			}
			for (int32_t p = 0; p < 8; p++) {
				if (!(LD(g, p).flags & leaderflag::kActive)) continue;
				for (int32_t k = 1; k <= 8; k++) {
					const int32_t cx = bx + move_x[k], cy = by + move_y[k];
					if (cx < 0 || cy < 0 || cx >= w.reg_xs || cy >= w.reg_ys) continue;
					do_danger(g, o, who, p, cy * w.reg_xs + cx, v / 2);
				}
				do_danger(g, o, who, p, by * w.reg_xs + bx, v);
			}
		}
	}
}

}  // namespace ron::sim
