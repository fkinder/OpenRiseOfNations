// The Conquer the World battle map (map styles 24 / 25, docs/randommap-terrain-groups.md B3.13 and "Fourth pass" in randommap-spec.md):
// ConquestCountries::get_scenario_map_rect @75c3b0 and draw_scenario_area @761310 (the square image of the two countries'
// silhouettes), ScenarioEditor::import_image @9a3f30 (the image becomes the land grid), Map::force_continent_spacing @68e6a0,
// MapConquest::make_continents @68dba0, check_starting_loc @68d7e0, add_additional_players @68d260 and check_sea_map @68d1a0.
// All float arithmetic is single precision in the original's order (the SSE code was read from the asm, the decompile loses it).
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include "sim/compass.hpp"
#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

namespace {

thread_local const ConquestInput* g_conquest_input = nullptr;

float absf(float v) { return v < 0.0f ? -v : v; }

}  // namespace

void set_conquest_input(const ConquestInput* in) { g_conquest_input = in; }
const ConquestInput* conquest_input() { return g_conquest_input; }

// ---------------------------------------------------------------- get_scenario_map_rect (texture-pixel branch)

ConquestRect conquest_scenario_rect(int x1, int y1, int x2, int y2, int tex_width, int map_style) {
	const float dx = float(x1 - x2);
	const float dy = float(y1 - y2);
	const int mn = std::min(x1, x2), mx = std::max(x1, x2);
	float wrap = (float(tex_width) - float(mx)) + float(mn);
	if (dx > 0.0f) wrap = -wrap;
	float cx = 0.0f, cy = 0.0f, size = 0.0f;
	switch (map_style) {
	case 0: case 1: case 2: case 3: case 8: {
		cx = float(x1) - dx * 0.5f;
		cy = float(y1) - dy * 0.5f;
		float ext = dx;
		if (absf(dx) > absf(wrap)) {
			cx = float(x1) - wrap * 0.5f;
			if (x1 > x2) cx -= float(tex_width);
			ext = wrap;
		}
		const float a = absf(ext), b = absf(dy);
		size = (a > b ? a : b) * 1.3f;
		break;
	}
	case 4: case 6: case 7:
	case 5: {
		const bool second = map_style != 5;
		cx = float(second ? x2 : x1);
		cy = float(second ? y2 : y1);
		float ext = dx;
		if (absf(dx) > absf(wrap)) ext = wrap;
		const float a = absf(ext), b = absf(dy);
		size = a > b ? a : b;
		break;
	}
	default: break;
	}
	ConquestRect r;
	const float half = size * 0.5f;
	r.left = cx - half;
	r.top = cy - half;
	r.right = r.left + size;
	r.bottom = r.top + size;
	return r;
}

// ---------------------------------------------------------------- draw_scenario_area

bool conquest_draw_scenario_area(const ConquestRect& rect, int tex_width, float scale, const std::vector<ConquestCountry>& countries,
                                 const ConquestPieceLoader& piece, ConquestInput& out) {
	out.ids.clear();
	out.xs.clear();
	out.ys.clear();
	const float L = float(int(rect.left)), T = float(int(rect.top));
	float R = float(int(rect.right)), B = float(int(rect.bottom));
	const float height_f = B - T, width_f = R - L;
	const int h_i = int(height_f), w_i = int(width_f);
	const int side = int(w_i > h_i ? width_f : height_f);
	if (float(side) > height_f) B += 1.0f;
	if (float(side) > width_f) R += 1.0f;
	if (side <= 0 || side > 8192) return false;
	out.side = side;
	out.land.assign(size_t(side) * size_t(side), 0);
	const int Li = int(L), Ti = int(T);
	for (size_t i = 0; i < countries.size(); i++) {
		const ConquestCountry& c = countries[i];
		float x5 = (float(c.left) * scale) / scale;    // left   (+0x178)
		float x7 = (float(c.right) * scale) / scale;   // right  (+0x17c)
		float x4 = (float(c.top) * scale) / scale;     // top    (+0x180)
		float x6 = (float(c.bottom) * scale) / scale;  // bottom (+0x184)
		auto apart = [&]() { return L > x7 || x5 > R || x4 > B || T > x6; };
		int shift = 0;
		if (apart()) {
			shift = -tex_width;
			x7 += float(shift);
			x5 += float(shift);
			if (apart()) continue;
		}
		const float clip_l = x5 > L ? x5 : L;
		const float clip_r = R > x7 ? x7 : R;
		const float clip_t = x4 > T ? x4 : T;
		const float clip_b = B > x6 ? x6 : B;
		const Image* img = piece ? piece(int(i)) : nullptr;
		if (img && img->width > 0 && img->height > 0) {
			const int pw = img->width, ph = img->height;
			const int src_left = int(x5), src_top = int(x4);
			const int y0 = int(clip_t), y1 = int(clip_b), xa = int(clip_l), xb = int(clip_r);
			for (int y = y0; y < y1; y++) {
				for (int x = xa; x < xb; x++) {
					const int s = pw * (y - src_top) - src_left + x;
					if (s < 0 || s >= pw * ph) continue;
					if (img->rgba[size_t(s) * 4 + 3] == 0) continue;
					const int d = (y - Ti) * side - Li + x;
					if (d < 0 || d >= side * side) continue;
					out.land[size_t(d)] = 1;
				}
			}
		}
		const float ccx = float(c.cx + shift), ccy = float(c.cy);
		if (!(ccx < clip_l) && clip_r > ccx && !(ccy < clip_t) && clip_b > ccy) {
			out.ids.push_back(int(i));
			out.xs.push_back(c.cx + shift - Li);
			out.ys.push_back(c.cy - Ti);
		}
	}
	return true;
}

