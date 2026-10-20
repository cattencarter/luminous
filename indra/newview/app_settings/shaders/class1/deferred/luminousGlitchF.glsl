/**
 * @file luminousGlitchF.glsl
 * @brief Photo mode: the picture as a broken digital image.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on Second Life Viewer Source Code, Copyright (C) 2007, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

// Glitch: the picture's red and cyan pulled apart; VHS, a worn tape's look
// -- the rows wavering sideways, soft tracking bands where they are thrown
// further and break into static, colour smeared sideways behind the detail
// and a little softness across; bands rippled in a zigzag; and scan lines.
// Laid out in picture heights from a hash, so it is the same live and in
// every frame of a render, at any size, on the picture rather than a
// render's margin. The frame is read where the pattern says; its alpha,
// Second Life's glow mask, is this pixel's.

out vec4 frag_color;

uniform sampler2D diffuseRect;        // the frame, drawn anew into the other target
uniform vec2 screen_res;
uniform float luminous_glitch;        // 0 .. 1, the whole effect
uniform vec4 luminous_glitch_parts;   // colour split, VHS, waves, scan lines, each 0 .. 1
uniform float luminous_glitch_seed;   // which pattern: a whole number
uniform float luminous_glitch_line_size; // a scan line every this many pixels of a 1080-line picture
uniform float luminous_frame_scale;   // the frame this many picture heights tall: a render's margin, or a shape chosen live
uniform float luminous_picture_aspect; // the picture's width over height; 0: the frame's (a shape chosen on the Render tab, live)

in vec2 vary_fragcoord;

const vec3 GLITCH_LUMA = vec3(0.2126, 0.7152, 0.0722);

float glitchHash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float glitchNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(glitchHash(i), glitchHash(i + vec2(1.0, 0.0)), f.x),
               mix(glitchHash(i + vec2(0.0, 1.0)), glitchHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

void main()
{
    float frame_scale = luminous_frame_scale > 0.0 ? luminous_frame_scale : 1.0;
    float aspect = screen_res.x / max(screen_res.y, 1.0);
    vec2 to_uv = 1.0 / (vec2(aspect, 1.0) * frame_scale);
    vec2 q = (vary_fragcoord - 0.5) / to_uv;          // picture heights from its middle
    float px = frame_scale / max(screen_res.y, 1.0);  // one pixel, in picture heights
    float seed = luminous_glitch_seed * 7.31;
    float picture_aspect = luminous_picture_aspect > 0.0 ? luminous_picture_aspect : aspect;

    float split = luminous_glitch_parts.x * luminous_glitch;
    float vhs = luminous_glitch_parts.y * luminous_glitch;
    float waves = luminous_glitch_parts.z * luminous_glitch;
    float lines = luminous_glitch_parts.w * luminous_glitch;

    vec2 p = q;           // where the frame is read
    float tracking = 0.0; // inside a tracking band, 0 .. 1

    if (vhs > 0.0)
    {
        // the rows waver sideways: slowly down the picture, and a little from row to row
        p.x += (glitchNoise(vec2(q.y * 14.0, seed + 31.0)) - 0.5) * 0.016 * vhs
             + (glitchNoise(vec2(q.y * 700.0, seed + 32.0)) - 0.5) * 0.003 * vhs;
        // the bottom rows skewed, where the tape's heads switch
        float bottom = 1.0 - smoothstep(0.0, 0.025, q.y + 0.5);
        p.x += bottom * bottom * 0.04 * vhs;
        // tracking bands, soft-edged: one low in the picture, and a second
        // anywhere at stronger settings
        for (int i = 0; i < 2; ++i)
        {
            float centre = i == 0 ? -0.45 + 0.2 * glitchHash(vec2(0.0, seed + 33.0))
                                  : (glitchHash(vec2(1.0, seed + 33.0)) - 0.5) * 0.9;
            float half_height = 0.01 + 0.04 * glitchHash(vec2(float(i), seed + 34.0));
            float band = 1.0 - smoothstep(0.4 * half_height, half_height, abs(q.y - centre));
            if (i == 1 && glitchHash(vec2(1.0, seed + 35.0)) > vhs)
            {
                band = 0.0;
            }
            p.x += (glitchNoise(vec2(q.y * 260.0, seed + 36.0 + float(i))) - 0.5) * 0.04 * vhs * band;
            tracking = max(tracking, band);
        }
    }

    if (waves > 0.0)
    {
        // ripples: in some bands, across a stretch of their width, the rows
        // shifted back and forth in a sharp zigzag
        float row = floor(q.y * 7.0);
        if (glitchHash(vec2(row, seed + 11.0)) < 0.35 * waves)
        {
            float centre = (glitchHash(vec2(row, seed + 12.0)) - 0.5) * picture_aspect;
            float half_width = 0.05 + 0.2 * glitchHash(vec2(row, seed + 13.0));
            float across = 1.0 - smoothstep(half_width * 0.7, half_width, abs(q.x - centre));
            float t = fract(q.y * 7.0);
            float within = smoothstep(0.1, 0.25, t) * (1.0 - smoothstep(0.75, 0.9, t));
            float freq = mix(120.0, 360.0, glitchHash(vec2(row, seed + 14.0)));
            float zig = abs(fract(q.y * freq) * 2.0 - 1.0) * 2.0 - 1.0; // a triangle wave, -1 .. 1
            float amplitude = (0.006 + 0.02 * glitchHash(vec2(row, seed + 15.0))) * waves;
            p.x += zig * amplitude * across * within;
        }
    }

    // red one way, green and blue the other: from nothing, finely at first
    // (the square of the setting), to 0.018 picture heights
    vec2 offset = vec2(1.0, 0.15) * 0.018 * split * split;
    vec3 colour;
    colour.r = texture(diffuseRect, (p + offset) * to_uv + 0.5).r;
    colour.gb = texture(diffuseRect, (p - offset) * to_uv + 0.5).gb;

    if (vhs > 0.0)
    {
        // a little soft across, and the colour smeared sideways behind the
        // detail: the picture's own brightness, the colour of a stretch to
        // its left
        vec3 left = texture(diffuseRect, (p - vec2(0.002, 0.0)) * to_uv + 0.5).rgb;
        vec3 right = texture(diffuseRect, (p + vec2(0.002, 0.0)) * to_uv + 0.5).rgb;
        colour = mix(colour, (left + colour + right) / 3.0, 0.7 * vhs);
        vec3 chroma = vec3(0.0);
        for (int k = 1; k <= 4; ++k)
        {
            vec3 behind = texture(diffuseRect, (p - vec2(float(k) * 0.005 * vhs, 0.0)) * to_uv + 0.5).rgb;
            chroma += behind - dot(behind, GLITCH_LUMA);
        }
        float y = dot(colour, GLITCH_LUMA);
        // the smeared colour, a little paler than the picture's
        colour = mix(colour, max(vec3(y) + chroma * 0.25 * (1.0 - 0.25 * vhs), vec3(0.0)), 0.85 * vhs);
        // static in the tracking bands: the picture greyed, with short bright
        // streaks along the rows here and there
        if (tracking > 0.0)
        {
            float n = glitchHash(floor(vec2(q.x * 90.0, q.y / max(px, 1e-5))) + seed);
            float streak = smoothstep(0.8, 0.95, n);
            float grey = dot(colour, GLITCH_LUMA);
            colour = mix(colour, vec3(grey * (0.8 + 2.5 * streak)), 0.7 * tracking * vhs);
        }
    }

    if (lines > 0.0)
    {
        // a line every so many pixels of a 1080-line picture, left out where
        // the frame has too few pixels to draw it without moire
        float period = clamp(luminous_glitch_line_size, 2.0, 8.0) / 1080.0;
        float shown = 1.0 - smoothstep(0.3, 0.45, px / period);
        float l = 0.5 + 0.5 * cos(6.2831853 * q.y / period);
        colour *= 1.0 - lines * 0.4 * l * shown;
    }

    frag_color = vec4(colour, texture(diffuseRect, vary_fragcoord).a);
}
