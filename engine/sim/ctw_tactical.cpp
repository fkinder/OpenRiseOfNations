// Conquer the World: the tactical bonus cards in a battle (docs/ctw-spec.md 4). Ported from
//   ConquestGame::setup_scenario_bonuses @78be90   the cards' immediate effects and the free techs of the tribe bonuses,
//   ConquestGame::setup_scenario_rares  @78ca60    the rare goods of the nations (and the Trade Embargo),
//   ConquestGame::build_conquest_game   @797c40    the Political Dissidents and Traitors loops,
//   ConquestGame::build_units           @797200    the units a card brings (elephants, generals, spies, citizens, ...),
//   ConquestGame::build_cities          @796140    Warpath's buildings,
//   ConquestLeader::select_tactical_cards @79d600  the AI's picks,
//   ConquestStyle::get_bonus_amt        @7a9240    the Skill Bonus amount (the style's `bonus` script).
// The cards that only change a rate (Great Thinker, Merchant Guild, Logistics, Partisans, Missionaries) are read where the
// rate is computed (costs.cpp, econ_market.cpp, attrition.cpp, build.cpp, econ_income.cpp, territory.cpp) through
// `Leader::num_bonus_cards[type]`, incremented here.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "script/bhs_common.hpp"
#include "script/bhs_vm.hpp"
#include "sim/ai.hpp"
#include "sim/ctw.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/production.hpp"
#include "sim/script_host.hpp"
#include "sim/placement.hpp"
#include "sim/search.hpp"
#include "sim/spells.hpp"

