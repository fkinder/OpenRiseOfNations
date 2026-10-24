// Terrain::init @850f70, the part that changes the simulation's world after Map::make (docs/randommap-spec.md "Terrain::init").
// The original builds the terrain heights first (TerrainOut::generate_land: land heights from two fractals blended with the
// distance to the coast, smoothing, the coast generators' influence lines, TerrainOut::build_coastal_data) and then lets the
// heights decide which wcoords are half water (Terrain::check_and_set_halfland), turns the beach cells next to open sea into
// shelf (convert_beach_to_sandy) and keeps the tile combinations legal (verify_land). All of it is ported here from the
// decompiles and the disassembly (re/decomp/mapgen_fid2), without the render-only side effects (vertex streams, RenderHelper
// lists, TerrainVis, the update lists).
//
// Float arithmetic is the original's SSE single precision, operation by operation: no fused multiply-add.
#pragma clang fp contract(off)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

namespace {

constexpr float kLandHeight = 30.0f;     // land_height (global, 30.0)
constexpr float kOceanBase = -303.0f;    // TerrainOut +0x5630: (kOceanBase + kLandHeight) = -273 is the open-sea height
constexpr float kMinPoly = 192.0f;       // MinPolySize: one tcoord in world units
constexpr float kOoWCoord = 1.0f / 768.0f;  // oo_WCoordSize (0x3aaaaaab)
constexpr float kBeachSteepness = 1.0f;

// Fractal::init + get_height (the same midpoint displacement as engine/render/rivers.cpp, which was checked against the
// texture fractal): an (xs + 1) x (ys + 1) byte grid on its own LCG seeded with `seed`.
class TFractal {
public:
	TFractal() = default;
	TFractal(int xs, int ys, int smooth, int flags, uint32_t seed) : col_(ys + 1), flags_(flags), f_(size_t(xs + 1) * size_t(ys + 1), 0) {
		uint32_t s = seed;
		auto rnd = [&](int lo, int hi) {
			s = s * 0x19660Du + 0x3C6EF35Fu;
			return lo + int((int64_t(s & 0xFFFFu) * int64_t(hi - lo)) >> 16);
		};
		const int f1 = flags & 1;
		smooth = std::clamp(smooth, 0, 5);
		auto clamp255 = [](int v) { return uint8_t(std::clamp(v, 0, 255)); };
		for (int L = smooth; L >= 0; L--) {
			const int mask = (1 << (L + 1)) - 1, st = 1 << L, amp = 1 << (7 - smooth + L), span = (1 << (8 - smooth + L)) - 1;
			if (!f1) {
				for (int y = 0; y <= ys; y++) at(xs, y) = at(0, y);
			}
			for (int cx = 0; cx < (xs >> L) + f1; cx++) {
				for (int cy = 0; cy < (ys >> L); cy++) {
					const int X = cx << L, Y = cy << L;
					if (L == smooth) {
						at(X, Y) = uint8_t(rnd(0, 255));
						continue;
					}
					if ((mask & X) == 0) {
						if ((mask & Y) == 0) continue;
						at(X, Y) = clamp255(((at(X, Y + st) + at(X, Y - st) + 1) >> 1) + rnd(0, span) - amp);
					} else if ((mask & Y) == 0) {
						at(X, Y) = clamp255(((at(X + st, Y) + 1 + at(X - st, Y)) >> 1) + rnd(0, span) - amp);
					} else {
						at(X, Y) = clamp255(((at(X - st, Y - st) + at(X + st, Y + st) + at(X - st, Y + st) + at(X + st, Y - st) + 2) >> 2) +
						                    rnd(0, span) - amp);
					}
				}
			}
		}
		if (!f1) {
			for (int y = 0; y <= ys; y++) at(xs, y) = at(0, y);
		}
		xinc_ = double(xs) / double(xs - f1 + 1);
	}
	int operator()(int x, int y) const {
		const double fx = xinc_ * (x + 0.5), fy = 1.0 * (y + 0.5);
		const int ix = int(fx), iy = int(fy);
		const double dx = fx - ix, dy = fy - iy;
		double v = (1 - dx) * (1 - dy) * get(ix, iy) + 0.0;
		v += (1 - dy) * dx * get(ix + 1, iy);
		v += (1 - dx) * dy * get(ix, iy + 1);
		v += get(ix + 1, iy + 1) * dy * dx;
		const int i = std::clamp(int(v), 0, 255);
		return (flags_ & 2) ? (i * 100) >> 8 : i;
	}

private:
	uint8_t& at(int x, int y) { return f_[size_t(x) * size_t(col_) + size_t(y)]; }
	double get(int x, int y) const { return f_[size_t(x) * size_t(col_) + size_t(y)]; }
	int col_ = 1, flags_ = 0;
	double xinc_ = 1;
	std::vector<uint8_t> f_;
};

// InitCoastInfo (PDB, 0x28 bytes): the coast generator state of one wcoord.
struct CoastInfo {
	uint8_t is_valid = 0, has_influence = 0, connected_out = 0, connected_in = 0;
	int16_t to[2][2] = {{0, 0}, {0, 0}};  // connected_to, connected_to2
	std::vector<int> influence;           // tcoord indices (x + 4 xs y)
};

// Bresenham, LineState @a48000 / @a48110.
struct LineState {
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0, cur_x = 0, cur_y = 0, dx = 0, dy = 0, xsign = 1, ysign = 1, type = 4, error = 0, addx = 0, addy = 0;
	bool first = true;
	void init(int ax, int ay, int bx, int by) {
		x1 = bx;
		y1 = by;
		first = true;
		x0 = ax;
		y0 = ay;
		cur_x = ax;
		cur_y = ay;
		dx = bx - ax;
		dy = by - ay;
		if (dx == 0) {
			if (dy == 0) {
				type = 4;
				xsign = 1;
				return;
			}
		} else {
			const int adx = std::abs(dx);
			if (dy == 0) {
				type = 0;
				xsign = dx / adx;
				return;
			}
			const int ady = std::abs(dy);
			dx = adx;
			xsign = (bx - ax) / adx;
			dy = ady;
			ysign = (by - ay) / ady;
			if (adx < ady) {
				type = 3;
				addx = adx * 2;
				addy = (adx - ady) * 2;
				error = adx * 2 - ady;
				return;
			}
			type = 2;
			addx = ady * 2;
			addy = (ady - adx) * 2;
			error = ady * 2 - adx;
			return;
		}
		type = 1;
		ysign = dy / std::abs(dy);
	}
	bool next(int* px, int* py) {
		if (!first) {
			switch (type) {
			case 0:
				if (cur_x == x1) return false;
				cur_x += xsign;
				*px = cur_x;
				*py = cur_y;
				return true;
			case 1:
				if (cur_y == y1) return false;
				cur_y += ysign;
				break;
			case 2:
				if (cur_x == x1) return false;
				cur_x += xsign;
				if (error < 0) {
					error += addx;
				} else {
					error += addy;
					cur_y += ysign;
				}
				*px = cur_x;
				*py = cur_y;
				return true;
			case 3:
				if (cur_y == y1) return false;
				cur_y += ysign;
				if (error < 0) {
					error += addx;
				} else {
					error += addy;
					cur_x += xsign;
				}
				break;
			default: return false;
			}
		} else {
			first = false;
		}
		*px = cur_x;
		*py = cur_y;
		return true;
	}
};

struct TerrainInit {
	Gen& g;
	World& w;
	const int xs, ys, txs, vxs, vys;  // txs = 4 xs (tcoords per row), vxs = 4 xs + 1
	int coast_base = 0;               // tileset group data coast_base
	std::vector<uint16_t> cf;         // CoordInfo.flags per wcoord
	std::vector<float> H;             // master_land_heights, vxs * vys
	std::vector<CoastInfo> ci;
	std::vector<std::array<int, 2>> generators;
	std::vector<uint8_t> adjusted;
	TFractal fa, fb, ftex;

