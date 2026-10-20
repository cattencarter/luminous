/**
 * @file luminousphotomode.h
 * @brief Photo mode: the one switch for everything Luminous adds to rendering.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
 * The idea of volumetric light by shadow-map marching is from the Black Dragon
 * Viewer by NiranV Dean; sun shafts follow Kenny Mitchell, GPU Gems 3 (2007).
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
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */
#ifndef LUMINOUS_PHOTO_MODE_H
#define LUMINOUS_PHOTO_MODE_H

#include "stdtypes.h"
#include "llsd.h"

#include <string>
#include <functional>
#include <vector>

class LLGLSLShader;
class LLRenderTarget;

/**
 * Luminous's rendering for pictures: light, lens and colour effects added to
 * Firestorm's. **Always on**, and kept quick: the costly effects are drawn
 * only in the rendered picture unless the user wants them live (each
 * effect's button), and the live view shows Firestorm's own in their place.
 *
 * Each feature has its own settings. The pipeline asks here rather than
 * reading them, so that what draws when lives in one place.
 */
namespace LuminousPhotoMode
{
    /// Always true; asked so that photo mode's hooks in Firestorm's files
    /// stay marked as such.
    bool isOn();

    /// A feature is wanted: photo mode is on and so is the feature. Each
    /// decides whether its shaders are built (and buffers allocated), so the
    /// answer may only change through a reload -- which the listeners on these
    /// settings do.
    bool volumetricLight();
    bool projectorBeams();
    bool sunShafts();
    bool vignette();

    /// The costly effects can be kept for renders (LuminousPhoto*RenderOnly):
    /// left out of the live preview, which shows Firestorm's own in their
    /// place where it has one, and drawn in every picture -- Luminous's
    /// renders, Firestorm's snapshots. Each ...Now() says whether the effect is drawn in this
    /// frame; its shaders are built while it is wanted at all, so the switch
    /// between preview and picture needs no reload. Depth of field's is
    /// drawsDepthOfField().
    bool volumetricLightNow();
    bool projectorBeamsNow();
    bool softShadowsNow();
    bool ambientOcclusionNow();
    /// How much Firestorm's softening pass blurs ambient occlusion while
    /// photo mode's is on (LuminousPhotoAOSoften): the y of
    /// RenderShadowGaussian, set with GTAO's other settings.
    F32 aoSoften();
    bool reflectionsNow();
    bool bloomNow();
    /// A picture with light the live preview did not have -- an effect kept
    /// for renders, drawn before the exposure is measured -- meters it afresh
    /// instead of easing on from the preview's (LLPipeline::generateExposure
    /// without history). In a still scene that is where the easing would end.
    bool meterAfresh();

    /// Called from LLPipeline::allocateScreenBufferInternal and
    /// releaseScreenBuffers, beside Firestorm's own post-process buffers.
    /// Alpha sorting, in a render's frame of several: (frame + 0.5) / frames,
    /// each worn see-through pixel drawn solid where its alpha is above a
    /// threshold that steps evenly through the frames from one offset per
    /// pixel, so their average is its alpha and every layer sorts by depth;
    /// -1 otherwise (LLDrawPoolAlpha, the alpha shaders' rigged variants).
    F32 alphaSortStep();
    /// Alpha sorting in the live view (and a single-frame picture): worn
    /// see-through things drawn in their place by distance among the rest,
    /// instead of before them all.
    bool alphaSortLive();
    /// Alpha sorting drawn now: worn alpha's depth only where it covers
    /// half, blended materials cut below their minimum alpha, and worn mesh's
    /// glare only as far as its texture covers. Off -- and the live view when
    /// it is kept for renders -- is Second Life's drawing exactly, to
    /// compare with.
    bool alphaSortFix();
    /// Faint worn layers drawn as Second Life draws them, writing their
    /// depth (LuminousPhotoAlphaSortFaintSL).
    bool alphaSortFaintAsSecondLife();
    void allocateBuffers(U32 resX, U32 resY);
    void releaseBuffers();
    /// A 1 x 1 shadow map read in place of an empty projector slot's: the
    /// sun pass reads both slots' maps always, and Apple's GL gives a map
    /// its memory once it is read -- 250 MB at the peak of a render at
    /// twice a 2560-wide window, no projector casting shadows.
    void allocateNoSpotShadow();
    LLRenderTarget* noSpotShadow(); ///< null until allocated
    /// At start-up (LLAppViewer::init), once per install: a word on a Mac
    /// with less than 16 GB about what Luminous needs.
    void warnIfLowMemory();
    /// After a render (LuminousHDRSnapshot::takeAndSave): the effects'
    /// targets, sized to the picture, given back.
    void releaseRenderSizedTargets();

