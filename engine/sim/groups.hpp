// Groups and formations (groups.obj, form.obj; docs/orders-spec.md sections 1-2, docs/movement-spec.md section 11):
// the 512 Group slots (64 per player) that commands, group moves and formations work through, Form (formation slot
// layout), the group actions (move_near / move_to / halt / form / stance) and the per-frame group move of a unit.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sim/heroes.hpp"
#include "sim/objects.hpp"

namespace ron::sim {

class Game;

// ObjectData::has_general @646b00 is sim/heroes.hpp's (included here for the callers that always had it from groups.hpp).

constexpr int32_t kGroupMax = 128, kGroupsPerLeader = 64, kNumGroups = 512, kGroupOpenSlots = 46;

// Group (0x9d4 bytes). Walked: raw [+4, +0x4c) then the first `num` entries of each array (state-spec.md).
struct Group {
	int32_t id = -1, army = -1, num = 0, form = 0, stamp = 0, ox = 0, oy = 0, o_dist = 0;
	uint32_t o_angle = 0;
	int32_t disband = 0, order_num = 0, priority = 0, role = 0, think_frame = 0, new_speed = 0, speed = 0, form_num = 0;
	int8_t facing = 0, buildings = 0, who = 0, march = 0;
	std::array<int32_t, kGroupMax> off_x{}, off_y{}, curr_x{}, curr_y{};
	std::array<int8_t, kGroupMax> angles{};
	std::array<int16_t, kGroupMax> list{};
};

// Groups: 512 slots, last_group per leader 0..7, proc_group.
struct Groups {
	std::array<Group, kNumGroups> g{};
	std::array<int32_t, 8> last_group{};
	int32_t proc_group = 0;
	Groups();  // Groups::clear @713f20
};

// ---- Group (groups.cpp)
void group_clear(Group& gr, int32_t id);                                       // Group::clear @713e80
bool group_add(Game& g, Group& gr, int32_t o, int32_t who, int32_t down, int32_t konst);  // Group::add @714350
void group_kill(Game& g, Group& gr, int32_t o, int32_t who, int32_t down, int32_t konst);  // Group::kill @714110
int32_t group_get_num(Game& g, Group& gr);       // virtual get_num (compacts dead members)
int32_t group_get_num_cap(Game& g, Group& gr);   // virtual get_num_cap (captains)
void group_normalize(Game& g, Group& gr);        // Group::normalize @711540
int32_t group_find_leader(const Game& g, const Group& gr);  // GroupData::find_leader @70ccb0: list index or -1
void group_find_role(Game& g, Group& gr);        // Group::find_role @7081f0
void group_compute_speed(Game& g, Group& gr);    // Group::compute_speed @707f80
void group_sort(Game& g, Group& gr);             // Group::sort @708090
void group_update_positions(Game& g, Group& gr, const Unit& leader);  // Group::update_positions @713810
void group_kill_group_move(Game& g, Group& gr, int32_t id);           // Group::kill_group_move @7123f0
void group_refresh_group_order(Game& g, Group& gr, Unit& u);          // Group::refresh_group_order @713a50

// ---- Groups
int32_t groups_push_group(Game& g, int32_t who, Group& tmp, int32_t force);  // Groups::push_group @70f9e0
int32_t groups_get_open_slot(Game& g, int32_t who);                         // Groups::get_open_slot @6fa460
void groups_copy_group(Game& g, Group& dst, const Group& src);              // Groups::copy_group @6fa690
void groups_process(Game& g);                                               // Groups::process @6fa210

// ---- formations (Form::*, FormData::type_cat; docs/movement-spec.md section 11.2). forms.cpp.
constexpr int32_t kNumFormCat = 18, kNumForms = 10;
// KEEP (this port's addition, not an original formation; docs/sim-implementation.md "Formations"): the members keep
// their current arrangement relative to each other, turned from the way they face to the way they go, so a player can
// lay out an army (cavalry on the flanks, archers behind ...) and move it as a body. Laid out by form_compute_keep on
// forms[0]'s scratch; do_group_move's members then always follow their slots instead of the original's midpoint rule.
constexpr int32_t kFormKeep = 10;

// FormData (0xe90 bytes; Form adds a vbtable). One per formation in the global `forms` (Forms::init @0072e9a0), used
// as scratch by Group::compute_form and zeroed from +0x30 (o ..) after every Group::action_move_near. Walked raw from
// +0x28 (form ..), so saves see it (always zero past density between commands).
struct FormData {
	int32_t form = 0, density = 2;  // density: 2 for forms 0-4, 0 for SPARSE, 1 for 6-9 (Form::init @0072dda0)
	int32_t o = 0, idx = 0, who = 0;
	std::array<int32_t, kNumFormCat> num_category{}, x_spacing{}, y_spacing{};
	std::array<int32_t, kGroupMax> cat_id{}, category{}, to_x{}, to_y{}, off_x{}, off_y{};
	int32_t wedge = 0, total = 0, guarding = 0;
	std::array<int32_t, kNumFormCat> per{};
	std::array<std::array<int32_t, 4>, kNumFormCat> space{};
	int32_t reverse = 0, across = 0;
	void clear_scratch();  // memset(+0x30, 0, 0xe60): everything after density
};
void forms_init(std::array<FormData, kNumForms>& forms);  // Forms::init: form = index, density

// FormData::type_cat @72dfc0 (FormCatIndex) of a unit type for leader `who`; the Unit overload uses u.ptype.
int32_t form_type_cat(const Game& g, int32_t type, int32_t who);
int32_t form_type_cat(const Game& g, const Unit& u);
// Form::categorize @72e250 (calls Group::sort, so it reorders gr.list). `water` = the target tcoord is water.
// Returns F.o (the leader's o); F.idx is its list index.
int32_t form_categorize(Game& g, FormData& F, Group& gr, bool water);
// Form::compute_rows_and_columns @72d910 (rows/cols: 18 entries each; the original's are uninitialised stack arrays,
// here zero-filled by the caller).
void form_compute_rows_and_columns(FormData& F, int32_t* rows, int32_t* cols, int32_t width);
// Form::compute_dests @72cba0: per member slot destinations (F.to_*), offsets (F.off_*, Group.off_* in ucoords),
// Group.angles and Group.form_num.
void form_compute_dests(Game& g, FormData& F, Group& gr, const int32_t* rows, const int32_t* cols, int32_t x,
                        int32_t y, uint32_t angle, int32_t reverse);
// Form::compute @72e8e0: rows/columns + destinations, then Group.o_angle / o_dist from the leader slot.
void form_compute(Game& g, FormData& F, Group& gr, int32_t x, int32_t y, uint32_t angle, int32_t width,
                  int32_t facing);
// KEEP layout (form_compute_keep): slots = destination + each member's offset from the group's centre, the offsets
// turned by (travel angle - facing), where facing is the circular mean of the captains' angles (the travel angle
// itself when they disagree). Offsets relative to the leader in Group.off_*, every Group.angles[i] = 0.
FormData* form_compute_keep(Game& g, Group& gr, int32_t x, int32_t y, bool set_angle, uint32_t* angle, int32_t loc_x,
                            int32_t loc_y);
// Group::compute_form @707c80: picks forms[form], categorize, the formation angle (*angle in/out; set_angle = use the
// commanded angle, possibly flipped), the facing toggle, Form::compute and the flip. (loc_x, loc_y) = the group's
// current location (get_loc / get_loc_to). Returns the FormData, or null without a leader.
FormData* group_compute_form(Game& g, Group& gr, int32_t x, int32_t y, int32_t form, int32_t width, bool set_angle,
                             uint32_t* angle, int32_t loc_x, int32_t loc_y, int32_t guarding);

// ---- actions (process_<verb> -> Group::action_<verb>)
void group_action_halt(Game& g, Group& gr, int32_t p);  // @70d0c0
void group_action_stance(Game& g, Group& gr, int32_t s);  // @70d440
// Group::action_follow @6fd510 and action_guard @6fcd30 (`p4`: 1 skips siege members; the verbs pass 0).
void group_action_follow(Game& g, Group& gr, int32_t ox, int32_t whom, QueuePos q);
void group_action_guard(Game& g, Group& gr, int32_t ox, int32_t whom, QueuePos q, int32_t p4);
// Group::action_move_to @70fba0 -> action_move_near(tolerance 0) @704990.
void group_action_move_to(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q, bool set_angle, uint32_t angle,
                          int32_t orders, int32_t p7, int32_t form, int32_t width, int32_t disembark);
void group_action_move_near(Game& g, Group& gr, int32_t x, int32_t y, int32_t tol, QueuePos q, bool set_angle,
                            uint32_t angle, int32_t orders, int32_t p8, int32_t form, int32_t width, int32_t disembark);
// Group::action_form @707220 (`p5` != 0: called from the insert pattern, no second insert).
void group_action_form(Game& g, Group& gr, int32_t form, int32_t rotate, QueuePos q, int32_t p5 = 0);
// GroupData::get_loc @70e030 / get_loc_to @70c5d0 (return 1 without a leader, outputs untouched).
int32_t group_get_loc(Game& g, const Group& gr, int32_t* x, int32_t* y);
int32_t group_get_loc_to(Game& g, const Group& gr, int32_t* x, int32_t* y);
// Group::set_up_insert @70e520 / finish_insert @70e620 (the QUEUE_FIRST pattern).
std::vector<Order> group_set_up_insert(Game& g, Group& gr);
void group_finish_insert(Game& g, Group& gr, std::vector<Order>& saved);

// ---- unit side of group moves
// Unit::add_group_move_order @5e4710.
void add_group_move_order(Game& g, Unit& u, int32_t ux, int32_t uy, uint32_t angle, int32_t move_id, int32_t slot,
                          int32_t leader_o, int32_t leader_who, int32_t orders, QueuePos q, bool expl, int32_t reverse,
                          int32_t orig_x, int32_t orig_y, int32_t disembark);
int32_t do_group_move(Game& g, Unit& u, Order& m);  // Unit::do_group_move @5e79a0
void ungroup_move_order(Game& g, Unit& u);          // Unit::ungroup_move_order @5fd140
void unit_kill_group_move(Game& g, Unit& u, int32_t id);  // Unit::kill_group_move @5e3400

}  // namespace ron::sim
