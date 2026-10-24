// Loading a save of the original game (RoNSave, 10 .SVX in re/oracle/Saves and the saves embedded in .rcx replays) into the
// engine's Game (docs/original-save.md).
//
// The save is decoded with the generic DataWalk interpreter (sim/datawalk.hpp) into a tree of object images. Every member
// the engine models is then read out of those images into the engine's own structs (sim/original_map.hpp lists them, by
// PDB name and offset); everything else stays in the decoded tree, the *shadow* (OriginalShadow, attached to the Game), so
// EngineProvider (sim/datawalk_engine.hpp) can emit the original's byte image of the whole state again: modelled members
// from the engine, the rest from the shadow.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/files.hpp"
#include "sim/anims.hpp"
#include "sim/datawalk.hpp"
#include "sim/game.hpp"
#include "sim/original_map.hpp"

namespace ron::sim {

// The decoded original save and the binding of its object images to the engine's objects.
struct OriginalShadow {
	std::unique_ptr<datawalk::Tree> tree;
	std::vector<datawalk::LocalValue> verify;  // the save's stored verify block, in stream order
	std::string magic;
	uint32_t version = 0;
	size_t walk_begin = 0, walk_end = 0;       // stream positions (after the version / after the walk)
	std::vector<uint8_t> rules_image;          // the save's own rules block (the last bytes of the walk)
	bool rules_equal = false;                  // the engine's rules tables write the same image
	std::unordered_map<const datawalk::Rec*, std::vector<omap::Segment>> segs;
	// Index of the object images the loader bound (forward direction: engine object -> image).
	std::vector<datawalk::Rec*> cells, cells2;  // World WData (part 5) and the block marker (part 9), by cell index
	std::vector<std::vector<datawalk::Rec*>> unit_recs, build_recs;  // [who][slot]
	bool leader_ai[kNumLeaders] = {};  // the leader's AI arrays (sites, make list) were allocated in the save (the gaia leader's are empty)
private:
	std::unique_ptr<datawalk::TreeProvider> provider_;
	std::map<std::tuple<const datawalk::Rec*, int, int64_t, int64_t>, datawalk::Rec*> fresh_;
public:
	// The one TreeProvider over `tree` (its replay cursors live in the Recs, so providers of one tree must share a pass counter).
	datawalk::TreeProvider& tree_provider() {
		if (!provider_) provider_ = std::make_unique<datawalk::TreeProvider>(*tree);
		return *provider_;
	}
	void bind(const datawalk::Rec* rec, const omap::ClassMap& map, int32_t a = 0, int32_t b = 0, int32_t c = 0, int64_t base = 0,
	          int64_t len = -1);
	// Binds `map` to `rec` with this id, replacing an earlier binding of the same map (an element whose engine index changed).
	void rebind(const datawalk::Rec* rec, const omap::ClassMap& map, const omap::Seg& id, int64_t base = 0, int64_t len = -1);
	// Images of objects the engine created after the load (no original bytes: zero, as a freshly allocated original object),
	// keyed by (parent image, walk step, call offset, index).
	datawalk::Rec* fresh_rec(const datawalk::Rec* parent, int step, int64_t base, int64_t index, const char* cls);
};

struct OriginalLoadReport {
	bool ok = false;
	std::string error;            // set when the decode or the load failed
	std::vector<std::string> notes;  // what the loader could not do (one line each)
	bool rules_equal = false;     // the loaded rules tables write the save's rules image byte for byte
	std::string rules_note;       // first difference, if any
	// Counts per class map: members of the original's walked ranges, those the engine maps, the rest (shadow).
	struct ClassCount {
		std::string name, cls;
		int walked = 0, mapped = 0;
		std::vector<std::string> unmapped;
	};
	std::vector<ClassCount> classes;
	int64_t units = 0, builds = 0, cells = 0, leaders = 0;
	int32_t frame = 0;
};

// Builds a Game from the decompressed bytes of a save (String magic, version, walk, verify block). `rules` is the loader's
// rules (load_rules + start_game(GameSetup())) and, like `files`, must outlive the Game. Returns null (report->error) when
// the stream does not decode. The returned game is a snapshot of the state at the save frame; stepping it needs the
// terrain data a save does not hold (heights), see docs/original-save.md.
std::unique_ptr<Game> load_original_save(const GameFiles& files, const Rules& rules, const AnimSource* anims,
                                         const std::vector<uint8_t>& stream, OriginalLoadReport* report = nullptr);

// A member-by-member audit of the maps: every bound object's mapped entries are emitted from the engine and compared with the
// original bytes of the decoded save. One line per (map, member) that differs somewhere.
struct AuditLine {
	std::string member;       // "Unit.angle"
	int64_t objects = 0;      // objects checked
	int64_t mismatches = 0;   // objects whose emitted bytes differ
	std::string example;      // the first one: object ids, engine bytes, original bytes
};
std::vector<AuditLine> audit_mapping(Game& game);

// Which mapped members changed between two states of the same loaded save (e.g. before and after stepping N frames): every bound
// object's entries are emitted from both games and compared. `a` and `b` must come from the same save (same bindings).
std::vector<AuditLine> diff_mapped_state(Game& a, Game& b);

// Experiment: copies the listed mapped members ("Unit.flags") of every bound object from `from` into `to` (same save, so the bindings
// match): undoes what the engine changed in them. Returns the number of member values copied.
int64_t revert_members(Game& to, Game& from, const std::vector<std::string>& members);

// A multiplayer game started with a game script (GameInfo.script_type 1: scenario/Scripts/<Name>/<name>.bhs, e.g. "no_nukes") keeps the
// script's name in the replay header only (the embedded save has no scenario_script). Creates g.scripts for a loaded game and compiles
// that script so Game::do_frame runs it again (a run_once block runs once more: the effects it has on the rules are already in the
// loaded rules image). False with `err` when the file is missing or does not compile.
bool original_attach_game_script(Game& g, const GameFiles& files, const std::string& name, std::string* err = nullptr);

// What loading a save does to the running game besides restoring its bytes (WalkDataGame::walk_data @5a2360, the `input` tail after the
// generator's seed was restored; docs/original-save.md section 6.2): `Leader::init(leaders[9], 9, -1, 9)` (the second gaia leader is not
// in the file: its random_personality and coin draws come from game_random), then `leaders[9].flags |= 7`, then `Farms::add_animals` for
// every pen record (the five farm animals of each pen are objects of list 9, which the file does not hold). The replay driver calls it
// right after the load, before the first frame, so the generator and list 9 are what the original's game had when it resumed.
// (Not part of the load itself: EngineProvider must keep emitting the file's seed.)
void original_post_load_effects(Game& g);

// Experiment (the replay search of apps/sav): every mapped (object, member) whose bytes differ between `to` (stepped) and `from` (the
// loaded save), and a switch that puts one of them back to the save's value and forth again.
struct Seg_t { int32_t a = 0, b = 0, c = 0; };
struct MemberChange {
	std::string key;      // "Unit.angle"
	Seg_t id;
	const void* segment = nullptr;  // the binding (omap::Segment), valid while the Game lives
	const void* entry = nullptr;    // the omap::Entry
	std::vector<uint8_t> now, before;
};
std::vector<MemberChange> changed_members(Game& to, Game& from);
std::vector<MemberChange> members_of(Game& g, const std::vector<std::string>& keys);  // every bound (object, member) named in `keys`
void apply_member(Game& to, const MemberChange& c, bool revert);
void apply_member_bytes(Game& to, const MemberChange& c, const std::vector<uint8_t>& bytes);  // load arbitrary member bytes  // revert: write `before`; else write `now`

}  // namespace ron::sim
