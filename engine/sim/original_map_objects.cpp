// Field maps of the map objects: Objects header, Unit / Animal, Build (docs/original-save.md).
#include <algorithm>
#include <cstring>

#include "sim/heroes.hpp"
#include "sim/original_save.hpp"
#include "sim/original_map.hpp"

namespace ron::sim::omap {

namespace {

constexpr uint32_t kXor = 0x63637;  // x / y / z are stored ^ 0x63637

// SubObject + Object members shared by units and buildings (the PDB class of the map carries the same names).
template <class T> void object_fields(Builder<T>& b) {
	b.f("flags", &Object::flags).f("who", &Object::who).f("o", &Object::o);
	b.fx("z_internal", &Object::z, kXor).fx("x_internal", &Object::x, kXor).fx("y_internal", &Object::y, kXor);
	b.f("myhits", &Object::myhits).f("damage", &Object::damage).f("inside_down", &Object::inside_down);
	b.f("up", &Object::up).f("down", &Object::down).f("down_who", &Object::down_who);
	b.f("uid", &Object::uid).f("hold_frames", &Object::hold_frames);
	b.f("near_o", &Object::near_o).f("near_who", &Object::near_who).f("healing", &Object::healing);
	b.f("infiltrated", &Object::infiltrated).f("damage_frac", &Object::damage_frac).f("mylos", &Object::mylos);
	b.f("targeted", &Object::targeted).f("inside_down_who", &Object::inside_down_who).f("up_who", &Object::up_who);
	b.f("visible", &Object::visible).f("launch_frames", &Object::launch_timer);  // +0x41 (airbases, carriers, silos)
}

// The must_walk byte the writer stores at each level (Local steps SubObject / Object / Unit::must_walk) and the type local.
bool object_locals(const Object& o, const char* name, int64_t& v) {
	if (std::strcmp(name, "must_walk") == 0) {
		bool w = o.ptype != nullptr || o.alive() || o.hold_frames != 0;
		if (o.is_unit && static_cast<const Unit&>(o).o_up >= 0) w = true;
		v = w ? 1 : 0;
		return true;
	}
	if (std::strcmp(name, "type") == 0) {
		v = o.ptype ? o.type : 0;
		return true;
	}
	return false;
}

}  // namespace

const ClassMap& map_objects_header() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Game> b(m, "Objects", "Objects");
		b.walks({"Objects::walk_data"}).resolve([](Game& g, const Seg&) { return &g; });
		b.c("ammo_index", [](Game& g, const uint8_t* p) { scalar_load(g.ammo_counter, p, 4, true); },
		    [](const Game& g, uint8_t* p) { scalar_emit(g.ammo_counter, p, 4); });
		b.c("good_mark", [](Game& g, const uint8_t* p) { scalar_load(g.good_mark, p, 4, true); },
		    [](const Game& g, uint8_t* p) { scalar_emit(g.good_mark, p, 4); });
		// unit_mark / build_mark / obj_ctr: 9 of the 10 per-player slots are walked.
		b.raw(0x15c, 36, "unit_mark",
		      [](Game& g, const uint8_t* p) { for (int w = 0; w < 9; ++w) scalar_load(g.objects[size_t(w)].unit_mark, p + 4 * w, 4, true); },
		      [](const Game& g, uint8_t* p) { for (int w = 0; w < 9; ++w) scalar_emit(g.objects[size_t(w)].unit_mark, p + 4 * w, 4); });
		b.raw(0x184, 36, "build_mark",
		      [](Game& g, const uint8_t* p) { for (int w = 0; w < 9; ++w) scalar_load(g.objects[size_t(w)].build_mark, p + 4 * w, 4, true); },
		      [](const Game& g, uint8_t* p) { for (int w = 0; w < 9; ++w) scalar_emit(g.objects[size_t(w)].build_mark, p + 4 * w, 4); });
		b.raw(0x1d4, 18, "obj_ctr",
		      [](Game& g, const uint8_t* p) { for (int w = 0; w < 9; ++w) scalar_load(g.objects[size_t(w)].obj_ctr, p + 2 * w, 2, false); },
		      [](const Game& g, uint8_t* p) { for (int w = 0; w < 9; ++w) scalar_emit(g.objects[size_t(w)].obj_ctr, p + 2 * w, 2); });
		const dw::Description& d = dw::Description::get();
		static const int step_ammo = d.find_step(d.find_function("PtrArray<Ammo>::walk_data"), dw::StepKind::Container);
		static const int step_death = d.find_step(d.find_function("ObjectArray<DeathObj>::walk_data"), dw::StepKind::Container);
		static const int step_lists = d.find_step(d.find_function("MultiPtrArray<Object>::walk_data"), dw::StepKind::Container);
		b.arrays([](const Game& g, const Seg&, int step, int64_t base, ArraySpec& spec) {
			if (step == step_lists && base >= 4 && (base - 4) % 0x1c == 0 && (base - 4) / 0x1c < 9) {
				// lists[who]: a fixed 3000-slot MultiPtrArray (units below 2000, buildings from 2000), allocated once the leader has objects
				const int32_t who = int32_t((base - 4) / 0x1c);
				const ObjectList& L = g.objects[size_t(who)];
				const bool any = L.unit_mark > 0 || L.build_mark > kBuildBase;
				spec.count = any ? 3000 : 0;
				spec.grow_only = true;
				spec.keep_size = true;
				spec.extra_present = [&g, who](int32_t i) {
					const ObjectList& l = g.objects[size_t(who)];
					if (i < kBuildBase) return size_t(i) < l.units.size() && l.units[size_t(i)] != nullptr;
					return size_t(i - kBuildBase) < l.builds.size() && l.builds[size_t(i - kBuildBase)] != nullptr;
				};
				spec.dtype = [who](int32_t i) { return i >= kBuildBase ? 1 : (who == 8 || who == kGaiaWho) ? 3 : 0; };
				spec.bind_child = [who](OriginalShadow& sh, dw::Rec* r, int32_t i) {
					if (i < kBuildBase) {
						sh.rebind(r, map_unit(), Seg{who, i, 0});
						if (who == 8 || who == kGaiaWho) sh.rebind(r, map_animal(), Seg{who, i, 0});
					} else {
						sh.rebind(r, map_build(), Seg{who, i, 0});
					}
				};
				return true;
			}
			if (step == step_ammo && base == 0x11c) {
				spec.count = int32_t(g.ammo.size());
				spec.child = &map_ammo();
			} else if (step == step_death && base == 0x13c) {
				spec.count = int32_t(g.deaths.size());
				spec.child = &map_death();
			} else {
				return false;
			}
			spec.child_id = [](int32_t i) { return Seg{i, 0, 0}; };
			spec.grow_only = true;
			return true;
		});
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_guy() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Guy> b(m, "Guy", "Guy");
		b.walks({"GuyData::walk_data"}).resolve([](Game& g, const Seg& s) -> Guy* {
			Unit* u = g.unit(s.a, s.b);
			return u && size_t(s.c) < u->guys.size() ? &u->guys[size_t(s.c)] : nullptr;
		});
		b.f("type", &Guy::type).f("x", &Guy::x).f("y", &Guy::y).f("z", &Guy::z).f("angle", &Guy::angle).f("last_angle", &Guy::last_angle);
		b.f("turret_angles", &Guy::turret_angles).f("des_turret_angles", &Guy::des_turret_angles);
		b.f("track_dx", &Guy::track_dx).f("track_dy", &Guy::track_dy).f("des_x", &Guy::des_x).f("des_y", &Guy::des_y);
		b.f("des_angle", &Guy::des_angle).f("last_x", &Guy::last_x).f("last_y", &Guy::last_y).f("last_z", &Guy::last_z);
		b.f("cur_time", &Guy::cur_time).f("end_time", &Guy::end_time).f("last_time", &Guy::last_time).f("last_speed", &Guy::last_speed);
		b.f("avg_speed", &Guy::avg_speed).f("gpiece", &Guy::gpiece).f("o", &Guy::o).f("ox", &Guy::ox).f("variation", &Guy::variation);
		b.f("off_x", &Guy::off_x).f("off_y", &Guy::off_y).f("node_flags", &Guy::node_flags).f("des_node_flags", &Guy::des_node_flags);
		b.f("guy_flags", &Guy::guy_flags).f("cur_anim", &Guy::cur_anim).f("stopped", &Guy::stopped).f("hold_attack", &Guy::hold_attack);
		b.f("whom", &Guy::whom).f("queued_attack", &Guy::queued_attack).f("who", &Guy::who).f("guy_num", &Guy::guy_num);
		b.f("bank", &Guy::bank).f("last_bank", &Guy::last_bank).f("pitch", &Guy::pitch).f("last_pitch", &Guy::last_pitch);  // aircraft banking (GuyData +0x44..+0x50)
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_pathdata() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<PathData> b(m, "PathData", "PathData");
		b.walks({"Stack<PathData>::walk_data"}).resolve([](Game& g, const Seg& s) -> PathData* {
			Unit* u = g.unit(s.a, s.b);
			return u && size_t(s.c) < u->path.size() ? &u->path[size_t(s.c)] : nullptr;
		});
		b.c("to_x", [](PathData& p, const uint8_t* q) { scalar_load(p.x, q, 4, true); }, [](const PathData& p, uint8_t* q) { scalar_emit(p.x, q, 4); });
		b.c("to_y", [](PathData& p, const uint8_t* q) { scalar_load(p.y, q, 4, true); }, [](const PathData& p, uint8_t* q) { scalar_emit(p.y, q, 4); });
		b.c("tolerance", [](PathData& p, const uint8_t* q) { scalar_load(p.tol, q, 4, true); }, [](const PathData& p, uint8_t* q) { scalar_emit(p.tol, q, 4); });
		b.c("flags", [](PathData& p, const uint8_t* q) { scalar_load(p.flags, q, 4, false); }, [](const PathData& p, uint8_t* q) { scalar_emit(p.flags, q, 4); });
		m.finish();
		return m;
	}();
	return m;
}

