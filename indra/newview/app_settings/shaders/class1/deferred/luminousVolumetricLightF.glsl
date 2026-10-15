/**
 * @file luminousVolumetricLightF.glsl
 * @brief Photo mode: volumetric light, as single scattering in the sun's shadow map.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * The idea, marching the view ray through the sun's shadow map, is from
 * volumetricLightF.glsl in the Black Dragon Viewer by NiranV Dean.
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

// The light that air scatters toward the camera, wherever the sun (or the moon,
// at night) actually reaches it. Drawn with additive blending straight into the
// HDR frame, before exposure and tonemapping, so it is light like any other.
//
// Per pixel, marching the view ray in metres:
//
//     light = sunlit * density * phase * sum over the ray of visible * transmittance * step
//
// Shaded air adds nothing and sunlit air adds light, and that contrast is the
// beam. The phase function makes it strongest looking toward the sun, as haze
// is, so no "facing the sun" switch is needed.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform int luminous_vol_samples;
uniform float luminous_vol_density;      // scattering per metre
uniform float luminous_vol_anisotropy;   // 0 even in every direction, toward 1 a tight glow around the sun
uniform float luminous_vol_distance;     // metres

uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int sun_up_factor;

#if defined(SUN_SHADOW)
#if defined(LUMINOUS_SOFT_SHADOWS)
// Soft shadows bind the sun's maps without hardware comparison (shadowUtil.glsl).
uniform sampler2D shadowMap0;
uniform sampler2D shadowMap1;
uniform sampler2D shadowMap2;
uniform sampler2D shadowMap3;
#define luminousShadowLookup(map, stc) step(stc.z + shadow_bias, texture(map, stc.xy).r)
#else
uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform sampler2DShadow shadowMap2;
uniform sampler2DShadow shadowMap3;
#define luminousShadowLookup(map, stc) texture(map, vec3(stc.xy, stc.z + shadow_bias))
#endif
uniform mat4 shadow_matrix[6];
uniform vec4 shadow_clip;
uniform float shadow_bias;
#endif

vec4 getPosition(vec2 pos_screen);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten);

#if defined(SUN_SHADOW)
// Along the view ray, each cascade's homogeneous shadow coordinates are a
// straight line in the distance t: worked out once per pixel (main), so a
// sample costs one multiply-add instead of a matrix product.
vec4 luminous_ray_start[4]; // a cascade's coordinates at the eye
vec4 luminous_ray_dir[4];   // and their change per metre along the ray

// One cascade's coordinates at distance t, and whether they land inside it.
bool luminousShadowCoord(int cascade, float t, out vec3 stc)
{
    vec4 lpos = luminous_ray_start[cascade] + luminous_ray_dir[cascade] * t;
    stc = lpos.xyz / lpos.w;
    return all(greaterThanEqual(stc, vec3(0.0))) && all(lessThanEqual(stc, vec3(1.0)));
}
#endif

// How much sun reaches the point t metres along the ray, at view depth dist:
// 1 lit, 0 blocked. The nearest cascade that covers the point answers. The
// march stops at the last cascade, so the fallback only catches a point
// between cascades' frusta.
float luminousSunVisibility(float t, float dist)
{
#if defined(SUN_SHADOW)
    vec3 stc;
    if (dist < shadow_clip.x && luminousShadowCoord(0, t, stc))
    {
        return luminousShadowLookup(shadowMap0, stc);
    }
    if (dist < shadow_clip.y && luminousShadowCoord(1, t, stc))
    {
        return luminousShadowLookup(shadowMap1, stc);
    }
    if (dist < shadow_clip.z && luminousShadowCoord(2, t, stc))
    {
        return luminousShadowLookup(shadowMap2, stc);
    }
    if (dist < shadow_clip.w && luminousShadowCoord(3, t, stc))
    {
        return luminousShadowLookup(shadowMap3, stc);
    }
#endif
    return 1.0;
}

// Henyey-Greenstein: how much light arriving along the light's direction is
// scattered toward the eye, by the angle between them.
float luminousPhase(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cos_theta, 1e-4);
    return (1.0 - g2) / (4.0 * 3.14159265 * denom * sqrt(denom));
}

// Interleaved gradient noise (Jimenez 2014): where along its ray each pixel
// starts sampling. Neighbours differ, so a coarse step reads as fine grain
// rather than as bands.
uniform float luminous_frame; // an accumulated snapshot's frame: fresh noise in each, so averaging smooths it

float luminousJitter(vec2 frag)
{
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec3 pos = getPosition(tc).xyz;

    float dist = length(pos);
    vec3 view_dir = pos / max(dist, 1e-4);

    // Only as far as the shadow map reaches: past it there is no telling lit
    // air from shaded, and the sky's own haze already covers the distance.
    float march = min(dist, luminous_vol_distance);
#if defined(SUN_SHADOW)
    march = min(march, shadow_clip.w);
#endif

    vec3 light_dir = (sun_up_factor == 1) ? sun_dir : moon_dir;

    // The sunlight that lands on surfaces here, so the beams match it: same
    // colour, same reddening near the horizon, same scale.
    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVarsLinear(pos, light_dir, light_dir, sunlit, amblit, additive, atten);

    int samples = max(luminous_vol_samples, 1);
    float step_len = march / float(samples);
    float jitter = luminousJitter(gl_FragCoord.xy + luminous_frame * vec2(37.0, 113.0));

#if defined(SUN_SHADOW)
    for (int c = 0; c < 4; ++c)
    {
        luminous_ray_start[c] = shadow_matrix[c][3];
        luminous_ray_dir[c] = shadow_matrix[c] * vec4(view_dir, 0.0);
    }
#endif
    // The transmittance to each sample, multiplied along rather than an
    // exp() each: the steps are even.
    float transmittance = exp(-luminous_vol_density * jitter * step_len);
    float step_transmittance = exp(-luminous_vol_density * step_len);
    float depth_per_metre = -view_dir.z;

    float lit = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        float t = (float(i) + jitter) * step_len;
        lit += luminousSunVisibility(t, t * depth_per_metre) * transmittance;
        transmittance *= step_transmittance;
    }
    lit *= step_len;

    float phase = luminousPhase(dot(view_dir, light_dir), luminous_vol_anisotropy);
    vec3 light = sunlit * (luminous_vol_density * phase * lit);

    frag_color = vec4(max(light, vec3(0.0)), 0.0);
}
