/**
 * @file luminousWornF.glsl
 * @brief Photo mode: the finished picture made to look like an old, handled print.
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

// Worn photo: uneven fading and stains, the paper's texture, dust and
// specks, scratches, and worn edges and corners, on the finished picture
// after the grain. Laid out on the picture -- its height is 1 -- from a
// hash, so it is the same live and in every frame of a render, at any size,
// and lies on the picture, not on a render's margin. Linked into the
// tonemap programs and the sharpening pass; whichever comes last draws it
// (LuminousPhotoMode::bindWornUniforms), the other has luminous_worn 0.

uniform float luminous_worn;         // 0 .. 1, the whole effect; 0 draws nothing
uniform vec4 luminous_worn_parts;    // dust, scratches, stains, edges, each 0 .. 1
uniform float luminous_worn_paper;   // the paper's texture, 0 .. 1
uniform float luminous_worn_seed;    // which pattern: a whole number
uniform float luminous_frame_scale;  // the frame this many picture heights tall: a render's margin, or a shape chosen live
uniform float luminous_picture_aspect; // the picture's width over height; 0: the frame's (a shape chosen on the Render tab, live)

const vec3 LUMINOUS_WORN_PAPER = vec3(0.94, 0.90, 0.81); // aged paper, where the picture has faded
const vec3 LUMINOUS_WORN_BARE = vec3(0.95, 0.93, 0.87);  // paper with the picture worn off
const vec3 LUMINOUS_WORN_STAIN = vec3(0.62, 0.47, 0.30); // a brown stain, multiplied

float luminousWornHash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 luminousWornHash2(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float luminousWornNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(luminousWornHash(i), luminousWornHash(i + vec2(1.0, 0.0)), f.x),
               mix(luminousWornHash(i + vec2(0.0, 1.0)), luminousWornHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// five octaves, about 0 .. 1
float luminousWornFbm(vec2 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i)
    {
        sum += amp * luminousWornNoise(p);
        p = p * 2.03 + vec2(17.1, 9.7);
        amp *= 0.5;
    }
    return sum / 0.96875;
}

vec3 luminousWornToSrgb(vec3 c)
{
    return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 luminousWornFromSrgb(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// Specks on a grid of cells, at most one in each: its place, size, depth
// and whether it is light (dust on the negative) or dark (on the print)
// from the cell's hash. How much of a light (x) and a dark (y) speck covers
// this point. Sizes in picture heights; px is one pixel.
vec2 luminousWornSpecks(vec2 q, float cells, float chance, float r_min, float r_max, float light_share, vec2 o, float px)
{
    vec2 p = q * cells;
    vec2 cell = floor(p);
    vec2 cover = vec2(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 id = cell + vec2(float(x), float(y)) + o;
            if (luminousWornHash(id + 71.3) >= chance)
            {
                continue;
            }
            vec2 centre = cell + vec2(float(x), float(y)) + luminousWornHash2(id);
            float size = luminousWornHash(id + 13.1);
            float r = mix(r_min, r_max, size * size * size); // most of them small
            // a ragged outline, not a perfect disc
            vec2 to = (p - centre) / cells;
            float angle = atan(to.y, to.x);
            r *= 0.8 + 0.4 * luminousWornNoise(vec2(angle * 1.6, 0.0) + id * 3.7);
            bool light = luminousWornHash(id + 5.7) < light_share;
            r *= light ? 1.0 : 0.6; // the dark ones smaller
            float drawn = max(r, 0.7 * px);
            float a = (1.0 - smoothstep(drawn - px, drawn + px, length(to))) * (r / drawn);
            a *= mix(0.45, 1.0, luminousWornHash(id + 29.9)) * (light ? 1.0 : 0.7);
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

// Scratches: at most one in each cell of a coarse grid, a thin line a
// little bent, broken here and there and fading at its ends; mostly light,
// where the picture's surface is scraped off. How much of a light (x) and a
// dark (y) scratch covers this point.
vec2 luminousWornScratches(vec2 q, float chance, vec2 o, float px)
{
    const float cells = 4.0; // per picture height; a scratch reaches less than two cells from its middle
    vec2 p = q * cells;
    vec2 cell = floor(p);
    vec2 cover = vec2(0.0);
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            vec2 id = cell + vec2(float(x), float(y)) + o + 400.0;
            if (luminousWornHash(id + 3.3) >= chance)
            {
                continue;
            }
            vec2 centre = (cell + vec2(float(x), float(y)) + luminousWornHash2(id)) / cells;
            float angle = luminousWornHash(id + 8.1) * 3.14159265;
            vec2 dir = vec2(cos(angle), sin(angle));
            vec2 normal = vec2(-dir.y, dir.x);
            float half_length = mix(0.04, 0.2, luminousWornHash(id + 17.7));
            float bend = (luminousWornHash(id + 23.9) - 0.5) * 0.25;
            vec2 d = q - centre;
            float t = dot(d, dir) / half_length; // -1 .. 1 along it
            if (abs(t) >= 1.0)
            {
                continue;
            }
            float across = dot(d, normal) - bend * half_length * (1.0 - t * t);
            float width = mix(0.0004, 0.0012, luminousWornHash(id + 31.3)); // 0.4 .. 1.3 pixels of a 1080-line picture
            float drawn = max(width, 0.7 * px);
            float a = (1.0 - smoothstep(0.5 * drawn, 0.5 * drawn + px, abs(across))) * (width / drawn);
            a *= 1.0 - smoothstep(0.6, 1.0, abs(t));                                          // fading at its ends
            a *= smoothstep(0.25, 0.45, luminousWornNoise(vec2(t * 9.0, 0.0) + id * 1.3));    // broken
            a *= mix(0.5, 0.9, luminousWornHash(id + 41.1));
            if (luminousWornHash(id + 47.5) < 0.8)
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

// frame_uv: the frame's own 0 .. 1; res: its size in pixels. c is linear,
// 0 .. 1, and so is the result.
vec3 luminousWorn(vec3 c, vec2 frame_uv, vec2 res)
{
    if (luminous_worn <= 0.0)
    {
        return c;
    }
    float frame_scale = luminous_frame_scale > 0.0 ? luminous_frame_scale : 1.0;
    float aspect = res.x / max(res.y, 1.0);
    vec2 q = (frame_uv - 0.5) * frame_scale * vec2(aspect, 1.0); // picture heights from its middle
    float px = frame_scale / max(res.y, 1.0);                     // one pixel, in picture heights
    vec2 o = vec2(37.0, 17.0) * luminous_worn_seed;               // the pattern's offset
    float picture_aspect = luminous_picture_aspect > 0.0 ? luminous_picture_aspect : aspect;

    float dust = luminous_worn_parts.x * luminous_worn;
    float scratches = luminous_worn_parts.y * luminous_worn;
    float stains = luminous_worn_parts.z * luminous_worn;
    float edges = luminous_worn_parts.w * luminous_worn;
    float paper = luminous_worn_paper * luminous_worn;

    vec3 s = luminousWornToSrgb(clamp(c, 0.0, 1.0));

    if (stains > 0.0)
    {
        // uneven fading: broad patches gone pale, and more toward the edges
        float f = luminousWornFbm(q * 2.2 + o);
        float r = length(q / vec2(0.5 * picture_aspect, 0.5)); // 0 in the middle, 1 at the edges' middles
        float fade = smoothstep(0.45, 0.85, f) * 0.55 + smoothstep(0.6, 1.4, r) * 0.25;
        s = mix(s, LUMINOUS_WORN_PAPER, clamp(fade * stains, 0.0, 0.8));
        // stains: blotches, darker at the rim where a dried drop leaves its line;
        // more of them the stronger the setting
        float b = luminousWornFbm(q * 3.1 + o + 40.0);
        float edge = mix(0.72, 0.6, stains);
        float inside = smoothstep(edge, edge + 0.04, b);
        float rim = smoothstep(edge - 0.02, edge + 0.02, b) * (1.0 - smoothstep(edge + 0.02, edge + 0.08, b));
        s *= mix(vec3(1.0), LUMINOUS_WORN_STAIN, clamp(stains * (0.3 * inside + 0.45 * rim), 0.0, 1.0));
        // foxing: small brown spots
        vec2 fox = luminousWornSpecks(q, 30.0, 0.08 * stains, 0.0025, 0.008, 0.0, o + 900.0, px);
        s *= mix(vec3(1.0), LUMINOUS_WORN_STAIN, 0.7 * fox.y);
    }

    if (paper > 0.0)
    {
        // the paper's grain and fibres, seen most in the light parts
        float n = 0.5 * luminousWornNoise(q * 650.0 + o) + 0.5 * luminousWornNoise(q * vec2(180.0, 420.0) + o + 7.0);
        float l = dot(s, vec3(0.2126, 0.7152, 0.0722));
        s *= 1.0 + (n - 0.5) * paper * 0.14 * (0.3 + 0.7 * l);
    }

    if (dust > 0.0)
    {
        // fine dust everywhere, and now and then a larger speck
        vec2 fine = luminousWornSpecks(q, 60.0, 0.2 * dust, 0.0006, 0.003, 0.85, o + 200.0, px);
        vec2 coarse = luminousWornSpecks(q, 10.0, 0.08 * dust, 0.002, 0.007, 0.9, o + 300.0, px);
        vec2 cover = max(fine, coarse);
        s = mix(s, LUMINOUS_WORN_BARE, cover.x);
        s = mix(s, vec3(0.08, 0.07, 0.06), cover.y);
    }

    if (scratches > 0.0)
    {
        vec2 cover = luminousWornScratches(q, 0.35 * scratches, o, px);
        s = mix(s, LUMINOUS_WORN_BARE, cover.x);
        s = mix(s, vec3(0.12, 0.1, 0.08), cover.y);
    }

    if (edges > 0.0)
    {
        // the picture worn off at the edges and, more, the rounded corners:
        // the paper shows, along a ragged line and in flecks near it
        float corner = 0.03 * edges;
        vec2 half_size = vec2(0.5 * picture_aspect, 0.5);
        vec2 dq = abs(q) - (half_size - corner);
        float inside = -(length(max(dq, 0.0)) + min(max(dq.x, dq.y), 0.0) - corner); // from the rounded edge, inward
        // worn in stretches, hardly at all between, and most at the corners
        float stretch = smoothstep(0.35, 0.75, luminousWornNoise(q * 4.0 + o + 90.0));
        float at_corner = 1.0 - smoothstep(0.0, 0.2, length(half_size - abs(q)));
        float band = (0.003 + 0.012 * edges) * (0.15 + 0.85 * stretch + 1.5 * at_corner);
        if (inside < band * 3.0)
        {
            float ragged = (luminousWornFbm(q * 70.0 + o + 60.0) - 0.5) * band * 1.5;
            float wear = 1.0 - smoothstep(0.0, band, inside + ragged);
            float fleck = smoothstep(0.68, 0.74, luminousWornNoise(q * 260.0 + o + 80.0));
            wear = max(wear, fleck * (1.0 - smoothstep(band, band * 3.0, inside)) * 0.7);
            s = mix(s, LUMINOUS_WORN_BARE, clamp(wear, 0.0, 1.0) * 0.9);
        }
    }

    return luminousWornFromSrgb(clamp(s, 0.0, 1.0));
}
