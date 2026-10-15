/**
 * @file postDeferredTonemap.glsl
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
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

in vec2 vary_fragcoord;

#ifdef GAMMA_CORRECT
uniform float gamma;
#endif

vec3 linear_to_srgb(vec3 cl);
vec3 toneMap(vec3 color);
vec3 luminousNoPost(vec3 color); // <Luminous/> tonemapUtilF.glsl

vec3 clampHDRRange(vec3 color);

// <Luminous> A dither of one step either way where the picture becomes eight
// bits: soft, dark gradients -- haze, a dusky sky -- otherwise break into
// flat bands up to 70 px wide. Triangular noise, so no bands and no shift in
// the mean. luminous_dither is one step of the target, 0 for anything finer
// than eight bits.
uniform float luminous_dither;

float luminousDitherHash(vec2 p)
{
    vec3 h = fract(vec3(p.xyx) * 0.1031);
    h += dot(h, h.yzx + 33.33);
    return fract((h.x + h.y) * h.z);
}

vec3 luminousDither(vec3 c)
{
    if (luminous_dither <= 0.0)
    {
        return c;
    }
    float n = luminousDitherHash(gl_FragCoord.xy) + luminousDitherHash(gl_FragCoord.xy + vec2(47.0, 113.0)) - 1.0;
    return c + n * luminous_dither;
}
// </Luminous>

#ifdef GAMMA_CORRECT
vec3 legacyGamma(vec3 color)
{
    vec3 c = 1. - clamp(color, vec3(0.), vec3(1.));
    c = 1. - pow(c, vec3(gamma)); // s/b inverted already CPU-side

    return c;
}
#endif

void main()
{
    //this is the one of the rare spots where diffuseRect contains linear color values (not sRGB)
    vec4 diff = texture(diffuseRect, vary_fragcoord);

#ifndef NO_POST
    diff.rgb = toneMap(diff.rgb);
#else
    // <Luminous> Firestorm's clamp, or photo mode's Look under a legacy sky:
    // tonemapUtilF.glsl, which sees the global LUMINOUS_LOOK define where
    // this program's own file does not.
    diff.rgb = luminousNoPost(diff.rgb);
#endif

#ifdef GAMMA_CORRECT
    diff.rgb = linear_to_srgb(diff.rgb);

#ifdef LEGACY_GAMMA
    diff.rgb = legacyGamma(diff.rgb);
#endif
    diff.rgb = luminousDither(diff.rgb); // <Luminous/> written as eight bits

#endif

    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0)); // We should always be 0-1 past this point

    //debugExposure(diff.rgb);
    frag_color = diff;
}

