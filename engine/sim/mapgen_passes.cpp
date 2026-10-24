// TerrainGroups::place_all (docs/randommap-terrain-groups.md A2, A4) and the passes that follow it: add_doobers,
// treeify_mountains, nubify_forest, the tileset base changes and fix_transitions / nubify_transitions (A6).
#include <cstdlib>
#include <cstdio>
#include <array>

#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

// TerrainGroups::add_doobers @6a1540 (decompile read): cosmetic bushes along the south / east edge of forest wcoords and rocks
// beside mountains. Doober::add_doober draws nothing, but its position (world units, 768 per wcoord) decides which wcoords
// Doober::has_doobers @8466d0 (floor(px / 768), floor(py / 768)); the oracle's draws fit without any skip in the rock pass.
void Gen::add_doobers() {
	doober_cells.assign(size_t(cells), 0);  // wcoords holding a doober
	doober_count = 0;
	auto place = [&](int tx, int ty, int count, int spacing, int tag) {
		(void)tag;
		const unsigned lx = unsigned(tx) * unsigned(ty);
		const int a = int(lx & 0x7f) - 0x3f;
		const int b = int(unsigned(ty + tx) & 0x7fu) - 0x3f;
		const unsigned ia = unsigned(a) & 3u, ib = unsigned(b) & 3u;
		for (int i = 0; i < count; i++) {
			const int px = a + tx * 0xc0 + 0x60 + (spacing + int((lx + unsigned(i)) & 3u) * 0x28) * move_x[ia + unsigned(i)];
			const int py = (spacing + int(unsigned(ty + tx + i) & 3u) * 0x28) * move_y[ib + unsigned(i)] + b + ty * 0xc0 + 0x60;
			doober_count++;
			if (px >= 0 && py >= 0) {
				const int cx = px / 768, cy = py / 768;
				if (cx < xs && cy < ys) doober_cells[size_t(cy) * size_t(xs) + size_t(cx)] = 1;
			}
		}
	};
	auto count_of = [&](int tx, int ty, int mn, int mx) {
		const int span = std::abs(mx - mn) + 1;
		return int((long long)ty * tx * tx % span) + mn;
	};
	for (int x = 1; x <= xs - 2; x++) {
		for (int y = 1; y <= ys - 2; y++) {
			const WData& d = W(x, y);
			if (!(d.flags & 0x20) || (d.flags & 0x1000)) continue;
			const bool south = y + 1 < ys && !(W(x, y + 1).flags & 0x20);
			const bool east = x + 1 < xs && !(W(x + 1, y).flags & 0x20);
			if (south && r.rnd_draw(100) < tgd.bush_clump_prob) {
				const int u = r.get() & 3;
				const int tx = x * 4 + u, ty = y * 4 + 3;
				const int n = count_of(tx, ty, tgd.bush_min, tgd.bush_max);
				if (n > 0) place(tx, ty, n, tgd.bush_spacing, 0);
			}
			if (east && r.rnd_draw(100) < tgd.bush_clump_prob) {
				const int u = r.get() & 3;
				const int tx = x * 4 + 3, ty = y * 4 + u;
				const int n = count_of(tx, ty, tgd.bush_min, tgd.bush_max);
				if (n > 0) place(tx, ty, n, tgd.bush_spacing, 0);
			}
		}
	}
	for (int x = 1; x <= xs - 2; x++) {
		for (int y = 1; y <= ys - 2; y++) {
			const WData& d = W(x, y);
			if (World::is_ocean(d) || (d.flags & 0x38) || (d.flags & 0x1000)) continue;
			// Doober::has_doobers @8466d0 never reports a doober for these wcoords in the oracle (the draws only fit without a skip)
			if ((W(x - 1, y).flags & 0x10) || (W(x, y - 1).flags & 0x10)) {
				if (r.rnd_draw(100) < tgd.mnt_rock_clump_prob) {
					const int v = r.get() & 15;
					const int tx = x * 4 + (v & 3), ty = y * 4 + (v >> 2);
					const int n = count_of(tx, ty, tgd.mnt_rock_min, tgd.mnt_rock_max);
					if (n > 0) place(tx, ty, n, tgd.mnt_rock_spacing, 1);
				}
			}
		}
	}
}

