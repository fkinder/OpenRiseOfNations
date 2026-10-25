#include "renderer.hpp"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace ron::metal {

namespace {

const char* kShaderSource =
#include "combiner_shader.inc"
	;

MTLBlendFactor blend_factor(BlendMode m) {
	switch (m) {
		case BlendMode::Zero: return MTLBlendFactorZero;
		case BlendMode::One: return MTLBlendFactorOne;
		case BlendMode::SrcColor: return MTLBlendFactorSourceColor;
		case BlendMode::InvSrcColor: return MTLBlendFactorOneMinusSourceColor;
		case BlendMode::SrcAlpha: return MTLBlendFactorSourceAlpha;
		case BlendMode::InvSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
		case BlendMode::DestAlpha: return MTLBlendFactorDestinationAlpha;
		case BlendMode::InvDestAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
		case BlendMode::DestColor: return MTLBlendFactorDestinationColor;
		case BlendMode::InvDestColor: return MTLBlendFactorOneMinusDestinationColor;
		case BlendMode::SrcAlphaSat: return MTLBlendFactorSourceAlphaSaturated;
		default: return MTLBlendFactorOne;
	}
}

MTLCompareFunction compare(CompareFunc f) {
	static const MTLCompareFunction table[] = {MTLCompareFunctionNever,   MTLCompareFunctionLess,
	                                           MTLCompareFunctionEqual,   MTLCompareFunctionLessEqual,
	                                           MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
	                                           MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
	return table[size_t(f) & 7];
}

MTLSamplerAddressMode address(TexAddress a) {
	switch (a) {
		case TexAddress::Mirror: return MTLSamplerAddressModeMirrorRepeat;
		case TexAddress::Clamp: return MTLSamplerAddressModeClampToEdge;
		case TexAddress::Border: return MTLSamplerAddressModeClampToBorderColor;
		case TexAddress::MirrorOnce: return MTLSamplerAddressModeMirrorClampToEdge;
		default: return MTLSamplerAddressModeRepeat;
	}
}

bool uses_texture(const TextureStage& s) {
	const bool color = s.color_op != TextureOp::Disable && (s.color_arg1 == TextureArg::Texture || s.color_arg2 == TextureArg::Texture);
	const bool alpha = s.alpha_op != TextureOp::Disable && (s.alpha_arg1 == TextureArg::Texture || s.alpha_arg2 == TextureArg::Texture);
	return color || alpha || s.color_op == TextureOp::BlendTextureAlpha || s.alpha_op == TextureOp::BlendTextureAlpha;
}

// TS_STAGES as d3dgl computes it: 0 if stage 0 is fully disabled, 2 if stage 1 is also active, else 1.
int active_stages(const RenderState& rs) {
	auto off = [](const TextureStage& s) { return s.color_op == TextureOp::Disable && s.alpha_op == TextureOp::Disable; };
	if (off(rs.stage[0])) return 0;
	return off(rs.stage[1]) ? 1 : 2;
}

}  // namespace

simd_float4 unpack_abgr(uint32_t c) {
	return simd_make_float4(float(c & 0xFF), float((c >> 8) & 0xFF), float((c >> 16) & 0xFF), float(c >> 24)) / 255.0f;
}

DrawUniforms draw_uniforms(const RenderState& rs, simd_float4x4 world) {
	DrawUniforms d{};
	d.world = world;
	d.tfactor = unpack_abgr(rs.tfactor);
	d.diffuse = simd_make_float4(1, 1, 1, 1);
	d.specular = simd_make_float4(0, 0, 0, 0);
	d.mat_ambient = simd_make_float4(1, 1, 1, 1);
	d.mat_diffuse = simd_make_float4(1, 1, 1, 1);
	d.alpha_ref = float(rs.alpha_ref) / 255.0f;
	d.depth_bias = float(rs.depth_bias);
	d.tl_alpha = 1.0f;
	return d;
}

bool StateCache::init(id<MTLDevice> device, MTLPixelFormat color, MTLPixelFormat depth, std::string& error) {
	device_ = device;
	color_ = color;
	depth_ = depth;
	NSError* err = nil;
	library_ = [device newLibraryWithSource:@(kShaderSource) options:nil error:&err];
	if (!library_) {
		error = std::string("Metal shader compile failed: ") + err.localizedDescription.UTF8String;
		return false;
	}
	return true;
}

id<MTLRenderPipelineState> StateCache::pipeline(const RenderState& rs, VertexKind kind, bool no_hdr) {
	const int stages = active_stages(rs);
	const TextureStage& a = rs.stage[0];
	const TextureStage& b = rs.stage[1];
	const int alpha_func = rs.has(rs_enable::kAlphaTest) ? int(rs.alpha_func) : int(CompareFunc::Always);
	const bool lighting = rs.has(rs_enable::kLighting), tl_fake = rs.has(rs_enable::kTLFakeLight);
	const bool tex0 = stages >= 1 && uses_texture(a), tex1 = stages >= 2 && uses_texture(b);
	const bool blend = rs.has(rs_enable::kAlphaBlend);
	const int ints[13] = {stages,
	                      int(a.color_op), int(a.color_arg1), int(a.color_arg2), int(a.alpha_op), int(a.alpha_arg1), int(a.alpha_arg2),
	                      int(b.color_op), int(b.color_arg1), int(b.color_arg2), int(b.alpha_op), int(b.alpha_arg1), int(b.alpha_arg2)};
	// Every field is a small enum or a flag: one byte each is a key without the snprintf this used to build per draw (a few percent of the frame).
	std::array<uint8_t, 24> key{};
	for (int i = 0; i < 13; i++) key[size_t(i)] = uint8_t(ints[i]);
	const int rest[11] = {alpha_func, lighting, tl_fake, no_hdr, tex0, tex1, blend, int(rs.src_blend), int(rs.dst_blend), int(kind), 0};
	for (int i = 0; i < 11; i++) key[size_t(13 + i)] = uint8_t(rest[i]);
	auto it = pipelines_.find(key);
	if (it != pipelines_.end()) return it->second;

	MTLFunctionConstantValues* fc = [MTLFunctionConstantValues new];
	for (NSUInteger i = 0; i < 13; i++) [fc setConstantValue:&ints[i] type:MTLDataTypeInt atIndex:i];
	[fc setConstantValue:&alpha_func type:MTLDataTypeInt atIndex:13];
	[fc setConstantValue:&lighting type:MTLDataTypeBool atIndex:14];
	[fc setConstantValue:&tl_fake type:MTLDataTypeBool atIndex:15];
	[fc setConstantValue:&no_hdr type:MTLDataTypeBool atIndex:16];
	[fc setConstantValue:&tex0 type:MTLDataTypeBool atIndex:17];
	[fc setConstantValue:&tex1 type:MTLDataTypeBool atIndex:18];
	NSError* err = nil;
	MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
	NSString* vfn = kind == VertexKind::Colored ? @"rs_vertex_color" : kind == VertexKind::Colored2 ? @"rs_vertex_color2" : @"rs_vertex";
	pd.vertexFunction = [library_ newFunctionWithName:vfn];
	pd.fragmentFunction = [library_ newFunctionWithName:@"rs_fragment" constantValues:fc error:&err];
	pd.colorAttachments[0].pixelFormat = color_;
	pd.depthAttachmentPixelFormat = depth_;
	MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
	ca.blendingEnabled = blend;
	if (blend) {
		// d3dgl: RGB uses the state's factors; alpha is always ONE / ZERO.
		ca.sourceRGBBlendFactor = blend_factor(rs.src_blend);
		ca.destinationRGBBlendFactor = blend_factor(rs.dst_blend);
		ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
		ca.destinationAlphaBlendFactor = MTLBlendFactorZero;
	}
	id<MTLRenderPipelineState> p = pd.fragmentFunction ? [device_ newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!p) std::fprintf(stderr, "pipeline for render state '%s' failed: %s\n", rs.name, err.localizedDescription.UTF8String);
	pipelines_[key] = p;
	return p;
}

id<MTLDepthStencilState> StateCache::depth_state(const RenderState& rs) {
	const bool test = rs.has(rs_enable::kDepthTest);
	const bool write = test && rs.has(rs_enable::kDepthWrite);
	const uint32_t key = (test ? uint32_t(rs.depth_func) : 7u) | (write ? 0x10u : 0u);
	auto it = depth_states_.find(key);
	if (it != depth_states_.end()) return it->second;
	MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
	dd.depthCompareFunction = test ? compare(rs.depth_func) : MTLCompareFunctionAlways;
	dd.depthWriteEnabled = write;
	return depth_states_[key] = [device_ newDepthStencilStateWithDescriptor:dd];
}

id<MTLSamplerState> StateCache::sampler(TexAddress u, TexAddress v, bool point) {
	const uint32_t key = uint32_t(u) | uint32_t(v) << 4 | (point ? 0x100u : 0u);
	auto it = samplers_.find(key);
	if (it != samplers_.end()) return it->second;
	// d3dgl ignores the state's filters: 16x anisotropic, or point sampling when the state asks for it.
	MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
	const MTLSamplerMinMagFilter f = point ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
	sd.minFilter = sd.magFilter = f;
	sd.mipFilter = point ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
	sd.maxAnisotropy = point ? 1 : 16;
	sd.sAddressMode = address(u);
	sd.tAddressMode = address(v);
	sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
	return samplers_[key] = [device_ newSamplerStateWithDescriptor:sd];
}

void StateCache::bind(id<MTLRenderCommandEncoder> enc, const RenderState& rs, VertexKind kind, bool no_hdr, bool flip_cull) {
	[enc setRenderPipelineState:pipeline(rs, kind, no_hdr)];
	[enc setDepthStencilState:depth_state(rs)];
	[enc setFrontFacingWinding:MTLWindingCounterClockwise];
	MTLCullMode cull = MTLCullModeNone;
	if (rs.cull == CullMode::CW) cull = flip_cull ? MTLCullModeFront : MTLCullModeBack;
	if (rs.cull == CullMode::CCW) cull = flip_cull ? MTLCullModeBack : MTLCullModeFront;
	[enc setCullMode:cull];
	[enc setTriangleFillMode:rs.fill == FillMode::Wireframe || rs.fill == FillMode::Point ? MTLTriangleFillModeLines
	                                                                                        : MTLTriangleFillModeFill];
	const bool point = rs.has(rs_enable::kPointSampling);
	[enc setFragmentSamplerState:sampler(rs.stage[0].address_u, rs.stage[0].address_v, point) atIndex:0];
	[enc setFragmentSamplerState:sampler(rs.stage[1].address_u, rs.stage[1].address_v, point) atIndex:1];
}

bool PostProcess::init(id<MTLDevice> device, id<MTLLibrary> library, MTLPixelFormat output, std::string& error) {
	device_ = device;
	auto make = [&](NSString* frag, std::initializer_list<MTLPixelFormat> formats) -> id<MTLRenderPipelineState> {
		MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = [library newFunctionWithName:@"quad_vertex"];
		pd.fragmentFunction = [library newFunctionWithName:frag];
		NSUInteger i = 0;
		for (MTLPixelFormat f : formats) pd.colorAttachments[i++].pixelFormat = f;
		NSError* err = nil;
		id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:pd error:&err];
		if (!p && error.empty()) error = std::string("post pipeline ") + frag.UTF8String + ": " + err.localizedDescription.UTF8String;
		return p;
	};
	luminance_ = make(@"luminance_fragment", {MTLPixelFormatR16Float});
	tonemap_ = make(@"tonemap_fragment", {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGB10A2Unorm});
	blur_ = make(@"blur_fragment", {MTLPixelFormatRGB10A2Unorm});
	blend_ = make(@"blend_fragment", {output});
	copy_ = make(@"copy_fragment", {output});
	return luminance_ && tonemap_ && blur_ && blend_ && copy_;
}

