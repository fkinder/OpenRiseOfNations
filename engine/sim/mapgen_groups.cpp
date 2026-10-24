// TerrainGroups::place_all @6a70d0 and TerrainGroup::place_region_group @6a2f60 (docs/randommap-terrain-groups.md A2, A4),
// re-derived from the decompiles and the disassembly of place_all (oracle: current 2.SVX, the first ten groups of the Great
// Lakes style). Every game_random draw is made in the original's order.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <map>

#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

// TerrainGroup::place_region_group @6a2f60: one clump somewhere in a region. The candidate cells are the region's cells walked by
// a 16-bit LFSR (Galois, polynomial 0xB400) from a drawn start; a seed that is dropped grows (trees, rocks) by orthogonal
// neighbours, mountains and oil deposits are single drops. Returns the value of the last seed drop (an original quirk: a group
// that was cleared after its growth got stuck still reports the seed's success when no later seed was tried).
int Gen::place_region_group(const GroupEntry& g, int size, int rid, int range_index, int oilcount, GroupRun& run) {
	const int t = g.type - 4;
	run.tiles.clear();
	int ret = 0;
	if (rid < 0 || rid >= Regions::kNum) return 0;
	const GenRegion& R = reg[size_t(rid)];
	const int n = int(R.cells.size());
	if (n == 0) return 0;
	const Circles& C = circles();
	const int np = int(starts.size());
	const int stay = size / 4 + 1;
	const int idx0 = (n - 1 < 1) ? 0 : r.rnd_draw(n);
	const int start = idx0 + 1;
	int cur = start;
	const int hx = xs / 2, hy = ys / 2;
	for (;;) {
		// ---- one candidate cell
		[&]() {
			const WCoord c = R.cells[size_t(cur - 1)];
			const int x = c.x, y = c.y;
			if (!inb(x, y)) return;
			const WData& d0 = W(x, y);
			const int land_c = (d0.flags & 4) ? 2 : d0.land;
			if (g.type == kOilGroup) {
				if (land_c != 2) return;
			} else if (!(d0.flags & 0x100) && (d0.land == 2 || d0.land == 1)) {
				return;
			}
			if (start_city_wcoord(x, y)) return;
			for (int p = 0; p < np; p++) {
				const int dd = vd(x - starts[size_t(p)].x, y - starts[size_t(p)].y);
				if (g.start_min > 0 && dd < g.start_min) return;
				if (g.start_max > 0 && g.start_max < dd) return;
			}
			if ((d0.flags & 0x10) || (d0.flags & 0x68) || (d0.flags & 0x1800) || d0.blocked != 0) return;
			if (g.type == kOilGroup) {
				int lim = C.radius[3];
				if (3 < g.rock_space) lim = C.radius[size_t(std::min(g.rock_space, 0x3f))];
				for (int k = 0; k < lim; k++) {
					const int nx = x + C.x[size_t(k)], ny = y + C.y[size_t(k)];
					if (!inb(nx, ny)) continue;
					const WData& nb = W(nx, ny);
					if (!(nb.flags & 0x100) && (nb.land == 2 || nb.land == 1)) {
						if (nb.flags & 0x800) return;
						continue;
					}
					if (k < C.radius[2]) return;
				}
			}
			if (g.cent_min > 0 || g.cent_max > 0) {
				const int ccx = hx + (x > hx ? 1 : 0), ccy = hy + (y > hy ? 1 : 0);
				const int dd = vd(x - ccx, y - ccy);
				if (g.cent_min > 0 && dd < g.cent_min) return;
				if (g.cent_max > 0 && g.cent_max < dd) return;
			}
			if (g.edge_min > 0 || g.edge_max > 0) {
				const int e = edge_d(x, y);
				if (g.edge_min > 0 && e < g.edge_min) return;
				if (g.edge_max > 0 && g.edge_max < e) return;
			}
			if (x == 0 || y == 0 || x == xs - 1 || y == xs - 1) return;
			if (g.corner_min > 0 || g.corner_max > 0) {
				const int e = corner_d(x, y, -1);
				if (g.corner_min > 0 && e < g.corner_min) return;
				if (g.corner_max > 0 && g.corner_max < e) return;
			}
			if (g.type == kTrees) {
				for (int k = 0; k < 9; k++) {
					const int nx = x + move_x[k], ny = y + move_y[k];
					if (inb(nx, ny) && (W(nx, ny).flags & 0x40)) return;
				}
			}
			if (is_helping) {
				int best = 0, bd = 0x7fffffff;
				for (int p = 0; p < np; p++) {
					const int dd = vd(starts[size_t(p)].x - x, starts[size_t(p)].y - y);
					if (dd < bd) {
						bd = dd;
						best = p;
					}
				}
				if (best != lowest_player[size_t(t)]) return;  // "Aborting drop ... Will not aid player"
			}
			ret = drop_tile(g, run, x, y, stay, range_index) ? 1 : 0;
			if (!ret) return;
			for (int p = 0; p < np && p < 8; p++) player_scores[size_t(p)][size_t(t)] += vd(starts[size_t(p)].x - x, starts[size_t(p)].y - y);
			{
				int best = 0, bv = 0;
				for (int p = 0; p < np && p < 8; p++) {
					if (bv < player_scores[size_t(p)][size_t(t)]) {
						best = p;
						bv = player_scores[size_t(p)][size_t(t)];
					}
				}
				lowest_player[size_t(t)] = best;
			}
			if (g.type == kMountains || g.type == kOilGroup || g.type == kCliffs) {
				ret = 2;  // done: return 1
				return;
			}
			// ---- growth by orthogonal neighbours of a source tile
			for (;;) {  // LAB_006a3cd0
				const int nt = int(run.tiles.size());
				const int draw = (nt - 1 < 1) ? 0 : r.rnd_draw(nt);
				const int first = draw + 1;
				int it = first;
				bool dropped = false;
				for (;;) {
					const int src = (it != int(run.tiles.size())) ? it : 0;
					int ro, rc;
					randomize_orthogs(&ro, &rc);
					for (int k = 0; k < 4 && !dropped; k++) {
						const int o = 4 - (ro + k) % 4;
						const WCoord s0 = run.tiles[size_t(src)];
						const int cx = s0.x + orthog_x[o], cy = s0.y + orthog_y[o];
						bool rej = false;
						for (const WCoord& tl : run.tiles) {
							if (tl.x == cx && tl.y == cy) rej = true;
						}
						if (!rej && !inb(cx, cy)) rej = true;
						if (!rej) {
							for (int p = 0; p < np && !rej; p++) {
								const int dd = vd(cx - starts[size_t(p)].x, cy - starts[size_t(p)].y);
								if ((g.start_min > 0 && dd < g.start_min) || (g.start_max > 0 && g.start_max < dd)) rej = true;
							}
						}
						if (!rej && g.cent_min > 0) {
							if (vd(cx - hx, cy - hy) < g.cent_min) rej = true;
						}
						if (!rej && g.cent_max > 0) {
							if (g.cent_max < vd(cx - hx, cy - hy)) rej = true;
						}
						if (!rej && g.type == kTrees) {
							for (int kk = 0; kk < 9; kk++) {
								const int nx = cx + move_x[kk], ny = cy + move_y[kk];
								if (inb(nx, ny) && (W(nx, ny).flags & 0x40)) rej = true;
							}
						}
						if (!rej && drop_tile(g, run, cx, cy, stay, -1)) dropped = true;
					}
					if (dropped) break;
					it = src + 1;
					if (it == first) {
						clear_group(run, g.type);
						return;  // growth stuck: next candidate (ret keeps the seed's 1)
					}
				}
				if (size <= int(run.tiles.size())) {
					if (g.type == kRocks) place_oil_deposits(run, oilcount);
					ret = 2;
					return;
				}
			}
		}();
		if (ret == 2) return 1;
		// ---- the LFSR step
		for (;;) {
			if (cur & 1) cur ^= 0x16800;
			cur >>= 1;
			if (cur <= n) break;
			if (cur == start) return ret;
		}
		if (cur == start) return ret;
	}
}

