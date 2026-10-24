// Bird flocks: Objects::add_flock @65c0e0 (docs/aircraft-spec.md section 13). The birds' flight is the AIR_ATTACK_GROUND
// machinery of aircraft.cpp (the type-403 arms of do_air_physics and the animal branch of land_plane).
#include "sim/flocks.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#include "sim/aircraft.hpp"
#include "sim/compass.hpp"
#include "sim/game.hpp"
#include "sim/guys.hpp"

namespace ron::sim {

namespace {

// Unit::update_z @606590: the unit's own z from the (tile) heights, and each guy's z from the terrain under it (0 for ships).
void unit_update_z(Game& g, Unit& u) {
	u.z = find_tcoord_z(g, tcoord_of(u.x), tcoord_of(u.y), 1);
	for (Guy& gy : u.guys) gy.z = u.ptype->domain == 1 ? 0 : find_data_z(g, gy.x, gy.y, 0);
}

}  // namespace

void add_flock(Game& g, int32_t tx, int32_t ty, int32_t who, int32_t n) {
	if (who > -1 && size_t(who) < g.leaders.size() && (g.leaders[size_t(who)].flags & leaderflag::kValid)) {
		Leader& L = g.leaders[size_t(who)];
		if (g.frame < L.flock_stamp + 0xe1) return;
		L.flock_stamp = g.frame;
	}
	const World& w = g.world;
	auto draw = [&g]() { return g.rng.get(0, 0xffff); };
	auto clamp_wc = [&w](int32_t& x, int32_t& y) {
		x = std::max(x, 0);
		y = std::max(y, 0);
		if (x >= w.xs) x = w.xs - 1;
		if (y >= w.ys) y = w.ys - 1;
	};
	// The four waypoints (WCoords): the start cell, a cell within 1 of it, a cell within 2 of that, and the first forest cell on
	// the circle around the third (the start cell again when there is none within circle radius 8).
	std::array<std::array<int32_t, 2>, 4> pts{};
	const int32_t wx0 = tx >> 2, wy0 = ty >> 2;
	pts[0] = {wx0, wy0};
	int32_t cx = wx0 - 1 + draw() % 3;
	int32_t cy = wy0 + draw() % 3 - 1;
	clamp_wc(cx, cy);
	pts[1] = {cx, cy};
	int32_t px = cx + draw() % 5 - 2;
	int32_t py = cy + draw() % 5 - 2;
	clamp_wc(px, py);
	pts[2] = {px, py};
	const Circles& C = circles();
	int32_t idx = C.radius[3] - 1 < 1 ? 0 : draw() % C.radius[3];
	pts[3] = {wx0, wy0};
	for (; idx < C.radius[8]; idx++) {
		const int32_t x = C.x[size_t(idx)] + px, y = C.y[size_t(idx)] + py;
		if (x >= 0 && y >= 0 && x < w.xs && y < w.ys && (w.w(x, y).flags & 0x20) != 0) {
			pts[3] = {x, y};
			break;
		}
	}
	uint32_t angle = uint32_t(draw() % 12) * 0x15555555u;
	if (air_trace())
		std::fprintf(stderr, "[air] f%d flock of %d at tile (%d,%d) for %d: waypoints (%d,%d) (%d,%d) (%d,%d) (%d,%d)\n", g.frame, n, tx, ty, who, pts[0][0],
		             pts[0][1], pts[1][0], pts[1][1], pts[2][0], pts[2][1], pts[3][0], pts[3][1]);
	for (int32_t k = 0; k < n; k++) {
		const int32_t o = init_unit(g, kGaiaWho, kFlockBirdType, tx * 0xc0 + 0x60, ty * 0xc0 + 0x60, -1, -1, -1);
		if (o < 0) continue;
		Unit& u = *g.unit(kGaiaWho, o);
		unit_set_angle(g, u, angle, true);
		angle += uint32_t(draw() % 5 + 3) * 0xb60b60u;  // 3..7 degrees further round for the next bird
		unit_update_z(g, u);
		guy_set_new_z(u.guys[0], u.guys[0].z + 200, false);  // starts 200 above the ground
		for (const auto& p : pts) {
			const int32_t ru = draw() & 0x1ff;  // the y scatter is drawn first
			const int32_t rv = draw() & 0x1ff;
			add_air_attack_ground_order(g, u, p[0] * 0x300 + 0x80 + rv, ru + 0x80 + p[1] * 0x300, -1, -1, kQueueLast, false);
		}
	}
}

}  // namespace ron::sim
