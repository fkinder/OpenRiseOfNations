// Conquer the World screens of the front end (ctw_ui.h). Drawn with the front end's own skin (TopMenu bronze frame, ribbon,
// buttons) through a Bridge; the strategic map is the campaign's own art (conquest/RenderMaps/RENDER_*.png) tinted by owner
// through the colour-picking map (conquest/CTW_*_Map_01.png), the way the original finds the country under the cursor.
#import "ctw_ui.h"

#import <Cocoa/Cocoa.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>

#include "sim/rules.hpp"

namespace ctwui {

namespace rm = ron::metal;
namespace ctw = ron::sim::ctw;
using ron::IfaceRect;
using ron::IfacePiece;
using rm::argb;
using rm::rgb;
using rm::UiType;

namespace {

constexpr uint32_t kWhite = 0xFFFFFFFF, kBlack = 0xFF000000;
const uint32_t kLabel = rgb(212, 212, 212), kSecondary = rgb(189, 205, 216), kGold = rgb(240, 214, 120), kRed = rgb(230, 90, 80),
               kGreen = rgb(120, 220, 120);

// One colour per nation slot (the strategic map tints a country with its owner's colour).
const uint8_t kPalette[24][3] = {
    {196, 98, 58},  {58, 170, 120}, {222, 190, 60}, {150, 110, 60}, {170, 90, 190}, {60, 110, 200}, {200, 60, 70},  {230, 170, 70},
    {80, 160, 170}, {210, 80, 80},  {70, 90, 190},  {200, 50, 60},  {110, 110, 120}, {170, 40, 40}, {220, 60, 40},  {240, 240, 240},
    {90, 190, 90},  {240, 200, 80}, {60, 140, 80},  {150, 70, 40},  {50, 80, 170}, {230, 140, 40}, {120, 70, 160}, {200, 160, 220}};

uint32_t slot_color(int slot, int alpha = 255) {
	if (slot < 0) return argb(alpha, 150, 150, 150);
	const uint8_t* c = kPalette[slot % 24];
	return argb(alpha, c[0], c[1], c[2]);
}

std::vector<std::string> wrap(rm::UiRenderer& ui, int f, const std::string& text, int width) {
	std::vector<std::string> lines;
	std::string cur;
	size_t i = 0;
	auto push = [&] {
		lines.push_back(cur);
		cur.clear();
	};
	while (i <= text.size()) {
		size_t e = text.find_first_of(" \n", i);
		if (e == std::string::npos) e = text.size();
		const std::string word = text.substr(i, e - i);
		const bool nl = e < text.size() && text[e] == '\n';
		if (!word.empty()) {
			const std::string cand = cur.empty() ? word : cur + " " + word;
			if (!cur.empty() && ui.text_width(f, cand) > width) {
				push();
				cur = word;
			} else {
				cur = cand;
			}
		}
		if (nl) push();
		if (e >= text.size()) break;
		i = e + 1;
	}
	if (!cur.empty()) push();
	return lines;
}

std::string age_name(int age) {
	static const char* const names[8] = {"Ancient", "Classical", "Medieval", "Gunpowder", "Enlightenment", "Industrial", "Modern", "Information"};
	return names[std::clamp(age, 0, 7)];
}

}  // namespace

// ---------------------------------------------------------------- sessions and saves

std::string default_save_dir() {
	NSArray* dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
	NSString* base = dirs.count ? dirs[0] : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support"];
	NSString* dir = [base stringByAppendingPathComponent:@"OpenRiseOfNations/Conquer the World"];
	[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
	return dir.UTF8String;
}

std::vector<std::string> list_saves(const std::string& dir) {
	std::vector<std::pair<double, std::string>> found;
	NSFileManager* fm = [NSFileManager defaultManager];
	for (NSString* f in [fm contentsOfDirectoryAtPath:@(dir.c_str()) error:nil]) {
		if (![f.pathExtension.lowercaseString isEqualToString:@"ctw"]) continue;
		NSString* path = [@(dir.c_str()) stringByAppendingPathComponent:f];
		NSDictionary* at = [fm attributesOfItemAtPath:path error:nil];
		found.push_back({[at.fileModificationDate timeIntervalSince1970], path.UTF8String});
	}
	std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first > b.first; });
	std::vector<std::string> out;
	for (auto& f : found) out.push_back(f.second);
	return out;
}

std::string Session::autosave_path() const {
	return save_dir + "/autosave.ctw";
}

std::shared_ptr<Session> new_session(const std::string& game_dir, const ctw::ConquestSetup& setup, std::string* err) {
	auto s = std::make_shared<Session>();
	try {
		s->files = std::make_shared<ron::GameFiles>(game_dir);
		s->rules = std::make_shared<ron::sim::Rules>(ron::sim::load_rules(*s->files));
	} catch (const std::exception& e) {
		if (err) *err = e.what();
		return nullptr;
	}
	ctw::ConquestSetup cs = setup;
	if (cs.seed == 0) cs.seed = uint32_t(time(nullptr)) * 2654435761u + 1u;
	const std::string e = s->game.setup(*s->files, *s->rules, cs);
	if (!e.empty()) {
		if (err) *err = e;
		return nullptr;
	}
	s->save_dir = default_save_dir();
	return s;
}

std::shared_ptr<Session> load_session(const std::string& game_dir, const std::string& path, std::string* err) {
	auto s = std::make_shared<Session>();
	try {
		s->files = std::make_shared<ron::GameFiles>(game_dir);
		s->rules = std::make_shared<ron::sim::Rules>(ron::sim::load_rules(*s->files));
	} catch (const std::exception& e) {
		if (err) *err = e.what();
		return nullptr;
	}
	const std::string e = s->game.load_file(*s->files, *s->rules, path);
	if (!e.empty()) {
		if (err) *err = e;
		return nullptr;
	}
	s->save_dir = default_save_dir();
	return s;
}

void Session::battle_finished(int outcome) {
	ctw::ConquestGame& g = game;
	const size_t news0 = g.news.size();
	g.last_battle_victory = outcome == 1;
	if (battle_is_defense) {
		g.cur_link = battle.link;
		g.cur_style = battle.style;
		g.resolve_player_defense(outcome == 1);
		resume_ai = true;
	} else {
		g.process_results(0, outcome == 1, battle.link, battle.style);
		g.refresh_links();
	}
	last_result = g.news.size() > news0 ? g.news[news0].text : std::string();
	battle_pending = false;
	g.save_file(autosave_path());
}

// ---------------------------------------------------------------- the map art

namespace {

// Decodes a PNG with ImageIO into w x h RGBA (smooth: high-quality scaling, else nearest: exact colours for the pick map).
bool decode_png(const std::string& path, int w, int h, bool smooth, std::vector<uint8_t>& out) {
	NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
	CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
	if (!src) return false;
	CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
	CFRelease(src);
	if (!img) return false;
	out.assign(size_t(w) * size_t(h) * 4, 0);
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx = CGBitmapContextCreate(out.data(), size_t(w), size_t(h), 8, size_t(w) * 4, cs, kCGImageAlphaPremultipliedLast);
	CGColorSpaceRelease(cs);
	if (!ctx) {
		CGImageRelease(img);
		return false;
	}
	CGContextSetInterpolationQuality(ctx, smooth ? kCGInterpolationHigh : kCGInterpolationNone);
	CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
	CGContextRelease(ctx);
	CGImageRelease(img);
	return true;
}

bool png_size(const std::string& path, int* w, int* h) {
	NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
	CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
	if (!src) return false;
	NSDictionary* props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr));
	CFRelease(src);
	*w = [props[(NSString*)kCGImagePropertyPixelWidth] intValue];
	*h = [props[(NSString*)kCGImagePropertyPixelHeight] intValue];
	return *w > 0 && *h > 0;
}

id<MTLTexture> make_texture(id<MTLDevice> dev, int w, int h, const void* rgba, bool mips) {
	MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                                             width:NSUInteger(w)
	                                                                            height:NSUInteger(h)
	                                                                         mipmapped:mips];
	d.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> t = [dev newTextureWithDescriptor:d];
	[t replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(w), NSUInteger(h)) mipmapLevel:0 withBytes:rgba bytesPerRow:NSUInteger(w) * 4];
	return t;
}

}  // namespace

struct MapArt {
	bool loaded = false;
	int w = 0, h = 0;          // the composite's size (the art's, at most 2048 on the long side)
	float scale = 1;           // composite pixels per art-space pixel (tex_width)
	std::vector<uint8_t> base, comp;
	std::vector<uint8_t> node_at;                 // node + 1 per pixel, 0 = none
	std::vector<std::vector<uint32_t>> pixels;    // per node
	id<MTLTexture> tex = nil;
	std::vector<int> state_owner;                 // last painted owner per node (-2 = never)
	std::vector<int> state_hi;                    // 0 none, 1 hover, 2 selected, 3 target
	bool dirty = false;

	void load(const ctw::CampaignDef& def, const ron::GameFiles& files, id<MTLDevice> dev) {
		loaded = false;
		const std::string art = files.resolve(def.map_art), pick = files.resolve(def.pick_map);
		int pw = def.tex_width > 0 ? def.tex_width : 1024, ph = def.tex_height > 0 ? def.tex_height : 512;
		{  // the art-space size even when the pictures cannot be read: markers and links still draw
			const float f0 = std::min(1.0f, 2048.0f / float(std::max(pw, ph)));
			w = std::max(1, int(std::lround(pw * f0)));
			h = std::max(1, int(std::lround(ph * f0)));
			scale = f0;
		}
		if (art.empty() || pick.empty() || !png_size(pick, &pw, &ph)) return;
		const float f = std::min(1.0f, 2048.0f / float(std::max(pw, ph)));
		w = std::max(1, int(std::lround(pw * f)));
		h = std::max(1, int(std::lround(ph * f)));
		scale = def.tex_width > 0 ? float(w) / float(def.tex_width) : f;
		if (!decode_png(art, w, h, true, base)) return;
		std::vector<uint8_t> pk;
		if (!decode_png(pick, w, h, false, pk)) return;
		std::map<uint32_t, int> by_rgb;
		for (size_t i = 0; i < def.countries.size(); i++) {
			const ctw::CountryDef& c = def.countries[i];
			by_rgb[(uint32_t(c.red) << 16) | (uint32_t(c.green) << 8) | uint32_t(c.blue)] = int(i);
		}
		node_at.assign(size_t(w) * size_t(h), 0);
		pixels.assign(def.countries.size(), {});
		for (size_t p = 0; p < size_t(w) * size_t(h); p++) {
			if (pk[p * 4 + 3] < 250) continue;
			auto it = by_rgb.find((uint32_t(pk[p * 4]) << 16) | (uint32_t(pk[p * 4 + 1]) << 8) | uint32_t(pk[p * 4 + 2]));
			if (it == by_rgb.end()) continue;
			node_at[p] = uint8_t(it->second + 1);
			pixels[size_t(it->second)].push_back(uint32_t(p));
		}
		comp = base;
		state_owner.assign(def.countries.size(), -2);
		state_hi.assign(def.countries.size(), 0);
		tex = make_texture(dev, w, h, comp.data(), false);
		loaded = tex != nil;
	}

