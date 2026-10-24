// The per-style continent recipes (docs/randommap-terrain-groups.md B2-B3, randommap-spec.md section 6): Map::set_map and
// every Map*::make_continents. Each recipe wipes the map, grows land regions and places the start locations. A recipe
// returns false when its retry loop gave up; the driver then falls back to the guaranteed ring layout.
#include <array>

#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

namespace {

uint32_t player_step(int n) { return 0xffffffffu / uint32_t(std::max(1, n)); }

int isqrt_newton(int v) {
	if (v < 2) return v;
	int x = v / 2;
	for (;;) {
		const int y = (x + v / x) >> 1;
		if (y >= x) break;
		x = y;
	}
	return x;
}

}  // namespace

// Map::set_map @6a1340: styles 0..5 pick one of 6..22 by a class table.
void Gen::set_map() {
	if (style >= 6) return;
	// ❓ randommap-spec §2: the class bytes were read from a stack image (2 = land map, 1 = sea map, 0 = neither); the
	// table puts Mediterranean among the land maps and Great Lakes among the sea maps, which may be mis-ordered.
	static const int cls[17] = {0, 0, 2, 2, 2, 2, 2, 2, 1, 2, 2, 1, 1, 1, 1, 1, 1};
	for (int guard = 0; guard < 4000; guard++) {
		const int i = r.rnd(17);
		const int s = 6 + i, c_ = cls[i];
		bool ok;
		switch (style) {
		case 1:
		case 4: ok = c_ == 2 && !(s == kStyleWarringStates && P.team_style == 2); break;
		case 2:
		case 5: ok = c_ == 1 && s != kStyleColonialPowers && !(P.team_style == 2 && s == kStyleWarringStates); break;
		default:
			ok = c_ != 0 && s != kStyleNewWorld && s != kStyleColonialPowers &&
			     !(P.team_style == 2 && s == kStyleWarringStates);
			break;
		}
		if (ok) {
			style = s;
			return;
		}
	}
	style = kStyleOldWorld;
}

// The guaranteed layout used when a recipe gives up: all land, the starts on a ring.
void Gen::fallback_ring() {
	res.fallback = true;
	wipe();
	clear_all();
	invert_land();
	const int h = std::min(xs, ys) / 2;
	const int R = std::max(2, std::min(int(0.8 * h), h - 4));
	uint32_t angle = 0;
	const uint32_t step = player_step(nations);
	for (int i = 0; i < nations; i++) {
		angle += step;
		int x, y;
		project_pt(xs / 2, ys / 2, angle, R, &x, &y);
		add_starting_location(x, y);
	}
}

bool Gen::make_continents() {
	attempts = 0;
	switch (style) {
	case kStyleOldWorld:
	case kStyleSahara:
	case kStyleAmazon:
	case kStyleHimalayas:
	case kStyleSouthwestMesa: return mc_all_land_ring();
	case kStyleAfrica: return mc_africa();
	case kStyleMediterranean: return mc_mediterranean();
	case kStyleOutback: return mc_outback();
	case kStyleGreatLakes: return mc_great_lakes();
	case kStyleWarringStates: return mc_warring_states();
	case kStyleNewWorld: return mc_new_world();
	case kStyleColonialPowers: return mc_colonial_powers();
	case kStyleEastIndies: return mc_east_indies();
	case kStyleEastMeetsWest: return mc_east_meets_west();
	case kStyleSeaPower: return mc_sea_power();
	case kStyleNileDelta: return mc_nile_delta();
	case kStyleBritishIsles: return mc_british_isles();
	case kStyleBlank: return mc_grass();
	default: return false;
	}
}

// MapGrass::make_continents (the blank map): every cell land, the starts on a circle of radius min(xs, ys) / 3.
bool Gen::mc_grass() {
	wipe();
	clear_all();
	for (WData& d : w.wdata) {
		d.land = 0;
		d.land_sub = 0;
	}
	const int R = std::min(xs, ys) / 3;
	uint32_t angle = 0;
	const uint32_t step = player_step(nations);
	for (int i = 0; i < nations; i++) {
		int x, y;
		project_pt(xs / 2, ys / 2, angle, R, &x, &y);
		add_starting_location(x, y);
		angle += step;
	}
	return true;
}

// B3.1 Old World, Great Sahara, Amazon Basin, Himalayas, Southwest Mesa (the five functions are identical).
bool Gen::mc_all_land_ring() {
	wipe();
	clear_all();
	invert_land();
	const int h = std::min(xs, ys) / 2;
	const int R = std::max(2, std::min(int(0.8 * h), h - 4));
	uint32_t angle;
	rnd_angle_pair(&angle);
	const int cx = xs / 2 + r.bit();
	const int cy = ys / 2 + r.bit();
	start_dist = std::max(xs / 16, 4);
	const uint32_t step = player_step(nations);
	for (int i = 0; i < nations; i++) {
		angle += step;
		int x, y;
		project_pt(cx, cy, angle, R, &x, &y);
		add_starting_location(x, y);
	}
	return true;
}

