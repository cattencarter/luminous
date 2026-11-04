// Helpers every lens program shares: the filters the translated lens passes use, written for explicit levels so
// that compute kernels and fragment functions read alike, and the picture's measure inside a render's frame.

#ifndef LUMINOUS_LENS_COMMON_H
#define LUMINOUS_LENS_COMMON_H

#include <metal_stdlib>
#include "lensshared.h"

using namespace metal;

constant float3 LENS_LUMA = float3(0.2126, 0.7152, 0.0722);

constexpr sampler lensLinearClamp(filter::linear, address::clamp_to_edge);
constexpr sampler lensTrilinearClamp(filter::linear, mip_filter::linear, address::clamp_to_edge);

/// The 9-tap tent of bloom's up filter: weights 1 2 1 / 2 4 2 / 1 2 1 over 16, taps one texel of the source apart.
inline float3 lensTent9(texture2d<float> source, float2 uv, float2 texel)
{
    float3 s = source.sample(lensLinearClamp, uv, level(0.0)).rgb * 4.0;
    s += (source.sample(lensLinearClamp, uv + float2(-texel.x, 0.0), level(0.0)).rgb +
          source.sample(lensLinearClamp, uv + float2(texel.x, 0.0), level(0.0)).rgb +
          source.sample(lensLinearClamp, uv + float2(0.0, -texel.y), level(0.0)).rgb +
          source.sample(lensLinearClamp, uv + float2(0.0, texel.y), level(0.0)).rgb) * 2.0;
    s += source.sample(lensLinearClamp, uv + float2(-texel.x, -texel.y), level(0.0)).rgb +
         source.sample(lensLinearClamp, uv + float2(texel.x, -texel.y), level(0.0)).rgb +
         source.sample(lensLinearClamp, uv + float2(-texel.x, texel.y), level(0.0)).rgb +
         source.sample(lensLinearClamp, uv + float2(texel.x, texel.y), level(0.0)).rgb;
    return s / 16.0;
}

/// Jimenez's thirteen taps, in source texels from the point, in the order lensJimenez13 takes their values.
constant float2 LENS_JIMENEZ_OFFSETS[13] = {
    float2(-2.0, 2.0), float2(0.0, 2.0), float2(2.0, 2.0), float2(-2.0, 0.0), float2(0.0, 0.0),
    float2(2.0, 0.0), float2(-2.0, -2.0), float2(0.0, -2.0), float2(2.0, -2.0), float2(-1.0, 1.0),
    float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0),
};

/// The translated down pass's sum of its thirteen bilinear taps (LENS_JIMENEZ_OFFSETS' order): five overlapping 2x2
/// boxes, the centre one a half, the four corners an eighth each, every tap clamped below half float's top.
inline float3 lensJimenez13(thread const float3* t)
{
    const float3 a = min(t[0], float3(65000.0));
    const float3 b = min(t[1], float3(65000.0));
    const float3 c = min(t[2], float3(65000.0));
    const float3 d = min(t[3], float3(65000.0));
    const float3 e = min(t[4], float3(65000.0));
    const float3 f = min(t[5], float3(65000.0));
    const float3 g = min(t[6], float3(65000.0));
    const float3 h = min(t[7], float3(65000.0));
    const float3 i = min(t[8], float3(65000.0));
    const float3 j = min(t[9], float3(65000.0));
    const float3 k = min(t[10], float3(65000.0));
    const float3 l = min(t[11], float3(65000.0));
    const float3 m = min(t[12], float3(65000.0));
    const float3 centre = (j + k + l + m) * 0.25;
    const float3 tl = (a + b + d + e) * 0.25;
    const float3 tr = (b + c + e + f) * 0.25;
    const float3 bl = (d + e + g + h) * 0.25;
    const float3 br = (e + f + h + i) * 0.25;
    return max(centre * 0.5 + (tl + tr + bl + br) * 0.125, float3(0.0));
}

/// The down pass at uv of its target, reading source (level 0 of the bound view) bilinearly with the edge clamped.
inline float3 lensDownSample(texture2d<float> source, float2 uv, float2 texel)
{
    float3 t[13];
    for (uint i = 0; i < 13; ++i)
    {
        t[i] = source.sample(lensLinearClamp, uv + LENS_JIMENEZ_OFFSETS[i] * texel, level(0.0)).rgb;
    }
    return lensJimenez13(t);
}

/// The metered exposure, as the translated passes read it (the 1 x 1 map at its middle).
inline float lensExposure(texture2d<float> exposure)
{
    return exposure.sample(lensLinearClamp, float2(0.5), level(0.0)).r;
}

/// The depth at uv, as a point sample with the edge clamped reads it.
inline float lensDepthAt(depth2d<float> depth, float2 uv)
{
    const uint2 size = uint2(depth.get_width(), depth.get_height());
    const int2 at = clamp(int2(floor(uv * float2(size))), int2(0), int2(size) - 1);
    return depth.read(uint2(at));
}

/// A render's frame has a margin round the picture cut from its middle: the picture's uv from the frame's, and back.
inline float lensFrameScale(constant LuminousLensParams& p)
{
    return p.frameScale > 0.0 ? p.frameScale : 1.0;
}

inline float2 lensToPicture(constant LuminousLensParams& p, float2 frame_uv)
{
    return (frame_uv - 0.5) * lensFrameScale(p) + 0.5;
}

inline float2 lensToFrame(constant LuminousLensParams& p, float2 picture_uv)
{
    return (picture_uv - 0.5) / lensFrameScale(p) + 0.5;
}

#endif // LUMINOUS_LENS_COMMON_H
