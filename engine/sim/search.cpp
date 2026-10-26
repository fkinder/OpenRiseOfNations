// Object queries around a point and UnitType::find_nearby_spot (see search.hpp).
//
// Evidence (decompiles + disassembly, local re/decomp/move_group2/): ObjectsData::find_unit_with_radius @00659890,
// find_unit_ordered_with_radius @00658ef0, Objects::find_collision @0065b1b0, find_ordered_collision @0065b440,
// Search::valid_search @0067daa0 / valid_filter @0067dbb0 (jump table at 0067e57c), project @0092cf40,
// UnitType::find_nearby_spot @0061de70.
#include "sim/search.hpp"

#include "sim/compass.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/movement.hpp"
#include "sim/placement.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

namespace {

const Object* obj_at(const Game& g, int32_t who, int32_t o) { return g.object(who, o); }
// vtable +8 is_valid_unit (buildings: 0) and +0xbc is_on_map.
bool valid_on_map_unit(const Object* ob) {
	return ob && ob->is_unit && ob->alive() && static_cast<const Unit*>(ob)->on_map();
}

// Search::valid_filter: only the filters the movement code passes are ported.
bool valid_filter(const Game& g, int32_t cand_who, int32_t cand_o, int32_t o, int32_t who, int32_t filter) {
	switch (filter) {
	case kFilterNotMe:
		return !(cand_o == o && cand_who == who);
	case 0x11: {  // FILTER_SEEN (jump entry 16 @67e1d5): vslot +0x48 is_seen(who, 0)
		const Object* ob = g.object(cand_who, cand_o);
		return ob && object_is_seen(g, *ob, who, false);
	}
	default:
		// TODO(search.obj): FILTER_TYPE .. FILTER_GOV_HERO (Search::valid_filter jump table @0067e57c). The movement
		// callers only use FILTER_ALL and FILTER_NOT_ME.
		return true;
	}
}

// The filter step of the searches: FILTER_ALL and indices outside 1..23 pass.
bool pass_filter(const Game& g, int32_t cand_who, int32_t cand_o, int32_t o, int32_t who, int32_t filter) {
	if (filter == kFilterAll || uint32_t(filter - 1) > 0x16u) return true;
	return valid_filter(g, cand_who, cand_o, o, who, filter);
}

bool move_family(OrderType t) {
	return t == kMoveTo || t == kAttackTo || t == kExploreTo || t == kFleeTo || t == kChangeForm || t == kGroupMove ||
	       t == kGroupAttackTo;
}


}  // namespace

void project(int32_t x, int32_t y, uint32_t angle, int32_t r, int32_t* ox, int32_t* oy) {
	*ox = x + sin_a(angle, r);
	*oy = y - cos_a(angle, r);
}

int32_t find_unit_with_radius(const Game& g, int32_t x, int32_t y, int32_t search, int32_t radius, int32_t filter,
                              int32_t o, int32_t who, int32_t* who_out) {
	// Search::valid_search is called as valid_search(cand_who, 0, search): switch(0) has no case, so every leader
	// passes (the arguments look swapped in the original). Objects +0x1fc/+0x200 (best distance / its leader) are
	// scratch outputs that nothing in the simulation reads back; not modelled.
	(void)search;
	int32_t best_d = 99999999, best = -1, best_who = -1;
	const Circles& C = circles();
	const int32_t k = (radius + 0x2ff) / 0x300;
	auto consider = [&](int32_t cw, int32_t co) -> int {  // 1 = return now
		const Object* ob = obj_at(g, cw, co);
		if (!valid_on_map_unit(ob)) return 0;
		if (!pass_filter(g, cw, co, o, who, filter)) return 0;
		const int32_t d = vector_dist(ob->x - x, ob->y - y);
		const int32_t br = ob->ptype->big_radius;
		if (d <= br) {
			best = co;
			best_who = cw;
			return 1;
		}
		const int32_t e = d - br;
		if (e <= best_d && (radius < 0 || e <= radius)) {
			best_d = e;
			best = co;
			best_who = cw;
		}
		return 0;
	};
	if (radius >= 0 && k <= 64 && C.radius[size_t(k)] <= g.total_units) {
		const World& w = g.world;
		const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
		for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
			const int32_t cx = wx + C.x[size_t(i)], cy = wy + C.y[size_t(i)];
			if (!w.in_w(cx, cy)) continue;
			const WData& d = w.w(cx, cy);
			int32_t co = d.down, cw = d.down_who;
			while (co >= 0) {
				const Object* ob = obj_at(g, cw, co);
				const int32_t no = ob ? ob->down : -1, nw = ob ? ob->down_who : 0;
				if (cw < 8 && consider(cw, co)) {
					if (who_out) *who_out = best_who;
					return best;
				}
				co = no;
				cw = nw;
			}
		}
	} else {
		// TODO: a radius whose circle index exceeds 64 reads past circle_radius in the original; the full scan is used.
		for (int32_t p = 0; p < 8; p++) {
			if (!(g.leaders[size_t(p)].flags & leaderflag::kValid)) continue;
			const int32_t mark = g.objects[size_t(p)].unit_mark;
			for (int32_t co = 0; co < mark; co++) {
				if (consider(p, co)) {
					if (who_out) *who_out = best_who;
					return best;
				}
			}
		}
	}
	if (who_out) *who_out = best_who;
	return best;
}

