// Localised UI strings (docs/ui-port.md section 12.6): the language the interface runs in and `tr()`, which turns an English
// UI literal into the install's translation (string_lookup.xml of the language: every category is searched for the hash of the
// English text; no entry leaves the text as it is). The language comes from --lang CODE (en, de, es, fr, it, or the file
// suffix .7 .10 .12 .16), else RON_LANG, else the profile (NSUserDefaults "Language"), else English.
#pragma once

#include <string>

#include "assets/strings.hpp"

namespace uistr {

// Chooses the language from the three sources above (an explicit `cli` wins). Call once, before the game is built.
void choose(const std::string& cli);
ron::Language language();
// The suffix of the localised data files (".7" ...; "" for English): load_rules' lang_suffix.
std::string suffix();
// The tables the translations come from (the renderer sets them after loading); until then tr() returns its argument.
void use(const ron::StringTables* tables);
std::string tr(const std::string& english);

}  // namespace uistr
