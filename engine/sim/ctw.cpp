// Conquer the World: the campaign state, setup (docs/ctw-spec.md 2.2), the round loop (2.3), armies and supply (2.4),
// tribute and income (2.5), ownership changes (transfer_to / defeat) and the available links (5.1).
#include "sim/ctw.hpp"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <stdexcept>

#include "sim/rules_xml.hpp"

namespace ron::sim::ctw {

namespace {

// goods type index by name (Kind::Good, 0..49), -1.
int good_index(const sim::Rules& r, const std::string& name) {
	const std::string t = lower(name);
	for (int i = 0; i < kEndGoods; i++) {
		if (lower(to_utf8(r.types[size_t(i)].name)) == t) return i;
	}
	return -1;
}

// building type index by name (wonders), -1.
int wonder_index(const sim::Rules& r, const std::string& name) {
	const std::string t = lower(name);
	for (int i = kBuilds; i < kEndBuilds; i++) {
		if (lower(to_utf8(r.types[size_t(i)].name)) == t) return i;
	}
	return -1;
}

int to_int(const std::string& s) { return std::atoi(s.c_str()); }

}  // namespace

// ---------------------------------------------------------------- small queries

const std::string& ConquestGame::tribe_name(int slot) const {
	static const std::string none = "Barbarians";
	if (!def || slot < 0 || slot >= kSlots) return none;
	return def->tribes[size_t(slot)].display_name;
}

std::string ConquestGame::node_name(int n) const {
	if (n < 0 || size_t(n) >= nodes.size()) return std::string();
	return nodes[size_t(n)].display_name.empty() ? def->countries[size_t(n)].name : nodes[size_t(n)].display_name;
}

int ConquestGame::eff(int owner) const {
	if (owner < 0 || owner >= kSlots) return owner;
	const int m = leaders[size_t(owner)].vassal_of;
	return m >= 0 ? m : owner;
}

int ConquestGame::find_node(const std::string& id) const { return def ? def->country(id) : -1; }

int ConquestGame::slot_of(const std::string& tribe) const {
	if (!def) return -1;
	const int k = def->tribe_by_key(tribe);
	if (k >= 0) return k;
	const std::string t = lower(tribe);
	for (int i = 0; i < kSlots; i++) {
		if (def->tribes[size_t(i)].present && lower(def->tribes[size_t(i)].display_name) == t) return i;
	}
	if (rules) {
		for (int i = 0; i < kSlots && i < int(rules->tribes.size()); i++) {
			if (lower(to_utf8(rules->tribes[size_t(i)].name)) == t) return i;
		}
	}
	return -1;
}

Piece* ConquestGame::army_at(int node) {
	for (auto& l : pieces) {
		for (Piece& p : l) {
			if (p.node == node) return &p;
		}
	}
	return nullptr;
}
bool ConquestGame::has_army(int node) const {
	for (const auto& l : pieces) {
		for (const Piece& p : l) {
			if (p.node == node) return true;
		}
	}
	return false;
}

std::vector<int> ConquestGame::owned_nodes(int slot, bool with_vassals) const {
	std::vector<int> out;
	for (size_t i = 0; i < nodes.size(); i++) {
		const int o = nodes[i].owner;
		if (o == slot || (with_vassals && o >= 0 && leaders[size_t(o)].vassal_of == slot)) out.push_back(int(i));
	}
	return out;
}
int ConquestGame::num_nodes(int slot, bool with_vassals) const { return int(owned_nodes(slot, with_vassals).size()); }

std::vector<int> ConquestGame::active_slots() const {
	std::vector<int> out;
	for (int i = 0; i < kSlots; i++) {
		if (leaders[size_t(i)].valid && leaders[size_t(i)].active) out.push_back(i);
	}
	return out;
}

void ConquestGame::add_news(const std::string& text) {
	news.push_back({cur_round, text});
	if (news.size() > 200) news.erase(news.begin(), news.begin() + 100);
	if (log.size() < 5000) log.push_back("r" + std::to_string(cur_round) + ": " + text);
}

// ---------------------------------------------------------------- setup

std::string ConquestGame::attach(const GameFiles& f, const sim::Rules& r, const std::string& campaign) {
	files = &f;
	rules = &r;
	try {
		def = load_campaign(f, r, campaign);
		crules = load_conquest_rules(f);
	} catch (const std::exception& e) {
		return e.what();
	}
	return std::string();
}

std::string ConquestGame::setup(const GameFiles& f, const sim::Rules& r, const ConquestSetup& S) {
	std::string err = attach(f, r, S.campaign);
	if (!err.empty()) return err;
	const CampaignDef& C = *def;
	for (const std::string& p : validate_campaign(C)) return p;

	// the saved scalars back to their defaults
	player_tribe = -1;
	difficulty = std::clamp(S.difficulty, 0, kMaxDifficulty);
	seed = S.seed ? S.seed : 1u;
	random.reseed(seed);
	cur_round = C.starting_round;
	num_turns = 0;
	turns_skipped = 0;
	cur_age = starting_age = C.starting_age;
	ending_age = C.ending_age;
	scen_per_age = C.turns_per_age;
	game_end = 0;
	return_to_node = -1;
	defcon_level = 0;
	nuke_launch_turns = 0;
	talleyrand_tribe = fouche_tribe = -1;
	talleyrand_state = 0;
	style_index1 = style_index2 = -1;
	cur_espionage_level = 0;
	has_fought_barbarian = free_move_turn = need_defensive_bonus = piece_moves_disabled = attacked_this_round = false;
	ai_phase = 0;
	random_nations = S.random_nations && C.allow_random_nations;
	random_bonuses = S.random_bonuses && C.allow_random_bonuses;
	map_name = C.name;
	map_id = C.file;
	leaders = {};
	nodes.assign(C.countries.size(), Node());
	colonies.clear();
	for (auto& p : pieces) p.clear();
	deck = C.deck;
	pool.clear();
	news.clear();
	valid_tribes.clear();
	overrun_armies.clear();
	continents_captured.assign(C.continents.size(), 0);
	reinforcements.clear();
	stored_ints.clear();
	stored_strs.clear();
	sub_goals.clear();
	allied_punks.clear();
	deals.clear();
	available_links.clear();
	script_links.clear();
	script_no_attack.clear();
	log.clear();
	style_table = C.styles.empty() ? crules->styles : C.styles;
	for (StyleDef& s : style_table) s.times_played = 0;
	scripts_drop();

	// 2. nodes: one per COUNTRY in file order; Eureka tech category
	for (size_t i = 0; i < nodes.size(); i++) nodes[i].tech_cat = (int(i) + rget(0, 5000)) & 3;

	// card pool (the deck shuffle happens when the first card is drawn, as make_card_list does)
	pool = make_card_list();

	// owners (random_nations: a shuffled permutation of the owning slots)
	std::vector<int> owner_of(nodes.size(), -1);
	std::vector<int> owner_slots;
	for (size_t i = 0; i < nodes.size(); i++) {
		const std::string& o = C.countries[i].owner;
		if (o.empty()) continue;
		owner_of[i] = C.tribe_by_key(o);
		if (owner_of[i] >= 0 && std::find(owner_slots.begin(), owner_slots.end(), owner_of[i]) == owner_slots.end())
			owner_slots.push_back(owner_of[i]);
	}
	std::map<int, int> remap;
	if (random_nations && owner_slots.size() > 1) {
		std::vector<std::pair<int, int>> keyed;  // ordered_insert keyed by rand(0..1000)
		for (int s : owner_slots) keyed.push_back({rget(0, 1000), s});
		std::stable_sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first < b.first; });
		for (size_t k = 0; k < owner_slots.size(); k++) remap[owner_slots[k]] = keyed[k].second;
	}

	for (size_t i = 0; i < nodes.size(); i++) {
		const CountryDef& c = C.countries[i];
		Node& n = nodes[i];
		n.continent = -1;
		for (size_t k = 0; k < C.continents.size(); k++) {
			if (ieq(C.continents[k].id, c.continent)) n.continent = int(k);
		}
		n.tileset = lower(c.tileset);
		n.value = std::max(0, c.value);
		n.nukes = c.nukes;
		if (c.supply_depot) n.flags |= kSupplyDepot;
		if (c.priority_target) n.flags |= kPriority;
		if (c.capital) n.flags |= kCapital;
		if (c.tileset == "random") n.tileset = "dirty";
		// tribute: random -> (rand(0..5)+1) x 10
		if (lower(c.tribute) == "random" || (random_bonuses && !c.tribute.empty())) n.tribute = (rget(0, 5) + 1) * 10;
		else n.tribute = to_int(c.tribute);
		// rare resource
		if (lower(c.rare) == "random" || (random_bonuses && !c.rare.empty())) {
			int g = rget(6, 49);
			while (g == 20 || (random_bonuses && (g == 6 || g == 31))) g = rget(6, 49);
			n.rare = g;
		} else if (!c.rare.empty()) {
			n.rare = good_index(r, c.rare);
		}
		// wonder
		if (lower(c.wonder) == "random") n.wonder = rget(0x20e, 0x21e);
		else if (!c.wonder.empty()) n.wonder = wonder_index(r, c.wonder);
		// bonus card
		if (lower(c.card) == "random" || (random_bonuses && !c.card.empty())) n.card = draw_card();
		else if (!c.card.empty()) parse_card(c.card, &n.card);
		// owner
		int o = owner_of[i];
		if (o >= 0 && remap.count(o)) o = remap[o];
		n.owner = n.founder = o;
		// links
		for (const LinkDef& l : c.links) {
			const int to = C.country(l.to);
			if (to < 0) continue;
			bool dup = false;
			for (int t : n.links) dup |= t == to;
			if (dup) continue;
			n.links.push_back(to);
			n.link_sea.push_back(l.sea ? 1 : 0);
		}
	}

	// colonies
	for (const ColonyDef& cd : C.colonies) {
		Colony c;
		c.id = cd.id;
		c.name = cd.name;
		c.value = cd.value;
		c.tribute = to_int(cd.tribute);
		for (const std::string& g : cd.rares) {
			const int gi = good_index(r, g);
			if (gi >= 0) c.rares.push_back(gi);
		}
		if (!cd.wonder.empty()) c.wonder = wonder_index(r, cd.wonder);
		if (!cd.owner.empty()) {
			int o = C.tribe_by_key(cd.owner);
			if (o >= 0 && remap.count(o)) o = remap[o];
			c.owner = o;
		}
		colonies.push_back(std::move(c));
	}

	// 4. leaders: every slot that owns a country or a colony is valid and active
	for (int s = 0; s < kSlots; s++) {
		Leader& L = leaders[size_t(s)];
		const TribeDef& td = C.tribes[size_t(s)];
		bool owns = false;
		for (const Node& n : nodes) owns |= n.owner == s;
		for (const Colony& c : colonies) owns |= c.owner == s;
		L.valid = L.active = owns;
		L.no_eliminating = td.no_eliminating;
		L.no_propose_ally = td.no_propose_ally;
		L.base_armies = td.base_armies;
		L.inherent_skill = td.skill > 0 ? td.skill : rget(1, 100);
		if (!owns) continue;
		for (int j = 0; j < kSlots; j++) {
			L.stances[size_t(j)] = kPeace;
			L.stance_time[size_t(j)] = 0;
			L.attitude_base[size_t(j)] = rget(0, 5) + K(r, "ctw_start_attitude") - 2;
		}
		L.stances[size_t(s)] = kAlly;
		L.tribute = K(r, "ctw_starting_tribute");
		L.met_mask = L.fought_mask = 0xffffff;
		if (!td.leaders.empty()) L.name = td.leaders[size_t(rget(0, int(td.leaders.size()) - 1))];
		else L.name = td.display_name;
		for (const std::string& cn : td.cards) {
			Card cd;
			if (parse_card(cn, &cd)) L.cards.push_back(cd);
		}
	}
	// capitals and start levels
	const int capital_level = std::clamp(K(r, "ctw_capital_starting_infra"), 0, 7);
	for (size_t i = 0; i < nodes.size(); i++) {
		Node& n = nodes[i];
		const CountryDef& c = C.countries[i];
		if (n.owner < 0) {
			n.level = std::clamp(c.size, 0, 7);
			continue;
		}
		Leader& L = leaders[size_t(n.owner)];
		if (c.capital) {
			if (L.capital < 0) L.capital = int(i);
			else if (L.capital2 < 0) L.capital2 = int(i);
			n.level = c.size > 0 ? std::clamp(c.size, 0, 7) : capital_level;
		} else {
			n.level = c.size > 0 ? std::clamp(c.size, 0, 7) : 0;
		}
	}
	// a nation without a flagged capital takes its first node
	for (int s = 0; s < kSlots; s++) {
		Leader& L = leaders[size_t(s)];
		if (!L.valid || L.capital >= 0) continue;
		for (size_t i = 0; i < nodes.size(); i++) {
			if (nodes[i].owner == s) {
				L.capital = int(i);
				nodes[i].flags |= kCapital;
				break;
			}
		}
	}
	for (int s = 0; s < kSlots; s++) {
		Leader& L = leaders[size_t(s)];
		L.name = L.valid ? L.name : std::string();
	}

	// armies: base_armies > 0 -> the first army on the capital; supply depots add one each
	for (int s = 0; s < kSlots; s++) {
		Leader& L = leaders[size_t(s)];
		if (!L.valid) continue;
		if (L.base_armies > 0 && L.capital >= 0) pieces[size_t(s)].push_back({L.capital, false});
	}

	// 5. the player's nation
	if (S.player >= 0 && S.player < kSlots && leaders[size_t(S.player)].valid) {
		player_tribe = S.player;
	} else {
		std::vector<int> cand;
		for (const std::string& v : C.valid_nations) {
			const int k = C.tribe_by_key(v);
			if (k >= 0 && leaders[size_t(k)].valid) cand.push_back(k);
		}
		if (cand.empty()) {
			for (int s = 0; s < kSlots; s++) {
				if (leaders[size_t(s)].valid && leaders[size_t(s)].base_armies > 0) cand.push_back(s);
			}
		}
		if (cand.empty()) return "the campaign has no nation the player can take";
		player_tribe = cand[size_t(rget(0, int(cand.size()) - 1))];
	}
	for (int s = 0; s < kSlots; s++) {
		if (leaders[size_t(s)].valid) {
			assign_supply(s);
			place_new_armies(s);
		}
	}
	// 6. immunity of the player, init script, bonuses
	turns_no_attack = crules->min_no_attack[size_t(difficulty)];
	assign_bonuses();
	run_init_script();
	assign_bonuses();
	begin_player_round();
	add_news("The campaign '" + C.name + "' begins. You lead the " + tribe_name(player_tribe) + ".");
	return std::string();
}

