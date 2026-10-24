// Loading a save of the original game into the engine's Game (docs/original-save.md; sim/original_save.hpp).
#include "sim/original_save.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "sim/mountains.hpp"
#include "sim/rules.hpp"

namespace ron::sim {

namespace dw = datawalk;

void OriginalShadow::bind(const dw::Rec* rec, const omap::ClassMap& map, int32_t a, int32_t b, int32_t c, int64_t base, int64_t len) {
	omap::Segment s;
	s.map = &map;
	s.base = base;
	s.len = len;
	s.id = omap::Seg{a, b, c};
	segs[rec].push_back(s);
}

void OriginalShadow::rebind(const dw::Rec* rec, const omap::ClassMap& map, const omap::Seg& id, int64_t base, int64_t len) {
	std::vector<omap::Segment>& v = segs[rec];
	for (omap::Segment& s : v) {
		if (s.map == &map && s.base == base) {
			s.id = id;
			return;
		}
	}
	omap::Segment s;
	s.map = &map;
	s.base = base;
	s.len = len;
	s.id = id;
	v.push_back(s);
}

dw::Rec* OriginalShadow::fresh_rec(const dw::Rec* parent, int step, int64_t base, int64_t index, const char* cls) {
	auto key = std::make_tuple(parent, step, base, index);
	auto it = fresh_.find(key);
	if (it != fresh_.end()) return it->second;
	dw::Rec* r = tree->new_rec(cls);
	fresh_[key] = r;
	return r;
}

namespace {

struct Loader {
	Loader(Game& game, OriginalShadow& shadow, OriginalLoadReport& report, const GameFiles& game_files)
		: files(game_files), g(game), sh(shadow), rep(report), tree(*shadow.tree), d(dw::Description::get()) {}

	const GameFiles& files;
	Game& g;
	OriginalShadow& sh;
	OriginalLoadReport& rep;
	dw::Tree& tree;
	const dw::Description& d;

	void note(const std::string& s) { rep.notes.push_back(s); }
	const dw::Rec& global(const char* name) {
		const int gid = d.find_global(name);
		const dw::Rec* r = gid >= 0 ? tree.find_global(gid) : nullptr;
		if (!r) throw std::runtime_error(std::string("save has no global ") + name);
		return *r;
	}
	dw::Rec& global_rw(const char* name) { return const_cast<dw::Rec&>(global(name)); }

	// Binds `map` to an image and loads the engine object from it.
	void bind_load(const dw::Rec& rec, const omap::ClassMap& map, int32_t a = 0, int32_t b = 0, int32_t c = 0, int64_t base = 0, int64_t len = -1) {
		sh.bind(&rec, map, a, b, c, base, len);
		void* obj = map.resolve ? map.resolve(g, omap::Seg{a, b, c}) : nullptr;
		if (obj) map.load_from(obj, rec, base);
	}

	// The k-th child object of `rec` created by step `step`, in event order, as (index, Rec).
	template <class F> void each_child(const dw::Rec& rec, int step, F f) {
		for (const dw::Event& e : rec.ev)
			if (e.type == 0 && e.step == step && e.child) f(e.value, e.child);
	}

	// The value of the first Local event of `step` in `rec`; false if the walk skipped it (a never-used slot).
	static bool local_event(const dw::Rec& rec, int step, int64_t& v) {
		for (const dw::Event& e : rec.ev)
			if (e.type == 1 && e.step == step) { v = e.value; return true; }
		return false;
	}

	// ---- objects: the per-player lists of units and buildings
	void load_objects() {
		const dw::Rec& objs = global("objects");
		bind_load(objs, omap::map_objects_header());
		const int fn_list = d.find_function("MultiPtrArray<Object>::walk_data");
		const int step_lists = d.find_step(fn_list, dw::StepKind::Container);
		const int fn_sub = d.find_function("SubObject::walk_data");
		const int step_type = d.find_step(fn_sub, dw::StepKind::Local);
		sh.unit_recs.assign(size_t(kNumLeaders), {});
		sh.build_recs.assign(size_t(kNumLeaders), {});
		int who = 0;
		for (const dw::Event& e : objs.ev) {
			if (e.type != 2 || e.step != step_lists || !e.cd) continue;
			if (who >= 9) break;
			load_list(who, *e.cd, step_type);
			++who;
		}
		// Engine-only link flag: an object is "linked" when it is reachable from a world cell's per-wcoord list.
		for (const WData& w : g.world.wdata) {
			int32_t o = w.down, ow = w.down_who;
			for (int guard = 0; o >= 0 && guard < 100000; ++guard) {
				Object* ob = g.object(ow, o);
				if (!ob) break;
				ob->linked = true;
				o = ob->down;
				ow = ob->down_who;
			}
		}
	}

