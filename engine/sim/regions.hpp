// Connectivity regions (regions.obj, docs/world-spec.md section 5): landmasses and water bodies over wcoords, written
// into WData.region / region2. 128 records: 0..63 land (0 unused, 0x3f overflow), 0x40..0x7f sea (0x40 unused, 0x7e
// overflow). Regions only change at map creation and scenario import.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sim/world.hpp"

namespace ron::sim {

// Region flags (PDB enum REGION_MERGE).
namespace region_flag {
constexpr int32_t kMerge = 1, kNoRare = 2, kPlayer = 4, kResource = 8, kGatherRecompute = 0x10, kGatherVerify = 0x20,
                  kRecalc = 0x40, kAnybodyHere = 0x80;
}

struct Region {
	int32_t region = 0;  // own index
	int32_t size = 0;    // wcoords
	int32_t rank = 0;    // position by descending size within land / sea
	int32_t flags = 0;
	int32_t borders = 0;  // +0x2c: territory recompute cursor (Region::fix_borders resets it, production-spec 5.10)
	int32_t climate = 0, fertile = 0;
	int32_t common_factor = 8, goody_factor = 8;
	uint64_t coastal = 0;  // BitMask<64> over land regions
	uint64_t coast = 0;    // BitMask<64>: bit k = sea region 0x40 + k
	// Region +0x3c / +0x40 (not walked by Region::walk_data, so a loaded game starts with both clear): Unit::think_scout marks the region as
	// scouted by a leader when it finds nothing left to explore there (bit `who` of `scouted`, `scout_mark` reset), and the ferry search
	// of Unit::think_civilian_transport skips the regions a leader has scouted (sim/ferry.cpp).
	uint8_t scouted = 0;
	int32_t scout_mark = 0;
	std::vector<WCoord> coords;  // raster order (rebuild_coords)
};

class Regions {
public:
	static constexpr int32_t kNum = 128, kFirstSea = 0x40, kLandOverflow = 0x3f, kSeaOverflow = 0x7e;
	Regions();

	std::array<Region, kNum> r;
	int32_t sea = kFirstSea;  // sea id counter
	int32_t land = 0;         // always 0

	// Regions::clear_all @00680060: every region loses flags/climate (and size/coords when non-empty); every
	// WData.region = 0 (region2 is kept); counters reset.
	void clear_all(World& w);
	// Regions::find_all @0067eff0 + set_coastals, sort_regions, rebuild_coords. Throws std::runtime_error on the
	// original's "Region Size mismatch" assert.
	void find_all(World& w);
	// Region::release @00680560 for land regions flagged RECALC (adjust_for_blocking step 2; no writer known).
	void release_recalc(World& w);

	// Region::is_coast @00680f90: same region -> true; a land/sea pair -> the land region's coast bit for the sea.
	bool is_coast(int32_t a, int32_t b) const;
	int32_t get_num_sea() const;  // non-empty sea regions

private:
	void find(World& w, int32_t x, int32_t y, int32_t id);
	void set_coastals(const World& w);
	void finalize_coastals(int32_t me);
	void sort_regions();
	void rebuild_coords(const World& w);
};

}  // namespace ron::sim