    /// Adds photo mode's light to the HDR frame, before exposure and
    /// tonemapping: volumetric light, projector beams, then sun shafts. Called from
    /// LLPipeline::renderFinalize, only while isOn().
    void render(LLRenderTarget* hdr_screen);

    /// Darkens the HDR frame's corners, after the exposure has been measured
    /// and before tonemapping. Called from LLPipeline::renderFinalize, only
    /// while isOn().
    void renderVignette(LLRenderTarget* hdr_screen);

    /// Photo mode's lens, after depth of field and before tonemapping:
    /// clarity, halation, bloom, streaks, flare, chromatic aberration and
    /// distortion. scratch is a frame-sized HDR target free at that point;
    /// the effects that draw the whole frame anew go back and forth between
    /// the two. Returns the one that holds the frame at the end.
    bool bloom();
    bool chromaticAberration();
    LLRenderTarget* renderLens(LLRenderTarget* hdr_screen, LLRenderTarget* scratch);
    /// Live, in a shape chosen on the Render tab: what a render will keep,
    /// the rest dimmed, drawn from src into dst. False (nothing drawn) in a
    /// snapshot, with the interface hidden, or in the window's own shape.
    bool renderPictureFrame(LLRenderTarget* src, LLRenderTarget* dst);

    /// Photo mode's depth of field, LuminousPhotoDoF: Firestorm's, with its
    /// focus and its lens settings, but blurred at full resolution in HDR,
    /// before tonemapping. depthOfField() says whether its shader is built;
    /// drawsDepthOfField() whether it replaces Firestorm's blur this frame
    /// (it falls back to Firestorm's if its shader did not build).
    bool depthOfField();
    bool drawsDepthOfField();
    /// Whether depth of field (RenderDepthOfField) is drawn in this frame:
    /// false in the live view while it is kept for renders
    /// (LuminousPhotoDepthOfFieldRenderOnly). LLPipeline::renderDoF still
    /// follows the focus then, so a render is focused as the live view is.
    bool depthOfFieldNow();

    /// The target Firestorm's circle-of-confusion pass draws into for the
    /// blur below, at the frame's size, with mip levels its flush makes.
    LLRenderTarget* depthOfFieldSource(U32 width, U32 height);
    /// The blur itself, called from LLPipeline::renderDoF after Firestorm's
    /// circle-of-confusion pass has written colour and blur radius to cof
    /// (depthOfFieldSource(), or any target, which is then copied there).
    /// Writes hdr_screen's colour; max_cof is the largest radius, in pixels.
    void renderDepthOfField(LLRenderTarget* cof, LLRenderTarget* hdr_screen, F32 max_cof);

    /// Photo mode's soft shadows, LuminousPhotoSoftShadows: sharp at contact,
    /// softening with the distance to what casts them (shadowUtil.glsl,
    /// LUMINOUS_SOFT_SHADOWS). The sun's shadow maps are then bound without
    /// hardware comparison, so this decides both the shaders' define and
    /// LLPipeline::allocateShadowBuffer's compare mode; its listeners reload
    /// both.
    bool softShadows();
    /// Called from LLPipeline::bindDeferredShader, beside the shadow bias.
    void bindShadowUniforms(LLGLSLShader& shader);
    /// Shadow strength (LuminousPhotoShadowStrength): how much light the
    /// sun's, moon's and projectors' shadow maps take, live and in pictures,
    /// for every shader that reads them -- from LLPipeline::bindShadowMaps,
    /// which the deferred, alpha and water passes all go through.
    void bindShadowStrength(LLGLSLShader& shader);

