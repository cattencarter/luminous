/**
 * @file luminousDepthOfFieldF.glsl
 * @brief Photo mode: depth of field, gathered at full resolution in HDR.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on Second Life Viewer Source Code, Copyright (C) 2007, Linden Research, Inc.
 * The idea of a better depth of field for photographs is from the Black Dragon
 * Viewer by NiranV Dean; the single-pass gather follows Dennis Gustafsson,
 * "Bokeh depth of field in a single pass" (2018).
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

// Reads what Firestorm's own circle-of-confusion pass (cofF.glsl) wrote: the
// HDR colour, and in alpha the signed blur radius in pixels, packed as
// cof/max_cof*0.5+0.5 -- positive in front of the focus, negative behind it.
// The radius is monotonic in depth, so it also says which of two pixels is
// nearer, and no depth needs reading.
//
// Every pixel looks at a disc of samples out to the largest blur there can be,
// and takes a sample when that sample's own blur reaches this far. So a blurred
// foreground spreads over whatever is behind it, edges included. A sample from
// behind this pixel may reach no further than twice this pixel's own blur, so a
// blurred background never spreads onto a subject in focus.
//
// The samples lie on a Vogel disc, one per equal area. They read a mip level
// as coarse as their spacing, so a highlight smaller than the gaps between them
// still lands on every pixel it should: a bright point becomes a full, even
// disc -- bokeh -- rather than a pattern of dots.
//
// The spiral runs outward, so a pixel stops at the last sample that the
// largest blur near it (the tiles, luminousDoFTileF.glsl) can reach: every
// sample further out would count for nothing. The picture is the one
// the whole spiral gives -- the same samples, at the same mip level -- and a
// pixel with nothing blurred near it gathers nothing. While a shot is set up,
// a pixel that would need more samples than luminous_dof_budget walks a
// thinner spiral instead, reading a coarser level to match.

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform sampler2D lightMap;         // the tiles: the largest blur that can reach each

uniform vec2 screen_res;
uniform float max_cof;

uniform int luminous_dof_samples;   // the whole spiral's samples
uniform float luminous_dof_lod;     // its mip level, matching their spacing
uniform int luminous_dof_budget;    // the most a pixel takes: the whole spiral
uniform int luminous_dof_tile;      // the tiles' size in pixels; 0: no tiles, the whole spiral everywhere
uniform float luminous_dof_front;   // 1: the foreground blurs too; 0: it stays sharp
uniform float luminous_dof_blades;  // 0: a round aperture; else its number of blades

in vec2 vary_fragcoord;

const float GOLDEN_ANGLE = 2.39996323;
const float PI = 3.14159265;

float cofOf(float a)
{
    float cof = (a * 2.0 - 1.0) * max_cof;
    return luminous_dof_front > 0.5 ? cof : min(cof, 0.0);
}

// The aperture's edge at this angle, as a fraction of its corners' radius.
float aperture(float angle)
{
    if (luminous_dof_blades < 3.0)
    {
        return 1.0;
    }
    float segment = 2.0 * PI / luminous_dof_blades;
    float a = mod(angle, segment) - segment * 0.5;
    return cos(segment * 0.5) / cos(a);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 centre = textureLod(diffuseRect, tc, 0.0);
    float centre_cof = cofOf(centre.a);
    float centre_size = abs(centre_cof);

    float n = float(luminous_dof_samples);
    float lod = luminous_dof_lod;
    int samples = luminous_dof_samples;
    if (luminous_dof_tile > 0 && max_cof > 0.0)
    {
        // Sample i lies at sqrt((i + 0.5) / n) * max_cof; one at or past the
        // largest blur near this pixel plus the half pixel of the soft edge
        // below has a reach of exactly 0.
        float far = texelFetch(lightMap, ivec2(gl_FragCoord.xy) / luminous_dof_tile, 0).r + 0.5;
        float share = min(far * far / (max_cof * max_cof), 1.0);
        samples = clamp(int(ceil(n * share - 0.5)), 0, luminous_dof_samples);
        if (samples > luminous_dof_budget)
        {
            // Thinned, so that the budget's worth fall inside that reach.
            n = float(luminous_dof_budget) / share;
            samples = luminous_dof_budget;
            lod = max(0.0, log2(max_cof * sqrt(PI / n)));
        }
    }

    vec3 colour = centre.rgb;
    float total = 1.0;

    for (int i = 0; i < samples; ++i)
    {
        float angle = float(i) * GOLDEN_ANGLE;
        float radius = sqrt((float(i) + 0.5) / n) * max_cof;
        vec2 offset = vec2(cos(angle), sin(angle)) * radius;

        vec4 s = textureLod(diffuseRect, tc + offset / screen_res, lod);
        float cof = cofOf(s.a);
        float size = abs(cof);
        if (cof < centre_cof)
        {
            // Behind this pixel.
            size = min(size, centre_size * 2.0);
        }
        size *= aperture(angle);

        // A sample that does not reach this pixel counts as what has been
        // gathered so far, so it changes nothing.
        float reach = smoothstep(radius - 0.5, radius + 0.5, size);
        colour += mix(colour / total, s.rgb, reach);
        total += 1.0;
    }

    frag_color = vec4(colour / total, 1.0);
}
