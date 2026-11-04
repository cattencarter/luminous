// Depth helpers of the screen-space passes. Every target has GL's row order (memory row 0 is GL's
// bottom row), so a GL texture coordinate tc reads texel floor(tc * size), as GL's nearest filter
// with clamping does.

#ifndef LUMINOUS_SCREEN_UTIL_H
#define LUMINOUS_SCREEN_UTIL_H

#include <metal_stdlib>
using namespace metal;

/// The texel GL's nearest filter reads at tc, clamped to the edge.
inline uint2 luminousNearestTexel(float2 tc, float2 size)
{
    return uint2(clamp(floor(tc * size), float2(0.0), size - 1.0));
}

/// The eye-space position of depth d at tc, as deferredUtil.glsl's getPosition: GL's depth range.
inline float3 luminousEyePosition(float4x4 inv_proj, float2 tc, float d)
{
    const float4 p = inv_proj * float4(tc * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

/// Metres in front of the eye: the eye-space position's -z.
inline float luminousLinearDepth(float4x4 inv_proj, float2 tc, float d)
{
    return -luminousEyePosition(inv_proj, tc, d).z;
}

/// The eye-space position at tc that lies linear metres in front of the eye: on the ray through tc, which a
/// perspective projection makes the same for every depth.
inline float3 luminousEyePositionAt(float4x4 inv_proj, float2 tc, float linear)
{
    const float3 ray = luminousEyePosition(inv_proj, tc, 0.5);
    return ray * (linear / max(-ray.z, 1e-6));
}

#endif // LUMINOUS_SCREEN_UTIL_H
