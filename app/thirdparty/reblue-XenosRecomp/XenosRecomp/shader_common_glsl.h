// tw2de: shader_common.h for the translator's GLSL output (ShaderRecompiler::glsl), the deko3d back end's GLSL
// (uam, GLSL 4.60 with GL_ARB_bindless_texture). The same helpers and the same bindings as the SPIR-V path gives after
// SPIRV-Cross and tools/deko3d_glsl.py: constant buffers at UBO bindings 0 (vertex float constants), 1 (pixel float
// constants) and 2 (SharedConstants), textures as bindless handles (dkh: the texture word's id in bits 0-19 and the
// sampler word's in bits 20-31), the address window at storage binding 15 and the memexport buffer at 14,
// specialization constants as SPIRV_CROSS_CONSTANT_ID_<n> defines (plume_deko3d inserts a variant's values after the
// first line). The translator writes, before this text: #version, the tw2de-dk line, and the TW2DE_STAGE_* /
// TW2DE_USES_* defines that pick what this shader needs.

#extension GL_ARB_bindless_texture : require

// ---- HLSL spellings the translated code uses
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4
#define asfloat uintBitsToFloat
#define asuint floatBitsToUint
#define frac fract
#define rsqrt inversesqrt
#define lerp mix
#define rcp(x) (1.0 / (x))

float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec2 saturate(vec2 x) { return clamp(x, vec2(0.0), vec2(1.0)); }
vec3 saturate(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }
vec4 saturate(vec4 x) { return clamp(x, vec4(0.0), vec4(1.0)); }

// select(c, a, b): a where c holds, else b (HLSL 2021's select; no branch).
float select(bool c, float a, float b) { return mix(b, a, c); }
vec2 select(bool c, vec2 a, vec2 b) { return mix(b, a, bvec2(c)); }
vec3 select(bool c, vec3 a, vec3 b) { return mix(b, a, bvec3(c)); }
vec4 select(bool c, vec4 a, vec4 b) { return mix(b, a, bvec4(c)); }
vec2 select(bvec2 c, vec2 a, vec2 b) { return mix(b, a, c); }
vec3 select(bvec3 c, vec3 a, vec3 b) { return mix(b, a, c); }
vec4 select(bvec4 c, vec4 a, vec4 b) { return mix(b, a, c); }
uint select(bool c, uint a, uint b) { return c ? a : b; }
bool select(bool c, bool a, bool b) { return c ? a : b; }

// ---- constants
#define FLT_MAX RENUT_SHARED_FLOAT(1340)
#define FLT_MIN (-RENUT_SHARED_FLOAT(1340))
#define RENUT_SHARED_VECTORS 86  // tw2de (2026-11-06): 1376 bytes, with the motion vector words

#ifdef TW2DE_STAGE_VERTEX
// tw2de (2026-10-23, pass bonerow of tools/deko3d_glsl_opt.py): 257 registers, the last always zero
// (render/upload_ring.h kFloatConstantsUboBytes): g_C reads a register past the 256 there instead of selecting zero.
layout(binding = 0, std140) uniform RenutUboVertex { vec4 v[257]; } g_UboVertex;
#else
layout(binding = 1, std140) uniform RenutUboPixel { vec4 v[257]; } g_UboPixel;
#endif
layout(binding = 2, std140) uniform RenutUboShared { uvec4 v[RENUT_SHARED_VECTORS]; } g_UboShared;

// A word of a constant buffer vector by a component index known only at run time: the vector loaded whole, the word
// picked from it (uam compiles NAME.v[a][b] with b not a constant as a load of the word at a * 16 + b * 4 rounded
// down to the vector: tools/deko3d_glsl.py, dk_pick).
uint dkPickU(uvec4 v, uint i) { return v[i]; }
#define RENUT_SHARED_UINT(o)       dkPickU(g_UboShared.v[uint(o) >> 4u], (uint(o) >> 2u) & 3u)
#define RENUT_SHARED_FLOAT(o)      uintBitsToFloat(RENUT_SHARED_UINT(o))
#define RENUT_SHARED_FLOAT2(o)     vec2(RENUT_SHARED_FLOAT(o), RENUT_SHARED_FLOAT(uint(o) + 4u))
#define RENUT_SHARED_UINT4(o)      (g_UboShared.v[uint(o) >> 4u])
#define g_Booleans(i)              RENUT_SHARED_UINT(512u + uint(i) * 4u)
#define g_ScreenOffset             RENUT_SHARED_FLOAT2(544)
#define g_ScreenScale              RENUT_SHARED_FLOAT2(568)
#define g_ScreenSpace              RENUT_SHARED_UINT(968)
#define g_ViewportSize             RENUT_SHARED_FLOAT2(1328)
// tw2de (2026-11-07, dynamic resolution): the targets' host pixels per guest pixel as drawn (SharedConstants
// resolution_scale: RenderScale::DrawFactor), as Renut has it; 1.0 from 2026-10-23 (pass fragscale) while deko3d drew
// every target at the console's scale.
#define g_ResolutionScale          RENUT_SHARED_FLOAT(1336)
// tw2de (2026-11-07, dynamic resolution): the step between two dynamic-resolution levels' drawn fractions
// (SharedConstants drs_step; render/dynamic_resolution.h).
#define g_DrsStep                  RENUT_SHARED_FLOAT(1356)
#define g_PixelOrigin              RENUT_SHARED_FLOAT2(1344)
// tw2de (2026-11-06, motion vectors for FSR 3; render/draw.cpp SharedConstants, nx_shaders/xenos_glsl.cpp
// MotionVectorPass): bit 0 the vertex shader also runs with the previous frame's constants (UBO 4) and the pixel shader
// writes the motion at colour target 1; bit 1 the pixel shader shows the motion in its colour (a diagnostic); bit 2 the
// position gets the jitter (in clip space, times w).
#define g_FnxfMotion               RENUT_SHARED_UINT(1360)
#define g_FnxfJitter               RENUT_SHARED_FLOAT2(1368)
// TW2 (2026-11-15; SharedConstants motion_prev): the float constants the vertex shader's previous-frame run takes
// from UBO 4: the first register in bits 0-15, their count in bits 16-31; the others are this frame's.
#define g_FnxfPrevRange            RENUT_SHARED_UINT(1364)
#define g_AlphaThreshold           RENUT_SHARED_FLOAT(552)
#define g_VertexFetchSlot(i)       RENUT_SHARED_UINT(576u + uint(i) * 4u)
#define g_VertexFetchBytes         RENUT_SHARED_UINT(972)
#define g_MemExportStream(i)       RENUT_SHARED_UINT4(976u + uint(i) * 16u)
#define g_Tessellation             RENUT_SHARED_UINT4(1040)
#define g_BoundFetch(i)            RENUT_SHARED_UINT4(1056u + uint(i) * 16u)
#define BOOL_BIT(n) ((g_Booleans(uint(n) / 32u) & (1u << (uint(n) & 31u))) != 0u)

