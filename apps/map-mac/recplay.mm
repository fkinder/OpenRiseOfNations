#include "recplay.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>

namespace recplay {

namespace fs = std::filesystem;
using ron::IfaceRect;
using ron::metal::UiType;

const double kFrameMs[kSpeedCount] = {134.0, 67.0, 34.0, 17.0, 8.0, 0.0};
const char* const kSpeedNames[kSpeedCount] = {"1/2x", "1x", "2x", "4x", "8x", "Max"};

std::string clock_text(int64_t frames) {
	const int64_t s = std::max<int64_t>(0, frames) / 15;
	char b[32];
	if (s >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
	else std::snprintf(b, sizeof b, "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
	return b;
}

// ---------------------------------------------------------------- the list

void Scanner::stop() {
	quit_ = true;
	if (th_.joinable()) th_.join();
	quit_ = false;
}

static int64_t mtime_of(const std::string& p) {
	struct stat st;
	return ::stat(p.c_str(), &st) == 0 ? int64_t(st.st_mtime) : 0;
}

void Scanner::start(const std::string& dir) {
	stop();
	std::vector<Row> rows;
	std::error_code ec;
	if (fs::is_directory(dir, ec)) {
		for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
			const std::string p = it->path().string();
			if (p.size() > 4 && p.compare(p.size() - 4, 4, ".rcx") == 0 && it->is_regular_file(ec)) {
				Row r;
				r.path = p;
				r.file = it->path().filename().string();
				r.mtime = mtime_of(p);
				rows.push_back(std::move(r));
			}
		}
	}
	std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.mtime != b.mtime ? a.mtime > b.mtime : a.file < b.file; });
	{
		std::lock_guard<std::mutex> lk(mu_);
		rows_ = std::move(rows);
	}
	++changed_;
	done_ = false;
	th_ = std::thread([this] { run(); });
}

void Scanner::start_file(const std::string& path) {
	stop();
	Row r;
	r.path = path;
	r.file = fs::path(path).filename().string();
	r.mtime = mtime_of(path);
	{
		std::lock_guard<std::mutex> lk(mu_);
		rows_.clear();
		rows_.push_back(std::move(r));
	}
	++changed_;
	done_ = false;
	th_ = std::thread([this] { run(); });
}

void Scanner::run() {
	size_t n;
	{
		std::lock_guard<std::mutex> lk(mu_);
		n = rows_.size();
	}
	for (size_t i = 0; i < n && !quit_; ++i) {
		std::string path;
		{
			std::lock_guard<std::mutex> lk(mu_);
			path = rows_[i].path;
		}
		ron::net::PlaybackSummary s;
		ron::net::playback_summary(path, s);
		std::lock_guard<std::mutex> lk(mu_);
		rows_[i].summary = std::move(s);
		rows_[i].read = true;
		++changed_;
	}
	done_ = true;
	++changed_;
}

std::vector<Row> Scanner::snapshot() {
	std::lock_guard<std::mutex> lk(mu_);
	return rows_;
}

// ---------------------------------------------------------------- the bar

namespace {
constexpr uint32_t kPanel = 0xC8101018u, kButton = 0xD0303040u, kButtonHot = 0xE05060A0u, kText = 0xFFE8E8E8u, kDim = 0xFFA0A0B0u;
}