	// Repaints node n: owner tint (alpha) and the highlight state.
	void paint(int n, int owner, int hi) {
		const uint8_t* c = kPalette[owner < 0 ? 12 : owner % 24];
		const float a = owner < 0 ? 0.0f : 0.42f;
		const int boost = hi == 2 ? 60 : hi == 1 ? 28 : hi >= 3 ? 30 : 0;
		for (uint32_t p : pixels[size_t(n)]) {
			for (int k = 0; k < 3; k++) {
				float v = float(base[size_t(p) * 4 + size_t(k)]) * (1 - a) + float(c[k]) * a;
				if (hi == 3) v = v * 0.7f + (k == 1 ? 150.0f : 40.0f) * 0.3f;
				if (hi == 4) v = v * 0.7f + (k == 0 ? 190.0f : 30.0f) * 0.3f;
				comp[size_t(p) * 4 + size_t(k)] = uint8_t(std::clamp(int(v) + boost, 0, 255));
			}
		}
		state_owner[size_t(n)] = owner;
		state_hi[size_t(n)] = hi;
		dirty = true;
	}
	void upload() {
		if (!dirty || !tex) return;
		[tex replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(w), NSUInteger(h)) mipmapLevel:0 withBytes:comp.data() bytesPerRow:NSUInteger(w) * 4];
		dirty = false;
	}
};

// ---------------------------------------------------------------- the screens

namespace {
enum HitId : int {
	kNone = 0, kCampaign, kDiff, kNationPrev, kNationNext, kRandNations, kRandBonus, kStart, kBack, kLoad, kSaveItem, kSaveCancel,
	kTab, kEndTurn, kSaveGame, kMenuBtn, kOption, kDialog, kCard, kCardPlay, kCardSelect, kCardBuy, kDiploRow, kDiploAct, kDealType,
	kDealGive, kDealAsk, kDealGo, kMap, kNewsUp, kNewsDown, kZoomIn, kZoomOut, kFit
};
}

struct Screen::Impl {
	std::shared_ptr<ron::GameFiles> files;
	std::shared_ptr<ron::sim::Rules> rules;
	std::string dir;
	std::shared_ptr<Session> s;
	bool map_screen = false;
	Action action = Action::None;

	// ---- campaign list / set-up
	std::vector<std::string> stems;
	std::vector<std::shared_ptr<const ctw::CampaignDef>> defs;
	int sel = 0, diff = 2, nation = 0;
	bool rand_nations = false, rand_bonuses = false;
	std::vector<std::string> saves;
	bool saves_open = false;
	std::string setup_msg;

	// ---- strategic map
	MapArt art;
	std::string art_for;
	id<MTLTexture> disc = nil, white = nil;
	bool textures_ready = false;
	float zoom = 0, cx = 0, cy = 0;
	int sel_node = -1, hover_node = -1, army_from = -1, tab = 0;
	int card_sel = -1, diplo_sel = -1, news_scroll = 0;
	int deal_type = 0, deal_give = 0, deal_ask = 0;
	std::vector<ctw::LinkChoice> army_attacks;
	std::vector<int> army_moves;
	int target_card = -1;
	std::vector<int> target_nodes;

	struct Dialog {
		std::string title, text;
		std::vector<std::string> buttons;
		std::function<void(int)> on;
	};
	std::vector<Dialog> dialogs;
	bool atk_open = false, atk_defense = false;
	ctw::LinkChoice atk;
	ctw::StyleChoice atk_style;
	ctw::ConquestGame::BattleDef atk_def;

	struct Hit {
		IfaceRect r;
		int id, arg;
	};
	std::vector<Hit> hits;
	int hot_id = 0, hot_arg = 0;
	float mx = -1, my = -1, down_x = 0, cx0 = 0, cy0 = 0;
	bool down = false, dragging = false, press_map = false;
	int press_id = 0, press_arg = 0;
	float down_y = 0;
	IfaceRect vp, panel;

	ctw::ConquestGame* G() { return s ? &s->game : nullptr; }
	// The rules the campaign data is read with (the campaign list needs them before a session exists).
	const ron::sim::Rules& R() {
		if (s) return *s->rules;
		if (!rules) rules = std::make_shared<ron::sim::Rules>(ron::sim::load_rules(*files));
		return *rules;
	}

	// ---------------------------------------------------------------- helpers

	void add_hit(const IfaceRect& r, int id, int arg = 0) { hits.push_back({r, id, arg}); }
	int state_of(int id, int arg) const { return (down && hot_id == id && hot_arg == arg) ? 2 : (hot_id == id && hot_arg == arg ? 1 : 0); }
	void btn(Bridge& b, const IfaceRect& r, const std::string& label, int id, int arg = 0, bool enabled = true, int font = 14) {
		b.button(r, label, enabled ? state_of(id, arg) : 3, b.ui->font(font, -1));
		if (enabled) add_hit(r, id, arg);
	}
	void text(Bridge& b, int f, const std::string& t, float x, float y, uint32_t c = kWhite) { b.ui->text(f, t, x, y, c, kBlack); }
	int paragraph(Bridge& b, int f, const std::string& t, int x, int y, int w, uint32_t c = kWhite, int max_lines = 99) {
		const ron::FontMetrics* m = b.ui->font_metrics(f);
		const int lh = m ? m->line_height : 16;
		int n = 0;
		for (const std::string& line : wrap(*b.ui, f, t, w)) {
			if (n++ >= max_lines) break;
			text(b, f, line, float(x), float(y), c);
			y += lh;
		}
		return y;
	}
	void message(const std::string& title, const std::string& text, std::vector<std::string> buttons = {"OK"}, std::function<void(int)> on = nullptr) {
		dialogs.push_back({title, text, std::move(buttons), std::move(on)});
	}

	std::shared_ptr<const ctw::CampaignDef> def_of(size_t i) {
		if (defs.size() < stems.size()) defs.resize(stems.size());
		if (!defs[i]) {
			try {
				defs[i] = ctw::load_campaign(*files, R(), stems[i]);
			} catch (const std::exception&) {
			}
		}
		return defs[i];
	}

	// The nation choices of the selected campaign: -1 = random, else slots.
	std::vector<int> nation_choices() {
		std::vector<int> out;
		auto d = sel >= 0 && size_t(sel) < stems.size() ? def_of(size_t(sel)) : nullptr;
		if (!d) return out;
		std::vector<int> valid;
		for (const std::string& v : d->valid_nations) {
			const int k = d->tribe_by_key(v);
			if (k >= 0) valid.push_back(k);
		}
		if (valid.empty()) {
			std::set<int> owners;
			for (const ctw::CountryDef& c : d->countries) {
				const int k = d->tribe_by_key(c.owner);
				if (k >= 0 && c.capital) owners.insert(k);
			}
			valid.assign(owners.begin(), owners.end());
		}
		if (valid.size() > 1) out.push_back(-1);
		for (int k : valid) out.push_back(k);
		return out;
	}
	std::string nation_label(int slot) {
		if (slot < 0) return "Random";
		auto d = def_of(size_t(sel));
		return d ? d->tribes[size_t(slot)].display_name : "?";
	}

	// ---------------------------------------------------------------- the campaign list / set-up screen

	void draw_setup(Bridge& b) {
		if (stems.empty()) {
			stems = ctw::list_campaigns(*files);
			defs.resize(stems.size());
		}
		rm::UiRenderer& ui = *b.ui;
		const int ox = b.ox, oy = b.oy;
		auto R = [&](int l, int r, int t, int bt) { return IfaceRect{l, r, t, bt}.offset(ox, oy); };
		b.banner("Conquer the World");
		ui.next_window();
		const int f12 = ui.font(12, -1), f14 = ui.font(14, -1), f16 = ui.font(16, -1), f18 = ui.font(18, -1);
		// list of campaigns
		const IfaceRect list = R(60, 400, 100, 590);
		b.frame(R(40, 420, 84, 606));
		ui.fill(float(list.l), float(list.t), float(list.w()), float(list.h()), argb(150, 13, 11, 40), UiType::ColorBlend);
		ui.next_layer();
		for (size_t i = 0; i < stems.size(); i++) {
			const IfaceRect row = R(64, 396, 106 + int(i) * 64, 106 + int(i) * 64 + 60);
			b.row(row, int(i) == sel, hot_id == kCampaign && hot_arg == int(i));
			add_hit(row, kCampaign, int(i));
			ui.next_layer();
			auto d = def_of(i);
			text(b, f16, d ? d->name : stems[i], float(row.l + 12), float(row.t + 8));
			if (d) {
				text(b, f12, std::to_string(d->length()) + " rounds, " + std::to_string(d->countries.size()) + " territories" +
				                (d->colonies.empty() ? "" : ", " + std::to_string(d->colonies.size()) + " colonies"),
				     float(row.l + 12), float(row.t + 32), kSecondary);
			}
		}
		// description
		ui.next_window();
		const IfaceRect info = R(446, 978, 100, 420);
		b.frame(R(430, 994, 84, 436));
		ui.fill(float(info.l), float(info.t), float(info.w()), float(info.h()), argb(150, 13, 11, 40), UiType::ColorBlend);
		ui.next_layer();
		if (auto d = sel >= 0 && size_t(sel) < stems.size() ? def_of(size_t(sel)) : nullptr) {
			text(b, f18, d->name, float(info.l + 14), float(info.t + 10), kGold);
			int y = info.t + 40;
			y = paragraph(b, f14, d->desc, info.l + 14, y, info.w() - 28, kWhite);
			if (!d->intro.empty()) y = paragraph(b, f12, d->intro, info.l + 14, y + 8, info.w() - 28, kSecondary, 9);
			if (!d->victory_conditions.empty()) paragraph(b, f12, "Victory: " + d->victory_conditions, info.l + 14, y + 8, info.w() - 28, kLabel, 4);
		}
		// options
		ui.next_window();
		b.frame(R(430, 994, 450, 622));
		const std::vector<int> choices = nation_choices();
		nation = choices.empty() ? 0 : std::clamp(nation, 0, int(choices.size()) - 1);
		const int ry = 488;
		text(b, f14, "Your nation:", float(ox + 466), float(oy + ry + 5), kLabel);
		btn(b, R(600, 630, ry, ry + 26), "<", kNationPrev, 0, choices.size() > 1);
		text(b, f16, choices.empty() ? "" : nation_label(choices[size_t(nation)]), float(ox + 642), float(oy + ry + 4));
		btn(b, R(880, 910, ry, ry + 26), ">", kNationNext, 0, choices.size() > 1);
		text(b, f14, "Difficulty:", float(ox + 466), float(oy + ry + 42), kLabel);
		static const char* const dn[6] = {"Easiest", "Easy", "Moderate", "Tough", "Tougher", "Toughest"};
		for (int k = 0; k < 6; k++) {
			const IfaceRect r = R(574 + k * 62, 574 + k * 62 + 60, ry + 38, ry + 66);
			ui.next_layer();
			b.row(r, diff == k, hot_id == kDiff && hot_arg == k);
			add_hit(r, kDiff, k);
			ui.next_layer();
			ui.text_centered(f12, dn[k], r, kWhite, kBlack);
		}
		auto d = sel >= 0 && size_t(sel) < stems.size() ? def_of(size_t(sel)) : nullptr;
		if (d && d->allow_random_nations) {
			text(b, f14, std::string(rand_nations ? "[x] " : "[ ] ") + "Random nations", float(ox + 466), float(oy + ry + 80), kLabel);
			add_hit(R(462, 660, ry + 76, ry + 98), kRandNations);
		}
		if (d && d->allow_random_bonuses) {
			text(b, f14, std::string(rand_bonuses ? "[x] " : "[ ] ") + "Random bonuses", float(ox + 700), float(oy + ry + 80), kLabel);
			add_hit(R(696, 900, ry + 76, ry + 98), kRandBonus);
		}
		if (!setup_msg.empty()) text(b, f12, setup_msg, float(ox + 60), float(oy + 616), kRed);
		// bottom buttons
		ui.next_window();
		btn(b, R(60, 233, 646, 678), "Back", kBack);
		btn(b, R(400, 586, 646, 678), "Load Campaign...", kLoad);
		btn(b, R(740, 913, 646, 678), "Start", kStart, d != nullptr);
		if (saves_open) draw_saves(b);
	}

