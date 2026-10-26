// Death, corpses and slot holding (docs/combat-spec.md section 6, docs/frame-spec.md section 8.2): Object::die,
// Unit::close (bookkeeping, uber-chain relink, death animation draws), Object::close, Objects::kill_guy /
// add_death and the DeathObj pool.
#include <algorithm>

#include "sim/aircraft.hpp"
#include "sim/capture.hpp"
#include "sim/combat_internal.hpp"
#include "sim/economy.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/pathfinder.hpp"
#include "sim/production.hpp"
#include "sim/spells.hpp"
#include "sim/victory.hpp"

namespace ron::sim {

using namespace combat_detail;

namespace {

bool blocks_while_dead(const Object& ob) { return ob.ptype && (ob.ptype->unit_flags & utflag::kBlocksWhileDead); }

// Objects::remove @658980 (also in objects.cpp): lower the mark while the top slot is dead, unheld and unchained.
void objects_remove_units(Game& g, int32_t who) {
	ObjectList& L = g.objects[size_t(who)];
	while (L.unit_mark > 0) {
		const Unit* u = g.unit(who, L.unit_mark - 1);
		if (!u || u->alive() || u->hold_frames != 0 || u->o_up >= 0) break;
		L.unit_mark--;
	}
}
void objects_remove_builds(Game& g, int32_t who) {
	ObjectList& L = g.objects[size_t(who)];
	while (L.build_mark > kBuildBase) {
		const Object* b = g.object(who, L.build_mark - 1);
		if (!b || b->alive() || b->hold_frames != 0) break;
		L.build_mark--;
	}
}

// DeathObj::clear_blocking @8d4ac0: unlink the dead unit and clear the corpse's footprint.
void clear_blocking(Game& g, DeathObj& d) {
	Unit* u = g.unit(d.who, d.o);
	if (!u) return;
	remove_from_world(g, *u);
	const int32_t tx = tcoord_of(d.x), ty = tcoord_of(d.y);
	const int32_t region = g.world.in_t(tx, ty) ? g.world.get_tregion(tx, ty) : -1;
	g.world.stamp_footprint(ucoord_of(d.x), ucoord_of(d.y), u->ptype->new_block_radius, region, false);
}

bool death_blocks(const Game& g, const DeathObj& d) {
	const Unit* u = g.unit(d.who, d.o);
	return u && blocks_while_dead(*u);
}

}  // namespace

// ---------------------------------------------------------------- die / close

// Object::close @647160 (the alive part; contents ejection and scenario ignore lists: TODO).
void object_close(Game& g, Object& ob, int32_t death) {
	if (ob.alive()) {
		if (!on_map(ob)) {
			// A garrisoned unit leaves its container first (Unit::close already dropped the death animation).
			unit_remove_from_inside(g, static_cast<Unit&>(ob));
			remove_from_world(g, ob);
		} else {
			// Whatever is inside comes out (a building raises EJECTING) or, in an aircraft carrier / silo, dies.
			if (num_inside(g, ob, 1) > 0) {
				if (can_carry_domain(g, ob, kDomainAir)) kill_contents(g, ob);
				else eject_contents(g, ob, 1, -1, 0, 1);
			}
			if (death == 0 || !blocks_while_dead(ob)) remove_from_world(g, ob);
		}
		if (ob.is_unit) objects_remove_units(g, ob.who);
		else objects_remove_builds(g, ob.who);
		ob.hold_frames = 30;
	}
	ob.damage = 0;
	ob.flags = 0;
}


void object_die(Game& g, Object& ob, int32_t death, int32_t gpiece, float deg) {
	// @647080
	if (ob.is_unit) unit_close(g, static_cast<Unit&>(ob), death, gpiece, deg);
	else build_close(g, static_cast<Build&>(ob), death);
	int32_t hold = 1;
	if (ob.ptype && raw_max_range(g, *ob.ptype) != 0) {
		// Keep the slot while this shooter's projectiles fly. TODO(combat-spec 11.5): the global added here
		// (@c0a888, "nuke_effect+0x108") is taken as 0.
		for (const Ammo& a : g.ammo) {
			if (!a.in_use() || a.o != ob.o || a.who != ob.who) continue;
			const int32_t v = 0 + 1 + int32_t(a.total_time - a.cur_time);
			hold = std::max(hold, v);
		}
	}
	hold = std::max<int32_t>(hold, ob.hold_frames);
	ob.hold_frames = uint16_t(hold);
}

void unit_die(Game& g, Unit& u, int32_t dt) { object_die(g, u, dt, -1, 0.0f); }

void build_close(Game& g, Build& b, int32_t death) {
	// Build::close @628980 -> Wall::close -> Object::close (production.hpp: queues, gather tiles, cities, wonders).
	hotkey_groups_kill(g, b.who, b.o);  // HotKeyGroups::kill
	build_close_production(g, b, death);
}

void unit_close(Game& g, Unit& u, int32_t death, int32_t gpiece, float deg) {
	// @60ee50
	bool blocked_merchant = false;
	const int32_t who = u.who;
	if (u.alive()) {
		hotkey_groups_kill(g, who, u.o);  // HotKeyGroups::kill (control groups; no simulation reads them)
		if (who < 9) g.total_units--;
		if (u.unit_masks & umask::kEntrenched) {
			u.unit_masks2 &= ~(umask2::kMarineEntrench | umask2::kAntipaterEntr);
			u.unit_masks &= ~umask::kEntrenched;
		}
		if (who < 8) g.leaders[size_t(who)].flags |= 0x800000u;  // LEADER_CALC_UNIT_STATS
		// A government hero's death starts the respawn clock (gov_hero_respawn seconds of 15 frames; heroes-spec 5.2).
		if (who < 8 && unit_is_gov_hero(u)) g.leaders[size_t(who)].gov_hero_frame = RON_K(g, "gov_hero_respawn") * 15 + g.frame;
		if ((u.ptype->unit_flags2 & 0x40) && u.supply >= 0 && who < 8) supplies_close_supply(g, who, u.supply);
		// TODO(economy / heroes): spies (Kremlin), carried production, planes leaving their
		// carrier, gatherer doobers, Lakota / American recompute flags.
		const int32_t t = u.type;
		if (t == 0x3d || t == 0x3e || t == 400) {
			if (who < kNumLeaders) g.leaders[size_t(who)].flags |= leaderflag::kRecompute;
			if (!(u.unit_masks & umask::kPacked) && !cast_is_packing(u)) {  // (a merchant mid-pack still holds its block)
				// An unpacked merchant's 2x2 market block is released; no death animation.
				const int32_t tx = tcoord_of(u.x), ty = tcoord_of(u.y);
				const int32_t cx[4] = {tx, tx - 1, tx, tx - 1}, cy[4] = {ty, ty, ty - 1, ty - 1};
				for (int k = 0; k < 4; k++)
					if (g.world.in_t(cx[k], cy[k])) g.world.set_blocked_at(cx[k], cy[k], false);
				blocked_merchant = true;
			}
		}
		// ---- uber chain (o_up +0x8e, o_down +0x90): the dead member is re-appended at the chain tail so that its
		// slot stays reserved (find_free needs o_up < 0) until the last member dies.
		const int16_t up = u.o_up;
		if (up >= 0) {
			if (Unit* p = g.unit(who, up)) p->o_down = u.o_down;
		}
		bool last = false;
		if (u.o_down < 0) {
			last = up < 0;
		} else {
			Unit* d = g.unit(who, u.o_down);
			if (d) d->o_up = up;
			if (!d || !d->alive()) last = up < 0;
		}
		if (!last && (up >= 0 || u.o_down >= 0)) {
			// Re-append at the tail (LAB_0060f679).
			int32_t tail = u.o;
			for (int32_t n = u.o_down, guard = 0; n >= 0 && guard < kUnitLimit; guard++) {
				tail = n;
				const Unit* q = g.unit(who, n);
				n = q ? q->o_down : -1;
			}
			if (tail == u.o) {
				if (Unit* p = g.unit(who, up)) p->o_down = u.o;
				u.o_down = -1;
				// o_up unchanged
			} else {
				if (Unit* q = g.unit(who, tail)) q->o_down = u.o;
				u.o_down = -1;
				u.o_up = int16_t(tail);
			}
		} else {
			// The last member of the chain: the leader's counters drop (production-spec 6.10: control, active,
			// track_unit_type for counted units; caravans), then every member's links are released. A leader left with no
			// unit and no city is defeated by conquest (victory-spec 7). TODO: Conquer the World seeds.
			if (unit_counted_type(g, u)) {
				if (!(u.unit_masks & umask::kDecoy)) {
					g.leaders[size_t(who)].control -= u.ptype->control_cost;
					g.leaders[size_t(who)].active--;
					track_unit_type(g, who, u.type, -1, u.o);
				}
				// Unit::close @60ee50: who < 8, no active unit and no city, not the editor / free mode (semaphore byte1 & 8),
				// and flags2 bit 0 clear (NO_CITY_DEFEAT).
				if (who < 8 && g.leaders[size_t(who)].active == 0 && g.leaders[size_t(who)].city_num == 0 && !g.editor &&
				    !(g.leaders[size_t(who)].flags2 & leaderflag2::kNoCityDefeat))
					leader_defeat(g, who, 0, -1, 0);
			}
			if (u.ptype->unit_flags2 & 8) g.leaders[size_t(who)].caras--;
			for (int32_t n = u.o_down, guard = 0; n >= 0 && guard < kUnitLimit; guard++) {
				Unit* q = g.unit(who, n);
				if (!q) break;
				n = q->o_down;
				q->o_down = -1;
				q->o_up = -1;
			}
			u.o_down = -1;
			u.o_up = -1;
			// Scripted rules / CTW: the scenario tally ScenarioData::units_killed[who][type] (unit_type_killed).
			if ((g.scripted_rules || g.ctw) && !(u.unit_masks & umask::kDecoy) && who < 8 && u.type >= kUnits &&
			    u.type < kUnits + 352)
				g.scen.units_killed[size_t(who)][size_t(u.type - kUnits)]++;
		}
		// Heroes::close_hero / Specials::close_special (close_supply and close_caravan are done elsewhere here).
		if (unit_is_hero(u) && u.cara >= 0 && who < 8) {
			heroes_close_hero(g, who, u.cara);
			u.cara = -1;
		} else if (unit_is_special(u) && u.hero >= 0 && who < 8 && u.type > 53) {
			specials_close_special(g, who, u.hero);
			u.hero = -1;
		}
		carrier_closed(g, u);  // a carrier re-anchors the patrols based on it
	}
	if (death != 0) {
		if (!blocked_merchant && is_live_unit(u) && u.on_map() && !(u.unit_masks & umask::kDecoy)) {
			const int32_t r1 = g.rng.get(0, 0xffff) & 1;  // @60fae6
			int32_t anim = death * 2 + 13 + r1;
			int32_t alt = int32_t(deg);
			if (death == kDeathScorch) {
				anim = (g.rng.get(0, 0xffff) & 1) + 0x11;  // @60fb24
				alt = 14 - (g.rng.get(0, 0xffff) & 3);     // @60fb42
			}
			auto one = [&](int32_t i) {
				int32_t a = anim;
				if (death == kDeathScorch && g.anims && g.anims->has_scorch(u.guys[size_t(i)].gpiece)) a = alt;
				kill_guy(g, who, u.o, i, int8_t(a), gpiece, deg);
			};
			for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) one(i);
			for (int32_t i = u.ptype->squad_size; size_t(i) < u.guys.size(); i++) one(i);
		} else {
			death = 0;  // (death sound only)
		}
	}
	u.path.clear();
	u.path_size = 0;  // @60ee50 (LAB_0060f9e0): the path stack's storage is freed and its size / length set to 0, so Unit::init's Stack<PathData>::init(10) runs again for the slot
	close_orders(g, u, (u.flags & objflag::kValid) != 0);
	// Caravans::close_caravan (the original closes it in the block above, before close_orders; here after, so the
	// TRADE_ROUTE order's end_trade_route still finds the record's cities).
	if (u.cara >= 0 && u.who < 8) {
		caravans_close(g, u.who, u.cara);
		u.cara = -1;
	}
	clear_partial_path(u);
	object_close(g, u, death);
	u.unit_masks &= ~umask::kDecoy;
}

