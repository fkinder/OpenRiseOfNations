#include "tips.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "assets/xml.hpp"

using ron::IfacePiece;
using ron::IfaceRect;
using ron::metal::UiRenderer;
using ron::metal::UiType;
using ron::metal::rgb;

namespace tips {

namespace {

std::string upper(std::string s) {
	for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

std::string decode_entities(std::string s) {
	static const std::pair<const char*, const char*> ents[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
	for (const auto& [from, to] : ents) {
		for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += 1) s.replace(p, std::strlen(from), to);
	}
	return s;
}

// A help.xml STRING body as text: <BULLET/> becomes '\x01', <SPACE/> a line break, other tags and comments vanish,
// whitespace runs collapse (the original's StringList keeps one entry per paragraph).
std::string flatten(const std::string& body) {
	std::string out;
	for (size_t i = 0; i < body.size();) {
		if (body.compare(i, 4, "<!--") == 0) {
			const size_t e = body.find("-->", i);
			i = e == std::string::npos ? body.size() : e + 3;
		} else if (body[i] == '<') {
			const size_t e = body.find('>', i);
			const std::string tag = body.substr(i, e == std::string::npos ? std::string::npos : e - i + 1);
			if (tag.compare(0, 7, "<BULLET") == 0) out += '\x01';
			else if (tag.compare(0, 6, "<SPACE") == 0) out += '\x02';
			i = e == std::string::npos ? body.size() : e + 1;
		} else {
			out += body[i++];
		}
	}
	out = decode_entities(out);
	std::string res;
	bool space = false;
	for (char c : out) {
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			space = true;
			continue;
		}
		if (space && !res.empty() && res.back() != '\x01' && res.back() != '\x02' && c != '\x01' && c != '\x02') res += ' ';
		space = false;
		res += c;
	}
	return res;
}

std::vector<std::string> split_lines(const std::string& text) {
	std::vector<std::string> out;
	std::string cur;
	bool bullet = false;
	auto flush = [&] {
		size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
		if (a != std::string::npos) out.push_back((bullet ? "\x01" : "") + cur.substr(a, b - a + 1));
		cur.clear();
	};
	for (char c : text) {
		if (c == '\x01') {
			flush();
			bullet = true;
		} else if (c == '\x02') {
			flush();
			bullet = false;
		} else {
			cur += c;
		}
	}
	flush();
	return out;
}

std::string substitute(const std::string& s, const std::map<std::string, std::string>& vars) {
	std::string out;
	for (size_t i = 0; i < s.size();) {
		if (s[i] != '$') {
			out += s[i++];
			continue;
		}
		size_t j = i + 1;
		while (j < s.size() && std::isupper(static_cast<unsigned char>(s[j]))) j++;
		const size_t name_end = j;
		while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) j++;
		const std::string tok = s.substr(i + 1, j - i - 1);
		if (name_end == i + 1) {
			out += s[i++];
			continue;
		}
		auto it = vars.find(tok);
		if (it != vars.end()) out += it->second;
		i = j;
	}
	return out;
}

// ---- layout: Buffer's slot markup, words and inline icons, greedy wrap

constexpr uint32_t kSlotColor[5] = {rgb(190, 190, 190), rgb(255, 255, 255), rgb(255, 0, 0), rgb(255, 255, 255), rgb(0, 100, 0)};
constexpr uint32_t kBlack = 0xFF000000;

struct Atom {
	enum Kind { Word, Space, Icon } kind = Word;
	std::string text;
	int slot = 0;
	int icon = 0;
	int w = 0;
};

struct Line {
	std::vector<Atom> atoms;
	int w = 0, h = 0, indent = 0;
	bool bullet = false;
};

struct Layouter {
	UiRenderer& ui;
	const Help& help;
	int fonts[5];