namespace ron::sim::ctw {

namespace {

constexpr int kLineBase[4] = {572, 565, 558, 551};  // epoch lines: military (The Art of War), civic, commerce, science

// Gains up to `n` epoch techs of a library line: the first not held, then the following ones (the lines have 7 epochs).
void epoch_line_techs(Game& g, int who, int line, int n) {
	const int base = kLineBase[line];
	int t = base;
	while (t < base + 7 && has_tech(g, who, t)) t++;
	for (int k = 0; k < n; k++) {
		if (t < base + 7) {
			gain_tech(g, who, t, -1, -1, false, true);
			t++;
		}
	}
}

int find_type_by_name(const sim::Rules& r, int lo, int hi, const std::string& name) {
	const std::string t = lower(name);
	for (int i = lo; i < hi; i++) {
		const Type& T = r.types[size_t(i)];
		if (!T.loaded) continue;
		if (lower(to_utf8(T.name)) == t || lower(to_utf8(T.type_name)) == t || lower(to_utf8(T.graph_name)) == t) return i;
	}
	return -1;
}

const Build* seat_first_city(const Game& g, int who) {
	const ObjectList& ol = g.objects[size_t(who)];
	for (int32_t o = kBuildBase; o < ol.build_mark; o++) {
		const Build* b = g.build(who, o);
		if (b && b->alive() && (b->flags & objflag::kCity)) return b;
	}
	return nullptr;
}

bool seat_start(const Game& g, int who, int32_t* x, int32_t* y) {
	if (const Build* c = seat_first_city(g, who)) {
		*x = c->x;
		*y = c->y;
		return true;
	}
	const ObjectList& ol = g.objects[size_t(who)];
	for (int32_t o = 0; o < ol.unit_mark; o++) {
		const Unit* u = g.unit(who, o);
		if (u && u->alive()) {
			*x = u->x;
			*y = u->y;
			return true;
		}
	}
	return false;
}

int place_card_unit(Game& g, int who, int type, int32_t x, int32_t y) {
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

// ---------------------------------------------------------------- the AI's picks (select_tactical_cards @79d600)

void ConquestGame::select_ai_tactical(int slot, const LinkChoice& l) {
	Leader& L = leaders[size_t(slot)];
	// the previous selection returns to the hand
	for (const Card& c : L.selected) L.cards.push_back(c);
	L.selected.clear();
	const int hand = int(L.cards.size());
	// at its own capital the AI plays everything when its hand is small, else with CTW_AI_ALL_CARDS_PROB the whole hand,
	// otherwise half of it (mode -1)
	int mode = 0;
	if (l.to == L.capital) {
		if (hand < K(*rules, "ctw_ai_all_cards_num")) mode = 1;
		else mode = rget(0, 100) < K(*rules, "ctw_ai_all_cards_prob") ? 1 : -1;
	}
	// allies fight on the link and the player is a party: a higher chance per card
	bool ally_prob = false;
	if (!l.att_supporters.empty() || !l.def_supporters.empty()) ally_prob = node_eff_owner(l.to) == player_tribe || node_eff_owner(l.from) == player_tribe;
	std::vector<bool> pick(L.cards.size(), false);
	int count = 0;
	for (int i = 0; i < hand; i++) {
		const CardTypeDef& d = crules->cards[size_t(L.cards[size_t(i)].type)];
		if (d.strategic()) continue;
		bool take = false;
		if (mode == 0) take = rget(0, 100) < K(*rules, ally_prob ? "ctw_ai_tac_card_ally_prob" : "ctw_ai_tac_card_prob");
		else if (mode < 0) take = count < hand / 2;
		else take = true;
		if (take) count++;
		// a second card of a type that cannot stack is left out
		for (int j = 0; j < i && take; j++) {
			if (L.cards[size_t(j)].type == L.cards[size_t(i)].type && !crules->cards[size_t(L.cards[size_t(i)].type)].can_stack()) take = false;
		}
		pick[size_t(i)] = take;
	}
	std::vector<Card> keep;
	for (int i = 0; i < hand; i++) {
		if (pick[size_t(i)]) L.selected.push_back(L.cards[size_t(i)]);
		else keep.push_back(L.cards[size_t(i)]);
	}
	L.cards = keep;
}

// The cards of a finished battle are spent (the AI's, too); General Parmenio stays after his first play.
void ConquestGame::consume_selected(int slot, bool count_played) {
	if (slot < 0) return;
	Leader& L = leaders[size_t(slot)];
	for (const Card& c : L.selected) {
		const bool first = L.bonus_card_types_played[size_t(c.type)] == 0;
		if (count_played) L.bonus_card_types_played[size_t(c.type)]++;
		if (c.type == kParmenio && (count_played ? first : L.bonus_card_types_played[size_t(c.type)] == 1)) L.cards.push_back(c);
	}
	L.selected.clear();
}

// ---------------------------------------------------------------- applying the cards to the built battle

void ConquestGame::apply_battle_cards(Game& g, const BattleDef& B) const {
	ConquestGame& self = const_cast<ConquestGame&>(*this);
	const int n = std::min(8, int(B.seats.size()));
	const LinkChoice& l = B.link;
	const bool def_style = B.defender_default;
	const int att_owner = node_eff_owner(l.from), def_owner = node_eff_owner(l.to);
	// ---- setup_scenario_bonuses, per seat (only the two main nations choose cards)
	for (int i = 0; i < n && i < 2; i++) {
		const BattleSeat& bs = B.seats[size_t(i)];
		if (bs.slot < 0) continue;
		const Leader& CL = leaders[size_t(bs.slot)];
		sim::Leader& GL = g.leaders[size_t(i)];
		const int opp_slot = bs.slot == l.attacker ? l.defender : l.attacker;
		for (const Card& c : CL.selected) {
			if (c.type < 0 || c.type >= kNumCardTypes || !crules->cards[size_t(c.type)].tactical()) continue;
			if (GL.num_bonus_cards[size_t(c.type)] < 255) GL.num_bonus_cards[size_t(c.type)]++;
			switch (c.type) {
			case kOath:
				if (c.variant >= 0 && c.variant < kSlots) GL.conquest_racial_powers[size_t(c.variant >> 3) & 3] |= uint8_t(1u << (c.variant & 7));
				if (c.variant == 9 && !B.custom_map.empty()) {
					// the Spanish oath on a campaign map: two scouts at the style's start
					const int scout = 0x45;
					const int have = GL.num_units[size_t(scout - kUnits)];
					if (have < 2) {
						const StyleDef& st = B.style.index >= 0 && size_t(B.style.index) < style_table.size() ? style_table[size_t(B.style.index)] : StyleDef();
						int sx = i == 0 ? st.player_start_x : st.ai_start_x, sy = i == 0 ? st.player_start_y : st.ai_start_y;
						int32_t px = 0, py = 0;
						const bool at_style = sx >= 0 && sy >= 0;
						if (at_style) {
							px = sx * 192 + 96;
							py = sy * 192 + 96;
						}
						if (at_style || seat_start(g, i, &px, &py)) {
							const int t = current_upgrade(g, i, scout);
							for (int k = have; k < 2; k++) place_card_unit(g, i, t, px, py);
						}
					}
				}
				if (c.variant == 0x17) GL.bucket[0] = ((K(*rules, "persians_bonus_food") + 100) * GL.bucket[0]) / 100;
				break;
			case kEureka:
				if (c.variant == 4) {
					for (int line = 0; line < 4; line++) epoch_line_techs(g, i, line, K(*rules, "ctw_eureka_all_techs"));
				} else {
					const int node = l.attacker == bs.slot ? l.from : l.to;
					const int line = c.variant >= 0 && c.variant < 4 ? c.variant : nodes[size_t(node)].tech_cat;
					epoch_line_techs(g, i, line, K(*rules, "ctw_eureka_techs"));
				}
				break;
			case kEconomicBoom:
				if (c.variant >= 0 && c.variant < 6) GL.num_ctw_rate_bonuses[size_t(c.variant)]++;
				break;
			case kMercenaries: break;  // +1 army: BattleDef::start_armies (make_battle)
			case kCulturalDominance:
				// the first leader that is not an ally loses its nation powers (@78c2f0: the loop breaks after one)
				for (int j = 0; j < 8; j++) {
					if (!(g.leaders[size_t(j)].flags & leaderflag::kValid)) continue;
					if (!is_ally(g, i, j)) {
						g.leaders[size_t(j)].flags2 |= 0x40;
						break;
					}
				}
				break;
			case kPropaganda:
				if (opp_slot >= 0) GL.conquest_racial_powers[size_t(opp_slot >> 3) & 3] |= uint8_t(1u << (opp_slot & 7));
				break;
			case kSkillBonus:
				for (int k = 0; k < 6; k++) GL.bucket[size_t(k)] += c.variant;
				break;
			case kSpyNetwork: GL.flags |= leaderflag::kExploreMap; break;
			case kWarpath:
				epoch_line_techs(g, i, 0, 2);
				break;
			default: break;
			}
		}
		(void)att_owner;
		(void)def_owner;
	}
	// Warpath: a Barracks, a Stable and a Siege Factory in every city of a seat that holds the card (build_cities @796140)
	for (int i = 0; i < n; i++) {
		if (g.leaders[size_t(i)].num_bonus_cards[kWarpath] == 0) continue;
		for (const char* bn : {"Barracks", "Stable", "Siege Factory"}) {
			const int t0 = find_type_by_name(*rules, kBuilds, kEndBuilds, bn);
			if (t0 < 0) continue;
			const int t = current_upgrade(g, i, t0);
			for (const City& c : g.cities[size_t(i)]) {
				if (!(c.city_flags & cityflag::kValid)) continue;
				produce_building(g, i, t >= kBuilds && t < kEndBuilds ? t : t0, c.o, 0);
			}
		}
	}
	// the free technologies of the tribe bonuses (Leader::init does them outside Conquer the World)
	for (int i = 0; i < n; i++) {
		auto free_epoch = [&](int bonus, const char* name, int cat) {
			if (!has_tribe_bonus(g, i, bonus) || K(*rules, name) == 0) return;
			const int lvl = g.leaders[size_t(i)].epoch[size_t(cat)];
			if (lvl >= 7) return;
			gain_tech(g, i, kLineBase[cat == 0 ? 0 : cat == 1 ? 1 : cat == 2 ? 2 : 3] + lvl, 0, 0, false, true);
		};
		free_epoch(0x16, "dutch_free_commerce", 2);
		free_epoch(0xd, "russian_free_civic", 1);
		free_epoch(0, "aztec_free_military", 0);
		free_epoch(6, "roman_free_military", 0);
		free_epoch(0x14, "americans_free_science", 3);
		if (has_tribe_bonus(g, i, 0x17) && K(*rules, "persians_despotism") != 0) gain_tech(g, i, 623, 0, 0, false, true);
	}
	// ---- setup_scenario_rares: the rare goods of the nations' territories and colonies (default styles)
	if (def_style) {
		for (int i = 0; i < n; i++) {
			const int slot = B.seats[size_t(i)].slot;
			if (slot < 0) continue;
			if (B.map_style == kMsCustom && i >= 6) continue;
			const Leader& L = leaders[size_t(slot)];
			if (L.rares_off) continue;
			// Trade Embargo: a card-holding nation's non-allied opponents get nothing
			bool embargoed = false;
			for (int j = 0; j < 8 && !embargoed; j++) {
				const sim::Leader& Hj = g.leaders[size_t(j)];
				if (!(Hj.flags & leaderflag::kValid) || Hj.num_bonus_cards[kTradeEmbargo] == 0 || j == i) continue;
				if (Hj.diplos(i) != 2 || g.leaders[size_t(i)].diplos(j) != 2) embargoed = true;
			}
			if (embargoed) continue;
			sim::Leader& GL = g.leaders[size_t(i)];
			auto give = [&](int rg) {
				if (rg < 6 || rg >= 50 || type_avail(g, i, rg, 1) == 0) return;
				GL.rare_conquest[size_t((rg - 6) >> 3)] |= uint8_t(1u << ((rg - 6) & 7));
			};
			for (int rg : L.rare_resources) give(rg);
			if (L.colony_blockade == 0) {
				for (const Colony& co : colonies) {
					if (co.owner != slot) continue;
					for (int rg : co.rares) give(rg);
				}
			}
		}
	}
	// ---- Political Dissidents: the holder's non-allied opponents lose their governments for the battle
	for (int i = 0; i < 8; i++) {
		if (g.leaders[size_t(i)].num_bonus_cards[kPoliticalDissidents] == 0) continue;
		for (int j = 0; j < 8; j++) {
			if (j == i || !(g.leaders[size_t(j)].flags & leaderflag::kValid)) continue;
			if (g.leaders[size_t(j)].diplos(i) == 2 && g.leaders[size_t(i)].diplos(j) == 2) continue;
			g.leaders[size_t(j)].flags2 |= 0x800;
			for (int t = 623; t <= 628; t++) {  // 0x26f..0x274 (Leader::lose_tech, reduced to dropping the held government)
				if (g.leaders[size_t(j)].has_tech_bit(t)) {
					g.leaders[size_t(j)].set_tech_bit(t, false);
					g.leaders[size_t(j)].discovered--;
				}
			}
		}
	}
	// ---- Traitors: informers at every enemy military building (a plant of the holder in the building)
	for (int i = 0; i < 8; i++) {
		if (g.leaders[size_t(i)].num_bonus_cards[kTraitors] == 0) continue;
		for (int j = 0; j < 8; j++) {
			if (j == i || !(g.leaders[size_t(j)].flags & leaderflag::kValid)) continue;
			if (g.leaders[size_t(j)].diplos(i) == 2 && g.leaders[size_t(i)].diplos(j) == 2) continue;
			const ObjectList& ol = g.objects[size_t(j)];
			for (int32_t o = kBuildBase; o < ol.build_mark; o++) {
				Build* b = g.build(j, o);
				if (!b || !b->alive()) continue;
				if (!spell_is_valid_target(g, 0x27f, i, o, j, nullptr)) continue;
				if (!(b->ptype->build_flags & 0x10) || (b->ptype->build_flags & 0x40) || type_is(*b->ptype, 0x19e)) continue;
				b->infiltrated = int8_t(uint8_t(b->infiltrated) | uint8_t(1u << (i & 7)));
			}
		}
	}
	// ---- the units the cards bring (build_units @797200): the attacker's and the defender's seat in a default style, only
	// the defender's in the special styles; at the seat's start
	for (int i = 0; i < n && i < 2; i++) {
		const BattleSeat& bs = B.seats[size_t(i)];
		if (bs.slot < 0) continue;
		const int eff_slot = eff(bs.slot);
		const bool eligible = def_style ? (eff_slot == att_owner || eff_slot == def_owner) : eff_slot == def_owner;
		if (!eligible) continue;
		const Leader& CL = leaders[size_t(bs.slot)];
		int32_t sx = 0, sy = 0;
		if (!seat_start(g, i, &sx, &sy)) continue;
		auto many = [&](int type, int count) {
			for (int k = 0; k < count; k++) place_card_unit(g, i, type, sx, sy);
		};
		for (const Card& c : CL.selected) {
			switch (c.type) {
			case kElephantArmy: many(current_upgrade(g, i, 261), K(*rules, "ctw_num_bonus_elephants")); break;  // War Elephant
			case kParmenio:
				many(360, 1);
				many(231, K(*rules, "ctw_parmenio_cav"));  // Companions
				break;
			case kAntipater: many(361, 1); break;
			case kPtolemy: many(362, 1); break;
			case kMemnon: many(364, 1); break;
			case kSpitamenes:
				many(365, 1);
				many(get_graft(g, i, current_upgrade(g, i, 187)), K(*rules, "ctw_spitamenes_archers"));  // Horse Archers
				break;
			case kPorus: many(367, 1); break;
			case kSpyAcademy: many(current_upgrade(g, i, 58), K(*rules, "ctw_num_bonus_spies")); break;
			case kPopulationBoom: many(50, K(*rules, "ctw_num_bonus_citizens")); break;  // Citizens
			case kFouche: many(371, 1); break;
			case kSchwarzenberg:
				many(372, 1);
				many(73, K(*rules, "schwarzenberg_commandos"));  // Commandos
				break;
			case kPaoli: many(375, 1); break;
			default: break;
			}
		}
	}
	(void)self;
}

// ---------------------------------------------------------------- Skill Bonus: the style's bonus script

int ConquestGame::run_bonus_script(Game& battle, const StyleDef& st, std::string* text) {
	if (st.bonus.empty() || !battle.scripts) return -1;
	ScriptHost& h = *battle.scripts;
	if (!h.ctw) h.ctw = this;
	std::string err;
	if (!h.compile(st.bonus, &err)) {
		log.push_back("bonus script " + st.bonus + ": " + err);
		return -1;
	}
	std::string base = st.bonus;
	for (char& ch : base) {
		if (ch == '\\') ch = '/';
	}
	const size_t slash = base.find_last_of('/');
	if (slash != std::string::npos) base = base.substr(slash + 1);
	const size_t dot = base.find_last_of('.');
	if (dot != std::string::npos) base = base.substr(0, dot);
	bhs::VP arg = bhs::make_string(bhs::to_u16(""), bhs::SC_VAR);
	const int st_run = h.run(bhs::to_u16(base), {arg});
	if (getenv("CTW_DEBUG")) std::fprintf(stderr, "bonus script %s (%s): status %d result %d\n", st.bonus.c_str(), base.c_str(), st_run, st_run == 0 && h.rt.return_value ? int(bhs::value_int(h.rt.return_value.get())) : -1);
	if (st_run != 0) {
		if (getenv("CTW_DEBUG")) {
			for (const std::string& m : h.messages) std::fprintf(stderr, "  %s\n", m.c_str());
			for (const auto& u : h.unported()) std::fprintf(stderr, "  unported %s x%d\n", u.first.c_str(), u.second);
		}
		return -1;
	}
	if (text) *text = bhs::to_utf8(bhs::value_string(arg.get()));
	const bhs::VP& rv = h.rt.return_value;
	return rv ? int(bhs::value_int(rv.get())) : 0;
}

}  // namespace ron::sim::ctw
