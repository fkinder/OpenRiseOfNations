// Integer geometry of the simulation (gamemath.obj, docs/movement-spec.md section 1): binary angles, the sine table,
// find_angle, angle_diff and the octagonal vector_dist. Every function reproduces the original's integer arithmetic,
// including its wrap-arounds; the simulation never uses libm except to build the sine table once.
#pragma once

#include <cstdint>

namespace ron::sim {

// Binary angles: full circle = 2^32. 0 points to -y, 0x40000000 to +x, 0x80000000 to +y, 0xC0000000 to -x.
// A displacement of length r at angle a is (sin_a(a, r), -cos_a(a, r)).
constexpr uint32_t kAngle90 = 0x40000000u, kAngle180 = 0x80000000u, kAngle270 = 0xC0000000u;

// vector_dist @0046cff0 ("max + min^2 / 2max"); takes signed deltas (it abs()es them itself).
int32_t vector_dist(int32_t dx, int32_t dy);

// find_angle @0092d130: the angle of the displacement (dx, dy). (0, 0) -> 0x80000000.
uint32_t find_angle(int32_t dx, int32_t dy);

// sin_table @00a46a00 on an angle as passed in ECX (no folding), radius r (EDX).
int32_t sin_table(uint32_t a, int32_t r);
// sinx @0092d100 / cosx @0092d0c0: the wrappers every caller uses (fold into the first quadrant, sign of r).
int32_t sin_a(uint32_t a, int32_t r);
int32_t cos_a(uint32_t a, int32_t r);

// angle_diff @0092d0b0: d = a - b; d > 0x80000000 ? ~d : d (note ~d, not -d).
inline uint32_t angle_diff(uint32_t a, uint32_t b) {
	const uint32_t d = a - b;
	return d > 0x80000000u ? ~d : d;
}

// angle_to_degrees @00a28e00: the integer degrees [0, 360) of a binary angle (unsigned arithmetic: the angle is split into quarters, eighths and
// finer fractions; the sum of the pieces is not exact division, e.g. 0x15555555 -> 120).
inline int32_t angle_to_degrees_exact(uint32_t in) {
	const uint32_t q = in >> 0x1e;
	uint32_t a = in - q * 0x40000000u;
	const uint32_t h = a >> 0x1d;
	a = a - h * 0x20000000u;
	const uint32_t b = a % 0x15555555u;
	const uint32_t c = b % 0xaaaaaaau;
	return int32_t((c % 0x38e38e3u + 0x5b05b0u) / 0xb60b60u +
	               (c / 0x38e38e3u + ((h + q * 2u) * 3u + b / 0xaaaaaaau + (a / 0x15555555u) * 2u) * 3u) * 5u);
}

// C division helpers used throughout (truncating, like the original's idiv).
inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

}  // namespace ron::sim