	// ---- regions, the scalar globals and the records only the engine's simulation reads
	void load_state() {
		// regions
		{
			const dw::Rec& r = global("regions");
			sh.bind(&r, omap::map_regions_global());
			omap::map_regions_global().load_from(&g, r, 0);
			static const int step_coords = container_step("Array<WCoordData>::walk_data");
			each_container(r, container_step("ObjectArray<Region>::walk_data"), [&](int, const dw::ContainerData& cd) {
				for (size_t i = 0; i < cd.elems.size() && i < size_t(Regions::kNum); ++i) {
					const dw::Rec* rr = cd.elems[i];
					if (!rr) continue;
					Region& reg = g.regions.r[i];
					sh.bind(rr, omap::map_region(), int32_t(i));
					omap::map_region().load_from(&reg, *rr, 0);
					for (const dw::Event& e : rr->ev) {
						if (e.type != 2 || e.step != step_coords || !e.cd) continue;
						reg.coords.assign(e.cd->elems.size(), WCoord());
						for (size_t k = 0; k < e.cd->elems.size(); ++k) {
							if (!e.cd->elems[k]) continue;
							sh.bind(e.cd->elems[k], omap::map_wcoord(), int32_t(i), int32_t(k));
							omap::map_wcoord().load_from(&reg.coords[k], *e.cd->elems[k], 0);
						}
					}
				}
			});
		}
		// the terrain height field: Terrain::master_land_heights, (4 * xs + 1) x (4 * ys + 1) floats
		each_container(global("terrain"), container_step("SimpleArray<float>::walk_data"), [&](int, const dw::ContainerData& cd) {
			g.heights.vxs = 4 * g.world.xs + 1;
			g.heights.vys = 4 * g.world.ys + 1;
			g.heights.h.assign(size_t(g.heights.vxs) * size_t(g.heights.vys), 0.0f);
			if (cd.data.size() == g.heights.h.size() * 4) std::memcpy(g.heights.h.data(), cd.data.data(), cd.data.size());
			else note("terrain height field has " + std::to_string(cd.data.size() / 4) + " samples, expected " + std::to_string(g.heights.h.size()));
		});
		// mountain ranges: the stamps (wcoord, template index) are saved, the template footprints come from the install
		{
			const dw::Rec& mr = global("mountains");
			std::vector<std::vector<uint8_t>> blocks;
			static const int step_wc = container_step("SimpleArray<WCoord>::walk_data");
			static const int step_int = container_step("SimpleArray<int>::walk_data");
			for (const dw::Event& e : mr.ev)
				if (e.type == 2 && e.cd && (e.step == step_wc || e.step == step_int)) blocks.push_back(e.cd->data);
			if (blocks.size() >= 3) {
				try {
					MountainTemplates templates(files);
					g.mountains.clear();
					const size_t n = blocks[0].size() / 4;
					for (size_t i = 0; i < n && i < blocks[1].size() / 4 && i < blocks[2].size() / 4; ++i) {
						PlacedRange pr;
						pr.wx = int32_t(omap::get_le(blocks[0].data() + 4 * i, 4));
						pr.wy = int32_t(omap::get_le(blocks[1].data() + 4 * i, 4));
						pr.range = int32_t(omap::get_le(blocks[2].data() + 4 * i, 4));
						if (const MountainRange* r = templates.get(pr.range)) {
							pr.solid = r->wcoords;
							pr.tiles = r->tcoords;
							pr.tcoord_count = int32_t(r->tcoords.size());
						} else {
							note("mountain template " + std::to_string(pr.range) + " is not in the install");
						}
						g.mountains.push_back(std::move(pr));
					}
				} catch (const std::exception& ex) {
					note(std::string("mountain templates: ") + ex.what());
				}
			}
		}
		// scalar globals
		struct GlobalMap { const char* name; const omap::ClassMap* map; };
		for (const GlobalMap& gm : {GlobalMap{"game_random", &omap::map_game_random()}, GlobalMap{"ai_speed", &omap::map_ai_speed()},
		                            GlobalMap{"ai_off", &omap::map_ai_off()}, GlobalMap{"armies_find_dist", &omap::map_armies_find_dist()},
		                            GlobalMap{"build_queue_fail", &omap::map_queue_fail()}, GlobalMap{"game_daemon", &omap::map_game_daemon()}}) {
			const dw::Rec& r = global(gm.name);
			bind_load(r, *gm.map);
		}
		bind_load(global("leader_options"), omap::map_leader_options());
		// constants: the engine's come from the rules tables; bound for the checksum, compared by the rules check
		sh.bind(&global("constants"), omap::map_constants());
		// farms (wheat fields and pens)
		{
			const dw::Rec& r = global("farms");
			each_container(r, container_step("Array<FarmStruct>::walk_data"), [&](int, const dw::ContainerData& cd) {
				g.farms.assign(cd.elems.size(), FarmRec());
				for (size_t i = 0; i < cd.elems.size(); ++i) {
					const dw::Rec* f = cd.elems[i];
					if (!f) continue;
					FarmRec& fr = g.farms[i];
					fr.who = int32_t(f->read_int(0, 4, true));
					fr.o = int32_t(f->read_int(4, 4, true));
					for (int x = 0; x < 4; ++x)
						for (int y = 0; y < 4; ++y) {
							uint8_t b4[4];
							f->read(8 + 4 * (x * 4 + y), b4, 4);
							std::memcpy(&fr.percent[size_t(x)][size_t(y)], b4, 4);
							fr.status[size_t(x)][size_t(y)] = uint8_t(f->read_int(0xac + x * 4 + y, 1, false));
						}
					fr.valid = f->read_int(0xbc, 1, false) != 0;
					fr.farm_type = uint8_t(f->read_int(0xbd, 1, false));
				}
			});
		}
		// unbuilt sites (UnbuiltCity / Fort / Wonder: o, who)
		{
			struct U { const char* global; const char* fn; std::array<std::vector<Unbuilt>, 8>* dst; };
			for (const U& u : {U{"unbuilt_cities", "Array<UnbuiltCity>::walk_data", &g.unbuilt_cities}, U{"unbuilt_forts", "Array<UnbuiltFort>::walk_data", &g.unbuilt_forts},
			                   U{"unbuilt_wonders", "Array<UnbuiltWonder>::walk_data", &g.unbuilt_wonders}}) {
				each_container(global(u.global), container_step(u.fn), [&](int who, const dw::ContainerData& cd) {
					if (who >= 8) return;
					auto& v = (*u.dst)[size_t(who)];
					v.assign(cd.elems.size(), Unbuilt());
					for (size_t i = 0; i < cd.elems.size(); ++i)
						if (cd.elems[i]) { v[i].o = int16_t(cd.elems[i]->read_int(0, 2, true)); v[i].who = int8_t(cd.elems[i]->read_int(2, 1, true)); }
				});
			}
		}
		// oil wells: the engine keeps each record's object slot, the mark is the vector size
		{
			int who_mark[8];
			for (int w = 0; w < 8; ++w) who_mark[w] = int(global("leaders").read_int(int64_t(w) * 0x6eec + 0x430, 4, true));
			each_container(global("oil_wells"), container_step("PtrArray<OilWell>::walk_data"), [&](int who, const dw::ContainerData& cd) {
				if (who >= 8) return;
				auto& v = g.leaders[size_t(who)].oil_wells;
				v.assign(size_t(std::max(0, std::min<int>(who_mark[who], int(cd.elems.size())))), -1);
				for (size_t i = 0; i < v.size(); ++i)
					if (cd.elems[i] && cd.present[i]) v[i] = int16_t(cd.elems[i]->read_int(2, 2, true));
			});
		}
		// heroes and specials: the records of each leader with their running powers
		for (int pass = 0; pass < 2; ++pass) {
			const bool specials = pass == 1;
			const char* gname = specials ? "specials" : "heroes";
			static const int step_spells = container_step("Array<ActiveSpell>::walk_data");
			each_container(global(gname), container_step(specials ? "Specials::walk_data" : "Heroes::walk_data"), [&](int who, const dw::ContainerData& cd) {
				if (who >= 8) return;
				Leader& l = g.leaders[size_t(who)];
				auto& v = specials ? l.specials : l.heroes;
				v.assign(cd.elems.size(), Caster());
				for (size_t i = 0; i < cd.elems.size(); ++i) {
					const dw::Rec* hr = cd.elems[i];
					if (!hr) continue;
					Caster& c = v[i];
					c.hero = int16_t(hr->read_int(0x24, 2, true));
					c.o = int16_t(hr->read_int(0x26, 2, true));
					c.hero_flags = uint8_t(hr->read_int(0x28, 1, false));
					c.who = int8_t(hr->read_int(0x29, 1, true));
					for (const dw::Event& e : hr->ev) {
						if (e.type != 2 || e.step != step_spells || !e.cd) continue;
						for (const dw::Rec* sp : e.cd->elems)
							if (sp) c.active_spells.push_back({int32_t(sp->read_int(0, 4, true)), int32_t(sp->read_int(4, 4, true)), int32_t(sp->read_int(8, 4, true))});
					}
				}
			});
		}
	}

