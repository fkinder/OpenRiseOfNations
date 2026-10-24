// Conquer the World: the 38 bonus cards (docs/ctw-spec.md section 4): names and variants, the shuffled deck, buying,
// the strategic cards (played on the map) and the tactical cards (selected before a battle; their battle-side effects
// are applied in ctw_battle.cpp).
#include <algorithm>

#include "sim/ctw.hpp"
#include "sim/rules_xml.hpp"

namespace ron::sim::ctw {

namespace {

const char* const kTechLines[5] = {"Military", "Civic", "Commerce", "Science", "Universal"};
const char* const kResources[5] = {"Food", "Timber", "Wealth", "Knowledge", "Metal"};

bool ends_with(const std::string& s, const std::string& suf) {
	return s.size() >= suf.size() && lower(s.substr(s.size() - suf.size())) == lower(suf);
}
std::string strip(std::string s) {
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
	size_t a = 0;
	while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) a++;
	return s.substr(a);
}

}  // namespace

std::string ConquestGame::card_name(const Card& c) const {
	if (!c.valid()) return std::string();
	switch (c.type) {
	case kOath: return tribe_name(c.variant) + " Oath of Fealty";
	case kEureka: return std::string(kTechLines[std::clamp(c.variant, 0, 4)]) + " Eureka";
	case kEconomicBoom: return std::string(kResources[std::clamp(c.variant, 0, 4)]) + " Economic Boom";
	default: return crules->cards[size_t(c.type)].name;
	}
}

std::string ConquestGame::card_desc(const Card& c) const {
	if (!c.valid()) return std::string();
	std::string d = crules->cards[size_t(c.type)].desc;
	if (c.type == kEureka) {
		return c.variant == 4 ? "Start one battle with extra technology from every line."
		                      : "Start one battle with extra " + std::string(kTechLines[c.variant]) + " technology.";
	}
	if (c.type == kEconomicBoom) return "Receive a 5% boost in " + std::string(kResources[std::clamp(c.variant, 0, 4)]) + " production in one battle.";
	if (c.type == kOath) return "Gain the special powers of the " + tribe_name(c.variant) + " for one battle.";
	return d;
}

bool ConquestGame::parse_card(const std::string& raw, Card* out) const {
	const std::string name = strip(raw);
	if (name.empty() || !crules) return false;
	Card c;
	if (ends_with(name, " Oath of Fealty")) {
		const int s = slot_of(name.substr(0, name.size() - 15));
		if (s < 0) return false;
		c.type = kOath;
		c.variant = s;
	} else if (ends_with(name, " Eureka") || lower(name) == "eureka") {
		c.type = kEureka;
		c.variant = 4;
		if (lower(name) != "eureka") {
			const std::string pre = name.substr(0, name.size() - 7);
			for (int k = 0; k < 5; k++) {
				if (ieq(pre, kTechLines[k])) c.variant = k;
			}
		}
	} else if (ends_with(name, " Economic Boom")) {
		c.type = kEconomicBoom;
		c.variant = -1;
		const std::string pre = name.substr(0, name.size() - 14);
		for (int k = 0; k < 5; k++) {
			if (ieq(pre, kResources[k])) c.variant = k;
		}
		if (c.variant < 0) c.variant = 0;
	} else {
		for (int t = 0; t < kNumCardTypes; t++) {
			if (t == kOath || t == kEureka || t == kEconomicBoom) continue;
			if (ieq(crules->cards[size_t(t)].key, name) || ieq(crules->cards[size_t(t)].name, name)) {
				c.type = t;
				break;
			}
		}
		if (c.type < 0) return false;
	}
	if (out) *out = c;
	return true;
}

int ConquestGame::card_value(const Card& c) const { return c.valid() ? crules->cards[size_t(c.type)].value() : 0; }

std::vector<Card> ConquestGame::make_card_list() {
	std::vector<std::pair<int, Card>> keyed;  // each variant is inserted with key rand(0..5000): a shuffle
	for (const std::string& n : deck) {
		const std::string t = strip(n);
		std::vector<Card> variants;
		if (t.compare(0, 7, "$TRIBE0") == 0) {
			if (def->no_oath_cards) continue;
			for (int s = 0; s < kSlots; s++) {
				if (def->tribes[size_t(s)].present) variants.push_back({kOath, s});
			}
		} else if (t.compare(0, 9, "$TECHCAT0") == 0) {
			for (int v = 0; v < 5; v++) variants.push_back({kEureka, v});
		} else if (t.compare(0, 5, "$RES0") == 0) {
			for (int v = 0; v < 5; v++) variants.push_back({kEconomicBoom, v});
		} else {
			Card c;
			if (parse_card(t, &c)) variants.push_back(c);
		}
		for (const Card& c : variants) keyed.push_back({rget(0, 5000), c});
	}
	std::stable_sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first < b.first; });
	std::vector<Card> out;
	for (auto& k : keyed) out.push_back(k.second);
	return out;
}

