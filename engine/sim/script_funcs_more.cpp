// The scenario builtins that the shipped Conquer the World battle scripts call and that the other script_funcs*.cpp files did not
// have (docs/script-builtins.md lists them with their call counts): the hero / spy craft wrappers (ScenarioFuncSet::cast_spell
// callers), craft points, ScenarioData ignore_orders, resources, cities, diplomacy switches and a few readers. Each body follows its
// ScenarioFuncSet decompile or disassembly (re/decomp/script_funcs; the wrappers that pass their craft in ECX were read from the
// disassembly, see the comments), with the original's validity checks in the original's order.
#include <algorithm>

#include "script/bhs_stdlib.hpp"
#include "sim/ai.hpp"
#include "sim/capture.hpp"
#include "sim/combat.hpp"
#include "sim/ctw.hpp"
#include "sim/economy.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/heroes.hpp"
#include "sim/orders.hpp"
#include "sim/production.hpp"
#include "sim/script_funcs_internal.hpp"
#include "sim/spells.hpp"

namespace ron::sim {
namespace sf {

namespace {

std::string u8(const U16& s) { return bhs::to_utf8(s); }

// The five script crafts of the hero / spy wrappers (CraftTypes 0x277..0x281; heroes-spec 2, spells-spec 1).
constexpr int32_t kCraftDecoy = 0x27a, kCraftAmbush = 0x27b, kCraftCounterIntel = 0x277, kCraftInfiltrate = 0x27f,
                  kCraftSabotage = 0x280, kCraftSniper = 0x281;

}  // namespace

void install_more_funcs(ScriptHost& h) {
	Game& g = h.game;
	bhs::Runtime& rt = h.rt;
	auto def = [&rt](const char* name, Fn f, int overload = 0) { rt.set_native(name, std::move(f), overload); };
	auto cg = [&h]() { return h.ctw; };

	// ---- map and presentation readers
	def("get_tileset", [&h, cg](NativeCall&) {  // @9e4d20: tilesets.cur_tileset->name (the scenario's tileset)
		if (!h.tileset.empty()) return make_string(bhs::to_u16(h.tileset));
		// A generated Conquer the World battle: the tileset the battle was built with (ConquestGame::make_battle: the defended node's, the
		// attacker's node for the "attacker" map style).
		if (ctw::ConquestGame* c = cg()) {
			const int idx = c->cur_style.index;
			const bool attacker = idx >= 0 && size_t(idx) < c->style_table.size() && c->style_table[size_t(idx)].map_style == ctw::kMsAttacker;
			const int n = attacker ? c->cur_link.from : c->cur_link.to;
			if (n >= 0 && size_t(n) < c->nodes.size() && c->nodes[size_t(n)].tileset != "dirty") return make_string(bhs::to_u16(c->nodes[size_t(n)].tileset));
		}
		return make_string(U16());
	});
	def("ui_hide_score", [&g](NativeCall&) {  // @9ff4c0: Window::hide(stat_win): the renderer hides its score window
		if (g.scripts) {
			ScriptEvent& e = push_event(g, ScriptEvent::kCamera);
			e.a = CameraRequest::kNone;  // a no-op camera event marks the change (as clear_selection does)
			e.text = "hide_score";
		}
		return make_int(1);
	});
	// MessageWin::change_ctw_objective @7e82d0 / remove_ctw_objective @7e7f00: the CTW objective list is the same
	// ScriptPresentation::objectives list here (ctw_add_objective); entries are matched by their exact id.
	def("ctw_change_objective_text", [&g](NativeCall& c) {
		const U16 id = c.arg_str(0);
		for (ScriptObjective& o : pres(g).objectives) {
			if (bhs::ieq(bhs::to_u16(o.id), id)) o.text = u8(c.arg_str(1));
		}
		return make_int(1);
	});
	def("ctw_remove_objective", [&g](NativeCall& c) {
		const U16 id = c.arg_str(0);
		auto& v = pres(g).objectives;
		v.erase(std::remove_if(v.begin(), v.end(), [&id](const ScriptObjective& o) { return bhs::ieq(bhs::to_u16(o.id), id); }), v.end());
		return make_int(1);
	});
	def("select_object", [&g](NativeCall& c) {  // @9f0760: the console's selection becomes the one object (single player only)
		const int32_t w = c.arg_int(0) - 1, tw = c.arg_int(1) - 1, to = c.arg_int(2);
		if (!live_leader(g, w) || !live_leader(g, tw) || mp(g) || !valid_object(g, tw, to)) return make_int(-1);
		ScriptInput& in = input(g);
		in.select_who = tw;
		in.selected.assign(1, to);
		ScriptEvent& e = push_event(g, ScriptEvent::kCamera);  // the renderer applies it (as clear_selection)
		e.a = CameraRequest::kNone;
		e.text = "select_object";
		e.who = tw;
		e.x = to;
		return make_int(1);
	});
	def("switch_player_control", [&g](NativeCall& c) {  // @9e5960: the console takes the seat (single player only)
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w) || mp(g)) return make_int(-1);
		const int32_t old = g.local_play;
		g.local_play = w;
		if (uint32_t(old) < 8) g.leaders[size_t(old)].flags &= ~leaderflag::kHuman;
		g.leaders[size_t(w)].flags |= leaderflag::kHuman;
		if (g.scripts) {
			input(g).selected.clear();
			input(g).select_who = -1;
			ScriptEvent& e = push_event(g, ScriptEvent::kCamera);  // SelectGroup::clear, Camera::outdate, fog.flags |= 2
			e.a = CameraRequest::kNone;
			e.text = "clear_selection";
		}
		g.world.fog_changes++;
		return make_int(1);
	});

