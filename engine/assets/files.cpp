#include "assets/files.hpp"

#include <cctype>
#include <filesystem>
#include <stdexcept>

#include "assets/chunk.hpp"

namespace fs = std::filesystem;

namespace ron {

std::string to_lower(std::string s) {
	for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string normalize_game_path(const std::string& p) {
	std::string s = p;
	for (char& c : s) {
		if (c == '\\') c = '/';
	}
	while (s.rfind("./", 0) == 0) s.erase(0, 2);
	return s;
}

GameFiles::GameFiles(std::string root) : root_(std::move(root)) {
	std::error_code ec;
	if (!fs::is_directory(root_, ec)) throw std::runtime_error("not a directory: " + root_);
	// Follow symlinks (files and directories) so a mod overlay built from links to the install works as a game dir.
	const auto opts = fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied;
	for (auto it = fs::recursive_directory_iterator(root_, opts, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		std::error_code stat_ec;  // a broken link must not stop the walk
		if (!fs::is_regular_file(it->path(), stat_ec)) continue;
		// lexically_relative, not fs::relative: the latter canonicalizes and would follow a link out of the root.
		const std::string rel = it->path().lexically_relative(root_).generic_string();
		by_lower_[to_lower(rel)] = it->path().string();
	}
}

std::string GameFiles::resolve(const std::string& relative) const {
	auto it = by_lower_.find(to_lower(normalize_game_path(relative)));
	return it == by_lower_.end() ? std::string() : it->second;
}

std::vector<std::string> GameFiles::list_dir(const std::string& dir) const {
	std::string prefix = to_lower(normalize_game_path(dir));
	if (!prefix.empty() && prefix.back() != '/') prefix += '/';
	std::vector<std::string> out;
	for (auto it = by_lower_.lower_bound(prefix); it != by_lower_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
		if (it->first.find('/', prefix.size()) == std::string::npos) out.push_back(it->first);
	}
	return out;
}

std::string GameFiles::read_text(const std::string& relative) const {
	const std::string path = resolve(relative);
	if (path.empty()) throw std::runtime_error("missing " + relative + " under " + root_);
	const Bytes b = read_file(path);
	return std::string(b.begin(), b.end());
}

}  // namespace ron
