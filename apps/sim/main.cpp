// ron-sim: headless simulation harness (engine/sim). Loads a scenario, applies scripted commands at given frames,
// runs N frames and prints a deterministic per-frame digest.
//
// Usage: ron-sim <game dir> <scenario.scx> [--frames N] [--seed S] [--list]
//                [--move WHO:o1,o2,...@X,Y[@FRAME][/form][%queue]]... [--halt WHO:o1,...@FRAME]...
//                [--form WHO:o1,...@FRAME/form[,rotate]]... [--attack WHO:o1,...@TWHO:TO[@FRAME][%queue]]...
//                [--dump-every K] [--digest-every K] [--combat-every K] [--quiet] [--no-selfcheck] [--anims] [--no-art]
//                [--reveal N] [--fog WHO] [--fog-check]
//                [--build WHO:o1,..@TYPE@X,Y[@FRAME][%queue]]... [--queue WHO:b1,..@TYPE[*N][@FRAME]]...
//                [--research ...(= --queue)] [--rally WHO:b1,..@X,Y[@FRAME][/action]]... [--swarm WHO:o1,..@TWHO:TO[@FRAME][/order]]...
//                [--disband WHO:o1,..[@FRAME]]... [--unqueue WHO:b@INDEX[@FRAME]]... [--prod-every K]
//                [--gather WHO:o1,..@B[@FRAME][%queue]]... [--city-gather WHO:c1,..@TYPE[@FRAME]]...
//                [--buy|--sell WHO:GOOD[*FLAGS][@FRAME]]... [--tribute WHO:WHOM@GOOD@AMOUNT[@FRAME]]...
//                [--respond WHO:WHOM@1|0[@FRAME]]... [--peasants WHO:N[@FRAME]]... [--econ-every K] [--ai-every K]
//                [--starting-resources N] [--difficulty N] [--no-scripts] [--script-log]
//                [--select WHO:o1,..@FRAME]... [--option N[:MENU[:TYPE]]@FRAME]...
//                [--garrison WHO:o1,..@BO:BWHO[@FRAME][%queue]]... [--eject-all WHO:o1,..[@FRAME][/back_to_work]]...
//        ron-sim --dump circle|ring|stamps
// Scripts: --script-log prints the scenario builtins' presentation queue as it fills (messages, sounds, objectives,
// camera, pings; sim/script_ui.hpp) and the run-time messages; --select / --option set the seat-local input a renderer
// would report (the local selection of leader WHO, the armed command OptionIndex) from FRAME on; --option's MENU opens
// the build options panel on that page (0 civic, 1 military, 2 wonders; -1 closed) and TYPE (a TypeIndex number) is
// the armed option's object (build_option_selected).
// Economy (docs/economy-spec.md): --gather issues GROUP + GATHER (the workers go to building B, or the nearest own
// gather building to it); --city-gather GROUP + CITY_GATHER (TYPE -1: farm / woodcutter / mine); --buy / --sell lots
// of 100 (FLAGS 1 x5, 4 x10, 2 all); --tribute TRIBUTE (AMOUNT < 0: DEMAND_TRIBUTE); --respond ACCEPT (1) / REJECT
// (0); --peasants LEADER_OPTIONS with the auto-gather delay N; --econ-every K prints per leader the stockpile, rates,
// caps and market prices, and every gather building with its gatherers; --starting-resources sets the GameInfo
// setting (0 Low .. 12 Random) before loading; --difficulty the GameInfo difficulty (0 easiest .. 5; default 2) the
// computer opponents use outside scripted scenarios. --ai-every K prints each AI leader's state, armies, score and
// diplomacy.
// Garrisons (docs/garrison-spec.md): --garrison issues GROUP + GARRISON (the units go into building BO of leader BWHO);
// --eject-all GROUP + EJECTALL (the listed buildings let their garrison out; /1 = back_to_work: citizens only, orders
// kept). --list prints `inside N` on a building (and on a unit) with a garrison.
// --attack issues GROUP + ATTACK (target object TO of leader TWHO). --combat-every prints a combat summary (alive
// units and hit points per leader, live ammo, corpses, kills) every K frames.
// --move issues GROUP + MOVE_TO for the listed units of leader WHO at FRAME (default 0); /form sets the formation
// byte (default -1 = keep), %queue the QueuePos (0 FIRST, 1 LAST, 2 NEW = default). --form issues FORM (form -1 next,
// -3 previous, -2 keep; rotate is added to the formation angle) with QUEUE_NEW. --dump-every prints the moved units'
// positions every K frames. The run is
// repeated from a fresh load and the digests compared (the determinism self-check) unless --no-selfcheck.
// Animation data (guy clocks) comes from the install's unit_graphics.xml / anim_graphics.xml / .bha files; --no-art
// runs without it (3-frame animations). --anims prints, after loading, the frames per animation of every graphics
// piece the scenario's units use.
// Fog of war: --reveal sets GameInfo.reveal_map (default: the scenario header's, else 1 = Normal); --fog WHO prints the
// fog map of that seat at the end; --fog-check verifies at every refresh (frame % 100 == 33) that the stale trail of
// moving units is cleared and stationary areas survive. --dump prints the circle / ring tables or every stamp range
// in the format of `node re/tools/los-ref.mjs dump circle|ring|stamps` (diff them).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "assets/scenario.hpp"
#include "sim/anim_assets.hpp"
#include "sim/combat.hpp"
#include "sim/commands.hpp"
#include "sim/orders.hpp"
#include "sim/ai.hpp"
#include "sim/production.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/guys.hpp"
#include "sim/rules.hpp"
#include "sim/script_host.hpp"
#include "sim/placement.hpp"
#include "sim/view.hpp"