	TerrainInit(Gen& gen)
	    : g(gen), w(gen.w), xs(gen.xs), ys(gen.ys), txs(gen.xs * 4), vxs(gen.xs * 4 + 1), vys(gen.ys * 4 + 1) {}

	WData& W(int x, int y) { return w.w(x, y); }
	bool inb(int x, int y) const { return x >= 0 && y >= 0 && x < xs && y < ys; }
	uint16_t& F(int x, int y) { return cf[size_t(y) * size_t(xs) + size_t(x)]; }
	float& h(int vx, int vy) { return H[size_t(vy) * size_t(vxs) + size_t(vx)]; }

	// ---- TerrainList::add_new_coord_info @84ba10: the CoordInfo flags of every wcoord
	void build_coord_flags() {
		cf.assign(size_t(xs) * size_t(ys), 0);
		for (int y = 0; y < ys; y++) {
			for (int x = 0; x < xs; x++) {
				const WData& d = W(x, y);
				int c7 = (d.flags & 4) ? 3 : d.land;
				uint16_t f = 0;
				bool coastal = false;
				if (c7 == 3) {
					f |= 0x8000;
					coastal = true;
				} else if ((d.flags & 0x100) || (d.flags & 0x400)) {
					coastal = true;
				}
				if (coastal) f |= 4;
				if (c7 != 2) {
					if (c7 == 0) {
						for (int k = 1; k <= 8; k++) {
							const int nx = x + move_x[k], ny = y + move_y[k];
							if (!inb(nx, ny)) continue;
							const WData& n = W(nx, ny);
							if ((n.flags & 4) || n.land == 3) {
								f |= 0x20;
								break;
							}
						}
					}
				} else {
					f |= 0x1000;
					for (int k = 1; k <= 24; k++) {
						const int nx = x + move_x[k], ny = y + move_y[k];
						if (!inb(nx, ny)) continue;
						const WData& n = W(nx, ny);
						if ((n.flags & 4) || n.land == 3) {
							f |= 0x80;
							break;
						}
					}
				}
				F(x, y) = f;
			}
		}
	}

