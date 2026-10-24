#include "notices.hpp"

#include "uistr.hpp"

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
constexpr int kNoticeGap = 400;  // frames between banners (frame - stamp > 400)

}  // namespace

// String::parse: "$NAMEn" is replaced by argument n (the digits end the placeholder), in the order of the digits.
std::string fill(std::string text, const std::vector<std::string>& args) {
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

std::string Notifier::parse(std::string text, const std::vector<std::string>& args) { return fill(std::move(text), args); }

// loc_str_array element `index` (docs/ui-spec.md 3.5), checked against a piece of its expected text so a different build's
// table falls back to the English text written here.
std::string Notifier::loc(int index, const char* expect, const char* fallback) const {
	if (tables_ && index >= 0 && size_t(index) < tables_->translated().size()) {
		const std::string& s = tables_->translated()[size_t(index)];
		// The tables are positional in every language: the expected English word only guards the English table against a
		// different build's numbering.
		if (uistr::language() != ron::Language::English ? !s.empty() : s.find(expect) != std::string::npos) return s;
	}
	return fallback;
}

void Notifier::reset(const ron::sim::Game& g) {
	events_seen_ = g.events.size();
	have_prev_ = false;
	prev_.clear();
	last_alert_.clear();
}

void Notifier::update(const Env& env, std::vector<Note>& out) {
	if (!env.game || !env.view) return;
	const ron::sim::Game& g = *env.game;
	const int me = env.me;
	auto nation = [&](int who) { return env.leader ? env.leader(who) : env.nation ? env.nation(who) : std::string(); };
	auto type_name = [&](int t) { return env.type_name ? env.type_name(t) : std::string(); };
	auto find_build = [&](int who, int o) -> const BuildView* {
		for (const BuildView& b : env.view->builds) {
			if (b.who == who && b.o == o) return &b;
		}
		return nullptr;
	};
	auto city_name = [&](int who, int city_index, int fallback_type) {
		if (who >= 0 && who < 8 && city_index >= 0 && size_t(city_index) < g.cities[size_t(who)].size()) {
			const std::string& n = g.cities[size_t(who)][size_t(city_index)].name;
			if (!n.empty()) return n;
		}
		return type_name(fallback_type);
	};
	static const char* goods_names[6] = {"Food", "Timber", "Wealth", "Knowledge", "Metal", "Oil"};
	// ---- the simulation's event log
	for (; events_seen_ < g.events.size(); events_seen_++) {
		const ron::sim::GameEvent& e = g.events[events_seen_];
		if (e.kind == 1) {  // city captured: who = new owner, other = old owner, o / o2 = old / new city object
			const BuildView* nb = find_build(e.who, e.o2);
			const std::string name = nb ? city_name(e.who, nb->city_index, nb->type) : std::string("the city");
			Note n;
			n.located = nb != nullptr;
			if (nb) n.x = float(nb->x), n.y = float(nb->y);
			if (e.who == me) {
				int best = -1, amount = 0;
				for (int k = 0; k < 6; k++) {
					if (e.goods[size_t(k)] > amount) amount = e.goods[size_t(k)], best = k;
				}
				n.text = best >= 0 ? loc_or(274, "Plunder", "City of $CITYNAME0 captured. Plunder: +$NUMBER0 $GOODTYPE1",
				                            {name, std::to_string(amount), goods_names[best]})
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
		} else if (e.kind == 8 && e.alert != 0 && e.frame >= 1) {
			// Object::do_damage's warnings (the throttles are the simulation's stamps). who = the victim's owner, other = the
			// attacker; value = the city index (classes 1-5), the territory owner (7, 8) or 0.
			namespace ga = ron::sim::gamealert;
			const std::string by = nation(e.other);
			Note n;
			n.argb = kWhite, n.located = true, n.x = float(e.x), n.y = float(e.y), n.ping = 1;
			auto notice = [&](const char* image) {
				Note b;
				b.kind = Note::Notice, b.image = image, b.text = n.text;
				out.push_back(b);
			};
			const bool mine = e.who == me;
			const bool ally = !mine && e.who >= 0 && me >= 0 && ron::sim::is_ally(g, e.who, me);
			const std::string cname = city_name(e.who, e.value, ron::sim::ptype::kVillage);
			switch (e.alert) {
				case ga::kCityAttacked:
				case ga::kCapitalAttacked: {
					const bool capital = e.alert == ga::kCapitalAttacked;
					if (!mine) break;
					int& last = last_alert_.try_emplace(key(e.who, e.value), -100000).first->second;
					const bool banner = e.frame - last > kNoticeGap;
					last = e.frame;
					n.text = capital ? loc_or(2684, "CAPITAL", "Our CAPITAL is under ATTACK by $SOMEJERK0!", {by})
					                 : loc_or(2686, "under attack", "Our city of $CITYNAME0 is under attack by $FRANCISXRUSHMASTER1!", {cname, by});
					n.sound = capital ? "S_YOUR_CAPITAL_ATTACKED" : "S_CITY_BEING_ATTACKED";
					out.push_back(n);
					if (banner) {
						notice(capital ? "capital_under_attack_notice" : "city_under_attack_notice");
					}
					break;
				}
				case ga::kAllyCityAttacked:
				case ga::kAllyCapitalAttacked: {
					if (!ally) break;
					const bool capital = e.alert == ga::kAllyCapitalAttacked;
					n.text = capital ? loc_or(2685, "Allied", "Allied Capital under attack by $JOHNQWHINER0!", {by})
					                 : loc_or(2687, "Allied", "Allied City of $CITYNAME0 is under attack by $JOHNQWHINER1!", {cname, by});
					n.sound = "S_ALLY_UNDER_ATTACK";
					out.push_back(n);
					break;
				}
				case ga::kCityRaided:
					if (!mine) break;
					n.text = loc_or(2688, "raided", "Our city of $CITYNAME0 is being raided by $FRANCISXRUSHMASTER1!", {cname, by});
					n.sound = "S_RAID_ATTACKED";
					out.push_back(n);
					break;
				case ga::kRaided:
					if (!mine) break;
					n.text = loc_or(2689, "RAIDED", "We are being RAIDED by $THEBURGLER0!", {by});
					n.sound = "S_RAID_ATTACKED";
					out.push_back(n);
					break;
				case ga::kUnitAttacked:
				case ga::kScoutAttacked: {
					if (!mine || ron::sim::is_ally(g, e.other, e.who)) break;
					// A scout is named only outside the viewer's own territory (e.value = the territory owner).
					const bool scout = e.alert == ga::kScoutAttacked && e.value != me;
					n.text = scout ? loc_or(2690, "Scout", "Our Scout is being attacked by $JOESIXPACK0!", {by})
					               : loc_or(2691, "ATTACK", "We are under ATTACK by $FREDTHEFLYINGTRACTOR0!", {by});
					n.sound = scout ? "S_SCOUT_ATTACKED" : "S_BEING_ATTACKED";
					out.push_back(n);
					break;
				}
				default: break;
			}
		} else if (e.kind == 9 && e.frame >= 1 && e.other != 0 && e.who == me) {  // tech researched
			Note n;
			n.text = loc_or(2298, "researched", "$BOZOTHECLOWN0 has researched $TECHNAME1", {nation(e.who), type_name(e.value)});
			out.push_back(n);
		} else if (e.kind == 10 && e.frame >= 1) {  // age advanced: who = the leader, value = the new age count
			if (e.who == me && e.value >= 1 && e.value <= 7) {
				Note b;
				b.kind = Note::Notice, b.image = "age_notice" + std::to_string(e.value), b.text = type_name(543 + e.value);
				out.push_back(b);
			} else if (e.who != me && e.value >= 1 && e.value <= 7) {
				Note n;
				n.text = nation(e.who) + " has entered the " + type_name(543 + e.value) + ".";  // (no stock line)
				n.argb = kWhite;
				out.push_back(n);
			}
		} else if ((e.kind == 11 || e.kind == 12) && e.frame >= 1) {  // wonder started / completed
			if (e.who < 0 || e.who >= 8) continue;
			Note n;
			n.argb = kWhite, n.located = true, n.x = float(e.x), n.y = float(e.y), n.ping = 2;
			const std::string name = type_name(e.value);
			if (e.kind == 11) {
				n.text = loc_or(96, "started a wonder", "$KINGTUT0 has started a wonder: $WONDERNAME1!", {nation(e.who), name});
				out.push_back(n);
			} else {
				n.text = e.who == me ? loc_or(107, "completed", "$WONDERNAME0 completed!", {name})
				                     : loc_or(109, "completed a wonder", "$PRESIDENTCLINTON0 has completed a wonder: $WONDERNAME1!", {nation(e.who), name});
				out.push_back(n);
				Note b;
				b.kind = Note::Notice, b.image = "wonder_notice", b.text = n.text;
				out.push_back(b);
			}
		} else if ((e.kind == 13 || e.kind == 14) && e.frame >= 1 && e.other == me && e.who != me) {  // tribute offered / demanded of us
			int k = e.value;
			const int amount = k >= 0 && k < 6 ? e.goods[size_t(k)] : 0;
			Note n;
			n.argb = kWhite;
			n.text = e.kind == 13 ? nation(e.who) + " is offering you " + std::to_string(amount) + " " + (k >= 0 && k < 6 ? goods_names[k] : "goods") + "."
			                      : nation(e.who) + " demands " + std::to_string(amount) + " " + (k >= 0 && k < 6 ? goods_names[k] : "goods") + " in tribute.";
			out.push_back(n);
		} else if (e.kind == 15 && e.frame >= 1 && e.who >= 0 && e.other >= 0) {  // diplomacy changed: value = the new state, alert = the old
			const std::string na = nation(e.who), nb = nation(e.other);
			Note n;
			n.argb = kWhite;
			if (e.value == 2) n.text = loc_or(574, "alliance", "$LEADERNAME0 has formed an alliance with $LEADERNAME1.", {na, nb});
			else if (e.alert == 2) n.text = loc_or(2338, "broken", "$DUBYA0 and $SADDAM1 have broken their alliance.", {na, nb});
			else if (e.value == 1 && e.alert == 0) n.text = loc_or(573, "peace", "$LEADERNAME0 has signed a peace treaty with $LEADERNAME1.", {na, nb});
			if (!n.text.empty()) out.push_back(n);
		} else if (e.kind == 16 && e.frame >= 1 && e.who == me) {  // resource flash
			Note f;
			f.kind = Note::Flash;
			for (int k = 0; k < 6; k++) {
				if (e.goods[size_t(k)] != 0) f.goods |= 1 << k;
			}
			if (f.goods) out.push_back(f);
		}
	}
	// ---- the view's differences: a building going active (its completion line; wonders have their events)
	std::unordered_map<int64_t, Obj> cur;
	cur.reserve(env.view->builds.size());
	for (const BuildView& b : env.view->builds) {
		if (!b.alive || b.who != me) continue;
		Obj o;
		o.type = b.type, o.active = b.active, o.build = true;
		cur[key(b.who, b.o)] = o;
	}
	if (have_prev_ && g.frame > 1) {
		for (const auto& [k, o] : cur) {
			auto pit = prev_.find(k);
			if (pit == prev_.end() || pit->second.active || !o.active || g.type(o.type).cat == 2) continue;
			std::string name = type_name(o.type);
			std::string lower = name;
			for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
			if (lower.find("wall") != std::string::npos || lower.find("gate") != std::string::npos) continue;
			Note n;
			n.text = loc_or(3734, "completed", "$BUILDINGNAME0 completed.", {name});
			if (const BuildView* b = find_build(int(k / 4096), int(k % 4096))) n.located = true, n.x = float(b->x), n.y = float(b->y);
			out.push_back(n);
		}
	}
	prev_ = std::move(cur);
	have_prev_ = true;
}

}  // namespace notices
