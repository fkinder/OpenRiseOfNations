// Conquer the World: the strategic-layer script hooks (docs/script-spec.md 6.3, docs/ctw-spec.md 8). The campaign has
// its own script host (a Game that never simulates + a ScriptHost whose `ctw` points back at the campaign) in which
// the campaign's files run: initialization_script (init_script), links (links), strategy (strategy), post_ai
// (post_ai), post_turn (post_turn), the battle script (battle_override) and the diplomacy scripts (diplo_response,
// diplo_offer). The builtins they call are script_funcs_ctw.cpp.
#include <algorithm>
#include <filesystem>
#include <unordered_map>

#include "script/bhs_common.hpp"
#include "script/bhs_vm.hpp"
#include "sim/ctw.hpp"
#include "sim/game.hpp"
#include "sim/save_archive.hpp"
#include "sim/script_host.hpp"

namespace ron::sim::ctw {

using bhs::VP;

class ScriptRuntime {
public:
	explicit ScriptRuntime(ConquestGame& c) : cg(c) {
		game = std::make_unique<sim::Game>();
		game->rules = c.rules;
		game->base_rules = c.rules;
		game->ctw = true;  // the strategic hooks run with the CtW flag (general scripts consult it)
		game->info.difficulty = uint8_t(c.difficulty);
		game->scripts = std::make_unique<ScriptHost>(*game);
		host().files = c.files;
		host().ctw = &c;
		host().verbose = false;
	}
	ScriptHost& host() { return *game->scripts; }

	bool ensure(const std::string& path) {
		if (path.empty()) return false;
		if (bad.count(path)) return false;
		std::string err;
		if (!host().compile(path, &err)) {
			bad.insert(path);
			cg.log.push_back("script " + path + ": " + err);
			return false;
		}
		return true;
	}
	// Runs `name` with args; returns the Runtime status (kOk = 0).
	int run(const std::string& name, const std::vector<VP>& args) {
		// rand_int & co. draw from game_random: here the campaign's own stream, so a saved campaign replays identically
		game->rng.reseed(cg.random.seed());
		const int st = host().run(bhs::to_u16(name), args);
		cg.random.reseed(game->rng.seed());
		for (const std::string& m : host().messages) cg.log.push_back(m);
		host().messages.clear();
		sweep_popups();
		return st;
	}
	// popup_dialog / print_msg texts of the hooks become the UI's messages
	void sweep_popups() {
		auto& ev = host().pres.events;
		for (const ScriptEvent& e : ev) {
			if ((e.kind == ScriptEvent::kPopup || e.kind == ScriptEvent::kNotice) && !e.text.empty()) cg.messages.push_back(e.text);
		}
		ev.clear();
	}
	const bhs::RtScript* find(const std::string& name) {
		return host().rt.find_script(bhs::to_u16(name));
	}

