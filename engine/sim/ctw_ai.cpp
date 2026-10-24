// Conquer the World: the AI's strategic turn (docs/ctw-spec.md 2.3, 6). `plan_strategy` itself (the scoring of
// targets and of army placement) is the spec's unknown "?": what follows uses the spec's known rules (who may attack
// whom, the AI-vs-AI win probabilities, overruns, the player's immunity window) and a documented heuristic for the
// choice among legal attacks.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>

#include "sim/ctw.hpp"

namespace ron::sim::ctw {

bool ConquestGame::check_overrun(const LinkChoice& l) const {
	// check_overrun @791f90: the attacker's side outnumbers the defender's available armies around the node by >= 2.
	const int att = 1 + int(l.att_supporters.size());
	int def_n = int(l.def_supporters.size());
	const Node& T = nodes[size_t(l.to)];
	bool defender_army = false;
	for (const auto& list : pieces) {
		for (const Piece& p : list) defender_army |= p.node == l.to;
	}
	(void)T;
	def_n += defender_army ? 1 : 0;
	return att - def_n >= 2;
}

bool ConquestGame::resolve_ai_vs_ai(const LinkChoice& l) {
	Leader& A = leaders[size_t(l.attacker)];
	if (getenv("CTW_DEBUG")) fprintf(stderr, "ai attack: slot %d from %d to %d defender %d\n", l.attacker, l.from, l.to, l.defender);
	Piece* army = nullptr;
	for (Piece& p : pieces[size_t(l.attacker)]) {
		if (p.node == l.from) army = &p;
	}
	bool won;
	if (check_overrun(l)) {
		won = true;  // resolve_overrun just transfers the node (spec 6)
	} else {
		int p;
		if (l.defender < 0) p = K(*rules, "ctw_ai_barb_chance");
		else if (leaders[size_t(l.defender)].base_armies == 0) p = 98;
		else p = A.inherent_skill > leaders[size_t(l.defender)].inherent_skill ? K(*rules, "ctw_ai_stronger_chance") : 100 - K(*rules, "ctw_ai_stronger_chance");
		won = rget(0, 100) < p;
	}
	if (army) army->moved = true;
	const std::string an = tribe_name(l.attacker);
	if (won) {
		const std::string dn = l.defender >= 0 ? "the " + tribe_name(l.defender) : "barbarians";
		std::string extra;
		const bool defender_is_client = l.defender >= 0 && leaders[size_t(l.defender)].base_armies == 0 && def->allow_vassals;
		if (defender_is_client) {
			set_vassal(l.attacker, l.defender);
			add_news("The " + an + " subdue " + dn + " at " + node_name(l.to) + ".");
		} else {
			transfer_to(l.to, l.attacker, true, &extra);
			if (army) {
				// the army moves in (it may have been removed together with a capital's elimination)
				for (Piece& p : pieces[size_t(l.attacker)]) {
					if (p.node == l.from) {
						p.node = l.to;
						p.moved = true;
					}
				}
			}
			add_news("The " + an + " take " + node_name(l.to) + " from " + dn + ".");
		}
	} else if (army) {
		// the failed attacker's army is lost (it re-musters at the next round's supply pass)
		pieces[size_t(l.attacker)].erase(std::remove_if(pieces[size_t(l.attacker)].begin(), pieces[size_t(l.attacker)].end(),
		                                                [&](const Piece& p) { return p.node == l.from; }),
		                                 pieces[size_t(l.attacker)].end());
		if (l.defender >= 0) give_tribute(l.defender, 0);
	}
	if (l.defender >= 0) leaders[size_t(l.defender)].fought_mask |= 1 << (l.attacker & 15);
	return won;
}

namespace {

// distance (in links) from every node to the nearest "enemy" node (barbarian or at war with `slot`).
std::vector<int> enemy_distance(const ConquestGame& g, int slot) {
	std::vector<int> d(g.nodes.size(), 1 << 20);
	std::deque<int> q;
	for (size_t i = 0; i < g.nodes.size(); i++) {
		const int o = g.node_eff_owner(int(i));
		if (o != g.eff(slot) && (o < 0 || g.stance(slot, o) == kWar)) {
			d[i] = 0;
			q.push_back(int(i));
		}
	}
	while (!q.empty()) {
		const int n = q.front();
		q.pop_front();
		for (size_t k = 0; k < g.nodes.size(); k++) {
			bool adj = false;
			for (int t : g.nodes[k].links) adj |= t == n;
			if (adj && d[k] > d[size_t(n)] + 1) {
				d[k] = d[size_t(n)] + 1;
				q.push_back(int(k));
			}
		}
	}
	return d;
}

}  // namespace

bool ConquestGame::plan_strategy(int slot, LinkChoice* attack) {
	// (the campaign's strategy script is run once per tribe turn by ai_step, not here)
	Leader& L = leaders[size_t(slot)];
	std::vector<LinkChoice> options;
	for (const LinkChoice& lc : find_available_links(slot)) {
		const Piece* p = nullptr;
		for (const Piece& q : pieces[size_t(slot)]) {
			if (q.node == lc.from) p = &q;
		}
		if (!p || p->moved) continue;
		if (lc.needs_war) continue;
		if (lc.defender == player_tribe && (turns_no_attack > 0 || L.no_attack_time[size_t(player_tribe)] > 0)) continue;
		options.push_back(lc);
	}
	const int pick = ai_army_target(slot, options);
	if (pick >= 0 && rget(0, 99) < 75) {
		*attack = options[size_t(pick)];
		for (Piece& q : pieces[size_t(slot)]) {
			if (q.node == attack->from) q.moved = true;
		}
		if (attack->defender == player_tribe) turns_no_attack = crules->min_no_attack[size_t(difficulty)];  // attacked_player
		return true;
	}
	// no attack: bring unmoved armies toward the nearest enemy
	const std::vector<int> dist = enemy_distance(*this, slot);
	for (Piece& p : pieces[size_t(slot)]) {
		if (p.moved) continue;
		int best = -1, bd = dist[size_t(p.node)];
		for (int t : nodes[size_t(p.node)].links) {
			if (can_drop_army(p.node, t, slot) != 0) continue;
			if (dist[size_t(t)] < bd) {
				bd = dist[size_t(t)];
				best = t;
			}
		}
		if (best >= 0 && dist[size_t(p.node)] > 1 && rget(0, 99) < 60) {
			p.node = best;
			p.moved = true;
		}
	}
	return false;
}

int ConquestGame::ai_army_target(int slot, const std::vector<LinkChoice>& options) const {
	int best = -1, best_score = -1000000;
	(void)slot;
	for (size_t i = 0; i < options.size(); i++) {
		const LinkChoice& lc = options[i];
		const Node& T = nodes[size_t(lc.to)];
		int score = T.value + T.tribute / 2 + (T.card.valid() ? 10 : 0) - T.level * 6;
		if (T.flags & kCapital) score += 25;
		if (T.flags & kPriority) score += 15;
		if (lc.defender < 0) score += 8;
		if (lc.defender == player_tribe) score += 5;
		score += int(lc.att_supporters.size()) * 3 - int(lc.def_supporters.size()) * 4;
		if (score > best_score) {
			best_score = score;
			best = int(i);
		}
	}
	return best;
}

}  // namespace ron::sim::ctw
