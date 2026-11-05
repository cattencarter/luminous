// The light leak (tonemapUtilF.glsl's luminousLightLeak) and Worn photo (luminousWornF.glsl) as the tonemap draws
// them, for MetalFX temporal's present: a frame drawn for the scaler leaves them out, and the present draws them on
// the scaled frame's linear colour. Both are laid out in picture heights on the world view, from a hash, and Worn
// photo's edges are a pixel of the present's wide, so each looks as it does in a frame of any size.

#ifndef LUMINOUS_WORN_LEAK_H
#define LUMINOUS_WORN_LEAK_H

#include <metal_stdlib>
#include "luminous/tone/toneshared.h"

using namespace metal;

namespace luminous_screen
{
constant float3 LUMA = float3(0.2126, 0.7152, 0.0722);
constant float3 WORN_PAPER = float3(0.94, 0.90, 0.81); // aged paper, where the picture has faded
constant float3 WORN_BARE = float3(0.95, 0.93, 0.87);  // paper with the picture worn off
constant float3 WORN_STAIN = float3(0.62, 0.47, 0.30); // a brown stain, multiplied

inline float hash(float2 p)
{
    float3 p3 = fract(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

inline float2 hash2(float2 p)
{
    float3 p3 = fract(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

inline float noise(float2 p)
{
    const float2 i = floor(p);
    float2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + float2(1.0, 0.0)), f.x),
               mix(hash(i + float2(0.0, 1.0)), hash(i + float2(1.0, 1.0)), f.x), f.y);
}

// five octaves, about 0 .. 1
inline float fbm(float2 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i)
    {
        sum += amp * noise(p);
        p = p * 2.03 + float2(17.1, 9.7);
        amp *= 0.5;
    }
    return sum / 0.96875;
}

// The sRGB curve both ways, as the tonemap's own, on colour within 0 .. 1.
inline float3 toSrgb(float3 c)
{
    return select(12.92 * c, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, c >= 0.0031308);
}

inline float3 fromSrgb(float3 c)
{
    return select(c / 12.92, pow((c + 0.055) / 1.055, float3(2.4)), c >= 0.04045);
}

// The picture's measure on the world view: the frame's width over its height, picture heights per frame height, the
// picture's shape, and a point's place in picture heights from its middle.
inline float aspectOf(constant LuminousTemporalScreenConstants& k)
{
    return k.outputSize.x / max(k.outputSize.y, 1.0);
}

inline float frameScaleOf(constant LuminousTemporalScreenConstants& k)
{
    return k.frameScale > 0.0 ? k.frameScale : 1.0;
}

inline float2 inPicture(float2 uv, constant LuminousTemporalScreenConstants& k)
{
    return (uv - 0.5) * frameScaleOf(k) * float2(aspectOf(k), 1.0);
}

// ---- the light leak ------------------------------------------------------------------------------------------------

// a turn of the hue round the grey axis, in radians
inline float3 leakTurn(float3 c, float a)
{
    const float3 axis = float3(0.57735027);
    const float ca = cos(a);
    return c * ca + cross(axis, c) * sin(a) + axis * dot(axis, c) * (1.0 - ca);
}

// Light that reached the film past the camera's seal, from the frame's edge, screened onto the picture. c is linear,
// 0 .. 1, and so is the result.
inline float3 lightLeak(float3 c, float2 uv, constant LuminousTemporalScreenConstants& k)
{
    if (k.leak <= 0.0)
    {
        return c;
    }
    const float aspect = aspectOf(k);
    const float2 half_size = float2(0.5 * (k.pictureAspect > 0.0 ? k.pictureAspect : aspect), 0.5);
    const float2 q = inPicture(uv, k);
    // where the frame's edge meets the chosen direction, and which way is in
    const float angle = k.leakPlace * 6.2831853;
    const float2 dir = float2(cos(angle), sin(angle));
    const float2 hit = half_size / max(abs(dir), float2(1e-4));
    const float2 at = dir * min(hit.x, hit.y);
    const float2 inward = hit.x < hit.y ? float2(-sign(dir.x), 0.0) : float2(0.0, -sign(dir.y));
    const float2 along = float2(-inward.y, inward.x);
    const float size = mix(0.15, 0.9, k.leakSize); // picture heights
    const float hard = clamp(k.leakHardness, 0.0, 1.0);
    const float rainbow = clamp(k.leakRainbow, 0.0, 1.0);
    const float2 o = float2(13.0, 29.0) * k.leakSeed;
    const float2 d = q - at;
    const float depth = max(dot(d, inward), 0.0); // into the picture
    const float side = dot(d, along);             // along the edge
    // how far in it reaches varies along the edge
    const float reach = size * (0.7 + 0.6 * noise(float2(side / size * 1.5, 3.0) + o)) * (1.0 + rainbow);
    // soft: a glow that fades in and along the edge; hard: a band most of the edge's length, fading toward its
    // sharp inner side
    const float in_reach = depth / reach;
    const float side_glow = side / (1.6 * size);
    const float side_band = side / (3.5 * size);
    const float glow = exp(-2.0 * in_reach * in_reach - side_glow * side_glow);
    const float band = (1.0 - smoothstep(reach * mix(0.3, 0.9, hard), reach, depth))
                     * mix(1.0, exp(-1.5 * depth / reach), 0.6) * exp(-side_band * side_band);
    float shape = mix(glow, band, hard);
    // stripes along the edge, and the light uneven over its length
    const float stripes = noise(float2(depth / size * 6.0, side / size * 0.25) + o + 7.0);
    shape *= mix(1.0, 0.5 + 0.9 * smoothstep(0.25, 0.75, stripes), 0.3 + 0.7 * hard);
    shape *= 0.8 + 0.4 * noise(q / size * 2.0 + o + 11.0);
    // brightest where it comes in
    const float in_core = depth / (0.2 * reach);
    const float core = exp(-in_core * in_core);
    const float exposure = shape * k.leak * 1.6 * (1.0 + 2.5 * core);
    // the film's layers: the leak's colour first, the others as it grows
    const float3 colour = k.leakColour.xyz;
    const float3 sens = colour / max(max(colour.r, colour.g), max(colour.b, 1e-3)) * 1.4 + 0.05;
    float3 light = 1.0 - exp(-exposure * sens);
    if (rainbow > 0.0)
    {
        light = clamp(leakTurn(light, rainbow * 3.5 * smoothstep(0.1, 1.0, depth / reach)), 0.0, 1.0);
    }
    float3 s = toSrgb(clamp(c, 0.0, 1.0));
    s = 1.0 - (1.0 - s) * (1.0 - light);
    // and where it is strong, its colour over the picture's, most for a band, least for a glow and a rainbow
    const float l = dot(s, LUMA);
    const float3 tinted = clamp(light * (l / max(dot(light, LUMA), 0.05)), 0.0, 1.0);
    s = mix(s, tinted, clamp(exposure * 0.6, 0.0, 1.0) * mix(0.3, 0.75, hard) * (1.0 - 0.5 * rainbow));
    return fromSrgb(s);
}

// ---- Worn photo ----------------------------------------------------------------------------------------------------

// Specks on a grid of cells, at most one in each: its place, size, depth and whether it is light (dust on the
// negative) or dark (on the print) from the cell's hash. How much of a light (x) and a dark (y) speck covers this
// point. Sizes in picture heights; px is one pixel.
inline float2 wornSpecks(float2 q, float cells, float chance, float r_min, float r_max, float light_share, float2 o,
                         float px)
{
    const float2 p = q * cells;
    const float2 cell = floor(p);
    float2 cover = float2(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const float2 id = cell + float2(float(x), float(y)) + o;
            if (hash(id + 71.3) >= chance)
            {
                continue;
            }
            const float2 centre = cell + float2(float(x), float(y)) + hash2(id);
            const float size = hash(id + 13.1);
            float r = mix(r_min, r_max, size * size * size); // most of them small
            // a ragged outline, not a perfect disc
            const float2 to = (p - centre) / cells;
            const float angle = precise::atan2(to.y, to.x); // as the translated GLSL's atan: the fast one moves outlines
            r *= 0.8 + 0.4 * noise(float2(angle * 1.6, 0.0) + id * 3.7);
            const bool light = hash(id + 5.7) < light_share;
            r *= light ? 1.0 : 0.6; // the dark ones smaller
            const float drawn = max(r, 0.7 * px);
            float a = (1.0 - smoothstep(drawn - px, drawn + px, length(to))) * (r / drawn);
            a *= mix(0.45, 1.0, hash(id + 29.9)) * (light ? 1.0 : 0.7);
            if (light)
            {
                cover.x = max(cover.x, a);
            }
            else
            {
                cover.y = max(cover.y, a);
            }
        }
    }
    return cover;
}