void PostProcess::ensure(NSUInteger w, NSUInteger h) {
	auto make = [&](MTLPixelFormat f, NSUInteger tw, NSUInteger th, bool mips) {
		MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:tw height:th mipmapped:mips];
		d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		d.storageMode = MTLStorageModePrivate;
		return [device_ newTextureWithDescriptor:d];
	};
	if (!lum_) lum_ = make(MTLPixelFormatR16Float, 512, 512, true);
	if (toned_ && toned_.width == w && toned_.height == h) return;
	toned_ = make(MTLPixelFormatRGBA8Unorm, w, h, false);
	bright_ = make(MTLPixelFormatRGB10A2Unorm, w, h, false);
	ping_ = make(MTLPixelFormatRGB10A2Unorm, std::max<NSUInteger>(w / 2, 1), std::max<NSUInteger>(h / 2, 1), false);
}

void PostProcess::encode(id<MTLCommandBuffer> cb, id<MTLTexture> hdr, id<MTLTexture> output, const ToneMapParams& params, bool bloom) {
	const NSUInteger w = hdr.width, h = hdr.height;
	ensure(w, h);
	auto pass = [&](std::initializer_list<id<MTLTexture>> targets) {
		MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
		NSUInteger i = 0;
		for (id<MTLTexture> t : targets) {
			rp.colorAttachments[i].texture = t;
			rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
			rp.colorAttachments[i].storeAction = MTLStoreActionStore;
			i++;
		}
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setCullMode:MTLCullModeNone];
		return enc;
	};
	auto draw = [](id<MTLRenderCommandEncoder> enc) {
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
	};

	id<MTLRenderCommandEncoder> enc = pass({lum_});
	[enc setRenderPipelineState:luminance_];
	[enc setFragmentTexture:hdr atIndex:0];
	draw(enc);
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	[blit generateMipmapsForTexture:lum_];
	[blit endEncoding];

	enc = pass({toned_, bright_});
	[enc setRenderPipelineState:tonemap_];
	[enc setFragmentBytes:&params length:sizeof params atIndex:0];
	[enc setFragmentTexture:hdr atIndex:0];
	[enc setFragmentTexture:lum_ atIndex:1];
	draw(enc);

	if (!bloom) {
		enc = pass({output});
		[enc setRenderPipelineState:copy_];
		[enc setFragmentTexture:toned_ atIndex:0];
		draw(enc);
		return;
	}
	struct Blur {
		simd_float2 screen_size, direction;
	} blur{simd_make_float2(float(w), float(h)), simd_make_float2(1, 0)};
	const MTLViewport half{0, 0, double(w / 2), double(h / 2), 0, 1};
	enc = pass({ping_});
	[enc setRenderPipelineState:blur_];
	[enc setFragmentBytes:&blur length:sizeof blur atIndex:0];
	[enc setFragmentTexture:bright_ atIndex:0];
	draw(enc);
	blur.direction = simd_make_float2(0, 1);
	enc = pass({bright_});  // the vertical pass lands in the bright target's top-left quarter, as in the original
	[enc setViewport:half];
	[enc setRenderPipelineState:blur_];
	[enc setFragmentBytes:&blur length:sizeof blur atIndex:0];
	[enc setFragmentTexture:ping_ atIndex:0];
	draw(enc);
	enc = pass({output});
	[enc setRenderPipelineState:blend_];
	[enc setFragmentTexture:bright_ atIndex:0];
	[enc setFragmentTexture:toned_ atIndex:1];
	draw(enc);
}

