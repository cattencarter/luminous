/**
 * @file llgoldenwait.h
 * @brief A golden-hook wait's timeout in frames with a floor on the wall clock, where a texture in use stands for
 *        the scene's readiness, the eye at the test camera, the reflection probes' tie and what the readiness and a
 *        tour's unwind ask of the probes, in a tour's later stops, and the home a tour's later stop asks for
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

#ifndef LL_LLGOLDENWAIT_H
#define LL_LLGOLDENWAIT_H

#include "stdtypes.h"
#include "v3math.h"

#include <cstddef>
#include <string>
#include <vector>

/// [Viewer Lab] Counts one more frame of a wait at now (wall-clock seconds) and says whether the wait has run out: more
/// than limit_frames frames AND more than limit_seconds since its first frame. A frame count alone runs out in seconds
/// when the viewer draws far over 60 frames a second (vsync off; Metal draws the world at about 125, and far more with
/// little to draw), and the golden hook's test clock is fixed per frame, so the floor is the wall clock (for the scene's
/// loading and for the glTF run's wait). frames == 0 (a new wait, or one the caller set back) starts the clock.
bool ll_golden_wait_over(U32& frames, F64& since, F64 now, U32 limit_frames, F64 limit_seconds);

/// [Viewer Lab] Where a texture in use stands for the golden hook's readiness.
enum class LLGoldenTexture
{
    Settled, ///< at the detail it keeps
    Loading, ///< not there yet: a fetch runs, the viewer will ask for more, or an LOD texture will be scaled down
    Dropped  ///< coarser than it wants, and the viewer will never ask for more on its own (see below)
};

/// [Viewer Lab] A texture in use, as the golden hook sees it: fetching (a fetch runs or the texture holds a fetcher),
/// fully_loaded (LLViewerFetchedTexture::isFullyLoaded: its full size known, no fetch), scales_down (an LOD texture),
/// boosted (boost level BOOST_HIGH or above), priority (its max virtual size, updateFetch's decode priority), discard
/// (its level), desired (the level it wants) and max_discard (the most its size allows, which caps desired).
/// - Settled at the level it wants, and also when sharper than that, fully loaded and of a kind nothing scales down:
///   only LOD textures can be scaled down (LLViewerLODTexture::scaleDown; LLViewerFetchedTexture's does nothing), so
///   any other texture sharper than it wants stays so. A boosted LOD texture (BOOST_AVATAR_BAKED or above) or one with
///   mForceToSaveRawImage is not scaled down either (LLViewerLODTexture::processTextureStats), but the hook passes
///   scales_down for every LOD texture, so one of them sharper than it wants stays Loading and is still waited for, for
///   ever if its wanted level stays (scales_down changes only if such a stall shows up). In a
///   teleport's first frame with fetches pending,
///   LLViewerTextureList::clearFetchingRequests calls forceToDeleteRequest on every texture, which raises the wanted
///   level to max discard + 1; a fully loaded UI image at 0 keeps 0 and that level, since processTextureStats then
///   leaves it alone, and the hook would wait for it for ever (seen in the functional profile on Metal).
/// - Dropped when coarser than it wants, not fetching, boosted and at priority 0: the same call stops a fetch that is
///   under way and sets the priority to 0 (resetTextureStats), a boosted texture's priority comes only from its boost
///   (setBoostLevel) and explicit addTextureStats, never from faces (LLViewerTextureList::updateImageDecodePriority),
///   and updateFetch makes no request at priority 0, so it stays coarse. The hook asks for it again with its boost.
/// The hook's READY_FRAMES go over every texture's stats a few times (32 or 5 % a frame), so one whose wanted level
/// comes down and asks for more detail is fetched, and no longer settled, before the scene counts as ready.
LLGoldenTexture ll_golden_texture_state(bool fetching, bool fully_loaded, bool scales_down, bool boosted, F32 priority,
                                        S32 discard, S32 desired, S32 max_discard);

/// [Viewer Lab] Whether a texture with data is sharper than the level it wants, capped by its size (discard, desired and
/// max_discard as above): the textures ll_golden_texture_state settles for being sharper, which the hook's ready line
/// counts with those of them above level 0. A fetch the teleport's clear cut mid-way can leave a
/// texture there at a level that depends on timing: with a known draw size that is set but not changed
/// (setKnownDrawSize: icons with a minimum size, previews, inspect, profile), its wanted level stays at max discard + 1.
/// Logging only; the rule above does not change.
bool ll_golden_sharper_than_wanted(S32 discard, S32 desired, S32 max_discard);

/// [Viewer Lab] The eye at the test camera, before the reflection probes start over.
constexpr F32 LL_GOLDEN_EYE_STILL_DISTANCE = 1e-4f;     ///< metres the eye may move in a frame and count as still
constexpr F32 LL_GOLDEN_EYE_AT_CAMERA_DISTANCE = 0.01f; ///< metres from the scene's camera position that count as there
constexpr U32 LL_GOLDEN_EYE_STILL_FRAMES = 60;          ///< still frames in a row that count as settled anywhere
constexpr F64 LL_GOLDEN_EYE_STILL_SECONDS = 2.0;        ///< and at least this long on the wall clock

/// [Viewer Lab] Whether a run is a tour's stop after its first: the stops whose eye and probe rules below are
/// the tour's own. A fresh run and a tour's first stop (its anchor, a fresh login's run in every sense) keep the rules
/// of a run outside a tour, so their frames, their capture and the GL references are the same (stop counts from 0).
bool ll_golden_later_tour_stop(bool tour_active, size_t stop);

/// Counts one more frame of the eye on its way to the test camera and says whether it has settled there: moved is how
/// far it moved since the frame before, from_camera how far it is from the scene's camera position, now the wall clock.
/// Not in a later tour stop: still in this frame (at most LL_GOLDEN_EYE_STILL_DISTANCE), wherever it is, as in a
/// run outside a tour (still_frames and still_since are not touched).
/// In a later tour stop: still in this frame and at the camera (at most LL_GOLDEN_EYE_AT_CAMERA_DISTANCE), or still for
/// more than LL_GOLDEN_EYE_STILL_FRAMES frames and LL_GOLDEN_EYE_STILL_SECONDS in a row wherever it is, since the viewer
/// may keep the eye off the position (0.2 m from the water, LLViewerCamera::updateCameraLocation; above the land and
/// within the draw distance, LLAgentCamera::calcCameraPositionTargetGlobal). Still alone is not enough there: every scene
/// moves the avatar with a local teleport, and idle() skips updateCamera while gTeleportDisplay is set, which
/// update_tp_display clears only in the frame after the one where the avatar arrives and setCamera runs. So the first
/// frame after setCamera never moves the eye, which still stands where the stop before left it (a tour's stop after one
/// with another camera). A frame that moves sets
/// still_frames back to 0; still_since is the first still frame's wall-clock time (ll_golden_wait_over).
bool ll_golden_eye_settled(bool later_tour_stop, F32 moved, F32 from_camera, F64 now, U32& still_frames,
                           F64& still_since);

/// [Viewer Lab] A reflection probe as the tie check sees it: its origin and radius (LLReflectionMapManager::probeState).
struct LLGoldenProbe
{
    LLVector3 origin;
    F32 radius = 0.f;
};

/// The first two probes equally far from the eye (the distance to the origin less the radius, within 1e-4 relative), as
/// "the reflection probes at (x, y, z) and (x, y, z)", or empty. Probes take their slots in the cube map array by that
/// distance, and std::sort is not stable, so a tie gives slots in an order that follows the network: the golden hook
/// fails the run.
std::string ll_golden_probe_tie(const LLVector3& eye, const std::vector<LLGoldenProbe>& probes);

/// [Viewer Lab] The reflection probes as the golden hook's readiness and a tour's unwind see them.
struct LLGoldenProbes
{
    bool on = true;      ///< LLPipeline::sReflectionProbesEnabled (RenderReflectionsEnabled and the feature table)
    bool paused = false; ///< LLReflectionMapManager::isPaused: an environment transition paused the updates
};

/// Whether the scene waits for the probes (their tie and their updates): always outside a later tour stop; in a
/// later tour stop only with them on. Off,
/// LLReflectionMapManager::update() returns at once, so no probe is created or updated and no pause ends (only update()
/// and the environment's transitions end one), and updateUniforms() and setUniforms() return at once, so no probe is
/// drawn. A run that starts with them off has no probes and passes the wait at once; a tour's stop that turns them off
/// keeps the earlier stops' probes and any pause, and would wait for them for ever (a stop with the reflections off
/// stalls on "probe updates are paused").
bool ll_golden_probes_awaited(bool later_tour_stop, const LLGoldenProbes& probes);

/// A tour's unwind audit, after the unwind set the stop's settings back and resumed a pause the stop left (a
/// fresh run's scene starts with the updates running): why the probes are not as at the tour's start (on or off as then,
/// and not paused), or empty.
std::string ll_golden_probes_unwind_problem(const LLGoldenProbes& baseline, const LLGoldenProbes& now);

/// [Viewer Lab] A home as the golden hook asks the region for it: the position in the region (the scene's avatar
/// spot across the ground, the avatar's feet for the height) and the look-at (the avatar's at axis); set is false when
/// there is none.
struct LLGoldenHome
{
    LLVector3 position;
    LLVector3 look_at;
    bool set = false;
};

/// The home a scene asks the region for: its own (own) outside a later tour stop; in a later tour stop the tour's first
/// stop's (anchor), which is a fresh run's, or one with set false when the first stop asked for none (the hook fails
/// the stop then). A later stop's own home had the at axis the stop's local teleport left, for example (0, 1, 0), and
/// the login after the tour faced north from it, with the follow camera at x = 128 between the probes at x = 112 and
/// 144: every fresh run after the tour would fail the probe tie. So in a tour every home the region gets is the first
/// stop's, and the tour leaves the home as a fresh run of that scene does, whichever stop it ends or aborts at (the
/// first stop's home is asked for before any later stop runs).
LLGoldenHome ll_golden_home_to_ask(bool later_tour_stop, const LLGoldenHome& own, const LLGoldenHome& anchor);

#endif // LL_LLGOLDENWAIT_H
