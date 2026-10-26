// The front end (docs/ui-port.md "Front end"). The original's pieces it reproduces (decompiled in re/decomp/frontend):
//   - TopMenu::on_redraw @0059c100: the BACKGROUND window's ANIM list (topmenu.xml) - each layer a full-screen quad of
//     its 1280x720 texture, shifted by (move * t + start_offset) * screen size, drawn in IFACETEXLOAD sort order with
//     render type 7; the whole picture fades in from black between 0.5 and 1.5 s and out over the last second of the
//     animation's duration, then the next animation starts (all four cycle). static_bg_tex (iface_wrapper1.tga) when
//     there are no animations.
//   - TopMenu::draw_logo @0059cff0: the EE logo is a quad of ron_logo_texture_map.tga lit through
//     ron_logo_normal_map.tga by a light whose direction follows the mouse around the logo's centre
//     (dir = normalize(k * unit(centre - mouse), -1), k = |logo size| / screen width). Wrapper_Main_Logo_01.tga is the
//     flat fallback the original uses on weak GPUs.
//   - TopMenuSelection::on_redraw @0059ee80 / on_button_redraw @0059eac0: the title ribbon (3-slice expander into
//     IFACERECT title, Georgia 22 white with a black shadow), the bronze IFACEBORDER "border" around the window below
//     the ribbon (top + title height), buttons from button_left / button_mid (tiled) / button_right at their 37 px
//     height: the atlas row one piece-height up when hovered / focused / pressed, one down when disabled; caption
//     font 18, white with a (64,64,64) shadow, centred, moved (+2, +2) when enabled. The window is sized around its
//     buttons with 41 px margins (TopMenuSelection::exec).
//   - TopMenu::on_window_redraw @0059cd20: the help text of the hovered button, font 16, white.
// The screens, their copy and their placement follow the user's design (Menus.dc.html); the controls the original
// draws in code (combo box, list, popup) use the install's GAMESPYREJOIN combo / large-button / edit-border pieces,
// SCROLLCLASS and POPUPSTYLE default.
#import "frontend.h"

#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "assets/cursors.hpp"
#include "assets/iface_layout.hpp"
#include "assets/tga.hpp"
#include "assets/xml.hpp"
#include "audio.hpp"
#include "keymap.hpp"
#include "options_ui.hpp"
#include "prefs.hpp"
#include "recplay.hpp"
#include <ctime>
#include "renderer.hpp"
#include "sim/rules.hpp"
#include "uistr.hpp"
#include "sim/save.hpp"
#include "ui.hpp"
#import "launcher.h"
#import "ctw_ui.h"

namespace {

namespace rm = ron::metal;
using ron::IfacePiece;
using ron::IfaceRect;
using rm::argb;
using rm::rgb;
using rm::UiType;

constexpr int kW = 1024, kH = 768;  // the design canvas (the front end's 1024 x 768 backgrounds)
constexpr uint32_t kWhite = 0xFFFFFFFF, kBlack = 0xFF000000;
const uint32_t kLabel = rgb(212, 212, 212), kSecondary = rgb(189, 205, 216), kButtonShadow = rgb(64, 64, 64);

const char* const kDifficulty[6] = {"Easiest", "Easy", "Moderate", "Tough", "Tougher", "Toughest"};
struct MainItem {
	const char* label;
	const char* help;
	bool enabled;
};
constexpr int kMainCount = 8;
const MainItem kMainItems[kMainCount] = {
    {"Tutorials", "Learn to play, step by step. Start with Tutorial 1.", true},
    {"Skirmish", "Battle computer opponents on a ready-made map, or set up a random map.", true},
    {"Scenarios", "Chess exercises and the battle maps of the Conquer the World campaigns.", true},
    {"Conquer the World", "Lead a nation through a campaign: move armies on the world map, make deals, play the battles.", true},
    {"Multiplayer", "Play with friends on a local network: one player hosts a room, the others join it by address.", true},
    {"Recorded Games", "Watch your recorded multiplayer games again: switch between the players, see the whole map or one player's view, pause and change the speed.", true},
    {"Options", "Game speed, scroll speed, sound volumes, the interface layout, the key map and the language.", true},
    {"Quit", "Leave Rise of Nations and return to the desktop.", true},
};

enum Screen { kMain = 0, kTutorials, kSkirmish, kScenarios, kSkirmishSetup, kCtw, kCtwMap, kMpConnect, kMpLobby, kOptions, kRecorded, kScreenCount };
const char* const kScreenNames[kScreenCount] = {"main", "tutorials", "skirmish", "scenarios", "skirmish-setup", "ctw", "ctw-map", "multiplayer", "mp-lobby", "options", "recorded"};
const char* const kListTitles[kScreenCount] = {"Main Menu", "Tutorials", "Skirmish", "Scenarios", "Skirmish Setup", "Conquer the World", "Conquer the World", "Multiplayer", "Multiplayer Lobby", "Options", "Recorded Games"};
const char* const kListNotes[kScreenCount] = {
    "",
    "Start with Tutorial 1. The tutorials guide you with voice-overs, arrows and objectives.",
    ("You play the map's human player against computer opponents. Press Random Map to choose a map style, its size, "
     "the nations and the teams, and play on a map made for you."),
    "Chess exercises and the battle maps of the Conquer the World campaigns.",
    "",
    "",
    "",
    "",
    "",
    "",
    "Your recorded multiplayer games. Choose one and press Watch: the game plays again from its recording, and you can change whose view you see.",
};

// Hit-test ids.
enum : int {
	kNone = -1,
	kMainBtn = 0,  // + item
	kChange = 10,
	kBack = 11,
	kPlay = 12,
	kDrop = 13,
	kDropItem = 20,  // + difficulty
	kScrollUp = 30,
	kScrollDown = 31,
	kThumb = 32,
	kTrack = 33,
	kPopupBtn = 40,  // + button
	kRow = 100,      // + entry
	kRecOpen = 17,   // the Recorded Games list's Open File... button
	kRandomMap = 14,  // the Skirmish list's Random Map… button
	kSeedField = 15,  // the setup screen's seed field and its Random button
	kSeedRandom = 16,
	kSetupCombo = 1000,  // + combo of the setup screen
	kSetupItem = 2000,   // + visible item of its open list
	kMpName = 2100,      // the Multiplayer screens
	kMpPort = 2101,
	kMpAddr = 2102,
	kMpHost = 2103,
	kMpJoin = 2104,
	kMpChat = 2105,
	kMpLoad = 2106,
	kMpKick = 2200,       // + peer: the host's Kick button under a player's name
	kMpAllow = 2210,      // the host's decision on a guest's request (approval mode "Ask")
	kMpDeny = 2211,
	kMpObserver = 2212,   // Watch only / Play
	kMpScenario = 2213,   // Scenario... / Random Map
	kMpCtw = 2214,        // Conquer the World...
	kMpApproval = 2215,   // Guests choose: Automatically / Ask me
	kMpList = 2300,       // + row of the LAN list of games on the connect screen
};

struct AnimLayer {
	IfacePiece piece;
	float move_x = 0, move_y = 0, off_x = 0, off_y = 0;
	int sort = 0;
};
struct Anim {
	float duration = 30;
	std::vector<AnimLayer> layers;  // in draw order (IFACETEXLOAD sort, as flush_all submits the packets)
};

constexpr int kRowH = 40;

// The skirmish setup screen's controls (rebuilt from the setup every frame, frontend.mm "the skirmish setup screen").
struct SetupCombo {
	IfaceRect r;
	std::vector<std::string> items;
	int sel = 0;
	bool enabled = true;
	int seat = -1;  // player table row, -1 for a rules combo
	int mpf = 0;    // lobby: a SeatField (1 nation .. 4 handicap) a guest asks the host for; 0 = edited directly
	std::string label;
	int lx = 0, ly = 0;
	std::function<void(int)> set;
};
struct SetupText {
	int x, y;
	std::string text;
	int kind;  // 0 label, 1 dimmed, 2 player name
};
struct SetupLists {
	bool loaded = false;
	std::vector<std::string> style, size, town, resources, start, end, victory, elim, pop, reveal, diff, tribes, handicap;
};

// A line of the lobby's chat log: kind 0 chat, 1 system, 2 team chat, 3 whisper; `prefix` ("Name: ") is drawn in `color`.
struct MpLine {
	int kind = 0;
	std::string text;
	std::string prefix;
	uint32_t color = 0xFFFFFFFFu;
	MpLine() = default;
	MpLine(int k, std::string t) : kind(k), text(std::move(t)) {}
};

struct Layout {
	IfaceRect title, title_text, frame;
	std::vector<IfaceRect> buttons;
	IfaceRect info, edit, change;
	int folder_y = 0, label_x = 0;
	IfaceRect list_frame, list, scroll, up, down, track, thumb;
	IfaceRect drop, back, play, rand;
	std::vector<IfaceRect> drop_items;
	int note_y = 0;
	int rows = 0;
};

const char* kLogoShader = R"(
#include <metal_stdlib>
using namespace metal;
struct LogoV { float4 pos [[position]]; float2 uv; };
struct LogoQuad { float4 rect; float4 view; float4 light; };  // rect x, y, w, h (UI px); view w, h; light xyz, flip
vertex LogoV logo_vertex(uint vid [[vertex_id]], constant LogoQuad& q [[buffer(0)]]) {
	const float2 c[4] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
	float2 p = q.rect.xy + c[vid] * q.rect.zw;
	LogoV o;
	o.pos = float4(p.x / q.view.x * 2.0 - 1.0, 1.0 - p.y / q.view.y * 2.0, 0.0, 1.0);
	o.uv = c[vid];
	return o;
}
fragment float4 logo_fragment(LogoV in [[stage_in]], texture2d<float> diffuse [[texture(0)]], texture2d<float> normals [[texture(1)]],
                              sampler s [[sampler(0)]], constant LogoQuad& q [[buffer(0)]]) {
	float4 c = diffuse.sample(s, in.uv);
	float3 n = normals.sample(s, in.uv).xyz * 2.0 - 1.0;
	n.y *= q.light.w;
	n = normalize(n);
	float ndl = max(dot(n, normalize(q.light.xyz)), 0.0);
	float3 h = normalize(normalize(q.light.xyz) + float3(0, 0, 1));
	float spec = pow(max(dot(n, h), 0.0), 24.0) * 0.35;
	return float4(saturate(c.rgb * (0.42 + 0.78 * ndl) + spec * c.a), c.a);
}
// The project's main-menu logo (premultiplied PNG): light.x = 0 draws its soft drop shadow (the alpha of a coarse mip,
// black at 60 %), 1 the image.
fragment float4 image_fragment(LogoV in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],
                               constant LogoQuad& q [[buffer(0)]]) {
	if (q.light.x < 0.5) return float4(0, 0, 0, t.sample(s, in.uv, level(q.light.y)).a * 0.6);
	return t.sample(s, in.uv);
}
)";

std::vector<std::string> wrap_text(rm::UiRenderer& ui, int f, const std::string& text, int width) {
	std::vector<std::string> lines;
	std::string line, word;
	auto flush_word = [&]() {
		if (word.empty()) return;
		const std::string cand = line.empty() ? word : line + " " + word;
		if (!line.empty() && ui.text_width(f, cand) > width) {
			lines.push_back(line);
			line = word;
		} else {
			line = cand;
		}
		word.clear();
	};
	for (char ch : text) {
		if (ch == ' ') flush_word();
		else word += ch;
	}
	flush_word();
	if (!line.empty()) lines.push_back(line);
	return lines;
}

}  // namespace

NSCursor* ron_game_cursor(const ron::GameFiles& files, int cid) {
	static std::vector<ron::CursorRow> rows;
	static std::string rows_root;
	if (rows_root != files.root()) rows = ron::load_cursors(files), rows_root = files.root();
	if (cid < 0 || size_t(cid) >= rows.size()) return [NSCursor arrowCursor];
	const ron::CursorRow& r = rows[size_t(cid)];
	try {
		const ron::Image img = ron::parse_tga(ron::read_file(files.resolve(r.file)));
		NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil pixelsWide:r.xs pixelsHigh:r.ys bitsPerSample:8
		                                                         samplesPerPixel:4 hasAlpha:YES isPlanar:NO
		                                                          colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:r.xs * 4 bitsPerPixel:32];
		for (int y = 0; y < r.ys; y++) {
			for (int x = 0; x < r.xs; x++) {
				const int sx = r.x + x, sy = r.y + y;
				uint8_t* d = rep.bitmapData + (y * r.xs + x) * 4;
				if (sx < img.width && sy < img.height) std::memcpy(d, &img.rgba[size_t(sy * img.width + sx) * 4], 4);
				else std::memset(d, 0, 4);
			}
		}
		NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(r.xs, r.ys)];
		[image addRepresentation:rep];
		return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(r.hot_x, r.hot_y)];
	} catch (const std::exception&) {
		return [NSCursor arrowCursor];
	}
}

@implementation FrontEnd {
	id<MTLDevice> _device;
	id<MTLCommandQueue> _queue;
	NSString* _dir;
	std::unique_ptr<ron::GameFiles> _files;
	std::unique_ptr<ron::IfaceLayout> _iface, _top;
	rm::UiRenderer _ui;
	// topmenu.xml / interface.xml pieces
	std::vector<Anim> _anims;
	IfacePiece _staticBg, _flatLogo;
	IfacePiece _btn[3], _ribbon[3];
	ron::IfaceBorder _border, _thin;
	IfaceRect _titleRect, _titleText;
	IfaceRect _btnRect;  // first MAINMENU button, window-relative
	int _btnInc;
	IfacePiece _popBtn[3][3];  // normal, highlight, down x 3 slices
	uint32_t _popBg;
	IfacePiece _combo[4], _large[3];
	IfacePiece _scrollUp, _scrollDown, _scrollBack[3], _scrollSlider[3];
	// the bump-mapped logo
	id<MTLRenderPipelineState> _logoPipe;
	id<MTLSamplerState> _logoSampler;
	id<MTLTexture> _logoTex, _logoNormal;
	id<MTLRenderPipelineState> _imagePipe;
	id<MTLTexture> _menuLogo;  // apps/map-mac/resources/menu-logo.png (the app bundle's Resources when packaged)
	// state
	Screen _screen;
	std::vector<launcher::Entry> _entries;
	recplay::Scanner _recScan;                 // the Recorded Games list: files found at once, headers read on a thread
	uint32_t _recSeen;
	std::vector<recplay::Row> _recRows;
	std::string _recDir;
	int _sel, _top0;  // selected entry, first visible row
	int _difficulty;
	int _focus;       // main menu: keyboard focus (button index)
	int _hover, _pressed;
	BOOL _dropOpen;
	BOOL _dragThumb;
	int _dragOffset;
	std::string _help;
	struct Popup {
		bool open = false;
		int kind = 0;  // 0 quit, 1 error, 2 busy
		std::string text;
		std::vector<std::string> buttons;
		std::vector<IfaceRect> rects;
	} _popup;
	// animation / time
	int _anim;
	double _animT, _timeMs, _lastFrame;
	BOOL _interactive;
	std::unique_ptr<ron::audio::Audio> _audio;
	std::vector<std::pair<std::string, std::vector<std::string>>> _soundGlobals;
	std::vector<std::pair<std::string, std::string>> _soundFiles;
	// view mapping (set every frame)
	float _fit;          // points per canvas pixel
	int _uiW, _uiH;      // the window in canvas pixels
	int _ox, _oy;        // the canvas origin in the window
	CGPoint _mouse;      // canvas pixels
	BOOL _mouseKnown;
	double _lastClickMs;
	int _lastClickRow;
	NSCursor* _cursor;
	// the skirmish setup screen (random maps; ron::sim::RandomGameSetup)
	ron::sim::RandomGameSetup _setup;
	SetupLists _lists;
	std::vector<SetupCombo> _sc;
	std::vector<SetupText> _st;
	IfaceRect _seedRect, _seedBtn, _setupListRect;
	int _rulesBottom, _setupListVis, _openCombo, _openTop;
	BOOL _seedRandom, _seedFocus;
	std::string _seedText;
	std::string _ctwSub;  // headless: the sub-screen to open
	// Multiplayer (lobby.h): the connect screen's fields and the room
	std::shared_ptr<mp::Room> _room;
	std::string _mpName, _mpPort, _mpAddr, _mpChat, _mpMessage;
	int _mpField;          // focused field: 0 name, 1 port, 2 address, 3 chat, -1 none
	std::string _mpPublished;
	IfaceRect _mpRect[3], _mpChatRect, _mpLogRect;
	size_t _mpChatSeen;
	BOOL _mpStarting;
	std::shared_ptr<std::vector<uint8_t>> _mpSave;  // the host's chosen saved game (the session continues it)
	std::string _mpSaveText;
	IfaceRect _mpLoadRect;
	std::vector<MpLine> _mpLines;  // the lobby's log
	ron::net::LanBrowser _lan;     // the connect screen's list of games on the network
	int _lanSel;                   // selected row, -1 none
	IfaceRect _lanRect[6], _lanBox, _mpHostBtn, _mpJoinBtn;
	std::array<IfaceRect, 8> _mpKickRect;
	IfaceRect _mpAllowRect, _mpDenyRect, _mpObsRect, _mpScenRect, _mpCtwRect, _mpApprovalRect;
	std::vector<std::array<int, 3>> _mpSwatch;   // x, y, colour slot: the colour square of each seat
	mp::ScenarioInfo _mpScen;                    // the host's chosen scenario (or a guest's copy of it)
	BOOL _mpScenOn;
	uint32_t _mpCtwSeen;                         // the last campaign result a guest took
	BOOL _mpCtwOn;                               // the host is choosing a Conquer the World battle (the screens are the campaign's)
	std::string _mpCtwText;
	// The Options page (options_ui.hpp) and the rules it names the Hotkeys pane's commands from
	std::unique_ptr<optui::Page> _opts;
	std::unique_ptr<ron::sim::Rules> _optRules;
	// Conquer the World (ctw_ui.h): the screens and their drawing bridge
	std::unique_ptr<ctwui::Screen> _ctw;
	ctwui::Bridge _ctwb;
}

