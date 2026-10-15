/**
 * @file luminousProjectorBeamsF.glsl
 * @brief Photo mode: projector beams, the light of a projector seen in the air.
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

// One projector's light, scattered toward the camera by the air inside its
// beam: drawn once per projector, with additive blending into the HDR frame,
// before exposure and tonemapping. The same single scattering as the
// volumetric light, but for a projector: its colour, its projected texture at
// each point, Linden's own distance falloff, and -- for the two projectors
// that cast shadows -- their spot shadow maps, so a beam is cut by what stands
// in it. The others light the air they reach, but things in their beam cut no
// gaps in it.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D projectionMap;

uniform int luminous_pb_samples;
uniform float luminous_pb_density;     // scattering per metre: the beams' strength
uniform float luminous_pb_anisotropy;  // toward the light: low, so beams show from the side

uniform vec3 luminous_pb_center;       // eye space
uniform float luminous_pb_size;        // range, metres
uniform vec3 luminous_pb_color;        // linear
uniform float luminous_pb_falloff;
uniform mat4 luminous_pb_proj_mat;     // eye space to the projector's texture space
uniform vec3 luminous_pb_proj_n;       // the projector's direction, eye space
uniform vec3 luminous_pb_proj_origin;  // the projector's apex, eye space
uniform float luminous_pb_proj_focus;
uniform float luminous_pb_proj_range;
uniform float luminous_pb_proj_lod;
uniform int luminous_pb_shadow_idx;    // -1, or which spot shadow map
uniform float luminous_pb_shadow_fade;

#if defined(SPOT_SHADOW)
uniform sampler2DShadow shadowMap4;
uniform sampler2DShadow shadowMap5;
uniform mat4 shadow_matrix[6];
uniform float spot_shadow_bias;
#endif

vec4 getPosition(vec2 pos_screen);

// Linden's calcLegacyDistanceAttenuation, so the air fades as the surfaces do.
float luminousLegacyAttenuation(float distance, float falloff)
{
    float dist_atten = 1.0 - clamp((distance + falloff) / (1.0 + falloff), 0.0, 1.0);
    return dist_atten * dist_atten * 2.0;
}

// lpos: the point in the spot shadow map's homogeneous coordinates.
float luminousSpotVisibility(vec4 lpos)
{
#if defined(SPOT_SHADOW)
    if (luminous_pb_shadow_idx >= 0)
    {
        vec3 stc = lpos.xyz / lpos.w;
        if (all(greaterThanEqual(stc, vec3(0.0))) && all(lessThanEqual(stc, vec3(1.0))))
        {
            float lit = (luminous_pb_shadow_idx == 0) ? texture(shadowMap4, vec3(stc.xy, stc.z + spot_shadow_bias))
                                                      : texture(shadowMap5, vec3(stc.xy, stc.z + spot_shadow_bias));
            return clamp(lit + luminous_pb_shadow_fade, 0.0, 1.0);
        }
    }
#endif
    return 1.0;
}

// The projector's light arriving at eye-space pos, before scattering: zero
// outside its beam or its range. projected and shadow_pos are pos in the
// projector's texture and shadow map, homogeneous (main works them out
// along the ray).
vec3 luminousProjectorLight(vec3 pos, vec4 projected, vec4 shadow_pos)
{
    vec3 lv = luminous_pb_center - pos;
    float dist = length(lv);
    if (dist >= luminous_pb_size)
    {
        return vec3(0.0);
    }
    // In front of the apex, or not lit at all: behind it the perspective
    // divide flips a point back into the texture, which would draw a round
    // glow around the projector, above it too. From the apex, not from the
    // near plane as Linden's surfaces are lit: that plane is the prim's front
    // face, often well below a lamp, and a beam that begins there hangs in
    // the air under it. What lies between the apex and the face is mostly
    // inside the lamp, hidden by it.
    float l_dist = -dot(lv, luminous_pb_proj_n);
    if (projected.w <= 0.0)
    {
        return vec3(0.0);
    }
    vec2 uv = projected.xy / projected.w;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        return vec3(0.0);
    }
    // Linden's getProjectedLightDiffuseColor: the texture blurs past the focus.
    float lod = clamp((l_dist - luminous_pb_proj_focus) / luminous_pb_proj_range, 0.0, 1.0) * luminous_pb_proj_lod;
    vec4 tex = textureLod(projectionMap, uv, lod);

    float atten = luminousLegacyAttenuation(dist / luminous_pb_size, luminous_pb_falloff);
    // The light spreads from the apex: the same light through a narrower cone
    // near the lamp, so the air there is lit more, falling off as the square
    // of the distance -- a factor of 1 at a third of the light's radius, at
    // most 25 closer in. Without it the narrow top of the beam scatters
    // almost nothing, and the beam seems to begin well below the lamp.
    float from_apex = length(pos - luminous_pb_proj_origin);
    float ref = luminous_pb_size / 4.5; // size is the radius times 1.5
    float spread = min((ref * ref) / max(from_apex * from_apex, 1e-4), 25.0);
    return luminous_pb_color * tex.rgb * tex.a * atten * spread * luminousSpotVisibility(shadow_pos);
}

float luminousPhase(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cos_theta, 1e-4);
    return (1.0 - g2) / (4.0 * 3.14159265 * denom * sqrt(denom));
}

uniform float luminous_frame; // an accumulated snapshot's frame: fresh noise in each, so averaging smooths it

float luminousJitter(vec2 frag)
{
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

// Narrows [lo, hi] to where a + b t >= 0.
void luminousKeepWhere(float a, float b, inout float lo, inout float hi)
{
    if (abs(b) < 1e-8)
    {
        if (a < 0.0)
        {
            hi = lo - 1.0; // nowhere
        }
    }
    else if (b > 0.0)
    {
        lo = max(lo, -a / b);
    }
    else
    {
        hi = min(hi, -a / b);
    }
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec3 pos = getPosition(tc).xyz;
    float dist = length(pos);
    vec3 view_dir = pos / max(dist, 1e-4);

    // Only the stretch of the line of sight inside the light's range.
    float b = dot(view_dir, luminous_pb_center);
    float c = dot(luminous_pb_center, luminous_pb_center) - luminous_pb_size * luminous_pb_size;
    float h = b * b - c;
    if (h <= 0.0)
    {
        discard;
    }
    h = sqrt(h);
    float t0 = max(b - h, 0.0);
    float t1 = min(b + h, dist);
    if (t1 <= t0)
    {
        discard;
    }

    int samples = max(luminous_pb_samples, 1);
    float step_len = (t1 - t0) / float(samples);
    float jitter = luminousJitter(gl_FragCoord.xy + luminous_frame * vec2(37.0, 113.0));
    float g = luminous_pb_anisotropy;

    // The projector's texture coordinates along the line of sight are a
    // straight line in homogeneous space; the light is only where they are
    // in front of the apex and inside the texture -- the cone. Only the
    // samples there are taken, the same ones a march across the whole range
    // takes there (the rest add nothing), so a line of sight that misses the
    // cone costs nothing.
    vec4 proj_start = luminous_pb_proj_mat[3];
    vec4 proj_dir = luminous_pb_proj_mat * vec4(view_dir, 0.0);
    float lo = t0;
    float hi = t1;
    luminousKeepWhere(proj_start.w, proj_dir.w, lo, hi);
    luminousKeepWhere(proj_start.x, proj_dir.x, lo, hi);
    luminousKeepWhere(proj_start.w - proj_start.x, proj_dir.w - proj_dir.x, lo, hi);
    luminousKeepWhere(proj_start.y, proj_dir.y, lo, hi);
    luminousKeepWhere(proj_start.w - proj_start.y, proj_dir.w - proj_dir.y, lo, hi);
    int first = max(int(ceil((lo - t0) / step_len - jitter)), 0);
    int last = min(int(floor((hi - t0) / step_len - jitter)), samples - 1);
    if (hi <= lo || last < first)
    {
        discard;
    }

    vec4 shadow_start = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 shadow_dir = vec4(0.0);
#if defined(SPOT_SHADOW)
    if (luminous_pb_shadow_idx >= 0)
    {
        mat4 shadow_mat = shadow_matrix[4 + luminous_pb_shadow_idx];
        shadow_start = shadow_mat[3];
        shadow_dir = shadow_mat * vec4(view_dir, 0.0);
    }
#endif

    vec3 light = vec3(0.0);
    for (int i = first; i <= last; ++i)
    {
        float t = t0 + (float(i) + jitter) * step_len;
        vec3 p = view_dir * t;
        vec3 arriving = luminousProjectorLight(p, proj_start + proj_dir * t, shadow_start + shadow_dir * t);
        if (any(greaterThan(arriving, vec3(0.0))))
        {
            // Light travels from the apex to p, and is scattered back toward the eye.
            vec3 travel = normalize(p - luminous_pb_proj_origin);
            // No fading with distance from the camera, unlike the volumetric
            // light: a beam is a few metres of air, and at stage-haze densities
            // fading would hide the beam.
            light += arriving * luminousPhase(dot(travel, -view_dir), g);
        }
    }
    light *= luminous_pb_density * step_len;

    frag_color = vec4(max(light, vec3(0.0)), 0.0);
}
