/**
 * @file luminousSunShaftsF.glsl
 * @brief Photo mode: sun shafts, a radial blur of the sky mask toward the sun.
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

// Each pixel gathers the sky mask along the line from itself toward the sun,
// fading with each step. Where that line crosses open sky the pixel brightens,
// where it crosses an occluder it does not, so rays fan out from the sun past
// every edge in front of it. Drawn with additive blending into the HDR frame,
// before exposure and tonemapping, like the volumetric light.
//
// This is a screen-space effect: it shows only with the sun in or near the
// frame, and it does not know where in the scene the light falls. That is what
// the volumetric light is for; the two are meant to be used together.

out vec4 frag_color;

uniform sampler2D diffuseRect;   // the mask, half resolution, filtered

uniform vec2 luminous_sun_uv;
uniform float luminous_shaft_length;     // how far toward the sun each pixel looks, 0..1
uniform float luminous_shaft_strength;   // already faded for a sun behind or far off screen
uniform int luminous_shaft_samples;
uniform float luminous_shaft_reach; // beyond this from the sun the mask is 0: in the frame's heights
uniform float luminous_aspect;      // width / height of the frame

in vec2 vary_fragcoord;

// Interleaved gradient noise (Jimenez 2014): offsets each pixel's first sample,
// so the step pattern reads as fine grain instead of rings.
uniform float luminous_frame; // an accumulated snapshot's frame: fresh noise in each, so averaging smooths it

float luminousJitter(vec2 frag)
{
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    int samples = max(luminous_shaft_samples, 1);
    vec2 step_uv = (luminous_sun_uv - tc) * (luminous_shaft_length / float(samples));
    float jitter = luminousJitter(gl_FragCoord.xy + luminous_frame * vec2(37.0, 113.0));

    // The line runs straight at the sun, so it meets the mask once, where it
    // comes within reach of the sun, and the samples before that read 0:
    // start there, with the weight those steps would have left.
    float to_sun = length((luminous_sun_uv - tc) * vec2(luminous_aspect, 1.0));
    float enter = to_sun > luminous_shaft_reach ? 1.0 - luminous_shaft_reach / to_sun : 0.0; // of the way to the sun
    float first_f = ceil(enter * float(samples) / max(luminous_shaft_length, 1e-4) - jitter);
    int first = int(clamp(first_f, 0.0, float(samples)));
    if (first >= samples)
    {
        discard;
    }
    vec2 coord = tc + step_uv * (float(first) + jitter);

    vec3 sum = vec3(0.0);
    float weight = pow(0.97, float(first));
    for (int i = first; i < samples; ++i)
    {
        sum += texture(diffuseRect, coord).rgb * weight;
        weight *= 0.97;
        coord += step_uv;
    }

    frag_color = vec4(sum * (luminous_shaft_strength / float(samples)), 0.0);
}
