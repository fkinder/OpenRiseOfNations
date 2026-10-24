// Conquer the World: the AI's strategic turn (docs/ctw-spec.md 2.3, 6). Ported from the decompiles in re/decomp/ctw:
//   ConquestLeader::plan_strategy @79ea80       the army moves, the overruns, the choice of the attack
//   ConquestLeader::run_strategy_script @79c630 (ctw_script.cpp)
//   ConquestGame::check_overrun @791f90 / check_potential_overrun @78aba0
//   ConquestGame::resolve_ai_strategy @78b360, resolve_overrun @792260
//   ConquestNode::num_adjacent_war / num_adjacent_enemies / num_supporting_armies / has_available_armies
//   ConquestLeader::get_num_adjacent @79c040, share_neighbor @79bfa0, get_nodes_controlled @7a2cf0
// The AI's diplomacy (do_diplomacy @79e030, propose_peace @7a0c20) is in ctw_diplo.cpp.
//
// Differences from the original, all marked: popups and sounds are dropped; an attack on the player is returned to the host
// (kPlayerAttacked) instead of opening the invade window; `ConquestPieces::move_army` is a plain move; the tie order of the
// original's ordered lists (LinkListBase::ordered_insert: a new node goes before equal keys) is kept where it decides.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "sim/ctw.hpp"

