// Field maps of the rest of the simulation state: goods, items, cities, groups and the small per-player record lists
// (forts, wonders, supplies, caravans, oil wells, heroes, specials, armies, unbuilt sites) (docs/original-save.md).
#include <algorithm>
#include <cstring>

#include "sim/original_map.hpp"

namespace ron::sim::omap {

namespace {

constexpr uint32_t kXor = 0x63637;
constexpr int64_t kPlayerListStride = 0x1c;  // PtrArray<T> lists[p] inside Cities / Forts / ... : one container per player

// ---- goods and items: SubObject images in a global PtrArray ---------------------------------------------------------------

template <bool Goods> GoodObj* good_of(Game& g, const Seg& s) {
	auto& v = Goods ? g.goods : g.items;
	return size_t(s.a) < v.size() ? &v[size_t(s.a)] : nullptr;
}

ClassMap make_good_map(const char* name, const char* cls, GoodObj* (*resolve)(Game&, const Seg&)) {
	ClassMap m;
	Builder<GoodObj> b(m, name, cls);
	b.walks({"Good::walk_data", "Item::walk_data", "SubObject::walk_data"}).resolve(resolve);
	b.f("flags", &GoodObj::flags).f("ever_seen", &GoodObj::ever_seen);
	b.fx("x_internal", &GoodObj::x, kXor).fx("y_internal", &GoodObj::y, kXor);
	b.locals([resolve](const Game& g, const Seg& s, const char* nm, int, int64_t& v) {
		const GoodObj* o = resolve(const_cast<Game&>(g), s);
		if (!o) return false;
		if (!std::strcmp(nm, "must_walk")) { v = (o->type >= 0 || o->alive()) ? 1 : 0; return true; }
		if (!std::strcmp(nm, "type")) { v = o->type >= 0 ? o->type : 0; return true; }
		return false;
	});
	// A slot that was never used (no type, not alive) walks only its flags.
	m.audited = [resolve](const Game& g, const Seg& s, const Entry& e) {
		const GoodObj* o = resolve(const_cast<Game&>(g), s);
		return e.off < 0x10 || (o && (o->type >= 0 || o->alive()));
	};
	m.finish();
	return m;
}

}  // namespace

const ClassMap& map_good() {
	static const ClassMap m = make_good_map("Good", "Good", [](Game& g, const Seg& s) { return good_of<true>(g, s); });
	return m;
}
const ClassMap& map_item() {
	static const ClassMap m = make_good_map("Item", "Item", [](Game& g, const Seg& s) { return good_of<false>(g, s); });
	return m;
}

namespace {

// A global that holds a PtrArray of goods / items.
ClassMap make_array_global(const char* name, const char* fn, const ClassMap& (*child)(), bool goods) {
	ClassMap m;
	m.name = name;
	m.cls = "";
	m.resolve = [](Game& g, const Seg&) -> void* { return &g; };
	const dw::Description& d = dw::Description::get();
	const int step = d.find_step(d.find_function(fn), dw::StepKind::Container);
	m.array_fn = [step, child, goods](const Game& g, const Seg&, int st, int64_t base, ArraySpec& spec) {
		if (st != step || base != 0) return false;
		spec.count = int32_t((goods ? g.goods : g.items).size());
		spec.child = &child();
		spec.child_id = [](int32_t i) { return Seg{i, 0, 0}; };
		spec.grow_only = true;
		return true;
	};
	return m;
}

}  // namespace

const ClassMap& map_goods_array() {
	static const ClassMap m = make_array_global("goods", "PtrArray<Good>::walk_data", &map_good, true);
	return m;
}
const ClassMap& map_items_array() {
	static const ClassMap m = make_array_global("items", "PtrArray<Item>::walk_data", &map_item, false);
	return m;
}

// ---- the per-player record lists --------------------------------------------------------------------------------------

namespace {

// A global of 8 PtrArray containers (lists[p] at base p * 0x1c): `count(game, who)` elements the engine has, bound to `child`.
ClassMap make_player_lists(const char* name, const char* fn, const ClassMap& (*child)(), int32_t (*count)(const Game&, int32_t)) {
	ClassMap m;
	m.name = name;
	m.cls = "";
	m.resolve = [](Game& g, const Seg&) -> void* { return &g; };
	const dw::Description& d = dw::Description::get();
	const int step = d.find_step(d.find_function(fn), dw::StepKind::Container);
	m.array_fn = [step, child, count](const Game& g, const Seg&, int st, int64_t base, ArraySpec& spec) {
		if (st != step || base < 0 || base % kPlayerListStride != 0 || base / kPlayerListStride >= 8) return false;
		const int32_t who = int32_t(base / kPlayerListStride);
		spec.count = count(g, who);
		spec.child = &child();
		spec.child_id = [who](int32_t i) { return Seg{who, i, 0}; };
		spec.grow_only = true;
		return true;
	};
	return m;
}

City* city_of(Game& g, const Seg& s) {
	auto& v = g.cities[size_t(s.a & 7)];
	return size_t(s.b) < v.size() ? &v[size_t(s.b)] : nullptr;
}

}  // namespace

const ClassMap& map_city() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<City> b(m, "City", "City");
		b.walks({"City::walk_data"}).resolve([](Game& g, const Seg& s) { return city_of(g, s); });
		b.fu("city_flags", &City::city_flags).f("city", &City::city).f("o", &City::o).f("reg", &City::reg).f("x", &City::x).f("y", &City::y);
		b.f("attack_stamp", &City::attack_stamp).f("raid_stamp", &City::raid_stamp).f("reduce_stamp", &City::reduce_stamp);
		b.f("capture_stamp", &City::capture_stamp).f("assimilation_timer", &City::assimilation_timer);
		b.f("capture_strength", &City::capture_strength).f("traded_with", &City::traded_with).f("scouted", &City::scouted);
		b.f("in_port", &City::in_port).f("peasant_dist", &City::peasant_dist).f("trade_val", &City::trade_val);
		b.f("conquest_node", &City::conquest_node).f("granary", &City::granary).f("lumber_mill", &City::lumber_mill);
		b.f("smelter", &City::smelter).f("refinery", &City::refinery).f("free", &City::free).f("busy", &City::busy);
		b.f("gatherers", &City::gatherers).f("pop", &City::pop).f("who", &City::who).f("race", &City::race).f("founder", &City::founder);
		b.f("plundered", &City::plundered).f("ocean", &City::ocean).f("land", &City::land).f("filled", &City::filled);
		b.f("bordering", &City::bordering).f("ocean_filled", &City::ocean_filled).f("dock_tile", &City::dock_tile);
		b.f("was_capital_flags", &City::was_capital_flags).f("space", &City::space).f("ter", &City::ter);
		// vans: Array<CaravanLink> (a container at +0x74): element i is vans[i]
		const dw::Description& d = dw::Description::get();
		static const int step_vans = d.find_step(d.find_function("Array<CaravanLink>::walk_data"), dw::StepKind::Container);
		b.arrays([](const Game& g, const Seg& s, int step, int64_t base, ArraySpec& spec) {
			if (step != step_vans || base != 0x74) return false;
			const City* c = city_of(const_cast<Game&>(g), s);
			if (!c) return false;
			spec.count = int32_t(c->vans.size());
			spec.child = &map_caravan_link();
			const int32_t a = s.a, bb = s.b;
			spec.child_id = [a, bb](int32_t i) { return Seg{a, bb, i}; };
			return true;
		});
		m.audited = [](const Game& g, const Seg& s, const Entry& e) {
			const City* c = city_of(const_cast<Game&>(g), s);
			return e.off < 6 || (c && c->valid());
		};
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_caravan_link() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<std::array<int16_t, 2>> b(m, "CaravanLink", "CaravanLink");
		b.walks({"CaravanLink::walk_data"}).resolve([](Game& g, const Seg& s) -> std::array<int16_t, 2>* {
			City* c = city_of(g, s);
			return c && size_t(s.c) < c->vans.size() ? &c->vans[size_t(s.c)] : nullptr;
		});
		b.raw(0, 4, "cara", [](std::array<int16_t, 2>& a, const uint8_t* p) { scalar_load(a[0], p, 4, true); },
		      [](const std::array<int16_t, 2>& a, uint8_t* p) { scalar_emit(a[0], p, 4); });
		b.raw(4, 4, "who", [](std::array<int16_t, 2>& a, const uint8_t* p) { scalar_load(a[1], p, 4, true); },
		      [](const std::array<int16_t, 2>& a, uint8_t* p) { scalar_emit(a[1], p, 4); });
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_cities_lists() {
	static const ClassMap m = make_player_lists("cities", "PtrArray<City>::walk_data", &map_city,
	                                            [](const Game& g, int32_t who) { return int32_t(g.cities[size_t(who)].size()); });
	return m;
}

namespace {
Fort* fort_of(Game& g, const Seg& s) { auto& v = g.forts[size_t(s.a & 7)]; return size_t(s.b) < v.size() ? &v[size_t(s.b)] : nullptr; }
Wonder* wonder_of(Game& g, const Seg& s) { auto& v = g.wonder_records[size_t(s.a & 7)]; return size_t(s.b) < v.size() ? &v[size_t(s.b)] : nullptr; }
Game::Supply* supply_of(Game& g, const Seg& s) { auto& v = g.supplies[size_t(s.a & 7)]; return size_t(s.b) < v.size() ? &v[size_t(s.b)] : nullptr; }
CaravanRec* caravan_of(Game& g, const Seg& s) { auto& v = g.caravans[size_t(s.a & 7)]; return size_t(s.b) < v.size() ? &v[size_t(s.b)] : nullptr; }
Army* army_of(Game& g, const Seg& s) { return s.b >= 0 && s.b < 16 ? &g.armies[size_t(s.a & 7)][size_t(s.b)] : nullptr; }
}  // namespace

const ClassMap& map_fort() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Fort> b(m, "Fort", "Fort");
		b.walks({"Fort::walk_data"}).resolve([](Game& g, const Seg& s) { return fort_of(g, s); });
		b.f("fort", &Fort::fort).f("o", &Fort::o).f("reg", &Fort::reg).f("fort_flags", &Fort::fort_flags).f("who", &Fort::who);
		m.finish();
		return m;
	}();
	return m;
}
const ClassMap& map_forts_lists() {
	static const ClassMap m = make_player_lists("forts", "PtrArray<Fort>::walk_data", &map_fort,
	                                            [](const Game& g, int32_t who) { return int32_t(g.forts[size_t(who)].size()); });
	return m;
}

