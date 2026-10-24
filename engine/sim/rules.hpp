// The game's rules tables (docs/rules-spec.md): every type (goods, units, buildings, the item, techs, crafts,
// bonuses), the constants, the combat balance table, the tribes and the categories, loaded from the install's XML
// exactly like the original loader (Game::init_rules_data + Types::init + Lands::init, then the per-game
// Types::finalize_grafting), plus the "rules image" codec: the byte stream Game::walk_rules_data writes into every
// replay, which playback loads over the XML tables.
//
// Port of re/tools/rulesdump.mjs + re/tools/rules/*.mjs. Correctness criterion: write_image() of the loaded tables is
// byte-identical to the image recorded in a replay of a game with the same setup.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/chunk.hpp"
#include "assets/files.hpp"
#include "sim/rules_xml.hpp"

namespace ron::sim {

// ---------------------------------------------------------------- TypeIndex ranges (compile-time in the original)
constexpr int kNumTypes = 806;
constexpr int kGoods = 0, kNumBasicGoods = 6, kEndGoods = 50;
constexpr int kUnits = 50, kEndUnits = 402, kGaia = 402, kEndGaia = 414;  // 50..401 units, 402..413 gaia
constexpr int kBuilds = 414, kEndBuilds = 543, kItem = 543;
constexpr int kTechs = 544, kAges = 544, kEndAges = 551, kEpochs = 551, kEndEpochs = 579, kEndTechs = 629;
constexpr int kSpells = 629, kDeclareWar = 668, kEndSpells = 684, kBonuses = 684, kEndBonuses = 806;
constexpr int kNumTribes = 24, kNumGraft = 352, kBalanceN = 493, kNumConstants = 0xd40 / 4;
constexpr int TYPE_NONE = -1, TYPE_ANY_BUILDING = -2, TYPE_ANY_UNIT = -3;

// The class of a TypeIndex. Units include gaia (402..413), which share UnitType.
enum class Kind : uint8_t { Good, Unit, Build, Item, Tech, Spell, Bonus };
Kind kind_of(int t);
inline bool is_object_kind(Kind k) { return k == Kind::Good || k == Kind::Unit || k == Kind::Build || k == Kind::Item; }

// SimpleArray<unsigned short> as walked: block is the growth step, flags the walked flag byte.
struct U16Array {
	int32_t count = 0;
	int32_t capacity = 0;
	uint16_t block = 0;
	uint8_t flags = 0;
	std::vector<uint16_t> v;
};

// Icon placement (TypeOut; interface only, not walked).
struct TexInfo {
	ustr id;
	int8_t col = 0, row = 0;
	int16_t x = 0, y = 0;
	int8_t clip = 0;
};

// Building footprints from masks.txt (docs/rules-spec.md section 8.6; not walked, simulation input).
struct BuildMask {
	bool present = false;
	int32_t w = 0, h = 0;
	std::vector<uint8_t> data;  // row-major, (u8) value; only 1 blocks
};
struct DooberMask {
	bool present = false;
	int32_t w = 0, h = 0;
	std::vector<int32_t> data;  // dir << 16 | flag << 8 | doober index, -1 empty
	std::vector<ustr> words;    // the raw grid words
};

// One rules type. A single struct covers every class: the walked fields of each class are listed in image order
// (sim/rules_image.cpp); the others are loader state the simulation or interface also reads.
struct Type {
	Kind kind = Kind::Bonus;
	bool loaded = false;  // allocated by Types::init (name keys skip types not created yet)

	// TypeData [0x04, 0x5e) (every type)
	int32_t type = 0;
	uint32_t job_time = 0, res_time = 0;
	int32_t tribe_mask = -1;
	int32_t cat = 0;
	std::array<int32_t, 6> costs{};
	std::array<int32_t, 3> preq{{-1, -1, -1}};
	int32_t from = -1, where = -1, upgrade = -1, jump = -1, obs = -2;
	std::array<int32_t, 2> show{{-2, -2}};
	int32_t modified = 0;
	int8_t grid_x = 0, grid_y = 0;
	ustr display_name;  // walked as a string
	// Type strings (not walked)
	ustr name, graph_name, help_name, type_name, abbrev, desc;

