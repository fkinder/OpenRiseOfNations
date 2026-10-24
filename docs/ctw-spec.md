# Conquer the World (CtW): campaign data, strategic rules and battle hand-off

Status 2026-10-24: ✅ file schemas (checked against all five shipped campaigns), campaign state layout, save format
(already in state-walk.md), diplomacy costs, cards table, battle set-up chain; 🟡 turn loop and outcome rules
(decompiles read, not run against a live game); ❓ the AI scoring in `plan_strategy`, the UI windows, the `.bhs` bodies.
Nothing here is ported yet (roadmap phase 7). Strategic layer = **not lockstep**: it runs on one machine between
battles with its own RNG; only the battle (`Setup::build_game` + sim) must stay bit-exact.

Related: script-spec §6.3 / §7 (CTW builtins, 330 scripts), rules-spec (all `CTW_*` constants, table at "ctw_starting_tribute"
and the lines after it), randommap-spec §1 / §3 (`build_game`, `Map::make`), randommap-resources §2 (`place_resources`),
state-spec §4 / state-walk.md "ConquestGame::walk_data" (the save), economy-spec / capture-spec (the battle-side effects).

## 0. Vocabulary and conventions

- **Slot** = one of the 24 base tribes (`aztecs, maya, inca, bantu, nubians, greeks, romans, egyptians, turks, spanish,
  french, germans, british, russians, chinese, japanese, koreans, mongols, iroquois, lakota, americans, indians, dutch,
  persian` = 0..23). `ConquestLeader` index, `ConquestNode.owner`, `ConquestPieces.lists[24]` and every stance matrix
  use the slot. A campaign **re-skins** slots: each `<TRIBE><KEY>` maps a slot to a campaign nation file (Alexander:
  slot 5 `greeks` = `MACEDONIANS.XML`, slot 23 `persian` = `PERSIANS.XML`; Napoleon: slot 0 `aztecs` = `SAXONY.XML`). Code
  that tests "Egyptians" / "Austria" tests a slot (7 / 5), not a name; Cold War US = 20, USSR = 13.
- **Effective owner** of a node: `owner`, replaced by `leader[owner].vassal_of` when that is ≥ 0. Almost every rule below
  says "owner" and means this (`eff(o)`). Barbarians are owner −1 (no leader).
- **Territory** = `ConquestNode` (stride 0xac); **army** = `ConquestPiece` (≤ 1 army per node, enforced by an error in `has_army`).
- `ConquestGame` (static `conquest_game`, 0x14bc bytes) owns an own `Random random` (+0x18). `Random::get(r, lo, hi)` here is
  **inclusive** (`rand(0,5)+... ×10` yields 10..60 tribute). Streams: every strategic draw uses `conquest_game->random`
  (seeded in `set_setupwin_data` from the pref "Seed (0 for random)", else `timeGetTime()`); `SoundGlobal::random` for
  voice lines; **`game_random`** only in `MapConquest::add_additional_players` (a battle-map draw) and, once per battle,
  `build_game` copies `info.seed = conquest_game.seed` into `game_random`. `random.random_seed` is inside the saved raw block.
- Difficulty 0..5 = easiest, easy, moderate, tough, tougher, toughest (`ConquestGame.difficulty`, also `info.difficulty`).

## 1. Files

| Path (install `Data/` or root) | Role |
|---|---|
| `Data/conquestrules.xml` (58 KB) | Game-wide rules: card types, stances, attitudes, map styles, game categories, ranks, default battle styles, army template, build-outs, resource multipliers, no-attack floor. Loaded by `init_rules_data` @78d460 (also after a load) |
| `Data/conquest.xml` (190 KB) | **UI only** (`CONQUESTWIN`, `CONQUESTSTARTWIN`, 977 `IFACETEX`, buttons, `CONQUESTHELPPOINTER`...). No rules; out of scope |
| `conquest/CTW_<Name>_Map_01.xml` ×5 | Campaign definitions: `Alexander`, `Napoleon`, `New_World`, `Cold_War`, `World` ("The Entire World", scriptless free-for-all). Found by `./conquest/` + name + `.xml` (`set_setupwin_data` @78e9c0); the list shown by the setup window is ❓ (window code not decompiled) |
| `conquest/CTW_*_Map_01.png`, `_strings.xml`, `_info_text.xml` (+ `.4 .7 .9 .10 .12 .16 .17 .18` = language variants), `RenderMaps/` | Map art (out of scope), localisation (`String::lookup`, category hashes) and the per-style story text (`<OFFENSIVE><ENTRY name="CONQUEST">`) |
| `conquest/<Campaign>/*.xml` | Nation files (`TRIBE name=... substitute="1"`, `LEADERS`, `CITIES` name pools; `RULES` as in `tribes/`); `alex_barbarians_*.xml` etc. via `<CONTINENT barbarian_file>` (default `barbarians.xml`) |
| `conquest/<Campaign>/*.scx/.sce` (165) | Custom battle maps (`<STYLE custom_map>`), loaded as scenarios (map style 8 / generator 0x1a) |
| `conquest/<Campaign>/*.bhs`, `conquest/scripts/*.bhs` (330 incl. 28 shared) | Strategic hooks and battle scripts, §8 |

### 1.1 Campaign file (`ROOT` attributes; all read in `setup_conquest_game` @7928c0 with defaults 0 unless stated)

`name, desc, intro, victory_conditions` (string-table keys), `intro_image, minimap_continents, tex_width/height, map_width/height,
map_scale/global_scale, camera_*` (art/UI), `starting_age` (0), `ending_age` (**7**), `starting_round` (0), `turns_per_age`
(**2**; stored `scen_per_age`), `hero_unit` (type name, → `hero_type`, −1 if unknown), `allow_vassals, allow_continue,
allow_wrap, client_state, strategic_nukes, ally_move, free_move, no_gov_hero, territory_income` (Cold War = 1, others 0),
`no_oath_cards, no_patriot, disclaimer, run_intro_scenario, camera_loc` (UI/flags), script paths `battle_script_file,
post_turn_script_file, diplo_response_script_file, diplo_offer_script_file, links_script_file, initialization_script_file,
strategy_script_file, post_ai_script_file`. `battle_script_file`, `post_turn`, `links` and `post_ai` are compiled at load;
a compile failure of the battle script clears the name.

