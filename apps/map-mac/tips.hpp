// Mouse-over help (docs/ui-spec.md 6.5): help.xml entries, the Buffer slot markup ({ } [ ] {*n and #ICONn), and the tooltip
// box Scene::render_mouseover_text draws bottom-left above the HUD (scroll plate tinted black, helpbox_size1..5 fonts,
// 1,1 black shadows, width at most half the screen). The caller builds a Tip (title, cost, hotkey, help lines) for the
// hovered element; this module lays it out and draws it.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/iface_layout.hpp"
#include "hud.hpp"
#include "ui.hpp"

namespace tips {

// A good in the cost string: icon n = good + 1 of icon_texs.xml (food, timber, wealth, knowledge, metal, oil), the amount,
// drawn in slot 1 (white) when affordable, slot 2 (red) when not.
struct Cost {
	int good = 0, amount = 0;
	bool affordable = true;
};

struct Tip {
	bool valid() const { return !first.empty(); }
	std::string first;              // info_first: the title, in slot-1 markup (white)
	std::vector<Cost> cost;         // resources_req
	std::string hotkey;             // the bound key ("G", "Ctrl+G"), empty when unbound
	std::vector<std::string> info;  // info_text: help lines (markup); a leading '\x01' marks a bullet line
	std::string last;               // info_last
	bool operator==(const Tip& o) const { return first == o.first && hotkey == o.hotkey && info == o.info && last == o.last && cost.size() == o.cost.size(); }
};

class Help {
public:
	bool init(const ron::GameFiles& files, std::string& error);
	// Scene::load_help_key: help.xml ENTRY `key` of one of the `sections` (the tooltip kind's group: BUILDINGS, UNITS, STATS, CAPS,
	// GOODS, MAIN_IFACE_BUTTONS, ...; empty = any), then key + "LV" (verbosity < 2) or "MV", then key + "NV" (< 3) or "V".
	// `vars` fills the $NUMBERn / $STRINGn placeholders; unknown ones are dropped. Bullet entries start with '\x01'.
	std::vector<std::string> lines(const std::string& key, int verbosity, const std::vector<std::string>& sections = {},
	                               const std::map<std::string, std::string>& vars = {}) const;
	bool has(const std::string& key, const std::vector<std::string>& sections = {}) const;
	// icon_texs.xml #ICONn (1-based); invalid when out of range.
	const ron::IfacePiece& icon(int n) const;

private:
	// SECTION -> upper-case key -> text with '\x01' bullets and '\n' breaks (the first ENTRY of a name in a section wins)
	std::map<std::string, std::map<std::string, std::string>> entries_;
	const std::string* find(const std::string& key, const std::vector<std::string>& sections) const;
	std::vector<ron::IfacePiece> icons_;
	ron::IfacePiece none_;
};

// Draws the box. `bottom` is the screen y the box's text bottom sits at (the HUD's top minus 4, else the screen's bottom minus 4).
void draw(ron::metal::UiRenderer& ui, const hud::Hud& hud, const Help& help, const Tip& tip, int verbosity, int bottom);

}  // namespace tips
