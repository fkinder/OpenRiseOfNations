#include "ui.hpp"

#include <unistd.h>

#import <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "assets/chunk.hpp"
#include "assets/strings.hpp"
#include "assets/tga.hpp"

namespace ron::metal {

namespace {

const char* kUiShader = R"(
#include <metal_stdlib>
using namespace metal;
struct UiVert { packed_float2 pos; packed_float2 uv; uint argb; };
struct VOut { float4 pos [[position]]; float2 uv; float4 color; };
vertex VOut ui_vertex(uint vid [[vertex_id]], const device UiVert* v [[buffer(0)]], constant float4& view [[buffer(1)]]) {
	UiVert x = v[vid];
	VOut o;
	o.pos = float4(float(x.pos[0]) / view.x * 2.0 - 1.0, 1.0 - float(x.pos[1]) / view.y * 2.0, 0.0, 1.0);
	o.uv = float2(x.uv[0], x.uv[1]) * view.zw;
	uint c = x.argb;
	o.color = float4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) / 255.0;
	return o;
}
struct UiFrag { int mode; int test; float ref; float pad; };
fragment float4 ui_fragment(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],
                            constant UiFrag& f [[buffer(0)]]) {
	float4 tx = t.sample(s, in.uv);
	float4 d = in.color;
	float3 c;
	float a;
	switch (f.mode) {
		case 0: c = tx.rgb * d.rgb; a = tx.a * d.a; break;               // MODULATE
		case 1: c = saturate(2.0 * tx.rgb * d.rgb); a = tx.a * d.a; break; // MODULATE2X
		case 2: c = saturate(2.0 * tx.rgb * d.rgb); a = tx.a; break;       // MODULATE2X, alpha SELECTARG1(texture)
		case 3: c = d.rgb; a = d.a; break;                                 // SELECTARG2 (diffuse)
		default: c = tx.rgb; a = tx.a * d.a; break;                        // colour SELECTARG1(texture)
	}
	if (f.test == 1 && a <= f.ref) discard_fragment();
	if (f.test == 2 && a < f.ref) discard_fragment();
	return float4(c, a);
}
)";

struct TypeInfo {
	int pipe;  // 0 none, 1 alpha, 2 alphasat, 3 add, 4 multiply
	int mode;
	int test;  // 0 none, 1 greater, 2 greater-equal
	float ref;
};

TypeInfo type_info(UiType t) {
	switch (t) {
		case UiType::Normal: return {0, 0, 0, 0};
		case UiType::Alpha:
		case UiType::Alpha2: return {1, 0, 1, 0};
		case UiType::ColorAlpha: return {2, 0, 1, 0};
		case UiType::Highlight: return {0, 1, 0, 0};
		case UiType::AlphaHighlight: return {1, 2, 1, 0};
		case UiType::ColorBlend: return {1, 3, 0, 0};
		case UiType::ColorAlphaMod: return {1, 0, 0, 0};
		case UiType::AddFB: return {3, 4, 0, 0};
		case UiType::Text: return {1, 0, 2, 8.0f / 255.0f};
		case UiType::Multiply: return {4, 0, 0, 0};
	}
	return {1, 0, 1, 0};
}

id<MTLTexture> make_rgba(id<MTLDevice> device, int w, int h, const uint8_t* rgba) {
	MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                                             width:NSUInteger(std::max(w, 1))
	                                                                            height:NSUInteger(std::max(h, 1))
	                                                                         mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> t = [device newTextureWithDescriptor:d];
	if (rgba) [t replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(w), NSUInteger(h)) mipmapLevel:0 withBytes:rgba bytesPerRow:NSUInteger(w) * 4];
	return t;
}

}  // namespace

// ---------------------------------------------------------------- fonts

