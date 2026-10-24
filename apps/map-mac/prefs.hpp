// The preferences the Options page edits (docs/ui-port.md section 15): game speed, scroll speed, sound volumes, interface switches
// and the key map. They live in a small text file under the user's Application Support folder
// (~/Library/Application Support/OpenRiseOfNations/preferences.txt, one `key = value` line each; never in the game install) and are
// applied when the front end or a game starts and whenever the page's OK is pressed. A key that is absent from the file keeps the
// window's own default (or its NSUserDefaults profile value), so an old install behaves as before.
#pragma once

#include <map>
#include <set>
#include <string>

namespace prefs {

struct Prefs {
	// Game pane (OptionsWinGame): game_speed slider 0..3 (Very Slow, Slow, Normal, Fast), diff_level 0..5, message_detail 0..3
	// (0 = no help boxes, 1..3 = the help verbosity), the advanced-orders and research-progress-bar switches.
	int game_speed = 2;
	int difficulty = 2;
	int help_detail = 2;
	bool advanced_menu = false;
	bool progress_bars = false;
	// Mouse pane (OptionsWinControl): scroll_speed 0..12 (speed_option table), drag_military = the box select prefers soldiers.
	int scroll_speed = 4;  // speed_option 4 = 1.0, the window's step before the page existed; OptionsWinControl::do_reset sets 6
	bool prefer_military = true;
	// Sound pane (OptionsWinSound): volumes 0..255, the two music switches.
	int fx_volume = 255;
	int music_volume = 153;
	bool music_in_game = true;
	bool music_in_menu = true;
	// Graphics pane: the interface mode (the display button's streamlined layout) and the window mode.
	bool streamlined = false;
	bool fullscreen = false;
	// Player pane: the interface language ("en", "de", "es", "fr", "it"; "" = not chosen), used by the next start (uistr::choose).
	std::string language;
	// Hotkeys pane: the entries whose key was changed, "name|mode|ctx|param" -> key string ("" = unbound).
	std::map<std::string, std::string> hotkeys;
	// Keys present in the file (the others keep the window's own value).
	std::set<std::string> present;

	bool has(const char* key) const { return present.count(key) != 0; }
};

// The process-wide preferences (loaded on first use).
Prefs& get();
// The defaults the page's "Reset Defaults" restores for one pane (0 Game, 1 Hotkeys, 2 Mouse, 3 Graphics, 4 Sound).
void reset_pane(Prefs& p, int pane);
// Read / written as text. The path can be overridden with RON_PREFS (tests); `load` returns false when the file is absent.
std::string path();
bool load();
bool save();
// speed_option: the camera scroll multiplier of a profile setting 0..12 (0.33, 0.5, 0.75, 0.9, 1, 1.1, 1.25, 1.5, 1.75, 2, 3, 4, 5).
float scroll_factor(int setting);
// Parses / writes the text form (used by load / save, and by the self-test).
void parse(const std::string& text, Prefs& out);
std::string serialize(const Prefs& p);

}  // namespace prefs
