// Game: loading a scenario into the simulation and Game::do_frame (docs/frame-spec.md sections 1-5).
#include "sim/game.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "sim/ai.hpp"
#include "sim/capture.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/garrison.hpp"
#include "sim/guys.hpp"
#include "sim/production.hpp"
#include "sim/script_host.hpp"
#include "sim/territory.hpp"
#include "sim/world_load.hpp"

namespace ron::sim {

Game::Game() { forms_init(forms); }
Game::~Game() = default;

// ---------------------------------------------------------------- diplomacy

bool is_ally(const Game& g, int32_t a, int32_t b) {
	if (a == b) return true;
	if (a < 0 || a >= kNumLeaders || b < 0 || b >= kNumLeaders) return false;
	return g.leaders[size_t(a)].diplos(b) == 2 && g.leaders[size_t(b)].diplos(a) == 2;
}
bool is_enemy(const Game& g, int32_t a, int32_t b) {
	if (a == b) return false;
	if (a < 0 || a >= kNumLeaders || b < 0 || b >= kNumLeaders) return false;
	return g.leaders[size_t(a)].diplos(b) == 0 || g.leaders[size_t(b)].diplos(a) == 0;
}
bool is_team(const Game& g, int32_t a, int32_t b) {
	if (a == b) return true;
	if (a < 0 || a >= 8 || b < 0 || b >= 8) return false;  // get_player finds no seat: no team
	// TODO(frame-spec): team_style 7 excludes team-8 seats; after frame 0 the original answers is_ally instead.
	const int8_t t = g.info.team[size_t(a)];
	return t >= 0 && t < 4 && t == g.info.team[size_t(b)];
}

namespace {

// Leader::init @6e3930, the diplomacy part: tribe < 0 (Leaders::init, every slot) is allied with itself only and at
// war with everyone; a scenario leader (tribe >= 0) is allied with its team and otherwise at war when teams are
// locked, else at peace. treaties start as reveal_map == 3, plus bit 1 for team members.
void leader_init_diplomacy(Game& g, int32_t who, int32_t tribe) {
	Leader& l = g.leaders[size_t(who)];
	const bool locked = g.info.team_style != 0 && g.info.team_style != 8 && g.info.team_style != 11;
	for (int32_t i = 0; i < 8; i++) {
		l.treaty(i) = g.info.reveal_map == 3 ? 1 : 0;
		if (tribe < 0) {
			l.diplo[size_t(i)] = i == who ? 2 : 0;
			continue;
		}
		const bool team = is_team(g, who, i);
		l.diplo[size_t(i)] = team ? 2 : locked ? 0 : 1;
		if (team) l.treaty(i) |= 1;
	}
	// ally_mask (fog-spec 6.6 item 1): bit(who), plus allies with ALLY_LOS or reveal_map != 0. Leader::init computes
	// it inside the slot loop and again after the starting techs; both see the default diplomacy above (the
	// scenario's chunk 41 is applied afterwards and first reaches the mask in plan_strategy at frame 0).
	if (tribe < 0) {
		l.ally_mask = uint8_t(1u << (uint32_t(who) & 31));
		return;
	}
	leader_init_ally_mask(g, who);
}

}  // namespace

// ---------------------------------------------------------------- loading

namespace {

std::string lower_ascii(std::string s) {
	for (char& c : s) {
		if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
	}
	return s;
}

// Types::get_type_index over [lo, hi): TYPENAME (NAME when empty), first match wins (case-insensitive, as the world
// loader).
std::map<std::string, int> type_names(const Rules& rules, int lo, int hi) {
	std::map<std::string, int> m;
	for (int i = lo; i < hi && size_t(i) < rules.types.size(); i++) {
		const Type& t = rules.types[size_t(i)];
		if (!t.loaded) continue;
		const ustr& n = t.type_name.empty() ? t.name : t.type_name;
		if (n.empty()) continue;
		m.emplace(lower_ascii(to_utf8(n)), i);
	}
	return m;
}

int lookup(const std::map<std::string, int>& m, const std::string& n) {
	auto it = m.find(lower_ascii(n));
	return it == m.end() ? -1 : it->second;
}

}  // namespace

Rules& Game::mut_rules() {
	if (!own_rules) {
		own_rules = std::make_unique<Rules>(*rules);
		rules = own_rules.get();
		for (auto& list : objects) {
			for (auto& u : list.units) {
				if (u && u->type >= 0 && u->type < kNumTypes) u->ptype = &rules->types[size_t(u->type)];
			}
			for (auto& b : list.builds) {
				if (b && b->type >= 0 && b->type < kNumTypes) b->ptype = &rules->types[size_t(b->type)];
			}
		}
	}
	return *own_rules;
}

void Game::load(const GameFiles& files, const Scenario& s, const Rules& r, uint32_t seed) {
	own_rules.reset();
	rules = &r;
	base_rules = &r;
	// ScenarioFuncSet::init: the script state of ScenarioData.
	scen = ScenarioData();
	buildings_gather = building_unit_bonus = building_resource_bonus = true;
	// World grid with goods, items and buildings (docs/world-spec.md section 3); units are created below through
	// Objects::init_unit, whose add_to_world / guy placement stamp their footprints.
	Scenario no_units = s;
	no_units.units.clear();
	WorldBuild wb;
	build_world(wb, files, no_units, &r, seed);
	world = std::move(wb.world);
	regions = wb.regions;
	heights.vxs = s.terrain.vxs();
	heights.vys = s.terrain.vys();
	heights.h = s.terrain.heights;
	if (heights.h.size() != size_t(heights.vxs) * size_t(heights.vys)) heights.h.assign(size_t(heights.vxs) * size_t(heights.vys), 0.0f);

	// Game::run: game_random := GameInfo.seed, Game::init_rules_and_teams -> Game::init_starting_resources (S1/S2;
	// economy-spec 5.7), then Setup::build_game reseeds (below), so its draws never reach the simulation's stream.
	rng.reseed(seed);
	init_starting_resources(*this);
	// Setup::build_game: game_random := GameInfo.seed and one draw for world.seed (the header chunk then overwrites it
	// with map_seed); the leaders chunk's Leader::init personality draws follow (ai-spec 2.1), and the terrain chunk's
	// ScenarioEditor::init_terrain reseeds game_random := GameInfo.seed before the objects (below, after the leaders).
	rng.reseed(seed);
	rng.get(0, 0xffff);
	world.sea_map = s.ai_sea_map;  // header: World.sea_map = ai_sea_map (random_personality reads it)
	this->seed = seed;
	frame = 0;
	tick = 0;

	// GameInfo from the scenario header (ScenarioRead::import; docs/scenario-objects-spec.md section 2). reveal_map
	// stays the caller's choice (ron-sim --reveal).
	if (s.has_settings) {
		info.team_style = uint8_t(s.team_style);
		info.game_rules = uint8_t(s.game_rules);
		info.starting_technology = uint8_t(s.start_tech);
		info.ending_technology = uint8_t(s.end_tech);
		info.elimination = uint8_t(s.elimination);
		info.victory = uint8_t(s.victory);
		info.score_goal = uint8_t(s.score_goal);
		info.time_limit = uint8_t(s.time_limit);
		info.wonderwin = uint8_t(s.wonderwin);
		info.popwin = uint8_t(s.popwin);
		info.econwin = uint8_t(s.econwin);
		info.rush_rules = uint8_t(s.rush_rules);
		if (s.pop_limit >= 0) info.pop_limit = uint8_t(s.pop_limit);
		if (s.scenario_flags & 4) info.flags |= 8;
		if (s.scenario_flags & 8) info.flags |= 4;
		info.map_style = uint8_t(s.map_style);
		scripted_rules = info.game_rules == 10;  // Game::run: semaphore byte1 |= 0x10 for scripted rules
	}
	// Mountain ranges as MountainsData keeps them (gather tiles of mines, production-spec 3.12).
	mountains.clear();
	for (const PlacedMountain& m : wb.mountains) {
		PlacedRange pr;
		pr.wx = m.wx;
		pr.wy = m.wy;
		pr.range = m.range;
		pr.solid = m.wcoords;
		pr.tiles = m.tcoords;
		pr.tcoord_count = int32_t(m.tcoords.size());
		mountains.push_back(std::move(pr));
	}
	for (auto& c : cities) c.clear();
	for (auto& c : forts) c.clear();
	for (auto& c : wonder_records) c.clear();
	for (auto& c : unbuilt_cities) c.clear();
	for (auto& c : unbuilt_forts) c.clear();
	for (auto& c : unbuilt_wonders) c.clear();
	wonders.fill(0);
	terra_cotta_total = world_pop = world_cities = 0;

	// Leaders (scenario chunk 11; ScenarioRead::load_leader_chunk). Gaia (9) is always valid. Leaders::init runs
	// Leader::init(who, -1) on every slot, then each scenario leader is re-inited with its tribe (diplomacy defaults
	// and the starting techs of production-spec 7.9.7), its record applied, and its child chunks read in file order:
	// 38 techs and 76 types (Leader::gain_tech(t, 0, 0, _, 1), stopping at the first unknown name), 41 diplomacy,
	// 44 stockpile (overwrites).
	for (int32_t who = 0; who < kNumLeaders; who++) {
		leaders[size_t(who)] = Leader();
		leader_init_diplomacy(*this, who, -1);
		leader_init_economy(*this, who, false);
		leader_calc_attrition(*this, who);  // Leader::init (combat-spec 7.4)
		leader_calc_anti_attrition(*this, who);
	}
	for (auto& v : supplies) v.clear();
	for (auto& v : attrition_free_points) v.clear();
	farms.clear();
	hold_doobers.clear();
	for (auto& c : caravans) c.clear();
	bool have_local = false;
	const auto all_types = type_names(r, 0, kNumTypes);
	everyone_mask = 0;
	// Game::init_rules_and_teams set every seat's flags (VALID | ACTIVE, HUMAN) and tribe from the GameInfo player
	// slots before the leaders chunk, so each Leader::init (personality draws, ai-spec 2.3) already sees the later
	// seats.
	for (const ScenarioPlayer& p : s.players) {
		if (p.who < 0 || p.who >= 8) continue;
		Leader& l = leaders[size_t(p.who)];
		l.tribe = p.tribe;
		l.flags |= leaderflag::kValid | leaderflag::kActive;
		if (p.is_human) l.flags |= leaderflag::kHuman;
	}
	for (const ScenarioPlayer& p : s.players) {
		if (p.who < 0 || p.who >= 8) continue;
		Leader& l = leaders[size_t(p.who)];
		everyone_mask |= 1u << (uint32_t(p.who) & 31);  // ScenarioRead::load_leader_chunk
		// Leader::init(who, tribe, color)
		l.tribe = p.tribe;
		l.color = p.color;
		leader_init_diplomacy(*this, p.who, p.tribe < 0 ? 0 : p.tribe);
		leader_init_techs(*this, p.who);
		leader_init_economy(*this, p.who, true);  // after the starting techs (type_avail of knowledge / metal / oil)
		leader_calc_attrition(*this, p.who);
		leader_calc_anti_attrition(*this, p.who);
		// Leader::init's AI part (production script, personality draws; needs the human / co-op flags) before the
		// file's child chunks (ai-spec 2.1, 2.2).
		if (p.is_human) l.flags |= leaderflag::kHuman;
		else l.flags &= ~leaderflag::kHuman;
		if (p.leader_flags & 0x400) l.flags |= 8;
		leader_init_ai(*this, p.who);
		l.difficulty = p.difficulty;
		l.gov = p.gov;
		l.name = p.name;
		l.handicap = p.handicap;
		if (p.is_human) l.flags |= leaderflag::kHuman;
		else l.flags &= ~leaderflag::kHuman;
		l.flags |= leaderflag::kValid | leaderflag::kActive;
		// File leader flags (scenario-objects-spec section 7).
		const uint32_t f = p.leader_flags;
		if (f & 0x1) l.flags2 |= 2;
		if (f & 0x400) l.flags |= 8;  // COOP_SOLO (not in a multiplayer game)
		if (f & 0x2) l.flags2 |= 4;
		if (f & 0x40) l.flags2 |= 8;
		if (f & 0x4) l.flags2 |= 0x20;
		if (f & 0x8) l.flags |= leaderflag::kCanTransportCiv;
		if (f & 0x80) l.flags |= leaderflag::kCanTransportMil;
		if (f & 0x100) l.flags |= leaderflag::kCanTransportSct;
		if (f & 0x10) l.flags |= leaderflag::kIgnorePopcap;
		if (f & 0x20) l.flags |= leaderflag::kAssimilateImmed;
		if (f & 0x200) l.flags2 |= 1;
		size_t di = 0;
		for (uint16_t id : p.child_order) {
			if (id == 38 || id == 76) {
				for (const std::string& name : id == 38 ? p.techs : p.types) {
					const int ti = lookup(all_types, name);
					if (ti < 0) break;
					gain_tech(*this, p.who, ti, 0, 0, false, true);
				}
			} else if (id == 41) {
				for (; di < p.diplomacy.size(); di++) {
					const ScenarioPlayer::Diplo& d = p.diplomacy[di];
					if (d.who < 0 || d.who >= 8) continue;  // (the original writes +0x74 + who * 4 unchecked)
					l.diplo[size_t(d.who)] = d.diplo;
					l.treaty(d.who) = d.treaty;
				}
			} else if (id == 44) {
				for (const ScenarioPlayer::Stock& st : p.stockpile) {
					if (st.good >= 0 && st.good < 6) l.bucket[size_t(st.good)] = st.amount;
				}
			}
		}
		if (p.is_human && !have_local) {
			local_play = p.who;
			have_local = true;
		}
	}
	leaders[kGaiaWho].flags = leaderflag::kValid;
	leaders[kGaiaWho].color = kGaiaWho;
	// The terrain chunk (after the leaders in every stock file): ScenarioEditor::init_terrain reseeds game_random.
	rng.reseed(seed);
	armies_init(*this);
	init_combat_data(*this);  // fire_proj from the art, the ammo and corpse pools
	// ScenarioEditor::init_terrain at the end of the terrain chunk (world-spec 3 step 6): territory before any object.
	compute_all_territory(*this);

	const auto unit_types = type_names(r, kUnits, kBuilds), build_types = type_names(r, kBuilds, kItem);

	// Items (chunk 3) then goods (chunk 4): Objects::init_item / init_good. Each becomes the terminal of its wcoord's
	// object list (WData.down = -3 / -2, down_who = index; a good overwrites an item on the same wcoord), below every
	// object linked later. Goods with unknown names are skipped (no slot); oil marks no wcoord.
	goods.clear();
	items.clear();
	scen_ally_mask.fill(0);
	for (auto& rp : reveal_points) rp.clear();
	good_mark = 0;
	for (const ScenarioGood& it : s.items) {
		GoodObj gi;
		gi.type = kItem;
		gi.x = it.cx;
		gi.y = it.cy;
		gi.flags = objflag::kValid;
		const int32_t wx = wcoord_of(it.cx), wy = wcoord_of(it.cy);
		if (world.in_w(wx, wy)) {
			world.w(wx, wy).down = -3;
			world.w(wx, wy).down_who = int16_t(items.size());
		}
		items.push_back(gi);
	}
	const auto good_types = type_names(r, kGoods, kEndGoods);
	for (const ScenarioGood& gs : s.goods) {
		const int t = lookup(good_types, gs.type);
		if (t < 0) continue;
		GoodObj gd;
		gd.type = t;
		gd.x = gs.cx;
		gd.y = gs.cy;
		gd.flags = objflag::kValid;
		const int32_t wx = wcoord_of(gs.cx), wy = wcoord_of(gs.cy);
		if (t != fogtype::kOil && world.in_w(wx, wy)) {
			world.w(wx, wy).down = -2;
			world.w(wx, wy).down_who = int16_t(goods.size());
		}
		goods.push_back(gd);
		good_mark = int32_t(goods.size());
		// TODO(herds): herd goods create their Herd (Herds::init_herd).
	}

	// Cities (chunk 7) and objects (chunk 5) in file order (Scenario seq): units, buildings and cities interleave, so
	// the Guy::init_real draws of units and the gather-tile shuffles of mines / woodcutters (Build::init) happen in the
	// original's order. scenario_loading: the grid effects of Wall::init / Wall::start were applied by build_world.
	struct Inside {
		int32_t who, o, in_o, in_who;
	};
	std::vector<Inside> inside;
	struct Item {
		int32_t seq;
		const ScenarioUnit* u;
		const ScenarioBuilding* b;
	};
	std::vector<Item> order;
	for (const ScenarioUnit& su : s.units) order.push_back({su.seq, &su, nullptr});
	for (const ScenarioBuilding& sb : s.buildings) order.push_back({sb.seq, nullptr, &sb});
	std::stable_sort(order.begin(), order.end(), [](const Item& x, const Item& y) { return x.seq < y.seq; });
	scenario_mode = scripted_rules || ctw;  // Game::is_scenario
	scenario_loading = true;
	for (const Item& it : order) {
		if (it.u) {
			const ScenarioUnit& su = *it.u;
			if (su.who < 0 || su.who >= kNumLeaders || su.o < 0 || su.o >= kUnitLimit) continue;
			const int t = lookup(unit_types, su.type);
			if (t < 0) continue;  // the game skips unknown types
			if (su.masks & 8) {
				// Crew slot: the slot is allocated but no unit is created.
				find_free(*this, su.who, 0, kUnitLimit, su.o);
				Unit& u = *unit(su.who, su.o);
				u.flags &= uint8_t(~objflag::kValid);
				u.o_up = int16_t(su.o_up);
				u.o_down = int16_t(su.o_down);
				continue;
			}
			const int32_t o = init_unit(*this, su.who, t, su.cx, su.cy, su.o, su.o_up, su.o_down);
			if (o < 0) continue;
			Unit& u = *unit(su.who, o);
			u.damage = su.damage;
			u.angle = su.angle;
			u.dest_angle = su.dest_angle;
			u.form = su.form;
			u.stance = su.stance;
			u.mana_burn = su.mana_burn;
			u.tolerance = su.tolerance;
			if (su.masks & 1) u.unit_masks |= umask::kAiOff;
			if (su.masks & 2) u.unit_masks |= umask::kDecoy;
			if (su.masks & 4) u.unit_masks |= umask::kUnderCover;
			if (su.masks & 0x10) u.unit_masks |= umask::kPacked;
			// TODO(scenario-objects 4.1): unpacked packers get SpellType::cast_unpack.
			unit_set_angle(*this, u, u.angle, true);
			if (su.inside >= 0) inside.push_back({su.who, o, su.inside, su.inside_who});
			continue;
		}
		const ScenarioBuilding& sb = *it.b;
		if (sb.who < 0 || sb.who >= kNumLeaders || sb.o < kBuildBase || sb.o >= kBuildLimit) continue;
		const int t = lookup(build_types, sb.type);
		if (t < 0) continue;
		// ScenarioRead::load_build_chunk / load_city_chunk: Objects::init_build(who, type, x, y, 0, o).
		const int32_t o = init_build_site(*this, sb.who, t, sb.cx, sb.cy, false, sb.o);
		if (o < 0) continue;
		Build& b = *build(sb.who, o);
		if (sb.active) {
			build_activate_full(*this, b, false, false, false);  // activate(0, 0, 0)
			if (sb.city) {
				update_seen_ally(*this, b);
				leaders[size_t(sb.who)].cities_built++;
			}
			b.damage = sb.damage;
			if (!sb.city) b.visible = 0;
			if (sb.city && b.city >= 0) {
				City& c = cities[size_t(sb.who)][size_t(b.city)];
				c.name = sb.city_name;
				c.founder = int8_t(sb.founder);
				if (sb.city_flags & 1) {
					c.city_flags |= cityflag::kCapital;
					if (sb.city_flags & 2) c.city_flags |= cityflag::kOriginalCapital;
				} else {
					c.city_flags &= uint16_t(~cityflag::kCapital);
				}
			}
		}
		// The production queue (chunk 85): Build::queue_up(t, 0), then Type::unpay_cost(who, -1, -1); a first item in
		// progress gets job_counter = its type index (buildings) or train_time (cities).
		if (!sb.queue.empty() && b.alive()) {
			for (const std::string& qn : sb.queue) {
				const int qt = lookup(all_types, qn);
				if (qt < 0) continue;
				build_queue_up(*this, b, qt, 0);
				unpay_cost(*this, qt, sb.who, -1, -1);
			}
			if (sb.first_in_progress != 0) {
				const int q0 = lookup(all_types, sb.queue[0]);
				if (q0 >= 0 && b.build_queue.queue_size > 0)
					b.build_queue.queue[0].job_counter = sb.city ? int32_t(train_time(*this, o, sb.who, q0)) : q0;
			}
		}
	}
	scenario_loading = false;
	// insert_inside: contained units leave the map and join the container's chain.
	for (const Inside& in : inside) {
		Unit* u = unit(in.who, in.o);
		if (!u || !u->alive()) continue;
		unit_insert_inside(*this, *u, in.in_o, in.in_who);
		// TODO(scenario-objects 4.4): aircraft carriers, transports.
	}
	// World::compute_all_territory with the buildings in place (world-spec 3 step 8, production-spec 5.10.2). TODO:
	// Game::run's Leaders::init_production_script and GameDaemon::calc_danger before the first frame.
	// World::analyze_map (Setup::build_game, before compute_all_territory): land_size = the land regions' sizes.
	// TODO(world-spec): its region flags 4 / 8 (player / resource regions) are not set.
	world_total_land = 0;
	for (int32_t r = 1; r < 64; r++) world_total_land += regions.r[size_t(r)].size;
	compute_all_territory(*this);
	calc_danger(*this);      // Game::run, before the first frame
	update_all_seen(*this);  // Game::run, before the first frame (frame 0: includes the allies' explored share)
}

// ---------------------------------------------------------------- the frame

namespace {

// GameDaemon::calc_market @732270: redraw good i's price trend (economy-spec 5.3). Three game_random draws (M1-M3).
void calc_market(Game& g, int32_t i) {
	const Constants& c = g.rules->constants;
	Market& m = g.market;
	const int32_t v = std::max(m.level[size_t(i)] / 2, c.at(0xce0));  // market_min_variance
	const int32_t half = (v + 1) / 2;
	int32_t a = 0, b = 0;
	if (half > 0) {
		a = g.rnd(half + 1);
		b = g.rnd(half + 1);
	}
	m.next_flux[size_t(i)] = a + b - (half + 1);
	const int32_t r = c.at(0xce8);  // market_trend_range
	const int32_t tr = r - 1 > 0 ? g.rnd(r) : 0;
	const int32_t len = c.at(0xce4) + tr;  // market_min_trend
	m.flux_length[size_t(i)] = len;
	const int32_t d = m.next_flux[size_t(i)] - m.flux[size_t(i)];
	const int32_t sgn = d > 0 ? 1 : d < 0 ? -1 : 0;
	m.delta_flux[size_t(i)] = len != 0 ? (d + (len - 1) * sgn) / len : 0;  // rounds away from zero
}

// GameDaemon::calc_markets @732180 (economy-spec 5.2): every frame (market_cycle_rate 1); good i on ticks
// (tick + i) % 8 == 0, all six at tick 0; drift toward equilibrium every 256 ticks.
void calc_markets(Game& g) {
	const Constants& c = g.rules->constants;
	const int32_t rate = c.at(0xcec);  // market_cycle_rate
	if (g.frame != 0 && rate >= 2 && g.frame % rate != 0) return;
	Market& m = g.market;
	const int32_t t = m.tick;
	for (int32_t i = 0; i < 6; i++) {
		if (t != 0 && (t + i) % 8 != 0) continue;
		if (((t + i) & 0xff) == 0) {
			const int32_t lv = m.level[size_t(i)], e = c.at(0xcdc);  // market_equilibrium
			if (lv > e) {
				m.level[size_t(i)] = e == 0 ? lv - 1 : lv - lv / e;
			} else if (lv < e) {
				m.level[size_t(i)] = lv + 1;
				if (lv + 1 < c.at(0xcd8)) m.level[size_t(i)] = lv + 2;  // market_basement
			}
		}
		if (t == 0) {
			calc_market(g, i);
		} else if (--m.flux_length[size_t(i)] <= 0) {
			calc_market(g, i);
		}
		m.flux[size_t(i)] += m.delta_flux[size_t(i)];
	}
	m.tick++;
}

// GameDaemon::process_all @732700 (frame-spec section 5).
void game_daemon_process_all(Game& g) {
	GameDaemon& d = g.daemon;
	if (d.busy) d.busy--;
	for (int32_t& r : d.repaths) {
		r /= 2;
		if (r < 3) r = 0;
	}
	game_daemon_process_victory(g);  // GameDaemon::process_victory (capture-spec 7)
	if (g.frame % 200 == 0) calc_danger(g);       // GameDaemon::calc_danger (ai-spec 0; the AI danger map)
	if (g.frame % 100 == 33) update_all_seen(g);  // the fog refresh (fog-spec 5.2)
	calc_markets(g);  // draws game_random from frame 0 (economy-spec 5.2, M1-M3)
	for (int32_t i = 0; i < 64; i++) {
		Region& r = g.regions.r[size_t(i)];
		r.flags &= ~0x10;
		if (r.flags & 0x20) {
			r.flags &= ~0x20;
			r.flags |= 0x10;
		}
	}
	check_borders(g);  // territory cursors (production-spec 5.10; the influence algorithm is TODO)
	g.world.process_coll_blocks();
	groups_process(g);
}

}  // namespace

void Game::do_frame() {
	// TurnControl::do_frame returns 0 once Game::check_victory has ended the game (semaphore byte0 & 0x40).
	if (game_over) return;
	// 1-4 presentation; 3 PLAYER_SPEED is a seat-local command (not needed for local play).
	// 5-6 the scenario script and the general powers script (sim/script_host.hpp). TODO(frame-spec 3): Conquer the
	// World reinforcements.
	if (scripts) scripts->run_frame_scripts();
	// 8 Leaders::process_all @6ed2a0, per ACTIVE leader 0..7 in order: the per-frame counters (pop_issues +0x7e8,
	// retarget_budget +0x9f4), Leader::gather (economy-spec 2.2), then the CALC_WALL_STATS / CALC_UNIT_STATS
	// recalculations (production-spec 7.7). TODO(frame-spec 4): flag 0x40000 (at war with a 0x20000 leader),
	// the taunts. Then process_elimination and the clocks (capture-spec 6, 7). LEADER_IDLE_PEASANT is cleared at the end
	// of each iteration.
	for (int32_t who = 0; who < 8; who++) {
		Leader& l = leaders[size_t(who)];
		if (!(l.flags & leaderflag::kActive)) continue;
		l.pop_issues = 0;
		l.retarget_budget = 0;
		leader_gather(*this, who);
		if (l.flags & leaderflag::kCalcWallStats) {
			l.flags &= ~leaderflag::kCalcWallStats;
			calc_wall_stats(*this, who);
		}
		if (l.flags & leaderflag::kCalcUnitStats) {
			l.flags &= ~leaderflag::kCalcUnitStats;
			calc_unit_stats(*this, who);
		}
		leader_process_elimination(*this, who);  // 4 (capture-spec 6.6)
		leader_step_timers(*this, who);          // 5
		// 6 TODO(frame-spec 4): taunts.
		l.flags &= ~leaderflag::kIdlePeasant;
	}
	// 9 TODO: rush-rules war declaration.
	if (!editor) leaders_strategy_all(*this);  // 10 (sim/ai.hpp)
	game_daemon_process_all(*this);  // 11
	if (!editor) armies_process_all(*this);  // 12 (sim/ai.hpp)
	objects_process_all(*this);  // 13
	objects_inc_time(*this);     // 14
	// 15-18 TODO: GraphicEvents::process, Leaders::end_process_all, Achieve::capture_data, process_event_frame.
	frame++;  // 19
	// 20 OrdersMemManager::cycle: nothing to do (orders are values here).
	// 21 TODO: Roads::scan_and_kill_stray_roads.
	if (frame % 15 == 0) tick++;  // 22 (check_cannon_time: TODO)
}

}  // namespace ron::sim