// ---------------------------------------------------------------- armies

void ConquestGame::assign_supply(int slot) {
	Leader& L = leaders[size_t(slot)];
	int depots = 0;
	for (const Node& n : nodes) {
		if (!(n.flags & kSupplyDepot) || n.owner < 0) continue;
		if (n.owner == slot || leaders[size_t(n.owner)].vassal_of == slot) depots++;
	}
	L.supply_depots = depots;
}

int ConquestGame::army_cap(int slot) const {
	const Leader& L = leaders[size_t(slot)];
	if (!L.valid || !L.active || L.vassal_of >= 0) return 0;
	int vassals = 0;
	for (int s = 0; s < kSlots; s++) {
		if (leaders[size_t(s)].valid && leaders[size_t(s)].active && leaders[size_t(s)].vassal_of == slot) vassals++;
	}
	return std::max(0, L.base_armies) + (L.base_armies > 0 && L.capital2 >= 0 ? 1 : 0) + vassals + L.supply_depots;
}

namespace {
// find_best_army_loc (AI): (?) the exact scoring is unknown; the spec says only that the AI picks one of its empty
// nodes. Rule here: the capital first, then supply depots, then the node with the most enemy neighbours (border).
int best_army_loc(const ConquestGame& g, int slot) {
	int best = -1, best_score = -1;
	for (int n : g.owned_nodes(slot, true)) {
		if (g.has_army(n)) continue;
		int score = 1;
		const Node& N = g.nodes[size_t(n)];
		if (N.flags & kCapital) score += 100;
		if (N.flags & kSupplyDepot) score += 40;
		for (int t : N.links) {
			const int o = g.node_eff_owner(t);
			if (o != slot && (o < 0 || g.stance(slot, o) == kWar)) score += 8;
		}
		score += N.level;
		if (score > best_score) {
			best_score = score;
			best = n;
		}
	}
	return best;
}
}  // namespace

