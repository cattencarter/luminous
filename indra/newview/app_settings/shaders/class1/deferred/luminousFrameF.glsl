/**
 * @file luminousFrameF.glsl
 * @brief Photo mode: the picture's frame over the live view, in a shape chosen on the Render tab.
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

// What a render will keep, shown on the finished live frame: outside it the
// view dimmed, not hidden, so what is near can still be seen; a thin light
// line along its edge.

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform vec2 screen_res;
uniform vec4 luminous_frame_rect; // the picture: left, bottom, right, top, 0 .. 1 of the frame

in vec2 vary_fragcoord;

void main()
{
    vec4 c = texture(diffuseRect, vary_fragcoord);
    vec2 px = vary_fragcoord * screen_res;
    vec4 rect = luminous_frame_rect * screen_res.xyxy;
    bool inside = px.x >= rect.x && px.x <= rect.z && px.y >= rect.y && px.y <= rect.w;
    if (!inside)
    {
        c.rgb *= 0.35;
    }
    // the edge: a line a pixel and a half wide, just inside
    float to_edge = min(min(px.x - rect.x, rect.z - px.x), min(px.y - rect.y, rect.w - px.y));
    if (inside && to_edge < 1.5)
    {
        c.rgb = mix(c.rgb, vec3(0.85), 0.6);
    }
    frag_color = c;
}
