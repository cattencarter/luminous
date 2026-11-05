/**
 * @file luminousmetal.cpp
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

#include "llviewerprecompiledheaders.h"

#include "luminousmetal.h"

#include "llrhi.h"
#include "llviewercontrol.h"
#include "pipeline.h"

#include <initializer_list>
#include <memory>

namespace LuminousMetal
{
    namespace
    {
        constexpr Effect NO_EFFECT = Effect::Count;

        // One effect: its switch on Metal, whether it lives in Firestorm's own programs (translated
        // to Metal with them), another effect it needs on Metal, and the passes of Luminous's own it
        // draws with.
        struct EffectInfo
        {
            const char* setting;
            bool hosted;
            Effect needs;
            std::initializer_list<Pass> passes;
        };

        // By Effect. Hosted, verified in the shaders:
        // - SoftShadows: shadowUtil.glsl's LUMINOUS_SOFT_SHADOWS, read by the sun pass, alpha, water and GLTF.
        // - ContactShadows: class2 sunLightF/sunLightSSAOF and class3 spotLightF (luminous_contact_*).
        // - GTAO: aoUtil.glsl's LUMINOUS_GTAO in the sun pass at full size; the live half-size pass
        //   (HalfGTAO) is Luminous's own and is drawn only when native, else GTAO is drawn at full size.
        // - AOLights: class3 pointLightF, spotLightF, multiPointLightF (luminous_ao_lights); it reads
        //   GTAO's result, so it needs GTAO.
        // - Reflections: class3 screenSpaceReflUtil.glsl and reflectionProbeF.glsl (LUMINOUS_SSR).
        // - Look: tonemapUtilF.glsl's LUMINOUS_LOOK. Its tables are sampler3D, translated to texture3d and
        //   bound as RHI 3D textures on the channels the GL code uses (luminousphotomode.cpp's bindMetalLut).
        // - Grain, Worn: in the Look's tonemap and in CASF.glsl's sharpening pass, which always carries
        //   them; with the Look off they are drawn in the sharpening pass only.
        // - LightLeak: in the Look's tonemap only, so it needs the Look.
        // - AlphaSort: alphaF, pbralphaF, materialF, fullbrightF with LUMINOUS_ALPHA_SORT_HASH on Metal (a
        //   hash of position, never the triangle's number); on unless LuminousMetalAlphaSort is 0.
        const std::array<EffectInfo, size_t(Effect::Count)> EFFECTS = { {
            { "LuminousMetalSunShafts", false, NO_EFFECT, { Pass::SunShaftMask, Pass::SunShaftSoften, Pass::SunShafts } },
            { "LuminousMetalVolumetricLight", false, NO_EFFECT, { Pass::VolumetricLight } },
            { "LuminousMetalProjectorBeams", false, NO_EFFECT, { Pass::ProjectorBeams } },
            { "LuminousMetalSoftShadows", true, NO_EFFECT, {} },
            { "LuminousMetalContactShadows", true, NO_EFFECT, {} },
            { "LuminousMetalGTAO", true, NO_EFFECT, {} },
            { "LuminousMetalAOLights", true, Effect::GTAO, {} },
            { "LuminousMetalReflections", true, NO_EFFECT, {} },
            { "LuminousMetalDepthOfField", false, NO_EFFECT, { Pass::DoFTiles, Pass::DoFGather } },
            { "LuminousMetalBloom", false, NO_EFFECT, { Pass::BloomChain, Pass::Bloom } },
            { "LuminousMetalClarity", false, NO_EFFECT, { Pass::BloomChain, Pass::Clarity } },
            { "LuminousMetalHalation", false, NO_EFFECT, { Pass::BloomChain, Pass::Halation } },
            { "LuminousMetalAnamorphic", false, NO_EFFECT, { Pass::Anamorphic } },
            { "LuminousMetalLensFlare", false, NO_EFFECT, { Pass::LensFlare } },
            { "LuminousMetalChromaticAberration", false, NO_EFFECT, { Pass::ChromaticAberration } },
            { "LuminousMetalDistortion", false, NO_EFFECT, { Pass::Distortion } },
            { "LuminousMetalGlitch", false, NO_EFFECT, { Pass::Glitch } },
            { "LuminousMetalVignette", false, NO_EFFECT, { Pass::Vignette } },
            { "LuminousMetalLook", true, NO_EFFECT, {} },
            { "LuminousMetalGrain", true, NO_EFFECT, {} },
            { "LuminousMetalWorn", true, NO_EFFECT, {} },
            { "LuminousMetalLightLeak", true, Effect::Look, {} },
            { "LuminousMetalPictureFrame", false, NO_EFFECT, { Pass::PictureFrame } },
            { "LuminousMetalUpscale", false, NO_EFFECT, { Pass::Upscale } },
            { "LuminousMetalAlphaSort", true, NO_EFFECT, {} },
            { "LuminousMetalAccumulation", false, NO_EFFECT, { Pass::AverageFrames } },
            { "LuminousMetalHDRSnapshot", false, NO_EFFECT, { Pass::HDRCapture } },
        } };

        // By Pass: the effect whose switch decides whether the pass is drawn. BloomChain is drawn when any
        // of its consumers (bloom, clarity, halation) is on (passOn).
        const std::array<Effect, size_t(Pass::Count)> PASS_EFFECT = {
            Effect::GTAO,                // HalfGTAO
            Effect::SunShafts,           // SunShaftMask
            Effect::SunShafts,           // SunShaftSoften
            Effect::VolumetricLight,     // VolumetricLight
            Effect::ProjectorBeams,      // ProjectorBeams
            Effect::SunShafts,           // SunShafts
            Effect::Vignette,            // Vignette
            Effect::DepthOfField,        // DoFTiles
            Effect::DepthOfField,        // DoFGather
            Effect::Bloom,               // BloomChain
            Effect::Clarity,             // Clarity
            Effect::Halation,            // Halation
            Effect::Bloom,               // Bloom
            Effect::Anamorphic,          // Anamorphic
            Effect::LensFlare,           // LensFlare
            Effect::ChromaticAberration, // ChromaticAberration
            Effect::Distortion,          // Distortion
            Effect::Glitch,              // Glitch
            Effect::Accumulation,        // AverageFrames
            Effect::HDRSnapshot,         // HDRCapture
            Effect::PictureFrame,        // PictureFrame
            Effect::Upscale,             // Upscale
        };

        PassContext sContext;
        std::array<bool, size_t(Pass::Count)> sReady{};

        const PassTable& table()
        {
            static PassTable passes;
            static bool registered = false;
            if (!registered)
            {
                registered = true;
                registerTone(passes);
                registerLens(passes);
                registerDepthOfField(passes);
                registerShadows(passes);
                registerScreenSpace(passes);
                registerAirLight(passes);
                registerRender(passes);
            }
            return passes;
        }

        U32 setting(Effect effect)
        {
            static std::array<std::unique_ptr<LLCachedControl<U32>>, size_t(Effect::Count)> controls;
            std::unique_ptr<LLCachedControl<U32>>& control = controls[size_t(effect)];
            if (!control)
            {
                control = std::make_unique<LLCachedControl<U32>>(gSavedSettings, EFFECTS[size_t(effect)].setting, 1U);
            }
            return (*control)();
        }

        // Whether a pass's effect is on; the bloom chain is shared, so it is on with any effect that reads it.
        bool passOn(Pass pass)
        {
            if (pass == Pass::BloomChain)
            {
                return on(Effect::Bloom) || on(Effect::Clarity) || on(Effect::Halation);
            }
            return on(effectOf(pass));
        }

        bool ownPassesReady(Effect effect)
        {
            const EffectInfo& info = EFFECTS[size_t(effect)];
            if (info.passes.size() == 0)
            {
                return false;
            }
            for (Pass pass : info.passes)
            {
                if (!passReady(pass))
                {
                    return false;
                }
            }
            return true;
        }
    }

    bool metal()
    {
        return LL::RHI::viewerBackend() == LL::RHI::BackendType::Metal;
    }

    bool photoModeOnMetal()
    {
        if (!metal())
        {
            return true;
        }
        static LLCachedControl<bool> dev(gSavedSettings, "LuminousMetalPhotoModeDev", true);
        return dev;
    }

    bool on(Effect effect)
    {
        if (!metal())
        {
            return true;
        }
        if (effect >= Effect::Count || !photoModeOnMetal() || setting(effect) == 0)
        {
            return false;
        }
        const EffectInfo& info = EFFECTS[size_t(effect)];
        if (info.needs != NO_EFFECT && !on(info.needs))
        {
            return false;
        }
        return info.hosted || ownPassesReady(effect);
    }

    bool native(Effect effect)
    {
        return metal() && effect < Effect::Count && ownPassesReady(effect);
    }

    bool passReady(Pass pass)
    {
        if (!metal())
        {
            return true;
        }
        return pass < Pass::Count && table().fn[size_t(pass)] != nullptr && sReady[size_t(pass)];
    }

    void setReady(Pass pass, bool ready)
    {
        if (pass < Pass::Count)
        {
            sReady[size_t(pass)] = ready;
        }
    }

    Effect effectOf(Pass pass)
    {
        return pass < Pass::Count ? PASS_EFFECT[size_t(pass)] : Effect::Count;
    }

    bool drawNative(Pass pass, PassTargets& targets)
    {
        if (!metal() || pass >= Pass::Count)
        {
            return false;
        }
        const U64 bit = 1ull << size_t(pass);
        if (sContext.drawnThisFrame & bit)
        {
            return true; // a fused native pass drew it already
        }
        if (!passOn(pass) || !passReady(pass))
        {
            return false;
        }
        PassFn fn = table().fn[size_t(pass)];
        if (!fn(sContext, targets))
        {
            return false;
        }
        sContext.drawnThisFrame |= bit;
        return true;
    }

    bool replaces(Pass pass, PassTargets& targets)
    {
        if (!metal())
        {
            return false;
        }
        drawNative(pass, targets);
        return true;
    }

    PassContext& context()
    {
        return sContext;
    }

    void beginFrame()
    {
        if (!metal())
        {
            return;
        }
        const U64 frame = sContext.frame + 1;
        sContext = PassContext();
        sContext.frame = frame;
        if (gPipeline.mRT)
        {
            sContext.depth = &gPipeline.mRT->deferredScreen;
        }
        sContext.exposure = &gPipeline.mExposureMap;
        beginTemporalFrame(frame);
    }

    void releaseRenderSizedTargets()
    {
        if (!metal())
        {
            return;
        }
        sContext.pool = nullptr; // a group's pool may be among those let go
        releaseLensTargets();
        releaseDepthOfFieldTargets();
        releaseScreenSpaceTargets();
        releaseAirLightTargets();
    }
}
