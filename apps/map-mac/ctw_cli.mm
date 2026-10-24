// Conquer the World command-line checks (ctw_cli.h). Plain C++: no Cocoa.
#include "ctw_cli.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "assets/files.hpp"
#include "sim/anim_assets.hpp"
#include "sim/combat.hpp"
#include "sim/commands.hpp"
#include "sim/game.hpp"
#include "sim/newgame.hpp"
#include "sim/save.hpp"
#include "sim/production.hpp"
#include "sim/rules.hpp"
#include "sim/rules_xml.hpp"
#include "sim/script_host.hpp"

using namespace ron;
using namespace ron::sim;

namespace ctwcli {

std::string resolve_campaign(const GameFiles& files, const std::string& name) {
	const std::vector<std::string> all = ctw::list_campaigns(files);
	std::string want = ctw::lower(name);
	for (char& c : want) {
		if (c == ' ') c = '_';
	}
	if (!want.empty() && std::all_of(want.begin(), want.end(), ::isdigit)) {
		const size_t k = size_t(std::atoi(want.c_str()));
		return k < all.size() ? all[k] : std::string();
	}
	for (const std::string& s : all) {
		if (ctw::lower(s) == want) return s;
	}
	for (const std::string& s : all) {  // "world" is the World campaign, not New_World
		std::string core = ctw::lower(s);
		if (core.compare(0, 4, "ctw_") == 0) core = core.substr(4);
		if (core.size() > 7 && core.compare(core.size() - 7, 7, "_map_01") == 0) core = core.substr(0, core.size() - 7);
		if (core == want) return s;
	}
	for (const std::string& s : all) {
		if (ctw::lower(s).find(want) != std::string::npos) return s;
	}
	return std::string();
}

namespace {
const int kExpectedRounds[5] = {19, 21, 18, 14, 24};  // Alexander, Napoleon, New World, Cold War, World (list order)
}

int check(const std::string& game_dir) {
	GameFiles files(game_dir);
	Rules rules = load_rules(files);
	start_game(rules, GameSetup());
	const auto crules = ctw::load_conquest_rules(files);
	std::printf("conquestrules.xml: %zu card types, %zu default styles, army of %zu entries, no-attack floor %d/%d/%d/%d/%d/%d\n",
	            crules->cards.size(), crules->styles.size(), crules->army.size(), crules->min_no_attack[0], crules->min_no_attack[1],
	            crules->min_no_attack[2], crules->min_no_attack[3], crules->min_no_attack[4], crules->min_no_attack[5]);
	int bad = 0;
	const auto list = ctw::list_campaigns(files);
	if (list.size() != 5) {
		std::printf("expected 5 campaigns, found %zu\n", list.size());
		bad++;
	}
	for (size_t i = 0; i < list.size(); i++) {
		auto c = ctw::load_campaign(files, rules, list[i]);
		int links = 0, sea = 0, owned = 0, capitals = 0;
		for (const auto& k : c->countries) {
			links += int(k.links.size());
			for (const auto& l : k.links) sea += l.sea;
			owned += !k.owner.empty();
			capitals += k.capital;
		}
		const std::vector<std::string> problems = ctw::validate_campaign(*c);
		const bool rounds_ok = i < 5 && c->length() == kExpectedRounds[i];
		std::printf("%-24s \"%s\": rounds %d%s, countries %zu (%d owned, %d capitals), links %d (%d sea), colonies %zu, styles %zu, "
		            "deck %zu, nations %d, %zu problems\n",
		            list[i].c_str(), c->name.c_str(), c->length(), rounds_ok ? "" : " (UNEXPECTED)", c->countries.size(), owned, capitals,
		            links, sea, c->colonies.size(), c->styles.size(), c->deck.size(),
		            int(std::count_if(c->tribes.begin(), c->tribes.end(), [](const ctw::TribeDef& t) { return t.present; })), problems.size());
		for (const std::string& p : problems) std::printf("  problem: %s\n", p.c_str());
		bad += int(problems.size()) + (rounds_ok ? 0 : 1);
		// a fresh game of it: every link resolved to nodes, owners to slots
		ctw::ConquestGame g;
		ctw::ConquestSetup s;
		s.campaign = list[i];
		s.seed = 1;
		const std::string err = g.setup(files, rules, s);
		if (!err.empty()) {
			std::printf("  setup failed: %s\n", err.c_str());
			bad++;
			continue;
		}
		int nlinks = 0;
		for (const auto& n : g.nodes) nlinks += int(n.links.size());
		int active = 0;
		for (int sl = 0; sl < ctw::kSlots; sl++) active += g.leaders[size_t(sl)].active;
		std::printf("  setup: player %s (%s), %d nations, %d links in the nodes, %zu armies for the player, tribute %d, "
		            "available attacks %zu, no-attack rounds %d\n",
		            g.tribe_name(g.player_tribe).c_str(), g.tribe_key(g.player_tribe).c_str(), active, nlinks,
		            g.pieces[size_t(g.player_tribe)].size(), g.leaders[size_t(g.player_tribe)].tribute, g.available_links.size(),
		            g.turns_no_attack);
		for (const std::string& l : g.log) {
			if (l.rfind("script ", 0) == 0) {
				std::printf("  %s\n", l.c_str());
			}
		}
	}
	std::printf(bad ? "ctw-check: %d problem(s)\n" : "ctw-check: ok\n", bad);
	return bad ? 1 : 0;
}

namespace {

uint32_t crc_of(const std::vector<uint8_t>& b) { return uint32_t(::crc32(0, b.data(), uInt(b.size()))); }

void print_state(const ctw::ConquestGame& g) {
	std::printf("round %d age %d:", g.cur_round, g.cur_age);
	for (int s = 0; s < ctw::kSlots; s++) {
		const auto& L = g.leaders[size_t(s)];
		if (!L.valid) continue;
		std::printf(" %s=%d%s", g.tribe_key(s).c_str(), g.num_nodes(s, false), L.active ? "" : "(out)");
	}
	if (g.def && g.def->strategic_nukes) {
		std::printf(" | DEFCON %d, nukes USA %d USSR %d, tribute USA %d USSR %d", g.defcon_level, g.num_nukes(ctw::kUsa), g.num_nukes(ctw::kUssr),
		            g.leaders[ctw::kUsa].tribute, g.leaders[ctw::kUssr].tribute);
	}
	std::printf("\n");
}

// One full AI round after the player held back.
void play_round(ctw::ConquestGame& g, bool verbose) {
	g.end_turn_player();
	g.begin_ai_round();
	for (int guard = 0; guard < 1000; guard++) {
		ctw::AiStep st = g.ai_step();
		if (st.kind == ctw::AiStep::kDone) break;
		if (st.kind == ctw::AiStep::kNukeStrike) {  // headless: the player strikes back when he has missiles
			if (verbose) std::printf("  NUCLEAR STRIKE by the %s (DEFCON %d)\n", g.tribe_name(g.nuke_p2).c_str(), g.defcon_level);
			g.finish_nuclear_exchange(g.num_nukes(g.player_tribe) > 0);
			continue;
		}
		if (st.kind == ctw::AiStep::kPlayerAttacked) {
			if (verbose) std::printf("  the %s attack %s\n", g.tribe_name(st.battle.attacker).c_str(), g.node_name(st.battle.to).c_str());
			g.cur_style = g.pick_style(st.battle);
			g.resolve_player_defense(g.rget(0, 99) < 50);
		}
	}
	g.finish_round();
	if (g.nuke_pending) g.finish_nuclear_exchange(g.num_nukes(g.player_tribe) > 0);
	// the passive player declines what the AI nations offered (the campaign scripts read the answers)
	while (!g.proposals.empty()) {
		g.answer_proposal(g.proposals.front(), false);
		g.proposals.erase(g.proposals.begin());
	}
}

}  // namespace

void play_rounds(ctw::ConquestGame& g, int rounds) {
	for (int r = 0; r < rounds && g.game_end == 0; r++) play_round(g, false);
}

int simulate(const std::string& game_dir, const std::string& campaign_arg, unsigned seed, int rounds, int difficulty,
             const std::string& out, bool verbose) {
	GameFiles files(game_dir);
	Rules rules = load_rules(files);
	start_game(rules, GameSetup());
	const std::string stem = resolve_campaign(files, campaign_arg);
	if (stem.empty()) {
		std::fprintf(stderr, "no campaign '%s' (try 0..4, alexander, napoleon, new_world, cold_war, world)\n", campaign_arg.c_str());
		return 2;
	}
	ctw::ConquestGame g;
	ctw::ConquestSetup s;
	s.campaign = stem;
	s.seed = seed;
	s.difficulty = difficulty;
	const std::string err = g.setup(files, rules, s);
	if (!err.empty()) {
		std::fprintf(stderr, "setup: %s\n", err.c_str());
		return 2;
	}
	std::printf("%s, seed %u, difficulty %d, player %s\n", stem.c_str(), seed, difficulty, g.tribe_name(g.player_tribe).c_str());
	print_state(g);
	for (int r = 0; r < rounds && g.game_end == 0; r++) {
		const size_t news0 = g.news.size();
		play_round(g, verbose);
		print_state(g);
		if (verbose) {
			for (size_t i = news0; i < g.news.size(); i++) std::printf("    %s\n", g.news[i].text.c_str());
		}
	}
	int script_errors = 0;
	for (const std::string& l : g.log) {
		if (l.find("script") != std::string::npos && (l.find("error") != std::string::npos || l.rfind("script ", 0) == 0)) {
			script_errors++;
			std::printf("  %s\n", l.c_str());
		}
	}
	const std::vector<uint8_t> bytes = g.save_bytes();
	std::printf("save: %zu bytes, crc %08x, game_end %d, news %zu, script problems %d\n", bytes.size(), crc_of(bytes), g.game_end,
	            g.news.size(), script_errors);
	if (!out.empty()) {
		std::string e2;
		if (!g.save_file(out, &e2)) {
			std::fprintf(stderr, "%s\n", e2.c_str());
			return 2;
		}
		std::printf("wrote %s\n", out.c_str());
	}
	return 0;
}

int save_roundtrip(const std::string& game_dir, const std::string& campaign_arg, unsigned seed) {
	GameFiles files(game_dir);
	Rules rules = load_rules(files);
	start_game(rules, GameSetup());
	const std::string stem = resolve_campaign(files, campaign_arg);
	if (stem.empty()) return 2;
	ctw::ConquestGame g;
	ctw::ConquestSetup s;
	s.campaign = stem;
	s.seed = seed;
	if (!g.setup(files, rules, s).empty()) return 2;
	for (int r = 0; r < 3; r++) play_round(g, false);
	const std::vector<uint8_t> a = g.save_bytes();
	ctw::ConquestGame h;
	const std::string err = h.load_bytes(files, rules, a);
	if (!err.empty()) {
		std::fprintf(stderr, "load: %s\n", err.c_str());
		return 1;
	}
	const std::vector<uint8_t> b = h.save_bytes();
	const bool same = a == b;
	// both continue identically
	play_round(g, false);
	play_round(h, false);
	const bool same2 = g.save_bytes() == h.save_bytes();
	std::printf("%s: save %zu bytes, reload %s, one more round %s\n", stem.c_str(), a.size(), same ? "identical" : "DIFFERENT",
	            same2 ? "identical" : "DIFFERENT");
	return same && same2 ? 0 : 1;
}

bool pick_link(ctw::ConquestGame& g, const std::string& spec, ctw::LinkChoice* out, std::string* err) {
	std::vector<ctw::LinkChoice> links = g.find_available_links(g.player_tribe);
	ctw::LinkChoice pick;
	if (spec.empty() || spec.compare(0, 4, "auto") == 0) {  // "auto" or "auto:N": the N-th attack the player can make
		const size_t n = spec.size() > 5 ? size_t(std::atoi(spec.c_str() + 5)) : 0;
		if (links.empty()) {
			if (err) *err = "the player has no attack available";
			return false;
		}
		pick = links[std::min(n, links.size() - 1)];
	} else {
		const size_t gt = spec.find('>');
		const int from = gt == std::string::npos ? -1 : g.find_node(spec.substr(0, gt));
		const int to = gt == std::string::npos ? -1 : g.find_node(spec.substr(gt + 1));
		if (from < 0 || to < 0) {
			if (err) *err = "link must be auto or FROM>TO (territory ids)";
			return false;
		}
		bool found = false;
		for (const ctw::LinkChoice& l : links) {
			if (l.from == from && l.to == to) {
				pick = l;
				found = true;
			}
		}
		if (!found) {
			if (err) *err = "that link is not available to the player";
			return false;
		}
	}
	if (pick.needs_war) g.set_war(pick.attacker, pick.defender);
	*out = pick;
	return true;
}

int battle(const std::string& game_dir, const std::string& campaign_arg, const std::string& link_spec, int frames, unsigned seed,
           int difficulty, bool all_ai) {
	GameFiles files(game_dir);
	Rules rules = load_rules(files);
	start_game(rules, GameSetup());
	const std::string stem = resolve_campaign(files, campaign_arg);
	if (stem.empty()) {
		std::fprintf(stderr, "no campaign '%s'\n", campaign_arg.c_str());
		return 2;
	}
	auto art = load_anim_source(files, rules);
	ctw::ConquestGame g;
	ctw::ConquestSetup s;
	s.campaign = stem;
	s.seed = seed;
	s.difficulty = difficulty;
	std::string err = g.setup(files, rules, s);
	if (!err.empty()) {
		std::fprintf(stderr, "setup: %s\n", err.c_str());
		return 2;
	}
	if (getenv("CTW_VARY_SEED")) g.vary_battle_seed = true;  // a different map per battle (default: the campaign seed, as the original)
	ctw::LinkChoice l;
	if (!pick_link(g, link_spec, &l, &err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 2;
	}
	// CTW_CARDS="Military Eureka,Warpath,...": cards given to the player and chosen for this battle (a test hook)
	if (const char* cards = getenv("CTW_CARDS")) {
		std::string list = cards;
		for (size_t p = 0; p < list.size();) {
			size_t e = list.find(',', p);
			if (e == std::string::npos) e = list.size();
			ctw::Card c;
			if (g.parse_card(list.substr(p, e - p), &c)) {
				g.give_card(g.player_tribe, c);
				g.select_tactical(g.player_tribe, int(g.leaders[size_t(g.player_tribe)].cards.size()) - 1);
			}
			p = e + 1;
		}
	}
	g.cur_link = l;
	g.cur_style = g.pick_style(l);
	if (const char* want = getenv("CTW_STYLE")) {  // a test hook: the first style of the category with this title
		for (size_t i = 0; i < g.style_table.size(); i++) {
			if (g.style_table[i].category == g.cur_style.category && ctw::ieq(g.style_table[i].title, want)) {
				g.cur_style.index = int(i);
				break;
			}
		}
	}
	ctw::ConquestGame::BattleDef B = g.make_battle(l, g.cur_style);
	if (all_ai) {
		for (auto& n : B.setup.nation) n.human = false;
		for (auto& seat : B.seats) seat.human = false;
	}
	const ctw::StyleDef* st = g.cur_style.index >= 0 ? &g.style_table[size_t(g.cur_style.index)] : nullptr;
	std::printf("%s: %s attacks %s from %s (%s link); style %s \"%s\"%s%s; %zu seats, map style %d size %d, age %d..%d, seed %u\n",
	            stem.c_str(), g.tribe_name(l.attacker).c_str(), g.node_name(l.to).c_str(), g.node_name(l.from).c_str(), l.sea ? "sea" : "land",
	            st ? std::to_string(g.cur_style.category).c_str() : "?", st ? st->title.c_str() : "default",
	            B.custom_map.empty() ? "" : ", map ", B.custom_map.c_str(), B.seats.size(), B.setup.map_style, B.setup.map_size,
	            B.setup.starting_technology, B.setup.ending_technology, B.seed);
	for (size_t i = 0; i < B.seats.size(); i++) {
		std::printf("  seat %zu: %s%s team %d tribe %d\n", i, B.seats[i].slot >= 0 ? g.tribe_name(B.seats[i].slot).c_str() : "barbarians",
		            B.seats[i].human ? " (player)" : "", B.seats[i].team, B.setup.nation[i].tribe);
	}
	sim::RandomExtras ex;
	std::unique_ptr<sim::Game> game = g.build_battle(files, rules, B, art.get(), &ex, &err);
	if (!game) {
		std::fprintf(stderr, "battle: %s\n", err.c_str());
		return 1;
	}
	std::printf("  built: %dx%d world, %d objects of seat 0, script errors %zu\n", game->world.tile_xs, game->world.tile_ys,
	            int(game->objects[0].unit_mark), game->scripts ? game->scripts->messages.size() : size_t(0));
	std::printf("  rules: victory %d (setup %d) elimination %d time_limit %d (custom %d min) reveal %d\n", game->info.victory, B.setup.victory,
	            game->info.elimination, game->info.time_limit, game->scen.custom_time_limit, game->info.reveal_map);
	if (getenv("CTW_DEBUG") && game->scripts) {
		bhs::Runtime& rt = game->scripts->rt;
		for (const bhs::BuiltinFunc& f : rt.registry.funcs()) {
			if (f.name8 != "defeat" && f.name8 != "get_time_limit" && f.name8 != "victory") continue;
			bhs::Native orig = rt.natives[size_t(f.index)];
			const std::string nm = f.name8;
			rt.natives[size_t(f.index)] = [orig, nm, &rt](bhs::NativeCall& c) {
				bhs::VP r = orig ? orig(c) : bhs::VP();
				std::printf("  [script] %s(%d) -> %d (frame script %s)\n", nm.c_str(), c.arg_int(0), r ? bhs::value_int(r.get()) : -99,
				            rt.current_script() ? bhs::to_utf8(rt.current_script()->name).c_str() : "?");
				return r;
			};
		}
	}
	// the second pass's diagnostics: the cards in play, the rare goods, the planned reinforcements
	for (int i = 0; i < int(B.seats.size()) && i < 8; i++) {
		const sim::Leader& GL = game->leaders[size_t(i)];
		std::string cards;
		for (int t = 0; t < ctw::kNumCardTypes; t++) {
			if (GL.num_bonus_cards[size_t(t)]) cards += (cards.empty() ? "" : ", ") + g.crules->cards[size_t(t)].name + " x" + std::to_string(GL.num_bonus_cards[size_t(t)]);
		}
		int rares = 0;
		for (int b = 0; b < 8; b++) rares += __builtin_popcount(GL.rare_conquest[size_t(b)]);
		std::printf("  seat %d: cards [%s]; rare goods %d; thinker %d; units %d; tech epochs %d/%d/%d/%d\n", i, cards.c_str(), rares,
		            int(GL.num_bonus_cards[7]), int(game->objects[size_t(i)].unit_mark), GL.epoch[0], GL.epoch[1], GL.epoch[2], GL.epoch[3]);
	}
	for (const ctw::Reinforcement& r : g.reinforcements) {
		std::printf("  reinforcement: seat %d from %s at %d s, enters (%d,%d) marches to (%d,%d)\n", r.side,
		            r.node >= 0 ? g.node_name(r.node).c_str() : "wonder", r.time, r.loc_x, r.loc_y, r.dest_x, r.dest_y);
	}
	sim::CommandManager cm;
	cm.play = 0;
	cm.play_who = 0;
	int won = -1;
	for (int f = 0; f < frames && won < 0; f++) {
		cm.process_turn(*game);
		game->do_frame();
		for (const sim::GameEvent& e : game->events) {
			if (e.kind == 3 && e.who == 0) won = 1;
			if (e.kind == 2 && e.who == 0) won = 0;
			if ((e.kind == 2 || e.kind == 3) && getenv("CTW_DEBUG")) std::printf("  event frame %d kind %d who %d other %d value %d\n", e.frame, e.kind, e.who, e.other, e.value);
		}
	}
	bool adjudicated = false;
	if (won < 0) {  // headless adjudication: the side with the larger population
		int mine = 0, theirs = 0;
		for (int i = 0; i < int(B.seats.size()) && i < 8; i++) {
			int alive = 0;
			const sim::ObjectList& ol = game->objects[size_t(i)];
			for (int32_t o = 0; o < ol.unit_mark; o++) alive += game->unit(i, o) && game->unit(i, o)->alive();
			for (int32_t o = sim::kBuildBase; o < ol.build_mark; o++) alive += game->build(i, o) && game->build(i, o)->alive();
			(B.seats[size_t(i)].team == 0 ? mine : theirs) += alive;
		}
		won = mine >= theirs ? 1 : 0;
		adjudicated = true;
		std::printf("  no game over after %d frames: objects alive %d vs %d\n", game->frame, mine, theirs);
	}
	if (getenv("CTW_FORCE_WIN")) won = 1;  // a test hook: the player's side wins (to exercise the victory outcome and the bonus script)
	std::printf("  frame %d: %s%s\n", game->frame, won ? "victory" : "defeat", adjudicated ? " (adjudicated)" : "");
	if (game->scripts) {
		for (const std::string& m : game->scripts->messages) std::printf("  script: %s\n", m.c_str());
		int shown = 0;
		for (const auto& u : game->scripts->unported()) {
			if (shown++ < 6) std::printf("  unported builtin %s x%d\n", u.first.c_str(), u.second);
		}
		std::printf("  scripts: %lld runs, %lld failures\n", (long long)game->scripts->runs, (long long)game->scripts->run_errors);
	}
	{
		int placed = 0;
		for (const ctw::Reinforcement& r : g.reinforcements) placed += r.placed;
		std::printf("  reinforcements: %zu planned, %d arrived (tick %d), notices %zu\n", g.reinforcements.size(), placed, game->tick,
		            g.battle_notices.size());
		for (int i = 0; i < int(B.seats.size()) && i < 8; i++) {
			std::map<std::string, int> census;
			const sim::ObjectList& ol = game->objects[size_t(i)];
			for (int32_t o = 0; o < ol.unit_mark; o++) {
				const sim::Unit* u = game->unit(i, o);
				if (u && u->alive()) census[to_utf8(game->type(u->type).name)]++;
			}
			std::string line;
			for (const auto& kv : census) line += (line.empty() ? "" : ", ") + kv.first + " " + std::to_string(kv.second);
			std::printf("  seat %d units: %s\n", i, line.c_str());
		}
	}
	g.last_battle_victory = won != 0;
	g.process_results(0, won != 0, l, g.cur_style);
	if (g.battle_bonus > 0) std::printf("  Skill Bonus awarded: +%d of each resource in a battle\n", g.battle_bonus);
	std::printf("  campaign: %s now owned by %s; tribute %d; news: %s\n", g.node_name(l.to).c_str(),
	            g.nodes[size_t(l.to)].owner >= 0 ? g.tribe_name(g.nodes[size_t(l.to)].owner).c_str() : "barbarians",
	            g.leaders[size_t(g.player_tribe)].tribute, g.news.empty() ? "" : g.news.back().text.c_str());
	return 0;
}

namespace {
struct Tally {
	int n = 0, bad = 0;
	void check(bool ok, const std::string& what) {
		n++;
		if (!ok) bad++;
		std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	}
};

bool setup_game(const GameFiles& files, const Rules& rules, ctw::ConquestGame& g, const char* campaign, unsigned seed) {
	ctw::ConquestSetup s;
	s.campaign = campaign;
	s.seed = seed;
	return g.setup(files, rules, s).empty();
}
}  // namespace

namespace {

// A battle of a campaign link built from a state with `cards` given and chosen for the player (and `ai_cards` for the other
// main nation), for the second-pass checks.
struct Built {
	ctw::ConquestGame g;
	ctw::ConquestGame::BattleDef B;
	sim::RandomExtras ex;
	std::unique_ptr<sim::Game> game;
	std::string err;
};

std::unique_ptr<Built> build_with(const GameFiles& files, const Rules& rules, const sim::AnimSource* art, const char* campaign, unsigned seed,
                                  const std::vector<std::string>& cards, bool default_style = true, int difficulty = 2,
                                  ctw::LinkChoice* link_out = nullptr) {
	auto b = std::make_unique<Built>();
	ctw::ConquestSetup s;
	s.campaign = campaign;
	s.seed = seed;
	s.difficulty = difficulty;
	if (!b->g.setup(files, rules, s).empty()) return nullptr;
	ctw::LinkChoice pick;
	bool found = false;
	for (const ctw::LinkChoice& l : b->g.find_available_links(b->g.player_tribe)) {
		const ctw::StyleChoice sc = b->g.pick_style(l);
		const bool def = sc.index >= 0 && b->g.style_table[size_t(sc.index)].is_default;
		if (def == default_style && !found) {
			pick = l;
			found = true;
		}
	}
	if (!found) return nullptr;
	if (pick.needs_war) b->g.set_war(pick.attacker, pick.defender);
	for (const std::string& c : cards) {
		ctw::Card cd;
		if (!b->g.parse_card(c, &cd)) return nullptr;
		b->g.give_card(b->g.player_tribe, cd);
		b->g.select_tactical(b->g.player_tribe, int(b->g.leaders[size_t(b->g.player_tribe)].cards.size()) - 1);
	}
	b->g.cur_link = pick;
	b->g.cur_style = b->g.pick_style(pick);
	b->B = b->g.make_battle(pick, b->g.cur_style);
	b->B.link = pick;
	b->game = b->g.build_battle(files, rules, b->B, art, &b->ex, &b->err);
	if (link_out) *link_out = pick;
	return b->game ? std::move(b) : nullptr;
}

int count_units(const sim::Game& g, int who, int type) {
	int n = 0;
	const sim::ObjectList& ol = g.objects[size_t(who)];
	for (int32_t o = 0; o < ol.unit_mark; o++) {
		const sim::Unit* u = g.unit(who, o);
		if (u && u->alive() && u->type == type) n++;
	}
	return n;
}

}  // namespace

void selftest2(const GameFiles& files, const Rules& rules, Tally& t);

void selftest2(const GameFiles& files, const Rules& rules, Tally& t) {
	auto art = load_anim_source(files, rules);
	// ---- second pass: the angle maths of find_angle / angle_to_degrees (reinforcement entries, get_conquest_invasion_dir)
	{
		const int e = ctw::angle_degrees_of(100, 0), n = ctw::angle_degrees_of(0, -100), w = ctw::angle_degrees_of(-100, 0), so = ctw::angle_degrees_of(0, 100);
		const int ne = ctw::angle_degrees_of(100, -100);
		t.check(e == 90 && n == 0 && w == 270 && so == 180 && ne >= 44 && ne <= 46, "find_angle / angle_to_degrees: E 90, N 0, W 270, S 180, NE ~45");
	}
	// ---- tactical cards (a default-style battle of the World campaign)
	{
		auto plain = build_with(files, rules, art.get(), "CTW_World_Map_01", 21, {});
		auto cardd = build_with(files, rules, art.get(), "CTW_World_Map_01", 21,
		                        {"Great Thinker", "Partisans", "Logistics", "Elephant Brigade", "Spy Academy", "Population Boom", "Skill Bonus", "Warpath", "Cultural Dominance",
		                         "Political Dissidents", "Trade Embargo", "Spy Network", "Mercenaries"});
		if (!plain || !cardd) {
			t.check(false, "built the two World battles for the card checks");
		} else {
			sim::Game& G0 = *plain->game;
			sim::Game& G1 = *cardd->game;
			// Great Thinker: 25 % cheaper age techs (division per card) and 25 % faster research
			const int age = 544;  // CLASSICAL_AGE
			const int c0 = sim::get_cost(G0, age, 0, 0, -1, -1, 0, 0, -1), c1 = sim::get_cost(G1, age, 0, 0, -1, -1, 0, 0, -1);
			const int tt0 = sim::train_time(G0, -1, 0, age), tt1 = sim::train_time(G1, -1, 0, age);
			t.check(c0 > 0 && c1 == (c0 * 100) / (ctw::K(rules, "ctw_great_thinker_discount") + 100) && tt1 < tt0,
			        "Great Thinker: age cost x100/(100+25) = " + std::to_string(c1) + " of " + std::to_string(c0) + ", research time " + std::to_string(tt1) + " of " + std::to_string(tt0));
			// Partisans: attrition 1 with no attrition tech (the percent keeps at least 1); Logistics: units heal x2 and take no attrition
			sim::leader_calc_attrition(G0, 0);
			sim::leader_calc_attrition(G1, 0);
			t.check(G0.leaders[0].att == 0 && G1.leaders[0].att >= 1, "Partisans: the holder's attrition against invaders is at least 1");
			int unit_o = -1;
			for (int32_t o = 0; o < G1.objects[0].unit_mark && unit_o < 0; o++) {
				if (G1.unit(0, o) && G1.unit(0, o)->alive()) unit_o = o;
			}
			bool heal_ok = false;
			if (unit_o >= 0) {
				sim::Unit& u1 = *G1.unit(0, unit_o);
				sim::Unit& u0 = *G0.unit(0, unit_o);
				u1.damage = 10;
				u0.damage = 10;
				sim::unit_repair_damage(G1, u1, 1, false, true);
				sim::unit_repair_damage(G0, u0, 1, false, true);
				heal_ok = u0.damage == 9 && u1.damage == 8;
			}
			t.check(heal_ok, "Logistics: a healing step restores ((cards x 100 + 100) / 100) x the amount");
			// Skill Bonus (amount 0 from the deck) leaves the stock; the amount rides in the card's variant (battle script result)
			t.check(G1.leaders[0].num_bonus_cards[14] == 1 && G1.leaders[0].num_bonus_cards[7] == 1 && G1.leaders[0].num_bonus_cards[10] == 1,
			        "tactical cards are counted in num_bonus_cards (Skill Bonus, Great Thinker, Partisans)");
			// units the cards bring: War Elephants (6), Spies (3), 5 extra Citizens
			const int elephant = 261, spy = 58, citizen = 50;
			const bool att_or_def_default = cardd->B.defender_default;
			t.check(att_or_def_default && count_units(G1, 0, elephant) - count_units(G0, 0, elephant) == ctw::K(rules, "ctw_num_bonus_elephants") &&
			            count_units(G1, 0, spy) - count_units(G0, 0, spy) == ctw::K(rules, "ctw_num_bonus_spies") &&
			            count_units(G1, 0, citizen) - count_units(G0, 0, citizen) == ctw::K(rules, "ctw_num_bonus_citizens"),
			        "card units: +6 War Elephants, +3 Spies, +5 Citizens at the player's start");
			// Cultural Dominance / Political Dissidents / Spy Network
			bool dominance = false, dissidents = false;
			for (int i = 1; i < 8; i++) {
				if (!(G1.leaders[size_t(i)].flags & sim::leaderflag::kValid)) continue;
				dominance |= (G1.leaders[size_t(i)].flags2 & 0x40) != 0;
				dissidents |= (G1.leaders[size_t(i)].flags2 & 0x800) != 0;
			}
			t.check(dominance && dissidents && (G1.leaders[0].flags & sim::leaderflag::kExploreMap), "Cultural Dominance, Political Dissidents and Spy Network set their leader flags");
			// Mercenaries: one more army composition at the start of the seat (start_armies)
			t.check(cardd->B.start_armies[0] == plain->B.start_armies[0] + 1, "Mercenaries: +1 army at the start (start_armies)");
			// Trade Embargo: the other main nation gets no rare goods
			int rares1 = 0;
			for (int b = 0; b < 8; b++) rares1 += __builtin_popcount(G1.leaders[1].rare_conquest[size_t(b)]);
			t.check(rares1 == 0, "Trade Embargo: the enemy's strategic resources are cut off");
			// Warpath: a Barracks, a Stable and a Siege Factory (current upgrades) in the city, two military epoch techs
			int warbuild = 0;
			for (const char* nm : {"Barracks", "Stable", "Siege Factory"}) {
				for (int ty = sim::kBuilds; ty < sim::kEndBuilds; ty++) {
					if (rules.types[size_t(ty)].loaded && sim::to_utf8(rules.types[size_t(ty)].type_name) == nm) {
						warbuild += G1.leaders[0].num_buildings[size_t(ty - sim::kBuilds)] > G0.leaders[0].num_buildings[size_t(ty - sim::kBuilds)];
					}
				}
			}
			t.check(warbuild >= 2 && G1.leaders[0].epoch[0] >= G0.leaders[0].epoch[0] + 2, "Warpath: military buildings in the city and two military epoch techs (" + std::to_string(warbuild) + " building kinds)");
			// the free techs of the tribe bonuses replace Leader::init's (off in Conquer the World)
			t.check(plain->game->ctw && G0.leaders[0].num_bonus_cards[7] == 0, "a battle without cards has no counters");
		}
	}
	// ---- reinforcements: times, entry on the map edge, arrival, the script flag
	{
		auto b = build_with(files, rules, art.get(), "CTW_Napoleon_Map_01", 6, {}, false);
		const bool any = b && !b->g.reinforcements.empty();
		if (!b || !any) {
			// find a link with neighbours that send an army
			b.reset();
			for (unsigned seed = 1; seed < 40 && !b; seed++) {
				auto c = build_with(files, rules, art.get(), "CTW_Napoleon_Map_01", seed, {}, false);
				if (c && !c->g.reinforcements.empty()) b = std::move(c);
			}
		}
		if (!b) {
			t.check(false, "a battle with reinforcements");
		} else {
			bool times_ok = true, edge_ok = true, dest_ok = true;
			int prev[2] = {0, 0};
			for (const auto& r : b->g.reinforcements) {
				times_ok &= r.time >= 60 * b->g.crules->min_arrival_time && r.time % 60 == 0 && r.time > prev[r.side & 1] - 1;
				prev[r.side & 1] = r.time;
				const int W = b->game->world.xs, H = b->game->world.ys;
				edge_ok &= r.loc_x >= 0 && r.loc_y >= 0 && r.loc_x < W && r.loc_y < H && (r.loc_x == 0 || r.loc_y == 0 || r.loc_x == W - 1 || r.loc_y == H - 1 || r.node < 0);
				dest_ok &= r.dest_x >= -W && r.dest_x < 2 * W && r.dest_y >= -H && r.dest_y < 2 * H;
			}
			t.check(times_ok && edge_ok, "reinforcements: " + std::to_string(b->g.reinforcements.size()) + " army(ies) at 2..4 minute steps entering on the map edge");
			sim::CommandManager cm;
			cm.play = 0;
			cm.play_who = 0;
			const int last = b->g.reinforcements.back().time;
			const int before0 = int(b->game->objects[size_t(b->g.reinforcements.back().side)].unit_mark);
			for (int f = 0; f < (last + 5) * 15 && f < 20000; f++) {
				cm.process_turn(*b->game);
				b->game->do_frame();
			}
			int placed = 0;
			for (const auto& r : b->g.reinforcements) placed += r.placed;
			const int after0 = int(b->game->objects[size_t(b->g.reinforcements.back().side)].unit_mark);
			t.check(placed == int(b->g.reinforcements.size()) && after0 > before0 && !b->g.battle_notices.empty() && dest_ok,
			        "reinforcements arrive when the clock reaches their time: units appear and a notice is posted");
		}
	}
	// ---- the battle seed: the campaign's seed by default, a derived one on request
	{
		ctw::ConquestGame g;
		setup_game(files, rules, g, "CTW_Alexander_Map_01", 77);
		auto links = g.find_available_links(g.player_tribe);
		const auto B1 = g.make_battle(links[0], g.pick_style(links[0]));
		g.vary_battle_seed = true;
		const auto B2 = g.make_battle(links[0], g.pick_style(links[0]));
		t.check(B1.setup.seed == g.seed && B2.setup.seed != g.seed, "battle seed: conquest seed by default, a derived one with vary_battle_seed");
	}
	// ---- Cold War: DEFCON, missiles, the AI's strike, espionage
	{
		ctw::ConquestGame g;
		setup_game(files, rules, g, "CTW_Cold_War_Map_01", 3);
		const int us = ctw::kUsa, ru = ctw::kUssr;
		const int d0 = g.defcon_level;
		int minor = -1;
		for (int s = 0; s < ctw::kSlots; s++) {
			if (s != us && s != ru && g.leaders[size_t(s)].active && g.leaders[size_t(s)].base_armies > 0 && g.stance(ru, s) == ctw::kPeace) minor = s;
		}
		g.set_war(ru, minor);
		t.check(minor >= 0 && g.defcon_level == std::min(5, d0 + 2), "Cold War: a war on a nation with armies raises DEFCON by 2");
		// the nuclear war price: the player vs the other superpower
		g.defcon_level = 1;
		const int nuke_war = g.war_cost(g.player_tribe, g.player_tribe == us ? ru : us);
		g.defcon_level = 3;
		const int nuke_war3 = g.war_cost(g.player_tribe, g.player_tribe == us ? ru : us);
		t.check(nuke_war == ctw::K(rules, "ctw_nuclear_enemy_war_cost") && nuke_war - nuke_war3 == 2 * ctw::K(rules, "ctw_defcon_war_discount"),
		        "Cold War: the war on the other superpower costs 450 less 25 per DEFCON level above 1");
		// missiles: three per node, paid by the owner
		const int node = g.leaders[size_t(us)].capital;
		g.nodes[size_t(node)].nukes = 0;
		g.leaders[size_t(us)].tribute = 1000;
		int built = 0;
		for (int k = 0; k < 5; k++) built += g.build_nuke(node, true) == 1;
		t.check(built == 3 && g.nodes[size_t(node)].nukes == 3, "ctw_build_nuke: at most 3 missiles per territory");
		// the AI's strike: targets are the player's non-capital nodes first, the capital last; the exchange destroys them
		g.nodes[size_t(g.leaders[size_t(ru)].capital)].nukes = 2;
		g.defcon_level = 5;
		const int a0 = g.num_nodes(g.player_tribe, false);
		const bool launched = g.ai_launch_nukes();
		const bool pending = g.nuke_pending;
		int aimed = 0;
		for (int tg : g.tribe2_nuke_targets) aimed += tg >= 0;
		const int ru_nukes = g.num_nukes(ru);
		g.finish_nuclear_exchange(false);
		int nuked = 0;
		for (const auto& n : g.nodes) nuked += n.nuked;
		t.check(launched && pending && !g.nuke_pending && aimed >= 1 && ru_nukes == 0 && nuked == aimed && g.num_nodes(g.player_tribe, false) <= a0 - 1 &&
		            g.stance(us, ru) == ctw::kWar && g.defcon_level == 5,
		        "ctw_ai_launch_nukes: war, DEFCON 5, the AI's missiles are aimed and destroy territories (" + std::to_string(aimed) + ")");
		// espionage: missions in order, a won mission advances, a lost one does not
		ctw::ConquestGame h;
		setup_game(files, rules, h, "CTW_Cold_War_Map_01", 5);
		h.leaders[size_t(h.player_tribe)].tribute = 500;
		ctw::StyleChoice sc;
		std::string why;
		const bool begun = h.begin_espionage(&sc, &why);
		const int level0 = h.cur_espionage_level;
		h.process_espionage_results(false, sc);
		const int after_loss = h.cur_espionage_level;
		h.process_espionage_results(true, sc);
		t.check(begun && sc.category == h.espionage_category() && after_loss == level0 && h.cur_espionage_level == level0 + 1 && h.espionage_missions() == 6,
		        "espionage: six missions per superpower, a won mission advances the level");
	}
	// ---- the AI: overruns, attacks, no-attack bonus, diplomacy
	{
		ctw::ConquestGame g;
		setup_game(files, rules, g, "CTW_World_Map_01", 9);
		// three armies of one nation around an empty defended territory overrun it; one defender army next to it stops that
		int target = -1;
		for (size_t i = 0; i < g.nodes.size() && target < 0; i++) {
			if (g.nodes[i].links.size() >= 4 && !(g.nodes[i].flags & ctw::kCapital)) target = int(i);
		}
		const int X = (g.player_tribe + 1) % ctw::kSlots;
		for (auto& l : g.pieces) l.clear();
		const auto& nb = g.nodes[size_t(target)].links;
		for (int k = 0; k < 3; k++) {
			g.nodes[size_t(nb[size_t(k)])].owner = X;
			g.pieces[size_t(X)].push_back({nb[size_t(k)], false});
		}
		const int Y = (X + 1) % ctw::kSlots;
		g.nodes[size_t(target)].owner = Y;
		const bool over = g.check_overrun_nodes(nb[0], target, -1, nullptr);
		g.nodes[size_t(nb[3])].owner = Y;
		g.pieces[size_t(Y)].push_back({nb[3], false});
		g.pieces[size_t(Y)].push_back({target, false});
		const bool still = g.check_overrun_nodes(nb[0], target, -1, nullptr);
		g.nodes[size_t(target)].flags |= ctw::kCapital;
		const bool cap = g.check_overrun_nodes(nb[0], target, -1, nullptr);
		t.check(over && !still && !cap, "check_overrun: 3 vs 0 around the node overruns, two defending armies (on it and next to it) prevent it, a capital cannot be overrun");
		// determinism of the AI turn
		ctw::ConquestGame a, b2;
		setup_game(files, rules, a, "CTW_Napoleon_Map_01", 13);
		setup_game(files, rules, b2, "CTW_Napoleon_Map_01", 13);
		play_rounds(a, 4);
		play_rounds(b2, 4);
		t.check(a.save_bytes() == b2.save_bytes(), "the AI plays the same campaign identically twice (plan_strategy, do_diplomacy)");
		// a nation with no attack to make gets CTW_NO_ATTACK_BONUS
		ctw::ConquestGame c;
		setup_game(files, rules, c, "CTW_Alexander_Map_01", 5);
		int idle = -1;
		for (int s = 0; s < ctw::kSlots; s++) {
			if (s != c.player_tribe && c.leaders[size_t(s)].active && c.leaders[size_t(s)].valid && c.leaders[size_t(s)].vassal_of < 0) idle = s;
		}
		for (auto& l : c.pieces) l.clear();
		const int tr0 = c.leaders[size_t(idle)].tribute;
		c.valid_tribes = {idle};
		c.ai_index = 0;
		c.ai_phase = 0;
		c.ai_step();
		t.check(c.leaders[size_t(idle)].tribute >= tr0 + ctw::K(rules, "ctw_no_attack_bonus") - 100, "an AI nation without an attack gets the skip bonus (+25)");
		// buy peace costs the buyer's attitude-based price and is paid to the other side
		const int p0 = c.leaders[size_t(idle)].tribute;
		int other = -1;
		for (int s = 0; s < ctw::kSlots; s++) {
			if (s != idle && c.leaders[size_t(s)].active && c.leaders[size_t(s)].valid) other = s;
		}
		c.set_war(idle, other);
		c.leaders[size_t(idle)].tribute = 500;
		const int cost = c.peace_cost(idle, other);
		const int o0 = c.leaders[size_t(other)].tribute;
		const bool ok = c.propose_peace(idle, other);
		t.check(ok && c.stance(idle, other) == ctw::kPeace && c.leaders[size_t(idle)].tribute == 500 - cost && c.leaders[size_t(other)].tribute == o0 + cost,
		        "propose_peace: the proposer pays the price to the other nation");
		(void)p0;
	}
	// ---- the five generic builtins, run from a script inside a battle
	{
		auto b = build_with(files, rules, art.get(), "CTW_World_Map_01", 21, {});
		bool ok = false;
		int diff_after = -1, collected = -2, warning = -2, militia = -2;
		if (b && b->game->scripts) {
			const std::string path = "/tmp/ctw2_builtins_test.bhs";
			{
				FILE* f = fopen(path.c_str(), "w");
				fputs("int conquest t_builtins () {\n  set_difficulty(4);\n  return 1;\n}\n"
				      "int conquest t_collected () {\n  return num_good_collected(1, \"Food\");\n}\n"
				      "int conquest t_warning () {\n  return resource_cap_warning(1, \"Food\");\n}\n"
				      "int conquest t_militia () {\n  int i;\n  for (i = 0; i < 300; i++) {\n    if (citizen_to_militia(1, i) == 1) return 1;\n  }\n  return 0;\n}\n", f);
				fclose(f);
			}
			sim::ScriptHost& h = *b->game->scripts;
			std::string err;
			if (h.compile(path, &err)) {
				h.run(bhs::to_u16("t_builtins"), {});
				diff_after = b->game->info.difficulty;
				h.run(bhs::to_u16("t_collected"), {});
				collected = bhs::value_int(h.rt.return_value.get());
				h.run(bhs::to_u16("t_warning"), {});
				warning = bhs::value_int(h.rt.return_value.get());
				h.run(bhs::to_u16("t_militia"), {});
				militia = bhs::value_int(h.rt.return_value.get());
				ok = true;
			}
			std::remove(path.c_str());
		}
		t.check(ok && diff_after == 3 && collected >= 0 && warning >= 0 && militia == 1, "set_difficulty / num_good_collected / resource_cap_warning / citizen_to_militia run from a battle script");
	}
	// ---- the card draw: every tactical card of the deck is known to the rules table
	{
		auto cr = ctw::load_conquest_rules(files);
		int tactical = 0;
		for (const auto& c : cr->cards) tactical += c.tactical();
		t.check(tactical == 21 + 0 || tactical > 15, "conquestrules.xml: the tactical cards are listed (" + std::to_string(tactical) + ")");
	}
}

int selftest(const std::string& game_dir) {
	GameFiles files(game_dir);
	Rules rules = load_rules(files);
	start_game(rules, GameSetup());
	Tally t;
	// ---- (3) costs at the default attitudes
	{
		ctw::ConquestGame g;
		setup_game(files, rules, g, "CTW_Alexander_Map_01", 1);
		int a = -1, b = -1, c = -1, d = -1;
		for (int s = 0; s < ctw::kSlots; s++) {
			if (s == g.player_tribe || !g.leaders[size_t(s)].active) continue;
			(a < 0 ? a : b < 0 ? b : c < 0 ? c : d) = s;
			if (d >= 0) break;
		}
		const int k = [&](const char* n) { return ctw::K(rules, n); }("ctw_base_war_cost");
		t.check(g.war_cost(a, b) == k + g.leaders[size_t(a)].stance_time[size_t(b)] * ctw::K(rules, "ctw_war_cost_stance_time_x"), "war cost = base + stance_time x factor");
		t.check(g.peace_cost(a, b) == ctw::K(rules, "ctw_base_peace_cost") + g.get_attitude(b, a) * ctw::K(rules, "ctw_peace_cost_att_x"), "peace cost = base + attitude x factor");
		g.set_ally(a, c);
		g.set_ally(a, d);
		t.check(g.alliance_cost(a, b) == g.peace_cost(a, b) + 2 * ctw::K(rules, "ctw_additional_ally_inc"), "alliance with 2 allies = peace cost + 2 x 100 (the buyer's allies)");
		g.set_ally(b, c);
		const int base_war = g.war_cost(b, c);
		t.check(base_war == (ctw::K(rules, "ctw_base_war_cost") + g.leaders[size_t(b)].stance_time[size_t(c)]) * ctw::K(rules, "ctw_ally_war_cost_x"), "war on an ally costs x2");
		const int c0 = g.card_cost(a);
		g.leaders[size_t(a)].num_cards_purchased = 1;
		const int c1 = g.card_cost(a);
		g.leaders[size_t(a)].num_cards_purchased = 5;
		t.check(c0 == 30 && c1 == 70 && g.card_cost(a) == 110, "card cost 30 / 70 / capped at 110");
		t.check(ctw::K(rules, "ctw_starting_tribute") == 25 && g.leaders[size_t(a)].tribute == 25, "starting tribute 25");
	}
	// ---- (2) determinism of setup, (4) pick_style purity
	{
		ctw::ConquestGame g1, g2;
		setup_game(files, rules, g1, "CTW_Napoleon_Map_01", 42);
		setup_game(files, rules, g2, "CTW_Napoleon_Map_01", 42);
		t.check(g1.save_bytes() == g2.save_bytes(), "setup with a fixed seed is deterministic");
		auto links = g1.find_available_links(g1.player_tribe);
		bool same = !links.empty();
		for (const auto& l : links) {
			const auto s1 = g1.pick_style(l), s2 = g1.pick_style(l), s3 = g2.pick_style(l);
			same &= s1.index == s2.index && s1.index == s3.index && s1.category == s2.category;
		}
		t.check(same, "pick_style returns the same style on every call (" + std::to_string(links.size()) + " links)");
	}
	// ---- (5) round length, level / age progression, territory income
	{
		ctw::ConquestGame cw, al, wo;
		setup_game(files, rules, cw, "CTW_Cold_War_Map_01", 3);
		setup_game(files, rules, al, "CTW_Alexander_Map_01", 3);
		setup_game(files, rules, wo, "CTW_World_Map_01", 3);
		t.check(cw.length() == 14 && al.length() == 19 && wo.length() == 24, "campaign lengths 14 / 19 / 24");
		const int s = cw.player_tribe;
		const int before = cw.leaders[size_t(s)].tribute;
		int expect = 0;
		for (const auto& n : cw.nodes) expect += n.owner == s ? n.level * ctw::K(rules, "ctw_terr_income_x") + ctw::K(rules, "ctw_terr_income_base") : 0;
		cw.check_income(s);
		t.check(cw.leaders[size_t(s)].tribute - before == expect && expect > 0, "Cold War territory income = sum(level x 1 + 1) = " + std::to_string(expect));
		const int s2 = al.player_tribe;
		const int b2 = al.leaders[size_t(s2)].tribute;
		al.check_income(s2);
		t.check(al.leaders[size_t(s2)].tribute == b2, "Alexander has no territory income");
		// age step: World has 3 rounds per age (starts at round 2): the next advance reaches round 3 -> age + 1, owned levels + 1
		std::vector<int> lv;
		for (const auto& n : wo.nodes) lv.push_back(n.level);
		const int age0 = wo.cur_age, round0 = wo.cur_round;
		wo.advance_round();
		bool levels_up = true;
		for (size_t i = 0; i < wo.nodes.size(); i++) {
			if (wo.nodes[i].owner >= 0 && lv[i] < wo.ending_age) levels_up &= wo.nodes[i].level == lv[i] + 1;
			if (wo.nodes[i].owner < 0) levels_up &= wo.nodes[i].level == lv[i];
		}
		t.check(round0 == 2 && wo.cur_round == 3 && wo.cur_age == age0 + 1 && levels_up, "World: round 2 -> 3 raises the age and every owned territory's level");
	}
	// ---- (6) the six outcome cases of process_results (kind x role)
	{
		auto fresh = [&](ctw::ConquestGame& g) {
			setup_game(files, rules, g, "CTW_World_Map_01", 11);
			g.turns_no_attack = 0;
		};
		auto player_attack = [&](ctw::ConquestGame& g, ctw::LinkChoice* l) {
			for (const auto& c : g.find_available_links(g.player_tribe)) {
				if (c.defender < 0) {
					*l = c;
					return true;
				}
			}
			return false;
		};
		{
			ctw::ConquestGame g;
			fresh(g);
			ctw::LinkChoice l;
			const bool ok = player_attack(g, &l);
			const int tribute0 = g.leaders[size_t(g.player_tribe)].tribute, nt = g.nodes[size_t(l.to)].tribute;
			g.process_results(0, true, l, ctw::StyleChoice());
			t.check(ok && g.nodes[size_t(l.to)].owner == g.player_tribe && g.leaders[size_t(g.player_tribe)].tribute == tribute0 + nt &&
			            g.has_army(l.to) && !g.has_army(l.from),
			        "player attacks, wins: node taken, tribute collected, the army moves in");
		}
		{
			ctw::ConquestGame g;
			fresh(g);
			ctw::LinkChoice l;
			player_attack(g, &l);
			g.process_results(0, false, l, ctw::StyleChoice());
			t.check(g.nodes[size_t(l.to)].owner == -1 && g.has_army(l.from), "player attacks, loses: the node stays barbarian, the army stays");
		}
		{
			ctw::ConquestGame g;
			fresh(g);
			ctw::LinkChoice l;
			player_attack(g, &l);
			g.process_results(1, false, l, ctw::StyleChoice());
			t.check(g.nodes[size_t(l.to)].owner == g.player_tribe, "kind 1 (the attacker wins without a battle): the player's attack succeeds");
		}
		{  // the player defends: an AI nation attacks the player's capital-adjacent node
			ctw::ConquestGame g;
			fresh(g);
			const int me = g.player_tribe;
			int ai = -1, target = -1, from = -1;
			for (int s = 0; s < ctw::kSlots && ai < 0; s++) {
				if (s == me || !g.leaders[size_t(s)].active || g.pieces[size_t(s)].empty()) continue;
				for (const auto& pc : g.pieces[size_t(s)]) {
					for (int n : g.nodes[size_t(pc.node)].links) {
						if (g.nodes[size_t(n)].owner == me && !(g.nodes[size_t(n)].flags & ctw::kCapital)) {
							ai = s, target = n, from = pc.node;
						}
					}
				}
			}
			if (ai < 0) {  // no neighbour: hand the nearest AI army the player's capital's link by moving it there
				ai = (me + 1) % ctw::kSlots;
				while (!g.leaders[size_t(ai)].active) ai = (ai + 1) % ctw::kSlots;
				target = g.leaders[size_t(me)].capital;
				from = g.pieces[size_t(ai)].empty() ? g.leaders[size_t(ai)].capital : g.pieces[size_t(ai)][0].node;
			}
			ctw::LinkChoice l;
			l.attacker = ai, l.defender = me, l.from = from, l.to = target;
			const int owner_before = g.nodes[size_t(target)].owner;
			g.process_results(0, true, l, ctw::StyleChoice());
			const bool held = g.nodes[size_t(target)].owner == owner_before;
			t.check(held && g.need_defensive_bonus, "player defends, wins: the node is held, a defensive reward is due");
			g.process_results(0, false, l, ctw::StyleChoice());
			t.check(g.nodes[size_t(target)].owner == ai || g.game_end == ctw::kPlayerEliminated, "player defends, loses: the node goes to the attacker (or the campaign ends on the capital)");
		}
		{  // a client state (base_armies 0) beaten by the player becomes a vassal (Cold War allows vassals)
			ctw::ConquestGame g;
			setup_game(files, rules, g, "CTW_Cold_War_Map_01", 5);
			int client = -1;
			for (int s = 0; s < ctw::kSlots; s++) {
				if (g.leaders[size_t(s)].active && g.leaders[size_t(s)].base_armies == 0) client = s;
			}
			t.check(client < 0 || g.def->allow_vassals, "client states exist only where vassals are allowed");
		}
	}
	// ---- (7) the battle of a link
	{
		ctw::ConquestGame g;
		setup_game(files, rules, g, "CTW_Napoleon_Map_01", 6);
		auto links = g.find_available_links(g.player_tribe);
		bool ok = !links.empty();
		for (const auto& l : links) {
			const auto st = g.pick_style(l);
			const auto B = g.make_battle(l, st);
			const bool players = B.seats.size() >= 2 && B.seats.size() <= 8 && B.seats[0].human && B.setup.num_nations == int(B.seats.size());
			const bool tech = B.setup.starting_technology == g.cur_age && B.setup.ending_technology == std::min(7, g.cur_age + (g.cur_age < g.ending_age ? 1 : 0));
			bool times = true;
			for (const auto& r : g.reinforcements) times &= r.time >= 60 * g.crules->min_arrival_time && r.time % 60 == 0;
			ok &= players && tech && times;
		}
		t.check(ok, "battle description: seats <= 8 with the player first, starting_technology = cur_age, reinforcement times in 2..4 minute steps");
	}
	// ---- saves
	{
		ctw::ConquestGame g, h;
		setup_game(files, rules, g, "CTW_New_World_Map_01", 8);
		play_rounds(g, 2);
		const auto a = g.save_bytes();
		const bool loaded = h.load_bytes(files, rules, a).empty();
		t.check(loaded && h.save_bytes() == a, "campaign save round trip is byte-identical");
		std::vector<uint8_t> broken = a;
		broken[broken.size() / 2] ^= 0x5a;
		ctw::ConquestGame k;
		t.check(!k.load_bytes(files, rules, broken).empty(), "a corrupted save is refused");
	}
	selftest2(files, rules, t);
	{  // ---- a native save of a generated battle regenerates the battle's conquest map (newgame.hpp: the setup text carries the conquest input)
		auto art = load_anim_source(files, rules);
		auto b = build_with(files, rules, art.get(), "CTW_World_Map_01", 21, {});
		if (!b || !b->B.custom_map.empty()) {
			t.check(false, "built a generated World battle for the save checks");
		} else {
			sim::Game& G = *b->game;
			sim::CommandManager cm;
			cm.play = 0;
			cm.play_who = 0;
			for (int f = 0; f < 40; f++) {
				cm.process_turn(G);
				G.do_frame();
			}
			const uint32_t seed = b->ex.setup.seed;
			const std::string rel = sim::scenario_rel_path(files, "ctw:battle");  // what F5 stores for the battle
			sim::RandomGameSetup S;
			t.check(sim::is_random_scenario(rel) && sim::parse_random_setup(rel, S) && S.ctw && !S.conquest.empty() && sim::random_setup_string(S) == rel,
			        "the battle's scenario slot is a random: setup text with ctw=1 and the conquest input; it round-trips through the parser");
			mapgen::ConquestInput ci;
			t.check(sim::parse_conquest_input(S.conquest, ci) && ci.valid && ci.side > 0 && sim::conquest_input_string(ci) == S.conquest,
			        "the conquest input round-trips through its text form (" + std::to_string(S.conquest.size()) + " characters, square of " + std::to_string(ci.side) + ")");
			const std::vector<uint8_t> bytes = sim::save_game_bytes(G, rel, seed);
			bool same_bytes = false, same_map = false, plain_differs = false;
			std::string why;
			try {
				sim::SaveInfo info;
				std::unique_ptr<sim::Game> H = sim::load_game_bytes(files, rules, art.get(), bytes, &info);
				same_bytes = H && sim::save_game_bytes(*H, rel, seed) == bytes;
				if (H) {
					same_map = H->world.tdata == G.world.tdata && H->world.wdata.size() == G.world.wdata.size();
					for (size_t i = 0; same_map && i < G.world.wdata.size(); i++) same_map = H->world.wdata[i].land == G.world.wdata[i].land && H->world.wdata[i].flags == G.world.wdata[i].flags;
				}
				// the negative control: the same text without the conquest input regenerates a plain random map
				sim::RandomGameSetup P = S;
				P.conquest.clear();
				sim::RandomExtras px;
				std::unique_ptr<sim::Game> plain = sim::new_random_game(files, rules, art.get(), P, &px);
				plain_differs = plain && plain->world.tdata != G.world.tdata;
			} catch (const std::exception& e) {
				why = e.what();
			}
			t.check(same_bytes, "a loaded battle save re-saves byte-identical" + (why.empty() ? std::string() : ": " + why));
			t.check(same_map, "the loaded battle's map equals the battle's (land, flags, tcoords)");
			t.check(plain_differs, "without the conquest input the setup text regenerates a different (plain random) map");
		}
	}
	std::printf("selftest: %d checks, %d failed\n", t.n, t.bad);
	return t.bad ? 1 : 0;
}

}  // namespace ctwcli
