// Conquer the World: from a link to a battle and back (docs/ctw-spec.md sections 5 and 6): the style pick, the
// GameInfo-shaped battle description, the engine build (new_random_game for generated maps, Game::load for the
// campaign's custom .scx maps), the tactical cards' battle-side effects and the outcome rules.
//
// Deviations / marks:
//  - conquest map styles 24-26: generated battles use MapConquest (24) or MapConquestSea (25, a sea link) of the generator
//    (mapgen_conquest.cpp, docs/randommap-terrain-groups.md B3.13): conquest_battle_input below draws the square around the two countries from
//    the silhouette pieces (conquest/RenderMaps/MapPieces/<map>/xpack_temp_country<N><map>.png) and build_battle keeps it in a
//    mapgen::ConquestScope while new_random_game runs. RandomGameSetup clamps its map style to the shipped ones, so BattleDef.setup keeps the
//    "Random Land" / "Random Sea" pickers (1 / 2) and the scope upgrades them. Style 26 (MapScenario) is the campaign's custom .scx, loaded as
//    a scenario. Without the piece files the battle falls back to the plain pickers.
//  - the battle seed: conquest_game.seed for every battle, as build_game does; ConquestGame::vary_battle_seed derives one per link
//    and round instead (an option, off by default).
//  - reinforcement armies: ctw_reinf.cpp; the tactical cards: ctw_tactical.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "assets/png.hpp"
#include "assets/scenario.hpp"
#include "sim/ai.hpp"
#include "sim/ctw.hpp"
#include "sim/game.hpp"
#include "sim/heroes.hpp"
#include "sim/mapgen.hpp"
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
	battle_game = nullptr;
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
	S.map_style = l.sea ? 2 : 1;  // upgraded to MapConquest / MapConquestSea by build_battle's ConquestScope
	// map size: 2-3 players -> 3, >= 4 -> 4, +1 for a sea link, or the style's `mapsizes`
	S.map_size = n >= 4 ? 4 : 3;
	if (l.sea) S.map_size = std::min(6, S.map_size + 1);
	if (!st.str("mapsizes").empty()) {
		const int v = cat_idx(*rules, "map_sizes", st.str("mapsizes"));
		if (v >= 0) S.map_size = v;
	}
	// build_game: info.seed = conquest_game.seed for every battle (the option derives one per link and round instead)
	S.seed = vary_battle_seed ? seed * 2654435761u + uint32_t(l.to) * 0x9e3779b1u + uint32_t(l.from) * 0x85ebca6bu + uint32_t(cur_round) * 0xc2b2ae35u + 1u : seed;
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
	// RON_CTW_GENERATE=1 plays a custom-map battle on a generated conquest map instead (a test aid: the early battles of Alexander and Napoleon
	// are all scripted historical maps, so this is the only way to see the generator on them)
	if (st.map_style == kMsCustom && !st.custom_map.empty() && !std::getenv("RON_CTW_GENERATE")) B.custom_map = st.custom_map;
	const bool attacker_tileset = st.map_style == kMsAttacker;
	B.tileset = nodes[size_t(attacker_tileset ? l.from : l.to)].tileset;
	// the AI's tactical cards (select_tactical_cards): only the two main nations, only in the default styles, and not an easy
	// game's AI against the player (setup_scenario_bonuses @78be90). The picks use the campaign's stream.
	B.map_style = st.map_style;
	if (st.is_default) {
		for (const BattleSeat& s : B.seats) {
			if (&s - B.seats.data() >= 2 || s.slot < 0 || s.slot == player_tribe) continue;
			const int opponent = s.slot == l.attacker ? l.defender : l.attacker;
			if (difficulty < 2 && opponent == player_tribe) {
				for (const Card& c : leaders[size_t(s.slot)].selected) leaders[size_t(s.slot)].cards.push_back(c);
				leaders[size_t(s.slot)].selected.clear();
				continue;
			}
			select_ai_tactical(s.slot, l);
		}
	} else {
		for (const BattleSeat& s : B.seats) {
			if (s.slot >= 0 && s.slot != player_tribe) {
				for (const Card& c : leaders[size_t(s.slot)].selected) leaders[size_t(s.slot)].cards.push_back(c);
				leaders[size_t(s.slot)].selected.clear();
			}
		}
	}
	// armies at the start (start_add_armies): the attacking army / the defended node's army, +1 per Mercenaries card
	B.start_armies.fill(0);
	B.start_armies[0] = 1;
	B.start_armies[1] = 1;
	for (int i = 0; i < 2 && i < n; i++) {
		if (B.seats[size_t(i)].slot < 0) continue;
		for (const Card& c : leaders[size_t(B.seats[size_t(i)].slot)].selected) {
			if (c.type == kMercenaries) B.start_armies[size_t(i)]++;
		}
	}
	for (int i = 2; i < n; i++) B.start_armies[size_t(i)] = 1;
	B.defender_default = st.is_default;
	B.setup = S;
	// the seats' campaign nations for the reinforcement placement and the tribe names
	battle_slot.fill(-1);
	battle_human.fill(false);
	for (int i = 0; i < n && i < 8; i++) {
		battle_slot[size_t(i)] = B.seats[size_t(i)].slot;
		battle_human[size_t(i)] = B.seats[size_t(i)].human;
	}
	setup_reinforcements(l, st);  // setup_reinforcements @789d70: who sends an army and when (the campaign's stream)
	reinforce_mark = 0;
	reinf_arrived.fill(0);
	battle_notices.clear();
	return B;
}

