// Lens flare, made from the frame's own bright light and from the sun's place, as the translated flare is: ghosts
// mirrored through the centre, a halo, a starburst with as many spikes as the depth of field's blades give, the sun
// drawn from its place, and dirt on the lens lit by a wide blur of the light. luminousFlareF.glsl says why each is
// as it is; the numbers here are its numbers.
//
// The work is split as the translated passes split it, at half size but the last step:
//   luminous_lens_flare_bright  the light bright enough to flare, and, apart, the brightest of it for the starburst,
//                               with the sun's region already taken out (the gather then reads one channel and
//                               needs no test per tap);
//   (the down chain's kernel)   a softer quarter-size copy;
//   (the common downsampler)    the starburst light's mip levels, with Star blur;
//   luminous_lens_flare         ghosts, halo, the sun and the starburst;
//   luminous_lens_flare_fs      added to the frame with the dirt.
// In a frame settled for MetalFX temporal the light is taken where the camera without the jitter saw the frame, with the
// starburst's light in its alpha and the sun's seen share in a 1 x 1 texture; both are averaged with the frames before
// (luminousmetaltone.cpp's settle) and luminous_lens_flare_core then makes the starburst's light from the average. The
// flare, dirt and all, is added where this frame shows each place, so the scaler sees it as jittered as the scene.

#include "lenscommon.h"
#include "luminous/common/fullscreen.h"

namespace luminous_lens
{
    constant int GHOSTS = 6;
    constant float GHOST_PHASE[GHOSTS] = { 0.9, 2.3, 4.0, 5.1, 0.2, 3.3 };
    constant float GHOST_PACE[GHOSTS] = { 1.0, 2.0, 1.0, 3.0, 2.0, 1.0 };
    constant float3 GHOST_TINT_A[GHOSTS] = { float3(1.0, 0.65, 0.35), float3(0.45, 0.75, 1.0), float3(0.75, 1.0, 0.55),
                                             float3(1.0, 0.55, 0.85), float3(0.55, 0.65, 1.0), float3(1.0, 0.85, 0.55) };
    constant float3 GHOST_TINT_B[GHOSTS] = { float3(0.55, 1.0, 0.8), float3(1.0, 0.75, 0.45), float3(0.7, 0.6, 1.0),
                                             float3(0.5, 0.9, 1.0), float3(1.0, 0.7, 0.6), float3(0.8, 1.0, 0.7) };

    /// Between 1.35 and 3.25 of the way across.
    inline float ghostT(constant LuminousLensParams& p, int i)
    {
        return 1.35 + 1.9 * (0.5 + 0.5 * sin(GHOST_PHASE[i] + p.flareVariation * 6.2831853 * GHOST_PACE[i]));
    }

    inline float3 ghostTint(constant LuminousLensParams& p, int i)
    {
        return mix(GHOST_TINT_A[i], GHOST_TINT_B[i], 0.5 - 0.5 * cos(p.flareVariation * 6.2831853 * GHOST_PACE[i]));
    }

    /// Nothing from outside the frame, and less from near its edge.
    inline float flareWindow(constant LuminousLensParams& p, float2 uv)
    {
        const float2 inside = step(float2(0.0), uv) * step(uv, float2(1.0));
        const float r = length((uv - 0.5) * float2(p.aspect, 1.0)) / length(float2(0.5 * p.aspect, 0.5));
        return inside.x * inside.y * pow(max(1.0 - r, 0.0), 1.5);
    }

    inline float3 fringed(texture2d<float> light, float2 uv, float2 dir)
    {
        return float3(light.sample(lensLinearClamp, uv - dir, level(0.0)).r, light.sample(lensLinearClamp, uv, level(0.0)).g,
                      light.sample(lensLinearClamp, uv + dir, level(0.0)).b);
    }

