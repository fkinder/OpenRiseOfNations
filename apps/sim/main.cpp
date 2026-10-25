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
//                [--flight WHO:o1,..@TO:TWHO[@FRAME][/flags]]... [--air-patrol|--launch-patrol|--attack-ground WHO:o1,..@X,Y[@FRAME][/flags]]...
//                [--scramble|--recall|--kill|--crash|--auto-explore|--carrier-repeat WHO:o1,..[@FRAME]]... [--stock WHO:N[@FRAME]]... [--air-every K]
//                [--unitmask WHO:o1,..@MASK[@FRAME]]... [--flock WHO:TX,TY,N[@FRAME]]...
//                [--save-at FRAME:path]... [--load path] [--roundtrip FRAME]
//        ron-sim --dump circle|ring|stamps
// Dead crafts, strafing slide, flocks, unit masks (docs/aircraft-spec.md 12-14): --crash kills the aircraft the way a shot-down or out-of-fuel plane
// dies (death type 2, so Objects::kill_guy gives each guy its Ammo::init_crash debris; --kill is the silent death 0); --flock adds a bird flock
// (Objects::add_flock; WHO -1 = gaia, else the leader's 225-frame limit applies) at tile (TX, TY); --unitmask issues UNITMASK (Group::action_unitmask)
// with an arbitrary mask, --auto-explore the AUTO_EXPLORE toggle (0x100) and --carrier-repeat the carrier's REPEAT toggle (0x200000).
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
// kept). --list prints `inside N` on a building (and on a unit) with a garrison. Transports (docs/transport-spec.md):
// --transport / --set-transport / --board (see parse_transport); RON_TRANSPORT_TRACE=1 logs casts, barges and landings.
// Heroes (docs/heroes-spec.md): --cast WHO:o@SPELL[@FRAME] issues GROUP + SPELL (verb 23, ox / whom -1, x / y at the unit) for
// a general's power (SPELL: a craft index such as 0x27b, or DECOY / AMBUSH / MARCH / ENTRENCH / SUPPLIES). Crafts
// (docs/spells-spec.md): --cast WHO:o@SPELL@TO:TWHO[@FRAME] casts a targeted craft on object TO of leader TWHO (BRIBE, COUNTERINTEL,
// INFORMER, SABOTAGE, SNIPER), @X,Y an area craft; PACK / UNPACK / CIVILIAN / TOARMS / DISBAND / DEPOPULATE need no target;
// RON_SPELL_TRACE=1 (+ RON_CAST_LOG=1) logs refused casts and every cast order created, RON_HERO_TRACE=1 every SpellType::cast;
// --teleport WHO:o@X,Y[@FRAME] moves a unit (a test aid); --spells-every K [--spells-all WHO] lists the casters, packers, merchants and
// planted buildings; --spell-fixtures [--verbose] runs the section 9 acceptance checks on the loaded scenario. --heroes-every K
// prints per leader every hero (radius in tiles, running powers), the units under cover and the decoys; --list shows each
// hero's radius and powers.
// --attack issues GROUP + ATTACK (target object TO of leader TWHO). --combat-every prints a combat summary (alive
// units and hit points per leader, live ammo, corpses, kills) every K frames.
// --move issues GROUP + MOVE_TO for the listed units of leader WHO at FRAME (default 0); /form sets the formation
// byte (default -1 = the units' own; 10 = KEEP, the current arrangement turned to the travel direction), %queue the QueuePos (0 FIRST, 1 LAST, 2 NEW = default). --form issues FORM (form -1 next,
// -3 previous, -2 keep; rotate is added to the formation angle) with QUEUE_NEW. --dump-every prints the moved units'
// positions every K frames. The run is
// repeated from a fresh load and the digests compared (the determinism self-check) unless --no-selfcheck.
// Animation data (guy clocks) comes from the install's unit_graphics.xml / anim_graphics.xml / .bha files; --no-art
// runs without it (3-frame animations). --anims prints, after loading, the frames per animation of every graphics
// piece the scenario's units use.
// Aircraft (docs/aircraft-spec.md, sim/aircraft*.cpp): --flight issues GROUP + FLIGHT (the planes, or the airbases / carriers
// named, attack object TO of leader TWHO; /move rebases onto it instead; /shift,/ctrl,/alt are the launch modifiers),
// --air-patrol PATROL, --launch-patrol LAUNCH_PATROL (planes out of the listed airbases / carriers), --attack-ground
// ATTACK_GROUND (/last,/first give the QueuePos); --scramble / --recall issue SCRAMBLE / RECALL for the listed units.
// --list and --air-every K print one line per airbase / carrier / silo (planes here with and without the queue, launch timer
// and list, the chain) and per aircraft (fuel left / mana, mana_burn, base, order, low / high band). RON_AIR_TRACE=1 logs
// launches, landings, carrier releases, crashes and missile detonations on stderr. Test aids that are not game commands:
// --kill (the objects die at once) and --stock (every good of the leader is set to N). The digest gains an aircraft block
// only when some aircraft state is non-default (RON_NO_AIR_DIGEST=1 leaves it out to compare against older builds);
// RON_DIGEST_PARTS=1 prints the running hash after each digest section on stderr.
// Fog of war: --reveal sets GameInfo.reveal_map (default: the scenario header's, else 1 = Normal); --fog WHO prints the
// fog map of that seat at the end; --fog-check verifies at every refresh (frame % 100 == 33) that the stale trail of
// moving units is cleared and stationary areas survive. --dump prints the circle / ring tables or every stamp range
// in the format of `node re/tools/los-ref.mjs dump circle|ring|stamps` (diff them).
// Save / load (sim/save.hpp, docs/save-format.md): --save-at FRAME:path writes the game state at the start of FRAME
// (before that frame's scripted commands; repeatable; a path ending in .gz is gzipped); --load path continues a save
// (the scenario argument may then be omitted: `ron-sim <game dir> --load path`), --frames N runs N more frames from the
// saved frame and the scripted commands still apply by absolute frame. --roundtrip FRAME is the self-check: run to FRAME,
// save to a temporary file here, keep running to the end recording the digests, then load that file, run the same
// remaining frames and compare digest by digest (and the full saved state of the last frame); it prints
// `save round-trip: OK` or the first differing frame, after the determinism self-check; it then asserts the script state (timers,
// goals, objectives, static variables, trigger bits, ScenarioData) the loaded game starts with equals the saved game's and prints it
// (`save round-trip: script state restored: ...`).
#include <algorithm>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "assets/scenario.hpp"
#include "assets/strings.hpp"
#include "sim/aircraft.hpp"
#include "sim/flocks.hpp"
#include "sim/anim_assets.hpp"
#include "sim/combat.hpp"
#include "sim/commands.hpp"
#include "sim/orders.hpp"
#include "sim/ai.hpp"
#include "sim/production.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/guys.hpp"
#include "sim/heroes.hpp"
#include "sim/economy.hpp"
#include "sim/spells.hpp"
#include "sim/combat_internal.hpp"
#include "sim/newgame.hpp"
#include "sim/rules.hpp"
#include "sim/save.hpp"
#include "sim/script_host.hpp"
#include "sim/placement.hpp"
#include "sim/view.hpp"
#include "sim/capture.hpp"
#include "sim/fog.hpp"
#include "sim/victory.hpp"
#include "sim/trig.hpp"

// cities.cpp: Wonders::init_wonder / close_wonder, used by the --victory fixtures.
namespace ron::sim {
int32_t wonders_init_wonder(Game& g, int32_t who, int32_t o);
void wonders_close_wonder(Game& g, int32_t who, int32_t idx);
}  // namespace ron::sim