bool UiFont::init(const GameFiles& files, const FontEntry& entry, float scale, id<MTLDevice> device) {
	const std::string path = files.resolve(entry.file).empty() ? files.resolve("Data/Fonts/arial.ttf") : files.resolve(entry.file);
	if (path.empty()) return false;
	entry_ = entry;
	scale_ = scale;
	device_ = device;
	Bytes bytes = read_file(path);
	CFDataRef data = CFDataCreate(nullptr, bytes.data(), CFIndex(bytes.size()));
	face_ = parse_ttf(std::move(bytes));
	metrics_ = ttf_metrics(face_, entry.size);
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	CGFontRef cg = CGFontCreateWithDataProvider(provider);
	CGDataProviderRelease(provider);
	CFRelease(data);
	if (!cg) return false;
	ct_font_ = (void*)CTFontCreateWithGraphicsFont(cg, CGFloat(entry.size * scale), nullptr, nullptr);
	CGFontRelease(cg);
	page_size_ = scale > 1.5f ? 512 : 256;
	return ct_font_ != nullptr;
}

int UiFont::advance(uint32_t cp) const {
	if (cp < 0x20) return 0;
	return ttf_advance(face_, entry_.size, cp) + ((entry_.bits & kFontOutline) ? 1 : 0);
}

const UiFont::Glyph& UiFont::glyph(uint32_t cp) {
	auto it = glyphs_.find(cp);
	if (it != glyphs_.end()) return it->second;
	Glyph g;
	CTFontRef font = (CTFontRef)ct_font_;
	UniChar chars[2];
	int nchars = 1;
	if (cp >= 0x10000) {
		chars[0] = UniChar(0xD800 + ((cp - 0x10000) >> 10));
		chars[1] = UniChar(0xDC00 + ((cp - 0x10000) & 0x3FF));
		nchars = 2;
	} else {
		chars[0] = UniChar(cp);
	}
	CGGlyph glyphs[2] = {0, 0};
	if (cp >= 0x20 && font && CTFontGetGlyphsForCharacters(font, chars, glyphs, nchars)) {
		const CGRect bb = CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, glyphs, nullptr, 1);
		if (bb.size.width > 0 && bb.size.height > 0) {
			// The bitmap's ink box with a 1-pixel margin; the ink's left edge goes to the cell's left edge (bitmap_left
			// dropped, D3D11GLFont / FontRenderManager::render_character_info), its top at Ascent - bitmap_top.
			const int left = int(std::floor(bb.origin.x)) - 1, bottom = int(std::floor(bb.origin.y)) - 1;
			const int right = int(std::ceil(bb.origin.x + bb.size.width)) + 1, top = int(std::ceil(bb.origin.y + bb.size.height)) + 1;
			const int w = right - left, h = top - bottom;
			std::vector<uint8_t> cov(size_t(w) * size_t(h), 0);
			CGContextRef ctx = CGBitmapContextCreate(cov.data(), size_t(w), size_t(h), 8, size_t(w), nullptr, kCGImageAlphaOnly);
			if (ctx) {
				CGContextSetShouldAntialias(ctx, true);
				CGContextSetAllowsFontSmoothing(ctx, false);
				CGContextSetShouldSmoothFonts(ctx, false);
				CGContextSetGrayFillColor(ctx, 1, 1);
				const CGPoint pos = CGPointMake(-left, -bottom);
				CTFontDrawGlyphs(font, glyphs, &pos, 1, ctx);
				CGContextRelease(ctx);
			}
			if (pen_x_ + w + 1 > page_size_) pen_x_ = 4, pen_y_ += row_h_ + 1, row_h_ = 0;
			if (pages_.empty() || pen_y_ + h + 1 > page_size_) {
				pages_.push_back({make_rgba(device_, page_size_, page_size_, nullptr), page_size_, page_size_});
				std::vector<uint8_t> clear(size_t(page_size_) * size_t(page_size_) * 4, 0);
				[pages_.back().tex replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(page_size_), NSUInteger(page_size_)) mipmapLevel:0
				                       withBytes:clear.data() bytesPerRow:NSUInteger(page_size_) * 4];
				pen_x_ = pen_y_ = 4, row_h_ = 0;
			}
			std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
			for (size_t i = 0; i < cov.size(); i++) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255, rgba[i * 4 + 3] = cov[i];
			[pages_.back().tex replaceRegion:MTLRegionMake2D(NSUInteger(pen_x_), NSUInteger(pen_y_), NSUInteger(w), NSUInteger(h)) mipmapLevel:0
			                       withBytes:rgba.data() bytesPerRow:NSUInteger(w) * 4];
			g.page = int(pages_.size()) - 1;
			g.x = pen_x_, g.y = pen_y_, g.w = w, g.h = h;
			g.top_px = int(std::lround(metrics_.ascent * scale_)) - top;
			pen_x_ += w + 1;
			row_h_ = std::max(row_h_, h);
		}
	}
	return glyphs_[cp] = g;
}

