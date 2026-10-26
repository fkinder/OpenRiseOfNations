#include "net/playback.hpp"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <set>
#include <tuple>

#include "net/lobby.hpp"
#include "sim/datawalk.hpp"
#include "sim/guys.hpp"
#include "sim/datawalk_engine.hpp"
#include "sim/original_map.hpp"
#include "sim/mountains.hpp"
#include "sim/original_save.hpp"
#include "sim/replay.hpp"
#include "sim/save.hpp"

namespace ron::net {

namespace dw = ron::sim::datawalk;

namespace {

bool is_drop_package(const sim::ReplayPackage& pk) {
	// the clear-text drop package of DropControl::process_drops has stamp 0 and is not a player's package
	return pk.plain.size() <= 4 && !pk.plain.empty() && pk.plain[0] == 0x50 && pk.stamp == 0;
}

bool automatic_verb(int op) { return op == sim::op::kCheckSums || op == sim::op::kTurnData || op == sim::op::kCamera; }

}  // namespace

// Rules per setup of the original's saves, loaded once from the install (apps/sav/main.cpp keeps the same cache).
struct Playback::RulesCache {
	explicit RulesCache(const ron::GameFiles& f) : files(f) {}
	const ron::GameFiles& files;
	std::unique_ptr<sim::Rules> base;
	std::map<std::string, std::unique_ptr<sim::Rules>> by_setup;
	const sim::Rules& get(const sim::GameSetup& s) {
		if (!base) base = std::make_unique<sim::Rules>(sim::load_rules(files));
		char key[96];
		std::snprintf(key, sizeof key, "%d/%d/%d/%d/%d", s.starting_technology, s.starting_technology2, s.ending_technology, s.game_rules, int(s.flag8));
		auto it = by_setup.find(key);
		if (it != by_setup.end()) return *it->second;
		auto r = std::make_unique<sim::Rules>(*base);
		sim::start_game(*r, s);
		return *by_setup.emplace(key, std::move(r)).first->second;
	}
};

// ---------------------------------------------------------------- the summary

bool playback_summary(const std::string& path, PlaybackSummary& out) {
	out = PlaybackSummary();
	out.path = path;
	const size_t slash = path.find_last_of('/');
	out.file = slash == std::string::npos ? path : path.substr(slash + 1);
	sim::Replay rp;
	std::string err;
	if (!sim::read_replay(path, rp, err)) {
		out.error = err;
		return true;
	}
	out.ok = true;
	out.multiplayer = rp.header.multiplayer;
	for (const sim::ReplayPlayer& p : rp.header.players) {
		if (!p.present()) continue;
		++out.players;
		out.humans += (p.flags & 4) != 0;
		out.tribes.push_back(int(p.tribe));
		out.seat_who.push_back(int(p.who));
	}
	const std::string sc = sim::to_utf8(rp.header.scenario_path);
	const bool native = rp.native_save || (!rp.has_save && sim::is_random_scenario(sc));
	out.original = !native;
	sim::RandomGameSetup rs;
	if (!rp.native_save && sim::is_random_scenario(sc) && sim::parse_random_setup(sc, rs)) {
		out.map_style = rs.map_style, out.map_size = rs.map_size;
	} else if (rp.native_save) {
		const sim::SaveInfo info = sim::read_save_info(rp.native_save_bytes());
		if (sim::is_random_scenario(info.scenario) && sim::parse_random_setup(info.scenario, rs)) out.map_style = rs.map_style, out.map_size = rs.map_size;
		out.ctw_battle = sim::is_ctw_map_scenario(info.scenario) || (sim::is_random_scenario(info.scenario) && rs.ctw);
		sim::CtwMapBattle cmb;
		if (sim::is_ctw_map_scenario(info.scenario) && sim::parse_ctw_map_battle(info.scenario, cmb)) {
			// "conquest/Alexander/latium.scx": the campaign is the folder, the map the file's name
			std::string m = cmb.map;
			std::replace(m.begin(), m.end(), '\\', '/');
			while (m.compare(0, 2, "./") == 0) m.erase(0, 2);
			const size_t sl = m.find_last_of('/');
			std::string stem = sl == std::string::npos ? m : m.substr(sl + 1);
			if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".scx") == 0) stem.resize(stem.size() - 4);
			if (!stem.empty()) stem[0] = char(std::toupper((unsigned char)stem[0]));
			out.ctw_map = stem;
			if (sl != std::string::npos && sl > 0) {
				const size_t sl2 = m.find_last_of('/', sl - 1);
				out.ctw_campaign = m.substr(sl2 == std::string::npos ? 0 : sl2 + 1, sl - (sl2 == std::string::npos ? 0 : sl2 + 1));
			}
		}
	} else {
		out.map_style = rp.header.setup_field(sim::kSetMapStyle), out.map_size = rp.header.setup_field(sim::kSetMapSize);
	}
	out.start_frame = rp.start_frame();
	out.end_frame = rp.last_frame();
	out.has_check_sums = rp.check_sum_rows() > 0;
	out.ctw_battle = out.ctw_battle || rp.header.conquest;
	out.recovery = rp.native_save && out.start_frame > 0;
	if (rp.header.conquest && !rp.native_save) out.unplayable = "A Conquer the World battle recording without a start state";
	else if (rp.packages.empty()) out.unplayable = "The recording holds no commands";
	else if (native) out.playable = true;
	else if (!rp.has_save) out.unplayable = "Starts from a random map of the original game (no save inside)";
	else if (!rp.has_save_info) out.unplayable = "The embedded save cannot be read";
	else out.playable = true;
	return true;
}


// ---------------------------------------------------------------- rivers and doobers of a loaded game

namespace {

// World +0x7c: the seed of the generator that made the map (kept in the original save's World record; `Game` does not keep it).
uint32_t saved_world_seed(const sim::Game& g) {
	if (g.original && g.original->tree) {
		const dw::Description& d = dw::Description::get();
		const int gid = d.find_global("world");
		const dw::Rec* r = gid >= 0 ? g.original->tree->find_global(gid) : nullptr;
		if (r) return uint32_t(r->read_int(0x7c, 4, false));
	}
	return g.seed;
}

// How many cosmetic doobers a loaded map gets. The generator counts the clumps TerrainGroups::add_doobers placed (bushes on the south /
// east edge of forest wcoords, rocks beside mountains) and only that count reaches the renderer; a save keeps neither. The count is
// estimated from the same edges the generator walks: a least-squares fit over twenty generated worlds (styles 6 - 20, two sizes) gives
// 2.0 per bush edge and 0.72 per rock edge (mean error 13 %, worst 35 %; the doobers are decoration).
int estimate_doober_count(const sim::World& w) {
	int bush_edges = 0, rock_edges = 0;
	for (int x = 1; x <= w.xs - 2; ++x) {
		for (int y = 1; y <= w.ys - 2; ++y) {
			const sim::WData& d = w.w(x, y);
			if (d.flags & 0x1000) continue;
			if (d.flags & 0x20) {
				bush_edges += (y + 1 < w.ys && !(w.w(x, y + 1).flags & 0x20)) + (x + 1 < w.xs && !(w.w(x + 1, y).flags & 0x20));
			} else if (!sim::World::is_ocean(d) && !(d.flags & 0x38)) {
				rock_edges += ((w.w(x - 1, y).flags & 0x10) != 0) || ((w.w(x, y - 1).flags & 0x10) != 0);
			}
		}
	}
	return (20 * bush_edges + 7 * rock_edges) / 10;
}

}  // namespace

