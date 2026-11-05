// The air light group's parameters, shared by C++ and MSL: sun shafts, volumetric light and
// projector beams. LuminousPhotoMode::fillAirParams fills LuminousAirParams with what the translated
// passes bind (their own uniforms and those bindDeferredShader gives them); fillAirBeams fills one
// LuminousAirBeam per projector as addProjectorBeam computes it. LuminousAirDraw is the native
// passes' own: the sizes and what one dispatch or draw adds.
// The types are <simd/simd.h>'s, which have MSL's sizes and alignment on both sides.

#ifndef LUMINOUS_AIR_SHARED_H
#define LUMINOUS_AIR_SHARED_H

#include <simd/simd.h>

enum LuminousAirSlots : unsigned int
{
    LUMINOUS_AIR_BUFFER_PARAMS = 0,     ///< LuminousAirParams
    LUMINOUS_AIR_BUFFER_DRAW = 1,       ///< LuminousAirDraw
    LUMINOUS_AIR_BUFFER_BEAMS = 2,      ///< LuminousAirBeam[LuminousAirDraw::beamCount]

    LUMINOUS_AIR_TEXTURE_DEPTH = 0,     ///< the frame's depth
    LUMINOUS_AIR_TEXTURE_SUN0 = 1,      ///< the sun's four cascades, slots 1 to 4
    LUMINOUS_AIR_TEXTURE_SPOT0 = 5,     ///< the two projector shadow maps, slots 5 and 6
    LUMINOUS_AIR_TEXTURE_MASK = 7,      ///< the sun shafts' softened mask
    LUMINOUS_AIR_TEXTURE_AIR = 8,       ///< the half-size sum of what is drawn at half size
    LUMINOUS_AIR_TEXTURE_FRAME = 9,     ///< the HDR frame (the mask kernel only)
    LUMINOUS_AIR_TEXTURE_PROJECTOR0 = 9,///< projected textures, slots 9 to 15 (not the mask kernel)
    LUMINOUS_AIR_PROJECTOR_TEXTURES = 7,///< projected textures one dispatch or draw can read

    LUMINOUS_AIR_BEAMS_MAX = 16,        ///< LuminousPhotoProjectorBeamsMax's top
    LUMINOUS_AIR_MASK_THREADS = 16,     ///< the mask kernel's threadgroup side
    LUMINOUS_AIR_MASK_HALO_MAX = 9,     ///< the softening's reach in mask texels, at most (spread 4: 2 x 4 + 1)
    LUMINOUS_AIR_THREADS = 8,           ///< the half-size kernel's threadgroup side
};

struct LuminousAirParams
{
    simd_float4 lightNorm;   ///< toward the sun or moon (LIGHTNORM) in xyz
    simd_float2 sunUV;       ///< luminous_sun_uv
    simd_float2 srcTexel;    ///< luminous_src_texel: the mask's grid in the frame's terms
    float shaftFade;         ///< how much of the shafts show; 0 with the sun behind the camera
    float shaftRadius;       ///< luminous_shaft_radius
    float shaftSoftness;     ///< the mask's blur spread, in mask texels (0: not softened)
    float shaftLength;       ///< luminous_shaft_length
    float shaftStrength;     ///< luminous_shaft_strength, the fade included
    int shaftSamples;        ///< luminous_shaft_samples
    float aspect;            ///< luminous_aspect
    int volSamples;          ///< luminous_vol_samples
    float volDensity;
    float volAnisotropy;
    float volDistance;
    int sunUp;               ///< SUN_UP_FACTOR
    int beamSamples;         ///< luminous_pb_samples
    float beamDensity;
    float beamAnisotropy;
    float frame;             ///< luminous_frame