	void draw_saves(Bridge& b) {
		rm::UiRenderer& ui = *b.ui;
		const int f14 = ui.font(14, -1), f12 = ui.font(12, -1);
		ui.next_window();
		ui.fill(0, 0, float(b.ui_w), float(b.ui_h), argb(150, 0, 0, 0), UiType::ColorBlend);
		const IfaceRect r = IfaceRect{262, 762, 150, 600}.offset(b.ox, b.oy);
		b.box(r, argb(235, 20, 18, 50));
		ui.next_layer();
		text(b, ui.font(18, -1), "Load Campaign", float(r.l + 16), float(r.t + 12), kGold);
		if (saves.empty()) text(b, f14, "No saved campaigns yet.", float(r.l + 16), float(r.t + 54), kSecondary);
		for (size_t i = 0; i < saves.size() && i < 9; i++) {
			const IfaceRect row{r.l + 12, r.r - 12, r.t + 48 + int(i) * 40, r.t + 48 + int(i) * 40 + 36};
			b.row(row, false, hot_id == kSaveItem && hot_arg == int(i));
			add_hit(row, kSaveItem, int(i));
			ui.next_layer();
			NSString* name = @(saves[i].c_str());
			text(b, f14, name.lastPathComponent.stringByDeletingPathExtension.UTF8String, float(row.l + 10), float(row.t + 9));
		}
		btn(b, IfaceRect{r.r - 190, r.r - 16, r.b - 50, r.b - 16}, "Cancel", kSaveCancel);
		(void)f12;
	}

	void start_campaign() {
		const std::vector<int> choices = nation_choices();
		ctw::ConquestSetup cs;
		cs.campaign = stems[size_t(sel)];
		cs.difficulty = diff;
		cs.player = choices.empty() ? -1 : choices[size_t(nation)];
		cs.random_nations = rand_nations;
		cs.random_bonuses = rand_bonuses;
		cs.seed = 0;
		if (const char* e = getenv("CTW_SEED")) cs.seed = uint32_t(strtoul(e, nullptr, 0));
		std::string err;
		auto ns = new_session(dir, cs, &err);
		if (!ns) {
			setup_msg = "Could not start: " + err;
			return;
		}
		set_session(ns);
		open(true);
	}

	void set_session(std::shared_ptr<Session> ns) {
		s = std::move(ns);
		art_for.clear();
		sel_node = hover_node = army_from = -1;
		zoom = 0;
		tab = 0;
		dialogs.clear();
		atk_open = false;
		target_card = -1;
	}

	void open(bool map) {
		map_screen = map && s;
		hits.clear();
		if (map_screen) {
			ctw::ConquestGame& g = *G();
			for (const std::string& m : g.messages) message("Message", m);
			g.messages.clear();
		}
	}

	// ---------------------------------------------------------------- the strategic map

	void ensure_textures(Bridge& b) {
		if (textures_ready) return;
		const int D = 64;
		std::vector<uint8_t> px(size_t(D) * D * 4);
		for (int y = 0; y < D; y++) {
			for (int x = 0; x < D; x++) {
				const float dx = (float(x) + 0.5f - D / 2.0f) / (D / 2.0f - 1), dy = (float(y) + 0.5f - D / 2.0f) / (D / 2.0f - 1);
				const float d = std::sqrt(dx * dx + dy * dy);
				const float a = std::clamp((1.0f - d) * (D / 2.0f - 1) / 1.5f, 0.0f, 1.0f);
				uint8_t* q = &px[(size_t(y) * D + size_t(x)) * 4];
				q[0] = q[1] = q[2] = 255;
				q[3] = uint8_t(a * 255);
			}
		}
		disc = make_texture(b.device, D, D, px.data(), false);
		const uint8_t w4[4] = {255, 255, 255, 255};
		white = make_texture(b.device, 1, 1, w4, false);
		b.ui->set_texture("ctwdisc", disc, D, D);
		b.ui->set_texture("ctwwhite", white, 1, 1);
		textures_ready = true;
	}

	void ensure_art(Bridge& b) {
		ctw::ConquestGame& g = *G();
		if (art_for == g.def->file) return;
		art = MapArt();
		art.load(*g.def, *files, b.device);
		art_for = g.def->file;
		if (art.loaded) b.ui->set_texture("ctwmap", art.tex, art.w, art.h);
		zoom = 0;
	}

	// ---- camera
	void fit_camera() {
		if (vp.w() <= 0 || vp.h() <= 0) return;
		zoom = std::min(float(vp.w()) / float(art.w), float(vp.h()) / float(art.h));
		cx = art.w / 2.0f;
		cy = art.h / 2.0f;
	}
	void clamp_camera() {
		const float z = zoom;
		if (float(art.w) * z <= float(vp.w())) cx = art.w / 2.0f;
		else cx = std::clamp(cx, float(vp.w()) / (2 * z), float(art.w) - float(vp.w()) / (2 * z));
		if (float(art.h) * z <= float(vp.h())) cy = art.h / 2.0f;
		else cy = std::clamp(cy, float(vp.h()) / (2 * z), float(art.h) - float(vp.h()) / (2 * z));
	}
	float to_sx(float mxp) const { return float(vp.l) + float(vp.w()) / 2 + (mxp - cx) * zoom; }
	float to_sy(float myp) const { return float(vp.t) + float(vp.h()) / 2 + (myp - cy) * zoom; }
	float to_mx(float sx) const { return cx + (sx - float(vp.l) - float(vp.w()) / 2) / zoom; }
	float to_my(float sy) const { return cy + (sy - float(vp.t) - float(vp.h()) / 2) / zoom; }
	void node_pos(int n, float* x, float* y) {
		const ctw::CountryDef& c = G()->def->countries[size_t(n)];
		*x = float(c.x) * art.scale;
		*y = float(c.y) * art.scale;
	}

	int node_at_screen(float sx, float sy) {
		if (!G() || zoom <= 0 || !vp.contains(int(sx), int(sy))) return -1;
		const float px = to_mx(sx), py = to_my(sy);
		if (art.loaded && px >= 0 && py >= 0 && px < float(art.w) && py < float(art.h)) {
			const int v = art.node_at[size_t(int(py)) * size_t(art.w) + size_t(int(px))];
			if (v > 0) return v - 1;
		}
		int best = -1;
		float bd = 18.0f * 18.0f;
		for (size_t n = 0; n < G()->nodes.size(); n++) {
			float x, y;
			node_pos(int(n), &x, &y);
			const float dx = to_sx(x) - sx, dy = to_sy(y) - sy;
			if (dx * dx + dy * dy < bd) {
				bd = dx * dx + dy * dy;
				best = int(n);
			}
		}
		return best;
	}

	// ---- drawing primitives on the map
	void line(Bridge& b, float x0, float y0, float x1, float y1, float th, uint32_t c) {
		const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
		if (len < 0.5f) return;
		const float nx = -dy / len * th / 2, ny = dx / len * th / 2;
		const float xy[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny};
		const float uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
		b.ui->add_quad("ctwwhite", xy, uv, c, UiType::ColorAlphaMod);
	}
	void dashed(Bridge& b, float x0, float y0, float x1, float y1, float th, uint32_t c) {
		const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
		if (len < 1) return;
		const float on = 9, off = 6;
		for (float t = 0; t < len; t += on + off) {
			const float t1 = std::min(len, t + on);
			line(b, x0 + dx * t / len, y0 + dy * t / len, x0 + dx * t1 / len, y0 + dy * t1 / len, th, c);
		}
	}
	void disc_at(Bridge& b, float x, float y, float r, uint32_t c) {
		const float xy[8] = {x - r, y - r, x + r, y - r, x + r, y + r, x - r, y + r};
		const float uv[8] = {0, 0, 64, 0, 64, 64, 0, 64};
		b.ui->add_quad("ctwdisc", xy, uv, c, UiType::ColorAlphaMod);
	}

	std::string owner_text(int owner) {
		ctw::ConquestGame& g = *G();
		if (owner < 0) return "Barbarians";
		std::string t = g.tribe_name(owner);
		if (g.leaders[size_t(owner)].vassal_of >= 0) t += " (vassal of " + g.tribe_name(g.leaders[size_t(owner)].vassal_of) + ")";
		return t;
	}

	void recompute_options() {
		army_moves.clear();
		army_attacks.clear();
		ctw::ConquestGame& g = *G();
		if (army_from < 0) return;
		army_moves = g.move_targets(army_from);
		for (const ctw::LinkChoice& l : g.available_links) {
			if (l.from == army_from) army_attacks.push_back(l);
		}
	}