namespace {

// The mountain retry ladder of the per-player pattern (place_all @6a8b25..6a8d20): the shape of the asked size is tried over the
// whole circular list (the failed shape once more first), then after randomize_mountains the next smaller size, and for size 2 a
// small mountain on top of a successful smaller one.
bool player_mountain(Gen& gn, const GroupEntry& g, int size_e, int p, int oil, GroupRun& run) {
	auto place = [&](int range) { return gn.place_player_group(g, 1, size_e, p, oil, range, nullptr, false, run); };
	int range = gn.get_range(size_e);
	bool r = place(range);
	if (r) return true;
	const int start = range;
	for (;;) {
		r = place(range);
		range = gn.get_range(size_e);
		if (range == start) {
			if (!r) {
				gn.randomize_mountains();
				if (1 < size_e) {
					int rr = gn.get_range(size_e - 1);
					const int s2 = rr;
					bool r2;
					do {
						r2 = place(rr);
						rr = gn.get_range(size_e - 1);
					} while (rr != s2 && !r2);
					gn.randomize_mountains();
					if (r2 && size_e == 2) {
						int r3 = gn.get_range(1);
						const int s3 = r3;
						bool ok3;
						do {
							ok3 = place(r3);
							r3 = gn.get_range(size_e - 1);
						} while (r3 != s3 && !ok3);
					}
					return r2;
				}
			}
			break;
		}
		if (r) break;
	}
	return r;
}

}  // namespace

