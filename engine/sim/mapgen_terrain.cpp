// TerrainGroups::place_all and the passes after it (docs/randommap-terrain-groups.md part A): forests, mountains, rocks and
// oil, doobers (draws only), mountain fringe trees, nubified forest edges, tileset base changes and subtype transitions.
// Cliffs are not placed: Cliffs::position_cliff / verify_defensive_position were never decoded (see the report).
#include <array>
#include <map>
#include <mutex>

#include "assets/xml.hpp"
#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

namespace {

// The mountain templates are decoded from the install's TGAs once per process and shared by every generation.
std::mutex g_mt_mutex;
std::map<std::string, std::unique_ptr<MountainTemplates>> g_mt;

// Corner (diagonal) offsets in the order of corner_x / corner_y [1..4].
const int kCornerX[4] = {-1, 1, 1, -1}, kCornerY[4] = {-1, -1, 1, 1};

}  // namespace

// ---------------------------------------------------------------- mountains

void Gen::load_mountain_lists() {
	if (mountains_loaded) return;
	mountains_loaded = true;
	int idx = 0;
	ron::for_each_element(ron::parse_xml(c.files.read_text("Data/effects_graphics.xml")), "MOUNTAINS", [&](const XmlElement& sec) {
		if (idx > 0) return;
		for (const XmlElement& m : sec.children) {
			if (m.name != "MOUNTAIN") continue;
			const std::string area = m.get("area");
			const int a = area == "sm" ? 1 : area == "med" ? 2 : 3;
			mountain_lists[a].push_back(idx++);
		}
		if (idx == 0) idx = -1;
	});
	for (int i = 1; i <= 3; i++) mountain_cursor[i] = 0;
}

const MountainRange* Gen::mount_range(int index) {
	std::lock_guard<std::mutex> lock(g_mt_mutex);
	std::unique_ptr<MountainTemplates>& p = g_mt[c.files.root()];
	if (!p) p = std::make_unique<MountainTemplates>(c.files);
	return p->get(index);
}

// Mountains::randomize_mountains @89ca70: three circular lists; each with more than one entry draws its cursor.
void Gen::randomize_mountains() {
	for (int s = 1; s <= 3; s++) {
		const int n = int(mountain_lists[s].size());
		if (n > 1) mountain_cursor[s] = r.rnd(n);
	}
}

// Mountains::get_range @89cb50: the list's current element, then advance.
int Gen::get_range(int size) {
	if (size < 1 || size > 3 || mountain_lists[size].empty()) return -1;
	const auto& l = mountain_lists[size];
	const int v = l[size_t(mountain_cursor[size] % int(l.size()))];
	mountain_cursor[size] = (mountain_cursor[size] + 1) % int(l.size());
	return v;
}

