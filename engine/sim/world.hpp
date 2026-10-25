// The simulation world grid (docs/world-spec.md): World / WorldData (world.obj), per-wcoord WData, per-tcoord TData
// masks, the primitive writers every system goes through (set_blocked_at, set_road_at, ...), the read predicates, and
// unit occupancy (CollBlock bitmaps per wcoord). Building the grid from a scenario is in sim/world_load.hpp, regions in
// sim/regions.hpp, buildings and units in sim/placement.hpp, the passability predicate in sim/passability.hpp.
//
// Everything here is saved and checksummed (World::walk_data), so every bit and counter must match the original.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sim/compass.hpp"

namespace ron::sim {

// WData.flags (PDB unnamed enum FLAG_GOODY)
namespace wflag {
constexpr uint16_t kResource = 0x1, kDeadBuild = 0x2, kRiverValley = 0x2, kCoast = 0x4, kRock = 0x8, kMountain = 0x10,
                   kForest = 0x20, kCliff = 0x40, kRoad = 0x80, kHalfLand = 0x100, kNearBlocking = 0x200,
                   kOriginalCoast = 0x400, kOil = 0x800, kRiver = 0x1000, kRiverbed = 0x2000, kBuilding = 0x4000,
                   kGoody = 0x8000;
constexpr uint16_t kImpassable = kMountain | kForest | kCliff;  // 0x70: is_passable / map_is_passable
}  // namespace wflag

// TData mask (PDB unnamed enum MASK_GATHER_EDGE). Bits 0-1 and 4-5 are two-bit fields.
namespace tmask {
constexpr uint16_t kLowField = 0x3, kCliff = 0x1, kMountain = 0x2, kBuilding = 0x3;  // mask & 3
constexpr uint16_t kTempBehind = 0x4, kPermBehind = 0x8;
constexpr uint16_t kMedium = 0x30, kRoad = 0x10, kWater = 0x20, kTree = 0x30;  // mask & 0x30
constexpr uint16_t kStarted2 = 0x40, kStarted = 0x80, kCity = 0x100, kResource = 0x200, kCoastal = 0x400,
                   kRiverB = 0x800, kGathered = 0x1000, kBadPath = 0x2000, kBlocked = 0x4000, kGatherEdge = 0x8000;
}  // namespace tmask

// LandIndex values of WData.land, and the gather-land indices of WorldData::get_land(w, 1).
namespace land {
constexpr int8_t kLand = 0, kSandy = 1, kOcean = 2, kCoast = 3, kForest = 4, kMountains = 5, kRocks = 6, kOil = 7,
                 kCliffs = 8;
constexpr int kNumLands = 9;
}  // namespace land

// One wcoord (0x1c bytes in the original; field order and widths as walked). Defaults: WData::WData @006b2310.
struct WData {
	uint16_t flags = 0;
	int8_t land = land::kOcean;
	uint8_t land_sub = 0;
	int16_t region = 0;
	int16_t region2 = 0;  // sea region of a half-water wcoord's water tcoords
	int16_t down = -1, down_who = 0;  // head of the per-wcoord object list
	uint8_t val = 0;    // AI site value (compute_all_vals)
	uint8_t goods = 0;  // nearby-resource bits (compute_all_vals)
	uint8_t light = 0;
	int8_t who = -1, who2 = -1;  // territory
	uint8_t blocked = 0;  // BLOCKED tcoords in this wcoord
	uint8_t bad = 0;      // BAD_PATH tcoords
	int8_t solid = 0;     // BLOCKED tcoords that are not trees
	uint8_t was_seen = 0;
	int32_t block = -1;   // CollBlock: index into World::coll_blocks, -1 = none (the original's null pointer)
};

// Unit occupancy of one wcoord: BitMask<768> (bits, size 0x60, flags). Only bits 0..255 are used: bit
// (ux mod 16) * 16 + (uy mod 16), x-major, C remainder. All 96 bytes are saved.
struct CollBlock {
	static constexpr int32_t kBits = 0x300, kBytes = 0x60;
	int32_t flags = 1;  // 1 known empty, 0 set since the last test, 2 dirty (BitMask<768>::empty caches here)
	std::array<uint8_t, kBytes> bytes{};

