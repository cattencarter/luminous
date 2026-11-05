/**
 * @file luminousmetaldof.cpp
 * @brief Photo mode on Metal: depth of field, the group's native passes.
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

#include "app_settings/shaders/metal/luminous/common/commonshared.h"

#include "llcallbacklist.h"
#include "llnativeprogram.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llstartup.h"

#include <algorithm>
#include <array>
#include <cmath>

extern bool gSnapshot;

// Photo mode's depth of field (Enhance) as native Metal passes: dof/dof.metal says what each does.
//
// The circle of confusion is Firestorm's cofF, translated, drawn into targets.cof before the hooks. Then:
// - DoFTiles: one compute pass -- the source's mip levels (the common single-pass downsampler, into a
//   half-size texture of this file's own, so the source needs no levels), the tiles, each tile's kind, and
//   the coarse tiles gathered at half or quarter size.
// - DoFGather: one native render pass into the HDR frame, colour only.
// Live, a tile whose every blur in and round it is large is gathered at half size (at least 8 pixels of
// blur) or quarter size (at least 32), its samples spaced as far apart as its texels; every other blurred
// tile, and every tile of a picture's frame, is gathered at each pixel exactly as the translated gather
// does.
//
// In a frame drawn for MetalFX temporal (LuminousMetal::temporalSettles, LuminousMetalFXTemporalSettleEnhance) the
// camera is jittered, and a blur gathered from the jittered frame and its circle of confusion changes its shape at
// focus edges from frame to frame, which the scaler passes on. The gather then goes into a layer of the frame's size,
// each texel the place the camera without the jitter saw: the samples' part of the blur and its weight. The layer is
// settled (averaged with the frames before, moved by the camera) and blended back where this frame shows each place,
// the frame keeping the rest of its own pixel, so what is in focus stays jittered as the scene is.
namespace
{
    namespace RHI = LL::RHI;
    using LuminousMetal::Pass;

    const char* const MANIFEST = "luminous/dof/programs.json";
    const char* const LIBRARY = "dof";
    const char* const COMPOSITE_VS = "luminous_fullscreen_vs";
    const char* const COMPOSITE_FS = "luminous_dof_composite_fs";
    const char* const LAYER_FS = "luminous_dof_layer_fs";
    const char* const SETTLED_FS = "luminous_dof_settled_fs";
    const std::array<const char*, 5> KERNELS = { "luminous_spd_downsample", "luminous_dof_tiles", "luminous_dof_rows",
                                                 "luminous_dof_classify", "luminous_dof_coarse" };
    enum Kernel
    {
        SPD,
        TILES,
        ROWS,
        CLASSIFY,
        COARSE
    };

    // The smallest blur, in pixels, everywhere in and round a tile for it to be gathered at half and at
    // quarter size in the live view. A blur that wide has nothing finer than the coarse texels' spacing.
    constexpr F32 COARSE_HALF = 8.f;
    constexpr F32 COARSE_QUARTER = 32.f;

    constexpr U32 TILE = LUMINOUS_DOF_TILE;

    // What the native passes hold. Everything goes when the viewer's device does.
    struct State
    {
        LLNativeProgram program;
        bool loaded = false;   // load() called for this device
        bool failed = false;   // the library or a pipeline failed: the effect stays off
        bool ready = false;    // every pipeline compiled; the passes are said ready

        // The source's mip levels from level 1 on, keyed by the source's size.
        U32 width = 0;
        U32 height = 0;
        RHI::Ref<RHI::TransientPool> chain;
        // The coarse gathers' textures, made the first time the live view uses them at this size.
        RHI::Ref<RHI::TransientPool> coarse;
        // The tiles' buffers, keyed by the tile count.
        U32 tileCount = 0;
        RHI::Ref<RHI::Buffer> tiles;
        RHI::Ref<RHI::Buffer> rows;
        RHI::Ref<RHI::Buffer> tileState;
        RHI::Ref<RHI::Buffer> counts;
        RHI::Ref<RHI::Buffer> lists;
        RHI::Ref<RHI::Buffer> spdCounter;
        RHI::Ref<RHI::Buffer> spdTail;
        // The layer the gather draws into in a frame drawn for MetalFX temporal, the source's size; let go in the
        // first frame gathered without it, or two frames after its last when the gather is no longer drawn.
        RHI::Ref<RHI::TransientPool> layer;
        U64 layerFrame = 0;

        // This frame's compute pass, for the gather.
        U64 preparedFrame = ~0ull;
        bool nothingToDraw = false;
        bool settled = false; // the gather goes into the layer, which is settled and added back
        LuminousDoFParams params{};
        RHI::Texture* source = nullptr;

        void releaseTargets()
        {
            chain = nullptr;
            coarse = nullptr;
            layer = nullptr;
            settled = false;
            tiles = rows = tileState = counts = lists = spdCounter = spdTail = nullptr;
            width = height = tileCount = 0;
            preparedFrame = ~0ull;
            source = nullptr;
        }
    };

    State& state()
    {
        static State s;
        return s;
    }

    RHI::Texture* chainTexture()
    {
        return state().chain.notNull() ? state().chain->texture(0) : nullptr;
    }

    RHI::Texture* coarseTexture(U32 index)
    {
        return state().coarse.notNull() ? state().coarse->texture(index) : nullptr;
    }

    RHI::Texture* layerTexture()
    {
        return state().layer.notNull() ? state().layer->texture(0) : nullptr;
    }

    RHI::PipelineDesc compositeDesc(RHI::Format format)
    {
        RHI::PipelineDesc desc;
        desc.colorCount = 1;
        desc.colorFormats[0] = format;
        desc.colorMask = RHI::COLOR_MASK_R | RHI::COLOR_MASK_G | RHI::COLOR_MASK_B; // the frame's alpha stays
        return desc;
    }

    // The layer: colour and weight, all four channels written.
    RHI::PipelineDesc layerDesc()
    {
        RHI::PipelineDesc desc;
        desc.colorCount = 1;
        desc.colorFormats[0] = RHI::Format::RGBA16Float;
        return desc;
    }

    // The settled layer onto the frame: its colour added, the frame kept by 1 - its weight; the frame's alpha stays.
    RHI::PipelineDesc settledDesc(RHI::Format format)
    {
        RHI::PipelineDesc desc = compositeDesc(format);
        desc.blend.enabled = true;
        desc.blend.srcColor = RHI::BlendFactor::One;
        desc.blend.dstColor = RHI::BlendFactor::OneMinusSourceAlpha;
        desc.blend.srcAlpha = RHI::BlendFactor::Zero;
        desc.blend.dstAlpha = RHI::BlendFactor::One;
        return desc;
    }

    // The layer's two pipelines, asked for when the library is ready so that they compile in the background; the
    // gather settles only once both are ready, and draws into the frame as before until then or if they fail.
    bool settlePipelinesReady(RHI::Format frame)
    {
        State& s = state();
        RHI::Pipeline* layer = s.program.render(COMPOSITE_VS, LAYER_FS, layerDesc());
        RHI::Pipeline* settled = s.program.render(COMPOSITE_VS, SETTLED_FS, settledDesc(frame));
        return layer && settled && layer->status() == RHI::CompileStatus::Ready
               && settled->status() == RHI::CompileStatus::Ready;
    }

    void forgetDevice()
    {
        State& s = state();
        s.releaseTargets();
        s.program.release();
        s.loaded = s.failed = s.ready = false;
        LuminousMetal::setReady(Pass::DoFTiles, false);
        LuminousMetal::setReady(Pass::DoFGather, false);
    }

    // Each idle frame until the library's pipelines are ready: loads the library once the viewer is in
    // the world on Metal, then says the passes are ready when every pipeline has compiled.
    bool pollLibrary()
    {
        State& s = state();
        if (!LuminousMetal::metal())
        {
            return true; // never on OpenGL
        }
        if (s.layer.notNull() && s.layerFrame + 2 < LuminousMetal::context().frame)
        {
            s.layer = nullptr;
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
            LL_WARNS("Luminous") << "Depth of field on Metal: its library did not compile: " << s.program.log() << LL_ENDL;
            s.failed = true;
            return false;
        }
        if (status != RHI::CompileStatus::Ready)
        {
            return false;
        }
        bool all = true;
        for (const char* kernel : KERNELS)
        {
            RHI::ComputePipeline* pipeline = s.program.compute(kernel);
            if (!pipeline || pipeline->status() == RHI::CompileStatus::Failed)
            {
                LL_WARNS("Luminous") << "Depth of field on Metal: kernel " << kernel << " did not compile" << LL_ENDL;
                s.failed = true;
                return false;
            }
            all = all && pipeline->status() == RHI::CompileStatus::Ready;
        }
        // The frame is RGBA16F; another format gets its own pipeline at its first draw.
        RHI::Pipeline* composite = s.program.render(COMPOSITE_VS, COMPOSITE_FS, compositeDesc(RHI::Format::RGBA16Float));
        if (!composite || composite->status() == RHI::CompileStatus::Failed)
        {
            LL_WARNS("Luminous") << "Depth of field on Metal: its composite did not compile" << LL_ENDL;
            s.failed = true;
            return false;
        }
        all = all && composite->status() == RHI::CompileStatus::Ready;
        if (all)
        {
            settlePipelinesReady(RHI::Format::RGBA16Float);
            s.ready = true;
            LuminousMetal::setReady(Pass::DoFTiles, true);
            LuminousMetal::setReady(Pass::DoFGather, true);
            LL_INFOS("Luminous") << "Depth of field on Metal: native passes ready" << LL_ENDL;
        }
        return false;
    }

    RHI::Ref<RHI::Buffer> storageBuffer(RHI::Device& device, U64 bytes, const char* name, bool zero = false)
    {
        RHI::BufferDesc desc;
        desc.size = std::max<U64>(bytes, 16);
        desc.usage = RHI::BUFFER_STORAGE;
        std::vector<std::byte> zeros;
        if (zero)
        {
            zeros.resize(size_t(desc.size));
        }
        return device.createBuffer(desc, zeros, name);
    }

    RHI::TransientTextureDesc storageTexture(U32 width, U32 height, U32 levels, const char* name)
    {
        RHI::TransientTextureDesc texture;
        texture.desc.format = RHI::Format::RGBA16Float;
        texture.desc.width = std::max(width, 1u);
        texture.desc.height = std::max(height, 1u);
        texture.desc.mipLevels = std::min(levels, RHI::fullMipCount(texture.desc.width, texture.desc.height));
        texture.desc.usage = RHI::TEXTURE_SAMPLED | RHI::TEXTURE_STORAGE;
        texture.name = name;
        return texture;
    }

    // The chain and the tiles' buffers for a source of width x height.
    bool ensureTargets(RHI::Device& device, U32 width, U32 height, U32 tile_count)
    {
        State& s = state();
        if (s.chain.isNull() || s.width != width || s.height != height)
        {
            RHI::TransientPoolDesc pool;
            pool.name = "luminous dof chain";
            pool.textures.push_back(storageTexture(width / 2, height / 2, LUMINOUS_DOF_CHAIN_LEVELS, "luminous dof chain"));
            s.chain = device.createTransientPool(pool);
            s.coarse = nullptr; // made again at this size when the live view wants it
            s.width = width;
            s.height = height;
        }
        if (s.tiles.isNull() || s.tileCount != tile_count)
        {
            s.tiles = storageBuffer(device, U64(tile_count) * 8, "luminous dof tiles");
            s.rows = storageBuffer(device, U64(tile_count) * 16, "luminous dof rows");
            s.tileState = storageBuffer(device, U64(tile_count) * 8, "luminous dof tile state");
            s.counts = storageBuffer(device, 16, "luminous dof counts", true);
            s.lists = storageBuffer(device, U64(tile_count) * 8, "luminous dof lists");
            s.tileCount = tile_count;
        }
        if (s.spdCounter.isNull())
        {
            // Only read past level 6, which this pass never asks for; bound because the kernel names them.
            s.spdCounter = storageBuffer(device, 16, "luminous dof spd counter", true);
            s.spdTail = storageBuffer(device, 16, "luminous dof spd tail");
        }
        return chainTexture() && s.tiles.notNull() && s.rows.notNull() && s.tileState.notNull() && s.counts.notNull()
               && s.lists.notNull() && s.spdCounter.notNull() && s.spdTail.notNull();
    }

    bool ensureLayer(RHI::Device& device)
    {
        State& s = state();
        RHI::Texture* layer = layerTexture();
        if (!layer || layer->desc().width != s.width || layer->desc().height != s.height)
        {
            RHI::TransientPoolDesc pool;
            pool.name = "luminous dof layer";
            RHI::TransientTextureDesc texture;
            texture.desc.format = RHI::Format::RGBA16Float;
            texture.desc.width = std::max(s.width, 1u);
            texture.desc.height = std::max(s.height, 1u);
            texture.desc.usage = RHI::TEXTURE_SAMPLED | RHI::TEXTURE_RENDER_TARGET;
            texture.name = "luminous dof layer";
            pool.textures.push_back(texture);
            s.layer = device.createTransientPool(pool);
        }
        return layerTexture() != nullptr;
    }

    bool ensureCoarse(RHI::Device& device)
    {
        State& s = state();
        if (s.coarse.isNull())
        {
            RHI::TransientPoolDesc pool;
            pool.name = "luminous dof coarse";
            pool.textures.push_back(storageTexture((s.width + 1) / 2, (s.height + 1) / 2, 1, "luminous dof half"));
            pool.textures.push_back(storageTexture((s.width + 3) / 4, (s.height + 3) / 4, 1, "luminous dof quarter"));
            s.coarse = device.createTransientPool(pool);
        }
        return coarseTexture(0) && coarseTexture(1);
    }

    F32 lodOf(F32 max_cof, F32 samples)
    {
        return std::max(0.f, std::log2(max_cof * std::sqrt(F_PI / samples)));
    }

    // The coarse gathers' spirals, and the reach and mip levels everything needs, from what
    // fillDoFParams gave. A coarse tile's samples lie at least its texels' spacing apart: fewer of them,
    // read at a coarser level to match, as the translated gather thins its spiral for its budget.
    void fillNativeParams(LuminousDoFParams& p, bool coarse, U32 chain_levels)
    {
        // The layer's texels read the source up to half a pixel off their own: a pixel more of reach.
        const F32 shift = p.layer ? 1.f : 0.f;
        if (p.layer)
        {
            const F32 margin = std::max(p.maxCoF, 0.f) + std::pow(2.f, std::ceil(p.lod) + 1.f) + 1.f + shift;
            p.reach = std::max(p.reach, (S32)std::ceil(margin / (F32)TILE));
        }
        const F32 max_cof = std::max(p.maxCoF, 1.f);
        auto thinned = [&](F32 spacing, S32& samples, F32& lod)
        {
            const F32 current = max_cof * std::sqrt(F_PI / (F32)p.samples);
            if (current >= spacing)
            {
                samples = p.samples;
                lod = p.lod;
                return;
            }
            samples = std::clamp((S32)std::ceil(F_PI * max_cof * max_cof / (spacing * spacing)), 16, p.samples);
            lod = lodOf(max_cof, (F32)samples);
        };
        thinned(2.f, p.halfSamples, p.halfLod);
        thinned(4.f, p.quarterSamples, p.quarterLod);
        p.coarseHalf = coarse ? COARSE_HALF : 0.f;
        p.coarseQuarter = coarse ? COARSE_QUARTER : 0.f;

        // The coarsest level any sample reads: the full spiral's, the coarse ones', or a spiral thinned
        // for the budget.
        F32 coarsest = p.lod;
        if (coarse)
        {
            coarsest = std::max({ coarsest, p.halfLod, p.quarterLod });
        }
        if (p.budget > 0 && p.budget < p.samples)
        {
            coarsest = std::max(coarsest, lodOf(max_cof, (F32)p.budget));
        }
        // A sample lies up to max_cof pixels away and reads texels spanning up to 2^(lod + 1) pixels round
        // it; a coarse texel's centre lies up to 2 pixels outside its tile, and it is read from 2 more (the
        // layer's a pixel more again).
        const F32 margin = std::max(p.maxCoF, 0.f) + std::pow(2.f, std::ceil(coarsest) + 1.f) + 1.f + 4.f + shift;
        p.reachCoarse = coarse ? (S32)std::ceil(margin / (F32)TILE) : 0;
        p.levels = (S32)std::clamp((U32)std::ceil(coarsest), 1u, chain_levels);
    }

    // The compute pass: the source's levels, the tiles, their kinds and the coarse tiles. Once a frame.
    bool prepare(LuminousMetal::PassContext& context, LuminousMetal::PassTargets& targets)
    {
        State& s = state();
        RHI::Device* device = RHI::viewerDevice();
        if (!s.ready || !device || !targets.cof || !targets.frame)
        {
            return false;
        }
        RHI::Texture* source = targets.cof->getRHITexture(0);
        RHI::Texture* frame = targets.frame->getRHITexture(0);
        // Prepared already for this frame's gather: the tiles' hook ran before it.
        if (s.preparedFrame == context.frame && s.source == source && s.params.maxCoF == targets.maxCoF)
        {
            return true;
        }
        // The gather reads the source at the frame's pixels: renderDoF makes them one size.
        if (!source || !frame || source->desc().width != frame->desc().width
            || source->desc().height != frame->desc().height)
        {
            return false;
        }
        const U32 width = source->desc().width;
        const U32 height = source->desc().height;

        LuminousDoFParams& p = s.params;
        LuminousPhotoMode::fillDoFParams(p, width, height, targets.maxCoF);
        s.source = source;
        s.preparedFrame = context.frame;
        s.settled = false;
        // No blur anywhere: the gather would give every pixel its own colour back.
        s.nothingToDraw = p.maxCoF <= 0.f;
        if (s.nothingToDraw)
        {
            return true;
        }
        if (gSnapshot)
        {
            p.picture = 1;
        }
        const U32 tile_count = p.tiles.x * p.tiles.y;
        if (!ensureTargets(*device, width, height, tile_count))
        {
            s.preparedFrame = ~0ull;
            return false;
        }
        // A live frame drawn for MetalFX temporal: the gather goes into the layer, each texel the place the camera
        // without the jitter saw, read from the source that far on. Never in a picture's frame.
        F32 jitter_u = 0.f;
        F32 jitter_v = 0.f;
        s.settled = !p.picture && settlePipelinesReady(frame->desc().format)
                    && LuminousMetal::temporalSettles(LuminousMetal::SettledLayer::Enhance, jitter_u, jitter_v)
                    && ensureLayer(*device);
        if (s.settled)
        {
            p.layer = 1;
            p.jitterX = jitter_u * (F32)width;
            p.jitterY = jitter_v * (F32)height;
            s.layerFrame = context.frame;
        }
        else
        {
            s.layer = nullptr;
        }
        const bool coarse = !p.picture && ensureCoarse(*device);
        RHI::Texture* chain = chainTexture();
        fillNativeParams(p, coarse, chain->desc().mipLevels);

        RHI::Encoder& encoder = device->encoder();
        if (!encoder.beginComputePass({ "luminous depth of field" }))
        {
            s.preparedFrame = ~0ull;
            return false;
        }
        const RHI::NativeStage cs = RHI::NativeStage::Compute;

        // The source's levels 1 to p.levels into the chain's levels 0 to p.levels - 1.
        {
            LuminousSPDConstants spd{};
            spd.sourceSize = simd_make_uint2(width, height);
            spd.groups = simd_make_uint2((width + LUMINOUS_SPD_TILE - 1) / LUMINOUS_SPD_TILE,
                                         (height + LUMINOUS_SPD_TILE - 1) / LUMINOUS_SPD_TILE);
            spd.mips = (U32)p.levels;
            encoder.setComputePipeline(s.program.compute(KERNELS[SPD]));
            RHI::setNativeConstants(encoder, cs, LUMINOUS_SPD_BUFFER_CONSTANTS, spd);
            encoder.setNativeBuffer(cs, LUMINOUS_SPD_BUFFER_COUNTER, s.spdCounter.get(), 0, RHI::Access::ReadWrite);
            encoder.setNativeBuffer(cs, LUMINOUS_SPD_BUFFER_TAIL, s.spdTail.get(), 0, RHI::Access::ReadWrite);
            encoder.setNativeTexture(cs, LUMINOUS_SPD_TEXTURE_SOURCE, source, RHI::Access::Read, { 0, 1 });
            for (U32 level = 0; level < (U32)p.levels; ++level)
            {
                encoder.setNativeTexture(cs, LUMINOUS_SPD_TEXTURE_FIRST_DEST + level, chain, RHI::Access::Write, { level, 1 });
            }
            encoder.dispatchThreadgroups({ spd.groups.x, spd.groups.y, 1 }, { LUMINOUS_SPD_THREADS, 1, 1 });
            // Its write bindings go, so the next dispatches neither write the chain nor wait on it needlessly.
            for (U32 level = 0; level < (U32)p.levels; ++level)
            {
                encoder.setNativeTexture(cs, LUMINOUS_SPD_TEXTURE_FIRST_DEST + level, nullptr);
            }
            encoder.setNativeBuffer(cs, LUMINOUS_SPD_BUFFER_COUNTER, nullptr, 0);
            encoder.setNativeBuffer(cs, LUMINOUS_SPD_BUFFER_TAIL, nullptr, 0);
        }

        const RHI::DispatchSize tile_grid{ p.tiles.x, p.tiles.y, 1 };
        RHI::setNativeConstants(encoder, cs, LUMINOUS_DOF_BUFFER_PARAMS, p);

        // The largest blur and the coarse key of each tile.
        encoder.setComputePipeline(s.program.compute(KERNELS[TILES]));
        encoder.setNativeTexture(cs, LUMINOUS_DOF_TEXTURE_SOURCE, source, RHI::Access::Read, { 0, 1 });
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_TILES, s.tiles.get(), 0, RHI::Access::Write);
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_COUNTS, s.counts.get(), 0, RHI::Access::ReadWrite);
        encoder.dispatchThreadgroups(tile_grid, { TILE, TILE, 1 });
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_COUNTS, nullptr, 0);
        encoder.setNativeTexture(cs, LUMINOUS_DOF_TEXTURE_SOURCE, nullptr);

        // Along the rows.
        encoder.setComputePipeline(s.program.compute(KERNELS[ROWS]));
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_TILES, s.tiles.get(), 0, RHI::Access::Read);
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_ROWS, s.rows.get(), 0, RHI::Access::Write);
        encoder.dispatchThreads(tile_grid, { 8, 8, 1 });
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_TILES, nullptr, 0);

        // Down the columns, and each tile's kind.
        encoder.setComputePipeline(s.program.compute(KERNELS[CLASSIFY]));
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_ROWS, s.rows.get(), 0, RHI::Access::Read);
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_STATE, s.tileState.get(), 0, RHI::Access::Write);
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_COUNTS, s.counts.get(), 0, RHI::Access::ReadWrite);
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_LISTS, s.lists.get(), 0, RHI::Access::Write);
        encoder.dispatchThreads(tile_grid, { 8, 8, 1 });
        encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_ROWS, nullptr, 0);

        // The coarse tiles, live only. Enough threadgroups for every tile to be coarse; those past the
        // lists' counts return at once.
        if (coarse)
        {
            const U32 groups = (tile_count + LUMINOUS_DOF_HALF_TILES_PER_GROUP - 1) / LUMINOUS_DOF_HALF_TILES_PER_GROUP
                               + (tile_count + LUMINOUS_DOF_QUARTER_TILES_PER_GROUP - 1) / LUMINOUS_DOF_QUARTER_TILES_PER_GROUP;
            encoder.setComputePipeline(s.program.compute(KERNELS[COARSE]));
            encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_STATE, s.tileState.get(), 0, RHI::Access::Read);
            encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_COUNTS, s.counts.get(), 0, RHI::Access::Read);
            encoder.setNativeBuffer(cs, LUMINOUS_DOF_BUFFER_LISTS, s.lists.get(), 0, RHI::Access::Read);
            encoder.setNativeTexture(cs, LUMINOUS_DOF_TEXTURE_SOURCE, source, RHI::Access::Read, { 0, 1 });
            encoder.setNativeTexture(cs, LUMINOUS_DOF_TEXTURE_CHAIN, chain, RHI::Access::Read);
            encoder.setNativeTexture(cs, LUMINOUS_DOF_TEXTURE_HALF, coarseTexture(0), RHI::Access::Write, { 0, 1 });
            encoder.setNativeTexture(cs, LUMINOUS_DOF_TEXTURE_QUARTER, coarseTexture(1), RHI::Access::Write, { 0, 1 });
            encoder.dispatchThreadgroups({ groups, 1, 1 }, { 256, 1, 1 });
        }
        encoder.endComputePass();
        return true;
    }

    // One fullscreen draw of pipeline into target in a native pass of its own: cleared first, or loaded.
    template <typename Bind>
    bool drawPass(RHI::Device& device, RHI::Texture* target, RHI::Pipeline* pipeline, bool clear, const char* name,
                  Bind bind)
    {
        RHI::RenderPassDesc pass;
        pass.name = name;
        pass.native = true;
        pass.colorCount = 1;
        pass.color[0].texture = target;
        pass.color[0].load = clear ? RHI::LoadAction::Clear : RHI::LoadAction::Load;
        pass.color[0].store = RHI::StoreAction::Store;
        RHI::Encoder& encoder = device.encoder();
        if (!encoder.beginRenderPass(pass))
        {
            return false;
        }
        encoder.setPipeline(pipeline);
        encoder.setViewport(0, 0, (S32)target->desc().width, (S32)target->desc().height);
        encoder.setScissor(false, 0, 0, 0, 0);
        encoder.setCullMode(RHI::CullMode::Off);
        bind(encoder);
        encoder.draw(RHI::PrimitiveType::Triangles, 0, 3);
        encoder.endRenderPass();
        return true;
    }

    // The gather into the HDR frame, after prepare() in the same frame; settled, into the layer, and the layer's
    // average blended into the frame where this frame shows each place.
    bool composite(LuminousMetal::PassTargets& targets)
    {
        State& s = state();
        if (s.nothingToDraw)
        {
            return true;
        }
        RHI::Device* device = RHI::viewerDevice();
        RHI::Texture* frame = targets.frame ? targets.frame->getRHITexture(0) : nullptr;
        RHI::Texture* chain = chainTexture();
        RHI::Texture* layer = s.settled ? layerTexture() : nullptr;
        if (!device || !frame || !chain || !s.source || (s.settled && !layer))
        {
            return false;
        }
        const LuminousDoFParams& p = s.params;
        RHI::Pipeline* pipeline = s.settled ? s.program.render(COMPOSITE_VS, LAYER_FS, layerDesc())
                                            : s.program.render(COMPOSITE_VS, COMPOSITE_FS, compositeDesc(frame->desc().format));
        if (!pipeline || pipeline->status() == RHI::CompileStatus::Failed)
        {
            return false;
        }
        // In a picture's frame the coarse textures are never read; the chain stands in their slots.
        RHI::Texture* half = p.coarseHalf > 0.f ? coarseTexture(0) : chain;
        RHI::Texture* quarter = p.coarseQuarter > 0.f ? coarseTexture(1) : chain;

        const RHI::NativeStage fs = RHI::NativeStage::Fragment;
        // The layer is cleared: its sharp tiles add nothing.
        if (!drawPass(*device, s.settled ? layer : frame, pipeline, s.settled, "luminous depth of field",
                      [&](RHI::Encoder& encoder)
                      {
                          RHI::setNativeConstants(encoder, fs, LUMINOUS_DOF_BUFFER_PARAMS, p);
                          encoder.setNativeBuffer(fs, LUMINOUS_DOF_BUFFER_STATE, s.tileState.get(), 0, RHI::Access::Read);
                          encoder.setNativeTexture(fs, LUMINOUS_DOF_TEXTURE_SOURCE, s.source, RHI::Access::Read, { 0, 1 });
                          encoder.setNativeTexture(fs, LUMINOUS_DOF_TEXTURE_CHAIN, chain, RHI::Access::Read);
                          encoder.setNativeTexture(fs, LUMINOUS_DOF_TEXTURE_HALF, half, RHI::Access::Read);
                          encoder.setNativeTexture(fs, LUMINOUS_DOF_TEXTURE_QUARTER, quarter, RHI::Access::Read);
                      }))
        {
            return false;
        }
        if (!s.settled)
        {
            return true;
        }

        // The layer averaged with the frames before (or as it is, when the settle cannot draw this frame), then
        // blended into the frame at uv minus the jitter, so the scaler sees it jittered as the scene is.
        RHI::Texture* average = LuminousMetal::temporalSettle(LuminousMetal::SettledLayer::Enhance, layer,
                                                              layer->desc().width, layer->desc().height);
        RHI::Pipeline* add = s.program.render(COMPOSITE_VS, SETTLED_FS, settledDesc(frame->desc().format));
        if (!average || !add || add->status() == RHI::CompileStatus::Failed)
        {
            return false;
        }
        return drawPass(*device, frame, add, false, "luminous depth of field settled",
                        [&](RHI::Encoder& encoder)
                        {
                            RHI::setNativeConstants(encoder, fs, LUMINOUS_DOF_BUFFER_PARAMS, p);
                            encoder.setNativeTexture(fs, LUMINOUS_DOF_TEXTURE_LAYER, average, RHI::Access::Read);
                        });
    }

    bool drawTiles(LuminousMetal::PassContext& context, LuminousMetal::PassTargets& targets)
    {
        return prepare(context, targets);
    }

    // Works without the tiles' hook too: it prepares the frame itself when they were not drawn.
    bool drawGather(LuminousMetal::PassContext& context, LuminousMetal::PassTargets& targets)
    {
        return prepare(context, targets) && composite(targets);
    }
}

namespace LuminousMetal
{
    void registerDepthOfField(PassTable& table)
    {
        table.fn[size_t(Pass::DoFTiles)] = drawTiles;
        table.fn[size_t(Pass::DoFGather)] = drawGather;
        if (!metal())
        {
            return;
        }
        RHI::onViewerDeviceRelease(forgetDevice);
        doOnIdleRepeating(pollLibrary);
    }

    void releaseDepthOfFieldTargets()
    {
        state().releaseTargets();
    }
}
