#include "node.hpp"

#include "sim/save.hpp"

namespace ronnet {

uint64_t fnv64(const std::vector<uint8_t>& b) {
	uint64_t h = 1469598103934665603ull;
	for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
	return h;
}

EngineNode::EngineNode(const ron::GameFiles& files, const sim::Rules& rules, const sim::AnimSource* art, std::unique_ptr<sim::Game> g,
                       const std::string& scenario, uint32_t seed, const std::array<int, 8>& who, net::ExecPolicy policy)
	: files_(files), rules_(rules), art_(art), game_(std::move(g)) {
	hooks_ = std::make_unique<net::GameHooks>(*game_, who, net::ChecksumMode::Native, policy, scenario, seed);
	hooks_->on_restore = [this](const net::Bytes& snap, std::string& err) {
		try {
			sim::SaveInfo info;
			std::unique_ptr<sim::Game> ng = sim::load_game_bytes(files_, rules_, art_, snap, &info);
			if (!ng) {
				err = "load_game_bytes returned nothing";
				return false;
			}
			game_ = std::move(ng);
			hooks_->rebind(*game_);
			hooks_->set_save_identity(info.scenario, info.seed);
			++recoveries;
			if (on_restored) on_restored();
			return true;
		} catch (const std::exception& e) {
			err = e.what();
			return false;
		}
	};
}

void EngineNode::attach(net::Session& s) {
	s.on_package = [this](int32_t frame, int slot, uint32_t stamp, const net::Bytes& data) {
		if (recording_) rec_.package(frame, slot, stamp, data);
	};
	s.on_recover = [this](const net::Bytes& snapshot, int32_t frame, uint32_t) {
		if (!recording_) return;
		std::string err;
		rec_.close(&err);
		++segment_;
		const std::string path = net::record_segment_path(base_, segment_);
		if (!rec_.open(path, net::record_info_for_segment(first_, snapshot, frame), err)) {
			error = err;
			recording_ = false;
			return;
		}
		segments_.push_back(path);
	};
}

bool EngineNode::record(const std::string& base, const net::RecordInfo& info, std::string& err) {
	base_ = base;
	first_ = info;
	segment_ = 1;
	segments_.clear();
	if (!rec_.open(base, info, err)) return false;
	segments_.push_back(base);
	recording_ = true;
	return true;
}

void EngineNode::finish_recording() {
	if (!recording_) return;
	std::string err;
	if (!rec_.close(&err) && error.empty()) error = err;
	recording_ = false;
}

std::vector<uint8_t> EngineNode::save_bytes() const { return sim::save_game_bytes(*game_, hooks_->scenario(), hooks_->seed()); }

}  // namespace ronnet