std::string ConquestGame::place_new_armies(int slot) {
	std::string text;
	std::vector<Piece>& list = pieces[size_t(slot)];
	const int cap = army_cap(slot);
	// vassal armies vanish (cap 0); owner-less nodes never keep an army
	list.erase(std::remove_if(list.begin(), list.end(), [&](const Piece& p) { return nodes[size_t(p.node)].owner < 0; }), list.end());
	while (int(list.size()) > cap) {
		text = "An army of the " + tribe_name(slot) + " disbands (over the supply limit).";
		list.erase(list.begin());
	}
	while (int(list.size()) < cap) {
		const int at = best_army_loc(*this, slot);
		if (at < 0) break;
		list.push_back({at, false});
		text = "A new army of the " + tribe_name(slot) + " musters at " + node_name(at) + ".";
		if (slot == player_tribe) add_news(text);
	}
	return text;
}

void ConquestGame::clear_moved() {
	for (auto& l : pieces) {
		for (Piece& p : l) p.moved = false;
	}
}

int ConquestGame::can_drop_army(int from, int to, int slot) const {
	if (from < 0 || to < 0 || size_t(from) >= nodes.size() || size_t(to) >= nodes.size()) return -1;
	const bool free = def->free_move || free_move_turn;
	const Node& F = nodes[size_t(from)];
	bool adjacent = free;
	for (int t : F.links) adjacent |= t == to;
	if (!adjacent) return -1;
	if (has_army(to)) return -3;
	const int o = node_eff_owner(to);
	if (o != slot) {
		if (!(def->ally_move && o >= 0 && stance(slot, o) >= kAlly)) return -7;
	}
	return 0;
}

