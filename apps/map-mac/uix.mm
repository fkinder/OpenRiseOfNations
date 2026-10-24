// HUD logic of the third pass (docs/ui-port.md section 13). See uix.hpp.
#include "uix.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#include "sim/aircraft.hpp"
#include "sim/combat.hpp"
#include "sim/commands.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/heroes.hpp"
#include "sim/orders.hpp"
#include "sim/placement.hpp"
#include "sim/production.hpp"
#include "sim/rules_xml.hpp"
#include "sim/trig.hpp"

using namespace ron::sim;

namespace uix {

namespace {

constexpr int kMaxSelect = 128;

QueuePos queue_of(Mods m) { return m.shift ? kQueueLast : m.alt ? kQueueFirst : kQueueNew; }

const Unit* unit_of(const Ctx& c, const std::pair<int32_t, int32_t>& k) { return c.game->unit(k.first, k.second); }

bool is_air(const Unit& u) { return u.ptype && u.ptype->domain == kDomainAir; }
bool is_heli(const Unit& u) { return u.ptype && u.ptype->domain == kDomainAir && (u.ptype->unit_flags & 0x20) != 0; }
bool is_container(const Object& ob) {  // airbase or carrier (IFaceAirGarrison)
	return ob.ptype && (type_is(*ob.ptype, airtype::kAirbase, true) || type_is(*ob.ptype, airtype::kCarrier, true));
}

std::vector<int32_t> own(const Ctx& c, const Sel& s) {
	std::vector<int32_t> v;
	for (const auto& [who, o] : s) {
		if (who == c.me) v.push_back(o);
	}
	return v;
}

// RON_UIX_TRACE=1 prints each command this module issues (a test aid; it changes nothing).
bool issue(Ctx& c, const char* what, const std::function<bool()>& fn) {
	if (!c.game) return false;
	c.cm->play = c.me, c.cm->play_who = c.me;
	const bool ok = fn();
	if (std::getenv("RON_UIX_TRACE")) std::fprintf(stderr, "[uix] f%d %s %s\n", c.game->frame, what, ok ? "issued" : "refused");
	if (!ok) return false;
	if (c.process) c.process();
	return true;
}

// The objects a group command applies to: the units when some are selected, else the buildings.
std::vector<int32_t> group_objs(const Ctx& c) {
	if (c.units && !c.units->empty()) return own(c, *c.units);
	return own(c, *c.builds);
}

hud::Option* add(std::vector<hud::Option>& out, int option, int gx, int gy, bool enabled, bool latched = false) {
	for (const hud::Option& o : out) {
		if (o.grid_x == gx && o.grid_y == gy) return nullptr;  // a slot clash drops the later option (Options::add)
	}
	hud::Option o;
	o.option = option, o.grid_x = gx, o.grid_y = gy, o.enabled = enabled, o.latched = latched;
	out.push_back(o);
	return &out.back();
}

bool slot_free(const std::vector<hud::Option>& out, int gx, int gy) {
	for (const hud::Option& o : out) {
		if (o.grid_x == gx && o.grid_y == gy) return false;
	}
	return true;
}

int fighter_line(const Object& container) { return type_is(*container.ptype, airtype::kCarrier, true) ? airtype::kFighterBomber : airtype::kBiplane; }

// 0 fighters, 1 bombers, 2 helicopters, -1 not a family of the garrison panel.
int family_of(const Ctx& c, const Object& container, const Unit& u) {
	if (!is_air(u)) return -1;
	if (unit_is(*c.game, u, fighter_line(container))) return 0;
	if (unit_is(*c.game, u, airtype::kBomber)) return 1;
	if (unit_is(*c.game, u, airtype::kHelicopter)) return 2;
	return -1;
}

void say(const Ctx& c, const std::string& s) {
	if (c.say) c.say(s);
}

void set_selection(Ctx& c, const Sel& units, const Sel& builds) {
	*c.units = units, *c.builds = builds;
	if (c.units->size() > kMaxSelect) c.units->resize(kMaxSelect);
	if (c.builds->size() > kMaxSelect) c.builds->resize(kMaxSelect);
}

int type_of_entry(const Ctx& c, const std::pair<int32_t, int32_t>& k) {
	const Object* ob = c.game->object(k.first, k.second);
	return ob ? ob->type : -1;
}

}  // namespace

// ---------------------------------------------------------------- panels

hud::AirGarrison air_garrison(const Ctx& c, const IconFn& icon) {
	hud::AirGarrison a;
	if (!c.game || c.units->size() + c.builds->size() != 1) return a;
	const int32_t who = c.builds->empty() ? c.units->front().first : c.builds->front().first;
	const int32_t o = c.builds->empty() ? c.units->front().second : c.builds->front().second;
	const Object* ob = c.game->object(who, o);
	if (!ob || !ob->alive() || !is_container(*ob) || (who != c.me && !is_ally(*c.game, who, c.me))) return a;
	a.active = true, a.who = who, a.o = o, a.own = who == c.me, a.carrier = type_is(*ob->ptype, airtype::kCarrier, true);
	long fuel_sum[3][2] = {};
	int32_t first_type[3] = {-1, -1, -1}, first_o[3] = {-1, -1, -1};
	int32_t cw = ob->inside_down_who, co = ob->inside_down;
	for (int guard = 0; co >= 0 && cw >= 0 && guard < 4096; guard++) {
		const Unit* u = c.game->unit(cw, co);
		if (!u) break;
		if (u->alive()) {
			const int f = family_of(c, *ob, *u);
			if (f >= 0) {
				const int32_t full = unit_mana(*c.game, *u), left = unit_mana_left(*c.game, *u);
				const int col = left != full ? 1 : 0;  // fuelling: the fuel is not full
				hud::AirCell& cell = a.cell[f][col];
				cell.count++;
				cell.objs.push_back(u->o);
				fuel_sum[f][col] += full > 0 ? left * 100 / full : 100;
				if (f == 2) a.helis = true;
				if (first_type[f] < 0) first_type[f] = u->type, first_o[f] = u->o;
			}
		}
		cw = u->inside_down_who, co = u->inside_down;
	}
	if (const char* test = getenv("RON_AIR_TEST")) {  // "family:column:count:fuel,..." (0 fighters, 1 bombers, 2 helicopters) for snapshots
		for (const char* q = test; q && *q;) {
			int f = 0, col = 0, n = 0, fuel = 100;
			std::sscanf(q, "%d:%d:%d:%d", &f, &col, &n, &fuel);
			if (f >= 0 && f < 3 && col >= 0 && col < 2) {
				a.cell[f][col].count = n, fuel_sum[f][col] = long(fuel) * n;
				a.cell[f][col].objs.assign(size_t(n), 0);
				if (f == 2) a.helis = n > 0;
			}
			q = std::strchr(q, ',');
			if (q) q++;
		}
	}
	for (int f = 0; f < 3; f++) {
		for (int col = 0; col < 2; col++) {
			hud::AirCell& cell = a.cell[f][col];
			cell.fuel = cell.count > 0 ? int(fuel_sum[f][col] / cell.count) : 100;
		}
		// the family's icon: the leader's current upgrade of the line
		int32_t line = f == 0 ? fighter_line(*ob) : f == 1 ? airtype::kBomber : airtype::kHelicopter;
		const int32_t t = first_type[f] >= 0 ? first_type[f] : current_upgrade(*c.game, who, line);
		if (icon) icon(t, who, first_o[f], a.icon[f]);
	}
	return a;
}

std::vector<int32_t> air_objects(const Ctx& c, int32_t who, int32_t o, int family) {
	std::vector<int32_t> v;
	const Object* ob = c.game ? c.game->object(who, o) : nullptr;
	if (!ob || !ob->alive()) return v;
	int32_t cw = ob->inside_down_who, co = ob->inside_down;
	for (int guard = 0; co >= 0 && cw >= 0 && guard < 4096; guard++) {
		const Unit* u = c.game->unit(cw, co);
		if (!u) break;
		if (u->alive() && is_air(*u)) {
			const int f = family_of(c, *ob, *u);
			if (f >= 0 && (family < 0 || family == f)) v.push_back(u->o);
		}
		cw = u->inside_down_who, co = u->inside_down;
	}
	return v;
}

void library_final(const Ctx& c, int first, const std::vector<std::pair<int, int>>& inprod, hud::Library& lib) {
	if (!c.game || c.me < 0 || c.me >= 8) return;
	const Game& g = *c.game;
	// IFaceLibrary::on_redraw: the final-techs view needs the Information Age line (has_preq(BASE_FINALTYPES)) and no rules / flag veto.
	const bool test = std::getenv("RON_FINAL_TEST") != nullptr;  // snapshots: force the view, one tech owned, one in production
	if (!test && (g.info.game_rules == 11 || (g.info.flags & 0x80000000u) || !has_preq(g, c.me, 579))) return;
	lib.final_view = true;
	const Build* fb = first >= 0 ? g.build(c.me, first) : nullptr;
	for (int k = 0; k < 4; k++) {
		hud::LibCell& cell = lib.fin[k];
		const int type = 579 + k;
		if (!g.type(type).loaded) continue;
		const Type& T = g.type(type);
		cell.type = type;
		cell.owned = has_tech(g, c.me, type);
		cell.enabled = !cell.owned && fb && build_can_make(g, *fb, type, 1) != 0 && can_pay_cost(g, type, c.me, -1, first, 1) > 0;
		cell.tex_id = to_utf8(T.tex.id), cell.tex_col = T.tex.col, cell.tex_row = T.tex.row;
		for (const auto& [qt, pct] : inprod) {
			if (qt == type) cell.percent = std::max(1, pct);
		}
		if (test) cell.owned = k == 1, cell.enabled = k != 1 && k != 2, cell.percent = k == 2 ? 55 : -1;
	}
}

// ---------------------------------------------------------------- the card

bool all_air(const Ctx& c) {
	if (!c.game || c.units->empty() || !c.builds->empty()) return false;
	for (const auto& k : *c.units) {
		const Unit* u = unit_of(c, k);
		if (!u || !u->alive() || !is_air(*u) || is_heli(*u)) return false;
	}
	return true;
}

// UnitOut::make_options @00615fc0, the air branch (type domain 2, not a helicopter): AIRSTRIKE (or PATROL for planes that cannot
// attack) at (0,0), TRANSFER at (2,0) for everything but the fighter-bomber, RETURN at (0,3) for a plane in the air; with the
// advanced menu BOMBARD at (1,0). A plane inside a base keeps the first two.
void air_unit_options(const Ctx& c, std::vector<hud::Option>& out) {
	bool own_all = true, attack = false, airborne = false, bomb = false, only_fb = true;
	for (const auto& k : *c.units) {
		const Unit* u = unit_of(c, k);
		if (!u) continue;
		own_all = own_all && k.first == c.me;
		attack = attack || u->ptype->attack > 0;
		airborne = airborne || u->on_map();
		bomb = bomb || type_can_attack_ground(*c.game, *u->ptype);
		only_fb = only_fb && unit_is(*c.game, *u, airtype::kFighterBomber);
	}
	const int pending = c.targeting ? *c.targeting : -1;
	add(out, attack ? kOptAirstrike : hud::kOptPatrol, 0, 0, own_all, pending == (attack ? kOptAirstrike : hud::kOptPatrol));
	if (c.advanced && bomb) add(out, kOptAttackGround, 1, 0, own_all, pending == kOptAttackGround);
	if (!only_fb) add(out, kOptTransfer, 2, 0, own_all, pending == kOptTransfer);
	if (airborne) add(out, kOptReturn, 0, 3, own_all);
}

Shown selection_stance(const Ctx& c) {
	Shown s;
	int cnt[8] = {};
	int kind = -1;
	for (const auto& k : *c.units) {
		const Unit* u = unit_of(c, k);
		if (!u || !u->alive() || is_air(*u) || !u->ptype) continue;
		const int st = unit_stance_type(*u->ptype);
		if (st < 0) continue;
		if (kind < 0) kind = st;
		if (st == kind && u->stance >= 0 && u->stance < 8) cnt[u->stance]++;
	}
	s.kind = kind;
	int best = 0, distinct = 0;
	for (int i = 0; i < 8; i++) {
		if (cnt[i] > 0) distinct++;
		if (cnt[i] > cnt[best]) best = i;
	}
	s.option = best, s.mixed = distinct > 1;
	return s;
}

Shown selection_form(const Ctx& c) {
	Shown s;
	int cnt[8] = {};
	for (const auto& k : *c.units) {
		const Unit* u = unit_of(c, k);
		if (!u || !u->alive() || is_air(*u)) continue;
		if (u->form >= 0 && u->form < 8) cnt[u->form]++;
		s.kind = 0;
	}
	int best = 0, distinct = 0;
	for (int i = 0; i < 8; i++) {
		if (cnt[i] > 0) distinct++;
		if (cnt[i] > cnt[best]) best = i;
	}
	s.option = best, s.mixed = distinct > 1;
	return s;
}

void unit_extra_options(const Ctx& c, std::vector<hud::Option>& out) {
	if (!c.game || c.units->empty()) return;
	if (c.units->size() == 1 && c.units->front().first == c.me) {
		// A carrier: AIRSTRIKE (1,1) and RECALL (2,3) (UnitOut::make_options; its REPEAT is a unit mask the commands do not carry).
		const Unit* u = unit_of(c, c.units->front());
		if (u && u->alive() && u->ptype && type_is(*u->ptype, airtype::kCarrier, true)) {
			const bool planes = num_aircraft_here(*c.game, *u, 0) > 0;
			const int pending = c.targeting ? *c.targeting : -1;
			add(out, kOptAirstrike, 1, 1, planes, pending == kOptAirstrike);
			add(out, kOptRecall, 2, 3, planes);
		}
	}
	if (!c.advanced) return;
	bool own_all = true, attacker = false, ground_attack = false, noncitizen = false;
	for (const auto& k : *c.units) {
		const Unit* u = unit_of(c, k);
		if (!u) continue;
		own_all = own_all && k.first == c.me;
		attacker = attacker || u->ptype->attack > 0;
		ground_attack = ground_attack || type_can_attack_ground(*c.game, *u->ptype);
		noncitizen = noncitizen || u->type != ptype::kPeasants;
	}
	if (!own_all) return;
	const int pending = c.targeting ? *c.targeting : -1;
	// "A" rows of UnitOut::make_options: GUARD (3,0), PATROL (4,0), ATTACK_GROUND (0,1), STANCE (2,2), FORM (3,2), DISBAND (4,3).
	if (noncitizen) {
		add(out, kOptGuard, 3, 0, true, pending == kOptGuard);
		add(out, hud::kOptPatrol, 4, 0, true, pending == hud::kOptPatrol);
	}
	if (ground_attack) add(out, kOptAttackGround, 0, 1, true, pending == kOptAttackGround);
	const Shown st = selection_stance(c);
	if (st.kind >= 0) {
		if (hud::Option* o = add(out, hud::kOptStance, 2, 2, true)) {
			static const char* lists[4] = {"combat", "worker", "caster", "packer"};
			o->variant_list = lists[st.kind], o->variant = st.option, o->orange = st.mixed;
		}
	}
	if (attacker) {
		if (hud::Option* o = add(out, hud::kOptForm, 3, 2, true)) {
			const Shown f = selection_form(c);
			o->variant_list = "form", o->variant = f.option, o->orange = f.mixed;
		}
	}
	add(out, hud::kOptDisband, 4, 3, true);
}

void build_extra_options(const Ctx& c, std::vector<hud::Option>& out) {
	if (!c.game || c.builds->size() != 1) return;
	const auto [who, o] = c.builds->front();
	const Build* b = c.game->build(who, o);
	if (!b || !b->alive() || !b->ptype || who != c.me || !is_build_active(*b)) return;
	const int pending = c.targeting ? *c.targeting : -1;
	// BuildOut::make_options @00629ed0: an airbase (or carrier): SCRAMBLE (0,3), REPEAT (1,3), RECALL (2,3); a missile silo with
	// a missile inside: ATTACK_GROUND (2,0). The air mission (launch patrol / flight) goes on the first free slot of row 1.
	if (type_is(*b->ptype, airtype::kAirbase, true)) {
		const bool planes = num_aircraft_here(*c.game, *b, 0) > 0;
		add(out, kOptScramble, 0, 3, planes);
		add(out, kOptRepeat, 1, 3, true, has_repeat_air(*b));
		add(out, kOptRecall, 2, 3, planes);
		for (int x = 4; x >= 0; x--) {
			if (slot_free(out, x, 1)) {
				add(out, kOptAirstrike, x, 1, planes, pending == kOptAirstrike);
				break;
			}
		}
	} else if (type_is(*b->ptype, airtype::kSilo, true) && num_inside(*c.game, *b, 1) > 0) {
		add(out, kOptAttackGround, 2, 0, true, pending == kOptAttackGround);
	}
	if (c.advanced) add(out, hud::kOptDisband, 4, 3, true);
}

// ---------------------------------------------------------------- orders

namespace {

// The message line of a targeting option (the instructions MessageWin shows): translated_strings #2983 patrol, #2984 launch patrol,
// #2988 air mission, #2989 transfer, #2991 bombard.
std::pair<int, const char*> targeting_hint(int option) {
	switch (option) {
		case hud::kOptPatrol: return {2983, "Left-click where you want to patrol."};
		case kOptGuard: return {-1, "Left-click on the unit you want to guard."};
		case kOptAttackGround: return {2991, "Left-click where you want to bombard."};
		case kOptTransfer: return {2989, "Left-click on a new airbase for these aircraft."};
		case kOptAirstrike: return {2988, "Left-click on a target or a map location."};
		case kOptAirPatrol: return {2984, "Left-click where you want to launch your patrol."};
		default: return {-1, ""};
	}
}

bool can_begin_targeting(const Ctx& c, int option) {
	if (!c.game || (c.units->empty() && c.builds->empty())) return false;
	if (option == kOptTransfer) return !c.units->empty();
	if (option == hud::kOptPatrol || option == kOptGuard) return !c.units->empty();
	return true;
}

}  // namespace

int run_option(Ctx& c, int option, int object, Mods m) {
	if (!c.game) return 0;
	const std::vector<int32_t> units = own(c, *c.units), builds = own(c, *c.builds);
	const std::vector<int32_t> objs = group_objs(c);
	switch (option) {
		case hud::kOptStance: case kOptStanceBack: {
			if (units.empty()) return 1;
			const int s = option == kOptStanceBack ? -2 : object;
			issue(c, "stance", [&] { return c.cm->issue_stance(*c.game, units, s); });
			return 1;
		}
		case hud::kOptForm: case kOptFormBack: {
			if (units.empty()) return 1;
			const int f = option == kOptFormBack ? -3 : object;
			const QueuePos q = queue_of(m);
			issue(c, "form", [&] { return c.cm->issue_form(*c.game, units, f, 0, q); });
			return 1;
		}
		case hud::kOptDisband: case kOptDisbandAll: {
			if (objs.empty()) return 1;
			issue(c, "disband", [&] { return c.cm->issue_disband(*c.game, objs, option == kOptDisbandAll); });
			return 1;
		}
		case kOptScramble:
			if (!objs.empty()) issue(c, "scramble", [&] { return c.cm->issue_scramble(*c.game, objs); });
			return 1;
		case kOptRecall: case kOptReturn:
			if (!objs.empty()) issue(c, "recall", [&] { return c.cm->issue_recall(*c.game, objs); });
			return 1;
		case kOptRepeat: {
			if (builds.empty()) return 1;  // a carrier's REPEAT is a unit mask (UNITMASK) the commands do not carry yet
			issue(c, "buildmask", [&] { return c.cm->issue_buildmask(*c.game, builds, 0x80, true); });
			return 1;
		}
		case kOptInfinite: {
			if (builds.empty()) return 1;
			issue(c, "buildmask", [&] { return c.cm->issue_buildmask(*c.game, builds, 0x40, true); });
			return 1;
		}
		case kOptQueue: {
			// OPTION_QUEUE (object < 0): the last queued item of each selected building; Shift x5, Ctrl all.
			const int idx = object >= 0 ? object : m.ctrl ? -10 : m.shift ? -5 : -1;
			for (int32_t o : builds) issue(c, "unqueue", [&] { return c.cm->issue_unqueue(*c.game, o, idx); });
			return 1;
		}
		case kOptFighters: case kOptBombers: case kOptHelis: case kOptAllAir: {
			if (c.units->size() + c.builds->size() != 1) return 1;
			const auto k = c.builds->empty() ? c.units->front() : c.builds->front();
			select_air(c, k.first, k.second, option == kOptAllAir ? -1 : option - kOptFighters);
			return 1;
		}
		case hud::kOptPatrol: case kOptGuard: case kOptAttackGround: case kOptTransfer: case kOptAirstrike: case kOptAirPatrol:
			if (can_begin_targeting(c, option)) {
				*c.targeting = option;
				const auto [idx, text] = targeting_hint(option);
				say(c, c.loc && idx >= 0 ? c.loc(idx, text) : std::string(text));
			}
			return 1;
		default: return 0;
	}
}

bool target_click(Ctx& c, int option, int32_t x, int32_t y, int tw, int to, Mods m) {
	if (!c.game) return false;
	const QueuePos q = queue_of(m);
	const std::vector<int32_t> objs = group_objs(c);
	if (objs.empty()) return false;
	const bool units = !c.units->empty();
	switch (option) {
		case hud::kOptPatrol:
			return issue(c, "patrol", [&] { return c.cm->issue_patrol(*c.game, objs, x, y, q); });
		case kOptAirPatrol:
			return issue(c, "launch_patrol", [&] { return c.cm->issue_launch_patrol(*c.game, objs, x, y, q, false, false, false); });
		case kOptAttackGround:
			return issue(c, "attack_ground", [&] { return c.cm->issue_attack_ground(*c.game, objs, x, y, q); });
		case kOptGuard:
			if (tw < 0 || to < 0 || !is_ally(*c.game, c.me, tw)) return false;
			return issue(c, "guard", [&] { return c.cm->issue_guard(*c.game, objs, to, tw, q); });
		case kOptTransfer: {
			const Object* t = tw >= 0 && to >= 0 ? c.game->object(tw, to) : nullptr;
			if (!t || !t->alive() || !is_ally(*c.game, c.me, tw) || !can_carry_air(*t)) return false;
			return issue(c, "flight", [&] { return c.cm->issue_flight(*c.game, objs, to, tw, 1, false, false, false); });
		}
		case kOptAirstrike: {
			if (tw >= 0 && to >= 0 && is_enemy(*c.game, c.me, tw))
				return issue(c, "flight", [&] { return c.cm->issue_flight(*c.game, objs, to, tw, 10, m.shift, m.ctrl, m.alt); });
			if (units && all_air(c)) return issue(c, "patrol", [&] { return c.cm->issue_patrol(*c.game, objs, x, y, q); });
			return issue(c, "launch_patrol", [&] { return c.cm->issue_launch_patrol(*c.game, objs, x, y, q, false, false, false); });
		}
		default: return false;
	}
}

bool right_click(Ctx& c, int32_t x, int32_t y, int tw, int to, Mods m) {
	if (!all_air(c) || own(c, *c.units).size() != c.units->size()) return false;
	const std::vector<int32_t> objs = own(c, *c.units);
	const QueuePos q = queue_of(m);
	if (std::getenv("RON_UIX_TRACE")) {
		std::fprintf(stderr, "[uix] right click air: target %d:%d; enemies of %d:", tw, to, c.me);
		for (int w = 0; w < 8; w++) std::fprintf(stderr, " %d%s", w, is_enemy(*c.game, c.me, w) ? "E" : is_ally(*c.game, c.me, w) ? "A" : "-");
		std::fprintf(stderr, "\n");
	}
	if (tw >= 0 && to >= 0) {
		const Object* t = c.game->object(tw, to);
		if (t && t->alive()) {
			// FLIGHT: attack an enemy object (orders 10), or rebase onto a friendly airbase / carrier (orders 1 = MOVE_TO).
			if (is_enemy(*c.game, c.me, tw))
				return issue(c, "flight", [&] { return c.cm->issue_flight(*c.game, objs, to, tw, 10, false, false, false); });
			if (is_ally(*c.game, c.me, tw) && can_carry_air(*t))
				return issue(c, "flight", [&] { return c.cm->issue_flight(*c.game, objs, to, tw, 1, false, false, false); });
		}
	}
	return issue(c, "patrol", [&] { return c.cm->issue_patrol(*c.game, objs, x, y, q); });  // the ground: a patrol (the formation move of an air group)
}

int gather_action(const Ctx& c) {
	if (!c.game || c.builds->empty()) return 0;
	for (const auto& k : *c.builds) {
		const Build* b = c.game->build(k.first, k.second);
		if (!b || !b->alive() || !b->ptype || !type_is(*b->ptype, airtype::kAirbase, true)) return 0;
	}
	return 3;
}

// ---------------------------------------------------------------- keys

int key_option(Ctx& c, const std::string& o, int param, Mods m) {
	if (!c.game) return -1;
	const bool units = !c.units->empty() && c.units->front().first == c.me;
	const bool builds = !c.builds->empty() && c.builds->front().first == c.me;
	auto go = [&](int option, int object = -1) { return run_option(c, option, object, m); };
	if (o == "OPTION_STANCE") return units ? go(hud::kOptStance, param) : 1;
	if (o == "OPTION_FORM") return units ? go(hud::kOptForm, param) : 1;
	if (o == "OPTION_PATROL") return units ? go(hud::kOptPatrol) : 1;
	if (o == "OPTION_GUARD") return units ? go(kOptGuard) : 1;
	if (o == "OPTION_ATTACK_GROUND") return units || builds ? go(kOptAttackGround) : 1;
	if (o == "OPTION_AIRSTRIKE") return units || builds ? go(kOptAirstrike) : 1;
	if (o == "OPTION_TRANSFER") return units ? go(kOptTransfer) : 1;
	if (o == "OPTION_RETURN") return units ? go(kOptReturn) : 1;
	if (o == "OPTION_RECALL") return units || builds ? go(kOptRecall) : 1;
	if (o == "OPTION_SCRAMBLE") return units || builds ? go(kOptScramble) : 1;
	if (o == "OPTION_REPEAT") return builds ? go(kOptRepeat) : 1;
	if (o == "OPTION_FIGHTERS") return go(kOptFighters);
	if (o == "OPTION_BOMBERS") return go(kOptBombers);
	if (o == "OPTION_HELIS") return go(kOptHelis);
	if (o == "OPTION_ALLAIR") return go(kOptAllAir);
	if (o == "OPTION_INFINITE") return builds ? go(kOptInfinite) : 1;
	if (o == "OPTION_QUEUE") return builds ? go(kOptQueue) : 1;
	if (o == "OPTION_DISBAND_ALL") return units || builds ? go(kOptDisbandAll) : 1;
	if (o == "OPTION_ALL_CURRENT") return select_all_current(c), 1;
	if (o == "OPTION_SELECT_DAMAGED") return select_damaged(c), 1;
	if (o == "OPTION_FILTER_DAMAGED") return filter_damaged(c), 1;
	if (o == "OPTION_FILTER_ONSCREEN") return filter_on_screen(c), 1;
	if (o == "OPTION_CYCLE_RESEARCH") {
		static int index = 15;
		return cycle_research(c, &index), 1;
	}
	if (o == "OPTION_CLEAR_GATHER") {
		if (!builds) return 1;
		const std::vector<int32_t> bs = own(c, *c.builds);
		issue(c, "gather_point", [&] { return c.cm->issue_gather_point(*c.game, bs, -1, -1, 0, false); });
		return 1;
	}
	return -1;
}

// ---------------------------------------------------------------- selection

void select_air(Ctx& c, int32_t who, int32_t o, int family) {
	// Options::select_air: the group's air units (the planes in the garrison) replace the selection.
	const std::vector<int32_t> planes = air_objects(c, who, o, family);
	if (planes.empty()) return;
	Sel units;
	for (int32_t p : planes) units.push_back({who, p});
	set_selection(c, units, {});
}

void select_object(Ctx& c, const std::vector<hud::Object>& list, int index, Mods m) {
	if (index < 0 || index >= int(list.size())) return;
	const hud::Object& ob = list[size_t(index)];
	const std::pair<int32_t, int32_t> key{ob.who, ob.o};
	Sel& mine = ob.building ? *c.builds : *c.units;
	if (m.ctrl && !m.shift) {  // Group::toggle: remove it
		auto it = std::find(mine.begin(), mine.end(), key);
		if (it != mine.end()) mine.erase(it);
		return;
	}
	if (m.shift) {  // only the objects of this type
		Sel keep;
		for (const auto& k : mine) {
			if (type_of_entry(c, k) == ob.type) keep.push_back(k);
		}
		if (ob.building) *c.builds = keep;
		else *c.units = keep;
		return;
	}
	if (ob.building) c.builds->assign(1, key), c.units->clear();
	else c.units->assign(1, key), c.builds->clear();
}

void select_type(Ctx& c, int type, Mods m) {
	auto filter = [&](Sel& s) {
		Sel keep;
		for (const auto& k : s) {
			const bool same = type_of_entry(c, k) == type;
			if (m.ctrl && !m.shift ? !same : same) keep.push_back(k);
		}
		s = keep;
	};
	filter(*c.units);
	filter(*c.builds);
}

void select_all_current(Ctx& c) {
	if (!c.game || c.me < 0 || c.me >= 8) return;
	std::map<int, bool> types;
	for (const auto& k : *c.units) {
		if (k.first == c.me) types[type_of_entry(c, k)] = false;
	}
	for (const auto& k : *c.builds) {
		if (k.first == c.me) types[type_of_entry(c, k)] = true;
	}
	if (types.empty()) return;
	Sel units, builds;
	const auto& slots = c.game->objects[size_t(c.me)];
	for (int32_t o = 0; o < slots.unit_mark && int(units.size()) < kMaxSelect; o++) {
		const Unit* u = c.game->unit(c.me, o);
		if (u && u->alive() && u->on_map() && u->ptype && types.count(u->type) && !types[u->type]) units.push_back({c.me, o});
	}
	for (int32_t o = kBuildBase; o < slots.build_mark && int(builds.size()) < kMaxSelect; o++) {
		const Build* b = c.game->build(c.me, o);
		if (b && b->alive() && is_build_active(*b) && types.count(b->type) && types[b->type]) builds.push_back({c.me, o});
	}
	if (!units.empty()) builds.clear();
	set_selection(c, units, builds);
}

void select_damaged(Ctx& c) {
	// Options::do_select_damaged: the viewer's units and buildings with damage, on the map.
	if (!c.game || c.me < 0 || c.me >= 8) return;
	Sel units;
	const auto& slots = c.game->objects[size_t(c.me)];
	for (int32_t o = 0; o < slots.unit_mark && int(units.size()) < kMaxSelect; o++) {
		const Unit* u = c.game->unit(c.me, o);
		if (u && u->alive() && u->on_map() && u->ptype && u->ptype->attack > 0 && u->damage > 0) units.push_back({c.me, o});
	}
	set_selection(c, units, {});
}

void filter_damaged(Ctx& c) {
	Sel keep;
	for (const auto& k : *c.units) {
		const Unit* u = unit_of(c, k);
		if (u && k.first == c.me && u->damage > 0) keep.push_back(k);
	}
	if (!keep.empty()) *c.units = keep;
}

void filter_on_screen(Ctx& c) {
	if (!c.on_screen) return;
	Sel units, builds;
	for (const auto& k : *c.units) {
		if (k.first == c.me && c.on_screen(k.first, k.second, false)) units.push_back(k);
	}
	for (const auto& k : *c.builds) {
		if (k.first == c.me && c.on_screen(k.first, k.second, true)) builds.push_back(k);
	}
	if (!units.empty() || !builds.empty()) set_selection(c, units, builds);
}

void box_filter(Ctx& c, Sel& list, Mods m, bool prefer) {
	if (!c.game || list.size() < 2) return;
	auto civilian = [](const Unit& u) {
		const int t = u.type;
		return (t >= ptype::kPeasants && t <= ptype::kScholarsKorean) || t == ptype::kCara || t == ptype::kMerchant || t == ptype::kMerchantDutch;
	};
	auto kind = [&](const std::pair<int32_t, int32_t>& k, int what) {  // 0 civilian, 1 attacker, 2 siege, 3 air, 4 citizen
		const Unit* u = unit_of(c, k);
		if (!u || !u->ptype) return false;
		switch (what) {
			case 0: return civilian(*u);
			case 1: return !civilian(*u) && u->ptype->attack > 0;
			case 2: return type_is_siege(*u->ptype);
			case 3: return is_air(*u);
			default: return u->type >= ptype::kPeasants && u->type <= ptype::kScholarsKorean;
		}
	};
	auto keep_if = [&](const std::function<bool(const std::pair<int32_t, int32_t>&)>& pred) {
		Sel keep;
		for (const auto& k : list) {
			if (pred(k)) keep.push_back(k);
		}
		if (!keep.empty()) list = keep;  // a filter that would leave nothing is not applied
	};
	if (m.alt) {
		if (m.ctrl) keep_if([&](const auto& k) { return kind(k, 2); });
		else if (m.shift) keep_if([&](const auto& k) { return kind(k, 3); });
		else keep_if([&](const auto& k) { return kind(k, 1) || kind(k, 2); });
		return;
	}
	if (m.ctrl && m.shift) return keep_if([&](const auto& k) { return kind(k, 4); });
	const bool military_first = m.ctrl ? !prefer : prefer;
	if (!military_first) return;
	bool any = false;
	for (const auto& k : list) any = any || !kind(k, 0);
	if (any) keep_if([&](const auto& k) { return !kind(k, 0); });
}

void cycle_research(Ctx& c, int* index) {
	// Options::do_cycle_research: the research buildings in turn (library, university, temple, ... airbase); the next one of the
	// first type that has any is selected and centred.
	static const int list[16] = {435, 438, 437, 436, 420, 423, 424, 425, 443, 439, 427, 428, 430, 432, 520, 447};
	if (!c.game || c.me < 0 || c.me >= 8) return;
	const auto& slots = c.game->objects[size_t(c.me)];
	for (int tries = 0; tries < 16; tries++) {
		*index = (*index + 1) & 15;
		std::vector<int32_t> found;
		for (int32_t o = kBuildBase; o < slots.build_mark; o++) {
			const Build* b = c.game->build(c.me, o);
			if (b && b->alive() && is_build_active(*b) && b->type == list[*index]) found.push_back(o);
		}
		if (found.empty()) continue;
		int32_t pick = found.front();
		if (!c.builds->empty()) {
			for (size_t i = 0; i < found.size(); i++) {
				if (found[i] == c.builds->front().second) pick = found[(i + 1) % found.size()];
			}
		}
		c.units->clear();
		c.builds->assign(1, {c.me, pick});
		if (const Build* b = c.game->build(c.me, pick)) {
			if (c.look_at) c.look_at(float(b->x), float(b->y));
		}
		return;
	}
}

std::vector<std::pair<int32_t, int32_t>> formation_slots(Ctx& c, int32_t x, int32_t y, int form, int width, bool set_angle, uint32_t angle) {
	std::vector<std::pair<int32_t, int32_t>> out;
	if (!c.game || form < 0 || form >= kNumForms) return out;
	Game& g = *c.game;
	auto tmp = std::make_unique<Group>();
	group_clear(*tmp, -1);
	std::vector<int32_t> members;
	for (const auto& k : *c.units) {
		const Unit* u = unit_of(c, k);
		if (!u || k.first != c.me || !u->alive() || u->type == ptype::kPeasants || is_air(*u)) continue;  // citizens and aircraft carry no formation
		if (group_add(g, *tmp, k.second, k.first, 0, 0)) members.push_back(k.second);
	}
	if (tmp->num <= 0 || group_get_num_cap(g, *tmp) <= 0) return out;
	int32_t lx = 0, ly = 0;
	group_get_loc(g, *tmp, &lx, &ly);
	const FormData saved = g.forms[size_t(form)];
	uint32_t a = angle;
	const FormData* F = group_compute_form(g, *tmp, x, y, form, width, set_angle, &a, lx, ly, 0);
	if (F) {
		for (int32_t i = 0; i < tmp->num; i++) out.push_back({F->to_x[size_t(i)], F->to_y[size_t(i)]});
	}
	g.forms[size_t(form)] = saved;
	return out;
}

const char* help_key(int option) {
	switch (option) {
		case hud::kOptPatrol: return "PATROL";
		case kOptGuard: return "GUARD";
		case kOptAttackGround: return "ATTACK_GROUND";
		case kOptTransfer: return "TRANSFER";
		case kOptAirstrike: return "AIRSTRIKE";
		case kOptScramble: return "SCRAMBLE";
		case kOptRepeat: return "REPEAT";
		case kOptRecall: return "RECALL";
		case kOptReturn: return "RETURN";
		case hud::kOptStance: return "STANCE";
		case hud::kOptForm: return "FORMATION";
		default: return "";
	}
}

}  // namespace uix
