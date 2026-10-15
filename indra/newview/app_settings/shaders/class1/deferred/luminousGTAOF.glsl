/**
 * @file luminousGTAOF.glsl
 * @brief Photo mode: GTAO at half size, for the live view.
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

// GTAO at half the frame's size, for the live view with low quality live
// on: aoUtil.glsl's own GTAO, with the frame's screen_res, so each texel
// takes the samples the full-size pass would at its place. Visibility in
// red, the point's view depth in green for luminousGTAOUpF's upsampling.

out vec4 frag_color;

in vec2 vary_fragcoord;

vec4 getPosition(vec2 pos_screen);
vec4 getNorm(vec2 pos_screen);
float luminousGTAO(vec4 pos, vec3 norm, vec2 pos_screen);

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 pos = getPosition(tc);
    vec4 norm = getNorm(tc);
    frag_color = vec4(luminousGTAO(pos, norm.xyz, tc), pos.z, 0.0, 1.0);
}
