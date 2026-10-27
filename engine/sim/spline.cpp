#include "sim/spline.hpp"

#include <cmath>

// Every function below follows the SSE2 instruction order of the original (float arithmetic, the lengths through a double sqrt like
// __libm_sse2_sqrt_precise): the generated points and normals are hashed bit for bit. Verified against the four splines of the recorded saves
// (22'28'15 and 22'45'30: the launch control point, the 13 / 3 points and normals, the total length).

namespace ron::sim {

namespace {

float dsqrt(float v) { return float(std::sqrt(double(v))); }  // CVTPS2PD, __libm_sse2_sqrt_precise, CVTSD2SS

// Array::increase_size(-1) before an element is added at `len` >= size: size += size (4 from an empty array).
void grow(int32_t& size, size_t len) {
	if (int32_t(len) >= size) size = size ? size * 2 : 4;
}
void push(std::vector<Vec3f>& v, int32_t& size, const Vec3f& p) {
	grow(size, v.size());
	v.push_back(p);
}

struct Quat {
	float x, y, z, w;
};

// fast_half_degree_to_cosine / sine @a29010 / @a29110: tables of cosf / sinf((float)k * 0.5f * 0.017453292f), k < 360.
float hd_arg(int k) { return (float(k) * 0.5f) * 0.017453292f; }
float hd_cos(int k) { return float(std::cos(double(hd_arg(k)))); }
float hd_sin(int k) { return float(std::sin(double(hd_arg(k)))); }

// Point<float>::rotate @420b10: p rotated by the quaternion q.
Vec3f rotate(const Vec3f& p, const Quat& q) {
	const float qx = q.x, qy = q.y, qz = q.z, qw = q.w, px = p.x, py = p.y, pz = p.z;
	const float qxqx = qx * qx, qyqy = qy * qy, qzqz = qz * qz;
	const float qyqx = qy * qx, qzqx = qz * qx, qzqy = qz * qy;
	const float qwqz = qw * qz, qwqy = qw * qy, qwqx = qw * qx;
	float x1 = qzqz + qyqy;
	float x0 = qwqz + qyqx;
	float x4 = qyqx - qwqz;
	float x2 = qzqx - qwqy;
	x1 += x1;
	x0 *= py;
	x4 *= px;
	x2 *= pz;
	float x7 = qwqy + qzqx;
	x2 += x0;
	x0 = 1.0f - x1;
	x1 = qwqx + qzqy;
	x7 *= px;
	x2 += x2;
	x0 *= px;
	x1 *= pz;
	x0 += x2;
	x2 = qzqz + qxqx;
	x1 += x4;
	const float nx = x0;
	x2 += x2;
	x1 += x1;
	x0 = 1.0f - x2;
	x0 *= py;
	x0 += x1;
	const float ny = x0;
	x1 = qyqy + qxqx;
	x0 = qzqy;
	x1 += x1;
	x0 -= qwqx;
	float x5 = 1.0f - x1;
	x0 *= py;
	x5 *= pz;
	x0 += x7;
	x0 += x0;
	x5 += x0;
	return Vec3f{nx, ny, x5};
}

// Spline::calc_bspline_basis @9116e0 (Cox - de Boor): the basis function i of degree p at t.
float basis(const float* t, int i, int p, const std::vector<float>& k) {
	if (i + p >= int(k.size())) return 1.0f;
	if (p == 0) return (*t >= k[size_t(i)] && k[size_t(i) + 1] > *t) ? 1.0f : 0.0f;
	float res = 0.0f;
	const float d1 = *t - k[size_t(i)], d2 = k[size_t(i + p)] - k[size_t(i)];
	if (d1 > 0.0f && d2 > 0.0f) {
		const float b = basis(t, i, p - 1, k);
		res = b * (d1 / d2);
	}
	const float e1 = k[size_t(i + p) + 1] - *t, e2 = k[size_t(i + p) + 1] - k[size_t(i) + 1];
	if (e1 > 0.0f && e2 > 0.0f) {
		const float b = basis(t, i + 1, p - 1, k);
		res = res + b * (e1 / e2);
	}
	return res;
}

// Spline::clamp_knots @910be0 (no weights): [0 x (degree + 1), 1, 2 ... up to count, then the last value repeated].
void clamp_knots(Spline& s) {
	const int count = int(s.control.size());
	int deg = s.degree;
	int n = deg + count;
	if (count < deg) {
		do {
			deg = (deg + 0xffff) & 0xffff;
			--n;
		} while (count < deg);
		s.degree = uint16_t(deg);
	}
	s.spline_knots.clear();
	float acc = 0.0f;
	for (int i = 0; i <= n; ++i) {
		if (i > deg && i <= count) acc += 1.0f;
		grow(s.spline_knots_size, s.spline_knots.size());
		s.spline_knots.push_back(acc);
	}
}

// Spline::generate_bspline @911820.
void generate_bspline(Spline& s) {
	if (!(s.flags & 0x80)) clamp_knots(s);
	const std::vector<float>& k = s.spline_knots;
	const int last = int(k.size()) - 1;
	float delta = (k[size_t(last)] - k[0]) / float(s.depth);
	if (delta == 0.0f) delta = 1.0f;  // (the original reports "Spline knot problem" first)
	float t = k[0] + delta;
	push(s.verts, s.verts_size, s.control[0]);
	int i = 0;
	const int nk = int(k.size());
	while (k[size_t(last)] > t) {
		float vx = 0.0f, vy = 0.0f, vz = 0.0f;
		int ecx = i + 1;
		while (ecx < nk) {
			const float k1 = k[size_t(ecx)];
			if (!(t > k1) && k[size_t(ecx) - 1] < k1) break;
			++ecx;
			++i;
		}
		if (i > last) break;
		int start = i - int(s.degree);
		if (start < 0) start = 0;
		if (start <= i) {
			float x = 0.0f, y = 0.0f, z = 0.0f;
			for (int j = start; j <= i; ++j) {
				const float b = basis(&t, j, int(s.degree), k);
				const Vec3f& c = s.control[size_t(j)];
				x = x + c.x * b;
				y = y + c.y * b;
				z = z + c.z * b;
			}
			vx = x;
			vy = y;
			vz = z;
		}
		t = t + delta;
		push(s.verts, s.verts_size, Vec3f{vx, vy, vz});
	}
	if (double(k[size_t(last)] - (t - delta)) > 0.001) push(s.verts, s.verts_size, s.control.back());
	if (s.verts.empty()) {
		s.verts = s.control;
		if (s.verts_size < int32_t(s.verts.size())) s.verts_size = int32_t(s.verts.size());
	}
}

// Spline::build_normals @911f60 for flags 0x10: the length of the polyline and the unit tangents (the sum of the two neighbouring segments; the last one is the
// direction of the last CONTROL segment).
void build_normals(Spline& s) {
	s.total_length = 0.0f;
	s.normals.clear();
	float px = 0.0f, py = 0.0f, pz = 0.0f;
	for (size_t i = 1; i < s.verts.size(); ++i) {
		const Vec3f& c = s.verts[i];
		const Vec3f& p = s.verts[i - 1];
		const float dx = c.x - p.x, dy = c.y - p.y, dz = c.z - p.z;
		float n2 = dy * dy;
		n2 = n2 + dx * dx;
		n2 = n2 + dz * dz;
		s.total_length = dsqrt(n2) + s.total_length;
		float sx = px + dx, sy = py + dy, sz = pz + dz;
		px = dx;
		py = dy;
		pz = dz;
		float m2 = sy * sy;
		m2 = m2 + sx * sx;
		m2 = m2 + sz * sz;
		if (m2 != 0.0f && m2 != 1.0f) {
			const float inv = 1.0f / std::sqrt(m2);
			sx = sx * inv;
			sy = sy * inv;
			sz = sz * inv;
		}
		push(s.normals, s.normals_size, Vec3f{sx, sy, sz});
	}
	const Vec3f& a = s.control[s.control.size() - 1];
	const Vec3f& b = s.control[s.control.size() - 2];
	float sx = a.x - b.x, sy = a.y - b.y, sz = a.z - b.z;
	float m2 = sy * sy;
	m2 = m2 + sx * sx;
	m2 = m2 + sz * sz;
	if (m2 != 0.0f && m2 != 1.0f) {
		const float inv = 1.0f / std::sqrt(m2);
		sx = sx * inv;
		sy = sy * inv;
		sz = sz * inv;
	}
	push(s.normals, s.normals_size, Vec3f{sx, sy, sz});
}

// Spline::calc_spline @912f00 (type 3).
void calc_spline(Spline& s) {
	const uint16_t degree0 = s.degree, depth0 = s.depth;
	const int count = int(s.control.size());
	if (count <= int(s.degree)) s.degree = uint16_t(count - 1);
	const int lim = int(float(count) * s.max_control_depth_ratio);
	if (lim != 0 && lim < int(s.depth)) s.depth = uint16_t(lim);
	if (s.depth < s.degree) s.depth = s.degree;
	s.verts.clear();
	generate_bspline(s);
	if (s.verts.empty() || (s.flags & 0x40)) {
		s.depth = depth0;
		s.degree = degree0;
		s.normals.clear();
		return;
	}
	build_normals(s);
	s.depth = depth0;
	s.degree = degree0;
}

// Spline::set_min_seg_length @9125e0: depth = (int)(control polyline length / min_seg) (stored in 16 bits).
void set_min_seg_length(Spline& s, float min_seg) {
	float acc = 0.0f;
	for (size_t i = 1; i < s.control.size(); ++i) {
		const Vec3f& a = s.control[i - 1];
		const Vec3f& b = s.control[i];
		const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
		float n2 = dx * dx;
		n2 = n2 + dy * dy;
		n2 = n2 + dz * dz;
		acc = dsqrt(n2) + acc;
	}
	s.depth = uint16_t(int32_t(acc / min_seg));
}

}  // namespace

void spline_clear(Spline& s) {
	s.degree = 4;
	s.depth = 1;
	s.type = 3;
	s.max_control_depth_ratio = 0.0f;
	s.flags = 0;
	s.total_length = 0.0f;
	s.last_knot = 0;
	s.curr_dist = -1.0f;
	s.search_scan = -1;
	s.next_search_dist = -1.0f;
	s.spline_knots.clear();
	s.weights.clear();
	s.verts.clear();
	s.normals.clear();
	s.control.clear();
}

void spline_calc_from_dir(Spline& s, float min_seg, const Vec3f& start, const Vec3f& mid, const Vec3f& end, const Vec3f& extra) {
	s.weights.clear();  // +0x84 (weights present) := 0
	s.control.clear();
	push(s.control, s.control_size, start);
	push(s.control, s.control_size, mid);
	float l2 = extra.x * extra.x;
	l2 = l2 + extra.y * extra.y;
	l2 = l2 + extra.z * extra.z;
	if (dsqrt(l2) != 0.0f) {
		push(s.control, s.control_size, extra);
		s.degree = 3;
	} else {
		s.degree = 2;
	}
	push(s.control, s.control_size, end);
	set_min_seg_length(s, min_seg);
	s.max_control_depth_ratio = 4.0f;
	calc_spline(s);
}

Vec3f spline_launch_control(const Vec3f& start, float magnitude, float pitch, float yaw_degrees) {
	Vec3f v{0.0f, -magnitude, 0.0f};
	// pitch: whole degrees folded into [0, 360) (negative: 359 - (|pitch| % 360)), the quaternion about x
	int ip = int(pitch);
	if (ip < 0) ip = 0x167 - ((-ip) % 0x168);
	else if (ip >= 0x168) ip %= 0x168;
	{
		const float sn = hd_sin(ip), cs = hd_cos(ip);
		v = rotate(v, Quat{sn, sn * 0.0f, sn * 0.0f, cs});
	}
	// yaw: -degrees in [0, 360) (a positive heading: 359 - (degrees % 360)), the quaternion about z
	int iy = -int(yaw_degrees);
	if (iy < 0) iy = 0x167 - ((-iy) % 0x168);
	else if (iy >= 0x168) iy %= 0x168;
	{
		const float sn = hd_sin(iy), cs = hd_cos(iy);
		v = rotate(v, Quat{sn * 0.0f, sn * 0.0f, sn, cs});
	}
	return Vec3f{v.x + start.x, start.y + v.y, start.z + v.z};
}

}  // namespace ron::sim
