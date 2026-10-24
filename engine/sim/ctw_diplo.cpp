// Conquer the World: diplomacy between turns (docs/ctw-spec.md section 3): the stance matrix, attitudes, the cost
// functions, buying war / peace / alliance / vassals / territory, tribute demands and bribes, the AI's diplomacy.
#include <algorithm>

#include "sim/ctw.hpp"

namespace ron::sim::ctw {

void ConquestGame::set_stance_pair(int a, int b, int st) {
	if (a < 0 || b < 0 || a == b) return;
	leaders[size_t(a)].stances[size_t(b)] = st;
	leaders[size_t(b)].stances[size_t(a)] = st;
}

int ConquestGame::num_allies(int slot) const {
	int n = 0;
	for (int s = 0; s < kSlots; s++) {
		if (s != slot && leaders[size_t(s)].active && leaders[size_t(slot)].stances[size_t(s)] == kAlly) n++;
	}
	return n;
}

void ConquestGame::set_war(int a, int b) {
	// ConquestLeader::set_war @7a35d0: both nations' attitude becomes min(stance_time + attitude_base + 5, max) of the declarer's
	// view, the clocks restart; in a Cold War a war on a nation with armies (base_armies > 0) raises DEFCON by 2.
	if (a < 0 || b < 0 || a == b || stance(a, b) == kWar) return;
	const int mx = K(*rules, "ctw_max_attitude");
	Leader& A = leaders[size_t(a)];
	Leader& B = leaders[size_t(b)];
	const int v = std::min(A.stance_time[size_t(b)] + A.attitude_base[size_t(b)] + 5, mx);
	A.attitude_base[size_t(b)] = v;
	B.attitude_base[size_t(a)] = v;
	// a vassal / master pair is dissolved by a declaration
	if (A.vassal_of == b) A.vassal_of = -1;
	if (B.vassal_of == a) B.vassal_of = -1;
	set_stance_pair(a, b, kWar);
	A.stance_time[size_t(b)] = B.stance_time[size_t(a)] = 0;
	add_news("The " + tribe_name(a) + " declare war on the " + tribe_name(b) + ".");
	if (def->strategic_nukes && B.base_armies != 0) defcon_level = std::clamp(defcon_level + 2, 0, 5);
	if (a == player_tribe && talleyrand_tribe == b) {
		talleyrand_tribe = -1;
		talleyrand_state = -1;
	}
	assign_supply(a);
	assign_supply(b);
}

void ConquestGame::set_peace(int a, int b) {
	// ConquestLeader::set_peace @7a3110: the clock restarts unless the pair was allied; both attitudes become the declarer's - 2
	if (a < 0 || b < 0 || a == b) return;
	Leader& A = leaders[size_t(a)];
	Leader& B = leaders[size_t(b)];
	if (A.stances[size_t(b)] != kAlly) A.stance_time[size_t(b)] = 0;
	A.stances[size_t(b)] = kPeace;
	if (B.stances[size_t(a)] != kAlly) B.stance_time[size_t(a)] = 0;
	B.stances[size_t(a)] = kPeace;
	const int v = std::max(0, A.attitude_base[size_t(b)] - 2);
	A.attitude_base[size_t(b)] = v;
	B.attitude_base[size_t(a)] = v;
	if (a == player_tribe && talleyrand_tribe == b) {
		talleyrand_tribe = -1;
		talleyrand_state = -1;
	}
	add_news("The " + tribe_name(a) + " and the " + tribe_name(b) + " make peace.");
}

void ConquestGame::set_ally(int a, int b) {
	// ConquestLeader::set_ally @7a3210: the stance clock is not reset
	if (a < 0 || b < 0 || a == b) return;
	Leader& A = leaders[size_t(a)];
	Leader& B = leaders[size_t(b)];
	A.stances[size_t(b)] = kAlly;
	B.stances[size_t(a)] = kAlly;
	const int v = std::max(0, A.attitude_base[size_t(b)] - 2);
	A.attitude_base[size_t(b)] = v;
	B.attitude_base[size_t(a)] = v;
	if (a == player_tribe && talleyrand_tribe == b) {
		talleyrand_tribe = -1;
		talleyrand_state = -1;
	}
	add_news("The " + tribe_name(a) + " and the " + tribe_name(b) + " form an alliance.");
}

void ConquestGame::set_vassal(int master, int vassal) {
	if (master < 0 || vassal < 0 || master == vassal) return;
	Leader& V = leaders[size_t(vassal)];
	// the former vassals of the new vassal are released first
	for (int s = 0; s < kSlots; s++) {
		if (leaders[size_t(s)].vassal_of == vassal) {
			leaders[size_t(s)].vassal_of = -1;
			set_stance_pair(vassal, s, kPeace);
			assign_supply(vassal);
		}
	}
	V.vassal_of = master;
	set_stance_pair(master, vassal, kVassal);
	// the master's other allies become peace partners of the vassal
	for (int s = 0; s < kSlots; s++) {
		if (s != master && s != vassal && stance(master, s) == kAlly) set_stance_pair(vassal, s, kPeace);
	}
	pieces[size_t(vassal)].clear();  // the vassal's armies are dropped
	assign_supply(master);
	assign_supply(vassal);
	place_new_armies(master);
	place_new_armies(vassal);
	add_news("The " + tribe_name(vassal) + " become vassals of the " + tribe_name(master) + ".");
}

// ---------------------------------------------------------------- attitudes and costs

int ConquestGame::get_attitude(int a, int b) const {
	const Leader& A = leaders[size_t(a)];
	int v = A.attitude_base[size_t(b)];
	// +2 when both hold the same, largest, node count
	int best = 0;
	for (int s = 0; s < kSlots; s++) {
		if (leaders[size_t(s)].active) best = std::max(best, num_nodes(s, false));
	}
	if (num_nodes(a, false) == best && num_nodes(b, false) == best) v += 2;
	// contacts: nodes of a adjacent to nodes of b
	int contacts = 0;
	for (size_t i = 0; i < nodes.size(); i++) {
		if (nodes[i].owner != a) continue;
		for (int t : nodes[i].links) {
			if (nodes[size_t(t)].owner == b) {
				contacts++;
				break;
			}
		}
	}
	if (contacts > 1) v += contacts - 1;
	const int st = stance(a, b);
	const int t = A.stance_time[size_t(b)];
	if (st == kWar) v += t;
	else {
		v -= t;
		if (st == kAlly) v -= 2;
	}
	if (A.no_attack_time[size_t(b)] > 0) v -= 1;
	return std::clamp(v, 0, K(*rules, "ctw_max_attitude"));
}

const std::string& ConquestGame::attitude_name(int a, int b) const {
	const int base = leaders[size_t(a)].attitude_base[size_t(b)];
	for (size_t i = 0; i < crules->attitude_bands.size(); i++) {
		if (base < crules->attitude_bands[i].second) return crules->attitude_names[i];
	}
	return crules->attitude_names.back();
}

int ConquestGame::war_cost(int a, int b) const {
	// ConquestLeader::get_war_cost @79c150: the base cost grows with the time at this stance; in a Cold War the player pays the
	// nuclear price against the other superpower (less in the final age and per DEFCON level above 1); allies cost double.
	const Leader& A = leaders[size_t(a)];
	int c = K(*rules, "ctw_base_war_cost") + A.stance_time[size_t(b)] * K(*rules, "ctw_war_cost_stance_time_x");
	if (def->strategic_nukes) {
		const int other = player_tribe == kUsa ? kUssr : kUsa;
		if (player_tribe == a && b == other) {
			c = K(*rules, "ctw_nuclear_enemy_war_cost");
			if (cur_age == ending_age) c -= K(*rules, "ctw_nuclear_war_age_discount");
			c -= K(*rules, "ctw_defcon_war_discount") * std::max(0, defcon_level - 1);
		}
	}
	if (A.stances[size_t(b)] == kAlly) return K(*rules, "ctw_ally_war_cost_x") * c;
	return c;
}

int ConquestGame::peace_cost(int a, int b) const {
	return K(*rules, "ctw_base_peace_cost") + get_attitude(b, a) * K(*rules, "ctw_peace_cost_att_x");
}

int ConquestGame::alliance_cost(int a, int b) const {
	// ConquestLeader::get_ally_cost @79c220: the buyer's own number of allies raises the price
	return peace_cost(a, b) + K(*rules, "ctw_additional_ally_inc") * num_allies(a);
}

int ConquestGame::territory_cost(int buyer, int node) const {
	const int owner = node_eff_owner(node);
	const int att = owner >= 0 ? get_attitude(owner, buyer) : 0;
	return K(*rules, "ctw_base_territory_cost") + nodes[size_t(node)].level * K(*rules, "ctw_territory_cost_str_x") +
	       att * K(*rules, "ctw_territory_cost_att_x");
}

int ConquestGame::vassal_cost(int master, int vassal) const {
	(void)master;
	(void)vassal;
	return 100;
}

int ConquestGame::card_cost(int slot) const {
	const int c = K(*rules, "ctw_base_card_cost") + K(*rules, "ctw_card_cost_num_purchased_x") * leaders[size_t(slot)].num_cards_purchased;
	return std::min(c, K(*rules, "ctw_card_cost_ramp_cap"));
}

int ConquestGame::build_cost(int node) const {
	return K(*rules, "ctw_base_build_cost") + K(*rules, "ctw_build_cost_time_held_x") * nodes[size_t(node)].level;
}

int ConquestGame::nuke_cost(int slot) const {
	int owned = 0;
	for (const Node& n : nodes) {
		if (n.owner == slot) owned += n.nukes;
	}
	return K(*rules, "ctw_nuke_base") + K(*rules, "ctw_nuke_ramp") * owned;
}

// ---------------------------------------------------------------- buying

bool ConquestGame::diplo_accept(int offerer, int target, int type) {
	// propose_peace / propose_buy_vassal ask run_diplo_response_script, which answers "yes" when the campaign has no such
	// script or it cannot run (@79b830 returns 1): the buttons of the map buy peace, alliance and vassals for their price, so
	// the only refusal is a nation that cannot be talked to. The negotiation window's deals are decided by diplo_response.bhs.
	(void)offerer;
	(void)type;
	if (target < 0) return false;
	return !leaders[size_t(target)].no_negotiation;
}

bool ConquestGame::can_declare_war(int a, int b, std::string* why) const {
	auto no = [&](const char* m) {
		if (why) *why = m;
		return false;
	};
	if (a == b || b < 0 || !leaders[size_t(b)].active) return no("There is no such nation.");
	if (stance(a, b) == kWar) return no("You are already at war.");
	if (stance(a, b) == kVassal) return no("A vassal cannot be attacked.");
	const auto& nd = leaders[size_t(a)].no_declare_war;
	if (std::find(nd.begin(), nd.end(), b) != nd.end()) return no("You may not declare war on that nation now.");
	return true;
}

bool ConquestGame::buy_war(int a, int b, std::string* why) {
	if (!can_declare_war(a, b, why)) return false;
	const int cost = war_cost(a, b);
	if (leaders[size_t(a)].tribute < cost) {
		if (why) *why = "A declaration of war costs " + std::to_string(cost) + " tribute.";
		return false;
	}
	leaders[size_t(a)].tribute -= cost;
	set_war(a, b);
	if (a == player_tribe) available_links = find_available_links(a);
	return true;
}

bool ConquestGame::buy_peace(int a, int b, std::string* why) {
	if (stance(a, b) != kWar) {
		if (why) *why = "You are not at war.";
		return false;
	}
	const int cost = peace_cost(a, b);
	if (leaders[size_t(a)].tribute < cost) {
		if (why) *why = "Peace costs " + std::to_string(cost) + " tribute.";
		return false;
	}
	if (!diplo_accept(a, b, kPeace)) {
		if (why) *why = "The " + tribe_name(b) + " refuse.";
		return false;
	}
	leaders[size_t(a)].tribute -= cost;
	leaders[size_t(b)].tribute += cost;
	set_peace(a, b);
	if (a == player_tribe) available_links = find_available_links(a);
	return true;
}

bool ConquestGame::buy_ally(int a, int b, std::string* why) {
	if (stance(a, b) == kAlly || stance(a, b) == kVassal || stance(a, b) == kWar) {
		if (why) *why = stance(a, b) == kWar ? "Make peace first." : "That nation is already allied.";
		return false;
	}
	const int cost = alliance_cost(a, b);
	if (leaders[size_t(a)].tribute < cost) {
		if (why) *why = "An alliance costs " + std::to_string(cost) + " tribute.";
		return false;
	}
	if (!diplo_accept(a, b, kAlly)) {
		if (why) *why = "The " + tribe_name(b) + " refuse an alliance.";
		return false;
	}
	leaders[size_t(a)].tribute -= cost;
	leaders[size_t(b)].tribute += cost;
	set_ally(a, b);
	if (a == player_tribe) available_links = find_available_links(a);
	return true;
}

bool ConquestGame::buy_vassal(int master, int vassal, std::string* why) {
	if (!def->allow_vassals) {
		if (why) *why = "Vassals are not allowed in this campaign.";
		return false;
	}
	if (leaders[size_t(vassal)].vassal_of >= 0 || stance(master, vassal) == kVassal) {
		if (why) *why = "That nation is already a vassal.";
		return false;
	}
	const int cost = vassal_cost(master, vassal);
	if (leaders[size_t(master)].tribute < cost) {
		if (why) *why = "A vassal costs " + std::to_string(cost) + " tribute.";
		return false;
	}
	if (!diplo_accept(master, vassal, kVassal)) {
		if (why) *why = "The " + tribe_name(vassal) + " refuse.";
		return false;
	}
	leaders[size_t(master)].tribute -= cost;
	leaders[size_t(vassal)].tribute += cost;
	set_vassal(master, vassal);
	if (master == player_tribe) available_links = find_available_links(master);
	return true;
}

bool ConquestGame::buy_territory(int buyer, int node, std::string* why) {
	const int owner = node_eff_owner(node);
	auto no = [&](const std::string& m) {
		if (why) *why = m;
		return false;
	};
	if (owner < 0 || owner == buyer) return no("That territory cannot be bought.");
	if (nodes[size_t(node)].flags & kCapital) return no("A capital is not for sale.");
	if (num_nodes(owner, false) < K(*rules, "ctw_territory_min_emp_size")) return no("That nation is too small to sell territory.");
	if (get_attitude(owner, buyer) > K(*rules, "ctw_territory_max_att")) return no("That nation is too hostile to sell.");
	if (stance(buyer, owner) == kWar) return no("Make peace first.");
	const int cost = territory_cost(buyer, node);
	if (leaders[size_t(buyer)].tribute < cost) return no("That territory costs " + std::to_string(cost) + " tribute.");
	leaders[size_t(buyer)].tribute -= cost;
	leaders[size_t(owner)].tribute += cost;
	transfer_to(node, buyer, false, nullptr);
	add_news("The " + tribe_name(buyer) + " buy " + node_name(node) + " from the " + tribe_name(owner) + ".");
	if (buyer == player_tribe) available_links = find_available_links(buyer);
	return true;
}

bool ConquestGame::buy_fortification(int slot, int node, std::string* why) {
	// ConquestLeader::buy_build @7a0260 / can_buy_infra: one more level of territory strength for 20 + 10 x level, up to level 7
	if (node_eff_owner(node) != slot) {
		if (why) *why = "You can only build in your own territory.";
		return false;
	}
	if (nodes[size_t(node)].level > 6) {
		if (why) *why = "That territory is at the highest level.";
		return false;
	}
	const int cost = build_cost(node);
	if (leaders[size_t(slot)].tribute < cost) {
		if (why) *why = "Building up this territory costs " + std::to_string(cost) + " tribute.";
		return false;
	}
	leaders[size_t(slot)].tribute -= cost;
	nodes[size_t(node)].level++;
	add_news(node_name(node) + " grows stronger.");
	return true;
}

bool ConquestGame::demand_tribute(int from, int to, std::string* why) {
	Leader& P = leaders[size_t(to)];  // the payer
	if (def->strategic_nukes || !no_negotiate_text.empty()) {  // ConquestLeader::demand_tribute @7a2460: no demands in a nuclear standoff
		if (why) *why = "They will not talk.";
		return false;
	}
	if (P.tribute / 2 < 11) {
		if (why) *why = "They have nothing worth taking.";
		return false;
	}
	const int prob = difficulty > 2 ? K(*rules, "ctw_demand_trib_tough_prob") : K(*rules, "ctw_demand_trib_prob");
	if (difficulty > 2 && get_attitude(to, from) > K(*rules, "ctw_max_bribe_att")) {
		if (why) *why = "The " + tribe_name(to) + " refuse.";
		return false;
	}
	if (rget(0, 100) >= prob) {
		if (why) *why = "The " + tribe_name(to) + " refuse your demand.";
		return false;
	}
	const int take = P.tribute / 2;
	P.tribute -= take;
	leaders[size_t(from)].tribute += take;
	const int rounds = K(*rules, "ctw_trib_no_attack") + 1;
	P.no_attack_time[size_t(from)] = rounds;
	leaders[size_t(from)].no_attack_time[size_t(to)] = rounds;
	P.bribe_paid[size_t(from)] = take;
	add_news("The " + tribe_name(to) + " pay " + std::to_string(take) + " tribute to the " + tribe_name(from) + ".");
	return true;
}

bool ConquestGame::offer_bribe(int payer, int receiver, int amount, std::string* why) {
	Leader& P = leaders[size_t(payer)];
	Leader& R = leaders[size_t(receiver)];
	auto no = [&](const std::string& m) {
		if (why) *why = m;
		return false;
	};
	if (R.reject_bribe || R.no_negotiation || def->strategic_nukes) return no("They will not talk.");
	if (amount < K(*rules, "ctw_bribe_trib_min") || P.tribute < amount) return no("You cannot offer that much.");
	int prob = K(*rules, "ctw_bribe_acc_prob");
	if (difficulty >= 3) prob = get_attitude(receiver, payer) > 15 ? K(*rules, "ctw_bribe_tough_hate_acc_prob") : K(*rules, "ctw_bribe_tough_acc_prob");
	if (rget(0, 100) >= prob) return no("The " + tribe_name(receiver) + " refuse the bribe.");
	P.tribute -= amount;
	R.tribute += amount;
	const int rounds = K(*rules, "ctw_trib_no_attack") + 1;
	R.no_attack_time[size_t(payer)] = rounds;
	P.no_attack_time[size_t(receiver)] = rounds;
	R.bribe_paid[size_t(payer)] = amount;
	add_news("The " + tribe_name(receiver) + " accept a bribe of " + std::to_string(amount) + " and will not attack for " + std::to_string(rounds) + " rounds.");
	return true;
}

// ---------------------------------------------------------------- AI diplomacy (ConquestLeader::do_diplomacy @79e030)

bool ConquestGame::propose_peace(int me, int j) {
	// ConquestLeader::propose_peace @7a0c20 for an AI proposer: peace (from war) or an alliance (from peace), paid by the
	// proposer to the other nation. The player is offered the deal (proposals); another AI accepts, but a nation that
	// already has several allies refuses an alliance most of the time. Returns 0 when it could not be afforded.
	if (j < 0) return false;
	Leader& L = leaders[size_t(me)];
	const int st = stance(me, j);
	const int cost = st == kPeace ? alliance_cost(me, j) : peace_cost(me, j);
	if (cost > L.tribute) return false;
	if (j == player_tribe) {
		if (st == kPeace && num_allies(j) > 1 && rget(0, 2) == 0) return true;
		DiploOffer o;
		o.tribe = tribe_key(me);
		o.tribe2 = tribe_key(j);
		o.offer_type = st == kPeace ? "Ally" : "Peace";
		o.tribe_trib = cost;
		o.response = st == kPeace ? "We propose an alliance, and offer " + std::to_string(cost) + " tribute to seal it."
		                          : "We propose peace, and offer " + std::to_string(cost) + " tribute.";
		o.id = "ai_" + o.offer_type + "_" + o.tribe;
		proposals.push_back(o);
		return true;
	}
	if (num_allies(j) > 1) {
		if (rget(0, 100) > 0x20) return true;
	}
	L.tribute -= cost;
	leaders[size_t(j)].tribute += cost;
	if (st == kPeace) set_ally(me, j);
	else set_peace(me, j);
	return true;
}

void ConquestGame::do_ai_diplomacy(int slot) {
	Leader& L = leaders[size_t(slot)];
	// the other nations in a random order (a key per nation from the campaign's stream), split by stance
	struct Keyed {
		int key, slot;
	};
	std::vector<Keyed> others;
	for (int s = 0; s < kSlots; s++) {
		if (s != slot && leaders[size_t(s)].active && leaders[size_t(s)].valid) others.push_back({rget(0, 100), s});
	}
	auto ordered = [](std::vector<Keyed>& v) {  // LinkListBase::ordered_insert: ascending, a new node before equal keys
		std::stable_sort(v.begin(), v.end(), [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
		for (size_t i = 0; i < v.size();) {
			size_t j = i;
			while (j < v.size() && v[j].key == v[i].key) j++;
			std::reverse(v.begin() + long(i), v.begin() + long(j));
			i = j;
		}
	};
	ordered(others);
	std::vector<Keyed> war, peace, ally;  // keyed by attitude
	bool war_neighbour = false;
	for (const Keyed& k : others) {
		const int st = L.stances[size_t(k.slot)];
		const int att = get_attitude(slot, k.slot);
		if (st == kWar) {
			war.push_back({att, k.slot});
			if (num_adjacent(slot, k.slot) != 0) war_neighbour = true;
		} else if (st == kPeace) {
			peace.push_back({att, k.slot});
		} else if (st == kAlly) {
			ally.push_back({att, k.slot});
		}
	}
	ordered(war);
	ordered(peace);
	ordered(ally);
	const int n_others = int(others.size());
	// ---- no barbarian neighbour and no war neighbour: declare war on the peaceful nation that borders us most
	if (num_adjacent(slot, -1) == 0 && (war.empty() || !war_neighbour)) {
		int best = -1, best_adj = 0;
		for (size_t k = peace.size(); k-- > 0;) {  // from the most hostile
			const int j = peace[k].slot;
			const int adj = num_adjacent(slot, j);
			if (best_adj >= adj) continue;
			if (war_cost(slot, j) > L.tribute || L.no_attack_time[size_t(j)] != 0) continue;
			if (!(rget(0, 3) + 2 <= L.stance_time[size_t(j)] || cur_round < 2)) continue;
			if (j == player_tribe && !(turns_no_attack == 0 && tribe_must_attack < 0)) continue;
			if (slot == 7) {  // the Egyptians leave the holder of General Ptolemy alone
				bool ptolemy = false;
				for (const Card& c : leaders[size_t(j)].cards) ptolemy |= c.type == kPtolemy;
				if (ptolemy) continue;
			}
			if (std::find(L.no_declare_war.begin(), L.no_declare_war.end(), j) != L.no_declare_war.end()) continue;
			best = j;
			best_adj = adj;
		}
		if (best >= 0) {
			if (best == player_tribe) tribe_must_attack = slot;
			buy_war(slot, best);  // declare_war(me, best, 0): the declaration is paid
		}
	}
	// ---- peace with a nation that has an army next to our capital and is strong or has taken our land
	{
		const int cap = L.capital >= 0 ? L.capital : L.capital2;
		if (n_others > 1 && cap >= 0) {
			for (int m : nodes[size_t(cap)].links) {
				const int o = node_eff_owner(m);
				if (o < 0 || L.stances[size_t(o)] != kWar || !available_army_at(m)) continue;
				if (leaders[size_t(o)].nodes_won_from[size_t(slot)] == 0) {
					if (nodes_controlled(o, true) < nodes_controlled(slot, true) * 2) continue;
				}
				if (get_attitude(slot, o) < 18 && L.tribute > 9) propose_peace(slot, o);
			}
		}
	}
	if (difficulty < 2) return;
	// ---- tougher games: alliances
	if (ally.empty() && n_others > 1 && !L.no_propose_ally) {
		int best = -1, best_att = K(*rules, "ctw_max_attitude") + 1, best_nodes = 0;
		for (const Keyed& k : peace) {  // from the friendliest
			if (!share_neighbor(slot, k.slot)) continue;
			const int att = get_attitude(k.slot, slot);
			if (att < best_att || (att == best_att && nodes_controlled(k.slot, true) > best_nodes)) {
				best = k.slot;
				best_att = att;
				best_nodes = nodes_controlled(k.slot, true);
			}
		}
		if (best >= 0) propose_peace(slot, best);
	} else if (!ally.empty()) {
		// an alliance older than CTW_MIN_KEEP_ALLIANCE rounds breaks with probability attitude x CTW_BREAK_ALLIANCE_PROB_X_ATT
		for (const Keyed& k : ally) {
			const int j = k.slot;
			const int att = get_attitude(j, slot);
			if (K(*rules, "ctw_min_keep_alliance") < L.stance_time[size_t(j)] &&
			    rget(0, 100) < K(*rules, "ctw_break_alliance_prob_x_att") * att) {
				if (L.stances[size_t(j)] != kAlly) L.stance_time[size_t(j)] = 0;
				L.stances[size_t(j)] = kPeace;
				if (leaders[size_t(j)].stances[size_t(slot)] != kAlly) leaders[size_t(j)].stance_time[size_t(slot)] = 0;
				leaders[size_t(j)].stances[size_t(slot)] = kPeace;
				add_news("The " + tribe_name(slot) + " break their alliance with the " + tribe_name(j) + ".");
				if (j == player_tribe) messages.push_back("The " + tribe_name(slot) + " have broken their alliance with you.");
			}
		}
	}
}

void ConquestGame::adjust_attitudes() {
	// post_adjust_attitudes @78bd10 sets attitude_base to min(0, base + 1) for every pair in a leader's `fought` mask, but
	// assign_bonuses -> clear_round_data empties the masks just before it runs (return_from_scen @78f040): in the shipped order
	// it changes nothing, so nothing is done here.
}

}  // namespace ron::sim::ctw