// ---- textures: bindless handles from SharedConstants' texture and sampler words
// TW2 (2026-11-12): the texture word's bits 16-19 hold the fetch constant's exponent adjust (renutExpScale below); the
// descriptor index is bits 0-15 (8192 slots).
#define dkh(t, s) uvec2((uint(t) & 0xFFFFu) | (uint(s) & 0xFFF00000u), 0u)
#define dkTex(t) uvec2(uint(t) & 0xFFFFu, 0u)
#define RENUT_RESOLUTION_SCALE_SHIFT 26u
#define RENUT_STACKED_TEXTURE 0x80000000u
#define RENUT_STACKED_LAYER_LINEAR 0x40000000u
#define RENUT_STACKED_LAYER_WRAP 0x20000000u

// tw2de (2026-10-23, pass scale1 of tools/deko3d_glsl_opt.py): deko3d draws every texture at the console's scale
// (render/surfaces.cpp and graphics_options.cpp force 1x on deko3d), so a texture word's scale code (bits 26-28) is 0
// and the guest size is the host size: no seven-way select, division and rounding before every offset sample.
float renutTextureScale(uint word) { return 1.0; }

uvec2 renutGuestSize(uvec2 hostSize, uint word) { return hostSize; }

// TW2 (2026-11-12): the fetch constant's exponent adjust (dword 3 bits 13-18: the fetch result times 2^adjust), which
// render/draw.cpp puts in the texture word's bits 16-19 (signed, -8..7). TW2 samples one 8-bit target at
// x1, x1/2 and x1/4 in its final composite; without it the picture came out green and too bright.
// tw2de (2026-11-12, tw2de_deko3d_word_fx_spec): specialization constant 914, given per draw (render/draw.cpp
// WordFxBits): bit 0 a bound texture word has an exponent adjust, bit 1 one has a dynamic-resolution code, bit 2 the
// draw's motion word is set (MotionVectorPass). 0 (the default): none, and uam folds them all away; every fetch ran
// them before (an exp2, two divides and a ceil a fetch).
#ifndef SPIRV_CROSS_CONSTANT_ID_914
#define SPIRV_CROSS_CONSTANT_ID_914 0u
#endif
const uint g_renutWordFx = SPIRV_CROSS_CONSTANT_ID_914;
float renutExpScale(uint word) { return (g_renutWordFx & 1u) != 0u ? exp2(float(int(word << 12u) >> 28)) : 1.0; }

uvec2 getTexture2DSize(uint t) { return renutGuestSize(uvec2(textureSize(sampler2D(dkTex(t)), 0)), t); }

// tw2de (2026-11-07, dynamic resolution; render/dynamic_resolution.h): a texture resolved from a dynamic-resolution
// target holds its picture in the top-left part its level drew (descriptor index bits 20-25: 0 none, n the fraction
// 1 - (n - 1) x g_DrsStep): the coordinates in the guest's [0, 1] scaled to it, and kept off its edge's texel centre (a
// bilinear tap there would blend in what lies beyond, not drawn this time). Offsets in guest texels scale with it.
// No branch (selects only): the word, the fraction and the bound are the same at every fetch of a texture, so they are
// worked out once a shader; a fetch adds a multiply and a minimum.
vec2 renutDrsUv(uint word, vec2 hostSize, vec2 uv)
{
    if ((g_renutWordFx & 2u) == 0u)
        return uv;
    const uint code = (word >> 20u) & 63u;
    const float f = 1.0 - float(max(code, 1u) - 1u) * g_DrsStep;
    // fnxf_xiii (2026-11-11): the last texel the level drew whole. A size times the fraction can end mid-texel (a 270-line
    // target at 630 of 1080 lines: 157.5); the rasteriser leaves that half-covered texel out (its centre on the edge), so
    // the bound is the centre of the one before, not the edge less half a texel (a bilinear tap there took half of the
    // stale texel: bloom's blurs carried it up the bottom of the picture). 0.501: a whole size stays whole in float.
    const vec2 bound = (ceil(vec2(f) * hostSize - 0.501) - 0.5) / hostSize;
    return code == 0u ? uv : min(uv * f, bound);
}

vec4 tfetch2D(uint t, uint s, vec2 uv, vec2 offset)
{
    const vec2 size = vec2(getTexture2DSize(t));
    return texture(sampler2D(dkh(t, s)), renutDrsUv(t, size, uv + offset / size)) * renutExpScale(t);
}

vec4 tfetch2DLevel(uint t, uint s, vec2 uv, vec2 offset, float lod)
{
    const vec2 size = vec2(getTexture2DSize(t));
    return textureLod(sampler2D(dkh(t, s)), renutDrsUv(t, size, uv + offset / size), lod) * renutExpScale(t);
}

vec2 getWeights2D(uint t, uint s, vec2 uv, vec2 offset)
{
    return select(isnan(uv), vec2(0.0), fract(uv * vec2(getTexture2DSize(t)) + offset - 0.5));
}

uvec3 getTexture3DSize(uint t)
{
    uvec3 d = uvec3(textureSize(sampler3D(dkTex(t)), 0));
    return uvec3(renutGuestSize(d.xy, t), d.z);
}

bool isStackedTexture(uint t) { return (t & RENUT_STACKED_TEXTURE) != 0u; }

uvec3 getStackedTextureDimensions(uint t)
{
    uvec3 d = uvec3(textureSize(sampler2DArray(dkTex(t)), 0));
    d.xy = renutGuestSize(d.xy, t);
    return d;
}

struct StackedCoords
{
    vec3 coord0;
    vec3 coord1;
    float weight;
};