// ---------------------------------------------------------------- import_image

// ScenarioEditor::import_image @9a3f30: every wcoord averages the box of image pixels it covers (both ends inclusive); white (water) is 1,
// land 0, and a cell is water when the average reaches percent / 100. Buffer::get_pixel leaves its output untouched for a pixel outside the
// image, so the box of the last row / column re-reads the previous pixel (`cur` below is the stale colour).
void Gen::conquest_import(const ConquestInput& ci, int percent) {
	const float fside = float(ci.side);
	const float threshold = float(percent) / 100.0f;
	uint32_t cur = 0;
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			const int x0 = int((float(x) / float(xs)) * fside);
			const int y0 = int((float(y) / float(ys)) * fside);
			const int x1 = int((float(x + 1) / float(xs)) * fside);
			const int y1 = int((float(y + 1) / float(ys)) * fside);
			float sum = 0.0f;
			for (int px = x0; px <= x1; px++) {
				for (int py = y0; py <= y1; py++) {
					if (px >= 0 && py >= 0 && px < ci.side && py < ci.side) cur = ci.land[size_t(py) * size_t(ci.side) + size_t(px)] ? 0xff000000u : 0xffffffffu;
					const uint32_t rgb = cur & 0xffffffu;
					if (rgb != 0) sum = float(rgb) / 16777215.0f + sum;
				}
			}
			const int count = (y1 - y0 + 1) * (x1 - x0 + 1);
			const float avg = count > 0 ? sum / float(count) : 0.0f;
			WData& d = W(x, y);
			d.flags = uint16_t(d.flags & 0xffc3);
			d.land = threshold <= avg ? 2 : 0;
		}
	}
	find_all();
	force_continent_spacing();
	make_coastlines();
}

// ---------------------------------------------------------------- Map::force_continent_spacing @68e6a0

// Land regions in ascending size (a new entry goes before the first of at least its size: ordered_insert); every edge cell of a region
// (is_edge_of_region: a neighbour of another region) with a land cell of another region within the 48 cells of move_x / move_y[1..48] becomes
// water and leaves the region's cell list. The region's size drops once per offending neighbour (the original decrements per hit even when
// the cell was already removed), and the walk over the list continues at the next index, so the cell that slides into the freed slot is skipped.
void Gen::force_continent_spacing() {
	std::vector<std::pair<int, int>> order;  // (region, size)
	for (int id = 1; id < 64; id++) {
		const int sz = int(int16_t(reg[size_t(id)].size));
		if (sz <= 0) continue;
		size_t pos = 0;
		while (pos < order.size() && order[pos].second < sz) pos++;
		order.insert(order.begin() + long(pos), {id, sz});
	}
	for (const auto& e : order) {
		const int id = e.first;
		GenRegion& R = reg[size_t(id)];
		for (size_t i = 0; i < R.cells.size(); i++) {
			const WCoord c = R.cells[i];
			bool edge = false;
			for (int k = 1; k <= 8 && !edge; k++) {
				const int nx = c.x + move_x[k], ny = c.y + move_y[k];
				if (inb(nx, ny) && W(nx, ny).region != W(c.x, c.y).region) edge = true;
			}
			if (!edge) continue;
			for (int k = 1; k <= 48; k++) {
				const int nx = c.x + move_x[k], ny = c.y + move_y[k];
				if (!inb(nx, ny)) continue;
				const WData& n = W(nx, ny);
				if (!(n.flags & 0x100) && (n.land == 2 || n.land == 1)) continue;
				if (n.region == id) continue;
				W(c.x, c.y).land = 2;
				for (size_t j = 0; j < R.cells.size(); j++) {
					if (R.cells[j].x == c.x && R.cells[j].y == c.y) {
						R.cells.erase(R.cells.begin() + long(j));
						break;
					}
				}
				R.size--;
			}
		}
	}
}

