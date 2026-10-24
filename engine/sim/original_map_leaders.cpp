// Field map of LeaderData (0x6eec bytes per leader) and LeaderDataEncrypt (docs/original-save.md).
#include <cstring>

#include "sim/original_map.hpp"
#include "sim/original_save.hpp"

namespace ron::sim::omap {

namespace {

// A BitMask<N> member: the walked image is i32 bits, i32 size, (flags), and `size` bytes at +0xc; the engine keeps the bytes.
template <size_t N>
void bitmask_bytes(Builder<Leader>& b, const char* name, std::array<uint8_t, N> Leader::*mp, int nbytes) {
	const dw::FieldInfo* f = dw::Description::get().find_field("Leader", name);
	if (!f) throw std::logic_error(std::string("Leader.") + name);
	const int n = nbytes < int(N) ? nbytes : int(N);
	b.raw(f->offset + 0xc, nbytes, name, [mp, n](Leader& l, const uint8_t* p) { std::memcpy((l.*mp).data(), p, size_t(n)); },
	      [mp, n](const Leader& l, uint8_t* p) { std::memcpy(p, (l.*mp).data(), size_t(n)); });
}

}  // namespace

const ClassMap& map_leader() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Leader> b(m, "LeaderData", "Leader");
		b.walks({"LeaderData::walk_data"}).resolve([](Game& g, const Seg& s) -> Leader* {
			return s.a >= 0 && s.a < kNumLeaders ? &g.leaders[size_t(s.a)] : nullptr;
		});
		b.f("leader_flags", &Leader::flags).f("leader_flags2", &Leader::flags2);
		b.f("tribe", &Leader::tribe).f("defeated_by", &Leader::defeated_by).f("gov", &Leader::gov).f("score", &Leader::score);
		b.f("score_explored", &Leader::score_explored).f("score_territory", &Leader::score_territory).f("score_units", &Leader::score_units);
		b.f("score_units_2", &Leader::score_attack_units).f("score_buildings", &Leader::score_buildings);
		b.f("score_economy", &Leader::score_economy).f("score_pop", &Leader::score_pop).f("score_unit_upgrades", &Leader::score_upgrades);
		b.f("score_research", &Leader::score_research).f("score_wonders", &Leader::score_wonders).f("score_combat", &Leader::score_combat);
		b.f("handicap", &Leader::handicap).f("multi_diff", &Leader::difficulty);
		b.f("chat_status", &Leader::chat_status);
		b.raw(0x74, 64, "diplos", [](Leader& l, const uint8_t* p) { arr_load(l.diplo, p, 4); }, [](const Leader& l, uint8_t* p) { arr_emit(l.diplo, p, 4); });
		// (`diplos` covers LeaderData.diplos and treaties: the engine keeps both in one contiguous array of 16)
		b.f("agendas", &Leader::agendas).f("good_deeds", &Leader::good_deeds).f("attack_stamp", &Leader::attack_stamp);
		b.f("raid_stamp", &Leader::raid_stamp).f("tribute_stamp", &Leader::tribute_stamp).f("gift_stamp", &Leader::gift_stamp);
		b.f("hire_stamp", &Leader::hire_stamp).f("hire_who", &Leader::hire_who);
		b.f("attrition_stamp", &Leader::attrition_warn_stamp).f("attrition_stamp2", &Leader::attrition_hit_stamp);
		b.f("attrition_stamp3", &Leader::attrition_stamp3).f("repair_stamp", &Leader::repair_stamp).f("blacken", &Leader::blacken);
		b.f("aggression", &Leader::aggression).f("strong", &Leader::strong).f("weak", &Leader::weak).f("dow", &Leader::dow);
		b.f("invaders", &Leader::invaders).f("broke_alliance", &Leader::broke_alliance).f("made_peace", &Leader::made_peace);
		b.f("got_diplo_message", &Leader::got_diplo_message).f("counteroffer", &Leader::counteroffer);
		b.f("tribute_demanded", &Leader::tribute_demanded);
		b.f("city_num", &Leader::city_num).f("village_num", &Leader::village_num).f("city_mine", &Leader::city_mine);
		b.f("village_mine", &Leader::village_mine).f("city_mark", &Leader::city_mark).f("city_name", &Leader::city_name);
		b.f("lost_city_stamp", &Leader::lost_city_stamp).f("lost_capital_stamp", &Leader::lost_capital_stamp);
		b.f("lost_capital_timer", &Leader::lost_capital_timer).f("lost_capital_modifier", &Leader::lost_capital_modifier);
		b.f("hero_mark", &Leader::num_heroes).f("wonder_mark", &Leader::wonder_mark).f("fort_mark", &Leader::fort_mark);
		b.f("supply_mark", &Leader::supply_mark).f("caravan_mark", &Leader::caravan_mark).f("special_mark", &Leader::num_specials);
		// (oil_well_mark is the OilWells record vector's size, owned by the OilWells records)
		b.f("popwin_stamp", &Leader::popwin_stamp).f("popwin_timer", &Leader::popwin_timer).f("wonderwin_stamp", &Leader::wonderwin_stamp);
		b.f("wonderwin_timer", &Leader::wonderwin_timer);
		b.f("econ", &Leader::econ).f("escrow", &Leader::escrow).f("escrow_rate", &Leader::escrow_rate).f("tributes", &Leader::tributes);
		b.f("base_rate", &Leader::base_rate).f("worst_good", &Leader::worst_good).f("best_good", &Leader::best_good);
		b.f("shortages", &Leader::shortages).f("reg_known_rares", &Leader::reg_known_rares).f("reg_unpack_merch", &Leader::reg_unpack_merch);
		b.f("known_rares", &Leader::known_rares).f("rares_collected", &Leader::rares_collected);
		b.f("production_step", &Leader::production_ai).f("prod_script_run", &Leader::prod_script_on).f("script_step", &Leader::prod_step);
		b.f("wonder_mod", &Leader::wonder_mod).f("ground_mod", &Leader::ground_mod).f("air_mod", &Leader::air_mod).f("sea_mod", &Leader::sea_mod);
		b.f("infra_mod", &Leader::infra_mod).f("defense_mod", &Leader::defense_mod).f("gather_stamp", &Leader::gather_stamp);
		b.f("nukes_used", &Leader::nukes_used).f("missiles_used", &Leader::missiles_used).f("tech_frame", &Leader::tech_frame);
		b.f("tech_cat_frame", &Leader::tech_cat_frame).f("victory_type", &Leader::victory_type).f("defeat_type", &Leader::defeat_type);
		b.f("site_mark", &Leader::site_mark).f("pop_cap", &Leader::pop_cap).f("pop_issues", &Leader::pop_issues).f("misery", &Leader::misery);
		b.f("att", &Leader::att).f("anti_att", &Leader::anti_att).f("give_att_disabled", &Leader::give_att_disabled);
		b.f("take_att_disabled", &Leader::take_att_disabled).f("neutral_attrition", &Leader::neutral_attrition);
		b.f("disable_building_attrition", &Leader::disable_building_attrition).f("units_built", &Leader::units_built);
		b.f("units_killed", &Leader::units_killed).f("units_lost", &Leader::units_lost).f("buildings_built", &Leader::buildings_built);
		b.f("buildings_razed", &Leader::buildings_razed).f("buildings_lost", &Leader::buildings_lost).f("cities_built", &Leader::cities_built);
		b.f("cities_captured", &Leader::cities_captured).f("cities_lost", &Leader::cities_lost).f("army_high", &Leader::army_high);
		b.f("peasant_high", &Leader::peasant_high).f("scholar_high", &Leader::scholar_high).f("caravan_high", &Leader::caravan_high);
		b.f("merchant_high", &Leader::merchant_high).f("city_high", &Leader::city_high).f("village_high", &Leader::village_high);
		b.f("population_high", &Leader::population_high).f("city_pop_high", &Leader::city_pop_high);
		b.f("resources_controlled", &Leader::resources_controlled).f("wonders_built", &Leader::wonders_built);
		b.f("wonders_held", &Leader::wonders_held).f("senates_built", &Leader::senates_built).f("tribute_sent", &Leader::tribute_sent);
		b.f("tribute_received", &Leader::tribute_received).f("goody_box_resources", &Leader::goody_box_resources);
		b.f("collected", &Leader::collected).f("gather_slots", &Leader::gather_slots).f("filled_gather_slots", &Leader::filled_gather_slots);
		b.f("gather_slots_high", &Leader::gather_slots_high).f("forts_built", &Leader::forts_built).f("age_stamp", &Leader::age_stamp);
		b.f("plunder_scale", &Leader::plunder_scale).f("bonus_cap", &Leader::bonus_cap);
		b.f("active", &Leader::active).f("control", &Leader::control).f("combat", &Leader::combat).f("non_siege", &Leader::non_siege);
		b.f("sea_combat", &Leader::sea_combat).f("siege", &Leader::siege).f("defense", &Leader::defense).f("attack", &Leader::attack);
		b.f("pop", &Leader::pop).f("naval", &Leader::naval).f("air", &Leader::air).f("missile", &Leader::missile);
		b.f("transports", &Leader::transports).f("fishermen", &Leader::fishermen).f("idle_fishermen", &Leader::idle_fishermen);
		b.f("peasants", &Leader::peasants).f("scholars", &Leader::scholars).f("caras", &Leader::caras).f("merchants", &Leader::merchants);
		b.f("scouts", &Leader::scouts).f("fighters", &Leader::fighters).f("bombers", &Leader::bombers).f("cruise", &Leader::cruise);
		b.f("nuke", &Leader::nuke).f("wars", &Leader::wars).f("allies", &Leader::allies).f("active_wars", &Leader::active_wars);
		b.f("active_wars_with", &Leader::active_wars_with).f("free_peasants", &Leader::free_peasants);
		b.f("xport_peasants", &Leader::xport_peasants).f("gatherers", &Leader::gatherers).f("home_reg", &Leader::home_reg);
		b.f("attacked", &Leader::attacked).f("full_cities", &Leader::full_cities).f("explored", &Leader::explored);
		b.f("territory", &Leader::territory).f("territory_high", &Leader::territory_high).f("effective_pop", &Leader::effective_pop);
		b.f("other_team_terr", &Leader::other_team_terr).f("min_other_team_terr", &Leader::min_other_team_terr);
		b.f("my_team_terr", &Leader::my_team_terr).f("scholar_militia", &Leader::scholar_militia).f("retargets", &Leader::retarget_budget);
		b.f("barracks_units", &Leader::barracks_units).f("stable_units", &Leader::stable_units).f("factory_units", &Leader::factory_units);
		b.f("combat_units", &Leader::combat_units).f("dock_units", &Leader::dock_units).f("air_units", &Leader::air_units);
		b.f("barracks_queued", &Leader::barracks_queued).f("stable_queued", &Leader::stable_queued);
		b.f("factory_queued", &Leader::factory_queued).f("combat_queued", &Leader::combat_queued).f("dock_queued", &Leader::dock_queued);
		b.f("air_queued", &Leader::air_queued).f("peasants_garr", &Leader::peasants_inside).f("stable_garr", &Leader::stable_inside);
		b.f("scout_garr", &Leader::special_inside).f("barracks_garr", &Leader::barracks_inside).f("nukes_launched", &Leader::nukes_launched);
		b.f("nukes_in_flight", &Leader::nukes_in_flight).f("frame_attacked", &Leader::frame_attacked).f("attacked_by", &Leader::attacked_by);
		b.f("gov_hero_frame", &Leader::gov_hero_frame);
		b.f("deaths_current_frame", &Leader::deaths_current_frame).f("kills_current_frame", &Leader::kills_current_frame);
		b.f("hits_current_frame", &Leader::hits_current_frame);
		b.f("strategy", &Leader::strategy).f("reg_active", &Leader::reg_active).f("reg_combat", &Leader::reg_combat);
		b.f("reg_defense", &Leader::reg_defense).f("reg_attack", &Leader::reg_attack).f("reg_pop", &Leader::reg_pop);
		b.f("reg_naval", &Leader::reg_naval).f("reg_transports", &Leader::reg_transports).f("reg_peasants", &Leader::reg_peasants);
		b.f("reg_free_peasants", &Leader::reg_free_peasants).f("reg_xport_peasants", &Leader::reg_xport_peasants);
		b.f("reg_gatherers", &Leader::reg_gatherers).f("reg_gather_slots", &Leader::reg_gather_slots).f("reg_cities", &Leader::reg_cities);
		b.f("reg_forts", &Leader::reg_forts).f("reg_land", &Leader::reg_land).f("reg_terr", &Leader::reg_terr);
		b.c("reg_buildings",
		    [](Leader& l, const uint8_t* p) {
			    for (size_t r = 0; r < 64; ++r) arr_load(l.reg_buildings[r], p + r * 258, 2);
		    },
		    [](const Leader& l, uint8_t* p) {
			    for (size_t r = 0; r < 64; ++r) arr_emit(l.reg_buildings[r], p + r * 258, 2);
		    });
		b.f("num_buildings", &Leader::num_buildings).f("high_buildings", &Leader::high_buildings).f("num_units", &Leader::num_units);
		b.f("num_queued", &Leader::num_queued).f("last_building_finished", &Leader::last_building_finished);
		b.f("last_unit_finished", &Leader::last_unit_finished);
		b.f("ages_queued", &Leader::ages_queued).f("epochs_queued", &Leader::epochs_queued).f("reg_attacked", &Leader::reg_attacked);
		b.f("reg_wars", &Leader::reg_wars).f("reg_neutrals", &Leader::reg_neutrals).f("reg_allies", &Leader::reg_allies);
		b.f("num_bonus_cards", &Leader::num_bonus_cards).f("num_ctw_rate_bonuses", &Leader::num_ctw_rate_bonuses);
		b.f("defeat_stamp", &Leader::defeat_stamp).f("ally_mask", &Leader::ally_mask);
		b.c("team_color", [](Leader& l, const uint8_t* p) { scalar_load(l.color, p, 1, false); },
		    [](const Leader& l, uint8_t* p) { scalar_emit(l.color, p, 1); });
		// dip[8]: Diplomacy (agree, any_offer, treaty, offers[6], dows[6], attacks[8]) = 0x5c bytes each.
		b.raw(0x692c, 8 * 0x5c, "dip",
		      [](Leader& l, const uint8_t* p) {
			      for (size_t k = 0; k < 8; ++k) {
				      const uint8_t* q = p + k * 0x5c;
				      Leader::Deal& d = l.dip[k];
				      scalar_load(d.agree, q, 4, true);
				      scalar_load(d.any_offer, q + 4, 4, true);
				      scalar_load(d.treaty, q + 8, 4, true);
				      arr_load(d.offers, q + 0xc, 4);
				      arr_load(d.dows, q + 0x24, 4);
				      arr_load(d.attacks, q + 0x3c, 4);
			      }
		      },
		      [](const Leader& l, uint8_t* p) {
			      for (size_t k = 0; k < 8; ++k) {
				      uint8_t* q = p + k * 0x5c;
				      const Leader::Deal& d = l.dip[k];
				      scalar_emit(d.agree, q, 4);
				      scalar_emit(d.any_offer, q + 4, 4);
				      scalar_emit(d.treaty, q + 8, 4);
				      arr_emit(d.offers, q + 0xc, 4);
				      arr_emit(d.dows, q + 0x24, 4);
				      arr_emit(d.attacks, q + 0x3c, 4);
			      }
		      });
		// pers: 24 ints
		b.c("pers",
		    [](Leader& l, const uint8_t* p) {
			    int32_t* v = &l.pers.rush;
			    for (int i = 0; i < 24; ++i) scalar_load(v[i], p + 4 * i, 4, true);
		    },
		    [](const Leader& l, uint8_t* p) {
			    const int32_t* v = &l.pers.rush;
			    for (int i = 0; i < 24; ++i) scalar_emit(v[i], p + 4 * i, 4);
		    });
		bitmask_bytes(b, "tech", &Leader::tech, 101);
		bitmask_bytes(b, "tech_at_start", &Leader::tech_at_start, 101);
		bitmask_bytes(b, "obs_flags", &Leader::obs_flags, 101);
		bitmask_bytes(b, "conquest_racial_powers", &Leader::conquest_racial_powers, 3);
		bitmask_bytes(b, "rare", &Leader::rare, 6);
		bitmask_bytes(b, "rare_owned", &Leader::rare_owned, 6);
		bitmask_bytes(b, "rare_conquest", &Leader::rare_conquest, 6);
		// The leader's containers: sites, make_list (fixed Array families) and the three SimpleArray<int> lists.
		const dw::Description& d = dw::Description::get();
		static const int step_sites = d.find_step(d.find_function("Array<Site>::walk_data"), dw::StepKind::Container);
		static const int step_make = d.find_step(d.find_function("Array<MakeObject>::walk_data"), dw::StepKind::Container);
		static const int step_simple = d.find_step(d.find_function("SimpleArray<int>::walk_data"), dw::StepKind::Container);
		b.arrays([](const Game& g, const Seg& s, int step, int64_t base, ArraySpec& spec) {
			if (s.a < 0 || s.a >= kNumLeaders) return false;
			const int64_t off = base - int64_t(s.a) * 0x6eec;
			const int32_t who = s.a;
			const Leader& l = g.leaders[size_t(who)];
			if (!(l.flags & 1)) return false;
			if ((step == step_sites || step == step_make) && g.original && !g.original->leader_ai[who]) return false;
			if (step == step_sites && off == 0x6e34) {
				spec.count = int32_t(l.sites.size());
				spec.child = &map_site();
				spec.child_id = [who](int32_t i) { return Seg{who, i, 0}; };
				return true;
			}
			if (step == step_make && off == 0x6ec8) {
				spec.count = int32_t(l.make_list.size());
				spec.child = &map_make_object();
				spec.child_id = [who](int32_t i) { return Seg{who, i, 0}; };
				return true;
			}
			if (step == step_simple && (off == 0x6e50 || off == 0x6e6c || off == 0x6e88)) {
				const std::vector<int32_t>& v = off == 0x6e50 ? l.mil_trainers : off == 0x6e6c ? l.new_rares : l.oil_patches;
				spec.count = int32_t(v.size());
				const std::vector<int32_t>* vp = &v;
				spec.block = [vp](std::vector<uint8_t>& out) {
					out.resize(vp->size() * 4);
					for (size_t i = 0; i < vp->size(); ++i) put_le(out.data() + 4 * i, 4, uint32_t((*vp)[i]));
				};
				spec.simple = true;
				return true;
			}
			return false;
		});
		static const int fn_leader = d.find_function("LeaderData::walk_data");
		b.strings([](const Game& g, const Seg& s, int step, int64_t base, std::u16string& out) {
			if (s.a < 0 || s.a >= kNumLeaders || dw::Description::get().step_function(step) != fn_leader) return false;
			if (base - int64_t(s.a) * 0x6eec != 0x6ea4) return false;
			const std::string& p = g.leaders[size_t(s.a)].prod_script;
			out.assign(p.begin(), p.end());
			return true;
		});
		// An inactive leader (leader_flags bit 0 clear) walks only its two flag words.
		m.audited = [](const Game& g, const Seg& s, const Entry& e) {
			return e.off < 8 || (s.a >= 0 && s.a < kNumLeaders && (g.leaders[size_t(s.a)].flags & 1));
		};
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_site() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Site> b(m, "Site", "Site");
		b.walks({"Site::walk_data"}).resolve([](Game& g, const Seg& s) -> Site* {
			return s.a >= 0 && s.a < kNumLeaders && size_t(s.b) < g.leaders[size_t(s.a)].sites.size() ? &g.leaders[size_t(s.a)].sites[size_t(s.b)] : nullptr;
		});
		b.f("wx", &Site::wx).f("wy", &Site::wy).f("val", &Site::val).f("reg", &Site::reg).f("dist", &Site::dist).f("rank", &Site::rank);
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_make_object() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<MakeObject> b(m, "MakeObject", "MakeObject");
		b.walks({"MakeObject::walk_data"}).resolve([](Game& g, const Seg& s) -> MakeObject* {
			return s.a >= 0 && s.a < kNumLeaders && size_t(s.b) < g.leaders[size_t(s.a)].make_list.size() ? &g.leaders[size_t(s.a)].make_list[size_t(s.b)] : nullptr;
		});
		b.f("t", &MakeObject::t).f("val", &MakeObject::val).f("escrow", &MakeObject::escrow).f("city", &MakeObject::city).f("up", &MakeObject::up);
		b.f("o", &MakeObject::o).f("num", &MakeObject::num).f("cat", &MakeObject::cat).f("wx", &MakeObject::wx).f("wy", &MakeObject::wy);
		m.finish();
		return m;
	}();
	return m;
}

