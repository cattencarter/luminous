/**
 * @file cofF.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;

uniform float depth_cutoff;
uniform float norm_cutoff;
uniform float focal_distance;
uniform float blur_constant;
uniform float tan_pixel_angle;
uniform float magnification;
uniform float max_cof;
// <Luminous> In a render that samples the lens across its frames: the share
// of each pixel's circle of confusion the sampled aperture gives (its radius
// over the one the f-number implies, kept within Most blur), and the share of
// that each frame blurs to fill between the samples. 0 outside such a render.
uniform float luminous_lens_share;
uniform float luminous_lens_fill;
// The lens distortion the picture has had when Second Life's blur runs on it,
// 0 if none, and a render's margin (luminousDistortionF.glsl).
uniform float luminous_depth_distortion;
uniform float luminous_frame_scale;
// </Luminous>

uniform mat4 inv_proj;
uniform vec2 screen_res;

in vec2 vary_fragcoord;

float calc_cof(float depth)
{
    float sc = (depth-focal_distance)/-depth*blur_constant;

    sc /= magnification;

    // tan_pixel_angle = pixel_length/-depth;
    float pixel_length =  tan_pixel_angle*-focal_distance;

    sc = sc/pixel_length;
    sc *= 1.414;

    return sc;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    // <Luminous> Each pixel's depth from where the lens distortion took the
    // pixel from, as luminousDistortionF.glsl maps it: read in place, the
    // depth's outline would lie beside the picture's, and the background
    // along an edge would stay as sharp as what stands in front of it.
    vec2 depth_tc = tc;
    if (luminous_depth_distortion != 0.0)
    {
        float aspect = screen_res.x / screen_res.y;
        vec2 p = (tc - 0.5) * vec2(aspect, 1.0) * luminous_frame_scale;
        float k = luminous_depth_distortion * 0.25;
        float corner2 = dot(vec2(aspect, 1.0) * 0.5, vec2(aspect, 1.0) * 0.5);
        float fit = k > 0.0 ? 1.0 / (1.0 + k * corner2) : 1.0;
        vec2 q = p * (1.0 + k * dot(p, p)) * fit;
        depth_tc = q / (vec2(aspect, 1.0) * luminous_frame_scale) + 0.5;
    }
    float z = texture(depthMap, depth_tc).r;
    // </Luminous>
    z = z*2.0-1.0;
    vec4 ndc = vec4(0.0, 0.0, z, 1.0);
    vec4 p = inv_proj*ndc;
    float depth = p.z/p.w;

    vec4 diff = texture(diffuseRect, vary_fragcoord.xy);

    float sc = calc_cof(depth);
    // <Luminous> The live view's Lens blur clamps every pixel's blur at Most
    // blur; the sampled aperture is one size for every depth, kept so the
    // background stays within it, which leaves near things sharper than the
    // live view shows them. Each frame's own blur gives the rest -- blurs
    // adding as their squares -- and a share of the sampled blur to fill
    // between the samples.
    float lens = sc * luminous_lens_share;
    sc = clamp(sc, -max_cof, max_cof);
    if (luminous_lens_share > 0.0)
    {
        float rest = sqrt(max(sc * sc - lens * lens, 0.0) + lens * lens * luminous_lens_fill * luminous_lens_fill);
        sc = sign(sc) * min(rest, max_cof);
    }
    // </Luminous>

    frag_color.rgb = diff.rgb;
    frag_color.a = sc/max_cof*0.5+0.5;
}
