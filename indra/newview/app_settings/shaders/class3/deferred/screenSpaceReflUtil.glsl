/**
 * @file class3/deferred/screenSpaceReflUtil.glsl
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

uniform sampler2D sceneMap;
uniform sampler2D sceneDepth;

uniform vec2 screen_res;
uniform mat4 projection_matrix;
//uniform float zNear;
//uniform float zFar;
uniform mat4 inv_proj;
uniform mat4 modelview_delta;  // should be transform from last camera space to current camera space
uniform mat4 inv_modelview_delta;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);

float random (vec2 uv)
{
    return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453123); //simple random function
}

// Based off of https://github.com/RoundedGlint585/ScreenSpaceReflection/
// A few tweaks here and there to suit our needs.

vec2 generateProjectedPosition(vec3 pos)
{
    vec4 samplePosition = projection_matrix * vec4(pos, 1.f);
    samplePosition.xy = (samplePosition.xy / samplePosition.w) * 0.5 + 0.5;
    return samplePosition.xy;
}

bool isBinarySearchEnabled = true;
bool isAdaptiveStepEnabled = true;
bool isExponentialStepEnabled = true;
bool debugDraw = false;

uniform float iterationCount;
uniform float rayStep;
uniform float distanceBias;
uniform float depthRejectBias;
uniform float glossySampleCount;
uniform float adaptiveStepMultiplier;
uniform float noiseSine;

float epsilon = 0.1;

float getLinearDepth(vec2 tc)
{
    float depth = texture(sceneDepth, tc).r;

    vec4 pos = getPositionWithDepth(tc, depth);

    return -pos.z;
}

bool traceScreenRay(vec3 position, vec3 reflection, out vec4 hitColor, out float hitDepth, float depth, sampler2D textureFrame)
{
    // transform position and reflection into same coordinate frame as the sceneMap and sceneDepth
    reflection += position;
    position = (inv_modelview_delta * vec4(position, 1)).xyz;
    reflection = (inv_modelview_delta * vec4(reflection, 1)).xyz;
    reflection -= position;

    depth = -position.z;

    vec3 step = rayStep * reflection;
    vec3 marchingPosition = position + step;
    float delta;
    float depthFromScreen;
    vec2 screenPosition;
    bool hit = false;
    hitColor = vec4(0);

    int i = 0;
    if (depth > depthRejectBias)
    {
        for (; i < iterationCount && !hit; i++)
        {
            screenPosition = generateProjectedPosition(marchingPosition);
            if (screenPosition.x > 1 || screenPosition.x < 0 ||
                screenPosition.y > 1 || screenPosition.y < 0)
            {
                hit = false;
                break;
            }
            depthFromScreen = getLinearDepth(screenPosition);
            delta = abs(marchingPosition.z) - depthFromScreen;

            if (depth < depthFromScreen + epsilon && depth > depthFromScreen - epsilon)
            {
                break;
            }

            if (abs(delta) < distanceBias)
            {
                vec4 color = vec4(1);
                if(debugDraw)
                    color = vec4( 0.5+ sign(delta)/2,0.3,0.5- sign(delta)/2, 0);
                hitColor = texture(sceneMap, screenPosition) * color;
                hitDepth = depthFromScreen;
                hit = true;
                break;
            }
            if (isBinarySearchEnabled && delta > 0)
            {
                break;
            }
            if (isAdaptiveStepEnabled)
            {
                float directionSign = sign(abs(marchingPosition.z) - depthFromScreen);
                //this is sort of adapting step, should prevent lining reflection by doing sort of iterative converging
                //some implementation doing it by binary search, but I found this idea more cheaty and way easier to implement
                step = step * (1.0 - rayStep * max(directionSign, 0.0));
                marchingPosition += step * (-directionSign);
            }
            else
            {
                marchingPosition += step;
            }

            if (isExponentialStepEnabled)
            {
                step *= adaptiveStepMultiplier;
            }
        }
        if(isBinarySearchEnabled)
        {
            for(; i < iterationCount && !hit; i++)
            {
                step *= 0.5;
                marchingPosition = marchingPosition - step * sign(delta);

                screenPosition = generateProjectedPosition(marchingPosition);
                if (screenPosition.x > 1 || screenPosition.x < 0 ||
                    screenPosition.y > 1 || screenPosition.y < 0)
                {
                    hit = false;
                    break;
                }
                depthFromScreen = getLinearDepth(screenPosition);
                delta = abs(marchingPosition.z) - depthFromScreen;

                if (depth < depthFromScreen + epsilon && depth > depthFromScreen - epsilon)
                {
                    break;
                }

                if (abs(delta) < distanceBias && depthFromScreen != (depth - distanceBias))
                {
                    vec4 color = vec4(1);
                    if(debugDraw)
                        color = vec4( 0.5+ sign(delta)/2,0.3,0.5- sign(delta)/2, 0);
                    hitColor = texture(sceneMap, screenPosition) * color;
                    hitDepth = depthFromScreen;
                    hit = true;
                    break;
                }
            }
        }
    }

    return hit;
}

uniform vec3 POISSON3D_SAMPLES[128] = vec3[128](
    vec3(0.5433144, 0.1122154, 0.2501391),
    vec3(0.6575254, 0.721409, 0.16286),
    vec3(0.02888453, 0.05170321, 0.7573566),
    vec3(0.06635678, 0.8286457, 0.07157445),
    vec3(0.8957489, 0.4005505, 0.7916042),
    vec3(0.3423355, 0.5053263, 0.9193521),
    vec3(0.9694794, 0.9461077, 0.5406441),
    vec3(0.9975473, 0.02789414, 0.7320132),
    vec3(0.07781899, 0.3862341, 0.918594),
    vec3(0.4439073, 0.9686955, 0.4055861),
    vec3(0.9657035, 0.6624081, 0.7082613),
    vec3(0.7712346, 0.07273269, 0.3292839),
    vec3(0.2489169, 0.2550394, 0.1950516),
    vec3(0.7249326, 0.9328285, 0.3352458),
    vec3(0.6028461, 0.4424961, 0.5393377),
    vec3(0.2879795, 0.7427881, 0.6619173),
    vec3(0.3193627, 0.0486145, 0.08109283),
    vec3(0.1233155, 0.602641, 0.4378719),
    vec3(0.9800708, 0.211729, 0.6771586),
    vec3(0.4894537, 0.3319927, 0.8087631),
    vec3(0.4802743, 0.6358885, 0.814935),
    vec3(0.2692913, 0.9911493, 0.9934899),
    vec3(0.5648789, 0.8553897, 0.7784553),
    vec3(0.8497344, 0.7870212, 0.02065313),
    vec3(0.7503014, 0.2826185, 0.05412734),
    vec3(0.8045461, 0.6167251, 0.9532926),
    vec3(0.04225039, 0.2141281, 0.8678675),
    vec3(0.07116079, 0.9971236, 0.3396397),
    vec3(0.464099, 0.480959, 0.2775862),
    vec3(0.6346927, 0.31871, 0.6588384),
    vec3(0.449012, 0.8189669, 0.2736875),
    vec3(0.452929, 0.2119148, 0.672004),
    vec3(0.01506042, 0.7102436, 0.9800494),
    vec3(0.1970513, 0.4713539, 0.4644522),
    vec3(0.13715, 0.7253224, 0.5056525),
    vec3(0.9006432, 0.5335414, 0.02206874),
    vec3(0.9960898, 0.7961011, 0.01468861),
    vec3(0.3386469, 0.6337739, 0.9310676),
    vec3(0.1745718, 0.9114985, 0.1728188),
    vec3(0.6342545, 0.5721557, 0.4553517),
    vec3(0.1347412, 0.1137158, 0.7793725),
    vec3(0.3574478, 0.3448052, 0.08741581),
    vec3(0.7283059, 0.4753885, 0.2240275),
    vec3(0.8293507, 0.9971212, 0.2747005),
    vec3(0.6501846, 0.000688076, 0.7795712),
    vec3(0.01149416, 0.4930083, 0.792608),
    vec3(0.666189, 0.1875442, 0.7256873),
    vec3(0.8538797, 0.2107637, 0.1547532),
    vec3(0.5826825, 0.9750752, 0.9105834),
    vec3(0.8914346, 0.08266425, 0.5484225),
    vec3(0.4374518, 0.02987111, 0.7810078),
    vec3(0.2287418, 0.1443802, 0.1176908),
    vec3(0.2671157, 0.8929081, 0.8989366),
    vec3(0.5425819, 0.5524959, 0.6963879),
    vec3(0.3515188, 0.8304397, 0.0502702),
    vec3(0.3354864, 0.2130747, 0.141169),
    vec3(0.9729427, 0.3509927, 0.6098799),
    vec3(0.7585629, 0.7115368, 0.9099342),
    vec3(0.0140543, 0.6072157, 0.9436461),
    vec3(0.9190664, 0.8497264, 0.1643751),
    vec3(0.1538157, 0.3219983, 0.2984214),
    vec3(0.8854713, 0.2968667, 0.8511457),
    vec3(0.1910622, 0.03047311, 0.3571215),
    vec3(0.2456353, 0.5568692, 0.3530164),
    vec3(0.6927255, 0.8073994, 0.5808484),
    vec3(0.8089353, 0.8969175, 0.3427134),
    vec3(0.194477, 0.7985603, 0.8712182),
    vec3(0.7256182, 0.5653068, 0.3985921),
    vec3(0.9889427, 0.4584851, 0.8363391),
    vec3(0.5718582, 0.2127113, 0.2950557),
    vec3(0.5480209, 0.0193435, 0.2992659),
    vec3(0.6598953, 0.09478426, 0.92187),
    vec3(0.1385615, 0.2193868, 0.205245),
    vec3(0.7623423, 0.1790726, 0.1508465),
    vec3(0.7569032, 0.3773386, 0.4393887),
    vec3(0.5842971, 0.6538072, 0.5224424),
    vec3(0.9954313, 0.5763943, 0.9169143),
    vec3(0.001311183, 0.340363, 0.1488652),
    vec3(0.8167927, 0.4947158, 0.4454727),
    vec3(0.3978434, 0.7106082, 0.002727509),
    vec3(0.5459411, 0.7473233, 0.7062873),
    vec3(0.4151598, 0.5614617, 0.4748358),
    vec3(0.4440694, 0.1195122, 0.9624678),
    vec3(0.1081301, 0.4813806, 0.07047641),
    vec3(0.2402785, 0.3633997, 0.3898734),
    vec3(0.2317942, 0.6488295, 0.4221864),
    vec3(0.01145542, 0.9304277, 0.4105759),
    vec3(0.3563728, 0.9228861, 0.3282344),
    vec3(0.855314, 0.6949819, 0.3175117),
    vec3(0.730832, 0.01478493, 0.5728671),
    vec3(0.9304829, 0.02653277, 0.712552),
    vec3(0.4132186, 0.4127623, 0.6084146),
    vec3(0.7517329, 0.9978395, 0.1330464),
    vec3(0.5210338, 0.4318751, 0.9721575),
    vec3(0.02953994, 0.1375937, 0.9458942),
    vec3(0.1835506, 0.9896691, 0.7919457),
    vec3(0.3857062, 0.2682322, 0.1264563),
    vec3(0.6319699, 0.8735335, 0.04390657),
    vec3(0.5630485, 0.3339024, 0.993995),
    vec3(0.90701, 0.1512893, 0.8970422),
    vec3(0.3027443, 0.1144253, 0.1488708),
    vec3(0.9149003, 0.7382028, 0.7914025),
    vec3(0.07979286, 0.6892691, 0.2866171),
    vec3(0.7743186, 0.8046008, 0.4399814),
    vec3(0.3128662, 0.4362317, 0.6030678),
    vec3(0.1133721, 0.01605821, 0.391872),
    vec3(0.5185481, 0.9210006, 0.7889017),
    vec3(0.8217013, 0.325305, 0.1668191),
    vec3(0.8358996, 0.1449739, 0.3668382),
    vec3(0.1778213, 0.5599256, 0.1327691),
    vec3(0.06690693, 0.5508637, 0.07212365),
    vec3(0.9750564, 0.284066, 0.5727578),
    vec3(0.4350255, 0.8949825, 0.03574753),
    vec3(0.8931149, 0.9177974, 0.8123496),
    vec3(0.9055127, 0.989903, 0.813235),
    vec3(0.2897243, 0.3123978, 0.5083504),
    vec3(0.1519223, 0.3958645, 0.2640327),
    vec3(0.6840154, 0.6463035, 0.2346607),
    vec3(0.986473, 0.8714055, 0.3960275),
    vec3(0.6819352, 0.4169535, 0.8379834),
    vec3(0.9147297, 0.6144146, 0.7313942),
    vec3(0.6554981, 0.5014008, 0.9748477),
    vec3(0.9805915, 0.1318207, 0.2371372),
    vec3(0.5980836, 0.06796348, 0.9941338),
    vec3(0.6836596, 0.9917196, 0.2319056),
    vec3(0.5276511, 0.2745509, 0.5422578),
    vec3(0.829482, 0.03758276, 0.1240466),
    vec3(0.2698198, 0.0002266169, 0.3449324)
);

vec3 getPoissonSample(int i) {
    return POISSON3D_SAMPLES[i] * 2 - 1;
}

#if defined(LUMINOUS_SSR)
// <Luminous> Photo mode's reflections, after Black Dragon's rewrite (NiranV
// Dean; itself after imanolfotia.com/blog/1): the reflected ray marched in
// growing steps until it passes behind the depth buffer by less than a
// thickness, then a binary search for the hit, refused when it does not
// converge; on rough surfaces several rays, importance-sampled from GGX, and
// the frame read from a mip level as coarse as the blur. It reaches surfaces
// down to roughness 0.6 (reflectionProbeF.glsl), where Firestorm stops at 0.1.
// Fewer steps and rays while the shot is set up, more in a snapshot.

uniform int luminous_ssr;          // 1 draws these; 0 Firestorm's, in the live preview when they are kept for renders
uniform int luminous_ssr_steps;
uniform int luminous_ssr_samples;
uniform float luminous_ssr_scale;  // photo mode's live view: render size over the frame's, 1 in a snapshot
uniform float luminous_frame;

const float LUMINOUS_SSR_STEP = 0.004;     // the first step, per metre away from the camera
const float LUMINOUS_SSR_MIN_STEP = 0.02;  // metres
const float LUMINOUS_SSR_GROWTH = 1.15;    // each step this much longer
const float LUMINOUS_SSR_MAX_STEP = 5.0;
const float LUMINOUS_SSR_THICKNESS = 1.0;  // how far behind a surface still counts as hitting it
const float LUMINOUS_SSR_BIAS = 0.002;     // the ray starts this far off the surface, per metre away
const float LUMINOUS_SSR_FAR = 128.0;
const float LUMINOUS_SSR_MAX_ROUGHNESS = 0.6;

vec3 luminousSsrSearch(vec3 dir, inout vec3 hit, inout float d_depth)
{
    float depth = 0.0;
    float first = length(dir);
    for (int i = 0; i < 8; ++i)
    {
        depth = getLinearDepth(generateProjectedPosition(hit));
        d_depth = abs(hit.z) - depth;
        dir *= 0.5;
        hit += d_depth > 0.0 ? -dir : dir;
    }
    vec2 tc = generateProjectedPosition(hit);
    float max_error = max(first * 0.1, 0.05) * max(1.0, depth * 0.01);
    return abs(d_depth) > max_error ? vec3(-1.0, -1.0, depth) : vec3(tc, depth);
}

// From the surface in steps that start small -- what stands on it, as a
// pier's foot in the water, is reflected right where it meets it -- and grow
// geometrically. jitter (0-1) moves every sample by that share of its own
// step, so that the steps' boundaries do not draw rings.
//
// A ray that passes behind something nearer -- a floor's ray behind the legs
// of someone standing on it -- cannot see what it would reflect. The probe,
// which in a room is a picture from its middle, is lighter than the wall it
// stands in for and would draw a pale outline round the feet. So a hit
// refused at an edge marches on; a ray that comes out from behind the
// nearer thing still behind what shows there, the background, takes that
// background's colour -- the same wall, a little further along; and one
// still hidden when it ends takes what it last passed in front of before it
// went behind -- what shows round the nearer thing's edge. A ray that comes
// out in front of things again is in the open: off the screen it takes the
// probe.
vec3 luminousSsrMarch(vec3 dir, float first, float jitter, inout vec3 hit, out float d_depth)
{
    float g = pow(LUMINOUS_SSR_GROWTH, jitter);
    hit += dir * (first * (g - 1.0) / (LUMINOUS_SSR_GROWTH - 1.0));
    dir *= first * g;
    d_depth = 0.0;
    float occluder = -1.0; // how far away the nearest thing the ray went behind is
    vec3 seen = vec3(-1.0);  // where it last passed in front of what shows
    vec3 seen_at = hit;      // and the ray's own point there, for how far it went
    for (int i = 0; i < luminous_ssr_steps; ++i)
    {
        hit += dir;
        vec2 tc = generateProjectedPosition(hit);
        if (tc.x < 0.0 || tc.x > 1.0 || tc.y < 0.0 || tc.y > 1.0)
        {
            hit = seen_at;
            return occluder > 0.0 ? seen : vec3(-1.0);
        }
        float raw = texture(sceneDepth, tc).r;
        float depth = -getPositionWithDepth(tc, raw).z;
        d_depth = abs(hit.z) - depth;
        // The sky is no surface to reflect, whatever the draw distance makes
        // its depth; the probes reflect it.
        if (raw >= 1.0 || depth > LUMINOUS_SSR_FAR)
        {
            return vec3(-1.0);
        }
        if (d_depth <= 0.0)
        {
            seen = vec3(tc, depth); // in the open again
            seen_at = hit;
            occluder = -1.0;
        }
        float len = length(dir);
        float thickness = max(LUMINOUS_SSR_THICKNESS, len * 1.5);
        if (d_depth > 0.0 && occluder > 0.0 && depth > occluder * 1.1 + 0.3)
        {
            // out from behind the nearer thing, and behind the background
            if (d_depth <= thickness * 2.0)
            {
                return vec3(tc, depth);
            }
            occluder = depth;
        }
        else if (d_depth > 0.0 && d_depth <= thickness)
        {
            vec3 before = hit;
            float d_before = d_depth;
            vec3 found = luminousSsrSearch(dir, hit, d_depth);
            if (found.x >= 0.0)
            {
                return found;
            }
            hit = before; // an edge, not a surface: on past it
            d_depth = d_before;
            occluder = occluder > 0.0 ? min(occluder, depth) : depth;
        }
        else if (d_depth > thickness)
        {
            occluder = occluder > 0.0 ? min(occluder, depth) : depth;
        }
        dir = normalize(dir) * min(len * LUMINOUS_SSR_GROWTH, LUMINOUS_SSR_MAX_STEP);
    }
    // (how far the ray went counts to where its colour comes from: counted
    // to the march's end, far behind what hid it, the fill would fade to
    // nothing and the probe show through all the same)
    hit = seen_at;
    return occluder > 0.0 ? seen : vec3(-1.0);
}

// How squarely the surface at tc faces along `plane`, from its neighbours'
// depths (the nearer each way, so not across an edge); 0 where they cannot
// tell.
float luminousSsrFacing(vec2 tc, vec3 p, vec3 plane)
{
    vec2 px = 2.0 / screen_res;
    vec2 tl = tc - vec2(px.x, 0.0), tr = tc + vec2(px.x, 0.0);
    vec2 td = tc - vec2(0.0, px.y), tu = tc + vec2(0.0, px.y);
    vec3 l = getPositionWithDepth(tl, texture(sceneDepth, tl).r).xyz;
    vec3 r = getPositionWithDepth(tr, texture(sceneDepth, tr).r).xyz;
    vec3 d = getPositionWithDepth(td, texture(sceneDepth, td).r).xyz;
    vec3 u = getPositionWithDepth(tu, texture(sceneDepth, tu).r).xyz;
    vec3 dx = abs(r.z - p.z) < abs(p.z - l.z) ? r - p : p - l;
    vec3 dy = abs(u.z - p.z) < abs(p.z - d.z) ? u - p : p - d;
    vec3 n = cross(dx, dy);
    float len = length(n);
    return len > 1e-8 ? abs(dot(n / len, plane)) : 0.0;
}

float luminousTapScreenSpaceReflection(int totalSamples, vec2 tc, vec3 viewPos, vec3 n, inout vec4 collectedColor, sampler2D source, float glossiness)
{
#ifdef TRANSPARENT_SURFACE
    collectedColor = vec4(1, 0, 1, 1);
    return 0;
#endif
    collectedColor = vec4(0.0);
    float roughness = 1.0 - glossiness;
    vec3 view = normalize(viewPos);
    vec3 normal = normalize(n);
    vec2 from_centre = abs(tc * 2.0 - 1.0);
    float edge = 1.0 - smoothstep(0.85, 1.0, max(from_centre.x, from_centre.y));
    if (roughness >= LUMINOUS_SSR_MAX_ROUGHNESS || dot(-view, normal) <= 0.0 || edge <= 0.001)
    {
        return 0.0;
    }
    float bias = max(0.01, -viewPos.z * LUMINOUS_SSR_BIAS);
    vec3 biased = viewPos + normal * bias; // just off the surface
    float first = max(LUMINOUS_SSR_MIN_STEP, -viewPos.z * LUMINOUS_SSR_STEP);
    vec3 start = (inv_modelview_delta * vec4(biased, 1.0)).xyz;
    vec3 plane = normalize((inv_modelview_delta * vec4(normal, 0.0)).xyz); // the surface's facing, where the march is
    if (-start.z > LUMINOUS_SSR_FAR)
    {
        return 0.0;
    }
    vec3 mirror = normalize(reflect(view, normal));
    vec2 seed = tc * screen_res + luminous_frame * vec2(37.0, 113.0);
    // The blur's mip level as for the full frame, then one level finer for
    // each halving of a live view rendered smaller, so that a glossy blur
    // covers the same share of the picture.
    float live_scale = luminous_ssr_scale > 0.0 ? luminous_ssr_scale : 1.0;
    float max_mip = floor(log2(max(screen_res.x, screen_res.y) / live_scale));
    float mip_bias = log2(live_scale);
    vec3 colour = vec3(0.0);
    float fade = 0.0;
    int hits = 0;
    int samples = max(1, roughness > 0.001 ? luminous_ssr_samples : 1);
    for (int k = 0; k < samples; ++k)
    {
        vec3 dir = mirror;
        if (roughness > 0.001)
        {
            float a = roughness * roughness;
            float u1 = random(seed + float(k) * 0.123);
            float u2 = random(seed * 1.7 + float(k) * 0.456 + 0.5);
            float theta = atan(a * sqrt(u1) / sqrt(max(1.0 - u1, 1e-4)));
            float phi = 6.2831853 * u2;
            vec3 up = abs(mirror.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
            vec3 t = normalize(cross(up, mirror));
            vec3 b = cross(mirror, t);
            vec3 h = normalize(sin(theta) * cos(phi) * t + sin(theta) * sin(phi) * b + cos(theta) * mirror);
            dir = normalize(reflect(-mirror, h));
        }
        vec3 target = (inv_modelview_delta * vec4(viewPos + dir, 1.0)).xyz;
        vec3 ray = normalize(target - start);
        if (ray.z >= 0.5)
        {
            continue; // toward the camera: nothing on screen to find
        }
        vec3 origin = start;
        vec3 hit = origin;
        float d_depth;
        vec3 found = luminousSsrMarch(ray, first, random(seed + float(k) * 0.789), hit, d_depth);
        if (found.x < 0.0)
        {
            continue;
        }
        // A hit in the surface's own plane, on a surface that faces the same
        // way, is the surface finding itself: far water seen almost edge-on,
        // its rays tipped down by the waves, would reflect the darker water
        // further out -- a dark band where the sea meets the sky -- so such a
        // hit is refused. What stands on the surface, as a pier's foot in the
        // water, faces another way and is kept to its very edge.
        vec3 surface = getPositionWithDepth(found.xy, texture(sceneDepth, found.xy).r).xyz;
        if (dot(surface - start, plane) + bias < 0.02 + 0.004 * found.z && luminousSsrFacing(found.xy, surface, plane) > 0.8)
        {
            continue;
        }
        float travelled = length(hit - origin);
        float blur = clamp(roughness + roughness * clamp(travelled / LUMINOUS_SSR_FAR, 0.0, 1.0), 0.0, 1.0);
        colour += textureLod(source, found.xy, max(max_mip * blur * 0.5 + mip_bias, 0.0)).rgb;
        vec2 hit_centre = abs(found.xy * 2.0 - 1.0);
        fade += (1.0 - smoothstep(0.85, 1.0, max(hit_centre.x, hit_centre.y)))
              * (1.0 - smoothstep(LUMINOUS_SSR_FAR * 0.8, LUMINOUS_SSR_FAR, found.z))
              * (1.0 - smoothstep(LUMINOUS_SSR_FAR * 0.6, LUMINOUS_SSR_FAR, travelled));
        ++hits;
    }
    if (hits == 0)
    {
        return 0.0;
    }
    colour /= float(hits);
    fade /= float(samples);
    float rough_fade = 1.0 - clamp((roughness - LUMINOUS_SSR_MAX_ROUGHNESS * 0.6) / (LUMINOUS_SSR_MAX_ROUGHNESS * 0.4), 0.0, 1.0);
    // and gone by LUMINOUS_SSR_FAR away gradually: past it a surface has
    // only the probes, and a hard stop draws a line across the sea
    float far_fade = 1.0 - smoothstep(LUMINOUS_SSR_FAR * 0.75, LUMINOUS_SSR_FAR, -start.z);
    collectedColor = vec4(colour, fade * rough_fade * edge * far_fade);
    return 1.0;
}

// Linden's, below, stays for the live preview when these are kept for
// renders; tapScreenSpaceReflection, after it, chooses between the two.
#define tapScreenSpaceReflection lindenTapScreenSpaceReflection
#endif // LUMINOUS_SSR

float tapScreenSpaceReflection(int totalSamples, vec2 tc, vec3 viewPos, vec3 n, inout vec4 collectedColor, sampler2D source, float glossiness)
{
#ifdef TRANSPARENT_SURFACE
collectedColor = vec4(1, 0, 1, 1);
    return 0;
#endif
    collectedColor = vec4(0);
    int hits = 0;

    float depth = -viewPos.z;

    vec3 rayDirection = normalize(reflect(viewPos, normalize(n)));

    vec2 uv2 = tc * screen_res;
    float c = (uv2.x + uv2.y) * 0.125;
    float jitter = mod( c, 1.0);

    vec2 screenpos = 1 - abs(tc * 2 - 1);
    float vignette = clamp((abs(screenpos.x) * abs(screenpos.y)) * 16,0, 1);
    vignette *= clamp((dot(normalize(viewPos), n) * 0.5 + 0.5) * 5.5 - 0.8, 0, 1);

    float zFar = 128.0;
    vignette *= clamp(1.0+(viewPos.z/zFar), 0.0, 1.0);

    vignette *= clamp(glossiness * 3 - 1.7, 0, 1);

    vec4 hitpoint;

    glossiness = 1 - glossiness;

    totalSamples = int(max(glossySampleCount, glossySampleCount * glossiness * vignette));

    totalSamples = max(totalSamples, 1);
    if (glossiness < 0.35)
    {
        if (vignette > 0)
        {
            for (int i = 0; i < totalSamples; i++)
            {
                vec3 firstBasis = normalize(cross(getPoissonSample(i), rayDirection));
                vec3 secondBasis = normalize(cross(rayDirection, firstBasis));
                vec2 coeffs = vec2(random(tc + vec2(0, i)) + random(tc + vec2(i, 0)));
                vec3 reflectionDirectionRandomized = rayDirection + ((firstBasis * coeffs.x + secondBasis * coeffs.y) * glossiness);

                //float hitDepth;

                bool hit = traceScreenRay(viewPos, normalize(reflectionDirectionRandomized), hitpoint, depth, depth, source);

                hitpoint.a = 0;

                if (hit)
                {
                    ++hits;
                    collectedColor += hitpoint;
                    collectedColor.a += 1;
                }
            }

            if (hits > 0)
            {
                collectedColor /= hits;
            }
            else
            {
                collectedColor = vec4(0);
            }
        }
    }
    float hitAlpha = hits;
    hitAlpha /= totalSamples;
    collectedColor.a = hitAlpha * vignette;
    return hits;
}

#if defined(LUMINOUS_SSR) // <Luminous>
#undef tapScreenSpaceReflection
float tapScreenSpaceReflection(int totalSamples, vec2 tc, vec3 viewPos, vec3 n, inout vec4 collectedColor, sampler2D source, float glossiness)
{
    return luminous_ssr != 0 ? luminousTapScreenSpaceReflection(totalSamples, tc, viewPos, n, collectedColor, source, glossiness)
                             : lindenTapScreenSpaceReflection(totalSamples, tc, viewPos, n, collectedColor, source, glossiness);
}
#endif // </Luminous>