// B3.2 African Watering Hole: a 5% water blob at the centre, inverted.
bool Gen::mc_africa() {
	avoid_continent = std::max(4, scale_land_area(avoid_continent, nations));
	const uint32_t step = player_step(nations);
	const int h = std::min(xs, ys) / 2;
	const int R = std::max(2, std::min(int(0.8 * h), h - 4));
	uint32_t angle = 0;
	for (;;) {
		wipe();
		clear_all();
		rnd_angle_pair(&angle);
		const int goal = cells * 5 / 100;
		const int cx = xs / 2 + r.bit();
		const int cy = ys / 2 + r.bit();
		make_region(1, cx, cy, goal);
		if (grow_region(1, goal, R, -1, -1, 0) != 0) {
			if (give_up()) return false;
			res.retries++;
			continue;
		}
		break;
	}
	invert_land();
	const int cx = xs / 2 + r.bit();
	const int cy = ys / 2 + r.bit();
	start_dist = std::max(xs / 16, 4);
	for (int i = 0; i < nations; i++) {
		angle += step;
		int x, y;
		project_pt(cx, cy, angle, R, &x, &y);
		add_starting_location(x, y);
	}
	return true;
}

// MapMediterranean::make_continents @69add0 (decompile and asm read)
bool Gen::mc_mediterranean() {
	avoid_continent = std::max(4, scale_land_area(avoid_continent, nations));
	const int n = nations;
	const int R_ck = (rules.types[0x15d].max_range + 3) / 4;  // ceil(max_range(Battleship) / 4)
	const int m = ((std::min(xs, ys) & ~1) / 3);
	const uint32_t step = player_step(n);
	for (;;) {
		int cx = 0, cy = 0;
		uint32_t angle = 0;
		for (;;) {
			wipe();
			clear_all();
			const int land_goal = 3 * m * m;
			rnd_angle_pair(&angle);
			cx = xs / 2 + r.bit();
			cy = ys / 2 + r.bit();
			make_region(1, cx, cy, land_goal);
			if (grow_region(1, land_goal, m, -1, -1, 0) == 0) break;
			if (give_up()) return false;
			res.retries++;
		}
		find_all();
		eliminate_pools(kEntire);
		clear_all();
		invert_land();
		find_all();
		eliminate_pools(kEntire);
		clear_all();
		for (int i = 0; i < n; i++) {
			angle += step;
			const int r_start = (xs * 3) / 2;
			int radius = r_start;
			bool found = false;
			int px = 0, py = 0;
			for (int guard = 0; guard < 20000 && !found; guard++) {
				project_pt(cx, cy, angle, radius, &px, &py);
				radius -= 3;
				if (radius < m) {
					angle += 0x71c71c6u;
					radius = r_start;
					continue;
				}
				bool edge = false;
				for (int o = 0; o <= 4; o++) {
					const int nx = px + orthog_x[o], ny = py + orthog_y[o];
					if (!inb(nx, ny) || is_edge(nx, ny)) edge = true;
				}
				if (!inb(px, py)) continue;
				const WData& d = W(px, py);
				if (!(d.flags & 0x100) && (d.land == 1 || d.land == 2)) continue;
				if (edge) continue;
				if (!is_near_ocean(px, py, 3, 9)) continue;
				found = true;
			}
			if (!found) {
				// ❓ no ray reached a cell that passed the coast test: any inland land cell near the middle
				for (int rr = m; rr < xs && !found; rr++) {
					project_pt(cx, cy, angle, rr, &px, &py);
					if (inb(px, py) && !is_water(px, py) && !is_edge(px, py)) found = true;
				}
			}
			add_starting_location(px, py);
		}
		check_player_land(R_ck);
		const int goal1 = std::max(0x4b, scale_land_area(0x96, n));
		cx = xs / 2 + r.bit();
		cy = ys / 2 + r.bit();
		make_region(0x20, cx, cy, goal1);
		if (grow_region(0x20, goal1, m, -1, -1, 0) == 0) {
			eliminate_pools(kEdges);
			return true;
		}
		if (give_up()) return false;
		res.retries++;
	}
}

// B3.3 Australian Outback
bool Gen::mc_outback() {
	const uint32_t step = player_step(nations);
	const int n = nations;
	for (;;) {
		wipe();
		clear_all();
		const int h = std::min(xs, ys) / 2;
		const int Rg = h * 55 / 100, Rs = h * 66 / 100;
		uint32_t angle;
		rnd_angle_pair(&angle);
		const int goal0 = xs * ys * 5 / 9;
		const int cx = xs / 2 + r.bit();
		const int cy = ys / 2 + r.bit();
		start_dist = std::max(xs / 16, 2);
		uint32_t mask = 0;
		for (int i = 1; i <= n; i++) {
			int px, py;
			project_pt(cx, cy, angle, Rs, &px, &py);
			add_starting_location(px, py);
			make_region(i, px, py, goal0);
			reg[size_t(i)].flags |= 1;
			reg[size_t(i)].climate = 1;
			mask |= 1u << (i & 31);
			angle += step;
		}
		mask |= 1u << 9;
		make_region(9, cx, cy, cells);
		reg[9].climate = 0;
		for (int j = 0; j < n; j++) grow_region((j & 7) + 1, (goal0 / n) / 4, Rg, cx, cy, 0);
		for (int j = 0; j < n; j++) grow_region((j & 7) + 1, (goal0 / n) / 2, Rg, cx, cy, 0);
		reg[9].flags |= 1;
		grow_region(9, goal0 / 3, Rg / 3, -1, -1, 0);
		const int step_sz = std::max(1, goal0 / 64);
		bool joined = false;
		for (int round = 0; round < 300 && !joined; round++) {
			if ((reg[9].mask & mask) == mask) {
				for (int j = 0; j < n; j++) {
					const int rid = (j & 7) + 1;
					GenRegion& gr = reg[size_t(rid)];
					for (const WCoord& c : gr.cells) {
						W(c.x, c.y).region = 9;
						reg[9].cells.push_back(c);
						reg[9].size++;
					}
					gr.cells.clear();
					gr.size = 0;
				}
				grow_region(9, goal0, Rg, -1, -1, 0);
				eliminate_pools(kEntire);
				clear_all();
				joined = true;
				break;
			}
			for (int k = 0; k < std::min(n, 8); k++) {
				const int rid = k + 1;
				if (reg[size_t(rid)].mask & 0x200) continue;
				if (goal0 < reg[size_t(rid)].size + step_sz) {
					grow_region(9, reg[9].size + step_sz, Rg / 4, -1, -1, 0);
				} else if (grow_region(rid, reg[size_t(rid)].size + step_sz, Rg, cx, cy, 0) != 0) {
					grow_region(9, reg[9].size + step_sz, Rg / 4, -1, -1, 0);
				}
			}
		}
		if (joined) return true;
		if (give_up()) return false;
		res.retries++;
	}
}