	ConquestGame& cg;
	std::unique_ptr<sim::Game> game;
	std::set<std::string> bad;
};

ConquestGame::ConquestGame() = default;
ConquestGame::~ConquestGame() = default;

ScriptRuntime& ConquestGame::script_runtime() {
	if (!scripts) scripts = std::make_unique<ScriptRuntime>(*this);
	return *scripts;
}
void ConquestGame::scripts_drop() { scripts.reset(); }

std::string ConquestGame::tribe_key(int slot) const {
	if (!def || slot < 0 || slot >= kSlots) return std::string();
	return def->tribes[size_t(slot)].key;
}

std::string ConquestGame::territory_owner_name(int node, bool raw) const {
	const int o = nodes[size_t(node)].owner;
	return tribe_key(raw ? o : eff(o));
}

namespace {
VP str_arg(const std::string& s) { return bhs::make_string(bhs::to_u16(s), bhs::SC_VAR); }
VP int_arg(int v) { return bhs::make_int(v, bhs::SC_VAR); }
}  // namespace

void ConquestGame::run_init_script() {
	if (def->initialization_script.empty()) return;
	ScriptRuntime& sr = script_runtime();
	if (!sr.ensure(def->initialization_script)) return;
	sr.run("init_script", {});
}

void ConquestGame::run_post_turn() {
	if (def->post_turn_script.empty()) return;
	ScriptRuntime& sr = script_runtime();
	if (!sr.ensure(def->post_turn_script)) return;
	sr.run("post_turn", {int_arg(cur_round)});
}

void ConquestGame::run_post_ai() {
	if (def->post_ai_script.empty()) return;
	ScriptRuntime& sr = script_runtime();
	if (!sr.ensure(def->post_ai_script)) return;
	sr.run("post_ai", {});
}

bool ConquestGame::run_links_script(int slot) {
	if (def->links_script.empty()) return false;
	ScriptRuntime& sr = script_runtime();
	if (!sr.ensure(def->links_script)) return false;
	if (sr.run("links", {str_arg(tribe_key(slot))}) != 0) return false;
	return sr.host().rt.ret_int() != 0;
}

bool ConquestGame::run_strategy_script(int slot) {
	if (def->strategy_script.empty()) return false;
	ScriptRuntime& sr = script_runtime();
	if (!sr.ensure(def->strategy_script)) return false;
	if (sr.run("strategy", {str_arg(tribe_key(slot))}) != 0) return false;
	return sr.host().rt.ret_int() != 0;
}

std::string ConquestGame::run_battle_override(const LinkChoice& l, StyleChoice* style) {
	if (def->battle_script.empty()) return std::string();
	ScriptRuntime& sr = script_runtime();
	if (!sr.ensure(def->battle_script)) return std::string();
	const bool player_attacks = l.attacker == player_tribe;
	const int enemy = player_attacks ? l.defender : l.attacker;
	const int reinf = int(player_attacks ? l.att_supporters.size() : l.def_supporters.size());
	const int enemy_reinf = int(player_attacks ? l.def_supporters.size() : l.att_supporters.size());
	std::vector<VP> args = {str_arg(def->countries[size_t(l.from)].id), str_arg(def->countries[size_t(l.to)].id),
	                        str_arg(tribe_key(player_tribe)), str_arg(tribe_key(enemy)), int_arg(cur_round), int_arg(cur_age),
	                        int_arg(nodes[size_t(l.to)].level), int_arg(reinf), int_arg(enemy_reinf), int_arg(reinf),
	                        int_arg(enemy_reinf), str_arg(""), str_arg(""), str_arg("")};
	if (sr.run("battle_override", args) != 0) return std::string();
	if (style) {
		style->info_text = bhs::to_utf8(bhs::value_string(args[11].get()));
		style->goal1 = bhs::to_utf8(bhs::value_string(args[12].get()));
		style->goal2 = bhs::to_utf8(bhs::value_string(args[13].get()));
	}
	const VP& rv = sr.host().rt.return_value;
	return rv ? bhs::to_utf8(bhs::value_string(rv.get())) : std::string();
}

// ---------------------------------------------------------------- diplomacy deals

namespace {

// ConquestDiploOffer (game_structs.bhs): tribe, tribe2, accepted, counter_offer, tribe_trib, tribe2_trib,
// disable_counter_offer, tribe_terr[], tribe2_terr[], tribe_col[], tribe2_col[], tribe_cards[], tribe2_cards[],
// offer_type, response, id.
VP make_offer_value(const DiploOffer& o, int32_t struct_type) {
	auto v = std::make_shared<bhs::Value>();
	v->cls = bhs::Value::Object;
	v->data_type = struct_type;
	v->scope = bhs::SC_VAR;
	auto add = [&](VP e) {
		e->scope = bhs::SC_VAR;
		v->elems.push_back(e);
	};
	auto str_array = [&](const std::vector<std::string>& items) {
		auto a = std::make_shared<bhs::Value>();
		a->cls = bhs::Value::Array;
		a->data_type = int32_t(bhs::type_hash(u"string[]"));
		a->scope = bhs::SC_VAR;
		a->blank = bhs::make_string(bhs::U16(), bhs::SC_VAR);
		for (const std::string& s : items) a->elems.push_back(bhs::make_string(bhs::to_u16(s), bhs::SC_VAR));
		return a;
	};
	add(bhs::make_string(bhs::to_u16(o.tribe)));
	add(bhs::make_string(bhs::to_u16(o.tribe2)));
	add(bhs::make_int(o.accepted));
	add(bhs::make_int(o.counter_offer));
	add(bhs::make_int(o.tribe_trib));
	add(bhs::make_int(o.tribe2_trib));
	add(bhs::make_int(o.disable_counter_offer));
	add(str_array(o.tribe_terr));
	add(str_array(o.tribe2_terr));
	add(str_array(o.tribe_col));
	add(str_array(o.tribe2_col));
	add(str_array(o.tribe_cards));
	add(str_array(o.tribe2_cards));
	add(bhs::make_string(bhs::to_u16(o.offer_type)));
	add(bhs::make_string(bhs::to_u16(o.response)));
	add(bhs::make_string(bhs::to_u16(o.id)));
	return v;
}

std::vector<std::string> read_strings(const bhs::Value* a) {
	std::vector<std::string> out;
	if (!a) return out;
	for (const VP& e : a->elems) out.push_back(e ? bhs::to_utf8(bhs::value_string(e.get())) : std::string());
	return out;
}

void read_offer_value(const bhs::Value* v, DiploOffer* o) {
	if (!v || v->elems.size() < 16) return;
	auto S = [&](size_t k) { return bhs::to_utf8(bhs::value_string(v->elems[k].get())); };
	auto I = [&](size_t k) { return bhs::value_int(v->elems[k].get()); };
	o->tribe = S(0);
	o->tribe2 = S(1);
	o->accepted = I(2);
	o->counter_offer = I(3);
	o->tribe_trib = I(4);
	o->tribe2_trib = I(5);
	o->disable_counter_offer = I(6);
	o->tribe_terr = read_strings(v->elems[7].get());
	o->tribe2_terr = read_strings(v->elems[8].get());
	o->tribe_col = read_strings(v->elems[9].get());
	o->tribe2_col = read_strings(v->elems[10].get());
	o->tribe_cards = read_strings(v->elems[11].get());
	o->tribe2_cards = read_strings(v->elems[12].get());
	o->offer_type = S(13);
	o->response = S(14);
	o->id = S(15);
}

}  // namespace

bool ConquestGame::run_diplo_response(DiploOffer* offer) {
	std::string path = def->diplo_response_script;
	if (path.empty()) path = "conquest/scripts/diplo_response.bhs";
	ScriptRuntime& sr = script_runtime();
	if (!sr.ensure(path)) return false;
	const bhs::RtScript* s = sr.find("diplo_response");
	if (!s || s->params.size() != 1) return false;
	VP v = make_offer_value(*offer, s->params[0]);
	if (sr.run("diplo_response", {v}) != 0) return false;
	read_offer_value(v.get(), offer);
	return true;
}

bool ConquestGame::run_diplo_offer(int slot, DiploOffer* o) {
	if (def->diplo_offer_script.empty()) return false;
	ScriptRuntime& sr = script_runtime();
	if (!sr.ensure(def->diplo_offer_script)) return false;
	const bhs::RtScript* s = sr.find("diplo_offer");
	if (!s || s->params.size() != 1) return false;
	*o = DiploOffer();
	o->tribe = tribe_key(slot);
	o->tribe2 = tribe_key(player_tribe);
	VP v = make_offer_value(*o, s->params[0]);
	if (sr.run("diplo_offer", {v}) != 0 || sr.host().rt.ret_int() == 0) return false;
	read_offer_value(v.get(), o);
	return !o->offer_type.empty();
}

std::string ConquestGame::answer_proposal(const DiploOffer& o, bool accept) {
	std::string err;
	if (accept) err = apply_offer(o);
	if (!o.id.empty()) deals[o.id] = accept && err.empty() ? 1 : 0;
	return err;
}

std::string ConquestGame::apply_offer(const DiploOffer& o) {
	const int a = slot_of(o.tribe), b = slot_of(o.tribe2);
	if (a < 0 || b < 0) return "unknown nation";
	Leader& A = leaders[size_t(a)];
	Leader& B = leaders[size_t(b)];
	const int give = std::min(o.tribe_trib, A.tribute), take = std::min(o.tribe2_trib, B.tribute);
	A.tribute += take - give;
	B.tribute += give - take;
	for (const std::string& t : o.tribe_terr) {
		const int n = find_node(t);
		if (n >= 0 && nodes[size_t(n)].owner == a) transfer_to(n, b, false, nullptr);
	}
	for (const std::string& t : o.tribe2_terr) {
		const int n = find_node(t);
		if (n >= 0 && nodes[size_t(n)].owner == b) transfer_to(n, a, false, nullptr);
	}
	for (const std::string& cname : o.tribe_col) {
		for (Colony& c : colonies) {
			if (ieq(c.id, cname) && c.owner == a) c.owner = b;
		}
	}
	for (const std::string& cname : o.tribe2_col) {
		for (Colony& c : colonies) {
			if (ieq(c.id, cname) && c.owner == b) c.owner = a;
		}
	}
	auto move_cards = [&](const std::vector<std::string>& names, Leader& from, int to) {
		for (const std::string& n : names) {
			Card c;
			if (!parse_card(n, &c)) continue;
			auto it = std::find(from.cards.begin(), from.cards.end(), c);
			if (it == from.cards.end()) continue;
			from.cards.erase(it);
			give_card(to, c);
		}
	};
	move_cards(o.tribe_cards, A, b);
	move_cards(o.tribe2_cards, B, a);
	const std::string t = lower(o.offer_type);
	if (t == "peace") set_peace(a, b);
	else if (t == "ally") set_ally(a, b);
	else if (t == "vassal") set_vassal(a, b);
	assign_bonuses();
	return std::string();
}

// ---------------------------------------------------------------- script statics (the save)

namespace {

struct ValueTable {
	std::unordered_map<const ron::bhs::Value*, uint32_t> ids;
	std::vector<ron::bhs::VP> vec;
};

// A script value graph with shared identity (the same walk as sim/save.cpp's walk_value).
template <class Ar>
void walk_value(Ar& ar, VP& p, ValueTable& t) {
	using namespace ron::sim::save;
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
		t.ids[p.get()] = uint32_t(t.ids.size());
		bhs::Value& v = *p;
		fields(ar, v.cls, v.scope, v.data_type, v.i, v.f, v.s);
		ar.count(v.elems.size());
		for (VP& e : v.elems) walk_value(ar, e, t);
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
			for (VP& e : v.elems) walk_value(ar, e, t);
			walk_value(ar, v.blank, t);
		} else {
			ar.error("bad script value tag");
		}
	}
}

