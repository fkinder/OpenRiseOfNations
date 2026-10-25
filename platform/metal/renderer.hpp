// Metal implementation of the game's render-state draw path (Objective-C++; include from .mm files only).
// StateCache turns a ron::RenderState into pipeline, depth-stencil, sampler and raster state, specialising one
// combiner shader per state; PostProcess tonemaps the HDR scene target to the display. See docs/renderstates.md.
#pragma once

#import <Metal/Metal.h>
#include <simd/simd.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>

#include "render/renderstate.hpp"
#include "render/lighting.hpp"
#include "render/vertex.hpp"

namespace ron::metal {

// Must match SceneUniforms / DrawUniforms / ToneMap in combiner_shader.inc.
struct SceneUniforms {
	simd_float4x4 view;
	simd_float4x4 proj;
	simd_float4 light_dir;
	simd_float4 light_diffuse;
	simd_float4 ambient_light;
};

struct DrawUniforms {
	simd_float4x4 world;
	simd_float4 tfactor;
	simd_float4 diffuse;
	simd_float4 specular;
	simd_float4 mat_ambient;
	simd_float4 mat_diffuse;
	float alpha_ref;
	float depth_bias;
	float tl_alpha;
	float pad;
};

struct ToneMapParams {
	simd_float4 tint;
	float exposure;
	float white_point;
	float threshold;
	float min_luminance;
	float saturation;
	float strength;
	float tightness;
	float pad;
};

// 0xAABBGGRR (the game's tfactor layout) to a float colour.
simd_float4 unpack_abgr(uint32_t c);

// Uniforms with the state's own constants filled in (tfactor, alpha ref, depth bias) and neutral materials.
DrawUniforms draw_uniforms(const RenderState& rs, simd_float4x4 world);

// Vertex layouts. Posed: ron::PosedVertex (32 bytes). Colored: ron::ColorVertex (36 bytes, adds an RGBA8 diffuse).
// Colored2: ron::ColorVertex2 (44 bytes, adds UV1 for texture stage 1).
enum class VertexKind { Posed, Colored, Colored2 };


// Buffer and texture slots: vertices at buffer 0, DrawUniforms at 1, SceneUniforms at 2;
// stage textures at texture/sampler 0 and 1.
class StateCache {
public:
	// Returns false and sets `error` if the shader library fails to compile.
	bool init(id<MTLDevice> device, MTLPixelFormat color, MTLPixelFormat depth, std::string& error);

	// Binds everything `rs` controls. `flip_cull` swaps culled faces for a mirrored coordinate convention.
	void bind(id<MTLRenderCommandEncoder> enc, const RenderState& rs, VertexKind kind = VertexKind::Posed,
	          bool no_hdr = false, bool flip_cull = false);

	id<MTLLibrary> library() const { return library_; }

private:
	id<MTLRenderPipelineState> pipeline(const RenderState& rs, VertexKind kind, bool no_hdr);
	id<MTLDepthStencilState> depth_state(const RenderState& rs);
	id<MTLSamplerState> sampler(TexAddress u, TexAddress v, bool point);

	id<MTLDevice> device_;
	id<MTLLibrary> library_;
	MTLPixelFormat color_ = MTLPixelFormatInvalid, depth_ = MTLPixelFormatInvalid;
	std::map<std::array<uint8_t, 24>, id<MTLRenderPipelineState>> pipelines_;  // the render state's pipeline-relevant fields, one byte each
	std::map<uint32_t, id<MTLDepthStencilState>> depth_states_;
	std::map<uint32_t, id<MTLSamplerState>> samplers_;
};

// HDR scene target -> display: the original post chain (docs/lighting-spec.md section 8). Luminance into a 512x512
// R16F target whose 1x1 mip is the frame's mean, the Hable tonemap writing a toned and a "bright" image, then (with
// bloom) a half-resolution blur and a screen blend, or (without) a straight copy.
class PostProcess {
public:
	bool init(id<MTLDevice> device, id<MTLLibrary> library, MTLPixelFormat output, std::string& error);
	void encode(id<MTLCommandBuffer> cb, id<MTLTexture> hdr, id<MTLTexture> output, const ToneMapParams& params, bool bloom);

private:
	void ensure(NSUInteger w, NSUInteger h);
	id<MTLDevice> device_;
	id<MTLRenderPipelineState> luminance_, tonemap_, blur_, blend_, copy_;
	id<MTLTexture> lum_, toned_, bright_, ping_;
};

// Tonemap parameters from the engine's settings.
ToneMapParams tonemap_params(const ToneMapSettings& s);

// The HDR scene colour target and depth buffer, recreated when the size changes.
class SceneTargets {
public:
	static constexpr MTLPixelFormat kColor = MTLPixelFormatRGBA16Float;
	static constexpr MTLPixelFormat kDepth = MTLPixelFormatDepth32Float;

	void ensure(id<MTLDevice> device, NSUInteger w, NSUInteger h);
	// A pass that clears colour to `clear` and depth to 1.
	MTLRenderPassDescriptor* scene_pass(MTLClearColor clear) const;
	id<MTLTexture> hdr() const { return hdr_; }

private:
	id<MTLTexture> hdr_, depth_;
};

// A pass over the tone-mapped output that keeps its contents (no depth): the post-tonemap overlays of Scene::render
// (docs/lighting-spec.md section 2, step 10: selection circles, health bars, drag box). Pipelines for it come from a
// StateCache initialised with the output format and MTLPixelFormatInvalid depth.
MTLRenderPassDescriptor* overlay_pass(id<MTLTexture> output);

// A CPU-readable BGRA8 texture for offscreen rendering, and a PNG writer for it.
id<MTLTexture> make_readback_target(id<MTLDevice> device, int w, int h);
bool write_png(id<MTLTexture> bgra8, const std::string& path);

}  // namespace ron::metal