namespace {

using namespace ron::sim;

struct Script {
	enum Kind { Move, Halt, Form, Attack, Build, Queue, Rally, Swarm, Disband, Unqueue, Gather, CityGather, Buy, Sell,
		        Tribute, Respond, Peasants, Select, Option, Garrison, EjectAll, Transport, SetTransport, Board, Cast, Flight,
		        AirPatrol, LaunchPatrol, AttackGround, Scramble, Recall, Kill, Crash, Flock, UnitMask, Stock, Teleport,
		        Hotkey, ComeOut, Speed, Chat, Pause, Rename, Cheat, Nuke } kind = Move;
	int32_t who = 0;
	std::vector<int32_t> objs;
	int32_t x = 0, y = 0, frame = 0, form = -1, queue = 2, rotate = 0;
	int32_t twho = -1, to = -1;
	std::string type;  // production: a type name (TYPENAME / NAME, case-insensitive) or a TypeIndex number
	int32_t num = 1, action = 0, order = 6;
	std::string text;  // --chat / --rename-city: the text; --cheat: the cheat's name
	float fx = -1.0f, fy = -1.0f;  // --hotkey bookmark
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

// The aircraft commands. WHO:o1,o2@TARGET[@FRAME][/flags] with TARGET = TO:TWHO (--flight: attack object TWHO:TO),
// X,Y (--air-patrol, --launch-patrol, --attack-ground: a map point); --scramble / --recall: WHO:o1,o2[@FRAME].
// flags (comma separated): move (--flight rebases onto the airbase / carrier instead of attacking), shift, ctrl, alt
// (LAUNCH_PATROL / FLIGHT modifiers), last / first (QueuePos).
Script parse_air(Script::Kind k, const std::string& a, const char* name) {
	Script s;
	s.kind = k;
	s.order = 10;  // ATTACK
	std::string t = a;
	const size_t slash = t.find('/');
	if (slash != std::string::npos) {
		std::stringstream ss(t.substr(slash + 1));
		std::string tok;
		while (std::getline(ss, tok, ',')) {
			if (tok == "move") s.order = 1;
			else if (tok == "shift") s.num |= 1;
			else if (tok == "ctrl") s.num |= 2;
			else if (tok == "alt") s.num |= 4;
			else if (tok == "last") s.queue = 1;
			else if (tok == "first") s.queue = 0;
			else throw std::runtime_error(std::string("bad --") + name + " flag " + tok);
		}
		t = t.substr(0, slash);
	}
	const size_t colon = t.find(':');
	if (colon == std::string::npos) throw std::runtime_error(std::string("bad --") + name + " " + a);
	s.who = int32_t(std::strtol(t.substr(0, colon).c_str(), nullptr, 0));
	std::string rest = t.substr(colon + 1);
	const size_t at1 = rest.find('@');
	s.objs = parse_list(rest.substr(0, at1));
	if (at1 == std::string::npos) return s;
	rest = rest.substr(at1 + 1);
	if (k == Script::Scramble || k == Script::Recall || k == Script::Kill || k == Script::Crash) {
		s.frame = int32_t(std::strtol(rest.c_str(), nullptr, 0));
		return s;
	}
	const size_t at2 = rest.find('@');
	const std::string tgt = rest.substr(0, at2);
	if (at2 != std::string::npos) s.frame = int32_t(std::strtol(rest.c_str() + at2 + 1, nullptr, 0));
	const size_t sep = tgt.find(k == Script::Flight ? ':' : ',');
	if (sep == std::string::npos) throw std::runtime_error(std::string("bad --") + name + " target " + a);
	const int32_t v1 = int32_t(std::strtol(tgt.substr(0, sep).c_str(), nullptr, 0));
	const int32_t v2 = int32_t(std::strtol(tgt.substr(sep + 1).c_str(), nullptr, 0));
	if (k == Script::Flight) s.to = v1, s.twho = v2;
	else s.x = v1, s.y = v2;
	return s;
}

// --nuke WHO:X,Y@FRAME: a nuclear explosion at world position (X, Y), fired by the first live unit of leader WHO (a stand-in for the
// missile). The run asserts the damage radius, the terrain scorch and the Armageddon count (docs/nukes-spec.md 8).
Script parse_nuke(const std::string& a) {
	Script s;
	s.kind = Script::Nuke;
	const size_t colon = a.find(':'), comma = a.find(',', colon == std::string::npos ? 0 : colon), at = a.find('@');
	if (colon == std::string::npos || comma == std::string::npos) throw std::runtime_error("bad --nuke " + a + " (WHO:X,Y@FRAME)");
	s.who = int32_t(std::strtol(a.c_str(), nullptr, 0));
	s.x = int32_t(std::strtol(a.c_str() + colon + 1, nullptr, 0));
	s.y = int32_t(std::strtol(a.c_str() + comma + 1, nullptr, 0));
	if (at != std::string::npos) s.frame = int32_t(std::strtol(a.c_str() + at + 1, nullptr, 0));
	return s;
}

// What --nuke measures: the objects around the centre, the height grid and the Armageddon counter before the explosion, checked at the end.
struct NukeProbe {
	struct Obj {
		int32_t who, o, dist, damage;
	};
	int32_t who = 0, o = -1, x = 0, y = 0, frame = 0, radius = 0, arm0 = 0, used0 = 0, craters0 = 0;
	std::vector<Obj> objs;
	std::vector<float> heights;
	std::vector<int32_t> arm_series;  // Game::armageddon after each frame from the detonation on
};

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

// Transports (docs/transport-spec.md 9): --transport WHO:o1,o2[@FRAME] (TRANSPORT: the units get a barge now),
// --set-transport WHO:o1,o2[@FRAME][/0|1] (SET_TRANSPORT flag, default 1), --board WHO:o1,o2@SHIP_O[@FRAME][%queue]
// (BOARD_SHIP: walk to the ship objects[WHO][SHIP_O] and go inside; queue default 2 = NEW).
Script parse_transport(const std::string& a, Script::Kind kind) {
	Script s;
	s.kind = kind;
	s.num = 1;  // SET_TRANSPORT flag
	std::string t = a;
	if (kind == Script::SetTransport) {
		const size_t slash = t.find('/');
		if (slash != std::string::npos) {
			s.num = int32_t(std::strtol(t.c_str() + slash + 1, nullptr, 0));
			t = t.substr(0, slash);
		}
	}
	if (kind == Script::Board) {
		const size_t pct = t.find('%');
		if (pct != std::string::npos) {
			s.queue = int32_t(std::strtol(t.c_str() + pct + 1, nullptr, 0));
			t = t.substr(0, pct);
		}
	}
	const size_t colon = t.find(':'), at = t.find('@');
	if (colon == std::string::npos) throw std::runtime_error("bad transport option " + a);
	s.who = int32_t(std::strtol(t.substr(0, colon).c_str(), nullptr, 0));
	s.objs = parse_list(t.substr(colon + 1, at == std::string::npos ? std::string::npos : at - colon - 1));
	std::string rest = at == std::string::npos ? std::string() : t.substr(at + 1);
	if (kind == Script::Board) {
		if (rest.empty()) throw std::runtime_error("bad --board (needs @SHIP_O) " + a);
		const size_t at2 = rest.find('@');
		s.to = int32_t(std::strtol(rest.substr(0, at2).c_str(), nullptr, 0));
		s.twho = s.who;
		rest = at2 == std::string::npos ? std::string() : rest.substr(at2 + 1);
	}
	if (!rest.empty()) s.frame = int32_t(std::strtol(rest.c_str(), nullptr, 0));
	return s;
}

std::vector<std::string> split(const std::string& s, char c);

// --cast WHO:o1,o2@SPELL[@TO:TWHO | @X,Y][@FRAME]: SPELL is a craft index (0x27b, 635) or a name (DECOY, AMBUSH, MARCH, ENTRENCH,
// SUPPLIES, BRIBE, COUNTERINTEL, INFORMER, SABOTAGE, SNIPER, DEPOPULATE, TRANSPORT, PACK, UNPACK, CIVILIAN, TOARMS, DISBAND).
// TO:TWHO names the target object (a targeted craft); X,Y the area of an area craft; both default to the caster's position.
Script parse_cast(const std::string& a) {
	Script s;
	s.kind = Script::Cast;
	const size_t colon = a.find(':'), at = a.find('@');
	if (colon == std::string::npos || at == std::string::npos || at < colon) throw std::runtime_error("bad --cast (WHO:o@SPELL[@TO:TWHO|@X,Y][@FRAME]) " + a);
	s.who = int32_t(std::strtol(a.substr(0, colon).c_str(), nullptr, 0));
	s.objs = parse_list(a.substr(colon + 1, at - colon - 1));
	const std::vector<std::string> parts = split(a.substr(at + 1), '@');
	std::string name = parts.empty() ? std::string() : parts[0];
	for (size_t i = 1; i < parts.size(); i++) {
		const std::string& p = parts[i];
		const size_t c2 = p.find(':'), cm = p.find(',');
		if (c2 != std::string::npos) {  // TO:TWHO
			s.num = int32_t(std::strtol(p.substr(0, c2).c_str(), nullptr, 0));
			s.twho = int32_t(std::strtol(p.c_str() + c2 + 1, nullptr, 0));
			s.action = 1;
		} else if (cm != std::string::npos) {  // X,Y
			s.x = int32_t(std::strtol(p.c_str(), nullptr, 0));
			s.y = int32_t(std::strtol(p.c_str() + cm + 1, nullptr, 0));
			s.action = 2;
		} else {
			s.frame = int32_t(std::strtol(p.c_str(), nullptr, 0));
		}
	}
	for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
	if (name == "DECOY") s.to = 0x27a;
	else if (name == "AMBUSH") s.to = 0x27b;
	else if (name == "MARCH") s.to = 0x27c;
	else if (name == "ENTRENCH") s.to = 0x27d;
	else if (name == "SUPPLIES") s.to = 0x27e;
	else if (name == "BRIBE") s.to = 0x275;
	else if (name == "COUNTERINTEL") s.to = 0x277;
	else if (name == "INFORMER") s.to = 0x27f;
	else if (name == "SABOTAGE") s.to = 0x280;
	else if (name == "SNIPER") s.to = 0x281;
	else if (name == "DEPOPULATE") s.to = 0x286;
	else if (name == "TRANSPORT") s.to = 0x28a;
	else if (name == "PACK") s.to = 0x28b;
	else if (name == "UNPACK") s.to = 0x28c;
	else if (name == "CIVILIAN") s.to = 0x293;
	else if (name == "TOARMS") s.to = 0x294;
	else if (name == "DISBAND") s.to = 0x29a;
	else if (name == "PARADROP") s.to = 0x284;
	else s.to = int32_t(std::strtol(name.c_str(), nullptr, 0));
	return s;
}


// The console / turn-control verbs (commands_extra.cpp):
// --hotkey WHO:o1,o2@N[@FRAME][/clear|/mark:X,Y,ZOOM]  control group N (0..17): GROUP + HOTKEY; /clear erases it, /mark stores the camera bookmark
// --come-out WHO:o1,o2[@FRAME]                         COME_OUT per unit        --pause 0|1[@FRAME]            PAUSE
// --speed N|up|down[@FRAME]                            SPEED_SET / UP / DOWN    --chat FRAME:WHO:text           CHAT to everybody
// --rename-city WHO:o@NAME[@FRAME]                     RENAME_CITY              --cheat NAME[:WHO|:T:X:Y][@FRAME]  NAME: viewall givetechs zerotechs buckets zerobuckets aispeed aispeednormal aitoggle unit
std::vector<std::string> split(const std::string& s, char c);
Script parse_console(Script::Kind kind, const std::string& a, const char* name) {
	Script s;
	s.kind = kind;
	std::string t = a;
	auto frame_suffix = [&](std::string& str) {
		const size_t at = str.rfind('@');
		if (at != std::string::npos && at + 1 < str.size() && std::isdigit(static_cast<unsigned char>(str[at + 1]))) {
			bool all = true;
			for (size_t i = at + 1; i < str.size(); i++) all &= std::isdigit(static_cast<unsigned char>(str[i])) != 0;
			if (all) {
				s.frame = int32_t(std::strtol(str.c_str() + at + 1, nullptr, 0));
				str = str.substr(0, at);
			}
		}
	};
	if (kind == Script::Chat) {  // FRAME:WHO:text
		const size_t c1 = t.find(':'), c2 = c1 == std::string::npos ? c1 : t.find(':', c1 + 1);
		if (c2 == std::string::npos) throw std::runtime_error(std::string("bad --") + name + " (FRAME:WHO:text) " + a);
		s.frame = int32_t(std::strtol(t.c_str(), nullptr, 0));
		s.who = int32_t(std::strtol(t.c_str() + c1 + 1, nullptr, 0));
		s.text = t.substr(c2 + 1);
		return s;
	}
	if (kind == Script::Speed || kind == Script::Pause) {
		frame_suffix(t);
		s.num = t == "up" ? -100 : t == "down" ? -101 : int32_t(std::strtol(t.c_str(), nullptr, 0));
		return s;
	}
	if (kind == Script::Cheat) {
		frame_suffix(t);
		const std::vector<std::string> p = split(t, ':');
		s.text = p.at(0);
		s.who = p.size() > 1 ? int32_t(std::strtol(p[1].c_str(), nullptr, 0)) : 0;
		if (s.text == "unit") {  // unit:WHO:T:X:Y (WHO -1 = every leader)
			if (p.size() < 5) throw std::runtime_error("bad --cheat unit:WHO:T:X:Y");
			s.num = int32_t(std::strtol(p[2].c_str(), nullptr, 0));
			s.x = int32_t(std::strtol(p[3].c_str(), nullptr, 0));
			s.y = int32_t(std::strtol(p[4].c_str(), nullptr, 0));
		}
		return s;
	}
	// WHO:o1,o2@... forms
	const size_t slash = t.find('/');
	std::string flag;
	if (slash != std::string::npos) {
		flag = t.substr(slash + 1);
		t = t.substr(0, slash);
	}
	const size_t colon = t.find(':');
	if (colon == std::string::npos) throw std::runtime_error(std::string("bad --") + name + " " + a);
	s.who = int32_t(std::strtol(t.substr(0, colon).c_str(), nullptr, 0));
	const std::vector<std::string> parts = split(t.substr(colon + 1), '@');
	s.objs = parse_list(parts.empty() ? std::string() : parts[0]);
	if (kind == Script::Hotkey) {
		if (parts.size() < 2) throw std::runtime_error("bad --hotkey WHO:o..@N");
		s.num = int32_t(std::strtol(parts[1].c_str(), nullptr, 0));
		if (parts.size() > 2) s.frame = int32_t(std::strtol(parts[2].c_str(), nullptr, 0));
		if (flag == "clear") s.action = 1;
		else if (flag.rfind("mark:", 0) == 0) {
			const std::vector<std::string> m = split(flag.substr(5), ',');
			s.action = 2;
			s.fx = m.size() > 0 ? float(std::atof(m[0].c_str())) : 0.0f;
			s.fy = m.size() > 1 ? float(std::atof(m[1].c_str())) : 0.0f;
			s.x = m.size() > 2 ? int32_t(std::strtol(m[2].c_str(), nullptr, 0)) : 0;
		}
	} else if (kind == Script::Rename) {
		if (parts.size() < 2) throw std::runtime_error("bad --rename-city WHO:o@NAME");
		s.text = parts[1];
		if (parts.size() > 2) s.frame = int32_t(std::strtol(parts[2].c_str(), nullptr, 0));
	} else {  // ComeOut
		if (parts.size() > 1) s.frame = int32_t(std::strtol(parts[1].c_str(), nullptr, 0));
	}
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
	static const bool parts = std::getenv("RON_DIGEST_PARTS") != nullptr;
#define PART(n) do { if (parts) std::fprintf(stderr, "f%d %s %016llx\n", g.frame, n, (unsigned long long)f.h); } while (0)
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
			if (std::getenv("RON_DIGEST_UNITS"))
				std::fprintf(stderr, "f%d unit %d:%d %016llx type %d pos %d,%d masks %x/%x orders %d%s damage %d idle %d\n", g.frame, who, o,
				             (unsigned long long)f.h, u->type, u->x, u->y, u->unit_masks, u->unit_masks2, int(u->orders.size()),
				             u->orders.empty() ? "" : (" front " + std::to_string(int(u->orders.front().type)) + " spell " + std::to_string(u->orders.front().spell)).c_str(),
				             u->damage, int(u->idle));
		}
	}
	PART("units");
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
	PART("fog");
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
	PART("combat");
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
			for (const CaravanRec& r : g.caravans[size_t(who)]) {
				f.add(int32_t(r.city2)), f.add(int32_t(r.city3)), f.add(uint32_t(r.caravan_flags)), f.add(int32_t(r.o));
				// the route's road and its suspended search (economy-spec 6.4)
				f.add(uint32_t(r.road.size())), f.add(int32_t(r.road_size)), f.add(int32_t(r.making_road)), f.add(int32_t(r.reset_road));
				for (const PathData& pd : r.road) f.add(int32_t(pd.x)), f.add(int32_t(pd.y)), f.add(uint32_t(pd.flags));
				if (r.trees) f.add(int32_t(r.search_dir0)), f.add(int32_t(r.search_gx)), f.add(int32_t(r.search_gy));
			}
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
	PART("prod");
	for (uint16_t m : g.world.tdata) f.add(uint32_t(m));
	// Unit occupancy bits.
	for (size_t i = 0; i < g.world.wdata.size(); i++) {
		const int32_t b = g.world.wdata[i].block;
		if (b < 0) continue;
		f.add(uint32_t(i));
		const CollBlock& cb = g.world.coll_blocks[size_t(b)];
		for (size_t k = 0; k < 32; k++) f.add(uint32_t(cb.bytes[k]));
	}
	// Aircraft (docs/aircraft-spec.md): fuel burned, the air order fields, the guys' bank / pitch and the containers'
	// launch timer / list. Added only when some aircraft state exists, so scenarios without air objects keep their digests.
	{
		Fnv a;
		bool any = false;
		auto fl = [&a](float v) {
			uint32_t bits;
			std::memcpy(&bits, &v, 4);
			a.add(bits);
		};
		for (int32_t who = 0; who < kNumLeaders; who++) {
			const ObjectList& L = g.objects[size_t(who)];
			for (int32_t o = 0; o < L.unit_mark; o++) {
				const Unit* u = g.unit(who, o);
				if (!u || !u->alive()) continue;
				bool air = u->mana_burn != 0 && u->ptype->domain == kDomainAir;  // (birds are domain air too: only fuel / orders count)
				for (const Order& o2 : u->orders) air = air || order_is_air(o2.type);
				for (const Guy& gy : u->guys) air = air || gy.bank != 0.0f || gy.pitch != 0.0f || gy.last_bank != 0.0f || gy.last_pitch != 0.0f;
				const bool cont = u->launch_timer != 0 || !u->launch_list.empty();
				if (!air && !cont) continue;
				any = true;
				a.add(who), a.add(o), a.add(int32_t(u->mana_burn)), a.add(int32_t(u->launch_timer));
				for (int32_t l : u->launch_list) a.add(l);
				for (const Order& o2 : u->orders) {
					if (!order_is_air(o2.type) && o2.type != kSpecialAnim) continue;
					a.add(int32_t(o2.type)), a.add(o2.cruising_alt), a.add(o2.sharp_turn), a.add(o2.old), a.add(o2.returning);
					a.add(o2.oxx), a.add(o2.whose), a.add(o2.xx), a.add(o2.yy), a.add(o2.wp_index), a.add(uint32_t(o2.wp.size()));
					for (const auto& w : o2.wp) a.add(w[0]), a.add(w[1]);
					a.add(o2.sa_type), a.add(o2.sa_started), a.add(o2.sa_frames), a.add(o2.sa_data1), a.add(o2.sa_data2);
					a.add(o2.sa_data3), a.add(o2.sa_data4);
				}
				for (const Guy& gy : u->guys) fl(gy.bank), fl(gy.last_bank), fl(gy.pitch), fl(gy.last_pitch), a.add(gy.last_z);
			}
			for (int32_t o = kBuildBase; o < L.build_mark; o++) {
				const Object* b = g.object(who, o);
				if (!b || !b->alive() || (b->launch_timer == 0 && b->launch_list.empty())) continue;
				any = true;
				a.add(who), a.add(o), a.add(int32_t(b->launch_timer));
				for (int32_t l : b->launch_list) a.add(l);
			}
		}
		static const bool no_air = std::getenv("RON_NO_AIR_DIGEST") != nullptr;  // compare against a build without the block
		if (any && !no_air) f.add(uint32_t(a.h)), f.add(uint32_t(a.h >> 32));
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
	int32_t air_every = 0;   // --air-every K (aircraft and their containers: air_describe)
	int32_t heroes_every = 0;  // --heroes-every K (heroes summary)
	int32_t spells_every = 0;  // --spells-every K[:WHO] (spies, special forces, packers, merchants, casting units; WHO: every unit of that leader)
	int32_t spells_all = -1;
	int32_t starting_resources = -1;  // --starting-resources N (GameInfo setting before loading)
	int32_t difficulty = -1;          // --difficulty N (GameInfo difficulty, the lobby setting; 0 easiest .. 5)
	bool scripts = true;              // --no-scripts: no scenario / general powers / AI production scripts
	bool script_log = false;          // --script-log: script run-time messages and prints
	std::string scenario_path;        // the scenario file (the scenario script is <dir>/<base>.bhs)
	// Save / load (sim/save.hpp).
	std::vector<std::pair<int32_t, std::string>> save_at;  // --save-at FRAME:path
	std::string load_path;            // --load path: start from a save instead of loading the scenario
	const ron::sim::RandomGameSetup* random_setup = nullptr;  // --random STYLE,SIZE,SEED,NATIONS: a generated game instead of the scenario (all seats AI)
	std::string save_scenario;        // the scenario path as a save records it (game-relative)
	uint32_t save_seed = 0;           // the Game::load seed a save records
	int32_t roundtrip_at = -1;        // the --roundtrip pass: save at this frame to roundtrip_file
	std::string roundtrip_file;
	bool capture_final = false;       // keep the full saved state of the last frame (the round-trip comparison)
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

// What a save must carry of the scripts (docs/save-format.md 6): the ScenarioData switches, the timers, the objectives, the trigger bits
// and the static variables of every compiled script, as counts and one hash (the --roundtrip check compares the game that was saved
// with the game that was loaded).
std::string script_state_summary(const Game& g) {
	if (!g.scripts) return "no scripts";
	const ScriptHost& h = *g.scripts;
	uint64_t hv = 1469598103934665603ull;
	auto mix = [&](uint64_t x) { hv = (hv ^ x) * 1099511628211ull; };
	int statics = 0, live_statics = 0, trig_on = 0, trig_all = 0;
	std::function<void(const ron::bhs::Value*, int)> val = [&](const ron::bhs::Value* v, int depth) {
		if (!v || depth > 8) {
			mix(0);
			return;
		}
		mix(uint64_t(v->cls) << 8 | uint64_t(v->scope));
		mix(uint64_t(uint32_t(v->i)));
		uint32_t fb;
		std::memcpy(&fb, &v->f, 4);
		mix(fb);
		for (char16_t c : v->s) mix(c);
		for (const ron::bhs::VP& e : v->elems) val(e.get(), depth + 1);
	};
	for (const auto& fp : h.rt.files) {
		if (!fp) continue;
		for (const ron::bhs::RtScript& rs : fp->scripts) {
			for (uint8_t b : rs.trigger_bits) {
				trig_all++;
				trig_on += b ? 1 : 0;
				mix(b);
			}
			for (const ron::bhs::VP& v : rs.static_vars) {
				statics++;
				live_statics += v ? 1 : 0;
				val(v.get(), 0);
			}
		}
	}
	int done = 0;
	for (const ScriptObjective& o : h.pres.objectives) {
		done += o.completed ? 1 : 0;
		for (char c : o.id) mix(uint8_t(c));
		mix(uint64_t(uint32_t(o.completed)));
	}
	for (const auto& t : h.timers.list) {
		for (char16_t c : t.name) mix(c);
		mix(uint64_t(uint32_t(t.expiry)));
	}
	int goals = 0;
	for (const auto& gl : g.scen.goals) goals += int(gl.size());
	for (int32_t c : h.find_counters) mix(uint64_t(uint32_t(c)));
	char buf[256];
	std::snprintf(buf, sizeof buf, "%zu timers, %d goals, %zu objectives (%d done), %d static vars (%d set), %d of %d triggers on, hash %016llx",
	              h.timers.list.size(), goals, h.pres.objectives.size(), done, statics, live_statics, trig_on, trig_all,
	              (unsigned long long)hv);
	return buf;
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
	int32_t final_frame = 0, start_frame = 0;
	std::vector<uint8_t> final_state;  // save_game_bytes of the last frame (FogOpts::capture_final)
	std::unique_ptr<Game> final_game;  // that game (to name the field of a difference)
	bool fog_failed = false;
	bool nuke_failed = false;  // --nuke: a check failed
	int64_t saved_bytes = 0;  // size of the round-trip save file
	bool roundtrip_saved = false;
	int32_t roundtrip_frame = 0;  // the frame it was taken at
	size_t roundtrip_iter = 0;    // loop iterations (digests) done before it
	std::string script_at_save, script_after_load;  // script_state_summary of the game when it was saved / right after it was loaded
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

// Heroes summary (--heroes-every K): per leader its heroes (radius in tiles, running powers with their expiry), the units
// under cover (UNIT_UNDER_COVER) and the decoys.
void print_heroes(const Game& g) {
	for (int32_t who = 0; who < 8; who++) {
		const Leader& l = g.leaders[size_t(who)];
		if (!(l.flags & leaderflag::kValid)) continue;
		int32_t cover = 0, decoys = 0;
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (!u || !u->alive()) continue;
			if (u->unit_masks & umask::kUnderCover) cover++;
			if ((u->unit_masks & umask::kDecoy) && u->o_up < 0) decoys++;
		}
		if (l.num_heroes == 0 && cover == 0 && decoys == 0) continue;
		std::printf("  f%-5d heroes L%d: %d (flags ambush %d march %d) under_cover %d decoys %d\n", g.frame, who, l.num_heroes,
		            (l.flags & 0x4000) ? 1 : 0, (l.flags & 0x8000) ? 1 : 0, cover, decoys);
		for (int32_t i = 0; i < l.num_heroes && size_t(i) < l.heroes.size(); i++) {
			const Caster& h = l.heroes[size_t(i)];
			if (!(h.hero_flags & 1)) continue;
			const Unit* u = g.unit(who, h.o);
			std::printf("    hero %d:%-4d %-14s mana_burn %d/%d radius %d tiles masks %08x spells", who, h.o,
			            u ? type_name(g, u->type).c_str() : "?", u ? int(u->mana_burn) : 0, u ? unit_mana(g, *u) : 0,
			            hero_get_radius(g, h), u ? u->unit_masks : 0u);
			for (const ActiveSpell& s : h.active_spells) std::printf(" [0x%x f%d..f%d]", unsigned(s.t), s.start, s.frame);
			std::printf("\n");
		}
	}
}

// --spells-every K (docs/spells-spec.md): the units a craft can touch. Per leader: special forces (spies, commandos), packers,
// merchants, every unit with a CAST_SPELL order or a cloak / casting bit, and every building a spy has planted; with
// --spells-all WHO every unit and building of that leader.
void print_spells(const Game& g, int32_t all_who) {
	for (int32_t who = 0; who < 8; who++) {
		const Leader& l = g.leaders[size_t(who)];
		if (!(l.flags & leaderflag::kValid)) continue;
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (!u || !u->alive()) continue;
			const Order* f = u->front();
			const bool cast = f && f->type == kCastSpell;
			const bool interesting = (u->ptype->unit_flags2 & (0x10 | 4)) || unit_is_merchant(*u) ||
			                         (u->unit_masks & (umask::kPacked | umask::kCasting | umask::kUnderCover | umask::kBlowCover)) || cast ||
			                         who == all_who;
			if (!interesting) continue;
			std::printf("  f%-5d %d:%-4d %-18s (%6d,%6d) hits %d/%d mana_burn %d masks %08x/%x%s%s", g.frame, who, o,
			            type_name(g, u->type).c_str(), u->x, u->y, obj_hits(g, *u) - u->damage, obj_hits(g, *u), int(u->mana_burn),
			            u->unit_masks, u->unit_masks2, (u->unit_masks & umask::kPacked) ? " PACKED" : "", u->on_map() ? "" : " inside");
			if (cast)
				std::printf(" CAST 0x%x target %d:%d paid %d time %d", unsigned(f->spell), int(f->whom), int(f->ox), int(f->paid), int(u->spell_time));
			else if (f)
				std::printf(" order %d", int(f->type));
			if (u->orders.size() > 1) std::printf(" (+%d)", int(u->orders.size() - 1));
			std::printf("\n");
		}
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			const Build* b = g.build(who, o);
			if (!b || !b->alive() || (b->infiltrated == 0 && who != all_who)) continue;
			std::printf("  f%-5d %d:%-4d %-18s (%6d,%6d) infiltrated by mask %02x hits %d/%d\n", g.frame, who, o, type_name(g, b->type).c_str(),
			            b->x, b->y, unsigned(uint8_t(b->infiltrated)), obj_hits(g, *b) - b->damage, obj_hits(g, *b));
		}
		if (l.units_bribed || l.buildings_razed)
			std::printf("  f%-5d L%d units_bribed %d units_killed %d buildings_razed %d score_combat %d\n", g.frame, who, int(l.units_bribed),
			            l.units_killed, l.buildings_razed, l.score_combat);
	}
}

