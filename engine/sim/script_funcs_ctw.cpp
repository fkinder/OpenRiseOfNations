// The Conquer the World builtins (docs/script-spec.md 7.6, docs/ctw-spec.md 8): the ctw_* / territory / tribute /
// diplomacy functions the 330 campaign scripts call, on the campaign state (sim/ctw.hpp). They run in two places: the
// campaign's strategic hooks (the campaign's own ScriptRuntime) and the battle scripts of a game built from a campaign
// link (ScriptHost::ctw set by ConquestGame::build_battle). Without a campaign (a battle started on its own) the reads
// keep the stand-alone answers of script_funcs_query.cpp ("" / 0 / -1).
#include <algorithm>

#include "sim/ctw.hpp"
#include "sim/script_funcs_internal.hpp"

namespace ron::sim::sf {

namespace {

using ctw::ConquestGame;

std::string S8(const U16& s) { return bhs::to_utf8(s); }
U16 U(const std::string& s) { return bhs::to_u16(s); }

void fill_array(bhs::Value* arr, const std::vector<std::string>& items) {
	if (!arr || !arr->is_array()) return;
	arr->elems.clear();
	for (const std::string& s : items) arr->elems.push_back(make_string(U(s), bhs::SC_VAR));
}

}  // namespace

void install_ctw_funcs(ScriptHost& h) {
	bhs::Runtime& rt = h.rt;
	auto def = [&rt](const char* name, Fn f, int overload = 0) { rt.set_native(name, std::move(f), overload); };
	auto cg = [&h]() { return h.ctw; };
	// slot of a tribe argument (key, display name, rules tribe name), -1
	auto slot = [cg](const U16& name) -> int {
		ConquestGame* c = cg();
		return c ? c->slot_of(S8(name)) : -1;
	};
	auto node = [cg](const U16& name) -> int {
		ConquestGame* c = cg();
		return c ? c->find_node(S8(name)) : -1;
	};

	// ---- stored values (the campaign keeps them; without one the host's own maps, script_funcs_query.cpp)
	def("get_ctw_int", [&h, cg](NativeCall& c) {
		if (ConquestGame* g = cg()) {
			auto it = g->stored_ints.find(S8(c.arg_str(0)));
			return make_int(it == g->stored_ints.end() ? 0 : it->second);
		}
		auto it = h.ctw_ints.find(c.arg_str(0));
		return make_int(it == h.ctw_ints.end() ? 0 : it->second);
	});
	def("set_ctw_int", [&h, cg](NativeCall& c) {
		if (ConquestGame* g = cg()) g->stored_ints[S8(c.arg_str(0))] = c.arg_int(1);
		else h.ctw_ints[c.arg_str(0)] = c.arg_int(1);
		return VP();
	});
	def("get_ctw_str", [&h, cg](NativeCall& c) {
		if (ConquestGame* g = cg()) {
			auto it = g->stored_strs.find(S8(c.arg_str(0)));
			return make_string(it == g->stored_strs.end() ? U16() : U(it->second));
		}
		auto it = h.ctw_strs.find(c.arg_str(0));
		return make_string(it == h.ctw_strs.end() ? U16() : it->second);
	});
	def("set_ctw_str", [&h, cg](NativeCall& c) {
		if (ConquestGame* g = cg()) g->stored_strs[S8(c.arg_str(0))] = S8(c.arg_str(1));
		else h.ctw_strs[c.arg_str(0)] = c.arg_str(1);
		return VP();
	});
	def("clear_ctw_val", [&h, cg](NativeCall& c) {
		if (ConquestGame* g = cg()) {
			g->stored_ints.erase(S8(c.arg_str(0)));
			g->stored_strs.erase(S8(c.arg_str(0)));
		} else {
			h.ctw_ints.erase(c.arg_str(0));
			h.ctw_strs.erase(c.arg_str(0));
		}
		return VP();
	});
	def("clear_ctw_vals", [&h, cg](NativeCall&) {
		if (ConquestGame* g = cg()) {
			g->stored_ints.clear();
			g->stored_strs.clear();
		} else {
			h.ctw_ints.clear();
			h.ctw_strs.clear();
		}
		return VP();
	});

	// ---- battle: the seats' start tiles (BattleDef: filled by ConquestGame::build_battle)
	def("get_starting_loc_x", [cg](NativeCall& c) {
		const int w = c.arg_int(0) - 1;
		return make_int(cg() && w >= 0 && w < 8 ? cg()->start_locs[size_t(w)][0] : -1);
	});
	def("get_starting_loc_y", [cg](NativeCall& c) {
		const int w = c.arg_int(0) - 1;
		return make_int(cg() && w >= 0 && w < 8 ? cg()->start_locs[size_t(w)][1] : -1);
	});

	// ---- Cold War: enable_type_by_tribe_with_type_name = enable_type_by_tribe with the type found by its type name
	// (Type::type_name, the unit line's key) rather than its display name (the Conquer the World scripts name upgraded units this way)
	auto enable_by_type_name = [&h](NativeCall& c) {
		Game& g = h.game;
		std::string want = S8(c.arg_str(0));
		const int32_t tr = tribe_index(g, c.arg_str(1));
		int32_t t = -1;
		for (int32_t i = kUnits; i < kEndBuilds; i++) {
			const Type& T = g.rules->types[size_t(i)];
			if (T.loaded && (ctw::ieq(to_utf8(T.type_name), want) || (T.type_name.empty() && ctw::ieq(to_utf8(T.name), want)))) {
				t = i;
				break;
			}
		}
		if (t < 0) t = type_index(g, c.arg_str(0));
		if (t < 0 || tr < 0) return make_int(-1);
		Type& T = g.mut_rules().types[size_t(t)];
		T.tribe_mask |= int32_t(1u << (tr & 31));
		T.modified = 1;
		for (int32_t w = 0; w < 8; w++) {
			Leader& l = g.leaders[size_t(w)];
			if ((l.flags & leaderflag::kValid) && l.tribe == tr) Leader::set_bit(l.obs_flags, t, false);
		}
		if (c.args.size() >= 5 && t < kBuilds) {  // where / grid column / row for units
			const int32_t b = type_index(g, c.arg_str(2));
			if (b >= kBuilds && b < kEndBuilds) {
				T.where = b;
				T.grid_x = int8_t(c.arg_int(4));
				T.grid_y = int8_t(c.arg_int(3));
			}
		}
		return make_int(1);
	};
	def("enable_type_by_tribe_with_type_name", enable_by_type_name, 0);
	def("enable_type_by_tribe_with_type_name", enable_by_type_name, 1);

	// ---- the campaign clock
	def("get_turn", [cg](NativeCall&) { return make_int(cg() ? cg()->cur_round : 0); });
	def("get_age", [cg](NativeCall&) { return make_int(cg() ? cg()->cur_age : 0); });
	def("get_ctw_campaign", [cg](NativeCall&) { return make_string(cg() ? U(cg()->map_name) : U16()); });
	def("ctw_get_num_campaign_turns", [cg](NativeCall&) { return make_int(cg() ? cg()->length() : -1); });
	def("get_ctw_seed", [cg](NativeCall&) { return make_int(cg() ? int32_t(cg()->seed & 0x7fffffff) : 0); });
	def("get_difficulty", [cg, &h](NativeCall&) {  // the campaign's own difficulty (a battle's info.difficulty is set from it)
		return make_int((cg() ? cg()->difficulty : h.game.info.difficulty) + 1);
	});
	def("ctw_last_battle_victory", [cg](NativeCall&) { return make_int(cg() && cg()->last_battle_victory ? 1 : 0); });
	def("get_attack_terr_str", [cg](NativeCall&) {  // the strength (infrastructure level) of the territory under attack
		return make_int(cg() && cg()->cur_link.to >= 0 ? cg()->nodes[size_t(cg()->cur_link.to)].level : -1);
	});
	def("get_attack_to", [cg](NativeCall&) { return make_string(cg() && cg()->cur_link.to >= 0 ? U(cg()->def->countries[size_t(cg()->cur_link.to)].id) : U16()); });
	def("get_attack_from", [cg](NativeCall&) { return make_string(cg() && cg()->cur_link.from >= 0 ? U(cg()->def->countries[size_t(cg()->cur_link.from)].id) : U16()); });

	// ---- tribes, ownership
	def("get_country_owner", [cg, node](NativeCall& c) {  // the master of a vassal
		ConquestGame* g = cg();
		const int n = node(c.arg_str(0));
		if (!g || n < 0) return make_string(U16());
		return make_string(U(g->tribe_key(g->node_eff_owner(n))));
	});
	auto territory_owner = [cg, node](NativeCall& c) {
		ConquestGame* g = cg();
		const int n = node(c.arg_str(0));
		if (!g || n < 0) return make_string(U16());
		const bool raw = c.args.size() > 1 && c.arg_int(1) != 0;
		return make_string(U(g->territory_owner_name(n, raw)));
	};
	def("get_territory_owner", territory_owner, 0);
	def("get_territory_owner", territory_owner, 1);
	def("get_num_territories", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		return make_int(cg() && s >= 0 ? cg()->num_nodes(s, false) : -1);
	});
	def("get_num_active_nations", [cg](NativeCall&) { return make_int(cg() ? int(cg()->active_slots().size()) : -1); });
	def("ctw_get_player_nation", [cg](NativeCall&) { return make_string(cg() ? U(cg()->tribe_key(cg()->player_tribe)) : U16()); });
	def("ctw_get_nation_capital", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0 || cg()->leaders[size_t(s)].capital < 0) return make_string(U16());
		return make_string(U(cg()->def->countries[size_t(cg()->leaders[size_t(s)].capital)].id));
	});
	def("ctw_set_leader_name", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0) return make_int(-1);
		cg()->leaders[size_t(s)].name = S8(c.arg_str(1));
		return make_int(1);
	});
	def("ctw_is_defeated", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		return make_int(cg() && s >= 0 ? (cg()->leaders[size_t(s)].active ? 0 : 1) : -1);
	});
	def("ctw_is_vassal", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		return make_int(cg() && s >= 0 ? (cg()->leaders[size_t(s)].vassal_of >= 0 ? 1 : 0) : -1);
	});
	def("ctw_get_master", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0 || cg()->leaders[size_t(s)].vassal_of < 0) return make_string(U16());
		return make_string(U(cg()->tribe_key(cg()->leaders[size_t(s)].vassal_of)));
	});
	def("ctw_get_num_allies", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		return make_int(cg() && s >= 0 ? cg()->num_allies(s) : -1);
	});
	def("get_num_ally", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		return make_int(cg() && s >= 0 ? cg()->num_allies(s) : -1);
	});
	auto stance_is = [cg, slot](int st) {
		return [cg, slot, st](NativeCall& c) {
			const int a = slot(c.arg_str(0)), b = slot(c.arg_str(1));
			if (!cg() || a < 0 || b < 0) return make_int(-1);
			return make_int(cg()->stance(a, b) == st ? 1 : 0);
		};
	};
	def("ctw_is_war", stance_is(ctw::kWar));
	def("ctw_is_peace", stance_is(ctw::kPeace));
	def("ctw_is_ally", stance_is(ctw::kAlly));
	auto stance_set = [cg, slot](int st) {
		return [cg, slot, st](NativeCall& c) {
			const int a = slot(c.arg_str(0)), b = slot(c.arg_str(1));
			if (!cg() || a < 0 || b < 0 || a == b) return make_int(-1);
			if (st == ctw::kWar) cg()->set_war(a, b);
			else if (st == ctw::kPeace) cg()->set_peace(a, b);
			else if (st == ctw::kAlly) cg()->set_ally(a, b);
			else cg()->set_vassal(a, b);
			return make_int(1);
		};
	};
	def("ctw_set_war", stance_set(ctw::kWar));
	def("ctw_set_peace", stance_set(ctw::kPeace));
	def("ctw_set_ally", stance_set(ctw::kAlly));
	def("ctw_set_vassal", stance_set(ctw::kVassal));
	auto buy = [cg, slot](int what) {
		return [cg, slot, what](NativeCall& c) {
			const int a = slot(c.arg_str(0)), b = slot(c.arg_str(1));
			if (!cg() || a < 0 || b < 0) return make_int(-1);
			bool ok = false;
			if (what == 0) ok = cg()->buy_war(a, b);
			else if (what == 1) ok = cg()->buy_peace(a, b);
			else if (what == 2) ok = cg()->buy_ally(a, b);
			else ok = cg()->buy_vassal(a, b);
			return make_int(ok ? 1 : 0);
		};
	};
	def("ctw_buy_war", buy(0));
	def("ctw_buy_peace", buy(1));
	def("ctw_buy_ally", buy(2));
	def("ctw_buy_vassal", buy(3));
	def("ctw_ai_no_declare_war", [cg, slot](NativeCall& c) {
		const int a = slot(c.arg_str(0)), b = slot(c.arg_str(1));
		if (!cg() || a < 0 || b < 0) return make_int(-1);
		auto& v = cg()->leaders[size_t(a)].no_declare_war;
		if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
		return make_int(1);
	});
	def("ctw_clear_ai_no_declare_war", [cg, slot](NativeCall& c) {
		const int a = slot(c.arg_str(0)), b = slot(c.arg_str(1));
		if (!cg() || a < 0 || b < 0) return make_int(-1);
		auto& v = cg()->leaders[size_t(a)].no_declare_war;
		v.erase(std::remove(v.begin(), v.end(), b), v.end());
		return make_int(1);
	});
	auto no_attack = [cg, slot, node](NativeCall& c) {  // set_no_attack(tribe, terr1[, terr2 ..]): 1 when any territory matched
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0) return make_int(-1);
		bool any = false;
		for (size_t k = 1; k < c.args.size(); k++) {
			const int n = node(c.arg_str(k));
			if (n < 0) continue;
			auto& v = cg()->leaders[size_t(s)].no_attack;
			if (std::find(v.begin(), v.end(), n) == v.end()) v.push_back(n);
			any = true;
		}
		return make_int(any ? 1 : 0);
	};
	for (int k = 0; k < 4; k++) def("set_no_attack", no_attack, k);
	def("ctw_disable_gunpowder", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0) return make_int(-1);
		cg()->leaders[size_t(s)].gunpowder_off = true;
		return make_int(1);
	});
	def("ctw_enable_gunpowder", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0) return make_int(-1);
		cg()->leaders[size_t(s)].gunpowder_off = false;
		return make_int(1);
	});
	def("disable_strategic_rares", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0) return make_int(-1);
		cg()->leaders[size_t(s)].rares_off = true;
		return make_int(1);
	});
	def("enable_strategic_rares", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0) return make_int(-1);
		cg()->leaders[size_t(s)].rares_off = false;
		return make_int(1);
	});

	// ---- tribute
	def("get_num_tribute", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		return make_int(cg() && s >= 0 ? cg()->leaders[size_t(s)].tribute : -1);
	});
	def("give_player_ctw_tribute", [cg](NativeCall& c) {
		if (!cg()) return make_int(-1);
		cg()->give_tribute(cg()->player_tribe, c.arg_int(0));
		return make_int(1);
	});
	def("give_ctw_tribute", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0) return make_int(-1);
		cg()->give_tribute(s, c.arg_int(1));
		return make_int(1);
	});
	def("get_nation_income", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0) return make_int(-1);
		ConquestGame& g = *cg();
		int add = 0;
		for (const ctw::Colony& col : g.colonies) add += col.owner == s ? col.tribute : 0;
		if (g.def->territory_income) {
			for (const ctw::Node& n : g.nodes) {
				if (n.owner == s) add += n.level * ctw::K(*g.rules, "ctw_terr_income_x") + ctw::K(*g.rules, "ctw_terr_income_base");
			}
		}
		return make_int(add);
	});
	def("give_player_bonus_card", [cg](NativeCall& c) {
		ctw::Card card;
		if (!cg() || !cg()->parse_card(S8(c.arg_str(0)), &card)) return make_int(-1);
		cg()->give_card(cg()->player_tribe, card);
		return make_int(1);
	});

	// ---- territories, armies, links
	def("get_territory_strength", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		return make_int(cg() && n >= 0 ? cg()->nodes[size_t(n)].level : -1);
	});
	def("set_territory_strength", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		if (!cg() || n < 0) return make_int(-1);
		cg()->set_node_level(n, c.arg_int(1));
		return make_int(1);
	});
	def("get_territory_turns_held", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		return make_int(cg() && n >= 0 ? cg()->nodes[size_t(n)].turns_held : -1);
	});
	def("get_territory_income", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		return make_int(cg() && n >= 0 ? cg()->nodes[size_t(n)].tribute : -1);
	});
	def("set_territory_display_name", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		if (!cg() || n < 0) return make_int(-1);
		cg()->nodes[size_t(n)].display_name = S8(c.arg_str(1));
		return make_int(1);
	});
	def("ctw_territory_has_army", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		return make_int(cg() && n >= 0 ? (cg()->has_army(n) ? 1 : 0) : -1);
	});
	def("ctw_give_army", [cg, slot, node](NativeCall& c) {
		const int s = slot(c.arg_str(0)), n = node(c.arg_str(1));
		if (!cg() || s < 0 || n < 0 || cg()->has_army(n)) return make_int(-1);
		cg()->pieces[size_t(s)].push_back({n, false});
		return make_int(1);
	});
	def("ctw_remove_army", [cg, slot, node](NativeCall& c) {
		const int s = slot(c.arg_str(0)), n = node(c.arg_str(1));
		if (!cg() || s < 0 || n < 0) return make_int(-1);
		auto& v = cg()->pieces[size_t(s)];
		const size_t before = v.size();
		v.erase(std::remove_if(v.begin(), v.end(), [n](const ctw::Piece& p) { return p.node == n; }), v.end());
		return make_int(v.size() < before ? 1 : 0);
	});
	def("ctw_remove_all_armies", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0) return make_int(-1);
		cg()->pieces[size_t(s)].clear();
		return make_int(1);
	});
	def("ctw_give_territory", [cg, slot, node](NativeCall& c) {
		const int n = node(c.arg_str(0)), s = slot(c.arg_str(1));
		if (!cg() || n < 0) return make_int(-1);
		const std::string flag = c.args.size() > 2 ? S8(c.arg_str(2)) : std::string("0");
		const bool collect = !flag.empty() && flag != "0" && flag != "false";
		cg()->transfer_to(n, s, collect, nullptr);
		return make_int(1);
	});
	def("ctw_territory_captured", [cg, slot, node](NativeCall& c) {  // the territory changed hands away from `owner` (to `attacker`)
		const int n = node(c.arg_str(0)), o = slot(c.arg_str(1));
		if (!cg() || n < 0) return make_int(-1);
		bool captured = cg()->nodes[size_t(n)].owner != o;
		if (captured && c.args.size() > 2) captured = cg()->nodes[size_t(n)].owner == slot(c.arg_str(2));
		return make_int(captured ? 1 : 0);
	}, 0);
	def("ctw_territory_captured", [cg, slot, node](NativeCall& c) {
		const int n = node(c.arg_str(0)), o = slot(c.arg_str(1));
		if (!cg() || n < 0) return make_int(-1);
		bool captured = cg()->nodes[size_t(n)].owner != o;
		if (captured && c.args.size() > 2) captured = cg()->nodes[size_t(n)].owner == slot(c.arg_str(2));
		return make_int(captured ? 1 : 0);
	}, 1);
	def("ctw_add_link", [cg, node](NativeCall& c) {
		const int a = node(c.arg_str(0)), b = node(c.arg_str(1));
		if (!cg() || a < 0 || b < 0) return make_int(-1);
		cg()->script_links.push_back({a, b});
		return make_int(1);
	});
	def("ctw_territory_link", [cg, node](NativeCall& c) {
		const int a = node(c.arg_str(0)), b = node(c.arg_str(1));
		if (!cg() || a < 0 || b < 0) return make_int(-1);
		for (int t : cg()->nodes[size_t(a)].links) {
			if (t == b) return make_int(1);
		}
		return make_int(0);
	});
	def("ctw_territory_sea_link", [cg, node](NativeCall& c) {
		const int a = node(c.arg_str(0)), b = node(c.arg_str(1));
		if (!cg() || a < 0 || b < 0) return make_int(-1);
		const auto& N = cg()->nodes[size_t(a)];
		for (size_t k = 0; k < N.links.size(); k++) {
			if (N.links[k] == b) return make_int(N.link_sea[k] ? 1 : 0);
		}
		return make_int(0);
	});
	def("ctw_num_adjacent_territories", [cg, slot](NativeCall& c) {
		const int a = slot(c.arg_str(0)), b = slot(c.arg_str(1));
		if (!cg() || a < 0 || b < 0) return make_int(-1);
		int n = 0;
		for (size_t i = 0; i < cg()->nodes.size(); i++) {
			if (cg()->nodes[i].owner != a) continue;
			for (int t : cg()->nodes[i].links) {
				if (cg()->nodes[size_t(t)].owner == b) {
					n++;
					break;
				}
			}
		}
		return make_int(n);
	});
	def("ctw_territory_list", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0 || c.args.size() < 2) return make_int(-1);
		std::vector<std::string> out;
		for (int n : cg()->owned_nodes(s, c.arg_int(2) != 0)) out.push_back(cg()->def->countries[size_t(n)].id);
		fill_array(c.args[1].get(), out);
		return make_int(int(out.size()));
	});
	def("ctw_colony_list", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0 || c.args.size() < 2) return make_int(-1);
		std::vector<std::string> out;
		for (const ctw::Colony& col : cg()->colonies) {
			if (col.owner == s) out.push_back(col.id);
		}
		fill_array(c.args[1].get(), out);
		return make_int(int(out.size()));
	});
	def("ctw_bonus_card_list", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0 || c.args.size() < 2) return make_int(-1);
		std::vector<std::string> out;
		for (const ctw::Card& card : cg()->leaders[size_t(s)].cards) out.push_back(cg()->card_name(card));
		fill_array(c.args[1].get(), out);
		return make_int(int(out.size()));
	});
	def("ctw_get_active_tribes", [cg](NativeCall& c) {
		std::vector<std::string> out;
		if (cg()) {
			for (int s : cg()->active_slots()) out.push_back(cg()->tribe_key(s));
		}
		if (!c.args.empty()) fill_array(c.args[0].get(), out);
		return VP();
	});

	// ---- colonies, cards, values
	auto colony_by = [cg](const U16& id) -> ctw::Colony* {
		if (!cg()) return nullptr;
		for (ctw::Colony& col : cg()->colonies) {
			if (ctw::ieq(col.id, S8(id)) || ctw::ieq(col.name, S8(id))) return &col;
		}
		return nullptr;
	};
	def("get_colony_owner", [cg, colony_by](NativeCall& c) {
		ctw::Colony* col = colony_by(c.arg_str(0));
		return make_string(col && col->owner >= 0 ? U(cg()->tribe_key(col->owner)) : U16());
	});
	def("ctw_give_colony", [cg, slot, colony_by](NativeCall& c) {
		ctw::Colony* col = colony_by(c.arg_str(0));
		if (!col) return make_int(-1);
		col->owner = slot(c.arg_str(1));
		(void)cg;
		return make_int(1);
	});
	def("ctw_remove_colony", [colony_by](NativeCall& c) {
		ctw::Colony* col = colony_by(c.arg_str(0));
		if (!col) return make_int(-1);
		col->owner = -1;
		return make_int(1);
	});
	def("ctw_get_colony_tribute", [colony_by](NativeCall& c) {
		ctw::Colony* col = colony_by(c.arg_str(0));
		return make_int(col ? col->tribute : -1);
	});
	def("ctw_get_colony_value", [colony_by](NativeCall& c) {
		ctw::Colony* col = colony_by(c.arg_str(0));
		return make_int(col ? col->value : -1);
	});
	def("ctw_get_territory_value", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		return make_int(cg() && n >= 0 ? cg()->nodes[size_t(n)].value : -1);
	});
	def("ctw_get_card_value", [cg](NativeCall& c) {
		ctw::Card card;
		if (!cg() || !cg()->parse_card(S8(c.arg_str(0)), &card)) return make_int(-1);
		return make_int(cg()->card_value(card));
	});

	// ---- deals
	def("ctw_deal_accepted", [cg](NativeCall& c) {
		if (!cg()) return make_int(-1);
		const std::string id = S8(c.arg_str(0));
		return make_int(std::find(cg()->deals.begin(), cg()->deals.end(), id) != cg()->deals.end() ? 1 : 0);
	});
	def("ctw_set_deal_accepted", [cg](NativeCall& c) {
		if (!cg()) return make_int(-1);
		const std::string id = S8(c.arg_str(0));
		auto& d = cg()->deals;
		auto it = std::find(d.begin(), d.end(), id);
		if (c.arg_int(1) != 0) {
			if (it == d.end()) d.push_back(id);
		} else if (it != d.end()) {
			d.erase(it);
		}
		return make_int(1);
	});
	def("ctw_clear_offer", [](NativeCall& c) {
		bhs::Value* o = c.args.empty() ? nullptr : c.args[0].get();
		if (!o) return make_int(-1);
		for (const bhs::VP& e : o->elems) {
			if (!e) continue;
			if (e->is_array()) e->elems.clear();
			else if (e->cls == bhs::Value::String) e->s.clear();
			else e->i = 0;
		}
		return make_int(1);
	});
	def("ctw_reject_bribes", [cg](NativeCall&) {
		if (!cg()) return make_int(-1);
		for (auto& l : cg()->leaders) l.reject_bribe = true;
		return make_int(1);
	});
	def("ctw_clear_bribes", [cg](NativeCall&) {
		if (!cg()) return make_int(-1);
		for (auto& l : cg()->leaders) {
			l.bribe_paid.fill(0);
			l.no_attack_time.fill(0);
			l.reject_bribe = false;
		}
		return make_int(1);
	});
	def("set_player_no_negotiate", [cg](NativeCall& c) {
		if (!cg()) return make_int(-1);
		cg()->no_negotiate_text = S8(c.arg_str(0));
		return make_int(1);
	});

	// ---- the end of the campaign, the UI switches
	def("ctw_game_over", [cg](NativeCall& c) {
		if (!cg()) return make_int(-1);
		const bool won = c.arg_int(0) != 0;
		const bool four = c.args.size() >= 4;
		cg()->end_victory = won ? 1 : 0;
		cg()->end_title = S8(c.arg_str(four ? 2 : 1));
		cg()->end_text = S8(c.arg_str(four ? 3 : 2));
		cg()->game_end = won ? ctw::kVictoryAllied : ctw::kPlayerEliminated;
		return make_int(1);
	}, 0);
	def("ctw_game_over", [cg](NativeCall& c) {
		if (!cg()) return make_int(-1);
		const bool won = c.arg_int(0) != 0;
		const bool four = c.args.size() >= 4;
		cg()->end_victory = won ? 1 : 0;
		cg()->end_title = S8(c.arg_str(four ? 2 : 1));
		cg()->end_text = S8(c.arg_str(four ? 3 : 2));
		cg()->game_end = won ? ctw::kVictoryAllied : ctw::kPlayerEliminated;
		return make_int(1);
	}, 1);
	def("ctw_custom_end_screen", [cg](NativeCall& c) {
		if (!cg()) return make_int(-1);
		cg()->end_title = S8(c.arg_str(0));
		cg()->end_text = S8(c.arg_str(1));
		cg()->end_image = S8(c.arg_str(2));
		cg()->end_button = S8(c.arg_str(3));
		cg()->end_victory = c.arg_int(4) != 0 ? 1 : 0;
		return make_int(1);
	});
	def("ctw_set_sub_goals", [cg](NativeCall& c) {
		if (!cg()) return make_int(-1);
		cg()->sub_goals.clear();
		for (size_t k = 0; k < 2; k++) {
			const std::string t = S8(c.arg_str(k));
			if (!t.empty()) cg()->sub_goals.push_back(t);
		}
		return make_int(1);
	});
	def("ctw_center_camera", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		if (!cg() || n < 0) return make_int(-1);
		cg()->center_camera = n;
		return make_int(1);
	});
	def("ctw_disable_piece_moves", [cg](NativeCall&) {
		if (cg()) cg()->piece_moves_disabled = true;
		return make_int(cg() ? 1 : -1);
	});
	def("ctw_enable_piece_moves", [cg](NativeCall&) {
		if (cg()) cg()->piece_moves_disabled = false;
		return make_int(cg() ? 1 : -1);
	});
	def("ctw_show_supply_depots", [cg](NativeCall&) {
		if (cg()) cg()->supply_depots_hidden = false;
		return make_int(cg() ? 1 : -1);
	});
	def("ctw_hide_supply_depots", [cg](NativeCall&) {
		if (cg()) cg()->supply_depots_hidden = true;
		return make_int(cg() ? 1 : -1);
	});
	def("ctw_disable_nuke_building", [cg](NativeCall&) {
		if (cg()) cg()->nuke_building_disabled = true;
		return make_int(cg() ? 1 : -1);
	});
	def("ctw_enable_nuke_building", [cg](NativeCall&) {
		if (cg()) cg()->nuke_building_disabled = false;
		return make_int(cg() ? 1 : -1);
	});
	def("ctw_disable_espionage", [cg](NativeCall&) {
		if (cg()) cg()->espionage_disabled = true;
		return make_int(cg() ? 1 : -1);
	});
	def("ctw_enable_espionage", [cg](NativeCall&) {
		if (cg()) cg()->espionage_disabled = false;
		return make_int(cg() ? 1 : -1);
	});

	// ---- Cold War: defcon and nukes (the numbers; the launch rules are the spec's unknown "?")
	def("get_defcon_level", [cg](NativeCall&) { return make_int(cg() ? cg()->defcon_level : -1); });
	def("set_defcon_level", [cg](NativeCall& c) {
		if (!cg()) return make_int(-1);
		cg()->defcon_level = std::clamp(c.arg_int(0), 0, 5);
		return make_int(1);
	});
	def("get_total_nukes", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		if (!cg() || s < 0) return make_int(-1);
		int n = 0;
		for (const ctw::Node& nd : cg()->nodes) n += nd.owner == s ? nd.nukes : 0;
		return make_int(n);
	});
	def("get_num_nukes", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		return make_int(cg() && n >= 0 ? cg()->nodes[size_t(n)].nukes : -1);
	});
	def("ctw_remove_nuke", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		if (!cg() || n < 0) return make_int(-1);
		cg()->nodes[size_t(n)].nukes = std::max(0, cg()->nodes[size_t(n)].nukes - 1);
		return make_int(1);
	});
	def("ctw_get_nuke_cost", [cg, slot](NativeCall& c) {
		const int s = slot(c.arg_str(0));
		return make_int(cg() && s >= 0 ? cg()->nuke_cost(s) : -1);
	});
	def("ctw_build_nuke", [cg, node](NativeCall& c) {
		const int n = node(c.arg_str(0));
		if (!cg() || n < 0) return make_int(-1);
		ctw::Node& nd = cg()->nodes[size_t(n)];
		if (nd.owner < 0) return make_int(-1);
		if (c.arg_int(1) != 0) {
			const int cost = cg()->nuke_cost(nd.owner);
			if (cg()->leaders[size_t(nd.owner)].tribute < cost) return make_int(0);
			cg()->leaders[size_t(nd.owner)].tribute -= cost;
		}
		nd.nukes++;
		return make_int(1);
	});
	def("ctw_ai_launch_nukes", [cg](NativeCall&) { return make_int(cg() ? 0 : -1); });  // (?) the launch rule is not decompiled

	// ---- the AI hooks of the strategy script
	def("ctw_ai_move_army", [cg, slot, node](NativeCall& c) {
		const int s = slot(c.arg_str(0)), a = node(c.arg_str(1)), b = node(c.arg_str(2));
		if (!cg() || s < 0 || a < 0 || b < 0) return make_int(-1);
		for (ctw::Piece& p : cg()->pieces[size_t(s)]) {
			if (p.node == a && !cg()->has_army(b)) {
				p.node = b;
				p.moved = true;
				return make_int(1);
			}
		}
		return make_int(0);
	});
	def("ctw_ai_set_attack", [cg, slot, node](NativeCall& c) {
		const int s = slot(c.arg_str(0)), a = node(c.arg_str(1)), b = node(c.arg_str(2));
		if (!cg() || s < 0 || a < 0 || b < 0) return make_int(-1);
		ctw::LinkChoice l;
		l.attacker = s;
		l.from = a;
		l.to = b;
		l.defender = cg()->node_eff_owner(b);
		const auto& N = cg()->nodes[size_t(a)];
		for (size_t k = 0; k < N.links.size(); k++) {
			if (N.links[k] == b) l.sea = N.link_sea[k] != 0;
		}
		cg()->pending_script_attack = l;
		return make_int(1);
	});
}

}  // namespace ron::sim::sf
