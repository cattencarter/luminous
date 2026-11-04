/**
 * @file luminousmetalscreen.cpp
 * @brief Photo mode on Metal: screen-space AO, contact shadows and reflections, the group's native passes.
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
#include "pipeline.h"

#include "app_settings/shaders/metal/luminous/common/commonshared.h"

#include <algorithm>
#include <array>
#include <cstring>

// The screen-space group's native passes on Metal. Only GTAO at half size (the live view with low quality
// live on) is a pass of Luminous's own here; GTAO at full size in the sun pass, contact shadows, AO on lamps
// and enhanced reflections are hosted in Firestorm's programs and translated with them.
//
// The half-size GTAO is one compute pass of three dispatches and one native render pass:
// 1. the depth pyramid (depthpyramid.metal): five levels of linear view depth from the depth buffer, in one
//    dispatch; any later pass that gathers or marches over depth can build one the same way
//    (buildDepthPyramid, below);
// 2. GTAO at half size (gtao.metal), the reference's math texel for texel, its wide samples from the pyramid;
// 3. a 3 x 3 denoise within each surface, from a tile in threadgroup memory;
// 4. the upsample into the light map's green as luminousGTAOUpF.glsl does it, blended (the destination
//    multiplied), so Firestorm's target is never written from compute.
// Firestorm's softening of the light map follows, as after the translated pass.
//
// Enhanced reflections read the scene map's mip levels for glossy surfaces. On Metal the scene map has none, so
// each time it is drawn while they are drawn, a chain of the group's own is made from it: level 0 copied, then in
// one compute pass the levels below, with the common downsampler (spd.metal) while the level above has even sides
// and scenemips.metal's bilinear step below an odd side. The reflections bind the chain in the scene map's place.
namespace LuminousMetal
{
    /// Records the depth pyramid of depth into pyramid inside an open compute pass: levels (at most
    /// LUMINOUS_PYRAMID_MAX_LEVELS, and no more than pyramid has) of positive linear view depth, the first at
    /// half of depth's size. pyramid is made with depthPyramidDesc. mode is a LuminousDepthPyramidMode; radius
    /// (metres) is the weighted mode's. inv_proj is the inverse of the projection the depth was drawn with.
    /// False, with nothing recorded, when the library or its kernel is not ready.
    bool buildDepthPyramid(LL::RHI::Encoder& encoder, LL::RHI::Texture* depth, LL::RHI::Texture* pyramid,
                           const glm::mat4& inv_proj, U32 mode, F32 radius);

    /// The pyramid's texture for a depth buffer of width x height: a half of its size, at most
    /// LUMINOUS_PYRAMID_MAX_LEVELS levels, sampled and written by kernels.
    LL::RHI::TextureDesc depthPyramidDesc(U32 width, U32 height, LL::RHI::Format format = LL::RHI::Format::R16Float);

    namespace
    {
        using namespace LL::RHI;

        constexpr const char* MANIFEST = "luminous/screen/programs.json";
        constexpr const char* LIBRARY = "screen";
        constexpr const char* KERNEL_PYRAMID = "luminous_depth_pyramid";
        constexpr const char* KERNEL_GTAO = "luminous_gtao_half";
        constexpr const char* KERNEL_DENOISE = "luminous_gtao_denoise";
        constexpr const char* VERTEX_FULLSCREEN = "luminous_fullscreen_vs";
        constexpr const char* FRAGMENT_UP = "luminous_gtao_up_fs";
        constexpr const char* KERNEL_SPD = "luminous_spd_downsample";
        constexpr const char* KERNEL_MIP_DOWN = "luminous_scene_mip_down";

        /// Samples further out than 2^(1 + this) frame pixels read the pyramid (XeGTAO's 3.3).
        constexpr F32 MIP_OFFSET = 3.3f;
        /// The denoise between GTAO and the upsample.
        constexpr bool DENOISE = true;

        LLNativeProgram sProgram;
        bool sLoaded = false;   // sProgram.load was called on the current device
        bool sPolling = false;  // the idle poll below is registered
        bool sReleaseHooked = false;

        // The pass's own textures, made again when the frame's size changes.
        enum PoolTexture : U32
        {
            POOL_PYRAMID = 0,
            POOL_RAW,
            POOL_DENOISED,
            POOL_COUNT
        };
        Ref<TransientPool> sPool;
        U32 sPoolWidth = 0;
        U32 sPoolHeight = 0;

        // The scene map's mip chain for enhanced reflections, made again when the scene map's size or format
        // changes, and the common downsampler's counter and per-threadgroup buffer.
        Ref<TransientPool> sSceneMips;
        TextureDesc sSceneMipsDesc;
        bool sSceneMipsCurrent = false; // made from the scene map's latest drawing
        Ref<Buffer> sSPDCounter;
        Ref<Buffer> sSPDTail;

        void releaseSceneMips()
        {
            sSceneMips = nullptr;
            sSceneMipsCurrent = false;
        }

        U32 pyramidLevels(U32 width, U32 height)
        {
            U32 side = std::max(std::max(width / 2, height / 2), 1U);
            U32 levels = 1;
            while (side > 1 && levels < LUMINOUS_PYRAMID_MAX_LEVELS)
            {
                side /= 2;
                ++levels;
            }
            return levels;
        }

        simd_float4x4 toSimd(const glm::mat4& m)
        {
            simd_float4x4 out;
            static_assert(sizeof(out) == sizeof(m), "both are four columns of four floats");
            std::memcpy(&out, &m, sizeof(out)); // column-major on both sides
            return out;
        }

        PipelineDesc upsampleDesc(Format light)
        {
            PipelineDesc desc;
            desc.colorCount = 1;
            desc.colorFormats[0] = light;
            desc.depthFormat = Format::Unknown;
            // The light map times the result (GL's BF_ZERO, BF_SOURCE_COLOR), green only: the reference writes
            // red and blue as 1 into the same multiply and leaves alpha alone.
            desc.blend.enabled = true;
            desc.blend.srcColor = BlendFactor::Zero;
            desc.blend.dstColor = BlendFactor::SourceColor;
            desc.blend.srcAlpha = BlendFactor::Zero;
            desc.blend.dstAlpha = BlendFactor::One;
            desc.colorMask = COLOR_MASK_G;
            return desc;
        }

        Pipeline* upsamplePipeline(Format light)
        {
            return sProgram.render(VERTEX_FULLSCREEN, FRAGMENT_UP, upsampleDesc(light));
        }

        LLRenderTarget* lightTarget()
        {
            return gPipeline.mRT ? &gPipeline.mRT->deferredLight : nullptr;
        }

        void startPolling();

        void onDeviceRelease()
        {
            // LLNativeProgram lets go of its library itself; the pool goes with the device too.
            sPool = nullptr;
            sPoolWidth = 0;
            sPoolHeight = 0;
            releaseSceneMips();
            sSPDCounter = nullptr;
            sSPDTail = nullptr;
            sLoaded = false;
            setReady(Pass::HalfGTAO, false);
            startPolling(); // for a device made again later
        }

        // Once per idle frame until the library and every pipeline are ready (or failed): loading waits for the
        // login, so the compilation runs in the background of the world's first frames.
        bool poll()
        {
            if (!metal())
            {
                sPolling = false;
                return true;
            }
            Device* device = viewerDevice();
            if (!device || LLStartUp::getStartupState() < STATE_STARTED)
            {
                return false;
            }
            const Capabilities& caps = device->capabilities();
            if (!caps.nativePrograms || !caps.computePasses)
            {
                sPolling = false;
                return true;
            }
            if (!sReleaseHooked)
            {
                sReleaseHooked = true;
                onViewerDeviceRelease(onDeviceRelease);
            }
            if (!sLoaded)
            {
                sLoaded = true;
                if (!sProgram.load(MANIFEST, LIBRARY))
                {
                    LL_WARNS("Luminous") << "Half-size GTAO on Metal: the screen library could not be loaded; GTAO "
                                         << "is drawn at full size" << LL_ENDL;
                    sPolling = false;
                    return true;
                }
            }
            const CompileStatus library = sProgram.status();
            if (library == CompileStatus::Pending)
            {
                return false;
            }
            if (library == CompileStatus::Failed)
            {
                LL_WARNS("Luminous") << "Half-size GTAO on Metal: the screen library failed; GTAO is drawn at full "
                                     << "size. " << sProgram.log() << LL_ENDL;
                sPolling = false;
                return true;
            }
            // The scene map's mip kernels compile beside the rest; until they are ready, or if they fail, the
            // reflections read the scene map's level 0 alone, and the half-size GTAO does not wait for them.
            sProgram.compute(KERNEL_SPD);
            sProgram.compute(KERNEL_MIP_DOWN);

            LLRenderTarget* light = lightTarget();
            Texture* light_texture = light ? light->getRHITexture(0) : nullptr;
            if (!light_texture)
            {
                return false; // the targets come with the first frames
            }
            bool pending = false;
            bool failed = false;
            const auto check = [&](CompileStatus status) {
                pending = pending || status == CompileStatus::Pending;
                failed = failed || status == CompileStatus::Failed;
            };
            for (const char* kernel : { KERNEL_PYRAMID, KERNEL_GTAO, KERNEL_DENOISE })
            {
                ComputePipeline* pipeline = sProgram.compute(kernel);
                check(pipeline ? pipeline->status() : CompileStatus::Failed);
            }
            Pipeline* up = upsamplePipeline(light_texture->desc().format);
            check(up ? up->status() : CompileStatus::Failed);
            if (failed)
            {
                LL_WARNS("Luminous") << "Half-size GTAO on Metal: a pipeline failed; GTAO is drawn at full size"
                                     << LL_ENDL;
                sPolling = false;
                return true;
            }
            if (pending)
            {
                return false;
            }
            setReady(Pass::HalfGTAO, true);
            sPolling = false;
            return true;
        }

        void startPolling()
        {
            if (!sPolling)
            {
                sPolling = true;
                doOnIdleRepeating(poll);
            }
        }

        bool ensurePool(Device& device, U32 width, U32 height)
        {
            if (sPool.notNull() && sPoolWidth == width && sPoolHeight == height)
            {
                return true;
            }
            sPool = nullptr;
            const U32 half_width = std::max(width / 2, 1U);
            const U32 half_height = std::max(height / 2, 1U);

            TransientPoolDesc desc;
            desc.name = "luminous gtao";
            TransientTextureDesc pyramid;
            pyramid.desc = depthPyramidDesc(width, height);
            pyramid.firstPhase = 0;
            pyramid.lastPhase = 1;
            pyramid.name = "luminous depth pyramid";
            TransientTextureDesc raw;
            raw.desc.type = TextureType::Tex2D;
            raw.desc.format = Format::R16Float;
            raw.desc.width = half_width;
            raw.desc.height = half_height;
            raw.desc.usage = TEXTURE_SAMPLED | TEXTURE_STORAGE;
            raw.firstPhase = 1;
            raw.lastPhase = 2;
            raw.name = "luminous gtao half";
            TransientTextureDesc denoised = raw;
            denoised.firstPhase = 2;
            denoised.lastPhase = 3;
            denoised.name = "luminous gtao denoised";
            desc.textures = { pyramid, raw, denoised };

            Ref<TransientPool> pool = device.createTransientPool(desc);
            if (pool.isNull() || pool->textureCount() < POOL_COUNT)
            {
                return false;
            }
            for (U32 i = 0; i < POOL_COUNT; ++i)
            {
                if (!pool->texture(i))
                {
                    return false;
                }
            }
            sPool = pool;
            sPoolWidth = width;
            sPoolHeight = height;
            return true;
        }

        bool ready(ComputePipeline* pipeline)
        {
            return pipeline && pipeline->status() == CompileStatus::Ready;
        }

        // The native HalfGTAO: draws into targets.light's green what renderHalfGTAO's two programs draw.
        bool drawHalfGTAO(PassContext& context, PassTargets& targets)
        {
            (void)context;
            Device* device = viewerDevice();
            LLRenderTarget* light = targets.light;
            if (!device || !light || !gPipeline.mRT)
            {
                return false;
            }
            if (!sProgram.ready())
            {
                // The device was made again, or the library let go: load it again before the next draw.
                setReady(Pass::HalfGTAO, false);
                sLoaded = false;
                startPolling();
                return false;
            }
            LLRenderTarget& deferred = gPipeline.mRT->deferredScreen;
            Texture* depth = deferred.getRHIDepth();
            Texture* albedo = deferred.getRHITexture(0);
            Texture* normals = deferred.getRHITexture(2);
            Texture* light_texture = light->getRHITexture(0);
            ComputePipeline* gtao = sProgram.compute(KERNEL_GTAO);
            ComputePipeline* denoise = sProgram.compute(KERNEL_DENOISE);
            Pipeline* up = light_texture ? upsamplePipeline(light_texture->desc().format) : nullptr;
            const bool failed = !gtao || !denoise || !up || gtao->status() == CompileStatus::Failed ||
                                denoise->status() == CompileStatus::Failed || up->status() == CompileStatus::Failed;
            if (failed)
            {
                // A pipeline that cannot be made: the sun pass draws GTAO at full size again from the next frame,
                // instead of leaving the light map without AO for the session.
                LL_WARNS("LuminousMetal") << "Half-size GTAO on Metal: a pipeline failed; GTAO falls back to the sun pass"
                                          << LL_ENDL;
                setReady(Pass::HalfGTAO, false);
                return false;
            }
            if (!depth || !albedo || !normals || !light_texture || !ready(gtao) || !ready(denoise))
            {
                return false;
            }

            // As the reference: the frame's size from the G-buffer (bindDeferredShader's screen_res), the half size
            // from the light map (renderHalfGTAO's w and h).
            const U32 width = deferred.getWidth();
            const U32 height = deferred.getHeight();
            const U32 half_width = std::max(light->getWidth() / 2, 1U);
            const U32 half_height = std::max(light->getHeight() / 2, 1U);
            if (!ensurePool(*device, width, height) || sPool->texture(POOL_RAW)->desc().width != half_width ||
                sPool->texture(POOL_RAW)->desc().height != half_height)
            {
                return false;
            }
            Texture* pyramid = sPool->texture(POOL_PYRAMID);
            Texture* raw = sPool->texture(POOL_RAW);
            Texture* denoised = sPool->texture(POOL_DENOISED);

            LuminousScreenParams params;
            LuminousPhotoMode::fillScreenParams(params, half_width, half_height);
            // inv_proj as the sun pass and the reference's programs get it: the inverse of LLRender's projection.
            const glm::mat4 inv_proj = glm::inverse(gGL.getProjectionMatrix());

            LuminousGTAOConstants constants{};
            constants.invProj = toSimd(inv_proj);
            constants.screenRes = simd_make_float2((F32)width, (F32)height);
            constants.halfSize = simd_make_float2((F32)half_width, (F32)half_height);
            constants.radius = params.aoRadius;
            constants.strength = params.aoStrength;
            constants.frame = params.frame;
            constants.slices = params.aoSlices;
            constants.steps = params.aoSteps;
            constants.mipOffset = MIP_OFFSET;
            constants.pyramidLevels = pyramid->desc().mipLevels;
            constants.denoise = DENOISE ? 1U : 0U;

            Encoder& encoder = device->encoder();
            LL_PROFILE_GPU_ZONE("luminous gtao half (native)");
            if (!encoder.beginComputePass({ "luminous gtao half" }))
            {
                return false;
            }
            const U32 threads = LUMINOUS_GTAO_THREADS;
            bool drawn = buildDepthPyramid(encoder, depth, pyramid, inv_proj, LUMINOUS_PYRAMID_WEIGHTED, params.aoRadius);
            if (drawn)
            {
                encoder.beginTransientPhase(sPool.get(), 1);
                encoder.setComputePipeline(gtao);
                setNativeConstants(encoder, NativeStage::Compute, LUMINOUS_SCREEN_BUFFER_PARAMS, constants);
                for (U32 slot = LUMINOUS_PYRAMID_TEXTURE_FIRST_LEVEL;
                     slot < LUMINOUS_PYRAMID_TEXTURE_FIRST_LEVEL + LUMINOUS_PYRAMID_MAX_LEVELS; ++slot)
                {
                    encoder.setNativeTexture(NativeStage::Compute, slot, nullptr); // the pyramid's write views
                }
                encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_SCREEN_TEXTURE_DEPTH, depth);
                encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_SCREEN_TEXTURE_NORMAL, normals);
                encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_SCREEN_TEXTURE_ALBEDO, albedo);
                encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_SCREEN_TEXTURE_PYRAMID, pyramid);
                encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_SCREEN_TEXTURE_OUT, raw, Access::Write, { 0, 1 });
                encoder.dispatchThreads({ half_width, half_height, 1 }, { threads, threads, 1 });

                encoder.beginTransientPhase(sPool.get(), 2);
                encoder.setComputePipeline(denoise);
                encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_SCREEN_TEXTURE_NORMAL, nullptr);
                encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_SCREEN_TEXTURE_ALBEDO, nullptr);
                encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_SCREEN_TEXTURE_PYRAMID, nullptr);
                encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_SCREEN_TEXTURE_AO, raw);
                encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_SCREEN_TEXTURE_OUT, denoised, Access::Write,
                                         { 0, 1 });
                encoder.dispatchThreadgroups({ (half_width + threads - 1) / threads, (half_height + threads - 1) / threads, 1 },
                                             { threads, threads, 1 });
            }
            encoder.endComputePass();
            if (!drawn)
            {
                return false;
            }

            // Into the light map's green, which the sun pass left at 1.
            encoder.beginTransientPhase(sPool.get(), 3);
            RenderPassDesc pass;
            pass.native = true;
            pass.name = "luminous gtao up";
            pass.colorCount = 1;
            pass.color[0].texture = light_texture;
            pass.color[0].load = LoadAction::Load;
            pass.color[0].store = StoreAction::Store;
            if (!encoder.beginRenderPass(pass))
            {
                return false;
            }
            encoder.setPipeline(up);
            setNativeConstants(encoder, NativeStage::Fragment, LUMINOUS_SCREEN_BUFFER_PARAMS, constants);
            encoder.setNativeTexture(NativeStage::Fragment, LUMINOUS_SCREEN_TEXTURE_DEPTH, depth);
            encoder.setNativeTexture(NativeStage::Fragment, LUMINOUS_SCREEN_TEXTURE_AO, denoised);
            encoder.draw(PrimitiveType::Triangles, 0, 3);
            encoder.endRenderPass();
            return true;
        }
    }

    namespace
    {
        U32 mipSize(U32 base, U32 level)
        {
            return std::max(base >> level, 1U);
        }

        /// A side the 2x2 box halves exactly: even, or 1 (its reads clamp to the one row or column).
        bool halvesExactly(U32 side)
        {
            return side == 1 || side % 2 == 0;
        }

        /// The chain for a scene map of desc, made again when it changed: every level down to 1 x 1.
        Texture* sceneMipTexture(Device& device, const TextureDesc& scene)
        {
            TextureDesc wanted;
            wanted.type = TextureType::Tex2D;
            wanted.format = scene.format;
            wanted.width = scene.width;
            wanted.height = scene.height;
            U32 levels = 1;
            for (U32 side = std::max(scene.width, scene.height); side > 1; side /= 2)
            {
                ++levels;
            }
            wanted.mipLevels = levels;
            wanted.usage = TEXTURE_SAMPLED | TEXTURE_STORAGE;
            if (sSceneMips.isNull() || !(sSceneMipsDesc == wanted))
            {
                sSceneMips = nullptr;
                TransientPoolDesc pool_desc;
                pool_desc.name = "luminous scene mips";
                pool_desc.textures.push_back({ wanted, 0, 0, "luminous scene mips" });
                sSceneMips = device.createTransientPool(pool_desc);
                sSceneMipsDesc = wanted;
            }
            return sSceneMips.notNull() && sSceneMips->textureCount() > 0 ? sSceneMips->texture(0) : nullptr;
        }

        bool spdBuffers(Device& device)
        {
            if (sSPDCounter.isNull() || sSPDTail.isNull())
            {
                const std::array<std::byte, 16> zero{};
                sSPDCounter = device.createBuffer({ 16, BUFFER_STORAGE, false }, zero, "luminous scene mips count");
                sSPDTail = device.createBuffer({ U64(LUMINOUS_SPD_MAX_GROUPS) * LUMINOUS_SPD_MAX_GROUPS * 16,
                                                 BUFFER_STORAGE, false }, {}, "luminous scene mips tail");
            }
            return sSPDCounter.notNull() && sSPDTail.notNull();
        }

        /// Records the chain's levels below level 0 in an open compute pass: runs of levels whose level above has
        /// sides the 2x2 box halves exactly go to the common downsampler (up to twelve a dispatch, six while level 0
        /// needs more threadgroups than its last threadgroup can gather), any other level to the bilinear step.
        void recordSceneMips(Encoder& encoder, Texture* chain, ComputePipeline* spd, ComputePipeline* down)
        {
            const NativeStage cs = NativeStage::Compute;
            const TextureDesc& desc = chain->desc();
            U32 base = 0;
            while (base + 1 < desc.mipLevels)
            {
                const U32 width = mipSize(desc.width, base);
                const U32 height = mipSize(desc.height, base);
                const U32 groups_x = (width + LUMINOUS_SPD_TILE - 1) / LUMINOUS_SPD_TILE;
                const U32 groups_y = (height + LUMINOUS_SPD_TILE - 1) / LUMINOUS_SPD_TILE;
                U32 cap = std::min<U32>(desc.mipLevels - 1 - base, LUMINOUS_SPD_MAX_MIPS);
                if (groups_x > LUMINOUS_SPD_MAX_GROUPS || groups_y > LUMINOUS_SPD_MAX_GROUPS)
                {
                    cap = std::min<U32>(cap, 6);
                }
                U32 exact = 0;
                while (exact < cap && halvesExactly(mipSize(desc.width, base + exact)) &&
                       halvesExactly(mipSize(desc.height, base + exact)))
                {
                    ++exact;
                }

                if (exact > 0)
                {
                    LuminousSPDConstants k{};
                    k.sourceSize = simd_make_uint2(width, height);
                    k.groups = simd_make_uint2(groups_x, groups_y);
                    k.mips = exact;
                    encoder.setComputePipeline(spd);
                    setNativeConstants(encoder, cs, LUMINOUS_SPD_BUFFER_CONSTANTS, k);
                    encoder.setNativeBuffer(cs, LUMINOUS_SPD_BUFFER_COUNTER, sSPDCounter.get(), 0, Access::ReadWrite);
                    encoder.setNativeBuffer(cs, LUMINOUS_SPD_BUFFER_TAIL, sSPDTail.get(), 0, Access::ReadWrite);
                    encoder.setNativeTexture(cs, LUMINOUS_SPD_TEXTURE_SOURCE, chain, Access::Read, { base, 1 });
                    for (U32 level = 1; level <= LUMINOUS_SPD_MAX_MIPS; ++level)
                    {
                        const U32 slot = LUMINOUS_SPD_TEXTURE_FIRST_DEST + level - 1;
                        if (level <= exact)
                        {
                            encoder.setNativeTexture(cs, slot, chain, Access::Write, { base + level, 1 });
                        }
                        else
                        {
                            encoder.setNativeTexture(cs, slot, nullptr);
                        }
                    }
                    encoder.dispatchThreadgroups({ groups_x, groups_y, 1 }, { LUMINOUS_SPD_THREADS, 1, 1 });
                    encoder.setNativeBuffer(cs, LUMINOUS_SPD_BUFFER_COUNTER, nullptr, 0);
                    encoder.setNativeBuffer(cs, LUMINOUS_SPD_BUFFER_TAIL, nullptr, 0);
                    base += exact;
                }
                else
                {
                    LuminousSceneMipConstants k{};
                    k.sourceSize = simd_make_uint2(width, height);
                    k.destSize = simd_make_uint2(mipSize(desc.width, base + 1), mipSize(desc.height, base + 1));
                    encoder.setComputePipeline(down);
                    setNativeConstants(encoder, cs, LUMINOUS_SCENE_MIP_BUFFER_CONSTANTS, k);
                    encoder.setNativeTexture(cs, LUMINOUS_SCENE_MIP_TEXTURE_SOURCE, chain, Access::Read, { base, 1 });
                    encoder.setNativeTexture(cs, LUMINOUS_SCENE_MIP_TEXTURE_DEST, chain, Access::Write, { base + 1, 1 });
                    for (U32 slot = LUMINOUS_SCENE_MIP_TEXTURE_DEST + 1; slot <= LUMINOUS_SPD_MAX_MIPS; ++slot)
                    {
                        encoder.setNativeTexture(cs, slot, nullptr);
                    }
                    const U32 threads = LUMINOUS_SCENE_MIP_THREADS;
                    encoder.dispatchThreads({ k.destSize.x, k.destSize.y, 1 }, { threads, threads, 1 });
                    base += 1;
                }
            }
            for (U32 slot = 0; slot <= LUMINOUS_SPD_MAX_MIPS; ++slot)
            {
                encoder.setNativeTexture(cs, slot, nullptr);
            }
        }

        bool makeSceneMips(LLRenderTarget* scene)
        {
            Device* device = viewerDevice();
            Texture* source = scene ? scene->getRHITexture(0) : nullptr;
            if (!device || !source || !sProgram.ready() || !device->capabilities().computePasses)
            {
                return false;
            }
            ComputePipeline* spd = sProgram.compute(KERNEL_SPD);
            ComputePipeline* down = sProgram.compute(KERNEL_MIP_DOWN);
            if (!ready(spd) || !ready(down) || !spdBuffers(*device))
            {
                return false;
            }
            const TextureDesc& scene_desc = source->desc();
            if (scene_desc.samples > 1)
            {
                return false;
            }
            // A scene map with its own levels, writable by compute (TMG_MANUAL), is its own chain: no copy of level 0
            // and no second texture. Otherwise a chain of its own, level 0 copied in.
            const bool in_place = scene_desc.mipLevels > 1 && (scene_desc.usage & TEXTURE_STORAGE);
            if (in_place)
            {
                sSceneMips = nullptr;
            }
            Texture* chain = in_place ? source : sceneMipTexture(*device, scene_desc);
            if (!chain)
            {
                return false;
            }

            Encoder& encoder = device->encoder();
            LL_PROFILE_GPU_ZONE("luminous scene mips");
            if (!in_place)
            {
                encoder.copyTexture(source, 0, 0, { 0, 0, scene_desc.width, scene_desc.height }, chain, 0, 0, 0, 0);
            }
            if (chain->desc().mipLevels > 1)
            {
                if (!encoder.beginComputePass({ "luminous scene mips" }))
                {
                    return false;
                }
                recordSceneMips(encoder, chain, spd, down);
                encoder.endComputePass();
            }
            return true;
        }
    }

    void sceneMapDrawn(LLRenderTarget* scene, bool wanted)
    {
        if (!metal())
        {
            return;
        }
        sSceneMipsCurrent = wanted && makeSceneMips(scene);
    }

    Texture* sceneMipChain(LLRenderTarget* scene)
    {
        if (!metal() || !sSceneMipsCurrent || !scene)
        {
            return nullptr;
        }
        // the scene map made into its own levels (makeSceneMips)
        if (Texture* own = scene->getRHITexture(0); own && own->desc().mipLevels > 1 && (own->desc().usage & TEXTURE_STORAGE))
        {
            return own;
        }
        if (sSceneMips.isNull() || sSceneMips->textureCount() == 0)
        {
            return nullptr;
        }
        Texture* chain = sSceneMips->texture(0);
        Texture* source = scene->getRHITexture(0);
        if (!chain || !source || chain->desc().width != source->desc().width ||
            chain->desc().height != source->desc().height)
        {
            return nullptr;
        }
        return chain;
    }

    TextureDesc depthPyramidDesc(U32 width, U32 height, Format format)
    {
        TextureDesc desc;
        desc.type = TextureType::Tex2D;
        desc.format = format;
        desc.width = std::max(width / 2, 1U);
        desc.height = std::max(height / 2, 1U);
        desc.mipLevels = pyramidLevels(width, height);
        desc.usage = TEXTURE_SAMPLED | TEXTURE_STORAGE;
        return desc;
    }

    bool buildDepthPyramid(Encoder& encoder, Texture* depth, Texture* pyramid, const glm::mat4& inv_proj, U32 mode,
                           F32 radius)
    {
        ComputePipeline* kernel = sProgram.ready() ? sProgram.compute(KERNEL_PYRAMID) : nullptr;
        if (!ready(kernel) || !depth || !pyramid)
        {
            return false;
        }
        const TextureDesc& depth_desc = depth->desc();
        const TextureDesc& desc = pyramid->desc();
        const U32 levels = std::min<U32>(desc.mipLevels, LUMINOUS_PYRAMID_MAX_LEVELS);

        LuminousDepthPyramidConstants constants{};
        constants.invProj = toSimd(inv_proj);
        constants.depthSize = simd_make_uint2(depth_desc.width, depth_desc.height);
        constants.size0 = simd_make_uint2(desc.width, desc.height);
        constants.levels = levels;
        constants.mode = mode;
        constants.radius = radius;

        encoder.setComputePipeline(kernel);
        setNativeConstants(encoder, NativeStage::Compute, LUMINOUS_PYRAMID_BUFFER_CONSTANTS, constants);
        encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_PYRAMID_TEXTURE_DEPTH, depth);
        for (U32 level = 0; level < LUMINOUS_PYRAMID_MAX_LEVELS; ++level)
        {
            const U32 slot = LUMINOUS_PYRAMID_TEXTURE_FIRST_LEVEL + level;
            if (level < levels)
            {
                encoder.setNativeTexture(NativeStage::Compute, slot, pyramid, Access::Write, { level, 1 });
            }
            else
            {
                encoder.setNativeTexture(NativeStage::Compute, slot, nullptr);
            }
        }
        // A threadgroup makes 16 x 16 texels of level 0 (a 32 x 32 tile of the depth buffer) and their levels.
        const U32 tile = LUMINOUS_PYRAMID_TILE / 2;
        encoder.dispatchThreadgroups({ (desc.width + tile - 1) / tile, (desc.height + tile - 1) / tile, 1 },
                                     { LUMINOUS_PYRAMID_THREADS, LUMINOUS_PYRAMID_THREADS, 1 });
        return true;
    }

    // The passes this group owns: HalfGTAO, said ready once the screen library and its pipelines have compiled
    // (after login, in the background). Until then the effect's live view draws GTAO at full size in the sun pass.
    void registerScreenSpace(PassTable& table)
    {
        table.fn[size_t(Pass::HalfGTAO)] = drawHalfGTAO;
        if (metal())
        {
            startPolling();
        }
    }

    void releaseScreenSpaceTargets()
    {
        sPool = nullptr;
        sPoolWidth = 0;
        sPoolHeight = 0;
        releaseSceneMips();
    }
}
