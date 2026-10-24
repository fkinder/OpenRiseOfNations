// Conquer the World: the campaign data model (docs/ctw-spec.md section 1) read from the install:
// conquest/CTW_<Name>_Map_01.xml (ROOT, COUNTRY / LINK, COLONY, TRIBES, BONUSCARDDECK, CONQUESTSTYLES) and
// Data/conquestrules.xml (cards, stances, attitudes, ARMIES, BUILDINGS_*, RESOURCEBONUS_*, MIN_NO_ATTACK).
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

#include "sim/ctw.hpp"
#include "sim/rules_xml.hpp"

namespace ron::sim::ctw {

std::string lower(std::string s) {
	for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}
bool ieq(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

int K(const sim::Rules& r, const char* name) {
	const int off = constant_offset(name);
	return off < 0 ? 0 : r.constants.at(uint32_t(off));
}

namespace {

ustr U(const char* s) { return from_ascii(s); }
std::string S(const ustr& s) { return to_utf8(s); }

std::string trim(std::string s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
	return s.substr(a, b - a);
}

std::string attr(const XNode* e, const char* name) { return e ? S(xml::get_attrib(e, U(name))) : std::string(); }
int attr_int(const XNode* e, const char* name, int def = 0) {
	const std::string v = trim(attr(e, name));
	if (v.empty()) return def;
	return std::atoi(v.c_str());
}
bool attr_bool(const XNode* e, const char* name) { return attr_int(e, name, 0) != 0; }
std::string text_of(const XNode* e, const char* child) { return e ? trim(S(xml::get_text(e, U(child)))) : std::string(); }

int find_name(const std::vector<std::string>& names, const std::string& v) {
	const std::string t = lower(trim(v));
	for (size_t i = 0; i < names.size(); i++) {
		if (lower(names[i]) == t) return int(i);
	}
	return -1;
}

// ---------------------------------------------------------------- styles

StyleDef read_style(const XNode* e, int category, const std::vector<std::string>& map_styles) {
	StyleDef s;
	s.category = category;
	for (const auto& kv : e->attrs) s.attr[lower(S(kv.first))] = S(kv.second);
	auto get = [&](const char* k) {
		auto it = s.attr.find(k);
		return it == s.attr.end() ? std::string() : it->second;
	};
	auto num = [&](const char* k, int def) {
		auto it = s.attr.find(k);
		if (it == s.attr.end() || trim(it->second).empty()) return def;
		return std::atoi(it->second.c_str());
	};
	s.title = get("title");
	s.id = get("id");
	s.setup = get("setup");
	s.runtime = get("runtime");
	s.bonus = get("bonus");
	s.custom_map = get("custom_map");
	s.objective = get("objective");
	s.description = get("description");
	s.victory_text = get("victory_text");
	s.defeat_text = get("defeat_text");
	const std::string ms = get("map_style");
	s.map_style = ms.empty() ? int(kMsStandard) : std::max(0, find_name(map_styles, ms));
	s.probability = num("probability", 100);
	s.min_players = num("min_players", 2);
	s.min_victory = num("min_victory", 0);
	s.max_victory = num("max_victory", 0);
	s.min_age = num("min_age", 0);
	s.reveal_buildings = num("reveal_buildings", 0);
	for (int d = 0; d < 6; d++) {
		const std::string k = "add_opponents_diff" + std::to_string(d);
		s.add_opponents[size_t(d)] = s.attr.count(k) ? num(k.c_str(), 0) : num("add_opponents", 0);
	}
	s.land_only = num("land_only", 0) != 0;
	s.water_only = num("water_only", 0) != 0;
	s.opp_team_together = num("opp_team_together", 0) != 0;
	s.is_default = num("default", 0) != 0;
	s.no_rare = num("no_rare", 0) != 0;
	s.no_hero = num("no_hero", 0) != 0;
	s.not_first_time = num("not_first_time", 0) != 0;
	s.enemy_override = num("enemy_override", 0) != 0;
	s.enemy_override_tribe = get("enemy_override_tribe");
	s.player_start_x = num("player_start_x", -1);
	s.player_start_y = num("player_start_y", -1);
	s.ai_start_x = num("ai_start_x", -1);
	s.ai_start_y = num("ai_start_y", -1);
	return s;
}

std::vector<StyleDef> read_styles(const XNode* root, const std::vector<std::string>& map_styles) {
	std::vector<StyleDef> out;
	const XNode* cs = xml::child(root, U("CONQUESTSTYLES"));
	if (!cs) return out;
	const char* groups[6] = {"OFFENSIVE", "DEFENSIVE", "BARBARIAN", "CUSTOM", "ESPIONAGEUSA", "ESPIONAGEUSSR"};
	for (int g = 0; g < 6; g++) {
		const XNode* grp = xml::child(cs, U(groups[g]));
		if (!grp) continue;
		for (const XNode* st : xml::children(grp, U("STYLE"))) out.push_back(read_style(st, g, map_styles));
	}
	return out;
}

}  // namespace

int StyleDef::num(const char* k, int def) const {
	auto it = attr.find(k);
	if (it == attr.end() || trim(it->second).empty()) return def;
	return std::atoi(it->second.c_str());
}
const std::string& StyleDef::str(const char* k) const {
	static const std::string empty;
	auto it = attr.find(k);
	return it == attr.end() ? empty : it->second;
}

int CampaignDef::country(const std::string& id) const {
	const std::string t = lower(trim(id));
	for (size_t i = 0; i < countries.size(); i++) {
		if (lower(countries[i].id) == t) return int(i);
	}
	for (size_t i = 0; i < countries.size(); i++) {
		if (lower(countries[i].name) == t) return int(i);
	}
	return -1;
}

int CampaignDef::tribe_by_key(const std::string& key) const {
	const std::string t = lower(trim(key));
	for (int i = 0; i < kSlots; i++) {
		if (tribes[size_t(i)].present && lower(tribes[size_t(i)].key) == t) return i;
	}
	return -1;
}

// ---------------------------------------------------------------- conquestrules.xml

std::shared_ptr<const ConquestRules> load_conquest_rules(const GameFiles& files) {
	auto R = std::make_shared<ConquestRules>();
	const XDoc d = read_game_xml(files, "conquestrules.xml");
	const XNode* root = d.root;
	if (!root) throw std::runtime_error("conquestrules.xml is empty");
	int ncards = 0;
	for (const XNode* cats : xml::children(root, U("CATEGORIES"))) {
		const std::string id = attr(cats, "id");
		const auto items = xml::children(cats, U("CATEGORY"));
		if (id == "conquest_bonus_cards") {
			for (const XNode* c : items) {
				if (ncards >= kNumCardTypes) break;
				CardTypeDef& t = R->cards[size_t(ncards)];
				t.name = attr(c, "name");
				t.key = attr(c, "key");
				t.desc = attr(c, "desc");
				const auto data = xml::children(c, U("DATA"));
				for (size_t k = 0; k < 6 && k < data.size(); k++) t.data[k] = std::atoi(S(xml::text(data[k])).c_str());
				ncards++;
			}
		} else if (id == "conquest_stances") {
			for (const XNode* c : items) R->stances.push_back(attr(c, "name"));
		} else if (id == "conquest_attitudes") {
			for (const XNode* c : items) {
				R->attitude_names.push_back(attr(c, "name"));
				const auto data = xml::children(c, U("DATA"));
				const int lo = data.size() > 0 ? std::atoi(S(xml::text(data[0])).c_str()) : 0;
				const int hi = data.size() > 1 ? std::atoi(S(xml::text(data[1])).c_str()) : 0;
				R->attitude_bands.push_back({lo, hi});
			}
		} else if (id == "conquest_map_styles") {
			for (const XNode* c : items) R->map_style_names.push_back(attr(c, "name"));
		} else if (id == "conquest_game_cats") {
			for (const XNode* c : items) R->game_cat_names.push_back(attr(c, "name"));
		} else if (id == "conquest_rankings") {
			for (const XNode* c : items) R->rankings.push_back(attr(c, "name"));
		}
	}
	if (ncards != kNumCardTypes)
		throw std::runtime_error("conquestrules.xml: " + std::to_string(ncards) + " bonus card types (expected 38)");
	for (int t = 0; t < kNumCardTypes; t++) {
		CardTypeDef& c = R->cards[size_t(t)];
		c.variants = t == kOath ? 24 : t == kEureka ? 5 : t == kEconomicBoom ? 5 : 1;
	}
	R->styles = read_styles(root, R->map_style_names);
	if (const XNode* a = xml::child(root, U("ARMIES"))) {
		R->min_arrival_time = attr_int(a, "min_arrival_time", 2);
		R->max_arrival_time = attr_int(a, "max_arrival_time", 4);
		for (const XNode* u : xml::children(a, U("UNIT"))) {
			ArmyUnit au;
			au.type = attr(u, "type");
			au.num = attr_int(u, "num", 1);
			au.min_diff = attr_int(u, "min_diff", 0);
			R->army.push_back(au);
		}
	}
	if (const XNode* m = xml::child(root, U("MIN_NO_ATTACK"))) {
		for (int k = 0; k < 6; k++) {
			const std::string tag = "DIFFICULTY" + std::to_string(k);
			if (const XNode* e = xml::child(m, U(tag.c_str()))) R->min_no_attack[size_t(k)] = attr_int(e, "val", R->min_no_attack[size_t(k)]);
		}
	}
	const std::vector<std::string> diffs = {"easiest", "easy", "moderate", "tough", "tougher", "toughest"};
	const std::string dd = trim(S(xml::get_text(root, U("DEFAULT_BUILD_DIFF"))));
	R->default_build_diff = std::max(0, find_name(diffs, dd));
	for (int k = 0; k < 6; k++) {
		std::string up = diffs[size_t(k)];
		for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
		if (const XNode* b = xml::child(root, U(("BUILDINGS_" + up).c_str()))) {
			int lv = 0;
			for (const XNode* L : xml::children(b, U("LEVEL"))) {
				if (lv >= 8) break;
				for (const XNode* C : xml::children(L, U("CITY"))) {
					CityBuild cb;
					for (const XNode* B : xml::children(C, U("BUILD"))) {
						BuildItem bi;
						bi.type = attr(B, "type");
						bi.alt = attr(B, "alt");
						bi.num = attr_int(B, "num", 1);
						cb.push_back(bi);
					}
					R->build_out[size_t(k)][size_t(lv)].push_back(std::move(cb));
				}
				lv++;
			}
		}
		if (const XNode* b = xml::child(root, U(("RESOURCEBONUS_" + up).c_str()))) {
			int lv = 0;
			for (const XNode* L : xml::children(b, U("LEVEL"))) {
				if (lv >= 8) break;
				R->resource_bonus[size_t(k)][size_t(lv++)] = attr_int(L, "bonus", 1);
			}
		}
	}
	return R;
}

// ---------------------------------------------------------------- campaigns

namespace {

const char* const kCampaignOrder[] = {"alexander", "napoleon", "new_world", "cold_war", "world"};

int campaign_rank(const std::string& stem) {
	const std::string l = lower(stem);
	for (int i = 0; i < 5; i++) {
		if (l == std::string("ctw_") + kCampaignOrder[i] + "_map_01") return i;
	}
	return 100;
}

}  // namespace

std::vector<std::string> list_campaigns(const GameFiles& files) {
	std::vector<std::string> out;
	for (const std::string& rel : files.list_dir("conquest")) {
		const std::string base = rel.substr(rel.find_last_of('/') == std::string::npos ? 0 : rel.find_last_of('/') + 1);
		if (base.size() < 12 || base.compare(0, 4, "ctw_") != 0) continue;
		if (base.size() < 7 || base.compare(base.size() - 7, 7, "_01.xml") != 0) continue;
		if (base.find("_map_01.xml") != base.size() - 11) continue;
		const std::string real = std::filesystem::path(files.resolve(rel)).stem().string();
		out.push_back(real);
	}
	std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
		const int ra = campaign_rank(a), rb = campaign_rank(b);
		return ra != rb ? ra < rb : a < b;
	});
	return out;
}

