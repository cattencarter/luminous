/**
 * @file llshadermgr.h
 * @brief Shader Manager
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_SHADERMGR_H
#define LL_SHADERMGR_H

#include "llgl.h"
#include "llglslshader.h"

namespace LL::RHI // [Viewer Lab]
{
struct ProgramDesc;
enum class ShaderStage : U8;
} // namespace LL::RHI

class LLShaderMgr
{
public:
    LLShaderMgr();
    virtual ~LLShaderMgr();

    // Note: although you can use statically hashed strings to just bind a random uniform, it's generally preferably that you use this.
    // Always document what the actual shader uniform is next to the shader uniform in this struct.
    // clang-format off
    typedef enum
    {                                       // Shader uniform name, set in LLShaderMgr::initAttribsAndUniforms()
        MODELVIEW_MATRIX = 0,               //  "modelview_matrix"
        PROJECTION_MATRIX,                  //  "projection_matrix"
        INVERSE_PROJECTION_MATRIX,          //  "inv_proj"
        MODELVIEW_PROJECTION_MATRIX,        //  "modelview_projection_matrix"
        INVERSE_MODELVIEW_MATRIX,           //  "inv_modelview"
        IDENTITY_MATRIX,                    //  "identity_matrix"
        NORMAL_MATRIX,                      //  "normal_matrix"
        TEXTURE_MATRIX0,                    //  "texture_matrix0"
        TEXTURE_MATRIX1,                    //  "texture_matrix1"
        TEXTURE_MATRIX2,                    //  "texture_matrix2"
        TEXTURE_MATRIX3,                    //  "texture_matrix3"
        OBJECT_PLANE_S,                     //  "object_plane_s"
        OBJECT_PLANE_T,                     //  "object_plane_t"

        TEXTURE_BASE_COLOR_TRANSFORM,         //  "texture_base_color_transform" (GLTF)
        TEXTURE_NORMAL_TRANSFORM,             //  "texture_normal_transform" (GLTF)
        TEXTURE_METALLIC_ROUGHNESS_TRANSFORM, //  "texture_metallic_roughness_transform" (GLTF)
        TEXTURE_OCCLUSION_TRANSFORM,          //  "texture_occlusion_transform" (GLTF)
        TEXTURE_EMISSIVE_TRANSFORM,           //  "texture_emissive_transform" (GLTF)
        BASE_COLOR_TEXCOORD,                  //  "base_color_texcoord" (GLTF)
        EMISSIVE_TEXCOORD,                    //  "emissive_texcoord" (GLTF)
        NORMAL_TEXCOORD,                      //  "normal_texcoord" (GLTF)
        METALLIC_ROUGHNESS_TEXCOORD,          //  "metallic_roughness_texcoord" (GLTF)
        OCCLUSION_TEXCOORD,                   //  "occlusion_texcoord" (GLTF)
        GLTF_NODE_ID,                         //  "gltf_node_id" (GLTF)
        GLTF_MATERIAL_ID,                     //  "gltf_material_id" (GLTF)

        TERRAIN_TEXTURE_TRANSFORMS,           //  "terrain_texture_transforms" (GLTF)

        VIEWPORT,                           //  "viewport"
        LIGHT_POSITION,                     //  "light_position"
        LIGHT_DIRECTION,                    //  "light_direction"
        LIGHT_ATTENUATION,                  //  "light_attenuation"
        LIGHT_DEFERRED_ATTENUATION,         //  "light_deferred_attenuation"
        LIGHT_DIFFUSE,                      //  "light_diffuse"
        LIGHT_AMBIENT,                      //  "light_ambient"
        MULTI_LIGHT_COUNT,                  //  "light_count"
        MULTI_LIGHT,                        //  "light"
        MULTI_LIGHT_COL,                    //  "light_col"
        MULTI_LIGHT_FAR_Z,                  //  "far_z"
        PROJECTOR_MATRIX,                   //  "proj_mat"
        PROJECTOR_NEAR,                     //  "proj_near"
        PROJECTOR_P,                        //  "proj_p"
        PROJECTOR_N,                        //  "proj_n"
        PROJECTOR_ORIGIN,                   //  "proj_origin"
        PROJECTOR_RANGE,                    //  "proj_range"
        PROJECTOR_AMBIANCE,                 //  "proj_ambiance"
        PROJECTOR_SHADOW_INDEX,             //  "proj_shadow_idx"
        PROJECTOR_SHADOW_FADE,              //  "shadow_fade"
        PROJECTOR_FOCUS,                    //  "proj_focus"
        PROJECTOR_LOD,                      //  "proj_lod"
        PROJECTOR_AMBIENT_LOD,              //  "proj_ambient_lod"
        DIFFUSE_COLOR,                      //  "color"
        EMISSIVE_COLOR,                     //  "emissiveColor"
        METALLIC_FACTOR,                    //  "metallicFactor"
        ROUGHNESS_FACTOR,                   //  "roughnessFactor"
        MIRROR_FLAG,                        //  "mirror_flag"
        CLIP_PLANE,                         //  "clipPlane"
        CLIP_SIGN,                          //  "clipSign"
        DIFFUSE_MAP,                        //  "diffuseMap"
        ALTERNATE_DIFFUSE_MAP,              //  "altDiffuseMap"
        SPECULAR_MAP,                       //  "specularMap"
        METALLIC_ROUGHNESS_MAP,             //  "metallicRoughnessMap"
        NORMAL_MAP,                         //  "normalMap"
        OCCLUSION_MAP,                      //  "occlusionMap"
        EMISSIVE_MAP,                       //  "emissiveMap"
        BUMP_MAP,                           //  "bumpMap"
        BUMP_MAP2,                          //  "bumpMap2"
        ENVIRONMENT_MAP,                    //  "environmentMap"
        SCENE_MAP,                          //  "sceneMap"
        SCENE_DEPTH,                        //  "sceneDepth"
        REFLECTION_PROBES,                  //  "reflectionProbes"
        IRRADIANCE_PROBES,                  //  "irradianceProbes"
        HERO_PROBE,                         //  "heroProbes"
        CLOUD_NOISE_MAP,                    //  "cloud_noise_texture"
        CLOUD_NOISE_MAP_NEXT,               //  "cloud_noise_texture_next"
        LIGHTNORM,                          //  "lightnorm"
        SUNLIGHT_COLOR,                     //  "sunlight_color"
        AMBIENT,                            //  "ambient_color"
        SKY_HDR_SCALE,                      //  "sky_hdr_scale"
        SKY_SUNLIGHT_SCALE,                 //  "sky_sunlight_scale"
        SKY_AMBIENT_SCALE,                  //  "sky_ambient_scale"
        CLASSIC_MODE,                       //  "classic_mode"
        BLUE_HORIZON,                       //  "blue_horizon"
        BLUE_DENSITY,                       //  "blue_density"
        HAZE_HORIZON,                       //  "haze_horizon"
        HAZE_DENSITY,                       //  "haze_density"
        CLOUD_SHADOW,                       //  "cloud_shadow"
        DENSITY_MULTIPLIER,                 //  "density_multiplier"
        DISTANCE_MULTIPLIER,                //  "distance_multiplier"
        MAX_Y,                              //  "max_y"
        GLOW,                               //  "glow"
        CLOUD_COLOR,                        //  "cloud_color"
        CLOUD_POS_DENSITY1,                 //  "cloud_pos_density1"
        CLOUD_POS_DENSITY2,                 //  "cloud_pos_density2"
        CLOUD_SCALE,                        //  "cloud_scale"
        GAMMA,                              //  "gamma"
        SCENE_LIGHT_STRENGTH,               //  "scene_light_strength"
        LIGHT_CENTER,                       //  "center"
        LIGHT_SIZE,                         //  "size"
        LIGHT_FALLOFF,                      //  "falloff"
        BOX_CENTER,                         //  "box_center"
        BOX_SIZE,                           //  "box_size"

        GLOW_MIN_LUMINANCE,                 //  "minLuminance"
        GLOW_MAX_EXTRACT_ALPHA,             //  "maxExtractAlpha"
        GLOW_LUM_WEIGHTS,                   //  "lumWeights"
        GLOW_WARMTH_WEIGHTS,                //  "warmthWeights"
        GLOW_WARMTH_AMOUNT,                 //  "warmthAmount"
        GLOW_STRENGTH,                      //  "glowStrength"
        GLOW_DELTA,                         //  "glowDelta"
        GLOW_NOISE_MAP,                     //  "glowNoiseMap"

        MINIMUM_ALPHA,                      //  "minimum_alpha"
        EMISSIVE_BRIGHTNESS,                //  "emissive_brightness"

        DEFERRED_SHADOW_MATRIX,             //  "shadow_matrix"
        DEFERRED_ENV_MAT,                   //  "env_mat"
        DEFERRED_SHADOW_CLIP,               //  "shadow_clip"
        DEFERRED_SUN_WASH,                  //  "sun_wash"
        DEFERRED_SHADOW_NOISE,              //  "shadow_noise"
        DEFERRED_BLUR_SIZE,                 //  "blur_size"
        DEFERRED_SSAO_RADIUS,               //  "ssao_radius"
        DEFERRED_SSAO_MAX_RADIUS,           //  "ssao_max_radius"
        DEFERRED_SSAO_FACTOR,               //  "ssao_factor"
        DEFERRED_SSAO_FACTOR_INV,           //  "ssao_factor_inv"
        DEFERRED_SSAO_EFFECT_MAT,           //  "ssao_effect_mat"
        DEFERRED_SCREEN_RES,                //  "screen_res"
        DEFERRED_NEAR_CLIP,                 //  "near_clip"
        DEFERRED_SHADOW_OFFSET,             //  "shadow_offset"
        DEFERRED_SHADOW_BIAS,               //  "shadow_bias"
        DEFERRED_SPOT_SHADOW_BIAS,          //  "spot_shadow_bias"
        DEFERRED_SPOT_SHADOW_OFFSET,        //  "spot_shadow_offset"
        DEFERRED_SUN_DIR,                   //  "sun_dir"
        DEFERRED_MOON_DIR,                  //  "moon_dir"
        DEFERRED_SHADOW_RES,                //  "shadow_res"
        DEFERRED_PROJ_SHADOW_RES,           //  "proj_shadow_res"
        DEFERRED_DEPTH_CUTOFF,              //  "depth_cutoff"
        DEFERRED_NORM_CUTOFF,               //  "norm_cutoff"
        DEFERRED_SHADOW_TARGET_WIDTH,       //  "shadow_target_width"

        DEFERRED_SSR_ITR_COUNT,             //  "iterationCount"
        DEFERRED_SSR_RAY_STEP,              //  "rayStep"
        DEFERRED_SSR_DIST_BIAS,             //  "distanceBias"
        DEFERRED_SSR_REJECT_BIAS,           //  "depthRejectBias"
        DEFERRED_SSR_GLOSSY_SAMPLES,        //  "glossySampleCount"
        DEFERRED_SSR_NOISE_SINE,            //  "noiseSine"
        DEFERRED_SSR_ADAPTIVE_STEP_MULT,    //  "adaptiveStepMultiplier"

        MODELVIEW_DELTA_MATRIX,             //  "modelview_delta"
        INVERSE_MODELVIEW_DELTA_MATRIX,     //  "inv_modelview_delta"
        CUBE_SNAPSHOT,                      //  "cube_snapshot"

        FXAA_TC_SCALE,                      //  "tc_scale"
        FXAA_RCP_SCREEN_RES,                //  "rcp_screen_res"
        FXAA_RCP_FRAME_OPT,                 //  "rcp_frame_opt"
        FXAA_RCP_FRAME_OPT2,                //  "rcp_frame_opt2"

        DOF_FOCAL_DISTANCE,                 //  "focal_distance"
        DOF_BLUR_CONSTANT,                  //  "blur_constant"
        DOF_TAN_PIXEL_ANGLE,                //  "tan_pixel_angle"
        DOF_MAGNIFICATION,                  //  "magnification"
        DOF_MAX_COF,                        //  "max_cof"
        DOF_RES_SCALE,                      //  "res_scale"
        DOF_WIDTH,                          //  "dof_width"
        DOF_HEIGHT,                         //  "dof_height"

        DEFERRED_DEPTH,                     //  "depthMap"
        DEFERRED_SHADOW0,                   //  "shadowMap0"
        DEFERRED_SHADOW1,                   //  "shadowMap1"
        DEFERRED_SHADOW2,                   //  "shadowMap2"
        DEFERRED_SHADOW3,                   //  "shadowMap3"
        DEFERRED_SHADOW4,                   //  "shadowMap4"
        DEFERRED_SHADOW5,                   //  "shadowMap5"
        DEFERRED_POSITION,                  //  "positionMap"
        DEFERRED_DIFFUSE,                   //  "diffuseRect"
        DEFERRED_SPECULAR,                  //  "specularRect"
        DEFERRED_EMISSIVE,                  //  "emissiveRect"
        EXPOSURE_MAP,                       //  "exposureMap"
        DEFERRED_BRDF_LUT,                  //  "brdfLut"
        DEFERRED_NOISE,                     //  "noiseMap"
        DEFERRED_LIGHTFUNC,                 //  "lightFunc"
        DEFERRED_LIGHT,                     //  "lightMap"
        DEFERRED_BLOOM,                     //  "bloomMap"
        DEFERRED_PROJECTION,                //  "projectionMap"
        DEFERRED_NORM_MATRIX,               //  "norm_mat"
        SPECULAR_COLOR,                     //  "specular_color"
        ENVIRONMENT_INTENSITY,              //  "env_intensity"

        AVATAR_MATRIX,                      //  "matrixPalette"
        AVATAR_TRANSLATION,                 //  "translationPalette"

        // <FS:CR> Import Vignette from Exodus
        RENDER_VIGNETTE,                    //  "vignette"
        // </FS:CR> Import Vignette from Exodus

        WATER_SCREENTEX,                    //  "screenTex"
        WATER_SCREENDEPTH,                  //  "screenDepth"
        WATER_REFTEX,                       //  "refTex"
        WATER_EXCLUSIONTEX,                 //  "exclusionTex"
        WATER_EYEVEC,                       //  "eyeVec"
        WATER_TIME,                         //  "time"
        WATER_WAVE_DIR1,                    //  "waveDir1"
        WATER_WAVE_DIR2,                    //  "waveDir2"
        WATER_LIGHT_DIR,                    //  "lightDir"
        WATER_SPECULAR,                     //  "specular"
        WATER_SPECULAR_EXP,                 //  "lightExp"
        WATER_FOGCOLOR,                     //  "waterFogColor"
        WATER_FOGCOLOR_LINEAR,              //  "waterFogColorLinear"
        WATER_FOGDENSITY,                   //  "waterFogDensity"
        WATER_FOGKS,                        //  "waterFogKS"
        WATER_REFSCALE,                     //  "refScale"
        WATER_WATERHEIGHT,                  //  "waterHeight"
        WATER_WATERPLANE,                   //  "waterPlane"
        WATER_NORM_SCALE,                   //  "normScale"
        WATER_FRESNEL_SCALE,                //  "fresnelScale"
        WATER_FRESNEL_OFFSET,               //  "fresnelOffset"
        WATER_BLUR_MULTIPLIER,              //  "blurMultiplier"
        WATER_SUN_ANGLE,                    //  "sunAngle"
        WATER_SCALED_ANGLE,                 //  "scaledAngle"
        WATER_SUN_ANGLE2,                   //  "sunAngle2"

        WL_CAMPOSLOCAL,                     //  "camPosLocal"

        // [RLVa:KB] - @setsphere
        RLV_EFFECT_MODE,                    //  "rlvEffectMode"
        RLV_EFFECT_PARAM1,                  //  "rlvEffectParam1"
        RLV_EFFECT_PARAM2,                  //  "rlvEffectParam2"
        RLV_EFFECT_PARAM3,                  //  "rlvEffectParam3"
        RLV_EFFECT_PARAM4,                  //  "rlvEffectParam4"
        RLV_EFFECT_PARAM5,                  //  "rlvEffectParam5"
        // [/RLVa:KB]

        AVATAR_WIND,                        //  "gWindDir"
        AVATAR_SINWAVE,                     //  "gSinWaveParams"
        AVATAR_GRAVITY,                     //  "gGravity"

        TERRAIN_DETAIL0,                    //  "detail_0"
        TERRAIN_DETAIL1,                    //  "detail_1"
        TERRAIN_DETAIL2,                    //  "detail_2"
        TERRAIN_DETAIL3,                    //  "detail_3"

        TERRAIN_ALPHARAMP,                  //  "alpha_ramp"
        TERRAIN_PAINTMAP,                   //  "paint_map"

        TERRAIN_DETAIL0_BASE_COLOR,                //  "detail_0_base_color" (GLTF)
        TERRAIN_DETAIL1_BASE_COLOR,                //  "detail_1_base_color" (GLTF)
        TERRAIN_DETAIL2_BASE_COLOR,                //  "detail_2_base_color" (GLTF)
        TERRAIN_DETAIL3_BASE_COLOR,                //  "detail_3_base_color" (GLTF)
        TERRAIN_DETAIL0_NORMAL,                    //  "detail_0_normal" (GLTF)
        TERRAIN_DETAIL1_NORMAL,                    //  "detail_1_normal" (GLTF)
        TERRAIN_DETAIL2_NORMAL,                    //  "detail_2_normal" (GLTF)
        TERRAIN_DETAIL3_NORMAL,                    //  "detail_3_normal" (GLTF)
        TERRAIN_DETAIL0_METALLIC_ROUGHNESS,        //  "detail_0_metallic_roughness" (GLTF)
        TERRAIN_DETAIL1_METALLIC_ROUGHNESS,        //  "detail_1_metallic_roughness" (GLTF)
        TERRAIN_DETAIL2_METALLIC_ROUGHNESS,        //  "detail_2_metallic_roughness" (GLTF)
        TERRAIN_DETAIL3_METALLIC_ROUGHNESS,        //  "detail_3_metallic_roughness" (GLTF)
        TERRAIN_DETAIL0_EMISSIVE,                  //  "detail_0_emissive" (GLTF)
        TERRAIN_DETAIL1_EMISSIVE,                  //  "detail_1_emissive" (GLTF)
        TERRAIN_DETAIL2_EMISSIVE,                  //  "detail_2_emissive" (GLTF)
        TERRAIN_DETAIL3_EMISSIVE,                  //  "detail_3_emissive" (GLTF)

        TERRAIN_BASE_COLOR_FACTORS,                //  "baseColorFactors" (GLTF)
        TERRAIN_METALLIC_FACTORS,                  //  "metallicFactors" (GLTF)
        TERRAIN_ROUGHNESS_FACTORS,                 //  "roughnessFactors" (GLTF)
        TERRAIN_EMISSIVE_COLORS,                   //  "emissiveColors" (GLTF)
        TERRAIN_MINIMUM_ALPHAS,                    //  "minimum_alphas" (GLTF)

        REGION_SCALE,                              //  "region_scale" (GLTF)

        SHINY_ORIGIN,                       //  "origin"
        DISPLAY_GAMMA,                      //  "display_gamma"

        INSCATTER_RT,                       //  "inscatter"
        SUN_SIZE,                           //  "sun_size"
        FOG_COLOR,                          //  "fog_color"

        // precomputed textures
        TRANSMITTANCE_TEX,                  //  "transmittance_texture"
        SCATTER_TEX,                        //  "scattering_texture"
        SINGLE_MIE_SCATTER_TEX,             //  "single_mie_scattering_texture"
        ILLUMINANCE_TEX,                    //  "irradiance_texture"
        BLEND_FACTOR,                       //  "blend_factor"

        MOISTURE_LEVEL,                     //  "moisture_level"
        DROPLET_RADIUS,                     //  "droplet_radius"
        ICE_LEVEL,                          //  "ice_level"
        RAINBOW_MAP,                        //  "rainbow_map"
        HALO_MAP,                           //  "halo_map"

        MOON_BRIGHTNESS,                    //  "moon_brightness"

        CLOUD_VARIANCE,                     //  "cloud_variance"

        REFLECTION_PROBE_AMBIANCE,          //  "reflection_probe_ambiance"
        REFLECTION_PROBE_MAX_LOD,            //  "max_probe_lod"
        REFLECTION_PROBE_STRENGTH,            //  "probe_strength"
        SH_INPUT_L1R,                       //  "sh_input_r"
        SH_INPUT_L1G,                       //  "sh_input_g"
        SH_INPUT_L1B,                       //  "sh_input_b"

        SUN_MOON_GLOW_FACTOR,               //  "sun_moon_glow_factor"
        WATER_EDGE_FACTOR,                  //  "water_edge"
        SUN_UP_FACTOR,                      //  "sun_up_factor"
        MOONLIGHT_COLOR,                    //  "moonlight_color"

        DEBUG_NORMAL_DRAW_LENGTH,           //  "debug_normal_draw_length"

        SMAA_EDGE_TEX,                      //  "edgesTex"
        SMAA_AREA_TEX,                      //  "areaTex"
        SMAA_SEARCH_TEX,                    //  "searchTex"
        SMAA_BLEND_TEX,                     //  "blendTex"

        // <FS:Beq> Uniforms for snapshot frame
        SNAPSHOT_BORDER_COLOR,              // "border_color"
        SNAPSHOT_BORDER_THICKNESS,          // "border_thickness"
        SNAPSHOT_FRAME_RECT,                // "frame_rect"
        // </FS:Beq>
        END_RESERVED_UNIFORMS
    } eGLSLReservedUniforms;
    // clang-format on

    // singleton pattern implementation
    static LLShaderMgr * instance();

    virtual void initAttribsAndUniforms(void);

    bool attachShaderFeatures(LLGLSLShader * shader);
    void dumpObjectLog(GLuint ret, bool warns = true, const std::string& filename = "");
    void dumpShaderSource(U32 shader_code_count, GLchar** shader_code_text);
    bool    linkProgramObject(GLuint obj, bool suppress_errors = false);
    bool    validateProgramObject(GLuint obj);
    struct ShaderSource; // [Viewer Lab] below
    GLuint loadShaderFile(const std::string& filename, S32& shader_level, GLenum type, std::map<std::string, std::string>* defines = NULL,
                          S32 texture_index_channels = -1, ShaderSource* assembled = nullptr); // [Viewer Lab] assembled

    // [Viewer Lab] The GPU workarounds in the GLSL LLShaderMgr assembles: the viewer's
    // device's capabilities, or without a device (headless, llappearanceutility, the tests' fixture) the GL
    // rules they replace
    struct ShaderWorkarounds
    {
        bool oldSelect = false; ///< OLD_SELECT
        bool amdCard = false;   ///< IS_AMD_CARD
        bool ifChain = false;   ///< diffuseLookup() as an if chain
    };
    static ShaderWorkarounds shaderWorkarounds();

    // [Viewer Lab] One shader file's source as loadShaderFile assembles it: the lines it gives GL, with the
    // version, the defines and the indexed channels' lookup in them, the class asked for and the class the file was
    // found in. Move-only; it frees the lines. Given to loadShaderFile, the lines are compiled when they are still at
    // the class asked for, and assembled again otherwise, so that afterwards it holds what GL compiled. The joined text
    // is kept until clear() changes the lines (a move clears both sides); only assembleShaderSource (after clear()),
    // clear() and the move write mLines, and text() joins them again when their number changed since.
    struct ShaderSource
    {
        ShaderSource() = default;
        ShaderSource(ShaderSource&& other) noexcept;
        ShaderSource& operator=(ShaderSource&& other) noexcept;
        ShaderSource(const ShaderSource&)            = delete;
        ShaderSource& operator=(const ShaderSource&) = delete;
        ~ShaderSource();

        const std::string& text() const; ///< the lines joined, as recordCompiled joins them, once
        void               clear();      ///< frees the lines and resets the rest

        std::string          mFile;                   ///< the file as the program names it
        GLenum               mType = 0;               ///< its stage as a GL shader type
        std::string          mOpenFileName;           ///< the file read
        S32                  mRequestedClass = 0;     ///< the shader class asked for
        S32                  mFoundClass     = 0;     ///< the class the file was found in
        bool                 mFound          = false; ///< a file was found and assembled
        bool                 mCompiled       = false; ///< loadShaderFile's last attempt compiled it (with no GL object off GL)
        std::vector<GLchar*> mLines;                  ///< strdup'd
        mutable std::string  mText;                   ///< the lines joined by text(), once; reset by clear()
        mutable bool         mTextReady      = false; ///< mText holds the joined lines
        mutable size_t       mTextLines      = 0;     ///< how many lines mText joined
    };
    // [Viewer Lab] loadShaderFile's assembly of a file at a shader class, without its compile and without GL: false, with
    // mFound false, when no class has the file, with loadShaderFile's WARNs only when warn is set
    bool assembleShaderSource(const std::string& filename, S32 shader_level, GLenum type, std::map<std::string, std::string>* defines,
                              S32 texture_index_channels, ShaderSource& source, bool warn = true);

    // [Viewer Lab] The text loadShaderFile last compiled a file and stage from, and its digest (recordCompiled's), while
    // the GL maps below hold the file; null otherwise
    struct ObjectSource
    {
        std::string mText;
        std::string mDigest;
    };
    const ObjectSource* objectSource(const std::string& path, GLenum type) const;

    // [Viewer Lab] The whole program before anything compiles, with neither GL nor the render interface:
    // one ShaderSource per mShaderFiles entry, at the shader's level with its defines and indexed
    // channels, and the shared objects attachShaderFeatures attaches, in its order, from a walk that attaches nothing.
    // The shader's mFeatures and mRiggedVariant stay as they were. False when a file or a shared object's text is
    // missing.
    bool assembleProgram(LLGLSLShader* shader, std::vector<ShaderSource>& own, std::vector<std::pair<std::string, GLenum>>& shared);
    // [Viewer Lab] A digest of the program's GLSL in attach order, which LLGLSLShader::hash() includes: each own
    // unit's type and text, then each shared unit's type and digest; a missing unit gives its path
    LLUUID programSourceHash(const std::vector<ShaderSource>& own, const std::vector<std::pair<std::string, GLenum>>& shared) const;
    // [Viewer Lab] The render interface's description of the program: one StageSource per stage that
    // occurs, vertex, fragment and geometry, with its own units in mShaderFiles order and then its shared units in the
    // walk's order. attributeMask is 0, known only after the link. False for a unit with no stage or no text.
    bool describeProgram(const LLGLSLShader* shader, const std::vector<ShaderSource>& own,
                         const std::vector<std::pair<std::string, GLenum>>& shared, LL::RHI::ProgramDesc& desc);

    // Implemented in the application to actually point to the shader directory.
    virtual std::string getShaderDirPrefix(void) = 0; // Pure Virtual

    // Implemented in the application to actually update out of date uniforms for a particular shader
    virtual void updateShaderUniforms(LLGLSLShader * shader) = 0; // Pure Virtual

    void initShaderCache(bool enabled, const LLUUID& old_cache_version, const LLUUID& current_cache_version, bool second_instance);
    void clearShaderCache();
    void persistShaderCacheMetadata();

    bool loadCachedProgramBinary(LLGLSLShader* shader);
    bool saveCachedProgramBinary(LLGLSLShader* shader);

    // [Viewer Lab] Shader manifest (behind the viewer's --shadermanifest): with a folder set, every
    // shader object compiled from then on is recorded with its stage, the shader class asked for
    // and the class its file was found in, and its assembled source (after the defines) goes to
    // <folder>/sources/<hash>.glsl; every program is recorded with the objects attached to it, in
    // attach order. writeManifest() writes the programs to <folder>/<name>.json, with `context`
    // from the application. setManifestFolder() starts the records of a load anew (the application
    // calls it before each load); an empty folder turns it off.
    void setManifestFolder(const std::string& folder);
    bool writeManifest(const std::string& name, const LLSD& context);

public:
    // Map of shader names to compiled
    std::map<std::string, GLuint> mVertexShaderObjects;
    std::map<std::string, GLuint> mFragmentShaderObjects;

    //global (reserved slot) shader parameters
    std::vector<std::string> mReservedAttribs;

    std::vector<std::string> mReservedUniforms;

    struct ProgramBinaryData
    {
        GLsizei mBinaryLength = 0;
        GLenum mBinaryFormat = 0;
        F32 mLastUsedTime = 0.0;
    };
    std::map<LLUUID, ProgramBinaryData> mShaderBinaryCache;
    LLUUID mShaderCacheVersion;
    bool mShaderCacheEnabled = false;
    std::string mShaderCacheDir;

protected:

    // our parameter manager singleton instance
    static LLShaderMgr * sInstance;

private:
    // [Viewer Lab] Shader manifest
    struct ManifestObject
    {
        std::string mFile;
        GLenum mType = 0;
        S32 mRequestedClass = 0; ///< the class asked for before any fall-back
        S32 mCompiledClass = 0;  ///< the class it compiled at, lower after a failed compile
        S32 mUsedClass = 0;      ///< the class its file was found in
        std::string mSource;     ///< hash of the assembled source; empty when there is no record
    };
    struct ManifestProgram
    {
        std::vector<ManifestObject> mAttached;
        U32 mAttempts = 0;    ///< link attempts in this load; more than one after a failed link
        S32 mFirstLevel = 0;  ///< the program's shader level once its own files compiled at the first attempt
    };
    void recordCompiled(const std::string& filename, GLenum type, S32 compiled_class, S32 used_class,
                        U32 shader_code_count, GLchar** shader_code_text);
    bool attachVertexObject(LLGLSLShader* shader, const std::string& object_path);
    bool attachFragmentObject(LLGLSLShader* shader, const std::string& object_path);
    void recordAttached(LLGLSLShader* shader, const std::string& object_path, GLenum type);

    std::string mManifestFolder;
    std::map<std::pair<std::string, GLenum>, ManifestObject> mManifestCompiled; ///< the last object from each file and stage
    std::map<const LLGLSLShader*, ManifestProgram> mManifestAttached;
    S32 mManifestDepth = 0;            ///< loadShaderFile's recursion depth
    S32 mManifestRequestedClass = 0;   ///< the class the outermost loadShaderFile was asked for
    /// describeProgram's stages of each program, with their units' digests in attach order
    std::map<const LLGLSLShader*, std::vector<std::pair<LL::RHI::ShaderStage, std::vector<std::string>>>> mManifestStages;
    /// writeManifest's check of mManifestStages against what GL compiled and attached
    void checkManifestStages();

    // [Viewer Lab]
    std::map<std::pair<std::string, GLenum>, ObjectSource> mObjectSources;        ///< beside the GL maps; loadShaderFile writes both
    std::vector<std::pair<std::string, GLenum>>*           mCollecting = nullptr; ///< assembleProgram's walk: the wrappers collect

}; //LLShaderMgr

#endif