namespace {

using namespace ron::sim;

struct Script {
	enum Kind { Move, Halt, Form, Attack, Build, Queue, Rally, Swarm, Disband, Unqueue, Gather, CityGather, Buy, Sell,
		        Tribute, Respond, Peasants, Select, Option, Garrison, EjectAll } kind = Move;
	int32_t who = 0;
	std::vector<int32_t> objs;
	int32_t x = 0, y = 0, frame = 0, form = -1, queue = 2, rotate = 0;
	int32_t twho = -1, to = -1;
	std::string type;  // production: a type name (TYPENAME / NAME, case-insensitive) or a TypeIndex number
	int32_t num = 1, action = 0, order = 6;
};

std::vector<int32_t> parse_list(const std::string& s) {
	std::vector<int32_t> v;
	std::stringstream ss(s);
	std::string item;
	while (std::getline(ss, item, ',')) {
		if (!item.empty()) v.push_back(int32_t(std::strtol(item.c_str(), nullptr, 0)));
	}
	return v;
}

// WHO:o1,o2@X,Y[@FRAME][/form]
Script parse_move(const std::string& a) {
	Script s;
	std::string t = a;
	const size_t pct = t.find('%');
	if (pct != std::string::npos) {
		s.queue = int32_t(std::strtol(t.c_str() + pct + 1, nullptr, 0));
		t = t.substr(0, pct);
	}
	const size_t slash = t.find('/');
	if (slash != std::string::npos) {
		s.form = int32_t(std::strtol(t.c_str() + slash + 1, nullptr, 0));
		t = t.substr(0, slash);
	}
	const size_t colon = t.find(':');
	const size_t at1 = t.find('@');
	if (colon == std::string::npos || at1 == std::string::npos || at1 < colon) throw std::runtime_error("bad --move " + a);
	s.who = int32_t(std::strtol(t.substr(0, colon).c_str(), nullptr, 0));
	s.objs = parse_list(t.substr(colon + 1, at1 - colon - 1));
	const std::string rest = t.substr(at1 + 1);
	const size_t at2 = rest.find('@');
	const std::string xy = rest.substr(0, at2);
	const size_t comma = xy.find(',');
	if (comma == std::string::npos) throw std::runtime_error("bad --move target " + a);
	s.x = int32_t(std::strtol(xy.substr(0, comma).c_str(), nullptr, 0));
	s.y = int32_t(std::strtol(xy.substr(comma + 1).c_str(), nullptr, 0));
	if (at2 != std::string::npos) s.frame = int32_t(std::strtol(rest.c_str() + at2 + 1, nullptr, 0));
	return s;
}

// WHO:o1,o2@FRAME/form[,rotate]
Script parse_form(const std::string& a) {
	Script s;
	s.kind = Script::Form;
	const size_t colon = a.find(':'), at = a.find('@'), slash = a.find('/');
	if (colon == std::string::npos || at == std::string::npos || slash == std::string::npos || slash < at)
		throw std::runtime_error("bad --form " + a);
	s.who = int32_t(std::strtol(a.substr(0, colon).c_str(), nullptr, 0));
	s.objs = parse_list(a.substr(colon + 1, at - colon - 1));
	s.frame = int32_t(std::strtol(a.substr(at + 1, slash - at - 1).c_str(), nullptr, 0));
	const std::string fr = a.substr(slash + 1);
	const size_t comma = fr.find(',');
	s.form = int32_t(std::strtol(fr.substr(0, comma).c_str(), nullptr, 0));
	if (comma != std::string::npos) s.rotate = int32_t(std::strtoul(fr.c_str() + comma + 1, nullptr, 0));
	return s;
}

// WHO:o1,o2@FRAME
Script parse_halt(const std::string& a) {
	Script s;
	s.kind = Script::Halt;
	const size_t colon = a.find(':');
	const size_t at = a.find('@');
	if (colon == std::string::npos || at == std::string::npos) throw std::runtime_error("bad --halt " + a);
	s.who = int32_t(std::strtol(a.substr(0, colon).c_str(), nullptr, 0));
	s.objs = parse_list(a.substr(colon + 1, at - colon - 1));
	s.frame = int32_t(std::strtol(a.c_str() + at + 1, nullptr, 0));
	return s;
}

// WHO:o1,o2@TWHO:TO[@FRAME][%queue]
Script parse_attack(const std::string& a) {
	Script s;
	s.kind = Script::Attack;
	std::string t = a;
	const size_t pct = t.find('%');
	if (pct != std::string::npos) {
		s.queue = int32_t(std::strtol(t.c_str() + pct + 1, nullptr, 0));
		t = t.substr(0, pct);
	}
	const size_t colon = t.find(':'), at = t.find('@');
	if (colon == std::string::npos || at == std::string::npos || at < colon) throw std::runtime_error("bad --attack " + a);
	s.who = int32_t(std::strtol(t.substr(0, colon).c_str(), nullptr, 0));
	s.objs = parse_list(t.substr(colon + 1, at - colon - 1));
	const std::string rest = t.substr(at + 1);
	const size_t c2 = rest.find(':');
	if (c2 == std::string::npos) throw std::runtime_error("bad --attack target " + a);
	s.twho = int32_t(std::strtol(rest.substr(0, c2).c_str(), nullptr, 0));
	const size_t at2 = rest.find('@');
	s.to = int32_t(std::strtol(rest.substr(c2 + 1, at2 == std::string::npos ? std::string::npos : at2 - c2 - 1).c_str(), nullptr, 0));
	if (at2 != std::string::npos) s.frame = int32_t(std::strtol(rest.c_str() + at2 + 1, nullptr, 0));
	return s;
}

// WHO:o1,o2@BO:BWHO[@FRAME][%queue]
Script parse_garrison(const std::string& a) {
	Script s;
	s.kind = Script::Garrison;
	std::string t = a;
	const size_t pct = t.find('%');
	if (pct != std::string::npos) {
		s.queue = int32_t(std::strtol(t.c_str() + pct + 1, nullptr, 0));
		t = t.substr(0, pct);
	}
	const size_t colon = t.find(':'), at = t.find('@');
	if (colon == std::string::npos || at == std::string::npos || at < colon) throw std::runtime_error("bad --garrison " + a);
	s.who = int32_t(std::strtol(t.substr(0, colon).c_str(), nullptr, 0));
	s.objs = parse_list(t.substr(colon + 1, at - colon - 1));
	const std::string rest = t.substr(at + 1);
	const size_t c2 = rest.find(':');
	if (c2 == std::string::npos) throw std::runtime_error("bad --garrison building " + a);
	s.to = int32_t(std::strtol(rest.substr(0, c2).c_str(), nullptr, 0));
	const size_t at2 = rest.find('@');
	s.twho = int32_t(std::strtol(rest.substr(c2 + 1, at2 == std::string::npos ? std::string::npos : at2 - c2 - 1).c_str(), nullptr, 0));
	if (at2 != std::string::npos) s.frame = int32_t(std::strtol(rest.c_str() + at2 + 1, nullptr, 0));
	return s;
}

// WHO:o1,o2[@FRAME][/back_to_work]
Script parse_eject_all(const std::string& a) {
	Script s;
	s.kind = Script::EjectAll;
	s.num = 0;  // back_to_work
	std::string t = a;
	const size_t slash = t.find('/');
	if (slash != std::string::npos) {
		s.num = int32_t(std::strtol(t.c_str() + slash + 1, nullptr, 0));
		t = t.substr(0, slash);
	}
	const size_t colon = t.find(':'), at = t.find('@');
	if (colon == std::string::npos) throw std::runtime_error("bad --eject-all " + a);
	s.who = int32_t(std::strtol(t.substr(0, colon).c_str(), nullptr, 0));
	s.objs = parse_list(t.substr(colon + 1, at == std::string::npos ? std::string::npos : at - colon - 1));
	if (at != std::string::npos) s.frame = int32_t(std::strtol(t.c_str() + at + 1, nullptr, 0));
	return s;
}

// Production scripts (docs/production-spec.md section 9).
// --build WHO:o1,o2@TYPE@X,Y[@FRAME][%queue]   citizens place and build a site
// --queue WHO:b1,b2@TYPE[*N][@FRAME]           buildings train / research / upgrade (QUEUE_UP)
// --rally WHO:b1,b2@X,Y[@FRAME][/action]       rally point (GATHER_POINT; X < 0 clears)
// --swarm WHO:o1@TWHO:TO[@FRAME][/order]       SWARM_AROUND: build (6) or repair (13) a building
// --disband WHO:o1[@FRAME]                     DISBAND (units die, sites are cancelled, buildings demolish)
// --unqueue WHO:b@INDEX[@FRAME]                UNQUEUE (index -1 = last)
std::vector<std::string> split(const std::string& s, char c) {
	std::vector<std::string> v;
	std::stringstream ss(s);
	std::string item;
	while (std::getline(ss, item, c)) v.push_back(item);
	return v;
}
Script parse_prod(Script::Kind kind, const std::string& a) {
	Script s;
	s.kind = kind;
	std::string t = a;
	const size_t pct = t.find('%');
	if (pct != std::string::npos) {
		s.queue = int32_t(std::strtol(t.c_str() + pct + 1, nullptr, 0));
		t = t.substr(0, pct);
	}
	const size_t slash = t.find('/');
	if (slash != std::string::npos) {
		const int32_t v = int32_t(std::strtol(t.c_str() + slash + 1, nullptr, 0));
		if (kind == Script::Swarm) s.order = v;
		else s.action = v;
		t = t.substr(0, slash);
	}
	const size_t colon = t.find(':');
	if (colon == std::string::npos) throw std::runtime_error("bad production option " + a);
	s.who = int32_t(std::strtol(t.substr(0, colon).c_str(), nullptr, 0));
	const std::vector<std::string> parts = split(t.substr(colon + 1), '@');
	if (parts.empty()) throw std::runtime_error("bad production option " + a);
	s.objs = parse_list(parts[0]);
	auto xy = [&](const std::string& p) {
		const size_t comma = p.find(',');
		if (comma == std::string::npos) throw std::runtime_error("bad X,Y in " + a);
		s.x = int32_t(std::strtol(p.substr(0, comma).c_str(), nullptr, 0));
		s.y = int32_t(std::strtol(p.c_str() + comma + 1, nullptr, 0));
	};
	size_t next = 1;
	switch (kind) {
	case Script::Build:
		if (parts.size() < 3) throw std::runtime_error("bad --build " + a);
		s.type = parts[1];
		xy(parts[2]);
		next = 3;
		break;
	case Script::Queue: {
		if (parts.size() < 2) throw std::runtime_error("bad --queue " + a);
		std::string ty = parts[1];
		const size_t star = ty.find('*');
		if (star != std::string::npos) {
			s.num = int32_t(std::strtol(ty.c_str() + star + 1, nullptr, 0));
			ty = ty.substr(0, star);
		}
		s.type = ty;
		next = 2;
		break;
	}
	case Script::Rally:
		if (parts.size() < 2) throw std::runtime_error("bad --rally " + a);
		xy(parts[1]);
		next = 2;
		break;
	case Script::Swarm: {
		if (parts.size() < 2) throw std::runtime_error("bad --swarm " + a);
		const size_t c2 = parts[1].find(':');
		if (c2 == std::string::npos) throw std::runtime_error("bad --swarm target " + a);
		s.twho = int32_t(std::strtol(parts[1].substr(0, c2).c_str(), nullptr, 0));
		s.to = int32_t(std::strtol(parts[1].c_str() + c2 + 1, nullptr, 0));
		next = 2;
		break;
	}
	case Script::Unqueue:
		if (parts.size() < 2) throw std::runtime_error("bad --unqueue " + a);
		s.num = int32_t(std::strtol(parts[1].c_str(), nullptr, 0));
		next = 2;
		break;
	case Script::Gather:
		if (parts.size() < 2) throw std::runtime_error("bad --gather " + a);
		s.to = int32_t(std::strtol(parts[1].c_str(), nullptr, 0));
		next = 2;
		break;
	case Script::CityGather:
		if (parts.size() < 2) throw std::runtime_error("bad --city-gather " + a);
		s.type = parts[1];
		next = 2;
		break;
	case Script::Tribute:
		// WHOM in objs[0]; GOOD@AMOUNT
		if (parts.size() < 3) throw std::runtime_error("bad --tribute " + a);
		s.num = int32_t(std::strtol(parts[1].c_str(), nullptr, 0));
		s.x = int32_t(std::strtol(parts[2].c_str(), nullptr, 0));
		next = 3;
		break;
	case Script::Respond:
		if (parts.size() < 2) throw std::runtime_error("bad --respond " + a);
		s.num = int32_t(std::strtol(parts[1].c_str(), nullptr, 0));
		next = 2;
		break;
	case Script::Buy: case Script::Sell: case Script::Peasants: {
		// GOOD[*FLAGS] (or N for --peasants) in objs[0] / num
		std::string v = t.substr(colon + 1);
		const size_t at = v.find('@');
		if (at != std::string::npos) {
			s.frame = int32_t(std::strtol(v.c_str() + at + 1, nullptr, 0));
			v = v.substr(0, at);
		}
		const size_t star = v.find('*');
		s.num = 0;
		if (star != std::string::npos) {
			s.num = int32_t(std::strtol(v.c_str() + star + 1, nullptr, 0));
			v = v.substr(0, star);
		}
		s.x = int32_t(std::strtol(v.c_str(), nullptr, 0));
		return s;
	}
	default:
		break;
	}
	if (parts.size() > next) s.frame = int32_t(std::strtol(parts[next].c_str(), nullptr, 0));
	return s;
}

// A type by name (TYPENAME, else NAME; case-insensitive, first match) or by TypeIndex number; -1 if unknown.
int32_t find_type(const Game& g, const std::string& n) {
	if (!n.empty() && (std::isdigit(static_cast<unsigned char>(n[0])) || n[0] == '-')) return int32_t(std::strtol(n.c_str(), nullptr, 0));
	auto low = [](std::string v) {
		for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
		return v;
	};
	const std::string want = low(n);
	for (int32_t pass = 0; pass < 2; pass++) {
		for (size_t i = 0; i < g.rules->types.size(); i++) {
			const Type& t = g.rules->types[i];
			if (!t.loaded) continue;
			const ustr& nm = pass == 0 ? (t.type_name.empty() ? t.name : t.type_name) : t.name;
			if (low(to_utf8(nm)) == want) return int32_t(i);
		}
	}
	return -1;
}

// FNV-1a 64 over the simulation state the movement core owns.
struct Fnv {
	uint64_t h = 0xcbf29ce484222325ull;
	void add(uint32_t v) {
		for (int i = 0; i < 4; i++) {
			h ^= (v >> (8 * i)) & 0xff;
			h *= 0x100000001b3ull;
		}
	}
	void add(int32_t v) { add(uint32_t(v)); }
	void add_bytes(const std::vector<uint8_t>& v) {
		for (uint8_t b : v) {
			h ^= b;
			h *= 0x100000001b3ull;
		}
	}
};

uint64_t digest(const Game& g) {
	Fnv f;
	f.add(uint32_t(g.frame));
	f.add(g.rng.seed());
	for (int32_t who = 0; who < kNumLeaders; who++) {
		const Leader& ld = g.leaders[size_t(who)];
		f.add(ld.production_ai), f.add(ld.prod_script_on), f.add(ld.prod_step);
		// AI state (ai-spec): personality, sites, make list, census summary, trainer list, armies.
		{
			const int32_t* pp = &ld.pers.rush;
			for (int32_t i = 0; i < 24; i++) f.add(uint32_t(pp[i]));
			for (const Site& st : ld.sites) f.add(uint32_t(st.wx)), f.add(uint32_t(st.wy)), f.add(uint32_t(st.val)), f.add(uint32_t(st.rank));
			for (const MakeObject& m : ld.make_list) f.add(uint32_t(m.t)), f.add(uint32_t(m.val)), f.add(uint32_t(m.num));
			f.add(ld.site_mark), f.add(uint32_t(ld.effective_pop)), f.add(uint32_t(ld.free_peasants)), f.add(uint32_t(ld.gatherers));
			for (int32_t e : ld.econ) f.add(uint32_t(e));
			for (int32_t o : ld.mil_trainers) f.add(uint32_t(o));
			if (who < 8) {
				for (const Army& A : g.armies[size_t(who)]) {
					f.add(uint32_t(A.valid));
					if (A.valid) f.add(uint32_t(A.status)), f.add(uint32_t(A.city)), f.add(uint32_t(A.target_o)), f.add(uint32_t(A.num_units));
				}
			}
		}
		const ObjectList& L = g.objects[size_t(who)];
		f.add(L.unit_mark);
		f.add(uint32_t(L.obj_ctr));
		for (int32_t o = 0; o < L.unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (!u) continue;
			f.add(uint32_t(u->flags));
			if (!u->alive()) {
				f.add(uint32_t(u->hold_frames));
				continue;
			}
			f.add(u->x), f.add(u->y), f.add(u->z), f.add(u->angle), f.add(u->dest_angle);
			f.add(int32_t(u->mylos)), f.add(int32_t(u->visible)), f.add(int32_t(u->infiltrated));
			f.add(u->unit_masks), f.add(int32_t(u->group)), f.add(int32_t(u->collide)), f.add(int32_t(u->idle));
			f.add(u->damage), f.add(int32_t(u->damage_frac)), f.add(int32_t(u->recharging)), f.add(u->unit_masks2);
			f.add(int32_t(u->o_up)), f.add(int32_t(u->o_down)), f.add(int32_t(u->targeted)), f.add(int32_t(u->cavarch_o));
			f.add(int32_t(u->inside_up)), f.add(int32_t(u->inside_down));  // garrisons (docs/garrison-spec.md)
			f.add(int32_t(u->near_o)), f.add(int32_t(u->damage_frame));
			f.add(uint32_t(u->orders.size()));
			for (const Order& o2 : u->orders) {
				f.add(int32_t(o2.type)), f.add(uint32_t(o2.flags)), f.add(o2.x), f.add(o2.y), f.add(o2.dest);
				f.add(o2.dest_x), f.add(o2.dest_y), f.add(o2.pause), f.add(o2.retry), f.add(o2.attempts);
				f.add(o2.ox), f.add(o2.whom), f.add(uint32_t(o2.uid)), f.add(int32_t(o2.mandatory));
			}
			f.add(uint32_t(u->path.size()));
			for (const PathData& p : u->path) f.add(p.x), f.add(p.y), f.add(p.tol), f.add(p.flags);
			for (const Guy& gy : u->guys) {
				f.add(gy.x), f.add(gy.y), f.add(gy.z), f.add(gy.angle), f.add(int32_t(gy.cur_anim));
				f.add(gy.cur_time), f.add(gy.end_time), f.add(gy.avg_speed);
			}
		}
	}
	for (const Group& gr : g.groups.g) {
		if (gr.num == 0) continue;
		f.add(gr.id), f.add(gr.num), f.add(gr.form), f.add(gr.speed), f.add(gr.order_num);
		for (int32_t i = 0; i < gr.num; i++) f.add(int32_t(gr.list[size_t(i)])), f.add(gr.off_x[size_t(i)]), f.add(gr.off_y[size_t(i)]);
	}
	for (int32_t r : g.daemon.repaths) f.add(r);
	f.add(g.market.tick);
	for (int32_t i = 0; i < 6; i++) {
		f.add(g.market.level[size_t(i)]), f.add(g.market.flux[size_t(i)]), f.add(g.market.next_flux[size_t(i)]);
		f.add(g.market.delta_flux[size_t(i)]), f.add(g.market.flux_length[size_t(i)]);
	}
	f.add(g.daemon.busy);
	// Fog of war (docs/fog-spec.md): buildings' memory, leaders' vision state, goods / items, the five grids.
	for (int32_t who = 0; who < kNumLeaders; who++) {
		const ObjectList& L = g.objects[size_t(who)];
		f.add(L.build_mark);
		for (const auto& b : L.builds) {
			if (!b) continue;
			f.add(uint32_t(b->flags));
			if (!b->alive()) continue;
			f.add(int32_t(b->mylos)), f.add(int32_t(b->visible)), f.add(int32_t(b->infiltrated));
			f.add(uint32_t(b->ever_seen)), f.add(uint32_t(b->ever_seen_completed));
		}
		const Leader& l = g.leaders[size_t(who)];
		f.add(l.flags), f.add(uint32_t(l.ally_mask)), f.add(l.explored);
		for (int32_t d : l.diplo) f.add(d);
		f.add(uint32_t(l.oil_patches.size()));
		for (int32_t i : l.oil_patches) f.add(i);
		f.add(uint32_t(l.new_rares.size()));
		for (int32_t i : l.new_rares) f.add(i);
	}
	for (const GoodObj& gd : g.goods) f.add(uint32_t(gd.flags) | uint32_t(gd.ever_seen) << 8);
	for (const GoodObj& it : g.items) f.add(uint32_t(it.flags) | uint32_t(it.ever_seen) << 8);
	f.add_bytes(g.world.seen);
	f.add_bytes(g.world.seen2);
	f.add_bytes(g.world.seen3);
	f.add_bytes(g.world.wcoord_seen);
	for (const WData& d : g.world.wdata) f.add(uint32_t(d.was_seen) | uint32_t(uint16_t(d.down)) << 8);
	// Combat: buildings, ammo, corpses, leader statistics.
	for (int32_t who = 0; who < kNumLeaders; who++) {
		const ObjectList& L = g.objects[size_t(who)];
		f.add(L.build_mark);
		for (int32_t o = kBuildBase; o < L.build_mark; o++) {
			const Object* b = g.object(who, o);
			if (!b) continue;
			f.add(uint32_t(b->flags)), f.add(b->damage), f.add(uint32_t(b->hold_frames));
			const Build* bb = static_cast<const Build*>(b);
			f.add(int32_t(bb->attack_ox)), f.add(int32_t(bb->recharging));
			f.add(int32_t(bb->inside_down)), f.add(bb->build_masks);  // garrisons: the first occupant, EJECTING
		}
		const Leader& l = g.leaders[size_t(who)];
		f.add(l.score_combat), f.add(l.units_killed), f.add(l.units_lost), f.add(l.retarget_budget);
	}
	f.add(uint32_t(g.ammo.size())), f.add(g.ammo_counter);
	for (const Ammo& a : g.ammo) {
		if (!a.in_use()) continue;
		f.add(uint32_t(a.flags)), f.add(a.ex), f.add(a.ey), f.add(a.ez), f.add(a.cur_time), f.add(a.total_time);
		f.add(a.ox), f.add(a.whom);
	}
	for (const DeathObj& d : g.deaths) {
		if (!d.valid) continue;
		f.add(d.who), f.add(d.o), f.add(d.x), f.add(d.y), f.add(d.anim), f.add(d.cur_frame);
	}
	// Production: stockpiles, population, tech bits, buildings' construction / queues / rally points, cities, grid.
	for (int32_t who = 0; who < kNumLeaders; who++) {
		const Leader& l = g.leaders[size_t(who)];
		for (int32_t k = 0; k < 6; k++) f.add(l.bucket[size_t(k)]), f.add(l.escrow[size_t(k)]);
		f.add(l.age), f.add(l.epochs), f.add(l.discovered), f.add(l.pop), f.add(l.pop_cap), f.add(l.control);
		f.add(l.active), f.add(l.caras), f.add(l.pop_issues), f.add(l.city_num), f.add(l.city_mark), f.add(l.flags2);
		f.add(l.buildings_built), f.add(l.cities_built), f.add(l.wonders_built), f.add(l.defense), f.add(l.units_built);
		f.add(l.gov), f.add(l.tech_frame), f.add(l.peasants), f.add(l.combat_units), f.add(l.combat_queued);
		f.add_bytes(std::vector<uint8_t>(l.tech.begin(), l.tech.end()));
		for (int32_t t = 0; t < kNumTypes; t++)
			if (l.num_queued[size_t(t)]) f.add(t), f.add(int32_t(l.num_queued[size_t(t)]));
		for (size_t t = 0; t < l.num_buildings.size(); t++)
			if (l.num_buildings[t] || l.high_buildings[t]) f.add(int32_t(t)), f.add(int32_t(l.num_buildings[t])), f.add(int32_t(l.high_buildings[t]));
		for (size_t t = 0; t < l.num_units.size(); t++)
			if (l.num_units[t]) f.add(int32_t(t)), f.add(int32_t(l.num_units[t]));
		const ObjectList& L = g.objects[size_t(who)];
		for (int32_t o = kBuildBase; o < L.build_mark; o++) {
			const Build* b = g.build(who, o);
			if (!b || !b->alive()) continue;
			f.add(b->type), f.add(b->x), f.add(b->y), f.add(b->job_counter), f.add(b->job_counter_2);
			f.add(int32_t(b->helpers)), f.add(uint32_t(b->build_masks)), f.add(int32_t(b->queued)), f.add(b->myhits);
			f.add(b->construct_hits), f.add(int32_t(b->city)), f.add(int32_t(b->gather_max)), f.add(int32_t(b->stance));
			for (int32_t i = 0; i < b->queued && i < b->build_queue.queue_size; i++)
				f.add(int32_t(b->build_queue.queue[size_t(i)].type)), f.add(b->build_queue.queue[size_t(i)].job_counter);
			for (const GatherPoint& gp : b->gather) f.add(gp.x), f.add(gp.y), f.add(int32_t(gp.action));
			f.add(uint32_t(b->gather_from.size()));
			for (const auto& t : b->gather_from) f.add(t[0]), f.add(t[1]);
		}
		if (who < 8) {
			for (const City& c : g.cities[size_t(who)]) {
				f.add(uint32_t(c.city_flags)), f.add(int32_t(c.o)), f.add(int32_t(c.pop)), f.add(int32_t(c.race));
				f.add(int32_t(c.founder)), f.add(int32_t(c.was_capital_flags)), f.add(c.capture_stamp), f.add(c.capture_strength);
				f.add(c.assimilation_timer), f.add(c.reduce_stamp), f.add(int32_t(c.plundered));
			}
		}
		// Capture / defeat / victory (docs/capture-spec.md).
		f.add(l.flags), f.add(l.cities_captured), f.add(l.cities_lost), f.add(l.defeated_by), f.add(l.defeat_stamp);
		f.add(l.lost_capital_stamp), f.add(l.lost_capital_timer), f.add(l.lost_capital_modifier);
		f.add(l.popwin_stamp), f.add(l.popwin_timer), f.add(l.wonderwin_stamp), f.add(l.wonderwin_timer);
	}
	f.add(uint32_t(g.game_over)), f.add(g.musical_chairs);
	// Scenario script state (ScenarioData, sim/scenario_data.hpp; the script timers and find counters of ScriptHost).
	{
		const ScenarioData& sd = g.scen;
		for (int32_t w = 0; w < 8; w++) {
			f.add(uint32_t(sd.goals[size_t(w)].size()));
			for (const auto& gl : sd.goals[size_t(w)]) f.add(uint32_t(gl.completed)), f.add(uint32_t(gl.id.size()));
			for (int16_t v : sd.units_killed[size_t(w)]) f.add(uint32_t(uint16_t(v)));
			for (int16_t v : sd.builds_destroyed[size_t(w)]) f.add(uint32_t(uint16_t(v)));
			f.add(sd.last_razed[size_t(w)]), f.add(uint32_t(sd.city_lost_to[size_t(w)])), f.add(sd.pop_cap[size_t(w)]);
			f.add(sd.last_unit_built_o[size_t(w)]);
		}
		f.add(uint32_t(sd.city_lost)), f.add(uint32_t(sd.groups.size()));
		for (const auto& gr : sd.groups) {
			f.add(uint32_t(gr.who));
			for (int32_t o : gr.ids) f.add(uint32_t(o));
		}
		for (int32_t o : sd.involved_objects) f.add(uint32_t(o));
		f.add(uint32_t(sd.units_free | (sd.buildings_free << 1) | (sd.techs_free << 2)));
		f.add(uint32_t(g.buildings_gather | (g.building_unit_bonus << 1) | (g.building_resource_bonus << 2)));
		if (const ScriptHost* h = g.scripts.get()) {
			for (int32_t c : h->find_counters) f.add(uint32_t(c));
			for (const auto& t : h->timers.list) f.add(uint32_t(t.expiry)), f.add(uint32_t(t.name.size()));
		}
	}
	// Economy: the leaders' LeaderDataEncrypt fields and savings, units' and buildings' gather state, farms, caravans.
	for (int32_t who = 0; who < kNumLeaders; who++) {
		const Leader& l = g.leaders[size_t(who)];
		for (int32_t k = 0; k < 6; k++) {
			f.add(l.leftover[size_t(k)]), f.add(l.over_cap[size_t(k)]), f.add(l.resources[size_t(k)]);
			f.add(l.income[size_t(k)]), f.add(l.escrow_rate[size_t(k)]), f.add(l.collected[size_t(k)]);
			f.add(l.tributes[size_t(k)]);
		}
		for (int32_t c : l.resource_cap) f.add(c);
		f.add(l.gather_stamp), f.add(l.known_rares), f.add(l.tribute_sent), f.add(l.tribute_received);
		for (uint8_t b : l.rare_owned) f.add(uint32_t(b));
		for (uint8_t b : l.rare) f.add(uint32_t(b));
		for (int32_t r : l.rares_collected) f.add(r);
		for (const Leader::Deal& d : l.dip) {
			f.add(d.agree), f.add(d.any_offer), f.add(d.treaty);
			for (int32_t k = 0; k < 6; k++) f.add(d.offers[size_t(k)]), f.add(d.dows[size_t(k)]);
		}
		f.add(uint32_t(l.oil_wells.size()));
		for (int16_t o : l.oil_wells) f.add(int32_t(o));
		const ObjectList& L = g.objects[size_t(who)];
		for (int32_t o = 0; o < L.unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (!u || !u->alive()) continue;
			f.add(int32_t(u->gather_down)), f.add(int32_t(u->good_obj)), f.add(u->rare), f.add(int32_t(u->doober));
			f.add(int32_t(u->cara)), f.add(int32_t(u->animal_ox));
			for (const Order& o2 : u->orders) {
				if (o2.type == kGather)
					f.add(o2.g_tx), f.add(o2.g_ty), f.add(o2.wait), f.add(int32_t(o2.goto_build)), f.add(int32_t(o2.dist_mod)),
					    f.add(int32_t(o2.been_there));
				else if (o2.type == kTradeRoute)
					f.add(o2.oxx), f.add(o2.whose), f.add(o2.started), f.add(o2.loaded);
			}
		}
		for (int32_t o = kBuildBase; o < L.build_mark; o++) {
			const Build* b = g.build(who, o);
			if (!b || !b->alive()) continue;
			f.add(int32_t(b->gather_down)), f.add(int32_t(b->fort));
		}
		if (who < 8) {
			for (const City& c : g.cities[size_t(who)]) {
				f.add(int32_t(c.trade_val)), f.add(uint32_t(c.granary) | uint32_t(c.lumber_mill) << 8 | uint32_t(c.smelter) << 16);
				for (int32_t t : c.traded_with) f.add(t);
				f.add(uint32_t(c.vans.size()));
			}
			for (const CaravanRec& r : g.caravans[size_t(who)])
				f.add(int32_t(r.city2)), f.add(int32_t(r.city3)), f.add(uint32_t(r.caravan_flags)), f.add(int32_t(r.o));
		}
	}
	f.add(uint32_t(g.farms.size()));
	for (const FarmRec& fr : g.farms) {
		f.add(fr.who), f.add(fr.o), f.add(uint32_t(fr.valid) | uint32_t(fr.farm_type) << 8);
		for (int32_t x = 0; x < 4; x++)
			for (int32_t y = 0; y < 4; y++) {
				float p = fr.percent[size_t(x)][size_t(y)];
				uint32_t bits;
				std::memcpy(&bits, &p, 4);
				f.add(bits), f.add(uint32_t(fr.status[size_t(x)][size_t(y)]));
			}
	}
	f.add(uint32_t(g.hold_doobers.size()));
	for (uint16_t m : g.world.tdata) f.add(uint32_t(m));
	// Unit occupancy bits.
	for (size_t i = 0; i < g.world.wdata.size(); i++) {
		const int32_t b = g.world.wdata[i].block;
		if (b < 0) continue;
		f.add(uint32_t(i));
		const CollBlock& cb = g.world.coll_blocks[size_t(b)];
		for (size_t k = 0; k < 32; k++) f.add(uint32_t(cb.bytes[k]));
	}
	return f.h;
}

// ---------------------------------------------------------------- fog of war checks (docs/fog-spec.md)

struct FogOpts {
	int32_t fog_who = -1;    // --fog WHO: ASCII fog map for that seat at the end of the run
	bool fog_check = false;  // --fog-check: refresh invariants at every frame % 100 == 33
	int32_t reveal = -1;     // --reveal N (default: the scenario header's value, else 1)
	int32_t econ_every = 0;  // --econ-every K (economy summary)
	int32_t ai_every = 0;    // --ai-every K (AI summary)
	int32_t starting_resources = -1;  // --starting-resources N (GameInfo setting before loading)
	int32_t difficulty = -1;          // --difficulty N (GameInfo difficulty, the lobby setting; 0 easiest .. 5)
	bool scripts = true;              // --no-scripts: no scenario / general powers / AI production scripts
	bool script_log = false;          // --script-log: script run-time messages and prints
	std::string scenario_path;        // the scenario file (the scenario script is <dir>/<base>.bhs)
};

// The presentation queue of the scenario builtins (sim/script_ui.hpp), printed as it fills (--script-log).
const char* script_event_name(ScriptEvent::Kind k) {
	switch (k) {
	case ScriptEvent::kGameMessage: return "game message";
	case ScriptEvent::kScenarioMessage: return "message";
	case ScriptEvent::kInstruction: return "instruction";
	case ScriptEvent::kFeedback: return "feedback";
	case ScriptEvent::kChat: return "chat";
	case ScriptEvent::kNotice: return "notice";
	case ScriptEvent::kBubble: return "bubble";
	case ScriptEvent::kPopup: return "popup";
	case ScriptEvent::kSound: return "sound";
	case ScriptEvent::kStopSound: return "stop sound";
	case ScriptEvent::kMusicMood: return "music mood";
	case ScriptEvent::kPing: return "ping";
	case ScriptEvent::kClearPings: return "clear pings";
	case ScriptEvent::kCamera: return "camera";
	case ScriptEvent::kClearMessages: return "clear messages";
	case ScriptEvent::kGoalDialog: return "goal dialog";
	case ScriptEvent::kScreenshot: return "screenshot";
	case ScriptEvent::kPause: return "pause";
	case ScriptEvent::kVictoryScreen: return "results";
	}
	return "?";
}
void print_script_events(const Game& g, uint64_t& seen, size_t& objectives_seen,
                         std::vector<std::pair<std::string, int32_t>>& done_seen) {
	const ScriptPresentation* p = script_presentation(g);
	if (!p) return;
	for (const ScriptEvent& e : p->events) {
		if (e.seq <= seen) continue;
		seen = e.seq;
		std::printf("  f%-5d SCRIPT %s", e.frame, script_event_name(e.kind));
		if (!e.text.empty()) std::printf(" \"%s\"", e.text.c_str());
		if (!e.text2.empty()) std::printf(" [%s]", e.text2.c_str());
		if (e.kind == ScriptEvent::kSound) std::printf(" %d ms", e.a);
		if (e.kind == ScriptEvent::kPing || e.kind == ScriptEvent::kCamera || e.kind == ScriptEvent::kBubble)
			std::printf(" at (%d, %d) who %d", e.x, e.y, e.who);
		if (e.kind == ScriptEvent::kChat) std::printf(" from L%d", e.who);
		std::printf("\n");
	}
	for (size_t k = objectives_seen; k < p->objectives.size(); k++) {
		const ScriptObjective& o = p->objectives[k];
		std::printf("  f%-5d SCRIPT objective added '%s': \"%s\"%s\n", o.frame, o.id.c_str(), o.text.c_str(),
		            o.announce ? "" : " (text only)");
	}
	objectives_seen = p->objectives.size();
	for (const ScriptObjective& o : p->objectives) {
		if (!o.completed) continue;
		bool known = false;
		for (const auto& d : done_seen) known |= d.first == o.id && d.second == o.frame;
		if (known) continue;
		done_seen.push_back({o.id, o.frame});
		std::printf("  f%-5d SCRIPT objective complete '%s'\n", o.completed, o.id.c_str());
	}
}

// Script summary at the end of a run: what was compiled, runs, failures, unported builtins.
void print_scripts(const Game& g, const ScriptLoadReport& rep, bool log) {
	const ScriptHost* h = g.scripts.get();
	if (!h) return;
	std::printf("scripts: scenario '%s' general powers '%s' production", rep.scenario_script.c_str(),
	            rep.general_powers_script.c_str());
	for (const std::string& p : rep.production_scripts) std::printf(" %s", p.c_str());
	std::printf("; %lld runs, %lld failed\n", (long long)h->runs, (long long)h->run_errors);
	for (const std::string& e : rep.errors) std::printf("  script load: %s\n", e.c_str());
	for (const auto& f : h->run_failures) std::printf("  script failures: %s x%d\n", f.first.c_str(), f.second);
	const auto un = h->unported();
	if (!un.empty()) {
		std::printf("  unported builtins called:");
		for (const auto& u : un) std::printf(" %s x%d", u.first.c_str(), u.second);
		std::printf("\n");
	}
	for (const auto& t : h->todo_calls) std::printf("  unported game functions: %s x%d\n", t.first.c_str(), t.second);
	for (int32_t who = 0; who < 8; who++) {
		const Leader& l = g.leaders[size_t(who)];
		if ((l.flags & leaderflag::kValid) && !l.prod_script.empty())
			std::printf("  L%d production script %s on %d step %d state %d\n", who, l.prod_script.c_str(), l.prod_script_on,
			            l.prod_step, l.production_ai);
	}
	if (log) {
		for (const std::string& m : h->messages) std::printf("  script: %s\n", m.c_str());
	}
}

// The fog cells inside the full stamp of every object of `who` (as update_all_seen stamps them). A small-LOS ground
// unit (projected centre) contributes the box of half-size r + 1 around its own cell, a superset of its disc for any
// facing. `exact` marks cells whose stamp does not depend on the facing.
std::vector<uint8_t> fog_coverage(const Game& g, int32_t who, std::vector<uint8_t>* exact) {
	const World& w = g.world;
	std::vector<uint8_t> cov(size_t(w.fog_size), 0);
	if (exact) exact->assign(size_t(w.fog_size), 0);
	auto mark = [&](int32_t fx, int32_t fy, bool ex) {
		if (fx < 0 || fy < 0 || fx >= w.fog_xs || fy >= w.fog_ys) return;
		cov[size_t(fy) * size_t(w.fog_xs) + size_t(fx)] = 1;
		if (ex && exact) (*exact)[size_t(fy) * size_t(w.fog_xs) + size_t(fx)] = 1;
	};
	const Circles& C = circles();
	auto stamp = [&](const Object& ob) {
		const int32_t los = object_los(g, ob);
		if (los <= 0) return;
		const StampRange r = stamp_range(los, false, false, false);
		const int32_t cx = fog_cell(ob.x), cy = fog_cell(ob.y);
		bool projected = false;
		if (r.r < 4 && ob.is_unit) {
			const Unit& u = static_cast<const Unit&>(ob);
			projected = u.ptype->domain == 0 && !(u.ptype->unit_flags2 & 4) && !(u.unit_masks & umask::kDecoy);
		}
		if (projected) {
			for (int32_t dy = -r.r - 1; dy <= r.r + 1; dy++) {
				for (int32_t dx = -r.r - 1; dx <= r.r + 1; dx++) mark(cx + dx, cy + dy, false);
			}
			return;
		}
		for (int32_t k = r.start; k < r.end; k++) mark(cx + C.x[size_t(k)], cy + C.y[size_t(k)], true);
	};
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		const Object* b = g.object(who, o);
		if (b && b->alive() && (b->flags & objflag::kActive)) stamp(*b);
	}
	for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
		const Unit* u = g.unit(who, o);
		if (u && u->alive() && u->on_map()) stamp(*u);
	}
	// Units revealed to `who` by attacking (ObjectData.visible bit who, set by Unit::set_attacking): the circle of
	// radius type.x_size around their fog cell (Unit::update_local_seen writes those cells into seen).
	for (int32_t p = 0; p < kNumLeaders; p++) {
		if (p == who || !(g.leaders[size_t(p)].flags & leaderflag::kValid)) continue;
		for (int32_t o = 0; o < g.objects[size_t(p)].unit_mark; o++) {
			const Unit* u = g.unit(p, o);
			if (!u || !u->alive() || !u->on_map() || !(u->visible & (1 << who))) continue;
			const int32_t xs = std::clamp(u->ptype->x_size, 0, 64);
			const int32_t cx = fog_cell(u->x), cy = fog_cell(u->y);
			for (int32_t k = 0; k < C.radius[size_t(xs)]; k++) mark(cx + C.x[size_t(k)], cy + C.y[size_t(k)], false);
		}
	}
	// Started wonders (not cities, not forts) of every leader are currently visible to everyone: footprint plus a
	// one-tile border (Wall::update_local_seen with mask 0xff).
	for (int32_t p = 0; p < 8; p++) {
		if (!(g.leaders[size_t(p)].flags & leaderflag::kValid)) continue;
		for (int32_t o = kBuildBase; o < g.objects[size_t(p)].build_mark; o++) {
			const Object* b = g.object(p, o);
			if (!b || !b->alive() || !(b->flags & objflag::kStarted) || (b->flags & objflag::kCity)) continue;
			if (b->type < fogtype::kWonderFirst || b->type > fogtype::kWonderLast || type_is(*b->ptype, fogtype::kFort)) continue;
			const Footprint f = footprint(*b->ptype, b->x, b->y);
			for (int32_t tx = f.x0 - 1; tx <= f.x0 + b->ptype->x_size; tx++) {
				for (int32_t ty = f.y0 - 1; ty <= f.y0 + b->ptype->y_size; ty++) {
					if (w.in_t(tx, ty)) mark(tx >> 1, ty >> 1, true);
				}
			}
		}
	}
	return cov;
}