// ---------------------------------------------------------------- MapConquest::check_starting_loc @68d7e0

// `mindist` = the start-to-start distance (ECX), `r` = the border / ring radius (EDX, capped at 0x40). want_land: no water cell on circle
// indices 1 .. circle_radius[r] - 1 around the point (a half-land cell counts as land). The rough-terrain test (4th stack argument) is not
// used by any caller and the list argument is always null here.
bool Gen::cq_check_starting_loc(int x, int y, int mindist, int r, bool want_land, int skip_player) {
	if (x < 0 || y < 0 || x >= xs || y >= ys) return false;
	if (r > 0x40) r = 0x40;
	if (r < 0) r = 0;
	const Circles& C = circles();
	if (want_land) {
		const int rad = C.radius[size_t(r)];
		for (int i = 0; i + 1 < rad; i++) {
			const int nx = C.x[size_t(i + 1)] + x, ny = C.y[size_t(i + 1)] + y;
			if (!inb(nx, ny)) continue;
			const WData& d = W(nx, ny);
			if (!(d.flags & 0x100) && (d.land == 2 || d.land == 1)) return false;
		}
	}
	if (x < r || y < r || x >= xs - r || y >= ys - r) return false;
	if (skip_player < 0) {
		for (const WCoord& s : starts) {
			if (vd(std::abs(s.x - x), std::abs(s.y - y)) < mindist) return false;
		}
	} else if (skip_player < int(starts.size())) {
		const WCoord& s = starts[size_t(skip_player)];
		if (vd(std::abs(s.x - x), std::abs(s.y - y)) < mindist) return false;
	}
	return true;
}

void Gen::cq_push_start(int x, int y, int who, int node, int armies) {
	add_starting_location(x, y);
	conq_starts.push_back({who, node, armies, x, y});
}

// ---------------------------------------------------------------- MapConquest::add_additional_players @68d260

// Seats 2.. : the link's supporters first (each around the start of the side it is allied to: the first start, or the second when it
// is allied to the defended node's owner; the first passing point of circle indices [circle_radius[city_spacing / 2], circle_radius[64]) ),
// then the barbarian seats (a region of the two starts, one game_random draw for the walk's first cell, check_starting_loc per cell).
// `region_b` / `region_a` = the region ids under the starts of b / a (-1 without that start; EDX and the first stack argument in the original).
void Gen::cq_add_additional_players(const ConquestInput& ci, int region_b, int region_a) {
	const int m = ci.barbarians + int(ci.supporters.size());
	if (m <= 0) return;
	const int num_sea = rg.get_num_sea();
	const int cs = int(rules.constants.at(uint32_t(constant_offset("city_spacing"))));
	const int fcnc = int(rules.constants.at(uint32_t(constant_offset("first_city_near_coast"))));
	const int r_near = (fcnc + ((fcnc >> 31) & 3)) >> 2;
	const int r_far = ((cs * 2) + (((cs * 2) >> 31) & 3)) >> 2;
	const Circles& C = circles();
	const int plus = (ci.node_a != ci.node_b ? 1 : 0) + 1;
	for (int idx = 0; idx < m; idx++) {
		bool found = false;
		if (idx < int(ci.supporters.size())) {
			const ConquestSupporter& sp = ci.supporters[size_t(idx)];
			const size_t near = size_t(sp.near_second ? 1 : 0);
			if (near >= starts.size()) {
				note("supporting nation " + std::to_string(idx) + ": the start it surrounds does not exist");
				continue;
			}
			const WCoord base = starts[near];
			const int lo = C.radius[size_t(std::max(0, std::min(64, r_far)))], hi = C.radius[64];
			for (int i = lo; i < hi; i++) {
				const int x = C.x[size_t(i)] + base.x, y = C.y[size_t(i)] + base.y;
				if (cq_check_starting_loc(x, y, r_far, r_near, num_sea != 0, -1)) {
					cq_push_start(x, y, plus + idx, -1, sp.armies);
					found = true;
					break;
				}
			}
			if (!found) note("Unable to place supporting nation " + std::to_string(idx));
		} else {
			int list[2];
			if (region_b != 0) {
				list[0] = region_b;
				list[1] = region_a;
			} else {
				list[0] = region_a;
				list[1] = 0;
			}
			for (int li = 0; li < 2 && !found; li++) {
				if (list[li] < 0 || list[li] >= Regions::kNum) continue;
				const std::vector<WCoord>& cells = reg[size_t(list[li])].cells;
				const int n = int(cells.size());
				const int start = r.rnd(n);  // Random::get(game_random, 0, 0xffff) % size, drawn only when size > 1
				for (int k = 0; k < n; k++) {
					const WCoord c = cells[size_t((start + k) % n)];
					if (cq_check_starting_loc(c.x, c.y, cs, r_near, num_sea != 0, -1)) {
						cq_push_start(c.x, c.y, plus + idx, -1, 0);
						found = true;
						break;
					}
				}
			}
			if (!found) note("unable to place barbarian city " + std::to_string(idx));
		}
	}
}

