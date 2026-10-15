/**
 * @file luminousFlareF.glsl
 * @brief Photo mode: lens flare, the ghosts, halo and starburst, at half size.
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

// A lens's flare, made from the bright light in the frame and from the sun's
// place, so that it follows each light's position, colour and strength:
//
// - Ghosts: reflections between the lens's glass surfaces put copies of a
//   bright light on the line through the frame's centre, mirrored, larger or
//   smaller, each tinted by its coatings and fringed red to blue. A copy t of
//   the way across lands 1/|1 - t| as large; all here are on the far side of
//   the centre, half to three times the light's size.
// - Halo: a ring round the centre, lit on the light's side.
// - Starburst: the aperture's blade edges diffract a bright point into
//   spikes, as many as the blades if even, twice as many if odd -- the blades
//   that also shape the bokeh.
// Variation (0-1) moves each ghost along its range at its own pace and turns
// the tints, the halo's size and the star: other lenses, other flares.
// Star blur (0-1) widens the spikes across, more the further out, into soft
// rays; the light spreads, its line a little brighter than a plain blur's,
// so a soft star still shows: across f times as wide, f^-0.5 as bright at
// its middle.
//
// The sun (or the moon) is drawn from its place on screen: in a photograph
// it is always far the brightest thing, but Second Life's sun after exposure
// may be no brighter than the sky round it at sunset, and is a wide soft
// disc, so copied from the frame it gives nothing, or blots. Its ghosts are
// the aperture's shape, brighter at the rim; its halo an arc on its side; its
// spikes thin; all in the sky's sunlight colour, and as much as the sky shows
// at its place in the depth buffer -- behind a lighthouse, none.
//
// Other lights -- lamps, glints, bright emissive faces -- are copied from the
// frame's light over three times white (diffuseRect), the starburst gathered
// from the brightest of it (alpha), with the sun's region left out.
//
// The composite pass adds this to the frame with the dirt on the lens.

out vec4 frag_color;

uniform sampler2D diffuseRect; // the bright light, half size; alpha: the brightest
uniform sampler2D bloomMap;    // the same, a quarter size and softer
uniform sampler2D exposureMap; // the metered exposure
uniform sampler2D depthMap;    // the scene's depth: is the sun's place sky?

uniform float luminous_aspect;         // width over height
uniform float luminous_flare_ghosts;
uniform float luminous_flare_halo;
uniform float luminous_flare_star;
uniform int luminous_flare_lines;      // the starburst's lines: half its spikes
uniform int luminous_flare_blades;     // the aperture's blades, 0 round
uniform float luminous_flare_angle;    // the blades' turn, radians
uniform float luminous_flare_star_len; // a spike's length, as a share of the frame's height
uniform int luminous_flare_star_taps;  // samples along each half line
uniform float luminous_flare_star_blur; // 0 sharp spikes, 1 soft rays; above 0 diffuseRect has mip levels
uniform vec2 luminous_sun_uv;          // the sun (or the moon) on screen
uniform float luminous_sun_weight;     // 0 when it is behind the camera, fading as it leaves the frame
uniform vec3 luminous_sun_colour;      // its light's colour, brightest channel 1
uniform float luminous_flare_variation;
uniform float luminous_frame_scale;    // a render's margin: the frame this much larger than the picture

// The flare is laid out in the picture's own measure and sampled in the
// frame's: a render's frame has a margin, the picture cut from its middle,
// so the ghosts and halo, laid out in the picture, come out the same size in
// a render as live, where frame and picture are one.
vec2 toPicture(vec2 frame_uv)
{
    float s = luminous_frame_scale > 0.0 ? luminous_frame_scale : 1.0;
    return (frame_uv - 0.5) * s + 0.5;
}

vec2 toFrame(vec2 picture_uv)
{
    float s = luminous_frame_scale > 0.0 ? luminous_frame_scale : 1.0;
    return (picture_uv - 0.5) / s + 0.5;
}

in vec2 vary_fragcoord;

const int GHOSTS = 6;
const float PHASE[GHOSTS] = float[](0.9, 2.3, 4.0, 5.1, 0.2, 3.3);
const float PACE[GHOSTS] = float[](1.0, 2.0, 1.0, 3.0, 2.0, 1.0);
const vec3 TINT_A[GHOSTS] = vec3[](vec3(1.0, 0.65, 0.35), vec3(0.45, 0.75, 1.0), vec3(0.75, 1.0, 0.55),
                                   vec3(1.0, 0.55, 0.85), vec3(0.55, 0.65, 1.0), vec3(1.0, 0.85, 0.55));
const vec3 TINT_B[GHOSTS] = vec3[](vec3(0.55, 1.0, 0.8), vec3(1.0, 0.75, 0.45), vec3(0.7, 0.6, 1.0),
                                   vec3(0.5, 0.9, 1.0), vec3(1.0, 0.7, 0.6), vec3(0.8, 1.0, 0.7));

// between 1.35 and 3.25 of the way across
float ghostT(int i)
{
    return 1.35 + 1.9 * (0.5 + 0.5 * sin(PHASE[i] + luminous_flare_variation * 6.2831853 * PACE[i]));
}

vec3 ghostTint(int i)
{
    return mix(TINT_A[i], TINT_B[i], 0.5 - 0.5 * cos(luminous_flare_variation * 6.2831853 * PACE[i]));
}

float window(vec2 uv)
{
    // nothing from outside the frame, and less from near its edge
    vec2 inside = step(vec2(0.0), uv) * step(uv, vec2(1.0));
    float r = length((uv - 0.5) * vec2(luminous_aspect, 1.0)) / length(vec2(0.5 * luminous_aspect, 0.5));
    return inside.x * inside.y * pow(max(1.0 - r, 0.0), 1.5);
}

// Level 0 always: with Star blur diffuseRect is bound with its mip levels.
vec3 fringed(sampler2D light, vec2 uv, vec2 dir)
{
    return vec3(textureLod(light, uv - dir, 0.0).r, textureLod(light, uv, 0.0).g, textureLod(light, uv + dir, 0.0).b);
}

// How much wider Star blur makes a spike: 1 to 4 (at 5 the spikes melt into
// a glow).
float starSpread()
{
    return 1.0 + 3.0 * luminous_flare_star_blur;
}

// The sun's region, which the drawn flare covers: left out of the copied one.
float awayFromSun(vec2 at)
{
    if (luminous_sun_weight <= 0.0)
    {
        return 1.0;
    }
    return smoothstep(0.12, 0.2, length((at - toPicture(luminous_sun_uv)) * vec2(luminous_aspect, 1.0)));
}

// Distance to the aperture's outline, 1 on it: a polygon of its blades, or
// a circle. p in units of the outline's radius, the frame's aspect undone.
float aperture(vec2 p)
{
    if (luminous_flare_blades < 3)
    {
        return length(p);
    }
    float sector = 6.2831853 / float(luminous_flare_blades);
    float a = atan(p.y, p.x) - luminous_flare_angle;
    float local = mod(a, sector) - 0.5 * sector;
    return length(p) * cos(local) / cos(0.5 * sector);
}

void main()
{
    vec2 uv = toPicture(vary_fragcoord); // the picture's measure; window(), awayFromSun() too
    float to_frame = luminous_frame_scale > 0.0 ? 1.0 / luminous_frame_scale : 1.0; // a length, picture to frame
    vec2 sun_uv = toPicture(luminous_sun_uv);
    vec2 to_centre = vec2(0.5) - uv;
    vec2 aspect = vec2(luminous_aspect, 1.0);
    float e = max(texture(exposureMap, vec2(0.5)).r, 1e-4);
    vec3 sum = vec3(0.0);

    // ---- the sun, drawn from its place ----
    if (luminous_sun_weight > 0.0)
    {
        float seen = 0.0;
        for (int i = 0; i < 9; ++i)
        {
            vec2 o = i == 0 ? vec2(0.0) : 0.008 * vec2(cos(0.785398 * float(i)), sin(0.785398 * float(i))) / aspect;
            seen += step(0.99999, texture(depthMap, luminous_sun_uv + o).r);
        }
        float sun = seen / 9.0 * luminous_sun_weight;
        if (sun > 0.0)
        {
            vec3 white = luminous_sun_colour / e; // its colour at white, after exposure
            vec2 sun_from_centre = sun_uv - vec2(0.5);
            vec2 from_sun = (uv - sun_uv) * aspect;

            if (luminous_flare_ghosts > 0.0)
            {
                for (int i = 0; i < GHOSTS; ++i)
                {
                    float k = 1.0 / (1.0 - ghostT(i)); // below 0: mirrored through the centre
                    vec2 c = vec2(0.5) + sun_from_centre * k;
                    float r = 0.035 * abs(k);
                    vec2 p = (uv - c) * aspect;
                    vec3 d = vec3(aperture(p / (r * 1.04)), aperture(p / r), aperture(p / (r * 0.96)));
                    vec3 disc = smoothstep(vec3(1.0), vec3(0.9), d) * (0.3 + 0.7 * smoothstep(vec3(0.5), vec3(0.97), d));
                    sum += disc * ghostTint(i) * white * (0.18 / max(abs(k), 0.5)) * sun * luminous_flare_ghosts;
                }
            }

            if (luminous_flare_halo > 0.0)
            {
                vec2 q = (uv - 0.5) * aspect;
                float dist = length(q);
                float rho = 0.24 + 0.16 * luminous_flare_variation;
                vec3 ring = exp(-pow((vec3(dist) - rho * vec3(1.015, 1.0, 0.985)) / 0.012, vec3(2.0)));
                vec2 toward = sun_from_centre * aspect;
                float facing = dist > 1e-4 && length(toward) > 1e-4 ? max(dot(q / dist, normalize(toward)), 0.0) : 0.0;
                sum += ring * pow(facing, 3.0) * white * 0.3 * sun * luminous_flare_halo;
            }

            if (luminous_flare_star > 0.0 && luminous_flare_lines > 0)
            {
                float spikes = 0.0;
                float spread = starSpread();
                for (int l = 0; l < luminous_flare_lines; ++l)
                {
                    float a = luminous_flare_angle + 3.14159265 * (float(l) + luminous_flare_variation) / float(luminous_flare_lines);
                    vec2 dir = vec2(cos(a), sin(a));
                    float across = abs(from_sun.x * dir.y - from_sun.y * dir.x);
                    float along = abs(dot(from_sun, dir));
                    // soft across (a bell, not a crease), widening outward
                    float width = (0.003 + along * 0.02) * spread;
                    spikes += exp(-(across * across) / (width * width)) * exp(-along / (luminous_flare_star_len * 0.2));
                }
                sum += white * spikes * sun * 3.0 * luminous_flare_star * inversesqrt(spread);
            }
        }
    }

    // ---- other bright lights, copied from the frame ----
    if (luminous_flare_ghosts > 0.0)
    {
        vec3 ghosts = vec3(0.0);
        for (int i = 0; i < GHOSTS; ++i)
        {
            float t = ghostT(i);
            vec2 at = uv + to_centre * t;
            float size = 1.0 / abs(1.0 - t);
            vec2 fringe = to_centre * 0.006 * size;
            ghosts += fringed(diffuseRect, toFrame(at), fringe * to_frame) * ghostTint(i) * (window(at) * awayFromSun(at) / (size * size));
        }
        sum += ghosts * 0.5 * luminous_flare_ghosts;
    }

    if (luminous_flare_halo > 0.0)
    {
        vec2 dir = to_centre * aspect;
        float len = length(dir);
        if (len > 1e-4)
        {
            vec2 step_to = (dir / len) / aspect * (0.24 + 0.16 * luminous_flare_variation);
            vec2 at = uv + step_to;
            sum += fringed(bloomMap, toFrame(at), step_to * 0.05 * to_frame) * vec3(0.9, 0.95, 1.0) * window(at) * awayFromSun(at) * 0.6 * luminous_flare_halo;
        }
    }

    if (luminous_flare_star > 0.0 && luminous_flare_lines > 0)
    {
        vec3 star = vec3(0.0);
        // Star blur reads coarser levels further out, as wide across as the
        // sun's soft spikes; a level's average keeps the light, and its
        // square root over the plain blur is the brighter line the sun's has.
        float texels = float(textureSize(diffuseRect, 0).y) * to_frame; // a picture height in texels
        float blur = luminous_flare_star_blur > 0.0 ? 3.0 * luminous_flare_star_blur : 0.0;
        for (int l = 0; l < luminous_flare_lines; ++l)
        {
            float a = luminous_flare_angle + 3.14159265 * (float(l) + luminous_flare_variation) / float(luminous_flare_lines);
            vec2 dir = vec2(cos(a) / luminous_aspect, sin(a)) * (luminous_flare_star_len / float(luminous_flare_star_taps));
            for (int k = 1; k <= luminous_flare_star_taps; ++k)
            {
                float w = exp(-5.0 * float(k) / float(luminous_flare_star_taps));
                float along = luminous_flare_star_len * float(k) / float(luminous_flare_star_taps);
                float lod = blur > 0.0 ? max(log2(blur * (0.003 + along * 0.02) * texels), 0.0) : 0.0;
                vec2 p = uv + dir * float(k);
                vec2 q = uv - dir * float(k);
                star += vec3(textureLod(diffuseRect, toFrame(p), lod).a * awayFromSun(p) + textureLod(diffuseRect, toFrame(q), lod).a * awayFromSun(q))
                        * w * exp2(0.5 * lod);
            }
        }
        // A point's light, spread along its spikes: per sample, so more taps
        // do not brighten them.
        sum += star * vec3(1.0, 0.93, 0.82) * (4.0 / float(luminous_flare_star_taps)) * luminous_flare_star;
    }

    frag_color = vec4(sum, 1.0);
}
