// MSL for the render-state draw path and the tonemap pass. Our own implementation of the classic two-stage
// fixed-function texture combiner (D3D9 TSS formulas) plus the lighting model documented in docs/renderstates.md.
// Specialised per RenderState with function constants (the original compiles one HLSL permutation per state).
R"METAL(
#include <metal_stdlib>
using namespace metal;

// Texture ops (ron::TextureOp) and args (ron::TextureArg).
enum { OP_DISABLE, OP_SELECT1, OP_SELECT2, OP_MOD, OP_MOD2X, OP_MOD4X, OP_ADD, OP_ADDSIGNED, OP_ADDSIGNED2X, OP_SUBTRACT,
       OP_ADDSMOOTH, OP_BLENDDIFFUSEALPHA, OP_BLENDTEXTUREALPHA, OP_BLENDFACTORALPHA, OP_BLENDTEXTUREALPHAPM,
       OP_BLENDCURRENTALPHA };
enum { ARG_CURRENT, ARG_DIFFUSE, ARG_SELECTMASK, ARG_SPECULAR, ARG_TEMP, ARG_TEXTURE, ARG_TFACTOR };
enum { CMP_NEVER, CMP_LESS, CMP_EQUAL, CMP_LESSEQUAL, CMP_GREATER, CMP_NOTEQUAL, CMP_GREATEREQUAL, CMP_ALWAYS };

constant int  fc_stages     [[function_constant(0)]];   // 0, 1 or 2 active stages
constant int  fc_s0_cop     [[function_constant(1)]];
constant int  fc_s0_ca1     [[function_constant(2)]];
constant int  fc_s0_ca2     [[function_constant(3)]];
constant int  fc_s0_aop     [[function_constant(4)]];
constant int  fc_s0_aa1     [[function_constant(5)]];
constant int  fc_s0_aa2     [[function_constant(6)]];
constant int  fc_s1_cop     [[function_constant(7)]];
constant int  fc_s1_ca1     [[function_constant(8)]];
constant int  fc_s1_ca2     [[function_constant(9)]];
constant int  fc_s1_aop     [[function_constant(10)]];
constant int  fc_s1_aa1     [[function_constant(11)]];
constant int  fc_s1_aa2     [[function_constant(12)]];
constant int  fc_alpha_func [[function_constant(13)]];  // CMP_ALWAYS when alpha test is off
constant bool fc_lighting   [[function_constant(14)]];
constant bool fc_tl_fake    [[function_constant(15)]];
constant bool fc_no_hdr     [[function_constant(16)]];
constant bool fc_tex0       [[function_constant(17)]];  // stage 0 reads its texture
constant bool fc_tex1       [[function_constant(18)]];

struct Vertex {
	packed_float3 position;
	packed_float3 normal;
	float2 uv;
};

// Same, plus a per-vertex diffuse colour (terrain: baked lighting and alpha).
struct ColorVertex {
	packed_float3 position;
	packed_float3 normal;
	packed_float2 uv;  // packed: float2 would align to 8 and pad the struct to 40 bytes
	uchar4 color;
};

// ColorVertex plus UV1 (terrain transitions: the mask).
struct ColorVertex2 {
	ColorVertex base;
	packed_float2 uv1;
};

struct SceneUniforms {
	float4x4 view;
	float4x4 proj;
	float4 light_dir;      // world space, direction the light travels
	float4 light_diffuse;
	float4 ambient_light;
};

struct DrawUniforms {
	float4x4 world;
	float4 tfactor;
	float4 diffuse;        // the vertex diffuse colour (constant per draw here)
	float4 specular;
	float4 mat_ambient;
	float4 mat_diffuse;
	float alpha_ref;       // 0..1
	float depth_bias;
	float tl_alpha;
	float pad;
};

// uv1 feeds texture stage 1 (TEX1_UV1); vertices without a second set repeat uv, as in the original.
struct VOut {
	float4 position [[position]];
	float3 view_normal;
	float2 uv;
	float2 uv1;
	float4 color;
};