	// ObjectTypeData [0x1e4, 0x27c) (goods, units, buildings, item)
	int32_t obj_masks = 0, attack = 0, to_hit = 0, attenuate = 0, recharge = 0, min_range = 0, max_range = 0;
	int32_t splash_area = 0, splash_percent = 0, ammo_per_att = 0, proj_speed = 0, hits = -1, armor = -1, domain = -1;
	int32_t los = 0, science_los = 0, guy_spacing = 0, x_spacing = -1, y_spacing = -1, abil = -1, x_size = 0, y_size = 0;
	int32_t guy_radius = 0, block_radius = 0, big_radius = 0, new_block_radius = 0, new_big_radius = 0;
	int32_t fly_high = 0, fly_low = 0, block_points = 0, graft = -1, special_upgrade = -1, special_upgrade_cost = 0;
	std::array<int32_t, 2> support{{-1, -1}};
	std::array<int32_t, 2> support_cost{};
	int32_t age = -1;  // ObjectTypeData::age (-1 until finalize_grafting) / TechTypeData::age (0 default)
	U16Array is_list, is_strict_list;  // walked after ObjectTypeData

	// UnitTypeData (units 50..413); fire_proj and second_max_range are not walked
	uint32_t unit_flags = 0, unit_flags2 = 0;
	int32_t mode = 0, moves = 0, turn_speed = 0, role = 0, fire_proj = 0, second_max_range = 0;
	int32_t carry = 0, carry_size = 0, military_level = 0, research_premium_cost = 0, research_premium_time = 0;
	uint32_t job_extra_time = 0;
	int32_t mana = 0;  // also SpellTypeData::mana
	int32_t control_cost = 0, progression = 0, push_size = 0, push_circles = 0, target_size = 0, squad_size = 0;
	int32_t uber_size = 0, crew_size = 0, base_form = 0;
	std::vector<int16_t> relative_value;  // 352 for units: 8.8 fixed point minus 1.0 vs each unit 50..401

	// BuildTypeData [0x2b4, 0x2e5) (buildings 414..542)
	int32_t town_hits = 0, min_city_size = 0, misery_rate = 0;
	uint32_t build_flags = 0;
	int32_t most_shots = 0, garrison_max = 0, base_arrows = 0, wonder_val = 0, plunder_value = 0, plunder_good = 0;
	int32_t behind_height = 0, to = 0;
	int8_t civ_graph_mask = 0;
	ustr mask_name, doober_name;  // building_graphics.xml keys (not walked)
	BuildMask mask;
	DooberMask doober_mask;
	TexInfo tex;  // buildings and techs

	// GoodTypeData [0x2b4, 0x2f8) (goods 0..49)
	int32_t undiscovered_cost_good = 0, obsolete_cost_good = 0, undiscovered_support_good = 0;
	int32_t obsolete_support_good = 0, obsolete_production_good = 0, undiscovered_cost_rate = 0, obsolete_cost_rate = 0;
	int32_t undiscovered_support_rate = 0, obsolete_support_rate = 0, obsolete_production_rate = 0;
	std::array<int32_t, 2> bonus_type{};
	std::array<int32_t, 2> bonus_num{};
	int32_t largest_gather = 0, block = 0, exclude_ctw = 0;
	int32_t random_rare_drop = 0;  // not walked

	// TechTypeData [0x1c8, 0x1e3) (techs 544..628; age above)
	std::array<int16_t, 11> ai{};
	uint8_t leader_off = 0;
	std::array<ustr, 8> alternate_name;  // walked; epochs 551..578 have one per age

