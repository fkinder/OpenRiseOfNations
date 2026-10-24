// The default hotkey table (docs/ui-spec.md 4.2): KeyMap's compiled-in defaults (keymap_table.inc, generated from the exe by
// re/tools/keymap-inc.mjs), key strings from Mac key events, the lookup KeyMap::lookup does (current mode, then mode 0;
// selection context gating; TRAIN also with Shift) and the display string tooltips show (KeyMap::get_hotkey_string).
#pragma once

#include <functional>
#include <string>

namespace keymap {

struct Entry {
	const char* name;    // KeyMapType name (OPTION_PAUSE2, OPTION_ARMORY_LIGHT, ...)
	const char* keys;    // "Ctrl+Alt+Shift+G": modifiers in that order, then the key name
	int mode;            // OptionMode: 0 standard, 1 build menu
	int ctx;             // type_selected: -1 always, -2 / -4 buildings, -3 units, -5 aircraft, -6 land / sea units, >= 0 a TypeIndex
	const char* option;  // OptionIndex name (OPTION_TRAIN, OPTION_BUILD, ...)
	int param;           // the entry's parameter (a TypeIndex for TRAIN / RESEARCH / BUILD, a good, a form), -1 none
};

// The key string of a Mac event: virtual key code (layout independent, as the Windows VK codes are) and modifiers
// (Control and Command both count as Ctrl, Option as Alt). Empty for keys the table has no name for.
std::string key_string(unsigned short key_code, bool ctrl, bool alt, bool shift);

// The first entry bound to `keys` in `mode` (then mode 0) whose selection context `ctx_ok` accepts.
const Entry* lookup(const std::string& keys, int mode, const std::function<bool(int ctx)>& ctx_ok);

// The key an option / parameter is bound to ("G", "Ctrl+G"), empty when unbound. `param` -2 matches any parameter,
// `mode` -1 any mode.
std::string hotkey(const std::string& option, int param, const std::function<bool(int ctx)>& ctx_ok = {}, int mode = -1);

}  // namespace keymap
