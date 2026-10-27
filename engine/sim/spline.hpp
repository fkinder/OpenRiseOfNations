// Spline (spline.obj, @910be0 - @914590) as the missile ammo uses it: a degree-2 uniform B-spline through (start, launch control point, end) whose
// generated points are the projectile's frame-by-frame path (Ammo::init @67bbf0, Ammo::inc_time @67d380). The whole SplineData is hashed in
// `ammo` (control_verts, knots, spline_knots, weights, spline_verts, spline_normals with their allocation sizes), so the arrays keep the original's
// allocation (`*_size`: Array / SimpleArray growth with increment -1: 0 -> 4 -> 8 -> 16 ...) and a Spline object that went back to the
// Recycler<Spline> pool keeps it (Spline::clear @914410 only resets the lengths and the scalar members).
#pragma once

#include <cstdint>
#include <vector>

namespace ron::sim {

struct Vec3f {
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

// SplineData (+0x40 .. +0x100). Only the B-spline type with flags 0x10 (tangent normals, the missile ammo) is generated; the other types of the original
// (bezier, cubic) are never created by the simulation.
struct Spline {
	int32_t type = 3;                      // +0x40 (3 = B-spline)
	uint32_t flags = 0;                    // +0x44 (0x10 for a missile ammo)
	float max_control_depth_ratio = 0.0f;  // +0x48 (4.0 once calc_from_dir ran)
	float total_length = 0.0f;             // +0x4c: the length of the generated polyline (build_normals)
	int32_t last_knot = 0;                 // +0x50
	float curr_dist = -1.0f;               // +0x54
	float next_search_dist = -1.0f;        // +0x58
	int32_t search_scan = -1;              // +0x5c
	uint16_t degree = 4, depth = 1;        // +0x60, +0x62 (Spline::clear stores 0x100004)
	std::vector<Vec3f> control, verts, normals;  // control_verts (+0x64), spline_verts (+0xd4), spline_normals (+0xf0)
	std::vector<float> knots, spline_knots, weights;  // knots (+0x78), spline_knots (+0xb8), weights (+0xa0); only spline_knots is ever filled
	// Allocation sizes (Array::size / SimpleArray::size): grown by increase_size(-1) (size += size, 4 from empty) when an element is added at length >= size.
	int32_t control_size = 0, verts_size = 0, normals_size = 0, knots_size = 0, spline_knots_size = 0, weights_size = 0;
};

// Spline::clear @914410.
void spline_clear(Spline& s);

// Spline::calc_from_dir @913960: the control points start, mid (and `extra` when its length is not 0) and end, depth = int(polyline length / min_seg),
// then calc_spline (bspline, normals).
void spline_calc_from_dir(Spline& s, float min_seg, const Vec3f& start, const Vec3f& mid, const Vec3f& end, const Vec3f& extra);

// The launch control point of Ammo::init @67cb1f - @67cd05: `start` + (0, -magnitude, 0) rotated by the quaternion of `pitch` (whole degrees,
// Guy +0x4c, truncated) about x, then by the quaternion of the yaw about z. `yaw_degrees` is fast_angle_to_degrees(guy angle) (a whole number).
Vec3f spline_launch_control(const Vec3f& start, float magnitude, float pitch, float yaw_degrees);

}  // namespace ron::sim