// TerrainGroups::treeify_mountains @6a1cc0 (disassembly-checked decompile): a wcoord with mountain tcoords that is not itself
// a mountain wcoord turns into forest when neither its (x, y+1) nor its (x+1, y) neighbour is a mountain wcoord; a mountain
// wcoord that has an open (no mountain tcoords) +x or +y orthogonal neighbour becomes forest-and-mountain (0x30). Both only
// off the coast (0x404) and with one draw each.
void Gen::treeify_mountains(int prob) {
	auto is_mountain = [&](int x, int y) { return inb(x, y) && (W(x, y).flags & 0x10) != 0; };
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			WData& d = W(x, y);
			if (d.flags & 0x10) {
				bool open = false;
				const int ox[2] = {1, 0}, oy[2] = {0, 1};  // orthog indices 2 and 3
				for (int k = 0; k < 2 && !open; k++) {
					const int nx = x + ox[k], ny = y + oy[k];
					if (inb(nx, ny) && !has_mountain_tcoords(nx, ny)) open = true;
				}
				if (open && (d.flags & 0x404) == 0) {
					if (r.rnd_draw(100) < prob) {
						d.flags = uint16_t(d.flags & 0xffc3);
						d.land = 0;
						d.flags |= 0x30;
					}
				}
			} else if (has_mountain_tcoords(x, y)) {
				if (!is_mountain(x, y + 1) && !is_mountain(x + 1, y) && (d.flags & 0x404) == 0) {
					if (r.rnd_draw(100) < prob) {
						d.flags = uint16_t(d.flags & 0xffc3);
						d.land = 0;
						d.flags |= 0x20;
					}
				}
			}
		}
	}
}

// TerrainGroups::nubify_forest @6a93b0 (decompile read): along every column (then every row) of forest wcoords a straight run of
// four or more that continues in the direction of its first neighbour grows a one-cell nub beside it. The run test comes
// first (a cell with forest on both sides still continues a run), the run restarts after every draw.
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
			if (dir != 0 && forest(x + dir, y)) {
				if (++run > 3) {
					const int k = r.mask(3);
					nub(x + dir, y - k);
					run = 0;
				}
			} else if (forest(x - 1, y) && forest(x + 1, y)) {
				run = 0;
			} else {
				run = 1;
				dir = forest(x + 1, y) ? 1 : forest(x - 1, y) ? -1 : 0;
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
			if (dir != 0 && forest(x, y + dir)) {
				if (++run > 3) {
					const int k = r.mask(3);
					nub(x - k, y + dir);
					run = 0;
				}
			} else if (forest(x, y - 1) && forest(x, y + 1)) {
				run = 0;
			} else {
				run = 1;
				dir = forest(x, y + 1) ? 1 : forest(x, y - 1) ? -1 : 0;
			}
		}
	}
}

// TerrainGroups::change_forest_base @6a9a60 / change_mountain_base @6a9c10 / change_coast_base @6a9dc0 (decompiles read; Map::make
// calls each only when the tileset's base is >= 0): a source cell and every grown neighbour also get land 0; a neighbour
// qualifies when it is not itself a source and not land 2.
void Gen::change_forest_base() {
	const int base = tgd.forest_base;
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			if (!(W(x, y).flags & 0x20)) continue;
			if (base >= 0) W(x, y).land_sub = uint8_t(base);
			W(x, y).land = 0;
			for (int o = 1; o <= 4; o++) {
				const int nx = x + orthog_x[o], ny = y + orthog_y[o];
				if (!inb(nx, ny) || (W(nx, ny).flags & 0x20) || W(nx, ny).land == 2) continue;
				if (r.rnd_draw(100) < tgd.forest_growth_prob) {
					if (base >= 0) W(nx, ny).land_sub = uint8_t(base);
					W(nx, ny).land = 0;
				}
			}
		}
	}
}

void Gen::change_mountain_base() {
	const int base = tgd.mountain_base;
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			if (!has_mountain_tcoords(x, y)) continue;
			if (base >= 0) W(x, y).land_sub = uint8_t(base);
			W(x, y).land = 0;
			for (int o = 1; o <= 4; o++) {
				const int nx = x + orthog_x[o], ny = y + orthog_y[o];
				if (!inb(nx, ny) || has_mountain_tcoords(nx, ny) || W(nx, ny).land == 2) continue;
				if (r.rnd_draw(100) < tgd.mountain_growth_prob) {
					if (base >= 0) W(nx, ny).land_sub = uint8_t(base);
					W(nx, ny).land = 0;
				}
			}
		}
	}
}

void Gen::change_coast_base() {
	const int base = tgd.coast_base;
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			if (!(W(x, y).flags & 0x04)) continue;
			if (base >= 0) W(x, y).land_sub = uint8_t(base);
			W(x, y).land = 0;
			for (int o = 1; o <= 4; o++) {
				const int nx = x + orthog_x[o], ny = y + orthog_y[o];
				if (!inb(nx, ny) || (W(nx, ny).flags & 0x04) || W(nx, ny).land == 2) continue;
				if (r.rnd_draw(100) < tgd.coast_growth_prob) {
					if (base >= 0) W(nx, ny).land_sub = uint8_t(base);
					W(nx, ny).land = 0;
				}
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
