/**
 * @file luminousmetalair.cpp
 * @brief Photo mode on Metal: light in the air, the group's native passes.
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

#include <set>
#include "llviewerprecompiledheaders.h"

#include "luminousmetal.h"
#include "luminousmetalparams.h"
#include "luminousphotomode.h"

#include "llcallbacklist.h"
#include "llimagegl.h"
#include "llnativeprogram.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llstartup.h"
#include "llviewertexture.h"
#include "pipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

// The air light group's native passes on Metal: sun shafts, volumetric light and projector beams.
//
// The three are drawn together, at the first of their hooks a frame reaches (the shafts' mask, else the
// volumetric light, else the beams), and the others' hooks find them drawn. What is drawn at half size this
// frame (low quality live) goes into one half-size sum, and what is drawn at full size is added to the HDR frame
// in the same draw that adds the sum through the tent:
// 1. a compute pass: the shafts' mask and its softening in one kernel (luminous_air_mask), then the half-size
//    items in one kernel (luminous_air_half), every projector of a batch in one dispatch;
// 2. one native render pass on the HDR frame, additive, alpha left alone (luminous_air_fs).
// The reference's order -- the mask taken from the frame before anything is added, then everything added -- is
// kept, since the mask is the first thing drawn and the rest only adds. On Metal no pass break is needed to have
// the depth stored: the passes that wrote it end with their store actions before these passes read it.
namespace LuminousMetal
{
    namespace
    {
        using namespace LL::RHI;

        constexpr const char* MANIFEST = "luminous/air/programs.json";
        constexpr const char* LIBRARY = "air";
        constexpr const char* KERNEL_MASK = "luminous_air_mask";
        constexpr const char* KERNEL_HALF = "luminous_air_half";
        constexpr const char* VERTEX_FULLSCREEN = "luminous_fullscreen_vs";
        constexpr const char* FRAGMENT_AIR = "luminous_air_fs";

        constexpr std::array<Pass, 5> PASSES = { Pass::SunShaftMask, Pass::SunShaftSoften, Pass::SunShafts,
                                                 Pass::VolumetricLight, Pass::ProjectorBeams };

        LLNativeProgram sProgram;
        bool sLoaded = false;   // sProgram.load was called on the current device
        bool sPolling = false;  // the idle poll below is registered
        bool sReleaseHooked = false;

        // The shafts' softened mask and the half-size sum, each made when first wanted at a size, RGBA16F as the
        // reference's RGB16F targets with an alpha Metal needs.
        Ref<TransientPool> sMaskPool;
        Ref<TransientPool> sAirPool;

        void setAllReady(bool ready)
        {
            for (Pass pass : PASSES)
            {
                setReady(pass, ready);
            }
        }

        U64 bit(Pass pass)
        {
            return 1ull << size_t(pass);
        }

        PipelineDesc airDesc(Format frame)
        {
            PipelineDesc desc;
            desc.colorCount = 1;
            desc.colorFormats[0] = frame;
            desc.depthFormat = Format::Unknown;
            // Added to the frame, its alpha left alone: the reference's BF_ONE, BF_ONE with colour mask (true, false).
            desc.blend.enabled = true;
            desc.blend.srcColor = BlendFactor::One;
            desc.blend.dstColor = BlendFactor::One;
            desc.blend.srcAlpha = BlendFactor::Zero;
            desc.blend.dstAlpha = BlendFactor::One;
            desc.colorMask = COLOR_MASK_R | COLOR_MASK_G | COLOR_MASK_B;
            return desc;
        }

        Pipeline* airPipeline(Format frame)
        {
            return sProgram.render(VERTEX_FULLSCREEN, FRAGMENT_AIR, airDesc(frame));
        }

        LLRenderTarget* frameTarget()
        {
            return gPipeline.mRT ? &gPipeline.mRT->screen : nullptr;
        }

        // Says once per reason why the air passes were not drawn, so a log shows where they stop.
        bool skipped(const char* why)
        {
            static std::set<std::string> said;
            if (said.insert(why).second)
            {
                LL_INFOS("Luminous") << "Light in the air on Metal: not drawn this frame: " << why << LL_ENDL;
            }
            return false;
        }

        void startPolling();

        void onDeviceRelease()
        {
            // LLNativeProgram lets go of its library itself; the pools go with the device too.
            sMaskPool = nullptr;
            sAirPool = nullptr;
            sLoaded = false;
            setAllReady(false);
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
                    LL_WARNS("Luminous") << "Light in the air on Metal: the air library could not be loaded; sun "
                                         << "shafts, volumetric light and projector beams stay off" << LL_ENDL;
                    sPolling = false;
                    return true;
                }
            }
            const CompileStatus library = sProgram.status();
            if (library == CompileStatus::Pending)
            {
                return skipped("the air library is still compiling");
            }
            if (library == CompileStatus::Failed)
            {
                LL_WARNS("Luminous") << "Light in the air on Metal: the air library failed; sun shafts, volumetric "
                                     << "light and projector beams stay off. " << sProgram.log() << LL_ENDL;
                sPolling = false;
                return true;
            }

            LLRenderTarget* frame = frameTarget();
            Texture* frame_texture = frame ? frame->getRHITexture(0) : nullptr;
            if (!frame_texture)
            {
                return skipped("the frame has no texture yet"); // the targets come with the first frames
            }
            bool pending = false;
            bool failed = false;
            const auto check = [&](CompileStatus status) {
                pending = pending || status == CompileStatus::Pending;
                failed = failed || status == CompileStatus::Failed;
            };
            for (const char* kernel : { KERNEL_MASK, KERNEL_HALF })
            {
                ComputePipeline* pipeline = sProgram.compute(kernel);
                check(pipeline ? pipeline->status() : CompileStatus::Failed);
            }
            Pipeline* air = airPipeline(frame_texture->desc().format);
            check(air ? air->status() : CompileStatus::Failed);
            if (failed)
            {
                LL_WARNS("Luminous") << "Light in the air on Metal: a pipeline failed; sun shafts, volumetric light "
                                     << "and projector beams stay off" << LL_ENDL;
                sPolling = false;
                return true;
            }
            if (pending)
            {
                return skipped("the air pipelines are still compiling");
            }
            setAllReady(true);
            LL_INFOS("Luminous") << "Light in the air on Metal: native passes ready" << LL_ENDL;
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

        /// pool's one texture at width x height, made again when the size differs.
        Texture* poolTexture(Device& device, Ref<TransientPool>& pool, U32 width, U32 height, const char* name)
        {
            if (pool.notNull())
            {
                Texture* texture = pool->texture(0);
                if (texture && texture->desc().width == width && texture->desc().height == height)
                {
                    return texture;
                }
            }
            pool = nullptr;
            TransientTextureDesc texture;
            texture.desc.type = TextureType::Tex2D;
            texture.desc.format = Format::RGBA16Float;
            texture.desc.width = width;
            texture.desc.height = height;
            texture.desc.usage = TEXTURE_SAMPLED | TEXTURE_STORAGE;
            texture.firstPhase = 0;
            texture.lastPhase = 1;
            texture.name = name;
            TransientPoolDesc desc;
            desc.name = name;
            desc.textures = { texture };
            Ref<TransientPool> made = device.createTransientPool(desc);
            if (made.isNull() || made->textureCount() < 1 || !made->texture(0))
            {
                return nullptr;
            }
            pool = made;
            return pool->texture(0);
        }

        bool ready(ComputePipeline* pipeline)
        {
            return pipeline && pipeline->status() == CompileStatus::Ready;
        }

        Texture* rhiTexture(LLViewerTexture* texture)
        {
            LLImageGL* image = texture ? texture->getGLTexture() : nullptr;
            return image ? image->getRHITexture() : nullptr;
        }

        /// Projectors that one dispatch or draw adds: a run of the beams, and the projected textures they read.
        struct BeamBatch
        {
            U32 first = 0;
            U32 count = 0;
            std::vector<Texture*> textures;
        };

        /// Splits the beams into runs whose projected textures fit one dispatch's slots, the same texture once a
        /// run. Sets each beam's texture index; a beam with no texture at all is left out.
        std::vector<BeamBatch> batchBeams(std::array<LuminousAirBeam, LUMINOUS_AIR_BEAMS_MAX>& beams,
                                          const std::array<Texture*, LUMINOUS_AIR_BEAMS_MAX>& textures, U32& count)
        {
            std::vector<BeamBatch> batches;
            U32 kept = 0;
            for (U32 i = 0; i < count; ++i)
            {
                Texture* texture = textures[i];
                if (!texture)
                {
                    continue;
                }
                if (batches.empty())
                {
                    batches.push_back({ kept, 0, {} });
                }
                BeamBatch* batch = &batches.back();
                auto found = std::find(batch->textures.begin(), batch->textures.end(), texture);
                if (found == batch->textures.end() && batch->textures.size() >= LUMINOUS_AIR_PROJECTOR_TEXTURES)
                {
                    batches.push_back({ kept, 0, {} });
                    batch = &batches.back();
                    found = batch->textures.end();
                }
                if (found == batch->textures.end())
                {
                    batch->textures.push_back(texture);
                    found = batch->textures.end() - 1;
                }
                LuminousAirBeam beam = beams[i];
                beam.texture = (S32)(found - batch->textures.begin());
                beams[kept++] = beam;
                ++batch->count;
            }
            count = kept;
            return batches;
        }

        /// The textures every air function declares, bound for a stage; what the dispatch or draw does not read is
        /// bound to a filler of its kind.
        struct AirTextures
        {
            Texture* depth = nullptr;
            std::array<Texture*, 4> sun{};
            std::array<Texture*, 2> spot{};
            Texture* mask = nullptr;   ///< null: the filler
            Texture* white = nullptr;  ///< the colour filler
        };

        void bindAirTextures(Encoder& encoder, NativeStage stage, const AirTextures& t, const BeamBatch* batch)
        {
            encoder.setNativeTexture(stage, LUMINOUS_AIR_TEXTURE_DEPTH, t.depth);
            for (U32 i = 0; i < 4; ++i)
            {
                encoder.setNativeTexture(stage, LUMINOUS_AIR_TEXTURE_SUN0 + i, t.sun[i] ? t.sun[i] : t.depth);
            }
            for (U32 i = 0; i < 2; ++i)
            {
                encoder.setNativeTexture(stage, LUMINOUS_AIR_TEXTURE_SPOT0 + i, t.spot[i] ? t.spot[i] : t.depth);
            }
            encoder.setNativeTexture(stage, LUMINOUS_AIR_TEXTURE_MASK, t.mask ? t.mask : t.white);
            for (U32 i = 0; i < LUMINOUS_AIR_PROJECTOR_TEXTURES; ++i)
            {
                Texture* texture = batch && i < batch->textures.size() ? batch->textures[i] : t.white;
                encoder.setNativeTexture(stage, LUMINOUS_AIR_TEXTURE_PROJECTOR0 + i, texture);
            }
        }

        /// Everything of the air this frame still to draw, at the first hook that reaches here. entry is that
        /// hook's pass; true when the entry's own effect was drawn.
        bool drawAir(PassContext& context, PassTargets& targets, Pass entry)
        {
            Device* device = viewerDevice();
            LLRenderTarget* frame = targets.frame;
            if (!device || !frame || !gPipeline.mRT)
            {
                return false;
            }
            if (!sProgram.ready())
            {
                // The device was made again, or the library let go: load it again before the next draw.
                setAllReady(false);
                sLoaded = false;
                startPolling();
                return false;
            }
            Texture* frame_texture = frame->getRHITexture(0);
            Texture* depth = gPipeline.mRT->deferredScreen.getRHIDepth();
            Texture* white = LLViewerFetchedTexture::sWhiteImagep.notNull() ? rhiTexture(LLViewerFetchedTexture::sWhiteImagep)
                                                                            : nullptr;
            ComputePipeline* mask_kernel = sProgram.compute(KERNEL_MASK);
            ComputePipeline* half_kernel = sProgram.compute(KERNEL_HALF);
            Pipeline* air = frame_texture ? airPipeline(frame_texture->desc().format) : nullptr;
            if (!frame_texture || !depth || !white)
            {
                return skipped("a frame, depth or white texture is missing");
            }
            if (!ready(mask_kernel) || !ready(half_kernel) || !air || air->status() != CompileStatus::Ready)
            {
                return skipped("a pipeline is not ready");
            }

            const U32 width = frame->getWidth();
            const U32 height = frame->getHeight();
            LuminousAirParams params;
            LuminousPhotoMode::fillAirParams(params, width, height);
            if (params.noPost)
            {
                return skipped("a snapshot without post-processing"); // gets none of photo mode
            }

            AirTextures textures;
            textures.depth = depth;
            textures.white = white;

            // What is still to draw. The shafts only from their own hook, which comes first and only when they are
            // drawn this frame; the others as their own hooks would ask.
            const U64 drawn = context.drawnThisFrame;
            const bool shafts = entry == Pass::SunShaftMask && !(drawn & bit(Pass::SunShafts)) && params.shaftFade > 0.f;
            bool vol = entry != Pass::ProjectorBeams && !(drawn & bit(Pass::VolumetricLight)) &&
                       LuminousPhotoMode::volumetricLightNow() && params.sunShadows != 0;
            for (U32 i = 0; vol && i < 4; ++i)
            {
                LLRenderTarget* map = gPipeline.getSunShadowTarget(i);
                textures.sun[i] = map ? map->getRHIDepth() : nullptr;
                vol = textures.sun[i] != nullptr;
            }
            const bool beams_asked = !(drawn & bit(Pass::ProjectorBeams)) && LuminousPhotoMode::projectorBeamsNow();

            std::array<LuminousAirBeam, LUMINOUS_AIR_BEAMS_MAX> beams{};
            std::array<LLViewerTexture*, LUMINOUS_AIR_BEAMS_MAX> beam_images{};
            std::array<Texture*, LUMINOUS_AIR_BEAMS_MAX> beam_textures{};
            U32 beam_count = 0;
            std::vector<BeamBatch> batches;
            if (beams_asked)
            {
                beam_count = LuminousPhotoMode::fillAirBeams(beams.data(), beam_images.data(), LUMINOUS_AIR_BEAMS_MAX);
                for (U32 i = 0; i < 2; ++i)
                {
                    LLRenderTarget* map = gPipeline.getSpotShadowTarget(i);
                    textures.spot[i] = map ? map->getRHIDepth() : nullptr;
                }
                for (U32 i = 0; i < beam_count; ++i)
                {
                    Texture* texture = rhiTexture(beam_images[i]);
                    beam_textures[i] = texture ? texture : white; // not loaded yet: as the white image
                    if (beams[i].shadowIdx >= 0 && !textures.spot[std::min(beams[i].shadowIdx, 1)])
                    {
                        beams[i].shadowIdx = -1;
                    }
                }
                batches = batchBeams(beams, beam_textures, beam_count);
            }
            const bool beams_drawn = !batches.empty();

            if (!shafts && !vol && !beams_drawn)
            {
                if (beams_asked)
                {
                    context.drawnThisFrame |= bit(Pass::ProjectorBeams); // none in view: nothing to ask again
                }
                if (entry == Pass::SunShaftMask)
                {
                    skipped(params.shaftFade > 0.f ? "sun shafts drawn already" : "the sun is out of the shafts' view");
                }
                if (!LuminousPhotoMode::volumetricLightNow())
                {
                    skipped("volumetric light not wanted now");
                }
                else if (params.sunShadows == 0)
                {
                    skipped("volumetric light needs sun shadows");
                }
                return false;
            }

            const bool shafts_half = shafts && params.shaftsHalf != 0;
            const bool vol_half = vol && params.volHalf != 0;
            const bool beams_half = beams_drawn && params.beamsHalf != 0;
            const bool any_half = shafts_half || vol_half || beams_half;

            // The mask's size, and the half-size sum's: half of the full window, which a smaller live view
            // renders fewer pixels of.
            const F32 live_scale = std::max(params.liveScale, 0.01f);
            const U32 half_width = std::max((U32)((F32)width * 0.5f / live_scale), 1U);
            const U32 half_height = std::max((U32)((F32)height * 0.5f / live_scale), 1U);
            Texture* mask = shafts ? poolTexture(*device, sMaskPool, half_width, half_height, "luminous shaft mask") : nullptr;
            Texture* sum = any_half ? poolTexture(*device, sAirPool, half_width, half_height, "luminous air half") : nullptr;
            if ((shafts && !mask) || (any_half && !sum))
            {
                return false;
            }
            textures.mask = mask;

            LuminousAirDraw base{};
            base.depthSize = simd_make_float2((F32)depth->desc().width, (F32)depth->desc().height);
            base.maskSize = simd_make_float2((F32)half_width, (F32)half_height);
            base.airTexel = simd_make_float2(1.f / (F32)half_width, 1.f / (F32)half_height);
            // How far from the sun the mask can be above zero, in the frame's heights: its radius, the softening's
            // two taps of reach and a texel for each bilinear read on the way.
            const F32 spread = params.shaftSoftness;
            base.shaftReach = params.shaftRadius + (2.f * spread + 4.f) / (F32)half_height;
            base.maskHalo = spread > 0.f ? std::min((S32)std::ceil(2.f * spread) + 1, (S32)LUMINOUS_AIR_MASK_HALO_MAX) : 0;

            const auto beams_bytes = std::as_bytes(std::span<const LuminousAirBeam>(beams.data(), std::max(beam_count, 1U)));
            const BeamBatch none;
            Encoder& encoder = device->encoder();
            LL_PROFILE_GPU_ZONE("luminous air (native)");

            if (shafts || any_half)
            {
                if (!encoder.beginComputePass({ "luminous air" }))
                {
                    return false;
                }
                if (shafts)
                {
                    LuminousAirDraw draw = base;
                    draw.targetSize = base.maskSize;
                    encoder.setComputePipeline(mask_kernel);
                    setNativeConstants(encoder, NativeStage::Compute, LUMINOUS_AIR_BUFFER_PARAMS, params);
                    setNativeConstants(encoder, NativeStage::Compute, LUMINOUS_AIR_BUFFER_DRAW, draw);
                    encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_AIR_TEXTURE_DEPTH, depth);
                    encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_AIR_TEXTURE_FRAME, frame_texture);
                    encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_AIR_TEXTURE_MASK, mask, Access::Write, { 0, 1 });
                    const U32 side = LUMINOUS_AIR_MASK_THREADS;
                    encoder.dispatchThreadgroups({ (half_width + side - 1) / side, (half_height + side - 1) / side, 1 },
                                                 { side, side, 1 });
                }
                if (any_half)
                {
                    encoder.setComputePipeline(half_kernel);
                    setNativeConstants(encoder, NativeStage::Compute, LUMINOUS_AIR_BUFFER_PARAMS, params);
                    encoder.setNativeBytes(NativeStage::Compute, LUMINOUS_AIR_BUFFER_BEAMS, beams_bytes);
                    const size_t runs = beams_half ? batches.size() : 1;
                    for (size_t run = 0; run < runs; ++run)
                    {
                        const BeamBatch& batch = beams_half ? batches[run] : none;
                        LuminousAirDraw draw = base;
                        draw.targetSize = base.maskSize;
                        draw.shafts = run == 0 && shafts_half ? 1 : 0;
                        draw.vol = run == 0 && vol_half ? 1 : 0;
                        draw.beamFirst = (S32)batch.first;
                        draw.beamCount = (S32)batch.count;
                        draw.accumulate = run > 0 ? 1 : 0;
                        setNativeConstants(encoder, NativeStage::Compute, LUMINOUS_AIR_BUFFER_DRAW, draw);
                        bindAirTextures(encoder, NativeStage::Compute, textures, &batch);
                        encoder.setNativeTexture(NativeStage::Compute, LUMINOUS_AIR_TEXTURE_AIR, sum, Access::ReadWrite, { 0, 1 });
                        const U32 side = LUMINOUS_AIR_THREADS;
                        encoder.dispatchThreads({ half_width, half_height, 1 }, { side, side, 1 });
                    }
                }
                encoder.endComputePass();
            }

            // Onto the frame: what is drawn at full size, and the half-size sum through the tent.
            RenderPassDesc pass;
            pass.native = true;
            pass.name = "luminous air";
            pass.colorCount = 1;
            pass.color[0].texture = frame_texture;
            pass.color[0].load = LoadAction::Load;
            pass.color[0].store = StoreAction::Store;
            if (!encoder.beginRenderPass(pass))
            {
                return false;
            }
            encoder.setPipeline(air);
            setNativeConstants(encoder, NativeStage::Fragment, LUMINOUS_AIR_BUFFER_PARAMS, params);
            encoder.setNativeBytes(NativeStage::Fragment, LUMINOUS_AIR_BUFFER_BEAMS, beams_bytes);
            const bool beams_full = beams_drawn && !beams_half;
            const size_t runs = beams_full ? batches.size() : 1;
            for (size_t run = 0; run < runs; ++run)
            {
                const BeamBatch& batch = beams_full ? batches[run] : none;
                LuminousAirDraw draw = base;
                draw.targetSize = simd_make_float2((F32)width, (F32)height);
                draw.shafts = run == 0 && shafts && !shafts_half ? 1 : 0;
                draw.vol = run == 0 && vol && !vol_half ? 1 : 0;
                draw.beamFirst = (S32)batch.first;
                draw.beamCount = (S32)batch.count;
                draw.addAir = run == 0 && any_half ? 1 : 0;
                setNativeConstants(encoder, NativeStage::Fragment, LUMINOUS_AIR_BUFFER_DRAW, draw);
                bindAirTextures(encoder, NativeStage::Fragment, textures, &batch);
                encoder.setNativeTexture(NativeStage::Fragment, LUMINOUS_AIR_TEXTURE_AIR, sum ? sum : white);
                encoder.draw(PrimitiveType::Triangles, 0, 3);
            }
            encoder.endRenderPass();

            if (shafts)
            {
                context.drawnThisFrame |= bit(Pass::SunShaftMask) | bit(Pass::SunShaftSoften) | bit(Pass::SunShafts);
            }
            if (vol)
            {
                context.drawnThisFrame |= bit(Pass::VolumetricLight);
            }
            if (beams_asked)
            {
                context.drawnThisFrame |= bit(Pass::ProjectorBeams);
            }
            switch (entry)
            {
                case Pass::SunShaftMask:    return shafts;
                case Pass::VolumetricLight: return vol;
                case Pass::ProjectorBeams:  return beams_drawn;
                default:                    return false;
            }
        }

        bool drawFromMask(PassContext& context, PassTargets& targets)
        {
            return drawAir(context, targets, Pass::SunShaftMask);
        }

        bool drawFromVolumetricLight(PassContext& context, PassTargets& targets)
        {
            return drawAir(context, targets, Pass::VolumetricLight);
        }

        bool drawFromProjectorBeams(PassContext& context, PassTargets& targets)
        {
            return drawAir(context, targets, Pass::ProjectorBeams);
        }

        // The softening and the gather are drawn with the mask; reached only when the mask was not drawn, and then
        // there is nothing to soften or gather.
        bool drawnWithMask(PassContext& context, PassTargets& targets)
        {
            (void)context;
            (void)targets;
            return false;
        }
    }

    // The passes this group owns, said ready once the air library and its pipelines have compiled (after login,
    // in the background). Until then sun shafts, volumetric light and projector beams are off on Metal.
    void registerAirLight(PassTable& table)
    {
        table.fn[size_t(Pass::SunShaftMask)] = drawFromMask;
        table.fn[size_t(Pass::SunShaftSoften)] = drawnWithMask;
        table.fn[size_t(Pass::SunShafts)] = drawnWithMask;
        table.fn[size_t(Pass::VolumetricLight)] = drawFromVolumetricLight;
        table.fn[size_t(Pass::ProjectorBeams)] = drawFromProjectorBeams;
        if (metal())
        {
            startPolling();
        }
    }

    void releaseAirLightTargets()
    {
        sMaskPool = nullptr;
        sAirPool = nullptr;
    }
}