// Scratches: at most one in each cell of a coarse grid, a thin line a little bent, broken here and there and fading
// at its ends; mostly light, where the picture's surface is scraped off. How much of a light (x) and a dark (y)
// scratch covers this point.
inline float2 wornScratches(float2 q, float chance, float2 o, float px)
{
    const float cells = 4.0; // per picture height; a scratch reaches less than two cells from its middle
    const float2 p = q * cells;
    const float2 cell = floor(p);
    float2 cover = float2(0.0);
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            const float2 id = cell + float2(float(x), float(y)) + o + 400.0;
            if (hash(id + 3.3) >= chance)
            {
                continue;
            }
            const float2 centre = (cell + float2(float(x), float(y)) + hash2(id)) / cells;
            const float angle = hash(id + 8.1) * 3.14159265;
            const float2 dir = float2(cos(angle), sin(angle));
            const float2 normal = float2(-dir.y, dir.x);
            const float half_length = mix(0.04, 0.2, hash(id + 17.7));
            const float bend = (hash(id + 23.9) - 0.5) * 0.25;
            const float2 d = q - centre;
            const float t = dot(d, dir) / half_length; // -1 .. 1 along it
            if (abs(t) >= 1.0)
            {
                continue;
            }
            const float across = dot(d, normal) - bend * half_length * (1.0 - t * t);
            const float width = mix(0.0004, 0.0012, hash(id + 31.3)); // 0.4 .. 1.3 pixels of a 1080-line picture
            const float drawn = max(width, 0.7 * px);
            float a = (1.0 - smoothstep(0.5 * drawn, 0.5 * drawn + px, abs(across))) * (width / drawn);
            a *= 1.0 - smoothstep(0.6, 1.0, abs(t));                                 // fading at its ends
            a *= smoothstep(0.25, 0.45, noise(float2(t * 9.0, 0.0) + id * 1.3));    // broken
            a *= mix(0.5, 0.9, hash(id + 41.1));
            if (hash(id + 47.5) < 0.8)
            {
                cover.x = max(cover.x, a);
            }
            else
            {
                cover.y = max(cover.y, a);
            }
        }
    }
    return cover;
}

