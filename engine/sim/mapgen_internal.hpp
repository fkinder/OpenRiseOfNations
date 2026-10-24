// Internals of the random-map generator (sim/mapgen.hpp): the `Map` object of the original as one class `Gen`, split over
// mapgen_data.cpp (map style data), mapgen_land.cpp (region primitives and land shaping), mapgen_styles.cpp (the per-style
// continent recipes), mapgen_terrain.cpp (terrain groups), mapgen_resources.cpp, mapgen_rivers.cpp, mapgen_empire.cpp
// (start order and Setup::build_empire) and mapgen.cpp (Setup::build_game's driver).
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sim/compass.hpp"
#include "sim/mapgen.hpp"
#include "sim/mountains.hpp"
#include "sim/placement.hpp"
#include "sim/rules_xml.hpp"
#include "sim/trig.hpp"

namespace ron::sim::mapgen {

// game_random with a draw counter. rnd(n) is GameAccess::rnd (no draw for n <= 1); rnd_draw always draws.
struct Rnd {
	Random& r;
	uint64_t draws = 0;
	explicit Rnd(Random& rr) : r(rr) {}
	int32_t get() {
		draws++;
		return r.get(0, 0xffff);
	}
	int32_t rnd(int32_t n) { return n <= 1 ? 0 : get() % n; }
	int32_t rnd_draw(int32_t n) { return get() % n; }
	int32_t bit() { return get() & 1; }
	int32_t mask(int32_t m) { return get() & m; }
};

inline std::string lc(std::string s) {
	for (char& ch : s) {
		if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
	}
	return s;
}

inline int vd(int dx, int dy) { return vector_dist(dx, dy); }

// ---------------------------------------------------------------- XML-derived data
struct GroupEntry {  // TerrainGroup (randommap-terrain-groups A1)
	int type = -1, chance = -1, grouping = -1;
	int min_clumps = 0, max_clumps = 0, pattern = 0;
	int min_size = 0, max_size = 0, cliff_face = -1;
	int start_min = 0, start_max = 0;
	int forest_space = 0, mount_space = 0, rock_space = 0, coast_space = 0;
	int cent_min = 0, cent_max = 0, edge_min = 0, edge_max = 0, corner_min = 0, corner_max = 0;
	int min_oil = 0, max_oil = 0;
};
enum GroupType { kTrees = 4, kMountains = 5, kRocks = 6, kOilGroup = 7, kCliffs = 8 };

struct BonusEntry {  // one <BONUS> (randommap-resources section 1)
	std::string type;
	int kind = 0;        // 0 named good, 1 Random, 2 Early, 3 Late
	int good = -1;
	bool resolved = false;
	int chance = -1, group = -1, numrare = 0, pattern = 0, saturate = 0;
	int pka = -1, psn = 0, sp = 0, gsp = 0, ck = 0, cs = 0, ek = 0, es = 0, cok = 0, cos = 0;
};

struct RiverEntry {  // <RIVER> of <RIVERLIST>
	int start_range = -1, end_range = -1, length = 0, minlen = 0, valley = 0, wide = -1, med = -1, twist = -1,
	    flags = -1;
};

struct TileGroupData {  // tilesets.xml <TERRAINGROUP>
	int clump_factor = 3, forest_base = -1, forest_growth_prob = 0, mountain_base = -1, mountain_growth_prob = 0,
	    mnt_fringe_tree_prob = 0, coast_base = -1, coast_growth_prob = 0, bush_clump_prob = 0, bush_spacing = 0,
	    bush_min = 0, bush_max = 0, mnt_rock_clump_prob = 0, mnt_rock_spacing = 0, mnt_rock_min = 0, mnt_rock_max = 0;
	int river_base = 0;  // <TILESET riverbase=...>: the land_sub of the cells a river runs through (Terrain::spot_update_land_batch)
};

// A region while a recipe builds it (Regions.list entry: cells in the order they were added).
struct GenRegion {
	int size = 0, flags = 0, common = 4, goody = 8, climate = 0;
	uint32_t mask = 0;
	std::vector<WCoord> cells;
};

// A placed terrain group's runtime state.
struct GroupRun {
	std::vector<WCoord> tiles;
};

constexpr int kEntire = 0, kEdges = 1, kCorners = 2;  // eliminate_pools modes

class Gen {
public:
	Gen(Context& ctx, const Params& p, Result& out);