std::shared_ptr<const CampaignDef> load_campaign(const GameFiles& files, const sim::Rules& rules, const std::string& stem) {
	auto C = std::make_shared<CampaignDef>();
	C->file = stem;
	const std::string rel = "conquest/" + stem + ".xml";
	const XDoc d = read_game_xml(files, rel);
	const XNode* root = d.root;
	if (!root) throw std::runtime_error(rel + " is empty");
	for (const auto& kv : root->attrs) C->root[lower(S(kv.first))] = S(kv.second);
	C->name = attr(root, "name");
	C->desc = attr(root, "desc");
	C->intro = attr(root, "intro");
	C->victory_conditions = attr(root, "victory_conditions");
	C->starting_age = attr_int(root, "starting_age", 0);
	C->ending_age = attr_int(root, "ending_age", 7);
	C->starting_round = attr_int(root, "starting_round", 0);
	C->turns_per_age = std::max(1, attr_int(root, "turns_per_age", 2));
	C->tex_width = attr_int(root, "tex_width", 0);
	C->tex_height = attr_int(root, "tex_height", 0);
	C->hero_unit = attr(root, "hero_unit");
	C->allow_vassals = attr_bool(root, "allow_vassals");
	C->allow_continue = attr_bool(root, "allow_continue");
	C->allow_wrap = attr_bool(root, "allow_wrap");
	C->strategic_nukes = attr_bool(root, "strategic_nukes");
	C->ally_move = attr_bool(root, "ally_move");
	C->free_move = attr_bool(root, "free_move");
	C->no_gov_hero = attr_bool(root, "no_gov_hero");
	C->territory_income = attr_bool(root, "territory_income");
	C->no_oath_cards = attr_bool(root, "no_oath_cards");
	C->no_patriot = attr_bool(root, "no_patriot");
	C->client_state = attr_bool(root, "client_state");
	C->allow_random_nations = attr_bool(root, "allow_random_nations");
	C->allow_random_bonuses = attr_bool(root, "allow_random_bonuses");
	C->battle_script = attr(root, "battle_script_file");
	C->post_turn_script = attr(root, "post_turn_script_file");
	C->diplo_response_script = attr(root, "diplo_response_script_file");
	C->diplo_offer_script = attr(root, "diplo_offer_script_file");
	C->links_script = attr(root, "links_script_file");
	C->initialization_script = attr(root, "initialization_script_file");
	C->strategy_script = attr(root, "strategy_script_file");
	C->post_ai_script = attr(root, "post_ai_script_file");
	if (const std::string ren = files.resolve("conquest/RenderMaps/RENDER_" + stem + ".png"); !ren.empty())
		C->map_art = "conquest/RenderMaps/RENDER_" + stem + ".png";
	if (!files.resolve("conquest/" + stem + ".png").empty()) C->pick_map = "conquest/" + stem + ".png";

	if (const XNode* cs = xml::child(root, U("CONTINENTS"))) {
		for (const XNode* c : xml::children(cs, U("CONTINENT"))) {
			ContinentDef cd;
			cd.name = attr(c, "name");
			cd.id = attr(c, "id");
			cd.barbarian_file = attr(c, "barbarian_file");
			cd.image = attr(c, "image_file");
			C->continents.push_back(cd);
		}
	}
	// The conquest style names the files use come from conquestrules.xml.
	const auto cr = load_conquest_rules(files);
	C->styles = read_styles(root, cr->map_style_names);

	for (const XNode* c : xml::children(root, U("COUNTRY"))) {
		CountryDef k;
		k.name = attr(c, "name");
		k.id = attr(c, "id");
		if (k.id.empty()) k.id = k.name;
		k.continent = attr(c, "continent");
		k.tileset = attr(c, "tileset");
		k.owner = trim(attr(c, "owner"));
		k.wonder = trim(attr(c, "wonder"));
		k.rare = trim(attr(c, "rare"));
		k.card = trim(attr(c, "card"));
		k.tribute = trim(attr(c, "tribute"));
		k.value = attr_int(c, "value", 0);
		k.size = attr_int(c, "size", 0);
		k.red = attr_int(c, "red"), k.green = attr_int(c, "green"), k.blue = attr_int(c, "blue");
		k.nukes = attr_int(c, "nukes", 0);
		k.capital = attr_bool(c, "capital");
		k.supply_depot = attr_bool(c, "supply_depot");
		k.priority_target = attr_bool(c, "priority_target");
		k.left = attr_int(c, "left"), k.right = attr_int(c, "right"), k.top = attr_int(c, "top"), k.bottom = attr_int(c, "bottom");
		k.x = attr_int(c, "x"), k.y = attr_int(c, "y");
		for (const XNode* l : xml::children(c, U("LINK"))) {
			LinkDef ld;
			ld.to = attr(l, "to");
			ld.sea = attr_bool(l, "sea");
			ld.from = attr(l, "from");
			k.links.push_back(ld);
		}
		C->countries.push_back(std::move(k));
	}
	for (const XNode* c : xml::children(root, U("COLONY"))) {
		ColonyDef k;
		k.name = attr(c, "name");
		k.id = attr(c, "id");
		if (k.id.empty()) k.id = k.name;
		k.owner = trim(attr(c, "owner"));
		k.wonder = trim(attr(c, "wonder"));
		k.value = attr_int(c, "value", 0);
		k.tribute = trim(attr(c, "tribute"));
		for (const XNode* r : xml::children(c, U("RARE"))) k.rares.push_back(attr(r, "type"));
		C->colonies.push_back(std::move(k));
	}
	for (const XNode* c : xml::children(root, U("BONUSCARDDECK"))) C->deck.push_back(attr(c, "name"));
	for (const XNode* c : xml::children(root, U("VALID_NATION"))) C->valid_nations.push_back(attr(c, "owner"));

	if (const XNode* ts = xml::child(root, U("TRIBES"))) {
		int slot = 0;
		for (const XNode* t : xml::children(ts, U("TRIBE"))) {
			if (slot >= kSlots) break;
			TribeDef& td = C->tribes[size_t(slot)];
			td.present = true;
			td.file = text_of(t, "FILE");
			td.key = text_of(t, "KEY");
			td.skill = [&] {
				const std::string s = text_of(t, "SKILL");
				return s.empty() ? -1 : std::atoi(s.c_str());
			}();
			td.no_eliminating = !text_of(t, "NO_ELIMINATING").empty() && std::atoi(text_of(t, "NO_ELIMINATING").c_str()) != 0;
			td.no_propose_ally = !text_of(t, "NO_PROPOSE_ALLY").empty() && std::atoi(text_of(t, "NO_PROPOSE_ALLY").c_str()) != 0;
			const std::string ba = text_of(t, "BASE_ARMIES");
			td.base_armies = ba.empty() ? 1 : std::atoi(ba.c_str());
			td.unique_unit = text_of(t, "UNIQUE_UNIT");
			if (const XNode* bc = xml::child(t, U("BONUS_CARDS"))) {
				for (const XNode* c : xml::children(bc, U("CARD"))) td.cards.push_back(trim(S(xml::text(c))));
			}
			// The nation file: display name, LEADERS, CITIES (the substitute tribe, spec section 1).
			if (!td.file.empty()) {
				std::string f = td.file;
				for (char& ch : f) {
					if (ch == '\\') ch = '/';
				}
				const XDoc nd = read_game_xml(files, f.find('/') == std::string::npos ? "./" + f : f, "", false);
				if (nd.root) {
					td.display_name = attr(nd.root, "name");
					if (const XNode* L = xml::child(nd.root, U("LEADERS"))) {
						for (const XNode* e : xml::children(L, U("LEADER"))) td.leaders.push_back(trim(S(xml::text(e))));
					}
					if (const XNode* L = xml::child(nd.root, U("CITIES"))) {
						for (const XNode* e : xml::children(L, U("CITY"))) td.cities.push_back(trim(S(xml::text(e))));
					}
				}
			}
			if (td.display_name.empty() && slot < int(rules.tribes.size())) td.display_name = S(rules.tribes[size_t(slot)].name);
			slot++;
		}
	}
	return C;
}

std::vector<std::string> validate_campaign(const CampaignDef& c) {
	std::vector<std::string> bad;
	for (const CountryDef& k : c.countries) {
		if (!k.owner.empty() && c.tribe_by_key(k.owner) < 0) bad.push_back(c.file + ": owner '" + k.owner + "' of " + k.id + " has no TRIBE");
		for (const LinkDef& l : k.links) {
			if (c.country(l.to) < 0) bad.push_back(c.file + ": link " + k.id + " -> '" + l.to + "' does not resolve");
		}
		if (!k.continent.empty()) {
			bool found = false;
			for (const ContinentDef& cd : c.continents) found |= ieq(cd.id, k.continent);
			if (!found) bad.push_back(c.file + ": continent '" + k.continent + "' of " + k.id + " is not declared");
		}
	}
	for (const ColonyDef& k : c.colonies) {
		if (!k.owner.empty() && c.tribe_by_key(k.owner) < 0) bad.push_back(c.file + ": owner '" + k.owner + "' of colony " + k.id + " has no TRIBE");
	}
	return bad;
}

}  // namespace ron::sim::ctw