// ---- orders: one map per walked order class. Images use the class's own offsets (the walk functions are called on the full
// object); `flags` is the UnitOrder subobject's byte at its base + 4.
namespace {

Order* order_of(Game& g, const Seg& s) {
	Unit* u = g.unit(s.a, s.b);
	return u && size_t(s.c) < u->orders.size() ? &u->orders[size_t(s.c)] : nullptr;
}

void order_flags(Builder<Order>& b, int32_t off) {
	b.raw(off, 1, "flags", [](Order& o, const uint8_t* p) { o.flags = p[0]; }, [](const Order& o, uint8_t* p) { p[0] = o.flags; });
}
void order_target(Builder<Order>& b) { b.f("ox", &Order::ox).f("whom", &Order::whom).f("uid", &Order::uid); }
void order_move(Builder<Order>& b) {
	b.f("x", &Order::x).f("y", &Order::y).f("angle", &Order::angle).f("dest", &Order::dest).f("tolerance", &Order::tolerance);
	b.f("pause", &Order::pause).f("retry", &Order::retry).f("attempts", &Order::attempts).f("timer", &Order::timer).f("facing", &Order::facing);
	b.f("dest_x", &Order::dest_x).f("dest_y", &Order::dest_y).f("last_x", &Order::last_x).f("last_y", &Order::last_y);
	b.f("coll_x", &Order::coll_x).f("coll_y", &Order::coll_y).f("orig_x", &Order::orig_x).f("orig_y", &Order::orig_y);
	b.f("off_x", &Order::off_x).f("off_y", &Order::off_y);
}
void order_group(Builder<Order>& b) {
	b.f("oxx", &Order::oxx).f("whose", &Order::whose).f("id", &Order::id).f("form_id", &Order::form_id);
	b.f("group_angle", &Order::group_angle);
}
void order_attack(Builder<Order>& b) {
	order_target(b);
	b.f("def_x", &Order::def_x).f("def_y", &Order::def_y).f("mandatory", &Order::mandatory).f("defensive", &Order::defensive);
	b.f("in_range", &Order::in_range).f("ever_in_range", &Order::ever_in_range).f("new_ord", &Order::new_ord);
}

// PatrolOrder (AIR_PATROL, GROUP_PATROL): x_pos at +4 and y_pos at +0x20, SimpleArray<Coord>, one waypoint per element.
void patrol_arrays(Builder<Order>& b) {
	static const int step_coord = dw::Description::get().find_step(dw::Description::get().find_function("SimpleArray<Coord>::walk_data"), dw::StepKind::Container);
	b.arrays([](const Game& g, const Seg& s, int step, int64_t base, ArraySpec& spec) {
		const Order* o = order_of(const_cast<Game&>(g), s);
		if (!o || step != step_coord || (base != 4 && base != 0x20)) return false;
		spec.count = int32_t(o->wp.size());
		spec.simple = true;
		const int axis = base == 4 ? 0 : 1;
		spec.block = [o, axis](std::vector<uint8_t>& out) {
			out.resize(o->wp.size() * 4);
			for (size_t i = 0; i < o->wp.size(); ++i) put_le(out.data() + 4 * i, 4, uint32_t(o->wp[i][size_t(axis)]));
		};
		return true;
	});
}

ClassMap make_order_map(const char* name, const char* cls, int32_t flags_off, void (*fill)(Builder<Order>&)) {
	ClassMap m;
	Builder<Order> b(m, name, cls);
	{
		const std::string fn = std::string(cls == std::string("AttackToOrder") ? "AttackToOrder" : cls) + "::walk_data";
		b.walks({fn.c_str()});
	}
	b.resolve([](Game& g, const Seg& s) { return order_of(g, s); });
	if (fill) fill(b);
	order_flags(b, flags_off);
	m.finish();
	return m;
}

}  // namespace