struct StaticsEntry {
	std::string file;
	bhs::U16 script;
	std::vector<VP> vars;
};

template <class Ar>
void walk_statics(Ar& ar, std::vector<StaticsEntry>& list) {
	using namespace ron::sim::save;
	ValueTable table;
	uint32_t n = uint32_t(list.size());
	io(ar, n);
	if constexpr (Ar::kReading) {
		if (size_t(n) > ar.remaining()) ar.error("static entries exceed the section");
		list.assign(n, StaticsEntry());
	}
	for (StaticsEntry& e : list) {
		io(ar, e.file);
		io(ar, e.script);
		uint32_t nv = uint32_t(e.vars.size());
		io(ar, nv);
		if constexpr (Ar::kReading) {
			if (size_t(nv) > ar.remaining()) ar.error("static count exceeds the section");
			e.vars.assign(nv, nullptr);
		}
		for (VP& v : e.vars) walk_value(ar, v, table);
	}
}

}  // namespace

std::vector<uint8_t> ConquestGame::script_statics() {
	std::vector<StaticsEntry> list;
	if (scripts) {
		for (auto& f : scripts->host().rt.files) {
			if (!f) continue;
			for (bhs::RtScript& s : f->scripts) {
				if (s.static_vars.empty()) continue;
				list.push_back({f->source_file, s.name, s.static_vars});
			}
		}
	}
	// canonical order (the files are compiled in the order the hooks first ran; a loaded campaign compiles them in a fixed order)
	std::sort(list.begin(), list.end(), [](const StaticsEntry& a, const StaticsEntry& b) {
		return a.file != b.file ? a.file < b.file : a.script < b.script;
	});
	save::Writer w;
	w.section("STAT", [&](auto& s) { walk_statics(s, list); });
	return std::move(w.buf);
}

void ConquestGame::restore_script_statics(const std::vector<uint8_t>& blob) {
	std::vector<StaticsEntry> list;
	{
		save::Reader r(blob.data(), blob.size(), "script statics");
		r.section("STAT", [&](auto& s) { walk_statics(s, list); });
	}
	if (list.empty()) return;
	ScriptRuntime& sr = script_runtime();
	for (const std::string& p : {def->initialization_script, def->links_script, def->strategy_script, def->post_ai_script,
	                            def->post_turn_script, def->battle_script, def->diplo_response_script.empty() ? std::string("conquest/scripts/diplo_response.bhs") : def->diplo_response_script,
	                            def->diplo_offer_script}) {
		sr.ensure(p);
	}
	for (StaticsEntry& e : list) {
		for (auto& f : sr.host().rt.files) {
			if (!f || !bhs::ieq(bhs::to_u16(f->source_file), bhs::to_u16(e.file))) continue;
			for (bhs::RtScript& s : f->scripts) {
				if (s.name == e.script) s.static_vars = e.vars;
			}
		}
	}
}

}  // namespace ron::sim::ctw