// B3.4 / B3.8 scatter loop: `count` blobs with the position search of Great Lakes (land_dist >= need).
bool Gen::scatter_lakes(bool land_regions, int first_region, int count) {
	const int Smax0 = std::max(cells / 20, 90);
	int Smin = std::max(cells / 40, 24), Smax_cur = Smax0, size = Smax0;
	int left = count, iter = 0, rid = first_region;
	while (left != 0 && ++iter < 100) {
		const int rho = isqrt_newton(size * 7 / 22);
		const int need = std::max(3, (avoid_continent + 1) / 2 + rho);
		int tries = 0, x, y;
		do {
			x = xs > 1 ? r.rnd_draw(xs) : 0;
			y = ys > 1 ? r.rnd_draw(ys) : 0;
		} while (land_dist(x, y, 1) < need && ++tries < 1000);
		if (tries < 1000) {
			if (grow_valid(rid + 1, x, y) && rid + 1 < 32) {
				rid++;
				make_region(rid, x, y, size);
				grow_region(rid, size, xs, -1, -1, 0);
				if (land_regions) {
					reg[size_t(rid)].common = reg[size_t(rid)].goody = 5;
					reg[size_t(rid)].climate = 1;
				}
				left--;
				iter = 0;
			}
			size = Smin + (Smax_cur - Smin > 1 ? r.rnd(Smax_cur - Smin) : 0);
		} else if (size > Smin) {
			Smax_cur = size = Smin;
		} else {
			Smin = Smin * 13 / 16;
			Smax_cur = size = Smin;
			if (Smin <= 19) break;
		}
	}
	return true;
}

// B3.4 Great Lakes
bool Gen::mc_great_lakes() {
	const int R_ck = (rules.types[0x15d].max_range + 3) / 4;
	avoid_continent = std::max(6, scale_land_area(avoid_continent, nations));
	const uint32_t step = player_step(nations);
	wipe();
	clear_all();
	uint32_t angle;
	rnd_angle_pair(&angle);
	const int lakes = xs / 12 + r.bit();
	scatter_lakes(false, 0, lakes);
	eliminate_pools(kEntire);
	invert_land();
	for (int i = 0; i < nations; i++) {
		angle += step;
		int R = xs * 3 / 2, x = xs / 2, y = ys / 2;
		bool found = false;
		for (int guard = 0; guard < xs * 2 && !found; guard++) {
			project_pt(xs / 2, ys / 2, angle, R, &x, &y);
			R -= 3;
			bool inside = true;
			for (int o = 0; o <= 4; o++) {
				const int nx = x + orthog_x[o], ny = y + orthog_y[o];
				if (!inb(nx, ny) || is_edge(nx, ny)) inside = false;
			}
			if (inside && !is_water(x, y)) found = true;
		}
		if (!found) {
			x = xs / 2;
			y = ys / 2;
		}
		add_starting_location(x, y);
	}
	check_player_land(R_ck);
	return true;
}

// MapFairness @68a100 (randommap-resources section 9): the team-balancing bookkeeping Warring States uses for its extra islands.
namespace {

struct Fairness {
	std::array<float, 8> fairness{}, dists{};
	std::array<int, 8> teams{{0, 1, 2, 3, 4, 5, 6, 7}};
	int lowest_player = 0, highest_player = 0, lowest_dist = 0, highest_dist = 0, num_players = 0;
	void recompute() {
		float lo = 1e12f, hi = 0.0f;
		for (int i = 0; i < num_players; i++) {
			const float f = fairness[size_t(teams[size_t(i)])];
			if (f < lo) {
				lowest_player = teams[size_t(i)];
				lo = f;
			}
			if (hi < f) {
				highest_player = teams[size_t(i)];
				hi = f;
			}
		}
	}
	void apply() {
		for (int i = 0; i < num_players; i++) fairness[size_t(teams[size_t(i)])] += dists[size_t(teams[size_t(i)])];
	}
	void unapply() {
		for (int i = 0; i < num_players; i++) fairness[size_t(teams[size_t(i)])] -= dists[size_t(teams[size_t(i)])];
	}
};

}  // namespace

