// Save / load of the simulation state (sim/save.hpp, docs/save-format.md).
//
// One walk function per state struct (the generated list in save_walk.inc plus the hand-written ones below), driven by a
// Writer or a Reader (sim/save_archive.hpp): the same code saves and loads, so a field cannot be written without being
// read. State that is rebuilt by Game::load from the install's files is not saved (see docs/save-format.md, "Derived").
#include "sim/save.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_map>

#include <zlib.h>

#include "assets/chunk.hpp"
#include "assets/scenario.hpp"
#include "sim/newgame.hpp"
#include "sim/pathfinder_search.hpp"
#include "sim/save_archive.hpp"
#include "sim/script_host.hpp"

namespace ron::sim {
namespace save {

// The member lists of the plain structs (generated, see the header of the file).
#include "sim/save_walk.inc"

// ---------------------------------------------------------------- hand-written walks

// PathFinder: the generated walk covers the tunables; the search trees follow (SavedSearch below).
using SearchPtr = std::unique_ptr<SavedSearch, SavedSearchDel>;

// Drift guards for the aggregates that are walked by hand (the generated walks have their own at the end of
// save_walk.inc). A member added to one of these changes its size and stops the build here: save it in the walk named in
// the message, or say in docs/save-format.md why it is derived.
#if defined(__LP64__) && defined(_LIBCPP_VERSION)
static_assert(sizeof(PathNode) == 40 && sizeof(SavedSearch) == 176, "the pathfinder search changed: update walk_search()");
static_assert(sizeof(ObjectList) == 64, "ObjectList changed: update walk_objects()");
static_assert(sizeof(Groups) == 1282084, "Groups changed: update walk_groups()");
static_assert(sizeof(Regions) == 10248, "Regions changed: update the REGN section in walk_body()");
// 1647392 + 16 (+ 32: Game::herds with its padding): Game::original (the original-save shadow, sim/original_save.hpp) is derived state the native save never writes.
// + 9192: Game::ctl (sim/control.hpp: TurnControl speed / pause, the control groups, cheat counters, chat lines) is console state no simulation code reads;
// the original saves its hotkey groups, the native save does not (docs/sim-implementation.md, "Orders and commands, second pass").
static_assert(sizeof(Game) == 1657592, "Game gained or lost a member: save it in walk_core() / walk_body() or list it as derived in docs/save-format.md");
// 1608 + 8: ScriptHost::ctw (the Conquer the World campaign the scripts run for, sim/ctw.hpp) is a run-time link the native battle save never writes.
static_assert(sizeof(ScriptHost) == 1616, "ScriptHost changed: update walk_scripts()");
#endif

// A time-sliced / cached search. PathNode pointers (parent, the open and closed trees) become indices into the arena.
template <class Ar>
void walk_search(Ar& ar, SavedSearch& s) {
	std::unordered_map<const PathNode*, int32_t> index;
	if constexpr (Ar::kReading) {
		const size_t n = ar.count(1);
		s.nodes.clear();
		s.nodes.resize(n);
	} else {
		ar.count(s.nodes.size());
		for (size_t i = 0; i < s.nodes.size(); i++) index[&s.nodes[i]] = int32_t(i);
	}
	auto node_ref = [&](PathNode*& p) {
		int32_t i = -1;
		if constexpr (!Ar::kReading) {
			if (p) {
				const auto it = index.find(p);
				if (it == index.end()) ar.error("a pathfinder node points outside its arena");
				i = it->second;
			}
		}
		io(ar, i);
		if constexpr (Ar::kReading) {
			if (i < -1 || i >= int32_t(s.nodes.size())) ar.error("pathfinder node index out of range");
			p = i < 0 ? nullptr : &s.nodes[size_t(i)];
		}
	};
	for (PathNode& n : s.nodes) {
		fields(ar, n.x, n.y, n.length, n.estimate, n.value, n.timeout, n.metric, n.transport, n.building);
		node_ref(n.parent);
	}
	// open: Tree keyed by (value, -sequence)
	if constexpr (Ar::kReading) {
		const size_t n = ar.count(1);
		s.open.clear();
		for (size_t i = 0; i < n; i++) {
			SavedSearch::OpenKey k;
			PathNode* p = nullptr;
			fields(ar, k.first, k.second);
			node_ref(p);
			s.open.emplace(k, p);
		}
	} else {
		ar.count(s.open.size());
		for (auto& kv : s.open) {
			SavedSearch::OpenKey k = kv.first;
			PathNode* p = kv.second;
			fields(ar, k.first, k.second);
			node_ref(p);
		}
	}
	io(ar, s.refs);
	if constexpr (Ar::kReading) {
		const size_t n = ar.count(1);
		s.closed.clear();
		for (size_t i = 0; i < n; i++) {
			uint32_t k = 0;
			PathNode* p = nullptr;
			io(ar, k);
			node_ref(p);
			s.closed.emplace(k, p);
		}
	} else {
		ar.count(s.closed.size());
		for (auto& kv : s.closed) {
			uint32_t k = kv.first;
			PathNode* p = kv.second;
			io(ar, k);
			node_ref(p);
		}
	}
	fields(ar, s.valid, s.blocks, s.seq);
}

template <class Ar>
void walk_search_ptr(Ar& ar, SearchPtr& p) {
	bool has = static_cast<bool>(p);
	io(ar, has);
	if (!has) {
		if constexpr (Ar::kReading) p.reset();
		return;
	}
	if constexpr (Ar::kReading) p.reset(new SavedSearch());
	walk_search(ar, *p);
}

template <class Ar>
void walk_pathfinder(Ar& ar, PathFinder& pf) {
	walk(ar, pf);
	walk_search_ptr(ar, pf.lists);
	// The blocklist always points into the current lists (pathfinder.cpp own_lists).
	if constexpr (Ar::kReading) pf.blocklist = pf.lists ? &pf.lists->blocks : nullptr;
}

// Object::ptype points into the rules tables: stored as the table index (bit 30: the loaded base tables, which a
// closed object can still point at after the scenario rewrote the rules).
constexpr int32_t kPtypeBase = 0x40000000;

int32_t encode_ptype(const Game& g, const Object& o) {
	if (!o.ptype) return -1;
	auto within = [&](const Rules* r, int32_t* idx) {
		if (!r || r->types.empty()) return false;
		const Type* b = r->types.data();
		if (o.ptype < b || o.ptype >= b + r->types.size()) return false;
		*idx = int32_t(o.ptype - b);
		return true;
	};
	int32_t idx = 0;
	if (within(g.rules, &idx)) return idx;
	if (within(g.base_rules, &idx)) return idx | kPtypeBase;
	throw std::runtime_error("save: section 'OBJS': an object's type pointer is outside both rules tables");
}

template <class Ar>
void walk_slot_extra(Ar& ar, Unit& u) {
	walk_search_ptr(ar, u.openlist);
}
template <class Ar>
void walk_slot_extra(Ar&, Build&) {}

template <class Ar, class T>
void walk_slots(Ar& ar, Game& g, std::vector<std::unique_ptr<T>>& v) {
	if constexpr (Ar::kReading) {
		const size_t n = ar.count(1);
		v.clear();
		v.resize(n);
	} else {
		ar.count(v.size());
	}
	size_t slot_index = 0;
	for (auto& slot : v) {
		bool present = static_cast<bool>(slot);
		if constexpr (!Ar::kReading) {
			if (ar.trace) ar.mark(std::string(std::is_same_v<T, Unit> ? "unit slot " : "build slot ") + std::to_string(slot_index));
		}
		slot_index++;
		io(ar, present);
		if (!present) continue;
		int32_t pt = -1;
		if constexpr (Ar::kReading) {
			slot = std::make_unique<T>();
		} else {
			pt = encode_ptype(g, *slot);
		}
		io(ar, pt);
		walk(ar, *slot);
		walk_slot_extra(ar, *slot);
		if constexpr (Ar::kReading) {
			if (pt >= 0) {
				const Rules* r = (pt & kPtypeBase) ? g.base_rules : g.rules;
				const int32_t idx = pt & ~kPtypeBase;
				if (!r || idx >= int32_t(r->types.size())) ar.error("object type index out of range");
				slot->ptype = &r->types[size_t(idx)];
			} else if (pt != -1) {
				ar.error("bad object type pointer code");
			}
		}
	}
}

template <class Ar>
void walk_objects(Ar& ar, Game& g) {
	int32_t who = 0;
	for (ObjectList& L : g.objects) {
		ar.mark("leader " + std::to_string(who++) + " object list");
		fields(ar, L.unit_mark, L.build_mark, L.obj_ctr);
		walk_slots(ar, g, L.units);
		walk_slots(ar, g, L.builds);
	}
}

template <class Ar>
void walk_world(Ar& ar, World& w) {
	std::array<int32_t, 12> dims{w.xs,       w.ys,       w.size,     w.tile_xs, w.tile_ys, w.tile_size,
	                             w.fog_xs,   w.fog_ys,   w.fog_size, w.reg_xs,  w.reg_ys,  w.reg_size};
	std::array<int32_t, 12> saved = dims;
	io(ar, saved);
	if constexpr (Ar::kReading) {
		if (saved != dims) {
			ar.error("the world is " + std::to_string(saved[0]) + "x" + std::to_string(saved[1]) +
			         " wcoords in the file but the scenario loads as " + std::to_string(dims[0]) + "x" +
			         std::to_string(dims[1]));
		}
	}
	fields(ar, w.sea_map, w.wdata, w.tdata, w.seen, w.seen2, w.seen3, w.wcoord_seen, w.danger, w.coll_blocks,
	       w.free_coll_blocks, w.empty_colls);
	if constexpr (Ar::kReading) {
		if (w.wdata.size() != size_t(w.size) || w.tdata.size() != size_t(w.tile_size) || w.seen.size() != size_t(w.fog_size) ||
		    w.seen2.size() != size_t(w.fog_size) || w.seen3.size() != size_t(w.fog_size) ||
		    w.wcoord_seen.size() != size_t(w.size) || w.danger.size() != size_t(8) * size_t(w.reg_size))
			ar.error("a world grid has the wrong size");
		for (const WData& d : w.wdata) {
			if (d.block >= int32_t(w.coll_blocks.size())) ar.error("a collision block index is out of range");
		}
		w.fog_changes++;  // renderers watching the fog grids refresh
	}
}

template <class Ar>
void walk_groups(Ar& ar, Game& g) {
	fields(ar, g.groups.g, g.groups.last_group, g.groups.proc_group, g.forms, g.grouppath, g.group_cols);
}

template <class Ar>
void walk_core(Ar& ar, Game& g) {
	uint32_t rng = g.rng.seed();
	io(ar, rng);
	if constexpr (Ar::kReading) g.rng.reseed(rng);
	fields(ar, g.frame, g.tick, g.seed, g.total_units, g.everyone_mask, g.daemon, g.queue_fail, g.check_borders_budget,
	       g.armageddon, g.game_over, g.victory_checked, g.musical_chairs, g.editor, g.scripted_rules, g.ctw,
	       g.multiplayer, g.field_battle, g.ai_speed, g.ai_off, g.scenario_mode, g.building_unit_bonus,
	       g.building_resource_bonus, g.buildings_gather, g.market, g.good_mark, g.world_total_land);
}

// ---- rules tables rewritten by scenario scripts (Game::own_rules): stored as a difference from the loaded tables.

template <class T>
std::vector<uint8_t> blob(const T& v) {
	Writer w;
	io(w, const_cast<T&>(v));
	return std::move(w.buf);
}

template <class Ar, class T>
void diff_vec(Ar& ar, std::vector<T>& cur, const std::vector<T>& base) {
	if constexpr (Ar::kReading) {
		uint32_t size = 0;
		io(ar, size);
		if (size != cur.size()) cur.resize(size);
		const size_t n = ar.count(1);
		for (size_t i = 0; i < n; i++) {
			uint32_t idx = 0;
			io(ar, idx);
			if (idx >= cur.size()) ar.error("rules element index out of range");
			io(ar, cur[idx]);
		}
	} else {
		uint32_t size = uint32_t(cur.size());
		io(ar, size);
		std::vector<uint32_t> changed;
		for (size_t i = 0; i < cur.size(); i++) {
			if (i >= base.size() || blob(cur[i]) != blob(base[i])) changed.push_back(uint32_t(i));
		}
		ar.count(changed.size());
		for (uint32_t idx : changed) {
			uint32_t k = idx;
			io(ar, k);
			io(ar, cur[idx]);
		}
	}
}

template <class Ar, class T>
void diff_whole(Ar& ar, T& cur, const T& base) {
	bool changed = false;
	if constexpr (!Ar::kReading) changed = blob(cur) != blob(base);
	io(ar, changed);
	if (changed) io(ar, cur);
}

template <class Ar>
void walk_rules(Ar& ar, Game& g) {
	bool own = static_cast<bool>(g.own_rules);
	io(ar, own);
	if (!own) return;
	if (!g.base_rules) ar.error("rewritten rules without the loaded tables to compare with");
	if constexpr (Ar::kReading) {
		g.own_rules = std::make_unique<Rules>(*g.base_rules);
		g.rules = g.own_rules.get();
	}
	Rules& cur = *g.own_rules;
	const Rules& base = *g.base_rules;
	diff_vec(ar, cur.types, base.types);
	diff_whole(ar, cur.constants, base.constants);
	diff_whole(ar, cur.balance, base.balance);
	diff_vec(ar, cur.tribes, base.tribes);
	diff_vec(ar, cur.categories, base.categories);
	diff_vec(ar, cur.lands, base.lands);
}

uint64_t rules_hash(const Rules& r) {
	Writer w;
	for (const Type& t : r.types) io(w, const_cast<Type&>(t));
	io(w, const_cast<Constants&>(r.constants));
	io(w, const_cast<std::vector<int16_t>&>(r.balance));
	for (const Tribe& t : r.tribes) io(w, const_cast<Tribe&>(t));
	uint64_t h = 0xcbf29ce484222325ull;
	for (uint8_t b : w.buf) {
		h ^= b;
		h *= 0x100000001b3ull;
	}
	return h;
}

// ---- the BigHugeScript run time and the ScriptHost state

struct ValueTable {
	std::unordered_map<const bhs::Value*, uint32_t> ids;  // writer
	std::vector<bhs::VP> vec;                             // reader
};

// A script value graph with shared identity: the first sight of a Value writes it in full, later sights write its id.
template <class Ar>
void walk_value(Ar& ar, bhs::VP& p, ValueTable& t) {
	uint8_t kind = 0;
	if constexpr (!Ar::kReading) {
		if (!p) {
			io(ar, kind);
			return;
		}
		const auto it = t.ids.find(p.get());
		if (it != t.ids.end()) {
			kind = 2;
			uint32_t id = it->second;
			io(ar, kind);
			io(ar, id);
			return;
		}
		kind = 1;
		io(ar, kind);
		const uint32_t new_id = uint32_t(t.ids.size());
		t.ids[p.get()] = new_id;
		bhs::Value& v = *p;
		fields(ar, v.cls, v.scope, v.data_type, v.i, v.f, v.s);
		ar.count(v.elems.size());
		for (bhs::VP& e : v.elems) walk_value(ar, e, t);
		walk_value(ar, v.blank, t);
	} else {
		io(ar, kind);
		if (kind == 0) {
			p.reset();
		} else if (kind == 2) {
			uint32_t id = 0;
			io(ar, id);
			if (id >= t.vec.size()) ar.error("script value reference out of range");
			p = t.vec[id];
		} else if (kind == 1) {
			p = std::make_shared<bhs::Value>();
			t.vec.push_back(p);
			bhs::Value& v = *p;
			fields(ar, v.cls, v.scope, v.data_type, v.i, v.f, v.s);
			const size_t n = ar.count(1);
			v.elems.assign(n, nullptr);
			for (bhs::VP& e : v.elems) walk_value(ar, e, t);
			walk_value(ar, v.blank, t);
		} else {
			ar.error("bad script value tag");
		}
	}
}

template <class Ar>
void walk_vm(Ar& ar, bhs::Runtime& rt) {
	ValueTable table;
	uint32_t nfiles = uint32_t(rt.files.size());
	io(ar, nfiles);
	if (nfiles != rt.files.size())
		ar.error("the file has " + std::to_string(nfiles) + " compiled script files, this game compiled " +
		         std::to_string(rt.files.size()));
	for (auto& fp : rt.files) {
		bhs::RtFile* f = fp.get();
		if (!f) ar.error("a compiled script file slot is empty");
		std::string src = f->source_file;
		io(ar, src);
		if (src != f->source_file) ar.error("script file '" + src + "' in the file, '" + f->source_file + "' compiled here");
		uint32_t nscripts = uint32_t(f->scripts.size());
		io(ar, nscripts);
		if (nscripts != f->scripts.size()) ar.error("script count differs in '" + src + "'");
		const size_t pool = f->const_pool.size();
		uint32_t npool = uint32_t(pool);
		io(ar, npool);
		if (npool != pool) ar.error("constant pool size differs in '" + src + "'");
		for (bhs::VP& v : f->const_pool) walk_value(ar, v, table);
		for (bhs::RtScript& s : f->scripts) {
			bhs::U16 name = s.name;
			io(ar, name);
			if (name != s.name) ar.error("script names differ in '" + src + "'");
			uint32_t nbits = uint32_t(s.trigger_bit_count);
			io(ar, nbits);
			if (int32_t(nbits) != s.trigger_bit_count) ar.error("trigger count differs in script of '" + src + "'");
			io(ar, s.trigger_bits);
			// The static variables are created by the script's first run, so their number is the file's, not this load's.
			uint32_t ns = uint32_t(s.static_vars.size());
			io(ar, ns);
			if constexpr (Ar::kReading) {
				if (size_t(ns) > ar.remaining()) ar.error("static variable count exceeds the section");
				s.static_vars.assign(ns, nullptr);
			}
			for (bhs::VP& v : s.static_vars) walk_value(ar, v, table);
		}
	}
}

// The player-profile XML documents scripts build in memory (open_file ...): nodes become ids so the shared_ptr web
// (the current / parent / root / element pointers into the documents) is restored with the same sharing.
template <class Ar>
void walk_xml(Ar& ar, ScriptHost& h) {
	using Node = ScriptHost::XmlNode;
	using NP = std::shared_ptr<Node>;
	std::vector<NP> nodes;
	std::unordered_map<const Node*, int32_t> ids;
	auto id_of = [&](const NP& p) -> int32_t { return p ? ids.at(p.get()) : -1; };
	if constexpr (!Ar::kReading) {
		std::vector<NP> work;
		auto add = [&](const NP& p) {
			if (!p || ids.count(p.get())) return;
			ids[p.get()] = int32_t(nodes.size());
			nodes.push_back(p);
			work.push_back(p);
		};
		for (auto& kv : h.xml_files) add(kv.second);
		add(h.xml_root), add(h.xml_curr), add(h.xml_parent);
		for (auto& e : h.xml_elements) add(e);
		while (!work.empty()) {
			NP n = work.back();
			work.pop_back();
			for (auto& c : n->children) add(c);
		}
		ar.count(nodes.size());
		for (const NP& n : nodes) {
			std::string name = n->name;
			io(ar, name);
			io(ar, n->attribs);
			std::vector<int32_t> kids;
			for (auto& c : n->children) kids.push_back(id_of(c));
			io(ar, kids);
		}
	} else {
		const size_t n = ar.count(1);
		nodes.resize(n);
		for (NP& p : nodes) p = std::make_shared<Node>();
		for (NP& p : nodes) {
			io(ar, p->name);
			io(ar, p->attribs);
			std::vector<int32_t> kids;
			io(ar, kids);
			for (int32_t k : kids) {
				if (k < 0 || k >= int32_t(n)) ar.error("xml node index out of range");
				p->children.push_back(nodes[size_t(k)]);
			}
		}
	}
	auto ref = [&](NP& p) {
		int32_t i = Ar::kReading ? -1 : id_of(p);
		io(ar, i);
		if constexpr (Ar::kReading) {
			if (i < -1 || i >= int32_t(nodes.size())) ar.error("xml node index out of range");
			p = i < 0 ? nullptr : nodes[size_t(i)];
		}
	};
	if constexpr (Ar::kReading) {
		const size_t nf = ar.count(1);
		h.xml_files.clear();
		for (size_t i = 0; i < nf; i++) {
			std::string key;
			NP p;
			io(ar, key);
			ref(p);
			h.xml_files[key] = p;
		}
	} else {
		ar.count(h.xml_files.size());
		for (auto& kv : h.xml_files) {
			std::string key = kv.first;
			NP p = kv.second;
			io(ar, key);
			ref(p);
		}
	}
	ref(h.xml_root);
	ref(h.xml_curr);
	ref(h.xml_parent);
	if constexpr (Ar::kReading) {
		const size_t ne = ar.count(1);
		h.xml_elements.assign(ne, nullptr);
		for (NP& p : h.xml_elements) ref(p);
	} else {
		ar.count(h.xml_elements.size());
		for (NP& p : h.xml_elements) ref(p);
	}
}

template <class Ar>
void walk_scripts(Ar& ar, ScriptHost& h) {
	fields(ar, h.scenario_script, h.general_powers_script, h.timers.list, h.find_counters, h.pres, h.scenario_dir,
	       h.sound, h.ctw_ints, h.ctw_strs, h.xml_curr_entry, h.xml_curr_index);
	// The seat-local input the renderer reports: the selection and the armed command feed some scripts (the tutorial
	// waits for them), the rest (screen, camera, keys, callbacks) is the renderer's and is not saved.
	ScriptInput& in = h.input;
	fields(ar, in.select_who, in.selected, in.option, in.option_object, in.mode_data, in.selecting_spot, in.build_options,
	       in.cursor_tx, in.cursor_ty, in.cursor_over_iface, in.user_warnings);
	walk_xml(ar, h);
	walk_vm(ar, h.rt);
}

// ---------------------------------------------------------------- the file

constexpr char kMagic[4] = {'R', 'O', 'N', 'S'};

template <class Ar>
void walk_meta(Ar& ar, SaveInfo& m) {
	fields(ar, m.scenario, m.seed, m.frame, m.tick, m.has_scripts, m.has_own_rules, m.rules_hash);
}

// Sections after META / INFO, shared by the writer and the reader (same order).
template <class Ar>
void walk_body(Ar& top, Game& g, bool has_scripts) {
	top.section("RULE", [&](auto& ar) { walk_rules(ar, g); });
	top.section("CORE", [&](auto& ar) { walk_core(ar, g); });
	top.section("WRLD", [&](auto& ar) { walk_world(ar, g.world); });
	top.section("REGN", [&](auto& ar) { fields(ar, g.regions.r, g.regions.sea, g.regions.land); });
	top.section("LEAD", [&](auto& ar) { fields(ar, g.leaders); });
	top.section("OBJS", [&](auto& ar) { walk_objects(ar, g); });
	top.section("GOOD", [&](auto& ar) { fields(ar, g.goods, g.items, g.herds); });
	top.section("HGHT", [&](auto& ar) { fields(ar, g.heights.h); });  // the terrain height grid: buildings terraform it (TerrainOut::terraform_for_building)
	top.section("CITY", [&](auto& ar) {
		fields(ar, g.cities, g.forts, g.wonder_records, g.unbuilt_cities, g.unbuilt_forts, g.unbuilt_wonders, g.wonders,
		       g.terra_cotta_total, g.world_pop, g.world_cities, g.starting, g.supplies, g.caravans, g.farms,
		       g.hold_doobers);
	});
	top.section("GRUP", [&](auto& ar) { walk_groups(ar, g); });
	top.section("PATH", [&](auto& ar) { walk_pathfinder(ar, g.pathfinder); });
	top.section("COMB", [&](auto& ar) { fields(ar, g.ammo, g.ammo_counter, g.deaths, g.range_swap_type, g.range_swap_value); });
	top.section("AIST", [&](auto& ar) { fields(ar, g.armies, g.armies_find_dist, g.find_dist, g.find_who); });
	top.section("SCEN", [&](auto& ar) {
		fields(ar, g.scen, g.scen_ally_mask, g.scen_diplomacy_setting, g.reveal_points, g.attrition_free_points);
	});
	if (has_scripts) top.section("SCRP", [&](auto& ar) { walk_scripts(ar, *g.scripts); });
	top.section("END!", [&](auto&) {});
}

}  // namespace save

namespace {

using save::Reader;
using save::Writer;

std::vector<uint8_t> read_all(const std::string& path) {
	gzFile f = gzopen(path.c_str(), "rb");  // plain files are read as they are
	if (!f) throw std::runtime_error("save: cannot open " + path);
	std::vector<uint8_t> out;
	uint8_t chunk[1 << 16];
	for (;;) {
		const int n = gzread(f, chunk, sizeof chunk);
		if (n < 0) {
			int err = 0;
			const char* msg = gzerror(f, &err);
			gzclose(f);
			throw std::runtime_error("save: cannot read " + path + ": " + (msg ? msg : "error"));
		}
		if (n == 0) break;
		out.insert(out.end(), chunk, chunk + n);
	}
	gzclose(f);
	return out;
}

void write_all(const std::string& path, const std::vector<uint8_t>& bytes) {
	const std::string tmp = path + ".tmp";
	const bool gz = path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
	if (gz) {
		gzFile f = gzopen(tmp.c_str(), "wb6");
		if (!f) throw std::runtime_error("save: cannot write " + tmp);
		size_t off = 0;
		while (off < bytes.size()) {
			const unsigned n = unsigned(std::min<size_t>(bytes.size() - off, 1u << 30));
			if (gzwrite(f, bytes.data() + off, n) != int(n)) {
				gzclose(f);
				std::remove(tmp.c_str());
				throw std::runtime_error("save: write failed on " + tmp);
			}
			off += n;
		}
		if (gzclose(f) != Z_OK) {
			std::remove(tmp.c_str());
			throw std::runtime_error("save: write failed on " + tmp);
		}
	} else {
		FILE* f = std::fopen(tmp.c_str(), "wb");
		if (!f) throw std::runtime_error("save: cannot write " + tmp);
		const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
		if (std::fclose(f) != 0 || !ok) {
			std::remove(tmp.c_str());
			throw std::runtime_error("save: write failed on " + tmp);
		}
	}
	std::error_code ec;
	std::filesystem::rename(tmp, path, ec);
	if (ec) {
		std::remove(tmp.c_str());
		throw std::runtime_error("save: cannot replace " + path + ": " + ec.message());
	}
}

// Header ("RONS", version, flags) and the META section; leaves `top` at INFO.
SaveInfo read_header(Reader& top) {
	char magic[4];
	if (top.remaining() < 12) throw std::runtime_error("save: not a save file (too short)");
	top.raw(magic, 4);
	if (std::memcmp(magic, save::kMagic, 4) != 0) throw std::runtime_error("save: not a save file (bad magic)");
	SaveInfo info;
	uint32_t flags = 0;
	top.raw(&info.version, 4);
	top.raw(&flags, 4);
	if (info.version != kSaveVersion)
		throw std::runtime_error("save: format version " + std::to_string(info.version) + " (this build reads version " +
		                         std::to_string(kSaveVersion) + ")");
	top.section("META", [&](Reader& ar) { save::walk_meta(ar, info); });
	return info;
}

}  // namespace

std::string scenario_rel_path(const GameFiles& files, const std::string& file) {
	namespace fs = std::filesystem;
	if (is_random_scenario(file)) return file;  // a random game: the setup text (sim/newgame.hpp)
	if (file == "ctw:battle") {  // a Conquer the World battle on a generated map: the setup text, conquest map input included
		const std::string text = battle_setup_text();
		if (!text.empty()) return text;
	}
	std::error_code ec;
	std::string root = fs::weakly_canonical(files.root(), ec).string();
	const std::string abs = fs::weakly_canonical(file, ec).string();
	if (ec) return file;
	if (!root.empty() && root.back() != '/') root += '/';
	if (abs.compare(0, root.size(), root) == 0) return abs.substr(root.size());
	return file;
}

namespace {
std::vector<uint8_t> save_bytes_impl(const Game& cg, const std::string& scenario_rel, uint32_t seed,
                                     std::vector<save::TraceEntry>* trace) {
	Game& g = const_cast<Game&>(cg);  // the Writer never modifies what it walks
	if (!g.rules) throw std::runtime_error("save: the game has no rules loaded");
	SaveInfo info;
	info.scenario = scenario_rel;
	info.seed = seed;
	info.frame = g.frame;
	info.tick = g.tick;
	info.has_scripts = static_cast<bool>(g.scripts);
	info.has_own_rules = static_cast<bool>(g.own_rules);
	info.rules_hash = save::rules_hash(g.base_rules ? *g.base_rules : *g.rules);
	Writer top;
	top.trace = trace;
	top.raw(save::kMagic, 4);
	const uint32_t version = kSaveVersion, flags = 0;
	top.raw(&version, 4);
	top.raw(&flags, 4);
	top.section("META", [&](Writer& ar) { save::walk_meta(ar, info); });
	top.section("INFO", [&](Writer& ar) { save::io(ar, g.info); });
	save::walk_body(top, g, info.has_scripts);
	return std::move(top.buf);
}
}  // namespace

std::vector<uint8_t> save_game_bytes(const Game& g, const std::string& scenario_rel, uint32_t seed) {
	return save_bytes_impl(g, scenario_rel, seed, nullptr);
}

std::string describe_game_difference(const Game& a, const Game& b, const std::string& scenario_rel, uint32_t seed) {
	std::vector<save::TraceEntry> trace;
	const std::vector<uint8_t> xa = save_bytes_impl(a, scenario_rel, seed, &trace);
	const std::vector<uint8_t> xb = save_bytes_impl(b, scenario_rel, seed, nullptr);
	const std::string d = describe_save_difference(xa, xb);
	if (d.empty()) return d;
	// "section 'OBJS' differs at payload byte N of M": name the field from the trace of the first save.
	const size_t q = d.find('\''), q2 = q == std::string::npos ? q : d.find('\'', q + 1);
	const size_t at = d.find("payload byte ");
	if (q2 == std::string::npos || at == std::string::npos) return d;
	const std::string tag = d.substr(q + 1, q2 - q - 1);
	const size_t pos = size_t(std::strtoull(d.c_str() + at + 13, nullptr, 10));
	std::vector<const save::TraceEntry*> near;
	for (const save::TraceEntry& e : trace) {
		if (e.section == tag && e.off <= pos) {
			near.push_back(&e);
			if (near.size() > 64) near.erase(near.begin());
		}
	}
	std::string where;
	for (size_t i = near.size(); i-- > 0 && where.size() < 160;) where += (where.empty() ? "" : " < ") + near[i]->name;
	return d + (where.empty() ? "" : " (in " + where + ")");
}

void save_game(const Game& g, const std::string& scenario_rel, uint32_t seed, const std::string& path) {
	write_all(path, save_game_bytes(g, scenario_rel, seed));
}

std::string describe_save_difference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
	if (a == b) return std::string();
	if (a.size() < 12 || b.size() < 12) return "one of the saves is too short";
	size_t pa = 12, pb = 12;
	while (pa + 16 <= a.size() && pb + 16 <= b.size()) {
		uint64_t la = 0, lb = 0;
		std::memcpy(&la, a.data() + pa + 4, 8);
		std::memcpy(&lb, b.data() + pb + 4, 8);
		const std::string ta(reinterpret_cast<const char*>(a.data() + pa), 4), tb(reinterpret_cast<const char*>(b.data() + pb), 4);
		if (ta != tb) return "section '" + ta + "' versus '" + tb + "'";
		if (pa + 16 + la > a.size() || pb + 16 + lb > b.size()) return "section '" + ta + "' is truncated";
		const uint8_t* xa = a.data() + pa + 16;
		const uint8_t* xb = b.data() + pb + 16;
		if (la != lb) return "section '" + ta + "' has " + std::to_string(la) + " bytes versus " + std::to_string(lb);
		for (uint64_t i = 0; i < la; i++) {
			if (xa[i] != xb[i]) return "section '" + ta + "' differs at payload byte " + std::to_string(i) + " of " + std::to_string(la);
		}
		pa += 16 + la;
		pb += 16 + lb;
	}
	return "the saves differ in length";
}

