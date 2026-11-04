/**
 * @file llgoldenwait_test.cpp
 * @brief The golden hook's wait timeout: frames and a wall-clock floor (P2-R3's D1, F28); where a texture in use
 *        stands for the scene's readiness (P3-01j); the eye at the test camera, the probes' tie and what the readiness
 *        and a tour's unwind ask of the probes (P3-T5)
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

#include "../llviewerprecompiledheaders.h"

#include "../test/lltut.h"

#include "../llgoldenwait.h"

namespace tut
{
    struct goldenwait
    {
    };

    typedef test_group<goldenwait, 14> goldenwait_t;
    typedef goldenwait_t::object goldenwait_object_t;
    tut::goldenwait_t tut_goldenwait("LLGoldenWait");

    // More frames than the limit within a second (a fast backend) is not over until the seconds have passed too.
    template<> template<>
    void goldenwait_object_t::test<1>()
    {
        U32 frames = 0;
        F64 since = 0.0;
        for (U32 i = 0; i < 100; ++i)
        {
            ensure("over after " + std::to_string(i + 1) + " frames in a second", !ll_golden_wait_over(frames, since, 10.0 + i * 0.01, 60, 30.0));
        }
        ensure_equals("the clock starts at the first frame", since, 10.0);
        ensure("over once the seconds have passed too", ll_golden_wait_over(frames, since, 40.5, 60, 30.0));
    }

    // The frame count stays a floor: a slow wait is not over before its frames, however long it took.
    template<> template<>
    void goldenwait_object_t::test<2>()
    {
        U32 frames = 0;
        F64 since = 0.0;
        for (U32 i = 0; i < 60; ++i)
        {
            ensure("over before the frames", !ll_golden_wait_over(frames, since, 100.0 * i, 60, 30.0));
        }
        ensure("over at frame 61 after both", ll_golden_wait_over(frames, since, 6000.0, 60, 30.0));
    }

    // Setting the frames back to 0 starts a new wait with its own clock.
    template<> template<>
    void goldenwait_object_t::test<3>()
    {
        U32 frames = 0;
        F64 since = 0.0;
        for (U32 i = 0; i < 70; ++i)
        {
            ll_golden_wait_over(frames, since, 1.0, 60, 30.0);
        }
        ensure("over", ll_golden_wait_over(frames, since, 32.0, 60, 30.0));
        frames = 0;
        ensure("a new wait is not over", !ll_golden_wait_over(frames, since, 50.0, 60, 30.0));
        ensure_equals("its clock", since, 50.0);
    }

    // The arguments: fetching, fully_loaded, scales_down, boosted, priority, discard, desired, max_discard.
    const F32 BOOST_PRIORITY = 2048.f * 2048.f; // what setBoostLevel gives a boosted texture

    // A texture at the level it wants, capped by its size, has settled unless something fetches it, whatever its kind.
    template<> template<>
    void goldenwait_object_t::test<4>()
    {
        for (bool lod : {false, true})
        {
            const std::string kind = lod ? "LOD texture" : "fetched texture";
            ensure(kind + " at its level", ll_golden_texture_state(false, true, lod, false, 100.f, 2, 2, 5) == LLGoldenTexture::Settled);
            ensure(kind + " at its size's cap", ll_golden_texture_state(false, true, lod, false, 100.f, 5, 6, 5) == LLGoldenTexture::Settled);
            ensure(kind + " at its level, size not known", ll_golden_texture_state(false, false, lod, true, 0.f, 2, 2, 5) == LLGoldenTexture::Settled);
            ensure(kind + " fetching", ll_golden_texture_state(true, false, lod, false, 100.f, 2, 2, 5) == LLGoldenTexture::Loading);
            ensure(kind + " fetching, no data yet", ll_golden_texture_state(true, false, lod, true, BOOST_PRIORITY, -1, 0, 5) == LLGoldenTexture::Loading);
        }
    }

    // P3-01j: a fetched texture sharper than it wants (a UI image at 0 that forceToDeleteRequest gave 6, its max discard
    // 5) is never scaled down, so it has settled once fully loaded; an LOD texture there waits for its scaleDown.
    template<> template<>
    void goldenwait_object_t::test<5>()
    {
        ensure("the UI image at 0 wanting 6 has settled", ll_golden_texture_state(false, true, false, true, 0.f, 0, 6, 5) == LLGoldenTexture::Settled);
        ensure("a fetched texture at 1 wanting 3 has settled", ll_golden_texture_state(false, true, false, false, 100.f, 1, 3, 5) == LLGoldenTexture::Settled);
        ensure("an LOD texture sharper than it wants waits for its scaleDown", ll_golden_texture_state(false, true, true, false, 100.f, 0, 6, 5) == LLGoldenTexture::Loading);
        ensure("not fully loaded, it has not settled", ll_golden_texture_state(false, false, false, true, 0.f, 0, 6, 5) == LLGoldenTexture::Loading);
        ensure("fetching, it has not settled", ll_golden_texture_state(true, false, false, true, 0.f, 0, 6, 5) == LLGoldenTexture::Loading);
    }

    // P3-01j: coarser than wanted is never settled. A boosted texture at priority 0 (the UI image at 5 wanting 0 whose
    // fetch forceToDeleteRequest stopped) was dropped; with any priority, or unboosted (its faces give it one), it loads.
    template<> template<>
    void goldenwait_object_t::test<6>()
    {
        ensure("the UI image at 5 wanting 0 at priority 0 was dropped", ll_golden_texture_state(false, false, false, true, 0.f, 5, 0, 5) == LLGoldenTexture::Dropped);
        ensure("an LOD texture boosted at priority 0 was dropped too", ll_golden_texture_state(false, false, true, true, 0.f, 3, 1, 5) == LLGoldenTexture::Dropped);
        ensure("boosted with its boost's priority it loads", ll_golden_texture_state(false, false, false, true, BOOST_PRIORITY, 5, 0, 5) == LLGoldenTexture::Loading);
        ensure("unboosted at priority 0 it loads", ll_golden_texture_state(false, false, true, false, 0.f, 3, 1, 5) == LLGoldenTexture::Loading);
        ensure("boosted at priority 0 but fetching it loads", ll_golden_texture_state(true, false, false, true, 0.f, 5, 0, 5) == LLGoldenTexture::Loading);
    }

    // P3-01j's review R2: the ready line's "settled sharper" counts a texture with data sharper than the level it wants,
    // capped by its size, and every such texture with full size known is one ll_golden_texture_state settles when
    // nothing scales it down. The arguments: discard, desired, max_discard.
    template<> template<>
    void goldenwait_object_t::test<7>()
    {
        ensure("the UI image at 0 wanting 6, its cap 5, is sharper", ll_golden_sharper_than_wanted(0, 6, 5));
        ensure("a cut fetch at 2 wanting 6, its cap 5, is sharper", ll_golden_sharper_than_wanted(2, 6, 5));
        ensure("a fetched texture at 1 wanting 3 is sharper", ll_golden_sharper_than_wanted(1, 3, 5));
        ensure("at the level it wants it is not", !ll_golden_sharper_than_wanted(2, 2, 5));
        ensure("at its size's cap it is not", !ll_golden_sharper_than_wanted(5, 6, 5));
        ensure("coarser than it wants it is not", !ll_golden_sharper_than_wanted(5, 0, 5));
        ensure("without data it is not", !ll_golden_sharper_than_wanted(-1, 6, 5));
        for (S32 discard = -1; discard <= 6; ++discard)
        {
            for (S32 desired = 0; desired <= 6; ++desired)
            {
                if (ll_golden_sharper_than_wanted(discard, desired, 5))
                {
                    const std::string at = "at " + std::to_string(discard) + " wanting " + std::to_string(desired);
                    ensure(at + ": settled when fully loaded and not scaled down",
                           ll_golden_texture_state(false, true, false, true, 0.f, discard, desired, 5) == LLGoldenTexture::Settled);
                    ensure(at + ": loading as an LOD texture", ll_golden_texture_state(false, true, true, false, 100.f, discard, desired, 5) == LLGoldenTexture::Loading);
                }
            }
        }
    }

    // P3-T5: batch 4's proof, midhigh/overview's spot. The avatar at x = 128 between the probes at x = 112 and 144, the
    // follow camera behind it at the same x, and the test camera at (127.5, 124, 27.5).
    const std::vector<LLGoldenProbe> BATCH4_PROBES = { { LLVector3(112.f, 112.f, 27.f), 8.f },
                                                       { LLVector3(144.f, 112.f, 27.f), 8.f } };
    const LLVector3 FOLLOW_EYE(128.f, 106.f, 28.f);

    // The eye's way from the follow camera to camera as updateCamera's smoothing takes it (0.44 a frame at 60 frames a
    // second), with a frame where it does not move first (setCamera's first frame) and one on the way. Each frame goes
    // through ll_golden_eye_settled; the eye where it first counts as settled, and that frame, come back.
    LLVector3 settle_eye(const LLVector3& camera, U32& frame)
    {
        LLVector3 eye = FOLLOW_EYE;
        LLVector3 last = eye;
        U32 still_frames = 0;
        F64 still_since = 0.0;
        for (frame = 1; frame <= 200; ++frame)
        {
            if (frame != 1 && frame != 5)
            {
                eye = lerp(eye, camera, 0.44f);
            }
            if (ll_golden_eye_settled(true, dist_vec(eye, last), dist_vec(eye, camera), frame / 60.0, still_frames, still_since))
            {
                return eye;
            }
            last = eye;
        }
        return eye;
    }

    // The race (P3-T2 S1's F3, batch 4): in setCamera's first frame the eye stood still at the follow camera, which the
    // old rule (still in the last frame) took for settled, and the tie check failed there. In a later tour stop the eye
    // counts as settled only at the test camera, where the probes are not equally far, and not in the frame it stops on
    // the way (P3-T5b: there the first frame stands at the stop before's camera).
    template<> template<>
    void goldenwait_object_t::test<8>()
    {
        ensure_equals("the follow camera's eye ties the two probes (batch 4's failure)",
                      ll_golden_probe_tie(FOLLOW_EYE, BATCH4_PROBES),
                      std::string("the reflection probes at (112.0, 112.0, 27.0) and (144.0, 112.0, 27.0)"));
        U32 still_frames = 0;
        F64 still_since = 0.0;
        ensure("not settled where it stood still in setCamera's first frame",
               !ll_golden_eye_settled(true, 0.f, dist_vec(FOLLOW_EYE, LLVector3(127.5f, 124.f, 27.5f)), 1.0, still_frames,
                                      still_since));
        U32 frame = 0;
        const LLVector3 camera(127.5f, 124.f, 27.5f);
        const LLVector3 eye = settle_eye(camera, frame);
        ensure("it settled", frame <= 200);
        ensure("not before the frame it stopped on the way", frame > 5);
        ensure("at the test camera", dist_vec(eye, camera) <= LL_GOLDEN_EYE_AT_CAMERA_DISTANCE);
        ensure_equals("no tie there", ll_golden_probe_tie(eye, BATCH4_PROBES), std::string());
    }

    // A real tie still fails: a test camera as far from both probes (x = 128) ties them once the eye is there, and
    // distances within 1e-4 of each other tie, while 1 cm apart at 20 m do not.
    template<> template<>
    void goldenwait_object_t::test<9>()
    {
        U32 frame = 0;
        const LLVector3 camera(128.f, 124.f, 27.5f);
        const LLVector3 eye = settle_eye(camera, frame);
        ensure("it settled at the camera", frame <= 200 && dist_vec(eye, camera) <= LL_GOLDEN_EYE_AT_CAMERA_DISTANCE);
        ensure("the tie there is found", !ll_golden_probe_tie(eye, BATCH4_PROBES).empty());
        const LLVector3 origin(0.f, 0.f, 0.f);
        ensure("20 m and 20.001 m tie",
               !ll_golden_probe_tie(origin, { { LLVector3(20.f, 0.f, 0.f), 0.f }, { LLVector3(0.f, 20.001f, 0.f), 0.f } }).empty());
        ensure_equals("20 m and 20.01 m do not",
                      ll_golden_probe_tie(origin, { { LLVector3(20.f, 0.f, 0.f), 0.f }, { LLVector3(0.f, 20.01f, 0.f), 0.f } }),
                      std::string());
        ensure("the radius counts",
               !ll_golden_probe_tie(origin, { { LLVector3(20.f, 0.f, 0.f), 0.f }, { LLVector3(0.f, 25.f, 0.f), 5.f } }).empty());
        ensure_equals("one probe has no tie", ll_golden_probe_tie(origin, { { LLVector3(20.f, 0.f, 0.f), 0.f } }), std::string());
    }

    // An eye the viewer keeps off the camera's position (0.2 m from the water) counts as settled once still for the
    // frames and the seconds in a row, not before, however fast the frames come; a frame that moves starts over.
    template<> template<>
    void goldenwait_object_t::test<10>()
    {
        U32 still_frames = 0;
        F64 still_since = 0.0;
        for (U32 i = 0; i < 1000; ++i)
        {
            ensure("not settled within the seconds", !ll_golden_eye_settled(true, 0.f, 0.2f, 10.0 + i * 0.001, still_frames, still_since));
        }
        ensure("settled after both", ll_golden_eye_settled(true, 0.f, 0.2f, 12.5, still_frames, still_since));
        ensure("a frame that moves is not settled", !ll_golden_eye_settled(true, 0.01f, 0.2f, 12.6, still_frames, still_since));
        ensure_equals("and starts the count over", still_frames, 0U);
        for (U32 i = 0; i < LL_GOLDEN_EYE_STILL_FRAMES; ++i)
        {
            ensure("not settled within the frames", !ll_golden_eye_settled(true, 0.f, 0.2f, 20.0 + i, still_frames, still_since));
        }
        ensure("settled after the frames too", ll_golden_eye_settled(true, 0.f, 0.2f, 100.0, still_frames, still_since));
        still_frames = 0;
        ensure("at the camera, one still frame is enough", ll_golden_eye_settled(true, 0.f, 0.005f, 0.0, still_frames, still_since));
        ensure("at the camera but moving is not", !ll_golden_eye_settled(true, 0.001f, 0.005f, 0.0, still_frames, still_since));
    }

    // The readiness with the probes off (batch 4's proof, stop 5 mh-reflections-off: off and paused, which nothing
    // ended, so the stop waited for "probe updates are paused" for ever): it no longer waits for them. On, it waits,
    // also while paused, since update() ends the pause.
    template<> template<>
    void goldenwait_object_t::test<11>()
    {
        ensure("off and paused (batch 4's stop 5): not awaited", !ll_golden_probes_awaited(true, { false, true }));
        ensure("off: not awaited", !ll_golden_probes_awaited(true, { false, false }));
        ensure("on and paused: awaited", ll_golden_probes_awaited(true, { true, true }));
        ensure("on: awaited", ll_golden_probes_awaited(true, { true, false }));
    }

    // A tour's unwind audit after the probes-off stop: the setting back on turns the probes on, and the pause the stop
    // left must be gone (the unwind resumes it), as a fresh run's scene starts; probes off, or on where the tour began
    // with them off, fail.
    template<> template<>
    void goldenwait_object_t::test<12>()
    {
        const LLGoldenProbes on = { true, false };
        ensure_equals("on and running as at the start", ll_golden_probes_unwind_problem(on, { true, false }), std::string());
        ensure_equals("the pause left without the resume",
                      ll_golden_probes_unwind_problem(on, { true, true }),
                      std::string("the reflection probe updates are still paused"));
        ensure_equals("still off",
                      ll_golden_probes_unwind_problem(on, { false, false }),
                      std::string("the reflection probes are off, not on as at the tour's start"));
        const LLGoldenProbes off = { false, false };
        ensure_equals("off as at the start", ll_golden_probes_unwind_problem(off, { false, false }), std::string());
        ensure_equals("on where the tour began with them off",
                      ll_golden_probes_unwind_problem(off, { true, false }),
                      std::string("the reflection probes are on, not off as at the tour's start"));
    }

    // P3-T5b: the rules of a tour's later stops apply there only. A fresh run and a tour's first stop (its anchor) keep
    // the rule from before tours, so their frames, their capture and the GL references do not move (Fix B's rule for
    // every run changed ultra/overview-threads and ultra/gltf-raycast on GL in batch 5): still in the last frame counts
    // as settled wherever the eye is, also at the follow camera 18 m off in setCamera's first frame, and the probes are
    // waited for even when off (a run that starts with them off has none).
    template<> template<>
    void goldenwait_object_t::test<13>()
    {
        ensure("a fresh run is no later tour stop", !ll_golden_later_tour_stop(false, 0) && !ll_golden_later_tour_stop(false, 3));
        ensure("a tour's anchor is none", !ll_golden_later_tour_stop(true, 0));
        ensure("its second stop is one", ll_golden_later_tour_stop(true, 1));
        ensure("and its last", ll_golden_later_tour_stop(true, 9));
        U32 still_frames = 7;
        F64 still_since = 3.0;
        const F32 follow = dist_vec(FOLLOW_EYE, LLVector3(127.5f, 124.f, 27.5f));
        ensure("fresh: still at the follow camera counts as settled, as before tours",
               ll_golden_eye_settled(false, 0.f, follow, 1.0, still_frames, still_since));
        ensure("fresh: at the boundary too", ll_golden_eye_settled(false, LL_GOLDEN_EYE_STILL_DISTANCE, follow, 1.0, still_frames, still_since));
        ensure("fresh: moving is not settled, even at the camera", !ll_golden_eye_settled(false, 2e-4f, 0.f, 1.0, still_frames, still_since));
        ensure("fresh: the still count is not touched", still_frames == 7 && still_since == 3.0);
        ensure("later stop: the same frame at the follow camera is not settled",
               !ll_golden_eye_settled(true, 0.f, follow, 1.0, still_frames, still_since));
        ensure("fresh: probes off and paused are still waited for, as before tours", ll_golden_probes_awaited(false, { false, true }));
        ensure("fresh: probes off are waited for", ll_golden_probes_awaited(false, { false, false }));
        ensure("fresh: probes on are waited for", ll_golden_probes_awaited(false, { true, false }));
    }

    // P3-T5b: the home a scene asks the region for. Outside a later tour stop its own, as before tours (a fresh run's and
    // the anchor's request do not move); in a later stop the anchor's, whatever the stop's own is (proof 1: a later
    // stop's own look-at was (0, 1, 0), and every fresh run after the tour then faced north at login and failed the
    // probe tie), and none when the anchor asked for none (the hook fails the stop then).
    template<> template<>
    void goldenwait_object_t::test<14>()
    {
        LLGoldenHome own;
        own.position = LLVector3(128.f, 110.f, 25.2335f);
        own.look_at = LLVector3(-2.4e-07f, 1.f, 0.f);
        own.set = true;
        LLGoldenHome anchor;
        anchor.position = LLVector3(128.f, 110.f, 25.211655f);
        anchor.look_at = LLVector3(0.7609197f, 0.6488462f, 0.f);
        anchor.set = true;
        const LLGoldenHome fresh = ll_golden_home_to_ask(ll_golden_later_tour_stop(false, 0), own, anchor);
        ensure("a fresh run asks for its own home", fresh.set && fresh.position == own.position && fresh.look_at == own.look_at);
        const LLGoldenHome first = ll_golden_home_to_ask(ll_golden_later_tour_stop(true, 0), own, LLGoldenHome());
        ensure("a tour's anchor asks for its own home", first.set && first.position == own.position && first.look_at == own.look_at);
        const LLGoldenHome later = ll_golden_home_to_ask(ll_golden_later_tour_stop(true, 1), own, anchor);
        ensure("a later stop asks for the anchor's home", later.set && later.position == anchor.position && later.look_at == anchor.look_at);
        const LLGoldenHome last = ll_golden_home_to_ask(ll_golden_later_tour_stop(true, 8), own, anchor);
        ensure("and the last stop too", last.set && last.position == anchor.position && last.look_at == anchor.look_at);
        ensure("a later stop without the anchor's home asks for none",
               !ll_golden_home_to_ask(ll_golden_later_tour_stop(true, 2), own, LLGoldenHome()).set);
    }
}
