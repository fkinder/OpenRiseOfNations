// Conquer the World: the campaign save (docs/ctw-spec.md 7). The original's `RonCTWMapSave` container (version 0x10)
// holds ConquestGame::walk_data; this port writes the same state through the engine's native save archive
// (sim/save_archive.hpp, docs/save-format.md section 9): a header (the magic string and the version), then tagged,
// CRC-checked sections, every struct walked once for both directions. Original-format compatibility is not a goal
// (the spec's layout is in state-walk.md). What is saved: the campaign file's name (the data files are re-read on
// load), the scalars including the campaign RNG's state, the leaders, nodes, colonies, armies, the deck and pool,
// news, the AI round in progress, script storage (stored_ints / stored_strs / deals) and the style play counts.
#include <algorithm>
#include <fstream>

#include "sim/ctw.hpp"
#include "sim/save_archive.hpp"

namespace ron::sim::save {

using namespace ron::sim::ctw;

template <class Ar> void walk(Ar& ar, Card& v) { FIELDS(ar, v.type, v.variant); }
template <class Ar> void walk(Ar& ar, Piece& v) { FIELDS(ar, v.node, v.moved); }
template <class Ar> void walk(Ar& ar, News& v) { FIELDS(ar, v.round, v.text); }
template <class Ar> void walk(Ar& ar, Overrun& v) { FIELDS(ar, v.node, v.slot); }
template <class Ar> void walk(Ar& ar, Reinforcement& v) { FIELDS(ar, v.time, v.side, v.node, v.dest_x, v.dest_y, v.edge); }
template <class Ar> void walk(Ar& ar, DiploOffer& v) {
	FIELDS(ar, v.tribe, v.tribe2, v.offer_type, v.response, v.id, v.accepted, v.counter_offer, v.tribe_trib, v.tribe2_trib,
	       v.disable_counter_offer, v.tribe_terr, v.tribe2_terr, v.tribe_col, v.tribe2_col, v.tribe_cards, v.tribe2_cards);
}
template <class Ar> void walk(Ar& ar, ctw::Node& v) {
	FIELDS(ar, v.owner, v.founder, v.level, v.turns_held, v.wonder, v.rare, v.tribute, v.flags, v.value, v.nukes, v.tech_cat,
	       v.continent, v.tileset, v.card, v.display_name, v.links, v.link_sea, v.take_tribute);
}
template <class Ar> void walk(Ar& ar, ctw::Colony& v) { FIELDS(ar, v.id, v.name, v.rares, v.wonder, v.value, v.tribute, v.owner); }
template <class Ar> void walk(Ar& ar, ctw::Leader& v) {
	FIELDS(ar, v.capital, v.capital2, v.nodes_controlled, v.supply_depots, v.tribute, v.strength, v.attack_link, v.inherent_skill,
	       v.num_cards_purchased, v.vassal_of, v.colony_blockade, v.base_armies, v.eliminated_by, v.base_rate, v.stances,
	       v.stance_time, v.attitude_base, v.no_attack_time, v.bribe_paid, v.nodes_won_from, v.bonus_card_types_played, v.active,
	       v.valid, v.no_eliminating, v.no_propose_ally, v.no_negotiation, v.reject_bribe, v.gunpowder_off, v.rares_off, v.name,
	       v.rare_resources, v.wonders, v.continents, v.new_nodes, v.no_attack, v.no_declare_war, v.cards, v.selected, v.fought_mask,
	       v.met_mask);
}

}  // namespace ron::sim::save