	// ---- goods, items, cities, groups and the small per-player record lists
	template <class F> void each_container(const dw::Rec& rec, int step, F f) {
		int k = 0;
		for (const dw::Event& e : rec.ev)
			if (e.type == 2 && e.step == step && e.cd) f(k++, *e.cd);
	}
	int container_step(const char* fn) { return d.find_step(d.find_function(fn), dw::StepKind::Container); }

	// Binds every present element of `cd` to `map` (ids a, i) and loads the engine record through `at(i)`.
	template <class At> void bind_elements(const dw::ContainerData& cd, const omap::ClassMap& map, int32_t a, At at) {
		for (size_t i = 0; i < cd.elems.size(); ++i) {
			if (!cd.elems[i] || !cd.present[i]) continue;
			sh.bind(cd.elems[i], map, a, int32_t(i));
			if (void* obj = at(i)) map.load_from(obj, *cd.elems[i], 0);
		}
	}

	void load_lists() {
		// goods / items
		for (int pass = 0; pass < 2; ++pass) {
			const bool goods = pass == 0;
			const dw::Rec& r = global(goods ? "goods" : "items");
			sh.bind(&r, goods ? omap::map_goods_array() : omap::map_items_array());
			auto& v = goods ? g.goods : g.items;
			each_container(r, container_step(goods ? "PtrArray<Good>::walk_data" : "PtrArray<Item>::walk_data"), [&](int, const dw::ContainerData& cd) {
				v.assign(size_t(cd.length), GoodObj());
				for (size_t i = 0; i < cd.elems.size() && i < v.size(); ++i) {
					if (!cd.elems[i] || !cd.present[i]) continue;
					int64_t type = 0;
					static const int step_type = d.find_step(d.find_function("SubObject::walk_data"), dw::StepKind::Local);
					const omap::ClassMap& m = goods ? omap::map_good() : omap::map_item();
					sh.bind(cd.elems[i], m, int32_t(i));
					if (local_event(*cd.elems[i], step_type, type)) {
						v[i].type = int32_t(type);
						m.load_from(&v[i], *cd.elems[i], 0);
					}
				}
			});
		}
		g.good_mark = std::min<int32_t>(g.good_mark, int32_t(g.goods.size()));
		// cities (+ caravan links)
		{
			const dw::Rec& r = global("cities");
			sh.bind(&r, omap::map_cities_lists());
			static const int step_vans = container_step("Array<CaravanLink>::walk_data");
			each_container(r, container_step("PtrArray<City>::walk_data"), [&](int who, const dw::ContainerData& cd) {
				if (who >= 8) return;
				auto& v = g.cities[size_t(who)];
				v.assign(size_t(cd.length), City());
				for (size_t i = 0; i < cd.elems.size() && i < v.size(); ++i) {
					const dw::Rec* cr = cd.elems[i];
					if (!cr || !cd.present[i]) continue;
					sh.bind(cr, omap::map_city(), who, int32_t(i));
					if (cr->read_int(4, 2, false) & 1) omap::map_city().load_from(&v[i], *cr, 0);
					else v[i].city_flags = uint16_t(cr->read_int(4, 2, false));
					for (const dw::Event& e : cr->ev) {
						if (e.type != 2 || e.step != step_vans || !e.cd) continue;
						v[i].vans.assign(e.cd->elems.size(), {0, 0});
						for (size_t k = 0; k < e.cd->elems.size(); ++k) {
							if (!e.cd->elems[k]) continue;
							sh.bind(e.cd->elems[k], omap::map_caravan_link(), who, int32_t(i), int32_t(k));
							omap::map_caravan_link().load_from(&v[i].vans[k], *e.cd->elems[k], 0);
						}
					}
				}
			});
		}
		// forts, wonders, supplies, caravans
		auto simple = [&](const char* global_name, const char* fn, const omap::ClassMap& lists, const omap::ClassMap& map, auto resize, auto at) {
			const dw::Rec& r = global(global_name);
			sh.bind(&r, lists);
			each_container(r, container_step(fn), [&](int who, const dw::ContainerData& cd) {
				if (who >= 8) return;
				resize(who, size_t(cd.length));
				bind_elements(cd, map, who, [&](size_t i) { return at(who, i); });
			});
		};
		simple("forts", "PtrArray<Fort>::walk_data", omap::map_forts_lists(), omap::map_fort(), [&](int w, size_t n) { g.forts[size_t(w)].assign(n, Fort()); },
		       [&](int w, size_t i) -> void* { return i < g.forts[size_t(w)].size() ? &g.forts[size_t(w)][i] : nullptr; });
		simple("wonders", "PtrArray<Wonder>::walk_data", omap::map_wonders_lists(), omap::map_wonder(), [&](int w, size_t n) { g.wonder_records[size_t(w)].assign(n, Wonder()); },
		       [&](int w, size_t i) -> void* { return i < g.wonder_records[size_t(w)].size() ? &g.wonder_records[size_t(w)][i] : nullptr; });
		simple("supplies", "PtrArray<Supply>::walk_data", omap::map_supplies_lists(), omap::map_supply(), [&](int w, size_t n) { g.supplies[size_t(w)].assign(n, Game::Supply()); },
		       [&](int w, size_t i) -> void* { return i < g.supplies[size_t(w)].size() ? &g.supplies[size_t(w)][i] : nullptr; });
		simple("caravans", "PtrArray<Caravan>::walk_data", omap::map_caravans_lists(), omap::map_caravan(), [&](int w, size_t n) { g.caravans[size_t(w)].assign(n, CaravanRec()); },
		       [&](int w, size_t i) -> void* { return i < g.caravans[size_t(w)].size() ? &g.caravans[size_t(w)][i] : nullptr; });
		// armies (the engine keeps 16 slots per player)
		{
			const dw::Rec& r = global("armies");
			sh.bind(&r, omap::map_armies_lists());
			each_container(r, container_step("PtrArray<Army>::walk_data"), [&](int who, const dw::ContainerData& cd) {
				if (who >= 8) return;
				if (cd.length != 16) note("armies list " + std::to_string(who) + " has " + std::to_string(cd.length) + " slots (the engine keeps 16)");
				for (size_t i = 0; i < cd.elems.size() && i < 16; ++i) {
					const dw::Rec* ar = cd.elems[i];
					if (!ar || !cd.present[i]) continue;
					sh.bind(ar, omap::map_army(), who, int32_t(i));
					Army& a = g.armies[size_t(who)][i];
					if (ar->read_int(0, 2, true) != 0) omap::map_army().load_from(&a, *ar, 0);
					else a.valid = 0;
				}
			});
		}
		// ammo pool and corpse slots (children of the objects global)
		{
			const dw::Rec& r = global("objects");
			each_container(r, container_step("PtrArray<Ammo>::walk_data"), [&](int, const dw::ContainerData& cd) {
				g.ammo.assign(size_t(cd.length), Ammo());
				for (size_t i = 0; i < cd.elems.size() && i < g.ammo.size(); ++i) {
					if (!cd.elems[i] || !cd.present[i]) continue;
					sh.bind(cd.elems[i], omap::map_ammo(), int32_t(i));
					if (cd.elems[i]->read_int(4, 1, false) & 3) omap::map_ammo().load_from(&g.ammo[i], *cd.elems[i], 0);
					else g.ammo[i].flags = uint8_t(cd.elems[i]->read_int(4, 1, false));
				}
			});
			each_container(r, container_step("ObjectArray<DeathObj>::walk_data"), [&](int, const dw::ContainerData& cd) {
				for (size_t i = 0; i < cd.elems.size() && i < g.deaths.size(); ++i) {
					if (!cd.elems[i]) continue;
					sh.bind(cd.elems[i], omap::map_death(), int32_t(i));
					if (cd.elems[i]->read_int(0, 4, true) != 0) omap::map_death().load_from(&g.deaths[i], *cd.elems[i], 0);
					else g.deaths[i].valid = 0;
				}
			});
		}
		// groups
		{
			const dw::Rec& r = global("groups");
			sh.bind(&r, omap::map_groups_global());
			omap::map_groups_global().load_from(&g, r, 0);
			each_container(r, container_step("Array<Group>::walk_data"), [&](int, const dw::ContainerData& cd) {
				if (cd.length > kNumGroups) note("more groups than the engine's " + std::to_string(kNumGroups));
				for (size_t i = 0; i < cd.elems.size() && i < size_t(kNumGroups); ++i) {
					if (!cd.elems[i]) continue;
					sh.bind(cd.elems[i], omap::map_group(), int32_t(i));
					omap::map_group().load_from(&g.groups.g[i], *cd.elems[i], 0);
				}
			});
		}
	}

