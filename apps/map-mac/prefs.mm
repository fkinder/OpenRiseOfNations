#include "prefs.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace prefs {

namespace {

Prefs g_prefs;
bool g_loaded = false;

std::string trim(std::string s) {
	const size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return {};
	const size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

int to_int(const std::string& v, int lo, int hi, int fallback) {
	char* end = nullptr;
	const long n = std::strtol(v.c_str(), &end, 10);
	if (end == v.c_str()) return fallback;
	return int(std::clamp<long>(n, lo, hi));
}

bool to_bool(const std::string& v, bool fallback) {
	if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
	if (v == "0" || v == "false" || v == "no" || v == "off") return false;
	return fallback;
}

}  // namespace

float scroll_factor(int setting) {
	static const float k[13] = {0.33f, 0.5f, 0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f, 4.0f, 5.0f};
	return k[std::clamp(setting, 0, 12)];
}

void reset_pane(Prefs& p, int pane) {
	const Prefs d;
	switch (pane) {
		case 0:
			p.game_speed = d.game_speed, p.difficulty = d.difficulty, p.help_detail = d.help_detail;
			p.advanced_menu = d.advanced_menu, p.progress_bars = d.progress_bars;
			p.message_length = d.message_length, p.auto_citizen = d.auto_citizen, p.auto_citizen_delay = d.auto_citizen_delay;
			p.building_stance = d.building_stance, p.auto_transport = d.auto_transport, p.auto_deploy = d.auto_deploy, p.record_games = d.record_games;
			break;
		case 1: p.hotkeys.clear(); break;
		case 2: p.scroll_speed = 6, p.prefer_military = d.prefer_military; break;  // OptionsWinControl::do_reset: scroll_speed 6, drag_military on
		case 3: p.streamlined = d.streamlined, p.fullscreen = d.fullscreen; break;
		case 5: p.language.clear(); break;
		case 4:
			p.fx_volume = d.fx_volume, p.music_volume = d.music_volume;
			p.music_in_game = d.music_in_game, p.music_in_menu = d.music_in_menu;
			break;
		default: break;
	}
}

void parse(const std::string& text, Prefs& out) {
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
		if (k.rfind("hotkey.", 0) == 0) {
			out.hotkeys[k.substr(7)] = v;
			continue;
		}
		bool known = true;
		if (k == "game_speed") out.game_speed = to_int(v, 0, 4, out.game_speed);
		else if (k == "difficulty") out.difficulty = to_int(v, 0, 5, out.difficulty);
		else if (k == "help_detail") out.help_detail = to_int(v, 0, 3, out.help_detail);
		else if (k == "advanced_menu") out.advanced_menu = to_bool(v, out.advanced_menu);
		else if (k == "progress_bars") out.progress_bars = to_bool(v, out.progress_bars);
		else if (k == "message_length") out.message_length = to_int(v, 0, 10, out.message_length);
		else if (k == "auto_citizen") out.auto_citizen = to_int(v, 0, 3, out.auto_citizen);
		else if (k == "auto_citizen_delay") out.auto_citizen_delay = to_int(v, 0, 5, out.auto_citizen_delay);
		else if (k == "building_stance") out.building_stance = to_int(v, 0, 5, out.building_stance);
		else if (k == "auto_transport") out.auto_transport = to_bool(v, out.auto_transport);
		else if (k == "auto_deploy") out.auto_deploy = to_bool(v, out.auto_deploy);
		else if (k == "record_games") out.record_games = to_bool(v, out.record_games);
		else if (k == "score_window") out.score_window = to_int(v, 0, 1, out.score_window);
		else if (k == "scroll_speed") out.scroll_speed = to_int(v, 0, 12, out.scroll_speed);
		else if (k == "prefer_military") out.prefer_military = to_bool(v, out.prefer_military);
		else if (k == "fx_volume") out.fx_volume = to_int(v, 0, 255, out.fx_volume);
		else if (k == "music_volume") out.music_volume = to_int(v, 0, 255, out.music_volume);
		else if (k == "music_in_game") out.music_in_game = to_bool(v, out.music_in_game);
		else if (k == "music_in_menu") out.music_in_menu = to_bool(v, out.music_in_menu);
		else if (k == "streamlined") out.streamlined = to_bool(v, out.streamlined);
		else if (k == "fullscreen") out.fullscreen = to_bool(v, out.fullscreen);
		else if (k == "language") out.language = v;
		else if (k == "game_dir") out.game_dir = v;
		else known = false;
		if (known) out.present.insert(k);
	}
}

