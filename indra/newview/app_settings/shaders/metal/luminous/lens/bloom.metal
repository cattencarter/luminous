// The bloom chain and the passes that read it: clarity, halation and bloom.
//
// The chain is the frame halved six times with Jimenez's thirteen taps, as the translated chain does, but made once
// a frame for all three (luminous_lens_bloom_down), not once for each, in four dispatches: the half-size level from
// the frame, then two levels a dispatch -- the first from its source with the hardware's bilinear taps, the second
// from the first as the threadgroup holds it in its own memory, with the same taps read bilinearly there, so the
// first is never read back from memory for it -- and the last level alone.
//
// Each effect then weights the levels its own way: going back up, each level of its up chain becomes (1 - w) of the
// down level and w of a tent over the coarser up level. Clarity and halation step theirs together
// (luminous_lens_bloom_up) and read the half-size level at one point only, so theirs is made where they read it,
// from the down level and a tent over their quarter-size level. Bloom, the chain's last reader in a frame, steps
// its in the down chain itself (luminous_lens_bloom_up_in_place), as the translated chain does, and its tent reads
// the half-size level that leaves.
//
// The rewrite of the frame (luminous_lens_tone_fs) applies clarity, halation and bloom in that order, those the mode
// names, in one pass; bloom without its glow blends in place instead (luminous_lens_bloom_fs).

#include "lenscommon.h"
#include "luminous/common/fullscreen.h"

namespace luminous_lens
{
    /// The bilinear value at pos (first-level texels, centres at k + 0.5) of the first level held in the tile, whose
    /// texel (0, 0) is the level's texel origin. Coordinates outside the level take its edge, as a clamped sampler does.
    inline float3 tileTap(threadgroup const half4* tile, int2 origin, int2 size, float2 pos)
    {
        const float2 q = pos - 0.5;
        const float2 f0 = floor(q);
        const float2 fr = q - f0;
        const int2 i0 = int2(f0);
        float3 v[4];
        for (int k = 0; k < 4; ++k)
        {
            int2 c = clamp(i0 + int2(k & 1, k >> 1), int2(0), size - 1) - origin;
            c = clamp(c, int2(0), int2(LUMINOUS_LENS_DOWN_SPAN - 1));
            v[k] = float3(tile[c.y * LUMINOUS_LENS_DOWN_SPAN + c.x].rgb);
        }
        return mix(mix(v[0], v[1], fr.x), mix(v[2], v[3], fr.x), fr.y);
    }
}

