// The single-pass downsampler: one dispatch makes up to 12 mip levels of a texture from its level 0,
// after AMD FidelityFX SPD. Each threadgroup reduces a 64x64 tile of level 0 to levels 1-6 in
// threadgroup memory, writing each level as it goes; the last threadgroup to finish (a device atomic
// counter) makes levels 7 and on from every threadgroup's level 6 texel. The texels and the counter are
// coherent(device), so what other threadgroups of the dispatch wrote before their fence and count is what
// the last threadgroup reads.
//
// The filter is a 2x2 box with reads clamped to the level's edge, so an odd size repeats its last
// row or column. Levels beyond 6 need at most 64 threadgroups on each axis (a level 0 up to 4096
// texels); the C++ side asks for no more levels than that allows.
//
// Bindings (commonshared.h): buffer 0 the constants, buffer 1 the counter (zero before the first
// dispatch, left zero by each dispatch), buffer 2 one float4 per threadgroup; texture 0 level 0,
// textures 1-12 single-level write views of levels 1-12. A slot past `mips` is never touched.
//
// The tile is the kernel's only threadgroup memory, 16 KB: shader validation adds a byte for each one
// declared, and 32 KB is the most a threadgroup may use.

#include <metal_stdlib>
#include "commonshared.h"

using namespace metal;

namespace luminous_spd
{
    constant uint TILE_SIDE = 32; // level 1 texels of a tile on each axis

    inline uint2 levelSize(uint2 size0, uint level)
    {
        return max(size0 >> level, uint2(1));
    }

    inline void writeLevel(array<texture2d<float, access::write>, LUMINOUS_SPD_MAX_MIPS> dst, uint level, uint2 size0,
                           uint2 coord, float4 value)
    {
        const uint2 size = levelSize(size0, level);
        if (all(coord < size))
        {
            dst[level - 1].write(value, coord);
        }
    }

    /// Reduces the tile, which holds `level` texels in its top-left n x n with n = side, level by level
    /// down to 1x1 or `last`, writing each level at origin * its side. Every thread of the group calls it.
    inline void reduce(threadgroup float4* tile, uint side, uint level, uint last, uint2 origin, uint2 size0,
                       array<texture2d<float, access::write>, LUMINOUS_SPD_MAX_MIPS> dst, uint thread_index)
    {
        while (side > 1 && level < last)
        {
            const uint half_side = side / 2;
            ++level;
            const uint count = half_side * half_side;
            float4 values[4];
            for (uint k = 0; k < 4; ++k)
            {
                const uint index = thread_index + k * LUMINOUS_SPD_THREADS;
                if (index < count)
                {
                    const uint2 at = uint2(index % half_side, index / half_side) * 2;
                    values[k] = (tile[at.y * TILE_SIDE + at.x] + tile[at.y * TILE_SIDE + at.x + 1] +
                                 tile[(at.y + 1) * TILE_SIDE + at.x] + tile[(at.y + 1) * TILE_SIDE + at.x + 1]) * 0.25;
                }
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            for (uint k = 0; k < 4; ++k)
            {
                const uint index = thread_index + k * LUMINOUS_SPD_THREADS;
                if (index < count)
                {
                    const uint2 at = uint2(index % half_side, index / half_side);
                    tile[at.y * TILE_SIDE + at.x] = values[k];
                    writeLevel(dst, level, size0, origin * half_side + at, values[k]);
                }
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            side = half_side;
        }
    }
}

kernel void luminous_spd_downsample(
    constant LuminousSPDConstants& constants [[buffer(LUMINOUS_SPD_BUFFER_CONSTANTS)]],
    coherent(device) device atomic_uint* counter [[buffer(LUMINOUS_SPD_BUFFER_COUNTER)]],
    coherent(device) device float4* tail [[buffer(LUMINOUS_SPD_BUFFER_TAIL)]],
    texture2d<float, access::read> source [[texture(LUMINOUS_SPD_TEXTURE_SOURCE)]],
    array<texture2d<float, access::write>, LUMINOUS_SPD_MAX_MIPS> dst [[texture(LUMINOUS_SPD_TEXTURE_FIRST_DEST)]],
    uint2 group [[threadgroup_position_in_grid]],
    uint thread_index [[thread_index_in_threadgroup]])
{
    using namespace luminous_spd;
    threadgroup float4 tile[TILE_SIDE * TILE_SIDE];

    const uint2 size0 = constants.sourceSize;
    const uint mips = min(constants.mips, uint(LUMINOUS_SPD_MAX_MIPS));

    // Level 1 of the tile: four texels per thread, each the box of four level 0 texels.
    const uint2 clamp0 = size0 - 1;
    for (uint k = 0; k < 4; ++k)
    {
        const uint index = thread_index + k * LUMINOUS_SPD_THREADS;
        const uint2 at = uint2(index % TILE_SIDE, index / TILE_SIDE);
        const uint2 coord = group * TILE_SIDE + at;
        const uint2 p = coord * 2;
        const float4 value = (source.read(min(p, clamp0)) + source.read(min(p + uint2(1, 0), clamp0)) +
                              source.read(min(p + uint2(0, 1), clamp0)) + source.read(min(p + uint2(1, 1), clamp0))) *
                             0.25;
        tile[at.y * TILE_SIDE + at.x] = value;
        writeLevel(dst, 1, size0, coord, value);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Levels 2-6 of the tile.
    reduce(tile, TILE_SIDE, 1, min(mips, 6u), group, size0, dst, thread_index);
    if (mips <= 6)
    {
        return;
    }

    // The tile's level 6 texel for the last threadgroup, then the count of threadgroups done. Whether this
    // threadgroup is the last reaches all its threads through tile[0].x (1 or 0), free once the texel is in
    // the tail.
    if (thread_index == 0)
    {
        tail[group.y * constants.groups.x + group.x] = tile[0];
        atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);
        const uint done = atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
        tile[0].x = done + 1 == constants.groups.x * constants.groups.y ? 1.0 : 0.0;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (tile[0].x == 0.0)
    {
        return;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup); // every thread has read tile[0] before level 7 writes it
    atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);
    if (thread_index == 0)
    {
        atomic_store_explicit(counter, 0u, memory_order_relaxed); // ready for the next dispatch
    }

    // Level 7 from the threadgroups' level 6 texels, then levels 8 and on, as one tile at the origin.
    const uint2 size6 = levelSize(size0, 6);
    const uint2 clamp6 = min(size6, constants.groups) - 1;
    for (uint k = 0; k < 4; ++k)
    {
        const uint index = thread_index + k * LUMINOUS_SPD_THREADS;
        const uint2 at = uint2(index % TILE_SIDE, index / TILE_SIDE);
        const uint2 p = at * 2;
        const auto texel = [&](uint2 q) {
            q = min(q, clamp6);
            return tail[q.y * constants.groups.x + q.x];
        };
        const float4 value = (texel(p) + texel(p + uint2(1, 0)) + texel(p + uint2(0, 1)) + texel(p + uint2(1, 1))) * 0.25;
        tile[at.y * TILE_SIDE + at.x] = value;
        writeLevel(dst, 7, size0, at, value);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    reduce(tile, TILE_SIDE, 7, mips, uint2(0), size0, dst, thread_index);
}