ToneMapParams tonemap_params(const ron::ToneMapSettings& s) {
	ToneMapParams p{};
	p.tint = simd_make_float4(s.tint[0], s.tint[1], s.tint[2], s.tint[3]);
	p.exposure = s.exposure;
	p.white_point = s.white_point;
	p.threshold = s.threshold;
	p.min_luminance = s.min_luminance;
	p.saturation = s.saturation;
	p.strength = s.bloom_strength;
	p.tightness = s.bloom_tightness;
	return p;
}

void SceneTargets::ensure(id<MTLDevice> device, NSUInteger w, NSUInteger h) {
	if (hdr_ && hdr_.width == w && hdr_.height == h) return;
	MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kColor width:w height:h mipmapped:NO];
	cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	cd.storageMode = MTLStorageModePrivate;
	hdr_ = [device newTextureWithDescriptor:cd];
	MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepth width:w height:h mipmapped:NO];
	dd.usage = MTLTextureUsageRenderTarget;
	dd.storageMode = MTLStorageModePrivate;
	depth_ = [device newTextureWithDescriptor:dd];
}

MTLRenderPassDescriptor* SceneTargets::scene_pass(MTLClearColor clear) const {
	MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = hdr_;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = clear;
	rp.depthAttachment.texture = depth_;
	rp.depthAttachment.loadAction = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth = 1.0;
	return rp;
}