std::vector<int> ConquestGame::move_targets(int node) {
	std::vector<int> out;
	const Piece* p = army_at(node);
	if (!p || p->moved) return out;
	int slot = -1;
	for (int s = 0; s < kSlots; s++) {
		for (const Piece& q : pieces[size_t(s)]) {
			if (&q == p) slot = s;
		}
	}
	if (slot < 0) return out;
	if (free_move_turn || def->free_move) {
		for (int o : owned_nodes(slot, true)) {
			if (o != node && can_drop_army(node, o, slot) == 0) out.push_back(o);
		}
		return out;
	}
	for (int t : nodes[size_t(node)].links) {
		if (can_drop_army(node, t, slot) == 0) out.push_back(t);
	}
	return out;
}

bool ConquestGame::move_army(int from, int to, std::string* why) {
	Piece* p = army_at(from);
	if (!p) {
		if (why) *why = "There is no army there.";
		return false;
	}
	int slot = -1;
	for (int s = 0; s < kSlots; s++) {
		for (const Piece& q : pieces[size_t(s)]) {
			if (&q == p) slot = s;
		}
	}
	if (p->moved && !(free_move_turn)) {
		if (why) *why = "That army has already moved this round.";
		return false;
	}
	const int code = can_drop_army(from, to, slot);
	if (code != 0) {
		if (why) {
			*why = code == -1 ? "That territory is not adjacent." : code == -3 ? "Another army is already there." :
			       "You can only move into your own territory.";
		}
		return false;
	}
	p->node = to;
	p->moved = true;
	return true;
}

