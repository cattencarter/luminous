/**
 * @file luminousmetallens.cpp
 * @brief Photo mode on Metal: the lens, the group's native passes.
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
#include "llrendertarget.h"
#include "llrhi.h"
#include "llstartup.h"
#include "luminousphotomode.h"

#include "app_settings/shaders/metal/luminous/common/commonshared.h"

#include <array>
#include <initializer_list>

extern bool gSnapshotNoPost;

// The lens on Metal, native: the vignette, the bloom chain with clarity, halation and bloom, the anamorphic streaks,
// the lens flare, and chromatic aberration, distortion and glitch. Each pass computes what its translated pass in
// luminousphotomode.cpp computes, from the same numbers (fillLensParams); the lens/*.metal files say how.
//
// What the native passes do differently from the translated ones, where it changes the work and not the picture:
// - The bloom chain is made once a frame for clarity, halation and bloom, not once for each, and each of them only
//   steps its own up chain. Halation adds light, so bloom after halation makes the chain again from the frame
//   halation drew; clarity changes only local contrast, so halation and bloom read the chain made before it.
// - Clarity and halation rewrite the frame in one pass when both are drawn. Bloom, the chain's last reader, steps its
//   up chain in the down chain itself, as the translated chain does, and blends in place when it has no glow.
// - Chromatic aberration and distortion are one pass; glitch joins them when aberration is off, reading through the
//   distortion directly.
// - The streaks are one dispatch, a threadgroup per row holding the row in its memory through every spread.
//
// Native results live in the group's own textures; Firestorm's targets are only drawn into by native render passes.
namespace LuminousMetal
{
    namespace
    {
        namespace RHI = LL::RHI;

        constexpr const char* MANIFEST = "luminous/lens/programs.json";
        constexpr const char* COMMON_MANIFEST = "luminous/common/programs.json";
        constexpr const char* VERTEX = "luminous_fullscreen_vs";
        constexpr U32 LEVELS = LUMINOUS_LENS_BLOOM_LEVELS;
        constexpr U32 GROUP = LUMINOUS_LENS_GROUP_SIDE;

        const std::initializer_list<Pass> LENS_PASSES = { Pass::Vignette, Pass::BloomChain, Pass::Clarity, Pass::Halation,
                                                          Pass::Bloom, Pass::Anamorphic, Pass::LensFlare,
                                                          Pass::ChromaticAberration, Pass::Distortion, Pass::Glitch };

        const char* const COMPUTE_FUNCTIONS[] = { "luminous_lens_bloom_down", "luminous_lens_bloom_up", "luminous_lens_bloom_up_in_place",
                                                  "luminous_lens_streak",
                                                  "luminous_lens_flare_bright", "luminous_lens_flare" };

        enum class Blend : U8
        {
            None,     ///< the frame drawn anew
            Alpha,    ///< bloom's blend toward its blur: source alpha, one minus source alpha
            Multiply, ///< the vignette: the destination times the source
            AddAlpha, ///< the streaks: source alpha, one
            Add,      ///< the flare: one, one
        };

        struct RenderFunction
        {
            const char* fragment;
            Blend blend;
        };

        const RenderFunction RENDER_FUNCTIONS[] = {
            { "luminous_lens_tone_fs", Blend::None },
            { "luminous_lens_bloom_fs", Blend::Alpha },
            { "luminous_lens_vignette_fs", Blend::Multiply },
            { "luminous_lens_streak_fs", Blend::AddAlpha },
            { "luminous_lens_flare_fs", Blend::Add },
            { "luminous_lens_finish_fs", Blend::None },
        };

        U64 bit(Pass pass)
        {
            return 1ull << size_t(pass);
        }

        // ---- the library, its pipelines and readiness ---------------------------------------------------------

        struct Programs
        {
            LLNativeProgram lens;
            LLNativeProgram common;
            RHI::Device* device = nullptr; ///< the device the libraries were loaded on; null after its release
            bool ready = false;
            bool warned = false;
        };

        Programs& programs()
        {
            static Programs p;
            return p;
        }

        void setAllReady(bool ready)
        {
            for (Pass pass : LENS_PASSES)
            {
                setReady(pass, ready);
            }
        }

        RHI::PipelineDesc pipelineDesc(RHI::Format format, Blend blend)
        {
            RHI::PipelineDesc desc;
            desc.colorCount = 1;
            desc.colorFormats[0] = format;
            switch (blend)
            {
            case Blend::None:
                desc.colorMask = RHI::COLOR_MASK_ALL;
                break;
            case Blend::Alpha:
                desc.blend.enabled = true;
                desc.blend.srcColor = desc.blend.srcAlpha = RHI::BlendFactor::SourceAlpha;
                desc.blend.dstColor = desc.blend.dstAlpha = RHI::BlendFactor::OneMinusSourceAlpha;
                desc.colorMask = U8(RHI::COLOR_MASK_R | RHI::COLOR_MASK_G | RHI::COLOR_MASK_B); // the glow mask stays
                break;
            case Blend::Multiply:
                desc.blend.enabled = true;
                desc.blend.srcColor = desc.blend.srcAlpha = RHI::BlendFactor::DestColor;
                desc.blend.dstColor = desc.blend.dstAlpha = RHI::BlendFactor::Zero;
                desc.colorMask = U8(RHI::COLOR_MASK_R | RHI::COLOR_MASK_G | RHI::COLOR_MASK_B);
                break;
            case Blend::AddAlpha:
                desc.blend.enabled = true;
                desc.blend.srcColor = desc.blend.srcAlpha = RHI::BlendFactor::SourceAlpha;
                desc.blend.dstColor = desc.blend.dstAlpha = RHI::BlendFactor::One;
                desc.colorMask = U8(RHI::COLOR_MASK_R | RHI::COLOR_MASK_G | RHI::COLOR_MASK_B);
                break;
            case Blend::Add:
                desc.blend.enabled = true;
                desc.blend.srcColor = desc.blend.srcAlpha = RHI::BlendFactor::One;
                desc.blend.dstColor = desc.blend.dstAlpha = RHI::BlendFactor::One;
                desc.colorMask = U8(RHI::COLOR_MASK_R | RHI::COLOR_MASK_G | RHI::COLOR_MASK_B);
                break;
            }
            return desc;
        }

        /// A render pipeline of the lens library for a target of this format, when it has compiled; null meanwhile.
        RHI::Pipeline* renderPipeline(const char* fragment, RHI::Format format, Blend blend)
        {
            RHI::Pipeline* pipeline = programs().lens.render(VERTEX, fragment, pipelineDesc(format, blend));
            return pipeline && pipeline->status() == RHI::CompileStatus::Ready ? pipeline : nullptr;
        }

        RHI::ComputePipeline* computePipeline(LLNativeProgram& program, const char* function)
        {
            RHI::ComputePipeline* pipeline = program.compute(function);
            return pipeline && pipeline->status() == RHI::CompileStatus::Ready ? pipeline : nullptr;
        }

        void warnOnce(const std::string& what)
        {
            Programs& p = programs();
            if (!p.warned)
            {
                p.warned = true;
                LL_WARNS("Luminous") << "The lens on Metal is off: " << what << LL_ENDL;
            }
        }

        // Every pipeline the passes use, for the frame's format: whether all have compiled. False while any is
        // pending; after one failed, false with a warning.
        bool pipelinesReady()
        {
            Programs& p = programs();
            bool ready = true;
            for (const char* function : COMPUTE_FUNCTIONS)
            {
                RHI::ComputePipeline* pipeline = p.lens.compute(function);
                if (!pipeline || pipeline->status() == RHI::CompileStatus::Failed)
                {
                    warnOnce(std::string("its kernel ") + function + " did not compile");
                    return false;
                }
                ready = ready && pipeline->status() == RHI::CompileStatus::Ready;
            }
            RHI::ComputePipeline* spd = p.common.compute("luminous_spd_downsample");
            if (!spd || spd->status() == RHI::CompileStatus::Failed)
            {
                warnOnce("the common downsampler did not compile");
                return false;
            }
            ready = ready && spd->status() == RHI::CompileStatus::Ready;
            for (const RenderFunction& function : RENDER_FUNCTIONS)
            {
                RHI::Pipeline* pipeline = p.lens.render(VERTEX, function.fragment,
                                                        pipelineDesc(RHI::Format::RGBA16Float, function.blend));
                if (!pipeline || pipeline->status() == RHI::CompileStatus::Failed)
                {
                    warnOnce(std::string("its program ") + function.fragment + " did not compile");
                    return false;
                }
                ready = ready && pipeline->status() == RHI::CompileStatus::Ready;
            }
            return ready;
        }

        void releaseResources();

        // Each frame, from the idle loop: once the viewer is in the world on a device with native programs, the
        // libraries load and compile in the background; the passes are said ready when every pipeline has compiled.
        bool poll()
        {
            if (!metal())
            {
                return true; // OpenGL: the translated passes, nothing to load
            }
            Programs& p = programs();
            RHI::Device* device = RHI::viewerDevice();
            if (!device || !device->capabilities().nativePrograms || !device->capabilities().computePasses ||
                LLStartUp::getStartupState() < STATE_STARTED || !photoModeOnMetal())
            {
                return false;
            }
            if (p.device != device)
            {
                p.device = device;
                p.ready = false;
                p.warned = false;
                setAllReady(false);
                if (!p.lens.load(MANIFEST, "lens") || !p.common.load(COMMON_MANIFEST, "common"))
                {
                    warnOnce("its programs could not be loaded");
                    return false;
                }
            }
            if (p.ready)
            {
                return false;
            }
            if (p.lens.status() == RHI::CompileStatus::Failed || p.common.status() == RHI::CompileStatus::Failed)
            {
                warnOnce("its library did not compile: " + p.lens.log() + p.common.log());
                return false;
            }
            if (!p.lens.ready() || !p.common.ready() || !pipelinesReady())
            {
                return false;
            }
            p.ready = true;
            setAllReady(true);
            LL_INFOS("Luminous") << "The lens on Metal is ready" << LL_ENDL;
            return false;
        }

        // ---- textures -----------------------------------------------------------------------------------------

        /// A texture of the group's own, as one texture of a transient pool, made again when its description
        /// changes.
        struct OwnTexture
        {
            RHI::Ref<RHI::TransientPool> pool;
            RHI::TextureDesc desc;

            RHI::Texture* get(const RHI::TextureDesc& wanted, const char* name)
            {
                if (pool.isNull() || !(desc == wanted))
                {
                    pool = nullptr;
                    RHI::Device* device = RHI::viewerDevice();
                    if (!device)
                    {
                        return nullptr;
                    }
                    RHI::TransientPoolDesc pool_desc;
                    pool_desc.name = name;
                    pool_desc.textures.push_back({ wanted, 0, 0, name });
                    pool = device->createTransientPool(pool_desc);
                    desc = wanted;
                }
                return pool.notNull() ? pool->texture(0) : nullptr;
            }

            void release()
            {
                pool = nullptr;
            }
        };

        RHI::TextureDesc storageDesc(U32 width, U32 height, U32 levels, RHI::Format format = RHI::Format::RGBA16Float)
        {
            RHI::TextureDesc desc;
            desc.format = format;
            desc.width = llmax(width, 1U);
            desc.height = llmax(height, 1U);
            desc.mipLevels = levels;
            desc.usage = RHI::TEXTURE_SAMPLED | RHI::TEXTURE_STORAGE;
            return desc;
        }

        U32 levelSize(U32 base, U32 level)
        {
            return llmax(base >> level, 1U);
        }

        RHI::DispatchSize groupsFor(U32 width, U32 height, U32 side)
        {
            return { (width + side - 1) / side, (height + side - 1) / side, 1 };
        }

        // ---- the bloom chain ----------------------------------------------------------------------------------

        /// The down chain (half the chain's size and five levels below it) and clarity's and halation's up chains from
        /// a quarter (the half-size level they read is made where they read it). Bloom, the chain's last reader in a
        /// frame, steps its up chain in the down chain itself, as the translated chain does.
        struct Chain
        {
            OwnTexture down;
            OwnTexture clarity;
            OwnTexture halation;
            U64 madeFrame = ~0ull; ///< the PassContext frame the down chain was made in
            bool stale = false;    ///< halation drew after it was made, or bloom stepped its up chain in it
            U32 width = 0;         ///< level 0 of the down chain
            U32 height = 0;
        };

        Chain& chain()
        {
            static Chain c;
            return c;
        }

        /// The streaks' and the flare's textures, and the common downsampler's buffers.
        struct LensTextures
        {
            OwnTexture streak;     ///< quarter width, full height
            OwnTexture bright;     ///< the flare's light, half size
            OwnTexture core;       ///< the starburst's light, half size, with mip levels for Star blur
            OwnTexture soft;       ///< the flare's light, a quarter size and softer
            OwnTexture flare;      ///< ghosts, halo and starburst, half size
            RHI::Ref<RHI::Buffer> spdCounter;
            RHI::Ref<RHI::Buffer> spdTail;
        };

        LensTextures& lensTextures()
        {
            static LensTextures t;
            return t;
        }

        void releaseResources()
        {
            LensTextures& t = lensTextures();
            t.streak.release();
            t.bright.release();
            t.core.release();
            t.soft.release();
            t.flare.release();
            t.spdCounter = nullptr;
            t.spdTail = nullptr;
            Chain& c = chain();
            c.down.release();
            c.clarity.release();
            c.halation.release();
            c.madeFrame = ~0ull;
        }

        /// The down chain's six levels from the frame, as the translated chain sizes them: from the full frame, so
        /// the blur covers the same share of it when the live view renders smaller. In the open compute pass.
        bool makeDownChain(RHI::Encoder& encoder, RHI::Texture* frame, U32 frame_width, U32 frame_height)
        {
            Chain& c = chain();
            RHI::ComputePipeline* down = computePipeline(programs().lens, "luminous_lens_bloom_down");
            if (!down)
            {
                return false;
            }
            const F32 scale = LuminousPhotoMode::liveRenderScale();
            const U32 width = llmax((U32)((F32)frame_width / scale + 0.5f) / 2, 1U);
            const U32 height = llmax((U32)((F32)frame_height / scale + 0.5f) / 2, 1U);
            if (RHI::fullMipCount(width, height) < LEVELS)
            {
                return false; // a frame under 64 pixels: no chain
            }
            RHI::Texture* levels = c.down.get(storageDesc(width, height, LEVELS), "luminous lens down chain");
            if (!levels)
            {
                return false;
            }
            c.width = width;
            c.height = height;

            encoder.setComputePipeline(down);
            // One level from the frame, then two a dispatch: the second from the first in threadgroup memory.
            RHI::Texture* source = frame;
            U32 source_level = 0;
            F32 source_width = (F32)frame_width;
            F32 source_height = (F32)frame_height;
            U32 next = 0;
            while (next < LEVELS)
            {
                const bool pair = source != frame && next + 1 < LEVELS;
                LuminousLensDownConstants k{};
                k.sizeA = simd_make_uint2(levelSize(width, next), levelSize(height, next));
                k.sizeB = pair ? simd_make_uint2(levelSize(width, next + 1), levelSize(height, next + 1)) : simd_make_uint2(0, 0);
                k.srcTexel = simd_make_float2(1.f / source_width, 1.f / source_height);
                k.hasB = pair ? 1 : 0;
                RHI::setNativeConstants(encoder, RHI::NativeStage::Compute, LUMINOUS_LENS_BUFFER_PASS, k);
                encoder.setNativeTexture(RHI::NativeStage::Compute, 0, source, RHI::Access::Read, { source_level, 1 });
                encoder.setNativeTexture(RHI::NativeStage::Compute, 1, levels, RHI::Access::Write, { next, 1 });
                if (pair)
                {
                    encoder.setNativeTexture(RHI::NativeStage::Compute, 2, levels, RHI::Access::Write, { next + 1, 1 });
                    const RHI::DispatchSize by_b = groupsFor(k.sizeB.x, k.sizeB.y, LUMINOUS_LENS_DOWN_TILE);
                    const RHI::DispatchSize by_a = groupsFor(k.sizeA.x, k.sizeA.y, 2 * LUMINOUS_LENS_DOWN_TILE);
                    encoder.dispatchThreadgroups({ llmax(by_b.x, by_a.x), llmax(by_b.y, by_a.y), 1 }, { GROUP, GROUP, 1 });
                }
                else
                {
                    encoder.setNativeTexture(RHI::NativeStage::Compute, 2, nullptr);
                    encoder.dispatchThreadgroups(groupsFor(k.sizeA.x, k.sizeA.y, LUMINOUS_LENS_DOWN_TILE), { GROUP, GROUP, 1 });
                }
                const U32 last = pair ? next + 1 : next;
                source = levels;
                source_level = last;
                source_width = (F32)levelSize(width, last);
                source_height = (F32)levelSize(height, last);
                next = last + 1;
            }
            for (U32 slot = 0; slot < 3; ++slot)
            {
                encoder.setNativeTexture(RHI::NativeStage::Compute, slot, nullptr);
            }
            return true;
        }

        /// An up chain to step: its texture, the chain level its texture's level 0 is, its weight, and the finest
        /// level it needs.
        struct UpChain
        {
            RHI::Texture* texture;
            U32 offset;
            F32 weight;
            U32 lowest;
        };

        /// Steps the up chains from the coarsest level down, each to its lowest, in the open compute pass.
        void stepUpChains(RHI::Encoder& encoder, std::initializer_list<UpChain> chains)
        {
            Chain& c = chain();
            RHI::ComputePipeline* up = computePipeline(programs().lens, "luminous_lens_bloom_up");
            RHI::Texture* down = c.down.pool.notNull() ? c.down.pool->texture(0) : nullptr;
            if (!up || !down || chains.size() == 0)
            {
                return;
            }
            encoder.setComputePipeline(up);
            for (S32 level = (S32)LEVELS - 2; level >= 0; --level)
            {
                LuminousLensUpConstants k{};
                k.size = simd_make_uint2(levelSize(c.width, level), levelSize(c.height, level));
                k.coarseTexel = simd_make_float2(1.f / (F32)levelSize(c.width, level + 1), 1.f / (F32)levelSize(c.height, level + 1));
                U32 count = 0;
                for (const UpChain& chain_up : chains)
                {
                    if ((S32)chain_up.lowest > level || !chain_up.texture || count == LUMINOUS_LENS_UP_CHAINS)
                    {
                        continue;
                    }
                    const U32 own = (U32)level - chain_up.offset;
                    if ((U32)level + 1 == LEVELS - 1)
                    {
                        encoder.setNativeTexture(RHI::NativeStage::Compute, 1 + count, down, RHI::Access::Read, { LEVELS - 1, 1 });
                    }
                    else
                    {
                        encoder.setNativeTexture(RHI::NativeStage::Compute, 1 + count, chain_up.texture, RHI::Access::Read, { own + 1, 1 });
                    }
                    encoder.setNativeTexture(RHI::NativeStage::Compute, 4 + count, chain_up.texture, RHI::Access::Write, { own, 1 });
                    k.weights[count] = llclamp(chain_up.weight, 0.f, 0.95f);
                    ++count;
                }
                if (count == 0)
                {
                    continue;
                }
                for (U32 unused = count; unused < LUMINOUS_LENS_UP_CHAINS; ++unused)
                {
                    encoder.setNativeTexture(RHI::NativeStage::Compute, 1 + unused, nullptr);
                    encoder.setNativeTexture(RHI::NativeStage::Compute, 4 + unused, nullptr);
                }
                k.chains = count;
                RHI::setNativeConstants(encoder, RHI::NativeStage::Compute, LUMINOUS_LENS_BUFFER_PASS, k);
                encoder.setNativeTexture(RHI::NativeStage::Compute, 0, down, RHI::Access::Read, { (U32)level, 1 });
                encoder.dispatchThreadgroups(groupsFor(k.size.x, k.size.y, GROUP), { GROUP, GROUP, 1 });
            }
            for (U32 slot = 0; slot < 7; ++slot)
            {
                encoder.setNativeTexture(RHI::NativeStage::Compute, slot, nullptr);
            }
        }

        /// Bloom's up chain, stepped in the down chain itself from the coarsest level to level 0, each level read
        /// and written by one binding, in the open compute pass. The down chain is bloom's blur afterwards.
        void stepBloomInPlace(RHI::Encoder& encoder, F32 weight)
        {
            Chain& c = chain();
            RHI::ComputePipeline* up = computePipeline(programs().lens, "luminous_lens_bloom_up_in_place");
            RHI::Texture* down = c.down.pool.notNull() ? c.down.pool->texture(0) : nullptr;
            if (!up || !down)
            {
                return;
            }
            const RHI::NativeStage stage = RHI::NativeStage::Compute;
            encoder.setComputePipeline(up);
            for (S32 level = (S32)LEVELS - 2; level >= 0; --level)
            {
                LuminousLensUpConstants k{};
                k.size = simd_make_uint2(levelSize(c.width, level), levelSize(c.height, level));
                k.coarseTexel = simd_make_float2(1.f / (F32)levelSize(c.width, level + 1), 1.f / (F32)levelSize(c.height, level + 1));
                k.weights = simd_make_float4(llclamp(weight, 0.f, 0.95f), 0.f, 0.f, 0.f);
                k.chains = 1;
                RHI::setNativeConstants(encoder, stage, LUMINOUS_LENS_BUFFER_PASS, k);
                encoder.setNativeTexture(stage, 1, down, RHI::Access::Read, { (U32)level + 1, 1 });
                encoder.setNativeTexture(stage, 4, down, RHI::Access::ReadWrite, { (U32)level, 1 });
                encoder.dispatchThreadgroups(groupsFor(k.size.x, k.size.y, GROUP), { GROUP, GROUP, 1 });
            }
            encoder.setNativeTexture(stage, 1, nullptr);
            encoder.setNativeTexture(stage, 4, nullptr);
        }

        // ---- drawing onto Firestorm's targets -----------------------------------------------------------------

        RHI::Texture* colour(LLRenderTarget* target)
        {
            return target ? target->getRHITexture(0) : nullptr;
        }

        RHI::Encoder* encoderFor(RHI::Texture* target)
        {
            RHI::Device* device = RHI::viewerDevice();
            return device && target && programs().ready ? &device->encoder() : nullptr;
        }

        /// A native render pass of one fullscreen triangle into target: bind() makes its bindings.
        template <typename Bind>
        bool drawFullscreen(RHI::Encoder& encoder, RHI::Texture* target, RHI::Pipeline* pipeline, bool load,
                            const char* name, const LuminousLensParams& params, Bind bind)
        {
            RHI::RenderPassDesc pass;
            pass.colorCount = 1;
            pass.color[0].texture = target;
            pass.color[0].load = load ? RHI::LoadAction::Load : RHI::LoadAction::DontCare;
            pass.native = true;
            pass.name = name;
            if (!encoder.beginRenderPass(pass))
            {
                return false;
            }
            encoder.setPipeline(pipeline);
            encoder.setViewport(0, 0, (S32)target->desc().width, (S32)target->desc().height);
            RHI::setNativeConstants(encoder, RHI::NativeStage::Fragment, LUMINOUS_LENS_BUFFER_PARAMS, params);
            bind();
            encoder.draw(RHI::PrimitiveType::Triangles, 0, 3);
            encoder.endRenderPass();
            return true;
        }

        // ---- the vignette -------------------------------------------------------------------------------------

        bool drawVignette(PassContext& /*context*/, PassTargets& targets)
        {
            RHI::Texture* frame = colour(targets.frame);
            RHI::Encoder* encoder = encoderFor(frame);
            if (!encoder || gSnapshotNoPost)
            {
                return false; // a snapshot without post-processing has none, as the translated vignette
            }
            LuminousLensParams params;
            LuminousPhotoMode::fillLensParams(params, targets.frame->getWidth(), targets.frame->getHeight());
            if (params.vignetteAmount <= 0.f)
            {
                return false;
            }
            RHI::Pipeline* pipeline = renderPipeline("luminous_lens_vignette_fs", frame->desc().format, Blend::Multiply);
            return pipeline && drawFullscreen(*encoder, frame, pipeline, true, "luminous vignette", params, [] {});
        }

        // ---- clarity, halation and bloom ----------------------------------------------------------------------

        /// Which of the three the next rewrite draws.
        struct ToneSet
        {
            bool clarity = false;
            bool halation = false;
            bool bloom = false;
        };

        bool drawnAlready(const PassContext& context, Pass pass)
        {
            return (context.drawnThisFrame & bit(pass)) != 0;
        }

        /// Halation is drawn this frame by its native pass, not yet drawn: its hook will ask.
        bool halationPending(const PassContext& context, const LuminousLensParams& params)
        {
            return params.halation > 0.f && on(Effect::Halation) && passReady(Pass::Halation) &&
                   !drawnAlready(context, Pass::Halation);
        }

        bool drawTone(PassContext& context, PassTargets& targets, ToneSet set)
        {
            RHI::Texture* frame = colour(targets.frame);
            RHI::Texture* other = colour(targets.other);
            RHI::Texture* exposure = context.exposure ? colour(context.exposure) : nullptr;
            RHI::Encoder* encoder = encoderFor(frame);
            if (!encoder || !exposure)
            {
                return false;
            }
            LuminousLensParams params;
            LuminousPhotoMode::fillLensParams(params, targets.frame->getWidth(), targets.frame->getHeight());
            set.clarity = set.clarity && params.clarity != 0.f;
            set.halation = set.halation && params.halation > 0.f;
            if (!set.clarity && !set.halation && !set.bloom)
            {
                return false;
            }
            if (params.bloomGlow <= 0.001f)
            {
                params.bloomGlow = 0.f; // as the translated bloom, which draws no glow below this
            }
            // Bloom alone without its glow blends in place; anything else draws the frame anew into the other target.
            const bool in_place = set.bloom && !set.clarity && !set.halation && params.bloomGlow == 0.f;
            if (!in_place && !other)
            {
                return false;
            }
            RHI::Pipeline* pipeline = in_place
                ? renderPipeline("luminous_lens_bloom_fs", frame->desc().format, Blend::Alpha)
                : renderPipeline("luminous_lens_tone_fs", other->desc().format, Blend::None);
            if (!pipeline)
            {
                return false;
            }

            Chain& c = chain();
            if (!encoder->beginComputePass({ "luminous lens chain" }))
            {
                return false;
            }
            bool made = c.madeFrame == context.frame && !c.stale && c.down.pool.notNull();
            if (!made)
            {
                made = makeDownChain(*encoder, frame, targets.frame->getWidth(), targets.frame->getHeight());
                if (made)
                {
                    c.madeFrame = context.frame;
                    c.stale = false;
                }
            }
            RHI::Texture* clarity_up = nullptr;
            RHI::Texture* halation_up = nullptr;
            if (made)
            {
                const U32 quarter_w = levelSize(c.width, 1);
                const U32 quarter_h = levelSize(c.height, 1);
                if (set.clarity)
                {
                    clarity_up = c.clarity.get(storageDesc(quarter_w, quarter_h, LEVELS - 2), "luminous lens clarity");
                }
                if (set.halation)
                {
                    halation_up = c.halation.get(storageDesc(quarter_w, quarter_h, LEVELS - 2), "luminous lens halation");
                }
                made = (!set.clarity || clarity_up) && (!set.halation || halation_up);
            }
            if (made)
            {
                if (set.clarity || set.halation)
                {
                    stepUpChains(*encoder, { { clarity_up, 1, 0.5f, 1 }, { halation_up, 1, params.halationReach, 1 } });
                }
                if (set.bloom)
                {
                    stepBloomInPlace(*encoder, params.bloomSize);
                }
            }
            encoder->endComputePass();
            if (!made)
            {
                return false;
            }

            RHI::Texture* blur = c.down.pool->texture(0);
            LuminousLensToneConstants k{};
            k.clarityTexel = simd_make_float2(1.f / (F32)levelSize(c.width, 1), 1.f / (F32)levelSize(c.height, 1));
            k.halationTexel = k.clarityTexel;
            k.bloomTexel = simd_make_float2(1.f / (F32)c.width, 1.f / (F32)c.height);
            k.clarityWeight = 0.5f;
            k.halationWeight = llclamp(params.halationReach, 0.f, 0.95f);
            k.mode = (set.clarity ? LUMINOUS_LENS_TONE_CLARITY : 0u) | (set.halation ? LUMINOUS_LENS_TONE_HALATION : 0u) |
                     (set.bloom ? LUMINOUS_LENS_TONE_BLOOM : 0u);
            const auto bind = [&]
            {
                const RHI::NativeStage stage = RHI::NativeStage::Fragment;
                RHI::setNativeConstants(*encoder, stage, LUMINOUS_LENS_BUFFER_PASS, k);
                if (!in_place)
                {
                    encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_FRAME, frame);
                    encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_EXPOSURE, exposure);
                    encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_BLUR, blur, RHI::Access::Read, { 0, 1 });
                }
                if (clarity_up)
                {
                    encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_CLARITY, clarity_up, RHI::Access::Read, { 0, 1 });
                }
                if (halation_up)
                {
                    encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_HALATION, halation_up, RHI::Access::Read, { 0, 1 });
                }
                if (set.bloom)
                {
                    encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_BLOOM, blur, RHI::Access::Read, { 0, 1 });
                }
            };
            if (!drawFullscreen(*encoder, in_place ? frame : other, pipeline, in_place, "luminous lens tone", params, bind))
            {
                return false;
            }
            targets.swapped = !in_place;
            if (set.halation || set.bloom)
            {
                // bloom after halation reads the frame halation drew, as the translated chain does; after bloom the
                // down chain holds bloom's blur
                c.stale = true;
            }
            context.drawnThisFrame |= bit(Pass::BloomChain);
            return true;
        }

        // Clarity draws halation with it when both are drawn this frame, and marks it drawn so its hook skips. Bloom
        // is drawn on its own: it steps its up chain in the down chain, which clarity and halation read.
        bool drawClarity(PassContext& context, PassTargets& targets)
        {
            LuminousLensParams params;
            LuminousPhotoMode::fillLensParams(params, targets.frame->getWidth(), targets.frame->getHeight());
            ToneSet set;
            set.clarity = true;
            set.halation = halationPending(context, params);
            if (!drawTone(context, targets, set))
            {
                return false;
            }
            context.drawnThisFrame |= set.halation ? bit(Pass::Halation) : 0;
            return true;
        }

        bool drawHalation(PassContext& context, PassTargets& targets)
        {
            ToneSet set;
            set.halation = true;
            return drawTone(context, targets, set);
        }

        bool drawBloom(PassContext& context, PassTargets& targets)
        {
            ToneSet set;
            set.bloom = true;
            return drawTone(context, targets, set);
        }

        // The chain has no hook of its own: its consumers make it. Said drawn when one of them did.
        bool drawBloomChain(PassContext& context, PassTargets& /*targets*/)
        {
            return drawnAlready(context, Pass::BloomChain);
        }

        // ---- anamorphic streaks -------------------------------------------------------------------------------

        bool drawStreaks(PassContext& context, PassTargets& targets)
        {
            RHI::Texture* frame = colour(targets.frame);
            RHI::Texture* exposure = context.exposure ? colour(context.exposure) : nullptr;
            RHI::Texture* depth = context.depth ? context.depth->getRHIDepth() : nullptr;
            RHI::Encoder* encoder = encoderFor(frame);
            RHI::ComputePipeline* kernel = computePipeline(programs().lens, "luminous_lens_streak");
            if (!encoder || !exposure || !depth || !kernel)
            {
                return false;
            }
            LuminousLensParams params;
            LuminousPhotoMode::fillLensParams(params, targets.frame->getWidth(), targets.frame->getHeight());
            if (params.streak <= 0.f)
            {
                return false;
            }
            // A quarter of the width, all of the height: the streak only blurs along the row.
            const U32 width = llmax(targets.frame->getWidth() / 4, 1U);
            const U32 height = targets.frame->getHeight();
            if (width > LUMINOUS_LENS_STREAK_MAX)
            {
                static bool said = false;
                if (!said)
                {
                    said = true;
                    LL_WARNS("Luminous") << "Anamorphic streaks on Metal: not drawn on a frame wider than "
                                         << LUMINOUS_LENS_STREAK_MAX * 4 << " pixels; the rest of the lens is"
                                         << LL_ENDL;
                }
                return false;
            }
            RHI::Pipeline* pipeline = renderPipeline("luminous_lens_streak_fs", frame->desc().format, Blend::AddAlpha);
            RHI::Texture* streak = lensTextures().streak.get(storageDesc(width, height, 1), "luminous lens streaks");
            if (!pipeline || !streak)
            {
                return false;
            }
            LuminousLensStreakConstants k{};
            k.size = simd_make_uint2(width, height);
            k.texel = simd_make_float2(1.f / (F32)width, 1.f / (F32)height);

            if (!encoder->beginComputePass({ "luminous lens streaks" }))
            {
                return false;
            }
            const RHI::NativeStage stage = RHI::NativeStage::Compute;
            encoder->setComputePipeline(kernel);
            RHI::setNativeConstants(*encoder, stage, LUMINOUS_LENS_BUFFER_PARAMS, params);
            RHI::setNativeConstants(*encoder, stage, LUMINOUS_LENS_BUFFER_PASS, k);
            encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_FRAME, frame);
            encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_EXPOSURE, exposure);
            encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_DEPTH, depth);
            encoder->setNativeTexture(stage, 3, streak, RHI::Access::Write, { 0, 1 });
            encoder->dispatchThreadgroups({ 1, height, 1 }, { LUMINOUS_LENS_STREAK_THREADS, 1, 1 });
            encoder->endComputePass();

            return drawFullscreen(*encoder, frame, pipeline, true, "luminous lens streaks", params, [&]
            {
                RHI::setNativeConstants(*encoder, RHI::NativeStage::Fragment, LUMINOUS_LENS_BUFFER_PASS, k);
                encoder->setNativeTexture(RHI::NativeStage::Fragment, 3, streak);
            });
        }

        // ---- lens flare ---------------------------------------------------------------------------------------

        /// Every level of texture below level 0, each the 2x2 box of the one above (as GL's own mip levels are), with
        /// the common downsampler, in the open compute pass: up to twelve levels a dispatch, six while level 0 is
        /// wider than it can take more for.
        bool makeMipLevels(RHI::Encoder& encoder, RHI::Texture* texture)
        {
            RHI::ComputePipeline* spd = computePipeline(programs().common, "luminous_spd_downsample");
            RHI::Device* device = RHI::viewerDevice();
            LensTextures& t = lensTextures();
            if (!spd || !device)
            {
                return false;
            }
            constexpr U32 MAX_GROUPS = LUMINOUS_SPD_MAX_GROUPS;
            if (t.spdCounter.isNull() || t.spdTail.isNull())
            {
                const std::array<std::byte, 16> zero{};
                t.spdCounter = device->createBuffer({ 16, RHI::BUFFER_STORAGE, false }, zero, "luminous lens downsampler count");
                t.spdTail = device->createBuffer({ U64(MAX_GROUPS) * MAX_GROUPS * 16, RHI::BUFFER_STORAGE, false }, {},
                                                 "luminous lens downsampler tail");
                if (t.spdCounter.isNull() || t.spdTail.isNull())
                {
                    return false;
                }
            }
            const RHI::TextureDesc& desc = texture->desc();
            const RHI::NativeStage stage = RHI::NativeStage::Compute;
            encoder.setComputePipeline(spd);
            encoder.setNativeBuffer(stage, LUMINOUS_SPD_BUFFER_COUNTER, t.spdCounter.get(), 0, RHI::Access::ReadWrite);
            encoder.setNativeBuffer(stage, LUMINOUS_SPD_BUFFER_TAIL, t.spdTail.get(), 0, RHI::Access::ReadWrite);
            U32 base = 0;
            while (base + 1 < desc.mipLevels)
            {
                const U32 width = levelSize(desc.width, base);
                const U32 height = levelSize(desc.height, base);
                const RHI::DispatchSize groups = groupsFor(width, height, LUMINOUS_SPD_TILE);
                U32 mips = llmin(desc.mipLevels - 1 - base, (U32)LUMINOUS_SPD_MAX_MIPS);
                if (mips > 6 && (groups.x > MAX_GROUPS || groups.y > MAX_GROUPS))
                {
                    mips = 6;
                }
                LuminousSPDConstants k{};
                k.sourceSize = simd_make_uint2(width, height);
                k.groups = simd_make_uint2(groups.x, groups.y);
                k.mips = mips;
                RHI::setNativeConstants(encoder, stage, LUMINOUS_SPD_BUFFER_CONSTANTS, k);
                encoder.setNativeTexture(stage, LUMINOUS_SPD_TEXTURE_SOURCE, texture, RHI::Access::Read, { base, 1 });
                for (U32 level = 1; level <= LUMINOUS_SPD_MAX_MIPS; ++level)
                {
                    if (level <= mips)
                    {
                        encoder.setNativeTexture(stage, LUMINOUS_SPD_TEXTURE_FIRST_DEST + level - 1, texture, RHI::Access::Write,
                                                 { base + level, 1 });
                    }
                    else
                    {
                        encoder.setNativeTexture(stage, LUMINOUS_SPD_TEXTURE_FIRST_DEST + level - 1, nullptr);
                    }
                }
                encoder.dispatchThreadgroups(groups, { LUMINOUS_SPD_THREADS, 1, 1 });
                base += mips;
            }
            for (U32 slot = 0; slot <= LUMINOUS_SPD_MAX_MIPS; ++slot)
            {
                encoder.setNativeTexture(stage, slot, nullptr);
            }
            return true;
        }

        bool drawLensFlare(PassContext& context, PassTargets& targets)
        {
            RHI::Texture* frame = colour(targets.frame);
            RHI::Texture* exposure = context.exposure ? colour(context.exposure) : nullptr;
            RHI::Texture* depth = context.depth ? context.depth->getRHIDepth() : nullptr;
            RHI::Encoder* encoder = encoderFor(frame);
            LLNativeProgram& lens = programs().lens;
            RHI::ComputePipeline* bright_kernel = computePipeline(lens, "luminous_lens_flare_bright");
            RHI::ComputePipeline* down_kernel = computePipeline(lens, "luminous_lens_bloom_down");
            RHI::ComputePipeline* flare_kernel = computePipeline(lens, "luminous_lens_flare");
            if (!encoder || !exposure || !depth || !bright_kernel || !down_kernel || !flare_kernel)
            {
                return false;
            }
            LuminousLensParams params;
            LuminousPhotoMode::fillLensParams(params, targets.frame->getWidth(), targets.frame->getHeight());
            if (params.flare <= 0.f)
            {
                return false;
            }
            RHI::Pipeline* pipeline = renderPipeline("luminous_lens_flare_fs", frame->desc().format, Blend::Add);
            if (!pipeline)
            {
                return false;
            }
            const U32 half_w = llmax(targets.frame->getWidth() / 2, 1U);
            const U32 half_h = llmax(targets.frame->getHeight() / 2, 1U);
            const U32 quarter_w = llmax(half_w / 2, 1U);
            const U32 quarter_h = llmax(half_h / 2, 1U);
            // Star blur reads the starburst's light from coarser levels: every level then, as GL's own mip levels.
            const bool mips = params.flareStarBlur > 0.f;
            LensTextures& t = lensTextures();
            RHI::Texture* bright = t.bright.get(storageDesc(half_w, half_h, 1), "luminous lens flare light");
            RHI::Texture* core = t.core.get(storageDesc(half_w, half_h, mips ? RHI::fullMipCount(half_w, half_h) : 1,
                                                        RHI::Format::R16Float),
                                            "luminous lens flare starburst light");
            RHI::Texture* soft = t.soft.get(storageDesc(quarter_w, quarter_h, 1), "luminous lens flare soft");
            RHI::Texture* flare = t.flare.get(storageDesc(half_w, half_h, 1), "luminous lens flare");
            if (!bright || !core || !soft || !flare)
            {
                return false;
            }
            LuminousLensFlareConstants k{};
            k.halfSize = simd_make_uint2(half_w, half_h);
            k.quarterSize = simd_make_uint2(quarter_w, quarter_h);
            const RHI::DispatchSize half_groups = groupsFor(half_w, half_h, GROUP);

            if (!encoder->beginComputePass({ "luminous lens flare" }))
            {
                return false;
            }
            const RHI::NativeStage stage = RHI::NativeStage::Compute;
            // the light bright enough to flare, and the starburst's
            encoder->setComputePipeline(bright_kernel);
            RHI::setNativeConstants(*encoder, stage, LUMINOUS_LENS_BUFFER_PARAMS, params);
            RHI::setNativeConstants(*encoder, stage, LUMINOUS_LENS_BUFFER_PASS, k);
            encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_FRAME, frame);
            encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_EXPOSURE, exposure);
            encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_DEPTH, depth);
            encoder->setNativeTexture(stage, 3, bright, RHI::Access::Write, { 0, 1 });
            encoder->setNativeTexture(stage, 4, core, RHI::Access::Write, { 0, 1 });
            encoder->dispatchThreadgroups(half_groups, { GROUP, GROUP, 1 });
            for (U32 slot = 0; slot <= 4; ++slot)
            {
                encoder->setNativeTexture(stage, slot, nullptr);
            }

            // a softer quarter-size copy: the down chain's first step
            LuminousLensDownConstants down{};
            down.sizeA = simd_make_uint2(quarter_w, quarter_h);
            down.sizeB = simd_make_uint2(0, 0);
            down.srcTexel = simd_make_float2(1.f / (F32)half_w, 1.f / (F32)half_h);
            down.hasB = 0;
            encoder->setComputePipeline(down_kernel);
            RHI::setNativeConstants(*encoder, stage, LUMINOUS_LENS_BUFFER_PASS, down);
            encoder->setNativeTexture(stage, 0, bright, RHI::Access::Read, { 0, 1 });
            encoder->setNativeTexture(stage, 1, soft, RHI::Access::Write, { 0, 1 });
            encoder->dispatchThreadgroups(groupsFor(quarter_w, quarter_h, LUMINOUS_LENS_DOWN_TILE), { GROUP, GROUP, 1 });
            encoder->setNativeTexture(stage, 0, nullptr);
            encoder->setNativeTexture(stage, 1, nullptr);

            if (mips && !makeMipLevels(*encoder, core))
            {
                encoder->endComputePass();
                return false;
            }

            // ghosts, halo and starburst
            encoder->setComputePipeline(flare_kernel);
            RHI::setNativeConstants(*encoder, stage, LUMINOUS_LENS_BUFFER_PARAMS, params);
            RHI::setNativeConstants(*encoder, stage, LUMINOUS_LENS_BUFFER_PASS, k);
            encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_EXPOSURE, exposure);
            encoder->setNativeTexture(stage, LUMINOUS_LENS_TEXTURE_DEPTH, depth);
            encoder->setNativeTexture(stage, 3, bright);
            encoder->setNativeTexture(stage, 4, soft);
            encoder->setNativeTexture(stage, 5, core);
            encoder->setNativeTexture(stage, 6, flare, RHI::Access::Write, { 0, 1 });
            encoder->dispatchThreadgroups(half_groups, { GROUP, GROUP, 1 });
            encoder->endComputePass();

            // added to the frame, with the dirt on the lens
            return drawFullscreen(*encoder, frame, pipeline, true, "luminous lens flare", params, [&]
            {
                encoder->setNativeTexture(RHI::NativeStage::Fragment, 4, soft);
                encoder->setNativeTexture(RHI::NativeStage::Fragment, 6, flare);
            });
        }

        // ---- chromatic aberration, distortion and glitch ------------------------------------------------------

        bool distortionPending(const PassContext& context, const LuminousLensParams& params)
        {
            return params.distortion != 0.f && on(Effect::Distortion) && passReady(Pass::Distortion) &&
                   !drawnAlready(context, Pass::Distortion);
        }

        bool glitchPending(const PassContext& context, const LuminousLensParams& params)
        {
            return params.glitch > 0.f && on(Effect::Glitch) && passReady(Pass::Glitch) && !drawnAlready(context, Pass::Glitch);
        }

        /// The frame drawn anew into the other target by the finish pass in this mode (LuminousLensFinishMode).
        bool drawFinish(PassTargets& targets, const LuminousLensParams& params, U32 mode)
        {
            RHI::Texture* frame = colour(targets.frame);
            RHI::Texture* other = colour(targets.other);
            RHI::Encoder* encoder = encoderFor(frame);
            if (!encoder || !other)
            {
                return false;
            }
            RHI::Pipeline* pipeline = renderPipeline("luminous_lens_finish_fs", other->desc().format, Blend::None);
            if (!pipeline)
            {
                return false;
            }
            LuminousLensFinishConstants k{};
            k.mode = mode;
            if (!drawFullscreen(*encoder, other, pipeline, false, "luminous lens finish", params, [&]
                {
                    RHI::setNativeConstants(*encoder, RHI::NativeStage::Fragment, LUMINOUS_LENS_BUFFER_PASS, k);
                    encoder->setNativeTexture(RHI::NativeStage::Fragment, LUMINOUS_LENS_TEXTURE_FRAME, frame);
                }))
            {
                return false;
            }
            targets.swapped = true;
            return true;
        }

        // Aberration draws distortion with it, at the point distortion reads; glitch then draws on its own.
        bool drawAberration(PassContext& context, PassTargets& targets)
        {
            LuminousLensParams params;
            LuminousPhotoMode::fillLensParams(params, targets.frame->getWidth(), targets.frame->getHeight());
            if (params.aberration <= 0.f)
            {
                return false; // at 0 it moves nothing
            }
            const bool distortion = distortionPending(context, params);
            if (!drawFinish(targets, params, LUMINOUS_LENS_FINISH_ABERRATION | (distortion ? LUMINOUS_LENS_FINISH_DISTORTION : 0u)))
            {
                return false;
            }
            if (distortion)
            {
                context.drawnThisFrame |= bit(Pass::Distortion);
            }
            return true;
        }

        // Without aberration, distortion draws glitch with it: glitch reads through the distortion.
        bool drawDistortion(PassContext& context, PassTargets& targets)
        {
            LuminousLensParams params;
            LuminousPhotoMode::fillLensParams(params, targets.frame->getWidth(), targets.frame->getHeight());
            if (params.distortion == 0.f)
            {
                return false;
            }
            const bool glitch = glitchPending(context, params);
            if (!drawFinish(targets, params, LUMINOUS_LENS_FINISH_DISTORTION | (glitch ? LUMINOUS_LENS_FINISH_GLITCH : 0u)))
            {
                return false;
            }
            if (glitch)
            {
                context.drawnThisFrame |= bit(Pass::Glitch);
            }
            return true;
        }

        bool drawGlitch(PassContext& /*context*/, PassTargets& targets)
        {
            LuminousLensParams params;
            LuminousPhotoMode::fillLensParams(params, targets.frame->getWidth(), targets.frame->getHeight());
            return params.glitch > 0.f && drawFinish(targets, params, LUMINOUS_LENS_FINISH_GLITCH);
        }
    }

    void registerLens(PassTable& table)
    {
        table.fn[size_t(Pass::Vignette)] = drawVignette;
        table.fn[size_t(Pass::BloomChain)] = drawBloomChain;
        table.fn[size_t(Pass::Clarity)] = drawClarity;
        table.fn[size_t(Pass::Halation)] = drawHalation;
        table.fn[size_t(Pass::Bloom)] = drawBloom;
        table.fn[size_t(Pass::Anamorphic)] = drawStreaks;
        table.fn[size_t(Pass::LensFlare)] = drawLensFlare;
        table.fn[size_t(Pass::ChromaticAberration)] = drawAberration;
        table.fn[size_t(Pass::Distortion)] = drawDistortion;
        table.fn[size_t(Pass::Glitch)] = drawGlitch;

        // Loaded from the idle loop once the viewer is in the world; let go with the device.
        doOnIdleRepeating(poll);
        LL::RHI::onViewerDeviceRelease([]
        {
            Programs& p = programs();
            p.device = nullptr;
            p.ready = false;
            setAllReady(false);
            releaseResources();
        });
    }

    void releaseLensTargets()
    {
        releaseResources();
    }
}