	// Setup::build_game steps (mapgen.cpp)
	void build_game();
	void make();

	// ---- inputs and shared state
	Context& c;
	Params P;
	Result& res;
	Rnd r;
	World& w;
	Regions& rg;
	const Rules& rules;
	int nations = 0;

	int style = 0;       // concrete MapStyleIndex after set_map
	int map_size = 0;
	int xs = 0, ys = 0, cells = 0;
	int ref_w = 70;      // map_sizes[3].data[0]: the reference width of scale_by_area / scale_by_axis
	uint32_t world_seed = 0;

	// Map fields (randommap-spec section 4)
	int river_thresh = 8, goody_factor = 8, common_factor = 4, avoid_center = 0, avoid_continent = 0;
	std::array<int, 4> avoid_edge{};
	int base_edge = -1, sea_map = -1, start_dist = 0, avoid_modify = 0, region_flags = 0;
	std::array<int32_t, 6> terr{};
	std::vector<WCoord> starts;
	std::vector<uint8_t> start_city_bits;     // World +0xf8: the 2x2 cells of every start (WorldData::start_city_wcoord)
	std::vector<WCoord> start_city_list;      // World +0xc8 / +0xe4: the same cells, for start_city_rad_wcoord
	std::array<GenRegion, Regions::kNum> reg;
	int reg_land = 0;
	bool in_generation = true;  // recipes use GenRegion; after sync_regions() the engine's regions

	// style files
	XDoc def_doc, style_doc;
	const XNode* def_root = nullptr;
	const XNode* style_root = nullptr;
	std::string style_file;

	// lands, tilesets, groups
	std::array<std::vector<int>, 9> lands_rare;
	std::string tileset;
	TileGroupData tgd;
	std::vector<uint8_t> partitions;
	std::vector<uint8_t> fractal_raw;  // (xs + 1) * (ys + 1), x-major
	std::vector<GroupEntry> groups;
	std::array<int, 3> tile_counts{{4, 1, 1}};  // TileSetData num_tiles: BASELAND / COASTAL / OCEAN <BASE> counts

	// ---- mapgen_data.cpp
	void load_map_data();
	int scale_by_area(int n) const;
	int scale_by_axis(int n) const;
	int scale_number(const XNode* el, const char16_t* attr, int def = 0) const;
	int scale_text(const ustr& text) const;
	const XNode* section(const char16_t* name) const;  // the style's element, else default.xml's
	void read_map_element(const XNode* map);
	void choose_tileset();
	void read_tileset_data(const XNode* root, bool is_default);
	void read_tileset_group_data();
	void init_resources(const XNode* root);
	void init_terrain_data();
	void fractal_init();
	void fill_fertile();
	std::vector<std::string> available_tilesets;
	std::vector<std::array<int, 4>> tileset_freqs_dummy;
	std::array<int, 4> base_freq{};
	int base_freq_count = 4;