// --spell-fixtures (docs/spells-spec.md section 9, acceptance 1 and 2): is_castable and get_job_time on objects made for the
// first two valid leaders of the loaded scenario. Prints one line per check and the failure count.
int run_spell_fixtures(Game& g, bool verbose) {
	int fails = 0, total = 0;
	auto check = [&](const char* what, int32_t got, int32_t want) {
		total++;
		if (got != want) fails++;
		if (got != want || verbose) std::printf("  %s %-58s got %d, want %d\n", got == want ? "ok  " : "FAIL", what, got, want);
	};
	int32_t me = -1, other = -1;
	for (int32_t w = 0; w < 8; w++) {
		if (!(g.leaders[size_t(w)].flags & leaderflag::kValid)) continue;
		if (me < 0) me = w;
		else if (other < 0) other = w;
	}
	if (me < 0 || other < 0) {
		std::printf("spell fixtures: the scenario needs two leaders\n");
		return 1;
	}
	// A spot on own land and on the other leader's land (a city centre of each).
	auto city_pos = [&](int32_t w, int32_t* x, int32_t* y, int32_t* co) {
		for (int32_t c = 0; c < g.leaders[size_t(w)].city_mark && size_t(c) < g.cities[size_t(w)].size(); c++) {
			const City& C = g.cities[size_t(w)][size_t(c)];
			if (!C.valid()) continue;
			*x = C.x, *y = C.y, *co = c;
			return true;
		}
		return false;
	};
	int32_t mx = 0, my = 0, mc = -1, ox = 0, oy = 0, oc = -1;
	if (!city_pos(me, &mx, &my, &mc) || !city_pos(other, &ox, &oy, &oc)) {
		std::printf("spell fixtures: the scenario needs a city for each of the first two leaders\n");
		return 1;
	}
	auto mk = [&](const char* name, int32_t who, int32_t x, int32_t y) {
		const int32_t t = find_type(g, name);
		if (t < 0) {
			std::printf("  FAIL unknown type %s\n", name);
			fails++;
			return -1;
		}
		return init_unit(g, who, t, x, y, -1, -1, -1);
	};
	const int32_t spy = mk("Spy", me, mx + 200, my), cat = mk("Catapult", me, mx + 400, my), kat = mk("Katyusha Rocket", me, mx + 600, my),
	              rocket = mk("Rocket Artillery", me, mx + 800, my), hmg = mk("Heavy Machine Gun", me, mx + 1000, my),
	              mg = mk("Machine Gun", me, mx + 1200, my), commando = mk("Commando", me, mx + 1400, my),
	              citizen = mk("Citizen", me, mx + 1600, my), merchant = mk("Merchant", me, mx + 1800, my);
	if (spy < 0 || cat < 0 || citizen < 0) return 1;
	std::printf("spell fixtures on %s (leader %d / %d)\n", "the loaded scenario", me, other);
	auto cast = [&](int32_t sp, int32_t o, int32_t p3 = 0) { return spell_is_castable(g, sp, o, me, p3); };
	// the owner chain and the decoy rule
	check("BRIBE by a spy", cast(0x275, spy), 3);
	check("BRIBE by a catapult (FROM mismatch)", cast(0x275, cat), 0);
	check("SABOTAGE by a commando", cast(0x280, commando), 3);
	Unit* decoy = g.unit(me, spy);
	decoy->unit_masks |= umask::kDecoy;
	check("BRIBE by a decoy spy", cast(0x275, spy), 0);
	decoy->unit_masks &= ~umask::kDecoy;
	// packers: packed / unpacked x the pack spells
	struct PK { int32_t o; int32_t pack, unpack; const char* n; } pk[] = {{cat, 0x28b, 0x28c, "catapult"}, {mg, 0x28d, 0x28e, "machine gun"},
	                                                                       {merchant, 0x28f, 0x290, "merchant"}};
	for (const PK& k : pk) {
		if (k.o < 0) continue;
		Unit* u = g.unit(me, k.o);
		u->unit_masks &= ~umask::kPacked;
		char w[96];
		std::snprintf(w, sizeof w, "PACK %s unpacked", k.n);
		check(w, cast(k.pack, k.o), 3);
		std::snprintf(w, sizeof w, "UNPACK %s unpacked", k.n);
		check(w, cast(k.unpack, k.o), 0);
		u->unit_masks |= umask::kPacked;
		std::snprintf(w, sizeof w, "PACK %s packed", k.n);
		check(w, cast(k.pack, k.o), 0);
		std::snprintf(w, sizeof w, "UNPACK %s packed", k.n);
		check(w, cast(k.unpack, k.o), 3);
		u->unit_masks &= ~umask::kPacked;
	}
	// TO ARMS: without / with the militia techs, on own / foreign land
	{
		const int32_t mil = tribe_can_type(g, me, 66) ? 66 : get_graft(g, me, 66);
		const int32_t mm = tribe_can_type(g, me, 67) ? 67 : get_graft(g, me, 67);
		const int32_t pa = tribe_can_type(g, me, 68) ? 68 : get_graft(g, me, 68);
		const bool had = g.leaders[size_t(me)].has_tech_bit(mil), had2 = g.leaders[size_t(me)].has_tech_bit(mm), had3 = g.leaders[size_t(me)].has_tech_bit(pa);
		g.leaders[size_t(me)].set_tech_bit(mil, false), g.leaders[size_t(me)].set_tech_bit(mm, false), g.leaders[size_t(me)].set_tech_bit(pa, false);
		check("TO ARMS without a militia tech", cast(0x294, citizen), 0);
		g.leaders[size_t(me)].set_tech_bit(mil, true);
		g.unit(me, citizen)->x = mx, g.unit(me, citizen)->y = my;
		check("TO ARMS with the tech on own land", cast(0x294, citizen), 3);
		g.unit(me, citizen)->x = ox, g.unit(me, citizen)->y = oy;
		check("TO ARMS with the tech on enemy land", cast(0x294, citizen), 0);
		g.leaders[size_t(me)].set_tech_bit(mil, had), g.leaders[size_t(me)].set_tech_bit(mm, had2), g.leaders[size_t(me)].set_tech_bit(pa, had3);
		g.unit(me, citizen)->x = mx + 1600, g.unit(me, citizen)->y = my;
	}
	// cities: DISBAND / DEPOPULATE on a city centre
	{
		City& C = g.cities[size_t(me)][size_t(mc)];
		Build* B = g.build(me, C.o);
		const uint16_t saved = C.city_flags;
		const uint8_t saved_was = C.was_capital_flags;
		C.city_flags = uint16_t(C.city_flags & ~cityflag::kCapital);
		C.was_capital_flags = 0;
		check("DISBAND on a city centre", spell_is_castable(g, 0x29a, C.o, me, 0), 3);
		C.city_flags |= cityflag::kCapital;
		check("DISBAND on the capital", spell_is_castable(g, 0x29a, C.o, me, 0), 0);
		C.city_flags = uint16_t(C.city_flags & ~cityflag::kCapital);
		// a former capital of an active rival
		C.was_capital_flags = uint8_t(1u << other);
		check("DISBAND on a former capital of an active rival", spell_is_castable(g, 0x29a, C.o, me, 0), 0);
		C.was_capital_flags = 0;
		check("DEPOPULATE with nothing queued (DISBAND rule)", spell_is_castable(g, 0x286, C.o, me, 0), 3);
		B->build_queue.set_queue(B->queued, 0x286, nullptr, false);
		B->queued++;
		check("DEPOPULATE with a DEPOPULATE queued (p3 0)", spell_is_castable(g, 0x286, C.o, me, 0), 1);
		B->queued--;
		B->build_queue.un_queue(B->queued, B->queued + 1);
		// a plain building with a DISBAND queued: p3 0 -> 0, p3 1 -> 3
		for (int32_t o = C.o; o >= 0;) {
			Build* nb = g.build(me, o);
			if (!nb) break;
			const int32_t next = nb->city_down;
			if (!(nb->flags & objflag::kCity) && nb->alive() && (nb->flags & objflag::kActive) && !(nb->ptype->build_flags & 0x10) && nb->queued < nb->build_queue.queue_size) {
				check("DISBAND on a building, nothing queued (p3 0)", spell_is_castable(g, 0x29a, nb->o, me, 0), 3);
				nb->build_queue.set_queue(nb->queued, 0x29a, nullptr, false);
				nb->queued++;
				check("DISBAND with a DISBAND queued (p3 0)", spell_is_castable(g, 0x29a, nb->o, me, 0), 0);
				check("DISBAND with a DISBAND queued (p3 1)", spell_is_castable(g, 0x29a, nb->o, me, 1), 3);
				nb->queued--;
				nb->build_queue.un_queue(nb->queued, nb->queued + 1);
				break;
			}
			o = next;
		}
		C.city_flags = saved;
		C.was_capital_flags = saved_was;
		check("LOCK GATE", cast(0x288, spy), 0);
		check("UNLOCK GATE", cast(0x289, spy), 0);
	}
	// job times
	auto jt = [&](int32_t sp, int32_t o) { return spell_job_time(g, sp, o, me); };
	check("job time PACK catapult", jt(0x28b, cat), 80);
	if (kat >= 0) check("job time PACK Katyusha", jt(0x28b, kat), 40);
	if (rocket >= 0) check("job time PACK Rocket Artillery", jt(0x28b, rocket), 20);
	if (hmg >= 0) check("job time PACK Heavy Machine Gun", jt(0x28d, hmg), 25);
	check("job time BRIBE", jt(0x275, spy), 100);
	{
		const bool had = g.leaders[size_t(me)].has_tech_bit(0x304);
		g.leaders[size_t(me)].set_tech_bit(0x304, true);
		check("job time BRIBE with SPIES_CRAFT_FASTER", jt(0x275, spy), has_preq(g, me, 0x304) ? 50 : 100);
		g.leaders[size_t(me)].set_tech_bit(0x304, had);
	}
	check("job time SABOTAGE", jt(0x280, commando), has_tribe_bonus(g, me, 9) ? 75 : 150);
	check("job time SNIPER", jt(0x281, commando), has_tribe_bonus(g, me, 9) ? 30 : 60);
	check("range BRIBE (spy, no upgrade)", spell_get_range(g, 0x275, me, -1, -1), 6 * 192 + get_spy_upgrade(g, me) * RON_K(g, "spy_bribe_upgrade_range") * 192);
	// ---- acceptance 3-5: the cast bodies (docs/spells-spec.md section 9)
	{
		Leader& LM = g.leaders[size_t(me)];
		Unit* sp = g.unit(me, spy);
		// BRIBE refunds exactly min(mana_burn, MANA) on the own object, on an ally (teams unlocked), on a CEO-less... see below
		sp->mana_burn = 1000;
		cast_bribe(g, 0x275, spy, me, citizen, me);
		check("BRIBE on an own object refunds the mana", sp->mana_burn, 0);
		sp->mana_burn = 300;
		cast_bribe(g, 0x275, spy, me, citizen, me);
		check("BRIBE refund is min(mana_burn, MANA)", sp->mana_burn, 0);
		const int32_t o_inf = mk("Infantry", other, ox + 400, oy);
		const int32_t d0 = g.leaders[size_t(other)].diplos(me), d1 = g.leaders[size_t(me)].diplos(other);
		const uint8_t ts = g.info.team_style;
		g.leaders[size_t(other)].diplos(me);  // (read only)
		g.leaders[size_t(me)].diplo[size_t(other)] = 2;
		g.leaders[size_t(other)].diplo[size_t(me)] = 2;
		g.info.team_style = 0;  // teams unlocked
		sp->mana_burn = 1000;
		cast_bribe(g, 0x275, spy, me, o_inf, other);
		check("BRIBE on an ally with teams unlocked refunds", sp->mana_burn, 0);
		g.info.team_style = ts;
		g.leaders[size_t(me)].diplo[size_t(other)] = d1;
		g.leaders[size_t(other)].diplo[size_t(me)] = d0;
		// a real bribe: war, the unit keeps its damage ratio, units_bribed + 1
		g.leaders[size_t(me)].diplo[size_t(other)] = 0;
		g.leaders[size_t(other)].diplo[size_t(me)] = 0;
		Unit* ev = g.unit(other, o_inf);
		ev->damage = obj_hits(g, *ev) / 2;
		const int32_t ratio_before = (ev->damage << 8) / obj_hits(g, *ev);
		const int32_t bribed0 = LM.units_bribed, marks = g.objects[size_t(me)].unit_mark;
		cast_bribe(g, 0x275, spy, me, o_inf, other);
		check("BRIBE counts units_bribed", LM.units_bribed - bribed0, 1);
		check("BRIBE kills the old unit", g.unit(other, o_inf) && g.unit(other, o_inf)->alive() ? 1 : 0, 0);
		int32_t nu = -1;
		(void)marks;
		for (int32_t o = 0; o < g.objects[size_t(me)].unit_mark; o++)
			if (const Unit* u = g.unit(me, o); u && u->alive() && u->o_up < 0 && u->type == g.type(find_type(g, "Infantry")).type && u->damage != 0 &&
			                                   (u->damage << 8) / obj_hits(g, *u) == ratio_before)
				nu = o;
		if (nu >= 0) {
			const Unit& U = *g.unit(me, nu);
			check("BRIBE: the new unit belongs to the briber", U.who, me);
			check("BRIBE: hit ratio kept (1/256)", (U.damage << 8) / obj_hits(g, U), ratio_before);
		} else {
			check("BRIBE: the new unit exists", 0, 1);
		}
		// SABOTAGE on a building: half the hits (at least COMMANDO_MIN_DAMAGE), SNIPER on infantry: die(2) + counters
		Build* tb = nullptr;
		for (int32_t o = kBuildBase; o < g.objects[size_t(other)].build_mark && !tb; o++) {
			Build* b = g.build(other, o);
			if (b && b->alive() && (b->flags & objflag::kActive) && !(b->flags & objflag::kCity) && obj_hits(g, *b) >= 1000) tb = b;
		}
		if (tb) {
			tb->damage = obj_hits(g, *tb) - 1000;
			cast_sabotage(g, 0x280, commando, me, tb->o, other);
			check("SABOTAGE on a 1000-hit building: damage 500", obj_hits_left(g, *tb), 500);
			tb->damage = obj_hits(g, *tb) - 100;
			const int32_t razed0 = LM.buildings_razed;
			const int32_t tb_o = tb->o;
			cast_sabotage(g, 0x280, commando, me, tb_o, other);
			check("SABOTAGE on a 100-hit building (floor 200) destroys it", g.build(other, tb_o) && g.build(other, tb_o)->alive() ? 1 : 0, 0);
			check("SABOTAGE counts buildings_razed", LM.buildings_razed - razed0, 1);
		} else {
			std::printf("  (no 1000-hit building of leader %d for the SABOTAGE checks)\n", other);
		}
		const int32_t o_inf2 = mk("Heavy Machine Gun", other, ox + 600, oy);  // (a single-member chain: the counters need curr_uber_size == 1)
		const int32_t kills0 = LM.units_killed, deaths0 = g.leaders[size_t(other)].deaths_current_frame, kc0 = LM.kills_current_frame,
		              score0 = LM.score_combat, lost0 = g.leaders[size_t(other)].units_lost;
		const int32_t kv = get_kill_value(g, g.unit(other, o_inf2)->type, other) / 5;
		cast_sabotage(g, 0x281, commando, me, o_inf2, other);
		check("SNIPER kills infantry", g.unit(other, o_inf2) && g.unit(other, o_inf2)->alive() ? 1 : 0, 0);
		check("SNIPER: units_killed + 1", LM.units_killed - kills0, 1);
		check("SNIPER: deaths_current_frame (+0xa58) + 1", g.leaders[size_t(other)].deaths_current_frame - deaths0, 1);
		check("SNIPER: kills_current_frame (+0xa5a) + 1", LM.kills_current_frame - kc0, 1);
		check("SNIPER: score_combat += kill value / 5", LM.score_combat - score0, kv);
		(void)lost0;
		// COUNTERINTEL: an enemy spy dies without units_lost; the plants on own objects are cleared. INFORMER plants one.
		const int32_t o_spy = mk("Spy", other, ox + 800, oy);
		const int32_t kills1 = LM.units_killed, lost1 = g.leaders[size_t(other)].units_lost;
		cast_counterintel(g, spy, me, o_spy, other);
		check("COUNTERINTEL kills an enemy spy", g.unit(other, o_spy) && g.unit(other, o_spy)->alive() ? 1 : 0, 0);
		check("COUNTERINTEL: units_killed + 1", LM.units_killed - kills1, 1);
		check("COUNTERINTEL: no units_lost for the victim", g.leaders[size_t(other)].units_lost - lost1, 0);
		Build* mb = g.build(me, g.cities[size_t(me)][size_t(mc)].o);
		mb->infiltrated = int8_t(1u << other);
		cast_counterintel(g, spy, me, mb->o, me);
		check("COUNTERINTEL clears the plants on an own object", int(mb->infiltrated), 0);
		Build* eb = g.build(other, g.cities[size_t(other)][size_t(oc)].o);
		cast_double_agent(g, spy, me, eb->o, other);
		check("INFORMER sets the plant bit of the caster", (eb->infiltrated >> me) & 1, 1);
		check("INFORMER: the building is seen by the caster", object_is_seen(g, *eb, me, false) ? 1 : 0, 1);
	}
	std::printf("spell fixtures: %d checks, %d failed\n", total, fails);
	return fails;
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

// One compact line per AI seat (--ai-every K, after the long print): age, citizens / military / pop cap, stockpiles, income
// rates (per 450 frames), the research queue, the units queued, the production script state and the AI's need bits.
void print_ai_brief(const Game& g) {
	static const char* kG = "ftwkmo";
	for (int32_t who = 0; who < 8; who++) {
		const Leader& L = g.leaders[size_t(who)];
		if (!(L.flags & leaderflag::kValid) || (L.flags & 0xc) == leaderflag::kHuman) continue;
		std::string rq;
		int32_t uq = 0;
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			const Build* b = g.build(who, o);
			if (!b || !b->alive() || !(b->flags & objflag::kActive)) continue;
			for (int32_t i = 0; i < b->queued; i++) {
				const int32_t v = b->build_queue.get_queue(i);
				if (is_tech_type(v)) rq += (rq.empty() ? "" : ",") + type_name(g, v) + (i == 0 ? "*" : "");
				else uq++;
			}
		}
		std::printf("  aib f%-5d L%d age %d cit %d mil %d units %d pop %d/%d | stock", g.frame, who, L.age, L.peasants, L.combat,
		            L.active, L.control, L.pop_cap);
		for (int32_t k = 0; k < 6; k++) std::printf(" %c%d", kG[k], L.bucket[size_t(k)]);
		std::printf(" | inc");
		for (int32_t k = 0; k < 6; k++) std::printf(" %c%d", kG[k], L.income[size_t(k)] / 16);
		std::printf(" | esc");
		for (int32_t k = 0; k < 6; k++) std::printf(" %d", L.escrow[size_t(k)]);
		std::printf(" | lib %d/%d/%d/%d tot %d need %d", L.epoch[0], L.epoch[1], L.epoch[2], L.epoch[3], L.epochs,
		            L.age < 7 ? techs_per_age(g, who, kAges + L.age) : 0);
		std::printf(" | econ");
		for (int32_t k = 0; k < 6; k++) std::printf(" %d", L.econ[size_t(k)]);
		int32_t trade = 0, routes = 0;  // caravan income (City::trade_val) and the routes' links of this seat's cities
		for (int32_t c = 0; c < L.city_mark && size_t(c) < g.cities[size_t(who)].size(); c++) {
			trade += g.cities[size_t(who)][size_t(c)].trade_val;
			routes += int32_t(g.cities[size_t(who)][size_t(c)].vans.size());
		}
		std::printf(" | strat st %d script %s on %d step %d make0 %s/%d | cities %d unitq %d caravans %d merch %d schol %d trade %d routes %d | research [%s]\n", L.production_ai,
		            L.prod_script.c_str(), L.prod_script_on, L.prod_step,
		            L.make_list[0].t >= 0 ? type_name(g, L.make_list[0].t).c_str() : "-", L.make_list[0].val, L.city_num, uq,
		            L.caras, L.merchants, L.scholars, trade, routes, rq.c_str());
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
	case 4: std::printf("  f%-5d GAME OVER%s\n", e.frame, e.value == 1 ? " (allied victory: end-game screen pending)" : ""); break;
	case 5: std::printf("  f%-5d CAPITAL LOST L%d (held by L%d)\n", e.frame, e.who, e.other); break;
	case 6: std::printf("  f%-5d CAPITAL RECAPTURED L%d\n", e.frame, e.who); break;
	case 7: std::printf("  f%-5d BUILDING CAPTURED L%d:%d -> L%d:%d\n", e.frame, e.other, e.o, e.who, e.o2); break;
	case 17: std::printf("  f%-5d WONDER VICTORY CLOCK L%d: %d frames to go (wonder net %d)\n", e.frame, e.who, e.value, e.other); break;
	case 18: std::printf("  f%-5d WONDER VICTORY IN %d frames (L%d)\n", e.frame, e.value, e.who); break;
	case 19: std::printf("  f%-5d WONDER VICTORY CLOCK STOPPED (L%d)\n", e.frame, e.who); break;
	case 20: std::printf("  f%-5d TERRITORY CLOCK L%d: %d frames to go (%d%% needed)\n", e.frame, e.who, e.value, e.other); break;
	case 21: std::printf("  f%-5d TERRITORY VICTORY IN %d frames (L%d)\n", e.frame, e.value, e.who); break;
	case 22: std::printf("  f%-5d TERRITORY CLOCK STOPPED (L%d)\n", e.frame, e.who); break;
	case 23: std::printf("  f%-5d TIME LIMIT in %d ticks\n", e.frame, e.value); break;
	case 24: std::printf("  f%-5d MUSICAL CHAIRS ROUND (%d min): %d defeated\n", e.frame, e.value, e.other); break;
	case 25: std::printf("  f%-5d L%d %s\n", e.frame, e.who, e.value == 0 ? "RESIGNED" : e.value == 1 ? "QUIT" : "DROPPED"); break;
	case 26: std::printf("  f%-5d NUKE COUNT %d\n", e.frame, e.value); break;
	case 27: std::printf("  f%-5d L%d NUCLEAR MISSILE %d LAUNCHED from silo %d at (%d,%d)\n", e.frame, e.who, e.o, e.o2, e.x, e.y); break;
	case 28: std::printf("  f%-5d L%d NUCLEAR EXPLOSION at (%d,%d), radius %d\n", e.frame, e.who, e.x, e.y, e.value); break;
	case 29: std::printf("  f%-5d L%d MISSILE STOPPED by the missile defence of L%d at (%d,%d)\n", e.frame, e.who, e.other, e.x, e.y); break;
	default: break;
	}
}

