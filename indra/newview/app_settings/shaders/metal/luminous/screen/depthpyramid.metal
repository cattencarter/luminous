// The depth pyramid: one dispatch makes up to five levels of linear view depth from the frame's depth
// buffer, at a half to a thirty-second of its size. Each threadgroup of 8 x 8 threads takes a 32 x 32
// tile of the depth buffer; each thread reduces its 4 x 4 depths to 2 x 2 texels of level 0 and those
// to one of level 1 in registers, and threadgroup memory carries levels 2 to 4. No threadgroup reads
// another's results, so no atomics are needed. How four depths become one is the constants' mode
// (screenshared.h). A depth of 1 (the sky) gives the far plane's distance.
//
// Bindings: buffer 0 the constants; texture 0 the depth buffer; textures 1-5 single-level write views of
// the pyramid's levels 0-4. A slot past `levels` is never touched.

#include <metal_stdlib>
#include "screenshared.h"
#include "screenutil.h"

using namespace metal;

namespace luminous_pyramid
{
    inline float reduce(float a, float b, float c, float d, uint mode, float radius)
    {
        if (mode == LUMINOUS_PYRAMID_NEAREST)
        {
            return min(min(a, b), min(c, d));
        }
        const float farthest = max(max(a, b), max(c, d));
        if (mode == LUMINOUS_PYRAMID_FARTHEST)
        {
            return farthest;
        }
        // XeGTAO's depth filter: full weight within (1 - 0.615) of the effect radius of the farthest, none past
        // the effect radius, which is 0.75 of the AO's radius.
        const float effect = 0.75 * max(radius, 1e-4);
        const float range = 0.615 * effect;
        const float from = effect - range;
        const float mul = -1.0 / range;
        const float add = from / range + 1.0;
        const float wa = saturate((farthest - a) * mul + add);
        const float wb = saturate((farthest - b) * mul + add);
        const float wc = saturate((farthest - c) * mul + add);
        const float wd = saturate((farthest - d) * mul + add);
        return (wa * a + wb * b + wc * c + wd * d) / (wa + wb + wc + wd); // the farthest weighs 1
    }

    inline uint2 levelSize(uint2 size0, uint level)
    {
        return max(size0 >> level, uint2(1));
    }

    inline void writeLevel(array<texture2d<float, access::write>, LUMINOUS_PYRAMID_MAX_LEVELS> levels, uint level,
                           uint count, uint2 size0, uint2 coord, float value)
    {
        if (level < count && all(coord < levelSize(size0, level)))
        {
            levels[level].write(float4(value), coord);
        }
    }
}

kernel void luminous_depth_pyramid(
    constant LuminousDepthPyramidConstants& k [[buffer(LUMINOUS_PYRAMID_BUFFER_CONSTANTS)]],
    depth2d<float, access::read> depth [[texture(LUMINOUS_PYRAMID_TEXTURE_DEPTH)]],
    array<texture2d<float, access::write>, LUMINOUS_PYRAMID_MAX_LEVELS> levels
        [[texture(LUMINOUS_PYRAMID_TEXTURE_FIRST_LEVEL)]],
    uint2 group [[threadgroup_position_in_grid]],
    uint2 lid [[thread_position_in_threadgroup]])
{
    using namespace luminous_pyramid;
    threadgroup float level1[LUMINOUS_PYRAMID_THREADS][LUMINOUS_PYRAMID_THREADS];
    threadgroup float level2[LUMINOUS_PYRAMID_THREADS / 2][LUMINOUS_PYRAMID_THREADS / 2];
    threadgroup float level3[LUMINOUS_PYRAMID_THREADS / 4][LUMINOUS_PYRAMID_THREADS / 4];

    const uint count = min(k.levels, uint(LUMINOUS_PYRAMID_MAX_LEVELS));
    const float2 depth_size = float2(k.depthSize);
    const uint2 last = k.depthSize - 1;

    // Level 0: the thread's 2 x 2 texels, each from 2 x 2 depths.
    const uint2 origin0 = group * (LUMINOUS_PYRAMID_TILE / 2) + lid * 2;
    float texel0[2][2];
    for (uint y = 0; y < 2; ++y)
    {
        for (uint x = 0; x < 2; ++x)
        {
            const uint2 coord = origin0 + uint2(x, y);
            float z[4];
            for (uint i = 0; i < 4; ++i)
            {
                const uint2 p = min(coord * 2 + uint2(i & 1, i >> 1), last);
                z[i] = luminousLinearDepth(k.invProj, (float2(p) + 0.5) / depth_size, depth.read(p));
            }
            texel0[y][x] = reduce(z[0], z[1], z[2], z[3], k.mode, k.radius);
            writeLevel(levels, 0, count, k.size0, coord, texel0[y][x]);
        }
    }

    // Level 1 in registers, then levels 2 to 4 through threadgroup memory.
    const float value1 = reduce(texel0[0][0], texel0[0][1], texel0[1][0], texel0[1][1], k.mode, k.radius);
    writeLevel(levels, 1, count, k.size0, group * (LUMINOUS_PYRAMID_TILE / 4) + lid, value1);
    if (count <= 2)
    {
        return;
    }
    level1[lid.y][lid.x] = value1;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (all(lid < uint2(4)))
    {
        const uint2 at = lid * 2;
        const float value = reduce(level1[at.y][at.x], level1[at.y][at.x + 1], level1[at.y + 1][at.x],
                                   level1[at.y + 1][at.x + 1], k.mode, k.radius);
        level2[lid.y][lid.x] = value;
        writeLevel(levels, 2, count, k.size0, group * (LUMINOUS_PYRAMID_TILE / 8) + lid, value);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (all(lid < uint2(2)))
    {
        const uint2 at = lid * 2;
        const float value = reduce(level2[at.y][at.x], level2[at.y][at.x + 1], level2[at.y + 1][at.x],
                                   level2[at.y + 1][at.x + 1], k.mode, k.radius);
        level3[lid.y][lid.x] = value;
        writeLevel(levels, 3, count, k.size0, group * (LUMINOUS_PYRAMID_TILE / 16) + lid, value);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (all(lid == uint2(0)))
    {
        const float value = reduce(level3[0][0], level3[0][1], level3[1][0], level3[1][1], k.mode, k.radius);
        writeLevel(levels, 4, count, k.size0, group, value);
    }
}
