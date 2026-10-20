/**
 * @file luminousDistortionF.glsl
 * @brief Photo mode: lens distortion, barrel or pincushion.
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

// A lens's radial distortion: positive bows straight lines outward (barrel,
// as a wide lens), negative pulls them in (pincushion, as a long one). The
// frame is scaled so that its corners never sample past its edges.

out vec4 frag_color;

uniform sampler2D diffuseRect; // the frame, drawn anew into the other target
uniform vec2 screen_res;
uniform float luminous_distortion;
uniform float luminous_frame_scale; // the frame this many picture heights tall: a render's margin, or a shape chosen live
uniform float luminous_picture_aspect; // the picture's width over height; 0: the frame's (a shape chosen on the Render tab, live)

in vec2 vary_fragcoord;

void main()
{
    float aspect = screen_res.x / screen_res.y;
    vec2 p = (vary_fragcoord - 0.5) * vec2(aspect, 1.0) * luminous_frame_scale; // in the picture's units
    float k = luminous_distortion * 0.25;
    float picture_aspect = luminous_picture_aspect > 0.0 ? luminous_picture_aspect : aspect;
    float corner2 = dot(vec2(picture_aspect, 1.0) * 0.5, vec2(picture_aspect, 1.0) * 0.5); // the picture's corner
    float fit = k > 0.0 ? 1.0 / (1.0 + k * corner2) : 1.0;
    vec2 q = p * (1.0 + k * dot(p, p)) * fit;
    // the alpha is Firestorm's glow mask: this pixel's, carried over
    // unmoved; only the colour is distorted
    frag_color = vec4(texture(diffuseRect, q / (vec2(aspect, 1.0) * luminous_frame_scale) + 0.5).rgb,
                      texture(diffuseRect, vary_fragcoord).a);
}
