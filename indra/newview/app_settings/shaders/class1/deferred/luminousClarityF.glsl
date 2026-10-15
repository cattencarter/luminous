/**
 * @file luminousClarityF.glsl
 * @brief Photo mode: clarity, local contrast on the luminance before tonemapping.
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

// How far each pixel's luminance stands from its surroundings' -- the bloom
// chain's wide blur -- in stops, scaled by clarity: positive pulls detail
// apart (texture, pores, weave), negative pulls it together (soft skin). The
// frame times that factor is drawn anew into the other target; colour keeps
// its hue. The detail is limited to two stops either way so edges against the
// sky do not halo hard.

out vec4 frag_color;

uniform sampler2D diffuseRect; // the frame
uniform sampler2D bloomMap;    // its wide blur, at half size
uniform float luminous_clarity;

in vec2 vary_fragcoord;

void main()
{
    const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
    vec4 frame = texture(diffuseRect, vary_fragcoord);
    float l = max(dot(frame.rgb, LUMA), 1e-5);
    float around = max(dot(texture(bloomMap, vary_fragcoord).rgb, LUMA), 1e-5);
    float detail = clamp(log2(l / around), -2.0, 2.0);
    // the alpha is Firestorm's glow mask: carried over
    frag_color = vec4(frame.rgb * exp2(luminous_clarity * 0.6 * detail), frame.a);
}
