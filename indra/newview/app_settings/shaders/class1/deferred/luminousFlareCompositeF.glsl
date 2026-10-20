/**
 * @file luminousFlareCompositeF.glsl
 * @brief Photo mode: lens flare added to the frame, with the dirt on the lens.
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

// Adds the flare (half size, drawn with additive blending) and the dirt on
// the front glass: specks and smudges that only show where bright light
// falls on the lens, lit by a wide blur of that light. The dirt is made here,
// from a hash, so no texture or licence is borrowed; it stays put on the
// frame, as dirt on a lens does.

out vec4 frag_color;

uniform sampler2D diffuseRect; // the flare, half size
uniform sampler2D bloomMap;    // the bright light, a quarter size and softer
uniform float luminous_aspect;
uniform float luminous_flare;      // the whole flare's strength
uniform float luminous_flare_dirt;
uniform float luminous_frame_scale; // a render's margin: the dirt lies on the picture, as live (luminousFlareF.glsl)

in vec2 vary_fragcoord;

vec2 hash2(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.xx + q.yz) * q.zy);
}

// Soft discs on a grid, one per cell at most, kept inside it.
float specks(vec2 p, float cells, float keep)
{
    vec2 g = p * cells;
    vec2 cell = floor(g);
    vec2 h = hash2(cell + cells * 7.0);
    if (h.x > keep)
    {
        return 0.0;
    }
    vec2 centre = 0.25 + 0.5 * hash2(cell + 19.0);
    float radius = 0.08 + 0.17 * h.y;
    float d = length(fract(g) - centre);
    return smoothstep(radius, radius * 0.25, d) * (0.4 + 0.6 * h.y);
}

float dirt(vec2 uv)
{
    vec2 p = (uv - 0.5) * vec2(luminous_aspect, 1.0); // from the picture's middle, so the same in any shape
    float d = specks(p, 9.0, 0.35) * 0.8 + specks(p + 0.37, 23.0, 0.25) * 0.6 + specks(p + 0.71, 57.0, 0.2) * 0.35;
    // a faint smear across it all
    float smear = 0.5 + 0.5 * sin(p.x * 3.1 + sin(p.y * 2.3) * 1.7) * sin(p.y * 1.7 + p.x * 0.6);
    return d + 0.15 * smear;
}

vec3 glow(vec2 uv)
{
    // a wide blur of the bright light, two rings out to a fifth of the
    // frame: what lights the dirt, well beyond the light's own glare
    vec3 s = texture(bloomMap, uv).rgb * 0.1;
    for (int i = 0; i < 8; ++i)
    {
        float a = 0.785398 * float(i);
        vec2 dir = vec2(cos(a) / luminous_aspect, sin(a)) / (luminous_frame_scale > 0.0 ? luminous_frame_scale : 1.0);
        s += texture(bloomMap, uv + dir * 0.09).rgb * 0.06;
        s += texture(bloomMap, uv + dir * 0.2).rgb * 0.0525;
    }
    return s;
}

void main()
{
    vec2 uv = vary_fragcoord;
    vec3 light = texture(diffuseRect, uv).rgb;
    if (luminous_flare_dirt > 0.0)
    {
        float s = luminous_frame_scale > 0.0 ? luminous_frame_scale : 1.0;
        light += dirt((uv - 0.5) * s + 0.5) * glow(uv) * luminous_flare_dirt * 4.0;
    }
    frag_color = vec4(max(light * luminous_flare, vec3(0.0)), 0.0);
}
