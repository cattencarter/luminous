// The screen-space group's parameters, shared by C++ and MSL. LuminousPhotoMode::fillScreenParams
// fills them with what the translated code binds: GTAO (aoUtil.glsl and the half-size pass), AO on
// lamps, contact shadows (the sun and spot light passes) and enhanced reflections.
// The types are <simd/simd.h>'s, which have MSL's sizes and alignment on both sides.
//
// The native passes' own constants follow: the depth pyramid (any pass that marches or gathers
// over the depth buffer may build one), the half-size GTAO and the scene map's mip chain.

#ifndef LUMINOUS_SCREEN_SHARED_H
#define LUMINOUS_SCREEN_SHARED_H

#include <simd/simd.h>

enum LuminousScreenSlots : unsigned int
{
    LUMINOUS_SCREEN_BUFFER_PARAMS = 0,  ///< LuminousScreenParams, or the pass's own constants below
    LUMINOUS_SCREEN_TEXTURE_DEPTH = 0,  ///< the frame's depth
    LUMINOUS_SCREEN_TEXTURE_NORMAL = 1, ///< the G-buffer's normals
    LUMINOUS_SCREEN_TEXTURE_LIGHT = 2,  ///< the light map whose green holds the AO
    LUMINOUS_SCREEN_TEXTURE_AO = 3,     ///< the half-size GTAO's visibility, read
    LUMINOUS_SCREEN_TEXTURE_ALBEDO = 4, ///< the G-buffer's diffuse colour
    LUMINOUS_SCREEN_TEXTURE_PYRAMID = 5, ///< the depth pyramid, every level, read
    LUMINOUS_SCREEN_TEXTURE_OUT = 6,    ///< a kernel's result, written
};

struct LuminousScreenParams
{
    simd_float4 contactLight; ///< luminous_contact_light in xyz: toward the sun or moon, eye space
    simd_float2 halfSize;     ///< luminous_half_size: the half-size GTAO's pixels
    int gtao;                 ///< luminous_gtao: 0 none (Linden's AO), 1 full size in the sun pass, 2 half size after it
    int aoSlices;
    int aoSteps;
    float aoRadius;           ///< metres
    float aoStrength;
    float aoSoften;           ///< the softening pass's y while GTAO is on
    float aoLights;           ///< luminous_ao_lights: the lamps' share of the AO
    float contactLength;      ///< metres; 0: no contact shadows
    float contactStrength;
    float contactSeed;
    int contactSteps;
    int contactAll;           ///< 1 in a picture
    int ssr;                  ///< luminous_ssr: 1 when enhanced reflections are drawn now
    int ssrSteps;
    int ssrSamples;
    float ssrScale;           ///< the live view's render scale
    float frame;              ///< luminous_frame
    float pad0;
};

static_assert(sizeof(LuminousScreenParams) == 96, "LuminousScreenParams must be 96 bytes in C++ and MSL");

// The depth pyramid (depthpyramid.metal): one dispatch turns the frame's depth buffer into up to
// five levels of positive linear view depth (metres in front of the eye), at a half, a quarter,
// an eighth, a sixteenth and a thirty-second of the frame's size. The pyramid's own level 0 is the
// half-size one. Each threadgroup of 8 x 8 threads reduces a 32 x 32 tile of the depth buffer.
enum LuminousDepthPyramidSlots : unsigned int
{
    LUMINOUS_PYRAMID_BUFFER_CONSTANTS = 0, ///< LuminousDepthPyramidConstants
    LUMINOUS_PYRAMID_TEXTURE_DEPTH = 0,    ///< the frame's depth, read
    LUMINOUS_PYRAMID_TEXTURE_FIRST_LEVEL = 1, ///< the pyramid's level 0 here, level 1 in the next slot, and so on:
                                              ///< single-level write views
    LUMINOUS_PYRAMID_MAX_LEVELS = 5,
    LUMINOUS_PYRAMID_THREADS = 8,          ///< threads per threadgroup on each axis
    LUMINOUS_PYRAMID_TILE = 32,            ///< depth texels per threadgroup on each axis
};