// Mountains::add_mountain @89xxxx (❓ not decoded): the range must fit on free land with the requested spacings; it is then
// stamped exactly as the scenario loader does (mountain wcoords, blocked mountain tcoords, behind flags).
bool Gen::add_mountain(int range_index, int wx, int wy, const GroupEntry& g, int space_m, int space_f, int space_r,
                       int space_c, int start_min) {
	(void)g;
	if (range_index < 0) return false;
	const MountainRange* mr = mount_range(range_index);
	if (!mr || mr->wcoords.empty()) return false;
	const Circles& C = circles();
	std::vector<WCoord> cs;
	for (const Offset& o : mr->wcoords) {
		const int x = wx + o[0], y = wy + o[1];
		if (!inb(x, y) || is_edge(x, y)) return false;
		const WData& d = W(x, y);
		if (World::is_ocean(d) || (d.flags & (0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x800 | 0x1000)) || d.blocked != 0) return false;
		cs.push_back({x, y});
	}
	for (const Offset& o : mr->tcoords) {
		const int tx = wx * 4 + o[0], ty = wy * 4 + o[1];
		if (!w.in_t(tx, ty) || (w.t(tx, ty) & tmask::kBlocked)) return false;
	}
	auto own = [&](int x, int y) {
		for (const WCoord& c2 : cs) {
			if (c2.x == x && c2.y == y) return true;
		}
		return false;
	};
	if (start_min > 0) {
		for (const WCoord& s : starts) {
			for (const WCoord& c2 : cs) {
				if (vd(c2.x - s.x, c2.y - s.y) < start_min) return false;
			}
		}
	}
	const int sm = std::min(64, std::max(space_m, std::max(space_f, std::max(space_r, space_c))));
	if (sm > 0) {
		for (const WCoord& c2 : cs) {
			for (int k = 1; k < C.radius[size_t(sm)]; k++) {
				const int nx = c2.x + C.x[size_t(k)], ny = c2.y + C.y[size_t(k)];
				if (!inb(nx, ny)) continue;
				const int dist = vd(C.x[size_t(k)], C.y[size_t(k)]);
				const WData& d = W(nx, ny);
				if (dist <= space_m && (d.flags & 0x10) && !own(nx, ny)) return false;
				if (dist <= space_f && (d.flags & 0x20)) return false;
				if (dist <= space_r && (d.flags & 0x08)) return false;
				if (dist <= space_c && World::is_ocean(d)) return false;
			}
		}
	}
	PlacedMountain pm;
	pm.wx = wx;
	pm.wy = wy;
	pm.range = range_index;
	pm.tcoords = mr->tcoords;
	pm.wcoords = mr->wcoords;
	for (const WCoord& c2 : cs) {
		WData& d = W(c2.x, c2.y);
		d.flags = uint16_t((d.flags & 0xffc3) | (d.flags & 0x404) | wflag::kMountain);
	}
	for (const Offset& o : mr->tcoords) {
		const int tx = wx * 4 + o[0], ty = wy * 4 + o[1];
		w.set_blocked_at(tx, ty, true);
		uint16_t& m = w.t(tx, ty);
		m = uint16_t((m & 0xfffe) | tmask::kMountain);
	}
	for (const Offset& o : mr->tcoords) {
		for (int k = 1; k <= 3; k++) {
			const int x = wx * 4 + o[0] + move_x[k], y = wy * 4 + o[1] + move_y[k];
			if (w.in_t(x, y) && !w.is_mountain_at(x, y)) w.t(x, y) |= tmask::kPermBehind;
		}
	}
	placed_mountains.push_back(std::move(pm));
	return true;
}

// ---------------------------------------------------------------- small placers

// World::set_oil_at @6b2a10: closes the oil goods already on the wcoord; on: OIL flag and a new Oil good at the cell centre.
void Gen::set_oil_at(int x, int y, bool on) {
	for (GoodObj& go : c.goods) {
		if (go.alive() && go.type == kOilGood && wcoord_of(go.x) == x && wcoord_of(go.y) == y) go.flags = uint8_t(go.flags & ~objflag::kValid);
	}
	WData& d = W(x, y);
	if (!on) {
		d.flags &= uint16_t(~wflag::kOil);
		return;
	}
	d.flags |= wflag::kOil;
	GoodObj go;
	go.type = kOilGood;
	go.x = x * 768 + 0x180;
	go.y = y * 768 + 0x180;
	go.flags = objflag::kValid;
	c.goods.push_back(go);
}

