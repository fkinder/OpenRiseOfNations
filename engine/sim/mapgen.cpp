// Setup::build_game for a random map (docs/randommap-spec.md section 1): steps 3 (world.seed), 4 (Map::new_map, Map::make,
// Terrain::init's rivers), 7 (start order), 9 (empires) and 10 (herds), with every game_random draw counted per phase.
#include "sim/mapgen.hpp"

#include <cstdlib>
#include <exception>
#include <stdexcept>

#include "sim/game.hpp"
#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

int style_from_name(const Rules& rules, const std::string& name) {
	if (name.empty()) return -1;
	bool numeric = true;
	for (char ch : name) {
		if (ch < '0' || ch > '9') numeric = false;
	}
	if (numeric) {
		const int v = std::atoi(name.c_str());
		return v >= 0 && v < kNumStyleNames ? v : -1;
	}
	const std::string key = lc(name);
	if (key == "blank" || key == "blank map" || key == "default") return kStyleBlank;
	const CategoryList* ms = rules.category_list("map_styles");
	if (ms) {
		for (size_t i = 0; i < ms->items.size(); i++) {
			if (lc(to_utf8(ms->items[i].key)) == key) return int(i);
		}
		// the style-file stem (OLDWORLD, SEAPOWER, ...)
		for (int s = 6; s < kNumStyleNames; s++) {
			const char* f = style_file_name(s);
			if (f && lc(f) == key) return s;
		}
	}
	return -1;
}

std::string style_name(const Rules& rules, int style) {
	const CategoryList* ms = rules.category_list("map_styles");
	if (ms && style >= 0 && size_t(style) < ms->items.size()) return to_utf8(ms->items[size_t(style)].key);
	if (style == kStyleBlank) return "Blank Map";
	if (style == kStyleConquest) return "Conquest Map";
	if (style == kStyleConquestSea) return "Conquest Map (sea)";
	if (style == kStyleScenario) return "Scenario Map";
	return "?";
}


Gen::Gen(Context& ctx, const Params& p, Result& out)
    : c(ctx), P(p), res(out), r(ctx.rng), w(ctx.world), rg(ctx.regions), rules(ctx.rules) {
	nations = std::max(1, std::min(8, p.nations));
	P.nations = nations;
	map_size = p.size;
	if (const CategoryList* sizes = rules.category_list("map_sizes")) {
		const int n = int(sizes->items.size());
		if (map_size < 0) map_size = 0;
		if (map_size >= n) map_size = n - 1;
		xs = sizes->items[size_t(map_size)].data[0];
		if (n > 3) ref_w = sizes->items[3].data[0];
	} else {
		xs = 70;
	}
	ys = xs;
	cells = xs * ys;
	style = p.style;
	// A CtW battle (sim/ctw_battle.cpp keeps a ConquestScope open): the random-map pickers 0..5 the setup carries become MapConquest /
	// MapConquestSea (build_conquest_game: style 0x18, 0x19 for a sea link).
	if (const ConquestInput* ci = conquest_input()) {
		if (ci->valid && style >= 0 && style <= 5) style = ci->sea ? kStyleConquestSea : kStyleConquest;
	}
}

void Gen::phase(const char* name) {
	res.phases.push_back({name, r.draws - phase_mark});
	phase_mark = r.draws;
}

// Map::make @68bc90
void Gen::make() {
	load_map_data();
	phase("load_map_data");
	if (base_edge < 0) base_edge = r.mask(3);
	const bool ok = make_continents();
	if (!ok) {
		starts.clear();
		fallback_ring();
		note("continent recipe gave up after " + std::to_string(attempts) + " attempts: guaranteed ring layout used");
	}
	phase("make_continents");
	clear_all();
	find_all();
	for (int i = 0; i < 6; i++) res.territory_limits[size_t(i)] = terr[size_t(i)];
	if (style == kStyleBlank) {
		if (sea_map >= 0) w.sea_map = sea_map;
		return;
	}
	fix_diag_land();
	make_coastlines();
	clear_all();
	find_all();
	phase("land shaping");
	fill_fertile();
	place_all();
	check_player_forest();
	nubify_forest();
	if (tgd.forest_base >= 0) change_forest_base();
	if (tgd.mountain_base >= 0) change_mountain_base();
	if (tgd.coast_base >= 0) change_coast_base();
	fix_transitions();
	phase("terrain groups");
	if (!(style >= kStyleConquest && conquest_input() && conquest_input()->no_rare)) place_resources();  // the CtW style says no_rare
	phase("place_resources");
	rg.clear_all(w);
	adjust_for_blocking(w, rg, placed_mountains);
	clear_all();
	find_all();
	r.draws += uint64_t(compute_all_vals(w, c.rng, land_goods(rules), rg));
	phase("compute_all_vals");
	if (sea_map >= 0) w.sea_map = sea_map;
}

