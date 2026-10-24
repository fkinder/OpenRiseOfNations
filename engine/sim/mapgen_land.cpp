// Region primitives and land shaping of Map::make: World::wipe, make_region / point / stamp / large_stamp / grow_valid /
// grow_region, land_dist, invert_land, fix_lakes, fix_diag_land, make_coastlines, eliminate_pools, check_player_land,
// check_player_forest and the start-location helpers (docs/randommap-spec.md sections 6-8, randommap-terrain-groups.md B1,
// randommap-resources.md section 9).
#include <array>
#include <functional>

#include "sim/mapgen_internal.hpp"
#include "sim/search.hpp"

namespace ron::sim::mapgen {

void Gen::project_pt(int cx, int cy, uint32_t angle, int rad, int* x, int* y) const {
	int32_t ox = 0, oy = 0;
	project(cx, cy, angle, rad, &ox, &oy);
	*x = ox;
	*y = oy;
}

int Gen::rnd_angle_pair(uint32_t* angle) {
	const uint32_t a = uint32_t(r.rnd_draw(0xffff) % 0xffff);
	const uint32_t b = uint32_t(r.rnd_draw(0xffff) % 0xffff);
	*angle = (a << 16) + b;
	return 0;
}

// ---------------------------------------------------------------- World::wipe / Regions

// World::add_starting_location @6b2de0: the start, its 2x2 city cells in the radius list and the city bit set.
void Gen::add_starting_location(int x, int y) {
	starts.push_back({x, y});
	static const int dx[4] = {0, -1, 0, -1}, dy[4] = {0, 0, -1, -1};
	for (int i = 0; i < 4; i++) {
		start_city_list.push_back({x + dx[i], y + dy[i]});
		const int idx = (y + dy[i]) * xs + x + dx[i];
		if (idx >= 0 && idx < cells) start_city_bits[size_t(idx)] = 1;
	}
}

void Gen::wipe() {
	starts.clear();
	start_city_list.clear();
	start_city_bits.assign(size_t(cells), 0);
	w.sea_map = -1;
	for (WData& d : w.wdata) {
		WData n;
		n.region = 64;
		n.who = -1;
		n.who2 = -1;
		d = n;
	}
}

void Gen::clear_all() {
	rg.clear_all(w);
	for (GenRegion& g : reg) g = GenRegion();
	reg_land = 0;
}

void Gen::sync_regions() {
	for (int i = 0; i < Regions::kNum; i++) {
		GenRegion& g = reg[size_t(i)];
		g = GenRegion();
		g.size = rg.r[size_t(i)].size;
		g.cells = rg.r[size_t(i)].coords;
		g.common = common_factor;
		g.goody = goody_factor;
	}
	in_generation = false;
}

void Gen::find_all() {
	// the generator's own region ids live in WData.region while a recipe runs: relabel by connectivity from scratch
	rg.clear_all(w);
	rg.find_all(w);
	sync_regions();
}

void Gen::make_region(int rid, int x, int y, int goal) {
	(void)goal;
	GenRegion& g = reg[size_t(rid)];
	g = GenRegion();
	g.size = 1;
	g.flags = region_flags;
	g.common = common_factor;
	g.goody = goody_factor;
	g.cells.push_back({x, y});
	WData& d = W(x, y);
	d.land = 0;
	d.land_sub = 0;
	d.region = int16_t(rid);
	reg_land = std::max(reg_land, rid);
}

// ---------------------------------------------------------------- growth

// Map::grow_valid @69d000 (asm read): avoid_center is the distance from the MAP CENTRE, applied while another non-empty region
// exists that is not exempt (both regions flagged REGION_MERGE exempt each other); avoid_continent a circle test for foreign
// region cells (an avoid_continent above 64 rejects every cell).
bool Gen::grow_valid(int rid, int x, int y) {
	const Circles& C = circles();
	if (avoid_continent > 64) return false;
	auto exempt = [&](int q) { return rid >= 0 && (reg[size_t(q)].flags & reg[size_t(rid)].flags & 1); };
	for (int q = 1; q < 64; q++) {
		if (reg[size_t(q)].size == 0 || q == rid || exempt(q)) continue;
		if (vd(std::abs(y - ys / 2), std::abs(x - xs / 2)) < avoid_center) return false;
	}
	for (int k = 0; k < C.radius[size_t(avoid_continent)]; k++) {
		const int cx = x + C.x[size_t(k)], cy = y + C.y[size_t(k)];
		if (!inb(cx, cy)) continue;
		const int q = W(cx, cy).region;
		if (q == 0 || q == rid) continue;
		if (rid < 0 || !exempt(q)) return false;
	}
	for (int e = 0; e < 4; e++) {
		if (avoid_edge[size_t(e)] == 0) continue;
		avoid_modify += r.rnd(3) - 1;
		avoid_modify = std::min(4, std::max(0, avoid_modify));
		const int margin = avoid_edge[size_t(e)] + avoid_modify;
		switch ((base_edge + e) & 3) {
		case 0: if (x < margin) return false; break;
		case 1: if (y < margin) return false; break;
		case 2: if (xs - x <= margin) return false; break;
		default: if (ys - y <= margin) return false; break;
		}
	}
	return true;
}

int Gen::point(int rid, int x, int y) {
	if (!inb(x, y) || W(x, y).region != 0) return 1;
	if (!grow_valid(rid, x, y)) return 0;
	WData& d = W(x, y);
	d.land = 0;
	d.land_sub = 0;
	d.region = int16_t(rid);
	GenRegion& g = reg[size_t(rid)];
	g.cells.push_back({x, y});
	g.size++;
	if (g.flags & 1) {
		// touching masks (transitive closure over regions 0..31)
		for (int k = 1; k <= 8; k++) {
			const int nx = x + move_x[k], ny = y + move_y[k];
			if (!inb(nx, ny)) continue;
			const int q = W(nx, ny).region;
			if (q <= 0 || q == rid || q >= 64) continue;
			const uint32_t m = reg[size_t(rid)].mask | reg[size_t(q)].mask | (1u << (rid & 31)) | (1u << (q & 31));
			reg[size_t(rid)].mask |= m;
			reg[size_t(q)].mask |= m;
			for (int i = 0; i < 64; i++) {
				if (reg[size_t(i)].mask & m) reg[size_t(i)].mask |= m;
			}
		}
	}
	return 1;
}

int Gen::stamp(int rid, int x, int y) {
	static const int dx[5] = {0, 1, -1, 0, 0}, dy[5] = {0, 0, 0, -1, 1};
	for (int i = 0; i < 5; i++) {
		if (point(rid, x + dx[i], y + dy[i]) == 0) return 0;
	}
	return 1;
}

int Gen::large_stamp(int rid, int x, int y, int left) {
	(void)left;
	if (stamp(rid, x, y) == 0) return 0;
	const int v = r.mask(15);
	static const int8_t pat[9][6][2] = {
		{{-1, -1}, {-2, -2}, {-4, -1}, {-4, 0}, {-5, -1}, {-5, -2}},
		{{1, 1}, {2, 2}, {4, 1}, {4, 0}, {5, 1}, {5, 2}},
		{{1, 0}, {1, -2}, {1, 2}, {2, -4}, {3, -4}, {4, 1}},
		{{-1, 0}, {-1, -2}, {-1, 2}, {-2, 4}, {-3, 4}, {-4, -1}},
		{{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
		{{1, -2}, {2, -4}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
		{{-1, -2}, {-2, -4}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
		{{-1, 2}, {-2, 4}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
		{{1, 2}, {2, 4}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}};
	if (v <= 3) {
		for (int i = 0; i < 6; i++) {
			if (stamp(rid, x + pat[v][i][0], y + pat[v][i][1]) == 0) return 0;
		}
		return 1;
	}
	if (v == 4) {
		if (stamp(rid, x, y - 1) == 0) return 0;
		if (stamp(rid, x, y + 1) == 0) return 0;
		for (int dx = -2; dx <= 2; dx++) {
			if (stamp(rid, x + dx, y) == 0) return 0;
		}
		int bit = r.bit();
		if (stamp(rid, x + (bit * 2 - 1), y - 1) == 0) return 0;
		bit = r.bit();
		return stamp(rid, x + (bit * 2 - 1), y + 1);
	}
	if (v >= 5 && v <= 8) {
		int res_ = 1;
		for (int i = 0; i < 2; i++) {
			res_ = stamp(rid, x + pat[v][i][0], y + pat[v][i][1]);
			if (!res_) return 0;
		}
		return res_;
	}
	return 1;
}

// Map::grow_region @69c600 (decompile read). Returns 0 on success when p6 == 0, else the final size (also when it gave up);
// an empty region returns 1.
int Gen::grow_region(int rid, int goal, int m, int ox, int oy, int p6) {
	GenRegion& g = reg[size_t(rid)];
	const Circles& C = circles();
	int k = 2;
	{
		const int q = goal / 4;
		if (q > 12) {
			do {
				if (k > 0x3f) break;
				k++;
			} while (k * k * 3 < q);
		}
	}
	if (g.size == 0) return 1;
	if (ox < 0 || oy < 0) {
		ox = g.cells[0].x;
		oy = g.cells[0].y;
	}
	int ring = 0;
	if (g.size < 2) {
		const WCoord first = g.cells[0];
		for (int i = 1; i < C.radius[size_t(k)]; i++) {
			const int cx = first.x + C.x[size_t(i)], cy = first.y + C.y[size_t(i)];
			if (inb(cx, cy) && grow_valid(rid, cx, cy)) point(rid, cx, cy);
		}
		ring = C.radius[size_t(k - 1)];
	}
	if (g.size >= goal) return p6 ? g.size : 0;
	const int thresh = scale_by_area(512);
	int stall_total = 0, stall_run = 0;
	for (int guard = 0; guard < goal * 16 + 1000; guard++) {
		const int size0 = g.size;
		int ci = -1;
		bool uniform;
		{
			const int e = size0 / 8;
			uniform = size0 < thresh || e - 1 < 1 || r.rnd_draw(e) == 0;
		}
		if (!uniform) {
			int q = (size0 + 7) / 8;
			q = q - 1 < 1 ? 0 : r.rnd_draw(q);
			ci = (size0 * 7) / 8 + q;
			if (ci > size0 - 1) ci = size0 - 1;
			const WCoord c0 = g.cells[size_t(ci)];
			if (m < vd(c0.x - ox, c0.y - oy)) uniform = true;
		}
		if (uniform) {
			if (ring == 0 || size0 - ring < C.radius[size_t(k)] - C.radius[size_t(k - 1)]) {
				ci = size0 - 1 < 1 ? 0 : r.rnd_draw(size0);
			} else {
				const int span = size0 - ring;
				ci = span - 1 < 1 ? ring : ring + r.rnd_draw(span);
			}
		}
		const WCoord cell = g.cells[size_t(ci)];
		if (goal - size0 < 0x40) stamp(rid, cell.x, cell.y);
		else large_stamp(rid, cell.x, cell.y, goal - size0);
		if (g.size == size0) {
			stall_total++;
			stall_run++;
			if (goal * 8 <= stall_total || goal <= stall_run) return g.size;
		} else {
			stall_run = 0;
		}
		if (goal <= g.size) return p6 ? g.size : 0;
	}
	return g.size;
}

// Map::land_dist @69d970
int Gen::land_dist(int x, int y, int outside_is_land) {
	if (!inb(x, y) || !is_water(x, y)) return 0;
	const Circles& C = circles();
	for (int k = 1; k <= 64; k++) {
		for (int i = C.radius[size_t(k - 1)]; i < C.radius[size_t(k)]; i++) {
			const int cx = x + C.x[size_t(i)], cy = y + C.y[size_t(i)];
			if (!inb(cx, cy)) {
				if (outside_is_land) return k;
				continue;
			}
			if (!is_water(cx, cy)) return k;
		}
	}
	return 65;
}

void Gen::invert_land() {
	for (WData& d : w.wdata) {
		if (World::is_ocean(d)) d.land = 0;
		else d.land = 2;
		d.land_sub = 0;
		d.region = 0;
	}
	clear_all();
}

void Gen::fix_lakes() {
	bool changed = true;
	int guard = 0;
	while (changed && ++guard < 200) {
		changed = false;
		for (int y = 1; y < ys - 1; y++) {
			for (int x = 1; x < xs - 1; x++) {
				if (!is_water(x, y)) continue;
				int n = 0;
				for (int k = 1; k <= 8; k++) {
					if (!is_water(x + move_x[k], y + move_y[k])) n++;
				}
				if (n > 5) {
					W(x, y).land = 0;
					W(x, y).land_sub = 0;
					changed = true;
				}
			}
		}
	}
}

// Map::fix_diag_land @69c250 (decompile read): a land cell (land 0) joined to a diagonal land cell with both orthogonal cells
// between them non-land becomes water; x outer, y inner.
void Gen::fix_diag_land() {
	static const int cx_[4] = {-1, 1, 1, -1}, cy_[4] = {-1, -1, 1, 1};
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			if (W(x, y).land != 0) continue;
			for (int k = 0; k < 4; k++) {
				const int nx = x + cx_[k], ny = y + cy_[k];
				if (!inb(nx, ny) || W(nx, ny).land != 0) continue;
				if (!inb(nx, y) || !inb(x, ny)) continue;
				if (W(nx, y).land != 0 && W(x, ny).land != 0) {
					W(x, y).land = 2;
					break;
				}
			}
		}
	}
}

// Map::make_coastlines @6947a0
void Gen::make_coastlines() {
	fix_lakes();
	for (int y = 0; y < ys; y++) {
		for (int x = 0; x < xs; x++) {
			WData& d = W(x, y);
			if (World::is_ocean(d) || (d.flags & 4)) continue;
			bool water = false;
			for (int k = 1; k <= 8 && !water; k++) {
				const int nx = x + move_x[k], ny = y + move_y[k];
				if (inb(nx, ny) && is_water(nx, ny)) water = true;
			}
			if (water) d.flags = uint16_t((d.flags & ~0x3c) | 4 | 0x400);
		}
	}
	for (int y = 0; y < ys; y++) {
		for (int x = 0; x < xs; x++) {
			WData& d = W(x, y);
			if (!(d.flags & 4)) continue;
			bool inland = false;
			for (int k = 1; k <= 8 && !inland; k++) {
				const int nx = x + move_x[k], ny = y + move_y[k];
				if (!inb(nx, ny)) continue;
				const WData& n = W(nx, ny);
				if (!World::is_ocean(n) && !(n.flags & 4)) inland = true;
			}
			if (!inland) {
				d.flags = uint16_t(d.flags & 0xfac3);
				d.land = 2;
			}
		}
	}
}

// Map::eliminate_pools @68b890 (decompile read): Regions::clear_all + find_all first, then every water body (sea region > 0x40)
// touching the selected cells but the largest becomes land of the first land region its cells touch.
void Gen::eliminate_pools(int mode) {
	find_all();
	std::vector<int> list;
	for (int y = 0; y < ys; y++) {
		for (int x = 0; x < xs; x++) {
			bool sel;
			if (mode == kEdges) sel = x == 0 || x == xs - 1 || y == 0 || y == ys - 1;
			else if (mode == kCorners) sel = x == 0 || x == xs - 1;  // as decompiled (the y test was lost)
			else sel = true;
			if (!sel) continue;
			const int id = W(x, y).region;
			if (id > 0x40 && std::find(list.begin(), list.end(), id) == list.end()) list.push_back(id);
		}
	}
	while (list.size() > 1) {
		size_t best = 0;
		int smallest = cells;
		for (size_t i = 0; i < list.size(); i++) {
			if (reg[size_t(list[i])].size < smallest) {
				smallest = reg[size_t(list[i])].size;
				best = i;
			}
		}
		const int pool = list[best];
		const std::vector<WCoord> pcs = reg[size_t(pool)].cells;
		int q = pool;
		bool found = false;
		for (const WCoord& c : pcs) {
			for (int k = 1; k <= 8 && !found; k++) {
				const int nx = c.x + move_x[k], ny = c.y + move_y[k];
				if (!inb(nx, ny)) continue;
				const int nr = W(nx, ny).region;
				if (nr != pool && nr < 0x41) {
					q = nr;
					found = true;
				}
			}
			if (found) break;
		}
		reg[size_t(pool)].cells.clear();
		reg[size_t(pool)].size = 0;
		for (const WCoord& c : pcs) {
			WData& d = W(c.x, c.y);
			d.land = 0;
			d.land_sub = 0;
			d.region = int16_t(q);
			reg[size_t(q)].cells.push_back(c);
			reg[size_t(q)].size++;
		}
		list.erase(list.begin() + long(best));
	}
}

// Map::eliminate_edge_canals @68b360 (❓ shape read from the summary of randommap-spec §7.5).
void Gen::eliminate_edge_canals() {
	for (int side = 0; side < 4; side++) {
		const int n = (side < 2) ? xs : ys;
		auto cell = [&](int i, int depth) -> WData* {
			int x = 0, y = 0;
			switch (side) {
			case 0: x = i; y = depth; break;
			case 1: x = i; y = ys - 1 - depth; break;
			case 2: x = depth; y = i; break;
			default: x = xs - 1 - depth; y = i; break;
			}
			return inb(x, y) ? &W(x, y) : nullptr;
		};
		std::vector<uint8_t> hit(size_t(n), 0);
		for (int i = 0; i + 2 < n; i++) {
			WData *a = cell(i, 0), *b = cell(i + 1, 0), *c2 = cell(i + 2, 0), *in = cell(i + 1, 1);
			if (!a || !b || !c2 || !in) continue;
			if (World::is_ocean(*a) && World::is_ocean(*b) && World::is_ocean(*c2) && !World::is_ocean(*in)) {
				in->land = 2;
				in->land_sub = 0;
				hit[size_t(i + 1)] = 1;
			}
		}
		for (int i = 0; i + 2 < n; i++) {
			if (hit[size_t(i)] && hit[size_t(i + 1)] && hit[size_t(i + 2)]) {
				if (WData* d = cell(i + 1, 2)) {
					d->land = 2;
					d->land_sub = 0;
				}
			}
		}
	}
}

// ---------------------------------------------------------------- start helpers

int Gen::scale_land_area(int v, int n) const {
	static const int base_tab[6] = {2, 2, 3, 4, 6, 8};
	const int base = map_size >= 0 && map_size < 6 ? base_tab[map_size] : 8;
	if (n == 0 || n <= base) return v;
	return int(float(v) / (float(n) / float(base)));
}

int Gen::get_player_overage(int n) const {
	static const int base_tab[6] = {2, 2, 3, 4, 6, 8};
	const int base = map_size >= 0 && map_size < 6 ? base_tab[map_size] : 8;
	return (n == 0 || n <= base) ? 0 : n - base;
}

void Gen::find_region_centroid(int rid, int* x, int* y) {
	const GenRegion& g = reg[size_t(rid)];
	if (g.size <= 0) {
		*x = xs / 2;
		*y = ys / 2;
		return;
	}
	int64_t sx = 0, sy = 0;
	for (const WCoord& c : g.cells) {
		sx += c.x;
		sy += c.y;
	}
	*x = int(sx / g.size);
	*y = int(sy / g.size);
}

bool Gen::is_near_ocean(int x, int y, int a, int b) {
	if (!inb(x, y)) return false;
	const Circles& C = circles();
	if (b != 0) {
		if (b + 1 > 64 || b - 1 < 0) return false;
		bool any = false;
		for (int k = C.radius[size_t(b - 1)]; k < C.radius[size_t(b + 1)] && !any; k++) {
			const int cx = x + C.x[size_t(k)], cy = y + C.y[size_t(k)];
			if (inb(cx, cy) && W(cx, cy).land != 0) any = true;
		}
		if (!any) return false;
	}
	if (a != 0) {
		if (a > 64) return false;
		for (int k = C.radius[0]; k < C.radius[size_t(a)]; k++) {
			const int cx = x + C.x[size_t(k)], cy = y + C.y[size_t(k)];
			if (inb(cx, cy) && W(cx, cy).land != 0) return false;
		}
	}
	return true;
}

bool Gen::place_start_in_region(int rid, int* ox, int* oy, int min_dist, const std::vector<WCoord>& others) {
	const GenRegion& g = reg[size_t(rid)];
	const int size = g.size;
	if (size <= 0) return false;
	for (int t = 1; t <= 2; t++) {
		const int lo = t == 1 ? 5 : 3, hi = t == 1 ? 6 : 4;
		const int s = size > 1 ? r.rnd(size) : 0;
		int idx = s + 1 < size ? s + 1 : 0;
		for (int n = 0; n < size; n++) {
			const WCoord c = g.cells[size_t(idx)];
			idx = idx + 1 < size ? idx + 1 : 0;
			if (c.x < lo || c.y < lo || c.x > xs - hi || c.y > ys - hi) continue;
			const int md = t == 2 ? std::min(min_dist, 6) : min_dist;
			bool ok = true;
			for (const WCoord& o : others) {
				if (vd(c.x - o.x, c.y - o.y) < md) {
					ok = false;
					break;
				}
			}
			if (!ok) continue;
			if (!(t == 1 ? is_near_ocean(c.x, c.y, 3, 9) : is_near_ocean(c.x, c.y, 2, 0))) continue;
			*ox = c.x;
			*oy = c.y;
			return true;
		}
	}
	return false;
}

// Map::find_avail_regions @68f380: regions with more than 5 cells, in descending id order; `current` = the rotated index.
std::vector<int> Gen::find_avail_regions(bool sea, int pattern, int* current) {
	std::vector<int> list;
	const int lo = sea ? 64 : 1, hi = sea ? 126 : 63;
	for (int id = hi; id >= lo; id--) {
		const GenRegion& g = reg[size_t(id)];
		if (g.size <= 5) continue;
		if (pattern == 3) {
			bool has_start = false;
			for (const WCoord& s : starts) {
				if (inb(s.x, s.y) && W(s.x, s.y).region == id) has_start = true;
			}
			if (has_start) continue;
		}
		list.push_back(id);
	}
	*current = list.size() > 1 ? r.rnd(int(list.size())) : 0;
	return list;
}

// Map::fill_cont @68a960 (decompile read): `extra` = the team ids without a valid leader (pseudo teams of the leaders with no
// team), `cont[team id]` = the continent of a team (-1 unused), `count[continent]` = its leaders; continents with equal counts
// may swap (one draw per ordered pair).
void Gen::fill_cont(std::array<int, 8>& count, std::array<int, 8>& cont, std::vector<int>* extra, int* sides) {
	auto team_of = [&](int slot) { return (P.team[size_t(slot)] >= 0 && P.team[size_t(slot)] < 8) ? P.team[size_t(slot)] : 8; };
	std::vector<int> ex;
	for (int t = 0; t < 8; t++) {
		bool any = false;
		for (int s = 0; s < nations && s < 8; s++) {
			if (team_of(s) == t) any = true;
		}
		if (!any) ex.push_back(t);
	}
	count.fill(0);
	cont.fill(-1);
	int k = 0;
	size_t ei = 0;
	for (int s = 0; s < nations && s < 8; s++) {
		const int t = team_of(s);
		if (t == 8) {
			const int pseudo = ei < ex.size() ? ex[ei] : 7;
			ei++;
			cont[size_t(pseudo)] = k++;
			count[size_t(cont[size_t(pseudo)])] = 1;
		} else {
			if (cont[size_t(t)] == -1) cont[size_t(t)] = k++;
			count[size_t(cont[size_t(t)])]++;
		}
	}
	for (int i = 0; i < 8; i++) {
		if (count[size_t(i)] == 0) continue;
		for (int j = 0; j < 8; j++) {
			if (j != i && count[size_t(j)] == count[size_t(i)] && r.bit()) {
				for (int e = 0; e < 8; e++) {
					const int iv = cont[size_t(e)];
					if (iv == i || iv == -1) cont[size_t(e)] = j;
					if (iv == j || iv == -1) cont[size_t(e)] = i;
				}
			}
		}
	}
	if (extra) *extra = ex;
	*sides = k;
}

bool Gen::near_start_city(int x, int y) const {
	const int ccr = rules.constants.at(uint32_t(constant_offset("city_center_radius")));
	for (const WCoord& s : start_city_list) {
		if (vd(x - s.x, y - s.y) * 4 < ccr - 1) return true;
	}
	return false;
}

bool Gen::has_mountain_tcoords(int x, int y) const {
	for (int i = 0; i < 16; i++) {
		if (w.is_mountain_at(traverse_x(x, i), traverse_y(y, i))) return true;
	}
	return false;
}

int Gen::center_d(int x, int y) const { return vd(x - xs / 2, y - ys / 2); }

int Gen::corner_d(int x, int y, int corner) const {
	static const int cbx[4] = {0, 0, 1, 1}, cby[4] = {0, 1, 0, 1};
	if (corner >= 0) return vd(x - cbx[corner] * xs, y - cby[corner] * ys);
	int best = 9999;
	for (int c_ = 0; c_ < 4; c_++) best = std::min(best, vd(x - cbx[c_] * xs, y - cby[c_] * ys));
	return best;
}

// Map::check_player_land @68ef00 (decompile read): land under and around every start location. `nearby` is the ECX argument
// of the call (every recipe but Warring States passes 1): a circle cell stays as it is when a land cell of another region
// with more than 4 cells lies within the square of Chebyshev radius min(avoid_continent, 10) around it.
void Gen::check_player_land(int range, bool nearby) {
	const Circles& C = circles();
	const int R = std::min(64, range < 0 ? 5 : range);
	for (const WCoord& s : starts) {
		if (!inb(s.x, s.y)) continue;
		int rid = W(s.x, s.y).region;
		if (rid == 0) {
			int adopt = 0;
			for (int k = 1; k <= 48 && !adopt; k++) {
				const int nx = s.x + move_x[k], ny = s.y + move_y[k];
				if (!inb(nx, ny)) continue;
				const int q = W(nx, ny).region;
				if (q >= 1 && q <= 0x3f) adopt = q;
			}
			if (adopt) {
				for (int k = 0; k <= 8; k++) {
					const int nx = s.x + move_x[k], ny = s.y + move_y[k];
					if (!inb(nx, ny)) continue;
					WData& d = W(nx, ny);
					d.land = 0;
					d.land_sub = 0;
					if (d.region != adopt) {
						d.region = int16_t(adopt);
						reg[size_t(adopt)].cells.push_back({nx, ny});
						reg[size_t(adopt)].size++;
					}
				}
				rid = adopt;
			}
		}
		const int cheb = std::min(avoid_continent, 10);
		for (int i = C.radius[0]; i < C.radius[size_t(R)]; i++) {
			const int cx = s.x + C.x[size_t(i)], cy = s.y + C.y[size_t(i)];
			if (!inb(cx, cy)) continue;
			if (nearby) {
				bool skip = false;
				for (int j = 1; j < kMoveRadius[cheb] && !skip; j++) {
					const int nx = cx + move_x[j], ny = cy + move_y[j];
					if (!inb(nx, ny)) continue;
					const WData& n = W(nx, ny);
					if (World::is_ocean(n)) continue;
					const int q = n.region;
					if (q != rid && q >= 0 && q < Regions::kNum && reg[size_t(q)].size > 4) skip = true;
				}
				if (skip) continue;
			}
			WData& d = W(cx, cy);
			d.land = 0;
			d.land_sub = 0;
			if (rid > 0 && d.region != rid) {
				reg[size_t(rid)].cells.push_back({cx, cy});
				reg[size_t(rid)].size++;
			}
			d.region = int16_t(rid);
		}
	}
}

// Map::check_player_forest @68e8f0 (randommap-terrain-groups A7): a starting forest of 3 (or 2, or 1) cells.
void Gen::check_player_forest() {
	const Circles& C = circles();
	const int ccr = rules.constants.at(uint32_t(constant_offset("city_center_radius")));
	const int R = std::max(3, std::min(64, ccr / 4 - 1));
	auto free_cell = [&](int x, int y) {
		if (!inb(x, y)) return false;
		const WData& d = W(x, y);
		return (d.flags & 0x30) == 0 && !World::is_ocean(d) && (d.flags & 0x48) == 0 && !start_city_wcoord(x, y) &&
		       (d.flags & 4) == 0;
	};
	for (const WCoord& s : starts) {
		int count = 0;
		for (int i = C.radius[0]; i < C.radius[size_t(R)]; i++) {
			const int cx = s.x + C.x[size_t(i)], cy = s.y + C.y[size_t(i)];
			if (inb(cx, cy) && (W(cx, cy).flags & 0x20)) count++;
		}
		if (count >= 3) continue;
		bool done = false;
		for (int target = 3; target >= 1 && !done; target--) {
			for (int i = C.radius[2]; i < C.radius[size_t(R)]; i++) {
				const int cx = s.x + C.x[size_t(i)], cy = s.y + C.y[size_t(i)];
				if (!free_cell(cx, cy)) continue;
				std::vector<WCoord> clump{{cx, cy}};
				for (int o = 1; o <= 4 && int(clump.size()) < target; o++) {
					const int nx = cx + orthog_x[o], ny = cy + orthog_y[o];
					if (free_cell(nx, ny)) clump.push_back({nx, ny});
				}
				if (int(clump.size()) != target) continue;
				for (const WCoord& c : clump) {
					WData& d = W(c.x, c.y);
					if (d.flags & 4) d.land = 2;
					d.flags = uint16_t((d.flags & 0xffc3) | 0x20);
				}
				done = true;
				break;
			}
		}
	}
}

}  // namespace ron::sim::mapgen