	// SpellTypeData [0x1c8, 0x1f8) (crafts 629..683; mana above)
	uint32_t spell_flags = 0;
	int32_t spell_range = 0, duration = 0, duration_upgrade = 0, from2 = 0;
	std::array<int32_t, 6> use{};
	int8_t grid_x2 = 0, grid_y2 = 0;  // not walked
};

// The constructor defaults of each class (Type::Type, ObjectType::ObjectType, UnitType::UnitType, ...).
Type make_type(Kind kind);

// Tribe (nation). Walked: [0x54, 0x6c) and graft.
struct Tribe {
	ustr file, name, eng_name, old_name;
	int32_t tribe = 0, barbarian = 0, people = 0, backup_build_continent = 0, build_continent = 0, unit_continent = 0;
	int32_t text_substitute = 0;  // not walked
	std::array<int32_t, kNumGraft> graft{};  // per unit 50..401: this tribe's replacement unit
};

struct Category {
	ustr name, key, desc;
	std::array<int32_t, 6> data{};
	int32_t alpha_order = 0;
};
struct CategoryList {
	std::string var;  // global the game stores it in ("unit_cats", ...)
	ustr id, title, list_name;
	bool mismatch = false;  // the game reports "Category data mismatch" and continues
	std::vector<Category> items;
	int find_key(const ustr& key) const;  // Categories::find_key: case-insensitive, -1 if none
};

// rules.xml CONSTANTS: the first 0xd40 bytes of struct Constants, as int32 by byte offset / 4.
struct Constants {
	std::array<int32_t, kNumConstants> v{};
	int32_t at(uint32_t byte_offset) const { return v[byte_offset / 4]; }
	// Offsets the loader itself reads.
	static constexpr uint32_t kUnitFormationSpacing = 0x0, kUnitBlockRadius = 0x10, kUnitGuySpacing = 0x14;
	static constexpr uint32_t kOneAgeDown = 0x70, kTwoAgesDown = 0x74, kThreeAgesDown = 0x78, kFourAgesDown = 0x7c,
	                          kFiveAgesDown = 0x80, kMongolThreeMilCavalry = 0x804;
};
// Byte offset of a named constant (spec section 12.6 names), or -1.
int constant_offset(const char* name);

struct LandMake {
	int32_t num = -1;
	int32_t good = TYPE_NONE;
};

// GameInfo values the loader reads through TypeData::get_preq, and GameInfo flag 8 for finalize_grafting.
struct GameSetup {
	int32_t starting_technology = 0;
	int32_t starting_technology2 = 0;
	int32_t ending_technology = 7;
	int32_t game_rules = 0;
	bool flag8 = false;
};

struct LoadOptions {
	std::string lang_suffix;  // "" English, ".7" German, ".10" Spanish, ".12" French, ".16" Italian
	GameSetup load_game;      // GameInfo in effect while Types::init runs (the default game; spec section 15.1)
	bool masks = true;        // read masks.txt footprints (not part of the image)
};

struct Rules {
	std::vector<Type> types;  // kNumTypes, by TypeIndex
	Constants constants;
	std::vector<int16_t> balance;  // kBalanceN^2, [attacker - 50][defender - 50] percent modifiers
	std::vector<Tribe> tribes;     // kNumTribes
	std::vector<CategoryList> categories;
	std::vector<std::vector<LandMake>> lands;  // rules.xml LANDS (9)
	std::vector<std::string> warnings;          // non-fatal load messages

	int16_t balance_at(int attacker, int defender) const {
		return balance[size_t(attacker - kUnits) * kBalanceN + size_t(defender - kUnits)];
	}
	const CategoryList* category_list(const std::string& var) const;
};

// Program-start load (Game::init_rules_data, Types::init, Lands::init). Throws on a missing file or a failed count
// assert (85 techs, 28x8 epoch names, 50 resources, 129 buildings, 364 units, 1 item, 55 crafts, 122 bonuses,
// 24 tribes, 9 lands).
Rules load_rules(const GameFiles& files, const LoadOptions& opts = LoadOptions());

// Per-game finalisation (Game::init_data: Types::init_civ_masks + Types::finalize_grafting). After it the walked
// tables equal what RecordGame::write_rules records for a game with this setup. Run it once per loaded Rules.
void start_game(Rules& rules, const GameSetup& game);

// ---------------------------------------------------------------- rules image (Game::walk_rules_data)
constexpr uint8_t kMarkerGameRules = 0x92, kMarkerTribeRules = 0x8f;

// One walked field, for locating a byte offset. where: "type 123 (Hoplites)", "constants", "balance", "tribe 4";
// index: the element of an array field (or the constant / balance cell number), -1 for scalars.
struct ImageField {
	size_t offset = 0, size = 0;
	std::string where;
	const char* field = "";
	int index = -1;
};

Bytes write_image(const Rules& rules, std::vector<ImageField>* layout = nullptr);

// Decodes an image starting at `offset` (the marker byte) into the walked fields of `out` (types are created with
// their constructor defaults first; unwalked fields keep them). Returns the end offset. Throws FormatError.
size_t read_image(const Bytes& b, size_t offset, Rules& out);
// The same over tables that are already loaded: only the walked fields change (the loader-only state - masks, icons, category
// lists - stays). Used to adopt the rules block of a save or replay (sim/original_save.cpp).
size_t read_image_into(const Bytes& b, size_t offset, Rules& rules);

// A replay's rules block: the first offset where a whole image decodes (rulesimage.mjs findImage).
struct ReplayRules {
	size_t offset = 0, end = 0;
	Rules rules;
};
bool find_image(const Bytes& b, ReplayRules& out);

// A replay file's bytes, gunzipped when compressed (.rcx), as find_image expects.
Bytes read_replay(const std::string& path);

}  // namespace ron::sim
