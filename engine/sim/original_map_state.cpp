// Field maps of the regions, the small global scalars (random state, daemon counters, AI switches) and the constants
// (docs/original-save.md).
#include <cstring>

#include "sim/original_map.hpp"

namespace ron::sim::omap {

namespace {

Region* region_of(Game& g, const Seg& s) { return s.a >= 0 && s.a < Regions::kNum ? &g.regions.r[size_t(s.a)] : nullptr; }

// A BitMask<N> member read as one little-endian integer: header (bits, size, flags) then `size` bytes at +0xc.
void bitmask_u64(Builder<Region>& b, const char* name, uint64_t Region::*mp) {
	const dw::FieldInfo* f = dw::Description::get().find_field("Region", name);
	if (!f) throw std::logic_error(std::string("Region.") + name);
	b.raw(f->offset + 0xc, 8, name, [mp](Region& r, const uint8_t* p) { r.*mp = get_le(p, 8); }, [mp](const Region& r, uint8_t* p) { put_le(p, 8, r.*mp); });
}

}  // namespace

const ClassMap& map_wcoord() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<WCoord> b(m, "WCoordData", "WCoordData");
		b.walks({"WCoordData::walk_data"}).resolve([](Game& g, const Seg& s) -> WCoord* {
			Region* r = region_of(g, s);
			return r && size_t(s.b) < r->coords.size() ? &r->coords[size_t(s.b)] : nullptr;
		});
		b.raw(0, 4, "x", [](WCoord& c, const uint8_t* p) { scalar_load(c.x, p, 4, true); }, [](const WCoord& c, uint8_t* p) { scalar_emit(c.x, p, 4); });
		b.raw(4, 4, "y", [](WCoord& c, const uint8_t* p) { scalar_load(c.y, p, 4, true); }, [](const WCoord& c, uint8_t* p) { scalar_emit(c.y, p, 4); });
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_region() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Region> b(m, "Region", "Region");
		b.walks({"Region::walk_data"}).resolve([](Game& g, const Seg& s) { return region_of(g, s); });
		b.f("common_factor", &Region::common_factor).f("goody_factor", &Region::goody_factor).f("flags", &Region::flags);
		b.f("climate", &Region::climate).f("region", &Region::region).f("size", &Region::size).f("rank", &Region::rank);
		b.f("fertile", &Region::fertile);
		bitmask_u64(b, "coastal", &Region::coastal);
		bitmask_u64(b, "coast", &Region::coast);
		const dw::Description& d = dw::Description::get();
		static const int step_coords = d.find_step(d.find_function("Array<WCoordData>::walk_data"), dw::StepKind::Container);
		b.arrays([](const Game& g, const Seg& s, int step, int64_t, ArraySpec& spec) {
			if (step != step_coords || s.a < 0 || s.a >= Regions::kNum) return false;
			spec.count = int32_t(g.regions.r[size_t(s.a)].coords.size());
			spec.child = &map_wcoord();
			const int32_t a = s.a;
			spec.child_id = [a](int32_t i) { return Seg{a, i, 0}; };
			return true;
		});
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_regions_global() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Game> b(m, "Regions", "Regions");
		b.walks({"Regions::walk_data"}).resolve([](Game& g, const Seg&) { return &g; });
		b.c("sea", [](Game& g, const uint8_t* p) { scalar_load(g.regions.sea, p, 4, true); }, [](const Game& g, uint8_t* p) { scalar_emit(g.regions.sea, p, 4); });
		b.c("land", [](Game& g, const uint8_t* p) { scalar_load(g.regions.land, p, 4, true); }, [](const Game& g, uint8_t* p) { scalar_emit(g.regions.land, p, 4); });
		const dw::Description& d = dw::Description::get();
		static const int step_regions = d.find_step(d.find_function("ObjectArray<Region>::walk_data"), dw::StepKind::Container);
		b.arrays([](const Game&, const Seg&, int step, int64_t base, ArraySpec& spec) {
			if (step != step_regions || base != 0) return false;
			spec.count = Regions::kNum;
			spec.child = &map_region();
			spec.child_id = [](int32_t i) { return Seg{i, 0, 0}; };
			return true;
		});
		m.finish();
		return m;
	}();
	return m;
}

// ---- small global scalars ------------------------------------------------------------------------------------------------

