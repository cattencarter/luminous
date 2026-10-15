/**
 * @file class3\deferred\spotLightF.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

uniform samplerCube environmentMap;
uniform sampler2D lightMap;
uniform sampler2D lightFunc;
// <Luminous> GTAO on this light too, a share of it (LuminousPhotoAOLights,
// 0 as Second Life has it): the sun pass's ambient occlusion is the light
// map's green.
uniform float luminous_ao_lights;
// </Luminous>

uniform mat4 proj_mat; //screen space to light space
uniform float proj_near; //near clip for projection
uniform vec3 proj_p; //plane projection is emitting from (in screen space)
uniform vec3 proj_n;
uniform float proj_focus; //distance from plane to begin blurring
uniform float proj_lod;  //(number of mips in proj map)
uniform float proj_range; //range between near clip and far clip plane of projection
uniform float proj_ambient_lod;
uniform float proj_ambiance;
uniform float near_clip;
uniform float far_clip;

uniform vec3 proj_origin; //origin of projection to be used for angular attenuation
uniform float sun_wash;
uniform int proj_shadow_idx;
uniform float shadow_fade;
uniform int classic_mode;

// Light params
#if defined(MULTI_SPOTLIGHT)
uniform vec3 center;
#else
in vec3 trans_center;
#endif
uniform float size;
uniform vec3 color;
uniform float falloff;

in vec4 vary_fragcoord;
uniform vec2 screen_res;

uniform mat4 inv_proj;

void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
float calcLegacyDistanceAttenuation(float distance, float falloff);
bool clipProjectedLightVars(vec3 center, vec3 pos, out float dist, out float l_dist, out vec3 lv, out vec4 proj_tc );
vec4 getNorm(vec2 screenpos);
vec3 getProjectedLightAmbiance(float amb_da, float attenuation, float lit, float nl, float noise, vec2 projected_uv);
vec3 getProjectedLightDiffuseColor(float light_distance, vec2 projected_uv );
vec2 getScreenCoord(vec4 clip);
vec3 srgb_to_linear(vec3 cs);
vec4 texture2DLodSpecular(vec2 tc, float lod);

vec4 getPosition(vec2 pos_screen);

const float M_PI = 3.14159265;

void pbrPunctual(vec3 diffuseColor, vec3 specularColor,
                    float perceptualRoughness,
                    float metallic,
                    vec3 n, // normal
                    vec3 v, // surface point to camera
                    vec3 l, // surface point to light
                    out float nl,
                    out vec3 diff,
                    out vec3 spec);

GBufferInfo getGBuffer(vec2 screenpos);

// <Luminous> Contact shadows for projectors, as sunLightF.glsl draws them for
// the sun and moon: from the point toward the projector, a short march on
// screen; a surface in front of the ray, but not far in front (so not
// something merely nearer the camera), shades the point. Two projectors at
// most have shadow maps, coarse ones, and the rest cast no shadow at all, so
// fingers on skin or a chin over a throat show here more than anywhere.
// luminous_contact_length 0 leaves it out (the live view as Luminous ships,
// reflection probes).
uniform float luminous_contact_length; // metres along the ray; 0 off
uniform float luminous_contact_seed;   // an accumulated render's frame: fresh noise in each
uniform int luminous_contact_steps;
uniform float luminous_contact_strength; // how dark at most: 1 none of the light through
uniform mat4 projection_matrix;         // the camera's, set with inv_proj: no inverse per pixel

float luminousContactHash(vec2 p)
{
    vec3 h = fract(vec3(p.xyx) * 0.1031);
    h += dot(h, h.yzx + 33.33);
    return fract((h.x + h.y) * h.z);
}

// The surface's own facing, from its neighbours' positions -- the nearer one
// each way, so not across an edge. The shading normal tilts with a normal
// map, and a ray started along it can begin below the surface; started along
// this one it can begin a millimetre off it, so a shadow reaches right into
// a crease, such as an arm against the side.
vec3 luminousContactFacing(vec3 pos, vec2 tc)
{
    vec2 px = 1.0 / screen_res;
    vec3 left = getPosition(tc - vec2(px.x, 0.0)).xyz;
    vec3 right = getPosition(tc + vec2(px.x, 0.0)).xyz;
    vec3 down = getPosition(tc - vec2(0.0, px.y)).xyz;
    vec3 up = getPosition(tc + vec2(0.0, px.y)).xyz;
    vec3 dx = abs(right.z - pos.z) < abs(pos.z - left.z) ? right - pos : pos - left;
    vec3 dy = abs(up.z - pos.z) < abs(pos.z - down.z) ? up - pos : pos - down;
    vec3 n = normalize(cross(dx, dy));
    return dot(n, pos) > 0.0 ? -n : n; // toward the camera
}

float luminousContactShadow(vec3 pos, vec3 norm, vec2 tc)
{
    if (luminous_contact_length <= 0.0 || pos.z < -48.0)
    {
        return 1.0;
    }
    vec3 l = normalize(proj_origin - pos);
    if (dot(norm, l) <= 0.0)
    {
        return 1.0; // facing away: unlit by this projector anyway
    }
    // Not as far as the lamp's face (proj_p, proj_n): its own prim, and the
    // apex behind it, would shadow everything the projector lights.
    float plane = dot(pos - proj_p, proj_n); // metres in front of the face
    float approach = dot(l, -proj_n);
    if (plane <= 0.0 || approach <= 0.0)
    {
        return 1.0;
    }
    float len = min(luminous_contact_length, 0.8 * plane / approach);
    if (len < 0.01)
    {
        return 1.0;
    }
    int steps = max(luminous_contact_steps, 4);
    float jitter = luminousContactHash(gl_FragCoord.xy + luminous_contact_seed * vec2(37.0, 113.0));
    vec3 start = pos + luminousContactFacing(pos, tc) * (0.0005 * -pos.z + 0.001);
    float thickness = max(0.1, 2.0 * len);
    // The ray on screen is a straight line in clip space: its two ends'
    // projections once, not a matrix product for each step.
    vec4 c_start = projection_matrix * vec4(start, 1.0);
    vec4 c_step = projection_matrix * vec4(l * len, 0.0);
    float occlusion = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        float t = (float(i) + jitter) / float(steps);
        vec3 p = start + l * (t * len);
        vec4 c = c_start + c_step * t;
        if (c.w <= 0.0)
        {
            break;
        }
        vec2 uv = c.xy / c.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        {
            break;
        }
        vec3 surface = getPosition(uv).xyz;
        float in_front = surface.z - p.z; // > 0: a surface nearer the camera than the ray
        float bias = 0.0005 * -p.z + 0.0005; // depth precision only: the ray starts off the surface itself
        // How surely that surface is what stands in the way, as a weight, not
        // yes or no: a test right at its limits shades some pixels and not
        // their neighbours, dotting the outline of anything held a little in
        // front of a surface. In front of the ray, but not by more than the
        // thickness; and near the point itself -- something touching or
        // nearly, not a nearer object whose outline the ray merely passes
        // behind.
        float hit = smoothstep(bias, 2.0 * bias, in_front)
                  * (1.0 - smoothstep(0.5 * thickness, thickness, in_front))
                  * (1.0 - smoothstep(len, 2.0 * len, distance(surface, pos)));
        // softer toward the end of the ray
        occlusion = max(occlusion, hit * (1.0 - t * t));
        if (occlusion > 0.99)
        {
            break;
        }
    }
    // gone by 48 m
    return 1.0 - occlusion * luminous_contact_strength * (1.0 - smoothstep(24.0, 48.0, -pos.z));
}
// </Luminous>

void main()
{
    vec3 final_color = vec3(0,0,0);
    vec2 tc          = getScreenCoord(vary_fragcoord);
    vec3 pos         = getPosition(tc).xyz;

    vec3 lv;
    vec4 proj_tc;
    float dist, l_dist;
    vec3 c;
#if defined(MULTI_SPOTLIGHT)
    c = center;
#else
    c = trans_center;
#endif

    if (clipProjectedLightVars(c, pos, dist, l_dist, lv, proj_tc))
    {
        discard;
    }

    float shadow = 1.0;

    if (proj_shadow_idx >= 0)
    {
        vec4 shd = texture(lightMap, tc);
        shadow = (proj_shadow_idx==0)?shd.b:shd.a;
        shadow += shadow_fade;
        shadow = clamp(shadow, 0.0, 1.0);
    }

    GBufferInfo gb = getGBuffer(tc);

    vec3 n = gb.normal;

    float dist_atten = calcLegacyDistanceAttenuation(dist, falloff);
    if (dist_atten <= 0.0)
    {
        discard;
    }

    lv = proj_origin-pos.xyz;
    vec3  h, l, v = -normalize(pos);
    float nh, nl, nv, vh, lightDist;
    calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);
    // <Luminous> Contact shadows once the light is known to reach the point
    // (in range, facing the projector), and where its shadow map has left
    // any light: elsewhere they could change nothing.
    if (nl > 0.0 && shadow > 0.0)
    {
        shadow = min(shadow, luminousContactShadow(pos, n, tc));
    }
    // </Luminous>

    vec3 diffuse = gb.albedo.rgb;
    vec4 spec    = gb.specular;
    vec3 dlit    = vec3(0, 0, 0);
    vec3 slit    = vec3(0, 0, 0);

    vec3 amb_rgb = vec3(0);

    if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
    {
        vec3 orm = spec.rgb;
        float perceptualRoughness = orm.g;
        float metallic = orm.b;
        vec3 f0 = vec3(0.04);
        vec3 baseColor = diffuse.rgb;

        vec3 diffuseColor = baseColor.rgb*(vec3(1.0)-f0);
        diffuseColor *= 1.0 - metallic;

        vec3 specularColor = mix(f0, baseColor.rgb, metallic);
        vec3 diffPunc = vec3(0);
        vec3 specPunc = vec3(0);

        // We need this additional test inside a light's frustum since a spotlight's ambiance can be applied
        if (proj_tc.x > 0.0 && proj_tc.x < 1.0
        &&  proj_tc.y > 0.0 && proj_tc.y < 1.0)
        {
            float lit = 0.0;
            float amb_da = 0.0;

            lv = normalize(lv);

            if (nl > 0.0)
            {
                amb_da += (nl*0.5 + 0.5) * proj_ambiance;

                dlit = getProjectedLightDiffuseColor( l_dist, proj_tc.xy );

                vec3 intensity = dist_atten * dlit * 3.25 * shadow; // Legacy attenuation, magic number to balance with legacy materials

                pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, n.xyz, v, normalize(lv), nl, diffPunc, specPunc);

                final_color += intensity * clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10));
            }

            amb_rgb = getProjectedLightAmbiance( amb_da, dist_atten, lit, nl, 1.0, proj_tc.xy ) * 3.25; //magic number to balance with legacy ambiance
            pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, n.xyz, v, normalize(lv), nl, diffPunc, specPunc);

            final_color += amb_rgb * clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10));
        }
    }
    else
    {
        float envIntensity = gb.envIntensity;

        diffuse = srgb_to_linear(diffuse);
        spec.rgb = srgb_to_linear(spec.rgb);

        if (proj_tc.z > 0.0 &&
            proj_tc.x < 1.0 &&
            proj_tc.y < 1.0 &&
            proj_tc.x > 0.0 &&
            proj_tc.y > 0.0)
        {
            float amb_da = 0;
            float lit = 0.0;

            if (nl > 0.0)
            {
                lit = nl * dist_atten;

                dlit = getProjectedLightDiffuseColor( l_dist, proj_tc.xy );

                final_color = dlit*lit*diffuse*shadow;

                // unshadowed for consistency between forward and deferred?
                amb_da += (nl*0.5+0.5) /* * (1.0-shadow) */ * proj_ambiance;
            }

            amb_rgb = getProjectedLightAmbiance( amb_da, dist_atten, lit, nl, 1.0, proj_tc.xy );
            final_color += diffuse.rgb * amb_rgb * max(dot(-normalize(lv), n), 0.0);
        }

        if (spec.a > 0.0)
        {
            dlit *= min(nl*6.0, 1.0) * dist_atten;

            float fres = pow(1 - vh, 5)*0.4+0.5;

            float gtdenom = 2 * nh;
            float gt = max(0, min(gtdenom * nv / vh, gtdenom * nl / vh));

            if (nh > 0.0)
            {
                float scol = fres*texture(lightFunc, vec2(nh, spec.a)).r*gt/(nh*nl);
                vec3 speccol = dlit*scol*spec.rgb*shadow;
                speccol = clamp(speccol, vec3(0), vec3(1));
                final_color += speccol;
            }
        }

        if (envIntensity > 0.0)
        {
            vec3 ref = reflect(normalize(pos), n);

            //project from point pos in direction ref to plane proj_p, proj_n
            vec3 pdelta = proj_p-pos;
            float ds = dot(ref, proj_n);

            if (ds < 0.0)
            {
                vec3 pfinal = pos + ref * dot(pdelta, proj_n)/ds;

                vec4 stc = (proj_mat * vec4(pfinal.xyz, 1.0));

                if (stc.z > 0.0)
                {
                    stc /= stc.w;

                    if (stc.x < 1.0 &&
                        stc.y < 1.0 &&
                        stc.x > 0.0 &&
                        stc.y > 0.0)
                    {
                        final_color += color.rgb * texture2DLodSpecular(stc.xy, (1 - spec.a) * (proj_lod * 0.6)).rgb * shadow * envIntensity;
                    }
                }
            }
        }
    }

    //not sure why, but this line prevents MATBUG-194
    final_color = max(final_color, vec3(0.0));
    // <Luminous>
    final_color *= mix(1.0, texture(lightMap, tc).g, luminous_ao_lights);
    // </Luminous>
    float final_scale = 1.0;
    if (classic_mode > 0)
        final_scale = 0.9;
    //output linear
    frag_color.rgb = final_color * final_scale;
    frag_color.a = 0.0;
}
