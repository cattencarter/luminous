/**
 * @file class2/deferred/sunLightSSAOF.glsl
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

//class 2 -- shadows and SSAO

// Inputs
in vec2 vary_fragcoord;

vec4 getPosition(vec2 pos_screen);
vec4 getNorm(vec2 pos_screen);

float sampleDirectionalShadow(vec3 shadow_pos, vec3 norm, vec2 pos_screen);
float sampleSpotShadow(vec3 shadow_pos, vec3 norm, int index, vec2 pos_screen);
float calcAmbientOcclusion(vec4 pos, vec3 norm, vec2 pos_screen);

// <Luminous> Contact shadows: from the point toward the sun or moon, a short
// march on screen; a surface in front of the ray, but not far in front (so
// not something merely nearer the camera), shades the point. The shadow maps
// are too coarse for where things touch -- fingers on skin, feet on a floor
// -- and this fills that in. luminous_contact_length 0 leaves it out.
uniform float luminous_contact_length; // metres along the ray; 0 off
uniform mat4 inv_proj;                 // the camera's, as the pipeline sets it for this pass
uniform float luminous_contact_seed;   // an accumulated render's frame: fresh noise in each
uniform int luminous_contact_steps;
uniform float luminous_contact_strength; // how dark at most: 1 none of the light through
uniform mat4 projection_matrix;         // the camera's, set with inv_proj: no inverse per pixel
uniform vec3 luminous_contact_light;   // toward the sun or moon, whichever casts shadows; eye space
uniform int luminous_contact_all;      // a rendered picture: surfaces facing away from the light too

float luminousContactHash(vec2 p)
{
    vec3 h = fract(vec3(p.xyx) * 0.1031);
    h += dot(h, h.yzx + 33.33);
    return fract((h.x + h.y) * h.z);
}

uniform vec2 screen_res;             // the frame's size, as the pipeline sets it for this pass

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
    vec3 l = normalize(luminous_contact_light);
    // Facing away, the shadow maps should have it -- but where something
    // rests on the ground they leave a lit rim along its underside, and the
    // shadow still darkens more than the direct light there. Live that is
    // left to them, for the frame rate; a picture marches here too.
    if (luminous_contact_all == 0 && dot(norm, l) <= 0.0)
    {
        return 1.0;
    }
    float len = luminous_contact_length;
    int steps = max(luminous_contact_steps, 4);
    float jitter = luminousContactHash(gl_FragCoord.xy + luminous_contact_seed * vec2(37.0, 113.0));
    vec3 start = pos + luminousContactFacing(pos, tc) * (0.0005 * -pos.z + 0.001);
    // how deep a surface in front of the ray may be and still be the thing
    // in the way: at a third of that, a small object's shadow breaks into
    // pieces. With the light behind what is shown, though, the ray runs away
    // from the camera, behind every surface it crosses, and at that depth
    // all of them count: a head shades the shoulders below it, and a lit
    // streak runs from any gap where the ray sees the sky. So there only a
    // surface a few centimetres in front of the ray counts: what touches.
    float thickness = mix(max(0.1, 2.0 * len), 0.04, smoothstep(0.5, 0.9, -l.z));
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
    vec2 pos_screen = vary_fragcoord.xy;
    vec4 pos  = getPosition(pos_screen);
    vec4 norm = getNorm(pos_screen);

    vec4 col;
    col.r = sampleDirectionalShadow(pos.xyz, norm.xyz, pos_screen);
    // <Luminous> Contact shadows only where the sun still reaches: under a
    // roof its shadow map has already shut it out, and min() would keep 0.
    if (col.r > 0.0)
    {
        col.r = min(col.r, luminousContactShadow(pos.xyz, norm.xyz, pos_screen));
    }
    // </Luminous>
    col.g = calcAmbientOcclusion(pos, norm.xyz, pos_screen);
    col.b = sampleSpotShadow(pos.xyz, norm.xyz, 0, pos_screen);
    col.a = sampleSpotShadow(pos.xyz, norm.xyz, 1, pos_screen);

    frag_color = clamp(col, vec4(0), vec4(1));
}