	bool is_player_army(int node) {
		for (const ctw::Piece& p : G()->pieces[size_t(G()->player_tribe)]) {
			if (p.node == node) return true;
		}
		return false;
	}

	void sync_colors(Bridge& b) {
		ctw::ConquestGame& g = *G();
		if (!art.loaded) return;
		for (size_t n = 0; n < g.nodes.size(); n++) {
			int hi = 0;
			if (int(n) == sel_node) hi = 2;
			else if (std::find(target_nodes.begin(), target_nodes.end(), int(n)) != target_nodes.end()) hi = 3;
			else if (std::any_of(army_attacks.begin(), army_attacks.end(), [&](const ctw::LinkChoice& l) { return l.to == int(n); })) hi = 4;
			else if (std::find(army_moves.begin(), army_moves.end(), int(n)) != army_moves.end()) hi = 3;
			else if (int(n) == hover_node) hi = 1;
			const int owner = g.nodes[n].owner;
			if (art.state_owner[n] != owner || art.state_hi[n] != hi) art.paint(int(n), owner, hi);
		}
		art.upload();
		(void)b;
	}

	void draw_map(Bridge& b) {
		ctw::ConquestGame& g = *G();
		rm::UiRenderer& ui = *b.ui;
		ensure_textures(b);
		ensure_art(b);
		const int W = b.ui_w, H = b.ui_h;
		vp = {0, W - 304, 34, H};
		panel = {W - 304, W, 34, H};
		if (zoom <= 0) fit_camera();
		clamp_camera();
		recompute_options();
		sync_colors(b);
		add_hit(vp, kMap);  // first: the buttons drawn later sit above it
		// background and the art
		ui.next_window();
		ui.fill(float(vp.l), float(vp.t), float(vp.w()), float(vp.h()), argb(255, 16, 30, 48), UiType::ColorBlend);
		ui.next_layer();
		if (art.loaded) {
			IfacePiece p;
			p.name = p.texture = "ctwmap";
			p.width = art.w;
			p.height = art.h;
			const float hw = float(vp.w()) / (2 * zoom), hh = float(vp.h()) / (2 * zoom);
			const float x0 = std::max(0.0f, cx - hw), x1 = std::min(float(art.w), cx + hw);
			const float y0 = std::max(0.0f, cy - hh), y1 = std::min(float(art.h), cy + hh);
			if (x1 > x0 && y1 > y0) ui.add_sub(p, x0, y0, x1 - x0, y1 - y0, to_sx(x0), to_sy(y0), (x1 - x0) * zoom, (y1 - y0) * zoom, kWhite, UiType::Normal);
		}
		// links
		ui.next_layer();
		const float th = std::clamp(zoom * 2.4f, 1.2f, 3.2f);
		for (size_t n = 0; n < g.nodes.size(); n++) {
			float x0, y0;
			node_pos(int(n), &x0, &y0);
			for (size_t k = 0; k < g.nodes[n].links.size(); k++) {
				const int t = g.nodes[n].links[k];
				bool reciprocal = false;
				for (int back : g.nodes[size_t(t)].links) reciprocal |= back == int(n);
				if (reciprocal && t < int(n)) continue;
				float x1, y1;
				node_pos(t, &x1, &y1);
				const bool hot = int(n) == sel_node || t == sel_node;
				bool atk_l = false;
				for (const ctw::LinkChoice& l : army_attacks) atk_l |= (l.from == int(n) && l.to == t);
				const uint32_t col = atk_l ? argb(235, 230, 70, 60) : hot ? argb(235, 250, 225, 90) : argb(120, 30, 20, 10);
				const float t2 = (hot || atk_l) ? th + 0.8f : th;
				if (g.nodes[n].link_sea[k]) dashed(b, to_sx(x0), to_sy(y0), to_sx(x1), to_sy(y1), t2, col);
				else line(b, to_sx(x0), to_sy(y0), to_sx(x1), to_sy(y1), t2, col);
			}
		}
		// labels, capitals, level pips
		ui.next_layer();
		const int f12 = ui.font(12, -1);
		for (size_t n = 0; n < g.nodes.size(); n++) {
			float x, y;
			node_pos(int(n), &x, &y);
			const float sx = to_sx(x), sy = to_sy(y);
			if (sx < float(vp.l) - 40 || sx > float(vp.r) + 40 || sy < float(vp.t) - 20 || sy > float(vp.b) + 20) continue;
			const ctw::Node& N = g.nodes[n];
			if (N.flags & ctw::kCapital) {
				disc_at(b, sx, sy, 6.5f, argb(255, 20, 16, 10));
				disc_at(b, sx, sy, 4.8f, argb(255, 255, 214, 80));
			} else {
				disc_at(b, sx, sy, 3.6f, argb(220, 20, 16, 10));
				disc_at(b, sx, sy, 2.4f, N.owner >= 0 ? slot_color(N.owner) : argb(255, 190, 190, 190));
			}
			const std::string name = g.node_name(int(n));
			const int tw = ui.text_width(f12, name);
			text(b, f12, name, sx - float(tw) / 2, sy + 8, int(n) == sel_node ? kGold : kWhite);
			if (N.level > 0 && zoom > 0.28f) {
				const float w0 = float(N.level) * 5 - 1;
				for (int k = 0; k < N.level; k++) ui.fill(sx - w0 / 2 + float(k) * 5, sy + 22, 4, 3, argb(235, 245, 235, 200), UiType::ColorBlend);
			}
			if (N.flags & ctw::kSupplyDepot) ui.fill(sx + float(tw) / 2 + 3, sy + 9, 5, 5, argb(235, 120, 220, 255), UiType::ColorBlend);
			if (N.flags & ctw::kFortified) ui.fill(sx + float(tw) / 2 + 3, sy + 16, 5, 5, argb(235, 220, 220, 220), UiType::ColorBlend);
		}
		// armies
		ui.next_layer();
		const float ar = std::clamp(zoom * 20.0f, 8.0f, 14.0f);
		for (int slot = 0; slot < ctw::kSlots; slot++) {
			for (const ctw::Piece& pc : g.pieces[size_t(slot)]) {
				float x, y;
				node_pos(pc.node, &x, &y);
				const float sx = to_sx(x), sy = to_sy(y) - ar - 4;
				if (sx < float(vp.l) - 20 || sx > float(vp.r) + 20 || sy < float(vp.t) - 20 || sy > float(vp.b) + 20) continue;
				const bool mine = slot == g.player_tribe;
				const bool selected = pc.node == army_from && mine;
				if (selected) disc_at(b, sx, sy, ar + 5, argb(255, 255, 235, 110));
				disc_at(b, sx, sy, ar + 2.2f, argb(255, 10, 8, 6));
				disc_at(b, sx, sy, ar, mine && pc.moved ? slot_color(slot, 150) : slot_color(slot));
				disc_at(b, sx, sy, ar * 0.42f, argb(mine ? 255 : 200, 250, 250, 250));
			}
		}
		// top bar
		ui.next_window();
		const IfaceRect top{0, W, 0, 34};
		b.box(top, argb(235, 14, 12, 38));
		ui.next_layer();
		const int f14 = ui.font(14, -1), f16 = ui.font(16, -1);
		const ctw::Leader& P = g.leaders[size_t(g.player_tribe)];
		text(b, f16, g.def->name, 12, 8, kGold);
		const std::string bar = "Round " + std::to_string(std::min(g.cur_round, g.length())) + " of " + std::to_string(g.length()) + "   " +
		                        age_name(g.cur_age) + " Age   " + g.tribe_name(g.player_tribe) + " (" + P.name + ")   Tribute " +
		                        std::to_string(P.tribute) + "   Territories " + std::to_string(g.num_nodes(g.player_tribe, true)) +
		                        "   Cards " + std::to_string(P.cards.size());
		text(b, f14, bar, float(24 + ui.text_width(f16, g.def->name)), 10, kWhite);
		if (g.turns_no_attack > 0) {
			const std::string tn = "Peace: " + std::to_string(g.turns_no_attack) + " more round" + (g.turns_no_attack == 1 ? "" : "s");
			text(b, f12, tn, float(W - 12 - ui.text_width(f12, tn)), 11, kSecondary);
		}
		draw_panel(b);
		// hint when targeting a card
		if (target_card >= 0) {
			const std::string hint = "Choose a highlighted territory for the card (Esc cancels).";
			const IfaceRect hb{vp.l + 20, vp.l + 40 + ui.text_width(f14, hint), vp.t + 10, vp.t + 36};
			b.box(hb, argb(220, 40, 30, 10));
			ui.next_layer();
			text(b, f14, hint, float(hb.l + 10), float(hb.t + 6), kGold);
		}
		// zoom buttons
		ui.next_window();
		btn(b, IfaceRect{vp.r - 40, vp.r - 12, vp.b - 40, vp.b - 12}, "+", kZoomIn, 0, true, 16);
		btn(b, IfaceRect{vp.r - 40, vp.r - 12, vp.b - 76, vp.b - 48}, "-", kZoomOut, 0, true, 16);
	}

	// ---------------------------------------------------------------- the side panel

