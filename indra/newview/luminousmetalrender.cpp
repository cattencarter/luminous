/**
 * @file luminousmetalrender.cpp
 * @brief Photo mode on Metal: renders and output, the group's native passes.
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

#include "luminousmetalrender.h"

#include "luminousmetal.h"
#include "luminousmetalparams.h"

#include "llcallbacklist.h"
#include "llnativeprogram.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llstartup.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <span>

// A render's frames on Metal, as native passes: render/render.metal says what each does.
// - AverageFrames: each frame of a render of several added at 1 / frames into the float sum LuminousHDRSnapshot
//   keeps (its LLRenderTarget, so that a sixteen-bit TIFF can be tonemapped into it afterwards, as on OpenGL), the
//   first frame's pass clearing it; the last frame's pass adds it and writes the sum into the HDR frame's colour.
//   The sum is the pass's attachment, read through it: no blending, so a 32-bit float sum works as a half one.
// - HDRCapture: the frame read into a texture of the picture's size (the margin cut away, box-filtered from a
//   render made larger), and that texture read back asynchronously; the reads are summed on the CPU as they come
//   in, and waited for once, after the frames. The light for OpenEXR comes back as half float at the render's
//   exposure; the finished picture for TIFF as 32-bit float.
namespace
{
    namespace RHI = LL::RHI;
    using LuminousMetal::Pass;

    const char* const MANIFEST = "luminous/render/programs.json";
    const char* const LIBRARY = "render";
    const char* const FULLSCREEN_VS = "luminous_fullscreen_vs";
    const char* const ADD_FS = "luminous_average_add_fs";
    const char* const LAST_FS = "luminous_average_last_fs";
    const char* const RESOLVE = "luminous_render_resolve";

    // One frame read, not yet added to the sum.
    struct PendingRead
    {
        RHI::Ref<RHI::Readback> picture;
        RHI::Ref<RHI::Readback> exposure;
        RHI::Format exposureFormat = RHI::Format::Unknown;
        U32 width = 0;
        U32 height = 0;
        bool light = false;
    };

    struct State
    {
        LLNativeProgram program;
        bool loaded = false;
        bool failed = false;
        bool ready = false;

        // The read asked for by LuminousHDRSnapshot, taken by the next HDRCapture.
        LuminousMetalRender::Read read;
        bool readAsked = false;

        // The picture-sized texture frames are read into, by its size and format.
        RHI::Ref<RHI::TransientPool> picture;
        U32 pictureWidth = 0;
        U32 pictureHeight = 0;
        RHI::Format pictureFormat = RHI::Format::Unknown;

        // The reads of this render: in flight, and the sum of those come in.
        std::vector<PendingRead> pending;
        std::vector<F32> sum;
        U32 sumWidth = 0;
        U32 sumHeight = 0;
        S32 recorded = 0;
        S32 summed = 0;
        F32 metered = 1.f;
        std::string error;

        void releaseRender()
        {
            pending.clear();
            std::vector<F32>().swap(sum);
            sumWidth = sumHeight = 0;
            recorded = summed = 0;
            metered = 1.f;
            error.clear();
            readAsked = false;
            picture = nullptr;
            pictureWidth = pictureHeight = 0;
            pictureFormat = RHI::Format::Unknown;
        }
    };

    State& state()
    {
        static State s;
        return s;
    }

    RHI::PipelineDesc addDesc(RHI::Format sum)
    {
        RHI::PipelineDesc desc;
        desc.colorCount = 1;
        desc.colorFormats[0] = sum;
        return desc;
    }

    // The last frame's pass writes colour only, in both: the frame's alpha is read further on.
    RHI::PipelineDesc lastDesc(RHI::Format sum, RHI::Format frame)
    {
        RHI::PipelineDesc desc;
        desc.colorCount = 2;
        desc.colorFormats[0] = sum;
        desc.colorFormats[1] = frame;
        desc.colorMask = RHI::COLOR_MASK_R | RHI::COLOR_MASK_G | RHI::COLOR_MASK_B;
        return desc;
    }

    void forgetDevice()
    {
        State& s = state();
        s.releaseRender();
        s.program.release();
        s.loaded = s.failed = s.ready = false;
        LuminousMetal::setReady(Pass::AverageFrames, false);
        LuminousMetal::setReady(Pass::HDRCapture, false);
    }

    // Each idle frame until the library's pipelines are ready: loads the library once the viewer is in the world
    // on Metal, then says the passes are ready when every pipeline has compiled.
    bool pollLibrary()
    {
        State& s = state();
        if (!LuminousMetal::metal())
        {
            return true; // never on OpenGL
        }
        if (s.failed || s.ready || LLStartUp::getStartupState() < STATE_STARTED || !LuminousMetal::photoModeOnMetal())
        {
            return false;
        }
        RHI::Device* device = RHI::viewerDevice();
        if (!device || !device->capabilities().computePasses || !device->capabilities().nativePrograms)
        {
            return false;
        }
        if (!s.loaded)
        {
            s.loaded = true;
            if (!s.program.load(MANIFEST, LIBRARY))
            {
                s.failed = true; // load() has said why
            }
            return false;
        }
        const RHI::CompileStatus status = s.program.status();
        if (status == RHI::CompileStatus::Failed)
        {
            LL_WARNS("Luminous") << "Renders on Metal: their library did not compile: " << s.program.log() << LL_ENDL;
            s.failed = true;
            return false;
        }
        if (status != RHI::CompileStatus::Ready)
        {
            return false;
        }
        bool all = true;
        RHI::ComputePipeline* resolve = s.program.compute(RESOLVE);
        if (!resolve || resolve->status() == RHI::CompileStatus::Failed)
        {
            LL_WARNS("Luminous") << "Renders on Metal: kernel " << RESOLVE << " did not compile" << LL_ENDL;
            s.failed = true;
            return false;
        }
        all = all && resolve->status() == RHI::CompileStatus::Ready;
        // The HDR frame is RGBA16F; another format gets its own pipelines at its first draw.
        const std::array<RHI::Pipeline*, 4> pipelines = {
            s.program.render(FULLSCREEN_VS, ADD_FS, addDesc(RHI::Format::RGBA16Float)),
            s.program.render(FULLSCREEN_VS, ADD_FS, addDesc(RHI::Format::RGBA32Float)),
            s.program.render(FULLSCREEN_VS, LAST_FS, lastDesc(RHI::Format::RGBA16Float, RHI::Format::RGBA16Float)),
            s.program.render(FULLSCREEN_VS, LAST_FS, lastDesc(RHI::Format::RGBA32Float, RHI::Format::RGBA16Float)),
        };
        for (RHI::Pipeline* pipeline : pipelines)
        {
            if (!pipeline || pipeline->status() == RHI::CompileStatus::Failed)
            {
                LL_WARNS("Luminous") << "Renders on Metal: an averaging pipeline did not compile" << LL_ENDL;
                s.failed = true;
                return false;
            }
            all = all && pipeline->status() == RHI::CompileStatus::Ready;
        }
        if (all)
        {
            s.ready = true;
            LuminousMetal::setReady(Pass::AverageFrames, true);
            LuminousMetal::setReady(Pass::HDRCapture, true);
            LL_INFOS("Luminous") << "Renders on Metal: native passes ready" << LL_ENDL;
        }
        return false;
    }

    // IEEE half to float.
    F32 halfToFloat(U16 h)
    {
        const U32 sign = (U32)(h & 0x8000u) << 16;
        U32 exponent = (h >> 10) & 0x1fu;
        U32 mantissa = h & 0x3ffu;
        U32 bits = 0;
        if (exponent == 0)
        {
            if (mantissa != 0)
            {
                // subnormal: normalised into float's range
                exponent = 127 - 15 + 1;
                while (!(mantissa & 0x400u))
                {
                    mantissa <<= 1;
                    --exponent;
                }
                mantissa &= 0x3ffu;
                bits = sign | (exponent << 23) | (mantissa << 13);
            }
            else
            {
                bits = sign;
            }
        }
        else if (exponent == 0x1f)
        {
            bits = sign | 0x7f800000u | (mantissa << 13);
        }
        else
        {
            bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
        }
        F32 value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    F32 readExposure(PendingRead& read)
    {
        if (read.exposure.isNull())
        {
            return 1.f;
        }
        read.exposure->wait();
        const std::span<const std::byte> data = read.exposure->data();
        if (read.exposureFormat == RHI::Format::R16Float && data.size() >= sizeof(U16))
        {
            U16 h;
            memcpy(&h, data.data(), sizeof(h));
            return halfToFloat(h);
        }
        if (read.exposureFormat == RHI::Format::R32Float && data.size() >= sizeof(F32))
        {
            F32 f;
            memcpy(&f, data.data(), sizeof(f));
            return f;
        }
        return 1.f;
    }

    // Adds one read to the sum, waiting for it if it is not in yet.
    void addRead(PendingRead& read)
    {
        State& s = state();
        read.picture->wait();
        const std::span<const std::byte> data = read.picture->data();
        const size_t texel = read.light ? 4 * sizeof(U16) : 4 * sizeof(F32);
        const size_t pitch = read.picture->rowPitch() ? read.picture->rowPitch() : read.width * texel;
        if (data.size() < pitch * (read.height - 1) + read.width * texel)
        {
            s.error = "the frame could not be read back";
            return;
        }
        if (s.sum.empty() || s.sumWidth != read.width || s.sumHeight != read.height)
        {
            if (!s.sum.empty())
            {
                s.error = "the frames were read at different sizes";
                return;
            }
            try
            {
                s.sum.assign((size_t)read.width * read.height * 3, 0.f);
            }
            catch (const std::bad_alloc&)
            {
                s.error = "not enough memory to read the frame";
                return;
            }
            s.sumWidth = read.width;
            s.sumHeight = read.height;
        }
        for (U32 y = 0; y < read.height; ++y)
        {
            const std::byte* row = data.data() + pitch * y;
            F32* out = &s.sum[(size_t)y * read.width * 3];
            if (read.light)
            {
                for (U32 x = 0; x < read.width; ++x)
                {
                    U16 h[4];
                    memcpy(h, row + x * texel, sizeof(h));
                    out[x * 3 + 0] += halfToFloat(h[0]);
                    out[x * 3 + 1] += halfToFloat(h[1]);
                    out[x * 3 + 2] += halfToFloat(h[2]);
                }
            }
            else
            {
                for (U32 x = 0; x < read.width; ++x)
                {
                    F32 f[4];
                    memcpy(f, row + x * texel, sizeof(f));
                    out[x * 3 + 0] += f[0];
                    out[x * 3 + 1] += f[1];
                    out[x * 3 + 2] += f[2];
                }
            }
        }
        s.metered = readExposure(read);
        ++s.summed;
    }

    // The reads that are in, in order, added to the sum; all of them when wait is true. Each read's memory goes as
    // soon as it is added.
    void harvest(bool wait)
    {
        State& s = state();
        size_t done = 0;
        for (; done < s.pending.size(); ++done)
        {
            PendingRead& read = s.pending[done];
            if (!wait && (!read.picture->ready() || (read.exposure.notNull() && !read.exposure->ready())))
            {
                break;
            }
            if (s.error.empty())
            {
                addRead(read);
            }
        }
        s.pending.erase(s.pending.begin(), s.pending.begin() + done);
    }

    // The picture-sized texture the frame is read into.
    RHI::Texture* pictureTexture(RHI::Device& device, U32 width, U32 height, RHI::Format format)
    {
        State& s = state();
        if (s.picture.isNull() || s.pictureWidth != width || s.pictureHeight != height || s.pictureFormat != format)
        {
            RHI::TransientPoolDesc pool;
            pool.name = "luminous render picture";
            RHI::TransientTextureDesc texture;
            texture.desc.format = format;
            texture.desc.width = width;
            texture.desc.height = height;
            texture.desc.usage = RHI::TEXTURE_STORAGE | RHI::TEXTURE_READBACK;
            texture.name = "luminous render picture";
            pool.textures.push_back(texture);
            s.picture = device.createTransientPool(pool);
            s.pictureWidth = width;
            s.pictureHeight = height;
            s.pictureFormat = format;
        }
        return s.picture.notNull() ? s.picture->texture(0) : nullptr;
    }

    // The frames averaged as light: frame and other are the HDR frame and the float sum, of one size.
    bool drawAverage(LuminousMetal::PassContext& /*context*/, LuminousMetal::PassTargets& targets)
    {
        State& s = state();
        RHI::Device* device = RHI::viewerDevice();
        if (!s.ready || !device || !targets.frame || !targets.other)
        {
            return false;
        }
        RHI::Texture* frame = targets.frame->getRHITexture(0);
        RHI::Texture* sum = targets.other->getRHITexture(0);
        if (!frame || !sum || frame->desc().width != sum->desc().width || frame->desc().height != sum->desc().height)
        {
            return false;
        }
        LuminousRenderParams params;
        LuminousPhotoMode::fillRenderParams(params);
        if (params.frames < 2 || params.frame < 0)
        {
            return false;
        }
        const bool first = params.frame == 0;
        const bool last = params.frame >= params.frames - 1;
        RHI::Pipeline* pipeline = last ? s.program.render(FULLSCREEN_VS, LAST_FS, lastDesc(sum->desc().format, frame->desc().format))
                                       : s.program.render(FULLSCREEN_VS, ADD_FS, addDesc(sum->desc().format));
        if (!pipeline || pipeline->status() == RHI::CompileStatus::Failed)
        {
            return false;
        }

        RHI::RenderPassDesc pass;
        pass.name = "luminous average frames";
        pass.native = true;
        pass.colorCount = last ? 2 : 1;
        // The first frame's pass clears the sum to nothing, whatever the viewer's clear colour is.
        pass.color[0].texture = sum;
        pass.color[0].load = first ? RHI::LoadAction::Clear : RHI::LoadAction::Load;
        pass.color[0].clear = { 0.f, 0.f, 0.f, 0.f };
        pass.color[0].store = RHI::StoreAction::Store;
        if (last)
        {
            pass.color[1].texture = frame;
            pass.color[1].load = RHI::LoadAction::Load;
            pass.color[1].store = RHI::StoreAction::Store;
        }
        RHI::Encoder& encoder = device->encoder();
        if (!encoder.beginRenderPass(pass))
        {
            return false;
        }
        const RHI::NativeStage fs = RHI::NativeStage::Fragment;
        encoder.setPipeline(pipeline);
        encoder.setViewport(0, 0, (S32)sum->desc().width, (S32)sum->desc().height);
        encoder.setScissor(false, 0, 0, 0, 0);
        encoder.setCullMode(RHI::CullMode::Off);
        RHI::setNativeConstants(encoder, fs, LUMINOUS_RENDER_BUFFER_PARAMS, params);
        if (!last)
        {
            encoder.setNativeTexture(fs, LUMINOUS_RENDER_TEXTURE_FRAME, frame, RHI::Access::Read, { 0, 1 });
        }
        encoder.draw(RHI::PrimitiveType::Triangles, 0, 3);
        encoder.endRenderPass();
        return true;
    }

    // One frame read into the picture: frame is what is read, the frame's exposure the metered one.
    bool drawRead(LuminousMetal::PassContext& context, LuminousMetal::PassTargets& targets)
    {
        State& s = state();
        RHI::Device* device = RHI::viewerDevice();
        if (!s.ready || !device || !s.readAsked || !targets.frame || !context.exposure)
        {
            return false;
        }
        s.readAsked = false;
        const LuminousMetalRender::Read& r = s.read;
        RHI::Texture* source = targets.frame->getRHITexture(0);
        RHI::Texture* exposure = context.exposure->getRHITexture(0);
        if (!source || !exposure || r.width == 0 || r.height == 0 || r.cutWidth == 0 || r.cutHeight == 0
            || r.renderWidth <= 0.f || r.renderHeight <= 0.f
            || (U64)r.cutX + r.cutWidth > source->desc().width || (U64)r.cutY + r.cutHeight > source->desc().height)
        {
            return false;
        }
        // What has come in so far is added now, so that no more than a frame or two wait in memory.
        harvest(false);

        const RHI::Format format = r.light ? RHI::Format::RGBA16Float : RHI::Format::RGBA32Float;
        RHI::Texture* picture = pictureTexture(*device, r.width, r.height, format);
        RHI::ComputePipeline* pipeline = s.program.compute(RESOLVE);
        if (!picture || !pipeline || pipeline->status() == RHI::CompileStatus::Failed)
        {
            return false;
        }

        LuminousRenderResolve constants{};
        constants.cutOrigin = simd_make_uint2(r.cutX, r.cutY);
        constants.cutSize = simd_make_uint2(r.cutWidth, r.cutHeight);
        constants.renderSize = simd_make_float2(r.renderWidth, r.renderHeight);
        constants.pictureSize = simd_make_uint2(r.width, r.height);
        constants.scale = r.light ? r.scale : 1.f;
        constants.exposure = r.light ? 1u : 0u;
        constants.halfRange = r.light ? 1u : 0u;

        RHI::Encoder& encoder = device->encoder();
        if (!encoder.beginComputePass({ "luminous render read" }))
        {
            return false;
        }
        const RHI::NativeStage cs = RHI::NativeStage::Compute;
        encoder.setComputePipeline(pipeline);
        RHI::setNativeConstants(encoder, cs, LUMINOUS_RENDER_BUFFER_PARAMS, constants);
        encoder.setNativeTexture(cs, LUMINOUS_RENDER_TEXTURE_SOURCE, source, RHI::Access::Read, { 0, 1 });
        encoder.setNativeTexture(cs, LUMINOUS_RENDER_TEXTURE_EXPOSURE, exposure, RHI::Access::Read, { 0, 1 });
        encoder.setNativeTexture(cs, LUMINOUS_RENDER_TEXTURE_PICTURE, picture, RHI::Access::Write, { 0, 1 });
        encoder.dispatchThreads({ r.width, r.height, 1 }, { LUMINOUS_RENDER_GROUP, LUMINOUS_RENDER_GROUP, 1 });
        encoder.endComputePass();

        PendingRead read;
        read.width = r.width;
        read.height = r.height;
        read.light = r.light;
        RHI::PixelLayout layout;
        layout.components = RHI::PixelLayout::Components::RGBA;
        layout.type = r.light ? RHI::PixelLayout::Type::F16 : RHI::PixelLayout::Type::F32;
        read.picture = encoder.readback(picture, 0, 0, { 0, 0, r.width, r.height }, layout);
        if (read.picture.isNull())
        {
            return false;
        }
        // The metered exposure, one texel, read with the frame.
        const RHI::Format exposure_format = exposure->desc().format;
        if (exposure_format == RHI::Format::R16Float || exposure_format == RHI::Format::R32Float)
        {
            RHI::PixelLayout one;
            one.components = RHI::PixelLayout::Components::R;
            one.type = exposure_format == RHI::Format::R16Float ? RHI::PixelLayout::Type::F16 : RHI::PixelLayout::Type::F32;
            read.exposure = encoder.readback(exposure, 0, 0, { 0, 0, 1, 1 }, one);
            read.exposureFormat = exposure_format;
        }
        s.pending.push_back(std::move(read));
        ++s.recorded;
        return true;
    }
}