// The placement of one active group (place_all's per-pattern dispatch).
void Gen::run_group(size_t gi, int nc, const std::vector<int>& sizes, const std::vector<int>& oils) {
	const GroupEntry& g = groups[gi];
	const int t = g.type - 4;
	const int np = int(starts.size());
		switch (g.pattern) {
		case 0: {  // per player
			if (g.type == kOilGroup) break;
			for (int i = 0; i < nc; i++) {
				Shape shape;
				for (int p = 0; p < np; p++) {
					GroupRun run;
					bool ok = false;
					if (g.type == kMountains) {
						ok = player_mountain(*this, g, sizes[size_t(i)], p, oils[size_t(i)], run);
					} else {
						ok = place_player_group(g, t, sizes[size_t(i)], p, oils[size_t(i)], -1, &shape, g.type == kTrees, run);
						if (!ok && g.type == kTrees) ok = place_player_group(g, t, sizes[size_t(i)], p, oils[size_t(i)], -1, &shape, false, run);
					}
				}
			}
			break;
		}
		case 1: {  // per continent
			const bool oil = g.type == kOilGroup;
			const int begin = oil ? 0x40 : 1, end = oil ? 0x7f : 0x40;
			std::vector<int> failed;
			auto one = [&](int i, int rid) {
				GroupRun run;
				int range = sizes[size_t(i)];
				if (g.type == kMountains) range = get_range(sizes[size_t(i)]);
				return place_region_group(g, sizes[size_t(i)], rid, range, oils[size_t(i)], run);
			};
			for (int rid = begin; rid < end; rid++) {
				if (reg[size_t(rid)].size <= 5) continue;
				for (int i = 0; i < nc; i++) {
					is_helping = help_lowest_index[size_t(t)] <= i ? 1 : 0;
					if (!one(i, rid)) failed.push_back(i);
				}
			}
			for (int i : failed) {
				for (int rid = begin; rid < end; rid++) {
					if (reg[size_t(rid)].size <= 5) continue;
					is_helping = 1;
					one(i, rid);
				}
			}
			break;
		}
		case 2:
		case 3: {  // world / nonplayer
			const bool oil = g.type == kOilGroup;
			const int begin = oil ? 0x40 : 1, end = oil ? 0x7f : 0x40;
			// The candidate list is built by inserting at the head: a forward walk reads the regions in descending order.
			std::list<int> L;
			for (int rid = begin; rid < end; rid++) {
				if (reg[size_t(rid)].size <= 5) continue;
				if (g.pattern == 3) {
					bool has_start = false;
					for (const WCoord& s : starts) {
						if (inb(s.x, s.y) && W(s.x, s.y).region == rid) has_start = true;
					}
					if (has_start) continue;
				}
				L.push_front(rid);
			}
			if (L.empty()) break;
			if (nc > 6 && L.size() < 6) {
				// proportional copies: float arithmetic as in the original, extra copies and one more of the largest region inserted at the head
				std::vector<int> ids(L.begin(), L.end());
				std::vector<int> szs;
				float total = 0.0f;
				int largest = 0, largest_size = 0;
				for (int id : ids) {
					const int sz = reg[size_t(id)].size;
					szs.push_back(sz);
					if (largest_size < sz) {
						largest = id;
						largest_size = sz;
					}
					total = total + float(sz);
				}
				std::vector<int> copies;
				for (int sz : szs) copies.push_back(int((float(sz) / total) * float(nc)));
				std::list<int> NL(L.begin(), L.end());  // the originals stay behind the new heads
				for (size_t k = 0; k < ids.size(); k++) {
					int extra = copies[k] - 1;
					if (copies[k] == 0) extra = 0;
					for (int q = 0; q < extra; q++) NL.push_front(ids[k]);
				}
				NL.push_front(largest);
				L = NL;
			}
			const int len = int(L.size());
			std::vector<int> ring(L.begin(), L.end());
			const int kdraw = (len - 1 < 1) ? 0 : r.rnd_draw(len);
			int pos = kdraw % len;  // the cursor index into the ring
			int marker = ring[size_t(pos)];
			std::array<uint8_t, 128> won{};  // regions that already received a mountain of this group
			int i = 0;
			while (i < nc) {
				const int region = ring[size_t(pos)];
				const int i_start = i;
				is_helping = help_lowest_index[size_t(t)] <= i ? 1 : 0;
				const int helping_saved = is_helping;
				GroupRun run;
				int res = 0;
				int range = -1;
				if (g.type == kMountains) {
					range = get_range(sizes[size_t(i)]);
					res = place_region_group(g, sizes[size_t(i)], region, range, oils[size_t(i)], run);
					if (!res && !won[size_t(region)]) {
						GroupEntry gg = g;
						is_helping = 0;
						place_region_group(g, sizes[size_t(i)], region, range, oils[size_t(i)], run);  // the result is ignored
						range = get_range(sizes[size_t(i)]);
						int first = range;
						for (;;) {
							res = place_region_group(g, sizes[size_t(i)], region, range, oils[size_t(i)], run);
							range = get_range(sizes[size_t(i)]);
							if (range == first) break;
							if (res) break;
						}
						if (!res) {
							range = get_range(sizes[size_t(i)]);
							first = range;
							const int saved_coast = g.coast_space;
							bool skip_coast = saved_coast == 0;
							if (!skip_coast) {
								gg.coast_space = saved_coast - 1;
								for (;;) {
									res = place_region_group(gg, sizes[size_t(i)], region, range, oils[size_t(i)], run);
									range = get_range(sizes[size_t(i)]);
									if (range == first) break;
									if (res) break;
								}
							}
							if (!res) {
								randomize_mountains();
								if (sizes[size_t(i)] > 1) {
									range = get_range(sizes[size_t(i)] - 1);
									first = range;
									for (;;) {
										res = place_region_group(g, sizes[size_t(i)] - 1, region, range, oils[size_t(i)], run);
										range = get_range(sizes[size_t(i)] - 1);
										if (range == first || res) break;
									}
									randomize_mountains();
									if (res && sizes[size_t(i)] == 2) {
										range = get_range(1);
										first = range;
										for (;;) {
											const int r3 = place_region_group(g, sizes[size_t(i)] - 1, region, range, oils[size_t(i)], run);
											range = get_range(sizes[size_t(i)] - 1);
											if (range == first || r3) break;
										}
									}
								}
							}
						}
						is_helping = helping_saved;
					}
					if (res) won[size_t(region)] = 1;
				} else {
					res = place_region_group(g, sizes[size_t(i)], region, -1, oils[size_t(i)], run);
				}
				i++;
				pos = (pos + 1) % len;
				if (!res) {
					if (ring[size_t(pos)] != marker) i = i_start;
				} else {
					marker = region;
				}
			}
			break;
		}
		default: break;  // pattern 4 (corner) is never placed
		}
}