// A save keeps the river tiles (tdata 0x800: the 3 x 3 tiles around every sample of the carved trough, a band about seven tiles wide) and the
// dug heights, not the polylines the generator's river list holds. The centre line is recovered from the band: thin the tile mask to a
// one tile skeleton (Zhang-Suen), take the longest path of each connected piece, put it in wcoords (the generator's polylines are wcoord
// paths) and orient it source to mouth (the mouth end is the one with open water around it, then the lower one). The end at the coast is
// continued to the nearest water wcoord, as the generator's path ends on the water cell (`ensure_ocean_end` finds the same cell).
std::vector<std::vector<std::array<int32_t, 2>>> playback_rivers(const sim::Game& g) {
	std::vector<std::vector<std::array<int32_t, 2>>> out;
	const sim::World& w = g.world;
	const int xs = w.xs, ys = w.ys, txs = xs * 4, tys = ys * 4;
	if (xs <= 0 || ys <= 0 || w.tdata.size() != size_t(txs) * size_t(tys)) return out;
	std::vector<uint8_t> m(size_t(txs) * size_t(tys), 0);
	size_t count = 0;
	for (size_t i = 0; i < m.size(); ++i) {
		m[i] = (w.tdata[i] & sim::tmask::kRiverB) != 0;
		count += m[i];
	}
	if (count < 8) return out;
	// Zhang-Suen thinning (the border row / column of the grid never holds a skeleton pixel)
	auto at = [&](int x, int y) -> int { return x < 0 || y < 0 || x >= txs || y >= tys ? 0 : m[size_t(y) * size_t(txs) + size_t(x)]; };
	std::vector<size_t> del;
	for (bool changed = true; changed;) {
		changed = false;
		for (int pass = 0; pass < 2; ++pass) {
			del.clear();
			for (int y = 0; y < tys; ++y) {
				for (int x = 0; x < txs; ++x) {
					if (!at(x, y)) continue;
					const int p2 = at(x, y - 1), p3 = at(x + 1, y - 1), p4 = at(x + 1, y), p5 = at(x + 1, y + 1), p6 = at(x, y + 1), p7 = at(x - 1, y + 1),
					          p8 = at(x - 1, y), p9 = at(x - 1, y - 1);
					const int nb = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
					if (nb < 2 || nb > 6) continue;
					const int seq[9] = {p2, p3, p4, p5, p6, p7, p8, p9, p2};
					int trans = 0;
					for (int k = 0; k < 8; ++k) trans += seq[k] == 0 && seq[k + 1] == 1;
					if (trans != 1) continue;
					if (pass == 0 ? (p2 * p4 * p6 || p4 * p6 * p8) : (p2 * p4 * p8 || p2 * p6 * p8)) continue;
					del.push_back(size_t(y) * size_t(txs) + size_t(x));
				}
			}
			for (size_t i : del) m[i] = 0;
			changed = changed || !del.empty();
		}
	}
	// the skeleton's pieces: BFS from any pixel to the farthest one, again from there, and the path between is the river
	std::vector<int32_t> par(m.size(), -2), dist(m.size(), 0);
	std::vector<uint8_t> seen(m.size(), 0);
	std::vector<int32_t> order;
	auto bfs = [&](int32_t start, std::vector<int32_t>& visited) -> int32_t {
		visited.clear();
		visited.push_back(start);
		par[size_t(start)] = -1, dist[size_t(start)] = 0;
		int32_t far = start;
		for (size_t h = 0; h < visited.size(); ++h) {
			const int32_t c = visited[h];
			const int cx = c % txs, cy = c / txs;
			if (dist[size_t(c)] > dist[size_t(far)]) far = c;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					if ((dx || dy) && at(cx + dx, cy + dy)) {
						const int32_t n = (cy + dy) * txs + cx + dx;
						if (par[size_t(n)] != -2) continue;
						par[size_t(n)] = c, dist[size_t(n)] = dist[size_t(c)] + 1;
						visited.push_back(n);
					}
				}
			}
		}
		return far;
	};
	auto reset = [&](const std::vector<int32_t>& v) {
		for (int32_t c : v) par[size_t(c)] = -2;
	};
	std::vector<int32_t> v1, v2;
	for (int32_t c = 0; c < int32_t(m.size()); ++c) {
		if (!m[size_t(c)] || seen[size_t(c)]) continue;
		const int32_t a = bfs(c, v1);
		for (int32_t k : v1) seen[size_t(k)] = 1;
		reset(v1);
		const int32_t b = bfs(a, v2);
		std::vector<int32_t> chain;   // pixel indexes from b back to a
		for (int32_t k = b; k >= 0; k = par[size_t(k)]) chain.push_back(k);
		reset(v2);
		if (chain.size() < 6) continue;   // a speck: under two wcoords long
		std::vector<std::array<int32_t, 2>> pts;
		for (int32_t k : chain) {
			const std::array<int32_t, 2> p{(k % txs) >> 2, (k / txs) >> 2};
			if (pts.empty() || pts.back() != p) pts.push_back(p);
		}
		if (pts.size() < 2) continue;
		// the mouth end: the one with more open water around it (cells off the map count as water), the lower one on a tie
		auto wet = [&](const std::array<int32_t, 2>& p) {
			int n = 0;
			for (int dy = -4; dy <= 4; ++dy)
				for (int dx = -4; dx <= 4; ++dx) {
					const int x = p[0] + dx, y = p[1] + dy;
					n += x < 0 || y < 0 || x >= xs || y >= ys || w.is_ocean(x, y);
				}
			return n;
		};
		auto low = [&](const std::array<int32_t, 2>& p) {
			const int vx = std::min(p[0] * 4 + 2, g.heights.vxs - 1), vy = std::min(p[1] * 4 + 2, g.heights.vys - 1);
			return g.heights.h.empty() ? 0.0f : g.heights.h[size_t(vy) * size_t(g.heights.vxs) + size_t(vx)];
		};
		const int wf = wet(pts.front()), wb = wet(pts.back());
		if (wf > wb || (wf == wb && low(pts.front()) < low(pts.back()))) std::reverse(pts.begin(), pts.end());
		// continue the mouth to the nearest water wcoord ahead of the path
		const std::array<int32_t, 2> e = pts.back(), p0 = pts[pts.size() >= 4 ? pts.size() - 4 : 0];
		if (!w.is_ocean(e[0], e[1])) {
			const float dxn = float(e[0] - p0[0]), dyn = float(e[1] - p0[1]);
			float best = 1e9f;
			std::array<int32_t, 2> bp{-1, -1};
			for (int dy = -3; dy <= 3; ++dy) {
				for (int dx = -3; dx <= 3; ++dx) {
					const int x = e[0] + dx, y = e[1] + dy;
					if (x < 0 || y < 0 || x >= xs || y >= ys || !w.is_ocean(x, y)) continue;
					if (float(dx) * dxn + float(dy) * dyn < 0) continue;
					const float d = float(dx * dx + dy * dy);
					if (d < best) best = d, bp = {x, y};
				}
			}
			if (bp[0] >= 0) pts.push_back(bp);
		}
		out.push_back(std::move(pts));
	}
	return out;
}

// ---------------------------------------------------------------- the renderer's terrain of a loaded game

bool playback_scenario(const sim::Game& g, const ron::GameFiles& files, Scenario& out, std::string& err) {
	if (!g.original || g.original->tileset.empty()) {
		err = "the game names no tileset";
		return false;
	}
	const sim::World& w = g.world;
	const int xs = w.xs, ys = w.ys;
	Terrain& t = out.terrain;
	t = Terrain();
	t.tileset = g.original->tileset;
	t.xs = xs, t.ys = ys;
	const int txs = xs * 4, tys = ys * 4;
	if (xs <= 0 || ys <= 0 || g.heights.vxs != t.vxs() || g.heights.vys != t.vys() || g.heights.h.size() != size_t(t.vxs()) * size_t(t.vys())) {
		err = "the loaded game holds no height field for its map";
		return false;
	}
	t.heights = g.heights.h;
	t.wcoords.resize(size_t(xs) * size_t(ys));
	t.coord_flags.resize(size_t(xs) * size_t(ys));
	// the same flag translation a generated random map gets (newgame.cpp synthesize_terrain)
	for (int y = 0; y < ys; y++) {
		for (int x = 0; x < xs; x++) {
			const sim::WData& d = w.w(x, y);
			WCoordCell c;
			uint16_t f = 0;
			if (d.flags & sim::wflag::kForest) f |= wfile::kForest;
			if (d.flags & sim::wflag::kRock) f |= wfile::kRocks;
			if (d.flags & sim::wflag::kCoast) f |= wfile::kCoast;
			if (d.flags & sim::wflag::kRiver) f |= wfile::kRiver;
			if (d.flags & sim::wflag::kOil) f |= 0x10;
			if (d.flags & sim::wflag::kCliff) f |= 0x40;
			if (d.flags & sim::wflag::kHalfLand) f |= wfile::kHalfWater;
			c.flags = f;
			c.land = (d.land == 1 || d.land == 2) ? uint8_t(d.land) : uint8_t(0);
			c.land_sub = d.land_sub;
			t.wcoords[size_t(y) * size_t(xs) + size_t(x)] = c;
			uint16_t cf = 0;
			if (d.flags & sim::wflag::kHalfLand) cf = 0xa08d;
			else if (d.land == 2) cf = 0x3009;
			else if (d.land == 1) cf = 0x3089;
			else if (d.flags & sim::wflag::kRiver) cf = 0x11;
			else if (d.flags & sim::wflag::kCoast) cf = 0x4;
			t.coord_flags[size_t(y) * size_t(xs) + size_t(x)] = cf;
		}
	}
	t.tcoord_mask.assign(size_t(txs) * size_t(tys), 0);
	for (int ty = 0; ty < tys; ty++) {
		for (int tx = 0; tx < txs; tx++) {
			const uint16_t m = w.t(tx, ty);
			uint16_t f = 0;
			if ((m & sim::tmask::kMedium) == sim::tmask::kWater) f |= tfile::kWater;
			if (m & sim::tmask::kCoastal) f |= tfile::kCoastal;
			if (m & sim::tmask::kRiverB) f |= tfile::kRiver;
			t.tcoord_mask[size_t(ty) * size_t(txs) + size_t(tx)] = f;
		}
	}
	// the placed mountain ranges (Game::mountains, rebuilt from the saved stamps) as chunk-69 stamps
	sim::MountainTemplates templates(files);
	for (const sim::PlacedRange& pr : g.mountains) {
		MountainStamp st;
		st.wx = uint8_t(pr.wx), st.wy = uint8_t(pr.wy), st.range_index = pr.range;
		const sim::MountainRange* mr = pr.range >= 0 ? templates.get(pr.range) : nullptr;
		st.num_coords = uint8_t(mr ? mr->face_wcoords : int32_t(pr.solid.size()));
		t.mountains.push_back(st);
	}
	// what the generator's polylines and doober list give a random map (the save keeps neither): the rivers from the river tiles, the doobers
	// from the generator's own hash placement with the seed the save kept
	t.rivers = playback_rivers(g);
	place_land_doobers(w, saved_world_seed(g), estimate_doober_count(w), t);
	out.name = "Recording of the original game";
	return true;
}