struct FogCheck {
	std::vector<uint8_t> before;  // seen at the end of the frame before the refresh
	std::array<std::vector<uint8_t>, 8> cov_before;  // per player: coverage at the positions the refresh stamps
	int64_t cleared = 0, violations = 0, refreshes = 0, lost = 0;
};

// Called after every frame. At frame % 100 == 33 (the next frame refreshes) the seen grid and each player's coverage
// (the refresh stamps those positions) are saved; one frame later every saved own-bit cell that no own object covered
// then or covers now must be gone (a moving unit's trail clears at the refresh), and every cell of the
// facing-independent stamp of an own building or unit must be seen (stationary areas survive the refresh; a unit
// that moved in the refresh frame is covered by its refresh stamp plus its rim update, which checks the ring tables).
void fog_check_frame(const Game& g, FogCheck& fc, bool print) {
	const World& w = g.world;
	if (g.info.reveal_map == 3) return;  // All Visible: no refresh at all (update_all_seen returns at once)
	if (g.frame % 100 == 33) {
		fc.before = w.seen;
		for (int32_t who = 0; who < 8; who++) fc.cov_before[size_t(who)] = fog_coverage(g, who, nullptr);
		return;
	}
	if (g.frame % 100 != 34 || fc.before.empty()) return;
	fc.refreshes++;
	int64_t cleared = 0, violations = 0, lost = 0;
	const Circles& C = circles();
	for (int32_t who = 0; who < 8; who++) {
		if (!(g.leaders[size_t(who)].flags & leaderflag::kValid)) continue;
		const std::vector<uint8_t> cov = fog_coverage(g, who, nullptr);
		const uint8_t bit = uint8_t(1u << who);
		for (size_t i = 0; i < cov.size(); i++) {
			const bool was = (fc.before[i] & bit) != 0, now = (w.seen[i] & bit) != 0;
			if (!was || cov[i] || fc.cov_before[size_t(who)][i]) continue;
			if (now) violations++;
			else cleared++;
		}
		// Survival: buildings always, units whose stamp ignores the facing.
		auto check_disc = [&](const Object& ob) {
			const int32_t los = object_los(g, ob);
			if (los <= 0) return;
			const StampRange r = stamp_range(los, false, false, false);
			if (r.r < 4 && ob.is_unit) return;  // possibly projected (facing-dependent)
			const int32_t cx = fog_cell(ob.x), cy = fog_cell(ob.y);
			for (int32_t k = r.start; k < r.end; k++) {
				const int32_t fx = cx + C.x[size_t(k)], fy = cy + C.y[size_t(k)];
				if (fx < 0 || fy < 0 || fx >= w.fog_xs || fy >= w.fog_ys) continue;
				if (!(w.seen[size_t(fy) * size_t(w.fog_xs) + size_t(fx)] & bit)) lost++;
			}
		};
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			const Object* b = g.object(who, o);
			if (b && b->alive() && (b->flags & objflag::kActive)) check_disc(*b);
		}
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (u && u->alive() && u->on_map()) check_disc(*u);
		}
	}
	fc.cleared += cleared;
	fc.violations += violations;
	fc.lost += lost;
	if (print)
		std::printf("fog refresh at frame %d: %lld stale cells cleared, %lld stale cells kept (violations), %lld covered "
		            "cells missing\n",
		            g.frame - 1, (long long)cleared, (long long)violations, (long long)lost);
}