SaveInfo read_save_info(const std::vector<uint8_t>& bytes) {
	Reader top(bytes.data(), bytes.size(), "");
	return read_header(top);
}

SaveInfo read_save_info(const std::string& path) { return read_save_info(read_all(path)); }

std::unique_ptr<Game> load_game(const GameFiles& files, const Rules& rules, const AnimSource* anims, const std::string& path,
                                SaveInfo* info_out) {
	return load_game_bytes(files, rules, anims, read_all(path), info_out);
}

std::unique_ptr<Game> load_game_bytes(const GameFiles& files, const Rules& rules, const AnimSource* anims,
                                      const std::vector<uint8_t>& bytes, SaveInfo* info_out) {
	Reader top(bytes.data(), bytes.size(), "");
	SaveInfo info = read_header(top);
	GameInfo ginfo;
	top.section("INFO", [&](Reader& ar) { save::io(ar, ginfo); });
	if (info.rules_hash != save::rules_hash(rules))
		throw std::runtime_error("save: section 'META': the save was made with different rules tables than the ones loaded");

	// Game::load first: it rebuilds everything derived from the install's files (rules, art, terrain heights, regions,
	// mountains), then every piece of state is overwritten from the file. A random game (the scenario slot holds its
	// setup text) is regenerated from the setup instead: new_random_game gives the same world, objects and scripts.
	auto g = std::make_unique<Game>();
	if (is_random_scenario(info.scenario)) {
		RandomGameSetup setup;
		if (!parse_random_setup(info.scenario, setup)) throw std::runtime_error("save: section 'META': malformed random game setup");
		if (setup.seed != info.seed) throw std::runtime_error("save: section 'META': the random game's seed does not match its setup");
		RandomExtras extras;
		g = new_random_game(files, rules, anims, setup, &extras);
		if (!g) throw std::runtime_error("save: section 'META': cannot regenerate the random game: " + extras.error);
		if (static_cast<bool>(g->scripts) != info.has_scripts)
			throw std::runtime_error("save: section 'META': the regenerated random game differs in its scripts");
	} else {
		std::string scx = info.scenario;
		std::error_code ec;
		if (!std::filesystem::exists(scx, ec) || std::filesystem::path(scx).is_relative()) {
			const std::string r = files.resolve(scx);
			if (!r.empty()) scx = r;
		}
		if (!std::filesystem::exists(scx, ec)) throw std::runtime_error("save: section 'META': scenario not found: " + info.scenario);
		const Scenario sc = parse_scenario(read_file(scx));
		g->anims = anims;
		g->info = ginfo;  // the options the original run set before loading (reveal map, difficulty ...)
		g->load(files, sc, rules, info.seed);
		if (info.has_scripts) {
			ScriptLoadReport rep = script_host_load(*g, files, std::filesystem::absolute(scx).string(), sc);
			info.script_errors = std::move(rep.errors);
		}
	}
	save::walk_body(top, *g, info.has_scripts);
	if (top.remaining() != 0) throw std::runtime_error("save: " + std::to_string(top.remaining()) + " bytes after the last section");
	g->info = ginfo;
	if (info_out) *info_out = info;
	return g;
}

}  // namespace ron::sim