namespace {

ClassMap scalar_global(const char* name, int32_t off, int32_t size, std::function<void(Game&, const uint8_t*)> ld, std::function<void(const Game&, uint8_t*)> em) {
	ClassMap m;
	Builder<Game> b(m, name, "");
	b.walks({}).resolve([](Game& g, const Seg&) { return &g; });
	b.raw(off, size, name, std::move(ld), std::move(em));
	m.finish();
	return m;
}

}  // namespace

const ClassMap& map_game_random() {
	static const ClassMap m = scalar_global("game_random", 0, 4, [](Game& g, const uint8_t* p) { g.rng.reseed(uint32_t(get_le(p, 4))); },
	                                        [](const Game& g, uint8_t* p) { put_le(p, 4, g.rng.seed()); });
	return m;
}
const ClassMap& map_ai_speed() {
	static const ClassMap m = scalar_global("ai_speed", 0, 4, [](Game& g, const uint8_t* p) { scalar_load(g.ai_speed, p, 4, true); },
	                                        [](const Game& g, uint8_t* p) { scalar_emit(g.ai_speed, p, 4); });
	return m;
}
const ClassMap& map_ai_off() {
	static const ClassMap m = scalar_global("ai_off", 0, 4, [](Game& g, const uint8_t* p) { g.ai_off = get_le(p, 4) != 0; },
	                                        [](const Game& g, uint8_t* p) { put_le(p, 4, g.ai_off ? 1 : 0); });
	return m;
}
const ClassMap& map_armies_find_dist() {
	static const ClassMap m = scalar_global("armies_find_dist", 0, 4, [](Game& g, const uint8_t* p) { scalar_load(g.armies_find_dist, p, 4, true); },
	                                        [](const Game& g, uint8_t* p) { scalar_emit(g.armies_find_dist, p, 4); });
	return m;
}
const ClassMap& map_queue_fail() {
	static const ClassMap m = scalar_global("build_queue_fail", 0, 4, [](Game& g, const uint8_t* p) { scalar_load(g.queue_fail, p, 4, true); },
	                                        [](const Game& g, uint8_t* p) { scalar_emit(g.queue_fail, p, 4); });
	return m;
}

// GameDaemon [0, 0x28): repaths[8], empty_colls, borders
const ClassMap& map_game_daemon() {
	static const ClassMap m = scalar_global(
	    "game_daemon", 0, 0x28,
	    [](Game& g, const uint8_t* p) {
		    arr_load(g.daemon.repaths, p, 4);
		    scalar_load(g.world.empty_colls, p + 0x20, 4, true);
		    scalar_load(g.check_borders_budget, p + 0x24, 4, true);
	    },
	    [](const Game& g, uint8_t* p) {
		    arr_emit(g.daemon.repaths, p, 4);
		    scalar_emit(g.world.empty_colls, p + 0x20, 4);
		    scalar_emit(g.check_borders_budget, p + 0x24, 4);
	    });
	return m;
}

// ScenarioData statics (each is its own global of the walk, ScenarioData::walk_data @997ad0). The members the engine models are
// loaded from the save and emitted back, so a change the simulation makes (last_razed, the kill tallies) reaches the hashed
// `scenario_data` part; the others stay in the shadow.
const ClassMap& map_scen_last_razed() {
	static const ClassMap m = scalar_global("ScenarioData::last_razed", 0, 32, [](Game& g, const uint8_t* p) { arr_load(g.scen.last_razed, p, 4); },
	                                        [](const Game& g, uint8_t* p) { arr_emit(g.scen.last_razed, p, 4); });
	return m;
}
const ClassMap& map_scen_units_killed() {  // units_killed[8][352] i16
	static const ClassMap m = scalar_global(
	    "ScenarioData::units_killed", 0, 8 * 0x2c0,
	    [](Game& g, const uint8_t* p) { for (size_t k = 0; k < 8; ++k) arr_load(g.scen.units_killed[k], p + k * 0x2c0, 2); },
	    [](const Game& g, uint8_t* p) { for (size_t k = 0; k < 8; ++k) arr_emit(g.scen.units_killed[k], p + k * 0x2c0, 2); });
	return m;
}
const ClassMap& map_scen_builds_destroyed() {  // builds_destroyed[8][129] i16
	static const ClassMap m = scalar_global(
	    "ScenarioData::builds_destroyed", 0, 8 * 0x102,
	    [](Game& g, const uint8_t* p) { for (size_t k = 0; k < 8; ++k) arr_load(g.scen.builds_destroyed[k], p + k * 0x102, 2); },
	    [](const Game& g, uint8_t* p) { for (size_t k = 0; k < 8; ++k) arr_emit(g.scen.builds_destroyed[k], p + k * 0x102, 2); });
	return m;
}
const ClassMap& map_scen_city_lost_to() {
	static const ClassMap m = scalar_global("ScenarioData::city_lost_to", 0, 16, [](Game& g, const uint8_t* p) { arr_load(g.scen.city_lost_to, p, 2); },
	                                        [](const Game& g, uint8_t* p) { arr_emit(g.scen.city_lost_to, p, 2); });
	return m;
}

