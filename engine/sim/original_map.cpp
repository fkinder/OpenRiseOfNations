// ClassMap mechanics and the maps of the game-level state: Game, GameInfo, World, WData (docs/original-save.md).
#include "sim/original_map.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace ron::sim::omap {

void ClassMap::finish() {
	std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.off < b.off; });
	max_size = 0;
	for (const Entry& e : entries) max_size = std::max(max_size, e.size);
}

void ClassMap::overlay(const void* obj, const dw::Rec& rec, int64_t seg_base, int64_t lo, int64_t hi, uint8_t* buf) const {
	if (!obj || hi <= lo) return;
	// Entries are sorted by offset; start at the first one that can reach `lo` (a long entry before it may too, so back up).
	size_t i = size_t(std::lower_bound(entries.begin(), entries.end(), lo, [this](const Entry& e, int64_t v) { return e.off + max_size < v; }) - entries.begin());
	std::vector<uint8_t> tmp;
	for (; i < entries.size(); ++i) {
		const Entry& e = entries[i];
		if (e.off >= hi) break;
		if (e.off + e.size <= lo) continue;
		if (e.off >= lo && e.off + e.size <= hi) {
			e.emit(obj, buf + (e.off - lo));
			continue;
		}
		tmp.resize(size_t(e.size));
		rec.read(seg_base + e.off, tmp.data(), tmp.size());
		e.emit(obj, tmp.data());
		const int64_t a = std::max<int64_t>(lo, e.off), b = std::min<int64_t>(hi, e.off + e.size);
		std::memcpy(buf + (a - lo), tmp.data() + (a - e.off), size_t(b - a));
	}
}

void ClassMap::load_from(void* obj, const dw::Rec& rec, int64_t base) const {
	std::vector<uint8_t> tmp;
	for (const Entry& e : entries) {
		tmp.resize(size_t(e.size));
		rec.read(base + e.off, tmp.data(), size_t(e.size));
		e.load(obj, tmp.data());
	}
}

// ---------------------------------------------------------------------------------------------------- field counts

namespace {
struct Range {
	int64_t lo, hi;
};
void raw_ranges(const dw::Description& d, const char* fn_name, std::vector<Range>& out) {
	const int fn = d.find_function(fn_name);
	if (fn < 0) return;
	for (int s = 0; s < d.step_count(); ++s) {
		if (d.step_function(s) != fn || d.step_kind(s) != dw::StepKind::Raw) continue;
		const int64_t a = d.step_const(s, 0), b = d.step_const(s, 1);
		if (a == INT64_MIN || b == INT64_MIN) continue;
		out.push_back({a, b});
	}
}
}  // namespace

FieldCount count_fields(const ClassMap& m) {
	const dw::Description& d = dw::Description::get();
	FieldCount fc;
	std::vector<Range> walked;
	for (const std::string& w : m.walks) raw_ranges(d, w.c_str(), walked);
	const int n = d.field_count(m.cls.c_str());
	for (int i = 0; i < n; ++i) {
		const dw::FieldInfo* f = d.field_at(m.cls.c_str(), i);
		const int64_t lo = f->offset, hi = f->offset + f->size;
		const std::string name = f->name;
		bool is_walked = false;
		for (const Range& r : walked) {
			if (lo < r.hi && hi > r.lo) is_walked = true;
		}
		if (!is_walked) continue;
		++fc.walked;
		bool mapped = false;
		for (const Entry& e : m.entries) {
			if (lo < e.off + e.size && hi > e.off && (e.name == name || (e.off <= lo && e.off + e.size >= hi))) mapped = true;
		}
		if (mapped) ++fc.mapped;
		else fc.unmapped.push_back(name);
	}
	return fc;
}

// ---------------------------------------------------------------------------------------------------- Game

