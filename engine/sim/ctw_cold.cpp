// Conquer the World, Cold War (docs/ctw-spec.md 11): DEFCON, nuclear missiles and the espionage missions. Ported from
//   ScenarioFuncSet::ctw_ai_launch_nukes @a01b60   the AI's launch (the decision is the campaign script's, coldwar_diplo_offer.bhs),
//   ConquestGame::shall_we_play_a_game  @790be0    the launch sites and the targets of the AI's missiles,
//   ConquestGame::the_only_move_is_not_to_play @791160  the exchange's result on the map,
//   ConquestGame::get_nuke_tribes @791ec0, ConquestLeader::set_war / get_war_cost (DEFCON, ctw_diplo.cpp),
//   ScenarioFuncSet::ctw_build_nuke @a01e70, ConquestStyles::pick_style (espionage branch) and process_results.
//
// What the original leaves to the user interface: the player's choice to retaliate and the player's own targets. Here a
// retaliation uses the same targeting as the AI (docs/sim-implementation.md "Conquer the World"): a priority node first, then
// any other node, the capital last.
#include <algorithm>

#include "sim/ctw.hpp"

namespace ron::sim::ctw {

bool ConquestGame::nuke_tribes(int* p1, int* p2) const {
	if (!def || !def->strategic_nukes) return false;
	*p1 = player_tribe;
	*p2 = player_tribe == kUsa ? kUssr : kUsa;
	return true;
}

int ConquestGame::num_nukes(int slot) const {
	int n = 0;
	for (const Node& nd : nodes) n += nd.owner == slot ? nd.nukes : 0;
	return n;
}

// ctw_build_nuke(node, pay): the owner (effective: a vassal's master) builds one nuke on the node, at most 3 per node.
int ConquestGame::build_nuke(int node, bool pay) {
	if (nuke_building_disabled) return 0;
	if (node < 0 || size_t(node) >= nodes.size()) return -1;
	Node& N = nodes[size_t(node)];
	if (N.owner < 0) return -1;
	const int owner = eff(N.owner);
	if (owner < 0) return -1;
	const int cost = pay ? nuke_cost(owner) : 0;
	Leader& L = leaders[size_t(owner)];
	if (N.nukes < 3 && L.tribute >= cost) {
		L.tribute -= cost;
		N.nukes++;
		add_news("The " + tribe_name(owner) + " install a nuclear missile at " + node_name(node) + ".");
		return 1;
	}
	return 0;
}

// The next target of a missile of `attacker` among `victim`'s nodes: a priority node first, then any other node, the capital
// last; never a node already targeted. -1 when none is left.
static int pick_target(const ConquestGame& g, int victim, const std::vector<int>& taken) {
	auto targeted = [&](int n) { return std::find(taken.begin(), taken.end(), n) != taken.end(); };
	for (size_t i = 0; i < g.nodes.size(); i++) {
		const Node& n = g.nodes[i];
		if ((n.flags & kPriority) && n.owner == victim && !(n.flags & kCapital) && !targeted(int(i))) return int(i);
	}
	for (size_t i = 0; i < g.nodes.size(); i++) {
		const Node& n = g.nodes[i];
		if (n.owner == victim && !(n.flags & kCapital) && !targeted(int(i))) return int(i);
	}
	const int cap = g.leaders[size_t(victim)].capital;
	if (cap >= 0 && !targeted(cap)) return cap;
	return -1;
}

// shall_we_play_a_game(p1, p2) @790be0: the missiles of both nations are listed (one entry per missile, by launch node) and the
// AI side (p2's) gets its targets among p1's nodes; every aimed missile leaves its silo.
void ConquestGame::shall_we_play_a_game(int p1, int p2) {
	tribe_nukes.clear();
	tribe2_nukes.clear();
	tribe_nuke_targets.clear();
	tribe2_nuke_targets.clear();
	for (size_t i = 0; i < nodes.size(); i++) {
		const Node& n = nodes[size_t(i)];
		if (n.nukes == 0) continue;
		if (n.owner == p1) {
			for (int k = 0; k < n.nukes; k++) tribe_nukes.push_back(int(i));
		} else if (n.owner == p2) {
			for (int k = 0; k < n.nukes; k++) tribe2_nukes.push_back(int(i));
		}
	}
	// the node order list draws one key per node from the campaign stream (the list is then walked in insertion order)
	for (size_t i = 0; i < nodes.size(); i++) (void)rget(0, 10000);
	if (p1 >= 0) {
		for (int silo : tribe2_nukes) {
			const int t = pick_target(*this, p1, tribe2_nuke_targets);
			tribe2_nuke_targets.push_back(t);
			if (t >= 0) nodes[size_t(silo)].nukes--;
		}
	} else {
		tribe2_nuke_targets.assign(tribe2_nukes.size(), -1);
	}
}

// ScenarioFuncSet::ctw_ai_launch_nukes: the AI superpower launches. Its missiles get their targets; the player may answer in
// kind (the UI asks; finish_nuclear_exchange applies the exchange). Returns false when the campaign has no strategic nukes.
bool ConquestGame::ai_launch_nukes() {
	nuke_launch_turns = 0;
	int p1, p2;
	if (!nuke_tribes(&p1, &p2)) return false;
	shall_we_play_a_game(p1, p2);
	set_war(p1, p2);
	defcon_level = 5;
	nuke_pending = true;
	nuke_p1 = p1;
	nuke_p2 = p2;
	add_news("The " + tribe_name(p2) + " launch their nuclear missiles!");
	return true;
}

// the_only_move_is_not_to_play @791160 after the (optional) retaliation. `retaliate`: the player's missiles are aimed too.
void ConquestGame::finish_nuclear_exchange(bool retaliate) {
	if (!nuke_pending) return;
	nuke_pending = false;
	const int p1 = nuke_p1, p2 = nuke_p2;
	if (retaliate) {
		for (int silo : tribe_nukes) {
			const int t = pick_target(*this, p2, tribe_nuke_targets);
			tribe_nuke_targets.push_back(t);
			if (t >= 0) nodes[size_t(silo)].nukes--;
		}
	}
	int hits = 0;
	for (const std::vector<int>* list : {&tribe_nuke_targets, &tribe2_nuke_targets}) {
		for (int t : *list) {
			if (t < 0) continue;
			transfer_to(t, -1, false);
			nodes[size_t(t)].nuked = true;
			nodes[size_t(t)].nukes = 0;
			hits++;
		}
	}
	add_news(std::to_string(hits) + " territories are destroyed in the nuclear exchange.");
	tribe_nukes.clear();
	tribe2_nukes.clear();
	tribe_nuke_targets.clear();
	tribe2_nuke_targets.clear();
	const Leader& A = leaders[size_t(p1)];
	const Leader& B = leaders[size_t(p2)];
	if ((!A.active || !A.valid) && (!B.active || !B.valid)) {
		game_end = kArmageddon;
	} else {
		check_allied_victory();
		if (game_end == 0 && cur_round == length()) game_end = kTimeOver;
	}
	available_links = find_available_links(player_tribe);
}

// ---------------------------------------------------------------- espionage

// The espionage styles of the player's superpower: the category (4 USA, 5 USSR) and how many missions it has.
int ConquestGame::espionage_category() const { return player_tribe == kUsa ? int(kEspionageUSA) : int(kEspionageUSSR); }

int ConquestGame::espionage_missions() const {
	int n = 0;
	for (const StyleDef& s : style_table) n += s.category == espionage_category();
	return n;
}

bool ConquestGame::can_espionage(std::string* why) const {
	auto no = [&](const char* m) {
		if (why) *why = m;
		return false;
	};
	if (!def || !def->strategic_nukes || (player_tribe != kUsa && player_tribe != kUssr)) return no("This campaign has no espionage.");
	if (espionage_disabled) return no("Espionage is not available now.");
	if (cur_espionage_level >= espionage_missions()) return no("Every mission has been completed.");
	if (leaders[size_t(player_tribe)].tribute < K(*rules, "ctw_espionage_cost")) {
		if (why) *why = "A mission costs " + std::to_string(K(*rules, "ctw_espionage_cost")) + " tribute.";
		return false;
	}
	return true;
}

// Starts the next espionage mission: pays CTW_ESPIONAGE_COST and returns its style (the cur_espionage_level-th style of the
// player's superpower; ConquestStyles::pick_style, categories 4 / 5).
bool ConquestGame::begin_espionage(StyleChoice* out, std::string* why) {
	if (!can_espionage(why)) return false;
	int k = 0, found = -1;
	for (size_t i = 0; i < style_table.size(); i++) {
		if (style_table[i].category != espionage_category()) continue;
		if (k++ == cur_espionage_level) found = int(i);
	}
	if (found < 0) return false;
	leaders[size_t(player_tribe)].tribute -= K(*rules, "ctw_espionage_cost");
	StyleChoice sc;
	sc.category = espionage_category();
	sc.index = found;
	style_index1 = sc.category;
	style_index2 = found;
	*out = sc;
	return true;
}

// The battle of an espionage mission: a LinkChoice without territories (link < 0 in the original). The player is seat 0, the
// style's scenario map and scripts do the rest; the other side is the other superpower (the missions' own enemies come from the map).
ConquestGame::BattleDef ConquestGame::make_espionage_battle(const StyleChoice& style) {
	LinkChoice l;
	l.attacker = player_tribe;
	l.defender = player_tribe == kUsa ? kUssr : kUsa;
	l.from = leaders[size_t(player_tribe)].capital;
	l.to = leaders[size_t(l.defender)].capital;
	if (l.from < 0) l.from = 0;
	if (l.to < 0) l.to = 0;
	return make_battle(l, style);
}

// The result of an espionage mission (process_results, link < 0): a won mission advances the level; the style's bonus script
// may award a Skill Bonus card; a lost mission can be tried again.
void ConquestGame::process_espionage_results(bool won, const StyleChoice& style) {
	if (style.index >= 0 && size_t(style.index) < style_table.size()) style_table[size_t(style.index)].times_played++;
	Leader& P = leaders[size_t(player_tribe)];
	consume_selected(player_tribe, true);
	int bonus = 0;
	if (won) {
		cur_espionage_level++;
		if (style.index >= 0 && size_t(style.index) < style_table.size() && battle_game) {
			std::string txt;
			bonus = std::max(0, run_bonus_script(*battle_game, style_table[size_t(style.index)], &txt));
		}
	}
	battle_game = nullptr;
	battle_bonus = bonus;
	finished_espionage = true;
	if (bonus > 0) {
		Card sb;
		sb.type = kSkillBonus;
		sb.variant = bonus;
		P.cards.push_back(sb);
	}
	add_news(won ? "The mission is a success." : "The mission failed.");
}

}  // namespace ron::sim::ctw
