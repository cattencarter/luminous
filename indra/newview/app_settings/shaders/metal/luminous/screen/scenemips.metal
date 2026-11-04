// One level of the scene map's mip chain for enhanced reflections, from the level above: each texel the
// bilinear sample of the level above at the texel's centre, reads clamped to the edge. Used below a level
// with an odd side, where a 2x2 box would leave out the last row or column; the levels below even sides
// come from the common downsampler (spd.metal).
//
// Bindings (screenshared.h): buffer 0 the constants; texture 0 the level above, texture 1 the level made.

#include <metal_stdlib>
#include "screenshared.h"

using namespace metal;

kernel void luminous_scene_mip_down(
    constant LuminousSceneMipConstants& k [[buffer(LUMINOUS_SCENE_MIP_BUFFER_CONSTANTS)]],
    texture2d<float, access::read> source [[texture(LUMINOUS_SCENE_MIP_TEXTURE_SOURCE)]],
    texture2d<float, access::write> dest [[texture(LUMINOUS_SCENE_MIP_TEXTURE_DEST)]],
    uint2 texel [[thread_position_in_grid]])
{
    if (any(texel >= k.destSize))
    {
        return;
    }
    // The texel's centre in the level above, in texels from its corner, less half a texel: the bilinear
    // footprint's first texel and the weights toward the next.
    const float2 p = (float2(texel) + 0.5) * float2(k.sourceSize) / float2(k.destSize) - 0.5;
    const float2 base = floor(p);
    const float2 w = p - base;
    const int2 last = int2(k.sourceSize) - 1;
    const int2 a = clamp(int2(base), int2(0), last);
    const int2 b = clamp(int2(base) + 1, int2(0), last);
    const float4 top = mix(source.read(uint2(a.x, a.y)), source.read(uint2(b.x, a.y)), w.x);
    const float4 bottom = mix(source.read(uint2(a.x, b.y)), source.read(uint2(b.x, b.y)), w.x);
    dest.write(mix(top, bottom, w.y), texel);
}
