// Rivers (docs/randommap-rivers.md): Map::make_rivers, make_riverbeds, make_riverlist and PathFinder::astar_river. Run from
// Terrain::init after Map::make. What the simulation keeps of a river is WData.flags & 0x1000 on every path cell.
#include <array>
#include <queue>
#include <set>
#include <tuple>

#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

void Gen::make_riverbeds() {
	for (WData& d : w.wdata) d.flags &= uint16_t(~(0x2000 | 0x0002));
	river_beds.clear();
	for (int y = 0; y < ys; y++) {
		for (int x = 0; x < xs; x++) {
			if ((W(x, y).flags & 0x21) != 0x20) continue;
			if (has_mountain_tcoords(x, y)) continue;
			if (near_start_city(x, y)) continue;
			int count = 0;
			bool abort = false;
			for (int i = 1; i <= 48 && !abort; i++) {
				const int cx = x + move_x[i], cy = y + move_y[i];
				if (!inb(cx, cy) || (W(cx, cy).flags & 0x2000)) {
					abort = true;
					break;
				}
				if (w.gather_land(cx, cy) == 2) {
					abort = true;
					break;
				}
				if (i < 9 && !has_mountain_tcoords(cx, cy) && !(W(cx, cy).flags & 8)) count++;
			}
			if (abort) count = 0;
			if (count >= river_thresh) {
				river_beds.push_back({x, y});
				W(x, y).flags |= 0x2000;
			}
		}
	}
}