    /// Contact shadows (LuminousPhotoContactShadows): short, sharp shadows
    /// where things touch, traced on screen toward the light in Firestorm's
    /// sun-shadow pass (sunLightF / sunLightSSAOF) and projector pass
    /// (spotLightF), and laid over the shadow maps' shadow there. Their uniforms are bound with every
    /// deferred shader; luminous_contact_length 0 leaves them out.
    bool contactShadows();
    bool contactShadowsNow();
    void bindContactShadowUniforms(LLGLSLShader& shader);

    /// Accumulated snapshots: while one is taken, each frame's camera is
    /// shifted by a fraction of a pixel (LLViewerCamera::setPerspective asks
    /// here) and the soft shadows' and AO's noise is seeded afresh, so that
    /// averaging the frames smooths edges and noise alike.
    /// Photo mode's sun shadow maps, square, LuminousPhotoShadowMapSize on a
    /// side; 0 when Firestorm's sizing applies (photo mode off, or the setting 0).
    U32 shadowMapSize();
    /// While a Luminous snapshot renders: the shadow maps at their chosen size
    /// (the snapshot reallocates them).
    /// size: the render's own choice of shadow map size (smaller where
    /// memory is short), 0 for LuminousPhotoShadowMapSize.
    void setSnapshotShadows(bool on, U32 size = 0);
    /// The largest side for the two projector shadow maps
    /// (LLPipeline::allocateShadowBuffer).
    U32 spotShadowMapLimit();
    /// A render of more than one frame is being drawn: its jittered frames
    /// smooth the edges, so the anti-aliasing pass and its buffers are left
    /// out (LLPipeline::allocateScreenBufferInternal).
    bool averagingFrames();
    /// Film grain after Firestorm's sharpening: renderFinalize says whether
    /// this frame's sharpening pass follows its tonemap pass. If so, the Look
    /// leaves grain out and bindGrainUniforms gives it to the sharpening.
    void setGrainAfterSharpening(bool on);
    void bindGrainUniforms(LLGLSLShader& shader);
    /// A dither of one step where the picture becomes eight bits (the tonemap
    /// or sharpening pass writing the finished picture); none for finer.
    void bindDither(LLGLSLShader& shader, bool eight_bit);
    /// Low quality in the live view, for the effects where it saves
    /// (LuminousPhotoLowQualityLive*): volumetric light (and sun shafts),
    /// projector beams and GTAO at half size, and Firestorm's shadow maps;
    /// never in a picture.
    enum class LiveQuality { Volumetric, Shadows, Beams, AO };
    constexpr LiveQuality LIVE_QUALITIES[] = { LiveQuality::Volumetric, LiveQuality::Shadows, LiveQuality::Beams, LiveQuality::AO };
    const char* lowQualityLiveSetting(LiveQuality effect);
    /// Whether this frame draws the effect at full quality: always in a
    /// picture (any of Firestorm's snapshots too), live unless its low
    /// quality is ticked.
    bool fullQuality(LiveQuality effect);

    /// Photo mode's live view (LuminousPhotoLiveScale): while a shot is set
    /// up, the 3D frame renders at this fraction of the window's pixels each
    /// way and is scaled up to it (upscaling() then), the interface drawn
    /// sharp over it. 0 in the setting is automatic: one over the display's
    /// backing scale, so a Retina display renders as many pixels as a standard
    /// one of its size. 1 -- full -- as Luminous ships, when photo mode is
    /// off, with FXAA, and while a FullResolution is held. LLPipeline sizes the main
    /// targets by it (never the reflection probes' or mirrors').
    F32 liveRenderScale();
    /// The scale the setting comes to, photo mode on or off (for the window).
    F32 chosenLiveScale();
    /// The present pass scales a smaller frame up to the window.
    bool upscaling();
    /// Held while a snapshot renders, so that it renders at full resolution:
    /// the first hold puts the main targets back to full size (and asks for
    /// one warm-up frame, takeWarmUp, so that reflections have a full-size
    /// frame to read); the last one lets the live scale return. `wanted`
    /// false holds nothing (a preview thumbnail, never saved), and with photo
    /// mode off a hold does nothing at all.
    class FullResolution
    {
    public:
        explicit FullResolution(bool wanted = true);
        ~FullResolution();
        FullResolution(const FullResolution&) = delete;
        FullResolution& operator=(const FullResolution&) = delete;
    private:
        bool mHeld;
    };
    /// True once after a hold has put the targets back to full size, or when
    /// the caller says it reallocated them (in photo mode, for the first frame
    /// of an accumulated snapshot): the caller draws one frame, unread, before
    /// the one it keeps, so that screen-space reflections have a frame to
    /// read. warmingUp() is true while it does, so that no snapshot takes it.
    bool takeWarmUp(bool reallocated);
    void setWarmingUp(bool on);
    bool warmingUp();

