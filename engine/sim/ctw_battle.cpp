// Conquer the World: from a link to a battle and back (docs/ctw-spec.md sections 5 and 6): the style pick, the
// GameInfo-shaped battle description, the engine build (new_random_game for generated maps, Game::load for the
// campaign's custom .scx maps), the tactical cards' battle-side effects and the outcome rules.
//
// Deviations / marks:
//  - conquest map styles 24-26 (MapConquest, the sea variant, MapScenario) are not in the generator: land links use the
//    "Random Land" picker (map style 1), sea links "Random Sea" (2), custom maps are loaded from their .scx.
//    TODO(ctw 24-26): MapConquest::make (the node-shaped terrain), check_starting_loc / check_sea_map.
//  - the battle seed: the original reuses conquest_game.seed for every battle; here it is derived per link and round
//    (seed, from, to, round) so two battles of one campaign are not the same map. (Deviation, documented.)
//  - reinforcement armies (setup_reinforcements) are computed into `reinforcements` but not spawned in the battle:
//    TODO(ctw) the edge spawns (find_reinforcement_loc / find_angle).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "assets/scenario.hpp"
#include "sim/ai.hpp"
#include "sim/ctw.hpp"
#include "sim/game.hpp"
#include "sim/heroes.hpp"
#include "sim/production.hpp"
#include "sim/rules_xml.hpp"
#include "sim/script_host.hpp"
#include "sim/search.hpp"