const ClassMap& map_game() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Game> b(m, "Game", "Game");
		b.walks({"Game::walk_data"}).resolve([](Game& g, const Seg&) { return &g; });
		b.f("frame", &Game::frame).f("tick", &Game::tick);
		b.c("market_tick", [](Game& g, const uint8_t* p) { scalar_load(g.market.tick, p, 4, true); },
		    [](const Game& g, uint8_t* p) { scalar_emit(g.market.tick, p, 4); });
		b.c("market", [](Game& g, const uint8_t* p) { arr_load(g.market.level, p, 4); },
		    [](const Game& g, uint8_t* p) { arr_emit(g.market.level, p, 4); });
		b.c("market_flux", [](Game& g, const uint8_t* p) { arr_load(g.market.flux, p, 4); },
		    [](const Game& g, uint8_t* p) { arr_emit(g.market.flux, p, 4); });
		b.c("next_flux", [](Game& g, const uint8_t* p) { arr_load(g.market.next_flux, p, 4); },
		    [](const Game& g, uint8_t* p) { arr_emit(g.market.next_flux, p, 4); });
		b.c("delta_flux", [](Game& g, const uint8_t* p) { arr_load(g.market.delta_flux, p, 4); },
		    [](const Game& g, uint8_t* p) { arr_emit(g.market.delta_flux, p, 4); });
		b.c("flux_length", [](Game& g, const uint8_t* p) { arr_load(g.market.flux_length, p, 4); },
		    [](const Game& g, uint8_t* p) { arr_emit(g.market.flux_length, p, 4); });
		b.f("starting", &Game::starting).f("wonders", &Game::wonders);
		b.c("num_players", [](Game& g, const uint8_t* p) { scalar_load(g.info.num_players, p, 4, true); },
		    [](const Game& g, uint8_t* p) { scalar_emit(g.info.num_players, p, 4); });
		b.c("num_nations", [](Game& g, const uint8_t* p) { scalar_load(g.info.num_nations, p, 4, true); },
		    [](const Game& g, uint8_t* p) { scalar_emit(g.info.num_nations, p, 4); });
		b.c("num_sides", [](Game& g, const uint8_t* p) { scalar_load(g.info.num_sides, p, 4, true); },
		    [](const Game& g, uint8_t* p) { scalar_emit(g.info.num_sides, p, 4); });
		b.f("musical_chairs", &Game::musical_chairs).f("terra_cotta_total", &Game::terra_cotta_total);
		b.f("world_pop", &Game::world_pop).f("world_cities", &Game::world_cities);
		b.f("total_units", &Game::total_units).f("everyone_mask", &Game::everyone_mask).f("armageddon", &Game::armageddon);
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_game_info() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Game> b(m, "GameInfo", "GameInfo");
		b.walks({"GameInfo::walk_data"}).resolve([](Game& g, const Seg&) { return &g; });
		b.c("seed", [](Game& g, const uint8_t* p) { scalar_load(g.seed, p, 4, false); },
		    [](const Game& g, uint8_t* p) { scalar_emit(g.seed, p, 4); });
		b.c("flags", [](Game& g, const uint8_t* p) { scalar_load(g.info.flags, p, 4, false); },
		    [](const Game& g, uint8_t* p) { scalar_emit(g.info.flags, p, 4); });
#define GI8(NAME, MEMBER)                                                                       \
	b.c(NAME, [](Game& g, const uint8_t* p) { scalar_load(g.info.MEMBER, p, 1, false); },       \
	    [](const Game& g, uint8_t* p) { scalar_emit(g.info.MEMBER, p, 1); })
		GI8("team_style", team_style);
		GI8("map_style", map_style);
		GI8("game_rules", game_rules);
		GI8("difficulty", difficulty);
		GI8("starting_town", starting_town);
		GI8("starting_resources", starting_resources);
		GI8("starting_resources2", starting_resources2);
		GI8("tech_cost", tech_cost);
		GI8("reveal_map", reveal_map);
		GI8("pop_limit", pop_limit);
		GI8("rush_rules", rush_rules);
		GI8("starting_technology", starting_technology);
		GI8("starting_technology2", starting_technology2);
		GI8("ending_technology", ending_technology);
		GI8("elimination", elimination);
		GI8("victory", victory);
		GI8("wonderwin", wonderwin);
		GI8("score_goal", score_goal);
		GI8("popwin", popwin);
		GI8("time_limit", time_limit);
		GI8("chairs", chairs);
		GI8("econwin", econwin);
#undef GI8
		// Player[8] at +0x38, 140 bytes each: flags (u16 +0x30), team (i8 +0x34).
		for (int k = 0; k < 8; ++k) {
			b.raw(0x38 + k * 140 + 0x30, 2, "player.flags", [k](Game& g, const uint8_t* p) { scalar_load(g.info.player_flags[size_t(k)], p, 2, false); },
			      [k](const Game& g, uint8_t* p) { scalar_emit(g.info.player_flags[size_t(k)], p, 2); });
			b.raw(0x38 + k * 140 + 0x34, 1, "player.team", [k](Game& g, const uint8_t* p) { scalar_load(g.info.team[size_t(k)], p, 1, true); },
			      [k](const Game& g, uint8_t* p) { scalar_emit(g.info.team[size_t(k)], p, 1); });
		}
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_world() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Game> b(m, "World", "World");
		b.walks({"World::walk_data"}).resolve([](Game& g, const Seg&) { return &g; });