// Constants [0, 0xd40) and [0x804, 0x808): the engine's rules constants (an int32 table indexed by offset).
const ClassMap& map_constants() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Game> b(m, "Constants", "");
		b.walks({}).resolve([](Game& g, const Seg&) { return &g; });
		b.raw(0, 0xd40, "constants",
		      [](Game&, const uint8_t*) {},  // the engine's constants come from the rules tables (compared by the loader)
		      [](const Game& g, uint8_t* p) {
			      if (!g.rules) return;
			      for (int32_t o = 0; o < 0xd40; o += 4) put_le(p + o, 4, uint32_t(g.rules->constants.at(o)));
		      });
		m.finish();
		return m;
	}();
	return m;
}

// LeaderOptions: list[10] of LeaderOptionData (0x20): who, peasants, peasants_wait, buildings, flags (BitMask<32>: data at +0x1c).
const ClassMap& map_leader_options() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Game> b(m, "LeaderOptions", "LeaderOptions");
		b.walks({}).resolve([](Game& g, const Seg&) { return &g; });
		for (int k = 0; k < kNumLeaders; ++k) {
			const int32_t base = k * 0x20;
			b.raw(base + 4, 12, "options",
			      [k](Game& g, const uint8_t* p) {
				      Leader& l = g.leaders[size_t(k)];
				      scalar_load(l.opt_peasants, p, 4, true);
				      scalar_load(l.opt_peasants_wait, p + 4, 4, true);
				      scalar_load(l.opt_buildings, p + 8, 4, true);
			      },
			      [k](const Game& g, uint8_t* p) {
				      const Leader& l = g.leaders[size_t(k)];
				      scalar_emit(l.opt_peasants, p, 4);
				      scalar_emit(l.opt_peasants_wait, p + 4, 4);
				      scalar_emit(l.opt_buildings, p + 8, 4);
			      });
			b.raw(base + 0x1c, 4, "flags", [k](Game& g, const uint8_t* p) { g.leaders[size_t(k)].options_flags = uint32_t(get_le(p, 4)); },
			      [k](const Game& g, uint8_t* p) { put_le(p, 4, g.leaders[size_t(k)].options_flags); });
		}
		m.finish();
		return m;
	}();
	return m;
}

std::vector<const ClassMap*> all_maps() {
	return {&map_game(), &map_game_info(), &map_world(), &map_wdata(), &map_wdata_block(), &map_collblock(), &map_objects_header(), &map_unit(), &map_animal(),
	        &map_build(), &map_guy(), &map_pathdata(), &map_order_node(), &map_queue_item(), &map_tcoord(), &map_gather_point(), &map_leader(),
	        &map_leader_encrypt(), &map_site(), &map_make_object(), &map_good(), &map_item(), &map_city(), &map_caravan_link(), &map_fort(), &map_dock(), &map_wonder(),
	        &map_supply(), &map_caravan(), &map_caravan_road(), &map_army(), &map_group(), &map_groups_global(), &map_ammo(), &map_death(), &map_region(), &map_wcoord(),
	        &map_regions_global(), &map_game_random(), &map_ai_speed(), &map_ai_off(), &map_armies_find_dist(), &map_queue_fail(), &map_game_daemon(),
	        &map_scen_last_razed(), &map_scen_units_killed(), &map_scen_builds_destroyed(), &map_scen_city_lost_to(), &map_constants(), &map_leader_options(), &map_order(1), &map_order(6), &map_order(7), &map_order(10), &map_order(11), &map_order(12), &map_order(14),
	        &map_order(15), &map_order(16), &map_order(17), &map_order(18), &map_order(19), &map_order(20), &map_order(22), &map_order(23), &map_order(24),
	        &map_order(25), &map_order(26), &map_order(27)};
}

}  // namespace ron::sim::omap