StackedCoords getStackedCoords(uint t, vec3 uvw, vec3 offset)
{
    uvec3 d = getStackedTextureDimensions(t);
    vec2 uv = uvw.xy + offset.xy / vec2(d.xy);
    float layer = uvw.z * float(d.z) + offset.z;
    float first = floor(layer);
    StackedCoords c;
    c.weight = 0.0;
    if ((t & RENUT_STACKED_LAYER_LINEAR) != 0u)
    {
        first = floor(layer - 0.5);
        c.weight = layer - 0.5 - first;
    }
    float layers = float(d.z);
    float second = first + 1.0;
    if ((t & RENUT_STACKED_LAYER_WRAP) != 0u)
    {
        first -= layers * floor(first / layers);
        second -= layers * floor(second / layers);
    }
    float lastLayer = layers - 1.0;
    c.coord0 = vec3(uv, clamp(first, 0.0, lastLayer));
    c.coord1 = vec3(uv, clamp(second, 0.0, lastLayer));
    return c;
}

// Both layers are always sampled: implicit derivatives need uniform control flow.
vec4 tfetchStacked(uint t, uint s, vec3 uvw, vec3 offset)
{
    StackedCoords c = getStackedCoords(t, uvw, offset);
    return mix(texture(sampler2DArray(dkh(t, s)), c.coord0), texture(sampler2DArray(dkh(t, s)), c.coord1), c.weight);
}

vec4 tfetchStackedLevel(uint t, uint s, vec3 uvw, vec3 offset, float lod)
{
    StackedCoords c = getStackedCoords(t, uvw, offset);
    return mix(textureLod(sampler2DArray(dkh(t, s)), c.coord0, lod), textureLod(sampler2DArray(dkh(t, s)), c.coord1, lod),
        c.weight);
}

vec4 tfetch3D(uint t, uint s, vec3 uvw, vec3 offset)
{
    if (isStackedTexture(t))
        return tfetchStacked(t, s, uvw, offset);
    return texture(sampler3D(dkh(t, s)), uvw + offset / vec3(getTexture3DSize(t)));
}

vec4 tfetch3DLevel(uint t, uint s, vec3 uvw, vec3 offset, float lod)
{
    if (isStackedTexture(t))
        return tfetchStackedLevel(t, s, uvw, offset, lod);
    return textureLod(sampler3D(dkh(t, s)), uvw + offset / vec3(getTexture3DSize(t)), lod);
}

// tw2de (2026-10-23, the old pipeline's bound textures, tools/deko3d_glsl.py): the fetches whose texture and sampler
// words are plain SharedConstants words (all the translator writes) take a bound sampler (xenos_glsl.cpp
// BoundTexturePass: binding k, its words in the tw2de-dk line's texb=, which plume_deko3d binds per draw as the handle
// dkh makes) and its level-0 size (from the texture size buffer, constant 908, or a query). The same texture, sampler
// and coordinates as the bindless forms above: uam writes TEXS (no handle arithmetic, fewer registers).
vec4 tfetch2D(sampler2D tex, ivec2 size, vec2 uv, vec2 offset) { return texture(tex, uv + offset / vec2(size)); }
vec4 tfetch2DLevel(sampler2D tex, ivec2 size, vec2 uv, vec2 offset, float lod)
{
    return textureLod(tex, uv + offset / vec2(size), lod);
}
// tw2de (2026-11-07, dynamic resolution): with the texture's descriptor index word (BoundTexturePass: DK_TEX_WORD_k,
// its shared constant) for renutDrsUv.
vec4 tfetch2D(sampler2D tex, ivec2 size, uint word, vec2 uv, vec2 offset)
{
    const vec2 sz = vec2(size);
    return texture(tex, renutDrsUv(word, sz, uv + offset / sz)) * renutExpScale(word);
}
vec4 tfetch2DLevel(sampler2D tex, ivec2 size, uint word, vec2 uv, vec2 offset, float lod)
{
    const vec2 sz = vec2(size);
    return textureLod(tex, renutDrsUv(word, sz, uv + offset / sz), lod) * renutExpScale(word);
}
vec2 getWeights2D(sampler2D tex, ivec2 size, vec2 uv, vec2 offset)
{
    return select(isnan(uv), vec2(0.0), fract(uv * vec2(size) + offset - 0.5));
}

vec2 texelToUV1D(uint t, float x) { return vec2(x / float(getTexture2DSize(t).x), 0.5); }
vec2 texelToUV2D(uint t, vec2 uv) { return uv / vec2(getTexture2DSize(t)); }
vec3 texelToUV3D(uint t, vec3 uvw)
{
    if (isStackedTexture(t))
        return uvw / vec3(getStackedTextureDimensions(t));
    return uvw / vec3(getTexture3DSize(t));
}

// tw2de (2026-10-21): the two directions as two variables, not an array indexed at run time: uam put the array in
// local memory (scratch) in every shader that samples a cube map (7,495 of the 12,811 pre-compiled ones: 2-8 KB of
// scratch a warp, and loads and stores of it), and in registers they fold away.
struct CubeMapData
{
    vec3 cubeMapDirection0;
    vec3 cubeMapDirection1;
    uint cubeMapIndex;
};
#define CUBE_MAP_DATA_ZERO CubeMapData(vec3(0.0), vec3(0.0), 0u)
vec3 cubeMapDirection(CubeMapData d, float index) { return index < 0.5 ? d.cubeMapDirection0 : d.cubeMapDirection1; }

vec4 tfetchCube(uint t, uint s, vec3 uvw, inout CubeMapData cubeMapData)
{
    return texture(samplerCube(dkh(t, s)), cubeMapDirection(cubeMapData, uvw.z));
}

vec4 tfetchCubeLevel(uint t, uint s, vec3 uvw, inout CubeMapData cubeMapData, float lod)
{
    return textureLod(samplerCube(dkh(t, s)), cubeMapDirection(cubeMapData, uvw.z), lod);
}

vec4 tfetchCube(samplerCube tex, vec3 uvw, inout CubeMapData cubeMapData)
{
    return texture(tex, cubeMapDirection(cubeMapData, uvw.z));
}

vec4 tfetchCubeLevel(samplerCube tex, vec3 uvw, inout CubeMapData cubeMapData, float lod)
{
    return textureLod(tex, cubeMapDirection(cubeMapData, uvw.z), lod);
}

// tw2de (2026-10-24): the direction stored by selects, not a branch: the index is a constant at nearly every call
// (the shader's first cube() is index 0), and uam's full pass left the if in (its late path cut it).
vec4 cube(vec4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    bool first = index == 0u;
    cubeMapData.cubeMapDirection0 = first ? value.xyz : cubeMapData.cubeMapDirection0;
    cubeMapData.cubeMapDirection1 = first ? cubeMapData.cubeMapDirection1 : value.xyz;
    ++cubeMapData.cubeMapIndex;
    return vec4(0.0, 0.0, 0.0, float(index));
}