	// ---- TerrainOut::get_vert_codes(x, y, 0) @86bf70
	uint16_t vert_codes(int vx, int vy) {
		const int wx = vx / 4, wy = vy / 4;
		uint16_t f00 = 0, fm0 = 0, f0m = 0, fmm = 0;
		if (wx >= 0 && wy >= 0 && wx < xs && wy < ys) f00 = F(wx, wy);
		if (wx - 1 >= 0 && wy >= 0 && wx - 1 < xs && wy < ys) fm0 = F(wx - 1, wy);
		if (wx >= 0 && wy - 1 >= 0 && wx < xs && wy - 1 < ys) f0m = F(wx, wy - 1);
		if (wx - 1 >= 0 && wy - 1 >= 0 && wx - 1 < xs && wy - 1 < ys) fmm = F(wx - 1, wy - 1);
		uint16_t r = f00;
		if (vx % 4 == 0) {
			if (vy % 4 == 0) return uint16_t(f00 | fm0 | f0m | fmm);
			r = uint16_t(f00 | fm0);
		}
		if (vy % 4 == 0) return uint16_t(r | f00 | f0m);
		return r;
	}

	// ---- TerrainOut::find_closest_coordinfo @86a260
	float find_closest(int vx, int vy, int rings) {
		const int wx = vx >> 2, wy = vy >> 2;
		const float fx = float(vx) * kMinPoly, fy = float(vy) * kMinPoly;
		float best = 1.0e20f;
		int found = -1, ecx = 0;
		for (int ring = 0; ring < rings; ring++) {
			if (found != -1 && ecx == 0) break;
			ecx = ecx == 0 ? 0 : ecx - 1;
			const int start = ring == 0 ? 0 : kMoveRadius[ring - 1];
			const int prev = found;
			for (int k = start; k < kMoveRadius[ring]; k++) {
				const int cx = wx + move_x[k], cy = wy + move_y[k];
				if (cx < 0 || cy < 0 || cx >= xs || cy >= ys) continue;
				if (!(F(cx, cy) & 0x20)) continue;
				for (int sy = 0; sy <= 4; sy++) {
					float dy = float(4 * cy + sy) * kMinPoly;
					dy = dy - fy;
					dy = dy * dy;
					for (int sx = 0; sx <= 4; sx++) {
						float dx = float(4 * cx + sx) * kMinPoly;
						dx = dx - fx;
						dx = dx * dx;
						const float d2 = dx + dy;
						if (best > d2) {
							best = d2;
							found = cx;
						}
					}
				}
			}
			if (found != -1 && prev == -1) ecx = 2;
		}
		if (found == -1) return -1.0f;
		return std::sqrt(best) * kOoWCoord;
	}

	// ---- TerrainOut::generate_land: the vertex heights (determine_land_height_color) and the smoothing
	void generate_land() {
		H.assign(size_t(vxs) * size_t(vys), 0.0f);
		std::vector<int> smooth_list;
		for (int vy = 0; vy < vys; vy++) {
			for (int vx = 0; vx < vxs; vx++) {
				const uint16_t codes = vert_codes(vx, vy);
				float hv;
				if (codes & 0x1000) {
					hv = (kOceanBase + kLandHeight) * kBeachSteepness;
				} else if (codes & 4) {
					hv = 0.0f;
				} else if (codes & 2) {
					hv = 100.0f;
				} else {
					const int a = fa(vx, vy), b = fb(vx, vy);
					float h0 = float(a);
					h0 = h0 * 7.5f;
					h0 = h0 + kLandHeight;
					float t = float(b);
					t = t * 1.875f;
					h0 = h0 + t;
					const float c = find_closest(vx, vy, 4);
					if (c < 0.0f) {
						hv = h0;
					} else {
						float x1 = c * 0.05f;
						const float x0 = 4.0f - c;
						float x2 = c * h0;
						x1 = x1 * 7.5f;
						x1 = x1 * kMinPoly;
						x1 = x1 + kLandHeight;
						x1 = x1 * x0;
						x1 = x1 * 0.25f;
						float r0 = 0.25f * x2;
						hv = r0 + x1;
						if (vx < 4 * xs && vy < 4 * xs) smooth_list.push_back(vx + xs * vy * 4);
					}
				}
				h(vx, vy) = hv;
			}
		}
		for (int idx : smooth_list) {
			const int x = idx % (xs * 4), y = idx / (xs * 4);
			smooth_tcoord(x, y);
			if ((x | y) & 1) smooth_tcoord(x, y);
		}
	}

