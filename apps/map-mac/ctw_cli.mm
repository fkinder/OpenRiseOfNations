// Conquer the World command-line checks (ctw_cli.h). Plain C++: no Cocoa.
#include "ctw_cli.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "assets/files.hpp"
#include "sim/anim_assets.hpp"
#include "sim/commands.hpp"
#include "sim/game.hpp"
#include "sim/rules.hpp"
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
	std::printf("\n");
}

// One full AI round after the player held back.
void play_round(ctw::ConquestGame& g, bool verbose) {
	g.end_turn_player();
	g.begin_ai_round();
	for (int guard = 0; guard < 1000; guard++) {
		ctw::AiStep st = g.ai_step();
		if (st.kind == ctw::AiStep::kDone) break;
		if (st.kind == ctw::AiStep::kPlayerAttacked) {
			if (verbose) std::printf("  the %s attack %s\n", g.tribe_name(st.battle.attacker).c_str(), g.node_name(st.battle.to).c_str());
			g.cur_style = g.pick_style(st.battle);
			g.resolve_player_defense(g.rget(0, 99) < 50);
		}
	}
	g.finish_round();
}

}  // namespace

void play_rounds(ctw::ConquestGame& g, int rounds) {
	for (int r = 0; r < rounds && g.game_end == 0; r++) play_round(g, false);
}

int simulate(const std::string& game_dir, const std::string& campaign_arg, unsigned seed, int rounds, int difficulty,
             const std::string& out, bool verbose) {
	GameFiles files(game_dir);
	Rules rules = load_rules(files);
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
	std::printf("  frame %d: %s%s\n", game->frame, won ? "victory" : "defeat", adjudicated ? " (adjudicated)" : "");
	if (game->scripts) {
		for (const std::string& m : game->scripts->messages) std::printf("  script: %s\n", m.c_str());
		int shown = 0;
		for (const auto& u : game->scripts->unported()) {
			if (shown++ < 6) std::printf("  unported builtin %s x%d\n", u.first.c_str(), u.second);
		}
		std::printf("  scripts: %lld runs, %lld failures\n", (long long)game->scripts->runs, (long long)game->scripts->run_errors);
	}
	g.last_battle_victory = won != 0;
	g.process_results(0, won != 0, l, g.cur_style);
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

int selftest(const std::string& game_dir) {
	GameFiles files(game_dir);
	Rules rules = load_rules(files);
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
		g.set_ally(b, c);
		g.set_ally(b, d);
		t.check(g.alliance_cost(a, b) == g.peace_cost(a, b) + 2 * ctw::K(rules, "ctw_additional_ally_inc"), "alliance with 2 allies = peace cost + 2 x 100");
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
	std::printf("selftest: %d checks, %d failed\n", t.n, t.bad);
	return t.bad ? 1 : 0;
}

}  // namespace ctwcli
