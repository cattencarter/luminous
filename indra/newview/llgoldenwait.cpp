/**
 * @file llgoldenwait.cpp
 * @brief A golden-hook wait's timeout in frames with a floor on the wall clock, and where a texture in use stands
 *        for the scene's readiness
 *
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Viewer Lab contributors
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

#include "llviewerprecompiledheaders.h"

#include "llgoldenwait.h"

#include "llformat.h"

#include <algorithm>
#include <utility>

bool ll_golden_wait_over(U32& frames, F64& since, F64 now, U32 limit_frames, F64 limit_seconds)
{
    if (frames == 0)
    {
        since = now;
    }
    ++frames;
    return frames > limit_frames && now - since > limit_seconds;
}

LLGoldenTexture ll_golden_texture_state(bool fetching, bool fully_loaded, bool scales_down, bool boosted, F32 priority,
                                        S32 discard, S32 desired, S32 max_discard)
{
    const S32 wanted = llmin(desired, max_discard);
    if (fetching || discard < 0)
    {
        return LLGoldenTexture::Loading;
    }
    if (discard == wanted)
    {
        return LLGoldenTexture::Settled;
    }
    if (discard < wanted)
    {
        // Sharper than wanted: an LOD texture is scaled down later, any other stays as it is.
        return fully_loaded && !scales_down ? LLGoldenTexture::Settled : LLGoldenTexture::Loading;
    }
    return boosted && priority <= 0.f ? LLGoldenTexture::Dropped : LLGoldenTexture::Loading;
}

bool ll_golden_sharper_than_wanted(S32 discard, S32 desired, S32 max_discard)
{
    return discard >= 0 && discard < llmin(desired, max_discard);
}

bool ll_golden_later_tour_stop(bool tour_active, size_t stop)
{
    return tour_active && stop > 0;
}

bool ll_golden_eye_settled(bool later_tour_stop, F32 moved, F32 from_camera, F64 now, U32& still_frames,
                           F64& still_since)
{
    if (!later_tour_stop)
    {
        return moved <= LL_GOLDEN_EYE_STILL_DISTANCE;
    }
    if (moved > LL_GOLDEN_EYE_STILL_DISTANCE)
    {
        still_frames = 0;
        return false;
    }
    const bool over = ll_golden_wait_over(still_frames, still_since, now, LL_GOLDEN_EYE_STILL_FRAMES,
                                          LL_GOLDEN_EYE_STILL_SECONDS);
    return from_camera <= LL_GOLDEN_EYE_AT_CAMERA_DISTANCE || over;
}

std::string ll_golden_probe_tie(const LLVector3& eye, const std::vector<LLGoldenProbe>& probes)
{
    std::vector<std::pair<F32, std::string>> distances;
    for (const LLGoldenProbe& probe : probes)
    {
        distances.emplace_back(dist_vec(eye, probe.origin) - probe.radius,
                               llformat("(%.1f, %.1f, %.1f)", probe.origin.mV[VX], probe.origin.mV[VY],
                                        probe.origin.mV[VZ]));
    }
    std::sort(distances.begin(), distances.end());
    for (size_t i = 1; i < distances.size(); ++i)
    {
        if (distances[i].first - distances[i - 1].first <= 1e-4f * llmax(1.f, fabsf(distances[i].first)))
        {
            return "the reflection probes at " + distances[i - 1].second + " and " + distances[i].second;
        }
    }
    return std::string();
}

bool ll_golden_probes_awaited(bool later_tour_stop, const LLGoldenProbes& probes)
{
    return !later_tour_stop || probes.on;
}

std::string ll_golden_probes_unwind_problem(const LLGoldenProbes& baseline, const LLGoldenProbes& now)
{
    if (now.on != baseline.on)
    {
        return std::string("the reflection probes are ") + (now.on ? "on" : "off") + ", not " +
               (baseline.on ? "on" : "off") + " as at the tour's start";
    }
    if (now.paused)
    {
        return "the reflection probe updates are still paused";
    }
    return std::string();
}

LLGoldenHome ll_golden_home_to_ask(bool later_tour_stop, const LLGoldenHome& own, const LLGoldenHome& anchor)
{
    return later_tour_stop ? anchor : own;
}