const ClassMap& map_wonder() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Wonder> b(m, "Wonder", "Wonder");
		b.walks({"Wonder::walk_data"}).resolve([](Game& g, const Seg& s) { return wonder_of(g, s); });
		b.f("wonder", &Wonder::wonder).f("o", &Wonder::o).f("stamp", &Wonder::stamp).f("timer", &Wonder::timer);
		b.f("wonder_flags", &Wonder::wonder_flags).f("who", &Wonder::who);
		m.finish();
		return m;
	}();
	return m;
}
const ClassMap& map_wonders_lists() {
	static const ClassMap m = make_player_lists("wonders", "PtrArray<Wonder>::walk_data", &map_wonder,
	                                            [](const Game& g, int32_t who) { return int32_t(g.wonder_records[size_t(who)].size()); });
	return m;
}

const ClassMap& map_supply() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Game::Supply> b(m, "Supply", "Supply");
		b.walks({"Supply::walk_data"}).resolve([](Game& g, const Seg& s) { return supply_of(g, s); });
		b.f("supply", &Game::Supply::o).f("o", &Game::Supply::unit).f("supply_flags", &Game::Supply::flags).f("who", &Game::Supply::who);
		m.finish();
		return m;
	}();
	return m;
}
const ClassMap& map_supplies_lists() {
	static const ClassMap m = make_player_lists("supplies", "PtrArray<Supply>::walk_data", &map_supply,
	                                            [](const Game& g, int32_t who) { return int32_t(g.supplies[size_t(who)].size()); });
	return m;
}