static VOut transform(float3 position, float3 normal, float2 uv, float4 color, constant DrawUniforms& d,
                      constant SceneUniforms& s, float2 uv1) {
	float4 vp = s.view * (d.world * float4(position, 1.0));
	vp.z += 8.0 * d.depth_bias;  // view space: +z is toward the camera
	VOut o;
	o.position = s.proj * vp;
	o.view_normal = (s.view * (d.world * float4(normal, 0.0))).xyz;
	o.uv = uv;
	o.uv1 = uv1;
	o.color = color;
	return o;
}

vertex VOut rs_vertex(uint vid [[vertex_id]],
                      const device Vertex* verts [[buffer(0)]],
                      constant DrawUniforms& d [[buffer(1)]],
                      constant SceneUniforms& s [[buffer(2)]]) {
	const Vertex v = verts[vid];
	return transform(float3(v.position), float3(v.normal), v.uv, float4(1.0), d, s, v.uv);
}

vertex VOut rs_vertex_color(uint vid [[vertex_id]],
                            const device ColorVertex* verts [[buffer(0)]],
                            constant DrawUniforms& d [[buffer(1)]],
                            constant SceneUniforms& s [[buffer(2)]]) {
	const ColorVertex v = verts[vid];
	return transform(float3(v.position), float3(v.normal), float2(v.uv), float4(v.color) / 255.0, d, s, float2(v.uv));
}

vertex VOut rs_vertex_color2(uint vid [[vertex_id]],
                             const device ColorVertex2* verts [[buffer(0)]],
                             constant DrawUniforms& d [[buffer(1)]],
                             constant SceneUniforms& s [[buffer(2)]]) {
	const ColorVertex2 v = verts[vid];
	return transform(float3(v.base.position), float3(v.base.normal), float2(v.base.uv), float4(v.base.color) / 255.0, d, s,
	                 float2(v.uv1));
}

static float4 arg(int a, int stage, float4 cur, float4 tex, float4 diff, constant DrawUniforms& d) {
	switch (a) {
		case ARG_CURRENT: return stage == 0 ? diff : cur;
		case ARG_DIFFUSE: return diff;
		case ARG_SPECULAR: return d.specular;
		case ARG_TEXTURE: return tex;
		case ARG_TFACTOR: return d.tfactor;
		case ARG_TEMP: return float4(0.0);
		default: return float4(1.0);
	}
}

static float4 op(int o, float4 a1, float4 a2, float4 cur, float4 tex, float4 diff, constant DrawUniforms& d) {
	switch (o) {
		case OP_SELECT1: return a1;
		case OP_SELECT2: return a2;
		case OP_MOD: return a1 * a2;
		case OP_MOD2X: return a1 * a2 * 2.0;
		case OP_MOD4X: return a1 * a2 * 4.0;
		case OP_ADD: return a1 + a2;
		case OP_ADDSIGNED: return a1 + a2 - 0.5;
		case OP_ADDSIGNED2X: return (a1 + a2 - 0.5) * 2.0;
		case OP_SUBTRACT: return a1 - a2;
		case OP_ADDSMOOTH: return a1 + a2 * (1.0 - a1);
		case OP_BLENDDIFFUSEALPHA: return mix(a2, a1, diff.a);
		case OP_BLENDTEXTUREALPHA: return mix(a2, a1, tex.a);
		case OP_BLENDFACTORALPHA: return mix(a2, a1, d.tfactor.a);
		case OP_BLENDTEXTUREALPHAPM: return a1 + a2 * (1.0 - tex.a);
		case OP_BLENDCURRENTALPHA: return mix(a2, a1, cur.a);
		default: return cur;  // DISABLE passes the current value through
	}
}