    /// The sun's region, which the drawn flare covers: left out of the copied one. at in the picture's uv.
    inline float awayFromSun(constant LuminousLensParams& p, float2 at)
    {
        if (p.sunWeight <= 0.0)
        {
            return 1.0;
        }
        return smoothstep(0.12, 0.2, length((at - lensToPicture(p, p.sunUV)) * float2(p.aspect, 1.0)));
    }

    /// Distance to the aperture's outline, 1 on it: a polygon of its blades, or a circle.
    inline float aperture(constant LuminousLensParams& p, float2 q)
    {
        if (p.flareBlades < 3)
        {
            return length(q);
        }
        const float sector = 6.2831853 / float(p.flareBlades);
        const float a = atan2(q.y, q.x) - p.flareAngle;
        const float local = (a - sector * floor(a / sector)) - 0.5 * sector; // GLSL's mod
        return length(q) * cos(local) / cos(0.5 * sector);
    }
}

// The light bright enough to flare, with a soft knee, weighted again by how far over it is; apart, the very
// brightest for the starburst, without the sun's region. In the sky a light must be wider than a few pixels.
kernel void luminous_lens_flare_bright(
    constant LuminousLensParams& p [[buffer(LUMINOUS_LENS_BUFFER_PARAMS)]],
    constant LuminousLensFlareConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> frame [[texture(LUMINOUS_LENS_TEXTURE_FRAME)]],
    texture2d<float> exposure [[texture(LUMINOUS_LENS_TEXTURE_EXPOSURE)]],
    depth2d<float> depth [[texture(LUMINOUS_LENS_TEXTURE_DEPTH)]],
    texture2d<float, access::write> bright [[texture(3)]],
    texture2d<float, access::write> core [[texture(4)]],
    texture2d<float, access::write> sunSeen [[texture(5)]],
    uint2 gid [[thread_position_in_grid]])
{
    using namespace luminous_lens;
    if (k.settled != 0 && all(gid == uint2(0)))
    {
        // the sun's seen share, as luminous_lens_flare takes it, where this frame shows the sun's place
        float seen = 0.0;
        const float2 aspect = float2(p.aspect, 1.0);
        for (int i = 0; i < 9; ++i)
        {
            const float2 o = i == 0 ? float2(0.0) : 0.008 * float2(cos(0.785398 * float(i)), sin(0.785398 * float(i))) / aspect;
            seen += step(0.99999, lensDepthAt(depth, p.sunUV + k.jitter + o));
        }
        sunSeen.write(float4(seen / 9.0, 0.0, 0.0, 1.0), gid);
    }
    if (any(gid >= k.halfSize))
    {
        return;
    }
    const float2 uv = (float2(gid) + 0.5) / float2(k.halfSize);
    const float2 at = uv + k.jitter; // where this frame shows the place (settled; else uv)
    const float e = max(lensExposure(exposure), 1e-4);
    float3 s = min(frame.sample(lensLinearClamp, at, level(0.0)).rgb, float3(65000.0));
    float l = dot(s, LENS_LUMA);
    if (lensDepthAt(depth, at) >= 1.0)
    {
        const float2 px = p.flareErode / p.frameSize;
        const float lx = min(dot(frame.sample(lensLinearClamp, at + float2(px.x, 0.0), level(0.0)).rgb, LENS_LUMA),
                             dot(frame.sample(lensLinearClamp, at - float2(px.x, 0.0), level(0.0)).rgb, LENS_LUMA));
        const float ly = min(dot(frame.sample(lensLinearClamp, at + float2(0.0, px.y), level(0.0)).rgb, LENS_LUMA),
                             dot(frame.sample(lensLinearClamp, at - float2(0.0, px.y), level(0.0)).rgb, LENS_LUMA));
        const float eroded = min(l, min(lx, ly));
        s *= eroded / max(l, 1e-4);
        l = eroded;
    }
    const float threshold = p.flareThreshold / e;
    const float knee = threshold * 0.5;
    float soft = clamp(l - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    const float over = max(soft, l - threshold);
    const float share = clamp(over / max(l, 1e-4), 0.0, 1.0);
    const float brightest = max(l - 5.0 / e, 0.0) * e / 5.0;
    const float starburst = brightest * awayFromSun(p, lensToPicture(p, uv));
    bright.write(float4(s * share * share, k.settled != 0 ? starburst : 1.0), gid);
    core.write(float4(starburst), gid);
}

// A settled frame: the starburst's light from the settled light's alpha.
kernel void luminous_lens_flare_core(
    constant LuminousLensFlareConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> bright [[texture(3)]],
    texture2d<float, access::write> core [[texture(4)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (any(gid >= k.halfSize))
    {
        return;
    }
    core.write(float4(max(bright.read(gid).a, 0.0)), gid);
}

// Ghosts, halo and starburst, at half size.
kernel void luminous_lens_flare(
    constant LuminousLensParams& p [[buffer(LUMINOUS_LENS_BUFFER_PARAMS)]],
    constant LuminousLensFlareConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> exposure [[texture(LUMINOUS_LENS_TEXTURE_EXPOSURE)]],
    depth2d<float> depth [[texture(LUMINOUS_LENS_TEXTURE_DEPTH)]],
    texture2d<float> bright [[texture(3)]],
    texture2d<float> soft [[texture(4)]],
    texture2d<float> core [[texture(5)]],
    texture2d<float, access::write> out [[texture(6)]],
    texture2d<float> sunSeen [[texture(7)]],
    uint2 gid [[thread_position_in_grid]])
{
    using namespace luminous_lens;
    if (any(gid >= k.halfSize))
    {
        return;
    }
    const float2 frame_uv = (float2(gid) + 0.5) / float2(k.halfSize);
    const float2 uv = lensToPicture(p, frame_uv); // the picture's measure
    const float to_frame = 1.0 / lensFrameScale(p); // a length, picture to frame
    const float2 sun_uv = lensToPicture(p, p.sunUV);
    const float2 to_centre = float2(0.5) - uv;
    const float2 aspect = float2(p.aspect, 1.0);
    const float e = max(lensExposure(exposure), 1e-4);
    float3 sum = float3(0.0);

    // ---- the sun, drawn from its place ----
    if (p.sunWeight > 0.0)
    {
        float seen = 0.0;
        if (k.settled != 0)
        {
            seen = 9.0 * saturate(sunSeen.read(uint2(0)).r); // settled, from the bright pass
        }
        else
        {
            for (int i = 0; i < 9; ++i)
            {
                const float2 o = i == 0 ? float2(0.0) : 0.008 * float2(cos(0.785398 * float(i)), sin(0.785398 * float(i))) / aspect;
                seen += step(0.99999, lensDepthAt(depth, p.sunUV + o));
            }
        }
        const float sun = seen / 9.0 * p.sunWeight;
        if (sun > 0.0)
        {
            const float3 white = p.sunColour.xyz / e; // its colour at white, after exposure
            const float2 sun_from_centre = sun_uv - float2(0.5);
            const float2 from_sun = (uv - sun_uv) * aspect;

            if (p.flareGhosts > 0.0)
            {
                for (int i = 0; i < GHOSTS; ++i)
                {
                    const float kk = 1.0 / (1.0 - ghostT(p, i)); // below 0: mirrored through the centre
                    const float2 c = float2(0.5) + sun_from_centre * kk;
                    const float r = 0.035 * abs(kk);
                    const float2 q = (uv - c) * aspect;
                    const float3 d = float3(aperture(p, q / (r * 1.04)), aperture(p, q / r), aperture(p, q / (r * 0.96)));
                    const float3 disc = smoothstep(float3(1.0), float3(0.9), d) * (0.3 + 0.7 * smoothstep(float3(0.5), float3(0.97), d));
                    sum += disc * ghostTint(p, i) * white * (0.18 / max(abs(kk), 0.5)) * sun * p.flareGhosts;
                }
            }

            if (p.flareHalo > 0.0)
            {
                const float2 q = (uv - 0.5) * aspect;
                const float dist = length(q);
                const float rho = 0.24 + 0.16 * p.flareVariation;
                const float3 ring = exp(-pow((float3(dist) - rho * float3(1.015, 1.0, 0.985)) / 0.012, float3(2.0)));
                const float2 toward = sun_from_centre * aspect;
                const float facing = dist > 1e-4 && length(toward) > 1e-4 ? max(dot(q / dist, normalize(toward)), 0.0) : 0.0;
                sum += ring * pow(facing, 3.0) * white * 0.3 * sun * p.flareHalo;
            }

            if (p.flareStar > 0.0 && p.flareLines > 0)
            {
                float spikes = 0.0;
                const float spread = 1.0 + 3.0 * p.flareStarBlur;
                for (int l = 0; l < p.flareLines; ++l)
                {
                    const float a = p.flareAngle + 3.14159265 * (float(l) + p.flareVariation) / float(p.flareLines);
                    const float2 dir = float2(cos(a), sin(a));
                    const float across = abs(from_sun.x * dir.y - from_sun.y * dir.x);
                    const float along = abs(dot(from_sun, dir));
                    const float width = (0.003 + along * 0.02) * spread; // soft across, widening outward
                    spikes += exp(-(across * across) / (width * width)) * exp(-along / (p.flareStarLength * 0.2));
                }
                sum += white * spikes * sun * 3.0 * p.flareStar * rsqrt(spread);
            }
        }
    }

    // ---- other bright lights, copied from the frame ----
    if (p.flareGhosts > 0.0)
    {
        float3 ghosts = float3(0.0);
        for (int i = 0; i < GHOSTS; ++i)
        {
            const float t = ghostT(p, i);
            const float2 at = uv + to_centre * t;
            const float size = 1.0 / abs(1.0 - t);
            const float2 fringe = to_centre * 0.006 * size;
            ghosts += fringed(bright, lensToFrame(p, at), fringe * to_frame) * ghostTint(p, i) *
                      (flareWindow(p, at) * awayFromSun(p, at) / (size * size));
        }
        sum += ghosts * 0.5 * p.flareGhosts;
    }

    if (p.flareHalo > 0.0)
    {
        const float2 dir = to_centre * aspect;
        const float len = length(dir);
        if (len > 1e-4)
        {
            const float2 step_to = (dir / len) / aspect * (0.24 + 0.16 * p.flareVariation);
            const float2 at = uv + step_to;
            sum += fringed(soft, lensToFrame(p, at), step_to * 0.05 * to_frame) * float3(0.9, 0.95, 1.0) *
                   flareWindow(p, at) * awayFromSun(p, at) * 0.6 * p.flareHalo;
        }
    }

    // The small lights' spikes, gathered along each line from the brightest light (its sun region already out).
    // Star blur reads coarser levels further out; a level's average keeps the light, and its square root over the
    // plain blur is the brighter line the sun's has.
    if (p.flareStar > 0.0 && p.flareLines > 0 && p.flareStarTaps > 0)
    {
        float star = 0.0;
        const float texels = float(core.get_height()) * to_frame; // a picture height in texels
        const float blur = p.flareStarBlur > 0.0 ? 3.0 * p.flareStarBlur : 0.0;
        const float taps = float(p.flareStarTaps);
        for (int l = 0; l < p.flareLines; ++l)
        {
            const float a = p.flareAngle + 3.14159265 * (float(l) + p.flareVariation) / float(p.flareLines);
            const float2 dir = float2(cos(a) / p.aspect, sin(a)) * (p.flareStarLength / taps);
            for (int i = 1; i <= p.flareStarTaps; ++i)
            {
                const float w = exp(-5.0 * float(i) / taps);
                const float along = p.flareStarLength * float(i) / taps;
                const float lod = blur > 0.0 ? max(log2(blur * (0.003 + along * 0.02) * texels), 0.0) : 0.0;
                const float2 forward = lensToFrame(p, uv + dir * float(i));
                const float2 back = lensToFrame(p, uv - dir * float(i));
                star += (core.sample(lensTrilinearClamp, forward, level(lod)).r +
                         core.sample(lensTrilinearClamp, back, level(lod)).r) * w * exp2(0.5 * lod);
            }
        }
        // a point's light, spread along its spikes: per sample, so more taps do not brighten them
        sum += star * float3(1.0, 0.93, 0.82) * (4.0 / taps) * p.flareStar;
    }

    out.write(float4(sum, 1.0), gid);
}

namespace luminous_lens
{
    inline float2 hash2(float2 q)
    {
        float3 r = fract(float3(q.xyx) * float3(0.1031, 0.1030, 0.0973));
        r += dot(r, r.yzx + 33.33);
        return fract((r.xx + r.yz) * r.zy);
    }

    /// Soft discs on a grid, one per cell at most, kept inside it.
    inline float specks(float2 q, float cells, float keep)
    {
        const float2 g = q * cells;
        const float2 cell = floor(g);
        const float2 h = hash2(cell + cells * 7.0);
        if (h.x > keep)
        {
            return 0.0;
        }
        const float2 centre = 0.25 + 0.5 * hash2(cell + 19.0);
        const float radius = 0.08 + 0.17 * h.y;
        const float d = length(fract(g) - centre);
        return smoothstep(radius, radius * 0.25, d) * (0.4 + 0.6 * h.y);
    }

    inline float dirt(constant LuminousLensParams& p, float2 uv)
    {
        const float2 q = (uv - 0.5) * float2(p.aspect, 1.0); // from the picture's middle, so the same in any shape
        const float d = specks(q, 9.0, 0.35) * 0.8 + specks(q + 0.37, 23.0, 0.25) * 0.6 + specks(q + 0.71, 57.0, 0.2) * 0.35;
        const float smear = 0.5 + 0.5 * sin(q.x * 3.1 + sin(q.y * 2.3) * 1.7) * sin(q.y * 1.7 + q.x * 0.6);
        return d + 0.15 * smear;
    }

    /// A wide blur of the bright light, two rings out to a fifth of the frame: what lights the dirt.
    inline float3 dirtGlow(constant LuminousLensParams& p, texture2d<float> soft, float2 uv)
    {
        float3 s = soft.sample(lensLinearClamp, uv, level(0.0)).rgb * 0.1;
        for (int i = 0; i < 8; ++i)
        {
            const float a = 0.785398 * float(i);
            const float2 dir = float2(cos(a) / p.aspect, sin(a)) / lensFrameScale(p);
            s += soft.sample(lensLinearClamp, uv + dir * 0.09, level(0.0)).rgb * 0.06;
            s += soft.sample(lensLinearClamp, uv + dir * 0.2, level(0.0)).rgb * 0.0525;
        }
        return s;
    }
}

// The flare added to the frame (one, one), with the dirt on the front glass where the bright light falls.
fragment float4 luminous_lens_flare_fs(
    LuminousFullscreenOut in [[stage_in]],
    constant LuminousLensParams& p [[buffer(LUMINOUS_LENS_BUFFER_PARAMS)]],
    constant LuminousLensFlareConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> soft [[texture(4)]],
    texture2d<float> flare [[texture(6)]])
{
    using namespace luminous_lens;
    const float2 uv = in.uv - k.jitter; // settled: the place this frame shows here, as the flare was made
    float3 light = flare.sample(lensLinearClamp, uv, level(0.0)).rgb;
    if (p.flareDirt > 0.0)
    {
        light += dirt(p, lensToPicture(p, uv)) * dirtGlow(p, soft, uv) * p.flareDirt * 4.0;
    }
    return float4(max(light * p.flare, float3(0.0)), 0.0);
}
