// The depth of field group's parameters, shared by C++ and MSL. LuminousPhotoMode::fillDoFParams
// fills the first part with what renderDepthOfField binds for its tiles and its gather; the native
// pass (luminousmetaldof.cpp) fills the rest, which only the native passes read.
// The types are <simd/simd.h>'s, which have MSL's sizes and alignment on both sides.

#ifndef LUMINOUS_DOF_SHARED_H
#define LUMINOUS_DOF_SHARED_H

#include <simd/simd.h>

/// Slots of dof.metal. Buffers and textures are numbered apart, as MSL numbers them.
enum LuminousDoFSlots : unsigned int
{
    LUMINOUS_DOF_BUFFER_PARAMS = 0,   ///< LuminousDoFParams
    LUMINOUS_DOF_BUFFER_TILES = 1,    ///< float2 per tile: the largest blur in it, and its coarse key (dof.metal)
    LUMINOUS_DOF_BUFFER_ROWS = 2,     ///< float4 per tile: the tiles' values combined along each row
    LUMINOUS_DOF_BUFFER_STATE = 3,    ///< float2 per tile: the blur that can reach it, and how it is drawn
    LUMINOUS_DOF_BUFFER_COUNTS = 4,   ///< uint4: the tiles gathered at half and at quarter size
    LUMINOUS_DOF_BUFFER_LISTS = 5,    ///< uint per tile, twice: the half-size tiles, then the quarter-size tiles

    LUMINOUS_DOF_TEXTURE_SOURCE = 0,  ///< colour and signed circle of confusion (cofF.glsl), level 0
    LUMINOUS_DOF_TEXTURE_CHAIN = 1,   ///< the source's mip levels from level 1 on, as this texture's levels from 0
    LUMINOUS_DOF_TEXTURE_HALF = 2,    ///< the blur gathered at half size, where the tiles allow it
    LUMINOUS_DOF_TEXTURE_QUARTER = 3, ///< and at quarter size
    LUMINOUS_DOF_TEXTURE_LAYER = 4,   ///< the settled layer luminous_dof_settled_fs adds back (MetalFX temporal)

    LUMINOUS_DOF_TILE = 16,           ///< pixels on a tile's side
    LUMINOUS_DOF_CHAIN_LEVELS = 6,    ///< the source's levels the chain holds beyond level 0
    LUMINOUS_DOF_HALF_TILES_PER_GROUP = 2,    ///< half-size tiles a threadgroup of the coarse gather draws
    LUMINOUS_DOF_QUARTER_TILES_PER_GROUP = 7, ///< quarter-size tiles a threadgroup of the coarse gather draws
};

/// How a tile is drawn (the state buffer's y).
enum LuminousDoFTileKind : unsigned int
{
    LUMINOUS_DOF_KIND_SHARP = 0,      ///< nothing blurred can reach it: the frame stays as it is
    LUMINOUS_DOF_KIND_FULL = 1,       ///< gathered at every pixel, as the translated gather does
    LUMINOUS_DOF_KIND_HALF = 2,       ///< gathered at half size and scaled up
    LUMINOUS_DOF_KIND_QUARTER = 3,    ///< gathered at quarter size and scaled up
};

struct LuminousDoFParams
{
    simd_float2 screenRes;  ///< the source's pixels
    simd_uint2 tiles;       ///< tiles on each axis
    float maxCoF;           ///< the largest circle of confusion, pixels
    float lod;              ///< the mip level the samples' spacing reads
    int samples;            ///< samples on the largest disc
    int budget;             ///< samples a pixel may take
    int reach;              ///< tiles a blur can reach across
    float front;            ///< 1: the foreground blurs too
    float blades;           ///< 0 round, else the aperture's blades
    float pad0;

    // Filled by fillDoFParams too.
    int picture;            ///< 1 in a picture's frame (a render or a snapshot): every blurred tile at full size

    // Filled by the native pass.
    float coarseHalf;       ///< a tile whose every blur near it is at least this many pixels may be drawn at half size; 0 never
    float coarseQuarter;    ///< and at quarter size; 0 never
    int reachCoarse;        ///< tiles a blur can reach across into a coarse tile, its border texels included
    int levels;             ///< the source's mip levels there are beyond level 0
    int halfSamples;        ///< samples on the largest disc at half size: spaced at least two pixels apart
    float halfLod;          ///< their mip level
    int quarterSamples;     ///< and at quarter size, at least four pixels apart
    float quarterLod;
    float jitterX;          ///< in the layer: the jitter in the source's pixels (GL's rows, up), else 0. Each texel
    float jitterY;          ///< is the place the camera without it saw, read from the source that far on
    int layer;              ///< 1: a frame drawn for MetalFX temporal, the blur gathered into a layer to settle
};

static_assert(sizeof(LuminousDoFParams) == 96, "LuminousDoFParams must be 96 bytes in C++ and MSL");

#endif // LUMINOUS_DOF_SHARED_H