int32_t find_unit_ordered_with_radius(const Game& g, int32_t x, int32_t y, int32_t search, int32_t radius,
                                      int32_t filter, int32_t o, int32_t who) {
	if (uint32_t(search) >= 8u) return -1;  // "Not passing valid who."
	int32_t best_d = 99999999, best = -1;
	const int32_t mark = g.objects[size_t(search)].unit_mark;
	for (int32_t co = 0; co < mark; co++) {
		const Unit* u = g.unit(search, co);
		if (!u || !u->alive()) continue;
		if (!pass_filter(g, search, co, o, who, filter)) continue;
		if (!move_family(u->order_type())) continue;
		const int32_t d = vector_dist(u->x - x, u->y - y);
		const int32_t br = u->ptype->big_radius;
		if (d <= br) return co;
		const int32_t e = d - br;
		if (e <= best_d && (radius < 0 || e <= radius)) {
			best_d = e;
			best = co;
		}
	}
	return best;
}

int32_t find_collision(Game& g, int32_t x, int32_t y, int32_t o, int32_t who, int32_t p5) {
	Unit* me = g.unit(who, o);
	if (!me) return 0;
	const int32_t r = me->ptype->new_block_radius;
	if (p5 == 0 && me->ptype->domain == 0) return collide_here(g, *me, ucoord_of(x), ucoord_of(y), r, nullptr, nullptr, false);
	if (r == 0) return 0;
	const World& w = g.world;
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y), ux = ucoord_of(x), uy = ucoord_of(y);
	for (int i = 0; i < 9; i++) {
		const int32_t cx = move_x[i] + wx, cy = move_y[i] + wy;
		if (!w.in_w(cx, cy)) continue;
		const WData& d = w.w(cx, cy);
		int32_t co = d.down, cw = d.down_who;
		while (co >= 0) {
			const Object* ob = obj_at(g, cw, co);
			const int32_t no = ob ? ob->down : -1, nw = ob ? ob->down_who : 0;
			if ((co != o || cw != who) && cw < 8 && valid_on_map_unit(ob)) {
				const int32_t r2 = ob->ptype->new_block_radius;
				if (r2 != 0) {
					const int32_t s = r + r2;
					if (iabs(ux - ucoord_of(ob->x)) <= s && iabs(uy - ucoord_of(ob->y)) <= s) return 1;
				}
			}
			co = no;
			cw = nw;
		}
	}
	return 0;
}

int32_t find_ordered_collision(const Game& g, int32_t x, int32_t y, int32_t o, int32_t who) {
	const Unit* me = g.unit(who, o);
	if (!me) return 0;
	const int32_t r = me->ptype->new_block_radius;
	if (r == 0) return 0;
	const World& w = g.world;
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y), ux = ucoord_of(x), uy = ucoord_of(y);
	for (int i = 0; i < 9; i++) {
		const int32_t cx = move_x[i] + wx, cy = move_y[i] + wy;
		if (!w.in_w(cx, cy)) continue;
		const WData& d = w.w(cx, cy);
		int32_t co = d.down, cw = d.down_who;
		while (co >= 0) {
			const Object* ob = obj_at(g, cw, co);
			const int32_t no = ob ? ob->down : -1, nw = ob ? ob->down_who : 0;
			if ((co != o || cw != who) && cw < 8 && valid_on_map_unit(ob)) {
				const Unit* u = static_cast<const Unit*>(ob);
				const int32_t r2 = u->ptype->new_block_radius;
				if (r2 != 0) {
					const int32_t s = r + r2;
					if (iabs(ux - ucoord_of(u->orders_x)) <= s && iabs(uy - ucoord_of(u->orders_y)) <= s) return 1;
				}
			}
			co = no;
			cw = nw;
		}
	}
	// The other members of my group (their order targets), when I am listed in it.
	if (me->group < 0) return 0;
	const Group& gr = g.groups.g[size_t(me->group)];
	if (who != gr.who) return 0;
	const Object* self = obj_at(g, gr.who, o);
	if (!self || !self->alive()) return 0;
	bool listed = false;
	for (int32_t i = 0; i < gr.num; i++)
		if (gr.list[size_t(i)] == o) {
			listed = true;
			break;
		}
	if (!listed || gr.buildings) return 0;
	for (int32_t i = 0; i < gr.num; i++) {
		const int32_t mo = gr.list[size_t(i)];
		const Unit* u = g.unit(who, mo);
		if (!u || !u->alive() || u->inside_up >= 0 || mo == o) continue;
		const int32_t r2 = u->ptype->new_block_radius;
		if (r2 == 0) continue;
		const int32_t s = r + r2;
		if (iabs(ux - ucoord_of(u->orders_x)) <= s && iabs(uy - ucoord_of(u->orders_y)) <= s) return 1;
	}
	return 0;
}

