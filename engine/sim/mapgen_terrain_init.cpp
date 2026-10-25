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
#include <functional>

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
				const bool valid = !(((nf[5] & 0x20) && (nf[3] & 0x20) && (nf[4] & 4)) || b);
				if (valid) {
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
				// The original keeps the CoastInfo pointer in EBX: `cur` for the first line, but the point loop below leaves the CURRENT OWNER
				// in EBX (68fd7 / 68fdf), and the loop's head (68e80) reads connected_out and to[] through EBX again. So the second line is
				// chosen by the owner cell the first line ended in: a line whose owner never switched (the owner switches only on a step that
				// changes both wx and wy, i.e. mostly diagonal links) sees the generator's own data, any other sees its neighbour's -- the
				// diagonal-first-link residue of the lake-corner shelf heights (docs/randommap-spec.md, fourth pass).
				const CoastInfo* src = line == 0 ? cur : owner;
				if (src->connected_out == 1 && line == 1) break;
				int tx, ty;
				if (src->connected_out == 0) {
					tx = x;
					ty = y;
				} else if (line == 0) {
					tx = src->to[0][0];
					ty = src->to[0][1];
				} else {
					tx = src->to[1][0];
					ty = src->to[1][1];
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


	// ---- Terrain::move_goody @84a3d0 / Terrain::move_ocean_goody @84aec0 (randommap-resources section 9): whatever stands on a wcoord
	// that stops being plain land (a cell turning half water, a river trough) is moved to the first acceptable cell of the ring
	// move_x[9..24]: the goody box (land cells), the resource (water cells for the ocean variant), the oil derrick.
	int find_goody_at(int x, int y) {
		const WData& d = W(x, y);
		if (d.down == -3 && d.down_who >= 0 && size_t(d.down_who) < g.c.items.size() && g.c.items[size_t(d.down_who)].alive()) return d.down_who;
		return -1;
	}
	int find_good_at(int x, int y) {  // ObjectsData::find_good_at(w, x, y, -1, 1, 0): the cell's good terminal, live and not oil
		const WData& d = W(x, y);
		if (d.down == -2 && d.down_who >= 0 && size_t(d.down_who) < g.c.goods.size()) {
			const GoodObj& go = g.c.goods[size_t(d.down_who)];
			if (go.alive() && go.type != kOilGood) return d.down_who;
		}
		return -1;
	}
	static void clear_down(WData& d) {
		if (d.down < 0) {
			d.down = -1;
			d.down_who = -1;
		}
	}
	// TerrainOut::is_clear_land @874b60 for the oil search (mask 0x80, | 8 for a derrick standing in water)
	bool is_clear_land_oil(int x, int y, unsigned mask) {
		const WData& d = W(x, y);
		const uint16_t f = d.flags;
		if ((f & 0x10) && !(mask & 0x40)) return false;
		if ((f & 0x20) && !(mask & 1)) return false;
		if ((f & 8) && !(mask & 4)) return false;
		if ((f & 0x800) && !(mask & 0x100)) return false;
		if (!(f & 0x100) && (d.land == 1 || d.land == 2) && !(mask & 8)) return false;
		if ((f & 0x1000) && !(mask & 0x10)) return false;
		if ((f & 0x40) && !(mask & 0x20)) return false;
		if (!(mask & 0x80)) {
			if (f & 0x8000) return false;
			if (find_good_at(x, y) >= 0) return false;
		}
		if (!(mask & 0x165) && d.blocked != 0) return false;
		return !(F(x, y) & 4);  // the per-wcoord record's coastal bit (mask & 2 is never set here)
	}
	int new_good_slot() {
		for (size_t i = 0; i < g.c.goods.size(); i++) {
			if (!g.c.goods[i].alive()) return int(i);
		}
		g.c.goods.push_back(GoodObj());
		return int(g.c.goods.size()) - 1;
	}
	void move_goody(int x, int y, bool ocean) {
		WData& d0 = W(x, y);
		int goody = -1;
		if (!(!(d0.flags & 0x100) && (d0.land == 1 || d0.land == 2))) goody = find_goody_at(x, y);
		const bool oil = (d0.flags & 0x800) != 0;
		const int good = find_good_at(x, y);
		if (goody == -1 && !oil && good < 0) return;
		int land[48];
		for (int k = 0; k < 48; k++) {
			const int nx = x + move_x[k + 1], ny = y + move_y[k + 1];
			land[k] = inb(nx, ny) ? W(nx, ny).land : -1;
		}
		auto interior = [&](int cx, int cy) { return cx != 0 && cy != 0 && cx != xs - 1 && cy != xs - 1; };
		if (goody != -1) {
			GoodObj& it = g.c.items[size_t(goody)];
			// Item::close: the goody flag of its cell, the down list
			{
				WData& od = W(wcoord_of(it.x), wcoord_of(it.y));
				od.flags &= 0x7fff;
				clear_down(od);
			}
			it.flags = uint8_t(it.flags & ~objflag::kValid);
			for (int j = 0; j < 16; j++) {
				const int k = 9 + j;
				const int cx = x + move_x[k], cy = y + move_y[k];
				if (land[k - 1] == -1 || !interior(cx, cy) || land[k - 1] != 0) continue;
				const WData& c = W(cx, cy);
				const uint16_t f = c.flags;
				if (f & 0x100) continue;
				if ((f & 0x10) || (f & 0x28) || (f & 0x800)) continue;
				if ((f & 0x1000) || (f & 0x40) || (f & 0x8000)) continue;
				if (find_good_at(cx, cy) >= 0 || c.blocked != 0 || (F(cx, cy) & 4) || g.has_south_forest(cx, cy)) continue;
				it.x = cx * 768 + 0x180;
				it.y = cy * 768 + 0x180;
				it.flags = uint8_t(it.flags | objflag::kValid);
				item_init(w, it.x, it.y);
				WData& nd = W(cx, cy);
				nd.down = -3;
				nd.down_who = int16_t(goody);
				for (auto& pi : g.placed_items) {
					if (pi[0] == x && pi[1] == y) {
						pi[0] = cx;
						pi[1] = cy;
					}
				}
				break;
			}
		}
		if (good >= 0) {
			GoodObj& go = g.c.goods[size_t(good)];
			const int type = go.type;
			const Type& gt = g.rules.types[size_t(type)];
			// Good::close: the resource tcoords, the blocking, the wcoord resource flag, the down list, the herd
			{
				const int x0 = tcoord_of(go.x) - gt.x_size / 2, y0 = tcoord_of(go.y) - gt.y_size / 2;
				for (int tx = x0; tx < x0 + gt.x_size; tx++) {
					for (int ty = y0; ty < y0 + gt.y_size; ty++) {
						if (!w.in_t(tx, ty)) continue;
						w.t(tx, ty) &= uint16_t(~tmask::kResource);
						if (gt.block) w.set_blocked_at(tx, ty, false);
					}
				}
				WData& od = W(wcoord_of(go.x), wcoord_of(go.y));
				od.flags &= 0xfffe;
				clear_down(od);
			}
			go.flags = uint8_t(go.flags & ~objflag::kValid);
			int herd_at = -1, herd_type = 0;
			for (size_t hi = 0; hi < g.herds.size(); hi++) {
				if (g.herds[hi].unit_type >= 0 && g.herds[hi].good_slot == good) herd_at = int(hi);
			}
			if (herd_at >= 0) {
				herd_type = g.herds[size_t(herd_at)].unit_type;
				g.herds[size_t(herd_at)].unit_type = -1;  // Herd::close: the slot is free until Herds::init_herd takes it
			}
			for (int j = 0; j < 16; j++) {
				const int k = 9 + j;
				const int cx = x + move_x[k], cy = y + move_y[k];
				const int l = land[k - 1];
				if (l == -1 || !interior(cx, cy)) continue;
				const WData& c = W(cx, cy);
				const uint16_t f = c.flags;
				if (ocean) {
					if (!(l == 2 || l == 1) || (f & 0x100)) continue;
					if ((f & 0x10) || (f & 0x68) || (f & 0x1800) || (f & 0x8000)) continue;
				} else {
					if (l != 0 || (f & 0x100)) continue;
					if ((f & 0x10) || (f & 0x28) || (f & 0x800) || (f & 0x1000) || (f & 0x40) || (f & 0x8000)) continue;
				}
				if (find_good_at(cx, cy) >= 0 || c.blocked != 0 || (!ocean && (F(cx, cy) & 4)) || g.has_south_forest(cx, cy)) continue;
				// GoodTypeData::snap_center, Objects::init_good (first free slot)
				const int slot = new_good_slot();
				GoodObj& ng = g.c.goods[size_t(slot)];
				ng = GoodObj();
				ng.type = type;
				ng.x = cx * 768 + ((gt.x_size & 1) ? 0x120 : 0x180);
				ng.y = cy * 768 + ((gt.y_size & 1) ? 0x120 : 0x180);
				ng.flags = objflag::kValid;
				good_init(w, type, gt, ng.x, ng.y);
				WData& nd = W(cx, cy);
				nd.down = -2;
				nd.down_who = int16_t(slot);
				for (auto& pg : g.placed_goods) {
					if (pg[0] == x && pg[1] == y && pg[2] == type) {
						pg[0] = cx;
						pg[1] = cy;
					}
				}
				if (herd_at >= 0) {
					HerdRec& hr = g.herds[size_t(herd_at)];  // Herds::init_herd takes the first free slot: the one just closed
					hr.wx = cx;
					hr.wy = cy;
					hr.unit_type = herd_type;
					hr.good_slot = slot;
				}
				break;
			}
		}
		if (oil) {
			g.set_oil_at(x, y, false);
			W(x, y).flags &= 0xfff7;
			int want_a, want_b;
			unsigned mask = 0;
			const int here = W(x, y).land;
			if (here == 1 || here == 2) {
				want_a = 2;
				want_b = 1;
				mask = 8;
			} else {
				want_a = want_b = here;
			}
			for (int k = 9; k <= 48; k++) {
				const int cx = x + move_x[k], cy = y + move_y[k];
				const int l = land[k - 1];
				if (l == -1 || !interior(cx, cy)) continue;
				if (!(l == want_a || l == want_b) || (W(cx, cy).flags & 0x100)) continue;
				if (!is_clear_land_oil(cx, cy, mask | 0x80)) continue;
				if (g.has_south_forest(cx, cy)) continue;
				g.set_oil_at(cx, cy, true);
				// Terrain::move_goody (land variant) @84ad29: `if (TerrainOil enabled, Terrain +0x4d7c, set by Terrain::init @851770)
				// TerrainOil::add_rocky_patch(new cell)` = the oil plateau (terraform_for_building) at the NEW place, run in the middle of the river
				// sample that moved the derrick; the ocean variant (move_ocean_goody) has no such call.
				if (!ocean) add_rocky_patch(cx, cy);
				// the ocean variant always sets the rock flag; the land variant only on land
				if (ocean || mask != 8) W(cx, cy).flags |= 8;
				break;
			}
		}
	}

	// ---- Terrain::mark_halfwater @84ece0
	void mark_halfwater(int x, int y, int tx, int ty, int reg_a, int reg_b) {
		if (!(W(x, y).flags & 0x100)) move_goody(x, y, true);  // Terrain::move_ocean_goody before the cell turns half water
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
				move_goody(x, y, true);  // @84e4a5: the cell joined another region: Terrain::move_ocean_goody
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

	// =====================================================================================================================
	// RiversOut::calculate_all @888820 (param_1 = 0, a new game): for every River the control spline (ensure_ocean_end,
	// Spline::calc_spline), the sections (RiverOut::create_sections @884e00 -> RiverSection::init @88c360 ->
	// create_trough_and_water @889f20 -> create_trough_edge @889660) and, per section, the trough: RiverSectionOut::
	// terraform_from_river_vert @88b360 averages the four corner heights of the tile under each of the 3 x 9 samples with the trough
	// depth and `World::set_river_at` marks (tdata 0x800, WData 0x1000) every tile of the 36-offset neighbourhood that has a corner
	// below zero. After the sections: TerrainOut::ensure_45_deg_slope @869890 and two smooth_tcoord passes over the touched
	// vertices, then Terrain::spot_update_land_batch(riverbase) on the wcoords the terraform visited.
	// The geometry (B-spline, arc-length walker, widths from the River fractal) follows engine/render/rivers.cpp, which was
	// checked against the original's meshes; docs/randommap-rivers.md section 9.
	using V3 = std::array<float, 3>;
	static constexpr float kMaxWidth = 0.75f;        // RiversOut +0x5c (rivers._92)
	static constexpr float kMaxDepth = -60.0f;       // rivers._252
	static constexpr float kSectionsPerWcoord = 1.7f;  // rivers._256

	std::vector<uint8_t> rv_bits;                    // RiverOut +0x298: tcoords already queued for smoothing
	std::vector<int> rv_list;                        // RiverOut +0xac: tcoord indices (x + 4 xs y) queued for smoothing
	std::vector<std::array<int, 2>> rv_wcoords;      // RiverOut +0x280: wcoords the terraform visited

	// RON_RIVER_TRACE=1: one stderr line per river, per terraform sample (APPLY / SKIP and why) and per tile marked 0x800 (river, section, k, tile, cell state)
	const bool trace_on = std::getenv("RON_RIVER_TRACE") != nullptr;
	int tr_river = 0, tr_section = 0, tr_k = 0, tr_sample = 0;

	bool t_inb(int tx, int ty) const { return tx >= 0 && ty >= 0 && tx < txs && ty < ys * 4; }

	void rv_set_river_at(int tx, int ty) {  // World::set_river_at @6b1f80 with param_3 = 1
		w.t(tx, ty) = uint16_t(w.t(tx, ty) | 0x800);
		W(tx >> 2, ty >> 2).flags |= 0x1000;
	}

	// RiverSectionOut::terraform_from_river_vert @88b360
	void terraform_from_river_vert(float x, float y, float z) {
		const int sample_no = tr_sample++;
		const int tx = tcoord_of(int(x)), ty = tcoord_of(int(y));
		const int wx = tx >> 2, wy = ty >> 2;
		const uint16_t tm = w.t(tx, ty);
		bool skip_cell = ((tm & 0x30) == 0x20) || W(wx, wy).land == 1;
		for (int i = 1; i <= 8; i++) {
			const int nx = wx + move_x[i], ny = wy + move_y[i];
			if (!inb(nx, ny)) continue;
			const WData& n = W(nx, ny);
			if (n.land == 2 || n.land == 1 || (n.flags & 0x100)) {
				if (trace_on) std::fprintf(stderr, "RIVER_TRACE sample#%d river %d section %d k %d tile (%d,%d) SKIP neighbour %d (%d,%d)\n", sample_no, tr_river, tr_section, tr_k, tx, ty, i, nx, ny);
				return;
			}
		}
		if (skip_cell || (tm & 3) == 2) {
			if (trace_on) std::fprintf(stderr, "RIVER_TRACE sample#%d river %d section %d k %d tile (%d,%d) SKIP cell\n", sample_no, tr_river, tr_section, tr_k, tx, ty);
			return;
		}
		if (trace_on) std::fprintf(stderr, "RIVER_TRACE sample#%d river %d section %d k %d tile (%d,%d) APPLY z %.2f\n", sample_no, tr_river, tr_section, tr_k, tx, ty, double(z));
		rv_wcoords.push_back({wx, wy});
		h(tx, ty) = (h(tx, ty) + z) * 0.5f;
		h(tx + 1, ty) = (z + h(tx + 1, ty)) * 0.5f;
		h(tx, ty + 1) = (z + h(tx, ty + 1)) * 0.5f;
		h(tx + 1, ty + 1) = (z + h(tx + 1, ty + 1)) * 0.5f;
		for (int i = 0; i < 121; i++) {
			const int ntx = tx + move_x[i], nty = ty + move_y[i];
			if (!t_inb(ntx, nty)) continue;
			const size_t idx = size_t(ntx) + size_t(txs) * size_t(nty);
			if (!(rv_bits[idx >> 3] & (1u << (idx & 7)))) {
				rv_list.push_back(int(idx));
				rv_bits[idx >> 3] = uint8_t(rv_bits[idx >> 3] | (1u << (idx & 7)));
			}
		}
	}

	// the marking of create_trough_edge (param_6 = param_7 = 0): the 3 x 3 tiles around the sample, move_x / move_y[0..8]. The decompile shows
	// `local_18 + 1` on an `undefined4 *` and `(int)local_18 < 0x24`: a BYTE offset (`ADD ECX,4; CMP ECX,0x24` @889b56 = 9 iterations, not 36).
	void mark_trough_tiles(int tx, int ty) {
		for (int i = 0; i < 9; i++) {
			const int nx = tx + move_x[i], ny = ty + move_y[i];
			if (!t_inb(nx, ny)) continue;
			if (!(h(nx, ny) < 0.0f || h(nx + 1, ny) < 0.0f || h(nx, ny + 1) < 0.0f || h(nx + 1, ny + 1) < 0.0f)) continue;
			const int cx = nx >> 2, cy = ny >> 2;
			const WData& c = W(cx, cy);
			const uint16_t f = c.flags;
			if ((f & 0x10) || (f & 0x28) || (f & 0x800)) continue;
			if (!(f & 0x100) && (c.land == 1 || c.land == 2)) continue;
			if ((f & 0x40) || c.blocked != 0 || (F(cx, cy) & 4)) continue;
			if (trace_on) std::fprintf(stderr, "RIVER_TRACE mark river %d section %d k %d tile (%d,%d) wcoord (%d,%d) flags %04x land %d cf %04x\n", tr_river, tr_section, tr_k, nx, ny, cx, cy, f, c.land, F(cx, cy));
			rv_set_river_at(nx, ny);
		}
	}

	// RiverSection::create_trough_edge @889660: nine samples across the river
	void trough_edge(const V3& P, const V3& N, float width) {
		static const float prof[9] = {1.0f, 0.75f, 0.26f, 0.0f, 0.0f, 0.0f, 0.26f, 0.75f, 1.0f};
		const float sx = width * N[0] * 2.0f * 768.0f * 0.125f;
		const float sy = N[1] * width * 2.0f * 768.0f * 0.125f;
		float x = P[0] - sx * 4.0f, y = P[1] - sy * 4.0f;
		for (int k = 0; k < 9; k++) {
			float z = (kMaxDepth - prof[k] * kMaxDepth) + kMaxDepth * 0.25f * -1.0f;
			const int tx = tcoord_of(int(x)), ty = tcoord_of(int(y));
			if (t_inb(tx, ty)) {
				tr_k = k;
				if ((w.t(tx, ty) & 0x30) == 0x20 && z < 0.0f && std::abs(k - 4) < 2) z = ((z - 30.0f) + z) * 0.5f;
				terraform_from_river_vert(x, y, z);
				mark_trough_tiles(tx, ty);
				const int cx = wcoord_of(int(x)), cy = wcoord_of(int(y));
				if (inb(cx, cy)) move_goody(cx, cy, false);
			} else {
				// A sample off the map is not terraformed, but the original clamps the RUNNING position (create_trough_edge @889660, the off-map branch before @889dd6: y < 0 -> 0, x < 0 -> 0, then
				// both to the map width xs * 768) before stepping on: the following samples of the row start from the clamped point, so those that
				// then fall on tile row / column 0 are terraformed (a river mouth on the map border digs its edge row; the first sample is lost).
				const float lim = float(xs * 768);
				if (y < 0.0f) y = 0.0f;
				if (x < 0.0f) x = 0.0f;
				if (lim < y) y = lim;
				if (lim < x) x = lim;
			}
			x += sx;
			y += sy;
		}
	}

	// Spline type 3: clamped uniform B-spline (Cox-de Boor with the exe's guards), single precision.
	static std::vector<V3> b_spline(const std::vector<V3>& Pc, int degree, int segs) {
		const int n = int(Pc.size());
		if (n <= degree) degree = n - 1;
		if (segs < degree) segs = degree;
		std::vector<float> knots;
		float k = 0;
		for (int i = 0; i <= n + degree; i++) {
			if (i > degree && i <= n) k += 1;
			knots.push_back(k);
		}
		const int m = int(knots.size());
		const float last = knots[size_t(m - 1)];
		std::function<float(float, int, int)> basis = [&](float t, int i, int d) -> float {
			if (i + d >= m) return 1;
			if (d == 0) return knots[size_t(i)] <= t && t < knots[size_t(i + 1)] ? 1.0f : 0.0f;
			float a = 0;
			const float d1 = t - knots[size_t(i)], w1 = knots[size_t(i + d)] - knots[size_t(i)];
			if (d1 > 0 && w1 > 0) a = basis(t, i, d - 1) * (d1 / w1);
			const float d2 = knots[size_t(i + d + 1)] - t, w2 = knots[size_t(i + d + 1)] - knots[size_t(i + 1)];
			if (d2 > 0 && w2 > 0) a = a + basis(t, i + 1, d - 1) * (d2 / w2);
			return a;
		};
		const float step = (last - knots[0]) / float(segs);
		std::vector<V3> out{Pc[0]};
		float t = knots[0] + step;
		int span = 0;
		while (last > t) {
			int i = span;
			while (i + 1 < m) {
				if (!(t > knots[size_t(i + 1)]) && knots[size_t(i)] < knots[size_t(i + 1)]) break;
				i++;
			}
			span = i;
			if (span > m - 1) break;
			V3 p{0, 0, 0};
			for (int j = std::max(0, span - degree); j <= span && j < n; j++) {
				const float Nb = basis(t, j, degree);
				for (int c = 0; c < 3; c++) p[size_t(c)] = p[size_t(c)] + Pc[size_t(j)][size_t(c)] * Nb;
			}
			out.push_back(p);
			t = t + step;
		}
		if (last - (t - step) > 0.001f) out.push_back(Pc[size_t(n - 1)]);
		return out;
	}
	static V3 norm3(V3 v) {
		const float l2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
		if (l2 == 0 || l2 == 1) return v;
		const float s = 1.0f / std::sqrt(l2);
		return {v[0] * s, v[1] * s, v[2] * s};
	}
	static float dist3(const V3& a, const V3& b) {
		return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
	}

	// Spline::next_at_dist: walks the evaluated polyline by arc length.
	struct Walker {
		const std::vector<V3>& pts_;
		const std::vector<V3>& normals_;
		float length_, acc_ = 0, target_ = 0;
		int idx_ = 0;
		Walker(const std::vector<V3>& pts, const std::vector<V3>& normals, float length) : pts_(pts), normals_(normals), length_(length) {
			if (pts.empty() || length == 0) acc_ = -1, idx_ = -1;
		}
		bool next(float step, V3& p, V3& n) {
			if (acc_ < 0 || idx_ < 0) return false;
			idx_ += 1;
			target_ += step;
			const float thr = step * 0.33f;
			float s = -1;
			while (idx_ < int(pts_.size()) && target_ > acc_) {
				s = dist3(pts_[size_t(idx_)], pts_[size_t(idx_ - 1)]);
				acc_ += s;
				idx_++;
			}
			idx_ -= 1;
			if (acc_ > target_ && idx_ < int(pts_.size()) && !(length_ - target_ < thr)) {
				if (s < 0) s = dist3(pts_[size_t(idx_)], pts_[size_t(idx_ - 1)]);
				const float f = s == 0 ? 0 : (acc_ - target_) / s;
				for (size_t c = 0; c < 3; c++) {
					p[c] = pts_[size_t(idx_)][c] + (pts_[size_t(idx_ - 1)][c] - pts_[size_t(idx_)][c]) * f;
					n[c] = normals_[size_t(idx_)][c] + (normals_[size_t(idx_ - 1)][c] - normals_[size_t(idx_)][c]) * f;
				}
				n = norm3(n);
				return true;
			}
			p = pts_[size_t(idx_)];
			n = normals_[size_t(idx_)];
			if (idx_ + 1 < int(pts_.size()) && acc_ < length_) return true;
			acc_ = length_;
			idx_ = -1;
			return false;
		}
	};

	// RiverOut::ensure_ocean_end @885400: one or two more control points towards the water
	void ensure_ocean_end(std::vector<V3>& Pc, float& length) {
		const int n = int(Pc.size());
		if (n <= 2) return;
		auto add = [&](int wx, int wy) {
			const V3 q{float(wx * 768 + 384), float(wy * 768 + 384), 0.0f};
			const V3& prev = Pc.back();
			const float dx = prev[0] - q[0], dy = prev[1] - q[1];
			Pc.push_back(q);
			length = std::sqrt(dy * dy + dx * dx) / 768.0f + length;
		};
		const V3 last = Pc[size_t(n - 1)], before = Pc[size_t(n - 2)];
		V3 d{last[0] - before[0], last[1] - before[1], last[2] - before[2]};
		const float l2 = d[1] * d[1] + d[0] * d[0] + d[2] * d[2];
		V3 u = d;
		if (!(l2 == 0.0f || l2 == 1.0f)) {
			const float inv = 1.0f / std::sqrt(l2);
			u = {d[0] * inv, d[1] * inv, d[2] * inv};
		}
		if (u[0] == 0.0f && u[1] == 0.0f) {
			const V3 third = Pc[size_t(n - 3)];
			u = {last[0] - third[0], last[1] - third[1], last[2] - third[2]};
			d = u;
		}
		if (u[0] == 0.0f && u[1] == 0.0f) return;
		float best = 1e6f, vx = d[0], vy = d[1];
		bool found = false;
		int bx = 0, by = 0;
		const int lx = floor_div3(int(last[0]) >> 8), ly = floor_div3(int(last[1]) >> 8);
		for (int i = 0; i < 49; i++) {
			const int cx = lx + move_x[i], cy = ly + move_y[i];
			if (cx < 0 || cy < 0 || cx >= xs || cy >= ys || W(cx, cy).land == 2) {
				vx = float(cx * 768 + 384) - last[0];
				vy = float(cy * 768 + 384) - last[1];
				if (0.0f <= vy * u[1] + vx * u[0] + u[2] * 0.0f) {
					const float dd = float(std::sqrt(double(vy * vy + vx * vx + 0.0f)));
					if (dd < best) {
						found = true;
						bx = cx;
						by = cy;
						best = dd;
					}
				}
			}
		}
		if (!found) return;
		add(bx, by);
		float ax = vx - u[0], ay = vy - u[1];
		const float l = ay * ay + ax * ax + (0.0f - u[2]) * (0.0f - u[2]);
		if (l != 0.0f && l != 1.0f) {
			const float s = 1.0f / std::sqrt(l);
			ax = ax * s;
			ay = ay * s;
		}
		const int ex = floor_div3(int(float(bx * 768 + 384) + ax * 768.0f) >> 8);
		const int ey = floor_div3(int(float(by * 768 + 384) + ay * 768.0f) >> 8);
		if (ex >= 0 && ey >= 0 && ex < xs && ey < ys && W(ex, ey).land == 2) add(ex, ey);
	}

	// TerrainOut::ensure_45_deg_slope @869890: one pass over the queued vertices; the changed ones are queued again
	void ensure_45_deg_slope() {
		std::vector<uint8_t> added(size_t(txs) * size_t(ys * 4) / 8 + 1, 0);
		const std::vector<int> cur = rv_list;
		rv_list.clear();
		for (int idx : cur) {
			const int ty = idx / txs, tx = idx % txs;
			for (int vy = ty; vy < ty + 2; vy++) {
				for (int vx = tx; vx < tx + 2; vx++) {
					if (!t_inb(vx, vy)) continue;
					const float h0 = h(vx, vy);
					for (int ny = vy - 1; ny < vy + 2; ny++) {
						for (int nx = vx - 1; nx < vx + 2; nx++) {
							if (!t_inb(nx, ny)) continue;
							const uint8_t b = uint8_t(w.t(nx, ny));
							if ((b & 3) == 3 || (b & 0x30) == 0x20 || (b & 3) == 2) continue;
							if (79.68f < h(nx, ny) - h0) {
								const float nv = (h0 + 79.68f) - 4.0f;
								if (0.0f < nv) {
									h(nx, ny) = nv;
									const size_t id = size_t(nx) + size_t(txs) * size_t(ny);
									if (!(added[id >> 3] & (1u << (id & 7)))) {
										rv_list.push_back(int(id));
										added[id >> 3] = uint8_t(added[id >> 3] | (1u << (id & 7)));
									}
								}
							}
						}
					}
				}
			}
		}
	}

	void river_geometry_run(const std::vector<RiverPath>& rivers) {
		const TFractal frac(50, 50, 2, 2, 0);  // River::one_time_init
		tr_river = -1;
		for (const RiverPath& rp : rivers) {
			tr_river++;
			if (trace_on && !rp.pts.empty()) std::fprintf(stderr, "RIVER_TRACE river %d path (%d,%d) -> (%d,%d) points %zu edge %d\n", tr_river, rp.pts.front()[0], rp.pts.front()[1], rp.pts.back()[0], rp.pts.back()[1], rp.pts.size(), int(rp.edge));
			if (rp.pts.empty()) continue;
			// the control points RiverOut::add_spline_point collects (Map::make_riverlist emission)
			std::vector<V3> Pc;
			float length = 0;
			auto add = [&](int wx, int wy) {
				const V3 q{float(wx * 768 + 384), float(wy * 768 + 384), 0.0f};
				if (!Pc.empty()) {
					const float dx = Pc.back()[0] - q[0], dy = Pc.back()[1] - q[1];
					length = std::sqrt(dy * dy + dx * dx) / 768.0f + length;
				}
				Pc.push_back(q);
			};
			for (const auto& pt : rp.pts) add(pt[0], pt[1]);
			const int ex = rp.pts.back()[0], ey = rp.pts.back()[1];
			if (rp.edge) {
				add(ex, ey);
				if (ex == 0) add(-1, ey);
				else if (ey == 0) add(ex, -1);
				else if (ex == xs - 1) add(xs, ey);
				else if (ey == ys - 1) add(ex, ys);
			}
			ensure_ocean_end(Pc, length);
			if (Pc.size() < 2) continue;
			const int segs = int(length * kSectionsPerWcoord) & 0xFFFF;
			const std::vector<V3> pts = b_spline(Pc, 3, segs);
			if (pts.size() < 2) continue;
			std::vector<V3> normals;
			V3 prev{0, 0, 0};
			float plen = 0;
			for (size_t j = 1; j < pts.size(); j++) {
				const V3 dv{pts[j][0] - pts[j - 1][0], pts[j][1] - pts[j - 1][1], pts[j][2] - pts[j - 1][2]};
				plen += std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
				const V3 nn = norm3({dv[1], -dv[0], 0});
				normals.push_back(norm3({prev[0] + nn[0], prev[1] + nn[1], prev[2] + nn[2]}));
				prev = nn;
			}
			normals.push_back(prev);
			Walker walk(pts, normals, plen);
			const float step = 768.0f / kSectionsPerWcoord;
			rv_bits.assign(size_t(txs) * size_t(ys * 4) / 8 + 1, 0);
			rv_list.clear();
			rv_wcoords.clear();
			V3 A, nA, B, nB;
			walk.next(0, A, nA);
			int a0 = 0, a4 = 0, cc = -1, index = 0;
			float prev_w = kMaxWidth;
			bool ocean = false;
			for (;;) {
				if (++a0 > 49) {
					a4++, a0 = 0;
					if (a4 > 49) a4 = 0;
				}
				const float w0 = prev_w;
				float wd = ocean ? std::min(kMaxWidth + kMaxWidth, w0 * 1.5f) : (float(frac(a0, a4)) * 0.005f + 0.5f) * kMaxWidth;
				prev_w = wd;
				const bool more = walk.next(step, B, nB);
				if (!ocean) {
					const int mx = tcoord_of(int((A[0] + B[0]) * 0.5f)), my = tcoord_of(int((A[1] + B[1]) * 0.5f));
					if (t_inb(mx, my) && (w.t(mx, my) & 0x30) == 0x20) {
						ocean = true;
						cc = index;
						wd *= 1.5f;
						prev_w = wd;
					}
				}
				const V3 mid{(A[0] + B[0]) * 0.5f, (A[1] + B[1]) * 0.5f, (A[2] + B[2]) * 0.5f};
				const V3 n_mid{(nA[0] + nB[0]) * 0.5f, (nA[1] + nB[1]) * 0.5f, (nA[2] + nB[2]) * 0.5f};
				tr_section = index;
				trough_edge(A, nA, w0);
				trough_edge(mid, n_mid, (w0 + wd) * 0.5f);
				trough_edge(B, nB, wd);
				A = B, nA = nB;
				index++;
				if (!more) break;
			}
			// create_sections' tail: the slope limiter drains the queue, then two smoothing passes over the original queue
			(void)cc;
			// (the original copies the queue before ensure_45_deg_slope drains it)
			std::vector<int> original = rv_list;
			rv_list = original;
			while (!rv_list.empty()) ensure_45_deg_slope();
			for (int pass = 0; pass < 2; pass++) {
				for (int idx : original) smooth_tcoord(idx % txs, idx / txs);
			}
			// Terrain::spot_update_land_batch(rivers' wcoords, land 0, riverbase)
			const int rb = g.tgd.river_base;
			for (const auto& c : rv_wcoords) {
				WData& d = W(c[0], c[1]);
				if (d.land != 0 || d.land_sub != rb) {
					d.land_sub = uint8_t(rb);
					d.land = 0;
				}
			}
			for (const auto& c : rv_wcoords) {
				int l = 0, s = rb;
				verify_land(c[0], c[1], &l, &s);
			}
		}
	}

	// ---- TerrainOut::terraform_for_building @875210 (as TerrainOil::add_rocky_patch @8a2b30 calls it: tcoord (4 wx, 4 wy), 4 x 4)
	// Averages the heights of the 8 x 8 vertices from (tx - 1, ty - 1) (the far corner left out) and writes the average on the 5 x 5
	// vertices of the wcoord and the mean of the old height and the average on the ring around them; nothing changes when any sampled
	// height is not above 0 (water), a tcoord next to a mountain / cliff tcoord is left alone, and so are ocean wcoords and
	// wcoords with a good on a resource tcoord.
	void terraform_for_building(int tx, int ty, int tw, int th) {
		if (tx == 0 || ty == 0 || tx == txs - 1 || ty == g.ys * 4 - 1) return;
		const int x0 = std::max(tx - 1, 0), y0 = std::max(ty - 1, 0);
		int y1 = th + ty + 2, x1 = tw + tx + 2;
		if (g.ys * 4 <= y1) y1 = g.ys * 4 - 1;
		if (txs <= x1) x1 = txs - 1;
		float sum = 0.0f;
		int count = 0;
		for (int y = y0; y < y1; y++) {
			const float v = h(x1, y);
			sum = sum + v;
			if (v <= 0.0f) return;
			count++;
		}
		for (int x = x0; x < x1; x++) {
			const float v = h(x, y1);
			sum = sum + v;
			if (v <= 0.0f) return;
			count++;
		}
		for (int y = y0; y < y1; y++) {
			for (int x = x0; x < x1; x++) {
				const float v = h(x, y);
				sum = sum + v;
				if (v <= 0.0f) return;
				count++;
			}
		}
		const float avg = sum / float(count);
		for (int y = y0; y < y1; y++) {
			for (int x = x0; x < x1; x++) {
				const int wx = x >> 2, wy = y >> 2;
				const WData& d = W(wx, wy);
				if ((d.flags & 0x100) == 0 && (d.land == 1 || d.land == 2)) continue;  // open water
				bool next_to_mountain = false;
				for (int k = 0; k < 9 && !next_to_mountain; k++) {
					const int nx = x + move_x[k], ny = y + move_y[k];
					if (nx < 0 || ny < 0) continue;
					if (nx < w.tile_xs && ny < w.tile_ys) {
						const uint16_t m = w.t(nx, ny) & 3;
						if (m == 2 || m == 1) next_to_mountain = true;
					}
				}
				if (next_to_mountain) continue;
				if (d.land != 0) continue;
				if (w.t(x, y) & tmask::kResource) {
					bool good_here = false;
					for (const GoodObj& go : g.c.goods) {
						if (go.alive() && wcoord_of(go.x) == wx && wcoord_of(go.y) == wy) good_here = true;
					}
					if (good_here) continue;
				}
				float nv = avg;
				if (y == y0 || y == y1 - 1 || x == x0 || x == x1 - 1) nv = (h(x, y) + avg) * 0.5f;
				h(x, y) = nv;
			}
		}
	}

	// ---- TerrainOil::create_doobers @8a28e0: the oil deposits' rocky patches (the doober geometry is cosmetic and uses another random
	// stream); what the generator keeps is the rock flag an ocean wcoord loses and the terraform under an oil wcoord.
	void create_doobers() {
		for (int y = 0; y < ys; y++) {
			for (int x = 0; x < xs; x++) {
				WData& d = W(x, y);
				bool patch = false;
				const bool ocean = (d.flags & 0x100) == 0 && (d.land == 1 || d.land == 2);
				bool good_here = false;
				for (const GoodObj& go : g.c.goods) {
					if (go.alive() && wcoord_of(go.x) == x && wcoord_of(go.y) == y) good_here = true;
				}
				// LeaderData::type_avail(console leader, the good's type, 1) == 0 and not the editor: assumed for every good (the oracle's
				// oil wcoords all carry the rock flag, so the oil / rock outcome does not depend on it)
				if (good_here && !ocean) patch = true;
				if (d.flags & 8) {
					if (ocean) {
						d.flags = uint16_t(d.flags & 0xfff7);
						if (patch) add_rocky_patch(x, y);
					} else {
						add_rocky_patch(x, y);
					}
				} else if (patch) {
					add_rocky_patch(x, y);
				}
			}
		}
	}
	void add_rocky_patch(int x, int y) {
		if (W(x, y).flags & 0x800) terraform_for_building(4 * x, 4 * y, 4, 4);
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
		create_doobers();
		convert_beach_to_sandy();
	}
};

}  // namespace

void Gen::terrain_init() {
	TerrainInit ti(*this);
	ti.run();
	coord_flags = ti.cf;
	res.heights = ti.H;
	res.height_vxs = ti.vxs;
	res.height_vys = ti.vys;
	// regions: the half-water wcoords joined the land regions and left the sea ones
	for (WData& d : w.wdata) d.region2 = 0;
	rg.clear_all(w);
	rg.find_all(w);
	sync_regions();
}

void Gen::move_goody_at(int x, int y) {
	TerrainInit ti(*this);
	ti.cf = coord_flags;
	ti.H = res.heights;  // an oil derrick moved here gets its plateau (TerrainOil::add_rocky_patch) on the finished grid
	ti.move_goody(x, y, false);
	res.heights = ti.H;
}

void Gen::river_geometry() {
	if (res.rivers.empty() || res.heights.empty()) return;
	TerrainInit ti(*this);
	ti.cf = coord_flags;
	ti.H = res.heights;
	ti.coast_base = tgd.coast_base;
	ti.river_geometry_run(res.rivers);
	res.heights = ti.H;
}

}  // namespace ron::sim::mapgen