	// CollBlock::get @00681e30 / set @00682070
	static int32_t bit_index(int32_t ux, int32_t uy) { return (ux % 16) * 16 + (uy % 16); }
	bool get(int32_t ux, int32_t uy) const {
		const int32_t i = bit_index(ux, uy);
		return (bytes[size_t(i >> 3)] & (1u << (i & 7))) != 0;
	}
	void set(int32_t ux, int32_t uy, bool on) {
		const int32_t i = bit_index(ux, uy);
		const uint8_t b = uint8_t(1u << (i & 7));
		if (on) {
			bytes[size_t(i >> 3)] |= b;
			flags = 0;
		} else {
			bytes[size_t(i >> 3)] &= uint8_t(~b);
			if (flags == 0) flags = 2;
		}
	}
	// BitMask<768>::empty @00479150: a read that refreshes the cached flags.
	bool empty();
};

// The road candidates of the original (CoordInfo.roads_in_wcoord + num_roads, saved by Terrain::walk_roads) and the Roads sweep cursor.
// Slab storage so that the state is plain integers (saved natively, loaded from an original save): `slot[wcoord]` is the cell's index in
// `words` or -1 (no roads_in_wcoord array); a cell is kWords words: [0] = num_roads, then 16 candidates of 4 words each:
// { element_num, flags, rotation | unknown << 8 | ref_a << 16 | ref_b << 24, support_codes | unknown << 16 | is_terrain_creation << 24 }
// (RoadElementCandidate, 16 bytes; sim/roadgraph.hpp). Freed cells are zeroed and reused LIFO.
struct RoadStore {
	static constexpr int32_t kWords = 65;
	std::vector<int32_t> slot;
	std::vector<uint32_t> words;
	std::vector<int32_t> free_slots;
	int32_t scan_x = 0, scan_y = 0;  // Roads +0x5fc / +0x600: the wcoord Roads::scan_and_kill_stray_roads processed last
	std::array<int32_t, 9> scan_road{};  // Roads +0x604: the scan's "tile is a road" cache (an off-map neighbour leaves the previous value)
};

class World {
public:
	// World::init @006b76f0 (after World::close): sizes, default WData, zeroed TData, fog and danger.
	void init(int32_t xs, int32_t ys);

	int32_t xs = 0, ys = 0, size = 0;
	int32_t tile_xs = 0, tile_ys = 0, tile_size = 0;
	int32_t fog_xs = 0, fog_ys = 0, fog_size = 0;
	int32_t reg_xs = 0, reg_ys = 0, reg_size = 0;
	int32_t sea_map = -1;
	std::vector<WData> wdata;      // size, row-major y * xs + x
	std::vector<uint16_t> tdata;   // tile_size, row-major ty * tile_xs + tx
	std::vector<uint8_t> seen, seen2, seen3;  // fog_size each, per-player bits (really seen / was seen / detected)
	std::vector<uint8_t> wcoord_seen;         // size
	// Bumped whenever a fog byte (seen / seen2) changes or the seen grid is cleared: a change counter for renderers
	// (the original's WorldMapFog::add_update queue). Not simulation state (never saved or checksummed).
	uint32_t fog_changes = 0;
	std::vector<int32_t> danger;              // 8 * reg_size, [player * reg_size + cell]

