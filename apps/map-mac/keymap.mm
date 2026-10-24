#include "keymap.hpp"

#include <cstring>

namespace keymap {

namespace {

const Entry kTable[] = {
#include "keymap_table.inc"
};
constexpr size_t kCount = sizeof(kTable) / sizeof(kTable[0]);

// The key of each entry now: the compiled default until the Options page (or the preferences file) rebinds it.
std::vector<std::string>& keys_table() {
	static std::vector<std::string> t = [] {
		std::vector<std::string> v;
		for (const Entry& e : kTable) v.push_back(e.keys);
		return v;
	}();
	return t;
}

// Mac virtual key codes (kVK_*, ANSI layout) to the key names of the table.
const char* key_name(unsigned short code) {
	switch (code) {
		case 0: return "A"; case 11: return "B"; case 8: return "C"; case 2: return "D"; case 14: return "E"; case 3: return "F";
		case 5: return "G"; case 4: return "H"; case 34: return "I"; case 38: return "J"; case 40: return "K"; case 37: return "L";
		case 46: return "M"; case 45: return "N"; case 31: return "O"; case 35: return "P"; case 12: return "Q"; case 15: return "R";
		case 1: return "S"; case 17: return "T"; case 32: return "U"; case 9: return "V"; case 13: return "W"; case 7: return "X";
		case 16: return "Y"; case 6: return "Z";
		case 29: return "0"; case 18: return "1"; case 19: return "2"; case 20: return "3"; case 21: return "4"; case 23: return "5";
		case 22: return "6"; case 26: return "7"; case 28: return "8"; case 25: return "9";
		case 43: return ","; case 47: return "."; case 44: return "/"; case 41: return ";"; case 39: return "'"; case 33: return "[";
		case 30: return "]"; case 42: return "\\"; case 27: return "-"; case 24: return "="; case 50: return "`";
		case 122: return "F1"; case 120: return "F2"; case 99: return "F3"; case 118: return "F4"; case 96: return "F5";
		case 97: return "F6"; case 98: return "F7"; case 100: return "F8"; case 101: return "F9"; case 109: return "F10";
		case 103: return "F11"; case 111: return "F12";
		case 53: return "Esc"; case 36: case 76: return "Enter"; case 49: return "Space"; case 48: return "Tab";
		case 51: return "Backspace"; case 117: return "Delete"; case 115: return "Home"; case 119: return "End";
		case 116: return "PgUp"; case 121: return "PgDn"; case 123: return "Left"; case 124: return "Right";
		case 125: return "Down"; case 126: return "Up"; case 114: return "Insert"; case 113: return "Pause";
		case 82: return "Num0"; case 83: return "Num1"; case 84: return "Num2"; case 85: return "Num3"; case 86: return "Num4";
		case 87: return "Num5"; case 88: return "Num6"; case 89: return "Num7"; case 91: return "Num8"; case 92: return "Num9";
		case 69: return "Num+"; case 78: return "Num-"; case 67: return "Num*"; case 75: return "Num/";
		default: return nullptr;
	}
}

}  // namespace

std::string key_string(unsigned short code, bool ctrl, bool alt, bool shift) {
	const char* k = key_name(code);
	if (!k) return {};
	std::string s;
	if (ctrl) s += "Ctrl+";
	if (alt) s += "Alt+";
	if (shift) s += "Shift+";
	return s + k;
}

const Entry* lookup(const std::string& keys, int mode, const std::function<bool(int)>& ctx_ok) {
	if (keys.empty()) return nullptr;
	// A key can mean several things (G garrisons units, trains at a barracks): an entry that names a type in the selection beats
	// the general "units" / "buildings" ones, then the table order decides.
	auto find = [&](int m, const std::string& k, bool train_only) -> const Entry* {
		for (int pass = 0; pass < 2; pass++) {
			for (const Entry& e : kTable) {
				if (e.mode != m || k != keys_table()[size_t(&e - kTable)]) continue;
				if ((pass == 0) != (e.ctx >= 0)) continue;
				if (train_only && std::strcmp(e.option, "OPTION_TRAIN") != 0) continue;
				if (ctx_ok && !ctx_ok(e.ctx)) continue;
				return &e;
			}
		}
		return nullptr;
	};
	for (int m : {mode, 0}) {
		if (const Entry* e = find(m, keys, false)) return e;
	}
	// TRAIN entries also match with Shift held (a batch of five).
	const size_t at = keys.find("Shift+");
	if (at != std::string::npos) {
		std::string plain = keys;
		plain.erase(at, 6);
		for (int m : {mode, 0}) {
			if (const Entry* e = find(m, plain, true)) return e;
		}
	}
	return nullptr;
}

std::string hotkey(const std::string& option, int param, const std::function<bool(int)>& ctx_ok, int mode) {
	for (const Entry& e : kTable) {
		if (option != e.option) continue;
		if (param != -2 && e.param != param) continue;
		if (mode >= 0 && e.mode != mode) continue;
		if (ctx_ok && !ctx_ok(e.ctx)) continue;
		const std::string& k = keys_table()[size_t(&e - kTable)];
		if (k.empty() || k.rfind("Mouse", 0) == 0 || k.rfind("Wheel", 0) == 0) continue;
		return k;
	}
	return {};
}

size_t count() { return kCount; }
const Entry& entry(size_t i) { return kTable[i]; }
std::string keys_of(size_t i) { return keys_table()[i]; }

std::string signature(size_t i) {
	const Entry& e = kTable[i];
	return std::string(e.name) + "|" + std::to_string(e.mode) + "|" + std::to_string(e.ctx) + "|" + std::to_string(e.param);
}

void set_keys(size_t i, const std::string& keys) {
	if (i < kCount) keys_table()[i] = keys;
}

void reset_keys() {
	for (size_t i = 0; i < kCount; i++) keys_table()[i] = kTable[i].keys;
}

void apply_overrides(const std::map<std::string, std::string>& hotkeys) {
	reset_keys();
	for (size_t i = 0; i < kCount; i++) {
		auto it = hotkeys.find(signature(i));
		if (it != hotkeys.end()) keys_table()[i] = it->second;
	}
}

std::map<std::string, std::string> overrides() {
	std::map<std::string, std::string> out;
	for (size_t i = 0; i < kCount; i++) {
		if (keys_table()[i] != kTable[i].keys) out[signature(i)] = keys_table()[i];
	}
	return out;
}

std::vector<size_t> conflicts(size_t i, const std::string& keys) {
	std::vector<size_t> out;
	if (keys.empty() || i >= kCount) return out;
	const Entry& a = kTable[i];
	for (size_t j = 0; j < kCount; j++) {
		if (j == i || keys_table()[j] != keys || kTable[j].mode != a.mode) continue;
		const Entry& b = kTable[j];
		// Two bindings clash when a selection can satisfy both: the same context, or either is "always" / a broad one.
		const bool broad_a = a.ctx == -1, broad_b = b.ctx == -1;
		if (a.ctx == b.ctx || broad_a || broad_b) out.push_back(j);
	}
	return out;
}

}  // namespace keymap
