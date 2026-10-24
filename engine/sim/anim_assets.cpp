#include "sim/anim_assets.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

#include "assets/chunk.hpp"
#include "assets/rules.hpp"
#include "assets/xml.hpp"
#include "sim/placement.hpp"
#include "sim/rules_xml.hpp"

// animmgr.frames is single-precision float math (AnimMgr::force_load @53b0a3); no fused multiply-add.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace ron::sim {

namespace {

// UnitAnimNames (dynamic initializer @40ee70): the <ANIM name> of unit_graphics.xml, index = UnitAnim.
constexpr const char* kUnitAnimNames[38] = {
    "CHAR_DEFAULT",       "CHAR_IDLE1",          "CHAR_IDLE2",          "CHAR_IDLE3",
    "CHAR_GROUP_IDLE1",   "CHAR_GROUP_IDLE2",    "CHAR_GROUP_IDLE3",    "CHAR_SLOG",
    "CHAR_WALK",          "CHAR_JOG",            "CHAR_ATTACKWALK",     "CHAR_ATTACK1",
    "CHAR_ATTACK2",       "CHAR_ATTACK3",        "CHAR_ATTACKSPECIAL",  "CHAR_DEATH_STAB1",
    "CHAR_DEATH_STAB2",   "CHAR_DEATH_SHOT1",    "CHAR_DEATH_SHOT2",    "CHAR_DEATH_SPLODED1",
    "CHAR_DEATH_SPLODED2", "CHAR_TURN_LEFT",     "CHAR_TURN_RIGHT",     "CHAR_PACK",
    "CHAR_UNPACK",        "CHAR_CHOP_WOOD",      "CHAR_WALK_WITH_WOOD", "CHAR_DUMP_WOOD",
    "CHAR_WALK_TO_WOOD",  "CHAR_MINE_ORE",       "CHAR_WALK_WITH_ORE",  "CHAR_DUMP_ORE",
    "CHAR_WALK_TO_ORE",   "CHAR_BUILD",          "CHAR_REPAIR",         "CHAR_SOW",
    "CHAR_REAP",          "CHAR_FARM"};
constexpr int32_t kNumUnitAnims = 38;

// The merchant build-style pieces [kTotalUnit - 6, kTotalUnit) (unit_node_from_gpiece's switch).
constexpr const char* kMerchantSpecial[6] = {"MERCHANT-KOREAN-AGE3",   "MERCHANT-NEUROPE-AGE3",
                                              "MERCHANT-IROQUOIS-AGE0", "MERCHANT-IROQUOIS-AGE3",
                                              "MERCHANT-COLONIAL-AGE3", "MERCHANT-EINDIAN-AGE3"};

std::string upper(std::string s) {
	for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

// TypeData::is_merchant @42dbd0: MERCHANT (61), MERCHANTDUTCH (62), FURTRAPPER (400).
bool is_merchant_type(int32_t t) { return t == 61 || t == 62 || t == 400; }

// Single-precision cvttss2si.
int32_t trunc_f(float f) { return int32_t(f); }

}  // namespace

AssetAnimSource::AssetAnimSource(const GameFiles& files, const Rules& rules) : files_(files) {
	// ---- per type: GRAPH and the meaning of the gender slot (unit_node_from_gpiece @902047: a unit type whose
	// role has the attacker bit 0x10000, a merchant, or one that is FISHERMEN (317) names it -PACKED, else -FEMALE).
	graph_.assign(rules.types.size(), std::string());
	gender_packed_.assign(rules.types.size(), false);
	for (size_t i = 0; i < rules.types.size(); i++) {
		const Type& t = rules.types[i];
		if (!t.loaded) continue;
		graph_[i] = upper(to_utf8(t.graph_name));
		if (i >= 50 && i < 414)
			gender_packed_[i] = (t.role & 0x10000) || is_merchant_type(int32_t(i)) || type_is(t, 317);
	}

	// ---- building pieces (building_graphics.xml ROOT > BUILDINGS > BUILD): GraphicPieces::build_node_from_gpiece looks a piece up by
	// the name GRAPH-STYLE-AGEn; the piece exists when the node is there and its art files load (a node naming a missing file does not).
	{
		const XmlElement doc = ron::parse_xml(files.read_text("Data/building_graphics.xml"));
		ron::for_each_element(doc, "BUILD", [&](const XmlElement& b) {
			const std::string name = upper(b.get("name"));
			if (name.empty()) return;
			auto ok = [&](const char* attr) {
				const std::string f = normalize_game_path(b.get(attr));
				return f.empty() || !files.resolve(f).empty();
			};
			if (ok("texture") && ok("zbuffer")) build_pieces_.insert(name);
		});
	}

	// ---- animmgr entries (init_anims_pool: ROOT > ANIMATIONS > LOOPING, NONLOOPING, BUILDING, PATH).
	std::unordered_map<std::string, int32_t> entry_by_name;  // lowercase name -> first entry (decipher_animation)
	{
		const XmlElement doc = ron::parse_xml(files.read_text("Data/anim_graphics.xml"));
		const XmlElement* root = doc.child("ROOT");
		const XmlElement* anims = root ? root->child("ANIMATIONS") : nullptr;
		const char* sections[4] = {"LOOPING", "NONLOOPING", "BUILDING", "PATH"};
		for (int s = 0; s < 4 && anims; s++) {
			const XmlElement* sec = anims->child(sections[s]);
			if (!sec) continue;
			for (const XmlElement& a : sec->children) {
				if (a.name != "ANIM") continue;
				Entry e;
				e.name = a.get("name");
				e.file = normalize_game_path(a.get("file"));
				e.looping = s == 0;
				entry_by_name.emplace(ron::to_lower(e.name), int32_t(entries_.size()));
				entries_.push_back(std::move(e));
			}
		}
	}

	// ---- AMMO pieces (effects_graphics.xml, document order: ammo piece k = first_ammo_piece + k) and their flags
	// (GraphicPieces ammo_flags; do_damage="0" is DO_NO_DAMAGE).
	std::unordered_map<std::string, int32_t> ammo_by_name;  // uppercase name -> first piece (String == ignores case)
	{
		const XmlElement doc = ron::parse_xml(files.read_text("Data/effects_graphics.xml"));
		ron::for_each_element(doc, "AMMO", [&](const XmlElement& a) {
			Ammo am;
			am.name = a.get("name");
			auto on = [&](const char* k) { return a.get_int(k, 0) != 0; };
			if (on("puncture")) am.flags |= ammoflag::kPuncture;
			if (on("puff")) am.flags |= ammoflag::kPuff;
			if (on("explosive")) am.flags |= ammoflag::kExplode;
			if (on("missile")) am.flags |= ammoflag::kMissile;
			if (on("flame")) am.flags |= ammoflag::kFlame;
			if (on("pulsewave")) am.flags |= ammoflag::kPulsewave;
			if (on("flak")) am.flags |= ammoflag::kFlak;
			if (a.has("do_damage") && a.get_int("do_damage", 1) == 0) am.flags |= ammoflag::kDoNoDamage;
			ammo_by_name.emplace(upper(am.name), int32_t(ammo_.size()));
			ammo_.push_back(std::move(am));
		});
	}
	// <SKELETON> / <SCORCH> names (GraphicPieces skeleton_names / scorch_names; String == ignores case).
	std::unordered_map<std::string, int32_t> skeletons, scorches;

	// ---- graphics pieces (unit_graphics.xml ROOT > UNITS > UNIT).
	{
		const XmlElement doc = ron::parse_xml(files.read_text("Data/unit_graphics.xml"));
		ron::for_each_element(doc, "SKELETON", [&](const XmlElement& e) { skeletons.emplace(upper(e.get("name")), 1); });
		ron::for_each_element(doc, "SCORCH", [&](const XmlElement& e) { scorches.emplace(upper(e.get("name")), 1); });
		const XmlElement* root = doc.child("ROOT");
		const XmlElement* list = root ? root->child("UNITS") : nullptr;
		for (size_t k = 0; list && k < list->children.size(); k++) {
			const XmlElement& u = list->children[k];
			if (u.name != "UNIT") continue;
			UnitArt art;
			art.model = normalize_game_path(u.get("model"));
			art.model_ok = !art.model.empty() && !files.resolve(art.model).empty();
			// init_unit_data: scale < 0.1 (or absent) -> 1; track offsets default 0.
			art.scale = u.get_float("scale", 0.0f);
			if (art.scale < 0.1f) art.scale = 1.0f;
			art.track_x = u.get_float("trackoffsetx", 0.0f);
			art.track_y = u.get_float("trackoffsety", 0.0f);
			// AnimationPacket::init_animation for actions 0..37 with -1, then each <ANIM>: the action by name
			// (an unknown name reports an error and uses index 38), the entry by decipher_animation (no match:
			// error, skipped). Later duplicates overwrite.
			art.ids.assign(size_t(kNumUnitAnims), -1);
			art.skeleton = skeletons.count(upper(u.get("skeleton"))) != 0;
			art.scorch = scorches.count(upper(u.get("scorch"))) != 0;
			// GraphicEvents::init_unit_events @8e2520: RELEASEEVENT (event type 1), INSTANTHIT, NORELEASE.
			for (const XmlElement& c : u.children) {
				if (c.name == "RELEASEEVENT") {
					const std::string an = upper(c.get("anim"));
					int32_t idx = 0;
					while (idx < kNumUnitAnims && an != kUnitAnimNames[idx]) idx++;
					if (idx == kNumUnitAnims) {
						warnings_.push_back(u.get("name") + ": unknown release animation " + c.get("anim"));
						continue;
					}
					ReleaseEvent ev;
					ev.anim = idx;
					ev.frame = std::max(1, int32_t(uint16_t(c.get_int("starttime", 0))) / 67);
					auto it = ammo_by_name.find(upper(c.get("type")));
					if (it == ammo_by_name.end()) warnings_.push_back(u.get("name") + ": unknown ammo " + c.get("type"));
					ev.ammo = it == ammo_by_name.end() ? 0 : it->second;  // unknown names fall back to ammo piece 0
					ev.resolved = it != ammo_by_name.end();
					ev.node = c.get_int("node", 0);
					art.releases.push_back(ev);
				} else if (c.name == "PLANERELEASE") {
					// event type 5 (the carrier's launch); the frame conversion is the release events'.
					const std::string an = upper(c.get("anim"));
					int32_t idx = 0;
					while (idx < kNumUnitAnims && an != kUnitAnimNames[idx]) idx++;
					if (idx == kNumUnitAnims) continue;
					PlaneRelease pr;
					pr.anim = idx;
					pr.frame = std::max(1, int32_t(uint16_t(c.get_int("starttime", 0))) / 67);
					pr.node = c.get_int("node", 0);
					pr.angle = c.get_int("angle", 0);
					art.planes.push_back(pr);
				} else if (c.name == "INSTANTHIT") {
					auto it = ammo_by_name.find(upper(c.get("ammo")));
					art.instant_hit = it == ammo_by_name.end() ? 0 : it->second;
				} else if (c.name == "NORELEASE") {
					art.no_release = true;
				}
			}
			for (const XmlElement& c : u.children) {
				if (c.name != "ANIM") continue;
				const std::string file = c.get("file");
				if (file.empty()) continue;
				const std::string action = c.get("name");
				int32_t idx = 0;
				while (idx < kNumUnitAnims && upper(action) != kUnitAnimNames[idx]) idx++;
				if (idx == kNumUnitAnims) warnings_.push_back(u.get("name") + ": unknown animation action " + action);
				auto it = entry_by_name.find(ron::to_lower(file));
				if (it == entry_by_name.end()) {
					warnings_.push_back(u.get("name") + ": unknown animation " + file);
					continue;
				}
				if (size_t(idx) >= art.ids.size()) art.ids.resize(size_t(idx) + 1, -1);
				art.ids[size_t(idx)] = it->second;
			}
			unit_by_name_.emplace(upper(u.get("name")), int32_t(units_.size()));
			units_.push_back(std::move(art));
		}
	}
}

bool AssetAnimSource::build_piece_exists(int32_t type, int32_t style, int32_t age) const {
	static const char* const kStyle[9] = {"DEFAULT", "NEUROPE", "ASIAN", "KOREAN", "AMERICAN", "ARAB", "IROQUOIS", "COLONIAL", "EINDIAN"};
	if (type < 0 || size_t(type) >= graph_.size() || style < 0 || style > 8 || graph_[size_t(type)].empty()) return false;
	return build_pieces_.count(graph_[size_t(type)] + "-" + kStyle[style] + "-AGE" + std::to_string(age)) != 0;
}

std::string AssetAnimSource::piece_name(int32_t piece) const {
	using namespace gpiece;
	if (piece >= kFirstBird && piece < kFirstBird + kNumBird) {
		const int32_t t = (piece - kFirstBird) / 3 + 0x192;
		const std::string& g = size_t(t) < graph_.size() ? graph_[size_t(t)] : std::string();
		return g + "-TYPE" + std::to_string((piece - kFirstBird) % 3);
	}
	if (piece < kFirstUnit || piece >= kTotalUnit) return std::string();
	if (piece >= kTotalUnit - 6) return kMerchantSpecial[piece - (kTotalUnit - 6)];
	const int32_t t = piece % kPerStyle + 50;                 // GraphicPieces::get_type @902540
	const int32_t style = (piece % kPerAge) / kPerStyle;       // cont_from_gpiece @907990 (> 5: invalid)
	const int32_t bucket = (piece % kPerGender) / kPerAge;     // AGE0 / AGE3 / AGE5
	const bool gender = (piece % kPerCrew) >= kPerGender;
	const int32_t crew = piece / kPerCrew;
	if (style > 5 || size_t(t) >= graph_.size()) return std::string();
	std::string n = graph_[size_t(t)] + "-" + unit_style_name(style) + "-AGE" + std::to_string(bucket == 2 ? 5 : bucket * 3);
	if (gender) n += gender_packed_[size_t(t)] ? "-PACKED" : "-FEMALE";
	if (crew > 0) n += "-CREW" + std::to_string(crew);
	return n;
}

const AssetAnimSource::UnitArt* AssetAnimSource::art(int32_t piece) const {
	auto it = piece_unit_.find(piece);
	if (it == piece_unit_.end()) {
		int32_t k = -1;
		const std::string n = piece_name(piece);
		if (!n.empty()) {
			auto u = unit_by_name_.find(n);  // String::operator== is case-insensitive; names are uppercased
			if (u != unit_by_name_.end()) k = u->second;
		}
		it = piece_unit_.emplace(piece, k).first;
	}
	return it->second >= 0 ? &units_[size_t(it->second)] : nullptr;
}

bool AssetAnimSource::piece_exists(int32_t piece) const {
	const UnitArt* a = art(piece);
	return a && a->model_ok;
}

// TODO(anim-spec open question 1): taken as "every unit piece whose model loads has an AnimationPacket"
// (RData::load builds packets[0x13] for the skinned unit models; not traced).
bool AssetAnimSource::has_packet(int32_t piece) const { return piece_exists(piece); }

void AssetAnimSource::force_load(const Entry& e) const {
	if (e.loaded) return;
	e.loaded = true;
	e.obj = false;
	e.times = 200;  // no AnimObj: times 200
	const std::string path = e.file.empty() ? std::string() : files_.resolve(e.file);
	if (!path.empty()) {
		try {
			const Bytes b = read_file(path);
			// The ChunkRead walk: id 0 opens (descends), the first id 8 is the root node, id 10 is skipped; any
			// other id makes the loop spin until "Invalid Animation File".
			const Chunk* root = nullptr;
			Chunk found;
			bool bad = false;
			std::function<void(size_t, size_t)> walk = [&](size_t s, size_t end) {
				for (const Chunk& c : read_chunks(b, s, end)) {
					if (root || bad) return;
					if (c.id == 0) walk(c.start, c.end);
					else if (c.id == 8) {
						found = c;
						root = &found;
					} else if (c.id != 10) {
						bad = true;
					}
				}
			};
			walk(0, b.size());
			if (root) {
				// AnimObj::load_hier: the node's first child chunk is its key track: u32 n (<= 100000), then n keys
				// of 36 bytes; time += (int)(dt * 1000.0f) & 0xffff, stored as u16. Key compression keeps the last
				// two keys, so only those times matter.
				const std::vector<Chunk> kids = read_chunks(b, root->start, root->end);
				std::vector<uint16_t> kt;
				if (!kids.empty()) {
					const uint32_t n = read_u32(b, kids[0].start);
					if (n <= 100000) {
						int32_t t = 0;
						for (uint32_t k = 0; k < n; k++) {
							const float dt = read_f32(b, kids[0].start + 4 + size_t(k) * 36);
							t += trunc_f(dt * 1000.0f) & 0xffff;
							kt.push_back(uint16_t(t));
						}
					}
				}
				if (!kt.empty()) {
					e.obj = true;
					uint16_t last = kt.back();
					// Non-looping entries end one key early (the last key time is replaced by the one before it).
					// (One key: the original reads before the array; taken as the last key here.)
					if (!e.looping && last != 0 && kt.size() >= 2) last = kt[kt.size() - 2];
					e.times = last;
				}
				// (No keys, malformed: the original keeps the AnimObj and reads before the array; here it counts
				// as missing so the 3-frame fallback applies.)
			}
		} catch (const std::exception&) {
			// unreadable: " animation was not found" in the original; stays without AnimObj
		}
	}
	// frames = round-half-up(times * 3 / 200) in single precision: 15 sim frames per second, times in ms.
	const float f = float(double(uint32_t(e.times * 3u))) / 200.0f;
	int32_t fr = trunc_f(f);
	if (f - float(double(uint32_t(fr))) >= 0.5f) fr++;
	e.frames = fr;
}

const AssetAnimSource::Entry* AssetAnimSource::entry(int32_t piece, int32_t anim) const {
	const UnitArt* a = art(piece);
	if (!a || !a->model_ok || anim < 0 || size_t(anim) >= a->ids.size()) return nullptr;
	const int32_t id = a->ids[size_t(anim)];
	if (id < 0 || size_t(id) >= entries_.size()) return nullptr;
	const Entry& e = entries_[size_t(id)];
	force_load(e);
	return &e;
}

bool AssetAnimSource::has_anim(int32_t piece, int32_t anim) const {
	const Entry* e = entry(piece, anim);
	return e && e->obj;
}

int32_t AssetAnimSource::frames(int32_t piece, int32_t anim) const {
	const Entry* e = entry(piece, anim);
	return e ? e->frames : 3;
}

uint32_t AssetAnimSource::time_ms(int32_t piece, int32_t anim) const {
	const Entry* e = entry(piece, anim);
	return e ? e->times : 200u;
}

bool AssetAnimSource::loops(int32_t piece, int32_t anim) const {
	const UnitArt* a = art(piece);
	if (!a || !a->model_ok || anim < 0 || size_t(anim) >= a->ids.size()) return false;
	const int32_t id = a->ids[size_t(anim)];
	return id >= 0 && size_t(id) < entries_.size() && entries_[size_t(id)].looping;
}

bool AssetAnimSource::track(int32_t piece, float* scale, float* offset_x, float* offset_y) const {
	const UnitArt* a = art(piece);
	if (!a || !a->model_ok) return false;
	*scale = a->scale;
	// init_unit_data reads the offsets only for crew pieces (piece / kPerCrew > 0, below the bird range) of unit
	// types; every other piece keeps 0.
	const bool crew = piece < gpiece::kFirstBird && piece / gpiece::kPerCrew > 0;
	*offset_x = crew ? a->track_x : 0.0f;
	*offset_y = crew ? a->track_y : 0.0f;
	return true;
}

const std::vector<ReleaseEvent>* AssetAnimSource::release_events(int32_t piece) const {
	const UnitArt* a = art(piece);
	return a && a->model_ok && !a->releases.empty() ? &a->releases : nullptr;
}

const std::vector<PlaneRelease>* AssetAnimSource::plane_releases(int32_t piece) const {
	const UnitArt* a = art(piece);
	return a && a->model_ok && !a->planes.empty() ? &a->planes : nullptr;
}

int32_t AssetAnimSource::instant_hit(int32_t piece) const {
	const UnitArt* a = art(piece);
	return a && a->model_ok ? a->instant_hit : -1;
}

bool AssetAnimSource::no_release(int32_t piece) const {
	const UnitArt* a = art(piece);
	return a && a->model_ok && a->no_release;
}

bool AssetAnimSource::has_skeleton(int32_t piece) const {
	const UnitArt* a = art(piece);
	return a && a->model_ok && a->skeleton;
}

bool AssetAnimSource::has_scorch(int32_t piece) const {
	const UnitArt* a = art(piece);
	return a && a->model_ok && a->scorch;
}

uint32_t AssetAnimSource::ammo_flags(int32_t ammo) const {
	return ammo >= 0 && size_t(ammo) < ammo_.size() ? ammo_[size_t(ammo)].flags : 0;
}

std::string AssetAnimSource::ammo_name(int32_t ammo) const {
	return ammo >= 0 && size_t(ammo) < ammo_.size() ? ammo_[size_t(ammo)].name : std::string();
}

std::string AssetAnimSource::anim_entry_name(int32_t piece, int32_t anim) const {
	const UnitArt* a = art(piece);
	if (!a || anim < 0 || size_t(anim) >= a->ids.size() || a->ids[size_t(anim)] < 0) return std::string();
	const Entry& e = entries_[size_t(a->ids[size_t(anim)])];
	return e.name + " (" + e.file + ")";
}

std::unique_ptr<AssetAnimSource> load_anim_source(const GameFiles& files, const Rules& rules) {
	return std::make_unique<AssetAnimSource>(files, rules);
}

}  // namespace ron::sim