	// ---- TerrainOut::smooth_tcoord @86c3a0
	void smooth_tcoord(int px, int py) {
		float loc[16] = {};
		for (int j = 0; j < 4; j++) {
			const int y = py - 1 + j;
			for (int i = 0; i < 4; i++) {
				const int x = px - 1 + i;
				if (x >= 0 && y >= 0 && x < txs && y < ys * 4) loc[i + j * 4] = h(x, y);
			}
		}
		int base = 1;  // index of loc + 1
		int row_y = py;
		for (int rr = 0; rr < 2; rr++) {
			int col_x = px;
			int p = base;
			for (int cc = 0; cc < 2; cc++) {
				int count = 0;
				float sum = 0.0f;
				int q = p;
				for (int k = 0; k < 3; k++) {
					if (loc[q - 1] != 0.0f) {
						count++;
						sum = sum + loc[q - 1];
					}
					if (loc[q] != 0.0f) {
						count++;
						sum = sum + loc[q];
					}
					if (loc[q + 1] != 0.0f) {
						count++;
						sum = sum + loc[q + 1];
					}
					q += 4;
				}
				if (count != 0) {
					float& cell = h(col_x, row_y);
					if (0.0f < cell) cell = sum / float(count);
				}
				p++;
				col_x++;
			}
			row_y++;
			base += 4;
		}
	}

	// ---- TerrainOut::mark_valid_coastal_influencers @869070
	void mark_valid_coastal_influencers() {
		ci.assign(size_t(xs) * size_t(ys), CoastInfo());
		generators.clear();
		for (int y = 0; y < ys; y++) {
			for (int x = 0; x < xs; x++) {
				if (!(F(x, y) & 0x20)) continue;
				uint16_t nf[8] = {};
				for (int k = 0; k < 8; k++) {
					const int nx = x + move_x[k + 1], ny = y + move_y[k + 1];
					if (inb(nx, ny)) nf[k] = F(nx, ny);
				}
				bool b = false;
				if ((nf[1] & 0x20) && (nf[7] & 0x20) && (nf[0] & 4)) b = true;
				if ((nf[1] & 0x20) && (nf[3] & 0x20) && (nf[2] & 4)) b = true;
				if ((nf[5] & 0x20) && (nf[7] & 0x20) && (nf[6] & 4)) b = true;
				if (!(((nf[5] & 0x20) && (nf[3] & 0x20) && (nf[4] & 4)) || b)) {
					ci[size_t(y) * size_t(xs) + size_t(x)].is_valid = 1;
					generators.push_back({x, y});
				}
			}
		}
	}

	int jitter(int x, int y) {
		const int b = ftex(x, y);
		float f = float(b);
		f = f * 0.16f;
		if (!(f < 15.5f)) f = 15.0f;
		return int(f);
	}

