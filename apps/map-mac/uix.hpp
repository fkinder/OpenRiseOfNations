// HUD logic of the third pass (docs/ui-port.md section 13), kept out of main.mm: what the air garrison panel shows and selects,
// the library's final techs, the command-card options of aircraft, airbases, carriers and silos and of the advanced orders
// (stance, formation, patrol, guard, disband, infinite queue), the orders a click on a target issues for them, the key-map
// actions that need them, and the selection rules of the object buttons (OPTION_OBJECT), "select all of the current types"
// and the damaged / on-screen filters. Everything goes through the lockstep commands (CommandManager::issue_*).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "hud.hpp"

namespace ron::sim {
class Game;
class CommandManager;
}  // namespace ron::sim

namespace uix {

using Sel = std::vector<std::pair<int32_t, int32_t>>;  // (leader, object slot)

struct Mods {
	bool shift = false, ctrl = false, alt = false;
};

// What the window lends to this module: the game and the command issuer, the viewing seat, the selection lists, the pending
// targeting option and a few presentation callbacks.
struct Ctx {
	ron::sim::Game* game = nullptr;
	ron::sim::CommandManager* cm = nullptr;     // the issuer (the multiplayer CommandSink, upcast)
	std::function<void()> process;              // CommandManager::process_turn of that issuer (executes, or sends through the session)
	int me = 0;
	Sel* units = nullptr;
	Sel* builds = nullptr;
	int* targeting = nullptr;  // the option waiting for a click (a hud option index), -1 none
	bool advanced = false;
	std::function<void(const std::string&)> say;                      // a message line (MessageWin feedback)
	std::function<std::string(int index, const char* fallback)> loc;  // translated_strings.xml entry (the install's language)
	std::function<bool(int32_t who, int32_t o, bool building)> on_screen;  // is_on_screen
	std::function<void(float x, float y)> look_at;                    // centre the camera
};

// OptionIndex values (rise.pdb enum) the card uses here, beyond hud.hpp's.
enum : int {
	kOptDisbandAll = 16, kOptStanceBack = 18, kOptFormBack = 20, kOptGuard = 6, kOptAttackGround = 34, kOptTransfer = 35, kOptAirstrike = 36,
	kOptAirPatrol = 37, kOptFighters = 39, kOptBombers = 40, kOptHelis = 41, kOptAllAir = 42, kOptScramble = 43, kOptRepeat = 44,
	kOptReturn = 45, kOptRecall = 46, kOptInfinite = 64, kOptAllCurrent = 120, kOptQueue = 166
};

using IconFn = std::function<void(int32_t type, int32_t who, int32_t o, hud::Icon& out)>;

// ---------------------------------------------------------------- panels

// IFaceAirGarrison's data for the single selected airbase / carrier of the viewer or an ally (inactive otherwise).
hud::AirGarrison air_garrison(const Ctx& c, const IconFn& icon);
// The planes inside container (who, o) of a family (0 fighters / the carrier's fighter-bombers, 1 bombers, 2 helicopters, -1 all).
std::vector<int32_t> air_objects(const Ctx& c, int32_t who, int32_t o, int family);
// IFaceLibrary: the final-techs view (the four final techs of the library lines and their state).
void library_final(const Ctx& c, int first_library, const std::vector<std::pair<int, int>>& in_production, hud::Library& lib);

// ---------------------------------------------------------------- the command card

bool all_air(const Ctx& c);  // every selected unit is an aircraft (planes and helicopters), the viewer's own
// The whole card of an aircraft selection (UnitOut::make_options, the air rows) / the additions to a ground selection
// (advanced rows: stance, formation, patrol, guard, disband, bombard) / to a single building (airbase, silo, queue).
void air_unit_options(const Ctx& c, std::vector<hud::Option>& out);
void unit_extra_options(const Ctx& c, std::vector<hud::Option>& out);
void build_extra_options(const Ctx& c, std::vector<hud::Option>& out);

// ---------------------------------------------------------------- orders

// Options::exec for the options of this module: 1 handled, 0 not ours.
int run_option(Ctx& c, int option, int object, Mods m);
// The click that ends a targeting option (picked_spot): (x, y) the world point, (tw, to) the object under the cursor or -1.
bool target_click(Ctx& c, int option, int32_t x, int32_t y, int tw, int to, Mods m);
// A right click with aircraft selected (execute_at_cursor: FLIGHT to attack / rebase, PATROL on the ground). False: not ours.
bool right_click(Ctx& c, int32_t x, int32_t y, int tw, int to, Mods m);
// The gather action of a right click for the selected buildings (3 for airbases: a patrol point for the planes, else 0).
int gather_action(const Ctx& c);
// Keys: the key map's option name and parameter. 1 handled, 0 bound but not runnable, -1 not ours.
int key_option(Ctx& c, const std::string& option, int param, Mods m);

// ---------------------------------------------------------------- selection

// OPTION_OBJECT: the clicked button of the selection panel. `list` is the HUD's selection (units then buildings).
void select_object(Ctx& c, const std::vector<hud::Object>& list, int index, Mods m);
// A grouped (counted) type button: plain keeps only that type, Shift removes it.
void select_type(Ctx& c, int type, Mods m);
// The planes of container (who, o) become the selection (select_air).
void select_air(Ctx& c, int32_t who, int32_t o, int family);
// OPTION_ALL_CURRENT / SELECT_DAMAGED / FILTER_DAMAGED / FILTER_ONSCREEN.
void select_all_current(Ctx& c);
void select_damaged(Ctx& c);
void filter_damaged(Ctx& c);
void filter_on_screen(Ctx& c);
// The box-select filters of Console::on_left_up (ui-spec 4.1): by default (Prefer Military, Ctrl flips it) a box that holds
// soldiers drops the citizens, caravans and merchants; Ctrl+Shift keeps citizens only; Alt drops civilians and non-attackers
// except siege; Alt+Ctrl keeps siege only; Alt+Shift keeps aircraft only. `list` is the boxed units of one owner.
void box_filter(Ctx& c, Sel& list, Mods m, bool prefer_military);
// OPTION_CYCLE_RESEARCH (Tab): the next building of the research list (library, university, ...), selected and centred.
void cycle_research(Ctx& c, int* cycle_index);

// Group::compute_form for the formation-drag preview: the slot (world x, y) of each selected own unit for a move to (x, y) in
// formation `form` (width 0..100), facing `angle` when set_angle. The routine writes the game's `forms` scratch, so the entry
// it touches is saved and put back: the game is left as it was (and the HUD stays out of the simulation's state).
std::vector<std::pair<int32_t, int32_t>> formation_slots(Ctx& c, int32_t x, int32_t y, int form, int width, bool set_angle, uint32_t angle);

// Stance / formation shown on the card for the selection: the most common option and whether the selection is mixed.
struct Shown {
	int option = 0;
	bool mixed = false;
	int kind = -1;  // the stance type (0 combat, 1 worker, 2 caster, 3 packer), -1 none
};
Shown selection_stance(const Ctx& c);
Shown selection_form(const Ctx& c);

// help.xml key of an option for the tooltip ("" none).
const char* help_key(int option);

}  // namespace uix
