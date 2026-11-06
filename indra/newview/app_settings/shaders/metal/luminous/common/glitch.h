// Glitch's pattern, shared by the lens library's finish pass and the tone library's present: a frame drawn for
// MetalFX temporal leaves Glitch out, and the present draws it on the scaled frame instead. The pattern says where
// the picture is read and how the colours read there are mixed; each caller reads its own picture (the HDR frame
// through the lens's other passes, or the scaled frame through live lens distortion).
//
// Glitch: red and cyan pulled apart; VHS (rows wavering, tracking bands with static, colour smeared sideways); bands
// rippled in a zigzag; scan lines. Laid out in picture heights from a hash, so the same live and in every frame of a
// render, at any size, on the picture rather than a render's margin.

#ifndef LUMINOUS_GLITCH_H
#define LUMINOUS_GLITCH_H

#include <metal_stdlib>
using namespace metal;

constant float3 GLITCH_LUMA = float3(0.2126, 0.7152, 0.0722);

inline float luminousGlitchHash(float2 q)
{
    float3 p3 = fract(float3(q.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

inline float luminousGlitchNoise(float2 q)
{
    const float2 i = floor(q);
    float2 f = fract(q);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(luminousGlitchHash(i), luminousGlitchHash(i + float2(1.0, 0.0)), f.x),
               mix(luminousGlitchHash(i + float2(0.0, 1.0)), luminousGlitchHash(i + float2(1.0, 1.0)), f.x), f.y);
}

/// Where one pixel's Glitch reads the picture, and what it needs to mix the colours read there.
struct LuminousGlitch
{
    float2 toUV;     ///< picture heights to the frame's uv, from the picture's middle
    float2 q;        ///< the pixel, in picture heights from the picture's middle
    float2 at;       ///< where the picture is read, in picture heights
    float2 offset;   ///< the colour split: red read at at + offset, green and blue at at - offset
    float px;        ///< one pixel of the frame, in picture heights
    float seed;
    float vhs;       ///< the VHS part, 0 to 1
    float lines;     ///< the scan lines, 0 to 1
    float tracking;  ///< inside a tracking band, 0 to 1
};

/// The pattern at the frame's uv. aspect is the frame's width over its height and height its pixels, frame_scale the
/// picture heights per frame height, picture_aspect the picture's (0: the frame's own); glitch the whole effect and
/// parts its colour split, VHS, waves and scan lines, each 0 to 1; seed which pattern, a whole number.
inline LuminousGlitch luminousGlitchAt(float2 uv, float aspect, float height, float frame_scale, float picture_aspect,
                                       float glitch, float4 parts, float seed)
{
    LuminousGlitch g;
    g.toUV = 1.0 / (float2(aspect, 1.0) * frame_scale);
    g.q = (uv - 0.5) / g.toUV;
    g.px = frame_scale / max(height, 1.0);
    g.seed = seed * 7.31;
    const float shape = picture_aspect > 0.0 ? picture_aspect : aspect;
    const float2 q = g.q;
    const float s = g.seed;

    const float split = parts.x * glitch;
    g.vhs = parts.y * glitch;
    const float waves = parts.z * glitch;
    g.lines = parts.w * glitch;

    g.at = q;
    g.tracking = 0.0;

    if (g.vhs > 0.0)
    {
        // the rows waver sideways: slowly down the picture, and a little from row to row
        g.at.x += (luminousGlitchNoise(float2(q.y * 14.0, s + 31.0)) - 0.5) * 0.016 * g.vhs
                + (luminousGlitchNoise(float2(q.y * 700.0, s + 32.0)) - 0.5) * 0.003 * g.vhs;
        // the bottom rows skewed, where the tape's heads switch
        const float bottom = 1.0 - smoothstep(0.0, 0.025, q.y + 0.5);
        g.at.x += bottom * bottom * 0.04 * g.vhs;
        // tracking bands, soft-edged: one low in the picture, and a second anywhere at stronger settings
        for (int i = 0; i < 2; ++i)
        {
            const float centre = i == 0 ? -0.45 + 0.2 * luminousGlitchHash(float2(0.0, s + 33.0))
                                        : (luminousGlitchHash(float2(1.0, s + 33.0)) - 0.5) * 0.9;
            const float half_height = 0.01 + 0.04 * luminousGlitchHash(float2(float(i), s + 34.0));
            float band = 1.0 - smoothstep(0.4 * half_height, half_height, abs(q.y - centre));
            if (i == 1 && luminousGlitchHash(float2(1.0, s + 35.0)) > g.vhs)
            {
                band = 0.0;
            }
            g.at.x += (luminousGlitchNoise(float2(q.y * 260.0, s + 36.0 + float(i))) - 0.5) * 0.04 * g.vhs * band;
            g.tracking = max(g.tracking, band);
        }
    }

    if (waves > 0.0)
    {
        // ripples: in some bands, across a stretch of their width, the rows shifted back and forth in a sharp zigzag
        const float row = floor(q.y * 7.0);
        if (luminousGlitchHash(float2(row, s + 11.0)) < 0.35 * waves)
        {
            const float centre = (luminousGlitchHash(float2(row, s + 12.0)) - 0.5) * shape;
            const float half_width = 0.05 + 0.2 * luminousGlitchHash(float2(row, s + 13.0));
            const float across = 1.0 - smoothstep(half_width * 0.7, half_width, abs(q.x - centre));
            const float t = fract(q.y * 7.0);
            const float within = smoothstep(0.1, 0.25, t) * (1.0 - smoothstep(0.75, 0.9, t));
            const float freq = mix(120.0, 360.0, luminousGlitchHash(float2(row, s + 14.0)));
            const float zig = abs(fract(q.y * freq) * 2.0 - 1.0) * 2.0 - 1.0; // a triangle wave, -1 .. 1
            const float amplitude = (0.006 + 0.02 * luminousGlitchHash(float2(row, s + 15.0))) * waves;
            g.at.x += zig * amplitude * across * within;
        }
    }

    // red one way, green and blue the other: from nothing, finely at first (the square of the setting), to 0.018
    // picture heights
    g.offset = float2(1.0, 0.15) * 0.018 * split * split;
    return g;
}

/// The frame's uv for a place in picture heights.
inline float2 luminousGlitchUV(thread const LuminousGlitch& g, float2 at)
{
    return at * g.toUV + 0.5;
}

/// Where the VHS part reads the picture beside the pixel: -1 and 1 the softening's left and right, 1 to 4 the colour
/// smeared from behind the detail (the k-th stretch to its left).
inline float2 luminousGlitchSideUV(thread const LuminousGlitch& g, float side)
{
    return luminousGlitchUV(g, g.at + float2(side * 0.002, 0.0));
}

inline float2 luminousGlitchBehindUV(thread const LuminousGlitch& g, int k)
{
    return luminousGlitchUV(g, g.at - float2(float(k) * 0.005 * g.vhs, 0.0));
}

/// The colour a stretch behind the detail gives the smear: its own less its brightness.
inline float3 luminousGlitchChroma(float3 behind)
{
    return behind - dot(behind, GLITCH_LUMA);
}

/// The VHS part on the split colour: a little soft across (left and right read at the sides), the colour smeared
/// sideways behind the detail (chroma, the sum of the four stretches behind it), and static in the tracking bands.
inline float3 luminousGlitchTape(thread const LuminousGlitch& g, float3 colour, float3 left, float3 right, float3 chroma)
{
    colour = mix(colour, (left + colour + right) / 3.0, 0.7 * g.vhs);
    const float y = dot(colour, GLITCH_LUMA);
    // the smeared colour, a little paler than the picture's
    colour = mix(colour, max(float3(y) + chroma * 0.25 * (1.0 - 0.25 * g.vhs), float3(0.0)), 0.85 * g.vhs);
    // static in the tracking bands: the picture greyed, with short bright streaks along the rows here and there
    if (g.tracking > 0.0)
    {
        const float n = luminousGlitchHash(floor(float2(g.q.x * 90.0, g.q.y / max(g.px, 1e-5))) + g.seed);
        const float streak = smoothstep(0.8, 0.95, n);
        const float grey = dot(colour, GLITCH_LUMA);
        colour = mix(colour, float3(grey * (0.8 + 2.5 * streak)), 0.7 * g.tracking * g.vhs);
    }
    return colour;
}

/// The scan lines: a line every line_size pixels of a 1080-line picture, left out where the frame has too few pixels
/// to draw it without moire.
inline float3 luminousGlitchLines(thread const LuminousGlitch& g, float3 colour, float line_size)
{
    if (g.lines > 0.0)
    {
        const float period = clamp(line_size, 2.0, 8.0) / 1080.0;
        const float shown = 1.0 - smoothstep(0.3, 0.45, g.px / period);
        const float l = 0.5 + 0.5 * cos(6.2831853 * g.q.y / period);
        colour *= 1.0 - g.lines * 0.4 * l * shown;
    }
    return colour;
}

#endif // LUMINOUS_GLITCH_H