#ifdef TW2DE_STAGE_PIXEL
// getCompTexLOD: the LOD the hardware would select (result in .x); textureQueryLod's y is the unclamped one.
vec4 getCompTexLod2D(uint t, uint s, vec2 uv, vec2 offset)
{
    return vec4(textureQueryLod(sampler2D(dkh(t, s)), uv + offset / vec2(getTexture2DSize(t))).y, 0.0, 0.0, 0.0);
}

vec4 getCompTexLod2D(sampler2D tex, ivec2 size, vec2 uv, vec2 offset)
{
    return vec4(textureQueryLod(tex, uv + offset / vec2(size)).y, 0.0, 0.0, 0.0);
}

vec4 getCompTexLodCube(samplerCube tex, vec3 uvw, inout CubeMapData cubeMapData)
{
    return vec4(textureQueryLod(tex, cubeMapDirection(cubeMapData, uvw.z)).y, 0.0, 0.0, 0.0);
}

vec4 getCompTexLod3D(uint t, uint s, vec3 uvw, vec3 offset)
{
    if (isStackedTexture(t))
    {
        uvec3 d = getStackedTextureDimensions(t);
        return vec4(textureQueryLod(sampler2DArray(dkh(t, s)), uvw.xy + offset.xy / vec2(d.xy)).y, 0.0, 0.0, 0.0);
    }
    return vec4(textureQueryLod(sampler3D(dkh(t, s)), uvw + offset / vec3(getTexture3DSize(t))).y, 0.0, 0.0, 0.0);
}

vec4 getCompTexLodCube(uint t, uint s, vec3 uvw, inout CubeMapData cubeMapData)
{
    return vec4(textureQueryLod(samplerCube(dkh(t, s)), cubeMapDirection(cubeMapData, uvw.z)).y, 0.0, 0.0, 0.0);
}
#endif

// ---- ALU helpers
// Xenos rcpf / rsqf: infinities become 0 rather than clamping to FLT_MAX.
float rcpf(float value)
{
    float result = 1.0 / value;
    return select(isinf(result), 0.0, result);
}

float rsqf(float value)
{
    float result = inversesqrt(value);
    return select(isinf(result), 0.0, result);
}

vec4 dst(vec4 src0, vec4 src1) { return vec4(1.0, src0.y * src1.y, src0.z, src1.w); }

float max4(vec4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }

// ---- pixel shader exits
#ifdef TW2DE_STAGE_PIXEL
// tw2de (2026-10-23, pass alpha): the alpha test's function as specialization constant 903, its value + 1 (0: read
// at run time). render/pipeline.cpp gives it from the pipeline key (tw2de_deko3d_alpha_spec), so a draw without the
// alpha test (function 7, always) has no compare and no discard, and keeps the GPU's early depth test.
// 2026-10-23: in the pixel section (only pixel shaders test alpha): a vertex shader has no constant 903, so its draws
// with another alpha test are no variants of it (plume_deko3d_spec_defaults.h).
#ifndef SPIRV_CROSS_CONSTANT_ID_903
#define SPIRV_CROSS_CONSTANT_ID_903 0u
#endif
const uint g_fnxf_xiii_lrAlphaFunc = SPIRV_CROSS_CONSTANT_ID_903;
#define g_AlphaTestFuncRuntime     RENUT_SHARED_UINT(556)
#define g_AlphaTestFunc            ((g_fnxf_xiii_lrAlphaFunc != 0u) ? (g_fnxf_xiii_lrAlphaFunc - 1u) : g_AlphaTestFuncRuntime)
#ifndef SPIRV_CROSS_CONSTANT_ID_900
#define SPIRV_CROSS_CONSTANT_ID_900 0u
#endif
const uint g_fnxf_xiii_lrBlendSkip = SPIRV_CROSS_CONSTANT_ID_900;
#ifndef SPIRV_CROSS_CONSTANT_ID_901
#define SPIRV_CROSS_CONSTANT_ID_901 0u
#endif
const uint g_fnxf_xiii_lrOutputTrim = SPIRV_CROSS_CONSTANT_ID_901;
#ifndef SPIRV_CROSS_CONSTANT_ID_902
#define SPIRV_CROSS_CONSTANT_ID_902 0u
#endif
const uint g_fnxf_xiii_lrTrimPoison = SPIRV_CROSS_CONSTANT_ID_902;

// tw2de (2026-10-23, pass alphamask): with the function read at run time (constant 903 at 0) the compare as the
// function's bit mask: D3D's functions are less (1) | equal (2) | greater (4); 0 never, 7 always. With a NaN no compare
// holds: only not-equal (5) and always (7) pass, as in the chain below.
bool fnxf_xiii_lrAlphaPassMask(uint f, float a, float r)
{
    uint m = mix(mix(mix(5u, 2u, a == r), 4u, a > r), 1u, a < r);
    return mix((f & m) != 0u, (f & 5u) == 5u, m == 5u);
}

void alphaTest(float alpha, uint func, float reference)
{
    if (g_fnxf_xiii_lrAlphaFunc == 0u)
    {
        if (!fnxf_xiii_lrAlphaPassMask(func, alpha, reference))
            discard;
        return;
    }
    bool pass = select(func == 0u, false,
                select(func == 1u, alpha < reference,
                select(func == 2u, alpha == reference,
                select(func == 3u, alpha <= reference,
                select(func == 4u, alpha > reference,
                select(func == 5u, alpha != reference,
                select(func == 6u, alpha >= reference, true)))))));
    if (!pass)
        discard;
}

void fnxf_xiii_lrBlendSkip(vec4 c)
{
    if ((g_fnxf_xiii_lrBlendSkip == 1u && c.w <= 0.0 && !any(isnan(c.xyz))) ||
        (g_fnxf_xiii_lrBlendSkip == 2u && all(lessThanEqual(c, vec4(0.0)))))
        discard;
}

vec4 fnxf_xiii_lrTrim(vec4 c, uint target)
{
    uint m = (g_fnxf_xiii_lrOutputTrim >> (target * 4u)) & 0xFu;
    float v = g_fnxf_xiii_lrTrimPoison != 0u ? 60000.0 : 0.0;
    return mix(c, vec4(v), bvec4((m & 1u) != 0u, (m & 2u) != 0u, (m & 4u) != 0u, (m & 8u) != 0u));
}