void print_fog_map(const Game& g, int32_t who) {
	const FogView fv = make_fog_view(g, who);
	std::vector<char> own(fv.level.size(), 0);
	for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
		const Unit* u = g.unit(who, o);
		if (!u || !u->alive() || !u->on_map()) continue;
		const int32_t fx = fog_cell(u->x), fy = fog_cell(u->y);
		if (fx >= 0 && fy >= 0 && fx < fv.fog_xs && fy < fv.fog_ys) own[size_t(fy) * size_t(fv.fog_xs) + size_t(fx)] = '@';
	}
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		const Object* b = g.object(who, o);
		if (!b || !b->alive()) continue;
		const int32_t fx = fog_cell(b->x), fy = fog_cell(b->y);
		if (fx >= 0 && fy >= 0 && fx < fv.fog_xs && fy < fv.fog_ys) own[size_t(fy) * size_t(fv.fog_xs) + size_t(fx)] = 'B';
	}
	int64_t n[4] = {0, 0, 0, 0};
	for (uint8_t l : fv.level) n[l & 3]++;
	std::printf("\nfog of war for leader %d at frame %d (%dx%d fog cells of 384 units; ally_mask %02x, reveal_map %d): "
	            "'o' visible %lld, '.' seen before %lld, '-' explored only %lld, '#' unexplored %lld; "
	            "@ own unit, B own building\n",
	            who, g.frame, fv.fog_xs, fv.fog_ys, unsigned(g.leaders[size_t(who)].ally_mask), int(g.info.reveal_map),
	            (long long)n[3], (long long)n[2], (long long)n[1], (long long)n[0]);
	const Leader& L = g.leaders[size_t(who)];
	int32_t goods_seen = 0, items_seen = 0, builds_met = 0, builds_other = 0;
	for (const GoodObj& gd : g.goods) goods_seen += (gd.ever_seen >> who) & 1;
	for (const GoodObj& it : g.items) items_seen += (it.ever_seen >> who) & 1;
	for (int32_t p = 0; p < 8; p++) {
		if (p == who) continue;
		for (const auto& b : g.objects[size_t(p)].builds) {
			if (!b || !b->alive()) continue;
			builds_other++;
			builds_met += object_is_seen(g, *b, who, false) ? 1 : 0;
		}
	}
	std::printf("leader %d: explored samples %d, oil patches %zu, new rares %zu, goods discovered %d/%zu, goody boxes "
	            "%d/%zu, other players' buildings seen %d/%d, treaties",
	            who, L.explored, L.oil_patches.size(), L.new_rares.size(), goods_seen, g.goods.size(), items_seen,
	            g.items.size(), builds_met, builds_other);
	for (int32_t p = 0; p < 8; p++) std::printf(" %d", L.treaty(p));
	std::printf("\n");
	static const char kCh[4] = {'#', '-', '.', 'o'};
	std::string line;
	for (int32_t fy = 0; fy < fv.fog_ys; fy++) {
		line.clear();
		for (int32_t fx = 0; fx < fv.fog_xs; fx++) {
			const size_t i = size_t(fy) * size_t(fv.fog_xs) + size_t(fx);
			line += own[i] ? own[i] : kCh[fv.level[i] & 3];
		}
		std::printf("%s\n", line.c_str());
	}
}

