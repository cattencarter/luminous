// The present of photo mode's live view when it is rendered smaller than the window: the finished frame
// scaled up to the window's world view with Catmull-Rom, which keeps the edges and fine detail bilinear
// blurs, then the faint present noise and the HDR range clamp of Second Life's present, and the frame's
// nearest depth for the 3D interface drawn over it. Drawn into the window's back buffer within the world
// view's viewport, as OpenGL's present (postDeferredNoDoFF.glsl with LUMINOUS_UPSCALE) draws it.
// The copy functions take a frame MetalFX has already scaled to the world view's size, texel for texel,
// with the same noise, clamp and depth.

#include "luminous/common/fullscreen.h"
#include "luminous/tone/toneshared.h"

namespace luminous_present
{
// Second Life's present noise (Morgan McGuire's hash and value noise), a dither of 0.003.
inline float hash(float2 p)
{
    return fract(1e4 * sin(17.0 * p.x + p.y * 0.1) * (0.1 + abs(sin(p.y * 13.0 + p.x))));
}

inline float noise(float2 x)
{
    const float2 i = floor(x);
    const float2 f = fract(x);
    const float a = hash(i);
    const float b = hash(i + float2(1.0, 0.0));
    const float c = hash(i + float2(0.0, 1.0));
    const float d = hash(i + float2(1.0, 1.0));
    const float2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

// Infinities to 1, NaNs to 0, the rest within 0 to 11.2, as Second Life's clampHDRRange.
inline float3 clampHDRRange(float3 c)
{
    c = select(c, float3(1.0), isinf(c));
    c = select(c, float3(0.0), isnan(c));
    return clamp(c, 0.0, 11.2);
}

// Catmull-Rom in five bilinear taps: each row's and column's two inner weights come from one tap between
// the two middle texels, and the four corner taps, whose weights are products of two negative lobes (at
// most 1.6 % together, halfway between texels), are left out and the rest renormalised. Then, as the reference, the result is kept
// within the four texels round the point, so the negative lobes cannot ring round a hard edge.
inline float4 catmullRom(texture2d<float> source, float2 uv)
{
    constexpr sampler linearClamp(filter::linear, address::clamp_to_edge);
    const float2 size = float2(source.get_width(), source.get_height());
    const float2 pos = uv * size;
    const float2 centre = floor(pos - 0.5) + 0.5;
    const float2 f = pos - centre;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;

    const float2 t0 = (centre - 1.0) / size;
    const float2 t12 = (centre + w2 / w12) / size;
    const float2 t3 = (centre + 2.0) / size;

    const float wTop = w12.x * w0.y;
    const float wLeft = w0.x * w12.y;
    const float wMiddle = w12.x * w12.y;
    const float wRight = w3.x * w12.y;
    const float wBottom = w12.x * w3.y;

    float4 c = source.sample(linearClamp, float2(t12.x, t0.y), level(0.0)) * wTop;
    c += source.sample(linearClamp, float2(t0.x, t12.y), level(0.0)) * wLeft;
    c += source.sample(linearClamp, t12, level(0.0)) * wMiddle;
    c += source.sample(linearClamp, float2(t3.x, t12.y), level(0.0)) * wRight;
    c += source.sample(linearClamp, float2(t12.x, t3.y), level(0.0)) * wBottom;
    c /= wTop + wLeft + wMiddle + wRight + wBottom;

    const int2 last = int2(size) - 1;
    const int2 p = int2(centre - 0.5);
    const float4 a = source.read(uint2(clamp(p, int2(0), last)));
    const float4 b = source.read(uint2(clamp(p + int2(1, 0), int2(0), last)));
    const float4 d = source.read(uint2(clamp(p + int2(0, 1), int2(0), last)));
    const float4 e = source.read(uint2(clamp(p + int2(1, 1), int2(0), last)));
    return clamp(c, min(min(a, b), min(d, e)), max(max(a, b), max(d, e)));
}

// The present's noise and clamp on a colour; window is the fragment's position in the window.
inline float4 finish(float4 diff, float2 window)
{
    const float2 tc = window * 4.0; // the window's pixels, not the smaller frame's
    const float3 seed = (diff.rgb + 1.0) * float3(tc.xy, tc.x + tc.y);
    const float3 nz = float3(noise(seed.rg), noise(seed.gb), noise(seed.rb));
    diff.rgb += nz * 0.003;
    diff.rgb = clampHDRRange(diff.rgb);
    return diff;
}

// The scaled frame, finished.
inline float4 colour(texture2d<float> source, float2 uv, float2 window)
{
    return finish(catmullRom(source, uv), window);
}

// A frame of the world view's size, read texel for texel and finished.
inline float4 copied(texture2d<float> source, float2 uv, float2 window)
{
    const int2 size = int2(source.get_width(), source.get_height());
    const int2 texel = clamp(int2(uv * float2(size)), int2(0), size - 1);
    return finish(source.read(uint2(texel)), window);
}

// The nearest depth of the smaller frame, not a blend across a silhouette.
inline float nearestDepth(depth2d<float> depthMap, float2 uv)
{
    const int2 size = int2(depthMap.get_width(), depthMap.get_height());
    const int2 texel = clamp(int2(uv * float2(size)), int2(0), size - 1);
    return depthMap.read(uint2(texel));
}
}

struct LuminousPresentOut
{
    float4 colour [[color(0)]];
    float depth [[depth(any)]];
};

fragment LuminousPresentOut luminous_present_upscale_fs(LuminousFullscreenOut in [[stage_in]],
                                                        texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]],
                                                        depth2d<float> depthMap [[texture(LUMINOUS_TONE_TEXTURE_DEPTH)]])
{
    LuminousPresentOut out;
    out.colour = luminous_present::colour(frame, in.uv, in.position.xy);
    out.depth = luminous_present::nearestDepth(depthMap, in.uv);
    return out;
}

// The same into a back buffer pass that has no depth to write.
fragment float4 luminous_present_upscale_nodepth_fs(LuminousFullscreenOut in [[stage_in]],
                                                    texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]])
{
    return luminous_present::colour(frame, in.uv, in.position.xy);
}

// MetalFX's frame, already at the world view's size, with the smaller frame's depth.
fragment LuminousPresentOut luminous_present_copy_fs(LuminousFullscreenOut in [[stage_in]],
                                                     texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]],
                                                     depth2d<float> depthMap [[texture(LUMINOUS_TONE_TEXTURE_DEPTH)]])
{
    LuminousPresentOut out;
    out.colour = luminous_present::copied(frame, in.uv, in.position.xy);
    out.depth = luminous_present::nearestDepth(depthMap, in.uv);
    return out;
}

// The same into a back buffer pass that has no depth to write.
fragment float4 luminous_present_copy_nodepth_fs(LuminousFullscreenOut in [[stage_in]],
                                                 texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]])
{
    return luminous_present::copied(frame, in.uv, in.position.xy);
}
