/**
 * @file luminousGTAOUpF.glsl
 * @brief Photo mode: the half-size GTAO upsampled into the light map.
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

// The half-size GTAO brought to the frame's size and multiplied into the
// light map's ambient occlusion (green; the sun pass wrote 1 there): the four
// half-size texels round each pixel by bilinear weight, each also by how
// near its depth is to the pixel's, so no occlusion crosses a silhouette.
// Where none is near, the nearest in depth. Drawn with the light map as the
// destination's factor, red and blue 1, alpha left alone.

out vec4 frag_color;

uniform sampler2D diffuseRect; // the half-size GTAO: visibility, view depth
uniform sampler2D depthMap;
uniform mat4 inv_proj;
uniform vec2 luminous_half_size;

in vec2 vary_fragcoord;

float luminousViewDepth(vec2 tc)
{
    float d = texture(depthMap, tc).r;
    vec4 p = inv_proj * vec4(tc * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.z / p.w;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float z = luminousViewDepth(tc);
    vec2 hp = tc * luminous_half_size - 0.5;
    vec2 base = floor(hp);
    vec2 f = hp - base;
    float tolerance = 0.02 * abs(z) + 0.02;

    float sum = 0.0;
    float total = 0.0;
    float nearest = 1.0;
    float nearest_dz = 1e20;
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            vec2 s = texture(diffuseRect, (base + vec2(x, y) + 0.5) / luminous_half_size).rg;
            float bilinear = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
            float dz = abs(s.g - z);
            float w = bilinear * exp(-dz / tolerance);
            sum += s.r * w;
            total += w;
            if (dz < nearest_dz)
            {
                nearest_dz = dz;
                nearest = s.r;
            }
        }
    }
    float ao = total > 1e-3 ? sum / total : nearest;
    frag_color = vec4(1.0, ao, 1.0, 1.0);
}