// B3.5 Warring States (MapWarringStates::make_continents @698b60, decompile and asm read): a chain of small "states" crossing the
// map, the leaders spread over the chain by team, the states grown until they touch and merged into one continent, then
// MapFairness-balanced extra islands.
bool Gen::mc_warring_states() {
	const int n = nations;
	avoid_continent = std::max(4, scale_land_area(avoid_continent, n));
	avoid_modify = 0;
	wipe();
	clear_all();
	const int mn = std::min(xs, ys);
	const int rho = mn / 2 - avoid_edge[0] - avoid_continent;
	uint32_t angle0;
	rnd_angle_pair(&angle0);
	const int goal = cells / 7;
	int x, y;
	project_pt(xs / 2, ys / 2, angle0, rho, &x, &y);
	int k = (n & 1) ? n + 1 : n;
	if (k < 1) k = 1;
	while (k < 6) k *= 2;
	const int stepR = (rho * 2) / k;
	start_dist = stepR / 4;
	std::vector<WCoord> seeds;
	uint32_t mask = 0;
	int s = 0;
	for (;;) {
		if (!inb(x, y) || s >= 40) break;  // (the original writes out of the map here)
		s++;
		make_region(s, x, y, goal);
		reg[size_t(s)].flags |= 1;
		mask |= 1u << (s & 31);
		seeds.push_back({x, y});
		const uint32_t heading = angle0 + ((uint32_t(r.mask(0x3fff)) + 0x6000u) << 16);
		project_pt(x, y, heading, stepR, &x, &y);
		const int dd = vd(std::abs(x - xs / 2), std::abs(y - ys / 2));
		if (!(dd <= rho + avoid_continent && dd <= mn - avoid_continent)) break;
	}
	if (s == 0) return false;
	// the order in which seeds are handed out: the far end of the chain first, then the near end, back to the start
	const int shift = (get_player_overage(n) != 0 || s < 2 * n) ? 1 : 0;
	std::vector<int> bq, evens;
	for (int i = 0; i < n; i++) {
		if (i & 1) bq.push_back(std::min(s - 1, std::max(0, s - ((i - 1) >> shift) - 1)));
		else evens.push_back(std::min(s - 1, i >> shift));
	}
	for (size_t j = evens.size(); j > 0; j--) bq.push_back(evens[j - 1]);
	std::array<int, 8> count{}, cont{};
	std::vector<int> extra;
	int sides = 0;
	fill_cont(count, cont, &extra, &sides);
	auto team_of = [&](int slot) { return (P.team[size_t(slot)] >= 0 && P.team[size_t(slot)] < 8) ? P.team[size_t(slot)] : 8; };
	std::array<int, 8> assigned{}, used{};
	std::array<WCoord, 8> loc{};
	int cur = 0;
	for (size_t idx = 0; idx < bq.size(); idx++) {
		int ei = 0;
		bool pending = false;
		for (int L = 0; L < n; L++) {
			if (assigned[size_t(L)]) continue;
			int pt;
			if (team_of(L) == 8) {
				while (ei < 8 && used[size_t(ei)]) ei++;
				pt = ei < int(extra.size()) ? extra[size_t(ei)] : 7;
				ei++;
			} else {
				pt = team_of(L);
			}
			if (cont[size_t(pt)] != cur) continue;
			assigned[size_t(L)] = 1;
			loc[size_t(L)] = seeds[size_t(bq[idx])];
			if (team_of(L) == 8 && ei >= 1) used[size_t(ei - 1)] = 1;
			for (int L2 = L + 1; L2 < n; L2++) {
				if (team_of(L2) != 8 && cont[size_t(team_of(L2))] == cur) {
					pending = true;
					break;
				}
			}
			break;
		}
		if (!pending) cur++;
	}
	for (int L = 0; L < n; L++) {
		const WCoord c0 = assigned[size_t(L)] ? loc[size_t(L)] : seeds[size_t(std::min(L, s - 1))];
		add_starting_location(c0.x, c0.y);
	}
	// grow every state until the land is large enough and all the states touch (or a whole round stalls)
	for (int guard = 0; guard < 4000; guard++) {
		int fails = 0, total = 0;
		for (int q = 1; q <= s; q++) {
			if (grow_region(q, reg[size_t(q)].size + 0x30, xs, -1, -1, 0) != 0) fails++;
			total += reg[size_t(q)].size;
		}
		if (!((total < goal || (reg[1].mask & mask) != mask) && fails < s)) break;
	}
	for (int q = 2; q <= s; q++) {
		GenRegion& gr = reg[size_t(q)];
		for (const WCoord& c0 : gr.cells) {
			W(c0.x, c0.y).region = 1;
			reg[1].cells.push_back(c0);
			reg[1].size++;
		}
		gr.cells.clear();
		gr.size = 0;
	}
	find_all();  // Regions::clear_all + find_all
	// the extra islands: the first sea region (the ocean) is dissolved so that its cells can be claimed again
	int blobs = xs / 15 + r.bit();
	Fairness F;
	F.num_players = n;
	static const int base_tab[6] = {2, 2, 3, 4, 6, 8};
	const int base = map_size >= 0 && map_size < 6 ? base_tab[map_size] : 8;
	if (base < n) {
		const int m2 = std::max(n / base, (n + 1) / base);
		for (int i = 0; i < n; i++) F.teams[size_t(i)] = i / m2;
	}
	int m = base > n ? blobs : std::max(blobs, n);
	if (m & 1) m++;
	std::vector<WCoord> cand = reg[65].cells;
	for (const WCoord& c0 : cand) W(c0.x, c0.y).region = 0;
	reg[65].cells.clear();
	reg[65].size = 0;
	const int cnt = int(cand.size());
	int attempts = 0, rid2 = 1;
	bool relax = false;
	auto calc_distances = [&](int px, int py) {
		float lo = 1e12f, hi = 0.0f;
		for (int i = 0; i < F.num_players; i++) {
			const int p = F.teams[size_t(i)];
			if (size_t(p) >= starts.size()) continue;
			F.dists[size_t(p)] = float(vd(starts[size_t(p)].x - px, starts[size_t(p)].y - py)) * 1.0f;
			if (F.dists[size_t(p)] < lo) {
				F.lowest_dist = p;
				lo = F.dists[size_t(p)];
			}
			if (hi < F.dists[size_t(p)]) {
				F.highest_dist = p;
				hi = F.dists[size_t(p)];
			}
		}
	};
	for (int guard = 0; m > 0 && cnt > 0 && guard < 200; guard++) {
		const int v = 0xc1 + r.rnd_draw(0x37);
		const int rho2 = isqrt_newton(v);
		const int need = std::max(5, (avoid_continent + rho2) >> 1);
		int start = 1;
		if (cnt - 1 >= 1) {
			start = r.rnd_draw(cnt);
			if (start == 0) start = 1;
		}
		int idx = start;
		bool accepted = false;
		int px = 0, py = 0;
		for (int it = 0; it < 2 * cnt + 4; it++) {
			if (idx == cnt) idx = 0;
			px = cand[size_t(idx)].x;
			py = cand[size_t(idx)].y;
			if (land_dist(px, py, 1) >= need) {
				calc_distances(px, py);
				if (relax || F.lowest_dist == F.highest_player) {
					accepted = true;
					break;
				}
			}
			idx++;
			if (idx == start - 1) break;
		}
		bool failed = !accepted;
		if (accepted) {
			F.apply();
			F.recompute();
			do {
				rid2++;
			} while (rid2 < 63 && reg[size_t(rid2)].size != 0);
			make_region(rid2, px, py, 0xdc);
			const int rc = grow_region(rid2, 0xdc, need + 1, -1, -1, 0);
			reg[size_t(rid2)].common = reg[size_t(rid2)].goody = 6;
			if (rc == 0) {
				m--;
				relax = false;
				attempts = 0;
			} else {
				failed = true;
			}
		}
		if (failed) {
			attempts++;
			relax = true;
			if (attempts < 5) {
				F.unapply();
				F.recompute();
			} else {
				m--;
			}
		}
	}
	check_player_land(-1, false);
	return true;
}

