// The lens group's parameters, shared by C++ and MSL. LuminousPhotoMode::fillLensParams fills
// them with what the translated lens passes bind: the bloom chain, clarity, halation, bloom,
// anamorphic streaks, lens flare, chromatic aberration, distortion, glitch and the vignette.
// The types are <simd/simd.h>'s, which have MSL's sizes and alignment on both sides. After them, the
// constants of the native passes' own dispatches and draws.

#ifndef LUMINOUS_LENS_SHARED_H
#define LUMINOUS_LENS_SHARED_H

#include <simd/simd.h>

enum LuminousLensSlots : unsigned int
{
    LUMINOUS_LENS_BUFFER_PARAMS = 0,     ///< LuminousLensParams
    LUMINOUS_LENS_BUFFER_PASS = 1,       ///< the dispatch's or draw's own constants (the structs below)
    LUMINOUS_LENS_TEXTURE_FRAME = 0,     ///< the HDR frame
    LUMINOUS_LENS_TEXTURE_EXPOSURE = 1,  ///< the metered exposure, 1 x 1
    LUMINOUS_LENS_TEXTURE_DEPTH = 2,     ///< the frame's depth (stars in the sky are left out of streaks and flare)
    LUMINOUS_LENS_TEXTURE_BLUR = 3,      ///< the bloom chain's down level 0, half the chain's size
    LUMINOUS_LENS_TEXTURE_CLARITY = 4,   ///< clarity's up chain from a quarter of the chain's size down
    LUMINOUS_LENS_TEXTURE_HALATION = 5,  ///< halation's up chain, likewise
    LUMINOUS_LENS_TEXTURE_BLOOM = 6,     ///< bloom's blur: the down chain's level 0 once bloom stepped its up chain in it
    LUMINOUS_LENS_BLOOM_LEVELS = 6,      ///< the chain's levels: half the frame, a quarter, ...
    LUMINOUS_LENS_UP_CHAINS = 3,         ///< up chains one dispatch steps at once
    LUMINOUS_LENS_DOWN_TILE = 16,        ///< second-level texels per threadgroup on each axis (luminous_lens_bloom_down)
    LUMINOUS_LENS_DOWN_SPAN = 38,        ///< first-level texels a threadgroup holds on each axis: its 32, 3 each side
    LUMINOUS_LENS_GROUP_SIDE = 16,       ///< every 2D kernel's threadgroup: 16 x 16 threads
    LUMINOUS_LENS_STREAK_THREADS = 256,  ///< threads per row of the anamorphic streaks
    LUMINOUS_LENS_STREAK_MAX = 4096,     ///< quarter-width texels a row of the streaks may have (a 16384-wide frame)
};

/// What one rewrite of the frame by the tone passes applies, in this order (LuminousLensToneConstants::mode).
enum LuminousLensToneMode : unsigned int
{
    LUMINOUS_LENS_TONE_CLARITY = 1u << 0,
    LUMINOUS_LENS_TONE_HALATION = 1u << 1,
    LUMINOUS_LENS_TONE_BLOOM = 1u << 2,  ///< bloom's blend toward its blur, then its glow above 0.3
};

/// What the finish pass draws: chromatic aberration, then distortion, then glitch (LuminousLensFinishConstants::mode).
enum LuminousLensFinishMode : unsigned int
{
    LUMINOUS_LENS_FINISH_ABERRATION = 1u << 0,
    LUMINOUS_LENS_FINISH_DISTORTION = 1u << 1,
    LUMINOUS_LENS_FINISH_GLITCH = 1u << 2,
};

struct LuminousLensParams
{
    simd_float4 glitchParts;    ///< luminous_glitch_parts: split, VHS, waves, lines
    simd_float4 streakTint;     ///< luminous_streak_tint in xyz, scaled to the classic blue's brightness
    simd_float4 sunColour;      ///< luminous_sun_colour in xyz, brightest channel 1
    simd_float2 sunUV;          ///< luminous_sun_uv: the sun (or moon) on screen
    simd_float2 frameSize;      ///< the HDR frame's pixels
    float frameScale;           ///< luminous_frame_scale: picture heights per frame height
    float pictureAspect;        ///< luminous_picture_aspect; 0 the frame's own
    float aspect;               ///< luminous_aspect: the frame's width over its height
    float frame;                ///< luminous_frame: a render's frame
    // bloom chain and bloom
    float bloomSize;            ///< the chain's weight for bloom (luminous_weight)
    float bloomStrength;        ///< the blend onto the frame, at most 0.3
    float bloomGlow;            ///< the spill above 0.3, given to the diffusion shader as luminous_diffusion
    float clarity;              ///< luminous_clarity; 0: not drawn
    // halation
    float halation;             ///< luminous_halation, 0 when not drawn now
    float halationReach;        ///< the chain's weight for halation
    // anamorphic streaks
    float streak;               ///< the strength (luminous_weight on the add); 0: not drawn
    float streakGain;           ///< luminous_streak_gain
    float streakErode;          ///< luminous_streak_erode
    float streakDecay;          ///< luminous_streak_decay, per texel of the quarter-width target
    float streakReach;          ///< texels of the quarter-width target the streak reaches
    // lens flare
    float flare;                ///< luminous_flare: the composite's strength, Amplify included; 0: not drawn
    float flareThreshold;       ///< luminous_flare_threshold
    float flareErode;           ///< luminous_flare_erode
    float flareGhosts;
    float flareHalo;
    float flareStar;
    float flareStarBlur;        ///< 0 without mip levels
    float flareVariation;
    float flareDirt;
    float sunWeight;            ///< luminous_sun_weight: how much of the sun's flare shows
    int flareLines;             ///< luminous_flare_lines: spikes / 2
    int flareStarTaps;          ///< 32 in a picture, 16 live
    int flareBlades;            ///< 0 round
    float flareAngle;
    float flareStarLength;
    // chromatic aberration, distortion, glitch
    float aberration;           ///< luminous_ca_amount in pixels of this frame
    float distortion;           ///< luminous_distortion; 0: not drawn
    float glitch;               ///< luminous_glitch; 0: not drawn
    float glitchLineSize;
    float glitchSeed;
    // vignette (multiplied onto the frame)
    float vignetteAmount;       ///< 0: not drawn
    float vignetteSize;
    float vignetteSoftness;
    float vignetteRoundness;
    float pad0;
};

