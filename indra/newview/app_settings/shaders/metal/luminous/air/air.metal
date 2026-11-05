// Light in the air: sun shafts, volumetric light and projector beams, the reference's math
// (luminousSunShaftMaskF, luminousSunShaftBlurF, luminousSunShaftsF, luminousVolumetricLightF,
// luminousProjectorBeamsF and the tent of luminousBloomUpF), drawn in at most one compute pass and
// one render pass a frame:
// - luminous_air_mask: the shafts' mask (the darkest of a 3 x 3 grid of the sky round the sun) and
//   its 5 x 5 tent softening in one kernel, the mask of a tile and its halo kept in threadgroup memory;
// - luminous_air_half: what is drawn at half size this frame (any of the shafts' gather, the
//   volumetric light and the beams, every projector of a batch in one pass), summed into one texture;
// - luminous_air_fs: what is drawn at full size, plus the half-size sum through the tent, added to the
//   HDR frame in one draw.
// Every target has GL's row order: a GL texture coordinate reads the same texel as in the reference.

#include <metal_stdlib>
#include "airshared.h"
#include "luminous/common/fullscreen.h"

using namespace metal;

namespace luminous_air
{
    constant float PI = 3.14159265;

    constexpr sampler linearClamp(filter::linear, address::clamp_to_edge);
    /// The reference's sampler2DShadow: GL_LEQUAL against the reference depth, bilinear.
    constexpr sampler shadowCompare(filter::linear, address::clamp_to_edge, compare_func::less_equal);
    /// A fetched texture's own sampling: trilinear, repeating.
    constexpr sampler projected(filter::linear, mip_filter::linear, address::repeat);

    /// The texel GL's nearest filter reads at tc, clamped to the edge.
    inline uint2 nearestTexel(float2 tc, float2 size)
    {
        return uint2(clamp(floor(tc * size), float2(0.0), size - 1.0));
    }