// B3.6 New World
bool Gen::mc_new_world() {
	avoid_continent = std::max(4, scale_land_area(avoid_continent, nations));
	for (int k = 0; k < 4; k++) avoid_edge[size_t(k)] = scale_land_area(avoid_edge[size_t(k)], nations);
	const uint32_t step = player_step(nations);
	const int n = nations;
	for (;;) {
		wipe();
		clear_all();
		avoid_center = 0;
		const int m = std::min(xs, ys);
		const int P_ = std::max(60, scale_land_area(250, n));
		int C_ = scale_land_area((cells - P_ * n) * 4 / 10, n);
		const uint32_t d1 = uint32_t(r.rnd_draw(0xffff));
		const int rho = (m / 2) * 7 / 8;
		uint32_t angle;
		if (n < 4) {
			angle = (d1 & 3u) * 0x3fffffffu + 0x1fffffffu;
			if (n != 3) C_ = (cells - P_ * n) * 2 / 3;
		} else {
			const uint32_t d2 = uint32_t(r.rnd_draw(0xffff));
			angle = ((d1 % 0xffffu) << 16) + (d2 % 0xffffu);
		}
		for (int i = 1; i <= n; i++) {
			angle += step;
			int x, y;
			project_pt(xs / 2, ys / 2, angle, rho, &x, &y);
			add_starting_location(x, y);
			make_region(i, x, y, P_);
			reg[size_t(i)].flags |= 2;
			reg[size_t(i)].common = reg[size_t(i)].goody = 12;
		}
		make_region(9, xs / 2, ys / 2, C_);
		reg[9].common = reg[9].goody = 6;
		avoid_center = scale_land_area((xs / 2) * 70 / 100, n);
		const int K = std::max(4, scale_land_area(4, n));
		bool fail = false;
		for (int pass = 0; pass < 2 && !fail; pass++) {
			const int div = pass == 0 ? 2 : 1;
			for (int i = 1; i <= n && !fail; i++) {
				const int sz = grow_region(i, P_ / div, (rho / K) / div, -1, -1, 1);
				if (sz < (P_ / div) / 2) fail = true;
			}
		}
		if (fail) {
			if (give_up()) {
				avoid_center = 0;
				return false;
			}
			res.retries++;
			continue;
		}
		avoid_center = 0;
		grow_region(9, C_, rho, -1, -1, 0);
		check_player_land(-1);
		eliminate_pools(kEntire);
		return true;
	}
}