// ---------------------------------------------------------------- the conquest map input (styles 24 / 25)

// Everything MapConquest::make_continents reads from the CtW layer for the battle B (docs/randommap-terrain-groups.md B3.13): the square image
// of the link's two countries (ConquestCountries::draw_scenario_area; the rectangle of get_scenario_map_rect with param_1 = the attacker's
// node, param_2 = the defended node), the start seats (start_who: the player's side is seat 0) and the extra opponents. Declared where it is
// used (build_battle, apps/mapgen). Returns false with `err` when a piece file is missing.
bool conquest_battle_input(const ConquestGame& g, const GameFiles& F, const ConquestGame::BattleDef& B, mapgen::ConquestInput* out, std::string* err) {
	if (!g.def) {
		if (err) *err = "no campaign";
		return false;
	}
	const CampaignDef& D = *g.def;
	const LinkChoice& l = B.link;
	const int a = l.to, b = l.from;  // link[0] = the defended node, link[1] = the attacker's
	if (a < 0 || b < 0 || size_t(a) >= D.countries.size() || size_t(b) >= D.countries.size() || a == b) {
		if (err) *err = "link without two distinct countries";
		return false;
	}
	mapgen::ConquestInput ci;
	ci.sea = l.sea;
	ci.node_a = a;
	ci.node_b = b;
	const mapgen::ConquestRect rect = mapgen::conquest_scenario_rect(D.countries[size_t(b)].x, D.countries[size_t(b)].y, D.countries[size_t(a)].x,
	                                                                  D.countries[size_t(a)].y, D.tex_width, B.map_style);
	std::vector<mapgen::ConquestCountry> cs;
	for (const CountryDef& c : D.countries) {
		mapgen::ConquestCountry k;
		k.left = c.left;
		k.right = c.right;
		k.top = c.top;
		k.bottom = c.bottom;
		k.cx = c.x;
		k.cy = c.y;
		cs.push_back(k);
	}
	std::map<int, Image> cache;
	std::string missing;
	auto piece = [&](int i) -> const Image* {
		auto it = cache.find(i);
		if (it == cache.end()) {
			Image img;
			const std::string rel = "conquest/RenderMaps/MapPieces/" + D.file + "/xpack_temp_country" + std::to_string(i) + D.file + ".png";
			const std::string path = F.resolve(rel);
			std::string e2;
			if (path.empty() || !load_png(path, img, &e2)) {
				if (missing.empty()) missing = path.empty() ? rel + " is missing" : e2;
				img = Image();
			}
			it = cache.emplace(i, std::move(img)).first;
		}
		return it->second.width > 0 ? &it->second : nullptr;
	};
	if (!mapgen::conquest_draw_scenario_area(rect, D.tex_width, g.map_scale(), cs, piece, ci)) {
		if (err) *err = "empty conquest map rectangle";
		return false;
	}
	if (!missing.empty()) {
		if (err) *err = missing;
		return false;
	}
	// start_who: seat 0 is the player. The first start (b, the attacker's node) is local_34 = (seat 0's nation == the defender), the second the other
	const int player_slot = B.seats.empty() ? g.player_tribe : B.seats[0].slot;
	ci.who_b = (l.defender >= 0 && l.defender == player_slot) ? 1 : 0;
	ci.who_a = 1 - ci.who_b;
	ci.swap = B.map_style == kMsAttackerDefense || B.map_style == kMsDefenderAttack;
	// seats 2..: the supporters (make_battle adds the attacker's side first, then the defender's: near_second = allied to the defended node's
	// owner), then the barbarian seats
	{
		size_t k = 2;
		if (B.defender_default) {
			for (int pass = 0; pass < 2; pass++) {
				const std::vector<int>& list = pass == 0 ? l.att_supporters : l.def_supporters;
				for (size_t i = 0; i < list.size() && k < B.seats.size(); i++, k++) {
					if (B.seats[k].barbarian) break;
					mapgen::ConquestSupporter sp;
					sp.near_second = pass == 1;
					ci.supporters.push_back(sp);
				}
			}
		}
		for (size_t i = 2; i < B.seats.size(); i++) ci.barbarians += B.seats[i].barbarian ? 1 : 0;
	}
	if (B.style.index >= 0 && size_t(B.style.index) < g.style_table.size()) ci.no_rare = g.style_table[size_t(B.style.index)].no_rare;
	ci.tileset = B.tileset;
	ci.valid = true;
	*out = std::move(ci);
	return true;
}