// ---------------------------------------------------------------- links (5.1)

std::vector<LinkChoice> ConquestGame::find_available_links(int slot) {
	std::vector<LinkChoice> out;
	if (slot < 0 || !leaders[size_t(slot)].active) return out;
	const Leader& L = leaders[size_t(slot)];
	script_links.clear();
	const bool only_script = run_links_script(slot);
	auto no_attack = [&](int n) { return std::find(L.no_attack.begin(), L.no_attack.end(), n) != L.no_attack.end(); };
	auto add = [&](int from, int to, bool sea) {
		const int o = node_eff_owner(to);
		if (o == eff(slot)) return;
		const Node& T = nodes[size_t(to)];
		if (T.flags & kFortified) return;
		if (no_attack(to)) return;
		if (o >= 0 && (T.flags & kCapital) && leaders[size_t(o)].no_eliminating && o != player_tribe) return;
		if (o >= 0 && stance(slot, o) >= kAlly) return;
		if (o >= 0 && L.no_attack_time[size_t(o)] > 0) return;
		LinkChoice lc;
		lc.from = from;
		lc.to = to;
		lc.attacker = slot;
		lc.defender = o;
		lc.sea = sea;
		if (o >= 0 && stance(slot, o) != kWar) {
			lc.needs_war = true;
			lc.declare_war_cost = war_cost(slot, o);
		}
		// supporters: neighbours of the defended node owned by an ally of one side with an available army (3 per side)
		for (int n2 : T.links) {
			if (n2 == from || !has_army(n2)) continue;
			const int o2 = node_eff_owner(n2);
			if (o2 < 0 || o2 == slot || o2 == o) continue;
			if (stance(slot, o2) >= kAlly && lc.att_supporters.size() < 3) lc.att_supporters.push_back(o2);
			else if (o >= 0 && stance(o, o2) >= kAlly && lc.def_supporters.size() < 3) lc.def_supporters.push_back(o2);
		}
		out.push_back(std::move(lc));
	};
	if (only_script) {
		for (const auto& pr : script_links) {
			const Node& F = nodes[size_t(pr.first)];
			bool sea = false;
			for (size_t k = 0; k < F.links.size(); k++) {
				if (F.links[k] == pr.second) sea = F.link_sea[k] != 0;
			}
			if (const Piece* p = const_cast<ConquestGame*>(this)->army_at(pr.first)) {
				(void)p;
				add(pr.first, pr.second, sea);
			}
		}
		return out;
	}
	std::vector<int> army_nodes;
	for (const Piece& p : pieces[size_t(slot)]) army_nodes.push_back(p.node);
	if (def->ally_move) {
		for (int s = 0; s < kSlots; s++) {
			if (s != slot && leaders[size_t(s)].active && stance(slot, s) >= kAlly) {
				for (const Piece& p : pieces[size_t(s)]) army_nodes.push_back(p.node);
			}
		}
	}
	for (int from : army_nodes) {
		const Node& F = nodes[size_t(from)];
		for (size_t k = 0; k < F.links.size(); k++) add(from, F.links[k], F.link_sea[k] != 0);
	}
	for (const auto& pr : script_links) {  // ctw_add_link additions in the "false" mode
		if (!has_army(pr.first)) continue;
		bool dup = false;
		for (const LinkChoice& l : out) dup |= l.from == pr.first && l.to == pr.second;
		if (!dup) add(pr.first, pr.second, false);
	}
	return out;
}

// ---------------------------------------------------------------- ownership

void ConquestGame::set_node_level(int n, int level) { nodes[size_t(n)].level = std::clamp(level, 0, 7); }

int ConquestGame::count_continent_nodes(int cont, int slot) const {
	int c = 0;
	for (const Node& n : nodes) {
		if (n.continent == cont && node_eff_owner(int(&n - nodes.data())) == slot) c++;
	}
	return c;
}