	// ---- leaders
	void load_leaders() {
		const dw::Rec& lr = global("leaders");
		const int fn_leader = d.find_function("LeaderData::walk_data");
		const int step_enc = d.find_step(fn_leader, dw::StepKind::Deref, 0x6eb8, 0);
		std::vector<int> active;
		for (int who = 0; who < 9; ++who) {
			const int64_t base = int64_t(who) * 0x6eec;
			sh.bind(&lr, omap::map_leader(), who, 0, 0, base, 0x6eec);
			Leader& l = g.leaders[size_t(who)];
			if (lr.read_int(base, 4, false) & 1) {
				omap::map_leader().load_from(&l, lr, base);
				active.push_back(who);
			} else {
				uint8_t w[8];
				lr.read(base, w, 8);
				scalar_load_flags(l, w);
			}
			++rep.leaders;
		}
		// containers of each active leader, in walk order: sites, make_list, mil_trainers, new_rares, oil_patches, prod_script
		{
			const int step_sites = container_step("Array<Site>::walk_data"), step_make = container_step("Array<MakeObject>::walk_data");
			const int step_simple = container_step("SimpleArray<int>::walk_data");
			int fn_ld = d.find_function("LeaderData::walk_data");
			size_t ai = 0;     // index into `active`
			int simple_k = 0;  // which SimpleArray of the leader
			Leader* cur = nullptr;
			for (const dw::Event& e : lr.ev) {
				if (e.type == 2 && e.cd && e.step == step_sites) {
					if (ai >= active.size()) break;
					cur = &g.leaders[size_t(active[ai])];
					sh.leader_ai[active[ai]] = !e.cd->elems.empty();
					simple_k = 0;
					for (size_t i = 0; i < e.cd->elems.size() && i < cur->sites.size(); ++i) {
						if (!e.cd->elems[i]) continue;
						sh.bind(e.cd->elems[i], omap::map_site(), active[ai], int32_t(i));
						omap::map_site().load_from(&cur->sites[i], *e.cd->elems[i], 0);
					}
				} else if (cur && e.type == 2 && e.cd && e.step == step_make) {
					for (size_t i = 0; i < e.cd->elems.size() && i < cur->make_list.size(); ++i) {
						if (!e.cd->elems[i]) continue;
						sh.bind(e.cd->elems[i], omap::map_make_object(), active[ai], int32_t(i));
						omap::map_make_object().load_from(&cur->make_list[i], *e.cd->elems[i], 0);
					}
				} else if (cur && e.type == 2 && e.cd && e.step == step_simple) {
					std::vector<int32_t>& v = simple_k == 0 ? cur->mil_trainers : simple_k == 1 ? cur->new_rares : cur->oil_patches;
					v.assign(size_t(e.cd->length), 0);
					for (size_t i = 0; i < v.size() && 4 * i + 4 <= e.cd->data.size(); ++i) v[i] = int32_t(omap::get_le(e.cd->data.data() + 4 * i, 4));
					if (++simple_k == 3) { }
				} else if (cur && e.type == 2 && e.cd && d.step_kind(e.step) == dw::StepKind::String && d.step_function(e.step) == fn_ld) {
					cur->prod_script.clear();
					for (size_t i = 0; i + 1 < e.cd->data.size(); i += 2) cur->prod_script.push_back(char(e.cd->data[i]));
					++ai;  // the string closes the leader's block
					cur = nullptr;
				}
			}
		}
		size_t k = 0;
		each_child(lr, step_enc, [&](int64_t, dw::Rec* r) {
			if (k >= active.size()) return;
			const int who = active[k++];
			sh.bind(r, omap::map_leader_encrypt(), who);
			EncryptLoader(g.leaders[size_t(who)], *r, d).run();
		});
	}
	static void scalar_load_flags(Leader& l, const uint8_t* w) {
		l.flags = uint32_t(w[0] | w[1] << 8 | w[2] << 16 | uint32_t(w[3]) << 24);
		l.flags2 = uint32_t(w[4] | w[5] << 8 | w[6] << 16 | uint32_t(w[7]) << 24);
	}
	// Reads the LeaderDataEncrypt locals of a leader's child image into the engine (the inverse of map_leader_encrypt's locals).
	struct EncryptLoader {
		EncryptLoader(Leader& leader, const dw::Rec& rec, const dw::Description& desc) : l(leader), r(rec), d(desc) {}
		Leader& l;
		const dw::Rec& r;
		const dw::Description& d;
		void run() {
			std::map<std::string, int> seen;
			for (const dw::Event& e : r.ev) {
				if (e.type != 1) continue;
				const std::string name = d.local_name(e.step);
				const int k = seen[name]++;
				const int32_t v = int32_t(e.value);
				auto set6 = [&](std::array<int32_t, 6>& a) { if (k < 6) a[size_t(k)] = v; };
				if (name == "bucket") set6(l.bucket);
				else if (name == "leftover") set6(l.leftover);
				else if (name == "resource_cap") { if (k < 6) l.resource_cap[size_t(k)] = v; }
				else if (name == "over_cap") set6(l.over_cap);
				else if (name == "resources") set6(l.resources);
				else if (name == "support") set6(l.support);
				else if (name == "income") set6(l.income);
				else if (name == "rate") set6(l.rate);
				else if (name == "bonus") set6(l.bonus);
				else if (name == "resource_cap6") l.resource_cap[6] = v;
				else if (name == "epoch") { if (k < 4) l.epoch[size_t(k)] = v; }
				else if (name == "ages") l.age = v;
				else if (name == "epochs") l.epochs = v;
				else if (name == "discovered") l.discovered = v;
			}
		}
	};

