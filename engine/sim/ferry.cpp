// Unit::think_civilian_transport(flag) @5f40d0 (docs/transport-spec.md, docs/ai-spec.md 12.6): the "ferry search" of a land unit that the
// leader can carry across water. After the guard block (a unit the leader cannot carry, or that is not flagged / not cargo, falls back
// to the explorer's random walk when it is auto-exploring) it looks for a land region the unit should be taken to and for the
// coast cell of the unit's own region (or of any region it shares a sea with) to wait at:
//   flag 0 (a scout, from the end of Unit::think_scout): every land region 1..63 that Region::go_here rates 1 or 2 (free to settle, or an
//     enemy's) and that this leader has not scouted yet (the region's `scouted` bit);
//   flag 1 (an AI citizen, from Unit::think_peasant): the regions rated 1 that the leader has no citizen on the way to yet
//     (`reg_xport_peasants`), only inside its own territory and while it has more cities than citizens already bound.
// For such a region and a sea region both regions border, the coast cells of the region (a 1/50 stride of its raster list, from a
// starting phase `o + frame`) score `vector_dist(unit, cell) * max(danger, 1)`; the cheapest one is where the unit's group moves
// (MOVE_TO, or EXPLORE_TO for an auto-exploring unit) and, for a citizen, the counters are bumped.
//
// Evidence (local, gitignored): re/decomp fidelity14 dir (Unit_think_civilian_transport@005f40d0.c, Region_coast_here@00681020.c,
// Region_go_here@006810f0.c, WorldData_num_waterhalf@006b4db0.c), the disassembly 5f4380 - 5f4620.
#include <algorithm>
#include <cstdint>

#include "sim/ai.hpp"
#include "sim/ai_internal.hpp"
#include "sim/compass.hpp"
#include "sim/game.hpp"
#include "sim/groups.hpp"
#include "sim/orders.hpp"
#include "sim/regions.hpp"
#include "sim/transport.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

namespace {

// Region::go_here @6810f0 (the same function ai_army.cpp keeps in its anonymous namespace for Army::do_transporting).
int32_t ferry_go_here(const Game& g, int32_t r, int32_t who) {
	const Region& R = g.regions.r[size_t(r)];
	const Leader& L = g.leaders[size_t(who)];
	int32_t v = 0;
	if ((R.flags & 8) && L.reg_cities[size_t(r & 63)] == 0 && L.reg_peasants[size_t(r & 63)] == 0) {
		bool crowded = false;
		for (int32_t p = 0; p < 8; p++) {
			if (p == who || !(g.leaders[size_t(p)].flags & leaderflag::kActive)) continue;
			const int32_t c = uint16_t(g.leaders[size_t(p)].reg_cities[size_t(r & 63)]);
			if (c != 0 && R.size < 200 * c) crowded = true;
		}
		if (!crowded) v |= 1;
	}
	for (int32_t p = 0; p < 8; p++) {
		const Leader& P = g.leaders[size_t(p)];
		if (p == who || !(P.flags & leaderflag::kActive)) continue;
		if (!(L.diplos(p) == 0 || P.diplos(who) == 0)) continue;  // at war (LeaderData::diplos both ways)
		if (P.reg_cities[size_t(r & 63)] == 0) continue;
		v |= (L.reg_cities[size_t(r & 63)] == 0 || (L.strategy[size_t(r & 63)] & 4)) ? 2 : 4;
	}
	return v;
}

// WorldData::num_waterhalf @6b4db0: the water tiles of a half-land wcoord (0 for any other).
int32_t num_waterhalf(const World& w, int32_t wx, int32_t wy) {
	if (!(w.w(wx, wy).flags & wflag::kHalfLand)) return 0;
	int32_t n = 0;
	for (int32_t i = 0; i < 16; i++)
		if ((w.t(4 * wx + (i & 3), 4 * wy + (i >> 2)) & tmask::kMedium) == tmask::kWater) n++;
	return n;
}

// Region::coast_here(this = land region, sea, wx, wy) @681020: the first neighbour direction (1..8) of the wcoord whose tile region is the
// other side of the coast, 0 when the wcoord is not on that coast.
int32_t coast_here(const Game& g, const Region& R, int32_t other, int32_t wx, int32_t wy) {
	if (!g.regions.is_coast(R.region, other)) return 0;
	const World& w = g.world;
	const int32_t here = w.in_w(wx, wy) ? w.w(wx, wy).region : 0;
	if (here != R.region) {
		if (here != other) return 0;
		other = R.region;
	}
	for (int32_t k = 1; k < 9; k++) {
		const int32_t nx = wx + move_x[k], ny = wy + move_y[k];
		if (nx < 0 || ny < 0 || nx >= w.xs || ny >= w.ys) continue;
		if (w.get_tregion(nx * 4 + 2, ny * 4 + 2) == other) return k;
	}
	return 0;
}

}  // namespace