void Viewer::draw(ron::metal::UiRenderer& ui, int ui_w, int ui_h, const BarState& s) {
	(void)ui_h;
	const int w = std::min(ui_w - 24, 820), h = 56;
	const int x0 = (ui_w - w) / 2, y0 = 62;
	panel_ = {x0, x0 + w, y0, y0 + h};
	const int f = ui.font(13, -1), fs = ui.font(12, -1);
	ui.next_window();
	ui.fill(float(x0), float(y0), float(w), float(h), kPanel, UiType::ColorBlend);
	ui.next_layer();

	// row 1: buttons
	const int by = y0 + 5, bh = 22;
	int x = x0 + 6;
	auto button = [&](int id, const std::string& label, int bw) {
		rect_[id] = {x, x + bw, by, by + bh};
		ui.fill(float(x), float(by), float(bw), float(bh), s.hover == id ? kButtonHot : kButton, UiType::ColorBlend);
		ui.next_layer();
		ui.text_centered(f, label, rect_[id], kText, 0xFF000000u);
		x += bw + 4;
	};
	button(kBtnRestart, "|<", 26);
	button(kBtnPause, s.ended ? "Replay" : s.paused ? "Play" : "Pause", 52);
	button(kBtnSlower, "-", 24);
	{
		const std::string sp = std::string("Speed ") + kSpeedNames[std::clamp(s.speed, 0, kSpeedCount - 1)];
		ui.text(f, sp, float(x + 2), float(by + 3), kText, 0xFF000000u);
		x += ui.text_width(f, sp) + 8;
	}
	button(kBtnFaster, "+", 24);
	x += 10;
	button(kBtnPrev, "<", 22);
	{
		const std::string who = std::string("Watching: ") + (s.seat_label.empty() ? "?" : s.seat_label);
		const int tw = std::max(150, ui.text_width(f, who) + 8);
		ui.text(f, who, float(x + 3), float(by + 3), 0xFFFFE070u, 0xFF000000u);
		x += tw;
	}
	button(kBtnNext, ">", 22);
	x += 6;
	button(kBtnFog, s.fog_on ? "Fog: on" : "Whole map", 82);
	// the exit button at the right edge
	{
		const int bw = 46;
		const int ex = x0 + w - bw - 6;
		rect_[kBtnExit] = {ex, ex + bw, by, by + bh};
		ui.fill(float(ex), float(by), float(bw), float(bh), s.hover == kBtnExit ? kButtonHot : kButton, UiType::ColorBlend);
		ui.next_layer();
		ui.text_centered(f, "Exit", rect_[kBtnExit], kText, 0xFF000000u);
	}

	// row 2: the timeline with the clock before it and the check sum state after it
	const int ty = y0 + 33, th = 14;
	const std::string now = clock_text(s.frame - s.start) + " / " + clock_text(s.end - s.start);
	const int cw = ui.text_width(fs, now);
	ui.text(fs, now, float(x0 + 8), float(ty), kText, 0xFF000000u);
	std::string right = s.seeking ? "Seeking..." : s.sync;
	uint32_t rcol = s.seeking ? 0xFFFFE070u : s.sync_kind == 1 ? 0xFF70E070u : s.sync_kind == 2 ? 0xFFFF6060u : kDim;
	if (right.empty() && !s.seeking) right = "No check sums";
	const int rw = ui.text_width(fs, right);
	ui.text(fs, right, float(x0 + w - 8 - rw), float(ty), rcol, 0xFF000000u);
	const int tl = x0 + 8 + cw + 10, tr = x0 + w - 8 - rw - 10;
	rect_[kBtnTimeline] = {tl, tr, ty, ty + th};
	ui.fill(float(tl), float(ty + 3), float(tr - tl), float(th - 6), 0xFF202030u, UiType::ColorBlend);
	ui.next_layer();
	const double span = double(std::max<int64_t>(1, s.end - s.start));
	const double frac = std::clamp(double(s.frame - s.start) / span, 0.0, 1.0);
	ui.fill(float(tl), float(ty + 3), float((tr - tl) * frac), float(th - 6), s.sync_kind == 2 ? 0xFFB05050u : 0xFF5080C0u, UiType::ColorBlend);
	ui.next_layer();
	ui.fill(float(tl + (tr - tl) * frac - 1), float(ty), 3.0f, float(th), 0xFFFFFFFFu, UiType::ColorBlend);
	ui.next_layer();
}

int Viewer::hit(int x, int y) const {
	for (int i = 0; i < kBtnCount; ++i)
		if (rect_[i].r > rect_[i].l && rect_[i].contains(x, y)) return i;
	return panel_.r > panel_.l && panel_.contains(x, y) ? kBtnCount : kBtnNone;   // kBtnCount: on the panel, no control
}

int64_t Viewer::frame_at(int x, const BarState& s) const {
	const IfaceRect& r = rect_[kBtnTimeline];
	if (r.r <= r.l) return s.start;
	const double f = std::clamp(double(x - r.l) / double(r.r - r.l), 0.0, 1.0);
	return s.start + int64_t(f * double(s.end - s.start));
}

}  // namespace recplay