void ConquestGame::check_continents(int slot) {
	if (slot < 0) return;
	Leader& L = leaders[size_t(slot)];
	for (size_t c = 0; c < def->continents.size(); c++) {
		int total = 0, mine = 0;
		for (size_t i = 0; i < nodes.size(); i++) {
			if (nodes[i].continent != int(c)) continue;
			total++;
			if (node_eff_owner(int(i)) == slot) mine++;
		}
		const bool has = std::find(L.continents.begin(), L.continents.end(), int(c)) != L.continents.end();
		if (total > 0 && mine == total && !has) {
			L.continents.push_back(int(c));
			if (slot != player_tribe) {
				give_tribute(slot, K(*rules, "ctw_continent_bonus") * total);  // AI bonus: 10 per node
			} else {
				give_tribute(slot, K(*rules, "ctw_continent_bonus") * total);
				add_news("You control all of " + def->continents[c].name + ": " + std::to_string(K(*rules, "ctw_continent_bonus") * total) +
				         " tribute.");
			}
			continents_captured[c] = slot + 1;
		} else if (has && mine < total) {
			L.continents.erase(std::find(L.continents.begin(), L.continents.end(), int(c)));
		}
	}
}

void ConquestGame::transfer_to(int node, int new_owner, bool collect, std::string* text) {
	Node& N = nodes[size_t(node)];
	const int old = N.owner;
	if (old == new_owner) return;
	// the army of the old owner on the node is lost
	for (auto& list : pieces) {
		list.erase(std::remove_if(list.begin(), list.end(), [&](const Piece& p) { return p.node == node; }), list.end());
	}
	bool eliminates = false;
	if (old >= 0) {
		Leader& O = leaders[size_t(old)];
		if (N.flags & kCapital) {
			if (O.capital == node && O.capital2 >= 0) {
				O.capital = O.capital2;
				O.capital2 = -1;
			} else if (O.capital2 == node) {
				O.capital2 = -1;
			} else if (O.capital == node) {
				eliminates = true;
			}
		}
		O.nodes_won_from[size_t(new_owner >= 0 ? new_owner : 0)] += 0;
		O.new_nodes.erase(std::remove(O.new_nodes.begin(), O.new_nodes.end(), node), O.new_nodes.end());
	}
	N.owner = new_owner;
	N.turns_held = 0;
	if (new_owner >= 0) {
		Leader& W = leaders[size_t(new_owner)];
		W.new_nodes.push_back(node);
		if (old >= 0) W.nodes_won_from[size_t(old)]++;
		if (collect && N.take_tribute) {
			give_tribute(new_owner, N.tribute);
			N.take_tribute = false;
			if (text) *text += " +" + std::to_string(N.tribute) + " tribute.";
		}
		if (N.card.valid() && collect) {
			give_card(new_owner, N.card);
			if (text) *text += " Card: " + card_name(N.card) + ".";
			N.card = Card();
		}
		if (N.flags & kSupplyDepot) assign_supply(new_owner);
	}
	if (old >= 0 && (N.flags & kSupplyDepot)) assign_supply(old);
	if (eliminates && old >= 0) defeat(old, new_owner);
	check_continents(new_owner);
	if (old >= 0) check_continents(old);
}

void ConquestGame::defeat(int slot, int by) {
	Leader& L = leaders[size_t(slot)];
	if (!L.active) return;
	L.active = false;
	L.eliminated_by = by;
	// vassals are freed
	for (int s = 0; s < kSlots; s++) {
		if (leaders[size_t(s)].vassal_of == slot) {
			leaders[size_t(s)].vassal_of = -1;
			set_stance_pair(slot, s, kPeace);
		}
	}
	L.vassal_of = -1;
	if (by >= 0) {
		give_tribute(by, L.tribute);
		for (size_t i = 0; i < nodes.size(); i++) {
			if (nodes[i].owner == slot) {
				nodes[i].owner = by;
				nodes[i].turns_held = 0;
				leaders[size_t(by)].new_nodes.push_back(int(i));
			}
		}
		for (Colony& c : colonies) {
			if (c.owner == slot) c.owner = by;
		}
	} else {
		for (Node& n : nodes) {
			if (n.owner == slot) n.owner = -1;
		}
		for (Colony& c : colonies) {
			if (c.owner == slot) c.owner = -1;
		}
	}
	L.tribute = 0;
	pieces[size_t(slot)].clear();
	for (int s = 0; s < kSlots; s++) {
		leaders[size_t(s)].stances[size_t(slot)] = kPeace;
	}
	add_news("The " + tribe_name(slot) + " have been eliminated" + (by >= 0 ? " by the " + tribe_name(by) + "." : "."));
	if (slot == player_tribe) game_end = kPlayerEliminated;
	if (by >= 0) {
		assign_supply(by);
		check_continents(by);
	}
}