// MapConquest::check_sea_map @68d1a0: without sea regions sea_map = 0; with them 3 when the starts are not all in one region, else 2.
void Gen::cq_check_sea_map() {
	if (rg.get_num_sea() == 0) {
		sea_map = 0;
		return;
	}
	int first = -1;
	for (const WCoord& s : starts) {
		const int rid = W(s.x, s.y).region;
		if (first < 0) first = rid;
		else if (first != rid) {
			sea_map = 3;
			return;
		}
	}
	sea_map = 2;
}

// ---------------------------------------------------------------- MapConquest::make_continents @68dba0

bool Gen::mc_conquest() {
	const ConquestInput* ci = conquest_input();
	if (!ci || !ci->valid || ci->side <= 0) throw std::runtime_error("No conquest map data available.");
	conq_starts.clear();
	wipe();
	clear_all();
	conquest_import(*ci, 50);
	find_all();
	auto find_id = [&](int node) {
		for (size_t i = 0; i < ci->ids.size(); i++) {
			if (ci->ids[i] == node) return int(i);
		}
		return -1;
	};
	const int idx_a = ci->node_a == ci->node_b ? -1 : find_id(ci->node_a);
	const int idx_b = find_id(ci->node_b);
	auto world_xy = [&](int idx, int* x, int* y) {
		*x = int((float(ci->xs[size_t(idx)]) / float(ci->side)) * float(xs));
		*y = int((float(ci->ys[size_t(idx)]) / float(ci->side)) * float(xs));
	};
	int region_b = -1, region_a = -1;
	if (idx_b >= 0) {
		int x, y;
		world_xy(idx_b, &x, &y);
		cq_push_start(x, y, ci->swap ? ci->who_a : ci->who_b, ci->node_b, 0);
		region_b = inb(x, y) ? int(W(x, y).region) : -1;
	}
	if (idx_a >= 0) {
		int x, y;
		world_xy(idx_a, &x, &y);
		cq_push_start(x, y, ci->swap ? ci->who_b : ci->who_a, ci->node_a, 0);
		region_a = inb(x, y) ? int(W(x, y).region) : -1;
	}
	cq_add_additional_players(*ci, region_b, region_a);
	cq_check_sea_map();
	// A seat without a start (the original reports an error and the battle would be broken): put it on the first land cell that passes
	// the distance test around the first start, then anywhere, so the battle is always playable.
	// The seat of a main country whose centre is outside the drawn square (conquest map styles 4-7 centre the square on one country) has no start
	// by design: it gets no city and its armies come in by reinforcement.
	std::vector<int> have;
	for (const ConquestStart& s : conq_starts) have.push_back(s.who);
	if (idx_b < 0) have.push_back(ci->swap ? ci->who_a : ci->who_b);
	if (idx_a < 0) have.push_back(ci->swap ? ci->who_b : ci->who_a);
	for (int who = 0; who < nations; who++) {
		if (std::find(have.begin(), have.end(), who) != have.end()) continue;
		bool done = false;
		const int cs = int(rules.constants.at(uint32_t(constant_offset("city_spacing"))));
		for (int pass = 0; pass < 3 && !done; pass++) {
			const int mind = pass == 0 ? cs / 2 : (pass == 1 ? cs / 4 : 0);
			for (int y = 2; y < ys - 2 && !done; y++) {
				for (int x = 2; x < xs - 2 && !done; x++) {
					if (is_water(x, y)) continue;
					if (!cq_check_starting_loc(x, y, mind, 2, true, -1)) continue;
					cq_push_start(x, y, who, -1, 0);
					note("seat " + std::to_string(who) + " had no start: fallback placement (" + std::to_string(x) + "," + std::to_string(y) + ")");
					done = true;
				}
			}
		}
	}
	return true;
}

}  // namespace ron::sim::mapgen
