// Map style data: Map::load_map_data @69dad0 / init_map_data @69f250 (docs/randommap-spec.md section 4), the tileset
// choice and its group data, the terrain-group table (docs/randommap-terrain-groups.md A1) and TerrainGroups::fill_fertile.
#include <cmath>

#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

namespace {

ustr U(const char* s) { return from_ascii(s); }

std::string lower(const std::string& s) { return lc(s); }

bool iequal(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

// Number at the start of a text and the first word after it.
void split_number(const ustr& s, int32_t* n, std::string* word) {
	size_t p = 0;
	while (p < s.size() && (s[p] == u' ' || s[p] == u'\t')) p++;
	*n = wtoi(s, p);
	if (p < s.size() && (s[p] == u'-' || s[p] == u'+')) p++;
	while (p < s.size() && s[p] >= u'0' && s[p] <= u'9') p++;
	while (p < s.size() && (s[p] == u' ' || s[p] == u'\t')) p++;
	std::string w;
	while (p < s.size() && ((s[p] >= u'a' && s[p] <= u'z') || (s[p] >= u'A' && s[p] <= u'Z') || s[p] == u'_')) {
		w.push_back(char(s[p] >= u'a' && s[p] <= u'z' ? s[p] - u'a' + u'A' : s[p]));
		p++;
	}
	*word = w;
}

}  // namespace

const char* style_file_name(int style) {
	static const char* names[kNumStyleNames] = {
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
		"OLDWORLD", "SAHARA", "AMAZON", "HIMALAYAS", "SOUTHWESTMESA", "AFRICA", "MEDITERRANEAN", "OUTBACK",
		"GREATLAKES", "WARRINGSTATES", "NEWWORLD", "COLONIALPOWERS", "EASTINDIES", "EASTWEST", "SEAPOWER", "NILEDELTA",
		"BRITISHISLES", "DEFAULT", "CONQUESTMAP", "CONQUESTMAPSEA", nullptr};
	return style >= 0 && style < kNumStyleNames ? names[style] : nullptr;
}

// Map::scale_by_area @6a0910 / scale_by_axis: R is the reference width (the "Standard" map size, map_sizes[3]).
int Gen::scale_by_area(int n) const {
	const int64_t R2 = int64_t(ref_w) * ref_w;
	return int(std::max<int64_t>(1, (R2 / 2 + int64_t(cells) * n) / R2));
}
int Gen::scale_by_axis(int n) const {
	return std::max(1, (ref_w / 2 + xs * n) / ref_w);
}

// Map::scale_number @6a0320: "N [AREA|SCALE|SCALEWATER|SCALELAND]"; a negative N means |N| * nations.
int Gen::scale_text(const ustr& text) const {
	int32_t n;
	std::string word;
	split_number(text, &n, &word);
	if (n < 0) n = -n * nations;
	if (word == "AREA") return scale_by_area(n);
	if (word == "SCALE") return scale_by_axis(n);
	if (word == "SCALEWATER" || word == "SCALELAND") {
		// `sea` = the summed size of the sea regions when the attribute is read: none exist while a map is being made.
		const int64_t sea = 0;
		const int64_t part = word == "SCALEWATER" ? sea : int64_t(cells) - sea;
		return int(float(scale_by_axis(n)) * 2.0f * float(part) / float(cells));
	}
	return n;
}

int Gen::scale_number(const XNode* el, const char16_t* attr, int def) const {
	if (!el) return def;
	const ustr s = xml::get_attrib(el, attr);
	if (s.empty()) return def;
	return scale_text(s);
}

const XNode* Gen::section(const char16_t* name) const {
	if (style_root) {
		if (const XNode* e = xml::child(style_root, name)) return e;
	}
	return def_root ? xml::child(def_root, name) : nullptr;
}

void Gen::read_map_element(const XNode* map) {
	if (!map) return;
	auto num = [&](const char16_t* tag, int* out) {
		if (const XNode* e = xml::child(map, tag)) *out = xml::get_attrib_num(e, u"value", -1);
	};
	num(u"RIVER_THRESHOLD", &river_thresh);
	num(u"GOODY_BOXES", &goody_factor);
	num(u"COMMON_RESOURCES", &common_factor);
	if (const XNode* e = xml::child(map, u"AVOID_CENTER")) avoid_center = scale_number(e, u"scalevalue");
	if (const XNode* e = xml::child(map, u"AVOID_CONTINENT")) {
		avoid_continent = std::max(scale_number(e, u"scalevalue"), rules.constants.at(uint32_t(constant_offset("min_continent_spacing"))));
	}
	static const char16_t* edge[4] = {u"AVOID_EDGE_0", u"AVOID_EDGE_1", u"AVOID_EDGE_2", u"AVOID_EDGE_3"};
	for (int i = 0; i < 4; i++) {
		if (const XNode* e = xml::child(map, edge[i])) avoid_edge[size_t(i)] = scale_number(e, u"scalevalue");
	}
	num(u"BASE_EDGE", &base_edge);
	num(u"SEA_MAP", &sea_map);
	static const char16_t* terr_tags[6] = {u"PLAYER_TERRITORY_BASE_HARD_LIMIT", u"PLAYER_TERRITORY_CIVIC_HARD_LIMIT",
	                                       u"PLAYER_TERRITORY_CITY_HARD_LIMIT", u"COLONIZED_TERRITORY_BASE_HARD_LIMIT",
	                                       u"COLONIZED_TERRITORY_CIVIC_HARD_LIMIT", u"COLONIZED_TERRITORY_CITY_HARD_LIMIT"};
	for (int i = 0; i < 6; i++) {
		if (const XNode* e = xml::child(map, terr_tags[i])) terr[size_t(i)] = xml::get_attrib_num(e, u"value", -1);
	}
}

// The tileset part of Map::init_map_data @69f250 (decompile read). It runs once for default.xml's <MAP> and once for the
// style file's <MAP>: a <TILESET> replaces the choice by a draw rnd(sum of the chances) (no draw when the sum is <= 1; the first
// child whose running sum reaches the draw wins; its `type` must name an available tileset), and a still empty choice is a
// uniform draw over the available tilesets (when there are several).
void Gen::choose_tileset() {
	available_tilesets.clear();
	const XDoc ts = read_game_xml(c.files, "tilesets.xml", "", false);
	for (const XNode* t : xml::children(ts.root, u"TILESET")) available_tilesets.push_back(to_utf8(xml::get_attrib(t, u"name")));
	tileset.clear();
	auto pass = [&](const XNode* root) {
		const XNode* map = root ? xml::child(root, u"MAP") : nullptr;
		if (!map) return;
		if (const XNode* tile = xml::child(map, u"TILESET")) {
			tileset.clear();
			const std::vector<XNode*> kids = xml::children(tile, u"TILECHANCE");
			int sum = 0;
			for (const XNode* k : kids) sum += xml::get_attrib_num(k, u"chance", -1);
			const int roll = sum - 1 < 1 ? 0 : r.rnd_draw(sum);
			int acc = 0;
			for (const XNode* k : kids) {
				acc += xml::get_attrib_num(k, u"chance", -1);
				if (roll <= acc) {
					const std::string name = to_utf8(xml::get_attrib(k, u"type"));
					for (const std::string& a : available_tilesets) {
						if (iequal(a, name)) tileset = a;
					}
					break;
				}
			}
		}
		if (tileset.empty() && !available_tilesets.empty()) {
			const int n = int(available_tilesets.size());
			tileset = available_tilesets[size_t(n - 1 < 1 ? 0 : r.rnd_draw(n))];
		}
	};
	pass(def_root);
	pass(style_root);
}

void Gen::read_tileset_group_data() {
	tgd = TileGroupData();
	const XDoc ts = read_game_xml(c.files, "tilesets.xml", "", false);
	for (const XNode* t : xml::children(ts.root, u"TILESET")) {
		if (!iequal(to_utf8(xml::get_attrib(t, u"name")), tileset)) continue;
		const XNode* g = xml::child(t, u"TERRAINGROUP");
		if (!g) return;
		auto num = [&](const char16_t* tag, int def) { return xml::get_attrib_num(xml::child(g, tag), u"value", def); };
		tgd.clump_factor = std::min(5, std::max(0, num(u"CLUMP_FACTOR", 3)));
		tgd.forest_base = num(u"FOREST_BASE", -1);
		tgd.forest_growth_prob = num(u"FOREST_GROWTH_PROB", 0);
		tgd.mountain_base = num(u"MOUNTAIN_BASE", -1);
		tgd.mountain_growth_prob = num(u"MOUNTAIN_GROWTH_PROB", 0);
		tgd.mnt_fringe_tree_prob = num(u"MOUNTAIN_FRINGE_TREE_PROB", 0);
		tgd.coast_base = num(u"COAST_BASE", -1);
		tgd.coast_growth_prob = num(u"COAST_GROWTH_PROB", 0);
		tgd.bush_clump_prob = num(u"BUSH_CLUMP_PROB", 0);
		tgd.bush_spacing = num(u"BUSH_SPACING", 0);
		tgd.bush_min = num(u"BUSH_MIN", 0);
		tgd.bush_max = num(u"BUSH_MAX", 0);
		tgd.mnt_rock_clump_prob = num(u"MOUNTAIN_ROCK_CLUMP_PROB", 0);
		tgd.mnt_rock_spacing = num(u"MOUNTAIN_ROCK_SPACING", 0);
		tgd.mnt_rock_min = num(u"MOUNTAIN_ROCK_MIN", 0);
		tgd.mnt_rock_max = num(u"MOUNTAIN_ROCK_MAX", 0);
		return;
	}
}

// TerrainGroups::init_tileset_data @6a61f0: the default file first, then the style file; the chosen tileset's element
// (<DIRTY>, <SW_DESERT>, ...) holds <LANDKEY name="baseland" frequency_0..3>.
void Gen::read_tileset_data(const XNode* root, bool is_default) {
	(void)is_default;
	if (!root) return;
	const XNode* data = xml::child(root, u"TILESET_DATA");
	if (!data) return;
	const XNode* ts = xml::child(data, from_ascii(tileset.c_str()));
	if (!ts) return;
	for (const XNode* k : xml::children(ts, u"LANDKEY")) {
		if (!iequal(to_utf8(xml::get_attrib(k, u"name")), "baseland")) continue;
		int n = 0;
		for (int i = 0; i < 4; i++) {
			const ustr key = U(("frequency_" + std::to_string(i)).c_str());
			if (!xml::get_attrib(k, key).empty()) {
				base_freq[size_t(i)] = xml::get_attrib_num(k, key, 0);
				n = i + 1;
			}
		}
		if (n) base_freq_count = n;
	}
}

static int good_key_of(const Rules& rules, const std::string& name) {
	const std::string key = lc(name);
	for (int i = kGoods; i < kEndGoods; i++) {
		const Type& t = rules.types[size_t(i)];
		if (!t.name.empty() && lc(to_utf8(t.name)) == key) return i;
		if (!t.type_name.empty() && lc(to_utf8(t.type_name)) == key) return i;
	}
	return -1;
}

int good_key(const Rules& rules, const std::string& name) { return good_key_of(rules, name); }

void Gen::init_resources(const XNode* root) {
	const XNode* res_el = root ? xml::child(root, u"RESOURCES") : nullptr;
	if (!res_el) return;
	for (int k = 0; k < 9; k++) {
		const XNode* land = xml::child(res_el, U(("LAND_" + std::to_string(k)).c_str()));
		if (!land) continue;
		lands_rare[size_t(k)].clear();
		const XNode* rare = xml::child(land, u"RARE_RESOURCES");
		if (!rare) continue;
		for (const XNode* e : xml::children(rare, u"RARE")) {
			const int g = good_key_of(rules, to_utf8(xml::get_attrib(e, u"name")));
			if (g >= 0) lands_rare[size_t(k)].push_back(g);  // an unknown name (key -3) is skipped
		}
	}
}

// Map::load_map_data: default.xml then the style file; every value the file has overrides the default.
void Gen::load_map_data() {
	terr[0] = terr[3] = rules.constants.at(uint32_t(constant_offset("territory_limit_base")));
	terr[1] = terr[4] = rules.constants.at(uint32_t(constant_offset("territory_limit_civic")));
	terr[2] = terr[5] = rules.constants.at(uint32_t(constant_offset("territory_limit_city")));
	def_doc = read_game_xml(c.files, "mapstyles/default.xml", "", false);
	def_root = def_doc.root;
	const char* sf = style_file_name(style);
	if (sf) {
		style_file = std::string("mapstyles/") + sf + ".xml";
		style_doc = read_game_xml(c.files, style_file, "", false);
		style_root = style_doc.root;
	}
	river_thresh = 8;
	read_map_element(def_root ? xml::child(def_root, u"MAP") : nullptr);
	read_map_element(style_root ? xml::child(style_root, u"MAP") : nullptr);
	// <TILESET> (draws) then the group data of the chosen tileset
	choose_tileset();
	read_tileset_group_data();
	base_freq = {{45, 25, 20, 10}};
	base_freq_count = 4;
	read_tileset_data(def_root, true);
	read_tileset_data(style_root, false);
	init_resources(def_root);
	init_resources(style_root);
	init_terrain_data();
}

void Gen::init_terrain_data() {
	groups.clear();
	const XNode* tg = section(u"TERRAIN_GROUPS");
	if (!tg) return;
	for (const XNode* e : xml::children(tg, u"GROUPENTRY")) {
		GroupEntry g;
		const std::string type = lc(to_utf8(xml::get_attrib(e, u"type")));
		if (type == "trees") g.type = kTrees;
		else if (type == "mountains") g.type = kMountains;
		else if (type == "rocks") g.type = kRocks;
		else if (type == "oil" || type == "sea oil") g.type = kOilGroup;
		else if (type == "cliffs") g.type = kCliffs;
		else continue;  // gather-land groups are unused by the shipped files
		g.chance = xml::get_attrib_num(e, u"chance", -1);
		g.grouping = xml::get_attrib_num(e, u"grouping", -1);
		g.min_clumps = scale_number(e, u"min_clumps");
		g.max_clumps = scale_number(e, u"max_clumps");
		const std::string pat = lc(to_utf8(xml::get_attrib(e, u"pattern")));
		g.pattern = pat == "continent" ? 1 : pat == "world" ? 2 : pat == "nonplayer" ? 3 : pat == "corner" ? 4 : 0;
		if (g.type == kMountains) {
			g.min_size = wtoi(xml::get_attrib(e, u"min_size"));
			g.max_size = wtoi(xml::get_attrib(e, u"max_size"));
		} else {
			g.min_size = scale_number(e, u"min_size");
			g.max_size = scale_number(e, u"max_size");
		}
		g.cliff_face = xml::get_attrib_num(e, u"cliff_face", -1);
		g.start_min = scale_number(e, u"city_keep_away");
		g.start_max = std::min(0x40, scale_number(e, u"city_stay_near"));
		auto clamp64 = [&](const char16_t* a) { return std::min(0x40, std::max(0, scale_number(e, a))); };
		g.forest_space = clamp64(u"forest_space");
		g.mount_space = clamp64(u"mount_space");
		g.rock_space = clamp64(u"rock_space");
		g.coast_space = clamp64(u"coast_space");
		g.cent_min = scale_number(e, u"cent_keep_away");
		g.cent_max = std::min(0x40, scale_number(e, u"cent_stay_near"));
		g.edge_min = scale_number(e, u"edge_keep_away");
		g.edge_max = std::min(0x40, scale_number(e, u"edge_stay_near"));
		g.corner_min = scale_number(e, u"corner_keep_away");
		g.corner_max = std::min(0x40, scale_number(e, u"corner_stay_near"));
		if (g.type == kRocks) {
			g.min_oil = scale_number(e, u"min_oil");
			g.max_oil = scale_number(e, u"max_oil");
		}
		groups.push_back(g);
	}
}

// Fractal::init @6aa2d0 (randommap-spec section 5): midpoint displacement on its own Random(world.seed).
void Gen::fractal_init() {
	const int smooth = std::min(5, std::max(0, tgd.clump_factor));
	const int step = 1 << smooth;
	const int W_ = xs + 1, H_ = ys + 1;
	const int GW = ((W_ - 1 + step - 1) / step) * step + 1, GH = ((H_ - 1 + step - 1) / step) * step + 1;
	std::vector<int> g(size_t(GW) * size_t(GH), 0);
	Random fr(world_seed);
	auto at = [&](int x, int y) -> int& { return g[size_t(x) * size_t(GH) + size_t(y)]; };
	for (int x = 0; x < GW; x += step) {
		for (int y = 0; y < GH; y += step) at(x, y) = fr.get(0, 255);
	}
	for (int level = smooth - 1; level >= 0; level--) {
		const int s = 1 << level, s2 = s * 2;
		const int range = 1 << (8 - smooth + level), half = 1 << (7 - smooth + level);
		auto disp = [&]() { return fr.get(0, range - 1) - half; };
		auto clamp = [](int v) { return std::min(255, std::max(0, v)); };
		for (int x = 0; x + s2 < GW + 0 && x + s2 <= GW - 1; x += s2) {
			for (int y = 0; y + s2 <= GH - 1; y += s2) {
				at(x + s, y + s) = clamp((at(x, y) + at(x + s2, y) + at(x, y + s2) + at(x + s2, y + s2) + 2) / 4 + disp());
			}
		}
		for (int x = 0; x < GW; x += s) {
			for (int y = 0; y < GH; y += s) {
				const bool xo = ((x / s) & 1) != 0, yo = ((y / s) & 1) != 0;
				if (xo == yo) continue;
				if (xo) {
					if (x + s >= GW) continue;
					at(x, y) = clamp((at(x - s, y) + at(x + s, y) + 1) / 2 + disp());
				} else {
					if (y + s >= GH) continue;
					at(x, y) = clamp((at(x, y - s) + at(x, y + s) + 1) / 2 + disp());
				}
			}
		}
	}
	fractal_raw.assign(size_t(W_) * size_t(H_), 0);
	for (int x = 0; x < W_; x++) {
		for (int y = 0; y < H_; y++) fractal_raw[size_t(x) * size_t(H_) + size_t(y)] = uint8_t(at(x, y));
	}
	partitions.clear();
	int acc = 0;
	for (int i = 0; i + 1 < base_freq_count; i++) {
		acc += int(float(base_freq[size_t(i)]) / 100.0f * 255.0f) & 0xff;
		partitions.push_back(uint8_t(acc));
	}
}

// TerrainGroups::fill_fertile @6a6f90: the baseland subtype is the noise field cut at the cumulative frequencies.
void Gen::fill_fertile() {
	fractal_init();
	const int H_ = ys + 1;
	for (int x = 0; x < xs; x++) {
		for (int y = 0; y < ys; y++) {
			WData& d = W(x, y);
			if (d.land != 0) continue;
			const int raw = fractal_raw[size_t(x) * size_t(H_) + size_t(y)];
			size_t k = 0;
			while (k < partitions.size() && raw >= partitions[k]) k++;
			d.land_sub = uint8_t(k);
		}
	}
}

}  // namespace ron::sim::mapgen