void ConquestGame::check_allied_victory() {
	if (game_end != 0) return;
	const Leader& P = leaders[size_t(player_tribe)];
	if (!P.active) return;
	bool all_with_armies = true, all_active = true;
	for (int s = 0; s < kSlots; s++) {
		if (s == player_tribe) continue;
		const Leader& L = leaders[size_t(s)];
		if (!L.valid || !L.active) continue;
		const bool friendly = stance(player_tribe, s) >= kAlly || L.vassal_of == player_tribe;
		if (!friendly) {
			all_active = false;
			if (!pieces[size_t(s)].empty()) all_with_armies = false;
		}
	}
	if (all_with_armies) game_end = all_active ? kVictoryAllied : kVictorySome;
}

// ---------------------------------------------------------------- income / bonuses

void ConquestGame::check_income(int slot) {
	Leader& L = leaders[size_t(slot)];
	if (!L.active) return;
	int add = 0;
	if (L.colony_blockade > 0) {
		L.colony_blockade--;
	} else {
		for (const Colony& c : colonies) {
			if (c.owner == slot) add += c.tribute;
		}
	}
	if (def->territory_income) {
		for (const Node& n : nodes) {
			if (n.owner == slot) add += n.level * K(*rules, "ctw_terr_income_x") + K(*rules, "ctw_terr_income_base");
		}
	}
	if (add) give_tribute(slot, add);
}

void ConquestGame::assign_bonuses() {
	for (int s = 0; s < kSlots; s++) {
		Leader& L = leaders[size_t(s)];
		L.nodes_controlled = 0;
		L.strength = 0;
		L.base_rate.fill(0);
		L.rare_resources.clear();
		L.wonders.clear();
		assign_supply(s);
		if (!L.valid || !L.active) continue;
		auto add_rare = [&](int g) {
			if (g < 0 || std::find(L.rare_resources.begin(), L.rare_resources.end(), g) != L.rare_resources.end()) return;
			L.rare_resources.push_back(g);
		};
		for (size_t i = 0; i < nodes.size(); i++) {
			const Node& n = nodes[i];
			if (n.owner < 0 || (n.owner != s && leaders[size_t(n.owner)].vassal_of != s)) continue;
			L.nodes_controlled++;
			L.strength += n.level;
			add_rare(n.rare);
			if (n.wonder >= 0) L.wonders.push_back(n.wonder);
		}
		if (L.colony_blockade <= 0) {
			for (const Colony& c : colonies) {
				if (c.owner != s) continue;
				for (int g : c.rares) add_rare(g);
				if (c.wonder >= 0) L.wonders.push_back(c.wonder);
			}
		}
		if (L.colony_blockade > 0) continue;  // colony_blockade also suppresses the rare bonuses
		for (int g : L.rare_resources) {
			const Type& T = rules->types[size_t(g)];
			for (int k = 0; k < 2; k++) {
				if (T.bonus_type[size_t(k)] >= 0 && T.bonus_type[size_t(k)] < 6) L.base_rate[size_t(T.bonus_type[size_t(k)])] += T.bonus_num[size_t(k)];
			}
		}
	}
}

// ---------------------------------------------------------------- the round

void ConquestGame::begin_player_round() {
	attacked_this_round = false;
	clear_moved();
	free_move_turn = false;
	available_links = find_available_links(player_tribe);
}

void ConquestGame::end_turn_player() {
	Leader& P = leaders[size_t(player_tribe)];
	if (!attacked_this_round) {
		int bonus = std::min((turns_skipped + 1) * K(*rules, "ctw_no_attack_bonus"), K(*rules, "ctw_no_attack_bonus_cap"));
		if (P.bonus_card_types_played[size_t(kAntipater)] > 0) bonus *= 2;
		P.tribute += bonus;
		turns_skipped++;
		add_news("You held your armies back: +" + std::to_string(bonus) + " tribute.");
	} else {
		turns_skipped = 0;
	}
	check_income(player_tribe);
}

void ConquestGame::begin_ai_round() {
	std::vector<std::pair<int, int>> keyed;
	for (int s = 0; s < kSlots; s++) {
		const Leader& L = leaders[size_t(s)];
		if (s == player_tribe || !L.valid || !L.active || L.vassal_of >= 0) continue;
		keyed.push_back({rget(0, 0x7fff), s});
	}
	std::stable_sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first < b.first; });
	valid_tribes.clear();
	for (auto& k : keyed) valid_tribes.push_back(k.second);
	ai_index = 0;
	ai_phase = 0;
	ai_in_progress = true;
}