Card ConquestGame::draw_card() {
	if (pool.empty()) pool = make_card_list();
	if (pool.empty()) return Card();
	Card c = pool.front();
	pool.erase(pool.begin());
	return c;
}

void ConquestGame::give_card(int slot, const Card& c) {
	if (!c.valid() || slot < 0) return;
	Leader& L = leaders[size_t(slot)];
	const CardTypeDef& T = crules->cards[size_t(c.type)];
	if (!T.can_stack()) {
		for (const Card& h : L.cards) {
			if (h.type == c.type && h.variant == c.variant) {  // already held: the card's tribute value instead
				L.tribute += T.value() / 2;
				return;
			}
		}
	}
	L.cards.push_back(c);
	if (slot == player_tribe) add_news("You receive the card '" + card_name(c) + "'.");
}

bool ConquestGame::buy_card(int slot, std::string* why) {
	Leader& L = leaders[size_t(slot)];
	const int cost = card_cost(slot);
	if (L.tribute < cost) {
		if (why) *why = "A card costs " + std::to_string(cost) + " tribute.";
		return false;
	}
	int wonders_held = 0;
	for (const Card& h : L.cards) wonders_held += h.type == kWonderCard;
	Card c;
	for (int tries = 0; tries < 12; tries++) {
		c = draw_card();
		if (!c.valid()) break;
		if (c.type == kWonderCard && wonders_held >= K(*rules, "ctw_max_wonder_cards")) {
			pool.push_back(c);
			c = Card();
			continue;
		}
		break;
	}
	if (!c.valid()) {
		if (why) *why = "There are no cards left to buy.";
		return false;
	}
	L.tribute -= cost;
	L.num_cards_purchased++;
	give_card(slot, c);
	return true;
}

// ---------------------------------------------------------------- strategic cards

std::vector<std::string> ConquestGame::find_available_wonders(int slot) const {
	std::vector<std::string> out;
	(void)slot;
	for (int t = 526; t < 543; t++) {
		const Type& T = rules->types[size_t(t)];
		if (T.age > cur_age) continue;
		bool built = false;
		for (const Node& n : nodes) built |= n.wonder == t;
		for (const Colony& c : colonies) built |= c.wonder == t;
		if (!built) out.push_back(to_utf8(T.name));
	}
	return out;
}

bool ConquestGame::card_targets(int slot, const Card& c, std::vector<int>* tn, std::vector<int>* ts) const {
	std::vector<int> nodes_out, slots_out;
	const Leader& L = leaders[size_t(slot)];
	auto enemy_node = [&](int i) {
		const int o = node_eff_owner(i);
		return o >= 0 && o != slot && stance(slot, o) < kAlly;
	};
	switch (c.type) {
	case kWonderCard:
		if (!find_available_wonders(slot).empty()) {
			for (int n : owned_nodes(slot, false)) {
				if (nodes[size_t(n)].wonder < 0) nodes_out.push_back(n);
			}
		}
		break;
	case kSabotage:
		for (size_t i = 0; i < nodes.size(); i++) {
			if (enemy_node(int(i)) && nodes[i].level > 0) nodes_out.push_back(int(i));
		}
		break;
	case kTreachery:
		for (size_t i = 0; i < nodes.size(); i++) {
			if (!enemy_node(int(i)) && nodes[i].owner >= 0) continue;
			if (nodes[i].owner == slot || (nodes[i].flags & kCapital)) continue;
			if (nodes[i].level > K(*rules, "ctw_max_treachery_infra") + 1) continue;
			bool adjacent = false;
			for (int t : nodes[i].links) adjacent |= node_eff_owner(t) == slot;
			if (adjacent) nodes_out.push_back(int(i));
		}
		break;
	case kBrigands:
		for (int s = 0; s < kSlots; s++) {
			if (s == slot || !leaders[size_t(s)].active || stance(slot, s) >= kAlly) continue;
			if (leaders[size_t(s)].capital >= 0) nodes_out.push_back(leaders[size_t(s)].capital);
		}
		break;
	case kMemnon:
		for (int n : owned_nodes(slot, false)) {
			if (nodes[size_t(n)].level < 7) nodes_out.push_back(n);
		}
		break;
	case kSpitamenes:
		for (size_t i = 0; i < nodes.size(); i++) {
			if (enemy_node(int(i)) && !(nodes[i].flags & kCapital)) nodes_out.push_back(int(i));
		}
		break;
	case kContinentalSystem: {
		const int gb = slot_of("british");
		for (int n : owned_nodes(gb, false)) {
			if (nodes[size_t(n)].level > 0) nodes_out.push_back(n);
		}
		break;
	}
	case kColonyBlockade:
	case kLetterOfMarque:
		for (int s = 0; s < kSlots; s++) {
			if (s != slot && leaders[size_t(s)].active && stance(slot, s) < kAlly) slots_out.push_back(s);
		}
		break;
	case kFouche:
		for (int s = 0; s < kSlots; s++) {
			if (s != slot && leaders[size_t(s)].active && leaders[size_t(s)].vassal_of < 0) slots_out.push_back(s);
		}
		break;
	case kTalleyrand:
		for (int s = 0; s < kSlots; s++) {
			if (s != slot && leaders[size_t(s)].active && leaders[size_t(s)].vassal_of < 0) slots_out.push_back(s);
		}
		break;
	case kPaoli:
		for (int s = 0; s < kSlots; s++) {
			if (leaders[size_t(s)].active && leaders[size_t(s)].vassal_of >= 0 && leaders[size_t(s)].vassal_of != slot) slots_out.push_back(s);
		}
		break;
	case kCommandeerTransport:
		for (const Piece& p : pieces[size_t(slot)]) nodes_out.push_back(p.node);
		break;
	default: break;
	}
	(void)L;
	if (tn) *tn = nodes_out;
	if (ts) *ts = slots_out;
	return !nodes_out.empty() || !slots_out.empty();
}