const ClassMap& map_caravan() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<CaravanRec> b(m, "Caravan", "Caravan");
		b.walks({"Caravan::walk_data"}).resolve([](Game& g, const Seg& s) { return caravan_of(g, s); });
		b.f("city2", &CaravanRec::city2).f("whom", &CaravanRec::whom).f("city3", &CaravanRec::city3).f("whose", &CaravanRec::whose);
		b.f("cara", &CaravanRec::cara).f("o", &CaravanRec::o).f("caravan_flags", &CaravanRec::caravan_flags).f("who", &CaravanRec::who);
		m.finish();
		return m;
	}();
	return m;
}
const ClassMap& map_caravans_lists() {
	static const ClassMap m = make_player_lists("caravans", "PtrArray<Caravan>::walk_data", &map_caravan,
	                                            [](const Game& g, int32_t who) { return int32_t(g.caravans[size_t(who)].size()); });
	return m;
}

const ClassMap& map_army() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Army> b(m, "Army", "Army");
		b.walks({"Army::walk_data"}).resolve([](Game& g, const Seg& s) { return army_of(g, s); });
		b.f("valid", &Army::valid).f("army", &Army::army).f("status", &Army::status).f("reg", &Army::reg).f("role", &Army::role);
		b.f("num_units", &Army::num_units).f("num_captains", &Army::num_captains).f("num_standard", &Army::num_standard);
		b.f("num_decoys", &Army::num_decoys).f("city", &Army::city).f("navy", &Army::navy).f("human_frame", &Army::human_frame);
		b.f("hurry", &Army::hurry).f("target_o", &Army::target_o).f("target_who", &Army::target_who).f("x", &Army::x).f("y", &Army::y);
		b.f("angle", &Army::angle).f("rally_dist", &Army::rally_dist).f("muster_x", &Army::muster_x).f("muster_y", &Army::muster_y);
		b.f("muster_angle", &Army::muster_angle).f("list", &Army::list).f("who", &Army::who).f("num_groups", &Army::num_groups);
		m.audited = [](const Game& g, const Seg& s, const Entry& e) {
			const Army* a = army_of(const_cast<Game&>(g), s);
			return e.off < 2 || (a && a->valid);  // an invalid army walks only its valid flag
		};
		m.finish();
		return m;
	}();
	return m;
}
const ClassMap& map_armies_lists() {
	static const ClassMap m = make_player_lists("armies", "PtrArray<Army>::walk_data", &map_army, [](const Game&, int32_t) { return 16; });
	return m;
}

