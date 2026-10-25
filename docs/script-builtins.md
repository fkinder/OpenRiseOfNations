# Script builtins: what the shipped scripts call, and what still has no body

Generated 2026-10-24 for the `script` round (docs/sim-implementation.md "Scripts"). Method: every one of the 365 `.bhs` files of the install is compiled by `ron-script` (the compiler matches `re/tools/bhs-parse.mjs` on all 365; `trigger_template.bhs` is an editor fragment and is not a main file, so 364 compilations), every image of each compilation (the main file and its includes) is disassembled and each `CALL_GAME` operand is counted. The builtins table (`engine/script/bhs_builtin_table.inc`, 873 registrations of 813 names) is then crossed with the natives `install_script_funcs` registers: a registration without a native is *logged-only* (the VM logs `unported builtin: NAME` once and returns the type's zero value, `ScriptHost::unported()`; `ron-sim` prints `unported builtins called:`). A one-line check of any script: `build/ron-script --disasm <game dir> conquest/ColdWar/countermissileruntime.bhs | grep CALL_GAME`.

Counts are call sites in the compiled images of each main file (a library included by several scenarios counts once per scenario that includes it, a call site inside a `for` loop counts once).

## Numbers

| | before | after |
|---|---|---|
| registrations without a native (of 873) | 247 | 198 |
| names without a native (of 813) | 231 | 183 |
| registrations the shipped scripts call (of those) | 46 | 0 |
| call sites of those in the 364 compilations | 274 | 0 |
| distinct registrations called by any shipped script | 565 | 565 |

The previous round's figure ("341 names have no body, 137 used by the shipped scripts") was stale: the CTW strategic layer (`script_funcs_ctw.cpp`), the order and spell builtins had been ported since. All 46 registrations that were still logged-only and called are all in the Conquer the World battle style scripts (`conquest/**`); the scenario, tutorial, Chess Exercise and AI scripts call only ported builtins.

## Ported this round (all called by the shipped scripts; most calls first)

| builtin | calls | scripts | original | where it is called (calls) |
|---|---|---|---|---|
| `special_forces_infiltrate` | 57 | 1 | @9f8ec0 | ColdWar/countermissileruntime 57 |
| `general_ambush` | 31 | 12 | @9f8b80 | scripts/offruntime1 15, Alexander/hideawayruntime 2, ColdWar/perubasesruntime 2, Napoleon/guerillabasesruntime 2, ... |
| `ctw_change_objective_text` | 24 | 13 | @a01a60 | Alexander/thrace 4, Napoleon/montenotteruntime 4, Alexander/capturedarius 2, ColdWar/guerillaactionruntime 2, ... |
| `set_unit_craft` | 23 | 3 | @9f68a0 | ColdWar/countermissileruntime 21, ColdWar/bribesetup 1, ColdWar/sovietfinalsetup 1 |
| `ctw_remove_objective` | 19 | 12 | @a01b20 | ColdWar/americafinalruntime 3, ColdWar/countermissileruntime 3, Alexander/sogdiana 2, Napoleon/belgiumruntime 2, ... |
| `special_forces_sniper` | 15 | 1 | @9f8fc0 | Napoleon/corsicaruntime 15 |
| `set_unit_type_max_craft` | 9 | 9 | @9f6980 | Alexander/hideawaysetup 1, ColdWar/bribesetup 1, ColdWar/countermissilesetup 1, ColdWar/perubasessetup 1, ... |
| `get_tileset` | 6 | 2 | @9e4d20 | Alexander/hideawaysetup 4, Alexander/hideawayruntime 2 |
| `special_forces_sabotage` | 6 | 2 | @9f8f40 | Napoleon/corsicaruntime 4, Napoleon/montenotte4runtime 2 |
| `defeat_attacker_and_allies` | 5 | 5 | @9e5f60 | scripts/conquestruntime 1, scripts/dipruntime 1, scripts/liberationruntime 1, scripts/offruntime1 1, ... |
| `lose_current_age` | 5 | 3 | @9ee6a0 | scripts/offsetup5 3, Napoleon/transylvaniasetup 1, NewWorld/nativerallysetup 1 |
| `general_is_ambushing` | 5 | 5 | @9f8ce0 | Alexander/hideawayruntime 1, ColdWar/perubasesruntime 1, Napoleon/guerillabasesruntime 1, Napoleon/madridruntime 1, ... |
| `have_trade` | 5 | 5 | @9fcea0 | Alexander/jhelum 1, Alexander/yemen 1, ColdWar/centralruntime 1, NewWorld/caribruntime 1, ... |
| `num_civilian_buildings` | 4 | 2 | @9e9ce0 | scripts/pillageruntime 3, Napoleon/pyramidsruntime 1 |
| `object_type` | 4 | 1 | @9f1580 | NewWorld/nativerallyruntime 4 |
| `train_unit` | 4 | 1 | @9f4030 | Napoleon/wagramruntime 4 |
| `set_plunder_scale` | 3 | 3 | @9e5450 | Alexander/pillagesetup 1, NewWorld/andessetup 1, scripts/pillagesetup 1 |
| `find_build_with_type_name` | 3 | 1 | @9ec310 | ColdWar/militia 3 |
| `find_civilian_build` | 3 | 2 | @9ec630 | Napoleon/pyramidsruntime 2, scripts/pillageruntime 1 |
| `set_rally` | 3 | 2 | @9f2020 | ColdWar/guerillaactionruntime 2, ColdWar/missilestrikeruntime 1 |
| `craft_percent` | 3 | 2 | @9f90c0 | Napoleon/montenotte4runtime 2, scripts/offruntime1 1 |
| `get_commerce_cap` | 3 | 3 | @9fbcc0 | Alexander/magadha 1, Alexander/maurya 1, Alexander/mauryamagadha 1 |
| `enable_city_defeat` | 3 | 1 | @9ffc40 | Napoleon/leipzig 3 |
| `unit_obey_orders` | 3 | 2 | @a017d0 | Alexander/capturedarius 2, Napoleon/wagramruntime 1 |
| `lose_upgrade` | 2 | 1 | @9eed00 | ColdWar/centralruntime 2 |
| `get_city_name` | 2 | 2 | @9efd00 | ColdWar/bayofpigsruntime 1, ColdWar/southafricaruntime 1 |
| `rename_city` | 2 | 2 | @9effc0, @9f00d0 | NewWorld/andessetup 1, NewWorld/plainssetup 1 |
| `is_object_at` | 2 | 2 | @9f0b70 | Alexander/thrace 1, Alexander/thrace2 1 |
| `unit_patrol_order_queue` | 2 | 1 | @9f81d0 | ColdWar/missilestrikeruntime 2 |
| `offer_alliance` | 2 | 1 | @9fc270 | Alexander/greece2 2 |
| `switch_player_control` | 1 | 1 | @9e5960 | Napoleon/napoleonopening 1 |
| `enable_tech` | 1 | 1 | @9ea590 | Alexander/capturedarius 1 |
| `select_object` | 1 | 1 | @9f0760 | Napoleon/pyramidsruntime 1 |
| `building_active` | 1 | 1 | @9f1ea0 | scripts/offruntime5 1 |
| `kill_unit_type_anim` | 1 | 1 | @9f6ea0 | ColdWar/guerillaactionsetup 1 |
| `general_decoy` | 1 | 1 | @9f8b00 | scripts/battleruntime 1 |
| `special_forces_counterintel` | 1 | 1 | @9f9040 | ColdWar/infiltrate 1 |
| `is_gathering` | 1 | 1 | @9f9420 | Napoleon/turkeyruntime 1 |
| `set_contact` | 1 | 1 | @9fcde0 | NewWorld/settlerssetup 1 |
| `ui_hide_score` | 1 | 1 | @9ff4c0 | Napoleon/napoleonopening 1 |
| `scout_counter_intel` | 1 | 1 | @a00170 | NewWorld/stealtechruntime 1 |
| `num_rare_resources` | 1 | 1 | @a004b0 | NewWorld/mercantileruntime 1 |
| `find_resource` | 1 | 1 | @a015d0 | NewWorld/mercantileruntime 1 |
| `resource_position_x` | 1 | 1 | @a016b0 | NewWorld/mercantileruntime 1 |
| `resource_position_y` | 1 | 1 | @a01700 | NewWorld/mercantileruntime 1 |
| `object_ignore_orders` | 1 | 1 | @a01850 | ColdWar/airliftsetup 1 |

## Per script (the builtins above, calls in each script that has them)

| script | total | calls |
|---|---|---|
| `conquest/ColdWar/countermissileruntime.bhs` | 81 | `special_forces_infiltrate` 57, `set_unit_craft` 21, `ctw_remove_objective` 3 |
| `conquest/Napoleon/corsicaruntime.bhs` | 19 | `special_forces_sniper` 15, `special_forces_sabotage` 4 |
| `conquest/scripts/offruntime1.bhs` | 17 | `general_ambush` 15, `craft_percent` 1, `defeat_attacker_and_allies` 1 |
| `conquest/Alexander/hideawaysetup.bhs` | 6 | `get_tileset` 4, `general_ambush` 1, `set_unit_type_max_craft` 1 |
| `conquest/Napoleon/montenotteruntime.bhs` | 6 | `ctw_change_objective_text` 4, `ctw_remove_objective` 2 |
| `conquest/Alexander/capturedarius.bhs` | 5 | `ctw_change_objective_text` 2, `unit_obey_orders` 2, `enable_tech` 1 |
| `conquest/Alexander/hideawayruntime.bhs` | 5 | `general_ambush` 2, `get_tileset` 2, `general_is_ambushing` 1 |
| `conquest/Alexander/thrace.bhs` | 5 | `ctw_change_objective_text` 4, `is_object_at` 1 |
| `conquest/ColdWar/perubasesruntime.bhs` | 5 | `ctw_change_objective_text` 2, `general_ambush` 2, `general_is_ambushing` 1 |
| `conquest/Napoleon/guerillabasesruntime.bhs` | 5 | `ctw_change_objective_text` 2, `general_ambush` 2, `general_is_ambushing` 1 |
| `conquest/Napoleon/pyramidsruntime.bhs` | 5 | `find_civilian_build` 2, `ctw_remove_objective` 1, `num_civilian_buildings` 1, `select_object` 1 |
| `conquest/Napoleon/wagramruntime.bhs` | 5 | `train_unit` 4, `unit_obey_orders` 1 |
| `conquest/ColdWar/guerillaactionruntime.bhs` | 4 | `ctw_change_objective_text` 2, `set_rally` 2 |
| `conquest/Napoleon/montenotte4runtime.bhs` | 4 | `craft_percent` 2, `special_forces_sabotage` 2 |
| `conquest/NewWorld/mercantileruntime.bhs` | 4 | `find_resource` 1, `num_rare_resources` 1, `resource_position_x` 1, `resource_position_y` 1 |
| `conquest/NewWorld/nativerallyruntime.bhs` | 4 | `object_type` 4 |
| `conquest/scripts/pillageruntime.bhs` | 4 | `num_civilian_buildings` 3, `find_civilian_build` 1 |
| `conquest/Alexander/thrace2.bhs` | 3 | `ctw_change_objective_text` 1, `ctw_remove_objective` 1, `is_object_at` 1 |
| `conquest/ColdWar/americafinalruntime.bhs` | 3 | `ctw_remove_objective` 3 |
| `conquest/ColdWar/centralruntime.bhs` | 3 | `lose_upgrade` 2, `have_trade` 1 |
| `conquest/ColdWar/militia.bhs` | 3 | `find_build_with_type_name` 3 |
| `conquest/ColdWar/missilestrikeruntime.bhs` | 3 | `unit_patrol_order_queue` 2, `set_rally` 1 |
| `conquest/Napoleon/leipzig.bhs` | 3 | `enable_city_defeat` 3 |
| `conquest/Napoleon/madridruntime.bhs` | 3 | `general_ambush` 2, `general_is_ambushing` 1 |
| `conquest/Napoleon/portugalruntime.bhs` | 3 | `general_ambush` 2, `general_is_ambushing` 1 |
| `conquest/scripts/offsetup5.bhs` | 3 | `lose_current_age` 3 |
| `conquest/Alexander/greece2.bhs` | 2 | `offer_alliance` 2 |
| `conquest/Alexander/sogdiana.bhs` | 2 | `ctw_remove_objective` 2 |
| `conquest/ColdWar/bribesetup.bhs` | 2 | `set_unit_craft` 1, `set_unit_type_max_craft` 1 |
| `conquest/ColdWar/perubasessetup.bhs` | 2 | `general_ambush` 1, `set_unit_type_max_craft` 1 |
| `conquest/ColdWar/sovietfinalsetup.bhs` | 2 | `set_unit_craft` 1, `set_unit_type_max_craft` 1 |
| `conquest/Napoleon/belgiumruntime.bhs` | 2 | `ctw_remove_objective` 2 |
| `conquest/Napoleon/friedlandruntime.bhs` | 2 | `ctw_change_objective_text` 2 |
| `conquest/Napoleon/guerillabasessetup.bhs` | 2 | `general_ambush` 1, `set_unit_type_max_craft` 1 |
| `conquest/Napoleon/madridsetup.bhs` | 2 | `general_ambush` 1, `set_unit_type_max_craft` 1 |
| `conquest/Napoleon/napoleonopening.bhs` | 2 | `switch_player_control` 1, `ui_hide_score` 1 |
| `conquest/Napoleon/portugalsetup.bhs` | 2 | `general_ambush` 1, `set_unit_type_max_craft` 1 |
| `conquest/NewWorld/andessetup.bhs` | 2 | `rename_city` 1, `set_plunder_scale` 1 |
| `conquest/scripts/battleruntime.bhs` | 2 | `general_ambush` 1, `general_decoy` 1 |
| `conquest/scripts/offruntime5.bhs` | 2 | `building_active` 1, `defeat_attacker_and_allies` 1 |
| `conquest/Alexander/darius2runtime.bhs` | 1 | `ctw_remove_objective` 1 |
| `conquest/Alexander/dariusruntime.bhs` | 1 | `ctw_remove_objective` 1 |
| `conquest/Alexander/jhelum.bhs` | 1 | `have_trade` 1 |
| `conquest/Alexander/magadha.bhs` | 1 | `get_commerce_cap` 1 |
| `conquest/Alexander/maurya.bhs` | 1 | `get_commerce_cap` 1 |
| `conquest/Alexander/mauryamagadha.bhs` | 1 | `get_commerce_cap` 1 |
| `conquest/Alexander/persiaruntime.bhs` | 1 | `ctw_remove_objective` 1 |
| `conquest/Alexander/pillagesetup.bhs` | 1 | `set_plunder_scale` 1 |
| `conquest/Alexander/yemen.bhs` | 1 | `have_trade` 1 |
| `conquest/ColdWar/afghanruntime.bhs` | 1 | `ctw_change_objective_text` 1 |
| `conquest/ColdWar/airliftsetup.bhs` | 1 | `object_ignore_orders` 1 |
| `conquest/ColdWar/australiaruntime.bhs` | 1 | `ctw_change_objective_text` 1 |
| `conquest/ColdWar/bayofpigsruntime.bhs` | 1 | `get_city_name` 1 |
| `conquest/ColdWar/countermissilesetup.bhs` | 1 | `set_unit_type_max_craft` 1 |
| `conquest/ColdWar/guerillaactionsetup.bhs` | 1 | `kill_unit_type_anim` 1 |
| `conquest/ColdWar/indiaruntime.bhs` | 1 | `ctw_change_objective_text` 1 |
| `conquest/ColdWar/infiltrate.bhs` | 1 | `special_forces_counterintel` 1 |
| `conquest/ColdWar/mexicoruntime.bhs` | 1 | `ctw_change_objective_text` 1 |
| `conquest/ColdWar/moscowruntime.bhs` | 1 | `ctw_change_objective_text` 1 |
| `conquest/ColdWar/seekdestroysetup.bhs` | 1 | `set_unit_type_max_craft` 1 |
| `conquest/ColdWar/southafricaruntime.bhs` | 1 | `get_city_name` 1 |
| `conquest/Napoleon/eylauruntime.bhs` | 1 | `ctw_remove_objective` 1 |
| `conquest/Napoleon/russiaruntime.bhs` | 1 | `ctw_remove_objective` 1 |
| `conquest/Napoleon/transylvaniasetup.bhs` | 1 | `lose_current_age` 1 |
| `conquest/Napoleon/turkeyruntime.bhs` | 1 | `is_gathering` 1 |
| `conquest/NewWorld/caribruntime.bhs` | 1 | `have_trade` 1 |
| `conquest/NewWorld/nativerallysetup.bhs` | 1 | `lose_current_age` 1 |
| `conquest/NewWorld/plainssetup.bhs` | 1 | `rename_city` 1 |
| `conquest/NewWorld/settlersruntime.bhs` | 1 | `have_trade` 1 |
| `conquest/NewWorld/settlerssetup.bhs` | 1 | `set_contact` 1 |
| `conquest/NewWorld/stealtechruntime.bhs` | 1 | `scout_counter_intel` 1 |
| `conquest/scripts/conquestruntime.bhs` | 1 | `defeat_attacker_and_allies` 1 |
| `conquest/scripts/dipruntime.bhs` | 1 | `defeat_attacker_and_allies` 1 |
| `conquest/scripts/liberationruntime.bhs` | 1 | `defeat_attacker_and_allies` 1 |
| `conquest/scripts/pillagesetup.bhs` | 1 | `set_plunder_scale` 1 |

## Bodies that were stubs or TODO and are real now

| builtin | before | now |
|---|---|---|
| `get_mapstyle` @9e4cc0 | always `""` (the AI production scripts' `sea_map` was always 0) | `map_styles[GameInfo.map_style].key`; moves the AI of `conquest/NewWorld/carib.scx` and `carib2.scx` (digests below) |
| `group_guard_order` @9fb4b0 | attack-moved to the guarded object, counted in `todo_calls` | `Group::action_guard` (5 + 6 + 6 + 1 + 1 calls in the style sweep) |
| `unit_eject_order` @9f84e0 | logged `todo_calls` | `unit_action_come_out` (the COME_OUT command's effect applied directly) |
| `create_unit` (sea unit on land) | logged `todo_calls` | `Group::action_transport(1)` |
| `unit_ignore_orders`, `object_ignore_orders`, `unit_obey_orders` | list filled, never consulted | the `ScenarioData::ignore_orders` prelude kills the listed objects from the group in the nine group actions of `aircraft_cmd.cpp` / `combat_orders.cpp`; the 24 order builtins zero `ignore_orders` around their call as the original does. The remaining sites (`groups.cpp` x5, `transport.cpp` x3, `garrison.cpp` x2, `construct.cpp` x3, `econ_gather.cpp` x1) are other owners' files and still `TODO(scenarios)` |
| `get_starting_loc_x/y`, `get_territory_strength`, `general_forced_march`, `unit_stop_effect`, `add_visibility`, reveal points, `set_explored`, `set_diplomacy` consumers | listed as open in older notes | already ported by the Conquer the World, spell and AI rounds (`script_funcs_ctw.cpp`, `script_funcs_order.cpp`, `script_funcs_mutate.cpp`, `ai_diplo.cpp`); nothing left to do |

## Results of the 3000-frame runs (2026-10-24)

`re/tools/sim-all.sh`-style run of every stock scenario and conquest map (156 `.scx`), 3000 frames, scripts on, `--ai-every 8`, determinism self-check on, before and after: self-check 156/156 both; **no scenario hits a logged-only builtin or a `todo_calls` entry** (before: none either on these maps, because the conquest battle scripts do not run on a plain `.scx`). The 600-frame digests (`sim-all 600 --ai-every 8`) are identical to the tree before the change on all 156. At 3000 frames two digests move, both for one reason: `conquest/NewWorld/carib.scx` and `carib2.scx` (map style of a sea map) now give the AI production scripts a real `get_mapstyle()` (`sea_map = 1`); a build with `get_mapstyle` forced back to `""` gives the old digests `afae73e2a2bf120e` / `d0bb1f7d41a8a551` again.

The battle-style scripts do run in the Conquer the World layer, so a second sweep plays **every style of the five campaigns** (229 styles, 1500 frames each, `ConquestGame::build_battle` on the first available link, the style forced): before, 63 style runs hit one of the 46 logged-only builtins or a `todo_calls` stub (`general_ambush` x792 in Battle of Lisbon, `craft_percent` x10099 in The Ruins of Montenotte, `set_unit_type_max_craft`, `set_plunder_scale`, `train_unit`, `lose_current_age`, `ctw_remove_objective`, `unit_obey_orders`, `object_ignore_orders`, `set_contact`, `group_guard_order` x5-6, `create_unit: Group::action_transport` x6-8 ...); after, **0 of 229**, script failures 2 (`eurorallysetup` / `aztecrallysetup` run out of instructions on the sweep's link, identical before). Styles `Hideaway`, `Tactics`, `Low Rations` end at frame 0 on that link (the generic link has no generals to eliminate; identical before), so their scripts are not exercised by that sweep (a stand-alone check of the builtins on Tutorial 1 with a throw-away script returned the expected values).

## Builtins that no shipped script calls and that still have no body (183 names)

`add_arrow_point`, `add_infiltrated`, `air_base_recall_order`, `air_unit_return_order`, `any_object_at`, `at_least_age`, `at_least_buildings`, `at_least_cities`, `at_least_civilian_buildings`, `at_least_civilian_units`, `at_least_gather_rate`, `at_least_military_buildings`, `at_least_military_units`, `at_least_points`, `at_least_pop`, `at_least_settlements`, `at_least_unit_category`, `at_least_villages`, `bubble_text_show`, `build_placement_warning`, `build_queue_percent_done`, `build_wonder_menu_active`, `buy_good`, `can_buy_sell`, `cancel_arrow`, `change_to_ai`, `city_cap_warning`, `city_exists`, `clear_extra_starting_locs`, `clear_msg`, `create_missile_and_target_loc`, `ctw_clear_objectives`, `ctw_play_objective_sound`, `ctw_set_objective_loc`, `ctw_set_objective_sound`, `demand_tribute`, `diplomacy_enable_war`, `diplomacy_set_accept`, `disable_unit_transport`, `draw_arrow_curve`, `enable_city_ai`, `enable_unit_transport`, `field_battle_disable`, `file_entry_exists`, `file_exists`, `file_get_category`, `file_read_text`, `file_remove`, `file_traverse_entries`, `file_up_category`, `file_write_text`, `find_buy_price`, `find_counter_unit`, `find_extra_starting_loc`, `find_idle_all_military`, `find_idle_caravan`, `find_idle_inside_military`, `find_inside_military`, `find_inside_military_category`, `find_target_id`, `find_target_who`, `garrisoned_in`, `get_attack_age`, `get_attack_turn`, `get_bonus_cap`, `get_button_rect`, `get_city_display_name`, `get_city_level`, `get_console_player`, `get_curr_filename`, `get_current_age`, `get_extra_starting_loc_x`, `get_extra_starting_loc_y`, `get_last_building_built`, `get_leader_difficulty`, `get_mouse_game_x`, `get_mouse_game_y`, `get_mouse_screen_x`, `get_mouse_screen_y`, `get_neutral_attrition`, `get_num_peace`, `get_num_war`, `get_percent_explored`, `get_plunder_scale`, `get_wonder_points`, `give_bonus_card`, `group_explore_order`, `group_id`, `group_patrol_order_queue`, `group_waypoint_order`, `has_follow_order`, `has_rare_resource`, `has_repeat_orders`, `home_base`, `involved_id`, `involved_nation`, `is_city`, `is_civilian_building`, `is_human`, `is_military_building`, `is_mouseover_forest`, `is_mouseover_mountain`, `is_mouseover_oil`, `is_mouseover_river`, `is_paused`, `is_right_click_action`, `is_victory_conquest`, `is_victory_standard`, `is_victory_sudden_death`, `is_victory_time_limit`, `is_zoomed_in`, `less_than_age`, `less_than_buildings`, `less_than_civilian_buildings`, `less_than_civilian_units`, `less_than_gather_rate`, `less_than_military_buildings`, `less_than_military_units`, `less_than_points`, `less_than_pop`, `less_than_settlements`, `less_than_type`, `less_than_unit_category`, `load_production_script`, `max_caravan_routes`, `max_military_unit_type`, `militia_to_citizen`, `mouse_down`, `num_ai_players`, `num_civilian_units`, `num_human_players`, `num_in_group`, `num_involved`, `object_obey_orders`, `object_type_at`, `offer_accepted`, `offer_mutual_attack`, `offer_rejected`, `offer_tribute`, `pause_enable`, `plunder_disable`, `plunder_enable`, `remove_visibility`, `repeat_orders_disable`, `repeat_orders_enable`, `researching_tech_at`, `researching_warning`, `resource_selected`, `revoke_offer`, `scrolling_lock_manual`, `scrolling_lock_minimap`, `scrolling_lock_shortcuts`, `scrolling_unlock_manual`, `scrolling_unlock_shortcuts`, `select_group`, `sell_good`, `set_arrow_color`, `set_arrow_thick`, `set_game_msg_color`, `set_game_msg_time`, `set_msg_align`, `set_msg_color`, `set_msg_time`, `set_object_type_min_range`, `speed_control_enable`, `spy_bribe`, `spy_pilfer`, `start_ping_arrow`, `start_screen_arrow`, `switch_unit_type`, `tech_warning`, `ui_show_score`, `unit_explore_order`, `unit_group_near`, `unit_group_near_build`, `was_attacked`, `was_capital_attacked`, `was_raided`, `zoom_out_on`, `zooming_lock_manual`, `zooming_lock_shortcuts`, `zooming_unlock_manual`, `zooming_unlock_shortcuts`