	// ---- mapgen_land.cpp
	bool inb(int x, int y) const { return x >= 0 && y >= 0 && x < xs && y < ys; }
	WData& W(int x, int y) { return w.w(x, y); }
	bool is_water(int x, int y) { return World::is_ocean(w.w(x, y)); }
	void wipe();
	void clear_all();
	void find_all();      // engine Regions::find_all, then sync
	void sync_regions();  // GenRegion table := the engine's regions
	void make_region(int rid, int x, int y, int goal);
	int point(int rid, int x, int y);
	int stamp(int rid, int x, int y);
	int large_stamp(int rid, int x, int y, int left);
	bool grow_valid(int rid, int x, int y);
	int grow_region(int rid, int goal, int m, int ox, int oy, int p6);
	int land_dist(int x, int y, int outside_is_land);
	void invert_land();
	void fix_lakes();
	void fix_diag_land();
	void make_coastlines();
	void eliminate_pools(int mode);
	void eliminate_edge_canals();
	void check_player_land(int range, bool nearby = true);
	void check_player_forest();
	int scale_land_area(int v, int n) const;
	int get_player_overage(int n) const;
	void find_region_centroid(int rid, int* x, int* y);
	bool place_start_in_region(int rid, int* ox, int* oy, int min_dist, const std::vector<WCoord>& others);
	bool is_near_ocean(int x, int y, int a, int b);
	std::vector<int> find_avail_regions(bool sea, int pattern, int* current);
	void add_starting_location(int x, int y);
	bool start_city_wcoord(int x, int y) const { return start_city_bits[size_t(y) * size_t(xs) + size_t(x)] != 0; }
	void project_pt(int cx, int cy, uint32_t angle, int rad, int* x, int* y) const;
	// team -> continent assignment (Map::fill_cont)
	void fill_cont(std::array<int, 8>& count, std::array<int, 8>& cont, std::vector<int>* extra, int* sides);
	int rnd_angle_pair(uint32_t* angle);
	bool near_start_city(int x, int y) const;  // start_city_wcoord / start_city_rad_wcoord
	bool has_mountain_tcoords(int x, int y) const;
	bool is_edge(int x, int y) const { return x == 0 || y == 0 || x == xs - 1 || y == ys - 1; }
	int center_d(int x, int y) const;
	int edge_d(int x, int y) const { return std::min(std::min(x, y), std::min(xs - x, ys - y)); }
	int corner_d(int x, int y, int corner) const;
	int tribe_of_start(int s) const { (void)s; return 0; }

	// ---- mapgen_styles.cpp
	void set_map();
	bool make_continents();  // false = the recipe gave up (caller falls back)
	bool mc_grass();
	bool mc_all_land_ring();     // Old World / Sahara / Amazon / Himalayas / Southwest Mesa
	bool mc_africa();
	bool mc_mediterranean();
	bool mc_outback();
	bool mc_great_lakes();
	bool mc_warring_states();
	bool mc_new_world();
	bool mc_colonial_powers();
	bool mc_east_indies();
	bool mc_east_meets_west();
	bool mc_sea_power();
	bool mc_nile_delta();
	bool mc_british_isles();
	void fallback_ring();
	bool scatter_lakes(bool land_regions, int first_region, int count_extra);
	int retry_budget = 0;
	int attempts = 0;
	bool give_up() { return ++attempts > 400; }

	// ---- mapgen_conquest.cpp (styles 24 / 25)
	bool mc_conquest();
	void conquest_import(const ConquestInput& ci, int percent);
	void force_continent_spacing();
	bool cq_check_starting_loc(int x, int y, int mindist, int r, bool want_land, int skip_player);
	void cq_push_start(int x, int y, int who, int node, int armies);
	void cq_add_additional_players(const ConquestInput& ci, int region_b, int region_a);
	void cq_check_sea_map();
	std::vector<ConquestStart> conq_starts;   // the conquest start lists, in the order of `starts`

	// ---- mapgen_terrain.cpp
	struct Shape {
		std::vector<int> ox, oy;
	};
	void place_all();
	void add_doobers();
	void treeify_mountains(int prob);
	void nubify_forest();
	void nubify_transitions(int s);
	void change_forest_base();
	void change_mountain_base();
	void change_coast_base();
	void fix_transitions();
	void set_oil_at(int x, int y, bool on);
	bool available_rough_tile(int x, int y, int kind, int mount_sp, int forest_sp, int rock_sp, int coast_sp,
	                          const GroupRun* run);
	bool drop_tile(const GroupEntry& g, GroupRun& run, int x, int y, int stay_near, int range_index = -1);
	void clear_group(GroupRun& run, int type);
	void place_oil_deposits(GroupRun& run, int count);
	bool add_mountain(int range_index, int wx, int wy, const GroupEntry& g, int space_m, int space_f, int space_r,
	                  int space_c, int start_min, int mode);
	bool quick_verify_template(const std::vector<Offset>& face, int wx, int wy);
	int grow_clump(const GroupEntry& g, GroupRun& run, int cx, int cy, int size, int sx, int sy, bool has_start,
	               Shape* shape, bool copy);
	bool tile_tests(const GroupEntry& g, int x, int y);
	int grow_player(const GroupEntry& g, GroupRun& run, int size, WCoord st, int smin, int smax, Shape* shape);
	bool place_player_group(const GroupEntry& g, int type_t, int size, int p, int oilcount, int range_index,
	                        Shape* shape, bool copy, GroupRun& run);
	int place_region_group(const GroupEntry& g, int size, int rid, int range_index, int oilcount, GroupRun& run);
	void run_group(size_t gi, int nc, const std::vector<int>& sizes, const std::vector<int>& oils);
	int is_helping = 0;  // TerrainGroup::is_helping (a global of the original, set by place_all before every region group)
	int get_range(int size);
	void randomize_mountains();
	std::vector<int> mountain_lists[4];  // [1] small, [2] medium, [3] large: range indices
	int mountain_cursor[4] = {0, 0, 0, 0};
	const MountainRange* mount_range(int index);
	bool mountains_loaded = false;
	void load_mountain_lists();
	std::vector<PlacedMountain> placed_mountains;
	std::vector<uint8_t> doober_cells;
	std::vector<uint8_t> mount_seen;                                          // add_mountain scratch (all zero between calls)
	std::map<int, std::vector<Offset>> mount_faces;                                // face wcoords per range
	std::map<int, const MountainRange*> mount_ptrs;                                // mount_range() results
	int doober_count = 0;
	// fairness state of place_all
	std::array<int, 5> help_lowest_index{};
	std::array<std::array<int, 5>, 8> player_scores{};
	std::array<int, 5> lowest_player{};
	void randomize_orthogs(int* ro, int* rc);