// ---- groups -------------------------------------------------------------------------------------------------------------

Group* group_of(Game& g, const Seg& s) { return s.a >= 0 && s.a < kNumGroups ? &g.groups.g[size_t(s.a)] : nullptr; }

const ClassMap& map_group() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Group> b(m, "Group", "Group");
		b.walks({"Group::walk_data"}).resolve([](Game& g, const Seg& s) { return group_of(g, s); });
		b.f("id", &Group::id).f("army", &Group::army).f("num", &Group::num).f("form", &Group::form).f("stamp", &Group::stamp);
		b.f("ox", &Group::ox).f("oy", &Group::oy).f("o_dist", &Group::o_dist).f("o_angle", &Group::o_angle).f("disband", &Group::disband);
		b.f("order_num", &Group::order_num).f("priority", &Group::priority).f("role", &Group::role).f("think_frame", &Group::think_frame);
		b.f("new_speed", &Group::new_speed).f("speed", &Group::speed).f("form_num", &Group::form_num).f("facing", &Group::facing);
		b.f("buildings", &Group::buildings).f("who", &Group::who).f("march", &Group::march);
		b.f("off_x", &Group::off_x).f("off_y", &Group::off_y).f("curr_x", &Group::curr_x).f("curr_y", &Group::curr_y);
		b.f("angles", &Group::angles).f("list", &Group::list);
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_groups_global() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Game> b(m, "Groups", "Groups");
		b.walks({}).resolve([](Game& g, const Seg&) { return &g; });
		b.c("last_group", [](Game& g, const uint8_t* p) { arr_load(g.groups.last_group, p, 4); }, [](const Game& g, uint8_t* p) { arr_emit(g.groups.last_group, p, 4); });
		b.c("proc_group", [](Game& g, const uint8_t* p) { scalar_load(g.groups.proc_group, p, 4, true); },
		    [](const Game& g, uint8_t* p) { scalar_emit(g.groups.proc_group, p, 4); });
		const dw::Description& d = dw::Description::get();
		static const int step_groups = d.find_step(d.find_function("Array<Group>::walk_data"), dw::StepKind::Container);
		b.arrays([](const Game&, const Seg&, int step, int64_t base, ArraySpec& spec) {
			if (step != step_groups || base != 0) return false;
			spec.count = kNumGroups;
			spec.child = &map_group();
			spec.child_id = [](int32_t i) { return Seg{i, 0, 0}; };
			spec.grow_only = true;
			return true;
		});
		m.finish();
		return m;
	}();
	return m;
}


