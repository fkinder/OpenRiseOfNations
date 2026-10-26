// The Recorded Games viewer (docs/ui-port.md section 18): the list scanner behind the front end's "Recorded Games" screen and the
// playback bar drawn over a game that is playing a recording (engine/net/playback.hpp is the engine side; main.mm drives it).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "assets/iface_layout.hpp"
#include "net/playback.hpp"
#include "ui.hpp"

namespace recplay {

// ---------------------------------------------------------------- the list

struct Row {
	std::string path, file;
	int64_t mtime = 0;                    // seconds since 1970
	bool read = false;                    // the header has been read (summary valid)
	ron::net::PlaybackSummary summary;
};

// Finds the .rcx files of a folder (recursively, newest first) at once and reads their headers on a thread, so the screen opens at once and the
// rows fill in. `snapshot` copies the rows; `changed` counts finished headers (a screen re-formats when it moves).
class Scanner {
public:
	~Scanner() { stop(); }
	void start(const std::string& dir);
	// A single file the player picked (Open File...): a list of one.
	void start_file(const std::string& path);
	void stop();
	std::vector<Row> snapshot();
	uint32_t changed() const { return changed_.load(); }
	bool done() const { return done_.load(); }

private:
	void run();
	std::mutex mu_;
	std::vector<Row> rows_;
	std::thread th_;
	std::atomic<uint32_t> changed_{0};
	std::atomic<bool> done_{true}, quit_{false};
};

// "m:ss" of a frame count at the game's 15 frames a second ("h:mm:ss" from an hour).
std::string clock_text(int64_t frames);

// ---------------------------------------------------------------- the playback bar

constexpr int kSpeedCount = 6;
extern const double kFrameMs[kSpeedCount];       // milliseconds per game frame (0: as fast as the machine allows)
extern const char* const kSpeedNames[kSpeedCount];
constexpr int kNormalSpeed = 1;

enum Button : int { kBtnNone = -1, kBtnRestart, kBtnPause, kBtnSlower, kBtnFaster, kBtnPrev, kBtnNext, kBtnFog, kBtnExit, kBtnTimeline, kBtnCount };

struct Seat {
	int who = 0;
	std::string label;
	int color = 0;
};

struct BarState {
	int64_t frame = 0, start = 0, end = 0;
	bool paused = false, ended = false, fog_on = false, seeking = false;
	int speed = kNormalSpeed;
	std::string seat_label, sync, name;
	int sync_kind = 0;   // 0 none, 1 in sync, 2 out of sync
	int hover = kBtnNone;
};

class Viewer {
public:
	explicit Viewer(std::shared_ptr<ron::net::Playback> p) : pb(std::move(p)) {}
	std::shared_ptr<ron::net::Playback> pb;
	std::vector<Seat> seats;
	int seat = 0;                      // index into `seats`
	int speed = kNormalSpeed;
	bool ended = false;
	int64_t seek_to = -1;              // a seek in progress: the frame being run to
	int64_t seek_from = 0;
	int64_t restart_seek = -1;         // after a rewind: the frame to run to
	bool announced_end = false;

	// Draws the bar at the top of the screen and keeps the button rectangles for `hit`.
	void draw(ron::metal::UiRenderer& ui, int ui_w, int ui_h, const BarState& s);
	int hit(int x, int y) const;
	bool laid_out() const { return panel_.r > panel_.l; }   // draw() has run at least once (the rectangles exist)
	// The frame a click on the timeline at x asks for.
	int64_t frame_at(int x, const BarState& s) const;

private:
	ron::IfaceRect rect_[kBtnCount];
	ron::IfaceRect panel_{};
};

}  // namespace recplay