bool ConquestGame::play_strategic_card(int slot, int hand_index, int target, int target2, std::string* why) {
	Leader& L = leaders[size_t(slot)];
	if (hand_index < 0 || size_t(hand_index) >= L.cards.size()) {
		if (why) *why = "There is no such card.";
		return false;
	}
	const Card c = L.cards[size_t(hand_index)];
	if (!crules->cards[size_t(c.type)].strategic()) {
		if (why) *why = "That card is played in battle.";
		return false;
	}
	std::vector<int> tn, ts;
	card_targets(slot, c, &tn, &ts);
	auto ok_node = [&](int n) { return std::find(tn.begin(), tn.end(), n) != tn.end(); };
	auto ok_slot = [&](int s) { return std::find(ts.begin(), ts.end(), s) != ts.end(); };
	const std::string who = slot == player_tribe ? "You" : "The " + tribe_name(slot);
	switch (c.type) {
	case kWonderCard: {
		const auto avail = find_available_wonders(slot);
		if (!ok_node(target) || avail.empty()) goto bad;
		// the newest wonder of this age or earlier
		int best = -1;
		for (int t = 542; t >= 526 && best < 0; t--) {
			if (std::find(avail.begin(), avail.end(), to_utf8(rules->types[size_t(t)].name)) != avail.end()) best = t;
		}
		nodes[size_t(target)].wonder = best;
		add_news(who + " raise the " + to_utf8(rules->types[size_t(best)].name) + " in " + node_name(target) + ".");
		break;
	}
	case kSabotage:
		if (!ok_node(target)) goto bad;
		set_node_level(target, nodes[size_t(target)].level - 2);
		add_news(who + " sabotage " + node_name(target) + " (strength " + std::to_string(nodes[size_t(target)].level) + ").");
		break;
	case kTreachery: {
		if (!ok_node(target)) goto bad;
		const int old = node_eff_owner(target);
		if (old >= 0 && stance(slot, old) != kWar) set_war(slot, old);
		std::string t;
		transfer_to(target, slot, true, &t);
		add_news(who + " take " + node_name(target) + " by treachery." + t);
		break;
	}
	case kCommandeerTransport:
		free_move_turn = true;
		add_news(who + " commandeer transport: armies move freely this round.");
		break;
	case kBrigands:
		if (!ok_node(target)) goto bad;
		set_node_level(target, nodes[size_t(target)].level / 2);
		add_news(who + " send brigands against " + node_name(target) + ".");
		break;
	case kMemnon:
		if (!ok_node(target)) goto bad;
		set_node_level(target, nodes[size_t(target)].level + K(*rules, "ctw_memnon_infra"));
		add_news(who + " strengthen " + node_name(target) + " with Memnon's works.");
		break;
	case kSpitamenes:
		if (!ok_node(target)) goto bad;
		{
			const int old = node_eff_owner(target);
			transfer_to(target, -1, false, nullptr);
			add_news(who + " rouse the tribes of " + node_name(target) + " against the " + tribe_name(old) + ".");
		}
		break;
	case kColonyBlockade:
		if (!ok_slot(target)) goto bad;
		leaders[size_t(target)].colony_blockade = K(*rules, "ctw_colony_blockade_turns");
		add_news(who + " blockade the colonies of the " + tribe_name(target) + ".");
		break;
	case kContinentalSystem:
		if (!ok_node(target)) goto bad;
		set_node_level(target, nodes[size_t(target)].level - 2);
		add_news(who + " close the continent to British trade (" + node_name(target) + " -2).");
		break;
	case kLetterOfMarque:
		if (!ok_slot(target)) goto bad;
		{
			const int take = std::min(K(*rules, "ctw_steal_trib_amt"), leaders[size_t(target)].tribute);
			leaders[size_t(target)].tribute -= take;
			L.tribute += take;
			add_news(who + " seize " + std::to_string(take) + " tribute from the " + tribe_name(target) + ".");
		}
		break;
	case kFouche:
		if (!ok_slot(target) || !ok_slot(target2) || target == target2) goto bad;
		if (stance(target, target2) != kWar) set_war(target, target2);
		fouche_tribe = target;
		add_news(who + " set the " + tribe_name(target) + " against the " + tribe_name(target2) + ".");
		break;
	case kTalleyrand:
		if (!ok_slot(target)) goto bad;
		talleyrand_tribe = target;
		talleyrand_state = 1;
		set_stance_pair(slot, target, kAlly);
		add_news(who + " win the " + tribe_name(target) + " over for one round.");
		break;
	case kPaoli:
		if (!ok_slot(target)) goto bad;
		{
			const int master = leaders[size_t(target)].vassal_of;
			leaders[size_t(target)].vassal_of = -1;
			set_stance_pair(master, target, kPeace);
			set_stance_pair(slot, target, kPeace);
			assign_supply(master);
			add_news(who + " liberate the " + tribe_name(target) + ".");
		}
		break;
	default: goto bad;
	}
	L.bonus_card_types_played[size_t(c.type)]++;
	L.cards.erase(L.cards.begin() + hand_index);
	assign_bonuses();
	if (slot == player_tribe) available_links = find_available_links(player_tribe);
	return true;
bad:
	if (why) *why = "That is not a valid target.";
	return false;
}