// ---- combat objects: the projectile pool and the corpse slots -------------------------------------------------------------

const ClassMap& map_ammo() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Ammo> b(m, "Ammo", "Ammo");
		b.walks({"AmmoData::walk_data"}).resolve([](Game& g, const Seg& s) -> Ammo* { return size_t(s.a) < g.ammo.size() ? &g.ammo[size_t(s.a)] : nullptr; });
		b.f("flags", &Ammo::flags).f("rolling", &Ammo::rolling).f("accuracy", &Ammo::accuracy).f("gpiece", &Ammo::gpiece);
		b.f("sx", &Ammo::sx).f("sy", &Ammo::sy).f("sz", &Ammo::sz).f("ex", &Ammo::ex).f("ey", &Ammo::ey).f("ez", &Ammo::ez);
		b.f("cur_time", &Ammo::cur_time).f("total_time", &Ammo::total_time).f("angle", &Ammo::angle).f("splash_area", &Ammo::splash_area);
		b.f("index", &Ammo::index).f("graph_index", &Ammo::graph_index).f("who", &Ammo::who).f("o", &Ammo::o).f("num_guys", &Ammo::num_guys);
		b.f("whom", &Ammo::whom).f("ox", &Ammo::ox).f("traj", &Ammo::traj).f("v1z", &Ammo::v1z).f("dx", &Ammo::dx);
		b.f("bank_dx", &Ammo::bank_dx).f("bank_dy", &Ammo::bank_dy).f("start_roll_angle", &Ammo::start_roll_angle);
		// A slot that is not in use (flags & 3 == 0) walks only its flags byte.
		m.audited = [](const Game& g, const Seg& s, const Entry& e) {
			return e.off < 5 || (size_t(s.a) < g.ammo.size() && g.ammo[size_t(s.a)].in_use());
		};
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_death() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<DeathObj> b(m, "DeathObj", "DeathObj");
		b.walks({"DeathObjData::walk_data"}).resolve([](Game& g, const Seg& s) -> DeathObj* { return size_t(s.a) < g.deaths.size() ? &g.deaths[size_t(s.a)] : nullptr; });
		b.f("valid", &DeathObj::valid).f("first_frame", &DeathObj::first_frame).f("cur_anim", &DeathObj::anim).f("x", &DeathObj::x);
		b.f("y", &DeathObj::y).f("z", &DeathObj::z).f("who", &DeathObj::who).f("o", &DeathObj::o).f("gpiece", &DeathObj::gpiece);
		b.f("ammo_gpiece", &DeathObj::ammo_piece).f("ammo_angle", &DeathObj::deg).f("new_angle", &DeathObj::angle);
		b.f("cur_frame", &DeathObj::cur_frame).f("skel_gpiece", &DeathObj::skeleton).f("unit_crew", &DeathObj::guy);
		m.audited = [](const Game& g, const Seg& s, const Entry& e) {
			return e.off < 4 || (size_t(s.a) < g.deaths.size() && g.deaths[size_t(s.a)].valid != 0);
		};
		m.finish();
		return m;
	}();
	return m;
}

}  // namespace ron::sim::omap
