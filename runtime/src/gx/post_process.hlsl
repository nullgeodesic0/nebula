// Presentation post-processing passes (post_process.cpp).
//
// post_process.cpp composes one source per pass from this file and the
// vendored libraries, defining exactly one PASS_* macro:
//   POST_SECTION 1        common declarations
//   SMAA.hlsl             (SMAA passes)  or  ffx_a.h, POST_SECTION 2, ffx_fsr1.h (FSR passes)
//   POST_SECTION 3        entry points
//
// Every pass draws one full-target triangle. Pixel positions are relative to
// the target viewport's origin, which present() places inside the letterboxed
// content rectangle of the back buffer.

#if POST_SECTION == 1

cbuffer PostConstants : register(b0) {
    // 1/w, 1/h, w, h of the pass input (SMAA_RT_METRICS).
    float4 g_rt_metrics;
    // FSR 1 constants (FsrEasuCon / FsrRcasCon).
    uint4 g_con0;
    uint4 g_con1;
    uint4 g_con2;
    uint4 g_con3;
    // Resample: xy source size, zw target size in pixels.
    float4 g_params;
    // xy: viewport origin in render-target pixels.
    int4 g_origin;
};

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

struct FullscreenOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

FullscreenOut FullscreenTriangle(uint id) {
    FullscreenOut o;
    o.uv = float2(id == 1u ? 2.0 : 0.0, id == 2u ? 2.0 : 0.0);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}

// Integer pixel inside the pass viewport.
int2 ViewportPixel(float4 pos) {
    return int2(pos.xy) - g_origin.xy;
}

#if defined(PASS_SMAA_EDGES) || defined(PASS_SMAA_WEIGHTS) || defined(PASS_SMAA_BLEND)
#define SMAA_CUSTOM_SL 1
#define SMAA_RT_METRICS g_rt_metrics
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#endif

#if defined(PASS_FSR_EASU) || defined(PASS_FSR_RCAS)
#define A_GPU 1
#define A_HLSL 1
#endif

#elif POST_SECTION == 2

Texture2D inputTex : register(t0);

#if defined(PASS_FSR_EASU)
AF4 FsrEasuRF(AF2 p) { return inputTex.GatherRed(LinearSampler, p, int2(0, 0)); }
AF4 FsrEasuGF(AF2 p) { return inputTex.GatherGreen(LinearSampler, p, int2(0, 0)); }
AF4 FsrEasuBF(AF2 p) { return inputTex.GatherBlue(LinearSampler, p, int2(0, 0)); }
#define FSR_EASU_F 1
#endif

#if defined(PASS_FSR_RCAS)
// The input matches the target size; reads past the edge repeat the border.
AF4 FsrRcasLoadF(ASU2 p) {
    const int2 last = int2(g_params.xy) - 1;
    return inputTex.Load(int3(clamp(int2(p), int2(0, 0), last), 0));
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#define FSR_RCAS_F 1
#endif

#elif POST_SECTION == 3

#if defined(PASS_SMAA_EDGES)
Texture2D colorTex : register(t0);
struct EdgesOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 offset[3] : TEXCOORD1;
};
EdgesOut VSMain(uint id : SV_VertexID) {
    const FullscreenOut f = FullscreenTriangle(id);
    EdgesOut o;
    o.pos = f.pos;
    o.uv = f.uv;
    SMAAEdgeDetectionVS(f.uv, o.offset);
    return o;
}
float2 PSMain(EdgesOut i) : SV_Target {
    return SMAALumaEdgeDetectionPS(i.uv, i.offset, colorTex);
}
#endif

#if defined(PASS_SMAA_WEIGHTS)
Texture2D edgesTex : register(t0);
Texture2D areaTex : register(t1);
Texture2D searchTex : register(t2);
struct WeightsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 pixcoord : TEXCOORD1;
    float4 offset[3] : TEXCOORD2;
};
WeightsOut VSMain(uint id : SV_VertexID) {
    const FullscreenOut f = FullscreenTriangle(id);
    WeightsOut o;
    o.pos = f.pos;
    o.uv = f.uv;
    SMAABlendingWeightCalculationVS(f.uv, o.pixcoord, o.offset);
    return o;
}
float4 PSMain(WeightsOut i) : SV_Target {
    return SMAABlendingWeightCalculationPS(
        i.uv, i.pixcoord, i.offset, edgesTex, areaTex, searchTex, float4(0.0, 0.0, 0.0, 0.0));
}
#endif

#if defined(PASS_SMAA_BLEND)
Texture2D colorTex : register(t0);
Texture2D blendTex : register(t1);
struct BlendOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 offset : TEXCOORD1;
};
BlendOut VSMain(uint id : SV_VertexID) {
    const FullscreenOut f = FullscreenTriangle(id);
    BlendOut o;
    o.pos = f.pos;
    o.uv = f.uv;
    SMAANeighborhoodBlendingVS(f.uv, o.offset);
    return o;
}
float4 PSMain(BlendOut i) : SV_Target {
    return float4(SMAANeighborhoodBlendingPS(i.uv, i.offset, colorTex, blendTex).rgb, 1.0);
}
#endif

#if defined(PASS_FSR_EASU)
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
float4 PSMain(FullscreenOut i) : SV_Target {
    AF3 color;
    FsrEasuF(color, AU2(ViewportPixel(i.pos)), g_con0, g_con1, g_con2, g_con3);
    return float4(color, 1.0);
}
#endif

#if defined(PASS_FSR_RCAS)
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
float4 PSMain(FullscreenOut i) : SV_Target {
    AF3 color;
    FsrRcasF(color.r, color.g, color.b, AU2(ViewportPixel(i.pos)), g_con0);
    return float4(color, 1.0);
}
#endif

