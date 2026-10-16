/**
 * @file luminousphotomode.cpp
 * @brief Photo mode: the one switch for everything Luminous adds to rendering.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
 * The idea of volumetric light by shadow-map marching is from the Black Dragon
 * Viewer by NiranV Dean.
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

#include "luminousphotomode.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcharacter.h"
#include "llenvironment.h"
#include "llmotioncontroller.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llrendertarget.h"
#include "llsdserialize.h"
#include "llvirtualtrackball.h"
#include "llsettingsvo.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "lluictrl.h"
#include "llviewercamera.h"
#include "llviewertexture.h"
#include "llvovolume.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "llvoavatarself.h"
#include "llinventorymodel.h"
#include "llinventoryobserver.h"
#include "llnotificationsutil.h"
#include "llsys.h"
#include "llviewerinventory.h"
#include "llcallbacklist.h"
#include "lumenfolders.h"
#include "luminoushdrsnapshot.h"
#include "llinventoryfunctions.h"
#include "llinventorysettings.h"

#include <climits>
#include <set>
#include <cstdlib>
#include <strings.h>

extern bool gSnapshotNoPost;
extern bool gSnapshot;
extern bool gCubeSnapshot;

namespace LuminousPhotoMode
{
    namespace
    {
        // Sun shafts' mask: the sky around the sun, at half resolution.
        // RGB16F, because it is HDR sky and the blur that reads it sums 96
        // samples of it.
        LLRenderTarget sShaftMask;
        // The same, softened, when Softness is above zero.
        LLRenderTarget sShaftMaskSoft;
        // Depth of field reads the circle-of-confusion pass through mip
        // levels, so it copies that pass here. RGBA16F at the frame's size,
        // allocated the first time it is drawn: no memory while depth of field
        // is off.
        LLRenderTarget sDoFSource;
        // Its tiles, DOF_TILE pixels square: the largest blur in each, then the
        // largest that can reach each from the tiles round it. R16F, allocated
        // with sDoFSource.
        const U32 DOF_TILE = 16;
        LLRenderTarget sDoFTiles;
        LLRenderTarget sDoFReach;
        // Tony McMapface's 48^3 table, RGB9E5, loaded the first time it is
        // used; released with photo mode's buffers.
        GLuint sMcMapfaceLut = 0;
        // The Look's .cube table, as loaded, and which one it is.
        GLuint sLookLut = 0;
        F32 sLookLutSize = 0.f;
        std::string sLookLutName;
        // Bloom's chain: half the frame, a quarter, ... RGB16F, allocated the
        // first time bloom is drawn.
        const S32 BLOOM_LEVELS = 6;
        LLRenderTarget sBloom[BLOOM_LEVELS];
        // Anamorphic streaks, a quarter of the frame's width; allocated when drawn.
        LLRenderTarget sStreak;
        LLRenderTarget sStreak2;    // the streak's other half of a ping-pong
        // Lens flare: the light bright enough to flare (half size), a softer
        // copy (quarter size), and the flare itself (half size).
        LLRenderTarget sFlareBright;
        LLRenderTarget sFlareSoft;
        LLRenderTarget sFlare;
        bool sFlareMips = false; // sFlareBright made with mip levels, for Star blur
        // Volumetric light at half size while setting up the shot; a
        // snapshot draws it at full size.
        LLRenderTarget sVolumetric;
        LLRenderTarget sBeams; // projector beams at half size, the same way
        LLRenderTarget sGTAOHalf;  // GTAO at half size live: visibility, view depth
        // A few pixels, only ever cleared: see storeFrameDepth.
        LLRenderTarget sPassBreak;

        // The volumetric light and the projector beams draw into the HDR
        // frame and read the depth buffer, which the frame shares with the
        // deferred targets. Apple's OpenGL keeps a framebuffer's depth in
        // tile memory while the same framebuffer stays bound: what the alpha
        // pass has just written there -- alpha-blended hair, which writes
        // depth -- is not in the texture yet, and the passes would read
        // stale tiles, square blocks of wild light on the hair. Clearing
        // another framebuffer first ends the GPU's render pass and stores
        // the depth. Firestorm's SSR copy and the sun shafts' mask do the
        // same when they run; this is for when neither does.
        void storeFrameDepth()
        {
            if (!sPassBreak.isComplete() && !sPassBreak.allocate(4, 4, GL_RGBA))
            {
                return;
            }
            sPassBreak.bindTarget();
            sPassBreak.clear();
            sPassBreak.flush();
        }

        // Says once in the log whenever a feature starts or stops drawing,
        // and why. Luminous has no endpoint to ask, and an effect that seems
        // to change nothing can have more than one cause.
        void logState(const char* feature, const char*& last, const char* state)
        {
            if (state != last)
            {
                LL_INFOS("LuminousPhotoMode") << feature << ": " << state << LL_ENDL;
                last = state;
            }
        }

        F32 smoothstep(F32 edge0, F32 edge1, F32 x)
        {
            F32 t = llclamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
            return t * t * (3.f - 2.f * t);
        }

        // luminous_frame: an accumulated snapshot's frame, so each draws fresh
        // noise (defined further down, with the frame it reads).
        void bindFrame(LLGLSLShader& shader);

        void drawScreenTriangle()
        {
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        }

        // A copy of an HDR frame as it is. LLPipeline::copyRenderTarget goes
        // through postDeferredNoDoFF.glsl, whose clampHDRRange caps every
        // channel at 11.2: a bright light would lose its gradation and shift
        // hue before tonemapping. Linden's plain copy program does not clamp.
        void copyFrame(LLRenderTarget* src, LLRenderTarget* dst)
        {
            LLGLDisable blend(GL_BLEND);
            dst->bindTarget();
            gCopyProgram.bind();
            gGL.getTexUnit(0)->bind(src);
            drawScreenTriangle();
            gGL.getTexUnit(0)->unbind(src->getUsage());
            gCopyProgram.unbind();
            dst->flush();
        }

        // Shared by both effects: no sun shadows means no shadow map to march
        // through, and a snapshot taken without post-processing gets none of
        // photo mode either.
        const char* skippedBecause(const LLGLSLShader& shader, bool needs_shadows)
        {
            static LLCachedControl<S32> shadow_detail(gSavedSettings, "RenderShadowDetail", 0);
            if (needs_shadows && shadow_detail() < 1) return "off, no sun shadows";
            if (gSnapshotNoPost) return "off, snapshot without post-processing";
            if (!shader.isComplete()) return "off, its shader did not build";
            return nullptr;
        }

        void renderVolumetricLight(LLRenderTarget* hdr_screen)
        {
            static const char* last = nullptr;
            LLGLSLShader& shader = gLuminousVolumetricLightProgram;
            if (const char* off = skippedBecause(shader, true))
            {
                logState("Volumetric light", last, off);
                return;
            }
            logState("Volumetric light", last, "drawing");

            LL_PROFILE_GPU_ZONE("luminous volumetric light");

            static LLCachedControl<U32> samples(gSavedSettings, "LuminousPhotoVolumetricSamples", 64);
            static LLCachedControl<F32> density(gSavedSettings, "LuminousPhotoVolumetricDensity", 0.005f);
            static LLCachedControl<F32> anisotropy(gSavedSettings, "LuminousPhotoVolumetricAnisotropy", 0.6f);
            static LLCachedControl<F32> distance(gSavedSettings, "LuminousPhotoVolumetricDistance", 128.f);

            static LLStaticHashedString s_samples("luminous_vol_samples");
            static LLStaticHashedString s_density("luminous_vol_density");
            static LLStaticHashedString s_anisotropy("luminous_vol_anisotropy");
            static LLStaticHashedString s_distance("luminous_vol_distance");

            // Added on top of the frame: the shader writes only the scattered
            // light, never reads the frame, and leaves alpha alone. It marches
            // the shadow map for every pixel, the costliest of photo mode's
            // passes (29 ms of 70 at 3332x2316), so with low quality live
            // for it, it is drawn at half size outside pictures -- haze is
            // soft -- and added with bloom's tent filter: four times less
            // work.
            // Half of the frame, that is: a live view already rendered at
            // half or less draws it at its own size.
            const F32 live_scale = liveRenderScale();
            const bool half = !fullQuality(LiveQuality::Volumetric) && live_scale > 0.5f && gLuminousBloomUpProgram.isComplete();
            if (half)
            {
                const U32 w = llmax((U32)((F32)hdr_screen->getWidth() * 0.5f / live_scale), 1U);
                const U32 h = llmax((U32)((F32)hdr_screen->getHeight() * 0.5f / live_scale), 1U);
                if (sVolumetric.getWidth() != w || sVolumetric.getHeight() != h)
                {
                    sVolumetric.allocate(w, h, GL_RGB16F);
                }
                sVolumetric.bindTarget();
                sVolumetric.clear();
            }
            else
            {
                hdr_screen->bindTarget();
            }
            LLGLEnable blend(GL_BLEND);
            gGL.setColorMask(true, false);
            gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);

            gPipeline.bindDeferredShader(shader);
            bindFrame(shader);

            LLEnvironment& environment = LLEnvironment::instance();
            shader.uniform1i(s_samples, llclamp((S32)samples(), 4, 512));
            shader.uniform1f(s_density, llclamp(density(), 0.f, 1.f));
            shader.uniform1f(s_anisotropy, llclamp(anisotropy(), 0.f, 0.95f));
            shader.uniform1f(s_distance, llmax(distance(), 1.f));
            shader.uniform1i(LLShaderMgr::SUN_UP_FACTOR, environment.getIsSunUp() ? 1 : 0);
            shader.uniform3fv(LLShaderMgr::LIGHTNORM, 1, environment.getClampedLightNorm().mV);

            drawScreenTriangle();

            gPipeline.unbindDeferredShader(shader);

            if (half)
            {
                sVolumetric.flush();
                static LLStaticHashedString s_texel("luminous_texel");
                static LLStaticHashedString s_weight("luminous_weight");
                LLGLSLShader& up = gLuminousBloomUpProgram;
                hdr_screen->bindTarget();
                up.bind();
                S32 channel = up.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sVolumetric, false, LLTexUnit::TFO_BILINEAR);
                if (channel > -1)
                {
                    gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
                }
                up.uniform2f(s_texel, 0.5f / (F32)sVolumetric.getWidth(), 0.5f / (F32)sVolumetric.getHeight());
                up.uniform1f(s_weight, 1.f);
                drawScreenTriangle();
                up.unbind();
            }

            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            gGL.setColorMask(true, true);
            hdr_screen->flush();
        }

        // Where the sun (or the moon, at night) is on screen, and how much of
        // the shafts to show: none with it behind the camera, fading as it
        // leaves the frame. The projection is still the world camera's here:
        // renderFinalize runs before the HUD pass sets its own.
        bool sunOnScreen(LLVector2& uv, F32& fade)
        {
            const LLVector4& dir = LLEnvironment::instance().getIsSunUp() ? gPipeline.mTransformedSunDir
                                                                           : gPipeline.mTransformedMoonDir;
            glm::vec4 clip = get_current_projection() * glm::vec4(dir.mV[VX], dir.mV[VY], dir.mV[VZ], 0.f);
            if (clip.w <= 1e-4f)
            {
                return false;
            }
            uv.set(clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f);

            // Eye space looks down -z.
            fade = smoothstep(0.f, 0.25f, -dir.mV[VZ]);
            F32 outside = llmax(llmax(-uv.mV[VX], uv.mV[VX] - 1.f), llmax(-uv.mV[VY], uv.mV[VY] - 1.f));
            fade *= 1.f - smoothstep(0.f, 0.6f, outside);
            return fade > 0.f;
        }

        // Sun shafts, first half: the sky around the sun, from the frame as it
        // stands -- taken before anything is added on top, so the shafts come
        // from the sky alone and not from the volumetric light as well.
        // False if the shafts are not drawn this frame, and why is logged.
        bool takeSunShaftMask(LLRenderTarget* hdr_screen, LLVector2& sun_uv, F32& fade)
        {
            static const char* last = nullptr;
            LLGLSLShader& mask = gLuminousSunShaftMaskProgram;
            const char* off = skippedBecause(mask, false);
            if (!off && !gLuminousSunShaftsProgram.isComplete()) off = "off, its shader did not build";
            if (!off && !sShaftMask.isComplete()) off = "off, its buffer is not allocated";
            if (!off && !sunOnScreen(sun_uv, fade)) off = "off, the sun is behind the camera";
            logState("Sun shafts", last, off ? off : "drawing");
            if (off)
            {
                return false;
            }

            LL_PROFILE_GPU_ZONE("luminous sun shaft mask");

            static LLCachedControl<F32> radius(gSavedSettings, "LuminousPhotoSunShaftsRadius", 0.6f);
            static LLStaticHashedString s_sun_uv("luminous_sun_uv");
            static LLStaticHashedString s_aspect("luminous_aspect");
            static LLStaticHashedString s_radius("luminous_shaft_radius");

            sShaftMask.bindTarget();
            mask.bind();
            mask.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, hdr_screen, false, LLTexUnit::TFO_BILINEAR);
            mask.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen, true, LLTexUnit::TFO_POINT);
            mask.uniform2f(s_sun_uv, sun_uv.mV[VX], sun_uv.mV[VY]);
            mask.uniform1f(s_aspect, (F32)hdr_screen->getWidth() / (F32)llmax(hdr_screen->getHeight(), 1U));
            mask.uniform1f(s_radius, llmax(radius(), 0.05f));
            // The frame's pixels, which the live view may render fewer of: the
            // grid stays the same share of the frame.
            static LLStaticHashedString s_src_texel("luminous_src_texel");
            const F32 scale = liveRenderScale();
            mask.uniform2f(s_src_texel, scale / (F32)llmax(hdr_screen->getWidth(), 1U),
                                        scale / (F32)llmax(hdr_screen->getHeight(), 1U));
            drawScreenTriangle();
            mask.unbind();
            sShaftMask.flush();
            return true;
        }

        // Softens the mask, so the rays soften. Returns the mask to read.
        LLRenderTarget* softenSunShaftMask()
        {
            static LLCachedControl<F32> softness(gSavedSettings, "LuminousPhotoSunShaftsSoftness", 0.3f);
            LLGLSLShader& blur = gLuminousSunShaftBlurProgram;
            if (softness() <= 0.f || !blur.isComplete() || !sShaftMaskSoft.isComplete())
            {
                return &sShaftMask;
            }

            LL_PROFILE_GPU_ZONE("luminous sun shaft softness");

            static LLStaticHashedString s_blur_step("luminous_blur_step");
            // Up to four mask texels between taps: a blur up to sixteen of the
            // frame's pixels either side at full Softness.
            // In the full frame's terms, which the live view may render fewer
            // pixels of.
            F32 spread = llclamp(softness(), 0.f, 1.f) * 4.f * liveRenderScale();

            sShaftMaskSoft.bindTarget();
            blur.bind();
            blur.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sShaftMask, false, LLTexUnit::TFO_BILINEAR);
            blur.uniform2f(s_blur_step, spread / (F32)llmax(sShaftMask.getWidth(), 1U),
                                        spread / (F32)llmax(sShaftMask.getHeight(), 1U));
            drawScreenTriangle();
            blur.unbind();
            sShaftMaskSoft.flush();
            return &sShaftMaskSoft;
        }

        // Second half: the mask blurred toward the sun, added to the frame.
        void addSunShafts(LLRenderTarget* hdr_screen, LLRenderTarget* shaft_mask, const LLVector2& sun_uv, F32 fade)
        {
            LL_PROFILE_GPU_ZONE("luminous sun shafts");

            static LLCachedControl<F32> strength(gSavedSettings, "LuminousPhotoSunShaftsStrength", 1.f);
            static LLCachedControl<F32> length(gSavedSettings, "LuminousPhotoSunShaftsLength", 0.9f);
            static LLStaticHashedString s_sun_uv("luminous_sun_uv");
            static LLStaticHashedString s_length("luminous_shaft_length");
            static LLStaticHashedString s_strength("luminous_shaft_strength");
            static LLStaticHashedString s_samples("luminous_shaft_samples");

            LLGLSLShader& shafts = gLuminousSunShaftsProgram;
            // With low quality live for the volumetric light, gathered live at
            // the mask's own size -- half the frame's -- into whichever of
            // the two mask targets is now free, then spread onto the frame
            // with bloom's tent filter, as the volumetric light is: 2-4 ms
            // at 5K for rays a pixel softer along silhouettes. Pictures
            // gather at full size.
            LLRenderTarget* gathered = (shaft_mask == &sShaftMask) ? &sShaftMaskSoft : &sShaftMask;
            const bool half = !fullQuality(LiveQuality::Volumetric) && gathered->isComplete()
                && gLuminousBloomUpProgram.isComplete();
            if (half)
            {
                gathered->bindTarget();
                gathered->clear();
            }
            else
            {
                hdr_screen->bindTarget();
            }
            LLGLEnable blend(GL_BLEND);
            gGL.setColorMask(true, false);
            gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);

            // How far from the sun the mask can be above zero, in the
            // shader's units (the frame's height): its radius, plus the
            // softening blur's two taps of reach and a texel for each
            // bilinear read on the way. The gather skips what lies further.
            static LLCachedControl<F32> radius(gSavedSettings, "LuminousPhotoSunShaftsRadius", 0.6f);
            static LLCachedControl<F32> softness(gSavedSettings, "LuminousPhotoSunShaftsSoftness", 0.3f);
            static LLStaticHashedString s_reach("luminous_shaft_reach");
            static LLStaticHashedString s_aspect("luminous_aspect");
            const F32 spread = (shaft_mask == &sShaftMaskSoft) ? llclamp(softness(), 0.f, 1.f) * 4.f * liveRenderScale() : 0.f;
            const F32 reach = llmax(radius(), 0.05f) + (2.f * spread + 4.f) / (F32)llmax(shaft_mask->getHeight(), 1U);

            shafts.bind();
            bindFrame(shafts);
            shafts.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, shaft_mask, false, LLTexUnit::TFO_BILINEAR);
            shafts.uniform2f(s_sun_uv, sun_uv.mV[VX], sun_uv.mV[VY]);
            shafts.uniform1f(s_length, llclamp(length(), 0.f, 1.f));
            shafts.uniform1f(s_strength, llmax(strength(), 0.f) * fade);
            shafts.uniform1i(s_samples, 96);
            shafts.uniform1f(s_reach, reach);
            shafts.uniform1f(s_aspect, (F32)hdr_screen->getWidth() / (F32)llmax(hdr_screen->getHeight(), 1U));
            drawScreenTriangle();
            shafts.unbind();

            if (half)
            {
                gathered->flush();
                static LLStaticHashedString s_texel("luminous_texel");
                static LLStaticHashedString s_weight("luminous_weight");
                LLGLSLShader& up = gLuminousBloomUpProgram;
                hdr_screen->bindTarget();
                up.bind();
                S32 channel = up.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, gathered, false, LLTexUnit::TFO_BILINEAR);
                if (channel > -1)
                {
                    gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
                }
                up.uniform2f(s_texel, 0.5f / (F32)gathered->getWidth(), 0.5f / (F32)gathered->getHeight());
                up.uniform1f(s_weight, 1.f);
                drawScreenTriangle();
                up.unbind();
            }

            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            gGL.setColorMask(true, true);
            hdr_screen->flush();
        }
    }

    namespace
    {
        // pipeline.cpp's DEFERRED_LIGHT_FALLOFF, which is local to that file:
        // the fudge every deferred light's falloff is taken with.
        constexpr F32 LUMINOUS_DEFERRED_LIGHT_FALLOFF = 0.5f;

        // Where on target a light's range can show: the corners of the box
        // round its sphere (eye space), projected. False when a corner lies
        // behind the eye -- the camera within reach -- and the whole target
        // is wanted.
        bool beamRect(const glm::vec3& center, F32 radius, const LLRenderTarget* target, LLRect& rect)
        {
            const glm::mat4 proj = get_current_projection();
            F32 x0 = 1.f, y0 = 1.f, x1 = -1.f, y1 = -1.f;
            for (S32 i = 0; i < 8; ++i)
            {
                const glm::vec4 corner(center.x + ((i & 1) ? radius : -radius),
                                       center.y + ((i & 2) ? radius : -radius),
                                       center.z + ((i & 4) ? radius : -radius), 1.f);
                const glm::vec4 clip = proj * corner;
                if (clip.w <= 0.01f)
                {
                    return false;
                }
                x0 = llmin(x0, clip.x / clip.w);
                x1 = llmax(x1, clip.x / clip.w);
                y0 = llmin(y0, clip.y / clip.w);
                y1 = llmax(y1, clip.y / clip.w);
            }
            const F32 w = (F32)target->getWidth();
            const F32 h = (F32)target->getHeight();
            const S32 left = (S32)floorf((llclamp(x0, -1.f, 1.f) * 0.5f + 0.5f) * w);
            const S32 right = (S32)ceilf((llclamp(x1, -1.f, 1.f) * 0.5f + 0.5f) * w);
            const S32 bottom = (S32)floorf((llclamp(y0, -1.f, 1.f) * 0.5f + 0.5f) * h);
            const S32 top = (S32)ceilf((llclamp(y1, -1.f, 1.f) * 0.5f + 0.5f) * h);
            rect.set(left, top, right, bottom);
            return true;
        }

        // One projector's beam, added to the frame. The projector's frustum is
        // set up as LLPipeline::setupSpotLight does it -- same matrices, same
        // texture lookups -- but without its side effect of re-ranking which
        // projectors get the two spot shadow maps.
        void addProjectorBeam(LLDrawable* drawablep, const LLRenderTarget* target)
        {
            LLVOVolume* volume = drawablep->getVOVolume();
            LLGLSLShader& shader = gLuminousProjectorBeamsProgram;

            const LLVector3& params = volume->getSpotLightParams();
            F32 fov = params.mV[0];
            F32 focus = params.mV[1];

            LLVector3 pos = drawablep->getPositionAgent();
            LLQuaternion quat = volume->getRenderRotation();
            LLVector3 scale = volume->getScale();

            LLVector3 at_axis(0, 0, -scale.mV[2] * 0.5f);
            at_axis *= quat;
            LLVector3 np = pos + at_axis;
            at_axis.normVec();
            F32 dist = (scale.mV[1] * 0.5f) / tanf(fov * 0.5f);
            LLVector3 origin = np - at_axis * dist;

            LLMatrix4 light_mat(quat, LLVector4(origin, 1.f));
            glm::mat4 light_to_agent(glm::make_mat4((F32*)light_mat.mMatrix));
            glm::mat4 modelview = get_current_modelview();
            glm::mat4 light_to_screen = modelview * light_to_agent;
            glm::mat4 screen_to_light = glm::inverse(light_to_screen);

            F32 size = volume->getLightRadius() * 1.5f;
            F32 near_clip = dist;
            F32 far_clip = size + dist - scale.mV[VZ];
            F32 aspect = scale.mV[VX] / scale.mV[VY];

            glm::mat4 trans(0.5f, 0.0f, 0.0f, 0.0f,
                            0.0f, 0.5f, 0.0f, 0.0f,
                            0.0f, 0.0f, 0.5f, 0.0f,
                            0.5f, 0.5f, 0.5f, 1.0f);
            glm::vec3 p1 = mul_mat4_vec3(light_to_screen, glm::vec3(0, 0, -(near_clip + 0.01f)));
            glm::vec3 p2 = mul_mat4_vec3(light_to_screen, glm::vec3(0, 0, -(near_clip + 1.f)));
            glm::vec3 screen_origin = mul_mat4_vec3(light_to_screen, glm::vec3(0, 0, 0));
            glm::vec3 n = glm::normalize(p2 - p1);
            F32 proj_range = far_clip - near_clip;
            glm::mat4 light_proj = glm::perspective(fov, aspect, near_clip, far_clip);
            screen_to_light = trans * light_proj * screen_to_light;

            glm::vec3 center = mul_mat4_vec3(modelview, glm::vec3(pos.mV[VX], pos.mV[VY], pos.mV[VZ]));
            LLColor3 color = volume->getLightLinearColor();

            S32 shadow_idx = -1;
            F32 shadow_fade = 1.f;
            for (U32 i = 0; i < 2; i++)
            {
                if (gPipeline.mShadowSpotLight[i] == drawablep)
                {
                    shadow_idx = i;
                    shadow_fade = 1.f - gPipeline.mSpotLightFade[i];
                }
            }

            static LLStaticHashedString s_center("luminous_pb_center");
            static LLStaticHashedString s_size("luminous_pb_size");
            static LLStaticHashedString s_color("luminous_pb_color");
            static LLStaticHashedString s_falloff("luminous_pb_falloff");
            static LLStaticHashedString s_proj_mat("luminous_pb_proj_mat");
            static LLStaticHashedString s_proj_n("luminous_pb_proj_n");
            static LLStaticHashedString s_proj_origin("luminous_pb_proj_origin");
            static LLStaticHashedString s_proj_focus("luminous_pb_proj_focus");
            static LLStaticHashedString s_proj_range("luminous_pb_proj_range");
            static LLStaticHashedString s_proj_lod("luminous_pb_proj_lod");
            static LLStaticHashedString s_shadow_idx("luminous_pb_shadow_idx");
            static LLStaticHashedString s_shadow_fade("luminous_pb_shadow_fade");

            shader.uniform3fv(s_center, 1, glm::value_ptr(center));
            shader.uniform1f(s_size, size);
            shader.uniform3fv(s_color, 1, color.mV);
            shader.uniform1f(s_falloff, volume->getLightFalloff(LUMINOUS_DEFERRED_LIGHT_FALLOFF));
            shader.uniformMatrix4fv(s_proj_mat, 1, false, glm::value_ptr(screen_to_light));
            shader.uniform3fv(s_proj_n, 1, glm::value_ptr(n));
            shader.uniform3fv(s_proj_origin, 1, glm::value_ptr(screen_origin));
            shader.uniform1f(s_proj_focus, focus);
            shader.uniform1f(s_proj_range, proj_range);
            shader.uniform1i(s_shadow_idx, shadow_idx);
            shader.uniform1f(s_shadow_fade, shadow_fade);

            LLViewerTexture* img = volume->getLightTexture();
            if (!img)
            {
                img = LLViewerFetchedTexture::sWhiteImagep;
            }
            S32 channel = shader.enableTexture(LLShaderMgr::DEFERRED_PROJECTION);
            if (channel > -1 && img)
            {
                gGL.getTexUnit(channel)->bind(img);
                shader.uniform1f(s_proj_lod, logf((F32)img->getWidth()) / logf(2.f));
            }

            // Only where its range can show: over the whole target, each
            // projector would run the shader everywhere, if only to find that
            // the line of sight misses it.
            LLRect rect;
            if (beamRect(center, size, target, rect))
            {
                if (rect.getWidth() <= 0 || rect.getHeight() <= 0)
                {
                    return; // off screen
                }
                LLGLEnable scissor(GL_SCISSOR_TEST);
                glScissor(rect.mLeft, rect.mBottom, rect.getWidth(), rect.getHeight());
                drawScreenTriangle();
            }
            else
            {
                drawScreenTriangle();
            }
        }

        void renderProjectorBeams(LLRenderTarget* hdr_screen)
        {
            static const char* last = nullptr;
            LLGLSLShader& shader = gLuminousProjectorBeamsProgram;
            const char* off = skippedBecause(shader, false);
            static LLCachedControl<U32> max_beams(gSavedSettings, "LuminousPhotoProjectorBeamsMax", 4);
            std::vector<LLDrawable*> projectors;
            if (!off)
            {
                gPipeline.getNearbySpotLights(projectors, llclamp((U32)max_beams(), 1U, 16U));
                if (projectors.empty()) off = "off, no projector in view";
            }
            logState("Projector beams", last, off ? off : "drawing");
            if (off)
            {
                return;
            }

            LL_PROFILE_GPU_ZONE("luminous projector beams");

            static LLCachedControl<U32> samples(gSavedSettings, "LuminousPhotoVolumetricSamples", 64);
            static LLCachedControl<F32> density(gSavedSettings, "LuminousPhotoProjectorBeamsDensity", 0.4f);
            static LLCachedControl<F32> anisotropy(gSavedSettings, "LuminousPhotoProjectorBeamsAnisotropy", 0.3f);
            static LLStaticHashedString s_samples("luminous_pb_samples");
            static LLStaticHashedString s_density("luminous_pb_density");
            static LLStaticHashedString s_anisotropy("luminous_pb_anisotropy");

            // With low quality live, drawn at half size outside pictures and
            // added with bloom's tent filter, as the volumetric light is: at
            // full size a worn projector's beam can cost 19 ms a frame live
            // on an M4 Mac. The shader places itself by vary_fragcoord, so
            // the smaller target needs nothing else.
            const F32 live_scale = liveRenderScale();
            const bool half = !fullQuality(LiveQuality::Beams) && live_scale > 0.5f && gLuminousBloomUpProgram.isComplete();
            if (half)
            {
                const U32 w = llmax((U32)((F32)hdr_screen->getWidth() * 0.5f / live_scale), 1U);
                const U32 h = llmax((U32)((F32)hdr_screen->getHeight() * 0.5f / live_scale), 1U);
                if (sBeams.getWidth() != w || sBeams.getHeight() != h)
                {
                    sBeams.allocate(w, h, GL_RGB16F);
                }
                sBeams.bindTarget();
                sBeams.clear();
            }
            else
            {
                hdr_screen->bindTarget();
            }
            LLGLEnable blend(GL_BLEND);
            gGL.setColorMask(true, false);
            gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);

            gPipeline.bindDeferredShader(shader);
            bindFrame(shader);
            shader.uniform1i(s_samples, llclamp((S32)samples(), 4, 512));
            shader.uniform1f(s_density, llclamp(density(), 0.f, 4.f));
            shader.uniform1f(s_anisotropy, llclamp(anisotropy(), 0.f, 0.95f));

            for (LLDrawable* drawablep : projectors)
            {
                addProjectorBeam(drawablep, half ? &sBeams : hdr_screen);
            }

            shader.disableTexture(LLShaderMgr::DEFERRED_PROJECTION);
            gPipeline.unbindDeferredShader(shader);

            if (half)
            {
                sBeams.flush();
                static LLStaticHashedString s_texel("luminous_texel");
                static LLStaticHashedString s_weight("luminous_weight");
                LLGLSLShader& up = gLuminousBloomUpProgram;
                hdr_screen->bindTarget();
                up.bind();
                S32 channel = up.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sBeams, false, LLTexUnit::TFO_BILINEAR);
                if (channel > -1)
                {
                    gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
                }
                up.uniform2f(s_texel, 0.5f / (F32)sBeams.getWidth(), 0.5f / (F32)sBeams.getHeight());
                up.uniform1f(s_weight, 1.f);
                drawScreenTriangle();
                up.unbind();
            }

            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            gGL.setColorMask(true, true);
            hdr_screen->flush();
        }
    }

    // Photo mode is always on: the live view stays quick by keeping the
    // costly effects for the rendered picture. Its hooks in Firestorm's
    // files still ask, so that they stay marked as photo mode's.
    bool isOn()
    {
        return true;
    }

    namespace
    {
        S32 sAccumulationFrame = -1;
        // A render's margin: the frame drawn this much larger each way than
        // the picture, which is cut from its middle; 1 without.
        F32 sFrameScale = 1.f;
        // The lens effects that place themselves by the frame's corners --
        // vignette, aberration, distortion -- measure from the picture's.
        void bindFrameScale(LLGLSLShader& shader)
        {
            static LLStaticHashedString s_scale("luminous_frame_scale");
            shader.uniform1f(s_scale, sFrameScale);
        }
        // The lens distortion drawn into this frame, 0 if none: Second
        // Life's depth of field comes after it (bindDistortedDepth).
        F32 sDistortionDrawn = 0.f;
        S32 sAccumulationFrames = 0;
        // renderDoF's focus and the aperture its blur implies, noted each
        // frame, and kept for the frames of a render from its first
        F32 sNotedFocus = 0.f;
        F32 sNotedAperture = 0.f;
        F32 sLensFocus = 0.f;
        F32 sLensAperture = 0.f;

        // A frame that becomes a picture: any of Firestorm's snapshots, and
        // each frame of Luminous's accumulated renders.
        bool pictureFrame()
        {
            return gSnapshot || sAccumulationFrame >= 0;
        }

        // Low quality in the live view: a smaller size while the shot is set
        // up, never in a picture, with one switch for each effect where it
        // saves, all on as Luminous ships. On an M4 Mac, each alone:
        // Firestorm's shadow maps in place of photo mode's save 20 ms a
        // frame (volumetric light marches them too), volumetric light at
        // half size 6 ms; projector beams at half size take 12 ms instead of
        // 25 at 5K in a room with a worn projector, the picture within 5 of
        // 255. GTAO's switch draws it at half size. Fewer samples for GTAO,
        // contact shadows and lens blur would save about 1 ms each, so they
        // keep the picture's.
        bool lowQualityLive(LiveQuality effect)
        {
            static LLCachedControl<bool> volumetric(gSavedSettings, "LuminousPhotoLowQualityLiveVolumetric", true);
            static LLCachedControl<bool> shadows(gSavedSettings, "LuminousPhotoLowQualityLiveShadows", true);
            static LLCachedControl<bool> beams(gSavedSettings, "LuminousPhotoLowQualityLiveBeams", true);
            static LLCachedControl<bool> ao(gSavedSettings, "LuminousPhotoLowQualityLiveAO", true);
            switch (effect)
            {
                case LiveQuality::Volumetric: return volumetric;
                case LiveQuality::Shadows:    return shadows;
                case LiveQuality::Beams:      return beams;
                case LiveQuality::AO:         return ao;
            }
            return false;
        }

        // The light and lens effects can be kept for renders
        // (LuminousPhoto*RenderOnly): left out of the live view, for its frame
        // rate, and drawn in every picture. The costly ones are kept for
        // renders unless the user says otherwise; colour grading is always
        // live. Volumetric light, projector beams and GTAO are live as
        // Luminous ships, at low quality. Enhanced reflections are always
        // kept, off or render only: live they look rougher than in the
        // picture. Alpha sorting too: only a render's frames sort it pixel
        // by pixel.
        enum class Kept { VolumetricLight, ProjectorBeams, SoftShadows, AmbientOcclusion, Reflections, DepthOfField, Enhance, Bloom,
                          SunShafts, Vignette, ChromaticAberration, Clarity, Halation, Anamorphic, Distortion, Grain,
                          LensFlare, ContactShadows, AlphaSort };

        bool keptForRenders(Kept effect)
        {
            static LLCachedControl<bool> volumetric(gSavedSettings, "LuminousPhotoVolumetricLightRenderOnly", false);
            static LLCachedControl<bool> beams(gSavedSettings, "LuminousPhotoProjectorBeamsRenderOnly", false);
            static LLCachedControl<bool> ao(gSavedSettings, "LuminousPhotoAORenderOnly", false);
            static LLCachedControl<bool> dof(gSavedSettings, "LuminousPhotoDepthOfFieldRenderOnly", false);
            static LLCachedControl<bool> enhance(gSavedSettings, "LuminousPhotoDoFRenderOnly", true);
            static LLCachedControl<bool> bloom(gSavedSettings, "LuminousPhotoBloomRenderOnly", true);
            static LLCachedControl<bool> shafts(gSavedSettings, "LuminousPhotoSunShaftsRenderOnly", false);
            static LLCachedControl<bool> vignette(gSavedSettings, "LuminousPhotoVignetteRenderOnly", false);
            static LLCachedControl<bool> aberration(gSavedSettings, "LuminousPhotoChromaticAberrationRenderOnly", false);
            static LLCachedControl<bool> clarity(gSavedSettings, "LuminousPhotoClarityRenderOnly", false);
            static LLCachedControl<bool> halation(gSavedSettings, "LuminousPhotoHalationRenderOnly", false);
            static LLCachedControl<bool> anamorphic(gSavedSettings, "LuminousPhotoAnamorphicRenderOnly", false);
            static LLCachedControl<bool> distortion(gSavedSettings, "LuminousPhotoDistortionRenderOnly", false);
            static LLCachedControl<bool> grain(gSavedSettings, "LuminousPhotoGrainRenderOnly", false);
            static LLCachedControl<bool> flare(gSavedSettings, "LuminousPhotoLensFlareRenderOnly", true);
            static LLCachedControl<bool> contact(gSavedSettings, "LuminousPhotoContactShadowsRenderOnly", true);
            static LLCachedControl<bool> soft(gSavedSettings, "LuminousPhotoSoftShadowsRenderOnly", true);
            switch (effect)
            {
                case Kept::VolumetricLight:     return volumetric;
                case Kept::ProjectorBeams:      return beams;
                case Kept::SoftShadows:         return soft;
                case Kept::AmbientOcclusion:    return ao;
                case Kept::Reflections:         return true;
                case Kept::DepthOfField:        return dof;
                case Kept::Enhance:             return enhance;
                case Kept::Bloom:               return bloom;
                case Kept::SunShafts:           return shafts;
                case Kept::Vignette:            return vignette;
                case Kept::ChromaticAberration: return aberration;
                case Kept::Clarity:             return clarity;
                case Kept::Halation:            return halation;
                case Kept::Anamorphic:          return anamorphic;
                case Kept::Distortion:          return distortion;
                case Kept::Grain:               return grain;
                case Kept::LensFlare:           return flare;
                case Kept::ContactShadows:      return contact;
                case Kept::AlphaSort:           return true; // never live
            }
            return false;
        }

        bool drawnNow(Kept effect)
        {
            return !keptForRenders(effect) || pictureFrame();
        }
    }

    // Alpha sorting: worn see-through things, such as hair, drawn in their
    // place by depth among the other alpha -- pixel by pixel in a render's
    // frames (alphaSortStep), otherwise by distance, one block per avatar
    // (alphaSortLive). LLDrawPoolAlpha asks both.
    F32 alphaSortStep()
    {
        static LLCachedControl<bool> on(gSavedSettings, "LuminousPhotoAlphaSort", true);
        if (!isOn() || !on() || gCubeSnapshot || sAccumulationFrames < 2 || sAccumulationFrame < 0)
        {
            return -1.f;
        }
        return ((F32)sAccumulationFrame + 0.5f) / (F32)sAccumulationFrames;
    }

    bool alphaSortLive()
    {
        static LLCachedControl<bool> on(gSavedSettings, "LuminousPhotoAlphaSort", true);
        return isOn() && on() && !gCubeSnapshot && drawnNow(Kept::AlphaSort) && alphaSortStep() < 0.f;
    }

    bool alphaSortFix()
    {
        // Kept for renders, the live view is Second Life's drawing, as for
        // every render-only effect: no depth pass and no shader changes, so
        // that render only and off look the same there.
        static LLCachedControl<bool> on(gSavedSettings, "LuminousPhotoAlphaSort", true);
        return isOn() && on() && drawnNow(Kept::AlphaSort);
    }

    bool alphaSortFaintAsSecondLife()
    {
        // The faint layers write their depth, as Second Life's rigged alpha
        // does, so the first faint layer hides those behind it: closer to
        // what creators see in Second Life -- a beard as they made it, say.
        static LLCachedControl<bool> faint_sl(gSavedSettings, "LuminousPhotoAlphaSortFaintSL", false);
        return faint_sl;
    }

    bool volumetricLight()
    {
        static LLCachedControl<bool> volumetric(gSavedSettings, "LuminousPhotoVolumetricLight", true);
        return isOn() && volumetric;
    }

    bool volumetricLightNow()
    {
        return volumetricLight() && drawnNow(Kept::VolumetricLight);
    }

    bool projectorBeams()
    {
        static LLCachedControl<bool> beams(gSavedSettings, "LuminousPhotoProjectorBeams", true);
        return isOn() && beams;
    }

    bool projectorBeamsNow()
    {
        return projectorBeams() && drawnNow(Kept::ProjectorBeams);
    }

    bool vignette()
    {
        static LLCachedControl<bool> vignette(gSavedSettings, "LuminousPhotoVignette", false);
        return isOn() && vignette;
    }

    void renderVignette(LLRenderTarget* hdr_screen)
    {
        if (!vignette() || !drawnNow(Kept::Vignette))
        {
            return;
        }
        static const char* last = nullptr;
        LLGLSLShader& shader = gLuminousVignetteProgram;
        const char* off = gSnapshotNoPost ? "off, snapshot without post-processing"
                        : !shader.isComplete() ? "off, its shader did not build"
                        : nullptr;
        logState("Vignette", last, off ? off : "drawing");
        if (off)
        {
            return;
        }

        LL_PROFILE_GPU_ZONE("luminous vignette");

        static LLCachedControl<F32> amount(gSavedSettings, "LuminousPhotoVignetteAmount", 0.35f);
        static LLCachedControl<F32> size(gSavedSettings, "LuminousPhotoVignetteSize", 0.75f);
        static LLCachedControl<F32> softness(gSavedSettings, "LuminousPhotoVignetteSoftness", 0.8f);
        static LLCachedControl<F32> roundness(gSavedSettings, "LuminousPhotoVignetteRoundness", 1.f);
        static LLStaticHashedString s_amount("luminous_vig_amount");
        static LLStaticHashedString s_size("luminous_vig_size");
        static LLStaticHashedString s_softness("luminous_vig_softness");
        static LLStaticHashedString s_roundness("luminous_vig_roundness");
        static LLStaticHashedString s_aspect("luminous_aspect");

        // The frame times the factor the shader writes: it never reads the
        // frame, and alpha is left alone.
        hdr_screen->bindTarget();
        LLGLEnable blend(GL_BLEND);
        gGL.setColorMask(true, false);
        gGL.blendFunc(LLRender::BF_ZERO, LLRender::BF_SOURCE_COLOR);

        shader.bind();
        shader.uniform1f(s_amount, llclamp(amount(), 0.f, 1.f));
        shader.uniform1f(s_size, llclamp(size(), 0.f, 1.5f));
        shader.uniform1f(s_softness, llclamp(softness(), 0.f, 2.f));
        shader.uniform1f(s_roundness, llclamp(roundness(), 0.f, 1.f));
        shader.uniform1f(s_aspect, (F32)hdr_screen->getWidth() / (F32)llmax(hdr_screen->getHeight(), 1U));
        bindFrameScale(shader);
        drawScreenTriangle();
        shader.unbind();

        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        gGL.setColorMask(true, true);
        hdr_screen->flush();
    }

    bool sunShafts()
    {
        static LLCachedControl<bool> shafts(gSavedSettings, "LuminousPhotoSunShafts", true);
        return isOn() && shafts;
    }

    void allocateBuffers(U32 resX, U32 resY)
    {
        if (sunShafts())
        {
            sShaftMask.allocate(llmax(resX / 2, 1U), llmax(resY / 2, 1U), GL_RGB16F);
            sShaftMaskSoft.allocate(llmax(resX / 2, 1U), llmax(resY / 2, 1U), GL_RGB16F);
        }
        else
        {
            sShaftMask.release();
            sShaftMaskSoft.release();
        }
    }

    namespace
    {
        // 1 x 1, read in place of the map of a projector slot no projector
        // holds (LLPipeline::bindShadowMaps)
        LLRenderTarget sNoSpotShadow;
    }

    void allocateNoSpotShadow()
    {
        if (sNoSpotShadow.getDepth() || !sNoSpotShadow.allocate(1, 1, 0, true))
        {
            return;
        }
        // as the projectors' own maps (LLPipeline::allocateShadowBuffer):
        // a shadow sampler must read a texture that compares
        gGL.getTexUnit(0)->bind(&sNoSpotShadow, true);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_ANISOTROPIC);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    LLRenderTarget* noSpotShadow()
    {
        return sNoSpotShadow.getDepth() ? &sNoSpotShadow : nullptr;
    }

    void releaseBuffers()
    {
        sNoSpotShadow.release();
        sShaftMask.release();
        sShaftMaskSoft.release();
        sDoFSource.release();
        sDoFTiles.release();
        sDoFReach.release();
        if (sMcMapfaceLut)
        {
            glDeleteTextures(1, &sMcMapfaceLut);
            sMcMapfaceLut = 0;
        }
        if (sLookLut)
        {
            glDeleteTextures(1, &sLookLut);
            sLookLut = 0;
            sLookLutName.clear();
        }
        for (LLRenderTarget& level : sBloom)
        {
            level.release();
        }
        sStreak.release();
        sStreak2.release();
        sFlareBright.release();
        sFlareSoft.release();
        sFlare.release();
        sVolumetric.release();
        sBeams.release();
        sGTAOHalf.release();
        sPassBreak.release();
    }

    void warnIfLowMemory()
    {
        // Once per install, a word on a Mac with less than 16 GB, the
        // minimum: a busy region and a render at twice the window take 12 GB
        // or more. The viewer still runs.
        if (gSavedSettings.getBOOL("LuminousLowMemoryWarned"))
        {
            return;
        }
        const F64 gb = (F64)gSysMemory.getPhysicalMemoryKB().value() / (1024.0 * 1024.0);
        if (gb <= 0.0 || gb >= 15.0)
        {
            return;
        }
        gSavedSettings.setBOOL("LuminousLowMemoryWarned", true);
        LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", llformat(
            "This Mac has %.0f GB of memory. Luminous needs 16 GB or more: with less, busy regions can be slow "
            "and make macOS swap, and renders often will not fit at twice the window's size -- Luminous then renders at "
            "the window's size instead.", gb)));
    }

    void releaseRenderSizedTargets()
    {
        // Each is made at the size it is next used at, so one drawn live is
        // made again at the live view's size in the next frame, and one kept
        // for renders does not hold a render's size, and its memory, for the
        // rest of the session. The sun shafts' masks are sized with the
        // screen's targets, and the pass break is 4 x 4.
        sDoFSource.release();
        sDoFTiles.release();
        sDoFReach.release();
        for (LLRenderTarget& level : sBloom)
        {
            level.release();
        }
        sStreak.release();
        sStreak2.release();
        sFlareBright.release();
        sFlareSoft.release();
        sFlare.release();
        sVolumetric.release();
        sBeams.release();
        sGTAOHalf.release();
    }

    bool bloom()
    {
        static LLCachedControl<bool> on(gSavedSettings, "LuminousPhotoBloom", false);
        return isOn() && on;
    }

    bool bloomNow()
    {
        return bloom() && drawnNow(Kept::Bloom);
    }

    bool chromaticAberration()
    {
        static LLCachedControl<bool> on(gSavedSettings, "LuminousPhotoChromaticAberration", false);
        return isOn() && on;
    }

    namespace
    {
        void bindClamped(LLGLSLShader& shader, LLRenderTarget* target)
        {
            S32 channel = shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, target, false, LLTexUnit::TFO_BILINEAR);
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }
        }

        // The lens effects that draw the whole frame anew -- clarity,
        // halation, bloom's glow, aberration, distortion -- read it from the
        // target that holds it and draw into the other, which then holds it,
        // instead of copying it first and drawing back (a full-frame copy
        // each, the larger part of a millisecond at 5K). Their shaders carry
        // the frame's alpha over: Firestorm's glow mask. The effects that add
        // light draw onto whichever holds the frame.
        struct LensFrame
        {
            LLRenderTarget* frame;
            LLRenderTarget* other;
        };

        void beginRewrite(LensFrame& lens, LLGLSLShader& shader)
        {
            lens.other->bindTarget();
            shader.bind();
            bindClamped(shader, lens.frame);
        }

        void endRewrite(LensFrame& lens, LLGLSLShader& shader)
        {
            shader.unbind();
            lens.other->flush();
            std::swap(lens.frame, lens.other);
        }

        // The bloom chain: the frame halved six times and back up, each level
        // a share (weight) of the coarser ones, leaving a wide soft blur of it
        // in sBloom[0], at half size. Bloom spreads it onto the frame; clarity
        // compares the frame with it.
        bool blurChain(LLRenderTarget* hdr_screen, F32 weight)
        {
            LLGLSLShader& down = gLuminousBloomDownProgram;
            LLGLSLShader& up = gLuminousBloomUpProgram;
            if (!down.isComplete() || !up.isComplete())
            {
                return false;
            }
            static LLStaticHashedString s_texel("luminous_texel");
            static LLStaticHashedString s_weight("luminous_weight");

            // Sized from the full frame, so that the blur covers the same
            // share of it when the live view renders smaller: then the first
            // level is the frame's own size, or a little smaller.
            const F32 scale = liveRenderScale();
            const U32 width = (U32)((F32)hdr_screen->getWidth() / scale + 0.5f);
            const U32 height = (U32)((F32)hdr_screen->getHeight() / scale + 0.5f);
            if (sBloom[0].getWidth() != llmax(width / 2, 1U) || sBloom[0].getHeight() != llmax(height / 2, 1U))
            {
                for (S32 i = 0; i < BLOOM_LEVELS; ++i)
                {
                    sBloom[i].allocate(llmax(width >> (i + 1), 1U), llmax(height >> (i + 1), 1U), GL_RGB16F);
                }
            }

            {
                LLGLDisable blend(GL_BLEND);
                down.bind();
                LLRenderTarget* src = hdr_screen;
                for (S32 i = 0; i < BLOOM_LEVELS; ++i)
                {
                    sBloom[i].bindTarget();
                    bindClamped(down, src);
                    down.uniform2f(s_texel, 1.f / (F32)src->getWidth(), 1.f / (F32)src->getHeight());
                    drawScreenTriangle();
                    sBloom[i].flush();
                    src = &sBloom[i];
                }
                down.unbind();
            }
            {
                LLGLEnable blend(GL_BLEND);
                gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
                up.bind();
                for (S32 i = BLOOM_LEVELS - 2; i >= 0; --i)
                {
                    sBloom[i].bindTarget();
                    bindClamped(up, &sBloom[i + 1]);
                    up.uniform2f(s_texel, 1.f / (F32)sBloom[i + 1].getWidth(), 1.f / (F32)sBloom[i + 1].getHeight());
                    up.uniform1f(s_weight, llclamp(weight, 0.f, 0.95f));
                    drawScreenTriangle();
                    sBloom[i].flush();
                }
                up.unbind();
                gGL.setSceneBlendType(LLRender::BT_ALPHA);
            }
            return true;
        }

        void bindBlur(LLGLSLShader& shader);

        // Bloom: up to 0.3, light spread from where it is into a wide glow
        // (blended, so none is added); above, a glow round bright things on
        // top -- the light the blur carries past an edge into darker
        // surroundings, added, up to two and a half times at 1.
        void renderBloom(LensFrame& lens)
        {
            LLRenderTarget* hdr_screen = lens.frame;
            static const char* last = nullptr;
            static LLCachedControl<F32> strength(gSavedSettings, "LuminousPhotoBloomStrength", 0.05f);
            static LLCachedControl<F32> size(gSavedSettings, "LuminousPhotoBloomSize", 0.6f);
            LL_PROFILE_GPU_ZONE("luminous bloom");
            const bool built = blurChain(hdr_screen, size());
            logState("Bloom", last, built ? "drawing" : "off, its shaders did not build");
            if (!built)
            {
                return;
            }
            static LLStaticHashedString s_texel("luminous_texel");
            static LLStaticHashedString s_weight("luminous_weight");
            LLGLSLShader& up = gLuminousBloomUpProgram;
            LLGLEnable blend(GL_BLEND);
            gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
            up.bind();
            hdr_screen->bindTarget();
            gGL.setColorMask(true, false);
            bindClamped(up, &sBloom[0]);
            up.uniform2f(s_texel, 1.f / (F32)sBloom[0].getWidth(), 1.f / (F32)sBloom[0].getHeight());
            up.uniform1f(s_weight, llclamp((F32)strength(), 0.f, 0.3f));
            drawScreenTriangle();
            gGL.setColorMask(true, true);
            hdr_screen->flush();
            up.unbind();
            gGL.setSceneBlendType(LLRender::BT_ALPHA);

            const F32 glow = llclamp(((F32)strength() - 0.3f) / 0.7f, 0.f, 1.f);
            LLGLSLShader& spill = gLuminousDiffusionProgram;
            if (glow > 0.001f && spill.isComplete())
            {
                static LLStaticHashedString s_diffusion("luminous_diffusion");
                static LLStaticHashedString s_halation("luminous_halation");
                LLGLDisable no_blend(GL_BLEND);
                beginRewrite(lens, spill);
                bindBlur(spill); // bloom's own wide blur
                spill.uniform1f(s_diffusion, glow * 3.f);
                spill.uniform1f(s_halation, 0.f);
                drawScreenTriangle();
                endRewrite(lens, spill);
            }
        }

        // Clarity: local contrast on the luminance, in stops against the
        // bloom chain's wide blur, before tonemapping. Positive brings out
        // texture -- fabric, wood, skin -- negative softens it.
        void renderClarity(LensFrame& lens)
        {
            static LLCachedControl<F32> clarity(gSavedSettings, "LuminousPhotoClarity", 0.2f);
            static const char* last = nullptr;
            if (std::fabs(clarity()) < 0.005f)
            {
                logState("Clarity", last, "off, at 0");
                return;
            }
            if (!drawnNow(Kept::Clarity))
            {
                return; // kept for renders
            }
            LLGLSLShader& shader = gLuminousClarityProgram;
            if (!shader.isComplete() || !blurChain(lens.frame, 0.5f))
            {
                logState("Clarity", last, "off, its shaders did not build");
                return;
            }
            logState("Clarity", last, "drawing");
            LL_PROFILE_GPU_ZONE("luminous clarity");
            static LLStaticHashedString s_clarity("luminous_clarity");

            LLGLDisable blend(GL_BLEND);
            beginRewrite(lens, shader);
            S32 channel = shader.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sBloom[0], false, LLTexUnit::TFO_BILINEAR);
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }
            shader.uniform1f(s_clarity, llclamp(clarity(), -1.f, 1.f));
            drawScreenTriangle();
            endRewrite(lens, shader);
        }

        void renderChromaticAberration(LensFrame& lens)
        {
            static const char* last = nullptr;
            LLGLSLShader& shader = gLuminousChromaticAberrationProgram;
            logState("Chromatic aberration", last, shader.isComplete() ? "drawing" : "off, its shader did not build");
            if (!shader.isComplete())
            {
                return;
            }
            LL_PROFILE_GPU_ZONE("luminous chromatic aberration");

            static LLCachedControl<F32> amount(gSavedSettings, "LuminousPhotoChromaticAberrationAmount", 2.f);
            static LLStaticHashedString s_amount("luminous_ca_amount");

            LLGLDisable blend(GL_BLEND);
            const F32 width = (F32)lens.frame->getWidth();
            const F32 height = (F32)lens.frame->getHeight();
            beginRewrite(lens, shader);
            shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, width, height);
            // Pixels at the corners of a 1080-line frame, scaled with the
            // frame, so a large snapshot fringes as the screen does.
            shader.uniform1f(s_amount, llclamp(amount(), 0.f, 20.f) * height / sFrameScale / 1080.f);
            bindFrameScale(shader);
            drawScreenTriangle();
            endRewrite(lens, shader);
        }
    }

    namespace
    {
        void bindBlur(LLGLSLShader& shader)
        {
            S32 channel = shader.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sBloom[0], false, LLTexUnit::TFO_BILINEAR);
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }
        }

        // Halation, over a smaller blur than bloom's: the frame drawn anew
        // into the other target. The shader's diffusion term is bloom's
        // glow above 0.3, so it is 0 here.
        void renderHalation(LensFrame& lens)
        {
            static LLCachedControl<F32> halation_setting(gSavedSettings, "LuminousPhotoHalation", 0.f);
            // Kept for renders, it draws as if at 0 in the live view.
            const F32 halation = drawnNow(Kept::Halation) ? (F32)halation_setting() : 0.f;
            if (halation < 0.005f)
            {
                return;
            }
            LLGLSLShader& shader = gLuminousDiffusionProgram;
            // a wider blur toward halation's top, so its glow reaches further
            // past the edge, and further past 1
            const F32 reach = 0.35f + 0.2f * llclamp((halation - 0.5f) * 2.f, 0.f, 1.f)
                            + 0.2f * llclamp(halation - 1.f, 0.f, 1.f);

            if (!shader.isComplete() || !blurChain(lens.frame, reach))
            {
                return;
            }
            LL_PROFILE_GPU_ZONE("luminous halation");
            static LLStaticHashedString s_diffusion("luminous_diffusion");
            static LLStaticHashedString s_halation("luminous_halation");
            LLGLDisable blend(GL_BLEND);
            beginRewrite(lens, shader);
            bindBlur(shader);
            shader.bindTexture(LLShaderMgr::EXPOSURE_MAP, &gPipeline.mExposureMap);
            shader.uniform1f(s_diffusion, 0.f);
            shader.uniform1f(s_halation, llclamp(halation, 0.f, 2.f));
            drawScreenTriangle();
            endRewrite(lens, shader);
        }

        // Anamorphic streaks: the light over the threshold at a quarter
        // width, spread along the rows in passes whose taps grow four times
        // apart each time, then added to the frame with bloom's up-filter.
        // luminousStreakF.glsl says why.
        void renderStreaks(LLRenderTarget* hdr_screen)
        {
            static LLCachedControl<F32> amount(gSavedSettings, "LuminousPhotoAnamorphic", 0.f);
            if (amount() < 0.005f || !drawnNow(Kept::Anamorphic))
            {
                return;
            }
            LLGLSLShader& shader = gLuminousStreakProgram;
            LLGLSLShader& up = gLuminousBloomUpProgram;
            if (!shader.isComplete() || !up.isComplete())
            {
                return;
            }
            LL_PROFILE_GPU_ZONE("luminous streaks");
            static LLStaticHashedString s_pass("luminous_streak_pass");
            static LLStaticHashedString s_spacing("luminous_streak_spacing");
            static LLStaticHashedString s_decay("luminous_streak_decay");
            static LLStaticHashedString s_erode("luminous_streak_erode");
            static LLStaticHashedString s_texel("luminous_texel");
            static LLStaticHashedString s_weight("luminous_weight");
            static LLStaticHashedString s_tint("luminous_streak_tint");
            static LLStaticHashedString s_gain("luminous_streak_gain");
            static LLCachedControl<LLColor4> tint_setting(gSavedSettings, "LuminousPhotoAnamorphicTint", LLColor4(0.55f, 0.75f, 1.f, 1.f));
            // The colour chosen, as bright as the classic blue (0.55, 0.75,
            // 1), so a colour changes the streak's hue, not its strength; at
            // most four times, for a deep red or blue.
            const LLColor4 chosen = tint_setting();
            LLColor3 tint(chosen.mV[VRED], chosen.mV[VGREEN], chosen.mV[VBLUE]);
            const F32 luma = 0.2126f * tint.mV[VRED] + 0.7152f * tint.mV[VGREEN] + 0.0722f * tint.mV[VBLUE];
            tint *= luma > 1e-3f ? llmin(0.7246f / luma, 4.f) : 0.f;
            // Stronger at the top: up to four times at 1, little changed
            // below the middle.
            const F32 strength = llclamp((F32)amount(), 0.f, 1.f);
            const F32 gain = 1.f + 3.f * strength * strength * strength;
            // A quarter of the width, all of the height: the streak is a
            // thin line, and only blurs along it.
            const U32 w = llmax(hdr_screen->getWidth() / 4, 1U);
            const U32 h = hdr_screen->getHeight();
            for (LLRenderTarget* target : { &sStreak, &sStreak2 })
            {
                if (target->getWidth() != w || target->getHeight() != h)
                {
                    target->allocate(w, h, GL_RGB16F);
                }
            }
            // Half the streak is a quarter of the picture's width (w / 4 of
            // these texels, less a render's margin: the frame is wider than the
            // picture cut from it, and the streak would come out a fifth longer
            // than live), the light falling to e^-4 over it.
            const F32 reach = llmax((F32)w * 0.25f / sFrameScale, 1.f);
            LLRenderTarget* src = &sStreak;
            LLRenderTarget* dst = &sStreak2;
            {
                LLGLDisable blend(GL_BLEND);
                shader.bind();
                // the light over the threshold; stars, about two pixels in 1080, left out
                sStreak.bindTarget();
                shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, hdr_screen, false, LLTexUnit::TFO_POINT);
                shader.bindTexture(LLShaderMgr::EXPOSURE_MAP, &gPipeline.mExposureMap);
                shader.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen, true, LLTexUnit::TFO_POINT);
                shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)hdr_screen->getWidth(), (F32)hdr_screen->getHeight());
                shader.uniform1i(s_pass, 0);
                shader.uniform1f(s_erode, llmax(2.5f * (F32)h / 1080.f, 1.f));
                shader.uniform3fv(s_tint, 1, tint.mV);
                shader.uniform1f(s_gain, gain);
                drawScreenTriangle();
                sStreak.flush();
                // spread along the row, the taps 1, 4, 16 ... texels apart
                shader.uniform1i(s_pass, 1);
                shader.uniform1f(s_decay, expf(-4.f / reach));
                F32 covered = 0.f;
                for (F32 spacing = 1.f; covered < reach; spacing *= 4.f)
                {
                    dst->bindTarget();
                    bindClamped(shader, src);
                    shader.uniform1f(s_spacing, spacing);
                    drawScreenTriangle();
                    dst->flush();
                    std::swap(src, dst);
                    covered += 3.f * spacing;
                }
                shader.unbind();
            }
            LLGLEnable blend(GL_BLEND);
            gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE);
            hdr_screen->bindTarget();
            gGL.setColorMask(true, false);
            up.bind();
            bindClamped(up, src);
            up.uniform2f(s_texel, 1.f / (F32)w, 1.f / (F32)h);
            up.uniform1f(s_weight, strength);
            drawScreenTriangle();
            up.unbind();
            gGL.setColorMask(true, true);
            hdr_screen->flush();
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
        }

        // Lens flare, made from the frame's own bright light, not sprites:
        // ghosts mirrored through the centre, a halo, a starburst with as
        // many spikes as the depth of field's blades give, and dirt on the
        // lens. It is added before tonemapping, so it is exposed like any
        // light.
        // luminousFlareF.glsl says how. Half size throughout but the last
        // step: a flare is soft, and a render of 16384 wide needs no full-size
        // copies of it. Off as Luminous ships; subtle at 0.1 to 0.2.
        void renderLensFlare(LLRenderTarget* hdr_screen)
        {
            static LLCachedControl<F32> amount(gSavedSettings, "LuminousPhotoLensFlare", 0.f);
            static const char* last = nullptr;
            if (amount() < 0.005f)
            {
                logState("Lens flare", last, "off, at 0");
                return;
            }
            if (!drawnNow(Kept::LensFlare))
            {
                return; // kept for renders
            }
            LLGLSLShader& bright = gLuminousFlareBrightProgram;
            LLGLSLShader& flare = gLuminousFlareProgram;
            LLGLSLShader& composite = gLuminousFlareCompositeProgram;
            LLGLSLShader& down = gLuminousBloomDownProgram;
            if (!bright.isComplete() || !flare.isComplete() || !composite.isComplete() || !down.isComplete())
            {
                logState("Lens flare", last, "off, its shaders did not build");
                return;
            }
            logState("Lens flare", last, "drawing");
            LL_PROFILE_GPU_ZONE("luminous lens flare");

            static LLCachedControl<F32> ghosts(gSavedSettings, "LuminousPhotoLensFlareGhosts", 1.f);
            static LLCachedControl<F32> halo(gSavedSettings, "LuminousPhotoLensFlareHalo", 0.5f);
            static LLCachedControl<F32> star(gSavedSettings, "LuminousPhotoLensFlareStarburst", 0.5f);
            static LLCachedControl<F32> star_blur_setting(gSavedSettings, "LuminousPhotoLensFlareStarBlur", 0.f);
            static LLCachedControl<F32> dirt(gSavedSettings, "LuminousPhotoLensFlareDirt", 0.5f);
            static LLCachedControl<F32> variation(gSavedSettings, "LuminousPhotoLensFlareVariation", 0.f);
            static LLCachedControl<F32> amplify_setting(gSavedSettings, "LuminousPhotoLensFlareAmplify", 0.f);
            // Amplify: dimmer light flares (the threshold from three times
            // white down to three quarters) and all of it up to three times.
            const F32 amplify = llclamp((F32)amplify_setting(), 0.f, 1.f);
            static LLCachedControl<U32> blades(gSavedSettings, "LuminousPhotoDoFBlades", 0);
            static LLStaticHashedString s_texel("luminous_texel");
            static LLStaticHashedString s_aspect("luminous_aspect");
            static LLStaticHashedString s_ghosts("luminous_flare_ghosts");
            static LLStaticHashedString s_halo("luminous_flare_halo");
            static LLStaticHashedString s_star("luminous_flare_star");
            static LLStaticHashedString s_lines("luminous_flare_lines");
            static LLStaticHashedString s_angle("luminous_flare_angle");
            static LLStaticHashedString s_star_len("luminous_flare_star_len");
            static LLStaticHashedString s_star_taps("luminous_flare_star_taps");
            static LLStaticHashedString s_star_blur("luminous_flare_star_blur");
            static LLStaticHashedString s_flare("luminous_flare");
            static LLStaticHashedString s_sun_uv("luminous_sun_uv");
            static LLStaticHashedString s_sun_weight("luminous_sun_weight");
            static LLStaticHashedString s_variation("luminous_flare_variation");
            static LLStaticHashedString s_blades("luminous_flare_blades");
            static LLStaticHashedString s_threshold("luminous_flare_threshold");
            static LLStaticHashedString s_sun_colour("luminous_sun_colour");
            static LLStaticHashedString s_dirt("luminous_flare_dirt");

            const U32 half_w = llmax(hdr_screen->getWidth() / 2, 1U);
            const U32 half_h = llmax(hdr_screen->getHeight() / 2, 1U);
            const U32 quarter_w = llmax(half_w / 2, 1U);
            const U32 quarter_h = llmax(half_h / 2, 1U);
            // Star blur reads the starburst's light from coarser mip levels
            const F32 star_blur = star() > 0.f ? llclamp((F32)star_blur_setting(), 0.f, 1.f) : 0.f;
            const bool mips = star_blur >= 0.005f;
            if (sFlareBright.getWidth() != half_w || sFlareBright.getHeight() != half_h || sFlareMips != mips)
            {
                // alpha: the brightest, for the starburst
                sFlareBright.allocate(half_w, half_h, GL_RGBA16F, false, LLTexUnit::TT_TEXTURE,
                                      mips ? LLTexUnit::TMG_AUTO : LLTexUnit::TMG_NONE);
                sFlareMips = mips;
                sFlare.allocate(half_w, half_h, GL_RGB16F);
                sFlareSoft.allocate(quarter_w, quarter_h, GL_RGB16F);
            }
            const F32 aspect = (F32)hdr_screen->getWidth() / (F32)llmax(hdr_screen->getHeight(), 1U);
            // A lens's spikes: as many as its blades if even, twice as many if
            // odd; a round aperture (0) is given eight.
            const U32 b = (U32)blades();
            const S32 spikes = b < 3 ? 8 : (b % 2 == 0 ? (S32)b : 2 * (S32)b);

            LLGLDisable blend(GL_BLEND);
            gGL.setColorMask(true, true); // the starburst's light is in alpha
            // the light bright enough to flare
            sFlareBright.bindTarget();
            bright.bind();
            bindClamped(bright, hdr_screen);
            bright.bindTexture(LLShaderMgr::EXPOSURE_MAP, &gPipeline.mExposureMap);
            bright.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen, true, LLTexUnit::TFO_POINT);
            bright.uniform1f(s_threshold, 3.f * (1.f - 0.75f * amplify));
            static LLStaticHashedString s_erode("luminous_flare_erode");
            bright.uniform1f(s_erode, llmax(2.5f * (F32)hdr_screen->getHeight() / 1080.f, 1.f)); // stars: about two pixels in 1080
            drawScreenTriangle();
            bright.unbind();
            sFlareBright.flush();
            // With mip levels, a unit that still holds sFlareBright keeps the
            // mip filtering it was bound with (LLTexUnit::bindManual sets it
            // only for a new texture), and the copy below would read level 1:
            // each bind starts from an empty unit.
            auto bindBright = [](LLGLSLShader& shader, LLTexUnit::eTextureFilterOptions filter)
            {
                S32 channel = shader.getTextureChannel(LLShaderMgr::DEFERRED_DIFFUSE);
                if (channel > -1)
                {
                    gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE);
                }
                channel = shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sFlareBright, false, filter);
                if (channel > -1)
                {
                    gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
                }
            };
            // a softer quarter-size copy
            sFlareSoft.bindTarget();
            down.bind();
            if (mips)
            {
                bindBright(down, LLTexUnit::TFO_BILINEAR);
            }
            else
            {
                bindClamped(down, &sFlareBright);
            }
            down.uniform2f(s_texel, 1.f / (F32)half_w, 1.f / (F32)half_h);
            drawScreenTriangle();
            down.unbind();
            sFlareSoft.flush();
            // ghosts, halo and starburst
            sFlare.bindTarget();
            flare.bind();
            if (mips)
            {
                bindBright(flare, LLTexUnit::TFO_TRILINEAR); // Star blur reads coarser levels
            }
            else
            {
                bindClamped(flare, &sFlareBright);
            }
            S32 channel = flare.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sFlareSoft, false, LLTexUnit::TFO_BILINEAR);
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }
            flare.bindTexture(LLShaderMgr::EXPOSURE_MAP, &gPipeline.mExposureMap);
            flare.uniform1f(s_aspect, aspect);
            bindFrameScale(flare); // laid out in the picture's measure, as live
            flare.uniform1f(s_ghosts, llclamp((F32)ghosts(), 0.f, 2.f));
            flare.uniform1f(s_halo, llclamp((F32)halo(), 0.f, 2.f));
            flare.uniform1f(s_star, llclamp((F32)star(), 0.f, 2.f));
            flare.uniform1i(s_lines, spikes / 2);
            flare.uniform1f(s_angle, 0.3f);
            flare.uniform1f(s_star_len, 0.35f);
            // The small lights' spikes, gathered: most of the flare's cost (6 of
            // 9 ms at 5K), so the live view takes half the samples; the sun's,
            // drawn, are the same in both.
            flare.uniform1i(s_star_taps, pictureFrame() ? 32 : 16);
            flare.uniform1f(s_star_blur, mips ? star_blur : 0.f);
            flare.uniform1f(s_variation, llclamp((F32)variation(), 0.f, 1.f));
            LLVector2 sun_uv;
            F32 sun_fade = 0.f;
            if (!sunOnScreen(sun_uv, sun_fade))
            {
                sun_fade = 0.f;
            }
            flare.uniform2f(s_sun_uv, sun_uv.mV[VX], sun_uv.mV[VY]);
            flare.uniform1f(s_sun_weight, sun_fade);
            flare.uniform1i(s_blades, b < 3 ? 0 : (S32)b);
            // The sky's sunlight colour (moonlight at night), brightest
            // channel 1: the drawn sun's flare is in it.
            LLColor3 sun_colour(1.f, 0.95f, 0.85f);
            LLEnvironment& environment = LLEnvironment::instance();
            if (environment.getCurrentEnvironmentInstance())
            {
                if (LLSettingsSky::ptr_t sky = environment.getCurrentSky())
                {
                    LLColor3 c = environment.getIsSunUp() ? sky->getSunlightColor() : sky->getMoonlightColor();
                    const F32 top = llmax(c.mV[0], llmax(c.mV[1], c.mV[2]));
                    if (top > 1e-4f)
                    {
                        sun_colour = c * (1.f / top);
                    }
                }
            }
            flare.uniform3fv(s_sun_colour, 1, sun_colour.mV);
            // whether the sun's place is sky
            flare.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen, true, LLTexUnit::TFO_POINT);
            drawScreenTriangle();
            flare.unbind();
            sFlare.flush();

            // added to the frame, with the dirt on the lens
            LLGLEnable add(GL_BLEND);
            gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
            hdr_screen->bindTarget();
            gGL.setColorMask(true, false);
            composite.bind();
            bindClamped(composite, &sFlare);
            channel = composite.bindTexture(LLShaderMgr::DEFERRED_BLOOM, &sFlareSoft, false, LLTexUnit::TFO_BILINEAR);
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }
            composite.uniform1f(s_aspect, aspect);
            bindFrameScale(composite);
            composite.uniform1f(s_flare, llclamp((F32)amount(), 0.f, 1.f) * (1.f + 2.f * amplify));
            composite.uniform1f(s_dirt, llclamp((F32)dirt(), 0.f, 2.f));
            drawScreenTriangle();
            composite.unbind();
            gGL.setColorMask(true, true);
            hdr_screen->flush();
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
        }

        // Lens distortion, last: it moves the picture, not its light.
        void renderDistortion(LensFrame& lens)
        {
            static LLCachedControl<F32> distortion(gSavedSettings, "LuminousPhotoDistortion", 0.f);
            if (std::fabs(distortion()) < 0.005f || !drawnNow(Kept::Distortion))
            {
                return;
            }
            LLGLSLShader& shader = gLuminousDistortionProgram;
            if (!shader.isComplete())
            {
                return;
            }
            LL_PROFILE_GPU_ZONE("luminous distortion");
            static LLStaticHashedString s_distortion("luminous_distortion");
            LLGLDisable blend(GL_BLEND);
            const F32 width = (F32)lens.frame->getWidth();
            const F32 height = (F32)lens.frame->getHeight();
            beginRewrite(lens, shader);
            shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, width, height);
            shader.uniform1f(s_distortion, llclamp(distortion(), -1.f, 1.f));
            bindFrameScale(shader);
            drawScreenTriangle();
            endRewrite(lens, shader);
            sDistortionDrawn = llclamp(distortion(), -1.f, 1.f);
        }
    }

    LLRenderTarget* renderLens(LLRenderTarget* hdr_screen, LLRenderTarget* scratch)
    {
        sDistortionDrawn = 0.f;
        if (gSnapshotNoPost)
        {
            return hdr_screen; // as the other effects: a snapshot without post-processing has none
        }
        LensFrame lens{ hdr_screen, scratch };
        renderClarity(lens);
        renderHalation(lens);
        if (bloomNow())
        {
            renderBloom(lens);
        }
        renderStreaks(lens.frame);
        renderLensFlare(lens.frame);
        if (chromaticAberration() && drawnNow(Kept::ChromaticAberration))
        {
            renderChromaticAberration(lens);
        }
        renderDistortion(lens);
        return lens.frame;
    }

    bool softShadows()
    {
        static LLCachedControl<bool> soft(gSavedSettings, "LuminousPhotoSoftShadows", true);
        return isOn() && soft;
    }

    bool softShadowsNow()
    {
        return softShadows() && drawnNow(Kept::SoftShadows);
    }

    bool contactShadows()
    {
        static LLCachedControl<bool> on(gSavedSettings, "LuminousPhotoContactShadows", true);
        return isOn() && on && LLPipeline::RenderShadowDetail > 0;
    }

    bool contactShadowsNow()
    {
        return contactShadows() && drawnNow(Kept::ContactShadows) && !gCubeSnapshot;
    }

    void bindContactShadowUniforms(LLGLSLShader& shader)
    {
        static LLStaticHashedString s_length("luminous_contact_length");
        static LLStaticHashedString s_seed("luminous_contact_seed");
        static LLStaticHashedString s_steps("luminous_contact_steps");
        static LLCachedControl<F32> length(gSavedSettings, "LuminousPhotoContactShadowsLength", 0.3f);
        static LLCachedControl<F32> strength(gSavedSettings, "LuminousPhotoContactShadowsStrength", 1.f);
        static LLStaticHashedString s_strength("luminous_contact_strength");
        if (!contactShadowsNow())
        {
            shader.uniform1f(s_length, 0.f);
            return;
        }
        shader.uniform1f(s_length, llclamp((F32)length(), 0.01f, 2.f));
        shader.uniform1f(s_strength, llclamp((F32)strength(), 0.f, 1.f));
        // Toward the light that casts the shadows, in eye space -- passed here
        // because Firestorm's sun-shadow pass is never told which is up.
        static LLStaticHashedString s_light("luminous_contact_light");
        const LLVector4& light = LLEnvironment::instance().getIsSunUp() ? gPipeline.mTransformedSunDir : gPipeline.mTransformedMoonDir;
        shader.uniform3f(s_light, light.mV[VX], light.mV[VY], light.mV[VZ]);
        shader.uniform1f(s_seed, (F32)llmax(sAccumulationFrame, 0));
        shader.uniform1i(s_steps, 24);
        static LLStaticHashedString s_all("luminous_contact_all");
        shader.uniform1i(s_all, pictureFrame() ? 1 : 0);
    }

    namespace
    {
        F32 halton(S32 index, S32 base)
        {
            F32 f = 1.f;
            F32 r = 0.f;
            for (S32 i = index + 1; i > 0; i /= base)
            {
                f /= (F32)base;
                r += f * (F32)(i % base);
            }
            return r;
        }

        void bindFrame(LLGLSLShader& shader)
        {
            static LLStaticHashedString s_frame("luminous_frame");
            shader.uniform1f(s_frame, (F32)llmax(sAccumulationFrame, 0));
        }

        bool sSnapshotShadows = false;
        U32 sSnapshotShadowSize = 0; // a render's own choice, smaller where memory is short; 0 the setting
        bool sGrainAfterSharpening = false; // this frame's sharpening pass follows its tonemap pass
    }

    const char* lowQualityLiveSetting(LiveQuality effect)
    {
        switch (effect)
        {
            case LiveQuality::Volumetric: return "LuminousPhotoLowQualityLiveVolumetric";
            case LiveQuality::Shadows:    return "LuminousPhotoLowQualityLiveShadows";
            case LiveQuality::Beams:      return "LuminousPhotoLowQualityLiveBeams";
            case LiveQuality::AO:         return "LuminousPhotoLowQualityLiveAO";
        }
        return "";
    }

    bool fullQuality(LiveQuality effect)
    {
        return pictureFrame() || !lowQualityLive(effect);
    }

    void setSnapshotShadows(bool on, U32 size)
    {
        sSnapshotShadows = on;
        sSnapshotShadowSize = on ? size : 0;
    }

    U32 spotShadowMapLimit()
    {
        // The two projector maps are square at the frame's width (1.2 GB at
        // twice a 5K window): no larger than the sun's.
        static LLCachedControl<U32> size(gSavedSettings, "LuminousPhotoShadowMapSize", 8192);
        const U32 limit = llmax((U32)size(), 2048U);
        return sSnapshotShadows && sSnapshotShadowSize ? llmin(limit, llmax(sSnapshotShadowSize, 1024U)) : limit;
    }

    bool averagingFrames()
    {
        return isOn() && sAccumulationFrames > 1;
    }

    void setGrainAfterSharpening(bool on)
    {
        sGrainAfterSharpening = on;
    }

    void bindDither(LLGLSLShader& shader, bool eight_bit)
    {
        static LLStaticHashedString s_dither("luminous_dither");
        shader.uniform1f(s_dither, eight_bit ? 1.f / 255.f : 0.f);
    }

    void bindGrainUniforms(LLGLSLShader& shader)
    {
        static LLCachedControl<F32> grain(gSavedSettings, "LuminousPhotoGrain", 0.f);
        static LLCachedControl<F32> grain_size(gSavedSettings, "LuminousPhotoGrainSize", 1.5f);
        static LLStaticHashedString s_grain("luminous_look_grain");
        static LLStaticHashedString s_grain_size("luminous_look_grain_size");
        const bool on = sGrainAfterSharpening && isOn() && !gSnapshotNoPost && drawnNow(Kept::Grain);
        shader.uniform1f(s_grain, on ? llclamp((F32)grain(), 0.f, 1.f) : 0.f);
        shader.uniform1f(s_grain_size, llclamp((F32)grain_size(), 1.f, 4.f));
    }

    namespace
    {
    }

    U32 shadowMapSize()
    {
        // The chosen size, except while the shot is set up with low quality
        // shadow maps in live preview: then Firestorm's, which follows the window.
        static LLCachedControl<U32> size(gSavedSettings, "LuminousPhotoShadowMapSize", 8192);
        U32 chosen = llmin((U32)size(), 16384U);
        if (sSnapshotShadows && sSnapshotShadowSize)
        {
            chosen = llmin(chosen, llmax(sSnapshotShadowSize, 1024U)); // a render short of memory
        }
        return isOn() && (sSnapshotShadows || !lowQualityLive(LiveQuality::Shadows)) ? chosen : 0;
    }

    namespace
    {
        S32 sFullResolutionHolds = 0;
        bool sWarmUpPending = false;
        bool sWarmingUp = false;
    }

    F32 chosenLiveScale()
    {
        static LLCachedControl<F32> setting(gSavedSettings, "LuminousPhotoLiveScale", 1.f);
        // FXAA sets the window's viewport on its target, so it needs the
        // target the window's size; SMAA, photo mode's own, does not.
        static LLCachedControl<U32> aa(gSavedSettings, "RenderFSAAType", 2);
        if (aa() == 1)
        {
            return 1.f;
        }
        F32 scale = setting();
        if (scale <= 0.f)
        {
            scale = gViewerWindow && gViewerWindow->getWindow()
                ? 1.f / llmax(gViewerWindow->getWindow()->getSystemUISize(), 1.f) : 1.f;
        }
        return llclamp(scale, 0.25f, 1.f);
    }

    F32 liveRenderScale()
    {
        if (!isOn() || sFullResolutionHolds > 0 || sAccumulationFrame >= 0)
        {
            return 1.f;
        }
        return chosenLiveScale();
    }

    bool upscaling()
    {
        return isOn() && gLuminousPresentProgram.isComplete() && gPipeline.mRT == &gPipeline.mMainRT
            && (gPipeline.mRT->screen.getWidth() < (U32)gViewerWindow->getWorldViewWidthRaw()
                || gPipeline.mRT->screen.getHeight() < (U32)gViewerWindow->getWorldViewHeightRaw());
    }

    FullResolution::FullResolution(bool wanted)
        : mHeld(wanted && isOn()) // off, Firestorm's snapshots exactly
    {
        if (!mHeld || sFullResolutionHolds++ > 0 || chosenLiveScale() >= 1.f || !gPipeline.shadersLoaded())
        {
            return;
        }
        // The live view is scaled: the targets back to the world view's full
        // size, as display() would on a resize.
        const U32 before_x = gPipeline.mMainRT.screen.getWidth();
        const U32 before_y = gPipeline.mMainRT.screen.getHeight();
        const bool forced = gResizeScreenTexture;
        gPipeline.resizeScreenTexture();
        gResizeScreenTexture = false;
        const U32 after_x = gPipeline.mMainRT.screen.getWidth();
        const U32 after_y = gPipeline.mMainRT.screen.getHeight();
        if (forced || after_x != before_x || after_y != before_y)
        {
            LL_INFOS("LuminousPhotoMode") << "Full resolution for a snapshot: " << before_x << "x" << before_y << " -> "
                << after_x << "x" << after_y << LL_ENDL;
            sWarmUpPending = true;
        }
    }

    FullResolution::~FullResolution()
    {
        if (mHeld && --sFullResolutionHolds == 0)
        {
            sWarmUpPending = false;
            // The next frame sizes the targets by the live scale again.
            if (liveRenderScale() < 1.f)
            {
                gResizeScreenTexture = true;
            }
        }
    }

    namespace
    {
        S32 sKeepTargetsHolds = 0;
        bool sKeptLarger = false;
    }

    KeepSnapshotTargets::KeepSnapshotTargets()
    {
        ++sKeepTargetsHolds;
    }

    KeepSnapshotTargets::~KeepSnapshotTargets()
    {
        if (--sKeepTargetsHolds == 0 && sKeptLarger)
        {
            sKeptLarger = false;
            gResizeScreenTexture = true; // the next frame sizes them for the window
        }
    }

    namespace
    {
        S32 sShaderHolds = 0;
        bool sShaderRebuildAsked = false;
    }

    HoldShaderRebuilds::HoldShaderRebuilds()
    {
        ++sShaderHolds;
    }

    HoldShaderRebuilds::~HoldShaderRebuilds()
    {
        if (--sShaderHolds == 0 && sShaderRebuildAsked)
        {
            sShaderRebuildAsked = false;
            handleSetShaderChanged(LLSD());
        }
    }

    bool holdShaderRebuild()
    {
        if (sShaderHolds > 0)
        {
            sShaderRebuildAsked = true;
            return true;
        }
        return false;
    }

    bool keepSnapshotTargets()
    {
        if (sKeepTargetsHolds > 0)
        {
            sKeptLarger = true;
            return true;
        }
        return false;
    }

    bool keepingSnapshotTargets()
    {
        return sKeepTargetsHolds > 0;
    }

    bool takeWarmUp(bool reallocated)
    {
        const bool pending = sWarmUpPending;
        sWarmUpPending = false;
        // Reallocated targets for this snapshot -- by the hold, by a world view
        // changed for it, or at its own size -- mean an empty scene map; the
        // first frame of an accumulated one is enough (the others reuse them,
        // except in an off-screen snapshot, where each frame reallocates).
        return pending || (isOn() && reallocated && sAccumulationFrame <= 0);
    }

    void setWarmingUp(bool on)
    {
        sWarmingUp = on;
    }

    bool warmingUp()
    {
        return sWarmingUp;
    }

    void setAccumulationFrame(S32 frame)
    {
        if (frame == 0)
        {
            // Only part of the aperture is sampled across the frames, each
            // frame's own blur giving the rest: with the whole aperture
            // sampled, a small bright light out of focus -- a star at night --
            // comes out as a cluster of the frames' copies instead of one
            // disc. At 0.3 of the aperture it is one even disc as live, its
            // edge a little softer, the blur as wide; what the samples see
            // round an edge goes down to a third.
            static LLCachedControl<F32> share(gSavedSettings, "LuminousSnapshotSampledLensShare", 0.3f);
            sLensFocus = sNotedFocus;
            sLensAperture = sNotedAperture * llclamp((F32)share(), 0.f, 1.f);
        }
        sAccumulationFrame = frame;
    }

    bool holdExposure()
    {
        return isOn() && sAccumulationFrame > 0;
    }

    void setFrameMargin(F32 scale)
    {
        sFrameScale = llmax(scale, 1.f);
    }

    F32 frameMarginScale()
    {
        return sFrameScale;
    }

    void setAccumulationFrames(S32 frames)
    {
        sAccumulationFrames = frames;
    }

    namespace
    {
        // A sampled lens and sun need several frames; and they share the
        // way the frames are spread over a disc: a Vogel spiral, so that any
        // number of frames covers it evenly.
        bool sampling()
        {
            return isOn() && sAccumulationFrame >= 0 && sAccumulationFrames > 1;
        }

        void vogel(S32 index, S32 count, F32 turn, F32& x, F32& y)
        {
            const F32 GOLDEN_ANGLE = 2.39996323f;
            const F32 r = sqrtf(((F32)index + 0.5f) / (F32)llmax(count, 1));
            const F32 a = (F32)index * GOLDEN_ANGLE + turn;
            x = r * cosf(a);
            y = r * sinf(a);
        }

        // Only with Lens blur, which blurs light before the picture is
        // toned, as moving the eye across the aperture does. Second Life's
        // blur works on the finished picture, where a star -- ten times
        // white at night -- is clipped to white and all but vanishes in a
        // wide blur, while a sampled lens keeps all its light and would
        // turn each star into a cluster of the frames' copies.
        bool lensSampled()
        {
            static LLCachedControl<bool> sampled(gSavedSettings, "LuminousSnapshotSampledDoF", true);
            return sampled && sampling() && !LuminousHDRSnapshot::previewing() && LLPipeline::sRenderDeferred && gPipeline.RenderDepthOfField
                && depthOfField() && gLuminousDepthOfFieldProgram.isComplete()
                && sLensFocus > 0.f && sLensAperture > 0.f;
        }

        bool shadowsSampled()
        {
            static LLCachedControl<bool> sampled(gSavedSettings, "LuminousSnapshotSampledShadows", true);
            return sampled && sampling() && !LuminousHDRSnapshot::previewing() && softShadowsNow();
        }

        // What of each frame's own blur or softening to keep: about the
        // spacing of the samples on their disc, so that their copies run
        // together (at half that, 16 frames leave a faint stripe in a large
        // blur).
        F32 fillScale()
        {
            static LLCachedControl<F32> fill(gSavedSettings, "LuminousSnapshotSampledFill", 1.4f);
            return llclamp((F32)fill(), 0.f, 4.f) / sqrtf((F32)llmax(sAccumulationFrames, 1));
        }
    }

    void bindDistortedDepth(LLGLSLShader& shader, bool after_lens)
    {
        static LLStaticHashedString s_distortion("luminous_depth_distortion");
        shader.uniform1f(s_distortion, after_lens ? sDistortionDrawn : 0.f);
        bindFrameScale(shader);
    }

    void noteDepthOfField(F32 focus, F32 aperture)
    {
        sNotedFocus = focus;
        sNotedAperture = aperture;
    }

    bool snapshotLens(F32& x, F32& y, F32& focus)
    {
        if (!lensSampled() || gCubeSnapshot || LLPipeline::sShadowRender || LLPipeline::sReflectionRender)
        {
            return false;
        }
        F32 u, v;
        vogel(sAccumulationFrame, sAccumulationFrames, 0.f, u, v);
        // The aperture's shape, as the lens blur draws it: a polygon of as
        // many sides as it has blades (vertices on the circle), else round.
        static LLCachedControl<U32> blades(gSavedSettings, "LuminousPhotoDoFBlades", 0);
        if (blades() >= 3)
        {
            const F32 sector = F_TWO_PI / (F32)blades();
            F32 a = atan2f(v, u);
            a -= floorf(a / sector) * sector;
            const F32 edge = cosf(sector * 0.5f) / cosf(a - sector * 0.5f);
            u *= edge;
            v *= edge;
        }
        x = u * sLensAperture;
        y = v * sLensAperture;
        focus = sLensFocus;
        return true;
    }

    F32 lensShare(F32 aperture)
    {
        return lensSampled() && aperture > 0.f ? llclamp(sLensAperture / aperture, 0.f, 1.f) : 0.f;
    }

    F32 lensFill()
    {
        return fillScale();
    }

    void jitterShadowCaster(LLVector3& caster_dir)
    {
        if (!shadowsSampled())
        {
            return;
        }
        // Across the sun's disc as the soft shadows widen it: its apparent
        // radius, 0.2665 degrees, times Softness.
        static LLCachedControl<F32> softness(gSavedSettings, "LuminousPhotoShadowSoftness", 2.5f);
        const F32 radius = tanf(0.2665f * DEG_TO_RAD) * llclamp((F32)softness(), 0.f, 20.f);
        F32 u, v;
        vogel(sAccumulationFrame, sAccumulationFrames, 1.3f, u, v);
        LLVector3 dir = caster_dir;
        dir.normVec();
        LLVector3 side = fabsf(dir.mV[VZ]) < 0.9f ? LLVector3(0.f, 0.f, 1.f) % dir : LLVector3(1.f, 0.f, 0.f) % dir;
        side.normVec();
        LLVector3 up = dir % side;
        up.normVec();
        caster_dir = dir + (side * u + up * v) * radius;
        caster_dir.normVec();
    }

    bool meterAfresh()
    {
        if (!isOn() || !pictureFrame())
        {
            return false;
        }
        // Only what is drawn before the exposure is measured changes it:
        // depth of field and bloom come after.
        return (volumetricLight() && keptForRenders(Kept::VolumetricLight))
            || (projectorBeams() && keptForRenders(Kept::ProjectorBeams))
            || (sunShafts() && keptForRenders(Kept::SunShafts))
            || (softShadows() && keptForRenders(Kept::SoftShadows))
            || (ambientOcclusion() && keptForRenders(Kept::AmbientOcclusion))
            || (contactShadows() && keptForRenders(Kept::ContactShadows))
            || (reflections() && keptForRenders(Kept::Reflections));
    }

    bool snapshotJitter(F32& x, F32& y)
    {
        if (sAccumulationFrame <= 0)
        {
            return false; // the first frame is the unshifted one
        }
        x = halton(sAccumulationFrame, 2) - 0.5f;
        y = halton(sAccumulationFrame, 3) - 0.5f;
        return true;
    }

    void bindShadowStrength(LLGLSLShader& shader)
    {
        // As a lift, so a shader never given it (0) shades as Firestorm's.
        static LLCachedControl<F32> strength(gSavedSettings, "LuminousPhotoShadowStrength", 1.f);
        static LLStaticHashedString s_lift("luminous_shadow_lift");
        shader.uniform1f(s_lift, 1.f - llclamp((F32)strength(), 0.f, 1.f));
    }

    void bindShadowUniforms(LLGLSLShader& shader)
    {
        bindFrame(shader);
        // Kept for renders, the live preview filters the maps as Firestorm does.
        static LLStaticHashedString s_soft("luminous_soft_shadows");
        shader.uniform1i(s_soft, softShadowsNow() ? 1 : 0);
        // The sun's apparent radius is 0.2665 degrees; Softness scales it, as
        // haze or thin cloud widens the sun's light.
        static LLCachedControl<F32> softness(gSavedSettings, "LuminousPhotoShadowSoftness", 2.5f);
        static LLStaticHashedString s_softness("luminous_shadow_softness");
        static LLStaticHashedString s_blocker_taps("luminous_blocker_taps");
        static LLStaticHashedString s_filter_taps("luminous_filter_taps");
        shader.uniform1i(s_blocker_taps, fullQuality(LiveQuality::Shadows) ? 32 : 16);
        shader.uniform1i(s_filter_taps, fullQuality(LiveQuality::Shadows) ? 64 : 32);
        // The widest penumbra is 64 texels of the chosen maps, and the same
        // width in metres on smaller maps -- the live view's, or a render's
        // made smaller for want of memory -- so that shadows on them are no
        // softer.
        static LLStaticHashedString s_max_texels("luminous_max_texels");
        static LLCachedControl<U32> picture_size(gSavedSettings, "LuminousPhotoShadowMapSize", 8192);
        F32 max_texels = 64.f;
        const U32 maps_size = llmax(gPipeline.mRT->shadow[0].getWidth(), gPipeline.mRT->shadow[0].getHeight());
        if (!gCubeSnapshot && picture_size() > 0 && maps_size > 0)
        {
            max_texels = llclamp(64.f * (F32)maps_size / (F32)llmin((U32)picture_size(), 16384U), 2.f, 64.f);
        }
        shader.uniform1f(s_max_texels, max_texels);
        static const F32 SUN_TAN_RADIUS = tanf(0.2665f * DEG_TO_RAD);
        // Sampled across the frames, the sun's own disc softens the shadows;
        // each frame's filter only fills the gaps.
        const F32 fill = shadowsSampled() ? fillScale() : 1.f;
        shader.uniform1f(s_softness, SUN_TAN_RADIUS * llclamp(softness(), 0.f, 20.f) * fill);
    }

    namespace
    {
        // White balance as a von Kries adaptation in LMS, after Unity's
        // ColorBalanceToLMSCoeffs: Warmth moves the assumed white along the
        // CIE daylight locus, Tint across it, and the picture is corrected
        // from that white to D65. Returned row-major, linear sRGB to linear sRGB.
        void whiteBalance(F32 warmth, F32 tint, F32 m[9])
        {
            static const F32 LIN_TO_LMS[9] = { 3.90405e-1f, 5.49941e-1f, 8.92632e-3f,
                                               7.08416e-2f, 9.63172e-1f, 1.35775e-3f,
                                               2.31082e-2f, 1.28021e-1f, 9.36245e-1f };
            static const F32 LMS_TO_LIN[9] = { 2.85847e+0f, -1.62879e+0f, -2.48910e-2f,
                                               -2.10182e-1f, 1.15820e+0f, 3.24281e-4f,
                                               -4.18120e-2f, -1.18169e-1f, 1.06867e+0f };
            if (warmth == 0.f && tint == 0.f)
            {
                const F32 identity[9] = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f };
                memcpy(m, identity, sizeof(identity));
                return;
            }
            const F32 t1 = warmth * 100.f / 65.f;
            const F32 t2 = tint * 100.f / 65.f;
            const F32 x = 0.31271f - t1 * (t1 < 0.f ? 0.1f : 0.05f);
            const F32 y = 2.87f * x - 3.f * x * x - 0.27509507f + t2 * 0.05f;
            const F32 X = x / y;
            const F32 Z = (1.f - x - y) / y;
            const F32 lms[3] = { 0.7328f * X + 0.4296f - 0.1624f * Z,
                                 -0.7036f * X + 1.6975f + 0.0061f * Z,
                                 0.0030f * X + 0.0136f + 0.9834f * Z };
            const F32 d65[3] = { 0.949237f, 1.03542f, 1.08728f };
            F32 k[3];
            for (S32 i = 0; i < 3; ++i)
            {
                k[i] = d65[i] / lms[i];
            }
            for (S32 r = 0; r < 3; ++r)
            {
                for (S32 c = 0; c < 3; ++c)
                {
                    F32 sum = 0.f;
                    for (S32 j = 0; j < 3; ++j)
                    {
                        sum += LMS_TO_LIN[r * 3 + j] * k[j] * LIN_TO_LMS[j * 3 + c];
                    }
                    m[r * 3 + c] = sum;
                }
            }
        }

        GLuint mcMapfaceLut()
        {
            static bool missing = false; // said once, not tried every frame
            if (sMcMapfaceLut || missing)
            {
                return sMcMapfaceLut;
            }
            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "luminous_tonemap", "tony_mc_mapface.rgb9e5");
            const size_t size = 48 * 48 * 48 * 4;
            std::vector<U8> data(size);
            LLFILE* file = LLFile::fopen(path, "rb");
            const bool read = file && fread(data.data(), 1, size, file) == size;
            if (file)
            {
                fclose(file);
            }
            if (!read)
            {
                LL_WARNS("LuminousPhotoMode") << "Tony McMapface's table missing: " << path << LL_ENDL;
                missing = true;
                return 0;
            }
            glGenTextures(1, &sMcMapfaceLut);
            glBindTexture(GL_TEXTURE_3D, sMcMapfaceLut);
            glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB9_E5, 48, 48, 48, 0, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, data.data());
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            return sMcMapfaceLut;
        }

        std::string lutFolder(bool user)
        {
            return user ? gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "luminous_luts")
                        : gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "luminous_luts");
        }

        std::string lutPath(const std::string& name)
        {
            for (bool user : { true, false })
            {
                for (const char* ext : { ".cube", ".CUBE", ".Cube" })
                {
                    std::string path = gDirUtilp->add(lutFolder(user), name + ext);
                    if (LLFile::isfile(path))
                    {
                        return path;
                    }
                }
            }
            return std::string();
        }

        // A .cube file: LUT_3D_SIZE N, then N^3 lines of red green blue, red
        // changing fastest; TITLE, DOMAIN_ and comments are skipped (domains
        // other than 0..1 are not supported).
        bool readCube(const std::string& path, std::vector<F32>& rgb, S32& size, std::string& error)
        {
            llifstream stream(path.c_str());
            if (!stream.is_open())
            {
                error = "could not open it";
                return false;
            }
            size = 0;
            rgb.clear();
            std::string line;
            bool first = true;
            while (std::getline(stream, line))
            {
                // Files from other tools: a UTF-8 byte-order mark, Windows line
                // ends, keywords in lower case, Resolve's LUT_3D_INPUT_RANGE.
                if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
                {
                    line.erase(0, 3);
                }
                first = false;
                LLStringUtil::trim(line);
                LLStringUtil::toUpper(line); // numbers are unaffected
                if (line.empty() || line[0] == '#' || line.compare(0, 5, "TITLE") == 0 || line.compare(0, 7, "DOMAIN_") == 0
                    || line.compare(0, 18, "LUT_3D_INPUT_RANGE") == 0 || line.compare(0, 18, "LUT_1D_INPUT_RANGE") == 0)
                {
                    continue;
                }
                if (line.compare(0, 11, "LUT_3D_SIZE") == 0)
                {
                    size = atoi(line.c_str() + 11);
                    if (size < 2 || size > 128)
                    {
                        error = "its size is not one Luminous reads (2 to 128)";
                        return false;
                    }
                    rgb.reserve((size_t)size * size * size * 3);
                    continue;
                }
                if (line.compare(0, 11, "LUT_1D_SIZE") == 0)
                {
                    error = "it is a 1D table; Luminous reads 3D ones";
                    return false;
                }
                F32 r, g, b;
                if (size > 0 && sscanf(line.c_str(), "%f %f %f", &r, &g, &b) == 3)
                {
                    rgb.push_back(r);
                    rgb.push_back(g);
                    rgb.push_back(b);
                }
            }
            if (size == 0 || rgb.size() != (size_t)size * size * size * 3)
            {
                error = "it is not a complete 3D .cube table";
                return false;
            }
            return true;
        }

        // The table named in LuminousPhotoLUT, loaded when the name changes.
        GLuint lookLut(const std::string& name)
        {
            if (name == sLookLutName)
            {
                return sLookLut;
            }
            sLookLutName = name;
            if (sLookLut)
            {
                glDeleteTextures(1, &sLookLut);
                sLookLut = 0;
            }
            std::vector<F32> rgb;
            S32 size = 0;
            std::string error;
            const std::string path = lutPath(name);
            if (path.empty() || !readCube(path, rgb, size, error))
            {
                LL_WARNS("LuminousPhotoMode") << "Look's table " << name << " not used: " << (path.empty() ? "not found" : error) << LL_ENDL;
                return 0;
            }
            glGenTextures(1, &sLookLut);
            glBindTexture(GL_TEXTURE_3D, sLookLut);
            glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16F, size, size, size, 0, GL_RGB, GL_FLOAT, rgb.data());
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            sLookLutSize = (F32)size;
            LL_INFOS("LuminousPhotoMode") << "Look's table " << name << ": " << size << "^3" << LL_ENDL;
            return sLookLut;
        }

        const char* const LOOK_SETTINGS[] = {
            "LuminousPhotoTonemapper", "LuminousPhotoExposure", "LuminousPhotoContrast",
            "LuminousPhotoSaturation", "LuminousPhotoWarmth", "LuminousPhotoTint",
            "LuminousPhotoMonochrome", "LuminousPhotoSepia", "LuminousPhotoHighlights",
            "LuminousPhotoShadows", "LuminousPhotoVibrance", "LuminousPhotoMonochromeFilter",
            "LuminousPhotoGrain", "LuminousPhotoGrainSize", "LuminousPhotoWhites", "LuminousPhotoBlacks",
            "LuminousPhotoShadowsWarmth", "LuminousPhotoShadowsTint", "LuminousPhotoMidtonesWarmth",
            "LuminousPhotoMidtonesTint", "LuminousPhotoHighlightsWarmth", "LuminousPhotoHighlightsTint",
            "LuminousPhotoClarity", "LuminousPhotoLUT", "LuminousPhotoLUTStrength", "LuminousPhotoTonemapMix",
            "LuminousPhotoColourAmount", "LuminousPhotoSplitTone",
            "LuminousPhotoHSLRedsHue", "LuminousPhotoHSLRedsSat", "LuminousPhotoHSLRedsLum",
            "LuminousPhotoHSLOrangesHue", "LuminousPhotoHSLOrangesSat", "LuminousPhotoHSLOrangesLum",
            "LuminousPhotoHSLYellowsHue", "LuminousPhotoHSLYellowsSat", "LuminousPhotoHSLYellowsLum",
            "LuminousPhotoHSLGreensHue", "LuminousPhotoHSLGreensSat", "LuminousPhotoHSLGreensLum",
            "LuminousPhotoHSLAquasHue", "LuminousPhotoHSLAquasSat", "LuminousPhotoHSLAquasLum",
            "LuminousPhotoHSLBluesHue", "LuminousPhotoHSLBluesSat", "LuminousPhotoHSLBluesLum",
            "LuminousPhotoHSLPurplesHue", "LuminousPhotoHSLPurplesSat", "LuminousPhotoHSLPurplesLum",
            "LuminousPhotoHSLMagentasHue", "LuminousPhotoHSLMagentasSat", "LuminousPhotoHSLMagentasLum",
        };

        // A range's colour balance as log2 gains on red, green and blue:
        // warmth trades blue for red, tint green for magenta.
        LLVector3 balanceGains(F32 warmth, F32 tint)
        {
            warmth = llclamp(warmth, -1.f, 1.f) * 0.5f;
            tint = llclamp(tint, -1.f, 1.f) * 0.5f;
            return LLVector3(warmth + 0.5f * tint, -tint, -warmth + 0.5f * tint);
        }

        // Black and white filters, as weights on linear red, green and blue
        // summing to one: none (luminance), red, orange, yellow, green, blue.
        const F32 MONO_FILTERS[6][3] = {
            { 0.2126f, 0.7152f, 0.0722f },
            { 1.00f, 0.25f, -0.25f },
            { 0.70f, 0.40f, -0.10f },
            { 0.45f, 0.55f, 0.00f },
            { 0.10f, 0.80f, 0.10f },
            { 0.05f, 0.35f, 0.60f },
        };
    }

    bool look()
    {
        return isOn();
    }

    void bindLookUniforms(LLGLSLShader& shader)
    {
        static LLCachedControl<S32> tonemapper(gSavedSettings, "LuminousPhotoTonemapper", -1);
        static LLCachedControl<F32> ev(gSavedSettings, "LuminousPhotoExposure", 0.f);
        static LLCachedControl<F32> contrast(gSavedSettings, "LuminousPhotoContrast", 1.f);
        static LLCachedControl<F32> saturation(gSavedSettings, "LuminousPhotoSaturation", 1.f);
        static LLCachedControl<F32> warmth(gSavedSettings, "LuminousPhotoWarmth", 0.f);
        static LLCachedControl<F32> tint(gSavedSettings, "LuminousPhotoTint", 0.f);
        static LLCachedControl<F32> mono(gSavedSettings, "LuminousPhotoMonochrome", 0.f);
        static LLCachedControl<F32> sepia(gSavedSettings, "LuminousPhotoSepia", 0.f);
        static LLStaticHashedString s_type("tonemap_type");
        static LLStaticHashedString s_balance("luminous_look_balance");
        static LLStaticHashedString s_ev("luminous_look_ev");
        static LLStaticHashedString s_contrast("luminous_look_contrast");
        static LLStaticHashedString s_saturation("luminous_look_saturation");
        static LLStaticHashedString s_mono("luminous_look_mono");
        static LLStaticHashedString s_tone("luminous_look_tone");
        static LLCachedControl<F32> highlights(gSavedSettings, "LuminousPhotoHighlights", 0.f);
        static LLCachedControl<F32> shadows(gSavedSettings, "LuminousPhotoShadows", 0.f);
        static LLCachedControl<F32> vibrance(gSavedSettings, "LuminousPhotoVibrance", 0.f);
        static LLCachedControl<S32> mono_filter(gSavedSettings, "LuminousPhotoMonochromeFilter", 0);
        static LLCachedControl<F32> grain(gSavedSettings, "LuminousPhotoGrain", 0.f);
        static LLCachedControl<F32> grain_size(gSavedSettings, "LuminousPhotoGrainSize", 1.5f);
        static LLStaticHashedString s_highlights("luminous_look_highlights");
        static LLStaticHashedString s_shadows("luminous_look_shadows");
        static LLStaticHashedString s_vibrance("luminous_look_vibrance");
        static LLStaticHashedString s_mono_weights("luminous_look_mono_weights");
        static LLStaticHashedString s_grain("luminous_look_grain");
        static LLStaticHashedString s_grain_size("luminous_look_grain_size");
        static LLCachedControl<F32> whites(gSavedSettings, "LuminousPhotoWhites", 0.f);
        static LLCachedControl<F32> blacks(gSavedSettings, "LuminousPhotoBlacks", 0.f);
        static LLCachedControl<F32> sh_warm(gSavedSettings, "LuminousPhotoShadowsWarmth", 0.f);
        static LLCachedControl<F32> sh_tint(gSavedSettings, "LuminousPhotoShadowsTint", 0.f);
        static LLCachedControl<F32> mid_warm(gSavedSettings, "LuminousPhotoMidtonesWarmth", 0.f);
        static LLCachedControl<F32> mid_tint(gSavedSettings, "LuminousPhotoMidtonesTint", 0.f);
        static LLCachedControl<F32> hi_warm(gSavedSettings, "LuminousPhotoHighlightsWarmth", 0.f);
        static LLCachedControl<F32> hi_tint(gSavedSettings, "LuminousPhotoHighlightsTint", 0.f);
        static LLStaticHashedString s_whites("luminous_look_whites");
        static LLStaticHashedString s_blacks("luminous_look_blacks");
        static LLStaticHashedString s_bal_sh("luminous_look_balance_shadows");
        static LLStaticHashedString s_bal_mid("luminous_look_balance_midtones");
        static LLStaticHashedString s_bal_hi("luminous_look_balance_highlights");

        // -1 leaves Firestorm's choice (RenderTonemapType, in Phototools).
        if (tonemapper() >= 0)
        {
            // Tony McMapface without its table would draw black: Neutral instead.
            S32 type = llmin((S32)tonemapper(), 6);
            if (type == 6 && !mcMapfaceLut())
            {
                type = 0;
            }
            shader.uniform1i(s_type, type);
        }
        // How much of the curve: below 0, the sky's own tone mix.
        static LLCachedControl<F32> mix(gSavedSettings, "LuminousPhotoTonemapMix", -1.f);
        static LLStaticHashedString s_mix("tonemap_mix");
        if (mix() >= 0.f)
        {
            shader.uniform1f(s_mix, llclamp(mix(), 0.f, 1.f));
        }
        static LLStaticHashedString s_on("luminous_look_on");
        shader.uniform1i(s_on, gSnapshotNoPost ? 0 : 1);
        // Tony McMapface's table, a 3D texture. Firestorm gives texture
        // channels only to the sampler names it knows, so it is bound to the
        // last channel by hand; the tonemap programs' own samplers sit low.
        static LLStaticHashedString s_lut("luminous_tonemap_lut");
        GLint lut_location = shader.getUniformLocation(s_lut);
        if (lut_location >= 0)
        {
            const GLint channel = 15;
            glActiveTexture(GL_TEXTURE0 + channel);
            glBindTexture(GL_TEXTURE_3D, mcMapfaceLut());
            glActiveTexture(GL_TEXTURE0 + gGL.getCurrentTexUnitIndex());
            glUniform1i(lut_location, channel);
        }
        // The .cube table, on channel 14, likewise by hand.
        static LLCachedControl<std::string> lut_name(gSavedSettings, "LuminousPhotoLUT", "");
        static LLCachedControl<F32> lut_strength(gSavedSettings, "LuminousPhotoLUTStrength", 1.f);
        static LLStaticHashedString s_lut_map("luminous_lut");
        static LLStaticHashedString s_lut_strength("luminous_lut_strength");
        static LLStaticHashedString s_lut_size("luminous_lut_size");
        GLuint look_lut = lut_name().empty() ? 0 : lookLut(lut_name());
        GLint lut_map = shader.getUniformLocation(s_lut_map);
        if (lut_map >= 0)
        {
            const GLint channel = 14;
            glActiveTexture(GL_TEXTURE0 + channel);
            glBindTexture(GL_TEXTURE_3D, look_lut);
            glActiveTexture(GL_TEXTURE0 + gGL.getCurrentTexUnitIndex());
            glUniform1i(lut_map, channel);
        }
        shader.uniform1f(s_lut_strength, look_lut ? llclamp(lut_strength(), 0.f, 1.f) : 0.f);
        shader.uniform1f(s_lut_size, llmax(sLookLutSize, 2.f));
        // HSL by colour range: hue, saturation, lightness for each of eight.
        static const char* const HSL_RANGES[8] = { "Reds", "Oranges", "Yellows", "Greens", "Aquas", "Blues", "Purples", "Magentas" };
        static LLStaticHashedString s_hsl("luminous_hsl");
        // The 24 settings found by name once, not each frame.
        static LLControlVariable* hsl_controls[24] = {};
        static bool hsl_found = false;
        if (!hsl_found)
        {
            static const char* const PARTS[3] = { "Hue", "Sat", "Lum" };
            for (S32 r = 0; r < 8; ++r)
            {
                for (S32 k = 0; k < 3; ++k)
                {
                    hsl_controls[r * 3 + k] = gSavedSettings.getControl(std::string("LuminousPhotoHSL") + HSL_RANGES[r] + PARTS[k]);
                }
            }
            hsl_found = true;
        }
        F32 hsl[24];
        for (S32 i = 0; i < 24; ++i)
        {
            hsl[i] = hsl_controls[i] ? llclamp((F32)hsl_controls[i]->getValue().asReal(), -1.f, 1.f) : 0.f;
        }
        shader.uniform3fv(s_hsl, 8, hsl);
        F32 balance[9];
        whiteBalance(llclamp(warmth(), -1.f, 1.f), llclamp(tint(), -1.f, 1.f), balance);
        GLint location = shader.getUniformLocation(s_balance);
        if (location >= 0)
        {
            glUniformMatrix3fv(location, 1, GL_TRUE, balance);
        }
        shader.uniform1f(s_ev, powf(2.f, llclamp(ev(), -5.f, 5.f)));
        shader.uniform1f(s_contrast, llclamp(contrast(), 0.25f, 4.f));
        shader.uniform1f(s_saturation, llclamp(saturation(), 0.f, 3.f));
        shader.uniform1f(s_mono, llclamp(mono(), 0.f, 1.f));
        shader.uniform1f(s_tone, llclamp(sepia(), 0.f, 1.f));
        shader.uniform1f(s_highlights, llclamp(highlights(), -1.f, 1.f));
        shader.uniform1f(s_shadows, llclamp(shadows(), -1.f, 1.f));
        shader.uniform1f(s_vibrance, llclamp(vibrance(), -1.f, 1.f));
        shader.uniform3fv(s_mono_weights, 1, MONO_FILTERS[llclamp((S32)mono_filter(), 0, 5)]);
        // kept for renders: none live; after sharpening: in that pass instead
        shader.uniform1f(s_grain, drawnNow(Kept::Grain) && !sGrainAfterSharpening ? llclamp((F32)grain(), 0.f, 1.f) : 0.f);
        shader.uniform1f(s_grain_size, llclamp(grain_size(), 1.f, 4.f));
        shader.uniform1f(s_whites, llclamp(whites(), -1.f, 1.f));
        shader.uniform1f(s_blacks, llclamp(blacks(), -1.f, 1.f));
        shader.uniform3fv(s_bal_sh, 1, balanceGains(sh_warm(), sh_tint()).mV);
        shader.uniform3fv(s_bal_mid, 1, balanceGains(mid_warm(), mid_tint()).mV);
        shader.uniform3fv(s_bal_hi, 1, balanceGains(hi_warm(), hi_tint()).mV);
    }

    void resetLook()
    {
        HoldShaderRebuilds hold;
        for (const char* name : LOOK_SETTINGS)
        {
            if (LLControlVariable* control = gSavedSettings.getControl(name))
            {
                control->resetToDefault(true);
            }
        }
    }

    namespace
    {
        // Presets: one LLSD file per name in a folder.
        std::string namedFile(const std::string& folder, const std::string& name)
        {
            return gDirUtilp->add(folder, LLDir::getScrubbedFileName(name) + ".xml");
        }

        void listNamed(const std::string& folder, std::vector<std::string>& names)
        {
            if (!LLFile::isdir(folder))
            {
                return;
            }
            std::vector<std::string> found;
            LLDirIterator iter(folder, "*.xml");
            std::string file;
            while (iter.next(file))
            {
                LLSD named;
                llifstream stream(gDirUtilp->add(folder, file).c_str());
                if (LLSDSerialize::fromXML(named, stream) > 0 && named.isMap())
                {
                    std::string name = named.has("name") ? named["name"].asString() : gDirUtilp->getBaseFileName(file, true);
                    if (std::find(names.begin(), names.end(), name) == names.end())
                    {
                        found.push_back(name);
                    }
                }
            }
            std::sort(found.begin(), found.end());
            names.insert(names.end(), found.begin(), found.end());
        }

    }

    std::vector<std::string> lutNames()
    {
        std::vector<std::string> names;
        for (bool user : { false, true })
        {
            const std::string folder = lutFolder(user);
            if (!LLFile::isdir(folder))
            {
                continue;
            }
            std::vector<std::string> found;
            LLDirIterator iter(folder, "*");
            std::string file;
            while (iter.next(file))
            {
                std::string ext = gDirUtilp->getExtension(file); // lower case
                if (ext != "cube")
                {
                    continue;
                }
                std::string name = gDirUtilp->getBaseFileName(file, true);
                if (std::find(names.begin(), names.end(), name) == names.end())
                {
                    found.push_back(name);
                }
            }
            std::sort(found.begin(), found.end());
            names.insert(names.end(), found.begin(), found.end());
        }
        return names;
    }

    bool importLUT(const std::string& path, std::string& name, std::string& error)
    {
        std::vector<F32> rgb;
        S32 size = 0;
        if (!readCube(path, rgb, size, error))
        {
            return false;
        }
        const std::string folder = lutFolder(true);
        if (!LLFile::isdir(folder))
        {
            LLFile::mkdir(folder);
        }
        name = gDirUtilp->getBaseFileName(path, true);
        // A table already in the folder -- under any case of its name, as the
        // Mac's disk ignores case -- is not copied onto itself, which would
        // empty it.
        const std::string dest = gDirUtilp->add(folder, name + ".cube");
        char real_from[PATH_MAX];
        char real_to[PATH_MAX];
        if (realpath(path.c_str(), real_from) && realpath(dest.c_str(), real_to) && strcasecmp(real_from, real_to) == 0)
        {
            sLookLutName = "\n";
            return true;
        }
        if (!LLFile::copy(path, dest))
        {
            error = "could not copy it into " + folder;
            return false;
        }
        sLookLutName = "\n"; // reload, if a table of that name was showing
        return true;
    }

    namespace
    {
        // The lens with the camera: depth of field on or off, the aperture
        // (f-number and blades), focal length and Most blur, and Lens blur's
        // switches, so that a saved camera position keeps its field of view,
        // focus and aperture.
        const char* const SHOT_SETTINGS[] = {
            "RenderDepthOfField", "CameraFNumber", "CameraFocalLength", "CameraMaxCoF",
            "LuminousPhotoDoF", "LuminousPhotoDoFFront", "LuminousPhotoDoFBlades",
        };

        std::string shotsFile()
        {
            return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "luminous_shots.xml");
        }

        LLSD readShots()
        {
            LLSD shots = LLSD::emptyMap();
            const std::string path = shotsFile();
            if (LLFile::isfile(path))
            {
                llifstream stream(path.c_str());
                if (LLSDSerialize::fromXML(shots, stream) <= 0 || !shots.isMap())
                {
                    shots = LLSD::emptyMap();
                }
            }
            return shots;
        }

        bool writeShots(const LLSD& shots)
        {
            llofstream stream(shotsFile().c_str());
            if (!stream.is_open())
            {
                return false;
            }
            LLSDSerialize::toPrettyXML(shots, stream);
            return true;
        }
    }

    std::vector<std::string> shotNames()
    {
        std::vector<std::string> names;
        LLSD shots = readShots();
        for (auto it = shots.beginMap(); it != shots.endMap(); ++it)
        {
            names.push_back(it->first);
        }
        return names; // LLSD maps are ordered
    }

    bool saveShot(const std::string& name)
    {
        if (name.empty() || gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "").empty())
        {
            return false;
        }
        // The camera as rendered -- a scripted or follow camera may have moved
        // it from where the agent's camera thinks it is -- aimed along its
        // view at the distance of the focus, which depth of field focuses on.
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3d origin = gAgent.getPosGlobalFromAgent(camera->getOrigin());
        F64 distance = (gAgentCamera.getFocusGlobal() - origin).magVec();
        // With depth of field on, the distance it focuses at -- where the
        // focus was clicked or locked, which need not be the camera's focus.
        if (gPipeline.RenderDepthOfField && sNotedFocus > 0.25f)
        {
            distance = sNotedFocus;
        }
        if (distance < 0.25 || distance > 256.0)
        {
            distance = 5.0;
        }
        LLSD shot;
        shot["camera"] = ll_sd_from_vector3d(origin);
        shot["focus"] = ll_sd_from_vector3d(origin + LLVector3d(camera->getAtAxis()) * distance);
        shot["fov"] = LLViewerCamera::getInstance()->getDefaultFOV();
        for (const char* setting : SHOT_SETTINGS)
        {
            shot[setting] = gSavedSettings.getControl(setting)->getValue();
        }
        LLSD shots = readShots();
        shots[name] = shot;
        return writeShots(shots);
    }

    bool goToShot(const std::string& name, std::string& error)
    {
        LLSD shots = readShots();
        if (!shots.has(name))
        {
            error = "there is no shot called " + name;
            return false;
        }
        const LLSD& shot = shots[name];
        const LLVector3d camera = ll_vector3d_from_sd(shot["camera"]);
        const LLVector3d focus = ll_vector3d_from_sd(shot["focus"]);
        if ((camera - gAgent.getPositionGlobal()).magVec() > 256.0)
        {
            error = "it was saved somewhere else; go there first";
            return false;
        }
        for (const char* setting : SHOT_SETTINGS)
        {
            if (shot.has(setting))
            {
                gSavedSettings.getControl(setting)->setValue(shot[setting]);
            }
        }
        if (shot.has("fov"))
        {
            LLViewerCamera::getInstance()->setDefaultFOV((F32)shot["fov"].asReal());
        }
        // As Firestorm's camera commands do: free the camera from the avatar
        // first, or it snaps back behind her.
        gAgentCamera.unlockView();
        gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setCameraPosAndFocusGlobal(camera, focus, LLUUID::null);
        // Focus where the shot was focused, not on a point locked since.
        LLPipeline::FSFocusPointLocked = false;
        return true;
    }

    bool deleteShot(const std::string& name)
    {
        LLSD shots = readShots();
        if (!shots.has(name))
        {
            return false;
        }
        shots.erase(name);
        return writeShots(shots);
    }

    namespace
    {
        // What a preset holds: the settings the Lighting, Tone, Colour and
        // Effects tabs' Reset buttons put back (floater_luminous.xml), in that
        // order, Firestorm's depth of field switch, which the Effects tab's
        // button sets, the legacy-sky switch and focus following the mouse --
        // and, apart from these, the Lighting tab's sky and sun (savePreset).
        // A setting added to one of those tabs belongs here too.
        const char* const PRESET_SETTINGS[] = {
            // Lighting
            "LuminousPhotoVolumetricLight", "LuminousPhotoVolumetricDensity", "LuminousPhotoVolumetricAnisotropy",
            "LuminousPhotoVolumetricDistance", "LuminousPhotoProjectorBeams", "LuminousPhotoProjectorBeamsDensity",
            "LuminousPhotoProjectorBeamsAnisotropy", "LuminousPhotoSunShafts", "LuminousPhotoSunShaftsStrength",
            "LuminousPhotoSunShaftsLength", "LuminousPhotoSunShaftsSoftness", "LuminousPhotoSoftShadows",
            "LuminousPhotoSoftShadowsRenderOnly", "LuminousPhotoShadowSoftness", "RenderShadowBlurSize", "RenderAttachedLights", "LuminousPhotoAO", "RenderDeferredSSAO", "LuminousPhotoAORadius", "LuminousPhotoAOStrength", "LuminousPhotoAOSoften", "LuminousPhotoAOLights",
            "LuminousPhotoVolumetricAmount", "LuminousPhotoBeamsAmount", "LuminousPhotoShaftsAmount",
            "LuminousPhotoAOAmount", "LuminousPhotoSSR", "LuminousPhotoAlphaSort", "LuminousPhotoAlphaSortFaintSL",
            "LuminousPhotoVolumetricLightRenderOnly",
            "LuminousPhotoProjectorBeamsRenderOnly", "LuminousPhotoSunShaftsRenderOnly",
            "LuminousPhotoAORenderOnly",
            "LuminousPhotoContactShadows", "LuminousPhotoContactShadowsLength", "LuminousPhotoContactShadowsRenderOnly",
            "LuminousPhotoContactShadowsStrength", "LuminousPhotoShadowStrength",
            
            // Tone
            "LuminousPhotoTonemapper", "LuminousPhotoTonemapMix", "LuminousPhotoExposure",
            "LuminousPhotoContrast", "LuminousPhotoHighlights", "LuminousPhotoShadows", "LuminousPhotoWhites",
            "LuminousPhotoBlacks", "LuminousPhotoClarity", "LuminousPhotoClarityRenderOnly", "RenderCASSharpness",
            "RenderSkyAutoAdjustLegacy",
            // Colour
            "LuminousPhotoSaturation", "LuminousPhotoVibrance", "LuminousPhotoWarmth", "LuminousPhotoTint",
            "LuminousPhotoShadowsWarmth", "LuminousPhotoShadowsTint", "LuminousPhotoMidtonesWarmth",
            "LuminousPhotoMidtonesTint", "LuminousPhotoHighlightsWarmth", "LuminousPhotoHighlightsTint",
            "LuminousPhotoMonochrome", "LuminousPhotoMonochromeFilter", "LuminousPhotoSepia", "LuminousPhotoLUT",
            "LuminousPhotoLUTStrength", "LuminousPhotoHSLRedsHue", "LuminousPhotoHSLRedsSat",
            "LuminousPhotoHSLRedsLum", "LuminousPhotoHSLOrangesHue", "LuminousPhotoHSLOrangesSat",
            "LuminousPhotoHSLOrangesLum", "LuminousPhotoHSLYellowsHue", "LuminousPhotoHSLYellowsSat",
            "LuminousPhotoHSLYellowsLum", "LuminousPhotoHSLGreensHue", "LuminousPhotoHSLGreensSat",
            "LuminousPhotoHSLGreensLum", "LuminousPhotoHSLAquasHue", "LuminousPhotoHSLAquasSat",
            "LuminousPhotoHSLAquasLum", "LuminousPhotoHSLBluesHue", "LuminousPhotoHSLBluesSat",
            "LuminousPhotoHSLBluesLum", "LuminousPhotoHSLPurplesHue", "LuminousPhotoHSLPurplesSat",
            "LuminousPhotoHSLPurplesLum", "LuminousPhotoHSLMagentasHue", "LuminousPhotoHSLMagentasSat",
            "LuminousPhotoHSLMagentasLum", "LuminousPhotoColourAmount", "LuminousPhotoSplitTone",
            // Lens
            "RenderDepthOfField", "LuminousPhotoDepthOfFieldRenderOnly", "FSFocusPointFollowsPointer",
            "LuminousPhotoDoF", "LuminousPhotoDoFFront", "CameraFNumber", "CameraMaxCoF", "LuminousPhotoDoFBlades",
            "LuminousPhotoVignette", "LuminousPhotoVignetteAmount", "LuminousPhotoVignetteSize",
            "LuminousPhotoVignetteSoftness", "LuminousPhotoVignetteRoundness", "LuminousPhotoBloom",
            "LuminousPhotoBloomStrength", "LuminousPhotoBloomSize", "RenderGlow", "RenderGlowStrength", "RenderGlowWidth",
            "LuminousPhotoChromaticAberration",
            "LuminousPhotoChromaticAberrationAmount", "LuminousPhotoHalation",
            "LuminousPhotoAnamorphic", "LuminousPhotoAnamorphicTint", "LuminousPhotoDistortion", "LuminousPhotoDoFBlur", "LuminousPhotoGrain",
            "LuminousPhotoGrainSize", "LuminousPhotoDoFRenderOnly", "LuminousPhotoVignetteRenderOnly",
            "LuminousPhotoBloomRenderOnly", "LuminousPhotoChromaticAberrationRenderOnly",
            "LuminousPhotoHalationRenderOnly", "LuminousPhotoAnamorphicRenderOnly",
            "LuminousPhotoDistortionRenderOnly", "LuminousPhotoGrainRenderOnly",
            "LuminousPhotoLensFlare", "LuminousPhotoLensFlareGhosts", "LuminousPhotoLensFlareHalo",
            "LuminousPhotoLensFlareStarburst", "LuminousPhotoLensFlareStarBlur", "LuminousPhotoLensFlareDirt", "LuminousPhotoLensFlareVariation",
            "LuminousPhotoLensFlareAmplify",
            "LuminousPhotoLensFlareRenderOnly",
        };

        std::string presetsFolder()
        {
            return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "luminous_presets");
        }

        // A look's settings, complete as a preset is: what it does not name
        // goes back to as Luminous ships it. Only what differs is set (some
        // settings rebuild shaders).
        void applySettings(const LLSD& values)
        {
            HoldShaderRebuilds hold;
            for (const char* setting : PRESET_SETTINGS)
            {
                LLControlVariable* control = gSavedSettings.getControl(setting);
                if (!control)
                {
                    continue;
                }
                const LLSD value = values.has(setting) ? values[setting] : control->getDefault();
                if (!llsd_equals(control->getValue(), value))
                {
                    control->setValue(value);
                }
            }
        }

        // The user's preset of a name. Luminous ships none: its styles do
        // that job, with the light and the camera.
        bool readPreset(const std::string& name, LLSD& preset)
        {
            const std::string path = namedFile(presetsFolder(), name);
            if (LLFile::isfile(path))
            {
                llifstream stream(path.c_str());
                return LLSDSerialize::fromXML(preset, stream) > 0 && preset.isMap();
            }
            return false;
        }
    }

    std::vector<std::string> presetNames()
    {
        std::vector<std::string> names;
        listNamed(presetsFolder(), names);
        return names;
    }

    void applyPresetEnvironment(const LLSD& preset); // below, with the environments

    bool applyPreset(const std::string& name)
    {
        LLSD preset;
        if (!readPreset(name, preset))
        {
            return false;
        }
        applySettings(preset);
        applyPresetEnvironment(preset);
        LL_INFOS("LuminousPhotoMode") << "Preset applied: " << name << LL_ENDL;
        return true;
    }

    namespace
    {
        std::string environmentsFolder()
        {
            return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "luminous_environments");
        }

        LLSettingsSky::ptr_t sEnvironmentSky; // the environment shown, as made, before its sun is placed
        // Its keys, if it has them: skies at sun heights, mixed for the
        // heights between as a day cycle mixes its own, so that the sky
        // fits the sun at any height.
        std::vector<std::pair<F32, LLSettingsSky::ptr_t>> sEnvironmentKeys;
        bool sOwnSky = false;                 // the personal sky showing is one of these, or the region's with its sun moved
        bool sKeepProbes = false;             // only the sun moves: the reflection probes are kept

        LLSettingsSky::ptr_t shownSky()
        {
            LLEnvironment& environment = LLEnvironment::instance();
            return environment.getCurrentEnvironmentInstance() ? environment.getCurrentSky() : LLSettingsSky::ptr_t();
        }

        // Personal Lighting's own conversion (onSunAzimElevChanged).
        LLQuaternion sunRotation(F32 azimuth_deg, F32 elevation_deg)
        {
            F32 elevation = elevation_deg * DEG_TO_RAD;
            if (is_approx_zero(elevation))
            {
                elevation = F_APPROXIMATELY_ZERO;
            }
            LLQuaternion quat;
            quat.setAngleAxis(-elevation, 0, 1, 0);
            LLQuaternion az_quat;
            az_quat.setAngleAxis(F_TWO_PI - azimuth_deg * DEG_TO_RAD, 0, 0, 1);
            quat *= az_quat;
            return quat;
        }

        // The environment's sky for a sun height: its keys mixed, the
        // nearest beyond them; or its one sky.
        LLSettingsSky::ptr_t environmentSkyAt(F32 elevation)
        {
            if (sEnvironmentKeys.empty())
            {
                return sEnvironmentSky ? sEnvironmentSky->buildClone() : LLSettingsSky::ptr_t();
            }
            if (elevation <= sEnvironmentKeys.front().first || sEnvironmentKeys.size() == 1)
            {
                return sEnvironmentKeys.front().second->buildClone();
            }
            for (size_t i = 1; i < sEnvironmentKeys.size(); ++i)
            {
                const auto& below = sEnvironmentKeys[i - 1];
                const auto& above = sEnvironmentKeys[i];
                if (elevation <= above.first)
                {
                    LLSettingsSky::ptr_t sky = below.second->buildClone();
                    LLSettingsBase::ptr_t end = above.second;
                    sky->blend(end, (elevation - below.first) / llmax(above.first - below.first, 0.01f));
                    return sky;
                }
            }
            return sEnvironmentKeys.back().second->buildClone();
        }

        bool sOwnWater = false; // the personal water showing came with one of these

        void showSky(const LLSettingsSky::ptr_t& sky, F32 azimuth, F32 elevation,
                     const LLSettingsWater::ptr_t& water = LLSettingsWater::ptr_t())
        {
            sky->setSunRotation(sunRotation(azimuth, elevation));
            sky->update();
            LLEnvironment& environment = LLEnvironment::instance();
            if (water)
            {
                water->update();
                environment.setEnvironment(LLEnvironment::ENV_LOCAL, sky, water);
            }
            else
            {
                environment.setEnvironment(LLEnvironment::ENV_LOCAL, sky);
            }
            environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
            environment.updateEnvironment(LLEnvironment::TRANSITION_INSTANT);
            sOwnSky = true;
        }
    }

    std::vector<std::string> environmentNames()
    {
        std::vector<std::string> names;
        listNamed(environmentsFolder(), names);
        return names;
    }

    // The styles' environments as Second Life environment items, in
    // #Luminous/#Environments: at login, once per account and version of
    // them, one sky per style, named as the style and made at its sun's
    // height, and the water of an environment that has its own -- usable in
    // Second Life's own environment windows, on land, and by friends. A
    // network call: each is uploaded to the grid as an inventory item
    // (settings items cost nothing). Names already there are left alone, so
    // it never makes doubles.
    namespace
    {
        constexpr S32 ENVIRONMENTS_VERSION = 2; // raise when the shipped styles change
        const std::string ENVIRONMENTS_FOLDER = "#Environments";
        const std::string OLD_ENVIRONMENTS_FOLDER = "Environments"; // version 1's, one sky per environment

        LLSD readDoc(const std::string& folder, const std::string& name)
        {
            LLSD doc;
            llifstream stream(namedFile(folder, name).c_str());
            if (!LLSDSerialize::fromXML(doc, stream) || !doc.isMap())
            {
                return LLSD();
            }
            return doc;
        }

        // An environment file's sky for a sun height, its keys mixed as
        // environmentSkyAt mixes them for the one shown
        LLSettingsSky::ptr_t environmentSkyFor(const LLSD& doc, F32 elevation)
        {
            LLSettingsSky::ptr_t sky = doc.has("sky") ? LLSettingsVOSky::buildSky(doc["sky"]) : LLSettingsSky::ptr_t();
            std::vector<std::pair<F32, LLSettingsSky::ptr_t>> keys;
            for (const LLSD& key : llsd::inArray(doc["keys"]))
            {
                if (LLSettingsSky::ptr_t key_sky = LLSettingsVOSky::buildSky(key["sky"]))
                {
                    keys.emplace_back((F32)key["elevation"].asReal(), key_sky);
                }
            }
            if (keys.empty())
            {
                return sky;
            }
            std::sort(keys.begin(), keys.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            if (elevation <= keys.front().first || keys.size() == 1)
            {
                return keys.front().second->buildClone();
            }
            for (size_t i = 1; i < keys.size(); ++i)
            {
                if (elevation <= keys[i].first)
                {
                    LLSettingsSky::ptr_t mixed = keys[i - 1].second->buildClone();
                    LLSettingsBase::ptr_t end = keys[i].second;
                    mixed->blend(end, (elevation - keys[i - 1].first) / llmax(keys[i].first - keys[i - 1].first, 0.01f));
                    return mixed;
                }
            }
            return keys.back().second->buildClone();
        }

        void fillEnvironments(const LLUUID& folder, S32 tries)
        {
            LLViewerInventoryCategory* cat = gInventory.getCategory(folder);
            if (!cat)
            {
                return;
            }
            if (cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
            {
                // what is in it must be known, or it would make doubles
                if (tries <= 0)
                {
                    LL_WARNS("LuminousPhotoMode") << "#Luminous/" << ENVIRONMENTS_FOLDER << " did not load; trying again next login." << LL_ENDL;
                    return;
                }
                cat->fetch();
                doAfterInterval([folder, tries]() { fillEnvironments(folder, tries - 1); }, 5.f);
                return;
            }
            std::set<std::string> have;
            LLInventoryModel::cat_array_t* cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(folder, cats, items);
            if (items)
            {
                for (const auto& item : *items)
                {
                    have.insert(item->getName());
                }
            }
            const std::string styles = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "luminous_styles");
            std::set<std::string> waters; // environments whose water goes in too
            S32 made = 0;
            for (const std::string& name : styleNames())
            {
                const LLSD style = readDoc(styles, name);
                const std::string environment = style["environment"].asString();
                const LLSD doc = environment.empty() ? LLSD() : readDoc(environmentsFolder(), environment);
                if (!doc.isMap())
                {
                    continue;
                }
                if (doc.has("water") && doc["water"].isMap())
                {
                    waters.insert(environment);
                }
                if (have.count(name))
                {
                    continue;
                }
                // the style's sun height; its direction comes from the camera
                // in a style, so the item has the sun from the south
                const F32 elevation = style.has("sun_elevation") ? (F32)style["sun_elevation"].asReal()
                                                                 : doc.has("sun_elevation") ? (F32)doc["sun_elevation"].asReal() : 30.f;
                if (LLSettingsSky::ptr_t sky = environmentSkyFor(doc, elevation))
                {
                    sky->setSunRotation(sunRotation(90.f, elevation));
                    sky->setName(name);
                    sky->update();
                    LLSettingsVOBase::createInventoryItem(sky, folder, name);
                    ++made;
                }
            }
            for (const std::string& environment : waters)
            {
                const std::string water_name = environment + " water";
                if (have.count(water_name))
                {
                    continue;
                }
                const LLSD doc = readDoc(environmentsFolder(), environment);
                LLSettingsWater::ptr_t water = std::make_shared<LLSettingsVOWater>(doc["water"]);
                water->setName(water_name);
                water->update();
                LLSettingsVOBase::createInventoryItem(water, folder, water_name);
                ++made;
            }
            gSavedPerAccountSettings.setS32("LuminousEnvironmentsInInventory", ENVIRONMENTS_VERSION);
            LL_INFOS("LuminousPhotoMode") << "Environments put in #Luminous/" << ENVIRONMENTS_FOLDER << ": " << made
                                          << " made, " << have.size() << " there already." << LL_ENDL;
        }
    }

    void addEnvironmentsToInventory()
    {
        if (gSavedPerAccountSettings.getS32("LuminousEnvironmentsInInventory") >= ENVIRONMENTS_VERSION)
        {
            return;
        }
        if (!gInventory.isInventoryUsable() || !LLEnvironment::instance().isInventoryEnabled())
        {
            LL_INFOS("LuminousPhotoMode") << "Environments not put in inventory now (inventory or region not ready); next login." << LL_ENDL;
            return;
        }
        LumenFolders::ensureFolder(LumenFolders::LUMINOUS_FOLDER, [](const LLUUID& root)
        {
            LLInventoryModel::cat_array_t* cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(root, cats, items);
            LLUUID existing;
            std::vector<LLUUID> old_folders;
            if (cats)
            {
                for (const auto& cat : *cats)
                {
                    if (cat->getName() == ENVIRONMENTS_FOLDER)
                    {
                        existing = cat->getUUID();
                    }
                    else if (cat->getName() == OLD_ENVIRONMENTS_FOLDER)
                    {
                        old_folders.push_back(cat->getUUID());
                    }
                }
            }
            // version 1's folder, which #Environments replaces, to the
            // Trash, which keeps it
            for (const LLUUID& old : old_folders)
            {
                LL_INFOS("LuminousPhotoMode") << "Moving #Luminous/" << OLD_ENVIRONMENTS_FOLDER << " to the Trash." << LL_ENDL;
                gInventory.removeCategory(old);
            }
            if (existing.notNull())
            {
                fillEnvironments(existing, 6);
                return;
            }
            gInventory.createNewCategory(root, LLFolderType::FT_NONE, ENVIRONMENTS_FOLDER, [](const LLUUID& id)
            {
                if (id.notNull())
                {
                    fillEnvironments(id, 6);
                }
            });
        });
    }

    // The user's own PBR skies, from inventory, listed in the Sky menu after
    // Luminous's own environments. Whether a sky is PBR is in its asset --
    // reflection probe ambiance above 0, the test LLPipeline::tonemap makes
    // -- not in the inventory, so each sky's asset is fetched once (a
    // network call; small, and usually cached) and the answer kept for the
    // session.
    namespace
    {
        const std::string INVENTORY_SKY = "inv:";  // the Sky menu's value for one: inv:<item id>
        std::map<LLUUID, bool> sSkyIsPBR;          // by asset
        std::set<LLUUID> sSkyAsked;                // assets being fetched
        std::function<void()> sSkiesChanged;
        bool sSkiesChangePending = false;

        // #Luminous/#Environments: Luminous's own, already first in the menu
        LLUUID ownEnvironmentsFolder()
        {
            const LLUUID root = gInventory.findCategoryByName(LumenFolders::LUMINOUS_FOLDER);
            if (root.isNull())
            {
                return LLUUID::null;
            }
            LLInventoryModel::cat_array_t* cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(root, cats, items);
            if (cats)
            {
                for (const auto& cat : *cats)
                {
                    if (cat->getName() == "#Environments")
                    {
                        return cat->getUUID();
                    }
                }
            }
            return LLUUID::null;
        }

        // The user's skies, walked out of the whole inventory once and again
        // only after it has changed, not at every scan, every opening of the
        // menu and every sky's answer: an inventory can hold tens of
        // thousands of items.
        std::vector<LLPointer<LLViewerInventoryItem>> sInventorySkies;
        bool sInventorySkiesStale = true;

        class InventorySkiesObserver : public LLInventoryObserver
        {
        public:
            ~InventorySkiesObserver()
            {
                sInventorySkies.clear(); // with the inventory, at logout
                sInventorySkiesStale = true;
            }

            void changed(U32) override
            {
                sInventorySkiesStale = true;
            }
        };

        std::vector<LLViewerInventoryItem*> inventorySkies()
        {
            std::vector<LLViewerInventoryItem*> skies;
            if (!gInventory.isInventoryUsable())
            {
                return skies;
            }
            static bool observing = false;
            if (!observing)
            {
                gInventory.addObserver(new InventorySkiesObserver); // the model deletes it at logout
                observing = true;
            }
            if (sInventorySkiesStale)
            {
                sInventorySkiesStale = false;
                sInventorySkies.clear();
                LLInventoryModel::cat_array_t cats;
                LLInventoryModel::item_array_t items;
                LLIsType is_settings(LLAssetType::AT_SETTINGS);
                gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, is_settings);
                const LLUUID ours = ownEnvironmentsFolder();
                for (const auto& item : items)
                {
                    if (item && !item->getIsLinkType() && item->getParentUUID() != ours
                        && LLSettingsType::fromInventoryFlags(item->getFlags()) == LLSettingsType::ST_SKY)
                    {
                        sInventorySkies.push_back(item);
                    }
                }
            }
            for (const auto& item : sInventorySkies)
            {
                skies.push_back(item.get());
            }
            return skies;
        }

        void skiesChanged()
        {
            if (sSkiesChangePending)
            {
                return;
            }
            sSkiesChangePending = true;
            doOnIdleOneTime([]()
            {
                sSkiesChangePending = false;
                if (sSkiesChanged)
                {
                    sSkiesChanged();
                }
            });
        }
    }

    void scanInventorySkies(std::function<void()> changed)
    {
        sSkiesChanged = changed;
        for (LLViewerInventoryItem* item : inventorySkies())
        {
            const LLUUID asset = item->getAssetUUID();
            if (asset.isNull() || sSkyIsPBR.count(asset) || sSkyAsked.count(asset))
            {
                continue;
            }
            sSkyAsked.insert(asset);
            LLSettingsVOBase::getSettingsAsset(asset, [](LLUUID asset_id, LLSettingsBase::ptr_t settings, S32, LLExtStat)
            {
                sSkyAsked.erase(asset_id);
                // Not fetched (the grid's asset service failing for a
                // moment): not noted, so the menu asks again next time it
                // opens, rather than leave the sky out for the session.
                if (!settings)
                {
                    return;
                }
                LLSettingsSky::ptr_t sky = std::dynamic_pointer_cast<LLSettingsSky>(settings);
                sSkyIsPBR[asset_id] = sky && sky->getReflectionProbeAmbiance(false) > 0.f;
                skiesChanged();
            });
        }
    }

    std::vector<std::pair<std::string, std::string>> inventoryPBRSkies()
    {
        std::vector<std::pair<std::string, std::string>> skies;
        std::set<LLUUID> listed; // the same sky in two folders is one sky
        for (LLViewerInventoryItem* item : inventorySkies())
        {
            const auto known = sSkyIsPBR.find(item->getAssetUUID());
            if (known != sSkyIsPBR.end() && known->second && listed.insert(item->getAssetUUID()).second)
            {
                skies.emplace_back(item->getName(), INVENTORY_SKY + item->getUUID().asString());
            }
        }
        std::sort(skies.begin(), skies.end(), [](const auto& a, const auto& b)
        {
            return LLStringUtil::compareDict(a.first, b.first) < 0;
        });
        return skies;
    }

    namespace
    {
        // One of the user's skies, shown as Luminous's own are: the sun keeps
        // its direction, its height is the sky's own; the sun sliders move it.
        // A preset gives the sun's place instead (sun_set).
        bool applyInventorySky(const std::string& value, bool sun_set = false, F32 sun_azimuth = 0.f, F32 sun_elevation = 0.f)
        {
            LLViewerInventoryItem* item = gInventory.getItem(LLUUID(value.substr(INVENTORY_SKY.size())));
            if (!item)
            {
                return false;
            }
            LLSettingsVOBase::getSettingsAsset(item->getAssetUUID(), [value, name = item->getName(), sun_set, sun_azimuth, sun_elevation](LLUUID, LLSettingsBase::ptr_t settings, S32, LLExtStat)
            {
                LLSettingsSky::ptr_t sky = std::dynamic_pointer_cast<LLSettingsSky>(settings);
                if (!sky)
                {
                    LL_WARNS("LuminousPhotoMode") << "Sky not loaded: " << name << LL_ENDL;
                    return;
                }
                F32 azimuth = gSavedSettings.getF32("LuminousEnvSunAzimuth");
                F32 elevation = 0.f;
                if (!sOwnSky)
                {
                    if (LLSettingsSky::ptr_t shown = shownSky())
                    {
                        LLVirtualTrackball::getAzimuthAndElevationDeg(shown->getSunRotation(), azimuth, elevation);
                    }
                }
                F32 sky_azimuth = 0.f;
                LLVirtualTrackball::getAzimuthAndElevationDeg(sky->getSunRotation(), sky_azimuth, elevation);
                if (sun_set)
                {
                    azimuth = sun_azimuth;
                    elevation = sun_elevation;
                }
                // the region's water again in place of one a Luminous sky brought
                LLSettingsWater::ptr_t water;
                if (sOwnWater)
                {
                    if (LLSettingsWater::ptr_t region_water = LLEnvironment::instance().getEnvironmentFixedWater(LLEnvironment::ENV_PARCEL, true))
                    {
                        water = region_water->buildClone();
                    }
                    sOwnWater = false;
                }
                sEnvironmentSky = sky;
                sEnvironmentKeys.clear();
                showSky(environmentSkyAt(elevation), azimuth, elevation, water);
                gSavedSettings.setF32("LuminousEnvSunAzimuth", fmodf(azimuth + 360.f, 360.f));
                gSavedSettings.setF32("LuminousEnvSunElevation", elevation);
                gSavedSettings.setString("LuminousEnvTemplate", value);
                LL_INFOS("LuminousPhotoMode") << "Environment from inventory: " << name << ", sun at " << azimuth << " / " << elevation << LL_ENDL;
            });
            return true;
        }
    }

    bool applyEnvironment(const std::string& name)
    {
        if (name.rfind(INVENTORY_SKY, 0) == 0)
        {
            return applyInventorySky(name);
        }
        return applyEnvironmentWithSun(name, nullptr, nullptr);
    }

    bool applyEnvironmentWithSun(const std::string& name, const F32* sun_azimuth, const F32* sun_elevation)
    {
        LLEnvironment& environment = LLEnvironment::instance();
        if (name.empty())
        {
            // the region's (or parcel's) own sky again
            sEnvironmentSky.reset();
            sEnvironmentKeys.clear();
            sOwnSky = false;
            sOwnWater = false;
            environment.clearEnvironment(LLEnvironment::ENV_LOCAL);
            environment.setSelectedEnvironment(LLEnvironment::ENV_PARCEL, LLEnvironment::TRANSITION_INSTANT);
            environment.updateEnvironment(LLEnvironment::TRANSITION_INSTANT);
            gSavedSettings.setString("LuminousEnvTemplate", "");
            followShownSun();
            return true;
        }
        LLSD doc;
        const std::string path = namedFile(environmentsFolder(), name);
        if (LLFile::isfile(path))
        {
            llifstream stream(path.c_str());
            LLSDSerialize::fromXML(doc, stream);
        }
        LLSettingsSky::ptr_t sky = doc.isMap() && doc.has("sky") ? LLSettingsVOSky::buildSky(doc["sky"]) : LLSettingsSky::ptr_t();
        if (!sky)
        {
            LL_WARNS("LuminousPhotoMode") << "Environment not read: " << path << LL_ENDL;
            return false;
        }
        // Keys at sun heights, if the environment's light changes with the
        // sun's height (Blue hour's belongs only at the horizon)
        std::vector<std::pair<F32, LLSettingsSky::ptr_t>> keys;
        for (const LLSD& key : llsd::inArray(doc["keys"]))
        {
            if (LLSettingsSky::ptr_t key_sky = LLSettingsVOSky::buildSky(key["sky"]))
            {
                keys.emplace_back((F32)key["elevation"].asReal(), key_sky);
            }
        }
        std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        // The sun from where it was (the light's side is the user's to
        // keep), at the height the environment was made for.
        F32 azimuth = gSavedSettings.getF32("LuminousEnvSunAzimuth");
        if (!sOwnSky)
        {
            if (LLSettingsSky::ptr_t shown = shownSky())
            {
                F32 elevation = 0.f;
                LLVirtualTrackball::getAzimuthAndElevationDeg(shown->getSunRotation(), azimuth, elevation);
            }
        }
        F32 elevation = doc.has("sun_elevation") ? (F32)doc["sun_elevation"].asReal() : 30.f;
        // a style places the sun for its camera
        if (sun_azimuth)
        {
            azimuth = *sun_azimuth;
        }
        if (sun_elevation)
        {
            elevation = *sun_elevation;
        }
        // Its own water, if it has one; if not, the region's again in place
        // of one an earlier environment brought, else whatever water shows.
        LLSettingsWater::ptr_t water;
        if (doc.has("water") && doc["water"].isMap())
        {
            water = std::make_shared<LLSettingsVOWater>(doc["water"]);
            sOwnWater = true;
        }
        else if (sOwnWater)
        {
            if (LLSettingsWater::ptr_t region_water = environment.getEnvironmentFixedWater(LLEnvironment::ENV_PARCEL, true))
            {
                water = region_water->buildClone();
            }
            sOwnWater = false;
        }
        sEnvironmentSky = sky;
        sEnvironmentKeys = keys;
        showSky(environmentSkyAt(elevation), azimuth, elevation, water);
        gSavedSettings.setF32("LuminousEnvSunAzimuth", fmodf(azimuth + 360.f, 360.f));
        gSavedSettings.setF32("LuminousEnvSunElevation", elevation);
        gSavedSettings.setString("LuminousEnvTemplate", name);
        LL_INFOS("LuminousPhotoMode") << "Environment: " << name << ", sun at " << azimuth << " / " << elevation << LL_ENDL;
        return true;
    }

    void moveEnvironmentSun()
    {
        const F32 elevation = gSavedSettings.getF32("LuminousEnvSunElevation");
        LLSettingsSky::ptr_t base = sEnvironmentSky ? environmentSkyAt(elevation) : shownSky();
        if (!base)
        {
            return;
        }
        // The same sky, its sun elsewhere: Linden empties every reflection
        // probe for any new sky, and until each is drawn again a place is lit
        // by the open sky: the frame goes dark, then bright, and settles over
        // some five seconds. Kept, they catch up as they update anyway.
        sKeepProbes = true;
        showSky(sEnvironmentSky ? base : base->buildClone(), gSavedSettings.getF32("LuminousEnvSunAzimuth"), elevation);
        sKeepProbes = false;
    }

    // The sky and the sun a preset saved with the Lighting tab; a preset
    // without them leaves them as they are.
    void applyPresetEnvironment(const LLSD& preset)
    {
        if (!preset.has("environment"))
        {
            return;
        }
        const std::string sky = preset["environment"].asString();
        const F32 azimuth = (F32)preset["sun_azimuth"].asReal();
        const F32 elevation = (F32)preset["sun_elevation"].asReal();
        if (preset["sky"].isMap())
        {
            // the sky itself, as it showed (savePreset), sun and moon
            // included; the sun sliders follow it until moved
            if (LLSettingsSky::ptr_t saved = LLSettingsVOSky::buildSky(preset["sky"]))
            {
                saved->update();
                LLEnvironment& environment = LLEnvironment::instance();
                LLSettingsWater::ptr_t water;
                if (sOwnWater)
                {
                    if (LLSettingsWater::ptr_t region_water = environment.getEnvironmentFixedWater(LLEnvironment::ENV_PARCEL, true))
                    {
                        water = region_water->buildClone();
                        water->update();
                    }
                    sOwnWater = false;
                }
                if (water)
                {
                    environment.setEnvironment(LLEnvironment::ENV_LOCAL, saved, water);
                }
                else
                {
                    environment.setEnvironment(LLEnvironment::ENV_LOCAL, saved);
                }
                environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
                environment.updateEnvironment(LLEnvironment::TRANSITION_INSTANT);
                sEnvironmentSky.reset();
                sEnvironmentKeys.clear();
                sOwnSky = false;
                gSavedSettings.setString("LuminousEnvTemplate", std::string());
                followShownSun();
                return;
            }
        }
        if (sky.rfind(INVENTORY_SKY, 0) == 0)
        {
            applyInventorySky(sky, true, azimuth, elevation);
        }
        else if (!sky.empty())
        {
            applyEnvironmentWithSun(sky, &azimuth, &elevation);
        }
        else
        {
            applyEnvironment(std::string()); // the region's sky
            if (preset["sun_moved"].asBoolean())
            {
                gSavedSettings.setF32("LuminousEnvSunAzimuth", azimuth);
                gSavedSettings.setF32("LuminousEnvSunElevation", elevation);
                moveEnvironmentSun();
            }
        }
    }

    bool keepingProbes()
    {
        return sKeepProbes;
    }

    void followShownSun()
    {
        if (sOwnSky)
        {
            return;
        }
        LLSettingsSky::ptr_t sky = shownSky();
        if (!sky)
        {
            return;
        }
        F32 azimuth = 0.f;
        F32 elevation = 0.f;
        LLVirtualTrackball::getAzimuthAndElevationDeg(sky->getSunRotation(), azimuth, elevation);
        azimuth = fmodf(azimuth + 360.f, 360.f);
        if (fabsf(gSavedSettings.getF32("LuminousEnvSunAzimuth") - azimuth) > 0.5f)
        {
            gSavedSettings.setF32("LuminousEnvSunAzimuth", azimuth);
        }
        if (fabsf(gSavedSettings.getF32("LuminousEnvSunElevation") - elevation) > 0.5f)
        {
            gSavedSettings.setF32("LuminousEnvSunElevation", elevation);
        }
    }

    namespace
    {
        std::string stylesFolder()
        {
            return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "luminous_styles");
        }

        // The framings: how much of her the frame holds, from how far round
        // and how high. Around 0 is straight in front of her; pitch above 0
        // looks down at her, below 0 up at her. Only the camera's place and
        // the way it looks: the view angle stays the user's, and the camera
        // is not tilted.
        // `ahead`: she looks the way she faces, not toward the camera --
        // from behind her the camera is where she would have to turn to.
        struct Framing { const char* name; const char* span; F32 around; F32 pitch; bool ahead; };
        // Her face, or her from the hips up: straight on, from below, a
        // little round and above (tilted), from behind a little to one side
        // with her back to the camera, or a little from the side and below.
        const Framing FRAMINGS[] = {
            { "face",   "face",   0.f,   0.f, false },
            { "half",   "half",   0.f,   0.f, false },
            { "low",    "half",   0.f, -15.f, false },
            { "tilted", "half",  20.f,   5.f, false },
            { "behind", "half", -150.f,  3.f, true },
            // a little from the side and a little from below, she looking
            // the way she faces
            { "sidelow", "half",  35.f, -10.f, true },
        };
    }

    std::vector<std::string> styleNames()
    {
        std::vector<std::string> names;
        listNamed(stylesFolder(), names);
        return names;
    }

    namespace
    {
        // The camera a framing set, while it is still the camera: depth of
        // field then focuses on her eyes where they are now, not where they
        // were when she was framed -- a pose or an AO turns her meanwhile,
        // and at a portrait's f-numbers a few centimetres blur the face.
        LLVector3d sFramedFocus;
        bool sFramed = false;

        // A framed camera is watched until it is the user's again -- zoomed,
        // turned, moved, or given back with Esc -- and depth of field then
        // stops following her eyes.
        bool sWatching = false;
        LLVector3d sFramedEye;
        bool sArrived = false;     // the camera has reached the framed place (it glides there)
        F32 sFramedAt = 0.f;

        void letGo()
        {
            if (!sWatching)
            {
                return;
            }
            sWatching = false;
            sFramed = false;
            LL_INFOS("LuminousPhotoMode") << "Framing let go: the camera is the user's" << LL_ENDL;
        }

        void watchFraming(void*)
        {
            if (!sWatching)
            {
                return;
            }
            const F32 off = (F32)(gAgentCamera.getCameraPositionGlobal() - sFramedEye).magVec();
            if (!sArrived)
            {
                // till it gets there, or for at most three seconds
                sArrived = off < 0.03f || (F32)gFrameTimeSeconds - sFramedAt > 3.f;
                if (sArrived && off >= 0.03f)
                {
                    sFramedEye = gAgentCamera.getCameraPositionGlobal();
                }
            }
            else if (off > 0.05f)
            {
                letGo();
                return;
            }
            if (gAgentCamera.getFocusOnAvatar() || (gAgentCamera.getFocusTargetGlobal() - sFramedFocus).magVec() > 0.01)
            {
                letGo();
            }
        }
    }

    bool eyeFocus(LLVector3& focus_agent)
    {
        if (!sFramed)
        {
            return false;
        }
        if (gAgentCamera.getFocusOnAvatar() || (gAgentCamera.getFocusTargetGlobal() - sFramedFocus).magVec() > 0.01
            || !isAgentAvatarValid() || !gAgentAvatarp->mEyeLeftp || !gAgentAvatarp->mEyeRightp
            || !gAgentAvatarp->mHeadp)
        {
            sFramed = false; // the camera has been moved since
            return false;
        }
        // The eye joints are the eyeballs' centres; the iris and the lashes,
        // what has to be sharp, are about a centimetre nearer the camera --
        // at a portrait's distance and f-number more than a full-size
        // render's depth of field.
        static LLCachedControl<F32> surface(gSavedSettings, "LuminousEyeFocusDepth", 0.012f);
        LLVector3 eyes = (gAgentAvatarp->mEyeLeftp->getWorldPosition() + gAgentAvatarp->mEyeRightp->getWorldPosition()) * 0.5f;
        LLVector3 toward = LLViewerCamera::getInstance()->getOrigin() - eyes;
        toward.normVec();
        focus_agent = eyes + toward * surface();
        // Seen from behind the eyes are hidden, and what shows -- her hair,
        // the back of her head -- is up to a head's depth nearer, and would
        // be out of focus. Turned away, the focus moves to the side of her
        // head that faces the camera.
        LLVector3 face = LLVector3::x_axis * gAgentAvatarp->mHeadp->getWorldRotation();
        face.normVec();
        const F32 away = llclamp((0.3f - face * toward) / 0.5f, 0.f, 1.f);
        if (away > 0.f)
        {
            const LLVector3 near_side = eyes - face * 0.08f + toward * 0.09f;
            focus_agent = lerp(focus_agent, near_side, away);
        }
        return true;
    }

    bool frameAvatar(const LLSD& camera, LLVector3* view_direction)
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->mHeadp || !gAgentAvatarp->mPelvisp)
        {
            return false;
        }
        // A named framing, its numbers changed by what the style gives
        Framing framing = FRAMINGS[1];
        const std::string name = camera["framing"].asString();
        for (const Framing& f : FRAMINGS)
        {
            if (name == f.name)
            {
                framing = f;
            }
        }
        const F32 around = camera.has("around") ? (F32)camera["around"].asReal() : framing.around;
        const F32 pitch = camera.has("pitch") ? (F32)camera["pitch"].asReal() : framing.pitch;
        // the user's view angle, to fit the span to
        const F32 fov = llclamp(LLViewerCamera::getInstance()->getDefaultFOV() * RAD_TO_DEG, 10.f, 100.f);

        // Where she is, from her skeleton: the frame's top and bottom for the
        // span, measured from her eyes, and the way she faces as her pose
        // turns her -- her face for a face, her hips for more of her (a pose
        // or an AO often turns her away from the way she walks). The camera
        // looks at the depth of her eyes, which is where depth of field
        // focuses: at f/1.8 a metre off, the head's middle -- ten
        // centimetres behind the face -- leaves the face blurred.
        LLVOAvatarSelf* avatar = gAgentAvatarp;
        const LLVector3 head = avatar->mHeadp->getWorldPosition();
        const LLVector3 pelvis = avatar->mPelvisp->getWorldPosition();
        const std::string span = framing.span;
        LLJoint* facing = span == "face" ? avatar->mHeadp : avatar->mPelvisp;
        LLVector3 forward = LLVector3::x_axis * facing->getWorldRotation();
        forward.mV[VZ] = 0.f;
        if (forward.normVec() < 0.3f) // lying down, or looking straight up
        {
            forward = gAgent.getAtAxis();
            forward.mV[VZ] = 0.f;
            if (forward.normVec() < 0.01f)
            {
                forward = LLVector3::x_axis;
            }
        }
        LLVector3 eyes = head + forward * 0.09f + LLVector3(0.f, 0.f, 0.07f);
        if (avatar->mEyeLeftp && avatar->mEyeRightp)
        {
            eyes = (avatar->mEyeLeftp->getWorldPosition() + avatar->mEyeRightp->getWorldPosition()) * 0.5f;
        }
        F32 top = eyes.mV[VZ] + 0.25f;
        F32 bottom = pelvis.mV[VZ] - 0.1f;
        if (span == "face")
        {
            // head and shoulders, the eyes about a third down
            top = eyes.mV[VZ] + 0.2f;
            bottom = eyes.mV[VZ] - 0.45f;
        }
        LLVector3 centre = eyes;
        centre.mV[VZ] = (top + bottom) * 0.5f;
        // Far enough that the span fills the frame's height
        const F32 distance = ((top - bottom) * 0.5f) / tanf(fov * DEG_TO_RAD * 0.5f);
        LLQuaternion turn;
        turn.setAngleAxis(around * DEG_TO_RAD, LLVector3::z_axis);
        LLVector3 out = forward * turn;
        out = out * cosf(pitch * DEG_TO_RAD) + LLVector3::z_axis * sinf(pitch * DEG_TO_RAD);
        const LLVector3 eye = centre + out * distance;
        // The camera looks at the point of its axis as far from it as her
        // eyes are: depth of field focuses on that depth, and a camera that
        // looks down or up at her would otherwise focus on her middle.
        LLVector3 axis = centre - eye;
        axis.normVec();
        const LLVector3 focus = eye + axis * ((eyes - eye) * axis);

        // As the saved camera positions and Firestorm's camera commands do
        gAgentCamera.unlockView();
        gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(eye), gAgent.getPosGlobalFromAgent(focus), LLUUID::null);
        // That also makes her look at the focus point, a point at her own
        // face, which crosses her eyes and turns her head out of focus. She
        // looks toward the camera instead, half as far up or down as it is:
        // her animation holds the head, and looking the whole way her eyes
        // would roll up to the whites from above and seem closed from below.
        LLVector3 look = eye;
        look.mV[VZ] = eyes.mV[VZ] + (eye.mV[VZ] - eyes.mV[VZ]) * 0.5f;
        if (framing.ahead)
        {
            // the way her face points, at the height of her eyes
            LLVector3 face = LLVector3::x_axis * avatar->mHeadp->getWorldRotation();
            face.mV[VZ] = 0.f;
            if (face.normVec() < 0.3f)
            {
                face = forward;
            }
            look = eyes + face * 10.f;
        }
        gAgentCamera.setLookAt(LOOKAT_TARGET_FOCUS, nullptr, look);
        LLPipeline::FSFocusPointLocked = false; // depth of field focuses on her
        sFramedFocus = gAgentCamera.getFocusTargetGlobal();
        sFramed = true;
        sWatching = true;
        sFramedEye = gAgent.getPosGlobalFromAgent(eye);
        sArrived = false;
        sFramedAt = (F32)gFrameTimeSeconds;
        if (view_direction)
        {
            *view_direction = centre - eye;
        }
        LL_INFOS("LuminousPhotoMode") << "Framed: " << name << ", " << distance << " m, around " << around << ", pitch " << pitch
                                      << ", at the view angle " << fov << LL_ENDL;
        return true;
    }

    bool applyStyle(const std::string& name, bool move_camera)
    {
        LLSD doc;
        const std::string path = namedFile(stylesFolder(), name);
        if (LLFile::isfile(path))
        {
            llifstream stream(path.c_str());
            LLSDSerialize::fromXML(doc, stream);
        }
        if (!doc.isMap())
        {
            LL_WARNS("LuminousPhotoMode") << "Style not read: " << path << LL_ENDL;
            return false;
        }
        // The camera first: the sun is placed for where it looks from.
        LLVector3 view = LLViewerCamera::getInstance()->getAtAxis();
        if (move_camera && doc.has("camera"))
        {
            frameAvatar(doc["camera"], &view);
        }
        if (doc.has("environment"))
        {
            // The sun round from straight behind the camera: 0 lights her
            // from the front, 180 from behind her.
            view.mV[VZ] = 0.f;
            if (view.normVec() < 0.01f)
            {
                view = LLVector3::x_axis;
            }
            LLQuaternion turn;
            turn.setAngleAxis((F32)doc["sun_offset"].asReal() * DEG_TO_RAD, LLVector3::z_axis);
            const LLVector3 sun = (-view) * turn;
            // Personal Lighting's azimuth: 0 east, 90 south (along x, then -y)
            F32 azimuth = atan2f(-sun.mV[VY], sun.mV[VX]) * RAD_TO_DEG;
            azimuth = fmodf(azimuth + 360.f, 360.f);
            F32 elevation = doc.has("sun_elevation") ? (F32)doc["sun_elevation"].asReal() : 0.f;
            applyEnvironmentWithSun(doc["environment"].asString(), &azimuth, doc.has("sun_elevation") ? &elevation : nullptr);
        }
        if (doc.has("settings"))
        {
            applySettings(doc["settings"]);
        }
        gSavedSettings.setString("LuminousStyle", name);
        LL_INFOS("LuminousPhotoMode") << "Style applied: " << name << LL_ENDL;
        return true;
    }

    bool savePreset(const std::string& name)
    {
        if (name.empty())
        {
            return false;
        }
        LLSD preset = llsd::map("name", name);
        for (const char* setting : PRESET_SETTINGS)
        {
            if (LLControlVariable* control = gSavedSettings.getControl(setting))
            {
                preset[setting] = control->getValue();
            }
        }
        // The sky and the sun as the Lighting tab shows them, so that the
        // preset loads as it looked when saved
        preset["environment"] = gSavedSettings.getString("LuminousEnvTemplate");
        preset["sun_azimuth"] = gSavedSettings.getF32("LuminousEnvSunAzimuth");
        preset["sun_elevation"] = gSavedSettings.getF32("LuminousEnvSunElevation");
        preset["sun_moved"] = sOwnSky; // the region's sky with its sun moved
        // Any sky the menu does not name goes in whole, as it shows: a
        // personal sky Second Life kept from the last session (Luminous's
        // choice is not kept across a restart) or set elsewhere, which by
        // name would load as the region's sky; and the region's own, its
        // sun moved or not, since the region's day goes on and would bring
        // another sky, sun and moon by the time the preset loads.
        LLEnvironment& environment = LLEnvironment::instance();
        if (gSavedSettings.getString("LuminousEnvTemplate").empty()
            && environment.getCurrentEnvironmentInstance())
        {
            if (LLSettingsSky::ptr_t sky = environment.getCurrentSky())
            {
                preset["sky"] = sky->getSettings();
            }
        }
        const std::string folder = presetsFolder();
        if (!LLFile::isdir(folder))
        {
            LLFile::mkdir(folder);
        }
        llofstream stream(namedFile(folder, name).c_str());
        if (!stream.is_open())
        {
            return false;
        }
        LLSDSerialize::toPrettyXML(preset, stream);
        return true;
    }

    bool deletePreset(const std::string& name)
    {
        const std::string path = namedFile(presetsFolder(), name);
        return LLFile::isfile(path) && LLFile::remove(path) == 0;
    }

    bool ambientOcclusion()
    {
        static LLCachedControl<bool> ao(gSavedSettings, "LuminousPhotoAO", true);
        return isOn() && ao;
    }

    F32 aoSoften()
    {
        static LLCachedControl<F32> soften(gSavedSettings, "LuminousPhotoAOSoften", 2.f);
        return llclamp((F32)soften(), 0.05f, 3.f);
    }

    bool ambientOcclusionNow()
    {
        return ambientOcclusion() && drawnNow(Kept::AmbientOcclusion);
    }

    namespace
    {
        // GTAO drawn at half size this frame: live, with its low quality on,
        // in the sun pass's AO (Firestorm's ambient occlusion on), both
        // programs built.
        bool gtaoHalfNow()
        {
            return ambientOcclusionNow() && !fullQuality(LiveQuality::AO) && LLPipeline::RenderDeferredSSAO
                && !gCubeSnapshot && gLuminousGTAOProgram.isComplete() && gLuminousGTAOUpProgram.isComplete();
        }
    }

    void bindAOUniforms(LLGLSLShader& shader)
    {
        bindFrame(shader);
        // Kept for renders, the live preview has Linden's ambient occlusion;
        // 2 when GTAO is drawn at half size after the sun pass instead.
        static LLStaticHashedString s_gtao("luminous_gtao");
        shader.uniform1i(s_gtao, ambientOcclusionNow() ? (gtaoHalfNow() ? 2 : 1) : 0);
        static LLCachedControl<F32> radius(gSavedSettings, "LuminousPhotoAORadius", 0.395f);
        static LLCachedControl<F32> strength(gSavedSettings, "LuminousPhotoAOStrength", 1.1f);
        static LLStaticHashedString s_radius("luminous_ao_radius");
        static LLStaticHashedString s_strength("luminous_ao_strength");
        static LLStaticHashedString s_slices("luminous_ao_slices");
        static LLStaticHashedString s_steps("luminous_ao_steps");
        shader.uniform1i(s_slices, 6);
        shader.uniform1i(s_steps, 8);
        shader.uniform1f(s_radius, llclamp(radius(), 0.05f, 4.f));
        shader.uniform1f(s_strength, llclamp(strength(), 0.f, 4.f));
    }

    void bindAOOnLights(LLGLSLShader& shader)
    {
        // Ambient occlusion darkens only ambient light, so in a studio lit
        // by projectors without a sun it shows nothing at all. Not a
        // camera's way, so an option: the lamps take this share of it. The
        // sun pass's AO is in the light map's green; Linden's AO (the live
        // stand-in when GTAO is kept for renders) is left to ambient light.
        static LLCachedControl<F32> share(gSavedSettings, "LuminousPhotoAOLights", 0.f);
        static LLStaticHashedString s_share("luminous_ao_lights");
        const F32 value = share() > 0.f && LLPipeline::RenderDeferredSSAO && ambientOcclusionNow()
                              ? llclamp((F32)share(), 0.f, 1.f) : 0.f;
        shader.uniform1f(s_share, value);
    }

    void renderHalfGTAO(LLRenderTarget* light_target)
    {
        if (!gtaoHalfNow() || !light_target)
        {
            return;
        }
        LL_PROFILE_GPU_ZONE("luminous gtao half");
        // GTAO is the costliest pass in a close-up (22 ms live at 5K):
        // its radius on screen grows as the camera nears, and the samples
        // spread with it. A quarter of the pixels live; Firestorm's softening
        // blurs it after anyway. Pictures draw it at full size in the sun pass.
        const U32 w = llmax(light_target->getWidth() / 2, 1U);
        const U32 h = llmax(light_target->getHeight() / 2, 1U);
        if (sGTAOHalf.getWidth() != w || sGTAOHalf.getHeight() != h)
        {
            sGTAOHalf.allocate(w, h, GL_RG16F);
        }

        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        {
            LLGLDisable blend(GL_BLEND);
            sGTAOHalf.bindTarget();
            gPipeline.bindDeferredShader(gLuminousGTAOProgram);
            drawScreenTriangle();
            gPipeline.unbindDeferredShader(gLuminousGTAOProgram);
            sGTAOHalf.flush();
        }

        // Into the light map's green, which the sun pass left at 1.
        static LLStaticHashedString s_half_size("luminous_half_size");
        LLGLSLShader& up = gLuminousGTAOUpProgram;
        LLGLEnable blend(GL_BLEND);
        gGL.blendFunc(LLRender::BF_ZERO, LLRender::BF_SOURCE_COLOR);
        light_target->bindTarget();
        gGL.setColorMask(true, false);
        up.bind();
        S32 channel = up.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sGTAOHalf, false, LLTexUnit::TFO_POINT);
        if (channel > -1)
        {
            gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        }
        up.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen, true, LLTexUnit::TFO_POINT);
        up.uniform2f(s_half_size, (F32)w, (F32)h);
        drawScreenTriangle();
        up.unbind();
        gGL.setColorMask(true, true);
        light_target->flush();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }

    bool reflections()
    {
        static LLCachedControl<bool> ssr(gSavedSettings, "LuminousPhotoSSR", true);
        static LLCachedControl<bool> firestorm_ssr(gSavedSettings, "RenderScreenSpaceReflections", false);
        return isOn() && ssr && firestorm_ssr;
    }

    bool reflectionsNow()
    {
        return reflections() && drawnNow(Kept::Reflections);
    }

    void bindSSRUniforms(LLGLSLShader& shader)
    {
        static LLStaticHashedString s_steps("luminous_ssr_steps");
        static LLStaticHashedString s_samples("luminous_ssr_samples");
        bindFrame(shader);
        // Kept for renders, the live preview has Firestorm's reflections.
        static LLStaticHashedString s_ssr("luminous_ssr");
        shader.uniform1i(s_ssr, reflectionsNow() ? 1 : 0);
        shader.uniform1i(s_steps, 96); // only in pictures: they are never live
        shader.uniform1i(s_samples, 4);
        static LLStaticHashedString s_scale("luminous_ssr_scale");
        shader.uniform1f(s_scale, liveRenderScale());
    }

    bool depthOfFieldNow()
    {
        return drawnNow(Kept::DepthOfField);
    }

    bool depthOfField()
    {
        static LLCachedControl<bool> dof(gSavedSettings, "LuminousPhotoDoF", true);
        return isOn() && dof;
    }

    bool drawsDepthOfField()
    {
        if (!depthOfField() || !drawnNow(Kept::Enhance) || !drawnNow(Kept::DepthOfField))
        {
            return false; // Firestorm's blur, if its depth of field is on and drawn now
        }
        static const char* last = nullptr;
        bool built = gLuminousDepthOfFieldProgram.isComplete();
        logState("Depth of field", last, built ? "drawing" : "Firestorm's, its shader did not build");
        return built;
    }

    LLRenderTarget* depthOfFieldSource(U32 width, U32 height)
    {
        if (sDoFSource.getWidth() != width || sDoFSource.getHeight() != height)
        {
            sDoFSource.allocate(width, height, GL_RGBA16F, false, LLTexUnit::TT_TEXTURE, LLTexUnit::TMG_AUTO);
        }
        return &sDoFSource;
    }

    void renderDepthOfField(LLRenderTarget* cof, LLRenderTarget* hdr_screen, F32 max_cof)
    {
        LL_PROFILE_GPU_ZONE("luminous depth of field");

        static LLCachedControl<U32> max_samples(gSavedSettings, "LuminousPhotoDoFSamples", 1024);
        static LLCachedControl<bool> front(gSavedSettings, "LuminousPhotoDoFFront", false);
        static LLCachedControl<U32> blades(gSavedSettings, "LuminousPhotoDoFBlades", 0);
        static LLStaticHashedString s_samples("luminous_dof_samples");
        static LLStaticHashedString s_lod("luminous_dof_lod");
        static LLStaticHashedString s_budget("luminous_dof_budget");
        static LLStaticHashedString s_tile("luminous_dof_tile");
        static LLStaticHashedString s_pass("luminous_dof_pass");
        static LLStaticHashedString s_reach("luminous_dof_reach");
        static LLStaticHashedString s_front("luminous_dof_front");
        static LLStaticHashedString s_blades("luminous_dof_blades");

        const U32 width = cof->getWidth();
        const U32 height = cof->getHeight();
        // Drawn straight into sDoFSource by renderDoF (its flush made the mip
        // levels); a copy only for another target, as the copy's flush does.
        if (cof != depthOfFieldSource(width, height))
        {
            copyFrame(cof, &sDoFSource);
        }

        LLGLDisable blend(GL_BLEND);

        // One sample a pixel apart over the largest disc, up to the most
        // allowed; past that they spread out, and read a coarser mip level to
        // match. The whole spiral, as a picture takes it.
        const F32 radius = llmax(max_cof, 1.f);
        const S32 most = (S32)llclamp((U32)max_samples(), 16U, 4096U);
        const S32 samples = llclamp((S32)(F_PI * radius * radius), 16, most);
        const F32 spacing = radius * sqrtf(F_PI / (F32)samples);
        const F32 lod = llmax(0.f, logf(spacing) / logf(2.f));
        // Every pixel may take the whole spiral, live as in a picture: fewer
        // while the shot is set up would save only about 1 ms a frame on an
        // M4 Mac.
        const S32 budget = samples;

        // The tiles, so that each pixel stops at the last sample a blur near
        // it can reach: in focus, none. Without them, the whole spiral.
        S32 tile = 0;
        if (gLuminousDoFTileProgram.isComplete())
        {
            const U32 tiles_x = (width + DOF_TILE - 1) / DOF_TILE;
            const U32 tiles_y = (height + DOF_TILE - 1) / DOF_TILE;
            bool ready = sDoFTiles.getWidth() == tiles_x && sDoFTiles.getHeight() == tiles_y
                         && sDoFReach.getWidth() == tiles_x && sDoFReach.getHeight() == tiles_y;
            if (!ready)
            {
                ready = sDoFTiles.allocate(tiles_x, tiles_y, GL_R16F) && sDoFReach.allocate(tiles_x, tiles_y, GL_R16F);
                if (!ready)
                {
                    sDoFTiles.release();
                    sDoFReach.release();
                }
            }
            if (ready)
            {
                LLGLSLShader& tiles = gLuminousDoFTileProgram;
                tiles.bind();
                tiles.uniform1f(LLShaderMgr::DOF_MAX_COF, max_cof);
                tiles.uniform1f(s_front, front ? 1.f : 0.f);
                tiles.uniform1i(s_tile, (S32)DOF_TILE);
                // A sample lies up to max_cof pixels away and reads a mip level
                // whose texels, filtered, span up to 2^(lod + 1) pixels round it:
                // so a blur can reach from this many tiles away.
                const F32 margin = llmax(max_cof, 0.f) + powf(2.f, ceilf(lod) + 1.f) + 1.f;
                tiles.uniform1i(s_reach, (S32)ceilf(margin / (F32)DOF_TILE));

                tiles.uniform1i(s_pass, 0);
                sDoFTiles.bindTarget();
                tiles.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sDoFSource, false, LLTexUnit::TFO_POINT);
                drawScreenTriangle();
                sDoFTiles.flush();

                tiles.uniform1i(s_pass, 1);
                sDoFReach.bindTarget();
                tiles.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sDoFTiles, false, LLTexUnit::TFO_POINT);
                drawScreenTriangle();
                sDoFReach.flush();
                tiles.unbind();
                tile = (S32)DOF_TILE;
            }
        }

        // Writes colour only: hdr_screen's alpha is left as it was.
        hdr_screen->bindTarget();
        gGL.setColorMask(true, false);

        LLGLSLShader& shader = gLuminousDepthOfFieldProgram;
        shader.bind();
        shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sDoFSource, false, LLTexUnit::TFO_TRILINEAR);
        if (tile)
        {
            shader.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &sDoFReach, false, LLTexUnit::TFO_POINT);
        }
        shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
        shader.uniform1f(LLShaderMgr::DOF_MAX_COF, max_cof);
        shader.uniform1i(s_samples, samples);
        shader.uniform1f(s_lod, lod);
        shader.uniform1i(s_budget, budget);
        shader.uniform1i(s_tile, tile);
        shader.uniform1f(s_front, front ? 1.f : 0.f);
        shader.uniform1f(s_blades, (F32)llmin((U32)blades(), 16U));
        drawScreenTriangle();
        shader.unbind();

        gGL.setColorMask(true, true);
        hdr_screen->flush();
    }

    void render(LLRenderTarget* hdr_screen)
    {
        LLVector2 sun_uv;
        F32 fade = 0.f;
        bool shafts = sunShafts() && drawnNow(Kept::SunShafts) && takeSunShaftMask(hdr_screen, sun_uv, fade);
        LLRenderTarget* shaft_mask = shafts ? softenSunShaftMask() : nullptr;
        if (volumetricLightNow() || projectorBeamsNow())
        {
            storeFrameDepth();
        }
        if (volumetricLightNow())
        {
            renderVolumetricLight(hdr_screen);
        }
        if (projectorBeamsNow())
        {
            renderProjectorBeams(hdr_screen);
        }
        if (shafts)
        {
            addSunShafts(hdr_screen, shaft_mask, sun_uv, fade);
        }
    }
}

namespace LuminousPhotoMode
{
    namespace
    {
        // Files of an older Luminous, whose photo mode swapped in graphics
        // settings of its own: the first holds the user's set, kept aside
        // beside settings.xml, the second photo mode's. A session that
        // ended with the user's set aside gets it back once, and both files
        // go.
        const char* const NORMAL_GRAPHICS_FILE = "luminous_graphics_normal.xml";
        const char* const PHOTO_GRAPHICS_FILE = "luminous_graphics_photo.xml";

        std::string userSettingsFile(const char* name)
        {
            return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, name);
        }

        bool readGraphics(const char* name, LLSD& values)
        {
            std::string path = userSettingsFile(name);
            if (!LLFile::isfile(path))
            {
                return false;
            }
            llifstream file(path.c_str());
            values = LLSD();
            return LLSDSerialize::fromXML(values, file) > 0 && values.isMap();
        }

        // Only what differs is set, so that nothing reloads needlessly.
        void applyGraphics(const LLSD& values)
        {
            for (LLSD::map_const_iterator it = values.beginMap(); it != values.endMap(); ++it)
            {
                LLControlVariable* control = gSavedSettings.getControl(it->first);
                if (control && !llsd_equals(control->getValue(), it->second))
                {
                    control->setValue(it->second);
                }
            }
        }
    }

    void restoreGraphicsAfterRestart()
    {
        LLSD normal;
        if (readGraphics(NORMAL_GRAPHICS_FILE, normal))
        {
            // Nothing listens to these settings yet, so nothing reloads.
            applyGraphics(normal);
            LLFile::remove(userSettingsFile(NORMAL_GRAPHICS_FILE));
            LL_INFOS("LuminousPhotoMode") << "Graphics: photo mode's old graphics swap had the user's own set aside; it is back" << LL_ENDL;
        }
        LLFile::remove(userSettingsFile(PHOTO_GRAPHICS_FILE));
    }

    void initCommands()
    {
        gIdleCallbacks.addFunction(watchFraming, nullptr);
        // The Lighting tab's environment: the menu (or a name, from LEAP or a
        // gesture; "" the region's sky) and the sun's sliders.
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.Environment",
            [](LLUICtrl* ctrl, const LLSD& param)
            {
                applyEnvironment(param.isString() && !param.asString().empty() ? param.asString()
                                 : ctrl ? ctrl->getValue().asString() : std::string());
            });
        // the region's own sky again (LEAP, a gesture)
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.RegionSky",
            [](LLUICtrl*, const LLSD&) { applyEnvironment(std::string()); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.EnvSun",
            [](LLUICtrl*, const LLSD&) { moveEnvironmentSun(); });
        // Styles: the Save / Load tab's menu and Apply, or a name from LEAP
        // or a gesture; and the camera's framings on their own.
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.ApplyStyle",
            [](LLUICtrl* ctrl, const LLSD& param)
            {
                static LLCachedControl<bool> move_camera(gSavedSettings, "LuminousStyleCamera", true);
                const std::string name = param.isString() && !param.asString().empty() ? param.asString()
                                         : ctrl ? ctrl->getValue().asString() : std::string();
                if (!name.empty())
                {
                    applyStyle(name, move_camera);
                }
            });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.Frame",
            [](LLUICtrl* ctrl, const LLSD& param)
            {
                // a name from LEAP or a gesture, or the Frame me menu's choice
                const std::string framing = param.isString() && !param.asString().empty() ? param.asString()
                                            : ctrl ? ctrl->getValue().asString() : std::string();
                if (!framing.empty())
                {
                    frameAvatar(llsd::map("framing", framing), nullptr);
                }
            });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.ResetLook",
            [](LLUICtrl*, const LLSD&) { resetLook(); });
        // A tab's Defaults button: the settings named in its parameter,
        // comma-separated, back to their defaults.
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.ResetSettings",
            [](LLUICtrl*, const LLSD& names)
            {
                std::vector<std::string> list;
                LLStringUtil::getTokens(names.asString(), list, ",");
                HoldShaderRebuilds hold;
                for (std::string name : list)
                {
                    LLStringUtil::trim(name);
                    if (LLControlVariable* control = gSavedSettings.getControl(name))
                    {
                        control->resetToDefault(true);
                    }
                }
            });
        // Add a .cube by its path, as the Load button does.
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.ImportLUT",
            [](LLUICtrl*, const LLSD& path)
            {
                std::string name;
                std::string error;
                if (importLUT(path.asString(), name, error))
                {
                    gSavedSettings.setString("LuminousPhotoLUT", name);
                }
                else
                {
                    LL_WARNS("LuminousPhotoMode") << "Could not add " << path.asString() << ": " << error << LL_ENDL;
                }
            });
        // A saved shot by name, likewise.
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.GoToShot",
            [](LLUICtrl*, const LLSD& name) { std::string error; goToShot(name.asString(), error); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.SaveShot",
            [](LLUICtrl*, const LLSD& name) { saveShot(name.asString()); });
        // A preset by name, for gestures and scripts as well as the window.
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.ApplyPreset",
            [](LLUICtrl*, const LLSD& name) { applyPreset(name.asString()); });
    }

    namespace
    {
        bool sScenePaused = false;
        bool sPausedClouds = false;  // the clouds were moving: set going again
        F64 sPausedAt = 0.0;         // when the pause began
        F64 sPausedFor = 0.0;        // all the pauses before, in seconds

        // Every avatar's animations at the playback speed, or held while
        // the scene is paused; new avatars take it from the motion controller.
        void applyAnimationSpeed()
        {
            static LLCachedControl<F32> speed(gSavedSettings, "LuminousAnimationSpeed", 1.f);
            const F32 factor = sScenePaused ? 0.f : llclamp((F32)speed(), 0.f, 2.f);
            LLMotionController::setCurrentTimeFactor(factor);
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                character->setAnimTimeFactor(factor);
            }
        }

        void setScenePaused(bool paused)
        {
            if (paused == sScenePaused)
            {
                return;
            }
            sScenePaused = paused;
            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (paused)
            {
                sPausedAt = now;
                sPausedClouds = !LLEnvironment::instance().isCloudScrollPaused();
                if (sPausedClouds)
                {
                    LLEnvironment::instance().pauseCloudScroll();
                }
            }
            else
            {
                sPausedFor += now - sPausedAt;
                if (sPausedClouds)
                {
                    LLEnvironment::instance().resumeCloudScroll();
                    sPausedClouds = false;
                }
            }
            // Objects, flexible prims, texture animations and the like, as
            // Linden's freeze frame for snapshots stops them
            if (gSavedSettings.getBOOL("FreezeTime") != paused)
            {
                gSavedSettings.setBOOL("FreezeTime", paused);
            }
            applyAnimationSpeed();
            LL_INFOS("LuminousPhotoMode") << (paused ? "Scene paused" : "Scene going again") << LL_ENDL;
        }
    }

    bool scenePaused()
    {
        return sScenePaused;
    }

    F64 sceneSeconds()
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        return (sScenePaused ? sPausedAt : now) - sPausedFor;
    }

    void initSceneTools()
    {
        gSavedSettings.getControl("LuminousAnimationSpeed")->getSignal()->connect(
            [](LLControlVariable*, const LLSD&, const LLSD&) { applyAnimationSpeed(); });
        gSavedSettings.getControl("LuminousScenePaused")->getSignal()->connect(
            [](LLControlVariable*, const LLSD& value, const LLSD&) { setScenePaused(value.asBoolean()); });
        // Firestorm's snapshot floater ends its freeze frame by clearing
        // FreezeTime; the pause goes with it, rather than stay half on.
        gSavedSettings.getControl("FreezeTime")->getSignal()->connect(
            [](LLControlVariable*, const LLSD& value, const LLSD&)
            {
                if (!value.asBoolean() && sScenePaused)
                {
                    gSavedSettings.setBOOL("LuminousScenePaused", false);
                }
            });
    }
}

// <Luminous> Declared in pipeline.h: the projectors the deferred pass lit the
// scene with this frame, nearest first, skipping what it skipped -- attached
// lights when those are off, and lights too small, too dim or out of view.
void LLPipeline::getNearbySpotLights(std::vector<LLDrawable*>& lights, U32 max_count) const
{
    lights.clear();
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    for (const Light& light : mNearbyLights)
    {
        if (lights.size() >= max_count)
        {
            break;
        }
        LLDrawable* drawablep = light.drawable;
        LLVOVolume* volume = drawablep ? drawablep->getVOVolume() : nullptr;
        if (!volume || !volume->isLightSpotlight())
        {
            continue;
        }
        if (volume->isAttachment() && !sRenderAttachedLights)
        {
            continue;
        }
        F32 size = volume->getLightRadius() * 1.5f;
        if (size <= 0.001f || volume->getLightLinearColor().magVecSquared() < 0.001f)
        {
            continue;
        }
        LLVector4a center;
        center.load3(drawablep->getPositionAgent().mV);
        LLVector4a extent;
        extent.splat(size);
        if (camera->AABBInFrustumNoFarClip(center, extent) == 0)
        {
            continue;
        }
        lights.push_back(drawablep);
    }
}
// </Luminous>