static bool alpha_pass(int f, float a, float ref) {
	const float eps = 0.0001;
	switch (f) {
		case CMP_NEVER: return false;
		case CMP_LESS: return a < ref;
		case CMP_EQUAL: return abs(a - ref) < eps;
		case CMP_LESSEQUAL: return a <= ref + eps;
		case CMP_GREATER: return a > ref;
		case CMP_NOTEQUAL: return abs(a - ref) >= eps;
		case CMP_GREATEREQUAL: return a >= ref - eps;
		default: return true;
	}
}

fragment float4 rs_fragment(VOut in [[stage_in]],
                            constant DrawUniforms& d [[buffer(1)]],
                            constant SceneUniforms& s [[buffer(2)]],
                            texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]],
                            texture2d<float> t1 [[texture(1)]], sampler s1 [[sampler(1)]]) {
	const float4 tex0 = fc_tex0 ? t0.sample(s0, in.uv) : float4(1.0);
	const float4 tex1 = fc_tex1 ? t1.sample(s1, in.uv1) : float4(1.0);

	// Each stage evaluates its colour and alpha ops on full vectors and keeps rgb / a respectively.
	const float4 diff = d.diffuse * in.color;
	float4 cur = float4(1.0);
	if (fc_stages >= 1) {
		const float4 c = op(fc_s0_cop, arg(fc_s0_ca1, 0, cur, tex0, diff, d), arg(fc_s0_ca2, 0, cur, tex0, diff, d), cur, tex0, diff, d);
		const float4 a = op(fc_s0_aop, arg(fc_s0_aa1, 0, cur, tex0, diff, d), arg(fc_s0_aa2, 0, cur, tex0, diff, d), cur, tex0, diff, d);
		cur = float4(c.rgb, a.a);
	}
	if (fc_stages >= 2) {
		const float4 c = op(fc_s1_cop, arg(fc_s1_ca1, 1, cur, tex1, diff, d), arg(fc_s1_ca2, 1, cur, tex1, diff, d), cur, tex1, diff, d);
		const float4 a = op(fc_s1_aop, arg(fc_s1_aa1, 1, cur, tex1, diff, d), arg(fc_s1_aa2, 1, cur, tex1, diff, d), cur, tex1, diff, d);
		cur = float4(c.rgb, a.a);
	}
	float4 color = saturate(cur);
	const float hdr = fc_no_hdr ? 1.0 : 5.0;

	if (fc_lighting) {
		// Interpolation can shrink a normal to zero (opposite normals on one triangle); normalize(0) is NaN and a NaN
		// pixel blacks the whole frame through the exposure average, so fall back to facing the camera.
		const float nl2 = dot(in.view_normal, in.view_normal);
		const float3 n = nl2 > 1e-20 ? in.view_normal * rsqrt(nl2) : float3(0.0, 0.0, 1.0);
		const float3 l = (s.view * float4(-normalize(s.light_dir.xyz), 0.0)).xyz;
		const float ndl = dot(n, l);
		const float3 ambient = max(0.0, hdr * d.mat_ambient.rgb * s.ambient_light.rgb);
		const float3 diffuse = max(0.0, hdr * M_1_PI_F * d.mat_diffuse.rgb * s.light_diffuse.rgb * ndl);
		color.rgb = fc_no_hdr ? color.rgb * (diffuse + ambient) : pow(color.rgb, 1.45) * (diffuse + ambient);
	} else if (fc_tl_fake) {
		color.rgb = pow(color.rgb, 1.9) * hdr;
		color.a *= d.tl_alpha;
	}
	color = max(0.0, color);
	if (!alpha_pass(fc_alpha_func, color.a, d.alpha_ref)) discard_fragment();
	return fc_no_hdr ? saturate(color) : color;
}

// ------------------------------------------------------------------------------------------------- post
// The original chain (docs/lighting-spec.md section 8): luminance -> mean (mip chain) -> tonemap (toned + bright) ->
// bloom blur (asymmetric taps) -> screen blend. All in gamma space, no sRGB.