	// ---- leaders and diplomacy
	def("set_plunder_scale", [&g](NativeCall& c) {  // @9e5450: Leader +0x914, never negative
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		const float v = c.arg_float(1);
		g.leaders[size_t(w)].plunder_scale = v <= 0.f ? 0.f : v;
		return make_int(1);
	});
	def("enable_city_defeat", [&g](NativeCall& c) {  // @9ffc40: leader_flags2 &= ~1
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.leaders[size_t(w)].flags2 &= ~leaderflag2::kNoCityDefeat;
		return make_int(1);
	});
	def("offer_alliance", [&g](NativeCall& c) {  // @9fc270: Leader::action_treaty(whom, 2)
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b) || a == b) return make_int(-1);
		leader_action_treaty(g, a, b, 2);
		return make_int(1);
	});
	def("set_contact", [&g](NativeCall& c) {  // @9fcde0: treaties[whom] bit 0 both ways
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b)) return make_int(-1);
		if (c.arg_int(2) == 0) {
			g.leaders[size_t(a)].treaty(b) &= ~1;
			g.leaders[size_t(b)].treaty(a) &= ~1;
		} else {
			g.leaders[size_t(a)].treaty(b) |= 1;
			g.leaders[size_t(b)].treaty(a) |= 1;
		}
		return make_int(1);
	});
	def("have_trade", [&g](NativeCall& c) {  // @9fcea0: a caravan in use between the two leaders
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b)) return make_int(-1);
		const auto& cv = g.caravans[size_t(a)];
		for (int32_t k = 0; k < g.leaders[size_t(a)].caras && size_t(k) < cv.size(); k++) {
			const CaravanRec& r = cv[size_t(k)];
			if ((r.caravan_flags & 1) && (r.whom == b || r.whose == b)) return make_int(1);
		}
		return make_int(0);
	});
	def("get_commerce_cap", [&g](NativeCall& c) {  // @9fbcc0: the commerce cap of a good (rate cap x16, stored at LeaderData +0x30 + 4t)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (!live_leader(g, w) || uint32_t(t) >= 0x32) return make_int(-1);
		// (the six goods and the unused slot 6 are the leader's resource_cap; the original reads the neighbouring leader fields for the
		// other goods, which a script never asks for)
		if (t >= int32_t(g.leaders[size_t(w)].resource_cap.size())) return make_int(-1);
		const int32_t v = g.leaders[size_t(w)].resource_cap[size_t(t)];
		return make_int((v + ((v >> 31) & 0xf)) >> 4);
	});
	def("defeat_attacker_and_allies", [&g, cg](NativeCall&) {  // @9e5f60 (Conquer the World only)
		ctw::ConquestGame* cc = cg();
		if (!g.ctw || !cc) return make_int(-1);
		const int from = cc->cur_link.from;
		if (from < 0 || size_t(from) >= cc->nodes.size()) return make_int(-1);
		const int eff = cc->node_eff_owner(from);  // the attacking node's owner, or its master
		for (int32_t p = 0; p < 8; p++) {
			if (!(g.leaders[size_t(p)].flags & leaderflag::kValid) || cc->battle_slot[size_t(p)] != eff) continue;
			for (int32_t q = 0; q < 8; q++) {
				if (p == q || (g.leaders[size_t(q)].diplos(p) == 2 && g.leaders[size_t(p)].diplos(q) == 2))
					leader_defeat(g, q, 0, -1, 0);
			}
			leader_defeat(g, p, 0, -1, 0);
			return make_int(1);
		}
		return make_int(-1);
	});

	// ---- cities
	def("get_city_name", [&g](NativeCall& c) {  // @9efd00: the name of the city a building belongs to ("" when it is not a city building)
		const int32_t w = c.arg_int(0) - 1;
		const Build* b = valid_build(g, w, c.arg_int(1));
		if (!b || !(b->flags & objflag::kCity) || b->city < 0 || size_t(b->city) >= g.cities[size_t(w)].size()) return make_string(U16());
		return make_string(bhs::to_u16(g.cities[size_t(w)][size_t(b->city)].name));
	});
	// rename_city by name @9effc0 issues CommandManager::issue_rename_city (applied by process_rename_city one turn later; applied
	// directly here: the 21 character trim, then the command's 20 characters); by building @9f00d0 is City::set_name.
	def("rename_city", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		const int32_t ci = city_index(g, w, c.arg_str(1));
		if (ci < 0) return make_int(-1);
		U16 name = c.arg_str(2);
		while (!name.empty() && name.back() == u' ') name.pop_back();
		size_t a = 0;
		while (a < name.size() && name[a] == u' ') a++;
		name = name.substr(a);
		if (name.size() > 0x15) name.resize(0x15);
		if (name.empty()) return make_int(-1);
		City& ct = g.cities[size_t(w)][size_t(ci)];
		const Object* ob = g.object(w, ct.o);
		if (ob && ob->alive() && !ob->is_unit && (ob->flags & objflag::kCity)) ct.name = u8(name.substr(0, 20));
		return make_int(1);
	}, 0);
	def("rename_city", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		const Build* b = valid_build(g, w, c.arg_int(1));
		if (!b || b->city < 0 || size_t(b->city) >= g.cities[size_t(w)].size()) return make_int(-1);
		g.cities[size_t(w)][size_t(b->city)].name = u8(c.arg_str(2));
		return make_int(1);
	}, 1);

	// ---- objects
	def("object_type", [&g](NativeCall& c) {  // @9f1580
		const Object* ob = valid_object(g, c.arg_int(0) - 1, c.arg_int(1));
		return make_string(ob && ob->ptype ? ob->ptype->name : U16());
	});
	def("building_active", [&g](NativeCall& c) {  // @9f1ea0
		const Build* b = valid_build(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!b) return make_int(-1);
		return make_int((b->flags & objflag::kValid) && (b->flags & objflag::kActive) ? 1 : 0);
	});
	def("is_object_at", [&g](NativeCall& c) {  // @9f0b70: the tile of the object (a unit: its captain)
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		if (!valid_object(g, w, o)) return make_int(-1);
		o = captain_of(g, w, o);
		const Object* ob = object_at(g, w, o);
		if (!ob) return make_int(0);
		return make_int(tile_of(ob->x) == c.arg_int(2) && tile_of(ob->y) == c.arg_int(3) ? 1 : 0);
	});
	def("is_gathering", [&g](NativeCall& c) {  // @9f9420: UnitData::is_gathering
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w) || !valid_object(g, w, c.arg_int(1))) return make_int(-1);
		const Object* ob = object_at(g, w, c.arg_int(1));
		return make_int(ob && ob->is_unit && unit_is_gathering(g, *static_cast<const Unit*>(ob)) ? 1 : 0);
	});
	def("set_rally", [&g](NativeCall& c) {  // @9f2020: Build::add_gather_point(tile centre, 1, QUEUE_NEW)
		const int32_t w = c.arg_int(0) - 1;
		Build* b = valid_build(g, w, c.arg_int(1));
		if (!b) return make_int(-1);
		build_add_gather_point(g, *b, c.arg_int(2) * 0xc0 + 0x60, c.arg_int(3) * 0xc0 + 0x60, 1, kQueueNew);
		return make_int(1);
	});
	def("num_civilian_buildings", [&g](NativeCall& c) {  // @9e9ce0: the building counts of category 0
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		for (int32_t k = 0; k < 129; k++) {
			if (g.type(kBuilds + k).cat == 0) n += uint16_t(g.leaders[size_t(w)].num_buildings[size_t(k)]);
		}
		return make_int(n);
	});
	def("find_civilian_build", [&g, &h](NativeCall& c) {  // @9ec630 (counter 0x15): the next building of a civilian line
		auto& fc = h.find_counters;
		const int32_t w = c.arg_int(0) - 1;
		const int32_t mark = uint32_t(w) < 8 ? g.objects[size_t(w)].build_mark : 0;
		if (fc[0x15] < 0 || mark <= fc[0x15]) fc[0x15] = kBuildBase;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t start = fc[0x15];
		int32_t i = start + 1;
		for (;;) {
			if (i >= mark) i = kBuildBase;
			const Build* b = build_at(g, w, i);
			if (b && (b->flags & objflag::kValid)) {
				const Type& T = *b->ptype;
				if ((!(T.build_flags & 0x2000) && !(T.build_flags & 0x10)) || type_is(T, 0x19e) || (T.build_flags & 0x40)) {
					fc[0x15] = i;
					return make_int(i);
				}
			}
			i++;
			if (i == start + 1) return make_int(-1);
		}
	});

	// ---- resources on the map (the goods array: find_counters[0x1a])
	def("find_resource", [&g, &h](NativeCall& c) {  // @a015d0 (the counter keeps found + 1, as the original stores it)
		auto& fc = h.find_counters;
		const int32_t n = int32_t(g.goods.size());
		if (fc[0x1a] < 0 || n <= fc[0x1a]) fc[0x1a] = 0;
		const int32_t t = type_index(g, c.arg_str(0));
		int32_t i = fc[0x1a] + 1;
		while (i != fc[0x1a]) {
			if (i < 0) i = 0;
			if (i < n) {
				const GoodObj& go = g.goods[size_t(i)];
				if (go.alive() && go.type == t) {
					fc[0x1a] = i + 1;
					return make_int(i);
				}
				i++;
			} else {
				i = 0;
			}
		}
		return make_int(-1);
	});
	auto resource_pos = [&g](NativeCall& c, bool y) {  // @a016b0 / @a01700
		const int32_t i = c.arg_int(0);
		if (i < 0 || size_t(i) >= g.goods.size() || !g.goods[size_t(i)].alive()) return make_int(-1);
		return make_int(tile_of(y ? g.goods[size_t(i)].y : g.goods[size_t(i)].x));
	};
	def("resource_position_x", [resource_pos](NativeCall& c) { return resource_pos(c, false); });
	def("resource_position_y", [resource_pos](NativeCall& c) { return resource_pos(c, true); });
	def("num_rare_resources", [&g](NativeCall& c) {  // @a004b0: the goods on the map, but oil, items, rares and whales, that the leader can use
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		for (const GoodObj& go : g.goods) {
			if (!go.alive() || go.type == 5 /*OIL*/ || go.type == 543 /*BASE_ITEMTYPES*/ || go.type == 6 /*BASE_RARE: fish*/ || go.type == 31 /*WHALES*/) continue;
			if (type_avail(g, w, go.type, 1) != 0) n++;
		}
		return make_int(n);
	});

	// ---- craft points (UnitData::mana, Unit.mana_burn @+0x96)
	def("unit_craft", [&g](NativeCall& c) {  // @9f6830
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_unit(g, w, c.arg_int(1))) return make_int(-1);
		const Unit* u = unit_at(g, w, get_captain(g, w, c.arg_int(1)));
		return make_int(std::max(0, unit_mana(g, *u) - u->mana_burn));
	});
	def("unit_max_craft", [&g](NativeCall& c) {  // @9f6ab0
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_unit(g, w, c.arg_int(1))) return make_int(-1);
		return make_int(unit_mana(g, *unit_at(g, w, get_captain(g, w, c.arg_int(1)))));
	});
	def("craft_percent", [&g](NativeCall& c) {  // @9f90c0
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w) || !valid_object(g, w, c.arg_int(1))) return make_int(-1);
		const Object* ob = object_at(g, w, c.arg_int(1));
		if (!ob || !ob->is_unit) return make_int(0);
		const Unit& u = *static_cast<const Unit*>(ob);
		const int32_t m = unit_mana(g, u);
		if (m == 0) return make_int(0);
		return make_int((std::max(0, m - u.mana_burn) * 100) / m);
	});
	def("set_unit_craft", [&g](NativeCall& c) {  // @9f68a0: mana_burn = mana * (100 - percent) / 100 (the x87 double products of the original)
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_unit(g, w, c.arg_int(1)) || c.arg_int(2) < 0) return make_int(-1);
		Unit* u = unit_at(g, w, get_captain(g, w, c.arg_int(1)));
		const int32_t p = std::min(100, c.arg_int(2));
		const double r = double(float(unit_mana(g, *u))) * (double(float(100 - p)) / 100.0);
		u->mana_burn = int16_t(int32_t(r));
		return make_int(1);
	});
	def("set_unit_type_max_craft", [&g](NativeCall& c) {  // @9f6980: mana of every unit type on the line, then Leader::calc_unit_stats
		const int32_t t = type_index(g, c.arg_str(0));
		if (t < 0 || c.arg_int(1) < 1 || !is_unit_type(t)) return make_int(-1);
		Rules& R = g.mut_rules();
		for (int32_t k = kUnits; k < kEndUnits; k++) {
			if (!type_is(R.types[size_t(k)], t)) continue;
			R.types[size_t(k)].mana = c.arg_int(1);
			R.types[size_t(k)].modified = 1;
		}
		for (int32_t w = 0; w < 8; w++) {
			if (live_leader(g, w)) calc_unit_stats(g, w);
		}
		return make_int(t);
	});

	// ---- hero and spy crafts: ScenarioFuncSet::cast_spell(craft in ECX, unit in EDX, who, ox, whom, x, y) -> Unit::add_cast_order
	def("general_ambush", [&g](NativeCall& c) {  // @9f8b80: no validity check of its own (cast_spell checks the unit)
		return make_int(scenario_cast_spell(g, c.arg_int(0) - 1, c.arg_int(1), -1, -1, -1, -1, kCraftAmbush));
	});
	def("general_is_ambushing", [&g](NativeCall& c) {  // @9f8ce0: unit_masks & CLOAK_RUNNING (raw)
		const Unit* u = valid_unit(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!u) return make_int(-1);
		return make_int(int32_t(u->unit_masks & umask::kCloakRunning));
	});
	// general_decoy @9f8b00 checks the target pair with the *raw* arguments (leaders[who_target], valid_unit_o(who_target, unit_o_target):
	// no "- 1"), then casts DECOY at ox = unit_o_target, whom = who_target (raw).
	def("general_decoy", [&g](NativeCall& c) {
		const int32_t tw = c.arg_int(2), to = c.arg_int(3);
		if (uint32_t(tw) > 7 || !live_leader(g, tw) || !valid_unit(g, tw, to)) return make_int(-1);
		return make_int(scenario_cast_spell(g, c.arg_int(0) - 1, c.arg_int(1), to, tw, -1, -1, kCraftDecoy));
	});
	// special_forces_* @9f8ec0..9f9040: only the target (who_target - 1, object_o) is checked here; the caster by cast_spell.
	auto special = [&g](NativeCall& c, int32_t craft) {
		const int32_t tw = c.arg_int(2) - 1, to = c.arg_int(3);
		if (!live_leader(g, tw) || !valid_object(g, tw, to)) return make_int(-1);
		return make_int(scenario_cast_spell(g, c.arg_int(0) - 1, c.arg_int(1), to, tw, -1, -1, craft));
	};
	def("special_forces_infiltrate", [special](NativeCall& c) { return special(c, kCraftInfiltrate); });
	def("special_forces_sabotage", [special](NativeCall& c) { return special(c, kCraftSabotage); });
	def("special_forces_sniper", [special](NativeCall& c) { return special(c, kCraftSniper); });
	def("special_forces_counterintel", [special](NativeCall& c) { return special(c, kCraftCounterIntel); });
	def("scout_counter_intel", [&g, special](NativeCall& c) {  // @a00170: also checks both leaders and the caster unit first
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w) || !live_leader(g, c.arg_int(2) - 1) || !valid_unit(g, w, c.arg_int(1))) return make_int(-1);
		return special(c, kCraftCounterIntel);
	});

	// ---- units
	def("kill_unit_type_anim", [&g](NativeCall& c) {  // @9f6ea0: every valid unit of the type (any type when ""), dying with animation `anim`
		const int32_t w = c.arg_int(0) - 1;
		int32_t t = -1;
		if (!c.arg_str(1).empty()) t = type_index(g, c.arg_str(1));
		const int32_t anim = c.arg_int(2);
		if ((!c.arg_str(1).empty() && t < 0) || anim < 1 || anim > 4 || !valid_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		for (int32_t k = 0; k < g.objects[size_t(w)].unit_mark; k++) {
			Unit* u = unit_at(g, w, k);
			if (!u || !(u->flags & objflag::kValid)) continue;
			if (t == 0 || (t > 0 && !type_is(*u->ptype, t))) continue;
			object_die(g, *u, anim - 1, -1, 0.f);
			n++;
		}
		return make_int(n);
	});
	def("unit_obey_orders", [&g](NativeCall& c) {  // @a017d0: the id leaves ScenarioData::objects_ignoring_orders[who] (slot set to -1)
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		for (int32_t& o : g.scen.objects_ignoring_orders[size_t(w)]) {
			if (o == c.arg_int(1)) o = -1;
		}
		return make_int(1);
	});
	def("object_ignore_orders", [&g](NativeCall& c) {  // @a01850 (same body as unit_ignore_orders @a01750)
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.scen.ignore_orders = 1;
		g.scen.objects_ignoring_orders[size_t(w)].push_back(c.arg_int(1));
		return make_int(1);
	});

	// ---- techs
	def("enable_tech", [&g](NativeCall& c) {  // @9ea590: the tech is offered to the leader again (TechTypeData leader_off bit cleared)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || is_unit_type(t) || is_build_type(t) || !is_tech_type(t)) return make_int(-1);
		if (uint32_t(w) < 8 && (g.rules->types[size_t(t)].leader_off & (1u << w))) {
			Type& T = g.mut_rules().types[size_t(t)];
			T.leader_off = uint8_t(T.leader_off & ~(1u << w));
		}
		return make_int(t);
	});
}

}  // namespace sf
}  // namespace ron::sim
