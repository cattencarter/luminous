// Lens distortion's mapping, shared by the lens library's finish pass and the tone library's present: a frame drawn
// for MetalFX temporal leaves the distortion out, and the present distorts the scaled frame instead.

#ifndef LUMINOUS_DISTORTION_H
#define LUMINOUS_DISTORTION_H

#include <metal_stdlib>
using namespace metal;

/// Where distortion reads the frame for uv: positive bows straight lines outward (barrel), negative pulls them in;
/// the frame scaled so that the picture's corners never read past its edges. aspect is the frame's width over its
/// height, frame_scale the picture heights per frame height, picture_aspect the picture's (0: the frame's own).
inline float2 luminousDistorted(float2 uv, float aspect, float frame_scale, float distortion, float picture_aspect)
{
    const float2 q = (uv - 0.5) * float2(aspect, 1.0) * frame_scale; // in the picture's units
    const float k = distortion * 0.25;
    const float shape = picture_aspect > 0.0 ? picture_aspect : aspect;
    const float corner2 = dot(float2(shape, 1.0) * 0.5, float2(shape, 1.0) * 0.5);
    const float fit = k > 0.0 ? 1.0 / (1.0 + k * corner2) : 1.0;
    const float2 r = q * (1.0 + k * dot(q, q)) * fit;
    return r / (float2(aspect, 1.0) * frame_scale) + 0.5;
}

#endif // LUMINOUS_DISTORTION_H
