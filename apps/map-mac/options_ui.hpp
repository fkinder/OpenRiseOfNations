// The Options page (docs/ui-port.md section 15): the original's OptionsWin (Data/optionswin.xml: the bronze frame, the six tabs, the
// area frames, sliders, check boxes, combo boxes, the hot-key lists and the OK / Reset Defaults / Cancel row) drawn with the UiRenderer
// in a 880 x 600 window, reached from the Main Menu and from the in-game menu. It edits a working copy of prefs::Prefs; OK saves the file and
// hands the values to the window (`Host::apply`), Cancel drops them, Reset Defaults restores the current pane (OptionsWin::do_reset).
// The page is independent of the window it appears in: the front end draws it on its scaled canvas, the game on the HUD's UI pixels.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/iface_layout.hpp"
#include "prefs.hpp"
#include "ui.hpp"

namespace optui {

// What the page asks of the window it is in.
struct Host {
	std::function<void(prefs::Prefs&)> live;            // overlay the running window's own values onto the working copy (game: speed, help, advanced ...)
	std::function<void(const prefs::Prefs&)> apply;     // OK: make the values take effect
	std::function<void(const prefs::Prefs&)> preview;   // while a slider moves (volumes); Cancel previews the saved values again
	std::function<std::string(int type)> type_name;     // names for the TRAIN / BUILD / RESEARCH keys of the Hotkeys pane
	std::function<std::string()> resolution;            // "1280 x 800": the window's size, shown on the Graphics pane
	std::function<void(const char* key)> sound;         // a soundglobals.xml KEY (OPTIONS_OPEN / OPTIONS_BUTTON / OPTIONS_TAB / OPTIONS_CLOSE)
	std::function<void()> closed;                       // after OK or Cancel
};

enum Pane { kGame = 0, kHotkeys, kMouse, kGraphics, kSound, kPlayer, kPaneCount };

class Page {
public:
	static constexpr int kW = 880, kH = 600;

	// `iface` is interface.xml (the scroll bar pieces); optionswin.xml is read from the install.
	Page(const ron::GameFiles& files, const ron::IfaceLayout& iface);
	~Page();
	bool ready() const;
	void open(const Host& host, int pane = 0);
	void close();
	bool is_open() const;
	int pane() const;
	void set_pane(int pane);

	// Draws the window with its top-left corner at (ox, oy) UI pixels; input uses the same space.
	void draw(ron::metal::UiRenderer& ui, int ox, int oy);
	bool mouse_move(int x, int y);
	bool mouse_down(int x, int y);
	bool mouse_drag(int x, int y);
	bool mouse_up(int x, int y);
	bool wheel(double dy, int x, int y);
	// A key press (Mac virtual key code, the first character, modifiers). True when the page took it.
	bool key(unsigned short code, unsigned ch, bool ctrl, bool alt, bool shift);
	// Headless test hooks (snapshots): select a command row of the Hotkeys pane / press the key it would capture.
	void test_select_hotkey(const std::string& option_name);
	void test_open_combo(int index);
	std::string state_text() const;

private:
	struct Impl;
	std::unique_ptr<Impl> p_;
};

}  // namespace optui
