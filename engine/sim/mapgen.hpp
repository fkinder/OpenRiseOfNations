// The random-map generator: Setup::build_game steps 3-4-7-9-10 (docs/randommap-spec.md, randommap-terrain-groups.md,
// randommap-resources.md, randommap-rivers.md). Given a map style, a size, a seed and the nations, it fills a World
// (land / terrain / tcoords, WData flags, regions, resources and goodies, rivers) and produces the starting-empire
// placement list of Setup::build_empire. Every game_random draw is made in the original's order and counted.
//
// Nothing here touches the per-frame simulation; a caller that owns a Game copies the World, Regions and the
// goods / items arrays into it (see apps/mapgen/main.cpp for the standalone use).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/tga.hpp"
#include "sim/objects.hpp"
#include "sim/random.hpp"
#include "sim/regions.hpp"
#include "sim/rules.hpp"
#include "sim/world.hpp"
#include "sim/world_load.hpp"

namespace ron::sim {
class Game;
}

namespace ron::sim::mapgen {

// MapStyleIndex (docs/randommap-spec.md section 2): 0..5 random pickers, 6..22 the shipped styles, 23 BLANK_MAP.
constexpr int kStyleOldWorld = 6, kStyleSahara = 7, kStyleAmazon = 8, kStyleHimalayas = 9, kStyleSouthwestMesa = 10,
              kStyleAfrica = 11, kStyleMediterranean = 12, kStyleOutback = 13, kStyleGreatLakes = 14,
              kStyleWarringStates = 15, kStyleNewWorld = 16, kStyleColonialPowers = 17, kStyleEastIndies = 18,
              kStyleEastMeetsWest = 19, kStyleSeaPower = 20, kStyleNileDelta = 21, kStyleBritishIsles = 22,
              kStyleBlank = 23, kStyleConquest = 24, kStyleConquestSea = 25, kStyleScenario = 26;
constexpr int kNumStyleNames = 27;

// ---------------------------------------------------------------- Conquer the World battle maps (styles 24 / 25, mapgen_conquest.cpp)
// MapConquest::make_continents @68dba0 turns the silhouettes of the two countries (and their surroundings) into the land grid. The
// inputs live in the CtW layer: the CtW battle builder (sim/ctw_battle.cpp) fills a ConquestInput and keeps it alive with a
// ConquestScope around new_random_game; the generator picks it up (a scope also upgrades the random-map styles 1 / 2 the
// RandomGameSetup carries to 24 / 25, because RandomGameSetup clamps its style to the shipped ones).
struct ConquestRect {  // RectTemplateBase<float>: left, right, top, bottom
	float left = 0, right = 0, top = 0, bottom = 0;
};
// ConquestCountries::get_scenario_map_rect @75c3b0, the texture-pixel branch (param_5 = 0), two different countries: the square drawn
// around them. (x1, y1) = starting_coords of `param_1` (the attacker's node), (x2, y2) of `param_2` (the defended node).
ConquestRect conquest_scenario_rect(int x1, int y1, int x2, int y2, int tex_width, int map_style);

struct ConquestCountry {  // one ConquestCountries entry
	int left = 0, right = 0, top = 0, bottom = 0;  // the COUNTRY attributes (texture pixels; the piece image covers right-left+1 x bottom-top+1)
	int cx = 0, cy = 0;                            // starting_coords_x / y (the COUNTRY x / y)
};
struct ConquestSupporter {
	bool near_second = false;  // allied to the defended node's owner: its start goes around the second start (else the first)
	int armies = 0;            // start_add_armies
};
struct ConquestInput {
	bool valid = false;
	bool sea = false;                    // MapConquestSea (style 25: CONQUESTMAPSEA.xml)
	// ConquestCountries::draw_scenario_area @761310: the square image (1 = land, 0 = water) and the countries whose centre is in it
	int side = 0;
	std::vector<uint8_t> land;           // side * side
	std::vector<int> ids, xs, ys;        // country index, centre in image pixels
	int node_a = -1, node_b = -1;        // the defended node (link.to) and the attacker's (link.from)
	int who_b = 0, who_a = 1;            // start_who of the starts of b and a (before the style 6 / 7 swap)
	bool swap = false;                   // the conquest map style is 6 or 7
	int barbarians = 0;                  // additional_opponents[difficulty]
	std::vector<ConquestSupporter> supporters;  // the link's supporters (seats 2..) before the barbarians
	std::string tileset;                 // the defended node's tileset (the attacker's for style 5); empty = the style's pick
	bool no_rare = false;                // the battle style says no_rare: Map::make skips place_resources
};
// draw_scenario_area: `piece(country)` returns the silhouette image of a country (alpha != 0 = land) or null. Fills side / land / ids / xs /
// ys of `out` (the other fields are the caller's). `scale` = the strategic map's map_scale (map_width / tex_width).
using ConquestPieceLoader = std::function<const Image*(int)>;
bool conquest_draw_scenario_area(const ConquestRect& rect, int tex_width, float scale, const std::vector<ConquestCountry>& countries,
                                 const ConquestPieceLoader& piece, ConquestInput& out);

void set_conquest_input(const ConquestInput* in);  // thread-local
const ConquestInput* conquest_input();
struct ConquestScope {
	explicit ConquestScope(const ConquestInput* in) : prev_(conquest_input()) { set_conquest_input(in); }
	~ConquestScope() { set_conquest_input(prev_); }
	ConquestScope(const ConquestScope&) = delete;
	ConquestScope& operator=(const ConquestScope&) = delete;
private:
	const ConquestInput* prev_;
};

struct ConquestStart {  // the conquest start lists (start_who / start_capital / start_node / start_add_armies) of one start
	int32_t who = 0, node = -1, armies = 0, x = 0, y = 0;
};

struct Params {
	int style = kStyleOldWorld;  // MapStyleIndex
	int size = 3;                // rules mapsizes index 0..6 (Tiny .. Big Huge)
	uint32_t seed = 1;           // GameInfo.seed
	int nations = 4;             // VALID leaders, in slots 0..nations-1
	std::array<int, 8> team{{8, 8, 8, 8, 8, 8, 8, 8}};  // team of each slot; 8 (or < 0) = no team
	bool randomize_starts = false;
	int team_style = 0;          // GameInfo.team_style (2 and 7 change a few picks)
	int starting_town = 1;       // 0 nomad, 1 city, 2 small town, 3 large town
	int starting_resources = 0;  // GameInfo.starting_resources (7 / 8 add citizens)
	int reveal_map = 1;
	bool ctw = false;            // Conquer the World (excludes Marble from the pool)
	std::array<int, 8> tribe{{0, 1, 2, 3, 4, 5, 6, 7}};  // tribe of each slot
};

struct Context {
	const GameFiles& files;
	const Rules& rules;
	World& world;
	Regions& regions;
	Random& rng;                  // game_random; the caller seeds nothing: generate() reseeds with Params::seed
	std::vector<GoodObj>& goods;  // Objects::init_good appends here
	std::vector<GoodObj>& items;  // Objects::init_item appends here
};

struct HerdRec {  // Herds::init_herd (randommap-resources section 8)
	int32_t id = 0, wx = 0, wy = 0, unit_type = 0, good_slot = 0;
};
struct HerdAnimal {  // one Herd::create_units unit (position = the herd cell: find_nearby_spot needs a Game)
	int32_t herd = 0, unit_type = 0, x = 0, y = 0;
	uint32_t angle = 0;
};

// One Setup::build_empire product. City / Building: type and (for the city) position; buildings other than the city
// are placed by the AI site search (Leader::produce_building), so x / y are -1. Unit: position from Setup::place_unit.
struct EmpireEntry {
	enum Kind : uint8_t { City, Building, Unit } kind = Unit;
	int32_t who = 0, start = 0;
	int32_t type = 0;
	int32_t x = -1, y = -1;        // world units
	int32_t note = 0;              // Unit: 1 = created at farm slot (no ring draw), 2 = go_inside the university
};

struct RiverPath {
	std::vector<std::array<int32_t, 2>> pts;  // wcoords, source to mouth
	int32_t segment = 0;                      // 0 whole river (shipped flags)
	bool edge = false;                        // the mouth is on the map border (make_riverlist adds the duplicate and the outside point)
};

struct Result {
	bool ok = false;
	std::string error;
	int style = 0, picked_style = 0, size = 0, xs = 0, ys = 0;
	uint32_t world_seed = 0;
	std::string tileset;
	std::array<int32_t, 6> territory_limits{};  // player base / civic / city, colonized base / civic / city
	std::vector<std::array<int32_t, 2>> starts;  // start_x / start_y
	std::vector<ConquestStart> conquest_starts;  // styles 24 / 25: one per start, in the same order
	std::vector<std::array<int32_t, 2>> oil_locations;
	std::array<int32_t, 8> start_order{};
	std::vector<PlacedMountain> mountains;
	std::vector<HerdRec> herds;
	std::vector<HerdAnimal> animals;
	std::vector<EmpireEntry> empire;
	std::vector<RiverPath> rivers;
	int32_t river_beds = 0;
	int32_t doobers = 0;
	// TerrainOut's height grid after Terrain::init's coast pass (master_land_heights): (4 xs + 1) x (4 ys + 1) floats, x fastest.
	// Land is the fractal height blended towards the coast, the sea is -273, the shelf follows the coast generators' influence
	// lines; mountains and river troughs are not in it (adjust_for_mountains and RiversOut are later passes).
	std::vector<float> heights;
	int32_t height_vxs = 0, height_vys = 0;
	int32_t retries = 0;           // restarts of the continent recipe
	bool fallback = false;         // the recipe gave up and the guaranteed ring layout was used
	// game_random draws consumed, in total and per phase
	uint64_t draws = 0;
	std::vector<std::pair<std::string, uint64_t>> phases;
	std::vector<std::string> notes;
	// For the PNG / dump: which goods are resources (type, wcoord).
	int32_t num_goods = 0, num_goodies = 0;
};

// Style names: MapStyleIndex <-> name (rules mapstyles keys, e.g. "Old World"); -1 when unknown.
int style_from_name(const Rules& rules, const std::string& name);
std::string style_name(const Rules& rules, int style);

// Generates straight into a Game whose `rules` are set: world, regions, goods / items (and good_mark), the seed and the
// land total World::analyze_map would compute. The empire list, herds, rivers and territory limits stay in the Result.
// (Leader::init, the objects of the empires and herds, compute_all_territory and the terrain heights are the caller's.)
Result generate_into(Game& g, const GameFiles& files, const Params& p);

// The whole generator. Resets ctx.world / ctx.regions / ctx.goods / ctx.items. Never throws for a supported style;
// failure sets Result::ok = false and Result::error.
Result generate(Context& ctx, const Params& p);

}  // namespace ron::sim::mapgen