void Gen::build_game() {
	// step 1/3
	c.rng.reseed(P.seed);
	r.draws = 0;
	phase_mark = 0;
	world_seed = uint32_t(r.rnd_draw(0xffff)) + 1;
	res.world_seed = world_seed;
	phase("world.seed");
	// step 4: Map::new_map, Map::make
	if (P.style < 0 || P.style >= kStyleScenario) throw std::runtime_error("map style " + std::to_string(P.style) + " is not a random map style");
	if ((style == kStyleConquest || style == kStyleConquestSea) && !(conquest_input() && conquest_input()->valid))
		throw std::runtime_error("No conquest map data available.");
	res.style = style;
	{
		// oracle: current 2.SVX (lobby style 1 -> 14, seed 18119929): the picks of Map::set_map are d1..d3 of the stream, and
		// the tileset (d1, d2: early_autumn), base_edge (d3) and the Great Lakes start angle (d4, d5) that follow reuse the
		// same draws: game_random is back at its post-world.seed state after the pick (the draws of the pick are not kept).
		const uint32_t after_world_seed = c.rng.seed();
		const uint64_t draws0 = r.draws;
		set_map();
		c.rng.reseed(after_world_seed);
		r.draws = draws0;
	}
	res.picked_style = style;
	phase("set_map");
	w.init(xs, ys);
	rg = Regions();
	c.goods.clear();
	c.items.clear();
	for (GenRegion& g : reg) g = GenRegion();
	res.xs = xs;
	res.ys = ys;
	res.size = map_size;
	make();
	res.tileset = tileset;
	// Terrain::init: the coast pass (half water, shelf, tile fixes), then the rivers
	terrain_init();
	phase("terrain_init");
	make_rivers();
	phase("make_rivers");
	mountain_heights();  // Terrain::init ends with TerrainOut::adjust_for_mountains(0) ...
	cliffs_init();       // ... and Cliffs::init
	if (!placed_cliffs.empty()) {
		int cw = 0, ct = 0;
		for (const WData& d : w.wdata) cw += (d.flags & wflag::kCliff) != 0;
		for (uint16_t m : w.tdata) ct += (m & tmask::kLowField) == tmask::kCliff;
		note(std::to_string(placed_cliffs.size()) + " cliffs placed: " + std::to_string(cw) + " cliff wcoords, " + std::to_string(ct) + " cliff tcoords");
	}
	// what a caller that creates the empire's objects as the entries are emitted (Params::on_empire_entry) needs of the result by now
	res.mountains = placed_mountains;
	for (const WCoord& s : starts) res.starts.push_back({s.x, s.y});
	res.conquest_starts = conq_starts;
	build_empires();
	create_herd_units();
	phase("herds");
	for (const GoodObj& go : c.goods) {
		if (go.alive() && go.type == kOilGood) res.oil_locations.push_back({wcoord_of(go.x), wcoord_of(go.y)});
	}
	res.mountains = placed_mountains;
	res.draws = r.draws;
}

Result generate(Context& ctx, const Params& p) {
	Result out;
	try {
		Gen g(ctx, p, out);
		g.build_game();
		out.ok = true;
	} catch (const std::exception& e) {
		out.ok = false;
		out.error = e.what();
	}
	return out;
}

}  // namespace ron::sim::mapgen

namespace ron::sim::mapgen {

Result generate_into(Game& g, const GameFiles& files, const Params& p) {
	Context ctx{files, *g.rules, g.world, g.regions, g.rng, g.goods, g.items};
	Result out = generate(ctx, p);
	if (!out.ok) return out;
	g.seed = p.seed;
	g.good_mark = int32_t(g.goods.size());
	g.world_total_land = 0;
	for (int32_t r = 1; r < 64; r++) g.world_total_land += g.regions.r[size_t(r)].size;
	g.info.map_style = uint8_t(out.picked_style);
	return out;
}

}  // namespace ron::sim::mapgen
