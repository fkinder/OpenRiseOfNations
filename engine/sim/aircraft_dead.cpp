// Dead crafts (docs/aircraft-spec.md section 12): what a dying aircraft leaves behind. Objects::kill_guy hands every guy of a
// non-missile unit of cat Air to Ammo::init_crash @67b800, which turns it into debris: an Ammo that keeps flying from the
// guy's position along its heading (the heading times the type's `moves` speed, for the time a body takes to fall from the
// guy's z under GRAV_Z), drifts by a per-frame random bank, ends at the first frame its ballistic height is below the
// terrain (Ammo::inc_time, combat_fight.cpp) and splashes there with radius 2 like any other ammo (so a crashing plane hurts
// what it falls on, never its own side). The strafing impact slide (Ammo::init) is in combat_fight.cpp.
//
// Single-precision math in the original's operation order: no fused multiply-add.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include <climits>
#include <cmath>
#include <cstdio>

#include "sim/aircraft.hpp"
#include "sim/game.hpp"
#include "sim/guys.hpp"
#include "sim/random.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

namespace {

constexpr float kGravZ = -10.4875f;  // GRAV_Z @cab378

// cvtss2sd + cvttsd2si: a value out of range (or a NaN) gives the "integer indefinite" 0x80000000.
int32_t ftoi(float f) {
	const double d = double(f);
	if (!(d > -2147483649.0 && d < 2147483648.0)) return INT_MIN;
	return int32_t(d);
}
float itof(uint32_t u) { return float(double(u)); }

}  // namespace

void add_crash_ammo(Game& g, Unit& u, int32_t guy_index) {
	if (guy_index < 0 || size_t(guy_index) >= u.guys.size()) return;
	const Guy& gy = u.guys[size_t(guy_index)];
	// Objects::kill_guy: the first Ammo slot not in use, else a new one at the end.
	size_t i = 0;
	while (i < g.ammo.size() && g.ammo[i].in_use()) i++;
	if (i == g.ammo.size()) g.ammo.emplace_back();
	Ammo& a = g.ammo[i];
	const int32_t graph_index = g.ammo_counter;
	g.ammo_counter++;

	// ---- Ammo::init_crash(guy, index, graph_index) @67b800
	a.who = gy.who;
	a.o = gy.o;
	a.flags &= 0xe3;
	a.index = int32_t(i);
	a.graph_index = graph_index;
	a.num_guys = 1;
	a.gpiece = kCrashPiece;
	a.crash_piece = gy.gpiece;  // Ammo +0x8 = the unit's vslot +0x34 (its piece), not a constant
	// start_roll_angle: the bank of a plane in a strafing / patrolling / bombing run (STRAFE, AIR_PATROL, AIR_ATTACK_GROUND).
	a.start_roll_angle = 0;
	if (const Order* f = u.front()) {
		if (f->type == kStrafe || f->type == kAirPatrol || f->type == kAirAttackGround) a.start_roll_angle = -ftoi(u.guys[0].bank);
	}
	a.sx = gy.x;
	a.sy = gy.y;
	a.sz = gy.z;
	const int32_t moves = g.type(gy.type).moves * g.rules->constants.at(0x4);  // type.moves * unit_move_speed
	int32_t sin_a_v = 0, sin_b_v = 0;
	if (moves != 0) {
		sin_a_v = sin_a(gy.angle - 0x40000000u, moves);  // the first sin_table call (-> ey)
		sin_b_v = sin_a(gy.angle, moves);                // the second (-> ex)
	}
	// t = the fall time from the guy's z: sqrtf((float)(-2 * sz) / GRAV_Z)
	const float t = std::sqrt(float(-gy.z - gy.z) / kGravZ);
	int32_t ex = ftoi(float(sin_b_v) * t) + u.x;  // the unit's position, not the guy's
	int32_t ey = ftoi(float(sin_a_v) * t) + u.y;
	if (ex < 0) ex = 0;
	if (ey < 0) ey = 0;
	if (ex >= g.world.xs * 0x300) ex = g.world.xs * 0x300 - 1;
	if (ey >= g.world.ys * 0x300) ey = g.world.ys * 0x300 - 1;
	a.ex = ex;
	a.ey = ey;
	a.ez = find_data_z(g, ex, ey, 0);
	a.flags |= 2;
	a.angle = gy.angle;
	a.splash_area = 2;
	a.cur_time = 0;
	// total = max(1, trunc(sqrtf(dist^2) / (float)(moves)))   (the unsigned compare keeps 0x80000000 for a type without speed)
	{
		const int32_t dx = ex - a.sx, dy = ey - a.sy;
		const float D = std::sqrt(float(dx * dx + dy * dy));
		int32_t total = ftoi(D / float(moves));
		if (uint32_t(total) < 1u) total = 1;
		a.total_time = uint32_t(total);
	}
	a.rolling = int8_t(g.rng.get(0, 0xffff) % 7 - 3);  // @67baf7
	a.traj = 1;
	a.v1z = 0.0f;
	{
		const int32_t dx = a.ex - a.sx, dy = a.ey - a.sy;
		a.dx = std::sqrt(float(dx * dx + dy * dy)) / itof(a.total_time);
	}
	// A local Random seeded with sx + sy + sz (not the game stream, but deterministic): the drift of the debris.
	Random local(uint32_t(a.sx + a.sy + a.sz));
	a.bank_dx = float(local.get(-30, 30));
	a.bank_dy = float(local.get(-30, 30));
	a.whom = -1;
	a.ox = -1;
	if (air_trace())
		std::fprintf(stderr, "[air] f%d L%d unit %d (type %d) crashes: debris slot %zu from (%d,%d,%d) to (%d,%d,%d) in %u frames\n",
		             g.frame, u.who, u.o, u.type, i, a.sx, a.sy, a.sz, a.ex, a.ey, a.ez, a.total_time);
}

}  // namespace ron::sim