AiStep ConquestGame::ai_step() {
	AiStep step;
	while (ai_index < int(valid_tribes.size())) {
		const int slot = valid_tribes[size_t(ai_index)];
		Leader& L = leaders[size_t(slot)];
		if (!L.active || game_end == kPlayerEliminated) {
			ai_index++;
			ai_phase = 0;
			continue;
		}
		if (ai_phase == 0) {
			play_ai_cards(slot);
			do_ai_diplomacy(slot);
			{
				DiploOffer offer;  // the campaign's story offers (diplo_offer hook)
				if (run_diplo_offer(slot, &offer)) proposals.push_back(offer);
			}
			ai_phase = 1;
			if (run_strategy_script(slot)) {  // the campaign's script plays this tribe's turn (at most one scripted attack)
				ai_phase = 2;
				LinkChoice atk = pending_script_attack;
				pending_script_attack = LinkChoice();
				bool legal = atk.from >= 0 && atk.attacker == slot && nodes[size_t(atk.to)].owner != slot;
				if (legal) {
					legal = false;
					for (Piece& p : pieces[size_t(slot)]) {
						if (p.node == atk.from) legal = true;
					}
				}
				if (legal) {
					atk.defender = node_eff_owner(atk.to);
					if (atk.defender == slot) legal = false;
				}
				if (legal) {
					if (atk.defender >= 0 && atk.defender == player_tribe) {
						step.kind = AiStep::kPlayerAttacked;
						step.battle = atk;
						cur_link = atk;
						return step;
					}
					resolve_ai_vs_ai(atk);
				}
			}
		}
		if (ai_phase == 1) {
			// plan_strategy until it finds no attack
			for (int guard = 0; guard < 32; guard++) {
				LinkChoice atk;
				if (!plan_strategy(slot, &atk)) break;
				if (atk.defender >= 0 && atk.defender == player_tribe) {
					step.kind = AiStep::kPlayerAttacked;
					step.battle = atk;
					cur_link = atk;
					return step;
				}
				resolve_ai_vs_ai(atk);
			}
		}
		check_income(slot);
		ai_index++;
		ai_phase = 0;
	}
	ai_in_progress = false;
	step.kind = AiStep::kDone;
	return step;
}

void ConquestGame::finish_round() {
	assign_bonuses();
	adjust_attitudes();
	advance_round();
	for (int s = 0; s < kSlots; s++) {
		if (leaders[size_t(s)].active) place_new_armies(s);
	}
	run_post_ai();
	run_post_turn();
	check_allied_victory();
	if (game_end == 0 && cur_round >= length()) {
		if (def->allow_continue) add_news("The campaign's time is over; play on if you wish.");
		else game_end = kTimeOver;
	}
	begin_player_round();
}

void ConquestGame::advance_round() {
	cur_round++;
	num_turns++;
	if (scen_per_age > 0 && cur_round % scen_per_age == 0 && cur_age < ending_age) {
		cur_age++;
		for (size_t i = 0; i < nodes.size(); i++) {
			Node& n = nodes[i];
			if (n.owner < 0 || n.level >= ending_age) continue;
			const auto& nn = leaders[size_t(n.owner)].new_nodes;
			if (std::find(nn.begin(), nn.end(), int(i)) != nn.end()) continue;
			if (n.level < 7) n.level++;
		}
		add_news("A new age dawns.");
	}
	for (size_t i = 0; i < nodes.size(); i++) {
		Node& n = nodes[i];
		if (n.owner < 0) continue;
		const auto& nn = leaders[size_t(n.owner)].new_nodes;
		if (std::find(nn.begin(), nn.end(), int(i)) == nn.end()) n.turns_held++;
	}
	// Talleyrand's one-turn stance is reverted
	if (talleyrand_state != 0 && talleyrand_tribe >= 0) {
		set_stance_pair(player_tribe, talleyrand_tribe, talleyrand_state == 1 ? kPeace : kPeace);
		talleyrand_state = 0;
		talleyrand_tribe = -1;
	}
	for (int a = 0; a < kSlots; a++) {
		Leader& L = leaders[size_t(a)];
		if (!L.valid) continue;
		for (int b = 0; b < kSlots; b++) {
			if (L.stances[size_t(b)] != kWar) L.stance_time[size_t(b)]++;
			if (L.no_attack_time[size_t(b)] > 0) {
				L.no_attack_time[size_t(b)]--;
				if (L.no_attack_time[size_t(b)] == 0) L.bribe_paid[size_t(b)] = 0;
			}
		}
		L.new_nodes.clear();
	}
	if (cur_age >= 6) {
		for (Node& n : nodes) n.flags |= kVisible;
	}
	if (turns_no_attack > 0) turns_no_attack--;
	if (nuke_launch_turns > 0) nuke_launch_turns--;
	free_move_turn = false;
}

}  // namespace ron::sim::ctw
