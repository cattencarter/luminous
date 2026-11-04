// GTAO at half the frame's size, for the live view with low quality live on, and its way into the light
// map. Three steps:
// - luminous_gtao_half: each half-size texel's visibility, computed as aoUtil.glsl's luminousGTAO does at
//   the same place in the frame (the same slices, steps, radius, falloff, noise, multi-bounce and
//   strength). Samples near the point read the depth buffer itself, as the reference does; samples
//   further out than the pyramid's threshold read the depth pyramid's level for their distance, so
//   the wide samples of a close-up stay in the cache (after XeGTAO).
// - luminous_gtao_denoise: a 3 x 3 tent within each surface, from a tile in threadgroup memory.
// - luminous_gtao_up_fs: the four half-size texels round each pixel by bilinear weight and nearness in
//   depth, as luminousGTAOUpF.glsl, drawn into the light map's green with the destination multiplied.
//
// A half-size texel's depth is the depth buffer's at the texel's centre in the frame, read again where
// it is needed instead of being stored beside the visibility.

#include <metal_stdlib>
#include "screenshared.h"
#include "screenutil.h"
#include "luminous/common/fullscreen.h"

using namespace metal;

namespace luminous_gtao
{
    constant float PI = 3.14159265;

    inline float hash(float2 p)
    {
        float3 h = fract(float3(p.xyx) * 0.1031);
        h += dot(h, h.yzx + 33.33);
        return fract((h.x + h.y) * h.z);
    }

    /// deferredUtil.glsl's decodeNormal of the G-buffer's normal.
    inline float3 decodeNormal(float4 encoded)
    {
        const float2 fenc = encoded.xy * 4.0 - 2.0;
        const float f = dot(fenc, fenc);
        const float g = sqrt(max(1.0 - f / 4.0, 0.0));
        return float3(fenc * g, 1.0 - f / 2.0);
    }

    /// Where a half-size texel lies in the frame: GL's texture coordinate of its centre.
    inline float2 halfTexCoord(uint2 texel, float2 half_size)
    {
        return (float2(texel) + 0.5) / half_size;
    }

    /// The linear depth of a half-size texel: the depth buffer's at its centre.
    inline float halfDepth(constant LuminousGTAOConstants& k, depth2d<float, access::read> depth, uint2 texel)
    {
        const float2 tc = halfTexCoord(texel, k.halfSize);
        return luminousLinearDepth(k.invProj, tc, depth.read(luminousNearestTexel(tc, k.screenRes)));
    }

    /// The centre of the frame texel that holds tc, in GL's texture coordinates; off the frame, of the texel that
    /// would hold it there.
    inline float2 frameTexelCentre(float2 tc, float2 screen_res)
    {
        return (floor(tc * screen_res) + 0.5) / screen_res;
    }

    /// The eye-space position the slice's sample at tc finds: from the depth buffer itself at mip 0, at the centre
    /// of the texel whose depth it reads, else from the pyramid's level for it. tc may lie off the frame; the depth
    /// is the edge's, the position that of tc's texel there.
    inline float3 samplePosition(constant LuminousGTAOConstants& k, depth2d<float, access::read> depth,
                                 texture2d<float> pyramid, float2 tc, uint mip)
    {
        if (mip == 0)
        {
            return luminousEyePosition(k.invProj, frameTexelCentre(tc, k.screenRes),
                                       depth.read(luminousNearestTexel(tc, k.screenRes)));
        }
        const uint level = mip - 1;
        const float2 size = float2(max(uint(pyramid.get_width()) >> level, 1u), max(uint(pyramid.get_height()) >> level, 1u));
        const float linear = pyramid.read(luminousNearestTexel(tc, size), level).r;
        return luminousEyePositionAt(k.invProj, tc, linear);
    }
}