bool fnxf_xiii_lrAlphaFailsEarlyRt(float alpha, uint func, float reference)
{
    float d = (abs(alpha) + abs(reference)) * (1.0 / 4096.0) + 1.0e-30;
    return select(func == 0u, true,
           select(func == 1u, alpha - d >= reference,
           select(func == 2u, abs(alpha - reference) > d,
           select(func == 3u, alpha - d > reference,
           select(func == 4u, alpha + d <= reference,
           select(func == 6u, alpha + d < reference, false))))));
}

void fnxf_xiii_lrEarlyAlphaQuad(bool fails)
{
    float f = select(fails, 1.0, 0.0);
    float dx = dFdxFine(f);
    float pair = select(f == 1.0 && dx == 0.0, 1.0, 0.0);
    float dy = dFdyFine(pair);
    if (pair == 1.0 && dy == 0.0)
        discard;
}
#endif

// ---- integer division: Maxwell has none, uam lowers one to float math that can be off by one.
uint fnxf_xiii_lrUDiv(uint a, uint b)
{
    float below = 0.99999;
    uint q = uint(float(a) / float(b) * below);
    uint r = a - q * b;
    q += uint(float(r) / float(b) * below);
    r = a - q * b;
    // At most two steps for any 32-bit a and b > 0 (shader_common.h): a bounded loop, which the compiler unrolls and,
    // where the quotient is not used, drops.
    for (int i = 0; i < 2; i++)
    {
        if (r >= b)
        {
            q += 1u;
            r -= b;
        }
    }
    return q;
}
#define TW2DE_UDIV(a, b) fnxf_xiii_lrUDiv((a), (b))

// ---- vertex shaders: fetches from memory, tessellation, memexport
#ifdef TW2DE_STAGE_VERTEX
#ifdef TW2DE_USES_WINDOW
// The 4 GB window of GPU addresses holding every buffer a shader addresses (its start has its low 32 bits zero, so an
// address's low word is the byte offset): read only.
layout(binding = 15, std430) readonly buffer DkAddressWindow { uint data[]; } g_DkAddressWindow;
#define TW2DE_DK_ADDRESS_LOW(o) RENUT_SHARED_UINT(o)

uint loadVertexFetchData(uint byteOffset)
{
    return g_DkAddressWindow.data[(TW2DE_DK_ADDRESS_LOW(560) + byteOffset) >> 2u];
}

uvec4 loadVertexFetchData4(uint byteOffset)
{
    uint i = (TW2DE_DK_ADDRESS_LOW(560) + byteOffset) >> 2u;
    return uvec4(g_DkAddressWindow.data[i], g_DkAddressWindow.data[i + 1u], g_DkAddressWindow.data[i + 2u],
        g_DkAddressWindow.data[i + 3u]);
}

uint vfetchSwap(uint value, uint endian)
{
    uint swap8in16 = ((value & 0x00FF00FFu) << 8u) | ((value >> 8u) & 0x00FF00FFu);
    uint swap16in32 = (value << 16u) | (value >> 16u);
    uint swap8in32 = (swap8in16 << 16u) | (swap8in16 >> 16u);
    return select(endian == 1u, swap8in16, select(endian == 2u, swap8in32, select(endian == 3u, swap16in32, value)));
}

float vfetchComponent(uint value, uint shift, uint bits, bool isSigned, bool isInteger, bool signedRfMode)
{
    uint raw = bits == 32u ? value : (value >> shift) & ((1u << bits) - 1u);
    if (isSigned)
    {
        // tw2de (2026-10-23, pass sext): the field sign-extended in one BFE (GLSL's extract of an int extends bit
        // bits - 1, as the shift pair did).
        int s = bits == 32u ? int(raw) : bitfieldExtract(int(value), int(shift), int(bits));
        if (isInteger)
            return float(s);
        float maxValue = bits == 32u ? 2147483647.0 : float((1u << (bits - 1u)) - 1u);
        return signedRfMode ? (float(s) * 2.0 + 1.0) / (maxValue * 2.0 + 1.0) : max(float(s) / maxValue, -1.0);
    }
    if (isInteger)
        return float(raw);
    return float(raw) / (bits == 32u ? 4294967295.0 : float((1u << bits) - 1u));
}

float f16tof32(uint v) { return unpackHalf2x16(v).x; }