// B3.7 Colonial Powers
bool Gen::mc_colonial_powers() {
	avoid_continent = std::max(4, scale_land_area(avoid_continent, nations));
	const int n = nations;
	const int saved_center = avoid_center;
	for (;;) {
		wipe();
		clear_all();
		avoid_center = saved_center;
		// ❓ team_style 7 splits the nations between the two continents; the other styles put everybody on region 2
		const int A = 0, B = n;
		const int h = std::min(xs, ys) / 2;
		const int rho = (h * 3) / 4;
		uint32_t angle0;
		rnd_angle_pair(&angle0);
		const int goal = cells * 2 / 3;
		uint32_t angle = angle0;
		for (int g = 0; g < 2; g++) {
			int x, y;
			project_pt(xs / 2, ys / 2, angle, rho, &x, &y);
			const int cnt = g == 0 ? A : B;
			const uint32_t arc = g == 0 ? 0x2aaaaaaau : 0x55555555u;
			uint32_t a2 = angle - arc / 2;
			for (int k = 0; k < cnt; k++) {
				int px, py;
				project_pt(xs / 2, ys / 2, a2, rho, &px, &py);
				px = std::min(xs - 5, std::max(5, px));
				py = std::min(ys - 5, std::max(5, py));
				add_starting_location(px, py);
				if (cnt > 1) a2 += arc / uint32_t(cnt - 1);
			}
			make_region(g + 1, x, y, goal);
			angle += 0x7fffffffu;
		}
		bool fail = false;
		for (int g = 0; g < 2 && !fail; g++) {
			if (g > 0) avoid_center = rho * 7 / 9;
			if (grow_region(g + 1, goal / 2, rho * 3 / 4, -1, -1, 0) != 0) fail = true;
		}
		avoid_center = saved_center;
		if (fail) {
			if (give_up()) return false;
			res.retries++;
			starts.clear();
			continue;
		}
		check_player_land(-1);
		eliminate_pools(kEdges);
		return true;
	}
}

// B3.8 East Indies (MapEastIndies::make_continents @697540, decompile read: both growth passes restart on failure)
bool Gen::mc_east_indies() {
	avoid_continent = std::max(4, scale_land_area(avoid_continent, nations));
	const uint32_t step = player_step(nations);
	const int n = nations;
	for (;;) {
		wipe();
		clear_all();
		const int m = std::min(xs, ys);
		const int G = scale_land_area(250, n);
		const uint32_t d1 = uint32_t(r.rnd_draw(0xffff));
		uint32_t angle;
		int rho;
		if (n < 4) {
			angle = (d1 & 3u) * 0x3fffffffu + 0x1fffffffu;
			rho = n == 3 ? ((m / 2) * 7) / 8 : m / 2;
		} else {
			const uint32_t d2 = uint32_t(r.rnd_draw(0xffff));
			angle = ((d1 % 0xffffu) << 16) + (d2 % 0xffffu);
			rho = ((m / 2) * 7) / 8;
		}
		const int rho0 = rho;
		const int edge = std::max(std::max(avoid_edge[0], avoid_edge[1]), std::max(avoid_edge[2], avoid_edge[3])) + 4;
		for (int i = 1; i <= n; i++) {
			angle += step;
			int x = 0, y = 0;
			for (int guard = 0; guard < 4 * xs; guard++) {
				project_pt(xs / 2, ys / 2, angle, rho, &x, &y);
				if (x > edge && xs - x > edge && y > edge && ys - y > edge) break;
				rho--;
			}
			add_starting_location(x, y);
			make_region(i, x, y, G);
			reg[size_t(i)].common = reg[size_t(i)].goody = 8;
			reg[size_t(i)].flags |= 2;
			reg[size_t(i)].climate = 0;
		}
		bool fail = false;
		for (int i = 1; i <= n && !fail; i++) {
			if (grow_region(i, G / 3, rho0 / 2, -1, -1, 0) != 0) fail = true;
		}
		for (int i = 1; i <= n && !fail; i++) {
			if (grow_region(i, G, rho0 / 2, -1, -1, 0) != 0) fail = true;
		}
		if (fail) {
			if (give_up()) return false;
			res.retries++;
			continue;
		}
		const int count = xs / 12 - n + 8 + r.bit();
		scatter_lakes(true, n, count);
		return true;
	}
}

