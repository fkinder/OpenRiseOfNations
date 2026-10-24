// TerrainGroups::place_all (docs/randommap-terrain-groups.md A2, A4) and the passes that follow it: add_doobers,
// treeify_mountains, nubify_forest, the tileset base changes and fix_transitions / nubify_transitions (A6).
#include <array>

#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

// TerrainGroup::place_region_group @6a2f60: one clump somewhere in a region.
bool Gen::place_region_group(const GroupEntry& g, int type_t, int size, int rid, int range_index, int oilcount,
                             bool is_helping, GroupRun& run) {
	if (rid < 0 || rid >= Regions::kNum) return false;
	const GenRegion& R = reg[size_t(rid)];
	const int n = R.size > 0 ? int(R.cells.size()) : 0;
	if (n == 0) return false;
	const bool oil = g.type == kOilGroup;
	const uint32_t idx0 = n > 1 ? uint32_t(1 + r.rnd(n)) : 1u;
	uint32_t x = idx0;
	do {
		if (x <= uint32_t(n)) {
			const WCoord c0 = R.cells[size_t(x - 1)];
			int verdict = 0;  // 0 reject, 1 placed, 2 abort the region
			[&]() {
				if (!inb(c0.x, c0.y)) return;
				const WData& d = W(c0.x, c0.y);
				const bool water = World::is_ocean(d) || (d.flags & 4);
				if (oil ? !water : water) return;
				{
					const uint16_t tm = w.t(c0.x * 4 + 2, c0.y * 4 + 2);
					if ((tm & tmask::kStarted) || (tm & tmask::kLowField) == tmask::kBuilding) return;
				}
				for (const WCoord& s : starts) {
					const int dd = vd(c0.x - s.x, c0.y - s.y);
					if (g.start_min > 0 && dd < g.start_min) return;
					if (g.start_max > 0 && dd > g.start_max) {
						verdict = 2;
						return;
					}
				}
				if ((d.flags & 0x10) || (d.flags & 0x68) || (d.flags & 0x1800) || d.blocked != 0) return;
				if (oil) {
					// ❓ randommap-terrain-groups A4 step 3: oil deposits keep apart (rock_space, at least 3) from each other
					const int sp = std::min(64, std::max(3, g.rock_space));
					const Circles& C = circles();
					for (int k = 1; k < C.radius[size_t(sp)]; k++) {
						const int nx = c0.x + C.x[size_t(k)], ny = c0.y + C.y[size_t(k)];
						if (inb(nx, ny) && (W(nx, ny).flags & wflag::kOil)) return;
					}
				}
				if (!tile_tests(g, c0.x, c0.y)) return;
				if (g.type == kTrees) {
					for (int nb = 1; nb <= 8; nb++) {
						const int nx = c0.x + move_x[nb], ny = c0.y + move_y[nb];
						if (inb(nx, ny) && (W(nx, ny).flags & 0x40)) return;
					}
				}
				if (is_helping && !starts.empty()) {
					int best = 0, bd = 1 << 30;
					for (size_t q = 0; q < starts.size(); q++) {
						const int dd = vd(c0.x - starts[q].x, c0.y - starts[q].y);
						if (dd < bd) {
							bd = dd;
							best = int(q);
						}
					}
					if (best != lowest_player[size_t(type_t)]) return;  // "Aborting drop type t. Will not aid player w"
				}
				WCoord first = c0;
				switch (g.type) {
				case kMountains:
					if (!add_mountain(range_index, c0.x, c0.y, g, g.mount_space, g.forest_space, g.rock_space, g.coast_space,
					                  g.start_min))
						return;
					break;
				case kCliffs: return;
				case kOilGroup:
					run.tiles.clear();
					if (!drop_tile(g, run, c0.x, c0.y, 1)) return;
					break;
				default: {
					const int gr = grow_clump(g, run, c0.x, c0.y, size, 0, 0, false, nullptr, false);
					if (gr == 1) return;
					if (gr == 2) {
						clear_group(run, g.type);
						return;
					}
					first = run.tiles[0];
					break;
				}
				}
				for (size_t p = 0; p < starts.size() && p < 8; p++) {
					player_scores[p][size_t(type_t)] += vd(starts[p].x - first.x, starts[p].y - first.y);
				}
				int worst = 0, wv = -1;
				for (size_t p = 0; p < starts.size() && p < 8; p++) {
					if (player_scores[p][size_t(type_t)] > wv) {
						wv = player_scores[p][size_t(type_t)];
						worst = int(p);
					}
				}
				lowest_player[size_t(type_t)] = worst;
				if (g.type == kRocks && oilcount > 0) place_oil_deposits(run, oilcount);
				verdict = 1;
			}();
			if (verdict == 1) return true;
			if (verdict == 2) return false;
		}
		x = (x >> 1) ^ ((x & 1) ? 0xB400u : 0u);
		if (x == 0) break;
	} while (x != idx0);
	return false;
}