- (NSString*)gameDir {
	return _dir;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device gameDir:(NSString*)dir error:(NSString* __autoreleasing*)error {
	if (!(self = [super init])) return nil;
	_device = device;
	_queue = [device newCommandQueue];
	_dir = dir;
	_fit = 1, _uiW = kW, _uiH = kH;
	_hover = _pressed = kNone;
	_lastClickRow = -1;
	std::string err;
	try {
		_files = std::make_unique<ron::GameFiles>(dir.UTF8String);
		_iface = std::make_unique<ron::IfaceLayout>(_files->read_text("Data/interface.xml"));
		_top = std::make_unique<ron::IfaceLayout>(_files->read_text("Data/topmenu.xml"));
	} catch (const std::exception& e) {
		if (error) *error = [NSString stringWithFormat:@"Could not read the game's menu data: %s", e.what()];
		return nil;
	}
	if (!_ui.init(device, _queue, MTLPixelFormatBGRA8Unorm, _files.get(), _iface.get(), err)) {
		if (error) *error = @(err.c_str());
		return nil;
	}
	_opts = std::make_unique<optui::Page>(*_files, *_iface);
	// topmenu.xml's atlas ids (TEX_TOPMENU_0, TEX_WRAPPER_LOGO, ANIM_*, TEX_WRAPPER_4) next to interface.xml's.
	std::map<std::string, int> sort_of;
	ron::for_each_element(_top->root(), "IFACETEXLOAD", [&](const ron::XmlElement& e) {
		const std::string id = ron::iface_attr(e, "id");
		if (!sort_of.count(id)) sort_of[id] = ron::iface_int(ron::iface_attr(e, "sort"));
	});
	for (const auto& [id, file] : _top->texloads()) _ui.add_texload(id, file);

	const ron::XmlElement* bg = _top->find("BACKGROUND");
	const ron::XmlElement* sel = _top->find("topmenuselection");
	const ron::XmlElement* mm = _top->find("MAINMENU");
	if (!bg || !sel || !mm) {
		if (error) *error = @"Data/topmenu.xml lacks the TopMenu windows";
		return nil;
	}
	for (const ron::XmlElement& a : bg->children) {
		if (!ron::iface_tag_is(a, "ANIM")) continue;
		Anim an;
		an.duration = a.get_float("duration", 30);
		for (const ron::XmlElement& l : a.children) {
			if (!ron::iface_tag_is(l, "ANIMLAYER")) continue;
			AnimLayer layer;
			layer.piece = ron::iface_find_piece(*bg, ron::iface_attr(l, "texture"));
			layer.move_x = l.get_float("move_x"), layer.move_y = l.get_float("move_y");
			layer.off_x = l.get_float("start_offset_x"), layer.off_y = l.get_float("start_offset_y");
			layer.sort = sort_of.count(layer.piece.texture) ? sort_of[layer.piece.texture] : 0;
			if (layer.piece.valid() && !_files->resolve(_top->texload_file(layer.piece.texture)).empty()) an.layers.push_back(layer);
		}
		std::stable_sort(an.layers.begin(), an.layers.end(), [](const AnimLayer& x, const AnimLayer& y) { return x.sort < y.sort; });
		if (!an.layers.empty() && an.duration > 0) _anims.push_back(an);
	}
	_staticBg = ron::iface_find_piece(*bg, "static_bg_tex");
	_flatLogo = ron::iface_find_piece(*bg, "logo");
	_thin = ron::iface_find_border(*bg, "motd_border");
	_btn[0] = ron::iface_find_piece(*sel, "button_left");
	_btn[1] = ron::iface_find_piece(*sel, "button_mid");
	_btn[2] = ron::iface_find_piece(*sel, "button_right");
	for (int k = 0; k < 3; k++) _ribbon[k] = ron::iface_find_piece(*sel, "ribbon", k);
	_border = ron::iface_find_border(*sel, "border");
	ron::iface_find_rect(*sel, "IFACERECT", "title", kW, kH, _titleRect);
	ron::iface_find_rect(*sel, "IFACERECT", "title_text", kW, kH, _titleText);
	const auto kids = ron::iface_layout_children(*mm, 548, 768);
	_btnRect = kids.empty() ? IfaceRect{41, 495, 114, 149} : kids[0].rect;
	_btnInc = kids.size() > 1 ? kids[1].rect.t - kids[0].rect.t : 36;
	// interface.xml: POPUPSTYLE default, the GameSpy dialog's combo box / large buttons, SCROLLCLASS.
	_popBg = argb(224, 39, 40, 43);
	if (const ron::XmlElement* pop = _iface->find("POPUP")) {
		for (const ron::XmlElement& st : pop->children) {
			if (!ron::iface_tag_is(st, "POPUPSTYLE") || ron::iface_attr(st, "name") != "default") continue;
			const char* kinds[3] = {"butnormal", "buthighlight", "butdown"};
			for (int s = 0; s < 3; s++) {
				for (int k = 0; k < 3; k++) _popBtn[s][k] = ron::iface_find_piece(st, std::string(kinds[s]) + std::to_string(k));
			}
			if (const ron::XmlElement* c = ron::iface_child(st, "IFACECOLOR", "bg_color")) _popBg = ron::iface_color(*c);
		}
	}
	if (const ron::XmlElement* gs = _iface->find("gamespyrejoin")) {
		const char* names[4] = {"combo_left", "combo_mid", "combo_right", "combo_arrow"};
		for (int k = 0; k < 4; k++) _combo[k] = ron::iface_find_piece(*gs, names[k]);
		for (int k = 0; k < 3; k++) _large[k] = ron::iface_find_piece(*gs, "large_button" + std::to_string(k));
	}
	if (const ron::XmlElement* sc = _iface->find("SCROLLCLASS")) {
		_scrollUp = ron::iface_find_piece(*sc, "button_up");
		_scrollDown = ron::iface_find_piece(*sc, "button_down");
		for (int k = 0; k < 3; k++) _scrollBack[k] = ron::iface_find_piece(*sc, "back_vert", k);
		for (int k = 0; k < 3; k++) _scrollSlider[k] = ron::iface_find_piece(*sc, "slider_vert", k);
	}
	[self loadLogo:_top->find("LOGO")];
	[self loadMenuLogo];
	// Sounds: soundglobals KEY -> soundfiles tag -> file (SoundGlobal::play).
	try {
		const ron::XmlElement sg = ron::parse_xml(_files->read_text("Data/soundglobals.xml"));
		ron::for_each_element(sg, "GLOBAL", [&](const ron::XmlElement& e) {
			const ron::XmlElement* k = e.child("KEY");
			if (!k) return;
			std::vector<std::string> tags;
			for (const ron::XmlElement& c : e.children) {
				if (c.name == "SOUND") tags.push_back(ron::to_lower(c.get("#text")));
			}
			_soundGlobals.push_back({k->get("#text"), tags});
		});
		const ron::XmlElement sf = ron::parse_xml(_files->read_text("Data/soundfiles.xml"));
		ron::for_each_element(sf, "SOUNDFILE", [&](const ron::XmlElement& e) {
			_soundFiles.push_back({ron::to_lower(e.get("tag")), ron::normalize_game_path(e.get("file"))});
		});
	} catch (const std::exception& e) {
		std::fprintf(stderr, "menu sounds: %s\n", e.what());
	}
	NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
	_difficulty = [d objectForKey:launcher::kDefDifficulty] ? int(std::clamp<NSInteger>([d integerForKey:launcher::kDefDifficulty], 0, 5)) : 2;
	_help = kMainItems[0].help;
	_animT = 0;
	if (const char* a = getenv("RON_MENU_ANIM")) {  // "N,T": animation N at T seconds (snapshot checks)
		std::sscanf(a, "%d,%lf", &_anim, &_animT);
		_anim = _anims.empty() ? 0 : std::clamp(_anim, 0, int(_anims.size()) - 1);
	} else {
		_animT = 6.0;  // headless renders show the picture after its fade-in; startInteractive restarts it
	}
	_mouse = CGPointMake(300, 250);  // a light from the menu side until the mouse moves
	[self showScreen:@(kScreenNames[0])];
	return self;
}

- (id<MTLTexture>)textureFromFile:(const std::string&)rel {
	const std::string path = _files->resolve(rel);
	if (path.empty()) return nil;
	try {
		const ron::Image img = ron::parse_tga(ron::read_file(path));
		MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		                                                                             width:NSUInteger(img.width)
		                                                                            height:NSUInteger(img.height)
		                                                                         mipmapped:YES];
		d.usage = MTLTextureUsageShaderRead;
		id<MTLTexture> t = [_device newTextureWithDescriptor:d];
		[t replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(img.width), NSUInteger(img.height)) mipmapLevel:0 withBytes:img.rgba.data()
		     bytesPerRow:NSUInteger(img.width) * 4];
		id<MTLCommandBuffer> cb = [_queue commandBuffer];
		id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
		[b generateMipmapsForTexture:t];
		[b endEncoding];
		[cb commit];
		return t;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "menu texture %s: %s\n", rel.c_str(), e.what());
		return nil;
	}
}

// topmenu.xml LOGO: LOGODIFFUSE texture + normalmap (TopMenu::init_logo).
- (void)loadLogo:(const ron::XmlElement*)logo {
	if (getenv("RON_FLAT_LOGO")) return;
	const ron::XmlElement* dif = logo ? logo->child("LOGODIFFUSE") : nullptr;
	if (!dif) return;
	_logoTex = [self textureFromFile:ron::normalize_game_path(dif->get("texture"))];
	_logoNormal = [self textureFromFile:ron::normalize_game_path(dif->get("normalmap"))];
	if (!_logoTex || !_logoNormal) {
		_logoTex = _logoNormal = nil;
		return;
	}
	NSError* err = nil;
	id<MTLLibrary> lib = [_device newLibraryWithSource:@(kLogoShader) options:nil error:&err];
	MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"logo_vertex"];
	pd.fragmentFunction = [lib newFunctionWithName:@"logo_fragment"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pd.colorAttachments[0].blendingEnabled = YES;
	pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
	pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
	_logoPipe = lib ? [_device newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!_logoPipe) {
		std::fprintf(stderr, "logo shader: %s\n", err.localizedDescription.UTF8String);
		_logoTex = _logoNormal = nil;
		return;
	}
	MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
	sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.mipFilter = MTLSamplerMipFilterLinear;
	sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	_logoSampler = [_device newSamplerStateWithDescriptor:sd];
}

// The main-menu logo the project ships (the user's adaptation of the EE logo): Contents/Resources in the app, else
// apps/map-mac/resources next to a dev build (build/ronmap) or this source file. Missing: the install's logo.
- (void)loadMenuLogo {
	if (getenv("RON_INSTALL_LOGO")) return;
	NSMutableArray<NSString*>* paths = [NSMutableArray array];
	if (NSString* p = [NSBundle.mainBundle pathForResource:@"menu-logo" ofType:@"png"]) [paths addObject:p];
	NSString* exeDir = NSBundle.mainBundle.executablePath.stringByDeletingLastPathComponent;
	if (exeDir) {
		[paths addObject:[exeDir stringByAppendingPathComponent:@"../Resources/menu-logo.png"]];
		[paths addObject:[exeDir stringByAppendingPathComponent:@"../apps/map-mac/resources/menu-logo.png"]];
		[paths addObject:[exeDir stringByAppendingPathComponent:@"resources/menu-logo.png"]];
	}
	[paths addObject:[@(__FILE__).stringByDeletingLastPathComponent stringByAppendingPathComponent:@"resources/menu-logo.png"]];
	NSBitmapImageRep* rep = nil;
	for (NSString* p in paths) {
		NSData* data = [NSData dataWithContentsOfFile:p.stringByStandardizingPath];
		if (data && (rep = [NSBitmapImageRep imageRepWithData:data])) break;
	}
	CGImageRef img = rep.CGImage;
	if (!img) return;
	if (!_logoSampler) {
		MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
		sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
		sd.mipFilter = MTLSamplerMipFilterLinear;
		sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
		_logoSampler = [_device newSamplerStateWithDescriptor:sd];
	}
	const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
	std::vector<uint8_t> px(w * h * 4);
	CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	CGColorSpaceRelease(cs);
	if (!ctx) return;
	CGContextDrawImage(ctx, CGRectMake(0, 0, CGFloat(w), CGFloat(h)), img);
	CGContextRelease(ctx);
	MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:YES];
	d.usage = MTLTextureUsageShaderRead;
	_menuLogo = [_device newTextureWithDescriptor:d];
	[_menuLogo replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px.data() bytesPerRow:w * 4];
	id<MTLCommandBuffer> cb = [_queue commandBuffer];
	id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
	[b generateMipmapsForTexture:_menuLogo];
	[b endEncoding];
	[cb commit];
	NSError* err = nil;
	id<MTLLibrary> lib = [_device newLibraryWithSource:@(kLogoShader) options:nil error:&err];
	MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"logo_vertex"];
	pd.fragmentFunction = [lib newFunctionWithName:@"image_fragment"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pd.colorAttachments[0].blendingEnabled = YES;
	pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;  // premultiplied
	pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
	_imagePipe = lib ? [_device newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!_imagePipe) {
		std::fprintf(stderr, "menu logo shader: %s\n", err.localizedDescription.UTF8String);
		_menuLogo = nil;
	}
}

// ---------------------------------------------------------------- state

- (void)showScreen:(NSString*)name {
	_popup = Popup{};
	_dropOpen = NO;
	_hover = _pressed = kNone;
	if ([name isEqualToString:@"quit-popup"]) {
		[self showScreen:@"main"];
		[self openQuitPopup];
		return;
	}
	Screen s = kMain;
	for (int i = 0; i < kScreenCount; i++) {
		if ([name isEqualToString:@(kScreenNames[i])]) s = Screen(i);
	}
	_screen = s;
	if (s != kOptions && _opts) _opts->close();
	if (s != kMpConnect) _lan.close();
	if (s != kRecorded) _recScan.stop();
	_sel = 0, _top0 = 0, _focus = 0;
	_entries.clear();
	if (s == kRecorded) {
		[self openRecorded];
		return;
	}
	if (s == kCtw || s == kCtwMap) {
		[self ctwOpen:s == kCtwMap];
		return;
	}
	if (s == kOptions) {
		[self openOptions];
		return;
	}
	if (s == kSkirmishSetup) [self openSetup];
	if (s == kMpConnect || s == kMpLobby) {
		[self mpOpen];
		return;
	}
	if (s != kMain && s != kSkirmishSetup) {
		_entries = launcher::entries_for(s == kTutorials ? launcher::Mode::Tutorials : s == kSkirmish ? launcher::Mode::Skirmish
		                                                                                              : launcher::Mode::Scenarios,
		                                 _dir);
		NSString* key = [NSString stringWithFormat:@"MenuSelection.%s", kScreenNames[s]];
		const NSInteger saved = [NSUserDefaults.standardUserDefaults integerForKey:key];
		if (saved > 0 && size_t(saved) < _entries.size()) _sel = int(saved);
		[self scrollToSelection];
		_dropOpen = getenv("RON_MENU_DROP") && s != kTutorials;  // snapshot check of the open drop-down
	}
	if (_interactive && s < kMpConnect) [NSUserDefaults.standardUserDefaults setObject:@(kScreenNames[s]) forKey:launcher::kDefScreen];
}

- (void)openQuitPopup {
	_popup = Popup{};
	_popup.open = true, _popup.kind = 0;
	_popup.text = "Are you sure you want to quit Rise of Nations?";
	_popup.buttons = {"Yes", "No"};
}

- (void)setBusy:(NSString*)text {
	if (!text) {
		if (_popup.kind == 2) _popup = Popup{};
		return;
	}
	_popup = Popup{};
	_popup.open = true, _popup.kind = 2, _popup.text = text.UTF8String;
}

- (void)showError:(NSString*)text {
	_popup = Popup{};
	_popup.open = true, _popup.kind = 1, _popup.text = text.UTF8String, _popup.buttons = {"OK"};
}

- (std::string)soundFile:(const std::string&)key {
	for (const auto& [k, tags] : _soundGlobals) {
		if (k != key || tags.empty()) continue;
		for (const auto& [t, f] : _soundFiles) {
			if (t == tags.front()) return _files->resolve(f);
		}
	}
	return {};
}

- (void)click {
	if (_audio) {
		const std::string f = [self soundFile:"TOP_MENU_BUTTON"];
		if (!f.empty()) _audio->play_effect(f);
	}
}

- (void)startInteractive {
	_interactive = YES;
	_animT = 0;
	_lastFrame = 0;
	if (_audio) return;
	_audio = std::make_unique<ron::audio::Audio>();
	if (const prefs::Prefs& pp = prefs::get(); pp.has("fx_volume") || pp.has("music_volume")) {  // the Options page's sound pane
		const float fx = float(pp.fx_volume) / 255.0f;
		_audio->set_volumes(fx, fx, float(pp.music_volume) / 255.0f);
	}
	// The front end's music: Data/sound.xml TRACKS/INTROS, shuffled by the JukeBox like any mood.
	try {
		std::vector<std::string> intros;
		const ron::XmlElement doc = ron::parse_xml(_files->read_text("Data/sound.xml"));
		ron::for_each_element(doc, "INTROS", [&](const ron::XmlElement& e) {
			if (!intros.empty()) return;
			for (const ron::XmlElement& s : e.children) {
				const std::string f = _files->resolve(ron::normalize_game_path(s.get("file")));
				if (!f.empty()) intros.push_back(f);
			}
		});
		if (!intros.empty() && !getenv("RON_NO_MUSIC") && prefs::get().music_in_menu) {
			_audio->set_music({{"economic", intros}}, "", "");
			_audio->update(_timeMs);
			_audio->music_start();
		}
	} catch (const std::exception& e) {
		std::fprintf(stderr, "menu music unavailable: %s\n", e.what());
	}
}

- (void)stopAudio {
	_audio.reset();
}

- (int)visibleRows {
	return std::max(1, [self layout].list.h() / kRowH);
}

- (void)scrollToSelection {
	const int rows = [self visibleRows];
	if (_sel < _top0) _top0 = _sel;
	if (_sel >= _top0 + rows) _top0 = _sel - rows + 1;
	_top0 = std::clamp(_top0, 0, std::max(0, int(_entries.size()) - rows));
}

// ---------------------------------------------------------------- layout (canvas pixels)

- (Layout)layout {
	Layout L;
	if (_screen == kMain) {
		// TopMenuSelection's window at the design's spot: frame (44, 112), the ribbon over its top edge.
		const int wx = 44, wy = 38;
		L.title = _titleRect.offset(wx, wy);
		L.title_text = _titleText.offset(wx, wy);
		int bottom = 0;
		for (int i = 0; i < kMainCount; i++) {
			const IfaceRect r = _btnRect.offset(wx, wy + i * _btnInc);
			L.buttons.push_back(r);
			bottom = r.b;
		}
		L.frame = {wx + _btnRect.l - 41, wx + _btnRect.r + 41, L.title.t + _titleRect.h() - _titleRect.t + _titleRect.t, bottom + 41};
		L.frame.t = wy + _titleRect.h();  // client top + title height (TopMenuSelection::on_redraw)
		L.info = {44, 580, 476, 556};
		L.folder_y = 640;
		L.label_x = 44;
		L.change = {980 - 110, 980, 638, 666};
		L.edit = {0, L.change.l - 14, 640, 664};  // left edge set once the label is measured
	} else {
		const int tw = _titleRect.w(), th = _titleRect.h();
		L.title = {(kW - tw) / 2, (kW - tw) / 2 + tw, 22, 22 + th};
		L.title_text = {L.title.l, L.title.r, 22, 22 + _titleText.h()};
		L.note_y = 104;
		L.list_frame = {95, 929, 140, 630};
		L.list = {137, 863, 182, 588};
		L.scroll = {863, 887, 182, 588};
		L.up = {867, 883, 184, 200};
		L.down = {867, 883, 570, 586};
		L.track = {867, 883, 202, 568};
		const int n = int(_entries.size()), rows = std::max(1, L.list.h() / kRowH);
		L.rows = rows;
		if (n > rows) {
			const int th2 = std::max(24, L.track.h() * rows / n);
			const int maxTop = n - rows;
			const int y = L.track.t + (L.track.h() - th2) * _top0 / std::max(1, maxTop);
			L.thumb = {L.track.l, L.track.r, y, y + th2};
		}
		L.drop = {0, 0, 652, 673};  // placed after the label
		L.play = {912 - 173, 912, 646, 678};
		L.back = {L.play.l - 12 - 173, L.play.l - 12, 646, 678};
		L.rand = {396, 546, 646, 678};
	}
	return L;
}

// ---------------------------------------------------------------- drawing helpers

- (void)menuButton:(const IfaceRect&)r label:(const char*)label hot:(bool)hot enabled:(bool)enabled {
	const IfaceRect R = r.offset(_ox, _oy);
	IfacePiece p[3] = {_btn[0], _btn[1], _btn[2]};
	for (IfacePiece& q : p) {
		if (!enabled) q.top += q.height + 1;
		else if (hot) q.top -= q.height + 1;
	}
	if (p[0].valid() && p[1].valid() && p[2].valid()) {
		_ui.add(p[0], float(R.l), float(R.t));
		_ui.add(p[2], float(R.r - p[2].width), float(R.t));
		_ui.tile(p[1], {R.l + p[0].width, R.r - p[2].width, R.t, R.t + p[1].height});
	}
	_ui.next_layer();
	const int f = _ui.font(18, -1);
	const IfaceRect tr = enabled ? R.offset(2, 2) : R;
	_ui.text_centered(f, label, tr, enabled ? kWhite : rgb(150, 150, 150), kButtonShadow);
}

- (void)popupButton:(const IfaceRect&)r label:(const std::string&)label state:(int)state font:(int)f {
	const IfaceRect R = r.offset(_ox, _oy);
	const IfacePiece* p = _popBtn[std::clamp(state, 0, 2)];
	if (p[0].valid() && p[1].valid() && p[2].valid()) _ui.expander(p[0], p[1], p[2], R);
	else _ui.fill(float(R.l), float(R.t), float(R.w()), float(R.h()), argb(255, 70, 70, 70));
	_ui.next_layer();
	_ui.text_centered(f, label, R, kWhite, kBlack);
}

- (void)thinBox:(const IfaceRect&)r fill:(uint32_t)fill {
	const IfaceRect R = r.offset(_ox, _oy);
	_ui.fill(float(R.l), float(R.t), float(R.w()), float(R.h()), fill, UiType::ColorBlend);
	_ui.next_layer();
	_ui.border(_thin, R);
}

- (void)banner:(const Layout&)L text:(const char*)text {
	_ui.next_window();
	if (_ribbon[0].valid()) _ui.expander(_ribbon[0], _ribbon[1], _ribbon[2], L.title.offset(_ox, _oy));
	_ui.next_layer();
	_ui.text_centered(_ui.font(22, -1), text, L.title_text.offset(_ox, _oy), kWhite, kBlack);
}

- (int)textAt:(int)f text:(const std::string&)s x:(float)x y:(float)y color:(uint32_t)c {
	_ui.text(f, s, x + float(_ox), y + float(_oy), c, kBlack);
	return _ui.text_width(f, s);
}

// ---------------------------------------------------------------- the backdrop

// The rect a 16:9 picture covers the window with (the original stretches it over the screen; the port keeps the
// picture's aspect and crops).
- (IfaceRect)coverFor:(int)w height:(int)h {
	const float a = float(w) / float(h);
	int cw = _uiW, ch = _uiH;
	if (float(_uiW) / float(_uiH) > a) ch = int(std::ceil(float(_uiW) / a));
	else cw = int(std::ceil(float(_uiH) * a));
	const int x = (_uiW - cw) / 2, y = (_uiH - ch) / 2;
	return {x, x + cw, y, y + ch};
}

- (void)drawBackdrop {
	_ui.next_window();
	if ((_screen != kMain && _screen != kOptions) || _anims.empty()) {
		// The setup screens' antique map (TEX_TOPMENU_0, iface_wrapper1.tga), TopMenu's static_bg_tex.
		if (_staticBg.valid()) {
			const IfaceRect c = [self coverFor:_staticBg.width height:_staticBg.height];
			_ui.add(_staticBg, float(c.l), float(c.t), float(c.w()), float(c.h()), kWhite, UiType::Normal);
		}
		return;
	}
	const Anim& a = _anims[size_t(_anim) % _anims.size()];
	const float t = float(_animT);
	float alpha = 255;
	const float left = a.duration - t;
	if (left > 0 && left <= 1) alpha = left * 255;
	else if (t <= 0.5f) alpha = 0;
	else if (t <= 1.5f) alpha = std::clamp((t - 0.5f) * 255.0f, 0.0f, 255.0f);
	const uint32_t col = argb(int(alpha), 255, 255, 255);
	const IfaceRect c = a.layers.empty() ? IfaceRect{} : [self coverFor:a.layers.front().piece.width height:a.layers.front().piece.height];
	for (const AnimLayer& l : a.layers) {
		const float dx = float(c.w()) * (l.move_x * t + l.off_x), dy = float(c.h()) * (l.move_y * t + l.off_y);
		_ui.add(l.piece, float(c.l) + dx, float(c.t) + dy, float(c.w()), float(c.h()), col, UiType::ColorAlphaMod);
	}
}

- (void)advanceAnimation:(double)dt {
	if (_anims.empty() || (_screen != kMain && _screen != kOptions)) return;
	_animT += dt;
	const Anim& a = _anims[size_t(_anim) % _anims.size()];
	if (_animT > a.duration) {
		for (const AnimLayer& l : a.layers) _ui.release(l.piece.texture);  // ~3.7 MB each; the next set loads on first use
		_animT = 0;
		_anim = (_anim + 1) % int(_anims.size());
	}
}

// The logo rect (canvas): the design's spot at the right of the menu.
- (IfaceRect)logoRect {
	return {505, 505 + 512, 35, 35 + 512};
}

