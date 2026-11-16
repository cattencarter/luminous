/**
 * @file luminousMotionF.glsl
 * @brief MetalFX temporal: the motion of a surface that moves by itself, where it is the surface the frame shows.
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

// Kept only where the fragment lies on the surface the frame shows: its
// distance within a few millimetres (plus 0.1 % of the distance) of the
// frame's depth at the texel. Its motion is where it was in the last frame
// minus where it is now, both without the jitter, in texels of the frame
// (GL's rows), as MetalFX temporal's camera motion is; clamped to 8192
// texels each way. Its depth is how far it lies from the frame's surface
// over that tolerance, so with a depth test of LESS the fragment closest to
// the surface the frame shows is kept, whatever the order of the draws.
// Blue is luminous_reactive (0 or 1): how much MetalFX temporal's reactive
// mask is to mark the texel, kept with the motion of the same fragment; the
// mask's stage fades it with distance.
// With LUMINOUS_TEXTURE_MOTION the motion also carries the texture's own
// shift s: the content at this fragment was at s from it in the last frame,
// where the last frame's texture coordinates equal this frame's here. To
// first order J s = d, J the last frame's coordinates' change per texel
// across the frame (dFdx and dFdy as columns, in GL's rows as gl_FragCoord
// and the motion are), d this frame's coordinates minus the last frame's,
// each part wrapped to [-0.5, 0.5) of a repeat. No shift where J is near
// singular (its determinant under LUMINOUS_TEXTURE_FLOOR times its columns'
// lengths); the shift is clamped to LUMINOUS_TEXTURE_LIMIT texels each way.

out vec4 frag_color;

uniform sampler2D depthMap;
uniform vec2 screen_res;
uniform vec2 luminous_depth_terms; // the projection's [2][2] and [3][2]
uniform float luminous_reactive;   // 0 or 1; 1: the surface changes in ways its motion does not carry

in vec4 vary_now;
in vec4 vary_last;

const float LUMINOUS_MOTION_NEAR = 0.003;   // metres
const float LUMINOUS_MOTION_SHARE = 0.001;  // of the distance
const float LUMINOUS_MOTION_LIMIT = 8192.0; // texels

// The distance from the camera of a depth in the depth buffer
float luminousDistance(float depth)
{
    return luminous_depth_terms.y / (depth * 2.0 - 1.0 + luminous_depth_terms.x);
}

#ifdef LUMINOUS_TEXTURE_MOTION
in vec2 vary_texture_now;
in vec2 vary_texture_last;

const float LUMINOUS_TEXTURE_FLOOR = 0.01; // the sine of the angle between J's columns
const float LUMINOUS_TEXTURE_LIMIT = 64.0; // texels

// The texture's own shift in texels of the frame: s with J s = d, J's
// columns dx and dy, by Cramer's rule; 0 where J is near singular.
vec2 luminousTextureShift(vec2 dx, vec2 dy)
{
    vec2 d = vary_texture_now - vary_texture_last;
    d -= floor(d + 0.5);
    float det = dx.x * dy.y - dy.x * dx.y;
    if (!(abs(det) > LUMINOUS_TEXTURE_FLOOR * length(dx) * length(dy)))
    {
        return vec2(0.0);
    }
    vec2 shift = vec2(dy.y * d.x - dy.x * d.y, dx.x * d.y - dx.y * d.x) / det;
    return clamp(shift, vec2(-LUMINOUS_TEXTURE_LIMIT), vec2(LUMINOUS_TEXTURE_LIMIT));
}
#endif

void main()
{
#ifdef LUMINOUS_TEXTURE_MOTION
    // taken while every fragment of the 2x2 quad still runs, before any discard
    vec2 texture_dx = dFdx(vary_texture_last);
    vec2 texture_dy = dFdy(vary_texture_last);
#endif
    float frame = luminousDistance(texelFetch(depthMap, ivec2(gl_FragCoord.xy), 0).r);
    float own = luminousDistance(gl_FragCoord.z);
    float tolerance = LUMINOUS_MOTION_NEAR + LUMINOUS_MOTION_SHARE * abs(frame);
    float off = abs(own - frame);
    if (!(off < tolerance) || !(vary_now.w > 0.0) || !(vary_last.w > 0.0))
    {
        discard;
    }
    vec2 motion = (vary_last.xy / vary_last.w - vary_now.xy / vary_now.w) * 0.5 * screen_res;
#ifdef LUMINOUS_TEXTURE_MOTION
    motion += luminousTextureShift(texture_dx, texture_dy);
#endif
    frag_color = vec4(clamp(motion, vec2(-LUMINOUS_MOTION_LIMIT), vec2(LUMINOUS_MOTION_LIMIT)), luminous_reactive, 0.0);
    gl_FragDepth = off / tolerance;
}