	void draw_panel(Bridge& b) {
		ctw::ConquestGame& g = *G();
		rm::UiRenderer& ui = *b.ui;
		ui.next_window();
		b.box(panel, argb(230, 14, 12, 40));
		ui.next_layer();
		const int f12 = ui.font(12, -1), f14 = ui.font(14, -1), f16 = ui.font(16, -1), f18 = ui.font(18, -1);
		static const char* const tabs[4] = {"Map", "Cards", "Diplomacy", "News"};
		const int tw = (panel.w() - 16) / 4;
		for (int i = 0; i < 4; i++) {
			const IfaceRect r{panel.l + 8 + i * tw, panel.l + 8 + (i + 1) * tw - 3, panel.t + 6, panel.t + 34};
			btn(b, r, tabs[i], kTab, i, true, 12);
			if (i == tab) ui.fill(float(r.l), float(r.b + 1), float(r.w()), 2, argb(255, 240, 214, 120), UiType::ColorBlend);
		}
		const int x = panel.l + 14, w = panel.w() - 28;
		int y = panel.t + 50;
		if (tab == 0) {
			if (sel_node >= 0) {
				const ctw::Node& N = g.nodes[size_t(sel_node)];
				text(b, f18, g.node_name(sel_node), float(x), float(y), kGold);
				y += 26;
				text(b, f14, "Owner: " + owner_text(N.owner), float(x), float(y), slot_color(N.owner >= 0 ? N.owner : -1) | 0xFF000000u);
				y += 20;
				text(b, f14, "Strength: " + std::to_string(N.level) + " of 7" + ((N.flags & ctw::kCapital) ? "   Capital" : ""), float(x), float(y));
				y += 18;
				if (N.tribute > 0 && N.take_tribute) {
					text(b, f14, "Tribute to the conqueror: " + std::to_string(N.tribute), float(x), float(y), kLabel);
					y += 18;
				}
				if (N.rare >= 0) {
					text(b, f14, "Resource: " + ron::sim::to_utf8(s->rules->types[size_t(N.rare)].name), float(x), float(y), kLabel);
					y += 18;
				}
				if (N.wonder >= 0) {
					text(b, f14, "Wonder: " + ron::sim::to_utf8(s->rules->types[size_t(N.wonder)].name), float(x), float(y), kLabel);
					y += 18;
				}
				if (N.card.valid()) {
					text(b, f14, "Card: " + g.card_name(N.card), float(x), float(y), kLabel);
					y += 18;
				}
				if (N.flags & ctw::kSupplyDepot) {
					text(b, f14, "Supply depot (one more army)", float(x), float(y), kLabel);
					y += 18;
				}
				if (N.flags & ctw::kFortified) {
					text(b, f14, "Fortified", float(x), float(y), kLabel);
					y += 18;
				}
				if (N.owner == g.player_tribe && !(N.flags & ctw::kFortified)) {
					const int cost = g.build_cost(sel_node);
					btn(b, IfaceRect{x, x + w, y + 4, y + 30}, "Fortify (" + std::to_string(cost) + " tribute)", kOption, 1000 + sel_node, g.leaders[size_t(g.player_tribe)].tribute >= cost, 12);
					y += 36;
				}
				y += 6;
				if (army_from == sel_node && sel_node >= 0) {
					text(b, f14, "Your army:", float(x), float(y), kGreen);
					y += 20;
					for (size_t i = 0; i < army_moves.size() && y < panel.b - 150; i++) {
						btn(b, IfaceRect{x, x + w, y, y + 24}, "Move to " + g.node_name(army_moves[i]), kOption, int(i), true, 12);
						y += 27;
					}
					for (size_t i = 0; i < army_attacks.size() && y < panel.b - 150; i++) {
						const ctw::LinkChoice& l = army_attacks[i];
						std::string lab = "Attack " + g.node_name(l.to) + (l.defender >= 0 ? " (" + g.tribe_name(l.defender) + ")" : " (barbarians)");
						if (l.needs_war) lab += " - war " + std::to_string(l.declare_war_cost);
						btn(b, IfaceRect{x, x + w, y, y + 24}, lab, kOption, 100 + int(i), true, 12);
						y += 27;
					}
					if (army_moves.empty() && army_attacks.empty()) text(b, f12, "This army cannot move or attack now.", float(x), float(y), kSecondary);
				} else if (sel_node >= 0 && is_player_army(sel_node)) {
					text(b, f12, "Click the army to give it orders.", float(x), float(y), kSecondary);
				}
			} else {
				text(b, f16, "Conquer the World", float(x), float(y), kGold);
				y += 26;
				y = paragraph(b, f12, "Click a territory to see it. Click your army and then a highlighted territory to move (green) or to attack (red). "
				                       "Use the wheel to zoom and drag to pan. End Turn lets the other nations play.", x, y, w, kSecondary);
				y += 8;
				const ctw::Leader& P = g.leaders[size_t(g.player_tribe)];
				text(b, f14, "Your armies: " + std::to_string(g.pieces[size_t(g.player_tribe)].size()) + " of " + std::to_string(g.army_cap(g.player_tribe)), float(x), float(y), kWhite);
				y += 18;
				if (!P.selected.empty()) {
					std::string sc;
					for (const ctw::Card& c : P.selected) sc += (sc.empty() ? "" : ", ") + g.card_name(c);
					y = paragraph(b, f12, "For the next battle: " + sc, x, y + 4, w, kGold);
				}
				if (!g.sub_goals.empty()) {
					y += 8;
					text(b, f14, "Goals", float(x), float(y), kGold);
					y += 18;
					for (const std::string& sg : g.sub_goals) y = paragraph(b, f12, "- " + sg, x, y, w, kLabel);
				}
			}
		} else if (tab == 1) {
			draw_cards(b, x, y, w);
		} else if (tab == 2) {
			draw_diplo(b, x, y, w);
		} else {
			draw_news(b, x, y, w);
		}
		// bottom buttons
		const int by = panel.b - 98;
		btn(b, IfaceRect{x, x + w, by, by + 36}, g.game_end ? "Campaign over" : "End Turn", kEndTurn, 0, g.game_end == 0 && !g.ai_in_progress, 16);
		btn(b, IfaceRect{x, x + w / 2 - 4, by + 46, by + 78}, "Save", kSaveGame, 0, true, 14);
		btn(b, IfaceRect{x + w / 2 + 4, x + w, by + 46, by + 78}, "Menu", kMenuBtn, 0, true, 14);
		(void)f16;
	}

	void draw_news(Bridge& b, int x, int y, int w) {
		ctw::ConquestGame& g = *G();
		rm::UiRenderer& ui = *b.ui;
		const int f12 = ui.font(12, -1), f14 = ui.font(14, -1);
		text(b, ui.font(16, -1), "News", float(x), float(y), kGold);
		y += 24;
		const ron::FontMetrics* m = ui.font_metrics(f12);
		const int lh = m ? m->line_height : 14;
		std::vector<std::pair<std::string, int>> lines;  // text, round
		for (size_t i = g.news.size(); i-- > 0;) {
			for (const std::string& l : wrap(ui, f12, g.news[i].text, w - 8)) lines.push_back({l, g.news[i].round});
			lines.push_back({"", -1});
		}
		const int room = (panel.b - 110 - y) / lh;
		news_scroll = std::clamp(news_scroll, 0, std::max(0, int(lines.size()) - room));
		for (int i = news_scroll; i < int(lines.size()) && y < panel.b - 110; i++) {
			if (!lines[size_t(i)].first.empty()) text(b, f12, lines[size_t(i)].first, float(x), float(y), kWhite);
			y += lh;
		}
		(void)f14;
	}

	void draw_cards(Bridge& b, int x, int y, int w) {
		ctw::ConquestGame& g = *G();
		rm::UiRenderer& ui = *b.ui;
		const int f12 = ui.font(12, -1), f14 = ui.font(14, -1);
		const int me = g.player_tribe;
		ctw::Leader& P = g.leaders[size_t(me)];
		text(b, ui.font(16, -1), "Bonus cards", float(x), float(y), kGold);
		y += 24;
		if (P.cards.empty()) text(b, f14, "You hold no cards.", float(x), float(y), kSecondary);
		card_sel = std::clamp(card_sel, -1, int(P.cards.size()) - 1);
		for (size_t i = 0; i < P.cards.size() && y < panel.b - 250; i++) {
			const IfaceRect r{x, x + w, y, y + 24};
			b.row(r, int(i) == card_sel, hot_id == kCard && hot_arg == int(i));
			add_hit(r, kCard, int(i));
			ui.next_layer();
			const ctw::CardTypeDef& T = g.crules->cards[size_t(P.cards[i].type)];
			text(b, f14, g.card_name(P.cards[i]), float(x + 8), float(y + 4));
			const std::string kind = T.strategic() ? "map" : "battle";
			text(b, f12, kind, float(x + w - 8 - ui.text_width(f12, kind)), float(y + 6), kSecondary);
			y += 26;
		}
		y += 6;
		if (card_sel >= 0) {
			const ctw::Card c = P.cards[size_t(card_sel)];
			y = paragraph(b, f12, g.card_desc(c), x, y, w, kLabel, 5);
			y += 6;
			const ctw::CardTypeDef& T = g.crules->cards[size_t(c.type)];
			std::vector<int> tn, ts;
			const bool targets = T.strategic() && g.card_targets(me, c, &tn, &ts);
			if (T.strategic()) btn(b, IfaceRect{x, x + w, y, y + 26}, targets ? "Play on the map" : "No valid target now", kCardPlay, card_sel, targets && g.game_end == 0, 12);
			else btn(b, IfaceRect{x, x + w, y, y + 26}, "Use in the next battle", kCardSelect, card_sel, true, 12);
			y += 32;
		}
		if (!P.selected.empty()) {
			text(b, f12, "Chosen for the next battle:", float(x), float(y), kGold);
			y += 16;
			for (size_t i = 0; i < P.selected.size() && y < panel.b - 140; i++) {
				const IfaceRect r{x, x + w, y, y + 22};
				b.row(r, false, hot_id == kCardSelect && hot_arg == 1000 + int(i));
				add_hit(r, kCardSelect, 1000 + int(i));
				ui.next_layer();
				text(b, f12, g.card_name(P.selected[i]) + "  (click to put back)", float(x + 8), float(y + 4));
				y += 24;
			}
		}
		const int cost = g.card_cost(me);
		btn(b, IfaceRect{x, x + w, panel.b - 148, panel.b - 118}, "Buy a card (" + std::to_string(cost) + " tribute)", kCardBuy, 0, P.tribute >= cost && g.game_end == 0, 12);
	}