// ---------------------------------------------------------------- the setup of an original random game

namespace {

// GameInfo +0x... setup bytes the engine's GameInfo does not hold as such are read from the header by index.
int hs(const sim::ReplayHeader& h, sim::ReplaySetupField f) { return h.setup_field(f); }

}  // namespace

bool original_random_setup(const sim::Replay& rp, sim::RandomGameSetup& out, std::string& err) {
	const sim::ReplayHeader& h = rp.header;
	if (rp.has_save || rp.native_save) {
		err = "the recording has an embedded save";
		return false;
	}
	if (!h.scenario_path.empty()) {
		err = "a scenario file game (the map comes from the .scx)";
		return false;
	}
	if (h.conquest) {
		err = "a Conquer the World battle";
		return false;
	}
	std::vector<const sim::ReplayPlayer*> present;
	for (const sim::ReplayPlayer& p : h.players)
		if (p.present()) present.push_back(&p);
	if (present.empty() || present.size() > 8) {
		err = "the header holds no players";
		return false;
	}
	if (rp.random_info.entries.size() < present.size()) {
		err = "no random-game-info block for the header's players";
		return false;
	}
	sim::RandomGameSetup s;
	s.map_style = hs(h, sim::kSetMapStyle);
	s.map_size = hs(h, sim::kSetMapSize);
	s.seed = h.seed;
	s.num_nations = int(present.size());
	s.starting_town = hs(h, sim::kSetStartingTown);
	s.starting_resources = hs(h, sim::kSetStartingResources);
	s.starting_technology = hs(h, sim::kSetStartingTechnology);
	s.ending_technology = hs(h, sim::kSetEndingTechnology);
	s.victory = hs(h, sim::kSetVictory);
	s.elimination = hs(h, sim::kSetElimination);
	s.score_goal = hs(h, sim::kSetScoreGoal);
	s.time_limit = hs(h, sim::kSetTimeLimit);
	s.wonderwin = hs(h, sim::kSetWonderWin);
	s.popwin = hs(h, sim::kSetPopWin);
	s.econwin = hs(h, sim::kSetEconWin);
	s.pop_limit = hs(h, sim::kSetPopLimit);
	s.tech_cost = hs(h, sim::kSetTechCost);
	s.rush_rules = hs(h, sim::kSetRushRules);
	s.reveal_map = hs(h, sim::kSetRevealMap);
	s.game_rules = hs(h, sim::kSetGameRules);
	s.game_speed = hs(h, sim::kSetGameSpeed);
	s.no_tribe_powers = (h.flags & 4) != 0;
	// The start locations are shuffled in a multiplayer game (Map::randomize_starts: the host's seat gets no fixed start; evidence: the two
	// start cities of the first segment of the 2026.08.30 chain sit at the swapped starts of the unshuffled order)
	s.randomize_starts = h.multiplayer;
	if (std::getenv("RON_RANDSTARTS")) s.randomize_starts = std::atoi(std::getenv("RON_RANDSTARTS")) != 0;
	// the seats: the leaders (`who`) in order; the entry of the random-game-info block of the player is its resolved team and nation
	std::vector<size_t> order(present.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = i;
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return present[a]->who < present[b]->who; });
	for (size_t k = 0; k < order.size(); ++k) {
		const sim::ReplayPlayer& p = *present[order[k]];
		const sim::ReplayRandomInfo::Entry& e = rp.random_info.entries[order[k]];
		sim::NationSetup& n = s.nation[k];
		n.tribe = e.leader_value >= 0 && e.leader_value < 24 ? e.leader_value : (p.tribe < 24 ? int(p.tribe) : -1);
		n.team = e.team >= 0 && e.team < 4 ? int(e.team) : -1;
		n.human = (p.flags & 4) != 0;
		n.difficulty = hs(h, sim::kSetDifficulty);
		n.color = int(k);
	}
	out = s;
	return true;
}

sim::RandomGameSetup setup_from_game(const sim::Game& g) {
	sim::RandomGameSetup s;
	const sim::GameInfo& gi = g.info;
	s.map_style = gi.map_style;
	s.map_size = 0;
	s.seed = g.seed;
	s.starting_town = gi.starting_town;
	s.starting_resources = gi.starting_resources;
	s.starting_technology = gi.starting_technology;
	s.ending_technology = gi.ending_technology;
	s.victory = gi.victory;
	s.elimination = gi.elimination;
	s.score_goal = gi.score_goal;
	s.time_limit = gi.time_limit;
	s.wonderwin = gi.wonderwin;
	s.popwin = gi.popwin;
	s.econwin = gi.econwin;
	s.pop_limit = gi.pop_limit;
	s.tech_cost = gi.tech_cost;
	s.rush_rules = gi.rush_rules;
	s.reveal_map = gi.reveal_map;
	s.game_rules = gi.game_rules;
	s.no_tribe_powers = (gi.flags & 4) != 0;
	int n = 0;
	for (int i = 0; i < 8; ++i) {
		const sim::Leader& l = g.leaders[size_t(i)];
		if (!(l.flags & sim::leaderflag::kValid)) continue;
		sim::NationSetup& ns = s.nation[size_t(n)];
		ns.tribe = l.tribe;
		ns.team = gi.team[size_t(i)] >= 0 && gi.team[size_t(i)] < 4 ? int(gi.team[size_t(i)]) : -1;
		ns.human = (l.flags & sim::leaderflag::kHuman) != 0;
		ns.difficulty = l.difficulty;
		ns.color = l.color;
		++n;
	}
	s.num_nations = n;
	return s;
}

sim::GameSetup original_rules_setup(const sim::ReplayHeader& h) {
	sim::GameSetup setup;
	setup.starting_technology = hs(h, sim::kSetStartingTechnology);
	setup.starting_technology2 = hs(h, sim::kSetStartingTechnology2);
	setup.ending_technology = hs(h, sim::kSetEndingTechnology);
	setup.game_rules = hs(h, sim::kSetGameRules);
	setup.flag8 = (h.flags & 8) != 0;
	return setup;
}

void apply_original_lobby(sim::Game& g, const sim::ReplayHeader& h) {
	sim::GameInfo& gi = g.info;
	gi.starting_resources2 = uint8_t(hs(h, sim::kSetStartingResources2));
	gi.starting_technology2 = hs(h, sim::kSetStartingTechnology2);
	gi.chairs = uint8_t(hs(h, sim::kSetChairs));
	gi.map_style = uint8_t(hs(h, sim::kSetMapStyle));
	gi.team_style = uint8_t(hs(h, sim::kSetTeamStyle));
	gi.flags = h.flags;
	// GameInfo.player[slot]: flags and the lobby's team byte as the header holds them (8 = no team)
	for (const sim::ReplayPlayer& p : h.players) {
		if (!p.present()) continue;
		gi.player_flags[size_t(p.slot & 7)] = p.flags;
		gi.team[size_t(p.slot & 7)] = p.team;
	}
}