Shipped values: Alexander 19 turns (age 1..1 ×19), Cold War 14 (ages 6..7 ×7), Napoleon 21 (age 4 ×21), New World 18 (ages 3..4 ×9),
World 24 (ages 0..7 ×3). **Length = (ending_age − starting_age + 1) × turns_per_age rounds**, `check_turns_done` @78ab70.

Children: `<TEXTURE file alpha tile_ratio>` ×6 (art), `<CONTINENTS image_file>` `<CONTINENT name id barbarian_file image_file>` (pushes
`conquest_continents`, `barbarian_files`; `id` is the key the countries reference), `<CONQUESTSTYLES>` (§1.3), `<COUNTRY>`+, `<COLONY>`
(Napoleon, 14), `<BONUSCARDDECK name>` (6..13; the campaign's deck, filtered by `Categories::find_key` on the card type name), `<TRIBES>`,
`<VALID_NATION owner>` (the nations the player may pick; ❓ exact effect), `<END_GAME_IMAGES>`, `<INSTRUCTIONS>` (Napoleon's two commands),
`<CONQUESTGRAPHICPIECES>` (Cold War, 20 `PIECE`+`SPHERE`: 3D army/nuke models, art), `<MINI_MAP_RECT>`.

### 1.2 `<COUNTRY>` / `<LINK>` / `<COLONY>` / `<TRIBE>`

`load_country_data` @75d630. `<COUNTRY name id value size red green blue continent tileset owner capital wonder rare card supply_depot
tribute priority_target nukes left right top bottom x y>`:

| Attribute | → `CountryInfo` → `ConquestNode` | Notes |
|---|---|---|
| `name`, `id` | `name` (+0x68), `id` (+0x7c) | `id` is what `<LINK to>` and scripts use; localised name via `COUNTRYNAMES` |
| `red green blue` | map-art pick colour | the country is found by reading the colour under the cursor of the map bitmap (`country_pick`) |
| `continent` | `continent` (+0x28) = index of `<CONTINENT id>` or −1 | |
| `tileset` | `tileset` (+0x24) = index in `tilesets.xml` `//TILESETS/TILESET` by name; `random` → rand(0..n) | selects the battle tileset (`TileSet::load_tileset` in `build_conquest_game`; default "dirty") |
| `owner` | `owner`, `founder`, → flags | tribe KEY (case-insensitive, matched to `<TRIBE><KEY>`); absent = −1 barbarian |
| `capital` | `capital` → node flag 2 | owner's capital (1 or 2 per nation; a 2nd `capital=1` of the same owner → `capital2`) |
| `size` | clamp 0..7; capital's start **level** (0 → `ctw_capital_starting_infra` = 2) | also non-capital owned countries get `level = size` if > 0 |
| `value` | `value` (+0x34) ≥ 0 | AI weighting ❓ (Alexander 0..12, Napoleon 40..280) |
| `tribute` | `tribute` (+0x2c); `random` → (rand(0..5)+1)×10 | **one-time** tribute paid to whoever conquers the node (`transfer_to`) |
| `rare` | `rare_resource` (+0x20) = goods type index (name lookup); `random` → rand(6..50), re-rolled while 20 (marble); with `random_bonuses` also while 6, 31 or a goods tier > 4 🟡 | one per country; colonies list several |
| `wonder` | `wonder` (+0x14) = wonder type index; `random` → rand(0x20e..0x21f) | owner gets it in `wonders` (Colossus, Hanging Gardens, Pyramids, Kremlin 0x21a ...) |
| `card` | `bonus_card` (+0x58): card name via `ConquestBonusCard::get_name`; `random` → a random card from `make_card_list` | conqueror receives the card (§4) |
| `supply_depot` | node flag 4 | each depot owned = +1 army (§2.4) |
| `priority_target` | flag 0x10 | AI preference ❓ |
| `nukes` | `nukes` (+0x38) | Cold War silos |
| `common_res`, `common_res_amt` (not in shipped XML) | +0x18 / +0x1c | ❓ unused |
| `left right top bottom x y` | map-art rectangle and label point | UI |

`<LINK to sea [from]>`: `to` = country id; `sea="1"` = needs a transport/sea crossing; links are listed on both ends (shipped files:
106 / 182 / 232 / 128 / 218 links, all resolvable, topology symmetric, the `sea` flag differs between the two directions on 4 / 6 / 0 / 2 / 4
pairs, and the World map has 2 one-way links; Cold War has 3 `from=`). `ConquestNode::add_link`; battle kind of a link = its `sea`.
`ConquestLink` runtime also holds `distance, battle_cat, battle_index, opponents, supporters, add_supporter_armies`.

`<COLONY name id tribute value owner wonder>` + `<RARE type=goodname>`+ → `ConquestColony{id,name,rares,wonder,value,tribute,owner}`
(`ConquestColonies::init`): income per turn, §2.5. `<TRIBE>`: `FILE, KEY, SKILL` (1..100, default rand(1..100) → `inherent_skill`),
`NO_ELIMINATING` (flag, `no_eliminating`), `NO_PROPOSE_ALLY`, `BASE_ARMIES` (default **1**; 0 = a *client state* that can only be absorbed
as a vassal, §6), `BONUS_CARDS/CARD` (starting hand), `UNIQUE_UNIT`, per-tribe `END_GAME_IMAGES`; read by `ConquestLeader::init` @79cc40 and `Tribes::init`.

### 1.3 `conquestrules.xml` and `<CONQUESTSTYLES>`

- `CATEGORIES id=conquest_bonus_cards`: 38 types (§4) with `DATA[0..5]` = strategic, tactical, in standard deck, may be placed on the map,
  may stack, tribute value. Entry stride 0x58 in `ConquestBonusCardTypes`; getters `is_strategic/is_tactical/can_stack/get_value`.
  `conquest_stances` {War 0, Peace 1, Ally 2, Vassal 3}; `conquest_attitudes` {Benevolent 0-2, Enthusiastic 2-5, Friendly 5-8, Cordial 8-11,
  Suspicious 11-14, Hostile 14-18, Hateful 18-21} (name = the first band whose upper bound is above `attitude_base`); `conquest_map_styles`
  {Standard 0, No City 1, Player City 2, Enemy City 3, Defender 4, Attacker 5, Attacker Defense 6, Defender Attack 7, Custom 8};
  `conquest_game_cats` {Offensive 0, Defensive 1, Barbarian 2, Custom 3, EspionageUSA 4, EspionageUSSR 5} (**index = style category**);
  `conquest_rankings` (10 end-game titles, cosmetic).
- `<CONQUESTSTYLES>` has `<OFFENSIVE> <DEFENSIVE> <BARBARIAN> <CUSTOM>` (+ `ESPIONAGEUSA/USSR` in Cold War) groups of `<STYLE>`; `ConquestStyle::init`
  @7a7ce0. Attribute names are the *ids* of the game-setup category tables, lower-cased: `eliminations` (Capital / Conquest / Sudden Death Capital),
  `victories` (Time Limit ...), `gamestyles` (team style), `rushrules` (`random` → 11..13), `startingresources`, `gamerules`, `revealmaps`, `mapsizes`;
  numbers: `reveal_buildings, add_opponents` or `add_opponents_diff0..5` (extra AI nations per difficulty), `probability`(100), `min_players`(2),
  `min_victory/max_victory` (minutes), `land_only, water_only, min_age, opp_team_together, default, no_rare, no_hero, not_first_time, enemy_override`
  (+`_tribe`), `player_start_x/y, ai_start_x/y` (tile coordinates, −1); strings `title, id, setup, runtime, bonus, custom_map, objective, description,
  victory_text, defeat_text` (`$NUM0` = `ctw_conquest_time_limit` = 90), `map_style` (name → index above, default 0). **The `map_sizes` / `reveal_maps` spellings
  that appear in the XML (Field Battle in conquestrules.xml, ColdWar) are not the category ids and are ignored by the reader** 🟡 (inferred from the
  attribute-name construction; `mapsizes` / `revealmaps` are honoured).
- `<ARMIES min_arrival_time=2 max_arrival_time=4>` `<UNIT type num min_diff>` ×11: **the composition of one army** (slingers, bowmen, hoplites, horse archer,
  light horse, supply wagon; `min_diff` default 0 means every difficulty). `<MIN_NO_ATTACK><DIFFICULTYn val>` = 4,4,3,3,2,2.
  `<DEFAULT_BUILD_DIFF>Tough` and `<BUILDINGS_<DIFF>>` (EASIEST..TOUGHEST) → 8 `<LEVEL>` (territory strength 0..7) → `<CITY>`+ → `<BUILD type alt num>`
  (`alt` = substitute if the building already exists; `Setup`-style build-out of the *defender's* start cities); `<RESOURCEBONUS_<DIFF>>` → 8
  `<LEVEL bonus=1|2>` (multiplier on the defender's six starting resources).

## 2. Campaign state and turn structure

### 2.1 State (`ConquestGame`, field list = pdbtypes; the saved subset in state-walk.md)

Scalars: `player_tribe, difficulty, seed, cur_round, num_turns, turns_skipped, cur_age, starting/ending_age, scen_per_age, turns_no_attack, game_end,
link` (index into `available_links`), `return_to_node, defcon_level, nuke_launch_turns, talleyrand_diplo_tribe/state, fouche_tribe`, flags from §1.1.
Arrays: `conquest_leaders[24]` (`ConquestLeader`, 0x484), `conquest_nodes` (0xac), `conquest_colonies`, `conquest_pieces` (24 lists of armies),
`reinforcements`, `valid_tribes` (the AI move order), `overrun_armies`, `continents_captured`, `bonus_card_deck` (names), `stored_ints/strs/diplo_deals`
(script storage, `get_ctw_int`...), `conquest_news`. Not saved: `available_links`, `start_*` arrays, card types, rules (re-read on load).
`ConquestLeader`: `tribe, capital, capital2, nodes_controlled, supply_depots, tribute, strength, attack_link, inherent_skill, num_cards_purchased,
vassal_of, base_rate[6], stances[24] (0 war,1 peace,2 ally,3 vassal), stance_time[24], attitude_base[24], no_attack_time[24], bribe_paid[24], nodes_won_from[24],
bonus_card_types_played[38], colony_blockade, base_armies, eliminated_by, active, valid, rare_resources(+num), wonders, techs, continents, bonus_cards,
fought/met bitmasks, bonus_cards_selected, new_nodes, no_attack (node list), no_declare_war`. `ConquestNode`: `owner, level (0..7), turns_held, founder, wonder,
rare_resource, tribute, flags (1 fortified, 2 capital, 4 supply depot, 8 visible, 0x10 priority), value, nukes, tech_cat, capital_name, bonus_card, links`.

### 2.2 Setup (`setup_conquest_game` @7928c0, `set_setupwin_data` @78e9c0)

1. Player picks campaign, difficulty (0..5), nation (−1 = random among `VALID_NATION` owners), `random_nations`, `random_bonuses`.
2. Nodes: one per `<COUNTRY>` in file order; `tech_cat = (i + rand(0..5000)) & 3` (Eureka's category for that node).
3. `random_bonuses`: rare / tribute / card of each country re-rolled (rare rand(6..50), tribute (rand(0..5)+1)×10, card from the deck list), values
   `random` in the XML are always rolled. The deck order is `make_card_list`: one entry per variant, each inserted with key `rand(0..5000)` (a shuffle).
4. Start positions: for each owned country, a capital (`founder` = owner) is set; `random_nations` assigns the owners from a shuffled list
   (`ordered_insert` keyed by `rand(0..1000)`) instead of the file's; capital `level`, start card hand, `ConquestLeader::init` (stances all **peace** (1),
   `stance_time` 0, `attitude_base[j] = rand(0..5) + ctw_start_attitude(10) − 2`, tribute = `ctw_starting_tribute` (25), met = all), `generate_name`,
   `base_armies > 0` → first army on the capital; supply-depot countries of an owner add an army.
5. If the player slot is not yet fixed: `player_tribe = candidates[rand(0..n)]`, player name = profile name.
6. `turns_no_attack = min_no_attack[difficulty]`; `initialization_script` runs (e.g. `set_no_attack`, `ctw_set_sub_goals`); `assign_bonuses`.

### 2.3 Round structure (one **round** = every nation moves once; `cur_round`++)

The player moves first: moves armies along `available_links` (`find_available_links` @794d80), fights or negotiates, cards, `end_turn` @792700
(skipping an attack: tribute `min((turns_skipped+1)×ctw_no_attack_bonus, ctw_no_attack_bonus_cap)` = 25.., cap 100, doubled by an Antipater card (type 20);
`turns_skipped++`). Then **`return_from_scen` @78f040** drives the AI: `valid_tribes` = all active, valid, non-player leaders in a shuffled order
(`rand(0..0x7fff)` key); for each tribe: `play_strategic_cards`, `do_diplomacy`, then loop `plan_strategy` until it finds no attack (`resolve_overrun` for
every auto-win it reports); an attack on the player or a human-visible one opens the invade window (`resolve_ai_strategy`) and the battle is fought
(§5). After all: `assign_bonuses`, `post_adjust_attitudes`, `advance_round`, `find_available_links(player)`.

`advance_round` @790220: `cur_round++`, `num_turns++`; when `cur_round % scen_per_age == 0` and `cur_age < ending_age`: `cur_age++` and every owned
node with `level < ending_age` and not in its owner's `new_nodes` gains **+1 level**; `turns_held++` for every node not in `new_nodes`;
Talleyrand's one-turn stance is applied/reverted; per pair `stance_time[j]++` if the stance is not war and `no_attack_time[j]--` (on 0 the
`bribe_paid[j]` = 0); `ctw_*` per-turn card counters reset (blockade); `clear_no_attacks`; from age 6 on all nodes become visible and `fought` full;
`check_allied_victory`; at `cur_round == length` `check_continue_playing` (continue if `allow_continue`, else `game_end = 3`); `turns_no_attack` −1 (floor 0);
`nuke_launch_turns` −1. Warnings are shown at 3, 2, 1 rounds left.

### 2.4 Armies and supply

Per leader: army cap = `1` (+1 with two capitals) `+ number of its vassals + supply_depots` (nodes with flag 4 owned, vassals' included);
`place_new_armies` @7a1f10 adds one army on an owned empty node (AI: `find_best_army_loc`; player: picks) or, if over the cap, removes the first one
(`army_lost` message for the player). A vassal has cap 0 (its armies vanish). `need_new_armies` compares with `base_armies + vassals + supply_depots`. An army
may move once per round (`moved`), to an adjacent node (`can_drop_army` @7a6540 returns −1 not adjacent, −3 an attack is pending, −7 / −8 war / no-attack
restrictions, ...). `free_move` (Cold War) lets armies reposition freely; `ally_move` lets allied armies use each other's nodes.

### 2.5 Tribute, income, colonies

- Tribute is the currency (`tribute`, 25 at start). Gained: conquest (`node.tribute` once, `take_tribute` flag), `end_turn` skip bonus, defending win
  (`ctw_def_victory_bonus` 50 to the AI defender who beat the player), vassal capture (+node tribute), continent capture (`ctw_continent_bonus` 10 × nodes for AI,
  a popup/card for the player), Letter of Marque, elimination (the victim's whole purse), `demand_tribute`.
- `check_income` @78edb0 (after each nation's move): colonies owned → `Σ colony.tribute` unless `colony_blockade > 0` (then the counter drops by 1);
  with `territory_income` (Cold War): `Σ over owned nodes (level × ctw_terr_income_x + ctw_terr_income_base)` (1 and 1).
- `assign_bonuses` @78b140 (each round): `nodes_controlled`/`territory_value` count; per leader the set of **distinct rare resources** it owns (nodes and colonies)
  adds each goods type's two economy rates (`GoodType +0x2dc/+0x2e0`, amounts `+0x2e4/+0x2e8`) into `base_rate[6]`; those become the battle's resource
  bonus (`num_ctw_rate_bonuses`, §5.3) and, with `setup_scenario_rares`, the leader's unlocked rare bits for the battle. `colony_blockade` also suppresses it.

## 3. Diplomacy between turns

Stance matrix `stances[i][j]` is symmetric (every setter writes both rows): 0 war, 1 peace, 2 ally, 3 vassal(master/vassal). `set_war` (only if not already
war): `attitude_base = min(stance_time + attitude + 5, ctw_max_attitude 20)`, stance_time 0, news; Cold War: `defcon_level += 2` (0..5) when the target has
armies. `set_peace` / `set_ally`: `attitude −2` (floor 0). `set_vassal(l, v)`: v.`vassal_of = l`, stance 3 both ways; the master's other allies become
peace with v; `supply_depots` recounted; v's armies dropped; both get fresh armies (`place_new_armies`); the former vassals of v are released first.

Costs (all in tribute, constants `CTW_*`): war = `base_war 20 + stance_time × 1` (× `ally_war_cost_x` 2 when currently allied; Cold War US/USSR pair uses
`nuclear_enemy_war_cost 450 − age discount 100 (final age) − defcon_war_discount 25 × (defcon−1)`); peace = `base_peace 20 + attitude × 1`; alliance = peace cost
`+ 100 × (current allies)`; territory = `75 + level × 25 + attitude × 2` (buyer ≤ `territory_max_att` 17, target size ≥ `territory_min_emp_size` 4); vassal
= 100 (default) paid to the vassal; bonus card = `min(30 + 40 × cards_bought, 110)`; build (a fortification) = `20 + 10 × level`; nuke = `75 + 35 × nukes owned`.
`attitude` (`get_attitude` @7a2e80) = `attitude_base` `+2` if the two hold the same largest node count, `+ (adjacent contacts − 1)` when > 1 shared border nodes,
`+ stance_time` (war: positive; peace/ally: `−stance_time`, and ally −2 more), −1 while `no_attack_time > 0`; the *name* uses `attitude_base` only.
`demand_tribute(target, node)` @7a2460 moves half of the payer's purse (≥ 10) to the receiver and sets `no_attack_time = ctw_trib_no_attack + 1` (4 rounds) between them (`bribe_paid` records it).
AI demands on the player succeed with `rand(0..100) < ctw_demand_trib_prob` 35 (`..._tough_prob` 50 above difficulty 2; refused when attitude > `ctw_max_bribe_att` 15 on tougher difficulties); a bribe
offered by the player is accepted with `ctw_bribe_acc_prob` 100 (difficulty < 3), `ctw_bribe_tough_acc_prob` 50, or `ctw_bribe_tough_hate_acc_prob` 35 when the attitude is above 15; never when
`reject_bribe`, `no_negotiation_text` is set, `strategic_nukes`, or the payer holds < `ctw_bribe_trib_min` 10.
Alliances are broken by `do_diplomacy` with `rand(0..100) < attitude × ctw_break_alliance_prob_x_att` once older than `ctw_min_keep_alliance` 4 ❓ (only the
constants and the call to `ConquestDiploOffer::make_ai_offer` / `swap_tribes` / `set_stances` were read; the `diplo_offer.bhs` and `diplo_response.bhs` bodies decide offers).
`allied_punks` = allies of the player's side that join a battle. `no_attack`/`no_declare_war` node lists (script `set_no_attack`) forbid links; `min_no_attack[diff]` is
the global post-attack immunity of the player (`attacked_player` @78a830 resets `turns_no_attack`; `can_attack_player` = 0 left).
Elimination: losing the **last** capital (`transfer_to` on a capital with no second one) → `ConquestLeader::defeat`: `active = 0`, `eliminated_by`, vassals freed,
purse + every node + colony go to the conqueror, armies deleted, player's slot → `game_end = 2`.

## 4. Bonus cards

Types in `conquest_bonus_cards` order (index = `ConquestBonusCard.type`): 0 Oath of Fealty (per-tribe: gain that nation's powers for one battle; variants 0..23),
1 Wonder of the World, 2 Sabotage, 3 Eureka (5 tech-category variants), 4 Economic Boom (6 resource variants minus #5), 5 Mercenaries, 6 Treachery, 7 Great Thinker,
8 Merchant Guild, 9 Logistics, 10 Partisans, 11 Cultural Dominance, 12 Trade Embargo, 13 Propaganda, 14 Skill Bonus, 15 Commandeer Transport, 16 Brigands, 17 Spy Network,
18 Elephant Army, 19..24 Generals Parmenio / Antipater / Ptolemy / Memnon / Spitamenes / Porus, 25 Traitors, 26 Colony Blockade, 27 Political Dissidents, 28 Spy Academy,
29 Continental System, 30 Letter of Marque, 31 Warpath, 32 Missionaries, 33 Population Boom, 34 Advisor Fouché, 35 Advisor Talleyrand, 36 Commander Schwarzenberg, 37 Commander Paoli.
`max_amt` / `amount` union: Spy Academy `ctw_num_bonus_spies` 3, Letter of Marque `ctw_steal_trib_amt` 100, Missionaries `ctw_missionaries_bonus` 25, Population Boom
`ctw_num_bonus_citizens` 5, Schwarzenberg `schwarzenberg_commandos`; Skill Bonus amount = the battle's `bonus` script result. A leader's `num_bonus_cards[type]`
(Leader +0x68f6) counts cards in play this battle.

**Strategic** (played on the map, `play_strategic_card` @78f3c0, targets from `set_card_targets` @791310): 1 build a wonder of the current or an earlier age in an owned node
(`find_available_wonders`; `ctw_wonder_min_civics` 4); 2 Sabotage: enemy node level −2 (min 0); 6 Treachery: take a non-capital node adjacent to you with level ≤ 4
(`ctw_max_treachery_infra` 3 caps the shown level), no battle, war declared on the old owner if not at war; 15 Commandeer Transport: teleport one army (`free_move`
for the turn); 16 Brigands: capital level ÷ 2; 22 Memnon: node level `+ctw_memnon_infra` 4 (cap 7); 23 Spitamenes: node → barbarian; 26 Colony Blockade:
`colony_blockade = ctw_colony_blockade_turns` 2; 29 Continental System: Great Britain node level −2; 30 Letter of Marque: steal `min(100, victim tribute)`; 34 Fouché: force war between
two nations; 35 Talleyrand: ally or war with the player for one round, then restore (`talleyrand_diplo_*`); 37 Paoli: liberate another nation's vassal.
**Tactical** (selected before a battle: `ConquestLeader.bonus_cards_selected`, applied in `setup_scenario_bonuses` @78be90, consumed in `process_results` unless kept):
- 0 Oath: tribe-bonus bit of the card's nation in `conquest_racial_powers`; 3 Eureka: `ctw_eureka_techs` 2 techs from the node's `tech_cat` line (or `ctw_eureka_all_techs` 1
  from each of four lines for variant 4); 4 Economic Boom: `num_ctw_rate_bonuses[res]++` (`ctw_prod_rate_bonus` 5 %); 5 Mercenaries: +1 extra army in the start group
  (`start_add_armies`); 11 Cultural Dominance: all non-allied opponents get `leader_flags2 |= 0x40` (powers off); 13 Propaganda: gain the enemy's nation power
  (bit of the opponent in `conquest_racial_powers`); 14 Skill Bonus: +amount of each of the six resources; 17 Spy Network: `leader_flags |= EXPLORE_MAP 0x1000`;
  31 Warpath: 2 military techs + Barracks, Stable, Siege Factory near the start (CTW and the scenario position); 25 Traitors: informers (spell 0x9fc) at every
  enemy military building (mask bit per player in the building, `build_conquest_game`); 27 Political Dissidents: non-allied opponents `leader_flags2 |= 0x800` and
  governments 0x26f..0x274 removed; 7/8/9/10/12/32 modify rates read in the sim by the `ctw_*` constants (Great Thinker 25 % cost/time, Merchant Guild 25 %/20 %,
  Logistics heal +100 % and no attrition, Partisans attrition +`ctw_attrition` 50 % via `Leader::calc_attrition`, Missionaries +25 % temple effect) — the reads in
  research / market / healing code are ❓ not traced; units from cards (18 elephants `ctw_num_bonus_elephants` 6, 19..24 generals + `ctw_parmenio_cav` 5 /
  `ctw_spitamenes_archers` 5, 28 spies, 33 citizens, 36 commandos, 34 Fouché's spy) are created in `build_units` @797200.
`process_results` consumes every selected card (`bonus_card_types_played[type]++`) except Parmenio (19) on its first play; a card gone from the hand is removed.
AI picks tactical cards with `ctw_ai_tac_card_prob` 25 (`..._ally_prob` 50 when allies fight) per card and uses its whole hand with probability `ctw_ai_all_cards_prob` 25 once it holds
`ctw_ai_all_cards_num` 4+ at its capital. Buying a card: `buy_bonus_card` (cost above; at most `ctw_max_wonder_cards` 1 wonder cards).

## 5. From a link to a battle

### 5.1 Which battle (`ConquestStyles::pick_style` @7a95f0, `setup_scenario_rules` @78cc80)

`find_available_links` gives, per army on an owned (or allied, `ally_move`) node, every adjacent node not owned by the mover, not fortified (flag 1), not a capital of a
`no_eliminating` nation unless the target is the player, not in the mover's `no_attack` list; plus the script `links_script_file`. `process_link` adds up to 3 enemy-side and 3
attacker-side **supporting** nations (a neighbour of the defended node owned by an ally of one side with an available army; allied supporters get `add_supporter_armies++`).
Category: player attacks → **0 Offensive** (2 Barbarian if the defender is barbarian), player defends → **1 Defensive**, espionage (`link < 0`) → 4/5 by `cur_espionage_level`.
Candidates are the category's styles with: `default` set, or (not a capital node, `level ≤ ctw_max_infra_special_scen` 3, army counts balanced `(1 + supporters_att == supporters_def)` or
`(1,0)`, no allied supporters) for the special ones; `has_fought_barbarian || !not_first_time`; `min_age ≤ cur_age`; land links reject `water_only`, sea links `land_only`. Each gets a key
`rand(0..100)` (own `Random` seeded `seed + to×0x25 + from×0x11` when the link is known, i.e. **deterministic per link**; else the campaign RNG). If any non-default style
qualifies the defaults are dropped; the lowest key wins. The campaign `battle_script_file` is then called (args: supporter counts, level, age, round, tribe names, node ids) and may
return a style `id` of the CUSTOM group (named historical battles) which overrides the pick. `style_index1/2` record the choice for the save.

### 5.2 Parameters (`setup_scenario` @7999e0 → `GameInfo`, `ConquestStyle::set_game_rules`)

`info` = `GameInfo::default_setting` ×30, `flags |= 0x80000000`, `starting/ending_technology = cur_age` (+1 on the end if `cur_age < ending_age`), `wonderwin 12`, `game_rules 1`,
`team_style 1`, `reveal_map 1` (Explored when the style says so), `difficulty`. The style overrides what it defines: `victory` + goal (time limit → `ScenarioData::custom_time_limit =
rand(min_victory..max_victory)` minutes, `time_limit = custom`; score / population / economic goals from their tables), `rush_rules, starting_resources, game_rules, reveal_map, map_size,
team_style, elimination`; `victory_text` / `defeat_text`. `starting_town = 2` (small cities) when the player attacks. **Players**: `info.players = additional_opponents[difficulty] +
|link opponents|`; in a default-style battle the link's supporters are added as real seats, in a special style they are dropped and those allied to the player are
remembered in `allied_punks` (the link's supporter lists are cleared); seat 0 = player (flags 7, team 0), seat 1 = the defender/attacker nation (team 1),
allies of the player's side share team 0, others team 1 (`opp_team_together` → all 1); barbarian seats get `tribe = 0xff` then the first free tribe index with `Tribe::set_barbarian(continent's
barbarian file)`. `map_size`: 2-3 players → 3, ≥ 4 → 4, `+1` for a sea link, or the style's `mapsizes`. Any seat whose tribe slot is not substituted gets
`Tribe::set_substitute_tribe(campaign name)` (so slot 5 plays as Macedonians). `runtime_script` becomes `info.scenario_path / scenario_script` (name without extension) and
`load_scenario_script = 1`; `objective` is added as the CTW objective; `ctw_conquest_time_limit` 90 = `$NUM0`.

### 5.3 `build_conquest_game` @797c40 (called from `Setup::build_game` step 4; semaphore byte2 & 2)

World size from `map_sizes[info.map_size]`; map generator = **`MapConquest`** (map style 0x18 land, 0x19 when the link is a sea link, 0x1a = scenario map when `map_style == 8`, then
no `World::init`/cities); tileset = the defended node's (`node.tileset`, `"dirty"` default; for style 5 the attacker's node) unless style 8; `Leader::init` per valid seat;
`new_map(...).make(seed = conquest_game.seed)`; `setup_scenario_diplo` for every seat pair (copies the campaign stances: war/peace/ally); `setup_scenario_bonuses` (tactical cards §4 and the
tribe bonuses Dutch/Russian/Aztec/Roman/American/Persian free techs and Persian despotism); `setup_scenario_rares` (rare bits from owned nodes and colonies, `known_rares`); then the two
loops that read `num_bonus_cards[27]` (Political Dissidents) and `[25]` (Traitors); `ScenarioEditor::init_terrain` unless style 8; `Setup::build_empire(who, start)` for every start with
`start_capital`, then the others (`build_cities` / `build_units`, below); `reveal_buildings` → `set_explored_show_buildings`; `setup_reinforcements`; the style's `setup_script` runs once
(`ConquestGame::get_setup_script`, script-spec §6.3), the `runtime_script` every frame.
**The CtW flag (`semaphore byte2 & 2`)**: `Setup::build_game` uses `conquest_game.seed` for `info.seed`, takes the scenario script from `load_scenario_script`, compiles the general-powers
script and the CtW setup script; `Map::place_resources` skips goods with `exclude_ctw` and the whole call when the style has `no_rare`; `Map::init_map_data` skips the `MAPINFO` element;
`Leader::calc_attrition` multiplies by `(ctw_attrition+100)%` when `num_bonus_cards[10]`; `Unit::find_repair_spot` reads `info.difficulty`; hero death/damage messages
(`Object::do_damage`, `Unit::close` when the unit type is `hero_type` and it is the console player's); `Unit::close` bumps `CommandPackage::random_seeds`. The flag is
cleared in `ConquestGame::close` @79adc0; **where it is set is ❓** (window code).

**Start positions.** `MapConquest::add_additional_players` places the extra nations near the main two (`game_random` draw); `check_starting_loc` / `check_sea_map` validate land/sea. 
`build_cities` @796140 (per seat `who`): none when `starting_town == 0`, nation flag 0x80, or by map style (1 No City, 8 Custom: none; 2 only seat 0; 3 only seat 1; 6/7 only seats 0..1);
a CITY at the start; name from the node's `capital_name` or `get_city_name` (rotates through the nation's `CITIES`, `city_name_mark[slot]`); `small/large_city_buildings`; then for the
**defender in a default-style battle**: `buildings = BUILDINGS_<diff>[node.level]` (the player uses `DEFAULT_BUILD_DIFF`; `build_out[diff][level]` lists, extra `<CITY>` entries found extra cities),
`alt` substitute if the building exists, age-gated (`type's age > cur_age` skipped), plus the wonder of the node (`produce_building`), and **all six starting resources ×
`RESOURCEBONUS_<diff>[level]`** (1 or 2). Warpath and similar cards add buildings. `build_units` @797200: `Setup::build_units` start group, then for each **army** present
(`start_add_armies[seat]`, from the invaders and the defending node's army, +1 per Mercenaries card) one copy of the `<ARMIES>` composition at the start (`army_comp.diff ≤ difficulty`
or a human seat), upgraded to the nation's current unit (`current_upgrade`, `no_gunpowder` falls back along the previous-unit chain); card units; the **hero** (`hero_type`) for the player's
nation in a default-style battle unless `no_hero`.
**Reinforcements** (`setup_reinforcements` @789d70): each neighbour of the defended node whose owner is the attacker's (with an available army) or the defender's (with an army) sends one
`ReinforcementArmy{time, who(0 player side, 1 other), node, loc, dest}`; arrival times accumulate per side: `t_k = Σ_{i≤k} rand(min_arrival..max_arrival)` minutes (2..4 each, window shifted by the
previous total), ×60 s; entry edge from the map direction (`find_reinforcement_loc`, `find_angle`, `map_to_wcoord`).

## 6. Outcomes (`process_results(kind)` @798940)

`kind` 0 = the battle was played (won iff `info.player[0].flags & 0x100`), 1 = attacker wins without a battle (overrun / autoresolve), otherwise a forced loss. Always: `player_scenarios++`,
`style.times_played++`, `player_time += game tick` (kind 0), played tactical cards consumed (§4), wonders no leader still owns are released from their node. On the loss path, when the
game's `armageddon` counter has reached its threshold (`armageddon + per_team × sides + per_nation × nations`, ×2/×3 for some starting-resource settings, ≥ 100 for setting 8) → `game_end = 4`.
- Espionage (`link < 0`): `cur_espionage_level++`, achievements at the style's count; bonus card from the `bonus` script; autosave.
- **Player won as attacker**: node → player (`transfer_to`, tribute and card collected); a defender with `base_armies == 0` (client state) is instead made a **vassal** (`set_vassal`) and its
  army retreats; the army moves in; `continents` bookkeeping: if the player (or an AI) now owns every node of the continent it gets `ctw_continent_bonus × nodes` tribute (AI) / a
  continent-victory screen (player); Skill Bonus card (type 14) if the `bonus` script returned > 0; `place_new_armies`; `return_to_node = target`; `check_income`.
- **Player won as defender**: news "failed invasion", `need_defensive_bonus = 1` (reward screen), Skill Bonus card as above.
- **Player lost as defender**: node → the attacker's owner (+ `place_new_armies`), `process_player_armies`, if it was the player's capital → `game_end = 2`.
- **Player lost as attacker**: failed-invasion news, `return_to_node = origin`, the AI defender gets `ctw_def_victory_bonus` 50 tribute.
AI vs AI (`resolve_ai_strategy` @78b360 / `resolve_overrun` @792260): attacker wins if `rand(0..100) < p`, `p = ctw_ai_barb_chance` 95 against barbarians, 98 when the defender has `base_armies == 0`,
`ctw_ai_stronger_chance` 80 if the attacker's `inherent_skill` is higher else 20. An **overrun** (`check_overrun` @791f90) occurs when the attacker's side outnumbers the defender's available armies
around the node by ≥ 2 and the roles satisfy the allies rules; in the player's turn it is offered, in the AI's it is automatic (`resolve_overrun` just transfers the node, with the reward / victory screens for the player; `ctw_overrun_peace_prob` 50 is read by `plan_strategy`; the
`ctw_overrun_easiest..tough_prob` constants (0 / 10 / 20 / 100) are not referenced by any function read here ❓).
`game_end` codes: 1 victory with every remaining nation allied or vassal, 5 victory while some are not, 2 player eliminated / capital lost, 3 time over, 4 armageddon, 6 / 7 nuclear
outcomes (Cold War), 9 ❓ (`leaders[0] field == 10`). Victory check = `check_allied_victory` @795170 (every active nation with armies must be ally/vassal of the player).

## 7. Campaign save (`ConquestSaveGame::save_game` @7a76e0, `ConquestGame::walk_data` @798410)

File in `PlayerProfile::get_ctw_directory`, name `<map_name> - <player nation>` + autosave suffix (`autosave_back0..` rotation by `copy_down_autosaves`), extension `SVX`
(`ConquestGame::save_game` @7953a0: manual slot via the save screen, `param_1 = 1` writes the autosave and rotates). Layout: `String` magic `"RonCTWMapSave"` (u32 length + UTF-16),
u32 version **0x10** (`check_version` accepts > 0xe), then `tag "ConquestWalk"` and the walk (**fully specified in state-walk.md** "ConquestGame::walk_data" / `ConquestLeader`, `ConquestNode`,
`ConquestLink`, `ConquestPiece`, `ConquestNewsItem` sections): raw blocks `[0x4,0x188)` of `ConquestGame` (including `random`), the leaders (raw `[0,0x34b)` + arrays),
nodes (raw `[0,0x44)` + card + strings + links), colonies, continents, barbarian files, the ten script path strings, `map_size_scale`, pieces (per slot: count, capacity, presence bytes,
then two raw ranges per piece), reinforcements, help-pointer bits, news, `valid_tribes`, `overrun_armies`, `continents_captured`, deck names, the style table, `stored_ints/strs`, `diplo_deals`,
campaign tribes (`ObjectArray<Tribe>`), end-game strings, `map_name/id`, `victory_conditions`, sub-goals, `allied_punks`. On load: `init_rules_data` and, if the CtW flag is set,
`game_style = game_styles[style_index1][style_index2]`. A native save can store the same data in any container; **requirements**: RNG state, `style_index*` (or the style id), the
per-node/leader/army state above, the per-link `available_links` is recomputed. The CtW **battle** save/replay header is separate (replay-spec §1, ❓ CTW header).

## 8. Scripts shipped for CtW (330 `.bhs`: Alexander 63, Napoleon 75, New World 65, Cold War 99, shared 28)

- Campaign hooks (script-spec §6.3): `initialization_script` (once: `set_no_attack`, `ctw_set_sub_goals`), `post_turn_script_file` (per round, by `ConquestWin::post_turn_processing` ❓ UI),
  `diplo_response_script_file` / `diplo_offer_script_file` (`conquest/scripts/diplo_response.bhs` 38 KB, `diplo_offer.bhs`), `links_script_file` (extra/removed links; Napoleon, Cold War, New World),
  `strategy_script_file` + `post_ai_script_file` (`run_strategy_script`: AI plans in script), `battle_script_file` (`<campaign>_battles.bhs`, §5.1), `bonus` scripts (`battlebonus.bhs`, `supplybonus.bhs`).
- Per style: `<style>setup.bhs` runs once at battle start, `<style>runtime.bhs` every frame (e.g. `conquestsetup/conquestruntime`, `defsetup0/defruntime0/1`, `offsetup0/1/5`, `meleesetup`, `nomadruntime`,
  `pillage*`, `dipruntime`, `liberation*`, `battlesetup/runtime`). Shared prelude: `include "ctw_lib.bhs"`; `conquestsetup.bhs` branches on `get_ctw_campaign()` and creates campaign leaders (Darius,
  Chandragupta Maurya, Djezzar...) with `create_unit` / `create_unit_upgrade`.
- 165 `.scx/.sce` pairs back the CUSTOM styles (Granicus, Tyre ×3, Jhelum, Jena, Austerlitz, Lodi, Inchon, ...). `include "game_structs.bhs"`. Builtins used by CTW scripts (427 distinct, 199 CTW-only):
  `get_turn, get_age, get_difficulty, get_ctw_campaign, get_ctw_int/set_ctw_int, get_country_owner, get_territory_owner, set_no_attack, ctw_get_active_tribes, ctw_add_objective, ctw_set_sub_goals,
  popup_choice` (script-spec §7). Their implementations read/write `ConquestGame` as in §2; `sinf/cosf` army placement needs CRT-exact floats (script-spec §7.6).

## 9. What the port needs

- **Module** `engine/ctw` (new, no sim dependency except `GameInfo` fill): `CampaignDef` loader (§1), `RulesDef` loader, `CampaignState` (§2.1) + `Rng` (own stream, same algorithm as `Random`),
  `turn::end_player_turn / ai_round / advance_round`, `diplomacy`, `cards`, `style_picker` (needs the BHS VM for `battle_script`/links/strategy scripts: reuse `script_host`), `battle_setup::fill_game_info`,
  `outcome::apply`, `save::{read,write}` (state-walk layout or a native one). The sim side: `Setup::build_game` CtW branches (§5.3), `MapConquest`, `build_cities/build_units`, reinforcement spawns,
  hero rules, the tactical-card hooks (§4) in research / market / heal / attrition.
- **UI** (ui-spec): campaign picker + difficulty + nation, world map with picked countries and links, army pieces and drag, invade window, diplomacy window, card windows (hand, shop, wonder, targets), news,
  reward / victory / final windows, objective box. The art is the user's own install.
- **Acceptance criteria without the original**: (1) all five campaign XMLs load; counts: countries 26/47/51/32/59, links 106/182/232/128/218, styles 44/60/52/55/0, colonies 0/0/14/0/0, every `owner` resolves to
  a `<TRIBE><KEY>`, every `<LINK to>` resolves; (2) `Random`-free parts deterministic: with a fixed seed `setup_conquest_game` yields the same nodes/leaders/decks in two runs and across a save round trip
  (`RonCTWMapSave` v0x10 byte-for-byte with state-walk.md if the original layout is kept); (3) cost functions of §3 reproduce the `CTW_*` table at the default attitudes (e.g. alliance with 2 allies = 20+att+200);
  (4) `pick_style` is a pure function of (seed, from, to, cur_age, levels, supporters) and returns the same style on every call; (5) round length formula, level/age progression and income rules unit-tested on
  Cold War (14 rounds, `territory_income`) vs Alexander (19 rounds); (6) outcome table of §6 for all six (kind × role) cases; (7) a CtW battle built from a link has: players = additional opponents[diff] + supporters,
  `starting_technology = cur_age`, defender build-out of the node level, resource ×bonus, reinforcement times within 2..4 min steps; (8) the world is replayable: battle replay with the existing `.rcx` oracle.

## 10. Evidence

| Function | Address | Use |
|---|---|---|
| `ConquestGame::setup_conquest_game` | 7928c0 | XML → nodes / leaders / deck, random assignment, init script |
| `ConquestCountries::load_country_data` | 75d630 | `COUNTRY`/`LINK`/`CONTINENT` attributes and `random` rules |
| `ConquestGame::init_rules_data` | 78d460 | `conquestrules.xml` loader (cards, styles, ARMIES, BUILDINGS_*, RESOURCEBONUS_*) |
| `ConquestStyle::init`, `ConquestStyles::pick_style` | 7a7ce0, 7a95f0 | style attributes, selection (§5.1) |
| `ConquestGame::setup_scenario` / `_rules` / `_bonuses` / `_rares` / `_diplo` | 7999e0, 78cc80, 78be90, 78ca60, 78bde0 | GameInfo fill, tactical cards, rares |
| `ConquestGame::build_conquest_game`, `build_cities`, `build_units`, `setup_reinforcements` | 797c40, 796140, 797200, 789d70 | battle world |
| `ConquestGame::process_results`, `resolve_ai_strategy`, `resolve_overrun`, `check_overrun` | 798940, 78b360, 792260, 791f90 | outcomes |
| `ConquestGame::return_from_scen`, `advance_round`, `end_turn`, `check_income`, `assign_bonuses` | 78f040, 790220, 792700, 78edb0, 78b140 | round loop |
| `ConquestGame::find_available_links`, `process_link`, `check_difficult_attack` | 794d80, 78ad20, 78a860 | legal attacks |
| `ConquestGame::play_strategic_card`, `ConquestBonusCard::*` | 78f3c0, 788xxx | cards |
| `ConquestLeader::get_*_cost`, `buy_*`, `set_*`, `demand_tribute`, `defeat`, `place_new_armies`, `plan_strategy` | 79c150.., 79bab0.., 7a35d0.., 7a2460, 79c460, 7a1f10, 79ea80 | diplomacy, armies, AI |
| `ConquestNode::transfer_to`, `can_drop_army` | 7a6a90, 7a6540 | ownership and moves |
| `ConquestGame::walk_data`, `ConquestSaveGame::save_game`, `ConquestLoadGame::check_version` | 798410, 7a76e0, 7a7a00 | save |
| `Setup::build_game` CtW branches, `Map::place_resources`, `Leader::calc_attrition` | 5ac190, 68f4f0, 6cdea0 | CtW flag consumers |
| `MapConquest::add_additional_players` / `check_starting_loc` | 68d260, 68d7e0 | battle map starts |

## 11. Unknowns (❓)

AI scoring in `plan_strategy` / `run_strategy_script` / `find_best_army_loc`; AI diplomacy decisions in `do_diplomacy`/`propose_*`; `ConquestDiploOffer` internals and the `.bhs` bodies; where the CtW flag is set and
the campaign list is built (`ConquestSetupWin`/`ConquestWin`, not decompiled); `VALID_NATION` and `priority_target` effects; `common_res*`; CtW replay header; `game_end = 9`; the sim-side readers of cards 7 8 9 12 32 (rate
constants); Cold War nukes/defcon/espionage (`shall_we_play_a_game`, `can_buy_nuke`); the `distance`/`battle_cat` link fields.