	void draw_diplo(Bridge& b, int x, int y, int w) {
		ctw::ConquestGame& g = *G();
		rm::UiRenderer& ui = *b.ui;
		const int f12 = ui.font(12, -1), f14 = ui.font(14, -1);
		const int me = g.player_tribe;
		text(b, ui.font(16, -1), "Diplomacy", float(x), float(y), kGold);
		y += 24;
		if (!g.no_negotiate_text.empty()) {
			paragraph(b, f12, g.no_negotiate_text, x, y, w, kRed);
			return;
		}
		std::vector<int> others;
		for (int s = 0; s < ctw::kSlots; s++) {
			if (s != me && g.leaders[size_t(s)].valid && g.leaders[size_t(s)].active) others.push_back(s);
		}
		static const char* const st[4] = {"War", "Peace", "Ally", "Vassal"};
		const uint32_t sc[4] = {kRed, kLabel, kGreen, kGold};
		int shown = 0;
		for (size_t i = 0; i < others.size() && shown < 8; i++, shown++) {
			const int s = others[i];
			const IfaceRect r{x, x + w, y, y + 22};
			b.row(r, s == diplo_sel, hot_id == kDiploRow && hot_arg == s);
			add_hit(r, kDiploRow, s);
			ui.next_layer();
			text(b, f12, g.tribe_name(s), float(x + 6), float(y + 4));
			const int stn = g.stance(me, s);
			text(b, f12, st[stn], float(x + w - 6 - ui.text_width(f12, st[stn])), float(y + 4), sc[stn]);
			text(b, f12, std::to_string(g.num_nodes(s, false)) + " terr.", float(x + w - 110), float(y + 4), kSecondary);
			y += 23;
		}
		if (others.size() > 8) {
			text(b, f12, "(and " + std::to_string(others.size() - 8) + " more nations)", float(x), float(y), kSecondary);
			y += 16;
		}
		if (diplo_sel >= 0 && g.leaders[size_t(diplo_sel)].active) {
			const int s = diplo_sel;
			y += 6;
			text(b, f14, g.tribe_name(s) + ": " + g.attitude_name(s, me) + ", " + std::to_string(g.leaders[size_t(s)].tribute) + " tribute", float(x), float(y), kWhite);
			y += 22;
			const int stn = g.stance(me, s);
			const int bw = w / 2 - 4;
			btn(b, IfaceRect{x, x + bw, y, y + 26}, stn == ctw::kWar ? "At war" : "War " + std::to_string(g.war_cost(me, s)), kDiploAct, 0, stn != ctw::kWar && stn != ctw::kVassal, 12);
			btn(b, IfaceRect{x + bw + 8, x + w, y, y + 26}, "Peace " + std::to_string(g.peace_cost(me, s)), kDiploAct, 1, stn == ctw::kWar, 12);
			y += 30;
			btn(b, IfaceRect{x, x + bw, y, y + 26}, "Alliance " + std::to_string(g.alliance_cost(me, s)), kDiploAct, 2, stn == ctw::kPeace, 12);
			btn(b, IfaceRect{x + bw + 8, x + w, y, y + 26}, "Vassal " + std::to_string(g.vassal_cost(me, s)), kDiploAct, 3, g.def->allow_vassals && stn != ctw::kVassal && stn != ctw::kWar, 12);
			y += 30;
			btn(b, IfaceRect{x, x + bw, y, y + 26}, "Demand tribute", kDiploAct, 4, stn != ctw::kVassal && g.leaders[size_t(s)].tribute >= 10, 12);
			btn(b, IfaceRect{x + bw + 8, x + w, y, y + 26}, "Bribe 25", kDiploAct, 5, g.leaders[size_t(me)].tribute >= 25, 12);
			y += 34;
			// the deal: what the nation's own script (diplo_response) answers
			static const char* const dt[4] = {"Peace", "Ally", "Vassal", "Exchange"};
			text(b, f12, "Propose a deal:", float(x), float(y + 4), kLabel);
			btn(b, IfaceRect{x + 100, x + w, y, y + 24}, dt[deal_type], kDealType, 0, true, 12);
			y += 30;
			text(b, f12, "You give", float(x), float(y + 5), kLabel);
			btn(b, IfaceRect{x + 90, x + 116, y, y + 24}, "-", kDealGive, -1, true, 12);
			text(b, f12, std::to_string(deal_give), float(x + 132), float(y + 5));
			btn(b, IfaceRect{x + 176, x + 202, y, y + 24}, "+", kDealGive, 1, true, 12);
			y += 28;
			text(b, f12, "They give", float(x), float(y + 5), kLabel);
			btn(b, IfaceRect{x + 90, x + 116, y, y + 24}, "-", kDealAsk, -1, true, 12);
			text(b, f12, std::to_string(deal_ask), float(x + 132), float(y + 5));
			btn(b, IfaceRect{x + 176, x + 202, y, y + 24}, "+", kDealAsk, 1, true, 12);
			y += 32;
			btn(b, IfaceRect{x, x + w, y, y + 26}, "Propose", kDealGo, 0, true, 12);
		} else if (!others.empty()) {
			text(b, f12, "Select a nation to negotiate.", float(x), float(y + 6), kSecondary);
		}
	}

	// ---------------------------------------------------------------- dialogs

	void draw_dialogs(Bridge& b) {
		rm::UiRenderer& ui = *b.ui;
		const int f12 = ui.font(12, -1), f14 = ui.font(14, -1), f18 = ui.font(18, -1);
		ctw::ConquestGame& g = *G();
		if (atk_open) {
			hits.clear();
			ui.next_window();
			ui.fill(0, 0, float(b.ui_w), float(b.ui_h), argb(140, 0, 0, 0), UiType::ColorBlend);
			const int dw = 640, dh = 400;
			const IfaceRect r{(b.ui_w - dw) / 2, (b.ui_w + dw) / 2, (b.ui_h - dh) / 2, (b.ui_h + dh) / 2};
			ui.next_window();
			b.box(r, argb(240, 20, 18, 52));
			ui.next_layer();
			const int x = r.l + 20, w = dw - 40;
			int y = r.t + 14;
			text(b, f18, atk_defense ? "Invasion of " + g.node_name(atk.to) + "!" : "Attack " + g.node_name(atk.to), float(x), float(y), kGold);
			y += 30;
			const std::string att = g.tribe_name(atk.attacker), dfn = atk.defender >= 0 ? g.tribe_name(atk.defender) : "the barbarians";
			y = paragraph(b, f14, "The " + att + " march from " + g.node_name(atk.from) + (atk.sea ? " by sea" : "") + " against " + dfn +
			                      " at " + g.node_name(atk.to) + " (strength " + std::to_string(g.nodes[size_t(atk.to)].level) + ").",
			              x, y, w, kWhite);
			y += 6;
			if (atk_style.index >= 0 && size_t(atk_style.index) < g.style_table.size()) {
				const ctw::StyleDef& st = g.style_table[size_t(atk_style.index)];
				text(b, ui.font(16, -1), "Battle: " + (st.title.empty() ? std::string("Conquest") : st.title), float(x), float(y), kGreen);
				y += 24;
				std::string d = !atk_style.info_text.empty() ? atk_style.info_text : st.description;
				d = d.empty() ? "A pitched battle." : d;
				// $NUM0 stands for the time limit in minutes
				for (size_t p; (p = d.find("$NUM0")) != std::string::npos;) d.replace(p, 5, std::to_string(atk_def.time_limit_min));
				y = paragraph(b, f12, d, x, y, w, kLabel, 5);
				std::string ob = st.objective;
				for (size_t p; (p = ob.find("$NUM0")) != std::string::npos;) ob.replace(p, 5, std::to_string(atk_def.time_limit_min));
				if (!ob.empty()) y = paragraph(b, f12, "Objective: " + ob, x, y + 4, w, kGold, 3);
			}
			y += 8;
			std::string seats;
			for (const ctw::ConquestGame::BattleSeat& s : atk_def.seats) {
				seats += (seats.empty() ? "" : ", ") + (s.human ? std::string("you") : s.barbarian ? std::string("barbarians") : g.tribe_name(s.slot));
				seats += (s.team == 0 && !s.human) ? " (your ally)" : "";
			}
			y = paragraph(b, f12, "Seats: " + seats + ". " + (atk_def.custom_map.empty() ? "A map is made for you." : "A prepared battlefield."), x, y, w, kSecondary, 3);
			if (atk.needs_war && !atk_defense) {
				y = paragraph(b, f14, "You are not at war with the " + dfn + ": declaring war costs " + std::to_string(atk.declare_war_cost) + " tribute.",
				              x, y + 4, w, g.leaders[size_t(g.player_tribe)].tribute >= atk.declare_war_cost ? kGold : kRed);
			}
			if (!g.leaders[size_t(g.player_tribe)].selected.empty()) {
				std::string sc;
				for (const ctw::Card& c : g.leaders[size_t(g.player_tribe)].selected) sc += (sc.empty() ? "" : ", ") + g.card_name(c);
				paragraph(b, f12, "Cards in play: " + sc, x, y + 4, w, kGold, 2);
			}
			const int by = r.b - 48;
			if (atk_defense) {
				btn(b, IfaceRect{r.l + 20, r.l + 200, by, by + 34}, "Defend (Play)", kDialog, 0, true, 14);
				btn(b, IfaceRect{r.l + 214, r.l + 424, by, by + 34}, "Auto-resolve", kDialog, 1, true, 14);
			} else {
				const bool can = !atk.needs_war || g.leaders[size_t(g.player_tribe)].tribute >= atk.declare_war_cost;
				btn(b, IfaceRect{r.l + 20, r.l + 200, by, by + 34}, atk.needs_war ? "Declare war, Play" : "Play", kDialog, 0, can, 14);
				btn(b, IfaceRect{r.r - 200, r.r - 20, by, by + 34}, "Cancel", kDialog, 1, true, 14);
			}
			return;
		}
		if (dialogs.empty()) return;
		hits.clear();
		const Dialog& d = dialogs.front();
		ui.next_window();
		ui.fill(0, 0, float(b.ui_w), float(b.ui_h), argb(140, 0, 0, 0), UiType::ColorBlend);
		const int dw = 520;
		const std::vector<std::string> lines = wrap(ui, f14, d.text, dw - 40);
		const ron::FontMetrics* m = ui.font_metrics(f14);
		const int lh = m ? m->line_height : 18;
		const bool column = d.buttons.size() > 3;
		const int btn_h = column ? 30 * int(d.buttons.size()) : 44;
		const int dh = 70 + int(lines.size()) * lh + btn_h + 20;
		const IfaceRect r{(b.ui_w - dw) / 2, (b.ui_w + dw) / 2, (b.ui_h - dh) / 2, (b.ui_h + dh) / 2};
		ui.next_window();
		b.box(r, argb(240, 20, 18, 52));
		ui.next_layer();
		text(b, f18, d.title, float(r.l + 20), float(r.t + 14), kGold);
		int y = r.t + 46;
		for (const std::string& l : lines) {
			text(b, f14, l, float(r.l + 20), float(y), kWhite);
			y += lh;
		}
		y += 14;
		const int n = int(d.buttons.size());
		for (int i = 0; i < n; i++) {
			if (column) btn(b, IfaceRect{r.l + 20, r.r - 20, y + i * 30, y + i * 30 + 26}, d.buttons[size_t(i)], kDialog, i, true, 12);
			else {
				const int bw = (dw - 40 - (n - 1) * 10) / n;
				btn(b, IfaceRect{r.l + 20 + i * (bw + 10), r.l + 20 + i * (bw + 10) + bw, y, y + 34}, d.buttons[size_t(i)], kDialog, i, true, 14);
			}
		}
	}

	// ---------------------------------------------------------------- turn logic

	void autosave() {
		if (s) s->game.save_file(s->autosave_path());
	}

	void drain_messages() {
		ctw::ConquestGame& g = *G();
		for (const std::string& m : g.messages) message("Message", m);
		g.messages.clear();
		// offers the AI nations made during the round (the campaign's diplo_offer scripts)
		while (!g.proposals.empty()) {
			const ctw::DiploOffer o = g.proposals.front();
			g.proposals.erase(g.proposals.begin());
			const int from = g.slot_of(o.tribe);
			std::string t = o.response.empty() ? "The " + (from >= 0 ? g.tribe_name(from) : o.tribe) + " make you an offer: " + o.offer_type + "." : o.response;
			if (o.tribe_trib || o.tribe2_trib) t += "\n\nThey give " + std::to_string(o.tribe_trib) + " tribute and ask " + std::to_string(o.tribe2_trib) + ".";
			message(from >= 0 ? g.tribe_name(from) : "Offer", t, {"Accept", "Decline"}, [this, o](int i) {
				if (i != 0) return;
				ctw::ConquestGame& gg = *G();
				// the offer is written from the proposer's side: tribe = the proposer, tribe2 = you
				const std::string err = gg.apply_offer(o);
				message("Deal", err.empty() ? "The deal is made." : err);
				gg.refresh_links();
			});
		}
	}