namespace {

uint64_t shadow_mix_key(int32_t step, int64_t base, int64_t index) {   // datawalk.cpp mix_key (the key of a recorded event)
	uint64_t h = uint64_t(uint32_t(step)) * 0x9E3779B97F4A7C15ull;
	h ^= (uint64_t(base) + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
	h = (h << 29) | (h >> 35);
	h ^= (uint64_t(index) + 0x1234567ull) * 0x165667B19E3779F9ull;
	return h;
}

// Forwards a checksum walk to the engine's provider and records the containers it asks for.
class ProbeProvider : public dw::Provider {
public:
	explicit ProbeProvider(dw::Provider& in) : in_(in) {}
	struct Site3 {
		const dw::Rec* rec;
		int step;
		int64_t base;
	};
	std::vector<Site3> sites;
	void begin_walk() override { in_.begin_walk(); }
	void hold_containers(bool on) override { in_.hold_containers(on); }
	dw::Obj global(int gid) override { return in_.global(gid); }
	const uint8_t* image(dw::Obj o, int64_t off, uint32_t n) override { return in_.image(o, off, n); }
	dw::Obj child(dw::Obj o, const dw::Site& site, int64_t index) override { return in_.child(o, site, index); }
	bool local(dw::Obj o, const dw::Site& site, int64_t& value) override { return in_.local(o, site, value); }
	bool container(dw::Obj o, const dw::Site& site, dw::ContainerHeader& hdr) override {
		sites.push_back({static_cast<const dw::Rec*>(o), site.step, site.base});
		return in_.container(o, site, hdr);
	}
	bool present(dw::Obj o, const dw::Site& site, int64_t i) override { return in_.present(o, site, i); }
	int32_t data_type(dw::Obj o, const dw::Site& site, int64_t i) override { return in_.data_type(o, site, i); }
	dw::Obj element(dw::Obj o, const dw::Site& site, int64_t i) override { return in_.element(o, site, i); }
	const uint8_t* bytes(dw::Obj o, const dw::Site& site, uint64_t n) override { return in_.bytes(o, site, n); }
	const uint8_t* string(dw::Obj o, const dw::Site& site, uint32_t& len) override { return in_.string(o, site, len); }

private:
	dw::Provider& in_;
};

dw::Rec* clone_rec(dw::Tree& tree, const dw::Rec& src) {
	dw::Rec* r = tree.new_rec(src.cls);
	r->origin = src.origin;
	r->bytes = src.bytes;
	r->ev = src.ev;
	return r;
}

}  // namespace

void prepare_fresh_shadow(sim::Game& g) {
	if (!g.original || !g.original->tree) return;
	sim::OriginalShadow& sh = *g.original;
	std::vector<ProbeProvider::Site3> sites;
	{
		sim::EngineProvider e(g);
		ProbeProvider probe(e);
		dw::compute_check_sums(probe);
		sites = probe.sites;
	}
	std::set<std::tuple<const dw::Rec*, int, int64_t>> done;
	for (const ProbeProvider::Site3& st : sites) {
		if (!done.insert(std::make_tuple(st.rec, st.step, st.base)).second) continue;
		auto it = sh.segs.find(st.rec);
		if (it == sh.segs.end()) continue;
		sim::omap::ArraySpec spec;
		bool owned = false;
		for (const sim::omap::Segment& s : it->second) {
			if (!s.map->array_fn) continue;
			if (s.len >= 0 && (st.base < s.base || st.base >= s.base + s.len)) continue;
			if (s.map->array_fn(g, s.id, st.step, st.base, spec)) { owned = true; break; }
		}
		if (!owned) {
			if (std::getenv("RON_PREPARE_TRACE")) std::printf("  site %s step %d base %lld: not engine-owned\n", dw::Description::get().function_name(dw::Description::get().step_function(st.step)), st.step, (long long)st.base);
			continue;
		}
		dw::Rec* rec = const_cast<dw::Rec*>(st.rec);
		dw::ContainerData* cd = nullptr;
		const uint64_t key = shadow_mix_key(st.step, st.base, 0);
		for (dw::Event& e : rec->ev)
			if (e.type == 2 && e.step == st.step && e.key == key && e.cd) { cd = e.cd; break; }
		if (!cd) continue;
		const int32_t want = spec.count;
		auto has = [&](int32_t i) -> bool {
			if (spec.present) return spec.present(i);
			if (spec.extra_present) return spec.extra_present(i);
			if (spec.child && spec.child_id && spec.child->resolve) return spec.child->resolve(g, spec.child_id(i)) != nullptr;
			return true;
		};
		const bool by_engine = bool(spec.present) || bool(spec.extra_present);
		dw::Rec* proto = nullptr;
		for (dw::Rec* c : cd->elems)
			if (c) { proto = c; break; }
		std::vector<dw::Rec*> elems(size_t(std::max(want, 0)), nullptr);
		std::vector<uint8_t> present(elems.size(), 0);
		std::vector<int32_t> types(elems.size(), 0);
		const dw::ContainerKind ck = dw::Description::get().container_kind(st.step);
		const bool ptr_kind = ck == dw::ContainerKind::PtrArray || ck == dw::ContainerKind::MultiPtrArray;
		for (int32_t i = 0; i < want; ++i) {
			const bool h = has(i);
			dw::Rec* old = size_t(i) < cd->elems.size() ? cd->elems[size_t(i)] : nullptr;
			if (h) elems[size_t(i)] = old ? old : (proto ? clone_rec(*sh.tree, *proto) : nullptr);
			present[size_t(i)] = by_engine ? (h ? 1 : 0) : 1;
			types[size_t(i)] = spec.dtype && h ? spec.dtype(i) : 0;
		}
		if (std::getenv("RON_PREPARE_TRACE")) {
			int hv = 0;
			for (int32_t i = 0; i < want; ++i) hv += has(i);
			std::printf("  site %s step %d base %lld: engine count %d (has %d), template length %d size %d elems %zu%s%s\n", dw::Description::get().function_name(dw::Description::get().step_function(st.step)), st.step,
			            (long long)st.base, want, hv, cd->length, cd->size, cd->elems.size(), spec.grow_only ? " grow_only" : "", spec.keep_size ? " keep_size" : "");
		}
		// Good / Item: SubObject +0xc (z_internal, XOR 0x63637; not modelled by the engine) is TerrainOut::find_data_z of the position, flag 1 (water and
		// negative heights give 0): the chain's 131 goods and 10 items all agree with it
		if (spec.child && (spec.child->name == "Good" || spec.child->name == "Item") && spec.child->resolve) {
			for (int32_t i = 0; i < want; ++i) {
				if (!elems[size_t(i)]) continue;
				const sim::GoodObj* o = static_cast<const sim::GoodObj*>(spec.child->resolve(g, spec.child_id(i)));
				if (!o) continue;
				const int32_t z = sim::find_data_z(g, o->x, o->y, 1) ^ 0x63637;
				elems[size_t(i)]->write(0xc, reinterpret_cast<const uint8_t*>(&z), 4);
				const uint8_t who = 0xff;   // SubObject +9 who = -1 and +0xa o = the list index (u16): the bytes before z_internal of every good / item
				const uint16_t oi = uint16_t(i);
				elems[size_t(i)]->write(9, &who, 1);
				elems[size_t(i)]->write(0xa, reinterpret_cast<const uint8_t*>(&oi), 2);
			}
		}
		cd->length = want;
		cd->elems = std::move(elems);
		if (ptr_kind) {
			cd->present = std::move(present);
			if (ck == dw::ContainerKind::MultiPtrArray) cd->types = std::move(types);
		}
	}
}

void clear_shadow_containers(sim::OriginalShadow& sh) {
	if (!sh.tree) return;
	const dw::Description& d = dw::Description::get();
	std::set<const dw::Rec*> seen;
	std::vector<dw::Rec*> todo;
	for (int gid = 0; gid < d.global_count(); ++gid)
		if (const dw::Rec* r = sh.tree->find_global(gid)) todo.push_back(const_cast<dw::Rec*>(r));
	while (!todo.empty()) {
		dw::Rec* r = todo.back();
		todo.pop_back();
		if (!r || !seen.insert(r).second) continue;
		for (dw::Event& e : r->ev) {
			if (e.type == 0 && e.child) todo.push_back(e.child);
			if (e.type != 2 || !e.cd) continue;
			dw::ContainerData& cd = *e.cd;
			bool any = false;
			for (dw::Rec* c : cd.elems) any = any || c != nullptr;
			if (!any) continue;
			cd.length = 0;   // the header's capacity (size / increment / flags) stays what the template had
			cd.present.clear();
			cd.types.clear();
			cd.elems.clear();
		}
	}
}

namespace {

// The checksum provider (sim/datawalk_engine.hpp) lays the engine's state over a decoded save of the original (the "shadow": the members the
// engine does not model, the padding, the container headers). A generated game has none, so a fresh-state save of the corpus serves as the
// template: `Saves/current 2.SVX` (frame 189 of a game that has just started) next to the "Recorded Games" folder of the recording, or the
// file named by RON_SHADOW_SAVE. Everything the engine models is overlaid from the generated game; only never-modelled members stay the
// template's.
std::string template_save_path(const std::string& replay_path) {
	if (const char* e = std::getenv("RON_SHADOW_SAVE")) {
		if (*e) return e;
	}
	// the embedded save of the first segment of the 2026.08.30 chain is a game two frames after its start (fresh scenario data, fresh object
	// lists); a save of the Saves folder is the fallback
	const size_t k = replay_path.find("Recorded Games");
	const std::string root = k != std::string::npos ? replay_path.substr(0, k) : std::string("re/oracle/");
	const std::string chain = root + "Recorded Games/Multi/Playback - 2026.08.30 21'23'34 (Sun).rcx";
	if (std::FILE* f = std::fopen(chain.c_str(), "rb")) {
		std::fclose(f);
		return chain;
	}
	return root + "Saves/current 2.SVX";
}

std::shared_ptr<sim::OriginalShadow> load_template_shadow(const ron::GameFiles& files, const sim::Rules& rules, const std::string& path, std::string& err) {
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) {
		err = "cannot read the template save " + path + " (the checksum shadow of a generated game)";
		return nullptr;
	}
	std::vector<uint8_t> raw;
	uint8_t buf[1 << 16];
	for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) raw.insert(raw.end(), buf, buf + n);
	std::fclose(f);
	std::vector<uint8_t> bytes;
	bool gz = false;
	if (path.size() > 4 && path.compare(path.size() - 4, 4, ".rcx") == 0) {
		// a recording: its embedded save is the template
		sim::Replay trp;
		if (!sim::read_replay_bytes(raw, trp, err) || !trp.has_save) {
			err = "the recording " + path + " holds no embedded save to serve as the template";
			return nullptr;
		}
		bytes = trp.save_bytes();
	} else if (!sim::replay_gunzip(raw, bytes, gz)) {
		err = "cannot inflate the template save " + path;
		return nullptr;
	}
	sim::OriginalLoadReport rep;
	std::unique_ptr<sim::Game> gt = sim::load_original_save(files, rules, nullptr, bytes, &rep);
	if (!gt) {
		err = "the template save does not load: " + rep.error;
		return nullptr;
	}
	return gt->original;
}