static_assert(sizeof(LuminousLensParams) == 224, "LuminousLensParams must be 224 bytes in C++ and MSL");

/// One dispatch of the down chain (luminous_lens_bloom_down): level A from the bound source, and with hasB level B
/// from A, which the threadgroup holds in its own memory.
struct LuminousLensDownConstants
{
    simd_uint2 sizeA;      ///< the first level written
    simd_uint2 sizeB;      ///< the second; unused without hasB
    simd_float2 srcTexel;  ///< one texel of the source, in uv
    unsigned int hasB;
    unsigned int pad0;
};

static_assert(sizeof(LuminousLensDownConstants) == 32, "LuminousLensDownConstants must be 32 bytes in C++ and MSL");

/// One step of up to three up chains at one level (luminous_lens_bloom_up): each chain's level becomes (1 - w) of the
/// down chain's level and w of a tent over its own coarser level.
struct LuminousLensUpConstants
{
    simd_uint2 size;         ///< this level
    simd_float2 coarseTexel; ///< one texel of the coarser level, in uv
    simd_float4 weights;     ///< per chain
    unsigned int chains;     ///< 1 to LUMINOUS_LENS_UP_CHAINS
    unsigned int pad0;
    unsigned int pad1;
    unsigned int pad2;
};

static_assert(sizeof(LuminousLensUpConstants) == 48, "LuminousLensUpConstants must be 48 bytes in C++ and MSL");

/// The rewrite of the frame by clarity, halation and bloom (luminous_lens_tone_fs), and bloom's blend in place
/// (luminous_lens_bloom_fs).
struct LuminousLensToneConstants
{
    simd_float2 clarityTexel;  ///< one texel of clarity's quarter-size level, in uv
    simd_float2 halationTexel; ///< one texel of halation's quarter-size level
    simd_float2 bloomTexel;    ///< one texel of bloom's half-size level
    float clarityWeight;       ///< the chain's weight, for the half-size level made here
    float halationWeight;
    unsigned int mode;         ///< LuminousLensToneMode
    unsigned int pad0;
    unsigned int pad1;
    unsigned int pad2;
};

static_assert(sizeof(LuminousLensToneConstants) == 48, "LuminousLensToneConstants must be 48 bytes in C++ and MSL");

/// The anamorphic streaks' quarter-width target.
struct LuminousLensStreakConstants
{
    simd_uint2 size;
    simd_float2 texel;  ///< one texel of it, in uv
};

static_assert(sizeof(LuminousLensStreakConstants) == 16, "LuminousLensStreakConstants must be 16 bytes in C++ and MSL");

/// The lens flare's targets: the half-size light and flare, the quarter-size soft copy.
struct LuminousLensFlareConstants
{
    simd_uint2 halfSize;
    simd_uint2 quarterSize;
    simd_float2 jitter; ///< a frame settled for MetalFX temporal: its jitter in uv, else 0. The light and the sun's seen
                        ///< share are then taken where the camera without it saw the frame, and the flare added that far off
    int settled;        ///< 1: the bright pass also writes the starburst's light into the light's alpha and the sun's seen
                        ///< share into the 1 x 1 sun texture, settled before the rest reads them
    int pad0;
};

static_assert(sizeof(LuminousLensFlareConstants) == 32, "LuminousLensFlareConstants must be 32 bytes in C++ and MSL");

/// The finish pass: which of aberration, distortion and glitch it draws.
struct LuminousLensFinishConstants
{
    unsigned int mode; ///< LuminousLensFinishMode
    unsigned int pad0;
    unsigned int pad1;
    unsigned int pad2;
};

static_assert(sizeof(LuminousLensFinishConstants) == 16, "LuminousLensFinishConstants must be 16 bytes in C++ and MSL");

#endif // LUMINOUS_LENS_SHARED_H