	void check_game_end() {
		ctw::ConquestGame& g = *G();
		static bool shown = false;
		if (g.game_end == 0) {
			shown = false;
			return;
		}
		if (shown) return;
		shown = true;
		std::string title, text;
		switch (g.game_end) {
		case ctw::kVictoryAllied: title = "Victory!"; text = "All the nations left are your allies and vassals. You have conquered the world."; break;
		case ctw::kVictorySome: title = "Victory!"; text = "Every nation that could oppose you has been beaten or joined you."; break;
		case ctw::kPlayerEliminated: title = "Defeat"; text = "Your nation has fallen."; break;
		case ctw::kTimeOver: title = "Time is up"; text = "The campaign's rounds have run out."; break;
		case ctw::kArmageddon: title = "Armageddon"; text = "The world has been destroyed."; break;
		default: title = "The campaign ends"; break;
		}
		if (!g.end_title.empty()) title = g.end_title;
		if (!g.end_text.empty()) text = g.end_text;
		message(title, text, {"OK"});
	}

	void run_ai() {
		ctw::ConquestGame& g = *G();
		for (int guard = 0; guard < 400; guard++) {
			ctw::AiStep st = g.ai_step();
			if (st.kind == ctw::AiStep::kDone) {
				g.finish_round();
				autosave();
				drain_messages();
				check_game_end();
				news_scroll = 0;
				tab = 3;  // the round's news
				army_from = -1;
				return;
			}
			if (st.kind == ctw::AiStep::kPlayerAttacked) {
				if (g.game_end == ctw::kPlayerEliminated) continue;
				g.cur_link = st.battle;
				g.cur_style = g.pick_style(st.battle);
				open_attack(st.battle, true);
				return;
			}
		}
	}

	void end_turn() {
		ctw::ConquestGame& g = *G();
		if (g.game_end != 0 || g.ai_in_progress) return;
		army_from = -1;
		sel_node = -1;
		g.end_turn_player();
		g.begin_ai_round();
		run_ai();
	}

	void open_attack(const ctw::LinkChoice& l, bool defense) {
		ctw::ConquestGame& g = *G();
		atk = l;
		atk_defense = defense;
		atk_style = defense ? g.cur_style : g.pick_style(l);
		atk_def = g.make_battle(l, atk_style);
		atk_open = true;
	}

	void attack_choice(int button) {
		ctw::ConquestGame& g = *G();
		if (atk_defense) {
			atk_open = false;
			if (button == 0) {
				s->battle = atk_def;
				s->battle_is_defense = true;
				s->battle_pending = true;
				action = Action::PlayBattle;
			} else {
				g.resolve_player_defense(g.rget(0, 99) < 50);
				message("Battle", g.news.empty() ? "" : g.news.back().text);
				run_ai();
			}
			return;
		}
		if (button == 1) {
			atk_open = false;
			return;
		}
		if (atk.needs_war) {
			std::string why;
			if (!g.buy_war(g.player_tribe, atk.defender, &why)) {
				message("Cannot attack", why);
				atk_open = false;
				return;
			}
			atk.needs_war = false;
			atk_def = g.make_battle(atk, atk_style);
		}
		atk_open = false;
		g.cur_link = atk;
		g.cur_style = atk_style;
		atk_def.link = atk;
		atk_def.style = atk_style;
		s->battle = atk_def;
		s->battle_is_defense = false;
		s->battle_pending = true;
		action = Action::PlayBattle;
	}

	void pick_slot_dialog(const std::string& title, const std::vector<int>& slots, std::function<void(int)> done) {
		ctw::ConquestGame& g = *G();
		std::vector<std::string> names;
		const size_t n = std::min<size_t>(slots.size(), 10);
		for (size_t i = 0; i < n; i++) names.push_back(g.tribe_name(slots[i]));
		names.push_back("Cancel");
		std::vector<int> sl(slots.begin(), slots.begin() + long(n));
		message(title, "Choose a nation.", names, [sl, done](int i) {
			if (i >= 0 && size_t(i) < sl.size()) done(sl[size_t(i)]);
		});
	}

	void play_card(int idx, int target, int target2 = -1) {
		ctw::ConquestGame& g = *G();
		std::string why;
		if (g.play_strategic_card(g.player_tribe, idx, target, target2, &why)) {
			message("Card played", g.news.empty() ? "" : g.news.back().text);
			g.refresh_links();
			recompute_options();
		} else {
			message("Card", why);
		}
		target_card = -1;
		target_nodes.clear();
	}

	void on_card_play(int idx) {
		ctw::ConquestGame& g = *G();
		ctw::Leader& P = g.leaders[size_t(g.player_tribe)];
		if (idx < 0 || size_t(idx) >= P.cards.size()) return;
		const ctw::Card c = P.cards[size_t(idx)];
		std::vector<int> tn, ts;
		if (!g.card_targets(g.player_tribe, c, &tn, &ts)) return;
		if (c.type == ctw::kCommandeerTransport) {
			play_card(idx, tn.empty() ? 0 : tn[0]);
			return;
		}
		if (!tn.empty()) {
			target_card = idx;
			target_nodes = tn;
			sel_node = -1;
			army_from = -1;
			tab = 0;
			return;
		}
		if (c.type == ctw::kFouche) {
			pick_slot_dialog("Fouche: first nation", ts, [this, idx, ts](int a) {
				std::vector<int> rest;
				for (int s2 : ts) {
					if (s2 != a) rest.push_back(s2);
				}
				pick_slot_dialog("Fouche: against", rest, [this, idx, a](int b2) { play_card(idx, a, b2); });
			});
			return;
		}
		pick_slot_dialog(g.card_name(c), ts, [this, idx](int a) { play_card(idx, a); });
	}

	void on_diplo_act(int act) {
		ctw::ConquestGame& g = *G();
		const int me = g.player_tribe, s2 = diplo_sel;
		if (s2 < 0) return;
		std::string why;
		bool ok = false;
		switch (act) {
		case 0: ok = g.buy_war(me, s2, &why); break;
		case 1: ok = g.buy_peace(me, s2, &why); break;
		case 2: ok = g.buy_ally(me, s2, &why); break;
		case 3: ok = g.buy_vassal(me, s2, &why); break;
		case 4: ok = g.demand_tribute(me, s2, &why); break;
		case 5: ok = g.offer_bribe(me, s2, 25, &why); break;
		}
		message("Diplomacy", ok ? (g.news.empty() ? "Done." : g.news.back().text) : why);
		g.refresh_links();
	}

	void on_deal_go() {
		ctw::ConquestGame& g = *G();
		const int me = g.player_tribe, s2 = diplo_sel;
		if (s2 < 0) return;
		static const char* const dt[4] = {"Peace", "Ally", "Vassal", "Exchange"};
		ctw::DiploOffer o;
		o.tribe = g.tribe_key(me);
		o.tribe2 = g.tribe_key(s2);
		o.offer_type = dt[deal_type];
		o.tribe_trib = std::min(deal_give, g.leaders[size_t(me)].tribute);
		o.tribe2_trib = std::min(deal_ask, g.leaders[size_t(s2)].tribute);
		o.id = "deal";
		if (g.run_diplo_response(&o)) {
			std::string reply = o.response.empty() ? (o.accepted ? "We agree." : "We refuse.") : o.response;
			if (o.accepted) {
				const std::string e = g.apply_offer(o);
				reply += e.empty() ? "\n\nThe deal is made." : "\n\n" + e;
				g.refresh_links();
			}
			message(g.tribe_name(s2), reply);
		} else {
			message("Diplomacy", "That nation does not answer.");
		}
	}

	void on_hit(int id, int arg) {
		ctw::ConquestGame* gp = G();
		switch (id) {
		case kCampaign: sel = arg; nation = 0; break;
		case kDiff: diff = arg; break;
		case kNationPrev: nation = std::max(0, nation - 1); break;
		case kNationNext: nation++; break;
		case kRandNations: rand_nations = !rand_nations; break;
		case kRandBonus: rand_bonuses = !rand_bonuses; break;
		case kStart: start_campaign(); break;
		case kBack: action = Action::Back; break;
		case kLoad:
			saves = list_saves(default_save_dir());
			saves_open = true;
			break;
		case kSaveItem: {
			std::string err;
			auto ns = load_session(dir, saves[size_t(arg)], &err);
			if (!ns) setup_msg = "Could not load: " + err;
			else {
				set_session(ns);
				open(true);
			}
			saves_open = false;
			break;
		}
		case kSaveCancel: saves_open = false; break;
		default: break;
		}
		if (!gp) return;
		ctw::ConquestGame& g = *gp;
		switch (id) {
		case kTab: tab = arg; news_scroll = 0; break;
		case kEndTurn: end_turn(); break;
		case kSaveGame: {
			const std::string path = s->save_dir + "/" + g.save_name() + " (round " + std::to_string(g.cur_round) + ").ctw";
			std::string err;
			message("Save", g.save_file(path, &err) ? "Saved as " + path.substr(path.find_last_of('/') + 1) : err);
			break;
		}
		case kMenuBtn:
			message("Menu", "Leave the campaign? Your progress is autosaved every round.", {"Main Menu", "Stay"}, [this](int i) {
				if (i == 0) {
					autosave();
					action = Action::Back;
				}
			});
			break;
		case kOption:
			if (arg >= 1000) {
				std::string why;
				if (!g.buy_fortification(g.player_tribe, arg - 1000, &why)) message("Fortify", why);
				g.refresh_links();
			} else if (arg >= 100) {
				if (size_t(arg - 100) < army_attacks.size()) open_attack(army_attacks[size_t(arg - 100)], false);
			} else if (size_t(arg) < army_moves.size()) {
				std::string why;
				const int to = army_moves[size_t(arg)];
				if (g.move_army(army_from, to, &why)) {
					army_from = -1;
					sel_node = to;
					g.refresh_links();
				} else {
					message("Move", why);
				}
			}
			break;
		case kDialog: {
			if (atk_open) {
				attack_choice(arg);
				break;
			}
			if (dialogs.empty()) break;
			auto on = dialogs.front().on;
			dialogs.erase(dialogs.begin());
			if (on) on(arg);
			break;
		}
		case kCard: card_sel = arg; break;
		case kCardPlay: on_card_play(arg); break;
		case kCardSelect: {
			ctw::Leader& P = g.leaders[size_t(g.player_tribe)];
			if (arg >= 1000) {
				const size_t i = size_t(arg - 1000);
				if (i < P.selected.size()) {
					P.cards.push_back(P.selected[i]);
					P.selected.erase(P.selected.begin() + long(i));
				}
			} else {
				std::string why;
				if (!g.select_tactical(g.player_tribe, arg, &why)) message("Card", why);
				card_sel = -1;
			}
			break;
		}
		case kCardBuy: {
			std::string why;
			if (!g.buy_card(g.player_tribe, &why)) message("Cards", why);
			break;
		}
		case kDiploRow: diplo_sel = arg; break;
		case kDiploAct: on_diplo_act(arg); break;
		case kDealType: deal_type = (deal_type + 1) % 4; break;
		case kDealGive: deal_give = std::clamp(deal_give + 10 * arg, 0, std::max(0, g.leaders[size_t(g.player_tribe)].tribute)); break;
		case kDealAsk: deal_ask = std::clamp(deal_ask + 10 * arg, 0, 500); break;
		case kDealGo: on_deal_go(); break;
		case kZoomIn: zoom = std::min(3.0f, zoom * 1.25f); break;
		case kZoomOut: zoom = std::max(0.05f, zoom / 1.25f); break;
		default: break;
		}
	}