	void load_list(int who, const dw::ContainerData& cd, int step_type) {
		ObjectList& L = g.objects[size_t(who)];
		auto& urecs = sh.unit_recs[size_t(who)];
		auto& brecs = sh.build_recs[size_t(who)];
		urecs.assign(size_t(std::max(L.unit_mark, 0)), nullptr);
		brecs.assign(size_t(std::max(L.build_mark - kBuildBase, 0)), nullptr);
		L.units.resize(urecs.size());
		L.builds.resize(brecs.size());
		for (size_t i = 0; i < cd.present.size(); ++i) {
			if (!cd.present[i] || !cd.elems[i]) continue;
			dw::Rec* r = cd.elems[i];
			const int32_t t = cd.types[i];
			int64_t type = 0;
			const bool walked = local_event(*r, step_type, type);
			if (t == 0 || t == 3) {
				if (i >= urecs.size()) { ++skipped_slots; continue; }
				auto u = std::make_unique<Unit>();
				u->who = int8_t(who);
				u->o = int16_t(i);
				if (walked && type >= 0 && type < kNumTypes) {
					u->type = int32_t(type);
					u->ptype = &g.rules->types[size_t(type)];
					omap::map_unit().load_from(u.get(), *r, 0);
					if (t == 3) omap::map_animal().load_from(u.get(), *r, 0);
				}
				if (walked) load_unit_children(*u, *r);
				urecs[i] = r;
				sh.bind(r, omap::map_unit(), who, int32_t(i));
				if (t == 3) sh.bind(r, omap::map_animal(), who, int32_t(i));
				L.units[i] = std::move(u);
				++rep.units;
			} else if (t == 1) {
				if (i < size_t(kBuildBase) || i - kBuildBase >= brecs.size()) { ++skipped_slots; continue; }
				auto b = std::make_unique<Build>();
				b->is_unit = false;
				b->who = int8_t(who);
				b->o = int16_t(i);
				if (walked && type >= 0 && type < kNumTypes) {
					b->type = int32_t(type);
					b->ptype = &g.rules->types[size_t(type)];
					omap::map_build().load_from(b.get(), *r, 0);
					load_build_children(*b, *r);
				}
				brecs[i - kBuildBase] = r;
				sh.bind(r, omap::map_build(), who, int32_t(i));
				L.builds[i - kBuildBase] = std::move(b);
				++rep.builds;
			} else {
				note("object list " + std::to_string(who) + " slot " + std::to_string(i) + " has data type " + std::to_string(t) + " (walls are not modelled)");
			}
		}
	}
	int64_t skipped_slots = 0;

