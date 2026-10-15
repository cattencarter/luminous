/**
 * @file luminousSunShaftMaskF.glsl
 * @brief Photo mode: the bright sky around the sun, for sun shafts.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * After "Volumetric Light Scattering as a Post-Process", Kenny Mitchell, GPU Gems 3 (2007).
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

// What the shafts are made of: sky only, so anything in front of the sun --
// a roof, a branch, a leaf -- is a gap in the mask, and the blur that follows
// turns the gaps between occluders into rays. Faded with distance from the sun
// on screen, so a bright sky elsewhere does not streak the whole frame.
// Rendered at half resolution: it is about to be blurred anyway.
//
// Small bright points are taken out: each mask texel keeps the darkest of a
// 3x3 grid of the frame, two pixels apart. A star is a pixel or two, so it
// vanishes, and so does any speck that would draw a thin streak of its own;
// the sun or moon and the sky between leaves are larger and survive.

out vec4 frag_color;

uniform sampler2D diffuseRect;   // the HDR frame, before tonemapping
uniform sampler2D depthMap;

uniform vec2 luminous_sun_uv;
uniform float luminous_aspect;   // width / height of the frame
uniform float luminous_shaft_radius;
uniform vec2 luminous_src_texel; // one pixel of the frame, in uv

in vec2 vary_fragcoord;

// The sky's colour at uv, or black where something nearer than the sky is.
vec3 luminousSky(vec2 uv)
{
    // The sky is drawn at the far plane; everything else is nearer.
    float sky = step(0.99999, texture(depthMap, uv).r);
    return texture(diffuseRect, uv).rgb * sky;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    vec3 darkest = vec3(1e20);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            darkest = min(darkest, luminousSky(tc + vec2(x, y) * 2.0 * luminous_src_texel));
        }
    }

    vec2 d = (tc - luminous_sun_uv) * vec2(luminous_aspect, 1.0);
    float near_sun = 1.0 - smoothstep(0.0, luminous_shaft_radius, length(d));

    frag_color = vec4(darkest * near_sun, 1.0);
}