    /// Held round all the frames of one of Luminous's renders. A render
    /// larger than the window makes rawSnapshot size the targets up to the
    /// picture and back down again; under the hold they stay at the
    /// picture's size from frame to frame (keepSnapshotTargets, asked by
    /// rawSnapshot instead of sizing them down), and the hold's end has the
    /// next frame size them for the window. Resizing every frame would empty
    /// the scene map that reflections read, and leave a full set of targets
    /// behind each frame until LLImageGL::flushDeletedTextures.
    class KeepSnapshotTargets
    {
    public:
        KeepSnapshotTargets();
        ~KeepSnapshotTargets();
        KeepSnapshotTargets(const KeepSnapshotTargets&) = delete;
        KeepSnapshotTargets& operator=(const KeepSnapshotTargets&) = delete;
    };
    bool keepSnapshotTargets();
    bool keepingSnapshotTargets(); ///< held, without noting anything kept

    /// Held while settings are set together -- a style, a preset, a tab's
    /// reset -- so that the shaders are rebuilt once, as the hold ends, and
    /// not once for every setting that changes them (handleSetShaderChanged
    /// rebuilds all of them at once). Apple's OpenGL keeps about 1.5 MB of
    /// every rebuild, and without the hold twenty styles applied in turn
    /// rebuild them 28 times.
    class HoldShaderRebuilds
    {
    public:
        HoldShaderRebuilds();
        ~HoldShaderRebuilds();
        HoldShaderRebuilds(const HoldShaderRebuilds&) = delete;
        HoldShaderRebuilds& operator=(const HoldShaderRebuilds&) = delete;
    };
    /// True while held: the rebuild asked for is noted, for the hold's end.
    bool holdShaderRebuild();

    bool holdExposure(); ///< a frame after the first of an accumulated render: keep the exposure
    void setAccumulationFrame(S32 frame); ///< -1 when no snapshot is being accumulated
    void setAccumulationFrames(S32 frames); ///< how many the render has; 0 after it
    /// A render's margin (LuminousSnapshotMargin): the frame is drawn this
    /// much larger each way, with the field of view widened to match, and the
    /// picture cut from its middle -- so that what looks at its neighbours on
    /// screen (reflections, GTAO, contact shadows, depth of
    /// field, bloom) has something there at the picture's edges. 1 without.
    /// Depth of field takes its focal length, and the lens effects their
    /// corners, from the picture, not the larger frame.
    void setFrameMargin(F32 scale);
    F32 frameMarginScale();
    bool snapshotJitter(F32& x, F32& y);  ///< in pixels, -0.5 to 0.5

    /// Depth of field and soft shadows sampled across the frames of an
    /// accumulated render (LuminousSnapshotSampledDoF, ...SampledShadows):
    /// each frame looks through another point of the lens's aperture, its
    /// focus plane held, and draws the sun's shadows from another point of
    /// its disc, so the frames' average blurs and softens as a lens and the
    /// sun do -- through hair and leaves too, which a depth-buffer blur
    /// cannot. Each frame's own blur gives what the sampled aperture does not
    /// and a share to fill the gaps between the samples (lensShare, lensFill,
    /// cofF.glsl); the soft shadows' softness is scaled down the same way. renderDoF notes the focus and the aperture its circle of
    /// confusion implies (noteDepthOfField); setPerspective asks
    /// snapshotLens; generateSunShadow, jitterShadowCaster.
    void noteDepthOfField(F32 focus, F32 aperture); ///< metres: the distance in focus, the aperture's radius
    /// Second Life's depth of field runs on the finished picture, after the
    /// lens distortion has moved it: its circle-of-confusion pass reads each
    /// pixel's depth where the distortion took the pixel from (cofF.glsl).
    /// Lens blur comes before the distortion, so after_lens is false there.
    void bindDistortedDepth(LLGLSLShader& shader, bool after_lens);
    bool snapshotLens(F32& x, F32& y, F32& focus);  ///< this frame's point in the aperture, eye-space metres
    F32 lensShare(F32 aperture);                    ///< sampled aperture over the f-number's; 0 unless sampled
    F32 lensFill();                                 ///< of the sampled blur, what each frame's own keeps
    void jitterShadowCaster(LLVector3& caster_dir); ///< toward the sun or moon: moved across its disc