	// ---- TerrainOut::mark_coast_vert_influence @868ca0
	void mark_coast_vert_influence() {
		for (const auto& gen : generators) {
			const int x = gen[0], y = gen[1];
			CoastInfo* cur = &ci[size_t(y) * size_t(xs) + size_t(x)];
			const int jc = jitter(x, y);
			int jn = jc;  // local_14: the jitter of the most recently connected neighbour (or the cell's own)
			for (int k = 0; k < 8; k++) {
				const int nx = x + move_x[k + 1], ny = y + move_y[k + 1];
				if (!inb(nx, ny)) continue;
				CoastInfo& n = ci[size_t(ny) * size_t(xs) + size_t(nx)];
				if (!n.is_valid) continue;
				if (n.connected_out != 0) {
					if (n.to[0][0] == nx && n.to[0][1] == ny) continue;
				}
				if (n.connected_out == 2 && n.to[1][0] == nx && n.to[1][1] == ny) continue;
				if (cur->connected_out == 2) continue;
				n.connected_in = 1;
				cur->connected_out++;
				const int idx = cur->connected_out != 1 ? 1 : 0;
				cur->to[idx][0] = int16_t(nx);
				cur->to[idx][1] = int16_t(ny);
				jn = jitter(nx, ny);
			}
			CoastInfo* owner = cur;
			int prev_wx = -1, prev_wy = -1;
			for (int line = 0; line < 2; line++) {
				if (cur->connected_out == 1 && line == 1) break;
				int tx, ty;
				if (cur->connected_out == 0) {
					tx = x;
					ty = y;
				} else if (line == 0) {
					tx = cur->to[0][0];
					ty = cur->to[0][1];
				} else {
					tx = cur->to[1][0];
					ty = cur->to[1][1];
				}
				LineState ls;
				ls.init((jc & 3) + x * 4, (jc >> 2) + y * 4, (jn & 3) + tx * 4, (jn >> 2) + ty * 4);
				prev_wx = -1;
				prev_wy = -1;
				int px, py;
				while (ls.next(&px, &py)) {
					const int idx = px + txs * py;
					owner->influence.push_back(idx);
					const int wx = px >> 2, wy = py >> 2;
					if (wx != prev_wx && wy != prev_wy) {
						owner = &ci[size_t(wy) * size_t(xs) + size_t(wx)];
						owner->has_influence = 1;
						prev_wx = wx;
						prev_wy = wy;
					}
				}
			}
		}
	}

	// ---- TerrainOut::adjust_aquatic_height @868820
	void adjust_aquatic_height() {
		adjusted.assign(size_t(xs) * size_t(ys), 0);
		for (int wy = 0; wy < ys; wy++) {
			for (int wx = 0; wx < xs; wx++) {
				const WData& d = W(wx, wy);
				const int land = d.land;
				bool scan;
				if (d.flags & 4) {
					F(wx, wy) |= 8;
					scan = true;
				} else if (land == 0) {
					scan = ci[size_t(wy) * size_t(xs) + size_t(wx)].has_influence != 0;
				} else if (land == 2) {
					F(wx, wy) |= 8;
					scan = true;
				} else {
					scan = true;
				}
				if (!scan) continue;
				bool any = false;
				for (int k = 0; k < 25 && !any; k++) {
					const int nx = wx + move_x[k], ny = wy + move_y[k];
					if (inb(nx, ny) && ci[size_t(ny) * size_t(xs) + size_t(nx)].has_influence) any = true;
				}
				if (!any) continue;
				if (!(d.flags & 4) && d.land == 2) F(wx, wy) |= 8;
				for (int r = 0; r < 5; r++) {
					for (int c = 0; c < 5; c++) {
						long best = 0x7fffffff;
						int best_idx = 0;
						for (int k = 0; k < 25; k++) {
							const int nx = wx + move_x[k], ny = wy + move_y[k];
							if (!inb(nx, ny)) continue;
							const CoastInfo& n = ci[size_t(ny) * size_t(xs) + size_t(nx)];
							for (int idx : n.influence) {
								const int ty = idx / txs, tx = idx % txs;
								const long ddy = long(4 * wy + r) - ty, ddx = long(4 * wx + c) - tx;
								const long d2 = ddx * ddx + ddy * ddy;
								if (d2 < best) {
									best = d2;
									best_idx = idx;
								}
							}
						}
						const int ty = best_idx / txs, tx = best_idx % txs;
						const int vx = 4 * wx + c, vy = 4 * wy + r;
						float fx = float(tx);
						fx = fx + 0.5f;
						fx = fx * kMinPoly;
						float sx = float(vx);
						sx = sx * kMinPoly;
						float fyy = float(ty);
						fyy = fyy + 0.5f;
						fyy = fyy * kMinPoly;
						float sy = float(vy);
						sy = sy * kMinPoly;
						float ddx = fx - sx, ddy = fyy - sy;
						ddx = ddx * ddx;
						ddy = ddy * ddy;
						float dist = std::sqrt(ddx + ddy);
						dist = dist * kOoWCoord;
						dist = std::min(dist, 10000.0f);
						float t = kOceanBase + kLandHeight;
						float v = dist * dist;
						v = v * 0.11111111f;
						v = v * t;
						v = v + kLandHeight;
						if (0.0f > v) v = v * kBeachSteepness;
						h(vx, vy) = v;
						adjusted[size_t(wy) * size_t(xs) + size_t(wx)] = 1;
					}
				}
			}
		}
	}

