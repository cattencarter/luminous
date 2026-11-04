// Sample positions on a disc.

#ifndef LUMINOUS_VOGEL_H
#define LUMINOUS_VOGEL_H

#include <metal_stdlib>
using namespace metal;

/// Sample i of n on a Vogel (golden angle) spiral in the unit disc, turned by phi radians: radius
/// sqrt((i + 0.5) / n), so the samples cover the disc evenly.
inline float2 luminousVogel(uint i, uint n, float phi)
{
    const float golden = 2.39996323;
    const float r = sqrt((float(i) + 0.5) / float(max(n, 1u)));
    const float theta = float(i) * golden + phi;
    return r * float2(cos(theta), sin(theta));
}

#endif // LUMINOUS_VOGEL_H