#ifndef TW2DE_VFETCH_ROWS
#define TW2DE_VFETCH_ROWS true
#endif
vec4 vfetchMemoryImpl(uint slot, float indexValue, bool roundIndex, int offsetDwords, uint strideDwords,
    uint format, bool isSigned, bool isInteger, bool signedRfMode, int expAdjust, bool rowsAllowed, bool clampAddress,
    uint fetchBytes)
{
    int index = int(roundIndex ? floor(indexValue + 0.5) : floor(indexValue));
    uint address = (slot & ~0x3u) + uint(index * int(strideDwords) + offsetDwords) * 4u;
    if (clampAddress)
        address = min(address, max(fetchBytes, 16u) - 16u);
    uint endian = slot & 0x3u;

    uvec4 w;
    if (TW2DE_VFETCH_ROWS && rowsAllowed && ((uint(offsetDwords) | strideDwords) & 3u) == 0u)
    {
        uvec4 v = loadVertexFetchData4(address);
        w = uvec4(vfetchSwap(v.x, endian), vfetchSwap(v.y, endian), vfetchSwap(v.z, endian), vfetchSwap(v.w, endian));
    }
    else
    {
        w.x = vfetchSwap(loadVertexFetchData(address), endian);
        w.y = vfetchSwap(loadVertexFetchData(address + 4u), endian);
        w.z = vfetchSwap(loadVertexFetchData(address + 8u), endian);
        w.w = vfetchSwap(loadVertexFetchData(address + 12u), endian);
    }

    vec4 result = vec4(0.0, 0.0, 0.0, 1.0);
    bool isFloat = false;
    switch (format)
    {
    case 6u: // 8_8_8_8
        result = vec4(vfetchComponent(w.x, 0u, 8u, isSigned, isInteger, signedRfMode), vfetchComponent(w.x, 8u, 8u, isSigned, isInteger, signedRfMode),
            vfetchComponent(w.x, 16u, 8u, isSigned, isInteger, signedRfMode), vfetchComponent(w.x, 24u, 8u, isSigned, isInteger, signedRfMode));
        break;
    case 7u: // 2_10_10_10
        result = vec4(vfetchComponent(w.x, 0u, 10u, isSigned, isInteger, signedRfMode), vfetchComponent(w.x, 10u, 10u, isSigned, isInteger, signedRfMode),
            vfetchComponent(w.x, 20u, 10u, isSigned, isInteger, signedRfMode), vfetchComponent(w.x, 30u, 2u, isSigned, isInteger, signedRfMode));
        break;
    case 16u: // 10_11_11
        result.xyz = vec3(vfetchComponent(w.x, 0u, 11u, isSigned, isInteger, signedRfMode), vfetchComponent(w.x, 11u, 11u, isSigned, isInteger, signedRfMode),
            vfetchComponent(w.x, 22u, 10u, isSigned, isInteger, signedRfMode));
        break;
    case 17u: // 11_11_10
        result.xyz = vec3(vfetchComponent(w.x, 0u, 10u, isSigned, isInteger, signedRfMode), vfetchComponent(w.x, 10u, 11u, isSigned, isInteger, signedRfMode),
            vfetchComponent(w.x, 21u, 11u, isSigned, isInteger, signedRfMode));
        break;
    case 25u: // 16_16
        result.xy = vec2(vfetchComponent(w.x, 0u, 16u, isSigned, isInteger, signedRfMode), vfetchComponent(w.x, 16u, 16u, isSigned, isInteger, signedRfMode));
        break;
    case 26u: // 16_16_16_16
        result = vec4(vfetchComponent(w.x, 0u, 16u, isSigned, isInteger, signedRfMode), vfetchComponent(w.x, 16u, 16u, isSigned, isInteger, signedRfMode),
            vfetchComponent(w.y, 0u, 16u, isSigned, isInteger, signedRfMode), vfetchComponent(w.y, 16u, 16u, isSigned, isInteger, signedRfMode));
        break;
    case 31u: // 16_16_FLOAT
        result.xy = vec2(f16tof32(w.x & 0xFFFFu), f16tof32(w.x >> 16u));
        isFloat = true;
        break;
    case 32u: // 16_16_16_16_FLOAT
        result = vec4(f16tof32(w.x & 0xFFFFu), f16tof32(w.x >> 16u), f16tof32(w.y & 0xFFFFu), f16tof32(w.y >> 16u));
        isFloat = true;
        break;
    case 33u: // 32
        result.x = vfetchComponent(w.x, 0u, 32u, isSigned, isInteger, signedRfMode);
        break;
    case 34u: // 32_32
        result.xy = vec2(vfetchComponent(w.x, 0u, 32u, isSigned, isInteger, signedRfMode), vfetchComponent(w.y, 0u, 32u, isSigned, isInteger, signedRfMode));
        break;
    case 35u: // 32_32_32_32
        result = vec4(vfetchComponent(w.x, 0u, 32u, isSigned, isInteger, signedRfMode), vfetchComponent(w.y, 0u, 32u, isSigned, isInteger, signedRfMode),
            vfetchComponent(w.z, 0u, 32u, isSigned, isInteger, signedRfMode), vfetchComponent(w.w, 0u, 32u, isSigned, isInteger, signedRfMode));
        break;
    case 36u: // 32_FLOAT
        result.x = uintBitsToFloat(w.x);
        isFloat = true;
        break;
    case 37u: // 32_32_FLOAT
        result.xy = uintBitsToFloat(w.xy);
        isFloat = true;
        break;
    case 38u: // 32_32_32_32_FLOAT
        result = uintBitsToFloat(w);
        isFloat = true;
        break;
    case 57u: // 32_32_32_FLOAT
        result.xyz = uintBitsToFloat(w.xyz);
        isFloat = true;
        break;
    }

    if (!isFloat && expAdjust != 0)
        result *= exp2(float(expAdjust));

    return result;
}

vec4 vfetchMemory(uint slot, float indexValue, bool roundIndex, int offsetDwords, uint strideDwords,
    uint format, bool isSigned, bool isInteger, bool signedRfMode, int expAdjust)
{
    return vfetchMemoryImpl(slot, indexValue, roundIndex, offsetDwords, strideDwords, format, isSigned, isInteger,
        signedRfMode, expAdjust, true, false, 0u);
}

#define vfetchMemoryClamped(slot, indexValue, roundIndex, offsetDwords, strideDwords, format, isSigned, isInteger, \
    signedRfMode, expAdjust) \
    vfetchMemoryImpl(slot, indexValue, roundIndex, offsetDwords, strideDwords, format, isSigned, isInteger, \
        signedRfMode, expAdjust, true, true, g_VertexFetchBytes)

vec4 vfetchBound(uvec4 element, uint slot, float indexValue, bool roundIndex, bool signedRfMode, int expAdjust)
{
    vec4 value = vfetchMemoryImpl(slot, indexValue, roundIndex, int(element.y), element.z, element.w & 0x3Fu,
        (element.w & 0x100u) != 0u, (element.w & 0x200u) != 0u, signedRfMode, expAdjust, false, false, 0u);
    vec4 result;
    for (uint i = 0u; i < 4u; i++)
    {
        uint swizzle = (element.w >> (10u + 3u * i)) & 7u;
        result[i] = swizzle < 4u ? value[swizzle] : (swizzle == 5u ? 1.0 : 0.0);
    }
    return result;
}
#endif

// tw2de (2026-10-23, pass tess): the patch kind (SharedConstants.tessellation[0], 0 for a draw that is not
// tessellated) as specialization constant 904, its value + 1 (0: read at run time). render/pipeline.cpp gives 1 to
// every pipeline whose key is not a tessellated draw's (tw2de_deko3d_tess_spec), so the patch-grid code folds away.
#ifndef SPIRV_CROSS_CONSTANT_ID_904
#define SPIRV_CROSS_CONSTANT_ID_904 0u
#endif
const uint g_fnxf_xiii_lrTessKind = SPIRV_CROSS_CONSTANT_ID_904;

