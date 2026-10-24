// Recording a live multiplayer session as a `.rcx` in the original's layout (docs/replay-spec.md sections 1-8, multiplayer-spec 6, 8.5):
//
//   header (Game / GameInfo / 8 players / scenario strings / Game[0x550,0x6e4) / semaphore 0x0c = multiplayer + recording / save_name)
//   [embedded save]        when the segment starts from a save (a game loaded from one, or the segment after an OOS recovery)
//   random-game info       5 bytes per present seat + 4
//   [rules block]          when there is no embedded save: Game::walk_rules_data, the exact image of sim::write_image
//   package records        i32 frame, i32 play, i32 valid (1), u32 stamp, i16 size, payload (MP XOR + the per-command padding)
//
// The whole file is written plain while the game runs and gzipped when it is closed (RecordGame::finalize); a crashed game leaves the
// plain file with a short last record, which the reader reports as truncated.
//
// Port extensions, all inside the original's layout: the embedded save of a native game is the engine's own save (docs/save-format.md),
// stored as String "RoNNativeSave", u32 save version, u32 length, bytes (the original's saves have no length; its readers find the end
// by scanning, and rcxdump.mjs does the same here); and the header's scenario_path of a random game holds the setup text ("random:..."),
// the same slot a native save uses, so a recording can be replayed without the original's lobby data.
#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "net/lockstep.hpp"
#include "sim/game.hpp"
#include "sim/newgame.hpp"
#include "sim/replay.hpp"

namespace ron::net {

struct RecordSeat {
	bool present = false, human = false, local = false;
	int tribe = 0, who = 0, team = -1, diff = 2, handicap = 0;
	std::string name;
};

struct RecordInfo {
	uint32_t seed = 0;                 // GameInfo.seed: the key of the packages' XOR and padding
	int32_t frame = 0, tick = 0;       // the game's frame when the segment starts
	std::array<uint8_t, sim::kSetupFieldCount> setup{};
	uint32_t info_flags = 0x121;
	std::array<RecordSeat, 8> seats;
	std::string scenario_path;         // "random:..." (random_setup_string) or a scenario path
	std::vector<std::pair<int8_t, int32_t>> random_info;  // per present seat: team, leader value (the tribe)
	int32_t world30 = 0;
	// Exactly one of these: the rules image of a game that starts from scratch, or the embedded save of one that starts from a save.
	std::vector<uint8_t> rules_image;
	std::vector<uint8_t> native_save;
	std::string save_name = "multiplayer.sav";  // header save_name of a segment with an embedded save
};

// The header facts of a random game: the setup bytes, the seats (`who_of` = Player.who of each seat) and the random-game info.
// `local_slot` marks the recording peer. `g` supplies the resolved tribes and the rules image (write_image of g.rules).
RecordInfo record_info_for_random(const sim::RandomGameSetup& setup, uint32_t seed, int local_slot, const std::array<int, 8>& who_of, const sim::Game& g,
                                  const std::string& scenario_text);
// The same for a game that starts from a native save segment (a recovery): the header of the first segment with the save embedded.
RecordInfo record_info_for_segment(const RecordInfo& first, const std::vector<uint8_t>& native_save, int32_t frame);

class Recorder {
public:
	~Recorder();
	// Opens `path` (plain, written as it goes). False with `error` on failure.
	bool open(const std::string& path, const RecordInfo& info, std::string& error);
	// One executed package (the Session::on_package arguments): `data` is the commands back to back as on the wire. A package with stamp 0 is a drop
	// ('P' command), written in the clear as DropControl::process_drops does.
	void package(int32_t frame, int slot, uint32_t stamp, const Bytes& data);
	// Gzips the file in place (to a temporary file, then a rename). Safe to call twice.
	bool close(std::string* error = nullptr);
	bool is_open() const { return out_.is_open(); }
	const std::string& path() const { return path_; }
	uint64_t packages() const { return packages_; }

private:
	std::ofstream out_;
	std::string path_;
	uint32_t seed_ = 0;
	uint64_t packages_ = 0;
};

// Segment file names: the first is `base`, the following ones `stem_2.rcx`, `stem_3.rcx` ... next to it.
std::string record_segment_path(const std::string& base, int segment);

}  // namespace ron::net