int32_t find_nearby_spot(Game& g, const Type& t, int32_t x, int32_t y, int32_t* ox, int32_t* oy, int32_t r0, int32_t r1,
                         int32_t step, uint32_t angle, int32_t filter, int32_t o, int32_t who, int32_t nocheck,
                         int32_t uber, int32_t bo, int32_t bwho, int32_t region) {
	const World& w = g.world;
	if ((r0 > 0 && r1 == 0) || r1 < 0) {
		if (uber) {
			r1 = r0 + 0xc0 + (((t.uber_size - 1) * t.guy_spacing) / 2 + t.big_radius) * 4;
		} else {
			r1 = r0 + t.big_radius * 4;
			if (t.big_radius == 0 && !(t.unit_flags & 0x10)) r1 = r0 + 0x240;
		}
	}
	if (r1 < r0) r1 = r0;
	if (step < 1) step = (r1 - r0) / 8;
	const int32_t inc = step > 1 ? step : 1;
	int32_t crad = t.block_radius;  // collision radius of the object test
	if (crad == 0 && filter == kFilterAll) crad = 0x180;
	bool use_coll = false;
	if (uber) {
		crad += ((t.uber_size - 1) * t.guy_spacing) / 2 + 0x30;
	} else if ((filter == kFilterNotMe || filter == kFilterCanCollide) && o >= 0) {
		use_coll = who >= 0;
	}
	// Naval units with an attack (or is(0x15f, strict)) avoid TData 0x2400.
	const bool naval_care = t.domain == 1 && (t.attack != 0 || type_is(t, 0x15f, true));
	// The building (bo, bwho) whose footprint the spot must stay out of (swarm_around's builders, gatherers and repairers pass their
	// target; every other caller passes -1). Citizens heading for a farm (ObjectData::is(0x1a1)) may stand inside it. @61de70.
	const Object* site = bo >= 0 ? g.object(bwho, bo) : nullptr;
	if (site && site->is_unit) site = nullptr;  // vtable +0x20 (is a building): 0 for units
	bool bldg_flag = false;
	if (site && bo >= 0 && (t.type == 0x32 || t.type == 0x33)) bldg_flag = type_is(*site->ptype, kFarm);
	Footprint site_fp{};
	if (site) site_fp = footprint(*site->ptype, site->x, site->y);
	int32_t c = 0;
	for (;;) {
		int32_t px = x, py = y;
		if (r0 != 0) {
			const uint32_t a = (uint32_t(c) << 28) + ((uint32_t(c + 7) > 14u) ? 0x8000000u : 0u) + angle;
			project(x, y, a, r0, &px, &py);
		}
		px = ucoord_of(px) * 48 + 24;
		py = ucoord_of(py) * 48 + 24;
		if (r0 == 0) c = 16;
		else c = -(c > 0 ? c + 1 : c);
		bool ok = false;
		do {
			if (px < 0 || py < 0 || px >= w.tile_xs * 192 || py >= w.tile_ys * 192) break;
			const int32_t tx = tcoord_of(px), ty = tcoord_of(py);
			if (region >= 0 && int16_t(w.get_tregion(tx, ty)) != region) break;
			const uint16_t m = w.t(tx, ty);
			if (t.domain != 2 && (m & tmask::kBlocked)) break;
			if (site && !bldg_flag && tx >= site_fp.x0 && tx < site_fp.x0 + site_fp.w && ty >= site_fp.y0 && ty < site_fp.y0 + site_fp.h)
				break;  // inside the building's footprint
			if (t.domain == 1) {
				if ((m & tmask::kMedium) != tmask::kWater) break;
				if (naval_care && (m & 0x2400)) break;
			} else if (t.domain != 2) {
				if ((m & tmask::kMedium) == tmask::kWater) break;
			}
			if (nocheck) {
				ok = true;
			} else if (use_coll) {
				ok = find_collision(g, px, py, o, who, 0) == 0 && find_ordered_collision(g, px, py, o, who) == 0;
			} else {
				ok = find_unit_with_radius(g, px, py, -1, crad, filter, o, who) < 0 &&
				     (who < 0 || find_unit_ordered_with_radius(g, px, py, who, crad, filter, o, who) < 0);
			}
		} while (false);
		if (ok) {
			*ox = px;
			*oy = py;
			return 0;
		}
		c++;
		if (c > 15) {
			if (r1 <= r0) break;
			r0 = r0 + inc < r1 ? r0 + inc : r1;
			c = 0;
		}
	}
	*ox = x;
	*oy = y;
	return 1;
}

}  // namespace ron::sim