// B3.9 East Meets West (MapEastMeetsWest::make_continents @696640, decompile and asm read)
bool Gen::mc_east_meets_west() {
	const int n = nations;
	int ac = std::max(4, scale_land_area(avoid_continent, n));
	int fail_counter = 4;
	if (ac > 11) fail_counter = 0;
	if (ac > 16) ac = 16;
	avoid_continent = ac;
	avoid_modify = 0;
	const uint32_t step = player_step(n);
	for (;;) {
		wipe();
		clear_all();
		std::array<int, 8> count{}, cont{};
		int sides = 1;
		fill_cont(count, cont, nullptr, &sides);
		sides = std::max(1, sides);
		const int h = std::min(xs, ys) / 2;
		const int rho = (h * 3) / 4;
		uint32_t angle;
		rnd_angle_pair(&angle);
		const int U = scale_land_area(((cells / n) * 5) / 9, n);
		for (int c_ = 0; c_ < sides; c_++) {
			int x, y;
			project_pt(xs / 2, ys / 2, angle, rho, &x, &y);
			make_region(c_ + 1, x, y, U);
			angle += (uint32_t(count[size_t(c_)]) * step >> 1) + (uint32_t(count[size_t((c_ + 1) % sides)]) * step >> 1);
		}
		bool fail = false;
		for (int c_ = 0; c_ < sides && !fail; c_++) {
			if (grow_region(c_ + 1, (U / 2) * count[size_t(c_)], (rho * 3) / 4, -1, -1, 0) != 0) fail = true;
		}
		for (int c_ = 0; c_ < sides && !fail; c_++) {
			if (grow_region(c_ + 1, count[size_t(c_)] * U, (rho * 3) / 4, -1, -1, 0) != 0) fail = true;
		}
		if (fail) {
			fail_counter++;
			if (fail_counter == 12) avoid_continent = 4;
			else if (fail_counter == 8) avoid_continent = 8;
			else if (fail_counter == 4) avoid_continent = 12;
			if (give_up()) return false;
			res.retries++;
			continue;
		}
		std::vector<WCoord> centroids;
		for (int c_ = 0; c_ < sides; c_++) {
			int x, y;
			find_region_centroid(c_ + 1, &x, &y);
			centroids.push_back({x, y});
		}
		eliminate_pools(kEntire);  // relabels the regions
		eliminate_edge_canals();
		auto region_at = [&](int px, int py) { return inb(px, py) ? int(W(px, py).region) : 0; };
		std::vector<WCoord> placed;
		const int r0 = (rho / 2 * 3) / 2;
		bool retry = false;
		for (int slot = 0; slot < n && !retry; slot++) {
			const int team = (P.team[size_t(slot)] >= 0 && P.team[size_t(slot)] < 8) ? P.team[size_t(slot)] : 8;
			int ci;  // continent index
			if (team == 8) {
				ci = cont[size_t(slot)];
				if (ci == -1) ci = slot;
			} else {
				int c2 = cont[size_t(team)];
				if (c2 == -1) c2 = slot;
				c2 = std::min(c2, sides - 1);
				ci = 0;
				const int want = region_at(centroids[size_t(c2)].x, centroids[size_t(c2)].y);
				do {
					if (region_at(centroids[size_t(std::min(ci, sides - 1))].x, centroids[size_t(std::min(ci, sides - 1))].y) == want) break;
					ci++;
				} while (ci <= sides);
			}
			ci = std::min(std::max(ci, 0), sides - 1);  // (the original reads past its arrays for mixed team / solo seats)
			const WCoord cen = centroids[size_t(ci)];
			const int R = region_at(cen.x, cen.y);
			const uint32_t ang0 = find_angle(xs / 2 - cen.x, ys / 2 - cen.y);
			const int members = count[size_t(ci)];
			uint32_t spread;
			if (members == 1) spread = 0;
			else if (members < 4) spread = 0x2aaaaaaau;
			else spread = uint32_t(0x6aaaaaaall / members);
			int32_t off;
			if (members < 2) off = int32_t(0xaaaaaaa) - r.rnd_draw(0x15555555);
			else off = -int32_t(uint32_t(members >> 1) * spread);
			uint32_t base_ang = ang0 + uint32_t(off);
			int x = 0, y = 0;
			bool have = false;
			const int min_dist = scale_land_area(scale_by_area(n), n);
			if (R > 0 && R < Regions::kNum && reg[size_t(R)].size > 0) have = place_start_in_region(R, &x, &y, min_dist, starts);
			auto dmin_at = [&](int px, int py) {
				int d = 9999;
				for (const WCoord& o : placed) d = std::min(d, vd(px - o.x, py - o.y));
				return d;
			};
			if (!have) {
				int attempt = 0, turns = 0, radius = r0;
				uint32_t ang = base_ang;
				bool relaxed = false;
				for (int guard = 0; guard < 2000 && !have && !relaxed; guard++) {
					attempt++;
					int px, py;
					project_pt(cen.x, cen.y, ang, radius, &px, &py);
					const int dm = dmin_at(px, py);
					bool ok = inb(px, py) && W(px, py).region == R && dm >= 8 && is_near_ocean(px, py, 3, 9);
					if (ok) {
						if (attempt < 30) {
							x = px;
							y = py;
							have = true;
							break;
						}
						relaxed = true;
						break;
					}
					const int nr = radius - 2;
					if (nr < 3 && turns < 3) {
						attempt = 0;
						ang += 0x71c71c6u;
						turns++;
						radius = rho / 2;
					} else {
						radius = nr;
						if (attempt >= 30) relaxed = true;
					}
				}
				if (!have) {
					int rr = r0;
					for (int t = 0; t < 30 && !have; t++, rr -= 2) {
						int px, py;
						project_pt(cen.x, cen.y, base_ang, rr, &px, &py);
						if (inb(px, py) && W(px, py).region == R && dmin_at(px, py) > 5 && is_near_ocean(px, py, 2, 7)) {
							x = px;
							y = py;
							have = true;
						}
					}
				}
				if (!have && R > 0 && R < Regions::kNum) {
					for (const WCoord& cell : reg[size_t(R)].cells) {
						if (dmin_at(cell.x, cell.y) > 7) {
							x = cell.x;
							y = cell.y;
							have = true;
							break;
						}
					}
				}
				if (!have) retry = true;
			}
			if (have) {
				add_starting_location(x, y);
				placed.push_back({x, y});
			}
		}
		if (retry) {
			starts.clear();
			if (give_up()) return false;
			res.retries++;
			continue;
		}
		check_player_land(-1);
		return true;
	}
}

// B3.10 Sea Power
bool Gen::mc_sea_power() {
	const int n = nations;
	int G0 = std::max(80, scale_land_area((cells / n) * 5 / 8, n));
	avoid_continent = std::max(4, scale_land_area(avoid_continent, n));
	const uint32_t step = player_step(n);
	for (;;) {
		wipe();
		clear_all();
		const int rho = ((std::min(xs, ys) / 2) * 7) / 8;
		uint32_t angle;
		rnd_angle_pair(&angle);
		for (int i = 1; i <= n; i++) {
			angle += step;
			int x, y;
			project_pt(xs / 2, ys / 2, angle, rho, &x, &y);
			add_starting_location(x, y);
			make_region(i, x, y, G0);
		}
		bool fail = false;
		for (int i = 1; i <= n && !fail; i++) {
			if (grow_region(i, G0 / 4, (rho * 3) / 4, -1, -1, 0) != 0) fail = true;
		}
		for (int i = 1; i <= n && !fail; i++) {
			if (grow_region(i, G0, (rho * 3) / 4, -1, -1, 0) != 0) {
				fail = true;
				G0 = std::max(G0 * 9 / 10, (cells / n) / 4);
			}
		}
		if (fail) {
			if (give_up()) return false;
			res.retries++;
			continue;
		}
		check_player_land(-1);
		eliminate_pools(kEdges);
		eliminate_edge_canals();
		return true;
	}
}

