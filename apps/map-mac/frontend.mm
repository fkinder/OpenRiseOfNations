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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/cursors.hpp"
#include "assets/iface_layout.hpp"
#include "assets/tga.hpp"
#include "assets/xml.hpp"
#include "audio.hpp"
#include "renderer.hpp"
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
const MainItem kMainItems[6] = {
    {"Tutorials", "Learn to play, step by step. Start with Tutorial 1.", true},
    {"Skirmish", "Battle computer opponents on a ready-made map, or set up a random map.", true},
    {"Scenarios", "Chess exercises and the battle maps of the Conquer the World campaigns.", true},
    {"Conquer the World", "Lead a nation through a campaign: move armies on the world map, make deals, play the battles.", true},
    {"Multiplayer", "Play with friends on a local network: one player hosts a room, the others join it by address.", true},
    {"Quit", "Leave Rise of Nations and return to the desktop.", true},
};

enum Screen { kMain = 0, kTutorials, kSkirmish, kScenarios, kSkirmishSetup, kCtw, kCtwMap, kMpConnect, kMpLobby };
const char* const kScreenNames[9] = {"main", "tutorials", "skirmish", "scenarios", "skirmish-setup", "ctw", "ctw-map", "multiplayer", "mp-lobby"};
const char* const kListTitles[9] = {"Main Menu", "Tutorials", "Skirmish", "Scenarios", "Skirmish Setup", "Conquer the World", "Conquer the World", "Multiplayer", "Multiplayer Lobby"};
const char* const kListNotes[9] = {
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
	std::vector<std::string> style, size, town, resources, start, end, victory, elim, pop, reveal, diff, tribes;
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
	std::vector<std::pair<int, std::string>> _mpLines;  // the lobby's log: (0 chat | 1 system, text)
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
	for (int i = 0; i < 9; i++) {
		if ([name isEqualToString:@(kScreenNames[i])]) s = Screen(i);
	}
	_screen = s;
	_sel = 0, _top0 = 0, _focus = 0;
	_entries.clear();
	if (s == kCtw || s == kCtwMap) {
		[self ctwOpen:s == kCtwMap];
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
		if (!intros.empty() && !getenv("RON_NO_MUSIC")) {
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
		for (int i = 0; i < 6; i++) {
			const IfaceRect r = _btnRect.offset(wx, wy + i * _btnInc);
			L.buttons.push_back(r);
			bottom = r.b;
		}
		L.frame = {wx + _btnRect.l - 41, wx + _btnRect.r + 41, L.title.t + _titleRect.h() - _titleRect.t + _titleRect.t, bottom + 41};
		L.frame.t = wy + _titleRect.h();  // client top + title height (TopMenuSelection::on_redraw)
		L.info = {44, 580, 452, 532};
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
	if (_screen != kMain || _anims.empty()) {
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
	if (_anims.empty() || _screen != kMain) return;
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
	for (int i = 0; i < 6; i++) {
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
	if (n == 0) {
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
		_ui.text(f12, e.detail, float(R.l + 14), float(R.t + 23), i == _sel ? rgb(225, 225, 235) : kSecondary, kBlack);
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
	if (_screen != kTutorials) {
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
	[self popupButton:L.play label:"Play" state:(_pressed == kPlay ? 2 : _hover == kPlay ? 1 : 0) font:fb];
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
	id<MTLTexture> out = rm::make_readback_target(_device, w, h);
	if (scale <= 0) scale = 1;
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
		for (int i = 0; i < 6; i++) {
			if (L.buttons[size_t(i)].contains(x, y) && kMainItems[i].enabled) return kMainBtn + i;
		}
		if (L.change.contains(x, y)) return kChange;
		return kNone;
	}
	if (_screen == kSkirmishSetup) return [self hitSetup:x y:y];
	if (_screen == kMpLobby) return [self hitLobby:x y:y];
	if (_screen == kMpConnect) return [self hitConnect:x y:y];
	if (L.back.contains(x, y)) return kBack;
	if (L.play.contains(x, y)) return kPlay;
	if (_screen == kSkirmish && L.rand.contains(x, y)) return kRandomMap;
	if (_screen != kTutorials) {
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
	if (_screen == kMain && h >= kMainBtn && h < kMainBtn + 6) {
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
	if (h >= kMainBtn && h < kMainBtn + 6) {
		[self click];
		switch (h - kMainBtn) {
			case 0: [self showScreen:@"tutorials"]; break;
			case 1: [self showScreen:@"skirmish"]; break;
			case 2: [self showScreen:@"scenarios"]; break;
			case 3: [self showScreen:@"ctw"]; break;
			case 4: [self showScreen:@"multiplayer"]; break;
			case 5: [self openQuitPopup]; break;
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
	} else if (h == kBack && _screen == kSkirmishSetup) {
		[self click];
		[self showScreen:@"skirmish"];
	} else if (h == kBack) {
		[self click];
		[self showScreen:@"main"];
		_focus = std::max(0, int(_screen));
	} else if (h == kPlay && _screen == kSkirmishSetup) {
		[self playSetup];
	} else if (h == kPlay) {
		[self play];
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
			for (int k = 0; k < 6; k++) {
				_focus = (_focus + d + 6) % 6;
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
	else if (c == NSLeftArrowFunctionKey && _screen != kTutorials) _difficulty = std::max(0, _difficulty - 1);
	else if (c == NSRightArrowFunctionKey && _screen != kTutorials) _difficulty = std::min(5, _difficulty + 1);
	else if (ret) [self play];
	else if (esc) {
		const Screen from = _screen;
		[self click];
		[self showScreen:@"main"];
		_focus = int(from) - 1;
		_help = kMainItems[_focus].help;
		return;
	}
	if (c == NSLeftArrowFunctionKey || c == NSRightArrowFunctionKey)
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
	const BOOL editable = !lobby || (_room && _room->is_host() && !_mpSave);  // only the host changes the setup, and not of a saved game
	ron::sim::RandomGameSetup* sp = &_setup;  // the closures outlive this call: capture the ivar's address, not self
	// ---- RULESWIN: the rules, label at the left, combo at the right
	{
		const int cx = 770, cw = 222, dy = 33;
		int y = 106;
		auto add = [&](const char* label, const std::vector<std::string>& items, int sel, std::function<void(int)> set) {
			SetupCombo c;
			c.r = {cx, cx + cw, y, y + 21};
			c.items = items;
			c.sel = std::clamp(sel, 0, std::max(0, int(items.size()) - 1));
			c.set = std::move(set);
			c.enabled = editable;
			c.label = label;
			c.lx = 592, c.ly = y;
			_sc.push_back(std::move(c));
			y += dy;
		};
		std::vector<std::string> styles = _lists.style;
		if (styles.size() > 23) styles.resize(23);
		add("Map Style:", styles, _setup.map_style, [sp](int v) { sp->map_style = v; });
		add("Map Size:", _lists.size, _setup.map_size, [sp](int v) { sp->map_size = v; });
		add("Nations:", numbered(2, 8), _setup.num_nations - 2, [sp](int v) { sp->num_nations = v + 2; });
		add("Start With:", _lists.town, _setup.starting_town, [sp](int v) { sp->starting_town = v; });
		add("Resources:", _lists.resources, _setup.starting_resources, [sp](int v) { sp->starting_resources = v; });
		add("Start Age:", _lists.start, _setup.starting_technology, [sp](int v) { sp->starting_technology = v; });
		add("End Age:", _lists.end, _setup.ending_technology, [sp](int v) { sp->ending_technology = v; });
		add("Victory:", _lists.victory, _setup.victory, [sp](int v) { sp->victory = v; });
		add("Elimination:", _lists.elim, _setup.elimination, [sp](int v) { sp->elimination = v; });
		add("Population:", _lists.pop, _setup.pop_limit, [sp](int v) { sp->pop_limit = v; });
		add("Reveal Map:", _lists.reveal, _setup.reveal_map, [sp](int v) { sp->reveal_map = v; });
		add("Random Starts:", {"No", "Yes"}, _setup.randomize_starts ? 1 : 0, [sp](int v) { sp->randomize_starts = v == 1; });
		if (lobby) {
			_seedRect = _seedBtn = IfaceRect{0, 0, 0, 0};  // the host draws the seed when the game starts
		} else {
			_seedRect = {cx, cx + cw - 84, y, y + 21};
			_seedBtn = {cx + cw - 78, cx + cw, y - 3, y + 25};
			_st.push_back({592, y, "Seed:", 0});
		}
		_rulesBottom = y + dy;
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
	c.sel = item;
	if (c.set) c.set(item);
	[self saveSetup];
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
		NSString* a = [d stringForKey:@"MpAddress"];
		_mpAddr = a.length ? a.UTF8String : "";
	}
	_mpMessage.clear();
	_mpStarting = NO;
	if (_screen == kMpConnect) {
		_room.reset();
		_mpField = 0;
		return;
	}
	// the lobby: the host edits the Skirmish setup, a guest sees the host's
	_mpField = -1;
	_mpChat.clear();
	_mpChatSeen = 0;
	_mpPublished.clear();
	_mpLines.clear();
	_mpSave.reset();
	_mpSaveText.clear();
	[self openSetup];
	_setup.game_speed = 2;
	_seedRandom = YES;
	if (_room && _room->is_host()) {
		_mpLines.push_back({1, "You are the host. Players join with your address and port " + std::to_string(_room->port()) + "."});
	} else if (_room) {
		_mpLines.push_back({1, "Connected. The host sets the game up; press Ready when you are."});
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
	std::string host = _mpAddr;
	int port = 7777;
	const size_t colon = host.rfind(':');
	if (colon != std::string::npos) {
		if (!parse_port(host.substr(colon + 1), port)) {
			_mpMessage = "The port after the colon must be a number between 1024 and 65535.";
			return;
		}
		host = host.substr(0, colon);
	}
	while (!host.empty() && host.back() == ' ') host.pop_back();
	if (host.empty()) {
		_mpMessage = "Type the host's address, for example 192.168.0.12:7777.";
		return;
	}
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

// The room's traffic every frame the lobby is up: new players, departures, chat, the setup changes of the host, the START.
- (void)mpPump {
	if (!_room || _mpStarting) return;
	_room->pump();
	for (std::string n; !(n = _room->take_notice()).empty();) _mpLines.push_back({1, n});
	ron::net::Lobby::ChatLine line;
	while (_room->lobby().take_chat(line)) {
		std::string who = _room->name_of(line.from);
		if (who.empty()) who = "Player " + std::to_string(line.from + 1);
		_mpLines.push_back({0, who + ": " + line.text});
	}
	if (_room->host_lost()) {
		[self mpLeave:@"The host closed the room."];
		return;
	}
	if (_room->is_host()) {
		// the seats of connected players are human, the rest computer; changes go to the guests (a saved game keeps its own seats)
		if (!_mpSave) {
			for (int i = 0; i < 8; i++) _setup.nation[size_t(i)].human = (_room->present_mask() >> i) & 1;
			_setup.nation[0].human = true;
			int top = 0;
			for (int i = 0; i < 8; i++)
				if ((_room->present_mask() >> i) & 1) top = i;
			_setup.num_nations = std::clamp(std::max(_setup.num_nations, top + 1), 2, 8);
		}
		ron::sim::RandomGameSetup s = _setup;
		s.seed = 0;
		const std::string cur = ron::sim::random_setup_string(s);
		if (cur != _mpPublished) {
			_room->publish(s);
			_mpPublished = cur;
		}
	} else {
		ron::sim::RandomGameSetup s;
		if (_room->read_setup(s)) {
			_setup = s;
			const BOOL fromSave = _room->lobby().has("from_save");
			if (_room->started() && (!fromSave || _room->lobby().file_complete() || _room->lobby().file_failed())) {
				_mpStarting = YES;
				if (fromSave && !_room->lobby().file_complete()) {
					[self mpLeave:@"The saved game did not arrive intact."];
					return;
				}
				std::shared_ptr<std::vector<uint8_t>> save;
				if (fromSave) save = std::make_shared<std::vector<uint8_t>>(_room->lobby().file());
				[self.host frontEndPlayMultiplayer:_room setup:s save:save];
			}
		}
	}
}

- (void)mpSend {
	if (!_room || _mpChat.empty()) return;
	_room->say(_mpChat);
	_mpLines.push_back({0, clean_name(_mpName) + ": " + _mpChat});
	_mpChat.clear();
}

// Play: the host starts the game (when every guest is ready and a second player is in), a guest toggles Ready.
- (void)mpPlay {
	if (!_room) return;
	if (!_room->is_host()) {
		[self click];
		_room->set_ready(!_room->ready_of(_room->slot()));
		return;
	}
	ron::sim::RandomGameSetup s = _setup;
	if (_mpSave) {  // continue a saved game: the connected players must sit in the seats that were human
		uint32_t need = 0;
		for (int i = 0; i < s.num_nations && i < 8; i++)
			if (s.nation[size_t(i)].human) need |= 1u << i;
		if (_room->present_mask() != need) {
			_mpLines.push_back({1, "The saved game needs players in exactly its human seats; the players in the room do not match them."});
			return;
		}
		if (!_room->guests_ready()) {
			_mpLines.push_back({1, "Every player must be ready before the game can start."});
			return;
		}
		[self click];
		_room->publish(s);
		_room->lobby().set("from_save", "1");
		_room->lobby().send_file(*_mpSave);
		_room->start(s.seed);
		_mpStarting = YES;
		[self.host frontEndPlayMultiplayer:_room setup:s save:_mpSave];
		return;
	}
	if (!_room->seat_humans(s)) {
		_mpLines.push_back({1, "Nobody has joined yet: a multiplayer game needs at least two players."});
		return;
	}
	if (!_room->guests_ready()) {
		_mpLines.push_back({1, "Every player must be ready before the game can start."});
		return;
	}
	[self click];
	s.game_speed = 2;
	s.seed = arc4random();
	if (s.seed == 0) s.seed = 1;
	[self saveSetup];
	_room->publish(s);
	_room->start(s.seed);
	_mpStarting = YES;
	[self.host frontEndPlayMultiplayer:_room setup:s save:nullptr];
}

// The host picks a saved multiplayer game (written by the in-game Save Game): the session continues it. The setup of the save fills the rules;
// the players must take the seats that were human.
- (void)mpLoadFile {
	if (_mpSave) {  // a second click forgets it
		_mpSave.reset();
		_mpSaveText.clear();
		_mpLines.push_back({1, "The saved game was forgotten; the setup is yours again."});
		return;
	}
	NSOpenPanel* panel = [NSOpenPanel openPanel];
	panel.title = @"Load a saved multiplayer game";
	panel.directoryURL = [NSURL fileURLWithPath:@(mp::default_mp_save_dir().c_str())];
	if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
	NSData* data = [NSData dataWithContentsOfURL:panel.URL];
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
		int humans = 0;
		for (int i = 0; i < s.num_nations && i < 8; i++) humans += s.nation[size_t(i)].human;
		_mpSaveText = std::string(panel.URL.lastPathComponent.UTF8String) + ", frame " + std::to_string(info.frame);
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
	[self thinBox:{200, 824, 104, 622} fill:argb(120, 0, 0, 0)];
	_ui.next_layer();
	[self textAt:f14 text:"Your name" x:232 y:152 color:kLabel];
	_mpRect[0] = {400, 780, 148, 172};
	[self drawField:_mpRect[0] text:_mpName hint:"Player" focused:_mpField == 0];
	_ui.next_layer();
	[self textAt:f16 text:"Host a game" x:232 y:208 color:kWhite];
	[self textAt:f14 text:"Port" x:232 y:252 color:kLabel];
	_mpRect[1] = {400, 520, 248, 272};
	[self drawField:_mpRect[1] text:_mpPort hint:"7777" focused:_mpField == 1];
	_ui.next_layer();
	const IfaceRect hostBtn{592, 780, 244, 276}, joinBtn{652, 780, 382, 414};
	[self popupButton:hostBtn label:"Host" state:(_pressed == kMpHost ? 2 : _hover == kMpHost ? 1 : 0) font:f14];
	int y = 290;
	for (const std::string& line : wrap_text(_ui, f12, "You become the host and open a room on this port. Players on your network join it with your address and the same port.", 560)) {
		[self textAt:f12 text:line x:232 y:float(y) color:kSecondary];
		y += lh12;
	}
	_ui.next_layer();
	[self textAt:f16 text:"Join a game" x:232 y:350 color:kWhite];
	[self textAt:f14 text:"Host address" x:232 y:390 color:kLabel];
	_mpRect[2] = {400, 640, 386, 410};
	[self drawField:_mpRect[2] text:_mpAddr hint:"192.168.0.12:7777" focused:_mpField == 2];
	_ui.next_layer();
	[self popupButton:joinBtn label:"Join" state:(_pressed == kMpJoin ? 2 : _hover == kMpJoin ? 1 : 0) font:f14];
	y = 428;
	for (const std::string& line : wrap_text(_ui, f12, "Type the host's address and port, for example 192.168.0.12:7777. If you leave the port out, 7777 is used.", 560)) {
		[self textAt:f12 text:line x:232 y:float(y) color:kSecondary];
		y += lh12;
	}
	if (!_mpMessage.empty()) {
		y = 484;
		for (const std::string& line : wrap_text(_ui, f14, _mpMessage, 560)) {
			[self textAt:f14 text:line x:232 y:float(y) color:rgb(255, 170, 120)];
			y += lh12 + 2;
		}
	}
	y = 548;
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
	if (IfaceRect{592, 780, 244, 276}.contains(x, y)) return kMpHost;
	if (IfaceRect{652, 780, 382, 414}.contains(x, y)) return kMpJoin;
	for (int i = 0; i < 3; i++)
		if (_mpRect[i].contains(x, y)) return kMpName + i;
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
	if (ret) {
		if (_mpField == 2) [self mpJoin];
		else [self mpHost];
		return;
	}
	std::string* target = _mpField == 0 ? &_mpName : _mpField == 1 ? &_mpPort : &_mpAddr;
	if (_mpField < 0) return;
	if ([self editText:*target event:e limit:_mpField == 0 ? 24 : _mpField == 1 ? 5 : 64]) _mpMessage.clear();
}

// ---- the lobby screen

- (int)hitLobby:(int)x y:(int)y {
	if (_openCombo < 0 && _mpChatRect.contains(x, y)) return kMpChat;
	if (_openCombo < 0 && _mpLoadRect.contains(x, y)) return kMpLoad;
	return [self hitSetup:x y:y];
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
	} cols[5] = {{36, "Player"}, {126, "Nation"}, {264, "Status"}, {370, "Difficulty"}, {470, "Team"}};
	for (const auto& c : cols) [self textAt:f14 text:c.t x:float(c.x) y:104 color:kSecondary];
	_ui.next_layer();
	for (const SetupText& t : _st) {
		const uint32_t col = t.kind == 1 ? rgb(130, 130, 130) : t.kind == 2 ? kWhite : t.kind == 3 ? rgb(150, 255, 150) : t.kind == 4 ? rgb(190, 190, 190) : kLabel;
		[self textAt:f14 text:t.text x:float(t.x) y:float(t.y) color:col];
	}
	for (const SetupCombo& c : _sc)
		if (!c.label.empty()) [self textAt:f14 text:c.label x:float(c.lx) y:float(c.ly + 2) color:kLabel];
	// The chat: the log above the input line
	{
		_ui.next_layer();
		const IfaceRect chat{28, 558, 446, 622};
		[self thinBox:chat fill:argb(110, 0, 0, 0)];
		_ui.next_layer();
		_mpLogRect = {36, 550, 450, 592};
		_mpChatRect = {36, 550, 596, 618};
		std::vector<std::pair<int, std::string>> shown;
		for (const auto& ln : _mpLines)
			for (const std::string& part : wrap_text(_ui, f12, ln.second, _mpLogRect.w())) shown.push_back({ln.first, part});
		const int lh12 = _ui.font_metrics(f12) ? _ui.font_metrics(f12)->line_height : 14;
		const int fit = std::max(1, _mpLogRect.h() / lh12);
		int y = _mpLogRect.t;
		for (size_t i = shown.size() > size_t(fit) ? shown.size() - size_t(fit) : 0; i < shown.size(); i++) {
			[self textAt:f12 text:shown[i].second x:float(_mpLogRect.l) y:float(y) color:shown[i].first == 1 ? rgb(255, 224, 140) : kWhite];
			y += lh12;
		}
		_ui.next_layer();
		[self drawField:_mpChatRect text:_mpChat hint:"Press Tab or start typing to chat" focused:_mpField == 3];
	}
	// The room, on the rules panel under the combos
	{
		_ui.next_layer();
		int y = _rulesBottom - 18;
		const std::string line1 = host ? "Hosting on port " + std::to_string(_room ? _room->port() : 0) : "Joined " + (_room ? _room->address() : std::string());
		[self textAt:f14 text:line1 x:592 y:float(y) color:kWhite];
		y += lh;
		if (host && _room) {
			std::string addr;
			for (const std::string& a : _room->lan_addresses()) addr += (addr.empty() ? "" : ", ") + a;
			for (const std::string& part : wrap_text(_ui, f12, addr.empty() ? "No network address found." : "Others type: " + addr + ":" + std::to_string(_room->port()), 395)) {
				[self textAt:f12 text:part x:592 y:float(y) color:kSecondary];
				y += lh;
			}
		}
		const int n = _room ? _room->humans() : 0;
		[self textAt:f12 text:std::to_string(n) + (n == 1 ? " player in the room." : " players in the room.") x:592 y:float(y) color:kSecondary];
		y += lh;
		const BOOL fromSave = _mpSave || (_room && _room->lobby().has("from_save"));
		if (fromSave) {
			for (const std::string& part : wrap_text(_ui, f12, "Continuing a saved game" + (_mpSaveText.empty() ? std::string() : ": " + _mpSaveText) + ". The rules come from the save.", 395)) {
				[self textAt:f12 text:part x:592 y:float(y) color:rgb(255, 224, 140)];
				y += lh;
			}
		}
		if (host) {
			_mpLoadRect = {592, 808, 588, 620};
			[self popupButton:_mpLoadRect label:_mpSave ? "Forget Saved Game" : "Load Saved Game..." state:(_pressed == kMpLoad ? 2 : _hover == kMpLoad ? 1 : 0) font:f12];
		} else {
			_mpLoadRect = {0, 0, 0, 0};
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
	const char* label = host ? (can ? "Start Game" : (_room && _room->humans() < 2 ? "Waiting for players" : "Waiting for ready")) : (ready ? "Not Ready" : "Ready");
	[self popupButton:L.play label:label state:(_pressed == kPlay ? 2 : _hover == kPlay ? 1 : 0) font:fb];
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
	for (int i = 0; i < 80; i++) {  // up to 4 s for the guests to come in and talk
		[self mpPump];
		if (_room->humans() > int(names.size()) && _mpLines.size() >= names.size() + 1) break;
		[NSThread sleepForTimeInterval:0.05];
	}
	[NSThread sleepForTimeInterval:0.3];
	[self mpPump];
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
		case ctwui::Action::Back: [self showScreen:@"main"]; _focus = 3; break;
		case ctwui::Action::PlayBattle: {
			std::shared_ptr<ctwui::Session> s = _ctw->session();
			if (s && [self.host respondsToSelector:@selector(frontEndPlayCtwBattle:)]) [self.host frontEndPlayCtwBattle:s];
			break;
		}
		default: break;
	}
}

@end