kernel void luminous_gtao_half(
    constant LuminousGTAOConstants& k [[buffer(LUMINOUS_SCREEN_BUFFER_PARAMS)]],
    depth2d<float, access::read> depth [[texture(LUMINOUS_SCREEN_TEXTURE_DEPTH)]],
    texture2d<float, access::read> normals [[texture(LUMINOUS_SCREEN_TEXTURE_NORMAL)]],
    texture2d<float, access::read> albedo [[texture(LUMINOUS_SCREEN_TEXTURE_ALBEDO)]],
    texture2d<float> pyramid [[texture(LUMINOUS_SCREEN_TEXTURE_PYRAMID)]],
    texture2d<float, access::write> out [[texture(LUMINOUS_SCREEN_TEXTURE_OUT)]],
    uint2 gid [[thread_position_in_grid]])
{
    using namespace luminous_gtao;
    if (any(float2(gid) >= k.halfSize))
    {
        return;
    }
    const float2 tc = halfTexCoord(gid, k.halfSize);
    const uint2 centre = luminousNearestTexel(tc, k.screenRes);
    const float d = depth.read(centre);
    // The sky: nothing to occlude, and softenLight never reads its AO.
    if (d >= 1.0)
    {
        out.write(float4(1.0), gid);
        return;
    }
    // The point, and the slices round it, at the centre of the frame texel whose depth and normal it takes. A
    // half-size texel's centre lies on the corner of four frame texels; a position made there from one of their
    // depths is off the surface by half a texel, and on ground seen at a grazing angle that raises every horizon.
    const float2 at = (float2(centre) + 0.5) / k.screenRes;
    const float3 P = luminousEyePosition(k.invProj, at, d);
    const float3 V = normalize(-P);
    const float3 N = normalize(decodeNormal(normals.read(centre)));

    // The radius on screen, in the frame's pixels: a perspective projection's focal length is
    // 1 / inv_proj[1][1] in half-heights.
    const float focal = 0.5 * k.screenRes.y / k.invProj[1][1];
    const float radius_px = min(k.radius * focal / max(-P.z, 0.01), 0.25 * k.screenRes.y);
    if (radius_px < 1.0)
    {
        out.write(float4(1.0), gid);
        return;
    }
    const float falloff_range = 0.615 * k.radius;

    // The noise of the reference's half-size pass: its gl_FragCoord is the half-size texel's centre.
    const float2 seed = float2(gid) + 0.5 + k.frame * float2(37.0, 113.0);
    const float rotation = hash(seed);
    const float jitter = hash(seed.yx + 71.0);

    const int slices = max(k.slices, 1);
    const int steps = max(k.steps, 1);
    const uint levels = min(k.pyramidLevels, uint(LUMINOUS_PYRAMID_MAX_LEVELS));
    float visibility = 0.0;
    for (int s = 0; s < slices; ++s)
    {
        const float phi = (float(s) + rotation) * PI / float(slices);
        const float2 omega = float2(cos(phi), sin(phi)); // on screen and in view space alike: both y up

        const float3 dir = float3(omega, 0.0);
        const float3 ortho = dir - dot(dir, V) * V;
        const float3 axis = normalize(cross(ortho, V));
        const float3 proj_n = N - axis * dot(N, axis);
        const float proj_len = length(proj_n);
        const float cos_n = clamp(dot(proj_n, V) / max(proj_len, 1e-4), 0.0, 1.0);
        const float n = sign(dot(ortho, proj_n)) * acos(cos_n);

        const float low0 = cos(n + 0.5 * PI);
        const float low1 = cos(n - 0.5 * PI);
        float horizon0 = low0;
        float horizon1 = low1;
        for (int step = 0; step < steps; ++step)
        {
            float t = (float(step) + jitter) / float(steps);
            t *= t; // denser near the point, where occlusion matters most
            const float offset_px = max(t * radius_px, 1.0);
            const float2 offset = omega * offset_px / k.screenRes;
            const uint mip = uint(clamp(floor(log2(offset_px) - k.mipOffset), 0.0, float(levels)));

            const float3 d0 = samplePosition(k, depth, pyramid, at + offset, mip) - P;
            const float3 d1 = samplePosition(k, depth, pyramid, at - offset, mip) - P;
            const float l0 = length(d0);
            const float l1 = length(d1);
            const float c0 = dot(d0, V) / max(l0, 1e-5);
            const float c1 = dot(d1, V) / max(l1, 1e-5);
            // Further than the radius counts for nothing, fading in from 0.385 of it.
            const float w0 = clamp((k.radius - l0) / falloff_range, 0.0, 1.0);
            const float w1 = clamp((k.radius - l1) / falloff_range, 0.0, 1.0);
            horizon0 = max(horizon0, mix(low0, c0, w0));
            horizon1 = max(horizon1, mix(low1, c1, w1));
        }

        float h0 = -acos(clamp(horizon1, -1.0, 1.0));
        float h1 = acos(clamp(horizon0, -1.0, 1.0));
        h0 = n + clamp(h0 - n, -0.5 * PI, 0.5 * PI);
        h1 = n + clamp(h1 - n, -0.5 * PI, 0.5 * PI);
        const float sin_n = sin(n);
        const float arc0 = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - n)) * 0.25;
        const float arc1 = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - n)) * 0.25;
        visibility += proj_len * (arc0 + arc1);
    }
    float ao = clamp(visibility / float(slices), 0.0, 1.0);

    // Light bounced between surfaces (Jimenez's fit), by the albedo's luminance.
    const float a_lum = clamp(dot(albedo.read(centre).rgb, float3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    const float a = 2.0404 * a_lum - 0.3324;
    const float b = -4.7951 * a_lum + 0.6417;
    const float c = 2.7552 * a_lum + 0.6903;
    ao = max(ao, ((ao * a + b) * ao + c) * ao);

    out.write(float4(clamp(pow(ao, k.strength), 0.0, 1.0)), gid);
}

kernel void luminous_gtao_denoise(
    constant LuminousGTAOConstants& k [[buffer(LUMINOUS_SCREEN_BUFFER_PARAMS)]],
    depth2d<float, access::read> depth [[texture(LUMINOUS_SCREEN_TEXTURE_DEPTH)]],
    texture2d<float, access::read> raw [[texture(LUMINOUS_SCREEN_TEXTURE_AO)]],
    texture2d<float, access::write> out [[texture(LUMINOUS_SCREEN_TEXTURE_OUT)]],
    uint2 group [[threadgroup_position_in_grid]],
    uint2 lid [[thread_position_in_threadgroup]],
    uint index [[thread_index_in_threadgroup]])
{
    using namespace luminous_gtao;
    constexpr uint SIDE = LUMINOUS_GTAO_THREADS + 2; // the group's texels and a ring of one round them
    threadgroup float tile_ao[SIDE * SIDE];
    threadgroup float tile_z[SIDE * SIDE];

    const uint2 half_size = uint2(k.halfSize);
    const int2 origin = int2(group * uint(LUMINOUS_GTAO_THREADS)) - 1;
    for (uint i = index; i < SIDE * SIDE; i += LUMINOUS_GTAO_THREADS * LUMINOUS_GTAO_THREADS)
    {
        const uint2 texel = uint2(clamp(origin + int2(i % SIDE, i / SIDE), int2(0), int2(half_size) - 1));
        tile_ao[i] = raw.read(texel).r;
        tile_z[i] = halfDepth(k, depth, texel);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint2 gid = group * uint(LUMINOUS_GTAO_THREADS) + lid;
    if (any(gid >= half_size))
    {
        return;
    }
    const uint middle = (lid.y + 1) * SIDE + lid.x + 1;
    if (k.denoise == 0)
    {
        out.write(float4(tile_ao[middle]), gid);
        return;
    }
    // A tent (1 2 1 on each axis), each neighbour also by how near its depth is to the texel's, with the
    // upsample's tolerance, so no occlusion crosses a silhouette.
    const float z = tile_z[middle];
    const float tolerance = 0.02 * abs(z) + 0.02;
    float sum = 0.0;
    float total = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const uint at = uint(int(middle) + y * int(SIDE) + x);
            const float tent = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            const float w = tent * exp(-abs(tile_z[at] - z) / tolerance);
            sum += tile_ao[at] * w;
            total += w;
        }
    }
    out.write(float4(sum / total), gid); // the middle weighs 4, so total > 0
}