// --dump circle|ring|stamps: the tables and stamp ranges, in re/tools/los-ref.mjs `dump` format, for diffing.
int dump_tables(const char* what) {
	const Circles& C = circles();
	const Rings& R = rings();
	if (std::strcmp(what, "circle") == 0 || std::strcmp(what, "ring") == 0) {
		const bool ring = what[0] == 'r';
		const std::vector<int8_t>& X = ring ? R.x : C.x;
		const std::vector<int8_t>& Y = ring ? R.y : C.y;
		for (size_t i = 0; i < X.size(); i++) std::printf("%zu %d %d\n", i, int(X[i]), int(Y[i]));
		return 0;
	}
	if (std::strcmp(what, "stamps") == 0) {
		for (int32_t los = 0; los <= 140; los++) {
			for (int m = 0; m < 2; m++) {
				for (int p = 0; p < 2; p++) {
					for (int d = 0; d < 2; d++) {
						const StampRange s = stamp_range(los, m != 0, p != 0, d != 0);
						std::printf("los %d move %d project %d detector %d: r %d %s [%d, %d) detect < %d:", los, m, p, d,
						            s.r, s.ring ? "ring" : "circle", s.start, s.end, s.detect_end);
						const std::vector<int8_t>& X = s.ring ? R.x : C.x;
						const std::vector<int8_t>& Y = s.ring ? R.y : C.y;
						for (int32_t k = s.start; k < s.end; k++)
							std::printf(" %d,%d%s", int(X[size_t(k)]), int(Y[size_t(k)]), k < s.detect_end ? "D" : "");
						std::printf("\n");
					}
				}
			}
		}
		return 0;
	}
	std::fprintf(stderr, "--dump circle|ring|stamps\n");
	return 2;
}

std::string type_name(const Game& g, int32_t t) {
	const Type& ty = g.type(t);
	return to_utf8(ty.type_name.empty() ? ty.name : ty.type_name);
}

struct Tracked {
	int32_t who, o;
	int32_t sx, sy;      // start position
	int32_t tx, ty;      // commanded target
	int32_t arrive = -1; // first frame after the command with no orders left
	int32_t cmd_frame = 0;
	int32_t max_step = 0;  // largest per-frame displacement (sanity: no teleports)
	int32_t px, py;
	int32_t frames_moving = 0;
};

struct RunResult {
	std::vector<uint64_t> digests;
	std::vector<Tracked> tracked;
	std::vector<std::array<int32_t, 2>> watched;  // buildings (who, o) named by --garrison / --eject-all
	int32_t final_frame = 0;
	bool fog_failed = false;
	std::array<uint16_t, kNumLeaders> initial_uids{};  // obj_ctr after loading (objects created later have higher uids)
};

// The animation table of --anims: per graphics piece used by a unit (guy 0 and crew), the frames of every action
// with an animmgr entry ('*' = looping), as the guy clock sees them.
void print_anim_table(const Game& g, const AssetAnimSource& art) {
	static const char* kNames[38] = {"DEFAULT", "IDLE1", "IDLE2", "IDLE3", "GIDLE1", "GIDLE2", "GIDLE3", "SLOG",
	                                 "WALK", "JOG", "ATKWALK", "ATTACK1", "ATTACK2", "ATTACK3", "ATKSPEC", "D_STAB1",
	                                 "D_STAB2", "D_SHOT1", "D_SHOT2", "D_SPL1", "D_SPL2", "TURN_L", "TURN_R", "PACK",
	                                 "UNPACK", "CHOP", "W_WOOD", "DUMPWOOD", "WTO_WOOD", "MINE", "W_ORE", "DUMPORE",
	                                 "WTO_ORE", "BUILD", "REPAIR", "SOW", "REAP", "FARM"};
	std::vector<int32_t> seen;
	std::printf("animation frames per graphics piece (frames at 15/s; * = looping, ! = .bha missing):\n");
	for (int32_t who = 0; who < kNumLeaders; who++) {
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (!u || !u->alive()) continue;
			for (const Guy& gy : u->guys) {
				if (gy.gpiece < 0 || std::find(seen.begin(), seen.end(), gy.gpiece) != seen.end()) continue;
				seen.push_back(gy.gpiece);
				std::printf("  %-28s piece %5d  (%s, guy %d, track %d,%d)\n   ", art.piece_name(gy.gpiece).c_str(),
				            gy.gpiece, type_name(g, u->type).c_str(), int(gy.guy_num), gy.track_dx, gy.track_dy);
				int col = 0;
				for (int32_t a = 0; a < 38; a++) {
					if (art.anim_entry_name(gy.gpiece, a).empty()) continue;
					if (col > 0 && col % 8 == 0) std::printf("\n   ");
					col++;
					std::printf(" %s=%d%s%s", kNames[a], art.frames(gy.gpiece, a), art.loops(gy.gpiece, a) ? "*" : "",
					            art.has_anim(gy.gpiece, a) ? "" : "!");
				}
				std::printf("\n");
			}
		}
	}
	std::printf("  gaia animal pieces (o = 0):");
	for (int32_t t = 0x192; t < 0x19e; t++) {
		const int32_t p = unit_gpiece(g, t, kGaiaWho, 0, 0, false);
		std::printf(" %s%s", art.piece_name(p).c_str(), art.piece_exists(p) ? "" : "(missing)");
	}
	std::printf("\n");
}

// Production summary (--prod-every K, and --list): per leader the stockpile and population, per building the
// construction progress, the queue with progress, and the rally points.
void print_production(const Game& g, bool full) {
	static const char* kGood[6] = {"food", "timber", "wealth", "knowledge", "metal", "oil"};
	for (int32_t who = 0; who < 8; who++) {
		const LeaderView lv = leader_view(g, who);
		if (!lv.valid) continue;
		std::printf("  f%-5d L%d age %d pop %d/%d (control) cities %d/%d |", g.frame, who, lv.age, lv.control, lv.pop_cap,
		            lv.cities, lv.city_limit);
		for (int32_t k = 0; k < 6; k++) std::printf(" %s %d", kGood[k], lv.stockpile[k]);
		std::printf("\n");
	}
	const GameView v = make_view(g, false);
	for (const BuildView& b : v.builds) {
		if (b.who >= 8) continue;
		const Build* sb0 = g.build(b.who, b.o);
		const bool busy = !b.active || !b.queue.empty() || !b.rally.empty() || (sb0 && sb0->inside_down >= 0);
		if (!full && !busy) continue;
		std::printf("    %d:%-4d %-20s (%6d,%6d) %s", b.who, b.o, type_name(g, b.type).c_str(), b.x, b.y,
		            b.active ? "done" : b.started ? "building" : "site");
		if (!b.active) std::printf(" %d/%d (%d%%)", b.job_counter, b.construct_time,
		                           b.construct_time ? int32_t(int64_t(b.job_counter) * 100 / b.construct_time) : 0);
		std::printf(" hp %d/%d", b.hit_points, b.max_hit_points);
		if (b.city_index >= 0) std::printf(" city %d", b.city_index);
		for (size_t i = 0; i < b.queue.size(); i++)
			std::printf("%s %s %d/%d", i == 0 ? " | queue:" : ",", type_name(g, b.queue[i].type).c_str(), b.queue[i].job_counter,
			            b.queue[i].time);
		if (b.repeat) std::printf(" (repeat)");
		for (size_t i = 0; i < b.rally.size(); i++) std::printf("%s (%d,%d)/%d", i == 0 ? " | rally" : " ->", b.rally[i].x, b.rally[i].y, b.rally[i].action);
		if (const Build* sb = g.build(b.who, b.o)) {
			if (sb->inside_down >= 0)
				std::printf(" | inside %d (pop %d) arrows %d", num_inside(g, *sb, 1), num_inside(g, *sb, 0),
				            sb->ptype->attack != 0 ? get_garrison_arrows(g, *sb) : 0);
		}
		std::printf("\n");
	}
}

