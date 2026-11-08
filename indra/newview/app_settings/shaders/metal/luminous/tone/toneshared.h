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
    LUMINOUS_TONE_BUFFER_TEMPORAL = 0,  ///< LuminousTemporalConstants (MetalFX temporal's programs)
    LUMINOUS_TONE_TEXTURE_BEFORE = 5,   ///< the HDR frame before the alpha drawn since (the reactive mask)
    LUMINOUS_TONE_TEXTURE_MASK = 6,     ///< the reactive mask so far this frame
    LUMINOUS_TONE_TEXTURE_BEFORE_DEPTH = 7, ///< the depth before the alpha drawn since (the reactive mask)
    LUMINOUS_TONE_BUFFER_OVERLAY = 1,   ///< LuminousOverlayConstants (the overlays after the present)
    LUMINOUS_TONE_TEXTURE_OVERLAY = 8,  ///< a copy of the world view as the present drew it (the capture frame's margin)
    LUMINOUS_TONE_BUFFER_SCREEN = 15,   ///< LuminousTemporalScreenConstants (MetalFX temporal's presents)
    LUMINOUS_TONE_HOLD_BLOCK = 32,      ///< texels per side of a block the hold compares
    LUMINOUS_TONE_HOLD_TILE = 8,        ///< texels per side of a tile the hold's first pass compares (a block's part)
};

/// The reactive mask's stages: how strongly the light alpha added marks a texel, and where it does.
struct LuminousTemporalMaskConstants
{
    float strength;   ///< the mask times this, 0 to 1
    float fadeStart;  ///< metres from the camera where the mask begins to fade out
    float fadeEnd;    ///< metres where it has faded out; 0: no fade
    float blendedOnly; ///< 1: only texels whose depth did not change (blended surfaces, which write none)
    float projA;      ///< the projection's [2][2] and [3][2], for the distance from depth
    float projB;
    float pad0;
    float pad1;
};

static_assert(sizeof(LuminousTemporalMaskConstants) == 32, "LuminousTemporalMaskConstants must be 32 bytes in C++ and MSL");

/// A blurred layer of a frame drawn for the temporal scaler (Second Life's depth of field, the glow map), made where
/// the camera without the jitter saw the frame, averaged with the layers of the frames before, moved by the camera.
struct LuminousTemporalSettleConstants
{
    simd_float4x4 toLast;    ///< this frame's clip space, without the jitter, to the last frame's
    simd_float2 region;      ///< the layer's texels over the whole frame (the lower left part of its target)
    simd_float2 lastSize;    ///< the last average's texture in texels
    simd_float2 depthJitter; ///< this frame's jitter in texture coordinates: a place's depth is read that far on
    simd_float2 frameSize;   ///< the live view's size in texels, for the camera's motion
    float share;             ///< this frame's share where the camera is still and the layer unchanged: 1 / frames
    float motionFull;        ///< texels of the live view the camera moved a place by, for this frame taken whole
    float reset;             ///< 1: no last average, this frame's layer as it is
    float react;             ///< 1: this frame also taken whole where the layer changed much; 2: where the last weight
                             ///< (alpha) lies outside this frame's round the place; 0: by the camera alone
};

static_assert(sizeof(LuminousTemporalSettleConstants) == 112, "LuminousTemporalSettleConstants must be 112 bytes in C++ and MSL");

/// MetalFX temporal's programs: the motion pass at the live view's size, and the present of the scaled frame.
struct LuminousTemporalConstants
{
    simd_float4x4 clipToLastClip; ///< this frame's clip space (jittered) to the last frame's (without jitter)
    simd_float2 size;             ///< the live view's size in texels
    simd_float2 jitter;           ///< this frame's jitter in texels (GL's rows, up)
    simd_float2 motionSign;       ///< per axis 1, or -1 to negate the vectors
    float grain;                  ///< film grain on the scaled frame, 0 to 1 (0 without)
    float grainSize;              ///< pixels of a 1080-line frame
    float outputHeight;           ///< the scaled frame's height in texels
    float pad0;
    float pad1;
    float pad2;
};

static_assert(sizeof(LuminousTemporalConstants) == 112, "LuminousTemporalConstants must be 112 bytes in C++ and MSL");

/// What a frame drawn for MetalFX temporal leaves out because it is fixed to the screen, drawn by the present on the
/// scaled frame at the world view's size, or on the Catmull-Rom scaled one when the scaler gives no frame: live lens
/// distortion, Glitch, the light leak and Worn photo (film grain is LuminousTemporalConstants'). The values are
/// LuminousToneParams' and the lens's; zero where nothing is left out.
struct LuminousTemporalScreenConstants
{
    simd_float4 wornParts;   ///< Worn photo's dust, scratches, stains, edges
    simd_float4 leakColour;  ///< the light leak's hue at full brightness in xyz
    simd_float4 glitchParts; ///< Glitch's colour split, VHS, waves and scan lines, each 0 to 1
    simd_float2 outputSize;  ///< the world view's size in pixels, the present's
    float distortion;       ///< live lens distortion, -1 to 1; 0 without
    float frameScale;       ///< picture heights per frame height
    float pictureAspect;    ///< the picture's width over its height; 0 the frame's own
    float worn;             ///< Worn photo, 0 to 1; 0 without
    float wornPaper;
    float wornSeed;
    float leak;             ///< the light leak, 0 to 1; 0 without
    float leakPlace;
    float leakSize;
    float leakHardness;
    float leakRainbow;
    float leakSeed;
    float glitch;           ///< Glitch, 0 to 1; 0 without
    float glitchSeed;       ///< Glitch's pattern, a whole number
    float glitchLineSize;   ///< a scan line every this many pixels of a 1080-line picture
    float pad0;
    float pad1;
    float pad2;
};

static_assert(sizeof(LuminousTemporalScreenConstants) == 128, "LuminousTemporalScreenConstants must be 128 bytes in C++ and MSL");

/// The picture frame over the live view: what a render in a shape of its own keeps.
struct LuminousPictureFrameConstants
{
    simd_float4 rect; ///< the picture in the target's pixels: left, bottom, right, top (GL's rows, bottom up)
};

static_assert(sizeof(LuminousPictureFrameConstants) == 16, "LuminousPictureFrameConstants must be 16 bytes in C++ and MSL");

/// What a frame drawn for MetalFX's temporal scaler leaves out because it is fixed to the screen, drawn over the
/// window's world view after the present (overlay.metal). Rects in the world view's pixels, GL's rows (bottom up).
struct LuminousOverlayConstants
{
    simd_float4 pictureRect;   ///< the picture frame's picture: left, bottom, right, top
    simd_float4 captureRect;   ///< Firestorm's capture frame: left, bottom, right, top
    simd_float4 captureBorder; ///< its border's colour in xyz, its width in pixels in w
    simd_float2 origin;        ///< the world view's lower left corner in the back buffer's pixels
    simd_float2 pad;
};

static_assert(sizeof(LuminousOverlayConstants) == 64, "LuminousOverlayConstants must be 64 bytes in C++ and MSL");

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
