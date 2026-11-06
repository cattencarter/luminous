// The vignette, and the lens passes that move the picture rather than add light: chromatic aberration, distortion
// and glitch, in that order.
//
// The three are one pass as far as their order allows: distortion reads the aberrated picture at one point, so the
// pass aberrates at the point distortion reads; glitch reads the picture up to eight times, so it reads through the
// distortion directly when aberration is off (eight taps each would be 64), and draws a pass of its own after the
// other two when aberration is on. The alpha, Second Life's glow mask, is the pixel's own throughout.

#include "lenscommon.h"
#include "luminous/common/distortion.h"
#include "luminous/common/glitch.h"
#include "luminous/common/fullscreen.h"

namespace luminous_lens
{
    /// Where distortion reads the frame for uv (luminousDistorted, which MetalFX temporal's present shares).
    inline float2 distorted(constant LuminousLensParams& p, float2 uv)
    {
        return luminousDistorted(uv, p.frameSize.x / p.frameSize.y, p.frameScale, p.distortion, p.pictureAspect);
    }

    /// Red and blue parted toward the picture's edges: eight points along the line to the centre, each weighted by
    /// where on the spectrum it lies.
    inline float3 aberrated(constant LuminousLensParams& p, texture2d<float> frame, float2 tc)
    {
        const float2 res = p.frameSize;
        const float2 from_centre = (tc - 0.5) * res;
        const float picture_aspect = p.pictureAspect > 0.0 ? p.pictureAspect : res.x / res.y;
        const float corner = 0.5 * length(float2(picture_aspect, 1.0)) * res.y / p.frameScale; // the picture's corner
        const float2 shift = from_centre / corner * p.aberration / res;
        float3 sum = float3(0.0);
        float3 weight = float3(0.0);
        for (int i = 0; i < 8; ++i)
        {
            const float t = (float(i) + 0.5) / 8.0 * 2.0 - 1.0;
            const float3 w = clamp(float3(1.0 - abs(t - 1.0), 1.0 - abs(t), 1.0 - abs(t + 1.0)), 0.0, 1.0);
            sum += frame.sample(lensLinearClamp, tc + shift * t, level(0.0)).rgb * w;
            weight += w;
        }
        return sum / weight;
    }

    /// The picture aberration and distortion make, those the mode names, at uv.
    inline float3 lensImage(constant LuminousLensParams& p, texture2d<float> frame, float2 uv, uint mode)
    {
        const float2 at = (mode & LUMINOUS_LENS_FINISH_DISTORTION) != 0 ? distorted(p, uv) : uv;
        return (mode & LUMINOUS_LENS_FINISH_ABERRATION) != 0 ? aberrated(p, frame, at)
                                                             : frame.sample(lensLinearClamp, at, level(0.0)).rgb;
    }

    /// Glitch (common/glitch.h) on the picture aberration and distortion make, those the mode names.
    inline float3 glitched(constant LuminousLensParams& p, texture2d<float> frame, float2 uv, uint mode)
    {
        const LuminousGlitch g = luminousGlitchAt(uv, p.frameSize.x / max(p.frameSize.y, 1.0), p.frameSize.y,
                                                  lensFrameScale(p), p.pictureAspect, p.glitch, p.glitchParts,
                                                  p.glitchSeed);
        float3 colour;
        colour.r = lensImage(p, frame, luminousGlitchUV(g, g.at + g.offset), mode).r;
        colour.gb = lensImage(p, frame, luminousGlitchUV(g, g.at - g.offset), mode).gb;
        if (g.vhs > 0.0)
        {
            const float3 left = lensImage(p, frame, luminousGlitchSideUV(g, -1.0), mode);
            const float3 right = lensImage(p, frame, luminousGlitchSideUV(g, 1.0), mode);
            float3 chroma = float3(0.0);
            for (int k = 1; k <= 4; ++k)
            {
                chroma += luminousGlitchChroma(lensImage(p, frame, luminousGlitchBehindUV(g, k), mode));
            }
            colour = luminousGlitchTape(g, colour, left, right, chroma);
        }
        return luminousGlitchLines(g, colour, p.glitchLineSize);
    }
}

// Aberration, distortion and glitch, those the mode names, drawn anew into the other target.
fragment float4 luminous_lens_finish_fs(
    LuminousFullscreenOut in [[stage_in]],
    constant LuminousLensParams& p [[buffer(LUMINOUS_LENS_BUFFER_PARAMS)]],
    constant LuminousLensFinishConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> frame [[texture(LUMINOUS_LENS_TEXTURE_FRAME)]])
{
    using namespace luminous_lens;
    const float alpha = frame.sample(lensLinearClamp, in.uv, level(0.0)).a;
    const float3 c = (k.mode & LUMINOUS_LENS_FINISH_GLITCH) != 0 ? glitched(p, frame, in.uv, k.mode)
                                                                 : lensImage(p, frame, in.uv, k.mode);
    return float4(c, alpha);
}

// A darkening factor only, multiplied onto the HDR frame (the pipeline blends with the destination colour), after
// the exposure is measured and before tonemapping. Distance from the picture's centre, its corner at 1 whatever the
// shape.
fragment float4 luminous_lens_vignette_fs(
    LuminousFullscreenOut in [[stage_in]],
    constant LuminousLensParams& p [[buffer(LUMINOUS_LENS_BUFFER_PARAMS)]])
{
    const float picture_aspect = p.pictureAspect > 0.0 ? p.pictureAspect : p.aspect;
    const float2 d = (in.uv - 0.5) * p.frameScale * float2(p.aspect / picture_aspect, 1.0);
    const float2 scale = float2(mix(1.0, picture_aspect, p.vignetteRoundness), 1.0);
    const float r = length(d * scale) / length(0.5 * scale);
    const float half_width = 0.5 * max(p.vignetteSoftness, 0.02);
    const float dark = smoothstep(p.vignetteSize - half_width, p.vignetteSize + half_width, r);
    const float factor = clamp(1.0 - p.vignetteAmount * dark, 0.0, 1.0);
    return float4(float3(factor), 1.0);
}