// The LeaderDataEncrypt block of an active leader: 62 u32 written as locals (memory ^ key = the plain value).
const ClassMap& map_leader_encrypt() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Leader> b(m, "LeaderDataEncrypt", "LeaderDataEncrypt");
		b.walks({"LeaderDataEncrypt::walk_data"}).resolve([](Game& g, const Seg& s) -> Leader* {
			return s.a >= 0 && s.a < kNumLeaders ? &g.leaders[size_t(s.a)] : nullptr;
		});
		b.locals([](const Game& g, const Seg& s, const char* name, int nth, int64_t& v) {
			if (s.a < 0 || s.a >= kNumLeaders) return false;
			const Leader& l = g.leaders[size_t(s.a)];
			const size_t k = size_t(nth);
			auto six = [&](const std::array<int32_t, 6>& a) { if (k >= 6) return false; v = a[k]; return true; };
			if (!std::strcmp(name, "bucket")) return six(l.bucket);
			if (!std::strcmp(name, "leftover")) return six(l.leftover);
			if (!std::strcmp(name, "resource_cap")) { if (k >= 6) return false; v = l.resource_cap[k]; return true; }
			if (!std::strcmp(name, "over_cap")) return six(l.over_cap);
			if (!std::strcmp(name, "resources")) return six(l.resources);
			if (!std::strcmp(name, "support")) return six(l.support);
			if (!std::strcmp(name, "income")) return six(l.income);
			if (!std::strcmp(name, "rate")) return six(l.rate);
			if (!std::strcmp(name, "bonus")) return six(l.bonus);
			if (!std::strcmp(name, "resource_cap6")) { v = l.resource_cap[6]; return true; }
			if (!std::strcmp(name, "epoch")) { if (k >= 4) return false; v = l.epoch[k]; return true; }
			if (!std::strcmp(name, "ages")) { v = l.age; return true; }
			if (!std::strcmp(name, "epochs")) { v = l.epochs; return true; }
			if (!std::strcmp(name, "discovered")) { v = l.discovered; return true; }
			return false;
		});
		m.finish();
		return m;
	}();
	return m;
}

}  // namespace ron::sim::omap
