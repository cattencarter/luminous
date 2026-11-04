// Anamorphic streaks: what an anamorphic lens draws from a bright light, a thin horizontal line in its tint.
//
// One threadgroup per row of the quarter-width target holds the row in its own memory: it takes the light over the
// threshold (each texel the average of its four frame pixels, lights in the sky narrower than a few pixels left
// out, so stars do not streak), then spreads it along the row in the translated passes' steps -- seven taps
// 1, 4, 16 ... texels apart, each weighted by the falloff over its distance, until the steps cover the streak's
// reach -- each step reading what the last left in memory, and writes the row once. The translated version draws
// a full-screen pass for each step. The steps are kept rather than an exact exponential filter: together they make
// a softer peak at the light than an exponential would, and that is the streak's look.
//
// The row is held as half floats, as the translated passes' RGB16F targets hold it between steps.

#include "lenscommon.h"
#include "luminous/common/fullscreen.h"

namespace luminous_lens
{
    constant uint STREAK_PER_THREAD = LUMINOUS_LENS_STREAK_MAX / LUMINOUS_LENS_STREAK_THREADS;

    inline float streakOver(texture2d<float> frame, int2 p, int2 last, float threshold)
    {
        return max(dot(frame.read(uint2(clamp(p, int2(0), last))).rgb, LENS_LUMA) - threshold, 0.0);
    }
}

kernel void luminous_lens_streak(
    constant LuminousLensParams& p [[buffer(LUMINOUS_LENS_BUFFER_PARAMS)]],
    constant LuminousLensStreakConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> frame [[texture(LUMINOUS_LENS_TEXTURE_FRAME)]],
    texture2d<float> exposure [[texture(LUMINOUS_LENS_TEXTURE_EXPOSURE)]],
    depth2d<float> depth [[texture(LUMINOUS_LENS_TEXTURE_DEPTH)]],
    texture2d<float, access::write> out [[texture(3)]],
    uint2 group [[threadgroup_position_in_grid]],
    uint t [[thread_index_in_threadgroup]])
{
    using namespace luminous_lens;
    threadgroup half rowR[LUMINOUS_LENS_STREAK_MAX];
    threadgroup half rowG[LUMINOUS_LENS_STREAK_MAX];
    threadgroup half rowB[LUMINOUS_LENS_STREAK_MAX];

    const uint y = group.y;
    const uint w = min(k.size.x, uint(LUMINOUS_LENS_STREAK_MAX));
    if (y >= k.size.y)
    {
        return; // the whole group
    }
    const int2 last = int2(frame.get_width(), frame.get_height()) - 1;
    const int2 depth_last = int2(depth.get_width(), depth.get_height()) - 1;
    const float threshold = 3.0 / max(lensExposure(exposure), 1e-4);
    const int r = int(p.streakErode + 0.5);
    const float3 gain = p.streakTint.xyz * 12.0 * p.streakGain;

    // the light over the threshold
    for (uint j = 0; j < STREAK_PER_THREAD; ++j)
    {
        const uint x = t + j * LUMINOUS_LENS_STREAK_THREADS;
        if (x >= w)
        {
            break;
        }
        const int2 base = int2(int(x) * 4, int(y));
        float3 sum = float3(0.0);
        for (int i = 0; i < 4; ++i)
        {
            const int2 at = base + int2(i, 0);
            const float3 s = frame.read(uint2(clamp(at, int2(0), last))).rgb;
            const float lum = dot(s, LENS_LUMA);
            float over = max(lum - threshold, 0.0);
            if (over > 0.0 && depth.read(uint2(clamp(at, int2(0), depth_last))) >= 1.0)
            {
                // in the sky: only as bright as the dimmest of a cross r pixels out
                over = min(over, min(min(streakOver(frame, at + int2(r, 0), last, threshold),
                                         streakOver(frame, at - int2(r, 0), last, threshold)),
                                     min(streakOver(frame, at + int2(0, r), last, threshold),
                                         streakOver(frame, at - int2(0, r), last, threshold))));
            }
            sum += s * (over / max(lum, 1e-4));
        }
        const half3 v = half3(sum * 0.25 * gain);
        rowR[x] = v.r;
        rowG[x] = v.g;
        rowB[x] = v.b;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // spread along the row, the taps 1, 4, 16 ... texels apart
    float covered = 0.0;
    for (int spacing = 1; covered < p.streakReach; spacing *= 4)
    {
        float weights[7];
        float total = 0.0;
        for (int i = -3; i <= 3; ++i)
        {
            weights[i + 3] = pow(p.streakDecay, float(abs(i) * spacing));
            total += weights[i + 3];
        }
        half3 spread[STREAK_PER_THREAD];
        for (uint j = 0; j < STREAK_PER_THREAD; ++j)
        {
            const uint x = t + j * LUMINOUS_LENS_STREAK_THREADS;
            if (x >= w)
            {
                break;
            }
            float3 sum = float3(0.0);
            for (int i = -3; i <= 3; ++i)
            {
                const uint at = uint(clamp(int(x) + i * spacing, 0, int(w) - 1));
                sum += float3(rowR[at], rowG[at], rowB[at]) * weights[i + 3];
            }
            spread[j] = half3(sum / total);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (uint j = 0; j < STREAK_PER_THREAD; ++j)
        {
            const uint x = t + j * LUMINOUS_LENS_STREAK_THREADS;
            if (x >= w)
            {
                break;
            }
            rowR[x] = spread[j].r;
            rowG[x] = spread[j].g;
            rowB[x] = spread[j].b;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        covered += 3.0 * float(spacing);
    }

    for (uint j = 0; j < STREAK_PER_THREAD; ++j)
    {
        const uint x = t + j * LUMINOUS_LENS_STREAK_THREADS;
        if (x >= w)
        {
            break;
        }
        out.write(float4(float(rowR[x]), float(rowG[x]), float(rowB[x]), 1.0), uint2(x, y));
    }
}

// The streaks added to the frame with bloom's up filter, the strength as alpha (source alpha, one).
fragment float4 luminous_lens_streak_fs(
    LuminousFullscreenOut in [[stage_in]],
    constant LuminousLensParams& p [[buffer(LUMINOUS_LENS_BUFFER_PARAMS)]],
    constant LuminousLensStreakConstants& k [[buffer(LUMINOUS_LENS_BUFFER_PASS)]],
    texture2d<float> streak [[texture(3)]])
{
    return float4(lensTent9(streak, in.uv, k.texel), p.streak);
}
