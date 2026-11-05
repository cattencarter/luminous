/**
 * @file luminousmetaltone.cpp
 * @brief Photo mode on Metal: tone and finish, the group's native passes.
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

#include "llviewerprecompiledheaders.h"

#include "luminousmetal.h"
#include "luminousmetalparams.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llnativeprogram.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "luminousbenchmark.h"
#include "luminousfloater.h"
#include "luminousoptimise.h"
#include "luminousphotomode.h"
#include "pipeline.h"

#include <array>
#include <cmath>
#include <deque>
#include <iterator>
#include <optional>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

extern bool gSnapshot;     // llviewerdisplay.cpp: a snapshot's frame
extern bool gCubeSnapshot; // pipeline.cpp: a reflection probe's or mirror's frame

// The tone and finish group's own passes on Metal:
// - PictureFrame: the picture's part of the finished live view, for a render in a shape of its own; the rest
//   dimmed and a thin line along the picture's edge, drawn into the next post-processing target.
// - Upscale: the present of a live view rendered smaller than the window, scaled up into the window's back
//   buffer, with the frame's depth for the interface drawn over it: by MetalFX's temporal scaler
//   (LuminousMetalFXTemporal) into a frame of the world view's size, copied texel for texel, or with Catmull-Rom.
// - MetalFX temporal's inputs: the jitter of the main camera, the reactive mask drawn round the alpha in the
//   deferred lighting, and the motion texture drawn at the present.
// - The overlays: in a frame drawn for MetalFX's temporal scaler, the picture frame and Firestorm's capture frame are
//   left out of the frame and drawn over the window's world view after the present, at the window's size
//   (drawOverlays).
// The Look, grain, Worn photo and the light leak are drawn inside Firestorm's tonemap and sharpening
// programs (hosted), translated to Metal with them; they are not drawn here, but for a live frame drawn for
// MetalFX temporal's scaler, which leaves out what is fixed to the screen: its present draws live lens distortion,
// the light leak, grain and Worn photo on the scaled frame (LuminousPhotoMode::fillTemporalScreen).
//
// The library compiles in the background once the viewer is in the world. Until it and its pipelines are
// ready the passes are not drawn: no frame is shown, and the live view stays at full size.
namespace LuminousMetal
{
    namespace
    {
        const char* const MANIFEST = "luminous/tone/programs.json";
        const char* const LIBRARY = "tone";
        const char* const VERTEX = "luminous_fullscreen_vs";
        const char* const FRAME_FRAGMENT = "luminous_picture_frame_fs";
        const char* const PRESENT_FRAGMENT = "luminous_present_upscale_fs";
        const char* const PRESENT_NO_DEPTH_FRAGMENT = "luminous_present_upscale_nodepth_fs";
        const char* const TEMPORAL_FRAGMENT = "luminous_present_temporal_fs";
        const char* const TEMPORAL_NO_DEPTH_FRAGMENT = "luminous_present_temporal_nodepth_fs";
        const char* const SCREEN_FRAGMENT = "luminous_present_screen_fs";
        const char* const SCREEN_NO_DEPTH_FRAGMENT = "luminous_present_screen_nodepth_fs";
        const char* const MOTION_FRAGMENT = "luminous_temporal_motion_fs";
        const char* const REACTIVE_FIRST_FRAGMENT = "luminous_temporal_reactive_first_fs";
        const char* const REACTIVE_MORE_FRAGMENT = "luminous_temporal_reactive_more_fs";
        const char* const REACTIVE_NONE_FRAGMENT = "luminous_temporal_reactive_none_fs";
        const char* const SETTLE_FRAGMENT = "luminous_temporal_settle_fs";
        const char* const HOLD_DIFF_FRAGMENT = "luminous_temporal_hold_diff_fs";
        const char* const OVERLAY_CAPTURE_FRAGMENT = "luminous_overlay_capture_frame_fs";
        const char* const OVERLAY_PICTURE_FRAGMENT = "luminous_overlay_picture_frame_fs";

        enum class LoadState : U8
        {
            Waiting,   // not asked for yet, or to be loaded again after the device let it go
            Compiling,
            Ready,
            Failed,    // the library did not compile: not tried again this session
        };

        LLNativeProgram sProgram;
        LoadState sState = LoadState::Waiting;
        bool sPolling = false;

        // What a frame drawn for MetalFX's temporal scaler leaves out because it is fixed to the screen, noted as the
        // post-processing would draw it and drawn over the window's world view after the present (drawOverlays).
        struct OverlayFrame
        {
            bool picture = false;      // the picture frame of a shape chosen on the Render tab
            F32 pictureAspect = 0.f;   // the picture's shape
            bool capture = false;      // Firestorm's capture frame for a snapshot floater
            F32 captureRect[4] = {};   // 0 to 1 of the world view: x0, y0, x1, y1
            F32 captureColour[3] = {}; // its border's colour
            F32 captureBorder = 0.f;   // its border's width in the window's pixels
        };
        OverlayFrame sOverlay;
        LL::RHI::Ref<LL::RHI::Texture> sOverlayCopy; // the world view as the present drew it, for the capture frame

        void setPassesReady(bool ready)
        {
            setReady(Pass::PictureFrame, ready);
            setReady(Pass::Upscale, ready);
            // The live view renders smaller only while Upscale is ready (liveRenderScale), so a change here
            // changes the main targets' size: they are made again at the next frame.
            static bool was_ready = false;
            if (ready != was_ready)
            {
                was_ready = ready;
                gResizeScreenTexture = true;
            }
        }

        LL::RHI::PipelineDesc pipelineDesc(LL::RHI::Format colour, LL::RHI::Format depth)
        {
            LL::RHI::PipelineDesc desc;
            desc.colorCount = 1;
            desc.colorFormats[0] = colour;
            desc.depthFormat = depth;
            return desc;
        }

        // The pipeline of a fragment function for these formats, once compiled; null while it compiles.
        LL::RHI::Pipeline* readyPipeline(const char* fragment, LL::RHI::Format colour, LL::RHI::Format depth)
        {
            LL::RHI::Pipeline* pipeline = sProgram.render(VERTEX, fragment, pipelineDesc(colour, depth));
            return pipeline && pipeline->status() == LL::RHI::CompileStatus::Ready ? pipeline : nullptr;
        }

        // An overlay over the window's back buffer: alpha blended, its alpha channel kept, no depth.
        LL::RHI::PipelineDesc overlayDesc(LL::RHI::Format colour)
        {
            LL::RHI::PipelineDesc desc = pipelineDesc(colour, LL::RHI::Format::Unknown);
            desc.blend.enabled = true;
            desc.blend.srcColor = desc.blend.srcAlpha = LL::RHI::BlendFactor::SourceAlpha;
            desc.blend.dstColor = desc.blend.dstAlpha = LL::RHI::BlendFactor::OneMinusSourceAlpha;
            desc.colorMask = U8(LL::RHI::COLOR_MASK_R | LL::RHI::COLOR_MASK_G | LL::RHI::COLOR_MASK_B);
            return desc;
        }

        LL::RHI::Pipeline* readyOverlayPipeline(const char* fragment, LL::RHI::Format colour)
        {
            LL::RHI::Pipeline* pipeline = sProgram.render(VERTEX, fragment, overlayDesc(colour));
            return pipeline && pipeline->status() == LL::RHI::CompileStatus::Ready ? pipeline : nullptr;
        }

        // The screen present, which draws what a frame drawn for the scaler leaves out (live lens distortion, the
        // light leak, film grain, Worn photo) where the scaler gives no frame for it, compiled for the window's back
        // buffer with its depth and without: no frame is drawn for the scaler before, or that frame would lack them.
        bool screenPresentReady()
        {
            LL::RHI::Device* device = LL::RHI::viewerDevice();
            LL::RHI::Surface* surface = device ? device->surface() : nullptr;
            LL::RHI::Texture* back = surface ? surface->backBuffer() : nullptr;
            LL::RHI::Texture* back_depth = surface ? surface->backBufferDepth() : nullptr;
            return back && back_depth
                && readyPipeline(SCREEN_FRAGMENT, back->desc().format, back_depth->desc().format)
                && readyPipeline(SCREEN_NO_DEPTH_FRAGMENT, back->desc().format, LL::RHI::Format::Unknown);
        }

        // The pipelines the passes will ask for, asked now so that they compile before the first draw:
        // the frame into an RGBA8 post-processing target, the present into the window's back buffer.
        void prepareUsualPipelines()
        {
            sProgram.render(VERTEX, FRAME_FRAGMENT, pipelineDesc(LL::RHI::Format::RGBA8Unorm, LL::RHI::Format::Unknown));
            LL::RHI::Device* device = LL::RHI::viewerDevice();
            LL::RHI::Surface* surface = device ? device->surface() : nullptr;
            LL::RHI::Texture* back = surface ? surface->backBuffer() : nullptr;
            LL::RHI::Texture* back_depth = surface ? surface->backBufferDepth() : nullptr;
            if (back && back_depth)
            {
                sProgram.render(VERTEX, PRESENT_FRAGMENT, pipelineDesc(back->desc().format, back_depth->desc().format));
                sProgram.render(VERTEX, TEMPORAL_FRAGMENT, pipelineDesc(back->desc().format, back_depth->desc().format));
                sProgram.render(VERTEX, OVERLAY_CAPTURE_FRAGMENT, overlayDesc(back->desc().format));
                sProgram.render(VERTEX, OVERLAY_PICTURE_FRAGMENT, overlayDesc(back->desc().format));
                sProgram.render(VERTEX, SCREEN_FRAGMENT, pipelineDesc(back->desc().format, back_depth->desc().format));
                sProgram.render(VERTEX, SCREEN_NO_DEPTH_FRAGMENT, pipelineDesc(back->desc().format, LL::RHI::Format::Unknown));
            }
            sProgram.render(VERTEX, MOTION_FRAGMENT, pipelineDesc(LL::RHI::Format::RG16Float, LL::RHI::Format::Unknown));
            sProgram.render(VERTEX, REACTIVE_FIRST_FRAGMENT, pipelineDesc(LL::RHI::Format::R8Unorm, LL::RHI::Format::Unknown));
            sProgram.render(VERTEX, REACTIVE_MORE_FRAGMENT, pipelineDesc(LL::RHI::Format::R8Unorm, LL::RHI::Format::Unknown));
            sProgram.render(VERTEX, REACTIVE_NONE_FRAGMENT, pipelineDesc(LL::RHI::Format::R8Unorm, LL::RHI::Format::Unknown));
            sProgram.render(VERTEX, HOLD_DIFF_FRAGMENT, pipelineDesc(LL::RHI::Format::R32Float, LL::RHI::Format::Unknown));
            sProgram.render(VERTEX, SETTLE_FRAGMENT, pipelineDesc(LL::RHI::Format::RGBA16Float, LL::RHI::Format::Unknown));
        }

        // A new size or format is made only once it has held for this many frames (Catmull-Rom meanwhile), so a
        // world view that changes size frame by frame -- the full-screen animation -- makes no scaler per frame.
        constexpr U32 METALFX_SETTLE_FRAMES = 8;

        void resetTemporal(); // MetalFX temporal's scaler, below

        // From the start again: MetalFX temporal's scaler and frames let go.
        void resetMetalFX()
        {
            resetTemporal();
            sOverlay = OverlayFrame();
            sOverlayCopy = nullptr;
        }


        // Idle, until the library is ready or has failed: loaded once the viewer is in the world.
        // True ends the polling.
        bool poll()
        {
            if (!metal())
            {
                sPolling = false;
                return true;
            }
            if (LLStartUp::getStartupState() < STATE_STARTED || !LL::RHI::viewerDevice())
            {
                return false;
            }
            switch (sState)
            {
            case LoadState::Waiting:
                if (!sProgram.load(MANIFEST, LIBRARY))
                {
                    sState = LoadState::Failed; // load() said why
                    break;
                }
                sState = LoadState::Compiling;
                return false;
            case LoadState::Compiling:
                if (sProgram.status() == LL::RHI::CompileStatus::Pending)
                {
                    return false;
                }
                if (!sProgram.ready())
                {
                    LL_WARNS("LuminousMetal") << "Tone library failed to compile: " << sProgram.log() << LL_ENDL;
                    sState = LoadState::Failed;
                    break;
                }
                sState = LoadState::Ready;
                prepareUsualPipelines();
                setPassesReady(true);
                LL_INFOS("LuminousMetal") << "Tone library ready: picture frame and upscale native" << LL_ENDL;
                break;
            case LoadState::Ready:
            case LoadState::Failed:
                break;
            }
            sPolling = false;
            return true;
        }

        void startPolling()
        {
            if (!sPolling && sState != LoadState::Failed)
            {
                sPolling = true;
                doOnIdleRepeating(poll);
            }
        }

        // The device lets go of the library: a library that was compiling or ready is loaded again on the next
        // device, its passes not drawn meanwhile. One that failed to compile stays failed.
        void onDeviceRelease()
        {
            if (sState != LoadState::Failed)
            {
                sState = LoadState::Waiting;
            }
            setPassesReady(false);
            resetMetalFX(); // the device's objects
            startPolling();
        }

        // Whether the library can draw now. One that was ready and is no longer (the device let it go) is
        // loaded again, its passes not drawn meanwhile.
        bool usable()
        {
            if (sState == LoadState::Ready && sProgram.ready())
            {
                return true;
            }
            if (sState == LoadState::Ready)
            {
                sState = LoadState::Waiting;
                setPassesReady(false);
                startPolling();
            }
            return false;
        }

        // A native pass in whatever the frame has open. An open stack pass is set aside by it and goes on
        // afterwards with its own bindings. With no pass open the pipeline bound before is bound again after it,
        // so the facades find the binding they left.
        class NativePass
        {
        public:
            NativePass(LL::RHI::Encoder& encoder, const LL::RHI::RenderPassDesc& pass)
                : mEncoder(encoder)
            {
                const LL::RHI::EncoderTracker& state = encoder.state();
                if (state.inPass() && !state.pass().stack)
                {
                    return; // a native pass may set aside only a stack pass
                }
                mRestore = !state.inPass();
                mPipeline = state.pipeline();
                mBegun = encoder.beginRenderPass(pass);
            }

            ~NativePass()
            {
                if (!mBegun)
                {
                    return;
                }
                mEncoder.endRenderPass();
                if (mRestore)
                {
                    mEncoder.setPipeline(mPipeline.get());
                }
            }

            bool begun() const { return mBegun; }

        private:
            LL::RHI::Encoder& mEncoder;
            LL::RHI::Ref<LL::RHI::Pipeline> mPipeline;
            bool mRestore = false;
            bool mBegun = false;
        };

        // MetalFX temporal. Frame N is drawn for the temporal scaler -- the main camera jittered, SMAA, FXAA and the
        // picture's grain left out -- when the scaler for the present's sizes was ready at the end of frame N-1
        // (sTemporalNext), which beginTemporalFrame latches. Its present then draws the motion texture and scales
        // the frame with the reactive mask drawn round the alpha; a frame not drawn for it falls back to Catmull-Rom.
        // The scaler and its textures are made once a size has held for METALFX_SETTLE_FRAMES (readyTemporal), and
        // wherever it is off, settling or cannot be used the present scales with Catmull-Rom.
        LL::RHI::Ref<LL::RHI::TemporalUpscaler> sTemporal;
        LL::RHI::Ref<LL::RHI::Texture> sTemporalFrame;  // the scaled frame, the world view's size
        bool sTemporalScaled = false;                   // sTemporalFrame holds a frame the scaler wrote
        LL::RHI::Ref<LL::RHI::Texture> sMotion;         // the live view's size, RG16Float
        LL::RHI::Ref<LL::RHI::Texture> sMask[2];        // the reactive mask's stages, ping-pong, R8Unorm
        LL::RHI::Ref<LL::RHI::Texture> sBefore;         // the HDR frame before the alpha drawn since, its format
        LL::RHI::Ref<LL::RHI::Texture> sBeforeDepth;    // its depth then, for a mask of blended surfaces only
        std::optional<LL::RHI::TemporalUpscalerDesc> sTemporalRefused;
        std::optional<LL::RHI::TemporalUpscalerDesc> sTemporalPending;
        U32 sTemporalPendingFrames = 0;
        constexpr U32 TEMPORAL_JITTER_FRAMES = 8;      // Halton (2, 3) cycle: thin far edges shimmer less than at 32
        constexpr F32 TEMPORAL_JUMP_METRES = 4.f;       // a camera cut, not a move: the history goes
        constexpr F32 TEMPORAL_JUMP_DEGREES = 25.f;

        // The frame being drawn.
        struct TemporalFrame
        {
            U64 frame = 0;
            bool on = false;          // drawn for the temporal scaler
            bool overlays = false;    // the overlays drawn after the present (overlayReady): on, or the scaler wanted
            bool jittered = false;    // the main camera moved by the jitter (setPerspective asked)
            bool unjittered = false;  // the projection set back after the present
            bool presented = false;   // the present was drawn: no jitter for what comes after it
            F32 jitterX = 0.f;        // texels of the live view
            F32 jitterY = 0.f;
            U32 width = 0;            // the live view's size the jitter was made for
            U32 height = 0;
            U32 maskStages = 0;       // reactive mask stages drawn
            bool copied = false;      // a copy of the frame waits for its mask stage
        };
        TemporalFrame sNow;
        bool sTemporalNext = false;
        bool sUsable = true; // MetalFX temporal can scale the live view as it is now (temporalUsable), once a frame
        U32 sJitterIndex = 0;
        U64 sLastTemporalFrame = 0;
        bool sResetAsked = true;
        bool sHaveLast = false;
        glm::dmat4 sLastViewProjection(1.0); // the last temporal frame's view-projection, without the jitter
        LLVector3 sLastEye;
        LLVector3 sLastAt;
        U64 sLastRegion = 0;
        F32 sSceneMapJitterX = 0.f; // the jitter the scene map was drawn with, in clip units (screen-space reflections)
        F32 sSceneMapJitterY = 0.f;

        // The hold: with the scene paused and the camera still for TEMPORAL_HOLD_SECONDS, the frames are drawn without
        // jitter and the last scaled frame is presented as it is, so a still view does not shimmer. The still time
        // counts frames drawn for the scaler only, from the first frame the current scaler has scaled: a new scaler's
        // frame holds nothing until then (Metal's new memory, black or old pictures), and from where the camera was
        // then, so that a slow, steady move adds up. After TEMPORAL_HOLD_SETTLE held frames one is fed to the scaler
        // once more and kept as the reference, so that the picture held and the reference are the same frame's;
        // every later one is compared with it on the GPU, block by block, and read back a few frames late. A
        // difference, a camera move, the pause ending, a changed setting, a reset history, a measure (Optimise, the
        // benchmark) or a new scaler ends the hold.
        constexpr F64 TEMPORAL_HOLD_SECONDS = 1.5;      // the history settles in about this long
        constexpr F32 TEMPORAL_HOLD_CHANGE = 20.f / 255.f; // one block that differs by more is a change
        constexpr F32 TEMPORAL_HOLD_SPREAD = 6.f / 255.f;  // ... as is a share of blocks differing by more than this
        constexpr F32 TEMPORAL_HOLD_SHARE = 0.01f;      // (the reflection probes' updates move a few blocks a little)
        constexpr U32 TEMPORAL_HOLD_SETTLE = 4;         // held frames before the reference: effects that read the
                                                        // last frame (reflections, exposure) lose its jitter
        constexpr size_t TEMPORAL_HOLD_READS = 6;       // readbacks in flight at most
        bool sHolding = false;
        bool sHoldReferenced = false;
        U32 sHoldFrames = 0;      // frames held so far
        F32 sHoldLargest = 0.f;   // the largest difference the last readbacks found
        F64 sStillSince = 0.0;
        LLVector3 sHoldEye;
        LLVector3 sHoldAt;
        LLVector3 sHoldUp;
        F32 sHoldView = 0.f;
        LL::RHI::Ref<LL::RHI::Texture> sHoldReference; // the first held frame, the live view's size and format
        LL::RHI::Ref<LL::RHI::Texture> sHoldDiff;      // per block the largest difference, R32Float
        std::deque<LL::RHI::Ref<LL::RHI::Readback>> sHoldReads;
        const char* sHoldBreak = nullptr; // why the hold ends at the next frame: a setting changed or the history was
                                          // asked to go since the last frame (resetTemporalHistory)
        bool sHoldUnavailable = false;    // the hold's textures could not be made for this scaler: no hold with it

        // The settle: Second Life's depth of field and the glow map in a frame drawn for the scaler, made where the
        // camera without the jitter saw the frame, averaged with the frames before (LuminousMetalFXTemporalSettleDoF,
        // ...SettleGlow), so what the jitter changes in them from frame to frame does not reach the scaler.
        struct Settled
        {
            LLRenderTarget average[2]; // the average, the size of the layer's target, RGBA16F; ping-pong
            U32 last = 0;              // which holds the last frame's
            U64 frame = 0;             // the frame it was made in; 0 none
            U32 width = 0;             // the layer's texels in it
            U32 height = 0;
        };
        Settled sSettledDoF;
        Settled sSettledGlow;
        // Photo mode's light in the air and the lens flare's light (temporalSettle): what the jitter changes in them, a
        // gap in the leaves opening and closing, is as large as a change of the layer, so they are taken whole only
        // where the camera moved, and a real change reaches them over the frames averaged. Enhance's blur is also taken
        // whole where its weight left what the frame holds round a place (React::Weight).
        std::array<Settled, size_t(SettledLayer::Count)> sSettledLayers;
        // Where a settle also takes this frame whole, besides where the camera moved (the settle's react).
        enum class React : U8
        {
            Camera,  // nowhere else
            Change,  // where the layer changed much
            Weight   // where the last average's weight (alpha) lies outside this frame's round the place
        };
        constexpr F32 SETTLE_MOTION_FULL = 16.f; // texels of the live view the camera moves a place by, for the frame
                                                 // taken whole
        constexpr F32 SETTLE_FRAMES_LEAST = 1.5f; // fewer frames to average: no settle

        U32 probeBits()
        {
            static LLCachedControl<U32> probe(gSavedSettings, "LuminousMetalFXTemporalProbe", 0);
            return probe();
        }

        F32 halton(U32 index, U32 base)
        {
            F32 f = 1.f;
            F32 r = 0.f;
            while (index > 0)
            {
                f /= (F32)base;
                r += f * (F32)(index % base);
                index /= base;
            }
            return r;
        }

        void endHold(const char* why = "the scaler goes")
        {
            if (sHolding)
            {
                LL_INFOS("LuminousMetal") << "MetalFX temporal hold ends: " << why << LL_ENDL;
            }
            sHolding = false;
            sHoldReferenced = false;
            sHoldFrames = 0;
            sHoldReads.clear();
        }

        // The hold ended at the present: the still time begins again, so that it does not start again at the next
        // frame.
        void stopHold(const char* why)
        {
            endHold(why);
            sStillSince = LLFrameTimer::getElapsedSeconds();
        }

        void releaseSettled(Settled& s)
        {
            s.average[0].release();
            s.average[1].release();
            s.frame = 0;
        }

        void releaseTemporal()
        {
            endHold();
            releaseSettled(sSettledDoF);
            releaseSettled(sSettledGlow);
            for (Settled& settled : sSettledLayers)
            {
                releaseSettled(settled);
            }
            sHoldReference = nullptr;
            sHoldDiff = nullptr;
            sHoldUnavailable = false;
            sTemporal = nullptr;
            sTemporalFrame = nullptr;
            sTemporalScaled = false;
            sMotion = nullptr;
            sMask[0] = nullptr;
            sMask[1] = nullptr;
            sBefore = nullptr;
            sBeforeDepth = nullptr;
            sTemporalNext = false;
            sResetAsked = true;
            sHaveLast = false;
        }

        void resetTemporal()
        {
            releaseTemporal();
            sTemporalRefused.reset();
            sTemporalPending.reset();
        }

        LL::RHI::Ref<LL::RHI::Texture> makeTexture(LL::RHI::Device& device, LL::RHI::Format format, U32 width, U32 height,
                                                    U8 usage, const char* name)
        {
            LL::RHI::TextureDesc desc;
            desc.format = format;
            desc.width = width;
            desc.height = height;
            desc.usage = usage;
            return device.createTexture(desc, name);
        }

        // The temporal scaler and its textures for the description, after the settle; false while it settles or
        // when it cannot be made (then refused for that description, Catmull-Rom).
        bool readyTemporal(LL::RHI::Device& device, const LL::RHI::TemporalUpscalerDesc& desc)
        {
            if (sTemporal.notNull() && sTemporal->desc() == desc)
            {
                return true;
            }
            releaseTemporal();
            if (sTemporalRefused && *sTemporalRefused == desc)
            {
                return false;
            }
            if (!sTemporalPending || !(*sTemporalPending == desc))
            {
                sTemporalPending = desc;
                sTemporalPendingFrames = 0;
            }
            if (++sTemporalPendingFrames < METALFX_SETTLE_FRAMES)
            {
                return false;
            }
            sTemporalPending.reset();
            sTemporal = device.createTemporalUpscaler(desc); // a WARN line when it cannot
            if (sTemporal.notNull())
            {
                sTemporalFrame = makeTexture(device, desc.outputFormat, desc.outputWidth, desc.outputHeight,
                                             sTemporal->outputUsage() | LL::RHI::TEXTURE_SAMPLED, "MetalFX temporal frame");
                sMotion = makeTexture(device, desc.motionFormat, desc.inputWidth, desc.inputHeight,
                                      sTemporal->motionUsage() | LL::RHI::TEXTURE_SAMPLED | LL::RHI::TEXTURE_RENDER_TARGET,
                                      "MetalFX motion");
                if (desc.reactiveFormat != LL::RHI::Format::Unknown)
                {
                    const U8 usage = sTemporal->reactiveUsage() | LL::RHI::TEXTURE_SAMPLED | LL::RHI::TEXTURE_RENDER_TARGET;
                    sMask[0] = makeTexture(device, desc.reactiveFormat, desc.inputWidth, desc.inputHeight, usage,
                                           "MetalFX reactive mask 0");
                    sMask[1] = makeTexture(device, desc.reactiveFormat, desc.inputWidth, desc.inputHeight, usage,
                                           "MetalFX reactive mask 1");
                }
            }
            const bool masks = desc.reactiveFormat == LL::RHI::Format::Unknown || (sMask[0].notNull() && sMask[1].notNull());
            if (sTemporal.isNull() || sTemporalFrame.isNull() || sMotion.isNull() || !masks)
            {
                releaseTemporal();
                sTemporalRefused = desc;
                LL_INFOS("LuminousMetal") << "MetalFX temporal cannot scale " << desc.inputWidth << "x" << desc.inputHeight
                                          << " to " << desc.outputWidth << "x" << desc.outputHeight
                                          << ": Catmull-Rom at this size" << LL_ENDL;
                return false;
            }
            LL_INFOS("LuminousMetal") << "MetalFX temporal scales the live view " << desc.inputWidth << "x"
                                      << desc.inputHeight << " to " << desc.outputWidth << "x" << desc.outputHeight
                                      << (desc.reactiveFormat != LL::RHI::Format::Unknown ? ", with a reactive mask" : "")
                                      << LL_ENDL;
            sResetAsked = true;
            return true;
        }

        // The scaler was refused for the world view's size as it is now.
        bool refusedAtWorldViewSize()
        {
            return sTemporalRefused && gViewerWindow
                   && sTemporalRefused->outputWidth == (U32)gViewerWindow->getWorldViewWidthRaw()
                   && sTemporalRefused->outputHeight == (U32)gViewerWindow->getWorldViewHeightRaw();
        }

        // Whether MetalFX temporal can scale the live view as it is now: nothing fixed to the screen is drawn into it
        // live (the present lets the scaler go then), and it was not refused for the world view's size. Only asked
        // while it would scale (temporal() is Scales, the one case its answer is read in); true otherwise, without
        // the screen-locked test's cost.
        bool usableNow()
        {
            if (LuminousPhotoMode::temporal() != LuminousPhotoMode::Temporal::Scales)
            {
                return true;
            }
            if (LuminousPhotoMode::screenLockedLive())
            {
                return false;
            }
            return !refusedAtWorldViewSize();
        }

        // A full-target native pass of one fragment function into colour, the caller binding its inputs.
        template <typename Bind>
        bool drawInto(LL::RHI::Device& device, LL::RHI::Texture* colour, const char* fragment, const char* name, Bind bind)
        {
            LL::RHI::Pipeline* pipeline = readyPipeline(fragment, colour->desc().format, LL::RHI::Format::Unknown);
            if (!pipeline)
            {
                return false;
            }
            LL::RHI::RenderPassDesc pass;
            pass.native = true;
            pass.name = name;
            pass.colorCount = 1;
            pass.color[0].texture = colour;
            pass.color[0].load = LL::RHI::LoadAction::DontCare; // every texel is drawn
            pass.color[0].store = LL::RHI::StoreAction::Store;
            LL::RHI::Encoder& encoder = device.encoder();
            NativePass native(encoder, pass);
            if (!native.begun())
            {
                return false;
            }
            encoder.setPipeline(pipeline);
            encoder.setViewport(0, 0, (S32)colour->desc().width, (S32)colour->desc().height);
            bind(encoder);
            encoder.draw(LL::RHI::PrimitiveType::Triangles, 0, 3);
            return true;
        }

        // The reactive mask's stages run in the main view's deferred lighting of a frame drawn for the scaler.
        bool maskStagesWanted(LLRenderTarget* screen)
        {
            return metal() && sNow.on && !sHolding && sTemporal.notNull() && sTemporal->desc().reactiveFormat != LL::RHI::Format::Unknown
                && screen && !gCubeSnapshot && gPipeline.mRT == &gPipeline.mMainRT && screen == &gPipeline.mMainRT.screen
                && !LLPipeline::sRenderingHUDs && !LLPipeline::sImpostorRender && screen->getWidth() == sTemporal->desc().inputWidth
                && screen->getHeight() == sTemporal->desc().inputHeight && usable();
        }

        // This frame's reactive mask: the last stage's, or none drawn.
        LL::RHI::Texture* frameMask(LL::RHI::Device& device)
        {
            if (sNow.maskStages > 0)
            {
                return sMask[(sNow.maskStages - 1) % 2].get();
            }
            LL::RHI::Texture* mask = sMask[0].get();
            const bool drawn = drawInto(device, mask, REACTIVE_NONE_FRAGMENT, "luminous temporal reactive none",
                                        [](LL::RHI::Encoder&) {});
            return drawn ? mask : nullptr;
        }

        // The last frame's view-projection without the jitter, against this frame's with it: this frame's clip
        // space to the last's, in double precision. The matrices are the main camera's, kept at the end of the
        // deferred lighting (gGLLastModelView, gGLLastProjection: this frame's at the present).
        glm::dmat4 clipToLastClip(bool reset, F32 jitter_ndc_x, F32 jitter_ndc_y)
        {
            const glm::dmat4 view(get_last_modelview());
            const glm::dmat4 jittered(get_last_projection());
            const glm::dmat4 unjitter = glm::translate(glm::dmat4(1.0), glm::dvec3(-jitter_ndc_x, -jitter_ndc_y, 0.0));
            const glm::dmat4 now_jittered = jittered * view;
            const glm::dmat4 now = unjitter * jittered * view;
            const glm::dmat4 last = reset || !sHaveLast ? now : sLastViewProjection;
            sLastViewProjection = now;
            sHaveLast = true;
            return last * glm::inverse(now_jittered);
        }

        // Whether the history must go this frame: a new scaler or an explicit ask, a gap since the last temporal
        // frame, a teleport, another region, a camera cut.
        bool historyGoes(const PassContext& context)
        {
            bool reset = sResetAsked || sLastTemporalFrame + 1 != context.frame || (probeBits() & 16);
            if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || gTeleportDisplay)
            {
                reset = true;
            }
            const U64 region = gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0;
            if (region != sLastRegion)
            {
                reset = true;
            }
            const LLViewerCamera* camera = LLViewerCamera::getInstance();
            const LLVector3 eye = camera->getOrigin();
            const LLVector3 at = camera->getAtAxis();
            const F32 turn = std::acos(llclamp(at * sLastAt, -1.f, 1.f)) * RAD_TO_DEG;
            if ((eye - sLastEye).length() > TEMPORAL_JUMP_METRES || turn > TEMPORAL_JUMP_DEGREES)
            {
                reset = true;
            }
            sLastRegion = region;
            sLastEye = eye;
            sLastAt = at;
            sResetAsked = false;
            return reset;
        }

        // Whether a readback in flight shows a block that changed since the hold began; finished ones are let go.
        bool holdChangeSeen()
        {
            while (!sHoldReads.empty() && sHoldReads.front()->ready())
            {
                LL::RHI::Ref<LL::RHI::Readback> read = sHoldReads.front();
                sHoldReads.pop_front();
                const std::span<const std::byte> data = read->data();
                const U32 pitch = read->rowPitch();
                if (sHoldDiff.isNull() || pitch == 0)
                {
                    continue;
                }
                const U32 width = sHoldDiff->desc().width;
                const U32 height = sHoldDiff->desc().height;
                U32 spread = 0;
                F32 largest = 0.f;
                for (U32 y = 0; y < height && (size_t)(y + 1) * pitch <= data.size(); ++y)
                {
                    const F32* row = reinterpret_cast<const F32*>(data.data() + (size_t)y * pitch);
                    for (U32 x = 0; x < width && (x + 1) * sizeof(F32) <= pitch; ++x)
                    {
                        if (!(row[x] <= TEMPORAL_HOLD_CHANGE)) // a NaN counts as a change
                        {
                            sHoldLargest = row[x];
                            return true;
                        }
                        if (row[x] > TEMPORAL_HOLD_SPREAD)
                        {
                            ++spread;
                            largest = llmax(largest, row[x]);
                        }
                    }
                }
                if ((F32)spread > TEMPORAL_HOLD_SHARE * (F32)(width * height))
                {
                    sHoldLargest = largest;
                    return true;
                }
            }
            return false;
        }

        // This frame fed to the scaler: its motion texture drawn and the source scaled into sTemporalFrame, with this
        // frame's jitter (none in a held frame, which is drawn without). In a hold (held) a history that must go is
        // not fed; the hold ends instead. False when the frame was not fed (Catmull-Rom); a scaler that refuses it is
        // let go and its description refused.
        bool feedScaler(PassContext& context, LL::RHI::Device& device, LL::RHI::Texture* source,
                        LL::RHI::Texture* frame_depth, LuminousTemporalConstants& constants, bool held)
        {
            const LL::RHI::TemporalUpscalerDesc desc = sTemporal->desc();
            const U32 probe = probeBits();
            LL::RHI::Texture* mask = desc.reactiveFormat != LL::RHI::Format::Unknown ? frameMask(device) : nullptr;
            if (desc.reactiveFormat != LL::RHI::Format::Unknown && !mask)
            {
                sResetAsked = true;
                return false;
            }
            const bool reset = historyGoes(context);
            if (held && reset)
            {
                sResetAsked = true; // for the next frame fed, after the hold
                return false;
            }
            const F32 jitter_x = sNow.jittered ? sNow.jitterX : 0.f;
            const F32 jitter_y = sNow.jittered ? sNow.jitterY : 0.f;
            const glm::dmat4 to_last = clipToLastClip(reset, 2.f * jitter_x / (F32)desc.inputWidth,
                                                      2.f * jitter_y / (F32)desc.inputHeight);
            for (int c = 0; c < 4; ++c)
            {
                constants.clipToLastClip.columns[c] = simd_make_float4((float)to_last[c][0], (float)to_last[c][1],
                                                                       (float)to_last[c][2], (float)to_last[c][3]);
            }
            constants.size = simd_make_float2((float)desc.inputWidth, (float)desc.inputHeight);
            constants.jitter = simd_make_float2(jitter_x, jitter_y);
            const F32 motion_sign = (probe & 2) ? -1.f : 1.f;
            constants.motionSign = simd_make_float2(motion_sign * ((probe & 256) ? -1.f : 1.f),
                                                    motion_sign * ((probe & 128) ? -1.f : 1.f));
            constants.outputHeight = (float)desc.outputHeight;
            const bool motion = drawInto(device, sMotion.get(), MOTION_FRAGMENT, "luminous temporal motion",
                [&](LL::RHI::Encoder& encoder)
                {
                    LL::RHI::setNativeConstants(encoder, LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_BUFFER_TEMPORAL,
                                                constants);
                    encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_DEPTH, frame_depth);
                });
            if (!motion)
            {
                sResetAsked = true;
                return false; // the motion pipeline is still compiling
            }
            LL::RHI::TemporalUpscaleInputs inputs;
            inputs.color = source;
            inputs.depth = frame_depth;
            inputs.motion = sMotion.get();
            inputs.reactive = mask;
            inputs.output = sTemporalFrame.get();
            inputs.width = desc.inputWidth;
            inputs.height = desc.inputHeight;
            const F32 jitter_sign = (probe & 1) ? -1.f : 1.f;
            inputs.jitterX = jitter_x * jitter_sign * ((probe & 32) ? -1.f : 1.f);
            inputs.jitterY = jitter_y * jitter_sign * ((probe & 64) ? -1.f : 1.f);
            inputs.reset = reset;
            if (!device.encoder().upscaleTemporal(sTemporal.get(), inputs))
            {
                // The encoder wrote why; Catmull-Rom at this size from now on.
                releaseTemporal();
                sTemporalRefused = desc;
                return false;
            }
            sLastTemporalFrame = context.frame;
            sTemporalScaled = true;
            return true;
        }

        // A held frame. The first TEMPORAL_HOLD_SETTLE present the last scaled frame as it is, while what reads the
        // last frame (reflections, exposure) loses its jitter. The next is fed to the scaler once more, without the
        // jitter, and kept as the reference, so the picture held and the reference are the same frame's and a change
        // in the settle frames reaches the screen. Each later one is compared with the reference and read back. False
        // when it cannot hold (Catmull-Rom this frame; the hold ends).
        bool heldFrame(PassContext& context, LL::RHI::Device& device, LL::RHI::Texture* source,
                       LL::RHI::Texture* frame_depth, LuminousTemporalConstants& constants)
        {
            const LL::RHI::TextureDesc& d = source->desc();
            if (++sHoldFrames <= TEMPORAL_HOLD_SETTLE)
            {
                // Settling: the held picture shows, nothing is compared yet.
            }
            else if (!sHoldReferenced)
            {
                if (sHoldReference.isNull() || sHoldReference->desc().format != d.format
                    || sHoldReference->desc().width != d.width || sHoldReference->desc().height != d.height)
                {
                    sHoldReference = makeTexture(device, d.format, d.width, d.height, LL::RHI::TEXTURE_SAMPLED,
                                                 "MetalFX hold reference");
                    const U32 block = LUMINOUS_TONE_HOLD_BLOCK;
                    sHoldDiff = makeTexture(device, LL::RHI::Format::R32Float, (d.width + block - 1) / block,
                                            (d.height + block - 1) / block,
                                            LL::RHI::TEXTURE_RENDER_TARGET | LL::RHI::TEXTURE_SAMPLED
                                                | LL::RHI::TEXTURE_READBACK,
                                            "MetalFX hold difference");
                }
                if (sHoldReference.isNull() || sHoldDiff.isNull())
                {
                    // Not again with this scaler, or the view would go round held frames and a Catmull-Rom one.
                    sHoldReference = nullptr;
                    sHoldDiff = nullptr;
                    sHoldUnavailable = true;
                    stopHold("no memory for the hold");
                    return false;
                }
                if (!feedScaler(context, device, source, frame_depth, constants, true))
                {
                    stopHold("the held frame could not be scaled");
                    return false;
                }
                LL::RHI::Region region;
                region.width = d.width;
                region.height = d.height;
                device.encoder().copyTexture(source, 0, 0, region, sHoldReference.get(), 0, 0, 0, 0);
                sHoldReferenced = true;
                LL_INFOS("LuminousMetal") << "MetalFX temporal hold begins" << LL_ENDL;
            }
            else if (sHoldReads.size() < TEMPORAL_HOLD_READS)
            {
                LL::RHI::Texture* reference = sHoldReference.get();
                const bool compared = drawInto(device, sHoldDiff.get(), HOLD_DIFF_FRAGMENT, "luminous temporal hold",
                    [&](LL::RHI::Encoder& encoder)
                    {
                        encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_FRAME, source);
                        encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_BEFORE, reference);
                    });
                if (compared)
                {
                    LL::RHI::PixelLayout layout;
                    layout.components = LL::RHI::PixelLayout::Components::R;
                    layout.type = LL::RHI::PixelLayout::Type::F32;
                    const LL::RHI::TextureDesc& dd = sHoldDiff->desc();
                    LL::RHI::Ref<LL::RHI::Readback> read =
                        device.encoder().readback(sHoldDiff.get(), 0, 0, { 0, 0, dd.width, dd.height }, layout);
                    if (read.notNull())
                    {
                        sHoldReads.push_back(read);
                    }
                }
            }
            // The history stays this frame's: the camera has not moved, so the next fed frame follows on.
            sLastTemporalFrame = context.frame;
            return true;
        }

        // The source scaled up to the world view by MetalFX's temporal scaler, or null: Catmull-Rom then (while the
        // scaler settles, in a frame not drawn for it, where it is off or cannot be used).
        LL::RHI::Texture* temporalScaled(PassContext& context, LL::RHI::Device& device, LL::RHI::Texture* source,
                                         LL::RHI::Texture* frame_depth, LuminousTemporalConstants& constants)
        {
            static LLCachedControl<bool> on(gSavedSettings, "LuminousMetalFXTemporal", false);
            if (gSnapshot || gCubeSnapshot)
            {
                // A snapshot's frame passing through, at its own sizes: the live view's scaler and its settle stay as
                // they are for the frames after it.
                return nullptr;
            }
            sTemporalNext = false;
            if (!on() || !device.capabilities().temporalUpscaler || !frame_depth || LuminousPhotoMode::screenLockedLive())
            {
                if (sTemporal.notNull())
                {
                    releaseTemporal();
                }
                return nullptr;
            }
            if (LuminousPhotoMode::liveRenderScale() >= 1.f)
            {
                // Smaller for another reason (RenderResolutionDivisor or ...Multiplier): no frame is drawn for the
                // scaler then (beginTemporalFrame), so nothing is kept for it.
                if (sTemporal.notNull())
                {
                    releaseTemporal();
                }
                sTemporalPending.reset();
                return nullptr;
            }
            const U32 probe = probeBits();
            LL::RHI::TemporalUpscalerDesc desc;
            desc.colorFormat = source->desc().format;
            desc.depthFormat = frame_depth->desc().format;
            desc.motionFormat = LL::RHI::Format::RG16Float;
            desc.reactiveFormat = (probe & 4) ? LL::RHI::Format::Unknown : LL::RHI::Format::R8Unorm;
            desc.outputFormat = desc.colorFormat;
            desc.inputWidth = source->desc().width;
            desc.inputHeight = source->desc().height;
            desc.outputWidth = (U32)llmax(gGLViewport[2], 0);
            desc.outputHeight = (U32)llmax(gGLViewport[3], 0);
            if (desc.outputWidth <= desc.inputWidth && desc.outputHeight <= desc.inputHeight)
            {
                releaseTemporal();
                return nullptr;
            }
            if (frame_depth->desc().width != desc.inputWidth || frame_depth->desc().height != desc.inputHeight)
            {
                return nullptr; // a frame whose depth is not the picture's size: Catmull-Rom
            }
            if (sTemporalRefused && *sTemporalRefused == desc)
            {
                return nullptr; // Catmull-Rom at these sizes
            }
            if (!readyTemporal(device, desc))
            {
                return nullptr;
            }
            sTemporalNext = true; // the next frame is drawn for it
            if (sHolding)
            {
                constants.outputHeight = (float)desc.outputHeight; // the present's grain
                return heldFrame(context, device, source, frame_depth, constants) ? sTemporalFrame.get() : nullptr;
            }
            if (!sNow.on || (sNow.jittered && (sNow.width != desc.inputWidth || sNow.height != desc.inputHeight)))
            {
                sResetAsked = true;
                return nullptr; // this frame was not drawn for it
            }
            if (!feedScaler(context, device, source, frame_depth, constants, false))
            {
                return nullptr;
            }
            // A settle not drawn in the last frames (its layer switched off, Only in renders, or drawn by photo mode's
            // own pass now): its averages let go, made again when next drawn.
            for (Settled* settled : { &sSettledDoF, &sSettledGlow })
            {
                if (settled->frame != 0 && settled->frame + 2 < sNow.frame)
                {
                    releaseSettled(*settled);
                }
            }
            for (Settled& settled : sSettledLayers)
            {
                if (settled.frame != 0 && settled.frame + 2 < sNow.frame)
                {
                    releaseSettled(settled);
                }
            }
            return sTemporalFrame.get();
        }

        // The picture's part of the finished live frame. The rect is the centred part of the source in the
        // picture's shape, as rawSnapshot keeps it and as the OpenGL pass finds it.
        bool drawPictureFrame(PassContext& /*context*/, PassTargets& targets)
        {
            LLRenderTarget* src = targets.frame;
            LLRenderTarget* dst = targets.other;
            LL::RHI::Device* device = LL::RHI::viewerDevice();
            if (!src || !dst || !device || !usable())
            {
                return false;
            }
            LL::RHI::Texture* source = src->getRHITexture(0);
            LL::RHI::Texture* destination = dst->getRHITexture(0);
            if (!source || !destination)
            {
                return false;
            }
            LuminousToneParams params;
            LuminousPhotoMode::fillToneParams(params);
            if (params.pictureAspect <= 0.f)
            {
                return false;
            }
            const F32 frame_aspect = (F32)src->getWidth() / (F32)llmax(src->getHeight(), 1U);
            const F32 w = params.pictureAspect < frame_aspect ? params.pictureAspect / frame_aspect : 1.f;
            const F32 h = params.pictureAspect < frame_aspect ? 1.f : frame_aspect / params.pictureAspect;
            const F32 width = (F32)dst->getWidth();
            const F32 height = (F32)dst->getHeight();
            LuminousPictureFrameConstants constants;
            constants.rect = simd_make_float4((0.5f - 0.5f * w) * width, (0.5f - 0.5f * h) * height,
                                              (0.5f + 0.5f * w) * width, (0.5f + 0.5f * h) * height);

            LL::RHI::Pipeline* pipeline = readyPipeline(FRAME_FRAGMENT, destination->desc().format, LL::RHI::Format::Unknown);
            if (!pipeline)
            {
                return false;
            }
            LL_PROFILE_GPU_ZONE("luminous frame");
            LL::RHI::RenderPassDesc pass;
            pass.native = true;
            pass.name = "luminous picture frame";
            pass.colorCount = 1;
            pass.color[0].texture = destination;
            pass.color[0].load = LL::RHI::LoadAction::DontCare; // every pixel is drawn
            pass.color[0].store = LL::RHI::StoreAction::Store;
            LL::RHI::Encoder& encoder = device->encoder();
            NativePass native(encoder, pass);
            if (!native.begun())
            {
                return false;
            }
            encoder.setPipeline(pipeline);
            LL::RHI::setNativeConstants(encoder, LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_BUFFER_FRAME, constants);
            encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_FRAME, source);
            encoder.draw(LL::RHI::PrimitiveType::Triangles, 0, 3);
            return true;
        }

        // The smaller live frame scaled up into the window's world view, in the back buffer's stack pass: the
        // same attachments, loaded, the world view's viewport, and the frame's depth written as OpenGL's
        // present writes it (always, whatever is there).
        bool drawUpscale(PassContext& context, PassTargets& targets)
        {
            LLRenderTarget* src = targets.frame;
            LL::RHI::Device* device = LL::RHI::viewerDevice();
            if (!src || !device || !context.depth || !usable())
            {
                return false;
            }
            LL::RHI::Texture* source = src->getRHITexture(0);
            LL::RHI::Texture* frame_depth = context.depth->getRHIDepth();
            if (!source || !frame_depth)
            {
                return false;
            }
            if (!LLRenderTarget::beginStackPass())
            {
                return false;
            }
            LL::RHI::RenderPassDesc window;
            if (!LLRenderTarget::currentPass(window) || window.colorCount < 1 || window.color[0].texture.isNull())
            {
                return false;
            }
            const bool write_depth = window.depth.texture.notNull() && !window.depth.readOnly;
            const LL::RHI::Format colour = window.color[0].texture->desc().format;
            const LL::RHI::Format depth = write_depth ? window.depth.texture->desc().format : LL::RHI::Format::Unknown;
            LL::RHI::Pipeline* scaling = readyPipeline(write_depth ? PRESENT_FRAGMENT : PRESENT_NO_DEPTH_FRAGMENT, colour, depth);
            LL::RHI::Pipeline* temporal_copying =
                readyPipeline(write_depth ? TEMPORAL_FRAGMENT : TEMPORAL_NO_DEPTH_FRAGMENT, colour, depth);
            // MetalFX scales the frame first, outside any native pass (it breaks the window's stack pass as a copy
            // does), once the copy that presents its frame can draw: the temporal scaler where it scales, else
            // Catmull-Rom.
            LuminousTemporalConstants temporal_constants{};
            LL::RHI::Texture* scaled = temporal_copying
                ? temporalScaled(context, *device, source, frame_depth, temporal_constants)
                : nullptr;
            const bool temporal = scaled != nullptr;
            // What a frame drawn for the scaler leaves out because it is fixed to the screen (live lens distortion,
            // the light leak, film grain, Worn photo) is drawn on the scaled frame; where the scaler gave no frame
            // for it, on the Catmull-Rom scaled one by the screen present. Any other frame has none of it, and its
            // present is as before.
            LuminousTemporalScreenConstants screen_constants;
            const bool screen = LuminousPhotoMode::fillTemporalScreen(screen_constants, (F32)gGLViewport[2],
                                                                      (F32)gGLViewport[3]);
            F32 grain_size = 1.f;
            temporal_constants.grain = LuminousPhotoMode::grainAfterUpscale(grain_size);
            temporal_constants.grainSize = grain_size;
            LL::RHI::Pipeline* screen_scaling = nullptr;
            if (!temporal && (screen || temporal_constants.grain > 0.f))
            {
                temporal_constants.outputHeight = (float)llmax(gGLViewport[3], 1);
                screen_scaling = readyPipeline(write_depth ? SCREEN_FRAGMENT : SCREEN_NO_DEPTH_FRAGMENT, colour, depth);
            }
            const bool screen_present = temporal || screen_scaling != nullptr;
            LL::RHI::Pipeline* pipeline = temporal ? temporal_copying : screen_scaling ? screen_scaling : scaling;
            if (!pipeline)
            {
                return false;
            }
            LL_PROFILE_GPU_ZONE("luminous present upscale");
            LL::RHI::RenderPassDesc pass;
            pass.native = true;
            pass.name = "luminous present upscale";
            pass.colorCount = 1;
            pass.color[0].texture = window.color[0].texture;
            pass.color[0].level = window.color[0].level;
            pass.color[0].layer = window.color[0].layer;
            pass.color[0].load = LL::RHI::LoadAction::Load; // outside the world view the window keeps what it has
            pass.color[0].store = LL::RHI::StoreAction::Store;
            if (write_depth)
            {
                pass.depth.texture = window.depth.texture;
                pass.depth.level = window.depth.level;
                pass.depth.layer = window.depth.layer;
                pass.depth.load = LL::RHI::LoadAction::Load;
                pass.depth.store = LL::RHI::StoreAction::Store;
            }
            LL::RHI::Encoder& encoder = device->encoder();
            NativePass native(encoder, pass); // sets the window's stack pass aside
            if (!native.begun())
            {
                return false;
            }
            encoder.setPipeline(pipeline);
            if (write_depth)
            {
                LL::RHI::DepthStencilDesc depth;
                depth.depthTest = true;
                depth.depthWrite = true;
                depth.depthFunc = LL::RHI::CompareFunc::AlwaysPass;
                encoder.setDepthStencilState(device->getDepthStencilState(depth).get());
                encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_DEPTH, frame_depth);
            }
            encoder.setViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
            encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_FRAME, scaled ? scaled : source);
            if (screen_present)
            {
                LL::RHI::setNativeConstants(encoder, LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_BUFFER_TEMPORAL,
                                            temporal_constants);
                LL::RHI::setNativeConstants(encoder, LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_BUFFER_SCREEN,
                                            screen_constants);
            }
            encoder.draw(LL::RHI::PrimitiveType::Triangles, 0, 3);
            return true;
        }
    }

    void beginTemporalFrame(U64 frame)
    {
        sNow = TemporalFrame();
        sNow.frame = frame;
        sOverlay = OverlayFrame();
        // A setting changed or the history was asked to go since the last frame: the hold ends, whatever its
        // comparison sees (a change under its thresholds, or one in its settle frames, is in the reference).
        const char* changed = sHoldBreak;
        sHoldBreak = nullptr;
        // The camera for this frame, against where it was when the still time began, so a slow, steady move that
        // stays under the thresholds from frame to frame still adds up to a move.
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 eye = camera->getOrigin();
        const LLVector3 at = camera->getAtAxis();
        const LLVector3 up = camera->getUpAxis();
        const F32 view = camera->getView();
        const F64 now = LLFrameTimer::getElapsedSeconds();
        auto still_from_now = [&]()
        {
            sStillSince = now;
            sHoldEye = eye;
            sHoldAt = at;
            sHoldUp = up;
            sHoldView = view;
        };
        // Whether MetalFX can scale the live view, once a frame. The live view's size follows it
        // (LuminousPhotoMode::chosenLiveScale: the full size where MetalFX would scale but cannot), so a change
        // makes the main targets again in this frame. Not in a snapshot's frames: they hide the UI and widen the world
        // view, so they would read it differently and make the targets again twice for each thumbnail; the live
        // frames' answer holds across them.
        if (!gSnapshot)
        {
            const F32 scale_before = LuminousPhotoMode::liveRenderScale();
            sUsable = usableNow();
            if (LuminousPhotoMode::liveRenderScale() != scale_before)
            {
                gResizeScreenTexture = true;
            }
        }
        // Drawn for the scaler only where the present will still scale it: the scaler ready at the last present for
        // the world view's size as it is, the main targets not about to be made again, nothing fixed to the screen
        // drawn live, and no still to be taken for a render (it shows the frame drawn as usual, with its
        // anti-aliasing and grain, not the scaler's jittered input).
        const bool wanted = !gSnapshot && LuminousPhotoMode::temporal() == LuminousPhotoMode::Temporal::Scales
                            && LuminousPhotoMode::liveRenderScale() < 1.f && sUsable && !LuminousPictureView::wantsStill();
        const bool latch = sTemporalNext && wanted && !gResizeScreenTexture && sTemporal.notNull() && gViewerWindow
                           && screenPresentReady()
                           && sTemporal->desc().outputWidth == (U32)gViewerWindow->getWorldViewWidthRaw()
                           && sTemporal->desc().outputHeight == (U32)gViewerWindow->getWorldViewHeightRaw();
        // The overlays (the picture frame, Firestorm's capture frame) are left out of the frame and drawn after the
        // present also while the scaler is being made or settles: the frame it is made for then ends in the same
        // target, of the same format, as the frames drawn for it, or each would make it again.
        sNow.overlays = wanted;
        if (!latch)
        {
            endHold("a frame not drawn for the scaler");
            still_from_now(); // the still time begins again with the scaler's frames
            return;
        }
        sNow.on = true;

        // The hold: the camera, a change of settings, the pause, a measure, and what the last comparisons found.
        static LLCachedControl<bool> hold(gSavedSettings, "LuminousMetalFXTemporalHold", true);
        const bool moved = (eye - sHoldEye).length() > 0.001f || (at - sHoldAt).length() > 1e-5f
                           || (up - sHoldUp).length() > 1e-5f || std::fabs(view - sHoldView) > 1e-6f;
        const char* why = moved ? "the camera moved"
                          : changed ? changed
                          : !hold() ? "switched off"
                          : LuminousOptimise::running() || LuminousBenchmark::running() ? "a measure runs"
                          : !LuminousPhotoMode::scenePaused() ? "the scene runs"
                          : sTemporal.isNull() ? "no scaler"
                          : sHoldUnavailable ? "no memory for the hold"
                          : !sTemporalScaled ? "nothing scaled yet"
                          : holdChangeSeen() ? "the view changed"
                          : nullptr;
        if (why && sHolding && std::string(why) == "the view changed")
        {
            LL_INFOS("LuminousMetal") << "MetalFX temporal hold: a block differs by " << sHoldLargest * 255.f << "/255"
                                      << LL_ENDL;
        }
        if (why)
        {
            endHold(why);
            still_from_now();
        }
        else if (!sHolding && now - sStillSince >= TEMPORAL_HOLD_SECONDS)
        {
            sHolding = true;
            sHoldReferenced = false;
            sHoldReads.clear();
        }
        if (sHolding)
        {
            return; // drawn without jitter
        }

        static LLCachedControl<F32> amplitude(gSavedSettings, "LuminousMetalFXTemporalJitterScale", 1.f);
        static LLCachedControl<U32> cycle(gSavedSettings, "LuminousMetalFXTemporalJitterCycle", TEMPORAL_JITTER_FRAMES);
        if (!(probeBits() & 8))
        {
            const U32 frames = llclamp((U32)cycle(), 2U, 64U);
            const F32 scale = llclamp((F32)amplitude(), 0.f, 1.f);
            const U32 index = (sJitterIndex++ % frames) + 1;
            sNow.jitterX = (halton(index, 2) - 0.5f) * scale;
            sNow.jitterY = (halton(index, 3) - 0.5f) * scale;
        }
    }

    bool temporalSupported()
    {
        const LL::RHI::Device* device = metal() ? LL::RHI::viewerDevice() : nullptr;
        return device && device->capabilities().temporalUpscaler;
    }

    F32 temporalMaxScale()
    {
        return temporalSupported() ? LL::RHI::viewerDevice()->capabilities().temporalMaxScale : 0.f;
    }

    bool temporalFrame()
    {
        return sNow.on;
    }

    bool temporalUsable()
    {
        return sUsable;
    }

    bool temporalRefused()
    {
        return refusedAtWorldViewSize();
    }

    bool liveJitter(F32& ndc_x, F32& ndc_y)
    {
        // Not after the present: a setPerspective there (setup3DRender for the interface drawn over the world, the
        // next idle) keeps the projection temporalAfterPresent set back.
        if (!sNow.on || sNow.presented || sHolding || (probeBits() & 8) || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return false;
        }
        const U32 width = gPipeline.mMainRT.screen.getWidth();
        const U32 height = gPipeline.mMainRT.screen.getHeight();
        if (width == 0 || height == 0)
        {
            return false;
        }
        sNow.jittered = true;
        sNow.unjittered = false;
        sNow.width = width;
        sNow.height = height;
        ndc_x = 2.f * sNow.jitterX / (F32)width;
        ndc_y = 2.f * sNow.jitterY / (F32)height;
        return true;
    }

    bool liveJitterApplied(F32& ndc_x, F32& ndc_y)
    {
        ndc_x = 0.f;
        ndc_y = 0.f;
        if (!sNow.on || !sNow.jittered || sNow.unjittered || sNow.width == 0 || sNow.height == 0
            || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return false;
        }
        ndc_x = 2.f * sNow.jitterX / (F32)sNow.width;
        ndc_y = 2.f * sNow.jitterY / (F32)sNow.height;
        return true;
    }

    void temporalAlphaBefore(LLRenderTarget* screen)
    {
        if (!maskStagesWanted(screen))
        {
            return;
        }
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        LL::RHI::Texture* frame = screen->getRHITexture(0);
        if (!device || !frame)
        {
            return;
        }
        static LLCachedControl<bool> blended_only(gSavedSettings, "LuminousMetalFXTemporalMaskBlendedOnly", true);
        LL::RHI::Texture* depth = gPipeline.mMainRT.deferredScreen.getRHIDepth();
        if (blended_only() && depth)
        {
            const LL::RHI::TextureDesc& dd = depth->desc();
            if (sBeforeDepth.isNull() || sBeforeDepth->desc().format != dd.format || sBeforeDepth->desc().width != dd.width
                || sBeforeDepth->desc().height != dd.height)
            {
                sBeforeDepth = makeTexture(*device, dd.format, dd.width, dd.height, LL::RHI::TEXTURE_SAMPLED,
                                           "MetalFX depth before alpha");
            }
            if (sBeforeDepth.notNull())
            {
                LL::RHI::Region depth_region;
                depth_region.width = dd.width;
                depth_region.height = dd.height;
                device->encoder().copyTexture(depth, 0, 0, depth_region, sBeforeDepth.get(), 0, 0, 0, 0);
            }
        }
        const LL::RHI::TextureDesc& d = frame->desc();
        if (sBefore.isNull() || sBefore->desc().format != d.format || sBefore->desc().width != d.width
            || sBefore->desc().height != d.height)
        {
            sBefore = makeTexture(*device, d.format, d.width, d.height, LL::RHI::TEXTURE_SAMPLED, "MetalFX before alpha");
            if (sBefore.isNull())
            {
                return;
            }
        }
        LL::RHI::Region region;
        region.width = d.width;
        region.height = d.height;
        device->encoder().copyTexture(frame, 0, 0, region, sBefore.get(), 0, 0, 0, 0);
        sNow.copied = true;
    }

    void temporalAlphaAfter(LLRenderTarget* screen)
    {
        if (!sNow.copied || !maskStagesWanted(screen) || sBefore.isNull())
        {
            return;
        }
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        LL::RHI::Texture* frame = screen->getRHITexture(0);
        if (!device || !frame)
        {
            return;
        }
        LL::RHI::Texture* depth = gPipeline.mMainRT.deferredScreen.getRHIDepth();
        if (!depth)
        {
            return;
        }
        LL_PROFILE_GPU_ZONE("luminous temporal reactive");
        static LLCachedControl<F32> strength(gSavedSettings, "LuminousMetalFXTemporalMaskStrength", 0.5f);
        static LLCachedControl<F32> fade(gSavedSettings, "LuminousMetalFXTemporalMaskFade", 30.f);
        static LLCachedControl<bool> blended_only(gSavedSettings, "LuminousMetalFXTemporalMaskBlendedOnly", true);
        const glm::mat4 projection = get_current_projection();
        LuminousTemporalMaskConstants constants{};
        constants.strength = llclamp((F32)strength(), 0.f, 1.f);
        constants.fadeEnd = llmax((F32)fade(), 0.f);
        constants.fadeStart = constants.fadeEnd * 0.5f;
        const bool depth_test = blended_only() && sBeforeDepth.notNull()
            && sBeforeDepth->desc().width == depth->desc().width && sBeforeDepth->desc().height == depth->desc().height;
        constants.blendedOnly = depth_test ? 1.f : 0.f;
        constants.projA = projection[2][2];
        constants.projB = projection[3][2];
        const U32 stage = sNow.maskStages;
        LL::RHI::Texture* mask = sMask[stage % 2].get();
        LL::RHI::Texture* last = stage > 0 ? sMask[(stage - 1) % 2].get() : nullptr;
        LL::RHI::Texture* before = sBefore.get();
        LL::RHI::Texture* before_depth = depth_test ? sBeforeDepth.get() : depth;
        const bool drawn = drawInto(*device, mask, last ? REACTIVE_MORE_FRAGMENT : REACTIVE_FIRST_FRAGMENT,
                                    "luminous temporal reactive", [&](LL::RHI::Encoder& encoder)
            {
                LL::RHI::setNativeConstants(encoder, LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_BUFFER_TEMPORAL,
                                            constants);
                encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_FRAME, frame);
                encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_BEFORE, before);
                encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_DEPTH, depth);
                encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_BEFORE_DEPTH, before_depth);
                if (last)
                {
                    encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_MASK, last);
                }
            });
        if (drawn)
        {
            ++sNow.maskStages;
        }
        sNow.copied = false;
    }

    void temporalAfterPresent()
    {
        sNow.presented = true;
        if (!sNow.jittered || sNow.unjittered || sNow.width == 0 || sNow.height == 0)
        {
            return;
        }
        sNow.unjittered = true;
        // The jitter taken off the projection the rest of the frame and the next idle see: the interface drawn over
        // the world, name tags and hover text stay where they are.
        const glm::mat4 jittered = get_current_projection();
        const glm::mat4 unjitter = glm::translate(glm::mat4(1.f), glm::vec3(-2.f * sNow.jitterX / (F32)sNow.width,
                                                                           -2.f * sNow.jitterY / (F32)sNow.height, 0.f));
        const glm::mat4 still = unjitter * jittered;
        if (gGL.getProjectionMatrix() == jittered)
        {
            gGL.matrixMode(LLRender::MM_PROJECTION);
            gGL.loadMatrix(glm::value_ptr(still));
            gGL.matrixMode(LLRender::MM_MODELVIEW);
        }
        set_current_projection(still);
    }

    namespace
    {
        // An overlay can be drawn after this frame's present: the frame is drawn for the temporal scaler, the library
        // is ready, and the overlay's pipeline has compiled for the window's back buffer. Until it has, the frame
        // draws it as before.
        bool overlayReady(const char* fragment)
        {
            if (!metal() || !(sNow.on || sNow.overlays) || gSnapshot || gCubeSnapshot || !usable())
            {
                return false;
            }
            LL::RHI::Device* device = LL::RHI::viewerDevice();
            LL::RHI::Surface* surface = device ? device->surface() : nullptr;
            LL::RHI::Texture* back = surface ? surface->backBuffer() : nullptr;
            return back && readyOverlayPipeline(fragment, back->desc().format);
        }
    }

    bool overlayPictureFrame(F32 picture_aspect)
    {
        if (picture_aspect <= 0.f || !overlayReady(OVERLAY_PICTURE_FRAGMENT))
        {
            return false;
        }
        sOverlay.picture = true;
        sOverlay.pictureAspect = picture_aspect;
        return true;
    }

    bool overlayCaptureFrame(F32 x0, F32 y0, F32 x1, F32 y1, const F32* colour, F32 border)
    {
        if (!colour || !overlayReady(OVERLAY_CAPTURE_FRAGMENT))
        {
            return false;
        }
        // A capture frame of the world view's own shape holds every pixel's centre: nothing would change, so no copy
        // and no pass.
        const F32 w = (F32)gGLViewport[2];
        const F32 h = (F32)gGLViewport[3];
        if (x0 * w <= 0.5f && y0 * h <= 0.5f && x1 * w >= w - 0.5f && y1 * h >= h - 0.5f)
        {
            return true;
        }
        sOverlay.capture = true;
        sOverlay.captureRect[0] = x0;
        sOverlay.captureRect[1] = y0;
        sOverlay.captureRect[2] = x1;
        sOverlay.captureRect[3] = y1;
        sOverlay.captureColour[0] = colour[0];
        sOverlay.captureColour[1] = colour[1];
        sOverlay.captureColour[2] = colour[2];
        sOverlay.captureBorder = border;
        return true;
    }

    void drawOverlays()
    {
        const OverlayFrame overlay = sOverlay;
        sOverlay = OverlayFrame();
        if (!overlay.capture && !gSnapshot && !gCubeSnapshot)
        {
            sOverlayCopy = nullptr; // made again when the capture frame shows; kept across a snapshot's frames
        }
        if (!overlay.picture && !overlay.capture)
        {
            return;
        }
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        if (!device || !usable() || !LLRenderTarget::beginStackPass())
        {
            return;
        }
        LL::RHI::RenderPassDesc window;
        if (!LLRenderTarget::currentPass(window) || window.colorCount < 1 || window.color[0].texture.isNull())
        {
            return;
        }
        LL::RHI::Texture* back = window.color[0].texture.get();
        const LL::RHI::TextureDesc& back_desc = back->desc();
        // The world view, as the present drew it.
        const S32 x = gGLViewport[0];
        const S32 y = gGLViewport[1];
        const S32 w = gGLViewport[2];
        const S32 h = gGLViewport[3];
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || U32(x + w) > back_desc.width || U32(y + h) > back_desc.height)
        {
            return;
        }
        LL::RHI::Pipeline* capture = overlay.capture ? readyOverlayPipeline(OVERLAY_CAPTURE_FRAGMENT, back_desc.format) : nullptr;
        LL::RHI::Pipeline* picture = overlay.picture ? readyOverlayPipeline(OVERLAY_PICTURE_FRAGMENT, back_desc.format) : nullptr;
        // The capture frame's margin is a blur of the picture round each pixel, so it reads a copy of the world view,
        // made before the pass: a copy breaks the window's stack pass, as MetalFX's scaler does.
        if (capture)
        {
            if (sOverlayCopy.isNull() || sOverlayCopy->desc().format != back_desc.format
                || sOverlayCopy->desc().width != (U32)w || sOverlayCopy->desc().height != (U32)h)
            {
                sOverlayCopy = makeTexture(*device, back_desc.format, (U32)w, (U32)h, LL::RHI::TEXTURE_SAMPLED,
                                           "luminous overlay copy");
            }
            if (sOverlayCopy.isNull())
            {
                capture = nullptr;
            }
            else
            {
                LL::RHI::Region region;
                region.x = (U32)x;
                region.y = (U32)y;
                region.width = (U32)w;
                region.height = (U32)h;
                device->encoder().copyTexture(back, window.color[0].level, window.color[0].layer, region,
                                              sOverlayCopy.get(), 0, 0, 0, 0);
            }
        }
        if (!capture && !picture)
        {
            return;
        }

        // The rects in the world view's pixels: the capture frame's as Firestorm's pass scales its own to the frame,
        // the picture's as drawPictureFrame finds it, for a frame of the world view's size.
        const F32 width = (F32)w;
        const F32 height = (F32)h;
        LuminousOverlayConstants constants{};
        constants.origin = simd_make_float2((F32)x, (F32)y);
        constants.captureRect = simd_make_float4(overlay.captureRect[0] * width, overlay.captureRect[1] * height,
                                                 overlay.captureRect[2] * width, overlay.captureRect[3] * height);
        constants.captureBorder = simd_make_float4(overlay.captureColour[0], overlay.captureColour[1],
                                                   overlay.captureColour[2], overlay.captureBorder);
        const F32 frame_aspect = width / height;
        const F32 aspect = overlay.pictureAspect;
        const F32 pw = aspect < frame_aspect ? aspect / frame_aspect : 1.f;
        const F32 ph = aspect < frame_aspect ? 1.f : frame_aspect / aspect;
        constants.pictureRect = simd_make_float4((0.5f - 0.5f * pw) * width, (0.5f - 0.5f * ph) * height,
                                                 (0.5f + 0.5f * pw) * width, (0.5f + 0.5f * ph) * height);

        LL_PROFILE_GPU_ZONE("luminous overlays");
        LL::RHI::RenderPassDesc pass;
        pass.native = true;
        pass.name = "luminous overlays";
        pass.colorCount = 1;
        pass.color[0].texture = window.color[0].texture;
        pass.color[0].level = window.color[0].level;
        pass.color[0].layer = window.color[0].layer;
        pass.color[0].load = LL::RHI::LoadAction::Load;
        pass.color[0].store = LL::RHI::StoreAction::Store;
        LL::RHI::Encoder& encoder = device->encoder();
        NativePass native(encoder, pass); // sets the window's stack pass aside; no depth attached, none written
        if (!native.begun())
        {
            return;
        }
        encoder.setViewport(x, y, w, h);
        if (capture)
        {
            encoder.setPipeline(capture);
            LL::RHI::setNativeConstants(encoder, LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_BUFFER_OVERLAY, constants);
            encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_OVERLAY, sOverlayCopy.get());
            encoder.draw(LL::RHI::PrimitiveType::Triangles, 0, 3);
        }
        if (picture)
        {
            encoder.setPipeline(picture);
            LL::RHI::setNativeConstants(encoder, LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_BUFFER_OVERLAY, constants);
            encoder.draw(LL::RHI::PrimitiveType::Triangles, 0, 3);
        }
    }

    namespace
    {
        // This frame's jitter in the live view's texture coordinates, while the main camera is jittered and its
        // present has not been drawn.
        bool jitterUV(F32& u, F32& v)
        {
            u = 0.f;
            v = 0.f;
            if (!sNow.on || !sNow.jittered || sNow.unjittered || sNow.width == 0 || sNow.height == 0
                || gPipeline.mRT != &gPipeline.mMainRT)
            {
                return false;
            }
            u = sNow.jitterX / (F32)sNow.width;
            v = sNow.jitterY / (F32)sNow.height;
            return true;
        }

        // A layer (now, target_width x target_height texels, its texels width x height in the lower left) averaged with
        // the frames before: the texture holding the new average (s.average[s.last]), or now as it is when the settle is
        // off or cannot draw. react: where else this frame is taken whole.
        LL::RHI::Texture* settleTexture(Settled& s, LL::RHI::Texture* now, U32 target_width, U32 target_height, U32 width,
                                        U32 height, F32 frames, React react, const char* name)
        {
            F32 u = 0.f;
            F32 v = 0.f;
            if (frames < SETTLE_FRAMES_LEAST || !metal() || sHolding || !now || !jitterUV(u, v) || width == 0 || height == 0
                || !usable())
            {
                return now;
            }
            LL::RHI::Device* device = LL::RHI::viewerDevice();
            LL::RHI::Texture* depth = gPipeline.mMainRT.deferredScreen.getRHIDepth();
            if (!device || !depth)
            {
                return now;
            }
            bool reset = s.frame == 0 || s.frame + 1 != sNow.frame || s.width != width || s.height != height
                         || sResetAsked || !sHaveLast || sLastTemporalFrame + 1 != sNow.frame;
            for (LLRenderTarget& average : s.average)
            {
                if (average.getWidth() != target_width || average.getHeight() != target_height)
                {
                    if (!average.allocate(target_width, target_height, GL_RGBA16F))
                    {
                        releaseSettled(s);
                        return now;
                    }
                    reset = true;
                }
            }
            // A teleport, another region or a camera cut, as historyGoes finds them at the present.
            if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || gTeleportDisplay
                || (gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0) != sLastRegion)
            {
                reset = true;
            }
            const LLViewerCamera* camera = LLViewerCamera::getInstance();
            const F32 turn = std::acos(llclamp(camera->getAtAxis() * sLastAt, -1.f, 1.f)) * RAD_TO_DEG;
            if ((camera->getOrigin() - sLastEye).length() > TEMPORAL_JUMP_METRES || turn > TEMPORAL_JUMP_DEGREES)
            {
                reset = true;
            }

            LuminousTemporalSettleConstants constants{};
            const glm::dmat4 view(get_last_modelview());
            const glm::dmat4 unjitter = glm::translate(glm::dmat4(1.0), glm::dvec3(-2.0 * sNow.jitterX / (F64)sNow.width,
                                                                                    -2.0 * sNow.jitterY / (F64)sNow.height,
                                                                                    0.0));
            const glm::dmat4 now_still = unjitter * glm::dmat4(get_last_projection()) * view;
            const glm::dmat4 to_last = sLastViewProjection * glm::inverse(now_still);
            for (int c = 0; c < 4; ++c)
            {
                constants.toLast.columns[c] = simd_make_float4((float)to_last[c][0], (float)to_last[c][1],
                                                               (float)to_last[c][2], (float)to_last[c][3]);
            }
            constants.region = simd_make_float2((float)width, (float)height);
            constants.lastSize = simd_make_float2((float)target_width, (float)target_height);
            constants.depthJitter = simd_make_float2(u, v);
            constants.frameSize = simd_make_float2((float)sNow.width, (float)sNow.height);
            constants.share = 1.f / llclamp(frames, 1.f, 64.f);
            constants.motionFull = SETTLE_MOTION_FULL;
            constants.reset = reset ? 1.f : 0.f;
            constants.react = (F32)react;

            LLRenderTarget& out = s.average[1 - s.last];
            LL::RHI::Texture* last = s.average[s.last].getRHITexture(0);
            LL::RHI::Texture* colour = out.getRHITexture(0);
            if (!last || !colour)
            {
                return now;
            }
            LL::RHI::Pipeline* pipeline = readyPipeline(SETTLE_FRAGMENT, colour->desc().format, LL::RHI::Format::Unknown);
            if (!pipeline)
            {
                return now;
            }
            LL::RHI::RenderPassDesc pass;
            pass.native = true;
            pass.name = name;
            pass.colorCount = 1;
            pass.color[0].texture = colour;
            pass.color[0].load = LL::RHI::LoadAction::DontCare; // the layer's texels are drawn, the rest never read
            pass.color[0].store = LL::RHI::StoreAction::Store;
            LL::RHI::Encoder& encoder = device->encoder();
            {
                NativePass native(encoder, pass);
                if (!native.begun())
                {
                    return now;
                }
                encoder.setPipeline(pipeline);
                encoder.setViewport(0, 0, (S32)width, (S32)height);
                LL::RHI::setNativeConstants(encoder, LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_BUFFER_TEMPORAL, constants);
                encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_FRAME, now);
                encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_BEFORE, last);
                encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_DEPTH, depth);
                encoder.draw(LL::RHI::PrimitiveType::Triangles, 0, 3);
            }
            s.last = 1 - s.last;
            s.frame = sNow.frame;
            s.width = width;
            s.height = height;
            return colour;
        }

        // The same for a layer in one of Firestorm's targets: the target holding the new average, or layer as it is.
        LLRenderTarget* settle(Settled& s, LLRenderTarget* layer, U32 width, U32 height, F32 frames, const char* name)
        {
            LL::RHI::Texture* now = layer ? layer->getRHITexture(0) : nullptr;
            if (!now)
            {
                return layer;
            }
            LL::RHI::Texture* settled = settleTexture(s, now, layer->getWidth(), layer->getHeight(), width, height, frames,
                                                      React::Change, name);
            return settled == now ? layer : &s.average[s.last];
        }

        F32 settleFrames(SettledLayer layer)
        {
            static LLCachedControl<F32> air(gSavedSettings, "LuminousMetalFXTemporalSettleAir", 8.f);
            static LLCachedControl<F32> flare(gSavedSettings, "LuminousMetalFXTemporalSettleFlare", 8.f);
            static LLCachedControl<F32> enhance(gSavedSettings, "LuminousMetalFXTemporalSettleEnhance", 8.f);
            switch (layer)
            {
                case SettledLayer::Air:     return air();
                case SettledLayer::Enhance: return enhance();
                default:                    return flare();
            }
        }
    }

    LLRenderTarget* temporalSettleDoF(LLRenderTarget* blur, U32 width, U32 height)
    {
        static LLCachedControl<F32> frames(gSavedSettings, "LuminousMetalFXTemporalSettleDoF", 8.f);
        return settle(sSettledDoF, blur, width, height, frames(), "luminous temporal settle depth of field");
    }

    LLRenderTarget* temporalSettleGlow(LLRenderTarget* glow)
    {
        static LLCachedControl<F32> frames(gSavedSettings, "LuminousMetalFXTemporalSettleGlow", 8.f);
        return glow ? settle(sSettledGlow, glow, glow->getWidth(), glow->getHeight(), frames(),
                             "luminous temporal settle glow")
                    : glow;
    }

    void releaseSettledTargets()
    {
        releaseSettled(sSettledDoF);
        releaseSettled(sSettledGlow);
        for (Settled& settled : sSettledLayers)
        {
            releaseSettled(settled);
        }
    }

    bool temporalSettles(SettledLayer layer, F32& u, F32& v)
    {
        if (layer >= SettledLayer::Count || settleFrames(layer) < SETTLE_FRAMES_LEAST || !jitterUV(u, v) || sHolding
            || !usable())
        {
            u = 0.f;
            v = 0.f;
            return false;
        }
        return true;
    }

    LL::RHI::Texture* temporalSettle(SettledLayer layer, LL::RHI::Texture* texture, U32 width, U32 height)
    {
        static const char* const NAMES[] = { "luminous temporal settle air", "luminous temporal settle flare light",
                                             "luminous temporal settle flare sun", "luminous temporal settle enhance" };
        static_assert(std::size(NAMES) == size_t(SettledLayer::Count), "a name per settled layer");
        if (layer >= SettledLayer::Count || !texture)
        {
            return texture;
        }
        return settleTexture(sSettledLayers[size_t(layer)], texture, texture->desc().width, texture->desc().height, width,
                             height, settleFrames(layer), layer == SettledLayer::Enhance ? React::Weight : React::Camera,
                             NAMES[size_t(layer)]);
    }

    bool temporalSettlesGlow(F32& u, F32& v)
    {
        static LLCachedControl<F32> frames(gSavedSettings, "LuminousMetalFXTemporalSettleGlow", 8.f);
        if (frames() < SETTLE_FRAMES_LEAST || !jitterUV(u, v))
        {
            u = 0.f;
            v = 0.f;
            return false;
        }
        return true;
    }

    bool temporalSettlesDoF(F32& u, F32& v)
    {
        static LLCachedControl<F32> frames(gSavedSettings, "LuminousMetalFXTemporalSettleDoF", 8.f);
        if (frames() < SETTLE_FRAMES_LEAST || !jitterUV(u, v))
        {
            u = 0.f;
            v = 0.f;
            return false;
        }
        return true;
    }

    void resetTemporalHistory()
    {
        sResetAsked = true;
        sHoldBreak = "the history was reset";
    }

    void temporalSceneMapCopied()
    {
        liveJitterApplied(sSceneMapJitterX, sSceneMapJitterY); // 0, 0 without
    }

    void ssrJitterOffset(F32& ndc_x, F32& ndc_y)
    {
        ndc_x = 0.f;
        ndc_y = 0.f;
        if (!metal() || gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return;
        }
        F32 now_x = 0.f;
        F32 now_y = 0.f;
        liveJitterApplied(now_x, now_y);
        ndc_x = sSceneMapJitterX - now_x;
        ndc_y = sSceneMapJitterY - now_y;
    }

    void liveViewNotScaled()
    {
        if (sTemporal.notNull() || sTemporalNext)
        {
            releaseTemporal();
        }
        sTemporalPending.reset();
    }

    void registerTone(PassTable& table)
    {
        table.fn[size_t(Pass::PictureFrame)] = drawPictureFrame;
        table.fn[size_t(Pass::Upscale)] = drawUpscale;
        if (metal())
        {
            LL::RHI::onViewerDeviceRelease(onDeviceRelease);
            // Switched off, or another preview resolution: the scaler and its frame go at once; the next frame that
            // needs them makes them again.
            for (const char* setting : { "LuminousMetalFXTemporal", "LuminousMetalFXTemporalProbe", "LuminousPhotoLiveScale" })
            {
                if (LLControlVariable* control = gSavedSettings.getControl(setting))
                {
                    control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { resetMetalFX(); });
                }
            }
            // A change of any Luminous or render setting ends a hold at the next frame (beginTemporalFrame): the
            // effects' and the Look's settings, styles, Preferences.
            struct HoldBreaker : public LLControlGroup::ApplyFunctor
            {
                void apply(const std::string& name, LLControlVariable* control) override
                {
                    if (control && (name.rfind("Luminous", 0) == 0 || name.rfind("Render", 0) == 0))
                    {
                        control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
                                                      { sHoldBreak = "a setting changed"; });
                    }
                }
            };
            HoldBreaker breaker;
            gSavedSettings.applyToAll(&breaker);
            startPolling();
        }
    }
}
