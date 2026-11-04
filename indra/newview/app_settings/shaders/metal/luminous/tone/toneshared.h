// The tone and finish group's parameters, shared by C++ and MSL. LuminousPhotoMode::fillToneParams
// fills them with what the translated passes bind: the Look's tonemap uniforms (tonemapUtilF.glsl),
// grain and Worn photo in the sharpening pass (CASF.glsl), the light leak, and the picture frame.
// The types are <simd/simd.h>'s, which have MSL's sizes and alignment on both sides.

#ifndef LUMINOUS_TONE_SHARED_H
#define LUMINOUS_TONE_SHARED_H

#include <simd/simd.h>

/// The textures the group's programs read, by MSL index.
enum LuminousToneSlots : unsigned int
{
    LUMINOUS_TONE_BUFFER_PARAMS = 0,    ///< LuminousToneParams
    LUMINOUS_TONE_TEXTURE_FRAME = 0,    ///< the HDR frame (tonemap) or the finished picture (frame, present)
    LUMINOUS_TONE_TEXTURE_EXPOSURE = 1, ///< the metered exposure, 1 x 1
    LUMINOUS_TONE_TEXTURE_LUT = 2,      ///< the Look's .cube table, 3D, RGBA16F
    LUMINOUS_TONE_TEXTURE_MCMAPFACE = 3, ///< Tony McMapface's table, 3D, unpacked to RGBA16F
    LUMINOUS_TONE_TEXTURE_DEPTH = 4,    ///< the live frame's depth, which the present writes for the interface drawn over it
    LUMINOUS_TONE_BUFFER_FRAME = 0,     ///< LuminousPictureFrameConstants (the picture frame's program)
};

/// The picture frame over the live view: what a render in a shape of its own keeps.
struct LuminousPictureFrameConstants
{
    simd_float4 rect; ///< the picture in the target's pixels: left, bottom, right, top (GL's rows, bottom up)
};

static_assert(sizeof(LuminousPictureFrameConstants) == 16, "LuminousPictureFrameConstants must be 16 bytes in C++ and MSL");

struct LuminousToneParams
{
    simd_float3x3 balance;          ///< luminous_look_balance: white balance, linear sRGB to linear sRGB, by column
    simd_float4 hsl[8];             ///< luminous_hsl: hue, saturation, lightness of each colour range in xyz
    simd_float4 monoWeights;        ///< luminous_look_mono_weights in xyz
    simd_float4 balanceShadows;     ///< luminous_look_balance_shadows in xyz, log2 gains
    simd_float4 balanceMidtones;    ///< luminous_look_balance_midtones in xyz
    simd_float4 balanceHighlights;  ///< luminous_look_balance_highlights in xyz
    simd_float4 wornParts;          ///< luminous_worn_parts: dust, scratches, stains, edges
    simd_float4 leakColour;         ///< luminous_leak_colour in xyz, the hue at full brightness
    simd_float4 frameRect;          ///< luminous_frame_rect: the picture's part of the frame, x0 y0 x1 y1 (PictureFrame)
    int tonemapType;                ///< tonemap_type; -1 leaves Second Life's choice
    float tonemapMix;               ///< tonemap_mix; below 0 the sky's own
    int lookOn;                     ///< luminous_look_on
    float ev;                       ///< luminous_look_ev, a factor
    float contrast;
    float saturation;
    float mono;
    float sepia;                    ///< luminous_look_tone
    float highlights;
    float shadows;
    float vibrance;
    float whites;
    float blacks;
    float grain;                    ///< luminous_look_grain in the tonemap pass (0 when the sharpening pass draws it)
    float grainSize;                ///< luminous_look_grain_size
    float worn;                     ///< luminous_worn in the tonemap pass
    float wornPaper;
    float wornSeed;
    float leak;                     ///< luminous_leak
    float leakPlace;
    float leakSize;
    float leakHardness;
    float leakRainbow;
    float leakSeed;
    float lutStrength;              ///< luminous_lut_strength; 0 without a table
    float lutSize;                  ///< luminous_lut_size
    float frameScale;               ///< luminous_frame_scale: picture heights per frame height
    float pictureAspect;            ///< luminous_picture_aspect; 0 the frame's own
    float frame;                    ///< luminous_frame: a render's frame, for fresh noise
    float pad0;
    float casGrain;                 ///< luminous_look_grain in the sharpening pass
    float casWorn;                  ///< luminous_worn in the sharpening pass
};

static_assert(sizeof(LuminousToneParams) == 416, "LuminousToneParams must be 416 bytes in C++ and MSL");

#endif // LUMINOUS_TONE_SHARED_H