    /// deferredUtil.glsl's getPosition: the eye-space position of the depth buffer at tc.
    inline float3 eyePosition(constant LuminousAirParams& p, float2 tc, float d)
    {
        const float4 pos = p.invProj * float4(tc * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
        return pos.xyz / pos.w;
    }

    /// Interleaved gradient noise (Jimenez 2014), offset per frame of a render as the reference.
    inline float jitter(float2 frag, float frame)
    {
        frag += frame * float2(37.0, 113.0);
        return fract(52.9829189 * fract(dot(frag, float2(0.06711056, 0.00583715))));
    }

    /// Henyey-Greenstein.
    inline float phase(float cos_theta, float g)
    {
        const float g2 = g * g;
        const float denom = max(1.0 + g2 - 2.0 * g * cos_theta, 1e-4);
        return (1.0 - g2) / (4.0 * PI * denom * sqrt(denom));
    }

    inline float lookup(depth2d<float> map, float3 stc, float bias)
    {
        return map.sample_compare(shadowCompare, stc.xy, stc.z + bias);
    }

    inline bool inside(float3 stc)
    {
        return all(stc >= float3(0.0)) && all(stc <= float3(1.0));
    }

    /// The volumetric light at a pixel whose view ray is view_dir, dist metres to the surface.
    float3 volumetric(constant LuminousAirParams& p, depth2d<float> map0, depth2d<float> map1, depth2d<float> map2,
                      depth2d<float> map3, float3 view_dir, float dist, float jit)
    {
        const float march = min(min(dist, p.volDistance), p.shadowClip.w);
        if (march <= 0.0 || p.volDensity <= 0.0)
        {
            return float3(0.0);
        }
        const int samples = max(p.volSamples, 1);
        const float step_len = march / float(samples);

        // Each cascade's homogeneous coordinates along the ray: a straight line in the distance t.
        float4 start[4];
        float4 dir[4];
        for (int c = 0; c < 4; ++c)
        {
            start[c] = p.shadowMatrix[c][3];
            dir[c] = p.shadowMatrix[c] * float4(view_dir, 0.0);
        }

        float transmittance = exp(-p.volDensity * jit * step_len);
        const float step_transmittance = exp(-p.volDensity * step_len);
        const float depth_per_metre = -view_dir.z;

        // The view depth only grows along the ray, so the cascades whose far plane it has passed are
        // never asked again; the rest are asked nearest first, as the reference asks them.
        int nearest = 0;
        float lit = 0.0;
        for (int i = 0; i < samples; ++i)
        {
            const float t = (float(i) + jit) * step_len;
            const float d = t * depth_per_metre;
            while (nearest < 4 && d >= p.shadowClip[nearest])
            {
                ++nearest;
            }
            float visible = 1.0;
            for (int c = nearest; c < 4; ++c)
            {
                const float4 lpos = start[c] + dir[c] * t;
                const float3 stc = lpos.xyz / lpos.w;
                if (inside(stc))
                {
                    visible = c == 0 ? lookup(map0, stc, p.shadowBias)
                            : c == 1 ? lookup(map1, stc, p.shadowBias)
                            : c == 2 ? lookup(map2, stc, p.shadowBias)
                                     : lookup(map3, stc, p.shadowBias);
                    break;
                }
            }
            lit += visible * transmittance;
            transmittance *= step_transmittance;
        }
        lit *= step_len;

        const float3 light_dir = p.lightDir.xyz;
        const float ph = phase(dot(view_dir, light_dir), p.volAnisotropy);
        return max(p.sunlit.xyz * (p.volDensity * ph * lit), float3(0.0));
    }

    /// Narrows [lo, hi] to where a + b t >= 0.
    inline void keepWhere(float a, float b, thread float& lo, thread float& hi)
    {
        if (abs(b) < 1e-8)
        {
            if (a < 0.0)
            {
                hi = lo - 1.0;
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

    /// Linden's calcLegacyDistanceAttenuation.
    inline float legacyAttenuation(float distance, float falloff)
    {
        const float dist_atten = 1.0 - clamp((distance + falloff) / (1.0 + falloff), 0.0, 1.0);
        return dist_atten * dist_atten * 2.0;
    }

    /// One projector's beam along the view ray: luminousProjectorBeamsF's main for one projector.
    float3 beam(constant LuminousAirParams& p, constant LuminousAirBeam& b, texture2d<float> tex,
                depth2d<float> spot0, depth2d<float> spot1, float3 view_dir, float dist, float jit)
    {
        const float3 center = b.center.xyz;
        const float size = b.center.w;

        // Only the stretch of the line of sight inside the light's range.
        const float bc = dot(view_dir, center);
        const float c = dot(center, center) - size * size;
        float h = bc * bc - c;
        if (h <= 0.0)
        {
            return float3(0.0);
        }
        h = sqrt(h);
        const float t0 = max(bc - h, 0.0);
        const float t1 = min(bc + h, dist);
        if (t1 <= t0)
        {
            return float3(0.0);
        }

        const int samples = max(p.beamSamples, 1);
        const float step_len = (t1 - t0) / float(samples);

        // The cone: the projector's texture coordinates along the ray are a straight line in homogeneous
        // space; the light is only where they are in front of the apex and inside the texture.
        const float4 proj_start = b.projMat[3];
        const float4 proj_dir = b.projMat * float4(view_dir, 0.0);
        float lo = t0;
        float hi = t1;
        keepWhere(proj_start.w, proj_dir.w, lo, hi);
        keepWhere(proj_start.x, proj_dir.x, lo, hi);
        keepWhere(proj_start.w - proj_start.x, proj_dir.w - proj_dir.x, lo, hi);
        keepWhere(proj_start.y, proj_dir.y, lo, hi);
        keepWhere(proj_start.w - proj_start.y, proj_dir.w - proj_dir.y, lo, hi);
        const int first = max(int(ceil((lo - t0) / step_len - jit)), 0);
        const int last = min(int(floor((hi - t0) / step_len - jit)), samples - 1);
        if (hi <= lo || last < first)
        {
            return float3(0.0);
        }

        const bool shadowed = p.spotShadows != 0 && b.shadowIdx >= 0;
        float4 shadow_start = float4(0.0, 0.0, 0.0, 1.0);
        float4 shadow_dir = float4(0.0);
        if (shadowed)
        {
            const float4x4 shadow_mat = p.shadowMatrix[4 + min(b.shadowIdx, 1)];
            shadow_start = shadow_mat[3];
            shadow_dir = shadow_mat * float4(view_dir, 0.0);
        }

        const float ref = size / 4.5; // size is the radius times 1.5
        float3 light = float3(0.0);
        for (int i = first; i <= last; ++i)
        {
            const float t = t0 + (float(i) + jit) * step_len;
            const float3 pos = view_dir * t;

            const float3 lv = center - pos;
            const float l = length(lv);
            if (l >= size)
            {
                continue;
            }
            const float4 proj = proj_start + proj_dir * t;
            if (proj.w <= 0.0)
            {
                continue;
            }
            const float2 uv = proj.xy / proj.w;
            if (any(uv < float2(0.0)) || any(uv > float2(1.0)))
            {
                continue;
            }
            // The texture blurs past the focus, as Linden's getProjectedLightDiffuseColor.
            const float l_dist = -dot(lv, b.projN.xyz);
            const float lod = clamp((l_dist - b.projN.w) / b.projOrigin.w, 0.0, 1.0) * b.projLod;
            const float4 texel = tex.sample(projected, uv, level(lod));

            const float atten = legacyAttenuation(l / size, b.color.w);
            const float from_apex = length(pos - b.projOrigin.xyz);
            const float spread = min((ref * ref) / max(from_apex * from_apex, 1e-4), 25.0);

            float visible = 1.0;
            if (shadowed)
            {
                const float4 lpos = shadow_start + shadow_dir * t;
                const float3 stc = lpos.xyz / lpos.w;
                if (inside(stc))
                {
                    const float lit = b.shadowIdx == 0 ? spot0.sample_compare(shadowCompare, stc.xy, stc.z + p.spotShadowBias)
                                                       : spot1.sample_compare(shadowCompare, stc.xy, stc.z + p.spotShadowBias);
                    visible = clamp(lit + b.shadowFade, 0.0, 1.0);
                }
            }

            const float3 arriving = b.color.xyz * texel.rgb * texel.a * atten * spread * visible;
            if (any(arriving > float3(0.0)))
            {
                // Light travels from the apex to pos and is scattered back toward the eye. No fading with
                // distance from the camera, as the reference.
                const float3 travel = normalize(pos - b.projOrigin.xyz);
                light += arriving * phase(dot(travel, -view_dir), p.beamAnisotropy);
            }
        }
        return max(light * (p.beamDensity * step_len), float3(0.0));
    }

    /// The sun shafts' gather at tc: the mask along the line toward the sun, fading with each step.
    float3 shafts(constant LuminousAirParams& p, constant LuminousAirDraw& draw, texture2d<float> mask, float2 tc,
                  float jit)
    {
        const int samples = max(p.shaftSamples, 1);
        const float2 step_uv = (p.sunUV - tc) * (p.shaftLength / float(samples));

        // The line meets the mask once, where it comes within reach of the sun: start there.
        const float to_sun = length((p.sunUV - tc) * float2(p.aspect, 1.0));
        const float enter = to_sun > draw.shaftReach ? 1.0 - draw.shaftReach / to_sun : 0.0;
        const float first_f = ceil(enter * float(samples) / max(p.shaftLength, 1e-4) - jit);
        const int first = int(clamp(first_f, 0.0, float(samples)));
        if (first >= samples)
        {
            return float3(0.0);
        }
        float2 coord = tc + step_uv * (float(first) + jit);
        float3 sum = float3(0.0);
        float weight = pow(0.97, float(first));
        for (int i = first; i < samples; ++i)
        {
            sum += mask.sample(linearClamp, coord).rgb * weight;
            weight *= 0.97;
            coord += step_uv;
        }
        return sum * (p.shaftStrength / float(samples));
    }

    /// What one dispatch or draw adds at tc: frag is the pixel's centre in the target written (for the noise). In a
    /// settled frame tc is where the camera without the jitter saw it: the shafts (mask and sun there too) at tc, the
    /// rest from the frame's depth and projection at tc + the jitter, where the jittered frame shows that place.
    float3 airAt(constant LuminousAirParams& p, constant LuminousAirDraw& draw, constant LuminousAirBeam* beams,
                 depth2d<float, access::read> depth, depth2d<float> sun0, depth2d<float> sun1, depth2d<float> sun2,
                 depth2d<float> sun3, depth2d<float> spot0, depth2d<float> spot1,
                 texture2d<float> mask, array<texture2d<float>, LUMINOUS_AIR_PROJECTOR_TEXTURES> textures,
                 float2 tc, float2 frag)
    {
        const float jit = jitter(frag, p.frame);
        float3 sum = float3(0.0);
        if (draw.shafts != 0)
        {
            sum += shafts(p, draw, mask, tc, jit);
        }
        if (draw.vol == 0 && draw.beamCount <= 0)
        {
            return sum;
        }
        tc += draw.jitter;
        const float3 pos = eyePosition(p, tc, depth.read(nearestTexel(tc, draw.depthSize)));
        const float dist = length(pos);
        const float3 view_dir = pos / max(dist, 1e-4);
        if (draw.vol != 0)
        {
            sum += volumetric(p, sun0, sun1, sun2, sun3, view_dir, dist, jit);
        }
        // One texel of the target round each projector's rect, as the scissor's rounding outward.
        const float2 texel = 1.0 / draw.targetSize;
        for (int i = 0; i < draw.beamCount; ++i)
        {
            constant LuminousAirBeam& b = beams[draw.beamFirst + i];
            if (any(tc < b.rect.xy - texel) || any(tc > b.rect.zw + texel))
            {
                continue;
            }
            sum += beam(p, b, textures[min(uint(b.texture), uint(LUMINOUS_AIR_PROJECTOR_TEXTURES - 1))], spot0, spot1,
                        view_dir, dist, jit);
        }
        return sum;
    }

    /// The sky's colour at uv, or black where something nearer than the sky is.
    inline float3 sky(texture2d<float> frame, depth2d<float, access::read> depth, float2 uv, float2 depth_size)
    {
        const float s = step(0.99999, depth.read(nearestTexel(uv, depth_size)));
        return frame.sample(linearClamp, uv).rgb * s;
    }

    /// One mask texel: the darkest of a 3 x 3 grid of the sky, two of the frame's pixels apart, faded
    /// with distance from the sun.
    inline half4 maskAt(constant LuminousAirParams& p, constant LuminousAirDraw& draw, texture2d<float> frame,
                        depth2d<float, access::read> depth, int2 texel)
    {
        const float2 tc = (float2(texel) + 0.5) / draw.maskSize;
        const float2 d = (tc - p.sunUV) * float2(p.aspect, 1.0);
        const float near_sun = 1.0 - smoothstep(0.0, p.shaftRadius, length(d));
        if (near_sun <= 0.0)
        {
            return half4(0.0);
        }
        // a settled frame's mask: where the camera without the jitter saw the sky, as the sun's place is
        const float2 at = tc + draw.jitter;
        float3 darkest = float3(1e20);
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                darkest = min(darkest, sky(frame, depth, at + float2(x, y) * 2.0 * p.srcTexel, draw.depthSize));
            }
        }
        return half4(half3(darkest * near_sun), 1.0h);
    }
}

using namespace luminous_air;

enum : int
{
    MASK_TILE_SIDE = LUMINOUS_AIR_MASK_THREADS + 2 * LUMINOUS_AIR_MASK_HALO_MAX,
};

/// The mask and its softening. A threadgroup makes 16 x 16 mask texels: first the mask of those and of a halo
/// as wide as the softening reaches (clamped to the edge, as the reference's sampler), then the 5 x 5 tent of
/// bilinear taps, spread apart, from threadgroup memory. Without softening the halo is 0 and the mask is
/// written as it is.
kernel void luminous_air_mask(
    constant LuminousAirParams& p [[buffer(LUMINOUS_AIR_BUFFER_PARAMS)]],
    constant LuminousAirDraw& draw [[buffer(LUMINOUS_AIR_BUFFER_DRAW)]],
    depth2d<float, access::read> depth [[texture(LUMINOUS_AIR_TEXTURE_DEPTH)]],
    texture2d<float> frame [[texture(LUMINOUS_AIR_TEXTURE_FRAME)]],
    texture2d<float, access::write> mask [[texture(LUMINOUS_AIR_TEXTURE_MASK)]],
    uint2 group [[threadgroup_position_in_grid]],
    uint2 local [[thread_position_in_threadgroup]],
    uint index [[thread_index_in_threadgroup]])
{
    threadgroup half4 tile[MASK_TILE_SIDE * MASK_TILE_SIDE];

    const int halo = clamp(draw.maskHalo, 0, int(LUMINOUS_AIR_MASK_HALO_MAX));
    const int side = int(LUMINOUS_AIR_MASK_THREADS) + 2 * halo;
    const int2 size = int2(draw.maskSize);
    const int2 origin = int2(group) * int(LUMINOUS_AIR_MASK_THREADS) - halo;
    const int threads = int(LUMINOUS_AIR_MASK_THREADS * LUMINOUS_AIR_MASK_THREADS);
    for (int i = int(index); i < side * side; i += threads)
    {
        const int2 at = int2(i % side, i / side);
        const int2 texel = clamp(origin + at, int2(0), size - 1);
        tile[at.y * MASK_TILE_SIDE + at.x] = maskAt(p, draw, frame, depth, texel);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const int2 texel = int2(group) * int(LUMINOUS_AIR_MASK_THREADS) + int2(local);
    if (any(texel >= size))
    {
        return;
    }
    if (halo == 0)
    {
        mask.write(float4(float3(tile[int(local.y) * MASK_TILE_SIDE + int(local.x)].rgb), 1.0), uint2(texel));
        return;
    }

    // luminousSunShaftBlurF: weights (3 - |x|)(3 - |y|), taps spread mask texels apart.
    const float spread = p.shaftSoftness;
    float3 sum = float3(0.0);
    float total = 0.0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            const float w = float(3 - abs(x)) * float(3 - abs(y));
            // The bilinear tap in texel space (texel centres at integers), relative to the tile.
            const float2 q = float2(texel - origin) + float2(x, y) * spread;
            const float2 base = floor(q);
            const float2 f = q - base;
            const int2 a = clamp(int2(base), int2(0), int2(side - 2));
            const float3 v00 = float3(tile[a.y * MASK_TILE_SIDE + a.x].rgb);
            const float3 v10 = float3(tile[a.y * MASK_TILE_SIDE + a.x + 1].rgb);
            const float3 v01 = float3(tile[(a.y + 1) * MASK_TILE_SIDE + a.x].rgb);
            const float3 v11 = float3(tile[(a.y + 1) * MASK_TILE_SIDE + a.x + 1].rgb);
            sum += mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y) * w;
            total += w;
        }
    }
    mask.write(float4(sum / total, 1.0), uint2(texel));
}

