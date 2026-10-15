/**
 * @file luminousStreakF.glsl
 * @brief Photo mode: anamorphic streaks, a horizontal glare from bright lights.
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

// What an anamorphic lens draws from a bright light: a thin horizontal line,
// faintly blue as the classic lenses draw it, or the colour chosen
// (`luminous_streak_tint`). Only what ends up more than three times white
// after exposure takes part -- the sun, its glints, a lamp at night -- not a
// bright sky, or every row of it would streak. Two kinds of pass, at a
// quarter of the frame's width:
//   0  the light over the threshold, each texel the average of its four
//      pixels, so a light one pixel wide is never stepped over. In the sky, a
//      light must be wider than a few pixels: the sun and moon streak, the
//      stars do not (at night the exposure lifts them over the threshold).
//   1  seven taps along the row, `luminous_streak_spacing` texels apart, each
//      weighted by the falloff over its distance; spacings 1, 4, 16 ... in
//      turn spread the light the whole length of the streak without gaps.

out vec4 frag_color;

uniform sampler2D diffuseRect; // pass 0: the frame; pass 1: the previous pass
uniform sampler2D exposureMap; // the metered exposure
uniform sampler2D depthMap;    // the sky is 1
uniform vec2 screen_res;       // the frame's size
uniform int luminous_streak_pass;
uniform float luminous_streak_spacing; // texels
uniform float luminous_streak_decay;   // the falloff per texel
uniform float luminous_streak_erode;   // pixels: lights in the sky narrower than twice this are left out
uniform vec3 luminous_streak_tint;     // the streak's colour, as bright as the classic blue
uniform float luminous_streak_gain;    // stronger toward the top of the slider

in vec2 vary_fragcoord;

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

float overAt(ivec2 p, float threshold)
{
    p = clamp(p, ivec2(0), ivec2(screen_res) - 1);
    return max(dot(texelFetch(diffuseRect, p, 0).rgb, LUMA) - threshold, 0.0);
}

vec3 brightPass()
{
    float threshold = 3.0 / max(texture(exposureMap, vec2(0.5)).r, 1e-4);
    ivec2 base = ivec2(int(gl_FragCoord.x) * 4, int(gl_FragCoord.y));
    int r = int(luminous_streak_erode + 0.5);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        ivec2 p = base + ivec2(i, 0);
        vec3 s = texelFetch(diffuseRect, clamp(p, ivec2(0), ivec2(screen_res) - 1), 0).rgb;
        float lum = dot(s, LUMA);
        float over = max(lum - threshold, 0.0);
        if (over > 0.0 && texelFetch(depthMap, clamp(p, ivec2(0), ivec2(screen_res) - 1), 0).r >= 1.0)
        {
            // In the sky: only as bright as the dimmest of a cross r pixels out.
            over = min(over, min(min(overAt(p + ivec2(r, 0), threshold), overAt(p - ivec2(r, 0), threshold)),
                                 min(overAt(p + ivec2(0, r), threshold), overAt(p - ivec2(0, r), threshold))));
        }
        sum += s * (over / max(lum, 1e-4));
    }
    // tinted, and strong enough that the spread line still shows
    return sum * 0.25 * luminous_streak_tint * 12.0 * luminous_streak_gain;
}

vec3 spread()
{
    float texel = 1.0 / float(textureSize(diffuseRect, 0).x);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (int i = -3; i <= 3; ++i)
    {
        float d = abs(float(i)) * luminous_streak_spacing;
        float w = pow(luminous_streak_decay, d);
        sum += texture(diffuseRect, vary_fragcoord + vec2(float(i) * luminous_streak_spacing * texel, 0.0)).rgb * w;
        weight += w;
    }
    return sum / weight;
}

void main()
{
    frag_color = vec4(luminous_streak_pass == 0 ? brightPass() : spread(), 1.0);
}