// ---------------------------------------------------------------- corpses

void kill_guy(Game& g, int32_t who, int32_t o, int32_t guy, int32_t anim, int32_t gpiece, float deg) {
	// @659410
	Unit* u = g.unit(who, o);
	if (!u || size_t(guy) >= u->guys.size()) return;
	const Guy& gy = u->guys[size_t(guy)];
	const Type& gt = g.type(gy.type);
	if (gt.cat == 8) {
		// Aircraft guys crash as an Ammo (Ammo::init_crash @67b800: one game_random draw, a local Random for the
		// drift; aircraft_dead.cpp), missiles die without debris.
		if (!has_objmask(*u, objmask::kMissile)) add_crash_ammo(g, *u, guy);
		return;
	}
	if (anim >= 11 && anim <= 20) add_death(g, who, o, guy, anim, gpiece, deg);
	if (gt.domain != kDomainAir && !blocks_while_dead(*u)) {
		const int32_t r = u->ptype->new_block_radius;
		if (r != 0) {
			const int32_t ux = ucoord_of(gy.x), uy = ucoord_of(gy.y);
			int32_t region = -1;
			if (g.world.in_u(ux, uy)) region = g.world.w(ux >> 4, uy >> 4).region;
			g.world.stamp_footprint(ux, uy, r, region, false);
		}
	}
	// (render: death sound, guy removal list)
}

