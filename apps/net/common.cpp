#include "common.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace ronnet {

std::string basename_of(const std::string& p) {
	const size_t s = p.find_last_of('/');
	return s == std::string::npos ? p : p.substr(s + 1);
}

std::string game_dir_default() {
	const char* home = std::getenv("HOME");
	return std::string(home ? home : "") + "/Games/RiseOfNations";
}

std::vector<std::string> list_rcx(const std::string& path) {
	std::vector<std::string> out;
	std::error_code ec;
	if (std::filesystem::is_directory(path, ec)) {
		for (auto it = std::filesystem::recursive_directory_iterator(path, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
			const std::string p = it->path().string();
			if (it->is_regular_file(ec) && p.size() > 4 && p.compare(p.size() - 4, 4, ".rcx") == 0) out.push_back(p);
		}
		std::sort(out.begin(), out.end());
	} else {
		out.push_back(path);
	}
	return out;
}

bool Feed::build(const sim::Replay& r, std::string& err) {
	rp = &r;
	for (auto& v : by_stamp) v.assign(1, nullptr);
	last.fill(0);
	resigned_at.fill(0);
	bool seen[8] = {};
	for (const sim::ReplayPackage& pk : r.packages) {
		const int p = pk.play;
		if (p < 0 || p > 7) {
			err = "package from slot " + std::to_string(p);
			return false;
		}
		// the clear-text drop package of DropControl::process_drops has stamp 0 and is not a player's package
		if (pk.plain.size() <= 4 && !pk.plain.empty() && pk.plain[0] == 0x50 && pk.stamp == 0) continue;
		if (pk.stamp != last[size_t(p)] + 1) {
			err = "slot " + std::to_string(p) + ": stamp " + std::to_string(pk.stamp) + " after " + std::to_string(last[size_t(p)]);
			return false;
		}
		last[size_t(p)] = pk.stamp;
		by_stamp[size_t(p)].push_back(&pk);
		seen[p] = true;
		max_player_bytes = std::max(max_player_bytes, pk.wire(true).size());
		for (const sim::ReplayCommand& c : pk.cmds)
			if ((c.op == sim::op::kResign || c.op == sim::op::kQuit) && resigned_at[size_t(p)] == 0) resigned_at[size_t(p)] = pk.stamp;
	}
	humans.clear();
	for (int p = 0; p < 8; ++p) {
		const bool flagged = r.header.players[size_t(p)].present() && (r.header.players[size_t(p)].flags & 4);
		if (seen[p] || flagged) humans.push_back(p);
		if (flagged && !seen[p]) {
			err = "slot " + std::to_string(p) + " is human in the header but sent no package";
			return false;
		}
		if (seen[p] && !flagged) {
			err = "slot " + std::to_string(p) + " sent packages but is not human in the header";
			return false;
		}
	}
	return !humans.empty() || (err = "no package in the replay", false);
}

uint32_t Feed::max_stamp(int64_t frame_limit) const {
	uint32_t m = 0;
	for (uint32_t s = 1;; ++s) {
		bool ok = true, any = false;
		for (int p : humans) {
			if (resigned_at[size_t(p)] && s > resigned_at[size_t(p)]) continue;
			if (s > last[size_t(p)]) {
				ok = false;
				break;
			}
			any = true;
			if (by_stamp[size_t(p)][s]->frame > frame_limit) ok = false;
		}
		if (!ok || !any) break;
		m = s;
	}
	return m;
}

net::SessionConfig session_config_for(const sim::Replay& rp, int slot, const std::vector<int>& observers) {
	net::SessionConfig c;
	c.local_slot = slot;
	c.seed = rp.header.seed;
	c.speed = rp.header.setup_field(sim::kSetGameSpeed);
	for (int p = 0; p < 8; ++p) c.slot_flags[size_t(p)] = rp.header.players[size_t(p)].present() ? rp.header.players[size_t(p)].flags : 0u;
	for (int o : observers) c.slot_flags[size_t(o)] = net::kPlayerValid | net::kPlayerObserver;
	c.max_pauses = 0;  // the recorded pauses were accepted; the quota is not recorded
	c.checksum_failure_threshold = rp.header.checksum_threshold;
	return c;
}

bool run_instances(std::vector<Inst>& inst, net::LoopbackHub& hub, const Feed& feed, uint32_t M, RunStats& st,
                   const std::function<void(size_t)>& per_boundary, std::string& err) {
	auto feed_one = [&](Inst& in, uint32_t stamp) {
		const sim::ReplayPackage* pk = feed.find(in.slot, stamp);
		if (!pk) return;
		const std::vector<uint8_t> w = pk->wire(true);
		if (!in.session->submit(w)) {
			in.failed = in.done = true;
			in.status = "package " + std::to_string(stamp) + " does not fit";
		}
	};
	for (Inst& in : inst) {
		in.session->on_boundary = [&in](const net::BoundaryInfo& b) { in.bis.push_back(b); };
		in.fed = 1;
		feed_one(in, 1);
		in.session->start();
	}
	size_t checked = 0;
	uint64_t idle = 0;
	auto compare_upto = [&](size_t common) {
		for (; checked < common; ++checked) {
			const net::BoundaryInfo& a = inst[0].bis[checked];
			for (size_t k = 1; k < inst.size(); ++k) {
				const net::BoundaryInfo& b = inst[k].bis[checked];
				if (std::memcmp(a.sums, b.sums, sizeof a.sums) != 0 || a.frame != b.frame) {
					if (st.first_disagree < 0) {
						st.first_disagree = int64_t(checked);
						std::string d = "boundary " + std::to_string(checked + 1) + " (frame " + std::to_string(a.frame) + "), instance 0 against " +
						                std::to_string(k) + ", parts that differ:";
						for (int q = 0; q < 16; ++q)
							if (a.sums[q] != b.sums[q]) d += " " + std::to_string(q);
						if (a.frame != b.frame) d += " (frames " + std::to_string(a.frame) + " / " + std::to_string(b.frame) + ")";
						st.disagree_text = d;
					}
				}
			}
			++st.boundaries_compared;
			if (per_boundary) per_boundary(checked);
		}
	};
	for (;;) {
		bool progressed = false, all_done = true;
		for (Inst& in : inst) {
			if (in.done) continue;
			all_done = false;
			while (in.fed < in.session->local_stamp() + 1) feed_one(in, ++in.fed);
			if (in.done) continue;
			const net::StepResult r = in.session->step();
			switch (r.kind) {
			case net::StepKind::Advanced:
				progressed = true;
				if (r.turn && in.session->boundaries() >= M) in.done = true;
				break;
			case net::StepKind::Stalled: break;
			default:
				in.done = in.failed = true;
				in.status = r.text.empty() ? "ended" : r.text;
				progressed = true;
				break;
			}
		}
		size_t common = SIZE_MAX;
		for (const Inst& in : inst) common = std::min(common, in.bis.size());
		compare_upto(common);
		hub.advance(1);
		++st.rounds;
		if (all_done) break;
		if (!progressed) {
			if (++idle > 200000) {
				err = "deadlock: no instance can advance";
				break;
			}
		} else {
			idle = 0;
		}
	}
	size_t common = SIZE_MAX;
	for (const Inst& in : inst) common = std::min(common, in.bis.size());
	compare_upto(common);
	for (const Inst& in : inst)
		if (in.failed) {
			++st.failed_instances;
			if (st.first_failure.empty()) st.first_failure = "instance " + std::to_string(in.slot) + ": " + in.status;
		}
	return err.empty();
}

}  // namespace ronnet