namespace ron::sim::ctw {

namespace {

std::string trim_s(std::string s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
	return s.substr(a, b - a);
}

// rules.xml category index of a name in one of the lists (trimmed, case-insensitive), -1.
int cat_idx(const sim::Rules& r, const char* var, const std::string& name) {
	const CategoryList* cl = r.category_list(var);
	if (!cl) return -1;
	const std::string t = lower(trim_s(name));
	for (size_t i = 0; i < cl->items.size(); i++) {
		if (lower(trim_s(to_utf8(cl->items[i].name))) == t || lower(trim_s(to_utf8(cl->items[i].key))) == t) return int(i);
	}
	return -1;
}

int find_type_named(const sim::Rules& r, int lo, int hi, const std::string& name) {
	const std::string t = lower(trim_s(name));
	for (int i = lo; i < hi; i++) {
		const Type& T = r.types[size_t(i)];
		if (!T.loaded) continue;
		if (lower(to_utf8(T.name)) == t || lower(to_utf8(T.type_name)) == t || lower(to_utf8(T.graph_name)) == t) return i;
	}
	return -1;
}

}  // namespace

// ---------------------------------------------------------------- style pick (5.1)

StyleChoice ConquestGame::pick_style(const LinkChoice& l) {
	StyleChoice out;
	const bool player_attacks = l.attacker == player_tribe;
	int cat = player_attacks ? (l.defender < 0 ? int(kBarbarian) : int(kOffensive)) : int(kDefensive);
	out.category = cat;
	const Node& T = nodes[size_t(l.to)];
	const bool is_capital = (T.flags & kCapital) != 0;
	const int a = int(l.att_supporters.size()), d = int(l.def_supporters.size());
	const bool balanced = a == d;
	const bool allied_to_player = (player_attacks ? a : d) > 0;
	// the link's own seeded Random: deterministic per link
	Random lr(seed + uint32_t(l.to) * 0x25u + uint32_t(l.from) * 0x11u);
	struct Cand {
		int idx, key;
		bool def;
	};
	std::vector<Cand> cand;
	bool any_special = false;
	for (size_t i = 0; i < style_table.size(); i++) {
		const StyleDef& s = style_table[i];
		if (s.category != cat) continue;
		bool ok;
		if (s.is_default) ok = true;
		else ok = !is_capital && T.level <= K(*rules, "ctw_max_infra_special_scen") && balanced && !allied_to_player;
		if (!ok) continue;
		if (!(has_fought_barbarian || !s.not_first_time)) continue;
		if (s.min_age > cur_age) continue;
		if (l.sea ? s.land_only : s.water_only) continue;
		cand.push_back({int(i), lr.get(0, 101), s.is_default});
		if (!s.is_default) any_special = true;
	}
	if (any_special) cand.erase(std::remove_if(cand.begin(), cand.end(), [](const Cand& c) { return c.def; }), cand.end());
	if (cand.empty()) {
		// a campaign without a fitting style: the first default style of the category, else the default Conquest style
		for (size_t i = 0; i < style_table.size(); i++) {
			if (style_table[i].category == cat && style_table[i].is_default) cand.push_back({int(i), 0, true});
		}
		if (cand.empty() && !style_table.empty()) cand.push_back({0, 0, true});
	}
	if (cand.empty()) return out;
	const Cand* best = &cand[0];
	for (const Cand& c : cand) {
		if (c.key < best->key) best = &c;
	}
	out.index = best->idx;
	out.category = style_table[size_t(best->idx)].category;
	// the campaign's battle script may name a CUSTOM style (a named historical battle)
	const std::string custom = run_battle_override(l, &out);
	if (!custom.empty()) {
		for (size_t i = 0; i < style_table.size(); i++) {
			const StyleDef& s = style_table[i];
			if (s.category == kCustom && (ieq(s.id, custom) || ieq(s.title, custom))) {
				out.index = int(i);
				out.category = kCustom;
				out.custom_id = custom;
			}
		}
	}
	style_index1 = out.category;
	style_index2 = out.index;
	return out;
}

// ---------------------------------------------------------------- the battle description (5.2)

ConquestGame::BattleDef ConquestGame::make_battle(const LinkChoice& l, const StyleChoice& style) {
	BattleDef B;
	B.link = l;
	B.style = style;
	static const StyleDef kFallback = [] {
		StyleDef s;
		s.title = "Conquest";
		s.is_default = true;
		s.min_victory = s.max_victory = 90;
		s.attr["eliminations"] = "Capital";
		s.attr["victories"] = "Time Limit";
		return s;
	}();
	const StyleDef& st = style.index >= 0 && size_t(style.index) < style_table.size() ? style_table[size_t(style.index)] : kFallback;
	const bool player_attacks = l.attacker == player_tribe;
	B.setup_script = st.setup;
	B.runtime_script = st.runtime;
	B.bonus_script = st.bonus;
	B.objective = st.objective;
	B.description = st.description;
	B.victory_text = st.victory_text;
	B.defeat_text = st.defeat_text;
	B.defender_level = nodes[size_t(l.to)].level;
	const int other_slot = player_attacks ? l.defender : l.attacker;
	const int other_node = player_attacks ? l.to : l.from;
	const int player_node = player_attacks ? l.from : l.to;
	auto add_seat = [&](int slot, int team, bool human, int node) {
		BattleSeat s;
		s.slot = slot;
		s.team = team;
		s.human = human;
		s.barbarian = slot < 0;
		s.node = node;
		s.name = slot >= 0 ? leaders[size_t(slot)].name : "Barbarians";
		B.seats.push_back(s);
	};
	add_seat(player_tribe, 0, true, player_node);
	add_seat(other_slot, 1, false, other_node);
	allied_punks.clear();
	if (st.is_default) {
		auto supporters = [&](const std::vector<int>& list, int team) {
			for (int s : list) {
				if (B.seats.size() >= 8) break;
				add_seat(s, team, false, -1);
			}
		};
		supporters(l.att_supporters, player_attacks ? 0 : 1);
		supporters(l.def_supporters, player_attacks ? 1 : 0);
	} else {
		for (int s : (player_attacks ? l.att_supporters : l.def_supporters)) allied_punks.push_back(s);
	}
	for (int k = 0; k < st.add_opponents[size_t(difficulty)] && B.seats.size() < 8; k++) add_seat(-1, 1, false, -1);
	if (st.opp_team_together) {
		for (BattleSeat& s : B.seats) {
			if (s.team != 0) s.team = 1;
		}
	}
	const int n = int(B.seats.size());
	// ---- the engine setup
	RandomGameSetup S = default_random_setup(std::max(2, n));
	S.ctw = true;
	S.map_style = l.sea ? 2 : 1;  // TODO(ctw 24-26): MapConquest / MapConquestSea
	// map size: 2-3 players -> 3, >= 4 -> 4, +1 for a sea link, or the style's `mapsizes`
	S.map_size = n >= 4 ? 4 : 3;
	if (l.sea) S.map_size = std::min(6, S.map_size + 1);
	if (!st.str("mapsizes").empty()) {
		const int v = cat_idx(*rules, "map_sizes", st.str("mapsizes"));
		if (v >= 0) S.map_size = v;
	}
	S.seed = seed * 2654435761u + uint32_t(l.to) * 0x9e3779b1u + uint32_t(l.from) * 0x85ebca6bu + uint32_t(cur_round) * 0xc2b2ae35u + 1u;
	B.seed = S.seed;
	S.starting_town = player_attacks ? 2 : 1;
	// build_cities by conquest map style: No City and Custom start without cities (nomads); the Player / Enemy City styles
	// (one side only) are approximated by the common start (the generator has one starting_town per game).
	if (st.map_style == kMsNoCity || st.map_style == kMsCustom) S.starting_town = 0;
	S.starting_technology = std::clamp(cur_age, 0, 7);
	S.ending_technology = std::clamp(cur_age + (cur_age < ending_age ? 1 : 0), 0, 7);
	S.game_rules = 1;
	S.reveal_map = 1;
	S.victory = 4;      // Time Limit unless the style says otherwise
	S.elimination = 1;  // Capital
	auto set_cat = [&](const char* attr_name, const char* var, int& dst) {
		const std::string v = st.str(attr_name);
		if (v.empty()) return;
		const int i = cat_idx(*rules, var, v);
		if (i >= 0) dst = i;
	};
	set_cat("eliminations", "eliminations", S.elimination);
	set_cat("victories", "victories", S.victory);
	set_cat("revealmaps", "reveal_maps", S.reveal_map);
	set_cat("rushrules", "rush_rules", S.rush_rules);
	set_cat("startingresources", "starting_resources", S.starting_resources);
	{
		const std::string gr = st.str("gamerules");
		if (!gr.empty()) {
			const int i = cat_idx(*rules, "game_rules", gr);
			if (i >= 0) S.game_rules = i;
		}
	}
	if (st.str("rushrules") == "random" || lower(st.str("rushrules")) == "random") S.rush_rules = 11 + rget(0, 2);
	// time limit: custom minutes rand(min..max) from the battle's own stream
	Random br(S.seed ^ 0x5bd1e995u);
	int minutes = K(*rules, "ctw_conquest_time_limit");
	if (st.max_victory > 0) minutes = st.min_victory + (st.max_victory > st.min_victory ? br.get(0, st.max_victory - st.min_victory + 1) : 0);
	B.time_limit_min = minutes;
	S.time_limit = 8;  // custom
	// seats
	std::vector<int> used;
	for (const BattleSeat& s : B.seats) {
		if (s.slot >= 0) used.push_back(s.slot);
	}
	for (int i = 0; i < n; i++) {
		BattleSeat& s = B.seats[size_t(i)];
		NationSetup& ns = S.nation[size_t(i)];
		int tribe = s.slot;
		if (tribe < 0) {  // barbarian seat: the first free tribe index
			for (int t = 0; t < kSlots; t++) {
				if (std::find(used.begin(), used.end(), t) == used.end()) {
					tribe = t;
					used.push_back(t);
					break;
				}
			}
		} else {
			// two seats of one nation (a supporter that is also the main opponent): give the second a free tribe
			int count = 0;
			for (int k = 0; k < i; k++) count += B.seats[size_t(k)].slot == tribe;
			if (count > 0) {
				for (int t = 0; t < kSlots; t++) {
					if (std::find(used.begin(), used.end(), t) == used.end()) {
						tribe = t;
						used.push_back(t);
						break;
					}
				}
			}
		}
		ns.tribe = tribe;
		ns.human = s.human;
		ns.team = s.team == 0 ? 0 : 1;
		ns.difficulty = difficulty;
		ns.color = i;
		s.slot = s.slot;  // the campaign nation stays in the seat; `ns.tribe` is the engine tribe
	}
	S.num_nations = n;
	if (st.map_style == kMsCustom && !st.custom_map.empty()) B.custom_map = st.custom_map;
	const bool attacker_tileset = st.map_style == kMsAttacker;
	B.tileset = nodes[size_t(attacker_tileset ? l.from : l.to)].tileset;
	// armies at the start (start_add_armies): the player's attacking army / the defended node's army, +1 per Mercenaries card
	B.start_armies.fill(0);
	B.start_armies[0] = 1;
	B.start_armies[1] = 1;
	for (const Card& c : leaders[size_t(player_tribe)].selected) {
		if (c.type == kMercenaries) B.start_armies[0]++;
	}
	for (int i = 2; i < n; i++) B.start_armies[size_t(i)] = 1;
	B.defender_default = st.is_default;
	B.setup = S;
	// setup_reinforcements @789d70: every neighbour of the defended node whose owner is the attacker's (with an army) or the
	// defender's (with an army) sends one army; arrival times accumulate per side, rand(min..max) minutes each.
	// (Recorded in the campaign state; the battle does not spawn them yet: TODO(ctw) find_reinforcement_loc / find_angle.)
	reinforcements.clear();
	{
		int total[2] = {0, 0};
		Random rr(S.seed ^ 0x1f123bb5u);
		for (int n2 : nodes[size_t(l.to)].links) {
			if (n2 == l.from || !has_army(n2)) continue;
			const int o2 = node_eff_owner(n2);
			int side = -1;
			if (o2 == l.attacker || (l.attacker >= 0 && o2 >= 0 && stance(l.attacker, o2) >= kAlly)) side = player_attacks ? 0 : 1;
			else if (l.defender >= 0 && (o2 == l.defender || (o2 >= 0 && stance(l.defender, o2) >= kAlly))) side = player_attacks ? 1 : 0;
			if (side < 0) continue;
			total[side] += rr.get(crules->min_arrival_time, crules->max_arrival_time + 1);
			Reinforcement r;
			r.time = total[side] * 60;
			r.side = side;
			r.node = n2;
			reinforcements.push_back(r);
		}
	}
	return B;
}

// ---------------------------------------------------------------- building the battle

namespace {

// Gains `n` technologies of library line `line` (0 military, 1 civic, 2 commerce, 3 science) the leader can research next.
void gain_line_techs(Game& g, int who, int line, int n) {
	for (int k = 0; k < n; k++) {
		int pick = -1;
		for (int t = 579; t < 629 && pick < 0; t++) {
			const Type& T = g.rules->types[size_t(t)];
			if (!T.loaded || T.cat != line || has_tech(g, who, t)) continue;
			if (is_gov_type(t)) continue;
			if (T.age > g.leaders[size_t(who)].age) continue;
			if (tribe_can_type(g, who, t) == 0) continue;
			pick = t;
		}
		if (pick < 0) return;
		for (int q = 0; q < 3; q++) {
			const int p = get_preq(g, pick, q, who);
			if (p >= 579 && p < 629 && !has_tech(g, who, p)) gain_tech(g, who, p, 0, 0, false, true);
		}
		gain_tech(g, who, pick, 0, 0, false, true);
	}
}

// The seat's first city (position and slot), or null.
const Build* seat_city(const Game& g, int who) {
	const ObjectList& ol = g.objects[size_t(who)];
	for (int32_t o = kBuildBase; o < ol.build_mark; o++) {
		const Build* b = g.build(who, o);
		if (b && b->alive() && (b->flags & objflag::kCity)) return b;
	}
	return nullptr;
}

int32_t place_unit_near(Game& g, int who, int type, int32_t x, int32_t y) {
	if (type < 0) return -1;
	const Type& T = g.type(type);
	if (!T.loaded) return -1;
	int32_t ox = x, oy = y;
	if (find_nearby_spot(g, T, x, y, &ox, &oy, 0, 4 * 768, 0, 0x80000000u, 0, -1, -1, 0, 0, -1, 0, -1) != 0) {
		ox = x;
		oy = y;
	}
	return init_unit(g, who, type, ox, oy);
}

}  // namespace

std::unique_ptr<sim::Game> ConquestGame::build_battle(const GameFiles& F, const sim::Rules& R, const BattleDef& B,
                                                      const sim::AnimSource* anims, sim::RandomExtras* extras, std::string* error) const {
	RandomExtras local;
	RandomExtras& X = extras ? *extras : local;
	std::unique_ptr<Game> gp;
	const int n = int(B.seats.size());
	if (B.custom_map.empty()) {
		gp = new_random_game(F, R, anims, B.setup, &X);
		if (!gp) {
			if (error) *error = X.error;
			return nullptr;
		}
	} else {
		const std::string path = F.resolve(B.custom_map);
		if (path.empty()) {
			if (error) *error = "custom map " + B.custom_map + " is missing";
			return nullptr;
		}
		Scenario sc = parse_scenario(read_file(path));
		for (ScenarioPlayer& p : sc.players) {
			if (p.who < 0 || p.who >= n) continue;
			const BattleSeat& s = B.seats[size_t(p.who)];
			p.tribe = B.setup.nation[size_t(p.who)].tribe;
			p.is_human = s.human ? 4 : 0;
			p.difficulty = difficulty;
			p.name = s.name;
		}
		gp = std::make_unique<Game>();
		gp->anims = anims;
		gp->ctw = true;
		gp->info.difficulty = uint8_t(difficulty);
		gp->load(F, sc, R, B.seed);
		X = RandomExtras();
		X.setup = B.setup;
		X.name = sc.name;
		X.scenario = sc;
		script_host_load(*gp, F, path, sc);
	}
	for (auto& sl : start_locs) sl = {-1, -1};
	if (B.custom_map.empty()) {
		// get_starting_loc_x / y: the seat's start in tiles (a city's tile, else its start cell's centre: cells are 4 tiles)
		for (const mapgen::EmpireEntry& e : X.result.empire) {
			if (e.who < 0 || e.who >= 8 || start_locs[size_t(e.who)][0] >= 0) continue;
			if (e.kind == mapgen::EmpireEntry::City && e.x >= 0) start_locs[size_t(e.who)] = {e.x / 192, e.y / 192};
			else if (e.start >= 0 && size_t(e.start) < X.result.starts.size())
				start_locs[size_t(e.who)] = {X.result.starts[size_t(e.start)][0] * 4 + 2, X.result.starts[size_t(e.start)][1] * 4 + 2};
		}
	}
	Game& g = *gp;
	g.ctw = true;
	g.info.difficulty = uint8_t(difficulty);
	if (g.scripts) g.scripts->ctw = const_cast<ConquestGame*>(this);
	g.info.starting_technology = B.setup.starting_technology;
	g.info.ending_technology = B.setup.ending_technology;
	g.scen.custom_time_limit = B.time_limit_min;
	g.info.time_limit = 8;
	g.info.reveal_map = uint8_t(B.setup.reveal_map);
	if (B.setup.reveal_map == 2) {
		// the style asks for an explored map: reveal for everyone (reveal_map 2 = Explored)
	}
	// seat names and leader names from the campaign
	for (int i = 0; i < n && i < 8; i++) {
		const BattleSeat& s = B.seats[size_t(i)];
		if (!(g.leaders[size_t(i)].flags & leaderflag::kValid)) continue;
		if (s.human) g.leaders[size_t(i)].name = player_name;
		else if (s.slot >= 0) g.leaders[size_t(i)].name = leaders[size_t(s.slot)].name;
		else g.leaders[size_t(i)].name = "Barbarians";
	}
	// diplomacy: the campaign stances of the seats (setup_scenario_diplo): same team allied, others at war
	for (int a = 0; a < n && a < 8; a++) {
		for (int b = 0; b < n && b < 8; b++) {
			if (a == b) continue;
			const int sa = B.seats[size_t(a)].slot, sb = B.seats[size_t(b)].slot;
			const int ta = B.seats[size_t(a)].team, tb = B.seats[size_t(b)].team;
			int d = ta == tb ? 2 : 0;
			if (sa >= 0 && sb >= 0 && ta != tb && stance(sa, sb) == kPeace && false) d = 1;
			leader_set_diplo(g, a, b, d);
		}
	}
	// rare resources and production bonuses of the nations (assign_bonuses / setup_scenario_rares)
	for (int i = 0; i < n && i < 8; i++) {
		const int s = B.seats[size_t(i)].slot;
		if (s < 0) continue;
		const Leader& L = leaders[size_t(s)];
		sim::Leader& GL = g.leaders[size_t(i)];
		for (int k = 0; k < 6; k++) GL.base_rate[size_t(k)] += L.base_rate[size_t(k)] << 4;  // set_base_rate: n << 4
		for (int rg : L.rare_resources) {
			if (rg >= 6 && rg < 50) GL.rare[size_t((rg - 6) >> 3)] |= uint8_t(1u << ((rg - 6) & 7));
		}
		for (const Card& c : L.selected) {
			if (c.type == kOath) GL.conquest_racial_powers[size_t(c.variant >> 3) & 3] |= uint8_t(1u << (c.variant & 7));
		}
	}
	// the defender's build-out and the resource multiplier (default styles only)
	const bool player_attacks = B.link.attacker == player_tribe;
	if (B.defender_default) {
		const int def_seat = player_attacks ? 1 : 0;
		const int diff_row = player_attacks ? difficulty : crules->default_build_diff;
		const int level = std::clamp(B.defender_level, 0, 7);
		const Build* city = seat_city(g, def_seat);
		if (city) {
			const auto& cities = crules->build_out[size_t(diff_row)][size_t(level)];
			if (!cities.empty()) {
				for (const BuildItem& bi : cities[0]) {
					int t = find_type_named(*rules, kBuilds, kEndBuilds, bi.type);
					if (t < 0) continue;
					if (rules->types[size_t(t)].age > cur_age) continue;
					for (int k = 0; k < bi.num; k++) {
						const bool exists = g.leaders[size_t(def_seat)].num_buildings[size_t(t - kBuilds)] > 0;
						int use = t;
						if (exists && !bi.alt.empty()) {
							const int alt = find_type_named(*rules, kBuilds, kEndBuilds, bi.alt);
							if (alt >= 0) use = alt;
						}
						produce_building(g, def_seat, use, city->o, 0);
					}
				}
			}
		}
		const int mult = crules->resource_bonus[size_t(diff_row)][size_t(level)];
		if (mult > 1) {
			for (int k = 0; k < 6; k++) g.leaders[size_t(def_seat)].bucket[size_t(k)] *= mult;
		}
	}
	// armies: one copy of the ARMIES composition per army at the start, upgraded to the nation's current unit
	for (int i = 0; i < n && i < 8; i++) {
		const Build* city = seat_city(g, i);
		int32_t cx = 0, cy = 0;
		if (city) {
			cx = city->x;
			cy = city->y;
		} else {
			bool found = false;
			const ObjectList& ol = g.objects[size_t(i)];
			for (int32_t o = 0; o < ol.unit_mark && !found; o++) {
				const Unit* u = g.unit(i, o);
				if (u && u->alive()) {
					cx = u->x;
					cy = u->y;
					found = true;
				}
			}
			if (!found) continue;
		}
		for (int a = 0; a < B.start_armies[size_t(i)]; a++) {
			for (const ArmyUnit& au : crules->army) {
				if (au.min_diff > difficulty && !B.seats[size_t(i)].human) continue;
				int t = find_type_named(*rules, kUnits, kEndUnits, au.type);
				if (t < 0) continue;
				const int up = current_upgrade(g, i, t);
				if (up >= kUnits && up < kEndUnits) t = up;
				for (int k = 0; k < au.num; k++) place_unit_near(g, i, t, cx, cy);
			}
		}
	}
	// the hero of the player's nation (default-style battles, unless the style says no_hero)
	if (B.defender_default && !B.link.sea && !def->hero_unit.empty()) {
		const int ht = find_type_named(*rules, kUnits, kEndUnits, def->hero_unit);
		const Build* city = seat_city(g, 0);
		if (ht >= 0 && city) place_unit_near(g, 0, ht, city->x, city->y);
	}
	// ---- the style's scripts: the runtime script runs every frame (as the scenario script), the setup script once now
	if (g.scripts) {
		ScriptHost& h = *g.scripts;
		auto base_name = [](const std::string& path) {
			std::string p = path;
			for (char& c : p) {
				if (c == '\\') c = '/';
			}
			const size_t slash = p.find_last_of('/');
			if (slash != std::string::npos) p = p.substr(slash + 1);
			const size_t dot = p.find_last_of('.');
			return dot == std::string::npos ? p : p.substr(0, dot);
		};
		std::string err;
		if (!B.runtime_script.empty()) {
			if (h.compile(B.runtime_script, &err)) h.scenario_script = bhs::to_u16(base_name(B.runtime_script));
			else std::fprintf(stderr, "ctw: runtime script %s: %s\n", B.runtime_script.c_str(), err.c_str());
		}
		if (!B.setup_script.empty()) {
			if (h.compile(B.setup_script, &err)) h.run(bhs::to_u16(base_name(B.setup_script)), {});
			else std::fprintf(stderr, "ctw: setup script %s: %s\n", B.setup_script.c_str(), err.c_str());
		}
	}
	// ---- tactical cards (setup_scenario_bonuses)
	const Leader& P = leaders[size_t(player_tribe)];
	for (const Card& c : P.selected) {
		sim::Leader& GL = g.leaders[0];
		switch (c.type) {
		case kEureka: {
			const int node = player_attacks ? B.link.from : B.link.to;
			if (c.variant == 4) {
				for (int line = 0; line < 4; line++) gain_line_techs(g, 0, line, K(*rules, "ctw_eureka_all_techs"));
			} else {
				const int line = c.variant >= 0 && c.variant < 4 ? c.variant : nodes[size_t(node)].tech_cat;
				gain_line_techs(g, 0, line, K(*rules, "ctw_eureka_techs"));
			}
			break;
		}
		case kEconomicBoom:
			if (c.variant >= 0 && c.variant < 6) GL.num_ctw_rate_bonuses[size_t(c.variant)]++;
			break;
		case kSkillBonus:
			for (int k = 0; k < 6; k++) GL.bucket[size_t(k)] += 100;  // (?) the amount comes from the style's bonus script
			break;
		case kSpyNetwork: GL.flags |= 0x1000; break;
		case kCulturalDominance:
			for (int i = 1; i < n && i < 8; i++) {
				if (B.seats[size_t(i)].team != 0) g.leaders[size_t(i)].flags2 |= 0x40;
			}
			break;
		case kPoliticalDissidents:
			for (int i = 1; i < n && i < 8; i++) {
				if (B.seats[size_t(i)].team != 0) g.leaders[size_t(i)].flags2 |= 0x800;
			}
			break;
		case kPropaganda:
			// gain the enemy's nation power: the opponent's tribe bit
			if (B.seats.size() > 1 && B.seats[1].slot >= 0) {
				const int sl = B.seats[1].slot;
				GL.conquest_racial_powers[size_t(sl >> 3) & 3] |= uint8_t(1u << (sl & 7));
			}
			break;
		case kWarpath:
			gain_line_techs(g, 0, 0, 2);
			if (const Build* city = seat_city(g, 0)) {
				for (const char* bn : {"Barracks", "Stable", "Siege Factory"}) {
					const int t = find_type_named(*rules, kBuilds, kEndBuilds, bn);
					if (t >= 0) produce_building(g, 0, t, city->o, 0);
				}
			}
			break;
		default: break;
		}
		if (c.type >= 0 && c.type < kNumCardTypes && GL.num_bonus_cards[size_t(c.type)] < 255) GL.num_bonus_cards[size_t(c.type)]++;
	}
	return gp;
}

// ---------------------------------------------------------------- outcomes (6)

void ConquestGame::process_results(int kind, bool player_won, const LinkChoice& l, const StyleChoice& style) {
	Leader& P = leaders[size_t(player_tribe)];
	const bool player_attacks = l.attacker == player_tribe;
	if (style.index >= 0 && size_t(style.index) < style_table.size()) style_table[size_t(style.index)].times_played++;
	// the selected tactical cards are consumed (General Parmenio survives its first play)
	for (const Card& c : P.selected) {
		const bool first = P.bonus_card_types_played[size_t(c.type)] == 0;
		P.bonus_card_types_played[size_t(c.type)]++;
		if (c.type == kParmenio && first) P.cards.push_back(c);
	}
	P.selected.clear();
	if (kind == 2) player_won = false;
	if (kind == 1) player_won = player_attacks;  // an overrun: the attacker wins without a battle
	// wonders nobody owns any more are released
	assign_bonuses();
	auto move_in = [&](int slot) {
		for (Piece& p : pieces[size_t(slot)]) {
			if (p.node == l.from) {
				p.node = l.to;
				p.moved = true;
				return;
			}
		}
	};
	if (player_attacks) {
		attacked_this_round = true;
		turns_skipped = 0;
		if (player_won) {
			const bool client = l.defender >= 0 && leaders[size_t(l.defender)].base_armies == 0 && def->allow_vassals;
			if (client) {
				set_vassal(player_tribe, l.defender);
				add_news("The " + tribe_name(l.defender) + " submit to you as vassals.");
				for (Piece& p : pieces[size_t(player_tribe)]) {
					if (p.node == l.from) p.moved = true;
				}
			} else {
				std::string text;
				transfer_to(l.to, player_tribe, true, &text);
				move_in(player_tribe);
				add_news("You conquer " + node_name(l.to) + "." + text);
			}
			if (l.defender < 0) has_fought_barbarian = true;
			place_new_armies(player_tribe);
			return_to_node = l.to;
			check_income(player_tribe);
		} else {
			add_news("Your invasion of " + node_name(l.to) + " failed.");
			return_to_node = l.from;
			for (Piece& p : pieces[size_t(player_tribe)]) {
				if (p.node == l.from) p.moved = true;
			}
			if (l.defender >= 0) give_tribute(l.defender, K(*rules, "ctw_def_victory_bonus"));
		}
	} else {
		// the player defends against l.attacker
		if (player_won) {
			add_news("You repel the " + tribe_name(l.attacker) + " at " + node_name(l.to) + ".");
			need_defensive_bonus = true;
			// the failed attacker's army is lost
			pieces[size_t(l.attacker)].erase(std::remove_if(pieces[size_t(l.attacker)].begin(), pieces[size_t(l.attacker)].end(),
			                                                [&](const Piece& p) { return p.node == l.from; }),
			                                 pieces[size_t(l.attacker)].end());
		} else {
			std::string text;
			transfer_to(l.to, l.attacker, true, &text);
			move_in(l.attacker);
			add_news("The " + tribe_name(l.attacker) + " take " + node_name(l.to) + " from you.");
			if (leaders[size_t(player_tribe)].active) place_new_armies(player_tribe);
		}
	}
	if (l.defender >= 0 && l.attacker >= 0) {
		leaders[size_t(l.defender)].fought_mask |= 1 << (l.attacker & 15);
		leaders[size_t(l.attacker)].fought_mask |= 1 << (l.defender & 15);
	}
	assign_bonuses();
	check_allied_victory();
}

void ConquestGame::resolve_player_defense(bool player_won) {
	process_results(0, player_won, cur_link, cur_style);
	cur_link = LinkChoice();
}

}  // namespace ron::sim::ctw
