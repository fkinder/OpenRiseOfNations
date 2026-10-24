# Roadmap to a complete game

Written 2026-10-23, after healing / attrition (combat-spec 7) landed. The phases are ordered so that each one
unblocks the next; every phase follows the project's method (spec from Ghidra in `docs/`, a reference tool in
`re/tools/`, the C++ port, a checker, a run in `ronmap`). `docs/STATUS.md` says where things stand inside a phase.

| Phase | What | Size |
|---|---|---|
| 0 | Recorded games as the oracle: replays (`.rcx`) and saves from the Windows game, `ron-sim --replay` comparing checksums frame by frame | ongoing |
| 1 | Leader bonuses: techs, wonders, rares and nation powers on hits, attack, armour, range, speed, LOS (combat-spec 5.1 step 8) | 1-2 sessions |
| 2 | Garrisons: enter / eject, garrison arrows, healing inside, capture with garrisons | 1-2 |
| 3 | Save / load (state-spec, `savdump.mjs`), script state included | 1-2 |
| 4 | Real skirmish: random map generation (bit-identical), the setup screen, menus and options | 3-5 |
| 5 | Heroes and generals (powers, spells, mana), transports, aircraft, carriers, missiles, nukes, spies, decoys, cloaking, packers, terraforming | 3-4 |
| 6 | Interface and finish: tooltips, market, library, messages, key map, unit sounds, options, a notarised app, performance with 8 players | 2-3 |
| 7 | Conquer the World: world map, territories, cards, campaign heroes, battle transitions | 3-4 |
| 8 | Multiplayer: lockstep networking, lobby, checksums, reconnection, replay compatibility | 4-6 |

Rules: one branch per phase, a commit per validated subsystem; nothing lands without the 156 stock scenarios passing
the self-check (and the replay comparison once phase 0 exists); every session ends by updating `STATUS.md`.
Phase 0 depends on recordings from the original game and makes every later phase safer.