- (void)encodeLogo:(id<MTLRenderCommandEncoder>)enc {
	if (_menuLogo) {
		// The design's spot (470 x 470 at right 28, top 56, drop-shadow(0 6px 14px rgba(0,0,0,.6))), 440 px on the same
		// vertical centre so that it clears the original's wider (536 px) bronze frame.
		const int size = 440, top = 56 + (470 - size) / 2;
		const IfaceRect r = IfaceRect{kW - 28 - size, kW - 28, top, top + size}.offset(_ox, _oy);
		const float blurLevel = std::log2(std::max(1.0f, 14.0f * float(_menuLogo.width) / float(size)));
		for (int pass = 0; pass < 2; pass++) {
			const float dy = pass == 0 ? 6 : 0;
			struct {
				float rect[4], view[4], light[4];
			} q = {{float(r.l), float(r.t) + dy, float(r.w()), float(r.h())}, {float(_uiW), float(_uiH), 0, 0}, {float(pass), blurLevel, 0, 0}};
			[enc setRenderPipelineState:_imagePipe];
			[enc setVertexBytes:&q length:sizeof q atIndex:0];
			[enc setFragmentBytes:&q length:sizeof q atIndex:0];
			[enc setFragmentTexture:_menuLogo atIndex:0];
			[enc setFragmentSamplerState:_logoSampler atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		}
		return;
	}
	const IfaceRect r = [self logoRect].offset(_ox, _oy);
	if (!_logoPipe) return;
	// TopMenu::draw_logo: the light leans toward the mouse: dir = normalize(k * unit(centre - mouse), -1); the shader's
	// L (toward the light) is its negation, in UI axes (x right, y down, z toward the viewer).
	const float cx = float(r.l + r.w() / 2), cy = float(r.t + r.h() / 2);
	float mx = float(_mouse.x + _ox) - cx, my = float(_mouse.y + _oy) - cy;
	const float len = std::sqrt(mx * mx + my * my);
	if (len > 0.001f) mx /= len, my /= len;
	else mx = 0, my = -1;
	const float k = std::sqrt(2.0f) * 512.0f / float(kW);
	struct {
		float rect[4], view[4], light[4];
	} q = {{float(r.l), float(r.t), float(r.w()), float(r.h())}, {float(_uiW), float(_uiH), 0, 0}, {mx * k, my * k, 1, -1}};
	[enc setRenderPipelineState:_logoPipe];
	[enc setVertexBytes:&q length:sizeof q atIndex:0];
	[enc setFragmentBytes:&q length:sizeof q atIndex:0];
	[enc setFragmentTexture:_logoTex atIndex:0];
	[enc setFragmentTexture:_logoNormal atIndex:1];
	[enc setFragmentSamplerState:_logoSampler atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

// ---------------------------------------------------------------- screens

- (void)drawMain:(const Layout&)L {
	// The bronze frame around the buttons, then the ribbon over its top edge (TopMenuSelection::on_redraw).
	_ui.next_window();
	_ui.border(_border, L.frame.offset(_ox, _oy));
	[self banner:L text:"Main Menu"];
	_ui.next_layer();
	for (int i = 0; i < kMainCount; i++) {
		const bool hot = _hover == kMainBtn + i || _pressed == kMainBtn + i || (_hover == kNone && _focus == i && !_popup.open);
		[self menuButton:L.buttons[size_t(i)] label:kMainItems[i].label hot:hot && kMainItems[i].enabled enabled:kMainItems[i].enabled];
		_ui.next_layer();
	}
	// Help box (TopMenu::on_window_redraw): the hovered button's help, font 16, in the MOTD style box.
	_ui.next_window();
	[self thinBox:L.info fill:argb(100, 0, 0, 0)];
	_ui.next_layer();
	const int hf = _ui.font(16, -1);
	const ron::FontMetrics* hm = _ui.font_metrics(hf);
	int y = L.info.t + 10;
	for (const std::string& line : wrap_text(_ui, hf, _help, L.info.w() - 20)) {
		[self textAt:hf text:line x:float(L.info.l + 10) y:float(y) color:kWhite];
		y += hm ? hm->line_height : 20;
	}
	// Game folder row.
	_ui.next_window();
	const int f14 = _ui.font(14, -1);
	const ron::FontMetrics* m14 = _ui.font_metrics(f14);
	const int fh = m14 ? m14->font_height : 16;
	const int lw = [self textAt:f14 text:"Game Folder:" x:float(L.label_x) y:float(L.edit.t + (L.edit.h() - fh) / 2) color:kLabel];
	IfaceRect edit = L.edit;
	edit.l = L.label_x + lw + 14;
	[self thinBox:edit fill:argb(170, 0, 0, 0)];
	_ui.next_layer();
	std::string path = _dir.UTF8String;
	const int room = edit.w() - 16;
	if (_ui.text_width(f14, path) > room) {
		while (path.size() > 4 && _ui.text_width(f14, "..." + path) > room) path.erase(0, 1);
		path = "..." + path;
	}
	[self textAt:f14 text:path x:float(edit.l + 8) y:float(edit.t + (edit.h() - fh) / 2) color:kWhite];
	[self popupButton:L.change label:"Change…" state:(_pressed == kChange ? 2 : _hover == kChange ? 1 : 0) font:f14];
	[self textAt:f14 text:"Your game files are ready." x:float(L.label_x) y:float(L.edit.b + 10) color:kSecondary];
}

- (void)drawList:(const Layout&)L {
	[self banner:L text:kListTitles[_screen]];
	_ui.next_window();
	const int f14 = _ui.font(14, -1), f12 = _ui.font(12, -1);
	const ron::FontMetrics* m14 = _ui.font_metrics(f14);
	const int lh = m14 ? m14->line_height : 16;
	int y = L.note_y;
	if (_screen == kRecorded) [self refreshRecorded];
	for (const std::string& line : wrap_text(_ui, f14, kListNotes[_screen], 800)) {
		[self textAt:f14 text:line x:float((kW - _ui.text_width(f14, line)) / 2) y:float(y) color:kSecondary];
		y += lh;
	}
	// The list in the bronze frame on a dark wash.
	_ui.next_window();
	const IfaceRect inner{L.list_frame.l + 30, L.list_frame.r - 30, L.list_frame.t + 30, L.list_frame.b - 30};
	_ui.fill(float(inner.l + _ox), float(inner.t + _oy), float(inner.w()), float(inner.h()), argb(150, 13, 11, 40), UiType::ColorBlend);
	_ui.next_layer();
	_ui.border(_border, L.list_frame.offset(_ox, _oy));
	_ui.next_layer();
	const int n = int(_entries.size());
	if (n == 0 && _screen == kRecorded) {
		int ny = L.list.t + 20;
		const std::string none = _recScan.done() ? "No recorded games yet. Multiplayer games are recorded in " + _recDir + ", and Open File... plays any .rcx file."
		                                         : "Looking for recorded games...";
		for (const std::string& line : wrap_text(_ui, f14, none, L.list.w() - 20)) {
			[self textAt:f14 text:line x:float(L.list.l + 10) y:float(ny) color:kSecondary];
			ny += lh;
		}
	} else if (n == 0) {
		const char* none = "None of these maps were found in your game folder.";
		[self textAt:f14 text:none x:float(L.list.l + (L.list.w() - _ui.text_width(f14, none)) / 2) y:float(L.list.t + 20) color:kSecondary];
	}
	const int rowW = n > L.rows ? L.list.w() - 4 : L.scroll.r - L.list.l;
	for (int r = 0; r < L.rows && _top0 + r < n; r++) {
		const int i = _top0 + r;
		const IfaceRect row{L.list.l, L.list.l + rowW, L.list.t + r * kRowH, L.list.t + (r + 1) * kRowH};
		const IfaceRect R = row.offset(_ox, _oy);
		if (i == _sel || _hover == kRow + i) {
			IfacePiece p[3] = {_large[0], _large[1], _large[2]};
			if (i != _sel)
				for (IfacePiece& q : p) q.top += q.height + 1;  // the darker row below for the hovered entry
			if (p[0].valid()) _ui.expander(p[0], p[1], p[2], {R.l, R.r, R.t + 1, R.b - 1});
		}
		_ui.next_layer();
		const launcher::Entry& e = _entries[size_t(i)];
		_ui.text(f14, e.title, float(R.l + 14), float(R.t + 5), kWhite, kBlack);
		std::string detail = e.detail;
		if (_screen == kRecorded) {  // one line of columns: it must fit the row
			const int room = R.w() - 28;
			if (_ui.text_width(f12, detail) > room) {
				while (detail.size() > 8 && _ui.text_width(f12, detail + "...") > room) detail.pop_back();
				detail += "...";
			}
		}
		_ui.text(f12, detail, float(R.l + 14), float(R.t + 23), i == _sel ? rgb(225, 225, 235) : kSecondary, kBlack);
		_ui.next_layer();
	}
	if (n > L.rows) {  // SCROLLCLASS: vertical back, slider, arrow buttons
		const IfaceRect s = L.scroll.offset(_ox, _oy), u = L.up.offset(_ox, _oy), d = L.down.offset(_ox, _oy);
		if (_scrollBack[0].valid()) _ui.vexpander(_scrollBack[0], _scrollBack[1], _scrollBack[2], s);
		_ui.next_layer();
		if (_scrollSlider[0].valid()) _ui.vexpander(_scrollSlider[0], _scrollSlider[1], _scrollSlider[2], L.thumb.offset(_ox + 0, _oy));
		_ui.add(_scrollUp, float(u.l), float(u.t));
		_ui.add(_scrollDown, float(d.l), float(d.t));
		_ui.next_layer();
	}
	// Bottom row: AI difficulty, Back, Play.
	_ui.next_window();
	if (_screen != kTutorials && _screen != kRecorded) {
		const ron::FontMetrics* fm = _ui.font_metrics(f14);
		const int fh = fm ? fm->font_height : 16;
		const int lw = [self textAt:f14 text:"AI Difficulty:" x:112 y:float(L.drop.t + (L.drop.h() - fh) / 2) color:kLabel];
		IfaceRect drop = L.drop;
		drop.l = 112 + lw + 12, drop.r = drop.l + 170;
		const IfaceRect D = drop.offset(_ox, _oy);
		// The GameSpy dialog's combo box: white field (left / mid / right) and the arrow button at the right.
		if (_combo[0].valid()) {
			const IfaceRect field{D.l, D.r - _combo[3].width + 2, D.t, D.b};
			_ui.expander(_combo[0], _combo[1], _combo[2], field);
			_ui.add(_combo[3], float(D.r - _combo[3].width), float(D.t));
		} else {
			[self thinBox:drop fill:rgb(240, 240, 240)];
		}
		_ui.next_layer();
		const int fo = _ui.font(12, 1);
		const ron::FontMetrics* om = _ui.font_metrics(fo);
		const int oh = om ? om->font_height : 14;
		_ui.text(fo, kDifficulty[_difficulty], float(D.l + 7), float(D.t + (D.h() - oh) / 2), kBlack);
		if (_dropOpen) {  // the option list opens upward (the box sits at the bottom of the screen)
			_ui.next_window();
			const int ih = 20, lh2 = 6 * ih + 6;
			const IfaceRect list{drop.l, drop.r - _combo[3].width, drop.t - lh2 - 2, drop.t - 2};
			[self thinBox:list fill:rgb(244, 244, 240)];
			_ui.next_layer();
			for (int k = 0; k < 6; k++) {
				const IfaceRect it{list.l + 3, list.r - 3, list.t + 3 + k * ih, list.t + 3 + (k + 1) * ih};
				const IfaceRect I = it.offset(_ox, _oy);
				const bool hot = _hover == kDropItem + k || (_hover < kDropItem && k == _difficulty);
				if (hot) _ui.fill(float(I.l), float(I.t), float(I.w()), float(I.h()), rgb(53, 47, 132), UiType::ColorBlend);
				_ui.next_layer();
				_ui.text(fo, kDifficulty[k], float(I.l + 5), float(I.t + (ih - oh) / 2), hot ? kWhite : kBlack);
			}
		}
	}
	_ui.next_window();
	const int fb = _ui.font(14, -1);
	[self popupButton:L.back label:"Back" state:(_pressed == kBack ? 2 : _hover == kBack ? 1 : 0) font:fb];
	[self popupButton:L.play label:(_screen == kRecorded ? "Watch" : "Play") state:(_pressed == kPlay ? 2 : _hover == kPlay ? 1 : 0) font:fb];
	if (_screen == kRecorded)
		[self popupButton:L.rand label:"Open File…" state:(_pressed == kRecOpen ? 2 : _hover == kRecOpen ? 1 : 0) font:fb];
	if (_screen == kSkirmish)
		[self popupButton:L.rand label:"Random Map…" state:(_pressed == kRandomMap ? 2 : _hover == kRandomMap ? 1 : 0) font:fb];
}

- (void)drawPopup {
	if (!_popup.open) return;
	_ui.next_window();
	_ui.fill(0, 0, float(_uiW), float(_uiH), argb(115, 0, 0, 0), UiType::ColorBlend);
	_ui.next_layer();
	const int f = _ui.font(14, -1);
	const ron::FontMetrics* m = _ui.font_metrics(f);
	const int lh = m ? m->line_height : 16;
	const std::vector<std::string> lines = wrap_text(_ui, f, _popup.text, 360);
	int tw = 0;
	for (const auto& l : lines) tw = std::max(tw, _ui.text_width(f, l));
	const int bw = 173, bh = 32, gap = 12, nb = int(_popup.buttons.size());
	const int w = std::max({tw, nb * bw + std::max(0, nb - 1) * gap, 240}) + 48;
	const int h = int(lines.size()) * lh + (nb ? bh + 24 : 0) + 48;
	const int x = (kW - w) / 2, y = (kH - h) / 2;
	[self thinBox:{x, x + w, y, y + h} fill:_popBg];
	_ui.next_layer();
	int ty = y + 24;
	for (const auto& l : lines) {
		[self textAt:f text:l x:float(x + (w - _ui.text_width(f, l)) / 2) y:float(ty) color:kWhite];
		ty += lh;
	}
	_popup.rects.clear();
	int bx = x + (w - (nb * bw + std::max(0, nb - 1) * gap)) / 2;
	for (int i = 0; i < nb; i++) {
		const IfaceRect r{bx, bx + bw, y + h - 24 - bh, y + h - 24};
		_popup.rects.push_back(r);
		[self popupButton:r label:_popup.buttons[size_t(i)] state:(_pressed == kPopupBtn + i ? 2 : _hover == kPopupBtn + i ? 1 : 0) font:f];
		bx += bw + gap;
	}
}

// ---------------------------------------------------------------- frame

- (void)updateMapping:(CGSize)points {
	const float pw = float(std::max(1.0, points.width)), ph = float(std::max(1.0, points.height));
	_fit = std::min(pw / kW, ph / kH);
	_uiW = int(std::ceil(pw / _fit)), _uiH = int(std::ceil(ph / _fit));
	_ox = (_uiW - kW) / 2, _oy = (_uiH - kH) / 2;
}

- (void)renderInto:(id<MTLTexture>)output commandBuffer:(id<MTLCommandBuffer>)cb points:(CGSize)points {
	[self updateMapping:points];
	const float scale = float(output.width) / float(_uiW);
	MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = output;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
	_ui.set_smooth(std::fabs(scale - std::round(scale)) > 0.01f);
	// 1. backdrop, 2. the logo (main menu), 3. the windows.
	_ui.begin(_uiW, _uiH, scale, kW);
	[self drawBackdrop];
	if (_screen == kMain && !_menuLogo && !_logoPipe && _flatLogo.valid()) {
		const IfaceRect r = [self logoRect].offset(_ox, _oy);
		_ui.next_window();
		_ui.add(_flatLogo, float(r.l), float(r.t + 512 / 2 - _flatLogo.height / 2));
	}
	_ui.encode(enc);
	if (_screen == kMain) [self encodeLogo:enc];
	_ui.begin(_uiW, _uiH, scale, kW);
	const Layout L = [self layout];
	if (_screen == kMpLobby) [self mpPump];
	if (_screen == kMain) [self drawMain:L];
	else if (_screen == kSkirmishSetup) [self drawSetup:L];
	else if (_screen == kMpConnect) [self drawConnect:L];
	else if (_screen == kMpLobby) [self drawLobby:L];
	else if (_screen == kCtw || _screen == kCtwMap) [self ctwDraw];
	else if (_screen == kOptions) [self drawOptions];
	else [self drawList:L];
	[self drawPopup];
	_ui.encode(enc);
	[enc endEncoding];
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}

- (void)drawInMTKView:(MTKView*)view {
	const double now = CACurrentMediaTime();
	const double dt = _lastFrame > 0 ? std::min(0.1, now - _lastFrame) : 0;
	_lastFrame = now;
	_timeMs += dt * 1000.0;
	[self advanceAnimation:dt];
	if (_audio) _audio->update(_timeMs);
	id<CAMetalDrawable> drawable = view.currentDrawable;
	if (!drawable) return;
	id<MTLCommandBuffer> cb = [_queue commandBuffer];
	[self renderInto:drawable.texture commandBuffer:cb points:view.bounds.size];
	[cb presentDrawable:drawable];
	[cb commit];
}

- (BOOL)snapshotTo:(NSString*)path width:(int)w height:(int)h scale:(float)scale {
	if (scale <= 0) scale = 1;
	if (_screen == kRecorded) {  // headless: the headers are read on a thread; the picture waits for the list
		for (int i = 0; i < 3000 && !_recScan.done(); i++) [NSThread sleepForTimeInterval:0.01];
		_recSeen = ~0u;
	}
	if (_screen == kOptions && _opts && _opts->is_open() && getenv("RON_OPT_SCRIPT")) [self runOptionsScript:@(getenv("RON_OPT_SCRIPT")) path:path width:w height:h scale:scale];
	id<MTLTexture> out = rm::make_readback_target(_device, w, h);
	id<MTLCommandBuffer> cb = [_queue commandBuffer];
	[self renderInto:out commandBuffer:cb points:CGSizeMake(w / scale, h / scale)];
	[cb commit];
	[cb waitUntilCompleted];
	if (!rm::write_png(out, path.UTF8String)) {
		std::fprintf(stderr, "could not write %s\n", path.UTF8String);
		return NO;
	}
	std::printf("wrote %s (%dx%d, menu %s)\n", path.UTF8String, w, h, kScreenNames[_screen]);
	return YES;
}

// ---------------------------------------------------------------- input

- (CGPoint)canvas:(CGPoint)p {
	return CGPointMake(p.x / _fit - _ox, p.y / _fit - _oy);
}

- (int)hit:(CGPoint)c {
	const int x = int(std::floor(c.x)), y = int(std::floor(c.y));
	const Layout L = [self layout];
	if (_popup.open) {
		for (size_t i = 0; i < _popup.rects.size(); i++) {
			if (_popup.rects[i].contains(x, y)) return kPopupBtn + int(i);
		}
		return kNone;
	}
	if (_screen == kMain) {
		for (int i = 0; i < kMainCount; i++) {
			if (L.buttons[size_t(i)].contains(x, y) && kMainItems[i].enabled) return kMainBtn + i;
		}
		if (L.change.contains(x, y)) return kChange;
		return kNone;
	}
	if (_screen == kOptions) return kNone;  // the page does its own hit tests
	if (_screen == kSkirmishSetup) return [self hitSetup:x y:y];
	if (_screen == kMpLobby) return [self hitLobby:x y:y];
	if (_screen == kMpConnect) return [self hitConnect:x y:y];
	if (L.back.contains(x, y)) return kBack;
	if (L.play.contains(x, y)) return kPlay;
	if (_screen == kSkirmish && L.rand.contains(x, y)) return kRandomMap;
	if (_screen == kRecorded && L.rand.contains(x, y)) return kRecOpen;
	if (_screen != kTutorials && _screen != kRecorded) {
		const IfaceRect drop = [self dropRect];
		if (_dropOpen) {
			const int ih = 20, lh2 = 6 * ih + 6;
			const IfaceRect list{drop.l, drop.r - _combo[3].width, drop.t - lh2 - 2, drop.t - 2};
			for (int k = 0; k < 6; k++) {
				if (IfaceRect{list.l + 3, list.r - 3, list.t + 3 + k * ih, list.t + 3 + (k + 1) * ih}.contains(x, y)) return kDropItem + k;
			}
		}
		if (drop.contains(x, y)) return kDrop;
	}
	const int n = int(_entries.size());
	if (n > L.rows) {
		if (L.up.contains(x, y)) return kScrollUp;
		if (L.down.contains(x, y)) return kScrollDown;
		if (L.thumb.contains(x, y)) return kThumb;
		if (L.scroll.contains(x, y)) return kTrack;
	}
	if (L.list.contains(x, y)) {
		const int i = _top0 + (y - L.list.t) / kRowH;
		if (i >= 0 && i < n) return kRow + i;
	}
	return kNone;
}

- (IfaceRect)dropRect {
	const int f14 = _ui.font(14, -1);
	const int lw = _ui.text_width(f14, "AI Difficulty:");
	const Layout L = [self layout];
	IfaceRect d = L.drop;
	d.l = 112 + lw + 12, d.r = d.l + 170;
	return d;
}

- (void)setHover:(int)h {
	_hover = h;
	if (_screen == kMain && h >= kMainBtn && h < kMainBtn + kMainCount) {
		_help = kMainItems[h - kMainBtn].help;
		_focus = h - kMainBtn;
	}
}

- (void)mouseMoved:(CGPoint)p {
	if (_ctw && (_screen == kCtw || _screen == kCtwMap)) {
		[self ctwBridge];
		_ctw->mouse_move(_ctwb, p.x / _fit, p.y / _fit);
		return;
	}
	_mouse = [self canvas:p];
	_mouseKnown = YES;
	if (_screen == kOptions && _opts) {
		_opts->mouse_move(int(p.x / _fit), int(p.y / _fit));
		return;
	}
	int h = [self hit:_mouse];
	// The help box also explains the disabled Multiplayer button.
	if (_screen == kMain && !_popup.open && h == kNone) {
		const Layout L = [self layout];
		if (L.buttons[4].contains(int(_mouse.x), int(_mouse.y))) _help = kMainItems[4].help;
	}
	[self setHover:h];
}

- (void)mouseDown:(CGPoint)p clicks:(NSInteger)clicks {
	if (_ctw && (_screen == kCtw || _screen == kCtwMap)) {
		[self ctwBridge];
		_ctw->mouse_down(_ctwb, p.x / _fit, p.y / _fit, int(clicks), false);
		return;
	}
	_mouse = [self canvas:p];
	if (_screen == kOptions && _opts && !_popup.open) {
		_opts->mouse_down(int(p.x / _fit), int(p.y / _fit));
		return;
	}
	const int h = [self hit:_mouse];
	if ((_screen == kSkirmishSetup || _screen == kMpLobby) && !_popup.open) {
		// An open list closes on a press outside it; the press itself then starts nothing.
		if (_openCombo >= 0 && !(h >= kSetupItem && h < kSetupItem + 100) && h != kSetupCombo + _openCombo) {
			_openCombo = -1;
			return;
		}
		if (_seedFocus && h != kSeedField) _seedFocus = NO;
		if (_screen == kMpLobby && h != kMpChat) _mpField = -1;
		_pressed = h;
		return;
	}
	if (_screen == kMpConnect && !_popup.open) {  // its buttons, fields and list rows are 2100+: not the Tutorials list's rows (kRow..), which would swallow the press
		_pressed = h;
		return;
	}
	if (_dropOpen && !(h >= kDropItem && h < kDropItem + 6) && h != kDrop) {
		_dropOpen = NO;
		return;
	}
	_pressed = h;
	if (h >= kRow) {
		_sel = h - kRow;
		[self remember];
		const double now = CACurrentMediaTime() * 1000.0;
		if (clicks >= 2 && _lastClickRow == _sel) [self play];
		_lastClickRow = _sel, _lastClickMs = now;
		_pressed = kNone;
	} else if (h == kThumb) {
		_dragThumb = YES;
		_dragOffset = int(_mouse.y) - [self layout].thumb.t;
	} else if (h == kScrollUp || h == kScrollDown) {
		[self scrollBy:h == kScrollUp ? -1 : 1];
	} else if (h == kTrack) {
		const Layout L = [self layout];
		[self scrollBy:int(_mouse.y) < L.thumb.t ? -L.rows : L.rows];
	}
}

- (void)mouseDragged:(CGPoint)p {
	if (_ctw && (_screen == kCtw || _screen == kCtwMap)) {
		[self ctwBridge];
		_ctw->mouse_drag(_ctwb, p.x / _fit, p.y / _fit);
		return;
	}
	_mouse = [self canvas:p];
	if (_screen == kOptions && _opts) {
		_opts->mouse_drag(int(p.x / _fit), int(p.y / _fit));
		return;
	}
	if (_dragThumb) {
		const Layout L = [self layout];
		const int n = int(_entries.size()), maxTop = std::max(0, n - L.rows);
		const int span = std::max(1, L.track.h() - L.thumb.h());
		_top0 = std::clamp(int(std::lround(double(int(_mouse.y) - _dragOffset - L.track.t) * maxTop / span)), 0, maxTop);
		return;
	}
	_hover = [self hit:_mouse] == _pressed ? _pressed : kNone;
}

- (void)mouseUp:(CGPoint)p {
	if (_ctw && (_screen == kCtw || _screen == kCtwMap)) {
		[self ctwBridge];
		_ctw->mouse_up(_ctwb, p.x / _fit, p.y / _fit);
		[self ctwTakeAction];
		return;
	}
	_mouse = [self canvas:p];
	_dragThumb = NO;
	if (_screen == kOptions && _opts) {
		_opts->mouse_up(int(p.x / _fit), int(p.y / _fit));
		return;
	}
	const int h = [self hit:_mouse];
	const int pressed = _pressed;
	_pressed = kNone;
	if (h != pressed || h == kNone) return;
	[self activate:h];
	[self setHover:[self hit:_mouse]];
}

- (void)scrollWheel:(CGFloat)dy at:(CGPoint)p {
	if (_ctw && (_screen == kCtw || _screen == kCtwMap)) {
		[self ctwBridge];
		_ctw->scroll(_ctwb, float(dy), p.x / _fit, p.y / _fit);
		return;
	}
	if (_screen == kOptions && _opts) {
		_opts->wheel(double(dy), int(p.x / _fit), int(p.y / _fit));
		return;
	}
	if (_screen == kMain || _popup.open) return;
	if (_screen == kMpConnect) return;
	if (_screen == kSkirmishSetup || _screen == kMpLobby) {
		if (_openCombo >= 0 && size_t(_openCombo) < _sc.size()) {
			static CGFloat sacc = 0;
			sacc += dy;
			const int n = int(_sc[size_t(_openCombo)].items.size());
			while (sacc >= 1) _openTop = std::max(0, _openTop - 1), sacc -= 1;
			while (sacc <= -1) _openTop = std::min(std::max(0, n - 12), _openTop + 1), sacc += 1;
		}
		return;
	}
	static CGFloat acc = 0;
	acc += dy;
	while (acc >= 1) [self scrollBy:-1], acc -= 1;
	while (acc <= -1) [self scrollBy:1], acc += 1;
}

- (void)scrollBy:(int)rows {
	const Layout L = [self layout];
	_top0 = std::clamp(_top0 + rows, 0, std::max(0, int(_entries.size()) - L.rows));
}

- (void)remember {
	NSString* key = [NSString stringWithFormat:@"MenuSelection.%s", kScreenNames[_screen]];
	[NSUserDefaults.standardUserDefaults setInteger:_sel forKey:key];
}

- (void)play {
	if (_screen == kRecorded) {
		[self playRecording];
		return;
	}
	if (_sel < 0 || size_t(_sel) >= _entries.size()) return;
	[self click];
	[self remember];
	const int diff = _screen == kTutorials ? -1 : _difficulty;
	[self.host frontEndPlay:@(_entries[size_t(_sel)].path.c_str()) difficulty:diff];
}

- (void)activate:(int)h {
	if (_popup.open) {
		if (h < kPopupBtn) return;
		const int b = h - kPopupBtn;
		const int kind = _popup.kind;
		_popup = Popup{};
		[self click];
		if (kind == 0 && b == 0) [self.host frontEndQuit];
		return;
	}
	if (h >= kMainBtn && h < kMainBtn + kMainCount) {
		[self click];
		switch (h - kMainBtn) {
			case 0: [self showScreen:@"tutorials"]; break;
			case 1: [self showScreen:@"skirmish"]; break;
			case 2: [self showScreen:@"scenarios"]; break;
			case 3: [self showScreen:@"ctw"]; break;
			case 4: [self showScreen:@"multiplayer"]; break;
			case 5: [self showScreen:@"recorded"]; break;
			case 6: [self showScreen:@"options"]; break;
			case 7: [self openQuitPopup]; break;
			default: break;
		}
		return;
	}
	if (h == kChange) {
		[self click];
		[self.host frontEndChangeFolder];
	} else if (h == kBack && _screen == kMpConnect) {
		[self click];
		[self showScreen:@"main"];
		_focus = 4;
	} else if (h == kBack && _screen == kMpLobby) {
		[self click];
		[self mpLeave:nil];
	} else if (h == kPlay && _screen == kMpLobby) {
		[self mpPlay];
	} else if (h == kMpHost) {
		[self mpHost];
	} else if (h == kMpJoin) {
		[self mpJoin];
	} else if (h >= kMpName && h <= kMpAddr) {
		_mpField = h - kMpName;
	} else if (h == kMpChat) {
		_mpField = 3;
	} else if (h == kMpLoad) {
		[self click];
		[self mpLoadFile];
	} else if (h == kMpScenario) {
		[self click];
		[self mpChooseScenario];
	} else if (h == kMpCtw) {
		[self click];
		[self mpToggleCtw];
	} else if (h == kMpObserver && _room) {
		[self click];
		_room->request(ron::net::SeatField::Observer, _room->lobby().observer_of(_room->slot()) ? 0 : 1);
	} else if (h == kMpApproval && _room) {
		[self click];
		_room->set_approval_ask(!_room->approval_ask());
		_mpLines.push_back({1, _room->approval_ask() ? "Guests' nation, team and colour requests now wait for your decision." : "Guests' nation, team and colour requests are granted automatically."});
	} else if ((h == kMpAllow || h == kMpDeny) && _room && !_room->lobby().pending().empty()) {
		[self click];
		const ron::net::SeatRequest rq = _room->lobby().pending().front();
		_room->lobby().answer(rq.id, h == kMpAllow);
		if (h == kMpDeny) _mpLines.push_back({1, "You refused " + _room->name_of(rq.peer) + "'s request."});
	} else if (h >= kMpKick && h < kMpKick + 8 && _room) {
		[self click];
		const int peer = h - kMpKick;
		const std::string nm = _room->name_of(peer);
		_room->kick(peer, false);
		(void)nm;
	} else if (h >= kMpList && h < kMpList + 6) {
		const int row = h - kMpList;
		const double now = CACurrentMediaTime() * 1000.0;
		if (row < int(_lan.games().size())) {
			const ron::net::LanGame& g = _lan.games()[size_t(row)];
			if (_lanSel == row && now - _lastClickMs < 450 && g.compatible()) {
				_mpAddr = g.address + ":" + std::to_string(g.port);
				[self mpJoin];
			} else {
				_lanSel = row;
				if (g.compatible()) _mpAddr = g.address + ":" + std::to_string(g.port);
				_mpMessage.clear();
			}
			_lastClickMs = now;
		}
	} else if (h == kBack && _screen == kSkirmishSetup) {
		[self click];
		[self showScreen:@"skirmish"];
	} else if (h == kBack) {
		[self click];
		const Screen from = _screen;
		[self showScreen:@"main"];
		_focus = from == kRecorded ? 5 : std::max(0, int(from));
	} else if (h == kPlay && _screen == kSkirmishSetup) {
		[self playSetup];
	} else if (h == kPlay) {
		[self play];
	} else if (h == kRecOpen) {
		[self click];
		[self openRecordingFile];
	} else if (h == kRandomMap) {
		[self click];
		[self showScreen:@"skirmish-setup"];
	} else if (h == kSeedField) {
		_seedFocus = YES;
		_openCombo = -1;
	} else if (h == kSeedRandom) {
		[self click];
		_seedRandom = YES;
		_seedText.clear();
		_seedFocus = NO;
	} else if (h >= kSetupItem && h < kSetupItem + 100) {
		[self chooseCombo:_openCombo item:_openTop + (h - kSetupItem)];
		_openCombo = -1;
	} else if (h >= kSetupCombo && h < kSetupCombo + 200) {
		const int i = h - kSetupCombo;
		if (_openCombo == i) {
			_openCombo = -1;
		} else if (size_t(i) < _sc.size()) {
			_openCombo = i;
			_seedFocus = NO;
			_openTop = std::clamp(_sc[size_t(i)].sel - 5, 0, std::max(0, int(_sc[size_t(i)].items.size()) - 12));
		}
	} else if (h == kDrop) {
		_dropOpen = !_dropOpen;
	} else if (h >= kDropItem && h < kDropItem + 6) {
		_difficulty = h - kDropItem;
		_dropOpen = NO;
		[NSUserDefaults.standardUserDefaults setInteger:_difficulty forKey:launcher::kDefDifficulty];
	}
}

- (void)keyDown:(NSEvent*)e {
	NSString* chars = e.charactersIgnoringModifiers;
	const unichar c = chars.length ? [chars characterAtIndex:0] : 0;
	const bool ret = c == '\r' || c == 3, esc = e.keyCode == 53;
	_hover = kNone;
	if (_ctw && (_screen == kCtw || _screen == kCtwMap)) {
		[self ctwBridge];
		_ctw->key(_ctwb, chars.UTF8String ?: "", int(e.keyCode), (e.modifierFlags & NSEventModifierFlagShift) != 0);
		[self ctwTakeAction];
		return;
	}
	if (_screen == kOptions && _opts && !_popup.open) {
		const NSEventModifierFlags m = e.modifierFlags;
		_opts->key(e.keyCode, c, (m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0, (m & NSEventModifierFlagOption) != 0,
		           (m & NSEventModifierFlagShift) != 0);
		return;
	}
	if (_screen == kSkirmishSetup && !_popup.open) {
		[self setupKey:e];
		return;
	}
	if (_screen == kMpConnect && !_popup.open) {
		[self connectKey:e];
		return;
	}
	if (_screen == kMpLobby && !_popup.open) {
		[self lobbyKey:e];
		return;
	}
	if (_popup.open) {
		if (_popup.kind == 2) return;  // busy
		if (esc) {
			_popup = Popup{};  // No / OK
		} else if (ret) {
			[self activate:kPopupBtn + (_popup.kind == 0 ? 0 : 0)];
		}
		return;
	}
	if (_dropOpen) {
		if (c == NSUpArrowFunctionKey) _difficulty = std::max(0, _difficulty - 1);
		else if (c == NSDownArrowFunctionKey) _difficulty = std::min(5, _difficulty + 1);
		else if (ret || esc) {
			_dropOpen = NO;
			[NSUserDefaults.standardUserDefaults setInteger:_difficulty forKey:launcher::kDefDifficulty];
		}
		return;
	}
	if (_screen == kMain) {
		auto step = [&](int d) {
			for (int k = 0; k < kMainCount; k++) {
				_focus = (_focus + d + kMainCount) % kMainCount;
				if (kMainItems[_focus].enabled) break;
			}
			_help = kMainItems[_focus].help;
		};
		if (c == NSUpArrowFunctionKey) step(-1);
		else if (c == NSDownArrowFunctionKey) step(1);
		else if (ret) [self activate:kMainBtn + _focus];
		else if (esc) [self openQuitPopup];  // TopMenuSelection::on_key_down: Esc at the top menu asks to quit
		return;
	}
	const int n = int(_entries.size()), rows = [self layout].rows;
	if (c == NSUpArrowFunctionKey) _sel = std::max(0, _sel - 1);
	else if (c == NSDownArrowFunctionKey) _sel = std::min(n - 1, _sel + 1);
	else if (c == NSPageUpFunctionKey) _sel = std::max(0, _sel - rows);
	else if (c == NSPageDownFunctionKey) _sel = std::min(n - 1, _sel + rows);
	else if (c == NSHomeFunctionKey) _sel = 0;
	else if (c == NSEndFunctionKey) _sel = n - 1;
	else if (c == NSLeftArrowFunctionKey && _screen != kTutorials && _screen != kRecorded) _difficulty = std::max(0, _difficulty - 1);
	else if (c == NSRightArrowFunctionKey && _screen != kTutorials && _screen != kRecorded) _difficulty = std::min(5, _difficulty + 1);
	else if (ret) [self play];
	else if (esc) {
		const Screen from = _screen;
		[self click];
		[self showScreen:@"main"];
		_focus = from == kRecorded ? 5 : int(from) - 1;
		_help = kMainItems[_focus].help;
		return;
	}
	if ((c == NSLeftArrowFunctionKey || c == NSRightArrowFunctionKey) && _screen != kRecorded)
		[NSUserDefaults.standardUserDefaults setInteger:_difficulty forKey:launcher::kDefDifficulty];
	_sel = std::max(0, _sel);
	[self scrollToSelection];
	[self remember];
}

// ---------------------------------------------------------------- the skirmish setup screen (random maps)

// The original's solo setup window (setupwin.xml PLAYERSWINSOLO + RULESWIN): a players table (name, nation, who, difficulty,
// team) on the left and the game rules as combo boxes on the right, drawn with the same pieces as the other front-end screens.
// The lists come from Data/rules.xml (the CATEGORIES the lobby's combo boxes fill from).

- (void)loadSetupLists {
	if (_lists.loaded) return;
	_lists.loaded = true;
	auto trim = [](std::string s) {
		const size_t a = s.find_first_not_of(" \t\r\n");
		if (a == std::string::npos) return std::string();
		return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
	};
	try {
		const ron::XmlElement doc = ron::parse_xml(_files->read_text("Data/rules.xml"));
		auto cats = [&](const char* id) {
			std::vector<std::string> out;
			ron::for_each_element(doc, "CATEGORIES", [&](const ron::XmlElement& e) {
				if (!out.empty() || ron::to_lower(e.get("id")) != id) return;
				for (const ron::XmlElement& c : e.children) {
					if (c.name != "CATEGORY") continue;
					std::string name = c.get("name");
					if (name.rfind("#ICON", 0) == 0) {  // "#ICON90Old World": an icon id, then the name
						size_t k = 5;
						while (k < name.size() && name[k] >= '0' && name[k] <= '9') k++;
						name = name.substr(k);
					}
					out.push_back(trim(name));
				}
			});
			return out;
		};
		_lists.style = cats("mapstyles");
		_lists.size = cats("mapsizes");
		_lists.town = cats("startingtowns");
		_lists.resources = cats("startingresources");
		_lists.start = cats("startingtechs");
		_lists.end = cats("endingtechs");
		_lists.victory = cats("victories");
		_lists.elim = cats("eliminations");
		_lists.pop = cats("poplimits");
		_lists.reveal = cats("revealmaps");
		_lists.diff = cats("difficulties");
		_lists.handicap = cats("handicaps");
		bool first = true;
		ron::for_each_element(doc, "TRIBES", [&](const ron::XmlElement& e) {
			if (!first) return;
			first = false;
			for (const ron::XmlElement& t : e.children) {
				if (t.name != "TRIBE") continue;
				const ron::XmlElement* k = t.child("KEY");
				std::string n = k ? trim(k->get("#text")) : std::string("?");
				if (!n.empty()) n[0] = char(std::toupper(static_cast<unsigned char>(n[0])));
				if (n == "Persian") n = "Persians";
				_lists.tribes.push_back(n);
			}
		});
	} catch (const std::exception& e) {
		std::fprintf(stderr, "skirmish setup lists: %s\n", e.what());
	}
	auto fallback = [](std::vector<std::string>& v, size_t n, const char* prefix) {
		for (size_t i = v.size(); i < n; i++) v.push_back(std::string(prefix) + " " + std::to_string(i + 1));
	};
	fallback(_lists.style, 23, "Map Style");
	fallback(_lists.size, 7, "Size");
	fallback(_lists.town, 4, "Start");
	fallback(_lists.resources, 13, "Resources");
	fallback(_lists.start, 10, "Age");
	fallback(_lists.end, 8, "Age");
	fallback(_lists.victory, 10, "Victory");
	fallback(_lists.elim, 4, "Elimination");
	fallback(_lists.pop, 6, "Population");
	fallback(_lists.reveal, 4, "Reveal");
	fallback(_lists.diff, 6, "Difficulty");
	fallback(_lists.tribes, 24, "Nation");
	if (_lists.victory.size() > 10) _lists.victory.resize(10);  // the shipped list ends with "Scenario Victory"
	if (_lists.start.size() > 10) _lists.start.resize(10);
}

// Entering the screen: the last setup (NSUserDefaults), seed on "random".
- (void)openSetup {
	[self loadSetupLists];
	_setup = ron::sim::default_random_setup(2);
	_setup.map_style = ron::sim::mapgen::kStyleOldWorld;
	_setup.map_size = 3;
	if (NSString* saved = [NSUserDefaults.standardUserDefaults stringForKey:@"SkirmishSetup"]) {
		ron::sim::RandomGameSetup s;
		if (ron::sim::parse_random_setup(saved.UTF8String, s)) _setup = s;
	}
	_setup.num_nations = std::clamp(_setup.num_nations, 2, 8);
	_setup.map_style = std::clamp(_setup.map_style, 0, 22);
	_setup.seed = 0;
	_seedRandom = YES;
	_seedText.clear();
	_seedFocus = NO;
	_openCombo = getenv("RON_SETUP_OPEN") ? atoi(getenv("RON_SETUP_OPEN")) : -1;  // snapshot check of an open list
	_openTop = 0;
	_sc.clear();
	_st.clear();
}

- (void)saveSetup {
	ron::sim::RandomGameSetup s = _setup;
	s.seed = 0;
	[NSUserDefaults.standardUserDefaults setObject:@(ron::sim::random_setup_string(s).c_str()) forKey:@"SkirmishSetup"];
}

static std::vector<std::string> numbered(int lo, int hi) {
	std::vector<std::string> v;
	for (int i = lo; i <= hi; i++) v.push_back(std::to_string(i));
	return v;
}

// Rebuilds the controls from the setup (positions follow setupwin.xml's two windows, in canvas pixels).
- (void)layoutSetup {
	[self loadSetupLists];
	_sc.clear();
	_st.clear();
	const BOOL lobby = _screen == kMpLobby;                              // the Multiplayer lobby shares these controls
	const BOOL editable = !lobby || (_room && _room->is_host() && !_mpSave && !_mpScenOn);  // only the host changes the setup, not of a saved game or a scenario
	ron::sim::RandomGameSetup* sp = &_setup;  // the closures outlive this call: capture the ivar's address, not self
	// ---- RULESWIN: the rules, label at the left, combo at the right
	{
		const int cx = 770, cw = 222, dy = lobby ? 29 : 33;
		int y = 106;
		auto add = [&](const char* label, const std::vector<std::string>& items, int sel, std::function<void(int)> set, BOOL enabled) {
			SetupCombo c;
			c.r = {cx, cx + cw, y, y + 21};
			c.items = items;
			c.sel = std::clamp(sel, 0, std::max(0, int(items.size()) - 1));
			c.set = std::move(set);
			c.enabled = enabled;
			c.label = label;
			c.lx = 592, c.ly = y;
			_sc.push_back(std::move(c));
			y += dy;
		};
		std::vector<std::string> styles = _lists.style;
		if (styles.size() > 23) styles.resize(23);
		add("Map Style:", styles, _setup.map_style, [sp](int v) { sp->map_style = v; }, editable);
		add("Map Size:", _lists.size, _setup.map_size, [sp](int v) { sp->map_size = v; }, editable);
		add("Nations:", numbered(2, 8), _setup.num_nations - 2, [sp](int v) { sp->num_nations = v + 2; }, editable);
		add("Start With:", _lists.town, _setup.starting_town, [sp](int v) { sp->starting_town = v; }, editable);
		add("Resources:", _lists.resources, _setup.starting_resources, [sp](int v) { sp->starting_resources = v; }, editable);
		add("Start Age:", _lists.start, _setup.starting_technology, [sp](int v) { sp->starting_technology = v; }, editable);
		add("End Age:", _lists.end, _setup.ending_technology, [sp](int v) { sp->ending_technology = v; }, editable);
		add("Victory:", _lists.victory, _setup.victory, [sp](int v) { sp->victory = v; }, editable);
		add("Elimination:", _lists.elim, _setup.elimination, [sp](int v) { sp->elimination = v; }, editable);
		add("Population:", _lists.pop, _setup.pop_limit, [sp](int v) { sp->pop_limit = v; }, editable);
		add("Reveal Map:", _lists.reveal, _setup.reveal_map, [sp](int v) { sp->reveal_map = v; }, editable);
		add("Random Starts:", {"No", "Yes"}, _setup.randomize_starts ? 1 : 0, [sp](int v) { sp->randomize_starts = v == 1; }, editable);
		if (lobby) {
			mp::Room* room = _room.get();
			add("Observers:", numbered(0, 6), room ? room->max_observers() : 0, [room](int v) { if (room) room->set_max_observers(v); }, room && room->is_host());
			_seedRect = _seedBtn = IfaceRect{0, 0, 0, 0};  // the host draws the seed when the game starts
		} else {
			_seedRect = {cx, cx + cw - 84, y, y + 21};
			_seedBtn = {cx + cw - 78, cx + cw, y - 3, y + 25};
			_st.push_back({592, y, "Seed:", 0});
		}
		_rulesBottom = y + dy;
	}
	if (lobby) {
		[self layoutLobbySeats];
		return;
	}
	// ---- PLAYERSWINSOLO: one row per seat
	{
		const int rowH = 38, y0 = 134;
		const int xn = 126, wn = 132, xw = 264, ww = 100, xd = 370, wd = 94, xt = 470, wt = 76;
		std::vector<std::string> nations{"Random"};
		for (const std::string& t : _lists.tribes) nations.push_back(t);
		std::vector<std::string> teams{"None", "1", "2", "3", "4"};
		for (int i = 0; i < 8; i++) {
			const int y = y0 + i * rowH;
			if (i >= _setup.num_nations) {
				_st.push_back({36, y + 3, "Seat " + std::to_string(i + 1) + ": closed", 1});
				continue;
			}
			ron::sim::NationSetup& ns = _setup.nation[size_t(i)];
			const BOOL taken = lobby && _room && ((_room->present_mask() >> i) & 1);  // a player of the room sits here
			std::string who = ns.human ? "Player" : "Computer " + std::to_string(i + 1);
			if (lobby) who = taken ? _room->name_of(i) : "Computer " + std::to_string(i + 1);
			while (lobby && _ui.text_width(_ui.font(14, -1), who) > 84 && who.size() > 3) who.pop_back();
			_st.push_back({36, y + 3, who, taken || !lobby ? 2 : 1});
			auto seat = [&](int x, int w, const std::vector<std::string>& items, int sel, bool enabled, std::function<void(int)> set) {
				SetupCombo c;
				c.r = {x, x + w, y, y + 21};
				c.items = items;
				c.sel = std::clamp(sel, 0, std::max(0, int(items.size()) - 1));
				c.set = std::move(set);
				c.enabled = enabled && editable;
				c.seat = i;
				_sc.push_back(std::move(c));
			};
			seat(xn, wn, nations, ns.tribe + 1, true, [sp, i](int v) { sp->nation[size_t(i)].tribe = v - 1; });
			if (lobby) {  // the status of the seat instead of the Human / Computer choice
				const BOOL ready = taken && _room->ready_of(i);
				_st.push_back({xw, y + 3, taken ? (i == 0 ? "Host" : ready ? "Ready" : "Not ready") : "Computer", taken ? (i == 0 || ready ? 3 : 4) : 1});
			} else {
				seat(xw, ww, {"Human", "Computer"}, ns.human ? 0 : 1, true, [sp, i](int v) { sp->nation[size_t(i)].human = v == 0; });
			}
			seat(xd, wd, _lists.diff, ns.difficulty, !ns.human && !taken, [sp, i](int v) { sp->nation[size_t(i)].difficulty = v; });
			seat(xt, wt, teams, ns.team + 1, true, [sp, i](int v) { sp->nation[size_t(i)].team = v - 1; });
		}
	}
}

- (void)layoutLobbySeats {
	const BOOL host = _room && _room->is_host();
	const BOOL saved = _mpSave != nullptr || (_room && _room->lobby().has("from_save"));
	const BOOL scen = _mpScenOn;
	const ron::net::SeatPlan plan = _room ? _room->lobby().plan(_setup.num_nations) : ron::net::SeatPlan();
	const int me = _room ? _room->slot() : 0;
	ron::sim::RandomGameSetup* sp = &_setup;
	ron::net::Lobby* lob = _room ? &_room->lobby() : nullptr;
	mp::Room* room = _room.get();
	const int rowH = 38, y0 = 134;
	const int xn = 112, wn = 100, xd = 216, wd = 84, xt = 304, wt = 66, xc = 374, wc = 76, xh = 454, wh = 106;
	std::vector<std::string> nations{"Random"};
	for (const std::string& t : _lists.tribes) nations.push_back(t);
	const std::vector<std::string> teams{"None", "1", "2", "3", "4"};
	const std::vector<std::string> colors{"Red", "Blue", "Pink", "Green", "Yellow", "Cyan", "White", "Orange"};
	_mpSwatch.clear();
	for (IfaceRect& r : _mpKickRect) r = IfaceRect{0, 0, 0, 0};
	for (int i = 0; i < 8; i++) {
		const int y = y0 + i * rowH;
		if (i >= _setup.num_nations) {
			_st.push_back({36, y + 3, "Seat " + std::to_string(i + 1) + ": closed", 1});
			continue;
		}
		ron::sim::NationSetup& ns = _setup.nation[size_t(i)];
		const int peer = plan.peer_of[size_t(i)];
		const BOOL taken = peer >= 0;
		const BOOL mine = taken && peer == me;
		std::string who = taken ? room->name_of(peer) : "AI " + std::to_string(i + 1);
		while (_ui.text_width(_ui.font(14, -1), who) > 62 && who.size() > 3) who.pop_back();
		const int colorIdx = ns.color >= 0 && ns.color < 8 ? ns.color : i;
		_mpSwatch.push_back({36, y + 5, colorIdx});
		_st.push_back({50, y + 3, who, taken ? 2 : 1});
		auto seat = [&](int x, int w, const std::vector<std::string>& items, int sel, bool enabled, int mpf, std::function<void(int)> set) {
			SetupCombo c;
			c.r = {x, x + w, y, y + 21};
			c.items = items;
			c.sel = std::clamp(sel, 0, std::max(0, int(items.size()) - 1));
			c.set = std::move(set);
			c.enabled = enabled;
			c.seat = i;
			c.mpf = mpf;
			_sc.push_back(std::move(c));
		};
		const BOOL canSeat = !saved && (host || mine);   // the host sets any seat, a guest asks for its own
		const BOOL canMap = canSeat && !scen;            // nation, team and colour are the scenario's in a scenario game
		seat(xn, wn, nations, ns.tribe + 1, canMap, 1, [sp, i](int v) {
			// a nation another seat holds changes places with it
			for (int j = 0; j < 8; j++)
				if (j != i && v > 0 && sp->nation[size_t(j)].tribe == v - 1) sp->nation[size_t(j)].tribe = sp->nation[size_t(i)].tribe;
			sp->nation[size_t(i)].tribe = v - 1;
		});
		const BOOL ready = taken && room->ready_of(peer);
		if (taken) _st.push_back({host && peer != me ? 80 : 44, y + 21, peer == 0 ? "Host" : ready ? "Ready" : "Not ready", peer == 0 || ready ? 5 : 6});   // the second line of the row (small)
		if (!taken) seat(xd, wd, _lists.diff, ns.difficulty, host && !saved && !scen, 0, [sp, i](int v) { sp->nation[size_t(i)].difficulty = v; });
		seat(xt, wt, teams, ns.team + 1, canMap, 2, [sp, i](int v) { sp->nation[size_t(i)].team = v - 1; });
		seat(xc, wc, colors, colorIdx, canMap, 3, [sp, i](int v) {
			for (int j = 0; j < 8; j++) {  // a colour another seat holds changes places with it
				const int cj = sp->nation[size_t(j)].color >= 0 && sp->nation[size_t(j)].color < 8 ? sp->nation[size_t(j)].color : j;
				if (j != i && cj == v) sp->nation[size_t(j)].color = sp->nation[size_t(i)].color >= 0 && sp->nation[size_t(i)].color < 8 ? sp->nation[size_t(i)].color : i;
			}
			sp->nation[size_t(i)].color = v;
		});
		if (taken) {
			const int hv = lob ? int(lob->get_int("handicap_" + std::to_string(i), 0)) : 0;
			seat(xh, wh, _lists.handicap, hv, canSeat, 4, [lob, i](int v) { if (lob) lob->set("handicap_" + std::to_string(i), v); });
			if (host && peer != me) _mpKickRect[size_t(peer)] = {36, 74, y + 21, y + 35};
		}
	}
}

- (std::string)seedShown {
	return _seedRandom ? std::string("Random") : _seedText;
}

- (void)drawCombo:(const SetupCombo&)c index:(int)idx {
	const IfaceRect D = c.r.offset(_ox, _oy);
	if (_combo[0].valid()) {
		const IfaceRect field{D.l, D.r - _combo[3].width + 2, D.t, D.b};
		_ui.expander(_combo[0], _combo[1], _combo[2], field);
		_ui.add(_combo[3], float(D.r - _combo[3].width), float(D.t));
	} else {
		[self thinBox:c.r fill:rgb(240, 240, 240)];
	}
	_ui.next_layer();
	const int fo = _ui.font(12, 1);
	const ron::FontMetrics* om = _ui.font_metrics(fo);
	const int oh = om ? om->font_height : 14;
	std::string text = c.items.empty() ? "" : c.items[size_t(c.sel)];
	const int room = c.r.w() - _combo[3].width - 12;
	while (text.size() > 3 && _ui.text_width(fo, text) > room) text.pop_back();
	_ui.text(fo, text, float(D.l + 7), float(D.t + (D.h() - oh) / 2), c.enabled ? kBlack : rgb(130, 130, 130));
	(void)idx;
}

- (void)drawSetup:(const Layout&)L {
	[self banner:L text:kListTitles[kSkirmishSetup]];
	[self layoutSetup];
	const int f14 = _ui.font(14, -1), f12 = _ui.font(12, -1);
	const ron::FontMetrics* m14 = _ui.font_metrics(f14);
	const int lh = m14 ? m14->line_height : 16;
	// The two windows: dark wash and the thin border of the setup windows.
	_ui.next_window();
	const IfaceRect players{20, 566, 88, 628}, rules{578, 1004, 88, 628};
	[self thinBox:players fill:argb(120, 0, 0, 0)];
	[self thinBox:rules fill:argb(120, 0, 0, 0)];
	_ui.next_layer();
	// Column titles (setupwin title_name / title_nation / title_who / title_diff / title_team)
	const struct {
		int x;
		const char* t;
	} cols[5] = {{36, "Player"}, {126, "Nation"}, {264, "Type"}, {370, "Difficulty"}, {470, "Team"}};
	for (const auto& c : cols) [self textAt:f14 text:c.t x:float(c.x) y:104 color:kSecondary];
	_ui.next_layer();
	for (const SetupText& t : _st) {
		[self textAt:f14 text:t.text x:float(t.x) y:float(t.y) color:t.kind == 1 ? rgb(130, 130, 130) : t.kind == 2 ? kWhite : kLabel];
	}
	for (const SetupCombo& c : _sc) {
		if (!c.label.empty()) [self textAt:f14 text:c.label x:float(c.lx) y:float(c.ly + 2) color:kLabel];
	}
	// Summary under the table: what the choices make.
	{
		_ui.next_layer();
		const std::string style = _setup.map_style < int(_lists.style.size()) ? _lists.style[size_t(_setup.map_style)] : "";
		const std::string size = _setup.map_size < int(_lists.size.size()) ? _lists.size[size_t(_setup.map_size)] : "";
		std::string text = style + ", " + size + ". ";
		if (_setup.map_style <= 5) text += "A map style is picked at random from the list. ";
		text += "Nations set to Random get a nation of their own; Computer players use their own difficulty.";
		int y = 134 + 8 * 38 + 4;
		for (const std::string& line : wrap_text(_ui, f12, text, players.w() - 32)) {
			[self textAt:f12 text:line x:36 y:float(y) color:kSecondary];
			y += lh;
		}
	}
	// The seed field and its Random button
	_ui.next_layer();
	{
		const IfaceRect D = _seedRect.offset(_ox, _oy);
		if (_combo[0].valid()) {
			_ui.expander(_combo[0], _combo[1], _combo[2], D);
		} else {
			[self thinBox:_seedRect fill:rgb(240, 240, 240)];
		}
		_ui.next_layer();
		const int fo = _ui.font(12, 1);
		const ron::FontMetrics* om = _ui.font_metrics(fo);
		const int oh = om ? om->font_height : 14;
		std::string shown = [self seedShown];
		if (_seedFocus && ((int)(_timeMs / 500) & 1) == 0) shown += "|";
		_ui.text(fo, shown, float(D.l + 7), float(D.t + (D.h() - oh) / 2), _seedRandom && !_seedFocus ? rgb(110, 110, 110) : kBlack);
		[self popupButton:_seedBtn label:"Random" state:(_pressed == kSeedRandom ? 2 : _hover == kSeedRandom ? 1 : 0) font:f12];
	}
	// Combos (closed state), then the open list on top
	_ui.next_layer();
	for (size_t i = 0; i < _sc.size(); i++) {
		[self drawCombo:_sc[i] index:int(i)];
		_ui.next_layer();
	}
	if (_openCombo >= 0 && size_t(_openCombo) < _sc.size()) [self drawOpenList];
	else _setupListVis = 0;
	_ui.next_window();
	const int fb = _ui.font(14, -1);
	[self popupButton:L.back label:"Back" state:(_pressed == kBack ? 2 : _hover == kBack ? 1 : 0) font:fb];
	[self popupButton:L.play label:"Play" state:(_pressed == kPlay ? 2 : _hover == kPlay ? 1 : 0) font:fb];
}

- (void)drawOpenList {
	const SetupCombo& c = _sc[size_t(_openCombo)];
	const int ih = 20, n = int(c.items.size()), vis = std::min(n, 12);
	IfaceRect list{c.r.l, c.r.r - _combo[3].width, c.r.b + 1, c.r.b + 1 + vis * ih + 6};
	if (list.b > kH - 8) {
		const int h = list.b - list.t;
		list.b = c.r.t - 1;
		list.t = list.b - h;
	}
	_ui.next_window();
	[self thinBox:list fill:rgb(244, 244, 240)];
	_ui.next_layer();
	const int fo = _ui.font(12, 1);
	const ron::FontMetrics* om = _ui.font_metrics(fo);
	const int oh = om ? om->font_height : 14;
	for (int k = 0; k < vis; k++) {
		const int item = _openTop + k;
		if (item >= n) break;
		const IfaceRect it{list.l + 3, list.r - 3, list.t + 3 + k * ih, list.t + 3 + (k + 1) * ih};
		const IfaceRect I = it.offset(_ox, _oy);
		const bool hot = _hover == kSetupItem + k || (_hover < kSetupItem && item == c.sel);
		if (hot) _ui.fill(float(I.l), float(I.t), float(I.w()), float(I.h()), rgb(53, 47, 132), UiType::ColorBlend);
		_ui.next_layer();
		_ui.text(fo, c.items[size_t(item)], float(I.l + 5), float(I.t + (ih - oh) / 2), hot ? kWhite : kBlack);
	}
	_setupListRect = list;
	_setupListVis = vis;

}

- (int)hitSetup:(int)x y:(int)y {
	const Layout L = [self layout];
	[self layoutSetup];
	if (_openCombo >= 0 && size_t(_openCombo) < _sc.size()) {
		const SetupCombo& c = _sc[size_t(_openCombo)];
		const int ih = 20, vis = std::min(int(c.items.size()), 12);
		IfaceRect list{c.r.l, c.r.r - _combo[3].width, c.r.b + 1, c.r.b + 1 + vis * ih + 6};
		if (list.b > kH - 8) {
			const int h = list.b - list.t;
			list.b = c.r.t - 1;
			list.t = list.b - h;
		}
		for (int k = 0; k < vis; k++) {
			if (_openTop + k >= int(c.items.size())) break;
			if (IfaceRect{list.l + 3, list.r - 3, list.t + 3 + k * ih, list.t + 3 + (k + 1) * ih}.contains(x, y)) return kSetupItem + k;
		}
		if (c.r.contains(x, y)) return kSetupCombo + _openCombo;
		return list.contains(x, y) ? kNone : kNone;
	}
	if (L.back.contains(x, y)) return kBack;
	if (L.play.contains(x, y)) return kPlay;
	if (_seedBtn.contains(x, y)) return kSeedRandom;
	if (_seedRect.contains(x, y)) return kSeedField;
	for (size_t i = 0; i < _sc.size(); i++) {
		if (_sc[i].enabled && _sc[i].r.contains(x, y)) return kSetupCombo + int(i);
	}
	return kNone;
}

- (void)chooseCombo:(int)idx item:(int)item {
	if (idx < 0 || size_t(idx) >= _sc.size()) return;
	SetupCombo& c = _sc[size_t(idx)];
	if (item < 0 || size_t(item) >= c.items.size()) return;
	if (_screen == kMpLobby && c.mpf && _room && !_room->is_host()) {  // a guest asks the host; the table shows the answer
		_room->request(ron::net::SeatField(c.mpf), c.mpf <= 2 ? item - 1 : item);
		return;
	}
	c.sel = item;
	if (c.set) c.set(item);
	if (_screen != kMpLobby || (_room && _room->is_host())) [self saveSetup];
}

- (void)playSetup {
	[self click];
	ron::sim::RandomGameSetup s = _setup;
	if (_seedRandom || _seedText.empty()) s.seed = arc4random();
	else s.seed = uint32_t(std::strtoull(_seedText.c_str(), nullptr, 10));
	if (s.seed == 0) s.seed = 1;
	[self saveSetup];
	[self.host frontEndPlayRandom:s];
}

// Keys on the setup screen: the open list (arrows, Return, Escape), the seed field (digits), Escape back to the list.
- (void)setupKey:(NSEvent*)e {
	NSString* chars = e.charactersIgnoringModifiers;
	const unichar c = chars.length ? [chars characterAtIndex:0] : 0;
	const bool ret = c == '\r' || c == 3, esc = e.keyCode == 53;
	if (_openCombo >= 0 && size_t(_openCombo) < _sc.size()) {
		SetupCombo& cb = _sc[size_t(_openCombo)];
		const int n = int(cb.items.size());
		if (c == NSUpArrowFunctionKey) [self chooseCombo:_openCombo item:std::max(0, cb.sel - 1)];
		else if (c == NSDownArrowFunctionKey) [self chooseCombo:_openCombo item:std::min(n - 1, cb.sel + 1)];
		else if (ret || esc) _openCombo = -1;
		if (_openCombo >= 0) {
			const int sel = _sc[size_t(_openCombo)].sel;
			if (sel < _openTop) _openTop = sel;
			if (sel >= _openTop + 12) _openTop = sel - 11;
		}
		return;
	}
	if (_seedFocus) {
		if (c >= '0' && c <= '9' && _seedText.size() < 10) {
			if (_seedRandom) _seedText.clear();
			_seedRandom = NO;
			_seedText += char(c);
		} else if (e.keyCode == 51 || c == 127) {  // Delete
			if (!_seedText.empty()) _seedText.pop_back();
			if (_seedText.empty()) _seedRandom = YES;
		} else if (ret || esc) {
			_seedFocus = NO;
		}
		return;
	}
	if (esc) {
		[self click];
		[self showScreen:@"skirmish"];
	} else if (ret) {
		[self playSetup];
	}
}

- (NSCursor*)cursor {
	if (!_cursor) _cursor = ron_game_cursor(*_files, ron::kCursorMain);
	return _cursor;
}

// ---------------------------------------------------------------- Options (options_ui.hpp)

// Entering the screen: the page opens over the Main Menu's backdrop (OptionsWin is a window of the top menu). What it edits is
// prefs::Prefs (preferences.txt under Application Support); applying sets the menu's own sound and difficulty and leaves the rest
// to the game, which reads the same file when it starts.
- (void)applyPrefsToMenu:(const prefs::Prefs&)p {
	if (_audio) {
		const float fx = float(p.fx_volume) / 255.0f;
		_audio->set_volumes(fx, fx, p.music_in_menu ? float(p.music_volume) / 255.0f : 0.0f);
	}
	_difficulty = std::clamp(p.difficulty, 0, 5);
	[NSUserDefaults.standardUserDefaults setInteger:_difficulty forKey:launcher::kDefDifficulty];
	NSWindow* win = self.view.window;
	if (win && ((win.styleMask & NSWindowStyleMaskFullScreen) != 0) != p.fullscreen) [win toggleFullScreen:nil];
}

- (void)openOptions {
	if (!_opts || !_opts->ready()) {
		[self showError:@"The game's options window (Data/optionswin.xml) could not be read."];
		_screen = kMain;
		return;
	}
	optui::Host h;
	__unsafe_unretained FrontEnd* me = self;
	h.live = [me](prefs::Prefs& p) { p.difficulty = std::clamp(me->_difficulty, 0, 5); };
	h.apply = [me](const prefs::Prefs& p) { [me applyPrefsToMenu:p]; };
	h.preview = [me](const prefs::Prefs& p) {
		if (me->_audio) me->_audio->set_volumes(float(p.fx_volume) / 255.0f, float(p.fx_volume) / 255.0f, p.music_in_menu ? float(p.music_volume) / 255.0f : 0.0f);
	};
	h.type_name = [me](int type) -> std::string {
		if (!me->_optRules) {
			try {
				ron::sim::LoadOptions lo;
				lo.lang_suffix = uistr::suffix();
				me->_optRules = std::make_unique<ron::sim::Rules>(ron::sim::load_rules(*me->_files, lo));
			} catch (...) {
				return {};
			}
		}
		if (type < 0 || size_t(type) >= me->_optRules->types.size()) return {};
		const ron::sim::Type& T = me->_optRules->types[size_t(type)];
		std::string n = ron::sim::to_utf8(T.display_name);
		return n.empty() ? ron::sim::to_utf8(T.name) : n;
	};
	h.resolution = [me]() -> std::string {
		const CGSize sz = me.view ? me.view.bounds.size : CGSizeMake(kW, kH);
		return std::to_string(int(sz.width)) + " x " + std::to_string(int(sz.height));
	};
	h.sound = [me](const char* key) {
		if (!me->_audio) return;
		const std::string f = [me soundFile:key];
		if (!f.empty()) me->_audio->play_effect(f);
	};
	h.closed = [me]() { [me showScreen:@"main"]; me->_focus = 6; me->_help = kMainItems[6].help; };
	int pane = 0;
	if (const char* e = getenv("RON_OPT_PANE")) pane = std::atoi(e);  // snapshot checks: Game, Hotkeys, Mouse, Graphics, Sound, Player
	_opts->open(h, pane);
	if (const char* e = getenv("RON_OPT_HK")) _opts->test_select_hotkey(e);
	if (const char* e = getenv("RON_OPT_COMBO")) _opts->test_open_combo(std::atoi(e));
}

// Headless test (RON_OPT_SCRIPT, used with --menu options --snapshot f.png): steps separated by ';' in the page's own pixels (880 x 600):
// c:X,Y click, d:X,Y / m:X,Y / u:X,Y press, drag, release, k:CODE[:CHARS[:cas]] a key (Mac key code, first character, c / a / s modifiers),
// w:DY@X,Y wheel, snap:NAME writes <f>_NAME.png, state prints the working copy.
- (void)runOptionsScript:(NSString*)script path:(NSString*)path width:(int)w height:(int)h scale:(float)scale {
	auto render = [&](NSString* file) {
		id<MTLTexture> out = rm::make_readback_target(_device, w, h);
		id<MTLCommandBuffer> cb = [_queue commandBuffer];
		[self renderInto:out commandBuffer:cb points:CGSizeMake(w / scale, h / scale)];
		[cb commit];
		[cb waitUntilCompleted];
		if (file) rm::write_png(out, file.UTF8String);
	};
	render(nil);  // a priming frame fixes the window mapping
	for (NSString* raw in [script componentsSeparatedByString:@";"]) {
		NSString* st = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		if (!st.length || !_opts->is_open()) continue;
		const int ox = _ox + (kW - optui::Page::kW) / 2, oy = _oy + (kH - optui::Page::kH) / 2;
		int x = 0, y = 0, code = 0;
		float dy = 0;
		const char* c = st.UTF8String;
		if (std::sscanf(c, "c:%d,%d", &x, &y) == 2) {
			_opts->mouse_move(ox + x, oy + y);
			_opts->mouse_down(ox + x, oy + y);
			_opts->mouse_up(ox + x, oy + y);
		} else if (std::sscanf(c, "d:%d,%d", &x, &y) == 2) {
			_opts->mouse_move(ox + x, oy + y);
			_opts->mouse_down(ox + x, oy + y);
		} else if (std::sscanf(c, "m:%d,%d", &x, &y) == 2) {
			_opts->mouse_drag(ox + x, oy + y);
		} else if (std::sscanf(c, "u:%d,%d", &x, &y) == 2) {
			_opts->mouse_up(ox + x, oy + y);
		} else if (std::sscanf(c, "w:%f@%d,%d", &dy, &x, &y) == 3) {
			_opts->wheel(dy, ox + x, oy + y);
		} else if (std::sscanf(c, "k:%d", &code) == 1) {
			unsigned ch = 0;
			bool ctrl = false, alt = false, shift = false;
			const char* a = std::strchr(c + 2, ':');
			if (a) {
				ch = unsigned(static_cast<unsigned char>(a[1]));
				if (const char* m = std::strchr(a + 1, ':')) {
					ctrl = std::strchr(m, 'c') != nullptr, alt = std::strchr(m, 'a') != nullptr, shift = std::strchr(m, 's') != nullptr;
				}
			}
			_opts->key(static_cast<unsigned short>(code), ch, ctrl, alt, shift);
		} else if (std::strncmp(c, "snap:", 5) == 0) {
			NSString* base = [path stringByDeletingPathExtension];
			NSString* f = [NSString stringWithFormat:@"%@_%s.png", base, c + 5];
			render(f);
			std::printf("wrote %s\n", f.UTF8String);
		} else if (std::strcmp(c, "state") == 0) {
			std::printf("[options] %s\n", _opts->state_text().c_str());
		}
		render(nil);
	}
}

- (void)drawOptions {
	if (!_opts || !_opts->is_open()) return;
	_ui.next_window();
	_ui.fill(0, 0, float(_uiW), float(_uiH), argb(90, 0, 0, 0), UiType::ColorBlend);
	_ui.next_layer();
	_opts->draw(_ui, _ox + (kW - optui::Page::kW) / 2, _oy + (kH - optui::Page::kH) / 2);
}

// ---------------------------------------------------------------- Multiplayer (lobby.h)

// The two Multiplayer screens. Connect: your name, a port to host on, an address to join. Lobby: the Skirmish setup screen's controls
// (the host's) with the seats filled by the players of the room, ready flags and a chat. Play starts the lockstep session (main.mm).

static std::string clean_name(std::string s) {
	while (!s.empty() && s.back() == ' ') s.pop_back();
	return s.empty() ? std::string("Player") : s;
}

- (void)mpOpen {
	NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
	if (_mpName.empty()) {
		NSString* n = [d stringForKey:@"MpName"];
		_mpName = n.length ? n.UTF8String : "Player";
		NSString* p = [d stringForKey:@"MpPort"];
		_mpPort = p.length ? p.UTF8String : "7777";
		if (const char* e = getenv("RON_MP_NAME")) _mpName = e;   // window tests (docs/ui-port.md 13.7): the name and port the screens start with
		if (const char* e = getenv("RON_MP_PORT")) _mpPort = e;
		NSString* a = [d stringForKey:@"MpAddress"];
		_mpAddr = a.length ? a.UTF8String : "";
	}
	_mpMessage.clear();
	_mpStarting = NO;
	if (_screen == kMpConnect) {
		_room.reset();
		_mpField = 0;
		_lanSel = -1;
		std::string err;
		if (!_lan.is_open() && !_lan.open(mp::lan_port(), err)) std::fprintf(stderr, "lan browser: %s\n", err.c_str());
		if (getenv("RON_MP_DEMO_LAN")) {  // snapshot: rooms of other machines on the network
			mp::spawn_demo_hosts(6);
			[NSThread sleepForTimeInterval:1.4];
		}
		return;
	}
	_lan.close();
	// the lobby: the host edits the Skirmish setup, a guest sees the host's
	_mpField = -1;
	_mpChat.clear();
	_mpChatSeen = 0;
	_mpPublished.clear();
	_mpLines.clear();
	_mpSave.reset();
	_mpSaveText.clear();
	_mpScenOn = NO;
	_mpCtwOn = NO;
	_mpCtwText.clear();
	[self openSetup];
	_setup.game_speed = 2;
	_seedRandom = YES;
	if (_room && _room->is_host()) {
		ron::net::LobbyLimits lim;
		lim.tribes = int(_lists.tribes.size());
		lim.handicaps = int(_lists.handicap.size());
		_room->lobby().set_limits(lim);
		if (!_room->lobby().has("max_observers")) _room->set_max_observers(2);
		std::string addr;
		for (const std::string& a : _room->lan_addresses()) addr += (addr.empty() ? "" : ", ") + a;
		_mpLines.push_back({1, "You are the host. Players on your network find this game in their Multiplayer list, or type " + (addr.empty() ? std::string("your address") : addr) + ":" + std::to_string(_room->port()) + "."});
		_mpLines.push_back({1, "Over the internet: forward TCP port " + std::to_string(_room->port()) + " on your router to this Mac and give the players its public address (or a host name) with the port; the game cannot open the port for you."});
	} else if (_room) {
		_mpLines.push_back({1, "Connected. The host sets the game up; choose your nation, team and colour in your row, then press Ready."});
	}
}

- (void)mpLeave:(NSString*)message {
	_room.reset();
	_mpStarting = NO;
	[self showScreen:@"multiplayer"];
	_mpMessage = message ? message.UTF8String : "";
}

static bool parse_port(const std::string& s, int& out) {
	if (s.empty() || s.size() > 5) return false;
	for (char c : s)
		if (c < '0' || c > '9') return false;
	out = std::atoi(s.c_str());
	return out >= 1024 && out <= 65535;
}

- (void)mpSaveFields {
	NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
	[d setObject:@(_mpName.c_str()) forKey:@"MpName"];
	[d setObject:@(_mpPort.c_str()) forKey:@"MpPort"];
	[d setObject:@(_mpAddr.c_str()) forKey:@"MpAddress"];
}

- (void)mpHost {
	int port = 0;
	if (!parse_port(_mpPort, port)) {
		_mpMessage = "The port must be a number between 1024 and 65535.";
		return;
	}
	[self mpSaveFields];
	auto room = std::make_shared<mp::Room>();
	std::string err;
	if (!room->host(uint16_t(port), clean_name(_mpName), err)) {
		_mpMessage = err;
		return;
	}
	_room = room;
	[self click];
	[self showScreen:@"mp-lobby"];
}

- (void)mpJoin {
	// "address", "address:port" or "name.example.org:port" (a host name is looked up by Room::join)
	ron::net::Endpoint ep;
	std::string perr;
	if (!ron::net::parse_endpoint(_mpAddr, 7777, ep, perr)) {
		_mpMessage = perr.empty() ? "Type the host's address, for example 192.168.0.12:7777." : perr + ".";
		return;
	}
	const std::string host = ep.host;
	const int port = ep.port;
	[self mpSaveFields];
	[self click];
	[self setBusy:@"Connecting…"];
	const std::string name = clean_name(_mpName);
	__unsafe_unretained FrontEnd* me = self;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.15 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		auto room = std::make_shared<mp::Room>();
		std::string err;
		const bool ok = room->join(host, uint16_t(port), name, 4000, err);
		[me setBusy:nil];
		if (!ok) {
			me->_mpMessage = err;
			return;
		}
		me->_room = room;
		[me showScreen:@"mp-lobby"];
	});
}

// The colour slot of a game slot's seat (its choice, else its number).
- (int)seatColor:(int)slot {
	if (slot < 0 || slot > 7) return 8;
	const int c = _setup.nation[size_t(slot)].color;
	return c >= 0 && c < 8 ? c : slot;
}

// The room's traffic every frame the lobby is up: new players, departures, chat, the setup changes of the host, the START.
- (void)mpPump {
	if (!_room || _mpStarting) return;
	_room->pump();
	for (std::string n; !(n = _room->take_notice()).empty();) _mpLines.push_back({1, n});
	const ron::net::SeatPlan plan = _room->lobby().plan(_setup.num_nations);
	ron::net::Lobby::ChatLine line;
	while (_room->lobby().take_chat(line)) {
		if (line.kind == 3) {
			_mpLines.push_back({1, line.text});
			continue;
		}
		std::string who = _room->name_of(line.from);
		if (who.empty()) who = "Player " + std::to_string(line.from + 1);
		MpLine ml;
		ml.kind = line.kind == 1 ? 2 : line.kind == 2 ? 3 : 0;
		ml.prefix = who + (line.kind == 1 ? " (team)" : line.kind == 2 ? " (private)" : "") + ": ";
		ml.text = line.text;
		const int seat = line.from >= 0 && line.from < 8 ? plan.slot_of[size_t(line.from)] : -1;
		ml.color = seat >= 0 && seat < plan.nations ? mp::seat_text_color([self seatColor:seat]) : rgb(190, 190, 190);
		_mpLines.push_back(ml);
	}
	if (_room->host_lost()) {
		[self mpLeave:_room->kicked() ? @"The host removed you from the room." : @"The host closed the room."];
		return;
	}
	if (_room->is_host()) {
		// a guest's request: applied to the table already (or waiting for the host); mirror the three seat fields into the setup
		ron::net::SeatRequest rq;
		while (_room->lobby().take_request(rq)) {
			const std::string who = _room->name_of(rq.peer);
			if (rq.pending) {
				_mpLines.push_back({1, who + " asks for " + [self requestText:rq] + ". Allow or deny it on the right."});
				continue;
			}
			if (rq.seat >= 0 && rq.seat < 8) {
				ron::sim::NationSetup& ns = _setup.nation[size_t(rq.seat)];
				if (rq.field == ron::net::SeatField::Tribe) ns.tribe = rq.value;
				else if (rq.field == ron::net::SeatField::Team) ns.team = rq.value;
				else if (rq.field == ron::net::SeatField::Color) ns.color = rq.value;
			}
			_mpLines.push_back({1, rq.field == ron::net::SeatField::Observer ? who + (rq.value ? " now watches the game." : " now plays.") : who + " now has " + [self requestText:rq] + "."});
		}
		// the seats of the players are human, the rest computer; changes go to the guests (a saved game keeps its own seats)
		if (!_mpSave) {
			for (int i = 0; i < 8; i++) _setup.nation[size_t(i)].human = i < plan.nations && plan.peer_of[size_t(i)] >= 0;
			_setup.num_nations = std::clamp(std::max(_setup.num_nations, plan.nations), 2, 8);
		}
		if (_room->counting()) {
			ron::sim::RandomGameSetup probe = _setup;
			if (!_room->guests_ready() || !_room->seat_humans(probe)) {
				_room->cancel_countdown();
				_mpLines.push_back({1, "The countdown stopped: " + std::string(_room->humans() < 2 ? "a player left the room." : "a player is not ready.")});
			} else if (_room->tick_countdown()) {
				[self mpStartNow];
				return;
			}
		}
		ron::sim::RandomGameSetup s = _setup;
		s.seed = 0;
		const std::string cur = ron::sim::random_setup_string(s);
		if (cur != _mpPublished) {
			_room->publish(s);
			_mpPublished = cur;
		}
		// the LAN announcement: the map in words
		std::string map;
		if (_mpScenOn) map = _mpScen.title;
		else if (_mpCtwOn) map = "Conquer the World";
		else {
			map = _setup.map_style < int(_lists.style.size()) ? _lists.style[size_t(_setup.map_style)] : "Random map";
			if (_setup.map_size < int(_lists.size.size())) map += ", " + _lists.size[size_t(_setup.map_size)];
		}
		_room->announce(map, uint8_t((_mpScenOn ? ron::net::kLanScenario : 0) | (_mpSave ? ron::net::kLanSaved : 0) | (_mpCtwOn ? ron::net::kLanCtw : 0)));
	} else {
		ron::sim::RandomGameSetup s;
		if (_room->read_setup(s)) {
			_setup = s;
			_mpScenOn = _room->read_scenario(_mpScen);
			std::vector<uint8_t> camp;
			int outcome = 0;
			if (mp::ctw_guest_result(*_room, _mpCtwSeen, camp, outcome) && [self.host respondsToSelector:@selector(frontEndCtwReceived:outcome:)]) {
				NSString* news = [self.host frontEndCtwReceived:camp outcome:outcome];
				_mpLines.push_back({1, std::string("The host's campaign moved on: ") + (outcome == 1 ? "the battle was won." : outcome == 0 ? "the battle was lost." : "the host retreated.") +
				                          (news.length ? std::string(" ") + news.UTF8String : std::string())});
			}
			const BOOL fromSave = _room->lobby().has("from_save");
			if (_room->started() && (!fromSave || _room->lobby().file_complete() || _room->lobby().file_failed())) {
				_mpStarting = YES;
				if (fromSave && !_room->lobby().file_complete()) {
					[self mpLeave:@"The saved game did not arrive intact."];
					return;
				}
				std::shared_ptr<std::vector<uint8_t>> save;
				if (fromSave) save = std::make_shared<std::vector<uint8_t>>(_room->lobby().file());
				if (_room->lobby().get_int("ctw_game") != 0 && [self.host respondsToSelector:@selector(frontEndPlayMultiplayerCtw:session:)]) {
					[self.host frontEndPlayMultiplayerCtw:_room session:nullptr];
				} else if (_mpScenOn && [self.host respondsToSelector:@selector(frontEndPlayMultiplayerScenario:)]) {
					[self.host frontEndPlayMultiplayerScenario:_room];
				} else {
					[self.host frontEndPlayMultiplayer:_room setup:s save:save];
				}
			}
		}
	}
}

// Chat: "/t text" to the team, "/w name text" to one player, anything else to everybody.
- (void)mpSend {
	if (!_room || _mpChat.empty()) return;
	const ron::net::SeatPlan plan = _room->lobby().plan(_setup.num_nations);
	const int mySeat = plan.slot_of[size_t(_room->slot() & 7)];
	MpLine ml;
	ml.prefix = clean_name(_mpName) + ": ";
	ml.color = mySeat >= 0 && mySeat < plan.nations ? mp::seat_text_color([self seatColor:mySeat]) : rgb(190, 190, 190);
	std::string text = _mpChat;
	if (text.rfind("/t ", 0) == 0 || text.rfind("/team ", 0) == 0) {
		text = text.substr(text.find(' ') + 1);
		if (!_room->say_to(1, -1, text)) _mpLines.push_back({1, "Nobody is on your team, so nobody heard that."});
		else {
			ml.kind = 2;
			ml.prefix = clean_name(_mpName) + " (team): ";
			ml.text = text;
			_mpLines.push_back(ml);
		}
	} else if (text.rfind("/w ", 0) == 0 || text.rfind("/whisper ", 0) == 0) {
		std::string rest = text.substr(text.find(' ') + 1);
		int target = -1;
		std::string label;
		for (int p = 0; p < 8 && target < 0; p++) {  // the longest name that starts the text
			const std::string nm = _room->name_of(p);
			if (nm.empty() || p == _room->slot()) continue;
			std::string a = rest.substr(0, nm.size()), b = nm;
			for (char& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
			for (char& c : b) c = char(std::tolower(static_cast<unsigned char>(c)));
			if (a == b && (rest.size() == nm.size() || rest[nm.size()] == ' ')) target = p, label = nm;
		}
		if (target < 0) {
			_mpLines.push_back({1, "Whisper to whom? Type /w and a player's name, then the message."});
		} else {
			const std::string msg = rest.size() > label.size() ? rest.substr(label.size() + 1) : std::string();
			if (!msg.empty() && _room->say_to(2, target, msg)) {
				ml.kind = 3;
				ml.prefix = "To " + label + ": ";
				ml.text = msg;
				_mpLines.push_back(ml);
			}
		}
	} else {
		_room->say(text);
		ml.text = text;
		_mpLines.push_back(ml);
	}
	_mpChat.clear();
}

// Play: the host starts the game (when every guest is ready and a second player is in; a ten second countdown first, as the original's), a guest
// toggles Ready.
- (void)mpPlay {
	if (!_room) return;
	if (!_room->is_host()) {
		[self click];
		_room->set_ready(!_room->ready_of(_room->slot()));
		return;
	}
	if (_room->counting()) {
		[self click];
		_room->cancel_countdown();
		return;
	}
	ron::sim::RandomGameSetup s = _setup;
	if (_mpSave) {  // continue a saved game: the players must sit in the seats that were human
		uint32_t need = 0;
		for (int i = 0; i < s.num_nations && i < 8; i++)
			if (s.nation[size_t(i)].human) need |= 1u << i;
		if (_room->plan().human_mask() != need) {
			_mpLines.push_back({1, "The saved game needs players in exactly its human seats; the players in the room do not match them."});
			return;
		}
	} else if (_mpScenOn && _room->plan().players > _mpScen.seats) {
		_mpLines.push_back({1, "The scenario has " + std::to_string(_mpScen.seats) + " seats but " + std::to_string(_room->plan().players) + " players are in the room; some must watch."});
		return;
	} else if (!_room->seat_humans(s)) {
		_mpLines.push_back({1, _room->humans() < 2 ? "Nobody has joined yet: a multiplayer game needs at least two players." : "The observers do not fit: there are more machines than the eight slots allow."});
		return;
	}
	if (!_room->guests_ready()) {
		_mpLines.push_back({1, "Every player must be ready before the game can start."});
		return;
	}
	if (_mpCtwOn) {
		_mpLines.push_back({1, "Choose the battle on the campaign map (Conquer the World...); it starts the game."});
		return;
	}
	[self click];
	if (getenv("RON_MP_DEMO_START") || getenv("RON_MP_NO_COUNTDOWN")) [self mpStartNow];
	else _room->begin_countdown(10);
}

// The host starts the game now: fix the setup, the seed and the plan, tell the guests, begin.
- (void)mpStartNow {
	ron::sim::RandomGameSetup s = _setup;
	if (_mpSave) {
		_room->publish(s);
		_room->lobby().set("from_save", "1");
		_room->lobby().send_file(*_mpSave);
		if (!_room->start(s.seed)) {
			_mpLines.push_back({1, "The game cannot start with these seats."});
			return;
		}
		_mpStarting = YES;
		[self.host frontEndPlayMultiplayer:_room setup:s save:_mpSave];
		return;
	}
	if (!_room->seat_humans(s)) {
		_mpLines.push_back({1, "The game cannot start with these seats."});
		return;
	}
	s.game_speed = 2;
	s.seed = arc4random();
	if (s.seed == 0) s.seed = 1;
	[self saveSetup];
	_room->publish(s);
	if (_mpScenOn) _room->publish_scenario(_mpScen);
	if (!_room->start(s.seed)) {
		_mpLines.push_back({1, "The game cannot start with these seats."});
		return;
	}
	_mpStarting = YES;
	if (_mpScenOn && [self.host respondsToSelector:@selector(frontEndPlayMultiplayerScenario:)]) [self.host frontEndPlayMultiplayerScenario:_room];
	else [self.host frontEndPlayMultiplayer:_room setup:s save:nullptr];
}

// The file chooser of the lobby's buttons. RON_MP_PICK=PATH answers it without a window (the window tests, docs/ui-port.md 13.7: a modal panel cannot be
// driven from a script); otherwise the open panel, nil when cancelled.
// ---------------------------------------------------------------- the Recorded Games screen (docs/ui-port.md section 18)

// The folder the app records multiplayer games into (mp::default_record_dir) as a list: name, date, map, players, duration. The files are found at
// once; their headers are read on a thread and the rows fill in as they finish.
- (void)openRecorded {
	[self loadSetupLists];
	_recDir = mp::default_record_dir();
	_recRows.clear();
	_recSeen = ~0u;
	_recScan.start(_recDir);
	[self refreshRecorded];
}

- (std::string)recordedDetail:(const recplay::Row&)r {
	char date[64] = "";
	const std::time_t t = std::time_t(r.mtime);
	std::tm tm{};
	localtime_r(&t, &tm);
	std::strftime(date, sizeof date, "%b %d, %Y %H:%M", &tm);
	std::string d = date;
	if (!r.read) return d + "   |   reading...";
	const ron::net::PlaybackSummary& s = r.summary;
	if (!s.ok) return d + "   |   cannot be read: " + s.error;
	auto name = [](const std::vector<std::string>& v, int i, const char* fallback) { return i >= 0 && size_t(i) < v.size() ? v[size_t(i)] : std::string(fallback); };
	if (!s.ctw_map.empty())   // a Conquer the World battle on a campaign map: the campaign and the map, not a generator style and size
		d += "   |   " + (s.ctw_campaign.empty() ? std::string() : s.ctw_campaign + ": ") + s.ctw_map;
	else
		d += "   |   " + name(_lists.style, s.map_style, "Map") + ", " + name(_lists.size, s.map_size, "?");
	d += "   |   " + std::to_string(s.players) + (s.players == 1 ? " player: " : " players: ");
	for (size_t i = 0; i < s.tribes.size() && i < 4; i++) d += std::string(i ? ", " : "") + name(_lists.tribes, s.tribes[i], "?");
	if (s.tribes.size() > 4) d += ", ...";
	d += "   |   " + recplay::clock_text(s.end_frame - s.start_frame);
	if (s.original) d += "   |   original game";
	if (s.ctw_battle) d += "   |   Conquer the World battle";
	if (s.recovery) d += "   |   starts at frame " + std::to_string(s.start_frame) + " (recovery)";
	if (!s.playable) d += "   |   " + s.unplayable;
	return d;
}

- (void)refreshRecorded {
	if (_screen != kRecorded || _recScan.changed() == _recSeen) return;
	_recSeen = _recScan.changed();
	std::string keep;
	if (_sel >= 0 && size_t(_sel) < _entries.size()) keep = _entries[size_t(_sel)].path;
	_recRows = _recScan.snapshot();
	_entries.clear();
	for (const recplay::Row& r : _recRows) {
		launcher::Entry e;
		e.path = r.path;
		e.title = r.file.size() > 4 && r.file.compare(r.file.size() - 4, 4, ".rcx") == 0 ? r.file.substr(0, r.file.size() - 4) : r.file;
		e.detail = [self recordedDetail:r];
		_entries.push_back(std::move(e));
	}
	for (size_t i = 0; i < _entries.size(); i++)
		if (!keep.empty() && _entries[i].path == keep) _sel = int(i);
}

- (void)playRecording {
	if (_sel < 0 || size_t(_sel) >= _recRows.size()) return;
	const recplay::Row& r = _recRows[size_t(_sel)];
	[self tryPlayRecording:r.path summary:r.read ? &r.summary : nullptr];
}

- (void)tryPlayRecording:(const std::string&)path summary:(const ron::net::PlaybackSummary*)s {
	if (!s) {
		[self showError:@"The header of this recording is still being read. Try again in a moment."];
		return;
	}
	if (!s->ok) {
		[self showError:[NSString stringWithFormat:@"This file cannot be read as a recording: %s", s->error.c_str()]];
		return;
	}
	if (!s->playable) {
		[self showError:[NSString stringWithFormat:@"This recording cannot be watched: %s.", s->unplayable.c_str()]];
		return;
	}
	[self click];
	if ([self.host respondsToSelector:@selector(frontEndPlayRecording:)]) [self.host frontEndPlayRecording:@(path.c_str())];
}

// Open File...: any .rcx file (RON_REC_PICK=PATH answers the chooser in tests).
- (void)openRecordingFile {
	NSURL* url = nil;
	if (const char* pick = getenv("RON_REC_PICK")) {
		url = [NSURL fileURLWithPath:@(pick)];
	} else {
		NSOpenPanel* panel = [NSOpenPanel openPanel];
		panel.title = @"Choose a recorded game";
		panel.directoryURL = [NSURL fileURLWithPath:@(_recDir.c_str())];
		if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
		url = panel.URL;
	}
	if (![url.pathExtension.lowercaseString isEqualToString:@"rcx"]) {
		[self showError:@"A recorded game is a file that ends in .rcx."];
		return;
	}
	ron::net::PlaybackSummary s;
	ron::net::playback_summary(url.path.UTF8String, s);
	[self tryPlayRecording:url.path.UTF8String summary:&s];
}

- (NSURL*)mpPickFile:(NSString*)title directory:(NSString*)dir {
	if (const char* pick = getenv("RON_MP_PICK")) return [NSURL fileURLWithPath:@(pick)];
	NSOpenPanel* panel = [NSOpenPanel openPanel];
	panel.title = title;
	panel.directoryURL = [NSURL fileURLWithPath:dir];
	if ([panel runModal] != NSModalResponseOK || !panel.URL) return nil;
	return panel.URL;
}

// The host picks a scenario: its players become the seats (humans first), the rules are the scenario's.
- (void)mpChooseScenario {
	if (_mpScenOn) {  // a second click goes back to a random map
		_mpScenOn = NO;
		_room->lobby().set("scenario_game", "");
		_mpLines.push_back({1, "Back to a random map: the setup is yours again."});
		_mpPublished.clear();
		return;
	}
	NSURL* url = [self mpPickFile:@"Choose a scenario for the room" directory:[_dir stringByAppendingPathComponent:@"scenario"]];
	if (!url) return;
	if (![url.pathExtension.lowercaseString isEqualToString:@"scx"]) {
		_mpLines.push_back({1, "A scenario file ends in .scx."});
		return;
	}
	[self mpUseScenario:url.path.UTF8String];
}

- (BOOL)mpUseScenario:(const std::string&)path {
	mp::ScenarioInfo si;
	std::string err;
	if (!mp::load_scenario_info(_dir.UTF8String, path, si, err)) {
		_mpLines.push_back({1, "That scenario cannot be played in multiplayer: " + err});
		return NO;
	}
	_mpScen = si;
	_mpScenOn = YES;
	_setup.num_nations = si.seats;
	for (int k = 0; k < 8; k++) {
		ron::sim::NationSetup& ns = _setup.nation[size_t(k)];
		ns.tribe = k < si.seats ? si.tribe[size_t(k)] : -1;
		ns.color = k < si.seats ? si.color[size_t(k)] : -1;
		ns.difficulty = si.difficulty[size_t(k)];
		ns.team = -1;
	}
	_room->publish_scenario(si);
	_mpLines.push_back({1, "Scenario: " + si.title + ", " + std::to_string(si.seats) + " seats. The scenario's players are the seats; the rules are the scenario's."});
	_mpPublished.clear();
	return YES;
}

// The host opens the campaign screens from the lobby: the battle it plays from the map is played by the whole room.
- (void)mpToggleCtw {
	if (_mpCtwOn) {
		_mpCtwOn = NO;
		_room->lobby().set("ctw_pending", "");
		_mpLines.push_back({1, "Back to a random map: the setup is yours again."});
		return;
	}
	if (_room->humans() < 2) {
		_mpLines.push_back({1, "Wait for a second player first: the campaign's battles are played by the whole room."});
		return;
	}
	_mpCtwOn = YES;
	_room->lobby().set("ctw_pending", 1);
	_mpLines.push_back({1, "Choose or load a campaign, then play a battle from its map: every player in the room takes a seat in it."});
	[self showScreen:@"ctw"];
}

// The Play button of the campaign map, with the room open: the battle goes to every machine.
- (void)mpStartCtw:(std::shared_ptr<ctwui::Session>)s {
	std::string err;
	if (!_room->guests_ready()) {
		s->battle_pending = false;
		_screen = kMpLobby;
		_mpLines.push_back({1, "Every player must be ready before the battle can start."});
		return;
	}
	if (!mp::ctw_host_start(*_room, *s, err)) {
		s->battle_pending = false;
		_screen = kMpLobby;
		_hover = _pressed = kNone;
		_mpLines.push_back({1, "The battle cannot start: " + err + "."});
		return;
	}
	_mpStarting = YES;
	[self.host frontEndPlayMultiplayerCtw:_room session:s];
}

// Back from a multiplayer game: see frontend.h.
- (void)mpResume:(std::shared_ptr<mp::Room>)room ctw:(BOOL)ctw message:(NSString*)message {
	_room = room;
	room->leave_game();
	if (ctw) {
		[self showScreen:@"ctw-map"];
		_mpCtwOn = YES;
		_mpStarting = NO;
		_mpLines.clear();
		if (message.length) _mpLines.push_back({1, message.UTF8String});
		return;
	}
	[self showScreen:@"mp-lobby"];
	if (message.length) _mpLines.push_back({1, message.UTF8String});
}

// The host picks a saved multiplayer game (written by the in-game Save Game): the session continues it. The setup of the save fills the rules;
// the players must take the seats that were human.
- (void)mpLoadFile {
	if (_mpSave) {  // a second click forgets it
		_mpSave.reset();
		_mpSaveText.clear();
		_room->lobby().set("identity", "");
		_mpLines.push_back({1, "The saved game was forgotten; the setup is yours again."});
		return;
	}
	NSURL* url = [self mpPickFile:@"Load a saved multiplayer game" directory:@(mp::default_mp_save_dir().c_str())];
	if (!url) return;
	NSData* data = [NSData dataWithContentsOfURL:url];
	if (!data.length) {
		_mpLines.push_back({1, "That file could not be read."});
		return;
	}
	auto bytes = std::make_shared<std::vector<uint8_t>>(static_cast<const uint8_t*>(data.bytes), static_cast<const uint8_t*>(data.bytes) + data.length);
	try {
		const ron::sim::SaveInfo info = ron::sim::read_save_info(*bytes);
		ron::sim::RandomGameSetup s;
		if (!ron::sim::parse_random_setup(info.scenario, s)) {
			_mpLines.push_back({1, "Only saved random-map games can be continued in multiplayer."});
			return;
		}
		s.seed = info.seed;
		_setup = s;
		_mpSave = bytes;
		_room->lobby().set("identity", 1);   // the players keep the seats they connected in
		int humans = 0;
		for (int i = 0; i < s.num_nations && i < 8; i++) humans += s.nation[size_t(i)].human;
		_mpSaveText = std::string(url.lastPathComponent.UTF8String) + ", frame " + std::to_string(info.frame);
		_mpLines.push_back({1, "Saved game " + _mpSaveText + ": it needs " + std::to_string(humans) + " players in its human seats."});
		_mpPublished.clear();
	} catch (const std::exception& e) {
		_mpLines.push_back({1, std::string("That is not a saved game of this version: ") + e.what()});
	}
}

// ---- the connect screen

- (void)drawField:(const IfaceRect&)r text:(const std::string&)text hint:(const char*)hint focused:(BOOL)focused {
	const IfaceRect D = r.offset(_ox, _oy);
	if (_combo[0].valid()) _ui.expander(_combo[0], _combo[1], _combo[2], D);
	else [self thinBox:r fill:rgb(240, 240, 240)];
	_ui.next_layer();
	const int fo = _ui.font(12, 1);
	const ron::FontMetrics* om = _ui.font_metrics(fo);
	const int oh = om ? om->font_height : 14;
	std::string shown = text;
	const int room = r.w() - 14;
	while (shown.size() > 1 && _ui.text_width(fo, shown) > room) shown.erase(0, 1);  // the end of a long text stays visible
	if (focused && ((int)(_timeMs / 500) & 1) == 0) shown += "|";
	const bool empty = text.empty() && !focused;
	_ui.text(fo, empty ? std::string(hint) : shown, float(D.l + 7), float(D.t + (D.h() - oh) / 2), empty ? rgb(130, 130, 130) : kBlack);
}

- (void)drawConnect:(const Layout&)L {
	[self banner:L text:kListTitles[kMpConnect]];
	_ui.next_window();
	const int f14 = _ui.font(14, -1), f12 = _ui.font(12, -1), f16 = _ui.font(16, -1);
	const ron::FontMetrics* m12 = _ui.font_metrics(f12);
	const int lh12 = m12 ? m12->line_height : 14;
	_lan.poll(ron::net::lan_now_ms());
	[self thinBox:{200, 824, 100, 628} fill:argb(120, 0, 0, 0)];
	_ui.next_layer();
	[self textAt:f14 text:"Your name" x:232 y:124 color:kLabel];
	_mpRect[0] = {400, 780, 120, 144};
	[self drawField:_mpRect[0] text:_mpName hint:"Player" focused:_mpField == 0];
	_ui.next_layer();
	[self textAt:f16 text:"Host a game" x:232 y:162 color:kWhite];
	[self textAt:f14 text:"Port" x:232 y:196 color:kLabel];
	_mpRect[1] = {400, 520, 192, 216};
	[self drawField:_mpRect[1] text:_mpPort hint:"7777" focused:_mpField == 1];
	_ui.next_layer();
	_mpHostBtn = {592, 780, 188, 220};
	_mpJoinBtn = {652, 780, 484, 516};
	[self popupButton:_mpHostBtn label:"Host" state:(_pressed == kMpHost ? 2 : _hover == kMpHost ? 1 : 0) font:f14];
	int y = 224;
	for (const std::string& line : wrap_text(_ui, f12, "You become the host and open a room on this port. Others on your network see it in their list.", 560)) {
		[self textAt:f12 text:line x:232 y:float(y) color:kSecondary];
		y += lh12;
	}
	// The games on the network
	_ui.next_layer();
	[self textAt:f16 text:"Games on your network" x:232 y:262 color:kWhite];
	_lanBox = {232, 792, 288, 424};
	[self thinBox:_lanBox fill:argb(110, 0, 0, 0)];
	_ui.next_layer();
	[self textAt:f12 text:"Host" x:244 y:290 color:kSecondary];
	[self textAt:f12 text:"Map" x:356 y:290 color:kSecondary];
	[self textAt:f12 text:"Players" x:690 y:290 color:kSecondary];
	const std::vector<ron::net::LanGame>& games = _lan.games();
	for (int i = 0; i < 6; i++) _lanRect[i] = IfaceRect{0, 0, 0, 0};
	if (games.empty()) {
		[self textAt:f12 text:_lan.is_open() ? "No games found yet. A host's room appears here a moment after it opens." : "Searching the network is not available; type the host's address below." x:244 y:312 color:rgb(160, 160, 160)];
	}
	for (int i = 0; i < 6 && i < int(games.size()); i++) {
		const ron::net::LanGame& g = games[size_t(i)];
		const int ry = 306 + i * 19;
		_lanRect[i] = {236, 788, ry, ry + 19};
		if (_lanSel == i || _hover == kMpList + i) {
			IfacePiece p[3] = {_large[0], _large[1], _large[2]};
			if (_lanSel != i)
				for (IfacePiece& q : p) q.top += q.height + 1;
			if (p[0].valid()) _ui.expander(p[0], p[1], p[2], IfaceRect{_lanRect[i].l, _lanRect[i].r, ry + 1, ry + 18}.offset(_ox, _oy));
		}
		_ui.next_layer();
		const uint32_t col = g.compatible() ? kWhite : rgb(130, 130, 130);
		std::string nm = g.name, mp_ = g.map + (g.flags & ron::net::kLanScenario ? " (scenario)" : g.flags & ron::net::kLanSaved ? " (saved game)" : g.flags & ron::net::kLanCtw ? " (campaign)" : "");
		if (!g.compatible()) mp_ = "another version of the game";
		while (nm.size() > 3 && _ui.text_width(f12, nm) > 104) nm.pop_back();
		while (mp_.size() > 3 && _ui.text_width(f12, mp_) > 320) mp_.pop_back();
		[self textAt:f12 text:nm x:244 y:float(ry + 3) color:col];
		[self textAt:f12 text:mp_ x:356 y:float(ry + 3) color:col];
		[self textAt:f12 text:std::to_string(g.players) + " / " + std::to_string(g.seats) x:690 y:float(ry + 3) color:col];
		[self textAt:f12 text:g.address x:728 y:float(ry + 3) color:rgb(150, 150, 150)];
		_ui.next_layer();
	}
	_ui.next_layer();
	[self textAt:f16 text:"Join a game" x:232 y:436 color:kWhite];
	[self textAt:f14 text:"Host address" x:232 y:462 color:kLabel];
	_mpRect[2] = {400, 640, 460, 484};
	[self drawField:_mpRect[2] text:_mpAddr hint:"192.168.0.12:7777" focused:_mpField == 2];
	_ui.next_layer();
	[self popupButton:_mpJoinBtn label:"Join" state:(_pressed == kMpJoin ? 2 : _hover == kMpJoin ? 1 : 0) font:f14];
	y = 496;
	for (const std::string& line : wrap_text(_ui, f12, "Click a game above (twice to join), or type the host's address and port, for example 192.168.0.12:7777; without a port, 7777 is used.", 400)) {
		[self textAt:f12 text:line x:232 y:float(y) color:kSecondary];
		y += lh12;
	}
	if (!_mpMessage.empty()) {
		y = 548;
		for (const std::string& line : wrap_text(_ui, f14, _mpMessage, 560)) {
			[self textAt:f14 text:line x:232 y:float(y) color:rgb(255, 170, 120)];
			y += lh12 + 2;
		}
	}
	y = 584;
	for (const std::string& line : wrap_text(_ui, f12, "Everyone needs the same version of the game and the same game files. The game runs in lockstep: all commands reach every machine and the "
	                                               "game waits for the slowest connection.", 560)) {
		[self textAt:f12 text:line x:232 y:float(y) color:kSecondary];
		y += lh12;
	}
	_ui.next_window();
	[self popupButton:L.back label:"Back" state:(_pressed == kBack ? 2 : _hover == kBack ? 1 : 0) font:_ui.font(14, -1)];
}

- (int)hitConnect:(int)x y:(int)y {
	const Layout L = [self layout];
	if (L.back.contains(x, y)) return kBack;
	if (_mpHostBtn.contains(x, y)) return kMpHost;
	if (_mpJoinBtn.contains(x, y)) return kMpJoin;
	for (int i = 0; i < 3; i++)
		if (_mpRect[i].contains(x, y)) return kMpName + i;
	for (int i = 0; i < 6; i++)
		if (_lanRect[i].contains(x, y)) return kMpList + i;
	return kNone;
}

// Text keys on the connect screen and in the chat field: returns YES when `e` was a text edit.
- (BOOL)editText:(std::string&)text event:(NSEvent*)e limit:(size_t)limit {
	NSString* chars = e.characters;
	if (e.keyCode == 51) {  // Delete
		while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
		if (!text.empty()) text.pop_back();
		return YES;
	}
	if ((e.modifierFlags & NSEventModifierFlagCommand) && chars.length && [chars.lowercaseString isEqualToString:@"v"]) {
		NSString* clip = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
		for (NSUInteger i = 0; i < clip.length && text.size() < limit; i++) {
			const unichar c = [clip characterAtIndex:i];
			if (c >= 32 && c < 127) text += char(c);
		}
		return YES;
	}
	if (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) return NO;
	const std::string add = chars.UTF8String ? chars.UTF8String : "";
	if (add.empty() || static_cast<unsigned char>(add[0]) < 32 || add[0] == 127 || (e.keyCode >= 123 && e.keyCode <= 126)) return NO;
	if (text.size() + add.size() <= limit) text += add;
	return YES;
}

- (void)connectKey:(NSEvent*)e {
	NSString* chars = e.charactersIgnoringModifiers;
	const unichar c = chars.length ? [chars characterAtIndex:0] : 0;
	const bool ret = c == '\r' || c == 3, esc = e.keyCode == 53, tab = e.keyCode == 48;
	if (esc) {
		[self click];
		[self showScreen:@"main"];
		_focus = 4;
		return;
	}
	if (tab) {
		_mpField = (_mpField + 1) % 3;
		return;
	}
	if (c == NSUpArrowFunctionKey || c == NSDownArrowFunctionKey) {
		const int n = std::min<int>(6, int(_lan.games().size()));
		if (n > 0) {
			_lanSel = std::clamp(_lanSel + (c == NSDownArrowFunctionKey ? 1 : -1), 0, n - 1);
			const ron::net::LanGame& g = _lan.games()[size_t(_lanSel)];
			if (g.compatible()) _mpAddr = g.address + ":" + std::to_string(g.port);
		}
		return;
	}
	if (ret) {
		if (_mpField == 2 || (_lanSel >= 0 && _mpField != 1)) [self mpJoin];
		else [self mpHost];
		return;
	}
	std::string* target = _mpField == 0 ? &_mpName : _mpField == 1 ? &_mpPort : &_mpAddr;
	if (_mpField < 0) return;
	if ([self editText:*target event:e limit:_mpField == 0 ? 24 : _mpField == 1 ? 5 : 64]) _mpMessage.clear();
}

// ---- the lobby screen

- (int)hitLobby:(int)x y:(int)y {
	if (_openCombo < 0) {
		if (_mpChatRect.contains(x, y)) return kMpChat;
		if (_mpLoadRect.contains(x, y)) return kMpLoad;
		if (_mpScenRect.contains(x, y)) return kMpScenario;
		if (_mpCtwRect.contains(x, y)) return kMpCtw;
		if (_mpObsRect.contains(x, y)) return kMpObserver;
		if (_mpApprovalRect.contains(x, y)) return kMpApproval;
		if (_mpAllowRect.contains(x, y)) return kMpAllow;
		if (_mpDenyRect.contains(x, y)) return kMpDeny;
		for (int p = 0; p < 8; p++)
			if (_mpKickRect[size_t(p)].contains(x, y)) return kMpKick + p;
	}
	return [self hitSetup:x y:y];
}

// A short button in the lobby's right panel (12 pt).
- (void)lobbyButton:(const IfaceRect&)r label:(const std::string&)label id:(int)hid enabled:(BOOL)enabled {
	if (r.w() <= 0) return;
	_ui.next_layer();
	if (!enabled) {
		const IfacePiece* p = _popBtn[0];
		const IfaceRect D = r.offset(_ox, _oy);
		if (p[0].valid() && p[1].valid() && p[2].valid()) _ui.expander(p[0], p[1], p[2], D);
		_ui.next_layer();
		_ui.text_centered(_ui.font(12, -1), label, D, rgb(150, 150, 150), kBlack);
		return;
	}
	[self popupButton:r label:label state:(_pressed == hid ? 2 : _hover == hid ? 1 : 0) font:_ui.font(12, -1)];
}

- (void)drawLobby:(const Layout&)L {
	[self banner:L text:kListTitles[kMpLobby]];
	[self layoutSetup];
	const int f14 = _ui.font(14, -1), f12 = _ui.font(12, -1);
	const ron::FontMetrics* m14 = _ui.font_metrics(f14);
	const int lh = m14 ? m14->line_height : 16;
	const BOOL host = _room && _room->is_host();
	_ui.next_window();
	const IfaceRect players{20, 566, 88, 628}, rules{578, 1004, 88, 628};
	[self thinBox:players fill:argb(120, 0, 0, 0)];
	[self thinBox:rules fill:argb(120, 0, 0, 0)];
	_ui.next_layer();
	const struct {
		int x;
		const char* t;
	} cols[6] = {{36, "Player"}, {112, "Nation"}, {216, "Level"}, {304, "Team"}, {374, "Colour"}, {454, "Handicap"}};
	for (const auto& c : cols) [self textAt:f14 text:c.t x:float(c.x) y:104 color:kSecondary];
	_ui.next_layer();
	for (const auto& sw : _mpSwatch) {
		const uint32_t col = mp::seat_text_color(sw[2]);
		_ui.fill(float(sw[0] + _ox), float(sw[1] + _oy), 9.0f, 14.0f, col, UiType::ColorBlend);
	}
	_ui.next_layer();
	for (const SetupText& t : _st) {
		const uint32_t col = t.kind == 1 ? rgb(130, 130, 130) : t.kind == 2 ? kWhite : t.kind == 3 || t.kind == 5 ? rgb(150, 255, 150) : t.kind == 4 || t.kind == 6 ? rgb(190, 190, 190) : kLabel;
		[self textAt:t.kind >= 5 ? f12 : f14 text:t.text x:float(t.x) y:float(t.y) color:col];   // kinds 5 and 6: the small second line of a seat row
	}
	for (const SetupCombo& c : _sc)
		if (!c.label.empty()) [self textAt:f14 text:c.label x:float(c.lx) y:float(c.ly + 2) color:kLabel];
	// The host's Kick buttons under the guests' names
	for (int p = 0; p < 8; p++) {
		const IfaceRect& k = _mpKickRect[size_t(p)];
		if (k.w() <= 0) continue;
		const bool hot = _hover == kMpKick + p;
		[self textAt:_ui.font(12, -1) text:"Kick" x:float(k.l) y:float(k.t) color:hot ? rgb(255, 150, 120) : rgb(190, 120, 100)];
	}
	// The chat: the log above the input line
	{
		_ui.next_layer();
		const IfaceRect chat{28, 558, 446, 622};
		[self thinBox:chat fill:argb(110, 0, 0, 0)];
		_ui.next_layer();
		_mpLogRect = {36, 550, 450, 592};
		_mpChatRect = {36, 550, 596, 618};
		struct Part {
			const MpLine* ln;
			std::string text;
			BOOL first;
		};
		std::vector<Part> shown;
		for (const MpLine& ln : _mpLines) {
			BOOL first = YES;
			for (const std::string& part : wrap_text(_ui, f12, ln.prefix + ln.text, _mpLogRect.w())) {
				shown.push_back({&ln, part, first});
				first = NO;
			}
		}
		const int lh12 = _ui.font_metrics(f12) ? _ui.font_metrics(f12)->line_height : 14;
		const int fit = std::max(1, _mpLogRect.h() / lh12);
		int y = _mpLogRect.t;
		for (size_t i = shown.size() > size_t(fit) ? shown.size() - size_t(fit) : 0; i < shown.size(); i++) {
			const MpLine& ln = *shown[i].ln;
			const uint32_t base = ln.kind == 1 ? rgb(255, 224, 140) : ln.kind == 2 ? rgb(150, 255, 150) : ln.kind == 3 ? rgb(255, 170, 230) : kWhite;
			if (shown[i].first && !ln.prefix.empty() && shown[i].text.compare(0, ln.prefix.size(), ln.prefix) == 0) {
				[self textAt:f12 text:ln.prefix x:float(_mpLogRect.l) y:float(y) color:ln.color];
				[self textAt:f12 text:shown[i].text.substr(ln.prefix.size()) x:float(_mpLogRect.l + _ui.text_width(f12, ln.prefix)) y:float(y) color:base];
			} else {
				[self textAt:f12 text:shown[i].text x:float(_mpLogRect.l) y:float(y) color:base];
			}
			y += lh12;
		}
		_ui.next_layer();
		[self drawField:_mpChatRect text:_mpChat hint:"Type to chat. /t team, /w name private" focused:_mpField == 3];
	}
	// The room, on the rules panel under the combos
	{
		_ui.next_layer();
		int y = _rulesBottom - 22;
		const ron::net::SeatPlan pl = _room ? _room->plan() : ron::net::SeatPlan();
		std::string where;
		if (host && _room) {
			const std::vector<std::string> addrs = _room->lan_addresses();
			where = "Hosting " + (addrs.empty() ? std::string("on port ") : addrs[0] + ":") + std::to_string(_room->port());
		} else {
			where = "Joined " + (_room ? _room->address() : std::string());
		}
		std::string counts = where + " - " + std::to_string(pl.players) + (pl.players == 1 ? " player" : " players");
		if (pl.observers) counts += ", " + std::to_string(pl.observers) + (pl.observers == 1 ? " observer" : " observers");
		while (counts.size() > 8 && _ui.text_width(f12, counts) > 400) counts.pop_back();
		[self textAt:f12 text:counts x:592 y:float(y) color:kWhite];
		y += lh;
		if (pl.observers && _room) {
			std::string watching = "Watching: ";
			for (int s = pl.nations; s < 8; s++)
				if (pl.peer_of[size_t(s)] >= 0) watching += (watching.size() > 10 ? ", " : "") + _room->name_of(pl.peer_of[size_t(s)]);
			while (watching.size() > 8 && _ui.text_width(f12, watching) > 400) watching.pop_back();
			[self textAt:f12 text:watching x:592 y:float(y) color:kSecondary];
			y += lh;
		}
		std::string note;
		const BOOL fromSave = _mpSave || (_room && _room->lobby().has("from_save"));
		if (fromSave) note = "Continuing a saved game" + (_mpSaveText.empty() ? std::string() : ": " + _mpSaveText) + ".";
		else if (_mpScenOn) note = "Scenario: " + _mpScen.title;
		else if (_mpCtwOn) note = "Conquer the World: " + (_mpCtwText.empty() ? std::string("choose a battle on the campaign map") : _mpCtwText);
		if (!note.empty()) {
			while (note.size() > 8 && _ui.text_width(f12, note) > 400) note.pop_back();
			[self textAt:f12 text:note x:592 y:float(y) color:rgb(255, 224, 140)];
		}
		// row A: watch only / play, and who decides the guests' requests
		const BOOL imObs = _room && _room->lobby().observer_of(_room->slot());
		_mpObsRect = {592, 794, 540, 564};
		_mpApprovalRect = host ? IfaceRect{802, 1000, 540, 564} : IfaceRect{0, 0, 0, 0};
		const BOOL canObs = _room && (imObs || _room->max_observers() > pl.observers);
		[self lobbyButton:_mpObsRect label:imObs ? "Play instead" : "Watch only" id:kMpObserver enabled:canObs && !_mpStarting];
		if (host) [self lobbyButton:_mpApprovalRect label:_room->approval_ask() ? "Guests' choices: ask me" : "Guests' choices: automatic" id:kMpApproval enabled:YES];
		// row B: a guest's request waiting for the host
		_mpAllowRect = _mpDenyRect = IfaceRect{0, 0, 0, 0};
		if (host && _room && !_room->lobby().pending().empty()) {
			const ron::net::SeatRequest& rq = _room->lobby().pending().front();
			std::string what = _room->name_of(rq.peer) + " asks for " + [self requestText:rq];
			while (what.size() > 8 && _ui.text_width(f12, what) > 270) what.pop_back();
			[self textAt:f12 text:what x:592 y:572 color:rgb(255, 224, 140)];
			_mpAllowRect = {870, 930, 570, 590};
			_mpDenyRect = {936, 996, 570, 590};
			[self lobbyButton:_mpAllowRect label:"Allow" id:kMpAllow enabled:YES];
			[self lobbyButton:_mpDenyRect label:"Deny" id:kMpDeny enabled:YES];
		}
		// row C: what the host can load
		_mpLoadRect = _mpScenRect = _mpCtwRect = IfaceRect{0, 0, 0, 0};
		if (host) {
			_mpLoadRect = {592, 722, 594, 622};
			_mpScenRect = {728, 858, 594, 622};
			_mpCtwRect = {864, 1000, 594, 622};
			[self lobbyButton:_mpLoadRect label:_mpSave ? "Forget Saved Game" : "Load Saved Game..." id:kMpLoad enabled:!_mpScenOn && !_mpCtwOn];
			[self lobbyButton:_mpScenRect label:_mpScenOn ? "Random Map" : "Scenario..." id:kMpScenario enabled:!_mpSave && !_mpCtwOn];
			[self lobbyButton:_mpCtwRect label:_mpCtwOn ? "Cancel Campaign" : "Conquer the World..." id:kMpCtw enabled:!_mpSave && !_mpScenOn];
		}
	}
	// Combos (closed state), then the open list on top
	_ui.next_layer();
	for (size_t i = 0; i < _sc.size(); i++) {
		[self drawCombo:_sc[i] index:int(i)];
		_ui.next_layer();
	}
	if (_openCombo >= 0 && size_t(_openCombo) < _sc.size()) [self drawOpenList];
	else _setupListVis = 0;
	_ui.next_window();
	const int fb = _ui.font(14, -1);
	[self popupButton:L.back label:"Leave" state:(_pressed == kBack ? 2 : _hover == kBack ? 1 : 0) font:fb];
	const bool ready = _room && _room->ready_of(_room->slot());
	const bool can = _room && _room->seat_humans(*std::make_unique<ron::sim::RandomGameSetup>(_setup)) && _room->guests_ready();
	std::string label;
	if (host) {
		if (_room && _room->counting()) label = "Cancel (" + std::to_string(_room->countdown_left()) + ")";
		else label = can ? "Start Game" : (_room && _room->humans() < 2 ? "Waiting for players" : "Waiting for ready");
	} else {
		label = ready ? "Not Ready" : "Ready";
	}
	[self popupButton:L.play label:label state:(_pressed == kPlay ? 2 : _hover == kPlay ? 1 : 0) font:fb];
}

// What a guest's request says, for the host's Allow / Deny line.
- (std::string)requestText:(const ron::net::SeatRequest&)rq {
	switch (rq.field) {
		case ron::net::SeatField::Tribe: return rq.value < 0 ? "a random nation" : "the " + (size_t(rq.value) < _lists.tribes.size() ? _lists.tribes[size_t(rq.value)] : std::string("?"));
		case ron::net::SeatField::Team: return rq.value < 0 ? "no team" : "team " + std::to_string(rq.value + 1);
		case ron::net::SeatField::Color: {
			static const char* names[8] = {"red", "blue", "pink", "green", "yellow", "cyan", "white", "orange"};
			return std::string(names[std::clamp(rq.value, 0, 7)]);
		}
		case ron::net::SeatField::Handicap: return size_t(rq.value) < _lists.handicap.size() ? _lists.handicap[size_t(rq.value)] : std::string("a handicap");
		case ron::net::SeatField::Observer: return rq.value ? "a seat as an observer" : "a place among the players";
	}
	return "a change";
}

// The key handling of the lobby (the setup screen's keys for the open list, text for the chat).
- (void)lobbyKey:(NSEvent*)e {
	NSString* chars = e.charactersIgnoringModifiers;
	const unichar c = chars.length ? [chars characterAtIndex:0] : 0;
	const bool ret = c == '\r' || c == 3, esc = e.keyCode == 53, tab = e.keyCode == 48;
	if (_openCombo >= 0) {
		[self setupKey:e];
		return;
	}
	if (_mpField == 3) {
		if (ret) [self mpSend];
		else if (esc) _mpField = -1;
		else if (tab) _mpField = -1;
		else [self editText:_mpChat event:e limit:200];
		return;
	}
	if (esc) {
		[self click];
		[self mpLeave:nil];
	} else if (ret) {
		[self mpPlay];
	} else if (tab) {
		_mpField = 3;
	} else {
		std::string probe = _mpChat;
		if ([self editText:probe event:e limit:200]) _mpChat = probe, _mpField = 3;  // typing opens the chat
	}
}

- (void)mpDemoStart {
	if (!_room) return;
	for (int i = 0; i < 50 && !_room->guests_ready(); i++) {  // the demo guests ready up one by one
		[self mpPump];
		[NSThread sleepForTimeInterval:0.05];
	}
	[self mpPlay];
	[NSThread sleepForTimeInterval:0.5];  // the guests' threads read START
}

- (BOOL)mpDemoJoin:(int)port name:(NSString*)name pumping:(FrontEnd*)hostFe {
	auto room = std::make_shared<mp::Room>();
	std::string err;
	bool ok = false;
	std::atomic<bool> done{false};
	std::shared_ptr<mp::Room> r = room;
	const std::string nm = name.UTF8String;
	std::thread t([&]() {
		ok = r->join("127.0.0.1", uint16_t(port), nm, 4000, err);
		done = true;
	});
	while (!done) {
		[hostFe mpPump];
		[NSThread sleepForTimeInterval:0.01];
	}
	t.join();
	if (!ok) {
		std::fprintf(stderr, "demo guest: %s\n", err.c_str());
		return NO;
	}
	_room = room;
	[self showScreen:@"mp-lobby"];
	return YES;
}

// The centre of a lobby / connect control in view points, for a scripted click through the real hit boxes (mouseMoved / mouseDown / mouseUp): "host", "join",
// "load", "scenario", "ctw", "start" (the Play button), "back". CGPointZero when the control is not on the screen.
- (CGPoint)mpDemoPoint:(NSString*)which {
	IfaceRect r{0, 0, 0, 0};
	if ([which isEqualToString:@"host"]) r = _mpHostBtn;
	else if ([which isEqualToString:@"join"]) r = _mpJoinBtn;
	else if ([which isEqualToString:@"load"]) r = _mpLoadRect;
	else if ([which isEqualToString:@"scenario"]) r = _mpScenRect;
	else if ([which isEqualToString:@"ctw"]) r = _mpCtwRect;
	else if ([which isEqualToString:@"start"]) r = [self layout].play;
	else if ([which isEqualToString:@"back"]) r = [self layout].back;
	if (r.r <= r.l) return CGPointZero;
	if (getenv("RON_MP_APP_DEBUG")) std::printf("[app test] %s: rect %d,%d,%d,%d ox %d oy %d fit %.3f hit %d (screen %d)\n", which.UTF8String, r.l, r.r, r.t, r.b, _ox, _oy, _fit, [self hit:CGPointMake(float(r.l + r.r) / 2, float(r.t + r.b) / 2)], int(_screen));
	return CGPointMake((float(r.l + r.r) / 2 + _ox) * _fit, (float(r.t + r.b) / 2 + _oy) * _fit);
}

// The Play button of the campaign map, as the screens' PlayBattle action is consumed (ctwTakeAction): with the room open the battle goes to every machine.
- (void)mpDemoCtwPlay {
	std::shared_ptr<ctwui::Session> s = _ctw ? _ctw->session() : nullptr;
	if (s && _room && _mpCtwOn && _room->is_host()) [self mpStartCtw:s];
}

- (NSString*)mpDemoState {
	NSString* scr = _screen == kMpLobby ? @"mp-lobby" : _screen == kMpConnect ? @"multiplayer" : _screen == kCtwMap ? @"ctw-map" : _screen == kCtw ? @"ctw" : @"other";
	std::string lines;   // the lobby's last chat lines (the host's notes to itself too)
	for (size_t k = _mpLines.size() > 3 ? _mpLines.size() - 3 : 0; k < _mpLines.size(); k++) lines += " | " + _mpLines[k].text;
	return [NSString stringWithFormat:@"screen=%@ room=%d humans=%d counting=%d save='%s' ctw=%d scen=%d message='%s' lines:%s", scr, _room ? 1 : 0, _room ? _room->humans() : 0,
	                                  _room && _room->counting() ? _room->countdown_left() : 0, _mpSaveText.c_str(), int(_mpCtwOn), int(_mpScenOn), _mpMessage.c_str(), lines.c_str()];
}

- (BOOL)mpDemoPort:(int)port guests:(NSArray<NSString*>*)guests {
	auto room = std::make_shared<mp::Room>();
	std::string err;
	if (!room->host(uint16_t(port), "Host", err)) {
		std::fprintf(stderr, "demo room: %s\n", err.c_str());
		return NO;
	}
	_room = room;
	std::vector<std::string> names, lines;
	for (NSString* g in guests) names.push_back(g.UTF8String);
	lines = {"Hi all, ready when you are.", "Is the map big enough for four?", "Give me the Nile map."};
	mp::spawn_demo_guests(uint16_t(port), names, lines, getenv("RON_MP_DEMO_START") ? 0b111 : 0b101);
	[self showScreen:@"mp-lobby"];
	if (getenv("RON_MP_DEMO_ASK")) _room->set_approval_ask(true);   // snapshot: a guest's request waiting for the host
	for (int i = 0; i < 80; i++) {  // up to 4 s for the guests to come in and talk
		[self mpPump];
		if (_room->humans() > int(names.size()) && _mpLines.size() >= names.size() + 1) break;
		[NSThread sleepForTimeInterval:0.05];
	}
	[NSThread sleepForTimeInterval:0.3];
	[self mpPump];
	for (int i = 0; i < 20 && getenv("RON_MP_DEMO_REQ"); i++) {  // the guests' requests are answered
		[NSThread sleepForTimeInterval:0.05];
		[self mpPump];
	}
	if (getenv("RON_MP_DEMO_SCEN")) [self mpUseScenario:getenv("RON_MP_DEMO_SCEN")];
	if (getenv("RON_MP_DEMO_COUNTDOWN")) _room->begin_countdown(7);
	return _room->humans() > 1;
}

// ---------------------------------------------------------------- Conquer the World (ctw_ui.h)

// Opens the campaign screens: the list / set-up, or the strategic map of the session the host holds (a campaign in progress
// comes back here after a battle).
- (void)ctwOpen:(BOOL)map {
	if (!_ctw) _ctw = std::make_unique<ctwui::Screen>(std::make_shared<ron::GameFiles>(_dir.UTF8String), _dir.UTF8String);
	std::shared_ptr<ctwui::Session> session = [self.host respondsToSelector:@selector(frontEndCtwSession)] ? [self.host frontEndCtwSession] : nullptr;
	if (session && !_ctw->has_session()) _ctw->set_session(session);  // a campaign in progress comes back after a battle
	_screen = map && _ctw->has_session() ? kCtwMap : kCtw;
	_ctw->open(_screen == kCtwMap, _ctwSub);
	_ctwSub.clear();
}

// Headless: the screens of a campaign in progress (--ctw ... --snapshot): `sub` is "", "news", "cards", "diplomacy", "attack", "menu".
- (void)ctwShowSession:(std::shared_ptr<ctwui::Session>)session sub:(NSString*)sub select:(NSString*)country {
	if (!_ctw) _ctw = std::make_unique<ctwui::Screen>(std::make_shared<ron::GameFiles>(_dir.UTF8String), _dir.UTF8String);
	_ctw->set_session(session);
	_ctwSub = sub ? sub.UTF8String : "";
	_screen = kCtwMap;
	_ctw->open(true, _ctwSub);
	if (country.length) _ctw->select_country(country.UTF8String);
}

- (void)ctwBridge {
	_ctwb.ui = &_ui;
	_ctwb.ox = _ox, _ctwb.oy = _oy, _ctwb.ui_w = _uiW, _ctwb.ui_h = _uiH;
	_ctwb.device = _device;
	_ctwb.queue = _queue;
	if (_ctwb.button) return;
	__unsafe_unretained FrontEnd* me = self;
	_ctwb.button = [me](const IfaceRect& r, const std::string& label, int state, int font) {
		const IfaceRect c = r.offset(-me->_ox, -me->_oy);
		const int f = font;
		if (state == 3) {  // disabled: the normal button with grey text
			const IfaceRect R = r;
			const IfacePiece* p = me->_popBtn[0];
			if (p[0].valid() && p[1].valid() && p[2].valid()) me->_ui.expander(p[0], p[1], p[2], R);
			me->_ui.next_layer();
			me->_ui.text_centered(f, label, R, rgb(150, 150, 150), kBlack);
			return;
		}
		[me popupButton:c label:label state:state font:f];
	};
	_ctwb.box = [me](const IfaceRect& r, uint32_t fill) { [me thinBox:r.offset(-me->_ox, -me->_oy) fill:fill]; };
	_ctwb.frame = [me](const IfaceRect& r) { me->_ui.border(me->_border, r); };
	_ctwb.row = [me](const IfaceRect& R, bool selected, bool hot) {
		if (!selected && !hot) return;
		IfacePiece p[3] = {me->_large[0], me->_large[1], me->_large[2]};
		if (!selected)
			for (IfacePiece& q : p) q.top += q.height + 1;
		if (p[0].valid()) me->_ui.expander(p[0], p[1], p[2], {R.l, R.r, R.t + 1, R.b - 1});
	};
	_ctwb.banner = [me](const std::string& title) {
		const Layout L = [me layout];
		[me banner:L text:title.c_str()];
	};
	_ctwb.click = [me]() { [me click]; };
}

- (void)ctwDraw {
	if (!_ctw) return;
	[self ctwBridge];
	_ctw->draw(_ctwb);
	[self ctwTakeAction];
}

// What the screens asked for: back to the main menu, or play the battle they prepared.
- (void)ctwTakeAction {
	if (!_ctw) return;
	switch (_ctw->take_action()) {
		case ctwui::Action::Back:
			if (_room && _mpCtwOn) {  // the campaign was opened from the multiplayer lobby: back to the room
				_screen = kMpLobby;
				_hover = _pressed = kNone;
				_mpField = -1;
				break;
			}
			[self showScreen:@"main"];
			_focus = 3;
			break;
		case ctwui::Action::PlayBattle: {
			std::shared_ptr<ctwui::Session> s = _ctw->session();
			if (s && _room && _mpCtwOn && _room->is_host()) {
				[self mpStartCtw:s];
				break;
			}
			if (s && [self.host respondsToSelector:@selector(frontEndPlayCtwBattle:)]) [self.host frontEndPlayCtwBattle:s];
			break;
		}
		default: break;
	}
}

@end