// AI summary (--ai-every K): per AI leader (not a human seat) the production state, age, population, stockpile,
// census (citizens free / gathering, combat units), buildings (finished / sites), techs held, cities, armies and the
// make list's first item.
void print_ai(const Game& g) {
	for (int32_t who = 0; who < 8; who++) {
		const Leader& L = g.leaders[size_t(who)];
		if (!(L.flags & leaderflag::kValid) || (L.flags & 0xc) == leaderflag::kHuman) continue;
		int32_t done = 0, sites = 0, units = 0, techs = 0, armies = 0, army_units = 0;
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			const Build* b = g.build(who, o);
			if (!b || !b->alive()) continue;
			if (b->flags & objflag::kActive) done++;
			else sites++;
		}
		int32_t mil = 0, mil_in_army = 0;
		std::vector<char> in_army(size_t(std::max(0, g.objects[size_t(who)].unit_mark)), 0);
		for (const Army& A : g.armies[size_t(who)]) {
			if (!A.valid) continue;
			for (int32_t i = 0; i < A.num_groups; i++) {
				const int32_t id = A.list[size_t(i)];
				if (id < 0) continue;
				const Group& gr = g.groups.g[size_t(id)];
				for (int32_t m = 0; m < gr.num; m++)
					if (gr.list[size_t(m)] >= 0 && size_t(gr.list[size_t(m)]) < in_army.size()) in_army[size_t(gr.list[size_t(m)])] = 1;
			}
		}
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (!u || !u->alive()) continue;
			units++;
			if (u->ptype->attack != 0 && u->type >= 70 && u->ptype->domain == 0) {  // land combat units (no citizens)
				mil++;
				if (in_army[size_t(o)]) mil_in_army++;
			}
		}
		for (int32_t t = kAges; t < 629; t++) techs += has_tech(g, who, t) ? 1 : 0;
		for (const Army& A : g.armies[size_t(who)]) {
			if (!A.valid) continue;
			armies++;
			army_units += A.num_units;
		}
		std::printf("  ai f%-5d L%d st %2d age %d pop %d/%d cities %d units %d (cit %d free %d gath %d, combat %d) "
		            "bldg %d+%d sites techs %d armies %d/%d | f %d t %d w %d k %d m %d o %d | make %s %d\n",
		            g.frame, who, L.production_ai, L.age, L.control, L.pop_cap, L.city_num, units, L.peasants,
		            L.free_peasants, L.gatherers, L.combat, done, sites, techs, armies, army_units, L.bucket[0], L.bucket[1],
		            L.bucket[2], L.bucket[3], L.bucket[4], L.bucket[5],
		            L.make_list[0].t >= 0 ? type_name(g, L.make_list[0].t).c_str() : "-", L.make_list[0].val);
		for (int32_t k = 0; k < 16; k++) {
			const Army& A = g.armies[size_t(who)][size_t(k)];
			if (!A.valid || A.num_units == 0) continue;
			std::string tgt;
			if (A.target_who >= 0 && A.target_o >= 0) {
				const Object* T = g.object(A.target_who, A.target_o);
				if (T && T->alive())
					tgt = " (" + type_name(g, T->type) + " damage " + std::to_string(T->damage) + "/" +
					      std::to_string(T->ptype->hits) + ")";
				else
					tgt = " (gone)";
			}
			int32_t attacking = 0;
			for (int32_t i = 0; i < A.num_groups; i++) {
				const int32_t id = A.list[size_t(i)];
				if (id < 0) continue;
				const Group& gr = g.groups.g[size_t(id)];
				for (int32_t m = 0; m < gr.num; m++) {
					const Unit* u = g.unit(who, gr.list[size_t(m)]);
					if (u && u->alive() && u->front() && u->front()->type == kAttack) attacking++;
				}
			}
			std::printf("      army %d status %#x units %d (captains %d standard %d, attacking %d) groups %d city %d reg %d "
			            "target %d:%d%s at (%d,%d) muster (%d,%d)%s\n",
			            k, A.status, A.num_units, A.num_captains, A.num_standard, attacking, A.num_groups, A.city, A.reg,
			            A.target_who, A.target_o, tgt.c_str(), A.x, A.y, A.muster_x, A.muster_y, A.navy ? " navy" : "");
		}
		std::printf("      combat: kills %d losses %d razed %d lost %d; land military units %d, in armies %d\n", L.units_killed,
		            L.units_lost, L.buildings_razed, L.buildings_lost, mil, mil_in_army);
		std::printf("      flags %#x diff %d rush %d raid %d early_army %d pop_cap %d rush_rules %d war_allowed %d epochs %d/%d/%d/%d\n",
		            L.flags, get_diff(g, who), L.pers.rush, L.pers.raid, L.pers.early_army, L.pop_cap, g.info.rush_rules,
		            war_allowed(g) ? 1 : 0, L.epoch[0], L.epoch[1], L.epoch[2], L.epoch[3]);
		std::printf("      victory clocks: wonder %d/%d territory %d/%d (team terr %d of land %d) lost capital %d/%d mod %d; "
		            "cities captured %d lost %d\n",
		            L.wonderwin_timer, L.wonderwin_stamp, L.popwin_timer, L.popwin_stamp, get_team_terr(g, who),
		            g.world_total_land, L.lost_capital_timer, L.lost_capital_stamp, L.lost_capital_modifier, L.cities_captured,
		            L.cities_lost);
		std::printf("      score %d (units %d bldg %d econ %d terr %d research %d upgrades %d wonders %d combat %d) wars %d\n",
		            L.score, L.score_units, L.score_buildings, L.score_economy, L.score_territory, L.score_research,
		            L.score_upgrades, L.score_wonders, L.score_combat, L.wars);
		std::printf("      diplo:");
		for (int32_t j = 0; j < 8; j++) {
			if (j == who || !(g.leaders[size_t(j)].flags & leaderflag::kValid)) continue;
			static const char* kD[3] = {"war", "peace", "ally"};
			const int32_t d = L.diplos(j);
			std::printf(" L%d %s met %d agenda %#x aggr %d strong %d weak %d;", j, d >= 0 && d < 3 ? kD[d] : "?",
			            L.treaty(j) & 1, L.agendas[size_t(j)], L.aggression[size_t(j)], L.strong[size_t(j)], L.weak[size_t(j)]);
		}
		std::printf("\n");
	}
}

// Economy summary (--econ-every K): per leader the stockpile, the gather rate and income (resources per 450 frames:
// rate / 16), caps, market prices; every gather building with gatherers (registered / arrived / max); the workers
// with a GATHER action and their state.
void print_economy(const Game& g) {
	static const char* kGood[6] = {"food", "timber", "wealth", "knowl", "metal", "oil"};
	for (int32_t who = 0; who < 8; who++) {
		const LeaderView lv = leader_view(g, who);
		if (!lv.valid) continue;
		std::printf("  f%-5d L%d econ:", g.frame, who);
		for (int32_t k = 0; k < 6; k++)
			std::printf(" %s %d (+%d/%d%s, %d/%d)", kGood[k], lv.stockpile[k], lv.income[k], lv.cap[k],
			            lv.over_cap[k] ? " capped" : "", lv.buy[k], lv.sell[k]);
		std::printf("%s\n", lv.can_trade ? " [market]" : "");
	}
	const GameView v = make_view(g, false);
	for (const BuildView& b : v.builds) {
		if (b.who >= 8 || (b.gatherers_max == 0 && b.gatherers == 0)) continue;
		std::printf("    %d:%-4d %-12s gatherers %d/%d (working %d)\n", b.who, b.o, type_name(g, b.type).c_str(), b.gatherers,
		            b.gatherers_max, b.gatherers_working);
	}
	for (const UnitView& u : v.units) {
		if (u.who >= 8 || (u.gather_o < 0 && u.collecting < 0)) continue;
		const Unit* un = g.unit(u.who, u.o);
		const Order* a = un ? get_action(*un) : nullptr;
		std::printf("      worker %d:%-4d %-10s at %d:%d %s carry %d (%6d,%6d) anim %d", u.who, u.o, type_name(g, u.type).c_str(),
		            u.gather_who, u.gather_o, u.gather_arrived ? "arrived" : "walking", u.carrying, u.x, u.y,
		            un && !un->guys.empty() ? int(un->guys[0].cur_anim) : -1);
		if (a && a->type == kGather) std::printf(" tile (%d,%d) wait %d goto %d", a->g_tx, a->g_ty, a->wait, int(a->goto_build));
		if (u.collecting >= 0) std::printf(" collecting %d", u.collecting);
		std::printf("\n");
	}
}

// Units created during the run (uid beyond the scenario's), with their state.
void print_new_units(const Game& g, const std::array<uint16_t, kNumLeaders>& initial) {
	for (int32_t who = 0; who < 8; who++) {
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (!u || !u->alive() || u->uid < initial[size_t(who)]) continue;
			const Order* f0 = u->front();
			std::printf("    new unit %d:%-4d %-18s (%6d,%6d) order %d%s target (%d,%d) orders %zu\n", who, o,
			            type_name(g, u->type).c_str(), u->x, u->y, int(u->order_type()), u->on_map() ? "" : " inside",
			            f0 ? f0->x : -1, f0 ? f0->y : -1, u->orders.size());
		}
	}
}

// A GameEvent (sim/game.hpp) as a log line: captures (with the plunder per good: food timber wealth knowledge metal
// oil), defeats, victories, game over, capital loss / recapture.
void print_event(const Game& g, const GameEvent& e) {
	switch (e.kind) {
	case 1: {
		const Build* nb = e.o2 >= 0 ? g.build(e.who, e.o2) : nullptr;
		std::printf("  f%-5d CITY CAPTURED L%d:%d -> L%d:%d (%s, city %d, damage %d/%d) buildings swapped %d closed %d "
		            "disbanded %d, units %d | plunder to L%d",
		            e.frame, e.other, e.o, e.who, e.o2, nb ? type_name(g, nb->type).c_str() : "?", e.value,
		            nb ? nb->damage : 0, nb ? obj_hits(g, *nb) : 0, e.n_swapped, e.n_closed, e.n_disbanded, e.n_units, e.who);
		for (int32_t k = 0; k < 6; k++) std::printf(" %d", e.goods[size_t(k)]);
		std::printf(" to L%d", e.other);
		for (int32_t k = 0; k < 6; k++) std::printf(" %d", e.goods2[size_t(k)]);
		std::printf("\n");
		break;
	}
	case 2: std::printf("  f%-5d LEADER DEFEATED L%d by %d (type %d)\n", e.frame, e.who, e.other, e.value); break;
	case 3: std::printf("  f%-5d VICTORY L%d (type %d)\n", e.frame, e.who, e.value); break;
	case 4: std::printf("  f%-5d GAME OVER\n", e.frame); break;
	case 5: std::printf("  f%-5d CAPITAL LOST L%d (held by L%d)\n", e.frame, e.who, e.other); break;
	case 6: std::printf("  f%-5d CAPITAL RECAPTURED L%d\n", e.frame, e.who); break;
	case 7: std::printf("  f%-5d BUILDING CAPTURED L%d:%d -> L%d:%d\n", e.frame, e.other, e.o, e.who, e.o2); break;
	default: break;
	}
}

