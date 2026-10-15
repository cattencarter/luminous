/**
 * @file luminousAverageF.glsl
 * @brief Luminous renders: a frame copied at a scale, to average frames as light.
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

// The frames of one of Luminous's renders are averaged before tonemapping,
// as light, not as finished pictures: each added at 1 / frames into a float
// target, the sum put back in the last frame's place. A copy at a scale.

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform float luminous_scale;

in vec2 vary_fragcoord;

void main()
{
    frag_color = vec4(texture(diffuseRect, vary_fragcoord).rgb * luminous_scale, 1.0);
}