	// ---- TerrainOut::relax_aquatic_verts @868470
	void relax_aquatic_verts() {
		for (int pass = 0; pass < 5; pass++) {
			for (int wy = 0; wy < ys; wy++) {
				for (int wx = 0; wx < xs; wx++) {
					if (!adjusted[size_t(wy) * size_t(xs) + size_t(wx)]) continue;
					if (wx == 0 || wy == 0 || wx == xs - 1 || wy == ys - 1) continue;
					for (int r = 0; r < 5; r++) {
						for (int c = 0; c < 5; c++) {
							const int row = 4 * wy + r, col = 4 * wx + c;
							float v = h(col, row + 1);
							v = v + h(col, row - 1);
							v = v + h(col - 1, row);
							v = v + h(col + 1, row);
							v = v + h(col, row);
							v = v * 0.2f;
							h(col, row) = v;
						}
					}
				}
			}
		}
	}

	// ---- the tile combination rules (TileSet::is_combo_acceptable @87abe0)
	int combo_acceptable(int l1, int s1, int l2, int s2) const {
		if (l1 == l2) {
			if (std::abs(s2 - s1) < 2) return (s2 <= s1) + 1;
		} else {
			if (l1 == 0) {
				if (l2 == 1) return s1 == coast_base ? 1 : 0;
			} else if (l1 == 1 && l2 == 0) {
				return s2 == coast_base ? 2 : 0;
			}
			if (l2 == 2) return 1;
			if (l2 < l1) {
				if (s2 == g.tile_counts[size_t(l2)] - 1) return s1 != 0 ? 0 : 2;
			} else if (s2 == 0) {
				return s1 == g.tile_counts[size_t(l1)] - 1 ? 1 : 0;
			}
		}
		return 0;
	}

	// ---- Terrain::verify_land @84d8a0
	int depth = 0;
	void verify_land(int x, int y, int* pl, int* ps) {
		depth++;
		if (depth > 500) {
			depth--;
			return;
		}
		char changed[8] = {};
		for (int k = 1; k <= 8; k++) {
			const int nx = x + move_x[k], ny = y + move_y[k];
			if (!inb(nx, ny)) continue;
			WData& n = W(nx, ny);
			const int nl = n.land, ns = n.land_sub;
			if (combo_acceptable(*pl, *ps, nl, ns) != 0) continue;
			if (nl == 2) {
				WData& c = W(x, y);
				c.land_sub = uint8_t(coast_base);
				c.land = 0;
				int l = 0, s = coast_base;
				verify_land(x, y, &l, &s);
				*pl = l;
				*ps = s;
			} else if (nl == 1) {
				WData& c = W(x, y);
				c.land_sub = uint8_t(coast_base);
				c.land = 0;
				int l = 0, s = coast_base;
				verify_land(x, y, &l, &s);
				*pl = l;
				*ps = s;
			} else {  // nl == 0
				int tgt = ns;
				if (std::abs(*ps - ns) > 1) tgt = ns <= *ps ? ns + 1 : ns - 1;
				if (*pl == 0) {
					if (ns != tgt && ns != *ps) {
						n.land_sub = uint8_t(tgt);
						n.land = 0;
						changed[k - 1] = 1;
					}
				} else if (*pl == 1 || *pl == 2) {
					n.land_sub = uint8_t(coast_base);
					n.land = 0;
					changed[k - 1] = 1;
				}
			}
		}
		for (int k = 1; k <= 8; k++) {
			const int nx = x + move_x[k], ny = y + move_y[k];
			if (!inb(nx, ny) || !changed[k - 1]) continue;
			int l = W(nx, ny).land, s = W(nx, ny).land_sub;
			verify_land(nx, ny, &l, &s);
		}
		depth--;
	}

	// ---- Terrain::spot_update_land @84fa50
	void spot_update_land(int x, int y, int nl, int ns) {
		if (!inb(x, y)) return;
		WData& d = W(x, y);
		if (d.land == nl && d.land_sub == ns) return;
		d.land_sub = uint8_t(ns);
		d.land = int8_t(nl);
		int l = nl, s = ns;
		verify_land(x, y, &l, &s);
	}

	// ---- region bookkeeping: the lists are rebuilt at the end, only WData.region / region2 are kept in step
	void set_region(int x, int y, int region, int region2) {
		WData& d = W(x, y);
		d.region = int16_t(region);
		d.region2 = int16_t(region2);
	}

