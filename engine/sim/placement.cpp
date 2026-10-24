#include "sim/placement.hpp"

#include <algorithm>

namespace ron::sim {

bool type_is(const Type& type, int t, bool strict) {
	if (type.type == t) return true;
	const U16Array& list = strict ? type.is_strict_list : type.is_list;
	return t >= 0 && std::find(list.v.begin(), list.v.end(), uint16_t(t)) != list.v.end();
}

bool connects_to_roads(const Type& bt) { return !((bt.build_flags & (kBuildFlagE | kBuildFlagG)) && !type_is(bt, kUniversity)); }

Footprint footprint(const Type& bt, int32_t cx, int32_t cy) {
	return {tcoord_of(cx) - (bt.x_size >> 1), tcoord_of(cy) - (bt.y_size >> 1), bt.x_size, bt.y_size};
}

std::array<int32_t, 2> snap_center(const World& w, const Type& bt, int32_t cx, int32_t cy) {
	if (type_is(bt, kOilWell) || type_is(bt, kOilPlatform)) {
		const int32_t wx = wcoord_of(cx), wy = wcoord_of(cy);
		if (w.in_w(wx, wy) && (w.w(wx, wy).flags & wflag::kOil)) {
			cx = wcoord_centre(wx);
			cy = wcoord_centre(wy);
		}
	}
	return {tcoord_of(cx) * kTCoordUnits + ((bt.x_size & 1) ? kTCoordUnits / 2 : 0),
	        tcoord_of(cy) * kTCoordUnits + ((bt.y_size & 1) ? kTCoordUnits / 2 : 0)};
}

void start_me(World& w, const Type& bt, int32_t cx, int32_t cy) {
	const Footprint f = footprint(bt, cx, cy);
	for (int32_t x = f.x0; x < f.x0 + f.w; x++) {
		for (int32_t y = f.y0; y < f.y0 + f.h; y++) {
			if (!w.in_t(x, y)) continue;
			uint16_t& m = w.t(x, y);
			m |= (m & tmask::kStarted) ? tmask::kStarted2 : tmask::kStarted;
		}
	}
}

void mask_me(World& w, const Type& bt, int32_t cx, int32_t cy, bool add, Regen regen, bool city_of_building) {
	// The original reports "Building does not have a valid mask" and stops; masks.txt covers every stock building.
	if (!bt.mask.present) return;
	const Footprint f = footprint(bt, cx, cy);
	const bool city = is_city_type(bt), conn = connects_to_roads(bt);
	WData& centre = w.w(wcoord_of(cx), wcoord_of(cy));
	if (add) centre.flags |= wflag::kBuilding;
	else centre.flags &= uint16_t(~wflag::kBuilding);
	for (int32_t x = 0; x < f.w; x++) {
		for (int32_t y = 0; y < f.h; y++) {
			const int32_t tx = f.x0 + x, ty = f.y0 + y;
			if (!w.in_t(tx, ty)) continue;
			uint16_t& m = w.t(tx, ty);
			m &= uint16_t(~(tmask::kStarted | tmask::kStarted2));
			if (add) {
				m |= tmask::kBuilding;
				if (!city && !conn) w.set_road_at(tx, ty, false);
			} else if ((m & tmask::kLowField) == tmask::kBuilding) {
				m &= uint16_t(~tmask::kLowField);
			}
			const size_t k = size_t(f.w) * size_t(y) + size_t(x);
			const uint8_t v = k < bt.mask.data.size() ? bt.mask.data[k] : 0;
			if (v == 1) {
				w.set_blocked_at(tx, ty, add);
			} else if (add) {
				w.set_blocked_at(tx, ty, false);
				if (city || conn) w.set_road_at(tx, ty, true);
			}
		}
	}
	// Docks: water near the dock is BAD_PATH (x outer).
	if (type_is(bt, kDock)) {
		for (int32_t x = f.x0 - 3; x < f.x0 + f.w + 3; x++) {
			for (int32_t y = f.y0 - 3; y < f.y0 + f.h + 3; y++) {
				if (w.in_t(x, y) && w.is_tocean(x, y)) w.set_bad_path(x, y, add);
			}
		}
	}
	if (add && regen == Regen::None) return;
	place_roads(w, bt, cx, cy, add, city_of_building);
}

void place_roads(World& w, const Type& bt, int32_t cx, int32_t cy, bool add, bool city_of_building) {
	const Footprint f = footprint(bt, cx, cy);
	int32_t left, top, right, bottom;
	if (is_city_type(bt) || connects_to_roads(bt)) {
		// The box's right and bottom sides are the footprint's own last column and row.
		left = f.x0 - 1, top = f.y0 - 1, right = f.x0 - 1 + f.w, bottom = f.y0 - 1 + f.h;
	} else if (!(bt.build_flags & kBuildFlagG) && city_of_building) {
		left = f.x0 - 1, top = f.y0 - 1, right = f.x0 + f.w, bottom = f.y0 + f.h;  // just outside the footprint
	} else {
		return;
	}
	for (int32_t x = left; x <= right; x++) {
		for (int32_t y = top; y <= bottom; y++) {
			if (!(x == left || x == right || y == top || y == bottom) || !w.in_t(x, y)) continue;
			const uint16_t m = w.t(x, y);
			if (m & tmask::kBlocked) continue;
			if ((m & tmask::kMedium) == tmask::kWater) continue;
			const bool built = (m & tmask::kLowField) == tmask::kBuilding || (m & tmask::kStarted);
			if (!built || (x != left && y != top)) w.set_road_at(x, y, add);
		}
	}
}

void mark_behind_tiles(World& w, const Type& bt, int32_t cx, int32_t cy, bool on) {
	const Footprint f = footprint(bt, cx, cy);
	auto mark = [&](int32_t x, int32_t y) {
		if (w.in_t(x, y)) w.set_mask_bit(x, y, tmask::kTempBehind, on);
	};
	for (int32_t k = 1; k <= bt.behind_height; k++) {
		for (int32_t x = f.x0 - k; x < f.x0 - k + f.w; x++) mark(x, f.y0 - k);
		for (int32_t y = f.y0 - k + 1; y < f.y0 - k + f.h; y++) mark(f.x0 - k, y);
	}
}

void wall_start(World& w, const Type& bt, int32_t cx, int32_t cy, int32_t who, void (*after_mask)(void*), void* ctx) {
	mask_me(w, bt, cx, cy, true, Regen::Force);
	if (after_mask) after_mask(ctx);
	const Footprint f = footprint(bt, cx, cy);
	const uint8_t bit = uint8_t(1u << (uint32_t(who) & 31));  // x86 shl: who 8 or -1 sets nothing in the byte
	for (int32_t x = f.x0; x < f.x0 + f.w; x++) {
		for (int32_t y = f.y0; y < f.y0 + f.h; y++) {
			if (!w.in_t(x, y)) continue;
			// The footprint is explored for the owner: WData.was_seen and seen2 of its fog cell (the original skips a
			// fog cell equal to the previous one; the ORs are idempotent).
			w.wdata[w.w_of_t(x, y)].was_seen |= bit;
			if (!w.seen2.empty()) w.seen2[size_t(y >> 1) * size_t(w.fog_xs) + size_t(x >> 1)] |= bit;
		}
	}
	// (Wall::start calls check_ever_seen(0) here: the object part, see objects.cpp build_activate.)
	mark_behind_tiles(w, bt, cx, cy, true);
}

int32_t good_init(World& w, int type_index, const Type& gt, int32_t cx, int32_t cy) {
	if (type_index == kOilGood) return 0;  // oil is the wcoord OIL flag
	int32_t outside = 0;
	const int32_t x0 = tcoord_of(cx) - gt.x_size / 2, y0 = tcoord_of(cy) - gt.y_size / 2;
	for (int32_t x = x0; x < x0 + gt.x_size; x++) {
		for (int32_t y = y0; y < y0 + gt.y_size; y++) {
			if (!w.in_t(x, y)) {
				outside++;
				continue;
			}
			w.t(x, y) |= tmask::kResource;
			if (gt.block) w.set_blocked_at(x, y, true);
		}
	}
	const int32_t wx = wcoord_of(cx), wy = wcoord_of(cy);
	if (w.in_w(wx, wy)) w.w(wx, wy).flags |= wflag::kResource;
	return outside;
}

void item_init(World& w, int32_t cx, int32_t cy) {
	const int32_t wx = wcoord_of(cx), wy = wcoord_of(cy);
	if (w.in_w(wx, wy)) w.w(wx, wy).flags |= wflag::kGoody;
}

int32_t unit_footprint(World& w, const Type& ut, int32_t cx, int32_t cy, bool on) {
	const int32_t r = ut.new_block_radius;
	if (r == 0 || ut.domain == 2) return 0;
	const int32_t ux = ucoord_of(cx), uy = ucoord_of(cy);
	const int32_t tx = tcoord_of(cx), ty = tcoord_of(cy);
	const int32_t region = w.in_t(tx, ty) ? w.get_tregion(tx, ty) : -1;
	return w.stamp_footprint(ux, uy, r, region, on);
}

}  // namespace ron::sim