/// What this frame draws at half size, into the half-size sum: the first dispatch writes it, a further batch of
/// projectors adds to it.
kernel void luminous_air_half(
    constant LuminousAirParams& p [[buffer(LUMINOUS_AIR_BUFFER_PARAMS)]],
    constant LuminousAirDraw& draw [[buffer(LUMINOUS_AIR_BUFFER_DRAW)]],
    constant LuminousAirBeam* beams [[buffer(LUMINOUS_AIR_BUFFER_BEAMS)]],
    depth2d<float, access::read> depth [[texture(LUMINOUS_AIR_TEXTURE_DEPTH)]],
    depth2d<float> sun0 [[texture(LUMINOUS_AIR_TEXTURE_SUN0)]],
    depth2d<float> sun1 [[texture(LUMINOUS_AIR_TEXTURE_SUN0 + 1)]],
    depth2d<float> sun2 [[texture(LUMINOUS_AIR_TEXTURE_SUN0 + 2)]],
    depth2d<float> sun3 [[texture(LUMINOUS_AIR_TEXTURE_SUN0 + 3)]],
    depth2d<float> spot0 [[texture(LUMINOUS_AIR_TEXTURE_SPOT0)]],
    depth2d<float> spot1 [[texture(LUMINOUS_AIR_TEXTURE_SPOT0 + 1)]],
    texture2d<float> mask [[texture(LUMINOUS_AIR_TEXTURE_MASK)]],
    texture2d<float, access::read_write> air [[texture(LUMINOUS_AIR_TEXTURE_AIR)]],
    array<texture2d<float>, LUMINOUS_AIR_PROJECTOR_TEXTURES> textures [[texture(LUMINOUS_AIR_TEXTURE_PROJECTOR0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (any(float2(gid) >= draw.targetSize))
    {
        return;
    }
    const float2 frag = float2(gid) + 0.5;
    const float2 tc = frag / draw.targetSize;
    float3 sum = airAt(p, draw, beams, depth, sun0, sun1, sun2, sun3, spot0, spot1, mask, textures, tc, frag);
    if (draw.accumulate != 0)
    {
        sum += air.read(gid).rgb;
    }
    air.write(float4(sum, 0.0), gid);
}

/// What this frame draws at full size, and the half-size sum through the tent (luminousBloomUpF: a 3 x 3 tent
/// of bilinear taps half a texel apart), added to the frame; alpha is left alone by the pipeline's mask.
fragment float4 luminous_air_fs(
    LuminousFullscreenOut in [[stage_in]],
    constant LuminousAirParams& p [[buffer(LUMINOUS_AIR_BUFFER_PARAMS)]],
    constant LuminousAirDraw& draw [[buffer(LUMINOUS_AIR_BUFFER_DRAW)]],
    constant LuminousAirBeam* beams [[buffer(LUMINOUS_AIR_BUFFER_BEAMS)]],
    depth2d<float, access::read> depth [[texture(LUMINOUS_AIR_TEXTURE_DEPTH)]],
    depth2d<float> sun0 [[texture(LUMINOUS_AIR_TEXTURE_SUN0)]],
    depth2d<float> sun1 [[texture(LUMINOUS_AIR_TEXTURE_SUN0 + 1)]],
    depth2d<float> sun2 [[texture(LUMINOUS_AIR_TEXTURE_SUN0 + 2)]],
    depth2d<float> sun3 [[texture(LUMINOUS_AIR_TEXTURE_SUN0 + 3)]],
    depth2d<float> spot0 [[texture(LUMINOUS_AIR_TEXTURE_SPOT0)]],
    depth2d<float> spot1 [[texture(LUMINOUS_AIR_TEXTURE_SPOT0 + 1)]],
    texture2d<float> mask [[texture(LUMINOUS_AIR_TEXTURE_MASK)]],
    texture2d<float> air [[texture(LUMINOUS_AIR_TEXTURE_AIR)]],
    array<texture2d<float>, LUMINOUS_AIR_PROJECTOR_TEXTURES> textures [[texture(LUMINOUS_AIR_TEXTURE_PROJECTOR0)]])
{
    // A settled frame: the place the camera without the jitter saw here, as the sum holds it; airAt draws the volumetric
    // light and the beams back at in.uv.
    const float2 uv = in.uv - draw.jitter;
    float3 sum = airAt(p, draw, beams, depth, sun0, sun1, sun2, sun3, spot0, spot1, mask, textures, uv, in.position.xy);
    if (draw.addAir != 0)
    {
        const float2 d = draw.airTexel * 0.5;
        float3 s = air.sample(linearClamp, uv).rgb * 4.0;
        s += (air.sample(linearClamp, uv + float2(-d.x, 0.0)).rgb + air.sample(linearClamp, uv + float2(d.x, 0.0)).rgb +
              air.sample(linearClamp, uv + float2(0.0, -d.y)).rgb + air.sample(linearClamp, uv + float2(0.0, d.y)).rgb) * 2.0;
        s += air.sample(linearClamp, uv + float2(-d.x, -d.y)).rgb + air.sample(linearClamp, uv + float2(d.x, -d.y)).rgb +
             air.sample(linearClamp, uv + float2(-d.x, d.y)).rgb + air.sample(linearClamp, uv + float2(d.x, d.y)).rgb;
        sum += s * (1.0 / 16.0);
    }
    return float4(sum, 0.0);
}
