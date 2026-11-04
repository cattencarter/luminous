// The render group's parameters, shared by C++ and MSL: a render's frames averaged as light, and the
// frame read back as the picture.
// The types are <simd/simd.h>'s, which have MSL's sizes and alignment on both sides.

#ifndef LUMINOUS_RENDER_SHARED_H
#define LUMINOUS_RENDER_SHARED_H

#include <simd/simd.h>

enum LuminousRenderSlots : unsigned int
{
    LUMINOUS_RENDER_BUFFER_PARAMS = 0,     ///< LuminousRenderParams (averaging) or LuminousRenderResolve (reading)
    LUMINOUS_RENDER_TEXTURE_FRAME = 0,     ///< the HDR frame, read where the sum is the pass's attachment
    LUMINOUS_RENDER_TEXTURE_SUM = 1,       ///< the sum of the frames as light (an attachment, read through it)
    LUMINOUS_RENDER_TEXTURE_SOURCE = 0,    ///< reading: the frame read (the HDR frame, or the finished picture)
    LUMINOUS_RENDER_TEXTURE_EXPOSURE = 1,  ///< reading: the metered exposure, one texel
    LUMINOUS_RENDER_TEXTURE_PICTURE = 2,   ///< reading: the picture at its size, top row first
    LUMINOUS_RENDER_GROUP = 16,            ///< threads per threadgroup on each axis of the read
};

struct LuminousRenderParams
{
    int frame;            ///< this frame of the render, from 0; -1 outside a render
    int frames;           ///< the render's frames
    float weight;         ///< what each frame adds: 1 / frames
    float frameScale;     ///< the margin: frame heights per picture height
};

static_assert(sizeof(LuminousRenderParams) == 16, "LuminousRenderParams must be 16 bytes in C++ and MSL");

/// The read of a frame into the picture: the part of the frame the picture is (the margin's cut, in frame
/// pixels), cropped to the render's shape and box-filtered to the picture's size, each picture pixel the
/// average of the frame pixels under it -- as the CPU's reduce() does it for OpenGL.
struct LuminousRenderResolve
{
    simd_uint2 cutOrigin;    ///< the cut's lower left in the frame (rows bottom first)
    simd_uint2 cutSize;      ///< the cut's size
    simd_float2 renderSize;  ///< the shape the cut is cropped to, centred
    simd_uint2 pictureSize;  ///< the picture, written top row first
    float scale;             ///< what the light is multiplied by (1 for the finished picture)
    unsigned int exposure;   ///< 1: also by the metered exposure (the light read as OpenEXR)
    unsigned int halfRange;  ///< 1: kept within half float's finite range, never below 0
    unsigned int pad0;
};

static_assert(sizeof(LuminousRenderResolve) == 48, "LuminousRenderResolve must be 48 bytes in C++ and MSL");

#endif // LUMINOUS_RENDER_SHARED_H
