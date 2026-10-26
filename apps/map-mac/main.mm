// ronmap: render a Rise of Nations scenario on macOS (AppKit + Metal) with the game's camera, its units driven by the
// simulation (engine/sim).
//
// Usage: ronmap [game-dir] [--menu [SCREEN] [--snapshot out.png [--size WxH]]]
//        ronmap [game-dir] <scenario.scx> [--zoom LEVEL] [--at X,Y] [--no-bloom] [--build NAME@X,Y[@TEAM] ...]
//               [--unit NAME@X,Y[@ANGLE] ...] [--no-sim] [--seed S] [--difficulty 0..5]
//               [--move WHO:o1,o2,...@X,Y[@FRAME] ...] [--attack WHO:o1,...@TWHO:TO[@FRAME] ...] [--frames N]
//               [--farm-time SECONDS] [--snapshot out.png [--size WxH]] [--no-hud] [--ui-scale S]
//               [--play WHO] [--select WHO:o1,o2,...] [--place-click] [--queue WHO:O@TYPE[*N]] [--select-build WHO:o1,...] [--build-page N] [--place TYPE@X,Y]
//               [--select WHO:o1,...@FRAME] [--option N[:MENU[:TYPE]]@FRAME] [--cursor X,Y@FRAME] [--look X,Y@FRAME]
//               [--zoom-at LEVEL@FRAME] [--script-log] [--no-scripts] [--load-game FILE] [--save-game FILE]
//               [--group NAME@FRAME] [--recall-group N@FRAME] [--regroup N@FRAME]
//   LEVEL is the game's zoom level 0 (far) .. 6 (closest); the game starts at 5. X,Y is the look-at point in world
//   units (default: player 1's start camera, else the map centre). The scenario's buildings and units are placed
//   automatically; --build / --unit add more by graphic name (static). --snapshot renders offscreen to a PNG and exits.
//   --farm-time runs that many seconds of unattended wheat growth first (a loaded scenario starts with bare farms).
//   The scenario's units are simulated (one frame per 67 ms, the game's normal speed) unless --no-sim. --move issues a
//   GROUP + MOVE_TO for leader WHO's units at FRAME (default 0) and --frames steps the simulation N frames before the
//   window opens or the snapshot is taken (headless checks).
//
//   The HUD (docs/ui-port.md) is drawn over the map and in snapshots unless --no-hud; --select / --select-build pick
//   objects for the selection panel and the command card, --build-page opens a citizen's build menu page, --place previews
//   placing building TYPE (TypeIndex) at X,Y.
//
//   Scenario scripts (docs/ui-port.md "Script UI"): the HUD's state is reported to the scripts every frame (selection,
//   armed command, build page, cursor, interface rectangles, camera, keys, sound playback) and their presentation is
//   drawn (objectives, messages, pings, arrows, flags, highlights, camera moves, locks, popups) and played (scenario
//   sounds, music) in the window. Headless checks: --select WHO:o1,..@FRAME selects as a click would at FRAME (an empty
//   list clears); --option N[:MENU[:TYPE]]@FRAME clicks command-card option N (OptionIndex: 9 Move, 0 / 1 / 2 the build
//   menus, -1 cancels), MENU opens a build page, TYPE starts placing that building; --cursor X,Y@FRAME puts the mouse
//   over world point X,Y; --look / --zoom-at move the camera as the player would; --script-log prints the
//   presentation queue as it is consumed. Army groups (docs/ui-port.md section 14): --group NAME@FRAME names the selected
//   soldiers as G + the name + Enter would (the first free number 1..9), --recall-group N@FRAME clicks group N's panel button,
//   --regroup N@FRAME its regroup button.
//
//   Save / load (engine/sim/save.hpp, docs/save-format.md): F5 quick-saves and F9 quick-loads the one save slot
//   ~/Library/Application Support/OpenRiseOfNations/Saves/quicksave.ronsave (the game menu has Save Game / Load Game
//   entries for the same slot: the front end has no text entry for names). A save loads only into the scenario it was
//   made in. Headless: --load-game FILE loads a save before the first frame, --save-game FILE writes one after the
//   --frames have run (before the snapshot).
//
//   Without a scenario the window opens in the front end (docs/ui-port.md "Front end"): the Main Menu drawn with the
//   install's own menu art, its Tutorials / Skirmish / Scenarios lists, Play starting the chosen map in the same window.
//   --menu SCREEN (main, tutorials, skirmish, scenarios, quit-popup) opens that screen; with --snapshot it is rendered
//   headless. The game folder comes from the command line, the remembered folder, or the usual install locations; a
//   native folder picker asks only when none is found.
//
// Controls (docs/ui-spec.md 4): click select (Shift adds, Ctrl/Cmd toggles, double click = same type on screen), drag a
//           box (units before buildings, 128 max); right-click: move / attack / rally point; minimap: click or drag to
//           look, right-click to order; Ctrl+0..9 stores a control group (no selection: a camera bookmark), 0..9 recalls,
//           twice centres; G names the selected soldiers as a control group (the first free number; a panel button per group:
//           click recalls, again centres, Shift+click renames, right-click erases; G with the group selected renames), Shift+G /
//           the panel's button / the card's Reagrupar regroups it; edge scrolling (2 px), mouse wheel / pinch zoom toward the cursor, trackpad scroll pans;
//           arrows scroll   + - zoom level   P pause   F fog   B bloom   H HUD   Esc cancel / clear / game menu   F10 game menu
//           (Resume, Save Game, Load Game, Return to Main Menu, Quit)   F5 quick save   F9 quick load; the game-over
//           screen offers Return to Main Menu.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#include <simd/simd.h>
#include <mach/mach.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/battlefield.hpp"
#include "assets/bh3.hpp"
#include "assets/bha.hpp"
#include "assets/buildings.hpp"
#include "assets/catalog.hpp"
#include "assets/effects.hpp"
#include "assets/files.hpp"
#include "assets/graphics_txt.hpp"
#include "assets/rules.hpp"
#include "assets/xml.hpp"
#include "assets/pose.hpp"
#include "assets/scenario.hpp"
#include "assets/tga.hpp"
#include "assets/tileset.hpp"
#include "render/dressing.hpp"
#include "render/farms.hpp"
#include "render/fog_overlay.hpp"
#include "render/forest.hpp"
#include "render/rivers.hpp"
#include "render/lighting.hpp"
#include "render/renderstate.hpp"
#include "render/terrain_mesh.hpp"
#include "sim/ai.hpp"
#include "sim/fog.hpp"
#include "sim/victory.hpp"
#include "sim/combat.hpp"
#include "sim/anim_assets.hpp"
#include "sim/commands.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/newgame.hpp"
#include "ctw_cli.h"
#include "ctw_ui.h"
#include "sim/rules.hpp"
#include "sim/rules_xml.hpp"
#include "sim/save.hpp"
#include "sim/script_host.hpp"
#include "sim/trig.hpp"
#include "sim/view.hpp"
#include "assets/cursors.hpp"
#include "assets/iface_layout.hpp"
#include "sim/production.hpp"
#include "hud.hpp"
#include "keymap.hpp"
#include "options_ui.hpp"
#include "prefs.hpp"
#include "endgame.hpp"
#include "notices.hpp"
#include "script_view.hpp"
#include "tips.hpp"
#include "hotkeys.hpp"
#include "uistr.hpp"
#include "uix.hpp"
#include "assets/strings.hpp"
#include "sim/capture.hpp"
#include "net/playback.hpp"
#include "recplay.hpp"
#include "audio.hpp"
#include "sim/script_ui.hpp"
#include "renderer.hpp"
#include "ui.hpp"
#include "unit_draw.hpp"
#import "frontend.h"
#import "launcher.h"

namespace rm = ron::metal;

// Game camera (docs/terrain-spec.md B7): orthographic, yaw 45 degrees, 30 degrees above the horizon.
// Basis from the disassembly; viewed from above, world +x runs right-down and +y left-down on screen.
static const simd_float3 kRight = {0.70710678f, -0.70710678f, 0.0f};
static const simd_float3 kUp = {-0.35355339f, -0.35355339f, 0.8660254f};
static const simd_float3 kBack = {0.61237244f, 0.61237244f, 0.5f};  // from the look-at point toward the eye
// World units per pixel at each zoom level, for screens at least 1280 pixels wide: 3 / ratio[level].
static const float kZoomRatio[7] = {0.08f, 0.1f, 0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 1.0f};

static float units_per_pixel(int level, float screen_w) {
	return 3.0f * std::max(screen_w, 1280.0f) / screen_w / kZoomRatio[std::clamp(level, 0, 6)];
}

// View (rows: right, up, back) and orthographic projection to Metal clip space (depth 0..1).
static void camera_matrices(simd_float3 look_at, float upp, float w, float h, simd_float4x4& view, simd_float4x4& proj) {
	const float dist = 60000.0f;
	const simd_float3 eye = look_at + kBack * dist;
	view = simd_matrix(simd_make_float4(kRight.x, kUp.x, kBack.x, 0), simd_make_float4(kRight.y, kUp.y, kBack.y, 0),
	                   simd_make_float4(kRight.z, kUp.z, kBack.z, 0),
	                   simd_make_float4(-simd_dot(kRight, eye), -simd_dot(kUp, eye), -simd_dot(kBack, eye), 1));
	const float zn = 1000.0f, zf = 2.0f * dist;
	proj = simd_matrix(simd_make_float4(2.0f / (w * upp), 0, 0, 0), simd_make_float4(0, 2.0f / (h * upp), 0, 0),
	                   simd_make_float4(0, 0, -1.0f / (zf - zn), 0), simd_make_float4(0, 0, -zn / (zf - zn), 1));
}

// Terrain render states (TerrainOut's own, not in the named table): "Base Terrain" is TEXTURE x DIFFUSE (the baked
// vertex light) with vertex alpha, alpha-blended, depth test and write, and lit again in the shader; water is the
// vertex colour, lit, blended over everything, no depth write. docs/lighting-spec.md section 7.
static ron::RenderState land_state() {
	ron::RenderState rs;
	rs.name = "Terrain Land";
	rs.enables = ron::rs_enable::kDepthTest | ron::rs_enable::kDepthWrite | ron::rs_enable::kAlphaBlend | ron::rs_enable::kLighting;
	rs.src_blend = ron::BlendMode::SrcAlpha;
	rs.dst_blend = ron::BlendMode::InvSrcAlpha;
	rs.cull = ron::CullMode::CCW;
	rs.stage[0] = {ron::TextureOp::Modulate, ron::TextureArg::Texture, ron::TextureArg::Diffuse,
	               ron::TextureOp::SelectArg2, ron::TextureArg::Texture, ron::TextureArg::Diffuse, 0,
	               ron::TexAddress::Wrap, ron::TexAddress::Wrap};
	return rs;
}

// Trees: the game's "Tree Blend" (fake-lit). Its diffuse is the tree's vertex colour and tfactor is set per draw in
// flush_trees (not decoded yet): a neutral 0.5 grey diffuse makes ADDSIGNED pass the texture through.
static ron::RenderState tree_state() {
	ron::RenderState rs = *ron::find_render_state("Tree Blend");
	rs.cull = ron::CullMode::None;
	return rs;
}

// Transitions (TerrainOut+0x442c): depth test LESSEQUAL without write, blended, lit. Colour = ground texture x diffuse,
// alpha = diffuse alpha x mask alpha (stage 1 samples the mask with UV1). docs/terrain-spec.md B2a T8.
static ron::RenderState transition_state() {
	ron::RenderState rs = land_state();
	rs.name = "Transitions";
	rs.enables = ron::rs_enable::kDepthTest | ron::rs_enable::kAlphaBlend | ron::rs_enable::kLighting;
	rs.stage[0].address_u = rs.stage[0].address_v = ron::TexAddress::Clamp;
	rs.stage[1] = {ron::TextureOp::SelectArg2, ron::TextureArg::Texture, ron::TextureArg::Current,
	               ron::TextureOp::Modulate, ron::TextureArg::Texture, ron::TextureArg::Current, 1,
	               ron::TexAddress::Clamp, ron::TexAddress::Clamp};
	return rs;
}

// Ocean overlays ("CoralSandy", then "CoralTrans" with the coral texture): no depth test, alpha from the mask only.
static ron::RenderState coral_state(bool coral) {
	ron::RenderState rs = transition_state();
	rs.name = coral ? "CoralTrans" : "CoralSandy";
	rs.enables = ron::rs_enable::kAlphaBlend | ron::rs_enable::kLighting | ron::rs_enable::kPointSampling;
	rs.stage[0].address_u = rs.stage[0].address_v = coral ? ron::TexAddress::Clamp : ron::TexAddress::Wrap;
	rs.stage[1].alpha_op = ron::TextureOp::SelectArg1;
	return rs;
}

// "OceanFloor": depth test without write, no blending, lit; rgb = tex(UV0) x tex(UV1) x 2 x diffuse (water-spec 2.5).
static ron::RenderState floor_state() {
	ron::RenderState rs = land_state();
	rs.name = "OceanFloor";
	rs.enables = ron::rs_enable::kDepthTest | ron::rs_enable::kLighting;
	rs.stage[0] = {ron::TextureOp::Modulate2x, ron::TextureArg::Texture, ron::TextureArg::Diffuse,
	               ron::TextureOp::SelectArg2, ron::TextureArg::Texture, ron::TextureArg::Diffuse, 0,
	               ron::TexAddress::Wrap, ron::TexAddress::Wrap};
	rs.stage[1] = {ron::TextureOp::Modulate, ron::TextureArg::Texture, ron::TextureArg::Current,
	               ron::TextureOp::SelectArg2, ron::TextureArg::Texture, ron::TextureArg::Current, 1,
	               ron::TexAddress::Clamp, ron::TexAddress::Clamp};
	return rs;
}

// "Mountain Blend" (MountainsOut): rgb = tex0 x diffuse + tex1 - 0.5, a = tex0.a x tex1.a; depth test and write,
// alpha test > 0, lit. Stage 1 carries the range's RING/MAIN alpha with baked shading on UV1.
static ron::RenderState mountain_state() {
	ron::RenderState rs;
	rs.name = "Mountain Blend";
	rs.enables = 0x287;
	rs.src_blend = ron::BlendMode::SrcAlpha;
	rs.dst_blend = ron::BlendMode::InvSrcAlpha;
	rs.alpha_func = ron::CompareFunc::Greater;
	rs.cull = ron::CullMode::CCW;
	rs.stage[0] = {ron::TextureOp::Modulate, ron::TextureArg::Texture, ron::TextureArg::Diffuse,
	               ron::TextureOp::SelectArg1, ron::TextureArg::Texture, ron::TextureArg::Current, 0,
	               ron::TexAddress::Wrap, ron::TexAddress::Wrap};
	rs.stage[1] = {ron::TextureOp::AddSigned, ron::TextureArg::Texture, ron::TextureArg::Current,
	               ron::TextureOp::Modulate, ron::TextureArg::Texture, ron::TextureArg::Current, 1,
	               ron::TexAddress::Clamp, ron::TexAddress::Clamp};
	return rs;
}

// "Weathering Underlay" (snowcaps): no depth test, blended, fake-lit; rgb = tex(UV0) x diffuse, a = tex(UV1).a.
static ron::RenderState snowcap_state() {
	ron::RenderState rs = mountain_state();
	rs.name = "Weathering Underlay";
	rs.enables = 0x20004;
	rs.stage[0] = {ron::TextureOp::Modulate, ron::TextureArg::Texture, ron::TextureArg::Diffuse,
	               ron::TextureOp::SelectArg2, ron::TextureArg::Texture, ron::TextureArg::Diffuse, 0,
	               ron::TexAddress::Clamp, ron::TexAddress::Clamp};
	rs.stage[1] = {ron::TextureOp::SelectArg2, ron::TextureArg::Texture, ron::TextureArg::Current,
	               ron::TextureOp::SelectArg1, ron::TextureArg::Texture, ron::TextureArg::Current, 1,
	               ron::TexAddress::Clamp, ron::TexAddress::Clamp};
	return rs;
}

// Fog-of-war overlay (docs/fog-spec.md 9.2): the FOGTEX ramp's colour and alpha (black, alpha by fog level), blended
// over everything drawn on the ground; depth tested so objects standing in front are not covered, no depth write.
static ron::RenderState fog_state() {
	ron::RenderState rs;
	rs.name = "Fog Overlay";
	rs.enables = 0x5;
	rs.src_blend = ron::BlendMode::SrcAlpha;
	rs.dst_blend = ron::BlendMode::InvSrcAlpha;
	rs.cull = ron::CullMode::None;
	rs.stage[0] = {ron::TextureOp::SelectArg1, ron::TextureArg::Texture, ron::TextureArg::Diffuse,
	               ron::TextureOp::SelectArg1, ron::TextureArg::Texture, ron::TextureArg::Diffuse, 0,
	               ron::TexAddress::Clamp, ron::TexAddress::Clamp};
	return rs;
}

// "Farm Blend" (Farms::init, a RenderState of its own): RenderState defaults plus enables |= 0x20084 (depth test and
// write, blend, alpha test with the default ALWAYS, fake light), SRCALPHA / INVSRCALPHA, stage 0 colour and alpha both
// MODULATE(TEXTURE, DIFFUSE). docs/farms-spec.md section 4.
static ron::RenderState farm_state() {
	ron::RenderState rs;
	rs.name = "Farm Blend";
	rs.enables = 0x20087;
	rs.src_blend = ron::BlendMode::SrcAlpha;
	rs.dst_blend = ron::BlendMode::InvSrcAlpha;
	rs.cull = ron::CullMode::CCW;
	rs.stage[0] = {ron::TextureOp::Modulate, ron::TextureArg::Texture, ron::TextureArg::Current,
	               ron::TextureOp::Modulate, ron::TextureArg::Texture, ron::TextureArg::Current, 0,
	               ron::TexAddress::Wrap, ron::TexAddress::Wrap};
	return rs;
}

// River troughs ("trough_rs"): no depth test, blended, lit; colour and alpha = texture x diffuse.
static ron::RenderState trough_state() {
	ron::RenderState rs = land_state();
	rs.name = "River Trough";
	rs.enables = 0x204;
	rs.cull = ron::CullMode::None;
	rs.stage[0].alpha_op = ron::TextureOp::Modulate;
	rs.stage[0].alpha_arg1 = ron::TextureArg::Texture;
	rs.stage[0].alpha_arg2 = ron::TextureArg::Diffuse;
	return rs;
}

// River water sheen ("water_rs"): depth test without write, blended, unlit; the newwater frame x diffuse, alpha =
// diffuse x the RIVERTEX_WATER alpha on UV1.
static ron::RenderState river_water_state() {
	ron::RenderState rs = land_state();
	rs.name = "River Water";
	rs.enables = 0x5;
	rs.cull = ron::CullMode::None;
	rs.stage[1] = {ron::TextureOp::SelectArg2, ron::TextureArg::Texture, ron::TextureArg::Current,
	               ron::TextureOp::Modulate, ron::TextureArg::Texture, ron::TextureArg::Current, 1,
	               ron::TexAddress::Wrap, ron::TexAddress::Wrap};
	return rs;
}

static ron::RenderState water_state() {
	ron::RenderState rs = land_state();
	rs.name = "Terrain Water";
	rs.enables = ron::rs_enable::kDepthTest | ron::rs_enable::kAlphaBlend;
	rs.cull = ron::CullMode::None;
	rs.stage[0].color_op = ron::TextureOp::SelectArg2;
	return rs;
}

// Default unit team colours (TeamColors::init, TEAM_COLOR_UNIT_*), RGB 0..255.
static const uint8_t kUnitTeamColors[9][3] = {{255, 0, 0},   {34, 82, 255},  {255, 0, 255}, {0, 208, 0},   {255, 255, 0},
                                              {0, 255, 255}, {255, 255, 255}, {255, 105, 0}, {255, 255, 255}};

// Default building team colours (TeamColors::init, TEAM_COLOR_BUILD_*), RGB 0..255.
static const uint8_t kBuildTeamColors[9][3] = {{102, 0, 0},   {26, 57, 172}, {128, 0, 128}, {0, 89, 0},     {98, 96, 0},
                                               {0, 92, 108},  {120, 120, 120}, {112, 48, 0}, {255, 255, 255}};

// A loaded building graphic: sprite, depth hull (pre-mapped to building space) and add-on models.
struct BuildAsset {
	const ron::BuildGraphic* graphic = nullptr;
	id<MTLTexture> sprite;
	int sprite_w = 0, sprite_h = 0;
	std::vector<ron::ColorVertex> hull;  // building space: (-3y, -3x, 3z) of the BH3 rest pose
	std::vector<uint16_t> hull_indices;
	id<MTLBuffer> hull_vb, hull_ib;  // the hull's buffers, made on first draw (the mesh never changes; it used to be re-uploaded every frame)
	struct Addon {
		std::unique_ptr<ron::BH3Model> model;
		std::unique_ptr<ron::Animation> anim;
		id<MTLTexture> texture;
		const ron::RenderState* state = nullptr;
		id<MTLBuffer> ib;  // the model's index buffer, made on first draw
		std::vector<ron::ColorVertex> verts;  // the posed vertices, reused every frame
	};
	std::vector<Addon> addons;  // BUILD_DEFAULT events, in file order
};

// 3D units are drawn at unit scale x guy_scale (4.8, a global in riseofnations.exe) with the chad's Y mirror.
constexpr float kGuyScale = 4.8f;

struct PlacedUnit {
	int graphic = -1;
	float x = 0, y = 0, angle_deg = 0;
	int team = 1;
};

struct PlacedBuilding {
	std::string name;
	float x = 0, y = 0;
	int team = 1;
	BuildAsset* asset = nullptr;
	int who = -1, o = -1;  // the simulation's building (fog visibility), -1 for --build extras
};

// UnitAnim (CHAR_*) names by value (the PDB enum; re/analysis/types_unitanim.txt), as unit_graphics.xml spells them.
static const char* const kCharNames[] = {
	"CHAR_DEFAULT", "CHAR_IDLE1", "CHAR_IDLE2", "CHAR_IDLE3", "CHAR_GROUP_IDLE1", "CHAR_GROUP_IDLE2",
	"CHAR_GROUP_IDLE3", "CHAR_SLOG", "CHAR_WALK", "CHAR_JOG", "CHAR_ATTACKWALK", "CHAR_ATTACK1", "CHAR_ATTACK2",
	"CHAR_ATTACK3", "CHAR_ATTACKSPECIAL", "CHAR_DEATH_STAB1", "CHAR_DEATH_STAB2", "CHAR_DEATH_SHOT1",
	"CHAR_DEATH_SHOT2", "CHAR_DEATH_SPLODED1", "CHAR_DEATH_SPLODED2", "CHAR_TURN_LEFT", "CHAR_TURN_RIGHT",
	"CHAR_PACK", "CHAR_UNPACK", "CHAR_CHOP_WOOD", "CHAR_WALK_WITH_WOOD", "CHAR_DUMP_WOOD", "CHAR_WALK_TO_WOOD",
	"CHAR_MINE_ORE", "CHAR_WALK_WITH_ORE", "CHAR_DUMP_ORE", "CHAR_WALK_TO_ORE", "CHAR_BUILD", "CHAR_REPAIR",
	"CHAR_SOW", "CHAR_REAP", "CHAR_FARM"};
constexpr int kNumCharNames = int(sizeof kCharNames / sizeof kCharNames[0]);

// Wall-clock length of one simulation frame per game speed (TurnControl timings[speed] @afc4a4, docs/frame-spec.md section 1):
// Very Slow, Slow, Normal, Fast, Hyper Fast. The speed is presentation (the simulation does not read it).
constexpr double kTimings[5] = {200.0, 125.0, 67.0, 50.0, 1.0};
constexpr double kSimFrameMs = 67.0;  // the normal speed

// One simulated figure as drawn this display frame (interpolated between the last two simulation frames).
struct SimFigure {
	int graphic = -1, anim = -1;  // unit graphic and clip index into its anims (-1: none)
	float x = 0, y = 0, angle_deg = 0;
	double clip_ms = 0;
	int team = 1;
	float scale = 1;  // set_unit_scale (ScriptPresentation::unit_scale by graphics piece)
};
// A projectile in flight or stuck in the ground (drawn with its effects_graphics.xml AMMO piece).
struct SimShot {
	std::string ammo;
	float x = 0, y = 0, z = 0, angle_deg = 0, pitch_deg = 0;
};
struct SimUnitDrawn {
	int32_t who = 0, o = 0;
	float x = 0, y = 0;  // the unit's interpolated position (selection)
	int32_t hp = 0, max_hp = 0;
	size_t first = 0, count = 0;  // its figures in _figures
	int32_t type = 0;
};

static int64_t sim_key(int32_t who, int32_t o, int32_t guy = 0) { return (int64_t(who) << 40) | (int64_t(o) << 16) | guy; }

// GameInfo difficulty from --difficulty (0 easiest .. 5; -1 keeps the default 2). AI armies only attack once they
// have enough standard units for the current military epoch (ai-spec find_target); in tut5 that takes difficulty 4.
static int g_difficulty = -1;
// --all-ai: every seat of a --random game is a computer player (AI-versus-AI checks).
static bool g_randomAllAi = false;
static bool g_noScripts = false;  // --no-scripts: the scenario runs without its scripts (headless checks of orders the tutorials block)

// Debugging aid: RON_SKIP=floor,land,trans,coral,water skips those passes.
static bool debug_skip(const char* pass) {
	static const std::string skip = getenv("RON_SKIP") ? std::string(",") + getenv("RON_SKIP") + "," : std::string();
	return !skip.empty() && skip.find(std::string(",") + pass + ",") != std::string::npos;
}

@interface MapRenderer : NSObject <MTKViewDelegate>
@property(nonatomic, weak) MTKView* view;
@property(nonatomic) int zoom;
@property(nonatomic) BOOL bloom;
- (instancetype)initWithDevice:(id<MTLDevice>)device
                       gameDir:(NSString*)dir
                      scenario:(NSString*)scenario
                      simulate:(BOOL)simulate
                          seed:(uint32_t)seed
                         error:(NSString* __autoreleasing*)error;
// A Conquer the World battle (sim/ctw.hpp): the Game is built by `build` (ConquestGame::build_battle) on the renderer's files / rules.
+ (instancetype)ctwBattleWithDevice:(id<MTLDevice>)device
                           gameDir:(NSString*)dir
                             build:(std::function<std::unique_ptr<ron::sim::Game>(const ron::GameFiles&, const ron::sim::Rules&, const ron::sim::AnimSource*,
                                                                                   ron::sim::RandomExtras*, std::string*)>)build
                             error:(NSString* __autoreleasing*)error;
// A multiplayer game of a scenario (docs/multiplayer-spec.md 8.6): the scenario's players are the seats; the leaders in `humans` (by Player.who) are
// human on every machine, the others are computer players.
+ (instancetype)scenarioMultiplayerWithDevice:(id<MTLDevice>)device
                                      gameDir:(NSString*)dir
                                     scenario:(NSString*)scenario
                                         seed:(uint32_t)seed
                                       humans:(const std::array<bool, 8>&)humans
                                        error:(NSString* __autoreleasing*)error;
// A random-map skirmish (sim/newgame.hpp): `setup` from the setup screen, or `spec` (the --random option text) when setup is null.
+ (instancetype)randomGameWithDevice:(id<MTLDevice>)device
                             gameDir:(NSString*)dir
                               setup:(const ron::sim::RandomGameSetup*)setup
                                spec:(NSString*)spec
                               error:(NSString* __autoreleasing*)error;
// Headless: per-leader census of the simulation (units, buildings, population, stockpiles, age) printed to stdout.
- (void)printCensus;
- (void)lookAtX:(float)x y:(float)y;
- (BOOL)placeBuilding:(const std::string&)name x:(float)x y:(float)y team:(int)team;
- (BOOL)placeUnit:(const std::string&)name x:(float)x y:(float)y angle:(float)deg team:(int)team;
- (BOOL)snapshotTo:(NSString*)path width:(int)w height:(int)h scale:(float)scale;
// --perf N: N headless frames, each one simulation step then one rendered frame (a reused offscreen target), timed separately; prints the averages.
- (void)perfRun:(int)frames width:(int)w height:(int)h scale:(float)scale;
// --long-run EVERY[,SNAPSHOT_PREFIX]: steps the simulation to `total` frames (or until a nation reaches the Information Age / the game ends), printing sim timing, a 100-frame render sample, census and RSS every EVERY frames.
// RON_LONGRUN_VICTORY=2 (headless test runs): conquest only, so a wonder / territory win does not end a long AI game early.
- (void)applyVictoryOverride;
- (void)longRun:(int)every snapshotPrefix:(NSString*)prefix total:(int)total width:(int)w height:(int)h scale:(float)scale;
- (void)advanceFarmsTo:(uint32_t)frame;
- (void)scrollByPixelsX:(float)dx y:(float)dy;
// Selection, in drawable pixels with a top-left origin. A rectangle of zero size picks the nearest unit.
- (void)selectInRect:(CGRect)r add:(BOOL)add;
- (void)selectAll;
- (void)handleKey:(NSEvent*)event;
// Simulation: MOVE_TO for the selection to the ground under a drawable pixel; scripted moves; stepping.
- (void)commandMoveAt:(CGPoint)p;
- (BOOL)issueMoveWho:(int)who objects:(const std::vector<int32_t>&)objs x:(int32_t)x y:(int32_t)y;
- (void)stepSimulation:(int)frames;
- (BOOL)issueAttackWho:(int)who objects:(const std::vector<int32_t>&)objs targetWho:(int)tw target:(int)to;
@property(nonatomic, readonly) BOOL simulating;
@property(nonatomic) BOOL paused;
@property(nonatomic) BOOL fogOn;  // F toggles: off = everything visible (observer)
@property(nonatomic) BOOL hudOn;  // the in-game interface (--no-hud turns it off)
@property(nonatomic) float uiScale;  // drawable pixels per UI pixel: the window's backing scale (1 for snapshots)
// Input in UI pixels (view points, top-left origin): offered to the HUD first, then to the map (docs/ui-spec.md 4).
- (void)uiMouseDown:(CGPoint)p right:(BOOL)right modifiers:(NSEventModifierFlags)m clicks:(NSInteger)clicks;
- (void)uiMouseDragged:(CGPoint)p right:(BOOL)right delta:(CGPoint)d;
- (void)uiMouseUp:(CGPoint)p right:(BOOL)right modifiers:(NSEventModifierFlags)m;
- (void)uiMouseMoved:(CGPoint)p;
- (void)uiScrollWheel:(NSEvent*)e at:(CGPoint)p;
- (void)uiMagnify:(float)amount at:(CGPoint)p;
- (NSCursor*)cursorAt:(CGPoint)p;
// Headless helpers (snapshots): select objects of one owner; start placing a building and preview it at a world point.
- (BOOL)selectWho:(int)who objects:(const std::vector<int32_t>&)objs buildings:(BOOL)b;
- (void)startPlacement:(int)type;
- (void)previewPlacementAtWorldX:(float)x y:(float)y;
- (void)setBuildPage:(int)page;
- (void)placeAtWorldX:(float)wx y:(float)wy modifiers:(NSEventModifierFlags)m;
- (void)setLocalPlayer:(int)who;
- (void)queueWho:(int)who building:(int)o type:(int)type count:(int)n;
// Scenario scripts: the UI size for headless runs, script logging, a command-card click (OptionIndex, object) as the HUD
// runs it, the interactive start (audio, music), key state.
- (void)setUiWidth:(int)w height:(int)h scale:(float)scale;
- (void)setScriptLog:(BOOL)on;
- (void)runCardOption:(int)option object:(int)object;
- (void)startInteractive;
- (void)scriptKey:(NSEvent*)e down:(BOOL)down;
- (void)scriptFlags:(NSEventModifierFlags)m;
- (void)setMouseInside:(BOOL)inside;
- (void)setCursorWorldX:(float)x y:(float)y;
- (void)testPopup:(const char*)text;
// Harness: the mouse at UI pixel (x, y) with the mouse-over help shown at once; the resource panel flags; the streamlined
// interface; the help detail; a key by its KeyMap string ("Ctrl+Shift+F").
- (void)setHoverX:(float)x y:(float)y;
- (void)setGameSpeed:(int)speed;
- (void)setProgressButtons:(BOOL)on;
- (void)harnessCarrier:(const char*)spec;
- (void)mouseEvent:(const std::string&)kind x:(float)x y:(float)y;
- (void)setPanels:(int)flags streamlined:(BOOL)streamlined help:(int)level;
- (void)runKeyString:(const std::string&)keys;
// Named army groups (docs/ui-port.md section 14): name the selected soldiers (G, the name, Enter), the panel's buttons.
- (void)nameGroupScript:(const std::string&)name;
- (void)recallGroup:(int)slot;
- (BOOL)regroupGroup:(int)slot;
// Leaving the game from the game menu or the game-over screen: quit = NO returns to the Main Menu. Nil: Quit only.
@property(nonatomic, copy) void (^onExit)(BOOL quit);
// Conquer the World battle only: called once with the result (1 won, 0 lost, 2 left the battle) a few seconds after the game
// is decided, or at once when the player returns to the main menu from the game menu.
@property(nonatomic, copy) void (^onCtwEnd)(int outcome);
// -1 while undecided, else 1 / 0 from the local player's game-over event.
- (int)ctwOutcome;
- (int)ctwAdjudicated;
// Opens the game menu (Esc / F10): 1 the pause menu, 2 the game-over choices.
- (void)openGameMenu:(int)kind;
// Save / load of the simulation (engine/sim/save.hpp). A save loads only into the scenario it was made in.
- (BOOL)saveGameTo:(NSString*)path;
- (BOOL)loadGameFrom:(NSString*)path;
- (BOOL)quickSave;
- (BOOL)quickLoad;
// Multiplayer (lobby.h): this game is one machine of a lockstep session. Headless: runs it to a turn boundary at or after `frames`.
- (void)attachMultiplayer:(std::shared_ptr<mp::Runtime>)rt loaded:(std::shared_ptr<std::vector<uint8_t>>)save;  // `save`: the game continues a saved one
@property(nonatomic, readonly) BOOL multiplayer;
- (BOOL)mpRunHeadless:(int)frames;
- (void)mpTypeScript:(const std::vector<std::pair<int, std::string>>&)lines;  // --mp-type: lines typed into the chat box through the key handler
- (void)mpClose;  // headless: waits for the other machines, then ends the session (closes the recording)
// Playback of a recording (recplay.hpp, engine/net/playback.hpp, docs/ui-port.md section 18): a game of this port's recording, driven by its packages.
+ (instancetype)playbackWithDevice:(id<MTLDevice>)device gameDir:(NSString*)dir playback:(std::shared_ptr<ron::net::Playback>)pb error:(NSString* __autoreleasing*)error;
@property(nonatomic, readonly) BOOL playbackActive;
@property(nonatomic, copy) void (^onPlayRecording)(NSString* path);   // View Playback of the end-game window: the app opens the recording of the game that just ended
- (BOOL)pbRunHeadless:(int64_t)frames;   // headless: steps the recording (frames < 0: to its end) and prints the check-sum report
- (void)pbSeek:(int64_t)frame;
- (int)pbSeekTest:(int64_t)f1 to:(int64_t)f2;   // headless: seek F1 then back to F2 must equal a straight run to F2 (frames after the start); 0 when it does
- (void)pbSelectSeat:(int)index;
- (void)pbSelectWho:(int)who;   // the seat of the leader `who`
- (void)harnessGameMenuChoose:(int)i;   // headless tests: the button i of the game menu / end-game window, as a click would
@end

// ---- HUD data adapter: the console player's numbers for the resource panel and the top strip, from the production view
// (sim::leader_view: stockpiles, population in use / cap, cities / limit, age). Income rates and the commerce cap belong
// to the economy port and are not exposed yet: they show 0 until then. TODO(economy view API).
static hud::Resources hud_resources(const ron::sim::Game* g, int who) {
	hud::Resources r;
	if (!g) return r;
	const ron::sim::LeaderView v = ron::sim::leader_view(*g, who);
	r.live = v.valid;
	for (size_t k = 0; k < 6; k++) {
		r.stock[k] = v.stockpile[k];  // indexed by good TypeIndex (food 0 ... oil 5)
		r.rate[k] = (v.income[k] + 8) / 16;  // draw_resource: (income + 8) / 16 (ui-spec, resource panel)
	}
	// draw_cap: the smallest commerce cap of the goods (resource_cap / 16; economy-spec 2.3).
	int cap = 999;
	for (size_t k = 0; k < 6; k++) {
		if (v.cap[k] > 0) cap = std::min(cap, v.cap[k] / 16);
	}
	r.commerce_cap = cap;
	for (size_t k = 0; k < 6; k++) {
		r.buy[k] = v.buy[k], r.sell[k] = v.sell[k], r.over_cap[k] = v.over_cap[k];
	}
	r.can_trade = v.can_trade;
	r.pop = v.control;
	r.pop_cap = v.pop_cap;
	r.cities = v.cities;
	r.city_limit = v.city_limit;
	r.age = v.age;
	return r;
}

@implementation MapRenderer {
	id<MTLDevice> _device;
	id<MTLCommandQueue> _queue;
	rm::StateCache _states;
	rm::StateCache _overlayStates;  // post-tonemap overlays on the BGRA8 output (no depth)
	rm::PostProcess _post;
	rm::SceneTargets _targets;
	ron::RenderState _landState, _waterState, _treeState;
	id<MTLBuffer> _treeVB;
	id<MTLBuffer> _treeIB[ron::kTreeSlots];
	NSUInteger _treeCount[ron::kTreeSlots];
	id<MTLTexture> _treeTex[ron::kTreeSlots];
	// Trees under fog (RTreeData::adjust_tree_colors, fog-spec 9.3): hidden in unexplored cells, darkened where not seen.
	std::vector<ron::ColorVertex> _treeVerts;                // lit colours as built
	std::vector<uint32_t> _treeFirstVert;                    // per tree, + one end entry
	std::vector<int32_t> _treeFogCell;                       // per tree: fy * fog_xs + fx
	std::vector<uint32_t> _treeTris[ron::kTreeSlots];        // all triangles (3 indices each)
	std::vector<uint32_t> _treeTriTree[ron::kTreeSlots];     // the tree of each triangle
	std::vector<uint8_t> _treeLevel;                         // the fog level each tree was last drawn with

	std::string _title;
	ron::TerrainMesh _mesh;
	id<MTLBuffer> _landVB, _waterVB, _waterIB;
	std::vector<id<MTLBuffer>> _bucketIB;
	std::vector<id<MTLTexture>> _bucketTex;
	struct TransitionDraw {
		id<MTLBuffer> vb, ib;
		NSUInteger count;
		id<MTLTexture> texture, mask;
		bool ocean;
	};
	std::vector<TransitionDraw> _transitions;
	id<MTLTexture> _coral;
	id<MTLBuffer> _floorVB, _floorIB;
	struct MeshDraw {
		id<MTLBuffer> vb, ib;
		NSUInteger count;
		id<MTLTexture> tex0, tex1, tex0b, tex1b;  // b: the second draw (mountains)
	};
	std::vector<MeshDraw> _mountainDraws, _dooberDraws;
	MeshDraw _snowcapDraw;
	MeshDraw _farmUnderlays[2];  // "wheatfarm" and "pigfarm" ground (Weathering Underlay)
	std::vector<ron::Farm> _farms;
	ron::FarmParams _farmParams;
	id<MTLTexture> _farmTex;
	uint32_t _farmFrame, _farmSeed;
	// With the simulation, farms come from Game::farms (growth, type, construction): the sim's (who, o) per _farms entry.
	std::vector<std::pair<int32_t, int32_t>> _farmKeys;
	id<MTLTexture> _farmUnderTex[2];
	ron::WheatMesh _wheat;
	std::vector<MeshDraw> _troughDraws;
	MeshDraw _riverWaterDraw;
	id<MTLTexture> _floorTex;
	id<MTLTexture> _white;
	simd_float3 _lookAt;

	std::unique_ptr<ron::GameFiles> _files;
	ron::Terrain _terrain;
	ron::BuildCatalog _builds;
	ron::SceneLighting _lighting;
	float _terrainLight[3];
	ron::AmmoCatalog _ammo;
	std::map<std::string, std::unique_ptr<BuildAsset>> _buildAssets;
	// Construction (docs/buildings-spec.md section 6): underlay sprites and 3D scaffold pieces.
	std::vector<ron::ConstructionSprite> _constructSprites;
	std::map<size_t, id<MTLTexture>> _constructTex;
	std::map<size_t, int> _constructW, _constructH;
	ron::ScaffoldSet _scaffolds;
	struct ScaffoldModel {
		std::unique_ptr<ron::BH3Model> model;
		std::unique_ptr<ron::Animation> anim;
		id<MTLTexture> texture;
		const ron::RenderState* state = nullptr;
	};
	std::map<const ron::ScaffoldPiece*, std::unique_ptr<ScaffoldModel>> _scaffoldModels;
	std::vector<PlacedBuilding> _placed;
	rm::UnitDrawer _units;
	std::vector<PlacedUnit> _placedUnits;
	struct GoodAsset {
		std::unique_ptr<ron::BH3Model> model;
		std::unique_ptr<ron::Animation> anim;
		id<MTLTexture> texture;
		id<MTLBuffer> indices;
		std::vector<ron::PosedVertex> posed;
	};
	std::map<std::string, std::unique_ptr<GoodAsset>> _goodAssets;  // by GOOD name; null without a 3D model
	struct PlacedGood {
		GoodAsset* asset;
		float x, y;
	};
	std::vector<PlacedGood> _placedGoods;
	// Simulation (engine/sim): the scenario's units. Static _placedUnits keep turrets, farm animals and --unit extras.
	std::unique_ptr<ron::sim::Rules> _simRules;
	std::unique_ptr<ron::sim::AssetAnimSource> _art;  // animation lengths and graphics pieces (docs/anim-spec.md)
	std::unique_ptr<ron::sim::Game> _game;
	std::string _scenarioPath;  // the scenario file as Game::load got it (a save records it relative to the install); a random game: its setup text
	BOOL _randomActive;         // a random-map skirmish (sim/newgame.hpp) instead of a scenario file
	std::vector<std::pair<int, std::string>> _mpTypes;  // --mp-type: (frame, text) typed into the chat box as key events
	uint64_t _mpFilterSeq;      // multiplayer chat filter: the presentation events below this sequence number were handled
	BOOL _mpScenarioHumans;     // a multiplayer scenario game: `_mpHuman` says which of the scenario's players are human
	std::array<bool, 8> _mpHuman;
	ron::sim::RandomGameSetup _randomSetup;
	std::string _randomSpec;    // the --random option text, parsed once the rules are loaded
	std::function<std::unique_ptr<ron::sim::Game>(const ron::GameFiles&, const ron::sim::Rules&, const ron::sim::AnimSource*,
	                                              ron::sim::RandomExtras*, std::string*)> _ctwBuild;  // a Conquer the World battle
	std::function<std::unique_ptr<ron::sim::Game>(const ron::GameFiles&, const ron::sim::Rules&, const ron::sim::AnimSource*,
	                                              ron::sim::RandomExtras*, std::string*)> _pbBuild;   // a recording of the original game: its loaded save and the terrain made of it
	int _ctwOutcome, _ctwEndFrames;
	std::unordered_map<int32_t, int> _pieceGraphic;    // gpiece -> unit graphic (-1: unknown)
	mp::CommandSink _cm;                 // the local player's commands: executed here, or sent through the multiplayer session (lobby.h)
	std::shared_ptr<mp::Runtime> _mp;    // a multiplayer game's lockstep session (null in a single-player game)
	BOOL _mpEnded;
	int32_t _mpFrame;
	std::unique_ptr<recplay::Viewer> _pbv;   // playing a recording: its Playback, the viewing seat, the speed, the bar
	BOOL _pbScrub;                           // the timeline is being dragged
	int64_t _pbScrubFrame;
	ron::sim::GameView _prevView, _curView;
	std::unordered_map<int64_t, size_t> _prevIndex;  // sim_key(who, o) -> index in _prevView.units
	std::map<std::pair<int, int>, int> _scenarioGraphic;  // (who, o) -> unit graphic from the scenario resolution
	std::map<int32_t, int> _typeGraphic;                 // sim type -> fallback graphic (units created in game)
	std::map<std::pair<int, int>, int> _clipIndex;       // (graphic, CHAR_*) -> clip index
	struct ClipClock {
		int anim = -1;
		double start_ms = 0;
	};
	std::unordered_map<int64_t, ClipClock> _clipClocks;  // per figure: when its current animation started
	double _simMs;                                       // wall-clock time since the last simulation step
	bool _simEnabled;
	uint32_t _simSeed;
	std::vector<SimFigure> _figures;
	// Fog of war (docs/fog-spec.md 9): levels for the local player per fog cell, the draped overlay and its ramp.
	ron::FogOverlay _fog;
	std::vector<uint8_t> _fogLevels;
	id<MTLBuffer> _fogVB, _fogIB;
	id<MTLTexture> _fogTex;
	bool _fogUploaded;
	ron::sim::FogView _fogView;
	std::vector<SimUnitDrawn> _drawnUnits;
	std::vector<SimShot> _shots;
	std::vector<std::pair<int32_t, int32_t>> _selected;  // (who, o) of selected simulated units
	simd_float3 _startCamera;
	bool _hasStartCamera;
	double _timeMs, _lastFrame;
	// Interface (docs/ui-spec.md, docs/ui-port.md).
	std::unique_ptr<ron::IfaceLayout> _layout;
	rm::UiRenderer _ui;
	hud::Hud _hud;
	std::map<int, NSCursor*> _cursors;
	id<MTLTexture> _miniBG, _miniFog;
	std::vector<uint8_t> _miniFogLevels;
	std::vector<simd_float2> _treeXY;
	std::vector<std::pair<int32_t, int32_t>> _selectedBuilds;  // selected buildings (never mixed with units)
	int _uiW, _uiH;
	hud::State _hudState;  // the last state handed to the HUD (input hit tests use it)
	// Mouse (UI pixels).
	CGPoint _press, _mouse;
	double _pressMs;
	BOOL _pressing, _boxing, _hudPress, _rightPress, _rightDragged;
	NSEventModifierFlags _pressMods;
	// Command modes.
	int _targeting;  // OptionIndex of a pending target mode (MOVE_TO / ATTACK_TO), -1 none
	int _buildPage;  // -1 unit orders; 0 civic, 1 military, 2 wonders (Options::set_mode BUILD_OPTIONS)
	int _placeType;  // building being placed, -1 none (docs/ui-spec.md 4.5)
	BOOL _ghostValid;
	float _ghostX, _ghostY;
	int32_t _ghostBlocked;
	// Control groups through the command layer (hotkeys.hpp: the HOTKEY record of replay-spec verb 34).
	hotkeys::Groups _hotkeys;
	int _lastGroupKey;
	double _lastGroupMs;
	// Army groups (docs/ui-port.md section 14): the control groups 0..9 with a name each (this window's) and a panel.
	std::array<std::string, 10> _groupNames;
	int _gnameSlot;  // the name dialog: the group being named or renamed, -1 closed
	BOOL _gnameRename;
	std::string _gnameText;
	std::string _groupIconId;  // the card's Agrupar / Reagrupar icon (UiRenderer id), "" when missing
	BOOL _groupIconChecked;
	std::vector<std::pair<int, ron::IfaceRect>> _groupRects;  // the panel's buttons this frame (slot, rect)
	ron::IfaceRect _regroupRect;
	int _regroupSlot;  // the slot the regroup button acts on, -1 no button
	int _lastNamedSlot;
	double _lastNamedMs;
	float _magnify;
	NSInteger _pressClicks;
	CGFloat _drawW, _drawH;  // the drawable size of the frame being rendered
	// Scenario scripts (docs/ui-port.md "Script UI"): the presentation consumer, audio, the seat-local input.
	scriptview::View _sview;
	BOOL _sviewOn;
	std::unique_ptr<ron::audio::Audio> _audio;
	BOOL _headless;  // no window (snapshots, harness runs): a virtual clock of one frame per step, no audio
	BOOL _scriptLog;
	uint64_t _logSeq;
	size_t _logObjectives;
	std::vector<std::pair<std::string, int>> _logDone;
	struct CameraPan {  // Camera pan_to / to_zoom_level state, advanced by check_zoom steps
		int panning = 0, to_x = 0, to_y = 0, from_x = 0, from_y = 0;
		bool zooming = false;
		int to_zoom = 5;
	} _pan;
	double _camAccumMs;
	int _hiliteOverride;  // highlight_option(name) resolved through option_info, -1 none
	std::array<uint8_t, 256> _keys;  // Game::keystate by virtual key (bit 7 down)
	uint32_t _pendingWarnings;       // ScenarioData::user_warnings bits raised by the card since the last frame
	BOOL _mouseInside;
	BOOL _inPopup;
	int _popupResult;
	size_t _gameEventsSeen;
	// The game menu (MainMenuBase in interface.xml MAINMENU look): 0 closed, 1 Esc menu, 2 game over.
	int _gameMenu;
	std::vector<ron::IfaceRect> _gmRects;
	int _gmHover, _gmPress;
	std::unique_ptr<optui::Page> _optPage;  // the Options page over the game menu (_gameMenu == 3, docs/ui-port.md section 15)
	double _overMs;  // when the local game-over was first seen (-1 not yet)
	BOOL _overOffered;
	int _seatSeen;  // local_play + 1 as the last consumeScript saw it (0 = not yet)
	BOOL _uiReady;
	// Mouse-over help, hotkeys, notices from the simulation, minimap layers (docs/ui-port.md section 11).
	std::unique_ptr<ron::StringTables> _tables;
	tips::Help _help;
	BOOL _helpOn;
	notices::Notifier _notifier;
	endgame::Screen _endgame;  // the end-game window (_gameMenu == 2, docs/ui-port.md section 17)
	hud::Hover _hover;
	int _hoverTick;       // the graphic tick the hover target last changed
	BOOL _hoverForce;     // --hover: the tooltip shows without the delay
	int _verbosity;       // player_profile.prefs.verbosity: help detail 1..3 (default 2)
	id<MTLTexture> _miniTerr;
	int _terrFrame;       // game frame of the last territory texture update
	int _profile[5];             // the preferences last written (panel flags, mini library, help detail, progress buttons, speed)
	std::string _cardMessage;    // IFaceMarket's text over the card (a Market without the right to trade)
	int _flashMask, _flashTick;  // Console::flash_resource_flags / _stamp (graphic ticks): the rows that turn red
	int _speed;                  // TurnControl speed: 0 Very Slow .. 4 Hyper Fast (default 2 Normal)
	BOOL _leaderOptsSent;        // the Options page's LEADER_OPTIONS went out for this game (-issueLeaderOptions:)
	int _scoreWin;               // the score window's preference: 0 the game decides, 1 hidden, 2 shown (Alt+Shift+0 / Alt+Shift+1)
	BOOL _progressButtons;       // pref 0x40: one button per production in progress
	// Formation drag (Console::on_right_up, ui-spec 4.1): a right drag with own units selected moves them to the press point
	// facing the drag direction in the chosen formation and spacing; a left press cycles the formation, the wheel the spacing.
	BOOL _formDrag;
	simd_float2 _formStart, _formEnd;  // world points: the press and the cursor
	int _formForm, _formWidth;
	std::vector<ron::ColorVertex> _miniRiverTris, _miniMountainTris;  // the minimap background's feature overlays
}

- (instancetype)initWithDevice:(id<MTLDevice>)device
                       gameDir:(NSString*)dir
                      scenario:(NSString*)scenarioPath
                      simulate:(BOOL)simulate
                          seed:(uint32_t)seed
                         error:(NSString* __autoreleasing*)error {
	if (!(self = [super init])) return nil;
	_ctwOutcome = -1;
	_ctwEndFrames = -1;
	_simEnabled = simulate;
	_fogOn = YES;
	_uiScale = 1;
	_targeting = -1, _buildPage = -1, _placeType = -1, _lastGroupKey = -1;
	_gnameSlot = -1, _regroupSlot = -1, _lastNamedSlot = -1, _lastNamedMs = 0, _gnameRename = NO, _groupIconChecked = NO;
	_gmHover = _gmPress = -1, _overMs = -1;
	_verbosity = 2, _terrFrame = -100000, _hoverTick = 0;
	_simSeed = seed;
	_device = device;
	_queue = [device newCommandQueue];
	_zoom = 5;
	_bloom = YES;
	_landState = land_state();
	_waterState = water_state();
	_treeState = tree_state();
	std::string err;
	if (!_states.init(device, rm::SceneTargets::kColor, rm::SceneTargets::kDepth, err) ||
	    !_overlayStates.init(device, MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid, err) ||
	    !_post.init(device, _states.library(), MTLPixelFormatBGRA8Unorm, err)) {
		*error = @(err.c_str());
		return nil;
	}
	try {
		_files = std::make_unique<ron::GameFiles>(dir.UTF8String);
		const ron::GameFiles& files = *_files;
		const auto anim_names = ron::load_anim_names(files);
		_builds = ron::load_build_catalog(files, anim_names);
		_constructSprites = ron::load_construction_sprites(files);
		_scaffolds = ron::load_scaffolds(files, anim_names);
		_lighting = ron::load_scene_lighting(files);
		_units.init(device, _queue, &_states, _files.get());
		const float* ua = _lighting.unit_ambient;
		const float* ud = _lighting.unit_diffuse;
		_units.set_unit_material(simd_make_float4(ua[0], ua[1], ua[2], 1), simd_make_float4(ud[0], ud[1], ud[2], 1));
		_ammo = ron::load_ammo_catalog(files, anim_names);
		std::string path;
		ron::Scenario sc;
		std::unique_ptr<ron::sim::Game> randomGame;
		if (_randomActive) {
			// Setup::build_game for a random map: the game first, then the scenario-shaped records the renderer draws from.
			ron::sim::LoadOptions lopts;
			lopts.lang_suffix = uistr::suffix();
			_simRules = std::make_unique<ron::sim::Rules>(ron::sim::load_rules(files, lopts));
			ron::sim::start_game(*_simRules, ron::sim::GameSetup());
			_art = ron::sim::load_anim_source(files, *_simRules);
			if (!_randomSpec.empty()) {
				const std::string perr = ron::sim::parse_random_option(*_simRules, _randomSpec, _randomSetup);
				if (!perr.empty()) {
					*error = @(perr.c_str());
					return nil;
				}
				if (g_randomAllAi) {
					for (ron::sim::NationSetup& n : _randomSetup.nation) n.human = false;
				}
				if (g_difficulty >= 0) {
					for (ron::sim::NationSetup& n : _randomSetup.nation) n.difficulty = g_difficulty;
				}
			}
			ron::sim::RandomExtras extras;
			if (_ctwBuild || _pbBuild) {
				std::string cerr;
				randomGame = (_pbBuild ? _pbBuild : _ctwBuild)(files, *_simRules, _art.get(), &extras, &cerr);
				if (!randomGame) {
					*error = [NSString stringWithFormat:@"The battle could not be built: %s", cerr.c_str()];
					return nil;
				}
			} else {
				randomGame = ron::sim::new_random_game(files, *_simRules, _art.get(), _randomSetup, &extras);
			}
			if (!randomGame) {
				*error = [NSString stringWithFormat:@"The random map could not be generated: %s", extras.error.c_str()];
				return nil;
			}
			_randomSetup = extras.setup;
			_simSeed = extras.setup.seed;
			path = _ctwBuild ? std::string("ctw:battle") : ron::sim::random_setup_string(extras.setup);
			sc = std::move(extras.scenario);
		} else {
			path = scenarioPath.UTF8String;
			if (!files.resolve(path).empty()) path = files.resolve(path);  // allow paths relative to the game dir
			sc = ron::parse_scenario(ron::read_file(path));
			if (_mpScenarioHumans)  // the room decides who plays which of the scenario's players (identically on every machine)
				for (ron::ScenarioPlayer& pl : sc.players)
					if (pl.who >= 0 && pl.who < 8) pl.is_human = _mpHuman[size_t(pl.who)] ? 4 : 0;
		}
		_scenarioPath = path;
		const ron::Tileset ts = ron::load_tileset(files, sc.terrain.tileset);
		const std::vector<ron::TransitionMask> masks = ron::load_transition_masks(files);
		_mesh = ron::build_terrain_mesh(sc.terrain, ts, &masks, sc.world_seed);
		_terrain = sc.terrain;
		for (int c = 0; c < 3; c++) _terrainLight[c] = ts.diffuse[c];  // the Sun during the terrain pass: tileset DIFFUSE
		_title = sc.name + "  (" + std::to_string(sc.terrain.xs) + "x" + std::to_string(sc.terrain.ys) + ", " + ts.name + ")";
		std::printf("%s: %zu land vertices, %zu textures, %zu water triangles\n", _title.c_str(), _mesh.land.size(),
		            _mesh.buckets.size(), _mesh.water_indices.size() / 3);
		[self buildTrees:sc.terrain tileset:ts files:files];
		std::map<std::string, id<MTLTexture>> tex_cache;
		auto tex = [&](const std::string& rel) {
			auto it = tex_cache.find(rel);
			if (it != tex_cache.end()) return it->second;
			return tex_cache[rel] = [self makeTexture:ron::parse_tga(ron::read_file(files.resolve(rel)))];
		};
		size_t patches = 0;
		for (const ron::TransitionBucket& b : _mesh.transitions) {
			_transitions.push_back({[device newBufferWithBytes:b.vertices.data() length:b.vertices.size() * sizeof(ron::ColorVertex2) options:MTLResourceStorageModeShared],
			                        [device newBufferWithBytes:b.indices.data() length:b.indices.size() * 4 options:MTLResourceStorageModeShared],
			                        b.indices.size(), tex(b.texture), tex(b.mask), b.ocean});
			patches += b.vertices.size() / 25;
		}
		if (!ts.coral.empty()) _coral = tex(ts.coral);
		const ron::Dressing dressing = ron::build_dressing(files, ron::load_graphics_txt(files), sc.terrain, ts);
		auto buffer = [&](const void* data, size_t bytes) { return [device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared]; };
		if (!ts.mountain_base.empty() && !ts.mountain_alpha.empty()) {
			for (const ron::MountainRangeMesh& m : dressing.mountains) {
				if (m.indices.empty() || files.resolve(m.ring_alpha).empty() || files.resolve(m.main_alpha).empty()) continue;
				_mountainDraws.push_back({buffer(m.vertices.data(), m.vertices.size() * sizeof(ron::ColorVertex2)),
				                          buffer(m.indices.data(), m.indices.size() * 4), m.indices.size(),
				                          tex(ts.mountain_alpha), tex(m.ring_alpha), tex(ts.mountain_base), tex(m.main_alpha)});
			}
		}
		if (!dressing.snowcap_indices.empty()) {
			_snowcapDraw = {buffer(dressing.snowcaps.data(), dressing.snowcaps.size() * sizeof(ron::ColorVertex2)),
			                buffer(dressing.snowcap_indices.data(), dressing.snowcap_indices.size() * 4), dressing.snowcap_indices.size(),
			                tex(ts.snowcap), tex(ts.snowcap), nil, nil};
		}
		size_t ndoobers = 0;
		for (const ron::DooberBatch& d : dressing.doobers) {
			_dooberDraws.push_back({buffer(d.vertices.data(), d.vertices.size() * sizeof(ron::ColorVertex)),
			                        buffer(d.indices.data(), d.indices.size() * 4), d.indices.size(), tex(d.texture), nil, nil, nil});
			ndoobers += d.vertices.size() / 4;
		}
		// The minimap background's feature overlays (WorldMapBackground::render_feature): mountains in 0x857F69 at alpha 0x77
		// (additive), rivers flat 0x576870.
		for (const ron::MountainRangeMesh& m : dressing.mountains) {
			for (size_t k = 0; k + 2 < m.indices.size(); k += 3) {
				for (size_t c = 0; c < 3; c++) {
					ron::ColorVertex v = m.vertices[m.indices[k + c]].base;
					v.r = 0x85, v.g = 0x7F, v.b = 0x69, v.a = 0x77;
					_miniMountainTris.push_back(v);
				}
			}
		}
		const ron::RiverMesh rivers = ron::build_rivers(sc.terrain);
		for (int ri = 0; ri < 3; ri++) {
			for (size_t k = 0; k + 2 < rivers.trough_indices[ri].size(); k += 3) {
				for (size_t c = 0; c < 3; c++) {
					ron::ColorVertex v = rivers.trough[ri][rivers.trough_indices[ri][k + c]];
					v.r = 0x57, v.g = 0x68, v.b = 0x70, v.a = 255;
					_miniRiverTris.push_back(v);
				}
			}
		}
		const std::string river_tex[3] = {ts.river_normal, ts.river_delta, ts.river_halfdelta};
		for (int i = 0; i < 3; i++) {
			if (rivers.trough_indices[i].empty() || river_tex[i].empty()) continue;
			_troughDraws.push_back({buffer(rivers.trough[i].data(), rivers.trough[i].size() * sizeof(ron::ColorVertex)),
			                        buffer(rivers.trough_indices[i].data(), rivers.trough_indices[i].size() * 4),
			                        rivers.trough_indices[i].size(), tex(river_tex[i]), nil, nil, nil});
		}
		const std::string newwater = "terrain art/newwater/water0000.tga";
		if (!rivers.water_indices.empty() && !ts.river_water.empty() && !files.resolve(newwater).empty()) {
			_riverWaterDraw = {buffer(rivers.water.data(), rivers.water.size() * sizeof(ron::ColorVertex2)),
			                   buffer(rivers.water_indices.data(), rivers.water_indices.size() * 4), rivers.water_indices.size(),
			                   tex(newwater), tex(ts.river_water), nil, nil};
		}
		std::printf("rivers: %zu river(s), %zu sections\n", sc.terrain.rivers.size(), rivers.water.size() / 4);
		std::printf("dressing: %zu mountain ranges, %zu snowcap vertices, %zu doobers\n", _mountainDraws.size(), dressing.snowcaps.size(), ndoobers);
		if (!_mesh.floor_indices.empty() && !_mesh.floor_texture.empty()) {
			_floorVB = [device newBufferWithBytes:_mesh.floor.data() length:_mesh.floor.size() * sizeof(ron::ColorVertex2) options:MTLResourceStorageModeShared];
			_floorIB = [device newBufferWithBytes:_mesh.floor_indices.data() length:_mesh.floor_indices.size() * 4 options:MTLResourceStorageModeShared];
			_floorTex = tex(_mesh.floor_texture);
		}
		std::printf("transitions: %zu patches in %zu batches\n", patches, _transitions.size());

		// The scenario's objects (docs/scenario-objects-spec.md): buildings, cities and units with their graphics.
		const std::string rel = _randomActive ? std::string() : path.rfind(files.root() + "/", 0) == 0 ? path.substr(files.root().size() + 1) : path;
		const ron::Battlefield bf = ron::resolve_battlefield(files, sc, rel, ron::load_rules(files),
		                                                     _builds, _units.units());
		if (_simEnabled) {
			try {
				if (randomGame) {
					_game = std::move(randomGame);  // built above, scripts included
				} else {
					ron::sim::LoadOptions lopts;
					lopts.lang_suffix = uistr::suffix();
					_simRules = std::make_unique<ron::sim::Rules>(ron::sim::load_rules(files, lopts));
					ron::sim::start_game(*_simRules, ron::sim::GameSetup());
					_art = ron::sim::load_anim_source(files, *_simRules);
					_game = std::make_unique<ron::sim::Game>();
					_game->anims = _art.get();
					if (g_difficulty >= 0) _game->info.difficulty = uint8_t(g_difficulty);
					_game->load(files, sc, *_simRules, _simSeed);
					// Scenario, general powers and AI production scripts (sim/script_host.hpp).
					if (!g_noScripts) {
						const ron::sim::ScriptLoadReport srep = ron::sim::script_host_load(*_game, files, path, sc);
						for (const std::string& e : srep.errors) std::fprintf(stderr, "script: %s\n", e.c_str());
					}
				}
				_curView = ron::sim::make_view(*_game);
				_prevView = _curView;
				[self indexPrevView];
			} catch (const std::exception& e) {
				std::fprintf(stderr, "simulation unavailable (%s); drawing the scenario's units where they stand\n", e.what());
				_game.reset();
			}
		}
		size_t nb = 0, nu = 0, nsim = 0;
		std::vector<ron::FarmSite> farm_sites;
		std::vector<ron::CitySite> city_sites;
		for (const ron::PlacedObject& o : bf.objects) {
			if (o.kind == ron::PlacedObject::Building && o.city) {
				const int level = o.graphic.rfind("METROPOLIS", 0) == 0 ? 3 : o.graphic.rfind("CITY", 0) == 0 ? 2 : 1;
				city_sites.push_back({o.x, o.y, o.who, level});
			}
			if (o.kind == ron::PlacedObject::Farm) farm_sites.push_back({o.x, o.y, o.who, o.slot});
			else if (o.kind == ron::PlacedObject::Good) [self placeGood:o.graphic x:o.x y:o.y files:files];
			else if (o.kind == ron::PlacedObject::Building) {
				if ([self placeBuilding:o.graphic x:o.x y:o.y team:o.color]) {
					nb++;
					for (PlacedBuilding& p : _placed) {
						if (p.x == o.x && p.y == o.y && p.who < 0) p.who = o.who, p.o = o.slot;
					}
				}
			}
			else if (_game && o.slot >= 0 && _units.find(o.graphic) >= 0) {
				_scenarioGraphic[{o.who, o.slot}] = _units.find(o.graphic);  // drawn where the simulation puts it
				nsim++;
			} else nu += [self placeUnit:o.graphic x:o.x y:o.y angle:o.angle_deg team:o.color] ? 1 : 0;
		}
		if (_game) std::printf("simulation: %zu scenario units bound, %zu simulated units alive\n", nsim, _curView.units.size());
		// Farms (docs/farms-spec.md): wheat boxes on a "wheatfarm" underlay, or an animal pen with five animals.
		_farmParams = ron::load_farm_params(files);
		_farms = ron::place_farms(sc.terrain, farm_sites, city_sites);
		_farmSeed = 1;
		const std::string farm_tex = !ts.farm_wheat.empty() ? ts.farm_wheat : (_builds.find("FARM") ? _builds.find("FARM")->texture : "");
		if (!farm_tex.empty() && !files.resolve(farm_tex).empty()) _farmTex = tex(farm_tex);
		size_t pens = 0;
		for (int k = 0; k < 2; k++) {
			const std::string& under = k == 0 ? ts.wheat_underlay : ts.animal_underlay;
			if (!under.empty() && !files.resolve(under).empty()) _farmUnderTex[k] = tex(under);
		}
		[self rebuildFarmUnderlays];
		for (const ron::Farm& f : _farms) {
			if (f.type != ron::FarmType::Animal) continue;
			pens++;
			if (_game) continue;  // the simulation creates the pen animals itself (Farms::add_animals)
			for (const ron::FarmAnimal& a : ron::farm_animals(f)) {
				const std::string name = a.type + "-TYPE" + std::to_string(a.variant);
				if (_units.find(name) >= 0) [self placeUnit:name x:a.x y:a.y angle:a.angle_deg team:8];
			}
		}
		std::printf("objects: %zu buildings, %zu units, %zu goods, %zu farms (%zu animal pens), %zu without a graphic\n", nb, nu,
		            _placedGoods.size(), _farms.size(), pens, bf.unresolved.size());
		if (getenv("RON_UNRESOLVED")) {
			std::map<std::string, int> counts;
			for (const std::string& u : bf.unresolved) counts[u]++;
			for (const auto& [name, n] : counts) std::printf("  unresolved: %s x%d\n", name.c_str(), n);
		}
		for (size_t i = 0; i < 8; i++) {
			if (sc.camera_x[i] >= 0 && sc.camera_y[i] >= 0) {
				_startCamera = simd_make_float3(float(sc.camera_x[i]), float(sc.camera_y[i]), 0);
				_hasStartCamera = true;
				break;
			}
		}
		if (!ts.fog.empty() && !files.resolve(ts.fog).empty()) {
			{
				// The ramp is sampled exactly by u (no mipmaps). Tilesets name fow_fade.tga or the stronger-falloff
				// fow_fade_dark.tga, whose "seen before" level (u 2/3) is almost clear (alpha 9).
				const ron::Image ramp = ron::parse_tga(ron::read_file(files.resolve(ts.fog)));
				MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				                                                                             width:NSUInteger(ramp.width)
				                                                                            height:NSUInteger(ramp.height)
				                                                                         mipmapped:NO];
				d.usage = MTLTextureUsageShaderRead;
				_fogTex = [device newTextureWithDescriptor:d];
				[_fogTex replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(ramp.width), NSUInteger(ramp.height)) mipmapLevel:0
				             withBytes:ramp.rgba.data() bytesPerRow:NSUInteger(ramp.width) * 4];
			}
			ron::build_fog_overlay(sc.terrain, _fog);
			_fogLevels.assign(size_t(_fog.fxs) * size_t(_fog.fys), 3);
			_fogIB = buffer(_fog.indices.data(), _fog.indices.size() * 4);
			_fogVB = buffer(_fog.vertices.data(), _fog.vertices.size() * sizeof(ron::ColorVertex));
		}
		for (const ron::TerrainBucket& b : _mesh.buckets) {
			_bucketIB.push_back([device newBufferWithBytes:b.indices.data() length:b.indices.size() * 4 options:MTLResourceStorageModeShared]);
			_bucketTex.push_back([self makeTexture:ron::parse_tga(ron::read_file(files.resolve(b.texture)))]);
		}
	} catch (const std::exception& e) {
		*error = [NSString stringWithFormat:@"%s", e.what()];
		return nil;
	}
	_landVB = [device newBufferWithBytes:_mesh.land.data() length:_mesh.land.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
	if (!_mesh.water_indices.empty()) {
		_waterVB = [device newBufferWithBytes:_mesh.water.data() length:_mesh.water.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
		_waterIB = [device newBufferWithBytes:_mesh.water_indices.data() length:_mesh.water_indices.size() * 4 options:MTLResourceStorageModeShared];
	}
	ron::Image white;
	white.width = white.height = 1;
	white.rgba = {255, 255, 255, 255};
	_white = [self makeTexture:white];
	_lookAt = _hasStartCamera ? _startCamera : simd_make_float3(_mesh.width / 2, _mesh.height / 2, 0);
	// The interface: layout, UI draw layer, HUD, cursors, the minimap background (docs/ui-port.md).
	try {
		_layout = std::make_unique<ron::IfaceLayout>(_files->read_text("Data/interface.xml"));
		std::string uerr;
		_hudOn = _ui.init(device, _queue, MTLPixelFormatBGRA8Unorm, _files.get(), _layout.get(), uerr) && _hud.init(*_files, *_layout, uerr);
		if (!_hudOn) std::fprintf(stderr, "interface unavailable: %s\n", uerr.c_str());
		_sviewOn = _hudOn && _sview.init(*_files, *_layout, uerr);
		_uiReady = _hudOn;
		_tables = std::make_unique<ron::StringTables>(*_files, uistr::language());
		uistr::use(_tables.get());
		_notifier.init(_tables.get());
		_endgame.init(*_layout);
		std::string herr;
		_helpOn = _hudOn && _help.init(*_files, herr);
		if (_hudOn && !_helpOn) std::fprintf(stderr, "help.xml unavailable: %s\n", herr.c_str());
	} catch (const std::exception& e) {
		std::fprintf(stderr, "interface unavailable: %s\n", e.what());
		_hudOn = NO;
	}
	[self renderMinimapBackground];
	[self syncSimBuildings];
	_headless = YES;
	_hiliteOverride = -1;
	_speed = 2;
	_flashTick = -1000;
	_keys.fill(0);
	if (_game) {
		if (ron::sim::ScriptInput* in = ron::sim::script_input(*_game)) {
			// BasePop::exec: popup_dialog / popup_choice block inside the script run until a button is chosen.
			__weak MapRenderer* weakSelf = self;
			in->popup = [weakSelf](const ron::sim::ScriptEvent& e) -> int32_t {
				MapRenderer* me = weakSelf;
				return me ? [me runPopup:e] : 0;
			};
		}
	}
	return self;
}

+ (instancetype)ctwBattleWithDevice:(id<MTLDevice>)device
                           gameDir:(NSString*)dir
                             build:(std::function<std::unique_ptr<ron::sim::Game>(const ron::GameFiles&, const ron::sim::Rules&, const ron::sim::AnimSource*,
                                                                                   ron::sim::RandomExtras*, std::string*)>)build
                             error:(NSString* __autoreleasing*)error {
	MapRenderer* r = [MapRenderer alloc];
	r->_randomActive = YES;
	r->_ctwBuild = std::move(build);
	return [r initWithDevice:device gameDir:dir scenario:@"" simulate:YES seed:1 error:error];
}

// Headless adjudication of an undecided battle: the side (team) with more live units and buildings wins.
- (int)ctwAdjudicated {
	const int o = [self ctwOutcome];
	if (o >= 0 || !_game) return o;
	const int me = [self localPlayer];
	int mine = 0, theirs = 0;
	for (int w = 0; w < 8; w++) {
		if (!(_game->leaders[size_t(w)].flags & 1)) continue;
		int alive = 0;
		const ron::sim::ObjectList& ol = _game->objects[size_t(w)];
		for (int32_t k = 0; k < ol.unit_mark; k++) alive += _game->unit(w, k) && _game->unit(w, k)->alive();
		for (int32_t k = ron::sim::kBuildBase; k < ol.build_mark; k++) alive += _game->build(w, k) && _game->build(w, k)->alive();
		const bool same = w == me || (_game->info.team[size_t(w)] >= 0 && _game->info.team[size_t(w)] == _game->info.team[size_t(me)]);
		(same ? mine : theirs) += alive;
	}
	return mine >= theirs ? 1 : 0;
}

- (int)ctwOutcome {
	if (!_game) return -1;
	for (const ron::sim::GameEvent& ev : _game->events) {
		if (ev.who != [self localPlayer]) continue;
		if (ev.kind == 3) return 1;
		if (ev.kind == 2) return 0;
	}
	return -1;
}

+ (instancetype)scenarioMultiplayerWithDevice:(id<MTLDevice>)device
                                      gameDir:(NSString*)dir
                                     scenario:(NSString*)scenario
                                         seed:(uint32_t)seed
                                       humans:(const std::array<bool, 8>&)humans
                                        error:(NSString* __autoreleasing*)error {
	MapRenderer* r = [MapRenderer alloc];
	r->_mpScenarioHumans = YES;
	r->_mpHuman = humans;
	return [r initWithDevice:device gameDir:dir scenario:scenario simulate:YES seed:seed error:error];
}

+ (instancetype)randomGameWithDevice:(id<MTLDevice>)device
                             gameDir:(NSString*)dir
                               setup:(const ron::sim::RandomGameSetup*)setup
                                spec:(NSString*)spec
                               error:(NSString* __autoreleasing*)error {
	MapRenderer* r = [MapRenderer alloc];
	r->_randomActive = YES;
	if (setup) r->_randomSetup = *setup;
	else if (spec) r->_randomSpec = spec.UTF8String;
	return [r initWithDevice:device gameDir:dir scenario:@"" simulate:YES seed:(setup ? setup->seed : 1) error:error];
}

// Headless census (--census): what each seat owns after the frames ran. The AI's own counters (units / buildings / training
// queues) show that it builds and trains; stockpiles show the economy running.
- (void)printCensus {
	if (!_game) return;
	std::printf("census at frame %d (tick %d), %zu events\n", _game->frame, _game->tick, _game->events.size());
	for (int who = 0; who < 8; who++) {
		const ron::sim::Leader& L = _game->leaders[size_t(who)];
		if (!(L.flags & ron::sim::leaderflag::kValid)) continue;
		const ron::sim::ObjectList& ol = _game->objects[size_t(who)];
		int units = 0, citizens = 0, military = 0, builds = 0, sites = 0, queued = 0, cities = 0;
		std::map<std::string, int> kinds;
		for (int o = 0; o < ol.unit_mark; o++) {
			const ron::sim::Unit* u = _game->unit(who, o);
			if (!u || !u->alive()) continue;
			units++;
			if (u->type == ron::sim::ptype::kPeasants) citizens++;
			else if (u->ptype && u->ptype->attack > 0) military++;
		}
		for (int o = ron::sim::kBuildBase; o < ol.build_mark; o++) {
			const ron::sim::Build* b = _game->build(who, o);
			if (!b || !b->alive()) continue;
			if (b->flags & ron::sim::objflag::kActive) {
				builds++;
				kinds[ron::sim::to_utf8(_game->type(b->type).name)]++;
			} else {
				sites++;
			}
			if (b->flags & ron::sim::objflag::kCity) cities++;
			queued += b->build_queue.queue_size;
		}
		std::string bl;
		for (const auto& [name, n] : kinds) bl += (bl.empty() ? "" : ", ") + name + (n > 1 ? " x" + std::to_string(n) : std::string());
		std::printf("  seat %d %s tribe %d %s age %d: %d units (%d citizens, %d military), %d buildings + %d sites, %d cities, %d queued, "
		            "stock F%d T%d W%d K%d M%d O%d pop %d/%d\n",
		            who, (L.flags & ron::sim::leaderflag::kHuman) ? "human" : "AI", L.tribe, L.name.c_str(), L.age, units, citizens,
		            military, builds, sites, cities, queued, L.bucket[0], L.bucket[1], L.bucket[2], L.bucket[3], L.bucket[4],
		            L.bucket[5], L.control, L.pop_cap);
		if (!bl.empty()) std::printf("      buildings: %s\n", bl.c_str());
	}
}

// Batches every tree into one vertex buffer (CPU-transformed rest poses) with an index list per texture slot.
- (void)buildTrees:(const ron::Terrain&)terrain tileset:(const ron::Tileset&)ts files:(const ron::GameFiles&)files {
	const ron::TreeSet set = ron::load_tree_set(files, ron::load_graphics_txt(files), ts);
	struct Model {
		std::vector<ron::PosedVertex> verts;
		std::vector<int> slot;  // per vertex
		std::vector<uint16_t> indices;
	};
	std::vector<Model> models;
	for (const std::string& path : set.models) {
		Model m;
		const ron::BH3Model bh3 = ron::parse_bh3(ron::read_file(files.resolve(path)));
		ron::pose_model(bh3, nullptr, 0, m.verts);
		m.slot.assign(m.verts.size(), -1);
		std::vector<const ron::BH3Node*> stack{&bh3.root};
		while (!stack.empty()) {
			const ron::BH3Node* n = stack.back();
			stack.pop_back();
			const int slot = ron::tree_slot_for_node(n->name);
			for (uint32_t v = n->first_vertex; v < n->first_vertex + n->vertex_count && v < m.slot.size(); v++) m.slot[v] = slot;
			for (const ron::BH3Node& c : n->children) stack.push_back(&c);
		}
		m.indices = bh3.meshes[0].indices;
		models.push_back(std::move(m));
	}
	const std::vector<ron::TreeInstance> trees = ron::place_trees(terrain, models.size());
	// Baked light for flat ground (as the terrain pass), at the tree's base.
	const float ndl = std::max(0.0f, -ron::kTerrainLightDir[2]);
	auto light = [&](int c) { return uint8_t(std::lround(std::clamp(ts.ambient[c] + ts.diffuse[c] * ndl, 0.0f, 1.0f) * 255)); };
	std::vector<ron::ColorVertex> verts;
	std::vector<uint32_t> idx[ron::kTreeSlots];
	const float s = set.scale;
	for (const ron::TreeInstance& t : trees) {
		const Model& m = models[size_t(t.model)];
		_treeXY.push_back(simd_make_float2(t.x, t.y));
		const uint32_t base = uint32_t(verts.size());
		_treeFirstVert.push_back(base);
		_treeFogCell.push_back(std::clamp(int(t.y / 384), 0, terrain.ys * 2 - 1) * terrain.xs * 2 +
		                       std::clamp(int(t.x / 384), 0, terrain.xs * 2 - 1));
		for (const ron::PosedVertex& p : m.verts) {
			verts.push_back({t.x + s * p.px, t.y - s * p.py, t.z + s * p.pz, p.nx, -p.ny, p.nz, p.u, p.v, light(0), light(1), light(2), 255});
		}
		for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
			const int slot = m.slot[m.indices[i]];
			if (slot < 0) continue;
			for (int k = 0; k < 3; k++) idx[slot].push_back(base + m.indices[i + size_t(k)]);
			_treeTriTree[slot].push_back(uint32_t(_treeFirstVert.size() - 1));
		}
	}
	_treeFirstVert.push_back(uint32_t(verts.size()));
	_treeVerts = verts;
	_treeLevel.assign(trees.size(), 3);
	std::printf("trees: %zu on %zu models\n", trees.size(), models.size());
	if (verts.empty()) return;
	_treeVB = [_device newBufferWithBytes:verts.data() length:verts.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
	for (int i = 0; i < ron::kTreeSlots; i++) {
		_treeCount[i] = idx[i].size();
		if (idx[i].empty() || set.textures[i].empty()) continue;
		_treeIB[i] = [_device newBufferWithBytes:idx[i].data() length:idx[i].size() * 4 options:MTLResourceStorageModeShared];
		_treeTris[i] = idx[i];
		_treeTex[i] = [self makeTexture:ron::parse_tga(ron::read_file(files.resolve(set.textures[i])))];
	}
}

- (id<MTLTexture>)makeTexture:(const ron::Image&)img {
	const bool mips = img.width > 1 || img.height > 1;
	MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                                             width:NSUInteger(img.width)
	                                                                            height:NSUInteger(img.height)
	                                                                         mipmapped:mips];
	d.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> t = [_device newTextureWithDescriptor:d];
	[t replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(img.width), NSUInteger(img.height)) mipmapLevel:0 withBytes:img.rgba.data() bytesPerRow:NSUInteger(img.width) * 4];
	if (mips) {
		id<MTLCommandBuffer> cb = [_queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit generateMipmapsForTexture:t];
		[blit endEncoding];
		[cb commit];
	}
	return t;
}

- (BOOL)placeBuilding:(const std::string&)name x:(float)x y:(float)y team:(int)team {
	BuildAsset* asset = [self buildAsset:name];
	if (!asset) {
		std::fprintf(stderr, "unknown or unloadable building graphic: %s\n", name.c_str());
		return NO;
	}
	_placed.push_back({name, x, y, std::clamp(team, 0, 8), asset});
	// Painter's order: back to front by x + y (ObjectsOut::sort_all).
	std::stable_sort(_placed.begin(), _placed.end(), [](const PlacedBuilding& a, const PlacedBuilding& b) { return a.x + a.y < b.x + b.y; });
	return YES;
}

- (BOOL)placeUnit:(const std::string&)name x:(float)x y:(float)y angle:(float)deg team:(int)team {
	const int g = _units.find(name);
	if (g < 0) {
		std::fprintf(stderr, "unknown unit graphic: %s\n", name.c_str());
		return NO;
	}
	_placedUnits.push_back({g, x, y, deg, std::clamp(team, 0, 8)});
	return YES;
}

// Units: translate(L) * rotate_z(angle) * scale(s, -s, s), s = unit scale x guy_scale (GuyOut::draw). The matrix is a
// mirror (det < 0) in the same basis as the terrain, which draws with the state's own cull faces: no flip for units
// either (flipping culled the faces towards the camera and left closed bodies hollow: trucks, wagons, siege engines).
- (void)drawUnitGraphic:(int)graphic anim:(int)anim ms:(double)ms x:(float)x y:(float)y angle:(float)deg team:(int)team
                encoder:(id<MTLRenderCommandEncoder>)enc {
	[self drawUnitGraphic:graphic anim:anim ms:ms x:x y:y angle:deg team:team scale:1 encoder:enc];
}

- (void)drawUnitGraphic:(int)graphic anim:(int)anim ms:(double)ms x:(float)x y:(float)y angle:(float)deg team:(int)team
                  scale:(float)extra encoder:(id<MTLRenderCommandEncoder>)enc {
	const ron::UnitGraphic& g = _units.units()[size_t(graphic)];
	const float s = g.scale * kGuyScale * extra, a = deg * float(M_PI) / 180.0f;
	// Objects stand on the ground, but never below sea level: ships float at z = 0 over the sea floor.
	const float z = std::max(0.0f, _terrain.sample_height(x, y));
	const float c = std::cos(a), sn = std::sin(a);
	const simd_float4x4 world = simd_matrix(simd_make_float4(c * s, sn * s, 0, 0), simd_make_float4(sn * s, -c * s, 0, 0),
	                                        simd_make_float4(0, 0, s, 0), simd_make_float4(x, y, z, 1));
	const uint8_t* tc = kUnitTeamColors[std::clamp(team, 0, 8)];  // TeamColor::get_unit
	_units.draw(enc, graphic, anim, ms, world, simd_make_float4(tc[0], tc[1], tc[2], 255) / 255.0f, false);
}

- (void)encodeUnits:(id<MTLRenderCommandEncoder>)enc {
	[self encodeFlags:enc];
	for (const PlacedUnit& u : _placedUnits) [self drawUnitGraphic:u.graphic anim:0 ms:_timeMs x:u.x y:u.y angle:u.angle_deg team:u.team encoder:enc];
	for (const SimFigure& f : _figures)
		[self drawUnitGraphic:f.graphic anim:f.anim ms:f.clip_ms x:f.x y:f.y angle:f.angle_deg team:f.team scale:f.scale encoder:enc];
	// Projectiles at unit scale, oriented along the heading and pitched (an approximation of AmmoOut).
	for (const SimShot& sh : _shots) {
		const float s = kGuyScale, a = sh.angle_deg * float(M_PI) / 180.0f, pt = sh.pitch_deg * float(M_PI) / 180.0f;
		const simd_float4x4 T = simd_matrix(simd_make_float4(1, 0, 0, 0), simd_make_float4(0, 1, 0, 0), simd_make_float4(0, 0, 1, 0),
		                                    simd_make_float4(sh.x, sh.y, sh.z, 1));
		const simd_float4x4 R = simd_matrix(simd_make_float4(std::cos(a), std::sin(a), 0, 0), simd_make_float4(-std::sin(a), std::cos(a), 0, 0),
		                                    simd_make_float4(0, 0, 1, 0), simd_make_float4(0, 0, 0, 1));
		const simd_float4x4 P = simd_matrix(simd_make_float4(1, 0, 0, 0), simd_make_float4(0, std::cos(pt), -std::sin(pt), 0),
		                                    simd_make_float4(0, std::sin(pt), std::cos(pt), 0), simd_make_float4(0, 0, 0, 1));
		const simd_float4x4 S = simd_matrix(simd_make_float4(s, 0, 0, 0), simd_make_float4(0, -s, 0, 0), simd_make_float4(0, 0, s, 0),
		                                    simd_make_float4(0, 0, 0, 1));
		_units.draw_ammo(enc, sh.ammo, _timeMs, simd_mul(T, simd_mul(R, simd_mul(P, S))), false);

	}
}

// Health bars over the selected units (ObjectsOut::draw_overlays, after tone mapping):
// a camera-facing strip above the unit, green for the remaining hit points over dark red.
- (void)encodeHealthBars:(id<MTLRenderCommandEncoder>)enc {
	if (_selected.empty()) return;
	static const ron::RenderState& bar_rs = *ron::find_render_state("Circle Blend");
	std::vector<ron::ColorVertex> verts;
	for (const SimUnitDrawn& u : _drawnUnits) {
		if (u.max_hp <= 0 || std::find(_selected.begin(), _selected.end(), std::make_pair(u.who, u.o)) == _selected.end()) continue;
		const float frac = std::clamp(float(u.hp) / float(u.max_hp), 0.0f, 1.0f);
		// A fixed on-screen size (28 x 4 pixels), 34 pixels above the unit's feet.
		const float upp = units_per_pixel(_zoom, float(_targets.hdr().width));
		const simd_float3 c = simd_make_float3(u.x, u.y, std::max(0.0f, _terrain.sample_height(u.x, u.y))) + kUp * (34.0f * upp);
		const float w = 28 * upp, h = 4 * upp;
		auto quad = [&](float x0, float x1, uint8_t r, uint8_t g, uint8_t b) {
			auto v = [&](float x, float y) {
				const simd_float3 p = c + kRight * x + kUp * y;
				return ron::ColorVertex{p.x, p.y, p.z, 0, 0, 1, 0, 0, r, g, b, 255};
			};
			const ron::ColorVertex q[6] = {v(x0, h), v(x1, h), v(x0, 0), v(x0, 0), v(x1, h), v(x1, 0)};
			verts.insert(verts.end(), q, q + 6);
		};
		quad(-w / 2, -w / 2 + w * frac, 40, 220, 40);
		if (frac < 1) quad(-w / 2 + w * frac, w / 2, 120, 0, 0);
	}
	if (verts.empty()) return;
	rm::DrawUniforms du = rm::draw_uniforms(bar_rs, matrix_identity_float4x4);
	_overlayStates.bind(enc, bar_rs, rm::VertexKind::Colored);
	[enc setCullMode:MTLCullModeNone];
	id<MTLBuffer> vb = [_device newBufferWithBytes:verts.data() length:verts.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
	[enc setVertexBuffer:vb offset:0 atIndex:0];
	[enc setVertexBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentTexture:_white atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:verts.size()];
}

// ---------------------------------------------------------------- simulation

- (BOOL)simulating {
	return _game != nullptr;
}

- (void)indexPrevView {
	_prevIndex.clear();
	for (size_t i = 0; i < _prevView.units.size(); i++) _prevIndex[sim_key(_prevView.units[i].who, _prevView.units[i].o)] = i;
}

// One simulation frame per iteration: execute the commands issued since the last step, then Game::do_frame
// (docs/frame-spec.md section 2). The renderer only reads the views.
static uint64_t g_doFrameCpuNs = 0;  // thread CPU time spent in Game::do_frame since the last report

// Headless checks of the endings the simulation cannot reach quickly (docs/ui-port.md section 17.5): RON_RESIGN_AT=N makes the
// viewing seat resign at frame N (CommandManager::issue_resign), RON_ARMAGEDDON_AT=N lets the nuke count reach the limit (the blast
// is not ported: Armageddon counts detonations, victory-spec 5.1), RON_ALLIED_AT=N allies every live leader with every other, RON_CONQUER_AT=N conquers them all.
- (void)harnessEndings {
	static const int resign = getenv("RON_RESIGN_AT") ? std::atoi(getenv("RON_RESIGN_AT")) : -1;
	static const int arma = getenv("RON_ARMAGEDDON_AT") ? std::atoi(getenv("RON_ARMAGEDDON_AT")) : -1;
	if (!_game || _mp) return;
	if (resign >= 0 && _game->frame == resign) _cm.issue_resign(false);
	static const int allied = getenv("RON_ALLIED_AT") ? std::atoi(getenv("RON_ALLIED_AT")) : -1;
	static const int conquer = getenv("RON_CONQUER_AT") ? std::atoi(getenv("RON_CONQUER_AT")) : -1;
	const int me = _game->local_play;
	if (allied >= 0 && _game->frame == allied) {  // every live leader allies with every other (a clique): Leader::set_diplo's allied victory
		for (int a = 0; a < 8; a++) {
			for (int b = a + 1; b < 8; b++) {
				if (!(_game->leaders[size_t(a)].flags & ron::sim::leaderflag::kActive) || !(_game->leaders[size_t(b)].flags & ron::sim::leaderflag::kActive)) continue;
				ron::sim::leader_set_diplo(*_game, a, b, 2);
				ron::sim::leader_set_diplo(*_game, b, a, 2);
			}
		}
	}
	if (conquer >= 0 && _game->frame == conquer) {  // every other leader is conquered (Leader::defeat type 0)
		for (int i = 0; i < 8; i++) {
			if (i != me && (_game->leaders[size_t(i)].flags & ron::sim::leaderflag::kActive)) ron::sim::leader_defeat(*_game, i, 0, me, 0);
		}
	}
	// RON_TRIBUTE_AT=N: the ally tribute lines of Leader::defeat. At frame N the viewer allies with the next live leader, who is given a
	// stockpile and the multiplayer bit (the sim shares a defeated leader's goods among his allies only in a multiplayer game); one frame
	// later that leader is conquered, so the viewer reads "+N Good received from X!" (the notifier reads the stockpile the frame before).
	static const int tribute = getenv("RON_TRIBUTE_AT") ? std::atoi(getenv("RON_TRIBUTE_AT")) : -1;
	if (tribute >= 0 && (_game->frame == tribute || _game->frame == tribute + 1)) {
		static int victim = -1;
		if (_game->frame == tribute) {
			victim = -1;
			for (int i = 0; i < 8 && victim < 0; i++) {
				if (i != me && (_game->leaders[size_t(i)].flags & 3u) == 3u) victim = i;
			}
			if (victim >= 0) {
				ron::sim::leader_set_diplo(*_game, me, victim, 2);
				ron::sim::leader_set_diplo(*_game, victim, me, 2);
				_game->multiplayer = true;
				const std::array<int32_t, 6> goods = {400, 250, 120, 0, 90, 60};
				_game->leaders[size_t(victim)].bucket = goods;
			}
		} else if (victim >= 0) {
			ron::sim::leader_defeat(*_game, victim, 0, me, 0);
		}
	}
	// RON_TERRITORY_AT=N[:M]: from frame N the world's land is cut every frame to what the leader with the biggest territory holds divided by
	// the goal (the territory goal of the game rules, 30% + 5% per setting), so that leader controls exactly the goal and a rival with less than
	// the goal / his territory stays below it:
	// the territory clock of GameDaemon::process_victory starts for real (events, warnings, the end-game window 3600 frames later);
	// with :M the land is restored at frame M (the clock halts). The map, the units and the clock arithmetic are the simulation's own.
	static const char* terr = getenv("RON_TERRITORY_AT");
	if (terr) {
		static int saved_land = 0, pinned = -1;
		int at = -1, off = -1;
		std::sscanf(terr, "%d:%d", &at, &off);
		if (_game->frame == at) {
			int bt = 0, second = 0;
			for (int i = 0; i < 8; i++) {
				if ((_game->leaders[size_t(i)].flags & 3u) != 3u) continue;
				const int t = ron::sim::get_team_terr(*_game, i);
				if (t > bt) second = bt, bt = t, pinned = i;
				else if (t > second) second = t;
			}
			saved_land = _game->world_total_land;
			std::fprintf(stderr, "frame %d: territory harness: leader %d holds %d, runner-up %d, goal %d%%\n", _game->frame, pinned, bt, second,
			             30 + 5 * int(_game->info.popwin));
		}
		if (pinned >= 0 && _game->frame >= at && (off < 0 || _game->frame < off)) {  // pinned every frame: the AI's growth cannot lose the goal
			const int bt = ron::sim::get_team_terr(*_game, pinned);
			if (bt > 0) _game->world_total_land = std::max(1, bt * 100 / (30 + 5 * int(_game->info.popwin)));
		}
		if (off >= 0 && _game->frame == off && saved_land > 0) _game->world_total_land = saved_land;
		static size_t seen_events = 0;  // the clock's events by frame, to pick the frames worth a snapshot
		for (; seen_events < _game->events.size(); seen_events++) {
			const ron::sim::GameEvent& e = _game->events[seen_events];
			if (e.kind >= 20 && e.kind <= 22) std::fprintf(stderr, "frame %d: territory event %d leader %d value %d\n", e.frame, e.kind, e.who, e.value);
		}
	}
	// RON_CAMERA_EVENT="select_object:WHO:O@FRAME" / "hide_score@FRAME": posts the no-op camera ScriptEvent the builtins post
	// (select_object, ui_hide_score), so the consumer in script_view runs end to end on a scenario that has scripts.
	static const char* ev = getenv("RON_CAMERA_EVENT");
	if (ev) {
		char text[64] = {0};
		int who = -1, obj = -1, at = 0;
		if (std::sscanf(ev, "%63[a-z_]:%d:%d@%d", text, &who, &obj, &at) < 4) std::sscanf(ev, "%63[a-z_]@%d", text, &at);
		const ron::sim::ScriptPresentation* p = [self pres];
		if (p && _game->frame == at) {
			ron::sim::ScriptPresentation* mp = const_cast<ron::sim::ScriptPresentation*>(p);
			ron::sim::ScriptEvent e;
			e.seq = mp->next_seq++, e.frame = _game->frame, e.kind = ron::sim::ScriptEvent::kCamera, e.text = text, e.who = who, e.x = obj;
			mp->events.push_back(e);
		}
	}
	if (arma >= 0 && _game->frame == arma) {
		_game->armageddon = std::max(0, ron::sim::get_armageddon(*_game) - 1);
		ron::sim::armageddon_nuke_detonated(*_game);
	}
}

- (void)stepSimulation:(int)frames {
	if (!_game) return;
	if (_pbv) {  // a recording: its packages decide what runs at each frame
		[self pbStep:frames];
		return;
	}
	if (_mp) {  // a multiplayer game: the session decides when a frame runs
		[self mpStep:frames];
		return;
	}
	for (int i = 0; i < frames; i++) {
		if (_headless) {
			_timeMs += kSimFrameMs;  // the virtual clock: graphic ticks advance with the frames (66 ms, whatever the speed)
			[self cameraStep:kSimFrameMs];
		}
		[self harnessEndings];
		_cm.process_turn(*_game);
		_hotkeys.process();
		[self fillScriptInput];
		const uint64_t c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
		_game->do_frame();
		g_doFrameCpuNs += clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0;  // --long-run / --perf report it: Game::do_frame alone, without the view and HUD bookkeeping around it
		[self consumeScript];
		_prevView = std::move(_curView);
		[self indexPrevView];
		_curView = ron::sim::make_view(*_game);
		[self notifyFrame];
	}
	if (frames > 0) [self syncSimBuildings];
}

// ---------------------------------------------------------------- multiplayer (lobby.h)

- (BOOL)multiplayer {
	return _mp != nullptr;
}

// The pause key: alone it flips the flag; in a multiplayer game it is a PAUSE command every machine runs at the same turn.
- (void)togglePause {
	if (_pbv) _paused = !_paused;   // a recording has no PAUSE command to send: the viewer just stops stepping
	else if (_mp) _mp->toggle_pause();
	else if (_game) {  // PAUSE 76 is a command too: it runs at once even while the game is paused (process_turn does not wait for a frame)
		_cm.issue_pause(!_paused);
		_cm.process_turn(*_game);
		_paused = _game->ctl.paused;
	} else _paused = !_paused;
}

// This game is one machine of a lockstep session: the local seat, the session's frame wrapper and its answer to an out-of-sync repair.
- (void)attachMultiplayer:(std::shared_ptr<mp::Runtime>)rt loaded:(std::shared_ptr<std::vector<uint8_t>>)save {
	_mp = rt;
	_mpEnded = NO;
	if (!_game) return;
	const int slot = rt->slot(), who = rt->who();
	_game->local_play = who;
	_cm.net = rt.get();
	_cm.play = slot, _cm.play_who = who;
	if (!save) rt->prepare_game(*_game);   // the players' names, handicaps and the multiplayer flag, identically on every machine
	if (rt->observer()) _fogOn = NO;       // an observer sees the whole map and issues nothing
	if (save) {  // a continued game: the saved state replaces the fresh one (same scenario: the terrain the renderer built stays)
		std::string err;
		if (![self mpRestore:*save error:err]) {
			std::fprintf(stderr, "multiplayer: cannot continue the saved game: %s\n", err.c_str());
			_mpEnded = YES;
			return;
		}
	}
	// the camera starts on this player's own seat
	const ron::sim::ObjectList& ol = _game->objects[size_t(who & 7)];
	for (int o = 0; o < ol.unit_mark && o < int(ol.units.size()); o++) {
		const ron::sim::Unit* u = ol.units[size_t(o)].get();
		if (u && u->alive()) {
			_lookAt = simd_make_float3(float(u->x), float(u->y), 0);
			break;
		}
	}
	_mpFrame = _game->frame;
	__unsafe_unretained MapRenderer* me = self;
	rt->before_frame = [me]() { [me fillScriptInput]; };
	rt->after_frame = [me]() { [me consumeScript]; };
	rt->log = [me](const std::string& text) { [me say:text]; };
	rt->restore = [me](const ron::net::Bytes& snap, std::string& err) { return [me mpRestore:snap error:err] == YES; };
	rt->attach([me]() -> ron::sim::Game& { return *me->_game; }, ron::sim::scenario_rel_path(*_files, _scenarioPath), _simSeed, _randomSetup, _speed, save.get());
}

// An out-of-sync repair: the authority's snapshot becomes this machine's game.
- (BOOL)mpRestore:(const std::vector<uint8_t>&)snap error:(std::string&)err {
	if (!_simRules || !_mp) return NO;
	try {
		ron::sim::SaveInfo info;
		std::unique_ptr<ron::sim::Game> g = ron::sim::load_game_bytes(*_files, *_simRules, _art.get(), snap, &info);
		if (!g) return NO;
		_game = std::move(g);
		_game->local_play = _mp->who();
		_cm.play = _mp->slot(), _cm.play_who = _mp->who();
		_mpFilterSeq = 0;
		if (ron::net::GameHooks* h = _mp->hooks()) h->rebind(*_game);
		[self afterGameReplaced];
		_mpFrame = _game->frame;
		return YES;
	} catch (const std::exception& e) {
		err = e.what();
		return NO;
	}
}

// One session step: 1 a tick ran, 0 nothing to do yet (the game waits), 2 recovered, -1 the session ended.
- (int)mpStepOnce:(bool*)turn {
	*turn = false;
	if (!_game || !_mp || _mpEnded) return -1;
	const mp::Runtime::Tick t = _mp->step(*turn);
	_speed = _mp->speed();
	_paused = _mp->paused();
	switch (t) {
		case mp::Runtime::Tick::Advanced:
			if (_game->frame != _mpFrame) {
				_mpFrame = _game->frame;
				if (_headless) {
					_timeMs += kSimFrameMs;  // the virtual clock, as stepSimulation keeps it
					[self cameraStep:kSimFrameMs];
				}
				_prevView = std::move(_curView);
				[self indexPrevView];
				_curView = ron::sim::make_view(*_game);
				[self notifyFrame];
			}
			return 1;
		case mp::Runtime::Tick::Recovered: return 2;
		case mp::Runtime::Tick::Ended:
			[self mpEnd:"The multiplayer game has ended."];
			return -1;
		case mp::Runtime::Tick::Error:
			[self mpEnd:_mp->error()];
			return -1;
		default: return 0;
	}
}

- (void)mpStep:(int)frames {
	bool turn = false;
	for (int i = 0; i < frames; i++) {
		const int r = [self mpStepOnce:&turn];
		if (r == 0 || r == -1) break;
	}
	if (frames > 0) [self syncSimBuildings];
}

- (void)mpEnd:(const std::string&)text {
	if (_mpEnded) return;
	_mpEnded = YES;
	[self say:text];
	std::fprintf(stderr, "multiplayer: %s\n", text.c_str());
	if (!_headless && self.onExit) {  // back to the menu after the player has read it
		__weak MapRenderer* weak = self;
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
			MapRenderer* me = weak;
			if (me && me.onExit) me.onExit(NO);
		});
	}
}

// Headless (--host / --join with --frames): the session runs until a turn boundary at or after `frames`.
- (BOOL)mpRunHeadless:(int)frames {
	if (!_mp) return NO;
	const double t0 = CACurrentMediaTime();
	bool turn = false;
	while (!_mpEnded) {
		const int r = [self mpStepOnce:&turn];
		if (r == -1) return NO;
		if (r == 1 && !_mpTypes.empty()) [self mpRunTyped];
		if (r == 1 && turn && (_game->frame >= frames || _game->game_over)) break;  // the game ended: the frames stop there
		if (r == 0) {
			[NSThread sleepForTimeInterval:0.0005];
			_mp->room().transport().idle(1);
			if (CACurrentMediaTime() - t0 > 600) {
				std::fprintf(stderr, "multiplayer: gave up after 10 minutes\n");
				return NO;
			}
		}
	}
	[self syncSimBuildings];
	std::printf("multiplayer: frame %d, %u recovery(ies), %d player(s)\n", _game->frame, _mp->epoch(), _mp->humans());
	for (const auto& ln : _game->ctl.chat)   // what this machine's console holds (every machine queues every line; the display filter hides the ones for others)
		std::printf("[mp] chat console: from %d to %s: \"%s\"\n", ln.from, ln.bits == 0xFFFFFFFFu ? "everybody" : ("mask " + std::to_string(ln.bits)).c_str(), ln.text.c_str());
	return !_mpEnded;
}

- (void)mpTypeScript:(const std::vector<std::pair<int, std::string>>&)lines {
	_mpTypes = lines;
}

// One key-down event to the key handler, as the window delivers it.
- (void)mpKey:(NSString*)chars code:(unsigned short)code {
	NSEvent* e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
	                            characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
	[self handleKey:e];
}

// --mp-type "TEXT@FRAME": at that frame the player presses Return (the key map opens the chat box), types TEXT character by character and presses Return.
- (void)mpRunTyped {
	for (auto it = _mpTypes.begin(); it != _mpTypes.end();) {
		if (!_game || _game->frame < it->first) {
			++it;
			continue;
		}
		NSString* text = @(it->second.c_str());
		it = _mpTypes.erase(it);
		[self mpKey:@"\r" code:36];
		if (!_hud.chat_active()) std::fprintf(stderr, "--mp-type: Return did not open the chat box\n");
		for (NSUInteger k = 0; k < text.length; k++) [self mpKey:[text substringWithRange:NSMakeRange(k, 1)] code:0];
		[self mpKey:@"\r" code:36];
	}
}

- (void)mpClose {
	if (!_mp) return;
	_mp->finish();
	_cm.net = nullptr;
	_mp.reset();
}

// A status line near the top: what the lockstep session is waiting for.
- (void)drawMpStatus {
	if (!_mp) return;
	std::string text;
	if (!_mp->started()) text = "Waiting for the other players to finish loading...";
	else if (_mp->recovering()) text = "The game was out of sync: repairing it from another player's game...";
	else text = _mp->stall_text();
	if (text.empty() && _mp->observer()) text = "Observing: you see the game from " + _mp->seat_name(0) + "'s seat. Chat is open; orders are not.";
	if (text.empty() && _mpEnded) text = _mp->error();
	if (text.empty()) return;
	const int f = _ui.font(16, -1);
	const ron::IfaceRect r{0, _uiW, 70, 94};
	_ui.fill(float(_uiW / 2 - 260), 70.0f, 520.0f, 24.0f, 0xA0000000u, ron::metal::UiType::ColorBlend);
	_ui.next_layer();
	_ui.text_centered(f, text, r, 0xFFFFE070u, 0xFF000000u);
}

// ---------------------------------------------------------------- playback of a recording (recplay.hpp, engine/net/playback.hpp)

+ (instancetype)playbackWithDevice:(id<MTLDevice>)device gameDir:(NSString*)dir playback:(std::shared_ptr<ron::net::Playback>)pb error:(NSString* __autoreleasing*)error {
	MapRenderer* r = [MapRenderer alloc];
	const std::string scn = pb->scenario();
	if (pb->kind() == ron::net::Playback::Kind::Original) {  // the original's save holds the map's grids, not a scenario or a setup: the terrain is made of the loaded game
		r->_randomActive = YES;
		ron::net::Playback* raw = pb.get();
		r->_pbBuild = [raw](const ron::GameFiles& files, const ron::sim::Rules&, const ron::sim::AnimSource*, ron::sim::RandomExtras* x, std::string* err) -> std::unique_ptr<ron::sim::Game> {
			std::unique_ptr<ron::sim::Game> g = raw->make_start(*err);
			if (!g) return nullptr;
			if (!ron::net::playback_scenario(*g, files, x->scenario, *err)) return nullptr;
			x->setup = ron::sim::RandomGameSetup();
			return g;
		};
		r = [r initWithDevice:device gameDir:dir scenario:@"" simulate:YES seed:pb->seed() error:error];
	} else if (ron::sim::is_random_scenario(scn)) {  // the map of the recording is generated again from its setup text
		ron::sim::RandomGameSetup setup;
		if (!ron::sim::parse_random_setup(scn, setup)) {
			if (error) *error = @"The map setup of the recording cannot be read.";
			return nil;
		}
		r->_randomActive = YES;
		r->_randomSetup = setup;
		r = [r initWithDevice:device gameDir:dir scenario:@"" simulate:YES seed:setup.seed error:error];
	} else if (ron::sim::is_ctw_map_scenario(scn)) {  // a Conquer the World battle on a custom map: the campaign's scenario file is the map
		ron::sim::CtwMapBattle cmb;
		if (!ron::sim::parse_ctw_map_battle(scn, cmb)) {
			if (error) *error = @"The battle map of the recording cannot be read.";
			return nil;
		}
		r = [r initWithDevice:device gameDir:dir scenario:@(cmb.map.c_str()) simulate:YES seed:pb->seed() error:error];
	} else {
		r = [r initWithDevice:device gameDir:dir scenario:@(scn.c_str()) simulate:YES seed:pb->seed() error:error];
	}
	if (!r) return nil;
	NSString* why = [r attachPlayback:pb];
	if (why) {
		if (error) *error = why;
		return nil;
	}
	return r;
}

- (BOOL)playbackActive {
	return _pbv != nullptr;
}

// The renderer built the scenario (terrain, trees, art); the recording's start state replaces the game it made and the playback drives it.
- (NSString*)attachPlayback:(std::shared_ptr<ron::net::Playback>)pb {
	if (!_game || !_simRules) return @"The game could not be set up.";
	_pbv = std::make_unique<recplay::Viewer>(pb);
	__unsafe_unretained MapRenderer* me = self;
	pb->before_frame = [me]() { [me fillScriptInput]; };
	pb->after_frame = [me]() { [me consumeScript]; };
	if (pb->kind() == ron::net::Playback::Kind::NativeRandom) {  // the renderer made the random game already: the room's names and handicaps on top
		pb->apply_header_seats(*_game);
	} else if (_pbBuild) {       // the original's recording: the renderer's game is the start state itself (built with its terrain)
	} else {
		std::string err;
		ron::net::PlaybackEnv env;
		env.files = _files.get(), env.rules = _simRules.get(), env.art = _art.get();
		std::unique_ptr<ron::sim::Game> g = pb->make_start(err, env);
		if (!g) return @(err.c_str());
		_game = std::move(g);
	}
	_cm.net = nullptr;
	_cm.discard = true;          // an observer's orders go nowhere: the game belongs to the recording
	[self afterGameReplaced];
	pb->bind(*_game);
	_pbv->speed = recplay::kNormalSpeed;
	[self pbCollectSeats];
	int first = 0;
	for (const ron::net::Playback::SeatInfo& si : pb->seats()) {
		if (!si.human) continue;
		for (size_t k = 0; k < _pbv->seats.size(); k++)
			if (_pbv->seats[k].who == si.who) first = int(k);
		break;
	}
	if (pb->kind() == ron::net::Playback::Kind::NativeSave && pb->start_frame() > 0) {  // an out-of-sync recovery segment: the snapshot is the start state
		_pbv->note = "Recording starts at frame " + std::to_string(pb->start_frame()) + " (an out-of-sync recovery snapshot)";
		[self say:_pbv->note];
	}
	_fogOn = NO;                 // the whole map to begin with; F shows a player's fog
	_overOffered = YES;          // the end-game window is not offered: the viewer decides when to leave
	_paused = NO;
	[self pbSelectSeat:first];
	return nil;
}

- (std::string)pbSeatLabel:(int)who {
	const ron::sim::Leader& L = _game->leaders[size_t(who)];
	std::string nation;
	if (_simRules && L.tribe >= 0 && size_t(L.tribe) < _simRules->tribes.size()) nation = ron::sim::to_utf8(_simRules->tribes[size_t(L.tribe)].name);
	if (L.name.empty()) return nation.empty() ? "Seat " + std::to_string(who + 1) : nation;
	return nation.empty() || L.name == nation ? L.name : L.name + " (" + nation + ")";
}

- (void)pbCollectSeats {
	_pbv->seats.clear();
	for (int who = 0; who < 8; who++) {
		const ron::sim::Leader& L = _game->leaders[size_t(who)];
		if (!(L.flags & ron::sim::leaderflag::kValid)) continue;
		recplay::Seat s;
		s.who = who;
		s.label = [self pbSeatLabel:who];
		s.color = L.color;
		_pbv->seats.push_back(std::move(s));
	}
	if (_pbv->seats.empty()) {
		recplay::Seat s;
		s.who = 0, s.label = "Seat 1";
		_pbv->seats.push_back(s);
	}
}

// The seat whose game the HUD shows (console.play: seat-local, never feeds the simulation), and the camera on its first unit.
- (void)pbSelectSeat:(int)index {
	const int n = int(_pbv->seats.size());
	if (n == 0 || !_game) return;
	_pbv->seat = ((index % n) + n) % n;
	const int who = _pbv->seats[size_t(_pbv->seat)].who;
	_game->local_play = who;
	_cm.play = who, _cm.play_who = who;
	_selected.clear(), _selectedBuilds.clear();
	_fogUploaded = NO;
	_notifier.reset(*_game);
	const ron::sim::ObjectList& ol = _game->objects[size_t(who & 7)];
	for (int o = 0; o < ol.unit_mark && o < int(ol.units.size()); o++) {
		const ron::sim::Unit* u = ol.units[size_t(o)].get();
		if (u && u->alive()) {
			_lookAt = simd_make_float3(float(u->x), float(u->y), 0);
			break;
		}
	}
}

- (void)harnessGameMenuChoose:(int)i {
	[self gameMenuChoose:i];
}

- (void)pbSelectWho:(int)who {
	for (size_t k = 0; k < _pbv->seats.size(); k++)
		if (_pbv->seats[k].who == who) [self pbSelectSeat:int(k)];
}

// One display frame's worth of playback: the frames the speed asks for, or a slice of a seek.
- (void)pbTick:(double)dtMs modal:(BOOL)modal {
	recplay::Viewer& v = *_pbv;
	ron::net::Playback& pb = *v.pb;
	if (v.seek_to >= 0) {  // a seek runs the recording forward as far as a slice of the display frame allows
		const double t0 = CACurrentMediaTime();
		while (pb.frame() < v.seek_to && !pb.at_end() && CACurrentMediaTime() - t0 < 0.03) pb.step();
		if (pb.frame() >= v.seek_to || pb.at_end()) {
			v.seek_to = -1;
			v.ended = pb.at_end();
			[self afterGameReplaced];
			[self pbSelectSeat:v.seat];
			if (v.ended) [self pbBattleEnded];
		}
		return;
	}
	if (_paused || modal || v.ended) return;
	_simMs += dtMs;
	const double frame_ms = recplay::kFrameMs[std::clamp(v.speed, 0, recplay::kSpeedCount - 1)];
	int steps;
	if (frame_ms <= 0) {  // as fast as the machine allows: a slice of the display frame
		steps = 0;
		const double t0 = CACurrentMediaTime();
		while (!v.ended && CACurrentMediaTime() - t0 < 0.012) {
			[self pbStep:1];
			steps++;
		}
		_simMs = 0;
	} else {
		steps = int(_simMs / frame_ms);
		const int max_steps = v.speed >= 3 ? 40 : 4;
		if (steps > max_steps) {
			_simMs = std::fmod(_simMs, frame_ms);
			steps = max_steps;
		} else {
			_simMs -= steps * frame_ms;
		}
		[self pbStep:steps];
	}
	if (steps && (_game->frame % 15) == 0) [self updateTitle];
}

- (void)pbStep:(int)frames {
	ron::net::Playback& pb = *_pbv->pb;
	for (int i = 0; i < frames; i++) {
		if (pb.at_end()) {
			[self pbReachedEnd];
			break;
		}
		if (_headless) {
			_timeMs += kSimFrameMs;  // the virtual clock, as stepSimulation keeps it
			[self cameraStep:kSimFrameMs];
		}
		pb.step();
		_prevView = std::move(_curView);
		[self indexPrevView];
		_curView = ron::sim::make_view(*_game);
		[self notifyFrame];
		if (pb.at_end()) {
			[self pbReachedEnd];
			break;
		}
	}
	if (frames > 0) [self syncSimBuildings];
}

- (void)pbReachedEnd {
	if (_pbv->ended) return;
	_pbv->ended = true;
	if (!_pbv->announced_end) {
		_pbv->announced_end = true;
		[self say:"End of the recording."];
	}
	[self pbBattleEnded];
}

// A Conquer the World battle recording reached its end: the line the battle's final state gives (who won, who was defeated, or the count of what
// is left when nobody was) goes under the timeline and into the message line. The campaign is not touched: this viewer has no campaign screens.
- (void)pbBattleEnded {
	if (!_pbv || !_game || !_pbv->pb->ctw_battle() || !_pbv->outcome.empty()) return;
	_pbv->outcome = [self pbBattleOutcome];
	if (!_pbv->outcome.empty()) [self say:_pbv->outcome];
}

- (std::string)pbBattleOutcome {
	auto join = [](const std::vector<std::string>& v) {
		std::string t;
		for (size_t i = 0; i < v.size(); i++) t += (i ? (i + 1 == v.size() ? " and " : ", ") : "") + v[i];
		return t;
	};
	std::vector<std::string> won, lost;
	std::array<bool, 8> seen{};
	for (const ron::sim::GameEvent& ev : _game->events) {
		if ((ev.kind != 2 && ev.kind != 3) || ev.who < 0 || ev.who >= 8 || seen[size_t(ev.who)]) continue;
		seen[size_t(ev.who)] = true;
		(ev.kind == 3 ? won : lost).push_back([self pbSeatLabel:ev.who]);
	}
	if (!won.empty() || !lost.empty()) {
		std::string t = "Battle over: ";
		if (!won.empty()) t += join(won) + " won";
		if (!won.empty() && !lost.empty()) t += "; ";
		if (!lost.empty()) t += join(lost) + (lost.size() == 1 ? " was defeated" : " were defeated");
		// the script's own game-over text for the seat on screen (the victory or defeat message the battle's scripts set)
		const ron::sim::ScriptPresentation* p = [self pres];
		const int me = _game->local_play;
		if (p && me >= 0 && me < 8 && seen[size_t(me)]) {
			const bool iwon = std::find(won.begin(), won.end(), [self pbSeatLabel:me]) != won.end();
			const std::string& msg = iwon ? p->victory_message : p->defeat_message;
			if (!msg.empty()) t += ". " + msg;
		}
		return t;
	}
	// nobody was defeated when the recording stopped: the sides (teams) by what each has left
	std::map<int, std::pair<std::vector<std::string>, int>> sides;
	for (int w = 0; w < 8; w++) {
		if (!(_game->leaders[size_t(w)].flags & ron::sim::leaderflag::kValid)) continue;
		int alive = 0;
		const ron::sim::ObjectList& ol = _game->objects[size_t(w)];
		for (int32_t k = 0; k < ol.unit_mark; k++) alive += _game->unit(w, k) && _game->unit(w, k)->alive();
		for (int32_t k = ron::sim::kBuildBase; k < ol.build_mark; k++) alive += _game->build(w, k) && _game->build(w, k)->alive();
		const int team = _game->info.team[size_t(w)];
		auto& side = sides[team >= 0 ? team : 100 + w];
		side.first.push_back([self pbSeatLabel:w]);
		side.second += alive;
	}
	if (sides.size() < 2) return std::string();
	std::vector<std::pair<int, std::string>> left;   // what each side has left, the largest first
	for (const auto& kv : sides) left.push_back({kv.second.second, join(kv.second.first)});
	std::stable_sort(left.begin(), left.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
	std::string t = "The recording ends with the battle undecided. Left: ";
	for (size_t i = 0; i < left.size(); i++) t += (i ? ", " : "") + left[i].second + " " + std::to_string(left[i].first);
	return t + " units and buildings";
}

// Jumps to a frame: forward runs the recording up to it, backward builds the start state again first.
- (void)pbSeek:(int64_t)target {
	if (!_pbv) return;
	recplay::Viewer& v = *_pbv;
	ron::net::Playback& pb = *v.pb;
	target = std::clamp<int64_t>(target, pb.start_frame(), pb.end_frame());
	if (target < pb.frame() || (v.ended && target < pb.end_frame())) {
		std::string err;
		ron::net::PlaybackEnv env;
		env.files = _files.get(), env.rules = _simRules.get(), env.art = _art.get();
		std::unique_ptr<ron::sim::Game> g = pb.make_start(err, env);
		if (!g) {
			[self say:"The recording cannot be rewound: " + err];
			return;
		}
		_game = std::move(g);
		pb.bind(*_game);
		[self afterGameReplaced];
		v.ended = false, v.announced_end = false;
		v.outcome.clear();
		[self pbCollectSeats];
	}
	v.seek_to = target > pb.frame() ? target : -1;
	if (v.seek_to < 0) [self pbSelectSeat:v.seat];
	else _pbScrub = NO;
	if (_headless && v.seek_to >= 0) {   // no display frames to spread the run over
		while (pb.frame() < v.seek_to && !pb.at_end()) pb.step();
		v.seek_to = -1;
		v.ended = pb.at_end();
		[self afterGameReplaced];
		[self pbSelectSeat:v.seat];
		if (v.ended) [self pbBattleEnded];
	}
}

// The rewind test (--pb-seek F1,F2; frames count from the recording's start). The straight run to F2 is the reference; then the playback goes to F1,
// comes back to F2 (a rewind when F1 > F2: the start state is built again and run forward) and restarts from the beginning to F2 once more. The state
// hash of playback_state_hash must be the same all three times.
- (int)pbSeekTest:(int64_t)f1 to:(int64_t)f2 {
	if (!_pbv) return 2;
	ron::net::Playback& pb = *_pbv->pb;
	const int64_t base = pb.start_frame();
	auto hash = [&](const char* what, int64_t want, uint64_t& h) {
		const bool ok = ron::net::playback_state_hash(pb, h);
		std::printf("  %-28s frame %d (asked %lld)  state hash %016llx%s\n", what, pb.frame(), (long long)want, (unsigned long long)h, ok ? "" : "  (cannot be hashed)");
		return ok && (pb.frame() == want || pb.at_end());
	};
	if (pb.frame() != base) {
		std::printf("seek test: the recording is not at its start (frame %d)\n", pb.frame());
		return 2;
	}
	std::printf("%s: seek test, straight to +%lld, seek to +%lld, back to +%lld, restart to +%lld\n", pb.path().c_str(), (long long)f2, (long long)f1, (long long)f2, (long long)f2);
	uint64_t straight = 0, via = 0, restart = 0, mid = 0;
	bool ok = true;
	[self pbSeek:base + f2];
	ok = hash("straight run", base + f2, straight) && ok;
	[self pbSeek:base + f1];
	ok = hash("seek to F1", base + f1, mid) && ok;
	[self pbSeek:base + f2];
	ok = hash(f1 > f2 ? "back to F2 (rewound)" : "on to F2", base + f2, via) && ok;
	[self pbSeek:base];
	[self pbSeek:base + f2];
	ok = hash("restart, then to F2", base + f2, restart) && ok;
	const bool same = straight == via && straight == restart;
	if (f1 <= f2) std::printf("  note: F1 <= F2, so the second step ran forward; only the restart rewound\n");
	std::printf("seek test: %s\n", ok && same ? "PASS (the state at F2 is the same)" : "FAIL (the state at F2 differs or a seek missed its frame)");
	return ok && same ? 0 : 1;
}

- (void)pbSpeedStep:(int)d {
	_pbv->speed = std::clamp(_pbv->speed + d, 0, recplay::kSpeedCount - 1);
	[self say:std::string("Playback speed: ") + recplay::kSpeedNames[_pbv->speed]];
}

- (recplay::BarState)pbBarState {
	recplay::BarState s;
	const ron::net::Playback& pb = *_pbv->pb;
	s.frame = _pbScrub ? _pbScrubFrame : pb.frame();
	s.start = pb.start_frame(), s.end = pb.end_frame();
	s.paused = _paused, s.ended = _pbv->ended, s.fog_on = _fogOn, s.seeking = _pbv->seek_to >= 0, s.speed = _pbv->speed;
	s.seat_label = _pbv->seats.empty() ? std::string() : _pbv->seats[size_t(_pbv->seat)].label;
	s.sync = pb.sync_text();
	const ron::net::PlaybackSync& sy = pb.sync();
	s.sync_kind = !sy.has_rows || sy.compared == 0 ? 0 : sy.first_out_of_sync >= 0 ? 2 : 1;
	s.name = _pbv->outcome.empty() ? _pbv->note : _pbv->note.empty() ? _pbv->outcome : _pbv->note + "  -  " + _pbv->outcome;
	s.hover = _pbv->hit(int(_mouse.x), int(_mouse.y));
	if (s.hover >= recplay::kBtnCount) s.hover = recplay::kBtnNone;
	return s;
}

- (void)drawPlaybackBar {
	if (!_pbv) return;
	_pbv->draw(_ui, _uiW, _uiH, [self pbBarState]);
}

// A press on the bar: 1 the bar took it.
- (BOOL)pbBarClick:(CGPoint)p {
	const int hit = _pbv->hit(int(p.x), int(p.y));
	if (hit == recplay::kBtnNone) return NO;
	recplay::Viewer& v = *_pbv;
	switch (hit) {
		case recplay::kBtnRestart: [self pbSeek:v.pb->start_frame()]; _paused = NO; break;
		case recplay::kBtnPause:
			if (v.ended) {
				[self pbSeek:v.pb->start_frame()];
				_paused = NO;
			} else {
				_paused = !_paused;
			}
			break;
		case recplay::kBtnSlower: [self pbSpeedStep:-1]; break;
		case recplay::kBtnFaster: [self pbSpeedStep:1]; break;
		case recplay::kBtnPrev: [self pbSelectSeat:v.seat - 1]; break;
		case recplay::kBtnNext: [self pbSelectSeat:v.seat + 1]; break;
		case recplay::kBtnFog: _fogOn = !_fogOn; _fogUploaded = NO; break;
		case recplay::kBtnExit:
			if (self.onExit) self.onExit(NO);
			break;
		case recplay::kBtnTimeline:
			_pbScrub = YES;
			_pbScrubFrame = v.frame_at(int(p.x), [self pbBarState]);
			break;
		default: break;
	}
	return YES;
}

- (void)pbBarDrag:(CGPoint)p {
	if (_pbScrub) _pbScrubFrame = _pbv->frame_at(int(p.x), [self pbBarState]);
}

- (void)pbBarRelease:(CGPoint)p {
	if (!_pbScrub) return;
	_pbScrub = NO;
	[self pbSeek:_pbv->frame_at(int(p.x), [self pbBarState])];
}

// Keys of the viewer: Tab / Shift+Tab the next / previous seat, [ and ] thirty seconds back / forward, Home the start. 1 handled.
- (BOOL)pbKey:(NSEvent*)event {
	if (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) return NO;
	switch (event.keyCode) {
		case 48: [self pbSelectSeat:_pbv->seat + ((event.modifierFlags & NSEventModifierFlagShift) ? -1 : 1)]; return YES;
		case 33: [self pbSeek:_pbv->pb->frame() - 450]; return YES;
		case 30: [self pbSeek:_pbv->pb->frame() + 450]; return YES;
		case 115: [self pbSeek:_pbv->pb->start_frame()]; _paused = NO; return YES;
		default: return NO;
	}
}

// Headless (--playback with --frames / --snapshot): steps `frames` after the start (< 0: to the end) and prints what ron-net playback prints.
- (BOOL)pbRunHeadless:(int64_t)frames {
	if (!_pbv) return NO;
	ron::net::Playback& pb = *_pbv->pb;
	const int64_t target = frames < 0 ? INT64_MAX : pb.start_frame() + frames;
	std::printf("%s: playback of a recording of this port, frames %lld to %lld, %u turns%s\n", pb.path().c_str(), (long long)pb.start_frame(), (long long)pb.end_frame(),
	            pb.turns_total(), pb.has_check_sums() ? ", CHECK_SUMS rows present" : "");
	while (!pb.at_end() && pb.frame() < target) [self pbStep:1];
	if (pb.at_end()) _pbv->ended = true;
	if (const char* t = getenv("RON_PB_TICKS")) {   // tests: N display frames of DT ms at speed SPEED through the real-time path (RON_PB_TICKS=N,SPEED,DT)
		int n = 0, sp = _pbv->speed;
		double dt = 66.7;
		std::sscanf(t, "%d,%d,%lf", &n, &sp, &dt);
		_pbv->speed = std::clamp(sp, 0, recplay::kSpeedCount - 1);
		const int32_t f0 = _game->frame;
		for (int i = 0; i < n; i++) [self pbTick:dt modal:NO];
		std::printf("  %d display frames at speed %s: frame %d -> %d%s\n", n, recplay::kSpeedNames[_pbv->speed], f0, _game->frame, _pbv->ended ? ", at the end" : "");
	}
	if (!_pbv->outcome.empty()) std::printf("  outcome: %s\n", _pbv->outcome.c_str());
	ron::net::playback_print_sync(pb);
	return YES;
}

// The messages, banners and pings the simulation's frame caused (notices.hpp), for the local seat.
- (void)notifyFrame {
	if (_game && !_leaderOptsSent) {  // a new game: the Options page's LEADER_OPTIONS, once, while it is still young
		_leaderOptsSent = YES;
		const prefs::Prefs& P = prefs::get();  // (loadProfile covers the interactive app; a headless run reads the file here)
		if (P.has("message_length")) scriptview::set_message_length(P.message_length);
		if (!_scoreWin && P.has("score_window")) _scoreWin = 1 + P.score_window;
		if (_game->frame <= 30) [self issueLeaderOptions:P];
	}
	if (!_game || !_sviewOn) return;
	notices::Env env;
	env.game = _game.get(), env.view = &_curView, env.me = [self localPlayer];
	env.type_name = [self](int t) { return [self typeName:t]; };
	env.nation = [self](int who) {
		if (!_simRules || who < 0 || who >= int(_game->leaders.size())) return std::string();
		const int tr = _game->leaders[size_t(who)].tribe;
		return tr >= 0 && size_t(tr) < _simRules->tribes.size() ? ron::sim::to_utf8(_simRules->tribes[size_t(tr)].name) : std::string();
	};
	env.leader = [self, nation = env.nation](int who) {
		const std::string& n = _game->leaders[size_t(who)].name;
		return n.empty() ? nation(who) : n;
	};
	std::vector<notices::Note> notes;
	_notifier.update(env, notes);
	if (notes.empty()) return;
	const scriptview::Env senv = [self scriptEnv];
	const scriptview::Callbacks cb = [self scriptCallbacks];
	for (const notices::Note& n : notes) {
		if (n.kind == notices::Note::Flash) {  // Console::flash_resource_flags / _stamp: the panel rows turn red for 8 ticks
			_flashMask = n.goods, _flashTick = int(_timeMs / 66.0);
			continue;
		}
		if (_scriptLog) std::printf("  f%-5d NOTE %s \"%s\"%s\n", _game->frame, n.kind == notices::Note::Notice ? "notice" : "message", n.text.c_str(), n.located ? " (located)" : "");
		_sview.post(n, senv, cb);
	}
}

// GROUP + MOVE_TO from leader `who`'s seat, executed at once (several process_turn calls per frame are allowed; each
// package is stamped with one seat, so one owner per call).
- (BOOL)issueMoveWho:(int)who objects:(const std::vector<int32_t>&)objs x:(int32_t)x y:(int32_t)y {
	if (!_game) return NO;
	_cm.play = who;
	_cm.play_who = who;
	if (!_cm.issue_move_to(*_game, objs, x, y)) return NO;
	_cm.process_turn(*_game);
	return YES;
}

// The ground point under a drawable pixel: invert screenOf on the terrain (a few fixed-point steps for the height).
- (simd_float2)groundAt:(CGPoint)p {
	const CGSize size = [self drawSize];
	const float upp = units_per_pixel(_zoom, float(size.width));
	const float R = (float(p.x) - float(size.width) / 2) * upp, U = (float(size.height) / 2 - float(p.y)) * upp;
	float h = 0, x = _lookAt.x, y = _lookAt.y;
	for (int it = 0; it < 4; it++) {
		const float diff = R / kRight.x;                          // dx - dy
		const float sum = (kUp.z * (h - _lookAt.z) - U) / -kUp.x;  // dx + dy
		x = _lookAt.x + (sum + diff) / 2;
		y = _lookAt.y + (sum - diff) / 2;
		h = std::max(0.0f, _terrain.sample_height(std::clamp(x, 0.0f, _mesh.width), std::clamp(y, 0.0f, _mesh.height)));
	}
	return simd_make_float2(x, y);
}

// The object under a drawable point for the air orders: the nearest drawn unit within 30 px, else the building whose footprint
// centre is within 450 world units (as commandAttackAt). Returns false with tw = to = -1 when there is none.
- (void)objectAtDrawable:(CGPoint)p who:(int&)tw object:(int&)to {
	tw = to = -1;
	float best = 30;
	for (const SimUnitDrawn& d : _drawnUnits) {
		const simd_float3 base = simd_make_float3(d.x, d.y, std::max(0.0f, _terrain.sample_height(d.x, d.y)));
		const float dist = simd_distance([self screenOf:base + simd_make_float3(0, 0, 60)], simd_make_float2(float(p.x), float(p.y)));
		if (dist < best) best = dist, tw = d.who, to = d.o;
	}
	if (tw >= 0) return;
	const simd_float2 g = [self groundAt:p];
	if (getenv("RON_UIX_TRACE")) std::fprintf(stderr, "[uix] click ground (%.0f, %.0f), %zu placed buildings\n", g.x, g.y, _placed.size());
	float bestw = 450;
	for (const PlacedBuilding& b : _placed) {
		if (b.who < 0) continue;
		if (_fogOn && !ron::sim::build_seen_by(*_game, _game->local_play, b.who, b.o)) continue;
		const float dist = simd_distance(g, simd_make_float2(b.x, b.y));
		if (dist < bestw) bestw = dist, tw = b.who, to = b.o;
	}
}

// A targeting option of the third pass (patrol, guard, bombard, transfer, air mission) takes the click.
- (BOOL)targetClickAt:(CGPoint)p modifiers:(NSEventModifierFlags)m {
	int tw, to;
	[self objectAtDrawable:p who:tw object:to];
	const simd_float2 g = [self groundAt:p];
	const int32_t x = int32_t(std::lround(std::clamp(g.x, 0.0f, _mesh.width - 1))), y = int32_t(std::lround(std::clamp(g.y, 0.0f, _mesh.height - 1)));
	uix::Ctx uc = [self uixCtx];
	const uix::Mods um{(m & NSEventModifierFlagShift) != 0, (m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0, (m & NSEventModifierFlagOption) != 0};
	return uix::target_click(uc, _targeting, x, y, tw, to, um);
}

- (void)commandMoveAt:(CGPoint)p {
	if (!_game || _selected.empty()) return;
	{
		// Aircraft selected: FLIGHT to attack an enemy / rebase onto a friendly airbase or carrier, a patrol on the ground.
		uix::Ctx uc = [self uixCtx];
		if (uix::all_air(uc)) {
			int tw, to;
			[self objectAtDrawable:p who:tw object:to];
			const simd_float2 g = [self groundAt:p];
			const int32_t x = int32_t(std::lround(std::clamp(g.x, 0.0f, _mesh.width - 1))), y = int32_t(std::lround(std::clamp(g.y, 0.0f, _mesh.height - 1)));
			const NSEventModifierFlags m = [NSEvent modifierFlags];
			const uix::Mods um{(m & NSEventModifierFlagShift) != 0, false, (m & NSEventModifierFlagOption) != 0};
			if (uix::right_click(uc, x, y, tw, to, um)) return;
		}
	}
	if ([self commandAttackAt:p]) return;
	const simd_float2 g = [self groundAt:p];
	const int32_t x = int32_t(std::lround(std::clamp(g.x, 0.0f, _mesh.width - 1))),
	              y = int32_t(std::lround(std::clamp(g.y, 0.0f, _mesh.height - 1)));
	std::map<int32_t, std::vector<int32_t>> by_owner;
	for (const auto& [who, o] : _selected) by_owner[who].push_back(o);
	for (const auto& [who, objs] : by_owner) [self issueMoveWho:who objects:objs x:x y:y];
	std::printf("move %zu unit(s) to (%d, %d) at frame %d\n", _selected.size(), x, y, _game->frame);
}

// OPTION_GARRISON's target: the own or allied building under the cursor takes the selected units that fit (GARRISON).
- (BOOL)commandGarrisonAt:(CGPoint)p {
	if (!_game || _selected.empty()) return NO;
	const simd_float2 g = [self groundAt:p];
	float bestw = 450;
	int32_t tw = -1, to = -1;
	for (const PlacedBuilding& b : _placed) {
		if (b.who < 0 || (_fogOn && !ron::sim::build_seen_by(*_game, _game->local_play, b.who, b.o))) continue;
		const float dist = simd_distance(g, simd_make_float2(b.x, b.y));
		if (dist < bestw) bestw = dist, tw = b.who, to = b.o;
	}
	if (tw < 0 || to < ron::sim::kBuildBase) return NO;
	std::map<int32_t, std::vector<int32_t>> garrison;
	for (const auto& [who, o] : _selected) {
		const ron::sim::Unit* u = _game->unit(who, o);
		if (u && u->alive() && ron::sim::is_ally(*_game, who, tw) && ron::sim::unit_can_garrison(*_game, *u, to, tw)) garrison[who].push_back(o);
	}
	for (const auto& [who, objs] : garrison) {
		_cm.play = who;
		_cm.play_who = who;
		if (_cm.issue_garrison(*_game, objs, to, tw)) _cm.process_turn(*_game);
	}
	std::printf("garrison into %d:%d with %zu owner group(s) at frame %d\n", tw, to, garrison.size(), _game->frame);
	return !garrison.empty();
}

// Right-click on a drawn unit or building of another player: ATTACK for each owner's selected units that are at war
// with it. Returns false when nothing hostile is under the cursor.
- (BOOL)commandAttackAt:(CGPoint)p {
	int32_t tw = -1, to = -1;
	float best = 30;  // pixels
	for (const SimUnitDrawn& d : _drawnUnits) {
		const simd_float3 base = simd_make_float3(d.x, d.y, std::max(0.0f, _terrain.sample_height(d.x, d.y)));
		const float dist = simd_distance([self screenOf:base + simd_make_float3(0, 0, 60)], simd_make_float2(float(p.x), float(p.y)));
		if (dist < best) best = dist, tw = d.who, to = d.o;
	}
	if (tw < 0) {
		const simd_float2 g = [self groundAt:p];
		float bestw = 450;  // world units from the footprint centre
		for (const PlacedBuilding& b : _placed) {
			if (b.who < 0) continue;
			if (_fogOn && !ron::sim::build_seen_by(*_game, _game->local_play, b.who, b.o)) continue;
			const float dist = simd_distance(g, simd_make_float2(b.x, b.y));
			if (dist < bestw) bestw = dist, tw = b.who, to = b.o;
		}
	}
	if (tw < 0) return NO;
	std::map<int32_t, std::vector<int32_t>> by_owner;
	for (const auto& [who, o] : _selected) {
		if (ron::sim::is_enemy(*_game, who, tw)) by_owner[who].push_back(o);
	}
	if (by_owner.empty()) {
		// A friendly building under the cursor: the selected units that fit in it garrison (GARRISON).
		if (to < ron::sim::kBuildBase) return NO;
		std::map<int32_t, std::vector<int32_t>> garrison;
		for (const auto& [who, o] : _selected) {
			const ron::sim::Unit* u = _game->unit(who, o);
			if (u && u->alive() && ron::sim::is_ally(*_game, who, tw) && ron::sim::unit_can_garrison(*_game, *u, to, tw))
				garrison[who].push_back(o);
		}
		if (garrison.empty()) return NO;
		for (const auto& [who, objs] : garrison) {
			_cm.play = who;
			_cm.play_who = who;
			if (_cm.issue_garrison(*_game, objs, to, tw)) _cm.process_turn(*_game);
		}
		std::printf("garrison into %d:%d with %zu owner group(s) at frame %d\n", tw, to, garrison.size(), _game->frame);
		return YES;
	}
	for (const auto& [who, objs] : by_owner) {
		_cm.play = who;
		_cm.play_who = who;
		if (_cm.issue_attack(*_game, objs, to, tw)) _cm.process_turn(*_game);
	}
	std::printf("attack %d:%d with %zu owner group(s) at frame %d\n", tw, to, by_owner.size(), _game->frame);
	return YES;
}

- (BOOL)issueAttackWho:(int)who objects:(const std::vector<int32_t>&)objs targetWho:(int)tw target:(int)to {
	if (!_game) return NO;
	_cm.play = who;
	_cm.play_who = who;
	if (!_cm.issue_attack(*_game, objs, to, tw)) return NO;
	_cm.process_turn(*_game);
	return YES;
}

// The unit graphic of a simulated unit: the scenario's resolution for units loaded from the file, else the type's
// DEFAULT style graphic (units created during play).
- (int)graphicForUnit:(const ron::sim::UnitView&)u {
	auto it = _scenarioGraphic.find({u.who, u.o});
	if (it != _scenarioGraphic.end()) return it->second;
	auto t = _typeGraphic.find(u.type);
	if (t != _typeGraphic.end()) return t->second;
	const std::string base = ron::sim::to_utf8(_game->type(u.type).graph_name);
	int g = -1;
	for (const char* age : {"-DEFAULT-AGE0", "-DEFAULT-AGE3", "-DEFAULT-AGE5", ""}) {
		if ((g = _units.find(base + age)) >= 0) break;
	}
	return _typeGraphic[u.type] = g;
}

// The unit graphic of a guy's graphics piece (the <UNIT> name the original draws), else `fallback`.
- (int)graphicForPiece:(int32_t)piece fallback:(int)fallback {
	if (!_art || piece < 0) return fallback;
	auto it = _pieceGraphic.find(piece);
	if (it == _pieceGraphic.end()) {
		const std::string name = _art->piece_name(piece);
		it = _pieceGraphic.emplace(piece, name.empty() ? -1 : _units.find(name)).first;
		if (it->second < 0 && !name.empty()) std::fprintf(stderr, "graphics piece %d (%s): no unit graphic\n", piece, name.c_str());
	}
	return it->second >= 0 ? it->second : fallback;
}

// The clip of `graphic` for animation `anim` (CHAR_*), with fallbacks for animations a graphic does not have.
- (int)clipFor:(int)graphic anim:(int)anim {
	const auto key = std::make_pair(graphic, anim);
	auto it = _clipIndex.find(key);
	if (it != _clipIndex.end()) return it->second;
	const std::vector<ron::UnitAnim>& anims = _units.units()[size_t(graphic)].anims;
	auto find = [&](const char* name) {
		for (size_t i = 0; i < anims.size(); i++) {
			if (anims[i].action == name && !anims[i].file.empty()) return int(i);
		}
		return -1;
	};
	int idx = anim >= 0 && anim < kNumCharNames ? find(kCharNames[anim]) : -1;
	if (idx < 0 && anim >= 7 && anim <= 10) {  // SLOG, WALK, JOG, ATTACKWALK
		for (const char* n : {"CHAR_WALK", "CHAR_JOG", "CHAR_SLOG"}) {
			if ((idx = find(n)) >= 0) break;
		}
	}
	if (idx < 0 && anim >= 4 && anim <= 6) idx = find("CHAR_IDLE1");  // GROUP_IDLE*
	if (idx < 0) idx = find("CHAR_DEFAULT");
	if (idx < 0) idx = anims.empty() ? -1 : 0;
	return _clipIndex[key] = idx;
}

// Fog levels for the local player (0 unexplored, 1 explored, 2 seen before, 3 visible) from the simulation's fog
// (make_fog_view); everything visible with fog off or without the simulation. Test patterns: RON_FOG_TEST=1 (around
// units), RON_FOG_LEVEL=N (all cells at N) or =MN (a checker of levels M and N).
- (void)updateFog {
	if (!_fogVB) return;
	static const bool fogEnv = getenv("RON_FOG_TEST") || getenv("RON_FOG_LEVEL");  // test hooks, read once (not per frame)
	if (_game && _fogOn && !fogEnv) {  // the normal path: no per-frame copy, only a compare against the uploaded levels
		ron::sim::make_fog_view(*_game, _game->local_play, _fogView);
		if (_fogView.fog_xs == _fog.fxs && _fogView.fog_ys == _fog.fys) {
			if (_fogUploaded && _fogView.level == _fogLevels) return;
			_fogLevels = _fogView.level;
			ron::update_fog_overlay(_fog, _fogLevels.data());
			std::memcpy(_fogVB.contents, _fog.vertices.data(), _fog.vertices.size() * sizeof(ron::ColorVertex));
			_fogUploaded = true;
			[self applyFogToTrees];
			return;
		}
	}
	std::vector<uint8_t> levels(_fogLevels.size(), 3);
	if (getenv("RON_FOG_LEVEL")) {
		// "N": every cell at level N; "MN": a checker of 6-cell squares at levels M and N.
		const char* v = getenv("RON_FOG_LEVEL");
		for (int fy = 0; fy < _fog.fys; fy++) {
			for (int fx = 0; fx < _fog.fxs; fx++) {
				const bool odd = v[1] && ((fx / 6 + fy / 6) & 1);
				levels[size_t(fy * _fog.fxs + fx)] = uint8_t((odd ? v[1] : v[0]) - '0');
			}
		}
	} else if (getenv("RON_FOG_TEST")) {
		std::fill(levels.begin(), levels.end(), 0);
		for (int fy = 0; fy < _fog.fys; fy++) {
			for (int fx = 0; fx < _fog.fxs; fx++) {
				if ((fx / 6 + fy / 6) % 2) levels[size_t(fy * _fog.fxs + fx)] = 1;
			}
		}
		for (const SimUnitDrawn& d : _drawnUnits) {
			const int cx = int(d.x / 384), cy = int(d.y / 384);
			for (int dy = -6; dy <= 6; dy++) {
				for (int dx = -6; dx <= 6; dx++) {
					const int fx = cx + dx, fy = cy + dy;
					if (fx < 0 || fy < 0 || fx >= _fog.fxs || fy >= _fog.fys) continue;
					const int r2 = dx * dx + dy * dy;
					uint8_t& l = levels[size_t(fy * _fog.fxs + fx)];
					if (r2 <= 36) l = std::max<uint8_t>(l, r2 <= 16 ? 3 : 2);
				}
			}
		}
	}
	if (levels == _fogLevels && _fogUploaded) return;
	_fogLevels = std::move(levels);
	ron::update_fog_overlay(_fog, _fogLevels.data());
	std::memcpy(_fogVB.contents, _fog.vertices.data(), _fog.vertices.size() * sizeof(ron::ColorVertex));
	_fogUploaded = true;
	[self applyFogToTrees];
}

// Trees in unexplored cells are not drawn; trees in cells not currently seen are darkened (DARK_GREY instead of the
// normal GREY diffuse: half the colour).
- (void)applyFogToTrees {
	if (!_treeVB || _treeLevel.empty() || _fogLevels.empty()) return;
	bool visible = false;
	ron::ColorVertex* vb = static_cast<ron::ColorVertex*>(_treeVB.contents);
	for (size_t t = 0; t < _treeLevel.size(); t++) {
		const uint8_t l = _fogLevels[size_t(_treeFogCell[t])];
		if (l == _treeLevel[t]) continue;
		if ((l == 0) != (_treeLevel[t] == 0)) visible = true;
		if ((l == 3) != (_treeLevel[t] == 3)) {
			const int k = l == 3 ? 2 : 1;  // x 1 or x 1/2
			for (uint32_t v = _treeFirstVert[t]; v < _treeFirstVert[t + 1]; v++) {
				const ron::ColorVertex& src = _treeVerts[v];
				vb[v].r = uint8_t(src.r * k / 2), vb[v].g = uint8_t(src.g * k / 2), vb[v].b = uint8_t(src.b * k / 2);
			}
		}
		_treeLevel[t] = l;
	}
	if (!visible) return;
	for (int i = 0; i < ron::kTreeSlots; i++) {
		if (!_treeIB[i]) continue;
		uint32_t* ib = static_cast<uint32_t*>(_treeIB[i].contents);
		size_t n = 0;
		for (size_t tri = 0; tri < _treeTriTree[i].size(); tri++) {
			if (_treeLevel[_treeTriTree[i][tri]] == 0) continue;
			std::memcpy(ib + n, &_treeTris[i][tri * 3], 12);
			n += 3;
		}
		_treeCount[i] = n;
	}
}

// The local player's fog level (0..3) at a world position; 3 without fog data.
- (int)fogLevelAtX:(float)x y:(float)y {
	if (_fogLevels.empty() || !_fogUploaded) return 3;
	const int fx = std::clamp(int(x / 384), 0, _fog.fxs - 1), fy = std::clamp(int(y / 384), 0, _fog.fys - 1);
	return _fogLevels[size_t(fy) * size_t(_fog.fxs) + size_t(fx)];
}

- (void)encodeFog:(id<MTLRenderCommandEncoder>)enc {
	if (!_fogVB || debug_skip("fog")) return;
	static const ron::RenderState fog_rs = fog_state();
	rm::DrawUniforms du = rm::draw_uniforms(fog_rs, matrix_identity_float4x4);
	_states.bind(enc, fog_rs, rm::VertexKind::Colored);
	[enc setCullMode:MTLCullModeNone];
	[enc setVertexBuffer:_fogVB offset:0 atIndex:0];
	[enc setVertexBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentTexture:_fogTex atIndex:0];
	[enc setFragmentTexture:_fogTex atIndex:1];
	[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:_fog.indices.size() indexType:MTLIndexTypeUInt32
	               indexBuffer:_fogIB indexBufferOffset:0];
}

// Interpolates the last two simulation frames by the time since the last step (graphic_percent).
- (void)buildFigures {
	_figures.clear();
	_drawnUnits.clear();
	if (!_game) return;
	const float t = float(std::clamp(_simMs / kTimings[std::clamp(_speed, 0, 4)], 0.0, 1.0));
	auto mix = [t](int32_t a, int32_t b) { return float(a) + float(b - a) * t; };
	auto angle = [t](uint32_t a, uint32_t b) {
		const double v = double(a) + double(int32_t(b - a)) * double(t);  // the short way round
		return float(v * 360.0 / 4294967296.0) + 180.0f;                  // GuyOut::draw adds 180 degrees
	};
	for (const ron::sim::UnitView& u : _curView.units) {
		if (!u.alive || !u.on_map) continue;
		if (_fogOn && !(u.seen_by & (1u << (_game->local_play & 7)))) continue;  // GraphicPieces::draw_unit: is_seen
		const int g = [self graphicForUnit:u];
		if (g < 0) continue;
		auto pi = _prevIndex.find(sim_key(u.who, u.o));
		const ron::sim::UnitView& p = pi != _prevIndex.end() ? _prevView.units[pi->second] : u;
		SimUnitDrawn d{u.who, u.o, mix(p.x, u.x), mix(p.y, u.y), u.hit_points, u.max_hit_points, _figures.size(), 0, u.type};
		const size_t n = std::max<size_t>(1, u.guys.size());
		for (size_t k = 0; k < n; k++) {
			ron::sim::GuyView cur{u.x, u.y, u.z, u.angle, 0}, prev{p.x, p.y, p.z, p.angle, 0};
			if (k < u.guys.size()) cur = u.guys[k];
			prev = k < p.guys.size() ? p.guys[k] : cur;
			SimFigure f;
			f.graphic = [self graphicForPiece:cur.gpiece fallback:g];
			f.x = mix(prev.x, cur.x);
			f.y = mix(prev.y, cur.y);
			f.angle_deg = angle(prev.angle, cur.angle);
			f.anim = [self clipFor:f.graphic anim:cur.anim];
			if (cur.end_time > 0 && f.anim >= 0) {
				// The sim's animation clock: frame cur_time of end_time, plus the interpolation fraction.
				const double len = _units.clip_ms(f.graphic, f.anim);
				f.clip_ms = std::min(1.0, (double(cur.cur_time) + double(t)) / double(cur.end_time)) * len;
				if (f.clip_ms >= len && len > 0) f.clip_ms = len - 0.5;  // hold the last pose, do not wrap
			} else {
				ClipClock& c = _clipClocks[sim_key(u.who, u.o, int32_t(k))];
				if (c.anim != cur.anim) c.anim = cur.anim, c.start_ms = _timeMs;
				f.clip_ms = _timeMs - c.start_ms;
			}
			f.team = u.color;
			if (const ron::sim::ScriptPresentation* sp = [self pres]) {
				auto us = sp->unit_scale.find(cur.gpiece);
				if (us != sp->unit_scale.end()) f.scale = us->second;
			}
			_figures.push_back(f);
		}
		d.count = _figures.size() - d.first;
		_drawnUnits.push_back(d);
	}
	// Corpses: the death animation, then its last pose until the corpse expires (combat-spec 6). Hidden where the
	// player does not see the cell now (deaths are skipped by is_seen of their cell, fog-spec 9.3).
	for (const ron::sim::CorpseView& c : _curView.corpses) {
		if (_fogOn && [self fogLevelAtX:float(c.x) y:float(c.y)] < 3) continue;
		SimFigure f;
		f.graphic = [self graphicForPiece:c.gpiece fallback:-1];
		if (f.graphic < 0) continue;
		f.x = float(c.x), f.y = float(c.y);
		f.angle_deg = float(double(c.angle) * 360.0 / 4294967296.0) + 180.0f;
		f.anim = [self clipFor:f.graphic anim:c.anim];
		const double len = f.anim >= 0 ? _units.clip_ms(f.graphic, f.anim) : 0;
		f.clip_ms = c.anim_frames > 0 ? std::min(1.0, (double(c.frame) + double(t)) / double(c.anim_frames)) * len : len;
		if (len > 0 && f.clip_ms >= len) f.clip_ms = len - 0.5;
		f.team = 8;
		for (const ron::sim::UnitView& u : _curView.units) {
			if (u.who == c.who) {
				f.team = u.color;
				break;
			}
		}
		_figures.push_back(f);
	}
	// Projectiles: heading from the shot, pitch from the launch-to-aim slope (AmmoOut::draw is not decoded).
	_shots.clear();
	for (const ron::sim::ProjectileView& pv : _curView.projectiles) {
		if (_fogOn && [self fogLevelAtX:float(pv.x) y:float(pv.y)] < 3) continue;
		SimShot sh;
		sh.ammo = _art ? _art->ammo_name(pv.ammo) : std::string();
		if (sh.ammo.empty()) continue;
		sh.x = float(pv.x), sh.y = float(pv.y), sh.z = float(pv.z);
		sh.angle_deg = float(double(pv.angle) * 360.0 / 4294967296.0);  // ammo models point the other way from units
		const float dx = float(pv.ex - pv.sx), dy = float(pv.ey - pv.sy), dz = float(pv.ez - pv.sz);
		sh.pitch_deg = pv.stuck ? 30.0f : std::atan2(dz, std::max(1.0f, std::sqrt(dx * dx + dy * dy))) * 57.29578f;
		_shots.push_back(sh);
	}
}

// The farm ground meshes (wheat field / animal pen underlays) for the current _farms.
- (void)rebuildFarmUnderlays {
	for (int k = 0; k < 2; k++) {
		_farmUnderlays[k] = MeshDraw{};
		const ron::FarmType type = k == 0 ? ron::FarmType::Wheat : ron::FarmType::Animal;
		ron::UnderlayMesh m;
		ron::build_farm_underlays(_terrain, _farms, type, m);
		if (m.indices.empty() || !_farmUnderTex[k]) continue;
		_farmUnderlays[k] = {[_device newBufferWithBytes:m.vertices.data() length:m.vertices.size() * sizeof(ron::ColorVertex2) options:MTLResourceStorageModeShared],
		                     [_device newBufferWithBytes:m.indices.data() length:m.indices.size() * 4 options:MTLResourceStorageModeShared],
		                     m.indices.size(), _farmUnderTex[k], _farmUnderTex[k], nil, nil};
	}
}

// Farms follow the simulation (Game::farms): the set is rebuilt when farms are added or removed (with the sim's
// wheat / pen choice), and every frame the growth, status and construction progress are copied for drawing.
- (void)syncSimFarms {
	if (!_game) return;
	std::vector<std::pair<int32_t, int32_t>> keys;
	std::vector<ron::FarmSite> sites;
	std::vector<const ron::sim::FarmRec*> recs;
	for (const ron::sim::FarmRec& r : _game->farms) {
		if (!r.valid) continue;
		const ron::sim::Build* b = _game->build(r.who, r.o);
		if (!b) continue;
		keys.push_back({r.who, r.o});
		sites.push_back({float(b->x), float(b->y), r.who, r.o});
		recs.push_back(&r);
	}
	if (keys != _farmKeys) {
		_farmKeys = keys;
		_farms = ron::place_farms(_terrain, sites, {});
		for (size_t i = 0; i < _farms.size() && i < recs.size(); i++)
			_farms[i].type = (recs[i]->farm_type & 1) ? ron::FarmType::Animal : ron::FarmType::Wheat;
		[self rebuildFarmUnderlays];
	}
	for (size_t i = 0; i < _farms.size() && i < recs.size(); i++) {
		ron::Farm& f = _farms[i];
		for (int x = 0; x < 4; x++) {
			for (int y = 0; y < 4; y++) {
				f.percent[x][y] = recs[i]->percent[size_t(x)][size_t(y)];
				f.status[x][y] = recs[i]->status[size_t(x)][size_t(y)];
			}
		}
		const ron::sim::BuildView* bv = [self buildView:keys[i].first o:keys[i].second];
		f.build = bv && !bv->active && bv->max_hit_points > 0 ? float(bv->hit_points) / float(bv->max_hit_points) : 1.0f;
	}
	_farmFrame = uint32_t(_game->frame);
}

// Wheat (FarmsOut::draw): rebuilt every frame from the growth state and the wind phase, after the objects.
- (void)encodeWheat:(id<MTLRenderCommandEncoder>)enc {
	[self syncSimFarms];
	if (!_farmTex || _farms.empty()) return;
	if (_fogUploaded && _fogOn) {
		// Farms in unexplored cells are not drawn.
		std::vector<ron::Farm> seen;
		for (const ron::Farm& f : _farms) {
			if ([self fogLevelAtX:f.cx y:f.cy] > 0) seen.push_back(f);
		}
		ron::build_wheat(seen, _farmParams, _farmFrame, _wheat);
	} else {
		ron::build_wheat(_farms, _farmParams, _farmFrame, _wheat);
	}
	if (_wheat.indices.empty()) return;
	static const ron::RenderState farm_rs = farm_state();
	rm::DrawUniforms du = rm::draw_uniforms(farm_rs, matrix_identity_float4x4);
	_states.bind(enc, farm_rs, rm::VertexKind::Colored);
	id<MTLBuffer> vb = [_device newBufferWithBytes:_wheat.vertices.data() length:_wheat.vertices.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
	id<MTLBuffer> ib = [_device newBufferWithBytes:_wheat.indices.data() length:_wheat.indices.size() * 4 options:MTLResourceStorageModeShared];
	[enc setVertexBuffer:vb offset:0 atIndex:0];
	[enc setVertexBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentTexture:_farmTex atIndex:0];
	[enc setFragmentTexture:_farmTex atIndex:1];
	[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:_wheat.indices.size() indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0];
}

// Goods (resources) with a 3D model (effects_graphics.xml <GOOD model=.bh3>). Billboard and underlay goods (oil, fish)
// are not drawn yet.
- (void)placeGood:(const std::string&)name x:(float)x y:(float)y files:(const ron::GameFiles&)files {
	auto it = _goodAssets.find(name);
	if (it == _goodAssets.end()) {
		std::unique_ptr<GoodAsset> a;
		const ron::XmlElement* found = nullptr;
		static const ron::XmlElement effects = ron::parse_xml(files.read_text("Data/effects_graphics.xml"));
		ron::for_each_element(effects, "GOOD", [&](const ron::XmlElement& e) {
			if (!found && e.get("name") == name) found = &e;
		});
		const std::string model = found ? ron::normalize_game_path(found->get("model")) : std::string();
		if (!model.empty() && !files.resolve(model).empty()) {
			try {
				a = std::make_unique<GoodAsset>();
				a->model = std::make_unique<ron::BH3Model>(ron::parse_bh3(ron::read_file(files.resolve(model))));
				const std::string tex = ron::normalize_game_path(found->get("texture"));
				a->texture = tex.empty() || files.resolve(tex).empty() ? _white : [self makeTexture:ron::parse_tga(ron::read_file(files.resolve(tex)))];
				const auto& ix = a->model->meshes[0].indices;
				if (!ix.empty()) a->indices = [_device newBufferWithBytes:ix.data() length:ix.size() * 2 options:MTLResourceStorageModeShared];
			} catch (const std::exception& e) {
				std::fprintf(stderr, "good %s: %s\n", name.c_str(), e.what());
				a.reset();
			}
		}
		it = _goodAssets.emplace(name, std::move(a)).first;
	}
	if (it->second && it->second->indices) _placedGoods.push_back({it->second.get(), x, y});
}

// Goods draw like building add-ons: rotate_z(270) * translate * scale(3, -3, 3), i.e. L + (-3y, -3x, 3z), "Good Blend".
- (void)encodeGoods:(id<MTLRenderCommandEncoder>)enc {
	static const ron::RenderState& good_rs = *ron::find_render_state("Good Blend");
	for (PlacedGood& g : _placedGoods) {
		if (_fogOn && [self fogLevelAtX:g.x y:g.y] == 0) continue;  // goods in unexplored cells (draw_good: was_seen)
		GoodAsset& a = *g.asset;
		ron::pose_model(*a.model, nullptr, 0, a.posed);
		std::vector<ron::ColorVertex> verts;
		verts.reserve(a.posed.size());
		for (const ron::PosedVertex& p : a.posed) verts.push_back({-3 * p.py, -3 * p.px, 3 * p.pz, -p.ny, -p.nx, p.nz, p.u, p.v, 255, 255, 255, 255});
		const float z = std::max(0.0f, _terrain.sample_height(g.x, g.y));
		const simd_float4x4 at = simd_matrix(simd_make_float4(1, 0, 0, 0), simd_make_float4(0, 1, 0, 0), simd_make_float4(0, 0, 1, 0),
		                                     simd_make_float4(g.x, g.y, z, 1));
		rm::DrawUniforms du = rm::draw_uniforms(good_rs, at);
		_states.bind(enc, good_rs, rm::VertexKind::Colored);
		[enc setCullMode:MTLCullModeNone];
		id<MTLBuffer> vb = [_device newBufferWithBytes:verts.data() length:verts.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
		[enc setVertexBuffer:vb offset:0 atIndex:0];
		[enc setVertexBytes:&du length:sizeof du atIndex:1];
		[enc setFragmentBytes:&du length:sizeof du atIndex:1];
		[enc setFragmentTexture:a.texture atIndex:0];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:a.model->meshes[0].indices.size() indexType:MTLIndexTypeUInt16
		               indexBuffer:a.indices indexBufferOffset:0];
	}
}

- (BuildAsset*)buildAsset:(const std::string&)name {
	auto it = _buildAssets.find(name);
	if (it != _buildAssets.end()) return it->second.get();
	std::unique_ptr<BuildAsset> a;
	if (const ron::BuildGraphic* g = _builds.find(name)) {
		try {
			a = std::make_unique<BuildAsset>();
			a->graphic = g;
			if (!g->texture.empty()) {
				const ron::Image img = ron::parse_tga(ron::read_file(_files->resolve(g->texture)));
				a->sprite_w = img.width;
				a->sprite_h = img.height;
				a->sprite = [self makeTexture:img];
			}
			if (!g->zbuffer.empty()) {
				const ron::BH3Model hull = ron::parse_bh3(ron::read_file(_files->resolve(g->zbuffer)));
				std::vector<ron::PosedVertex> posed;
				ron::pose_model(hull, nullptr, 0, posed);
				for (const ron::PosedVertex& p : posed) a->hull.push_back({-3 * p.py, -3 * p.px, 3 * p.pz, 0, 0, 1, 0, 0, 0, 0, 0, 0});
				a->hull_indices = hull.meshes[0].indices;
			}
			for (const ron::BuildAddon& ev : g->addons) {
				if (ev.anim != ron::BuildAnim::Default) continue;
				const ron::AmmoPiece* piece = _ammo.find(ev.subject);
				if (!piece) continue;
				BuildAsset::Addon ad;
				ad.model = std::make_unique<ron::BH3Model>(ron::parse_bh3(ron::read_file(_files->resolve(piece->model))));
				const std::string clip = ev.anim_file.empty() ? piece->anim : ev.anim_file;
				if (!clip.empty() && !_files->resolve(clip).empty()) {
					ad.anim = std::make_unique<ron::Animation>(ron::parse_bha(ron::read_file(_files->resolve(clip))));
				}
				const std::string tex = piece->texture.empty() ? std::string() : _files->resolve(piece->texture);
				ad.texture = tex.empty() ? _white : [self makeTexture:ron::parse_tga(ron::read_file(tex))];
				ad.state = ron::find_render_state(piece->renderstate);
				if (!ad.state) ad.state = ron::find_render_state("Ammo Blend");
				a->addons.push_back(std::move(ad));
			}
		} catch (const std::exception& e) {
			std::fprintf(stderr, "building %s: %s\n", name.c_str(), e.what());
			a.reset();
		}
	}
	BuildAsset* raw = a.get();
	_buildAssets[name] = std::move(a);
	return raw;
}

// Buildings in painter's order (docs/buildings-spec.md section 7): the sprite with depth off (team-colour pass, then the
// rest), the invisible depth hull, then the add-ons depth-tested against it.
- (void)encodeBuildings:(id<MTLRenderCommandEncoder>)enc {
	static const ron::RenderState& team_rs = *ron::find_render_state("Team Color Blend");
	static const ron::RenderState& build_rs = *ron::find_render_state("Building Blend");
	static const ron::RenderState hull_rs = [] {
		ron::RenderState rs = *ron::find_render_state("Building ZBuffer Blend");
		rs.cull = ron::CullMode::None;  // the hull mapping is a mirror
		return rs;
	}();
	static const ron::RenderState& blocked_rs = *ron::find_render_state("Blocked Building Blend");
	std::vector<ron::PosedVertex> posed;
	// The building being placed (Options::draw_cursor_follows): a ghost drawn with the normal passes when the site is
	// valid, with Blocked Building Blend otherwise; no depth hull or add-ons.
	std::vector<PlacedBuilding> ghost;
	if (_game && _placeType >= 0 && _ghostValid) {
		const int me = _game->local_play;
		const std::string name = [self buildingGraphicForType:_placeType who:me];
		if (BuildAsset* asset = name.empty() ? nullptr : [self buildAsset:name]) {
			ghost.push_back({name, _ghostX, _ghostY, std::clamp(_game->leaders[size_t(me)].color, 0, 8), asset, -2, -1});
		}
	}
	std::vector<const PlacedBuilding*> order;
	for (const PlacedBuilding& b : _placed) order.push_back(&b);
	for (const PlacedBuilding& b : ghost) order.push_back(&b);
	for (const PlacedBuilding* bp : order) {
		const PlacedBuilding& b = *bp;
		const bool is_ghost = b.who == -2;
		// Fog: never-seen buildings are not drawn (BuildData::is_seen; remembered ones stay).
		if (_game && _fogOn && b.who >= 0 && !ron::sim::build_seen_by(*_game, _game->local_play, b.who, b.o)) continue;
		const BuildAsset& a = *b.asset;
		const ron::BuildGraphic& g = *a.graphic;
		const float h = g.halfoffset ? 96.0f : 0.0f;
		const simd_float3 L = simd_make_float3(b.x - h, b.y - h, _terrain.sample_height(b.x, b.y));
		// Construction progress (WallOut::draw): p = hit points / max for unfinished buildings, 0 before the start.
		const ron::sim::BuildView* bv = _game && b.who >= 0 ? [self buildView:b.who o:b.o] : nullptr;
		float p = 1.0f;
		bool unfinished = false;
		if (bv && !bv->active) {
			unfinished = true;
			p = bv->started && bv->max_hit_points > 0 ? float(bv->hit_points) / float(bv->max_hit_points) : 0.0f;
		}
		// The sprite is hidden below 0.84 and fades in over 0.84..0.88; the underlay is the mirror image.
		const int sprite_alpha = !unfinished ? 255 : int(255.0f - 255.0f * std::min(1.0f, std::max(0.0f, 25.0f * (0.88f - p))));
		if (unfinished) {
			[self encodeConstructionOf:b graphic:g type:bv->type at:L alpha:255 - sprite_alpha encoder:enc];
			if (bv->started) [self encodeScaffoldOf:b type:bv->type at:L half:h progress:p encoder:enc];
			if (sprite_alpha <= 0) continue;
		}
		static const uint8_t kBlockedTint[3] = {255, 40, 40};  // the blocked ghost's diffuse (inferred; not decoded)
		const uint8_t* tc = is_ghost && _ghostBlocked != 0 ? kBlockedTint : kBuildTeamColors[b.team];
		if (a.sprite) {
			// One texel covers 3 world units along the camera's right and down axes; texel (spritexoff, spriteyoff) sits on L.
			auto corner = [&, sprite_alpha](float i, float j) {
				const simd_float3 p = L + kRight * (3.0f * (i - float(g.spritexoff))) - kUp * (3.0f * (j - float(g.spriteyoff)));
				return ron::ColorVertex{p.x, p.y, p.z, 0, 0, 1, i / float(a.sprite_w), j / float(a.sprite_h), tc[0], tc[1], tc[2],
				                        uint8_t(sprite_alpha)};
			};
			const float w = float(a.sprite_w), hh = float(a.sprite_h);
			const ron::ColorVertex quad[6] = {corner(0, 0), corner(w, 0), corner(0, hh), corner(0, hh), corner(w, 0), corner(w, hh)};
			const bool blocked = is_ghost && _ghostBlocked != 0;
			const ron::RenderState* passes[2] = {blocked ? &blocked_rs : &team_rs, blocked ? nullptr : &build_rs};
			for (const ron::RenderState* rs : passes) {
				if (!rs) continue;
				rm::DrawUniforms du = rm::draw_uniforms(*rs, matrix_identity_float4x4);
				_states.bind(enc, *rs, rm::VertexKind::Colored);
				[enc setCullMode:MTLCullModeNone];
				[enc setVertexBytes:quad length:sizeof quad atIndex:0];
				[enc setVertexBytes:&du length:sizeof du atIndex:1];
				[enc setFragmentBytes:&du length:sizeof du atIndex:1];
				[enc setFragmentTexture:a.sprite atIndex:0];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
			}
		}
		if (is_ghost) continue;
		const simd_float4x4 at = simd_matrix(simd_make_float4(1, 0, 0, 0), simd_make_float4(0, 1, 0, 0), simd_make_float4(0, 0, 1, 0),
		                                     simd_make_float4(L.x, L.y, L.z, 1));
		if (!a.hull_indices.empty()) {
			rm::DrawUniforms du = rm::draw_uniforms(hull_rs, at);
			_states.bind(enc, hull_rs, rm::VertexKind::Colored);
			BuildAsset& ma = *b.asset;  // `a` is const: the buffers are a lazy cache of the asset
			if (!ma.hull_vb) {
				ma.hull_vb = [_device newBufferWithBytes:a.hull.data() length:a.hull.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
				ma.hull_ib = [_device newBufferWithBytes:a.hull_indices.data() length:a.hull_indices.size() * 2 options:MTLResourceStorageModeShared];
			}
			id<MTLBuffer> vb = ma.hull_vb, ib = ma.hull_ib;
			[enc setVertexBuffer:vb offset:0 atIndex:0];
			[enc setVertexBytes:&du length:sizeof du atIndex:1];
			[enc setFragmentBytes:&du length:sizeof du atIndex:1];
			[enc setFragmentTexture:_white atIndex:0];
			[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:a.hull_indices.size() indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
		}
		for (BuildAsset::Addon& ad : b.asset->addons) {
			const uint32_t t = uint32_t(_timeMs);
			ron::pose_model(*ad.model, ad.anim.get(), t, posed);
			std::vector<ron::ColorVertex>& verts = ad.verts;
			verts.clear();
			verts.reserve(posed.size());
			for (const ron::PosedVertex& p : posed) verts.push_back({-3 * p.py, -3 * p.px, 3 * p.pz, -p.ny, -p.nx, p.nz, p.u, p.v, 255, 255, 255, 255});
			const auto& ix = ad.model->meshes[0].indices;
			if (ix.empty()) continue;
			rm::DrawUniforms du = rm::draw_uniforms(*ad.state, at);
			du.tfactor = simd_make_float4(1, 1, 1, 1);
			_states.bind(enc, *ad.state, rm::VertexKind::Colored);
			[enc setCullMode:MTLCullModeNone];
			if (!ad.ib) ad.ib = [_device newBufferWithBytes:ix.data() length:ix.size() * 2 options:MTLResourceStorageModeShared];
			const size_t vbytes = verts.size() * sizeof(ron::ColorVertex);
			if (vbytes <= 4096) {  // Metal's setVertexBytes limit: small models go inline, with no buffer allocated per frame
				[enc setVertexBytes:verts.data() length:vbytes atIndex:0];
			} else {
				[enc setVertexBuffer:[_device newBufferWithBytes:verts.data() length:vbytes options:MTLResourceStorageModeShared] offset:0 atIndex:0];
			}
			id<MTLBuffer> ib = ad.ib;
			[enc setVertexBytes:&du length:sizeof du atIndex:1];
			[enc setFragmentBytes:&du length:sizeof du atIndex:1];
			[enc setFragmentTexture:ad.texture atIndex:0];
			[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:ix.size() indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
		}
	}
}

// The construction underlay (WallOut::draw_underlays, get_construct_gpiece): CONSTRUCTION[constructsize - 1], or by the
// building type's size; docks and Senate-type buildings have their own entries (matched by graphic name here: the type
// flags are not in the simulation's rules yet). Drawn like a building sprite.
- (void)encodeConstructionOf:(const PlacedBuilding&)b graphic:(const ron::BuildGraphic&)g type:(int32_t)type at:(simd_float3)L
                      alpha:(int)alpha encoder:(id<MTLRenderCommandEncoder>)enc {
	if (alpha <= 0 || _constructSprites.empty()) return;
	const ron::sim::Type& T = _game->type(type);
	int size = g.constructsize > 0 ? g.constructsize : std::max(T.x_size, T.y_size);
	size_t idx = size_t(std::clamp(size, 1, 8) - 1);
	const std::string low = ron::to_lower(g.name);
	if (low.find("dock") != std::string::npos) idx = 9;
	else if (low.find("senate") != std::string::npos) idx = 10;
	if (idx >= _constructSprites.size()) return;
	const ron::ConstructionSprite& c = _constructSprites[idx];
	if (!_constructTex.count(idx)) {
		id<MTLTexture> t = nil;
		const std::string path = c.texture.empty() ? std::string() : _files->resolve(c.texture);
		if (!path.empty()) {
			const ron::Image img = ron::parse_tga(ron::read_file(path));
			t = [self makeTexture:img];
			_constructW[idx] = img.width, _constructH[idx] = img.height;
		}
		_constructTex[idx] = t;
	}
	id<MTLTexture> tex = _constructTex[idx];
	if (!tex) return;
	const float w = float(_constructW[idx]), hh = float(_constructH[idx]);
	const uint8_t* tc = kBuildTeamColors[b.team];
	auto corner = [&](float i, float j) {
		const simd_float3 q = L + kRight * (3.0f * (i - float(c.spritexoff))) - kUp * (3.0f * (j - float(c.spriteyoff)));
		return ron::ColorVertex{q.x, q.y, q.z, 0, 0, 1, i / w, j / hh, tc[0], tc[1], tc[2], uint8_t(alpha)};
	};
	const ron::ColorVertex quad[6] = {corner(0, 0), corner(w, 0), corner(0, hh), corner(0, hh), corner(w, 0), corner(w, hh)};
	static const ron::RenderState& build_rs = *ron::find_render_state("Building Blend");
	rm::DrawUniforms du = rm::draw_uniforms(build_rs, matrix_identity_float4x4);
	_states.bind(enc, build_rs, rm::VertexKind::Colored);
	[enc setCullMode:MTLCullModeNone];
	[enc setVertexBytes:quad length:sizeof quad atIndex:0];
	[enc setVertexBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentTexture:tex atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

// Scaffolding (draw_scaffold @907420): SCAFFOLD_TIMING list size - 1 (the last list for Senate-type buildings); each
// piece of the owner's age set (0-2, 3-4, 5+) is drawn while start <= p <= end at coord - 96h + offset, rotated by its
// angle, scale (3, -3, 3). The offset uses the add-ons' building-space mapping (x, y, z) -> (-3y, -3x, 3z).
- (void)encodeScaffoldOf:(const PlacedBuilding&)b type:(int32_t)type at:(simd_float3)L half:(float)h progress:(float)p
                 encoder:(id<MTLRenderCommandEncoder>)enc {
	if (_scaffolds.timings.empty()) return;
	const ron::sim::Type& T = _game->type(type);
	size_t list = size_t(std::clamp(std::max(T.x_size, T.y_size), 1, int(_scaffolds.timings.size()) - 1) - 1);
	if (ron::to_lower(b.name).find("senate") != std::string::npos) list = _scaffolds.timings.size() - 1;
	const int age = _game->leaders[size_t(b.who)].age;
	const int age_set = age <= 2 ? 0 : age <= 4 ? 1 : 2;
	std::vector<ron::PosedVertex> posed;
	for (const ron::ScaffoldTiming& t : _scaffolds.timings[list]) {
		if (p < t.start || p > t.end) continue;
		const ron::ScaffoldPiece* piece = _scaffolds.find(age_set, t.name);
		if (!piece) continue;
		auto it = _scaffoldModels.find(piece);
		if (it == _scaffoldModels.end()) {
			std::unique_ptr<ScaffoldModel> m;
			try {
				if (!piece->model.empty() && !_files->resolve(piece->model).empty()) {
					m = std::make_unique<ScaffoldModel>();
					m->model = std::make_unique<ron::BH3Model>(ron::parse_bh3(ron::read_file(_files->resolve(piece->model))));
					if (!piece->anim_file.empty() && !_files->resolve(piece->anim_file).empty())
						m->anim = std::make_unique<ron::Animation>(ron::parse_bha(ron::read_file(_files->resolve(piece->anim_file))));
					const std::string tp = piece->texture.empty() ? std::string() : _files->resolve(piece->texture);
					m->texture = tp.empty() ? _white : [self makeTexture:ron::parse_tga(ron::read_file(tp))];
					m->state = ron::find_render_state(piece->renderstate);
					if (!m->state) m->state = ron::find_render_state("Ammo Blend");
				}
			} catch (const std::exception& e) {
				std::fprintf(stderr, "scaffold %s: %s\n", piece->name.c_str(), e.what());
				m.reset();
			}
			it = _scaffoldModels.emplace(piece, std::move(m)).first;
		}
		ScaffoldModel* m = it->second.get();
		if (!m || m->model->meshes.empty() || m->model->meshes[0].indices.empty()) continue;
		ron::pose_model(*m->model, m->anim.get(), uint32_t(_timeMs), posed);
		const float a = t.angle * float(M_PI) / 180.0f, ca = std::cos(a), sa = std::sin(a);
		std::vector<ron::ColorVertex> verts;
		verts.reserve(posed.size());
		for (const ron::PosedVertex& v : posed) {
			// Rotate in model space by the piece's angle, then the building-space mapping of the add-ons.
			const float px = v.px * ca - v.py * sa, py = v.px * sa + v.py * ca;
			const float nx = v.nx * ca - v.ny * sa, ny = v.nx * sa + v.ny * ca;
			verts.push_back({-3 * (py + t.y), -3 * (px + t.x), 3 * (v.pz + t.z), -ny, -nx, v.nz, v.u, v.v, 255, 255, 255, 255});
		}
		const simd_float4x4 at = simd_matrix(simd_make_float4(1, 0, 0, 0), simd_make_float4(0, 1, 0, 0), simd_make_float4(0, 0, 1, 0),
		                                     simd_make_float4(L.x, L.y, L.z, 1));
		const auto& ix = m->model->meshes[0].indices;
		rm::DrawUniforms du = rm::draw_uniforms(*m->state, at);
		du.tfactor = simd_make_float4(1, 1, 1, 1);
		_states.bind(enc, *m->state, rm::VertexKind::Colored);
		[enc setCullMode:MTLCullModeNone];
		id<MTLBuffer> vb = [_device newBufferWithBytes:verts.data() length:verts.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
		id<MTLBuffer> ib = [_device newBufferWithBytes:ix.data() length:ix.size() * 2 options:MTLResourceStorageModeShared];
		[enc setVertexBuffer:vb offset:0 atIndex:0];
		[enc setVertexBytes:&du length:sizeof du atIndex:1];
		[enc setFragmentBytes:&du length:sizeof du atIndex:1];
		[enc setFragmentTexture:m->texture atIndex:0];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:ix.size() indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
	}
	(void)h;
}

- (void)lookAtX:(float)x y:(float)y {
	_lookAt = simd_make_float3(x, y, 0);
}

// World point -> drawable pixel (top-left origin), with the same camera as the renderer.
- (simd_float2)screenOf:(simd_float3)p {
	const CGSize size = [self drawSize];
	const float upp = units_per_pixel(_zoom, float(size.width));
	const simd_float3 d = p - _lookAt;
	return simd_make_float2(float(size.width) / 2 + simd_dot(kRight, d) / upp, float(size.height) / 2 - simd_dot(kUp, d) / upp);
}

- (void)selectAll {
	_selected.clear();
	if (!_game) return;
	for (const ron::sim::UnitView& u : _curView.units) {
		if (u.alive && u.on_map && [self graphicForUnit:u] >= 0) _selected.push_back({u.who, u.o});
	}
}

- (void)selectInRect:(CGRect)r add:(BOOL)add {
	if (!add) _selected.clear();
	const bool click = r.size.width < 4 && r.size.height < 4;
	float best = 40;  // pick radius in pixels for a click
	const SimUnitDrawn* best_u = nullptr;
	auto selected = [&](const SimUnitDrawn& d) {
		return std::find(_selected.begin(), _selected.end(), std::make_pair(d.who, d.o)) != _selected.end();
	};
	for (const SimUnitDrawn& d : _drawnUnits) {
		// Aim at the unit's middle rather than its feet.
		const simd_float3 base = simd_make_float3(d.x, d.y, std::max(0.0f, _terrain.sample_height(d.x, d.y)));
		const simd_float2 sp = [self screenOf:base + simd_make_float3(0, 0, 60)];
		if (click) {
			const float dist = simd_distance(sp, simd_make_float2(float(r.origin.x), float(r.origin.y)));
			if (dist < best) best = dist, best_u = &d;
		} else if (CGRectContainsPoint(r, CGPointMake(sp.x, sp.y)) && !selected(d)) {
			_selected.push_back({d.who, d.o});
		}
	}
	if (click && best_u && !selected(*best_u)) _selected.push_back({best_u->who, best_u->o});
	[self updateTitle];
}

// Selection rings under selected units: a ground ring in the "Circle Blend" state (vertex colour, no depth test).
// The original draws one per guy at a radius from the unit type (UnitOut::draw_unit_circles); one ring per unit for now.
- (void)encodeSelection:(id<MTLRenderCommandEncoder>)enc {
	if (_selected.empty() && _selectedBuilds.empty()) return;
	static const ron::RenderState& circle_rs = *ron::find_render_state("Circle Blend");
	std::vector<ron::ColorVertex> verts;
	std::vector<uint32_t> idx;
	const int kSeg = 24;
	for (const SimUnitDrawn& u : _drawnUnits) {
		if (std::find(_selected.begin(), _selected.end(), std::make_pair(u.who, u.o)) == _selected.end()) continue;
		const float r0 = 70, r1 = 85;
		const uint32_t base = uint32_t(verts.size());
		for (int k = 0; k <= kSeg; k++) {
			const float a = float(k) / kSeg * 2 * float(M_PI), c = std::cos(a), s = std::sin(a);
			for (float r : {r0, r1}) {
				const float x = u.x + c * r, y = u.y + s * r;
				verts.push_back({x, y, std::max(0.0f, _terrain.sample_height(x, y)) + 4, 0, 0, 1, 0, 0, 255, 255, 255, 255});
			}
		}
		for (uint32_t k = 0; k < uint32_t(kSeg); k++) {
			const uint32_t a = base + k * 2;
			idx.insert(idx.end(), {a, a + 1, a + 2, a + 2, a + 1, a + 3});
		}
	}
	// Selected buildings: a ring at the footprint's radius.
	for (const auto& [who, o] : _selectedBuilds) {
		const ron::sim::BuildView* v = [self buildView:who o:o];
		if (!v || !_game) continue;
		const float r0 = float(std::max(_game->type(v->type).x_size, _game->type(v->type).y_size)) * 96.0f + 60, r1 = r0 + 18;
		const uint32_t base = uint32_t(verts.size());
		for (int k = 0; k <= kSeg; k++) {
			const float a = float(k) / kSeg * 2 * float(M_PI), c = std::cos(a), sn = std::sin(a);
			for (float r : {r0, r1}) {
				const float x = float(v->x) + c * r, y = float(v->y) + sn * r;
				verts.push_back({x, y, std::max(0.0f, _terrain.sample_height(x, y)) + 4, 0, 0, 1, 0, 0, 255, 255, 255, 255});
			}
		}
		for (uint32_t k = 0; k < uint32_t(kSeg); k++) {
			const uint32_t a = base + k * 2;
			idx.insert(idx.end(), {a, a + 1, a + 2, a + 2, a + 1, a + 3});
		}
	}
	if (verts.empty()) return;
	rm::DrawUniforms du = rm::draw_uniforms(circle_rs, matrix_identity_float4x4);
	// Drawn after tone mapping (ObjectsOut::draw_overlays, lighting-spec step 10), so Circle Blend's plain vertex colour
	// shows at full brightness (lighting-spec open question 2).
	_overlayStates.bind(enc, circle_rs, rm::VertexKind::Colored);
	[enc setCullMode:MTLCullModeNone];
	id<MTLBuffer> vb = [_device newBufferWithBytes:verts.data() length:verts.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
	id<MTLBuffer> ib = [_device newBufferWithBytes:idx.data() length:idx.size() * 4 options:MTLResourceStorageModeShared];
	[enc setVertexBuffer:vb offset:0 atIndex:0];
	[enc setVertexBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentTexture:_white atIndex:0];
	[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:idx.size() indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0];
}

- (void)scrollByPixelsX:(float)dx y:(float)dy {
	// Screen right = kRight, screen up = the ground projection of kUp. Clamp to the map, as the game does.
	const CGSize size = [self drawSize];
	const float upp = units_per_pixel(_zoom, float(size.width));
	const simd_float3 ground_up = simd_normalize(simd_make_float3(kUp.x, kUp.y, 0)) / 0.5f;  // 1 px up = 2 ground units/upp
	_lookAt += (kRight * dx + ground_up * dy) * upp;
	_lookAt.x = std::clamp(_lookAt.x, 0.0f, _mesh.width);
	_lookAt.y = std::clamp(_lookAt.y, 0.0f, _mesh.height);
}

- (void)handleKey:(NSEvent*)event {
	if (_gameMenu) {
		[self gameMenuKey:event];
		return;
	}
	if (_hud.chat_active()) {  // the chat line takes every key until Return or Esc
		[self chatKey:event];
		return;
	}
	if (_gnameSlot >= 0) {  // the group name dialog takes every key until Return or Esc
		[self groupNameKey:event];
		return;
	}
	if (_pbv && [self pbKey:event]) return;
	if (event.keyCode == 109 && !_inPopup) {  // F10: the game menu
		[self openGameMenu:1];
		return;
	}
	if (event.keyCode == 96 && !_inPopup) {  // F5: quick save
		[self quickSave];
		return;
	}
	if (event.keyCode == 101 && !_inPopup) {  // F9: quick load
		[self quickLoad];
		return;
	}
	if ([self handleGroupKey:event]) {
		[self updateTitle];
		return;
	}
	if (_inPopup) {  // BasePop: Return takes the first (accept) button; other keys wait for a click
		if (event.keyCode == 36 || event.keyCode == 76) _popupResult = 0;
		return;
	}
	if (_sviewOn && _sview.goal_dialog_open() && (event.keyCode == 53 || event.keyCode == 36)) {  // Esc / Return close
		_sview.close_goal_dialog();
		return;
	}
	const ron::sim::ScriptPresentation* sp = [self pres];
	const bool scroll_locked = sp && (sp->scroll_lock_flags & 1), zoom_locked = sp && (sp->zoom_lock_flags & 1);
	if (event.keyCode == 53 && (_placeType >= 0 || _targeting >= 0 || _buildPage >= 0)) {  // Escape: cancel the mode
		_placeType = -1, _targeting = -1, _buildPage = -1;
		return;
	}
	if (event.keyCode == 5 && !(event.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand | NSEventModifierFlagOption))) {
		// G: name the selected soldiers as an army group (or rename the selected one); Shift+G: regroup the selected named group.
		if ([self groupKey:(event.modifierFlags & NSEventModifierFlagShift) != 0]) {
			[self updateTitle];
			return;
		}
	}
	{
		// The default key map (KeyMap::lookup): the key's meaning in the current mode and selection context.
		const NSEventModifierFlags mf = event.modifierFlags;
		const std::string ks = keymap::key_string(event.keyCode, (mf & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0,
		                                          (mf & NSEventModifierFlagOption) != 0, (mf & NSEventModifierFlagShift) != 0);
		if (const keymap::Entry* e = keymap::lookup(ks, _buildPage >= 0 ? 1 : 0, [self](int ctx) { return [self selectionContext:ctx] == YES; })) {
			if ([self execKeyOption:e->option param:e->param modifiers:mf] >= 0) {
				[self updateTitle];
				return;
			}
		}
	}
	NSString* chars = event.charactersIgnoringModifiers;
	const unichar c = chars.length ? [chars characterAtIndex:0] : 0;
	const float step = 120;
	switch (c) {
		case NSRightArrowFunctionKey: if (!scroll_locked) [self scrollByPixelsX:step y:0]; break;
		case NSLeftArrowFunctionKey: if (!scroll_locked) [self scrollByPixelsX:-step y:0]; break;
		case NSUpArrowFunctionKey: if (!scroll_locked) [self scrollByPixelsX:0 y:step]; break;
		case NSDownArrowFunctionKey: if (!scroll_locked) [self scrollByPixelsX:0 y:-step]; break;
		case '+': case '=': if (!zoom_locked) _zoom = std::min(_zoom + 1, 6); break;
		case '-': case '_': if (!zoom_locked) _zoom = std::max(_zoom - 1, 0); break;
		case 'b': case 'B': _bloom = !_bloom; break;
		case 27:  // Escape: clear the selection, else the game menu
			if (_selected.empty() && _selectedBuilds.empty() && !(_sviewOn && _sview.modal())) [self openGameMenu:1];
			_selected.clear(), _selectedBuilds.clear();
			break;
		case 'h': case 'H': _hudOn = !_hudOn; break;
		case 'p': case 'P':
			if (!(sp && sp->pause_disabled)) [self togglePause];  // pause_disable
			break;
		case 'f': case 'F': _fogOn = !_fogOn; break;
		default: break;
	}
	[self updateTitle];
}

// The chat line (hud.mm): Return sends the text as a CHAT command to everybody / the allies / the enemies, Esc drops it.
- (void)chatKey:(NSEvent*)e {
	if (e.keyCode == 53) {  // Esc
		_hud.chat_close();
		return;
	}
	if (e.keyCode == 36 || e.keyCode == 76) {  // Return / keypad Enter
		[self chatSend];
		return;
	}
	if (e.keyCode == 51) {  // Delete
		_hud.chat_backspace();
		return;
	}
	if (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) return;
	NSString* chars = e.characters;
	for (NSUInteger i = 0; i < chars.length; i++) {
		const unichar c = [chars characterAtIndex:i];
		if (c >= 0x20 && c != 0x7f && !(c >= 0xF700 && c <= 0xF8FF)) _hud.chat_append(std::string([[NSString stringWithCharacters:&c length:1] UTF8String]));
	}
}

// CommandManager::issue_chat: the recipients are a mask of leaders (-1 everybody); the sender is always among them.
- (void)chatSend {
	const std::string text = _hud.chat_text();
	const int mode = _hud.chat_mode();
	_hud.chat_close();
	if (!_game || text.empty()) return;
	if (_mp) {  // multiplayer: an observer's chat goes out of band; "/ignore NAME" and "/hear NAME" are CHAT_SET
		if (_mp->observer()) {
			_mp->say(text);
			return;
		}
		if ([self mpChatCommand:text]) return;
	}
	const int me = [self localPlayer];
	int32_t bits = -1;
	std::u16string line = ron::utf8_to_utf16(text);
	int lineMode = mode;
	if (_mp) {  // the chat box's prefixes (as the lobby's): "/t text" to the allies, "/w name text" to one player
		auto low = [](std::string v) {
			for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
			return v;
		};
		auto after = [&](size_t n) {
			std::string r = text.substr(n);
			while (!r.empty() && r.front() == ' ') r.erase(r.begin());
			return r;
		};
		if (text.rfind("/t ", 0) == 0 || text.rfind("/team ", 0) == 0) {
			lineMode = 1;
			line = ron::utf8_to_utf16(after(text.find(' ')));
		} else if (text.rfind("/w ", 0) == 0 || text.rfind("/whisper ", 0) == 0) {
			std::string rest = after(text.find(' '));
			const size_t sp = rest.find(' ');
			const std::string name = low(rest.substr(0, sp));
			const std::string body = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
			int target = -1;
			for (int slot = 0; slot < _mp->plan().nations && slot < 8; slot++)
				if (_mp->plan().peer_of[size_t(slot)] >= 0 && slot != _mp->slot() && low(_mp->seat_name(slot)) == name) target = _mp->who_of(slot);
			if (target < 0 || body.empty()) {
				[self say:target < 0 ? "There is no player named \"" + rest.substr(0, sp) + "\"." : std::string("Whisper what? /w NAME text")];
				return;
			}
			lineMode = 3;
			bits = (1 << me) | (1 << target);
			line = ron::utf8_to_utf16(body);
		}
		if (line.empty()) return;
	}
	if (lineMode == 1 || lineMode == 2) {
		bits = 1 << me;
		for (int i = 0; i < 8; i++) {
			if (i == me || !(_game->leaders[size_t(i)].flags & ron::sim::leaderflag::kValid)) continue;
			const bool ally = _game->leaders[size_t(me)].diplos(i) == 2 && _game->leaders[size_t(i)].diplos(me) == 2;
			if ((lineMode == 1) == ally) bits |= 1 << i;
		}
	}
	const std::u16string tx = line;
	[self issueAs:me block:^bool { return _cm.issue_chat(tx, bits); }];
}

// "/ignore NAME" stops the broadcast chat of a player (CHAT_SET, executed by every machine), "/hear NAME" takes it back.
- (BOOL)mpChatCommand:(const std::string&)text {
	const bool ign = text.rfind("/ignore ", 0) == 0, hear = text.rfind("/hear ", 0) == 0;
	if (!ign && !hear) return NO;
	std::string name = text.substr(text.find(' ') + 1);
	auto low = [](std::string v) {
		for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
		return v;
	};
	for (int slot = 0; slot < _mp->plan().nations && slot < 8; slot++) {
		if (_mp->plan().peer_of[size_t(slot)] < 0 || slot == _mp->slot() || low(_mp->seat_name(slot)) != low(name)) continue;
		_mp->set_chat_mute(_mp->who_of(slot), ign);
		[self say:std::string(ign ? "You will not hear " : "You hear again ") + _mp->seat_name(slot) + "'s messages to everybody."];
		return YES;
	}
	[self say:"There is no player named \"" + name + "\"."];
	return YES;
}

// In a multiplayer game every machine queues every chat line (the engine's presentation queue is saved state, so it must not depend on who sits at
// this machine: net::GameHooks::execute); the line is shown here only to the recipients of its mask, and a broadcast line not at all when the local
// player ignores its speaker (CHAT_SET). Returns a copy without the hidden events, or null when nothing new has to be hidden.
- (std::unique_ptr<ron::sim::ScriptPresentation>)mpFilteredPresentation:(const ron::sim::ScriptPresentation*)p {
	if (!_mp || !p || !_game) return nullptr;
	const uint64_t from = _mpFilterSeq;
	_mpFilterSeq = p->next_seq;   // the view consumes everything below this frame's next_seq now
	const int me = _game->local_play;
	auto hidden = [&](const ron::sim::ScriptEvent& e) {
		if (e.kind != ron::sim::ScriptEvent::kChat) return false;
		for (auto it = _game->ctl.chat.rbegin(); it != _game->ctl.chat.rend(); ++it) {
			if (it->from != e.who || it->text != e.text) continue;
			if (it->bits == 0xFFFFFFFFu) return _mp->chat_muted(me, e.who);                          // to everybody: unless the player ignores the speaker
			return !(me >= 0 && me < 32 && ((it->bits >> uint32_t(me)) & 1u));                       // to a team or one player: only its recipients
		}
		return false;
	};
	bool any = false;
	if (getenv("RON_MP_DEBUG"))
		for (const ron::sim::ScriptEvent& e : p->events)
			if (e.seq >= from && e.kind == ron::sim::ScriptEvent::kChat)
				std::fprintf(stderr, "[mp] chat event seq %llu from %d \"%s\" me %d hidden %d (console lines %zu, muted %d)\n", (unsigned long long)e.seq, e.who, e.text.c_str(), me, int(hidden(e)),
				             _game->ctl.chat.size(), int(_mp->chat_muted(me, e.who)));
	for (const ron::sim::ScriptEvent& e : p->events)
		if (e.seq >= from && hidden(e)) {
			any = true;
			break;
		}
	if (!any) return nullptr;
	auto copy = std::make_unique<ron::sim::ScriptPresentation>(*p);
	for (ron::sim::ScriptEvent& e : copy->events)
		if (e.seq >= from && hidden(e)) {  // an empty game message: the view steps over it (its cursor must pass the sequence number) and shows nothing
			e.kind = ron::sim::ScriptEvent::kGameMessage;
			e.text.clear();
		}
	return copy;
}

// --chat / --chat-draft: Enter, the text typed, and (--chat) Return.
- (void)chatScript:(const std::string&)text send:(bool)send {
	if (!_game) return;
	[self execKeyOption:"OPTION_CHAT" param:-1 modifiers:0];
	_hud.chat_append(text);
	if (send) [self chatSend];
}

- (void)updateTitle {
	self.view.window.title = [NSString stringWithFormat:@"%s  —  zoom %d  (%.0f, %.0f)%@", _title.c_str(), _zoom,
	                                                    _lookAt.x, _lookAt.y, _bloom ? @"  [bloom]" : @""];
	if (!_selected.empty()) self.view.window.title = [self.view.window.title stringByAppendingFormat:@"  —  %zu selected", _selected.size()];
	if (_game) self.view.window.title = [self.view.window.title stringByAppendingFormat:@"  —  frame %d%@", _game->frame, _paused ? @" (paused)" : @""];
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}

- (void)renderInto:(id<MTLTexture>)output commandBuffer:(id<MTLCommandBuffer>)cb {
	const NSUInteger w = output.width, h = output.height;
	_drawW = CGFloat(w), _drawH = CGFloat(h);
	_targets.ensure(_device, w, h);
	[self buildFigures];
	[self updateFog];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:_targets.scene_pass(MTLClearColorMake(0.02, 0.05, 0.12, 1))];

	rm::SceneUniforms scene{};
	camera_matrices(_lookAt, units_per_pixel(_zoom, float(w)), float(w), float(h), scene.view, scene.proj);
	scene.light_dir = simd_make_float4(ron::kSunDir[0], ron::kSunDir[1], ron::kSunDir[2], 0);
	scene.ambient_light = simd_make_float4(_lighting.ambient_light[0], _lighting.ambient_light[1], _lighting.ambient_light[2], 1);
	// Terrain pass: the Sun carries the tileset's diffuse light (prep_terrain_lighting).
	scene.light_diffuse = simd_make_float4(_terrainLight[0], _terrainLight[1], _terrainLight[2], 1);
	[enc setVertexBytes:&scene length:sizeof scene atIndex:2];
	[enc setFragmentBytes:&scene length:sizeof scene atIndex:2];

	// The camera basis is mirrored relative to a right-handed view (the original's world is left-handed). Together with
	// the terrain's index order, CULL CCW then removes back faces exactly as in the original: no flip needed.
	const bool flip = false;
	// Ocean floor sheet: covers the faded-out shelf and deep land under the sea. The original draws it right after the
	// land, where faded land (still writing depth) between z = -52 and -101 would show the black clear colour as a dark
	// rim along every coast (docs/water-spec.md 2.5, unconfirmed). Drawing it first lets that land fade over the floor
	// instead; revisit once compared with an in-game screenshot.
	if (_floorVB && !debug_skip("floor")) {
		static const ron::RenderState floor_rs = floor_state();
		rm::DrawUniforms fu = rm::draw_uniforms(floor_rs, matrix_identity_float4x4);
		_states.bind(enc, floor_rs, rm::VertexKind::Colored2, false, flip);
		[enc setVertexBuffer:_floorVB offset:0 atIndex:0];
		[enc setVertexBytes:&fu length:sizeof fu atIndex:1];
		[enc setFragmentBytes:&fu length:sizeof fu atIndex:1];
		[enc setFragmentTexture:_floorTex atIndex:0];
		[enc setFragmentTexture:_floorTex atIndex:1];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:_mesh.floor_indices.size() indexType:MTLIndexTypeUInt32 indexBuffer:_floorIB indexBufferOffset:0];
	}
	rm::DrawUniforms du = rm::draw_uniforms(_landState, matrix_identity_float4x4);
	_states.bind(enc, _landState, rm::VertexKind::Colored, false, flip);
	[enc setVertexBuffer:_landVB offset:0 atIndex:0];
	[enc setVertexBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentBytes:&du length:sizeof du atIndex:1];
	for (size_t i = 0; i < _bucketIB.size() && !debug_skip("land"); i++) {
		[enc setFragmentTexture:_bucketTex[i] atIndex:0];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:_mesh.buckets[i].indices.size() indexType:MTLIndexTypeUInt32
		               indexBuffer:_bucketIB[i] indexBufferOffset:0];
	}
	// Transitions (after land, before objects): ground texture on stage 0, mask on stage 1 with UV1.
	static const ron::RenderState trans_rs = transition_state(), sandy_rs = coral_state(false), coral_rs = coral_state(true);
	for (const TransitionDraw& t : _transitions) {
		// Ocean overlays (CoralSandy + CoralTrans, no depth test) come out as hard turquoise bands over deep water rather
		// than the faint fringe the spec expects (docs/water-spec.md 2.6), so they are off unless RON_CORAL=1.
		if (t.ocean && !getenv("RON_CORAL")) continue;
		if (debug_skip(t.ocean ? "coral" : "trans")) continue;
		const int passes = t.ocean && _coral ? 2 : 1;
		for (int pass = 0; pass < passes; pass++) {
			const ron::RenderState& rs = !t.ocean ? trans_rs : pass == 0 ? sandy_rs : coral_rs;
			rm::DrawUniforms tu = rm::draw_uniforms(rs, matrix_identity_float4x4);
			_states.bind(enc, rs, rm::VertexKind::Colored2, false, flip);
			[enc setVertexBuffer:t.vb offset:0 atIndex:0];
			[enc setVertexBytes:&tu length:sizeof tu atIndex:1];
			[enc setFragmentBytes:&tu length:sizeof tu atIndex:1];
			[enc setFragmentTexture:(pass == 0 ? t.texture : _coral) atIndex:0];
			[enc setFragmentTexture:t.mask atIndex:1];
			[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:t.count indexType:MTLIndexTypeUInt32 indexBuffer:t.ib indexBufferOffset:0];
		}
	}
	// River troughs, mountains (two draws per range), snowcaps, then doobers (terrain-dressing-spec section 6).
	static const ron::RenderState mtn_rs = mountain_state(), snow_rs = snowcap_state(), trough_rs = trough_state();
	auto draw_mesh = [&](const ron::RenderState& rs, rm::VertexKind kind, const MeshDraw& m, id<MTLTexture> t0, id<MTLTexture> t1) {
		rm::DrawUniforms du = rm::draw_uniforms(rs, matrix_identity_float4x4);
		_states.bind(enc, rs, kind, false, flip);
		[enc setVertexBuffer:m.vb offset:0 atIndex:0];
		[enc setVertexBytes:&du length:sizeof du atIndex:1];
		[enc setFragmentBytes:&du length:sizeof du atIndex:1];
		[enc setFragmentTexture:t0 atIndex:0];
		[enc setFragmentTexture:(t1 ?: t0) atIndex:1];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:m.count indexType:MTLIndexTypeUInt32 indexBuffer:m.ib indexBufferOffset:0];
	};
	// Farm ground (render_weathering(0, 3): the pigfarm and wheatfarm underlays), before roads and river troughs.
	for (int k = 1; k >= 0; k--) {
		if (_farmUnderlays[k].vb) draw_mesh(snow_rs, rm::VertexKind::Colored2, _farmUnderlays[k], _farmUnderlays[k].tex0, _farmUnderlays[k].tex1);
	}
	for (const MeshDraw& m : _troughDraws) draw_mesh(trough_rs, rm::VertexKind::Colored, m, m.tex0, nil);
	for (const MeshDraw& m : _mountainDraws) {
		draw_mesh(mtn_rs, rm::VertexKind::Colored2, m, m.tex0, m.tex1);
		draw_mesh(mtn_rs, rm::VertexKind::Colored2, m, m.tex0b, m.tex1b);
	}
	if (_snowcapDraw.vb) draw_mesh(snow_rs, rm::VertexKind::Colored2, _snowcapDraw, _snowcapDraw.tex0, _snowcapDraw.tex1);
	// Object pass onward (objects, water): the Sun at full white (prep_unit_lighting).
	scene.light_diffuse = simd_make_float4(1, 1, 1, 1);
	[enc setVertexBytes:&scene length:sizeof scene atIndex:2];
	[enc setFragmentBytes:&scene length:sizeof scene atIndex:2];
	static const ron::RenderState& doober_rs = *ron::find_render_state("Doober Blend");
	for (const MeshDraw& m : _dooberDraws) draw_mesh(doober_rs, rm::VertexKind::Colored, m, m.tex0, nil);
	scene.light_diffuse = simd_make_float4(1, 1, 1, 1);
	[enc setVertexBytes:&scene length:sizeof scene atIndex:2];
	[enc setFragmentBytes:&scene length:sizeof scene atIndex:2];
	[self encodeFootprint:enc];
	[self encodeBuildings:enc];
	[self encodeGoods:enc];
	[self encodeUnits:enc];
	if (_treeVB) {
		rm::DrawUniforms tu = rm::draw_uniforms(_treeState, matrix_identity_float4x4);
		tu.diffuse = simd_make_float4(0.5f, 0.5f, 0.5f, 1);
		_states.bind(enc, _treeState, rm::VertexKind::Colored, false, flip);
		[enc setVertexBuffer:_treeVB offset:0 atIndex:0];
		[enc setVertexBytes:&tu length:sizeof tu atIndex:1];
		[enc setFragmentBytes:&tu length:sizeof tu atIndex:1];
		for (int i = 0; i < ron::kTreeSlots; i++) {
			if (!_treeIB[i]) continue;
			[enc setFragmentTexture:_treeTex[i] atIndex:0];
			[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:_treeCount[i] indexType:MTLIndexTypeUInt32
			               indexBuffer:_treeIB[i] indexBufferOffset:0];
		}
	}
	[self encodeWheat:enc];
	if (_waterIB && !debug_skip("water")) {
		rm::DrawUniforms wu = rm::draw_uniforms(_waterState, matrix_identity_float4x4);
		wu.mat_ambient = simd_make_float4(0.2f, 0.2f, 0.2f, 1);  // the water material (TerrainOut::one_time_init)
		wu.mat_diffuse = simd_make_float4(1, 1, 1.5f, 1);
		_states.bind(enc, _waterState, rm::VertexKind::Colored, false, flip);
		[enc setVertexBuffer:_waterVB offset:0 atIndex:0];
		[enc setVertexBytes:&wu length:sizeof wu atIndex:1];
		[enc setFragmentBytes:&wu length:sizeof wu atIndex:1];
		[enc setFragmentTexture:_white atIndex:0];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:_mesh.water_indices.size() indexType:MTLIndexTypeUInt32
		               indexBuffer:_waterIB indexBufferOffset:0];
	}
	if (_riverWaterDraw.vb) {  // the faint river sheen, after the sea water
		static const ron::RenderState river_rs = river_water_state();
		draw_mesh(river_rs, rm::VertexKind::Colored2, _riverWaterDraw, _riverWaterDraw.tex0, _riverWaterDraw.tex1);
	}
	// Fog of war last, so it also covers water (the original draws it before objects and water, terrain-spec B6).
	[self encodeFog:enc];
	[enc endEncoding];

	_post.encode(cb, _targets.hdr(), output, rm::tonemap_params(ron::ToneMapSettings{}), _bloom);

	// Post-tonemap overlays (Scene::render step 10): selection circles and health bars.
	id<MTLRenderCommandEncoder> oe = [cb renderCommandEncoderWithDescriptor:rm::overlay_pass(output)];
	[oe setVertexBytes:&scene length:sizeof scene atIndex:2];
	[oe setFragmentBytes:&scene length:sizeof scene atIndex:2];
	[self encodeSelection:oe];
	[self encodeAnnounce:oe];
	[self encodeHealthBars:oe];
	// The interface (Window::update_screen + IFaceRenderManager::flush_all + FontFamily::flush_fonts), after the overlays.
	[self encodeHud:oe width:w height:h];
	[oe endEncoding];
}

- (void)drawInMTKView:(MTKView*)view {
	const double now = CACurrentMediaTime(), before = _timeMs;
	if (_lastFrame > 0) _timeMs += std::min(0.1, now - _lastFrame) * 1000.0;
	_lastFrame = now;
	[self advanceFarmsTo:_farmFrame + uint32_t(_timeMs * 0.015) - uint32_t(before * 0.015)];  // 15 simulation frames a second
	const bool modal = _inPopup || (_sviewOn && _sview.goal_dialog_open()) || _gameMenu != 0 || _gnameSlot >= 0;
	// The game is over for the local player: after the notice has played, offer the way back (ENDGAMEWIN's role).
	if (_sviewOn && _sview.game_is_over() && !_overOffered) {
		if (_overMs < 0) _overMs = _timeMs;
		if (_timeMs - _overMs > 5000 && !_inPopup && !_gameMenu) {
			_overOffered = YES;
			[self openGameMenu:2];
		}
	}
	if (_pbv && _game) {
		[self pbTick:_timeMs - before modal:modal];
	} else if (_game && ((!_paused && !modal) || _mp)) {
		_simMs += _timeMs - before;
		const double frame_ms = kTimings[std::clamp(_speed, 0, 4)];
		int steps = int(_simMs / frame_ms);
		const int max_steps = _speed >= 4 ? 40 : 4;  // Hyper Fast runs as many frames as fit in a display frame
		if (steps > max_steps) {  // fell behind (a stall): drop the backlog rather than fast-forwarding
			_simMs = std::fmod(_simMs, frame_ms);
			steps = max_steps;
		} else {
			_simMs -= steps * frame_ms;
		}
		[self stepSimulation:steps];
		if (steps && (_game->frame % 15) == 0) [self updateTitle];
	}
	_uiScale = view.window ? float(view.window.backingScaleFactor) : 1.0f;
	if (!_inPopup) [self cameraStep:_timeMs - before];
	if (_audio) _audio->update(_timeMs);
	[self edgeScroll:_timeMs - before];
	id<CAMetalDrawable> drawable = view.currentDrawable;
	if (!drawable) return;
	id<MTLCommandBuffer> cb = [_queue commandBuffer];
	[self renderInto:drawable.texture commandBuffer:cb];
	[cb presentDrawable:drawable];
	[cb commit];
}

// Unattended farm growth, one Farms::inc_time per simulation frame (15 per second).
- (void)advanceFarmsTo:(uint32_t)frame {
	if (_game) return;  // the simulation grows its farms (Farms::inc_time); syncSimFarms copies the state
	for (; _farmFrame < frame; _farmFrame++) ron::farms_inc_time(_farms, _farmSeed);
}

- (BOOL)snapshotTo:(NSString*)path width:(int)w height:(int)h scale:(float)scale {
	id<MTLTexture> out = rm::make_readback_target(_device, w, h);
	_uiScale = scale > 0 ? scale : 1;  // UI pixels drawn at `scale` snapshot pixels (1: the original's screen pixels)
	id<MTLCommandBuffer> cb = [_queue commandBuffer];
	[self renderInto:out commandBuffer:cb];
	[cb commit];
	[cb waitUntilCompleted];
	if (!rm::write_png(out, path.UTF8String)) {
		std::fprintf(stderr, "could not write %s\n", path.UTF8String);
		return NO;
	}
	std::printf("wrote %s (%dx%d, zoom %d)\n", path.UTF8String, w, h, _zoom);
	return YES;
}

// phys_footprint (resident + compressed): the plain resident size drops when macOS compresses idle pages of a loaded machine and hides a leak.
static double footprintMb() {
	task_vm_info_data_t info;
	mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
	return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n) == KERN_SUCCESS ? double(info.phys_footprint) / 1048576.0 : 0.0;
}

- (void)perfRun:(int)frames width:(int)w height:(int)h scale:(float)scale {
	const double footBefore = footprintMb();
	using Clock = std::chrono::steady_clock;
	auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
	[self applyVictoryOverride];
	id<MTLTexture> out = rm::make_readback_target(_device, w, h);
	_uiScale = scale > 0 ? scale : 1;
	g_doFrameCpuNs = 0;
	std::vector<double> sim, enc, gpu, simCpu, encCpu;  // *Cpu: this thread's CPU time (steady on a loaded machine, unlike the wall clock)
	auto cpuMs = [] { return double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)) / 1e6; };
	for (int f = 0; f < frames; f++) {
		const auto t0 = Clock::now();
		const double c0 = cpuMs();
		if (_game && !_mp) [self stepSimulation:1];
		const double c1 = cpuMs();
		const auto t1 = Clock::now();
		@autoreleasepool {
			id<MTLCommandBuffer> cb = [_queue commandBuffer];
			[self renderInto:out commandBuffer:cb];
			const auto t2 = Clock::now();
			const double c2 = cpuMs();
			simCpu.push_back(c1 - c0), encCpu.push_back(c2 - c1);
			[cb commit];
			[cb waitUntilCompleted];
			const auto t3 = Clock::now();
			sim.push_back(ms(t0, t1)), enc.push_back(ms(t1, t2)), gpu.push_back(ms(t2, t3));
		}
	}
	auto stat = [&](const char* name, std::vector<double> v) {
		if (v.empty()) return;
		double sum = 0;
		for (double x : v) sum += x;
		std::sort(v.begin(), v.end());
		std::printf("  %-22s avg %7.3f ms  p50 %7.3f  p95 %7.3f  max %7.3f\n", name, sum / double(v.size()), v[v.size() / 2], v[size_t(double(v.size() - 1) * 0.95)], v.back());
	};
	std::printf("perf: %d frames at %dx%d (frame %d of the game); footprint %.1f MB before, %.1f MB after\n", frames, w, h, _game ? _game->frame : 0, footBefore, footprintMb());
	stat("sim step", sim);
	stat("sim step (thread CPU)", simCpu);
	std::printf("  do_frame alone (thread CPU) avg %7.3f ms\n", double(g_doFrameCpuNs) / 1e6 / double(std::max(frames, 1)));
	stat("render (encode CPU time)", encCpu);
	stat("render (CPU encode)", enc);
	stat("render (GPU wait)", gpu);
	std::vector<double> render(enc.size());
	for (size_t i = 0; i < enc.size(); i++) render[i] = enc[i] + gpu[i];
	stat("render total", render);
	double sa = 0, ra = 0;
	for (double x : sim) sa += x;
	for (double x : render) ra += x;
	const double n = std::max<double>(1, double(frames));
	// The window runs one simulation step per 66 ms at normal speed and draws at the display rate: a 30 fps frame carries about half a step.
	std::printf("  estimated display rate: %.1f fps (render %.2f ms + half a sim step %.2f ms)\n", 1000.0 / (ra / n + sa / n / 2), ra / n, sa / n / 2);
}

- (void)applyVictoryOverride {
	if (const char* v = getenv("RON_LONGRUN_VICTORY"); v && _game) _game->info.victory = uint8_t(std::atoi(v));
}

- (void)longRun:(int)every snapshotPrefix:(NSString*)prefix total:(int)total width:(int)w height:(int)h scale:(float)scale {
	using Clock = std::chrono::steady_clock;
	if (!_game || every <= 0) return;
	auto rss = [] { return footprintMb(); };
	std::vector<double> sim, simCpu;
	int maxAge = 0;
	while (_game->frame < total && !_game->game_over && maxAge < 7) {
		const auto t0 = Clock::now();
		const double c0 = double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)) / 1e6;
		[self stepSimulation:1];
		simCpu.push_back(double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)) / 1e6 - c0);
		sim.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
		if (_game->frame % every != 0) continue;
		double sum = 0;
		for (double x : sim) sum += x;
		std::vector<double> v = sim;
		std::sort(v.begin(), v.end());
		double csum = 0;
		for (double x : simCpu) csum += x;
		std::sort(simCpu.begin(), simCpu.end());
		std::printf("== frame %d: sim avg %.3f p95 %.3f max %.3f ms over %zu steps (thread CPU time: avg %.3f p95 %.3f max %.3f); footprint %.1f MB; game_over %d\n", _game->frame, sum / double(v.size()),
		            v[size_t(double(v.size() - 1) * 0.95)], v.back(), v.size(), csum / double(v.size()), simCpu[size_t(double(v.size() - 1) * 0.95)], simCpu.back(), rss(), int(_game->game_over));
		std::printf("   do_frame alone (thread CPU): avg %.3f ms\n", double(g_doFrameCpuNs) / 1e6 / double(v.size()));
		g_doFrameCpuNs = 0;
		sim.clear(), simCpu.clear();
		[self printCensus];
		std::fflush(stdout);
		if (prefix.length) {
			const int k = _game->frame / 1000;
			if (k == 10 || k == 30 || k == 60) [self snapshotTo:[NSString stringWithFormat:@"%@%dk.png", prefix, k] width:w height:h scale:scale];
		}
		[self perfRun:100 width:w height:h scale:scale];
		for (int who = 0; who < 8; who++) maxAge = std::max(maxAge, int(_game->leaders[size_t(who)].age));
	}
	std::printf("== end: frame %d, game_over %d, max age %d, footprint %.1f MB\n", _game->frame, int(_game->game_over), maxAge, rss());
	[self printCensus];
	if (prefix.length) [self snapshotTo:[NSString stringWithFormat:@"%@end.png", prefix] width:w height:h scale:scale];
	std::fflush(stdout);
}

// ---------------------------------------------------------------- interface (docs/ui-port.md)

// The drawable size of the frame being rendered (the window's drawable, or the snapshot target).
- (CGSize)drawSize {
	if (_drawW > 0 && _drawH > 0) return CGSizeMake(_drawW, _drawH);
	return self.view.drawableSize;
}

// WorldMapBackground::render @008cfc40: the map drawn once, top-down (yaw 0, ortho over [0,W] x [0,H]), into a 256 x 256
// texture: terrain and water, then the feature overlays: trees as flat tree_minimap_color (0x425542) squares, rivers flat
// 0x576870 and the mountains' 0x857F69 at alpha 0x77 (additive). The ocean gradient quad is not drawn yet.
- (void)renderMinimapBackground {
	if (!_hudOn || _bucketIB.empty()) return;
	const NSUInteger S = 256;
	MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:S height:S mipmapped:NO];
	cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	cd.storageMode = MTLStorageModePrivate;
	_miniBG = [_device newTextureWithDescriptor:cd];
	MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:S height:S mipmapped:NO];
	dd.usage = MTLTextureUsageRenderTarget;
	dd.storageMode = MTLStorageModePrivate;
	id<MTLTexture> depth = [_device newTextureWithDescriptor:dd];
	rm::StateCache cache;
	std::string err;
	if (!cache.init(_device, MTLPixelFormatBGRA8Unorm, MTLPixelFormatDepth32Float, err)) return;
	MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = _miniBG;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	rp.depthAttachment.texture = depth;
	rp.depthAttachment.loadAction = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth = 1;
	id<MTLCommandBuffer> cb = [_queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	rm::SceneUniforms scene{};
	scene.view = matrix_identity_float4x4;
	const float W = _mesh.width, H = _mesh.height;
	scene.proj = simd_matrix(simd_make_float4(2 / W, 0, 0, 0), simd_make_float4(0, -2 / H, 0, 0), simd_make_float4(0, 0, -1.0f / 20000, 0),
	                         simd_make_float4(-1, 1, 0.5f, 1));
	scene.light_dir = simd_make_float4(ron::kSunDir[0], ron::kSunDir[1], ron::kSunDir[2], 0);
	scene.ambient_light = simd_make_float4(_lighting.ambient_light[0], _lighting.ambient_light[1], _lighting.ambient_light[2], 1);
	scene.light_diffuse = simd_make_float4(_terrainLight[0], _terrainLight[1], _terrainLight[2], 1);
	[enc setVertexBytes:&scene length:sizeof scene atIndex:2];
	[enc setFragmentBytes:&scene length:sizeof scene atIndex:2];
	rm::DrawUniforms du = rm::draw_uniforms(_landState, matrix_identity_float4x4);
	cache.bind(enc, _landState, rm::VertexKind::Colored, true);
	[enc setCullMode:MTLCullModeNone];
	[enc setVertexBuffer:_landVB offset:0 atIndex:0];
	[enc setVertexBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentBytes:&du length:sizeof du atIndex:1];
	for (size_t i = 0; i < _bucketIB.size(); i++) {
		[enc setFragmentTexture:_bucketTex[i] atIndex:0];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:_mesh.buckets[i].indices.size() indexType:MTLIndexTypeUInt32
		               indexBuffer:_bucketIB[i] indexBufferOffset:0];
	}
	if (_waterIB) {
		rm::DrawUniforms wu = rm::draw_uniforms(_waterState, matrix_identity_float4x4);
		wu.mat_ambient = simd_make_float4(0.2f, 0.2f, 0.2f, 1);
		wu.mat_diffuse = simd_make_float4(1, 1, 1.5f, 1);
		cache.bind(enc, _waterState, rm::VertexKind::Colored, true);
		[enc setCullMode:MTLCullModeNone];
		[enc setVertexBuffer:_waterVB offset:0 atIndex:0];
		[enc setVertexBytes:&wu length:sizeof wu atIndex:1];
		[enc setFragmentBytes:&wu length:sizeof wu atIndex:1];
		[enc setFragmentTexture:_white atIndex:0];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:_mesh.water_indices.size() indexType:MTLIndexTypeUInt32
		               indexBuffer:_waterIB indexBufferOffset:0];
	}
	if (!_treeXY.empty()) {
		ron::RenderState rs = *ron::find_render_state("Circle Blend");
		rs.enables = 0;  // opaque, no depth test
		std::vector<ron::ColorVertex> v;
		const float r = std::max(W, H) / 256.0f * 0.75f;
		for (const simd_float2& t : _treeXY) {
			auto p = [&](float dx, float dy) { return ron::ColorVertex{t.x + dx, t.y + dy, 5000, 0, 0, 1, 0, 0, 0x42, 0x55, 0x42, 255}; };
			const ron::ColorVertex q[6] = {p(-r, -r), p(r, -r), p(-r, r), p(-r, r), p(r, -r), p(r, r)};
			v.insert(v.end(), q, q + 6);
		}
		rm::DrawUniforms tu = rm::draw_uniforms(rs, matrix_identity_float4x4);
		cache.bind(enc, rs, rm::VertexKind::Colored, true);
		[enc setCullMode:MTLCullModeNone];
		id<MTLBuffer> vb = [_device newBufferWithBytes:v.data() length:v.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
		[enc setVertexBuffer:vb offset:0 atIndex:0];
		[enc setVertexBytes:&tu length:sizeof tu atIndex:1];
		[enc setFragmentBytes:&tu length:sizeof tu atIndex:1];
		[enc setFragmentTexture:_white atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:v.size()];
	}
	auto draw_tris = [&](const std::vector<ron::ColorVertex>& tris, const ron::RenderState& rs) {
		if (tris.empty()) return;
		rm::DrawUniforms fu = rm::draw_uniforms(rs, matrix_identity_float4x4);
		cache.bind(enc, rs, rm::VertexKind::Colored, true);
		[enc setCullMode:MTLCullModeNone];
		id<MTLBuffer> vb = [_device newBufferWithBytes:tris.data() length:tris.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
		[enc setVertexBuffer:vb offset:0 atIndex:0];
		[enc setVertexBytes:&fu length:sizeof fu atIndex:1];
		[enc setFragmentBytes:&fu length:sizeof fu atIndex:1];
		[enc setFragmentTexture:_white atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:tris.size()];
	};
	{
		ron::RenderState rs = *ron::find_render_state("Circle Blend");
		rs.enables = 0;  // opaque, no depth test: the rivers are flat 0x576870
		draw_tris(_miniRiverTris, rs);
		rs.enables = 0x4;  // the mountains: SRCALPHA / ONE at alpha 0x77
		rs.src_blend = ron::BlendMode::SrcAlpha;
		rs.dst_blend = ron::BlendMode::One;
		draw_tris(_miniMountainTris, rs);
	}
	[enc endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	_ui.set_texture("MINIMAP_BG", _miniBG, int(S), int(S));
}

// WorldMapFog::fill_colors_fog @8d1420: luminance 0xff (visible), 0x80 (seen), 0x50 (explored), 0 (unexplored).
- (void)updateMinimapFog {
	if (!_fogUploaded || _fogLevels.empty() || _fog.fxs <= 0) return;
	if (_miniFog && _miniFogLevels == _fogLevels) return;
	_miniFogLevels = _fogLevels;
	if (!_miniFog) {
		MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		                                                                             width:NSUInteger(_fog.fxs)
		                                                                            height:NSUInteger(_fog.fys)
		                                                                         mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		_miniFog = [_device newTextureWithDescriptor:d];
		_ui.set_texture("MINIMAP_FOG", _miniFog, _fog.fxs, _fog.fys);
	}
	static const uint8_t lum[4] = {0, 0x50, 0x80, 0xff};
	std::vector<uint8_t> rgba(_miniFogLevels.size() * 4);
	for (size_t i = 0; i < _miniFogLevels.size(); i++) {
		const uint8_t l = lum[std::min<uint8_t>(_miniFogLevels[i], 3)];
		rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = l;
		rgba[i * 4 + 3] = 255;
	}
	[_miniFog replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(_fog.fxs), NSUInteger(_fog.fys)) mipmapLevel:0 withBytes:rgba.data()
	            bytesPerRow:NSUInteger(_fog.fxs) * 4];
}

// The building_graphics.xml graphic of a building type for leader `who` (nation build style, age), as the scenario
// resolver picks it; "" for farms and graphics without a sprite.
- (std::string)buildingGraphicForType:(int32_t)type who:(int32_t)who {
	if (!_game || !_simRules || who < 0 || who >= int32_t(_game->leaders.size())) return std::string();
	const std::string graph = ron::sim::to_utf8(_game->type(type).graph_name);
	if (graph.empty() || graph == "FARM") return std::string();
	const ron::sim::Leader& L = _game->leaders[size_t(who)];
	int bc = 0, backup = 0;
	if (L.tribe >= 0 && size_t(L.tribe) < _simRules->tribes.size()) {
		bc = _simRules->tribes[size_t(L.tribe)].build_continent;
		backup = _simRules->tribes[size_t(L.tribe)].backup_build_continent;
	}
	const ron::BuildGraphic* g = _builds.find_variant(graph, ron::build_style_name(bc), L.age, ron::build_style_name(backup));
	return g && !g->texture.empty() ? g->name : std::string();
}

// Buildings follow the simulation: sites placed in play appear, destroyed ones disappear. Scenario buildings keep the
// graphic the scenario resolver gave them while their (who, o) is alive.
- (void)syncSimBuildings {
	if (!_game) return;
	std::set<std::pair<int32_t, int32_t>> alive;
	for (const ron::sim::BuildView& b : _curView.builds) {
		if (b.alive) alive.insert({b.who, b.o});
	}
	_placed.erase(std::remove_if(_placed.begin(), _placed.end(),
	                             [&](const PlacedBuilding& p) { return p.who >= 0 && !alive.count({p.who, p.o}); }),
	              _placed.end());
	std::set<std::pair<int32_t, int32_t>> have;
	for (const PlacedBuilding& p : _placed) {
		if (p.who >= 0) have.insert({p.who, p.o});
	}
	for (const ron::sim::BuildView& b : _curView.builds) {
		if (!b.alive || have.count({b.who, b.o})) continue;
		const std::string g = [self buildingGraphicForType:b.type who:b.who];
		if (g.empty() || ![self placeBuilding:g x:float(b.x) y:float(b.y) team:b.color]) continue;
		for (PlacedBuilding& p : _placed) {
			if (p.who < 0 && p.x == float(b.x) && p.y == float(b.y) && p.name == g) {
				p.who = b.who, p.o = b.o;
				break;
			}
		}
	}
	// Selected buildings that no longer exist leave the selection.
	_selectedBuilds.erase(std::remove_if(_selectedBuilds.begin(), _selectedBuilds.end(), [&](const auto& k) { return !alive.count(k); }),
	                      _selectedBuilds.end());
}

- (const ron::sim::UnitView*)unitView:(int32_t)who o:(int32_t)o {
	for (const ron::sim::UnitView& u : _curView.units) {
		if (u.who == who && u.o == o) return &u;
	}
	return nullptr;
}

- (const ron::sim::BuildView*)buildView:(int32_t)who o:(int32_t)o {
	for (const ron::sim::BuildView& b : _curView.builds) {
		if (b.who == who && b.o == o) return &b;
	}
	return nullptr;
}

- (std::string)typeName:(int32_t)t {
	const ron::sim::Type& T = _game->type(t);
	std::string n = ron::sim::to_utf8(T.display_name);
	if (n.empty()) n = ron::sim::to_utf8(T.name);
	return n;
}

// The interface icon of a unit type for an owner: its graphic's tex_id / tex_col / tex_row.
- (void)unitIcon:(int32_t)type who:(int32_t)who o:(int32_t)o into:(std::string&)tex col:(int&)col row:(int&)row x:(int&)tx y:(int&)ty {
	ron::sim::UnitView u;
	u.who = who, u.o = o, u.type = type;
	const int g = [self graphicForUnit:u];
	if (g < 0) return;
	const ron::UnitGraphic& ug = _units.units()[size_t(g)];
	tex = ug.tex_id, col = ug.tex_col, row = ug.tex_row, tx = ug.tex_x, ty = ug.tex_y;
}

- (int)localPlayer {
	return _game ? _game->local_play : 0;
}

// The command card (Options, docs/ui-spec.md 6.4): unit orders (UnitOut::make_options), the build menu pages
// (BUILD_OPTIONS: building types the leader can build, type_avail == 4, by GRID_X / GRID_Y and page = cat), or a
// building's train / research buttons (BuildData::can_make: 4 train, 2 research; units at (GRID_X, GRID_Y), their
// upgrades one row down, techs and building upgrades at their own slots).
- (std::vector<hud::Option>)hudOptions {
	std::vector<hud::Option> out;
	_cardMessage.clear();
	if (!_game) return out;
	const int me = [self localPlayer];
	auto opt = [&](int option, int gx, int gy, bool enabled) {
		hud::Option o;
		o.option = option, o.grid_x = gx, o.grid_y = gy, o.enabled = enabled, o.latched = _targeting == option;
		out.push_back(o);
		return &out.back();
	};
	if (!_selected.empty()) {
		bool own = true, citizens = false;
		for (const auto& [who, o] : _selected) {
			own = own && who == me;
			if (const ron::sim::UnitView* u = [self unitView:who o:o]) {
				citizens = citizens || u->type == ron::sim::ptype::kPeasants || u->type == ron::sim::ptype::kPeasantsKorean;
			}
		}
		if (_placeType >= 0 || _targeting >= 0) {
			opt(hud::kOptCancel, 4, 3, true);
			return out;
		}
		if (_buildPage >= 0 && citizens && own) {
			for (int32_t t = 414; t < 543; t++) {
				const ron::sim::Type& T = _game->type(t);
				if (!T.loaded || T.cat != _buildPage) continue;
				if (ron::sim::type_avail(*_game, me, t, 1) != 4) continue;
				hud::Option* o = opt(1000, T.grid_x, T.grid_y, ron::sim::can_pay_cost(*_game, t, me, -1, -1, 1) > 0);
				o->object = t;
				o->tex_id = ron::sim::to_utf8(T.tex.id), o->tex_col = T.tex.col, o->tex_row = T.tex.row;
				o->tex_x = T.tex.x > 0 ? T.tex.x : -1, o->tex_y = T.tex.y > 0 ? T.tex.y : -1;
			}
			opt(66, 2, 3, true);  // BACK
			opt(65, 3, 3, true);  // NEXT
			opt(hud::kOptCancel, 4, 3, true);
			return out;
		}
		{
			uix::Ctx uc = [self uixCtx];
			if (uix::all_air(uc)) {  // aircraft: AIRSTRIKE / PATROL, TRANSFER, RETURN (UnitOut::make_options, the air rows)
				uix::air_unit_options(uc, out);
				return out;
			}
		}
		opt(hud::kOptMoveTo, 0, 0, own);
		opt(hud::kOptAttackTo, 1, 0, false);  // TODO(sim): the ATTACK_TO verb (attack-move) is not ported
		if (own && ![self militarySelection].empty()) {  // army groups: "Agrupar" (the G dialog) or "Reagrupar" with a group selected
			hud::Option* o = opt(hud::kOptGroup, 2, 0, true);
			o->tex_id = [self groupIconId];
			o->tex_x = o->tex_y = 0;
			if (o->tex_id.empty()) o->label = [self selectedGroup] >= 0 ? "Reagrupar" : "Agrupar";
		}
		if (citizens) {
			opt(hud::kOptBuild, 0, 1, own);
			opt(hud::kOptBuildMilitary, 1, 1, own);
			opt(hud::kOptBuildWonder, 2, 1, own);
			opt(hud::kOptRepair, 3, 1, false);
			opt(hud::kOptAlarmPeasant, 4, 0, false);
		}
		opt(hud::kOptHalt, 0, 3, own);
		{
			uix::Ctx uc = [self uixCtx];
			if (!uix::lone_carrier(uc)) opt(hud::kOptGarrison, 1, 3, own);  // a carrier has REPEAT there instead
			uix::unit_extra_options(uc, out);  // carriers; the advanced rows (stance, formation, patrol, guard, disband)
		}
		return out;
	}
	if (!_selectedBuilds.empty()) {
		const auto [who, o] = _selectedBuilds.front();
		const ron::sim::Build* b = _game->build(who, o);
		if (!b) return out;
		const bool own = who == me;
		for (int32_t t = 50; t < 629; t++) {
			if (!_game->type(t).loaded) continue;
			const int32_t a = ron::sim::build_can_make(*_game, *b, t, 1);
			if (a == 0) continue;
			const ron::sim::Type& T = _game->type(t);
			const bool unit = ron::sim::is_unit_type(t);
			hud::Option* op = opt(unit && a >= 4 ? hud::kOptTrain : hud::kOptResearch, T.grid_x, T.grid_y + (unit && a < 4 ? 1 : 0),
			                      own && ron::sim::can_pay_cost(*_game, t, who, -1, o, 1) > 0);
			op->object = t;
			if (unit) {
				[self unitIcon:t who:who o:-1 into:op->tex_id col:op->tex_col row:op->tex_row x:op->tex_x y:op->tex_y];
			} else {
				op->tex_id = ron::sim::to_utf8(T.tex.id), op->tex_col = T.tex.col, op->tex_row = T.tex.row;
				op->tex_x = T.tex.x > 0 ? T.tex.x : -1, op->tex_y = T.tex.y > 0 ? T.tex.y : -1;
			}
		}
		// Which grid cell is free (Options::grid pass 2: the first free slot in the row or below).
		auto free_slot = [&](int row, int* gx, int* gy) {
			for (int y = row; y < 4; y++) {
				for (int x = 0; x < 5; x++) {
					bool taken = false;
					for (const hud::Option& o2 : out) taken = taken || (o2.grid_x == x && o2.grid_y == y);
					if (!taken) return *gx = x, *gy = y, true;
				}
			}
			return false;
		};
		const ron::sim::BuildView* bv = [self buildView:who o:o];
		const bool active = bv && bv->active;
		// A Market (BuildOut::make_options @00629ed0): SELL on row 2 and BUY on row 3 for food, timber, metal and oil, when the
		// leader may trade (the Bartering tech / the Nubians); IFaceMarket otherwise writes what is needed.
		if (own && active && ron::sim::type_is(*b->ptype, ron::sim::ptype::kMarket)) {
			const ron::sim::LeaderView lv = ron::sim::leader_view(*_game, me);
			if (lv.can_trade) {
				static const int goods[4] = {0, 1, 4, 5};
				bool first_sell = true, first_buy = true;
				for (int gi = 0; gi < 4; gi++) {
					const int gd = goods[gi];
					if (!ron::sim::type_avail(*_game, me, gd, 1)) continue;
					int gx, gy;
					if (free_slot(2, &gx, &gy)) {
						hud::Option* op = opt(60, gx, gy, lv.stockpile[gd] >= 100);
						op->object = gd, op->good = gd, op->price = lv.sell[gd], op->trend = _game->market.delta_flux[size_t(gd)] < 0 ? -1 : _game->market.delta_flux[size_t(gd)] > 0 ? 1 : 0;
						if (first_sell) op->label = [self locString:2049 fallback:"Sell:"], first_sell = false;
					}
				}
				for (int gi = 0; gi < 4; gi++) {
					const int gd = goods[gi];
					if (!ron::sim::type_avail(*_game, me, gd, 1)) continue;
					int gx, gy;
					if (free_slot(3, &gx, &gy)) {
						hud::Option* op = opt(57, gx, gy, lv.stockpile[2] >= lv.buy[gd]);
						op->object = gd, op->good = gd, op->price = lv.buy[gd], op->trend = _game->market.delta_flux[size_t(gd)] < 0 ? -1 : _game->market.delta_flux[size_t(gd)] > 0 ? 1 : 0;
						if (first_buy) op->label = [self locString:2050 fallback:"Buy:"], first_buy = false;
					}
				}
			} else {
				const ron::sim::Type& bs = _game->type(ron::sim::ptype::kBuySell);
				const int need = bs.preq[0];
				if (need >= 0) {
					static const char* cats[4] = {"Military", "Civic", "Commerce", "Science"};
					const ron::sim::Type& NT = _game->type(need);
					_cardMessage = notices::fill([self locString:2046 fallback:"Need $TECHNAME0 To Buy/Sell Resources"], {[self typeName:need]}) + "\n" +
					               notices::fill([self locString:2047 fallback:"$TECHCATEGORY0 Level $NUMBER0"],
					                             {uistr::tr(cats[NT.cat & 3]), std::to_string(NT.age + 1)});
				}
			}
		}
		// EJECT_ALL (2,3): a building with units inside.
		if (own && active && ron::sim::num_inside(*_game, *b, 1) > 0 && !ron::sim::can_carry_domain(*_game, *b, ron::sim::kDomainAir)) {
			int gx, gy;
			if (!free_slot(3, &gx, &gy)) gx = 2, gy = 3;
			// BACK_TO_WORK (1,3): citizens (50) or militia (66) inside, next to EJECT_ALL (BuildOut::make_options @00629ed0).
			if (ron::sim::count_inside(*_game, *b, ron::sim::kCountType, ron::sim::ptype::kPeasants) != 0 || ron::sim::count_inside(*_game, *b, ron::sim::kCountType, 66) != 0)
				opt(hud::kOptBackToWork, 1, 3, true);
			opt(hud::kOptEjectAll, std::min(gx, 2), 3, true);
		}
		{
			uix::Ctx uc = [self uixCtx];
			uix::build_extra_options(uc, out);  // airbase: SCRAMBLE / REPEAT / RECALL; silo: ATTACK_GROUND
		}
	}
	return out;
}

// IFaceResources::draw_infra's counts (LeaderView::infra_workers / infra_limit): the citizens working at each kind of gather
// building and the leader's slot limits; wealth is the caravans over get_caravan_limit.
- (void)fillInfra:(hud::Resources&)r who:(int)me {
	if (!_game || me < 0 || me >= 8) return;
	const ron::sim::LeaderView lv = ron::sim::leader_view(*_game, me);
	for (int k = 0; k < 6; k++) r.workers[size_t(k)] = lv.infra_workers[k], r.limit[size_t(k)] = lv.infra_limit[k];
}

// IFaceMainBase::draw_timers (docs/ui-spec.md 6.1): the victory and elimination clocks that run. The wonder and territory
// clocks count down from the map-scaled rules constants (Game::wonder_timer / popwin_timer), the capital clock from
// Game::retake_capital, the time limit from the game's seconds.
- (std::vector<hud::Timer>)gameTimers {
	std::vector<hud::Timer> out;
	if (!_game || !_game->rules) return out;
	const ron::sim::Game& g = *_game;
	auto constant = [&](const char* name) -> int32_t {
		const int off = ron::sim::constant_offset(name);
		return off < 0 ? -1 : g.rules->constants.at(uint32_t(off));
	};
	auto map_scaled = [&](int32_t k) -> int32_t {  // (ms / 2 + xs * k) / ms, at least 1 (ms = map_sizes[3], 70)
		if (k < 1) return 0;
		int32_t ms = 70;
		if (const ron::sim::CategoryList* cl = g.rules->category_list("map_sizes"); cl && cl->items.size() > 3 && cl->items[3].data[0] > 0)
			ms = cl->items[3].data[0];
		return std::max(1, (ms / 2 + g.world.xs * k) / ms);
	};
	// IFaceMainBase::draw_timers (standard victory rules): the wonder clock shows the leader Game::wonder_winning names (the best net
	// wonder points at or over the goal; allies share the tie), with `wonder_timer` map-scaled frames minus the time since that
	// leader's stamp, 0 under a World Government, and the net points (LeaderData::get_wonder_net) over the icon.
	if (g.info.victory == 0) {
		const int w = ron::sim::wonder_winning(g);
		if (w >= 0 && w < 8) {
			const ron::sim::Leader& WL = g.leaders[size_t(w)];
			int left = map_scaled(constant("wonder_timer"));
			if (WL.wonderwin_timer) left -= g.frame - WL.wonderwin_stamp;
			if (left < 0 || ron::sim::has_preq(g, w, ron::sim::ptype::kGlobalGovernmentBonus)) left = 0;
			out.push_back({0, left, WL.color, ron::sim::get_wonder_net(g, w)});
		}
	}
	for (int i = 0; i < 8; i++) {
		const ron::sim::Leader& L = g.leaders[size_t(i)];
		if ((L.flags & 3u) != 3u) continue;
		if (L.popwin_timer) out.push_back({3, map_scaled(constant("popwin_timer")) - (g.frame - L.popwin_stamp), L.color, -1});
		if (L.lost_capital_timer && g.info.elimination == 1)
			out.push_back({1, ron::sim::game_retake_capital(g, i) - (g.frame - L.lost_capital_stamp), L.color, -1});
	}
	if (g.info.victory == 4) {
		int32_t lim = 300;
		if (g.info.time_limit < 8) {
			if (const ron::sim::CategoryList* cl = g.rules->category_list("time_limits"); cl && g.info.time_limit < cl->items.size())
				lim = cl->items[g.info.time_limit].data[0];
		} else if (g.scen.custom_time_limit != 0) {
			lim = g.scen.custom_time_limit;
		}
		// draw_timers: the clock is the time-limit icon; in the last 300 frames it flashes (red / yellow under 120, white / yellow under
		// 300), and with team style 3 (an attacker against a defender) the barbarian icon takes over for those last frames.
		const int left = std::max(0, lim * 900 - g.frame);
		hud::Timer t{5, (lim * 60 - g.tick) * 15, -1, -1};
		if (left < 120) t.tint = 1, t.kind = g.info.team_style == 3 ? 6 : 5;
		else if (left < 300) t.tint = 2, t.kind = g.info.team_style == 3 ? 6 : 5;
		out.push_back(t);
	}
	// Musical chairs: the next round, chairs[chairs] * 900 frames after the last defeat (draw_timers kind 7; red / yellow flash in the last 450).
	if (g.info.victory == 5) {
		int32_t minutes = 1;
		if (const ron::sim::CategoryList* cl = g.rules->category_list("chairs"); cl && g.info.chairs < cl->items.size()) minutes = cl->items[g.info.chairs].data[0];
		hud::Timer t{7, std::max(0, minutes * 900 - (g.frame - g.musical_chairs)), -1, -1};
		if (t.frames < 450) t.tint = 1;
		out.push_back(t);
	}
	// The armageddon clock: the nukes left before Armageddon (Game::get_armageddon - armageddon) once one has been launched.
	if (g.armageddon > 0) out.push_back({2, 0, -1, std::max(0, ron::sim::get_armageddon(g) - g.armageddon)});
	for (hud::Timer& t : out) t.frames = std::max(0, t.frames);
	return out;
}

// IFaceMiniLibrary: the next tech of each library line (epoch tech 551 + 7 * line base + epoch held) and the next age, with
// the progress of one in production at an own Library (435).
- (hud::MiniLib)miniLibState:(int)me {
	hud::MiniLib ml;
	if (!_game || me < 0 || me >= 8) return ml;
	const ron::sim::Leader& L = _game->leaders[size_t(me)];
	bool library = false;
	std::vector<std::pair<int, int>> inprod;  // (type, percent)
	for (const ron::sim::BuildView& b : _curView.builds) {
		if (!b.alive || b.who != me || b.type != 435) continue;
		if (b.active) library = true;
		for (const auto& q : b.queue) inprod.push_back({q.type, q.time > 0 ? int(int64_t(q.job_counter) * 100 / q.time) : 0});
	}
	auto fill = [&](hud::MiniLibButton& b, int type) {
		if (type < 0 || !_game->type(type).loaded) return;
		const ron::sim::Type& T = _game->type(type);
		b.valid = true, b.enabled = library && ron::sim::can_pay_cost(*_game, type, me, -1, -1, 1) > 0;
		b.tex_id = ron::sim::to_utf8(T.tex.id), b.tex_col = T.tex.col, b.tex_row = T.tex.row;
		b.tex_x = T.tex.x > 0 ? T.tex.x : -1, b.tex_y = T.tex.y > 0 ? T.tex.y : -1;
		for (const auto& [qt, pct] : inprod) {
			if (qt == type) b.percent = pct;
		}
	};
	static const int base[4] = {572, 565, 558, 551};  // military, civic, commerce, science
	for (int c = 0; c < 4; c++) {
		ml.epoch[c] = L.epoch[size_t(c)];
		if (L.epoch[size_t(c)] < 7) fill(ml.cat[c], base[c] + L.epoch[size_t(c)]);
	}
	if (L.age < 7) fill(ml.age, 544 + L.age);
	return ml;
}

// WorldMap::render_borders / WorldMapFog::fill_colors_who: a fog-resolution texture of the owners' border colours, alpha 0x50
// inside and 0xC8 on edges (a neighbour unowned or not allied; water ignored), for what the viewer has explored. Rebuilt every
// 45 frames: territory changes slowly.
- (void)updateTerritory {
	if (!_game || !_uiReady || _fog.fxs <= 0 || _fogLevels.empty()) return;
	if (_miniTerr && _game->frame - _terrFrame < 45 && _game->frame >= _terrFrame) return;
	_terrFrame = _game->frame;
	const int fx = _fog.fxs, fy = _fog.fys;
	if (!_miniTerr) {
		MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:NSUInteger(fx) height:NSUInteger(fy) mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		_miniTerr = [_device newTextureWithDescriptor:d];
		_ui.set_texture("MINIMAP_TERR", _miniTerr, fx, fy);
	}
	static const uint8_t border[9][3] = {{255, 0, 0}, {0, 0, 255}, {188, 0, 250}, {0, 255, 0}, {255, 255, 0},
	                                     {0, 255, 255}, {255, 255, 255}, {255, 128, 0}, {255, 255, 255}};
	const ron::sim::World& w = _game->world;
	auto owner = [&](int x, int y) -> int {  // -1 unowned, -2 water / off the map (ignored)
		if (x < 0 || y < 0 || x >= fx || y >= fy) return -2;
		const ron::sim::WData& d = w.w(x / 2, y / 2);
		if (ron::sim::World::is_ocean(d)) return -2;
		return d.who >= 0 && d.who < 8 ? d.who : -1;
	};
	std::vector<uint8_t> rgba(size_t(fx) * size_t(fy) * 4, 0);
	const bool fog = _fogOn;
	for (int y = 0; y < fy; y++) {
		for (int x = 0; x < fx; x++) {
			const int who = owner(x, y);
			if (who < 0) continue;
			if (fog && _fogLevels[size_t(y) * size_t(fx) + size_t(x)] == 0) continue;
			bool edge = false;
			for (int dy = -1; dy <= 1 && !edge; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					if (!dx && !dy) continue;
					const int n = owner(x + dx, y + dy);
					if (n == -2) continue;
					if (n == -1 || (n != who && !ron::sim::is_ally(*_game, who, n))) {
						edge = true;
						break;
					}
				}
			}
			const uint8_t* c = border[std::clamp(_game->leaders[size_t(who)].color, 0, 8)];
			uint8_t* px = &rgba[(size_t(y) * size_t(fx) + size_t(x)) * 4];
			px[0] = c[0], px[1] = c[1], px[2] = c[2], px[3] = edge ? 0xC8 : 0x50;
		}
	}
	[_miniTerr replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(fx), NSUInteger(fy)) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:NSUInteger(fx) * 4];
}

// An order failed for lack of goods (GroupOut::issue_queue_up / validate_build -> LeaderOut::warn_resources): the rows of the
// goods the item's price needs more of than the leader has turn red for 8 ticks.
- (void)flashLacking:(int)type who:(int)who {
	if (!_game || who < 0 || who >= 8) return;
	int mask = 0;
	for (int k = 0; k < 6; k++) {
		if (!ron::sim::type_avail(*_game, who, k, 1)) continue;
		const int32_t c = ron::sim::get_cost(*_game, type, k, who, -1, -1, 0, 1, -1);
		if (c > _game->leaders[size_t(who)].bucket[size_t(k)]) mask |= 1 << k;
	}
	if (mask) _flashMask = mask, _flashTick = int(_timeMs / 66.0);
}

// IFaceLibrary: shown for a single selected, finished Library of the viewer (or an ally); the cells are the seven ages and the
// seven levels of the four lines (military 572.., civic 565.., commerce 558.., science 551..), enabled as the Library's own
// card would offer them (BuildData::can_make against the first Library, affordable), owned when researched, with the progress of
// the one in production.
- (hud::Library)libraryState:(int)me {
	hud::Library lib;
	if (!_game || me < 0 || me >= 8 || _selectedBuilds.size() != 1 || !_selected.empty()) return lib;
	const int32_t who = _selectedBuilds.front().first, o = _selectedBuilds.front().second;
	const ron::sim::BuildView* bv = [self buildView:who o:o];
	if (!bv || !bv->active || bv->type != 435) return lib;
	if (who != me && !ron::sim::is_ally(*_game, who, me)) return lib;
	const ron::sim::Build* b = _game->build(who, o);
	if (!b) return lib;
	lib.active = true;
	const ron::sim::Leader& L = _game->leaders[size_t(me)];
	std::vector<std::pair<int, int>> inprod;
	for (const ron::sim::BuildView& v : _curView.builds) {
		if (!v.alive || v.who != me || v.type != 435) continue;
		for (const auto& q : v.queue) inprod.push_back({q.type, q.time > 0 ? int(int64_t(q.job_counter) * 100 / q.time) : 0});
	}
	const int first = ron::sim::get_first_library(*_game, me);
	const ron::sim::Build* fb = first >= 0 ? _game->build(me, first) : b;
	auto fill = [&](hud::LibCell& c, int type) {
		if (type < 0 || !_game->type(type).loaded) return;
		const ron::sim::Type& T = _game->type(type);
		c.type = type;
		c.owned = ron::sim::has_tech(*_game, me, type);
		c.enabled = who == me && !c.owned && fb && ron::sim::build_can_make(*_game, *fb, type, 1) != 0 && ron::sim::can_pay_cost(*_game, type, me, -1, first, 1) > 0;
		c.tex_id = ron::sim::to_utf8(T.tex.id), c.tex_col = T.tex.col, c.tex_row = T.tex.row;
		c.tex_x = T.tex.x > 0 ? T.tex.x : -1, c.tex_y = T.tex.y > 0 ? T.tex.y : -1;
		for (const auto& [qt, pct] : inprod) {
			if (qt == type) c.percent = pct;
		}
	};
	static const int base[4] = {572, 565, 558, 551};
	for (int i = 0; i < 7; i++) {
		fill(lib.age[i], 544 + i);
		for (int k = 0; k < 4; k++) fill(lib.tech[k][i], base[k] + i);
	}
	(void)L;
	{
		uix::Ctx uc = [self uixCtx];
		uix::library_final(uc, first, inprod, lib);  // the final-techs view (IFaceLibrary jeopardy)
	}
	return lib;
}

// IFaceGarrison: the units inside the selected building / carrier, grouped by owner and type.
- (hud::Garrison)garrisonState:(int)me {
	hud::Garrison gar;
	if (!_game || (_selected.size() + _selectedBuilds.size()) != 1) return gar;
	int32_t who, o;
	if (!_selectedBuilds.empty()) who = _selectedBuilds.front().first, o = _selectedBuilds.front().second;
	else who = _selected.front().first, o = _selected.front().second;
	const ron::sim::Object* ob = _game->object(who, o);
	if (!ob || !ob->alive() || ron::sim::num_inside(*_game, *ob, 1) == 0) return gar;
	if (who != me && !ron::sim::is_ally(*_game, who, me)) return gar;
	// Air garrisons (airbases, carriers: IFaceAirGarrison) are not drawn.
	if (ron::sim::can_carry_domain(*_game, *ob, ron::sim::kDomainAir)) return gar;
	gar.active = true;
	gar.building = o, gar.owner = who;
	std::map<std::pair<int32_t, int32_t>, size_t> index;
	int32_t cw = ob->inside_down_who, co = ob->inside_down;
	for (int guard = 0; co >= 0 && guard < 4096; guard++) {
		const ron::sim::Unit* u = _game->unit(cw, co);
		if (!u) break;
		if (u->alive()) {
			auto [it, fresh] = index.try_emplace({u->who, u->type}, gar.units.size());
			if (fresh) {
				hud::GarrisonUnit g;
				g.who = u->who, g.type = u->type;
				g.own = u->who == me;
				const int color = _game->leaders[size_t(u->who & 7)].color;
				const uint8_t* tc = kUnitTeamColors[std::clamp(color, 0, 8)];
				g.rgb[0] = tc[0], g.rgb[1] = tc[1], g.rgb[2] = tc[2];
				[self unitIcon:u->type who:u->who o:u->o into:g.tex_id col:g.tex_col row:g.tex_row x:g.tex_x y:g.tex_y];
				gar.units.push_back(g);
			} else {
				gar.units[it->second].count++;
			}
			gar.units[it->second].objects.push_back(u->o);
			if (u->who != who) gar.mixed_owners = true;
		}
		cw = u->inside_down_who, co = u->inside_down;
	}
	if (gar.units.empty()) gar.active = false;
	return gar;
}

// IFaceMainBase::setup_progress_buttons: every queued unit or tech of the viewer's active buildings (and the wonders under
// construction), with the progress of the item and the producer's position.
- (std::vector<hud::ProgressItem>)progressItems:(int)me {
	std::vector<hud::ProgressItem> out;
	if (!_game || me < 0 || me >= 8) return out;
	const uint8_t* tc = kUnitTeamColors[std::clamp(_game->leaders[size_t(me)].color, 0, 8)];
	for (const ron::sim::BuildView& b : _curView.builds) {
		if (!b.alive || b.who != me) continue;
		if (!b.active) {
			if (_game->type(b.type).cat == 2 && b.started && b.construct_time > 0) {  // an unbuilt wonder
				hud::ProgressItem p;
				const ron::sim::Type& T = _game->type(b.type);
				p.tex_id = ron::sim::to_utf8(T.tex.id), p.tex_col = T.tex.col, p.tex_row = T.tex.row;
				p.tex_x = T.tex.x > 0 ? T.tex.x : -1, p.tex_y = T.tex.y > 0 ? T.tex.y : -1;
				p.percent = std::clamp(b.job_counter * 100 / b.construct_time, 0, 100);
				p.who = b.who, p.o = b.o, p.wx = float(b.x), p.wy = float(b.y);
				p.rgb[0] = tc[0], p.rgb[1] = tc[1], p.rgb[2] = tc[2];
				p.name = [self typeName:b.type];
				out.push_back(p);
			}
			continue;
		}
		for (const auto& q : b.queue) {
			if (q.type < 0) continue;
			if (!ron::sim::is_unit_type(q.type) && ron::sim::has_tech(*_game, me, q.type)) continue;
			hud::ProgressItem p;
			if (ron::sim::is_unit_type(q.type)) {
				[self unitIcon:q.type who:me o:-1 into:p.tex_id col:p.tex_col row:p.tex_row x:p.tex_x y:p.tex_y];
			} else {
				const ron::sim::Type& Q = _game->type(q.type);
				p.tex_id = ron::sim::to_utf8(Q.tex.id), p.tex_col = Q.tex.col, p.tex_row = Q.tex.row;
				p.tex_x = Q.tex.x > 0 ? Q.tex.x : -1, p.tex_y = Q.tex.y > 0 ? Q.tex.y : -1;
			}
			p.percent = q.time > 0 ? std::clamp(int(int64_t(q.job_counter) * 100 / q.time), 0, 100) : 0;
			p.who = b.who, p.o = b.o, p.wx = float(b.x), p.wy = float(b.y);
			p.rgb[0] = tc[0], p.rgb[1] = tc[1], p.rgb[2] = tc[2];
			p.name = [self typeName:q.type];
			out.push_back(p);
			if (out.size() >= 70) return out;
		}
	}
	return out;
}

- (hud::State)makeHudState {
	hud::State s;
	s.world_w = std::max(1.0f, _mesh.width);
	s.world_h = std::max(1.0f, _mesh.height);
	if (_fogOn && _miniFog && !_miniFogLevels.empty()) {
		s.fog = &_miniFogLevels;
		s.fog_xs = _fog.fxs, s.fog_ys = _fog.fys;
	}
	s.has_minimap_bg = _miniBG != nil;
	const int me = [self localPlayer];
	if (_game) {
		for (const ron::sim::UnitView& u : _curView.units) {
			if (!u.alive || !u.on_map) continue;
			if (_fogOn && !(u.seen_by & (1u << (me & 7)))) continue;
			if (u.attacking && ((int(_timeMs / 66.0)) & 8)) continue;  // units attacking blink on the minimap (WorldMap::add_blip batch 3)
			hud::Blip b;
			b.x = float(u.x), b.y = float(u.y);
			const uint8_t* tc = kUnitTeamColors[std::clamp(u.color, 0, 8)];
			b.rgb[0] = tc[0], b.rgb[1] = tc[1], b.rgb[2] = tc[2];
			b.size = u.type >= 50 && u.type <= 53 ? 1 : 2;  // citizens and scholars size 1
			s.blips.push_back(b);
		}
		for (const ron::sim::BuildView& v : _curView.builds) {
			if (!v.alive) continue;
			if (_fogOn && !(v.seen_by & (1u << (me & 7)))) continue;
			if (v.under_attack && v.who == me && ((int(_timeMs / 66.0)) & 8)) continue;  // an own building under attack blinks (BuildView::under_attack)
			hud::Blip b;
			b.x = float(v.x), b.y = float(v.y), b.building = true;
			const uint8_t* tc = kUnitTeamColors[std::clamp(v.color, 0, 8)];
			b.rgb[0] = tc[0], b.rgb[1] = tc[1], b.rgb[2] = tc[2];
			b.size = v.city ? 6 : 3;
			s.blips.push_back(b);
		}
	}
	const CGSize size = [self drawSize];
	const simd_float2 a = [self groundAt:CGPointMake(0, 0)], b = [self groundAt:CGPointMake(size.width, size.height)];
	s.view_corners[0] = a.x, s.view_corners[1] = a.y, s.view_corners[2] = b.x, s.view_corners[3] = b.y;
	for (const auto& [who, o] : _selected) {
		const ron::sim::UnitView* u = [self unitView:who o:o];
		if (!u) continue;
		hud::Object ob;
		ob.who = who, ob.o = o, ob.type = u->type, ob.hp = u->hit_points, ob.max_hp = u->max_hit_points, ob.own = who == me;
		const uint8_t* tc = kUnitTeamColors[std::clamp(u->color, 0, 8)];
		ob.rgb[0] = tc[0], ob.rgb[1] = tc[1], ob.rgb[2] = tc[2];
		ob.name = [self typeName:u->type];
		[self unitIcon:u->type who:who o:o into:ob.tex_id col:ob.tex_col row:ob.tex_row x:ob.tex_x y:ob.tex_y];
		const ron::sim::Type& T = _game->type(u->type);
		ob.attack = T.attack > 0 ? T.attack / 10 : -1;  // draw_stat: base / 10 (+ bonus / 10, no bonuses yet)
		ob.armor = T.armor > 0 ? T.armor : -1;
		ob.range = T.max_range > 0 ? T.max_range : -1;
		s.selection.push_back(ob);
	}
	for (const auto& [who, o] : _selectedBuilds) {
		const ron::sim::BuildView* v = [self buildView:who o:o];
		if (!v) continue;
		hud::Object ob;
		ob.building = true;
		ob.who = who, ob.o = o, ob.type = v->type, ob.hp = v->hit_points, ob.max_hp = v->max_hit_points, ob.own = who == me;
		const uint8_t* tc = kUnitTeamColors[std::clamp(v->color, 0, 8)];
		ob.rgb[0] = tc[0], ob.rgb[1] = tc[1], ob.rgb[2] = tc[2];
		ob.name = [self typeName:v->type];
		const ron::sim::Type& T = _game->type(v->type);
		ob.tex_id = ron::sim::to_utf8(T.tex.id), ob.tex_col = T.tex.col, ob.tex_row = T.tex.row;
		ob.tex_x = T.tex.x > 0 ? T.tex.x : -1, ob.tex_y = T.tex.y > 0 ? T.tex.y : -1;
		if (!v->active && v->construct_time > 0) ob.progress = std::clamp(v->job_counter * 100 / v->construct_time, 0, 100), ob.building_site = true;
		for (const auto& q : v->queue) {
			hud::QueueItem qi;
			qi.type = q.type;
			qi.percent = q.time > 0 ? std::clamp(int(int64_t(q.job_counter) * 100 / q.time), 0, 100) : 0;
			if (ron::sim::is_unit_type(q.type)) [self unitIcon:q.type who:who o:-1 into:qi.tex_id col:qi.tex_col row:qi.tex_row x:qi.tex_x y:qi.tex_y];
			else {
				const ron::sim::Type& Q = _game->type(q.type);
				qi.tex_id = ron::sim::to_utf8(Q.tex.id), qi.tex_col = Q.tex.col, qi.tex_row = Q.tex.row;
			}
			qi.name = [self typeName:q.type];
			ob.queue.push_back(qi);
		}
		ob.repeat = v->repeat;
		s.selection.push_back(ob);
	}
	s.options = [self hudOptions];
	s.res = hud_resources(_game.get(), me);
	[self fillInfra:s.res who:me];
	s.timers = [self gameTimers];
	if (const char* tt = getenv("RON_TIMER_TEST")) {  // "kind:seconds[:colour[:count]],..." for snapshots
		for (const char* q = tt; q && *q;) {
			int kind = 0, secs = 0, who = -1, count = -1;
			std::sscanf(q, "%d:%d:%d:%d", &kind, &secs, &who, &count);
			s.timers.push_back({kind, secs * 15, who, count});
			q = std::strchr(q, ',');
			if (q) q++;
		}
	}
	s.minilib = [self miniLibState:me];
	s.card_message = _cardMessage;
	s.library = [self libraryState:me];
	s.garrison = [self garrisonState:me];
	{
		uix::Ctx uc = [self uixCtx];
		s.air = uix::air_garrison(uc, [self](int32_t type, int32_t who, int32_t o, hud::Icon& ic) {
			[self unitIcon:type who:who o:o into:ic.tex_id col:ic.col row:ic.row x:ic.x y:ic.y];
		});
	}
	if (_progressButtons) s.progress = [self progressItems:me];
	s.flash_mask = _flashMask, s.flash_tick = _flashTick;
	if (const char* ft = getenv("RON_FLASH_TEST")) s.flash_mask = std::atoi(ft), s.flash_tick = int(_timeMs / 66.0);  // a bit mask of goods, for snapshots
	s.speed_name = [self speedName];
	s.paused_name = uistr::tr("Paused");
	if (_game && me >= 0 && me < 8) {
		const ron::sim::LeaderView lv = ron::sim::leader_view(*_game, me);
		for (const ron::sim::LeaderView::Rare& r : lv.rares) s.rares.push_back({r.good, r.count});
	}
	if (const char* rt = getenv("RON_RARE_TEST")) {  // "good:count,..." for snapshots
		for (const char* q = rt; q && *q;) {
			int good = 0, count = 1;
			std::sscanf(q, "%d:%d", &good, &count);
			s.rares.push_back({good, count});
			q = std::strchr(q, ',');
			if (q) q++;
		}
	}
	[self updateTerritory];
	s.has_territory = _miniTerr != nil;
	if (_game && me >= 0 && me < int(_game->leaders.size())) {
		const ron::sim::Leader& L = _game->leaders[size_t(me)];
		std::string nation;
		if (_simRules && L.tribe >= 0 && size_t(L.tribe) < _simRules->tribes.size()) nation = ron::sim::to_utf8(_simRules->tribes[size_t(L.tribe)].name);
		// The age name: the age tech's display name (Classical = 544 .. Information = 550); Ancient has no tech.
		const std::string age = L.age >= 1 && L.age <= 7 ? [self typeName:543 + L.age] : std::string("Ancient Age");
		s.caption = nation.empty() ? age : nation + " - " + age;
		int bc = 0;
		if (_simRules && L.tribe >= 0 && size_t(L.tribe) < _simRules->tribes.size()) bc = _simRules->tribes[size_t(L.tribe)].build_continent;
		_ui.set_skin(ron::iface_skin_code(L.age, bc));
	}
	s.frame = _game ? _game->frame : 0;
	s.paused = _paused;
	s.graphic_tick = int(_timeMs / 66.0);
	if (const ron::sim::ScriptPresentation* p = [self pres]) {
		s.highlight = p->highlight_active;
		s.hilite_option = p->hilite_option >= 0 ? p->hilite_option : _hiliteOverride;
		s.hilite_object = p->hilite_object;
		if (s.highlight) {
			static std::string highlight_tex;
			if (highlight_tex.empty()) {
				// effects_graphics.xml BUTTONHIGHLIGHT (GraphicPieces::button_high_light).
				std::string file = "art/tutorial_button_highlight_01.tga";
				try {
					ron::for_each_element(ron::parse_xml(_files->read_text("Data/effects_graphics.xml")), "BUTTONHIGHLIGHT",
					                      [&](const ron::XmlElement& e) { file = ron::normalize_game_path(e.get("file")); });
				} catch (const std::exception&) {
				}
				_ui.file_texture(file, &highlight_tex);
			}
			s.highlight_tex = highlight_tex;
		}
	}
	if (_sviewOn) {
		s.pings = _sview.pings();
		if (!s.pings.empty()) {
			s.ping_styles = _sview.ping_styles();  // EFFECTS/MINIMAP/PING
			for (hud::PingStyle& st : s.ping_styles) {
				for (hud::PingPhase& ph : st.phases) _ui.file_texture(ph.file, &ph.tex_id);
			}
		}
	}
	return s;
}

// ---------------------------------------------------------------- mouse-over help (docs/ui-spec.md 6.5)

// The hotkey of an option for the current selection: the key bound to it in the build menu or the standard mode, gated by
// the selection's context (units, buildings, a building's type).
- (BOOL)selectionContext:(int)ctx {
	if (ctx == -1) return YES;
	const bool units = !_selected.empty(), builds = !_selectedBuilds.empty();
	if (ctx == -2 || ctx == -4) return builds;
	if (ctx == -3) return units;
	if (ctx == -5 || ctx == -6) {  // -5 aircraft, -6 land and sea units
		if (!units || !_game) return NO;
		bool air = true, ground = true;
		for (const auto& [who, o] : _selected) {
			const ron::sim::Unit* u = _game->unit(who, o);
			const bool is_air = u && u->ptype && u->ptype->domain == ron::sim::kDomainAir;
			air = air && is_air, ground = ground && !is_air;
		}
		return ctx == -5 ? air : ground;
	}
	if (!_game) return NO;
	for (const auto& [who, o] : _selectedBuilds) {
		if (const ron::sim::BuildView* b = [self buildView:who o:o]) {
			if (b->type == ctx) return YES;
		}
	}
	for (const auto& [who, o] : _selected) {
		if (const ron::sim::UnitView* u = [self unitView:who o:o]) {
			if (u->type == ctx) return YES;
		}
	}
	return NO;
}

- (std::string)hotkeyFor:(const char*)option param:(int)param {
	return keymap::hotkey(option, param, [self](int ctx) { return [self selectionContext:ctx] == YES; });
}

static std::string up_str(std::string s) {
	for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

// The OptionIndex -> KeyMap option name of the card's plain buttons.
static const char* keymap_option_name(int option) {
	switch (option) {
		case hud::kOptBuild: return "OPTION_BUILD";
		case hud::kOptBuildMilitary: return "OPTION_BUILD_MILITARY";
		case hud::kOptBuildWonder: return "OPTION_BUILD_WONDER";
		case hud::kOptRepair: return "OPTION_REPAIR";
		case hud::kOptPatrol: return "OPTION_PATROL";
		case hud::kOptMoveTo: return "OPTION_MOVE_TO";
		case hud::kOptAttackTo: return "OPTION_ATTACK_TO";
		case hud::kOptDisband: return "OPTION_DISBAND";
		case hud::kOptStance: return "OPTION_STANCE";
		case hud::kOptForm: return "OPTION_FORM";
		case hud::kOptHalt: return "OPTION_HALT";
		case hud::kOptGarrison: return "OPTION_GARRISON";
		case hud::kOptAlarmPeasant: return "OPTION_ALARM_PEASANT";
		case hud::kOptBackToWork: return "OPTION_BACK_TO_WORK";
		case hud::kOptEjectAll: return "OPTION_EJECT_ALL";
		case hud::kOptAutoExplore: return "OPTION_AUTO_EXPLORE";
		case hud::kOptCancel: return "OPTION_CANCEL";
		case 65: return "OPTION_NEXT";
		case 66: return "OPTION_BACK";
		case uix::kOptDisbandAll: return "OPTION_DISBAND_ALL";
		case uix::kOptGuard: return "OPTION_GUARD";
		case uix::kOptAttackGround: return "OPTION_ATTACK_GROUND";
		case uix::kOptTransfer: return "OPTION_TRANSFER";
		case uix::kOptAirstrike: return "OPTION_AIRSTRIKE";
		case uix::kOptFighters: return "OPTION_FIGHTERS";
		case uix::kOptBombers: return "OPTION_BOMBERS";
		case uix::kOptHelis: return "OPTION_HELIS";
		case uix::kOptAllAir: return "OPTION_ALLAIR";
		case uix::kOptScramble: return "OPTION_SCRAMBLE";
		case uix::kOptRepeat: return "OPTION_REPEAT";
		case uix::kOptReturn: return "OPTION_RETURN";
		case uix::kOptRecall: return "OPTION_RECALL";
		case uix::kOptInfinite: return "OPTION_INFINITE";
		case uix::kOptQueue: return "OPTION_QUEUE";
		default: return nullptr;
	}
}

// The help.xml group and key of a plain card button (Options::set_help_type's choice for the options the card offers).
static std::pair<const char*, const char*> option_help_key(int option) {
	switch (option) {
		case hud::kOptMoveTo: return {"ORDERS", "MOVETO"};
		case hud::kOptAttackTo: return {"ORDERS", "ATTACKTO"};
		case hud::kOptPatrol: return {"ORDERS", "PATROL"};
		case hud::kOptHalt: return {"EJECTS", "HALT"};
		case hud::kOptGarrison: return {"EJECTS", "GARRISON"};
		case hud::kOptAlarmPeasant: return {"EJECTS", "ALARMPEASANT"};
		case hud::kOptEjectAll: return {"EJECTS", "EJECTALL"};
		case hud::kOptAutoExplore: return {"ORDERS", "AUTOEXPLORE"};
		case uix::kOptGuard: return {"ORDERS", "GUARD"};
		case uix::kOptAttackGround: return {"ORDERS", "ATTACKGROUND"};
		case uix::kOptTransfer: return {"AIR", "AIRTRANSFER"};
		case uix::kOptAirstrike: return {"AIR", "AIRSTRIKE"};
		case uix::kOptAirPatrol: return {"AIR", "AIRPATROL"};
		case uix::kOptFighters: return {"AIR", "AIRFIGHTERS"};
		case uix::kOptBombers: return {"AIR", "AIRBOMBERS"};
		case uix::kOptHelis: return {"AIR", "AIRHELIS"};
		case uix::kOptAllAir: return {"AIR", "AIRALL"};
		case uix::kOptScramble: return {"AIR", "AIRSCRAMBLE"};
		case uix::kOptRepeat: return {"AIR", "AIRREPEAT"};
		case uix::kOptReturn: return {"AIR", "AIRRETURN"};
		case uix::kOptRecall: return {"AIR", "AIRRECALL"};
		case hud::kOptStance: return {"STANCES", "STANCE"};
		default: return {nullptr, nullptr};
	}
}

// The tooltip of the hovered element (MouseOver::describe -> Options::describe / the spot handlers): title, cost, hotkey and
// the help.xml lines at the profile's verbosity.
- (tips::Tip)tipFor:(const hud::Hover&)h state:(const hud::State&)s {
	tips::Tip tip;
	if (!_game || !_helpOn || h.kind == hud::HoverKind::None) return tip;
	const int me = [self localPlayer];
	const ron::sim::LeaderView lv = ron::sim::leader_view(*_game, me);
	const int v = _verbosity;
	auto lines = [&](const std::string& key, std::vector<std::string> sections, const std::map<std::string, std::string>& vars = {}) {
		for (std::string& l : _help.lines(key, v, sections, vars)) tip.info.push_back(std::move(l));
	};
	auto good_name = [&](int g) { return [self typeName:g]; };
	std::map<std::string, std::string> vars;
	switch (h.kind) {
		case hud::HoverKind::Option: {
			if ((h.a == 57 || h.a == 60) && h.b >= 0 && h.b < 6) {
				// The Market's own BUY / SELL buttons (IFaceMarket): as the resource panel's.
				const bool buy = h.a == 57;
				tip.first = (_hud.option_name(h.a).empty() ? uistr::tr(buy ? "Buy" : "Sell") : _hud.option_name(h.a)) + " " + good_name(h.b);
				const int price = buy ? lv.buy[h.b] : lv.sell[h.b];
				if (buy) tip.cost.push_back({2, price, lv.stockpile[2] >= price});
				else tip.info.push_back("{*1" + std::string("#ICON3 ") + std::to_string(price) + "} per 100");
				tip.hotkey = [self hotkeyFor:(buy ? "OPTION_BUY" : "OPTION_SELL") param:h.b];
				lines(buy ? "BUY" : "SELL", {"SPELLS"});
			} else if (h.b >= 0) {
				// Train / research / build buttons: the type's name, its price, the hotkey, the help.xml text for its graphic.
				const ron::sim::Type& T = _game->type(h.b);
				tip.first = [self typeName:h.b];
				const int builder = [self ownSelectedBuilds].empty() ? -1 : [self ownSelectedBuilds].front();
				for (int g = 0; g < 6; g++) {
					const int c = ron::sim::get_cost(*_game, h.b, g, me, builder, -1, 0, 0, -1);
					if (c > 0) tip.cost.push_back({g, c, lv.stockpile[g] >= c});
				}
				if (h.a == 1000) tip.hotkey = [self hotkeyFor:"OPTION_BUILD" param:h.b].empty() ? [self hotkeyFor:"OPTION_BUILD_MILITARY" param:h.b] : [self hotkeyFor:"OPTION_BUILD" param:h.b];
				else {
					// The key names the first unit of a line (Slingers for the light infantry): follow the upgrade chain back.
					const char* opt = h.a == hud::kOptTrain ? "OPTION_TRAIN" : "OPTION_RESEARCH";
					int t = h.b;
					for (int step = 0; step < 12 && t >= 0 && tip.hotkey.empty(); step++) {
						tip.hotkey = [self hotkeyFor:opt param:t];
						t = _game->type(t).from;
					}
				}
				const std::string key = up_str(ron::sim::to_utf8(T.help_name.empty() ? T.name : T.help_name));
				std::vector<std::string> secs = ron::sim::is_unit_type(h.b) ? std::vector<std::string>{"UNITS", "BUILDINGS", "SPELLS", "LIBRARY"}
				                                                           : std::vector<std::string>{"BUILDINGS", "LIBRARY", "SPELLS", "UNITS"};
				lines(key, secs);
				if (tip.info.empty() && !T.desc.empty()) tip.info.push_back(ron::sim::to_utf8(T.desc));
			} else {
				if (h.a == hud::kOptGroup) {  // the port's army-group button
					const bool re = [self selectedGroup] >= 0;
					tip.first = re ? "Reagrupar: reúne o grupo em formação" : "Agrupar: dá nome a um grupo de exército (1-9)";
					tip.hotkey = re ? "Shift+G" : "G";
					break;
				}
				// Plain buttons: "Name: description" (verbosity 2 and up; the name alone below), the hotkey, the help text.
				const std::string name = _hud.option_name(h.a), desc = _hud.option_desc(h.a);
				tip.first = v >= 2 && !desc.empty() ? (name.empty() ? desc : name + ": " + desc) : (name.empty() ? desc : name);
				if (const char* kn = keymap_option_name(h.a)) tip.hotkey = [self hotkeyFor:kn param:-2];
				const auto [sec, key] = option_help_key(h.a);
				if (sec) lines(key, {sec});
			}
			break;
		}
		case hud::HoverKind::SelObject: {
			if (size_t(h.a) >= s.selection.size()) break;
			const hud::Object& o = s.selection[size_t(h.a)];
			tip.first = o.name;
			tip.info.push_back("{*0" + std::to_string(o.hp) + "/" + std::to_string(o.max_hp) + " hit points}");
			if (const ron::sim::Type& T = _game->type(o.type); T.loaded) {
				const std::string key = up_str(ron::sim::to_utf8(T.help_name.empty() ? T.name : T.help_name));
				lines(key, o.building ? std::vector<std::string>{"BUILDINGS", "UNITS"} : std::vector<std::string>{"UNITS", "BUILDINGS"});
			}
			break;
		}
		case hud::HoverKind::SelQueue: {
			if (s.selection.empty() || size_t(h.a) >= s.selection.front().queue.size()) break;
			const hud::QueueItem& q = s.selection.front().queue[size_t(h.a)];
			tip.first = q.name;
			tip.info.push_back("Click to remove from the queue.");
			break;
		}
		case hud::HoverKind::SelStat: {
			static const char* keys[3] = {"STATATTACK", "STATARMOR", "STATRANGE"};
			static const char* names[3] = {"Attack", "Armor", "Range"};
			tip.first = names[std::clamp(h.a, 0, 2)];
			lines(keys[std::clamp(h.a, 0, 2)], {"STATS"});
			break;
		}
		case hud::HoverKind::SelHitPoints:
			tip.first = "Hit Points";
			lines("STATHITPOINTS", {"STATS"});
			break;
		case hud::HoverKind::Good: {
			static const char* keys[6] = {"FOOD", "TIMBER", "WEALTH", "KNOWLEDGE", "METAL", "OIL"};
			const int g = std::clamp(h.a, 0, 5);
			const int rate = s.res.rate[size_t(g)];
			tip.first = good_name(g);
			vars["NUMBER0"] = std::to_string(s.res.stock[size_t(g)]);
			vars["STRING0"] = std::to_string(rate);
			lines(keys[g], {"GOODS"}, vars);
			if (s.res.over_cap[size_t(g)]) lines("GOODCAPPED", {"CAPS"});
			break;
		}
		case hud::HoverKind::CommerceCap:
			tip.first = "Commerce Cap";
			lines("RESCAP", {"CAPS"});
			break;
		case hud::HoverKind::PopCap:
			tip.first = "Population Limit";
			vars["NUMBER0"] = std::to_string(s.res.pop), vars["NUMBER1"] = std::to_string(s.res.pop_cap);
			lines("POPCAP", {"CAPS"}, vars);
			break;
		case hud::HoverKind::CityCap:
			tip.first = "City Limit";
			vars["NUMBER0"] = std::to_string(s.res.cities), vars["NUMBER1"] = std::to_string(s.res.city_limit);
			lines("CITYCAP", {"CAPS"}, vars);
			break;
		case hud::HoverKind::MarketBuy: case hud::HoverKind::MarketSell: {
			const bool buy = h.kind == hud::HoverKind::MarketBuy;
			const int g = std::clamp(h.a, 0, 5);
			tip.first = (_hud.option_name(buy ? 57 : 60).empty() ? uistr::tr(buy ? "Buy" : "Sell") : _hud.option_name(buy ? 57 : 60)) + " " + good_name(g);
			const int price = buy ? s.res.buy[size_t(g)] : s.res.sell[size_t(g)];
			if (buy) tip.cost.push_back({2, price, lv.stockpile[2] >= price});
			else tip.info.push_back("{*1" + std::string("#ICON3 ") + std::to_string(price) + "} per 100");
			tip.hotkey = [self hotkeyFor:(buy ? "OPTION_BUY" : "OPTION_SELL") param:g];
			lines(buy ? "BUY" : "SELL", {"SPELLS"});
			break;
		}
		case hud::HoverKind::ToggleMarket: tip.first = "Market Interface"; lines("MARKET", {"MAIN_IFACE_BUTTONS"}); tip.hotkey = [self hotkeyFor:"OPTION_TOGGLE_IFACE_MARKET" param:-2]; break;
		case hud::HoverKind::ToggleResources: tip.first = "Resource Interface"; lines("RESOURCES", {"MAIN_IFACE_BUTTONS"}); tip.hotkey = [self hotkeyFor:"OPTION_TOGGLE_IFACE_RESOURCES" param:-2]; break;
		case hud::HoverKind::ToggleInfra: tip.first = "Infrastructure Interface"; lines("INFRA", {"MAIN_IFACE_BUTTONS"}); tip.hotkey = [self hotkeyFor:"OPTION_TOGGLE_IFACE_INFRA" param:-2]; break;
		case hud::HoverKind::Infra: {
			static const char* keys[6] = {"FOODCAP", "WOODCAP", "WEALTHCAP", "KNOWLEDGECAP", "METALCAP", "OILCAP"};
			static const char* heads[6] = {"FARMHEADS", "WOODCUTTERHEADS", "CARAVANCAP", "UNIVERSITYHEADS", "MINEHEADS", "OILHEADS"};
			const int g = std::clamp(h.a, 0, 5);
			tip.first = good_name(g) + " " + uistr::tr("Infrastructure");
			lines(g == 2 ? heads[g] : keys[g], {"CAPS", "STATS"});
			if (g != 2) lines(heads[g], {"STATS"});
			break;
		}
		case hud::HoverKind::BarButton: {
			static const char* titles[8] = {"Pause", "Advanced Menu", "Ping", "Diplomacy", "Chat", "Data Display", "Help Text Detail", "Menu"};
			static const char* keys[8] = {"PAUSE", "TOGGLEADVANCED", "PING", "DIPLOMACY", "CHAT", "NEXTDATA", "TOGGLEHELP", "MENU"};
			static const char* secs[8] = {"MAIN_IFACE_BUTTONS", "MAIN_IFACE_BUTTONS", "EJECTS", "DIPLOMACY", "EJECTS", "DATADISPLAY", "MAIN_IFACE_BUTTONS", "MAIN_IFACE_BUTTONS"};
			static const char* kmo[8] = {"OPTION_PAUSE", "OPTION_TOGGLE_ADVANCED", "OPTION_PING", "OPTION_DIPLO", "OPTION_CHAT", "OPTION_NEXT_DATA", "OPTION_TOGGLE_HELP", "OPTION_MENU"};
			const int i = std::clamp(h.a, 0, 7);
			tip.first = titles[i];
			tip.hotkey = [self hotkeyFor:kmo[i] param:-2];
			lines(keys[i], {secs[i]});
			break;
		}
		case hud::HoverKind::Display:
			tip.first = _hud.opened() ? "Streamlined Interface" : "Full Interface";
			tip.hotkey = [self hotkeyFor:"OPTION_TOGGLE_IFACE_MAIN" param:-2];
			lines("MAIN", {"MAIN_IFACE_BUTTONS"});
			break;
		case hud::HoverKind::MinimapButton: {
			static const char* titles[4] = {"Next Military Unit", "Next General", "Objectives", "Next Idle Citizen"};
			static const char* keys[4] = {"NEXTMILITARY", "NEXTGENERAL", "OBJECTIVES", "IDLECITIZEN"};
			static const char* secs[4] = {"CYCLES", "CYCLES", "MAIN_IFACE_BUTTONS", "CYCLES"};
			static const char* kmo[4] = {"OPTION_CYCLE_MILITARY", "OPTION_CYCLE_GENERAL", "OPTION_OBJECTIVES", "OPTION_CYCLE_PEASANT"};
			const int i = std::clamp(h.a, 0, 3);
			tip.first = titles[i];
			tip.hotkey = [self hotkeyFor:kmo[i] param:-2];
			lines(keys[i], {secs[i]});
			break;
		}
		case hud::HoverKind::Minimap:
			tip.first = "Minimap";
			lines("IFACEMINIMAP_MOUSEOVER", {"EJECTS"});
			break;
		case hud::HoverKind::MiniLibCat: {
			static const char* titles[5] = {"Next Age", "Military Research", "Civic Research", "Commerce Research", "Science Research"};
			static const char* keys[5] = {"AGETECH", "MILITARY", "CIVIC", "COMMERCE", "SCIENCE"};
			const int i = std::clamp(h.a, 0, 4);
			tip.first = titles[i];
			const hud::MiniLibButton& b = i == 0 ? s.minilib.age : s.minilib.cat[i - 1];
			if (b.valid) {
				const int base[4] = {572, 565, 558, 551};
				const int type = i == 0 ? 544 + _game->leaders[size_t(me)].age : base[i - 1] + _game->leaders[size_t(me)].epoch[size_t(i - 1)];
				tip.first = [self typeName:type];
				for (int g = 0; g < 6; g++) {
					const int c = ron::sim::get_cost(*_game, type, g, me, -1, -1, 0, 0, -1);
					if (c > 0) tip.cost.push_back({g, c, lv.stockpile[g] >= c});
				}
			}
			lines(keys[i], {"LIBRARY"});
			break;
		}
		case hud::HoverKind::MiniLibToggle: tip.first = "Library"; lines("LIBRARY", {"MAIN_IFACE_BUTTONS"}); break;
		case hud::HoverKind::Timer: {
			static const char* titles[8] = {"Wonder Victory Timer", "Capital Recapture Timer", "Armageddon Clock", "Territory Victory Timer", "Peace Timer", "Time Limit", "Barbarian Timer", "Musical Chairs"};
			static const char* keys[8] = {"WONDERTIMER", "CAPITALTIMER", "ARMAGEDDONCLOCK", "TERRITORYTIMER", "", "", "", ""};
			const int i = std::clamp(h.a, 0, 7);
			tip.first = titles[i];
			if (keys[i][0]) lines(keys[i], {"STATS"});
			break;
		}
		case hud::HoverKind::Clock:
			tip.first = "Game Clock";
			lines("PAUSE", {"MAIN_IFACE_BUTTONS"});
			break;
		case hud::HoverKind::Rare: {
			// A rare good of the column (spot id good | 0x80000000): its name and rules description.
			const ron::sim::Type& T = _game->type(h.a);
			tip.first = [self typeName:h.a];
			if (!T.desc.empty()) tip.info.push_back(ron::sim::to_utf8(T.desc));
			break;
		}
		case hud::HoverKind::LibraryCell: {
			// A library cell: the tech or age with its price and the help.xml text, as the card's research button.
			const ron::sim::Type& T = _game->type(h.a);
			tip.first = [self typeName:h.a];
			for (int g = 0; g < 6; g++) {
				const int c = ron::sim::get_cost(*_game, h.a, g, me, -1, -1, 0, 0, -1);
				if (c > 0) tip.cost.push_back({g, c, lv.stockpile[g] >= c});
			}
			const std::string key = up_str(ron::sim::to_utf8(T.help_name.empty() ? T.name : T.help_name));
			lines(key, {"LIBRARY", "SPELLS", "BUILDINGS", "UNITS"});
			if (tip.info.empty() && !T.desc.empty()) tip.info.push_back(ron::sim::to_utf8(T.desc));
			break;
		}
		case hud::HoverKind::Garrison: {
			if (size_t(h.a) >= s.garrison.units.size()) break;
			const hud::GarrisonUnit& gu = s.garrison.units[size_t(h.a)];
			tip.first = [self typeName:gu.type] + (gu.count > 1 ? " x" + std::to_string(gu.count) : std::string());
			tip.info.push_back(uistr::tr("Click to eject."));
			lines("EJECT", {"EJECTS", "ORDERS"});
			break;
		}
		case hud::HoverKind::AirButton: case hud::HoverKind::AirHeader: {
			// IFaceAirGarrison::on_button_move (options 0x27..0x29: select fighters / bombers / helicopters) and on_spot_move (the
			// ready / fuelling headers): the option's name, help.xml's AIR entries.
			const bool header = h.kind == hud::HoverKind::AirHeader;
			const int fam = std::clamp(header ? 0 : h.a, 0, 2), col = header ? h.a : h.b;
			if (!header) {
				tip.first = _hud.option_name(uix::kOptFighters + fam);
				static const char* const keys[3] = {"AIRFIGHTERS", "AIRBOMBERS", "AIRHELIS"};
				static const char* const hk[3] = {"OPTION_FIGHTERS", "OPTION_BOMBERS", "OPTION_HELIS"};
				lines(keys[fam], {"AIR"});
				tip.hotkey = [self hotkeyFor:hk[fam] param:-2];
			} else {
				tip.first = uistr::tr(col == 0 ? "Ready" : "Fueling");
			}
			lines(col == 0 ? "AIRREADY" : "AIRFUELING", {"AIR"});
			break;
		}
		case hud::HoverKind::TypeGroup: {
			// A counted type button of the selection panel: Click keeps only the type, Shift as well, Ctrl removes it.
			tip.first = [self typeName:h.a];
			const std::string d1 = _hud.option_desc(167), d2 = _hud.option_desc(174);  // OPTION_OBJECT / "Double clicking selects all objects of this type"
			tip.info.push_back(d1.empty() ? uistr::tr("Click to select only this type, CTRL-Click to remove it.") : d1);
			if (!d2.empty()) tip.info.push_back(d2);
			break;
		}
		case hud::HoverKind::LibraryCat: {
			// OPTION 0xaf "Category of research": the next age / tech of the line.
			const ron::sim::Leader& L = _game->leaders[size_t(me & 7)];
			static const int base[4] = {572, 565, 558, 551};
			const int type = h.a == 0 ? 544 + L.age : base[std::clamp(h.a - 1, 0, 3)] + L.epoch[size_t(std::clamp(h.a - 1, 0, 3))];
			if (type < 0 || type > 628 || !_game->type(type).loaded) break;
			tip.first = [self typeName:type];
			for (int g = 0; g < 6; g++) {
				const int c = ron::sim::get_cost(*_game, type, g, me, -1, -1, 0, 0, -1);
				if (c > 0) tip.cost.push_back({g, c, lv.stockpile[g] >= c});
			}
			const ron::sim::Type& T = _game->type(type);
			lines(up_str(ron::sim::to_utf8(T.help_name.empty() ? T.name : T.help_name)), {"LIBRARY", "SPELLS", "BUILDINGS", "UNITS"});
			break;
		}
		case hud::HoverKind::LibraryFinal: {
			const int type = 579 + std::clamp(h.a, 0, 3);
			if (!_game->type(type).loaded) break;
			const ron::sim::Type& T = _game->type(type);
			tip.first = [self typeName:type];
			for (int g = 0; g < 6; g++) {
				const int c = ron::sim::get_cost(*_game, type, g, me, -1, -1, 0, 0, -1);
				if (c > 0) tip.cost.push_back({g, c, lv.stockpile[g] >= c});
			}
			lines(up_str(ron::sim::to_utf8(T.help_name.empty() ? T.name : T.help_name)), {"LIBRARY", "SPELLS", "BUILDINGS", "UNITS"});
			if (tip.info.empty() && !T.desc.empty()) tip.info.push_back(ron::sim::to_utf8(T.desc));
			break;
		}
		case hud::HoverKind::Progress: {
			if (size_t(h.a) >= s.progress.size()) break;
			const hud::ProgressItem& p = s.progress[size_t(h.a)];
			tip.first = p.name + " " + std::to_string(p.percent) + "%";
			tip.info.push_back(uistr::tr("Click to centre on the producer."));
			break;
		}
		default: break;
	}
	tip.first = uistr::tr(tip.first);
	for (std::string& l : tip.info) {
		if (l.find("{*") == std::string::npos && l.find('#') == std::string::npos) l = uistr::tr(l);
	}
	return tip;
}

- (void)encodeHud:(id<MTLRenderCommandEncoder>)enc width:(NSUInteger)w height:(NSUInteger)h {
	if (!_uiReady) return;
	const float sc = _uiScale > 0 ? _uiScale : 1;
	_uiW = int(std::lround(double(w) / sc));
	_uiH = int(std::lround(double(h) / sc));
	if (!_hudOn) {  // H: no interface, but the game menu still shows
		if (!_gameMenu) return;
		_ui.begin(_uiW, _uiH, sc);
		[self drawGameMenu];
		_ui.encode(enc);
		return;
	}
	[self updateMinimapFog];
	_hudState = [self makeHudState];
	_ui.begin(_uiW, _uiH, sc);
	_hud.layout(_uiW, _uiH);
	if (![self hudVisible]) {  // ui_hide (or H): only the script layers
		if (_sviewOn) _sview.draw(_ui, [self pres], [self scriptEnv]);
		[self drawGameMenu];
		_ui.encode(enc);
		return;
	}
	if (_formDrag) {
		// The formation preview: a ring in the owner's colour at every slot, and a line from the press point along the facing.
		const uint8_t* tc = kUnitTeamColors[std::clamp(_game ? _game->leaders[size_t([self localPlayer] & 7)].color : 0, 0, 8)];
		const uint32_t col = 0xFF000000u | uint32_t(tc[0]) << 16 | uint32_t(tc[1]) << 8 | uint32_t(tc[2]);
		auto at = [&](simd_float2 w) {
			return [self screenOf:simd_make_float3(w.x, w.y, std::max(0.0f, _terrain.sample_height(w.x, w.y)))] / _uiScale;
		};
		for (const simd_float2& slot : [self formationSlots]) {
			// A circle on the ground: a 2:1 ellipse of eight short strokes.
			const simd_float2 u = at(slot);
			_ui.fill(u.x - 3, u.y - 4, 6, 1, col, ron::metal::UiType::Normal), _ui.fill(u.x - 3, u.y + 3, 6, 1, col, ron::metal::UiType::Normal);
			_ui.fill(u.x - 6, u.y - 2, 1, 4, col, ron::metal::UiType::Normal), _ui.fill(u.x + 5, u.y - 2, 1, 4, col, ron::metal::UiType::Normal);
			_ui.fill(u.x - 5, u.y - 3, 2, 1, col, ron::metal::UiType::Normal), _ui.fill(u.x + 3, u.y - 3, 2, 1, col, ron::metal::UiType::Normal);
			_ui.fill(u.x - 5, u.y + 2, 2, 1, col, ron::metal::UiType::Normal), _ui.fill(u.x + 3, u.y + 2, 2, 1, col, ron::metal::UiType::Normal);
		}
		const simd_float2 a = at(_formStart), b = at(_formEnd);
		const float seg = std::max(1.0f, simd_length(b - a));
		for (float t = 0; t < seg; t += 6) {  // the drag: a dotted line from the press point, ending in a larger dot
			const simd_float2 q = a + (b - a) * (t / seg);
			_ui.fill(q.x - 1, q.y - 1, 2, 2, 0xFFFFFFFF, ron::metal::UiType::Normal);
		}
		_ui.fill(b.x - 2, b.y - 2, 4, 4, 0xFFFFFFFF, ron::metal::UiType::Normal);
	}
	if (_boxing) {
		// The drag box (post-tonemap, Scene::render step 10): a white frame.
		const CGFloat x0 = std::min(_press.x, _mouse.x), y0 = std::min(_press.y, _mouse.y);
		_ui.frame(float(std::floor(x0)), float(std::floor(y0)), float(std::fabs(_mouse.x - _press.x)), float(std::fabs(_mouse.y - _press.y)), 0xFFFFFFFF);
	}
	_hud.draw(_ui, _hudState);
	[self drawGroupPanel];
	// Mouse-over help: shown once the mouse has rested on the same element for a few ticks (MouseOver::describe: 3 ticks),
	// not while the scene scrolls or a modal window is open.
	if (_helpOn && _mouseInside && !_boxing && _placeType < 0 && !_pressing && !(_sviewOn && _sview.modal()) && !_gameMenu) {
		const hud::Hover hv = _hud.hover_at(int(_mouse.x), int(_mouse.y), _hudState);
		const int tick = int(_timeMs / 66.0);
		if (hv != _hover) _hover = hv, _hoverTick = tick;
		if (hv.kind != hud::HoverKind::None && (_hoverForce || tick - _hoverTick >= 3)) {
			const tips::Tip tip = [self tipFor:hv state:_hudState];
			tips::draw(_ui, _hud, _help, tip, _verbosity, _hud.bar_top() - 4);
		}
	}
	// MessageWin, AnimNotice, ScreenArrowSpline, the modal windows (docs/ui-port.md "Script UI").
	if (_sviewOn) _sview.draw(_ui, [self pres], [self scriptEnv]);
	[self drawMpStatus];
	[self drawPlaybackBar];
	[self drawGroupNameDialog];
	[self drawGameMenu];
	_ui.encode(enc);
	[self syncProfile];
}

// ---------------------------------------------------------------- save / load (engine/sim/save.hpp, docs/save-format.md)

// The one save slot: ~/Library/Application Support/OpenRiseOfNations/Saves/quicksave.ronsave. (The front end has no text
// entry, so named saves wait for one.)
- (NSString*)quickSavePath {
	NSArray* dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
	NSString* base = dirs.count ? dirs[0] : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support"];
	NSString* dir = [base stringByAppendingPathComponent:@"OpenRiseOfNations/Saves"];
	[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
	return [dir stringByAppendingPathComponent:@"quicksave.ronsave"];
}

// A translated_strings.xml entry by index (the decompiled call sites address them by offset / 20), English text as the fallback
// only when the table is missing.
- (std::string)locString:(int)i fallback:(const char*)fallback {
	if (_tables && i >= 0 && size_t(i) < _tables->translated().size() && !_tables->translated()[size_t(i)].empty()) return _tables->translated()[size_t(i)];
	return fallback;
}

// The game speed word of the clock ("Normal"): rules.xml `gamespeeds`, localised through string_lookup.
- (std::string)speedName {
	std::string n = "Normal";
	if (_game && _game->rules) {
		if (const ron::sim::CategoryList* cl = _game->rules->category_list("game_speeds"); cl && size_t(_speed) < cl->items.size())
			n = ron::sim::to_utf8(cl->items[size_t(_speed)].name);
	}
	return uistr::tr(n);
}

- (void)say:(const std::string&)text {
	std::printf("%s\n", text.c_str());
	if (_sviewOn) _sview.say(text, [self scriptEnv]);
}

- (BOOL)saveGameTo:(NSString*)path {
	if (!_game || _inPopup || _pbv) return NO;
	try {
		ron::sim::save_game(*_game, ron::sim::scenario_rel_path(*_files, _scenarioPath), _simSeed, path.UTF8String);
		[self say:"Game saved"];
		return YES;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "save failed: %s\n", e.what());
		[self say:"Could not save the game"];
		return NO;
	}
}

// Replaces the simulation with the saved one (the same scenario only: the terrain, trees and art the renderer built stay).
- (BOOL)loadGameFrom:(NSString*)path {
	if (!_game || !_simRules || _inPopup || _pbv) return NO;   // a recording's game belongs to the recording
	try {
		const ron::sim::SaveInfo head = ron::sim::read_save_info(path.UTF8String);
		auto key = [&](const std::string& s) {
			const std::string r = _files->resolve(s);
			return ron::to_lower(r.empty() ? s : r);
		};
		// _scenarioPath is "ctw:battle" for a generated Conquer the World battle; the save keeps what scenario_rel_path made of it (the battle's setup text).
		if (key(head.scenario) != key(ron::sim::scenario_rel_path(*_files, _scenarioPath))) {
			std::fprintf(stderr, "load: the save is for %s, this game is %s\n", head.scenario.c_str(), _scenarioPath.c_str());
			[self say:"That save belongs to another scenario"];
			return NO;
		}
		ron::sim::SaveInfo info;
		std::unique_ptr<ron::sim::Game> g = ron::sim::load_game(*_files, *_simRules, _art.get(), path.UTF8String, &info);
		for (const std::string& e : info.script_errors) std::fprintf(stderr, "script: %s\n", e.c_str());
		_game = std::move(g);
		[self afterGameReplaced];
		[self say:"Game loaded"];
		return YES;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "load failed: %s\n", e.what());
		[self say:"Could not load the game"];
		return NO;
	}
}

// Everything the renderer derived from the simulation that a replaced Game (a loaded save, an out-of-sync recovery) invalidates.
- (void)afterGameReplaced {
	_cm.reset();
	_hotkeys.clear();
	_selected.clear(), _selectedBuilds.clear();
	_targeting = -1, _placeType = -1, _buildPage = -1, _formDrag = NO;
	_ghostValid = NO;
	_clipClocks.clear();
	_curView = ron::sim::make_view(*_game);
	_prevView = _curView;
	[self indexPrevView];
	[self syncSimBuildings];
	_fogUploaded = NO;
	_gameEventsSeen = _game->events.size();
	_notifier.reset(*_game);
	_leaderOptsSent = NO;
	_terrFrame = -100000;
	const ron::sim::ScriptPresentation* p = [self pres];
	_logSeq = p && p->next_seq > 0 ? p->next_seq - 1 : 0;
	_logObjectives = p ? p->objectives.size() : 0;
	_logDone.clear();
	if (p) {
		for (const ron::sim::ScriptObjective& o : p->objectives) {
			if (o.completed) _logDone.push_back({o.id, o.frame});
		}
	}
	if (_sviewOn) _sview.reset(p);
	if (_audio) _audio->stop_voice();
	_hiliteOverride = -1;
	_pan = CameraPan();
	_overMs = -1, _overOffered = NO;
	_simMs = 0;
	_lastFrame = 0;
}

- (BOOL)quickSave {
	return [self saveGameTo:[self quickSavePath]];
}

- (BOOL)quickLoad {
	NSString* path = [self quickSavePath];
	if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
		[self say:"No saved game"];
		return NO;
	}
	return [self loadGameFrom:path];
}

// ---------------------------------------------------------------- the game menu (docs/ui-port.md "Front end")

- (std::vector<std::string>)gameMenuItems {
	if (_gameMenu == 2) return _endgame.items();  // Examine Map, Game Stats, View Playback, Quit (EndGameWin)
	if (_pbv) return {"Resume Playback", "Restart", "Options", "Return to Main Menu", "Quit"};
	if (_mp) return {"Resume Game", "Save Game", "Resign", "Options", "Return to Main Menu", "Quit"};  // a multiplayer save is synchronized (no loading mid-game)
	return {"Resume Game", "Save Game", "Load Game", "Options", "Return to Main Menu", "Quit"};
}

- (void)openGameMenu:(int)kind {
	if (!_uiReady) return;
	_gameMenu = kind;
	_gmHover = 0, _gmPress = -1;
	_pressing = _boxing = _hudPress = _rightPress = NO;
	if (kind == 2 && _game) [self openEndGame];
	if (kind == 3) [self openOptions];  // harness: RON_GAME_MENU=3 shows the Options page
}

// The file the game in this window is being recorded into (a multiplayer game; the Recorder flushes every package, so it can be read at once), or nil.
- (NSString*)recordingPath {
	if (!_mp || _mp->record_path().empty()) return nil;
	NSString* path = @(_mp->record_path().c_str());
	return [[NSFileManager defaultManager] fileExistsAtPath:path] ? path : nil;
}

// EndGameWin::exec: the window for the decided game. Return answers Quit (EndGameWin::on_key_click), Esc Examine Map.
- (void)openEndGame {
	endgame::Names names;
	names.leader = [self](int who) {
		const ron::sim::Leader& L = _game->leaders[size_t(who)];
		if (!L.name.empty()) return L.name;
		return _simRules && L.tribe >= 0 && size_t(L.tribe) < _simRules->tribes.size() ? ron::sim::to_utf8(_simRules->tribes[size_t(L.tribe)].name) : std::string();
	};
	names.age = [self](int n) { return n >= 1 && n <= 7 ? [self typeName:543 + n] : std::string("Ancient Age"); };  // as the age line of the top bar
	const ron::sim::ScriptPresentation* p = [self pres];
	_endgame.open(endgame::make_info(*_game, [self localPlayer], _tables.get(), names, p ? p->victory_message : std::string(), p ? p->defeat_message : std::string()));
	const endgame::Info& in = _endgame.info();
	std::printf("frame %d: end-game window: %s (%s) \"%s\"; game time %d:%02d, score %d\n", _game->frame, in.mode == 0 ? "victory" : in.mode == 1 ? "defeat" : "armageddon", in.kind.c_str(),
	            in.message.c_str(), in.frames / 15 / 60, in.frames / 15 % 60, in.score);
	if (_scriptLog) {
		for (int i = 0; i < 8; i++) {
			const ron::sim::Leader& L = _game->leaders[size_t(i)];
			if (L.flags & ron::sim::leaderflag::kValid)
				std::printf("  leader %d: flags 0x%x/0x%x victory type %d, defeat type %d, score %d\n", i, L.flags, L.flags2, L.victory_type, L.defeat_type, L.score);
		}
	}
	_gmHover = 3;
	_overOffered = YES;
	_endgame.set_playback(!_pbv && _mp && self.onPlayRecording && [self recordingPath] != nil);   // View Playback: a game that was recorded
	if (_sviewOn) _sview.clear_notices();
	if (getenv("RON_ENDGAME_STATS")) _endgame.show_stats(true);  // snapshots: the score table
}

// MainMenuBase (interface.xml MAINMENU): the 9-piece scroll background bg0..bg8, the title in font 24, a column of
// 173 x 32 buttons (butnormal / buthighlight / butdown), centred on the screen.
- (void)drawGameMenu {
	_gmRects.clear();
	if (!_gameMenu || !_layout) return;
	if (_gameMenu == 3) {
		[self drawOptions];
		return;
	}
	if (_gameMenu == 2) {
		_endgame.draw(_ui, _uiW, _uiH, _gmHover, _gmPress, _gmRects);
		return;
	}
	const ron::XmlElement* mm = nullptr;
	for (const ron::XmlElement& c : _layout->root().children) {
		if (ron::iface_tag_is(c, "MAINMENU")) mm = &c;
	}
	const std::vector<std::string> items = [self gameMenuItems];
	const int bw = 173, bh = 32, gap = 8, pad = 22, titleH = 44;
	const int w = bw + pad * 2, h = titleH + int(items.size()) * (bh + gap) - gap + pad * 2;
	const int x = (_uiW - w) / 2, y = (_uiH - h) / 2 - 40;
	_ui.next_window();
	_ui.fill(0, 0, float(_uiW), float(_uiH), rm::argb(90, 0, 0, 0), rm::UiType::ColorBlend);
	_ui.next_layer();
	ron::IfacePiece bg[9];
	if (mm) {
		for (int k = 0; k < 9; k++) bg[k] = ron::iface_find_piece(*mm, "bg" + std::to_string(k));
	}
	if (bg[0].valid() && bg[4].valid() && bg[8].valid()) {
		const int lw = bg[0].width, rw = bg[2].width, th = bg[0].height, bh2 = bg[6].height;
		const int cx[3] = {x, x + lw, x + w - rw}, cw[3] = {lw, w - lw - rw, rw};
		const int cy[3] = {y, y + th, y + h - bh2}, ch[3] = {th, h - th - bh2, bh2};
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) _ui.add(bg[r * 3 + c], float(cx[c]), float(cy[r]), float(cw[c]), float(ch[r]));
		}
	} else {
		_ui.fill(float(x), float(y), float(w), float(h), rm::argb(224, 39, 40, 43), rm::UiType::ColorBlend);
	}
	_ui.next_layer();
	const int tf = _ui.font(24, -1), bf = _ui.font(14, -1);
	_ui.text_centered(tf, _gameMenu == 2 ? "Game Over" : "Game Menu", {x, x + w, y + pad - 6, y + pad - 6 + titleH}, 0xFFFFFFFF, 0xFF000000);
	for (size_t i = 0; i < items.size(); i++) {
		const int by = y + pad + titleH + int(i) * (bh + gap);
		const ron::IfaceRect r{x + pad, x + pad + bw, by, by + bh};
		_gmRects.push_back(r);
		const char* kind = _gmPress == int(i) ? "butdown" : _gmHover == int(i) ? "buthighlight" : "butnormal";
		ron::IfacePiece p[3];
		if (mm) {
			for (int k = 0; k < 3; k++) p[k] = ron::iface_find_piece(*mm, std::string(kind) + std::to_string(k));
		}
		_ui.next_layer();
		if (p[0].valid()) _ui.expander(p[0], p[1], p[2], r);
		else _ui.fill(float(r.l), float(r.t), float(r.w()), float(r.h()), rm::argb(255, 70, 70, 70));
		_ui.next_layer();
		_ui.text_centered(bf, items[i], r, 0xFFFFFFFF, 0xFF000000);
	}
}

- (int)gameMenuAt:(CGPoint)p {
	for (size_t i = 0; i < _gmRects.size(); i++) {
		if (_gmRects[i].contains(int(p.x), int(p.y))) return int(i);
	}
	return -1;
}

- (void)gameMenuChoose:(int)i {
	const std::vector<std::string> items = [self gameMenuItems];
	if (i < 0 || size_t(i) >= items.size()) return;
	if (_gameMenu == 2) {  // EndGameWin::on_button_clicked
		using A = endgame::Screen::Action;
		if (!_endgame.enabled(i)) return;
		const A a = _endgame.action(i);
		if (a == A::Stats || a == A::Close) {
			_endgame.show_stats(a == A::Stats);
			_gmHover = _gmPress = -1;
			return;
		}
		if (a == A::Playback) {  // the recording of the game that just ended, from its first frame
			NSString* path = [self recordingPath];
			if (path && self.onPlayRecording) {
				_gameMenu = 0;
				self.onPlayRecording(path);
			}
			return;
		}
		if (a != A::Examine && a != A::Quit && a != A::Continue) return;
		_gameMenu = 0;
		_lastFrame = 0;
		if (a == A::Examine) return;  // the game stays on screen to be looked over
		if (_ctwBuild && self.onCtwEnd) {  // leaving a campaign battle
			auto cb = self.onCtwEnd;
			self.onCtwEnd = nil;
			cb(_ctwOutcome >= 0 ? _ctwOutcome : 2);
		} else if (self.onExit) {
			self.onExit(NO);
		}
		return;
	}
	const std::string& it = items[size_t(i)];
	if (it == "Options") {
		[self openOptions];
		return;
	}
	_gameMenu = 0;
	_lastFrame = 0;  // no catch-up after the pause
	if (it == "Restart" && _pbv) {
		[self pbSeek:_pbv->pb->start_frame()];
		_paused = NO;
		return;
	}
	if (it == "Resign") {
		if (_mp) _mp->resign();
		return;
	}
	if (it == "Save Game" && _mp) {
		_mp->request_save();
		return;
	}
	if (it == "Save Game") [self quickSave];
	else if (it == "Load Game") [self quickLoad];
	else if (it == "Return to Main Menu" && _ctwBuild && self.onCtwEnd) {  // leaving a campaign battle: it is lost
		auto cb = self.onCtwEnd;
		self.onCtwEnd = nil;
		cb(_ctwOutcome >= 0 ? _ctwOutcome : 2);
	}
	else if (it == "Return to Main Menu" && self.onExit) self.onExit(NO);
	else if (it == "Quit") {
		if (self.onExit) self.onExit(YES);
		else [NSApp terminate:nil];
	}
}

- (void)gameMenuKey:(NSEvent*)e {
	NSString* chars = e.charactersIgnoringModifiers;
	const unichar c = chars.length ? [chars characterAtIndex:0] : 0;
	if (_gameMenu == 3) {
		if (_optPage) {
			const NSEventModifierFlags m = e.modifierFlags;
			_optPage->key(e.keyCode, c, (m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0, (m & NSEventModifierFlagOption) != 0,
			              (m & NSEventModifierFlagShift) != 0);
		}
		return;
	}
	const int n = int([self gameMenuItems].size());
	if (c == NSUpArrowFunctionKey) _gmHover = (std::max(_gmHover, 0) + n - 1) % n;
	else if (c == NSDownArrowFunctionKey) _gmHover = (_gmHover + 1) % n;
	else if (c == '\r' || c == 3) [self gameMenuChoose:std::max(_gmHover, 0)];
	else if ((e.keyCode == 53 || e.keyCode == 109) && _gameMenu == 2 && _endgame.stats_view()) _endgame.show_stats(false);
	else if (e.keyCode == 53 || e.keyCode == 109) _gameMenu = 0, _lastFrame = 0;  // Esc / F10: back to the game
}

// ---------------------------------------------------------------- the Options page (docs/ui-port.md section 15)

// Steps the game speed to `target` (0..3) the way the keys do: SPEED_UP / SPEED_DOWN commands, so a recording and the other machines
// follow. A multiplayer game keeps the host's speed (the session owns it).
- (void)stepGameSpeedTo:(int)target {
	if (!_game || _mp || _pbv) return;
	target = std::clamp(target, 0, 3);
	for (int guard = 0; guard < 6 && _speed != target; guard++) {
		_game->ctl.speed = _speed, _game->ctl.target_frame_time = int32_t(kTimings[std::clamp(_speed, 0, 4)]);
		if (target > _speed) _cm.issue_speed_up();
		else _cm.issue_speed_down();
		_cm.process_turn(*_game);
		if (_game->ctl.speed == _speed) break;
		_speed = _game->ctl.speed;
	}
}

// LEADER_OPTIONS (OptionsWinGame::do_save -> CommandManager::issue_leader_options): the Auto Citizen stance and delay, the default
// stance of buildings and the transport / deploy option flags go to the simulation as a command, so a recording and the other
// machines follow. Only what differs from the leader's current options is sent.
- (void)issueLeaderOptions:(const prefs::Prefs&)p {
	const int me = [self localPlayer];
	if (!_game || me < 0 || me >= int(_game->leaders.size())) return;
	const ron::sim::Leader& L = _game->leaders[size_t(me)];
	const int32_t peasants = p.has("auto_citizen") ? p.auto_citizen : L.opt_peasants;
	const int32_t wait = p.has("auto_citizen_delay") ? p.auto_citizen_delay : L.opt_peasants_wait;
	const int32_t buildings = p.has("building_stance") ? p.building_stance : L.opt_buildings;
	uint32_t flags = L.options_flags;
	if (p.has("auto_transport")) flags = (flags & ~2u) | (p.auto_transport ? 2u : 0u);  // bit 1: the transport option
	if (p.has("auto_deploy")) flags = (flags & ~8u) | (p.auto_deploy ? 8u : 0u);         // bit 3: packers (siege units) deploy when idle
	if (peasants == L.opt_peasants && wait == L.opt_peasants_wait && buildings == L.opt_buildings && flags == L.options_flags) return;
	if (getenv("RON_UIX_TRACE")) std::fprintf(stderr, "[opts] leader options peasants=%d wait=%d buildings=%d flags=0x%x\n", peasants, wait, buildings, flags);
	[self issueAs:me block:^bool { return _cm.issue_leader_options(peasants, wait, buildings, flags); }];
	if (getenv("RON_UIX_TRACE")) std::fprintf(stderr, "[opts] f%d now peasants=%d wait=%d buildings=%d flags=0x%x\n", _game->frame, L.opt_peasants, L.opt_peasants_wait, L.opt_buildings, L.options_flags);
}

// What OK hands to the window: the preferences that act on a running game (the rest are read when a game or the menu starts).
- (void)applyPrefs:(const prefs::Prefs&)p {
	if (p.has("game_speed") && p.game_speed != _speed && !(p.game_speed >= 3 && _speed >= 3)) [self stepGameSpeedTo:p.game_speed];
	if (p.has("help_detail")) {
		_helpOn = p.help_detail > 0;
		if (p.help_detail > 0) _verbosity = std::clamp(p.help_detail, 1, 3);
	}
	if (p.has("advanced_menu") && _hud.advanced() != p.advanced_menu) {
		_hud.set_advanced(p.advanced_menu);
		[[NSUserDefaults standardUserDefaults] setBool:p.advanced_menu forKey:@"AdvancedMenu"];
	}
	if (p.has("progress_bars")) [self setProgressButtons:p.progress_bars];
	if (p.has("message_length")) scriptview::set_message_length(p.message_length);
	[self issueLeaderOptions:p];
	if (p.has("prefer_military")) _hud.set_prefer_military(p.prefer_military);
	if (p.has("streamlined") && _hud.opened() == p.streamlined) _hud.set_opened(!p.streamlined);
	if (_audio && (p.has("fx_volume") || p.has("music_volume") || p.has("music_in_game"))) {
		const float fx = float(p.fx_volume) / 255.0f;
		_audio->set_volumes(fx, fx, p.music_in_game ? float(p.music_volume) / 255.0f : 0.0f);
	}
	NSWindow* win = self.view.window;
	if (win && p.has("fullscreen") && ((win.styleMask & NSWindowStyleMaskFullScreen) != 0) != p.fullscreen) [win toggleFullScreen:nil];
}

// Harness (RON_CARRIER_TEST=WHO:O, docs/ui-port.md 14.1): the unit becomes a Carrier (type 351) so the card's REPEAT can be looked at.
- (void)harnessCarrier:(const char*)spec {
	int who = 0, o = 0;
	std::sscanf(spec, "%d:%d", &who, &o);
	if (!_game) return;
	if (ron::sim::Unit* u = _game->unit(who, o)) u->type = ron::sim::ptype::kCarrier, u->ptype = &_game->type(ron::sim::ptype::kCarrier);
}

- (void)openOptions {
	if (!_uiReady || !_layout || !_files) return;
	if (!_optPage) _optPage = std::make_unique<optui::Page>(*_files, *_layout);
	if (!_optPage->ready()) {
		[self say:"The options window (Data/optionswin.xml) could not be read"];
		return;
	}
	optui::Host h;
	__unsafe_unretained MapRenderer* me = self;
	h.live = [me](prefs::Prefs& p) {  // the running game's own values win over the saved ones
		p.game_speed = std::clamp(me->_speed, 0, 4);
		p.help_detail = me->_helpOn ? me->_verbosity : 0;
		p.advanced_menu = me->_hud.advanced();
		p.progress_bars = me->_progressButtons;
		p.prefer_military = me->_hud.prefer_military();
		if (me->_game && [me localPlayer] >= 0 && [me localPlayer] < int(me->_game->leaders.size())) {  // the leader's own options win
			const ron::sim::Leader& L = me->_game->leaders[size_t([me localPlayer])];
			p.auto_citizen = std::clamp(int(L.opt_peasants), 0, 3), p.auto_citizen_delay = std::clamp(int(L.opt_peasants_wait), 0, 5);
			p.building_stance = std::clamp(int(L.opt_buildings), 0, 5);
			p.auto_transport = (L.options_flags & 2u) != 0, p.auto_deploy = (L.options_flags & 8u) != 0;
		}
		p.streamlined = !me->_hud.opened();
		if (NSWindow* w = me.view.window) p.fullscreen = (w.styleMask & NSWindowStyleMaskFullScreen) != 0;
	};
	h.apply = [me](const prefs::Prefs& p) { [me applyPrefs:p]; };
	h.preview = [me](const prefs::Prefs& p) {
		if (me->_audio) me->_audio->set_volumes(float(p.fx_volume) / 255.0f, float(p.fx_volume) / 255.0f, p.music_in_game ? float(p.music_volume) / 255.0f : 0.0f);
	};
	h.type_name = [me](int t) -> std::string { return me->_game && t >= 0 && t < ron::sim::kNumTypes ? [me typeName:t] : std::string(); };
	h.resolution = [me]() { return std::to_string(me->_uiW) + " x " + std::to_string(me->_uiH); };
	h.closed = [me]() {
		me->_gameMenu = 1;  // back to the game menu
		me->_gmHover = 0, me->_gmPress = -1;
	};
	int pane = 0;
	if (const char* e = getenv("RON_OPT_PANE")) pane = std::atoi(e);  // snapshot checks
	_gameMenu = 3;
	_optPage->open(h, pane);
	if (const char* e = getenv("RON_OPT_HK")) _optPage->test_select_hotkey(e);
	if (const char* e = getenv("RON_OPT_COMBO")) _optPage->test_open_combo(std::atoi(e));
}

- (void)drawOptions {
	if (!_optPage || !_optPage->is_open()) return;
	_ui.next_window();
	_ui.fill(0, 0, float(_uiW), float(_uiH), rm::argb(120, 0, 0, 0), rm::UiType::ColorBlend);
	_ui.next_layer();
	_optPage->draw(_ui, (_uiW - optui::Page::kW) / 2, (_uiH - optui::Page::kH) / 2);
}

// ---------------------------------------------------------------- building placement (docs/ui-spec.md 4.5)

- (void)startPlacement:(int)type {
	_placeType = type;
	_ghostValid = NO;
	_targeting = -1;
}

- (void)previewPlacementAtWorldX:(float)x y:(float)y {
	if (!_game || _placeType < 0) return;
	const int me = [self localPlayer];
	int32_t sx = int32_t(x), sy = int32_t(y);
	// BuildTypeData::snap_center + blocked_site: the same validation Group::action_build runs (no RNG draws).
	ron::sim::snap_center_who(*_game, _placeType, int32_t(x), int32_t(y), &sx, &sy, me);
	_ghostBlocked = ron::sim::blocked_site(*_game, _placeType, sx, sy, me, -1, nullptr);
	_ghostX = float(sx), _ghostY = float(sy);
	_ghostValid = YES;
	if (getenv("RON_PLACE_DEBUG")) std::fprintf(stderr, "place %d at (%.0f,%.0f) -> snapped (%d,%d) size %dx%d blocked %d\n", _placeType, x, y, sx, sy,
	                                         _game->type(_placeType).x_size, _game->type(_placeType).y_size, _ghostBlocked);
}

// The footprint (BuildTypeOut::draw_template): Template Blend tiles, green checkerboard when the site is valid, orange
// for the soft blocks (city / fort distance, one-per-city, farm, wonder), red otherwise.
- (void)encodeFootprint:(id<MTLRenderCommandEncoder>)enc {
	if (!_game || _placeType < 0 || !_ghostValid) return;
	const ron::sim::Type& T = _game->type(_placeType);
	const int xs = std::max(1, T.x_size), ys = std::max(1, T.y_size);
	const int tx0 = int(_ghostX) / 192 - (xs >> 1), ty0 = int(_ghostY) / 192 - (ys >> 1);
	namespace bk = ron::sim::block;
	const int32_t c = _ghostBlocked;
	const bool soft = c == bk::kOne || c == bk::kOneOther || c == bk::kFarm || c == bk::kCityDistance || c == bk::kFortDistance ||
	                  c == bk::kFortCityDistance || c == bk::kWonder;
	std::vector<ron::ColorVertex> v;
	for (int ty = ty0; ty < ty0 + ys; ty++) {
		for (int tx = tx0; tx < tx0 + xs; tx++) {
			uint8_t r = 0, g = ((tx + ty) & 1) ? 200 : 255, b = 0;
			if (c != 0) r = 255, g = soft ? 140 : 0;
			auto p = [&](int ix, int iy) {
				const float x = float(ix * 192), y = float(iy * 192);
				return ron::ColorVertex{x, y, std::max(0.0f, _terrain.sample_height(x, y)) + 6, 0, 0, 1, 0, 0, r, g, b, 0x70};
			};
			const ron::ColorVertex q[6] = {p(tx, ty), p(tx + 1, ty), p(tx, ty + 1), p(tx, ty + 1), p(tx + 1, ty), p(tx + 1, ty + 1)};
			v.insert(v.end(), q, q + 6);
		}
	}
	static const ron::RenderState& tmpl = *ron::find_render_state("Template Blend");
	rm::DrawUniforms du = rm::draw_uniforms(tmpl, matrix_identity_float4x4);
	_states.bind(enc, tmpl, rm::VertexKind::Colored);
	[enc setCullMode:MTLCullModeNone];
	id<MTLBuffer> vb = [_device newBufferWithBytes:v.data() length:v.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
	[enc setVertexBuffer:vb offset:0 atIndex:0];
	[enc setVertexBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentTexture:_white atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:v.size()];
}

// Options::picked_spot: BUILD with the selected citizens; Ctrl or Shift keeps placing (Ctrl QUEUE_FIRST, Shift QUEUE_LAST).
- (void)placeAt:(CGPoint)p modifiers:(NSEventModifierFlags)m {
	const simd_float2 g = [self groundAt:[self toDrawable:p]];
	[self placeAtWorldX:g.x y:g.y modifiers:m];
}

- (void)placeAtWorldX:(float)wx y:(float)wy modifiers:(NSEventModifierFlags)m {
	if (!_game || _placeType < 0) return;
	const int me = [self localPlayer];
	const simd_float2 g = simd_make_float2(wx, wy);
	[self previewPlacementAtWorldX:wx y:wy];
	std::vector<int32_t> objs;
	for (const auto& [who, o] : _selected) {
		if (who == me) objs.push_back(o);
	}
	const bool ctrl = (m & NSEventModifierFlagControl) != 0, shift = (m & NSEventModifierFlagShift) != 0;
	const ron::sim::QueuePos q = ctrl ? ron::sim::kQueueFirst : shift ? ron::sim::kQueueLast : ron::sim::kQueueNew;
	if (!objs.empty()) {
		_cm.play = me, _cm.play_who = me;
		if (_cm.issue_build(*_game, objs, _placeType, int32_t(g.x), int32_t(g.y), q)) _cm.process_turn(*_game);
		[self stepSimulation:0];
		_curView = ron::sim::make_view(*_game);
		[self syncSimBuildings];
		std::printf("build %d at (%.0f, %.0f) with %zu citizen(s): %s\n", _placeType, g.x, g.y, objs.size(), _ghostBlocked ? "blocked" : "ok");
	}
	if (!ctrl && !shift) _placeType = -1, _buildPage = -1;
}

// ---------------------------------------------------------------- commands from the card

- (void)issueAs:(int)who block:(bool (^)(void))issue {
	if (!_game) return;
	_cm.play = who, _cm.play_who = who;
	if (issue()) _cm.process_turn(*_game);
}

// SoundGlobal::play of a soundglobals.xml KEY (no S_) on the effects channel.
- (void)playGlobal:(const char*)key {
	if (!_audio && !getenv("RON_UIX_TRACE")) return;  // headless runs have no audio; RON_UIX_TRACE=1 still prints the sound that would play
	const std::string f = _sview.sound_file(key);
	if (getenv("RON_UIX_TRACE")) std::fprintf(stderr, "[uix] sound S_%s -> %s\n", key, f.empty() ? "(none)" : f.c_str());
	if (_audio && !f.empty()) _audio->play_effect(f);
}

// What uix.* (the third HUD pass: air, advanced orders, selection rules) needs from this window.
- (uix::Ctx)uixCtx {
	uix::Ctx c;
	c.game = _game.get();
	c.cm = &_cm;
	c.me = [self localPlayer];
	c.process = [self] { _cm.process_turn(*_game); };
	c.units = &_selected, c.builds = &_selectedBuilds, c.targeting = &_targeting;
	c.advanced = _hud.advanced();
	c.say = [self](const std::string& t) { [self say:t]; };
	c.loc = [self](int i, const char* fallback) { return [self locString:i fallback:fallback]; };
	c.look_at = [self](float x, float y) { [self lookAtX:x y:y]; };
	c.sound = [self](const char* key) { [self playGlobal:key]; };  // the card's acknowledgement sounds (docs/ui-port.md section 16)
	c.on_screen = [self](int32_t who, int32_t o, bool building) {
		const float bottom = float(_hud.bar_top());
		auto inside = [&](simd_float2 sp) { return sp.x >= 0 && sp.y >= 0 && sp.x < _uiW && sp.y < (_hudOn ? bottom : _uiH); };
		if (!building) {
			for (const SimUnitDrawn& d : _drawnUnits) {
				if (d.who == who && d.o == o) return inside([self uiPointOfUnit:d]);
			}
			return false;
		}
		for (const PlacedBuilding& b : _placed) {
			if (b.who == who && b.o == o) return inside([self uiPointOfBuilding:b]);
		}
		return false;
	};
	return c;
}

- (std::vector<int32_t>)ownSelectedUnits {
	std::vector<int32_t> v;
	for (const auto& [who, o] : _selected) {
		if (who == [self localPlayer]) v.push_back(o);
	}
	return v;
}

- (std::vector<int32_t>)ownSelectedBuilds {
	std::vector<int32_t> v;
	for (const auto& [who, o] : _selectedBuilds) {
		if (who == [self localPlayer]) v.push_back(o);
	}
	return v;
}

- (void)runOption:(int)option object:(int)object modifiers:(NSEventModifierFlags)m {
	if (!_game) return;
	const int me = [self localPlayer];
	switch (option) {
		case hud::kOptMoveTo: case hud::kOptGarrison: _targeting = option; break;
		case hud::kOptGroup: [self groupKey:[self selectedGroup] >= 0]; break;  // Agrupar: the name dialog; Reagrupar: the group move
		case hud::kOptHalt: {
			const std::vector<int32_t> objs = [self ownSelectedUnits];
			[self issueAs:me block:^bool { return _cm.issue_halt(*_game, objs); }];
			if (!objs.empty()) [self playGlobal:"HALT_ACK"];  // Options::do_halt
			break;
		}
		case hud::kOptBuild: case hud::kOptBuildMilitary: case hud::kOptBuildWonder: _buildPage = option; break;
		case 1000:
			// validate_build: a building the player cannot pay for raises ScenarioData::user_warnings bit 0.
			if (ron::sim::can_pay_cost(*_game, object, me, -1, -1, 1) <= 0) _pendingWarnings |= 1, [self flashLacking:object who:me];
			[self startPlacement:object];
			break;
		case 65: _buildPage = (_buildPage + 1) % 3; break;  // NEXT
		case 66: _buildPage = -1; break;                     // BACK
		case hud::kOptCancel: _buildPage = -1, _placeType = -1, _targeting = -1; break;
		case 57: case 60: {
			// OPTION_BUY / OPTION_SELL of the Market's card: the good in `object`; Shift = x5 (500), Alt = everything.
			const int flags = (m & NSEventModifierFlagOption) ? 2 : (m & NSEventModifierFlagShift) ? 1 : 0;
			const bool buy = option == 57;
			[self issueAs:me block:^bool { return buy ? _cm.issue_buy(object, flags) : _cm.issue_sell(object, flags); }];
			break;
		}
		case hud::kOptEjectAll: {
			// OPTION_EJECT_ALL: EJECTALL for the selected buildings (their garrison comes out).
			const std::vector<int32_t> objs = [self ownSelectedBuilds];
			[self issueAs:me block:^bool { return _cm.issue_eject_all(*_game, objs); }];
			if (!objs.empty()) [self playGlobal:"EJECT_ACK"];  // Options::do_eject_all
			break;
		}
		case hud::kOptBackToWork: {
			// Options::do_back_to_work: EJECTALL with back_to_work set (the citizens inside resume their orders), S_BACK_TO_WORK_ACK.
			const std::vector<int32_t> objs = [self ownSelectedBuilds];
			[self issueAs:me block:^bool { return _cm.issue_eject_all(*_game, objs, true); }];
			if (!objs.empty()) [self playGlobal:"BACK_TO_WORK_ACK"];
			break;
		}
		case hud::kOptTrain: case hud::kOptResearch: {
			// Options::exec: QUEUE_UP (Shift x5 on TRAIN).
			const std::vector<int32_t> objs = [self ownSelectedBuilds];
			const int n = option == hud::kOptTrain && (m & NSEventModifierFlagShift) ? 5 : 1;
			// GroupOut::issue_queue_up: an unaffordable item raises ScenarioData::user_warnings bit 0 (resource_warning).
			if (!objs.empty() && ron::sim::can_pay_cost(*_game, object, me, -1, objs.front(), 1) <= 0) _pendingWarnings |= 1, [self flashLacking:object who:me];
			[self issueAs:me block:^bool { return _cm.issue_queue(*_game, objs, object, n); }];
			break;
		}
		default: {
			// The options of the third pass: stances, formations, patrol, air orders, disband, queue (uix.mm).
			uix::Ctx uc = [self uixCtx];
			uix::Mods um{(m & NSEventModifierFlagShift) != 0, (m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0, (m & NSEventModifierFlagOption) != 0};
			uix::run_option(uc, option, object, um);
			break;
		}
	}
}

// ---------------------------------------------------------------- hotkeys (docs/ui-spec.md 4.2, keymap_table.inc)

- (void)barButton:(int)i {
	static const char* opts[8] = {"OPTION_PAUSE", "OPTION_TOGGLE_ADVANCED", "OPTION_PING", "OPTION_DIPLO", "OPTION_CHAT",
	                              "OPTION_NEXT_DATA", "OPTION_TOGGLE_HELP", "OPTION_MENU"};
	[self execKeyOption:opts[std::clamp(i, 0, 7)] param:-1 modifiers:0];
}

// Own objects for the cycle keys, in (type, slot) order.
- (std::vector<std::pair<int32_t, int32_t>>)cycleList:(const std::string&)what param:(int)param {
	std::vector<std::pair<int32_t, int32_t>> out;
	const int me = [self localPlayer];
	if (what == "building") {
		for (const ron::sim::BuildView& b : _curView.builds) {
			if (b.alive && b.who == me && b.active && b.type == param) out.push_back({b.who, b.o});
		}
		return out;
	}
	for (const ron::sim::UnitView& u : _curView.units) {
		if (!u.alive || u.who != me || !u.on_map) continue;
		const ron::sim::Type& T = _game->type(u.type);
		const bool citizen = u.type == ron::sim::ptype::kPeasants || u.type == ron::sim::ptype::kPeasantsKorean;
		const bool scholar = u.type == ron::sim::ptype::kScholars || u.type == ron::sim::ptype::kScholarsKorean;
		bool ok = false;
		if (what == "military") ok = !citizen && !scholar && T.attack > 0;
		else if (what == "peasant") ok = citizen && u.order == 0;  // idle citizens
		else if (what == "general") ok = u.type == ron::sim::ptype::kGeneral;
		else if (what == "scout") ok = u.type == ron::sim::ptype::kScout;
		else if (what == "spy") ok = u.type == ron::sim::ptype::kSpy;
		if (ok) out.push_back({u.who, u.o});
	}
	return out;
}

// The cycle keys (C, K, ,, ., ...): select the next own object of the kind after the current selection and centre on it
// (Ctrl: without centring, Shift: all of them).
- (void)cycle:(const std::string&)what param:(int)param zoom:(BOOL)zoom all:(BOOL)all building:(BOOL)building {
	const std::vector<std::pair<int32_t, int32_t>> list = [self cycleList:what param:param];
	if (list.empty()) return;
	size_t next = 0;
	const auto& cur = building ? _selectedBuilds : _selected;
	if (!cur.empty()) {
		for (size_t i = 0; i < list.size(); i++) {
			if (list[i] == cur.front()) next = (i + 1) % list.size();
		}
	}
	_selected.clear(), _selectedBuilds.clear();
	auto& dst = building ? _selectedBuilds : _selected;
	if (all) dst = list;
	else dst.push_back(list[next]);
	if (building) [self syncSimBuildings];
	[self selectionChanged];
	if (zoom) {
		const auto k = dst.front();
		if (building) {
			if (const ron::sim::BuildView* b = [self buildView:k.first o:k.second]) [self lookAtX:float(b->x) y:float(b->y)];
		} else if (const ron::sim::UnitView* u = [self unitView:k.first o:k.second]) {
			[self lookAtX:float(u->x) y:float(u->y)];
		}
	}
}

// Options::exec for the keys: 1 handled, 0 a bound key whose action the port does not have yet, -1 not for us (the caller
// goes on with its own keys).
- (int)execKeyOption:(const char*)opt param:(int)param modifiers:(NSEventModifierFlags)m {
	if (!_game) return -1;
	const std::string o = opt;
	const int me = [self localPlayer];
	const ron::sim::ScriptPresentation* sp = [self pres];
	const BOOL shift = (m & NSEventModifierFlagShift) != 0;
	auto own_units = [&] { return !_selected.empty() && _selected.front().first == me; };
	auto own_builds = [&] { return !_selectedBuilds.empty() && _selectedBuilds.front().first == me; };
	if (_pbv && (o == "OPTION_SLOWER" || o == "OPTION_FASTER")) {  // the viewer's own speeds
		[self pbSpeedStep:o == "OPTION_FASTER" ? 1 : -1];
		return 1;
	}
	if (_mp && (o == "OPTION_SLOWER" || o == "OPTION_FASTER")) {  // SPEED_UP / SPEED_DOWN through the session: every machine changes together
		_mp->speed_step(o == "OPTION_FASTER" ? 1 : -1);
		return 1;
	}
	if (o == "OPTION_SLOWER" || o == "OPTION_FASTER") {
		// TurnControl::issue_speed_down / _up: SPEED_DOWN 54 / SPEED_UP 53 are commands (recorded, replayed, networked); the speed
		// they leave in Game::ctl is the one the frame pacing reads. The keys step 0 Very Slow .. 3 Fast (process_speed_up stops
		// there); Hyper Fast comes from SPEED_SET (--speed 4, the profile).
		_game->ctl.speed = _speed, _game->ctl.target_frame_time = int32_t(kTimings[std::clamp(_speed, 0, 4)]);  // the pacing the window already uses
		if (o == "OPTION_FASTER") _cm.issue_speed_up();
		else _cm.issue_speed_down();
		_cm.process_turn(*_game);
		if (_game->ctl.speed != _speed) {
			_speed = _game->ctl.speed;
			[self say:uistr::tr("Game Speed:") + " " + [self speedName]];
		}
		return 1;
	}
	if (o == "OPTION_CHAT" || o == "OPTION_CHAT_ALL" || o == "OPTION_CHAT_ALLIES" || o == "OPTION_CHAT_ENEMIES") {
		_hud.chat_open(o == "OPTION_CHAT_ALLIES" ? 1 : o == "OPTION_CHAT_ENEMIES" ? 2 : 0);  // the chat line (hud.mm); Return sends CHAT
		return 1;
	}
	if (o == "OPTION_MENU") {
		if (!_inPopup) [self openGameMenu:1];
		return 1;
	}
	if (o == "OPTION_PAUSE") {
		if (!(sp && sp->pause_disabled)) [self togglePause];
		return 1;
	}
	if (o == "OPTION_ZOOM_IN" || o == "OPTION_ZOOM_OUT") {
		if (!(sp && (sp->zoom_lock_flags & 1))) _zoom = std::clamp(_zoom + (o == "OPTION_ZOOM_IN" ? 1 : -1), 0, 6);
		return 1;
	}
	if (o == "OPTION_CANCEL") {
		if (_placeType >= 0 || _targeting >= 0 || _buildPage >= 0) {
			_placeType = -1, _targeting = -1, _buildPage = -1;
			return 1;
		}
		return -1;  // Esc with nothing to cancel clears the selection / opens the menu (handleKey)
	}
	if (o == "OPTION_TOGGLE_IFACE_MAIN") return _hud.set_opened(!_hud.opened()), 1;
	if (o == "OPTION_TOGGLE_IFACE_RESOURCES") return _hud.toggle_panel(1), 1;
	if (o == "OPTION_TOGGLE_IFACE_MARKET") return _hud.toggle_panel(0), 1;
	if (o == "OPTION_TOGGLE_IFACE_INFRA") return _hud.toggle_panel(2), 1;
	if (o == "OPTION_TOGGLE_HELP") {  // Help Text Detail: LOW / MEDIUM / HIGH
		_verbosity = param >= 1 && param <= 3 ? param : _verbosity % 3 + 1;
		// Options::exec 0x7e: "Help Text Detail: " (#3006) + LOW / MEDIUM / HIGH (#2807..#2809), S_PREF_CHANGE.
		static const char* names[4] = {"", "LOW", "MEDIUM", "HIGH"};
		[self say:[self locString:3006 fallback:"Help Text Detail: "].append([self locString:2806 + _verbosity fallback:names[_verbosity]])];
		[self playGlobal:"PREF_CHANGE"];
		return 1;
	}
	if (o == "OPTION_NEXT_MESSAGE" || o == "OPTION_PREV_MESSAGE") {
		float x, y;
		if (_sviewOn && _sview.next_event(x, y)) [self lookAtX:x y:y];
		return 1;
	}
	if (o == "OPTION_MOVE_TO") {
		if (own_units()) _targeting = hud::kOptMoveTo;
		return 1;
	}
	if (o == "OPTION_HALT") {
		if (own_units()) [self runOption:hud::kOptHalt object:-1 modifiers:m];
		return 1;
	}
	if (o == "OPTION_GARRISON") {  // G with units: the next click names the building
		if (own_units()) _targeting = hud::kOptGarrison;
		return 1;
	}
	if (o == "OPTION_EJECT_ALL") {  // J: the selected buildings (or carriers) let their garrison out
		if (own_builds()) [self runOption:hud::kOptEjectAll object:-1 modifiers:m];
		return 1;
	}
	if (o == "OPTION_BACK_TO_WORK") {  // Shift+J: the citizens inside the selected buildings go back to work
		if (own_builds()) [self runOption:hud::kOptBackToWork object:-1 modifiers:m];
		return 1;
	}
	if (o == "OPTION_DISBAND") {
		if (own_units()) {
			const std::vector<int32_t> objs = [self ownSelectedUnits];
			[self issueAs:me block:^bool { return _cm.issue_disband(*_game, objs); }];
		}
		return 1;
	}
	if (o == "OPTION_BUILD" || o == "OPTION_BUILD_MILITARY" || o == "OPTION_BUILD_WONDER") {
		const int opt_id = o == "OPTION_BUILD" ? hud::kOptBuild : o == "OPTION_BUILD_MILITARY" ? hud::kOptBuildMilitary : hud::kOptBuildWonder;
		if (param >= 0) {  // the build menu's letter: start placing that building
			if (_buildPage >= 0 && ron::sim::type_avail(*_game, me, param, 1) == 4) [self runOption:1000 object:param modifiers:m];
			return 1;
		}
		bool citizens = false;
		for (const auto& [who, ob] : _selected) {
			if (const ron::sim::UnitView* u = [self unitView:who o:ob]) {
				citizens = citizens || u->type == ron::sim::ptype::kPeasants || u->type == ron::sim::ptype::kPeasantsKorean;
			}
		}
		if (citizens && own_units()) _buildPage = opt_id;
		return 1;
	}
	if (o == "OPTION_NEXT") return _buildPage >= 0 ? ([self runOption:65 object:-1 modifiers:m], 1) : 0;
	if (o == "OPTION_BACK") return _buildPage >= 0 ? ([self runOption:66 object:-1 modifiers:m], 1) : 0;
	if (o == "OPTION_TRAIN" || o == "OPTION_RESEARCH") {
		if (!own_builds() || param < 0) return 1;
		// The key names the first unit of a line; the card offers its current (upgraded) form: the option on the card whose
		// upgrade chain leads back to `param`.
		const int want = o == "OPTION_TRAIN" ? hud::kOptTrain : hud::kOptResearch;
		for (const hud::Option& op : [self hudOptions]) {
			if (op.option != want || op.object < 0 || !op.enabled) continue;
			int t = op.object;
			for (int step = 0; step < 12 && t >= 0 && t != param; step++) t = _game->type(t).from;
			if (t == param) {
				[self runOption:want object:op.object modifiers:m];
				break;
			}
		}
		return 1;
	}
	// The cycle keys: OPTION_CYCLE_BUILDING/type (zoom), OPTION_NOZOOM_BUILDING, OPTION_ALL_BUILDING and the unit kinds.
	struct Cyc {
		const char* name;
		const char* what;
		BOOL zoom, all, building;
	};
	static const Cyc cycles[] = {
	    {"OPTION_CYCLE_BUILDING", "building", YES, NO, YES}, {"OPTION_NOZOOM_BUILDING", "building", NO, NO, YES},
	    {"OPTION_ALL_BUILDING", "building", NO, YES, YES},   {"OPTION_CYCLE_MILITARY", "military", YES, NO, NO},
	    {"OPTION_NOZOOM_MILITARY", "military", NO, NO, NO},  {"OPTION_ALL_MILITARY", "military", NO, YES, NO},
	    {"OPTION_CYCLE_PEASANT", "peasant", YES, NO, NO},    {"OPTION_NOZOOM_PEASANT", "peasant", NO, NO, NO},
	    {"OPTION_ALL_PEASANT", "peasant", NO, YES, NO},      {"OPTION_CYCLE_GENERAL", "general", YES, NO, NO},
	    {"OPTION_NOZOOM_GENERAL", "general", NO, NO, NO},    {"OPTION_ALL_GENERAL", "general", NO, YES, NO},
	    {"OPTION_CYCLE_SCOUT", "scout", YES, NO, NO},        {"OPTION_NOZOOM_SCOUT", "scout", NO, NO, NO},
	    {"OPTION_ALL_SCOUT", "scout", NO, YES, NO},          {"OPTION_CYCLE_SPY", "spy", YES, NO, NO},
	    {"OPTION_NOZOOM_SPY", "spy", NO, NO, NO},            {"OPTION_ALL_SPY", "spy", NO, YES, NO},
	};
	for (const Cyc& c : cycles) {
		if (o == c.name) {
			[self cycle:c.what param:param zoom:c.zoom all:c.all building:c.building];
			return 1;
		}
	}
	if (o == "OPTION_OPTIONS") {  // F11: the Options page (docs/ui-port.md section 15)
		if (!_gameMenu && !_inPopup) [self openGameMenu:3];
		return 1;
	}
	if (o == "OPTION_STAT_NORMAL" || o == "OPTION_STAT_OFF") {  // Options::exec 0x87 / 0x90: the score window (prefs bit 4, stat_mode 0)
		const bool on = o == "OPTION_STAT_NORMAL";
		_scoreWin = on ? 2 : 1;
		if (on) _sview.show_score();
		prefs::Prefs& P = prefs::get();
		P.score_window = on ? 1 : 0, P.present.insert("score_window");
		prefs::save();
		[self playGlobal:"PREF_CHANGE"];
		return 1;
	}
	if (o == "OPTION_TOGGLE_ADVANCED") {  // the bar's Advanced Options button (pref 0x2: the card's stance / formation / patrol rows)
		_hud.set_advanced(!_hud.advanced());
		[[NSUserDefaults standardUserDefaults] setBool:_hud.advanced() forKey:@"AdvancedMenu"];
		// Options::do_toggle (pref 0x2): "Advanced Options" (#3010) + ": " (#2806) + ON / OFF (#2782 / #2780), S_PREF_CHANGE.
		[self say:[self locString:3010 fallback:"Advanced Options"] + [self locString:2806 fallback:": "] +
		              [self locString:_hud.advanced() ? 2782 : 2780 fallback:_hud.advanced() ? "ON" : "OFF"]];
		[self playGlobal:"PREF_CHANGE"];
		return 1;
	}
	{
		uix::Ctx uc = [self uixCtx];
		const uix::Mods um{shift != NO, (m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0, (m & NSEventModifierFlagOption) != 0};
		const int r = uix::key_option(uc, o, param, um);
		if (r >= 0) return r;
	}
	return 0;
}

- (void)handleHudAction:(const hud::Action&)a modifiers:(NSEventModifierFlags)m {
	switch (a.kind) {
		case hud::ActionKind::MinimapLook:
			if (!([self pres] && ([self pres]->scroll_lock_flags & 4))) [self lookAtX:a.wx y:a.wy];  // scrolling_lock minimap bit
			break;
		case hud::ActionKind::MinimapOrder: [self orderAtWorldX:a.wx y:a.wy modifiers:m]; break;
		case hud::ActionKind::Option: [self runOption:a.option object:a.object modifiers:m]; break;
		case hud::ActionKind::Infinite: [self runOption:uix::kOptInfinite object:-1 modifiers:m]; break;
		case hud::ActionKind::SelectIndex: case hud::ActionKind::SelectType: case hud::ActionKind::AirSelect: {
			// OPTION_OBJECT (click: only this one, Ctrl: remove it, Shift: only the objects of its type), a counted type button
			// (a.object) and the air garrison's family buttons (a.index: 0 fighters, 1 bombers, 2 helicopters).
			uix::Ctx uc = [self uixCtx];
			const uix::Mods um{(m & NSEventModifierFlagShift) != 0, (m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0, (m & NSEventModifierFlagOption) != 0};
			if (a.kind == hud::ActionKind::SelectIndex) uix::select_object(uc, _hudState.selection, a.index, um);
			else if (a.kind == hud::ActionKind::SelectType) uix::select_type(uc, a.object, um);
			else if (_hudState.air.active) uix::select_air(uc, _hudState.air.who, _hudState.air.o, a.index);
			[self selectionChanged];
			break;
		}
		case hud::ActionKind::Buy: case hud::ActionKind::Sell: {
			// IFaceResources::on_button_clicked: OPTION_BUY / OPTION_SELL for the good; Shift = x5 (500), Alt = everything.
			const int flags = (m & NSEventModifierFlagOption) ? 2 : (m & NSEventModifierFlagShift) ? 1 : 0;
			const bool buy = a.kind == hud::ActionKind::Buy;
			const int good = a.object;
			const int me = [self localPlayer];
			[self issueAs:me block:^bool { return buy ? _cm.issue_buy(good, flags) : _cm.issue_sell(good, flags); }];
			break;
		}
		case hud::ActionKind::BarButton: [self barButton:a.index]; break;
		case hud::ActionKind::MiniLib: {
			// IFaceMiniLibrary::on_button_clicked: OPTION_RESEARCH of the line's next tech (or the next age) at the first Library.
			const int me = [self localPlayer];
			if (me < 0 || me >= 8) break;
			const ron::sim::Leader& L = _game->leaders[size_t(me)];
			static const int base[4] = {572, 565, 558, 551};
			const int type = a.index == 0 ? 544 + L.age : base[std::clamp(a.index - 1, 0, 3)] + L.epoch[size_t(std::clamp(a.index - 1, 0, 3))];
			if (a.index == 0 ? L.age >= 7 : L.epoch[size_t(a.index - 1)] >= 7) break;
			for (const ron::sim::BuildView& b : _curView.builds) {
				if (!b.alive || !b.active || b.who != me || b.type != 435) continue;
				const std::vector<int32_t> objs = {b.o};
				if (ron::sim::can_pay_cost(*_game, type, me, -1, b.o, 1) <= 0) _pendingWarnings |= 1;
				[self issueAs:me block:^bool { return _cm.issue_queue(*_game, objs, type, 1); }];
				break;
			}
			break;
		}
		case hud::ActionKind::MinimapButton: {
			static const char* opts[4] = {"OPTION_CYCLE_MILITARY", "OPTION_CYCLE_GENERAL", "OPTION_OBJECTIVES", "OPTION_CYCLE_PEASANT"};
			[self execKeyOption:opts[std::clamp(a.index, 0, 3)] param:-1 modifiers:m];
			break;
		}
		case hud::ActionKind::Library: {
			// IFaceLibrary::on_button_clicked @00807930: OPTION_RESEARCH of the cell's type at the first Library.
			const int me = [self localPlayer];
			if (me < 0 || me >= 8 || a.object < 0) break;
			const int first = ron::sim::get_first_library(*_game, me);
			if (first < 0) break;
			const std::vector<int32_t> objs = {first};
			const int type = a.object;
			if (ron::sim::can_pay_cost(*_game, type, me, -1, first, 1) <= 0) {
				_pendingWarnings |= 1;
				[self flashLacking:type who:me];
			}
			[self issueAs:me block:^bool { return _cm.issue_queue(*_game, objs, type, 1); }];
			break;
		}
		case hud::ActionKind::GarrisonEject: {
			// IFaceGarrison::on_button_clicked / _right_clicked: OPTION_EJECT = UnitOut::issue_come_out (COME_OUT, verb 49) for the first
			// own unit of the clicked type (a right click: every unit of the type).
			const hud::Garrison& gar = _hudState.garrison;
			if (!gar.active || a.index < 0 || a.index >= int(gar.units.size())) break;
			const hud::GarrisonUnit& gu = gar.units[size_t(a.index)];
			if (!gu.own || gu.objects.empty()) break;
			std::vector<int32_t> picks = gu.objects;
			if (!a.all) picks.resize(1);
			const int me = [self localPlayer];
			const int32_t owner = gu.who;
			[self issueAs:me block:^bool {
				bool any = false;
				for (int32_t o : picks) any |= _cm.issue_come_out(*_game, owner, o);
				return any;
			}];
			break;
		}
		case hud::ActionKind::Progress: {
			// A progress button: select the producer and centre the camera on it (negative id: select + OPTION_CENTER).
			if (a.index < 0 || a.index >= int(_hudState.progress.size())) break;
			const hud::ProgressItem& p = _hudState.progress[size_t(a.index)];
			_selected.clear(), _selectedBuilds.clear();
			_selectedBuilds.push_back({p.who, p.o});
			[self lookAtX:p.wx y:p.wy];
			[self selectionChanged];
			break;
		}
		case hud::ActionKind::QueueItem: {
			// OPTION_QUEUE: remove the clicked queue item.
			if (!_selectedBuilds.empty() && _selectedBuilds.front().first == [self localPlayer]) {
				const int32_t o = _selectedBuilds.front().second;
				const int idx = a.index;
				[self issueAs:[self localPlayer] block:^bool { return _cm.issue_unqueue(*_game, o, idx); }];
			}
			break;
		}
		default: break;
	}
}

- (void)selectionChanged {
	_buildPage = -1, _targeting = -1, _placeType = -1;
	[self updateTitle];
}

// A right-click order at a world point: units move (MOVE_TO; Shift QUEUE_LAST, Alt QUEUE_FIRST); own buildings set their
// rally point (GATHER_POINT).
- (void)orderAtWorldX:(float)x y:(float)y modifiers:(NSEventModifierFlags)m {
	if (!_game) return;
	const int me = [self localPlayer];
	const int32_t ix = int32_t(std::lround(std::clamp(x, 0.0f, _mesh.width - 1))), iy = int32_t(std::lround(std::clamp(y, 0.0f, _mesh.height - 1)));
	const ron::sim::QueuePos q = (m & NSEventModifierFlagShift) ? ron::sim::kQueueLast : (m & NSEventModifierFlagOption) ? ron::sim::kQueueFirst
	                                                                                                                     : ron::sim::kQueueNew;
	if (!_selected.empty()) {
		const std::vector<int32_t> objs = [self ownSelectedUnits];
		// Two or more units walk as a body that keeps the player's own arrangement (kFormKeep, sim/groups.hpp); the
		// original's formations are chosen with the right drag or the card's formation button.
		const int32_t form = objs.size() >= 2 ? ron::sim::kFormKeep : -1;
		[self issueAs:me block:^bool { return _cm.issue_move_to(*_game, objs, ix, iy, q, form); }];
	} else if (!_selectedBuilds.empty()) {
		const std::vector<int32_t> objs = [self ownSelectedBuilds];
		const bool add = (m & NSEventModifierFlagShift) != 0;
		uix::Ctx uc = [self uixCtx];
		const int action = uix::gather_action(uc);  // 3 for airbases: a patrol point for their planes
		[self issueAs:me block:^bool { return _cm.issue_gather_point(*_game, objs, ix, iy, action, add); }];
		std::printf("rally %zu building(s) at (%d, %d)\n", objs.size(), ix, iy);
	}
}

// ---------------------------------------------------------------- selection (docs/ui-spec.md 4.1)

- (CGPoint)toDrawable:(CGPoint)p {
	return CGPointMake(p.x * _uiScale, p.y * _uiScale);
}

// A drawn unit's pick point in UI pixels (its middle rather than its feet).
- (simd_float2)uiPointOfUnit:(const SimUnitDrawn&)d {
	const simd_float3 base = simd_make_float3(d.x, d.y, std::max(0.0f, _terrain.sample_height(d.x, d.y)));
	return [self screenOf:base + simd_make_float3(0, 0, 60)] / _uiScale;
}

- (simd_float2)uiPointOfBuilding:(const PlacedBuilding&)b {
	return [self screenOf:simd_make_float3(b.x, b.y, std::max(0.0f, _terrain.sample_height(b.x, b.y)))] / _uiScale;
}

- (BOOL)buildingVisible:(const PlacedBuilding&)b {
	return b.who >= 0 && (!_game || !_fogOn || ron::sim::build_seen_by(*_game, _game->local_play, b.who, b.o));
}

- (BOOL)selectWho:(int)who objects:(const std::vector<int32_t>&)objs buildings:(BOOL)builds {
	_selected.clear(), _selectedBuilds.clear();
	for (int32_t o : objs) (builds ? _selectedBuilds : _selected).push_back({who, o});
	[self selectionChanged];
	return !objs.empty();
}

- (void)setBuildPage:(int)page {
	_buildPage = page;
}

- (void)queueWho:(int)who building:(int)o type:(int)type count:(int)n {
	std::vector<int32_t> objs{o};
	[self issueAs:who block:^bool { return _cm.issue_queue(*_game, objs, type, std::max(1, n)); }];
}

// The viewing seat (console.play): seat-local, never feeds the simulation.
- (void)setLocalPlayer:(int)who {
	if (_game && who >= 0 && who < 8) _game->local_play = who;
}

// The object under a UI point: the nearest drawn unit within 20 px, else a seen building whose footprint is near the
// ground point. Returns 1 unit, 2 building, 0 nothing.
- (int)pickAt:(CGPoint)p who:(int32_t&)who o:(int32_t&)o type:(int32_t&)type {
	float best = 20;
	int kind = 0;
	for (const SimUnitDrawn& d : _drawnUnits) {
		const float dist = simd_distance([self uiPointOfUnit:d], simd_make_float2(float(p.x), float(p.y)));
		if (dist < best) best = dist, who = d.who, o = d.o, type = d.type, kind = 1;
	}
	if (kind) return kind;
	const simd_float2 g = [self groundAt:[self toDrawable:p]];
	float bestw = 1e9f;
	for (const PlacedBuilding& b : _placed) {
		if (![self buildingVisible:b]) continue;
		const ron::sim::BuildView* v = [self buildView:b.who o:b.o];
		const int32_t t = v ? v->type : -1;
		const float r = t >= 0 ? float(std::max(_game->type(t).x_size, _game->type(t).y_size)) * 96.0f + 96.0f : 400.0f;
		const float dist = simd_distance(g, simd_make_float2(b.x, b.y));
		if (dist < r && dist < bestw) bestw = dist, who = b.who, o = b.o, type = t, kind = 2;
	}
	return kind;
}

// Console::left_click: no modifier selects only the object, Shift adds, Ctrl toggles; a double click selects every
// on-screen object of the same owner and type (Shift keeps the old selection). Units and buildings never mix.
- (void)clickSelectAt:(CGPoint)p modifiers:(NSEventModifierFlags)m double:(BOOL)dbl {
	int32_t who = -1, o = -1, type = -1;
	const int kind = [self pickAt:p who:who o:o type:type];
	const bool shift = (m & NSEventModifierFlagShift) != 0, ctrl = (m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0;
	if (!kind) {
		if (!shift && !ctrl) _selected.clear(), _selectedBuilds.clear();
		[self selectionChanged];
		return;
	}
	auto& list = kind == 1 ? _selected : _selectedBuilds;
	auto& other = kind == 1 ? _selectedBuilds : _selected;
	other.clear();
	const auto key = std::make_pair(who, o);
	if (dbl) {
		if (!shift) list.clear();
		const float bottom = float(_hud.bar_top());
		auto on_screen = [&](simd_float2 sp) { return sp.x >= 0 && sp.y >= 0 && sp.x < _uiW && sp.y < (_hudOn ? bottom : _uiH); };
		if (kind == 1) {
			for (const SimUnitDrawn& d : _drawnUnits) {
				if (d.who == who && d.type == type && on_screen([self uiPointOfUnit:d]) && std::find(list.begin(), list.end(), std::make_pair(d.who, d.o)) == list.end())
					list.push_back({d.who, d.o});
			}
		} else {
			for (const PlacedBuilding& b : _placed) {
				const ron::sim::BuildView* v = [self buildingVisible:b] ? [self buildView:b.who o:b.o] : nullptr;
				if (v && v->who == who && v->type == type && on_screen([self uiPointOfBuilding:b])) list.push_back({b.who, b.o});
			}
		}
	} else if (ctrl) {
		auto it = std::find(list.begin(), list.end(), key);
		if (it != list.end()) list.erase(it);
		else list.push_back(key);
	} else if (shift) {
		if (std::find(list.begin(), list.end(), key) == list.end()) list.push_back(key);
	} else {
		list.assign(1, key);
	}
	if (list.size() > 128) list.resize(128);
	[self selectionChanged];
}

// Console::on_left_up box select: units first, buildings only when no unit is boxed; the viewer's own objects win, other
// players' only if none of his are boxed; one owner, at most 128 (SelectGroup::add). Shift adds within the category.
- (void)boxSelect:(CGRect)r modifiers:(NSEventModifierFlags)m {
	const bool shift = (m & NSEventModifierFlagShift) != 0;
	const int me = [self localPlayer];
	std::map<int32_t, std::vector<std::pair<int32_t, int32_t>>> units, builds;
	for (const SimUnitDrawn& d : _drawnUnits) {
		const simd_float2 sp = [self uiPointOfUnit:d];
		if (CGRectContainsPoint(r, CGPointMake(sp.x, sp.y))) units[d.who].push_back({d.who, d.o});
	}
	if (units.empty()) {
		for (const PlacedBuilding& b : _placed) {
			if (![self buildingVisible:b]) continue;
			const simd_float2 sp = [self uiPointOfBuilding:b];
			if (CGRectContainsPoint(r, CGPointMake(sp.x, sp.y))) builds[b.who].push_back({b.who, b.o});
		}
	}
	auto pick = [&](auto& by_owner) -> std::vector<std::pair<int32_t, int32_t>> {
		if (by_owner.empty()) return {};
		auto it = by_owner.find(me);
		return it != by_owner.end() ? it->second : by_owner.begin()->second;
	};
	const bool is_units = !units.empty();
	std::vector<std::pair<int32_t, int32_t>> found = is_units ? pick(units) : pick(builds);
	if (is_units) {
		// Console::on_left_up's filters: Prefer Military, Ctrl+Shift citizens, Alt soldiers, Alt+Ctrl siege, Alt+Shift aircraft.
		uix::Ctx uc = [self uixCtx];
		const uix::Mods um{(m & NSEventModifierFlagShift) != 0, (m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0, (m & NSEventModifierFlagOption) != 0};
		uix::box_filter(uc, found, um, _hud.prefer_military());
	}
	if (found.empty()) {
		if (!shift) _selected.clear(), _selectedBuilds.clear();
		[self selectionChanged];
		return;
	}
	auto& list = is_units ? _selected : _selectedBuilds;
	(is_units ? _selectedBuilds : _selected).clear();
	if (!shift || (!list.empty() && list.front().first != found.front().first)) list.clear();
	for (const auto& k : found) {
		if (list.size() >= 128) break;
		if (std::find(list.begin(), list.end(), k) == list.end()) list.push_back(k);
	}
	[self selectionChanged];
}

// ---------------------------------------------------------------- mouse

// The formation drag starts from the selection's own formation and spacing (the members' common Unit::form, the average
// form_mod; default 50).
- (void)beginFormationDrag {
	_formDrag = YES;
	_formEnd = _formStart;
	int form = -1, n = 0, sum = 0;
	bool common = true;
	for (const auto& [who, o] : _selected) {
		const ron::sim::Unit* u = _game->unit(who, o);
		if (!u || u->type < 50 || (u->type >= 50 && u->type <= 53)) continue;  // citizens carry no formation
		if (form < 0) form = u->form;
		else if (form != u->form) common = false;
		sum += u->form_mod, n++;
	}
	_formForm = common && form >= 0 && form < 5 ? form : 0;
	_formWidth = n > 0 && sum / n > 0 ? std::clamp(sum / n, 0, 100) : 50;
}

// The slots of the drag preview (Scene::draw_drag_formations): the front rank at the press point, ranks behind it, the width
// of the formation set by the spacing. Group::compute_form lays out the real one when the command runs.
- (std::vector<simd_float2>)formationSlots {
	std::vector<simd_float2> out;
	const int n = int(_selected.size());
	if (n <= 0) return out;
	{
		// Group::compute_form (uix::formation_slots): the slots the MOVE_TO will produce.
		uix::Ctx uc = [self uixCtx];
		const simd_float2 d = _formEnd - _formStart;
		const bool facing = simd_length(d) > 96;
		const uint32_t angle = facing ? ron::sim::find_angle(int32_t(std::lround(d.x)), int32_t(std::lround(d.y))) : 0;
		const auto slots = uix::formation_slots(uc, int32_t(std::lround(_formStart.x)), int32_t(std::lround(_formStart.y)), _formForm, _formWidth, facing, angle);
		if (!slots.empty()) {
			for (const auto& p : slots) out.push_back(simd_make_float2(float(p.first), float(p.second)));
			return out;
		}
	}
	simd_float2 fwd = _formEnd - _formStart;
	const float len = simd_length(fwd);
	fwd = len > 1 ? fwd / len : simd_make_float2(0, 1);
	const simd_float2 side = simd_make_float2(-fwd.y, fwd.x);
	const int cols = std::clamp(int(std::ceil(n * (0.1 + _formWidth / 100.0 * 0.9))), 1, n);
	const float gap = 190.0f;
	for (int i = 0; i < n; i++) {
		const int r = i / cols, c = i % cols;
		const int in_row = std::min(cols, n - r * cols);
		const float lateral = (float(c) - float(in_row - 1) / 2) * gap;
		out.push_back(_formStart + side * lateral - fwd * (float(r) * gap));
	}
	return out;
}

- (void)endFormationDrag:(NSEventModifierFlags)m {
	_formDrag = NO;
	if (!_game || _selected.empty()) return;
	const int me = [self localPlayer];
	const std::vector<int32_t> objs = [self ownSelectedUnits];
	if (objs.empty()) return;
	const ron::sim::QueuePos q = (m & NSEventModifierFlagShift) ? ron::sim::kQueueLast : (m & NSEventModifierFlagOption) ? ron::sim::kQueueFirst
	                                                                                                                     : ron::sim::kQueueNew;
	const int32_t ix = int32_t(std::lround(std::clamp(_formStart.x, 0.0f, _mesh.width - 1))), iy = int32_t(std::lround(std::clamp(_formStart.y, 0.0f, _mesh.height - 1)));
	const simd_float2 d = _formEnd - _formStart;
	const bool facing = simd_length(d) > 96;  // a drag too short to name a direction keeps the units' own
	const uint32_t angle = facing ? ron::sim::find_angle(int32_t(std::lround(d.x)), int32_t(std::lround(d.y))) : 0;
	const int form = _formForm, width = _formWidth;
	[self issueAs:me block:^bool { return _cm.issue_move_to(*_game, objs, ix, iy, q, form, facing, angle, 1, width); }];
	std::printf("formation move %zu unit(s) to (%d, %d), form %d%s spacing %d%s at frame %d\n", objs.size(), ix, iy, form, form == ron::sim::kFormKeep ? " (keep arrangement)" : "", width, facing ? ", facing set" : "", _game->frame);
}

- (void)uiMouseDown:(CGPoint)p right:(BOOL)right modifiers:(NSEventModifierFlags)m clicks:(NSInteger)clicks {
	_mouse = p;
	_mouseInside = YES;
	if (_gameMenu == 3 && _optPage) {
		if (!right) _optPage->mouse_down(int(p.x), int(p.y));
		return;
	}
	if (_gameMenu) {
		_gmPress = right ? -1 : [self gameMenuAt:p];
		return;
	}
	if (_gnameSlot >= 0) return;  // the group name dialog: keys only
	if (_pbv && !right && [self pbBarClick:p]) return;
	if (_sviewOn && [self modalMouse:p up:NO]) return;
	if ([self hudVisible] && [self groupPanelClick:p right:right rename:(m & NSEventModifierFlagShift) != 0]) return;
	if ([self hudVisible] && _hud.captures(int(p.x), int(p.y))) {
		_hudPress = YES;
		[self handleHudAction:_hud.mouse_down(int(p.x), int(p.y), right, _hudState) modifiers:m];
		return;
	}
	_hudPress = NO;
	if (_formDrag && !right) {  // a left press during a right formation drag cycles the formation
		_formForm = _formForm == ron::sim::kFormKeep ? 0 : _formForm == 4 ? ron::sim::kFormKeep : _formForm + 1;  // LINE .. ECHELON_R, then KEEP
		return;
	}
	_press = p;
	if (right) {
		_rightPress = YES, _rightDragged = NO;
		_formDrag = NO;
		_formStart = [self groundAt:[self toDrawable:p]];
		return;
	}
	_pressing = YES, _boxing = NO, _pressMs = _timeMs, _pressMods = m;
	_pressClicks = clicks;
}

- (void)uiMouseDragged:(CGPoint)p right:(BOOL)right delta:(CGPoint)d {
	_mouse = p;
	if (_pbScrub) {
		[self pbBarDrag:p];
		return;
	}
	if (_gameMenu == 3 && _optPage) {
		_optPage->mouse_drag(int(p.x), int(p.y));
		return;
	}
	if (_gameMenu) {
		_gmHover = [self gameMenuAt:p];
		return;
	}
	if (_hudPress) {
		[self handleHudAction:_hud.mouse_drag(int(p.x), int(p.y), _hudState) modifiers:0];
		return;
	}
	const float dist = float(std::hypot(p.x - _press.x, p.y - _press.y));
	if (right) {
		// Click vs drag: 6 px for the right button. With own units selected a right drag is a formation move; otherwise it
		// scrolls the map.
		if (_rightPress && dist > 6) _rightDragged = YES;
		if (_rightDragged && !_formDrag && _game && !_selected.empty() && _selected.front().first == [self localPlayer] && _placeType < 0 && _targeting < 0) {
			[self beginFormationDrag];
		}
		if (_formDrag) {
			_formEnd = [self groundAt:[self toDrawable:p]];
		} else if (_rightDragged) {
			[self scrollByPixelsX:float(-d.x * _uiScale) y:float(d.y * _uiScale)];
		}
		return;
	}
	// Left: 20 px once held more than 7 graphic ticks (462 ms), else 64 px (Console::init / on_mouse_move).
	const float threshold = _timeMs - _pressMs > 7 * 66.0 ? 20 : 64;
	if (_pressing && !_boxing && dist > threshold && _placeType < 0 && _targeting < 0) _boxing = YES;
}

- (void)uiMouseUp:(CGPoint)p right:(BOOL)right modifiers:(NSEventModifierFlags)m {
	_mouse = p;
	if (_pbScrub) {
		[self pbBarRelease:p];
		return;
	}
	if (_gameMenu == 3 && _optPage) {
		if (!right) _optPage->mouse_up(int(p.x), int(p.y));
		return;
	}
	if (_gameMenu) {
		const int i = [self gameMenuAt:p];
		const int pressed = _gmPress;
		_gmPress = -1;
		if (!right && i >= 0 && i == pressed) [self gameMenuChoose:i];
		return;
	}
	if (_sviewOn && (_sview.modal()) && [self modalMouse:p up:YES]) return;
	if (_hudPress) {
		_hudPress = NO;
		[self handleHudAction:_hud.mouse_up(int(p.x), int(p.y), right, _hudState) modifiers:m];
		return;
	}
	if (right) {
		if (_formDrag) {
			_formEnd = [self groundAt:[self toDrawable:p]];
			[self endFormationDrag:m];
			_rightPress = NO;
			return;
		}
		if (_rightPress && !_rightDragged) {
			if (_placeType >= 0 || _targeting >= 0) {
				_placeType = -1, _targeting = -1;  // a right click cancels a pending placement or target
			} else if (!_selected.empty()) {
				[self commandMoveAt:[self toDrawable:p]];
			} else if (!_selectedBuilds.empty()) {
				const simd_float2 g = [self groundAt:[self toDrawable:p]];
				[self orderAtWorldX:g.x y:g.y modifiers:m];
			}
		}
		_rightPress = NO;
		return;
	}
	if (!_pressing) return;
	_pressing = NO;
	if (_placeType >= 0) {
		[self placeAt:p modifiers:m];
	} else if (_targeting >= 0) {
		const simd_float2 g = [self groundAt:[self toDrawable:p]];
		if (_targeting == hud::kOptMoveTo) [self orderAtWorldX:g.x y:g.y modifiers:m];
		else if (_targeting == hud::kOptGarrison) [self commandGarrisonAt:[self toDrawable:p]];
		else [self targetClickAt:[self toDrawable:p] modifiers:m];
		_targeting = -1;
	} else if ([self pres] && [self pres]->mouse_selection_disabled) {
		_boxing = NO;  // mouse_selection_disable: clicks and boxes select nothing
	} else if (_boxing) {
		_boxing = NO;
		[self boxSelect:CGRectMake(std::min(_press.x, p.x), std::min(_press.y, p.y), std::fabs(p.x - _press.x), std::fabs(p.y - _press.y))
		      modifiers:m];
	} else {
		[self clickSelectAt:p modifiers:m double:_pressClicks >= 2];
	}
}

- (void)uiMouseMoved:(CGPoint)p {
	_mouse = p;
	_mouseInside = YES;
	if (_gameMenu == 3 && _optPage) {
		_optPage->mouse_move(int(p.x), int(p.y));
		return;
	}
	if (_gameMenu) {
		const int i = [self gameMenuAt:p];
		if (i >= 0) _gmHover = i;
		return;
	}
	_hud.mouse_move(int(p.x), int(p.y));
	if (_sviewOn && _sview.popup_open()) _sview.popup().hover = _sview.popup_button_at(int(p.x), int(p.y));
	if (_placeType >= 0 && !([self hudVisible] && _hud.captures(int(p.x), int(p.y)))) {
		const simd_float2 g = [self groundAt:[self toDrawable:p]];
		[self previewPlacementAtWorldX:g.x y:g.y];
	}
}

// Camera::zoom_in(x, y) / zoom_out: one level per step, toward the cursor; in to 6, out to 4.
- (void)zoomBy:(int)d toward:(CGPoint)p {
	if ([self pres] && ([self pres]->zoom_lock_flags & 1)) return;  // zooming_lock
	const int z = std::clamp(_zoom + d, d < 0 ? std::min(_zoom, 4) : 0, 6);
	if (z == _zoom) return;
	const CGPoint dp = [self toDrawable:p];
	const simd_float2 before = [self groundAt:dp];
	_zoom = z;
	if ([self pres] && ([self pres]->scroll_lock_flags & 1)) {  // Camera::zoom_in: no pan toward the cursor while locked
		[self updateTitle];
		return;
	}
	const simd_float2 after = [self groundAt:dp];
	_lookAt.x = std::clamp(_lookAt.x + before.x - after.x, 0.0f, _mesh.width);
	_lookAt.y = std::clamp(_lookAt.y + before.y - after.y, 0.0f, _mesh.height);
	[self updateTitle];
}

// A mouse wheel notch zooms (ZOOM_IN3 / ZOOM_OUT3); trackpad scrolling pans the map.
- (void)uiScrollWheel:(NSEvent*)e at:(CGPoint)p {
	if (_gameMenu == 3 && _optPage) {
		_optPage->wheel(double(e.scrollingDeltaY), int(p.x), int(p.y));
		return;
	}
	if (_gameMenu) return;
	if (_formDrag && e.scrollingDeltaY != 0) {  // the wheel changes the formation's spacing by 5 (0..100)
		_formWidth = std::clamp(_formWidth + (e.scrollingDeltaY > 0 ? 5 : -5), 0, 100);
		return;
	}
	if (!e.hasPreciseScrollingDeltas) {
		if (e.scrollingDeltaY != 0) [self zoomBy:(e.scrollingDeltaY > 0 ? 1 : -1) toward:p];
		return;
	}
	[self scrollByPixelsX:float(-e.scrollingDeltaX * _uiScale) y:float(e.scrollingDeltaY * _uiScale)];
}

- (void)uiMagnify:(float)amount at:(CGPoint)p {
	if (_gameMenu) return;
	_magnify += amount;
	if (std::fabs(_magnify) >= 0.15f) {
		[self zoomBy:(_magnify > 0 ? 1 : -1) toward:p];
		_magnify = 0;
	}
}

// Camera::check_scroll @008440f0: a 2 px edge band; the left and right edges only above the HUD top - 40 px. Step
// min(1, dt / 67 * 0.5) * 63.9936 * speed (1.0) * scroll_speeds[zoom] world units along the camera axes.
- (void)edgeScroll:(double)dtMs {
	if (_gameMenu) return;
	NSWindow* win = self.view.window;
	if (!win || !win.isKeyWindow || _hudPress || _rightDragged) return;
	if ([self pres] && ([self pres]->scroll_lock_flags & 1)) return;  // scrolling_lock (Camera::check_scroll)
	if (_inPopup || (_sviewOn && _sview.modal())) return;
	const NSPoint wp = [self.view convertPoint:[win mouseLocationOutsideOfEventStream] fromView:nil];
	const NSRect b = self.view.bounds;
	if (!NSPointInRect(wp, b)) return;
	const float x = float(wp.x), y = float(b.size.height - wp.y), W = float(b.size.width), H = float(b.size.height);
	static const float speeds[7] = {48, 32, 24, 16, 12, 9, 8};
	const float option = prefs::get().has("scroll_speed") ? prefs::scroll_factor(prefs::get().scroll_speed) : 1.0f;  // speed_option of the Options page
	const float step = float(std::min(1.0, dtMs / 67.0 * 0.5) * 63.9936) * option * speeds[std::clamp(_zoom, 0, 6)] * (_boxing ? 0.25f : 1.0f);
	const bool sides = ![self hudVisible] || y < float(_hud.bar_top() - 40) || _boxing;
	float dx = 0, dy = 0;
	if (x < 2 && sides) dx = -1;
	if (x >= W - 2 && sides) dx = 1;
	if (y < 2) dy = 1;
	if (y >= H - 2) dy = -1;
	if (dx == 0 && dy == 0) return;
	const simd_float3 ground_up = simd_normalize(simd_make_float3(kUp.x, kUp.y, 0));
	const simd_float3 right = simd_normalize(simd_make_float3(kRight.x, kRight.y, 0));
	_lookAt += (right * dx + ground_up * dy) * step;
	_lookAt.x = std::clamp(_lookAt.x, 0.0f, _mesh.width);
	_lookAt.y = std::clamp(_lookAt.y, 0.0f, _mesh.height);
}

// ---------------------------------------------------------------- control groups (docs/ui-spec.md 4.3)

// Ctrl+N stores the selection (or, with nothing selected, bookmarks the camera); Ctrl+Shift+N erases; N recalls (a
// second press within 500 ms centres the camera on the group); Shift+N adds the group. Seat-local for now.
- (BOOL)handleGroupKey:(NSEvent*)e {
	static const unsigned short codes[10] = {29, 18, 19, 20, 21, 23, 22, 26, 28, 25};  // kVK_ANSI_0..9
	int n = -1;
	for (int i = 0; i < 10; i++) {
		if (e.keyCode == codes[i]) n = i;
	}
	if (n < 0) return NO;
	const NSEventModifierFlags m = e.modifierFlags;
	return [self controlGroup:n ctrl:(m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0 shift:(m & NSEventModifierFlagShift) != 0];
}

// Ctrl+n assigns (an empty selection bookmarks the camera), Ctrl+Shift+n erases, n recalls (twice quickly: centre), Shift+n adds
// the group to the selection.
- (BOOL)controlGroup:(int)n ctrl:(bool)ctrl shift:(bool)shift {
	if (!_game) return YES;
	_hotkeys.attach(*_game);  // the group table is the simulation's (HOTKEY command, sim/commands_extra.cpp)
	const int play = _cm.play;
	hotkeys::Command cmd;
	cmd.play = play, cmd.group = n;
	if (ctrl && shift) {
		cmd.clear = 1;  // HOTKEY with clear: Ctrl+Shift erases the group
		_hotkeys.issue(_cm, *_game, cmd, {}, {});
		_cm.process_turn(*_game);  // runs at once in single player; the multiplayer sink hands it to the session
		if (n >= 0 && n < 10) _groupNames[size_t(n)].clear();
		std::printf("HOTKEY group %d cleared\n", n);
	} else if (ctrl) {
		if (n >= 0 && n < 10 && [self selectedGroup] != n) _groupNames[size_t(n)].clear();  // another army takes the number: its name goes
		if (_selected.empty() && _selectedBuilds.empty()) {
			cmd.x = _lookAt.x, cmd.y = _lookAt.y, cmd.zoom = _zoom;  // an empty selection bookmarks the camera
			_hotkeys.issue(_cm, *_game, cmd, {}, {});
		} else {
			cmd.valid = 1;
			_hotkeys.issue(_cm, *_game, cmd, _selected, _selectedBuilds);
		}
		_cm.process_turn(*_game);
		std::printf("HOTKEY group %d %s (%zu units, %zu buildings)\n", n, cmd.valid ? "assigned" : "bookmarked", _selected.size(), _selectedBuilds.size());
	} else if ([self pres] && [self pres]->hotkey_selection_disabled) {
		// hotkey_selection_disable: control groups do not recall
	} else {
		hotkeys::Slot g = _hotkeys.slot(play, n);
		if (g.bookmark) {
			_lookAt = simd_make_float3(g.x, g.y, _lookAt.z), _zoom = g.zoom;
		} else if (!g.units.empty() || !g.builds.empty()) {
			hotkeys::ObjectList alive;
			for (const auto& k : g.units) {
				const ron::sim::UnitView* u = [self unitView:k.first o:k.second];
				if (u && u->alive) alive.push_back(k);
			}
			g.units = alive;
			_hotkeys.mut_slot(play, n).units = alive;
			if (shift) {
				for (const auto& k : g.units) {
					if (std::find(_selected.begin(), _selected.end(), k) == _selected.end()) _selected.push_back(k);
				}
			} else {
				if (_lastGroupKey == n && _timeMs - _lastGroupMs < 500) {
					float sx = 0, sy = 0, c = 0;
					for (const auto& k : g.units) {
						if (const ron::sim::UnitView* u = [self unitView:k.first o:k.second]) sx += float(u->x), sy += float(u->y), c++;
					}
					for (const auto& k : g.builds) {
						if (const ron::sim::BuildView* b = [self buildView:k.first o:k.second]) sx += float(b->x), sy += float(b->y), c++;
					}
					if (c > 0) [self lookAtX:sx / c y:sy / c];
				}
				_selected = g.units, _selectedBuilds = g.builds;
				[self syncSimBuildings];
				std::printf("HOTKEY group %d recalled (%zu units, %zu buildings)\n", n, g.units.size(), g.builds.size());
			}
			[self selectionChanged];
		}
	}
	_lastGroupKey = n, _lastGroupMs = _timeMs;
	return YES;
}

// ---------------------------------------------------------------- army groups (docs/ui-port.md section 14)
//
// The control groups 0..9 (Ctrl+n assigns, n recalls: controlGroup above, HOTKEY slots of the local leader) with a name each, a HUD
// panel and a regroup order. G with soldiers selected opens a name dialog; Enter stores the members in the first free number (1..9,
// then 0) through HOTKEY, so the group answers to its key exactly like one made with Ctrl+n, and keeps the name here (a Ctrl+n group
// shows as "Grupo n" until renamed). The panel lists every unit group: click recalls (as the key does), a second click centres,
// Shift+click renames, right-click erases; with the group selected G renames, and Shift+G, the panel's button or the card's
// Reagrupar regroup it: one MOVE_TO of the whole group to its centroid, so the engine's formation code places the members together
// and they walk there as one group move. Names are not saved (the port's save leaves the control-group table out too, sim/control.hpp).

static constexpr int kGroupKeys = 10;
static int group_key_order(int i) { return (i + 1) % kGroupKeys; }  // panel / free-slot order: 1..9, then 0

// The own soldiers of the selection: alive, not citizens / scholars / caravans / merchants, not aircraft.
- (hotkeys::ObjectList)militarySelection {
	hotkeys::ObjectList out;
	if (!_game) return out;
	namespace pt = ron::sim::ptype;
	const int me = [self localPlayer];
	for (const auto& k : _selected) {
		if (k.first != me) continue;
		const ron::sim::Unit* u = _game->unit(k.first, k.second);
		if (!u || !u->alive() || !u->ptype) continue;
		const int t = u->type;
		if ((t >= pt::kPeasants && t <= pt::kScholarsKorean) || t == pt::kCara || t == pt::kMerchant || t == pt::kMerchantDutch) continue;
		if (ron::sim::unit_is_air(*u)) continue;
		out.push_back(k);
	}
	return out;
}

// The living unit members of control group `slot` (0..9) as the simulation's table holds them (none for a bookmark or buildings).
- (hotkeys::ObjectList)groupMembers:(int)slot {
	hotkeys::ObjectList out;
	if (!_game || slot < 0 || slot >= kGroupKeys) return out;
	_hotkeys.attach(*_game);
	const hotkeys::Slot g = _hotkeys.slot([self localPlayer], slot);
	if (g.bookmark) return out;
	for (const auto& k : g.units) {
		const ron::sim::UnitView* u = [self unitView:k.first o:k.second];
		if (u && u->alive) out.push_back(k);
	}
	return out;
}

- (std::string)groupName:(int)slot {
	return _groupNames[size_t(slot)].empty() ? "Grupo " + std::to_string(slot) : _groupNames[size_t(slot)];
}

// The captains of a list (the table and a recall hold followers too; the player counts and boxes captains).
- (hotkeys::ObjectList)captainsOf:(const hotkeys::ObjectList&)m {
	hotkeys::ObjectList out;
	for (const auto& k : m) {
		const ron::sim::Unit* u = _game ? _game->unit(k.first, k.second) : nullptr;
		if (u && u->o_up < 0) out.push_back(k);
	}
	return out;
}

// The control group whose living captains are exactly the selection's (-1 none).
- (int)selectedGroup {
	if (_selected.empty() || !_selectedBuilds.empty()) return -1;
	const hotkeys::ObjectList sel = [self captainsOf:_selected];
	for (int s = 0; s < kGroupKeys; s++) {
		const hotkeys::ObjectList m = [self captainsOf:[self groupMembers:s]];
		if (m.empty() || m.size() != sel.size()) continue;
		bool same = true;
		for (const auto& k : sel) same = same && std::find(m.begin(), m.end(), k) != m.end();
		if (same) return s;
	}
	return -1;
}

- (void)openGroupDialog:(int)slot rename:(BOOL)rename {
	_gnameSlot = slot, _gnameRename = rename;
	_gnameText = rename ? [self groupName:slot] : "Grupo " + std::to_string(slot);
	_pressing = _boxing = _hudPress = _rightPress = NO;
}

// G (shift = NO): the name dialog for a new group of the selected soldiers, or for the selected group (rename); Shift+G: regroup
// the selected group. Returns NO when the key meant nothing here (it then goes to the key map).
- (BOOL)groupKey:(BOOL)shift {
	if (!_game || _inPopup) return NO;
	const int cur = [self selectedGroup];
	if (shift) return cur >= 0 && [self regroupGroup:cur];
	if (cur >= 0) {
		[self openGroupDialog:cur rename:YES];
		return YES;
	}
	if ([self militarySelection].empty()) return NO;
	_hotkeys.attach(*_game);
	int slot = -1;
	for (int i = 0; i < kGroupKeys && slot < 0; i++) {
		const int s = group_key_order(i);
		if (!_hotkeys.slot([self localPlayer], s).used) slot = s;
	}
	if (slot < 0) {
		[self say:"Os 10 grupos (0-9) estão em uso"];
		return YES;
	}
	[self openGroupDialog:slot rename:NO];
	return YES;
}

- (void)groupNameKey:(NSEvent*)e {
	if (e.keyCode == 53) {  // Esc
		_gnameSlot = -1;
		return;
	}
	if (e.keyCode == 36 || e.keyCode == 76) {  // Return / keypad Enter
		[self confirmGroupName];
		return;
	}
	if (e.keyCode == 51) {  // Delete: one UTF-8 character
		while (!_gnameText.empty()) {
			const bool cont = (static_cast<unsigned char>(_gnameText.back()) & 0xC0) == 0x80;
			_gnameText.pop_back();
			if (!cont) break;
		}
		return;
	}
	if (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) return;
	NSString* chars = e.characters;
	for (NSUInteger i = 0; i < chars.length; i++) {
		const unichar c = [chars characterAtIndex:i];
		if (c >= 0x20 && c != 0x7f && !(c >= 0xF700 && c <= 0xF8FF) && _gnameText.size() < 24) _gnameText += [[NSString stringWithCharacters:&c length:1] UTF8String];
	}
}

// Enter: a new group stores the selected soldiers through HOTKEY slot n (GROUP + the verb, as Ctrl+n does); a rename only changes
// the name.
- (void)confirmGroupName {
	const int slot = _gnameSlot;
	_gnameSlot = -1;
	if (slot < 0 || !_game) return;
	std::string name = _gnameText;
	while (!name.empty() && name.back() == ' ') name.pop_back();
	while (!name.empty() && name.front() == ' ') name.erase(name.begin());
	if (_gnameRename) {
		_groupNames[size_t(slot)] = name;  // empty: back to "Grupo n"
		std::printf("group %d renamed \"%s\"\n", slot, [self groupName:slot].c_str());
		return;
	}
	const hotkeys::ObjectList members = [self militarySelection];
	if (members.empty()) return;
	hotkeys::Command cmd;
	cmd.play = [self localPlayer], cmd.group = slot, cmd.valid = 1;
	_hotkeys.attach(*_game);
	__block bool ok = false;
	[self issueAs:cmd.play block:^bool { return ok = _hotkeys.issue(_cm, *_game, cmd, members, {}); }];
	if (!ok) return;
	_groupNames[size_t(slot)] = name;
	_selected = members, _selectedBuilds.clear();
	[self selectionChanged];
	std::printf("group %d \"%s\" assigned (%zu units) at frame %d\n", slot, [self groupName:slot].c_str(), members.size(), _game->frame);
}

- (void)nameGroupScript:(const std::string&)name {
	if (![self groupKey:NO] || _gnameSlot < 0) {
		std::fprintf(stderr, "--group: no soldiers selected (or no free group)\n");
		return;
	}
	_gnameText = name;
	[self confirmGroupName];
}

- (simd_float2)centroidOf:(const hotkeys::ObjectList&)m {
	float sx = 0, sy = 0, c = 0;
	for (const auto& k : m) {
		if (const ron::sim::UnitView* u = [self unitView:k.first o:k.second]) sx += float(u->x), sy += float(u->y), c++;
	}
	return c > 0 ? simd_make_float2(sx / c, sy / c) : simd_make_float2(-1, -1);
}

// The panel button: the living members become the selection (as the number key does); clicked again (within 500 ms, or with the
// group already selected) the camera centres on them.
- (void)recallGroup:(int)slot {
	const hotkeys::ObjectList m = [self groupMembers:slot];
	if (m.empty()) return;
	const bool again = [self selectedGroup] == slot || (_lastNamedSlot == slot && _timeMs - _lastNamedMs < 500);
	_selected = m, _selectedBuilds.clear();
	[self syncSimBuildings];
	[self selectionChanged];
	if (again) {
		const simd_float2 c = [self centroidOf:m];
		if (c.x >= 0) [self lookAtX:c.x y:c.y];
	}
	_lastNamedSlot = slot, _lastNamedMs = _timeMs;
	std::printf("group %d \"%s\" recalled (%zu units)%s\n", slot, [self groupName:slot].c_str(), m.size(), again ? ", centred" : "");
}

// Right click on the button: HOTKEY clear (as Ctrl+Shift+n), and the name goes.
- (void)deleteGroup:(int)slot {
	if (!_game || slot < 0 || slot >= kGroupKeys) return;
	hotkeys::Command cmd;
	cmd.play = [self localPlayer], cmd.group = slot, cmd.clear = 1;
	_hotkeys.attach(*_game);
	[self issueAs:cmd.play block:^bool { return _hotkeys.issue(_cm, *_game, cmd, {}, {}); }];
	std::printf("group %d \"%s\" erased\n", slot, [self groupName:slot].c_str());
	_groupNames[size_t(slot)].clear();
}

// Regroup: one MOVE_TO for the whole group to its centroid. Group::action_move_near lays the members out in their formation
// around that point (docs/movement-spec.md section 11) and they go there as one group move, at the leader's speed (march speed
// with a general), so the body re-forms instead of each unit walking alone.
- (BOOL)regroupGroup:(int)slot {
	if (!_game) return NO;
	const hotkeys::ObjectList m = [self groupMembers:slot];
	if (m.size() < 2) return NO;
	const simd_float2 c = [self centroidOf:m];
	const int32_t ix = int32_t(std::lround(std::clamp(c.x, 0.0f, _mesh.width - 1))), iy = int32_t(std::lround(std::clamp(c.y, 0.0f, _mesh.height - 1)));
	std::vector<int32_t> objs;
	for (const auto& k : m) objs.push_back(k.second);
	__block bool ok = false;
	[self issueAs:[self localPlayer] block:^bool { return ok = _cm.issue_move_to(*_game, objs, ix, iy); }];
	if (ok) std::printf("group %d \"%s\" regroups at (%d, %d), %zu units, frame %d\n", slot, [self groupName:slot].c_str(), ix, iy, m.size(), _game->frame);
	return ok;
}

// A press on the panel: left recalls / regroups, Shift+left renames, right erases. NO when the point is not on the panel.
- (BOOL)groupPanelClick:(CGPoint)p right:(BOOL)right rename:(BOOL)rename {
	for (const auto& [slot, r] : _groupRects) {
		if (!r.contains(int(p.x), int(p.y))) continue;
		if (right) [self deleteGroup:slot];
		else if (rename) [self openGroupDialog:slot rename:YES];
		else [self recallGroup:slot];
		return YES;
	}
	if (_regroupSlot >= 0 && _regroupRect.contains(int(p.x), int(p.y))) {
		if (!right) [self regroupGroup:_regroupSlot];
		return YES;
	}
	return NO;
}

// The icon of the card's Agrupar / Reagrupar button: apps/map-mac/resources/group-icon.tga (the app bundle's Resources when
// packaged), registered with the UI renderer under "file:<path>". Empty when missing (the button then shows its label).
- (std::string)groupIconId {
	if (_groupIconChecked) return _groupIconId;
	_groupIconChecked = YES;
	NSMutableArray<NSString*>* paths = [NSMutableArray array];
	if (NSString* p = [NSBundle.mainBundle pathForResource:@"group-icon" ofType:@"tga"]) [paths addObject:p];
	if (NSString* exeDir = NSBundle.mainBundle.executablePath.stringByDeletingLastPathComponent) {
		[paths addObject:[exeDir stringByAppendingPathComponent:@"../Resources/group-icon.tga"]];
		[paths addObject:[exeDir stringByAppendingPathComponent:@"../apps/map-mac/resources/group-icon.tga"]];
		[paths addObject:[exeDir stringByAppendingPathComponent:@"resources/group-icon.tga"]];
	}
	[paths addObject:[@(__FILE__).stringByDeletingLastPathComponent stringByAppendingPathComponent:@"resources/group-icon.tga"]];
	for (NSString* p in paths) {
		if (![NSFileManager.defaultManager fileExistsAtPath:p]) continue;
		std::string id;
		if (_ui.file_texture(p.stringByStandardizingPath.UTF8String, &id)) {
			_groupIconId = id;
			break;
		}
	}
	return _groupIconId;
}

// The panel: a 150 x 22 button per control group that holds units ("n  name", the captain count) down the right edge under the
// top strip, the selected group's in orange with a "Reagrupar" button under the list. A group whose members all died disappears.
- (void)drawGroupPanel {
	_groupRects.clear();
	_regroupSlot = -1;
	if (!_game) return;
	const int bw = 150, bh = 22, gap = 3, x = _uiW - bw - 10;
	int y = 150;
	const int f = _ui.font(14, -1);
	const int sel = [self selectedGroup];
	const bool mouse = _mouseInside && _gnameSlot < 0 && !_gameMenu;
	bool any = false;
	auto button = [&](const ron::IfaceRect& r, bool lit, const std::string& left, const std::string& right_text) {
		const bool hover = mouse && r.contains(int(_mouse.x), int(_mouse.y));
		const uint32_t fill = lit ? rm::argb(235, 150, 100, 20) : hover ? rm::argb(235, 80, 80, 92) : rm::argb(215, 28, 28, 34);
		_ui.fill(float(r.l) - 1, float(r.t) - 1, float(r.w()) + 2, float(r.h()) + 2, 0xFF000000, rm::UiType::Normal);
		_ui.fill(float(r.l), float(r.t), float(r.w()), float(r.h()), fill, rm::UiType::ColorBlend);
		_ui.next_layer();
		_ui.text(f, left, float(r.l) + 6, float(r.t) + 3, 0xFFFFFFFF, 0xFF000000);
		if (!right_text.empty()) _ui.text(f, right_text, float(r.r) - 6 - float(_ui.text_width(f, right_text)), float(r.t) + 3, 0xFFFFE080, 0xFF000000);
	};
	for (int i = 0; i < kGroupKeys; i++) {
		const int s = group_key_order(i);
		const hotkeys::ObjectList m = [self groupMembers:s];
		if (m.empty()) {
			_groupNames[size_t(s)].clear();
			continue;
		}
		if (!any) _ui.next_window(), any = true;
		const ron::IfaceRect r{x, x + bw, y, y + bh};
		_groupRects.push_back({s, r});
		button(r, s == sel, std::to_string(s) + "  " + [self groupName:s], std::to_string([self captainsOf:m].size()));
		y += bh + gap;
	}
	if (sel >= 0 && any) {
		_regroupRect = ron::IfaceRect{x, x + bw, y + gap, y + gap + bh};
		_regroupSlot = sel;
		button(_regroupRect, false, "Reagrupar", "Shift+G");
	}
}

// The name dialog (G): a box with the title, the text field with a blinking cursor and the keys.
- (void)drawGroupNameDialog {
	if (_gnameSlot < 0) return;
	const int w = 320, h = 110, x = (_uiW - w) / 2, y = (_uiH - h) / 2 - 60;
	_ui.next_window();
	_ui.fill(0, 0, float(_uiW), float(_uiH), rm::argb(70, 0, 0, 0), rm::UiType::ColorBlend);
	_ui.next_layer();
	_ui.fill(float(x) - 1, float(y) - 1, float(w) + 2, float(h) + 2, 0xFF000000, rm::UiType::Normal);
	_ui.fill(float(x), float(y), float(w), float(h), rm::argb(240, 39, 40, 43), rm::UiType::ColorBlend);
	_ui.next_layer();
	const int tf = _ui.font(18, -1), f = _ui.font(14, -1);
	const std::string title = (_gnameRename ? "Renomear grupo " : "Nome do grupo ") + std::to_string(_gnameSlot);
	_ui.text_centered(tf, title, {x, x + w, y + 8, y + 32}, 0xFFFFFFFF, 0xFF000000);
	const ron::IfaceRect field{x + 16, x + w - 16, y + 42, y + 68};
	_ui.fill(float(field.l) - 1, float(field.t) - 1, float(field.w()) + 2, float(field.h()) + 2, 0xFF000000, rm::UiType::Normal);
	_ui.fill(float(field.l), float(field.t), float(field.w()), float(field.h()), 0xFF1C1C1C, rm::UiType::Normal);
	_ui.next_layer();
	const int tick = int(_timeMs / 66.0);
	_ui.text(f, _gnameText + ((tick & 8) ? "" : "_"), float(field.l) + 5, float(field.t) + 5, 0xFFFFFFFF, 0xFF000000);
	_ui.text_centered(f, "Enter confirma   Esc cancela", {x, x + w, y + h - 30, y + h - 8}, 0xFFB0B0B0, 0xFF000000);
}

// ---------------------------------------------------------------- scenario scripts (docs/ui-port.md "Script UI")

- (const ron::sim::ScriptPresentation*)pres {
	return _game ? ron::sim::script_presentation(*_game) : nullptr;
}

// The interface is drawn and takes the mouse unless the user turned it off (H) or a script hid it (ui_hide).
- (BOOL)hudVisible {
	const ron::sim::ScriptPresentation* p = [self pres];
	return _hudOn && !(p && p->ui_hidden);
}

- (void)setUiWidth:(int)w height:(int)h scale:(float)scale {
	_uiScale = scale > 0 ? scale : 1;
	_uiW = int(std::lround(w / _uiScale));
	_uiH = int(std::lround(h / _uiScale));
	_drawW = CGFloat(w), _drawH = CGFloat(h);
	if (_hudOn) _hud.layout(_uiW, _uiH);
}

- (void)setScriptLog:(BOOL)on {
	_scriptLog = on;
}

- (void)setMouseInside:(BOOL)inside {
	_mouseInside = inside;
}

// Debugging aid (RON_POPUP_TEST="text"): a two-button popup left open for a snapshot of its look.
- (void)testPopup:(const char*)text {
	ron::sim::ScriptEvent e;
	e.text = text, e.text2 = "Yes", e.text3 = "No", e.a = 2;
	if (_sviewOn) _sview.open_popup(e);
}

// Headless: the mouse over a world point (ground level), as if the player pointed there.
- (void)setHoverX:(float)x y:(float)y {
	_mouse = CGPointMake(x, y);
	_mouseInside = YES;
	_hoverForce = YES;
	_hud.mouse_move(int(x), int(y));
}

- (void)setGameSpeed:(int)speed {
	_speed = std::clamp(speed, 0, 4);
}

- (void)setProgressButtons:(BOOL)on {
	_progressButtons = on;
	_hud.set_progress_buttons(on);
}

// The harness' mouse: the events a person would make (docs/ui-port.md section 7).
- (void)mouseEvent:(const std::string&)kind x:(float)x y:(float)y {
	const CGPoint p = CGPointMake(x, y);
	if (_hudOn && _uiW > 0) {  // no frame has been drawn: the HUD's hit tests need this frame's layout and state
		_hud.layout(_uiW, _uiH);
		_hudState = [self makeHudState];
	}
	if (_gameMenu && _gmRects.empty() && _uiReady) {  // no frame has been drawn since the menu opened: lay its buttons out
		_ui.begin(_uiW, _uiH, 1.0f);
		[self drawGameMenu];
	}
	if (_pbv && _uiReady && !_pbv->laid_out()) {  // no frame has been drawn: lay the playback bar out for its hit tests
		_ui.begin(_uiW, _uiH, 1.0f);
		[self drawPlaybackBar];
	}
	if (kind == "move") [self uiMouseMoved:p];
	else if (kind == "down") [self uiMouseDown:p right:NO modifiers:0 clicks:1];
	else if (kind == "rdown") [self uiMouseDown:p right:YES modifiers:0 clicks:1];
	else if (kind == "drag") [self uiMouseDragged:p right:NO delta:CGPointMake(x - _mouse.x, y - _mouse.y)];
	else if (kind == "rdrag") [self uiMouseDragged:p right:YES delta:CGPointMake(x - _mouse.x, y - _mouse.y)];
	else if (kind == "up") [self uiMouseUp:p right:NO modifiers:0];
	else if (kind == "rup") [self uiMouseUp:p right:YES modifiers:0];
	else if (kind == "wheel") [self zoomBy:y > 0 ? 1 : -1 toward:p];
}

- (void)setPanels:(int)flags streamlined:(BOOL)streamlined help:(int)level {
	if (flags >= 0) _hud.set_panel_flags(flags);
	if (streamlined) _hud.set_opened(false);
	if (level >= 1 && level <= 3) _verbosity = level;
}

- (void)runKeyString:(const std::string&)keys {
	if (!_game) return;
	{
		// Control groups ("Ctrl+3", "Ctrl+Shift+3", "Shift+3", "3") are not key map entries.
		std::string k = keys;
		bool ctrl = false, shift = false;
		for (bool again = true; again;) {
			again = false;
			if (k.rfind("Ctrl+", 0) == 0) ctrl = again = true, k = k.substr(5);
			else if (k.rfind("Shift+", 0) == 0) shift = again = true, k = k.substr(6);
		}
		if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') {
			[self controlGroup:k[0] - '0' ctrl:ctrl shift:shift];
			return;
		}
		if ((k == "G" || k == "g") && !ctrl && [self groupKey:shift]) return;  // named army groups (G / Shift+G)
	}
	if (const keymap::Entry* e = keymap::lookup(keys, _buildPage >= 0 ? 1 : 0, [self](int ctx) { return [self selectionContext:ctx] == YES; }))
		[self execKeyOption:e->option param:e->param modifiers:keys.find("Shift+") != std::string::npos ? NSEventModifierFlagShift : 0];
	else std::fprintf(stderr, "no binding for %s\n", keys.c_str());
}

- (void)setCursorWorldX:(float)x y:(float)y {
	const float z = std::max(0.0f, _terrain.sample_height(x, y));
	const simd_float2 sp = [self screenOf:simd_make_float3(x, y, z)] / (_uiScale > 0 ? _uiScale : 1);
	_mouse = CGPointMake(sp.x, sp.y);
	_mouseInside = YES;
}

- (void)runCardOption:(int)option object:(int)object {
	[self runOption:option object:object modifiers:0];
}

// The window is up: real time drives the graphic clock, sounds and music play (Data/sound.xml TRACKS).
// The profile's interface preferences (player_profile: market_open / resource_open / infra_open, mini_lib_open, verbosity, the
// progress-button pref bit) in NSUserDefaults: read when the window opens, written when one changes. Headless runs neither
// read nor write them.
- (void)loadProfile {
	NSUserDefaults* d = [NSUserDefaults standardUserDefaults];
	if ([d objectForKey:@"PanelFlags"]) _hud.set_panel_flags(int([d integerForKey:@"PanelFlags"]));
	if ([d objectForKey:@"MiniLibOpen"] && [d boolForKey:@"MiniLibOpen"] != _hud.minilib_open()) _hud.toggle_minilib();
	if ([d objectForKey:@"HelpVerbosity"]) _verbosity = std::clamp(int([d integerForKey:@"HelpVerbosity"]), 1, 3);
	if ([d objectForKey:@"ProgressButtons"]) [self setProgressButtons:[d boolForKey:@"ProgressButtons"]];
	if ([d objectForKey:@"GameSpeed"]) _speed = std::clamp(int([d integerForKey:@"GameSpeed"]), 0, 4);
	if ([d objectForKey:@"AdvancedMenu"]) _hud.set_advanced([d boolForKey:@"AdvancedMenu"]);      // pref 0x2
	if ([d objectForKey:@"GroupByType"]) _hud.set_group_by_type([d boolForKey:@"GroupByType"]);  // pref 0x20000
	if ([d objectForKey:@"PreferMilitary"]) _hud.set_prefer_military([d boolForKey:@"PreferMilitary"]);
	{  // preferences.txt (the Options page) wins over the profile for the keys it names
		const prefs::Prefs& P = prefs::get();
		if (P.has("game_speed")) _speed = std::clamp(P.game_speed, 0, 4);
		if (P.has("help_detail")) {
			_helpOn = P.help_detail > 0;
			if (P.help_detail > 0) _verbosity = std::clamp(P.help_detail, 1, 3);
		}
		if (P.has("advanced_menu")) _hud.set_advanced(P.advanced_menu);
		if (P.has("progress_bars")) [self setProgressButtons:P.progress_bars];
		if (P.has("prefer_military")) _hud.set_prefer_military(P.prefer_military);
		if (P.has("streamlined")) _hud.set_opened(!P.streamlined);
		if (P.has("message_length")) scriptview::set_message_length(P.message_length);
		_scoreWin = P.has("score_window") ? 1 + P.score_window : 0;
	}
	_profile[0] = _hud.panel_flags(), _profile[1] = _hud.minilib_open(), _profile[2] = _verbosity, _profile[3] = _progressButtons, _profile[4] = _speed;
}

- (void)syncProfile {
	if (_headless) return;
	const int now[5] = {_hud.panel_flags(), _hud.minilib_open() ? 1 : 0, _verbosity, _progressButtons ? 1 : 0, _speed};
	if (std::equal(now, now + 5, _profile)) return;
	NSUserDefaults* d = [NSUserDefaults standardUserDefaults];
	[d setInteger:now[0] forKey:@"PanelFlags"];
	[d setBool:now[1] != 0 forKey:@"MiniLibOpen"];
	[d setInteger:now[2] forKey:@"HelpVerbosity"];
	[d setBool:now[3] != 0 forKey:@"ProgressButtons"];
	[d setInteger:now[4] forKey:@"GameSpeed"];
	std::copy(now, now + 5, _profile);
}

- (void)startInteractive {
	_headless = NO;
	[self loadProfile];
	_drawW = _drawH = 0;
	_audio = std::make_unique<ron::audio::Audio>();
	if (const prefs::Prefs& pp = prefs::get(); pp.has("fx_volume") || pp.has("music_volume")) {  // the Options page's sound pane
		const float fx = float(pp.fx_volume) / 255.0f;
		_audio->set_volumes(fx, fx, float(pp.music_volume) / 255.0f);
	}
	try {
		std::map<std::string, std::vector<std::string>> moods;
		std::string win, lose;
		const ron::XmlElement doc = ron::parse_xml(_files->read_text("Data/sound.xml"));
		auto file_of = [&](const ron::XmlElement& snd) { return _files->resolve(ron::normalize_game_path(snd.get("file"))); };
		ron::for_each_element(doc, "TRACKS", [&](const ron::XmlElement& tracks) {
			if (!moods.empty()) return;
			for (const ron::XmlElement& c : tracks.children) {
				if (c.name == "WIN" && !c.children.empty()) win = file_of(c.children.front());
				if (c.name == "LOSE" && !c.children.empty()) lose = file_of(c.children.front());
				if (c.name != "TRIBE" || ron::to_lower(c.get("tribe")) != "default") continue;
				for (const ron::XmlElement& age : c.children) {
					for (const ron::XmlElement& mood : age.children) {
						if (mood.name != "MOOD") continue;
						// JukeBox MoodIndex by name: BATTLE_VICTORY = winning, BATTLE_DEFEAT = losing, ECONOMIC = economic.
						const std::string m = mood.get("mood");
						const std::string key = m == "BATTLE_VICTORY" ? "winning" : m == "BATTLE_DEFEAT" ? "losing" : m == "ECONOMIC" ? "economic" : "";
						if (key.empty()) continue;
						for (const ron::XmlElement& snd : mood.children) {
							const std::string f = file_of(snd);
							if (!f.empty()) moods[key].push_back(f);
						}
					}
				}
			}
		});
		_audio->set_music(moods, win, lose);
		_audio->update(_timeMs);
		if (prefs::get().music_in_game) _audio->music_start();
	} catch (const std::exception& e) {
		std::fprintf(stderr, "music unavailable: %s\n", e.what());
	}
}

// AppKit key codes -> Windows virtual keys for Game::keystate (key_down reads VkKeyScan of a character).
static int vk_of(NSEvent* e) {
	switch (e.keyCode) {
		case 123: return 0x25;  // left
		case 124: return 0x27;  // right
		case 125: return 0x28;  // down
		case 126: return 0x26;  // up
		case 36: return 0x0D;   // return
		case 53: return 0x1B;   // escape
		case 48: return 0x09;   // tab
		case 49: return 0x20;   // space
		case 51: return 0x08;   // backspace
		default: break;
	}
	NSString* c = e.charactersIgnoringModifiers;
	if (c.length == 0) return -1;
	unichar ch = [c characterAtIndex:0];
	if (ch >= 'a' && ch <= 'z') ch = unichar(ch - 32);
	return ch < 256 ? int(ch) : -1;
}

- (void)scriptKey:(NSEvent*)e down:(BOOL)down {
	const int vk = vk_of(e);
	if (vk >= 0) _keys[size_t(vk)] = down ? 0x80 : 0;
}

- (void)scriptFlags:(NSEventModifierFlags)m {
	_keys[0x10] = (m & NSEventModifierFlagShift) ? 0x80 : 0;
	_keys[0x11] = (m & NSEventModifierFlagControl) ? 0x80 : 0;
	_keys[0x12] = (m & NSEventModifierFlagOption) ? 0x80 : 0;
}

// The type's build menu page (0 civic, 1 military, 2 wonders) for the armed BUILD option.
- (int)pageOfType:(int)t {
	if (!_game || t < 0) return 0;
	const int cat = _game->type(t).cat;
	return cat >= 0 && cat <= 2 ? cat : 0;
}

// ScriptInput (sim/script_ui.hpp) from ronmap's state, written between frames. Single player: the builtins keep the
// original's multiplayer guards themselves (semaphore byte0 & 4), so the input is filled the same way either way.
- (void)fillScriptInput {
	if (!_game) return;
	ron::sim::ScriptInput* in = ron::sim::script_input(*_game);
	if (!in) return;
	in->attached = true;
	// console.play's SelectGroup: the owner and the captains of the selection.
	in->select_who = -1;
	in->selected.clear();
	const auto& sel = !_selected.empty() ? _selected : _selectedBuilds;
	if (!sel.empty()) {
		in->select_who = sel.front().first;
		for (const auto& [who, o] : sel) {
			if (who == in->select_who) in->selected.push_back(o);
		}
	}
	// Options: the armed command and the build options panel.
	in->option = -1, in->option_object = -1, in->mode_data = 0, in->selecting_spot = false, in->build_options = false;
	if (_targeting >= 0) in->option = _targeting, in->selecting_spot = true;
	if (_buildPage >= 0) in->build_options = true, in->mode_data = _buildPage, in->option = _buildPage;
	if (_placeType >= 0) {
		const int page = _buildPage >= 0 ? _buildPage : [self pageOfType:_placeType];
		in->option = page, in->option_object = _placeType, in->selecting_spot = true, in->build_options = true, in->mode_data = page;
	}
	// Screen, interface rectangles (get_button_*), the objectives box (get_objectives_*).
	in->screen_w = _uiW, in->screen_h = _uiH;
	in->iface_rects.clear();
	if (_hudOn) {
		_hud.layout(_uiW, _uiH);
		for (const auto& [name, r] : _hud.named_rects()) in->iface_rects[name] = ron::sim::ScriptRect{r.l, r.r, r.t, r.b};
	}
	if (_headless && _sviewOn) [self layoutScriptView];  // no frames are drawn: lay the objectives box out anyway
	in->objectives_rect = _sview.objectives_rect();
	// Console: the tile under the cursor and whether it is over the interface.
	in->cursor_tx = in->cursor_ty = -1;
	in->cursor_over_iface = false;
	if (_mouseInside && _uiW > 0) {
		const simd_float2 g = [self groundAt:[self toDrawable:_mouse]];
		if (g.x >= 0 && g.y >= 0 && g.x < _mesh.width && g.y < _mesh.height) in->cursor_tx = int(g.x / 192), in->cursor_ty = int(g.y / 192);
		in->cursor_over_iface = [self hudVisible] && _mouse.y > _hud.bar_top();
	}
	// Camera.
	in->camera_x = _lookAt.x, in->camera_y = _lookAt.y;
	in->zoom_level = _zoom;
	in->camera_panning = _pan.panning != 0;
	__weak MapRenderer* weakSelf = self;
	in->on_screen = [weakSelf](int32_t x, int32_t y) {
		MapRenderer* me = weakSelf;
		if (!me) return false;
		const CGSize size = [me drawSize];
		const float z = std::max(0.0f, me->_terrain.sample_height(float(x), float(y)));
		const simd_float2 sp = [me screenOf:simd_make_float3(float(x), float(y), z)];
		return sp.x >= 0 && sp.y >= 0 && sp.x < float(size.width) && sp.y < float(size.height);
	};
	in->keystate = _keys;
	in->user_warnings |= _pendingWarnings;
	_pendingWarnings = 0;
	// ScenarioData::sound: "still playing" while our player plays it; otherwise the WAV-length model decides, so a sound
	// the script starts within a frame is never reported finished before it was heard.
	in->sound_state = (_audio && _audio->voice_playing()) ? 0 : -1;
	in->graphic_clock = int32_t(_timeMs);
	in->player_profile_name = "Player";
}

- (scriptview::Env)scriptEnv {
	scriptview::Env env;
	env.ui_w = _uiW, env.ui_h = _uiH;
	env.tick = int(_timeMs / 66.0);
	env.ms = _timeMs;
	env.local = [self localPlayer];
	env.game = _game.get();
	env.hud = _hudOn ? &_hud : nullptr;
	env.hud_visible = [self hudVisible];
	env.score_window = _scoreWin ? _scoreWin == 2 : (_ctwBuild || _mp || getenv("RON_SCORE_WIN") != nullptr);  // StatWin (docs/ui-port.md section 17.4)
	env.game_over_line = _gameMenu != 2;
	__weak MapRenderer* weakSelf = self;
	env.project = [weakSelf](float wx, float wy, float& x, float& y) {
		MapRenderer* me = weakSelf;
		if (!me) return false;
		const float z = std::max(0.0f, me->_terrain.sample_height(std::clamp(wx, 0.0f, me->_mesh.width), std::clamp(wy, 0.0f, me->_mesh.height)));
		const simd_float2 sp = [me screenOf:simd_make_float3(wx, wy, z)] / me->_uiScale;
		x = sp.x, y = sp.y;
		return true;
	};
	return env;
}

- (scriptview::Callbacks)scriptCallbacks {
	scriptview::Callbacks cb;
	__weak MapRenderer* weakSelf = self;
	cb.play_voice = [weakSelf](const std::string& path) {
		MapRenderer* me = weakSelf;
		if (me && me->_audio) me->_audio->play_voice(path);
	};
	cb.stop_voice = [weakSelf]() {
		MapRenderer* me = weakSelf;
		if (me && me->_audio) me->_audio->stop_voice();
	};
	cb.music_mood = [weakSelf](const std::string& mood) {
		MapRenderer* me = weakSelf;
		if (me && me->_audio) me->_audio->set_music_mood(mood);
	};
	cb.play_effect = [weakSelf](const std::string& path) {
		MapRenderer* me = weakSelf;
		if (me && me->_audio) me->_audio->play_effect(path);
	};
	cb.camera = [weakSelf](int kind, int x, int y, int zoom) {
		MapRenderer* me = weakSelf;
		if (me) [me cameraRequest:kind x:x y:y zoom:zoom];
	};
	cb.clear_selection = [weakSelf]() {
		MapRenderer* me = weakSelf;
		if (!me) return;
		me->_selected.clear(), me->_selectedBuilds.clear();
		[me selectionChanged];
	};
	cb.select_object = [weakSelf](int who, int o) {  // select_object @9f0760: the selection becomes that one unit or building
		MapRenderer* me = weakSelf;
		if (me) [me selectScriptObject:who o:o];
	};
	cb.pause = [weakSelf](bool on) {
		MapRenderer* me = weakSelf;
		if (me && !me->_headless && !me->_mp) me->_paused = on;
	};
	cb.highlight_option = [weakSelf](int option) {
		MapRenderer* me = weakSelf;
		if (me) me->_hiliteOverride = option;
	};
	cb.screenshot = [](const std::string& file) { std::printf("script: take_screenshot \"%s\" (not written)\n", file.c_str()); };
	return cb;
}

// select_object: the one object becomes the HUD's selection (a unit, or a building: the two are never mixed). RON_CAMERA_EVENT
// (harnessEndings) posts the script event that reaches this method.
- (void)selectScriptObject:(int)who o:(int)o {
	if (!_game) return;
	_selected.clear(), _selectedBuilds.clear();
	if ([self unitView:who o:o]) {
		_selected.push_back({who, o});
	} else if ([self buildView:who o:o]) {
		_selectedBuilds.push_back({who, o});
		[self syncSimBuildings];
	}
	[self selectionChanged];
	if (_scriptLog) std::printf("  select_object %d:%d -> %zu unit(s), %zu building(s) selected\n", who, o, _selected.size(), _selectedBuilds.size());
}

// After each Game::do_frame: the presentation queue, the local game-over, and --script-log output.
- (void)consumeScript {
	if (!_game) return;
	const ron::sim::ScriptPresentation* p = [self pres];
	std::unique_ptr<ron::sim::ScriptPresentation> mpFiltered = [self mpFilteredPresentation:p];
	if (mpFiltered) p = mpFiltered.get();
	const scriptview::Env env = [self scriptEnv];
	const scriptview::Callbacks cb = [self scriptCallbacks];
	if (p && _scriptLog) [self logScript:*p];
	if (p && _sviewOn) _sview.consume(*p, env, cb);
	if (p && !p->highlight_active) _hiliteOverride = -1;
	for (; _gameEventsSeen < _game->events.size(); _gameEventsSeen++) {
		const ron::sim::GameEvent& ev = _game->events[_gameEventsSeen];
		const int me = [self localPlayer];
		const bool won = ev.kind == 3 && ev.who == me, lost = ev.kind == 2 && ev.who == me;
		if (!won && !lost) continue;
		std::string msg = p ? (won ? p->victory_message : p->defeat_message) : std::string();
		if (lost && msg.empty()) msg = [self defeatReason];
		std::printf("frame %d: %s%s%s\n", _game->frame, won ? "victory" : "defeat", msg.empty() ? "" : ": ", msg.c_str());
		if (_sviewOn) _sview.game_over(won, msg, env, cb);
		if (_audio) {
			if (won) _audio->music_win();
			else _audio->music_lose();
		}
		if (_ctwBuild && _ctwOutcome < 0) {  // Conquer the World: back to the campaign after a short look at the result
			_ctwOutcome = won ? 1 : 0;
			_ctwEndFrames = 75;
		}
	}
	// Game::process_end_game: the frame that decided the game (or the allied victory, which leaves the simulation running) raises
	// the end-game window and clears the pending bit. A campaign battle keeps its own hand-over (_ctwEndFrames).
	if (_game->victory_checked) {
		_game->victory_checked = false;
		if (!_ctwBuild && !_overOffered && !_gameMenu) [self openGameMenu:2];
	}
	// switch_player_control: the viewed seat changed under the script; the sim's clear_selection event dropped the selection, the
	// fog, minimap and HUD read the seat each frame, so only the notifier's history has to follow.
	if (_seatSeen != 0 && _seatSeen != _game->local_play + 1) std::printf("frame %d: viewing seat %d\n", _game->frame, _game->local_play);
	_seatSeen = _game->local_play + 1;
	if (_ctwBuild && _ctwOutcome >= 0 && _ctwEndFrames >= 0 && --_ctwEndFrames < 0 && self.onCtwEnd) {
		auto cb = self.onCtwEnd;
		self.onCtwEnd = nil;
		cb(_ctwOutcome);
	}
}

// Why the local leader lost, from the install's own strings (Leader::defeat's DefeatIndex, or the winner's victory type).
- (std::string)defeatReason {
	if (!_game || !_tables) return {};
	const int me = [self localPlayer];
	if (me < 0 || me >= int(_game->leaders.size())) return {};
	auto s = [&](size_t i, const char* expect, const char* fallback) -> std::string {
		return i < _tables->translated().size() && _tables->translated()[i].find(expect) != std::string::npos ? _tables->translated()[i] : fallback;
	};
	const ron::sim::Leader& L = _game->leaders[size_t(me)];
	switch (L.defeat_type) {
		case 0: return s(1134, "conquered", "Your nation has been conquered!");
		case 1: case 2: return s(1136, "capital", "You lost your capital!");
		case 5:
			for (const ron::sim::Leader& W : _game->leaders) {
				if (!(W.flags & 0x20)) continue;
				switch (W.victory_type) {
					case 1: return s(1180, "wonder", "You lost to a wonder victory.");
					case 2: return s(1182, "territory", "You lost to a territory victory.");
					case 3: return s(1183, "technology", "You lost to a technology victory.");
					case 4: return s(1184, "score", "You lost to a score victory.");
					case 5: return s(1185, "economic", "You lost to an economic victory.");
					default: break;
				}
			}
			return {};
		case 8: return s(1189, "ARMAGEDDON", "ARMAGEDDON!");
		default: return {};
	}
}

- (void)logScript:(const ron::sim::ScriptPresentation&)p {
	static const char* names[] = {"game message", "message", "instruction", "feedback", "chat", "notice", "bubble",
	                              "popup", "sound", "stop sound", "music mood", "ping", "clear pings", "camera",
	                              "clear messages", "goal dialog", "screenshot", "pause", "results"};
	for (const ron::sim::ScriptEvent& e : p.events) {
		if (e.seq <= _logSeq) continue;
		_logSeq = e.seq;
		std::printf("  f%-5d SCRIPT %s", e.frame, names[std::min<int>(e.kind, 18)]);
		if (!e.text.empty()) std::printf(" \"%s\"", e.text.c_str());
		if (!e.text2.empty()) std::printf(" [%s]", e.text2.c_str());
		if (e.kind == ron::sim::ScriptEvent::kPing || e.kind == ron::sim::ScriptEvent::kCamera) std::printf(" at (%d, %d)", e.x, e.y);
		std::printf("\n");
	}
	for (size_t k = _logObjectives; k < p.objectives.size(); k++)
		std::printf("  f%-5d SCRIPT objective added '%s': \"%s\"\n", p.objectives[k].frame, p.objectives[k].id.c_str(), p.objectives[k].text.c_str());
	_logObjectives = p.objectives.size();
	for (const ron::sim::ScriptObjective& o : p.objectives) {
		if (!o.completed) continue;
		bool known = false;
		for (const auto& d : _logDone) known = known || (d.first == o.id && d.second == o.frame);
		if (known) continue;
		_logDone.push_back({o.id, o.frame});
		std::printf("  f%-5d SCRIPT objective complete '%s'\n", o.completed, o.id.c_str());
	}
}

// A headless layout pass of the script layers (fonts need a UI frame): fills the objectives box for get_objectives_*.
- (void)layoutScriptView {
	if (_uiW <= 0) return;
	_ui.begin(_uiW, _uiH, _uiScale > 0 ? _uiScale : 1);
	_sview.draw(_ui, [self pres], [self scriptEnv]);
}

// Camera moves from the scripts: pan_to (panning = 1), set_loc (jump), to_zoom_level (one level per check_zoom).
- (void)cameraRequest:(int)kind x:(int)x y:(int)y zoom:(int)zoom {
	using CR = ron::sim::CameraRequest;
	if ((kind == CR::kPan || kind == CR::kPanZoom) && x >= 0) {
		_pan.panning = 1, _pan.to_x = x, _pan.to_y = y, _pan.from_x = int(_lookAt.x), _pan.from_y = int(_lookAt.y);
	}
	if (kind == CR::kJump && x >= 0) {
		_pan.panning = 0;
		[self lookAtX:float(x) y:float(y)];
	}
	if ((kind == CR::kZoom || kind == CR::kPanZoom) && zoom >= 0) _pan.zooming = true, _pan.to_zoom = std::clamp(zoom, 0, 6);
}

// Camera::check_zoom @00843c30, run every 33 ms of the graphic clock: zoom one level toward to_zoom_level; pan by
// max(0x300, distance / 2) per axis toward the target, snapping once within 0x300 on both axes (scroll_lock_flags & 8
// blocks the moves).
- (void)cameraStep:(double)dtMs {
	_camAccumMs += dtMs;
	const ron::sim::ScriptPresentation* p = [self pres];
	const bool locked = p && (p->scroll_lock_flags & 8);
	while (_camAccumMs >= 33.0) {
		_camAccumMs -= 33.0;
		if (_pan.zooming) {
			if (_zoom < _pan.to_zoom) _zoom++;
			else if (_zoom > _pan.to_zoom) _zoom--;
			else _pan.zooming = false;
		}
		if (_pan.panning) {
			const int lx = int(_lookAt.x), ly = int(_lookAt.y);
			const int dx = _pan.to_x - lx, dy = _pan.to_y - ly;
			if (std::abs(dx) < 0x300 && std::abs(dy) < 0x300) {
				if (!locked) [self lookAtX:float(_pan.to_x) y:float(_pan.to_y)];
				_pan.panning = 0;
				continue;
			}
			const int sx = std::max(0x300, std::abs(dx) / 2), sy = std::max(0x300, std::abs(dy) / 2);
			const int nx = std::abs(dx) < 0x300 ? lx : lx + (dx > 0 ? sx : -sx);
			const int ny = std::abs(dy) < 0x300 ? ly : ly + (dy > 0 ? sy : -sy);
			if (!locked) [self lookAtX:float(std::clamp(nx, 0, int(_mesh.width))) y:float(std::clamp(ny, 0, int(_mesh.height)))];
		}
	}
}

// popup_dialog / popup_choice: in a window, a nested event loop draws the popup and waits for a button (the script is
// suspended inside Game::do_frame, as in BasePop::exec); headless runs answer button 0.
- (int)runPopup:(const ron::sim::ScriptEvent&)e {
	std::printf("script popup: \"%s\"\n", e.text.c_str());
	if (_headless && _sviewOn && getenv("RON_POPUP_KEEP")) _sview.open_popup(e);  // snapshot check of the popup's look
	if (!_sviewOn || _headless || !self.view.window) return 0;
	_sview.open_popup(e);
	_inPopup = YES;
	_popupResult = -1;
	while (_popupResult < 0 && self.view.window.isVisible) {
		@autoreleasepool {
			NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:1.0 / 60]
			                                    inMode:NSDefaultRunLoopMode dequeue:YES];
			if (ev) [NSApp sendEvent:ev];
			[self.view draw];
		}
	}
	_inPopup = NO;
	_sview.close_popup();
	return std::max(0, _popupResult);
}

// Mouse on the modal windows (popup buttons, the goal dialog's OK). Returns YES when consumed.
- (BOOL)modalMouse:(CGPoint)p up:(BOOL)up {
	if (_sview.popup_open()) {
		const int b = _sview.popup_button_at(int(p.x), int(p.y));
		if (!up) _sview.popup().pressed = b;
		else if (b >= 0 && b == _sview.popup().pressed) _popupResult = b;
		if (up) _sview.popup().pressed = -1;
		return YES;
	}
	if (_sview.goal_dialog_open()) {
		if (up && _sview.goal_dialog_close_at(int(p.x), int(p.y))) _sview.close_goal_dialog();
		return YES;
	}
	return NO;
}

// ScenarioData::draw_components @00997920: each add_flag is the ScenarioFlag piece (first_flag_piece + 2) at the tile
// centre on the terrain, scale (3, -3, 3), angle 45, leader 0's colour, its "Banner Wave" clock offset by the position
// ((frame + y * 17 + x * 13) % frames) * 67 ms. The banner is single-sided: with this matrix (the units' mirrored
// basis) it faces the camera only with the cull mode left unflipped (checked by snapshot), as the units do.
- (void)encodeFlags:(id<MTLRenderCommandEncoder>)enc {
	const ron::sim::ScriptPresentation* p = [self pres];
	if (!p || p->flags.empty()) return;
	const int team = _game && !_game->leaders.empty() ? std::clamp(_game->leaders[0].color, 0, 8) : 0;
	const uint8_t* tc = kUnitTeamColors[team];
	const simd_float4 tint = simd_make_float4(tc[0], tc[1], tc[2], 255) / 255.0f;
	const float s = 3, a = 45 * float(M_PI) / 180.0f, c = std::cos(a), sn = std::sin(a);
	const char* piece = "ScenarioFlag";
	for (const auto& f : p->flags) {
		const float x = float(f[0] * 192 + 96), y = float(f[1] * 192 + 96);
		const float z = std::max(0.0f, _terrain.sample_height(x, y));
		const simd_float4x4 world = simd_matrix(simd_make_float4(c * s, sn * s, 0, 0), simd_make_float4(sn * s, -c * s, 0, 0),
		                                        simd_make_float4(0, 0, s, 0), simd_make_float4(x, y, z, 1));
		const double ms = double((_game ? _game->frame : 0) + int(y) * 17 + int(x) * 13) * 67.0;
		if (!_units.draw_ammo(enc, piece, ms, world, false, tint)) {
			static bool warned = false;
			if (!warned) std::fprintf(stderr, "flag: the ScenarioFlag piece has no model\n"), warned = true;
		}
	}
}

// MessageWin::add_event's ANNOUNCE_CIRCLE: a TRANSPARENT_WHITE ring on the terrain shrinking over 8 ticks.
- (void)encodeAnnounce:(id<MTLRenderCommandEncoder>)enc {
	if (!_sviewOn || _sview.announces().empty()) return;
	static const ron::RenderState& circle_rs = *ron::find_render_state("Circle Blend");
	std::vector<ron::ColorVertex> verts;
	std::vector<uint32_t> idx;
	const int kSeg = 48, tick = int(_timeMs / 66.0);
	for (const scriptview::View::Announce& an : _sview.announces()) {
		const float r1 = 192.0f * float(8 - std::clamp(tick - an.tick, 0, 7)), r0 = r1 - 40;
		const uint32_t base = uint32_t(verts.size());
		for (int k = 0; k <= kSeg; k++) {
			const float a = float(k) / kSeg * 2 * float(M_PI), c = std::cos(a), s = std::sin(a);
			for (float r : {std::max(0.0f, r0), r1}) {
				const float x = an.x + c * r, y = an.y + s * r;
				verts.push_back({x, y, std::max(0.0f, _terrain.sample_height(x, y)) + 4, 0, 0, 1, 0, 0, 255, 255, 255, 64});
			}
		}
		for (uint32_t k = 0; k < uint32_t(kSeg); k++) {
			const uint32_t a = base + k * 2;
			idx.insert(idx.end(), {a, a + 1, a + 2, a + 2, a + 1, a + 3});
		}
	}
	rm::DrawUniforms du = rm::draw_uniforms(circle_rs, matrix_identity_float4x4);
	_overlayStates.bind(enc, circle_rs, rm::VertexKind::Colored);
	[enc setCullMode:MTLCullModeNone];
	id<MTLBuffer> vb = [_device newBufferWithBytes:verts.data() length:verts.size() * sizeof(ron::ColorVertex) options:MTLResourceStorageModeShared];
	id<MTLBuffer> ib = [_device newBufferWithBytes:idx.data() length:idx.size() * 4 options:MTLResourceStorageModeShared];
	[enc setVertexBuffer:vb offset:0 atIndex:0];
	[enc setVertexBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentTexture:_white atIndex:0];
	[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:idx.size() indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0];
}

// ---------------------------------------------------------------- cursors (docs/ui-spec.md 7)

- (NSCursor*)cursorWithId:(int)cid {
	auto it = _cursors.find(cid);
	if (it != _cursors.end()) return it->second;
	return _cursors[cid] = ron_game_cursor(*_files, cid);
}

// The context cursor (Console::execute_at_cursor preview): Move To while targeting, Attack over an enemy with own units
// selected, else Main.
- (NSCursor*)cursorAt:(CGPoint)p {
	int cid = ron::kCursorMain;
	if (!([self hudVisible] && _hud.captures(int(p.x), int(p.y))) && _game) {
		if (_targeting == hud::kOptMoveTo) {
			cid = ron::kCursorMoveTo;
		} else if (!_selected.empty() && _placeType < 0) {
			int32_t who = -1, o = -1, type = -1;
			const int me = [self localPlayer];
			if ([self pickAt:p who:who o:o type:type] && who != me && ron::sim::is_enemy(*_game, me, who)) cid = ron::kCursorAttack;
		}
	}
	return [self cursorWithId:cid];
}

@end

// The game window's view: events go to the front end while it is up, else to the game.
@interface MapView : MTKView
@property(nonatomic, weak) MapRenderer* renderer;
@property(nonatomic, weak) FrontEnd* frontEnd;
@end

@implementation MapView {
	NSTrackingArea* _tracking;
}
- (BOOL)acceptsFirstResponder {
	return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
	return YES;
}
- (void)keyDown:(NSEvent*)event {
	if (self.frontEnd) {
		[self.frontEnd keyDown:event];
		return;
	}
	[self.renderer scriptKey:event down:YES];
	[self.renderer handleKey:event];
}
- (void)keyUp:(NSEvent*)event {
	if (!self.frontEnd) [self.renderer scriptKey:event down:NO];
}
- (void)flagsChanged:(NSEvent*)event {
	if (!self.frontEnd) [self.renderer scriptFlags:event.modifierFlags];
}
- (void)mouseEntered:(NSEvent*)event {
	if (!self.frontEnd) [self.renderer setMouseInside:YES];
}
- (void)mouseExited:(NSEvent*)event {
	if (!self.frontEnd) [self.renderer setMouseInside:NO];
}
// Events go to the renderer in UI pixels: view points with a top-left origin (the HUD's coordinate space).
- (CGPoint)uiPoint:(NSEvent*)event {
	const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
	return CGPointMake(p.x, self.bounds.size.height - p.y);
}
- (NSCursor*)cursorAt:(CGPoint)p {
	if (self.frontEnd) return [self.frontEnd cursor];
	if (self.renderer) return [self.renderer cursorAt:p];
	return [NSCursor arrowCursor];
}
- (void)updateTrackingAreas {
	[super updateTrackingAreas];
	if (_tracking) [self removeTrackingArea:_tracking];
	_tracking = [[NSTrackingArea alloc] initWithRect:self.bounds
	                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect | NSTrackingCursorUpdate
	                                           owner:self
	                                        userInfo:nil];
	[self addTrackingArea:_tracking];
}
- (void)cursorUpdate:(NSEvent*)event {
	[[self cursorAt:[self uiPoint:event]] set];
}
- (void)mouseMoved:(NSEvent*)event {
	const CGPoint p = [self uiPoint:event];
	if (self.frontEnd) [self.frontEnd mouseMoved:p];
	else [self.renderer uiMouseMoved:p];
	[[self cursorAt:p] set];
}
- (void)mouseDown:(NSEvent*)event {
	if (self.frontEnd) [self.frontEnd mouseDown:[self uiPoint:event] clicks:event.clickCount];
	else [self.renderer uiMouseDown:[self uiPoint:event] right:NO modifiers:event.modifierFlags clicks:event.clickCount];
}
- (void)mouseDragged:(NSEvent*)event {
	if (self.frontEnd) [self.frontEnd mouseDragged:[self uiPoint:event]];
	else [self.renderer uiMouseDragged:[self uiPoint:event] right:NO delta:CGPointMake(event.deltaX, event.deltaY)];
}
- (void)mouseUp:(NSEvent*)event {
	if (self.frontEnd) [self.frontEnd mouseUp:[self uiPoint:event]];
	else [self.renderer uiMouseUp:[self uiPoint:event] right:NO modifiers:event.modifierFlags];
	[[self cursorAt:[self uiPoint:event]] set];
}
- (void)rightMouseDown:(NSEvent*)event {
	if (!self.frontEnd) [self.renderer uiMouseDown:[self uiPoint:event] right:YES modifiers:event.modifierFlags clicks:event.clickCount];
}
- (void)rightMouseDragged:(NSEvent*)event {
	if (!self.frontEnd) [self.renderer uiMouseDragged:[self uiPoint:event] right:YES delta:CGPointMake(event.deltaX, event.deltaY)];
}
- (void)rightMouseUp:(NSEvent*)event {
	if (!self.frontEnd) [self.renderer uiMouseUp:[self uiPoint:event] right:YES modifiers:event.modifierFlags];
}
- (void)scrollWheel:(NSEvent*)event {
	if (self.frontEnd) {
		const CGFloat dy = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY / 24.0 : event.scrollingDeltaY;
		[self.frontEnd scrollWheel:dy at:[self uiPoint:event]];
		return;
	}
	[self.renderer uiScrollWheel:event at:[self uiPoint:event]];
}
- (void)magnifyWithEvent:(NSEvent*)event {
	if (!self.frontEnd) [self.renderer uiMagnify:float(event.magnification) at:[self uiPoint:event]];
}
@end

// The app: one window whose view shows either the front end or a game; Play and Return to Main Menu swap them in place.
@interface MapAppDelegate : NSObject <NSApplicationDelegate, FrontEndHost>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) MapView* view;
@property(nonatomic, strong) MapRenderer* renderer;  // the running game (nil while the menu is up)
@property(nonatomic, strong) FrontEnd* menu;         // the front end (nil during a game)
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, copy) NSString* gameDir;
@property(nonatomic, copy) NSString* startScreen;
- (void)setCtwSession:(std::shared_ptr<ctwui::Session>)s;
@end

@implementation MapAppDelegate {
	std::shared_ptr<ctwui::Session> _ctwSession;  // the Conquer the World campaign in progress (outlives the menu and the battles)
}
// Everything is created once the app has finished launching, inside [NSApp run]: a window ordered front (or a modal
// session run) before that is torn down during launch, and the app then quits as its last window closed.
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
	// Come to the front even when started from Finder, `open` or a terminal (macOS 14 restricts activateIgnoringOtherApps).
	if (@available(macOS 14.0, *)) [NSApp activate];
	else [NSApp activateIgnoringOtherApps:YES];
	if (!self.renderer) {
		NSString* dir = self.gameDir;
		if (!launcher::looks_like_install(dir)) dir = launcher::saved_or_guessed_install();
		if (!launcher::looks_like_install(dir)) dir = launcher::ask_for_install(nil);
		if (!dir) {
			[NSApp terminate:nil];
			return;
		}
		launcher::remember_install(dir);
		self.gameDir = dir;
	}
	NSRect frame = NSMakeRect(0, 0, 1440, 900);
	if (NSScreen* screen = NSScreen.mainScreen) {
		const NSRect vis = screen.visibleFrame;
		frame.size.width = std::min<CGFloat>(frame.size.width, std::floor(vis.size.width * 0.92));
		frame.size.height = std::min<CGFloat>(frame.size.height, std::floor(vis.size.height * 0.92));
	}
	self.window = [[NSWindow alloc] initWithContentRect:frame
	                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                                                    NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
	                                            backing:NSBackingStoreBuffered
	                                              defer:NO];
	self.window.releasedWhenClosed = NO;
	self.window.title = @"OpenRiseOfNations";
	self.window.contentMinSize = NSMakeSize(640, 480);
	self.view = [[MapView alloc] initWithFrame:frame device:self.device];
	self.view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
	self.view.framebufferOnly = YES;
	self.window.contentView = self.view;
	if (self.renderer) {
		[self attachGame:self.renderer];
	} else if (![self showMenu:self.startScreen]) {
		[NSApp terminate:nil];
		return;
	}
	[self.window center];
	[self.window makeKeyAndOrderFront:nil];
	[self.window makeFirstResponder:self.view];
	if (@available(macOS 14.0, *)) [NSApp activate];
	else [NSApp activateIgnoringOtherApps:YES];
	[self.window orderFrontRegardless];
	if (prefs::get().has("fullscreen") && prefs::get().fullscreen) [self.window toggleFullScreen:nil];  // the Options page's Graphics pane
	if (getenv("RON_FLOW_TEST")) [self flowTest:0];
	if (getenv("RON_MP_APP_TEST")) [self mpAppTest:0];
	if (getenv("RON_CTW_FLOW_TEST") && _ctwSession) [self ctwFlowTest:0];
}

// Debugging aid (RON_CTW_FLOW_TEST=1 with --ctw CAMPAIGN): the strategic map -> Play (the first attack) -> the battle opens in
// the window -> it is won (the game-over callback the real end triggers) -> the front end is back on the map with the node taken.
- (void)ctwFlowTest:(int)step {
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		ctwui::Session* s = _ctwSession.get();
		std::printf("ctw flow step %d: %s\n", step, self.renderer ? "game" : self.menu ? "menu" : "?");
		std::fflush(stdout);
		switch (step) {
			case 0: {
				ron::sim::ctw::LinkChoice l;
				std::string err;
				if (!ctwcli::pick_link(s->game, "auto", &l, &err)) {
					std::printf("ctw flow: %s\n", err.c_str());
					[NSApp terminate:nil];
					return;
				}
				s->game.cur_link = l;
				s->game.cur_style = s->game.pick_style(l);
				s->battle = s->game.make_battle(l, s->game.cur_style);
				s->battle_is_defense = false;
				s->battle_pending = true;
				std::printf("ctw flow: attacking %s\n", s->game.node_name(l.to).c_str());
				[self frontEndPlayCtwBattle:_ctwSession];
				break;
			}
			case 1:
				if (self.renderer.onCtwEnd) self.renderer.onCtwEnd(1);
				break;
			case 2: {
				const auto& l = s->battle.link;
				std::printf("ctw flow: %s is now owned by %s; news: %s\n", s->game.node_name(l.to).c_str(),
				            s->game.nodes[size_t(l.to)].owner >= 0 ? s->game.tribe_name(s->game.nodes[size_t(l.to)].owner).c_str() : "nobody",
				            s->game.news.empty() ? "" : s->game.news.back().text.c_str());
				[NSApp terminate:nil];
				return;
			}
		}
		[self ctwFlowTest:step + 1];
	});
}

// Debugging aid (RON_FLOW_TEST=1): menu -> Play (tutorial 1) -> game menu "Return to Main Menu" -> Play (skirmish) ->
// back -> quit, a few seconds apart, printing each step: checks the in-window swaps without clicking.
- (void)flowTest:(int)step {
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		std::printf("flow test step %d: %s\n", step, self.renderer ? "game" : self.menu ? "menu" : "?");
		std::fflush(stdout);
		switch (step) {
			case 0: [self frontEndPlay:@"scenario/Tutorial1/tut1.scx" difficulty:-1]; break;
			case 1: [self.renderer openGameMenu:1]; break;
			case 2: if (self.renderer.onExit) self.renderer.onExit(NO); break;
			case 3: [self frontEndPlay:@"scenario/Custom/skirmish/skirmish.scx" difficulty:3]; break;
			case 4: if (self.renderer.onExit) self.renderer.onExit(NO); break;
			default: [NSApp terminate:nil]; return;
		}
		[self flowTest:step + 1];
	});
}

// Window test of the multiplayer screens through the real app delegate (docs/ui-port.md 13.7). RON_MP_APP_TEST=PREFIX, RON_MP_APP_SCRIPT = steps separated by ';':
//   w:MS wait | c:NAME click a control of the front end (host, join, load, scenario, ctw, start, back: FrontEnd mpDemoPoint) through its mouse handlers
//   snap:NAME snapshot of the window's current content to PREFIX_NAME.png | st print the state | ctw:CAMPAIGN[:LINK] a campaign session for the host, its map open
//   ctwplay the campaign map's Play button | ctwend:N the battle in the window ends with outcome N (1 won) | quit.
// Only the host's window is driven; the other machines are headless `ronmap --join` processes.
- (void)mpAppTest:(NSUInteger)index {
	static NSArray<NSString*>* steps;
	if (!steps) {
		// a window started from a script is not visible to the display link (occluded, other Space): the test draws it itself
		[NSTimer scheduledTimerWithTimeInterval:0.02 repeats:YES block:^(NSTimer*) { [self.view draw]; }];
		const char* sc = getenv("RON_MP_APP_SCRIPT");
		steps = [(sc ? @(sc) : @"st") componentsSeparatedByString:@";"];
	}
	if (index >= steps.count) {
		[NSApp terminate:nil];
		return;
	}
	const std::string step = steps[index].UTF8String;
	const size_t colon = step.find(':');
	const std::string cmd = step.substr(0, colon), arg = colon == std::string::npos ? "" : step.substr(colon + 1);
	double delay = 0.4;
	const char* prefix = getenv("RON_MP_APP_TEST");
	const CGSize ds = self.view.drawableSize;
	const float scale = float(self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 1);
	if (cmd == "w") {
		delay = std::atoi(arg.c_str()) / 1000.0;
	} else if (cmd == "c") {
		FrontEnd* m = self.menu;
		const CGPoint p = m ? [m mpDemoPoint:@(arg.c_str())] : CGPointZero;
		if (CGPointEqualToPoint(p, CGPointZero)) {
			std::printf("[app test] click %s: that control is not on the screen (%s)\n", arg.c_str(), m ? "front end" : "a game is running");
		} else {
			[m mouseMoved:p];
			[m mouseDown:p clicks:1];
			[m mouseUp:p];
			std::printf("[app test] clicked %s at %.0f,%.0f\n", arg.c_str(), p.x, p.y);
		}
	} else if (cmd == "snap") {
		NSString* path = [NSString stringWithFormat:@"%s_%s.png", prefix, arg.c_str()];
		const BOOL ok = self.menu ? [self.menu snapshotTo:path width:int(ds.width) height:int(ds.height) scale:scale]
		                          : self.renderer ? [self.renderer snapshotTo:path width:int(ds.width) height:int(ds.height) scale:scale] : NO;
		std::printf("[app test] snapshot %s %s\n", path.UTF8String, ok ? "written" : "FAILED");
	} else if (cmd == "st") {
		if (self.menu) {
			std::printf("[app test] front end: %s\n", [self.menu mpDemoState].UTF8String);
		} else if (self.renderer) {
			[self.renderer updateTitle];
			std::printf("[app test] game window: %s%s\n", self.window.title.UTF8String, self.renderer.multiplayer ? " (multiplayer)" : "");
		}
	} else if (cmd == "ctw") {
		const size_t c2 = arg.find(':');
		ron::GameFiles files(self.gameDir.UTF8String);
		ron::sim::ctw::ConquestSetup cs;
		cs.campaign = ctwcli::resolve_campaign(files, arg.substr(0, c2));
		cs.seed = 1;
		std::string err;
		std::shared_ptr<ctwui::Session> session = cs.campaign.empty() ? nullptr : ctwui::new_session(self.gameDir.UTF8String, cs, &err);
		ron::sim::ctw::LinkChoice link;
		if (!session || !ctwcli::pick_link(session->game, c2 == std::string::npos ? "auto" : arg.substr(c2 + 1), &link, &err)) {
			std::printf("[app test] ctw: %s\n", session ? err.c_str() : "no campaign");
		} else {
			ron::sim::ctw::ConquestGame& cg = session->game;
			cg.cur_link = link;
			cg.cur_style = cg.pick_style(link);
			session->battle = cg.make_battle(link, cg.cur_style);
			session->battle_is_defense = false;
			session->battle_pending = true;
			_ctwSession = session;
			[self.menu ctwShowSession:session sub:@"" select:nil];
			std::printf("[app test] campaign %s: %s attacks %s; map open\n", cs.campaign.c_str(), cg.tribe_name(link.attacker).c_str(), cg.node_name(link.to).c_str());
		}
	} else if (cmd == "ctwplay") {
		[self.menu mpDemoCtwPlay];
	} else if (cmd == "ctwend") {
		if (self.renderer.onCtwEnd) self.renderer.onCtwEnd(std::atoi(arg.c_str()));
		else std::printf("[app test] ctwend: no campaign battle in the window\n");
	} else if (cmd == "quit") {
		[NSApp terminate:nil];
		return;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		std::fflush(stdout);
		[self mpAppTest:index + 1];
	});
}

- (void)alert:(NSString*)title text:(NSString*)text {
	NSAlert* a = [NSAlert new];
	a.messageText = title;
	a.informativeText = text ?: @"Unknown error";
	[a runModal];
}

// The front end in the window (screen nil: the one remembered from last time).
- (BOOL)showMenu:(NSString*)screen {
	if (!self.menu) {
		NSString* err = nil;
		self.menu = [[FrontEnd alloc] initWithDevice:self.device gameDir:self.gameDir error:&err];
		if (!self.menu) {
			[self alert:@"Could not open the menu" text:err];
			return NO;
		}
	}
	FrontEnd* m = self.menu;
	self.view.renderer = nil;
	self.view.frontEnd = m;
	self.view.delegate = m;
	m.view = self.view;
	m.host = self;
	self.renderer = nil;  // the game (its sounds stop with it)
	[m startInteractive];
	[m showScreen:screen ?: ([NSUserDefaults.standardUserDefaults stringForKey:launcher::kDefScreen] ?: @"main")];
	self.window.title = @"OpenRiseOfNations";
	[self.window makeFirstResponder:self.view];
	[[m cursor] set];
	return YES;
}

- (void)attachGame:(MapRenderer*)r {
	self.renderer = r;
	r.view = self.view;
	self.view.frontEnd = nil;
	self.view.renderer = r;
	self.view.delegate = r;
	__weak MapAppDelegate* weakSelf = self;
	// Swapped on the next turn of the run loop: the game's own event handler is still on the stack.
	r.onExit = ^(BOOL quit) {
		dispatch_async(dispatch_get_main_queue(), ^{
			if (quit) [NSApp terminate:nil];
			else [weakSelf showMenu:nil];
		});
	};
	r.onPlayRecording = ^(NSString* path) {   // View Playback of the end-game window
		dispatch_async(dispatch_get_main_queue(), ^{
			[weakSelf playRecordingAt:path inGame:YES];
		});
	};
	[r updateTitle];
	[r startInteractive];
	[self.window makeFirstResponder:self.view];
}

- (void)frontEndPlay:(NSString*)scenario difficulty:(int)difficulty {
	[self.menu setBusy:@"Loading…"];
	[self.view draw];
	dispatch_async(dispatch_get_main_queue(), ^{
		g_difficulty = difficulty;
		NSString* err = nil;
		MapRenderer* r = [[MapRenderer alloc] initWithDevice:self.device gameDir:self.gameDir scenario:scenario simulate:YES seed:12345 error:&err];
		if (!r) {
			[self.menu setBusy:nil];
			[self.menu showError:[NSString stringWithFormat:@"Could not load the scenario: %@", err ?: @"unknown error"]];
			return;
		}
		const CGSize ds = self.view.drawableSize;
		const float scale = float(self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 1);
		[r setUiWidth:int(ds.width) height:int(ds.height) scale:scale];
		[self.menu stopAudio];
		self.view.frontEnd = nil;
		self.menu = nil;  // its backdrop textures are large; the menu is rebuilt on the way back
		[self attachGame:r];
	});
}

// A random-map skirmish from the setup screen (sim/newgame.hpp): the map is generated, then the game opens as for a scenario.
- (void)frontEndPlayRandom:(const ron::sim::RandomGameSetup&)setup {
	const ron::sim::RandomGameSetup copy = setup;
	[self.menu setBusy:@"Generating the map…"];
	[self.view draw];
	dispatch_async(dispatch_get_main_queue(), ^{
		NSString* err = nil;
		MapRenderer* r = [MapRenderer randomGameWithDevice:self.device gameDir:self.gameDir setup:&copy spec:nil error:&err];
		if (!r) {
			[self.menu setBusy:nil];
			[self.menu showError:[NSString stringWithFormat:@"Could not create the map: %@", err ?: @"unknown error"]];
			return;
		}
		const CGSize ds = self.view.drawableSize;
		const float scale = float(self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 1);
		[r setUiWidth:int(ds.width) height:int(ds.height) scale:scale];
		[self.menu stopAudio];
		self.view.frontEnd = nil;
		self.menu = nil;
		[self attachGame:r];
	});
}

// Recorded Games (recplay.hpp): the recording at `path` from its first frame, in the game window. A failure from the menu shows on the menu; from the
// end-game window of a game that was just recorded (`inGame`) it is an alert and the game stays.
- (void)frontEndPlayRecording:(NSString*)path {
	[self.menu setBusy:@"Loading the recording…"];
	[self.view draw];
	dispatch_async(dispatch_get_main_queue(), ^{
		[self playRecordingAt:path inGame:NO];
	});
}

- (void)playRecordingAt:(NSString*)path inGame:(BOOL)inGame {
	std::string perr;
	std::shared_ptr<ron::net::Playback> pb(ron::net::Playback::open(path.UTF8String, self.gameDir.UTF8String, perr).release());
	NSString* err = nil;
	MapRenderer* r = nil;
	if (!pb) err = @(perr.c_str());
	else r = [MapRenderer playbackWithDevice:self.device gameDir:self.gameDir playback:pb error:&err];
	if (!r) {
		NSString* text = [NSString stringWithFormat:@"Could not play the recording: %@", err ?: @"unknown error"];
		if (inGame) {
			[self alert:@"Recorded game" text:text];
		} else {
			[self.menu setBusy:nil];
			[self.menu showError:text];
		}
		return;
	}
	const CGSize ds = self.view.drawableSize;
	const float scale = float(self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 1);
	[r setUiWidth:int(ds.width) height:int(ds.height) scale:scale];
	[self.menu stopAudio];
	self.view.frontEnd = nil;
	self.menu = nil;
	[self attachGame:r];
	__weak MapAppDelegate* weakSelf = self;
	r.onExit = ^(BOOL quit) {   // back to the list of recordings
		dispatch_async(dispatch_get_main_queue(), ^{
			if (quit) [NSApp terminate:nil];
			else [weakSelf showMenu:@"recorded"];
		});
	};
}

// A multiplayer game from the lobby (lobby.h): the same map on every machine (the host's seed), played through the lockstep session.
- (void)frontEndPlayMultiplayer:(std::shared_ptr<mp::Room>)room setup:(const ron::sim::RandomGameSetup&)setup save:(std::shared_ptr<std::vector<uint8_t>>)save {
	const ron::sim::RandomGameSetup copy = setup;
	[self.menu setBusy:@"Generating the map…"];
	[self.view draw];
	dispatch_async(dispatch_get_main_queue(), ^{
		NSString* err = nil;
		MapRenderer* r = [MapRenderer randomGameWithDevice:self.device gameDir:self.gameDir setup:&copy spec:nil error:&err];
		if (!r) {
			[self.menu setBusy:nil];
			[self.menu showError:[NSString stringWithFormat:@"Could not create the map: %@", err ?: @"unknown error"]];
			[self.menu showScreen:@"multiplayer"];
			return;
		}
		[r attachMultiplayer:std::make_shared<mp::Runtime>(room) loaded:save];
		const CGSize ds = self.view.drawableSize;
		const float scale = float(self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 1);
		[r setUiWidth:int(ds.width) height:int(ds.height) scale:scale];
		[self.menu stopAudio];
		self.view.frontEnd = nil;
		self.menu = nil;
		[self attachGame:r];
	});
}

// A multiplayer game of a scenario: the room's table carries the scenario and the seats (the scenario's players, humans first).
- (void)frontEndPlayMultiplayerScenario:(std::shared_ptr<mp::Room>)room {
	[self.menu setBusy:@"Loading the scenario…"];
	[self.view draw];
	dispatch_async(dispatch_get_main_queue(), ^{
		mp::ScenarioInfo si;
		NSString* err = nil;
		MapRenderer* r = nil;
		std::shared_ptr<mp::Runtime> rt;
		if (!room->read_scenario(si)) {
			err = @"the host's scenario did not arrive";
		} else if (room->lobby().start_plan().players == 0) {
			err = @"the host's seats did not arrive";
		} else {
			const ron::net::SeatPlan plan = room->lobby().start_plan();
			std::array<bool, 8> humans{};
			std::array<int, 8> who{};
			for (int k = 0; k < 8; k++) who[size_t(k)] = k < si.seats ? si.who[size_t(k)] : k;
			for (int k = 0; k < si.seats && k < 8; k++) humans[size_t(si.who[size_t(k)] & 7)] = plan.peer_of[size_t(k)] >= 0;
			r = [MapRenderer scenarioMultiplayerWithDevice:self.device gameDir:self.gameDir scenario:@(si.rel.c_str()) seed:room->lobby().seed() humans:humans error:&err];
			if (r) {
				rt = std::make_shared<mp::Runtime>(room);
				rt->set_who(who);
				[r attachMultiplayer:rt loaded:nullptr];
			}
		}
		if (!r) {
			[self.menu setBusy:nil];
			[self.menu showError:[NSString stringWithFormat:@"Could not start the scenario: %@", err ?: @"unknown error"]];
			[self.menu showScreen:@"multiplayer"];
			return;
		}
		const CGSize ds = self.view.drawableSize;
		const float scale = float(self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 1);
		[r setUiWidth:int(ds.width) height:int(ds.height) scale:scale];
		[self.menu stopAudio];
		self.view.frontEnd = nil;
		self.menu = nil;
		[self attachGame:r];
	});
}

// A Conquer the World battle of the host's campaign, played by the room (lobby.h "Conquer the World battles in multiplayer").
- (void)frontEndPlayMultiplayerCtw:(std::shared_ptr<mp::Room>)room session:(std::shared_ptr<ctwui::Session>)hostSession {
	[self.menu setBusy:@"Preparing the battle…"];
	[self.view draw];
	dispatch_async(dispatch_get_main_queue(), ^{
		std::string cerr;
		std::shared_ptr<ctwui::Session> session = mp::ctw_battle_session(*room, hostSession, self.gameDir.UTF8String, cerr);
		NSString* err = nil;
		MapRenderer* r = nil;
		if (!session) {
			err = @(cerr.c_str());
		} else {
			if (room->is_host()) _ctwSession = session;
			auto build = [session](const ron::GameFiles& f, const ron::sim::Rules& rr, const ron::sim::AnimSource* a, ron::sim::RandomExtras* x, std::string* e) {
				return session->game.build_battle(f, rr, session->battle, a, x, e);
			};
			r = [MapRenderer ctwBattleWithDevice:self.device gameDir:self.gameDir build:build error:&err];
		}
		if (!r) {
			[self.menu setBusy:nil];
			[self.menu showError:[NSString stringWithFormat:@"Could not prepare the battle: %@", err ?: @"unknown error"]];
			[self.menu showScreen:@"multiplayer"];
			return;
		}
		auto rt = std::make_shared<mp::Runtime>(room);
		[r attachMultiplayer:rt loaded:nullptr];   // recorded like every multiplayer game (Recorded Games folder) when its map is a generated one
		__weak MapAppDelegate* weakSelf = self;
		const bool host = room->is_host();
		__weak MapRenderer* weakR = r;
		r.onCtwEnd = ^(int outcome) {
			dispatch_async(dispatch_get_main_queue(), ^{
				MapAppDelegate* me = weakSelf;
				if (!me) return;
				if (host) mp::ctw_host_result(*room, *session, outcome);   // the host's side is the campaign's: its outcome counts
				[weakR mpClose];
				[me showMenu:host ? @"ctw-map" : @"main"];
				NSString* line = host ? [NSString stringWithFormat:@"The battle is over: %s.", outcome == 1 ? "won" : outcome == 0 ? "lost" : "retreated"]
				                      : @"The battle is over. The host's campaign decides what it means.";
				[me.menu mpResume:room ctw:host ? YES : NO message:line];
			});
		};
		const CGSize ds = self.view.drawableSize;
		const float scale = float(self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 1);
		[r setUiWidth:int(ds.width) height:int(ds.height) scale:scale];
		[self.menu stopAudio];
		self.view.frontEnd = nil;
		self.menu = nil;
		[self attachGame:r];
	});
}

// A guest takes the campaign the host sent after the battle in place of its copy.
- (NSString*)frontEndCtwReceived:(const std::vector<uint8_t>&)campaign outcome:(int)outcome {
	const std::string tmp = std::string(NSTemporaryDirectory().UTF8String) + "ron-ctw-recv-" + std::to_string(getpid()) + ".ctw";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f) return nil;
	std::fwrite(campaign.data(), 1, campaign.size(), f);
	std::fclose(f);
	std::string err;
	std::shared_ptr<ctwui::Session> s = ctwui::load_session(self.gameDir.UTF8String, tmp, &err);
	std::remove(tmp.c_str());
	if (!s) return [NSString stringWithFormat:@"(the campaign copy could not be loaded: %s)", err.c_str()];
	_ctwSession = s;
	(void)outcome;
	return s->game.news.empty() ? nil : @(s->game.news.back().text.c_str());
}

// ---- Conquer the World (ctw_ui.h): the campaign's own battles run in the game window like any other game; its end brings
// the front end back to the strategic map with the outcome applied.
- (std::shared_ptr<ctwui::Session>)frontEndCtwSession {
	return _ctwSession;
}

- (void)setCtwSession:(std::shared_ptr<ctwui::Session>)s {
	_ctwSession = std::move(s);
}

- (void)frontEndPlayCtwBattle:(std::shared_ptr<ctwui::Session>)session {
	_ctwSession = session;
	[self.menu setBusy:@"Preparing the battle…"];
	[self.view draw];
	dispatch_async(dispatch_get_main_queue(), ^{
		auto build = [session](const ron::GameFiles& f, const ron::sim::Rules& r, const ron::sim::AnimSource* a, ron::sim::RandomExtras* x,
		                       std::string* err) { return session->game.build_battle(f, r, session->battle, a, x, err); };
		NSString* err = nil;
		MapRenderer* r = [MapRenderer ctwBattleWithDevice:self.device gameDir:self.gameDir build:build error:&err];
		if (!r) {
			[self.menu setBusy:nil];
			[self.menu showError:[NSString stringWithFormat:@"Could not prepare the battle: %@", err ?: @"unknown error"]];
			return;
		}
		__weak MapAppDelegate* weakSelf = self;
		r.onCtwEnd = ^(int outcome) {
			dispatch_async(dispatch_get_main_queue(), ^{
				session->battle_finished(outcome);
				[weakSelf showMenu:@"ctw-map"];
			});
		};
		const CGSize ds = self.view.drawableSize;
		const float scale = float(self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 1);
		[r setUiWidth:int(ds.width) height:int(ds.height) scale:scale];
		[self.menu stopAudio];
		self.view.frontEnd = nil;
		self.menu = nil;
		[self attachGame:r];
	});
}

- (void)frontEndChangeFolder {
	NSString* dir = launcher::choose_install(self.gameDir);
	if (!dir || [dir isEqualToString:self.gameDir]) return;
	NSString* err = nil;
	FrontEnd* m = [[FrontEnd alloc] initWithDevice:self.device gameDir:dir error:&err];
	if (!m) {
		[self alert:@"Could not open the menu" text:err];
		return;
	}
	launcher::remember_install(dir);
	self.gameDir = dir;
	[self.menu stopAudio];
	self.menu = m;
	[self showMenu:@"main"];
}

- (void)frontEndQuit {
	[NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
	return YES;
}
@end

static MapAppDelegate* g_delegate;  // the app delegate property is weak; keep it alive for the whole run

static void run_app(MapAppDelegate* d) {
	NSApplication* app = [NSApplication sharedApplication];
	[app setActivationPolicy:NSApplicationActivationPolicyRegular];
	NSMenu* menuBar = [NSMenu new];
	NSMenuItem* appItem = [NSMenuItem new];
	[menuBar addItem:appItem];
	NSMenu* appMenu = [NSMenu new];
	[appMenu addItemWithTitle:@"Quit OpenRiseOfNations" action:@selector(terminate:) keyEquivalent:@"q"];
	appItem.submenu = appMenu;
	app.mainMenu = menuBar;
	g_delegate = d;
	app.delegate = d;
	[app run];
}

// A stand-in for the app delegate in the lobby test (RON_MP_DEMO_START): records the multiplayer start the front end asks for.
@interface MpTestHost : NSObject <FrontEndHost>
@property(nonatomic, copy) NSString* log;
@end
@implementation MpTestHost
- (void)frontEndPlay:(NSString*)scenario difficulty:(int)difficulty {
}
- (void)frontEndPlayRandom:(const ron::sim::RandomGameSetup&)setup {
}
- (void)frontEndChangeFolder {
}
- (void)frontEndQuit {
}
- (void)frontEndPlayMultiplayer:(std::shared_ptr<mp::Room>)room setup:(const ron::sim::RandomGameSetup&)setup save:(std::shared_ptr<std::vector<uint8_t>>)save {
	std::string humans;
	for (int i = 0; i < setup.num_nations; i++) humans += " " + std::to_string(int(setup.nation[size_t(i)].human));
	self.log = [NSString stringWithFormat:@"frontEndPlayMultiplayer: slot %d, seed %u, %d nations, humans:%s, %s", room->slot(), setup.seed, setup.num_nations, humans.c_str(), save ? "from a save" : "new game"];
}
@end

// RON_MP_DEMO_UI=PREFIX + RON_MP_DEMO_SCRIPT: the host's lobby (`host`, the demo room with its thread guest) and a second front end that joins it as guest "Ayla",
// both driven through their real hit boxes. Steps separated by ';': `g:X,Y` / `h:X,Y` a click on the guest's / host's screen (UI pixels), `gs:NAME` / `hs:NAME`
// a snapshot PREFIX_NAME.png, `w:MS` keep both lobbies running, `st` print both screens' state. The host's delegate is an MpTestHost; so is the guest's.
static int mpDemoUiTest(FrontEnd* host, id<MTLDevice> device, NSString* dir, int port, const char* prefix, int w, int h, float scale) {
	NSString* error = nil;
	FrontEnd* guest = [[FrontEnd alloc] initWithDevice:device gameDir:dir error:&error];
	if (!guest) return 1;
	NSString* sink = [NSTemporaryDirectory() stringByAppendingPathComponent:@"ron_mp_demo_sink.png"];
	MpTestHost* hostLog = [MpTestHost new];
	MpTestHost* guestLog = [MpTestHost new];
	host.host = hostLog;
	guest.host = guestLog;
	if (![guest mpDemoJoin:port name:@"Ayla" pumping:host]) return 1;
	auto frames = [&](int ms) {
		for (int t = 0; t < ms; t += 40) {
			[host snapshotTo:sink width:w height:h scale:scale];
			[guest snapshotTo:sink width:w height:h scale:scale];
			[NSThread sleepForTimeInterval:0.04];
		}
	};
	auto click = [&](FrontEnd* fe, float x, float y) {
		const CGPoint p = CGPointMake(x, y);
		[fe snapshotTo:sink width:w height:h scale:scale];
		[fe mouseMoved:p];
		[fe mouseDown:p clicks:1];
		[fe mouseUp:p];
		frames(120);
	};
	frames(1500);
	const char* script = getenv("RON_MP_DEMO_SCRIPT");
	std::string sc = script ? script : "gs:guest;hs:host;st";
	int rc = 0;
	for (size_t q = 0; q < sc.size();) {
		size_t semi = sc.find(';', q);
		if (semi == std::string::npos) semi = sc.size();
		const std::string step = sc.substr(q, semi - q);
		q = semi + 1;
		const size_t colon = step.find(':');
		const std::string cmd = step.substr(0, colon), arg = colon == std::string::npos ? "" : step.substr(colon + 1);
		float x = 0, y = 0;
		if (cmd == "g" || cmd == "h") {
			std::sscanf(arg.c_str(), "%f,%f", &x, &y);
			click(cmd == "g" ? guest : host, x, y);
		} else if (cmd == "gs" || cmd == "hs") {
			frames(80);
			NSString* path = [NSString stringWithFormat:@"%s_%s.png", prefix, arg.c_str()];
			if (![(cmd == "gs" ? guest : host) snapshotTo:path width:w height:h scale:scale]) rc = 1;
			std::printf("snapshot %s\n", path.UTF8String);
		} else if (cmd == "w") {
			frames(std::atoi(arg.c_str()));
		} else if (cmd == "st") {
			std::printf("host:  %s | %s\n", [host mpDemoState].UTF8String, hostLog.log.UTF8String ?: "-");
			std::printf("guest: %s | %s\n", [guest mpDemoState].UTF8String, guestLog.log.UTF8String ?: "-");
		}
	}
	return rc;
}

int main(int argc, const char* argv[]) {
	@autoreleasepool {
		NSString* gameDir = [NSHomeDirectory() stringByAppendingPathComponent:@"Games/RiseOfNations"];
		NSString* scenario = nil;
		NSString* snapshot = nil;
		int zoom = -1, w = 1600, h = 1000;
		float atX = NAN, atY = NAN;
		bool bloom = true, select_all = false, simulate = true, fog = true, hud_on = true;
		float ui_scale = 1;
		int sel_who = -1, build_page = -1, place_type = -1, play = -1;
		bool place_click = false, script_log = false;
		struct TimedSelect {
			int who = 0, frame = 0;
			bool builds = false;
			std::vector<int32_t> objs;
		};
		std::vector<TimedSelect> timed_selects;
		struct TimedOption {
			int option = -1, menu = -1, type = -1, frame = 0;
		};
		std::vector<TimedOption> timed_options;
		struct TimedCursor {
			float x = 0, y = 0;
			int frame = 0;
		};
		std::vector<TimedCursor> timed_cursors, timed_looks;
		float hover_x = NAN, hover_y = NAN;
		std::string lang_arg;
		int speed_arg = -1;
		bool progress_arg = false;
		std::vector<std::string> mouse_script;  // --mouse "down|rdown|drag|up|rup:X,Y@FRAME"
		int panel_flags = -1, help_level = 0;
		bool streamlined = false;
		std::vector<std::string> timed_keys;
		std::vector<std::pair<std::string, std::string>> timed_groups;  // --group / --recall-group / --regroup
		std::vector<std::pair<std::string, bool>> timed_chats;  // --chat / --chat-draft "TEXT@FRAME": the chat line typed (and sent)
		struct TimedZoom {
			int level = 5, frame = 0;
		};
		std::vector<TimedZoom> timed_zooms;
		struct QueueArg {
			int who = 0, o = 0, type = 0, n = 1;
		};
		std::vector<QueueArg> queues;
		bool sel_builds = false;
		NSString *load_game = nil, *save_game = nil, *random_spec = nil;
		bool census = false;
		int mpHostPort = 0, mpHumans = 2, mpDiverge = -1;  // --host PORT / --join HOST:PORT: one machine of a lockstep game (lobby.h), headless
		std::string mpJoin, mpRecord, mpLoad, mpSave;
		int pb_seat = -1;      // --pb-seat WHO: the leader whose seat the playback shows (default: the first human seat); --pb-fog: with that seat's fog (default: the whole map)
		bool pb_fog = false;
		int64_t pb_seek1 = -1, pb_seek2 = -1;   // --pb-seek F1,F2: the headless seek test (frames after the start)
		NSString* playback_path = nil;   // --playback FILE: a recording of this port, played by its packages (docs/ui-port.md section 18)
		bool frames_given = false;
		std::vector<std::pair<std::string, uint32_t>> mpActs;  // --mp-act what@turn,...
		bool mpBot = false, mpPublic = false;                      // --public: the host prints how players outside the LAN can reach it
		std::vector<std::pair<int, std::string>> mpTypes;            // --mp-type "TEXT@FRAME": typed into the chat box through the key handler
		int mpObservers = 0, mpMaxObs = 2;          // --mp-observers K: the host waits for K observer machines; --mp-max-observers N
		bool mpWatch = false, ctwMp = false;        // --mp-watch: this guest asks to be an observer; --ctw-mp: a campaign battle with the room
		std::string mpName;                         // --mp-name
		std::vector<std::pair<ron::net::SeatField, int>> mpAsk;   // --mp-nation / --mp-team / --mp-color / --mp-handicap: this machine's seat requests
		std::vector<int32_t> sel_objs;
		float place_x = 0, place_y = 0;
		uint32_t seed = 12345;
		int sim_frames = 0;
		int perf_frames = 0, long_run = 0;
		NSString* long_run_prefix = nil;
		struct MoveArg {
			int who = 0, frame = 0;
			std::vector<int32_t> objs;
			int32_t x = 0, y = 0;
		};
		std::vector<MoveArg> moves;
		struct AttackArg {
			int who = 0, frame = 0, twho = 0, to = 0;
			std::vector<int32_t> objs;
		};
		std::vector<AttackArg> attacks;
		float farm_time = 0;
		struct BuildArg {
			std::string name;
			float x, y;
			int team;
		};
		std::vector<BuildArg> builds;
		std::vector<BuildArg> units;  // name, x, y, angle in `team`
		std::vector<NSString*> positional;
		bool menu = false;
		NSString* menuScreen = nil;
		// Conquer the World (ctw_cli.h, ctw_ui.h)
		NSString *ctwCampaign = nil, *ctwBattle = nil, *ctwLoad = nil, *ctwScreen = nil, *ctwSelect = nil, *ctwOut = nil;
		int ctwRound = 0, ctwRounds = 3;
		std::vector<CGPoint> ctwClicks;  // --ctw-click X,Y: mouse clicks (window points) played on the CtW screens before the snapshot
		bool ctwCheck = false, ctwSelftest = false, ctwSim = false, ctwSaveLoad = false, ctwVerbose = false;
		for (int i = 1; i < argc; i++) {
			const std::string a = argv[i];
			const bool more = i + 1 < argc;
			if (a == "--zoom" && more) {
				zoom = std::atoi(argv[++i]);
			} else if (a == "--at" && more) {
				std::sscanf(argv[++i], "%f,%f", &atX, &atY);
			} else if (a == "--snapshot" && more) {
				snapshot = @(argv[++i]);
			} else if (a == "--pb-seat" && more) {
				pb_seat = std::atoi(argv[++i]);
			} else if (a == "--pb-fog") {
				pb_fog = true;
			} else if (a == "--pb-seek" && more) {
				long long f1 = -1, f2 = -1;
				if (std::sscanf(argv[++i], "%lld,%lld", &f1, &f2) != 2 || f1 < 0 || f2 < 0) {
					std::fprintf(stderr, "--pb-seek F1,F2: two frame counts after the start\n");
					return 2;
				}
				pb_seek1 = f1, pb_seek2 = f2;
			} else if (a == "--playback" && more) {
				playback_path = @(argv[++i]);
			} else if (a == "--load-game" && more) {
				load_game = @(argv[++i]);
			} else if (a == "--save-game" && more) {
				save_game = @(argv[++i]);
			} else if (a == "--random" && more) {
				random_spec = @(argv[++i]);  // STYLE,SIZE,SEED,NATIONS[,TRIBES...]: a random-map skirmish (sim/newgame.hpp)
			} else if (a == "--host" && more) {
				mpHostPort = std::atoi(argv[++i]);
			} else if (a == "--join" && more) {
				mpJoin = argv[++i];
			} else if (a == "--humans" && more) {
				mpHumans = std::atoi(argv[++i]);
			} else if (a == "--public") {
				mpPublic = true;
			} else if (a == "--mp-type" && more) {
				std::string v = argv[++i];
				const size_t at = v.rfind('@');
				mpTypes.push_back({at == std::string::npos ? 0 : std::atoi(v.c_str() + at + 1), v.substr(0, at)});
			} else if (a == "--mp-bot") {
				mpBot = true;  // a demo player gives this machine's seat something to do
			} else if (a == "--mp-record" && more) {
				mpRecord = argv[++i];  // the .rcx of this machine's game (default for the app: the Recorded Games folder; headless: none)
			} else if (a == "--mp-load" && more) {
				mpLoad = argv[++i];  // host: continue this saved multiplayer game (the guests receive the file from the host)
			} else if (a == "--mp-save" && more) {
				mpSave = argv[++i];  // where this machine writes the synchronized save ("save@TURN" acts, the game menu)
			} else if (a == "--mp-act" && more) {
				// pause|resume|faster|slower|resign @ TURN (session turn boundaries), comma separated: this machine issues them (test hook)
				std::string list = argv[++i];
				for (size_t q = 0; q < list.size();) {
					const size_t comma = list.find(',', q), at = list.find('@', q);
					const size_t end = comma == std::string::npos ? list.size() : comma;
					if (at != std::string::npos && at < end) mpActs.push_back({list.substr(q, at - q), uint32_t(std::atoi(list.c_str() + at + 1))});
					q = end + 1;
				}
			} else if (a == "--scenario" && more) {
				positional.push_back(@(argv[++i]));   // the scenario file, as the positional argument does
			} else if (a == "--mp-observers" && more) {
				mpObservers = std::atoi(argv[++i]);
			} else if (a == "--mp-max-observers" && more) {
				mpMaxObs = std::atoi(argv[++i]);
			} else if (a == "--mp-watch") {
				mpWatch = true;
			} else if (a == "--ctw-mp") {
				ctwMp = true;
			} else if (a == "--mp-name" && more) {
				mpName = argv[++i];
			} else if (a == "--mp-nation" && more) {
				mpAsk.push_back({ron::net::SeatField::Tribe, std::atoi(argv[++i])});
			} else if (a == "--mp-team" && more) {
				mpAsk.push_back({ron::net::SeatField::Team, std::atoi(argv[++i])});
			} else if (a == "--mp-color" && more) {
				mpAsk.push_back({ron::net::SeatField::Color, std::atoi(argv[++i])});
			} else if (a == "--mp-handicap" && more) {
				mpAsk.push_back({ron::net::SeatField::Handicap, std::atoi(argv[++i])});
			} else if (a == "--mp-diverge" && more) {
				mpDiverge = std::atoi(argv[++i]);  // test: this machine's state is corrupted at that frame, the session must repair it
			} else if (a == "--all-ai") {
				g_randomAllAi = true;  // every seat of the --random game is a computer player
			} else if (a == "--census") {
				census = true;  // print the per-seat census after the frames
			} else if (a == "--size" && more) {
				std::sscanf(argv[++i], "%dx%d", &w, &h);
			} else if (a == "--build" && more) {
				// NAME@X,Y[@TEAM]
				const std::string spec = argv[++i];
				const size_t at = spec.find('@');
				BuildArg b{spec.substr(0, at), 0, 0, 1};
				if (at != std::string::npos) std::sscanf(spec.c_str() + at + 1, "%f,%f@%d", &b.x, &b.y, &b.team);
				builds.push_back(b);
			} else if (a == "--unit" && more) {
				// NAME@X,Y[@ANGLE]
				const std::string spec = argv[++i];
				const size_t at = spec.find('@');
				BuildArg u{spec.substr(0, at), 0, 0, 0};
				if (at != std::string::npos) std::sscanf(spec.c_str() + at + 1, "%f,%f@%d", &u.x, &u.y, &u.team);
				units.push_back(u);
			} else if (a == "--farm-time" && more) {
				farm_time = float(std::atof(argv[++i]));
			} else if (a == "--select-all") {
				select_all = true;
			} else if (a == "--no-scripts") {
				g_noScripts = true;
			} else if (a == "--no-sim") {
				simulate = false;
			} else if (a == "--seed" && more) {
				seed = uint32_t(std::strtoul(argv[++i], nullptr, 0));
			} else if (a == "--perf" && more) {
				perf_frames = std::atoi(argv[++i]);  // N more headless frames, timed: simulation step and rendered frame (after --frames)
			} else if (a == "--long-run" && more) {
				const std::string v = argv[++i];  // EVERY[,SNAPSHOT_PREFIX]
				long_run = std::atoi(v.c_str());
				if (v.find(',') != std::string::npos) long_run_prefix = [NSString stringWithUTF8String:v.c_str() + v.find(',') + 1];
			} else if (a == "--frames" && more) {
				frames_given = true;
				sim_frames = std::atoi(argv[++i]);
			} else if (a == "--move" && more) {
				// WHO:o1,o2,...@X,Y[@FRAME]
				const std::string spec = argv[++i];
				MoveArg m;
				const size_t colon = spec.find(':'), at = spec.find('@');
				if (colon == std::string::npos || at == std::string::npos || at < colon) {
					std::fprintf(stderr, "bad --move %s (WHO:o1,o2@X,Y[@FRAME])\n", spec.c_str());
					return 2;
				}
				m.who = std::atoi(spec.c_str());
				for (size_t p = colon + 1; p < at;) {
					m.objs.push_back(int32_t(std::strtol(spec.c_str() + p, nullptr, 0)));
					const size_t comma = spec.find(',', p);
					if (comma == std::string::npos || comma > at) break;
					p = comma + 1;
				}
				std::sscanf(spec.c_str() + at + 1, "%d,%d@%d", &m.x, &m.y, &m.frame);
				moves.push_back(m);
			} else if (a == "--attack" && more) {
				// WHO:o1,o2,...@TWHO:TO[@FRAME]
				const std::string spec = argv[++i];
				AttackArg k;
				const size_t colon = spec.find(':'), at = spec.find('@');
				if (colon == std::string::npos || at == std::string::npos || at < colon) {
					std::fprintf(stderr, "bad --attack %s (WHO:o1,o2@TWHO:TO[@FRAME])\n", spec.c_str());
					return 2;
				}
				k.who = std::atoi(spec.c_str());
				for (size_t p = colon + 1; p < at;) {
					k.objs.push_back(int32_t(std::strtol(spec.c_str() + p, nullptr, 0)));
					const size_t comma = spec.find(',', p);
					if (comma == std::string::npos || comma > at) break;
					p = comma + 1;
				}
				std::sscanf(spec.c_str() + at + 1, "%d:%d@%d", &k.twho, &k.to, &k.frame);
				attacks.push_back(k);
			} else if (a == "--no-hud") {
				hud_on = false;
			} else if (a == "--ui-scale" && more) {
				ui_scale = float(std::atof(argv[++i]));
			} else if ((a == "--select" || a == "--select-build") && more && std::strchr(argv[i + 1], '@')) {
				// WHO:o1,o2,...@FRAME: a selection made at FRAME (the scripts see it from then on)
				const std::string spec = argv[++i];
				TimedSelect t;
				t.builds = a == "--select-build";
				t.who = std::atoi(spec.c_str());
				const size_t colon = spec.find(':'), at = spec.find('@');
				for (size_t q = colon == std::string::npos ? at : colon + 1; q < at;) {
					t.objs.push_back(int32_t(std::strtol(spec.c_str() + q, nullptr, 0)));
					const size_t comma = spec.find(',', q);
					if (comma == std::string::npos || comma > at) break;
					q = comma + 1;
				}
				t.frame = std::atoi(spec.c_str() + at + 1);
				timed_selects.push_back(t);
			} else if (a == "--option" && more) {
				// N[:MENU[:TYPE]]@FRAME
				const std::string spec = argv[++i];
				TimedOption t;
				const size_t at = spec.find('@');
				std::sscanf(spec.c_str(), "%d:%d:%d", &t.option, &t.menu, &t.type);
				if (at != std::string::npos) t.frame = std::atoi(spec.c_str() + at + 1);
				timed_options.push_back(t);
			} else if (a == "--cursor" && more) {
				TimedCursor t;
				std::sscanf(argv[++i], "%f,%f@%d", &t.x, &t.y, &t.frame);
				timed_cursors.push_back(t);
			} else if (a == "--look" && more) {  // X,Y@FRAME: the player scrolls the camera there
				TimedCursor t;
				std::sscanf(argv[++i], "%f,%f@%d", &t.x, &t.y, &t.frame);
				timed_looks.push_back(t);
			} else if (a == "--zoom-at" && more) {  // LEVEL@FRAME: the player zooms
				TimedZoom t;
				std::sscanf(argv[++i], "%d@%d", &t.level, &t.frame);
				timed_zooms.push_back(t);
			} else if (a == "--hover" && more) {  // X,Y: the mouse at that UI pixel, mouse-over help shown
				std::sscanf(argv[++i], "%f,%f", &hover_x, &hover_y);
			} else if (a == "--panels" && more) {  // resource panel flags: 1 market, 2 resources, 4 infrastructure
				panel_flags = std::atoi(argv[++i]);
			} else if (a == "--streamlined") {
				streamlined = true;
			} else if (a == "--lang" && more) {  // en, de, es, fr, it (or .7 .10 .12 .16): the language of the interface
				lang_arg = argv[++i];
			} else if (a == "--speed" && more) {  // 0 Very Slow .. 4 Hyper Fast (default 2 Normal)
				speed_arg = std::atoi(argv[++i]);
			} else if (a == "--progress-buttons") {
				progress_arg = true;
			} else if (a == "--mouse" && more) {  // ACTION:X,Y[@FRAME]: a mouse event (down, drag, up; rdown, rdrag, rup) in UI pixels
				mouse_script.push_back(argv[++i]);
			} else if (a == "--help-level" && more) {
				help_level = std::atoi(argv[++i]);
			} else if (a == "--key" && more) {  // "KEYS@FRAME" (e.g. "Alt+M@10"): a hotkey as the key map resolves it
				timed_keys.push_back(argv[++i]);
			} else if ((a == "--group" || a == "--recall-group" || a == "--regroup") && more) {  // named army groups: "NAME@FRAME" / "N@FRAME"
				timed_groups.push_back({a, argv[++i]});
			} else if ((a == "--chat" || a == "--chat-draft") && more) {  // "TEXT@FRAME": typed into the chat line (--chat sends it)
				timed_chats.push_back({argv[++i], a == "--chat"});
			} else if (a == "--script-log") {
				script_log = true;
			} else if ((a == "--select" || a == "--select-build") && more) {
				// WHO:o1,o2,...
				const std::string spec = argv[++i];
				sel_builds = a == "--select-build";
				sel_who = std::atoi(spec.c_str());
				const size_t colon = spec.find(':');
				for (size_t q = colon == std::string::npos ? spec.size() : colon + 1; q < spec.size();) {
					sel_objs.push_back(int32_t(std::strtol(spec.c_str() + q, nullptr, 0)));
					const size_t comma = spec.find(',', q);
					if (comma == std::string::npos) break;
					q = comma + 1;
				}
			} else if (a == "--place-click") {
				place_click = true;
			} else if (a == "--queue" && more) {
				// WHO:O@TYPE[*N]  a building trains / researches TYPE (QUEUE_UP), as the card's buttons do
				QueueArg q;
				std::sscanf(argv[++i], "%d:%d@%d*%d", &q.who, &q.o, &q.type, &q.n);
				queues.push_back(q);
			} else if (a == "--play" && more) {
				play = std::atoi(argv[++i]);
			} else if (a == "--build-page" && more) {
				build_page = std::atoi(argv[++i]);
			} else if (a == "--place" && more) {
				std::sscanf(argv[++i], "%d@%f,%f", &place_type, &place_x, &place_y);
			} else if (a == "--difficulty" && more) {
				g_difficulty = std::atoi(argv[++i]);
			} else if (a == "--no-fog") {
				fog = false;
			} else if (a == "--no-bloom") {
				bloom = false;
			} else if (a == "--ctw-check") {
				ctwCheck = true;
			} else if (a == "--ctw-selftest") {
				ctwSelftest = true;
			} else if (a == "--ctw-sim" && more) {
				ctwSim = true;
				ctwCampaign = @(argv[++i]);
			} else if (a == "--ctw-saveload" && more) {
				ctwSaveLoad = true;
				ctwCampaign = @(argv[++i]);
			} else if (a == "--ctw" && more) {
				ctwCampaign = @(argv[++i]);  // the strategic map of a new campaign (--round N plays N AI rounds first)
			} else if (a == "--ctw-load" && more) {
				ctwLoad = @(argv[++i]);
			} else if (a == "--ctw-battle" && more) {
				ctwBattle = @(argv[++i]);  // CAMPAIGN:auto or CAMPAIGN:FROM>TO: one hand-off battle
			} else if (a == "--round" && more) {
				ctwRound = std::atoi(argv[++i]);
			} else if (a == "--rounds" && more) {
				ctwRounds = std::atoi(argv[++i]);
			} else if (a == "--ctw-screen" && more) {
				ctwScreen = @(argv[++i]);  // news, cards, diplomacy, attack, menu
			} else if (a == "--ctw-select" && more) {
				ctwSelect = @(argv[++i]);  // a territory id selected on the map
			} else if (a == "--ctw-out" && more) {
				ctwOut = @(argv[++i]);
			} else if (a == "--ctw-click" && more) {
				float cx = 0, cy = 0;
				std::sscanf(argv[++i], "%f,%f", &cx, &cy);
				ctwClicks.push_back(CGPointMake(cx, cy));
			} else if (a == "--ctw-verbose") {
				ctwVerbose = true;
			} else if (a == "--menu") {
				menu = true;
				static const char* const screens[] = {"main", "tutorials", "skirmish", "skirmish-setup", "scenarios", "quit-popup", "ctw", "ctw-map", "multiplayer", "mp-lobby", "options", "recorded"};
				for (const char* sname : screens) {
					if (more && std::strcmp(argv[i + 1], sname) == 0) {
						menuScreen = @(argv[++i]);
						break;  // `more` is stale after the ++i: the next compare would read argv[argc] when the screen name is the last argument
					}
				}
			} else if (a.rfind("--", 0) == 0) {
				std::printf("usage: %s [game-dir] [--menu [main|tutorials|skirmish|skirmish-setup|scenarios|quit-popup|ctw|ctw-map|multiplayer|mp-lobby|options|recorded]] [--snapshot out.png [--size WxH]]\n"
				            "       ronmap [game-dir] --playback FILE.rcx [--frames N] [--pb-seat WHO] [--pb-fog] [--pb-seek F1,F2] [--snapshot out.png [--size WxH]]\n"
				            "       %s [game-dir] --random STYLE,SIZE,SEED,NATIONS[,TRIBES...] [--all-ai] [--difficulty 0..5] [--frames N] [--census] [--save-game FILE] [--snapshot out.png]\n"
				            "       %s [game-dir] <scenario.scx> [--zoom 0..6] [--at X,Y] [--no-bloom] [--farm-time S] [--no-sim] [--seed S] "
				            "[--move WHO:o1,o2@X,Y[@FRAME]]... [--frames N] [--load-game FILE] [--save-game FILE] [--snapshot out.png [--size WxH]]\n", argv[0], argv[0], argv[0]);
				return a == "--help" ? 0 : 2;
			} else {
				if (a.rfind("-psn_", 0) != 0) positional.push_back(@(argv[i]));  // Finder's process serial number
			}
		}
		if (positional.size() == 2) gameDir = positional[0], scenario = positional[1];
		NSString* explicitDir = positional.size() == 2 ? positional[0] : nil;
		if (positional.size() == 1) {
			// One argument: a game folder opens the menu, anything else is a scenario.
			if (launcher::looks_like_install(positional[0])) explicitDir = gameDir = positional[0];
			else scenario = positional[0];
		}
		uistr::choose(lang_arg);
		keymap::apply_overrides(prefs::get().hotkeys);  // the Options page's Hotkeys pane
		if (ctwCheck) return ctwcli::check(gameDir.UTF8String);
		if (ctwSelftest) return ctwcli::selftest(gameDir.UTF8String);
		if (ctwSim) return ctwcli::simulate(gameDir.UTF8String, ctwCampaign.UTF8String, seed, ctwRounds, g_difficulty >= 0 ? g_difficulty : 2,
		                                    ctwOut ? ctwOut.UTF8String : "", ctwVerbose);
		if (ctwSaveLoad) return ctwcli::save_roundtrip(gameDir.UTF8String, ctwCampaign.UTF8String, seed);
		std::shared_ptr<ron::net::Playback> playback;
		if (playback_path) {
			std::string perr;
			playback.reset(ron::net::Playback::open(playback_path.UTF8String, gameDir.UTF8String, perr).release());
			if (!playback) {
				std::fprintf(stderr, "--playback: %s\n", perr.c_str());
				return 1;
			}
		}
		if (!scenario && !random_spec && !ctwBattle && mpJoin.empty() && !playback) menu = true;
		if (menu) {
			// The front end: headless with --snapshot, else the app opens its window on the Main Menu.
			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			if (!device) {
				std::fprintf(stderr, "no Metal device\n");
				return 1;
			}
			if (snapshot) {
				NSString* dir = launcher::looks_like_install(explicitDir) ? explicitDir : launcher::saved_or_guessed_install();
				NSString* error = nil;
				FrontEnd* fe = [[FrontEnd alloc] initWithDevice:device gameDir:dir error:&error];
				if (!fe) {
					std::fprintf(stderr, "%s\n", (error ?: @"no Rise of Nations install found").UTF8String);
					return 1;
				}
				if (ctwCampaign || ctwLoad) {
					std::string err;
					std::shared_ptr<ctwui::Session> session;
					if (ctwLoad) {
						session = ctwui::load_session(dir.UTF8String, ctwLoad.UTF8String, &err);
					} else {
						ron::GameFiles files(dir.UTF8String);
						ron::sim::ctw::ConquestSetup cs;
						cs.campaign = ctwcli::resolve_campaign(files, ctwCampaign.UTF8String);
						cs.seed = seed;
						cs.difficulty = g_difficulty >= 0 ? g_difficulty : 2;
						session = cs.campaign.empty() ? nullptr : ctwui::new_session(dir.UTF8String, cs, &err);
						if (cs.campaign.empty()) err = "no such campaign";
						if (session) ctwcli::play_rounds(session->game, ctwRound);
					}
					if (!session) {
						std::fprintf(stderr, "%s\n", err.c_str());
						return 1;
					}
					[fe ctwShowSession:session sub:ctwScreen select:ctwSelect];
					if (!ctwClicks.empty()) {
						[fe snapshotTo:@"/dev/null" width:w height:h scale:ui_scale];  // a priming frame builds the hit boxes
						for (const CGPoint& p : ctwClicks) {
							[fe mouseMoved:p];
							[fe mouseDown:p clicks:1];
							[fe mouseUp:p];
							[fe snapshotTo:@"/dev/null" width:w height:h scale:ui_scale];
						}
					}
				} else {
					if ([menuScreen isEqualToString:@"mp-lobby"]) {  // a room with two guests (threads of this process) for the lobby snapshots
						const char* port = getenv("RON_MP_DEMO_PORT");
						const char* uiTest = getenv("RON_MP_DEMO_UI");   // two real front ends (host + guest "Ayla") driven by scripted clicks: docs/ui-port.md 13.x
						if (![fe mpDemoPort:port ? std::atoi(port) : 17777 guests:uiTest ? @[@"Marcus"] : @[@"Ayla", @"Marcus"]]) std::fprintf(stderr, "the demo room could not be set up\n");
						if (uiTest) return mpDemoUiTest(fe, device, dir, port ? std::atoi(port) : 17777, uiTest, w, h, ui_scale);
						if (getenv("RON_MP_DEMO_START")) {  // the host presses Start: a test host receives the call the app delegate would get
							MpTestHost* th = [MpTestHost new];
							fe.host = th;
							[fe mpDemoStart];
							std::printf("[demo host] Start pressed: %s\n", th.log.UTF8String ?: "nothing happened");
						}
					} else {
						[fe showScreen:menuScreen ?: @"main"];
					}
				}
				return [fe snapshotTo:snapshot width:w height:h scale:ui_scale] ? 0 : 1;
			}
			MapAppDelegate* d = [MapAppDelegate new];
			d.device = device;
			d.gameDir = explicitDir;
			d.startScreen = menuScreen;
			if (ctwCampaign || ctwLoad) {
				const std::string dirStr = (explicitDir ?: gameDir).UTF8String;
				std::string err;
				std::shared_ptr<ctwui::Session> session;
				if (ctwLoad) {
					session = ctwui::load_session(dirStr, ctwLoad.UTF8String, &err);
				} else {
					ron::GameFiles files(dirStr);
					ron::sim::ctw::ConquestSetup cs;
					cs.campaign = ctwcli::resolve_campaign(files, ctwCampaign.UTF8String);
					cs.seed = seed;
					cs.difficulty = g_difficulty >= 0 ? g_difficulty : 2;
					session = cs.campaign.empty() ? nullptr : ctwui::new_session(dirStr, cs, &err);
					if (session) ctwcli::play_rounds(session->game, ctwRound);
				}
				if (!session) {
					std::fprintf(stderr, "%s\n", err.empty() ? "no such campaign" : err.c_str());
					return 1;
				}
				[d setCtwSession:session];
				d.startScreen = @"ctw-map";
			}
			run_app(d);
			return 0;
		}
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (!device) {
			std::fprintf(stderr, "no Metal device\n");
			return 1;
		}
		NSString* error = nil;
		std::shared_ptr<ctwui::Session> ctwSess;
		MapRenderer* r = nil;
		std::shared_ptr<ctwui::Session> ctwMpSess;   // --ctw-mp: the campaign copy this machine plays the battle from
		std::shared_ptr<mp::Room> ctwMpRoom;
		if (ctwMp && (mpHostPort > 0 || !mpJoin.empty())) {
			// A campaign battle with the room, headless: the host's campaign (CAMPAIGN:LINK as for --ctw-battle) is fought by --humans players; the
			// host applies the outcome to its campaign and sends it; every guest ends with an identical copy.
			auto room = std::make_shared<mp::Room>();
			std::string merr;
			if (mpHostPort > 0) {
				if (!ctwBattle) {
					std::fprintf(stderr, "--ctw-mp --host needs --ctw-battle CAMPAIGN:LINK\n");
					return 2;
				}
				const std::string spec = ctwBattle.UTF8String;
				const size_t colon = spec.find(':');
				ron::GameFiles files(gameDir.UTF8String);
				ron::sim::ctw::ConquestSetup cs;
				cs.campaign = ctwcli::resolve_campaign(files, spec.substr(0, colon));
				cs.seed = seed;
				cs.difficulty = g_difficulty >= 0 ? g_difficulty : 2;
				std::string err;
				if (!cs.campaign.empty()) ctwMpSess = ctwui::new_session(gameDir.UTF8String, cs, &err);
				if (!ctwMpSess) {
					std::fprintf(stderr, "ctw-mp: %s\n", cs.campaign.empty() ? "no such campaign" : err.c_str());
					return 1;
				}
				if (ctwRound > 0) ctwcli::play_rounds(ctwMpSess->game, ctwRound);
				ron::sim::ctw::LinkChoice link;
				if (!ctwcli::pick_link(ctwMpSess->game, colon == std::string::npos ? "auto" : spec.substr(colon + 1), &link, &err)) {
					std::fprintf(stderr, "ctw-mp: %s\n", err.c_str());
					return 1;
				}
				ron::sim::ctw::ConquestGame& cg = ctwMpSess->game;
				cg.cur_link = link;
				cg.cur_style = cg.pick_style(link);
				ctwMpSess->battle = cg.make_battle(link, cg.cur_style);
				if (!room->host(uint16_t(mpHostPort), mpName.empty() ? "Host" : mpName, merr)) {
					std::fprintf(stderr, "%s\n", merr.c_str());
					return 2;
				}
				std::printf("[host] ctw battle %s attacks %s; listening on port %d for %d player(s)\n", cg.tribe_name(link.attacker).c_str(), cg.node_name(link.to).c_str(), mpHostPort, mpHumans - 1);
				if (mpPublic) std::printf("[host] %s\n", ron::net::public_host_note(uint16_t(mpHostPort)).c_str());
				const double t0 = CACurrentMediaTime();
				while (room->humans() < mpHumans + mpObservers || !room->guests_ready()) {
					room->pump();
					room->transport().idle(20);
					if (CACurrentMediaTime() - t0 > 120) {
						std::fprintf(stderr, "[host] nobody joined\n");
						return 2;
					}
				}
				const double t1 = CACurrentMediaTime();
				while ((room->players() != mpHumans || room->observers() != mpObservers) && CACurrentMediaTime() - t1 < 10) {
					room->pump();
					room->transport().idle(20);
				}
				if (!mp::ctw_host_start(*room, *ctwMpSess, err)) {
					std::fprintf(stderr, "[host] %s\n", err.c_str());
					return 2;
				}
			} else {
				ron::net::Endpoint ep;   // HOST, HOST:PORT: an address or a host name that DNS resolves
				if (!ron::net::parse_endpoint(mpJoin, 7777, ep, merr)) {
					std::fprintf(stderr, "--join: %s\n", merr.c_str());
					return 2;
				}
				if (!room->join(ep.host, ep.port, mpName.empty() ? "Guest" : mpName, 30000, merr)) {
					std::fprintf(stderr, "%s\n", merr.c_str());
					return 2;
				}
				room->set_name(mpName.empty() ? "Guest " + std::to_string(room->slot()) : mpName);
				room->set_ready(true);
				if (mpWatch) room->request(ron::net::SeatField::Observer, 1);
				const double t0 = CACurrentMediaTime();
				while (!room->started()) {
					room->pump();
					room->transport().idle(20);
					if (CACurrentMediaTime() - t0 > 120 || room->host_lost()) {
						std::fprintf(stderr, "[join] the host never started the battle\n");
						return 2;
					}
				}
			}
			std::string err;
			ctwMpSess = mp::ctw_battle_session(*room, ctwMpSess, gameDir.UTF8String, err);
			if (!ctwMpSess) {
				std::fprintf(stderr, "ctw-mp: %s\n", err.c_str());
				return 2;
			}
			std::shared_ptr<ctwui::Session> sess = ctwMpSess;
			auto build = [sess](const ron::GameFiles& f, const ron::sim::Rules& rr, const ron::sim::AnimSource* a, ron::sim::RandomExtras* x, std::string* e2) {
				return sess->game.build_battle(f, rr, sess->battle, a, x, e2);
			};
			r = [MapRenderer ctwBattleWithDevice:device gameDir:gameDir build:build error:&error];
			if (r) {
				auto rt = std::make_shared<mp::Runtime>(room);
				rt->record_to(mpRecord);  // a campaign battle is recorded like any game (generated maps only; none unless asked here)
				rt->enable_bot(mpBot);
				for (const auto& act : mpActs) rt->add_act(act.first, act.second);
				[r attachMultiplayer:rt loaded:nullptr];
			}
			ctwMpRoom = room;
		} else if (ctwBattle) {
			// CAMPAIGN:LINK -> a hand-off battle of a fresh campaign (the player attacks along LINK: auto or FROM>TO)
			const std::string spec = ctwBattle.UTF8String;
			const size_t colon = spec.find(':');
			ron::GameFiles files(gameDir.UTF8String);
			ron::sim::ctw::ConquestSetup cs;
			cs.campaign = ctwcli::resolve_campaign(files, spec.substr(0, colon));
			cs.seed = seed;
			cs.difficulty = g_difficulty >= 0 ? g_difficulty : 2;
			std::string err;
			if (!cs.campaign.empty()) ctwSess = ctwui::new_session(gameDir.UTF8String, cs, &err);
			if (!ctwSess) {
				std::fprintf(stderr, "ctw-battle: %s\n", cs.campaign.empty() ? "no such campaign" : err.c_str());
				return 1;
			}
			if (ctwRound > 0) ctwcli::play_rounds(ctwSess->game, ctwRound);
			ron::sim::ctw::LinkChoice link;
			if (!ctwcli::pick_link(ctwSess->game, colon == std::string::npos ? "auto" : spec.substr(colon + 1), &link, &err)) {
				std::fprintf(stderr, "ctw-battle: %s\n", err.c_str());
				return 1;
			}
			ron::sim::ctw::ConquestGame& cg = ctwSess->game;
			cg.cur_link = link;
			cg.cur_style = cg.pick_style(link);
			ctwSess->battle = cg.make_battle(link, cg.cur_style);
			if (g_randomAllAi) {
				for (auto& n : ctwSess->battle.setup.nation) n.human = false;
				for (auto& seat : ctwSess->battle.seats) seat.human = false;
			}
			std::printf("ctw battle: %s attacks %s (%s) from %s; style \"%s\"%s%s; %zu seats\n", cg.tribe_name(link.attacker).c_str(), cg.node_name(link.to).c_str(),
			            link.defender >= 0 ? cg.tribe_name(link.defender).c_str() : "barbarians", cg.node_name(link.from).c_str(),
			            cg.cur_style.index >= 0 ? cg.style_table[size_t(cg.cur_style.index)].title.c_str() : "default",
			            ctwSess->battle.custom_map.empty() ? "" : ", map ", ctwSess->battle.custom_map.c_str(), ctwSess->battle.seats.size());
			std::shared_ptr<ctwui::Session> sess = ctwSess;
			auto build = [sess](const ron::GameFiles& f, const ron::sim::Rules& rr, const ron::sim::AnimSource* a, ron::sim::RandomExtras* x, std::string* e2) {
				return sess->game.build_battle(f, rr, sess->battle, a, x, e2);
			};
			r = [MapRenderer ctwBattleWithDevice:device gameDir:gameDir build:build error:&error];
		} else if (mpHostPort > 0 || !mpJoin.empty()) {
			// One machine of a LAN game without the lobby screens: the host fixes the setup (--random, or --scenario FILE) once --humans players (and
			// --mp-observers observers) are in, the guests take it
			auto room = std::make_shared<mp::Room>();
			std::string merr;
			ron::sim::RandomGameSetup mpSetup;
			mp::ScenarioInfo mpScen;
			bool mpScenario = false;
			std::shared_ptr<std::vector<uint8_t>> mpLoaded;
			if (mpHostPort > 0) {
				if (scenario) {
					if (!mp::load_scenario_info(gameDir.UTF8String, scenario.UTF8String, mpScen, merr)) {
						std::fprintf(stderr, "--scenario: %s\n", merr.c_str());
						return 2;
					}
					mpScenario = true;
					mpSetup = ron::sim::default_random_setup(mpScen.seats);
					mpSetup.num_nations = mpScen.seats;
					for (int k = 0; k < mpScen.seats; k++) {
						mpSetup.nation[size_t(k)].tribe = mpScen.tribe[size_t(k)];
						mpSetup.nation[size_t(k)].color = mpScen.color[size_t(k)];
						mpSetup.nation[size_t(k)].difficulty = mpScen.difficulty[size_t(k)];
					}
				} else if (!random_spec || !mp::parse_spec(gameDir.UTF8String, random_spec.UTF8String, mpSetup, merr)) {
					std::fprintf(stderr, "--host needs --random STYLE,SIZE,SEED,NATIONS or --scenario FILE: %s\n", merr.c_str());
					return 2;
				}
				if (!room->host(uint16_t(mpHostPort), mpName.empty() ? "Host" : mpName, merr)) {
					std::fprintf(stderr, "%s\n", merr.c_str());
					return 2;
				}
				room->set_max_observers(mpMaxObs);
				room->publish(mpSetup);
				if (mpScenario) room->publish_scenario(mpScen);
				for (const auto& ask : mpAsk)  // the host's own seat (the first player's): handicap only, the rest is the setup's
					if (ask.first == ron::net::SeatField::Handicap) room->lobby().set("handicap_0", ask.second);
				std::printf("[host] listening on port %d for %d player(s)%s\n", mpHostPort, mpHumans - 1, mpScenario ? " (scenario)" : "");
				if (mpPublic) std::printf("[host] %s\n", ron::net::public_host_note(uint16_t(mpHostPort)).c_str());
				// guests' requests are mirrored into the setup as they arrive, as the lobby screen does
				auto serve = [&]() {
					room->pump();
					ron::net::SeatRequest rq;
					bool changed = false;
					while (room->lobby().take_request(rq)) {
						if (rq.seat >= 0 && rq.seat < 8) {
							ron::sim::NationSetup& ns = mpSetup.nation[size_t(rq.seat)];
							if (rq.field == ron::net::SeatField::Tribe) ns.tribe = rq.value;
							else if (rq.field == ron::net::SeatField::Team) ns.team = rq.value;
							else if (rq.field == ron::net::SeatField::Color) ns.color = rq.value;
						}
						changed = true;
					}
					if (changed) room->publish(mpSetup);
				};
				const double t0 = CACurrentMediaTime();
				while (room->humans() < mpHumans + mpObservers) {
					serve();
					room->transport().idle(20);
					if (CACurrentMediaTime() - t0 > 120) {
						std::fprintf(stderr, "[host] nobody joined\n");
						return 2;
					}
				}
				const double t1 = CACurrentMediaTime();  // the guests' first requests (an observer's seat, nation, team, colour, handicap)
				while ((room->players() != mpHumans || room->observers() != mpObservers || !room->guests_ready()) && CACurrentMediaTime() - t1 < 15) {
					serve();
					room->transport().idle(20);
				}
				for (int k = 0; k < 20; k++) {
					serve();
					room->transport().idle(10);
				}
				mpSetup.game_speed = 2;
				if (!mpLoad.empty()) {  // continue a saved game: its setup and seed, the file goes to the guests before START
					NSData* data = [NSData dataWithContentsOfFile:@(mpLoad.c_str())];
					if (!data.length) {
						std::fprintf(stderr, "[host] cannot read %s\n", mpLoad.c_str());
						return 2;
					}
					mpLoaded = std::make_shared<std::vector<uint8_t>>(static_cast<const uint8_t*>(data.bytes), static_cast<const uint8_t*>(data.bytes) + data.length);
					try {
						const ron::sim::SaveInfo info = ron::sim::read_save_info(*mpLoaded);
						if (!ron::sim::parse_random_setup(info.scenario, mpSetup)) {
							std::fprintf(stderr, "[host] only saved random-map games can be continued\n");
							return 2;
						}
						mpSetup.seed = info.seed;
						std::printf("[host] continuing %s (frame %d)\n", mpLoad.c_str(), info.frame);
					} catch (const std::exception& e) {
						std::fprintf(stderr, "[host] %s\n", e.what());
						return 2;
					}
					room->lobby().set("identity", 1);
					room->publish(mpSetup);
					room->lobby().set("from_save", "1");
					room->lobby().send_file(*mpLoaded);
					room->start(mpSetup.seed);
				} else {
					if (!room->seat_humans(mpSetup)) {
						std::fprintf(stderr, "[host] the room cannot start a game (fewer than two machines, or no seat fits)\n");
						return 2;
					}
					room->publish(mpSetup);
					if (!room->start(mpSetup.seed)) {
						std::fprintf(stderr, "[host] the seats do not fit\n");
						return 2;
					}
				}
				{
					const ron::net::SeatPlan pl = room->lobby().start_plan();
					std::printf("[host] plan: %d player(s), %d observer(s), %d seat(s):", pl.players, pl.observers, pl.nations);
					for (int sl = 0; sl < 8; sl++)
						if (pl.peer_of[size_t(sl)] >= 0) std::printf(" slot%d=%s", sl, room->name_of(pl.peer_of[size_t(sl)]).c_str());
					std::printf("\n");
				}
			} else {
				ron::net::Endpoint ep;   // HOST, HOST:PORT: an address or a host name that DNS resolves
				if (!ron::net::parse_endpoint(mpJoin, 7777, ep, merr)) {
					std::fprintf(stderr, "--join: %s\n", merr.c_str());
					return 2;
				}
				if (!room->join(ep.host, ep.port, mpName.empty() ? "Guest" : mpName, 30000, merr)) {
					std::fprintf(stderr, "%s\n", merr.c_str());
					return 2;
				}
				room->set_name(mpName.empty() ? "Guest " + std::to_string(room->slot()) : mpName);
				room->set_ready(true);
				if (mpWatch) room->request(ron::net::SeatField::Observer, 1);
				for (const auto& ask : mpAsk) room->request(ask.first, ask.second);
				const double t0 = CACurrentMediaTime();
				while (!room->started()) {
					room->pump();
					for (std::string n; !(n = room->take_notice()).empty();) std::printf("[join] %s\n", n.c_str());
					room->transport().idle(20);
					if (CACurrentMediaTime() - t0 > 120 || room->host_lost()) {
						std::fprintf(stderr, "[join] the host never started the game\n");
						return 2;
					}
				}
				if (!room->read_setup(mpSetup)) {
					std::fprintf(stderr, "[join] the lobby has no map settings\n");
					return 2;
				}
				mpScenario = room->read_scenario(mpScen);
				if (room->lobby().has("from_save")) {  // the host continues a saved game: its file arrives before START
					if (!room->lobby().file_complete()) {
						std::fprintf(stderr, "[join] the saved game did not arrive\n");
						return 2;
					}
					mpLoaded = std::make_shared<std::vector<uint8_t>>(room->lobby().file());
				}
				const ron::net::SeatPlan pl = room->lobby().start_plan();
				std::printf("[join] peer %d is game slot %d (%s): style %d size %d seed %u nations %d\n", room->slot(), pl.slot_of[size_t(room->slot() & 7)],
				            pl.slot_of[size_t(room->slot() & 7)] >= pl.nations ? "observer" : "player", mpSetup.map_style, mpSetup.map_size, mpSetup.seed, mpSetup.num_nations);
			}
			std::array<int, 8> mpWho{};
			for (int k = 0; k < 8; k++) mpWho[size_t(k)] = mpScenario && k < mpScen.seats ? mpScen.who[size_t(k)] : k;
			if (mpScenario) {
				const ron::net::SeatPlan pl = room->lobby().start_plan();
				std::array<bool, 8> humans{};
				for (int k = 0; k < mpScen.seats && k < 8; k++) humans[size_t(mpScen.who[size_t(k)] & 7)] = pl.peer_of[size_t(k)] >= 0;
				r = [MapRenderer scenarioMultiplayerWithDevice:device gameDir:gameDir scenario:@(mpScen.rel.c_str()) seed:room->lobby().seed() humans:humans error:&error];
			} else {
				r = [MapRenderer randomGameWithDevice:device gameDir:gameDir setup:&mpSetup spec:nil error:&error];
			}
			if (r) {
				auto rt = std::make_shared<mp::Runtime>(room);
				rt->set_who(mpWho);
				rt->enable_bot(mpBot);
				rt->record_to(mpRecord);  // none unless asked: headless runs must not fill the user's folder
				if (mpDiverge >= 0) rt->diverge_at(mpDiverge);
				if (!mpSave.empty()) rt->set_save_path(mpSave);
				for (const auto& act : mpActs) rt->add_act(act.first, act.second);
				[r attachMultiplayer:rt loaded:mpLoaded];
			}
		} else if (playback) {
			r = [MapRenderer playbackWithDevice:device gameDir:gameDir playback:playback error:&error];
		} else {
			r = random_spec ? [MapRenderer randomGameWithDevice:device gameDir:gameDir setup:nullptr spec:random_spec error:&error]
			                : [[MapRenderer alloc] initWithDevice:device gameDir:gameDir scenario:scenario simulate:simulate seed:seed error:&error];
		}
		if (!r) {
			std::fprintf(stderr, "%s\n", error.UTF8String);
			return 1;
		}
		if (zoom >= 0) r.zoom = zoom;
		if (!std::isnan(atX)) [r lookAtX:atX y:atY];
		r.bloom = bloom;
		r.fogOn = fog;
		if (farm_time > 0) [r advanceFarmsTo:uint32_t(farm_time * 15.0f)];
		for (const BuildArg& b : builds) [r placeBuilding:b.name x:b.x y:b.y team:b.team];
		for (const BuildArg& u : units) [r placeUnit:u.name x:u.x y:u.y angle:float(u.team) team:1];
		r.hudOn = r.hudOn && hud_on;
		[r setUiWidth:w height:h scale:ui_scale];  // the UI the scripts query before any frame is drawn
		[r setScriptLog:script_log];
		[r setPanels:panel_flags streamlined:streamlined help:help_level];
		if (speed_arg >= 0) [r setGameSpeed:speed_arg];
		if (progress_arg) [r setProgressButtons:YES];
		if (!std::isnan(hover_x)) [r setHoverX:hover_x y:hover_y];
		if (const char* t = getenv("RON_POPUP_TEST")) [r testPopup:t];
		if (const char* t = getenv("RON_GAME_MENU")) [r openGameMenu:std::atoi(t)];  // 1 the game menu, 2 game over, 3 the Options page
		if (const char* t = getenv("RON_CARRIER_TEST")) [r harnessCarrier:t];       // "WHO:O" turns that unit into a Carrier (no stock scenario has one)
		if (play >= 0) [r setLocalPlayer:play];
		if (sel_who >= 0) [r selectWho:sel_who objects:sel_objs buildings:sel_builds];
		if (build_page >= 0) [r setBuildPage:build_page];
		if (place_type >= 0) {
			[r startPlacement:place_type];
			[r previewPlacementAtWorldX:place_x y:place_y];
		}
		if (place_type >= 0 && place_click) [r placeAtWorldX:place_x y:place_y modifiers:0];
		for (const QueueArg& q : queues) [r queueWho:q.who building:q.o type:q.type count:q.n];
		if (load_game && ![r loadGameFrom:load_game]) {
			std::fprintf(stderr, "--load-game failed\n");
			return 1;
		}
		if (r.playbackActive && pb_seek1 >= 0) {   // the seek test: no display, the exit code is the verdict
			const int rc = [r pbSeekTest:pb_seek1 to:pb_seek2];
			std::fflush(stdout);
			return rc;
		}
		if (r.playbackActive && (frames_given || snapshot)) {  // headless: the recording runs to the frame asked for (default: its end), then the report
			r.onPlayRecording = ^(NSString* path) { std::printf("[view playback] %s\n", path.UTF8String); };
			if (pb_seat >= 0) [r pbSelectWho:pb_seat];
			r.fogOn = pb_fog;
			[r pbRunHeadless:frames_given ? sim_frames : -1];
			if (!snapshot) {
				std::fflush(stdout);
				return 0;
			}
			sim_frames = 0;
		}
		if (r.multiplayer && !mpTypes.empty()) [r mpTypeScript:mpTypes];
		if (r.multiplayer && ![r mpRunHeadless:sim_frames]) {
			std::fprintf(stderr, "the multiplayer game did not run to frame %d\n", sim_frames);
			[r mpClose];
			return 3;
		}
		if (r.multiplayer) {   // tests: the end-game window after the run (RON_GAME_MENU_AFTER=2), and a click on one of its buttons (RON_GAME_MENU_CLICK=2: View Playback)
			r.onPlayRecording = ^(NSString* path) { std::printf("[view playback] %s\n", path.UTF8String); };
			if (const char* t = getenv("RON_GAME_MENU_AFTER")) [r openGameMenu:std::atoi(t)];
			if (const char* c = getenv("RON_GAME_MENU_CLICK")) [r harnessGameMenuChoose:std::atoi(c)];
		}
		if (ctwMpSess && ctwMpRoom) {
			// the battle is decided (or adjudicated by the units left): the host's outcome moves the host's campaign, the guests copy the result
			const int outcome = [r ctwAdjudicated];
			const uint64_t mine = [&]() {
				uint64_t h = 1469598103934665603ull;
				for (uint8_t c : ctwMpSess->game.save_bytes()) h = (h ^ c) * 1099511628211ull;
				return h;
			}();
			std::printf("[%s] ctw battle: frame %d, local outcome %s\n", ctwMpRoom->is_host() ? "host" : "join", sim_frames, outcome == 1 ? "victory" : "defeat");
			if (ctwMpRoom->is_host()) {
				mp::ctw_host_result(*ctwMpRoom, *ctwMpSess, outcome);
				[r mpClose];
				uint64_t after = 1469598103934665603ull;
				for (uint8_t c : ctwMpSess->game.save_bytes()) after = (after ^ c) * 1099511628211ull;
				std::printf("[host] campaign after the battle: %s; fnv64 %016llx\n", ctwMpSess->last_result.c_str(), (unsigned long long)after);
				ctwMpRoom->transport().idle(200);
			} else {
				[r mpClose];
				uint32_t seq = 0;
				std::vector<uint8_t> camp;
				int host_outcome = -1;
				const double t0 = CACurrentMediaTime();
				while (!mp::ctw_guest_result(*ctwMpRoom, seq, camp, host_outcome) && CACurrentMediaTime() - t0 < 20) {
					ctwMpRoom->lobby().poll();
					ctwMpRoom->transport().idle(20);
				}
				uint64_t got = 1469598103934665603ull;
				for (uint8_t c : camp) got = (got ^ c) * 1099511628211ull;
				std::printf("[join] campaign copy from the host: %zu bytes, host outcome %d, fnv64 %016llx (my copy before the battle %016llx)\n", camp.size(), host_outcome, (unsigned long long)got,
				            (unsigned long long)mine);
				return camp.empty() ? 3 : 0;
			}
			return 0;
		}
		if (long_run > 0 && r.simulating && !r.multiplayer) {
			[r longRun:long_run snapshotPrefix:long_run_prefix total:sim_frames width:w height:h scale:ui_scale];
			std::fflush(stdout);
			[r mpClose];
			return 0;  // headless: never opens the window
		}
		[r applyVictoryOverride];
		// Scripted moves, then N simulation frames (headless checks with --snapshot).
		for (int f = 0; f <= sim_frames && r.simulating && !r.multiplayer; f++) {
			for (const MoveArg& m : moves) {
				if (m.frame == f && ![r issueMoveWho:m.who objects:m.objs x:m.x y:m.y])
					std::fprintf(stderr, "--move for leader %d issued nothing\n", m.who);
			}
			for (const AttackArg& k : attacks) {
				if (k.frame == f && ![r issueAttackWho:k.who objects:k.objs targetWho:k.twho target:k.to])
					std::fprintf(stderr, "--attack for leader %d issued nothing\n", k.who);
			}
			for (const TimedSelect& t : timed_selects) {
				if (t.frame == f) [r selectWho:t.who objects:t.objs buildings:t.builds];
			}
			for (const TimedOption& t : timed_options) {
				if (t.frame != f) continue;
				// As the card runs it: the option's button, then the page and the building button.
				[r runCardOption:(t.option < 0 ? hud::kOptCancel : t.option) object:-1];
				if (t.menu >= 0) [r setBuildPage:t.menu];
				if (t.type >= 0) [r runCardOption:1000 object:t.type];
			}
			for (const TimedCursor& t : timed_cursors) {
				if (t.frame == f) [r setCursorWorldX:t.x y:t.y];
			}
			for (const TimedCursor& t : timed_looks) {
				if (t.frame == f) [r lookAtX:t.x y:t.y];
			}
			for (const TimedZoom& t : timed_zooms) {
				if (t.frame == f) r.zoom = t.level;
			}
			for (const std::string& ms : mouse_script) {
				const size_t colon = ms.find(':'), at = ms.find('@');
				float mx = 0, my = 0;
				std::sscanf(ms.c_str() + colon + 1, "%f,%f", &mx, &my);
				if ((at == std::string::npos ? 0 : std::atoi(ms.c_str() + at + 1)) == f) [r mouseEvent:ms.substr(0, colon) x:mx y:my];
			}
			for (const std::string& k : timed_keys) {
				const size_t at = k.find('@');
				if ((at == std::string::npos ? 0 : std::atoi(k.c_str() + at + 1)) == f) [r runKeyString:k.substr(0, at)];
			}
			for (const auto& [k, send] : timed_chats) {
				const size_t at = k.rfind('@');
				if ((at == std::string::npos ? 0 : std::atoi(k.c_str() + at + 1)) == f) [r chatScript:k.substr(0, at) send:send];
			}
			for (const auto& [opt, spec] : timed_groups) {
				const size_t at = spec.rfind('@');
				if ((at == std::string::npos ? 0 : std::atoi(spec.c_str() + at + 1)) != f) continue;
				const std::string v = spec.substr(0, at);
				if (opt == "--group") [r nameGroupScript:v];
				else if (opt == "--recall-group") [r recallGroup:std::atoi(v.c_str())];
				else if (![r regroupGroup:std::atoi(v.c_str())]) std::fprintf(stderr, "--regroup %s issued nothing\n", v.c_str());
			}
			if (f < sim_frames) [r stepSimulation:1];
		}
		if (ctwSess) {
			int outcome = [r ctwAdjudicated];
			const bool decided = [r ctwOutcome] >= 0;
			ctwSess->battle_finished(outcome);  // last_result: the first news line the outcome produced
			std::printf("ctw battle: frame %d, %s%s; campaign: %s\n", sim_frames, outcome == 1 ? "victory" : "defeat", decided ? "" : " (adjudicated: more units and buildings)",
			            ctwSess->last_result.c_str());
		}
		if (select_all) [r selectAll];
		if (census) [r printCensus];
		if (save_game && ![r saveGameTo:save_game]) {
			std::fprintf(stderr, "--save-game failed\n");
			return 1;
		}
		if (perf_frames > 0) {  // headless: print and exit (a --snapshot after it is not taken)
			[r perfRun:perf_frames width:w height:h scale:ui_scale];
			std::fflush(stdout);
			[r mpClose];
			return 0;
		}
		if (snapshot) {
			const BOOL shot = [r snapshotTo:snapshot width:w height:h scale:ui_scale];
			[r mpClose];
			return shot ? 0 : 1;
		}

		MapAppDelegate* d = [MapAppDelegate new];
		d.device = device;
		d.renderer = r;
		d.gameDir = gameDir;
		run_app(d);
	}
	return 0;
}
