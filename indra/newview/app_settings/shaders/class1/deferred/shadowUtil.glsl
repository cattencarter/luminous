/**
 * @file class1/deferred/shadowUtil.glsl
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

uniform sampler2D   normalMap;

#if defined(SUN_SHADOW)
#if defined(LUMINOUS_SOFT_SHADOWS)
// <Luminous> Photo mode's soft shadows read the sun's shadow maps' depths, so
// photo mode binds them without hardware comparison
// (LLPipeline::allocateShadowBuffer) and compares them here.
uniform sampler2D shadowMap0;
uniform sampler2D shadowMap1;
uniform sampler2D shadowMap2;
uniform sampler2D shadowMap3;
#else
uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform sampler2DShadow shadowMap2;
uniform sampler2DShadow shadowMap3;
#endif
#endif

#if defined(SPOT_SHADOW)
uniform sampler2DShadow shadowMap4;
uniform sampler2DShadow shadowMap5;
#endif

#if defined(LUMINOUS_SOFT_SHADOWS)
// <Luminous> 1 draws photo mode's soft shadows; 0, in the live preview when
// they are kept for renders, Firestorm's filtering on the same maps
uniform int luminous_soft_shadows;
#endif

uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform vec2 shadow_res;
uniform vec2 proj_shadow_res;
uniform mat4 shadow_matrix[6];
uniform vec4 shadow_clip;
uniform float shadow_bias;
uniform float shadow_offset;
uniform float spot_shadow_bias;
uniform float spot_shadow_offset;
uniform mat4 inv_proj;
uniform vec2 screen_res;
uniform int sun_up_factor;
// <Luminous> How much light shadows let through, everywhere a shadow map is
// read -- the sun pass, alpha, water: 1 - the Lighting tab's Shadow strength.
// Unset, 0: shadows as Firestorm's.
uniform float luminous_shadow_lift;
// </Luminous>

float pcfShadow(sampler2DShadow shadowMap, vec3 norm, vec4 stc, float bias_mul, vec2 pos_screen, vec3 light_dir)
{
#if defined(SUN_SHADOW) && !defined(LUMINOUS_SOFT_SHADOWS) // <Luminous/> the maps are not sampler2DShadow then
    float offset = shadow_bias * bias_mul;
    stc.xyz /= stc.w;
    stc.z += offset * 2.0;
    stc.x = floor(stc.x*shadow_res.x + fract(pos_screen.y*shadow_res.y))/shadow_res.x; // add some chaotic jitter to X sample pos according to Y to disguise the snapping going on here
    float cs = texture(shadowMap, stc.xyz);
    float shadow = cs * 4.0;
    shadow += texture(shadowMap, stc.xyz+vec3( 1.5/shadow_res.x,  0.5/shadow_res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3( 0.5/shadow_res.x, -1.5/shadow_res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-1.5/shadow_res.x, -0.5/shadow_res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-0.5/shadow_res.x,  1.5/shadow_res.y, 0.0));
    return clamp(shadow * 0.125, 0.0, 1.0);
#else
    return 1.0;
#endif
}

#if defined(SUN_SHADOW) && defined(LUMINOUS_SOFT_SHADOWS)
// <Luminous> Photo mode's soft shadows: sharp where a thing meets what it
// shades, softer the further the shadow falls, as under the sun's disc.
//
// Percentage-closer soft shadows (Randima Fernando, 2005): first look for
// blockers -- shadow-map texels nearer the light than this point -- and take
// their average distance; that distance times the size of the sun's disc is
// the penumbra; then average lit-or-not over a disc of that size. Black
// Dragon's soft shadows are one fixed-size filter in shadow-map texels, so
// they are as soft at an avatar's feet as at the end of a tree's shadow, and
// change at every cascade.
//
// Linden's cascades can be perspective-warped, so the scales -- shadow-map uv
// per metre across the light, depth per metre along it -- are measured here
// through the cascade's own matrix, and the receiving surface's slope is
// followed across the disc so that it does not shadow itself.

uniform float luminous_shadow_softness; // tan of the sun's apparent radius, times Softness
uniform float luminous_frame; // an accumulated snapshot's frame, for fresh noise each; 0 otherwise

const float LUMINOUS_GOLDEN_ANGLE = 2.39996323;
uniform int luminous_blocker_taps; // 16 while setting up a shot, 32 in a snapshot
uniform int luminous_filter_taps;  // 32, or 64
const float LUMINOUS_SHADOW_REACH = 32.0;  // metres: blockers further off than this soften no more
uniform float luminous_max_texels;         // the largest disc, in shadow-map texels: 64 of the picture's maps

// The four texels a gather at uv returns, and the receiving surface's depth
// at each of their centres -- not at uv: on a sloping surface the texels a
// texel away sit nearer or further from the light, and comparing them with
// one depth draws stripes along the shadow map's rows (acne).
// slope_uv is the surface's depth per uv. Gather order: x (0,1), y (1,1),
// z (1,0), w (0,0), in texels from the lower left.
vec4 luminousGather(sampler2D map, vec2 uv, float ref, vec2 slope_uv, out vec4 refs, out vec2 f)
{
    vec2 t = uv * shadow_res - 0.5;
    vec2 i = floor(t);
    f = t - i;
    vec2 texel = 1.0 / shadow_res;
    refs = ref + vec4(dot(slope_uv, (vec2(0.0, 1.0) - f) * texel),
                      dot(slope_uv, (vec2(1.0, 1.0) - f) * texel),
                      dot(slope_uv, (vec2(1.0, 0.0) - f) * texel),
                      dot(slope_uv, (vec2(0.0, 0.0) - f) * texel));
    return textureGather(map, (i + 1.0) * texel);
}

// What one sampler2DShadow lookup gives -- four texels' lit-or-not, blended
// bilinearly -- done by hand, each against the surface at its own centre.
float luminousShadowTap(sampler2D map, vec2 uv, float ref, vec2 slope_uv)
{
    vec4 refs;
    vec2 f;
    vec4 d = luminousGather(map, uv, ref, slope_uv, refs, f);
    vec4 lit = step(refs, d); // GL_LEQUAL, as Linden's comparison
    return mix(mix(lit.w, lit.z, f.x), mix(lit.x, lit.y, f.x), f.y);
}

float luminousSunShadow(sampler2D map, mat4 smat, vec4 spos, vec3 plane_n, vec3 light_dir)
{
    vec4 c = smat * spos;
    vec3 stc = c.xyz / c.w;
    float ref = stc.z + shadow_bias * 2.0; // pcfShadow's bias

    // One metre across the light, two ways, and one toward it. The discs are
    // laid out in metres across the light, where the sun's disc is round, and
    // mapped into the shadow map through uv_per_m.
    vec3 t1 = normalize(cross(light_dir, abs(light_dir.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3 t2 = cross(light_dir, t1);
    vec4 a = smat * (spos + vec4(t1, 0.0));
    vec4 b = smat * (spos + vec4(t2, 0.0));
    vec4 l = smat * (spos + vec4(light_dir, 0.0));
    mat2 uv_per_m = mat2(a.xy / a.w - stc.xy, b.xy / b.w - stc.xy);
    float depth_per_m = stc.z - l.z / l.w; // nearer the light is less deep
    float scale = sqrt(abs(determinant(uv_per_m)));
    if (scale <= 0.0 || depth_per_m <= 0.0)
    {
        return luminousShadowTap(map, stc.xy, ref, vec2(0.0));
    }

    // The surface's depth across the disc: a step of w across the light meets
    // the surface (w.N)/(L.N) metres nearer the light. N is the surface's own
    // (plane_n), not the shading normal with its normal map. Steep slopes are
    // limited, as at a grazing sun the surface is barely lit anyway.
    float ndl = max(dot(plane_n, light_dir), 0.2);
    vec2 slope = clamp(vec2(dot(t1, plane_n), dot(t2, plane_n)) / ndl, vec2(-4.0), vec2(4.0)) * depth_per_m;
    vec2 slope_uv = transpose(inverse(uv_per_m)) * slope; // the same, per uv

    float texel_m = 1.0 / (max(shadow_res.x, shadow_res.y) * scale);
    // A random turn of the discs per screen pixel (Dave Hoskins's hash). Not
    // interleaved gradient noise: that is made for games that average it over
    // frames, and in one frame it lines up into diagonal stripes across a
    // penumbra. Not keyed to the shadow map either: in a close-up, one key
    // spans many pixels and the penumbra comes out in square blocks.
    vec3 h = fract(vec3((gl_FragCoord.xy + luminous_frame * vec2(37.0, 113.0)).xyx) * 0.1031);
    h += dot(h, h.yzx + 33.33);
    float rotation = 6.2831853 * fract((h.x + h.y) * h.z);

    float max_texels = luminous_max_texels > 0.0 ? luminous_max_texels : 64.0;
    float search = clamp(LUMINOUS_SHADOW_REACH * luminous_shadow_softness, 2.0 * texel_m, max(max_texels, 2.0) * texel_m);
    float blocker_depth = 0.0;
    float blockers = 0.0;
    for (int i = 0; i < luminous_blocker_taps; ++i)
    {
        float angle = float(i) * LUMINOUS_GOLDEN_ANGLE + rotation;
        vec2 w = vec2(cos(angle), sin(angle)) * sqrt((float(i) + 0.5) / float(luminous_blocker_taps)) * search;
        vec4 refs;
        vec2 f;
        vec4 d = luminousGather(map, stc.xy + uv_per_m * w, ref + dot(slope, w), slope_uv, refs, f);
        vec4 near = vec4(lessThan(d, refs));
        blocker_depth += dot(near, d);
        blockers += dot(near, vec4(1.0));
    }
    if (blockers < 0.5)
    {
        return 1.0;
    }
    if (blockers > 4.0 * float(luminous_blocker_taps) - 0.5)
    {
        return 0.0; // the whole reach is shaded, and the penumbra is no larger
    }
    float distance_m = max(ref - blocker_depth / blockers, 0.0) / depth_per_m;
    float radius = clamp(distance_m * luminous_shadow_softness, texel_m, search);

    float lit = 0.0;
    for (int i = 0; i < luminous_filter_taps; ++i)
    {
        float angle = float(i) * LUMINOUS_GOLDEN_ANGLE + rotation;
        vec2 w = vec2(cos(angle), sin(angle)) * sqrt((float(i) + 0.5) / float(luminous_filter_taps)) * radius;
        lit += luminousShadowTap(map, stc.xy + uv_per_m * w, ref + dot(slope, w), slope_uv);
    }
    return lit / float(luminous_filter_taps);
}

// Firestorm's pcfShadow, with each comparison done by hand: the live
// preview's shadows while soft shadows are kept for renders.
float luminousHardShadow(sampler2D map, vec4 stc, vec2 pos_screen)
{
    stc.xyz /= stc.w;
    stc.z += shadow_bias * 2.0;
    stc.x = floor(stc.x*shadow_res.x + fract(pos_screen.y*shadow_res.y))/shadow_res.x;
    float shadow = luminousShadowTap(map, stc.xy, stc.z, vec2(0.0)) * 4.0;
    shadow += luminousShadowTap(map, stc.xy + vec2( 1.5,  0.5) / shadow_res, stc.z, vec2(0.0));
    shadow += luminousShadowTap(map, stc.xy + vec2( 0.5, -1.5) / shadow_res, stc.z, vec2(0.0));
    shadow += luminousShadowTap(map, stc.xy + vec2(-1.5, -0.5) / shadow_res, stc.z, vec2(0.0));
    shadow += luminousShadowTap(map, stc.xy + vec2(-0.5,  1.5) / shadow_res, stc.z, vec2(0.0));
    return clamp(shadow * 0.125, 0.0, 1.0);
}

float luminousCascadeShadow(sampler2D map, mat4 smat, vec4 spos, vec4 lpos, vec3 plane_n, vec3 light_dir, vec2 pos_screen)
{
    return luminous_soft_shadows != 0 ? luminousSunShadow(map, smat, spos, plane_n, light_dir)
                                      : luminousHardShadow(map, lpos, pos_screen);
}
#endif

float pcfSpotShadow(sampler2DShadow shadowMap, vec4 stc, float bias_scale, vec2 pos_screen)
{
#if defined(SPOT_SHADOW) && defined(LUMINOUS_SOFT_SHADOWS)
    // <Luminous> Photo mode: Black Dragon's 4x4 filter (after The Witness)
    // on projector shadows, in place of Linden's snapped five taps. Both spot
    // maps share proj_shadow_res, so the size is right for each. Linden's
    // below in the live preview while soft shadows are kept for renders.
    if (luminous_soft_shadows != 0)
    {
        stc.xyz /= stc.w;
        float depth = stc.z + spot_shadow_bias * bias_scale;
        vec2 inv = 1.0 / proj_shadow_res;
        vec2 uv = stc.xy * proj_shadow_res;
        vec2 base = floor(uv + 0.5);
        float s = uv.x + 0.5 - base.x;
        float t = uv.y + 0.5 - base.y;
        base = (base - 0.5) * inv;

        vec3 uw = vec3(4.0 - 3.0 * s, 7.0, 1.0 + 3.0 * s);
        vec3 uo = vec3((3.0 - 2.0 * s) / uw.x - 2.0, (3.0 + s) / uw.y, s / uw.z + 2.0);
        vec3 vw = vec3(4.0 - 3.0 * t, 7.0, 1.0 + 3.0 * t);
        vec3 vo = vec3((3.0 - 2.0 * t) / vw.x - 2.0, (3.0 + t) / vw.y, t / vw.z + 2.0);

        float sum = 0.0;
        for (int j = 0; j < 3; ++j)
        {
            for (int i = 0; i < 3; ++i)
            {
                sum += uw[i] * vw[j] * texture(shadowMap, vec3(base + vec2(uo[i], vo[j]) * inv, depth));
            }
        }
        return sum / 144.0;
    }
#endif
#if defined(SPOT_SHADOW)
    stc.xyz /= stc.w;
    stc.z += spot_shadow_bias * bias_scale;
    stc.x = floor(proj_shadow_res.x * stc.x + fract(pos_screen.y*0.666666666)) / proj_shadow_res.x; // snap

    float cs = texture(shadowMap, stc.xyz);
    float shadow = cs;

    vec2 off = 1.0/proj_shadow_res;
    off.y *= 1.5;

    shadow += texture(shadowMap, stc.xyz+vec3(off.x*2.0, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(off.x, -off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x*2.0, -off.y, 0.0));
    return shadow*0.2;
#else
    return 1.0;
#endif
}

float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen)
{
#if defined(SUN_SHADOW)
    float shadow = 0.0f;
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);

    float dp_directional_light = max(0.0, dot(norm.xyz, light_dir));
          dp_directional_light = clamp(dp_directional_light, 0.0, 1.0);

    vec3 shadow_pos = pos.xyz;

    vec3 offset = light_dir.xyz * (1.0 - dp_directional_light);

    shadow_pos += offset * shadow_offset * 2.0;

    vec4 spos = vec4(shadow_pos.xyz, 1.0);

#if defined(LUMINOUS_SOFT_SHADOWS)
    // <Luminous> The receiving surface's own normal, from its position across
    // neighbouring pixels: the shading normal carries the normal map's bumps,
    // which would tilt the soft shadows' receiver plane pixel by pixel. Where
    // the two disagree -- across an edge, or with no neighbours to compare --
    // the shading normal stands in.
    vec3 plane_n = cross(dFdx(pos), dFdy(pos));
    float plane_len = length(plane_n);
    plane_n = plane_len > 1e-12 ? plane_n / plane_len : norm;
    if (dot(plane_n, norm) < 0.0)
    {
        plane_n = -plane_n;
    }
    if (!(dot(plane_n, norm) > 0.5))
    {
        plane_n = norm;
    }
#endif

    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;
        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        if (spos.z < near_split.z)
        {
            lpos = shadow_matrix[3]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.z, 0.0)/transition_domain.z;
            //w = clamp(w, 0.0, 1.0);
#if defined(LUMINOUS_SOFT_SHADOWS)
            float contrib = luminousCascadeShadow(shadowMap3, shadow_matrix[3], spos, lpos, plane_n, light_dir, pos_screen)*w; // <Luminous/>
#else
            float contrib = pcfShadow(shadowMap3, norm, lpos, 1.0, pos_screen, light_dir)*w;
#endif
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
        }

        if (spos.z < near_split.y && spos.z > far_split.z)
        {
            lpos = shadow_matrix[2]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.y, 0.0)/transition_domain.y;
            w -= max(near_split.z-spos.z, 0.0)/transition_domain.z;
            //w = clamp(w, 0.0, 1.0);
#if defined(LUMINOUS_SOFT_SHADOWS)
            float contrib = luminousCascadeShadow(shadowMap2, shadow_matrix[2], spos, lpos, plane_n, light_dir, pos_screen)*w; // <Luminous/>
#else
            float contrib = pcfShadow(shadowMap2, norm, lpos, 1.0, pos_screen, light_dir)*w;
#endif
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z < near_split.x && spos.z > far_split.y)
        {
            lpos = shadow_matrix[1]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.x, 0.0)/transition_domain.x;
            w -= max(near_split.y-spos.z, 0.0)/transition_domain.y;
            //w = clamp(w, 0.0, 1.0);
#if defined(LUMINOUS_SOFT_SHADOWS)
            float contrib = luminousCascadeShadow(shadowMap1, shadow_matrix[1], spos, lpos, plane_n, light_dir, pos_screen)*w; // <Luminous/>
#else
            float contrib = pcfShadow(shadowMap1, norm, lpos, 1.0, pos_screen, light_dir)*w;
#endif
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z > far_split.x)
        {
            lpos = shadow_matrix[0]*spos;

            float w = 1.0;
            w -= max(near_split.x-spos.z, 0.0)/transition_domain.x;
            //w = clamp(w, 0.0, 1.0);
#if defined(LUMINOUS_SOFT_SHADOWS)
            float contrib = luminousCascadeShadow(shadowMap0, shadow_matrix[0], spos, lpos, plane_n, light_dir, pos_screen)*w; // <Luminous/>
#else
            float contrib = pcfShadow(shadowMap0, norm, lpos, 1.0, pos_screen, light_dir)*w;
#endif
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        shadow /= weight;
        // <Luminous> The last cascade fades its shadows out by adding to the
        // sum but not to the weight, so near the far split the light comes
        // out above full and falls back to 1 past it: a line across water at
        // the draw distance. Never more than lit.
        shadow = min(shadow, 1.0);
    }
    else
    {
        return 1.0f; // lit beyond the far split...
    }
    //shadow = min(dp_directional_light,shadow);
    return mix(shadow, 1.0, luminous_shadow_lift); // <Luminous/> Shadow strength
#else
    return 1.0;
#endif
}

float sampleSpotShadow(vec3 pos, vec3 norm, int index, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    float shadow = 0.0f;
    pos += norm * spot_shadow_offset;

    vec4 spos = vec4(pos,1.0);
    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;

        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        {
            float w = 1.0;
            w -= max(spos.z-far_split.z, 0.0)/transition_domain.z;

            if (index == 0)
            {
                lpos = shadow_matrix[4]*spos;
                shadow += pcfSpotShadow(shadowMap4, lpos, 0.8, spos.xy)*w;
            }
            else
            {
                lpos = shadow_matrix[5]*spos;
                shadow += pcfSpotShadow(shadowMap5, lpos, 0.8, spos.xy)*w;
            }
            weight += w;
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
        }

        shadow /= weight;
    }
    else
    {
        shadow = 1.0f;
    }
    return mix(shadow, 1.0, luminous_shadow_lift); // <Luminous/> Shadow strength
#else
    return 1.0;
#endif
}