	Layouter(UiRenderer& u, const Help& h) : ui(u), help(h) {
		for (int i = 0; i < 5; i++) fonts[i] = ui.font_tag("helpbox_size" + std::to_string(i + 1));
	}
	int line_height(int slot) const {
		const ron::FontMetrics* m = ui.font_metrics(fonts[slot]);
		return m ? m->line_height : 14;
	}
	// Buffer::write_multi_font_raw_l's markup: '{' slot 1, '{*n' slot n, '[' slot 2, '}' and ']' slot 0, doubled = literal.
	std::vector<Atom> parse(const std::string& s, int start_slot = 0) const {
		std::vector<Atom> atoms;
		int slot = start_slot;
		std::string word;
		auto flush = [&] {
			if (word.empty()) return;
			Atom a;
			a.kind = Atom::Word, a.text = word, a.slot = slot;
			a.w = ui.text_width(fonts[slot], word);
			atoms.push_back(a);
			word.clear();
		};
		for (size_t i = 0; i < s.size();) {
			const char c = s[i];
			if (c == '{' || c == '[' || c == '}' || c == ']') {
				if (i + 1 < s.size() && s[i + 1] == c) {  // doubled: a literal
					word += c;
					i += 2;
					continue;
				}
				flush();
				if (c == '{' && i + 2 < s.size() && s[i + 1] == '*' && s[i + 2] >= '0' && s[i + 2] <= '5') {
					slot = s[i + 2] - '0';
					i += 3;
				} else {
					slot = c == '{' ? 1 : c == '[' ? 2 : 0;
					i++;
				}
				continue;
			}
			if (c == '#' && s.compare(i, 5, "#ICON") == 0 && i + 5 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 5]))) {
				flush();
				size_t j = i + 5;
				int n = 0;
				while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) n = n * 10 + (s[j++] - '0');
				Atom a;
				a.kind = Atom::Icon, a.icon = n, a.slot = slot;
				a.w = help.icon(n).valid() ? help.icon(n).width + 4 : 0;  // advances by the icon width + 4
				atoms.push_back(a);
				i = j;
				continue;
			}
			if (c == ' ') {
				flush();
				Atom a;
				a.kind = Atom::Space, a.text = " ", a.slot = slot;
				a.w = ui.text_width(fonts[slot], " ");
				atoms.push_back(a);
				i++;
				continue;
			}
			word += c;
			i++;
		}
		flush();
		return atoms;
	}
	// Greedy word wrap of `atoms` into lines no wider than `maxw`; `indent` applies to every line.
	void wrap(const std::vector<Atom>& atoms, int maxw, int indent, bool bullet, std::vector<Line>& out) const {
		Line cur;
		cur.indent = indent, cur.bullet = bullet;
		auto finish = [&] {
			while (!cur.atoms.empty() && cur.atoms.back().kind == Atom::Space) cur.w -= cur.atoms.back().w, cur.atoms.pop_back();
			int h = 0;
			for (const Atom& a : cur.atoms) h = std::max(h, a.kind == Atom::Icon ? 17 : line_height(a.slot));
			cur.h = h ? h : line_height(0);
			out.push_back(cur);
			cur = Line();
			cur.indent = indent;
		};
		for (const Atom& a : atoms) {
			if (a.kind == Atom::Space && cur.atoms.empty()) continue;
			if (a.kind != Atom::Space && !cur.atoms.empty() && indent + cur.w + a.w > maxw) finish();
			cur.atoms.push_back(a);
			cur.w += a.w;
		}
		if (!cur.atoms.empty() || out.empty()) finish();
	}
};

}  // namespace

bool Help::init(const ron::GameFiles& files, std::string& error) {
	try {
		const std::string text = files.read_text("Data/help.xml");
		// The groups are the children of <ROOT>; entries are looked up in the group of the tooltip's kind.
		std::string section;
		int depth = 0;
		for (size_t pos = 0; pos < text.size();) {
			const size_t lt = text.find('<', pos);
			if (lt == std::string::npos) break;
			if (text.compare(lt, 4, "<!--") == 0) {
				const size_t e = text.find("-->", lt);
				pos = e == std::string::npos ? text.size() : e + 3;
				continue;
			}
			const size_t gt = text.find('>', lt);
			if (gt == std::string::npos) break;
			if (text[lt + 1] == '?' || text[lt + 1] == '!') {
				pos = gt + 1;
				continue;
			}
			const bool closing = text[lt + 1] == '/', selfc = text[gt - 1] == '/';
			size_t ne = lt + 1 + (closing ? 1 : 0);
			while (ne < gt && !std::isspace(static_cast<unsigned char>(text[ne])) && text[ne] != '/' && text[ne] != '>') ne++;
			const std::string name = text.substr(lt + 1 + (closing ? 1 : 0), ne - lt - 1 - (closing ? 1 : 0));
			pos = gt + 1;
			if (name == "ENTRY" && !closing && !selfc) {
				const std::string tag = text.substr(lt, gt - lt);
				const size_t q = tag.find("name=\"");
				const size_t entry_end = text.find("</ENTRY>", gt);
				if (entry_end == std::string::npos) break;
				if (q != std::string::npos) {
					const size_t q2 = tag.find('"', q + 6);
					const std::string key = upper(tag.substr(q + 6, q2 - q - 6));
					auto& sec = entries_[section];
					if (!sec.count(key)) {
						const std::string body = text.substr(gt + 1, entry_end - gt - 1);
						const size_t s0 = body.find("<STRING");
						std::string content;
						if (s0 != std::string::npos) {
							const size_t s1 = body.find('>', s0);
							if (s1 != std::string::npos && body[s1 - 1] != '/') {
								const size_t s2 = body.find("</STRING>", s1);
								content = flatten(body.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1));
							}
						}
						sec[key] = content;
					}
				}
				pos = entry_end + 8;
				continue;
			}
			if (name == "STRING" || name == "BULLET" || name == "SPACE" || name == "TRANSLATION") continue;
			if (selfc) continue;
			if (closing) depth--;
			else {
				if (depth == 1) section = name;
				depth++;
			}
		}
		const ron::XmlElement doc = ron::parse_xml(files.read_text("Data/icon_texs.xml"));
		ron::for_each_element(doc, "IFACETEX", [&](const ron::XmlElement& e) { icons_.push_back(ron::iface_piece(e)); });
	} catch (const std::exception& e) {
		error = e.what();
		return false;
	}
	return true;
}