namespace {

// Calls fn(range_index) for every shape of the list of `size`, then once more with the next smaller size after
// randomize_mountains (❓ randommap-terrain-groups A2 step 5: the exact retry ladder).
template <typename Fn>
bool mountain_ladder(Gen& gn, int size, Fn&& fn) {
	int cur = std::min(3, std::max(1, size));
	for (int round = 0; round < 3 && cur >= 1; round++) {
		const int cnt = std::max<int>(1, int(gn.mountain_lists[cur].size()));
		for (int k = 0; k < cnt; k++) {
			const int range = gn.get_range(cur);
			if (range < 0) break;
			if (fn(cur, range)) return true;
		}
		gn.randomize_mountains();
		cur--;
	}
	return false;
}

}  // namespace

// TerrainGroups::place_all @6a70d0
void Gen::place_all() {
	load_mountain_lists();
	randomize_mountains();
	const int np = int(starts.size());
	help_lowest_index.fill(0);
	for (auto& a : player_scores) a.fill(0);
	lowest_player.fill(0);
	const size_t ng = groups.size();
	std::vector<char> active(ng, 0);
	std::vector<int> clumps(ng, 0);
	int roll = 0, taken = 0, last_grouping = -2;
	for (size_t gi = 0; gi < ng; gi++) {
		const GroupEntry& ge = groups[gi];
		bool act = true;
		if (ge.grouping == 0 || ge.grouping != last_grouping) {
			roll = r.rnd(100);
			last_grouping = ge.grouping;
			taken = 0;
		}
		if (roll < 0) {
			if (ge.chance != 0) {
				act = false;
				taken = 0;
			} else if (!taken) {
				act = false;
			}
		} else {
			roll -= ge.chance;
			if (roll >= 0) {
				act = false;
				taken = 0;
			} else {
				taken = 1;
			}
		}
		active[gi] = act;
		if (!act) continue;
		int n = ge.min_clumps;
		if (ge.max_clumps > ge.min_clumps) n = ge.min_clumps + r.rnd(ge.max_clumps - ge.min_clumps + 1);
		n = std::max(0, n);
		clumps[gi] = n;
		help_lowest_index[size_t(ge.type - 4)] += n;
	}
	for (int& h : help_lowest_index) {
		if (h < 2) h = 0x7fffffff;
		else {
			h >>= 1;
			h -= h & 1;
		}
	}
	for (size_t gi = 0; gi < ng; gi++) {
		if (!active[gi]) continue;
		const GroupEntry& g = groups[gi];
		const int t = g.type - 4;
		const int nc = clumps[gi];
		int size_lo = std::min(g.min_size, g.max_size), size_hi = std::max(g.min_size, g.max_size);
		int oil_lo = std::min(g.min_oil, g.max_oil), oil_hi = std::max(g.min_oil, g.max_oil);
		if (g.type == kMountains) size_hi = std::min(size_hi, 3);
		if (g.type == kCliffs) size_hi = std::min(size_hi, 5);
		if (g.type == kRocks) oil_hi = std::min(oil_hi, size_hi);
		std::vector<int> sizes(size_t(nc), 0), oils(size_t(nc), 0);
		for (int i = 0; i < nc; i++) {
			int sz = size_lo;
			if (size_lo < size_hi) sz = size_lo + r.rnd(size_hi - size_lo + 1);
			if (g.type == kTrees && g.pattern == 0) {
				if (sz & 1) sz++;
				sz = std::min(sz, 9);
			}
			sizes[size_t(i)] = sz;
			if (g.type == kRocks) {
				int o = oil_lo;
				if (oil_lo < oil_hi) o = oil_lo + r.rnd(oil_hi - oil_lo + 1);
				oils[size_t(i)] = o;
			} else {
				oils[size_t(i)] = sz;
			}
		}
		if (g.type == kCliffs) continue;  // not decoded
		switch (g.pattern) {
		case 0: {  // per player
			if (g.type == kOilGroup) break;
			for (int i = 0; i < nc; i++) {
				Shape shape;
				for (int p = 0; p < np; p++) {
					GroupRun run;
					bool ok = false;
					if (g.type == kMountains) {
						ok = mountain_ladder(*this, sizes[size_t(i)], [&](int, int range) {
							return place_player_group(g, t, 1, p, 0, range, nullptr, false, run);
						});
					} else if (g.type == kTrees) {
						ok = place_player_group(g, t, sizes[size_t(i)], p, oils[size_t(i)], -1, &shape, true, run);
						if (!ok) ok = place_player_group(g, t, sizes[size_t(i)], p, oils[size_t(i)], -1, &shape, false, run);
					} else {
						ok = place_player_group(g, t, sizes[size_t(i)], p, oils[size_t(i)], -1, nullptr, false, run);
					}
					(void)ok;
				}
			}
			break;
		}
		case 1: {  // per continent
			const bool oil = g.type == kOilGroup;
			std::vector<int> ids;
			for (int rid = oil ? 64 : 1; rid <= (oil ? 126 : 63); rid++) {
				if (reg[size_t(rid)].size > 5) ids.push_back(rid);
			}
			std::vector<int> failed;
			auto place_one = [&](int i, int rid, bool helping) {
				GroupRun run;
				if (g.type == kMountains) {
					return mountain_ladder(*this, sizes[size_t(i)], [&](int cur, int range) {
						return place_region_group(g, t, cur, rid, range, 0, helping, run);
					});
				}
				return place_region_group(g, t, sizes[size_t(i)], rid, -1, oils[size_t(i)], helping, run);
			};
			for (int rid : ids) {
				for (int i = 0; i < nc; i++) {
					const bool helping = help_lowest_index[size_t(t)] <= i;
					if (!place_one(i, rid, helping)) failed.push_back(i);
				}
			}
			for (int i : failed) {
				for (int rid : ids) {
					if (place_one(i, rid, true)) break;
				}
			}
			break;
		}
		case 2:
		case 3: {  // world / nonplayer
			const bool oil = g.type == kOilGroup;
			std::vector<int> ids;
			for (int rid = oil ? 64 : 1; rid <= (oil ? 126 : 63); rid++) {
				if (reg[size_t(rid)].size <= 5) continue;
				if (g.pattern == 3) {
					bool has_start = false;
					for (const WCoord& s : starts) {
						if (inb(s.x, s.y) && W(s.x, s.y).region == rid) has_start = true;
					}
					if (has_start) continue;
				}
				ids.push_back(rid);
			}
			if (nc > 6 && ids.size() < 6 && !ids.empty()) {
				int64_t total = 0;
				for (int rid : ids) total += reg[size_t(rid)].size;
				std::vector<int> copies;
				int sum = 0, largest = 0;
				for (size_t k = 0; k < ids.size(); k++) {
					const int cpy = int(int64_t(nc) * reg[size_t(ids[k])].size / std::max<int64_t>(1, total));
					copies.push_back(cpy);
					sum += cpy;
					if (reg[size_t(ids[k])].size > reg[size_t(ids[size_t(largest)])].size) largest = int(k);
				}
				copies[size_t(largest)] += nc - sum;
				std::vector<int> expanded;
				for (size_t k = 0; k < ids.size(); k++) {
					for (int q = 0; q < copies[k]; q++) expanded.push_back(ids[k]);
				}
				if (!expanded.empty()) ids = expanded;
			}
			if (ids.empty()) break;
			const int len = int(ids.size());
			int cursor = len > 1 ? r.rnd(len) : 0;
			std::array<char, Regions::kNum> failed_reg{};
			for (int i = 0; i < nc; i++) {
				const bool helping = help_lowest_index[size_t(t)] <= i;
				for (int tried = 0; tried < len; tried++) {
					const int rid = ids[size_t((cursor + tried) % len)];
					if (failed_reg[size_t(rid)]) continue;
					GroupRun run;
					bool ok;
					if (g.type == kMountains) {
						ok = mountain_ladder(*this, sizes[size_t(i)], [&](int cur, int range) {
							return place_region_group(g, t, cur, rid, range, 0, helping, run);
						});
					} else {
						ok = place_region_group(g, t, sizes[size_t(i)], rid, -1, oils[size_t(i)], helping, run);
					}
					if (ok) {
						cursor = (cursor + tried + 1) % len;
						break;
					}
					failed_reg[size_t(rid)] = 1;
				}
			}
			break;
		}
		default: break;  // pattern 4 (corner) is never placed
		}
	}
	add_doobers();
	if (style != kStyleHimalayas) treeify_mountains(tgd.mnt_fringe_tree_prob);
	res.doobers = doober_count;
}