const ClassMap& map_order(int t) {
	static const ClassMap move = make_order_map("MoveOrder", "AttackToOrder", 0x58, [](Builder<Order>& b) { order_move(b); });
	static const ClassMap form = make_order_map("FormOrder", "FormOrder", 0x60, [](Builder<Order>& b) {
		order_move(b);
		b.f("newform", &Order::newform).f("delay", &Order::delay);
	});
	static const ClassMap gmove = make_order_map("GroupMoveOrder", "GroupMoveOrder", 0x74, [](Builder<Order>& b) {
		order_move(b);
		order_group(b);
		b.f("in_group", &Order::in_group);
	});
	static const ClassMap target = make_order_map("TargetOrder", "TargetOrder", 0x1c, [](Builder<Order>& b) { order_target(b); });
	static const ClassMap gather = make_order_map("GatherOrder", "GatherOrder", 0x30, [](Builder<Order>& b) {
		order_target(b);
		b.f("tx", &Order::g_tx).f("ty", &Order::g_ty).f("build_type", &Order::build_type).f("wait", &Order::wait);
		b.f("goto_build", &Order::goto_build).f("non_flat_gather", &Order::non_flat).f("dist_mod", &Order::dist_mod).f("been_there", &Order::been_there);
	});
	static const ClassMap attack = make_order_map("AttackOrder", "AttackOrder", 0x2c, [](Builder<Order>& b) { order_attack(b); });
	static const ClassMap gattack = make_order_map("GroupAttackOrder", "GroupAttackOrder", 0x50, [](Builder<Order>& b) {
		order_attack(b);
		order_group(b);
	});
	static const ClassMap cast = make_order_map("CastOrder", "CastOrder", 0x2c, [](Builder<Order>& b) {
		order_target(b);
		b.f("x", &Order::x).f("y", &Order::y).f("paid", &Order::paid).f("spell", &Order::spell);
	});
	static const ClassMap trade = make_order_map("TradeOrder", "TradeOrder", 0x30, [](Builder<Order>& b) {
		order_target(b);
		b.f("oxx", &Order::oxx).f("whose", &Order::whose).f("started", &Order::started).f("loaded", &Order::loaded).f("uid2", &Order::uid2);
	});
	static const ClassMap garrison = make_order_map("GarrisonOrder", "GarrisonOrder", 0x20, [](Builder<Order>& b) {
		order_target(b);
		b.f("search", &Order::find);
	});
	// Aircraft orders (the members the aircraft port added to Order; aircraft-spec 3).
	static const ClassMap strafe = make_order_map("StrafeOrder", "StrafeOrder", 0x50, [](Builder<Order>& b) {
		order_attack(b);
		b.f("oxx", &Order::oxx).f("whose", &Order::whose).f("cruising_alt", &Order::cruising_alt).f("sharp_turn", &Order::sharp_turn);
		b.f("old", &Order::old).f("returning", &Order::returning).f("xx", &Order::xx).f("yy", &Order::yy);
	});
	static const ClassMap airpatrol = make_order_map("AirPatrolOrder", "AirPatrolOrder", 0x64, [](Builder<Order>& b) {
		b.f("waypoint", &Order::wp_index).f("oxx", &Order::oxx).f("whose", &Order::whose).f("cruising_alt", &Order::cruising_alt);
		b.f("sharp_turn", &Order::sharp_turn).f("old", &Order::old).f("returning", &Order::returning);
		patrol_arrays(b);
	});
	static const ClassMap airattack = make_order_map("AirAttackGroundOrder", "AirAttackGroundOrder", 0x44, [](Builder<Order>& b) {
		b.f("att_x", &Order::x).f("att_y", &Order::y).f("oxx", &Order::oxx).f("whose", &Order::whose).f("cruising_alt", &Order::cruising_alt);
		b.f("sharp_turn", &Order::sharp_turn).f("old", &Order::old).f("returning", &Order::returning);
	});
	static const ClassMap special = make_order_map("SpecialAnimOrder", "SpecialAnimOrder", 0x4, [](Builder<Order>& b) {
		b.f("type", &Order::sa_type).f("started", &Order::sa_started).f("frames", &Order::sa_frames).f("data1", &Order::sa_data1);
		b.f("data2", &Order::sa_data2).f("data3", &Order::sa_data3).f("data4", &Order::sa_data4).f("ox", &Order::ox).f("whom", &Order::whom);
	});
	// Classes with no modelled field but the flags byte.
	static const ClassMap follow = make_order_map("FollowOrder", "FollowOrder", 0x28, [](Builder<Order>& b) {
		order_target(b);
		b.f("oxx", &Order::oxx).f("whose", &Order::whose).f("uid2", &Order::uid2);
	});
	static const ClassMap guard = make_order_map("GuardOrder", "GuardOrder", 0x34, [](Builder<Order>& b) {
		order_target(b);
		// (no members of their own: dx / dy = def_x / def_y, the guard point = x / y, idle = timer, retry = pause; see orders.hpp)
		b.f("dx", &Order::def_x).f("dy", &Order::def_y).f("guard_x", &Order::x).f("guard_y", &Order::y);
		b.f("idle", &Order::timer).f("retry", &Order::pause);
	});
	// GROUP_PATROL: the waypoint index, the arrays and the GroupOrder part (leader o / who, command id, member index; orders_patrol.cpp).
	static const ClassMap gpatrol = make_order_map("GroupPatrolOrder", "GroupPatrolOrder", 0x60, [](Builder<Order>& b) {
		b.f("waypoint", &Order::wp_index);
		b.f("oxx", &Order::oxx).f("whose", &Order::whose).f("id", &Order::id).f("form_id", &Order::form_id).f("group_angle", &Order::group_angle);
		patrol_arrays(b);
	});
	// ATTACK_GROUND: the point, the accuracy (Order::tolerance) and attack_unit (Order::attempts) of orders_patrol.cpp.
	static const ClassMap attackground = make_order_map("AttackGroundOrder", "AttackGroundOrder", 0x1c, [](Builder<Order>& b) {
		b.f("att_x", &Order::x).f("att_y", &Order::y).f("accuracy", &Order::tolerance).f("attack_unit", &Order::attempts);
	});
	static const ClassMap think = make_order_map("ThinkOrder", "UnitOrder", 0x4, nullptr);
	switch (t) {
		case 1: case 2: case 3: case 4: return move;
		case 6: case 8: case 9: case 13: return target;
		case 7: return gather;
		case 10: return attack;
		case 11: return follow;
		case 12: return guard;
		case 14: return cast;
		case 15: return trade;
		case 16: return strafe;
		case 17: return airpatrol;
		case 18: return form;
		case 19: case 21: return gmove;
		case 20: return gattack;
		case 22: return gpatrol;
		case 23: return attackground;
		case 24: return airattack;
		case 25: return special;
		case 26: return garrison;
		case 27: return think;
		default: return think;  // callers check the type first
	}
}