	// ---- indexing
	bool in_w(int32_t wx, int32_t wy) const { return wx >= 0 && wy >= 0 && wx < xs && wy < ys; }
	bool in_t(int32_t tx, int32_t ty) const { return tx >= 0 && ty >= 0 && tx < tile_xs && ty < tile_ys; }
	bool in_u(int32_t ux, int32_t uy) const { return ux >= 0 && uy >= 0 && ux < xs * 16 && uy < ys * 16; }
	size_t wi(int32_t wx, int32_t wy) const { return size_t(wy) * size_t(xs) + size_t(wx); }
	size_t ti(int32_t tx, int32_t ty) const { return size_t(ty) * size_t(tile_xs) + size_t(tx); }
	size_t w_of_t(int32_t tx, int32_t ty) const { return wi(tx >> 2, ty >> 2); }
	WData& w(int32_t wx, int32_t wy) { return wdata[wi(wx, wy)]; }
	const WData& w(int32_t wx, int32_t wy) const { return wdata[wi(wx, wy)]; }
	uint16_t& t(int32_t tx, int32_t ty) { return tdata[ti(tx, ty)]; }
	uint16_t t(int32_t tx, int32_t ty) const { return tdata[ti(tx, ty)]; }

	// ---- predicates (WorldData::*)
	// is_ocean @006b4830: open water (land 1/2 and not half water).
	static bool is_ocean(const WData& d) {
		return (d.flags & wflag::kHalfLand) == 0 && (d.land == land::kSandy || d.land == land::kOcean);
	}
	bool is_ocean(int32_t wx, int32_t wy) const { return is_ocean(w(wx, wy)); }
	bool is_tocean(int32_t tx, int32_t ty) const { return (t(tx, ty) & tmask::kMedium) == tmask::kWater; }
	bool is_tree_at(int32_t tx, int32_t ty) const { return (t(tx, ty) & tmask::kMedium) == tmask::kTree; }
	bool is_road_at(int32_t tx, int32_t ty) const { return (t(tx, ty) & tmask::kMedium) == tmask::kRoad; }
	bool is_mountain_at(int32_t tx, int32_t ty) const { return (t(tx, ty) & tmask::kLowField) == tmask::kMountain; }
	bool is_cliff_at(int32_t tx, int32_t ty) const { return (t(tx, ty) & tmask::kLowField) == tmask::kCliff; }
	bool is_built_at(int32_t tx, int32_t ty) const {
		const uint16_t m = t(tx, ty);
		return (m & tmask::kLowField) == tmask::kBuilding || (m & tmask::kStarted);
	}
	// is_blocked_at @00461340: BLOCKED; with `iroquois` (forest walkers) trees do not count.
	bool is_blocked_at(int32_t tx, int32_t ty, bool iroquois = false) const {
		const uint16_t m = t(tx, ty);
		return (m & tmask::kBlocked) && !(iroquois && (m & tmask::kMedium) == tmask::kTree);
	}
	bool is_river(int32_t tx, int32_t ty) const { return (t(tx, ty) & tmask::kRiverB) != 0; }
	bool is_gathered_from(int32_t tx, int32_t ty) const { return (t(tx, ty) & tmask::kGathered) != 0; }
	// map_is_passable / is_passable: the wcoord has no mountain, forest or cliff.
	bool is_passable_wcoord(int32_t wx, int32_t wy) const { return (w(wx, wy).flags & wflag::kImpassable) == 0; }
	bool buildings_allowed(int32_t wx, int32_t wy) const { return (w(wx, wy).flags & 0x78) == 0; }
	// get_land(w, 1) @006b4730: the gather-land index (rules <LANDS>) of a wcoord.
	int32_t gather_land(int32_t wx, int32_t wy) const;
	// has_blocked_neighbors @006b2990: any in-map neighbour (move 1..8) BLOCKED.
	bool has_blocked_neighbors(int32_t tx, int32_t ty) const;
	// has_gather_access(t, -1, 0, 1) @006b4e50: a tree, mountain or cliff tcoord with an in-map orthogonal neighbour
	// that is neither water nor BLOCKED.
	bool has_gather_access(int32_t tx, int32_t ty) const;
	// get_tregion @006b52e0: region2 for the water tcoords of a half-water wcoord, else region.
	int32_t get_tregion(int32_t tx, int32_t ty) const;
	// World::is_edge_of_region @006b3180: an in-map 8-neighbour in another region.
	bool is_edge_of_region(int32_t wx, int32_t wy) const;