// ---------------------------------------------------------------- renderer

bool UiRenderer::init(id<MTLDevice> device, id<MTLCommandQueue> queue, MTLPixelFormat color, const GameFiles* files,
                      const IfaceLayout* layout, std::string& error) {
	device_ = device;
	queue_ = queue;
	files_ = files;
	layout_ = layout;
	itxt_ = std::make_unique<InterfaceTxt>(load_interface_txt(*files));
	NSError* err = nil;
	id<MTLLibrary> lib = [device newLibraryWithSource:@(kUiShader) options:nil error:&err];
	if (!lib) {
		error = std::string("UI shader: ") + err.localizedDescription.UTF8String;
		return false;
	}
	for (int i = 0; i < 5; i++) {
		MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = [lib newFunctionWithName:@"ui_vertex"];
		pd.fragmentFunction = [lib newFunctionWithName:@"ui_fragment"];
		pd.colorAttachments[0].pixelFormat = color;
		MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
		ca.blendingEnabled = i != 0;
		const MTLBlendFactor src[5] = {MTLBlendFactorOne, MTLBlendFactorSourceAlpha, MTLBlendFactorSourceAlphaSaturated,
		                               MTLBlendFactorSourceAlpha, MTLBlendFactorZero};
		const MTLBlendFactor dst[5] = {MTLBlendFactorZero, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
		                               MTLBlendFactorOne, MTLBlendFactorSourceColor};
		ca.sourceRGBBlendFactor = src[i];
		ca.destinationRGBBlendFactor = dst[i];
		ca.sourceAlphaBlendFactor = MTLBlendFactorOne;  // d3dgl: alpha ONE / ZERO
		ca.destinationAlphaBlendFactor = MTLBlendFactorZero;
		pipes_[i] = [device newRenderPipelineStateWithDescriptor:pd error:&err];
		if (!pipes_[i]) {
			error = std::string("UI pipeline: ") + err.localizedDescription.UTF8String;
			return false;
		}
	}
	MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
	sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
	linear_ = [device newSamplerStateWithDescriptor:sd];
	sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
	nearest_ = [device newSamplerStateWithDescriptor:sd];
	const uint8_t white[4] = {255, 255, 255, 255};
	white_ = {make_rgba(device, 1, 1, white), 1, 1};
	return true;
}

void UiRenderer::set_skin(const std::string& code) {
	if (code == skin_) return;
	skin_ = code;
	for (const char* id : {"TEX_CIV", "TEX_LIBRARY_BACK", "TEX_LIBRARY_BACK2"}) textures_.erase(id);
}

const UiTexture* UiRenderer::texture(const std::string& id) {
	if (id.empty() || id == "TEX_NONE") return nullptr;
	auto it = textures_.find(id);
	if (it != textures_.end()) return it->second.tex ? &it->second : nullptr;
	auto ex = extra_texloads_.find(id);
	std::string file = ex != extra_texloads_.end() ? ex->second : layout_->texload_file(id);
	if (id == "TEX_CIV" || id == "TEX_LIBRARY_BACK" || id == "TEX_LIBRARY_BACK2") file = iface_skin_file(file, skin_);
	UiTexture t;
	const std::string key = to_lower(file);
	auto ft = file_textures_.find(key);
	if (ft != file_textures_.end()) {
		t = ft->second;
	} else if (!file.empty() && !files_->resolve(file).empty()) {
		try {
			const Image img = parse_tga(read_file(files_->resolve(file)));
			t = {make_rgba(device_, img.width, img.height, img.rgba.data()), img.width, img.height};
		} catch (const std::exception& e) {
			std::fprintf(stderr, "UI atlas %s: %s\n", file.c_str(), e.what());
		}
		file_textures_[key] = t;
	}
	textures_[id] = t;
	return t.tex ? &textures_[id] : nullptr;
}