int32_t unit_think_ferry(Game& g, Unit& u, int32_t flag) {
	const int32_t who = u.who;
	const int32_t dom = u.ptype->domain;
	if (dom == 1 && (u.ptype->unit_flags & 0x10) == 0) return 0;
	if (dom != 2) {
		if (leader_can_transport(g, who) < unit_transport_type(g, u) || !unit_can_transport(u) || !object_is_cargo(g, u)) {
			if (!(u.unit_masks & umask::kAutoExplore)) return 0;
			return think_scout(g, u, 1);
		}
	}
	const World& w = g.world;
	Leader& L = g.leaders[size_t(who)];
	const int32_t tx = tcoord_of(u.x), ty = tcoord_of(u.y);
	const int32_t R = w.in_t(tx, ty) ? w.get_tregion(tx, ty) : 0;
	const int32_t uwx = wcoord_of(u.x), uwy = wcoord_of(u.y);
	if (flag != 0) {
		if (L.city_num <= L.xport_peasants) return 0;
		if (!w.in_w(uwx, uwy) || w.w(uwx, uwy).who != who) return 0;
		if (L.reg_cities[size_t(R & 63)] == 0) return 0;
	}
	int32_t best_region = -1, best = 99999999, bx = tx, by = ty;
	for (int32_t k = 1; k < 64; k++) {
		const Region& K = g.regions.r[size_t(k)];
		if (K.size == 0) continue;
		const int32_t go = ferry_go_here(g, k, who);
		bool candidate;
		if (flag == 0) candidate = (go & 3) != 0 && !(K.scouted & (1u << (who & 7)));
		else candidate = (go & 1) != 0 && L.reg_xport_peasants[size_t(k)] == 0;
		if (!candidate) continue;
		// A sea region both this unit's region and region k border.
		int32_t sea = 0x41;
		for (; sea < Regions::kNum - 1; sea++) {
			const Region& S = g.regions.r[size_t(sea)];
			if (S.size != 0 && g.regions.is_coast(S.region, R) && g.regions.is_coast(S.region, k)) break;
		}
		if (sea == Regions::kNum - 1) continue;
		int32_t n = K.size / 0x32;
		if (n < 0x10) n = 0x10;
		const int32_t count = int32_t(K.coords.size());
		bool found = false;
		int32_t phase = u.o + g.frame;
		for (int32_t attempt = 0; attempt < n; attempt++, phase++) {
			int32_t idx = phase % n;
			if (idx >= count) continue;
			for (; idx < count; idx += n) {
				const int32_t cx = K.coords[size_t(idx)].x, cy = K.coords[size_t(idx)].y;
				if (!coast_here(g, K, sea, cx, cy)) continue;
				if (num_waterhalf(w, cx, cy) != 0) continue;
				found = true;
				const int32_t danger = ai::danger_at_w(g, who, cx, cy);
				const int32_t score = vector_dist(std::abs(cy - uwy), std::abs(cx - uwx)) * std::max(danger, 1);
				if (score >= 0 && score < best) {
					best_region = k;
					bx = cx;
					by = cy;
					best = score;
				}
			}
			if (found) break;
		}
	}
	if (best_region >= 0) {
		Group tmp;
		group_clear(tmp, -1);
		group_add(g, tmp, u.o, who, 0, 0);
		const int32_t id = groups_push_group(g, who, tmp, 1);
		if (id >= 0)
			group_action_move_near(g, g.groups.g[size_t(id)], bx * 0x300 + 0x180, by * 0x300 + 0x180, 0, kQueueNew, false, 0,
			                       (u.unit_masks & umask::kAutoExplore) ? kExploreTo : kMoveTo, 0, -1, -1, 0);
		if (flag != 0) {
			L.xport_peasants++;
			const int32_t r = w.in_w(bx, by) ? w.w(bx, by).region : 0;
			if (r >= 0 && r < 64) L.reg_xport_peasants[size_t(r)]++;
		}
		return 1;
	}
	if (!(u.unit_masks & umask::kAutoExplore)) return 0;
	return think_scout(g, u, 1);
}

}  // namespace ron::sim
