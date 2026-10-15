/**
 * @file luminousBloomDownF.glsl
 * @brief Photo mode: bloom, one step down the chain of half-size copies.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on Second Life Viewer Source Code, Copyright (C) 2007, Linden Research, Inc.
 * The filters follow Jorge Jimenez, "Next Generation Post Processing in Call
 * of Duty: Advanced Warfare" (SIGGRAPH 2014).
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

// Halves the frame, or the last half: Jimenez's thirteen taps, read as five
// overlapping 2x2 boxes -- the centre one weighted a half, the four corners an
// eighth each -- which keeps the chain from shimmering as the camera moves.
// Games weight the first step's boxes by 1/(1+luma) (Brian Karis's average)
// against flicker from single bright pixels; that takes a lamp's own light
// out of its bloom and darkens the frame round it, and a still has no
// flicker to fear, so every box counts by its area alone.

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform vec2 luminous_texel;  // one texel of the source, in uv

in vec2 vary_fragcoord;

vec3 tap(vec2 offset)
{
    return min(texture(diffuseRect, vary_fragcoord + offset * luminous_texel).rgb, vec3(65000.0));
}

void main()
{
    vec3 a = tap(vec2(-2.0,  2.0));
    vec3 b = tap(vec2( 0.0,  2.0));
    vec3 c = tap(vec2( 2.0,  2.0));
    vec3 d = tap(vec2(-2.0,  0.0));
    vec3 e = tap(vec2( 0.0,  0.0));
    vec3 f = tap(vec2( 2.0,  0.0));
    vec3 g = tap(vec2(-2.0, -2.0));
    vec3 h = tap(vec2( 0.0, -2.0));
    vec3 i = tap(vec2( 2.0, -2.0));
    vec3 j = tap(vec2(-1.0,  1.0));
    vec3 k = tap(vec2( 1.0,  1.0));
    vec3 l = tap(vec2(-1.0, -1.0));
    vec3 m = tap(vec2( 1.0, -1.0));

    vec3 centre = (j + k + l + m) * 0.25;
    vec3 tl = (a + b + d + e) * 0.25;
    vec3 tr = (b + c + e + f) * 0.25;
    vec3 bl = (d + e + g + h) * 0.25;
    vec3 br = (e + f + h + i) * 0.25;

    vec3 result = centre * 0.5 + (tl + tr + bl + br) * 0.125;
    frag_color = vec4(max(result, vec3(0.0)), 1.0);
}