void renutVertexRegisters(uint vertexId, uint instanceId, uvec4 tessellationWords, out vec4 r0, out vec4 r1)
{
    uvec4 tessellation = tessellationWords;
    if (g_fnxf_xiii_lrTessKind != 0u)
        tessellation.x = g_fnxf_xiii_lrTessKind - 1u;
    r0 = vec4(float(vertexId + tessellation.w), 0.0, 0.0, 0.0);
    r1 = vec4(0.0);
    if (tessellation.x == 0u)
        return;

    uint level = max(tessellation.y, 1u);
    float patchIndex = float(tessellation.z + instanceId);
    if (tessellation.x == 3u)
    {
        uint a = uint((sqrt(float(8u * vertexId + 1u)) - 1.0) * 0.5);
        if (a * (a + 1u) / 2u > vertexId)
            --a;
        else if ((a + 1u) * (a + 2u) / 2u <= vertexId)
            ++a;
        uvec2 vertex = uvec2(a, vertexId - a * (a + 1u) / 2u);
        vec3 weights = vec3(float(level - vertex.x), float(vertex.x - vertex.y), float(vertex.y)) / float(level);
        r0.xyz = weights.zyx;
        r1.x = patchIndex;
        return;
    }
    if (tessellation.x == 4u)
    {
        uint row = TW2DE_UDIV(vertexId, level + 1u);
        uvec2 position = uvec2(vertexId - row * (level + 1u), row);
        vec2 uv = vec2(position) / float(level);
        r0 = vec4(patchIndex, uv, 0.0);
        return;
    }
    uint triangleIndex = vertexId / 3u;
    uint corner = vertexId % 3u;
    if (tessellation.x == 1u)
    {
        uint row = uint(sqrt(float(triangleIndex)));
        if (row * row > triangleIndex)
            --row;
        else if ((row + 1u) * (row + 1u) <= triangleIndex)
            ++row;
        uint k = triangleIndex - row * row;
        uint b = k / 2u;
        uvec2 vertex;
        if ((k & 1u) == 0u)
            vertex = corner == 0u ? uvec2(row, b) : (corner == 1u ? uvec2(row + 1u, b) : uvec2(row + 1u, b + 1u));
        else
            vertex = corner == 0u ? uvec2(row, b) : (corner == 1u ? uvec2(row + 1u, b + 1u) : uvec2(row, b + 1u));
        vec3 weights = vec3(float(level - vertex.x), float(vertex.x - vertex.y), float(vertex.y)) / float(level);
        r0.xyz = weights.zyx;
        r1.x = patchIndex;
    }
    else
    {
        uint cell = triangleIndex / 2u;
        uint cellRow = TW2DE_UDIV(cell, level);
        uvec2 position = uvec2(cell - cellRow * level, cellRow);
        uvec2 offset;
        if ((triangleIndex & 1u) == 0u)
            offset = corner == 0u ? uvec2(0u, 0u) : (corner == 1u ? uvec2(1u, 0u) : uvec2(1u, 1u));
        else
            offset = corner == 0u ? uvec2(0u, 0u) : (corner == 1u ? uvec2(1u, 1u) : uvec2(0u, 1u));
        vec2 uv = vec2(position + offset) / float(level);
        r0 = vec4(patchIndex, uv, 0.0);
    }
}

#ifdef TW2DE_USES_MEMEXPORT
// The draw's memexport buffer (draw.cpp binds it at storage binding 14).
layout(binding = 14, std430) buffer DkMemExport { uint data[]; } g_DkMemExport;
// tw2de (2026-10-30, tw2de_memexport_zero_copy): when the exports go to guest memory itself (g_DkMemExport is
// then the GPU's mapping of all of it, and a stream's offset its physical address), the page flags live here: a word
// for each 4 KB page of physical memory, set to 1 when a kernel writes the page. Bit 1 of the stream's flag word
// (draw.cpp) selects it. Bound at storage binding 12 by draw.cpp for every draw with exports.
layout(binding = 12, std430) buffer DkMemExportFlags { uint data[]; } g_DkMemExportFlags;

void memExportStore(uint offset, uint value) { g_DkMemExport.data[offset >> 2u] = value; }

// tw2de (2026-10-29): the export buffer's write mask, a bit for each dword of the buffer a kernel wrote: the words
// from 1 MB on (after the page flags: draw.cpp kExportFlagPrefix / kExportMaskPrefix), the bit of dword d at word
// d / 32, bit d % 32, for the first 256 MB of the buffer (draw.cpp kExportMaskCover: the windows end at
// tw2de_memexport_slot_mb, 128 MB, and PrepareMemExportLocked refuses a window past the cover, so no check here).
// The CPU takes back to guest memory only the dwords the mask has, so a window needs no copy of its seed to compare the
// results with (draw.cpp CopyPage): the 32 MB reference copies a window a frame, and the seed blocks that held them,
// are gone. About 12 instructions an export, one atomic (a plain RED on Maxwell).
#define TW2DE_MEMEXPORT_MASK_WORDS 262144u
void memExportMark(uint offset, uint dwords)
{
    uint d = offset >> 2u;
    uint lo = d & 31u;
    uint bits = (1u << dwords) - 1u;
    atomicOr(g_DkMemExport.data[TW2DE_MEMEXPORT_MASK_WORDS + (d >> 5u)], bits << lo);
    if (lo + dwords > 32u)  // an element of 2 or 4 dwords across two mask words (a stream 4-aligned, not 8 or 16)
        atomicOr(g_DkMemExport.data[TW2DE_MEMEXPORT_MASK_WORDS + (d >> 5u) + 1u], bits >> (32u - lo));
}

uint f32tof16(float v) { return packHalf2x16(vec2(v, 0.0)) & 0xFFFFu; }

// tw2de (2026-10-27): no arrays indexed at run time (the element's words and widths, the four streams): uam put
// them in local memory (16 KB of scratch a warp) and the eight exports of the game's copy kernel (VS BE95EE803D59E654)
// took 0.8 us a vertex, 0.2-1 s of GPU time a frame in its area loads. One component packed (a width of 2-16 or 32).
uint memExportPack(float v, uint width, bool isSigned, bool isInteger)
{
    if (width == 32u)
        return isInteger ? (isSigned ? uint(int(v)) : uint(max(v, 0.0)))
                         : (isSigned ? uint(int(clamp(v, -1.0, 1.0) * 2147483647.0))
                                     : uint(clamp(v, 0.0, 1.0) * 4294967295.0));
    float top = isSigned ? float((1u << (width - 1u)) - 1u) : float((1u << width) - 1u);
    float scaled = isInteger ? v : round((isSigned ? clamp(v, -1.0, 1.0) : clamp(v, 0.0, 1.0)) * top);
    return uint(int(scaled)) & ((1u << width) - 1u);
}