// B3.11 Nile Delta
bool Gen::mc_nile_delta() {
	const int n = nations;
	int Gs0 = std::max(100, scale_land_area((cells / n) * 6 / 8, n));
	avoid_continent = std::max(4, scale_land_area(avoid_continent, n));
	const uint32_t step = player_step(n);
	for (;;) {
		wipe();
		clear_all();
		const int rho = ((std::min(xs, ys) / 2) * 3) / 4;
		uint32_t angle;
		rnd_angle_pair(&angle);
		for (int i = 1; i <= n; i++) {
			angle += step;
			int x, y;
			project_pt(xs / 2, ys / 2, angle, rho, &x, &y);
			add_starting_location(x, y);
			make_region(i, x, y, Gs0);
		}
		const int K = std::max(1, scale_land_area(4, n));
		int acc = Gs0;
		bool fail = false;
		for (int stage = 0; stage < K && !fail; stage++) {
			for (int i = 1; i <= n && !fail; i++) {
				if (grow_region(i, acc / K, rho / 2, -1, -1, 0) != 0) {
					fail = true;
					Gs0 = std::max(Gs0 * 4 / 5, (cells / n) / 4);
				}
			}
			acc += Gs0;
		}
		if (fail) {
			if (give_up()) return false;
			res.retries++;
			continue;
		}
		eliminate_pools(kEntire);
		eliminate_edge_canals();
		check_player_land(-1);
		return true;
	}
}

// B3.12 British Isles (MapBritishIsles::make_continents @695130, decompile read)
bool Gen::mc_british_isles() {
	const int n = nations;
	avoid_continent = std::max(4, scale_land_area(avoid_continent, n));
	const uint32_t step = player_step(n);
	for (;;) {
		wipe();
		clear_all();
		int k = n < 3 ? n : std::min(n, r.rnd(n) + 2);
		std::array<int, 8> share{};
		for (int i = 0; i < k; i++) {
			share[size_t(i)] = (n + i % k) / k;
			if (share[size_t(i)] == 0 && i < n) share[size_t(i)] = 1;
		}
		const int rho = ((std::min(xs, ys) / 2) * 6) / 8;
		uint32_t angle;
		rnd_angle_pair(&angle);
		const int U = std::max(120, scale_land_area(((cells / n) * 5) / 9, k));
		for (int i = 0; i < k; i++) {
			int x, y;
			project_pt(xs / 2, ys / 2, angle, rho, &x, &y);
			make_region(i + 1, x, y, share[size_t(i)] * U);
			grow_region(i + 1, (share[size_t(i)] * U) / 4, rho / 6, -1, -1, 0);
			angle += (uint32_t(share[size_t((i + 1) % k)]) * step >> 1) + (uint32_t(share[size_t(i)]) * step >> 1);
		}
		const int stage = (get_player_overage(n) != 0 && U == 120) ? 1 : 0;
		bool restart = false;
		int i = 0;
		while (i < k && !restart) {
			const int goal = stage ? U : share[size_t(i)] * U;
			const int size = grow_region(i + 1, goal, rho / 3, -1, -1, 1);
			if (size < goal / 2 || size < 0x78) {
				k--;
				if (i != k) {
					restart = true;
				} else {
					// the last island is dropped; the previous island is credited with one more player (as decompiled)
					if (i >= 1) share[size_t(i - 1)]++;
				}
				break;
			}
			i++;
		}
		if (restart || k <= 0) {
			if (give_up()) return false;
			res.retries++;
			continue;
		}
		std::vector<WCoord> cand;
		for (int ii = 0; ii < k; ii++) {
			for (int sh = 0; sh < share[size_t(ii)]; sh++) {
				int x = 0, y = 0;
				if (!place_start_in_region(ii + 1, &x, &y, 10, cand)) {
					// ❓ the original appends the previous (x, y) again; here any cell of the island far enough from the candidates
					const GenRegion& g = reg[size_t(ii + 1)];
					bool ok = false;
					for (int md = 8; md >= 0 && !ok; md -= 4) {
						for (const WCoord& cell : g.cells) {
							bool far_ok = true;
							for (const WCoord& o : cand) {
								if (vd(cell.x - o.x, cell.y - o.y) < md) far_ok = false;
							}
							if (far_ok) {
								x = cell.x;
								y = cell.y;
								ok = true;
								break;
							}
						}
					}
					if (!ok && !g.cells.empty()) {
						x = g.cells[0].x;
						y = g.cells[0].y;
					}
				}
				cand.push_back({x, y});
			}
		}
		while (int(cand.size()) < n) cand.push_back(cand.empty() ? WCoord{xs / 2, ys / 2} : cand.back());
		std::array<int, 16> used{};
		for (int ii = 0; ii < n; ii++) {
			int j;
			int guard = 0;
			do {
				j = r.rnd(n);
				if (++guard > 200) {
					for (j = 0; j < n && used[size_t(j)]; j++) {
					}
					break;
				}
			} while (used[size_t(j)]);
			used[size_t(j)] = 1;
			add_starting_location(cand[size_t(j)].x, cand[size_t(j)].y);
		}
		check_player_land(-1);
		eliminate_pools(kEntire);
		return true;
	}
}

}  // namespace ron::sim::mapgen