bool ConquestGame::select_tactical(int slot, int hand_index, std::string* why) {
	Leader& L = leaders[size_t(slot)];
	if (hand_index < 0 || size_t(hand_index) >= L.cards.size()) return false;
	const Card c = L.cards[size_t(hand_index)];
	if (!crules->cards[size_t(c.type)].tactical()) {
		if (why) *why = "That card is played on the map.";
		return false;
	}
	L.selected.push_back(c);
	L.cards.erase(L.cards.begin() + hand_index);
	return true;
}

void ConquestGame::clear_selected(int slot) {
	Leader& L = leaders[size_t(slot)];
	for (const Card& c : L.selected) L.cards.push_back(c);
	L.selected.clear();
}

void ConquestGame::play_ai_cards(int slot) {
	Leader& L = leaders[size_t(slot)];
	for (int guard = 0; guard < 6; guard++) {
		int idx = -1;
		for (size_t i = 0; i < L.cards.size(); i++) {
			if (crules->cards[size_t(L.cards[i].type)].strategic()) {
				idx = int(i);
				break;
			}
		}
		if (idx < 0) return;
		const Card c = L.cards[size_t(idx)];
		std::vector<int> tn, ts;
		if (!card_targets(slot, c, &tn, &ts)) return;
		int target = -1, target2 = -1;
		// (?) AI card use is not decompiled: strongest sensible target; the AI plays the card with probability 50%
		if (rget(0, 99) >= 50) return;
		switch (c.type) {
		case kWonderCard:
		case kMemnon: {
			int best = -1;
			for (int n : tn) {
				if (best < 0 || nodes[size_t(n)].value > nodes[size_t(best)].value) best = n;
			}
			target = best;
			break;
		}
		case kSabotage:
		case kBrigands:
		case kContinentalSystem:
		case kTreachery:
		case kSpitamenes: {
			int best = -1;
			for (int n : tn) {
				const int o = node_eff_owner(n);
				if (o >= 0 && stance(slot, o) != kWar && c.type != kTreachery) continue;  // only against enemies at war
				if (best < 0 || nodes[size_t(n)].level > nodes[size_t(best)].level) best = n;
			}
			target = best;
			break;
		}
		case kColonyBlockade:
		case kLetterOfMarque:
			for (int s : ts) {
				if (stance(slot, s) == kWar && (target < 0 || leaders[size_t(s)].tribute > leaders[size_t(target)].tribute)) target = s;
			}
			break;
		default:  // (the AI does not use Commandeer Transport: its armies already move where they need)
			target = -1;
		}
		if (target < 0) {
			// an unplayable strategic card is kept; stop so the same card is not retried this turn
			return;
		}
		if (c.type == kFouche || c.type == kTalleyrand || c.type == kPaoli) return;
		(void)target2;
		play_strategic_card(slot, idx, target, target2, nullptr);
	}
}

}  // namespace ron::sim::ctw
