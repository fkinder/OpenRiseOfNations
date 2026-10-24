#include "notices.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "sim/production.hpp"

namespace notices {

using ron::sim::BuildView;
using ron::sim::UnitView;

namespace {

constexpr int64_t key(int who, int o) { return int64_t(who) * 4096 + o; }
constexpr uint32_t kWhite = 0xFFFFFFFF;
constexpr int kAlertGap = 300;   // frames between warnings for the same city (Object::do_damage: frame - city stamp > 300)
constexpr int kNoticeGap = 400;  // frames between banners (frame - stamp > 400)

}  // namespace

// String::parse: "$NAMEn" is replaced by argument n (the digits end the placeholder), in the order of the digits.
std::string Notifier::parse(std::string text, const std::vector<std::string>& args) {
	std::string out;
	for (size_t i = 0; i < text.size();) {
		if (text[i] != '$') {
			out += text[i++];
			continue;
		}
		size_t j = i + 1;
		while (j < text.size() && std::isupper(static_cast<unsigned char>(text[j]))) j++;
		const size_t digits = j;
		while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) j++;
		if (digits == i + 1 || digits == j) {
			out += text[i++];
			continue;
		}
		const size_t n = size_t(std::atoi(text.substr(digits, j - digits).c_str()));
		if (n < args.size()) out += args[n];
		i = j;
	}
	return out;
}

// loc_str_array element `index` (docs/ui-spec.md 3.5), checked against a piece of its expected text so a different build's
// table falls back to the English text written here.
std::string Notifier::loc(int index, const char* expect, const char* fallback) const {
	if (tables_ && index >= 0 && size_t(index) < tables_->translated().size()) {
		const std::string& s = tables_->translated()[size_t(index)];
		if (s.find(expect) != std::string::npos) return s;
	}
	return fallback;
}

void Notifier::reset(const ron::sim::Game& g) {
	events_seen_ = g.events.size();
	have_prev_ = false;
	prev_.clear();
	last_alert_.clear();
	last_unit_alert_ = last_notice_ = -100000;
	prev_age_.clear();
	prev_rel_.clear();
}