namespace LuminousMetalRender
{
    void beginRender()
    {
        state().releaseRender();
    }

    void setRead(const Read& read)
    {
        State& s = state();
        s.read = read;
        s.readAsked = true;
    }

    bool takePicture(std::vector<F32>& rgb, U32 width, U32 height, F32& metered, std::string& error)
    {
        State& s = state();
        harvest(true);
        if (!s.error.empty())
        {
            error = s.error;
            return false;
        }
        if (s.summed == 0 || s.sum.empty())
        {
            error = "the frame was not rendered";
            return false;
        }
        if (s.sumWidth != width || s.sumHeight != height)
        {
            error = llformat("the picture came out %u x %u, not %u x %u", s.sumWidth, s.sumHeight, width, height);
            return false;
        }
        if (s.summed > 1)
        {
            const F32 scale = 1.f / (F32)s.summed;
            for (F32& v : s.sum)
            {
                v *= scale;
            }
        }
        rgb = std::move(s.sum);
        s.sum.clear();
        s.sumWidth = s.sumHeight = 0;
        metered = s.metered;
        return true;
    }

    void endRender()
    {
        state().releaseRender();
    }

    S32 framesRead()
    {
        return state().recorded;
    }

    U64 readBytes(U32 width, U32 height, bool eight_bit, bool light)
    {
        if (eight_bit || !LuminousMetal::metal())
        {
            return 0;
        }
        RHI::TextureDesc desc;
        desc.format = light ? RHI::Format::RGBA16Float : RHI::Format::RGBA32Float;
        desc.width = llmax(width, 1U);
        desc.height = llmax(height, 1U);
        desc.usage = RHI::TEXTURE_STORAGE | RHI::TEXTURE_READBACK;
        const U64 pixels = (U64)desc.width * desc.height;
        // The texture, two reads in flight (each the texture's bytes, tight rows) and the sum of RGB floats.
        return RHI::textureBytes(desc) + 2 * pixels * (light ? 8 : 16) + pixels * 3 * sizeof(F32);
    }
}

namespace LuminousMetal
{
    void registerRender(PassTable& table)
    {
        table.fn[size_t(Pass::AverageFrames)] = drawAverage;
        table.fn[size_t(Pass::HDRCapture)] = drawRead;
        if (!metal())
        {
            return;
        }
        RHI::onViewerDeviceRelease(forgetDevice);
        doOnIdleRepeating(pollLibrary);
    }
}
