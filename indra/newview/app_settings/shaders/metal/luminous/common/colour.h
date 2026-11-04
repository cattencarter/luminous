// Colour helpers.

#ifndef LUMINOUS_COLOUR_H
#define LUMINOUS_COLOUR_H

#include <metal_stdlib>
using namespace metal;

/// Rec. 709 luminance of linear RGB.
inline float luminousLuminance(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

/// The sRGB transfer function, each component, exactly (not a gamma approximation).
inline float3 luminousSRGBToLinear(float3 c)
{
    const float3 low = c / 12.92;
    const float3 high = pow((c + 0.055) / 1.055, float3(2.4));
    return select(high, low, c <= 0.04045);
}

inline float3 luminousLinearToSRGB(float3 c)
{
    const float3 low = c * 12.92;
    const float3 high = 1.055 * pow(max(c, 0.0), float3(1.0 / 2.4)) - 0.055;
    return select(high, low, c <= 0.0031308);
}

#endif // LUMINOUS_COLOUR_H
