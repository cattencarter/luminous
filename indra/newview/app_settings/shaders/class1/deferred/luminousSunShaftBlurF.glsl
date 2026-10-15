/**
 * @file luminousSunShaftBlurF.glsl
 * @brief Photo mode: softens the sun shaft mask, so the rays soften.
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

// A 5x5 tent-weighted blur of the shaft mask, spread by the Softness setting:
// soft rays instead of crisp ones. Runs only with Softness above zero, after
// the mask has lost its stars, so it cannot smear them back in.

out vec4 frag_color;

uniform sampler2D diffuseRect;    // the mask
uniform vec2 luminous_blur_step;  // uv between taps

in vec2 vary_fragcoord;

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            float w = float(3 - abs(x)) * float(3 - abs(y));
            sum += texture(diffuseRect, tc + vec2(x, y) * luminous_blur_step).rgb * w;
            total += w;
        }
    }
    frag_color = vec4(sum / total, 1.0);
}