// TerrainGroups::place_all @6a70d0
void Gen::place_all() {
	load_mountain_lists();
	randomize_mountains();
	help_lowest_index.fill(0);
	for (auto& a : player_scores) a.fill(0);
	lowest_player.fill(0);
	const size_t ng = groups.size();
	std::vector<char> active(ng, 0);
	std::vector<int> clumps(ng, 0);
	int roll = 0, taken = 0, last_grouping = -10;
	for (size_t gi = 0; gi < ng; gi++) {
		const GroupEntry& ge = groups[gi];
		bool act = true;
		if (ge.grouping == 0 || ge.grouping != last_grouping) {
			roll = r.rnd(100);
			last_grouping = ge.grouping;
			taken = 0;
		}
		if ((roll < 0 && (ge.chance != 0 || !taken)) || (roll -= ge.chance, roll >= 0)) {
			act = false;
			taken = 0;
		} else {
			taken = 1;
		}
		active[gi] = act;
		if (!act) continue;
		int n = ge.min_clumps;
		if (ge.min_clumps < ge.max_clumps) n = ge.min_clumps + r.rnd(ge.max_clumps - ge.min_clumps + 1);
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
	is_helping = 0;
	for (size_t gi = 0; gi < ng; gi++) {
		if (!active[gi]) continue;
		const GroupEntry& g = groups[gi];
		const int nc = clumps[gi];
		int size_lo = std::min(g.min_size, g.max_size), size_hi = std::max(g.min_size, g.max_size);
		int oil_lo = std::min(g.min_oil, g.max_oil), oil_hi = std::max(g.min_oil, g.max_oil);
		if (g.type == kMountains) size_hi = std::min(size_hi, 3);
		else if (g.type == kCliffs) size_hi = std::min(size_hi, 5);
		else if (g.type == kRocks) {
			oil_hi = std::min(oil_hi, size_hi);
			if (oil_lo < 0) oil_lo = 0;
		}
		std::vector<int> sizes(size_t(nc), 0), oils(size_t(nc), 0);
		for (int i = 0; i < nc; i++) {
			int sz = size_lo;
			if (size_lo < size_hi) sz = size_lo + r.rnd(size_hi - size_lo + 1);
			if (g.pattern == 0 && g.type == kTrees) {
				if (!(sz & 1)) sz++;  // rounded up to ODD, capped at 9
				sz = std::min(sz, 9);
			}
			sizes[size_t(i)] = sz;
			if (g.type == kRocks) {
				int o = oil_lo;
				if (oil_lo < oil_hi) o = oil_lo + r.rnd(oil_hi - oil_lo + 1);
				oils[size_t(i)] = o;
			} else {
				oils[size_t(i)] = 0;
			}
		}
		if (g.type == kCliffs) continue;  // Cliffs::position_cliff is not decoded
		run_group(gi, nc, sizes, oils);
	}
	add_doobers();
	if (style != kStyleHimalayas) treeify_mountains(tgd.mnt_fringe_tree_prob);
	res.doobers = doober_count;
}

}  // namespace ron::sim::mapgen