// One or two levels of the down chain. Without hasB: one thread per texel of A, 16 x 16 threads a group. With hasB:
// each group owns 16 x 16 texels of B and the 32 x 32 of A under them; it makes A over its own texels and three
// each side (the reach of B's taps), writes its own, and makes B from what it holds. The C++ side dispatches
// max(ceil(B / 16), ceil(A / 32)) groups on each axis then, so every texel of A is some group's own.
kernel void luminous_lens_bloom_down(
    constant LuminousLensDownConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> source [[texture(0)]],
    texture2d<float, access::write> dstA [[texture(1)]],
    texture2d<float, access::write> dstB [[texture(2)]],
    uint2 group [[threadgroup_position_in_grid]],
    uint2 local [[thread_position_in_threadgroup]],
    uint index [[thread_index_in_threadgroup]])
{
    threadgroup half4 tile[LUMINOUS_LENS_DOWN_SPAN * LUMINOUS_LENS_DOWN_SPAN];
    const int2 sizeA = int2(k.sizeA);
    const float2 invA = 1.0 / float2(k.sizeA);

    if (k.hasB == 0)
    {
        const uint2 a = group * uint(LUMINOUS_LENS_DOWN_TILE) + local;
        if (all(a < k.sizeA))
        {
            dstA.write(float4(lensDownSample(source, (float2(a) + 0.5) * invA, k.srcTexel), 1.0), a);
        }
        return;
    }

    const int2 own = int2(group) * int(2 * LUMINOUS_LENS_DOWN_TILE);
    const int2 origin = own - 3;
    const uint count = LUMINOUS_LENS_DOWN_SPAN * LUMINOUS_LENS_DOWN_SPAN;
    for (uint i = index; i < count; i += LUMINOUS_LENS_DOWN_TILE * LUMINOUS_LENS_DOWN_TILE)
    {
        const int2 a = origin + int2(i % LUMINOUS_LENS_DOWN_SPAN, i / LUMINOUS_LENS_DOWN_SPAN);
        const int2 at = clamp(a, int2(0), sizeA - 1);
        const float3 value = lensDownSample(source, (float2(at) + 0.5) * invA, k.srcTexel);
        tile[i] = half4(half3(value), 1.0h);
        if (all(a == at) && all(a >= own) && all(a < own + int(2 * LUMINOUS_LENS_DOWN_TILE)))
        {
            dstA.write(float4(value, 1.0), uint2(a));
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint2 b = group * uint(LUMINOUS_LENS_DOWN_TILE) + local;
    if (all(b < k.sizeB))
    {
        // the point in A's texels, as a bilinear tap at B's uv with A's texel size puts it
        const float2 centre = (float2(b) + 0.5) * float2(k.sizeA) / float2(k.sizeB);
        float3 t[13];
        for (uint i = 0; i < 13; ++i)
        {
            t[i] = luminous_lens::tileTap(tile, origin, sizeA, centre + LENS_JIMENEZ_OFFSETS[i]);
        }
        dstB.write(float4(lensJimenez13(t), 1.0), b);
    }
}

// One level of up to three up chains: out = mix(down, tent over the coarser level, w). The coarser level is the
// chain's own, or the down chain's coarsest for the first step. 16 x 16 threads a group, one per texel.
kernel void luminous_lens_bloom_up(
    constant LuminousLensUpConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> down [[texture(0)]],
    texture2d<float> coarse0 [[texture(1)]],
    texture2d<float> coarse1 [[texture(2)]],
    texture2d<float> coarse2 [[texture(3)]],
    texture2d<float, access::write> out0 [[texture(4)]],
    texture2d<float, access::write> out1 [[texture(5)]],
    texture2d<float, access::write> out2 [[texture(6)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (any(gid >= k.size))
    {
        return;
    }
    const float3 d = down.read(gid).rgb;
    const float2 uv = (float2(gid) + 0.5) / float2(k.size);
    out0.write(float4(mix(d, lensTent9(coarse0, uv, k.coarseTexel), k.weights.x), 1.0), gid);
    if (k.chains > 1)
    {
        out1.write(float4(mix(d, lensTent9(coarse1, uv, k.coarseTexel), k.weights.y), 1.0), gid);
    }
    if (k.chains > 2)
    {
        out2.write(float4(mix(d, lensTent9(coarse2, uv, k.coarseTexel), k.weights.z), 1.0), gid);
    }
}

// Bloom's up chain, one level, in the down chain itself: the level is read and written through one binding, each
// thread its own texel, and the coarser level (bloom's already) is read through another.
kernel void luminous_lens_bloom_up_in_place(
    constant LuminousLensUpConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> coarse [[texture(1)]],
    texture2d<float, access::read_write> level [[texture(4)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (any(gid >= k.size))
    {
        return;
    }
    const float3 d = level.read(gid).rgb;
    const float2 uv = (float2(gid) + 0.5) / float2(k.size);
    level.write(float4(mix(d, lensTent9(coarse, uv, k.coarseTexel), k.weights.x), 1.0), gid);
}

// Clarity, halation and bloom, those the mode names, drawn anew into the other target; the alpha, Second Life's
// glow mask, is carried over.
fragment float4 luminous_lens_tone_fs(
    LuminousFullscreenOut in [[stage_in]],
    constant LuminousLensParams& p [[buffer(LUMINOUS_LENS_BUFFER_PARAMS)]],
    constant LuminousLensToneConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> frame [[texture(LUMINOUS_LENS_TEXTURE_FRAME)]],
    texture2d<float> exposure [[texture(LUMINOUS_LENS_TEXTURE_EXPOSURE)]],
    texture2d<float> blur [[texture(LUMINOUS_LENS_TEXTURE_BLUR)]],
    texture2d<float> clarityUp [[texture(LUMINOUS_LENS_TEXTURE_CLARITY)]],
    texture2d<float> halationUp [[texture(LUMINOUS_LENS_TEXTURE_HALATION)]],
    texture2d<float> bloomUp [[texture(LUMINOUS_LENS_TEXTURE_BLOOM)]])
{
    const float2 uv = in.uv;
    const float4 source = frame.sample(lensLinearClamp, uv, level(0.0));
    float3 c = source.rgb;

    if ((k.mode & LUMINOUS_LENS_TONE_CLARITY) != 0)
    {
        // how far the pixel's luminance stands from its surroundings', in stops, limited to two either way
        const float3 around = mix(blur.sample(lensLinearClamp, uv, level(0.0)).rgb,
                                  lensTent9(clarityUp, uv, k.clarityTexel), k.clarityWeight);
        const float l = max(dot(c, LENS_LUMA), 1e-5);
        const float a = max(dot(around, LENS_LUMA), 1e-5);
        const float detail = clamp(log2(l / a), -2.0, 2.0);
        c *= exp2(p.clarity * 0.6 * detail);
    }

    if ((k.mode & LUMINOUS_LENS_TONE_HALATION) != 0)
    {
        // light from brighter surroundings, red-orange, round the brightest edges; measured also after exposure
        const float3 around = mix(blur.sample(lensLinearClamp, uv, level(0.0)).rgb,
                                  lensTent9(halationUp, uv, k.halationTexel), k.halationWeight);
        const float3 spill = max(around - c, float3(0.0));
        const float h = min(p.halation, 1.0);
        const float more = clamp(p.halation - 1.0, 0.0, 1.0);
        const float lum = dot(around, LENS_LUMA);
        const float exposed = max(lum, lum * lensExposure(exposure));
        const float bright = smoothstep(mix(mix(0.8, 0.4, h * h), 0.25, more), mix(mix(4.0, 2.0, h * h), 1.5, more), exposed);
        c += spill * bright * float3(1.0, 0.32, 0.12) * (h * 1.5 + h * h * h * 4.5) * (1.0 + 1.5 * more);
    }

    if ((k.mode & LUMINOUS_LENS_TONE_BLOOM) != 0)
    {
        // up to 0.3 a blend toward the wide blur; above, its glow: the light the blur carries past an edge
        c = mix(c, lensTent9(bloomUp, uv, k.bloomTexel), p.bloomStrength);
        const float3 around = bloomUp.sample(lensLinearClamp, uv, level(0.0)).rgb;
        c += max(around - c, float3(0.0)) * p.bloomGlow * 3.0 * 0.8;
    }

    return float4(c, source.a);
}

// Bloom without its glow: the tent over its half-size level, drawn onto the frame with the strength as alpha.
fragment float4 luminous_lens_bloom_fs(
    LuminousFullscreenOut in [[stage_in]],
    constant LuminousLensParams& p [[buffer(LUMINOUS_LENS_BUFFER_PARAMS)]],
    constant LuminousLensToneConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> bloomUp [[texture(LUMINOUS_LENS_TEXTURE_BLOOM)]])
{
    return float4(lensTent9(bloomUp, in.uv, k.bloomTexel), p.bloomStrength);
}
