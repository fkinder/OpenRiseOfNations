// TerrainGroups::place_all (docs/randommap-terrain-groups.md A2, A4) and the passes that follow it: add_doobers,
// treeify_mountains, nubify_forest, the tileset base changes and fix_transitions / nubify_transitions (A6).
#include <cstdlib>
#include <cstdio>
#include <array>

#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

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