	// ---- Terrain::mark_halfwater @84ece0
	void mark_halfwater(int x, int y, int tx, int ty, int reg_a, int reg_b) {
		WData& d = W(x, y);
		bool fresh = false;
		if (!(d.flags & 0x100)) fresh = true;
		uint16_t& tm = w.t(tx, ty);
		tm = uint16_t((tm & 0xffef) | 0x20);
		d.flags |= 0x100;
		int nwater = 0;
		for (int i = 0; i < 16; i++) {
			if ((w.t(4 * x + (i & 3), 4 * y + (i >> 2)) & 0x30) == 0x20) nwater++;
		}
		const bool all = nwater == 16;
		int land_reg, sea_reg;
		if (reg_a < 0x41) {
			sea_reg = reg_b;
			land_reg = reg_a;
		} else {
			sea_reg = reg_a;
			land_reg = reg_b;
		}
		if (!fresh) {
			if (!all) return;
			d.flags &= 0xfeff;
			set_region(x, y, sea_reg, 0);
			d.land = 2;
			d.land_sub = 0;
			int l = 2, s = 0;
			verify_land(x, y, &l, &s);
		} else {
			set_region(x, y, land_reg, sea_reg);
			d.land = 0;
			d.land_sub = 0;
			int l = 0, s = 0;
			verify_land(x, y, &l, &s);
		}
	}

	// ---- Terrain::clear_halfland_wcoord @84e500
	void clear_halfland(int x, int y) {
		for (int i = 0; i < 16; i++) {
			uint16_t& m = w.t(4 * x + (i & 3), 4 * y + (i >> 2));
			if ((m & 0x30) == 0x20) m = uint16_t(m & 0xffcf);
		}
		W(x, y).flags &= 0xfeff;
		for (int i = 0; i < 16; i++) w.t(4 * x + (i & 3), 4 * y + (i >> 2)) &= 0xfbff;
		// (a cell that was not half water also leaves region[region2]'s list: its region2 is 0, nothing to do here)
	}

	bool tcoord_below(int tx, int ty) {
		const float lim = -30.0f;
		return h(tx, ty) < lim && h(tx + 1, ty) < lim && h(tx, ty + 1) < lim && h(tx + 1, ty + 1) < lim;
	}

	// ---- Terrain::check_and_set_halfland_wcoord @84dec0
	void check_and_set_halfland_wcoord(int x, int y, std::vector<int>& queue) {
		if ((F(x, y) & 0x84) == 0) return;
		clear_halfland(x, y);
		const int cur_region = W(x, y).region;
		int other = -1, last_r2 = -1;
		for (int k = 1; k <= 24; k++) {
			const int nx = x + move_x[k], ny = y + move_y[k];
			if (!inb(nx, ny)) continue;
			const WData& n = W(nx, ny);
			if (!(n.flags & 0x100)) {
				if (n.region == cur_region) continue;
				other = n.region;
				break;
			} else {
				if (n.region2 != cur_region) last_r2 = n.region2;
			}
		}
		int count = 0;
		for (int ty = 0; ty < 4; ty++) {
			for (int tx = 0; tx < 4; tx++) {
				const int px = 4 * x + tx, py = 4 * y + ty;
				if (!tcoord_below(px, py)) continue;
				count++;
				if (other == -1) {
					if (last_r2 == -1) continue;
					other = last_r2;
				}
				mark_halfwater(x, y, px, py, cur_region, other);
			}
		}
		if (count != 0) {
			spot_update_land(x, y, 1, 0);
			return;
		}
		WData& d = W(x, y);
		int nl = d.land, ns = d.land_sub;
		const int old_l = nl;
		if (old_l == 2) {
			nl = 0;
			ns = coast_base;
		}
		if (ns >= 0) d.land_sub = uint8_t(ns);
		if (nl >= 0) d.land = int8_t(nl);
		d.flags &= 0xffc3;
		F(x, y) &= 0xfffb;
		for (int k = 1; k <= 4; k++) {
			const int nx = x + orthog_x[k], ny = y + orthog_y[k];
			if (!inb(nx, ny)) continue;
			WData& n = W(nx, ny);
			if (n.land != 0 && !(n.flags & 4)) {
				n.flags |= 4;
				F(nx, ny) |= 4;
				queue.push_back(ny * xs + nx);
				break;
			}
		}
		{
			int l = nl, s = ns;
			verify_land(x, y, &l, &s);
		}
		const int cr = W(x, y).region;
		for (int k = 1; k <= 24; k++) {
			const int nx = x + move_x[k], ny = y + move_y[k];
			if (!inb(nx, ny)) continue;
			const WData& n = W(nx, ny);
			if (!(n.flags & 0x100) && n.region != cr) {
				W(x, y).region = n.region;
				break;
			}
		}
	}