// PathFinder::astar_river @686690: a randomised best-first search (randommap-rivers section 6).
int Gen::astar_river(int sx, int sy, int twist, int minlen, std::vector<std::array<int, 2>>* out) {
	struct Node {
		int x, y, parent, length, value;
	};
	std::vector<Node> nodes;
	std::vector<uint8_t> state(size_t(cells), 0);  // 1 open, 2 closed
	std::set<std::tuple<int, int, int>> open;      // (value, seq, node)
	int seq = 0;
	auto cell = [&](int x, int y) { return size_t(y) * size_t(xs) + size_t(x); };
	auto water_at = [&](int x, int y) {
		const WData& d = W(x, y);
		return !(d.flags & 0x100) && (d.land == 1 || d.land == 2);
	};
	for (int o = 1; o <= 4; o++) {
		const int nx = sx + orthog_x[o], ny = sy + orthog_y[o];
		if (!inb(nx, ny) || (W(nx, ny).flags & 0x1000) || water_at(nx, ny) || is_edge(nx, ny)) return 0;
	}
	nodes.push_back({sx, sy, -1, 0, 0});
	open.insert({0, seq++, 0});
	state[cell(sx, sy)] = 1;
	auto on_chain = [&](int node, int x, int y) {
		for (int n = node; n >= 0; n = nodes[size_t(n)].parent) {
			if (nodes[size_t(n)].x == x && nodes[size_t(n)].y == y) return true;
		}
		return false;
	};
	int iterations = 0;
	while (!open.empty() && ++iterations < 400000) {
		const auto top = *open.begin();
		open.erase(open.begin());
		const int ni = std::get<2>(top);
		const Node nd = nodes[size_t(ni)];
		state[cell(nd.x, nd.y)] = 2;
		const WData& cd = W(nd.x, nd.y);
		const bool expandable = (cd.flags & 0x100) || !water_at(nd.x, nd.y);
		const bool finish = !expandable || (cd.flags & 0x1000) || is_edge(nd.x, nd.y);
		if (finish) {
			if (minlen < 1 || minlen < nd.length) {
				std::vector<std::array<int, 2>> chain;
				for (int n = ni; n >= 0; n = nodes[size_t(n)].parent) chain.push_back({nodes[size_t(n)].x, nodes[size_t(n)].y});
				std::reverse(chain.begin(), chain.end());
				if (out) *out = chain;
				return int(chain.size());
			}
			continue;
		}
		std::vector<Node> kept;
		for (int dir = 1; dir <= 4; dir++) {
			const int nx = nd.x + orthog_x[dir], ny = nd.y + orthog_y[dir];
			if (!inb(nx, ny) || state[cell(nx, ny)] != 0 || on_chain(ni, nx, ny)) continue;
			if (W(nx, ny).flags & 0x0002) continue;
			const bool terminal = water_at(nx, ny) || is_edge(nx, ny) || (W(nx, ny).flags & 0x1000);
			if (terminal) {
				const int D = vd(nx - sx, ny - sy);
				bool parent_river = false;
				for (int o = 1; o <= 4; o++) {
					const int qx = nd.x + orthog_x[o], qy = nd.y + orthog_y[o];
					if (qx == nx && qy == ny) continue;
					if (inb(qx, qy) && (W(qx, qy).flags & 0x1000)) parent_river = true;
				}
				kept.clear();
				if (D >= minlen && !parent_river) kept.push_back({nx, ny, ni, nd.length + 1, 0});
				break;
			}
			int pinch = 0;
			for (int o = 1; o <= 4; o++) {
				const int qx = nx + orthog_x[o], qy = ny + orthog_y[o];
				if (!inb(qx, qy)) continue;
				if (nodes[size_t(ni)].parent >= 0 && qx == nd.x && qy == nd.y) continue;
				if (qx == nd.x && qy == nd.y) continue;
				if (on_chain(ni, qx, qy)) continue;
				if (water_at(qx, qy) || is_edge(qx, qy) || (W(qx, qy).flags & 0x1000)) pinch++;
			}
			// ❓ randommap-rivers §6: the pinch test (>= 2 water / border / river neighbours) only applies to non-terminal
			// children: a border or water child always has such neighbours and could never end a river otherwise
			if (pinch >= 2) continue;
			if (near_start_city(nx, ny) || W(nx, ny).blocked != 0 || (W(nx, ny).flags & 8)) continue;
			int rr = r.rnd_draw(20) + 10;  // PathFinder::calc_river_cost: DRAW B
			const int len = nd.length + 1;
			if (len > 1 && nd.parent >= 0) {
				const Node& gp = nodes[size_t(nd.parent)];
				if (nx == gp.x || ny == gp.y) {
					rr *= twist + 1;
				} else {
					if (twist == 0) rr *= 10;
					rr *= 101 - twist;
				}
			}
			const int l = nd.length;
			int pa, pb, pc;
			if (minlen < l) {
				pa = pb = 0;
				pc = l - minlen;
			} else if (l < minlen / 2) {
				pa = minlen - l;
				pb = pa / 2;
				pc = 0;
			} else {
				const int t = minlen - l;
				pa = t / 2;
				pb = t / 4;
				pc = 0;
			}
			const int value = (pb + 2 * pa) * 10 + 1 + pc + rr;
			if (value < 0) continue;
			kept.push_back({nx, ny, ni, len, value});
		}
		for (const Node& k : kept) {
			if (state[cell(k.x, k.y)] != 0) continue;
			nodes.push_back(k);
			open.insert({k.value, seq++, int(nodes.size()) - 1});
			state[cell(k.x, k.y)] = 1;
		}
	}
	return 0;
}