fragment float4 luminous_gtao_up_fs(
    LuminousFullscreenOut in [[stage_in]],
    constant LuminousGTAOConstants& k [[buffer(LUMINOUS_SCREEN_BUFFER_PARAMS)]],
    depth2d<float, access::read> depth [[texture(LUMINOUS_SCREEN_TEXTURE_DEPTH)]],
    texture2d<float, access::read> ao [[texture(LUMINOUS_SCREEN_TEXTURE_AO)]])
{
    using namespace luminous_gtao;
    const float2 tc = in.uv;
    const float z = luminousLinearDepth(k.invProj, tc, depth.read(luminousNearestTexel(tc, k.screenRes)));
    const float2 hp = tc * k.halfSize - 0.5;
    const float2 base = floor(hp);
    const float2 f = hp - base;
    const float tolerance = 0.02 * abs(z) + 0.02;
    const int2 last = int2(k.halfSize) - 1;

    float sum = 0.0;
    float total = 0.0;
    float nearest = 1.0;
    float nearest_dz = 1e20;
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            const uint2 texel = uint2(clamp(int2(base) + int2(x, y), int2(0), last));
            const float visibility = ao.read(texel).r;
            const float bilinear = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
            const float dz = abs(halfDepth(k, depth, texel) - z);
            const float w = bilinear * exp(-dz / tolerance);
            sum += visibility * w;
            total += w;
            if (dz < nearest_dz)
            {
                nearest_dz = dz;
                nearest = visibility;
            }
        }
    }
    const float result = total > 1e-3 ? sum / total : nearest;
    // Drawn with the light map as the destination's factor and only green written.
    return float4(1.0, result, 1.0, 1.0);
}