	// ---- primitive writers (World::set_*). Keep every caller's loop order: the BAD_PATH result is order dependent.
	void set_bad_path(int32_t tx, int32_t ty, bool on);   // @006b4610: bit + WData.bad
	void set_blocked_at(int32_t tx, int32_t ty, bool on); // @006b4900
	// @006b43b0. `quiet` (param_5) skips Roads::road_added / road_cleared, the road graph (sim/roadgraph.hpp): only the Roads code itself
	// passes it, after doing its own bookkeeping.
	void set_road_at(int32_t tx, int32_t ty, bool on, bool quiet = false);
	void set_tree_at(int32_t tx, int32_t ty, bool on);    // @006b2060 (blocking is separate)
	void set_mountain_at(int32_t tx, int32_t ty, bool on);  // @006b1f00
	void set_cliff_at(int32_t tx, int32_t ty, bool on);     // @006b1eb0
	void set_building_at(int32_t tx, int32_t ty, bool on);  // @006b45c0: |= 3 / clear when 3
	void set_mask_bit(int32_t tx, int32_t ty, uint16_t bit, bool on) {  // city, started, started2, resource, ...
		uint16_t& m = t(tx, ty);
		m = on ? uint16_t(m | bit) : uint16_t(m & ~bit);
	}
	// set_river_at: 0x800 on the tcoord, RIVER on the wcoord; off clears RIVER when no tcoord of the wcoord keeps it.
	void set_river_at(int32_t tx, int32_t ty, bool on);
	// set_land(w, land, sub, feature) @006b2b50: flags = (flags & ~0x3c) | feature; feature 4 also ORIGINAL_COAST.
	void set_land(int32_t wx, int32_t wy, int32_t land_index, int32_t sub, uint16_t feature);

	// ---- unit occupancy (CollBlock per wcoord, docs/world-spec.md section 7)
	std::vector<CollBlock> coll_blocks;   // pool; WData.block indexes it
	std::vector<int32_t> free_coll_blocks;  // released pool slots (reused LIFO; slot identity is not observable)
	int32_t empty_colls = 0;  // GameDaemon.empty_colls: process_coll_blocks cursor (saved with the GameDaemon)
	RoadStore roads;          // the road candidates (sim/roadgraph.hpp)

	// get_coll_block @006b5350: the wcoord's block, nullptr for none; `region` >= 0 and a different WData.region
	// gives *mismatch = true (the original's (CollBlock*)-1).
	CollBlock* coll_block(int32_t wx, int32_t wy, int32_t region = -1, bool* mismatch = nullptr);
	CollBlock& new_coll_block(int32_t wx, int32_t wy);  // World::new_coll_block @0046d250 (zeroed, flags 1)
	void free_coll_block(int32_t wx, int32_t wy);
	// The bit of ucoord (ux, uy); false off the map or without a block.
	bool coll_get(int32_t ux, int32_t uy) const;
	// One ucoord: setting creates the block if needed, clearing needs an existing block. Off map: nothing.
	void coll_set(int32_t ux, int32_t uy, bool on);
	// A unit footprint (Object::add_to_world / remove_from_world): the (2r+1)^2 ucoords around (ux, uy) in move
	// spiral order, skipping off-map cells and wcoords whose region differs from `region` (the guy's get_tregion;
	// -1 = no filter). r = new_block_radius (0 = nothing). Returns the number of bits that changed.
	int32_t stamp_footprint(int32_t ux, int32_t uy, int32_t r, int32_t region, bool on);
	// GameDaemon::process_coll_blocks @00731f90, once per frame: max(trunc(xs/4), 5) wcoords from empty_colls
	// (wrapping at size); frees blocks that are known or found empty.
	void process_coll_blocks();
};

}  // namespace ron::sim