	void on_map_click(int n) {
		ctw::ConquestGame& g = *G();
		if (target_card >= 0) {
			if (n >= 0 && std::find(target_nodes.begin(), target_nodes.end(), n) != target_nodes.end()) play_card(target_card, n);
			return;
		}
		if (n < 0) {
			sel_node = -1;
			army_from = -1;
			return;
		}
		if (army_from >= 0 && n != army_from) {
			if (std::find(army_moves.begin(), army_moves.end(), n) != army_moves.end()) {
				std::string why;
				if (g.move_army(army_from, n, &why)) {
					army_from = -1;
					sel_node = n;
					g.refresh_links();
				} else {
					message("Move", why);
				}
				return;
			}
			for (const ctw::LinkChoice& l : army_attacks) {
				if (l.to == n) {
					sel_node = n;
					open_attack(l, false);
					return;
				}
			}
		}
		sel_node = n;
		army_from = is_player_army(n) && g.game_end == 0 && !g.ai_in_progress ? n : -1;
		if (tab != 0) tab = 0;
	}

};


// ---------------------------------------------------------------- the public screen

Screen::Screen(std::shared_ptr<ron::GameFiles> files, std::string game_dir) : p_(new Impl) {
	p_->files = std::move(files);
	p_->dir = std::move(game_dir);
}
Screen::~Screen() = default;

void Screen::set_session(std::shared_ptr<Session> s) { p_->set_session(std::move(s)); }
std::shared_ptr<Session> Screen::session() const { return p_->s; }
bool Screen::has_session() const { return p_->s != nullptr; }
bool Screen::on_map() const { return p_->map_screen; }

void Screen::open(bool map_screen, const std::string& sub) {
	Impl& I = *p_;
	I.open(map_screen);
	I.action = Action::None;
	if (!I.map_screen) return;
	ctw::ConquestGame& g = *I.G();
	if (I.s && !I.s->last_result.empty()) {
		I.message("Battle result", I.s->last_result);
		I.s->last_result.clear();
	}
	if (sub == "map") I.dialogs.clear();  // headless: the plain map without the campaign's opening messages
	else if (sub == "news") I.tab = 3;
	else if (sub == "cards") I.tab = 1;
	else if (sub == "diplomacy") {
		I.tab = 2;
		for (int s = 0; s < ctw::kSlots; s++) {
			if (s != g.player_tribe && g.leaders[size_t(s)].active && g.stance(g.player_tribe, s) == ctw::kWar) {
				I.diplo_sel = s;
				break;
			}
		}
		if (I.diplo_sel < 0) {
			for (int s = 0; s < ctw::kSlots; s++) {
				if (s != g.player_tribe && g.leaders[size_t(s)].active) {
					I.diplo_sel = s;
					break;
				}
			}
		}
	} else if (sub == "attack") {
		// the first attack the player can make, with the dialog open
		g.refresh_links();
		if (!g.available_links.empty()) {
			I.sel_node = g.available_links[0].from;
			I.army_from = I.sel_node;
			I.open_attack(g.available_links[0], false);
		}
	} else if (sub == "menu") {
		I.on_hit(kMenuBtn, 0);
	}
}

Action Screen::take_action() {
	const Action a = p_->action;
	p_->action = Action::None;
	return a;
}

void Screen::select_node(int n) {
	Impl& I = *p_;
	if (!I.G() || n < 0 || size_t(n) >= I.G()->nodes.size()) return;
	I.sel_node = n;
	I.army_from = I.is_player_army(n) ? n : -1;
	I.tab = 0;
}
void Screen::select_country(const std::string& id) { select_node(p_->G() ? p_->G()->find_node(id) : -1); }
void Screen::set_zoom(float z) { p_->zoom = z; }

namespace {
void update_hot(Screen::Impl& I) {
	I.hot_id = 0;
	I.hot_arg = 0;
	for (size_t k = I.hits.size(); k-- > 0;) {
		if (I.hits[k].r.contains(int(I.mx), int(I.my))) {
			I.hot_id = I.hits[k].id;
			I.hot_arg = I.hits[k].arg;
			return;
		}
	}
}
}  // namespace

void Screen::draw(Bridge& b) {
	Impl& I = *p_;
	ctw::ConquestGame* g = I.G();
	if (g && I.map_screen) {
		if (I.s->resume_ai) {
			I.s->resume_ai = false;
			I.run_ai();
		}
		I.drain_messages();
		I.check_game_end();
	}
	I.hits.clear();
	if (!I.map_screen || !g) {
		I.draw_setup(b);
	} else {
		if (I.mx >= 0 && !I.down && I.vp.contains(int(I.mx), int(I.my)) && I.dialogs.empty() && !I.atk_open) I.hover_node = I.node_at_screen(I.mx, I.my);
		else if (I.mx < 0 || !I.vp.contains(int(I.mx), int(I.my))) I.hover_node = -1;
		I.draw_map(b);
	}
	if (I.map_screen && g) I.draw_dialogs(b);
	update_hot(I);
}

void Screen::mouse_move(Bridge& b, float x, float y) {
	(void)b;
	Impl& I = *p_;
	I.mx = x;
	I.my = y;
	if (I.down && I.map_screen && I.press_map) {
		if (!I.dragging && (std::fabs(x - I.down_x) > 4 || std::fabs(y - I.down_y) > 4)) I.dragging = true;
		if (I.dragging && I.zoom > 0) {
			I.cx = I.cx0 - (x - I.down_x) / I.zoom;
			I.cy = I.cy0 - (y - I.down_y) / I.zoom;
		}
	}
	update_hot(I);
}

void Screen::mouse_down(Bridge& b, float x, float y, int clicks, bool right) {
	(void)b;
	(void)clicks;
	Impl& I = *p_;
	I.mx = x;
	I.my = y;
	update_hot(I);
	if (right) {
		I.army_from = -1;
		I.target_card = -1;
		I.target_nodes.clear();
		return;
	}
	I.down = true;
	I.dragging = false;
	I.down_x = x;
	I.down_y = y;
	I.cx0 = I.cx;
	I.cy0 = I.cy;
	I.press_id = I.hot_id;
	I.press_arg = I.hot_arg;
	I.press_map = I.hot_id == kMap;
}

void Screen::mouse_drag(Bridge& b, float x, float y) { mouse_move(b, x, y); }

void Screen::mouse_up(Bridge& b, float x, float y) {
	Impl& I = *p_;
	I.mx = x;
	I.my = y;
	update_hot(I);
	const bool was = I.down;
	I.down = false;
	if (!was) return;
	if (I.dragging) {
		I.dragging = false;
		return;
	}
	if (I.hot_id == I.press_id && I.hot_arg == I.press_arg && I.hot_id != kNone) {
		if (I.hot_id == kMap) {
			if (I.G()) I.on_map_click(I.node_at_screen(x, y));
		} else {
			b.click();
			I.on_hit(I.hot_id, I.hot_arg);
		}
	}
}

void Screen::scroll(Bridge& b, float dy, float x, float y) {
	(void)b;
	Impl& I = *p_;
	if (!I.map_screen || !I.G()) return;
	if (I.panel.contains(int(x), int(y))) {
		if (I.tab == 3) I.news_scroll = std::max(0, I.news_scroll - int(dy));
		return;
	}
	if (I.zoom <= 0) return;
	const float f = dy > 0 ? 1.12f : 1 / 1.12f;
	const float wx = I.to_mx(x), wy = I.to_my(y);
	I.zoom = std::clamp(I.zoom * f, 0.05f, 3.0f);
	I.cx = wx - (x - float(I.vp.l) - float(I.vp.w()) / 2) / I.zoom;
	I.cy = wy - (y - float(I.vp.t) - float(I.vp.h()) / 2) / I.zoom;
}

void Screen::key(Bridge& b, const std::string& chars, int keycode, bool shift) {
	(void)b;
	Impl& I = *p_;
	(void)shift;
	if (keycode == 53) {  // Esc
		if (I.atk_open) {
			if (!I.atk_defense) I.atk_open = false;
		} else if (!I.dialogs.empty()) {
			I.on_hit(kDialog, int(I.dialogs.front().buttons.size()) - 1);
		} else if (I.saves_open) {
			I.saves_open = false;
		} else if (I.target_card >= 0) {
			I.target_card = -1;
			I.target_nodes.clear();
		} else if (I.army_from >= 0 || I.sel_node >= 0) {
			I.army_from = -1;
			I.sel_node = -1;
		} else {
			I.action = Action::Back;
		}
		return;
	}
	if (!I.map_screen) return;
	if (keycode == 36 && !I.dialogs.empty()) {  // Return
		I.on_hit(kDialog, 0);
		return;
	}
	if (chars == "+" || chars == "=") I.zoom = std::min(3.0f, I.zoom * 1.25f);
	else if (chars == "-") I.zoom = std::max(0.05f, I.zoom / 1.25f);
	else if (chars == "f") I.zoom = 0;
	else if (keycode == 123) I.cx -= 60 / std::max(0.1f, I.zoom);
	else if (keycode == 124) I.cx += 60 / std::max(0.1f, I.zoom);
	else if (keycode == 126) I.cy -= 60 / std::max(0.1f, I.zoom);
	else if (keycode == 125) I.cy += 60 / std::max(0.1f, I.zoom);
}

}  // namespace ctwui
