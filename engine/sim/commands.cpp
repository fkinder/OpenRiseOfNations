#include "sim/commands.hpp"

#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/production.hpp"

namespace ron::sim {

// ---------------------------------------------------------------- executor

void CommandPackage::process(Game& g, const Command& c, int32_t play, int32_t play_who) {
	switch (c.verb) {
	case Verb::Group: {
		// @94a0c0: a GROUP for another leader is ignored and the current group is left unchanged.
		if (c.who != play_who) return;
		const int32_t who = c.who;
		Group tmp;
		group_clear(tmp, -1);
		tmp.id = -1;
		bool added = false;
		auto add_one = [&](int32_t o) {
			Object* ob = g.object(who, o);
			if (!ob || !ob->alive()) return false;
			if (ob->is_unit && o >= g.objects[size_t(who)].unit_mark) return false;
			group_add(g, tmp, o, who, 0, 0);
			added = true;
			if (ob->is_unit) {
				// unit.play = sender seat, down the o_down chain too
				Unit* u = static_cast<Unit*>(ob);
				for (int guard = 0; u && guard < kUnitLimit; guard++) {
					u->play = int8_t(play);
					u = u->o_down >= 0 ? g.unit(who, u->o_down) : nullptr;
				}
			}
			return true;
		};
		Last& L = last[size_t(play & 7)];
		if (!c.objects.empty()) {
			L.objects.clear();
			L.uids.clear();
			for (uint16_t o : c.objects) {
				if (add_one(o)) {
					L.objects.push_back(o);
					L.uids.push_back(g.object(who, o)->uid);
				}
			}
		} else {
			for (size_t k = 0; k < L.objects.size(); k++) {
				const Object* ob = g.object(who, L.objects[k]);
				if (ob && ob->alive() && ob->uid == L.uids[k]) add_one(L.objects[k]);
			}
		}
		group = added ? groups_push_group(g, who, tmp, 1) : -1;
		return;
	}
	case Verb::Stance:
		if (group >= 0) group_action_stance(g, g.groups.g[size_t(group)], c.stance);
		return;
	case Verb::Form:
		if (group >= 0) group_action_form(g, g.groups.g[size_t(group)], c.form32, c.rotate, QueuePos(c.queued));
		return;
	case Verb::MoveTo:
		// process_move_to @9497c0 -> action_move_to(x, y, q, set_angle, angle, orders, 1, form, width, disembark)
		if (group >= 0)
			group_action_move_to(g, g.groups.g[size_t(group)], c.x, c.y, QueuePos(c.queued), c.set_angle != 0, c.angle,
			                     c.orders, 1, c.form, c.width, c.disembark);
		return;
	case Verb::Halt:
		if (group >= 0) group_action_halt(g, g.groups.g[size_t(group)], 0);
		return;
	case Verb::Attack:
		// process_attack -> Group::action_attack(ox, whom, mandatory 1, queued, ignore) (combat-spec 3.3.1)
		if (group >= 0) group_action_attack(g, g.groups.g[size_t(group)], c.ox, c.whom, 1, QueuePos(c.queued), c.ignore);
		return;
	case Verb::SwarmAround: {
		// process_swarm_around @949970: if whom < 0, ox < 0 or the target is alive.
		if (group < 0) return;
		const Object* t = (c.whom >= 0 && c.ox >= 0) ? g.object(c.whom, c.ox) : nullptr;
		if (c.whom < 0 || c.ox < 0 || (t && t->alive()))
			group_action_swarm_around(g, g.groups.g[size_t(group)], c.ox, c.whom, QueuePos(c.queued), c.order_kind, true);
		return;
	}
	case Verb::Disband:
		if (group >= 0) group_action_disband(g, g.groups.g[size_t(group)], c.all);  // @948660
		return;
	case Verb::GatherPoint:
		if (group >= 0) group_action_gather_point(g, g.groups.g[size_t(group)], c.x, c.y, c.action, c.add_to_end);  // @948510
		return;
	case Verb::QueueUp:
		if (group >= 0) group_action_queue_up(g, g.groups.g[size_t(group)], c.type, c.num);  // @948230
		return;
	case Verb::Build:
		if (group >= 0) group_action_build(g, g.groups.g[size_t(group)], c.x, c.y, c.type, QueuePos(c.queued));  // @948110
		return;
	case Verb::BuildMask:
		if (group >= 0) group_action_buildmask(g, g.groups.g[size_t(group)], c.mask, c.set);  // @947680
		return;
	case Verb::Gather: {
		// process_gather @9488b0: ox < 0, or objects[group owner][ox] valid -> Group::action_gather(ox, queued).
		if (group < 0) return;
		Group& gr = g.groups.g[size_t(group)];
		const Object* t = c.ox >= 0 ? g.object(gr.who, c.ox) : nullptr;
		if (c.ox < 0 || (t && t->alive())) group_action_gather(g, gr, c.ox, QueuePos(c.queued));
		return;
	}
	case Verb::Garrison: {
		// process_garrison: ox < 0, whom < 0 or the target is alive -> Group::action_garrison(ox, whom, queued, 0).
		if (group < 0) return;
		const Object* t = (c.ox >= 0 && c.whom >= 0) ? g.object(c.whom, c.ox) : nullptr;
		if (c.ox < 0 || c.whom < 0 || (t && t->alive()))
			group_action_garrison(g, g.groups.g[size_t(group)], c.ox, c.whom, QueuePos(c.queued), 0);
		return;
	}
	case Verb::EjectAll:
		if (group >= 0)
			group_action_eject_all(g, g.groups.g[size_t(group)], c.back_to_work, c.who, c.eject_o, c.eject_who);  // @710b40
		return;
	case Verb::CityGather:
		if (group >= 0) group_action_city_gather(g, g.groups.g[size_t(group)], c.type, QueuePos(c.queued));  // @948a10
		return;
	case Verb::Trade: {
		// process_trade @948b20: both endpoints negative or alive.
		if (group < 0) return;
		const Object* a = (c.ox >= 0 && c.whom >= 0) ? g.object(c.whom, c.ox) : nullptr;
		const Object* b = (c.x2 >= 0 && c.y2 >= 0) ? g.object(c.y2, c.x2) : nullptr;
		if ((c.ox < 0 || (a && a->alive())) && (c.x2 < 0 || (b && b->alive())))
			group_action_trade(g, g.groups.g[size_t(group)], c.ox, c.whom, c.x2, c.y2, QueuePos(c.queued));
		return;
	}
	case Verb::Buy:
		action_buy(g, c.who, c.good, c.flags);  // process_buy @946a20
		return;
	case Verb::Sell:
		action_sell(g, c.who, c.good, c.flags);  // process_sell @9468a0
		return;
	case Verb::Tribute:
		action_offer(g, c.who, c.whom, c.good, c.amount);  // process_tribute @946d70
		return;
	case Verb::DemandTribute:
		action_offer(g, c.who, c.whom, c.good, -c.amount);  // process_demand_tribute @946c50
		return;
	case Verb::ClearTributes:
		action_clear_tributes(g, c.who, c.whom);  // @9471b0
		return;
	case Verb::Accept:
		action_respond(g, c.who, c.whom, 1);  // process_accept @946fb0
		return;
	case Verb::Reject:
		// process_reject @946e90: withdraw an agreement (0), else reject (2).
		if (c.who >= 0 && c.who < 8 && c.whom >= 0 && c.whom < 8)
			action_respond(g, c.who, c.whom, g.leaders[size_t(c.who)].dip[size_t(c.whom)].agree == 1 ? 0 : 2);
		return;
	case Verb::LeaderOptions:
		// TODO(spec): process_leader_options also applies stances (Unit::set_stance); only the options are kept.
		if (c.who >= 0 && c.who < kNumLeaders) {
			Leader& l = g.leaders[size_t(c.who)];
			l.opt_peasants = c.peasants;
			l.opt_peasants_wait = c.peasants_wait;
			l.opt_buildings = c.buildings;
			l.options_flags = c.option_flags;
		}
		return;
	case Verb::Unqueue: {
		// process_unqueue @9466f0: object (who, o) alive with the matching uid; buildings -> Build::action_unqueue.
		// TODO(air): units -> Unit::action_unqueue (carrier aircraft).
		Object* ob = g.object(c.who, c.ox);
		if (!ob || !ob->alive() || ob->uid != c.uid || ob->is_unit) return;
		build_action_unqueue(g, static_cast<Build&>(*ob), c.num);
		return;
	}
	}
}

// ---------------------------------------------------------------- issuer

bool CommandManager::issue_group(const Game& g, const std::vector<int32_t>& objs) {
	const int32_t who = play_who;
	std::vector<uint16_t> keep, uids;
	for (int32_t o : objs) {
		const Object* ob = g.object(who, o);
		if (!ob || !ob->alive()) continue;
		if (ob->is_unit) {
			if (o >= g.objects[size_t(who)].unit_mark) continue;
			if (static_cast<const Unit*>(ob)->o_up >= 0) continue;  // captains only
		}
		keep.push_back(uint16_t(o));
		uids.push_back(ob->uid);
	}
	if (keep.empty()) return false;
	Command c;
	c.verb = Verb::Group;
	c.who = who;
	if (!(who == last_sent_who_ && keep == last_sent_ && uids == last_sent_uids_)) c.objects = keep;  // else num = 0
	last_sent_who_ = who;
	last_sent_ = keep;
	last_sent_uids_ = uids;
	local_.push_back(std::move(c));
	return true;
}

bool CommandManager::issue_move_to(const Game& g, const std::vector<int32_t>& objs, int32_t x, int32_t y, QueuePos q,
                                   int32_t form, bool set_angle, uint32_t angle, int32_t orders, int32_t width) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::MoveTo;
	c.x = x;
	c.y = y;
	c.set_angle = set_angle ? 1 : 0;
	c.angle = angle;
	c.orders = int8_t(orders);
	c.queued = int8_t(q);
	c.form = int8_t(form);
	c.width = int8_t(width);
	c.disembark = 0;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_halt(const Game& g, const std::vector<int32_t>& objs) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::Halt;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_attack(const Game& g, const std::vector<int32_t>& objs, int32_t ox, int32_t whom,
                                  QueuePos q, int32_t ignore) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::Attack;
	c.ox = ox;
	c.whom = whom;
	c.ignore = ignore;
	c.queued = int8_t(q);
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_form(const Game& g, const std::vector<int32_t>& objs, int32_t form, int32_t rotate,
                                QueuePos q) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::Form;
	c.form32 = form;
	c.rotate = rotate;
	c.queued = int8_t(q);
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_stance(const Game& g, const std::vector<int32_t>& objs, int32_t stance) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::Stance;
	c.stance = stance;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_build(const Game& g, const std::vector<int32_t>& objs, int32_t type, int32_t x, int32_t y,
                                 QueuePos q) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::Build;
	c.x = x;
	c.y = y;
	c.x2 = x;
	c.y2 = y;
	c.type = type;
	c.queued = int8_t(q);
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_swarm_around(const Game& g, const std::vector<int32_t>& objs, int32_t ox, int32_t whom,
                                        int32_t order, QueuePos q) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::SwarmAround;
	c.ox = ox;
	c.whom = whom;
	c.order_kind = order;
	c.queued = int8_t(q);
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_queue(const Game& g, const std::vector<int32_t>& buildings, int32_t type, int32_t num) {
	if (!issue_group(g, buildings)) return false;
	Command c;
	c.verb = Verb::QueueUp;
	c.type = type;
	c.num = num;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_gather_point(const Game& g, const std::vector<int32_t>& buildings, int32_t x, int32_t y,
                                        int32_t action, bool add_to_end) {
	if (!issue_group(g, buildings)) return false;
	Command c;
	c.verb = Verb::GatherPoint;
	c.x = x;
	c.y = y;
	c.action = action;
	c.add_to_end = add_to_end ? 1 : 0;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_disband(const Game& g, const std::vector<int32_t>& objs, bool all) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::Disband;
	c.all = all ? 1 : 0;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_buildmask(const Game& g, const std::vector<int32_t>& buildings, int32_t mask, bool set) {
	if (!issue_group(g, buildings)) return false;
	Command c;
	c.verb = Verb::BuildMask;
	c.mask = mask;
	c.set = set ? 1 : 0;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_unqueue(const Game& g, int32_t o, int32_t index) {
	const Object* ob = g.object(play_who, o);
	if (!ob || !ob->alive()) return false;
	Command c;
	c.verb = Verb::Unqueue;
	c.who = play_who;
	c.ox = o;
	c.num = index;
	c.uid = ob->uid;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_gather(const Game& g, const std::vector<int32_t>& objs, int32_t ox, QueuePos q) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::Gather;
	c.ox = ox;
	c.queued = int8_t(q);
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_city_gather(const Game& g, const std::vector<int32_t>& cities, int32_t t, QueuePos q) {
	if (!issue_group(g, cities)) return false;
	Command c;
	c.verb = Verb::CityGather;
	c.type = t;
	c.queued = int8_t(q);
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_garrison(const Game& g, const std::vector<int32_t>& objs, int32_t ox, int32_t whom, QueuePos q) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::Garrison;
	c.ox = ox;
	c.whom = whom;
	c.queued = int8_t(q);
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_eject_all(const Game& g, const std::vector<int32_t>& objs, bool back_to_work, int32_t who,
                                     int32_t eject_o, int32_t eject_who) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::EjectAll;
	c.back_to_work = back_to_work ? 1 : 0;
	c.who = who;
	c.eject_o = eject_o;
	c.eject_who = eject_who;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_trade(const Game& g, const std::vector<int32_t>& objs, int32_t ox, int32_t whom, int32_t oxx,
                                 int32_t whose, QueuePos q) {
	if (!issue_group(g, objs)) return false;
	Command c;
	c.verb = Verb::Trade;
	c.ox = ox;
	c.whom = whom;
	c.x2 = oxx;
	c.y2 = whose;
	c.queued = int8_t(q);
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_buy(int32_t good, int32_t flags) {
	Command c;
	c.verb = Verb::Buy;
	c.who = play_who;
	c.good = good;
	c.flags = flags;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_sell(int32_t good, int32_t flags) {
	Command c;
	c.verb = Verb::Sell;
	c.who = play_who;
	c.good = good;
	c.flags = flags;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_tribute(int32_t whom, int32_t good, int32_t amount, bool demand) {
	Command c;
	c.verb = demand ? Verb::DemandTribute : Verb::Tribute;
	c.who = play_who;
	c.whom = whom;
	c.good = good;
	c.amount = amount;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_clear_tributes(int32_t whom) {
	Command c;
	c.verb = Verb::ClearTributes;
	c.who = play_who;
	c.whom = whom;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_respond(int32_t whom, bool accept) {
	Command c;
	c.verb = accept ? Verb::Accept : Verb::Reject;
	c.who = play_who;
	c.whom = whom;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_leader_options(int32_t peasants, int32_t peasants_wait, int32_t buildings, uint32_t flags) {
	Command c;
	c.verb = Verb::LeaderOptions;
	c.who = play_who;
	c.peasants = peasants;
	c.peasants_wait = peasants_wait;
	c.buildings = buildings;
	c.option_flags = flags;
	local_.push_back(c);
	return true;
}

void CommandManager::process_turn(Game& g) {
	for (const Command& c : local_) {
		recorded.push_back({g.frame, c});
		package_.process(g, c, play, play_who);
	}
	local_.clear();
}

}  // namespace ron::sim