void Notifier::update(const Env& env, std::vector<Note>& out) {
	if (!env.game || !env.view) return;
	const ron::sim::Game& g = *env.game;
	const int me = env.me, frame = g.frame;
	auto nation = [&](int who) { return env.leader ? env.leader(who) : env.nation ? env.nation(who) : std::string(); };
	auto type_name = [&](int t) { return env.type_name ? env.type_name(t) : std::string(); };
	auto find_build = [&](int who, int o) -> const BuildView* {
		for (const BuildView& b : env.view->builds) {
			if (b.who == who && b.o == o) return &b;
		}
		return nullptr;
	};
	auto city_name = [&](int who, int o, int type) {
		const BuildView* b = find_build(who, o);
		if (b && b->city_index >= 0 && who >= 0 && who < 8 && size_t(b->city_index) < g.cities[size_t(who)].size()) {
			const std::string& n = g.cities[size_t(who)][size_t(b->city_index)].name;
			if (!n.empty()) return n;
		}
		return type_name(type);
	};
	// ---- the simulation's event log
	for (; events_seen_ < g.events.size(); events_seen_++) {
		const ron::sim::GameEvent& e = g.events[events_seen_];
		if (e.kind == 1) {  // city captured: who = new owner, other = old owner, o / o2 = old / new city object
			const BuildView* nb = find_build(e.who, e.o2);
			const std::string name = nb ? city_name(e.who, e.o2, nb->type) : std::string("the city");
			Note n;
			n.located = nb != nullptr;
			if (nb) n.x = float(nb->x), n.y = float(nb->y);
			if (e.who == me) {
				int best = -1, amount = 0;
				for (int k = 0; k < 6; k++) {
					if (e.goods[size_t(k)] > amount) amount = e.goods[size_t(k)], best = k;
				}
				static const char* goods[6] = {"Food", "Timber", "Wealth", "Knowledge", "Metal", "Oil"};
				n.text = best >= 0 ? loc_or(274, "Plunder", "City of $CITYNAME0 captured. Plunder: +$NUMBER0 $GOODTYPE1",
				                            {name, std::to_string(amount), goods[best]})
				                   : loc_or(275, "captured", "City of $CITYNAME0 captured.", {name});
				n.argb = kWhite, n.ping = 4;
				out.push_back(n);
				Note b;
				b.kind = Note::Notice, b.image = "city_captured_notice", b.text = loc(276, "Captured", "City Captured!");
				out.push_back(b);
			} else if (e.other == me) {
				n.text = loc_or(277, "lost", "City of $CITYNAME0 lost.", {name});
				n.argb = kWhite, n.ping = 3;
				out.push_back(n);
				Note b;
				b.kind = Note::Notice, b.image = "city_lost_notice", b.text = loc(278, "Lost", "City Lost!");
				out.push_back(b);
			}
		} else if (e.kind == 7 && e.who == me) {  // a building of another leader captured
			const BuildView* nb = find_build(e.who, e.o2);
			Note n;
			n.text = loc_or(281, "captured", "$BUILDTYPE0 captured!", {nb ? type_name(nb->type) : std::string("Building")});
			if (nb) n.located = true, n.x = float(nb->x), n.y = float(nb->y);
			out.push_back(n);
		} else if (e.kind == 5) {  // capital lost: who = the loser, other = the holder
			if (e.who == me || (e.who >= 0 && ron::sim::is_ally(g, e.who, me))) {
				Note n;
				n.text = e.who == me ? loc(2308, "CAPITAL", "Your CAPITAL has been captured! You will lose if you do not retake it immediately!")
				                     : loc(2309, "CAPITAL", "Your ALLY'S CAPITAL has been captured! They will lose if they do not retake it immediately!");
				n.argb = kWhite;
				out.push_back(n);
				Note b;
				b.kind = Note::Notice, b.image = "capital_notice", b.text = n.text.substr(0, n.text.find('!') + 1);
				out.push_back(b);
			}
		}
	}
	// ---- the view's differences
	std::unordered_map<int64_t, Obj> cur;
	cur.reserve(env.view->units.size() + env.view->builds.size());
	for (const UnitView& u : env.view->units) {
		if (!u.alive || u.who != me) continue;
		Obj o;
		o.hp = u.hit_points, o.type = u.type;
		cur[key(u.who, u.o)] = o;
	}
	for (const BuildView& b : env.view->builds) {
		if (!b.alive) continue;
		const bool wonder = g.type(b.type).cat == 2;
		if (b.who != me && !(wonder && b.who >= 0 && b.who < 8)) continue;
		Obj o;
		o.hp = b.hit_points, o.type = b.type, o.active = b.active, o.build = true, o.city = b.city, o.started = b.started;
		if (!b.queue.empty()) {
			o.q_type = b.queue.front().type;
			o.q_percent = b.queue.front().time > 0 ? int(int64_t(b.queue.front().job_counter) * 100 / b.queue.front().time) : 0;
		}
		cur[key(b.who, b.o)] = o;
	}
	if (have_prev_) {
		auto attacker = [&](int tw, int to, float px, float py) {
			int who = -1;
			for (const UnitView& u : env.view->units) {
				if (u.alive && u.target_who == tw && u.target_o == to && u.who != tw) return u.who;
			}
			for (const BuildView& b : env.view->builds) {
				if (b.alive && b.target_who == tw && b.target_o == to && b.who != tw) return b.who;
			}
			float best = 2500;
			for (const UnitView& u : env.view->units) {
				if (!u.alive || u.who == tw || u.who >= 8 || !ron::sim::is_enemy(g, tw, u.who)) continue;
				const float d = std::hypot(float(u.x) - px, float(u.y) - py);
				if (d < best) best = d, who = u.who;
			}
			return who;
		};
		for (const auto& [k, o] : cur) {
			auto pit = prev_.find(k);
			if (pit == prev_.end()) {
				// A wonder started by someone: "$LEADER has started a wonder: $NAME!" (only sites appearing now).
				if (o.build && !o.active && o.started && g.type(o.type).cat == 2 && frame > 1) {
					const int who = int(k / 4096);
					Note n;
					n.text = loc_or(96, "started a wonder", "$KINGTUT0 has started a wonder: $WONDERNAME1!", {nation(who), type_name(o.type)});
					n.argb = kWhite;
					const BuildView* b = find_build(who, int(k % 4096));
					if (b) n.located = true, n.x = float(b->x), n.y = float(b->y), n.ping = 2;
					out.push_back(n);
				}
				continue;
			}
			const Obj& p = pit->second;
			const int who = int(k / 4096), oo = int(k % 4096);
			if (o.build && !p.started && o.started && !o.active && g.type(o.type).cat == 2) {  // a wonder begun
				Note n;
				n.text = loc_or(96, "started a wonder", "$KINGTUT0 has started a wonder: $WONDERNAME1!", {nation(who), type_name(o.type)});
				n.argb = kWhite;
				if (const BuildView* b = find_build(who, oo)) n.located = true, n.x = float(b->x), n.y = float(b->y), n.ping = 2;
				out.push_back(n);
			}
			if (o.build && !p.active && o.active) {  // construction finished
				std::string name = type_name(o.type);
				std::string lower = name;
				for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
				const bool wonder = g.type(o.type).cat == 2;
				if (wonder) {
					Note n;
					n.text = who == me ? loc_or(107, "completed", "$WONDERNAME0 completed!", {name})
					                   : loc_or(109, "completed a wonder", "$PRESIDENTCLINTON0 has completed a wonder: $WONDERNAME1!", {nation(who), name});
					n.argb = kWhite;
					if (const BuildView* b = find_build(who, oo)) n.located = true, n.x = float(b->x), n.y = float(b->y), n.ping = 2;
					out.push_back(n);
					Note b;
					b.kind = Note::Notice, b.image = "wonder_notice", b.text = n.text;
					out.push_back(b);
				} else if (who == me && lower.find("wall") == std::string::npos && lower.find("gate") == std::string::npos) {
					Note n;
					n.text = loc_or(3734, "completed", "$BUILDINGNAME0 completed.", {name});
					if (const BuildView* b = find_build(who, oo)) n.located = true, n.x = float(b->x), n.y = float(b->y);
					out.push_back(n);
				}
			}
			if (who != me) continue;
			// A tech leaving the front of a queue at (nearly) full progress: "$LEADER has researched $TECH".
			if (o.build && p.q_type >= 0 && !ron::sim::is_unit_type(p.q_type) && p.q_percent >= 90 && o.q_type != p.q_type) {
				Note n;
				n.text = loc_or(2298, "researched", "$BOZOTHECLOWN0 has researched $TECHNAME1", {nation(who), type_name(p.q_type)});
				out.push_back(n);
			}
			// Attack warnings (Object::do_damage): an own object lost hit points.
			if (o.hp < p.hp) {
				const BuildView* b = o.build ? find_build(who, oo) : nullptr;
				float px = 0, py = 0;
				if (b) px = float(b->x), py = float(b->y);
				else {
					for (const UnitView& u : env.view->units) {
						if (u.who == who && u.o == oo) px = float(u.x), py = float(u.y);
					}
				}
				const int enemy = attacker(who, oo, px, py);
				if (enemy < 0 || frame < 5) continue;  // no attacker in sight: attrition, a script, the start-up settling
				const std::string by = nation(enemy);
				auto notice = [&](const char* image, const std::string& text) {
					if (frame - last_notice_ <= kNoticeGap) return;
					last_notice_ = frame;
					Note n;
					n.kind = Note::Notice, n.image = image, n.text = text;
					out.push_back(n);
				};
				if (o.build && o.city && b) {
					int& last = last_alert_.try_emplace(k, -100000).first->second;
					if (frame - last > kAlertGap) {
						last = frame;
						bool capital = false;
						if (b->city_index >= 0 && size_t(b->city_index) < g.cities[size_t(who)].size())
							capital = (g.cities[size_t(who)][size_t(b->city_index)].city_flags & ron::sim::cityflag::kCapital) != 0;
						Note n;
						n.text = capital ? loc_or(2684, "CAPITAL", "Our CAPITAL is under ATTACK by $SOMEJERK0!", {by})
						                 : loc_or(2686, "under attack", "Our city of $CITYNAME0 is under attack by $FRANCISXRUSHMASTER1!",
						                          {city_name(who, oo, o.type), by});
						n.argb = kWhite, n.located = true, n.x = px, n.y = py, n.ping = 1;
						n.sound = capital ? "S_YOUR_CAPITAL_ATTACKED" : "S_CITY_BEING_ATTACKED";
						out.push_back(n);
						notice(capital ? "capital_under_attack_notice" : "city_under_attack_notice", n.text);
					}
				} else if (!o.build && frame - last_unit_alert_ > kAlertGap) {
					last_unit_alert_ = frame;
					Note n;
					n.text = o.type == ron::sim::ptype::kScout ? loc_or(2690, "Scout", "Our Scout is being attacked by $JOESIXPACK0!", {by})
					                                           : loc_or(2691, "ATTACK", "We are under ATTACK by $FREDTHEFLYINGTRACTOR0!", {by});
					n.argb = kWhite, n.located = true, n.x = px, n.y = py, n.ping = 1;
					out.push_back(n);
				}
			}
		}
	}
	prev_ = std::move(cur);
	have_prev_ = true;
	// ---- ages and diplomacy
	const size_t nl = std::min<size_t>(8, g.leaders.size());
	if (prev_age_.size() != nl) {
		prev_age_.assign(nl, 0);
		for (size_t i = 0; i < nl; i++) prev_age_[i] = g.leaders[i].age;
	}
	for (size_t i = 0; i < nl; i++) {
		const ron::sim::Leader& L = g.leaders[i];
		if (!(L.flags & 1) || L.age <= prev_age_[i]) {
			prev_age_[i] = L.age;
			continue;
		}
		prev_age_[i] = L.age;
		if (int(i) == me && L.age >= 1 && L.age <= 7) {
			Note b;
			b.kind = Note::Notice, b.image = "age_notice" + std::to_string(L.age), b.text = type_name(543 + L.age);
			out.push_back(b);
		}
	}
	std::vector<int> rel(nl * nl, 1);
	for (size_t a = 0; a < nl; a++) {
		for (size_t b = 0; b < nl; b++) {
			if (a == b || !(g.leaders[a].flags & 1) || !(g.leaders[b].flags & 1)) continue;
			rel[a * nl + b] = ron::sim::is_ally(g, int(a), int(b)) ? 2 : ron::sim::is_enemy(g, int(a), int(b)) ? 0 : 1;
		}
	}
	if (prev_rel_.size() == rel.size()) {
		for (size_t a = 0; a < nl; a++) {
			for (size_t b = a + 1; b < nl; b++) {
				const int was = prev_rel_[a * nl + b], now = rel[a * nl + b];
				if (was == now || !(g.leaders[a].flags & 1) || !(g.leaders[b].flags & 1)) continue;
				const std::string na = nation(int(a)), nb = nation(int(b));
				Note n;
				n.argb = kWhite;
				if (now == 2) n.text = loc_or(574, "alliance", "$LEADERNAME0 has formed an alliance with $LEADERNAME1.", {na, nb});
				else if (was == 2) n.text = loc_or(2338, "broken", "$DUBYA0 and $SADDAM1 have broken their alliance.", {na, nb});
				else if (now == 1) n.text = loc_or(573, "peace", "$LEADERNAME0 has signed a peace treaty with $LEADERNAME1.", {na, nb});
				if (!n.text.empty()) out.push_back(n);
			}
		}
	}
	prev_rel_ = std::move(rel);
}

}  // namespace notices