	// The queue slots, mining list and rally points of a building.
	void load_build_children(Build& bd, const dw::Rec& r) {
		static const int step_queue = d.find_step(d.find_function("BuildQueue::walk_data"), dw::StepKind::Deref);
		static const int step_mining = d.find_step(d.find_function("Array<TCoordData>::walk_data"), dw::StepKind::Container);
		static const int step_gather = d.find_step(d.find_function("PtrLinkListAbstract<GatherPoint,unsigned_char>::walk_data"), dw::StepKind::Container);
		for (const dw::Event& e : r.ev) {
			if (e.type == 0 && e.step == step_queue && e.child) {
				const size_t idx = size_t(e.value);
				if (idx >= bd.build_queue.queue.size()) continue;
				sh.bind(e.child, omap::map_queue_item(), bd.who, bd.o, int32_t(idx));
				omap::map_queue_item().load_from(&bd.build_queue.queue[idx], *e.child, 0);
			} else if (e.type == 2 && e.step == step_mining && e.cd) {
				bd.gather_from.assign(e.cd->elems.size(), {0, 0});
				for (size_t i = 0; i < e.cd->elems.size(); ++i) {
					if (!e.cd->elems[i]) continue;
					sh.bind(e.cd->elems[i], omap::map_tcoord(), bd.who, bd.o, int32_t(i));
					omap::map_tcoord().load_from(&bd.gather_from[i], *e.cd->elems[i], 0);
				}
			} else if (e.type == 2 && e.step == step_gather && e.cd) {
				const size_t n = e.cd->elems.size();
				bd.gather.assign(n, GatherPoint());
				for (size_t i = 0; i < n; ++i) {
					if (!e.cd->elems[i]) continue;
					sh.bind(e.cd->elems[i], omap::map_gather_point(), bd.who, bd.o, int32_t(i));
					omap::map_gather_point().load_from(&bd.gather[n - 1 - i], *e.cd->elems[i], 0);
				}
			}
		}
	}

	// The guys and the path stack of a unit: child images bound one by one.
	void load_unit_children(Unit& u, const dw::Rec& r) {
		static const int step_orders = d.find_step(d.find_function("OrderList::walk_data"), dw::StepKind::Container);
		static const int step_order_type = d.find_step(d.find_function("OrderList::walk_data"), dw::StepKind::Local);
		static const int step_order_data = d.find_step(d.find_function("OrderList::walk_data"), dw::StepKind::Deref);
		static const int step_guys = d.find_step(d.find_function("PtrArray<Guy>::walk_data"), dw::StepKind::Container);
		static const int step_path = d.find_step(d.find_function("Stack<PathData>::walk_data"), dw::StepKind::Deref);
		for (const dw::Event& e : r.ev) {
			if (e.type == 2 && e.step == step_guys && e.cd) {
				u.guys.assign(size_t(e.cd->length), Guy());
				for (size_t i = 0; i < e.cd->elems.size() && i < u.guys.size(); ++i) {
					if (!e.cd->elems[i] || !e.cd->present[i]) continue;
					sh.bind(e.cd->elems[i], omap::map_guy(), u.who, u.o, int32_t(i));
					omap::map_guy().load_from(&u.guys[i], *e.cd->elems[i], 0);
				}
			} else if (e.type == 2 && e.step == step_orders && e.cd) {
				// node i of the file (written tail first) is orders[i]: the current order first
				u.orders.assign(e.cd->elems.size(), Order());
				for (size_t i = 0; i < e.cd->elems.size(); ++i) {
					const dw::Rec* node = e.cd->elems[i];
					int64_t type = 0;
					if (!node || !local_event(*node, step_order_type, type)) continue;
					Order& o = u.orders[i];
					o.type = OrderType(type);
					sh.bind(node, omap::map_order_node(), u.who, u.o, int32_t(i));
					const dw::Rec* data = nullptr;
					for (const dw::Event& ne : node->ev)
						if (ne.type == 0 && ne.step == step_order_data) data = ne.child;
					if (!data) continue;
					const omap::ClassMap& om = omap::map_order(int(type));
					sh.bind(data, om, u.who, u.o, int32_t(i));
					om.load_from(&o, *data, 0);
					if (type == 17) {  // AirPatrolOrder: the waypoints are two SimpleArray<Coord> (x_pos, y_pos) in the order's image
						static const int step_coord = d.find_step(d.find_function("SimpleArray<Coord>::walk_data"), dw::StepKind::Container);
						const dw::ContainerData* xs = nullptr;
						const dw::ContainerData* ys = nullptr;
						for (const dw::Event& ev : data->ev)
							if (ev.type == 2 && ev.step == step_coord && ev.cd) (xs ? ys : xs) = ev.cd;
						if (xs && ys) {
							const size_t n = std::min(xs->data.size(), ys->data.size()) / 4;
							o.wp.assign(n, {0, 0});
							for (size_t k = 0; k < n; ++k)
								o.wp[k] = {int32_t(omap::get_le(xs->data.data() + 4 * k, 4)), int32_t(omap::get_le(ys->data.data() + 4 * k, 4))};
						}
					}
				}
			} else if (e.type == 0 && e.step == step_path && e.child) {
				const size_t idx = size_t(e.value);
				if (u.path.size() <= idx) u.path.resize(idx + 1);
				sh.bind(e.child, omap::map_pathdata(), u.who, u.o, int32_t(idx));
				omap::map_pathdata().load_from(&u.path[idx], *e.child, 0);
			}
		}
	}