// TerrainGroup::available_rough_tile @6a24d0
bool Gen::available_rough_tile(int x, int y, int kind, int mount_sp, int forest_sp, int rock_sp, int coast_sp,
                               const GroupRun* run) {
	if (!inb(x, y)) return false;
	const WData& d = W(x, y);
	if (World::is_ocean(d) || (d.flags & 0x28) || is_edge(x, y) || (d.flags & 0x10)) return false;
	if (start_city_wcoord(x, y)) return false;
	if (d.flags & 0x44) return false;
	const Circles& C = circles();
	auto own = [&](int px, int py) {
		if (!run) return false;
		for (const WCoord& t : run->tiles) {
			if (t.x == px && t.y == py) return true;
		}
		return false;
	};
	auto ring = [&](int space, uint16_t flag, bool water) {
		const int sp = std::min(64, space);
		for (int k = C.radius[0]; k < C.radius[size_t(sp)]; k++) {
			const int nx = x + C.x[size_t(k)], ny = y + C.y[size_t(k)];
			if (!inb(nx, ny)) continue;
			const WData& n = W(nx, ny);
			if (water) {
				if (World::is_ocean(n)) return true;
			} else if ((n.flags & flag) && !own(nx, ny)) {
				return true;
			}
		}
		return false;
	};
	if (mount_sp > 0 && ring(mount_sp, 0x10, false)) return false;
	if (forest_sp > 0 && ring(forest_sp, 0x20, false)) return false;
	if (rock_sp > 0 && ring(rock_sp, 0x08, false)) return false;
	if (coast_sp > 0 && ring(coast_sp, 0, true)) return false;
	if (kind == 8) {
		for (int k = 1; k <= 8; k++) {
			const int nx = x + move_x[k], ny = y + move_y[k];
			if (!inb(nx, ny)) continue;
			if (has_mountain_tcoords(nx, ny)) return false;
			if ((W(nx, ny).flags & 0x08) && !own(nx, ny)) return false;
		}
	}
	return true;
}

// TerrainGroup::drop_tile @6a2ab0
bool Gen::drop_tile(const GroupEntry& g, GroupRun& run, int x, int y, int stay_near) {
	switch (g.type) {
	case kTrees: {
		if (!available_rough_tile(x, y, 0x20, g.mount_space, g.forest_space, g.rock_space, g.coast_space, &run)) return false;
		if (!run.tiles.empty() && vd(run.tiles[0].x - x, run.tiles[0].y - y) > stay_near) return false;
		WData& d = W(x, y);
		if (d.flags & 4) d.land = 2;
		d.flags = uint16_t((d.flags & 0xffc3) | 0x20);
		for (int i = 0; i < 16; i++) w.set_blocked_at(traverse_x(x, i), traverse_y(y, i), true);
		run.tiles.push_back({x, y});
		return true;
	}
	case kRocks: {
		if (!available_rough_tile(x, y, 8, g.mount_space, g.forest_space, g.rock_space, g.coast_space, &run)) return false;
		if (!run.tiles.empty() && vd(run.tiles[0].x - x, run.tiles[0].y - y) > stay_near) return false;
		WData& d = W(x, y);
		if (d.flags & 4) d.land = 2;
		d.flags = uint16_t((d.flags & 0xffc3) | 0x08);
		run.tiles.push_back({x, y});
		return true;
	}
	case kOilGroup:
		set_oil_at(x, y, true);
		run.tiles.push_back({x, y});
		return true;
	default: return false;
	}
}

// TerrainGroup::clear_group @6a28e0
void Gen::clear_group(GroupRun& run, int type) {
	(void)type;
	for (const WCoord& t : run.tiles) {
		WData& d = W(t.x, t.y);
		if (!(d.flags & 0x28)) continue;
		d.flags &= 0xffd7;
		if (d.flags & wflag::kOil) set_oil_at(t.x, t.y, false);
		for (int i = 0; i < 16; i++) {
			const int tx = traverse_x(t.x, i), ty = traverse_y(t.y, i);
			if (!w.is_mountain_at(tx, ty)) w.set_blocked_at(tx, ty, false);
		}
	}
	run.tiles.clear();
}

// TerrainGroup::place_oil_deposits @6a2d90
void Gen::place_oil_deposits(GroupRun& run, int count) {
	count = std::min(count, int(run.tiles.size()));
	int done = 0;
	for (const WCoord& t : run.tiles) {
		if (done >= count) break;
		if (W(t.x, t.y).flags & wflag::kOil) continue;
		bool ok = false;
		for (int k = 1; k <= 8 && !ok; k++) {
			const int nx = t.x + move_x[k], ny = t.y + move_y[k];
			if (inb(nx, ny) && (W(nx, ny).flags & 0x28) == 0 && !has_mountain_tcoords(nx, ny)) ok = true;
		}
		if (ok) {
			set_oil_at(t.x, t.y, true);
			done++;
		}
	}
}

