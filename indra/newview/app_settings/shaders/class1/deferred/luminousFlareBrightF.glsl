/**
 * @file luminousFlareBrightF.glsl
 * @brief Photo mode: lens flare, the light bright enough to flare, at half size.
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

// What a lens flares from: the light more than three times white after
// exposure, as the anamorphic streaks take it (Amplify lowers it to three
// quarters of white, so lamps and a dim sun flare too) -- the sun, glints,
// lamps, bright emissive faces -- with a soft knee, so a light does not pop
// in as it crosses the line, and weighted again by how far over it is, so a
// light's core flares and the wide glow round the sun hardly does (Second
// Life's sun is a large soft disc: all of it, copied, would make ghosts like
// blots). In alpha, the very brightest -- five times white -- for the
// starburst: points, not glows, make spikes. At half size: one bilinear tap
// at a half-size texel's centre averages the four frame texels under it.
// The rest is black. In the sky, a light must be wider than a few pixels:
// the sun and moon flare, the stars do not (at night the exposure lifts them
// over the threshold, and each would make its own ghosts).

out vec4 frag_color;

uniform sampler2D diffuseRect; // the frame
uniform sampler2D exposureMap; // the metered exposure
uniform sampler2D depthMap;    // the sky is 1
uniform float luminous_flare_threshold; // in times white after exposure
uniform float luminous_flare_erode;     // pixels: lights in the sky narrower than twice this are left out

in vec2 vary_fragcoord;

void main()
{
    const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
    float e = max(texture(exposureMap, vec2(0.5)).r, 1e-4);
    vec3 s = min(texture(diffuseRect, vary_fragcoord).rgb, vec3(65000.0));
    float l = dot(s, LUMA);
    if (texture(depthMap, vary_fragcoord).r >= 1.0)
    {
        // only as bright as the dimmest of a cross erode pixels out
        vec2 px = luminous_flare_erode / vec2(textureSize(diffuseRect, 0));
        float lx = min(dot(texture(diffuseRect, vary_fragcoord + vec2(px.x, 0.0)).rgb, LUMA),
                       dot(texture(diffuseRect, vary_fragcoord - vec2(px.x, 0.0)).rgb, LUMA));
        float ly = min(dot(texture(diffuseRect, vary_fragcoord + vec2(0.0, px.y)).rgb, LUMA),
                       dot(texture(diffuseRect, vary_fragcoord - vec2(0.0, px.y)).rgb, LUMA));
        float eroded = min(l, min(lx, ly));
        s *= eroded / max(l, 1e-4);
        l = eroded;
    }
    float threshold = luminous_flare_threshold / e;
    float knee = threshold * 0.5;
    float soft = clamp(l - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float over = max(soft, l - threshold);
    float share = clamp(over / max(l, 1e-4), 0.0, 1.0);
    float core = max(l - 5.0 / e, 0.0) * e / 5.0;
    frag_color = vec4(s * share * share, core);
}
