// Objects on the world grid (docs/world-spec.md sections 3.5, 6, 7): goods, items, building footprints (snap,
// foundation, BuildType::mask_me, place_roads, behind tiles) and unit footprints in the CollBlock bitmaps.
#pragma once

#include <array>
#include <cstdint>

#include "sim/rules.hpp"
#include "sim/world.hpp"

namespace ron::sim {

// TypeIndex values these rules test with ObjectTypeData::is.
constexpr int kOilGood = 5, kVillage = 414, kFarm = 417, kUniversity = 420, kOilWell = 421, kOilPlatform = 422,
              kDock = 432;
constexpr uint32_t kBuildFlagE = 0x10, kBuildFlagG = 0x40;  // BUILD_FLAGS 'e' and 'g' (gather type)

// ObjectTypeData::is @0065f7d0 on finalised rules (start_game built the is-lists): the type itself or its (strict)
// IS list contains t.
bool type_is(const Type& type, int t, bool strict = false);
// BuildTypeData::connects_to_roads @006398c0: not 'g' or 'e', unless a university.
bool connects_to_roads(const Type& bt);
inline bool is_city_type(const Type& bt) { return type_is(bt, kVillage); }

// A building's footprint in tcoords (BuildTypeData::tile_corner @00636440 of a snapped centre).
struct Footprint {
	int32_t x0 = 0, y0 = 0, w = 0, h = 0;
};
Footprint footprint(const Type& bt, int32_t cx, int32_t cy);

// BuildTypeData::snap_center @00636190, the scenario-load subset: oil wells/platforms move to the centre of an OIL
// wcoord, then each axis snaps to the tcoord grid (+96 for odd sizes). The dock / mine / woodcutter search through
// blocked_site is not reproduced (it cannot succeed for mines and woodcutters while nothing is explored).
std::array<int32_t, 2> snap_center(const World& w, const Type& bt, int32_t cx, int32_t cy);

// Wall::start_me @0063d790 with 1: every in-map footprint tcoord (x outer, y inner) gets STARTED, or STARTED2 when
// STARTED is already set.
void start_me(World& w, const Type& bt, int32_t cx, int32_t cy);

// RegenRoads of mask_me / place_roads. FORCE paths a road to the city (PathFinder::find_road) and TOTAL flags the
// city for that; neither is reproduced here (the caller owns cities and the pathfinder).
enum class Regen : uint8_t { None, Simple, Force, Total };

// BuildType::mask_me @006312a0 (add or remove). `city_of_building`: the building belongs to a city (Build +0x72 >= 0),
// which gives 'e' buildings their outer road ring. Not reproduced: Wall::mask_city (CITY bits; needs the city radius)
// and Docks::remask_docks on removal.
void mask_me(World& w, const Type& bt, int32_t cx, int32_t cy, bool add, Regen regen, bool city_of_building = false);
// BuildType::place_roads @0063c580 (the ring roads; see mask_me).
void place_roads(World& w, const Type& bt, int32_t cx, int32_t cy, bool add, bool city_of_building = false);
// Wall::mark_behind_tiles @0063d230: TEMP_BEHIND on the diagonal band north-west of the footprint.
void mark_behind_tiles(World& w, const Type& bt, int32_t cx, int32_t cy, bool on);
// The grid part of Wall::start @0063e810 (also reached from Build::activate for scenario buildings): mask_me(1,
// REGEN_FORCE), was_seen and seen2 bits for `who` (the footprint is explored), mark_behind_tiles(1).
// kill_competing_buildings, check_ever_seen and the forced road are the caller's.
void wall_start(World& w, const Type& bt, int32_t cx, int32_t cy, int32_t who, void (*after_mask)(void*) = nullptr, void* ctx = nullptr,
                bool city_of_building = false);
// (`after_mask(ctx)` runs between mask_me(1, REGEN_FORCE) and the fog marks: the forced road to the city is laid there, because
// calc_road_cost reads the footprint's was_seen bits before Wall::start sets them.)

// Good::init @0066da20: RESOURCE on the x_size * y_size tcoords around the centre (x outer, y inner, no bounds check
// in the original), BLOCKED too when the type blocks, FLAG_RESOURCE on the centre wcoord. Oil marks nothing.
// Returns the number of tcoords outside the map (skipped here; the original writes out of bounds).
int32_t good_init(World& w, int type_index, const Type& gt, int32_t cx, int32_t cy);
// Item::init @006770e0: FLAG_GOODY on the item's wcoord.
void item_init(World& w, int32_t cx, int32_t cy);

// Object::add_to_world / remove_from_world for one guy at (cx, cy) of a unit type: the new_block_radius footprint,
// filtered to the guy tcoord's get_tregion; nothing for air units or radius 0. Returns the bits changed.
int32_t unit_footprint(World& w, const Type& ut, int32_t cx, int32_t cy, bool on);

}  // namespace ron::sim