void UiRenderer::add_texload(const std::string& id, const std::string& file) {
	extra_texloads_[id] = normalize_game_path(file);
	textures_.erase(id);
}

void UiRenderer::release(const std::string& id) {
	auto ex = extra_texloads_.find(id);
	const std::string file = ex != extra_texloads_.end() ? ex->second : layout_->texload_file(id);
	textures_.erase(id);
	file_textures_.erase(to_lower(file));
}

void UiRenderer::set_texture(const std::string& key, id<MTLTexture> tex, int w, int h) { textures_[key] = {tex, w, h}; }

const UiTexture* UiRenderer::file_texture(const std::string& game_path, std::string* id) {
	const std::string key = "file:" + to_lower(game_path);
	if (id) *id = key;
	auto it = textures_.find(key);
	if (it != textures_.end()) return it->second.tex ? &it->second : nullptr;
	UiTexture t;
	std::string path = files_->resolve(game_path);
	if (path.empty() && !game_path.empty() && game_path[0] == '/' && access(game_path.c_str(), R_OK) == 0) path = game_path;  // the app's own resources
	if (!path.empty()) {
		try {
			const Image img = parse_tga(read_file(path));
			t = {make_rgba(device_, img.width, img.height, img.rgba.data()), img.width, img.height};
		} catch (const std::exception& e) {
			std::fprintf(stderr, "UI texture %s: %s\n", game_path.c_str(), e.what());
		}
	}
	textures_[key] = t;
	return t.tex ? &textures_[key] : nullptr;
}

void UiRenderer::begin(int ui_w, int ui_h, float scale, int font_res_w) {
	w_ = ui_w, h_ = ui_h, scale_ = scale;
	depth = 0;
	quads_.clear();
	seq_ = 0;
	const int res_w = font_res_w > 0 ? font_res_w : ui_w;
	if (fonts_res_width_ != res_w || fonts_scale_ != scale) {
		const FontsXml fx = load_fonts_xml(*files_, Language::English, res_w);
		if (fx.res != fonts_xml_.res || fonts_scale_ != scale || fonts_.empty()) {
			fonts_xml_ = fx;
			fonts_.clear();
			fonts_.resize(fonts_xml_.fonts.size());
		}
		fonts_res_width_ = res_w;
		fonts_scale_ = scale;
	}
}

void UiRenderer::push(const UiTexture* tex, const float xy[8], const float uv[8], uint32_t c, UiType t, bool point, bool text) {
	Quad q;
	q.depth = depth;
	q.seq = seq_++;
	q.text = text;
	q.tex = tex ? tex : &white_;
	q.type = t;
	q.point = point;
	for (int i = 0; i < 4; i++) q.v[i] = {xy[i * 2], xy[i * 2 + 1], tex ? uv[i * 2] : 0.5f, tex ? uv[i * 2 + 1] : 0.5f, c};
	quads_.push_back(q);
}

