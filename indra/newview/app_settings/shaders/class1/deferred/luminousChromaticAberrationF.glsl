/**
 * @file luminousChromaticAberrationF.glsl
 * @brief Photo mode: lateral chromatic aberration, before tonemapping.
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

// A lens bends red and blue light by different amounts, so toward the frame's
// edges their images part: coloured fringes on contrast, growing with the
// distance from the centre and absent in the middle. Each pixel reads the
// frame (a copy) at eight points along the line to the centre, each weighted
// by where on the spectrum it lies -- blue inward, green in the middle, red
// outward -- so the fringes are smooth gradients rather than three offset
// copies.

out vec4 frag_color;

uniform sampler2D diffuseRect; // the frame, drawn anew into the other target
uniform vec2 screen_res;
uniform float luminous_ca_amount; // the spread at the frame's corners, in pixels
uniform float luminous_frame_scale; // the frame this many picture heights tall: a render's margin, or a shape chosen live
uniform float luminous_picture_aspect; // the picture's width over height; 0: the frame's (a shape chosen on the Render tab, live)

in vec2 vary_fragcoord;

void main()
{
    vec2 tc = vary_fragcoord;
    vec2 from_centre = (tc - 0.5) * screen_res;
    float picture_aspect = luminous_picture_aspect > 0.0 ? luminous_picture_aspect : screen_res.x / screen_res.y;
    float corner = 0.5 * length(vec2(picture_aspect, 1.0)) * screen_res.y / luminous_frame_scale; // the picture's corner
    vec2 shift = from_centre / corner * luminous_ca_amount / screen_res;

    vec3 sum = vec3(0.0);
    vec3 weight = vec3(0.0);
    const int N = 8;
    for (int i = 0; i < N; ++i)
    {
        float t = (float(i) + 0.5) / float(N) * 2.0 - 1.0;
        vec3 w = clamp(vec3(1.0 - abs(t - 1.0), 1.0 - abs(t), 1.0 - abs(t + 1.0)), 0.0, 1.0);
        sum += texture(diffuseRect, tc + shift * t).rgb * w;
        weight += w;
    }
    // the alpha is Firestorm's glow mask: this pixel's, carried over
    frag_color = vec4(sum / weight, texture(diffuseRect, tc).a);
}
