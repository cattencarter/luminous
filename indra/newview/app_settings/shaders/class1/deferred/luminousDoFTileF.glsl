/**
 * @file luminousDoFTileF.glsl
 * @brief Photo mode: depth of field's tiles, the largest blur in each.
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

// Two passes, one pixel a tile. The first writes the largest blur radius in
// its tile of the frame; the second, the largest among the tiles near enough
// to reach into it. The gather then looks only as far as that: a sample
// further out cannot reach the pixel, so leaving it out changes nothing, and
// where everything near is in focus there is nothing to gather.

out vec4 frag_color;

uniform sampler2D diffuseRect;  // pass 0: the frame, the blur in alpha; pass 1: the tiles

uniform int luminous_dof_pass;
uniform int luminous_dof_tile;  // pass 0: the tile's size in pixels
uniform int luminous_dof_reach; // pass 1: how many tiles out a blur can reach from
uniform float max_cof;
uniform float luminous_dof_front;

void main()
{
    ivec2 tile = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(diffuseRect, 0);
    float largest = 0.0;

    if (luminous_dof_pass == 0)
    {
        ivec2 origin = tile * luminous_dof_tile;
        for (int y = 0; y < luminous_dof_tile; ++y)
        {
            for (int x = 0; x < luminous_dof_tile; ++x)
            {
                ivec2 p = min(origin + ivec2(x, y), size - 1);
                float cof = (texelFetch(diffuseRect, p, 0).a * 2.0 - 1.0) * max_cof;
                // As the gather reads it: no foreground blur, no foreground.
                cof = luminous_dof_front > 0.5 ? cof : min(cof, 0.0);
                largest = max(largest, abs(cof));
            }
        }
    }
    else
    {
        for (int y = -luminous_dof_reach; y <= luminous_dof_reach; ++y)
        {
            for (int x = -luminous_dof_reach; x <= luminous_dof_reach; ++x)
            {
                ivec2 p = clamp(tile + ivec2(x, y), ivec2(0), size - 1);
                largest = max(largest, texelFetch(diffuseRect, p, 0).r);
            }
        }
    }

    frag_color = vec4(largest, 0.0, 0.0, 1.0);
}