// Trees decoded from the rules blocks of recordings (their nodes are referenced by the shadows they were transplanted into).
std::vector<std::unique_ptr<dw::Tree>>& kept_trees() {
	static std::vector<std::unique_ptr<dw::Tree>> v;
	return v;
}

// The recording's own rules block (Game::walk_rules_data; replay-spec section 6) replaces the template's: the `rules` part hashes the image the
// game was played with, which is the data revision of the recording (two revisions in the corpus), not the template's.
bool transplant_rules(const sim::Replay& rp, sim::OriginalShadow& sh) {
	if (!rp.has_rules || rp.rules_at == 0) return false;
	auto tree = std::make_unique<dw::Tree>(dw::Description::get());
	dw::DecodeResult r = dw::decode_function(*tree, rp.data.data(), rp.data.size(), rp.rules_at, "Game::walk_rules_data", "game", 0x10);
	if (std::getenv("RON_PREPARE_TRACE")) std::printf("  rules block of the recording: at %zu size %zu: decode %s end %zu error %s\n", rp.rules_at, rp.rules_size, r.ok ? "ok" : "FAILED", r.end, r.error.c_str());
	if (!r.ok || r.end != rp.rules_at + rp.rules_size) return false;
	const dw::Description& d = dw::Description::get();
	int copied = 0;
	for (int gid = 0; gid < d.global_count(); ++gid) {
		const dw::Rec* src = tree->find_global(gid);
		if (!src || (src->bytes.empty() && src->ev.empty())) continue;
		dw::Rec* dst = sh.tree->global(gid);
		dst->origin = src->origin;
		dst->bytes = src->bytes;
		dst->ev = src->ev;
		dst->pass = 0;
		dst->cursor = 0;
		++copied;
	}
	if (std::getenv("RON_PREPARE_TRACE")) std::printf("  rules block: %d globals replaced in the shadow\n", copied);
	if (!copied) return false;
	kept_trees().push_back(std::move(tree));
	return true;
}

// World's unmodelled members (docs/original-save.md section 6.21): the resolved style, the territory limits, World::analyze_map's counts, the
// generator's seed. They are walked by the `world` part and never change after the start.
void patch_world_shadow(sim::Game& g, const sim::RandomExtras& X) {
	const dw::Description& d = dw::Description::get();
	dw::Rec* rec = const_cast<dw::Rec*>(g.original->tree->find_global(d.find_global("world")));
	if (!rec) return;
	auto put = [&](const char* name, int64_t v) {
		const dw::FieldInfo* f = d.find_field("World", name);
		if (!f) return;
		const int32_t x = int32_t(v);
		rec->write(f->offset, reinterpret_cast<const uint8_t*>(&x), 4);
	};
	const sim::mapgen::Result& r = X.result;
	put("map", r.picked_style);
	put("player_territory_limit", r.territory_limits[0]);
	put("player_territory_limit_civic", r.territory_limits[1]);
	put("player_territory_limit_city", r.territory_limits[2]);
	put("colonized_territory_limit", r.territory_limits[3]);
	put("colonized_territory_limit_civic", r.territory_limits[4]);
	put("colonized_territory_limit_city", r.territory_limits[5]);
	put("player_reg", X.analysis.player_reg);
	put("resource_reg", X.analysis.resource_reg);
	put("forest_size", X.analysis.forest_size);
	put("mountain_size", X.analysis.mountain_size);
	put("rock_size", X.analysis.rock_size);
	put("total_metal", X.analysis.total_metal);
	put("total_oil", X.analysis.total_oil);
	put("goodies", X.analysis.goodies);
	put("land_resources", X.analysis.land_resources);
	put("sea_resources", X.analysis.sea_resources);
	put("seed", r.world_seed);
	// World.start_x / start_y (the start locations in order) and start_city_x / start_city_y (the four wcoords of each start's city site: (x, y),
	// (x - 1, y), (x, y - 1), (x - 1, y - 1), start after start); the oil lists stay empty in every save
	if (dw::Rec* wr = const_cast<dw::Rec*>(g.original->tree->find_global(d.find_global("world")))) {
		int idx = 0;
		for (dw::Event& e : wr->ev) {
			if (e.type != 2 || !e.cd || std::string(d.function_name(d.step_function(e.step))) != "SimpleArray<WCoord>::walk_data") continue;
			std::vector<int32_t> v;
			for (const auto& st : r.starts) {
				if (idx == 0) v.push_back(st[0]);
				else if (idx == 1) v.push_back(st[1]);
				else if (idx == 2) { v.push_back(st[0]); v.push_back(st[0] - 1); v.push_back(st[0]); v.push_back(st[0] - 1); }
				else if (idx == 3) { v.push_back(st[1]); v.push_back(st[1]); v.push_back(st[1] - 1); v.push_back(st[1] - 1); }
			}
			if (idx < 4) {
				dw::ContainerData& cd = *e.cd;
				cd.data.assign(reinterpret_cast<const uint8_t*>(v.data()), reinterpret_cast<const uint8_t*>(v.data()) + v.size() * 4);
				cd.length = int32_t(v.size());
				if (!v.empty()) {
					int32_t size = 4;
					while (size < int32_t(v.size())) size *= 2;
					cd.size = size;
					cd.increment = -1;
					cd.flags = 0;
				}
			}
			++idx;
		}
	}
	// Terrain.halfland_locs / halfland_types / halfland_subtypes (World::walk_data parts 10 - 12): the log of Terrain::mark_halfwater; Array and SimpleArray
	// capacities double from 4 (SimpleArray<int>::increase_size @4220e0 with the default increment)
	if (dw::Rec* tr = const_cast<dw::Rec*>(g.original->tree->find_global(d.find_global("terrain")))) {
		int simple = 0;
		for (dw::Event& e : tr->ev) {
			if (e.type != 2 || !e.cd) continue;
			const std::string fn = d.function_name(d.step_function(e.step));
			dw::ContainerData& cd = *e.cd;
			auto header = [&](size_t n) {
				cd.length = int32_t(n);
				if (!n) return;
				int32_t size = 4;
				while (size < int32_t(n)) size *= 2;
				cd.size = size;
				cd.increment = -1;
				cd.flags = 0;
			};
			if (fn == "Array<WCoordData>::walk_data" && cd.data.empty()) {
				cd.elems.clear();
				for (const auto& l : r.halfland_locs) {
					dw::Rec* w = g.original->tree->new_rec("WCoordData");
					w->write(0, reinterpret_cast<const uint8_t*>(&l[0]), 4);
					w->write(4, reinterpret_cast<const uint8_t*>(&l[1]), 4);
					cd.elems.push_back(w);
				}
				header(cd.elems.size());
			} else if (fn == "SimpleArray<int>::walk_data" && simple < 2) {
				const std::vector<int32_t>& v = simple == 0 ? r.halfland_types : r.halfland_subtypes;
				cd.data.assign(reinterpret_cast<const uint8_t*>(v.data()), reinterpret_cast<const uint8_t*>(v.data()) + v.size() * 4);
				header(v.size());
				++simple;
			} else if (fn == "SimpleArray<int>::walk_data") {
				++simple;
			}
		}
	}
}