    simd_float4x4 invProj;          ///< inv_proj: GL's clip space (depth -1 to 1) to eye space
    simd_float4x4 shadowMatrix[6];  ///< shadow_matrix: eye space to the sun's cascades (0-3) and the spot maps (4, 5)
    simd_float4 shadowClip;         ///< shadow_clip: each cascade's far view depth
    simd_float4 sunlit;             ///< xyz: calcAtmosphericVarsLinear's sunlit, which depends on no position
    simd_float4 lightDir;           ///< xyz: sun_dir or moon_dir by sunUp, eye space
    float shadowBias;               ///< shadow_bias
    float spotShadowBias;           ///< spot_shadow_bias
    float liveScale;                ///< liveRenderScale()
    int sunShadows;                 ///< RenderShadowDetail >= 1: the volumetric light can be drawn
    int spotShadows;                ///< RenderShadowDetail >= 2: SPOT_SHADOW
    int volHalf;                    ///< the volumetric light at half size (low quality live, not a picture)
    int beamsHalf;                  ///< the projector beams at half size
    int shaftsHalf;                 ///< the sun shafts gathered at the mask's size
    int noPost;                     ///< gSnapshotNoPost: nothing of the air is drawn
    int beamsMax;                   ///< LuminousPhotoProjectorBeamsMax, 1 to 16
    int pad0;
    int pad1;
};

static_assert(sizeof(LuminousAirParams) == 640, "LuminousAirParams must be 640 bytes in C++ and MSL");

/// One projector, as addProjectorBeam binds it.
struct LuminousAirBeam
{
    simd_float4x4 projMat;   ///< luminous_pb_proj_mat: eye space to the projector's texture space
    simd_float4 center;      ///< xyz luminous_pb_center (eye space), w luminous_pb_size (range, metres)
    simd_float4 color;       ///< xyz luminous_pb_color (linear), w luminous_pb_falloff
    simd_float4 projN;       ///< xyz luminous_pb_proj_n, w luminous_pb_proj_focus
    simd_float4 projOrigin;  ///< xyz luminous_pb_proj_origin (the apex), w luminous_pb_proj_range
    simd_float4 rect;        ///< where its range can show, GL uv: x0, y0, x1, y1 (the scissor)
    float projLod;           ///< luminous_pb_proj_lod
    int shadowIdx;           ///< luminous_pb_shadow_idx: -1, or which spot shadow map
    float shadowFade;        ///< luminous_pb_shadow_fade
    int texture;             ///< which of the draw's projected textures (set by the native pass)
};

static_assert(sizeof(LuminousAirBeam) == 160, "LuminousAirBeam must be 160 bytes in C++ and MSL");

/// What one dispatch or draw does.
struct LuminousAirDraw
{
    simd_float2 depthSize;   ///< the depth buffer's pixels
    simd_float2 targetSize;  ///< the pixels this dispatch or draw writes
    simd_float2 maskSize;    ///< the shafts' mask
    simd_float2 airTexel;    ///< one texel of the half-size sum, in uv
    simd_float2 jitter;      ///< a frame settled for MetalFX temporal: its jitter in uv, else 0. The mask and the sum are
                             ///< then made where the camera without it saw the frame, and read back that far off; what
                             ///< reads the depth (volumetric light, beams) reads it that far on, where this frame shows it
    float shaftReach;        ///< luminous_shaft_reach: beyond this from the sun the mask is 0, in the frame's heights
    int maskHalo;            ///< the mask kernel's halo in mask texels
    int vol;                 ///< add the volumetric light
    int shafts;              ///< add the sun shafts' gather
    int beamFirst;           ///< the first beam of LUMINOUS_AIR_BUFFER_BEAMS this one adds
    int beamCount;           ///< how many
    int accumulate;          ///< the half-size kernel adds to what the sum holds
    int addAir;              ///< the full-size draw adds the half-size sum with the tent
};

static_assert(sizeof(LuminousAirDraw) == 72, "LuminousAirDraw must be 72 bytes in C++ and MSL");

#endif // LUMINOUS_AIR_SHARED_H
