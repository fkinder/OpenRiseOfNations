# UI port: what ronmap's interface implements and how

Companion to [ui-spec.md](ui-spec.md) (the decoded original; port plan in its section 8). This file records what the
port does, the decisions that differ from or go beyond the spec, and what is left. Status: U0 done; U1 steps 8–11, 13
and most of 12 and 14 done (2026-10-12); the scripts' interface and audio (sections 8-9, 2026-10-13); the front end
in the game window (section 10, 2026-10-13); the HUD leftovers (tooltips, simulation messages and timers, market,
infrastructure and mini library, minimap layers, streamlined mode, key map; section 11, 2026-10-24); the second HUD pass
(simulation events replace the view heuristics, library grid, market card, garrison grid, rare-goods column, progress buttons,
formation drag, game speed, control groups through a command record, localised strings; section 12, 2026-10-24).
Third pass (air garrison panel, library final techs and category column, group-by-type selection and the selection rules, stances,
formations, patrol, guard, queue / infinite hotkeys, the box filters, the compute_form preview; section 13, 2026-10-24).
Fifth pass (carrier REPEAT and AUTO_EXPLORE, the streamlined air / garrison panels and the wider streamlined slot, the Options page and the preferences
file; section 15, 2026-10-24; section 14 is the army groups).
Sixth pass (the victory messages and sounds, the musical-chairs and flashing time-limit clocks, the end-game window with its score table, the score window and the script camera events; section 17, 2026-10-25).

## 1. Pieces

| Where | What |
|---|---|
| `engine/assets/iface_layout.*` | interface.xml (any GraphicWinXML file): element tree, `resolve_iface_rect` (= `get_attrib_rect_ex`, ui-spec 2.3: anchors, `_inverse`, `_percent`, `center_off`, negative sizes, 16-bit truncation), POSITIONING groups flattened (`iface_layout_children`), IFACETEX pieces, 16-slot IFACEBORDERs, IFACECOLOR, IFACETEXLOAD ids; interface.txt (`InterfaceTxt`: the two bitmap digit sets and any `KEY, TEX, x, y, w, h` line); the per-civ skin code (`iface_skin_code`, ui-spec 1.1) and file swap |
| `engine/assets/strings.*` | internal / translated / string_lookup / typenames tables, `string_hash` (String::generate_hash), `lookup(text, category)`, language suffixes (.7 German, .10 Spanish, .12 French, .16 Italian) |
| `engine/assets/fonts.*` | fonts.xml (CODEPAGE "default", RESOLUTION closest to the screen width, FONT list, LOOKUP tags), `FontsXml::pick` = FontFamily::get(size, style) (exact, then bold-only difference, then nearest size), TrueType metrics as FreeType derives them (LineHeight, Ascent, Descent, FontHeight, hinted integer advances) |
| `engine/assets/cursors.*` | cursors.xml rows (strip cells for negative x / y) |
| `engine/assets/catalog.*` | unit_graphics.xml icon fields (`tex_id`, `tex_col`, `tex_row`, `tex_x`, `tex_y`) |
| `apps/ui-check` → `ron-ui-check` | `tree` / `texloads` print exactly what `re/tools/ifacelayout.mjs` prints; `fonts`, `strings`, and `check` (every window at 1024×768 and 1920×1080, every atlas in all 15 skins, digits, fonts, tables, cursors; ends with `failures: N`) |
| `platform/metal/ui.*` | `UiRenderer`: IFaceRenderManager's API (`add` = add_smooth, `add_sub`, `tile`, `border` = draw_border, `expander` / `vexpander`, `fill`, `frame`, `write_digits` = write), the 9 IFaceRenderType states plus text and a multiply state, `UiFont` glyph atlases, one encode into the post-tonemap overlay pass |
| `apps/map-mac/hud.*` | the HUD panels and their hit tests / clicks (reads a `hud::State`, never the simulation) |
| `apps/map-mac/main.mm` | fills `hud::State` from the simulation views, runs the card's commands, building placement, selection rules, input routing, control groups, edge scroll, zoom, cursors, the minimap background; fills `sim::ScriptInput` and applies the scripts' camera moves, locks and pauses (section 8) |
| `apps/map-mac/script_view.*` | the scenario scripts' presentation (section 8): consumes `sim::script_presentation`, keeps and draws the MessageWin / AnimNotice / ping / popup state |
| `platform/audio/audio.*` | AVFoundation sound layer (section 9): the scenario voice channel, effects, the JukeBox |
| `apps/map-mac/hud_extra.mm` | market and infrastructure sections, mini library, timers, scroll plate, the tooltip target (`Hud::hover_at`) |
| `apps/map-mac/hud_panels.mm` | the second pass's panels (section 12): rare-goods column, progress buttons, library grid, garrison grid, the Market card's buy / sell buttons |
| `apps/map-mac/hotkeys.*` | control groups as the HOTKEY command record (verb 34, section 12.5) |
| `apps/map-mac/uix.*`, `hud_air.mm` | the third pass (section 13): the air garrison panel, the library's final techs and category column, the aircraft / airbase / carrier / silo card and orders, stances, formations, patrol, guard, disband, queue, the key-map actions that need them, the selection rules (object buttons, select-all-of-types, damaged / on-screen filters, box filters), `Group::compute_form` for the drag preview |
| `apps/map-mac/options_ui.*`, `prefs.*` | the Options page and the preferences file (section 15): optionswin.xml's frame, tabs, areas, sliders, check boxes, the Hotkeys pane over `keymap.*`; `preferences.txt` under Application Support |
| `apps/map-mac/uistr.*` | the interface language (`--lang`, `RON_LANG`, the profile) and `tr()` for UI literals (section 12.6) |
| `apps/map-mac/tips.*` | help.xml reader, the Buffer slot markup and the mouse-over box (section 11.1) |
| `apps/map-mac/keymap.*`, `keymap_table.inc` | the default key map and its lookup (section 11.4); the table is generated by `re/tools/keymap-inc.mjs` |
| `apps/map-mac/notices.*` | messages, banners and pings the simulation causes (section 11.2) |
| `apps/map-mac/endgame.*` | the end-game window (`EndGameWin`) and the post-game score table (section 17.3); `notices.mm` also carries the victory messages (17.1) |
| `apps/map-mac/frontend.*`, `launcher.*` | the front end in the game window (section 10): Main Menu, map lists, AI difficulty, Quit popup; game-folder detection and the first-run picker |

Check: `./build/ron-ui-check ~/Games/RiseOfNations check` → `failures: 0`; `ron-ui-check tree --all --w W --h H --file F`
is byte-identical to `node re/tools/ifacelayout.mjs tree --all ...` for interface.xml, topmenu.xml, setupwin.xml and
optionswin.xml at 800×600, 1024×768, 1280×800, 1920×1080 and 2560×1440, except where the JS tool prints raw XML entities
(`Tools &amp; Extras`): the C++ reader decodes them, as MSXML does. `ron-ui-check fonts --w 1024` matches
`fontpreview.mjs list` line for line.

## 2. Decisions

**Font backend: CoreText.** FreeType is not available with the Command Line Tools and the build must not need extra
installs, so glyphs are rasterised with CoreText from the install's own TTF files (`CGFontCreateWithDataProvider`,
8-bit coverage, no font smoothing). The *layout* does not depend on the rasteriser: metrics and advances come from the
TTF tables with FreeType's rounding (`ttf_metrics`, `ttf_advance`), so widths, line heights and cell positions equal the
original's and `fontpreview.mjs`'s. Kept from the decoded backend (ui-spec 3.2): pixel size = fonts.xml size, cell
height = FontHeight, the bitmap's ink box placed at the cell's left edge (left bearing dropped, open question 2), top at
Ascent − bitmap_top, no kerning, coverage as alpha with white RGB, alpha test ≥ 8. Difference: CoreText does not hint,
so stems can differ by a pixel from FreeType's hinted output. Outline (O) fonts are drawn as plain fonts (EE leaves them
empty; ui-spec 10 Q3) — nothing in the HUD asks for them.

**Retina: original pixels, rendered at the backing scale.** The UI is laid out in UI pixels = window points, which play
the role of the original's screen pixels (no UI scaling, ui-spec 2.4; fonts.xml's resolution is chosen from the width in
points). The pass draws them at `backingScaleFactor` drawable pixels each: atlas pieces drawn at their own size are
point-sampled, so they double crisply; stretched pieces are bilinear; glyphs are rasterised at size × scale with the 1×
layout, so text is sharp while metrics stay the original's. Snapshots render at scale 1 (`--ui-scale 2` for a Retina
check). The map keeps its own drawable-pixel camera.

**Order instead of depth.** Every quad carries the window's `update_depth` (+1 per window, +2 per layer, ui-spec 5.3).
`UiRenderer::encode` sorts quads by depth (stable; glyphs after the quads of the same depth, as `flush_fonts` runs after
`flush_all`) and draws back to front in the overlay pass, batching consecutive quads with the same atlas, state and
sampler. This is the LESSEQUAL depth test of the original without a depth attachment (and sidesteps ui-spec 10 Q1).

**Render types.** NORMAL / HIGHLIGHT opaque; ALPHA, ALPHA2, ALPHAHIGHLIGHT blend with alpha test > 0; COLORALPHA with
SRCALPHASAT; COLORBLEND = vertex colour; type 7 blend without test; ADDFB additive with texture colour; MODULATE2X
saturates. Alpha blend factors ONE / ZERO, as d3dgl.

**Simulation boundary.** The HUD reads `GameView`, `FogView`, `LeaderView` and rules types; commands go through
`CommandManager` (`issue_move_to`, `issue_halt`, `issue_build`, `issue_queue`, `issue_unqueue`, `issue_gather_point`).
The placement preview calls `snap_center_who` and `blocked_site` on the live game (no RNG draws in build_place.cpp).
Selection, control groups, build pages and targeting modes are seat-local.

## 3. What is drawn

- **Bar (IFACEMAINBASE, opened mode)**: slot backgrounds (left slot `back` + border only without a selection; right slot
  `back`), `bar` across the top 34 px, clock (`time_back` stretched into `timer_area2`, "Normal: H:MM:SS" in `clock_size`
  with a 1,1 shadow, blinking while paused), frame edges, the bar buttons' textures, the display button (3-slice + three
  arrows). Skin from the viewer's age and nation build style (`IFaceMainBase::age_update`).
- **Top strip**: caption plate (105 + 10 × 21 + 105) with "Nation - Age", border3 around the cap area and help + menu,
  pop cap ("control/pop_cap", red blink at the cap) and city cap ("cities/limit") on HIGHLIGHT plates with digit set 0.
- **Resource panel**: `adjust_rects` geometry, the three vertical toggles (clickable; `--panels N`, Alt+R / Alt+M / Alt+E),
  commerce-cap box (blinks red while any good is over the cap), six rows in good_order with icon, stockpile, "/" and rate
  (plate yellow with black text, blinking, while the good is over the cap). **Market** (open with the resources): Buy | Sell
  title, four rows (food, timber, metal, oil) of 3-slice price buttons (buy 0x006C1E, sell 0x007B9B, HIGHLIGHT, prices in
  digit set 1, pressed shifts the content 1 px, disabled without a market and the trade right ×2/3). A click issues `BUY` /
  `SELL` (Shift = 500, Alt = all). **Infrastructure**: a cell per good with the worker icon and "workers/limit".
- **Mini library** (IFACEMINILIBRARY, top centre, tab toggles it): the next tech of each library line and the next age as
  icon buttons, progress bars for what a Library is researching, the 2×5 level ticks per line. Clicking a button queues that
  tech at the first Library.
- **Timers** (`draw_timers`): the wonder, territory, capital-recapture and time-limit clocks as 42×39 icon + M:SS digits at the
  top right from y = 45, team-coloured borders for a player's clock.
- **Mouse-over help** (section 11.1), **game messages and notices** from the simulation (11.2), **streamlined mode** (11.3).
- **Selection panel**: single object: frame (back, misc_border, misc_tile, joints), team-colour name bar with the name
  (12 B, shadow), owner plate, portrait (icon cell ×2, frame overlay, health bar by health_level), hit points, stats
  (attack / 10, armor, range), buildings: construction or production progress bar and text, current item button with the
  green progress overlay, the rest of the queue, the infinite-queue icon. Several objects: the 60×35 grid, column-major,
  k-shrink with small cells, health bars.
- **Command card**: dividers, joints, `blank_button` in empty slots; icon buttons with the HIGHLIGHT grey factors
  (0x80, hover / latched 0xa4, disabled 0x40) and the frame overlay; research badge. Contents: unit orders (MOVE_TO,
  HALT; citizens BUILD / BUILD_MILITARY / BUILD_WONDER), the build menu pages (every building type with
  `type_avail == 4` and `cat == page`, at GRID_X / GRID_Y, disabled when `can_pay_cost` fails; BACK, NEXT, CANCEL), a
  building's TRAIN / RESEARCH buttons (`build_can_make`: 4 → train at the unit's slot, 2 → its upgrade one row down,
  techs at theirs).
- **Minimap**: frame `back`, the background texture (rendered once: terrain, water, trees 0x425542, rivers 0x576870 and the
  mountains' 0x857F69 at alpha 0x77, additive, top-down 256²) on the 2:1 diamond, the **territory layer** (border colour of
  each owner at fog resolution, alpha 0x50 inside and 0xC8 on edges, only where the viewer has explored; rebuilt every 45
  frames from `WData.who`), fog multiplied (luminance 0xff / 0x80 / 0x50 / 0), blips (black-framed squares: units 2,
  citizens and scholars 1, buildings 3, cities 6) in team colours, pings with the EFFECTS/MINIMAP/PING styles of
  effects_graphics.xml (default, attack, wonder, citylost, citywon: phases by p, triangle alpha, scale, rotation), the white
  view box, the four buttons (clickable: next military unit / general / objectives / idle citizen).
- **Placement**: Template Blend footprint (green checkerboard valid, orange soft blocks, red otherwise), the ghost
  building with the normal passes or Blocked Building Blend.
- **Cursors**: NSCursors from cursors.xml (Main, Move To while targeting, Attack over an enemy with units selected).
- **Second pass (section 12)**: the library grid replacing the card for a finished Library, the Market's SELL / BUY card buttons
  (good icon, price, trend arrow) or its "Need ... To Buy/Sell Resources" message, the garrison grid right of the selection
  panel with EJECT ALL on the card, the rare-goods column (and the 50 px per column MessageWin shifts right), the progress
  buttons, the armageddon clock, the red flash of a resource row, the formation-drag preview, the infrastructure counts from
  `LeaderView`, own buildings under attack and attacking units blinking on the minimap.

## 4. Input (ui-spec 4)

HUD windows (slots, bar buttons, display button, cap area, caption, resource panel) take clicks; the bar strip between
buttons is MOUSE_THRU. Left click vs drag: 20 px once held 7 graphic ticks (462 ms), else 64 px; right 6 px. Click:
select, Shift adds, Ctrl or Cmd toggles; double click: every on-screen object of the same owner and type; box: units
first, buildings only when no unit is boxed, the viewer's own objects win, one owner, 128 max. Right click: move or
attack (units; Shift waypoint, Alt queue first), rally point (own buildings), cancel (placement / target). Card: MOVE_TO
targets the next left click; build buttons start placement (Ctrl / Shift keep placing). Minimap: left press / drag looks,
right click orders. Control groups Ctrl+0..9 / 0..9 / twice to centre / Shift adds / Ctrl+Shift erases; empty selection
bookmarks the camera. Edge scroll: 2 px band (sides only above the HUD top − 40), `scroll_speeds[zoom]` steps. Mouse
wheel and pinch zoom one level toward the cursor (in to 6, out to 4); trackpad scrolling pans.

## 5. Inferred or approximate (check against the game)

- Divider tees (`draw_horiz_divider` / `draw_vert_divider`): positions came in registers; placed at the bar ends,
  centred.
- The blocked ghost's diffuse is red (the decompile shows only the render state).
- Button frame overlay drawn in the button's grey (spec: "same colour"); queue buttons stretch the full 61×39 icon into
  30×32.
- Command card: ATTACK_TO, REPAIR, ALARM are shown disabled (no ATTACK_TO verb; the others not wired); GARRISON targets the
  next click (section 12.3); unit stances, formations, auto-explore, patrol, disband are not offered yet.
- Rates and the commerce cap come from `leader_view` (the economy port exposes them).
- The clock's speed word is the rules' `gamespeeds` name for the current speed, localised through `uistr::tr` (section 12.6).
- Unfinished construction sites are drawn as the finished building.
- Section 11 lists the new approximations: the help verbosity default (2), the attacker of an attack warning, the attack-ping
  lifetime (6 cycles), the streamlined single-unit panel, the infrastructure counts and the mini library's progress bars.

## 6. Not yet

What is still missing after the fourth pass (section 13):

