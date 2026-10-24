#include "uistr.hpp"

#import <Foundation/Foundation.h>

#include <cstdlib>
#include <unordered_map>

namespace uistr {

namespace {

ron::Language g_lang = ron::Language::English;
const ron::StringTables* g_tables = nullptr;
std::unordered_map<std::string, std::string> g_cache;

}  // namespace

void choose(const std::string& cli) {
	std::string code = cli;
	if (code.empty()) {
		if (const char* e = std::getenv("RON_LANG")) code = e;
	}
	if (code.empty()) {
		@autoreleasepool {
			NSString* s = [[NSUserDefaults standardUserDefaults] stringForKey:@"Language"];
			if (s.length) code = s.UTF8String;
		}
	}
	g_lang = code.empty() ? ron::Language::English : ron::parse_language(code);
	g_cache.clear();
}

ron::Language language() { return g_lang; }

std::string suffix() { return ron::language_suffix(g_lang) ? ron::language_suffix(g_lang) : ""; }

void use(const ron::StringTables* tables) {
	g_tables = tables;
	g_cache.clear();
}

std::string tr(const std::string& english) {
	if (!g_tables || g_lang == ron::Language::English || english.empty()) return english;
	auto it = g_cache.find(english);
	if (it != g_cache.end()) return it->second;
	std::string out = english;
	const ron::StringHash h = ron::string_hash(english);
	for (const auto& [cat, entries] : g_tables->categories()) {
		bool found = false;
		for (const ron::StringTables::Entry& e : entries) {
			if (e.hash == h.hash) {
				out = e.text, found = true;
				break;
			}
		}
		if (found) break;
	}
	return g_cache.emplace(english, out).first->second;
}

}  // namespace uistr