#if defined(PASS_FXAA)
// Fast approximate AA (the FXAA quality-preset method, written for Nebula):
// find the local luma edge, walk along it in both directions to its ends,
// and resample the pixel across the edge by its position along the span;
// a 3x3 contrast term softens single-pixel features. Needs no lookup
// textures, so it is the fallback when SMAA is unavailable.
Texture2D inputTex : register(t0);
float FxaaLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float FxaaLumaAt(float2 uv) { return FxaaLuma(inputTex.SampleLevel(LinearSampler, uv, 0).rgb); }
float FxaaLumaOff(float2 uv, int2 o) { return FxaaLuma(inputTex.SampleLevel(PointSampler, uv, 0, o).rgb); }
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
float4 PSMain(FullscreenOut i) : SV_Target {
    const float2 texel = g_rt_metrics.xy;
    const float2 uv = (float2(ViewportPixel(i.pos)) + 0.5) * texel;
    const float3 center = inputTex.SampleLevel(PointSampler, uv, 0).rgb;
    const float lc = FxaaLuma(center);
    const float ln = FxaaLumaOff(uv, int2(0, -1));
    const float ls = FxaaLumaOff(uv, int2(0, 1));
    const float lw = FxaaLumaOff(uv, int2(-1, 0));
    const float le = FxaaLumaOff(uv, int2(1, 0));
    const float lmax = max(lc, max(max(ln, ls), max(lw, le)));
    const float lmin = min(lc, min(min(ln, ls), min(lw, le)));
    const float range = lmax - lmin;
    if (range < max(0.0312, lmax * 0.125)) return float4(center, 1.0);
    const float lnw = FxaaLumaOff(uv, int2(-1, -1));
    const float lne = FxaaLumaOff(uv, int2(1, -1));
    const float lsw = FxaaLumaOff(uv, int2(-1, 1));
    const float lse = FxaaLumaOff(uv, int2(1, 1));
    // Edge orientation from second differences of the 3x3 block.
    const float horizontal = abs(lnw + lne - 2.0 * ln) + 2.0 * abs(lw + le - 2.0 * lc) + abs(lsw + lse - 2.0 * ls);
    const float vertical = abs(lnw + lsw - 2.0 * lw) + 2.0 * abs(ln + ls - 2.0 * lc) + abs(lne + lse - 2.0 * le);
    const bool horz = horizontal >= vertical;
    // The neighbour across the edge with the larger contrast.
    float l1 = horz ? ln : lw;
    float l2 = horz ? ls : le;
    const float g1 = abs(l1 - lc);
    const float g2 = abs(l2 - lc);
    const bool negative = g1 >= g2;
    float across = horz ? texel.y : texel.x;
    float lpair;
    if (negative) { across = -across; lpair = l1; } else { lpair = l2; }
    const float gradient = 0.25 * max(g1, g2);
    const float lavg = 0.5 * (lpair + lc);
    // Walk along the edge, half a pixel over towards the chosen neighbour.
    float2 start = uv;
    if (horz) start.y += 0.5 * across; else start.x += 0.5 * across;
    const float2 along = horz ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    static const float kSteps[10] = {1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0};
    float2 pn = start - along;
    float2 pp = start + along;
    float en = FxaaLumaAt(pn) - lavg;
    float ep = FxaaLumaAt(pp) - lavg;
    bool dn = abs(en) >= gradient;
    bool dp = abs(ep) >= gradient;
    [loop] for (int s = 1; s < 10 && !(dn && dp); ++s) {
        if (!dn) { pn -= along * kSteps[s]; en = FxaaLumaAt(pn) - lavg; dn = abs(en) >= gradient; }
        if (!dp) { pp += along * kSteps[s]; ep = FxaaLumaAt(pp) - lavg; dp = abs(ep) >= gradient; }
    }
    const float distn = horz ? uv.x - pn.x : uv.y - pn.y;
    const float distp = horz ? pp.x - uv.x : pp.y - uv.y;
    const bool nearer_negative = distn < distp;
    const float dist = min(distn, distp);
    const float span = distn + distp;
    // Blend only when the nearer end's luma moves away from the centre's side.
    const bool centre_below = lc - lavg < 0.0;
    const bool correct = ((nearer_negative ? en : ep) < 0.0) != centre_below;
    float offset = correct ? 0.5 - dist / span : 0.0;
    // Sub-pixel term from the 3x3 average.
    const float l3x3 = (2.0 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 12.0;
    const float sub = saturate(abs(l3x3 - lc) / range);
    const float sub2 = (-2.0 * sub + 3.0) * sub * sub;
    offset = max(offset, sub2 * sub2 * 0.75);
    float2 final_uv = uv;
    if (horz) final_uv.y += offset * across; else final_uv.x += offset * across;
    return float4(inputTex.SampleLevel(LinearSampler, final_uv, 0).rgb, 1.0);
}
#endif

#if defined(PASS_SSAO_AO) || defined(PASS_SSAO_BLUR) || defined(PASS_SSAO_APPLY)
// Screen-space ambient occlusion on the EFB at the end of the 3D scene
// (Alchemy-style, after McGuire et al. 2011). Depth is the renderer's reversed
// 24-bit depth: d = 1 at the camera's near plane, 0 at far, linear in 1/z.
// Constants: g_rt_metrics = target (1/w, 1/h, w, h); g_params = the scene
// viewport in target pixels (x, y, w, h); g_con0 = (near, far, A, B) and
// g_con1 = (C, D, radius, strength) as floats, A..D the GX projection terms
// (clip x = A x + B z, clip y = C y + D z); g_con2.x = bias.
//
// The same pass marches contact shadows (screen-space shadows): a short ray from
// each pixel toward the scene's main light, tested against the depth buffer, so
// small occluders (Mario's limbs and cap, ledges, grass, coins) shadow what is
// right behind them. g_con2.yzw = the light's view-space position; g_con3 =
// (shadow strength 0-1, ray length, occluder thickness) as floats, lengths in
// world units. The target holds R = occlusion, G = shadow visibility; blur and
// apply treat both channels alike and the apply multiplies their product.
#endif

#if defined(PASS_SSAO_AO) || defined(PASS_SSAO_BLUR) || defined(PASS_DOF_BLUR) || defined(PASS_DOF_FOCUS) || defined(PASS_SSR)
Texture2D depthTex : register(t0);
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
// Distance in front of the camera, or -1 where nothing was drawn.
float SsaoViewZ(int2 p) {
    if (any(p < 0) || any(float2(p) >= g_rt_metrics.zw)) return -1.0;
    const float d = depthTex.Load(int3(p, 0)).r;
    if (d <= 1e-6) return -1.0;
    const float inv_near = 1.0 / asfloat(g_con0.x);
    const float inv_far = 1.0 / asfloat(g_con0.y);
    return 1.0 / (inv_far + d * (inv_near - inv_far));
}
float3 SsaoViewPos(int2 p, float zv) {
    const float4 vp = g_params;
    const float nx = 2.0 * ((float(p.x) + 0.5) - vp.x) / vp.z - 1.0;
    const float ny = 1.0 - 2.0 * ((float(p.y) + 0.5) - vp.y) / vp.w;
    return float3((nx + asfloat(g_con0.w)) * zv / asfloat(g_con0.z),
                  (ny + asfloat(g_con1.y)) * zv / asfloat(g_con1.x), -zv);
}
bool SsaoInView(int2 p) {
    const float4 vp = g_params;
    return float(p.x) >= vp.x && float(p.y) >= vp.y &&
           float(p.x) < vp.x + vp.z && float(p.y) < vp.y + vp.w;
}
#endif

#if defined(PASS_SSAO_AO) || defined(PASS_SSR)
// The EFB stencil: each depth-writing GX draw writes its material class there
// (kMaterial* in shader_keys.h). 0x80 marks a pixel a GX draw wrote; without it
// (depth restored by a copy or carry) the material is unknown.
Texture2D<uint2> stencilTex : register(t2);
static const uint kMaterialWritten = 0x80u;
static const uint kMaterialLit = 0x01u;
static const uint kMaterialGlossy = 0x02u;
static const uint kMaterialIndirect = 0x04u;
static const uint kMaterialSkinned = 0x08u;
uint SceneMaterial(int2 p) { return stencilTex.Load(int3(p, 0)).g; }
// Target pixel of a view-space point (inverse of SsaoViewPos).
float2 SsaoProject(float3 v) {
    const float zv = -v.z;
    const float nx = asfloat(g_con0.z) * v.x / zv - asfloat(g_con0.w);
    const float ny = asfloat(g_con1.x) * v.y / zv - asfloat(g_con1.y);
    return float2(g_params.x + (nx + 1.0) * 0.5 * g_params.z,
                  g_params.y + (1.0 - ny) * 0.5 * g_params.w);
}
// Camera-facing view-space normal at P (pixel p, distance zv), from the nearer
// neighbour on each axis (keeps silhouettes clean). `slope` receives the depth
// change to the next pixel on P's own surface.
float3 SsaoNormal(int2 p, float zv, float3 P, out float slope) {
    const float zl = SsaoViewZ(p + int2(-1, 0));
    const float zr = SsaoViewZ(p + int2(1, 0));
    const float zu = SsaoViewZ(p + int2(0, -1));
    const float zd = SsaoViewZ(p + int2(0, 1));
    float3 dx = float3(0.0, 0.0, 0.0);
    float3 dy = float3(0.0, 0.0, 0.0);
    if (zr > 0.0 && (zl < 0.0 || abs(zr - zv) < abs(zl - zv))) dx = SsaoViewPos(p + int2(1, 0), zr) - P;
    else if (zl > 0.0) dx = P - SsaoViewPos(p + int2(-1, 0), zl);
    if (zd > 0.0 && (zu < 0.0 || abs(zd - zv) < abs(zu - zv))) dy = SsaoViewPos(p + int2(0, 1), zd) - P;
    else if (zu > 0.0) dy = P - SsaoViewPos(p + int2(0, -1), zu);
    slope = max(abs(dx.z), abs(dy.z));
    float3 N = cross(dx, dy);
    if (dot(N, N) < 1e-12) return -normalize(P);
    N = normalize(N);
    return dot(N, P) > 0.0 ? -N : N;  // face the camera
}
#endif

#if defined(PASS_SSAO_AO)

// Visibility of the light from P (pixel p), 1 = lit. Facing away from the light
// returns 1: the game's own lighting already darkens those surfaces. `slope` is
// the depth change to the next pixel on P's own surface, the tolerance that
// keeps surfaces seen at a grazing angle from shadowing themselves.
// One shadow ray from P toward the light, aimed inside a cone of tangent `cone`
// around the light direction (the jitter that makes the shadow's edge a
// penumbra once the blur pass averages the 4x4 pattern), marched through the
// depth buffer in `steps` steps. `thickness` grows with the ray's own length by
// `growth`, so a long ray still catches thin walls without a short one making
// thick ghosts. Returns the shadow, 0 (lit) to 1 (fully shadowed); an occluder
// near the end of the ray casts a softer, fainter shadow.
float ShadowRay(int2 p, float3 P, float3 N, float zv, float slope, float noise, float3 L,
                float ray_length, float thickness_base, float growth, float cone, int steps,
                bool receiver_skinned) {
    // Lift the start off the surface by about a pixel's depth error so flat
    // ground does not shadow itself.
    const float lift = 0.0015 * zv;
    const float3 origin = P + N * lift;
    // Jitter within the cone: a ring position from the 4x4 pattern.
    const float3 helper = abs(L.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 tangent = normalize(cross(L, helper));
    const float3 bitangent = cross(L, tangent);
    const float angle = noise * 6.2831853;
    const float spread = sqrt(frac(noise * 7.0 + 0.37));
    const float3 dir = normalize(L + (tangent * cos(angle) + bitangent * sin(angle)) * (cone * spread));
    // A ray toward the camera stops at the near plane.
    const float3 ray = dir * ray_length;
    const float reach = ray.z > 0.0 ? saturate((-origin.z - asfloat(g_con0.x)) / ray.z) : 1.0;
    const float3 end = origin + ray * reach;
    // Rays shorter than two pixels on screen cannot find an occluder.
    const float2 span = SsaoProject(end) - SsaoProject(origin);
    if (dot(span, span) < 4.0) return 0.0;

    [loop] for (int k = 0; k < steps; ++k) {
        // Steps grow toward the end of the ray, where precision matters less.
        const float t = (float(k) + noise) / float(steps);
        const float3 S = lerp(origin, end, t * t);
        const int2 q = int2(floor(SsaoProject(S)));
        if (!SsaoInView(q)) break;
        if (all(q == p)) continue;
        const float zq = SsaoViewZ(q);
        if (zq < 0.0) continue;
        // Positive when the depth buffer holds a surface in front of the sample.
        const float delta = -S.z - zq;
        const float thickness = thickness_base + 0.01 * zv + growth * ray_length * t * t;
        if (delta > 0.002 * zq + slope && delta < thickness) {
            // An actor's own parts (Mario's cap over his face) shadow each other
            // only faintly: the hard self-shadow looks like a rendering error.
            const bool self = receiver_skinned && (SceneMaterial(q) & kMaterialSkinned) != 0u;
            return (1.0 - smoothstep(0.55, 1.0, t)) * (self ? 0.2 : 1.0);
        }
    }
    return 0.0;
}

// Visibility of the light from P (pixel p) as the product of two rays: a short
// hard one for contact shadows under small occluders, and a long jittered one
// for soft, widening shadows of larger ones (a tree on the grass, a wall across
// the path). 1 = lit.
float ContactShadow(int2 p, float3 P, float3 N, float zv, float slope, float noise, uint material) {
    const float strength = asfloat(g_con3.x);
    if (strength <= 0.0) return 1.0;
    const float3 L = normalize(float3(asfloat(g_con2.y), asfloat(g_con2.z), asfloat(g_con2.w)) - P);
    const float ndl = dot(N, L);
    if (ndl <= 0.0) return 1.0;
    const bool skinned = (material & kMaterialWritten) != 0u && (material & kMaterialSkinned) != 0u;
    // Characters get a softer contact ray (a wider cone spreads the edge into a
    // penumbra) as well as the faint self-shadow in ShadowRay.
    const float contact = ShadowRay(p, P, N, zv, slope, noise, L, asfloat(g_con3.y),
                                    asfloat(g_con3.z), 0.0, skinned ? 0.04 : 0.006, 16, skinned);
    const float long_length = asfloat(g_con3.w);
    float far_shadow = 0.0;
    if (long_length > 0.0) {
        // Different jitter phase than the contact ray, so the two do not alias.
        far_shadow = ShadowRay(p, P, N, zv, slope, frac(noise * 3.0 + 0.5), L, long_length,
                               asfloat(g_con3.z), 0.12, 0.07, 24, skinned) * 0.85;
    }
    const float visible = (1.0 - contact * strength) * (1.0 - far_shadow * strength);
    // Near grazing angles the game's lighting is already dim; fade the shadow in
    // with N.L so terminators do not get a hard double edge.
    return lerp(1.0, visible, saturate(ndl * 4.0));
}

float4 PSMain(FullscreenOut i) : SV_Target {
    const int2 p = int2(i.pos.xy);
    if (!SsaoInView(p)) return float4(1.0, 1.0, 1.0, 1.0);
    const float zv = SsaoViewZ(p);
    // Nothing drawn, or the far-plane sky.
    if (zv < 0.0 || zv > 0.98 * asfloat(g_con0.y)) return float4(1.0, 1.0, 1.0, 1.0);
    const float3 P = SsaoViewPos(p, zv);

    float slope;
    const float3 N = SsaoNormal(p, zv, P, slope);

    // A 4x4 ordered pattern that the blur pass averages away exactly.
    static const float kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const float noise = kBayer[(p.y & 3) * 4 + (p.x & 3)] / 16.0 + 0.03125;
    // Only surfaces the game lights take contact shadows: glows, the sky and
    // other unlit materials are their own light.
    const uint material = SceneMaterial(p);
    const bool lit = (material & kMaterialWritten) == 0u || (material & kMaterialLit) != 0u;
    const float shadow = lit ? ContactShadow(p, P, N, zv, slope, noise, material) : 1.0;

    const float radius = asfloat(g_con1.z);
    const float strength = asfloat(g_con1.w);
    if (strength <= 0.0) return float4(1.0, shadow, 1.0, 1.0);
    const float bias = asfloat(g_con2.x);
    const float radius_px = clamp(radius * asfloat(g_con1.x) / zv * 0.5 * g_params.w, 3.0, 96.0);
    const float angle0 = noise * 6.2831853;
    const int kSamples = 12;
    float occlusion = 0.0;
    [unroll] for (int k = 0; k < kSamples; ++k) {
        const float t = (float(k) + 0.5) / float(kSamples);
        const float angle = angle0 + float(k) * 2.39996323;
        const float r = radius_px * sqrt(t);
        const int2 q = p + int2(round(float2(cos(angle), sin(angle)) * r));
        const float zq = SsaoViewZ(q);
        if (zq < 0.0 || !SsaoInView(q)) continue;
        const float3 v = SsaoViewPos(q, zq) - P;
        const float d2 = dot(v, v);
        const float range = 1.0 - saturate(d2 / (4.0 * radius * radius));
        occlusion += max(0.0, dot(v, N) - bias * zv) / (d2 + 1e-4 * radius * radius) * range;
    }
    const float ao = saturate(1.0 - occlusion * radius * 2.0 / float(kSamples));
    const float shaped = lerp(1.0, ao * ao, strength);
    return float4(shaped, shadow, 1.0, 1.0);
}
#endif

#if defined(PASS_SSAO_BLUR)
Texture2D aoTex : register(t1);
float4 PSMain(FullscreenOut i) : SV_Target {
    const int2 p = int2(i.pos.xy);
    const float zc = SsaoViewZ(p);
    if (zc < 0.0 || !SsaoInView(p)) return float4(1.0, 1.0, 1.0, 1.0);
    float2 sum = float2(0.0, 0.0);
    float weight = 0.0;
    [unroll] for (int y = -2; y < 2; ++y) {
        [unroll] for (int x = -2; x < 2; ++x) {
            const int2 q = p + int2(x, y);
            const float zq = SsaoViewZ(q);
            if (zq < 0.0 || !SsaoInView(q) || abs(zq - zc) > 0.04 * zc) continue;
            sum += aoTex.Load(int3(q, 0)).rg;
            weight += 1.0;
        }
    }
    const float2 ao = weight > 0.0 ? sum / weight : float2(1.0, 1.0);
    return float4(ao, 1.0, 1.0);
}
#endif

#if defined(PASS_SSAO_APPLY)
// Multiplied onto the EFB colour by the pipeline's blend state.
Texture2D aoTex : register(t0);
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
float4 PSMain(FullscreenOut i) : SV_Target {
    const float2 terms = aoTex.Load(int3(int2(i.pos.xy), 0)).rg;
    // Shadows take a cool tint, the way a lit scene's shade picks up the sky's
    // colour: a grey multiply looks dead. Full shadow tints fully.
    const float3 tint = lerp(float3(0.74, 0.84, 1.0), float3(1.0, 1.0, 1.0), terms.g);
    return float4(terms.r * terms.g * tint, 1.0);
}
#endif

#if defined(PASS_SSR)
// Screen-space reflections on the scene EFB, on the materials that reflect:
// environment-mapped or specular draws (metal, ice, glass, chrome; F0 0.25)
// and indirect-textured ones (water; F0 0.04), from the material stencil.
// Matte surfaces do not reflect; a pixel of unknown material reflects faintly.
// Schlick's Fresnel term makes the reflection clearest at grazing angles. The
// reflected view ray is marched through the depth (24 growing steps, then a
// 4-step binary refine) and fades out near the screen edges, at the end of the
// ray and where it turns back toward the camera.
// Constants as the ambient occlusion pass: g_con0 = (near, far, A, B), g_con1 =
// (C, D, max distance in world units, strength 0-1); g_params = scene viewport.
// The alpha output is the blend weight the apply pass uses.
Texture2D colorTex : register(t1);
// How strongly the material at pixel p reflects (0-1) and its Fresnel F0.
float Reflectivity(int2 p, out float f0) {
    f0 = 0.04;
    if (!SsaoInView(p)) return 0.0;
    const uint material = SceneMaterial(p);
    if ((material & kMaterialWritten) == 0u) return 0.25;
    if ((material & kMaterialGlossy) != 0u) { f0 = 0.25; return 1.0; }
    if ((material & kMaterialIndirect) != 0u) return 1.0;
    return 0.0;
}
float4 PSMain(FullscreenOut i) : SV_Target {
    const int2 p = int2(i.pos.xy);
    if (!SsaoInView(p)) return float4(0.0, 0.0, 0.0, 0.0);
    const float zv = SsaoViewZ(p);
    if (zv < 0.0 || zv > 0.98 * asfloat(g_con0.y)) return float4(0.0, 0.0, 0.0, 0.0);
    const float3 P = SsaoViewPos(p, zv);
    float slope;
    const float3 N = SsaoNormal(p, zv, P, slope);
    // The reflectivity is averaged over a small plus-shaped neighbourhood, so it
    // fades across the border of a reflective surface instead of cutting there.
    float f0;
    float reflectivity = Reflectivity(p, f0);
    {
        float f0_n;
        const int2 kOffsets[4] = {int2(3, 0), int2(-3, 0), int2(0, 3), int2(0, -3)};
        [unroll] for (int n = 0; n < 4; ++n) {
            reflectivity += Reflectivity(p + kOffsets[n], f0_n);
            f0 = max(f0, f0_n);
        }
        reflectivity *= 0.2;
    }
    if (reflectivity <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    // Only reasonably flat surfaces reflect. Small curved details that happen to
    // use a glossy or environment-mapped material (Mario's eyes, buttons, gems)
    // would otherwise show a mirror image of the scene instead of their own
    // colour. Flatness is how well the normals four pixels away agree with this
    // one; curved metal still reflects, with a weight that falls as it curves.
    {
        float flatness = 1.0;
        const int2 kProbe[4] = {int2(4, 0), int2(-4, 0), int2(0, 4), int2(0, -4)};
        [unroll] for (int n = 0; n < 4; ++n) {
            const int2 q = p + kProbe[n];
            const float zq = SsaoViewZ(q);
            if (zq < 0.0) { flatness = min(flatness, 0.0); continue; }
            float slope_q;
            const float3 Nq = SsaoNormal(q, zq, SsaoViewPos(q, zq), slope_q);
            flatness = min(flatness, dot(N, Nq));
        }
        reflectivity *= smoothstep(0.86, 0.985, flatness);
        if (reflectivity <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float3 V = normalize(P);
    const float cos_v = saturate(dot(-V, N));
    const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cos_v, 5.0);
    const float strength = asfloat(g_con1.w) * reflectivity * fresnel;
    if (strength < 0.01) return float4(0.0, 0.0, 0.0, 0.0);
    const float3 R = reflect(V, N);
    // Rays back toward the camera leave the depth buffer's view.
    const float toward = saturate(-R.z * 4.0 + 1.0);
    if (toward <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);

    const float max_dist = asfloat(g_con1.z);
    const float3 origin = P + N * (0.002 * zv + slope);
    // A ray toward the camera stops before the near plane.
    const float reach = R.z > 0.0 ? saturate((-origin.z - asfloat(g_con0.x)) / (R.z * max_dist)) : 1.0;
    const float3 ray = R * max_dist * reach;
    static const float kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const float noise = kBayer[(p.y & 3) * 4 + (p.x & 3)] / 16.0;
    const int kSteps = 24;
    float prev_t = 0.0;
    float hit_t = -1.0;
    // A ray that passes just behind a surface by up to half again its thickness
    // is a soft hit, with a confidence that falls off to zero: a reflection then
    // fades as the reflected object slides away instead of vanishing at a
    // threshold.
    float soft_t = -1.0;
    float soft_prev_t = 0.0;
    float soft_conf = 0.0;
    float last_t = 0.0;
    [loop] for (int k = 0; k < kSteps; ++k) {
        const float s = (float(k) + 0.5 + noise) / float(kSteps);
        const float t = s * s;
        const float3 S = origin + ray * t;
        const int2 q = int2(floor(SsaoProject(S)));
        if (!SsaoInView(q)) break;
        last_t = t;
        const float zq = SsaoViewZ(q);
        const float delta = -S.z - zq;
        // Thickness grows with distance so thin geometry still catches rays.
        const float thickness = 0.05 * zq + max_dist * (t - prev_t);
        if (zq > 0.0 && delta > 0.0) {
            const float ratio = delta / thickness;
            if (ratio < 1.0) {
                hit_t = t;
                break;
            }
            const float conf = saturate((1.5 - ratio) * 2.0);
            if (conf > soft_conf) {
                soft_conf = conf;
                soft_t = t;
                soft_prev_t = prev_t;
            }
        }
        prev_t = t;
    }
    float hit_conf = 1.0;
    if (hit_t < 0.0 && soft_t >= 0.0) {
        hit_t = soft_t;
        prev_t = soft_prev_t;
        hit_conf = soft_conf;
    }
    if (hit_t < 0.0) {
        // No hit: a faint reflection of the environment, the scene colour where
        // the ray ends on screen (the sky for a ray that climbs away). Without it
        // the reflection would drop from full to nothing the moment the ray misses.
        const float2 end = clamp(SsaoProject(origin + ray * max(last_t, 0.05)),
                                 g_params.xy + 2.0, g_params.xy + g_params.zw - 2.0);
        const float3 env = colorTex.SampleLevel(LinearSampler, end * g_rt_metrics.xy, 0).rgb;
        if (asfloat(g_con2.x) > 0.5) return float4(0.0, 1.0, 1.0, saturate(strength * toward * 0.9));
        return float4(env, strength * toward * 0.3);
    }
    float lo_t = prev_t;
    float hi_t = hit_t;
    [unroll] for (int b = 0; b < 4; ++b) {
        const float mid = 0.5 * (lo_t + hi_t);
        const float3 S = origin + ray * mid;
        const float zq = SsaoViewZ(int2(floor(SsaoProject(S))));
        if (zq > 0.0 && -S.z > zq) hi_t = mid; else lo_t = mid;
    }
    const float2 hit = SsaoProject(origin + ray * hi_t);
    // Fade only in the last 3% of the viewport: a reflected object high on the
    // screen (Mario mid-jump) must not lose its reflection to the edge fade.
    const float2 edge = min(hit - g_params.xy, g_params.xy + g_params.zw - hit) / (0.03 * g_params.zw);
    const float fade = saturate(min(edge.x, edge.y)) * (1.0 - hi_t * hi_t) * toward;
    // Point-sampled: a bilinear tap averages a small detail (Mario's blue irises,
    // a few pixels wide) with the white and the skin around it, which washes it
    // to white in the reflection.
    const float3 colour = colorTex.Load(int3(int2(floor(hit)), 0)).rgb;
    // The environment fallback is the floor under a confident hit's weight, so the
    // blend between the two is continuous.
    const float env_floor = 0.3 * toward;
    const float3 env = colorTex.SampleLevel(
        LinearSampler, clamp(SsaoProject(origin + ray * max(last_t, 0.05)),
                             g_params.xy + 2.0, g_params.xy + g_params.zw - 2.0) * g_rt_metrics.xy, 0).rgb;
    const float weight = max(fade * hit_conf, env_floor);
    const float3 mixed = lerp(env, colour, saturate(fade * hit_conf / max(weight, 1e-4)));
    // g_con2.x = 1 (GALAXY_REFLECTION_DEBUG): show how each pixel resolved. Magenta
    // is a hit, yellow a soft hit, cyan the environment fallback; the strength shows
    // as the alpha, so rays that vanish with distance are easy to see.
    if (asfloat(g_con2.x) > 0.5) {
        const float3 key = hit_conf >= 0.999 ? float3(1.0, 0.0, 1.0) : float3(1.0, 1.0, 0.0);
        return float4(key, saturate(strength * weight * 3.0));
    }
    return float4(mixed, strength * weight);
}
#endif

#if defined(PASS_DOF_FOCUS)
// The depth of field's focus distance, one value per scene in a 1x1 R32F target.
// Candidate: the median of a 3x3 cluster of depth samples around the viewport
// centre, where the camera keeps Mario (a thin pole or a flying coin through a
// few samples cannot move a median). It is then eased toward the previous focus
// (t1) in log space with a per-scene step limit, so the blur never pumps.
// g_con0.xy = (near, far); g_con1.x = step limit per scene (ratio > 1),
// g_con1.y = ease rate 0-1, g_con1.z = 1 resets to the candidate (first scene
// or a camera cut); g_params = scene viewport.
Texture2D prevFocusTex : register(t1);
float4 PSMain(FullscreenOut i) : SV_Target {
    const float2 c = g_params.xy + g_params.zw * 0.5;
    const float step = g_params.w * 0.03;
    const float far_z = asfloat(g_con0.y);
    float z[9];
    [unroll] for (int k = 0; k < 9; ++k) {
        const float2 o = float2(float(k % 3) - 1.0, float(k / 3) - 1.0) * float2(step, step * 1.3);
        const float s = SsaoViewZ(int2(c + o));
        z[k] = s > 0.0 ? s : far_z;
    }
    // Median of nine: a partial selection sort to the fifth element.
    [unroll] for (int a = 0; a < 5; ++a) {
        [unroll] for (int b = a + 1; b < 9; ++b) {
            if (z[b] < z[a]) { const float t = z[a]; z[a] = z[b]; z[b] = t; }
        }
    }
    const float candidate = max(z[4], asfloat(g_con0.x) * 2.0);
    const float prev = prevFocusTex.Load(int3(0, 0, 0)).r;
    float focus = candidate;
    if (prev > 0.0 && asfloat(g_con1.z) < 0.5) {
        const float limit = log(max(asfloat(g_con1.x), 1.0001));
        focus = prev * exp(clamp(log(candidate / prev) * asfloat(g_con1.y), -limit, limit));
    }
    return float4(focus, 0.0, 0.0, 0.0);
}
#endif

#if defined(PASS_DOF_BLUR)
// Far-field depth of field on the scene EFB, the soft distant backdrop of a
// modern platformer. The focus (t2, from the focus pass) is the smoothed depth
// at the viewport centre, where the camera keeps Mario. The blur radius is 0 up to
// focus * g_con1.y and grows to g_con1.x pixels at focus * g_con1.y * g_con1.z;
// the far plane (sky) and cleared pixels are fully blurred. Nothing nearer than
// the focus is blurred, so the ground under Mario and the HUD stay sharp.
// g_con0.xy = (near, far); g_params = scene viewport; the alpha output is the
// blend weight the apply pass uses.
Texture2D colorTex : register(t1);
Texture2D focusTex : register(t2);
float DofCoc(float zv, float focus) {
    const float far_z = asfloat(g_con0.y);
    if (zv < 0.0) zv = far_z;
    const float start = focus * asfloat(g_con1.y);
    const float full = start * asfloat(g_con1.z);
    return saturate((zv - start) / max(1e-3, full - start)) * asfloat(g_con1.x);
}
float DofFocus() {
    return focusTex.Load(int3(0, 0, 0)).r;
}
float4 PSMain(FullscreenOut i) : SV_Target {
    const int2 p = int2(i.pos.xy);
    if (!SsaoInView(p)) return float4(0.0, 0.0, 0.0, 0.0);
    const float focus = DofFocus();
    const float coc = DofCoc(SsaoViewZ(p), focus);
    if (coc < 0.5) return float4(0.0, 0.0, 0.0, 0.0);
    // Samples stay inside the scene viewport.
    const float2 lo = (g_params.xy + 0.5) * g_rt_metrics.xy;
    const float2 hi = (g_params.xy + g_params.zw - 0.5) * g_rt_metrics.xy;
    const float2 centre = float2(p) + 0.5;
    float3 sum = colorTex.Load(int3(p, 0)).rgb;
    float weight = 1.0;
    const int kTaps = 24;
    [unroll] for (int k = 0; k < kTaps; ++k) {
        const float t = (float(k) + 0.5) / float(kTaps);
        const float r = coc * sqrt(t);
        const float angle = float(k) * 2.39996323;
        const float2 q = centre + float2(cos(angle), sin(angle)) * r;
        // A sharper (nearer) sample only counts where its own blur reaches this
        // pixel, so in-focus silhouettes do not smear into the backdrop.
        const float cq = DofCoc(SsaoViewZ(int2(q)), focus);
        const float w = saturate(1.0 + cq - r);
        sum += colorTex.SampleLevel(LinearSampler, clamp(q * g_rt_metrics.xy, lo, hi), 0).rgb * w;
        weight += w;
    }
    return float4(sum / weight, saturate(coc - 0.5));
}
#endif

#if defined(PASS_DOF_APPLY)
// Blended over the EFB colour by the pipeline's blend state.
Texture2D dofTex : register(t0);
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
float4 PSMain(FullscreenOut i) : SV_Target {
    return dofTex.Load(int3(int2(i.pos.xy), 0));
}
#endif

#if defined(PASS_MOTION_BLUR)
// Per-pixel motion blur on the scene EFB from the scene's motion vectors (the
// ones the temporal upscaler uses): every pixel carries its own screen-space
// velocity, current to previous position in render pixels, from the camera and
// from the object (Mario's limbs, enemies, falling things). Each pixel is
// smeared along its velocity, scaled by the shutter, with 16 interleaved taps.
// Two guards keep it clean: a tap on a clearly nearer surface that moves much
// less than this pixel is skipped (a still Mario must not smear into a moving
// backdrop), and the sky and unwritten pixels (no velocity) stay sharp.
// t0 velocity (RG16F), t1 depth (reversed: larger is nearer), t2 colour;
// g_params = scene viewport; g_con0.x = shutter, g_con0.y = maximum length in
// pixels. The alpha output is the blend weight the apply pass uses.
Texture2D velocityTex : register(t0);
Texture2D depthTex : register(t1);
Texture2D colorTex : register(t2);
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
float4 PSMain(FullscreenOut i) : SV_Target {
    const int2 p = int2(i.pos.xy);
    const float4 vp = g_params;
    if (float(p.x) < vp.x || float(p.y) < vp.y || float(p.x) >= vp.x + vp.z ||
        float(p.y) >= vp.y + vp.w) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    float2 v = velocityTex.Load(int3(p, 0)).rg * asfloat(g_con0.x);
    float len = length(v);
    if (!(len >= 0.75)) return float4(0.0, 0.0, 0.0, 0.0);
    const float max_len = asfloat(g_con0.y);
    if (len > max_len) { v *= max_len / len; len = max_len; }
    const float depth_here = depthTex.Load(int3(p, 0)).r;
    const float2 centre = float2(p) + 0.5;
    const float2 lo = vp.xy + 0.5;
    const float2 hi = vp.xy + vp.zw - 0.5;
    static const float kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const float noise = kBayer[(p.y & 3) * 4 + (p.x & 3)] / 16.0;
    const int kTaps = 16;
    float3 sum = colorTex.Load(int3(p, 0)).rgb;
    float weight = 1.0;
    [unroll] for (int k = 0; k < kTaps; ++k) {
        const float t = (float(k) + noise) / float(kTaps) - 0.5;
        const float2 q = clamp(centre + v * t, lo, hi);
        const int2 qi = int2(q);
        const float depth_q = depthTex.Load(int3(qi, 0)).r;
        const float speed_q = length(velocityTex.Load(int3(qi, 0)).rg * asfloat(g_con0.x));
        const bool occluder = depth_q > depth_here + 0.002 && speed_q < 0.5 * len;
        if (occluder) continue;
        sum += colorTex.SampleLevel(LinearSampler, q * g_rt_metrics.xy, 0).rgb;
        weight += 1.0;
    }
    return float4(sum / weight, saturate(len - 0.5));
}
#endif

#if defined(PASS_BLOOM_DOWN)
// Bloom pyramid, downsample (13-tap, Jimenez 2014). g_rt_metrics describes the
// source, g_params.xy is 1/destination size. The first level (g_con0.x = 1)
// keeps only the bright part of each tap, with a soft knee, so the chain
// carries highlights and not the whole image. g_con1.x threshold, .y knee.
Texture2D inputTex : register(t0);
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
float3 BloomPrefilter(float3 c) {
    const float threshold = asfloat(g_con1.x);
    const float knee = max(asfloat(g_con1.y), 1e-4);
    const float peak = max(c.r, max(c.g, c.b));
    float soft = clamp(peak - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    const float weight = max(soft, peak - threshold) / max(peak, 1e-4);
    return c * weight;
}
float3 BloomTap(float2 uv, float2 offset, bool first) {
    const float3 c = inputTex.SampleLevel(LinearSampler, uv + offset * g_rt_metrics.xy, 0).rgb;
    return first ? BloomPrefilter(c) : c;
}
float4 PSMain(FullscreenOut i) : SV_Target {
    const bool first = asfloat(g_con0.x) > 0.5;
    const float2 uv = (float2(ViewportPixel(i.pos)) + 0.5) * g_params.xy;
    const float3 a = BloomTap(uv, float2(-2, -2), first);
    const float3 b = BloomTap(uv, float2(0, -2), first);
    const float3 c = BloomTap(uv, float2(2, -2), first);
    const float3 d = BloomTap(uv, float2(-2, 0), first);
    const float3 e = BloomTap(uv, float2(0, 0), first);
    const float3 f = BloomTap(uv, float2(2, 0), first);
    const float3 g = BloomTap(uv, float2(-2, 2), first);
    const float3 h = BloomTap(uv, float2(0, 2), first);
    const float3 k = BloomTap(uv, float2(2, 2), first);
    const float3 j = BloomTap(uv, float2(-1, -1), first);
    const float3 l = BloomTap(uv, float2(1, -1), first);
    const float3 m = BloomTap(uv, float2(-1, 1), first);
    const float3 n = BloomTap(uv, float2(1, 1), first);
    const float3 sum = e * 0.125 + (a + c + g + k) * 0.03125 + (b + d + f + h) * 0.0625 +
                       (j + l + m + n) * 0.125;
    return float4(sum, 1.0);
}
#endif

#if defined(PASS_BLOOM_UP)
// Bloom pyramid, upsample: the finer level plus a 3x3 tent of the coarser one.
// t0 coarse, t1 fine; g_rt_metrics describes the coarse level, g_params.xy is
// 1/destination size, g_con0.x scales the coarse contribution.
Texture2D coarseTex : register(t0);
Texture2D fineTex : register(t1);
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
float4 PSMain(FullscreenOut i) : SV_Target {
    const float2 uv = (float2(ViewportPixel(i.pos)) + 0.5) * g_params.xy;
    const float2 t = g_rt_metrics.xy;
    float3 tent = coarseTex.SampleLevel(LinearSampler, uv, 0).rgb * 4.0;
    tent += coarseTex.SampleLevel(LinearSampler, uv + float2(-t.x, 0), 0).rgb * 2.0;
    tent += coarseTex.SampleLevel(LinearSampler, uv + float2(t.x, 0), 0).rgb * 2.0;
    tent += coarseTex.SampleLevel(LinearSampler, uv + float2(0, -t.y), 0).rgb * 2.0;
    tent += coarseTex.SampleLevel(LinearSampler, uv + float2(0, t.y), 0).rgb * 2.0;
    tent += coarseTex.SampleLevel(LinearSampler, uv + float2(-t.x, -t.y), 0).rgb;
    tent += coarseTex.SampleLevel(LinearSampler, uv + float2(t.x, -t.y), 0).rgb;
    tent += coarseTex.SampleLevel(LinearSampler, uv + float2(-t.x, t.y), 0).rgb;
    tent += coarseTex.SampleLevel(LinearSampler, uv + float2(t.x, t.y), 0).rgb;
    const float3 fine = fineTex.SampleLevel(LinearSampler, uv, 0).rgb;
    return float4(fine + tent * (1.0 / 16.0) * asfloat(g_con0.x), 1.0);
}
#endif

#if defined(PASS_ENHANCE)
// Final look: the bloom pyramid and lens flare added to the scene, a soft
// shoulder so bloom cannot clip harshly, then saturation, contrast and a
// vignette.
// t0 scene (window size), t1 bloom (half size), t2 the bloom bright pass at
// quarter size for the flare. g_params.xy is the window size.
// g_con0 = (bloom, saturation, contrast, vignette), g_con1.x = lens flare, as floats.
Texture2D sceneTex : register(t0);
Texture2D bloomTex : register(t1);
Texture2D flareTex : register(t2);
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
// Pseudo lens flare (after Chapman 2013): ghosts of the brightest highlights
// mirrored through the screen centre with a slight chromatic split, and a thin
// halo ring, both fading toward the screen edge. Only light well above the
// bloom threshold flares, so star bits and coins do not litter the screen.
float3 FlareSample(float2 uv, float2 dir) {
    const float3 c = float3(
        flareTex.SampleLevel(LinearSampler, uv + dir * 0.006, 0).r,
        flareTex.SampleLevel(LinearSampler, uv, 0).g,
        flareTex.SampleLevel(LinearSampler, uv - dir * 0.006, 0).b);
    return max(c - 0.35, 0.0);
}
float3 LensFlare(float2 uv) {
    const float2 flipped = 1.0 - uv;
    const float2 ghost_vec = (0.5 - flipped) * 0.42;
    const float2 dir = normalize(ghost_vec + 1e-6);
    static const float3 kTints[4] = {
        float3(1.0, 0.75, 0.45), float3(0.55, 0.8, 1.0), float3(0.85, 0.6, 1.0), float3(0.6, 1.0, 0.75)};
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll] for (int g = 0; g < 4; ++g) {
        const float2 guv = frac(flipped + ghost_vec * float(g));
        const float w = pow(1.0 - saturate(length(0.5 - guv) / 0.7071), 6.0);
        sum += FlareSample(guv, dir) * kTints[g] * w;
    }
    const float2 huv = flipped + dir * 0.45;
    const float hw = pow(1.0 - saturate(length(0.5 - frac(huv)) / 0.7071), 6.0);
    sum += FlareSample(huv, dir) * hw * 0.5;
    return sum;
}
float4 PSMain(FullscreenOut i) : SV_Target {
    const int2 p = ViewportPixel(i.pos);
    const float2 size = g_params.xy;
    const float2 uv = (float2(p) + 0.5) / size;
    const float4 k = asfloat(g_con0);
    float3 c = sceneTex.Load(int3(p, 0)).rgb;
    c += bloomTex.SampleLevel(LinearSampler, uv, 0).rgb * k.x;
    const float flare = asfloat(g_con1.x);
    if (flare > 0.0) c += LensFlare(uv) * flare;
    // Soft shoulder above 0.8; below it the image is untouched.
    const float3 over = max(c - 0.8, 0.0);
    c = min(c, 0.8) + 0.2 * tanh(over / 0.2);
    const float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(luma, luma, luma), c, k.y);
    c = (c - 0.5) * k.z + 0.5;
    const float2 d = (uv - 0.5) * float2(size.x / size.y, 1.0);
    c *= 1.0 - k.w * smoothstep(0.35, 1.05, length(d));
    return float4(saturate(c), 1.0);
}
#endif

#if defined(PASS_RESAMPLE)
// Box filter over each target pixel's source footprint: one bilinear tap per
// axis when enlarging, up to eight evenly spaced bilinear taps per axis when
// reducing, so a supersampled image averages every source pixel instead of
// skipping rows and columns.
Texture2D inputTex : register(t0);
FullscreenOut VSMain(uint id : SV_VertexID) { return FullscreenTriangle(id); }
float4 PSMain(FullscreenOut i) : SV_Target {
    const float2 source = g_params.xy;
    const float2 target = g_params.zw;
    const float2 scale = source / target;
    const int2 taps = int2(clamp(ceil(scale - 0.001), 1.0, 8.0));
    const float2 start = float2(ViewportPixel(i.pos)) * scale;
    const float2 step = scale / float2(taps);
    float3 sum = 0.0;
    [loop] for (int y = 0; y < taps.y; ++y) {
        [loop] for (int x = 0; x < taps.x; ++x) {
            const float2 p = start + (float2(x, y) + 0.5) * step;
            sum += inputTex.SampleLevel(LinearSampler, p / source, 0).rgb;
        }
    }
    return float4(sum / float(taps.x * taps.y), 1.0);
}
#endif

#endif