const ClassMap& map_order_node() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Order> b(m, "OrderNode", "UnitOrder");
		b.walks({}).resolve([](Game& g, const Seg& s) { return order_of(g, s); });
		b.locals([](const Game& g, const Seg& s, const char* name, int, int64_t& v) {
			if (std::strcmp(name, "order_type") != 0) return false;
			const Unit* u = g.unit(s.a, s.b);
			if (!u || size_t(s.c) >= u->orders.size()) return false;
			v = int64_t(u->orders[size_t(s.c)].type);
			return true;
		});
		static const int step_data = dw::Description::get().find_step(dw::Description::get().find_function("OrderList::walk_data"), dw::StepKind::Deref);
		b.children([](const Game& g, const Seg& s, int step, int64_t, int64_t, ChildSpec& c) {
			if (step != step_data) return false;
			const Unit* u = g.unit(s.a, s.b);
			if (!u || size_t(s.c) >= u->orders.size()) return false;
			const int t = int(u->orders[size_t(s.c)].type);
			c.map = &map_order(t);
			c.id = s;
			c.reuse_local = "order_type";
			c.reuse_value = t;
			return true;
		});
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_unit() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Unit> b(m, "Unit", "Unit");
		b.walks({"SubObject::walk_data", "Object::walk_data", "Unit::walk_data"}).resolve([](Game& g, const Seg& s) -> Unit* {
			return g.unit(s.a, s.b);
		});
		object_fields(b);
		b.locals([](const Game& g, const Seg& s, const char* name, int, int64_t& v) {
			const Unit* u = g.unit(s.a, s.b);
			return u && object_locals(*u, name, v);
		});
		b.f("collide_frame", &Unit::collide_frame).f("damage_frame", &Unit::damage_frame).f("angle", &Unit::angle);
		// +0x54: rare (merchants / fishermen / scholar militia) or air_alt (aircraft); the engine keeps two members.
		b.c("rare", [](Unit& u, const uint8_t* p) { scalar_load(u.rare, p, 4, true); u.air_alt = u.rare; },
		    [](const Unit& u, uint8_t* p) { scalar_emit(u.ptype && u.ptype->domain == 2 ? u.air_alt : u.rare, p, 4); });
		b.f("dest_angle", &Unit::dest_angle).f("trench_angle", &Unit::trench_angle).f("tolerance", &Unit::tolerance);
		b.f("queue_time", &Unit::queue_time).f("unit_masks", &Unit::unit_masks).f("unit_masks2", &Unit::unit_masks2);
		b.f("orders_x", &Unit::orders_x).f("orders_y", &Unit::orders_y).f("los_x", &Unit::los_x).f("los_y", &Unit::los_y);
		b.f("group", &Unit::group).f("inside_up", &Unit::inside_up).f("supply", &Unit::supply);
		// +0x86: one slot, several meanings (hero record / special record / worker's held doober / caravan slot).
		b.c("cara",
		    [](Unit& u, const uint8_t* p) {
			    int16_t v;
			    scalar_load(v, p, 2, true);
			    u.cara = u.doober = u.hero = -1;
			    if (u.type >= 50 && u.type <= 53) u.doober = v;
			    else if (unit_is_special(u) && u.type > 53) u.hero = v;
			    else u.cara = v;
		    },
		    [](const Unit& u, uint8_t* p) {
			    const int16_t v = (u.type >= 50 && u.type <= 53) ? u.doober : (unit_is_special(u) && u.type > 53) ? u.hero : u.cara;
			    scalar_emit(v, p, 2);
		    });
		b.f("collide", &Unit::collide).f("collide_o", &Unit::collide_o).f("collide_guy", &Unit::collide_guy);
		b.f("o_up", &Unit::o_up).f("o_down", &Unit::o_down).f("gather_down", &Unit::gather_down).f("good_obj", &Unit::good_obj);
		b.f("mana_burn", &Unit::mana_burn).f("spell_time", &Unit::spell_time).f("myspeed", &Unit::myspeed).f("myarmor", &Unit::myarmor);
		b.f("attrition", &Unit::attrition).f("num_queued", &Unit::num_queued).f("cavarch_o", &Unit::cavarch_o);
		b.f("damage_o", &Unit::damage_o).f("cavarch_uid", &Unit::cavarch_uid).f("cavarch_who", &Unit::cavarch_who);
		b.f("damage_who", &Unit::damage_who).f("form", &Unit::form).f("form_mod", &Unit::form_mod).f("full", &Unit::full);
		b.f("waiting", &Unit::waiting).f("recharging", &Unit::recharging).f("path_recursion", &Unit::path_recursion);
		b.f("idle", &Unit::idle).f("stance", &Unit::stance).f("safe", &Unit::safe).f("collide_who", &Unit::collide_who);
		b.f("inside_up_who", &Unit::inside_up_who).f("guy_mark", &Unit::guy_mark).f("play", &Unit::play);
		// path: Stack<PathData> at +0xb8: raw [+4, +0xd) = i32 size (allocation), i32 length, u8 increment; the elements are Deref children.
		b.raw(0xb8 + 4, 9, "path",
		      [](Unit& u, const uint8_t* p) {  // the elements are loaded one by one from the children
			      u.path_size = int32_t(get_le(p, 4));
			      u.path_inc = p[8];
		      },
		      [](const Unit& u, uint8_t* p) {
			      // Stack<PathData>: size (allocation), length, increment. Push grows by `increment` when size <= length, so the allocation
			      // follows the longest the stack ever was, not its length; a stack that was never initialised (size 0) stays 0 until it grows.
			      int32_t size = u.path_size, len = int32_t(u.path.size());
			      const int inc = u.path_inc ? u.path_inc : 10;
			      while (size < len) size += inc;
			      scalar_emit(size, p, 4);
			      scalar_emit(len, p + 4, 4);
			      p[8] = u.path_inc;
		      });
		const dw::Description& d = dw::Description::get();
		static const int step_guys = d.find_step(d.find_function("PtrArray<Guy>::walk_data"), dw::StepKind::Container);
		static const int step_path = d.find_step(d.find_function("Stack<PathData>::walk_data"), dw::StepKind::Deref);
		b.children([](const Game&, const Seg& s, int step, int64_t base, int64_t index, ChildSpec& c) {
			if (step != step_path || base != 0xb8) return false;
			c.map = &map_pathdata();
			c.id = Seg{s.a, s.b, int32_t(index)};
			return true;
		});
		static const int step_orders = d.find_step(d.find_function("OrderList::walk_data"), dw::StepKind::Container);
		b.arrays([](const Game& g, const Seg& s, int step, int64_t base, ArraySpec& spec) {
			const Unit* u = g.unit(s.a, s.b);
			if (!u) return false;
			if (step == step_guys && base == 0xe4) {
				spec.count = int32_t(u->guys.size());
				spec.child = &map_guy();
			} else if (step == step_orders && base == 0xc8) {
				spec.count = int32_t(u->orders.size());  // file order = execution order: node i is orders[i] (written tail first)
				spec.child = &map_order_node();
				spec.keep_size = true;
			} else {
				return false;
			}
			const int32_t a = s.a, bb = s.b;
			spec.child_id = [a, bb](int32_t i) { return Seg{a, bb, i}; };
			return true;
		});
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_animal() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Unit> b(m, "Animal", "Animal");
		b.walks({"AnimalData::walk_data"}).resolve([](Game& g, const Seg& s) -> Unit* { return g.unit(s.a, s.b); });
		b.f("ox", &Unit::animal_ox).f("whom", &Unit::animal_whom).f("aid", &Unit::animal_aid);
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_queue_item() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<QueueItem> b(m, "QueueItem", "QueueItem");
		b.walks({"BuildQueue::walk_data"}).resolve([](Game& g, const Seg& s) -> QueueItem* {
			Build* bd = g.build(s.a, s.b);
			return bd && size_t(s.c) < bd->build_queue.queue.size() ? &bd->build_queue.queue[size_t(s.c)] : nullptr;
		});
		b.f("job_counter", &QueueItem::job_counter).f("type", &QueueItem::type).f("good", &QueueItem::good).f("cost", &QueueItem::cost);
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_tcoord() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<std::array<int32_t, 2>> b(m, "TCoordData", "TCoordData");
		b.walks({"TCoordData::walk_data"}).resolve([](Game& g, const Seg& s) -> std::array<int32_t, 2>* {
			Build* bd = g.build(s.a, s.b);
			return bd && size_t(s.c) < bd->gather_from.size() ? &bd->gather_from[size_t(s.c)] : nullptr;
		});
		b.raw(0, 4, "tx", [](std::array<int32_t, 2>& a, const uint8_t* p) { scalar_load(a[0], p, 4, true); },
		      [](const std::array<int32_t, 2>& a, uint8_t* p) { scalar_emit(a[0], p, 4); });
		b.raw(4, 4, "ty", [](std::array<int32_t, 2>& a, const uint8_t* p) { scalar_load(a[1], p, 4, true); },
		      [](const std::array<int32_t, 2>& a, uint8_t* p) { scalar_emit(a[1], p, 4); });
		m.finish();
		return m;
	}();
	return m;
}