// TerrainGroups::add_doobers @6a1540: cosmetic bushes and mountain rocks; only the draws are reproduced.
void Gen::add_doobers() {
	doober_cells.assign(size_t(cells), 0);
	doober_count = 0;
	for (int x = 1; x <= xs - 2; x++) {
		for (int y = 1; y <= ys - 2; y++) {
			const WData& d = W(x, y);
			if (!(d.flags & 0x20) || (d.flags & 0x1000)) continue;
			const bool s = !(W(x, y + 1).flags & 0x20), e = !(W(x + 1, y).flags & 0x20);
			for (int side = 0; side < 2; side++) {
				if (!(side == 0 ? s : e)) continue;
				if (r.rnd(100) < tgd.bush_clump_prob) {
					r.mask(3);  // q: which sub-cell the bushes go to
					doober_count++;
					doober_cells[size_t(y) * size_t(xs) + size_t(x)] = 1;
				}
			}
		}
	}
	for (int x = 1; x <= xs - 2; x++) {
		for (int y = 1; y <= ys - 2; y++) {
			const WData& d = W(x, y);
			if (World::is_ocean(d) || (d.flags & 0x38) || (d.flags & 0x1000)) continue;
			if (doober_cells[size_t(y) * size_t(xs) + size_t(x)]) continue;
			if ((W(x - 1, y).flags & 0x10) || (W(x, y - 1).flags & 0x10)) {
				if (r.rnd(100) < tgd.mnt_rock_clump_prob) {
					r.mask(15);
					doober_count++;
				}
			}
		}
	}
}

