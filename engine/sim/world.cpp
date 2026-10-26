#include "sim/world.hpp"

#include <algorithm>

#include "sim/roadgraph.hpp"

namespace ron::sim {

bool CollBlock::empty() {
	if (flags & 1) return true;
	if (flags == 0) return false;
	uint8_t any = 0;
	for (uint8_t b : bytes) any |= b;
	flags = any ? 0 : 1;
	return any == 0;
}

void World::init(int32_t x, int32_t y) {
	xs = x;
	ys = y;
	size = xs * ys;
	fog_xs = (xs * 8) >> 2;
	fog_ys = (ys * 8) >> 2;
	fog_size = fog_xs * fog_ys;
	tile_xs = xs * 4;
	tile_ys = ys * 4;
	tile_size = tile_xs * tile_ys;
	reg_xs = (xs * 4) >> 3;
	reg_ys = (ys * 4) >> 3;
	reg_size = reg_xs * reg_ys;
	sea_map = -1;
	wdata.assign(size_t(size), WData());
	tdata.assign(size_t(tile_size), 0);
	seen.assign(size_t(fog_size), 0);
	seen2.assign(size_t(fog_size), 0);
	seen3.assign(size_t(fog_size), 0);
	wcoord_seen.assign(size_t(size), 0);
	danger.assign(size_t(reg_size) * 8, 0);
	coll_blocks.clear();
	free_coll_blocks.clear();
	empty_colls = 0;
	roads = RoadStore();
	roads.slot.assign(size_t(size), -1);
}

// ---------------------------------------------------------------- predicates

int32_t World::gather_land(int32_t wx, int32_t wy) const {
	const WData& d = w(wx, wy);
	const uint16_t f = d.flags;
	if (f & wflag::kCoast) return land::kCoast;
	if (f & wflag::kForest) return land::kForest;
	if (f & (wflag::kMountain | wflag::kCliff)) return land::kMountains;
	if (f & wflag::kRock) return (f & wflag::kOil) ? land::kOil : land::kRocks;
	if (is_ocean(d) && (f & wflag::kOil)) return land::kOil;
	return d.land;
}

int32_t World::gather_land_tile(int32_t tx, int32_t ty) const {
	const WData& d = w(tx >> 2, ty >> 2);
	const uint16_t f = d.flags;
	const uint16_t m = t(tx, ty);
	if (f & wflag::kCoast) return (m & tmask::kMedium) == tmask::kWater ? land::kOcean : land::kLand;
	if (f & wflag::kForest) return land::kForest;
	if ((m & tmask::kLowField) == tmask::kMountain) return land::kMountains;
	if (f & wflag::kOil) return land::kOil;
	if (f & wflag::kRock) return land::kRocks;
	return d.land;
}

bool World::has_blocked_neighbors(int32_t tx, int32_t ty) const {
	for (int k = 1; k <= 8; k++) {
		const int32_t x = tx + move_x[k], y = ty + move_y[k];
		if (in_t(x, y) && (t(x, y) & tmask::kBlocked)) return true;
	}
	return false;
}

bool World::has_gather_access(int32_t tx, int32_t ty) const {
	const uint16_t m = t(tx, ty);
	const bool gatherable = (m & tmask::kMedium) == tmask::kTree || (m & tmask::kLowField) == tmask::kMountain ||
	                        (m & tmask::kLowField) == tmask::kCliff;
	if (!gatherable) return false;
	for (int k = 1; k <= 4; k++) {
		const int32_t x = tx + orthog_x[k], y = ty + orthog_y[k];
		if (!in_t(x, y)) continue;
		const uint16_t n = t(x, y);
		if ((n & tmask::kMedium) != tmask::kWater && !(n & tmask::kBlocked)) return true;
	}
	return false;
}

int32_t World::get_tregion(int32_t tx, int32_t ty) const {
	const WData& d = wdata[w_of_t(tx, ty)];
	return (d.flags & wflag::kHalfLand) && (t(tx, ty) & tmask::kMedium) == tmask::kWater ? d.region2 : d.region;
}

bool World::is_edge_of_region(int32_t wx, int32_t wy) const {
	const int32_t r = w(wx, wy).region;
	for (int k = 1; k <= 8; k++) {
		const int32_t x = wx + move_x[k], y = wy + move_y[k];
		if (in_w(x, y) && w(x, y).region != r) return true;
	}
	return false;
}

// ---------------------------------------------------------------- writers

void World::set_bad_path(int32_t tx, int32_t ty, bool on) {
	uint16_t& m = t(tx, ty);
	WData& d = wdata[w_of_t(tx, ty)];
	if (on) {
		if (!(m & tmask::kBadPath)) d.bad++;
		m |= tmask::kBadPath;
	} else {
		if (m & tmask::kBadPath) d.bad--;
		m &= uint16_t(~tmask::kBadPath);
	}
}

void World::set_blocked_at(int32_t tx, int32_t ty, bool on) {
	uint16_t& m = t(tx, ty);
	WData& d = wdata[w_of_t(tx, ty)];
	if (!on) {
		if (m & tmask::kBlocked) {
			d.blocked--;
			d.solid--;
		}
		m &= 0x3fff;  // also clears GATHER_EDGE (sic)
	} else {
		if (!(m & tmask::kBlocked)) {
			d.blocked++;
			d.solid++;
		}
		m |= tmask::kBlocked;
		set_bad_path(tx, ty, false);
		set_road_at(tx, ty, false);
	}
	// Neighbours: blocking marks them all BAD_PATH (even blocked ones); unblocking clears it where no blocked
	// neighbour remains.
	for (int k = 1; k <= 8; k++) {
		const int32_t x = tx + move_x[k], y = ty + move_y[k];
		if (!in_t(x, y)) continue;
		if (on) set_bad_path(x, y, true);
		else if (!has_blocked_neighbors(x, y)) set_bad_path(x, y, false);
	}
	if (!on && has_blocked_neighbors(tx, ty)) set_bad_path(tx, ty, true);
}

void World::set_road_at(int32_t tx, int32_t ty, bool on, bool quiet) {
	uint16_t& m = t(tx, ty);
	const int32_t wx = tx >> 2, wy = ty >> 2;
	if (on) {
		const uint16_t before = m;
		m = uint16_t((m & ~tmask::kWater) | tmask::kRoad);  // clears only bit 0x20: water and trees both become road
		w(wx, wy).flags |= wflag::kRoad;
		if ((before & tmask::kMedium) != tmask::kRoad && !quiet) roads_road_added(*this, tx, ty);  // Roads::road_added: the road graph
		return;
	}
	if ((m & tmask::kMedium) == tmask::kRoad) {
		m &= uint16_t(~tmask::kMedium);
		if (!quiet) roads_road_cleared(*this, tx, ty);  // Roads::road_cleared (before the wcoord's ROAD flag is re-evaluated)
	}
	// WData ROAD goes when no tcoord of the wcoord is a road (x outer, y inner), even if this one was not.
	for (int32_t x = wx * 4; x < wx * 4 + 4; x++) {
		for (int32_t y = wy * 4; y < wy * 4 + 4; y++) {
			if ((t(x, y) & tmask::kMedium) == tmask::kRoad) return;
		}
	}
	w(wx, wy).flags &= uint16_t(~wflag::kRoad);
}

void World::set_tree_at(int32_t tx, int32_t ty, bool on) {
	uint16_t& m = t(tx, ty);
	WData& d = wdata[w_of_t(tx, ty)];
	if (on) {
		if ((m & tmask::kMedium) != tmask::kTree) d.solid--;
		m |= tmask::kTree | tmask::kPermBehind;
		return;
	}
	if ((m & tmask::kMedium) == tmask::kTree) {
		m &= uint16_t(~tmask::kMedium);
		d.solid++;
	}
	m &= uint16_t(~tmask::kPermBehind);
}

void World::set_mountain_at(int32_t tx, int32_t ty, bool on) {
	set_blocked_at(tx, ty, on);
	uint16_t& m = t(tx, ty);
	if (on) m = uint16_t((m & 0xfffe) | tmask::kMountain);
	else if ((m & tmask::kLowField) == tmask::kMountain) m &= 0xfffc;
}

void World::set_cliff_at(int32_t tx, int32_t ty, bool) {
	set_blocked_at(tx, ty, true);  // the original ignores `on`
	uint16_t& m = t(tx, ty);
	m = uint16_t((m & 0xfffd) | tmask::kCliff);
}

void World::set_building_at(int32_t tx, int32_t ty, bool on) {
	uint16_t& m = t(tx, ty);
	if (on) m |= tmask::kBuilding;
	else if ((m & tmask::kLowField) == tmask::kBuilding) m &= 0xfffc;
}

void World::set_river_at(int32_t tx, int32_t ty, bool on) {
	uint16_t& m = t(tx, ty);
	WData& d = wdata[w_of_t(tx, ty)];
	if (on) {
		m |= tmask::kRiverB;
		d.flags |= wflag::kRiver;
		return;
	}
	m &= uint16_t(~tmask::kRiverB);
	const int32_t wx = tx >> 2, wy = ty >> 2;
	for (int i = 0; i < 16; i++) {
		if (t(traverse_x(wx, i), traverse_y(wy, i)) & tmask::kRiverB) return;
	}
	d.flags &= uint16_t(~wflag::kRiver);
}

void World::set_land(int32_t wx, int32_t wy, int32_t land_index, int32_t sub, uint16_t feature) {
	WData& d = w(wx, wy);
	if (sub >= 0) d.land_sub = uint8_t(sub);
	if (land_index >= 0) d.land = int8_t(land_index);
	d.flags = uint16_t((d.flags & 0xffc3) | feature);
	if (feature == wflag::kCoast) d.flags |= wflag::kOriginalCoast;
}

// ---------------------------------------------------------------- unit occupancy

CollBlock* World::coll_block(int32_t wx, int32_t wy, int32_t region, bool* mismatch) {
	const WData& d = w(wx, wy);
	if (mismatch) *mismatch = false;
	if (region >= 0 && d.region != region) {
		if (mismatch) *mismatch = true;
		return nullptr;
	}
	return d.block >= 0 ? &coll_blocks[size_t(d.block)] : nullptr;
}

CollBlock& World::new_coll_block(int32_t wx, int32_t wy) {
	int32_t slot;
	if (!free_coll_blocks.empty()) {
		slot = free_coll_blocks.back();
		free_coll_blocks.pop_back();
		coll_blocks[size_t(slot)] = CollBlock();
	} else {
		slot = int32_t(coll_blocks.size());
		coll_blocks.emplace_back();
	}
	w(wx, wy).block = slot;
	return coll_blocks[size_t(slot)];
}

void World::free_coll_block(int32_t wx, int32_t wy) {
	WData& d = w(wx, wy);
	if (d.block < 0) return;
	free_coll_blocks.push_back(d.block);
	d.block = -1;
}

bool World::coll_get(int32_t ux, int32_t uy) const {
	if (!in_u(ux, uy)) return false;
	const WData& d = w(ux >> 4, uy >> 4);
	return d.block >= 0 && coll_blocks[size_t(d.block)].get(ux, uy);
}

void World::coll_set(int32_t ux, int32_t uy, bool on) {
	if (!in_u(ux, uy)) return;
	const int32_t wx = ux >> 4, wy = uy >> 4;
	const int32_t b = w(wx, wy).block;
	if (b >= 0) coll_blocks[size_t(b)].set(ux, uy, on);
	else if (on) new_coll_block(wx, wy).set(ux, uy, true);
}

int32_t World::stamp_footprint(int32_t ux, int32_t uy, int32_t r, int32_t region, bool on) {
	if (r <= 0) return 0;
	const int32_t n = kMoveRadius[std::min(r, 10)];
	int32_t changed = 0;
	for (int32_t k = 0; k < n; k++) {
		const int32_t x = ux + move_x[k], y = uy + move_y[k];
		if (!in_u(x, y)) continue;
		const int32_t wx = x >> 4, wy = y >> 4;
		if (region >= 0 && w(wx, wy).region != region) continue;
		if (coll_get(x, y) != on) changed++;
		coll_set(x, y, on);
	}
	return changed;
}

void World::process_coll_blocks() {
	if (size <= 0) return;
	int32_t n = std::max(xs / 4, 5);
	while (n-- > 0) {
		WData& d = wdata[size_t(empty_colls)];
		if (d.block >= 0 && coll_blocks[size_t(d.block)].empty()) {
			free_coll_blocks.push_back(d.block);
			d.block = -1;
		}
		if (++empty_colls >= size) empty_colls = 0;
	}
}

}  // namespace ron::sim
