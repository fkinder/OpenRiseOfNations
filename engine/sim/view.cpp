#include "sim/view.hpp"

#include <algorithm>

#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/game.hpp"
#include "sim/orders.hpp"
#include "sim/production.hpp"
#include "sim/transport.hpp"

namespace ron::sim {

namespace {

AnimState anim_state(int32_t anim) {
	switch (anim) {
	case kCharDefault: case kCharIdle1: case kCharIdle2: case kCharIdle3: case 4: case 5: case 6:
		return AnimState::Idle;
	case kCharSlog: case kCharWalk: case kCharJog: case kCharAttackWalk:
		return AnimState::Walk;
	case kCharTurnLeft: case kCharTurnRight:
		return AnimState::Turn;
	default:
		return AnimState::Other;
	}
}

void fill(const Game& g, const Unit& u, UnitView& v, bool with_guys) {
	v.who = u.who;
	v.o = u.o;
	v.uid = u.uid;
	v.type = u.type;
	v.color = g.leaders[size_t(u.who)].color;
	v.x = u.x;
	v.y = u.y;
	v.z = u.z;
	v.angle = u.guys.empty() ? u.angle : u.guys[0].angle;
	v.anim = u.guys.empty() ? AnimState::Idle : anim_state(u.guys[0].cur_anim);
	v.alive = u.alive();
	v.on_map = u.on_map();
	v.packed = (u.unit_masks & umask::kPacked) != 0;
	v.group = u.group;
	v.order = u.order_type();
	v.dest_x = u.orders.empty() ? -1 : u.orders_x;
	v.dest_y = u.orders.empty() ? -1 : u.orders_y;
	{
		const int32_t uber = u.ptype && u.ptype->uber_size > 1 ? u.ptype->uber_size : 1;
		v.max_hit_points = u.myhits / uber;
		v.hit_points = std::max(0, v.max_hit_points - u.damage);
		const Order* f = u.front();
		if (f && (f->type == kAttack || f->type == kGroupAttack)) {
			v.target_who = f->whom;
			v.target_o = f->ox;
		}
		v.attacking = (u.flags & objflag::kAttacking) != 0;
		v.recharging = u.recharging;
	}
	if (const Order* a = get_action(u); a && a->type == kGather) {
		v.gather_who = a->whom;
		v.gather_o = a->ox;
		v.gather_arrived = a->been_there != 0;
	}
	v.carrying = (u.unit_masks & umask_econ::kHasWood) ? 1 : (u.unit_masks & umask_econ::kHasMetal) ? 4 : -1;
	v.collecting = (u.rare >= 6 && u.rare < 50 && unit_is_rare_collector(g, u)) ? u.rare : -1;
	v.cloaked = u.ptype && unit_is_cloaked(u);
	v.seen_by = v.detected_by = 0;
	if (u.alive()) {
		for (int32_t p = 0; p < 8; p++) {
			if (object_is_seen(g, u, p, true)) v.seen_by = uint8_t(v.seen_by | (1u << p));
			if (unit_is_detected(g, u, p)) v.detected_by = uint8_t(v.detected_by | (1u << p));
		}
	}
	v.guys.clear();
	if (with_guys) {
		const int32_t squad = u.ptype ? u.ptype->squad_size : 0;
		for (size_t i = 0; i < u.guys.size(); i++) {
			if (int32_t(i) >= u.guy_mark && int32_t(i) < squad) continue;  // dead squad figures
			const Guy& gy = u.guys[i];
			v.guys.push_back({gy.x, gy.y, gy.z, gy.angle, gy.cur_anim, gy.gpiece, gy.cur_time, gy.end_time});
		}
	}
}

}  // namespace

bool unit_view(const Game& g, int32_t who, int32_t o, UnitView& out, bool with_guys) {
	const Unit* u = g.unit(who, o);
	if (!u) return false;
	fill(g, *u, out, with_guys);
	return true;
}

GameView make_view(const Game& g, bool with_guys) {
	GameView v;
	v.frame = g.frame;
	v.tick = g.tick;
	v.rng_seed = g.rng.seed();
	for (int32_t who = 0; who < kNumLeaders; who++) {
		const ObjectList& L = g.objects[size_t(who)];
		for (int32_t o = 0; o < L.unit_mark && size_t(o) < L.units.size(); o++) {
			const Unit* u = L.units[size_t(o)].get();
			if (!u || !u->alive()) continue;
			UnitView uv;
			fill(g, *u, uv, with_guys);
			v.units.push_back(std::move(uv));
		}
		for (const auto& b : L.builds) {
			if (!b || !b->alive()) continue;
			BuildView bv;
			bv.who = b->who;
			bv.o = b->o;
			bv.type = b->type;
			bv.color = g.leaders[size_t(who)].color;
			bv.x = b->x;
			bv.y = b->y;
			bv.z = b->z;
			bv.alive = true;
			bv.active = (b->flags & objflag::kActive) != 0;
			bv.started = (b->flags & objflag::kStarted) != 0;
			bv.city = (b->flags & objflag::kCity) != 0;
			bv.city_index = b->city;
			bv.max_hit_points = b->myhits;
			bv.hit_points = std::max(0, obj_hits(g, *b) - b->damage);
			bv.job_counter = b->job_counter;
			bv.construct_time = construct_time(g, *b, 0);
			for (int32_t i = 0; i < b->queued && i < b->build_queue.queue_size; i++) {
				const QueueItem& it = b->build_queue.queue[size_t(i)];
				BuildView::QueueEntry e;
				e.type = it.type;
				e.job_counter = it.job_counter;
				e.time = it.type >= 0 ? int32_t(train_time(g, b->o, b->who, it.type)) : 0;
				bv.queue.push_back(e);
			}
			bv.repeat = (b->build_masks & bmask::kInfinite) != 0;
			for (const GatherPoint& gp : b->gather) bv.rally.push_back({gp.x, gp.y, gp.action});
			bv.rally_inside = build_gather_inside(*b);
			if (is_gather_type(*b->ptype)) {
				bv.gatherers = build_num_gatherers(g, *b, false, false);
				bv.gatherers_working = build_num_gatherers(g, *b, true, true);
				bv.gatherers_max = b->gather_max;
			}
			bv.target_who = b->attack_whom;
			bv.target_o = b->attack_ox;
			bv.attacking = (b->flags & objflag::kAttacking) != 0;
			bv.under_attack = (b->build_masks & bmask::kUnderAttack) != 0;
			bv.ever_seen = b->ever_seen;
			bv.ever_seen_completed = b->ever_seen_completed;
			for (int32_t p = 0; p < 8; p++) {
				if (object_is_seen(g, *b, p, false)) bv.seen_by = uint8_t(bv.seen_by | (1u << p));
				if (object_is_now_seen(g, *b, p)) bv.now_seen_by = uint8_t(bv.now_seen_by | (1u << p));
			}
			v.builds.push_back(bv);
		}
	}
	for (const DeathObj& d : g.deaths) {
		if (!d.valid) continue;
		CorpseView c;
		c.who = d.who;
		c.o = d.o;
		const Unit* u = g.unit(d.who, d.o);
		c.type = u ? u->type : -1;
		c.x = d.x;
		c.y = d.y;
		c.z = d.z;
		c.angle = d.angle;
		c.anim = d.anim;
		c.gpiece = d.gpiece;
		c.frame = d.cur_frame;
		c.anim_frames = g.anims ? g.anims->frames(d.gpiece, d.anim) : 3;
		v.corpses.push_back(c);
	}
	for (const Ammo& a : g.ammo) {
		if (!a.in_use()) continue;
		ProjectileView p;
		p.ammo = a.gpiece;
		p.sx = a.sx, p.sy = a.sy, p.sz = a.sz, p.ex = a.ex, p.ey = a.ey, p.ez = a.ez;
		p.angle = a.angle;
		p.time = int32_t(a.cur_time);
		p.total = int32_t(a.total_time);
		p.stuck = !(a.flags & ammobits::kFlying);
		p.who = a.who;
		p.o = a.o;
		if (p.stuck || a.total_time == 0) {
			p.x = a.ex, p.y = a.ey, p.z = a.ez;
		} else if (a.has_path && a.cur_time < a.spline.verts.size()) {
			// A missile ammo flies its B-spline, one point per frame (Ammo::inc_time).
			const Vec3f& v = a.spline.verts[a.cur_time];
			p.x = int32_t(v.x), p.y = int32_t(v.y), p.z = int32_t(v.z);
		} else {
			// The ballistic arc of Ammo::inc_time (presentation only; the sim tracks just the endpoints).
			const float t = float(a.cur_time), k = t / float(a.total_time);
			p.x = int32_t(float(a.ex - a.sx) * k + float(a.sx));
			p.y = int32_t(float(a.ey - a.sy) * k + float(a.sy));
			p.z = int32_t((a.v1z * t + float(a.sz)) + ((-10.4875f * 0.5f) * t) * t);
		}
		v.projectiles.push_back(p);
	}
	return v;
}

int32_t fog_cell(int32_t coord) { return fcoord_of(coord); }

uint8_t fog_level(const Game& g, int32_t who, int32_t fx, int32_t fy) {
	const World& w = g.world;
	if (fx < 0 || fy < 0 || fx >= w.fog_xs || fy >= w.fog_ys) return 0;
	if (is_seen(g, fx, fy, who)) return 3;
	if (was_really_seen(g, fx, fy, who)) return 2;
	return was_seen(g, fx, fy, who) ? 1 : 0;
}

void make_fog_view(const Game& g, int32_t who, FogView& out) {
	const World& w = g.world;
	out.who = who;
	out.fog_xs = w.fog_xs;
	out.fog_ys = w.fog_ys;
	out.changes = w.fog_changes;
	out.level.resize(size_t(w.fog_xs) * size_t(w.fog_ys));
	for (int32_t fy = 0; fy < w.fog_ys; fy++) {
		for (int32_t fx = 0; fx < w.fog_xs; fx++) out.level[size_t(fy) * size_t(w.fog_xs) + size_t(fx)] = fog_level(g, who, fx, fy);
	}
}

FogView make_fog_view(const Game& g, int32_t who) {
	FogView v;
	make_fog_view(g, who, v);
	return v;
}

bool unit_seen_by(const Game& g, int32_t viewer, int32_t who, int32_t o) {
	const Unit* u = g.unit(who, o);
	return u && u->alive() && object_is_seen(g, *u, viewer, true);
}

bool build_seen_by(const Game& g, int32_t viewer, int32_t who, int32_t o, bool* now_seen) {
	const Object* b = g.object(who, o);
	const bool ok = b && !b->is_unit && b->alive();
	if (now_seen) *now_seen = ok && object_is_now_seen(g, *b, viewer);
	return ok && object_is_seen(g, *b, viewer, false);
}

LeaderView leader_view(const Game& g, int32_t who) {
	LeaderView v;
	v.who = who;
	if (who < 0 || who >= kNumLeaders) return v;
	const Leader& l = g.leaders[size_t(who)];
	v.valid = (l.flags & leaderflag::kValid) != 0;
	v.human = (l.flags & leaderflag::kHuman) != 0;
	v.tribe = l.tribe;
	v.age = l.age;
	for (int32_t k = 0; k < 6; k++) v.stockpile[k] = l.bucket[size_t(k)];
	v.pop = l.pop;
	v.pop_cap = l.pop_cap;
	v.control = l.control;
	if (who < 8) {
		v.cities = get_total_cities(g, who);
		v.city_limit = get_city_limit(g, who);
	}
	v.units_built = l.units_built;
	v.buildings_built = l.buildings_built;
	if (who < 8) {
		const LeaderEconView e = leader_econ_view(g, who);
		for (int32_t k = 0; k < 6; k++) {
			v.gather_rate[k] = e.rate[size_t(k)];
			v.income[k] = e.income[size_t(k)];
			v.cap[k] = e.cap[size_t(k)];
			v.over_cap[k] = e.over_cap[size_t(k)];
			v.buy[k] = e.buy[size_t(k)];
			v.sell[k] = e.sell[size_t(k)];
		}
		v.can_trade = e.can_trade;
		// Infrastructure: citizens by what they gather at, caravans for wealth; limits from the leader's gather slots.
		const ObjectList& ol = g.objects[size_t(who)];
		for (int32_t o = 0; o < ol.unit_mark && size_t(o) < ol.units.size(); o++) {
			const Unit* u = ol.units[size_t(o)].get();
			if (!u || !u->alive()) continue;
			if (unit_is_caravan(*u)) {
				v.infra_workers[2]++;
				continue;
			}
			if (u->type < 50 || u->type > 53) continue;
			const Order* act = get_action(*u);
			if (!act || act->type != kGather || act->ox < 0 || act->whom != who) continue;
			const Build* gb = g.build(act->whom, act->ox);
			if (!gb || !gb->alive()) continue;
			int32_t good = -1;
			switch (gb->type) {
				case 417: good = 0; break;
				case 418: good = 1; break;
				case 419: good = 4; break;
				case 420: good = 3; break;
				case 421: case 422: good = 5; break;
				default: break;
			}
			if (good >= 0) v.infra_workers[good]++;
		}
		for (int32_t k = 0; k < 6; k++) v.infra_limit[k] = k == 2 ? get_caravan_limit(g, who, 1) : l.gather_slots[size_t(k)];
		for (int32_t k = 0; k < 44; k++) {
			const int32_t bit = k;  // TypeIndex 6 + k
			const bool owned = ((l.rare[size_t(bit >> 3)] | l.rare_conquest[size_t(bit >> 3)]) >> (bit & 7)) & 1;
			if (owned) v.rares.push_back({6 + k, l.rares_collected[size_t(k)]});
		}
	}
	v.armageddon = g.armageddon;
	return v;
}

}  // namespace ron::sim