// The element's words (w0-w3) and their count (1, 2 or 4; 0: a format not handled). The float formats directly; the
// others by their components' widths (one packing path for all of them: a case for each made the eight exports of a
// shader twice as long).
uint memExportElement(vec4 data, uint format, uint numFormat, out uint w0, out uint w1, out uint w2, out uint w3)
{
    w0 = 0u;
    w1 = 0u;
    w2 = 0u;
    w3 = 0u;
    if (format == 36u || format == 37u || format == 38u)
    {
        w0 = floatBitsToUint(data.x);
        w1 = floatBitsToUint(data.y);
        w2 = floatBitsToUint(data.z);
        w3 = floatBitsToUint(data.w);
        return format == 36u ? 1u : (format == 37u ? 2u : 4u);
    }
    if (format == 31u || format == 32u)
    {
        w0 = f32tof16(data.x) | (f32tof16(data.y) << 16u);
        w1 = f32tof16(data.z) | (f32tof16(data.w) << 16u);
        return format == 31u ? 1u : 2u;
    }
    // Widths of x, y, z, w (0: none) and the words: 6 8_8_8_8, 7 2_10_10_10, 25 16_16, 26 16_16_16_16, 33 32,
    // 34 32_32, 35 32_32_32_32.
    uvec4 width;
    uint dwords;
    if (format == 6u) { width = uvec4(8u, 8u, 8u, 8u); dwords = 1u; }
    else if (format == 7u) { width = uvec4(10u, 10u, 10u, 2u); dwords = 1u; }
    else if (format == 25u) { width = uvec4(16u, 16u, 0u, 0u); dwords = 1u; }
    else if (format == 26u) { width = uvec4(16u, 16u, 16u, 16u); dwords = 2u; }
    else if (format == 33u) { width = uvec4(32u, 0u, 0u, 0u); dwords = 1u; }
    else if (format == 34u) { width = uvec4(32u, 32u, 0u, 0u); dwords = 2u; }
    else if (format == 35u) { width = uvec4(32u, 32u, 32u, 32u); dwords = 4u; }
    else return 0u;
    bool s = (numFormat & 1u) != 0u;
    bool n = (numFormat & 2u) != 0u;
    // Each component at its bit: the word bit / 32, the shift bit % 32 (a component never spans two words).
    uint bit = 0u;
    for (int c = 0; c < 4; c++)
    {
        uint wc = c == 0 ? width.x : (c == 1 ? width.y : (c == 2 ? width.z : width.w));
        float v = c == 0 ? data.x : (c == 1 ? data.y : (c == 2 ? data.z : data.w));
        uint bits = wc == 0u ? 0u : memExportPack(v, wc, s, n) << (bit & 31u);
        uint word = bit >> 5u;
        w0 |= word == 0u ? bits : 0u;
        w1 |= word == 1u ? bits : 0u;
        w2 |= word == 2u ? bits : 0u;
        w3 |= word == 3u ? bits : 0u;
        bit += wc;
    }
    return dwords;
}

uint memExportSwap(uint value, uint endian)
{
    if (endian == 2u)
        return (value >> 24u) | ((value >> 8u) & 0xFF00u) | ((value << 8u) & 0xFF0000u) | (value << 24u);
    if (endian == 1u)
        return ((value >> 8u) & 0x00FF00FFu) | ((value << 8u) & 0xFF00FF00u);
    if (endian == 3u)
        return (value >> 16u) | (value << 16u);
    return value;
}

// 'stream' (base, size, offset in the export buffer, offset of its page flags or 0) holds [address, address + bytes):
// its offset there and its page's flag, the first such stream's ('found' once one did).
void memExportFind(uvec4 stream, uint address, uint bytes, inout bool found, inout uint offset, inout uint flag)
{
    bool inside = !found && address >= stream.x && address + bytes <= stream.x + stream.y;
    offset = inside ? stream.z + (address - stream.x) : offset;
    flag = inside ? (stream.w != 0u ? stream.w + ((address - stream.x) >> 12u) * 4u : 0u) : flag;
    found = found || inside;
}

void memExport(vec4 eA, vec4 data, uint element, uvec4 stream0, uvec4 stream1, uvec4 stream2, uvec4 stream3)
{
    float firstIndex = eA.y - 8388608.0;
    float count = eA.w - 8388608.0;
    if (!(firstIndex >= 0.0) || !(count >= 0.0))
        return;
    uint index = uint(firstIndex) + element;
    if (index >= uint(count))
        return;

    uint info = floatBitsToUint(eA.z);
    uint w0, w1, w2, w3;
    uint dwords = memExportElement(data, (info >> 8u) & 0x3Fu, (info >> 16u) & 7u, w0, w1, w2, w3);
    uint endian = info & 7u;
    if (dwords == 0u || endian > 3u)
        return;

    // The stream first, then the words swapped once and stored once (swapping inside each stream's test made every
    // export sixteen swaps long).
    uint bytes = dwords * 4u;
    uint address = (floatBitsToUint(eA.x) & 0x3FFFFFFFu) * 4u + index * bytes;
    bool found = false;
    uint offset = 0u, flag = 0u;
    memExportFind(stream0, address, bytes, found, offset, flag);
    memExportFind(stream1, address, bytes, found, offset, flag);
    memExportFind(stream2, address, bytes, found, offset, flag);
    memExportFind(stream3, address, bytes, found, offset, flag);
    if (!found)
        return;
    // tw2de (2026-10-27): the 4 KB page written, flagged in its window's page flags (draw.cpp: one word a page after
    // the window): the CPU takes back only the pages a kernel wrote (all 32 MB of every window before, 25-95 ms a wait).
    // tw2de (2026-10-29): bit 0 of the stream's flag offset (draw.cpp, tw2de_memexport_write_mask): the dwords
    // go in the write mask too (without it the mask's place is the windows').
    // tw2de (2026-10-30): bit 1 (tw2de_memexport_zero_copy): the page's flag by its physical page, in the flags
    // buffer; the stores below then land in guest memory (offset is the physical address).
    if ((flag & 2u) != 0u)
    {
        g_DkMemExportFlags.data[address >> 12u] = 1u;
    }
    else if (flag != 0u)
    {
        memExportStore(flag & ~3u, 1u);
        if ((flag & 1u) != 0u)
            memExportMark(offset, dwords);
    }
    memExportStore(offset, memExportSwap(w0, endian));
    if (dwords >= 2u)
        memExportStore(offset + 4u, memExportSwap(w1, endian));
    if (dwords >= 4u)
    {
        memExportStore(offset + 8u, memExportSwap(w2, endian));
        memExportStore(offset + 12u, memExportSwap(w3, endian));
    }
}
#endif
#endif