	// ---- mapgen_terrain_init.cpp
	void terrain_init();
	void river_geometry();  // RiversOut::calculate_all: the river sections, their trough tiles and heights (randommap-rivers section 9)
	std::vector<uint16_t> coord_flags;  // TerrainOut's per-wcoord CoordInfo.flags after Terrain::init's coast pass (bit 4 = coastal)

	// ---- mapgen_resources.cpp
	void place_resources();
	int place_player_resource(int good, int count, const BonusEntry& b, int kind, bool water_class);
	int place_region_resource(int good, const BonusEntry& b, int kind);
	bool good_in_pool(int g) const;
	std::vector<int> pool_early, pool_late, pool_water;
	std::vector<uint8_t> bits_early, bits_late, bits_water;
	void pool_add(int g);
	void pool_done();
	int pool_get(int which);
	int land_class(int x, int y);
	bool rare_ok(int u, int good) const;
	bool has_south_forest(int x, int y);
	bool object_spacing_ok(int x, int y, int sp, bool water, bool goody) const;
	void put_good(int good, int x, int y);
	void put_goody(int x, int y);
	std::vector<std::array<int, 3>> placed_goods;  // wx, wy, type of every placed good
	std::vector<std::array<int, 2>> placed_items;  // wx, wy of every goody box
	bool region_accept(int x, int y, int good, const BonusEntry& b, bool water, bool goody, int round, int k, int th, int worst,
	                   const std::vector<WCoord>& placed);
	std::vector<BonusEntry> read_bonuses(const char16_t* name);
	std::vector<HerdRec> herds;
	void create_herd_units();

	// ---- mapgen_rivers.cpp
	void make_rivers();
	void make_riverbeds();
	void make_riverlist(int start, int range, int length, int minlen, int valley, int wide, int med, int twist,
	                    int flags);
	int astar_river(int sx, int sy, int twist, int minlen, std::vector<std::array<int, 2>>* out);
	std::vector<WCoord> river_beds;

	// ---- mapgen_empire.cpp
	void build_empires();
	void build_empire(int who, int start);
	int place_unit(int who, bool has_city, int type, int x, int y, int ux, int uy);
	void emit_unit(int who, int start, int type, int x, int y, int note);
	std::vector<uint8_t> occupied;  // wcoords with a placed unit / the city footprint
	std::vector<uint8_t> blocked_tiles;  // the city's footprint tcoords
	std::array<int, 10> active_units{};
	int cur_start = 0;

	// phases for the report
	void phase(const char* name);
	uint64_t phase_mark = 0;
	void note(const std::string& s) { res.notes.push_back(s); }
};

class Gen;
int good_key(const Rules& rules, const std::string& name);
int pool_age(const Rules& rules, int g);
// Style index helpers
const char* style_file_name(int style);
bool valid_leader_slot(const Params& p, int slot);
bool is_team(const Params& p, int a, int b);

}  // namespace ron::sim::mapgen
