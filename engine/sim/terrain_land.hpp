// Terrain::object_placed @850c40 / spot_update_land @84fa50 / verify_land @84d8a0 / TileSet::is_combo_acceptable @87abe0: the land sub-type
// (WData.land_sub, hashed by the `world` checksum) of the plain-land wcoords under and around a building that starts.
#pragma once

#include <cstdint>
#include <string>

#include "assets/files.hpp"

namespace ron::sim {

class Game;

// What the tileset (Data/tilesets.xml <TILESET name=...>) contributes: `weatherbase` (the land_sub of a wcoord a building covers),
// TERRAINGROUP/COAST_BASE and the BASE counts of the land classes (TileSet::num_tiles: BASELAND, COASTAL, OCEAN).
struct TileLandInfo {
	bool known = false;
	int32_t weatherbase = 0;
	int32_t coast_base = -1;
	int32_t num_tiles[3] = {4, 1, 1};
};

TileLandInfo load_tile_land_info(const GameFiles& files, const std::string& tileset);

// Terrain::object_placed(corner, size, 1): called by Wall::start with the footprint's corner tcoord and size. A game that does not know
// its tileset (anything but a game loaded from an original save) skips it.
void terrain_object_placed(Game& g, int32_t tile_x, int32_t tile_y, int32_t xsize, int32_t ysize);

}  // namespace ron::sim