// TerrainGroup::randomize_orthogs @6a2320: rotates the orthogonal and the corner lists (2 draws).
void Gen::randomize_orthogs(int* ro, int* rc) {
	*ro = r.rnd_draw(4);
	*rc = r.rnd_draw(4);
}

// The centre / edge / corner / border tests shared by the placers (randommap-terrain-groups A3 step 2).
bool Gen::tile_tests(const GroupEntry& g, int x, int y) {
	if (!inb(x, y) || is_edge(x, y)) return false;
	if (g.cent_min > 0 || g.cent_max > 0) {
		const int cx = xs / 2 + (x > xs / 2 ? 1 : 0), cy = ys / 2 + (y > ys / 2 ? 1 : 0);
		const int dd = vd(x - cx, y - cy);
		if (g.cent_min > 0 && dd < g.cent_min) return false;
		if (g.cent_max > 0 && dd > g.cent_max) return false;
	}
	if (g.edge_min > 0 || g.edge_max > 0) {
		const int e = edge_d(x, y);
		if (g.edge_min > 0 && e < g.edge_min) return false;
		if (g.edge_max > 0 && e > g.edge_max) return false;
	}
	if (g.corner_min > 0 || g.corner_max > 0) {
		const int k = corner_d(x, y, -1);
		if (g.corner_min > 0 && k < g.corner_min) return false;
		if (g.corner_max > 0 && k > g.corner_max) return false;
	}
	return true;
}

// The growth of one clump from its seed (A3 step 3 "grow"): 0 done, 1 the seed cannot be dropped, 2 growth stuck.
int Gen::grow_clump(const GroupEntry& g, GroupRun& run, int cx, int cy, int size, int sx, int sy, bool has_start,
                    Shape* shape, bool copy) {
	(void)copy;
	run.tiles.clear();
	if (!drop_tile(g, run, cx, cy, size / 4 + 1)) return 1;
	std::vector<char> exhausted(1, 0);
	int guard = 0;
	while (int(run.tiles.size()) < size && ++guard < 300 * size + 300) {
		const int n = int(run.tiles.size());
		const int src = n > 1 ? r.rnd(n) : 0;
		if (exhausted.size() < size_t(n)) exhausted.resize(size_t(n), 0);
		int ro, rc;
		randomize_orthogs(&ro, &rc);
		bool added = false;
		const WCoord s0 = run.tiles[size_t(src)];
		for (int d = 0; d < 4 && int(run.tiles.size()) < size; d++) {
			int dx, dy;
			if (n < 3 || r.bit() == 0) {
				const int o = (d + ro) % 4 + 1;
				dx = orthog_x[o];
				dy = orthog_y[o];
			} else {
				const int o = (d + rc) % 4;
				dx = kCornerX[o];
				dy = kCornerY[o];
			}
			const int nx = s0.x + dx, ny = s0.y + dy;
			bool is_tile = false;
			for (const WCoord& t : run.tiles) {
				if (t.x == nx && t.y == ny) is_tile = true;
			}
			if (is_tile || !inb(nx, ny)) continue;
			if (has_start) {
				const int dd = vd(nx - sx, ny - sy);
				if ((g.start_min > 0 && dd < g.start_min) || (g.start_max > 0 && dd > g.start_max)) continue;
			} else if (g.start_min > 0) {
				bool near = false;
				for (const WCoord& s2 : starts) {
					if (vd(nx - s2.x, ny - s2.y) < g.start_min) near = true;
				}
				if (near) continue;
			}
			if (!tile_tests(g, nx, ny)) continue;
			if (drop_tile(g, run, nx, ny, size / 4 + 1)) added = true;
		}
		if (!added) {
			exhausted[size_t(src)] = 1;
			bool all = true;
			for (int i = 0; i < int(run.tiles.size()); i++) {
				if (i >= int(exhausted.size()) || !exhausted[size_t(i)]) all = false;
			}
			if (all) return 2;  // "Growth Stuck! Removing group"
		}
	}
	if (int(run.tiles.size()) < size) return 2;
	if (shape && g.type == kTrees) {
		shape->ox.clear();
		shape->oy.clear();
		for (const WCoord& t : run.tiles) {
			shape->ox.push_back(t.x - run.tiles[0].x);
			shape->oy.push_back(t.y - run.tiles[0].y);
		}
	}
	return 0;
}