// A node of a building's rally-point list. The file writes the list tail first; the engine keeps it from the head.
const ClassMap& map_gather_point() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<GatherPoint> b(m, "GatherPoint", "GatherPoint");
		b.walks({"GatherPoint::walk_data"}).resolve([](Game& g, const Seg& s) -> GatherPoint* {
			Build* bd = g.build(s.a, s.b);
			const int64_t n = bd ? int64_t(bd->gather.size()) : 0;
			return bd && s.c >= 0 && s.c < n ? &bd->gather[size_t(n - 1 - s.c)] : nullptr;
		});
		b.f("x", &GatherPoint::x).f("y", &GatherPoint::y).f("action", &GatherPoint::action);
		b.locals([](const Game&, const Seg&, const char* name, int, int64_t& v) {
			if (std::strcmp(name, "dtype") != 0) return false;
			v = 0;
			return true;
		});
		m.finish();
		return m;
	}();
	return m;
}

const ClassMap& map_build() {
	static const ClassMap m = [] {
		ClassMap m;
		Builder<Build> b(m, "Build", "Build");
		b.walks({"SubObject::walk_data", "Object::walk_data", "WallData::walk_data", "BuildData::walk_data"}).resolve([](Game& g, const Seg& s) -> Build* {
			return g.build(s.a, s.b);
		});
		object_fields(b);
		b.locals([](const Game& g, const Seg& s, const char* name, int, int64_t& v) {
			const Build* bd = g.build(s.a, s.b);
			return bd && object_locals(*bd, name, v);
		});
		// WallData [0x48, 0x66)
		b.f("job_counter", &Build::job_counter).f("job_counter_2", &Build::job_counter_2).f("constr_time", &Build::constr_time);
		b.f("construct_hits", &Build::construct_hits).f("frame_started", &Build::frame_started);
		b.fu("build_masks", &Build::build_masks).f("ever_seen", &Build::ever_seen).f("ever_seen_completed", &Build::ever_seen_completed);
		b.f("helpers", &Build::helpers).f("demolition", &Build::demolition);
		b.f("orig_type", &Build::orig_type);
		// BuildData [0x70, 0x86)
		b.f("gather_down", &Build::gather_down).f("city", &Build::city).f("city_down", &Build::city_down).f("wonder", &Build::wonder);
		b.f("fort", &Build::fort).f("recharging", &Build::recharging).f("attack_ox", &Build::attack_ox).f("stance", &Build::stance);
		b.f("founder", &Build::founder).f("gather_max", &Build::gather_max).f("attack_whom", &Build::attack_whom);
		b.f("queued", &Build::queued).f("max_age", &Build::max_age).f("infiltrate", &Build::infiltrate);
		// BuildQueue at +0x88: queue_size, then the slots as Deref children
		b.raw(0x88, 4, "queue_size",
		      [](Build& bd, const uint8_t* p) {
			      int32_t n;
			      scalar_load(n, p, 4, true);
			      if (n <= 0) bd.build_queue.close();
			      else { bd.build_queue.queue_size = n; bd.build_queue.queue.assign(size_t(n), QueueItem{}); }
		      },
		      [](const Build& bd, uint8_t* p) { scalar_emit(bd.build_queue.queue_size, p, 4); });
		// the MiningList's deposit bytes
		b.raw(0xb4, 2, "gather_from.mtn",
		      [](Build& bd, const uint8_t* p) { scalar_load(bd.mtn, p, 1, true); scalar_load(bd.cliff, p + 1, 1, true); },
		      [](const Build& bd, uint8_t* p) { scalar_emit(bd.mtn, p, 1); scalar_emit(bd.cliff, p + 1, 1); });
		const dw::Description& d = dw::Description::get();
		static const int step_queue = d.find_step(d.find_function("BuildQueue::walk_data"), dw::StepKind::Deref);
		static const int step_mining = d.find_step(d.find_function("Array<TCoordData>::walk_data"), dw::StepKind::Container);
		static const int step_gather = d.find_step(d.find_function("PtrLinkListAbstract<GatherPoint,unsigned_char>::walk_data"), dw::StepKind::Container);
		b.children([](const Game&, const Seg& s, int step, int64_t base, int64_t index, ChildSpec& c) {
			if (step != step_queue || base != 0x88) return false;
			c.map = &map_queue_item();
			c.id = Seg{s.a, s.b, int32_t(index)};
			return true;
		});
		b.arrays([](const Game& g, const Seg& s, int step, int64_t base, ArraySpec& spec) {
			const Build* bd = g.build(s.a, s.b);
			if (!bd) return false;
			if (step == step_mining && base == 0x98) {
				spec.count = int32_t(bd->gather_from.size());
				spec.child = &map_tcoord();
			} else if (step == step_gather && base == 0xb8) {
				spec.count = int32_t(bd->gather.size());
				spec.child = &map_gather_point();
				spec.keep_size = true;
			} else {
				return false;
			}
			const int32_t a = s.a, bb = s.b;
			spec.child_id = [a, bb](int32_t i) { return Seg{a, bb, i}; };
			return true;
		});
		m.finish();
		return m;
	}();
	return m;
}

}  // namespace ron::sim::omap
