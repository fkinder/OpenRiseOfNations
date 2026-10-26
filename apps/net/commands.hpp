#pragma once

namespace ron::sim { struct Replay; }
#include <string>
namespace ronnet {
int cmd_replay(int argc, char** argv);
int cmd_playback(int argc, char** argv);   // the direct player (engine/net/playback.hpp): one game, the recording's packages at their frames
int cmd_selftest(int argc, char** argv);
int cmd_netgame(int argc, char** argv);  // host | join
int cmd_loopback(int argc, char** argv);
int replay_native(const ron::sim::Replay& rp, const std::string& path, const std::string& game_dir, int dlo, int dhi, const std::string& save_out, bool verbose);
}  // namespace ronnet