// Uneven fading and stains, the paper's texture, dust and specks, scratches, and worn edges and corners, on the
// finished picture after the grain. c is linear, 0 .. 1, and so is the result.
inline float3 worn(float3 c, float2 uv, constant LuminousTemporalScreenConstants& k)
{
    if (k.worn <= 0.0)
    {
        return c;
    }
    const float2 res = k.outputSize;
    const float frame_scale = k.frameScale > 0.0 ? k.frameScale : 1.0;
    const float aspect = res.x / max(res.y, 1.0);
    const float2 q = (uv - 0.5) * frame_scale * float2(aspect, 1.0); // picture heights from its middle
    const float px = frame_scale / max(res.y, 1.0);                   // one pixel of the present, in picture heights
    const float2 o = float2(37.0, 17.0) * k.wornSeed;    // the pattern's offset
    const float picture_aspect = k.pictureAspect > 0.0 ? k.pictureAspect : aspect;

    const float dust = k.wornParts.x * k.worn;
    const float scratches = k.wornParts.y * k.worn;
    const float stains = k.wornParts.z * k.worn;
    const float edges = k.wornParts.w * k.worn;
    const float paper = k.wornPaper * k.worn;

    float3 s = toSrgb(clamp(c, 0.0, 1.0));

    if (stains > 0.0)
    {
        // uneven fading: broad patches gone pale, and more toward the edges
        const float f = fbm(q * 2.2 + o);
        const float r = length(q / float2(0.5 * picture_aspect, 0.5)); // 0 in the middle, 1 at the edges' middles
        const float fade = smoothstep(0.45, 0.85, f) * 0.55 + smoothstep(0.6, 1.4, r) * 0.25;
        s = mix(s, WORN_PAPER, clamp(fade * stains, 0.0, 0.8));
        // stains: blotches, darker at the rim where a dried drop leaves its line; more of them the stronger the
        // setting
        const float b = fbm(q * 3.1 + o + 40.0);
        const float edge = mix(0.72, 0.6, stains);
        const float inside = smoothstep(edge, edge + 0.04, b);
        const float rim = smoothstep(edge - 0.02, edge + 0.02, b) * (1.0 - smoothstep(edge + 0.02, edge + 0.08, b));
        s *= mix(float3(1.0), WORN_STAIN, clamp(stains * (0.3 * inside + 0.45 * rim), 0.0, 1.0));
        // foxing: small brown spots
        const float2 fox = wornSpecks(q, 30.0, 0.08 * stains, 0.0025, 0.008, 0.0, o + 900.0, px);
        s *= mix(float3(1.0), WORN_STAIN, 0.7 * fox.y);
    }

    if (paper > 0.0)
    {
        // the paper's grain and fibres, seen most in the light parts
        const float n = 0.5 * noise(q * 650.0 + o) + 0.5 * noise(q * float2(180.0, 420.0) + o + 7.0);
        const float l = dot(s, LUMA);
        s *= 1.0 + (n - 0.5) * paper * 0.14 * (0.3 + 0.7 * l);
    }

    if (dust > 0.0)
    {
        // fine dust everywhere, and now and then a larger speck
        const float2 fine = wornSpecks(q, 60.0, 0.2 * dust, 0.0006, 0.003, 0.85, o + 200.0, px);
        const float2 coarse = wornSpecks(q, 10.0, 0.08 * dust, 0.002, 0.007, 0.9, o + 300.0, px);
        const float2 cover = max(fine, coarse);
        s = mix(s, WORN_BARE, cover.x);
        s = mix(s, float3(0.08, 0.07, 0.06), cover.y);
    }

    if (scratches > 0.0)
    {
        const float2 cover = wornScratches(q, 0.35 * scratches, o, px);
        s = mix(s, WORN_BARE, cover.x);
        s = mix(s, float3(0.12, 0.1, 0.08), cover.y);
    }

    if (edges > 0.0)
    {
        // the picture worn off at the edges and, more, the rounded corners: the paper shows, along a ragged line and
        // in flecks near it
        const float corner = 0.03 * edges;
        const float2 half_size = float2(0.5 * picture_aspect, 0.5);
        const float2 dq = abs(q) - (half_size - corner);
        // from the rounded edge, inward
        const float inside = -(length(max(dq, 0.0)) + min(max(dq.x, dq.y), 0.0) - corner);
        // worn in stretches, hardly at all between, and most at the corners
        const float stretch = smoothstep(0.35, 0.75, noise(q * 4.0 + o + 90.0));
        const float at_corner = 1.0 - smoothstep(0.0, 0.2, length(half_size - abs(q)));
        const float band = (0.003 + 0.012 * edges) * (0.15 + 0.85 * stretch + 1.5 * at_corner);
        if (inside < band * 3.0)
        {
            const float ragged = (fbm(q * 70.0 + o + 60.0) - 0.5) * band * 1.5;
            float wear = 1.0 - smoothstep(0.0, band, inside + ragged);
            const float fleck = smoothstep(0.68, 0.74, noise(q * 260.0 + o + 80.0));
            wear = max(wear, fleck * (1.0 - smoothstep(band, band * 3.0, inside)) * 0.7);
            s = mix(s, WORN_BARE, clamp(wear, 0.0, 1.0) * 0.9);
        }
    }

    return fromSrgb(clamp(s, 0.0, 1.0));
}
}

#endif // LUMINOUS_WORN_LEAK_H