std::string serialize(const Prefs& p) {
	std::ostringstream o;
	o << "# OpenRiseOfNations preferences (edited by the Options page; safe to edit by hand)\n";
	o << "game_speed = " << p.game_speed << "\n";
	o << "difficulty = " << p.difficulty << "\n";
	o << "help_detail = " << p.help_detail << "\n";
	o << "advanced_menu = " << (p.advanced_menu ? 1 : 0) << "\n";
	o << "progress_bars = " << (p.progress_bars ? 1 : 0) << "\n";
	o << "message_length = " << p.message_length << "\n";
	o << "auto_citizen = " << p.auto_citizen << "\n";
	o << "auto_citizen_delay = " << p.auto_citizen_delay << "\n";
	o << "building_stance = " << p.building_stance << "\n";
	o << "auto_transport = " << (p.auto_transport ? 1 : 0) << "\n";
	o << "auto_deploy = " << (p.auto_deploy ? 1 : 0) << "\n";
	o << "record_games = " << (p.record_games ? 1 : 0) << "\n";
	if (p.present.count("score_window")) o << "score_window = " << p.score_window << "\n";
	o << "scroll_speed = " << p.scroll_speed << "\n";
	o << "prefer_military = " << (p.prefer_military ? 1 : 0) << "\n";
	o << "fx_volume = " << p.fx_volume << "\n";
	o << "music_volume = " << p.music_volume << "\n";
	o << "music_in_game = " << (p.music_in_game ? 1 : 0) << "\n";
	o << "music_in_menu = " << (p.music_in_menu ? 1 : 0) << "\n";
	o << "streamlined = " << (p.streamlined ? 1 : 0) << "\n";
	o << "fullscreen = " << (p.fullscreen ? 1 : 0) << "\n";
	if (!p.language.empty()) o << "language = " << p.language << "\n";
	if (!p.game_dir.empty()) o << "game_dir = " << p.game_dir << "\n";
	for (const auto& [sig, keys] : p.hotkeys) o << "hotkey." << sig << " = " << keys << "\n";
	return o.str();
}

std::string path() {
	if (const char* e = std::getenv("RON_PREFS")) return e;
	@autoreleasepool {
		NSArray* dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
		NSString* base = dirs.count ? dirs[0] : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support"];
		return [[base stringByAppendingPathComponent:@"OpenRiseOfNations"] stringByAppendingPathComponent:@"preferences.txt"].UTF8String;
	}
}

bool load() {
	g_loaded = true;
	g_prefs = Prefs();
	@autoreleasepool {
		NSData* d = [NSData dataWithContentsOfFile:@(path().c_str())];
		if (!d) return false;
		NSString* s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
		if (!s) return false;
		parse(s.UTF8String, g_prefs);
	}
	return true;
}

bool save() {
	@autoreleasepool {
		NSString* p = @(path().c_str());
		[[NSFileManager defaultManager] createDirectoryAtPath:[p stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
		const std::string body = serialize(g_prefs);
		Prefs again;  // a saved file names every key: the window's own values no longer win
		parse(body, again);
		g_prefs.present = again.present;
		NSString* text = @(body.c_str());
		return [text writeToFile:p atomically:YES encoding:NSUTF8StringEncoding error:nil];
	}
}

bool save_game_dir(const std::string& dir) {
	@autoreleasepool {
		NSString* p = @(path().c_str());
		[[NSFileManager defaultManager] createDirectoryAtPath:[p stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
		NSString* old = [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil] ?: @"";
		NSMutableArray<NSString*>* lines = [NSMutableArray array];
		for (NSString* l in [old componentsSeparatedByString:@"\n"]) {
			NSString* t = [l stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
			if ([t hasPrefix:@"game_dir"] && [[t substringFromIndex:8] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].UTF8String[0] == '=') continue;
			if (l.length) [lines addObject:l];
		}
		[lines addObject:[NSString stringWithFormat:@"game_dir = %s", dir.c_str()]];
		g_prefs.game_dir = dir;
		return [[[lines componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"] writeToFile:p atomically:YES encoding:NSUTF8StringEncoding error:nil];
	}
}

Prefs& get() {
	if (!g_loaded) load();
	return g_prefs;
}

}  // namespace prefs