void UiRenderer::push_rect(const UiTexture* tex, float sx, float sy, float sw, float sh, float x, float y, float w, float h,
                           uint32_t c, UiType t, bool text) {
	if (w <= 0 || h <= 0) return;
	const float xy[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
	const float uv[8] = {sx, sy, sx + sw, sy, sx + sw, sy + sh, sx, sy + sh};
	// Unstretched pieces are point-sampled so that integer backing scales double pixels exactly.
	const bool point = !smooth_ && std::fabs(w - sw) < 0.01f && std::fabs(h - sh) < 0.01f;
	if (!point && !text && tex && sw >= 2 && sh >= 2) {
		// Bilinear: keep the samples inside the piece so the atlas neighbours (guide lines, other pieces) don't bleed in.
		const float uvi[8] = {sx + 0.5f, sy + 0.5f, sx + sw - 0.5f, sy + 0.5f, sx + sw - 0.5f, sy + sh - 0.5f, sx + 0.5f, sy + sh - 0.5f};
		push(tex, xy, uvi, c, t, false, text);
		return;
	}
	push(tex, xy, uv, c, t, point || text, text);
}

void UiRenderer::add(const IfacePiece& p, float x, float y, float w, float h, uint32_t c, UiType t) {
	if (!p.valid()) return;
	add_sub(p, 0, 0, float(p.width), float(p.height), x, y, w > 0 ? w : float(p.width), h > 0 ? h : float(p.height), c, t);
}

void UiRenderer::add_sub(const IfacePiece& p, float sx, float sy, float sw, float sh, float x, float y, float w, float h, uint32_t c,
                         UiType t) {
	const UiTexture* tex = texture(p.texture);
	if (!tex) return;
	push_rect(tex, float(p.left) + sx, float(p.top) + sy, sw, sh, x, y, w, h, c, t);
}

void UiRenderer::add_quad(const std::string& tex_id, const float xy[8], const float uv[8], uint32_t c, UiType t, bool point) {
	const UiTexture* tex = tex_id.empty() ? nullptr : texture(tex_id);
	if (!tex_id.empty() && !tex) return;
	push(tex, xy, uv, c, t, point, false);
}

void UiRenderer::fill(float x, float y, float w, float h, uint32_t c, UiType t) {
	if (w <= 0 || h <= 0) return;
	const float xy[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
	const float uv[8] = {};
	push(nullptr, xy, uv, c, t, true, false);
}

void UiRenderer::frame(float x, float y, float w, float h, uint32_t c) {
	fill(x, y, w, 1, c);
	fill(x, y + h - 1, w, 1, c);
	fill(x, y + 1, 1, h - 2, c);
	fill(x + w - 1, y + 1, 1, h - 2, c);
}

void UiRenderer::tile(const IfacePiece& p, const IfaceRect& r, uint32_t c, UiType t) {
	if (!p.valid() || p.width <= 0 || p.height <= 0) return;
	const UiTexture* tex = texture(p.texture);
	if (!tex) return;
	for (int y = r.t; y < r.b; y += p.height) {
		for (int x = r.l; x < r.r; x += p.width) {
			const int w = std::min(p.width, r.r - x), h = std::min(p.height, r.b - y);
			push_rect(tex, float(p.left), float(p.top), float(w), float(h), float(x), float(y), float(w), float(h), c, t);
		}
	}
}

void UiRenderer::border(const IfaceBorder& b, const IfaceRect& r, uint32_t c, UiType t) {
	const IfacePiece *c0 = &b.slot[0], *c1 = &b.slot[1], *c2 = &b.slot[2], *c3 = &b.slot[3];
	const IfacePiece *e0 = &b.slot[4], *e1 = &b.slot[5], *e2 = &b.slot[6], *e3 = &b.slot[7];
	auto wd = [](const IfacePiece* p) { return p->valid() ? p->width : 0; };
	auto ht = [](const IfacePiece* p) { return p->valid() ? p->height : 0; };
	if (e0->valid()) tile(*e0, {r.l + wd(c0), r.r - wd(c1), r.t, r.t + ht(e0)}, c, t);
	if (e2->valid()) tile(*e2, {r.l + wd(c3), r.r - wd(c2), r.b - ht(e2), r.b}, c, t);
	if (e3->valid()) tile(*e3, {r.l, r.l + wd(e3), r.t + ht(c0), r.b - ht(c3)}, c, t);
	if (e1->valid()) tile(*e1, {r.r - wd(e1), r.r, r.t + ht(c1), r.b - ht(c2)}, c, t);
	if (c0->valid()) add(*c0, float(r.l), float(r.t), 0, 0, c, t);
	if (c1->valid()) add(*c1, float(r.r - wd(c1)), float(r.t), 0, 0, c, t);
	if (c2->valid()) add(*c2, float(r.r - wd(c2)), float(r.b - ht(c2)), 0, 0, c, t);
	if (c3->valid()) add(*c3, float(r.l), float(r.b - ht(c3)), 0, 0, c, t);
}

void UiRenderer::expander(const IfacePiece& l, const IfacePiece& m, const IfacePiece& r, const IfaceRect& rc, uint32_t c, UiType t) {
	const int lw = l.valid() ? l.width : 0, rw = r.valid() ? r.width : 0;
	add(l, float(rc.l), float(rc.t), float(lw), float(rc.h()), c, t);
	add(m, float(rc.l + lw), float(rc.t), float(rc.w() - lw - rw), float(rc.h()), c, t);
	add(r, float(rc.r - rw), float(rc.t), float(rw), float(rc.h()), c, t);
}

void UiRenderer::vexpander(const IfacePiece& top, const IfacePiece& mid, const IfacePiece& bot, const IfaceRect& rc, uint32_t c,
                           UiType t) {
	const int th = top.valid() ? top.height : 0, bh = bot.valid() ? bot.height : 0;
	add(top, float(rc.l), float(rc.t), float(rc.w()), float(th), c, t);
	add(mid, float(rc.l), float(rc.t + th), float(rc.w()), float(rc.h() - th - bh), c, t);
	add(bot, float(rc.l), float(rc.b - bh), float(rc.w()), float(bh), c, t);
}

int UiRenderer::write_digits(const std::string& s, float x, float y, uint32_t c, int set) {
	float pen = x;
	for (char ch : s) {
		const int i = iface_digit_index(ch);
		if (i < 0) continue;
		const IfacePiece& p = itxt_->digit[set & 1][i];
		add(p, pen, y, 0, 0, c, UiType::Alpha);
		pen += float(p.width);
	}
	return int(pen - x);
}

int UiRenderer::digits_width(const std::string& s, int set) const {
	int w = 0;
	for (char ch : s) {
		const int i = iface_digit_index(ch);
		if (i >= 0) w += itxt_->digit[set & 1][i].width;
	}
	return w;
}

int UiRenderer::digits_height(int set) const { return itxt_->digit[set & 1][0].height; }

int UiRenderer::font(int size, int style) {
	const int i = fonts_xml_.pick(size, style);
	if (i < 0) return -1;
	if (!fonts_[size_t(i)]) {
		auto f = std::make_unique<UiFont>();
		if (!f->init(*files_, fonts_xml_.fonts[size_t(i)], scale_, device_)) return -1;
		fonts_[size_t(i)] = std::move(f);
	}
	return i;
}

int UiRenderer::font_tag(const std::string& tag) {
	const int i = fonts_xml_.pick_tag(tag);
	if (i < 0) return -1;
	return font(fonts_xml_.fonts[size_t(i)].size, fonts_xml_.fonts[size_t(i)].bits);
}

const FontMetrics* UiRenderer::font_metrics(int f) const {
	return f >= 0 && size_t(f) < fonts_.size() && fonts_[size_t(f)] ? &fonts_[size_t(f)]->metrics() : nullptr;
}

int UiRenderer::text_width(int f, const std::string& utf8) {
	if (f < 0 || !fonts_[size_t(f)]) return 0;
	int w = 0;
	for (char32_t cp : utf8_to_utf32(utf8)) w += fonts_[size_t(f)]->advance(cp);
	return w;
}

void UiRenderer::text(int f, const std::string& utf8, float x, float y, uint32_t c, uint32_t shadow, int sdx, int sdy) {
	if (f < 0 || !fonts_[size_t(f)]) return;
	UiFont& font = *fonts_[size_t(f)];
	const float s = font.scale();
	auto run = [&](float ox, float oy, uint32_t col) {
		float pen = ox;
		for (char32_t cp : utf8_to_utf32(utf8)) {
			const UiFont::Glyph& g = font.glyph(cp);
			if (g.page >= 0) {
				// The bitmap's 1-pixel margin column sits one scaled pixel left of the pen, so the ink starts at the pen.
				push_rect(&font.pages()[size_t(g.page)], float(g.x), float(g.y), float(g.w), float(g.h), pen - 1.0f / s,
				          oy + float(g.top_px) / s, float(g.w) / s, float(g.h) / s, col, UiType::Text, true);
			}
			pen += float(font.advance(cp));
		}
	};
	if (shadow >> 24) run(x + float(sdx), y + float(sdy), shadow);
	run(x, y, c);
}

void UiRenderer::text_scaled(int f, const std::string& utf8, float x, float y, float sc, uint32_t c, uint32_t shadow) {
	if (f < 0 || !fonts_[size_t(f)]) return;
	UiFont& font = *fonts_[size_t(f)];
	const float s = font.scale();
	auto run = [&](float ox, float oy, uint32_t col) {
		float pen = 0;
		for (char32_t cp : utf8_to_utf32(utf8)) {
			const UiFont::Glyph& g = font.glyph(cp);
			if (g.page >= 0) {
				const float gx = ox + (pen - 1.0f / s) * sc, gy = oy + float(g.top_px) / s * sc;
				const float xy[8] = {gx, gy, gx + float(g.w) / s * sc, gy, gx + float(g.w) / s * sc, gy + float(g.h) / s * sc, gx,
				                     gy + float(g.h) / s * sc};
				const float uv[8] = {float(g.x), float(g.y), float(g.x + g.w), float(g.y), float(g.x + g.w), float(g.y + g.h), float(g.x),
				                     float(g.y + g.h)};
				push(&font.pages()[size_t(g.page)], xy, uv, col, UiType::Text, false, true);
			}
			pen += float(font.advance(cp));
		}
	};
	if (shadow >> 24) run(x + sc, y + sc, shadow);
	run(x, y, c);
}

void UiRenderer::text_centered(int f, const std::string& utf8, const IfaceRect& r, uint32_t c, uint32_t shadow) {
	const FontMetrics* m = font_metrics(f);
	if (!m) return;
	const int w = text_width(f, utf8);
	text(f, utf8, float(r.l + (r.w() - w) / 2), float(r.t + (r.h() - m->font_height) / 2), c, shadow);
}

id<MTLRenderPipelineState> UiRenderer::pipeline(UiType t) { return pipes_[type_info(t).pipe]; }

void UiRenderer::encode(id<MTLRenderCommandEncoder> enc) {
	if (quads_.empty()) return;
	std::stable_sort(quads_.begin(), quads_.end(), [](const Quad& a, const Quad& b) {
		if (a.depth != b.depth) return a.depth < b.depth;
		if (a.text != b.text) return !a.text;
		return a.seq < b.seq;
	});
	std::vector<Vert> verts;
	verts.reserve(quads_.size() * 6);
	for (const Quad& q : quads_) {
		const int order[6] = {0, 1, 2, 0, 2, 3};
		for (int k : order) verts.push_back(q.v[k]);
	}
	id<MTLBuffer> vb = [device_ newBufferWithBytes:verts.data() length:verts.size() * sizeof(Vert) options:MTLResourceStorageModeShared];
	[enc setCullMode:MTLCullModeNone];
	[enc setVertexBuffer:vb offset:0 atIndex:0];
	size_t i = 0;
	while (i < quads_.size()) {
		size_t j = i + 1;
		while (j < quads_.size() && quads_[j].tex == quads_[i].tex && quads_[j].type == quads_[i].type && quads_[j].point == quads_[i].point) j++;
		const Quad& q = quads_[i];
		const TypeInfo ti = type_info(q.type);
		struct {
			int mode, test;
			float ref, pad;
		} frag{ti.mode, ti.test, ti.ref, 0};
		const float view[4] = {float(w_), float(h_), 1.0f / float(q.tex->w), 1.0f / float(q.tex->h)};
		[enc setRenderPipelineState:pipeline(q.type)];
		[enc setVertexBytes:view length:sizeof view atIndex:1];
		[enc setFragmentBytes:&frag length:sizeof frag atIndex:0];
		[enc setFragmentTexture:q.tex->tex atIndex:0];
		[enc setFragmentSamplerState:(q.point ? nearest_ : linear_) atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:i * 6 vertexCount:(j - i) * 6];
		i = j;
	}
	quads_.clear();
}

}  // namespace ron::metal