// ---------------------------------------------------------------- building the battle

namespace {

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
		mapgen::ConquestInput conquest;
		std::string conquest_err;
		const bool have_conquest = conquest_battle_input(*this, F, B, &conquest, &conquest_err);
		if (!have_conquest) std::fprintf(stderr, "ctw: no conquest map (%s): a plain random map is generated\n", conquest_err.c_str());
		mapgen::ConquestScope conquest_scope(have_conquest ? &conquest : nullptr);
		// The setup the battle is generated from also carries the conquest map input (and the CtW flag): the text form of X.setup is what a
		// native save of the battle keeps in its scenario slot, and loading it regenerates the same map (newgame.hpp, conquest_input_string).
		RandomGameSetup S = B.setup;
		S.ctw = true;
		if (have_conquest) S.conquest = conquest_input_string(conquest);
		gp = new_random_game(F, R, anims, S, &X);
		if (!gp) {
			if (error) *error = X.error;
			return nullptr;
		}
		{  // the text a save of the battle keeps: the resolved setup, the conquest input and the style's scripts
			RandomGameSetup T = X.setup;
			T.battle_runtime = B.runtime_script;
			T.battle_setup = B.setup_script;
			set_battle_setup_text(random_setup_string(T));
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
	// ---- tactical cards (setup_scenario_bonuses), rares, the card units, reinforcement entries
	apply_battle_cards(g, B);
	{
		ConquestGame& self = const_cast<ConquestGame&>(*this);
		self.battle_game = gp.get();
		self.locate_reinforcements(B.link, B.map_style, g.world.xs, g.world.ys);
		self.apply_tribe_names(const_cast<sim::Rules&>(R), B);
	}
	return gp;
}

// Tribe::set_substitute_tribe: the battle shows the campaign's nation names (slot 5 plays as the Macedonians) in the HUD. The
// names live in the rules the renderer reads (Rules::tribes[].name); the originals are kept and put back by restore_tribe_names.
void ConquestGame::apply_tribe_names(sim::Rules& r, const BattleDef& B) {
	restore_tribe_names(r);
	for (size_t i = 0; i < B.seats.size() && i < 8; i++) {
		const int slot = B.seats[i].slot;
		const int tribe = B.setup.nation[i].tribe;
		if (slot < 0 || tribe < 0 || size_t(tribe) >= r.tribes.size() || !def) continue;
		const std::string& nm = def->tribes[size_t(slot)].display_name;
		if (nm.empty()) continue;
		if (std::none_of(saved_names.begin(), saved_names.end(), [&](const auto& p) { return p.first == tribe; }))
			saved_names.push_back({tribe, r.tribes[size_t(tribe)].name});
		r.tribes[size_t(tribe)].name = decode_utf8(reinterpret_cast<const uint8_t*>(nm.data()), nm.size());
	}
}

void ConquestGame::restore_tribe_names(sim::Rules& r) {
	for (const auto& p : saved_names) {
		if (size_t(p.first) < r.tribes.size()) r.tribes[size_t(p.first)].name = p.second;
	}
	saved_names.clear();
}

// ---------------------------------------------------------------- outcomes (6)

void ConquestGame::process_results(int kind, bool player_won, const LinkChoice& l, const StyleChoice& style) {
	Leader& P = leaders[size_t(player_tribe)];
	const bool player_attacks = l.attacker == player_tribe;
	if (style.index >= 0 && size_t(style.index) < style_table.size()) style_table[size_t(style.index)].times_played++;
	// the selected tactical cards of both main nations are consumed (General Parmenio survives his first play)
	consume_selected(l.attacker, true);
	consume_selected(l.defender, true);
	consume_selected(player_tribe, true);
	if (kind == 2) player_won = false;
	if (kind == 1) player_won = player_attacks;  // an overrun: the attacker wins without a battle
	// the Skill Bonus card (get_bonus_amt): a won battle's style script rates the performance
	int bonus = 0;
	if (kind == 0 && player_won && style.index >= 0 && size_t(style.index) < style_table.size() && battle_game) {
		std::string txt;
		bonus = std::max(0, run_bonus_script(*battle_game, style_table[size_t(style.index)], &txt));
	}
	battle_game = nullptr;
	battle_bonus = bonus;
	if (bonus > 0) {
		Card sb;
		sb.type = kSkillBonus;
		sb.variant = bonus;
		P.cards.push_back(sb);
		add_news("Skill Bonus: +" + std::to_string(bonus) + " of every resource in a battle.");
	}
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
