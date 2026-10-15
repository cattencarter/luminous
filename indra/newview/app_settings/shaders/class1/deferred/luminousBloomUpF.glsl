/**
 * @file luminousBloomUpF.glsl
 * @brief Photo mode: bloom, one step back up the chain, or onto the frame.
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

// A 3x3 tent over the coarser level, drawn over the finer one with alpha
// luminous_weight. Each level becomes (1 - w) of itself and w of everything
// coarser, so the finished bloom is a weighted average of the levels that sums
// to one: it spreads light without adding any. The last step draws it onto the
// frame with the strength as alpha, so bloom takes light from where it is and
// lays it around, as a lens's glare does.

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform vec2 luminous_texel;  // one texel of the coarser level, in uv
uniform float luminous_weight;

in vec2 vary_fragcoord;

vec3 tap(vec2 offset)
{
    return texture(diffuseRect, vary_fragcoord + offset * luminous_texel).rgb;
}

void main()
{
    vec3 s = tap(vec2(0.0, 0.0)) * 4.0;
    s += (tap(vec2(-1.0, 0.0)) + tap(vec2(1.0, 0.0)) + tap(vec2(0.0, -1.0)) + tap(vec2(0.0, 1.0))) * 2.0;
    s += tap(vec2(-1.0, -1.0)) + tap(vec2(1.0, -1.0)) + tap(vec2(-1.0, 1.0)) + tap(vec2(1.0, 1.0));
    frag_color = vec4(s / 16.0, luminous_weight);
}
