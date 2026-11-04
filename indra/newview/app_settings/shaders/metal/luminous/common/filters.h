// Sampling filters of the blur and bloom chains.

#ifndef LUMINOUS_FILTERS_H
#define LUMINOUS_FILTERS_H

#include <metal_stdlib>
using namespace metal;

/// The 9-tap tent upsample: a 3x3 tent of bilinear taps radius texels apart (weights 1 2 1 / 2 4 2 /
/// 1 2 1, over 16). texel is one texel of the source in uv.
inline float4 luminousTent9(texture2d<float> source, sampler s, float2 uv, float2 texel, float radius)
{
    const float2 d = texel * radius;
    float4 sum = source.sample(s, uv) * 4.0;
    sum += (source.sample(s, uv + float2(-d.x, 0.0)) + source.sample(s, uv + float2(d.x, 0.0)) +
            source.sample(s, uv + float2(0.0, -d.y)) + source.sample(s, uv + float2(0.0, d.y))) * 2.0;
    sum += source.sample(s, uv + float2(-d.x, -d.y)) + source.sample(s, uv + float2(d.x, -d.y)) +
           source.sample(s, uv + float2(-d.x, d.y)) + source.sample(s, uv + float2(d.x, d.y));
    return sum * (1.0 / 16.0);
}

/// The 13-tap downsample (Jimenez): five overlapping 2x2 boxes of bilinear taps, the centre box
/// weighted 0.5 and the four corner boxes 0.125. texel is one texel of the source in uv.
inline float4 luminousJimenez13(texture2d<float> source, sampler s, float2 uv, float2 texel)
{
    const float4 a = source.sample(s, uv + texel * float2(-2.0, -2.0));
    const float4 b = source.sample(s, uv + texel * float2(0.0, -2.0));
    const float4 c = source.sample(s, uv + texel * float2(2.0, -2.0));
    const float4 d = source.sample(s, uv + texel * float2(-1.0, -1.0));
    const float4 e = source.sample(s, uv + texel * float2(1.0, -1.0));
    const float4 f = source.sample(s, uv + texel * float2(-2.0, 0.0));
    const float4 g = source.sample(s, uv);
    const float4 h = source.sample(s, uv + texel * float2(2.0, 0.0));
    const float4 i = source.sample(s, uv + texel * float2(-1.0, 1.0));
    const float4 j = source.sample(s, uv + texel * float2(1.0, 1.0));
    const float4 k = source.sample(s, uv + texel * float2(-2.0, 2.0));
    const float4 l = source.sample(s, uv + texel * float2(0.0, 2.0));
    const float4 m = source.sample(s, uv + texel * float2(2.0, 2.0));
    float4 sum = (d + e + i + j) * (0.5 / 4.0);
    sum += (a + b + f + g) * (0.125 / 4.0);
    sum += (b + c + g + h) * (0.125 / 4.0);
    sum += (f + g + k + l) * (0.125 / 4.0);
    sum += (g + h + l + m) * (0.125 / 4.0);
    return sum;
}

#endif // LUMINOUS_FILTERS_H