    /// Photo mode's ambient occlusion, LuminousPhotoAO: GTAO in place of
    /// Linden's (aoUtil.glsl, LUMINOUS_GTAO), where Firestorm computes AO --
    /// so it needs ambient occlusion on, as Firestorm's does.
    bool ambientOcclusion();
    /// Photo mode's screen-space reflections (LuminousPhotoSSR, with
    /// Firestorm's RenderScreenSpaceReflections on): screenSpaceReflUtil.glsl
    /// under LUMINOUS_SSR, a mipmapped scene map, and their uniforms.
    bool reflections();
    void bindSSRUniforms(LLGLSLShader& shader);
    /// Called from LLPipeline::bindDeferredShader, beside the shadow bias.
    void bindAOUniforms(LLGLSLShader& shader);
    /// The share of GTAO the deferred lamps and projectors take
    /// (`LuminousPhotoAOLights`), 0 unless GTAO is drawn in this frame.
    void bindAOOnLights(LLGLSLShader& shader);
    /// With low quality live for GTAO: GTAO drawn at half size and multiplied
    /// into light_target's ambient occlusion, which the sun pass then left at
    /// 1 (luminous_gtao 2). Called after the sun pass, before the softening.
    void renderHalfGTAO(LLRenderTarget* light_target);

    /// Photo mode's Look (tonemapUtilF.glsl, LUMINOUS_LOOK): exposure,
    /// contrast, saturation, white balance, black and white and sepia on the
    /// exposed scene-linear frame, and a choice of tonemapper -- Firestorm's
    /// two, AgX, AgX Punchy, Hable's filmic. At its defaults it changes
    /// nothing. Always on in photo mode; the define is set by isOn().
    bool look();
    /// Called from LLPipeline::tonemap, after Firestorm's own uniforms.
    void bindLookUniforms(LLGLSLShader& shader);
    /// The settings the Look tab's Reset button puts back.
    void resetLook();

    /// Colour lookup tables (.cube) for the Look's last step
    /// (LuminousPhotoLUT, by name; LuminousPhotoLUTStrength). Luminous's own
    /// are in app_settings/luminous_luts; the user's in
    /// user_settings/luminous_luts, where importLUT copies a chosen file.
    std::vector<std::string> lutNames(); ///< Luminous's first, then the user's
    bool importLUT(const std::string& path, std::string& name, std::string& error);

    /// Saved shots: the camera's place and aim (global, so a shot belongs to
    /// a place), the field of view, and depth of field's settings -- on or
    /// off, f-number, focal length, largest blur. The aim is the camera's focus,
    /// which is also where depth of field focuses. Per account, in
    /// luminous_shots.xml. Going to a shot more than 256 m from the avatar is
    /// refused: the camera cannot reach it.
    std::vector<std::string> shotNames();
    bool saveShot(const std::string& name);
    bool goToShot(const std::string& name, std::string& error);
    bool deleteShot(const std::string& name);