// TerrainGroups::treeify_mountains @6a1cc0
void Gen::treeify_mountains(int prob) {
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			WData& d = W(x, y);
			if (has_mountain_tcoords(x, y) && !(d.flags & 0x10)) {
				bool next = false;
				if (inb(x, y + 1) && (W(x, y + 1).flags & 0x10)) next = true;
				if (inb(x + 1, y) && (W(x + 1, y).flags & 0x10)) next = true;
				if (next && (d.flags & 0x404) == 0) {
					if (r.rnd(100) < prob) d.flags = uint16_t((d.flags & 0xffc3) | 0x20);
				}
			} else if (d.flags & 0x10) {
				bool open = false;
				const int ox[2] = {1, 0}, oy[2] = {0, 1};  // orthog indices 2 and 3
				for (int k = 0; k < 2; k++) {
					const int nx = x + ox[k], ny = y + oy[k];
					if (inb(nx, ny) && !has_mountain_tcoords(nx, ny)) open = true;
				}
				if (open && (d.flags & 0x404) == 0) {
					if (r.rnd(100) < prob) d.flags = uint16_t((d.flags & 0xffc3) | 0x30);
				}
			}
		}
	}
}

// TerrainGroups::nubify_forest @6a93b0
void Gen::nubify_forest() {
	auto forest = [&](int x, int y) { return inb(x, y) && (W(x, y).flags & 0x20) != 0; };
	auto nub = [&](int nx, int ny) {
		if (!inb(nx, ny) || start_city_wcoord(nx, ny) || is_water(nx, ny) || w.is_edge_of_region(nx, ny)) return false;
		WData& d = W(nx, ny);
		if (d.flags & 4) d.land = 2;
		d.flags = uint16_t((d.flags & 0xffc3) | 0x20);
		return true;
	};
	for (int x = 1; x <= xs - 2; x++) {
		int run = 0, dir = 0;
		for (int y = 1; y <= ys - 2; y++) {
			if (!forest(x, y)) {
				run = 0;
				continue;
			}
			if (forest(x - 1, y) && forest(x + 1, y)) {
				run = 0;
				continue;
			}
			if (dir != 0 && forest(x + dir, y)) {
				run++;
			} else {
				run = 1;
				dir = forest(x + 1, y) ? 1 : forest(x - 1, y) ? -1 : 0;
			}
			if (run >= 4) {
				const int k = r.mask(3);
				if (!nub(x + dir, y - k)) run = 0;
			}
		}
	}
	for (int y = 1; y <= ys - 2; y++) {
		int run = 0, dir = 0;
		for (int x = 1; x <= xs - 2; x++) {
			if (!forest(x, y)) {
				run = 0;
				continue;
			}
			if (forest(x, y - 1) && forest(x, y + 1)) {
				run = 0;
				continue;
			}
			if (dir != 0 && forest(x, y + dir)) {
				run++;
			} else {
				run = 1;
				dir = forest(x, y + 1) ? 1 : forest(x, y - 1) ? -1 : 0;
			}
			if (run >= 4) {
				const int k = r.mask(3);
				if (!nub(x - k, y + dir)) run = 0;
			}
		}
	}
}

