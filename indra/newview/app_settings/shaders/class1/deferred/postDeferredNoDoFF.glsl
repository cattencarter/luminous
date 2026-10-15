/**
 * @file postDeferredNoDoFF.glsl
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;

uniform vec2 screen_res;
in vec2 vary_fragcoord;

//=================================
// borrowed noise from:
//  <https://www.shadertoy.com/view/4dS3Wd>
//  By Morgan McGuire @morgan3d, http://graphicscodex.com
//
float hash(float n) { return fract(sin(n) * 1e4); }
float hash(vec2 p) { return fract(1e4 * sin(17.0 * p.x + p.y * 0.1) * (0.1 + abs(sin(p.y * 13.0 + p.x)))); }

float noise(float x) {
    float i = floor(x);
    float f = fract(x);
    float u = f * f * (3.0 - 2.0 * f);
    return mix(hash(i), hash(i + 1.0), u);
}

float noise(vec2 x) {
    vec2 i = floor(x);
    vec2 f = fract(x);

    // Four corners in 2D of a tile
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    // Simple 2D lerp using smoothstep envelope between the values.
    // return vec3(mix(mix(a, b, smoothstep(0.0, 1.0, f.x)),
    //          mix(c, d, smoothstep(0.0, 1.0, f.x)),
    //          smoothstep(0.0, 1.0, f.y)));

    // Same code, with the clamps in smoothstep and common subexpressions
    // optimized away.
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

//=============================

vec3 clampHDRRange(vec3 color);

#ifdef LUMINOUS_UPSCALE
// <Luminous> Photo mode's live view, rendered smaller than the window and
// scaled up to it here: Catmull-Rom, which keeps edges and fine detail that
// bilinear blurs, read in nine bilinear taps (the four inner weights of each
// row and column come from one tap between the two middle texels).
vec4 luminousCatmullRom(vec2 uv)
{
    vec2 size = vec2(textureSize(diffuseRect, 0));
    vec2 pos = uv * size;
    vec2 centre = floor(pos - 0.5) + 0.5;
    vec2 f = pos - centre;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;

    vec2 t0 = (centre - 1.0) / size;
    vec2 t12 = (centre + w2 / w12) / size;
    vec2 t3 = (centre + 2.0) / size;

    vec4 c = vec4(0.0);
    c += textureLod(diffuseRect, vec2(t0.x,  t0.y),  0.0) * w0.x  * w0.y;
    c += textureLod(diffuseRect, vec2(t12.x, t0.y),  0.0) * w12.x * w0.y;
    c += textureLod(diffuseRect, vec2(t3.x,  t0.y),  0.0) * w3.x  * w0.y;
    c += textureLod(diffuseRect, vec2(t0.x,  t12.y), 0.0) * w0.x  * w12.y;
    c += textureLod(diffuseRect, vec2(t12.x, t12.y), 0.0) * w12.x * w12.y;
    c += textureLod(diffuseRect, vec2(t3.x,  t12.y), 0.0) * w3.x  * w12.y;
    c += textureLod(diffuseRect, vec2(t0.x,  t3.y),  0.0) * w0.x  * w3.y;
    c += textureLod(diffuseRect, vec2(t12.x, t3.y),  0.0) * w12.x * w3.y;
    c += textureLod(diffuseRect, vec2(t3.x,  t3.y),  0.0) * w3.x  * w3.y;
    // The negative lobes overshoot at a hard edge -- a halo round a backlit
    // silhouette -- so keep within the four texels round the point.
    ivec2 p = ivec2(centre - 0.5);
    ivec2 last = ivec2(size) - 1;
    vec4 a = texelFetch(diffuseRect, clamp(p, ivec2(0), last), 0);
    vec4 b = texelFetch(diffuseRect, clamp(p + ivec2(1, 0), ivec2(0), last), 0);
    vec4 d = texelFetch(diffuseRect, clamp(p + ivec2(0, 1), ivec2(0), last), 0);
    vec4 e = texelFetch(diffuseRect, clamp(p + ivec2(1, 1), ivec2(0), last), 0);
    return clamp(c, min(min(a, b), min(d, e)), max(max(a, b), max(d, e)));
}
#endif

void main()
{
#ifdef LUMINOUS_UPSCALE
    vec4 diff = luminousCatmullRom(vary_fragcoord.xy);
#else
    vec4 diff = texture(diffuseRect, vary_fragcoord.xy);
#endif

#ifdef HAS_NOISE
#ifdef LUMINOUS_UPSCALE
    vec2 tc = gl_FragCoord.xy*4.0; // the window's pixels, not the smaller frame's
#else
    vec2 tc = vary_fragcoord.xy*screen_res*4.0;
#endif
    vec3 seed = (diff.rgb+vec3(1.0))*vec3(tc.xy, tc.x+tc.y);
    vec3 nz = vec3(noise(seed.rg), noise(seed.gb), noise(seed.rb));
    diff.rgb += nz*0.003;
#endif

    diff.rgb = clampHDRRange(diff.rgb);
    frag_color = diff;

#ifdef LUMINOUS_UPSCALE
    // The nearest depth, not a blend across a silhouette, for the 3D
    // interface drawn over the frame.
    gl_FragDepth = texelFetch(depthMap, ivec2(vary_fragcoord.xy * vec2(textureSize(depthMap, 0))), 0).r;
#else
    gl_FragDepth = texture(depthMap, vary_fragcoord.xy).r;
#endif
}

