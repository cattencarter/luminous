/**
 * @file class1/deferred/aoUtil.glsl
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

uniform sampler2D   noiseMap;
uniform sampler2D   depthMap;

uniform float ssao_radius;
uniform float ssao_max_radius;
uniform float ssao_factor;
uniform float ssao_factor_inv;

uniform mat4 inv_proj;
uniform vec2 screen_res;

vec2 getScreenCoordinateAo(vec2 screenpos)
{
    vec2 sc = screenpos.xy * 2.0;
    return sc - vec2(1.0, 1.0);
}

float getDepthAo(vec2 pos_screen)
{
    float depth = texture(depthMap, pos_screen).r;
    return depth;
}

vec4 getPositionAo(vec2 pos_screen)
{
    float depth = getDepthAo(pos_screen);
    vec2 sc = getScreenCoordinateAo(pos_screen);
    vec4 ndc = vec4(sc.x, sc.y, 2.0*depth-1.0, 1.0);
    vec4 pos = inv_proj * ndc;
    pos /= pos.w;
    pos.w = 1.0;
    return pos;
}

vec2 getKern(int i)
{
    vec2 kern[8];
    // exponentially (^2) distant occlusion samples spread around origin
    kern[0] = vec2(-1.0, 0.0) * 0.125*0.125;
    kern[1] = vec2(1.0, 0.0) * 0.250*0.250;
    kern[2] = vec2(0.0, 1.0) * 0.375*0.375;
    kern[3] = vec2(0.0, -1.0) * 0.500*0.500;
    kern[4] = vec2(0.7071, 0.7071) * 0.625*0.625;
    kern[5] = vec2(-0.7071, -0.7071) * 0.750*0.750;
    kern[6] = vec2(-0.7071, 0.7071) * 0.875*0.875;
    kern[7] = vec2(0.7071, -0.7071) * 1.000*1.000;

    return kern[i] / screen_res;
}

#if defined(LUMINOUS_GTAO)
// <Luminous> Photo mode's ambient occlusion: GTAO, ground-truth ambient
// occlusion (Jorge Jimenez et al., "Practical Real-Time Strategies for Accurate
// Indirect Occlusion", 2016), laid out after Intel's XeGTAO. In slices through
// the view direction, find the highest horizon on either side within a radius
// in metres, and integrate the cosine-weighted sky the normal still sees
// between the two horizons -- what a ray tracer would find, where Linden's
// counts eight samples against a guessed falloff. Then a correction for light
// bounced between surfaces, by the surface's own brightness, so that bright
// skin in a crease is not greyed. Firestorm's blur and its use of AO follow,
// as they do for Linden's.

uniform sampler2D diffuseRect;
uniform float luminous_ao_radius;   // metres
uniform float luminous_ao_strength; // a power on the result; 1 as computed
uniform float luminous_frame;       // an accumulated snapshot's frame, for fresh noise each; 0 otherwise

uniform int luminous_gtao;       // 1 draws GTAO; 2 GTAO drawn at half size elsewhere; 0 Linden's, live when GTAO is kept for renders
uniform int luminous_ao_slices; // 6
uniform int luminous_ao_steps;  // 8
const float LUMINOUS_PI = 3.14159265;

float luminousAoHash(vec2 p)
{
    vec3 h = fract(vec3(p.xyx) * 0.1031);
    h += dot(h, h.yzx + 33.33);
    return fract((h.x + h.y) * h.z);
}

float luminousGTAO(vec4 pos, vec3 norm, vec2 pos_screen)
{
    // The sky: nothing to occlude, and softenLight never reads its AO.
    if (getDepthAo(pos_screen) >= 1.0)
    {
        return 1.0;
    }
    vec3 P = pos.xyz;
    vec3 V = normalize(-P);
    vec3 N = normalize(norm);

    // The radius on screen, in pixels: a perspective projection's focal length
    // is 1 / inv_proj[1][1] in half-heights.
    float focal = 0.5 * screen_res.y / inv_proj[1][1];
    float radius_px = min(luminous_ao_radius * focal / max(-P.z, 0.01), 0.25 * screen_res.y);
    if (radius_px < 1.0)
    {
        return 1.0;
    }
    float falloff_range = 0.615 * luminous_ao_radius;
    float falloff_from = luminous_ao_radius - falloff_range;

    vec2 seed = gl_FragCoord.xy + luminous_frame * vec2(37.0, 113.0);
    float rotation = luminousAoHash(seed);
    float jitter = luminousAoHash(seed.yx + 71.0);

    float visibility = 0.0;
    for (int s = 0; s < luminous_ao_slices; ++s)
    {
        float phi = (float(s) + rotation) * LUMINOUS_PI / float(luminous_ao_slices);
        vec2 omega = vec2(cos(phi), sin(phi)); // on screen and in view space alike: both y up

        vec3 dir = vec3(omega, 0.0);
        vec3 ortho = dir - dot(dir, V) * V;
        vec3 axis = normalize(cross(ortho, V));
        vec3 proj_n = N - axis * dot(N, axis);
        float proj_len = length(proj_n);
        float cos_n = clamp(dot(proj_n, V) / max(proj_len, 1e-4), 0.0, 1.0);
        float n = sign(dot(ortho, proj_n)) * acos(cos_n);

        float low0 = cos(n + 0.5 * LUMINOUS_PI);
        float low1 = cos(n - 0.5 * LUMINOUS_PI);
        float horizon0 = low0;
        float horizon1 = low1;
        for (int k = 0; k < luminous_ao_steps; ++k)
        {
            float t = (float(k) + jitter) / float(luminous_ao_steps);
            t *= t; // denser near the point, where occlusion matters most
            vec2 offset = omega * max(t * radius_px, 1.0) / screen_res;

            vec3 d0 = getPositionAo(pos_screen + offset).xyz - P;
            vec3 d1 = getPositionAo(pos_screen - offset).xyz - P;
            float l0 = length(d0);
            float l1 = length(d1);
            float c0 = dot(d0, V) / max(l0, 1e-5);
            float c1 = dot(d1, V) / max(l1, 1e-5);
            // Further than the radius counts for nothing, fading in from 0.385 of it.
            float w0 = clamp((luminous_ao_radius - l0) / falloff_range, 0.0, 1.0);
            float w1 = clamp((luminous_ao_radius - l1) / falloff_range, 0.0, 1.0);
            horizon0 = max(horizon0, mix(low0, c0, w0));
            horizon1 = max(horizon1, mix(low1, c1, w1));
        }

        float h0 = -acos(clamp(horizon1, -1.0, 1.0));
        float h1 = acos(clamp(horizon0, -1.0, 1.0));
        h0 = n + clamp(h0 - n, -0.5 * LUMINOUS_PI, 0.5 * LUMINOUS_PI);
        h1 = n + clamp(h1 - n, -0.5 * LUMINOUS_PI, 0.5 * LUMINOUS_PI);
        float sin_n = sin(n);
        float arc0 = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - n)) * 0.25;
        float arc1 = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - n)) * 0.25;
        visibility += proj_len * (arc0 + arc1);
    }
    float ao = clamp(visibility / float(luminous_ao_slices), 0.0, 1.0);

    // Light bounced between surfaces (Jimenez's fit), by the albedo's luminance.
    float albedo = clamp(dot(texture(diffuseRect, pos_screen).rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    float a = 2.0404 * albedo - 0.3324;
    float b = -4.7951 * albedo + 0.6417;
    float c = 2.7552 * albedo + 0.6903;
    ao = max(ao, ((ao * a + b) * ao + c) * ao);

    return clamp(pow(ao, luminous_ao_strength), 0.0, 1.0);
}

// Linden's, below, stays for the live preview when GTAO is kept for renders;
// calcAmbientOcclusion, after it, chooses between the two.
#define calcAmbientOcclusion lindenAmbientOcclusion
#endif // LUMINOUS_GTAO

//calculate decreases in ambient lighting when crowded out (SSAO)
float calcAmbientOcclusion(vec4 pos, vec3 norm, vec2 pos_screen)
{
    float ret = 1.0;
    vec3 pos_world = pos.xyz;
    vec2 noise_reflect = texture(noiseMap, pos_screen.xy * (screen_res / 128)).xy;

    float angle_hidden = 0.0;
    float points = 0;

    float scale = min(ssao_radius / -pos_world.z, ssao_max_radius);

    // it was found that keeping # of samples a constant was the fastest, probably due to compiler optimizations (unrolling?)
    for (int i = 0; i < 8; i++)
    {
        vec2 samppos_screen = pos_screen + scale * reflect(getKern(i), noise_reflect);
        vec3 samppos_world = getPositionAo(samppos_screen).xyz;

        vec3 diff = pos_world - samppos_world;
        float dist2 = dot(diff, diff);

        // assume each sample corresponds to an occluding sphere with constant radius, constant x-sectional area
        // --> solid angle shrinking by the square of distance
        //radius is somewhat arbitrary, can approx with just some constant k * 1 / dist^2
        //(k should vary inversely with # of samples, but this is taken care of later)

        float funky_val = (dot((samppos_world - 0.05*norm - pos_world), norm) > 0.0) ? 1.0 : 0.0;
        angle_hidden = angle_hidden + funky_val * min(1.0/dist2, ssao_factor_inv);

        // 'blocked' samples (significantly closer to camera relative to pos_world) are "no data", not "no occlusion"
        float diffz_val = (diff.z > -1.0) ? 1.0 : 0.0;
        points = points + diffz_val;
    }

    angle_hidden = min(ssao_factor*angle_hidden/points, 1.0);

    float points_val = (points > 0.0) ? 1.0 : 0.0;
    ret = (1.0 - (points_val * angle_hidden));

    ret = max(ret, 0.0);
    return min(ret, 1.0);
}

#if defined(LUMINOUS_GTAO) // <Luminous>
#undef calcAmbientOcclusion
float calcAmbientOcclusion(vec4 pos, vec3 norm, vec2 pos_screen)
{
    // 2: GTAO drawn at half size live (luminousGTAOF), multiplied into the
    // light map after this pass; 1: GTAO here; 0: Linden's.
    if (luminous_gtao == 2)
    {
        return 1.0;
    }
    return luminous_gtao != 0 ? luminousGTAO(pos, norm, pos_screen) : lindenAmbientOcclusion(pos, norm, pos_screen);
}
#endif // </Luminous>
