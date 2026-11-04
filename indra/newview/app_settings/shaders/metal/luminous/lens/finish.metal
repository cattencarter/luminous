// The vignette, and the lens passes that move the picture rather than add light: chromatic aberration, distortion
// and glitch, in that order.
//
// The three are one pass as far as their order allows: distortion reads the aberrated picture at one point, so the
// pass aberrates at the point distortion reads; glitch reads the picture up to eight times, so it reads through the
// distortion directly when aberration is off (eight taps each would be 64), and draws a pass of its own after the
// other two when aberration is on. The alpha, Second Life's glow mask, is the pixel's own throughout.

#include "lenscommon.h"
#include "luminous/common/fullscreen.h"

namespace luminous_lens
{
    /// Where distortion reads the frame for uv: positive bows straight lines outward (barrel), negative pulls them in;
    /// the frame scaled so that the picture's corners never read past its edges.
    inline float2 distorted(constant LuminousLensParams& p, float2 uv)
    {
        const float aspect = p.frameSize.x / p.frameSize.y;
        const float2 q = (uv - 0.5) * float2(aspect, 1.0) * p.frameScale; // in the picture's units
        const float k = p.distortion * 0.25;
        const float picture_aspect = p.pictureAspect > 0.0 ? p.pictureAspect : aspect;
        const float corner2 = dot(float2(picture_aspect, 1.0) * 0.5, float2(picture_aspect, 1.0) * 0.5);
        const float fit = k > 0.0 ? 1.0 / (1.0 + k * corner2) : 1.0;
        const float2 r = q * (1.0 + k * dot(q, q)) * fit;
        return r / (float2(aspect, 1.0) * p.frameScale) + 0.5;
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

    inline float glitchHash(float2 q)
    {
        float3 p3 = fract(float3(q.xyx) * 0.1031);
        p3 += dot(p3, p3.yzx + 33.33);
        return fract((p3.x + p3.y) * p3.z);
    }

    inline float glitchNoise(float2 q)
    {
        const float2 i = floor(q);
        float2 f = fract(q);
        f = f * f * (3.0 - 2.0 * f);
        return mix(mix(glitchHash(i), glitchHash(i + float2(1.0, 0.0)), f.x),
                   mix(glitchHash(i + float2(0.0, 1.0)), glitchHash(i + float2(1.0, 1.0)), f.x), f.y);
    }

    /// Glitch: red and cyan pulled apart; VHS (rows wavering, tracking bands with static, colour smeared sideways);
    /// bands rippled in a zigzag; scan lines. Laid out in picture heights from a hash, so the same live and in every
    /// frame of a render, on the picture rather than a render's margin.
    inline float3 glitched(constant LuminousLensParams& p, texture2d<float> frame, float2 uv, uint mode)
    {
        const float frame_scale = lensFrameScale(p);
        const float aspect = p.frameSize.x / max(p.frameSize.y, 1.0);
        const float2 to_uv = 1.0 / (float2(aspect, 1.0) * frame_scale);
        const float2 q = (uv - 0.5) / to_uv;          // picture heights from its middle
        const float px = frame_scale / max(p.frameSize.y, 1.0); // one pixel, in picture heights
        const float seed = p.glitchSeed * 7.31;
        const float picture_aspect = p.pictureAspect > 0.0 ? p.pictureAspect : aspect;

        const float split = p.glitchParts.x * p.glitch;
        const float vhs = p.glitchParts.y * p.glitch;
        const float waves = p.glitchParts.z * p.glitch;
        const float lines = p.glitchParts.w * p.glitch;

        float2 at = q;         // where the picture is read
        float tracking = 0.0;  // inside a tracking band, 0 .. 1

        if (vhs > 0.0)
        {
            at.x += (glitchNoise(float2(q.y * 14.0, seed + 31.0)) - 0.5) * 0.016 * vhs
                  + (glitchNoise(float2(q.y * 700.0, seed + 32.0)) - 0.5) * 0.003 * vhs;
            const float bottom = 1.0 - smoothstep(0.0, 0.025, q.y + 0.5);
            at.x += bottom * bottom * 0.04 * vhs;
            for (int i = 0; i < 2; ++i)
            {
                const float centre = i == 0 ? -0.45 + 0.2 * glitchHash(float2(0.0, seed + 33.0))
                                            : (glitchHash(float2(1.0, seed + 33.0)) - 0.5) * 0.9;
                const float half_height = 0.01 + 0.04 * glitchHash(float2(float(i), seed + 34.0));
                float band = 1.0 - smoothstep(0.4 * half_height, half_height, abs(q.y - centre));
                if (i == 1 && glitchHash(float2(1.0, seed + 35.0)) > vhs)
                {
                    band = 0.0;
                }
                at.x += (glitchNoise(float2(q.y * 260.0, seed + 36.0 + float(i))) - 0.5) * 0.04 * vhs * band;
                tracking = max(tracking, band);
            }
        }

        if (waves > 0.0)
        {
            const float row = floor(q.y * 7.0);
            if (glitchHash(float2(row, seed + 11.0)) < 0.35 * waves)
            {
                const float centre = (glitchHash(float2(row, seed + 12.0)) - 0.5) * picture_aspect;
                const float half_width = 0.05 + 0.2 * glitchHash(float2(row, seed + 13.0));
                const float across = 1.0 - smoothstep(half_width * 0.7, half_width, abs(q.x - centre));
                const float t = fract(q.y * 7.0);
                const float within = smoothstep(0.1, 0.25, t) * (1.0 - smoothstep(0.75, 0.9, t));
                const float freq = mix(120.0, 360.0, glitchHash(float2(row, seed + 14.0)));
                const float zig = abs(fract(q.y * freq) * 2.0 - 1.0) * 2.0 - 1.0; // a triangle wave, -1 .. 1
                const float amplitude = (0.006 + 0.02 * glitchHash(float2(row, seed + 15.0))) * waves;
                at.x += zig * amplitude * across * within;
            }
        }

        const float2 offset = float2(1.0, 0.15) * 0.018 * split * split;
        float3 colour;
        colour.r = lensImage(p, frame, (at + offset) * to_uv + 0.5, mode).r;
        colour.gb = lensImage(p, frame, (at - offset) * to_uv + 0.5, mode).gb;

        if (vhs > 0.0)
        {
            const float3 left = lensImage(p, frame, (at - float2(0.002, 0.0)) * to_uv + 0.5, mode);
            const float3 right = lensImage(p, frame, (at + float2(0.002, 0.0)) * to_uv + 0.5, mode);
            colour = mix(colour, (left + colour + right) / 3.0, 0.7 * vhs);
            float3 chroma = float3(0.0);
            for (int k = 1; k <= 4; ++k)
            {
                const float3 behind = lensImage(p, frame, (at - float2(float(k) * 0.005 * vhs, 0.0)) * to_uv + 0.5, mode);
                chroma += behind - dot(behind, LENS_LUMA);
            }
            const float y = dot(colour, LENS_LUMA);
            colour = mix(colour, max(float3(y) + chroma * 0.25 * (1.0 - 0.25 * vhs), float3(0.0)), 0.85 * vhs);
            if (tracking > 0.0)
            {
                const float n = glitchHash(floor(float2(q.x * 90.0, q.y / max(px, 1e-5))) + seed);
                const float streak = smoothstep(0.8, 0.95, n);
                const float grey = dot(colour, LENS_LUMA);
                colour = mix(colour, float3(grey * (0.8 + 2.5 * streak)), 0.7 * tracking * vhs);
            }
        }

        if (lines > 0.0)
        {
            // a line every so many pixels of a 1080-line picture, left out where the frame has too few pixels
            const float period = clamp(p.glitchLineSize, 2.0, 8.0) / 1080.0;
            const float shown = 1.0 - smoothstep(0.3, 0.45, px / period);
            const float l = 0.5 + 0.5 * cos(6.2831853 * q.y / period);
            colour *= 1.0 - lines * 0.4 * l * shown;
        }
        return colour;
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