#define WI(NAME)                                                                                  \
	b.c(#NAME, [](Game& g, const uint8_t* p) { scalar_load(g.world.NAME, p, 4, true); },          \
	    [](const Game& g, uint8_t* p) { scalar_emit(g.world.NAME, p, 4); })
		WI(xs);
		WI(ys);
		WI(size);
		WI(fog_xs);
		WI(fog_ys);
		WI(fog_size);
		WI(tile_xs);
		WI(tile_ys);
		WI(tile_size);
		WI(reg_xs);
		WI(reg_ys);
		WI(reg_size);
		WI(sea_map);
#undef WI
		b.c("land_size", [](Game& g, const uint8_t* p) { scalar_load(g.world_total_land, p, 4, true); },
		    [](const Game& g, uint8_t* p) { scalar_emit(g.world_total_land, p, 4); });
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_wdata() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<WData> b(m, "WData", "WData");
		b.walks({"World::walk_data"}).resolve([](Game& g, const Seg& s) -> WData* {
			return size_t(s.a) < g.world.wdata.size() ? &g.world.wdata[size_t(s.a)] : nullptr;
		});
		b.f("flags", &WData::flags).f("land", &WData::land).f("land_sub", &WData::land_sub).f("region", &WData::region);
		b.f("region2", &WData::region2).f("down", &WData::down).f("down_who", &WData::down_who).f("val", &WData::val);
		b.f("goods", &WData::goods).f("light", &WData::light).f("who", &WData::who).f("who2", &WData::who2);
		b.f("blocked", &WData::blocked).f("bad", &WData::bad).f("solid", &WData::solid).f("was_seen", &WData::was_seen);
		m.finish();
		return m;
	}();
	return m;
}

// The second image of a cell (World::walk_data part 9): the writer-side flag "this cell owns a CollBlock".
const ClassMap& map_wdata_block() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<WData> b(m, "WData.block", "WData");
		b.walks({}).resolve([](Game& g, const Seg& s) -> WData* { return size_t(s.a) < g.world.wdata.size() ? &g.world.wdata[size_t(s.a)] : nullptr; });
		b.locals([](const Game& g, const Seg& s, const char* name, int, int64_t& v) {
			if (std::strcmp(name, "has_block") != 0 || size_t(s.a) >= g.world.wdata.size()) return false;
			v = g.world.wdata[size_t(s.a)].block >= 0 ? 1 : 0;
			return true;
		});
		// the cell's CollBlock is its Deref child (WData +0x18)
		static const int step_block = dw::Description::get().find_step(dw::Description::get().find_function("World::walk_data"), dw::StepKind::Deref, 24, 0);
		b.children([](const Game& g, const Seg& s, int step, int64_t, int64_t, ChildSpec& c) {
			if (step != step_block || size_t(s.a) >= g.world.wdata.size()) return false;
			const int32_t blk = g.world.wdata[size_t(s.a)].block;
			if (blk < 0) return false;
			c.map = &map_collblock();
			c.id = Seg{blk, 0, 0};
			return true;
		});
		m.finish();
		return m;
	}();
	return m;
}

// A cell's CollBlock = BitMask<768>: i32 bits, i32 size, flags, then the bit bytes at +0xc (the walked image).
const ClassMap& map_collblock() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<CollBlock> b(m, "CollBlock", "WData");
		b.walks({}).resolve([](Game& g, const Seg& s) -> CollBlock* {
			return size_t(s.a) < g.world.coll_blocks.size() ? &g.world.coll_blocks[size_t(s.a)] : nullptr;
		});
		const dw::Description& d = dw::Description::get();
		const int fn = d.find_function("World::walk_data");
		const int step = d.find_step(fn, dw::StepKind::BitMask, -1, 0);
		const int64_t at = step >= 0 && d.step_const(step, 0) != INT64_MIN ? d.step_const(step, 0) : 0;
		// BitMask<768>: i32 bits = 0x300, i32 size = 0x60 (constants; a block the engine created has no recorded header)
		b.raw(int32_t(at), 8, "header", [](CollBlock&, const uint8_t*) {}, [](const CollBlock&, uint8_t* p) { put_le(p, 4, 0x300); put_le(p + 4, 4, 0x60); });
		b.raw(int32_t(at + 0xc), CollBlock::kBytes, "bits", [](CollBlock& c, const uint8_t* p) { std::memcpy(c.bytes.data(), p, CollBlock::kBytes); },
		      [](const CollBlock& c, uint8_t* p) { std::memcpy(p, c.bytes.data(), CollBlock::kBytes); });
		m.finish();
		return m;
	}();
	return m;
}

}  // namespace ron::sim::omap