namespace ron::sim::ctw {

namespace {

constexpr uint32_t kSaveVersion = 0x10;  // the original's RonCTWMapSave version

template <class Ar>
void walk_all(Ar& ar, ConquestGame& g, std::string& campaign, std::vector<int>& style_plays, std::vector<uint8_t>& statics) {
	using namespace ron::sim::save;
	std::string magic = "RonCTWMapSave";
	uint32_t version = kSaveVersion;
	std::string m2 = magic;
	io(ar, m2);
	if (Ar::kReading && m2 != magic) ar.error("not a Conquer the World save");
	io(ar, version);
	if (Ar::kReading && version != kSaveVersion) ar.error("unsupported campaign save version " + std::to_string(version));
	ar.section("CTWG", [&](auto& s) {
		uint32_t rng = g.random.seed();
		FIELDS(s, campaign, g.seed, rng, g.player_tribe, g.difficulty, g.cur_round, g.num_turns, g.turns_skipped, g.cur_age,
		       g.starting_age, g.ending_age, g.scen_per_age, g.turns_no_attack, g.game_end, g.return_to_node, g.defcon_level,
		       g.nuke_launch_turns, g.talleyrand_tribe, g.talleyrand_state, g.fouche_tribe, g.style_index1, g.style_index2,
		       g.cur_espionage_level, g.random_nations, g.random_bonuses, g.has_fought_barbarian, g.free_move_turn,
		       g.need_defensive_bonus, g.piece_moves_disabled, g.attacked_this_round, g.ai_phase, g.ai_index, g.ai_in_progress,
		       g.map_name, g.map_id, g.player_name, g.end_title, g.end_text, g.end_image, g.end_button, g.no_negotiate_text,
		       g.end_victory, g.last_battle_victory, g.nuke_building_disabled, g.supply_depots_hidden, g.espionage_disabled);
		if (Ar::kReading) g.random.reseed(rng);
	});
	ar.section("CTWL", [&](auto& s) { io(s, g.leaders); });
	ar.section("CTWN", [&](auto& s) { io(s, g.nodes); });
	ar.section("CTWC", [&](auto& s) { FIELDS(s, g.colonies, g.continents_captured); });
	ar.section("CTWP", [&](auto& s) { FIELDS(s, g.pieces, g.reinforcements, g.overrun_armies, g.valid_tribes); });
	ar.section("CTWD", [&](auto& s) { FIELDS(s, g.deck, g.pool); });
	ar.section("CTWV", [&](auto& s) { io(s, statics); });
	ar.section("CTWS", [&](auto& s) {
		FIELDS(s, g.stored_ints, g.stored_strs, g.deals, g.sub_goals, g.allied_punks, g.news, style_plays, g.proposals);
	});
}

}  // namespace

std::vector<uint8_t> ConquestGame::save_bytes() const {
	save::Writer w;
	ConquestGame& g = const_cast<ConquestGame&>(*this);
	std::string campaign = def ? def->file : std::string();
	std::vector<int> plays;
	for (const StyleDef& s : style_table) plays.push_back(s.times_played);
	std::vector<uint8_t> statics = g.script_statics();
	walk_all(w, g, campaign, plays, statics);
	return std::move(w.buf);
}

std::string ConquestGame::load_bytes(const GameFiles& f, const sim::Rules& r, const std::vector<uint8_t>& bytes) {
	try {
		// the campaign name comes first: read it from a scratch state, then attach the data and walk into this one
		ConquestGame probe;
		std::string campaign;
		std::vector<int> plays;
		std::vector<uint8_t> statics;
		{
			save::Reader rd(bytes.data(), bytes.size(), "campaign save");
			walk_all(rd, probe, campaign, plays, statics);
		}
		std::string err = attach(f, r, campaign);
		if (!err.empty()) return err;
		scripts_drop();
		// copy the walked state
		save::Reader rd(bytes.data(), bytes.size(), "campaign save");
		std::string c2;
		std::vector<int> plays2;
		std::vector<uint8_t> statics2;
		walk_all(rd, *this, c2, plays2, statics2);
		style_table = def->styles.empty() ? crules->styles : def->styles;
		for (size_t i = 0; i < style_table.size(); i++) style_table[i].times_played = i < plays2.size() ? plays2[i] : 0;
		if (nodes.size() != def->countries.size()) return "the save does not match the campaign files (territories)";
		available_links.clear();
		script_links.clear();
		cur_link = LinkChoice();
		restore_script_statics(statics2);
		available_links = find_available_links(player_tribe);
	} catch (const std::exception& e) {
		return e.what();
	}
	return std::string();
}

bool ConquestGame::save_file(const std::string& path, std::string* err) const {
	const std::vector<uint8_t> b = save_bytes();
	std::ofstream o(path, std::ios::binary);
	if (!o) {
		if (err) *err = "cannot write " + path;
		return false;
	}
	o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
	return bool(o);
}

std::string ConquestGame::load_file(const GameFiles& f, const sim::Rules& r, const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return "cannot read " + path;
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return load_bytes(f, r, b);
}

std::string ConquestGame::save_name() const { return map_name + " - " + tribe_name(player_tribe); }

}  // namespace ron::sim::ctw