// --perf: time Game::do_frame alone (no per-frame digest, no event / AI / economy printing) and print ms per frame (docs/ui-port.md section 16).
bool g_perf = false;
double g_perf_total_ms = 0, g_perf_max_ms = 0;

RunResult run(const ron::GameFiles& files, const ron::Scenario& sc, const Rules& rules, const AnimSource* anims,
              uint32_t seed, int32_t frames, const std::vector<Script>& scripts, int32_t dump_every,
              int32_t digest_every, bool print, const FogOpts& fo, int32_t combat_every = 0, int32_t prod_every = 0) {
	std::unique_ptr<Game> g;
	ScriptLoadReport srep;
	if (!fo.load_path.empty()) {
		SaveInfo sinfo;
		g = load_game(files, rules, anims, fo.load_path, &sinfo);
		srep.errors = sinfo.script_errors;
		if (g->scripts) g->scripts->verbose = fo.script_log;
		if (fo.capture_final) {  // the round trip: saving the loaded game again must give the file's own bytes
			const std::string why = describe_save_difference(ron::read_file(fo.load_path), save_game_bytes(*g, sinfo.scenario, sinfo.seed));
			if (!why.empty()) throw std::runtime_error("re-saving the loaded game gives different bytes: " + why);
		}
		if (print) std::printf("loaded %s: scenario %s, seed %u, frame %d tick %d\n", fo.load_path.c_str(), sinfo.scenario.c_str(),
		                       sinfo.seed, g->frame, g->tick);
	} else if (fo.random_setup) {
		g = ron::sim::new_random_game(files, rules, anims, *fo.random_setup);
		if (!g) throw std::runtime_error("the random map could not be generated");
	} else {
		g = std::make_unique<Game>();
		g->anims = anims;
		if (fo.reveal >= 0) g->info.reveal_map = uint8_t(fo.reveal);
		if (fo.starting_resources >= 0) g->info.starting_resources = uint8_t(fo.starting_resources);
		if (fo.difficulty >= 0) g->info.difficulty = uint8_t(fo.difficulty);
		g->load(files, sc, rules, seed);
		if (fo.scripts) {
			srep = script_host_load(*g, files, fo.scenario_path, sc);
			g->scripts->verbose = fo.script_log;
		}
	}
	size_t ev_seen = g->events.size();
	FogCheck fc;
	CommandManager cm;
	RunResult rr;
	if (!fo.load_path.empty()) rr.script_after_load = script_state_summary(*g);
	uint64_t script_seq = 0;
	size_t objectives_seen = 0;
	std::vector<std::pair<std::string, int32_t>> objectives_done;
	for (int32_t who = 0; who < kNumLeaders; who++) rr.initial_uids[size_t(who)] = g->objects[size_t(who)].obj_ctr;
	rr.start_frame = g->frame;
	const int32_t end_frame = g->frame + frames;
	// --save-at / the round-trip pass: the state at the start of a frame, before its scripted commands.
	std::vector<bool> saved_at(fo.save_at.size(), false);
	bool roundtrip_saved = false;
	auto write_saves = [&]() {
		for (size_t i = 0; i < fo.save_at.size(); i++) {
			if (saved_at[i] || fo.save_at[i].first != g->frame) continue;
			saved_at[i] = true;
			save_game(*g, fo.save_scenario, fo.save_seed, fo.save_at[i].second);
			if (print) std::printf("saved %s at frame %d\n", fo.save_at[i].second.c_str(), g->frame);
		}
		if (fo.roundtrip_at == g->frame && !roundtrip_saved) {
			roundtrip_saved = true;
			rr.roundtrip_saved = true;
			rr.roundtrip_frame = g->frame;
			rr.roundtrip_iter = rr.digests.size();
			rr.script_at_save = script_state_summary(*g);
			save_game(*g, fo.save_scenario, fo.save_seed, fo.roundtrip_file);
			rr.saved_bytes = int64_t(std::filesystem::file_size(fo.roundtrip_file));
		}
	};
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
		if (s.kind == Script::Garrison || s.kind == Script::Board) watch(s.twho, s.to);
		if (s.kind == Script::EjectAll || s.kind == Script::Transport || s.kind == Script::SetTransport || s.kind == Script::Board)
			for (int32_t o : s.objs) watch(s.who, o);
		if (s.kind == Script::Attack) {  // the attack target too
			bool known = false;
			for (const Tracked& t : rr.tracked) known |= t.who == s.twho && t.o == s.to;
			const Unit* u = g->unit(s.twho, s.to);
			if (!known && u) rr.tracked.push_back({s.twho, s.to, u->x, u->y, u->x, u->y, -1, s.frame, 0, u->x, u->y, 0});
		}
	}
	std::vector<NukeProbe> probes;
	for (int32_t f = 0; f < frames; f++) {
		write_saves();
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
			} else if (s.kind == Script::Transport) {
				cm.issue_transport(*g, s.objs);
				if (print) std::printf("  f%-5d TRANSPORT %d:%d%s\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0], s.objs.size() > 1 ? "+" : "");
			} else if (s.kind == Script::SetTransport) {
				cm.issue_set_transport(*g, s.objs, s.num != 0);
				if (print) std::printf("  f%-5d SET_TRANSPORT %d:%d%s = %d\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0],
				                       s.objs.size() > 1 ? "+" : "", s.num != 0);
			} else if (s.kind == Script::Cast) {
				const Object* cu = s.objs.empty() ? nullptr : g->object(s.who, s.objs[0]);
				const bool targeted = s.action == 1;
				const Object* tg = targeted ? g->object(s.twho, s.num) : nullptr;
				int32_t cx = cu ? cu->x : 0, cy = cu ? cu->y : 0;
				if (s.action == 2) cx = s.x, cy = s.y;
				else if (tg) cx = tg->x, cy = tg->y;
				// PACK / UNPACK name the artillery craft; the UI sends the variant of the selection's type (machine guns 0x28d / 0x28e,
				// merchants 0x28f / 0x290, fishermen 0x291 / 0x292), which the artillery craft's FROM would refuse.
				int32_t sp = s.to;
				if ((sp == 0x28b || sp == 0x28c) && cu && cu->is_unit) {
					const Unit& cuu = static_cast<const Unit&>(*cu);
					const int32_t add = sp == 0x28c ? 1 : 0;
					if (unit_is(*g, cuu, 0x7b)) sp = 0x28d + add;
					else if (cuu.type == 0x3d || cuu.type == 0x3e || cuu.type == 400) sp = 0x28f + add;
					else if (unit_is(*g, cuu, 0x13d)) sp = 0x291 + add;
				}
				cm.issue_spell(*g, s.objs, sp, targeted ? s.num : -1, targeted ? s.twho : -1, cx, cy);
				if (print) {
					std::printf("  f%-5d CAST %d:%d%s spell 0x%x", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0],
					            s.objs.size() > 1 ? "+" : "", unsigned(s.to));
					if (targeted) std::printf(" -> %d:%d", s.twho, s.num);
					else if (s.action == 2) std::printf(" at (%d,%d)", cx, cy);
					std::printf("\n");
				}
			} else if (s.kind == Script::Flight) {
				cm.issue_flight(*g, s.objs, s.to, s.twho, s.order, (s.num & 1) != 0, (s.num & 2) != 0, (s.num & 4) != 0);
				if (print) std::printf("  f%-5d FLIGHT %d:%d%s -> %s %d:%d\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0],
				                       s.objs.size() > 1 ? "+" : "", s.order == 1 ? "rebase onto" : "attack", s.twho, s.to);
			} else if (s.kind == Script::AirPatrol) {
				cm.issue_patrol(*g, s.objs, s.x, s.y, QueuePos(s.queue == 2 ? 2 : s.queue));
				if (print) std::printf("  f%-5d PATROL %d:%d%s -> (%d,%d)\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0],
				                       s.objs.size() > 1 ? "+" : "", s.x, s.y);
			} else if (s.kind == Script::LaunchPatrol) {
				cm.issue_launch_patrol(*g, s.objs, s.x, s.y, QueuePos(s.queue), (s.num & 1) != 0, (s.num & 2) != 0, (s.num & 4) != 0);
				if (print) std::printf("  f%-5d LAUNCH_PATROL %d:%d%s -> (%d,%d)\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0],
				                       s.objs.size() > 1 ? "+" : "", s.x, s.y);
			} else if (s.kind == Script::AttackGround) {
				cm.issue_attack_ground(*g, s.objs, s.x, s.y, QueuePos(s.queue));
				if (print) std::printf("  f%-5d ATTACK_GROUND %d:%d%s -> (%d,%d)\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0],
				                       s.objs.size() > 1 ? "+" : "", s.x, s.y);
			} else if (s.kind == Script::Hotkey) {
				if (s.action == 2) cm.issue_hotkey_bookmark(s.num, s.fx, s.fy, s.x);
				else cm.issue_hotkey(*g, s.objs, s.num, s.action == 1);
				if (print) std::printf("  f%-5d HOTKEY %d group %d%s\n", g->frame, s.who, s.num, s.action == 1 ? " clear" : s.action == 2 ? " bookmark" : "");
			} else if (s.kind == Script::ComeOut) {
				for (int32_t o : s.objs) cm.issue_come_out(*g, s.who, o);
				if (print) std::printf("  f%-5d COME_OUT %d:%d%s\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0], s.objs.size() > 1 ? "+" : "");
			} else if (s.kind == Script::Speed) {
				if (s.num == -100) cm.issue_speed_up();
				else if (s.num == -101) cm.issue_speed_down();
				else cm.issue_speed_set(s.num);
				if (print) std::printf("  f%-5d SPEED %s\n", g->frame, s.num == -100 ? "up" : s.num == -101 ? "down" : std::to_string(s.num).c_str());
			} else if (s.kind == Script::Pause) {
				cm.issue_pause(s.num != 0);
				if (print) std::printf("  f%-5d PAUSE %d\n", g->frame, s.num != 0);
			} else if (s.kind == Script::Chat) {
				const std::u16string tx = ron::utf8_to_utf16(s.text);
				cm.issue_chat(tx);
				if (print) std::printf("  f%-5d CHAT from %d: %s\n", g->frame, s.who, s.text.c_str());
			} else if (s.kind == Script::Rename) {
				for (int32_t o : s.objs) cm.issue_rename_city(*g, o, ron::utf8_to_utf16(s.text));
				if (print) std::printf("  f%-5d RENAME_CITY %d:%d -> %s\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0], s.text.c_str());
			} else if (s.kind == Script::Cheat) {
				const std::string& n = s.text;
				if (n == "viewall") cm.issue_cheat(Verb::CheatViewAll, s.who);
				else if (n == "givetechs") cm.issue_cheat(Verb::CheatGiveTechs, s.who);
				else if (n == "zerotechs") cm.issue_cheat(Verb::CheatZeroTechs, s.who);
				else if (n == "buckets") cm.issue_cheat(Verb::CheatIncreaseBuckets, s.who);
				else if (n == "zerobuckets") cm.issue_cheat(Verb::CheatZeroBuckets, s.who);
				else if (n == "aispeed") cm.issue_cheat(Verb::CheatAiSpeedIncrease, s.who);
				else if (n == "aispeednormal") cm.issue_cheat(Verb::CheatAiSpeedNormal, s.who);
				else if (n == "aitoggle") cm.issue_cheat(Verb::CheatAiToggle, s.who);
				else if (n == "unit") cm.issue_cheat_init_unit(s.who, s.num, s.x, s.y);
				else throw std::runtime_error("unknown --cheat " + n);
				if (print) std::printf("  f%-5d CHEAT %s (%d)\n", g->frame, n.c_str(), s.who);
			} else if (s.kind == Script::Nuke) {
				// A test aid: Ammo::do_damage's nuclear branch, fired by a NUCLEARMISSILE of the leader created at the impact (it dies at once, as the
				// real one does when it fires its warhead).
				NukeProbe p;
				p.who = s.who, p.x = s.x, p.y = s.y, p.frame = g->frame;
				p.o = init_unit(*g, s.who, 315, s.x, s.y);
				if (p.o < 0) throw std::runtime_error("--nuke: no room for a missile of leader " + std::to_string(s.who));
				const int32_t splash = g->type(315).splash_area;
				p.radius = splash * 192;
				p.arm0 = g->armageddon, p.used0 = g->leaders[size_t(s.who)].nukes_used, p.craters0 = int32_t(g->nukes.craters.size());
				p.heights = g->heights.h;
				for (int32_t w = 0; w < 8; w++) {
					for (int32_t o = 0; o < g->objects[size_t(w)].unit_mark; o++) {
						const Unit* u = g->unit(w, o);
						if (!u || !u->alive() || !u->on_map() || type_is(*u->ptype, 315, false)) continue;
						p.objs.push_back({w, o, vector_dist(s.x - u->x, s.y - u->y), u->damage});
					}
					for (int32_t o = kBuildBase; o < g->objects[size_t(w)].build_mark; o++) {
						const Build* b = g->build(w, o);
						if (!b || !b->alive()) continue;
						const int32_t dx = std::max(0, std::abs(s.x - b->x) - b->ptype->x_size * 96), dy = std::max(0, std::abs(s.y - b->y) - b->ptype->y_size * 96);
						p.objs.push_back({w, o, vector_dist(dx, dy), b->damage});
					}
				}
				nuke_detonate(*g, s.who, p.o, s.x, s.y, splash);
				if (Object* m = g->object(s.who, p.o); m && m->alive()) object_die(*g, *m, 0, -1, 0.0f);
				if (print) std::printf("  f%-5d NUKE %d:%d at (%d,%d), splash %d (radius %d): %zu objects measured, %zu tiles sunk\n", g->frame, s.who, p.o, s.x, s.y,
				                       splash, p.radius, p.objs.size(), g->nukes.craters.size() - size_t(p.craters0));
				probes.push_back(std::move(p));
			} else if (s.kind == Script::Scramble) {
				cm.issue_scramble(*g, s.objs);
				if (print) std::printf("  f%-5d SCRAMBLE %d:%d%s\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0], s.objs.size() > 1 ? "+" : "");
			} else if (s.kind == Script::Stock) {
				for (int32_t& v : g->leaders[size_t(s.who)].bucket) v = s.num;  // (a test aid, not a game command)
			} else if (s.kind == Script::Kill) {
				// A test aid, not a game command: the objects die at once (aircraft acceptance tests: a base lost in flight).
				for (int32_t o : s.objs)
					if (Object* ob = g->object(s.who, o); ob && ob->alive()) object_die(*g, *ob, 0, -1, 0.0f);
			} else if (s.kind == Script::Crash) {
				// A test aid: the aircraft die as a shot-down plane does (death 2 -> kill_guy -> the debris of Ammo::init_crash).
				for (int32_t o : s.objs)
					if (Object* ob = g->object(s.who, o); ob && ob->alive()) object_die(*g, *ob, 2, -1, 0.0f);
				if (print) std::printf("  f%-5d CRASH %d:%d%s\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0], s.objs.size() > 1 ? "+" : "");
			} else if (s.kind == Script::Flock) {
				// A test aid: Objects::add_flock at tile (x, y) as the forest and fort triggers call it.
				const int32_t before = g->objects[size_t(kGaiaWho)].unit_mark;
				add_flock(*g, s.x, s.y, s.who, s.num);
				if (print) std::printf("  f%-5d FLOCK %d birds at tile (%d,%d) for %d: %d new bird slots, rng %08x\n", g->frame, s.num, s.x, s.y, s.who,
				                       g->objects[size_t(kGaiaWho)].unit_mark - before, g->rng.seed());
			} else if (s.kind == Script::UnitMask) {
				cm.issue_unitmask(*g, s.objs, s.num);
				if (print) std::printf("  f%-5d UNITMASK %d:%d%s mask %x\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0], s.objs.size() > 1 ? "+" : "", s.num);
			} else if (s.kind == Script::Teleport) {
				// A test aid, not a game command: the units (and their followers) appear at (x, y) (spells acceptance tests).
				for (int32_t o : s.objs) {
					if (Unit* u = g->unit(s.who, o); u && u->alive()) {
						unit_set_new_location(*g, *u, s.x, s.y, true, true);
						if (print) std::printf("  f%-5d TELEPORT %d:%d -> (%d,%d)\n", g->frame, s.who, o, s.x, s.y);
					}
				}
			} else if (s.kind == Script::Recall) {
				cm.issue_recall(*g, s.objs);
				if (print) std::printf("  f%-5d RECALL %d:%d%s\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0], s.objs.size() > 1 ? "+" : "");
			} else if (s.kind == Script::Board) {
				cm.issue_board_ship(*g, s.objs, s.to, QueuePos(s.queue));
				if (print) std::printf("  f%-5d BOARD_SHIP %d:%d%s -> ship %d:%d\n", g->frame, s.who, s.objs.empty() ? -1 : s.objs[0],
				                       s.objs.size() > 1 ? "+" : "", s.twho, s.to);
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
		if (g_perf) {
			const auto t0 = std::chrono::steady_clock::now();
			g->do_frame();
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			g_perf_total_ms += ms, g_perf_max_ms = std::max(g_perf_max_ms, ms);
		} else {
			g->do_frame();
		}
		if (print && !g_perf) {
			for (; ev_seen < g->events.size(); ev_seen++) print_event(*g, g->events[ev_seen]);
			if (fo.script_log) print_script_events(*g, script_seq, objectives_seen, objectives_done);
		}
		if (fo.fog_check) fog_check_frame(*g, fc, print);
		const uint64_t d = g_perf && g->frame < end_frame ? 0 : digest(*g);  // --perf: only the last frame's digest
		rr.digests.push_back(d);
		for (NukeProbe& p : probes) p.arm_series.push_back(g->armageddon);
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
		if (print && !g_perf && digest_every > 0 && (g->frame % digest_every == 0 || g->frame == end_frame))
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
			int32_t live = 0, stuck = 0, corpses = 0, debris = 0, flock = 0;
			for (const Ammo& a : g->ammo) {
				if (a.flags & ammobits::kFlying) live++;
				else if (a.flags & ammobits::kStuck) stuck++;
				if ((a.flags & ammobits::kFlying) && a.gpiece == kCrashPiece) debris++;
			}
			for (const DeathObj& d : g->deaths) corpses += d.valid ? 1 : 0;
			for (int32_t o = 0; o < g->objects[size_t(kGaiaWho)].unit_mark; o++)
				if (const Unit* u = g->unit(kGaiaWho, o); u && u->alive() && u->type == 403) flock++;
			std::printf(" | ammo %d stuck %d corpses %d", live, stuck, corpses);
			if (debris) std::printf(" debris %d", debris);
			if (flock) std::printf(" flock birds %d", flock);
			std::printf("\n");
		}
		if (print && fo.econ_every > 0 && g->frame % fo.econ_every == 0) print_economy(*g);
		if (print && fo.ai_every > 0 && g->frame % fo.ai_every == 0) {
			print_ai(*g);
			print_ai_brief(*g);
		}
		if (print && fo.air_every > 0 && g->frame % fo.air_every == 0)
			for (int32_t who = 0; who < kNumLeaders; who++)
				for (const std::string& l : air_describe(*g, who)) std::printf("  f%-5d %s\n", g->frame, l.c_str());
		if (print && fo.heroes_every > 0 && g->frame % fo.heroes_every == 0) print_heroes(*g);
		if (print && fo.spells_every > 0 && g->frame % fo.spells_every == 0) print_spells(*g, fo.spells_all);
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
	if (g_perf) {
		const double n = double(std::max<int32_t>(1, g->frame - (end_frame - frames)));
		std::printf("perf: %d frames of Game::do_frame: %.3f ms/frame average, %.3f ms worst, %.1f s total (frame %d at the end)\n", int(n),
		            g_perf_total_ms / n, g_perf_max_ms, g_perf_total_ms / 1000.0, g->frame);
	}
	write_saves();
	for (const NukeProbe& p : probes) {  // --nuke: the damage radius, the terrain scorch, the Armageddon count and the embargo stamp
		const int32_t age = g->frame - p.frame;
		int32_t bad = 0, checks = 0, hit_in = 0, near_n = 0, far_n = 0;
		auto check = [&](const char* what, bool ok, const char* detail = "") {
			checks++;
			if (!ok) bad++;
			if (print) std::printf("  nuke %s: %s %s\n", ok ? "PASS" : "FAIL", what, detail);
		};
		if (age >= 40) {  // the fireball has reached its full radius (10 frames of delay, 30 of growth)
			bool inside_ok = true, outside_ok = true;
			for (const NukeProbe::Obj& e : p.objs) {
				const Object* ob = g->object(e.who, e.o);
				const bool dead = !ob || !ob->alive();
				const int32_t dmg = ob ? ob->damage : e.damage;
				if (e.dist * 10 < p.radius * 6) {  // within 60% of the radius the hit is at least 40% of a full one
					near_n++;
					if (!(dead || dmg > e.damage)) inside_ok = false;
					else hit_in++;
				} else if (e.dist > p.radius && e.damage == 0) {
					far_n++;
					if (dead || dmg != e.damage) outside_ok = false;
				}
			}
			check("everything within 60% of the radius is hurt or dead", inside_ok, (std::to_string(near_n) + " objects, " + std::to_string(hit_in) + " hurt").c_str());
			check("nothing beyond the radius is touched", outside_ok, (std::to_string(far_n) + " objects").c_str());
			// the scorch: only the 7 x 7 tiles around the impact change, by multiples of the depth 30
			const int32_t vxs = g->heights.vxs, cx = tcoord_of(p.x), cy = tcoord_of(p.y);
			int32_t lowered = 0, stray = 0, odd = 0;
			for (size_t i = 0; i < p.heights.size() && i < g->heights.h.size(); i++) {
				const float dh = p.heights[i] - g->heights.h[i];
				if (dh == 0.0f) continue;
				const int32_t vx = int32_t(i % size_t(vxs)), vy = int32_t(i / size_t(vxs));
				if (vx < cx - 3 || vx > cx + 4 || vy < cy - 3 || vy > cy + 4) stray++;
				const float k = dh / 30.0f;
				if (k != float(int(k)) || k < 1 || k > 4) odd++;
				lowered++;
			}
			check("the height grid changes only in the 7 x 7 tiles around the impact", stray == 0, (std::to_string(stray) + " stray vertices").c_str());
			check("every lowered vertex sank by whole multiples of 30", odd == 0 && (lowered > 0 || g->nukes.craters.size() == size_t(p.craters0)),
			      (std::to_string(lowered) + " vertices, " + std::to_string(g->nukes.craters.size() - size_t(p.craters0)) + " tiles").c_str());
		} else if (print) {
			std::printf("  nuke: only %d frames since the explosion (the damage radius needs 40): not checked\n", age);
		}
		check("the leader's nukes_used and embargo stamp", g->leaders[size_t(p.who)].nukes_used == p.used0 + 1 && g->nukes.stamp[size_t(p.who)] == p.frame);
		if (age >= 112) {  // Armageddon: one more, exactly 110 frames after the explosion began
			const int32_t expect = (g->info.starting_resources != 8 && (g->multiplayer || g->info.num_nations > 1)) ? 1 : 0;
			int32_t first = -1;
			for (size_t i = 0; i < p.arm_series.size(); i++)
				if (p.arm_series[i] != p.arm0) {
					first = int32_t(i);
					break;
				}
			char buf[96];
			std::snprintf(buf, sizeof buf, "(counted after the frame run %d frames past the detonation)", first);
			check("Armageddon counts the nuke once, 110 frames later", g->armageddon - p.arm0 == expect && (expect == 0 || first + 1 == 110 || first == 110), buf);
		} else if (print) {
			std::printf("  nuke: only %d frames since the explosion (Armageddon counts at 110): not checked\n", age);
		}
		if (bad) rr.nuke_failed = true;
		std::printf("nuke at (%d,%d) frame %d: %d checks, %d failed\n", p.x, p.y, p.frame, checks, bad);
	}
	if (fo.capture_final) rr.final_state = save_game_bytes(*g, fo.save_scenario, fo.save_seed);
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
		{
			const ControlState& cs = g->ctl;
			bool any = cs.speed != 2 || cs.paused || cs.view_all || !cs.chat.empty() || g->ai_off || g->ai_speed != 1;
			for (const HotKeyGroup& h : cs.hotkeys) any |= h.used != 0;
			for (int32_t v : cs.accum_cheated) any |= v != 0;
			if (any) {
				std::printf("console: speed %d (%d ms, %d changes) paused %d view_all %d ai_off %d ai_speed %d; control groups:", cs.speed,
				            cs.target_frame_time, cs.speed_changes, cs.paused, cs.view_all, g->ai_off, g->ai_speed);
				for (size_t k = 0; k < cs.hotkeys.size(); k++) {
					const HotKeyGroup& h = cs.hotkeys[k];
					if (!h.used) continue;
					if (h.bookmark) std::printf(" [%zu bookmark %.0f,%.0f z%d]", k, double(h.x), double(h.y), h.zoom);
					else {
						std::printf(" [%zu L%d%s f%d:", k, int(h.who), h.buildings ? " builds" : "", h.stamp);
						for (int16_t o : h.list) std::printf(" %d", o);
						std::printf("]");
					}
				}
				std::printf("\n");
				for (const ControlState::ChatLine& l : cs.chat) std::printf("  chat f%d from L%d mask %08x: %s\n", l.frame, l.from, l.bits, l.text.c_str());
				for (size_t w = 0; w < cs.accum_cheated.size(); w++)
					if (cs.accum_cheated[w]) std::printf("  cheats by L%zu: %d\n", w, cs.accum_cheated[w]);
			}
		}
		for (const Script& s : scripts) {  // --unitmask / --auto-explore / --carrier-repeat: the bit on the named units at the end
			if (s.kind != Script::UnitMask) continue;
			std::printf("unitmask %x on %d:", s.num, s.who);
			for (int32_t o : s.objs) {
				const Unit* u = g->unit(s.who, o);
				std::printf(" %d=%s", o, !u || !u->alive() ? "gone" : (u->unit_masks & uint32_t(s.num)) ? "set" : "clear");
			}
			std::printf("\n");
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
	if (fo.capture_final) rr.final_game = std::move(g);
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

// --victory MODE (docs/victory-spec.md section 10): each victory mode of the lobby driven to its end on the loaded scenario,
// asserting the winner and the game-over state. MODE: all, teams, conquest, capital, sudden, time, score, wonder, territory,
// economic, chairs, armageddon, allied, tech, resign, reset. The fixtures that need more than two leaders say "skip".
struct VictoryRig {
	const ron::GameFiles& files;
	const ron::Scenario& sc;
	const Rules& rules;
	uint32_t seed;
	const AnimSource* art;
	int32_t reveal;
	bool verbose;
	int fails = 0, total = 0;

	std::unique_ptr<Game> make() const {
		auto g = std::make_unique<Game>();
		g->anims = art;
		g->info.reveal_map = uint8_t(reveal);
		g->load(files, sc, rules, seed);
		return g;
	}
	void check(const char* what, int64_t got, int64_t want) {
		total++;
		if (got != want) fails++;
		if (got != want || verbose) std::printf("  %s %-64s got %lld, want %lld\n", got == want ? "ok  " : "FAIL", what, (long long)got, (long long)want);
	}
};

static std::vector<int32_t> victory_leaders(const Game& g) {
	std::vector<int32_t> v;
	for (int32_t w = 0; w < 8; w++)
		if (g.leaders[size_t(w)].flags & leaderflag::kValid) v.push_back(w);
	return v;
}
static bool v_defeated(const Game& g, int32_t w) { return (g.leaders[size_t(w)].flags & leaderflag::kDefeated) != 0; }
static bool v_won(const Game& g, int32_t w) { return (g.leaders[size_t(w)].flags & leaderflag::kVictory) != 0; }
// A rules list entry / constant rewritten for one game (Game::mut_rules copies the shared tables first).
static void v_set_list(Game& g, const char* list, int32_t idx, int32_t value) {
	Rules& R = g.mut_rules();
	CategoryList* cl = const_cast<CategoryList*>(R.category_list(list));
	if (cl && idx >= 0 && size_t(idx) < cl->items.size()) cl->items[size_t(idx)].data[0] = value;
}
static void v_set_constant(Game& g, const char* name, int32_t value) {
	const int off = constant_offset(name);
	if (off >= 0) g.mut_rules().constants.v[size_t(off) / 4] = value;
}
static int32_t v_constant(const Game& g, const char* name) {
	const int off = constant_offset(name);
	return off < 0 ? -1 : g.rules->constants.at(uint32_t(off));
}
// The constant that makes a map-scaled clock (Game::wonder_timer / popwin_timer: (35 + width * k) / 70) last `frames`.
static int32_t v_clock_constant(const Game& g, int32_t frames) { return (frames * 70 + g.world.xs - 1) / std::max(1, g.world.xs); }
static int32_t v_count_events(const Game& g, int32_t kind, int32_t value = INT32_MIN) {
	int32_t n = 0;
	for (const GameEvent& e : g.events)
		if (e.kind == kind && (value == INT32_MIN || e.value == value)) n++;
	return n;
}
static int32_t v_first_event_frame(const Game& g, int32_t kind, int32_t value = INT32_MIN) {
	for (const GameEvent& e : g.events)
		if (e.kind == kind && (value == INT32_MIN || e.value == value)) return e.frame;
	return -1;
}
static void v_kill_units(Game& g, int32_t who) {
	for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
		Unit* u = g.unit(who, o);
		if (u && u->alive()) object_die(g, *u, 0, -1, 0.0f);
	}
}
static std::vector<int32_t> v_cities(const Game& g, int32_t who) {
	std::vector<int32_t> v;
	for (int32_t c = 0; c < g.leaders[size_t(who)].city_mark && size_t(c) < g.cities[size_t(who)].size(); c++)
		if (g.cities[size_t(who)][size_t(c)].valid()) v.push_back(c);
	return v;
}
static int32_t v_capital(const Game& g, int32_t who) {
	for (int32_t c : v_cities(g, who))
		if (g.cities[size_t(who)][size_t(c)].city_flags & cityflag::kCapital) return c;
	return -1;
}
static int32_t v_best_score(const Game& g, const std::vector<int32_t>& L) {
	int32_t best = L[0];
	for (int32_t w : L)
		if (g.leaders[size_t(w)].score > g.leaders[size_t(best)].score) best = w;
	return best;
}
// Frames of Game::do_frame until `stop` or `limit` frames; returns the frames run.
template <class F>
static int32_t v_run(Game& g, int32_t limit, F stop) {
	int32_t n = 0;
	while (n < limit && !stop()) {
		g.do_frame();
		n++;
	}
	return n;
}

static void victory_teams(VictoryRig& r) {
	std::printf(" teams / num_sides / Game::get_armageddon\n");
	auto g = r.make();
	const std::vector<int32_t> L = victory_leaders(*g);
	const int32_t n = int32_t(L.size());
	r.check("num_players = num_nations = the seats (GameInfo.players + 1)", g->info.num_nations * 100 + g->info.num_players, n * 100 + n);
	g->info.team.fill(-1);
	game_init_teams(*g);
	r.check("no teams: every seat is a side", g->info.num_sides, n);
	if (n >= 2) {
		g->info.team[size_t(L[0])] = 0;
		g->info.team[size_t(L[1])] = 0;
		game_init_teams(*g);
		const TeamCounts tc = game_team_counts(*g);
		r.check("one team of two: on_team[0]", tc.on_team[0], 2);
		r.check("one team of two: num_teams", tc.num_teams, 1);
		r.check("one team of two: num_sides = teams + loose seats", g->info.num_sides, 1 + (n - 2));
		r.check("get_armageddon = per_team * sides + per_nation * nations + base",
		        get_armageddon(*g),
		        v_constant(*g, "armageddon_per_team") * g->info.num_sides + v_constant(*g, "armageddon_per_nation") * g->info.num_nations +
		            v_constant(*g, "armageddon"));
		r.check("leader_get_team of a member", leader_get_team(*g, L[0]), 0);
		for (int32_t w = 0; w < 8; w++) {
			if (g->leaders[size_t(w)].flags & leaderflag::kValid) continue;
			r.check("leader_get_team without a seat is 8", leader_get_team(*g, w), 8);
			break;
		}
	}
	if (n >= 3) {
		g->info.team[size_t(L[2])] = 5;  // random team: counted by the lobby later
		game_init_teams(*g);
		r.check("a team-5 seat waits for the lobby", g->info.num_sides, 1 + (n - 3));
	}
}

static void victory_conquest(VictoryRig& r) {
	std::printf(" conquest (elimination 0): the last unit and the last city of every rival\n");
	auto g = r.make();
	g->info.victory = 0;
	g->info.elimination = 0;
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	const int32_t a = L[0];
	for (size_t i = 1; i < L.size(); i++) {
		const int32_t b = L[i];
		const std::vector<int32_t> cs = v_cities(*g, b);
		bool had_units = false;
		for (int32_t o = 0; o < g->objects[size_t(b)].unit_mark; o++) {
			const Unit* u = g->unit(b, o);
			had_units = had_units || (u && u->alive());
		}
		v_kill_units(*g, b);
		if (!cs.empty() && had_units) {
			r.check("units gone, a city left: not defeated", v_defeated(*g, b), 0);
			for (int32_t c : cs) cities_capture_city(*g, a, c, b);
		} else if (!cs.empty()) {
			for (int32_t c : cs) cities_capture_city(*g, a, c, b);
		}
		if (cs.empty() && !had_units) {
			// A seat that starts with nothing has no unit to lose and no city to capture: nothing ever triggers its defeat.
			r.check("an empty seat is not defeated by itself", v_defeated(*g, b), 0);
			leader_defeat(*g, b, 0, a, 0);
		}
		r.check("rival without units or cities is defeated by conquest", v_defeated(*g, b) ? g->leaders[size_t(b)].defeat_type : -1, 0);
		if (i + 1 < L.size()) r.check("game still on while rivals remain", g->game_over, 0);
	}
	r.check("game over once one side is left", g->game_over, 1);
	r.check("the survivor has the victory (type 0)", v_won(*g, a) ? g->leaders[size_t(a)].victory_type : -1, 0);
	const int32_t f = g->frame;
	g->do_frame();
	r.check("Game::do_frame returns at once after game over", g->frame, f);
}

static void victory_capital(VictoryRig& r) {
	std::printf(" capital (elimination 1): the capital clock\n");
	{
		auto g = r.make();
		g->info.victory = 0;
		g->info.elimination = 1;
		const std::vector<int32_t> L = victory_leaders(*g);
		if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
		const int32_t a = L[0], b = L[1];
		if (v_capital(*g, b) < 0) return std::printf("  skip: the second leader has no capital\n"), void();
		leader_lost_capital(*g, b, a);
		r.check("lost_capital starts the clock", g->leaders[size_t(b)].lost_capital_timer, 1);
		const int32_t t0 = g->frame, need = game_retake_capital(*g, b);
		v_run(*g, 200, [&] { return false; });
		leader_recapture_capital(*g, b);
		r.check("recapture stops the clock", g->leaders[size_t(b)].lost_capital_timer, 0);
		v_run(*g, need + 100, [&] { return v_defeated(*g, b); });
		r.check("a stopped clock does not eliminate", v_defeated(*g, b), 0);
		leader_lost_capital(*g, b, a);
		const int32_t t1 = g->frame;
		const int32_t ran = v_run(*g, need * 2 + 600, [&] { return v_defeated(*g, b); });
		r.check("the clock eliminates by capital (type 1)", v_defeated(*g, b) ? g->leaders[size_t(b)].defeat_type : -1, 1);
		r.check("... not before the retake time", ran >= need ? 1 : 0, 1);
		(void)t0, (void)t1;
	}
	{
		auto g = r.make();
		g->info.elimination = 2;
		const std::vector<int32_t> L = victory_leaders(*g);
		// The Persians (tribe bonus 0x17) survive a lost capital while they keep another one: pick a victim without it.
		int32_t a = -1, b = -1, cap = -1;
		for (int32_t w : L) {
			if (v_capital(*g, w) < 0 || has_tribe_bonus(*g, w, 0x17)) continue;
			b = w, cap = v_capital(*g, w);
			break;
		}
		for (int32_t w : L)
			if (w != b) a = w;
		if (b < 0 || a < 0) return std::printf("  skip: no non-Persian leader with a capital\n"), void();
		const size_t ncities = v_cities(*g, b).size();
		cities_capture_city(*g, a, cap, b);
		r.check("capital sudden death: the lost capital defeats at once", v_defeated(*g, b), 1);
		// The type is recorded by the first Leader::defeat to finish: defeat(2) hands the other cities over first (defeat_by), and the
		// nested lost_a_city of the last one defeats by conquest (type 0) before the outer call returns (capture-spec 6.4).
		r.check("... its type: 2 with a lone capital, else the nested conquest defeat 0", g->leaders[size_t(b)].defeat_type, ncities == 1 ? 2 : 0);
	}
}

static void victory_sudden(VictoryRig& r) {
	std::printf(" sudden death (elimination 3)\n");
	auto g = r.make();
	g->info.elimination = 3;
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	const int32_t a = L[0], b = L[1];
	const std::vector<int32_t> cs = v_cities(*g, b);
	if (cs.empty()) return std::printf("  skip: no city\n"), void();
	cities_capture_city(*g, a, cs[0], b);
	r.check("one lost city of their own defeats (type 3)", v_defeated(*g, b) ? g->leaders[size_t(b)].defeat_type : -1, 3);
	r.check("game over exactly when two sides remain", g->game_over, L.size() == 2 ? 1 : 0);
}

static void victory_time(VictoryRig& r) {
	std::printf(" time limit (victory 4): the custom limit of the scenario slot, 3 minutes\n");
	auto g = r.make();
	g->info.victory = 4;
	g->info.time_limit = 8;
	g->scen.custom_time_limit = 3;
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	v_run(*g, 3000, [&] { return g->game_over; });
	r.check("the game ends at tick 180 (frame 2700)", g->frame, 2701);
	r.check("two-minute warning at frame 900", v_first_event_frame(*g, victoryevent::kTimeWarning, 120), 900);
	r.check("no five-minute warning in a three-minute game", v_count_events(*g, victoryevent::kTimeWarning, 300), 0);
	const int32_t best = v_best_score(*g, L);
	int32_t winner = -1;
	for (int32_t w : L)
		if (v_won(*g, w) && g->leaders[size_t(w)].victory_type == 6) winner = w;
	r.check("the best score wins (type 6)", winner, best);
	r.check("game over", g->game_over, 1);
}

static void victory_score(VictoryRig& r) {
	std::printf(" score victory (victory 3)\n");
	auto g = r.make();
	g->info.victory = 3;
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	v_set_list(*g, "scores", g->info.score_goal, 500);
	for (int32_t w : L) g->leaders[size_t(w)].score = 100;
	game_daemon_process_victory(*g);
	r.check("below the goal: no victory", v_won(*g, L[0]) || v_won(*g, L[1]), 0);
	g->leaders[size_t(L[1])].score = 500;
	g->leaders[size_t(L[0])].score = 500;  // a tie keeps the lower index
	game_daemon_process_victory(*g);
	r.check("a tie at the goal: the lower index wins (type 4)", v_won(*g, L[0]) ? g->leaders[size_t(L[0])].victory_type : -1, 4);
	r.check("the rivals are defeated (type 5)", v_defeated(*g, L[1]) ? g->leaders[size_t(L[1])].defeat_type : -1, 5);
	r.check("game over (every rival defeated by type 5)", g->game_over, 1);
}

static void victory_wonder(VictoryRig& r) {
	std::printf(" wonder victory (victory 0): the clock, its warnings and its stop\n");
	auto g = r.make();
	g->info.victory = 0;
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	const int32_t a = L[0];
	const int32_t T = 4600;
	v_set_constant(*g, "wonder_timer", v_clock_constant(*g, T));
	const std::vector<int32_t> cs = v_cities(*g, a);
	int32_t x = cs.empty() ? 3000 : g->cities[size_t(a)][size_t(cs[0])].x, y = cs.empty() ? 3000 : g->cities[size_t(a)][size_t(cs[0])].y;
	std::vector<int32_t> made;
	for (int32_t k = 0; k < 17 && wonder_winning(*g) != a; k++) {
		const int32_t o = init_build_site(*g, a, 526 + k, x + 400 * (k + 1), y, true, -1);
		if (o < 0) continue;
		wonders_init_wonder(*g, a, o);
		made.push_back(o);
	}
	r.check("enough wonders: Game::wonder_winning names the leader", wonder_winning(*g), a);
	g->frame = 100;
	game_daemon_process_victory(*g);
	r.check("the clock starts", g->leaders[size_t(a)].wonderwin_timer, 1);
	r.check("Game::is_victory_timer sees the running clock", game_is_victory_timer(*g), 1);
	r.check("start event carries the countdown", v_count_events(*g, victoryevent::kWonderStart), 1);
	const int32_t T0 = g->frame, total = [&] {
		for (const GameEvent& e : g->events)
			if (e.kind == victoryevent::kWonderStart) return e.value;
		return 0;
	}();
	for (int32_t left : {4500, 1800, 900}) {
		g->frame = T0 + total - left;
		game_daemon_process_victory(*g);
		if (left < total) r.check("warning at this many frames left", v_count_events(*g, victoryevent::kWonderWarning, left), 1);
	}
	// stop: the wonders fall (records closed), the clock halts holding the elapsed time
	g->frame = T0 + 1000;
	const std::vector<Wonder> keep = g->wonder_records[size_t(a)];
	for (size_t i = 0; i < keep.size(); i++) wonders_close_wonder(*g, a, int32_t(i));
	game_daemon_process_victory(*g);
	r.check("the lost lead stops the clock", g->leaders[size_t(a)].wonderwin_timer, 0);
	r.check("stop event", v_count_events(*g, victoryevent::kWonderStop), 1);
	r.check("Game::is_victory_timer after the stop", game_is_victory_timer(*g), 0);
	g->wonder_records[size_t(a)] = keep;
	g->leaders[size_t(a)].wonder_mark = int32_t(keep.size());
	g->frame += 200;
	game_daemon_process_victory(*g);
	r.check("regained: the clock restarts where it stopped", g->leaders[size_t(a)].wonderwin_timer, 1);
	const int32_t vs = g->leaders[size_t(a)].wonderwin_stamp;  // the shifted start frame
	g->frame = vs + total - 1;
	game_daemon_process_victory(*g);
	r.check("one frame early: no victory", v_won(*g, a), 0);
	g->frame = vs + total;
	game_daemon_process_victory(*g);
	r.check("the countdown reaches zero: wonder victory (type 1)", v_won(*g, a) ? g->leaders[size_t(a)].victory_type : -1, 1);
	r.check("game over (every rival defeated by type 5)", g->game_over, 1);
	(void)made;
}

static void victory_territory(VictoryRig& r) {
	std::printf(" territory victory (victory 0): the share of the land and its clock\n");
	auto g = r.make();
	g->info.victory = 0;
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	const int32_t a = L[0];
	const int32_t T = 4600, land = g->world_total_land;
	v_set_constant(*g, "popwin_timer", v_clock_constant(*g, T));
	v_set_list(*g, "popwins", g->info.popwin, 10);
	for (int32_t w : L) g->leaders[size_t(w)].territory = 0;
	g->leaders[size_t(a)].territory = land / 2;
	for (int32_t w : L)
		if (w != a) g->leaders[size_t(w)].territory = land / 20;  // 5%: below the 10% needed
	g->frame = 200;
	game_daemon_process_victory(*g);
	r.check("the clock starts", g->leaders[size_t(a)].popwin_timer, 1);
	int32_t total = 0;
	for (const GameEvent& e : g->events)
		if (e.kind == victoryevent::kPopStart) total = e.value;
	r.check("start event carries the countdown", total > 0 ? 1 : 0, 1);
	const int32_t T0 = g->frame;
	for (int32_t left : {4500, 1800, 900}) {
		g->frame = T0 + total - left;
		game_daemon_process_victory(*g);
		if (left < total) r.check("warning at this many frames left", v_count_events(*g, victoryevent::kPopWarning, left), 1);
	}
	// a rival at the share stops the clock; dropping it restarts
	g->leaders[size_t(L[1])].territory = land / 5;
	g->frame = T0 + total - 500;
	game_daemon_process_victory(*g);
	r.check("a rival at the share stops the clock", g->leaders[size_t(a)].popwin_timer, 0);
	r.check("halted event", v_count_events(*g, victoryevent::kPopStop), 1);
	g->leaders[size_t(L[1])].territory = land / 20;
	g->frame += 10;
	game_daemon_process_victory(*g);
	r.check("the clock restarts", g->leaders[size_t(a)].popwin_timer, 1);
	const int32_t vs = g->leaders[size_t(a)].popwin_stamp;  // the shifted start frame
	g->frame = vs + total - 1;
	game_daemon_process_victory(*g);
	r.check("one frame early: no victory", v_won(*g, a), 0);
	g->frame = vs + total;
	game_daemon_process_victory(*g);
	r.check("territory victory (type 2)", v_won(*g, a) ? g->leaders[size_t(a)].victory_type : -1, 2);
	r.check("game over", g->game_over, 1);
}

static void victory_economic(VictoryRig& r) {
	std::printf(" economic victory (victory 8)\n");
	auto g = r.make();
	g->info.victory = 8;
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	v_set_list(*g, "econwins", g->info.econwin, 10);
	for (int32_t w : L) g->leaders[size_t(w)].income.fill(0);
	int32_t good = -1;
	for (int32_t k = 0; k < 6 && good < 0; k++)
		if (k != ptype::kKnowledge && type_avail(*g, L[1], k, 1)) good = k;
	if (good < 0) return std::printf("  skip: the leader can use no good\n"), void();
	g->leaders[size_t(L[1])].income[size_t(good)] = 16 * 40;  // 40 per good-rate unit: (40 + 4) / 5 = 8
	game_daemon_process_victory(*g);
	r.check("get_economic = mean of the five income rates, rounded up", leader_get_economic(*g, L[1]), (40 + 4) / 5);
	r.check("below the goal: no victory", v_won(*g, L[1]), 0);
	g->leaders[size_t(L[1])].income[size_t(good)] = 16 * 100;
	game_daemon_process_victory(*g);
	r.check("at the goal: economic victory (type 5)", v_won(*g, L[1]) ? g->leaders[size_t(L[1])].victory_type : -1, 5);
	r.check("the rivals are defeated (type 5)", v_defeated(*g, L[0]) ? g->leaders[size_t(L[0])].defeat_type : -1, 5);
	r.check("game over", g->game_over, 1);
}

static void victory_chairs(VictoryRig& r) {
	std::printf(" musical chairs (victory 5)\n");
	auto g = r.make();
	g->info.victory = 5;
	g->info.team_style = 0;
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	v_set_list(*g, "chairs", g->info.chairs, 1);  // a round every minute: 900 frames
	for (size_t i = 0; i < L.size(); i++) g->leaders[size_t(L[i])].score = 100 + int32_t(i) * 10;
	g->musical_chairs = 0;
	g->frame = 0;
	game_daemon_process_victory(*g);
	r.check("frame 0: no round", v_defeated(*g, L[0]), 0);
	g->frame = 899;
	game_daemon_process_victory(*g);
	r.check("one frame early: no round", v_defeated(*g, L[0]), 0);
	// a tie for the lowest spares everyone, and the round still counts
	g->leaders[size_t(L[1])].score = g->leaders[size_t(L[0])].score;
	g->frame = 900;
	game_daemon_process_victory(*g);
	r.check("tie for the lowest score: nobody is defeated", v_defeated(*g, L[0]) || v_defeated(*g, L[1]), 0);
	r.check("... but the round moved the stamp", g->musical_chairs, 900);
	g->leaders[size_t(L[1])].score = g->leaders[size_t(L[0])].score + 5;
	g->frame = 1799;
	game_daemon_process_victory(*g);
	r.check("the stamp restarts the wait", v_defeated(*g, L[0]), 0);
	g->frame = 1800;
	game_daemon_process_victory(*g);
	r.check("the lowest score is defeated (type 4)", v_defeated(*g, L[0]) ? g->leaders[size_t(L[0])].defeat_type : -1, 4);
	r.check("round event", v_count_events(*g, victoryevent::kChairsRound) >= 2 ? 1 : 0, 1);
	// keep going until one leader is left
	for (int32_t round = 3; round < 12 && !g->game_over; round++) {
		g->frame = round * 900;
		game_daemon_process_victory(*g);
	}
	r.check("game over when one leader is left", g->game_over, 1);
	{
		// Game::check_victory ends the game at once when every human seat is out (no winner), or when the leaders left are
		// all allies (they share the victory).
		bool humans = false, human_live = false;
		int live = 0, won = 0;
		for (int32_t w : L) {
			const bool human = (g->leaders[size_t(w)].flags & leaderflag::kHuman) != 0;
			humans = humans || human;
			if (v_defeated(*g, w)) continue;
			human_live = human_live || human;
			live++;
			won += v_won(*g, w) ? 1 : 0;
		}
		r.check("the survivors share the victory (unless the humans are all out)", (humans && !human_live) || (live > 0 && won == live), 1);
	}
	if (L.size() >= 4) {
		auto t = r.make();
		t->info.victory = 5;
		t->info.team_style = 12;
		t->info.team.fill(-1);
		t->info.team[size_t(L[0])] = t->info.team[size_t(L[1])] = 0;
		t->info.team[size_t(L[2])] = t->info.team[size_t(L[3])] = 1;
		v_set_list(*t, "chairs", t->info.chairs, 1);
		const int32_t sc[4] = {10, 20, 30, 40};
		for (int i = 0; i < 4; i++) t->leaders[size_t(L[size_t(i)])].score = sc[i];
		t->musical_chairs = 0;
		t->frame = 900;
		game_daemon_process_victory(*t);
		r.check("teams: the weakest of team 0 is defeated", v_defeated(*t, L[0]), 1);
		r.check("teams: the weakest of team 1 is defeated", v_defeated(*t, L[2]), 1);
		r.check("teams: the stronger members stay", v_defeated(*t, L[1]) || v_defeated(*t, L[3]), 0);
	}
}

static void victory_armageddon(VictoryRig& r) {
	std::printf(" armageddon (the nuke count)\n");
	auto g = r.make();
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	const int32_t lim = get_armageddon(*g);
	g->info.starting_resources = 8;
	g->armageddon = 0;
	armageddon_nuke_detonated(*g);
	r.check("starting resources 8: nukes are not counted", g->armageddon, 0);
	g->info.starting_resources = 0;
	const int32_t lim0 = get_armageddon(*g);
	g->armageddon = lim0 - 1;
	game_daemon_process_victory(*g);
	r.check("one nuke short: nobody is defeated", v_defeated(*g, L[0]) || v_defeated(*g, L[1]), 0);
	armageddon_nuke_detonated(*g);
	r.check("the nuke adds one", g->armageddon, lim0);
	game_daemon_process_victory(*g);
	bool all = true;
	for (int32_t w : L) all = all && v_defeated(*g, w) && g->leaders[size_t(w)].defeat_type == 8 && !v_won(*g, w);
	r.check("at the limit every leader is defeated (type 8), no winner", all, 1);
	r.check("game over", g->game_over, 1);
	r.check("scores are zeroed by the Armageddon rule", g->leaders[size_t(L[0])].score, 0);
	(void)lim;
	// The same through the explosion itself: Nuke::do_damage counts the nuke 110 frames after it began, and the game ends.
	auto n = r.make();
	const std::vector<int32_t> N = victory_leaders(*n);
	n->info.starting_resources = 0;
	n->armageddon = get_armageddon(*n) - 1;
	int32_t shooter = -1;
	for (int32_t o = 0; o < n->objects[size_t(N[0])].unit_mark && shooter < 0; o++)
		if (const Unit* u = n->unit(N[0], o); u && u->alive() && u->on_map()) shooter = o;
	if (shooter < 0) return std::printf("  skip: no unit to shoot with\n"), void();
	const Unit* su = n->unit(N[0], shooter);
	nuke_detonate(*n, N[0], shooter, su->x, su->y, 1);
	for (int32_t f = 0; f < 109; f++) {
		n->frame++;
		nuke_do_damage(*n);
	}
	r.check("a detonation does not count at once, nor in its first 109 frames", n->armageddon, get_armageddon(*n) - 1);
	n->frame++;
	nuke_do_damage(*n);
	r.check("the explosion counts as a nuke on its 110th frame", n->armageddon, get_armageddon(*n));
	n->frame++;
	nuke_do_damage(*n);
	r.check("and only once", n->armageddon, get_armageddon(*n));
	game_daemon_process_victory(*n);
	bool alln = true;
	for (int32_t w : N) alln = alln && v_defeated(*n, w) && n->leaders[size_t(w)].defeat_type == 8;
	r.check("the counted nuke ends the game for everyone (type 8)", alln && n->game_over, 1);
}

static void victory_allied(VictoryRig& r) {
	std::printf(" allied victory (Leader::set_diplo)\n");
	auto g = r.make();
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	for (int32_t a : L)
		for (int32_t b : L)
			if (a != b) g->leaders[size_t(a)].diplo[size_t(b)] = 1;
	// A clique, with the last pair set through Leader::set_diplo. (Victory fires when a new alliance leaves no outsider, and then
	// every ally runs Leader::victory itself and defeats whoever it is not allied with: an incomplete clique loses members.)
	const size_t n = L.size();
	if (n >= 3) {
		leader_set_diplo(*g, L[0], L[1], 2);
		r.check("an alliance with rivals left outside: no victory", v_won(*g, L[0]) || g->victory_checked, 0);
	}
	for (size_t i = 0; i < n; i++)
		for (size_t j = i + 1; j < n; j++)
			if (!(i == n - 2 && j == n - 1) && !(i == 0 && j == 1 && n >= 3))
				g->leaders[size_t(L[i])].diplo[size_t(L[j])] = g->leaders[size_t(L[j])].diplo[size_t(L[i])] = 2;
	r.check("a clique minus one pair: still no victory", v_won(*g, L[0]) || g->victory_checked, 0);
	leader_set_diplo(*g, L[n - 2], L[n - 1], 2);
	r.check("the last alliance leaves nobody outside: victory", v_won(*g, L[n - 2]), 1);
	r.check("end-game flag (semaphore byte2 & 0x40)", g->victory_checked, 1);
	bool allies_won = true;
	for (int32_t w : L) allies_won = allies_won && v_won(*g, w) && g->leaders[size_t(w)].victory_type == 0;
	r.check("every ally shares the victory (type 0)", allies_won, 1);
	r.check("the simulation is not stopped by it", g->game_over, 0);
	r.check("the front end sees the game as ended", game_ended(*g), 1);
	r.check("game-over event for the allied victory", v_count_events(*g, 4, 1), 1);
}

static void victory_tech(VictoryRig& r) {
	std::printf(" tech race (victory 9)\n");
	auto g = r.make();
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	g->info.victory = 9;
	const int32_t a = L[0];
	int32_t t = kAges;
	while (t < kEndAges && has_tech(*g, a, t)) t++;
	if (t >= kEndAges) return std::printf("  skip: every age is known\n"), void();
	g->info.ending_technology = uint8_t(g->leaders[size_t(a)].age + 2);
	gain_tech(*g, a, t, 0, 0, false, true);
	r.check("an age short of the end: no victory", v_won(*g, a), 0);
	while (t < kEndAges && has_tech(*g, a, t)) t++;
	if (t >= kEndAges) return std::printf("  skip: no further age\n"), void();
	gain_tech(*g, a, t, 0, 0, false, true);
	r.check("the last age: tech-race victory (type 3)", v_won(*g, a) ? g->leaders[size_t(a)].victory_type : -1, 3);
	r.check("the rivals are defeated (type 5)", v_defeated(*g, L[1]) ? g->leaders[size_t(L[1])].defeat_type : -1, 5);
	r.check("game over", g->game_over, 1);
}

static void victory_resign(VictoryRig& r) {
	std::printf(" resign (Player::resign -> leave_game 6)\n");
	auto g = r.make();
	const std::vector<int32_t> L = victory_leaders(*g);
	if (L.size() < 2) return std::printf("  skip: needs two leaders\n"), void();
	for (size_t i = 1; i < L.size(); i++) {
		player_resign(*g, L[i], false);
		r.check("the resigning leader is defeated (type 6)", v_defeated(*g, L[i]) ? g->leaders[size_t(L[i])].defeat_type : -1, 6);
	}
	r.check("one leader left: game over", g->game_over, 1);
	r.check("the survivor wins (type 0)", v_won(*g, L[0]) ? g->leaders[size_t(L[0])].victory_type : -1, 0);
	r.check("resign events", v_count_events(*g, victoryevent::kResigned), int64_t(L.size()) - 1);
	// the command path: CommandManager::issue_resign executes process_resign from the seat
	auto h = r.make();
	CommandManager cm;
	cm.play = L[1];
	cm.play_who = L[1];
	cm.issue_resign(false);
	cm.process_turn(*h);
	r.check("RESIGN command: the seat's leader is defeated (type 6)", v_defeated(*h, L[1]) ? h->leaders[size_t(L[1])].defeat_type : -1, 6);
}

static void victory_reset(VictoryRig& r) {
	std::printf(" Leader::reset_score and the score\n");
	Leader l;
	r.check("a fresh leader's age stamps are -1", l.age_stamp[0] == -1 && l.age_stamp[6] == -1, 1);
	l.score = 99, l.score_units = 5, l.units_killed = 3, l.age_stamp[2] = 400;
	leader_reset_score(l);
	r.check("reset_score: score terms zero", l.score + l.score_units + l.units_killed, 0);
	r.check("reset_score: age stamps back to -1", l.age_stamp[2], -1);
	auto g = r.make();
	const std::vector<int32_t> L = victory_leaders(*g);
	for (int32_t w : L) leader_compute_score(*g, w, 1);
	for (int32_t w : L) {
		const Leader& x = g->leaders[size_t(w)];
		const int32_t sum = x.score_combat + x.score_wonders + x.score_research + x.score_upgrades + x.score_economy + x.score_buildings +
		                    x.score_units + x.score_territory + x.score_pop + x.score_explored;
		r.check("score = the sum of its terms", x.score, sum);
	}
}

int run_victory_fixtures(const ron::GameFiles& files, const ron::Scenario& sc, const Rules& rules, uint32_t seed, const AnimSource* art,
                         int32_t reveal, const std::string& mode, bool verbose) {
	VictoryRig r{files, sc, rules, seed, art, reveal, verbose};
	struct Entry {
		const char* name;
		void (*fn)(VictoryRig&);
	};
	static const Entry kModes[] = {{"teams", victory_teams},         {"conquest", victory_conquest},   {"capital", victory_capital},
	                               {"sudden", victory_sudden},       {"time", victory_time},           {"score", victory_score},
	                               {"wonder", victory_wonder},       {"territory", victory_territory}, {"economic", victory_economic},
	                               {"chairs", victory_chairs},       {"armageddon", victory_armageddon}, {"allied", victory_allied},
	                               {"tech", victory_tech},           {"resign", victory_resign},       {"reset", victory_reset}};
	bool ran = false;
	for (const Entry& e : kModes) {
		if (mode != "all" && mode != e.name) continue;
		ran = true;
		const int before = r.fails;
		e.fn(r);
		std::printf("%s %s\n", r.fails == before ? "PASS" : "FAIL", e.name);
	}
	if (!ran) {
		std::printf("unknown --victory mode '%s' (all, teams, conquest, capital, sudden, time, score, wonder, territory, economic, chairs, armageddon, allied, tech, resign, reset)\n", mode.c_str());
		return 2;
	}
	std::printf("victory fixtures: %d checks, %d failed\n", r.total, r.fails);
	return r.fails == 0 ? 0 : 1;
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
		// `ron-sim <game dir> --load path ...`: the scenario and the seed come from the save.
		std::vector<std::string> arg_store;
		std::vector<char*> arg_ptrs;
		SaveInfo load_info;
		bool loading = false;
		for (int i = 2; i + 1 < argc; i++) {
			if (std::strcmp(argv[i], "--load") != 0) continue;
			loading = true;
			load_info = read_save_info(argv[i + 1]);
			if (std::strncmp(argv[2], "--", 2) == 0) {
				arg_store.assign(argv, argv + argc);
				arg_store.insert(arg_store.begin() + 2, load_info.scenario);
				for (std::string& a : arg_store) arg_ptrs.push_back(a.data());
				argv = arg_ptrs.data();
				argc = int(arg_ptrs.size());
			}
			break;
		}
		// `ron-sim <game dir> --random STYLE,SIZE,SEED,NATIONS ...`: a random-map skirmish of AI seats (same grammar as ronmap's --random).
		const bool random_mode = std::strcmp(argv[2], "--random") == 0;
		if (random_mode && argc >= 4) {
			arg_store.assign(argv, argv + argc);
			arg_store.insert(arg_store.begin() + 2, "random");
			arg_ptrs.clear();
			for (std::string& a : arg_store) arg_ptrs.push_back(a.data());
			argv = arg_ptrs.data();
			argc = int(arg_ptrs.size());
		}
		std::string file = argv[2];
		std::error_code ec;
		if (!random_mode && !std::filesystem::exists(file, ec)) {
			const std::string r = files.resolve(file);
			if (r.empty()) throw std::runtime_error("scenario not found: " + file);
			file = r;
		}
		ron::Scenario sc;
		if (!random_mode) sc = ron::parse_scenario(ron::read_file(file));
		Rules rules = load_rules(files);
		start_game(rules, GameSetup());
		ron::sim::RandomGameSetup random_setup;
		if (random_mode) {
			const char* spec = arg_value(argc, argv, "--random");
			if (!spec) throw std::runtime_error("--random needs STYLE,SIZE,SEED,NATIONS");
			const std::string perr = ron::sim::parse_random_option(rules, spec, random_setup);
			if (!perr.empty()) throw std::runtime_error(perr);
			for (ron::sim::NationSetup& n : random_setup.nation) n.human = false;
			if (const char* v = std::getenv("RON_LONGRUN_VICTORY")) random_setup.victory = std::atoi(v);  // as ronmap: 2 = conquest only, so a wonder win does not end a long run
			ron::sim::RandomExtras rx;  // the generated scenario records stand in for the file (the first build only gives them)
			if (!ron::sim::new_random_game(files, rules, nullptr, random_setup, &rx)) throw std::runtime_error("the random map could not be generated: " + rx.error);
			sc = std::move(rx.scenario);
			random_setup = rx.setup;
		}
		const char* fa = arg_value(argc, argv, "--frames");
		const int32_t frames = fa ? int32_t(std::strtol(fa, nullptr, 0)) : 300;
		const char* sa = arg_value(argc, argv, "--seed");
		const uint32_t seed = loading ? load_info.seed : sa ? uint32_t(std::strtoul(sa, nullptr, 0)) : 12345u;
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
			else if (std::strcmp(argv[i], "--flight") == 0) scripts.push_back(parse_air(Script::Flight, argv[i + 1], "flight"));
			else if (std::strcmp(argv[i], "--air-patrol") == 0 || std::strcmp(argv[i], "--patrol") == 0)  // PATROL: planes fly it, ground groups walk it
				scripts.push_back(parse_air(Script::AirPatrol, argv[i + 1], "patrol"));
			else if (std::strcmp(argv[i], "--launch-patrol") == 0)
				scripts.push_back(parse_air(Script::LaunchPatrol, argv[i + 1], "launch-patrol"));
			else if (std::strcmp(argv[i], "--attack-ground") == 0)
				scripts.push_back(parse_air(Script::AttackGround, argv[i + 1], "attack-ground"));
			else if (std::strcmp(argv[i], "--scramble") == 0) scripts.push_back(parse_air(Script::Scramble, argv[i + 1], "scramble"));
			else if (std::strcmp(argv[i], "--recall") == 0) scripts.push_back(parse_air(Script::Recall, argv[i + 1], "recall"));
			else if (std::strcmp(argv[i], "--stock") == 0) {  // WHO:N[@FRAME]: every good of the leader is set to N (a test aid)
				Script st;
				st.kind = Script::Stock;
				const std::vector<std::string> p1 = split(argv[i + 1], '@');
				const std::vector<std::string> p2 = split(p1.at(0), ':');
				st.who = int32_t(std::strtol(p2.at(0).c_str(), nullptr, 0));
				st.num = int32_t(std::strtol(p2.at(1).c_str(), nullptr, 0));
				if (p1.size() > 1) st.frame = int32_t(std::strtol(p1[1].c_str(), nullptr, 0));
				scripts.push_back(st);
			}
			else if (std::strcmp(argv[i], "--teleport") == 0) scripts.push_back(parse_air(Script::Teleport, argv[i + 1], "teleport"));  // WHO:o1,o2@X,Y[@FRAME]
			else if (std::strcmp(argv[i], "--kill") == 0) scripts.push_back(parse_air(Script::Kill, argv[i + 1], "kill"));  // WHO:o1,o2[@FRAME]
			else if (std::strcmp(argv[i], "--crash") == 0) scripts.push_back(parse_air(Script::Crash, argv[i + 1], "crash"));  // WHO:o1,o2[@FRAME]
			else if (std::strcmp(argv[i], "--auto-explore") == 0 || std::strcmp(argv[i], "--carrier-repeat") == 0) {  // WHO:o1,o2[@FRAME]
				Script um = parse_air(Script::Crash, argv[i + 1], "unitmask");
				um.kind = Script::UnitMask;
				um.num = std::strcmp(argv[i], "--auto-explore") == 0 ? 0x100 : 0x200000;
				scripts.push_back(um);
			}
			else if (std::strcmp(argv[i], "--unitmask") == 0) {  // WHO:o1,o2@MASK[@FRAME]
				const std::vector<std::string> p1 = split(argv[i + 1], '@');
				const std::vector<std::string> p2 = split(p1.at(0), ':');
				Script um;
				um.kind = Script::UnitMask;
				um.who = int32_t(std::strtol(p2.at(0).c_str(), nullptr, 0));
				um.objs = parse_list(p2.at(1));
				um.num = int32_t(std::strtol(p1.at(1).c_str(), nullptr, 0));
				if (p1.size() > 2) um.frame = int32_t(std::strtol(p1[2].c_str(), nullptr, 0));
				scripts.push_back(um);
			}
			else if (std::strcmp(argv[i], "--flock") == 0) {  // WHO:TX,TY,N[@FRAME]  (WHO -1: gaia)
				const std::vector<std::string> p1 = split(argv[i + 1], '@');
				const std::vector<std::string> p2 = split(p1.at(0), ':');
				const std::vector<int32_t> xyn = parse_list(p2.at(1));
				if (xyn.size() < 3) throw std::runtime_error(std::string("bad --flock ") + argv[i + 1]);
				Script fl;
				fl.kind = Script::Flock;
				fl.who = int32_t(std::strtol(p2.at(0).c_str(), nullptr, 0));
				fl.x = xyn[0], fl.y = xyn[1], fl.num = xyn[2];
				if (p1.size() > 1) fl.frame = int32_t(std::strtol(p1[1].c_str(), nullptr, 0));
				scripts.push_back(fl);
			}
			else if (std::strcmp(argv[i], "--eject-all") == 0) scripts.push_back(parse_eject_all(argv[i + 1]));
			else if (std::strcmp(argv[i], "--transport") == 0) scripts.push_back(parse_transport(argv[i + 1], Script::Transport));
			else if (std::strcmp(argv[i], "--set-transport") == 0) scripts.push_back(parse_transport(argv[i + 1], Script::SetTransport));
			else if (std::strcmp(argv[i], "--cast") == 0) scripts.push_back(parse_cast(argv[i + 1]));
			else if (std::strcmp(argv[i], "--board") == 0) scripts.push_back(parse_transport(argv[i + 1], Script::Board));
			else if (std::strcmp(argv[i], "--hotkey") == 0) scripts.push_back(parse_console(Script::Hotkey, argv[i + 1], "hotkey"));
			else if (std::strcmp(argv[i], "--come-out") == 0) scripts.push_back(parse_console(Script::ComeOut, argv[i + 1], "come-out"));
			else if (std::strcmp(argv[i], "--speed") == 0) scripts.push_back(parse_console(Script::Speed, argv[i + 1], "speed"));
			else if (std::strcmp(argv[i], "--chat") == 0) scripts.push_back(parse_console(Script::Chat, argv[i + 1], "chat"));
			else if (std::strcmp(argv[i], "--pause") == 0) scripts.push_back(parse_console(Script::Pause, argv[i + 1], "pause"));
			else if (std::strcmp(argv[i], "--rename-city") == 0) scripts.push_back(parse_console(Script::Rename, argv[i + 1], "rename-city"));
			else if (std::strcmp(argv[i], "--cheat") == 0) scripts.push_back(parse_console(Script::Cheat, argv[i + 1], "cheat"));
			else if (std::strcmp(argv[i], "--nuke") == 0) scripts.push_back(parse_nuke(argv[i + 1]));  // WHO:X,Y@FRAME
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
		// A test aid for --cast ...@PARADROP: the shipped craftrules give PARADROP "FROM none" (never castable, aircraft-spec 9), so the
		// run gives it the Special Forces unit as its caster. The cast body is the original's; only the owner chain is patched.
		for (const Script& s : scripts) {
			if (s.kind != Script::Cast || s.to != 0x284) continue;
			for (size_t i = 0; i < rules.types.size(); i++) {
				const Type& t = rules.types[i];
				std::string nm = to_utf8(t.type_name.empty() ? t.name : t.type_name), flat;
				for (char c : nm)
					if (c != ' ' && c != '_') flat += char(std::toupper(static_cast<unsigned char>(c)));
				if (t.loaded && flat == "SPECIALFORCES") rules.types[0x284].from = int32_t(i);
			}
			break;
		}
		FogOpts fo;
		for (int i = 3; i + 1 < argc; i++) {
			if (std::strcmp(argv[i], "--save-at") == 0) {  // FRAME:path
				const std::string a = argv[i + 1];
				const size_t colon = a.find(':');
				if (colon == std::string::npos || colon == 0 || colon + 1 >= a.size())
					throw std::runtime_error("bad --save-at " + a + " (FRAME:path)");
				fo.save_at.emplace_back(int32_t(std::strtol(a.c_str(), nullptr, 0)), a.substr(colon + 1));
			}
		}
		if (loading) fo.load_path = arg_value(argc, argv, "--load");
		if (random_mode) fo.random_setup = &random_setup;
		const char* pa = arg_value(argc, argv, "--prod-every");
		const int32_t prod_every = pa ? int32_t(std::strtol(pa, nullptr, 0)) : 0;
		// GameInfo.reveal_map: --reveal N, else the scenario header's value, else 1 (Normal).
		const char* ra = arg_value(argc, argv, "--reveal");
		const int32_t reveal =
		    ra ? int32_t(std::strtol(ra, nullptr, 0)) : (sc.reveal_map >= 0 && sc.reveal_map <= 3 ? sc.reveal_map : 1);
		if (const char* vm = arg_value(argc, argv, "--victory")) {  // --victory MODE: docs/victory-spec.md section 10
			return run_victory_fixtures(files, sc, rules, seed, art.get(), reveal, vm, has_flag(argc, argv, "--verbose"));
		}
		if (has_flag(argc, argv, "--spell-fixtures")) {
			Game g;
			g.anims = art.get();
			g.info.reveal_map = uint8_t(reveal);
			g.load(files, sc, rules, seed);
			return run_spell_fixtures(g, has_flag(argc, argv, "--verbose")) == 0 ? 0 : 1;
		}
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
					if (unit_is_hero(*u) && u->cara >= 0 && size_t(u->cara) < l.heroes.size()) {
						const Caster& h = l.heroes[size_t(u->cara)];
						std::printf("      hero slot %d radius %d tiles mana %d/%d spells", int(u->cara), hero_get_radius(g, h),
						            int(u->mana_burn), unit_mana(g, *u));
						for (const ActiveSpell& s : h.active_spells) std::printf(" [0x%x f%d..f%d]", unsigned(s.t), s.start, s.frame);
						std::printf("\n");
					}
				}
				for (const std::string& l : air_describe(g, who)) std::printf("  air %s\n", l.c_str());
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
		if (const char* fw = arg_value(argc, argv, "--fog")) fo.fog_who = int32_t(std::strtol(fw, nullptr, 0));
		fo.fog_check = has_flag(argc, argv, "--fog-check");
		fo.reveal = reveal;
		if (const char* ee = arg_value(argc, argv, "--econ-every")) fo.econ_every = int32_t(std::strtol(ee, nullptr, 0));
		if (const char* ae = arg_value(argc, argv, "--ai-every")) fo.ai_every = int32_t(std::strtol(ae, nullptr, 0));
		if (const char* ai = arg_value(argc, argv, "--air-every")) fo.air_every = int32_t(std::strtol(ai, nullptr, 0));
		if (const char* he = arg_value(argc, argv, "--heroes-every")) fo.heroes_every = int32_t(std::strtol(he, nullptr, 0));
		if (const char* se = arg_value(argc, argv, "--spells-every")) fo.spells_every = int32_t(std::strtol(se, nullptr, 0));
		if (const char* sa = arg_value(argc, argv, "--spells-all")) fo.spells_all = int32_t(std::strtol(sa, nullptr, 0));
		if (const char* sr = arg_value(argc, argv, "--starting-resources"))
			fo.starting_resources = int32_t(std::strtol(sr, nullptr, 0));
		if (const char* df = arg_value(argc, argv, "--difficulty")) fo.difficulty = int32_t(std::strtol(df, nullptr, 0));
		if (has_flag(argc, argv, "--perf")) {  // time do_frame alone: no per-frame digest and none of the --*-every printouts
			g_perf = true;
			fo.econ_every = fo.ai_every = fo.air_every = fo.heroes_every = fo.spells_every = 0;
		}
		fo.scripts = !has_flag(argc, argv, "--no-scripts");
		fo.script_log = has_flag(argc, argv, "--script-log");
		fo.scenario_path = std::filesystem::absolute(file).string();
		fo.save_scenario = scenario_rel_path(files, fo.scenario_path);
		fo.save_seed = seed;
		// --roundtrip FRAME: the first run saves at FRAME; the load pass below continues from that file.
		const char* rtv = arg_value(argc, argv, "--roundtrip");
		const int32_t rt_at = rtv ? int32_t(std::strtol(rtv, nullptr, 0)) : -1;
		std::string rt_file;
		if (rtv) {
			rt_file = "ron-roundtrip-" + std::to_string(long(getpid())) + ".ronsave";
			fo.roundtrip_at = rt_at;
			fo.roundtrip_file = rt_file;
			fo.capture_final = true;
		}
		const RunResult a = run(files, sc, rules, art.get(), seed, frames, scripts, dump_every, digest_every, true, fo,
		                        combat_every, prod_every);
		if (a.fog_failed || a.nuke_failed) return 1;
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
		if (rtv) {
			// The run may never reach the frame (a game that ends early stops advancing): then take the round trip one
			// frame before its end instead, with a second run.
			RunResult a2;
			if (!a.roundtrip_saved) {
				fo.roundtrip_at = std::max<int32_t>(0, a.final_frame - 1);
				std::printf("save round-trip: frame %d is not reached (the game stops at frame %d); using frame %d\n", rt_at,
				            a.final_frame, fo.roundtrip_at);
				a2 = run(files, sc, rules, art.get(), seed, frames, scripts, 0, 0, false, fo);
			}
			const RunResult& A = a.roundtrip_saved ? a : a2;
			if (!A.roundtrip_saved) {
				std::printf("save round-trip: FAILED: could not save at frame %d\n", fo.roundtrip_at);
				return 1;
			}
			const int32_t R = A.roundtrip_frame;
			FogOpts fo2 = fo;
			fo2.roundtrip_at = -1;
			fo2.save_at.clear();
			fo2.load_path = rt_file;
			fo2.capture_final = true;
			const int32_t remaining = int32_t(A.digests.size() - A.roundtrip_iter);
			RunResult c;
			try {
				c = run(files, sc, rules, art.get(), seed, remaining, scripts, 0, 0, false, fo2);
			} catch (const std::exception& e) {
				std::remove(rt_file.c_str());
				std::printf("save round-trip: FAILED: %s\n", e.what());
				return 1;
			}
			std::remove(rt_file.c_str());
			for (size_t j = 0; j < c.digests.size(); j++) {
				if (A.roundtrip_iter + j >= A.digests.size() || A.digests[A.roundtrip_iter + j] != c.digests[j]) {
					std::printf("save round-trip: FAILED at frame %d (saved at %d)\n", R + int32_t(j) + 1, R);
					return 1;
				}
			}
			std::string why = describe_save_difference(A.final_state, c.final_state);
			if (!why.empty() && A.final_game && c.final_game)
				why = describe_game_difference(*A.final_game, *c.final_game, fo.save_scenario, fo.save_seed);
			if (!why.empty()) {
				std::printf("save round-trip: FAILED: digests identical but the saved state of frame %d differs: %s\n",
				            A.final_frame, why.c_str());
				return 1;
			}
			if (A.script_at_save != c.script_after_load) {
				std::printf("save round-trip: FAILED: the script state differs after the load:\n  saved:  %s\n  loaded: %s\n",
				            A.script_at_save.c_str(), c.script_after_load.c_str());
				return 1;
			}
			std::printf("save round-trip: OK (saved at frame %d, %lld bytes; %zu frames compared, final state of %zu bytes identical)\n",
			            R, (long long)A.saved_bytes, c.digests.size(), A.final_state.size());
			std::printf("save round-trip: script state restored: %s\n", A.script_at_save.c_str());
		}
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