- (Done in section 15: the air garrison in the streamlined layout, the carrier's REPEAT, AUTO_EXPLORE, the Options page.) TRANSPORT, ADVANCE (no verb) and
  ATTACK_TO (no verb) are not offered; REPAIR, ALARM stay disabled.
- Silo / carrier art states that need a scenario with a carrier: no stock scenario has one, so the carrier branch of the panel
  (the `FIGHTERBOMBER` family, `CARRIER_*` keys) is covered by the shared code and the snapshots of the airbase only.
- Localised option help for the literals the port writes itself (section 12.6 lists them; the air panel's "Ready" / "Fueling"
  titles and the type-button hint go through `uistr::tr`), the profile's notification flags, and the profile page that would set
  "Advanced Menu" / "Group by type" / "Prefer Military" (they are read from `NSUserDefaults`: `AdvancedMenu`, `GroupByType`,
  `PreferMilitary`; the bar's Advanced button toggles the first).
- `type vslot 0x104` in the do_damage threshold (engine side).

Done in the third pass (12.8): HOTKEY, COME_OUT (ejecting one unit / every unit of a type), SPEED_*, PAUSE and CHAT as lockstep
commands. Done in the fourth pass: everything else the earlier list named (section 13).

## 8. Script UI (2026-10-13)

The scenario scripts (`engine/sim/script_ui.hpp`, sim-implementation "Scripts") are played in `ronmap`: the window
reports the local seat's interface to them every frame and draws / plays what they post.

**Input (`fillScriptInput`, before every `Game::do_frame`).**
- `attached = true`; `select_who` / `selected` = the owner and objects of the current selection (units, else buildings).
- The armed command from the card state: MOVE_TO targeting gives `option 9` and `selecting_spot`; an open build page
  gives `build_options`, `mode_data` = the page and `option` = that page's BUILD option (0 / 1 / 2); placing a building
  gives `option` = its page, `option_object` = the type and `selecting_spot`.
- The cursor tile from the ground under the mouse (-1 when the mouse is outside the window); `cursor_over_iface` = below
  the bar's top while the HUD is shown.
- `screen_w` / `screen_h` in UI pixels; `iface_rects` = every IFACERECT of IFACEMAINBASE by lower-case name
  (`left_window`, `mid_window`, `right_window`, ... after the bar's offset; the top-strip ones at the top of the
  screen), plus `minimap` and `butt_XY` (`Hud::named_rects`).
- `objectives_rect` = the objectives box of the last draw. Headless runs lay the script layers out every step, so
  `get_objectives_*` work without frames.
- Camera position, zoom and panning; `on_screen` = the tile centre at terrain height projects inside the drawable.
- `keystate` from key down / up (letters, digits, space, return, escape, tab, arrows, Shift / Ctrl / Alt as Windows
  virtual keys).
- `user_warnings` |= bit 0 when the card queues an item or starts a building the player cannot pay for
  (GroupOut::issue_queue_up / validate_build).
- `sound_state` = 0 while the scenario sound plays, else -1 (the WAV-length model). A sound started inside a frame is
  never reported finished before it was heard, and a slower simulation never waits beyond the real playback.
- `graphic_clock` = the wall clock in ms.
- `popup` = a nested event loop that draws the popup and waits for a button: the script stays suspended inside
  `do_frame`, as in `BasePop::exec`. Headless runs answer button 0.
- Multiplayer: the builtins keep the original's `semaphore byte0 & 4` guards themselves, so the input is filled the
  same way in any game; nothing in ronmap feeds the presentation back into the simulation.

**Presentation (`scriptview::View`, after every `do_frame`; drawn after the HUD).** From the decompiles in
`re/decomp/ui_hud` (MessageWin) and `re/decomp/scriptui_render` (ScreenArrowSpline, TextBubble, Scene::add_ping,
ScenarioData::draw_components, GraphicPieces::draw_highlight_circle, JukeBox, SoundGlobal, BasePop):

| What | How |
|---|---|
| Objectives (`draw_scen_objectives`) | At x = 10, y = resource panel bottom + 20 (the top of the screen while the HUD is hidden). `scenario_objectives` font (12 B), white, width at most w / 3, on the IFACERENDERMANAGER `scroll` 9-slice tinted BLACK and padded 8 / 4. Check boxes `check_off` / `check_on` (MESSAGEWIN, 24 x 21) when any entry came from `add_objective`; text-only entries are indented but unchecked. A completed entry's check wipes in over 15 graphic ticks and the entry goes 30 ticks after completion. The box is `get_objectives_*`'s rectangle. |
| Message list | Under the objectives, `message_size` (12 B): the last 10 live entries, wrapped to the running maximum of min(width, w / 3), on one 0x50000000 quad with 3 px padding, colours with a 1,1 black shadow. `print_game_msg` in game_msg_color for message_length x 45 = 225 ticks; chat as "Name (Nation): " in the sender's TEAM_COLOR_TEXT with the text white, 450 ticks; bubbles in the console's neon colour. |
| Top centre | Feedback (red-tinted plate, white text, black shadow, 225 ticks), else instructions (black plate); then the scenario message (`print_msg`: `scenario_size` 12, red plate, white text, `msg_time` graphic ticks, -1 forever). Centred, width min(text, w / 2) + 1, top at 45. |
| Bubbles (`TextBubble::draw_update`) | 120 ticks: grow over 12 ticks (scale 0.2 to 1, alpha 128 to 255, the rect rising on ((2s - 1)^2 - 1) * 50 px), fade over the last 36; +-100 x (-70 .. +10) px around the projected object, `bubble_size` scaled, neon colour, black shadow. Hidden after `bubble_text_hide`. |
| Notices (AnimNotice) | The TEX_ALERTS icon of IFACEMAINBASE centred at native size; after message_length * 400 + 500 ms it moves in 500 ms to half size at (w, h). 22 px bold white text with a black shadow 5 px under it while waiting; a black scroll plate behind. |
| Arrows (`ScreenArrowSpline::draw`) | The `SPLINEARROW` texture (`spline_arrow.tga`, arrowhead in the first `tex_arrowhead` texels) as a strip diag(screen) * 0.4 * 0.03 px wide (0.4 = ArrowSpline::init_class's set_width_percent), the head over the last tenth of the line (`next_at_dist_reverse` 0.1), RED with alpha abs(8 - frame % 17) * 24 + 58. An arrow with a duration ends that many **game seconds** (Game::tick) after it was added; 0 or negative = until cleared. World arrows (tile coordinates) are projected. |
| Highlight (`IFaceButton::on_redraw`, `draw_highlight_circle`) | A card button whose option (plain option buttons) equals `hilite_option` or whose object (train / research / build buttons) equals `hilite_object` gets `BUTTONHIGHLIGHT` (`tutorial_button_highlight_01.tga`) centred on it, half extents = half the texture, turning 20 degrees per graphic tick. `highlight_option(name)` is resolved through options.xml's ENGLISH_NAME in the renderer (the port's builtin passes the name on). |
| Pings (`Scene::add_ping`) | Only for the console leader (or everyone); each clears the previous ping. On the minimap: the "default" EFFECTS/MINIMAP/PING phase (`AttackAlertCircle1.tga`, half-size 16 px growing x1.5 over the 8-tick cycle, alpha a triangle wave) and the pinging leader's neon square (half-size 16 * p, black shadow) for 180 ticks. On the map: the ANNOUNCE_CIRCLE, a TRANSPARENT_WHITE ring shrinking over 8 ticks. Sound S_PING (`soundglobals.xml` PING, `sounds/ping.wav`). |
| Flags (`ScenarioData::draw_components`) | The ScenarioFlag piece (FLAG `banner_0n`, first_flag_piece + 2) at the tile centre on the terrain, scale 3, angle 45 degrees, leader 0's colour, its Banner Wave clock offset by ((frame + y * 17 + x * 13) mod frames) * 67 ms. |
| Camera (`Camera::pan_to` / `set_loc` / `check_zoom`) | Jumps at once. Pans and zooms step every 33 ms: one zoom level per step toward the target level; pans by max(0x300, distance / 2) per axis, snapping within 0x300. `scroll_lock_flags & 8` blocks the moves. |
| Locks and switches | `scroll_lock_flags & 1`: no edge / arrow-key scrolling and no pan toward the cursor when zooming; `& 4`: no minimap look. `zoom_lock_flags & 1`: no wheel / pinch / key zoom. `ui_hidden`: the HUD is neither drawn nor takes the mouse (the script layers stay). `mouse_selection_disabled`: clicks and boxes select nothing. `hotkey_selection_disabled`: control groups do not recall. `pause_disabled`: P does nothing. `pause_game` / `unpause_game` pause the window's stepping. `clear_selection` clears the selection. |
| Popups (BasePop, POPUPSTYLE "default") | A centred box in `bg_color` (39, 40, 43, 224), the `popup_dialog` font, the `butnormal` / `buthighlight` / `butdown` 3-slices 173 x 32. Layout approximated (calculate_dimensions not ported). |
| Goal dialog (`show_goal_dialog`) | A 480 x 330 box (OBJECTIVESDLG) listing the local leader's `add_goal` goals with check boxes and an OK button; the game waits while it is open; Esc or Return close it. |
| Game over | From the simulation's event log (victory / defeat of the local leader): the `victory_notice` / `defeat_notice` AnimNotice, YOU_WIN / YOU_LOSE, the WIN / LOSE track, and "Victory!" / "Defeat!" with the scenario's victory / defeat message in `game_over_message_size`, centred under the resource panel. |
| Unit scale | `set_unit_scale` multiplies the figure's graphics-piece scale. |

Port notes and approximations (check against the game):
- The objectives box sizes text-only entries for their drawn spacing (+5 per entry); the decompile sizes them
  wrap_height + 1 (Buffer::wrap's return convention is not decoded).
- The ScenarioFlag (`banner_0n.BH3`) poses with non-finite, unnormalised normals, which light to NaN;
  `UnitDrawer::draw_ammo` renormalises posed normals (+z fallback). Open: the file's data or the BH3 reader. The banner
  is single-sided and faces the camera with the units' mirrored basis only when the cull mode is not flipped (checked
  by snapshot); its team colour does not show because the texture's alpha is 255 everywhere.
- Objectives that the original's renderer deletes 30 ticks after completion stay in `ScriptPresentation::objectives`
  (the sim keeps them), so `play_objective_sound("")` can differ when completed entries come first.
- `goal_dialog_shown` (ScenarioData::objective_dlg_viewed) is not set by the dialog (ScriptInput has no field for it).
- `display_results` / `display_aging_results` / `save_results` (kVictoryScreen) are informational: their high-score
  lists are objective texts. `take_screenshot` is logged, not written. `use_basic_menu` and `speed_control_disable`
  change nothing visible (the card has no advanced-only options and ronmap no speed control yet). Unit acknowledgement
  and selection sounds (soundtypes.xml) are not played.
- The rare-goods column that pushes MessageWin right (50 px per column) and the mini library's +55 px for the top
  centre text are not drawn yet, so both anchors use their base values.
- The pan cadence (one check_zoom step per 33 ms) and the ANNOUNCE_CIRCLE size are assumptions.

Harness: `--select WHO:o1,..@FRAME` (as a click), `--option N[:MENU[:TYPE]]@FRAME` (the card's option N, then build
page MENU, then building TYPE's button), `--cursor X,Y@FRAME`, `--look X,Y@FRAME`, `--zoom-at LEVEL@FRAME`,
`--script-log`; `RON_POPUP_TEST="text"` leaves a popup open for a snapshot, `RON_POPUP_KEEP=1` keeps the scripts' popups
open in headless runs.

Checked by reading snapshots:
- tut1: selecting Boadicea completes `Select_Boadicea`, adds the flag and the arrow to `right_window`; the Move button
  gets the highlight; the scroll objective completes from `--look` (is_on_screen), the zoom objective from `--zoom-at`;
  the reminder arrow points at the objectives box.
- tut2: `--select 0:0@900 --option 1@1500` completes `select_citizen` at 900 and `military_menu` at 1500 (military
  build page open, the Lookout highlighted); pings show the announce ring and the minimap ping; script pans move the
  camera to the citizen.
- ceaging1: the text-only objective list and the defeat at 3:00 (notice, game-over line). cehotkey1: the hidden HUD
  and the red scenario-message plate. tut5: the first game message. The popup's look via `RON_POPUP_TEST`.

## 9. Audio

`platform/audio/audio.*` uses AVFoundation's `AVAudioPlayer` (build-mac.sh links `-framework AVFoundation`; nothing to
install).
- **Voice**: ScenarioData::sound. `play_sound` / `play_objective_sound` resolve `<scenario dir>\<file>` through the
  install; one sound at a time; `stop_sound` stops it; the playback state feeds `sound_finished` (section 8).
- **Effects**: one-shots from `soundglobals.xml` KEY to `soundfiles.xml` tag (S_PING, YOU_WIN, YOU_LOSE), a random entry
  when a key lists several (SoundGlobal::play).
- **Music** (the JukeBox): tracks per mood from `Data/sound.xml` TRACKS / TRIBE "default" / AGE 0 / MOOD
  (BATTLE_VICTORY = `winning`, BATTLE_DEFEAT = `losing`, ECONOMIC = `economic`); the starting mood is ECONOMIC; a
  random track, never the same one twice in a row; the next starts 1000 ms (JukeBox::timing) after one ends;
  `set_music_mood` switches at once only when the playing track began more than 30 s ago (JukeBox::set_next_mood),
  with a cross-fade over min(5000 ms, the old track's remainder) (JukeBox::play), else the next track takes the new
  mood; the WIN / LOSE track at game over. `set_music_volume` has an empty body in the original.
- Volumes: effects and voice 1.0, music 0.6 (the profile sliders are not read); `RON_SFX_VOL`, `RON_VOICE_VOL`,
  `RON_MUSIC_VOL` (0..1), `RON_MUTE=1`, `RON_NO_MUSIC=1`. Snapshots and harness runs play nothing: the window starts
  the audio.

Not done / to confirm: the tribe-specific first track of a non-scenario game (JukeBox::shuffle's switch on the
console's tribe when frame < 11), audio ducking, the profile's volumes, positional effects (SoundGlobal::play with
coordinates), unit acknowledgements.

## 7. Snapshots

```
./build/ronmap ~/Games/RiseOfNations scenario/Tutorial1/tut1.scx --select 0:2 --snapshot tut1.png --size 1024x768
./build/ronmap ... scenario/Tutorial1/tut1.scx --select-build 0:2002 --at 3936,5088 --size 1280x800 ...
./build/ronmap ... "scenario/Chess Exercise/ceraiding1.scx" --play 1 --select 1:10,11,12 --build-page 0 --at 4536,32568 ...
./build/ronmap ... "scenario/Chess Exercise/ceraiding1.scx" --play 1 --select 1:10,11,12,13,14 --place 427@8600,31200 --place-click --frames 150 ...
./build/ronmap ... "scenario/Chess Exercise/ceraiding1.scx" --select-build 0:2000 --queue '0:2000@50*3' --frames 60 --at 29472,20448 ...
./build/ronmap ... --ui-scale 2 --size 2048x1536 ...   # Retina check
./build/ronmap ... scenario/Tutorial1/tut1.scx --frames 420 --select 0:1@300 --size 1024x768 ...   # arrow, flag
./build/ronmap ... scenario/Tutorial2/tut2.scx --frames 1600 --select 0:0@900 --option 1@1500 --script-log ...   # highlight
./build/ronmap ... "scenario/Chess Exercise/ceaging1.scx" --frames 2720 ...   # defeat notice, text-only objectives
./build/ronmap ~/Games/RiseOfNations --menu main --snapshot m.png --size 1024x768   # main, tutorials, skirmish, scenarios, quit-popup
./build/ronmap ... --menu main --snapshot m.png --size 2880x1800 --ui-scale 2       # a Retina 1440 x 900 window
RON_MENU_DROP=1 ./build/ronmap ... --menu skirmish ...                             # the open AI difficulty drop-down
RON_MENU_ANIM=2,10 ./build/ronmap ... --menu main ...                              # backdrop animation 2 at 10 s
RON_INSTALL_LOGO=1 ./build/ronmap ... --menu main ...                              # the install's bump-mapped EE logo
RON_GAME_MENU=1 ./build/ronmap ... scenario/Tutorial1/tut1.scx --snapshot g.png    # the game menu (2: game over)
RON_FLOW_TEST=1 ./build/ronmap                                                     # menu -> game -> menu -> game -> quit, one window
```

Section 11 harness (game folder `~/Games/RiseOfNations`, 1280x800, PNGs under `re/previews/`, gitignored):

```
./build/ronmap ... scenario/Custom/europe/europe.scx --frames 200 --panels 7 --snapshot re/previews/ui_market_infra_minilib.png
./build/ronmap ... europe.scx --select-build 0:2000 --panels 7 --hover 170,30 --snapshot re/previews/ui_tip_good.png       # --hover X,Y: mouse + tooltip at once
./build/ronmap ... europe.scx --play 0 --select-build 0:2041 --hover 158,650 --snapshot re/previews/ui_tip_train_hotkey.png
./build/ronmap ... europe.scx --play 0 --select 0:0 --hover 30,650 --snapshot re/previews/ui_tip_move.png
./build/ronmap ... europe.scx --frames 2410 --snapshot re/previews/ui_notes_attack.png --script-log                         # NOTE lines list the messages
RON_TIMER_TEST="0:754:1:12,1:83:0,3:3000:3,5:1800" ./build/ronmap ... europe.scx --streamlined --select-build 0:2000 ...    # kind:seconds:colour:count
./build/ronmap ... europe.scx --play 0 --select 0:0 --key "B@10" --key "F@20" ...                                          # --key "KEYS@FRAME": the key map
--panels N (1 market, 2 resources, 4 infrastructure), --streamlined, --help-level 1..3
```

Section 12 harness (same game folder, 1280x800, `re/previews/ui2_*.png`):

```
./build/ronmap ... europe.scx --play 0 --select-build 0:2052 --frames 30 --snapshot re/previews/ui2_library.png          # library grid (0:2052 a Library of seat 0)
./build/ronmap ... europe.scx --play 0 --select-build 0:2054 --frames 30 --snapshot re/previews/ui2_market_card.png      # the Market's SELL / BUY buttons
./build/ronmap ... europe.scx --play 0 --select 0:49,52,53,50,51@0 --option 26@1 --mouse down:640,300@2 --mouse up:640,300@2 \
    --select-build 0:2065@399 --frames 400 --snapshot re/previews/ui2_garrison.png          # GARRISON target click, then the fortress's garrison
... --mouse down:985,660@400 --mouse up:985,660@400 --frames 430 ...                       # a click on the garrison button ejects (ui2_garrison_eject.png)
./build/ronmap ... europe.scx --play 0 --progress-buttons --queue '0:2041@100*4' --queue '0:2052@544*1' --select-build 0:2041@1 ...   # progress buttons
RON_RARE_TEST="7:1,8:3,16:5" ./build/ronmap ... --frames 230 ...                              # rare-goods column (RON_RARE_TEST: good:count,...)
RON_FLASH_TEST=17 ./build/ronmap ... --frames 6 ...                                          # red flash of the rows of goods 0 and 4 (bit mask)
./build/ronmap ... --play 0 --select 0:0,1,2,3,4,5,6,7 --mouse rdown:560,300 --mouse rdrag:640,300 --mouse rdrag:700,250 ...       # formation-drag preview; add --mouse rup:700,250 to issue it
./build/ronmap ... --key '=@2' --key '-@5' ...  /  --speed 0..4                                      # game speed (clock word)
./build/ronmap ... --key 'Ctrl+2@1' --select 0:12@3 --key '2@7' ...                           # control groups (Ctrl+n assign, Ctrl+Shift+n erase, n recall)
./build/ronmap ... --lang de --hover 158,730 ...                                              # German interface (en de es fr it)
RON_TIMER_TEST="2:0:-1:7,0:754:1:12" ...                                                      # kind 2 = the armageddon clock (count in the 4th field)
```

`--mouse "KIND:X,Y@FRAME"` plays a mouse event (down, drag, up, rdown, rdrag, rup, move, wheel) in UI pixels through the same
handlers the window uses (the HUD's layout and state are refreshed first). Snapshots read for section 12: `ui2_library.png`,
`ui2_market_card.png`, `ui2_garrison.png`, `ui2_garrison_eject.png`, `ui2_progress_buttons.png`, `ui2_rares.png`, `ui2_flash.png`,
`ui2_formation_drag.png`, `ui2_notes_attack.png`, `ui2_speed.png`, `ui2_hotkeys.png`, `ui2_lang_de.png`, `ui2_market_tip_de.png`,
`ui2_timers_armageddon.png`.

- `ui_market_infra_minilib.png`: the three resource sections open (Buy | Sell prices, workers / limit), the mini library under
  the caption, the top-left message list.
- `ui_tip_good.png`, `ui_tip_cap.png`, `ui_tip_buy.png`: help.xml tooltips of a resource row, the commerce cap (bullets from
  `bullet_points`, inline `#ICON` pieces) and a market button (cost in wealth).
- `ui_tip_train.png`, `ui_tip_train_riflemen.png`, `ui_tip_train_hotkey.png`: train buttons with the cost icons, the help text
  of the unit's graphic and the hotkey (`Machine Gun (Cost: ..) (Hotkey: E)`).
- `ui_tip_move.png`, `ui_tip_pause.png`, `ui_tip_stat.png`: plain options (`Name: description`, hotkey, help lines), a bar
  button, a stat icon.
- `ui_notes_attack.png`: "Our city of Dublin is under attack by ..." and a wonder completed (message list, AnimNotice
  banners, attack pings on the minimap).
- `ui_streamlined_timers.png`: the collapsed HUD (minimap at the right end, slots only with a selection, no clock or bar
  buttons) and the four timer kinds at the top right.
- `ui_minimap_territory.png`, `ui_minimap_tut2.png`: the territory layer (europe: five nations; tut2: red and blue).
- `ui_key_build.png`, `ui_key_train.png`: `B` then `F` start placing a farm; `Shift+G` queues five Riflemen at a Barracks.

## 10. Front end (2026-10-13)

`apps/map-mac/frontend.*` draws the front end inside the game window (Metal, the UI layer of section 1); `launcher.*`
keeps only the game-folder detection, the first-run folder picker and the map lists. The screens, copy and placement
follow the user's design (Main Menu with the title ribbon top left, the button frame, the help box, the game-folder row
and the logo on the right; list screens with a centred ribbon, a note, a framed list, AI Difficulty, Back / Play; the
Quit popup). The look is the install's own front-end art, decompiled in `re/decomp/frontend` (TopMenu,
TopMenuSelection, MainMenuBase, TopMenuAnimation):

| Element | Original art and behaviour used |
|---|---|
| Main Menu backdrop | topmenu.xml `BACKGROUND` `ANIM` list (`TopMenu::on_redraw` @0059c100): four parallax animations of 1280×720 layers (`art/anim/anim_bg_N_layer_K.tga` + the `anim_overlay.tga` vignette), each layer shifted by `(move * t + start_offset) * size`, drawn in IFACETEXLOAD `sort` order with render type 7; fade in from black between 0.5 and 1.5 s, out over the last second of `duration` (30 s), then the next animation. Layers are released when an animation ends. |
| List backdrop | `static_bg_tex` / `TEX_TOPMENU_0` = `art/iface_wrapper1.tga` (the antique map of the setup screens; the design's "atlas" backdrop) |
| Title ribbon | TOPMENUSELECTION `ribbon` ×3 (`wrapper_pieces_05.tga`) as a 3-slice expander into IFACERECT `title` (454×74); the title in font 22 (Georgia 22 B), white, black 1 px shadow, centred in `title_text` |
| Button frame | TOPMENUSELECTION IFACEBORDER `border` (`wrapper_pieces_01.tga`, 42 px corners and extras) around the window below the ribbon: client top + title height (74) down to the last button + 41, buttons with 41 px side margins (`TopMenuSelection::exec` sizes the window around its buttons) |
| Menu buttons | `button_left` / `button_mid` (tiled) / `button_right` (`wrapper_pieces_02.tga`, 37 px) at MAINMENU's POSITIONING (454×35, 36 px apart); the atlas row one piece-height up when hovered / focused / pressed, one down when disabled (`on_button_redraw` @0059eac0); caption font 18 (Georgia 18 B), white with a (64,64,64) shadow, moved (+2, +2) when enabled |
| Help box | the hovered (or keyboard-focused) button's help text in font 16 (= Georgia 18 B at 1024; `TopMenu::on_window_redraw`), in the MOTD box style: black at alpha 100 with `motd_border` |
| Logo | the project's own `apps/map-mac/resources/menu-logo.png` (the user's adaptation of the EE logo, packaged into `Contents/Resources`) at the design's spot (right 28; 440 px on the 470 px box's centre so that it clears the 536 px original frame), with a soft shadow from a coarse mip. Fallback (file missing, or `RON_INSTALL_LOGO=1`): topmenu.xml `LOGO`, `ron_logo_texture_map.tga` lit through `ron_logo_normal_map.tga` by a light leaning toward the mouse (`TopMenu::draw_logo` @0059cff0: `dir = normalize(k · unit(centre − mouse), −1)`, k = logo size / screen width); last resort `Wrapper_Main_Logo_01.tga`. |
| Combo box (AI Difficulty) | interface.xml GAMESPYREJOIN `combo_left` / `combo_mid` / `combo_right` / `combo_arrow` (white field, black Arial 12 B text); the open list in the same white with `motd_border` |
| List | the bronze `border` on a dark wash; the selected row on GAMESPYREJOIN `large_button` (bright), the hovered one on the row below; SCROLLCLASS `back_vert` / `slider_vert` / `button_up` / `button_down` |
| Back / Play / popups | POPUPSTYLE default (`butnormal` / `buthighlight` / `butdown`, TEX_DIPLO, 173×32), `bg_color` (39,40,43,224) inside `motd_border` |
| Game menu (in a game) | interface.xml MAINMENU (MainMenuBase): the 9-piece `bg0..bg8` scroll from TEX_CIV, title font 24, TEX_DIPLO buttons: Resume Game, Return to Main Menu, Quit. `Esc` with nothing selected or `F10`; 5 s after the local game-over notice it opens as Game Over (Return to Main Menu, Keep Watching, Quit). The simulation pauses while it is open. |
| Sounds | `TOP_MENU_BUTTON` (soundglobals.xml) on every click; music: `Data/sound.xml` TRACKS / INTROS through the JukeBox (shuffled, as one mood) |
| Cursor | cursors.xml row 0 (the game's main cursor) |

**Decisions.**
- *Scale to fit.* The menu is laid out on a 1024×768 canvas scaled uniformly to fit the window and centred (as the
  design does); fonts are rasterised at the final scale (fonts.xml's 1024 table), pieces bilinear at fractional scales
  (UVs kept half a texel inside each stretched piece, so atlas neighbours don't bleed in). The in-game HUD keeps the
  unscaled rule of section 2. Backdrops cover the whole window keeping their aspect (the original stretches them).
- *Flow.* Without a scenario argument the app opens its window directly on the Main Menu; the window, the first-run
  folder picker (only when no install is remembered or found) and the menu are created in
  `applicationDidFinishLaunching`, inside `[NSApp run]` (no modal session before launch has finished), and brought
  forward with `[NSApp activate]` (macOS 14) + `orderFrontRegardless`. Play shows a Loading box, builds a
  `MapRenderer` and swaps the view's delegate in place; Return to Main Menu drops the game (its audio stops with it)
  and rebuilds the menu in the same window. The relaunch in `applicationShouldTerminateAfterLastWindowClosed` is gone:
  closing the window quits. A game started from the command line returns to the menu the same way.
- *Remembered* (NSUserDefaults): `GameDirectory`, `Difficulty`, `MenuScreen` (the menu reopens on the last screen,
  e.g. the Skirmish list after a skirmish), `MenuSelection.<screen>` (the last map of each list). Headless renders
  write nothing.
- *Keyboard*: Up / Down move the focus or the selection (Page Up / Down, Home / End in lists), Return activates /
  plays, Esc goes back (on the Main Menu it opens the Quit popup, as `TopMenuSelection::on_key_down` does), Left /
  Right change the AI difficulty in lists. Double-click plays a map; the mouse wheel scrolls lists.

**To confirm against the real game.** Whether `state & 1` in `on_button_redraw` is "disabled" (the black row) and
the (+2, +2) caption offset for enabled buttons (read literally from the decompile); the disabled caption colour (grey
here); the menu's drop-in slide (`GravWindow::animate`, S_MENU_DOWN / S_MENU_UP) is not reproduced; the install
logo's lighting (approximated: `tex · (0.42 + 0.78 N·L)` plus a small highlight); which animation plays first (the
port starts with the Destroyer, index 0); whether the menu music is INTROS or FULLINTROS.

### 10.1 Skirmish setup screen (random maps, 2026-10-24)

The Skirmish list keeps the ready-made maps and gains a **Random Map...** button (left of Back) that opens `skirmish-setup`
(`--menu skirmish-setup --snapshot x.png`; `RON_SETUP_OPEN=n` opens combo n for a snapshot). It follows the original's solo
setup (`Data/setupwin.xml`: `PLAYERSWINSOLO` at left 2 % / top 2.6 % / 55 % wide with the columns name, nation, who, difficulty,
team; `RULESWIN` at the right, 40.5 % wide, with a stack of combo boxes) on the same 1024 x 768 canvas, ribbon and backdrop as the
other screens:

| Part | What |
|---|---|
| Players window (left) | one row per seat (rows 38 px apart): name (`Player` for the human, `Computer N`), Nation combo (Random + the 24 tribes in rules.xml TRIBES order), Type (Human / Computer), Difficulty (the 6 `difficulties`, greyed for a human), Team (None, 1..4). Seats beyond the Nations combo read "Seat N: closed". A line below describes the map and the Random nation rule |
| Rules window (right) | Map Style (the 23 `mapstyles` names, the random pickers first), Map Size (`mapsizes`), Nations 2..8, Start With (`startingtowns`), Resources (`startingresources`), Start Age (`startingtechs`, incl. All Technologies and Random Age), End Age, Victory, Elimination, Population (`poplimits`), Reveal Map, Random Starts (No / Yes), **Seed** (an edit field and a Random button; empty = random, digits only, 10 at most) |
| Controls | the GAMESPYREJOIN combo pieces as the AI Difficulty box (white field, black Arial 12 B, blue arrow); an open list is 12 rows high (wheel and arrows scroll, it opens upward near the bottom edge); `motd_border` boxes with the setup windows' alpha-120 wash; Back / Play are the popup buttons |
| Keys | Esc back to the Skirmish list (closes an open list first), Return plays, Up / Down change the value of an open list, digits and Delete edit the focused seed |
| Play | `frontEndPlayRandom:` shows "Generating the map..." and builds a `MapRenderer` through `+randomGameWithDevice:` (`newgame.cpp`: the generator, the objects, the scripts), then opens it like a scenario. A seed left on random is drawn from `arc4random` at Play and recorded in the setup, so a save regenerates the same map |
| Remembered | the setup (not the seed) in NSUserDefaults `SkirmishSetup` as the setup text of `random_setup_string` |

Lists come from `Data/rules.xml` CATEGORIES (the lobby fills its combos from the same table), parsed once when the screen opens
(`#ICONnn` prefixes of the style names are dropped). Per-seat difficulty is kept per seat; the simulation reads the first AI seat's
(sim-implementation.md "Random games"). Not offered yet: custom colours, handicaps, game rules, rush rules, tech cost, victory option
lists (score goal, time limit, wonder / population / economic settings), a map preview, named saves.

In a random game F5 / F9 work as in a scenario (the save records the setup text, the load regenerates the map and overwrites the
state), and `--random STYLE,SIZE,SEED,NATIONS[,TRIBES...] [--all-ai] [--frames N] [--census] [--save-game f] [--snapshot f]` runs one
headless (`--difficulty` sets every computer seat).

### 10.2 Conquer the World (`ctw_ui.*`, `ctw_cli.*`, 2026-10-24)

The Main Menu gains **Conquer the World** (fourth button; Multiplayer and Quit move down one) opening `ctw` (campaign list and
set-up) and, with a campaign in play, `ctw-map` (the strategic map). `ctwui::Screen` draws them through a `Bridge` of the front
end's own primitives (popup button, thin box, bronze frame, list row, ribbon), so the look is the other screens' (section 10);
`ctwui::Session` holds the campaign (`sim/ctw.hpp`'s `ConquestGame`, its own files and rules) and outlives the front end, which is
rebuilt around every battle.

| Screen | What |
|---|---|
| `ctw` | ribbon "Conquer the World"; the five campaigns in a framed list (name, rounds, territories, colonies), the selected one's description / intro / victory text; **Your nation** (< > over the campaign's `VALID_NATION`s, "Random" first when there are several), **Difficulty** (six buttons), Random nations / Random bonuses (only where the campaign allows them); Back, **Load Campaign...** (the autosave and the manual saves of `~/Library/Application Support/OpenRiseOfNations/Conquer the World`, newest first), **Start** |
| `ctw-map` | the campaign's art (`conquest/RenderMaps/RENDER_<file>.png`, at most 2048 px on the long side) with every country tinted by its owner (24 fixed colours by nation slot, 42 % alpha; barbarian countries untinted), found through the colour-picking map `conquest/<file>.png` (`<COUNTRY red green blue>`, nearest-sampled so the colours stay exact); link lines from country label point to label point (dashed for sea links, gold from the selected territory, red for the attacks of the selected army), a dot per country (a gold ring for capitals), the name, level pips (one per strength level), supply-depot / fortified marks; **armies are discs** in the owner's colour (white centre; dimmed once moved; a gold ring for the selected one), not the original's 3D pieces (`CONQUESTGRAPHICPIECES` is Cold War art not used) |
| top bar | campaign, round n of N, age name, nation and leader, tribute, territories, cards, the peace rounds left |
| right panel | tabs **Map** (the selected territory: owner / vassal, strength, capital, one-time tribute, resource, wonder, card, depot, fortified, Fortify; with an army selected a button per move and per attack, the attack naming the defender and the war price) , **Cards** (hand with "map" / "battle" tags, description, Play on the map / Use in the next battle, the cards chosen for the battle, Buy a card), **Diplomacy** (nations with stance and territories; War / Peace / Alliance / Vassal with their prices, Demand tribute, Bribe 25, and a **deal** builder: type Peace / Ally / Vassal / Exchange, tribute each way, Propose -> the campaign's own `diplo_response` script answers and an accepted deal is applied), **News**; End Turn, Save, Menu |
| dialogs | modal: messages of the campaign scripts (`popup_dialog`), the attack (who marches where, the picked battle's title / description / objective with `$NUM0` = the time limit, the seats, the war price, the cards in play; **Play** or Cancel), the invasion of the player's territory (**Defend (Play)** / Auto-resolve), AI offers, card target choices, game end |

Input: click a territory to select it (the player's army on it becomes the selected army); click a green territory to move, a red one to
attack; wheel zooms around the pointer, drag pans, `+` / `-` / `f` zoom and fit, arrows pan, Esc cancels (dialog, targeting, selection,
then back to the menu), right button deselects. A card that needs a territory highlights the valid ones in green; one that needs a
nation opens a button list. End Turn runs the AI round at once and shows the News tab; the autosave is written after every round.

Flow into a battle (`frontEndPlayCtwBattle:` in `main.mm`): Play fills `Session::battle` (`BattleDef`: link, style, seats,
`RandomGameSetup` or the `.scx`), the host builds `MapRenderer` through `+ctwBattleWithDevice:...build:` whose `build` lambda is
`ConquestGame::build_battle(files, rules, battle, anims, extras, error)` on the renderer's own files / rules (the `Game` keeps
pointers into them), then attaches it like a skirmish. The game-over line shows as usual; 75 simulation frames (5 s) after the local
player's victory / defeat `onCtwEnd(1 | 0)` fires, or at once with `onCtwEnd(2)` when "Return to Main Menu" is chosen in the game menu
(a retreat is a loss); the host calls `Session::battle_finished(outcome)` (`process_results` or `resolve_player_defense`, then the
autosave), rebuilds the front end and opens `ctw-map`; an AI round interrupted by an invasion resumes there (`resume_ai`).
`RON_CTW_FLOW_TEST=1 ronmap --ctw alexander` runs map -> battle -> win -> map in the real window and prints the steps.

Headless (all `--snapshot out.png [--size WxH]`): `--menu ctw` (the list), `--ctw CAMPAIGN [--round N] [--seed S] [--difficulty D]
[--ctw-screen map|news|cards|diplomacy|attack|menu] [--ctw-select ID] [--ctw-click X,Y]...` (N AI rounds first; the clicks are
real mouse events on the screens), `--ctw-load FILE`, `--ctw-battle CAMPAIGN:LINK --frames N` (the battle in the renderer). Snapshots
read for this section: `re/previews/ctw_list.png`, `ctw_map_alexander.png`, `ctw_map_select.png`, `ctw_map_napoleon.png`,
`ctw_map_newworld.png`, `ctw_map_coldwar.png`, `ctw_map_world.png`, `ctw_news.png`, `ctw_cards.png`, `ctw_diplomacy.png`,
`ctw_attack.png`, `ctw_battle_alexander.png`.

**Not done / approximate.** The original's `conquest.xml` windows (977 `IFACETEX`; the invade, diplomacy, card, wonder, reward, victory
and final windows) are not reproduced: the panels above are this port's layout. No army drag (click-click), no animation of AI
moves (the round resolves at once and the News tab reports it), no sound, the Cold War nuke / espionage / defcon screens, the
continent-victory and end-game image screens, a bonus-card reward screen after a defence.

### 10.3 Multiplayer (`lobby.*`, the lobby screens of `frontend.mm`, 2026-10-24; third pass: lobby rules, LAN list, scenarios, campaign battles)

The Main Menu's fifth button (**Multiplayer**, enabled now) opens `multiplayer`; Host / Join lead to `mp-lobby`. The network side is `mp::Room` (`apps/map-mac/lobby.h`: a LAN room over
`engine/net` TCP, `docs/multiplayer-spec.md` §8.5.3 and §8.6), the screens only draw and drive it.

| Screen | What |
|---|---|
| `multiplayer` (connect) | ribbon "Multiplayer"; **Your name**, **Host a game** (Port, default 7777, **Host**), **Games on your network** (the LAN list: host, map, players / seats, address; rows come and go as hosts announce / stop, a grey row is another version of the game; a click fills the address, a double click or Return joins, Up / Down choose), **Join a game** (Host address as `host:port`, port 7777 when left out, **Join**), the error of the last try in orange, a note that everybody needs the same version and game files. Fields use the setup screen's white combo field; Tab switches field, Return hosts / joins, Cmd+V pastes, Esc goes back. Remembered in NSUserDefaults (`MpName`, `MpPort`, `MpAddress`) |
| `mp-lobby` | the Skirmish setup screen's two windows (`layoutSetup` is shared; the seat rows are the lobby's own, `layoutLobbySeats`). Left, per seat: a **colour square**, the player's name (white; "AI n" for a computer seat, "Seat n: closed"), **Nation**, **Level** (computer seats only), **Team**, **Colour**, **Handicap** (players only), and a small second line with `Host` / `Ready` (green) / `Not ready` and, for the host, **Kick**. The host's combos are always live; a guest's combos are live on its own row and send a request (the table's answer is what the combo then shows; a refusal is a yellow line). In a scenario game nation / team / colour are grey (the scenario's). Under the table the chat (log: sender names in their seat colour, team lines green, private lines pink, system lines yellow; a text field, Tab or any key opens it, Return sends; `/t text` to the team, `/w name text` private). Right: the rules combos (the host's; read-only grey for a guest) and **Observers** (0..6), then the room line (`Hosting 192.168.0.12:7777 - 3 players, 1 observer`, who watches, a note for a saved game / scenario / campaign), **Watch only** / **Play instead**, the host's **Guests' choices: automatic / ask me**, a waiting request ("Ayla asks for team 2" with **Allow** / **Deny**), and the host's **Load Saved Game...**, **Scenario...** (a `.scx` picker: the scenario's players become the seats; a second click goes back to a random map) and **Conquer the World...** (opens the campaign screens with the room kept; Back returns to the lobby; Play on the map starts the battle for the whole room). Buttons: **Leave**; host **Start Game** (label says why not: "Waiting for players", "Waiting for ready"; pressed it starts a 10 s countdown that becomes **Cancel (n)**), guest **Ready** / **Not Ready** |
| Start | after the countdown the host publishes the final setup (players take seats in joining order, computer players the rest, observers sit above the nations; `num_nations` grows to cover them), draws the seed (`arc4random`) and sends START with the plan; every machine, host first, calls `frontEndPlayMultiplayer:setup:save:` (random map), `frontEndPlayMultiplayerScenario:` or `frontEndPlayMultiplayerCtw:session:`, which shows "Generating the map...", builds the same game, attaches an `mp::Runtime` and opens the game. A guest's lobby sees the START arrive by polling the room (`mpPump`, every frame the lobby is drawn) |
| Load Saved Game... | the host picks a `*.ronsave` written by the in-game **Save Game** of a multiplayer game (a file chooser opened on the Saves/Multiplayer folder): the screen takes the save's setup and seed, the rules combos and every seat turn read-only, a line says "Continuing a saved game: <file>, frame N" and Start needs the connected players to occupy exactly the saved human seats (observers may come on top). A second click forgets the file |

In the game: the clock shows the session's pause and speed; **Resign** and **Save Game** (a synchronized save) are in the game menu, which no longer pauses the game; messages about pause / resume / speed / resign / drop / desync / recovery / save arrive through the game's
message area (`say`), and `drawMpStatus` shows "Waiting for the other players to finish loading...", "Waiting for <name>... N s" when a package is late, "The game was out of sync: repairing it from another player's game..." and, for an observer, "Observing: you see the game from X's seat. Chat is open; orders are not." in the top centre. A received chat line shows
"Name (Nation): text" with the sender's colour; the HUD's chat box (`OPTION_CHAT*`) sends the CHAT verb to everybody / the allies / the enemies (an observer's line goes out of band) and understands `/ignore NAME` and `/hear NAME` (CHAT_SET); a line to a team or one player is shown
only to its recipients, a broadcast not at all to a player who ignores the speaker (`mpFilteredPresentation`). Esc / the bar menu and the speed keys work as in a single-player game except that the game does not stop for menus. Every game of a random map or a scenario is recorded
(`Recorded Games/Multi/<date>.rcx`); a Conquer the World battle is not (and has no out-of-sync repair). After a battle the host is back on the campaign map with the outcome applied, the guests in the lobby with the host's new campaign.

Headless / test: `--menu multiplayer --snapshot f.png` (`RON_MP_DEMO_LAN=1` makes three rooms announce themselves on `RON_LAN_PORT` for the list); `--menu mp-lobby --snapshot f.png` (hosts a room on `RON_MP_DEMO_PORT`, default 17777, and lets guests join from threads of this process, say a line and get ready;
`RON_MP_DEMO_REQ=1` they ask for teams / colours / handicaps / nations, `RON_MP_DEMO_OBS=1` the last one watches, `RON_MP_DEMO_ASK=1` the host decides, `RON_MP_DEMO_SCEN=path.scx` picks a scenario, `RON_MP_DEMO_COUNTDOWN=1` starts the countdown, `RON_MP_DEMO_START=1` presses Start);
`--host PORT --humans N [--mp-observers K] (--random ... | --scenario FILE) [--mp-name S --mp-max-observers N --mp-handicap H]` / `--join HOST:PORT [--mp-watch --mp-nation N --mp-team T --mp-color C --mp-handicap H --mp-name S]` with `--frames N --snapshot f.png --save-game f` run a LAN game without the screens;
`--ctw-mp --ctw-battle CAMPAIGN:LINK --host ...` / `--ctw-mp --join ...` a campaign battle; `--mp-act what@TURN` also takes `chat=TEXT`, `team=TEXT`, `ignore=LEADER` (`multiplayer-spec.md` §8.5.3 and §8.6 list the rest). Snapshots read for this section: `re/previews/mp_connect.png`, `mp_lobby.png`, `mp_game_3p_messages.png`,
`mp_game_recovered.png` (second pass) and `mp3_lobby.png`, `mp3_lobby_ask.png`, `mp3_lobby_scenario.png`, `mp3_connect.png`, `mp3_chat_alice.png`, `mp3_chat_bob.png`, `mp3_game_observer.png` (third pass).

**Not done.** NAT traversal (LAN only), an observer choosing which seat to look from, a Block button (the room API has it), "Open" / "Closed" seat types, saved scenario games and saved campaign battles, the original's ranked / matchmaking rules, speed shown for other players' changes beyond the message.

## 11. HUD leftovers (2026-10-24)

### 11.1 Mouse-over help (`tips.*`, `main.mm` `tipFor`)

`Scene::render_mouseover_text` (ui-spec 6.5): the box sits bottom-left above the HUD (x = 10, text bottom = the HUD's top - 4;
`Hud::bar_top()`), a black-tinted `scroll` plate with 8 / 4 padding, 1,1 black shadows, width at most half the screen. Fonts
`helpbox_size1..5` (slots 0..4: grey 190, white, red, white italic, dark green).

- **Markup**: `{` slot 1, `{*n` slot n, `[` slot 2, `}` / `]` slot 0, doubled = literal, `#ICONn` = `icon_texs.xml` entry n
  (**1-based**; verified against the help text: `#ICON20` population limit, `#ICON21` commerce limit, `#ICON28` Classical
  age, `#ICON45` Ancient age), advancing by the icon width + 4. `<BULLET/>` lines get the `bullet_points` piece (icon 44) and a
  hanging indent; `<SPACE/>` is a break.
- **Content**: title (`info_first`, white) + cost (" (Cost: " + `{*1#ICONn amount}` affordable / `{*2` red, verbosity >= 2) +
  hotkey (" (Hotkey: K)"), on one line when it fits in the width - 5, else on a second one; a blank line (verbosity >= 2); the
  help lines. **Verbosity** 2 by default (the profile is not read; `Ctrl+Alt+1/2/3` and `OPTION_TOGGLE_HELP` change it):
  `Scene::load_help_key` appends `KEY`, `KEY` + `LV` / `MV` (< 2 / >= 2) and `KEY` + `NV` / `V` (< 3 / >= 3).
- **help.xml is grouped**: its entries live under `<BUILDINGS>`, `<UNITS>`, `<SPELLS>` (techs, buy / sell, disband), `<LIBRARY>`,
  `<STATS>` (stat icons, infra heads, the victory timers), `<CAPS>`, `<GOODS>`, `<CYCLES>`, `<ORDERS>`, `<EJECTS>`
  (halt, garrison, minimap), `<MAIN_IFACE_BUTTONS>`, `<DATADISPLAY>`, ... and the same name occurs in several groups (`FOOD`:
  GOODS and CONQUESTIFACE; `PAUSE`: main buttons, replay, observer; `MARKET`: the building and the toggle). The reader keeps
  group -> key -> text and each tooltip kind names its groups.
- **Titles / costs**: unit, tech and building buttons use the type's display name, `get_cost` per good (with discounts)
  against the stockpile, the hotkey of the key map (a unit's hotkey is found through its upgrade chain: Riflemen use the key
  of Slingers) and the help entry of the type's `GRAPH` name. Plain options use options.xml `NAME: DESC`
  (translated_strings #2806 is the ": " the decompile appends at verbosity >= 2).
- **Targets** (`Hud::hover_at`): card buttons, selected object / queue item / stat icons / hit points, resource rows and the
  commerce cap, pop and city caps, market buttons (title "Buy Food", price as the cost), the three toggles, infrastructure
  cells, bar and top-strip buttons, the display button, minimap buttons and the minimap, the mini library buttons and tab, the
  timers, the clock.
- **Delay**: the target must stay the same for 3 graphic ticks (`MouseOver::describe`); nothing shows while boxing, placing,
  pressing, a modal window or the game menu is open.

Approximations: titles for the caps, toggles and bar buttons are English literals (the original takes them from strings the
port does not look up yet); the option help key choice (`Options::set_help_type`, a 2000-line jump table the decompiler
refuses) is a small table of the options the card offers; `$NUMBERn` / `$STRINGn` are filled for the goods and caps only;
the group a plain option's description comes from is options.xml, not help.xml, except move / halt / garrison / alarm /
attack-move / patrol / eject-all / auto-explore.

### 11.2 Messages, banners and pings from the simulation (`notices.*`, `script_view` `post`)

`Game::events` is the simulation's tool log (`GameEvent` kinds in `sim/game.hpp`; never read by the simulation, not walked, not in a
digest): 1 city captured, 2 leader defeated, 3 victory, 4 game over, 5 capital lost, 6 capital recaptured, 7 building captured
(capture.cpp), and since the second pass 8 object damaged / attack warning (`Object::do_damage`), 9 technology researched and 10
age advanced (`gain_tech`), 11 wonder started and 12 wonder completed (`build_start`, `build_activate_full`), 13 tribute offered and
14 demanded (`action_offer`), 15 diplomacy changed (`leader_set_diplo`, with the old state) and 16 resource flash (`leader_action_declare`
cannot pay). `Notifier::update` runs after every frame for the local seat and reads only events (frame >= 1: the start-up grants are
not announced) plus one view difference, the "X completed." line of an own building going `active`. The texts are the install's
`translated_strings.xml` entries (the decompiled call sites address them by byte offset; **the index is the offset / 20**:
`Object::do_damage` uses 0xd1d8 = #2686; `StringTables::translated()` indexes them), checked against an expected word in the English
table (the other languages' tables are positional too and used as they are) and otherwise replaced by the English text.

| Cause | Message (index) | Banner (IFACEMAINBASE piece) | Ping | Source |
|---|---|---|---|---|
| city captured by us | "City of X captured." / with plunder (#275, #274) | `city_captured_notice` "City Captured!" | citywon | event 1 |
| city lost | "City of X lost." (#277) | `city_lost_notice` "City Lost!" | citylost | event 1 |
| building captured | "X captured!" (#281) | | | event 7 |
| capital lost (own / ally) | "Your CAPITAL has been captured! ..." (#2308 / #2309) | `capital_notice` | | event 5 |
| own city attacked | "Our city of X is under attack by Y!" (#2686) | `city_under_attack_notice` | attack | event 8, alert 1: a hit that takes the building past hits / 4 (/ 10 for a city centre), or any siege hit, once the city's attack stamp is > 300 frames old; the banner when the previous alert for the city is > 400 frames old |
| own capital attacked | "Our CAPITAL is under ATTACK by Y!" (#2684) | `capital_under_attack_notice` | attack | alert 2, throttled by the leader's `capital_stamp` |
| ally's city / capital attacked | "Allied City of X is under attack by Y!" (#2687) / "Allied Capital under attack by Y!" (#2685) | | attack | alerts 4 / 5, the leader's `ally_stamp` (300 frames) |
| own city raided | "Our city of X is being raided by Y!" (#2688) | | attack | alert 3 (a light hit on a city building, or a worker of the city hit), the city's `raid_stamp` |
| own units / workers raided | "We are being RAIDED by Y!" (#2689) | | attack | alert 6: a worker, caravan or merchant, or a light hit on a building, in its owner's territory (`raid_stamp[attacker]`) |
| own unit attacked | "We are under ATTACK by Y!" (#2691); a scout outside our territory #2690 | | attack | alerts 7 / 8 (`attack_stamp[attacker]`, 300 frames) |
| building finished | "X completed." (#3734) | | | `active` goes true (walls and gates skipped) |
| wonder started / finished | "L has started a wonder: W!" (#96), "W completed!" (#107), others #109 | `wonder_notice` | wonder | events 11 and 12 |
| tech researched | "L has researched T" (#2298) | | | event 9 (own, announced when `gain_tech` is asked to notify) |
| new age | own: `age_noticeN` banner with the age name; others: "L has entered the A." (no stock line) | | | event 10 |
| tribute | "L is offering you N Good." / "L demands N Good in tribute." (no stock line) | | | events 13 / 14 addressed to us |
| alliance / peace / broken alliance | #574 / #573 / #2338 | | | event 15 (new state and old state) |
| resource flash | the panel's rows of the goods lacking turn red for 8 ticks | | | event 16; the card's unaffordable queue / build / library clicks flash the same way (`flashLacking`) |
| game over | the scenario's message, else why (#1134, #1136, #1180..#1185, #1189) | `victory_notice` / `defeat_notice` | | events 2 and 3 |

Colour LIGHT_GREY (180,180,180) (MessageWin's default) except the attack and capture lines, WHITE as the decompile passes
`&WHITE`; duration `message_length * 45` = 225 ticks; banners as section 8 (AnimNotice). The original has **no "unit lost"
message**: only the attack warnings. A located message also adds a minimap ping of its style (6 cycles of 8 ticks: assumption,
the original's `(k + 1) * duration` is not decoded), the ANNOUNCE_CIRCLE and an entry for Space (`OPTION_NEXT_MESSAGE`).

**`Object::do_damage` stamps** (@64a480, ported in `combat_damage.cpp` `damage_warnings`; they are walked state): for a hit across
owners the target owner's `attack_stamp` / `raid_stamp` / `capital_stamp` / `ally_stamp` (`Leader`, indexed by the attacker's
leader; the last two are new members, `save_walk.inc` and `original_map_leaders.cpp`, save version 4) and the city's own
`attack_stamp` / `raid_stamp` (+0x14 / +0x18) are written with the original's 300-frame throttles; a city or capital warning that fires
also runs `Armies::emergency` for an AI owner after a non-lethal hit. The original's message conditions that name the console
(`param_2 == console->who`, `is_ally(console, ...)`) are the HUD's: the event carries the class (`GameEvent::alert`, `gamealert::`)
and the Notifier filters by seat. Approximation: the type vslot 0x104 that picks hits / 4 for a plain building (a "heavy" hit) is
not decoded and is read as clear; the Conquer-the-World "general in danger" branch is skipped.

The events 17-26 (victory clocks, time limit, resignations, nuke count) are in section 17.1.

Victory clocks (`gameTimers`): wonder `wonder_timer` and territory `popwin_timer` (rules constants scaled by the map width:
(ms / 2 + xs * k) / ms, ms = map_sizes[3], at least 1) minus `frame - stamp` for every leader whose flag runs; capital
recapture `game_retake_capital - (frame - lost_capital_stamp)` in elimination mode 1; the time limit from `Game::tick`; the
armageddon clock (`Game::get_armageddon() - armageddon` nukes left, two digits between the `armageddon_timer_left` / `_right`
slices) once `armageddon > 0`. The peace timer and musical chairs are not read yet.

### 11.3 Streamlined (collapsed) HUD

The display button, `Alt+I` (`OPTION_TOGGLE_IFACE_MAIN`) and `--streamlined` toggle `Hud::set_opened`: the minimap moves to the
right end against the display button (x = W - 14 - 332), the right slot to x = 316 and 362 px wide, the slots exist only with
a selection (`has_sel_`; `captures` ignores them otherwise), the `bar` covers only the left slot, no clock, bar buttons or
right frame edges; `left_tile`, `left_tile_only` and the 24×197 `streamlined_end_cap` (placed at the right slot's right edge:
the decompile gives no position) frame the left slot; the display arrow points left. A building shows only progress and queue at
the `*_streamlined` rects; the multi-selection grid is unchanged; a single unit gets its portrait and name (the spec says
streamlined draws none of the single-object frame).

### 11.4 Key map (`keymap.*`)

`re/tools/keymap-inc.mjs` writes `keymap_table.inc` from `re/tools/keymap.mjs` (the exe's compiled defaults, 293 gameplay
rows; the debug SYSTEM / MENU entries and the unbound ones are left out). `lookup` follows `KeyMap::lookup`: the current mode
(1 while a build page is open), then mode 0; an entry that names a type in the selection beats "units" / "buildings"
(`G` garrisons, or trains at a barracks); a TRAIN key also matches with Shift (a batch of five).

Executed (`execKeyOption`): `F10` menu, `P` / `Pause`, `PgUp` / `PgDn` / `Num9` / `Num3` zoom, `Esc` cancel, `Alt+I` / `Alt+R` /
`Alt+M` / `Alt+E` panels, `Ctrl+Alt+1..3` and `Ctrl+Z` help detail, `-` / `=` (and the numpad) game speed, `G` (the next click names the
building to garrison) and `J` (eject all), `Space` next located message, `M` move, `H` halt, `Delete`
disband, `B` / `Y` / `W` build pages and the build-menu letters (`F` farm, `W` woodcutter, `K` barracks, ...), `B` next page,
`Shift+B` back, every TRAIN / RESEARCH key of a selected building (`G`, `H`, `A`, `Ctrl+G` ...), the cycle keys `C K L N S D I
X U [` (+ `Ctrl` without centring, `Shift` select all), `,` military, `.` idle citizens, `/` general, `'` scout, `;` spy. A key
that the table binds but the port cannot run yet is swallowed in its context (so `F` with units selected no longer toggles
the fog); the old debug keys (`B` bloom, `H` HUD, `F` fog, `+` / `-` zoom) work when no binding applies (`-` and `=` are the speed keys now). Not run: ping, diplomacy, repair, auto-explore, transport, advance, attack-to (section 13 lists what the fourth pass bound).

### 11.5 Where the specs and the install disagree

- ui-spec 6.5 names help.xml ENTRY keys without saying the file is grouped: several keys occur in more than one group.
- ui-spec 6.7 says MessageWin entries default to LIGHT_GREY; the attack / capture sites pass WHITE.
- The original has no unit-lost message (only attack warnings), contrary to the task's list.
- `docs/ui-spec.md` 3.4 describes `#ICONn` without the base; it is 1-based.
- The mini-library "toggle" tab: `min_button_tex` at (64,0) / `max_button_tex` at (64,40) read literally put the minimise tab on
  top; the port draws the maximise tab at the top when collapsed and the minimise tab under the plate when expanded.
- `interface.xml` defines `TEX_SPECIAL_RESOURCES` twice: `iface_specialresource.tga` in the global texture list and `resource_icon.tga`
  inside the main bar (the rare-goods buttons); the layout keeps the last. The goods' icons (`TEX_COL` / `TEX_ROW` of resourcerules.xml,
  interface.txt's `BASE_GOOD_TEX`) and the rare column's `TEX2_*` rectangles are in the first file, so the panels draw those
  through a private id (`TEX_GOOD_ICONS`, `UiRenderer::add_texload`).
- **IFaceMarket is not a panel of its own** (ui-spec 3 / 6.1 call it "an IFaceOptions subclass"): it is the command card of a
  Market, whose SELL (row 2) and BUY (row 3) options (`BuildOut::make_options` @00629ed0: `Options::add(OPTION_SELL / BUY, -1,
  row, good, ...)`, placed in the first free slot of the row) `IFaceMarket::on_button_redraw` draws with the good's icon, price and
  trend arrow; `on_redraw` writes the text over the lower rows when the leader cannot trade. translated_strings #2046 "Need
  $TECHNAME0 To Buy/Sell Resources", #2047 "$TECHCATEGORY0 Level $NUMBER0", #2048 "Nuclear Embargo: ", #2049 "Sell:", #2050 "Buy:".
- translated_strings indices = the byte offset in the decompile / 20 (`sizeof(String)`), not / 4.

## 12. The second HUD pass (2026-10-24)

Decompiles: `re/decomp/ui_panels` (IFaceMarket, IFaceGarrison, IFaceLibrary, `draw_special_resources`, `setup_progress_buttons`),
`re/decomp/ui_panels2` (`BuildOut::make_options`, `Options::add` / `grid`), `re/decomp/aircraft/Object_do_damage@0064a480.c`.

### 12.1 Simulation hooks

The `GameEvent` kinds of section 11.2 and the `Object::do_damage` stamps. `engine/sim` writes the events from `combat_damage.cpp`
(`damage_warnings`, `log_damage`: hits across owners are summed per victim, attacker and frame and capped at 100000 events),
`techs.cpp` (`gain_tech`), `build.cpp` (wonders), `econ_market.cpp` (`action_offer`) and `ai_diplo.cpp` (`leader_set_diplo`,
`leader_action_declare`). `BuildView::under_attack` (the UNDER_ATTACK build mask) and `LeaderView::infra_workers` / `infra_limit` /
`rares` / `armageddon` (view.hpp) feed the HUD; the infrastructure counts walk the leader's citizens' GATHER orders and caravans,
the limits are `gather_slots` and `get_caravan_limit` (ui-spec 6.2).

`Leader::flash_resource_stamp / flags` is **Console** state in the original (state-walk.md: `Console::walk_data`, a console-side
field of the saved globals, not part of a subsystem checksum), so it stays out of the simulation: the HUD keeps it
(`_flashMask`, `_flashTick`), set from event 16 and from the card's unaffordable clicks.

**Digest changes** (`ron-sim <game> <scx> --frames 3000 --ai-every 8 --no-selfcheck`, the final digest of all 156 stock scenarios,
a build with the warnings switched off by a temporary environment toggle compared with the normal one): the events and the other hooks change
nothing (156 / 156 digests identical); the stamps change **8** scenarios, all battles: `conquest/ColdWar/central`, `germany`,
`germany2`, `korea`, `korea4` and `conquest/Napoleon/austerlitz`, `lodi2`, `montenotte4`. Two of them (`ColdWar/central`,
`Napoleon/lodi2`) change through the city `attack_stamp` alone (`ai_army.cpp` reads it: `C.attack_stamp + 9000 < frame`), the other
six through the `Armies::emergency` call the original makes when a city or capital warning fires for an AI owner. `sim-all` stays
156 / 156 (self-check and, for germany, the save round trip). `kSaveVersion` is 4 (`Leader::capital_stamp` / `ally_stamp`; the
`sizeof(Leader)` / `sizeof(Game)` guards moved by 64 and 640 bytes); older saves are refused.

### 12.2 Library, Market, garrison

- **Library grid** (`Hud::draw_library`, `main.mm` `libraryState:`): replaces the card for a single selected, finished Library of the
  viewer or an ally, 319 x 168 centred in the left slot. `IFACELIBRARY`'s `back` (the civ-skinned `TEX_LIBRARY_BACK`, already painted
  with the grid and the line icons of the first column), then per column an `age` button (36 x 36) with its `age_underlay`
  (types 544..550) and the `tech0..3` buttons (36 x 28; the four lines' levels, military 572.., civic 565.., commerce 558.., science
  551..); the icon is the type's rules icon, HIGHLIGHT grey 0x80 (hover 0xa4, 0x40 when it cannot be queued now: `build_can_make` at the first
  Library and `can_pay_cost`), the `*_overlay1` frame when researched (`*_overlay0` otherwise), and `age_progress` / `tech_progress`
  up the cell for the item in production. A click queues the type at the first Library (`issue_queue`, the unaffordable ones flash
  the panel). The tooltip is the research button's. (The final-techs "jeopardy" view (`jep_*`, four 74 x 157 buttons) and
  the category column's buttons are section 13.2.)
- **Market card**: for an own, finished Market and a leader that can trade (`LeaderView::can_trade`): SELL buttons in the first free
  slots from row 2 (food, timber, metal, oil; disabled below 100 in stock), BUY buttons from row 3 (disabled below the price in wealth),
  each drawn by `Hud::draw_market_button`: `BASE_GOOD_TEX` + 62 px per good (ALPHA, GREY when disabled), the price in digit set 1 centred
  (WHITE, RED when disabled), the `Game::delta_flux` arrow at 2/3 size (position inferred: centred at the bottom), the row head "Sell:" /
  "Buy:" (#2049 / #2050) over the first button (position inferred). A click runs `BUY` / `SELL` as the resource panel's buttons (Shift x5,
  Alt all). Without the right to trade the lower rows carry "Need <tech> To Buy/Sell Resources" and "<category> Level n".
- **Garrison grid** (`Hud::draw_garrison`, `garrisonState:`): for a single selected building or carrier with units inside (own or
  allied; air garrisons excluded), right of the selection panel up to the display button: 50 x 30 cells, 2 px apart, column-major, shrunk by
  k with small icons when they do not fit, one button per owner and type with the count (digit set 1; the original groups only with
  pref 0x20000), outlined in the owner's colour when several owners are inside. A click (any button, own units only) sends EJECTALL for
  that owner from that container: the original ejects one unit (left) or every unit of the type (right) through COME_OUT (verb 49), which
  has no command here, so both clicks eject the owner's whole garrison (gap listed in 12.7). The card gains EJECT ALL (`J`) for a building with units inside.

### 12.3 Orders

- **GARRISON** (card button, `G`): the next click names an own or allied building; the selected units that fit garrison
  (`issue_garrison`), as the right-click already did.
- **Formation drag** (`uiMouseDragged:` / `endFormationDrag:`, ui-spec 4.1): a right drag of more than 6 px with own units selected starts
  from the selection's common `Unit::form` and mean `form_mod` (default line, 50), shows the preview, and on release issues `MOVE_TO`
  to the press point with the drag direction as `set_angle` / `angle` (`find_angle`, only when the drag is longer than 96 world units),
  the form and the spacing; a left press during the drag cycles the form (LINE .. ECHELON_R, then KEEP = the selection's own
  arrangement turned to the drag direction, `kFormKeep`, sim-implementation.md "KEEP"), the wheel changes the spacing by 5 (0..100),
  Shift / Alt on release queue last / first. The preview draws a ring per selected unit in the owner's colour on a block of
  `ceil(n * (0.1 + 0.9 * width / 100))` columns, 190 world units apart, behind the press point, plus a dotted line to the cursor:
  `Group::compute_form` mutates the global `forms` scratch and cannot be called read-only, so the preview is an approximation of
  the slots the command will produce. Citizens carry no formation; with no units selected a right drag still scrolls the map.

### 12.4 Rare goods, progress buttons, clocks, minimap

- **Rare-goods column** (`Hud::draw_rares`, `draw_special_resources` @0080ef80): one icon per rare owned (`Leader::rare | rare_conquest`,
  `LeaderView::rares`) from the good's `TEX2_*` rectangle (27 x 27) at alpha 0x80, from x = 10, y = the resource panel's bottom + 20, 4 px
  apart, a new column every 8 icons; the collected count (when > 1) in digit set 0 at the bottom right; `Hud::rare_columns()` moves
  MessageWin right by 50 px per column (the mini library's +55 px for the top-centre line was already `feedback_top`). The tooltip names the good.
- **Progress buttons** (`--progress-buttons` or the profile; `setup_progress_buttons` @0080db10, pref bit 0x40): one 61 x 39 button per
  queued unit or tech of the viewer's buildings (finished techs skipped) and per wonder under construction, down the right edge from the
  bottom of the timers, 6 px apart, the first column six buttons and the later ones seven (the decompile counts before it tests), new columns to the
  left; the green fill is the item's progress, the outline the owner's colour; a click selects the producer and centres the camera.
- **Resource flash**: the row's plate turns RED with black text for 8 graphic ticks.
- **Armageddon clock**: see 11.2. **Minimap**: units attacking and own buildings under attack (`BuildView::under_attack`) blink off
  on `graphic_tick & 8` (the spec's "inferred" batch 3 rule).

### 12.5 Game speed, control groups, profile

- **Speed**: `-` / `=` (and the numpad) step `game_speed` 0..4 (Very Slow, Slow, Normal, Fast, Hyper Fast = `TurnControl::timings` 200,
  125, 67, 50, 1 ms per frame; Hyper Fast runs up to 40 frames per display frame). The speed is presentation (the simulation does not read
  it): `ronmap` paces `Game::do_frame` by `kTimings[_speed]`; headless runs keep one virtual 67 ms step per frame. The clock shows the
  rules' `game_speeds` word, a message announces the change. In multiplayer the keys send SPEED_UP / SPEED_DOWN (52-54) through the lockstep session and the speed follows it (section 10.3); in single player they are the same commands through `CommandManager` (12.8) and stop at Fast, as `process_speed_up` does.
- **HOTKEY** (`apps/map-mac/hotkeys.*`): replay-spec verb 34 is `i32 group, i32 clear, i32 valid, f32 x, f32 y, i32 zoom` and copies the
  current command group (`HotKeyGroups::copy_group`). Since the third pass (12.8) it is a real command (`CommandManager::issue_hotkey`, executed
  by `CommandPackage::process`, table `Game::ctl.hotkeys`); `hotkeys::Groups` only issues it and reads the table back.
- **Profile** (`NSUserDefaults`, interactive window only): resource panel flags, mini library open, help verbosity, progress buttons and
  game speed are read when the window opens and written when one changes.

### 12.6 Localised strings

`--lang CODE` (en, de, es, fr, it or the suffix .7 .10 .12 .16), else `RON_LANG`, else the profile's `Language` default, else English
(`uistr::choose`). The language selects the localised data files (`options.xml.N`, `help.xml.N`, `translated_strings.xml.N`, `typenames`
through `load_rules`' `lang_suffix`) and `uistr::tr(english)` translates the port's own literals through `string_lookup.xml` (the hash
of the English text in any category), used for the clock's speed word, the market titles, "Construction n%", the tooltip titles and
captions. A literal with no entry stays English; the literals without a stock line are in English in every language: the new-age, tribute and
"Click to ..." lines, the "Game Speed:" message (translated when the install has the string) and the drag / progress hints.
Checked with `--lang de` (`ui2_lang_de.png`, `ui2_market_tip_de.png`: the German nation / age caption, "Marktplatz", "Verk.:" / "Kauf.:" and
the German help text).

### 12.7 Gaps this pass leaves (also section 6)

(Closed by section 13: the air garrison panel, the library's final techs and category buttons, `Group::compute_form` for the preview, the
group-by-type multi-selection.) Still open: the options' help for the orders the card does not offer yet; `type vslot 0x104` in the do_damage threshold. The Market's row-head and arrow positions and the
library's overlay choice (owned = `*_overlay1`) are inferred from the art, not decoded.

### 12.8 The command verbs of the HUD (third pass, 2026-10-24; engine side in sim-implementation.md "Orders and commands, second pass")

- **Control groups**: Ctrl+N issues GROUP + HOTKEY (`CommandManager::issue_hotkey`, an empty selection a camera bookmark `issue_hotkey_bookmark`, Ctrl+Shift+N clear); the verb runs in `CommandPackage` and fills `Game::ctl.hotkeys[leader * 18 + n]`
  (digits 0..9 here; F1..F8 = 10..17 are in the table, not bound yet); N reads the table (`hotkeys::Groups::slot`). The group is therefore recorded, replayed and networked; a dying unit or building leaves its groups (`HotKeyGroups::kill`).
- **Game speed and pause**: `-` / `=` and the pause key issue SPEED_DOWN / SPEED_UP / PAUSE as commands in single player (multiplayer sends the same verbs through the session); `ronmap` reads the resulting `Game::ctl.speed` (the keys reach Fast; Hyper Fast comes from `--speed 4` / the profile).
- **Garrison grid**: a click issues COME_OUT (verb 49) for the first own unit of the clicked type, a right click for every unit of that type (`IFaceGarrison::on_button_clicked / _right_clicked` @805f30 / @805e50 -> `OPTION_EJECT` -> `UnitOut::issue_come_out`).
- **Chat line**: Enter / Shift+Enter (everybody), Ctrl+Enter (allies), Alt+Enter (enemies), or the bar's chat button, open a one-line edit box above the bar (`Hud::draw_chat`); Return sends a CHAT command to the recipient mask, Esc drops it; the received line appears through the script
  presentation's chat plate (`ScriptEvent::kChat`) on the machines whose seat is a recipient. `ronmap --chat "TEXT@FRAME"` / `--chat-draft "TEXT@FRAME"` type and send / leave open (the line reads "Say to everybody: hello there_" with a blinking cursor).

## 13. The fourth HUD pass (2026-10-24)

Decompiles: `re/decomp/ui_air` (IFaceAirGarrison, `Options::exec` air cases, scramble / recall / repeat / select_air / picked_spot),
`re/decomp/ui_air3` (`Options::do_*` of the targeting options, stance / formation / disband; `UnitOut::make_options` @00615fc0 as a
listing, its decompile fails), `re/decomp/ui_sel` (IFaceSelected `update_selected` / `on_button_clicked`, `GroupData::group_air`,
`do_all_current`, `do_select_damaged`, `do_filter_onscreen`, `do_cycle_research`). The logic is in `apps/map-mac/uix.*` (C++ over a
small `Ctx`: the game, the issuer, the selection lists, the pending targeting option), the drawing in `hud_air.mm`; `main.mm` only builds the `Ctx`
(`uixCtx`) and calls in at the points marked "uix". The commands are the lockstep verbs (`CommandManager::issue_*` through the
multiplayer `CommandSink`'s own `process_turn`).

### 13.1 Air garrison (IFaceAirGarrison)

- **When**: a single selected airbase or carrier of the viewer or an ally (the original: airbase always, carrier "only when it is the only
  thing selected"); silos get no panel. Right of the selection panel in the garrison's rectangle (`Hud::garrison_rect`).
- **Data** (`uix::air_garrison`): the planes inside by family (fighters = the biplane line for an airbase, fighter-bombers for a carrier;
  bombers; helicopters) and by state: the column is "ready" when `unit_mana_left == unit_mana`, else "fuelling" (`update_visibility`:
  `iVar10 != iVar11`); count and mean fuel per cell; the family icon is the leader's current upgrade of the line.
- **Layout** (`Hud::air_layout`; the XML window is `IFACEAIRGARRISON`: `big_button` 60 x 40, `small_button` 50 x 30, `meter` x3 59 x 32,
  `small_meter_ready` / `_fueling` 50 x 15, spacing 2): wide (>= 126 px): two columns, the `meter` headers over three rows of 60 x 40 buttons,
  the helicopter row only when there are some; narrow (the 1024 px case, about 58 px): one column of 50 x 30 buttons, a block per state (the ready
  header, fighters, bombers; then the fuelling header and the same), and with helicopters inside a single block of three. (The decompile's
  "switch at 101 px" is its own button width; the port switches where two 60 px columns stop fitting.) Disabled (grey) for allies and empty
  cells; the count in digit set 1; the fuelling cell also draws the mean fuel as a 4 px bar (the original only draws the headers; the task asked
  for fuel bars).
- **Clicks**: `IFaceAirGarrison::on_button_clicked` runs `OPTION_FIGHTERS` / `BOMBERS` / `HELIS` (0x27..0x29; `ALLAIR` 0x2a from the keys
  `Alt+F` / `Alt+B` / `Alt+H`), which `Options::select_air` turns into `GroupData::group_air`: the selection becomes the container's planes of that family
  (ready and fuelling alike). The card of a plane selection then offers what `UnitOut::make_options` gives it. Hover: the option's name
  and help.xml `AIRFIGHTERS` / `AIRBOMBERS` / `AIRHELIS` plus `AIRREADY` / `AIRFUELING`; the headers (spot option 0xb1) show the same
  column help.
- **Card** (`BuildOut::make_options` @00629ed0, `UnitOut::make_options`): an airbase: SCRAMBLE (0,3), REPEAT (1,3, lit while `REPEAT_AIR`),
  RECALL (2,3), and the air mission (AIRSTRIKE) on the first free slot of row 1 (the original has no such button on the airbase; the right
  click and the panel's selection reach the same orders there, the button is the launch-patrol / flight entry the task asked for); a silo with a missile inside:
  ATTACK_GROUND (2,0); a carrier: AIRSTRIKE (1,1), RECALL (2,3) (its REPEAT is a unit mask; no verb). A plane selection (airborne or inside a base): AIRSTRIKE
  (PATROL for a plane that cannot attack) (0,0), TRANSFER (2,0) unless fighter-bomber, RETURN (0,3) when airborne; ATTACK_GROUND (1,0) for bombers with the
  Advanced Menu.
- **Orders**: SCRAMBLE / RECALL / RETURN issue `SCRAMBLE` / `RECALL`; REPEAT `BUILDMASK 0x80`; AIRSTRIKE and the targeting options (PATROL, GUARD,
  ATTACK_GROUND, TRANSFER, AIR_PATROL) set `_targeting` with the instruction line (translated_strings #2983 / #2984 / #2988 / #2989 / #2991) and the click
  ends them (`picked_spot`): AIRSTRIKE on an enemy object = `FLIGHT` attack (orders 10; a building / carrier group launches planes: `action_launch_flight`),
  on the ground = `LAUNCH_PATROL` for a container group, `PATROL` for planes; TRANSFER on a friendly airbase / carrier = `FLIGHT` orders 1 (rebase);
  ATTACK_GROUND = `ATTACK_GROUND`; PATROL = `PATROL`.
- **Right click** with aircraft selected (`uix::right_click`): an enemy object = `FLIGHT` attack; an own / allied airbase or carrier = `FLIGHT` rebase;
  the ground = `PATROL` (the formation move of an air group). With airbases selected the rally point is **gather action 3** (a map point the airbase's planes patrol:
  `group_action_gather_point` action 3 -> `airbase_gather_added`), other buildings keep action 0.
- Keys bound: `G` / `Y` / `E` (airbase), `Alt+F` / `Alt+B` / `Alt+H`, `A` (airstrike), `T` (transfer), `Y` (return), `Ctrl+A` / `Alt+F` / `Y` / `E` of the carrier rows,
  `B` (bombard); the aircraft / land-unit key contexts (-5 / -6) now test the selection.
- Checked: `ui3_air_garrison.png` (europe 0:2071, three biplanes ready), `ui3_air_garrison_mixed.png` (`RON_AIR_TEST="0:0:4:100,0:1:2:40,1:0:3:100,1:1:1:15,2:0:2:100"`
  fakes the counts: `family:column:count:fuel`), `ui3_air_garrison_1024.png` (the narrow single column), `ui3_hud_1920_air.png`,
  `ui3_air_select_click.png` (a click on the fighters selects the three planes inside), `ui3_air_scramble.png` (the aircraft card after `G`),
  `ui3_air_launch_patrol.png`, `ui3_air_flight.png` (a right click on an own airbase rebases: `RON_AIR_TRACE=1` shows the landing at 2072),
  `ui3_air_tip_fighters.png`, `ui3_air_tip_header.png`. `RON_UIX_TRACE=1` prints every command uix issues.

### 13.2 Library: final techs and the category column

- **Final techs** (`IFaceLibrary::on_redraw` / `update_visibility` / `on_button_redraw`): when `has_preq(579)` (Information Age, Computerization,
  Globalization, International Law, Selective Service), the game rules are not 11 and flag bit 31 is clear, the grid is replaced by `jep_back0` and four
  `final_jep` buttons (74 x 157) for the final techs 579..582 (military, civic, commerce, science; colours HOT_RED, CYAN, GREEN, YELLOW). Researched =
  the bright art (`jep_tech_button`), the star and the complete overlay; in production = the dim art (the next 158 px row of the sheet), the overlay and
  `jep_bar` / `jep_bar_fill` filled to the progress in the line's colour; available = the dim art with the overlay; otherwise only the hover shows the bright art.
  The overlays' rectangles are relative to the button (not the window). A click queues the tech at the first Library (`OPTION_RESEARCH`, as the grid);
  an unaffordable one flashes. `RON_FINAL_TEST=1` forces the view for snapshots (`ui3_library_final.png`: line 1 owned, 2 in production, 0 and 3 available).
- **Category column** (`cat_age` 34 x 38 and `cat` 0..3, 34 x 28, at the left of the grid): a click is the mini library's: the next age / the next tech of the
  line (`ActionKind::MiniLib`); the next tech in production fills the cell from the bottom in the line's colour, a researchable one gets a hover frame; the hover is
  OPTION 0xaf "Category of research" with the tech's price and help (`ui3_library_cat.png`: the click queued Nation-in-Arms at 9 %).

### 13.3 Selection

- **Object buttons** (`IFaceSelected::on_button_clicked` -> `OPTION_OBJECT`, options.xml 0xa7: "Click to select this object, CTRL-Click to remove,
  SHIFT-Click to select only objects of this type"): click = only this object, Ctrl = remove it, Shift = keep only the objects of its type.
- **Group by type** (pref 0x20000, `update_selected`): when the selection does not fit the 60 x 35 cells (nx = (W + 2) / 62, ny = (H + 2) / 37) and holds several types
  that fit as one full-size button each (column-major, in type order: units 50.., buildings 414..), one counted button per type replaces the individual ones (digit
  set 1, bottom right; no health bar). A click keeps only that type (Shift as well), Ctrl removes the type (`Options::exec` OPTION_OBJECT with a negative
  object). One type, or too many types, keeps the individual buttons shrunk by k. Default on (`GroupByType` in the profile, `RON_GROUP_TYPES=0` off) -- the
  original's default is not in the exe's tables, the pref only matters on overflow (`ui3_group_by_type.png`: 15 types, "Citizen 209").
- **Select all of the current types** (`OPTION_ALL_CURRENT`, `Home`, options.xml 0x78 "Select all of Currently Selected Types"; `Options::do_all_current`): every
  active object of the viewer whose type is in the selection (128 at most). Double-click and Tab: a double click already selected the on-screen objects of one owner and
  type (section 4); `Tab` is `OPTION_CYCLE_RESEARCH` in the key map (not select-all): the next research building in the list Library, University, Temple, ..., Silo, Airbase
  is selected and centred (`ui3_tab_cycle_research.png`, `ui3_all_current.png`). `Ctrl+Home` selects the damaged soldiers, `Shift+Home` keeps the damaged, `Alt+Home`
  the on-screen ones (`do_select_damaged` / `do_filter_onscreen`).
- **Box filters** (ui-spec 4.1, "Prefer Military" profile pref `PreferMilitary`, default on, `RON_PREFER_MILITARY=0` off): a box with soldiers drops citizens, scholars,
  caravans and merchants; Ctrl flips the preference, Ctrl+Shift keeps citizens only, Alt soldiers and siege only, Alt+Ctrl siege only, Alt+Shift aircraft only; a filter
  that would empty the selection is not applied.

### 13.4 Advanced orders, queue keys

- **Advanced Menu** (pref bit 0x2, `AdvancedMenu`, the bar's Advanced button or `RON_ADVANCED=1`): the card gains the "A" rows of `UnitOut::make_options`: GUARD (3,0), PATROL
  (4,0), ATTACK_GROUND (0,1) for siege, STANCE (2,2), FORM (3,2), DISBAND (4,3). STANCE and FORM are EXPLICIT buttons: the icon is the `combat` / `worker` / `caster` /
  `packer` / `form` piece of IFACEOPTIONS for the group's most common option, tinted ORANGE when mixed (`ui3_advanced_card.png`: defensive stance after `Shift+E`, line formation).
  A click cycles (`STANCE` -1 / `FORM` -1; the key `Shift+T` / `Alt+F`), the keys `Shift+A/E/G/R/Z/H` and `Alt+N/V/W/Shift+R/Shift+L` set one. PATROL / GUARD / BOMBARD use the targeting
  mode (`ui3_patrol_targeting.png`: the instruction line, only Cancel on the card).
- **Queue and infinite**: `\` (`OPTION_QUEUE`) removes the last queued item of the selected buildings (Shift x5, Ctrl all: `UNQUEUE`), `Q` (`OPTION_INFINITE`) toggles `BUILDMASK 0x40`;
  the selection panel's infinite toggle is now a button (hover = options.xml 0x40). `Ctrl+Delete` = DISBAND_ALL, `Shift+R` = CLEAR_GATHER (`ui3_infinite_queue.png`).
- **Formation drag preview** (`uix::formation_slots`): `Group::compute_form` on a temporary group of the selected own non-citizen, non-air units; the routine writes
  the game's `forms` scratch, so that entry is saved and restored (the HUD leaves the game as it found it). The preview rings are the real slots
  (`ui3_formation_compute.png`).

### 13.5 Layout check (1280 x 800, 1920 x 1080)

Snapshots read: `ui3_hud_1920_air.png` (bar, resource / market / infrastructure panels, air panel, card), `ui3_hud_1920_fortress.png`, `ui3_hud_1280_market.png`,
`ui3_hud_1280_streamlined_air.png` (the streamlined layout draws only the selection's button and the card: the air panel is not shown there, as the garrison grid),
`ui3_hud_1280_menu.png` (game menu over the HUD). No regression found; the one fix was the air panel's top inset (the third row of helicopters touched the bar at 1280).

### 13.6 Harness added

`RON_AIR_TEST="family:column:count:fuel,..."` (fake counts for the air panel), `RON_FINAL_TEST=1` (force the final-techs view), `RON_ADVANCED=1`, `RON_GROUP_TYPES=0|1`,
`RON_PREFER_MILITARY=0|1`, `RON_UIX_TRACE=1` (every command uix issues), `--key "Home@5"`, `--key "Tab@3"`, `--option 36@2` + `--mouse down/up` for a targeting click, `--mouse rdown/rup` for the
right click orders; `--no-fog` so the clicks find buildings.

### 13.7 Multiplayer window pass (2026-10-24)

Method: the front end is driven through its real hit boxes by `RON_MP_DEMO_UI=PREFIX ronmap GAME --menu mp-lobby --snapshot x.png` (two `FrontEnd`s in one process: the host's
lobby with a thread guest "Marcus", and a second front end that joins it as guest "Ayla" with `mpDemoJoin`), steps in `RON_MP_DEMO_SCRIPT` (`g:X,Y` / `h:X,Y` click on the guest's /
host's screen, `gs:NAME` / `hs:NAME` snapshot `PREFIX_NAME.png`, `w:MS`, `st` prints both screens' state). Games: headless `--host` / `--join` processes on 127.0.0.1:27450+ with
`--snapshot`. A *windowed* two-machine session could not be driven (computer-use access was declined, and a windowed ronmap started from a plain shell does not come up), so the
in-game HUD chat box was only checked through `--mp-act chat=` / `team=` (the CHAT verb path), not typed in.

| Flow | Result |
|---|---|
| Host creates a room (Multiplayer > Host), LAN address shown in the lobby | PASS (`ui_host.png`) |
| Guest joins (Join on the connect screen path = `Room::join`), host chat "Ayla joined the room." | PASS |
| Team / colour / handicap / nation changes of the guest mirror on the host (rows + chat lines "Ayla now has team 2.", "green", "Skill +2", "the Maya") | PASS (`ui_host_seats.png`) |
| Host's settings (map, rules) shown read-only on the guest | PASS (`ui_guest.png`) |
| Lobby chat (guest lines reach the host with the sender's colour) | PASS (`lobby.png`) |
| Ready / Not ready, host's "Waiting for ready" until all guests are ready | PASS |
| Watch only / Play instead (guest becomes an observer: "Watching: Ayla", seat becomes AI 3, START humans `1 1 0`) | PASS (`ui_obs_guest.png`) |
| Kick: the kicked window returns to the Multiplayer screen with "The host removed you from the room."; the host's room shrinks to 2 humans | PASS (`ui_kicked_guest.png`) |
| Kick with block: the same name is refused on rejoin | PASS (Room level) |
| Guest Leave: host stays in the lobby with 2 humans; host Leave: the guest gets "The host closed the room." | PASS |
| Start countdown: guests see "The game starts in 10 seconds..." in the chat, host's button reads "Cancel (n)", a guest turning Not Ready stops it ("The countdown stopped: a player is not ready.") | PASS (`ui_guest_count.png`, `ui_cancel_host.png`) |
| START: both front ends call `frontEndPlayMultiplayer` with the same seed, 3 nations and the same humans vector, their own slots | PASS |
| 4 machines (3 players + 1 observer), chat / team chat / faster / slower / pause / resume, 300 frames | PASS (hook note below) |
| Observer view ("Observing: you see the game from Host's seat..."), observer chat out of band | PASS (`game_observer.png`) |
| Out-of-sync recovery notice ("Back in sync at frame 157 (recovery 1); the game goes on.") with `--mp-diverge 150` | PASS (`game_host_recovered.png`) |
| Conquer the World battle hosted from a campaign (`--ctw-mp --ctw-battle alexander:auto`), host and guest end with the same campaign hash | PASS |
| Synchronized save (`save@6`) writes byte-identical files; the host reloads it (`--mp-load`) and the guest receives it | PASS |
| Lobby "Load Saved Game..." clicked through the real app delegate (fifth pass: `RON_MP_APP_TEST`, below): "Saved game mp_host.ronsave, frame 55: it needs 2 players in its human seats", Start Game, the window game continues from frame 55 in lockstep with a headless guest | PASS (`app_load_lobby_loaded.png`, `app_load_game.png`) |
| Lobby "Conquer the World..." through the delegate: campaign map, Play, the battle in the window with a headless guest (`--ctw-mp --join`), end -> "The battle is over: won." on the campaign map with the room kept, guest receives the campaign; the host's recording is written and replays in lockstep | PASS (`app_ctw_ctw_list.png`, `app_ctw_ctw_map.png`, `app_ctw_battle.png`, `app_ctw_back.png`) |
| In-game HUD chat box typed into: Return opens it (key map), characters as key events, Return sends; `/t text` to the allies, `/w name text` to one player, 3 machines (host, 2 guests in a team): each machine shows only the lines for it | PASS (`chat_host.png`, `chat_ayla.png`, `chat_marcus.png`) |
| Join by host name (`localhost:PORT` from `--join`, headless guest "Marcus" in the chat run) | PASS |

Fifth pass (2026-10-24), how the two NOT RUN rows were run. **Chat:** `ronmap ... --mp-type "TEXT@FRAME"` (repeatable) delivers Return, TEXT character by character and Return to
`handleKey:` as `NSEvent` key-downs at that game frame (headless `--host` / `--join`, with `--snapshot`; every machine prints the lines its console holds with the recipient masks:
`from 1 to mask 6`, whisper `mask 5`). The in-game box now understands `/t` and `/w NAME` like the lobby's. Results: Marcus (team with Ayla) sees the broadcast, the team line, his own line
and the whisper to him, not "hi host"; the host sees no team line; Ayla sees no whisper to Marcus. **Delegate:** `RON_MP_APP_TEST=PREFIX RON_MP_APP_SCRIPT="w:MS;c:host;snap:x;c:load;c:start;
ctw:CAMPAIGN:LINK;ctwplay;ctwend:1;st;quit"` (windowed `ronmap --menu multiplayer`, the host's real `MapAppDelegate`; clicks go to the front end's mouse handlers at the control's centre
(`mpDemoPoint`); the window is drawn by a timer because a window started from a script is not visible to the display link; the guest is a headless `ronmap --join`).
Test seams: `RON_MP_PICK=PATH` answers the file chooser, `RON_MP_NAME` / `RON_MP_PORT`, `RON_MP_NO_COUNTDOWN`, `RON_RECORD_DIR`, `RON_MP_SAVE_DIR`. The campaign chosen with `ctw:` stands
for the campaign list's New / Load clicks (those screens are `ctw_ui`, checked in section 10); Play is the screens' PlayBattle action consumed as `ctwTakeAction` does. Run in the
fifth pass on the final build (the whole table above PASS; ports 27460+). Found: the connect screen's Host / Join buttons did not work with the mouse (list-row test swallowed ids 2100+): fixed.

Fixed in this pass: `--menu NAME` as the last argument crashed the app at start (`std::strcmp(argv[argc], ...)` in the screens loop, `more` was stale after the match), so
`ronmap GAME --menu multiplayer` never opened; `Session::lockstep_verb` speed guards (below).

Test-hook note: `--mp-act "pause@5,resume@6"` loses the resume (the act fires while the pause is still in flight, `paused()` is false, the act is dropped) and the game stays paused;
space the acts at least two turns apart.

Speed guards (`engine/net/lockstep.cpp` vs `engine/sim/commands_extra.cpp`): `process_speed_up` acts only below Fast (speed < 3), `process_speed_down` only above Very Slow, all three
verbs do nothing when `Game::multiplayer && (info.flags & 0x100)`. The session accepted `SpeedUp` up to Hyper Fast (4), so its clock could run at 1 ms while the game's `ctl.speed` stayed at
3, and it ignored the lock. Now `kSpeedUp` needs `speed_ < 3`, `kSpeedDown` `speed_ != 0`, and `SessionConfig::speed_locked` (set by `Runtime::ready` from the game's flags) makes `set_speed` a no-op.

## 14. Army groups (2026-10-24)

The player's complaint: armies scatter. `ronmap`'s control groups 0..9 now carry a name, a HUD button each and a regroup order; nothing
new was added to the simulation (an earlier draft kept named groups in a second HOTKEY table, `Game::ctl.named`; it was removed when
the two were unified, so the original's 9 x 18 `hotkey_groups` layout and the replay encoding are exactly the original's).

- **Making one**: Ctrl+n as before (section 12.8), or with soldiers selected (own units that are not citizens, scholars, caravans,
  merchants or aircraft; buildings and civilians in the selection are left out, nothing military selected does nothing) **G** opens a
  small dialog (`drawGroupNameDialog`) for the first free number (1..9, then 0), prefilled with "Grupo n"; the keys go to its text field
  (Return confirms, Esc cancels, Delete edits, up to 24 characters), the mouse is ignored and the single-player simulation pauses like
  the other modal windows. Confirming issues the selection as GROUP + **HOTKEY slot n** through `hotkeys::Groups::issue` /
  `CommandManager::issue_hotkey`, exactly what Ctrl+n sends, so the group answers to its number key, is recorded in replays, networked
  in lockstep and pruned by `HotKeyGroups::kill` when a member dies. The card's **Agrupar** button (below) does the same as G.
- **Names** are presentation data in the window (`_groupNames[10]`): a Ctrl+n group shows as "Grupo n" until renamed; assigning
  another army to a number drops its old name, erasing the group too. The port's save does not store the control-group table (the
  original's does: `docs/state-spec.md`), so neither the members nor the names survive a save / load yet.
- **Panel** (`drawGroupPanel`, `UiRenderer` quads and the 14-point font): one 150 x 22 button per control group that holds units (not
  bookmarks or building groups) down the right edge under the top strip (y 150, 10 px from the edge), in key order 1..9, 0: "n  name"
  on the left, the living captain count on the right; the selected group's button is orange and a **Reagrupar** button follows the
  list. Left click recalls the living members as the selection (as the number key does); a second click within 500 ms, or a click
  while the group is already the selection, also centres the camera on the members' centroid. Shift+click opens the rename dialog.
  Right click erases the group (HOTKEY clear, the name dropped). A group whose members all died disappears. G with a group selected
  (the selection's captains equal its living captains) opens the dialog as a rename.
- **Command card**: with soldiers selected the card's slot (2, 0) is the army-group button (`hud::kOptGroup`, a port option index
  outside the original's enum) with the icon `apps/map-mac/resources/group-icon.tga` (61 x 39, found like the menu logo: the bundle's
  Resources, next to a dev build, or this source tree; `UiRenderer::file_texture` accepts such an absolute path), its tooltip
  "Agrupar: ..." (G) or "Reagrupar: ..." (Shift+G) when the selection is a group; without the file it is a text button.
- **Stance and formation always on the card**: for own soldiers (a combat stance list, attackers for the formation) the card shows
  STANCE (2, 2) and FORM (3, 2) without the Advanced Menu (`uix::unit_extra_options`); the original keeps them behind it. A click
  opens the choice as before (the formation's spacing / facing comes from the right-drag). Citizens' stances, guard, patrol, bombard
  and disband stay advanced-only.
- **Regroup** (`regroupGroup`, the panel button, the card button or **Shift+G** with the group selected): one `issue_move_to` for the
  whole group to the centroid of its living members. `Group::action_move_near` lays the members out in their formation around that
  point (`docs/movement-spec.md` section 11) and they walk there as one group move. A right-click move of a group is the same group
  order (`commandMoveAt` -> one MOVE_TO per owner) with form KEEP when two or more units are selected (2026-10-25: the player's
  own arrangement, cavalry on the flanks and archers behind, walks as a body and turns with the travel direction; the original's
  line and the other formations come from the right drag or the card), so the formation and the group speed apply: `Unit::do_group_move` already holds
  `Group::speed` / `new_speed` to the slowest member (`gr.march`, the general's march speed) in `engine/sim/groups.cpp`.
- **Keys taken**: G (OPTION_GARRISON's key in the key map) now opens the dialog whenever soldiers are selected; the garrison order
  stays on the card. Shift+G (STANCE_STANDGROUND) regroups only while a group is the selection; otherwise it reaches the key map.
- **Harness**: `--group NAME@FRAME` (the dialog confirmed with NAME), `--recall-group N@FRAME`, `--regroup N@FRAME` (N the key
  number); the console lines `group N "..." assigned / recalled / regroups / erased` report what ran. Snapshots in `re/previews/`:
  `groups_panel.png` (the panel with a group selected), `groups_dialog.png` (the dialog), `groups_scattered.png` /
  `groups_regrouped.png` (before and after the regroup), `card_reagrupar.png` (the card button).

## 15. The fifth HUD pass (2026-10-24)

(Numbered 15: section 14 is the army groups, written by another pass at the same time.) Phase 6's leftovers: the carrier REPEAT and the
AUTO_EXPLORE buttons, the streamlined air panel, and the Options page with a preferences file. Decompiles in `re/decomp/ui_hud4`
(`IFaceMain::position_windows` / `update_visibility`, `IFaceMainBase::update_visibility`, the `OptionsWin*` family, `IFaceOptions`),
`re/decomp/ui_air3/UnitOut_make_options@00615fc0.c` (the unit card, read as a listing), `re/decomp/aircraft3/GroupData_get_auto_explore`.

### 15.1 REPEAT (carrier) and AUTO_EXPLORE on the command card

Both are unit masks, issued with `CommandManager::issue_unitmask` (verb 32, `group_action_unitmask`: the first unit's bit decides, the rest
follow), through the same `issue` path as every other card command, so they are recorded, replayed and sent by the multiplayer sink.

| Button | Slot | Offered when (`UnitOut::make_options`) | Lit when | Key / tooltip |
|---|---|---|---|---|
| **REPEAT** (`OPTION_REPEAT`, 0x2c) | (1,3), where other units have GARRISON | a single carrier of the viewer (type `is` 0x15f; the garrison option is skipped for it, @0061675b), next to AIRSTRIKE (1,1) and RECALL (2,3) | `has_repeat_air` (mask `0x200000`) | `E` (`OPTION_CARRIER_REPEAT`); options.xml "Repeat Orders" + help.xml `AIRREPEAT` |
| **AUTO_EXPLORE** (`OPTION_AUTO_EXPLORE`, 0x21) | (0,2) | every selected unit that is not a fixed-wing plane (a helicopter is allowed: the inlined `is_plane` is `domain == air && !(unit_flags & 0x20)`), and, without the Advanced Menu, is not a caravan | `GroupData::get_auto_explore`: one unit = its mask `0x100`, several = the majority; ORANGE while the selection is mixed | `Ctrl+E`; options.xml "Auto Explore" + help.xml `AUTOEXPLORE` |

The decompile does not restrict AUTO_EXPLORE to scouts: the original offers it to citizens and soldiers as well (the card of a citizen
shows it at (0,2)); the sim skips planes (`group_action_unitmask`) and the AI's explore think already reads the mask. The brief's wording
("scouts / explorers") is the unit most players use it on, not a rule in the exe. The mask buttons use the card's usual latched look
(grey factor 0xa4, section 3); the original plays `S_AUTO_EXPLORE_ACK` / `_OFF` and `S_REPEAT_ORDERS_ON_ACK` / `_OFF_ACK`: the unit
acknowledgement sounds of the port are not wired to the card yet. `main.mm` only skips the generic GARRISON button for a lone carrier
(`uix::lone_carrier`). No stock scenario has a carrier: `RON_CARRIER_TEST=WHO:O` turns that unit into a Carrier (type 351) for snapshots.
Checked: `ui4_carrier_card.png` (REPEAT lit after the click, AIRSTRIKE / RECALL / AUTO_EXPLORE), `ui4_carrier_repeat_tip.png`,
`ui4_auto_explore_tip.png`, `ui4_auto_explore_on.png`; `RON_UIX_TRACE=1` shows `unitmask issued`.

### 15.2 Streamlined mode: the right slot and the air / garrison panels

- `IFaceMainBase::update_visibility` sizes the streamlined right window from the left window's right edge to the minimap
  (`right_window_rect.r - left.r - mid width`): 362 px at 1024, 618 at 1280, 1258 at 1920 (the selection window's `main_streamlined` rect is
  626 wide for its queue strip). The port had the 1024 value hard-coded; `Hud::layout` now uses the minimap's left edge, so the multi-selection
  grid and the end cap follow the window width.
- `IFaceMain::position_windows` (streamlined: `(char)this->opened == 0`) sizes the selection window to `main_streamlined`, then moves the air
  garrison window to **x = 25** of that slot, level with the selection window (the opened layout puts it beside the selection panel). Its width
  is `main width - selection width - display button width - 8`, which in this layout is far under 101 px, so `draw_small` is set: streamlined
  always uses the narrow layout of section 13.1 (one column of 50 x 30 buttons, the ready block over the fuelling block). `Hud::garrison_rect(air)`
  returns that rect (58 px wide plate for the air panel, the 294 px of the garrison window for the plain garrison grid, which had the same
  gap: it used to be laid over the minimap), and the air panel's drawing, hover and clicks no longer stop at `!opened_`.
- With the panel up, the streamlined slot's own portrait / name button (a port addition: the original draws only progress and queue there)
  is skipped, since the panel starts at x = 25 over it.
- Checked: `ui4_streamlined_air_1024x768.png`, `_1280x800.png`, `_1920x1080.png` (europe, seat 0's airbase 0:2071, `--streamlined`), and
  `ui4_opened_air_1280x800.png` for comparison. The decompile's meaning for the window parent / origin of the `x = 25` is the one open point
  (the field names of `Window` in the decompile are ambiguous); the 25 is taken as the slot's own coordinate.

### 15.3 The Options page (`options_ui.*`, `prefs.*`)

`optionswin.xml` (OPTIONSWIN, 880 x 600) drawn with `UiRenderer`: the bronze `border` frame around (0,0)-(880,509), the six tabs (132 x 38 from x = 44;
`tab_*_0` idle, `_1` current; font 14), the area frames (`area_partial` with a title on the top edge, `area_top` continuing it; `area_full` untitled), the
sliders (a grey double track, ticks, the `caret` piece), the check boxes (`check_off` / `check_on`, the next sheet row for the lit state, label at +8),
the GameSpy combo box, the `button_border` strip with the three 258 x 30 `large_button` buttons (hot row = the piece, idle row = one row down, disabled = two,
label font 16, +2,+2 when pressed), the SCROLLCLASS bars of the lists. The tab names are translated_strings #3019..#3024 (Game, Hotkeys, Mouse, Graphics,
Sound, Player); the slider end labels #3025 Slow / #3026 Fast, #3029..#3035 Long / High / Short / Very Slow / Fast / Easiest / Toughest, and the Hotkeys
pane's #3042..#3053 ("System keys may not be reassigned.", "Filter: ", "Commands", "Hotkey Mappings", "Hotkey: ") are the install's, through `uistr::tr`.
OK saves and applies; Cancel drops the working copy (and previews the saved volumes again); **Reset Defaults restores the current pane** (`OptionsWin::do_reset`:
the Mouse pane's `scroll_speed = 6`, drag-military on); Esc = Cancel, Return = OK, Tab / Shift+Tab change the pane. The sounds are `OPTIONS_OPEN` / `_TAB` /
`_BUTTON` / `_CLOSE` in the front end (the game has no global-sound table yet).

| Pane | What is wired | Not offered (no feature behind it in the port) |
|---|---|---|
| **Game** | laid out as optionswin.xml GAMEPANE (2026-10-25): **Auto Citizen** (Idle / Build / Gather / Build and Gather radio group = `LEADER_OPTIONS` peasants stance 3 / 2 / 0 / 1) and **Auto Citizen Delay** (0..5, `peasants_wait`), **Building Stances** (the default stance of new military buildings and units, 0..5), Scouts Auto Transport (option flag 1), Siege units deploy when idle (flag 3), Record games, Game Speed 0..3 (Very Slow..Fast; the game steps with SPEED_UP / SPEED_DOWN commands, a multiplayer game keeps the host's), Difficulty Level 0..5 (the lists' AI difficulty), Help Message Detail 0..3 (0 = help boxes off), **Message Duration** 0..10 (`message_length`), Show Advanced Options, Show Research Progress Bars | friend / foe colours, abilities as group, city names, start paused, auto all-clear / combine, disable cross-network play (no feature behind them) |
| **Hotkeys** | the whole key map (`keymap_table.inc`: 293 rows), filter All / Commands / Build Mode / Building / Unit / City, labels from options.xml NAME or the type name (TRAIN, BUILD, RESEARCH ...), a key capture box, Assign / Remove, the list of commands bound to the key in the box (a shared key is flagged), Esc and F10 refused | mouse buttons and the wheel as keys (the table's `Mouse*` entries show but are not captured), Import / Export (commented out in the XML) |
| **Mouse** | Map Scroll Speed 0..12 (`speed_option`: 0.33 .. 5; applied to the edge scroll), Drag-Select Prefer Military | left-click interface, one-click garrison, right-click execute |
| **Graphics** | window mode (windowed / full screen: `toggleFullScreen`), the streamlined interface (the display button's layout), the window's resolution as a read-only line | the resolution list and the render-element switches (the Metal renderer has no such levels; the window resizes freely) |
| **Sound** | FX Volume and Music Volume 0..255 (live preview while dragging), Allow In-Game Music, Allow Main Menu Music | taunt volume and taunts (there are none), audio ducking |
| **Player** | the interface language (en, de, es, fr, it: the next start; `uistr::choose` reads it after `--lang` and `RON_LANG`) | profiles and player names |

**Preferences file**: `~/Library/Application Support/OpenRiseOfNations/preferences.txt` (`RON_PREFS=path` overrides it for tests): `key = value` lines
(`game_speed difficulty help_detail advanced_menu progress_bars message_length auto_citizen auto_citizen_delay building_stance auto_transport
auto_deploy record_games scroll_speed prefer_military fx_volume music_volume music_in_game music_in_menu streamlined fullscreen language`, and
`score_window` once the Alt+Shift+0 / Alt+Shift+1 keys have been used) and `hotkey.NAME|mode|ctx|param = KEY` for each rebound entry. A key missing from the file keeps the window's own value
(so a fresh install behaves as before, including a scroll step of 1.0 until `scroll_speed` is saved); a saved file names every key. Applied at start:
`loadProfile` (speed, help, advanced, progress bars, prefer military, streamlined), `startInteractive` (volumes, in-game music), the front end's
`startInteractive` (volumes, menu music), `main()` (`keymap::apply_overrides`, before any screen), the app delegate (full screen), `edgeScroll` (scroll
speed); applied on OK: `MapRenderer applyPrefs:` / `FrontEnd applyPrefsToMenu:`. The in-game page shows the running game's own values first
(`Host::live`), then the file's.

**Where it is reached**: Main Menu > **Options** (the new item before Quit; screen `options`), the game menu's **Options** (`_gameMenu == 3`, the game stays
paused or, in multiplayer, running as before; closing returns to the game menu) and **F11** (`OPTION_OPTIONS`). Keys: `keymap.*` gained `count / entry /
keys_of / set_keys / apply_overrides / overrides` (the lookup and the tooltips' hotkey text read the effective keys).

**Harness**: `--menu options` (also added to the `--menu` list) with `RON_OPT_PANE=0..5`, `RON_OPT_HK=OPTION_NAME` (selects that command),
`RON_OPT_COMBO=N` (opens widget N's list, 100 = the Hotkeys filter), and `RON_OPT_SCRIPT="c:X,Y;d:..;m:..;u:..;k:CODE[:CHARS[:cas]];w:DY@X,Y;snap:NAME;state"` (page
pixels; `snap:NAME` writes `<snapshot>_NAME.png`); in a game `RON_GAME_MENU=3` shows the page. Snapshots read: `ui4_main_menu.png` (7 buttons), `ui4_options_game.png`,
`_hotkeys.png`, `_hotkeys_selected.png` (Auto Explore: Ctrl+E, with the other commands bound to it), `_hotkeys_assigned.png` (after capturing Ctrl+Z for "Slower": Assign
done, "Toggle Help" flagged as sharing it), `_filter.png`, `_mouse.png`, `_graphics.png`, `_combo.png`, `_sound.png`, `_player.png`, `ui4_ingame_options.png`,
`ui4_ingame_menu.png`. A scripted run (slider click, check box, key capture, Assign, OK) wrote the expected file and `hotkey.OPTION_SLOWER|0|-1|-1 = Ctrl+Z`.

**The settings with a feature behind them (2026-10-25).** `message_length` feeds `scriptview::set_message_length` (messages last `message_length * 45`
ticks, the banners `message_length * 400 + 500` ms; `script_view.mm` had the constant 5). The Auto Citizen stance / delay, the building stance and the two
option flags are the fields of the simulation's `LEADER_OPTIONS` command (`opt_peasants`, `opt_peasants_wait`, `opt_buildings`, `options_flags` bits 1 and 3,
which `Unit::init` / `Build::start` read): `MapRenderer issueLeaderOptions:` sends the command (through `_cm.issue_leader_options`, so a recording and the other
machines follow) when OK is pressed and once at the start of every game (`notifyFrame`, frame <= 30), and only when a value differs from the leader's current
one; the page shows the leader's own values first (`Host::live`). `RON_UIX_TRACE=1` prints `[opts] leader options peasants=3 wait=4 buildings=2 flags=0x8` and
the values the sim holds afterwards. `record_games` gates the Recorded Games folder of multiplayer games (`lobby.mm` `start_recording`: off = no `.rcx`; an
explicit `--mp-record` path still records). The score window (`Options::exec` 0x87 / 0x90: Alt+Shift+1 = `stat_mode` 0 and prefs bit 4 set, Alt+Shift+0 =
bit cleared) is the `score_window` preference: `OPTION_STAT_NORMAL` shows it (also after a `ui_hide_score`), `OPTION_STAT_OFF` hides it, both play `S_PREF_CHANGE`
and are saved; without the key in the file the game decides as before (Conquer the World, multiplayer). The other `OPTION_STAT_*` modes (economy, market,
military ... diplomacy columns) are not ported. Not offered: profiles and player names, the unfinished switches listed above.

### 15.4 Still open after this pass

- The unit-acknowledgement sounds for the card's toggles; the card's latched look is subtle (no separate "on" art found in `IFaceButton`).
- Options: the unwired settings of the table above (friend / foe colours, city names, start paused, abilities as group, all-clear, combine), profiles / player names, a mouse-button capture in the Hotkeys pane, `Host::sound` in a game.
- The clicks of the game menu's "Options" item could not be driven by the `--mouse` harness (it does not reach the menu); the page itself was exercised through
  `RON_GAME_MENU=3`, the front end's scripted run and by reading each pane.

## 16. Phase 6 finish: the app bundle, performance with 8 players, the card toggles' sounds (2026-10-24)

### 16.1 The app bundle

`bash build-mac.sh --app` builds for this Mac and then runs `package-mac.sh --app-only`, which writes `build/OpenRiseOfNations.app` (the ronmap binary as
`Contents/MacOS/OpenRiseOfNations`, `Contents/Resources` = `AppIcon.icns` + the front end's `menu-logo.png` / `group-icon.tga` + LICENSE / README). `bash
package-mac.sh` alone is the release path: a universal (arm64 + x86_64) build, the app and a `.dmg`.

- `Info.plist`: id `org.openriseofnations.app`, `CFBundleShortVersionString` = `git describe --tags --always` (without the `v`; the VERSION file when there
  is no git), `CFBundleVersion` = the VERSION file (dotted numbers), `LSMinimumSystemVersion` 12.0 (= `-mmacosx-version-min`), `NSHighResolutionCapable`, no
  `CFBundleDocumentTypes`. The icon is `tools/AppIcon.png` (the 1024 px master; `apps/map-mac/resources/menu-logo.png` when it is missing) scaled by `sips`
  into an `.iconset` and `iconutil -c icns`.
- Signing: ad-hoc by default (`codesign --options runtime --sign -`); `--sign "Developer ID Application: Name (TEAMID)"` (or `CODESIGN_ID=`) signs with that
  identity and a secure timestamp. `--notarize` (needs `--sign`) zips the app, runs `xcrun notarytool submit --wait --keychain-profile "$NOTARY_PROFILE"`
  (default profile `ron-notary`, created once with `xcrun notarytool store-credentials`) and `xcrun stapler staple` on the app (and the `.dmg`). It uploads to
  Apple with the user's own credentials, so nothing runs it unless asked: **not run here**.
- First launch (no argument): `launcher::saved_or_guessed_install()` reads `game_dir` from the preferences file (`~/Library/Application Support/OpenRiseOfNations/
  preferences.txt`, `RON_PREFS` overrides), then the NSUserDefaults key of earlier builds, then the usual guesses (`~/Games/RiseOfNations`, a Steam library, CrossOver
  bottles); with none, `ask_for_install` shows an alert and an NSOpenPanel until the folder looks like an install (`Data` plus `scenario` **or** `riseofnations.exe`).
  `remember_install` writes `game_dir` with `prefs::save_game_dir`, which rewrites only that line (the other preferences, and their "present" state, stay as
  they are). The menu's "Change..." button goes through the same call.
- Checked: `RON_PREFS=file build/OpenRiseOfNations.app/Contents/MacOS/OpenRiseOfNations --menu main --snapshot m.png` shows the prefs' `game_dir` in the Game
  Folder box; `open -n --env RON_PREFS=... build/OpenRiseOfNations.app` (LaunchServices, as Finder does) stayed up, found the install and wrote
  `game_dir = ...` into the new file; quit after the check.

### 16.2 Performance with 8 players

Tools: `ronmap ... --perf N` (after `--frames`) runs N headless frames, each one `stepSimulation:1` plus one rendered frame into a reused 1440x900 target, and
prints avg / p50 / p95 / max of the sim step, the CPU encode, the GPU wait and the render total, then an estimated display rate (render + half a sim step: the window
steps the sim once per 66 ms). `ron-sim ... --perf` times `Game::do_frame` alone: without it the harness computes `digest(*g)` for **every** frame (66 % of a
`ron-sim` run, and with `--ai-every 1` it prints each AI line each frame), so the brief's `time ron-sim ... --ai-every 1` measures the harness: europe, 3000 frames,
4.04 s = 1.35 ms per frame; `--perf` says 0.34 - 0.37 ms per frame (worst frame 8 ms in the first 3000, 40 ms in 30000).

8 AI seats: `ronmap <game> --random 12,4,1,8 --all-ai --frames 12000 --zoom 0 --perf 1000 --size 1440x900` (Mediterranean, 80x80, 8 nations, AI against AI;
`ron-sim` has no random-map option, and europe has only two AI seats).

| | sim step | render CPU encode | render GPU wait | render total | est. display rate |
|---|---|---|---|---|---|
| before | 0.554 ms | 2.872 ms (p95 3.41) | 1.757 ms | 4.629 ms | 204 fps |
| after | 0.498 ms | 2.575 ms (p95 2.84) | 1.738 ms | 4.313 ms | 219 fps |

Other points: zoom 5 3.9 ms total, zoom 3 4.0 ms, europe (zoom 0, frame 4000) 5.9 ms + 1.0 ms sim. Offscreen, so no vsync; the target (8 AI at 30 fps or more at 1440x900)
is met by a factor of 6 or more, and the AI-vs-AI game at 12000 frames is still in ages 0 - 1 (about 20 - 40 units per seat), so a late-game test is still to do.

Profile (`sample`, 5 s of the perf loop): the main thread spends 84 % of the encode in `encodeUnits` -> `UnitDrawer::draw` (platform/metal/unit_draw.*, not touched
here): `pose_model` / `pose_node` (engine/assets) is 53 % of the draw and builds strings per node (`std::to_string`, `string::insert`, `tolower`: about 40 % of
`pose_node`); caching the pose per (model, animation, frame) or per-node ids would be the next win. Next: `encodeBuildings` 21 %, the HUD 12 % (`iface_find_rect` /
`iface_layout_children` re-walk the layout XML every frame in `draw_bar`, `draw_minilib`, `draw_top_strip`: cache the rects at layout time), `updateFog` 4 %.
Fixed in `main.mm`: the buildings' hull was re-uploaded as two new MTLBuffers per building per frame (now cached in the `BuildAsset`), the add-ons did the same
(index buffer cached, vertices through `setVertexBytes` when they fit 4 KB, vertex vector reused), and `updateFog` made three `getenv` calls and copied the level
vector each frame (now a compare against the uploaded levels).

Hot spots in engine/sim (not touched; `ron-sim --perf`, europe, 90000 frames, share of the samples by self time): `astar_path` 13.7 %, `valid_ucoord` 12.2 %,
`constant_offset(const char*)` ~19 % (a `strcmp` table lookup by name, called per frame: precompute the offsets), `std::map` insertion in the path search
(`__tree_balance_after_insert` 3.3 %, `SavedSearch::insert_open` 3.0 %, `first_open_node` 1.4 %), `collide_here` 3.1 %, malloc / free ~3 %.

### 16.2b A full-length 8-AI skirmish, 60,000 frames (2026-10-24)

Harness: `ronmap <game> --random 12,4,1,8 --all-ai --frames 60000 --long-run 10000,PREFIX --zoom 0 --size 1440x900` (headless, never opens a window). `--long-run EVERY[,PREFIX]`
steps the simulation one frame at a time to `--frames`, or until a nation reaches the Information Age or `game_over`, and every EVERY frames prints the step timing (wall and this
thread's CPU time, avg / p95 / max), `Game::do_frame` alone (thread CPU), the footprint (`phys_footprint`, resident + compressed), the per-seat census, then runs a 100-frame `--perf`
sample (it advances the game by 100 frames) and writes `PREFIX10k.png / 30k / 60k / end.png`. `--perf` now also prints the thread CPU time of the sim step and of the render encode and the
footprint before / after (the wall clock of a loaded machine lies: during these runs the Mac had load averages of 50 to 300 from other builds, GPU wait included). `RON_LONGRUN_VICTORY=2`
(also read by `ron-sim --random`) makes the game conquest-only: with the default rules a Turkish Pyramids wins by wonder at frame 18041 (20 min, ages 1 - 2; "You lost to a wonder victory", a correct
end), so the late game needs the override. `ron-sim <game> --random STYLE,SIZE,SEED,NATIONS --frames N [--perf]` now exists too (all seats AI; `new_random_game`; the determinism self-check runs as for a scenario).

Result: **no crash, assert, stall or error line in 60,000 frames** (`ronmap`, 3 runs) and `ron-sim --random 12,4,1,8 --frames 60000` (self-check OK: identical digests twice). The game never reaches the Information Age:
at 60,000 frames (66 min of game time) the nations are in ages 2 / 3 / 4 / 2 / 3 / 3 / 3 / 2 (0 Ancient ... 4 Enlightenment), 542 units and 340 buildings in all.

| frame | ages (seats 0-7) | units | buildings | sim step avg wall / thread CPU | `do_frame` CPU | render encode CPU | render total (wall, loaded Mac) avg / p95 | footprint |
|---|---|---|---|---|---|---|---|---|
| 10,000 | 11101111 | 222 | 139 | 2.5 / 0.50 ms | 0.33 ms | 3.99 ms | 17.8 / 36.7 ms | 280 MB |
| 20,000 | 22212222 | 499 | 242 | 2.7 / 0.78 | 0.46 | 5.78 | 37.6 / 84.0 | 208 |
| 30,000 | 23313322 | 599 | 305 | 4.5 / 1.75 | 1.29 | 5.55 | 12.4 / 24.1 | 369 |
| 40,000 | 23423322 | 615 | 338 | 2.2 / 1.71 | 1.29 | 6.51 | 22.6 / 41.1 | 249 |
| 50,000 | 23423322 | 543 | 340 | 2.8 / 1.94 | 1.46 | 4.75 | 7.1 / 9.6 | 253 |
| 60,000 | 23423332 | 542 | 340 | 2.3 / 1.70 | 1.25 | 4.95 | 7.9 / 14.7 | 400 |

Snapshots (read): 10k (11:06, Classical Age, the Spanish corner of the Mediterranean map: farms, woodcutters, two cities), 30k (33:20, Medieval, a Siege Factory completed, a clump of soldiers on the road, food
527 / knowledge 1262 banked), 60k (1:06:40, still Medieval for the viewed seat, food 706 / knowledge 1124 unspent, pop 64/100, a Pyramids-like wonder foundation at the west); the HUD (resources, top strip,
minimap, timer) intact at every size. Worst single sim step 59 - 144 ms wall (22 ms at most in thread CPU time at frame 10k, 6 - 10 ms later: the wall outliers are the loaded machine).

Memory: the footprint does not grow with the frame count: 274 MB at frame 5000 ... 250 - 270 MB at 40,000 - 55,000 (the plain RSS falls to 76 MB at times because macOS compresses it, hence `phys_footprint`). Each `--perf` / `--snapshot`
adds a one-off 270 - 360 MB (render targets and in-flight Metal buffers; the same +270 MB after 100 and after 4000 frames, released by the next checkpoint), so there is no per-frame leak in the render path. The
sim's own growth is the event log (26,671 entries at 60k) and the AI / path data.

Fixes (all outside engine/sim):
- `StateCache::pipeline` (platform/metal/renderer.mm): built a 160-byte `snprintf` key and looked it up in a `std::map<std::string>` **per draw call**; now a 24-byte array of the render state's enum fields (the
  `snprintf` was 15 % of the render CPU in the profile, 450 of 2954 samples).
- `engine/assets/pose.cpp`: `is_pivot_node` / `slot_for_name` allocated and compared up to ~60 strings per node per unit per frame; now an allocation-free parse of "pivot node N" / "ammo node N" / "smoke node N" / "node N"
  (identical results: `ron-check` 802/802 models, 1823/1826 anims, failures 0).
- apps/map-mac/hud.hpp + the four `rect_in` helpers (hud.mm, hud_air.mm, hud_extra.mm, hud_panels.mm): `iface_find_rect` re-resolved the whole window layout on every call; `hud::cached_iface_rect` keeps the result per (window, tag, name, size, nth).
  The HUD is now about 6 % of the encode (190 of 2954 samples; it was 12 %).

Render encode CPU time, same 8-AI game at frame 41,000, both builds running at once on the loaded Mac: before 8.05 - 8.47 ms avg (p50 7.9 - 8.2), after the pose + HUD fixes 5.24 - 6.21 ms, after the pipeline key 4.8 - 6.5 ms in the late game
(a 25 - 40 % cut). With the Mac quiet (frames 50k and 60k above: GPU wait 1.8 ms) the render total is 7.1 - 7.9 ms (target 8); at the other checkpoints the wall number is inflated by the load (GPU wait up to 12.7 ms), not by the encode.
The sim step in the app is 1.7 - 1.9 ms of thread CPU late in the game (`do_frame` 1.25 - 1.46 ms + 0.4 ms of view / HUD bookkeeping), under the 2 ms target; `ron-sim --random ... --perf` 60,000 frames, 2.10 ms/frame wall at load 100+.

Remaining hot spots (not touched):
- `platform/metal/unit_draw.mm`: `draw_mesh` makes a new `MTLBuffer` (`newBufferWithBytes`) per unit per frame: 563 of the 1751 samples under `encodeUnits` (32 %, 19 % of the whole frame, mostly IOGPU resource creation).
  Use `setVertexBytes` below 4 KB (as the building add-ons do) or one ring buffer per frame, and cache the pose per (model, animation, frame) (units that stand still re-pose the same frame every frame). `pose_model` is still 20 % of the frame
  (`pose_node` 322 samples + `sample` 279, the quaternion slerp with `acos` / `sin` per node).
- engine/sim (profile of the sim phase, 18,577 samples at frames 25k - 45k): `do_frame` 62 %, of which `unit_process` -> `work` -> `do_move` -> `find_tpath` 4821 samples (26 % of the main thread, 42 % of `do_frame`; `find_tpath` itself
  2192 self) with `SavedSearch::insert_open` / `__tree_balance_after_insert` / `__tree_remove` / `first_open_node` (the `std::map` open list, about 1700, 9 %), `calc_cost` 297, `valid_ucoord` 286, `invalid_loc` 318; a `strcmp` table lookup in
  `constant_offset(const char*)` called from `process_healing` (551) and `leader_calc_resource_caps` (154), 4 % of the thread (precompute the offsets once); `object_is_seen` / `unit_is_detected` 779 + 296 (from the view fill and the
  fog checks), `get_graft` 243. `find_tpath` is called every frame by units whose move keeps failing or restarting (not proven which); a back-off would be a behaviour change and needs the decompile of `do_move`.
  Outside the sim: `make_view` per step is 10 % of a headless step (one view per sim step is right for the window: one step per 66 ms tick).
- AI behaviour seen in the census (not investigated; engine/sim): seat 7 (Koreans) has **0 citizens** from frame 3000 to 60,000 (39 military, 44 units, ages 2) and seat 3 (Greeks) holds 21 units with 2 soldiers after 66 minutes; the
  viewed Spanish seat banks 700 food / 1100 knowledge unspent in Medieval at 60k, and no nation reaches the Information Age in 60,000 frames.

### 16.3 The card toggles' acknowledgement sounds

From the decompiles (`Options::do_auto_explore`, `set_air_repeat`, `do_transport`, `do_stance`, `do_formation`, `do_halt`, `do_eject_all`, `do_back_to_work`,
`recall_aircraft`, `scramble_aircraft`; `Group::action_buildmask` for the infinite queue); the soundglobals.xml KEYs map through soundfiles.xml:

| Card command | Global played | File |
|---|---|---|
| AUTO_EXPLORE turned on / off (`get_auto_explore` read before the command: off -> ACK, on -> OFF) | `AUTO_EXPLORE_ACK` / `AUTO_EXPLORE_OFF` | `toggleon.wav` / `toggleoff.wav` |
| REPEAT (carrier unit mask, airbase building mask; off -> ON_ACK, on -> OFF_ACK) | `REPEAT_ORDERS_ON_ACK` / `_OFF_ACK` | `toggleon.wav` / `toggleoff.wav` |
| Infinite queue (building mask 0x40; the original plays it from the action, here from the click) | `INFINITE_QUEUE` / `INFINITE_QUEUE_OFF` | `infinitequeue.wav` / `infinitequeueoff.wav` |
| Stance / formation | `CHANGE_STANCE` / `CHANGE_FORMATION` | `but_diplomat-sendmessagebuttons.wav` |
| HALT / EJECT ALL | `HALT_ACK` / `EJECT_ACK` | `targetack.wav` / the install's entry |

`uix::Ctx::sound` (set by `main.mm` from `MapRenderer playGlobal:`, which asks `scriptview::View::sound_file`, now public: a random entry of the key, as `SoundGlobal::play`)
plays them on the effects channel when the command was accepted. The state is read before the command because the command runs later in a lockstep game.
`RON_UIX_TRACE=1` prints `[uix] sound S_KEY -> file` (also headless, where nothing plays). Checked: europe seat 0, `--key Ctrl+E@5 --key Ctrl+E@12 --key H@20`:
`AUTO_EXPLORE_ACK` -> toggleon, `AUTO_EXPLORE_OFF` -> toggleoff, `HALT_ACK`; with `RON_CARRIER_TEST=0:0` and `E@5 / E@12`: `REPEAT_ORDERS_ON_ACK` then `_OFF_ACK`.
Added 2026-10-25: `SCRAMBLE_ACK` (`Options::scramble_aircraft`, after the command is accepted), `RECALL_AIRCRAFT_ACK` for a building group's recall (the airbase card) and
`RETURN_TO_BASE_ACK` for the planes' own recall / return (`Options::recall_aircraft` chooses by a flag of the selection group that the port reads as "buildings
selected"), `BACK_TO_WORK_ACK` (the new BACK_TO_WORK button, below) and `S_PREF_CHANGE` (the Help Text Detail key and the Advanced Options key print the original's
"Help Text Detail: LOW" / "Advanced Options: ON" lines, #3006 / #3010 + #2806 + #2807..#2809 / #2782 / #2780, and play it; the Options page itself keeps its
`OPTIONS_*` sounds, `Options::do_toggle` is the in-game key path). Checked with `RON_UIX_TRACE=1`: a carrier (`RON_CARRIER_TEST=0:0`) `Y` prints `recall issued` and
`sound S_RETURN_TO_BASE_ACK`; `E` still gives `S_REPEAT_ORDERS_ON_ACK`.
**BACK_TO_WORK** (`OPTION_BACK_TO_WORK`, Shift+J; card slot (1,3) next to EJECT_ALL, offered when citizens or militia are inside the building,
`BuildOut::make_options` @00629ed0) issues `EJECTALL` with `back_to_work` (the citizens inside resume their orders; `Options::do_back_to_work`). Not wired:
`TRANSPORT_ON_ACK` / `_OFF_ACK` (the port has no transport toggle on the card: the sim has `SET_TRANSPORT`, `Options::do_transport` is the whole unit-mask card
button of a scout and the card does not offer it yet; the Options page's "Scouts Auto Transport" is the leader option, not that toggle).

## 17. The victory clocks and endings on screen (2026-10-25)

The engine side is `docs/victory-spec.md` (events 17-26 of `Game::events`, `Game::victory_checked`). Decompiles: `re/decomp/endgame/` (`EndGameWin::*`,
`StatWin::*`, `IFaceMainBase::draw_timers` / `draw_game_timer`, `MessageWin::add_simple_message`; `re/jobs/endgame.txt`, `endgame2.txt`) and
`re/decomp/victory/` (`GameDaemon::process_victory`, `Leader::defeat`, `Player::resign` / `drop` / `win` / `lose`, `Game::process_end_game`).
Strings are `translated_strings.xml` entries (index = the decompile's byte offset / 20, section 11.2), looked up through the positional table of the
active language with the English text as the guard, as the rest of `notices.mm`.

### 17.1 Messages, banners and sounds (`notices.mm`, `script_view` `post`)

All messages are `MessageWin::add_message` with RED (`argb 0xFFFF0000`) and the standard duration; a note without text only plays its sound. The sounds are
`soundglobals.xml` KEYs resolved through `soundfiles.xml` by `scriptview::View::sound_file` (a random entry of the key, `SoundGlobal::play`) and played on the
effects channel (`platform/audio`, unchanged). `post` now plays the sound of a banner note too.

| Event (kind) | Text (index) | Condition | Sound / banner |
|---|---|---|---|
| wonder clock started (17) | "X now has N net wonder points!" (#1342; "X and team now have ..." #1341 when `num_team_members(X) >= 2`), then "You will lose unless you capture/build/destroy enough wonders to reduce this score!" (#1343) | the second line unless the viewer is X or an ally | `S_WONDER_VICTORY_START` always; banner `wonder_timer` "Wonder Timer Started!" (#2034, `draw_timers` posts it when a leader first leads the wonders) |
| wonder warning (18; 4500 / 1800 / 900 frames left) | #1345 ("has"; #1344 "and team have") + #1343 | only when the viewer is not an ally of X | `S_WONDER_WARNING` |
| wonder clock stopped (19) | none | | `S_WONDER_VICTORY_STOP`, once per frame (the original plays it once per pass) |
| territory clock started (20) | "X controls N% of the world!" (#1349; "X and team control ..." #1348), then "You will lose unless you take control of more territory!" (#1350) | the second line unless the viewer is on X's team | `S_POP_VICTORY_TIMER_BEGUN`; banner `territory_timer` "Territory Victory: M:SS" (#2035) |
| territory warning (21) | #1349 / #1348 + #1350 | only when the viewer is not on X's team | `S_POP_VICTORY_WARNING` |
| territory clock halted (22) | "X no longer controls N% of the world." (#1347; team #1346) | once per pass, to everybody | `S_POP_VICTORY_HALTED` |
| time limit (23; 600 / 300 / 120 ticks left) | "The game will end in TEN / FIVE / TWO minutes!" (#1353 / #1352 / #1351); under game_rules 8 also "You will win if you can survive until the time runs out!" (#1354, viewer on team 0) or "Your team will lose unless you can eliminate the defender in time!" (#1355) | | `S_TIME_LIMIT_WARNING` (the two-minute line picks a random entry of the same category: `sound_global.lookups[0x9b]` is that key, so the random pick `sound_file` makes is the original's) |
| leader defeated (2, type != 6) | "X defeated." (#2306) | every defeat, also by the end of the game (musical chairs, conquest of everyone, Armageddon) | `S_SOMEONE_ELSE_LOSES` for other leaders (`Player::lose`; the viewer's `S_YOU_LOSE` is the game-over flow's); banner `defeat_notice` with the line for the first other loser while the game is still on |
| resigned / quit / dropped (25) | "X resigned." (#3085) / "X dropped." (#3084) | other leaders | `S_ALLY_RESIGNED` (the leader is the viewer's ally) / `S_ENEMY_RESIGNED`; banner `drop_notice`; the viewer hears `S_YOU_RESIGN` (resign) or `S_YOU_QUIT` (quit, drop) and sees no line |
| musical chairs round (24) | none of its own: the losers print #2306; the clock below shows the next round | | |
| nuke counted (26) | none: the original prints no line for the count (the Armageddon clock counts down; `Game::get_armageddon` reached ends the game) | | `S_NUKE_HIT` (`Nuke::add_nuke` plays lookup 0x50 = `S_NUKE_HIT` at the blast; the count event carries no position, so it plays unlocated) |
| allied victory (kind 4, value 1), conquest, score ... | `Leader::victory` prints nothing | | the end-game window (17.3); `S_YOU_WIN` / `S_YOU_LOSE` come from the existing game-over flow |

`String::parse` has overloads (String x n, int x m): the digit of `$NUMBERn` counts among the numbers, every other placeholder among the strings, so
`notices::fill_typed(text, strings, numbers)` replaces `fill` for templates that mix both. (The old plunder line "City of $CITYNAME0 captured. Plunder: +$NUMBER0
$GOODTYPE1" printed the city name for the number; fixed.) The ally tribute lines of `Leader::defeat` are done (17.7). The names in the victory-clock, wonder and tech lines are `LeaderData::say_full_name`
(17.7), the defeat / resignation lines keep the player's name (`get_name`).

### 17.2 The clocks in the bar (`main.mm` `gameTimers`, `hud_extra.mm` `draw_timers`)

`draw_timers` / `draw_game_timer` were already ported (a 42 x 39 icon column from y = 45, the 60 x 26 `background_timer`, team-coloured borders, the 11 x 20 digits). Checked
against the decompile this pass: the **territory** clock (kind 3, drawn while a leader's `popwin_timer` runs, `popwin_timer - (frame - stamp)` clipped at 0) and the **time
limit** (kind 5: `limit * 900 - frame`) were right. Added: **musical chairs** (kind 7 `musical_timer`: `chairs[chairs] * 900 - (frame - Game.musical_chairs)`, red / yellow
flashing digits under 450 frames), the time limit's flashing digits (red / yellow under 120 frames, white / yellow under 300, by `graphic_tick & 8`; with team style 3 the barbarian
icon, kind 6, shows for those last frames) through `hud::Timer::tint`. The wonder clock follows `Game::wonder_winning` since 2026-10-25 (17.7).

### 17.3 The end-game window (`endgame.*`, `main.mm` kind 2 of the game menu)

`Game::process_end_game` raises `EndGameWin` at the end of the frame that set `victory_checked` (check_victory's end, or the allied victory); `consumeScript` clears the
flag and opens the window at once (a Conquer the World battle keeps its own hand-over: `_ctwEndFrames`; the 5 s fallback of section 10 still opens it when the viewer's own game-over is
seen). Banners are dropped (`EndGameWin::exec` cleans the AnimNotice list) and the "Victory!" / "Defeat!" line is hidden while it is up. The game menu's kind 2 (the old
"Game Over" list) is now this window: its hover / press / pause handling is the menu's, `_gmRects` are the four buttons, Return = Quit, Esc = Examine Map (or back from the table).

**Layout** (interface.xml `ENDGAMEWIN`, 248 x 308, centred; `IFACERENDERMANAGER` `scroll` plate with the pads 8 / 4): `banner_victory` / `banner_defeat` (496 x 94 at -117, 32 of
`iface_resources2.tga`) with `trophy_victory` (gold cup) / `trophy_defeat` (skull and sword) on it; `trophy_arma` (the mushroom cloud, no banner) for Armageddon; title in `title_*`
(font 18 bold: "VICTORY!" #1187 / "DEFEAT!" #1188 / "ARMAGEDDON!" #1189), the reason in `message_*` (174 x 63, font 14 bold, wrapped and centred in both directions); two boxes (`time`, `score`,
`small_border`) with "Game Time: " #1193 (H:MM:SS) and "Your Score: " #1194 right-aligned against them; four TEX_DIPLO buttons (`butnormal` / `buthighlight` / `butdown`, 12 + 149 + 12) from
y = 168, 35 apart: **Examine Map** (closes; the game stays on screen), **Game Stats** (this port's score table, below), **View Playback** (disabled: `update_visibility` enables it only
with the replay preference outside scripted / multiplayer games, and the port records no playback), **Quit** (`CommandManager::issue_quit`: back to the main menu; "Continue to Strategic Map"
#1114 in a Conquer the World battle, where it hands the result to the campaign). The XML buttons are 250 wide and the window 248: they are fitted to the window.

**The sentence** (`EndGameWin::get_type_string`, transcribed in `endgame.mm` `sentence`): from the mode (0 victory when the viewer is not defeated, 1 defeat, 2 Armageddon when
`armageddon >= get_armageddon`), the winner's `victory_type` and the losers' `defeat_type` (the scan of `EndGameWin::exec`: the first loser's type, 0 when a second non-resign loser follows), the
instant flags (`kVictoryFlag` / `kDefeatFlag`: the World Government variants) and whether it is a team game (`num_team_members > 1`, or the winner's team in a defeat). The index table:
victory #1160-#1176 (conquest, capital, sudden death, score at the check, resigned / dropped opponents #1165-#1167 / #1121, wonder #1169-#1170, territory #1171-#1172, tech #1173, score #1174,
economic #1175, time limit #1176 / #1131), team variants #1116-#1132, defeat #1134-#1142 and #1177-#1178 (conquered, capital, sudden death, lowest score, resigned, objectives, "lost to a wonder / territory / technology /
score / economic victory" #1179-#1185, time limit #1186), another / the other team #1143-#1158, CTW battle #1133 / #1141. The scenario's `set_victory_message` / `set_defeat_message` replace the sentence under
scripted rules (`semaphore` byte1 & 0x10) or Conquer the World.

**Game Stats** has no counterpart in the install: the original's button opens `AchieveWin` (the Steam achievements). The port shows the post-game score table instead: a scroll panel with
one row per leader, best first, the viewer's row lit, the leader's name in his team text colour, his age, and the terms `Leader::compute_score` added up (units `+0x24`, buildings `+0x2c`,
economy `+0x30`, research `+0x3c` plus upgrades `+0x38`, territory `+0x20`, wonders `+0x40`) and their sum (gold for a winner, grey for a loser); an OK button returns to the window. The labels are the
install's where it has them (`StatWin::draw_normal_stats`: "Army Units:", "Economy:", "Research:", "Wonders:", ...; the others go through `uistr::tr`). The kind of ending is printed
on the log line `frame N: end-game window: victory | defeat | armageddon (conquest | capital | sudden death | wonder | territory | tech | score | economic | time limit | chairs | resign | allied | armageddon) "sentence"`.

### 17.4 The score window and the script camera events

`ui_hide_score`, `select_object` and `switch_player_control` post no-op `kCamera` `ScriptEvent`s (texts `hide_score`, `select_object`, `clear_selection`); `script_view` consumes them:

- `select_object` (`e.who` owner, `e.x` object): `MapRenderer selectScriptObject` makes it the HUD selection (a unit or a building, never mixed; the panel and card follow);
- `ui_hide_score` hides **the score window**: `StatWin::draw_normal_stats` in its default mode, a list of the players ranked by score at the lower left (rows stacked up from 196 px above the screen bottom, name in the
  team text colour, score right-aligned, `data_size` font). The original shows it from `player_profile.prefs.stat_mode` (the pref is not in the Options page): the port shows it in a Conquer the World battle, a
  multiplayer game and with `RON_SCORE_WIN=1`; the diplomatic status columns and the age are not drawn. Only the Napoleon opening (`conquest/Napoleon/napoleonopening.bhs`) calls the builtin;
- `switch_player_control`: the sim has already moved `local_play` and posts `clear_selection` (the selection is dropped by the existing handler); the fog, minimap, HUD and notifier read the seat each
  frame, so the new seat is simply what is drawn from the next frame (`frame N: viewing seat S` is logged).

### 17.5 Harness and snapshots (`re/previews/`, gitignored; 1280 x 800, game folder `~/Games/RiseOfNations`)

```
RON_LONGRUN_VICTORY=4 ./build/ronmap $G --random 12,4,1,8 --all-ai --frames 14000 --snapshot endgame_timelimit_defeat.png     # 3 score 4 time limit 5 chairs 7 territory 8 economic (victory mode override)
./build/ronmap $G --random 12,4,1,8 --all-ai --frames 24000 --snapshot endgame_wonder_defeat.png                            # standard rules: the wonder clock; --play 4 is the winner (the sim moved: frame 23,064 now)
RON_RESIGN_AT=600 ./build/ronmap $G --random 12,4,1,4 --frames 900 ...      # the viewing seat resigns (issue_resign)       RON_ARMAGEDDON_AT=800: the nuke count reaches the limit
RON_ALLIED_AT=600 ...  RON_CONQUER_AT=600 ...                                 # every leader allied with every other / every other leader conquered
RON_ENDGAME_STATS=1 ...                                                       # opens on the Game Stats table;  RON_GAME_MENU=2 opens the window on the current state
RON_SCORE_WIN=1 ./build/ronmap $G scenario/Tutorial1/tut1.scx --frames 60 ...  # the score window;  RON_CAMERA_EVENT="hide_score@10" / "select_object:0:1@10" posts the script event
--mouse down:640,435@N --mouse up:640,435@N                                    # clicks the window's Game Stats button (the buttons are laid out on demand)
```

| Snapshot | Shows |
|---|---|
| `endgame_wonder_defeat.png` / `endgame_wonder_victory.png` | frame 23,064 of `--random 12,4,1,8`: "You lost to a wonder victory." (skull banner) / "You won a wonder victory!" (cup banner, `--play 4`) |
| `endgame_timelimit_defeat.png` / `_victory.png` / `_warning.png` | the 15-minute limit (frame 13,501): "You did not have the high score at the time limit." / "You had the high score at the time limit!" (`--play 5`); frame 11,760: the red "The game will end in TWO minutes!" line with the time-limit clock at 1:55 |
| `endgame_stats.png` | Game Stats of the time-limit game: eight rows, terms, the winner's score in gold |
| `endgame_score_defeat.png`, `endgame_economic_defeat.png`, `endgame_chairs_defeat.png` | "You lost to a score / an economic victory.", "You had the lowest score at the time check!" (musical chairs, frame 12,601) |
| `endgame_resign.png`, `endgame_armageddon.png`, `endgame_conquest.png`, `endgame_allied.png` | "You resigned or quit!", the mushroom cloud, "You won by conquest!", "Your team won by conquest!" (see below) |
| `endgame_wonder_clock_start.png` / `_rival.png` | frame 14,835: the pyramid clock 5:41, "Spanish now has 1 net wonder points!" in red, the "Wonder Timer Started!" banner; frame 17,928: the rival's start with the "You will lose unless ..." line |
| `endgame_chairs_clock.png` / `_round.png` | the musical-chairs clock flashing under 450 frames (0:20); "Turks defeated." in red with the defeat banner after the first round |
| `endgame_clocks.png` | `RON_TIMER_TEST="3:300:1,5:90,7:200,0:600:2:3"`: the territory, time-limit, musical-chairs and wonder (with its net points) icons in the column |
| `endgame_score_window.png` / `_hidden.png`, `endgame_select_object.png` | the score window; the same scenario after the `hide_score` event; the `select_object` event selecting Boadicea (panel and card) |

`endgame_allied.png` ends as a partial alliance, as victory-spec 6 warns: `Leader::victory` makes each ally of the winner run `victory` and defeat everybody the ally is not allied with, so with the pairs allied in
index order the winner's allies defeat the others (type 5, two losers: "by conquest" in the sentence, exactly what the original's scan of `exec` gives).

### 17.6 Open

- The `OPTION_STAT_*` modes of the score window other than the ranking (economy, market, military, library, technology, groups, diplomacy, wonders), the
  stat window's diplomacy columns, the replay ("View Playback", another pass) and the Steam stats behind "Game Stats".
- Sudden death, tech race, a score victory in a team game and the team-style sentences are transcribed but not run on screen.
- `MessageWin::add_message`'s `Color*` argument (`&RED` at every call site) goes to `MessageWin::add_event` (the ping of a located message); the text colour is
  `next_message_color` (LIGHT_GREY unless `set_message_color` ran first). The port still prints the victory lines in red as section 17.1 says; if the
  original really shows them grey, `Note::argb` of those lines is the one place to change.

### 17.7 The clocks and endings on screen, the tribute lines (2026-10-25)

- **The wonder clock over the icon.** `IFaceMainBase::draw_game_timer` case 0 writes `LeaderData::get_wonder_net` over the icon: font 18 (default style), the
  leader's team colour, a black shadow at +1,+1, centred (`Buffer::write_cent_l`) in a box the icon's size that starts 5 px left of the icon
  (`hud_extra.mm` `draw_timers`; the old count was drawn left of the plate). The clock's leader is `Game::wonder_winning` (`gameTimers`, kind 0, only under the
  standard victory rules): the best net points at or over the goal, allies sharing a tie by wonder value, `wonder_timer` frames (map scaled) minus the time since
  that leader's stamp, 0 under a World Government. The line to the left of the plate is `draw_timers`' "Wonders (N #ICON46): M:SS" (#2030 + #2031 + net + #2032 +
  `#ICON` 46 + #2033 + the timer, in the team text colour, font 12, with the 17 x 17 `wonder` piece of icon_texs.xml between the two halves). Checked:
  `RON_TIMER_TEST="3:300:1,5:90,0:341:2:12"` at 1024 / 1440 / 1920 and a real `--random 12,4,1,8 --all-ai --frames 14900` game (the pyramid with "1" on it, the red
  4:52 plate, "Wonders (1 ^): 4:52"): `re/previews/ui5_wonder_clock_real.png`, `ui5_clocks_*.png`.
- **The territory victory, rendered.** The AI games never keep the goal share, so `RON_TERRITORY_AT=N[:M]` (`harnessEndings`) cuts `world_total_land` from frame N,
  every frame, to what the biggest territory holds divided by the goal: that leader holds exactly the goal (30% + 5% per setting) and the real
  `process_victory` code runs: `kPopStart` at frame 2000 ("Russians controls 30% of the world!" + "You will lose unless you take control of more territory!" in
  red, the `territory_timer` banner "Territory Victory: 4:34", the territory icon with its 4:32 plate), then what the real sim does on this map: a rival crosses the goal at frame 3286
  ("Russians no longer controls 30% of the world." in red, the plate gone: the halt), the clock restarts at 3795, the warning at 1800 frames left (frame 4925: the
  same two red lines again, the plate at 1:59), the second warning at 5825 and the victory at frame 6726 (`--random 12,4,1,8 --all-ai`: "You lost to a territory
  victory." with the skull banner, game time 7:28, the red defeat list at the left; with `--play 6`: "You won a territory victory!"). `RON_TERRITORY_AT=2000:3200` restores the
  land at 3200 for a clean halt. The harness prints `frame N: territory event K leader L value V` for the clock's events. Snapshots `ui5_territory_start.png`, `_banner`,
  `_clock.png` (frame 4230), `_halt.png` (frame 3292), `_warning.png` (4935), `_defeat.png` (6735), `_victory.png`. The banner text had lost the blank after the colon in the XML table: `notices.mm` adds it.
- **The ally tribute lines** (`Leader::defeat` @6ecb00): in a multiplayer game that goes on, a defeated leader's stockpile is shared among his live allies (the
  simulation does it: `leader_defeat`, `bucket_add`) and the viewer, when he is one of them, reads "+N Good received from X!" (#2307
  `+$NUMBER0 $GOODTYPE0 received from $PRESIDENTRONALDREAGAN1!`, red, `S_RECEIVED_TRIBUTE`) for every good both leaders have except Knowledge, N =
  `scale_tribute(stock / allies)`. The sim clears the stockpile in the same call and the event has no payload, so `notices.mm` reads the stockpile the previous
  update saw (`prev_buckets_`; at game speed the figure can differ from the exact one by the gathering of the frames in between: an event with the shares in
  `leader_defeat` would make it exact). Harness `RON_TRIBUTE_AT=N`: allies the viewer with the next leader, gives him a stockpile and the multiplayer bit,
  defeats him a frame later: `+232 Food`, `+145 Timber`, `+70 Wealth received from Dutch!` (age-1 leaders have no metal or oil): `ui5_tribute_lines.png`.
- **`say_full_name`** (@6ebe90) = the player's name + " (" (#2198) + the tribe's name + ")" (#2199); the port uses it where the decompile does: the wonder / territory
  clock messages (`GameDaemon::process_victory`), wonder started / completed (`Build::start` / `activate`), a finished tech (`Leader::gain_tech`) and the diplomacy
  line of `Leader::set_diplo` between two other leaders (#2337 war, #2338 alliance broken, #2339 peace, #2340 alliance; told only to a viewer who has met both
  (`treaties & 1`), in the original's DARK_ORANGE, `S_OTHER_NATION_DIPLO`; before, every change was told to everybody with other texts). A leader without a player name
  shows the nation alone. `Leader::ally_diplo`'s "War declared with X!" / "Alliance cancelled with X!" (#2341 / #2342, the viewer's allies) are not in the event
  log and stay open.
- **Layout pass** (every `--menu` screen and the HUD at 1024 x 768, 1440 x 900, 1920 x 1080, every PNG read): main, tutorials, skirmish, skirmish setup, scenarios,
  quit popup, multiplayer, lobby, options: nothing clipped or overlapping; the HUD (resources, the top bar, the clocks column, the minimap, the card, the selection
  panel, the wonder line) fits at all three sizes. The two defects found in the Conquer the World screens are fixed (below).
- **Conquer the World layout pass** (`ctw_ui.mm`; `--menu ctw`, `--ctw CAMPAIGN --ctw-screen map|news|cards|diplomacy|attack|menu`, the Cold War map, at 1024 x 768, 1440 x 900 and 1920 x 1080, every PNG read).
  The screens are the port's own layout (no `CTWSetup*` XML drives them), so the fixes are in the drawing code. `b.frame` draws its art inset about 16 px from the rectangle it is given
  (the border is 16-18 px thick): content has to sit 32 px or more inside the rectangle's edge.
  - *The "Different battle map each time" box* sat on the description frame's bottom border (y 392 against a frame ending at 420). The description frame is now `R(430, 994, 84, 412)`
    and the options frame `R(430, 994, 424, 628)`; the box is the last row of the options frame (`ry + 98`, `ry` = 464), under Random nations / Random bonuses (`ry + 72`) where a campaign allows them.
  - *The difficulty labels*: "Moderate" was as wide as its 60 px button. The six buttons are now 64 px wide on a 66 px pitch from x = 554 (they end at 948, inside the frame), so "Moderate" has room at all three sizes.
  - *Territory names on the strategic map* overlapped each other and the army discs (Megale Hellas under the Sicily army, Syria / Mesopotamia, most of central Europe in Napoleon). Each name is now placed
    below its dot as before unless that spot is taken by an army disc or an earlier name, then above, to the right, to the left, at the corners, below the level pips; if every spot overlaps, the one with the least overlap
    wins. The supply-depot / fortified marks follow the name. (Napoleon and the Cold War at the fit zoom are still dense in Europe; the wheel zooms in.)
  - *The side panel's tabs* had equal widths and "Diplomacy" / "Cold War" filled their buttons edge to edge; the widths now follow the label widths.
  - *The status line* of the map (round, age, nation (leader), tribute, territories, cards) ran under the right-hand DEFCON / Missiles text of the Cold War campaign at 1024 and overwrote it. The DEFCON and the Peace
    texts keep the right edge; the line drops "(leader)", then the age, then the nation, then shrinks to font 12 to fit what is left.
  Not changed, found while reading: the Entire World description still shows the unexpanded `$TRIBENAME0` token (the campaign text is passed through verbatim; the original expands it with the chosen nation).
  `ronmap --ctw-selftest` 75/75, `ron-ui-check check` 0 failures, zero warnings.

## 18. Recorded Games and the playback viewer (2026-10-25)

The original's playback viewer, inside the app: a **Recorded Games** screen on the Main Menu, the game window playing a `.rcx` back with the HUD as an observer, a bar with the
speed, pause, seat, fog and timeline controls and a live "in sync / out of sync at frame N" indicator, the End-game window's **View Playback** button, and `ronmap --playback`.
The engine side is `engine/net/playback.*` (docs/multiplayer-spec.md 8.9); `apps/map-mac/recplay.*` is the list scanner and the bar; `main.mm` drives it.

### 18.1 Pieces

| Where | What |
|---|---|
| `engine/net/playback.{hpp,cpp}` | `Playback`: opens a `.rcx` (`Kind::Original` / `NativeSave` / `NativeRandom`), builds the start game (`make_start`: rules, art, the original's prologue and game script, the room's names and handicaps), then `bind` / `step` / `run_to` drive ONE game with the recording's packages and compare its CHECK_SUMS (`sync()`). `playback_summary` is the header-only read of the list. `ron-net replay` builds the start game of each of its lockstep instances through the same `make_start`; `ron-net playback FILE` and `ronmap --playback FILE` print the same report. |
| `apps/map-mac/recplay.{hpp,mm}` | `Scanner` (the folder's `.rcx` files at once, newest first, headers read on a thread, so the screen opens at once and the rows fill in), `Viewer` (the bar: drawing, rectangles for the hit tests, speeds, seats, seek state), `clock_text`. |
| `frontend.mm` | the `Recorded Games` screen (`kRecorded`, `showScreen:@"recorded"`), Main Menu item 6 of 8 (the help box and the ribbon window moved 24 px down for the eighth button), the host call `frontEndPlayRecording:`. |
| `main.mm` | `+playbackWithDevice:` (builds the recording's map the way a random or scenario game does, then the recording's start state replaces the game), `pbStep` / `pbTick` / `pbSeek`, the bar's mouse and key handling, `--playback`. `mp::CommandSink::discard` (lobby.h) drops what the HUD issues: an observer's orders go nowhere. |
| `endgame.*` | `Screen::set_playback`: the View Playback button is enabled for a game that was recorded. |

### 18.2 The Recorded Games screen

Main Menu > **Recorded Games**: the folder `mp::default_record_dir()` names (`~/Library/Application Support/OpenRiseOfNations/Recorded Games/Multi`; `RON_RECORD_DIR` points it elsewhere),
searched recursively, newest first. One row per file: the file name, then `date | map style, size | N players: nations | duration | original game | why it cannot be watched`.
The detail line fills in while the headers are read. **Watch** (or Return, or a double click) plays the row; **Open File...** asks for any `.rcx` (`RON_REC_PICK=PATH` answers the chooser in tests) and plays it.
Only the folder the app records into is ever listed; the original's own recordings (`re/oracle`, gitignored test data) are opened only through a path somebody gives (Open File..., `--playback`).
Recordings of the original game are listed and marked `original game`: they play and check in `ronmap --playback` / `ron-net playback` (the check sums agree up to the engine's own fidelity frontier), but their map
is not a scenario or a setup text the renderer can build, so **Watch** says so instead of opening a window (18.7). A single-player file without a start state, a Conquer the World battle and a file that does not parse
are listed with the reason.

### 18.3 Watching

The game window opens on the recording's start state with the camera on the first human seat and the whole map visible; nothing the viewer orders is sent anywhere
(`CommandSink::discard`; the HUD still works as the watched seat's: resources, selection, minimap, messages from `notices`). The bar (top centre, under the resource strip):

| Control | Does |
|---|---|
| `\|<` / Home | back to the first frame (the start state is built again) |
| Pause / Play / Replay (P, the HUD's pause button) | stop and resume the stepping; after the end, Replay restarts |
| `-` `+` (the keys of OPTION_SLOWER / OPTION_FASTER) | speed 1/2x, 1x, 2x, 4x, 8x, Max (as many frames as fit in 12 ms of each display frame); 1x is the normal 15 frames a second |
| `<` `>` / Tab, Shift+Tab | the watched seat (the leader the HUD, fog and messages follow: `console.play`, which never feeds the simulation) |
| `Fog: on` / `Whole map` (F) | the watched seat's fog, or everything |
| timeline (click or drag) / `[` `]` | jump: forward runs the recording on in slices of the display frame ("Seeking..."), backward builds the start state again first; `[` `]` are 30 s |
| right of the timeline | `in sync` (green), `out of sync at frame N` (red; the fill of the timeline turns red), `No check sums` (a recording without CHECK_SUMS rows) |
| Exit | back to the list |

The game menu (F10 / Esc) has Resume Playback, Restart, Options, Return to Main Menu, Quit; Save / Load are refused (the game belongs to the recording). When the last package has run the bar shows
`End of the recording.` and Replay.

Speed is the viewer's own: a recorded PAUSE or SPEED_* is not executed (they are presentation), and the end-game window is not offered.

### 18.4 View Playback (End-game window)

`Screen::enabled(2)` is true when the game that ended is a multiplayer game with a recording on disk (`mp::Runtime::record_path()` names the file the Recorder flushes after every package, so it can be read at once;
after an out-of-sync recovery it is the newest segment, which embeds the snapshot and so starts at that frame). The button asks the app (`onPlayRecording`) to open the recording at frame 0 in a new game window; failing
that shows an alert and leaves the game as it was. Single-player games are not recorded: the button stays disabled there.

### 18.5 Commands

```
ronmap [GAME] --menu recorded [--snapshot out.png]               # RON_RECORD_DIR=DIR picks the folder
ronmap [GAME] --playback FILE.rcx                                # opens the window on the recording
ronmap [GAME] --playback FILE.rcx --frames N [--snapshot out.png [--size WxH]] [--pb-seat WHO] [--pb-fog]
        # headless: N frames after the start (no --frames with --snapshot: to the end), the check-sum report, the picture; --pb-seat WHO the leader whose seat the HUD shows,
        # --pb-fog that seat's fog; --mouse down:X,Y@0 / up clicks the bar (the first mouse event lays it out)
ronmap [GAME] --playback ORIGINAL.rcx [--frames N]               # a recording of the original game: played and checked, nothing drawn (prints the same report as ron-net playback)
ron-net playback FILE.rcx [--frames N] [--all-verbs] [--no-art] [--save out.ronsave]
RON_GAME_MENU_AFTER=2 RON_GAME_MENU_CLICK=2 ronmap --host ... --mp-record F.rcx --snapshot x.png     # the end-game window after the run; clicks View Playback (prints "[view playback] PATH")
```

### 18.6 Results (`re/previews/`, gitignored; 1280 x 800)

A short native recording made with two headless machines (`ronmap --host 17813 --humans 2 --random 18,0,7,2 --frames 1200 --mp-bot --mp-record F.rcx --snapshot x.png`, and `--join` with `--mp-bot`; East Indies, Tiny, 2 players, 1:20; kept as
`re/previews/playback_native_example.rcx`):

| Snapshot | Shows |
|---|---|
| `playback_main_menu.png` | the Main Menu with the eighth button, Recorded Games |
| `playback_recorded_games.png` | the list (`RON_RECORD_DIR` with two copies of the example): name, date, map style and size, the nations, duration; Open File..., Back, Watch |
| `playback_frame0.png` / `playback_frame400.png` / `playback_frame1200.png` | `--playback F --frames 0 / 400 / 1200`: the Mongols' (host) view with the whole map; 0:00, 0:26, 1:20 on the timeline; `in sync` from the first compared turn (67 and 200 boundaries, all 15 parts equal) |
| `playback_seat2_fog.png` | `--frames 600` then clicks on the bar's `>` and `Fog`: the Greeks' seat (guest), its resources and messages, the camera on its city, its fog on the minimap |
| `playback_seek_back.png` | frame 900, then a click on the timeline at 20 %: the start state is built again and run to 0:15 (`in sync` again, the HUD clock 0:00:15) |
| `playback_endgame_view_playback.png` | `RON_GAME_MENU_AFTER=2` after a recorded two-machine game: View Playback enabled (white); with `RON_GAME_MENU_CLICK=2` the click prints `[view playback] PATH` and opens that file |

Checks: `ron-net replay` of the original recording 2026.08.30 21'23'34 still gives 320 fully equal turns with `units` first differing at 1,928; `ronmap --playback` and `ron-net playback` of it print the same first
out-of-sync frame (1,928, per part as `ron-net replay`); the native recording plays in sync to the end (200 of 200 boundaries); the real-time path (`RON_PB_TICKS=N,SPEED,DT` runs `pbTick` N times) steps 59 frames in 60 display
frames at 1x, 235 at 4x and 1,078 at Max (60 slices of 12 ms) and the whole recording in 3,000 slices; `ron-check` 0 failures, `ron-ui-check check` 0, `ronmap --ctw-selftest` 75/75, `ron-net selftest` 106/106.
Not run: the window itself (a recording opened from the list in a real window, a click on Open File..., the timeline drag): every code path under them is the same one the harness drives, only the event delivery differs.

### 18.7 Open

- **Recordings of the original game cannot be watched**: the start state is the original's save (`load_original_save`), which holds no terrain art and no scenario file for a random map (the original regenerates it
  from the setup in the stream). `ronmap --playback` and `ron-net playback` run and check them; drawing them needs a renderer that builds terrain from the loaded `World` instead of from a scenario.
- Conquer the World battle recordings (the campaign is part of the start state) are listed as not watchable; a recording that starts from a recovery snapshot plays from the snapshot.
- The viewer's players' names come from the game's leaders; the original's recordings keep names out of every printout (`ReplayPlayer::name`).
- A drop package (the clear `P` record) is not executed by the direct player: the lockstep session's drop agreement acts on the game through `Session::apply_drop`, the viewer only reads the packages.