    /// Presets: everything set on the Lighting, Tone, Colour and Effects
    /// tabs under a name -- each effect's switch and whether it is kept for
    /// the picture, every slider, the LUT -- but not the camera, which
    /// saved shots keep, nor the Render and Settings tabs. One
    /// LLSD file each in user_settings/luminous_presets, to share by copying.
    /// A preset is complete: what it does not name goes back to its default.
    /// Luminous ships none (its styles do that job, with light and camera).
    std::vector<std::string> presetNames();
    bool applyPreset(const std::string& name);
    /// Luminous's environments (app_settings/luminous_environments): skies
    /// made for photographing people, each a sky's settings and the sun's
    /// height it was made for. Shown as the personal environment, on this
    /// screen only, as Personal Lighting does.
    std::vector<std::string> environmentNames();
    /// The user's own PBR skies in inventory known so far, as (name, Sky menu
    /// value) sorted by name; scanInventorySkies asks about the rest (each
    /// sky's asset fetched once) and calls `changed` as answers come.
    std::vector<std::pair<std::string, std::string>> inventoryPBRSkies();
    void scanInventorySkies(std::function<void()> changed);
    /// Luminous's environments as environment items in #Luminous/Environments,
    /// once per account (at login).
    void addEnvironmentsToInventory();
    /// Shows the environment of that name, its sun from the direction it
    /// came from before and at the height it was made for; "" the region's
    /// own sky again. LuminousEnvTemplate, ...EnvSunAzimuth and
    /// ...EnvSunElevation follow.
    bool applyEnvironment(const std::string& name);
    /// The sun moved to LuminousEnvSunAzimuth / ...Elevation: in the
    /// environment showing, or in the region's sky, frozen as Personal
    /// Lighting freezes it.
    void moveEnvironmentSun();
    /// While the region's own sky shows: the sun settings follow its sun.
    void followShownSun();
    /// As applyEnvironment, the sun where given instead, and the moon where
    /// given (kept as the sun moves, until another sky).
    bool applyEnvironmentWithSun(const std::string& name, const F32* sun_azimuth, const F32* sun_elevation,
                                 const F32* moon_azimuth = nullptr, const F32* moon_elevation = nullptr);
    /// Luminous's styles (app_settings/luminous_styles): looks of light,
    /// blur, volumetric light, colour, tone, sky, water and fog, each an
    /// environment, the sun placed round from behind the camera, a preset's
    /// settings (complete), and a framing of her.
    std::vector<std::string> styleNames();
    bool applyStyle(const std::string& name);
    /// The camera framed on her from her skeleton: [framing] face, half,
    /// low, tilted, behind or sidelow; [around] and [pitch] in degrees
    /// change it. view_direction, if given, gets where it looks.
    bool frameAvatar(const LLSD& camera, LLVector3* view_direction);
    // While the camera is the one a framing set: her eyes, in agent
    // coordinates, for depth of field to focus on.
    bool eyeFocus(LLVector3& focus_agent);
    /// True while only the sun of the sky showing is being moved: then
    /// LLEnvironment keeps the reflection probes, which catch up as they
    /// update, instead of emptying them all (emptied, they would turn the
    /// frame bright, then slowly darker as they fill again).
    bool keepingProbes();
    bool savePreset(const std::string& name);
    bool deletePreset(const std::string& name);

    /// An older Luminous's photo mode swapped in graphics settings of its
    /// own and kept the user's aside. Called from LLAppViewer::init before
    /// settings_to_globals: a session that ended with them aside gets them
    /// back, before anything reads them, and the swap's files go.
    void restoreGraphicsAfterRestart();
    /// Called from settings_setup_listeners: the window's commands (resets,
    /// presets, LUTs, shots).
    void initCommands();
    /// Called from settings_setup_listeners: connects LuminousAnimationSpeed,
    /// which sets every avatar's animation speed as Advanced > Animation
    /// Speed does, down to 0, which holds every pose still; and
    /// LuminousScenePaused.
    void initSceneTools();
    /// The scene paused (LuminousScenePaused, the pause button by Render
    /// image): Linden's FreezeTime -- objects no longer move on screen,
    /// whatever the server says, flexible prims, texture animations,
    /// spinning, trees and grass stand still -- and with it every
    /// animation, particles, water, clouds, stars and the day's course.
    /// The camera moves freely, and everything worked out from it (light,
    /// shadows, reflections) follows it.
    bool scenePaused();
    /// Seconds since start, standing still while the scene is paused: for
    /// water's waves and the stars, in place of the clock.
    F64 sceneSeconds();
}

#endif // LUMINOUS_PHOTO_MODE_H