	void mark_ocean_tcoords(int x, int y) {
		const WData& d = W(x, y);
		if (!(d.flags & 0x100) && (d.land == 1 || d.land == 2)) {
			for (int i = 0; i < 16; i++) {
				uint16_t& m = w.t(4 * x + (i & 3), 4 * y + (i >> 2));
				m = uint16_t((m & 0xffef) | 0x20);
			}
		}
	}

	// ---- Terrain::check_and_set_coastal @84dd70
	void check_and_set_coastal(int x, int y) {
		const int twx = w.tile_xs, twy = w.tile_ys;
		for (int j = 0; j < 4; j++) {
			for (int i = 0; i < 4; i++) {
				const int tx = i + x * 4, ty = j + y * 4;
				if ((w.t(tx, ty) & 0x30) != 0x20) continue;
				for (int k = 1; k <= 8; k++) {
					const int nx = tx + move_x[k], ny = ty + move_y[k];
					if (nx < 0 || ny < 0 || nx >= twx || ny >= twy) continue;
					if ((w.t(nx, ny) & 0x30) == 0x20) continue;
					for (int m = 1; m <= 48; m++) {
						const int ax = nx + move_x[m], ay = ny + move_y[m];
						if (ax >= 0 && ay >= 0 && ax < twx && ay < twy && (w.t(ax, ay) & 0x30) == 0x20) w.t(ax, ay) |= 0x400;
					}
					break;
				}
			}
		}
	}

	// ---- Terrain::check_and_set_halfland @84e610
	void check_and_set_halfland() {
		std::vector<int> queue;
		for (int y = 0; y < ys; y++) {
			for (int x = 0; x < xs; x++) {
				check_and_set_halfland_wcoord(x, y, queue);
				mark_ocean_tcoords(x, y);
			}
		}
		while (!queue.empty()) {
			std::vector<int> cur;
			cur.swap(queue);
			for (int idx : cur) {
				const int x = idx % xs, y = idx / xs;
				check_and_set_halfland_wcoord(x, y, queue);
				mark_ocean_tcoords(x, y);
			}
		}
		for (int y = 0; y < ys; y++) {
			for (int x = 0; x < xs; x++) check_and_set_coastal(x, y);
		}
		for (int y = 0; y < ys; y++) {
			for (int x = 0; x < xs; x++) {
				WData& d = W(x, y);
				if (d.land != 0) continue;
				const int reg = d.region;
				if (reg <= 0x40) continue;
				if (d.region2 == 0) {
					for (int k = 1; k <= 24; k++) {
						const int nx = x + move_x[k], ny = y + move_y[k];
						if (!inb(nx, ny)) continue;
						const WData& n = W(nx, ny);
						if (n.land == 0 && n.region <= 0x40 && n.region != reg) {
							d.region = n.region;
							break;
						}
					}
				} else {
					const int r2 = d.region2;
					d.region = int16_t(r2);
					d.region2 = int16_t(reg);
				}
			}
		}
	}

	// ---- Terrain::convert_beach_to_sandy @84b8e0
	void convert_beach_to_sandy() {
		for (int y = 0; y < ys; y++) {
			for (int x = 0; x < xs; x++) {
				if (W(x, y).land != 0) continue;
				for (int k = 1; k <= 8; k++) {
					const int nx = x + move_x[k], ny = y + move_y[k];
					if (!inb(nx, ny)) continue;
					if (W(nx, ny).land == 2) {
						spot_update_land(x, y, 1, 0);
						break;
					}
				}
			}
		}
	}

	void run() {
		coast_base = g.tgd.coast_base;
		fa = TFractal(txs + 1, g.ys * 4 + 1, 5, 2, g.world_seed);
		fb = TFractal(txs + 1, g.ys * 4 + 1, 3, 2, g.world_seed * 2u);
		ftex = TFractal(xs + 1, ys + 1, 0, 2, g.world_seed);
		build_coord_flags();
		generate_land();
		mark_valid_coastal_influencers();
		mark_coast_vert_influence();
		adjust_aquatic_height();
		relax_aquatic_verts();
		check_and_set_halfland();
		convert_beach_to_sandy();
	}
};

}  // namespace

void Gen::terrain_init() {
	TerrainInit ti(*this);
	ti.run();
	res.heights = ti.H;
	res.height_vxs = ti.vxs;
	res.height_vys = ti.vys;
	// regions: the half-water wcoords joined the land regions and left the sea ones
	for (WData& d : w.wdata) d.region2 = 0;
	rg.clear_all(w);
	rg.find_all(w);
	sync_regions();
}

}  // namespace ron::sim::mapgen