// The template (a two-player game) walks only its two active leaders in full; a leader beyond them stores 8 bytes, so a game with more seats finds no
// children, strings or containers where LeaderData::walk_data asks for them. Leader k gets leader 0's image and events at its own base (the bases are
// recovered from the recorded keys), and the same for the object lists of `objects`.
void activate_template_leaders(sim::OriginalShadow& sh, int n) {
	const dw::Description& d = dw::Description::get();
	auto clone_event = [&](const dw::Event& src, uint64_t key) {
		dw::Event e = src;
		e.key = key;
		if (e.child) e.child = clone_rec(*sh.tree, *e.child);
		if (e.cd) {
			dw::ContainerData* cd = sh.tree->new_container();
			*cd = *e.cd;
			for (dw::Rec*& el : cd->elems)
				if (el) el = clone_rec(*sh.tree, *el);
			e.cd = cd;
		}
		return e;
	};
	// the events of a global whose recorded bases lie in [lo, hi): (event index, base, index); filter = the walk function name of the step ("" = any)
	auto recover = [&](dw::Rec& rec, int64_t lo, int64_t hi, const char* fn) {
		struct Ev {
			size_t at;
			int64_t base, index;
		};
		std::vector<Ev> out;
		for (size_t i = 0; i < rec.ev.size(); ++i) {
			const dw::Event& e = rec.ev[i];
			if (*fn && std::string(d.function_name(d.step_function(e.step))) != fn) continue;
			const int64_t index = e.type == 0 ? e.value : 0;
			for (int64_t b = lo; b < hi; ++b)
				if (shadow_mix_key(e.step, b, index) == e.key) {
					out.push_back({i, b, index});
					break;
				}
		}
		return out;
	};
	if (dw::Rec* lr = const_cast<dw::Rec*>(sh.tree->find_global(d.find_global("leaders")))) {
		auto active = [&](int k) {
			uint8_t b = 0;
			lr->read(int64_t(k) * 0x6eec, &b, 1);
			return (b & 1) != 0;
		};
		if (active(0)) {
			auto evs = recover(*lr, 0, 0x6eec, "");
			std::vector<uint8_t> img(0x6eec);
			lr->read(0, img.data(), img.size());
			for (int k = 1; k < n && k < 8; ++k) {
				if (active(k)) continue;
				const int32_t who = k;
				std::memcpy(img.data() + 8, &who, 4);   // LeaderData.who
				lr->write(int64_t(k) * 0x6eec, img.data(), img.size());
				std::vector<dw::Event> add;
				for (const auto& v : evs) add.push_back(clone_event(lr->ev[v.at], shadow_mix_key(lr->ev[v.at].step, v.base + int64_t(k) * 0x6eec, v.index)));
				for (dw::Event& e : add) lr->ev.push_back(e);
			}
		}
	}
	if (dw::Rec* orc = const_cast<dw::Rec*>(sh.tree->find_global(d.find_global("objects")))) {
		auto evs = recover(*orc, 4, 4 + 0x1c, "MultiPtrArray<Object>::walk_data");
		for (int k = 1; k < n && k < 8; ++k) {
			bool have = false;
			for (const auto& v : evs) {
				const uint64_t key = shadow_mix_key(orc->ev[v.at].step, v.base + int64_t(k) * 0x1c, v.index);
				for (const dw::Event& e : orc->ev) have = have || e.key == key;
			}
			if (have) continue;
			std::vector<dw::Event> add;
			for (const auto& v : evs) add.push_back(clone_event(orc->ev[v.at], shadow_mix_key(orc->ev[v.at].step, v.base + int64_t(k) * 0x1c, v.index)));
			for (dw::Event& e : add) orc->ev.push_back(e);
		}
	}
}

// The members of the template that belong to the template's own game rather than to a fresh one.
void patch_shadow_for_setup(sim::Game& g, const sim::Replay& rp) {
	const sim::ReplayHeader& h = rp.header;
	sim::OriginalShadow& sh = *g.original;
	const dw::Description& d = dw::Description::get();
	transplant_rules(rp, sh);   // the block of the stream is the image before any game script edited it (the first row of a no_nukes game; the script's later edits stay unmodelled in the shadow)
	const bool scripted = h.setup_field(sim::kSetScriptType) != 0 || !h.scenario_script.empty();
	if (!scripted) {
		// no game script: ScenarioData::load_scenario_script is 0 (the template's game ran one) and the script runtime holds no script file
		// (RunTimeEnv::walk_data: a bare count; the 262145 of every scriptless multiplayer row)
		if (dw::Rec* sc = const_cast<dw::Rec*>(sh.tree->find_global(d.find_global("ScenarioData::load_scenario_script")))) {
			const int32_t zero = 0;
			sc->write(0, reinterpret_cast<const uint8_t*>(&zero), 4);
		}
		if (dw::Rec* rt = const_cast<dw::Rec*>(sh.tree->find_global(d.find_global("run_time_env")))) {
			for (dw::Event& e : rt->ev)
				if (e.type == 1) { e.value = 0; break; }
		}
	}
}

}  // namespace

std::unique_ptr<sim::Game> original_random_game(const ron::GameFiles& files, const sim::Rules& rules, const sim::AnimSource* art, const sim::Replay& rp,
                                                const sim::RandomGameSetup& setup, std::string& err) {
	sim::RandomExtras extras;
	extras.original = load_template_shadow(files, rules, template_save_path(rp.path), err);
	if (!extras.original) return nullptr;
	std::shared_ptr<sim::OriginalShadow> shadow = extras.original;
	// The game ran on the rules block the recording carries (adopt_rules does the same for a save): two data revisions exist in the corpus, and the
	// `rules` part hashes the engine's tables. The adopted tables are used for the generation too; the Game keeps them as its own.
	std::unique_ptr<sim::Rules> adopted;
	if (rp.has_rules && rp.rules_at != 0) {
		try {
			auto own = std::make_unique<sim::Rules>(rules);
			sim::read_image_into(rp.data, rp.rules_at, *own);
			if (sim::write_image(*own) != sim::write_image(rules)) adopted = std::move(own);
		} catch (const std::exception&) {
		}
	}
	std::unique_ptr<sim::Game> g = sim::new_random_game(files, adopted ? *adopted : rules, art, setup, &extras);
	if (!g) {
		err = extras.error.empty() ? "the generator failed" : extras.error;
		return nullptr;
	}
	if (adopted) {
		g->own_rules = std::move(adopted);
		g->rules = g->own_rules.get();
		g->base_rules = &rules;
	}
	for (int who = 0; who < sim::kNumLeaders; ++who) shadow->leader_ai[who] = who < 8 && (g->leaders[size_t(who)].flags & sim::leaderflag::kValid) != 0;
	apply_original_lobby(*g, rp.header);
	{
		int seats = 0;
		for (int who = 0; who < 8; ++who) seats += (g->leaders[size_t(who)].flags & sim::leaderflag::kValid) != 0 ? 1 : 0;
		activate_template_leaders(*shadow, seats);
	}
	patch_shadow_for_setup(*g, rp);
	patch_world_shadow(*g, extras);
	return g;
}