MTLRenderPassDescriptor* overlay_pass(id<MTLTexture> output) {
	MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = output;
	rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	return rp;
}

id<MTLTexture> make_readback_target(id<MTLDevice> device, int w, int h) {
	MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                                              width:NSUInteger(w)
	                                                                             height:NSUInteger(h)
	                                                                          mipmapped:NO];
	cd.usage = MTLTextureUsageRenderTarget;
	cd.storageMode = MTLStorageModeShared;  // Apple silicon: CPU-readable render target
	return [device newTextureWithDescriptor:cd];
}

bool write_png(id<MTLTexture> tex, const std::string& path) {
	const NSInteger w = NSInteger(tex.width), h = NSInteger(tex.height);
	NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil
	                                                               pixelsWide:w
	                                                               pixelsHigh:h
	                                                            bitsPerSample:8
	                                                          samplesPerPixel:4
	                                                                 hasAlpha:YES
	                                                                 isPlanar:NO
	                                                           colorSpaceName:NSDeviceRGBColorSpace
	                                                              bytesPerRow:w * 4
	                                                             bitsPerPixel:32];
	uint8_t* px = rep.bitmapData;
	[tex getBytes:px bytesPerRow:NSUInteger(w) * 4 fromRegion:MTLRegionMake2D(0, 0, NSUInteger(w), NSUInteger(h)) mipmapLevel:0];
	for (size_t i = 0; i < size_t(w) * size_t(h); i++) {
		std::swap(px[i * 4], px[i * 4 + 2]);  // BGRA -> RGBA
		px[i * 4 + 3] = 255;
	}
	NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
	return [png writeToFile:@(path.c_str()) atomically:YES];
}

}  // namespace ron::metal