struct ToneMap {
	float4 tint;         // rgb, w = weight
	float exposure;
	float white_point;
	float threshold;
	float min_luminance;
	float saturation;
	float strength;
	float tightness;
	float pad;
};

struct QuadOut {
	float4 position [[position]];
	float2 uv;
};

vertex QuadOut quad_vertex(uint vid [[vertex_id]]) {
	const float2 p = float2((vid << 1) & 2, vid & 2);  // fullscreen triangle
	QuadOut o;
	o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
	o.uv = float2(p.x, 1.0 - p.y);
	return o;
}

constexpr sampler linear_clamp(filter::linear, mip_filter::nearest, address::clamp_to_edge);
constant float3 kLuminance = float3(0.3, 0.59, 0.11);

fragment float4 luminance_fragment(QuadOut in [[stage_in]], texture2d<float> hdr [[texture(0)]]) {
	return float4(max(0.0, dot(hdr.sample(linear_clamp, in.uv).rgb, kLuminance)));
}

static float3 filmic(float3 x) {
	return ((0.15 * x + 0.05) * x + 0.004) / ((0.15 * x + 0.5) * x + 0.06) - (0.02 / 0.3);
}

struct ToneOut {
	float4 toned [[color(0)]];
	float4 bright [[color(1)]];
};

fragment ToneOut tonemap_fragment(QuadOut in [[stage_in]], constant ToneMap& t [[buffer(0)]],
                                  texture2d<float> hdr [[texture(0)]], texture2d<float> lum [[texture(1)]]) {
	const float3 src = hdr.sample(linear_clamp, in.uv).rgb;
	const float avg = max(lum.sample(linear_clamp, float2(0.5), level(lum.get_num_mip_levels() - 1)).r, t.min_luminance);
	float3 c = filmic(t.exposure * src) / filmic(float3(avg * t.white_point));
	const float l = dot(c, kLuminance);
	ToneOut o;
	o.toned = float4(saturate(mix(float3(l), c, t.saturation) * mix(float3(1.0), t.tint.rgb, t.tint.w)), 1.0);
	const float3 b = l > 0.0 ? t.strength * pow(max(c, 0.0), t.tightness) * max(0.0, l - t.threshold) / l : float3(0.0);
	o.bright = float4(saturate(b), 1.0);
	return o;
}

struct Blur {
	float2 screen_size;  // full-resolution size
	float2 direction;
};

constant float kKernel[9] = {0.0001338302257648854, 0.004431848411938008, 0.05399096651318806, 0.24197072451914337,
                             0.3989422804014327, 0.24197072451914337, 0.05399096651318806, 0.004431848411938008,
                             0.0001338302257648854};

// The original's taps are offset (i - 4) * (i / 2) steps: 0, -1.5, -2, -1.5, 0, 2.5, 6, 10.5, 16 (smears right/down).
fragment float4 blur_fragment(QuadOut in [[stage_in]], constant Blur& b [[buffer(0)]], texture2d<float> src [[texture(0)]]) {
	const float2 step = 3.0 / b.screen_size;
	float3 sum = 0.0;
	for (int i = 0; i < 9; i++) {
		const float2 uv = clamp(in.uv + b.direction * step * float(i - 4) * (float(i) / 2.0), 0.0, 1.0);
		sum += kKernel[i] * src.sample(linear_clamp, uv).rgb;
	}
	return float4(sum, 1.0);
}

// Screen blend of the blurred bloom (top-left quarter of the bright target) over the toned image.
fragment float4 blend_fragment(QuadOut in [[stage_in]], texture2d<float> bloom [[texture(0)]], texture2d<float> toned [[texture(1)]]) {
	const float3 c = 1.0 - (1.0 - bloom.sample(linear_clamp, in.uv * 0.5).rgb) * (1.0 - toned.sample(linear_clamp, in.uv).rgb);
	return float4(c, 1.0);
}

fragment float4 copy_fragment(QuadOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
	return float4(src.sample(linear_clamp, in.uv).rgb, 1.0);
}
)METAL"