RunResult run(const ron::GameFiles& files, const ron::Scenario& sc, const Rules& rules, const AnimSource* anims,
              uint32_t seed, int32_t frames, const std::vector<Script>& scripts, int32_t dump_every,
              int32_t digest_every, bool print, const FogOpts& fo, int32_t combat_every = 0, int32_t prod_every = 0) {
	auto g = std::make_unique<Game>();
	g->anims = anims;
	if (fo.reveal >= 0) g->info.reveal_map = uint8_t(fo.reveal);
	if (fo.starting_resources >= 0) g->info.starting_resources = uint8_t(fo.starting_resources);
	if (fo.difficulty >= 0) g->info.difficulty = uint8_t(fo.difficulty);
	g->load(files, sc, rules, seed);
	size_t ev_seen = g->events.size();
	ScriptLoadReport srep;
	if (fo.scripts) {
		srep = script_host_load(*g, files, fo.scenario_path, sc);
		g->scripts->verbose = fo.script_log;
	}
	FogCheck fc;
	CommandManager cm;
	RunResult rr;
	uint64_t script_seq = 0;
	size_t objectives_seen = 0;
	std::vector<std::pair<std::string, int32_t>> objectives_done;
	for (int32_t who = 0; who < kNumLeaders; who++) rr.initial_uids[size_t(who)] = g->objects[size_t(who)].obj_ctr;
	// Every scripted unit is tracked from its first command.
	for (const Script& s : scripts) {
		for (int32_t o : s.objs) {
			bool known = false;
			for (const Tracked& t : rr.tracked) known |= t.who == s.who && t.o == o;
			if (known) continue;
			const Unit* u = g->unit(s.who, o);
			if (!u) continue;
			rr.tracked.push_back({s.who, o, u->x, u->y, s.x, s.y, -1, s.frame, 0, u->x, u->y, 0});
		}
		auto watch = [&](int32_t w, int32_t o) {
			for (const auto& e : rr.watched)
				if (e[0] == w && e[1] == o) return;
			rr.watched.push_back({w, o});
		};
		if (s.kind == Script::Garrison) watch(s.twho, s.to);
		if (s.kind == Script::EjectAll)
			for (int32_t o : s.objs) watch(s.who, o);
		if (s.kind == Script::Attack) {  // the attack target too
			bool known = false;
			for (const Tracked& t : rr.tracked) known |= t.who == s.twho && t.o == s.to;
			const Unit* u = g->unit(s.twho, s.to);
			if (!known && u) rr.tracked.push_back({s.twho, s.to, u->x, u->y, u->x, u->y, -1, s.frame, 0, u->x, u->y, 0});
		}
	}
	for (int32_t f = 0; f < frames; f++) {
		for (const Script& s : scripts) {
			if (s.frame != g->frame) continue;
			cm.play = s.who;
			cm.play_who = s.who;
			if (s.kind == Script::Move) {
				cm.issue_move_to(*g, s.objs, s.x, s.y, QueuePos(s.queue), s.form);
				for (Tracked& t : rr.tracked) {
					for (int32_t o : s.objs) {
						if (t.who == s.who && t.o == o) {
							t.tx = s.x;
							t.ty = s.y;
							t.cmd_frame = s.frame;
							t.arrive = -1;
						}
					}
				}
			} else if (s.kind == Script::Form) {
				cm.issue_form(*g, s.objs, s.form, s.rotate);
			} else if (s.kind == Script::Attack) {
				cm.issue_attack(*g, s.objs, s.to, s.twho, QueuePos(s.queue));
			} else if (s.kind == Script::Garrison) {
				cm.issue_garrison(*g, s.objs, s.to, s.twho, QueuePos(s.queue));
				if (print) std::printf("  f%-5d GARRISON %d:%d%s -> building %d:%d\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0],
				                       s.objs.size() > 1 ? "+" : "", s.twho, s.to);
			} else if (s.kind == Script::EjectAll) {
				cm.issue_eject_all(*g, s.objs, s.num != 0);
				if (print) std::printf("  f%-5d EJECTALL %d:%d%s%s\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0],
				                       s.objs.size() > 1 ? "+" : "", s.num ? " (back to work)" : "");
			} else if (s.kind == Script::Build || s.kind == Script::Queue) {
				const int32_t t = find_type(*g, s.type);
				if (t < 0) throw std::runtime_error("unknown type " + s.type);
				if (s.kind == Script::Build) cm.issue_build(*g, s.objs, t, s.x, s.y, QueuePos(s.queue));
				else cm.issue_queue(*g, s.objs, t, s.num);
				if (print) std::printf("  f%-5d %s %s (%d) by %d:%d%s\n", g->frame, s.kind == Script::Build ? "BUILD" : "QUEUE_UP",
				                       type_name(*g, t).c_str(), t, s.who, s.objs.empty() ? -1 : s.objs[0],
				                       s.objs.size() > 1 ? "+" : "");
			} else if (s.kind == Script::Rally) {
				cm.issue_gather_point(*g, s.objs, s.x, s.y, s.action, s.queue == 1);
			} else if (s.kind == Script::Swarm) {
				cm.issue_swarm_around(*g, s.objs, s.to, s.twho, s.order, QueuePos(s.queue));
			} else if (s.kind == Script::Disband) {
				cm.issue_disband(*g, s.objs, s.objs.size() > 1);
			} else if (s.kind == Script::Unqueue) {
				for (int32_t o : s.objs) cm.issue_unqueue(*g, o, s.num);
			} else if (s.kind == Script::Gather) {
				cm.issue_gather(*g, s.objs, s.to, QueuePos(s.queue));
			} else if (s.kind == Script::CityGather) {
				cm.issue_city_gather(*g, s.objs, s.type == "-1" ? -1 : find_type(*g, s.type));
			} else if (s.kind == Script::Buy || s.kind == Script::Sell) {
				if (s.kind == Script::Buy) cm.issue_buy(s.x, s.num);
				else cm.issue_sell(s.x, s.num);
				if (print) std::printf("  f%-5d %s good %d flags %d by L%d\n", g->frame, s.kind == Script::Buy ? "BUY" : "SELL", s.x,
				                       s.num, s.who);
			} else if (s.kind == Script::Tribute) {
				const int32_t whom = s.objs.empty() ? 0 : s.objs[0];
				cm.issue_tribute(whom, s.num, s.x < 0 ? -s.x : s.x, s.x < 0);
			} else if (s.kind == Script::Respond) {
				cm.issue_respond(s.objs.empty() ? 0 : s.objs[0], s.num != 0);
			} else if (s.kind == Script::Peasants) {
				const Leader& l = g->leaders[size_t(s.who)];
				cm.issue_leader_options(s.x, l.opt_peasants_wait, l.opt_buildings, l.options_flags);
			} else if (s.kind == Script::Select || s.kind == Script::Option) {
				// The seat-local input the renderer would report (sim/script_ui.hpp): the selection / armed command.
				if (ScriptInput* in = script_input(*g)) {
					in->attached = false;  // keep the headless sound model
					if (s.kind == Script::Select) {
						in->select_who = s.who;
						in->selected = s.objs;
					} else {
						in->option = s.x;
						in->selecting_spot = s.x >= 0;
						in->build_options = s.y >= 0;
						in->mode_data = s.y;
						in->option_object = s.to;
					}
				}
			} else {
				cm.issue_halt(*g, s.objs);
			}
			cm.process_turn(*g);
		}
		g->do_frame();
		if (print) {
			for (; ev_seen < g->events.size(); ev_seen++) print_event(*g, g->events[ev_seen]);
			if (fo.script_log) print_script_events(*g, script_seq, objectives_seen, objectives_done);
		}
		if (fo.fog_check) fog_check_frame(*g, fc, print);
		const uint64_t d = digest(*g);
		rr.digests.push_back(d);
		for (Tracked& t : rr.tracked) {
			const Unit* u = g->unit(t.who, t.o);
			if (!u || !u->alive()) continue;
			const int32_t step = std::abs(u->x - t.px) + std::abs(u->y - t.py);
			if (step > t.max_step) t.max_step = step;
			if (step) t.frames_moving++;
			t.px = u->x;
			t.py = u->y;
			if (t.arrive < 0 && g->frame > t.cmd_frame && u->orders.empty()) t.arrive = g->frame;
		}
		if (print && digest_every > 0 && (g->frame % digest_every == 0 || g->frame == frames))
			std::printf("frame %5d digest %016llx rng %08x\n", g->frame, (unsigned long long)d, g->rng.seed());
		if (print && combat_every > 0 && g->frame % combat_every == 0) {
			std::printf("  f%-5d", g->frame);
			for (int32_t who = 0; who < 8; who++) {
				if (!(g->leaders[size_t(who)].flags & leaderflag::kValid)) continue;
				int32_t n = 0, hp = 0;
				for (int32_t o = 0; o < g->objects[size_t(who)].unit_mark; o++) {
					const Unit* u = g->unit(who, o);
					if (!u || !u->alive()) continue;
					n++;
					hp += std::max(0, u->myhits / std::max(1, u->ptype->uber_size) - u->damage);
				}
				std::printf(" L%d:%d units %d hp kills %d", who, n, hp, g->leaders[size_t(who)].units_killed);
			}
			int32_t live = 0, stuck = 0, corpses = 0;
			for (const Ammo& a : g->ammo) {
				if (a.flags & ammobits::kFlying) live++;
				else if (a.flags & ammobits::kStuck) stuck++;
			}
			for (const DeathObj& d : g->deaths) corpses += d.valid ? 1 : 0;
			std::printf(" | ammo %d stuck %d corpses %d\n", live, stuck, corpses);
		}
		if (print && fo.econ_every > 0 && g->frame % fo.econ_every == 0) print_economy(*g);
		if (print && fo.ai_every > 0 && g->frame % fo.ai_every == 0) print_ai(*g);
		if (print && prod_every > 0 && g->frame % prod_every == 0) {
			print_production(*g, false);
			print_new_units(*g, rr.initial_uids);
		}
		if (print && dump_every > 0 && g->frame % dump_every == 0) {
			for (const Tracked& t : rr.tracked) {
				const Unit* u = g->unit(t.who, t.o);
				if (!u) continue;
				const Order* f0 = u->front();
				std::printf("  f%-5d %d:%-4d (%6d,%6d) a=%08x order=%d tgt=%d:%d path=%zu anim=%d collide=%d dmg=%d%s%s\n",
				            g->frame, t.who, t.o, u->x, u->y, u->angle, int(u->order_type()), f0 ? f0->whom : -1,
				            f0 ? f0->ox : -1, u->path.size(), u->guys.empty() ? -1 : int(u->guys[0].cur_anim),
				            int(u->collide), u->damage, u->alive() ? "" : " DEAD", u->on_map() ? "" : " INSIDE(on_map=0)");
			}
			// The buildings the garrison scripts name: what is inside and the garrison arrows they fire.
			for (const auto& wb : rr.watched) {
				const Build* b = g->build(wb[0], wb[1]);
				if (!b || !b->alive()) continue;
				std::printf("  f%-5d bld %d:%-4d %-14s inside %d (pop %d) arrows %d limit %d%s\n", g->frame, wb[0], wb[1],
				            type_name(*g, b->type).c_str(), num_inside(*g, *b, 1), num_inside(*g, *b, 0),
				            b->ptype->attack != 0 ? get_garrison_arrows(*g, *b) : 0, get_garrison_limit(*g, b->type, b->who),
				            (b->build_masks & bmask::kEjecting) ? " EJECTING" : "");
			}
		}
	}
	rr.final_frame = g->frame;
	if (print) {
		std::printf("\nmoved units (target, start -> end, distance left, arrival frame, frames moving, max step):\n");
		for (const Tracked& t : rr.tracked) {
			const Unit* u = g->unit(t.who, t.o);
			if (!u) continue;
			const int32_t left = std::abs(u->x - t.tx) + std::abs(u->y - t.ty);
			std::printf("  %d:%-4d %-22s target (%6d,%6d)  (%6d,%6d) -> (%6d,%6d)  left %5d  arrive %5d  moving %4d  max %d\n",
			            t.who, t.o, type_name(*g, u->type).c_str(), t.tx, t.ty, t.sx, t.sy, u->x, u->y, left, t.arrive,
			            t.frames_moving, t.max_step);
		}
		std::printf("market tick %d:", g->market.tick);
		for (int32_t i = 0; i < 6; i++) std::printf(" %d%+d", g->market.level[size_t(i)], g->market.flux[size_t(i)]);
		std::printf("  (level+flux per good: food timber wealth knowledge metal oil)\n");
		if (prod_every > 0) {
			print_production(*g, true);
			print_new_units(*g, rr.initial_uids);
		}
		std::printf("final frame %d tick %d rng %08x digest %016llx\n", g->frame, g->tick, g->rng.seed(),
		            (unsigned long long)rr.digests.back());
		if (fo.fog_check) {
			std::printf("fog check: %lld refreshes, %lld stale cells cleared, %lld violations, %lld covered cells "
			            "missing: %s\n",
			            (long long)fc.refreshes, (long long)fc.cleared, (long long)fc.violations, (long long)fc.lost,
			            fc.violations == 0 && fc.lost == 0 ? "OK" : "FAILED");
		}
		if (fo.fog_who >= 0 && fo.fog_who < 8) print_fog_map(*g, fo.fog_who);
		print_scripts(*g, srep, fo.script_log);
	}
	rr.fog_failed = fo.fog_check && (fc.violations != 0 || fc.lost != 0);
	return rr;
}