const std::string* Help::find(const std::string& key, const std::vector<std::string>& sections) const {
	const std::string k = upper(key);
	auto look = [&](const std::map<std::string, std::string>& sec) -> const std::string* {
		auto it = sec.find(k);
		return it != sec.end() && !it->second.empty() ? &it->second : nullptr;
	};
	if (sections.empty()) {
		for (const auto& [name, sec] : entries_) {
			if (const std::string* s = look(sec)) return s;
		}
		return nullptr;
	}
	for (const std::string& name : sections) {
		auto it = entries_.find(name);
		if (it == entries_.end()) continue;
		if (const std::string* s = look(it->second)) return s;
	}
	return nullptr;
}

bool Help::has(const std::string& key, const std::vector<std::string>& sections) const { return find(key, sections) != nullptr; }

const IfacePiece& Help::icon(int n) const { return n >= 1 && size_t(n) <= icons_.size() ? icons_[size_t(n - 1)] : none_; }

std::vector<std::string> Help::lines(const std::string& key, int verbosity, const std::vector<std::string>& sections,
                                     const std::map<std::string, std::string>& vars) const {
	std::vector<std::string> out;
	for (const std::string& name : {key, key + (verbosity < 2 ? "LV" : "MV"), key + (verbosity < 3 ? "NV" : "V")}) {
		const std::string* s = find(name, sections);
		if (!s) continue;
		for (std::string& l : split_lines(substitute(*s, vars))) out.push_back(std::move(l));
	}
	return out;
}

void draw(UiRenderer& ui, const hud::Hud& hud, const Help& help, const Tip& tip, int verbosity, int bottom) {
	if (!tip.valid() || verbosity <= 0) return;
	Layouter L(ui, help);
	const int maxw = ui.width() / 2;  // help_box_width
	std::vector<Line> lines;
	// Cost and hotkey strings (Scene::prepare_mouseover_text): " (Cost: {*1#ICON1 100}, ...)" and " (Hotkey: G)".
	std::string extra;
	if (!tip.cost.empty()) {
		extra += verbosity >= 2 ? " (Cost: " : " (";
		for (size_t i = 0; i < tip.cost.size(); i++) {
			if (i) extra += ", ";
			extra += std::string(tip.cost[i].affordable ? "{*1" : "{*2") + "#ICON" + std::to_string(tip.cost[i].good + 1) + " " +
			         std::to_string(tip.cost[i].amount) + "}";
		}
		extra += ")";
	}
	if (!tip.hotkey.empty()) extra += (verbosity >= 2 ? " (Hotkey: " : " (") + tip.hotkey + ")";
	std::vector<Atom> title = L.parse("{" + tip.first + "}");
	std::vector<Atom> tail = L.parse(extra);
	int title_w = 0, tail_w = 0;
	for (const Atom& a : title) title_w += a.w;
	for (const Atom& a : tail) tail_w += a.w;
	if (title_w + tail_w <= maxw - 5) {
		title.insert(title.end(), tail.begin(), tail.end());
		L.wrap(title, maxw, 0, false, lines);
	} else {
		L.wrap(title, maxw, 0, false, lines);
		if (!tail.empty()) L.wrap(tail, maxw, 0, false, lines);
	}
	const bool any_info = !tip.info.empty() || !tip.last.empty();
	if (any_info && verbosity >= 2) {
		Line blank;
		blank.h = L.line_height(0);
		lines.push_back(blank);
	}
	for (const std::string& s : tip.info) {
		const bool bullet = !s.empty() && s[0] == '\x01';
		const int bullet_w = help.icon(44).valid() ? help.icon(44).width + 4 : 8;
		L.wrap(L.parse(bullet ? s.substr(1) : s), maxw, bullet ? bullet_w : 0, bullet, lines);
	}
	if (!tip.last.empty()) L.wrap(L.parse(tip.last), maxw, 0, false, lines);
	int w = 0, h = 0;
	for (const Line& l : lines) w = std::max(w, l.indent + l.w), h += l.h;
	w = std::min(w, maxw);
	const int x = 10, y = bottom - h;
	hud.scroll_plate(ui, {x - 8, x + w + 8, y - 4, y + h + 4}, kBlack);
	ui.next_layer();
	int cy = y;
	for (const Line& l : lines) {
		int cx = x;
		if (l.bullet && help.icon(44).valid()) {
			const IfacePiece& b = help.icon(44);
			ui.add(b, float(x), float(cy + (l.h - b.height) / 2));
		}
		cx += l.indent;
		for (const Atom& a : l.atoms) {
			if (a.kind == Atom::Icon) {
				const IfacePiece& ic = help.icon(a.icon);
				if (ic.valid()) ui.add(ic, float(cx), float(cy + (l.h - ic.height) / 2));
			} else if (a.kind == Atom::Word) {
				ui.text(L.fonts[a.slot], a.text, float(cx), float(cy + l.h - L.line_height(a.slot)), kSlotColor[a.slot], kBlack);
			}
			cx += a.w;
		}
		cy += l.h;
	}
	ui.next_layer();
}

}  // namespace tips