// TerrainGroup::place_player_group @6a4190
bool Gen::place_player_group(const GroupEntry& g, int type_t, int size, int p, int oilcount, int range_index, Shape* shape,
                             bool copy, GroupRun& run) {
	(void)type_t;
	if (p < 0 || p >= int(starts.size())) return false;
	const Circles& C = circles();
	const WCoord st = starts[size_t(p)];
	const int reg0 = inb(st.x, st.y) ? W(st.x, st.y).region : 0;
	int smax = g.start_max;
	if (smax == 0 || smax > 0x40) smax = 0x40;
	const int lo = C.radius[size_t(std::min(64, std::max(0, g.start_min)))], hi = C.radius[size_t(smax)];
	if (hi <= lo) return false;
	const int i0 = (lo < hi - 1) ? lo + r.rnd(hi - lo) : lo;
	const int span = hi - lo;
	for (int k = 0; k < span; k++) {
		const int i = lo + ((i0 + 1 - lo + k) % span);
		const int x = st.x + C.x[size_t(i)], y = st.y + C.y[size_t(i)];
		if (!inb(x, y)) continue;
		const int rr = W(x, y).region;
		if (!(rr > 63 || rr == reg0)) continue;
		const int own_d = vd(x - st.x, y - st.y);
		bool closer = false;
		for (size_t q = 0; q < starts.size(); q++) {
			if (int(q) != p && vd(x - starts[q].x, y - starts[q].y) < own_d) closer = true;
		}
		if (closer) continue;
		const WData& d = W(x, y);
		if ((d.flags & 0x10) || (d.flags & 0x68) || (d.flags & 0x1800) || d.blocked != 0) continue;
		if (!tile_tests(g, x, y)) continue;
		if (g.type == kTrees) {
			bool bad = false;
			for (int nb = 1; nb <= 8 && !bad; nb++) {
				const int nx = x + move_x[nb], ny = y + move_y[nb];
				if (!inb(nx, ny)) continue;
				if (W(nx, ny).flags & 0x40) bad = true;
				if (copy && (W(nx, ny).flags & 0x20)) bad = true;
			}
			if (bad) continue;
		}
		switch (g.type) {
		case kMountains:
			if (add_mountain(range_index, x, y, g, g.mount_space, g.forest_space, g.rock_space, g.coast_space, g.start_min))
				return true;
			continue;
		case kCliffs: return false;  // Cliffs::position_cliff is not decoded
		case kTrees:
			if (shape && !shape->ox.empty()) {
				const int dm = r.mask(3);
				const int mx = kCornerX[dm], my = kCornerY[dm];
				bool ok = true;
				run.tiles.clear();
				for (size_t t = 0; t < shape->ox.size() && ok; t++) {
					ok = available_rough_tile(x + mx * shape->ox[t], y + my * shape->oy[t], 0x20, g.mount_space,
					                          g.forest_space, g.rock_space, g.coast_space, &run);
					if (ok) run.tiles.push_back({x + mx * shape->ox[t], y + my * shape->oy[t]});
				}
				run.tiles.clear();
				if (ok) {
					for (size_t t = 0; t < shape->ox.size(); t++) drop_tile(g, run, x + mx * shape->ox[t], y + my * shape->oy[t], 99);
					return true;
				}
			}
			/* fall through */
		default: {
			Shape* rec = (g.type == kTrees) ? shape : nullptr;
			const int gr = grow_clump(g, run, x, y, size, st.x, st.y, true, rec, copy);
			if (gr == 1) continue;  // the seed cannot be dropped here: next candidate
			if (gr == 2) {
				clear_group(run, g.type);
				return false;
			}
			if (g.type == kRocks && oilcount > 0) place_oil_deposits(run, oilcount);
			return true;
		}
		}
	}
	return false;
}

}  // namespace ron::sim::mapgen