// ---------------------------------------------------------------- open

std::unique_ptr<Playback> Playback::open(const std::string& path, const std::string& game_dir, std::string& err) {
	std::unique_ptr<Playback> pb(new Playback(game_dir));
	pb->path_ = path;
	if (!sim::read_replay(path, pb->rp_, err)) return nullptr;
	const sim::Replay& rp = pb->rp_;
	const std::string sc = sim::to_utf8(rp.header.scenario_path);
	if (rp.packages.empty()) {
		err = "the recording holds no commands";
		return nullptr;
	}
	pb->seed_ = rp.header.seed;
	pb->ctw_battle_ = rp.header.conquest;
	if (rp.native_save) {
		pb->kind_ = Kind::NativeSave;
		const sim::SaveInfo info = sim::read_save_info(rp.native_save_bytes());
		pb->scenario_ = info.scenario;
		pb->seed_ = info.seed;
		sim::RandomGameSetup rs;
		pb->ctw_battle_ = pb->ctw_battle_ || sim::is_ctw_map_scenario(info.scenario) || (sim::is_random_scenario(info.scenario) && sim::parse_random_setup(info.scenario, rs) && rs.ctw);
	} else if (!rp.has_save && sim::is_random_scenario(sc)) {
		pb->kind_ = Kind::NativeRandom;
		if (!sim::parse_random_setup(sc, pb->setup_)) {
			err = "the header's random setup text does not parse";
			return nullptr;
		}
		pb->scenario_ = sc;
		pb->seed_ = pb->setup_.seed;
	} else {
		pb->kind_ = Kind::Original;
		if (!rp.has_save) {
			// no embedded save: the game starts from the lobby's setup (a random map; the rules block of the stream is the install's rules)
			std::string serr;
			if (!original_random_setup(rp, pb->setup_, serr)) {
				err = "no embedded save and " + serr;
				return nullptr;
			}
			pb->kind_ = Kind::OriginalRandom;
			pb->scenario_ = sim::random_setup_string(pb->setup_);
		} else if (!rp.has_save_info) {
			err = "cannot read the embedded save's game info";
			return nullptr;
		}
	}
	for (int i = 0; i < 8; ++i) pb->who_[size_t(i)] = i;
	for (const sim::ReplayPlayer& p : rp.header.players)
		if (p.present()) pb->who_[size_t(p.play & 7)] = p.who;

	// the package groups: consecutive records of one frame are one turn boundary
	for (size_t i = 0; i < rp.packages.size(); ++i) {
		const sim::ReplayPackage& pk = rp.packages[i];
		if (is_drop_package(pk) || pk.play < 0 || pk.play > 7) continue;
		if (!pb->groups_.empty() && pb->groups_.back().frame == pk.frame && pb->groups_.back().last == i) {
			pb->groups_.back().last = i + 1;
		} else {
			pb->groups_.push_back({pk.frame, i, i + 1});
		}
	}
	if (pb->groups_.empty()) {
		err = "the recording holds no player packages";
		return nullptr;
	}
	// every CHECK_SUMS row: record frame -> the tuple most senders agree on (a fully equal row stands for all of them)
	std::map<int64_t, std::map<std::vector<uint32_t>, int>> rows;
	for (const sim::ReplayPackage& pk : rp.packages)
		for (const sim::ReplayCommand& c : pk.cmds) {
			if (c.op != sim::op::kCheckSums) continue;
			uint32_t v[16];
			if (!sim::replay_check_sums(pk.data(c), c.len, v)) continue;
			++rows[pk.frame][std::vector<uint32_t>(v, v + dw::kCheckPartCount)];
		}
	for (const auto& kv : rows) {
		auto best = std::max_element(kv.second.begin(), kv.second.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
		pb->rows_[kv.first] = best->first;
	}
	return pb;
}

std::vector<Playback::SeatInfo> Playback::seats() const {
	std::vector<SeatInfo> out;
	for (const sim::ReplayPlayer& p : rp_.header.players) {
		if (!p.present()) continue;
		SeatInfo s;
		s.slot = p.play & 7, s.who = p.who, s.tribe = p.tribe, s.team = p.team, s.human = (p.flags & 4) != 0;
		out.push_back(s);
	}
	return out;
}

// ---------------------------------------------------------------- the start game

std::vector<std::string> Playback::take_notes() {
	std::vector<std::string> n;
	n.swap(notes_);
	return n;
}

std::unique_ptr<sim::Game> Playback::make_start(std::string& err, const PlaybackEnv& env, bool with_art) {
	try {
		if (kind_ == Kind::Original) {
			sim::GameSetup setup;
			setup.starting_technology = rp_.save_info.setup_field(sim::kSetStartingTechnology);
			setup.starting_technology2 = rp_.save_info.setup_field(sim::kSetStartingTechnology2);
			setup.ending_technology = rp_.save_info.setup_field(sim::kSetEndingTechnology);
			setup.game_rules = rp_.save_info.setup_field(sim::kSetGameRules);
			setup.flag8 = (rp_.save_info.flags & 8) != 0;
			if (!cache_) cache_ = std::make_shared<RulesCache>(files_);
			const sim::Rules& rules = cache_->get(setup);
			sim::OriginalLoadReport r;
			std::unique_ptr<sim::Game> g = sim::load_original_save(files_, rules, nullptr, rp_.save_bytes(), &r);
			if (!g) {
				err = "load failed: " + r.error;
				return nullptr;
			}
			rules_equal_ = r.rules_equal;
			if (with_art) {
				arts_.push_back(sim::load_anim_source(files_, *g->rules));
				g->anims = arts_.back().get();
			}
			// The playback prologue of ron-sav --replay: what the original's load and Game::run do before the first frame (the pens' animals,
			// leader 9's reset, calc_danger, update_all_seen), and the game script the header names (Setup::build_game's script part).
			sim::original_post_load_effects(*g);
			const std::string script = sim::to_utf8(rp_.header.scenario_script);
			std::string serr;
			if (!script.empty()) {
				if (sim::original_attach_game_script(*g, files_, script, &serr)) notes_.push_back("game script \"" + script + "\" attached");
				else notes_.push_back("game script \"" + script + "\" not attached: " + serr);
			}
			return g;
		}
		if (kind_ == Kind::OriginalRandom) {
			if (!cache_) cache_ = std::make_shared<RulesCache>(files_);
			const sim::Rules& rules = cache_->get(original_rules_setup(rp_.header));
			if (with_art && arts_.empty()) arts_.push_back(sim::load_anim_source(files_, rules));
			std::unique_ptr<sim::Game> g = original_random_game(files_, rules, with_art ? arts_.back().get() : nullptr, rp_, setup_, err);
			if (!g) return nullptr;
			rules_equal_ = true;
			notes_.push_back("random map generated from the header's setup (style " + std::to_string(setup_.map_style) + ", size " + std::to_string(setup_.map_size) +
			                 ", seed " + std::to_string(setup_.seed) + ", " + std::to_string(setup_.num_nations) + " nations)");
			sim::original_post_load_effects(*g);
			const std::string script = sim::to_utf8(rp_.header.scenario_script);
			std::string serr;
			if (!script.empty()) {
				if (sim::original_attach_game_script(*g, files_, script, &serr)) notes_.push_back("game script \"" + script + "\" attached");
				else notes_.push_back("game script \"" + script + "\" not attached: " + serr);
			}
			prepare_fresh_shadow(*g);
			return g;
		}
		const ron::GameFiles& files = env.files ? *env.files : files_;
		const sim::Rules* rules = env.rules;
		if (!rules) {
			if (!own_rules_) {
				own_rules_ = std::make_unique<sim::Rules>(sim::load_rules(files));
				sim::start_game(*own_rules_, sim::GameSetup());
			}
			rules = own_rules_.get();
		}
		const sim::AnimSource* art = env.art;
		if (!art && with_art) {
			if (arts_.empty()) arts_.push_back(sim::load_anim_source(files, *rules));
			art = arts_.back().get();
		}
		rules_equal_ = true;
		std::unique_ptr<sim::Game> g = kind_ == Kind::NativeSave ? sim::load_game_bytes(files, *rules, art, rp_.native_save_bytes())
		                                                         : sim::new_random_game(files, *rules, art, setup_);
		if (!g) {
			err = "the start state could not be built";
			return nullptr;
		}
		if (kind_ == Kind::NativeRandom) apply_header_seats(*g);
		return g;
	} catch (const std::exception& e) {
		err = e.what();
		return nullptr;
	}
}

// The room made a random game a multiplayer game: the header carries the players' names and handicaps (an embedded save has them already).
void Playback::apply_header_seats(sim::Game& g) {
	MpSeats seats;
	seats.plan = make_seat_plan(0, 0, int(setup_.num_nations), true);
	for (const sim::ReplayPlayer& p : rp_.header.players) {
		if (!p.present() || p.play < 0 || p.play > 7) continue;
		if (!(p.flags & 4)) continue;   // human
		seats.plan.peer_of[size_t(p.play)] = p.play;
		seats.name[size_t(p.play)] = sim::to_utf8(p.name);
		seats.handicap[size_t(p.play)] = p.handicap;
	}
	seats.plan.nations = setup_.num_nations;
	apply_mp_seats(g, who_, seats);
}

// ---------------------------------------------------------------- the direct player

void Playback::bind(sim::Game& g, bool all_verbs) {
	game_ = &g;
	const bool original = kind_ == Kind::Original || kind_ == Kind::OriginalRandom;
	ExecPolicy policy = original && !all_verbs ? ExecPolicy::sav_compatible() : ExecPolicy::all_engine();
	hooks_ = std::make_unique<GameHooks>(g, who_, original ? ChecksumMode::Original : ChecksumMode::Native, policy, scenario_, seed_);
	hooks_->before_frame = before_frame;
	hooks_->after_frame = after_frame;
	next_ = 0;
	start_ = g.frame;
	sync_ = PlaybackSync();
	sync_.has_rows = !rows_.empty();
	sync_.first_diff.fill(-1);
	sync_.last_match.fill(-1);
	// expected value per boundary frame (ron-sav --replay): the row at record frame R describes the boundary R - turn, the first row's
	// boundary is the start frame + 1
	expect_.clear();
	int64_t prev = 0;
	bool first = true;
	for (const auto& kv : rows_) {
		expect_[first ? start_ + 1 : prev] = kv.second;
		prev = kv.first;
		first = false;
	}
}

void Playback::compare(int64_t frame) {
	if (!checking_ || !hooks_) return;
	auto it = expect_.find(frame);
	if (it == expect_.end()) return;
	uint32_t v[16];
	if (!hooks_->checksums(v)) return;
	int match = 0;
	for (int k = 0; k < dw::kCheckPartCount; ++k) {
		const bool same = v[k] == it->second[size_t(k)];
		match += same;
		if (same) {
			if (sync_.first_diff[size_t(k)] < 0) sync_.last_match[size_t(k)] = frame;
		} else if (sync_.first_diff[size_t(k)] < 0) {
			sync_.first_diff[size_t(k)] = frame;
		}
	}
	++sync_.compared;
	sync_.equal += match == dw::kCheckPartCount;
	sync_.last_frame = frame;
	sync_.last_ok = match == dw::kCheckPartCount;
	if (!sync_.last_ok && sync_.first_out_of_sync < 0) sync_.first_out_of_sync = frame;
}

void Playback::deliver() {
	while (next_ < groups_.size() && groups_[next_].frame <= game_->frame) {
		const Group& gr = groups_[next_];
		if (gr.frame == game_->frame) compare(gr.frame);
		for (size_t i = gr.first; i < gr.last; ++i) {
			const sim::ReplayPackage& pk = rp_.packages[i];
			if (is_drop_package(pk)) continue;
			hooks_->begin_package(pk.play, pk.stamp);
			for (const sim::ReplayCommand& c : pk.cmds) {
				if (automatic_verb(c.op)) continue;
				hooks_->execute(pk.play, pk.data(c), c.len);
			}
			hooks_->end_package(pk.play);
		}
		++next_;
	}
}

bool Playback::step() {
	if (!game_ || at_end()) return false;
	deliver();
	if (at_end()) return false;
	hooks_->step_frame();
	return true;
}

int64_t Playback::run_to(int64_t frame) {
	int64_t n = 0;
	while (game_ && game_->frame < frame && step()) ++n;
	return n;
}

std::string Playback::sync_text() const {
	if (!sync_.has_rows || !checking_) return std::string();
	if (sync_.first_out_of_sync >= 0) return "out of sync at frame " + std::to_string(sync_.first_out_of_sync);
	return sync_.compared > 0 ? "in sync" : "";
}

bool playback_state_hash(Playback& pb, uint64_t& out) {
	if (!pb.bound()) return false;
	uint32_t v[16];
	if (!pb.hooks().checksums(v)) return false;
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](uint64_t x) {
		for (int i = 0; i < 8; ++i) h = (h ^ ((x >> (8 * i)) & 0xff)) * 1099511628211ull;
	};
	mix(uint64_t(uint32_t(pb.frame())));
	for (int k = 0; k < dw::kCheckPartCount; ++k) mix(v[k]);
	out = h;
	return true;
}