namespace ron::sim::ctw {

namespace {

// ConquestNode::has_available_armies(node, 1): an owned node with an army that has not moved this round.
bool available_army(const ConquestGame& g, int node) {
	if (g.nodes[size_t(node)].owner < 0) return false;
	for (const auto& list : g.pieces) {
		for (const Piece& p : list) {
			if (p.node == node && !p.moved) return true;
		}
	}
	return false;
}

int node_army(const ConquestGame& g, int node) { return g.has_army(node) ? 1 : 0; }

// ConquestNode::num_adjacent_war: neighbours whose nation is at war with the node's own.
int num_adjacent_war(const ConquestGame& g, int node) {
	const int o1 = g.node_eff_owner(node);
	int n = 0;
	for (int m : g.nodes[size_t(node)].links) {
		const int o2 = g.node_eff_owner(m);
		if (o1 != o2 && o1 >= 0 && o2 >= 0 && g.leaders[size_t(o1)].stances[size_t(o2)] == kWar) n++;
	}
	return n;
}

// ConquestNode::num_adjacent_enemies: neighbours of another nation (barbarians included).
int num_adjacent_enemies(const ConquestGame& g, int node) {
	const int o1 = g.node_eff_owner(node);
	int n = 0;
	for (int m : g.nodes[size_t(node)].links) n += g.node_eff_owner(m) != o1;
	return n;
}

// ConquestNode::num_supporting_armies(this, other): neighbours of `node` (not `other`) of `other`'s nation with an available army.
int num_supporting(const ConquestGame& g, int node, int other) {
	const int o = g.node_eff_owner(other);
	int n = 0;
	for (int m : g.nodes[size_t(node)].links) {
		if (m != other && g.node_eff_owner(m) == o && available_army(g, m)) n++;
	}
	return n;
}

bool at_war_or_barbarian(const ConquestGame& g, int me, int o) { return o < 0 || g.stance(me, o) == kWar; }

}  // namespace

bool ConquestGame::available_army_at(int node) const { return available_army(*this, node); }

// ConquestLeader::get_num_adjacent(this, j): border links from this nation's nodes to nodes of `j` (-1: barbarians).
int ConquestGame::num_adjacent(int me, int j) const {
	int n = 0;
	for (size_t i = 0; i < nodes.size(); i++) {
		if (node_eff_owner(int(i)) != me) continue;
		for (int m : nodes[i].links) n += node_eff_owner(m) == j;
	}
	return n;
}

// ConquestLeader::share_neighbor: borders `j`, or has a common neighbouring nation.
bool ConquestGame::share_neighbor(int me, int j) const {
	if (num_adjacent(me, j) != 0) return true;
	for (int k = 0; k < kSlots; k++) {
		if (k == me || k == j) continue;
		if (num_adjacent(me, k) != 0 && num_adjacent(k, j) != 0) return true;
	}
	return false;
}

// ConquestLeader::get_nodes_controlled(this, with_vassals): 0 for a vassal.
int ConquestGame::nodes_controlled(int slot, bool with_vassals) const {
	if (leaders[size_t(slot)].vassal_of >= 0) return 0;
	int n = 0;
	for (size_t i = 0; i < nodes.size(); i++) {
		const int o = nodes[i].owner;
		n += (with_vassals ? eff(o) : o) == slot;
	}
	return n;
}

// ConquestGame::check_overrun(attacker_node, defended_node, forced_defender, *potential) @791f90: the attacker's side
// outnumbers the defender's available armies around the defended node by 2 or more.
bool ConquestGame::check_overrun_nodes(int attacker_node, int defended_node, int forced_defender, bool* potential) const {
	if (potential) *potential = false;
	if (nodes[size_t(defended_node)].flags & kCapital) return false;
	int att = 0, def_n = 0, counter = 0;
	if (available_army(*this, attacker_node)) att = node_army(*this, attacker_node);
	if (forced_defender < 0) def_n = node_army(*this, defended_node);
	const int a = node_eff_owner(attacker_node);
	int d = node_eff_owner(defended_node);
	if (forced_defender >= 0) d = forced_defender;
	for (int m : nodes[size_t(defended_node)].links) {
		if (m == attacker_node) continue;
		const int o = node_eff_owner(m);
		const bool avail = available_army(*this, m);
		if (o == a) {
			if (avail) att += node_army(*this, m);
			counter++;
		} else if (o == d && avail) {
			def_n += node_army(*this, m);
		} else if (o >= 0) {
			const int st_oa = a >= 0 ? leaders[size_t(o)].stances[size_t(a)] : -1;
			const int st_od = d >= 0 ? leaders[size_t(o)].stances[size_t(d)] : -1;
			if (st_oa == kAlly && (d < 0 || st_od != kAlly)) {
				if (avail) att += node_army(*this, m);
				counter++;
			} else if (d >= 0 && st_od == kAlly && a >= 0 && st_oa != kAlly && avail) {
				def_n += node_army(*this, m);
			}
		}
	}
	if (potential && (att - def_n > 1 || (node_army(*this, defended_node) != 0 && att == 2 && counter > 2))) *potential = true;
	return att - def_n > 1;
}

bool ConquestGame::check_overrun(const LinkChoice& l) const { return check_overrun_nodes(l.from, l.to, -1, nullptr); }

// ConquestGame::check_potential_overrun(me, target) @78aba0: some non-allied neighbour of the target could overrun it from `me`.
bool ConquestGame::check_potential_overrun(int me, int target) const {
	if (me == player_tribe && available_links.size() <= 1) return false;
	for (int n : nodes[size_t(target)].links) {
		const int o = node_eff_owner(n);
		if (o < 0 || o == me || stance(o, me) == kAlly) continue;
		bool potential = false;
		const bool r = check_overrun_nodes(n, target, me, &potential);
		if (r || potential) return true;
	}
	return false;
}

// ConquestGame::resolve_overrun @792260: the node falls at once, the army moves in (and may not move again this round).
void ConquestGame::resolve_overrun(const LinkChoice& l) {
	const int attacker = node_eff_owner(l.from);
	const int defender = node_eff_owner(l.to);
	if (attacker < 0) return;
	leaders[size_t(attacker)].num_overruns++;
	std::string extra;
	transfer_to(l.to, attacker, true, &extra);
	for (Piece& p : pieces[size_t(attacker)]) {
		if (p.node == l.from) {
			p.node = l.to;
			p.moved = true;
			break;
		}
	}
	overrun_armies.push_back({l.to, attacker});
	add_news("The " + tribe_name(attacker) + " overrun " + node_name(l.to) + (defender >= 0 ? " of the " + tribe_name(defender) : std::string()) + "." + extra);
	assign_bonuses();
	place_new_armies(attacker);
	if (defender >= 0 && leaders[size_t(defender)].active) place_new_armies(defender);
}

// ConquestGame::resolve_ai_strategy @78b360 for an attack that does not involve the player: both sides' tactical cards are
// spent, the attacker wins with the AI-vs-AI chances (barbarians CTW_AI_BARB_CHANCE, a client state 98, else the stronger
// nation CTW_AI_STRONGER_CHANCE).
bool ConquestGame::resolve_ai_vs_ai(const LinkChoice& l) {
	const int A = l.attacker, D = l.defender;
	Leader& AL = leaders[size_t(A)];
	// the two sides pick their tactical cards (the picks use the campaign's stream) and spend them
	select_ai_tactical(A, l);
	if (D >= 0) select_ai_tactical(D, l);
	consume_selected(A, false);
	consume_selected(D, false);
	int p;
	if (D < 0) {
		p = K(*rules, "ctw_ai_barb_chance");
	} else if (leaders[size_t(D)].base_armies == 0) {
		p = 98;
	} else {
		leaders[size_t(A)].fought_mask |= 1 << (D & 31);
		leaders[size_t(D)].fought_mask |= 1 << (A & 31);
		p = leaders[size_t(D)].inherent_skill < AL.inherent_skill ? K(*rules, "ctw_ai_stronger_chance") : 100 - K(*rules, "ctw_ai_stronger_chance");
	}
	const bool won = rget(0, 100) < p;
	const std::string an = tribe_name(A);
	Piece* army = nullptr;
	for (Piece& q : pieces[size_t(A)]) {
		if (q.node == l.from) army = &q;
	}
	if (won) {
		const std::string dn = D >= 0 ? "the " + tribe_name(D) : "barbarians";
		if (D >= 0 && leaders[size_t(D)].base_armies == 0) {
			// a client state: its army retreats and it becomes a vassal
			pieces[size_t(D)].erase(std::remove_if(pieces[size_t(D)].begin(), pieces[size_t(D)].end(), [&](const Piece& q) { return q.node == l.to; }),
			                        pieces[size_t(D)].end());
			set_vassal(A, D);
			give_tribute(A, nodes[size_t(l.to)].tribute);
			if (nodes[size_t(l.to)].card.valid()) {
				give_card(A, nodes[size_t(l.to)].card);
				nodes[size_t(l.to)].card = Card();
			}
			if (army) {
				army->node = l.to;
				army->moved = true;
			}
			add_news("The " + an + " subdue " + dn + " at " + node_name(l.to) + ".");
		} else {
			std::string extra;
			transfer_to(l.to, A, true, &extra);
			if (army) {
				for (Piece& q : pieces[size_t(A)]) {
					if (q.node == l.from) {
						q.node = l.to;
						q.moved = true;
					}
				}
			}
			add_news("The " + an + " take " + node_name(l.to) + " from " + dn + ".");
		}
	} else {
		// the failed attacker's army is lost; the defender's purse grows
		if (army) {
			pieces[size_t(A)].erase(std::remove_if(pieces[size_t(A)].begin(), pieces[size_t(A)].end(), [&](const Piece& q) { return q.node == l.from; }),
			                        pieces[size_t(A)].end());
		}
		if (D >= 0) give_tribute(D, K(*rules, "ctw_def_victory_bonus"));
		add_news("The " + an + " fail to take " + node_name(l.to) + ".");
	}
	if (leaders[size_t(A)].active) place_new_armies(A);
	if (D >= 0 && leaders[size_t(D)].active) place_new_armies(D);
	return won;
}

// ---------------------------------------------------------------- plan_strategy

int ConquestGame::plan_strategy(int slot, LinkChoice* attack) {
	Leader& L = leaders[size_t(slot)];
	L.attack_link = -1;
	pending_script_attack = LinkChoice();
	// ---- the campaign's strategy script (run_strategy_script) plays the turn when it says so
	if (run_strategy_script(slot)) {
		LinkChoice atk = pending_script_attack;
		pending_script_attack = LinkChoice();
		bool legal = atk.from >= 0 && atk.attacker == slot && nodes[size_t(atk.to)].owner != slot;
		if (legal) legal = army_at(atk.from) != nullptr;
		if (!legal) return 0;
		atk.defender = node_eff_owner(atk.to);
		if (atk.defender == slot) return 0;
		atk.overrun = check_overrun(atk);
		*attack = atk;
		return atk.overrun ? 1 : 2;
	}
	std::vector<LinkChoice> links = find_available_links(slot);
	// ---- the armies move toward the front
	if (!def->free_move) {
		for (size_t k = 0; k < pieces[size_t(slot)].size(); k++) {
			const int a = pieces[size_t(slot)][k].node;
			int best = -1, best_count = -1;
			bool at_front = false;
			for (int nb : nodes[size_t(a)].links) {
				const int o = node_eff_owner(nb);
				if (o == slot) {
					if (has_army(nb)) continue;
					int count = 0;
					for (int m : nodes[size_t(nb)].links) {
						const int o2 = node_eff_owner(m);
						if (o2 != slot && at_war_or_barbarian(*this, slot, o2)) count++;
					}
					if (count > best_count) {
						best_count = count;
						best = nb;
					}
				} else if (at_war_or_barbarian(*this, slot, o)) {
					at_front = true;
					break;
				}
			}
			if (!at_front && best >= 0) {
				pieces[size_t(slot)][k].node = best;
				pieces[size_t(slot)][k].moved = true;
				links = find_available_links(slot);
			}
		}
	} else {
		// free movement (Cold War): rear armies go to empty nodes of the front
		std::vector<int> cand;
		for (size_t n = 0; n < nodes.size(); n++) {
			if (!at_war_or_barbarian(*this, slot, node_eff_owner(int(n)))) continue;
			for (int m : nodes[n].links) {
				if (node_eff_owner(m) != slot) continue;
				if (has_army(m)) break;
				if (std::find(cand.begin(), cand.end(), m) == cand.end()) cand.push_back(m);
			}
		}
		for (int c : cand) {
			for (Piece& p : pieces[size_t(slot)]) {
				if (num_adjacent_war(*this, p.node) == 0 && !has_army(c)) {
					p.node = c;
					break;
				}
			}
		}
		links = find_available_links(slot);
	}
	// ---- group the attacks by the defending nation; look for an overrun on the way
	std::vector<LinkChoice> usable;
	for (const LinkChoice& lc : links) {
		const Piece* p = nullptr;
		for (const Piece& q : pieces[size_t(slot)]) {
			if (q.node == lc.from) p = &q;
		}
		if (p && !p->moved) usable.push_back(lc);
	}
	std::vector<int> owners;                      // the defending nations in order of first appearance (-1: barbarians)
	std::vector<std::vector<int>> groups;         // link indexes per owner
	std::vector<int> war_list;                    // nations at war with us with no pending truce
	const int max_att = K(*rules, "ctw_max_attitude");
	for (size_t i = 0; i < usable.size(); i++) {
		const LinkChoice& lc = usable[size_t(i)];
		const int D = lc.defender;
		size_t gi = 0;
		while (gi < owners.size() && owners[gi] != D) gi++;
		if (gi == owners.size()) {
			owners.push_back(D);
			groups.emplace_back();
		}
		groups[gi].push_back(int(i));
		if (D >= 0 && std::find(war_list.begin(), war_list.end(), D) == war_list.end() && L.stances[size_t(D)] == kWar &&
		    L.no_attack_time[size_t(D)] == 0)
			war_list.push_back(D);
		// the overrun: the original tests no_attack_time[D] (for a barbarian target it reads the slot before the array)
		const int truce = D >= 0 ? L.no_attack_time[size_t(D)] : L.attitude_base[size_t(kSlots - 1)];
		if (truce != 0) continue;
		if (!check_overrun_nodes(lc.from, lc.to, -1, nullptr)) continue;
		bool do_it = true;
		if (D >= 0) {
			const int st = L.stances[size_t(D)];
			const int att = get_attitude(slot, D);
			if (st == kAlly || (st == kPeace && (att > max_att * 3 / 5 || war_cost(slot, D) > L.tribute))) {
				do_it = false;
			} else if (D != player_tribe) {
				if (st == kPeace && rget(0, 100) > K(*rules, "ctw_overrun_peace_prob")) do_it = false;
			}
		}
		if (D == player_tribe) {
			int p;
			if (difficulty == 0) p = K(*rules, "ctw_overrun_easiest_prob");
			else if (difficulty == 1) p = K(*rules, "ctw_overrun_easy_prob");
			else if (difficulty == 2) p = K(*rules, "ctw_overrun_moderate_prob");
			else if (L.stances[size_t(D)] == kPeace && rget(0, 100) > K(*rules, "ctw_overrun_peace_prob")) p = 0;
			else p = 100;
			if (p < rget(0, 100)) continue;
		}
		if (!do_it) continue;
		if (D >= 0 && L.stances[size_t(D)] == kPeace) {
			// declare_war(me, D, 0): the declaration costs tribute (the overrun goes on even when it cannot be paid)
			const int cost = war_cost(slot, D);
			if (L.tribute >= cost) {
				L.tribute -= cost;
				set_war(slot, D);
			}
		}
		if (D == player_tribe && demand_tribute(slot, D)) continue;  // the player paid
		*attack = lc;
		attack->overrun = true;
		L.attack_link = int(i);
		return 1;
	}
	// ---- a fortification (infrastructure) at the weakest border node, or a card
	if (rget(0, 4) == 0) {
		int best = -1, best_level = 8;
		for (size_t i = 0; i < nodes.size(); i++) {
			if (node_eff_owner(int(i)) != slot || num_adjacent_enemies(*this, int(i)) == 0) continue;
			if (nodes[i].level < best_level) {
				best_level = nodes[i].level;
				best = int(i);
			}
		}
		if (best >= 0) {
			const int cost = build_cost(best);
			if (L.tribute >= cost && nodes[size_t(best)].level <= 6) {
				L.tribute -= cost;
				nodes[size_t(best)].level++;
			}
		}
	} else {
		const int price = std::min(K(*rules, "ctw_card_cost_num_purchased_x") * L.num_cards_purchased + K(*rules, "ctw_base_card_cost"),
		                           K(*rules, "ctw_card_cost_ramp_cap"));
		if (price <= L.tribute && rget(0, 100) < 10) buy_card(slot);
	}
	// ---- barbarians first: the link whose target has the most neighbours of other nations
	for (size_t gi = 0; gi < owners.size(); gi++) {
		if (owners[gi] != -1) continue;
		if (slot != tribe_must_attack) {
			int best = -1, best_count = -1;
			for (int li : groups[gi]) {
				int count = 0;
				for (int m : nodes[size_t(usable[size_t(li)].to)].links) {
					const int o = node_eff_owner(m);
					if (o >= 0 && o != slot) count++;
				}
				if (count > best_count) {
					best_count = count;
					best = li;
				}
			}
			if (best >= 0) {
				*attack = usable[size_t(best)];
				L.attack_link = best;
				return 2;
			}
		}
		break;
	}
	// ---- nations at war with us: the best-scoring link (easy games prefer strong targets, others weak ones)
	if (!war_list.empty()) {
		int best_score = -10;
		struct Cand {
			int link, metric;
		};
		std::vector<Cand> cands;  // kept in the original's order: ascending metric, a new equal key goes first
		auto insert = [&](int link, int metric) {
			size_t pos = 0;
			while (pos < cands.size() && cands[pos].metric < metric) pos++;
			cands.insert(cands.begin() + long(pos), {link, metric});
		};
		for (int D : war_list) {
			size_t gi = 0;
			while (gi < owners.size() && owners[gi] != D) gi++;
			for (int li : groups[gi]) {
				const LinkChoice& lc = usable[size_t(li)];
				if (slot == tribe_must_attack && D != player_tribe) continue;
				if (check_potential_overrun(slot, lc.to)) continue;
				const Node& N = nodes[size_t(lc.to)];
				int score, metric;
				if (difficulty < 2) {
					score = N.level;
					metric = num_supporting(*this, lc.to, lc.to) - num_supporting(*this, lc.to, lc.from);
				} else {
					score = -N.level;
					metric = num_supporting(*this, lc.to, lc.from) - num_supporting(*this, lc.to, lc.to);
				}
				if (best_score < score) {
					best_score = score;
					cands.clear();
					insert(li, metric);
				} else if (score == best_score) {
					insert(li, metric);
				}
			}
		}
		bool tried_player = false;
		for (size_t k = cands.size(); k-- > 0;) {  // from the largest metric
			const int li = cands[k].link;
			const LinkChoice& lc = usable[size_t(li)];
			if (node_eff_owner(lc.to) != player_tribe) {
				*attack = lc;
				L.attack_link = li;
				return 2;
			}
			if (!tried_player && turns_no_attack == 0 && (L.prev_node_attacked != lc.to || cands.size() < 2)) {
				if (slot != tribe_must_attack) {
					if (demand_tribute(slot, node_eff_owner(lc.to))) {
						tried_player = true;  // the player paid: no attack on his territory this turn
						continue;
					}
				}
				*attack = lc;
				L.attack_link = li;
				turns_no_attack = crules->min_no_attack[size_t(difficulty)];
				L.prev_node_attacked = lc.from;
				return 2;
			}
		}
	}
	return 0;
}

}  // namespace ron::sim::ctw
