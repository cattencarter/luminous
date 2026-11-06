/**
 * @file luminousmetal.h
 * @brief Photo mode on Metal: one switch per effect, the passes and the table of native passes.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
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

#ifndef LUMINOUS_METAL_H
#define LUMINOUS_METAL_H

#include "stdtypes.h"

#include <array>

class LLRenderTarget;

namespace LL::RHI
{
class Texture;
class TransientPool;
}

// Photo mode on the Metal renderer. With OpenGL nothing here changes anything: every effect is on
// (as each effect's own settings say), and Luminous's own passes draw as they always have.
//
// On Metal there are two kinds of effect:
// - hosted: Luminous's code inside Firestorm's own programs (soft shadows in shadowUtil, GTAO in
//   aoUtil, contact shadows in the sun and spot light passes, AO on lamps, enhanced reflections,
//   the Look, grain, Worn photo and the light leak in the tonemap and sharpening passes). These are
//   translated to Metal with the rest of Firestorm's programs and are on as their setting says.
// - own passes: everything Luminous draws with programs of its own. These have no translated path
//   on Metal: such an effect is on only when its group has registered a native function for every
//   one of its passes (and its native library is ready). Until then it is off, and Firestorm's
//   stand-in, where there is one, shows instead.
namespace LuminousMetal
{
/// Photo mode's effects, one switch each on Metal (the setting LuminousMetal<name>). The groups that draw their
/// passes are listed by file at the register functions below.
enum class Effect : U8
{
    SunShafts,
    VolumetricLight,
    ProjectorBeams,
    SoftShadows,
    ContactShadows,
    GTAO,
    AOLights,
    Reflections,
    DepthOfField,        // Enhance, photo mode's own blur
    Bloom,
    Clarity,
    Halation,
    Anamorphic,
    LensFlare,
    ChromaticAberration,
    Distortion,
    Glitch,
    Vignette,
    Look,
    Grain,
    Worn,
    LightLeak,
    PictureFrame,
    Upscale,             // the smaller live view scaled up to the window
    AlphaSort,
    Accumulation,        // a render's frames averaged as light
    HDRSnapshot,         // renders, read back as light
    Count
};

/// Luminous's own passes, in the order a frame draws them.
enum class Pass : U8
{
    HalfGTAO,            // GTAO at half size after the sun pass, and its upsample into the light map
    SunShaftMask,
    SunShaftSoften,
    VolumetricLight,
    ProjectorBeams,
    SunShafts,
    Vignette,
    DoFTiles,            // the depth of field's tiles (the circle of confusion before them is Firestorm's cofF)
    DoFGather,
    BloomChain,          // the chain clarity, halation and bloom share: no hook of its own, its consumers ask, and it is
                         // drawn when any of them is on
    Clarity,
    Halation,
    Bloom,
    Anamorphic,
    LensFlare,
    ChromaticAberration,
    Distortion,
    Glitch,
    AverageFrames,
    HDRCapture,
    PictureFrame,
    Upscale,
    Count
};

static_assert(size_t(Pass::Count) <= 64, "PassContext::drawnThisFrame has a bit per pass");

/// What a native pass reads and writes, given by the translated pass's hook: the targets that pass
/// would use. A pass that draws the lens frame anew draws into other and sets swapped, as the
/// translated lens passes ping-pong between the two.
struct PassTargets
{
    LLRenderTarget* frame = nullptr; ///< the HDR frame (the source buffer for PictureFrame and Upscale)
    LLRenderTarget* other = nullptr; ///< the lens chain's other target; PictureFrame's destination
    LLRenderTarget* cof = nullptr;   ///< DoF: the circle-of-confusion pass's output
    LLRenderTarget* light = nullptr; ///< HalfGTAO: the deferred light target whose green holds the AO
    F32 maxCoF = 0.f;                ///< DoF: the largest circle of confusion in pixels
    bool swapped = false;            ///< out: the frame now lives in other
};

/// The frame's state for native passes, one per frame (display()), reset by beginFrame().
struct PassContext
{
    U64 frame = 0;              ///< counts beginFrame() calls; a render's frames each count
    U64 drawnThisFrame = 0;     ///< bit per Pass: drawn already, by its own function or a fused one
    LL::RHI::TransientPool* pool = nullptr; ///< the frame's transient textures; beginFrame() clears it, so a group
                                            ///< that uses one sets it again each frame
    LLRenderTarget* depth = nullptr;        ///< the frame's depth (gPipeline.mRT->deferredScreen)
    LLRenderTarget* exposure = nullptr;     ///< the metered exposure (gPipeline.mExposureMap)
};

/// A native pass: true when it drew. It may also mark other passes it drew fused with it in
/// context.drawnThisFrame, so their own hooks skip them.
using PassFn = bool (*)(PassContext& context, PassTargets& targets);

/// The native functions by pass, filled once by the groups' register functions.
struct PassTable
{
    std::array<PassFn, size_t(Pass::Count)> fn{};
};

/// The groups' register functions, each in its group's own file.
void registerTone(PassTable& table);          // luminousmetaltone.cpp
void registerLens(PassTable& table);          // luminousmetallens.cpp
void registerDepthOfField(PassTable& table);  // luminousmetaldof.cpp
void registerShadows(PassTable& table);       // luminousmetalshadow.cpp
void registerScreenSpace(PassTable& table);   // luminousmetalscreen.cpp
void registerAirLight(PassTable& table);      // luminousmetalair.cpp
void registerRender(PassTable& table);        // luminousmetalrender.cpp

/// The groups' textures sized to the frame, let go after a render, each in its group's own file. Each is made
/// again at the size it is next used at, so one drawn live comes back at the live view's size and one kept for
/// renders holds nothing until the next render. The textures go through the device's deferred release, so
/// frames still in flight keep them until the GPU is done. The render group lets go of its own at
/// LuminousMetalRender::endRender, and the tone group has none.
void releaseLensTargets();           // luminousmetallens.cpp
void releaseDepthOfFieldTargets();   // luminousmetaldof.cpp
void releaseScreenSpaceTargets();    // luminousmetalscreen.cpp
void releaseAirLightTargets();       // luminousmetalair.cpp

/// After a render: every group's frame-sized textures let go (the functions above). Nothing on OpenGL.
void releaseRenderSizedTargets();

/// Main thread, at a present that does not scale the live view up (it is drawn at the window's size): MetalFX's
/// scaler and its frame, if made, are let go; the next frame that scales makes them again.
void liveViewNotScaled();          // luminousmetaltone.cpp

// MetalFX temporal (luminousmetaltone.cpp), on Metal only; nothing on OpenGL.
/// Metal runs and its device has MetalFX's temporal scaler; false on OpenGL.
bool temporalSupported();
/// The largest scale each way the temporal scaler takes on this device (Apple's range: 3 on Apple silicon); 0 without
/// the scaler.
F32 temporalMaxScale();
/// Main thread, from beginFrame: whether this frame is drawn for the temporal scaler (its scaler for the live view's
/// sizes was ready at the last present), and this frame's jitter.
void beginTemporalFrame(U64 frame);
/// This frame is drawn for the temporal scaler: the main camera jittered, SMAA and FXAA left out, and what is fixed to
/// the screen (grain, live lens distortion, Glitch, the light leak, Worn photo), which its present draws on the scaled
/// frame.
bool temporalFrame();
/// The temporal scaler can scale the live view as it is now: it was not refused for the world view's size. Found once
/// a frame, in beginTemporalFrame; true before the first. LuminousPhotoMode::chosenLiveScale and fxaaLeftOut ask it, so that
/// the live view keeps the full size, FXAA running, wherever the scaler would scale but cannot.
bool temporalUsable();
/// The temporal scaler was refused for the world view's size as it is now (it could not be made, or would not scale
/// a frame): the live view is at full size until the window's size or the settings change.
bool temporalRefused();
/// The main view's jitter this frame in clip units, for LLViewerCamera::setPerspective; false without, and false
/// once the present was drawn (temporalAfterPresent), until the next frame begins.
bool liveJitter(F32& ndc_x, F32& ndc_y);
/// While the main view's projection carries this frame's jitter (set by setPerspective, not yet taken off after the
/// present, and the main targets drawn): true, with the jitter in clip units, so that what is fitted to the camera
/// (the sun's shadow cascades) can take it off; false, and 0, otherwise.
bool liveJitterApplied(F32& ndc_x, F32& ndc_y);
/// The reactive mask's stages round the alpha drawn into the main view's HDR frame: Before copies the frame, After
/// marks how much the alpha drawn since changed it. Nothing unless this frame is drawn for the temporal scaler.
void temporalAlphaBefore(LLRenderTarget* screen);
void temporalAlphaAfter(LLRenderTarget* screen);
/// After the present: the projection set back without the jitter for what is drawn after the world, and no jitter
/// for a perspective set up after it this frame.
void temporalAfterPresent();
/// What is fixed to the screen, in a frame drawn for the temporal scaler: left out of the frame, which the scaler would
/// smear as the camera's jitter moves under it, and drawn over the window's world view after the present, at the
/// window's size (drawOverlays). Main thread, from the post-processing pass that would draw it: true when this frame
/// draws it after the present (the pass then draws nothing); false in any other frame, on OpenGL, and until the
/// overlay's pipeline has compiled, when the pass draws it into the frame as before.
/// The picture frame of a shape chosen on the Render tab, the picture's shape (width over height) given.
bool overlayPictureFrame(F32 picture_aspect);
/// Firestorm's capture frame for a snapshot floater: its rect as 0 to 1 of the world view (x0, y0, x1, y1, GL's rows),
/// its border's colour (three floats) and its width in the window's pixels.
bool overlayCaptureFrame(F32 x0, F32 y0, F32 x1, F32 y1, const F32* colour, F32 border);
/// Main thread, right after the present into the window's back buffer, whichever present drew it: what this frame
/// left out for the overlays (above), drawn in the world view's viewport -- Firestorm's capture frame, then the
/// picture frame. No depth is written. Nothing in any other frame and on OpenGL.
void drawOverlays();
/// Second Life's depth of field and the glow map in a frame drawn for the temporal scaler are settled: made where the
/// camera without the jitter saw the frame (the glow from four reads across each of its texels' part of the frame) and
/// averaged with the frames before, moved by the camera (LuminousMetalFXTemporalSettleDoF / ...SettleGlow: about how
/// many frames; 0 off). Settles: true with this frame's jitter in the live view's texture coordinates (GL's rows, up),
/// added where the pass reads the frame and taken off where its result is read back; false, and 0, otherwise.
bool temporalSettlesGlow(F32& u, F32& v);
bool temporalSettlesDoF(F32& u, F32& v);
/// The average, before the layer is read back: the target holding it, or the one given when the layer is not settled.
/// blur is Second Life's depth of field's gather, its width x height texels in the lower left.
LLRenderTarget* temporalSettleDoF(LLRenderTarget* blur, U32 width, U32 height);
LLRenderTarget* temporalSettleGlow(LLRenderTarget* glow);
/// Photo mode's layers settled the same way in a frame drawn for the temporal scaler, each made where the camera without
/// the jitter saw the frame: Air, the air's sum (the sun shafts, their mask from the frame read there, and the volumetric
/// light and beams when drawn at half size) before it is added to the frame (LuminousMetalFXTemporalSettleAir); FlareLight and
/// FlareSun, the lens flare's light taken from the frame and how much of the sun is seen (...SettleFlare); Enhance, photo
/// mode's depth of field, the samples' part of each gather and their weight in alpha, before it is blended into the frame
/// (...SettleEnhance). What the jitter changes in them, a gap in the leaves opening and closing, is as large as a real
/// change, so they are taken whole only where the camera moved; a change reaches them over the frames averaged.
/// Enhance is also taken whole where its weight leaves what the frame holds round a place, as where something in focus
/// moves over the blur.
enum class SettledLayer : U8
{
    Air,
    FlareLight,
    FlareSun,
    Enhance,
    Count
};
/// The layer is settled this frame: true with the jitter in the live view's texture coordinates (GL's rows, up), added
/// where the layer reads the frame and taken off where it is added back; false, and 0, otherwise.
bool temporalSettles(SettledLayer layer, F32& u, F32& v);
/// The average, before the layer is read: the texture holding it (RGBA16F, texture's size), or texture when it is not
/// settled. Its texels width x height in the lower left are the layer's. Outside a render or compute pass.
LL::RHI::Texture* temporalSettle(SettledLayer layer, LL::RHI::Texture* texture, U32 width, U32 height);
/// Main thread, as the pipeline lets its screen-sized targets go: the settle's averages go too, made again when next
/// needed.
void releaseSettledTargets();
/// The next temporal frame drops the scaler's history (a style's or Frame me's camera, a saved shot).
void resetTemporalHistory();
/// Main thread, as the main view's frame is copied into the scene map that screen-space reflections read in the
/// next frame: the jitter it was drawn with is kept.
void temporalSceneMapCopied();
/// For screen-space reflections, which project a point into the scene map through this frame's projection: the
/// scene map's jitter minus this frame's, in clip units (luminous_ssr_jitter). 0 on OpenGL, outside the main view,
/// and while neither frame was drawn with the jitter.
void ssrJitterOffset(F32& ndc_x, F32& ndc_y);

/// Main thread, Metal, after the scene map that screen-space reflections read was drawn: with wanted (photo mode's
/// reflections are drawn), its mip chain is made from it, level 0 a copy and the levels below by compute; without,
/// or when it cannot be made yet, the chain is out of date. luminousmetalscreen.cpp.
void sceneMapDrawn(LLRenderTarget* scene, bool wanted);

/// Main thread, Metal: the mip chain made from the scene map's latest drawing, of the scene map's size; null when
/// there is none, and on OpenGL. Photo mode's reflections read it in the scene map's place.
LL::RHI::Texture* sceneMipChain(LLRenderTarget* scene);

/// The Metal renderer runs; false on OpenGL.
bool metal();

/// Luminous's own translated programs and passes are built and drawn: OpenGL only.
inline bool translatedPasses() { return !metal(); }

/// LuminousMetalPhotoModeDev: photo mode runs on Metal at all. An internal developer switch, not in
/// the interface. True on OpenGL, where photo mode is always on.
bool photoModeOnMetal();

/// OpenGL: true, always (each effect's own settings decide, as before). Metal: photo mode runs, the
/// effect's LuminousMetal<Effect> setting is not 0, and the effect is hosted in Firestorm's
/// programs or every one of its own passes is native and ready. Alpha sorting is off on Metal unless
/// its setting is set (see the table in luminousmetal.cpp).
bool on(Effect effect);

/// Metal, and every one of the effect's own passes is native and ready (false for an effect with
/// no passes of its own).
bool native(Effect effect);

/// OpenGL: true (the translated pass). Metal: a native function is registered for the pass and its
/// group has said the pass is ready.
bool passReady(Pass pass);

/// A group says whether a pass's native library has compiled: native libraries compile in the
/// background, and until a group says so its passes are not drawn. Registered passes start not ready.
void setReady(Pass pass, bool ready);

/// The hook at the top of each of Luminous's own translated passes. False on OpenGL: the
/// translated pass runs as before. On Metal always true, meaning "skip the translated pass": the
/// native function drew it, a fused native pass drew it already this frame, or it is not drawn at
/// all (its effect is off, or no native function is registered and ready), since Luminous's own
/// passes have no translated path on Metal.
bool replaces(Pass pass, PassTargets& targets);
inline bool replaces(Pass pass)
{
    PassTargets targets;
    return replaces(pass, targets);
}

/// The native function of a pass, when it is ready: true only when it drew (or a fused pass drew
/// it this frame). False on OpenGL. For hooks that must know the pass was drawn.
bool drawNative(Pass pass, PassTargets& targets);

/// The frame's state, reset at the start of each frame.
PassContext& context();

/// Main thread, at the start of each frame (display()): a new PassContext. Nothing on OpenGL.
void beginFrame();

/// The effect a pass belongs to. BloomChain gives Bloom, but is drawn when bloom, clarity or halation is on.
Effect effectOf(Pass pass);
}

#endif // LUMINOUS_METAL_H
