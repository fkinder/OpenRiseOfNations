#include "net/playback.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "net/lobby.hpp"
#include "sim/datawalk.hpp"
#include "sim/mountains.hpp"
#include "sim/original_save.hpp"
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
	out.name = "Recording of the original game";
	return true;
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
	if (rp.native_save) {
		pb->kind_ = Kind::NativeSave;
		const sim::SaveInfo info = sim::read_save_info(rp.native_save_bytes());
		pb->scenario_ = info.scenario;
		pb->seed_ = info.seed;
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
			err = "no embedded save: the game starts from the setup (random map, rules block in the stream), which is not supported";
			return nullptr;
		}
		if (!rp.has_save_info) {
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
	ExecPolicy policy = kind_ == Kind::Original && !all_verbs ? ExecPolicy::sav_compatible() : ExecPolicy::all_engine();
	hooks_ = std::make_unique<GameHooks>(g, who_, kind_ == Kind::Original ? ChecksumMode::Original : ChecksumMode::Native, policy, scenario_, seed_);
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
	const char* kinds[] = {"original game", "native save", "native random game"};
	std::printf("%s: playback of a recording of the %s, from frame %lld to %lld, %u turns, %s\n", name.c_str(), kinds[int(pb.kind())],
	            (long long)pb.start_frame(), (long long)pb.end_frame(), pb.turns_total(), pb.has_check_sums() ? "CHECK_SUMS rows present" : "no CHECK_SUMS rows");
	const int64_t target = frames > 0 ? pb.start_frame() + frames : INT64_MAX;
	pb.run_to(target);
	playback_print_sync(pb);
	return frames > 0 || pb.at_end() ? 0 : 1;
}

}  // namespace ron::net