const char* arg_value(int argc, char** argv, const char* key) {
	for (int i = 3; i + 1 < argc; ++i) {
		if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
	}
	return nullptr;
}
bool has_flag(int argc, char** argv, const char* key) {
	for (int i = 3; i < argc; ++i) {
		if (std::strcmp(argv[i], key) == 0) return true;
	}
	return false;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc >= 3 && std::strcmp(argv[1], "--dump") == 0) return dump_tables(argv[2]);
	if (argc < 3) {
		std::fprintf(stderr,
		             "usage: %s <game dir> <scenario.scx> [--frames N] [--seed S] [--list] "
		             "[--move WHO:o1,o2@X,Y[@FRAME][/form][%%queue]]... [--halt WHO:o1,o2@FRAME]... "
		             "[--form WHO:o1,o2@FRAME/form[,rotate]]... [--dump-every K] "
		             "[--digest-every K] [--no-selfcheck] [--anims] [--no-art] [--reveal N] [--fog WHO] [--fog-check]\n"
		             "       %s --dump circle|ring|stamps\n",
		             argv[0], argv[0]);
		return 2;
	}
	try {
		const ron::GameFiles files(argv[1]);
		std::string file = argv[2];
		std::error_code ec;
		if (!std::filesystem::exists(file, ec)) {
			const std::string r = files.resolve(file);
			if (r.empty()) throw std::runtime_error("scenario not found: " + file);
			file = r;
		}
		const ron::Scenario sc = ron::parse_scenario(ron::read_file(file));
		Rules rules = load_rules(files);
		start_game(rules, GameSetup());
		const char* fa = arg_value(argc, argv, "--frames");
		const int32_t frames = fa ? int32_t(std::strtol(fa, nullptr, 0)) : 300;
		const char* sa = arg_value(argc, argv, "--seed");
		const uint32_t seed = sa ? uint32_t(std::strtoul(sa, nullptr, 0)) : 12345u;
		const char* da = arg_value(argc, argv, "--dump-every");
		const int32_t dump_every = da ? int32_t(std::strtol(da, nullptr, 0)) : 0;
		const char* ca = arg_value(argc, argv, "--combat-every");
		const int32_t combat_every = ca ? int32_t(std::strtol(ca, nullptr, 0)) : 0;
		const char* ga = arg_value(argc, argv, "--digest-every");
		const int32_t digest_every = ga ? int32_t(std::strtol(ga, nullptr, 0)) : 1;
		std::unique_ptr<AssetAnimSource> art;
		if (!has_flag(argc, argv, "--no-art")) art = load_anim_source(files, rules);
		std::vector<Script> scripts;
		for (int i = 3; i + 1 < argc; i++) {
			if (std::strcmp(argv[i], "--move") == 0) scripts.push_back(parse_move(argv[i + 1]));
			else if (std::strcmp(argv[i], "--halt") == 0) scripts.push_back(parse_halt(argv[i + 1]));
			else if (std::strcmp(argv[i], "--select") == 0) {  // WHO:o1,o2@FRAME (WHO = leader index; empty list clears)
				Script s = parse_halt(argv[i + 1]);
				s.kind = Script::Select;
				scripts.push_back(s);
			} else if (std::strcmp(argv[i], "--option") == 0) {  // N[:MENU[:TYPE]]@FRAME: the armed OptionIndex (-1 none)
				Script s;
				s.kind = Script::Option;
				const std::string a = argv[i + 1];
				const size_t at = a.find('@');
				char* end = nullptr;
				s.x = int32_t(std::strtol(a.c_str(), &end, 0));
				s.y = -1, s.to = -1;
				if (end && *end == ':') {
					s.y = int32_t(std::strtol(end + 1, &end, 0));
					if (end && *end == ':') s.to = int32_t(std::strtol(end + 1, &end, 0));
				}
				if (at != std::string::npos) s.frame = int32_t(std::strtol(a.c_str() + at + 1, nullptr, 0));
				scripts.push_back(s);
			}
			else if (std::strcmp(argv[i], "--form") == 0) scripts.push_back(parse_form(argv[i + 1]));
			else if (std::strcmp(argv[i], "--attack") == 0) scripts.push_back(parse_attack(argv[i + 1]));
			else if (std::strcmp(argv[i], "--garrison") == 0) scripts.push_back(parse_garrison(argv[i + 1]));
			else if (std::strcmp(argv[i], "--eject-all") == 0) scripts.push_back(parse_eject_all(argv[i + 1]));
			else if (std::strcmp(argv[i], "--build") == 0) scripts.push_back(parse_prod(Script::Build, argv[i + 1]));
			else if (std::strcmp(argv[i], "--queue") == 0 || std::strcmp(argv[i], "--research") == 0)
				scripts.push_back(parse_prod(Script::Queue, argv[i + 1]));
			else if (std::strcmp(argv[i], "--rally") == 0) scripts.push_back(parse_prod(Script::Rally, argv[i + 1]));
			else if (std::strcmp(argv[i], "--swarm") == 0) scripts.push_back(parse_prod(Script::Swarm, argv[i + 1]));
			else if (std::strcmp(argv[i], "--disband") == 0) scripts.push_back(parse_prod(Script::Disband, argv[i + 1]));
			else if (std::strcmp(argv[i], "--unqueue") == 0) scripts.push_back(parse_prod(Script::Unqueue, argv[i + 1]));
			else if (std::strcmp(argv[i], "--gather") == 0) scripts.push_back(parse_prod(Script::Gather, argv[i + 1]));
			else if (std::strcmp(argv[i], "--city-gather") == 0) scripts.push_back(parse_prod(Script::CityGather, argv[i + 1]));
			else if (std::strcmp(argv[i], "--buy") == 0) scripts.push_back(parse_prod(Script::Buy, argv[i + 1]));
			else if (std::strcmp(argv[i], "--sell") == 0) scripts.push_back(parse_prod(Script::Sell, argv[i + 1]));
			else if (std::strcmp(argv[i], "--tribute") == 0) scripts.push_back(parse_prod(Script::Tribute, argv[i + 1]));
			else if (std::strcmp(argv[i], "--respond") == 0) scripts.push_back(parse_prod(Script::Respond, argv[i + 1]));
			else if (std::strcmp(argv[i], "--peasants") == 0) scripts.push_back(parse_prod(Script::Peasants, argv[i + 1]));
		}
		const char* pa = arg_value(argc, argv, "--prod-every");
		const int32_t prod_every = pa ? int32_t(std::strtol(pa, nullptr, 0)) : 0;
		// GameInfo.reveal_map: --reveal N, else the scenario header's value, else 1 (Normal).
		const char* ra = arg_value(argc, argv, "--reveal");
		const int32_t reveal =
		    ra ? int32_t(std::strtol(ra, nullptr, 0)) : (sc.reveal_map >= 0 && sc.reveal_map <= 3 ? sc.reveal_map : 1);
		if (has_flag(argc, argv, "--list")) {
			Game g;
			g.anims = art.get();
			g.info.reveal_map = uint8_t(reveal);
			g.load(files, sc, rules, seed);
			std::printf("%s: %dx%d wcoords (%d x %d world units), seed %u, rng after load %08x\n", file.c_str(), g.world.xs,
			            g.world.ys, g.world.xs * 768, g.world.ys * 768, seed, g.rng.seed());
			std::printf("settings: team_style %d game_rules %d elimination %d victory %d score_goal %d time_limit %d "
			            "wonderwin %d popwin %d econwin %d nations %d\n",
			            g.info.team_style, g.info.game_rules, g.info.elimination, g.info.victory, g.info.score_goal,
			            g.info.time_limit, g.info.wonderwin, g.info.popwin, g.info.econwin, g.info.num_nations);
			for (int32_t who = 0; who < kNumLeaders; who++) {
				const Leader& l = g.leaders[size_t(who)];
				if (!(l.flags & leaderflag::kValid)) continue;
				std::printf("leader %d flags %x tribe %d colour %d %s  diplos", who, l.flags, l.tribe, l.color, l.name.c_str());
				for (int32_t i = 0; i < 10; i++) std::printf(" %d", l.diplos(i));
				std::printf("  ally_mask %02x science %d explored %d\n", unsigned(l.ally_mask), l.science, l.explored);
				for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
					const Unit* u = g.unit(who, o);
					if (!u || !u->alive()) continue;
					std::printf("  unit %d:%-4d %-24s (%6d,%6d) angle %08x speed %d los %d hits %d attack %d armor %d range %d%s%s\n", who, o,
					            type_name(g, u->type).c_str(), u->x, u->y, u->angle, int(u->myspeed), object_los(g, *u),
					            obj_hits(g, *u), obj_attack(g, *u), obj_armor(g, *u), obj_max_range(g, *u),
					            u->on_map() ? "" : " inside", u->o_up >= 0 ? " follower" : "");
				}
			}
			print_production(g, true);
			return 0;
		}
		if (has_flag(argc, argv, "--anims") && art) {
			Game g;
			g.anims = art.get();
			g.load(files, sc, rules, seed);
			print_anim_table(g, *art);
		}
		FogOpts fo;
		if (const char* fw = arg_value(argc, argv, "--fog")) fo.fog_who = int32_t(std::strtol(fw, nullptr, 0));
		fo.fog_check = has_flag(argc, argv, "--fog-check");
		fo.reveal = reveal;
		if (const char* ee = arg_value(argc, argv, "--econ-every")) fo.econ_every = int32_t(std::strtol(ee, nullptr, 0));
		if (const char* ae = arg_value(argc, argv, "--ai-every")) fo.ai_every = int32_t(std::strtol(ae, nullptr, 0));
		if (const char* sr = arg_value(argc, argv, "--starting-resources"))
			fo.starting_resources = int32_t(std::strtol(sr, nullptr, 0));
		if (const char* df = arg_value(argc, argv, "--difficulty")) fo.difficulty = int32_t(std::strtol(df, nullptr, 0));
		fo.scripts = !has_flag(argc, argv, "--no-scripts");
		fo.script_log = has_flag(argc, argv, "--script-log");
		fo.scenario_path = std::filesystem::absolute(file).string();
		const RunResult a = run(files, sc, rules, art.get(), seed, frames, scripts, dump_every, digest_every, true, fo,
		                        combat_every, prod_every);
		if (a.fog_failed) return 1;
		if (!has_flag(argc, argv, "--no-selfcheck")) {
			const RunResult b = run(files, sc, rules, art.get(), seed, frames, scripts, 0, 0, false, fo);
			size_t diff = a.digests.size();
			for (size_t i = 0; i < a.digests.size() && i < b.digests.size(); i++) {
				if (a.digests[i] != b.digests[i]) {
					diff = i;
					break;
				}
			}
			if (diff == a.digests.size() && a.digests.size() == b.digests.size()) {
				std::printf("determinism self-check: OK (%zu frames, identical digests)\n", a.digests.size());
			} else {
				std::printf("determinism self-check: FAILED at frame %zu\n", diff + 1);
				return 1;
			}
		}
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