int32_t add_death(Game& g, int32_t who, int32_t o, int32_t guy, int32_t anim, int32_t gpiece, float deg) {
	// @653b60: the first free slot, else the oldest corpse of a type that does not block while dead (slot 0 if
	// every corpse blocks).
	int32_t best_frame = g.frame + 1, best = 0;
	size_t i = 0;
	for (; i < g.deaths.size(); i++) {
		const DeathObj& d = g.deaths[i];
		if (d.valid == 0) break;
		if (!death_blocks(g, d) && d.first_frame < best_frame) {
			best_frame = d.first_frame;
			best = int32_t(i);
		}
	}
	if (i == g.deaths.size()) i = size_t(best);
	int32_t ammo_piece = -1;
	if (gpiece >= 0 && g.anims && gpiece < g.anims->num_ammo() && (g.anims->ammo_flags(gpiece) & ammoflag::kPuncture))
		ammo_piece = gpiece;
	DeathObj& d = g.deaths[i];
	if (d.valid && death_blocks(g, d)) clear_blocking(g, d);
	// DeathObj::init @8d4c60
	const Unit& u = *g.unit(who, o);
	const Guy& gy = u.guys[size_t(guy)];
	d = DeathObj();
	d.who = who;
	d.o = o;
	d.gpiece = gy.gpiece;  // UnitData::get_gpiece(guy, packed) = the guy's piece
	d.ammo_piece = ammo_piece;
	d.deg = deg;
	d.x = gy.x;
	d.y = gy.y;
	d.z = u.ptype->domain == kDomainSea ? 0 : find_data_z(g, d.x, d.y, 0);
	d.angle = gy.angle;
	d.turret_angles = gy.turret_angles;  // DeathObj::init @8d4c60 copies the guy's four pivot angles ...
	d.node_flags = -1;                   // ... and sets the node flags (+0x48) to 0xffff
	d.anim = anim;
	d.valid = 1;
	d.cur_frame = 0;
	d.guy = guy < u.ptype->squad_size ? int8_t(-1) : int8_t(guy - u.ptype->squad_size);
	d.skeleton = g.anims ? g.anims->skeleton_gpiece(d.gpiece) : -1;  // DeathObj +0x44 = GraphicPieces::get_skeleton_gpiece (a gpiece number, -1 none)
	d.first_frame = g.frame;
	if (!g.anims || !g.anims->piece_exists(d.gpiece) || !g.anims->has_packet(d.gpiece)) {
		// No model, or "No Animation Packet Defined": no corpse.
		d.valid = 0;
		if (death_blocks(g, d)) clear_blocking(g, d);
	}
	return int32_t(i);
}

void deaths_inc_time(Game& g) {
	for (DeathObj& d : g.deaths) {
		if (d.valid == 0) continue;
		// DeathObj::inc_time @8d5240
		Unit* u = g.unit(d.who, d.o);
		if (!u) {
			d.valid = 0;
			continue;
		}
		u->hold_frames = uint16_t(u->hold_frames + 1);  // the corpse holds the dead unit's slot
		if (!g.anims || !g.anims->piece_exists(d.gpiece)) {
			d.valid = 0;
			if (blocks_while_dead(*u)) clear_blocking(g, d);
			continue;
		}
		int32_t frames = 0;
		if (!g.anims->has_packet(d.gpiece)) d.valid = 0;
		else frames = g.anims->frames(d.gpiece, d.anim);
		d.cur_frame++;
		int32_t L;
		if (d.skeleton == -1) L = u->ptype->domain == kDomainSea ? frames : frames + 0x87;
		else L = frames + 0x273;
		if (d.cur_frame >= L) d.valid = 0;
		if (blocks_while_dead(*u)) {
			if (d.valid == 0) clear_blocking(g, d);
			else if (u->hold_frames < 30) u->hold_frames = 30;
		}
	}
}

}  // namespace ron::sim
