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

#include "llcallbacklist.h"
#include "llnativeprogram.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llstartup.h"
#include "llviewerdisplay.h"

// The tone and finish group's own passes on Metal:
// - PictureFrame: the picture's part of the finished live view, for a render in a shape of its own; the rest
//   dimmed and a thin line along the picture's edge, drawn into the next post-processing target.
// - Upscale: the present of a live view rendered smaller than the window, scaled up with Catmull-Rom into
//   the window's back buffer, with the frame's depth for the interface drawn over it.
// The Look, grain, Worn photo and the light leak are drawn inside Firestorm's tonemap and sharpening
// programs (hosted), translated to Metal with them; they are not drawn here.
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
            }
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
            LL::RHI::Pipeline* pipeline = write_depth
                ? readyPipeline(PRESENT_FRAGMENT, window.color[0].texture->desc().format, window.depth.texture->desc().format)
                : readyPipeline(PRESENT_NO_DEPTH_FRAGMENT, window.color[0].texture->desc().format, LL::RHI::Format::Unknown);
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
            encoder.setNativeTexture(LL::RHI::NativeStage::Fragment, LUMINOUS_TONE_TEXTURE_FRAME, source);
            encoder.draw(LL::RHI::PrimitiveType::Triangles, 0, 3);
            return true;
        }
    }

    void registerTone(PassTable& table)
    {
        table.fn[size_t(Pass::PictureFrame)] = drawPictureFrame;
        table.fn[size_t(Pass::Upscale)] = drawUpscale;
        if (metal())
        {
            LL::RHI::onViewerDeviceRelease(onDeviceRelease);
            startPolling();
        }
    }
}