	void run() {
		// Game header and GameInfo.
		const dw::Rec& game = global("game");
		bind_load(game, omap::map_game_info(), 0, 0, 0, 0xc);
		bind_load(game, omap::map_game());
		rep.frame = g.frame;

		// World: scalars, then the cells.
		const dw::Rec& world = global("world");
		const int32_t xs = int32_t(world.read_int(0, 4, true)), ys = int32_t(world.read_int(4, 4, true));
		if (xs <= 0 || ys <= 0 || xs > 1024 || ys > 1024) throw std::runtime_error("implausible world size");
		g.world.init(xs, ys);
		bind_load(world, omap::map_world());
		const int fn_world = d.find_function("World::walk_data");
		const int step_cells = d.find_step(fn_world, dw::StepKind::Deref, 308, 0);
		const int step_blocks = d.find_step(fn_world, dw::StepKind::Deref, 308, 1);
		sh.cells.assign(size_t(g.world.size), nullptr);
		sh.cells2.assign(size_t(g.world.size), nullptr);
		each_child(world, step_cells, [&](int64_t i, dw::Rec* r) {
			if (i < 0 || i >= g.world.size) return;
			sh.cells[size_t(i)] = r;
			bind_load(*r, omap::map_wdata(), int32_t(i));
			++rep.cells;
		});
		// The grid buffers (tdata, the three fog bytes per fog cell, wcoord_seen, danger[8]): Buf events in walk order.
		{
			int danger_k = 0;
			for (const dw::Event& e : world.ev) {
				if (e.type != 2 || !e.cd || d.step_kind(e.step) != dw::StepKind::Buf) continue;
				const int64_t ptr = d.step_const(e.step, 0);
				const std::vector<uint8_t>& data = e.cd->data;
				auto fill8 = [&](std::vector<uint8_t>& v) { if (data.size() == v.size()) v = data; else note("world buffer size mismatch"); };
				switch (ptr) {
					case 0x138:
						if (data.size() == g.world.tdata.size() * 2) std::memcpy(g.world.tdata.data(), data.data(), data.size());
						else note("world tdata size mismatch");
						break;
					case 0x15c: fill8(g.world.seen); break;
					case 0x160: fill8(g.world.seen2); break;
					case 0x164: fill8(g.world.seen3); break;
					case 0x168: fill8(g.world.wcoord_seen); break;
					default:
						if (ptr == INT64_MIN && danger_k < 8 && data.size() == size_t(g.world.reg_size) * 4) {
							std::memcpy(g.world.danger.data() + size_t(danger_k) * size_t(g.world.reg_size), data.data(), data.size());
							++danger_k;
						} else {
							note("unexpected world buffer at step " + std::to_string(e.step));
						}
				}
			}
		}
		const int step_has_block = d.find_step(fn_world, dw::StepKind::Local, -1, 0);
		const int step_block = d.find_step(fn_world, dw::StepKind::Deref, 24, 0);  // *block (WData +0x18)
		each_child(world, step_blocks, [&](int64_t i, dw::Rec* r) {
			if (i < 0 || i >= g.world.size) return;
			sh.cells2[size_t(i)] = r;
			sh.bind(r, omap::map_wdata_block(), int32_t(i));
			int64_t has = 0;
			if (!local_event(*r, step_has_block, has) || !has) return;
			// The cell's CollBlock: pool slot in cell order, flags 2 (dirty) as the loader sets them.
			g.world.coll_blocks.emplace_back();
			CollBlock& cb = g.world.coll_blocks.back();
			cb.flags = 2;
			g.world.wdata[size_t(i)].block = int32_t(g.world.coll_blocks.size() - 1);
			each_child(*r, step_block, [&](int64_t, dw::Rec* br) {
				sh.bind(br, omap::map_collblock(), g.world.wdata[size_t(i)].block);
				omap::map_collblock().load_from(&cb, *br, 0);
			});
		});
		load_leaders();
		load_objects();
		load_lists();
		load_state();
		if (skipped_slots) note(std::to_string(skipped_slots) + " object slots lie beyond their list's high mark (preallocated empty objects; shadow only)");
	}
};

}  // namespace

// The save carries the rules tables it was played with (the last block of the walk). When the install's tables write the same
// image they are used as they are; when they differ (the corpus mixes two data revisions, and scripted games edit the rules while
// they run) the engine adopts the save's walked fields over a copy of the install's tables, so the game runs on the original's rules.
static void adopt_rules(Game& game, OriginalShadow& sh, const Rules& rules, const std::vector<uint8_t>& stream, OriginalLoadReport& rep) {
	std::vector<ImageField> layout;
	const Bytes mine = write_image(rules, &layout);
	// Where the block starts: the walk ends with it, but its length depends on the strings of the data revision, so look for the
	// marker (type 0 follows, with a tribe mask) near the install's expectation and accept the start whose decode ends at the walk's end.
	auto i32_at = [&](size_t p) { return int32_t(stream[p] | stream[p + 1] << 8 | stream[p + 2] << 16 | uint32_t(stream[p + 3]) << 24); };
	size_t at = std::string::npos;
	std::unique_ptr<Rules> own;
	if (sh.walk_end >= mine.size() && stream.size() >= sh.walk_end) {
		const size_t guess = sh.walk_end - mine.size();
		for (size_t d = 0; d <= 60000 && at == std::string::npos; ++d) {
			for (int sgn = 0; sgn < 2; ++sgn) {
				if (d == 0 && sgn) continue;
				const size_t p = sgn ? guess + d : (guess >= d ? guess - d : size_t(-1));
				if (p == size_t(-1) || p + 200 > stream.size() || stream[p] != kMarkerGameRules || i32_at(p + 1) != 0 || i32_at(p + 13) == 0) continue;
				auto t = std::make_unique<Rules>(rules);
				try {
					if (read_image_into(stream, p, *t) == sh.walk_end) { at = p; own = std::move(t); break; }
				} catch (const std::exception&) {
				}
			}
		}
	}
	if (at == std::string::npos) {
		rep.rules_note = "the save's rules block was not found at the end of the walk";
		return;
	}
	const size_t save_len = sh.walk_end - at;
	size_t diff = 0;
	while (diff < mine.size() && diff < save_len && mine[diff] == stream[at + diff]) ++diff;
	sh.rules_equal = rep.rules_equal = diff == mine.size() && save_len == mine.size();
	if (sh.rules_equal) return;
	std::string where = "image offset " + std::to_string(diff);
	for (const ImageField& f : layout)
		if (diff >= f.offset && diff < f.offset + f.size) { where = f.where + " " + f.field + (f.index >= 0 ? "[" + std::to_string(f.index) + "]" : ""); break; }
	const Bytes again = write_image(*own);
	size_t n_diff = 0;
	for (size_t i = 0; i < again.size() && i < mine.size(); ++i) n_diff += again[i] != mine[i];
	game.own_rules = std::move(own);
	game.rules = game.own_rules.get();
	game.base_rules = &rules;
	sh.rules_image.assign(stream.begin() + std::ptrdiff_t(at), stream.begin() + std::ptrdiff_t(sh.walk_end));
	rep.rules_note = std::string("the save's rules differ from the install's tables (first difference: ") + where + ", " + std::to_string(n_diff) +
	                 " image bytes, sizes " + std::to_string(save_len) + " vs " + std::to_string(mine.size()) + "); the game runs on the save's own rules" + (again == sh.rules_image ? "" : " (RE-ENCODING DIFFERS)");
}

std::unique_ptr<Game> load_original_save(const GameFiles& files, const Rules& rules, const AnimSource* anims, const std::vector<uint8_t>& stream,
                                         OriginalLoadReport* report) {
	OriginalLoadReport local;
	OriginalLoadReport& rep = report ? *report : local;
	rep = OriginalLoadReport();
	auto shadow = std::make_shared<OriginalShadow>();
	shadow->tree = std::make_unique<dw::Tree>(dw::Description::get());
	dw::SaveDecode sd = dw::decode_save(*shadow->tree, stream.data(), stream.size(), 0);
	if (!sd.result.ok) {
		rep.error = "decode failed at " + std::to_string(sd.result.end) + ": " + sd.result.error;
		return nullptr;
	}
	shadow->magic = sd.magic;
	shadow->version = sd.version;
	shadow->walk_begin = sd.walk_begin;
	shadow->walk_end = sd.end >= 4 * sd.verify.size() ? sd.end - 4 * sd.verify.size() : sd.walk_end;  // the verify block is one u32 per entry
	shadow->verify = sd.verify;

	auto game = std::make_unique<Game>();
	game->rules = &rules;
	game->base_rules = &rules;
	game->anims = anims;
	game->original = shadow;
	try {
		adopt_rules(*game, *shadow, rules, stream, rep);
		Loader L(*game, *shadow, rep, files);
		L.run();
	} catch (const std::exception& e) {
		rep.error = std::string("load failed: ") + e.what();
		return nullptr;
	}
	for (const omap::ClassMap* m : omap::all_maps()) {
		const omap::FieldCount fc = omap::count_fields(*m);
		OriginalLoadReport::ClassCount cc;
		cc.name = m->name;
		cc.cls = m->cls;
		cc.walked = fc.walked;
		cc.mapped = fc.mapped;
		cc.unmapped = fc.unmapped;
		rep.classes.push_back(std::move(cc));
	}
	rep.ok = true;
	return game;
}

std::vector<AuditLine> audit_mapping(Game& game) {
	std::vector<AuditLine> out;
	if (!game.original) return out;
	std::map<std::string, size_t> index;
	std::vector<uint8_t> shadow, emitted;
	for (const auto& kv : game.original->segs) {
		const dw::Rec& rec = *kv.first;
		for (const omap::Segment& seg : kv.second) {
			void* obj = seg.map->resolve ? seg.map->resolve(game, seg.id) : nullptr;
			if (!obj) continue;
			for (const omap::Entry& e : seg.map->entries) {
				if (seg.map->audited && !seg.map->audited(game, seg.id, e)) continue;
				const std::string key = seg.map->name + "." + e.name;
				auto it = index.find(key);
				if (it == index.end()) {
					it = index.emplace(key, out.size()).first;
					out.push_back(AuditLine{key, 0, 0, ""});
				}
				AuditLine& line = out[it->second];
				shadow.resize(size_t(e.size));
				rec.read(seg.base + e.off, shadow.data(), shadow.size());
				emitted = shadow;
				emitted.resize(shadow.size() + 8, 0xa5);  // canary: an emit() that writes past its declared size
				e.emit(obj, emitted.data());
				++line.objects;
				bool canary = true;
				for (size_t k = shadow.size(); k < emitted.size(); ++k) canary = canary && emitted[k] == 0xa5;
				emitted.resize(shadow.size());
				if (!canary) {
					line.member += " [WRITES PAST ITS SIZE]";
					++line.mismatches;
				}
				if (emitted != shadow) {
					if (!line.mismatches++) {
						char b[512];
						std::snprintf(b, sizeof b, "object (%d,%d,%d) +0x%x engine", seg.id.a, seg.id.b, seg.id.c, e.off);
						line.example = b;
						for (size_t k = 0; k < emitted.size() && k < 12; ++k) { std::snprintf(b, sizeof b, " %02x", emitted[k]); line.example += b; }
						line.example += " original";
						for (size_t k = 0; k < shadow.size() && k < 12; ++k) { std::snprintf(b, sizeof b, " %02x", shadow[k]); line.example += b; }
					}
				}
			}
		}
	}
	return out;
}

std::vector<AuditLine> diff_mapped_state(Game& a, Game& b) {
	std::vector<AuditLine> out;
	if (!a.original) return out;
	std::map<std::string, size_t> index;
	std::vector<uint8_t> shadow, ea, eb;
	for (const auto& kv : a.original->segs) {
		const dw::Rec& rec = *kv.first;
		for (const omap::Segment& seg : kv.second) {
			void* oa = seg.map->resolve ? seg.map->resolve(a, seg.id) : nullptr;
			void* ob = seg.map->resolve ? seg.map->resolve(b, seg.id) : nullptr;
			if (!oa && !ob) continue;
			for (const omap::Entry& e : seg.map->entries) {
				const std::string key = seg.map->name + "." + e.name;
				auto it = index.find(key);
				if (it == index.end()) {
					it = index.emplace(key, out.size()).first;
					out.push_back(AuditLine{key, 0, 0, ""});
				}
				AuditLine& line = out[it->second];
				shadow.resize(size_t(e.size));
				rec.read(seg.base + e.off, shadow.data(), shadow.size());
				ea = shadow;
				eb = shadow;
				if (oa) e.emit(oa, ea.data());
				if (ob) e.emit(ob, eb.data());
				++line.objects;
				if (ea != eb && !line.mismatches++) {
					char buf[512];
					std::snprintf(buf, sizeof buf, "object (%d,%d,%d) +0x%x before", seg.id.a, seg.id.b, seg.id.c, e.off);
					line.example = buf;
					for (size_t k = 0; k < ea.size() && k < 8; ++k) { std::snprintf(buf, sizeof buf, " %02x", ea[k]); line.example += buf; }
					line.example += " after";
					for (size_t k = 0; k < eb.size() && k < 8; ++k) { std::snprintf(buf, sizeof buf, " %02x", eb[k]); line.example += buf; }
				}
			}
		}
	}
	return out;
}

int64_t revert_members(Game& to, Game& from, const std::vector<std::string>& members) {
	int64_t n = 0;
	if (!to.original) return 0;
	std::vector<uint8_t> shadow, bytes;
	for (const auto& kv : to.original->segs) {
		const dw::Rec& rec = *kv.first;
		for (const omap::Segment& seg : kv.second) {
			void* ot = seg.map->resolve ? seg.map->resolve(to, seg.id) : nullptr;
			void* of = seg.map->resolve ? seg.map->resolve(from, seg.id) : nullptr;
			if (!ot || !of) continue;
			for (const omap::Entry& e : seg.map->entries) {
				const std::string key = seg.map->name + "." + e.name;
				if (std::find(members.begin(), members.end(), key) == members.end()) continue;
				shadow.resize(size_t(e.size));
				rec.read(seg.base + e.off, shadow.data(), shadow.size());
				bytes = shadow;
				e.emit(of, bytes.data());
				e.load(ot, bytes.data());
				++n;
			}
		}
	}
	return n;
}

}  // namespace ron::sim