/// How four depths become one in the pyramid.
enum LuminousDepthPyramidMode : unsigned int
{
    /// XeGTAO's filter: the average of the four, each weighted down as it lies nearer than the farthest of
    /// them, by more than (1 - 0.615) of 0.75 x radius and fully at 0.75 x radius. For ambient occlusion: a far
    /// sample never takes the depth of a thin thing in front of it.
    LUMINOUS_PYRAMID_WEIGHTED = 0,
    LUMINOUS_PYRAMID_NEAREST = 1,  ///< the nearest: a march may skip what lies in front of it (hierarchical depth)
    LUMINOUS_PYRAMID_FARTHEST = 2, ///< the farthest
};

struct LuminousDepthPyramidConstants
{
    simd_float4x4 invProj;   ///< inv_proj: GL's clip space (depth -1 to 1) to eye space
    simd_uint2 depthSize;    ///< the depth buffer's texels
    simd_uint2 size0;        ///< the pyramid's level 0: half the depth buffer's size, at least 1
    unsigned int levels;     ///< levels written, 1 to LUMINOUS_PYRAMID_MAX_LEVELS
    unsigned int mode;       ///< LuminousDepthPyramidMode
    float radius;            ///< metres: the weighted mode's effect radius
    float pad0;
};

static_assert(sizeof(LuminousDepthPyramidConstants) == 96,
              "LuminousDepthPyramidConstants must be 96 bytes in C++ and MSL");

// The half-size GTAO (gtao.metal): luminous_gtao_half computes the visibility of every half-size
// texel as aoUtil.glsl's luminousGTAO does at the same place, reading wide samples from the depth
// pyramid; luminous_gtao_denoise filters it 3 x 3 within each surface; luminous_gtao_up_fs brings it
// to the frame's size and the light map multiplies it into its green.
enum LuminousGTAOSlots : unsigned int
{
    LUMINOUS_GTAO_THREADS = 8, ///< threads per threadgroup on each axis, for both kernels
};

struct LuminousGTAOConstants
{
    simd_float4x4 invProj;   ///< inv_proj, as the sun pass has it
    simd_float2 screenRes;   ///< the frame's size (screen_res): the depth buffer's and the light map's
    simd_float2 halfSize;    ///< the half-size GTAO's texels
    float radius;            ///< luminous_ao_radius, metres
    float strength;          ///< luminous_ao_strength
    float frame;             ///< luminous_frame
    int slices;              ///< luminous_ao_slices
    int steps;               ///< luminous_ao_steps
    float mipOffset;         ///< samples further out than 2^(mipOffset + 1) frame pixels read the pyramid
    unsigned int pyramidLevels; ///< the pyramid's levels; 0 reads the depth buffer alone
    unsigned int denoise;    ///< 1 filters the result 3 x 3 before the upsample, 0 copies it
};

static_assert(sizeof(LuminousGTAOConstants) == 112, "LuminousGTAOConstants must be 112 bytes in C++ and MSL");

// The scene map's mip chain for enhanced reflections (scenemips.metal): the levels whose sides above
// are even (or 1) come from the common downsampler's 2x2 box; luminous_scene_mip_down makes a level
// below one with an odd side, each texel the bilinear sample at its centre, as a backend's mipmap
// filter does there.
enum LuminousSceneMipSlots : unsigned int
{
    LUMINOUS_SCENE_MIP_BUFFER_CONSTANTS = 0, ///< LuminousSceneMipConstants
    LUMINOUS_SCENE_MIP_TEXTURE_SOURCE = 0,   ///< the level above, a single-level read view
    LUMINOUS_SCENE_MIP_TEXTURE_DEST = 1,     ///< the level made, a single-level write view
    LUMINOUS_SCENE_MIP_THREADS = 8,          ///< threads per threadgroup on each axis
};

struct LuminousSceneMipConstants
{
    simd_uint2 sourceSize; ///< the level above, in texels
    simd_uint2 destSize;   ///< the level made, in texels
};

static_assert(sizeof(LuminousSceneMipConstants) == 16, "LuminousSceneMipConstants must be 16 bytes in C++ and MSL");

#endif // LUMINOUS_SCREEN_SHARED_H
