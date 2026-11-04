// The shadow group's parameters, shared by C++ and MSL. LuminousPhotoMode::fillShadowParams
// fills what bindShadowUniforms and bindShadowStrength bind for the soft shadows in shadowUtil.glsl.
// The types are <simd/simd.h>'s, which have MSL's sizes and alignment on both sides.
//
// Still to add, when the group's passes are written:
// - each sun cascade's shadow matrix and split (gPipeline.mSunShadowMatrix, mSunClipPlanes) and the
//   projector maps' matrices, for a native pass that reads the maps;
// - the min/max map's size and levels, if a native blocker search uses one;
// - the sampled sun's offset for a render's frame (jitterShadowCaster).

#ifndef LUMINOUS_SHADOW_SHARED_H
#define LUMINOUS_SHADOW_SHARED_H

#include <simd/simd.h>

enum LuminousShadowSlots : unsigned int
{
    LUMINOUS_SHADOW_BUFFER_PARAMS = 0,    ///< LuminousShadowParams
    LUMINOUS_SHADOW_TEXTURE_DEPTH = 0,    ///< the frame's depth
    LUMINOUS_SHADOW_TEXTURE_SUN_MAPS = 1, ///< the four sun maps in slots 1 to 4
};

struct LuminousShadowParams
{
    int soft;             ///< luminous_soft_shadows: 1 when drawn now
    int blockerTaps;      ///< luminous_blocker_taps
    int filterTaps;       ///< luminous_filter_taps
    float softness;       ///< luminous_shadow_softness: the sun's tangent radius times Softness
    float maxTexels;      ///< luminous_max_texels
    float lift;           ///< luminous_shadow_lift: 1 - the shadows' strength
    float frame;          ///< luminous_frame
    float pad0;
};

static_assert(sizeof(LuminousShadowParams) == 32, "LuminousShadowParams must be 32 bytes in C++ and MSL");

#endif // LUMINOUS_SHADOW_SHARED_H
