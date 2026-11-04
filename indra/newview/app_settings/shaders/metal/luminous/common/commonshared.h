// Constants and slots of the common library's programs, shared by C++ and MSL.
// The types are <simd/simd.h>'s, which have MSL's sizes and alignment on both sides.

#ifndef LUMINOUS_COMMON_SHARED_H
#define LUMINOUS_COMMON_SHARED_H

#include <simd/simd.h>

/// The single-pass downsampler (spd.metal). Slots are the MSL indices the C++ side binds with
/// setNativeBytes, setNativeBuffer and setNativeTexture on the compute stage.
enum LuminousSPDSlots : unsigned int
{
    LUMINOUS_SPD_BUFFER_CONSTANTS = 0, ///< LuminousSPDConstants
    LUMINOUS_SPD_BUFFER_COUNTER = 1,   ///< one uint, zero before the first dispatch; the last threadgroup sets it to zero again
    LUMINOUS_SPD_BUFFER_TAIL = 2,      ///< float4 per threadgroup: its level 6 texel, read by the last threadgroup
    LUMINOUS_SPD_TEXTURE_SOURCE = 0,   ///< level 0, read
    LUMINOUS_SPD_TEXTURE_FIRST_DEST = 1, ///< level 1 here, level 2 in the next slot, and so on: single-level write views
    LUMINOUS_SPD_MAX_MIPS = 12,        ///< levels written beyond level 0
    LUMINOUS_SPD_THREADS = 256,        ///< threads per threadgroup
    LUMINOUS_SPD_TILE = 64,            ///< level 0 texels per threadgroup on each axis
    LUMINOUS_SPD_MAX_GROUPS = 64,      ///< threadgroups on each axis at most when more than 6 levels are written
};

/// What one dispatch reads. Threadgroups: ceil(sourceSize / 64) on each axis, 256 threads each.
struct LuminousSPDConstants
{
    simd_uint2 sourceSize; ///< level 0 in texels
    simd_uint2 groups;     ///< the dispatch's threadgroups on each axis
    unsigned int mips;     ///< levels written, 1 to LUMINOUS_SPD_MAX_MIPS
    unsigned int pad0;
    unsigned int pad1;
    unsigned int pad2;
};

static_assert(sizeof(LuminousSPDConstants) == 32, "LuminousSPDConstants must be 32 bytes in C++ and MSL");

#endif // LUMINOUS_COMMON_SHARED_H