// TerrainGroups::change_forest_base @6a9a60 / change_mountain_base @6a9c10 / change_coast_base @6a9dc0
void Gen::change_forest_base() {
	if (tgd.forest_base < 0) return;
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			if (!(W(x, y).flags & 0x20)) continue;
			W(x, y).land_sub = uint8_t(tgd.forest_base);
			for (int o = 1; o <= 4; o++) {
				const int nx = x + orthog_x[o], ny = y + orthog_y[o];
				if (!inb(nx, ny) || (W(nx, ny).flags & 0x20) || W(nx, ny).land == 2) continue;
				if (r.rnd(100) < tgd.forest_growth_prob) W(nx, ny).land_sub = uint8_t(tgd.forest_base);
			}
		}
	}
}

void Gen::change_mountain_base() {
	if (tgd.mountain_base < 0) return;
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			if (!has_mountain_tcoords(x, y)) continue;
			W(x, y).land_sub = uint8_t(tgd.mountain_base);
			for (int o = 1; o <= 4; o++) {
				const int nx = x + orthog_x[o], ny = y + orthog_y[o];
				if (!inb(nx, ny) || has_mountain_tcoords(nx, ny)) continue;
				if (r.rnd(100) < tgd.mountain_growth_prob) W(nx, ny).land_sub = uint8_t(tgd.mountain_base);
			}
		}
	}
}

void Gen::change_coast_base() {
	if (tgd.coast_base < 0) return;
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			if (!(W(x, y).flags & 0x04)) continue;
			W(x, y).land_sub = uint8_t(tgd.coast_base);
			for (int o = 1; o <= 4; o++) {
				const int nx = x + orthog_x[o], ny = y + orthog_y[o];
				if (!inb(nx, ny) || (W(nx, ny).flags & 0x04)) continue;
				if (r.rnd(100) < tgd.coast_growth_prob) W(nx, ny).land_sub = uint8_t(tgd.coast_base);
			}
		}
	}
}

// TerrainGroups::nubify_transitions @6a1f30
void Gen::nubify_transitions(int s) {
	auto match = [&](int x, int y) {
		if (!inb(x, y)) return false;
		const WData& d = W(x, y);
		return d.land == 0 && !(d.flags & 4) && d.land_sub == s;
	};
	auto target = [&](int x, int y) {
		if (!inb(x, y)) return false;
		const WData& d = W(x, y);
		return !(d.flags & 4) && d.land == 0 && d.land_sub < s;
	};
	for (int x = 0; x < xs; x++) {
		int run = 0;
		for (int y = 0; y < ys; y++) {
			if (!match(x, y)) {
				run = 0;
				continue;
			}
			run++;
			if (run >= 5) {
				const int k = r.rnd_draw(3);
				const int ny = y - 1 - k;
				if (target(x - 1, ny)) {
					W(x - 1, ny).land_sub = uint8_t(s);
					run = 0;
				} else if (target(x + 1, ny)) {
					W(x + 1, ny).land_sub = uint8_t(s);
					run = 0;
				}
			}
		}
	}
	for (int y = 0; y < ys; y++) {
		int run = 0;
		for (int x = 0; x < xs; x++) {
			if (!match(x, y)) {
				run = 0;
				continue;
			}
			run++;
			if (run >= 4) {
				const int k = r.mask(3);
				if (target(x - k, y - 1)) {
					W(x - k, y - 1).land_sub = uint8_t(s);
					run = 0;
				} else if (target(x - k, y + 1)) {
					W(x - k, y + 1).land_sub = uint8_t(s);
					run = 0;
				}
			}
		}
	}
}

// TerrainGroups::fix_transitions @6a9f70
void Gen::fix_transitions() {
	nubify_transitions(3);
	for (int step = 0; step < 2; step++) {
		const int from = step == 0 ? 3 : 2;
		for (int x = 0; x < xs; x++) {
			for (int y = 0; y < ys; y++) {
				const WData& d = W(x, y);
				if (d.land != 0 || d.land_sub != from) continue;
				for (int k = 1; k <= 8; k++) {
					const int nx = x + move_x[k], ny = y + move_y[k];
					if (!inb(nx, ny)) continue;
					WData& n = W(nx, ny);
					if (World::is_ocean(n) && !(n.flags & 4)) continue;
					if (n.land == 0 && (step == 0 ? (n.land_sub == 3 || n.land_sub == 2)
					                              : (n.land_sub == 3 || n.land_sub == 2 || n.land_sub == 1)))
						continue;
					n.land = 0;
					n.land_sub = uint8_t(step == 0 ? 2 : 1);
				}
			}
		}
		nubify_transitions(step == 0 ? 2 : 1);
	}
}

}  // namespace ron::sim::mapgen