// ---------------------------------------------------------------- the shared report

void playback_print_sync(Playback& pb) {
	const PlaybackSync& s = pb.sync();
	std::printf("  played to frame %d (%u of %u turns, %llu commands executed); %lld boundaries compared, %lld with all 15 parts equal\n", pb.frame(), pb.turns_run(),
	            pb.turns_total(), (unsigned long long)pb.commands_run(), (long long)s.compared, (long long)s.equal);
	if (s.has_rows) {
		if (s.compared == 0) std::printf("  check sums: no boundary compared yet\n");
		else if (s.first_out_of_sync < 0) std::printf("  check sums: in sync through frame %lld\n", (long long)s.last_frame);
		else std::printf("  check sums: out of sync at frame %lld\n", (long long)s.first_out_of_sync);
		for (int k = 0; k < dw::kCheckPartCount; ++k) {
			if (s.first_diff[size_t(k)] < 0) std::printf("  %-16s %s\n", dw::kCheckPartNames[k], s.compared > 0 ? ("matches through frame " + std::to_string(s.last_frame)).c_str() : "no boundary compared yet");
			else std::printf("  %-16s first differs at boundary frame %lld (matched through %lld)\n", dw::kCheckPartNames[k], (long long)s.first_diff[size_t(k)],
			                 (long long)s.last_match[size_t(k)]);
		}
	}
	const GameHooks& h = pb.hooks();
	if (h.first_unsupported_frame >= 0) {
		std::map<std::string, uint64_t> by_name;
		for (const auto& kv : h.unsupported) by_name[sim::replay_verb_name(kv.first)] += kv.second;
		std::printf("  commands not executed (first at frame %d):", h.first_unsupported_frame);
		for (const auto& kv : by_name) std::printf(" %s x%llu", kv.first.c_str(), (unsigned long long)kv.second);
		std::printf("\n");
	}
}

int playback_report(Playback& pb, sim::Game& g, int64_t frames, bool all_verbs) {
	pb.bind(g, all_verbs);
	const std::string name = [&] {
		const std::string& p = pb.path();
		const size_t s = p.find_last_of('/');
		return s == std::string::npos ? p : p.substr(s + 1);
	}();
	const char* kinds[] = {"original game", "native save", "native random game", "random map of the original game"};
	std::printf("%s: playback of a recording of the %s, from frame %lld to %lld, %u turns, %s\n", name.c_str(), kinds[int(pb.kind())],
	            (long long)pb.start_frame(), (long long)pb.end_frame(), pb.turns_total(), pb.has_check_sums() ? "CHECK_SUMS rows present" : "no CHECK_SUMS rows");
	const int64_t target = frames > 0 ? pb.start_frame() + frames : INT64_MAX;
	pb.run_to(target);
	playback_print_sync(pb);
	return frames > 0 || pb.at_end() ? 0 : 1;
}

}  // namespace ron::net
