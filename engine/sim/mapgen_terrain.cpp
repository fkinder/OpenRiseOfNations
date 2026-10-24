// TerrainGroups::place_all and the passes after it (docs/randommap-terrain-groups.md part A): forests, mountains, rocks and
// oil, doobers (draws only), mountain fringe trees, nubified forest edges, tileset base changes and subtype transitions.
// Cliffs are not placed: Cliffs::position_cliff / verify_defensive_position were never decoded (see the report).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
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
	// The circular lists are built by inserting at the head: a forward walk reads them in reverse file order (oracle:
	// current 2.SVX, the five home mountains took ranges 9, 8, 7, 14, 13 of the medium list 7..14).
	for (int i = 1; i <= 3; i++) {
		std::reverse(mountain_lists[i].begin(), mountain_lists[i].end());
		mountain_cursor[i] = 0;
	}
}

const MountainRange* Gen::mount_range(int index) {
	const auto cached = mount_ptrs.find(index);
	if (cached != mount_ptrs.end()) return cached->second;
	std::lock_guard<std::mutex> lock(g_mt_mutex);
	std::unique_ptr<MountainTemplates>& p = g_mt[c.files.root()];
	if (!p) p = std::make_unique<MountainTemplates>(c.files);
	const MountainRange* mr = p->get(index);
	mount_ptrs[index] = mr;
	return mr;
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

// The template's face wcoords (Mountains list +0x44 / +0x50 / +0x6c): the distinct wcoords of its complete cells in tcoord order.
// Derived from the tcoord offsets: a wcoord offset is the floor division of the tcoord offset by 4.
static std::vector<Offset> face_wcoords_of(const MountainRange& mr) {
	auto fdiv = [](int v) { return v >= 0 ? v / 4 : -((-v + 3) / 4); };
	std::vector<Offset> out;
	for (const Offset& t : mr.tcoords) {
		const Offset f{fdiv(t[0]), fdiv(t[1])};
		bool seen = false;
		for (const Offset& o : out) {
			if (o == f) {
				seen = true;
				break;
			}
		}
		if (!seen) out.push_back(f);
	}
	return out;
}

// Mountains::quick_verify_template @898ab0: every face wcoord is land (half-land counts), free of 0x30 / 0x48 flags, outside the
// start-city cells, and has no start-city cell among its eight neighbours (an off-map neighbour fails too).
bool Gen::quick_verify_template(const std::vector<Offset>& face, int wx, int wy) {
	const Circles& C = circles();
	for (const Offset& f : face) {
		const int x = f[0] + wx, y = f[1] + wy;
		if (x < 0 || y < 0 || x >= xs || y >= ys) return false;
		const WData& d = W(x, y);
		if (d.flags & 0x30) return false;
		if (!(d.flags & 0x100) && (d.land == 1 || d.land == 2)) return false;
		if (d.flags & 0x48) return false;
		if (start_city_wcoord(x, y)) return false;
		for (int k = C.radius[0]; k < C.radius[1]; k++) {
			const int nx = C.x[size_t(k)] + x, ny = C.y[size_t(k)] + y;
			if (nx < 0 || ny < 0 || nx >= xs || ny >= ys) return false;
			if (start_city_wcoord(nx, ny)) return false;
		}
	}
	return true;
}

// Mountains::add_mountain @89c2e0. mode 4: excluding_verify (the template at the asked place), mode 5: sliding_excluding_verify
// (the template may slide so that any one of its face wcoords lands on the asked cell; the first that fits wins).
bool Gen::add_mountain(int range_index, int wx, int wy, const GroupEntry& g, int space_m, int space_f, int space_r,
                       int space_c, int start_min, int mode) {
	(void)g;
	if (range_index < 0) return false;
	const MountainRange* mr = mount_range(range_index);
	if (!mr) return false;
	auto fit = mount_faces.find(range_index);
	if (fit == mount_faces.end()) fit = mount_faces.emplace(range_index, face_wcoords_of(*mr)).first;
	const std::vector<Offset>& face = fit->second;
	const Circles& C = circles();
	if (mount_seen.size() != size_t(xs) * size_t(ys)) mount_seen.assign(size_t(xs) * size_t(ys), 0);
	std::vector<uint8_t>& seen = mount_seen;
	std::vector<int> to_clear;
	auto near_start = [&](int x, int y) {
		for (const WCoord& s : starts) {
			if (vd(x - s.x, y - s.y) < start_min) return true;
		}
		return false;
	};
	bool verified = false;
	if (mode == 4) {
		bool fail = false;
		for (const Offset& f : face) {
			const int x = f[0] + wx, y = f[1] + wy;
			if (x < 0 || y < 0 || x >= xs || y >= ys) continue;
			const size_t ci = size_t(y) * size_t(xs) + size_t(x);
			if (seen[ci]) continue;
			seen[ci] = 1;
			to_clear.push_back(int(ci));
			if (near_start(x, y)) {
				fail = true;
				break;
			}
		}
		for (int ci : to_clear) seen[size_t(ci)] = 0;
		if (fail) return false;
		verified = quick_verify_template(face, wx, wy);
	} else {
		for (size_t k = 0; k < face.size() && !verified; k++) {
			bool bad = false;
			for (size_t j = 0; j < face.size() && !bad; j++) {
				const int x = face[j][0] - face[k][0] + wx, y = face[j][1] - face[k][1] + wy;
				if (x < 0 || y < 0 || x >= xs || y >= ys) continue;
				const size_t ci = size_t(y) * size_t(xs) + size_t(x);
				if (seen[ci]) continue;
				seen[ci] = 1;  // the marks survive the next slide: only cleared when the call ends (original quirk)
				to_clear.push_back(int(ci));
				if (near_start(x, y)) bad = true;
			}
			if (bad) continue;
			const int nx = wx - face[k][0], ny = wy - face[k][1];
			if (quick_verify_template(face, nx, ny)) {
				verified = true;
				wx = nx;
				wy = ny;
			}
		}
		for (int ci : to_clear) seen[size_t(ci)] = 0;
	}
	if (!verified) return false;
	for (const Offset& f : face) {
		const int x = f[0] + wx, y = f[1] + wy;
		for (int k = C.radius[0]; k < C.radius[size_t(space_m)]; k++) {
			const int nx = C.x[size_t(k)] + x, ny = C.y[size_t(k)] + y;
			if (inb(nx, ny) && (W(nx, ny).flags & 0x50)) return false;
		}
		for (int k = C.radius[0]; k < C.radius[size_t(space_c)]; k++) {
			const int nx = C.x[size_t(k)] + x, ny = C.y[size_t(k)] + y;
			if (inb(nx, ny) && (W(nx, ny).flags & 0x04)) return false;
		}
		for (int k = C.radius[0]; k < C.radius[size_t(space_f)]; k++) {
			const int nx = C.x[size_t(k)] + x, ny = C.y[size_t(k)] + y;
			if (inb(nx, ny) && (W(nx, ny).flags & 0x20)) return false;
		}
		for (int k = C.radius[0]; k < C.radius[size_t(space_r)]; k++) {
			const int nx = C.x[size_t(k)] + x, ny = C.y[size_t(k)] + y;
			if (inb(nx, ny) && (W(nx, ny).flags & 0x08)) return false;
		}
	}
	PlacedMountain pm;
	pm.wx = wx;
	pm.wy = wy;
	pm.range = range_index;
	pm.tcoords = mr->tcoords;
	pm.wcoords = mr->wcoords;
	for (const Offset& o : mr->wcoords) {
		const int x = wx + o[0], y = wy + o[1];
		if (!inb(x, y)) continue;
		WData& d = W(x, y);
		const uint16_t keep = uint16_t(d.flags & 0x404);
		d.flags = uint16_t((d.flags & 0xffc3) | keep | wflag::kMountain);
	}
	for (const Offset& o : mr->tcoords) {
		const int tx = wx * 4 + o[0], ty = wy * 4 + o[1];
		if (!w.in_t(tx, ty)) continue;
		w.set_blocked_at(tx, ty, true);
		uint16_t& m = w.t(tx, ty);
		m = uint16_t((m & 0xfffe) | tmask::kMountain);
	}
	for (const Offset& o : mr->tcoords) {
		for (int k = 1; k <= 3; k++) {
			const int x = wx * 4 + o[0] + move_x[k], y = wy * 4 + o[1] + move_y[k];
			if (w.in_t(x, y) && (w.t(x, y) & 3) != 2) w.t(x, y) |= tmask::kPermBehind;
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
bool Gen::drop_tile(const GroupEntry& g, GroupRun& run, int x, int y, int stay_near, int range_index) {
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
		(void)stay_near;
		if (!available_rough_tile(x, y, 8, g.mount_space, g.forest_space, g.rock_space, g.coast_space, &run)) return false;
		WData& d = W(x, y);
		if (d.flags & 4) d.land = 2;
		d.flags = uint16_t((d.flags & 0xffc3) | 0x08);
		run.tiles.push_back({x, y});
		return true;
	}
	case kMountains:
		if (!add_mountain(range_index, x, y, g, g.mount_space, g.forest_space, g.rock_space, g.coast_space, g.start_min, 4)) return false;
		run.tiles.push_back({x, y});
		return true;
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

// The growth of one per-player clump after its seed tile (place_player_group @6a4190, LAB_006a4e40): 0 done (size reached),
// 1 stuck. Every round picks the source tile (trees always grow from the first tile), re-rotates the four direction lists
// (randomize_orthogs, 2 draws) and tries its four orthogonal / diagonal neighbours; one dropped tile restarts the round.
int Gen::grow_player(const GroupEntry& g, GroupRun& run, int size, WCoord st, int smin, int smax, Shape* shape) {
	// The circular lists are built by inserting [1..4] at the head, so a forward walk from the head reads [4], [3], [2], [1].
	const int stay = size / 4 + 1;
	for (;;) {  // LAB_006a4e40
		const int n0 = int(run.tiles.size());
		if (n0 == 0) return 1;
		int cur = (n0 - 1 < 1) ? 0 : r.rnd_draw(n0);
		cur += 1;
		const int start_cur = cur;
		int src_base = g.type != kTrees ? cur : 0;
		bool restart = false;
		do {
			const bool b = cur != int(run.tiles.size());
			const int next_cur = b ? cur : 0;
			const int s = b ? src_base : 0;
			int ro, rc;
			randomize_orthogs(&ro, &rc);
			for (int k = 0; k < 4 && !restart; k++) {
				int dx, dy;
				if (run.tiles.size() < 3 || r.bit() == 0) {
					const int o = 4 - (ro + k) % 4;
					dx = orthog_x[o];
					dy = orthog_y[o];
				} else {
					const int ci = 3 - (rc + k) % 4;
					dx = kCornerX[ci];
					dy = kCornerY[ci];
				}
				const WCoord src = run.tiles[size_t(s)];
				const int cx = src.x + dx, cy = src.y + dy;
				bool rej = false;
				for (const WCoord& t : run.tiles) {
					if (t.x == cx && t.y == cy) rej = true;
				}
				if (!rej && (!inb(cx, cy) || start_city_wcoord(cx, cy))) rej = true;
				if (!rej) {
					const int dd = vd(cx - st.x, cy - st.y);
					if (dd < smin || dd > smax) rej = true;
				}
				if (!rej && (g.cent_min > 0 || g.cent_max > 0)) {
					const int ccx = xs / 2 + (cx > xs / 2 ? 1 : 0), ccy = ys / 2 + (cy > ys / 2 ? 1 : 0);
					const int dd = vd(cx - ccx, cy - ccy);
					if ((g.cent_min > 0 && dd < g.cent_min) || (g.cent_max > 0 && dd > g.cent_max)) rej = true;
				}
				if (!rej && (g.edge_min > 0 || g.edge_max > 0)) {
					const int e = edge_d(cx, cy);
					if ((g.edge_min > 0 && e < g.edge_min) || (g.edge_max > 0 && e > g.edge_max)) rej = true;
				}
				if (!rej && (g.corner_min > 0 || g.corner_max > 0)) {
					const int e = corner_d(cx, cy, -1);
					if ((g.corner_min > 0 && e < g.corner_min) || (g.corner_max > 0 && e > g.corner_max)) rej = true;
				}
				if (rej) continue;
				if (drop_tile(g, run, cx, cy, stay)) {
					if (g.type == kTrees && shape) {
						shape->ox.push_back(cx - run.tiles[0].x);
						shape->oy.push_back(cy - run.tiles[0].y);
					}
					if (int(run.tiles.size()) < size) restart = true;
					else return 0;
				}
			}
			if (restart) break;
			cur = next_cur + 1;
			src_base = g.type == kTrees ? s : s + 1;
		} while (cur != start_cur);
		if (!restart) return 1;
	}
}

// TerrainGroup::place_player_group @6a4190
bool Gen::place_player_group(const GroupEntry& g, int type_t, int size, int p, int oilcount, int range_index, Shape* shape,
                             bool copy, GroupRun& run) {
	(void)type_t;
	run.tiles.clear();
	if (p < 0 || p >= int(starts.size())) return false;
	const Circles& C = circles();
	const WCoord st = starts[size_t(p)];
	const int reg0 = inb(st.x, st.y) ? W(st.x, st.y).region : 0;
	const int smin = g.start_min;
	int smax = g.start_max;
	if (smax == 0 || smax > 0x40) smax = 0x40;
	if (smax < smin) return false;
	const int lo = C.radius[size_t(std::min(64, std::max(0, smin)))], hi = C.radius[size_t(smax)];
	int i0 = lo;
	if (lo < hi - 1) i0 = r.rnd_draw(hi - lo) + lo;
	const int stop = i0 + 1;
	int idx = stop;
	const int np = int(starts.size());
	for (;;) {
		if (idx == hi) idx = lo;
		const int x = st.x + C.x[size_t(idx)], y = st.y + C.y[size_t(idx)];
		if (inb(x, y) && (W(x, y).region > 63 || W(x, y).region == reg0)) {
			bool rej = false;
			const int own_d = vd(x - st.x, y - st.y);
			for (int q = 0; q < np && !rej; q++) {
				if (q != p && vd(x - starts[size_t(q)].x, y - starts[size_t(q)].y) < own_d) rej = true;
			}
			const WData& d = W(x, y);
			if ((d.flags & 0x10) || (d.flags & 0x68) || (d.flags & 0x1800) || d.blocked != 0) {
				rej = true;
			} else if (!rej) {
				if (g.cent_min > 0 || g.cent_max > 0) {
					const int ccx = xs / 2 + (x > xs / 2 ? 1 : 0), ccy = ys / 2 + (y > ys / 2 ? 1 : 0);
					const int dd = vd(x - ccx, y - ccy);
					if ((g.cent_min > 0 && dd < g.cent_min) || (g.cent_max > 0 && dd > g.cent_max)) rej = true;
				}
			}
			if (g.type == kTrees && copy) {  // no forest tile within the 9 x 9 block
				for (int k = 0; k < 81; k++) {
					const int nx = x + move_x[k], ny = y + move_y[k];
					if (inb(nx, ny) && (W(nx, ny).flags & 0x20)) rej = true;
				}
			}
			if (!rej && (g.edge_min > 0 || g.edge_max > 0)) {
				const int e = edge_d(x, y);
				if ((g.edge_min > 0 && e < g.edge_min) || (g.edge_max > 0 && e > g.edge_max)) rej = true;
			}
			if (x == 0 || y == 0 || x == xs - 1 || y == xs - 1) rej = true;
			if (!rej && (g.corner_min > 0 || g.corner_max > 0)) {
				const int e = corner_d(x, y, -1);
				if ((g.corner_min > 0 && e < g.corner_min) || (g.corner_max > 0 && e > g.corner_max)) rej = true;
			}
			if (g.type == kTrees) {
				for (int k = 0; k < 9; k++) {
					const int nx = x + move_x[k], ny = y + move_y[k];
					if (inb(nx, ny) && (W(nx, ny).flags & 0x40)) rej = true;
				}
			}
			if (!rej) {
				if (g.type == kMountains) {
					if (add_mountain(range_index, x, y, g, g.mount_space, g.forest_space, g.rock_space, g.coast_space, g.start_min, 5))
						return true;
				} else if (g.type == kCliffs) {
					// Cliffs::position_cliff / verify_defensive_position are not decoded
				} else if (g.type == kTrees && shape && !shape->ox.empty()) {
					const int dm = r.mask(3);
					const int mx = kCornerX[dm], my = kCornerY[dm];
					bool ok = true;
					for (size_t t = 0; t < shape->ox.size() && ok; t++) {
						GroupRun none;
						ok = available_rough_tile(x + mx * shape->ox[t], y + my * shape->oy[t], 0x20, g.mount_space, g.forest_space,
						                          g.rock_space, g.coast_space, &none);
					}
					if (ok) {
						for (size_t t = 0; t < shape->ox.size(); t++) drop_tile(g, run, x + mx * shape->ox[t], y + my * shape->oy[t], size);
						return true;
					}
				} else if (drop_tile(g, run, x, y, size / 4 + 1)) {
					if (g.type == kTrees && shape) {
						shape->ox.push_back(0);
						shape->oy.push_back(0);
					}
					const int gr = grow_player(g, run, size, st, smin, smax, shape);
					if (gr == 0) {
						if (g.type == kRocks) place_oil_deposits(run, oilcount);
						return true;
					}
					if (int(run.tiles.size()) < size) clear_group(run, g.type);
				}
			}
		}
		idx++;
		if (idx == stop) break;
	}
	return false;
}

}  // namespace ron::sim::mapgen
