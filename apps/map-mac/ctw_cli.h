// Conquer the World command-line checks of ronmap (docs/ui-port.md section 10, docs/sim-implementation.md "Conquer the
// World"): no window, no renderer. The headless battle with a snapshot is in main.mm (it needs the renderer).
#pragma once

#include <string>

#include "sim/ctw.hpp"

namespace ctwcli {

// Case-insensitive pick of a campaign file stem from "alexander", "cold_war", "CTW_World_Map_01", "3" (list index) ...; "" if none.
std::string resolve_campaign(const ron::GameFiles& files, const std::string& name);

// --ctw-check: loads every campaign and prints its numbers (rounds, countries, links, colonies, styles); returns 0 when every
// campaign loads with the expected round counts (19 / 14 / 21 / 18 / 24 for the five shipped ones) and every link resolves.
int check(const std::string& game_dir);

// --ctw-sim CAMPAIGN [--seed S] [--rounds N] [--diff D] [--ctw-out FILE]: a headless campaign. The player holds back every
// round (the "no attack" bonus), the AI tribes play N rounds (battles against the player are decided by a draw of the
// campaign's own stream). Prints a summary and the save's CRC; writes the campaign save when `out` is not empty.
int simulate(const std::string& game_dir, const std::string& campaign, unsigned seed, int rounds, int difficulty,
             const std::string& out, bool verbose);

// Plays `rounds` rounds with the player holding back (AI rounds only; attacks on the player are decided by a draw).
void play_rounds(ron::sim::ctw::ConquestGame& g, int rounds);

// --ctw-selftest: the spec's acceptance checks that need no battle (docs/ctw-spec.md section 9): cost functions at the default
// attitudes, pick_style as a pure function, round length / level / age progression / territory income, the six outcome cases of
// process_results, the battle description of a link. Prints one line per check; returns 0 when all pass.
int selftest(const std::string& game_dir);

// --ctw-saveload CAMPAIGN: setup, 3 rounds, save, load, save again: the two files must be byte-identical.
int save_roundtrip(const std::string& game_dir, const std::string& campaign, unsigned seed);

// A link named on the command line: "auto" (the player's first available attack), "auto:N" (the N-th) or "FROM>TO" (territory ids). Fills `out`;
// returns false with a message in `err`. The player is made to be at war with the defender when the link needs it.
bool pick_link(ron::sim::ctw::ConquestGame& g, const std::string& spec, ron::sim::ctw::LinkChoice* out, std::string* err);

// --ctw-battle CAMPAIGN:LINK --frames N (no window): builds the battle of the link (every seat a computer player when `all_ai`),
// runs N frames, adjudicates (a game-over event, else the larger population wins: headless only) and applies the outcome to
// the campaign. Prints the numbers; returns 0 when the battle ran. `snapshot` hooks live in main.mm.
int battle(const std::string& game_dir, const std::string& campaign, const std::string& link, int frames, unsigned seed,
           int difficulty, bool all_ai);

}  // namespace ctwcli