// Map::make_riverlist @693820: one <RIVER> entry.
void Gen::make_riverlist(int start, int range, int length, int minlen, int valley, int wide, int med, int twist, int flags) {
	(void)wide;
	(void)med;
	const int n = int(river_beds.size());
	if (n == 0) return;
	const int d = range > 0 ? r.rnd(range + 1) : 0;
	int goal = std::max(1, (d + start > 0) ? scale_by_axis(d + start) : 0);
	uint32_t seed, taps;
	if (n > 4000) {
		seed = world_seed & 0xffffu;
		taps = 0xb400u;
	} else if (n > 250) {
		seed = world_seed & 0xfffu;
		taps = 0xca0u;
	} else {
		seed = world_seed & 0xffu;
		taps = 0xb8u;
	}
	const uint32_t s0 = seed % uint32_t(n) + 1;
	uint32_t x = s0;
	int pass = 0;
	std::array<int, 8> counters{};
	int mincount = 0;
	bool again = true;
	const Circles& C = circles();
	auto nearest_start = [&](int px, int py) {
		int best = -1, bd = 1 << 30;
		for (int i = 0; i < nations && i < int(starts.size()); i++) {
			const int dd = vd(px - starts[size_t(i)].x, py - starts[size_t(i)].y);
			if (dd < bd) {
				bd = dd;
				best = i;
			}
		}
		return best;
	};
	for (int guard = 0; guard < 400000; guard++) {
		x = (x & 1u) ? ((x >> 1) ^ taps) : (x >> 1);
		if (x == s0) {
			pass++;
			if (pass >= 3) return;
		}
		if (x == 0) return;
		if (x > uint32_t(n)) continue;
		const WCoord bed = river_beds[size_t(x - 1)];
		if (nations > 0) {
			const int best = nearest_start(bed.x, bed.y);
			if (best >= 0 && counters[size_t(best)] > mincount) continue;
		}
		std::vector<std::array<int, 2>> path;
		const int P_ = astar_river(bed.x, bed.y, twist, (flags & 8) ? length : 0, &path);
		if (P_ <= 0 || P_ >= 400) continue;
		const int ex = path.back()[0], ey = path.back()[1];
		const bool water = !(W(ex, ey).flags & 0x100) && (W(ex, ey).land == 1 || W(ex, ey).land == 2);
		const bool edge = is_edge(ex, ey);
		if (water) {
			if (!(flags & 1)) continue;
		} else if (edge) {
			if (!(flags & 2)) continue;
		} else if (!(flags & 4)) {
			continue;
		}
		if (!(P_ >= minlen && P_ >= length / (pass + 1))) continue;
		goal = std::max(goal - 1, 0);
		again = goal != 0 && pass < 2;
		if (nations > 0) {
			const int k = nearest_start(ex, ey);
			if (k >= 0) counters[size_t(k)]++;
			mincount = 1 << 30;
			for (int i = 0; i < nations && i < 8; i++) mincount = std::min(mincount, counters[size_t(i)]);
		}
		RiverPath rp;
		for (const auto& pt : path) {
			rp.pts.push_back(pt);
			W(pt[0], pt[1]).flags |= 0x1000;
			if (valley > 0) {
				const int reg_ = W(pt[0], pt[1]).region;
				for (int k = 0; k < C.radius[size_t(std::min(valley, 64))]; k++) {
					const int cx = pt[0] + C.x[size_t(k)], cy = pt[1] + C.y[size_t(k)];
					if (inb(cx, cy) && W(cx, cy).region == reg_) W(cx, cy).flags |= 0x0002;
				}
			}
		}
		if (edge) W(ex, ey).flags |= 0x1000;
		res.rivers.push_back(std::move(rp));
		if (!again) return;
	}
}

// Map::make_rivers @692fb0
void Gen::make_rivers() {
	make_riverbeds();
	for (WData& d : w.wdata) d.flags &= uint16_t(~0x1000);
	res.river_beds = int(river_beds.size());
	if (river_beds.empty()) return;
	const XNode* list = section(u"RIVERLIST");
	if (list) {
		auto scaled = [&](int a) { return a <= 0 ? 0 : std::max(1, (ref_w / 2 + xs * a) / ref_w); };
		for (const XNode* e : xml::children(list, u"RIVER")) {
			const int start = xml::get_attrib_num(e, u"start_range", -1);
			const int range = std::max(0, xml::get_attrib_num(e, u"end_range", -1) - start);
			const int length = scaled(xml::get_attrib_num(e, u"river_length", -1));
			const int minlen = scaled(xml::get_attrib_num(e, u"river_min_length", -1));
			const int valley = scaled(xml::get_attrib_num(e, u"river_valley", -1));
			make_riverlist(start, range, std::min(length, 399), minlen, valley, xml::get_attrib_num(e, u"river_wide", -1),
			               xml::get_attrib_num(e, u"river_med", -1), xml::get_attrib_num(e, u"river_twist", -1),
			               xml::get_attrib_num(e, u"river_flags", -1));
		}
	}
	for (WData& d : w.wdata) d.flags &= uint16_t(~(0x2000 | 0x0002));
	// Regions::find_all at the end of make_rivers: 0x1000 is not a barrier for the regions, so nothing changes here.
}

}  // namespace ron::sim::mapgen
