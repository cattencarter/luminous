/**
 * @file llgoldentest.cpp
 * @brief Test hooks for golden-image tests of the viewer, behind --golden (level 2)
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

#include "llgoldentest.h"
#include "llgoldenwait.h" // [Viewer Lab]

#include "gltf/asset.h"
#include "gltfscenemanager.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llchannelmanager.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldrawable.h"
#include "lldynamictexture.h"
#include "llenvironment.h"
#include "llface.h"
#include "llfeaturemanager.h"
#include "llfetchedgltfmaterial.h"
#include "llfile.h"
#include "llfloater.h"
#include "llfloaterbvhpreview.h"
#include "llfloatermodelpreview.h"
#include "llfloaterreg.h"
#include "llfloatersnapshot.h"
#include "llfocusmgr.h"
#include "llglslshader.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include "llgltfmateriallist.h"
#include "llimagegl.h"
#include "llimagepng.h"
#include "lllineeditor.h"
#include "llmanip.h"
#include "llmaniptranslate.h"
#include "llmd5.h"
#include "llmediactrl.h"
#include "llmediaentry.h"
#include "llmeshrepository.h"
#include "llmodaldialog.h"
#include "llmodelpreview.h"
#include "llnotifications.h"
#include "llperfstats.h"
#include "llpluginclassmedia.h"
#include "llrand.h"
#include "llrhi.h"
#include "llrhimetal.h"
#include "llrendertarget.h"
#include "llrootview.h"
#include "llscenemonitor.h"
#include "llsdjson.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "llselectmgr.h"
#include "llskinningutil.h"
#include "llsnapshotlivepreview.h"
#include "llspatialpartition.h"
#include "llstartup.h"
#include "llstatusbar.h"
#include "llsys.h"
#include "llterrainpaintmap.h"
#include "lltexturefetch.h"
#include "lltoolcomp.h"
#include "lltoolmgr.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewermedia.h"
#include "llviewermediaobserver.h"
#include "llviewernetwork.h"
#include "llviewerpartsim.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llvocache.h"
#include "llvosurfacepatch.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"
#if LL_RHI_METAL
#include "llwindowmacosx-objc.h" // the window's bounds and frame, and the test backing scale
#endif


#include <algorithm>
#include <boost/json.hpp>
#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <unordered_set>

#if LL_DARWIN
extern bool gHiDPISupport; // llwindowmacosx.cpp: RenderHiDPI, read at startup
#endif
extern S32 gDebugRaycastGLTFNodeHit;      // llviewerwindow.cpp: the glTF node the debug ray hit, or -1
extern S32 gDebugRaycastGLTFPrimitiveHit; // and its primitive
extern bool gGoldenDrawUnloadedSelf;      // lldrawpoolavatar.cpp: the own avatar draws while the hook times it

// [Viewer Lab] The config is LLSD XML (tests/golden/README.md, level 2):
//   snapshot        the PNG file to write the presented frame to
//   render_profile  the JSON file to write the render profile to, next to the snapshot
//   graphics_level  the graphics level (0-6) and
//   gpu_class       the GPU class (0-5) to lock, before the pipeline is made
//   settings        {name: value} set on top of the graphics level
//   window          [width, height] of the window's content in points: in pixels at RenderHiDPI 0 (every scene); at
//                   RenderHiDPI 1 on macOS points x the window's backing scale (getSystemUISize) in pixels, as
//                   AppKit sizes windows in points (tests/golden/README.md)
//   warmup_frames   frames to render with the scene set before the readback
//   avatar          where to move the avatar in the region, out of the camera's view
//   camera          {position, focus} in region coordinates
//   materials       the scene's glTF material assets, waited for before the warm-up
//   particles       false hides particles altogether (RenderMaxPartCount 0 still lets one through)
//   quit            quit after the snapshot
//   program_fault   {program, deferred, times}: golden level 2's test fault (LL::RHI::ProgramFault),
//                   armed at startup before the first shader load; optional
//   program_faults  [{program, times}]: Metal's fault list beside it (LL::RHI::setMetalProgramFaults),
//                   each name distinct from the others and from program_fault's, deferred as program_fault;
//                   armed with program_fault, which it needs; Metal builds only; optional
//   program_delay   {program, frames}: golden level 2's test delay (LL::RHI::ProgramDelay): every
//                   creation of the program stays Pending for frames frames, so that the frame's trigger
//                   publishes it; armed with program_fault; optional
//   login_screen    frames to show at the login screen before the readback, after which the run quits
//                   there without logging in; such a run needs none of the in-world keys. The mouse
//                   must be outside the window at the readback, as with show_ui
//   login_page      with login_screen: the absolute path of a local HTML file the login panel's browser shows in
//                   place of the grid's login page, whose address has the build's revision and the OS version
//   cached_settings {name: [type, value]}: settings only an LLCachedControl declares, Boolean or String,
//                   declared with the run's value before the pipeline reads them; optional
//   show_ui         true keeps the UI, name tags and HUD text in the snapshot: the hook keeps
//                   RENDER_DEBUG_FEATURE_UI on instead of off, draws no avatar (its name tag shows), hides the
//                   status bar's clock, mutes the toast channels and leaves the account's home as it is; the
//                   mouse must stay outside the window. Not with login_screen; optional
//   floaters        with show_ui: [{name, rect: [left, top, right, bottom], hide: [child, ...]}], floaters the
//                   hook opens without focus, in this order, once the camera is set, at the rect in the floater
//                   view's UI coordinates, with the named children hidden (optional; for widgets that follow
//                   the wall clock); each must still have its rect at the snapshot; optional
//   window_origin   with show_ui: [x, y], where the hook puts the window's frame once it has its size (on macOS
//                   from the screen's bottom left), so that the mouse can stay outside it; optional
//   previews        with show_ui and previews_result: the preview run's steps, after the warm-up, each
//                   {name, floater, frames, ...} on a fixed number of frames: at its first frame the step opens its
//                   floater (upload_image, upload_model or upload_anim_bvh) with file at rect [left, top, right,
//                   bottom], the floater's own size, or changes the open one: combo [child, index] selects and
//                   commits, check [child, ...] checks and commits, click [child, ...] clicks, physics loads the
//                   model's physics from a file, and hide [child, ...] keeps those children of the floater hidden
//                   from then on (the model floater's file field shows the file's absolute path). At its last frame
//                   the floater must be at its rect with its preview rendered and the hidden children hidden, and
//                   the step saves the floater's part of the frame and readBackRaw of its preview textures. The
//                   run's snapshot is the last step's frame; optional
//   previews_result with previews: the JSON file, ending .previews.json, to write the steps' results to
//   paintmap_check  true: after the snapshot (and after the transfer checks, when there are any) the hook bakes the PBR
//                   paint map from the region's heights into a texture of its own, as Develop > Terrain > Create Local
//                   Paintmap does (LLAdvancedTerrainCreateLocalPaintMap) but without setPaintMap or
//                   LocalTerrainPaintEnabled, so the terrain draws as before; reads level 0, level 1 and the last level
//                   of the device texture (its description's, as LLImageGL describes it, at most
//                   MAX_DISCARD_LEVEL below level 0, on both backends: GL's GL_TEXTURE_MAX_LEVEL stops
//                   glGenerateMipmap there)
//                   back through the readback facade (LLGPUReadback) and rewrites the render profile with
//                   measured.paintmap {result, size, levels, checksums}, the bake's result, level 0's width and height
//                   and the levels' MD5s. The
//                   bake's program exists only with LocalTerrainPaintEnabled (LLViewerShaderMgr), which the scene sets
//                   in its settings. In-world only; optional
//   avatar_gpu      true: after the snapshot (and after the transfer checks and the paint map check, when there are
//                   any) the hook times the own avatar's GPU time in the next idles:
//                   LLPipeline::profileAvatar(self) AVATAR_PROFILES times, as auto-tune does, each after the last
//                   one's result was read, then profileAvatar(self, true) once, as the Performance floater does, and
//                   one empty span (a timer query's begin and end with no draw, in the profile's pass state).
//                   gGoldenDrawUnloadedSelf (lldrawpoolavatar.cpp, test-only) is set only around the
//                   profileAvatar calls, so the own avatar, never fully loaded on the local region, draws instead of
//                   the debug sphere. The hook rewrites the render profile with
//                   measured.avatar_gpu {median_ms, spread_ms, times_ms, attachments, attachment_ms, max_art_ms,
//                   too_slow, empty_span_ms, timers} (tests/golden/level2/avatar-times.py) and never collects the
//                   profile again, so no compared key changes. In-world only; optional
//   select          with show_ui (renderSelections draws only with the UI): [name, ...], the scene's objects to select.
//                   While the scene loads, the hook finds each as the region's root object nearest its
//                   expected_positions entry (within 0.5 m), selects it with its family in this order
//                   (LLSelectMgr::selectObjectAndFamily) and makes tool the current tool (selected in the basic
//                   toolset, which stays the current one). Object names come only with the properties the region sends for a
//                   selection, so the scene is ready only once every node has them and its name is the one named (any
//                   other name fails the run), and once the account may move (translate) or modify (scale) every selected
//                   object, which the hook asks the region for with a full update of each (a cache miss): only then does
//                   renderSelections draw the manipulator. The selection settles before the reflection probes start over. The
//                   render profile has measured.selection {names, positions {name: [x, y, z]}} in region coordinates
//                   and measured.manip {tool, highlighted, captured}: the manipulator's highlighted part
//                   (getHighlightedPart, none with the mouse outside the window) and whether it holds the mouse; optional
//   expected_positions with select: {name: [x, y, z]}, each selected object's place from make-scene.py, in region
//                   coordinates (level2.py fails a run whose object is more than 1 mm from it)
//   tool            with select: translate (LLToolCompTranslate) or scale (LLToolCompScale)
//   drag            with tool translate: xy_plane. At the warm-up's start the hook probes the manipulator's
//                   highlightManipulators over a fixed grid of window points (the UI's GL coordinates) around the
//                   selection centre's projection, takes the first that highlights the XY plane handle and starts a
//                   drag there (handleMouseDownOnPart, never a mouse-up); from then on LLGoldenTest::beforeUI pins the
//                   mouse there in every frame, so the snap guides draw their grid. measured.manip.drag {point, probes}.
//                   SnapToMouseCursor must be off, as it would move the system's cursor; optional
//   monitor_check   true: the scene monitor (SceneLoadingMonitorEnabled, which the scene sets) must be on. Over the
//                   warm-up the hook counts the frames in which the monitor's last capture target changed (capture
//                   swaps its two targets), and at the snapshot it reads that target back through the readback facade
//                   (LLGPUReadback): measured.monitor {captures, size, checksum}. In-world only; optional
//   back_buffer_copy_check true: in the snapshot's frame, after the snapshot has read the presented back buffer and
//                   before the transfer checks, the hook copies the back buffer with LLRenderTarget::copyFromBackBuffer
//                   into a target of the window's size made as the scene monitor makes its captures (GL_RGB), while a
//                   scratch target cleared to (1, 0, 1, 1) is bound, reads the copy back through the device as the
//                   snapshot does and compares its RGB bytes with the presented frame's: measured.back_buffer_copy
//                   {result, copied, differing_bytes, max_delta}, result whether the copy was read back, copied what
//                   copyFromBackBuffer returned. In-world only; optional
//   rebake          true: at the warm-up's start the hook asks the own avatar for a full local rebake
//                   (LLVOAvatarSelf::forceBakeAllTextures(true), Develop > Avatar > Rebake Textures with its debug slam),
//                   and in each warm-up frame takes the renders of that frame's LLViewerDynamicTexture::updateAllInstances.
//                   With no preview texture (ORDER_FIRST or ORDER_MIDDLE), a frame with renders is one whose bake loop
//                   rendered, so display cleared the depth after it (llviewerdisplay.cpp): measured.rebake
//                   {bake_frames, renders, preview_textures}, the most preview textures seen in a warm-up frame.
//                   In-world only; optional
//   render_debug    [name, ...]: debug displays (Develop > Render Metadata, RENDER_DEBUG_*) the hook keeps on in
//                   every in-world frame from STATE_STARTED, turning on only those that are off
//                   (LLPipeline::toggleRenderDebug), with the UI as show_ui has it: physics_shapes,
//                   octree, bboxes, normals, occlusion, shadow_frusta, raycast and nodes; raycast needs
//                   cursor, as the debug ray follows the mouse; not with login_screen or metal_login; optional
//   render_debug_late [name, ...]: as render_debug, but on only from the warm-up's start, once the scene is ready:
//                   shadow_frusta freezes the sun's shadow frusta at the frame it comes on
//                   (LLPipeline::generateSunShadow), which in the loading frames differs from run to run. The first
//                   draws come in the warm-up's first frame, the one frame that may ask the device for pipelines
//                   (level2.py, draws.after_first_frame); no name in both; optional
//   terrain_load    "as_loaded": the terrain octree stays as the network's timing made it, without the replay
//                   (to look at the scene state's run-to-run differences). measured.terrain has the octree as loaded and as
//                   replayed: its groups, an MD5 of its sorted nodes and what the camera saw of it then
//   wireframe       true draws the world in wireframe (Develop > Rendering > Wireframe, gUseWireframe) in every
//                   in-world frame; not with login_screen or metal_login; optional
//   gltf            {file, host}: the glTF run. file is the absolute path of a binary glTF (.glb), host
//                   [x, y, z] the region coordinates of the scene's root object (within 0.5 m) the asset hangs off.
//                   Once the region's features have come, the hook turns GLTFEnabled on for the run (not saved) and
//                   loads the shaders again, as llviewerregion.cpp does for a region that sends it; the local region
//                   does not, and its features turn it off, so the hook's work runs behind the region's on the main
//                   loop's queue. The scene is set up only after that (the window sizing waits for it, so the reload
//                   is at the same point of every run) and ready only with the host in the scene. At the warm-up's
//                   start and again GLTF_RELOAD_FRAME frames later the hook selects the host alone, loads the file
//                   (GLTFSceneManager::load, as File > Open does: it hangs the asset off the selection's first root
//                   object and opens the asset editor), closes the editor, which holds the asset until it closes, and
//                   deselects; so the first asset dies while the device lives. The render profile has GLTFEnabled in
//                   its settings and measured.gltf, last of all its bind_texture probe (gltfBindTextureProbe). Needs
//                   warmup_frames above GLTF_RELOAD_FRAME; in-world only; optional
//   cursor          [x, y]: the mouse point, in the UI's GL coordinates (LLViewerWindow::getCurrentMouse), from the
//                   first in-world frame on: LLGoldenTest::beforeUI saves it in every frame before the window's UI
//                   update raycasts at it (LLViewerWindow::saveLastMouse, which a mouse move from the window system
//                   calls), so the raycast debug display does not follow the system's mouse. measured.cursor counts
//                   the warm-up frames in which the point had moved by the swap (moved_frames). With the raycast
//                   display, LLGoldenTest::afterUI repeats updateUI's debug raycast after the tool's hover, which the
//                   system's mouse inside the window allows and whose pick moves the hit point away, so the display
//                   is the same wherever that mouse is; measured.cursor counts the warm-up frames in which the repeat
//                   moved the point (hover_frames). Not with show_ui or drag (the mouse must be outside the window
//                   there); in-world only; optional
//   projector       {light_at: [x, y, z], texture: a UUID, fov, focus, ambiance}: the region's light object nearest
//                   light_at (region coordinates, within 0.5 m) becomes a projector locally: the hook
//                   gives it PARAMS_LIGHT_IMAGE with that texture and (fov, focus, ambiance) through
//                   setParameterEntry with local_origin false, which sends nothing to the region, once it is in the
//                   scene and before the reflection probes start over, so the warm-up's first frame draws it. The
//                   object must be a light already. Profile-neutral; in-world only; optional
//   lights          [{at: [x, y, z], radius, colour: [r, g, b]}]: each object nearest at becomes a local light the same
//                   way (PARAMS_LIGHT, the colour linear at intensity 1, falloff 0.75 as make-scene.py's light). A full
//                   ObjectUpdate resets the blocks the region does not send, so beforeUI gives them back in every
//                   in-world frame after the network's messages and counts it (measured.local_lights {objects:
//                   [{kind, object, position, held, spotlight, light}], reapplied, reapplied_in_warmup}); a block
//                   missing at the snapshot fails the run (the scene is no longer ready). In-world only; optional
//   select_light    with show_ui: true selects the scene's one light at the warm-up's start, with the basic
//                   toolset's translate tool (LLToolMgr::inEdit), so renderSelections draws its radius sphere when
//                   RenderLightRadius is on, which must be; at the snapshot RenderLightRadius, an edit tool and a
//                   selected light must hold still, and LLRender's cull face must be GL_BACK again; optional
//   snapshot_floater with show_ui: [left, top], where the hook opens the snapshot floater without focus, at its own
//                   size, with the floaters; at the warm-up's start its preview takes a new snapshot without the
//                   floater's delay, at the next idle, so in the warm-up's first frame and not on the wall clock;
//                   at the snapshot the floater must be open with that snapshot and its thumbnail; optional
//   hud_text        with show_ui: {text, region: [x, y, z]}, a free-standing hover text (LLHUDText without a source
//                   object, as a beacon) the hook places at that region point at the warm-up's start, white, without
//                   fading or depth test, so the HUD text layer is in the frame without the avatar's jitter; optional
//   media           {faces: [{name, host: [x, y, z], face, url}, ...], width, height, frames}: the media phase after the
//                   warm-up (not with previews, in-world only): a baseline window of frames frames, then each
//                   face of a legacy (not PBR) face of the region's root object nearest host (findSceneObjectAt) gets a
//                   client-side LLMediaEntry (LLVOVolume::syncMediaData as shared media from the region: autoplay,
//                   loop, width x height, the file:// url; the region is unchanged), and from the frame after every
//                   face's first media texture update a media window of frames frames, then the snapshot.
//                   measured.media has both windows (mediaWindow) and each face's impl; optional
//   metal_login     the Metal login hook, only in the Metal UI dev mode (--metal-ui-dev, world
//                   off), not with login_screen: {frames, scenes, scale, autologin}. With autologin true the hook asks
//                   for an autologin at startup (UserLoginInfo, unsaved, with a made-up name and no password), which
//                   the dev mode must ignore (llstartup.cpp logs 'autologin ignored'). Once the window has the config's size (and
//                   the login panel's browser shows login_page, which is optional here) the hook counts frames
//                   login-screen frames and starts each of scenes {name: frame} at that frame, one after the other
//                   in frame order (a scene waits for the one before it). Every frame from the first on it checks
//                   that the device's frames, the surface's presents and the window's swaps since then are equal,
//                   and on Metal that no drawable timed out, each presented drawable had one wait and one signal
//                   on the queue, and presents without a drawable came only from the zero scene. The scenes:
//                     pending_program  a copy of the UI program under a new name, made outside setShaders, must be
//                                      Ready within 30 frames, hasPendingPublications() true while it is pending
//                     drawn            the drawn gate: the back buffer has at least 16 colours, the login
//                                      panel, its username field, its Log In button and the menu bar each at least
//                                      2 colours at their rects, and on Metal the layer is opaque, without a
//                                      colorspace and BGRA8Unorm and the frame's drawable is the back
//                                      buffer flipped, exactly; saves the back buffer to snapshot and the render
//                                      profile, which only here has window.back_buffer, window.system_ui_size,
//                                      window.ui_scale, measured.image_gl's swaps and measured.rhi.geometry_shaders
//                     glyphs           the username field's line editor, its cursor at 0 and nothing selected, gets
//                                      Latin-1 letters no login-screen widget shows, which stay for the snapshot;
//                                      on Metal no texture upload inside a pass in the 10 frames before,
//                                      and an upload pass break (an upload inside a pass that closed its open
//                                      segment) within 5 frames after
//                     resize           the window 64 points larger, then back; then, with a login page, it waits
//                                      until the browser's media has painted the page at the size it is back to
//                                      and its texture has those pixels, at most 15 s after the window was asked
//                                      back (wall clock); these frames are not counted, so the scenes' frames and
//                                      frames stay the config's
//                     zero             the window's content 0 points high for one present, then back
//                     scale            the test backing scale (scale, above 0) on the view, then off
//                     gates            the window's no-context gates: RenderVSyncEnable off and on again with
//                                      the layer's displaySync following, RenderAppleUseMultGL toggled and back,
//                                      afterDialog() and createSharedContext() (null), under the armed raw-GL trap;
//                                      then the world-off gate in setStartupState: STATE_LOGIN_CLEANUP asked for
//                                      directly must leave STATE_LOGIN_WAIT as it is (its notification is cancelled)
//                   After each window scene the drawable, getSize, the back buffer and the window are the view's
//                   bounds x s, and contentsScale and getSystemUISize are s (s = the scale at RenderHiDPI 1 with the
//                   override, else the system UI size at the start). On GL (the dev mode's injected refusal) drawn
//                   runs without the layer and the drawable, glyphs only sets the text, and resize checks the bounds,
//                   getSize and the window; the other scenes are skipped. The hook logs 'Golden test: metal
//                   login passed' when all are done, 'gl manager values' at the start, and keeps checking the
//                   counters every frame until the quit; with quit it quits once done
// At startup the test sets the graphics level, GPU class and settings. Once in-world it locks the
// environment, sizes the window, moves the avatar and sets the camera, and waits for the scene's
// assets. Then it makes the terrain octree again as one load makes it (otherwise the octree and its groups' state
// follow the network's timing, and with them the terrain's visible extents and the sun's shadow frusta), starts
// the reflection probes over and the test clock (a fixed frame
// step), waits until every probe has settled, and runs warmup_frames with a fixed
// seed before the readback and the render profile. tests/golden/README.md lists the time
// sources this covers. The readback goes through the viewer's render interface device, whose
// surface must have the window's size, and over the warm-up the device must have run one frame
// per main-loop frame.
// Every step logs a line starting "Golden test", and a failure logs "Golden test failed" and
// quits, so the harness never waits for a run that cannot finish. The hooks run only on the local
// OpenSim region (scripts/metal/run-local.sh): they move the avatar and set its home, which must
// never happen on a Linden grid.

namespace
{
    enum class Phase
    {
        Unchecked,
        Off,
        WaitingForWorld,
        LoginScreen, ///< the run's snapshot is at the login screen (login_screen)
        MetalLogin,  ///< the Metal login hook's scenes at the login screen (metal_login)
        Sizing,
        Moving,
        Loading,
        Warming,
        Previews,  ///< the preview run's steps, after the warm-up and before the snapshot (previews)
        Media,     ///< the media phase's windows, after the warm-up and before the snapshot (media)
        Transfers, ///< the snapshot is saved; the transfer checks, the paint map check and the own
                   ///< avatar's GPU times run in the next idles
        Done,
    };

    const std::string LOCAL_GRID = "127.0.0.1:9000"; ///< run-local.sh's grid
    constexpr U32 MOVE_TIMEOUT_FRAMES = 1800; ///< about 30 s at 60 frames a second
    constexpr F64 MOVE_TIMEOUT_SECONDS = 30.0; ///< and at least this long on the wall clock
    constexpr U32 SIZING_FRAMES = 30;         ///< frames for the window system to resize
    constexpr U32 SIZING_TRIES = 4;
    constexpr F32 ARRIVED_DISTANCE = 1.f;    ///< metres across the ground from the avatar's target
    constexpr U32 LOAD_TIMEOUT_FRAMES = 7200; ///< about 2 minutes
    constexpr F64 LOAD_TIMEOUT_SECONDS = 120.0; ///< and at least this long on the wall clock
    constexpr F64 GLTF_WAIT_SECONDS = 30.0;     ///< the glTF run's wait: MOVE_TIMEOUT_FRAMES and this
    constexpr U32 READY_FRAMES = 60;          ///< the scene stays ready this long before warming up
    constexpr U32 NOT_READY_LOG_FRAMES = 600; ///< how often to log why the scene is not ready yet
    constexpr S32 AVATAR_WAIT_FRAMES = 600;   ///< the own avatar's wait at most: its composites' 10 s timer on the test clock
    constexpr F32 IN_USE_SECONDS = 1.f;       ///< a texture referenced this recently is in use
    constexpr U32 PROBE_UPDATES = 3;          ///< whole updates of every probe once the assets are ready
    constexpr F64 FRAME_STEP = 1.0 / 60.0;    ///< the test clock's frame
    constexpr U32 SEED = 20261016;
    // The transfer checks: rawSnapshot's scratch-target path needs a size above the window's, and readBackRaw
    // refuses more than 2048 pixels on a side (its LL_ERRS limits).
    constexpr S32 LARGE_SNAPSHOT_WIDTH = 1920;
    constexpr S32 LARGE_SNAPSHOT_HEIGHT = 1080;
    constexpr size_t MAX_READBACK_TEXTURES = 16;
    constexpr S32 MAX_READBACK_SIZE = 2048;
    const std::string TRANSFERS_SUFFIX = ".transfers.json";
    const std::string PREVIEWS_SUFFIX = ".previews.json"; ///< the preview run's results
    constexpr F32 FIND_DISTANCE = 0.5f;  ///< metres from a selected object's expected place (select)
    constexpr S32 DRAG_PROBE_RANGE = 160; ///< UI pixels either way from the selection centre's projection (drag)
    constexpr S32 DRAG_PROBE_STEP = 4;
    constexpr U32 GLTF_RELOAD_FRAME = 60; ///< the warm-up frame of the glTF run's second load (gltf)
    constexpr U32 MEDIA_WAIT_FRAMES = 1800;   ///< the media phase's wait for every face's first update (media)
    constexpr F64 MEDIA_WAIT_SECONDS = 60.0;  ///< and at least this long on the wall clock
    constexpr U32 MEDIA_HITCH_US = 33000;     ///< a frame longer than this is a hitch

    /// One frame of the media phase (media): its time from the frame before's swap, and the media texture
    /// updates' main-thread microseconds (doMediaTexUpdate's and updateMediaImage's) and doMediaTexUpdate's calls in it.
    struct MediaFrame
    {
        U32 us = 0;
        U32 costUs = 0;
        U32 updates = 0;
    };

    // Settings the viewer reads once at startup, before init(), with nothing to apply a change
    // later (settings_to_globals, LLImageGL::initClass, the window): the graphics level and the
    // config's settings must leave them as the profile has them. Metal's two load-thread settings
    // are read with Linden's two at LLImageGL::initClass.
    const char* const STARTUP_SETTINGS[] = { "RenderCompressTextures", "RenderGLMultiThreadedTextures",
                                             "RenderGLMultiThreadedMedia", "MetalMultiThreadedTextures",
                                             "MetalMultiThreadedMedia", "RenderHiDPI" };

    /// What the login panel's browser last finished loading (login_screen).
    class LoginPageObserver : public LLViewerMediaObserver
    {
    public:
        std::string loaded;
        U32 loads = 0; ///< the loads finished since the run started

        void handleMediaEvent(LLPluginClassMedia* self, EMediaEvent event) override
        {
            if (event == MEDIA_EVENT_NAVIGATE_COMPLETE)
            {
                loaded = self->getNavigateURI();
                ++loads;
                LL_INFOS("Golden") << "Golden test: the login panel's browser loaded " << loaded << LL_ENDL;
            }
        }
    };
    LoginPageObserver sLoginPageObserver;

    struct State
    {
        Phase phase = Phase::Unchecked;
        LLSD config;
        bool loginSized = false; ///< the window has the config's size at the login screen (login_screen)
        std::string loginPage;  ///< the file:// address of the config's login_page (login_screen)
        bool loginPageLoaded = false; ///< the login panel's browser has loaded loginPage (login_screen)
        U32 loginPageLoads = 0; ///< the browser's loads when the frames last started over (login_screen)
        S32 benchmarkClass = -1; ///< the GPU class before the config's replaced it
        U32 frames = 0;
        U32 tries = 0;
        S32 requestWidth = 0;  ///< what movieSize was last asked for
        S32 requestHeight = 0;
        bool environmentLocked = false;
        U32 readyFrames = 0;
        U32 readyAfter = 0;   ///< frames from the camera to the scene being ready
        U32 mostReady = 0;    ///< the most frames in a row the scene was ready while loading
        std::string lastWhy;  ///< the last reason the scene was not ready
        S32 unavailable = 0;  ///< textures in use the region does not have
        S32 unrequested = 0;  ///< textures in use with no data that nothing fetches
        S32 unrequestedBoosted = 0; ///< of them boosted, which a show_ui scene draws blank
        S32 settledSharper = 0;     ///< textures in use settled while sharper than they want
        S32 settledSharperAbove0 = 0; ///< of them above level 0, where a cut fetch may have left them
        S32 askedAgain = 0;   ///< boosted textures whose stopped fetch the hook asked for again
        F32 probesSince = -1.f; ///< frame time from which every probe must have updated, or -1
        std::set<const LLReflectionMap*> probesBegun;          ///< probes whose update began since then
        std::map<const LLReflectionMap*, U32> probesFinished;  ///< and how many such updates finished
        U32 probeResets = 0;  ///< the manager's reset count when the counting began
        bool ownReset = false; ///< the next reset is the hook's own
        LLVector3 lastEye;        ///< the camera's origin in the frame before, from setCamera on
        LLVector3d cameraEye;     ///< the scene's camera position, global (setCamera)
        F32 eyeFromCamera = 0.f;  ///< the eye's distance from it in the last frame
        U32 eyeStillFrames = 0;   ///< frames in a row the eye has not moved (ll_golden_eye_settled)
        F64 eyeStillSince = 0.0;  ///< and the first one's wall-clock time
        bool eyeSettled = false;  ///< it has settled (ll_golden_eye_settled: at the test camera in a later tour stop)
        bool setHome = false;  ///< the home is set here when the probes start over (homeNotConfirmed)
        time_t homeAsked = 0;  ///< when the home was asked for, until the region's confirmation was handled, or 0
        U64 deviceFrameAtWarmup = 0; ///< the render device's frame number when the warm-up began
        U32 viewerFrameAtWarmup = 0; ///< and LLFrameTimer's frame count
        LLVertexBuffer::DrawCounts drawsAtWarmup; ///< and LLVertexBuffer's draws
        LLPipeline::ReadOnlySiteCounts readOnlySitesAtWarmup[LLPipeline::RO_SITE_COUNT]; ///< and the pipeline's
        LLVertexBuffer::DrawCounts drawsAtFrame;  ///< at the last warm-up frame's end
        LLVertexBuffer::DrawCounts drawsMost;     ///< the most of each count in one warm-up frame
        U32 drawFrames = 0;                             ///< the warm-up frames noted
        LLVertexBuffer::DrawCounts drawsAfterFirst;     ///< the counts of the warm-up frames after the first
        LLVertexBuffer::DrawCounts drawsMostAfterFirst; ///< the most of each count in one of them
        U32 scaleDownsAtFrame = 0;        ///< LLImageGL::sScaleDowns at the last warm-up frame's end
        U32 scaleDownsAfterFirst = 0;     ///< scaleDowns in the warm-up frames after the first
        U32 scaleDownsMostAfterFirst = 0; ///< the most in one of them
        LLRender::DynamicStateCounts dynamicStateAtWarmup; ///< and LLRender's dynamic state counts
        LLRender::DynamicStateCounts dynamicStateAtFrame;  ///< at the last warm-up frame's end
        LLRender::DynamicStateCounts dynamicStateMost;     ///< the most of each count in one warm-up frame
        LLTexUnit::BindCounts texturesAtWarmup; ///< LLTexUnit's bindings when the warm-up began
        LLTexUnit::BindCounts texturesAtFrame;  ///< at the last warm-up frame's end
        LLTexUnit::BindCounts texturesMost;     ///< the most of each count in one warm-up frame
        U64 typeSlotReadsAtWarmup = 0;          ///< the Metal encoder's type slot reads when the warm-up began
        U64 depthBiasChangesAtWarmup = 0;       ///< the Metal encoder's depth bias changes then
        U32 textureFrames = 0;                  ///< the warm-up frames noted
        U64 renderDebug = 0;     ///< the config's render_debug as RENDER_DEBUG_* bits
        U64 renderDebugLate = 0; ///< and its render_debug_late
        LL::RHI::ProgramFault programFault; ///< the config's test fault, armed when it names a program
        std::vector<LL::RHI::ProgramFault> programFaults; ///< the config's fault list on Metal, as armed
        LL::RHI::ProgramDelay programDelay; ///< the config's test delay, armed when it names a program
        LLPointer<LLImageRaw> presented; ///< the presented frame the snapshot saved, for the transfer checks
        LLSD profile; ///< the render profile as written, which the paint map check adds to (paintmap_check)
        LLSD transferCounts; ///< the facades' counts at the warm-up's start, the snapshot and after each check
        size_t previewStep = 0; ///< the preview run's step (previews)
        std::map<std::string, LLHandle<LLFloater>> previewFloaters; ///< its floaters, by name
        std::map<std::string, LLRect> previewRects;                 ///< and their rects
        std::vector<LLAnimPauseRequest> previewPauses; ///< the image preview's avatars, held still
        std::vector<std::pair<std::string, std::string>> previewHidden; ///< (floater, child) the steps keep hidden
        LLSD previewResults = LLSD::emptyArray();      ///< the steps' results
        std::vector<std::pair<std::string, LLUUID>> selected; ///< (name, object) the hook selected (select)
        bool dragPinned = false; ///< beforeUI pins the mouse at dragPoint (drag)
        LLCoordGL dragPoint;     ///< in the UI's GL coordinates
        S32 dragProbes = 0;      ///< the grid points probed up to dragPoint
        const LLRenderTarget* monitorLast = nullptr; ///< the scene monitor's last capture target (monitor_check)
        U32 monitorCaptures = 0; ///< the warm-up frames in which it changed
        LLSD monitor;            ///< measured.monitor, read at the snapshot
        LLSD backBufferCopy;     ///< measured.back_buffer_copy, made at the snapshot (back_buffer_copy_check)
        U32 ssrCopiesAtWarmup = 0; ///< LLPipeline's screen-space reflection copies when the warm-up began
        S32 bakeFrames = 0;     ///< the warm-up frames whose dynamic textures rendered (rebake)
        S32 bakeRenders = 0;    ///< and their renders
        S32 previewTextures = 0; ///< the most ORDER_FIRST and ORDER_MIDDLE dynamic textures in a warm-up frame
        S32 avatarWaited = 0;    ///< the frames ownAvatarNotSettled has waited in a row for the own avatar's composites
        bool avatarGaveUp = false; ///< and it stopped waiting after AVATAR_WAIT_FRAMES of them
        bool gltfPosted = false; ///< GLTFEnabled's work is on the main loop's queue (gltf)
        bool gltfOn = false;     ///< and has run: GLTFEnabled on and the shaders loaded again
        U32 gltfWaitFrames = 0;  ///< the frames the scene's setup waited for that
        F64 gltfWaitSince = 0.0; ///< and the wall clock at the first of them
        F64 loadingSince = 0.0;  ///< the wall clock when Loading's frame count began
        F64 movingSince = 0.0;   ///< the wall clock when Moving's frame count began
        LLUUID gltfHost;         ///< the scene object the asset hangs off
        LLSD gltfLoads = LLSD::emptyArray();      ///< the main-loop frame of each load, counted from the warm-up's start
        std::weak_ptr<LL::GLTF::Asset> gltfFirst; ///< the first load's asset, which the second load lets go of
        U32 gltfRayFrames = 0; ///< the warm-up frames after the first load, with the raycast debug display on
        U32 gltfRayHits = 0;   ///< and those whose debug ray hit the host's asset
        bool cursorLocked = false; ///< beforeUI saves cursorPoint as the mouse point (cursor)
        LLCoordGL cursorPoint;     ///< in the UI's GL coordinates
        U32 cursorMoved = 0;       ///< the warm-up frames in which the mouse point had moved by the swap
        U32 cursorHoverRay = 0;    ///< the warm-up frames in which afterUI's debug raycast moved the hit point
        LLSD terrain;              ///< measured.terrain: the terrain octree as loaded and as replayed
        /// One object the config's projector or lights give light parameters locally: the object found at
        /// at, and the block the hook keeps in use on it (PARAMS_LIGHT_IMAGE for the projector, PARAMS_LIGHT for a light).
        struct LocalLight
        {
            std::string kind; ///< "projector" or "light"
            LLVector3 at;     ///< the config's place, in region coordinates
            LLUUID object;    ///< the object found there, null until found
            LLLightParams light;
            LLLightImageParams image;
        };
        std::vector<LocalLight> localLights; ///< made from the config at the start; empty without projector and lights
        bool localLightsFound = false; ///< every object found and its block applied once
        U32 localLightReapplies = 0;   ///< blocks applied again after the region took them away (information)
        U32 localLightReappliesInWarmup = 0; ///< and those in the warm-up's frames
        // The media phase (media)
        U32 mediaStep = 0; ///< 0 the baseline window, 1 the wait for every face's first update, 2 the media window
        std::chrono::steady_clock::time_point mediaLastSwap; ///< the media phase's last frame's swap (or the warm-up's)
        U64 mediaCostAtSwap = 0;    ///< the media counts' main-thread microseconds then
        U64 mediaUpdatesAtSwap = 0; ///< and doMediaTexUpdate's calls
        F64 mediaWaitSince = 0.0;   ///< the wall clock when the wait's frame count began
        std::vector<MediaFrame> mediaFrames;              ///< the window's frames
        LLViewerMediaImpl::TexUpdateCounts mediaCountsAt; ///< the media counts when the window began
        U64 mediaDeferredSwapsAt = 0;                     ///< and LLImageGL's deferred swaps
        U64 mediaInPlaceUpdatesAt = 0;                    ///< and its in-place media updates
        std::vector<viewer_media_t> mediaImpls;           ///< the faces' impls, in the config's order
        std::vector<U64> mediaImplUpdatesAt;              ///< their updates when the media window began
        LLSD mediaResult;                                 ///< measured.media
        // The own avatar's GPU times (avatar_gpu)
        U32 avatarProfiles = 0;                 ///< the whole-avatar profiles begun
        U32 avatarWaitIdles = 0;                ///< the idles the last one's result has been waited for
        std::vector<F32> avatarTimes;           ///< and the results read, in ms
        bool avatarAttachmentsTimed = false;    ///< profileAvatar(self, true) has run
        LLSD avatarAttachmentTimes = LLSD::emptyArray(); ///< and each attachment's time in ms
        LLGPUQuery avatarEmptySpan;             ///< the empty span's query
        S32 avatarEmptySpanTries = 0;           ///< the idles its result has been asked for
    };

    State sState;

    LLVector3 vector3(const LLSD& value)
    {
        return LLVector3(F32(value[0].asReal()), F32(value[1].asReal()), F32(value[2].asReal()));
    }

    bool isVector3(const LLSD& value)
    {
        return value.isArray() && value.size() == 3;
    }

    void finish(bool quit)
    {
        sState.phase = Phase::Done;
        // [Viewer Lab] The device's GPU errors from commit feedback, read without waiting (0 on GL)
        if (LL::RHI::Device* device = LL::RHI::viewerDevice())
        {
            LL_INFOS("Golden") << "Golden test: render device GPU errors " << device->gpuErrorCount() << LL_ENDL;
        }
        // [Viewer Lab] The gated GL calls' counts (LLGLGated), also for runs without a render profile
        std::string gated;
        for (const auto& [name, calls] : LLGLGated::counts())
        {
            gated += (gated.empty() ? "" : ", ") + name + " " + std::to_string(calls);
        }
        LL_INFOS("Golden") << "Golden test: gated GL calls: " << (gated.empty() ? "none" : gated) << LL_ENDL;
        // The media phase's references go now, on success and failure alike: the faces keep their own, so the app's
        // cleanup destroys the impls in order. Held by sState they would outlive it and die in static destruction,
        // after the coroutine and plugin machinery, which aborts.
        sState.mediaImpls.clear();
        sState.mediaImplUpdatesAt.clear();
        // The empty span's query goes now too, on the main thread while the device lives (avatar_gpu).
        sState.avatarEmptySpan.release();
        if (quit)
        {
            LLAppViewer::instance()->requestQuit();
        }
    }

    void fail(const std::string& why)
    {
        LL_WARNS("Golden") << "Golden test failed: " << why << LL_ENDL;
        finish(true);
    }

    /// Only the local region; anywhere else the test fails before it changes anything.
    bool onLocalGrid()
    {
        const std::string grid = LLGridManager::getInstance()->getGrid();
        if (grid != LOCAL_GRID)
        {
            fail("the grid is " + grid + ", not the local " + LOCAL_GRID);
            return false;
        }
        return true;
    }

    /// The Metal login hook's scenes (metal_login).
    const char* const METAL_LOGIN_SCENES[] = { "pending_program", "drawn", "glyphs", "resize", "zero", "scale", "gates" };

    /// The config's metal_login: only in the Metal UI dev mode, where the world is off, in the build with the
    /// Metal backend, and not beside login_screen. False after fail().
    bool checkMetalLoginConfig(bool login_screen)
    {
        const LLSD& metal = sState.config["metal_login"];
        if (!LL_RHI_METAL || !gMetalUIDev)
        {
            fail("the config's metal_login needs the Metal UI dev mode (--metal-ui-dev) of a build with the Metal backend");
            return false;
        }
        if (login_screen || !metal.isMap() || !metal["frames"].isInteger() || metal["frames"].asInteger() < 1
            || !metal["scenes"].isMap())
        {
            fail("the config's metal_login needs no login_screen beside it, frames of 1 or more and scenes as a map");
            return false;
        }
        for (const auto& [name, frame] : llsd::inMap(metal["scenes"]))
        {
            if (std::find(std::begin(METAL_LOGIN_SCENES), std::end(METAL_LOGIN_SCENES), name) == std::end(METAL_LOGIN_SCENES)
                || !frame.isInteger() || frame.asInteger() < 1 || frame.asInteger() > metal["frames"].asInteger())
            {
                fail("the metal_login scene " + name
                     + " is not pending_program, drawn, glyphs, resize, zero, scale or gates, or its frame is not from 1 to frames");
                return false;
            }
        }
        const LLSD& scale = metal["scale"];
        if (metal["scenes"].has("scale") && (!(scale.isReal() || scale.isInteger()) || scale.asReal() <= 0.0))
        {
            fail("the metal_login scene scale needs scale, a number above 0");
            return false;
        }
        if (metal.has("autologin") && !metal["autologin"].isBoolean())
        {
            fail("the metal_login autologin needs a boolean");
            return false;
        }
        return true;
    }

    /// The RENDER_DEBUG_* bits of a render_debug or render_debug_late array: 0 unless it is a non-empty array
    /// of names the hook knows, none twice.
    U64 renderDebugMask(const LLSD& names)
    {
        static const std::map<std::string, U64> BITS = {
            { "physics_shapes", LLPipeline::RENDER_DEBUG_PHYSICS_SHAPES },
            { "octree", LLPipeline::RENDER_DEBUG_OCTREE },
            { "bboxes", LLPipeline::RENDER_DEBUG_BBOXES },
            { "normals", LLPipeline::RENDER_DEBUG_NORMALS },
            { "occlusion", LLPipeline::RENDER_DEBUG_OCCLUSION },
            { "shadow_frusta", LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA },
            { "raycast", LLPipeline::RENDER_DEBUG_RAYCAST },
            { "nodes", LLPipeline::RENDER_DEBUG_NODES },
        };
        if (!names.isArray() || names.size() == 0)
        {
            return 0;
        }
        U64 mask = 0;
        for (const LLSD& name : llsd::inArray(names))
        {
            const auto it = name.isString() ? BITS.find(name.asString()) : BITS.end();
            if (it == BITS.end() || (mask & it->second))
            {
                return 0;
            }
            mask |= it->second;
        }
        return mask;
    }

    /// Reads the config named by --golden, or turns the hooks off when there is none.
    // [Viewer Lab] Tours: many scenes in one login. The config's stops are scenes that differ from the launch
    // only in what a scene changes in-world: the avatar's spot, the camera, the warm-up and the run-time settings
    // level2.py allows (TOUR_RUNTIME_SETTINGS, each with its listener or per-frame read); every other key is the
    // launch's. Each stop gets a fresh State, its own snapshot, render profile and transfers, and after it its settings
    // go back to the level's baseline, audited ("tour unwind"). The keys below are what a stop may hold; the refused
    // keys need an unwind of their own the tours do not have yet, so a tour fails on them.
    // avatar_gpu is a launch key, so its times would run after every stop.
    const char* const TOUR_STOP_KEYS[] = { "name", "snapshot", "render_profile", "transfers", "avatar", "camera",
                                           "warmup_frames", "settings" };
    const char* const TOUR_REFUSED_KEYS[] = { "login_screen", "metal_login", "show_ui", "previews", "media", "gltf",
                                              "select", "drag", "lights", "projector", "render_debug",
                                              "render_debug_late", "paintmap_check", "monitor_check",
                                              "back_buffer_copy_check", "rebake", "wireframe", "terrain_load", "cursor",
                                              "sky", "program_fault", "program_delay", "window_origin", "floaters",
                                              "avatar_gpu" };

    struct Tour
    {
        LLSD stops;               ///< the config's stops; undefined outside a tour
        size_t stop = 0;          ///< the stop running, from 0
        LLSD launch;              ///< the config without its stops: the launch part, with the first stop's keys
        LLSD baseline;            ///< every setting a stop sets, as the level had it when the first stop began
        LLSD imageGLAtBegin;      ///< measured.image_gl when a later stop began (level2.py's floors per stop)
        LLSD metalUploadsAtBegin; ///< and measured.metal_uploads on Metal
        LLGoldenProbes probes;    ///< the reflection probes when the first stop began (the unwind's audit)
        LLGoldenHome anchorHome;  ///< the home the first stop asked for, which every later stop asks for

        bool active() const { return stops.isArray(); }
    };
    Tour sTour;
    void finishScene(); // after the transfer checks too (finishAfterChecks), below snapshotAndFinish's helpers

    /// [Viewer Lab] A tour's stop after its first: the eye's and the probes' rules of the tour's own; a fresh run
    /// and the anchor keep the rules of a run outside a tour (ll_golden_later_tour_stop).
    bool laterTourStop()
    {
        return ll_golden_later_tour_stop(sTour.active(), sTour.stop);
    }

    /// The running stop as "k/n name" for the markers.
    std::string tourStopName()
    {
        return llformat("%u/%u %s", U32(sTour.stop + 1), U32(sTour.stops.size()),
                        sTour.stops[sTour.stop]["name"].asString().c_str());
    }

    /// One marker in the log and on stderr, where level2.py slices a tour's log and validation lines by stop.
    void tourMarker(const std::string& text)
    {
        LL_INFOS("Golden") << "Golden test: " << text << LL_ENDL;
        fprintf(stderr, "Golden test: %s\n", text.c_str());
        fflush(stderr);
    }

    /// measured.image_gl as the render profile writes it.
    LLSD imageGLMeasures()
    {
        const LLImageGL::DeviceCounts device_counts = LLImageGL::deviceCounts();
        const LLImageGL::SyncCounts sync_counts = LLImageGL::syncCounts();
        LLSD image_gl = llsd::map("uploads", (S32)device_counts.uploads,
                                  "mipmaps", (S32)device_counts.mipmaps,
                                  "untracked", (S32)device_counts.untracked,
                                  "respecifications", (S32)device_counts.respecifications,
                                  "upload_context", (S32)device_counts.uploadContext,
                                  "thread_syncs", (S32)sync_counts.threadSyncs,
                                  "swaps", (S32)sync_counts.swaps,
                                  "handle_swaps", (S32)sync_counts.handleSwaps,
                                  "fences", (S32)sync_counts.fences,
                                  "fence_waits", (S32)sync_counts.fenceWaits,
                                  "deferred_swaps", (S32)sync_counts.deferredSwaps,
                                  "fenceless_syncs", (S32)sync_counts.fencelessSyncs,
                                  "thread_handle_swaps", (S32)sync_counts.threadHandleSwaps,
                                  "deferred_thread_swaps", (S32)sync_counts.deferredThreadSwaps);
        // [Viewer Lab] The media updates made in the image's own texture, also counted in deferred_swaps; 0 on GL
        image_gl["in_place_updates"] = (S32)sync_counts.inPlaceUpdates;
        // [Viewer Lab] Information, no check: the main thread's microseconds in createGLTexture with an image
        // and in the swap items' handover of the upload thread's fences (the encoder's waitForFence with the thread's
        // mips), the A/B of a scene without the upload thread against one with it.
        auto microseconds = [](U64 us) { return (S32)llmin(us, U64(S32_MAX)); };
        image_gl["main_create_us"] = microseconds(sync_counts.mainCreateUs);
        image_gl["swap_mip_us"] = microseconds(sync_counts.swapMipUs);
        return image_gl;
    }

    /// measured.metal_uploads on Metal, the upload queue's counts, which level2.py checks in a scene with upload_thread
    /// (no Fence::wait() on the main thread, no failed command buffer, a submit); undefined on GL.
    LLSD metalUploadMeasures()
    {
#if LL_RHI_METAL
        if (LL::RHI::Device* device = LL::RHI::viewerDevice(); device && device->backend() == LL::RHI::BackendType::Metal)
        {
            const LL::RHI::MetalUploadCounts uploads = LL::RHI::metalUploadCounts(*device);
            return llsd::map("submits", (S32)uploads.submits,
                             "uploads", (S32)uploads.uploads,
                             "deferred_mips", (S32)uploads.deferredMips,
                             "signalled_at_handover", (S32)uploads.signalledAtHandover,
                             "queue_waits", (S32)uploads.queueWaits,
                             "main_thread_waits", (S32)uploads.mainThreadWaits,
                             "errors", (S32)uploads.errors);
        }
#endif
        return LLSD();
    }

    /// The config's stops, when it has them: two or more, each a map of only the stop keys with a name, a
    /// snapshot and a render profile, the transfers file as the run's, the avatar, the camera, warmup_frames and
    /// settings as a map; the top level is the first stop's, and the launch has no refused key. False when the run
    /// failed.
    bool checkTourConfig(bool at_login)
    {
        const LLSD& config = sState.config;
        const LLSD& stops = config["stops"];
        if (stops.isUndefined())
        {
            return true;
        }
        if (at_login || !stops.isArray() || stops.size() < 2)
        {
            fail("the config's stops need two or more stops, and no login_screen or metal_login");
            return false;
        }
        for (const char* key : TOUR_REFUSED_KEYS)
        {
            if (config.has(key))
            {
                fail(std::string("a tour has no unwind for the config's ") + key + " yet (P3-T5)");
                return false;
            }
        }
        for (size_t i = 0; i < stops.size(); ++i)
        {
            const LLSD& stop = stops[i];
            std::string why;
            if (!stop.isMap())
            {
                why = "is not a map";
            }
            else
            {
                for (const auto& [key, value] : llsd::inMap(stop))
                {
                    if (std::find_if(std::begin(TOUR_STOP_KEYS), std::end(TOUR_STOP_KEYS),
                                     [&key](const char* known) { return key == known; }) == std::end(TOUR_STOP_KEYS))
                    {
                        why = "has the key " + key + ", which a stop cannot change";
                    }
                }
                const LLSD& transfers = stop["transfers"];
                if (!stop["name"].isString() || stop["name"].asString().empty() || !stop["snapshot"].isString()
                    || stop["snapshot"].asString().empty() || !stop["render_profile"].isString()
                    || stop["render_profile"].asString().empty() || !isVector3(stop["avatar"])
                    || !isVector3(stop["camera"]["position"]) || !isVector3(stop["camera"]["focus"])
                    || stop["warmup_frames"].asInteger() < 1
                    || !(stop["settings"].isMap() || stop["settings"].isUndefined())
                    || transfers.isDefined() != config["transfers"].isDefined()
                    || (transfers.isDefined()
                        && (!transfers.isString() || transfers.asString().size() <= TRANSFERS_SUFFIX.size()
                            || transfers.asString().compare(transfers.asString().size() - TRANSFERS_SUFFIX.size(),
                                                            TRANSFERS_SUFFIX.size(), TRANSFERS_SUFFIX) != 0)))
                {
                    why = "needs name, snapshot, render_profile, avatar, camera position and focus, warmup_frames of 1 or "
                          "more, settings as a map and transfers as the run's";
                }
            }
            if (!why.empty())
            {
                fail(llformat("the config's stop %u ", U32(i + 1)) + why);
                return false;
            }
        }
        // The top level is the first stop's, so everything before the tour's start runs as that scene's.
        if (config["snapshot"].asString() != stops[0]["snapshot"].asString()
            || config["render_profile"].asString() != stops[0]["render_profile"].asString())
        {
            fail("the config's snapshot and render_profile are not its first stop's");
            return false;
        }
        sTour.stops = stops;
        sTour.launch = config;
        sTour.launch.erase("stops");
        return true;
    }

    void readConfig()
    {
        const std::string path = gSavedSettings.getString("GoldenTestConfig");
        if (path.empty())
        {
            sState.phase = Phase::Off;
            return;
        }
        LL_INFOS("Golden") << "Golden test: config " << path << LL_ENDL;
        sState.phase = Phase::WaitingForWorld;
        if (!onLocalGrid())
        {
            return;
        }
        llifstream file(path.c_str());
        if (!file.is_open() || LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXMLDocument(sState.config, file))
        {
            fail("could not read " + path);
            return;
        }
        const LLSD& config = sState.config;
        const LLSD& camera = config["camera"];
        const LLSD& window = config["window"];
        // At the login screen the run has no scene.
        const LLSD& login_frames = config["login_screen"];
        const bool login_screen = login_frames.isDefined();
        if (login_screen && (!login_frames.isInteger() || login_frames.asInteger() < 1))
        {
            fail("the config's login_screen needs a number of frames of 1 or more");
            return;
        }
        // The Metal login hook stays at the login screen too; its login page is optional.
        const bool metal_login = config["metal_login"].isDefined();
        if (metal_login && !checkMetalLoginConfig(login_screen))
        {
            return;
        }
        const bool at_login = login_screen || metal_login;
        // The run's login page: a local file whose address needs no escaping, so that its address is
        // the one the browser reports when it has loaded it.
        if (login_screen || (metal_login && config["login_page"].isDefined()))
        {
            const std::string page = config["login_page"].asString();
            if (page.empty() || page[0] != '/'
                || page.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/._-")
                       != std::string::npos
                || !LLFile::isfile(page))
            {
                fail("the config's login_screen needs login_page, the absolute path of a file, in letters, digits and /._-");
                return;
            }
            sState.loginPage = "file://" + page;
            // The login panel's own loads (LLPanelLogin::loadLoginPage) show the page too, with the panel's query,
            // so no load of the grid's page can finish after the hook's; for this run only, not saved.
            LLControlVariable* force = gSavedSettings.getControl("ForceLoginURL");
            if (!force)
            {
                fail("ForceLoginURL is not declared");
                return;
            }
            force->setPersist(LLControlVariable::PERSIST_NO);
            gSavedSettings.setString("ForceLoginURL", sState.loginPage);
        }
        if (!config["snapshot"].isString() || config["snapshot"].asString().empty() || !window.isArray()
            || window.size() != 2 || window[0].asInteger() < 1 || window[1].asInteger() < 1
            || (!at_login
                && (!isVector3(config["avatar"]) || !isVector3(camera["position"]) || !isVector3(camera["focus"])
                    || config["warmup_frames"].asInteger() < 1)))
        {
            fail("the config needs snapshot, window, warmup_frames, avatar and camera position and focus");
            return;
        }
        const LLSD& level = config["graphics_level"];
        const LLSD& gpu_class = config["gpu_class"];
        if (!config["render_profile"].isString() || config["render_profile"].asString().empty() || !level.isInteger()
            || level.asInteger() < 0 || !LLFeatureManager::instance().isValidGraphicsLevel(U32(level.asInteger()))
            || !gpu_class.isInteger() || gpu_class.asInteger() < GPU_CLASS_0 || gpu_class.asInteger() > GPU_CLASS_5
            || !(config["settings"].isMap() || config["settings"].isUndefined()))
        {
            fail("the config needs render_profile, a graphics_level, a gpu_class and settings as a map");
            return;
        }
        // The transfer checks are optional: level2.py names their file.
        const LLSD& transfers = config["transfers"];
        if (transfers.isDefined()
            && (!transfers.isString() || transfers.asString().size() <= TRANSFERS_SUFFIX.size()
                || transfers.asString().compare(transfers.asString().size() - TRANSFERS_SUFFIX.size(),
                                                TRANSFERS_SUFFIX.size(), TRANSFERS_SUFFIX) != 0))
        {
            fail("the config's transfers needs the path of a file ending in " + TRANSFERS_SUFFIX);
            return;
        }
        // The test fault is optional.
        const LLSD& fault = config["program_fault"];
        if (fault.isDefined()
            && (!fault.isMap() || !fault["program"].isString() || fault["program"].asString().empty()
                || !fault["deferred"].isBoolean() || !fault["times"].isInteger() || fault["times"].asInteger() < 0))
        {
            fail("the config's program_fault needs a program name, deferred as a boolean and times as an integer of 0 or more");
            return;
        }
        // The fault list is optional, on Metal only: more programs beside program_fault's,
        // each with its own times and program_fault's deferred.
        const LLSD& faults = config["program_faults"];
        if (faults.isDefined())
        {
#if LL_RHI_METAL
            if (!fault.isMap())
            {
                fail("the config's program_faults needs program_fault");
                return;
            }
            if (!faults.isArray())
            {
                fail("the config's program_faults needs an array of {program, times}");
                return;
            }
            std::unordered_set<std::string> names = { fault["program"].asString() };
            for (const LLSD& entry : llsd::inArray(faults))
            {
                if (!entry.isMap() || !entry["program"].isString() || entry["program"].asString().empty()
                    || !entry["times"].isInteger() || entry["times"].asInteger() < 0)
                {
                    fail("the config's program_faults needs a program name and times as an integer of 0 or more in each entry");
                    return;
                }
                if (!names.insert(entry["program"].asString()).second)
                {
                    fail("the config's program_faults names " + entry["program"].asString()
                         + " twice, or as program_fault does");
                    return;
                }
            }
#else
            fail("the config's program_faults needs a Metal build");
            return;
#endif
        }
        // The test delay is optional.
        const LLSD& delay = config["program_delay"];
        if (delay.isDefined()
            && (!delay.isMap() || !delay["program"].isString() || delay["program"].asString().empty()
                || !delay["frames"].isInteger() || delay["frames"].asInteger() < 0))
        {
            fail("the config's program_delay needs a program name and frames as an integer of 0 or more");
            return;
        }
        // Settings only an LLCachedControl declares, as [type, value].
        const LLSD& cached = config["cached_settings"];
        if (cached.isDefined() && !cached.isMap())
        {
            fail("the config's cached_settings must be a map");
            return;
        }
        for (const auto& [name, entry] : llsd::inMap(cached))
        {
            const std::string type = entry.isArray() && entry.size() == 2 ? entry[0].asString() : std::string();
            if (!((type == "Boolean" && entry[1].isBoolean()) || (type == "String" && entry[1].isString())))
            {
                fail("the cached setting " + name + " needs [Boolean, a boolean] or [String, a string]");
                return;
            }
        }
        // The debug displays are optional, in-world only.
        const LLSD& render_debug = config["render_debug"];
        const LLSD& render_debug_late = config["render_debug_late"];
        sState.renderDebug = renderDebugMask(render_debug);
        sState.renderDebugLate = renderDebugMask(render_debug_late);
        if ((render_debug.isDefined() && (at_login || sState.renderDebug == 0))
            || (render_debug_late.isDefined()
                && (at_login || sState.renderDebugLate == 0 || (sState.renderDebug & sState.renderDebugLate) != 0)))
        {
            fail("the config's render_debug and render_debug_late need non-empty arrays of names the hook knows "
                 "(physics_shapes, octree, bboxes, normals, occlusion, shadow_frusta, raycast, nodes), none twice or in "
                 "both, and no login_screen or metal_login");
            return;
        }
        // The paint map check is optional, in-world only.
        const LLSD& paintmap = config["paintmap_check"];
        if (paintmap.isDefined() && (!paintmap.isBoolean() || at_login))
        {
            fail("the config's paintmap_check needs a boolean and no login_screen or metal_login");
            return;
        }
        // The own avatar's GPU times are optional, in-world only.
        const LLSD& avatar_gpu = config["avatar_gpu"];
        if (avatar_gpu.isDefined() && (!avatar_gpu.isBoolean() || at_login))
        {
            fail("the config's avatar_gpu needs a boolean and no login_screen or metal_login");
            return;
        }
        // The terrain octree as the network made it (terrain_load "as_loaded") is optional, in-world only.
        const LLSD& terrain_load = config["terrain_load"];
        if (terrain_load.isDefined() && (terrain_load.asString() != "as_loaded" || at_login))
        {
            fail("the config's terrain_load needs \"as_loaded\" and no login_screen or metal_login");
            return;
        }
        // The UI in the snapshot is optional, in-world only.
        const LLSD& show_ui = config["show_ui"];
        const LLSD& floaters = config["floaters"];
        const LLSD& origin = config["window_origin"];
        if ((show_ui.isDefined() && (!show_ui.isBoolean() || at_login))
            || (floaters.isDefined() && (!floaters.isArray() || !show_ui.asBoolean()))
            || (origin.isDefined()
                && (!show_ui.asBoolean() || !origin.isArray() || origin.size() != 2 || !origin[0].isInteger()
                    || !origin[1].isInteger())))
        {
            fail("the config's show_ui needs a boolean and no login_screen, floaters needs show_ui and an array, and "
                 "window_origin show_ui and two integers");
            return;
        }
        for (const LLSD& floater : llsd::inArray(floaters))
        {
            const LLSD& rect = floater["rect"];
            if (!floater["name"].isString() || floater["name"].asString().empty() || !rect.isArray() || rect.size() != 4
                || !rect[0].isInteger() || !rect[1].isInteger() || !rect[2].isInteger() || !rect[3].isInteger()
                || rect[0].asInteger() >= rect[2].asInteger() || rect[3].asInteger() >= rect[1].asInteger()
                || !(floater["hide"].isUndefined() || floater["hide"].isArray()))
            {
                fail("each of the config's floaters needs a name, a rect [left, top, right, bottom] of integers and "
                     "hide, when it has one, as an array");
                return;
            }
        }
        // Wireframe, the selected light and the snapshot floater are optional, in-world only.
        const LLSD& wireframe = config["wireframe"];
        const LLSD& select_light = config["select_light"];
        const LLSD& snapshot_floater = config["snapshot_floater"];
        const LLSD& hud_text = config["hud_text"];
        if (hud_text.isDefined()
            && (!show_ui.asBoolean() || !hud_text.isMap() || !hud_text["text"].isString()
                || hud_text["text"].asString().empty() || !hud_text["region"].isArray() || hud_text["region"].size() != 3
                || !hud_text["region"][0].isReal() || !hud_text["region"][1].isReal() || !hud_text["region"][2].isReal()))
        {
            fail("the config's hud_text needs show_ui, a text and a region point of three reals (P3-03b)");
            return;
        }
        // The media phase is optional, in-world only and not with previews.
        if (const LLSD& media = config["media"]; media.isDefined())
        {
            bool ok = media.isMap() && !at_login && !config["previews"].isDefined() && media["faces"].isArray()
                      && media["faces"].size() > 0 && media["width"].isInteger() && media["width"].asInteger() > 0
                      && media["height"].isInteger() && media["height"].asInteger() > 0 && media["frames"].isInteger()
                      && media["frames"].asInteger() > 0;
            for (size_t i = 0; ok && i < media["faces"].size(); ++i)
            {
                const LLSD& face = media["faces"][i];
                ok = face.isMap() && face["name"].isString() && !face["name"].asString().empty()
                     && isVector3(face["host"]) && face["face"].isInteger() && face["face"].asInteger() >= 0
                     && face["url"].isString() && face["url"].asString().rfind("file://", 0) == 0;
            }
            if (!ok)
            {
                fail("the config's media needs {faces: [{name, host: [x, y, z], face, url: file://...}, ...], width, "
                     "height, frames}, without login_screen, metal_login or previews (P3-03b)");
                return;
            }
        }
        if ((wireframe.isDefined() && (!wireframe.isBoolean() || at_login))
            || (select_light.isDefined() && (!select_light.isBoolean() || !show_ui.asBoolean()))
            || (snapshot_floater.isDefined()
                && (!show_ui.asBoolean() || !snapshot_floater.isArray() || snapshot_floater.size() != 2
                    || !snapshot_floater[0].isInteger() || !snapshot_floater[1].isInteger())))
        {
            fail("the config's wireframe needs a boolean and no login_screen or metal_login, select_light show_ui and a "
                 "boolean, and snapshot_floater show_ui and two integers");
            return;
        }
        // The preview run is optional, with show_ui.
        const LLSD& previews = config["previews"];
        const LLSD& previews_result = config["previews_result"];
        if (previews.isDefined()
            && (!previews.isArray() || previews.size() == 0 || !show_ui.asBoolean() || !previews_result.isString()
                || previews_result.asString().size() <= PREVIEWS_SUFFIX.size()
                || previews_result.asString().compare(previews_result.asString().size() - PREVIEWS_SUFFIX.size(),
                                                      PREVIEWS_SUFFIX.size(), PREVIEWS_SUFFIX) != 0))
        {
            fail("the config's previews needs a non-empty array, show_ui and previews_result, the path of a file ending in "
                 + PREVIEWS_SUFFIX);
            return;
        }
        std::set<std::string> step_names;
        std::set<std::string> opened;
        for (const LLSD& step : llsd::inArray(previews))
        {
            const std::string name = step["name"].asString();
            const std::string floater = step["floater"].asString();
            const LLSD& rect = step["rect"];
            const bool opens = !opened.count(floater);
            const auto strings = [](const LLSD& list)
            {
                if (!list.isArray())
                {
                    return false;
                }
                for (const LLSD& item : llsd::inArray(list))
                {
                    if (!item.isString() || item.asString().empty())
                    {
                        return false;
                    }
                }
                return true;
            };
            if (!step["name"].isString() || name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos
                || !step_names.insert(name).second
                || (floater != "upload_image" && floater != "upload_model" && floater != "upload_anim_bvh")
                || !step["frames"].isInteger() || step["frames"].asInteger() < 1
                || opens != (step["file"].isString() && !step["file"].asString().empty())
                || opens != rect.isArray()
                || (opens && (rect.size() != 4 || !rect[0].isInteger() || !rect[1].isInteger() || !rect[2].isInteger()
                              || !rect[3].isInteger() || rect[0].asInteger() >= rect[2].asInteger()
                              || rect[3].asInteger() >= rect[1].asInteger()))
                || (step.has("combo") && (!step["combo"].isArray() || step["combo"].size() != 2 || !step["combo"][0].isString()
                                          || !step["combo"][1].isInteger() || step["combo"][1].asInteger() < 0))
                || (step.has("check") && !strings(step["check"])) || (step.has("click") && !strings(step["click"]))
                || (step.has("hide") && !strings(step["hide"]))
                || (step.has("physics") && (floater != "upload_model" || opens || !step["physics"].isString()
                                            || step["physics"].asString().empty())))
            {
                fail("each of the config's previews needs a unique name of a-z, 0-9 and -, a floater upload_image, "
                     "upload_model or upload_anim_bvh, frames above 0, a file and a rect [left, top, right, bottom] of "
                     "integers exactly when it opens its floater, and as it has them combo [child, index], check, "
                     "click and hide [child, ...], and physics a file, for an open upload_model (" + name + ")");
                return;
            }
            opened.insert(floater);
        }
        // The selection run is optional, with show_ui: renderSelections draws only with the UI.
        const LLSD& select = config["select"];
        const LLSD& expected = config["expected_positions"];
        const LLSD& tool = config["tool"];
        const LLSD& drag = config["drag"];
        if (select.isDefined() || expected.isDefined() || tool.isDefined() || drag.isDefined())
        {
            const auto point = [](const LLSD& value)
            {
                return isVector3(value) && (value[0].isReal() || value[0].isInteger())
                       && (value[1].isReal() || value[1].isInteger()) && (value[2].isReal() || value[2].isInteger());
            };
            bool valid = select.isArray() && select.size() > 0 && show_ui.asBoolean() && expected.isMap()
                         && expected.size() == select.size() && tool.isString()
                         && (tool.asString() == "translate" || tool.asString() == "scale")
                         && (drag.isUndefined() || (drag.isString() && drag.asString() == "xy_plane"
                                                    && tool.asString() == "translate"));
            std::set<std::string> names;
            for (const LLSD& name : llsd::inArray(select))
            {
                valid = valid && name.isString() && !name.asString().empty() && names.insert(name.asString()).second
                        && point(expected[name.asString()]);
            }
            if (!valid)
            {
                fail("the config's select needs show_ui, a non-empty array of distinct names, each with its [x, y, z] in "
                     "expected_positions and no other there, tool translate or scale, and drag, when it has one, xy_plane "
                     "with tool translate");
                return;
            }
        }
        // The scene monitor's check is optional, in-world only.
        const LLSD& monitor_check = config["monitor_check"];
        if (monitor_check.isDefined() && (!monitor_check.isBoolean() || at_login))
        {
            fail("the config's monitor_check needs a boolean and no login_screen or metal_login");
            return;
        }
        // The back buffer copy's check is optional, in-world only.
        const LLSD& back_buffer_copy_check = config["back_buffer_copy_check"];
        if (back_buffer_copy_check.isDefined() && (!back_buffer_copy_check.isBoolean() || at_login))
        {
            fail("the config's back_buffer_copy_check needs a boolean and no login_screen or metal_login");
            return;
        }
        // The own avatar's local rebake is optional, in-world only.
        const LLSD& rebake = config["rebake"];
        if (rebake.isDefined() && (!rebake.isBoolean() || at_login))
        {
            fail("the config's rebake needs a boolean and no login_screen or metal_login");
            return;
        }
        // The projector and the local lights are optional, in-world only and profile-neutral: the region
        // holds neither, so the hook gives scene objects light parameters locally (localLightsNotReady).
        const LLSD& projector = config["projector"];
        const LLSD& lights = config["lights"];
        const auto number = [](const LLSD& value) { return value.isReal() || value.isInteger(); };
        if (projector.isDefined())
        {
            LLUUID texture;
            if (at_login || !projector.isMap() || !isVector3(projector["light_at"]) || !projector["texture"].isString()
                || !texture.set(projector["texture"].asString(), false) || texture.isNull() || !number(projector["fov"])
                || projector["fov"].asReal() <= 0.0 || projector["fov"].asReal() > 3.0 || !number(projector["focus"])
                || !number(projector["ambiance"]) || projector["ambiance"].asReal() < 0.0)
            {
                fail("the config's projector needs {light_at: [x, y, z], texture: a UUID, fov above 0 and at most 3, focus, "
                     "ambiance at least 0} and no login_screen or metal_login");
                return;
            }
            State::LocalLight entry;
            entry.kind = "projector";
            entry.at = vector3(projector["light_at"]);
            entry.image.setLightTexture(texture);
            entry.image.setParams(LLVector3(F32(projector["fov"].asReal()), F32(projector["focus"].asReal()),
                                            F32(projector["ambiance"].asReal())));
            sState.localLights.push_back(entry);
        }
        if (lights.isDefined() && (at_login || !lights.isArray() || lights.size() == 0))
        {
            fail("the config's lights needs a non-empty array and no login_screen or metal_login");
            return;
        }
        for (const LLSD& light : llsd::inArray(lights))
        {
            const LLSD& colour = light["colour"];
            if (!light.isMap() || !isVector3(light["at"]) || !number(light["radius"]) || light["radius"].asReal() <= 0.0
                || light["radius"].asReal() > LIGHT_MAX_RADIUS || !isVector3(colour))
            {
                fail(llformat("each of the config's lights needs {at: [x, y, z], radius above 0 and at most %.0f, "
                              "colour: [r, g, b]}", LIGHT_MAX_RADIUS));
                return;
            }
            State::LocalLight entry;
            entry.kind = "light";
            entry.at = vector3(light["at"]);
            // As make-scene.py gives the scene's own light: the colour linear at intensity 1, falloff 0.75.
            entry.light.setLinearColor(LLColor4(vector3(colour).mV[0], vector3(colour).mV[1], vector3(colour).mV[2], 1.f));
            entry.light.setRadius(F32(light["radius"].asReal()));
            entry.light.setFalloff(0.75f);
            sState.localLights.push_back(entry);
        }
        // The sky's reflection probe ambiance is optional, in-world only and local: lockEnvironment sets it
        // on its clone of the region's sky, which goes to no one. At 0 the pipeline takes the legacy gamma programs
        // (applyCAS's gCASLegacyGammaProgram, gammaCorrect's no-post path).
        const LLSD& sky = config["sky"];
        if (sky.isDefined()
            && (at_login || !sky.isMap() || sky.size() != 1 || !number(sky["probe_ambiance"])
                || sky["probe_ambiance"].asReal() < 0.0))
        {
            fail("the config's sky needs {probe_ambiance: at least 0} and no login_screen or metal_login");
            return;
        }
        // The glTF run is optional, in-world only.
        const LLSD& gltf = config["gltf"];
        if (gltf.isDefined())
        {
            const std::string file = gltf["file"].asString();
            if (at_login || !gltf.isMap() || !gltf["file"].isString() || file.size() < 5 || file[0] != '/'
                || file.compare(file.size() - 4, 4, ".glb") != 0 || !LLFile::isfile(file) || !isVector3(gltf["host"])
                || config["warmup_frames"].asInteger() <= S32(GLTF_RELOAD_FRAME))
            {
                fail(llformat("the config's gltf needs {file, host}: file the absolute path of a .glb that exists, host "
                              "[x, y, z]; warmup_frames above %u, and no login_screen or metal_login",
                              GLTF_RELOAD_FRAME));
                return;
            }
        }
        // The locked mouse point is optional, in-world only, without show_ui or drag; the raycast debug
        // display needs it.
        const LLSD& cursor = config["cursor"];
        if (cursor.isDefined())
        {
            if (at_login || show_ui.asBoolean() || drag.isDefined() || !cursor.isArray() || cursor.size() != 2
                || !cursor[0].isInteger() || !cursor[1].isInteger() || cursor[0].asInteger() < 0
                || cursor[1].asInteger() < 0 || cursor[0].asInteger() > window[0].asInteger()
                || cursor[1].asInteger() > window[1].asInteger())
            {
                fail("the config's cursor needs [x, y], two integers inside the window, and no login_screen, "
                     "metal_login, show_ui or drag");
                return;
            }
            sState.cursorLocked = true;
            sState.cursorPoint = LLCoordGL(cursor[0].asInteger(), cursor[1].asInteger());
        }
        if (((sState.renderDebug | sState.renderDebugLate) & LLPipeline::RENDER_DEBUG_RAYCAST) && !sState.cursorLocked)
        {
            fail("the config's raycast debug display needs cursor: the debug ray follows the mouse");
            return;
        }
        if (!checkTourConfig(at_login))
        {
            return;
        }
        if (login_screen)
        {
            sState.phase = Phase::LoginScreen;
        }
        else if (metal_login)
        {
            sState.phase = Phase::MetalLogin;
        }
        // Before the pipeline makes its noise textures from the generator.
        ll_seed_rand(SEED);
    }

    /// The config's cached settings, declared with the run's values before anything reads them, as
    /// LLControlCache would declare them, not saved. A setting that exists already fails the run: a settings
    /// file declares it, and the config's settings are for it.
    void declareCachedSettings()
    {
        const LLSD& config = sState.config;
        for (const auto& [name, entry] : llsd::inMap(config["cached_settings"]))
        {
            if (gSavedSettings.getControl(name))
            {
                fail(name + " is declared already; cached_settings is for settings only an LLCachedControl declares");
                return;
            }
            const std::string comment = "[Viewer Lab] golden test cached setting";
            if (entry[0].asString() == "Boolean")
            {
                gSavedSettings.declareBOOL(name, entry[1].asBoolean(), comment, LLControlVariable::PERSIST_NO);
            }
            else
            {
                gSavedSettings.declareString(name, entry[1].asString(), comment, LLControlVariable::PERSIST_NO);
            }
            LL_INFOS("Golden") << "Golden test: cached setting " << name << " = " << entry[1].asString() << LL_ENDL;
        }
    }

    /// The config's test fault, from init(), so before gPipeline.init loads the shaders
    /// for the first time: the viewer's device fails the named program at its publication.
    void armProgramFault()
    {
        const LLSD& config = sState.config; // a const lookup, which adds no key
        const LLSD& fault = config["program_fault"];
        if (!fault.isMap())
        {
            return;
        }
        LL::RHI::ProgramFault& armed = sState.programFault;
        armed.program = fault["program"].asString();
        armed.deferred = fault["deferred"].asBoolean();
        armed.times = U32(fault["times"].asInteger());
        LL::RHI::setProgramFault(armed);
        LL_INFOS("Golden") << "Golden test: program fault on " << armed.program << " ("
                           << (armed.deferred ? "deferred" : "inline") << ", times " << armed.times << ")" << LL_ENDL;
#if LL_RHI_METAL
        // Metal's fault list, deferred as the shared fault; readConfig has checked it
        const LLSD& faults = config["program_faults"];
        if (!faults.isArray())
        {
            return;
        }
        std::vector<LL::RHI::ProgramFault>& list = sState.programFaults;
        list.clear();
        for (const LLSD& entry : llsd::inArray(faults))
        {
            LL::RHI::ProgramFault& added = list.emplace_back();
            added.program = entry["program"].asString();
            added.deferred = armed.deferred;
            added.times = U32(entry["times"].asInteger());
            LL_INFOS("Golden") << "Golden test: program fault on " << added.program << " ("
                               << (added.deferred ? "deferred" : "inline") << ", times " << added.times << ")" << LL_ENDL;
        }
        LL::RHI::setMetalProgramFaults(list);
#endif
    }

    /// The config's test delay, armed with the fault, so before setShaders: the viewer's device keeps every
    /// creation of the named program Pending until frames frames after it, and the frame's trigger publishes it.
    void armProgramDelay()
    {
        const LLSD& config = sState.config; // a const lookup, which adds no key
        const LLSD& delay = config["program_delay"];
        if (!delay.isMap())
        {
            return;
        }
        LL::RHI::ProgramDelay& armed = sState.programDelay;
        armed.program = delay["program"].asString();
        armed.creations = 0; // every creation, in each shader load
        armed.frames = U32(delay["frames"].asInteger());
        LL::RHI::setProgramDelay(armed);
        LL_INFOS("Golden") << "Golden test: program delay on " << armed.program << " (frames " << armed.frames << ")"
                           << LL_ENDL;
    }

    /// A value as the type the setting is declared with (a boolean setting can hold an integer
    /// from a settings file), or undefined for other types.
    LLSD typedValue(LLControlVariable* control, const LLSD& value)
    {
        switch (control->type())
        {
        case TYPE_BOOLEAN:
            return value.asBoolean();
        case TYPE_S32:
        case TYPE_U32:
            return value.asInteger();
        case TYPE_F32:
            return F64(F32(value.asReal()));
        case TYPE_STRING:
            return value.asString();
        default:
            return LLSD();
        }
    }

    /// A setting from the config, with the type the setting is declared with. False with why
    /// when there is no such setting or it refuses the value.
    bool setSetting(const std::string& name, const LLSD& value, std::string& why)
    {
        LLControlVariable* control = gSavedSettings.getControl(name);
        if (!control)
        {
            why = "there is no setting " + name;
            return false;
        }
        const LLSD typed = typedValue(control, value);
        if (typed.isUndefined())
        {
            why = name + " is not a boolean, a number or a string";
            return false;
        }
        control->setValue(typed);
        if (!llsd_equals(typedValue(control, control->getValue()), typed))
        {
            why = name + " refused " + value.asString();
            return false;
        }
        return true;
    }

    /// The graphics level and GPU class the config locks, with the config's settings on
    /// top, before the pipeline is made, as --graphicslevel sets its level. The benchmark gives
    /// a lower class under load; on macOS the class only reaches the default graphics preset
    /// (which masks the features) and the statistics, and the profile skips the benchmark
    /// (SkipBenchmark), so the class is set here.
    void applyGraphics()
    {
        const LLSD& config = sState.config;
        std::vector<LLSD> startup;
        for (const char* name : STARTUP_SETTINGS)
        {
            LLControlVariable* control = gSavedSettings.getControl(name);
            startup.push_back(typedValue(control, control->getValue()));
        }
        LLFeatureManager& features = LLFeatureManager::instance();
        // The class loadGPUClass gave, from the benchmark when the profile does not skip it (level2.py --benchmark),
        // before the config's class replaces it.
        sState.benchmarkClass = (S32)features.getGPUClass();
        LL_INFOS("Golden") << "Golden test: benchmark class " << sState.benchmarkClass << LL_ENDL;
        features.setGPUClass(EGPUClass(config["gpu_class"].asInteger()));
        const U32 level = U32(config["graphics_level"].asInteger());
        features.setGraphicsLevel(level, false);
        gSavedSettings.setU32("RenderQualityPerformance", level);
        std::string why;
        for (const auto& [name, value] : llsd::inMap(config["settings"]))
        {
            if (!setSetting(name, value, why))
            {
                fail(why);
                return;
            }
        }
        for (size_t i = 0; i < std::size(STARTUP_SETTINGS); ++i)
        {
            LLControlVariable* control = gSavedSettings.getControl(STARTUP_SETTINGS[i]);
            const LLSD now = typedValue(control, control->getValue());
            if (!llsd_equals(now, startup[i]))
            {
                fail(llformat("%s is read once at startup, so the profile must have it at %s, not %s",
                              STARTUP_SETTINGS[i], now.asString().c_str(), startup[i].asString().c_str()));
                return;
            }
        }
        LL_INFOS("Golden") << "Golden test: graphics level " << level << " (" << features.getNameForGraphicsLevel(level)
                           << "), GPU class " << features.getGPUClass() << ", " << config["settings"].size()
                           << " settings on top" << LL_ENDL;
    }

    /// The region's environment, held still: its first sky with the clouds not moving, and its
    /// first water with no waves moving, as the local environment. Cloud scrolling and
    /// waves follow the clock; without movement neither shows it. False until the region's day
    /// has arrived.
    bool lockEnvironment()
    {
        LLEnvironment& environment = LLEnvironment::instance();
        LLSettingsDay::ptr_t day = environment.getEnvironmentDay(LLEnvironment::ENV_REGION);
        if (!day)
        {
            return false;
        }
        const LLSettingsDay::CycleTrack_t& skies = day->getCycleTrackConst(LLSettingsDay::TRACK_GROUND_LEVEL);
        const LLSettingsDay::CycleTrack_t& waters = day->getCycleTrackConst(LLSettingsDay::TRACK_WATER);
        if (skies.empty() || waters.empty())
        {
            fail("the region's day has no sky or no water");
            return false;
        }
        LLSettingsSky::ptr_t sky = std::static_pointer_cast<LLSettingsSky>(skies.begin()->second)->buildClone();
        LLSettingsWater::ptr_t water = std::static_pointer_cast<LLSettingsWater>(waters.begin()->second)->buildClone();
        sky->setCloudScrollRate(LLVector2(0.f, 0.f));
        water->setWave1Dir(LLVector2(0.f, 0.f));
        water->setWave2Dir(LLVector2(0.f, 0.f));
        const LLSD& config = sState.config; // const: reading a key must not add it
        const LLSD& probe_ambiance = config["sky"]["probe_ambiance"];
        if (probe_ambiance.isDefined())
        {
            // Local only, as the rest of the clone; it also ends the sky's auto adjustment.
            sky->setReflectionProbeAmbiance(F32(probe_ambiance.asReal()));
        }
        environment.setEnvironment(LLEnvironment::ENV_LOCAL, sky, water);
        environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT, true);
        LL_INFOS("Golden") << "Golden test: environment locked to the region's first sky and water, nothing moving"
                           << (probe_ambiance.isDefined() ? llformat(", the sky's probe ambiance %.2f",
                                                                     sky->getReflectionProbeAmbiance(false))
                                                          : std::string())
                           << LL_ENDL;
        return true;
    }

    /// Why the scene's assets are not ready yet, or empty: a texture fetch, decode, GL creation
    /// or downscale pending, the discard bias above its floor of 1, a texture in use not at the
    /// discard level it wants, a mesh request, or a scene material not loaded.
    std::string assetsNotReady()
    {
        if (!sState.environmentLocked)
        {
            return "the environment is not locked yet";
        }
        const S32 fetches = LLAppViewer::getTextureFetch()->getNumRequests();
        if (fetches > 0)
        {
            return llformat("%d texture fetches pending", fetches);
        }
        if (!gTextureList.mCreateTextureList.empty())
        {
            return llformat("%d textures waiting for GL", (S32)gTextureList.mCreateTextureList.size());
        }
        if (!gTextureList.mDownScaleQueue.empty())
        {
            return llformat("%d textures waiting to be scaled down", (S32)gTextureList.mDownScaleQueue.size());
        }
        if (LLViewerTexture::sDesiredDiscardBias > 1.f)
        {
            return llformat("the discard bias is %.2f", LLViewerTexture::sDesiredDiscardBias);
        }
        S32 coarse = 0;
        S32 dropped = 0;
        std::string example;
        sState.unavailable = 0;
        sState.unrequested = 0;
        sState.unrequestedBoosted = 0;
        sState.settledSharper = 0;
        sState.settledSharperAbove0 = 0;
        for (const LLPointer<LLViewerFetchedTexture>& entry : gTextureList)
        {
            LLViewerFetchedTexture* image = entry.get(); // the list's entries are const, the textures not
            const S32 desired = image->getDesiredDiscardLevel();
            if (image->getLastReferencedTimer()->getElapsedTimeF32() >= IN_USE_SECONDS || desired < 0)
            {
                continue; // not in use
            }
            if (image->isMissingAsset())
            {
                ++sState.unavailable; // the region does not have it
                continue;
            }
            if (!image->isFetching() && !image->hasFetcher() && image->getDiscardLevel() < 0)
            {
                // No data and never fetched: the viewer does not ask for it (nothing of it shows).
                // The check after the warm-up sees it if that changes. A boosted one may be a fetch the teleport's
                // clear cut before its first data: the ready line counts those.
                ++sState.unrequested;
                if (image->getBoostLevel() >= LLGLTexture::BOOST_HIGH)
                {
                    ++sState.unrequestedBoosted;
                }
                continue;
            }
            // The level a texture can reach is capped by its size. More detail than wanted is scaled down later on an
            // LOD texture and stays on any other; a boosted texture whose fetch the teleport's first frame stopped is
            // asked for again (ll_golden_texture_state).
            const bool boosted = image->getBoostLevel() >= LLGLTexture::BOOST_HIGH;
            const LLGoldenTexture state =
                ll_golden_texture_state(image->isFetching() || image->hasFetcher(), image->isFullyLoaded(),
                                        image->getType() == LLViewerTexture::LOD_TEXTURE, boosted, image->getMaxVirtualSize(),
                                        image->getDiscardLevel(), desired, (S32)image->getMaxDiscardLevel());
            if (state == LLGoldenTexture::Settled
                && ll_golden_sharper_than_wanted(image->getDiscardLevel(), desired, (S32)image->getMaxDiscardLevel()))
            {
                // Logging only: the ready line shows whether a cut fetch left one coarser than 0.
                ++sState.settledSharper;
                if (image->getDiscardLevel() > 0)
                {
                    ++sState.settledSharperAbove0;
                }
            }
            if (state != LLGoldenTexture::Settled)
            {
                if (coarse++ == 0)
                {
                    example = llformat(", e.g. %s at %d, wants %d, %s, %dx%d, boost %d", image->getID().asString().c_str(),
                                       image->getDiscardLevel(), desired, image->isFetching() ? "fetching" : "not fetching",
                                       image->getFullWidth(), image->getFullHeight(), image->getBoostLevel());
                }
            }
            if (state == LLGoldenTexture::Dropped)
            {
                // Its boost's own request once more: the priority setBoostLevel gives a boosted texture, and level 0.
                image->setBoostLevel(image->getBoostLevel());
                ++dropped;
            }
        }
        if (dropped > 0)
        {
            sState.askedAgain += dropped;
            LL_INFOS("Golden") << "Golden test: asked again for " << dropped << " boosted textures whose fetch had stopped"
                               << " at priority 0 (" << sState.askedAgain << " in all)" << LL_ENDL;
        }
        if (coarse > 0)
        {
            return llformat("%d textures in use not at their desired discard level", coarse) + example;
        }
        const S32 meshes = LLMeshRepoThread::sActiveHeaderRequests + LLMeshRepoThread::sActiveLODRequests
                         + LLMeshRepoThread::sActiveSkinRequests + (S32)gMeshRepo.mPendingRequests.size();
        if (meshes > 0)
        {
            return llformat("%d mesh requests pending", meshes);
        }
        for (const LLSD& id : llsd::inArray(sState.config["materials"]))
        {
            LLFetchedGLTFMaterial* material = gGLTFMaterialList.getMaterial(id.asUUID());
            if (!material || !material->isLoaded())
            {
                return "material " + id.asString() + " not loaded";
            }
        }
        return {};
    }

    /// [Viewer Lab] The reflection probes on or off, and their updates paused or not.
    LLGoldenProbes probeStatus()
    {
        return { LLPipeline::sReflectionProbesEnabled, gPipeline.mReflectionMapManager.isPaused() };
    }

    /// Fails when two reflection probes are equally far from the camera: they would take their
    /// slots in the cube map array in an order that follows the network (std::sort is not
    /// stable). False then.
    bool probeDistancesDiffer()
    {
        std::vector<LLGoldenProbe> probes;
        for (const LLSD& probe : llsd::inArray(gPipeline.mReflectionMapManager.probeState()["probes"]))
        {
            if (!probe["default"].asBoolean())
            {
                probes.push_back({ LLVector3(F32(probe["origin"][0].asReal()), F32(probe["origin"][1].asReal()),
                                             F32(probe["origin"][2].asReal())),
                                   F32(probe["radius"].asReal()) });
            }
        }
        // [Viewer Lab] The comparison is ll_golden_probe_tie's, so a unit test holds it.
        const std::string tie = ll_golden_probe_tie(LLViewerCamera::getInstance()->getOrigin(), probes);
        if (!tie.empty())
        {
            fail(tie + " are equally far from the camera; move the camera");
            return false;
        }
        return true;
    }

    // [Viewer Lab] Golden's selection run (select, tool, drag): the scene's objects selected with a manipulator
    // tool, so that renderSelections draws their silhouettes and the manipulator (llviewerwindow.cpp), which only the UI
    // pass does (show_ui).

    /// The config's tool (select): LLToolCompTranslate or LLToolCompScale, or null without one.
    LLToolComposite* selectionTool()
    {
        const std::string tool = sState.config["tool"].asString();
        if (tool == "translate")
        {
            return LLToolCompTranslate::getInstance();
        }
        if (tool == "scale")
        {
            return LLToolCompScale::getInstance();
        }
        return nullptr;
    }

    /// The tool's manipulator, the composite's current tool from its construction on (lltoolcomp.cpp), or null.
    LLManip* selectionManip()
    {
        LLToolComposite* tool = selectionTool();
        return tool ? dynamic_cast<LLManip*>(tool->getCurrentTool()) : nullptr;
    }

    /// A manipulator part as the render profile names it: none, the translate handles by name, any other by number.
    std::string manipPartName(S32 part)
    {
        static const char* const NAMES[] = { "none", "x_arrow", "y_arrow", "z_arrow", "yz_plane", "xz_plane", "xy_plane" };
        return part >= 0 && part < S32(std::size(NAMES)) ? NAMES[part] : llformat("part_%d", part);
    }

    /// The root object of the agent's region, not an avatar, nearest expected (region coordinates) within
    /// FIND_DISTANCE, or null while there is none.
    LLViewerObject* findSceneObjectAt(const LLVector3& expected)
    {
        LLViewerRegion* region = gAgent.getRegion();
        LLViewerObject* nearest = nullptr;
        F32 nearest_distance = FIND_DISTANCE;
        for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
        {
            LLViewerObject* object = gObjectList.getObject(i);
            if (!object || object->isDead() || object->isAvatar() || object->getParent() || object->getRegion() != region)
            {
                continue;
            }
            const F32 distance = dist_vec(object->getPositionRegion(), expected);
            if (distance < nearest_distance)
            {
                nearest = object;
                nearest_distance = distance;
            }
        }
        return nearest;
    }

    /// The scene's object the config names (select), at the config's expected place (findSceneObjectAt). Its name is
    /// checked once it is selected, when the region has sent its properties.
    LLViewerObject* findSceneObject(const std::string& name)
    {
        return findSceneObjectAt(vector3(sState.config["expected_positions"][name]));
    }

    /// The config's selection (select): once the region has sent every named object, selects them in the config's
    /// order and makes the config's tool the current one. Returns why it is not ready: an object not in the scene yet,
    /// or a node without its properties from the region. A node whose name is not the one named, an object that left
    /// the selection or another current tool fails the run. Empty when ready or when the config selects nothing.
    std::string selectionNotReady()
    {
        const LLSD& names = sState.config["select"];
        if (!names.isArray())
        {
            return {};
        }
        LLSelectMgr* select_mgr = LLSelectMgr::getInstance();
        if (sState.selected.empty())
        {
            std::vector<LLViewerObject*> objects;
            for (const LLSD& name : llsd::inArray(names))
            {
                LLViewerObject* object = findSceneObject(name.asString());
                if (!object)
                {
                    return "the object " + name.asString() + " is not in the scene yet";
                }
                objects.push_back(object);
            }
            for (size_t i = 0; i < objects.size(); ++i)
            {
                // The local region's first updates of a static object come with no flags for the account (move,
                // modify and you-owner 0 on every object the account owns but the animated one;
                // OpenSim's GenerateClientFlags gives 0 without a presence), and selecting an owned object sends only
                // its properties. A full update asked for as a cache miss (RequestMultipleObjects, which
                // LLWorld::requestCacheMisses sends in an idle) brings them; without them renderSelections draws no
                // manipulator.
                objects[i]->getRegion()->addCacheMissFull(objects[i]->getLocalID());
                select_mgr->selectObjectAndFamily(objects[i], true);
                sState.selected.emplace_back(names[S32(i)].asString(), objects[i]->getID());
                LL_INFOS("Golden") << "Golden test: selected " << objects[i]->getID() << " at " << objects[i]->getPositionRegion()
                                   << " for " << names[S32(i)].asString() << LL_ENDL;
            }
            // As a toolbar's tool button picks it in the basic toolset, which stays the current toolset.
            if (LLToolMgr::getInstance()->getCurrentToolset() != gBasicToolset)
            {
                fail("the selection needs the basic toolset");
                return "failed";
            }
            gBasicToolset->selectTool(selectionTool());
            // Without the Build floater: LLViewerWindow::updateLayout opens it for a build tool unless the toolset says
            // not to, and it shows text that comes from the region and the name cache on the wall clock (names, land
            // impact) over the left of the view.
            gBasicToolset->setShowFloaterTools(false);
            LL_INFOS("Golden") << "Golden test: the current tool is " << sState.config["tool"].asString() << LL_ENDL;
        }
        // OpenSim's FailedToGetBenefits alert (alertmodal) comes with every login, and a visible modal dialog takes
        // the mouse whenever the floaters sync their order (LLFloaterView::syncFloaterTabOrder, from updateUI), so no
        // drag could hold it: cancelled here, as a user would close it with OK (its callback only logs). Any other modal
        // dialog keeps the selection waiting.
        LLNotifications::instance().cancelByName("FailedToGetBenefits");
        if (LLModalDialog::activeCount() > 0)
        {
            return "a modal dialog is open, which holds the mouse";
        }
        LLObjectSelectionHandle selection = select_mgr->getSelection();
        for (const auto& [name, id] : sState.selected)
        {
            LLViewerObject* object = gObjectList.findObject(id);
            LLSelectNode* node = object && !object->isDead() ? selection->findNode(object) : nullptr;
            if (!node)
            {
                fail("the object " + name + " is not selected (or no longer in the scene)");
                return "failed";
            }
            if (!node->mValid)
            {
                return "the region has not sent the properties of " + name + " yet";
            }
            if (node->mName != name)
            {
                fail("the object found for " + name + " is named " + node->mName + "; reload the scene's OAR");
                return "failed";
            }
        }
        if (LLToolMgr::getInstance()->getCurrentTool() != selectionTool())
        {
            fail("the current tool is no longer the config's " + sState.config["tool"].asString());
            return "failed";
        }
        // renderSelections draws the manipulator only when the account may move (translate) or modify (scale) every
        // selected object (llviewerwindow.cpp), and the manipulator draws for its own selection handle. The objects'
        // move and modify flags come with the region's object updates, not with the properties, so this waits too.
        bool move = false;
        bool modify = false;
        select_mgr->selectGetEditMoveLinksetPermissions(move, modify);
        LLManip* manip = selectionManip();
        const S32 manip_objects = manip ? manip->getSelection()->getObjectCount() : -1;
        if (!(sState.config["tool"].asString() == "translate" ? move : modify) || manip_objects != S32(sState.selected.size()))
        {
            std::string why = llformat("the %s manipulator would not draw: move %d, modify %d, its selection has %d objects",
                                       sState.config["tool"].asString().c_str(), move, modify, manip_objects);
            for (const auto& [name, id] : sState.selected)
            {
                LLViewerObject* object = gObjectList.findObject(id);
                LLSelectNode* node = object ? selection->findNode(object) : nullptr;
                why += llformat("; %s: move flag %d, modify flag %d, owner %s", name.c_str(),
                                object && object->flagObjectMove(), object && object->flagObjectModify(),
                                node && node->mPermissions ? (node->mPermissions->getOwner() == gAgent.getID()
                                                                  ? "the account" : node->mPermissions->getOwner().asString().c_str())
                                                           : "unknown");
            }
            return why;
        }
        return {};
    }

    /// The drag (drag xy_plane), at the warm-up's start: the first point of a fixed grid around the selection centre's
    /// projection whose highlightManipulators highlights the XY plane handle, where the drag starts and the mouse is
    /// pinned from then on (beforeUI). The manipulator's handles are those of the frames before, which drew them. False
    /// after fail().
    bool startDrag()
    {
        if (gSavedSettings.getBOOL("SnapToMouseCursor"))
        {
            fail("the drag needs SnapToMouseCursor off: handleMouseDownOnPart would move the system's cursor");
            return false;
        }
        LLManipTranslate* manip = dynamic_cast<LLManipTranslate*>(selectionManip());
        LLCoordGL centre;
        if (!manip
            || !LLViewerCamera::getInstance()->projectPosAgentToScreen(
                   LLSelectMgr::getInstance()->getBBoxOfSelection().getCenterAgent(), centre))
        {
            fail("the drag needs the translate manipulator and the selection's centre on the screen");
            return false;
        }
        S32 probes = 0;
        for (S32 dy = -DRAG_PROBE_RANGE; dy <= DRAG_PROBE_RANGE; dy += DRAG_PROBE_STEP)
        {
            for (S32 dx = -DRAG_PROBE_RANGE; dx <= DRAG_PROBE_RANGE; dx += DRAG_PROBE_STEP)
            {
                ++probes;
                const S32 x = centre.mX + dx;
                const S32 y = centre.mY + dy;
                manip->highlightManipulators(x, y);
                if (manip->getHighlightedPart() != LLManip::LL_XY_PLANE)
                {
                    continue;
                }
                manip->handleMouseDownOnPart(x, y, MASK_NONE);
                if (!manip->hasMouseCapture() || manip->getHighlightedPart() != LLManip::LL_XY_PLANE)
                {
                    fail(llformat("the drag on the XY plane handle at %d, %d did not start", x, y));
                    return false;
                }
                sState.dragPinned = true;
                sState.dragPoint = LLCoordGL(x, y);
                sState.dragProbes = probes;
                LL_INFOS("Golden") << "Golden test: dragging the XY plane handle from " << x << ", " << y << " (probe "
                                   << probes << ", selection centre at " << centre.mX << ", " << centre.mY << ")" << LL_ENDL;
                return true;
            }
        }
        fail(llformat("no point within %d of the selection centre at %d, %d highlights the XY plane handle (%d probes); "
                      "the camera must look down on it",
                      DRAG_PROBE_RANGE, centre.mX, centre.mY, probes));
        return false;
    }

    /// GLTFEnabled for the glTF run (gltf), from each in-world frame. LLViewerRegion::setSimulatorFeatures queues
    /// its work on the main loop's queue before it says the features came (setSimulatorFeaturesReceived), and that work
    /// turns GLTFEnabled off for a region that does not send it, as the local one does not. So once the features have
    /// come, the hook queues its own work behind the region's (the queue runs in order): GLTFEnabled on for this run,
    /// not saved, and the shaders loaded again, as the region's work does for a region that sends it on, so that the
    /// glTF program is made. From then on it must stay on.
    void keepGLTF()
    {
        if (!sState.config.has("gltf"))
        {
            return;
        }
        if (!sState.gltfPosted)
        {
            LLViewerRegion* region = gAgent.getRegion();
            if (!region || !region->simulatorFeaturesReceived())
            {
                return;
            }
            sState.gltfPosted = true;
            LLAppViewer::instance()->postToMainCoro(
                []()
                {
                    if (sState.phase == Phase::Done)
                    {
                        return;
                    }
                    LLControlVariable* control = gSavedSettings.getControl("GLTFEnabled");
                    if (!control)
                    {
                        fail("gltf: GLTFEnabled is not declared");
                        return;
                    }
                    control->setPersist(LLControlVariable::PERSIST_NO);
                    gSavedSettings.setBOOL("GLTFEnabled", true);
                    LLViewerShaderMgr::instance()->setShaders();
                    sState.gltfOn = true;
                    LL_INFOS("Golden") << "Golden test: GLTFEnabled on for the run and the shaders loaded again" << LL_ENDL;
                });
            return;
        }
        if (sState.gltfOn && !gSavedSettings.getBOOL("GLTFEnabled"))
        {
            fail("gltf: GLTFEnabled was turned off again");
        }
    }

    /// The glTF run's readiness (gltf): GLTFEnabled on and the shaders loaded again (keepGLTF), then the host in
    /// the scene. Empty when ready or when the config has no gltf.
    std::string gltfNotReady()
    {
        if (!sState.config.has("gltf"))
        {
            return {};
        }
        if (!sState.gltfOn)
        {
            return "glTF is not on yet (the region's features have not come, or the hook's work has not run)";
        }
        LLViewerObject* host = findSceneObjectAt(vector3(sState.config["gltf"]["host"]));
        if (!host)
        {
            return "the glTF host object is not in the scene yet";
        }
        sState.gltfHost = host->getID();
        return {};
    }

    /// Whether the object has the config's block of a local light in use, as the hook gave it.
    bool localLightHeld(const State::LocalLight& entry, const LLViewerObject* object)
    {
        if (entry.kind == "projector")
        {
            const LLLightImageParams* image = object->getLightImageParams();
            return image && image->operator==(static_cast<const LLNetworkData&>(entry.image));
        }
        const LLLightParams* light = object->getLightParams();
        return light && light->operator==(static_cast<const LLNetworkData&>(entry.light));
    }

    /// Gives the object its block locally: setParameterEntry with local_origin false, so nothing goes to
    /// the region (LLViewerObject::parameterChanged sends only a local origin), and LLVOVolume::parameterChanged puts a
    /// new light into the pipeline's light set.
    void applyLocalLight(const State::LocalLight& entry, LLViewerObject* object)
    {
        if (entry.kind == "projector")
        {
            object->setParameterEntry(LLNetworkData::PARAMS_LIGHT_IMAGE, entry.image, false);
        }
        else
        {
            object->setParameterEntry(LLNetworkData::PARAMS_LIGHT, entry.light, false);
        }
    }

    /// The config's projector and local lights, before the probes start over, so the warm-up's first frame
    /// has them: once every object is in the scene (the root object nearest its place, within FIND_DISTANCE, as
    /// select finds one), the hook gives each its block once; the projector's object must be a light already (the
    /// region's point light). Returns why it is not ready: an object not in the scene yet, or a block the region took
    /// away again (keepLocalLights gives it back before the next frame's UI), which at the snapshot fails the run.
    std::string localLightsNotReady()
    {
        for (State::LocalLight& entry : sState.localLights)
        {
            LLViewerObject* object = entry.object.notNull() ? gObjectList.findObject(entry.object) : nullptr;
            if (!object || object->isDead())
            {
                if (sState.localLightsFound)
                {
                    return llformat("the %s object at %.1f, %.1f, %.1f left the scene", entry.kind.c_str(),
                                    entry.at.mV[0], entry.at.mV[1], entry.at.mV[2]);
                }
                object = findSceneObjectAt(entry.at);
                if (!object || !dynamic_cast<LLVOVolume*>(object) || object->mDrawable.isNull())
                {
                    return llformat("the %s object at %.1f, %.1f, %.1f is not in the scene yet", entry.kind.c_str(),
                                    entry.at.mV[0], entry.at.mV[1], entry.at.mV[2]);
                }
                if (entry.kind == "projector" && !object->getLightParams())
                {
                    fail(llformat("projector: the object at %.1f, %.1f, %.1f is not a light", entry.at.mV[0],
                                  entry.at.mV[1], entry.at.mV[2]));
                    return "failed";
                }
                entry.object = object->getID();
                applyLocalLight(entry, object);
                LL_INFOS("Golden") << "Golden test: the " << entry.kind << " is " << object->getID() << " at "
                                   << object->getPositionRegion() << LL_ENDL;
            }
            if (!localLightHeld(entry, object))
            {
                return llformat("the %s object at %.1f, %.1f, %.1f lost its local block", entry.kind.c_str(),
                                entry.at.mV[0], entry.at.mV[1], entry.at.mV[2]);
            }
        }
        sState.localLightsFound = true;
        return {};
    }

    /// The local blocks given back in every in-world frame before the UI, after the network's messages (idleNetwork):
    /// a full ObjectUpdate resets every block the region does not send (LLViewerObject::unpackParameterEntry's callers),
    /// so the frame drawn next has them again. Counted as information (measured.local_lights).
    void keepLocalLights()
    {
        for (const State::LocalLight& entry : sState.localLights)
        {
            LLViewerObject* object = entry.object.notNull() ? gObjectList.findObject(entry.object) : nullptr;
            if (object && !object->isDead() && !localLightHeld(entry, object))
            {
                applyLocalLight(entry, object);
                ++sState.localLightReapplies;
                if (sState.phase == Phase::Warming || sState.phase == Phase::Previews)
                {
                    ++sState.localLightReappliesInWarmup;
                }
                LL_INFOS("Golden") << "Golden test: the " << entry.kind << " block of " << entry.object
                                   << " given again at frame " << LLFrameTimer::getFrameCount() << LL_ENDL;
            }
        }
    }

    /// measured.local_lights: each object, its kind, place and whether it holds its block and is a spot
    /// light (the projector) or a light at the snapshot, and the blocks given again.
    LLSD localLightsMeasures()
    {
        LLSD objects = LLSD::emptyArray();
        for (const State::LocalLight& entry : sState.localLights)
        {
            LLViewerObject* object = gObjectList.findObject(entry.object);
            LLVOVolume* volume = dynamic_cast<LLVOVolume*>(object);
            const LLVector3 at = object ? object->getPositionRegion() : LLVector3::zero;
            objects.append(llsd::map("kind", entry.kind, "object", entry.object, "position",
                                     llsd::array(at.mV[0], at.mV[1], at.mV[2]), "held",
                                     object != nullptr && localLightHeld(entry, object), "spotlight",
                                     volume != nullptr && volume->isLightSpotlight(), "light",
                                     volume != nullptr && volume->getIsLight()));
        }
        return llsd::map("objects", objects, "reapplied", (S32)sState.localLightReapplies, "reapplied_in_warmup",
                         (S32)sState.localLightReappliesInWarmup);
    }

    /// One load of the glTF run (gltf), at the warm-up's start and GLTF_RELOAD_FRAME frames later: the host
    /// selected alone, GLTFSceneManager::load(file) as File > Open does, which hangs the new asset off the selection's
    /// first root object and opens the asset editor, the editor closed, as it lets go of its asset only when it closes
    /// (LLFloaterGLTFAssetEditor::onClose), and the host deselected. The second load replaces the first asset, which
    /// then dies. False after a failure.
    bool loadGLTF()
    {
        LLViewerObject* host = gObjectList.findObject(sState.gltfHost);
        if (!host || host->isDead())
        {
            fail("gltf: the host object is no longer in the scene");
            return false;
        }
        const std::string file = sState.config["gltf"]["file"].asString();
        const S32 frame = S32(LLFrameTimer::getFrameCount() - sState.viewerFrameAtWarmup);
        LL_INFOS("Golden") << "Golden test: glTF load " << sState.gltfLoads.size() + 1 << " of " << file << " on "
                           << host->getID() << " at " << host->getPositionRegion() << ", warm-up frame " << frame
                           << LL_ENDL;
        LLSelectMgr* select_mgr = LLSelectMgr::getInstance();
        select_mgr->deselectAll();
        select_mgr->selectObjectAndFamily(host);
        const std::shared_ptr<LL::GLTF::Asset> before = host->mGLTFAsset;
        LL::GLTFSceneManager::instance().load(file);
        const std::shared_ptr<LL::GLTF::Asset> asset = host->mGLTFAsset;
        if (LLFloater* editor = LLFloaterReg::findInstance("gltf_asset_editor"))
        {
            editor->closeFloater();
        }
        select_mgr->deselectAll();
        if (!asset || asset == before)
        {
            fail("gltf: GLTFSceneManager::load hung no new asset off the host (the log has the loader's warnings)");
            return false;
        }
        const auto& objects = LL::GLTFSceneManager::instance().mObjects;
        if (std::none_of(objects.begin(), objects.end(), [host](const LLPointer<LLViewerObject>& o) { return o.get() == host; }))
        {
            fail("gltf: the host is not among the glTF scene manager's objects");
            return false;
        }
        if (LLFloaterReg::instanceVisible("gltf_asset_editor") || select_mgr->getSelection()->getObjectCount() != 0)
        {
            fail("gltf: the asset editor is still open, or something is still selected");
            return false;
        }
        if (sState.gltfLoads.size() == 0)
        {
            sState.gltfFirst = asset;
        }
        sState.gltfLoads.append(frame);
        LL_INFOS("Golden") << "Golden test: glTF loaded: " << asset->mNodes.size() << " nodes, " << asset->mMeshes.size()
                           << " meshes, " << asset->mMaterials.size() << " materials, " << asset->mSkins.size()
                           << " skins, " << asset->mAnimations.size() << " animations, " << asset->mImages.size()
                           << " images; host rotation " << host->getRotationRegion() << ", scale " << host->getScale()
                           << LL_ENDL;
        return true;
    }

    // The terrain partition's octree. Where a terrain patch sits in the octree follows the order in which the
    // patches went in and moved while the region loaded, not only where the patches are: a new node's centre is pushed
    // towards the element that made it and stays while the node has elements (LLOctreeNode::insert), the root grows
    // towards the first element and shrinks on balance (LLOctreeRoot), and a patch that still fits its node after a
    // move stays there (LLSpatialGroup::updateInGroup). A group the camera partly sees adds every patch in it to
    // LLSpatialPartition::getVisibleExtents, and the sun's shadow frusta are fitted to those extents
    // (LLPipeline::generateSunShadow). Layer data that comes in a frame after the first world frame therefore groups the
    // patches differently and moves the shadows: the scene state's shadow_extents and
    // visible_extents_by_partition/1 then differ from run to run.

    /// The partition's nodes (centre, size and element count, sorted) and its drawables.
    class TerrainNodes : public OctreeTraveler
    {
    public:
        std::vector<std::string> mNodes;
        std::vector<LLDrawable*> mDrawables;

        void visit(const OctreeNode* node) override
        {
            const LLVector4a& center = node->getCenter();
            const LLVector4a& size = node->getSize();
            mNodes.push_back(llformat("%.4f %.4f %.4f %.4f %u", center[0], center[1], center[2], size[0],
                                      node->getElementCount()));
            for (OctreeNode::const_element_iter it = node->getDataBegin(); it != node->getDataEnd(); ++it)
            {
                if (LLDrawable* drawable = (LLDrawable*)(*it)->getDrawable())
                {
                    mDrawables.push_back(drawable);
                }
            }
        }
    };

    /// The octree's groups, an MD5 of its sorted nodes and what the world camera sees of it, as measured has them.
    LLSD terrainOctree(LLSpatialPartition* part)
    {
        TerrainNodes nodes;
        nodes.traverse(part->mOctree);
        std::sort(nodes.mNodes.begin(), nodes.mNodes.end());
        std::string text;
        for (const std::string& node : nodes.mNodes)
        {
            text += node + "\n";
        }
        char digest[33];
        LLMD5((const unsigned char*)text.c_str()).hex_digest(digest);
        LLSD result = llsd::map("groups", (S32)nodes.mNodes.size(), "nodes", digest);
        const S32 camera = LLViewerCamera::sCurCameraID;
        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
        LLVector3 min(65536.f, 65536.f, 65536.f);
        LLVector3 max(-65536.f, -65536.f, -65536.f);
        part->getVisibleExtents(*LLViewerCamera::getInstance(), min, max);
        LLViewerCamera::sCurCameraID = (LLViewerCamera::eCameraID)camera;
        if (min.mV[VX] <= max.mV[VX])
        {
            result["visible"] = llsd::array(ll_sd_from_vector3(min), ll_sd_from_vector3(max));
        }
        return result;
    }

    /// The patches of the region's terrain partition in LLSurface's patch order (y, then x).
    std::vector<LLDrawable*> terrainPatches(LLSpatialPartition* part)
    {
        TerrainNodes nodes;
        nodes.traverse(part->mOctree);
        std::vector<LLDrawable*> patches = nodes.mDrawables;
        std::sort(patches.begin(), patches.end(), [](LLDrawable* a, LLDrawable* b)
        {
            const LLVector3& pa = a->getVObj()->LLXform::getPosition();
            const LLVector3& pb = b->getVObj()->LLXform::getPosition();
            return pa.mV[VY] != pb.mV[VY] ? pa.mV[VY] < pb.mV[VY] : pa.mV[VX] < pb.mV[VX];
        });
        return patches;
    }

    /// The region's terrain octree made again from nothing, as a load whose layer data all came before the first world
    /// frame makes it. Only the octree and its groups change; the heights, the patches' objects and their geometry stay.
    /// - LLSurface::createPatchData makes each patch's object before setOriginGlobal gives the patch its centre, so the
    ///   object stands at the region's origin, flat (scale 16, 16, 0), until the patch's first updateVerticalStats; the
    ///   partition's root is as LLViewerOctreePartition makes it (centre 0, size 1).
    /// - In the first world frame LLSurface::idleUpdate gives every patch updateVerticalStats in patch order, whose
    ///   dirtyPatch puts the drawable in the partition (dirtyGeom's movePartition) before it sets the object's position
    ///   and scale; then LLPipeline::updateMovedList moves each to its bounds in patch order (createPatchData marked them
    ///   moved in that order), and the frame's balance.
    bool replayTerrainOctree(LLSpatialPartition* part, const std::vector<LLDrawable*>& patches)
    {
        std::vector<std::pair<LLVector3, LLVector3>> finals; // each object's position and scale
        for (LLDrawable* drawable : patches)
        {
            finals.emplace_back(drawable->getVObj()->LLXform::getPosition(), drawable->getVObj()->getScale());
            if (LLSpatialGroup* group = drawable->getSpatialGroup())
            {
                part->remove(drawable, group); // LLOctreeNode::remove deletes each node it leaves empty
            }
        }
        OctreeNode* root = part->mOctree;
        if (patches.empty() || root->getChildCount() != 0 || root->getElementCount() != 0)
        {
            return false; // something but the patches is in the terrain partition
        }
        LLVector4a center;
        LLVector4a size;
        center.splat(0.f);
        size.splat(1.f);
        root->setCenter(center);
        root->setSize(size);
        root->updateMinMax();
        for (size_t i = 0; i < patches.size(); ++i)
        {
            LLViewerObject* object = patches[i]->getVObj();
            object->LLXform::setPosition(LLVector3::zero);
            object->LLXform::setScale(LLVector3(finals[i].second.mV[VX], finals[i].second.mV[VY], 0.f));
            part->put(patches[i]);
            object->LLXform::setPosition(finals[i].first);
            object->LLXform::setScale(finals[i].second);
        }
        for (LLDrawable* drawable : patches)
        {
            part->move(drawable, drawable->getSpatialGroup());
        }
        root->balance();
        // The groups are new: each patch's geometry is made again, as when a patch changes (LLVOSurfacePatch::dirtyGeom).
        for (LLDrawable* drawable : patches)
        {
            static_cast<LLVOSurfacePatch*>(drawable->getVObj().get())->dirtyGeom();
        }
        return true;
    }

    /// The scene's terrain octree, the same in every run before the reflection probes start over: replayed as a
    /// load with all layer data before the first world frame, whatever the network did. The config's terrain_load
    /// "as_loaded" leaves it as the network made it (to look at its run-to-run differences).
    bool settleTerrain()
    {
        const bool replay = sState.config["terrain_load"].asString() != "as_loaded";
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            LLSpatialPartition* part = region->getSpatialPartition(LLViewerRegion::PARTITION_TERRAIN);
            if (!part)
            {
                continue;
            }
            const std::vector<LLDrawable*> patches = terrainPatches(part);
            sState.terrain["as_loaded"] = terrainOctree(part);
            sState.terrain["patches"] = (S32)patches.size();
            if (replay && !replayTerrainOctree(part, patches))
            {
                fail("the terrain partition holds something but the region's patches");
                return false;
            }
            sState.terrain["replayed"] = replay;
            sState.terrain["octree"] = terrainOctree(part);
            LL_INFOS("Golden") << "Golden test: terrain octree " << sState.terrain << LL_ENDL;
        }
        return true;
    }

    /// [Viewer Lab] The home on the scene's avatar spot (config "avatar"), not where the avatar stands. In
    /// level 2's pool two avatars on one spot stand apart, deterministically up to about 0.2 m, and a home set where
    /// the avatar stood would move each pooled slot's home off the parking spot, so the pool's parking guard would refuse
    /// the next pool until OpenSim restarted. LLAgent::setStartPosition's request with the spot's x and y and, as there,
    /// the avatar's feet for the height; the region confirms it (homeNotConfirmed). Alone the avatar stands
    /// on the spot, so the home is what setStartPosition asked for. The line names the avatar's distance from the
    /// spot, which level2.py's parking guard checks in the pool.
    bool askForHomeOnSpot()
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            fail("the home: the avatar has no region");
            return false;
        }
        const LLVector3 spot = vector3(sState.config["avatar"]);
        LLVector3 position = gAgent.getPositionAgent() - region->getOriginAgent();
        if (isAgentAvatarValid())
        {
            // The feet, as setStartPosition has them.
            position.mV[VZ] -= 0.5f * (gAgentAvatarp->mBodySize.mV[VZ] + gAgentAvatarp->mAvatarOffset.mV[VZ]);
        }
        const F32 off = dist_vec(LLVector3(position.mV[VX], position.mV[VY], 0.f),
                                 LLVector3(spot.mV[VX], spot.mV[VY], 0.f));
        position.mV[VX] = spot.mV[VX];
        position.mV[VY] = spot.mV[VY];
        position.mV[VZ] = llclamp(position.mV[VZ], region->getLandHeightRegion(position),
                                  LLWorld::getInstance()->getRegionMaxHeight());
        // [Viewer Lab] In a tour's later stop the first stop's home, never the stop's own (ll_golden_home_to_ask):
        // the tour then leaves the home as a fresh run of its first scene does, wherever it ends.
        LLGoldenHome own;
        own.position = position;
        own.look_at = gAgent.getAtAxis();
        own.set = true;
        const LLGoldenHome asked = ll_golden_home_to_ask(laterTourStop(), own, sTour.anchorHome);
        if (!asked.set)
        {
            fail("the home: tour stop " + tourStopName() + " has no home of the tour's first stop to ask for");
            return false;
        }
        if (sTour.active() && sTour.stop == 0)
        {
            sTour.anchorHome = own;
        }
        // As llagent.cpp's ll_sdmap_from_vector3, which no header declares.
        auto map = [](const LLVector3& v) { return llsd::map("X", v.mV[VX], "Y", v.mV[VY], "Z", v.mV[VZ]); };
        LLSD home;
        home["LocationId"] = LLSD::Integer(START_LOCATION_ID_HOME);
        home["LocationPos"] = map(asked.position);
        home["LocationLookAt"] = map(asked.look_at);
        LLSD body;
        body["HomeLocation"] = home;
        if (laterTourStop())
        {
            LL_INFOS("Golden") << "Golden test: the home is asked for as the tour's first stop asked for it, "
                               << asked.position << " looking along " << asked.look_at << " (the stop's own: "
                               << position << " along " << own.look_at << "; the avatar stands "
                               << llformat("%.3f", off) << " m across the ground from the spot)" << LL_ENDL;
        }
        else
        {
            LL_INFOS("Golden") << "Golden test: the home is asked for on the scene's avatar spot " << position
                               << "; the avatar stands " << llformat("%.3f", off) << " m across the ground from it"
                               << LL_ENDL;
        }
        const U64 handle = region->getHandle();
        // The viewer's own record of the home, as setStartPositionSuccess keeps it.
        auto confirmed = [handle](const LLSD& result)
        {
            const LLSD& pos = result["HomeLocation"]["LocationPos"];
            if (result["success"].asString().rfind("true", 0) == 0 && pos.has("X") && pos.has("Y") && pos.has("Z"))
            {
                gAgent.setHomePosRegion(handle, LLVector3((F32)pos["X"].asInteger(), (F32)pos["Y"].asInteger(),
                                                          (F32)pos["Z"].asInteger()));
            }
        };
        if (!gAgent.requestPostCapability("HomeLocation", body, confirmed))
        {
            fail("the home: the region has no HomeLocation capability");
            return false;
        }
        return true;
    }

    /// The region confirms a new home with the alert "Home position set.", and the viewer then saves screen_home with
    /// saveSnapshot, a frame drawn outside the main loop. Where that frame fell, before or after the probes started over,
    /// followed the network and changed midhigh/overview's snapshot by 49 px: so the home is asked for when the
    /// probes start over, and the scene is ready only once that snapshot is saved (its file is newer than the request).
    bool homeNotConfirmed()
    {
        if (sState.homeAsked == 0)
        {
            return false;
        }
        const std::string path = gDirUtilp->getLindenUserDir() + gDirUtilp->getDirDelimiter() +
                                 LLStartUp::getScreenHomeFilename();
        llstat st;
        if (LLFile::stat(path, &st) == 0 && st.st_mtime >= sState.homeAsked)
        {
            LL_INFOS("Golden") << "Golden test: the region confirmed the home position" << LL_ENDL;
            sState.homeAsked = 0;
            return false;
        }
        return true;
    }

    /// The own avatar's local composites (LLViewerTexLayerSetBuffer, rendered by LLViewerDynamicTexture::updateAllInstances
    /// with gAlphaMaskProgram) have rendered their final textures, so no composite renders in the warm-up.
    /// Until every local texture is at its final discard level, a composite renders again when one arrives or on its
    /// 10 s timer (llviewertexlayer.cpp, isReadyToUpdate). Such a render in the warm-up is a draw the frame does not have,
    /// and on Metal it asked the device for a pipeline after the first warm-up frame when a scene without reflection
    /// probes was ready about a second after its assets. Asked after the probes, so a scene already settled is ready on
    /// the same frame as without this wait. A missing asset counts as final (getLocalDiscardLevel). The rebake scene's forced bake
    /// comes after this, at the warm-up's start. The wait gives up after AVATAR_WAIT_FRAMES in a row and logs it, so a
    /// local texture that never gets final costs those frames, not the load timeout's failure; a composite in a midhigh
    /// warm-up then fails level2's warm-up ceiling on 'Alpha Mask Shader'.
    std::string ownAvatarNotSettled()
    {
        if (!isAgentAvatarValid() || sState.avatarGaveUp)
        {
            return std::string();
        }
        std::string why;
        if (!gAgentAvatarp->isAllLocalTextureDataFinal())
        {
            why = "the own avatar's local textures are not final, so its composites render again";
        }
        for (S32 baked = 0; why.empty() && baked < LLAvatarAppearanceDefines::BAKED_NUM_INDICES; ++baked)
        {
            LLViewerTexLayerSet* layers = gAgentAvatarp->getLayerSet(LLAvatarAppearanceDefines::EBakedTextureIndex(baked));
            if (layers && layers->hasComposite() && layers->getViewerComposite()->needsRender())
            {
                why = "the own avatar's composite " + std::to_string(baked) + " is to render";
            }
        }
        if (why.empty() && sState.avatarWaited > 0)
        {
            LL_INFOS("Golden") << "Golden test: the own avatar's composites settled after " << sState.avatarWaited
                               << " frames of waiting" << LL_ENDL;
            sState.avatarWaited = 0;
        }
        else if (!why.empty() && sState.avatarWaited >= AVATAR_WAIT_FRAMES)
        {
            LL_WARNS("Golden") << "Golden test: gave up waiting for the own avatar after " << sState.avatarWaited
                               << " frames: " << why << LL_ENDL;
            sState.avatarGaveUp = true;
            return std::string();
        }
        else if (!why.empty() && sState.avatarWaited++ == 0)
        {
            LL_INFOS("Golden") << "Golden test: waiting for the own avatar: " << why << LL_ENDL;
        }
        return why;
    }

    /// The assets, then every reflection probe updated since the assets were ready, so no
    /// probe shows the scene half loaded. The selection settles first, like the assets, and the glTF run's
    /// GLTFEnabled and host.
    std::string notReady()
    {
        std::string assets = selectionNotReady();
        if (assets.empty())
        {
            assets = gltfNotReady();
        }
        if (assets.empty())
        {
            assets = localLightsNotReady();
        }
        if (assets.empty())
        {
            assets = assetsNotReady();
        }
        if (!assets.empty())
        {
            sState.probesSince = -1.f;
            sState.probesBegun.clear();
            sState.probesFinished.clear();
            return assets;
        }
        // [Viewer Lab] With the probes off nothing updates or draws them, so a later tour stop does not wait for them
        // (only there: a fresh run always waits, and with them off it has none).
        const bool probes_awaited = ll_golden_probes_awaited(laterTourStop(), probeStatus());
        if (sState.probesSince < 0.f)
        {
            // Probes take their slots in the cube map array as they appear, nearest first, so the
            // slots follow the network and ties in distance from where the avatar logged in, and a
            // pixel blends the probes by slot. With everything else in, every probe starts over and
            // takes its slot by its distance from the test camera on the next update. From here on
            // the test clock runs: which probe updates next follows the frame times of the
            // updates before, and a probe renders the others into its own.
            // [Viewer Lab] updateCamera's smoothing moves the eye to the test camera over several
            // frames, from the follow camera, which follows where and how the avatar logged in. A
            // run whose assets are ready in setCamera's first frames would fail the
            // tie check below on that path. So the probes start over only once the
            // eye has stopped moving. In a later tour stop it must have stopped at the test camera:
            // the first frame after setCamera never moves the eye (idle() skips updateCamera until
            // the local teleport's display ends, ll_golden_eye_settled), which there still stands at
            // the stop before's camera. A fresh run keeps the rule of a run outside a
            // tour, so its frames and the GL references do not change; a race at the follow
            // camera can still fail such a run. With the probes off, nothing uses their slots.
            if (!sState.eyeSettled)
            {
                return "the camera is still moving to the test camera";
            }
            if (probes_awaited && !probeDistancesDiffer())
            {
                return "two probes are equally far from the camera";
            }
            // The terrain octree as one load makes it, before the probes render the terrain.
            if (!settleTerrain())
            {
                return "failed";
            }
            gPipeline.mReflectionMapManager.restart();
            LLFrameTimer::setFixedFrameStep(FRAME_STEP);
            sState.probesSince = gFrameTimeSeconds;
            sState.probeResets = gPipeline.mReflectionMapManager.getResetCount();
            sState.ownReset = true;
            LL_INFOS("Golden") << "Golden test: the assets are ready; the reflection probes start over on the test clock"
                               << LL_ENDL;
            if (!probes_awaited)
            {
                LL_INFOS("Golden") << "Golden test: the reflection probes are off; the scene does not wait for them"
                                   << LL_ENDL;
            }
            if (laterTourStop() && sState.eyeFromCamera > LL_GOLDEN_EYE_AT_CAMERA_DISTANCE)
            {
                LL_INFOS("Golden") << "Golden test: the eye stopped " << sState.eyeFromCamera
                                   << " m from the test camera's position (the viewer keeps it there)" << LL_ENDL;
            }
            if (sState.setHome)
            {
                sState.setHome = false;
                sState.homeAsked = time(nullptr);
                if (!askForHomeOnSpot())
                {
                    return "failed";
                }
            }
        }
        // The manager also resets every probe by itself (environment and settings changes, the
        // probe count): then the counting starts over.
        if (gPipeline.mReflectionMapManager.getResetCount() != sState.probeResets)
        {
            if (!sState.ownReset)
            {
                LL_INFOS("Golden") << "Golden test: the reflection probes were reset; counting their updates again"
                                   << LL_ENDL;
            }
            sState.ownReset = false;
            sState.probeResets = gPipeline.mReflectionMapManager.getResetCount();
            sState.probesSince = gFrameTimeSeconds;
            sState.probesBegun.clear();
            sState.probesFinished.clear();
        }
        // Every frame, so that no update's beginning is missed.
        const std::string probes = probes_awaited
                                       ? gPipeline.mReflectionMapManager.probesNotComplete(sState.probesSince,
                                                                                           PROBE_UPDATES,
                                                                                           sState.probesBegun,
                                                                                           sState.probesFinished)
                                       : std::string();
        if (probes.empty() && homeNotConfirmed())
        {
            return "the region has not confirmed the home position";
        }
        if (probes.empty())
        {
            return ownAvatarNotSettled();
        }
        return probes;
    }

    /// No UI or HUDs, kept so every frame: the viewer toggles the UI flag off and on again in
    /// pairs (HUD rendering, snapshots), which can turn it back on. With particles off in the
    /// config, no particles either: with RenderMaxPartCount 0 LLViewerPartSim::shouldAddPart
    /// still adds one whenever there is none, and where it drifts moves the sun's shadow frusta,
    /// which are fitted to what is visible.
    void hideUI()
    {
        if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
        {
            LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        }
        LLPipeline::sShowHUDAttachments = false;
        if (sState.config.has("particles") && !sState.config["particles"].asBoolean()
            && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_PARTICLES))
        {
            LLPipeline::toggleRenderType(LLPipeline::RENDER_TYPE_PARTICLES);
        }
    }

    /// The UI kept in the snapshot (show_ui), so every frame: the UI flag on, which the viewer toggles in
    /// pairs, and as hideUI no HUD attachments and, with particles off in the config, no particles. No avatar is
    /// drawn, so its pose, which follows the wall clock from the login, is never in the picture; its name tag is
    /// (LLVOAvatar::idleUpdate updates the own avatar's tag whatever is drawn). The status bar's clock is hidden, and
    /// the toast channels are muted, so no toast whose life follows the wall clock comes up.
    void showUI()
    {
        if (!gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
        {
            LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        }
        LLPipeline::sShowHUDAttachments = false;
        if (sState.config.has("particles") && !sState.config["particles"].asBoolean()
            && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_PARTICLES))
        {
            LLPipeline::toggleRenderType(LLPipeline::RENDER_TYPE_PARTICLES);
        }
        if (gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_AVATAR))
        {
            LLPipeline::toggleRenderType(LLPipeline::RENDER_TYPE_AVATAR);
        }
        if (gStatusBar)
        {
            if (LLView* clock = gStatusBar->findChildView("TimeText"))
            {
                clock->setVisible(false);
            }
        }
        LLNotificationsUI::LLChannelManager::getInstance()->muteAllChannels(true);
    }

    /// The config's debug displays, so every frame: render_debug's from the first in-world frame on,
    /// render_debug_late's from the warm-up's start on. Only those that are off are turned on; nothing turns them off.
    void keepRenderDebug()
    {
        U64 mask = sState.renderDebug;
        if (sState.phase == Phase::Warming || sState.phase == Phase::Previews || sState.phase == Phase::Media)
        {
            mask |= sState.renderDebugLate;
        }
        for (U64 bit = 1; bit != 0 && bit <= mask; bit <<= 1)
        {
            if ((mask & bit) && !gPipeline.hasRenderDebugMask(bit))
            {
                LLPipeline::toggleRenderDebug(bit);
            }
        }
    }

    /// The UI and the debug displays as the config wants them in every frame of the run's in-world phases, and the
    /// config's wireframe, which only turns it on, as the menu would.
    void keepUI()
    {
        if (sState.config["show_ui"].asBoolean())
        {
            showUI();
        }
        else
        {
            hideUI();
        }
        keepRenderDebug();
        if (sState.config["wireframe"].asBoolean())
        {
            gUseWireframe = true;
        }
    }

    /// The config's floaters (show_ui), opened without focus in the config's order at their rects. False after a
    /// failure.
    bool openFloaters()
    {
        for (const LLSD& entry : llsd::inArray(sState.config["floaters"]))
        {
            const std::string name = entry["name"].asString();
            const LLSD& r = entry["rect"];
            const LLRect rect(r[0].asInteger(), r[1].asInteger(), r[2].asInteger(), r[3].asInteger());
            LLFloater* floater = LLFloaterReg::showInstance(name, LLSD(), false);
            if (!floater)
            {
                fail("there is no floater " + name);
                return false;
            }
            // As a user's move, so the floater keeps its place when the floater view reshapes (a floater that
            // positions itself centered, like test_widgets, would centre itself again).
            floater->setShape(rect, true);
            LL_INFOS("Golden") << "Golden test: floater " << name << " at " << floater->getRect() << LL_ENDL;
            for (const LLSD& child : llsd::inArray(entry["hide"]))
            {
                LLView* view = floater->findChildView(child.asString());
                if (!view)
                {
                    fail("the floater " + name + " has no " + child.asString() + " to hide");
                    return false;
                }
                view->setVisible(false);
                LL_INFOS("Golden") << "Golden test: " << child.asString() << " hidden in " << name << LL_ENDL;
            }
        }
        // The snapshot floater at its own size, its top left at the config's point, as a user's move.
        if (const LLSD& at = sState.config["snapshot_floater"]; at.isArray())
        {
            LLFloaterSnapshot* floater = LLFloaterReg::showTypedInstance<LLFloaterSnapshot>("snapshot", LLSD(), false);
            if (!floater)
            {
                fail("there is no snapshot floater");
                return false;
            }
            const LLRect size = floater->getRect();
            floater->setShape(LLRect(at[0].asInteger(), at[1].asInteger(), at[0].asInteger() + size.getWidth(),
                                     at[1].asInteger() - size.getHeight()),
                              true);
            LL_INFOS("Golden") << "Golden test: floater snapshot at " << floater->getRect() << LL_ENDL;
        }
        return true;
    }

    /// LLViewerDynamicTexture's render count and instances, which only a subclass may read; never made (rebake).
    struct DynamicTextureCounts : public LLViewerDynamicTexture
    {
        DynamicTextureCounts() = delete;
        /// The renders since the last call, which zeroes them: updateAllInstances counts its renders from 0 and nothing
        /// else reads the count, so a frame without an updateAllInstances gives 0, not the frame before's.
        static S32 takeRenders()
        {
            const S32 renders = sNumRenders;
            sNumRenders = 0;
            return renders;
        }
        /// The preview textures, ORDER_FIRST and ORDER_MIDDLE, whose renders the count would mix with the bakes'.
        static S32 previews() { return S32(sInstances[ORDER_FIRST].size() + sInstances[ORDER_MIDDLE].size()); }
    };

    /// At the warm-up's start: the scene's one light selected with the basic toolset's translate tool
    /// (select_light), and a new snapshot of the snapshot floater's preview without the floater's delay
    /// (snapshot_floater), which its idle takes in the warm-up's first frame. False after a failure.
    bool startWarmupUI()
    {
        if (sState.config["select_light"].asBoolean())
        {
            LLVOVolume* light = nullptr;
            S32 lights = 0;
            for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
            {
                LLVOVolume* volume = dynamic_cast<LLVOVolume*>(gObjectList.getObject(i));
                if (volume && !volume->isDead() && volume->getIsLight())
                {
                    light = volume;
                    ++lights;
                }
            }
            if (lights != 1)
            {
                fail(llformat("select_light: the scene has %d lights, not 1", lights));
                return false;
            }
            gBasicToolset->selectTool(LLToolCompTranslate::getInstance());
            LLSelectMgr::getInstance()->selectObjectAndFamily(light);
            if (!LLToolMgr::getInstance()->inEdit() || LLSelectMgr::getInstance()->getSelection()->getObjectCount() == 0)
            {
                fail("select_light: the light is not selected with an edit tool");
                return false;
            }
            // renderSelections draws the radius sphere, with its two cull faces, only with RenderLightRadius.
            if (!LLSelectMgr::sRenderLightRadius)
            {
                fail("select_light: RenderLightRadius is off");
                return false;
            }
            LL_INFOS("Golden") << "Golden test: light " << light->getID() << " selected, radius " << light->getLightRadius()
                               << ", RenderLightRadius " << LLSelectMgr::sRenderLightRadius << LL_ENDL;
        }
        if (sState.config["snapshot_floater"].isArray())
        {
            LLFloaterSnapshot* floater = LLFloaterSnapshot::findInstance();
            LLSnapshotLivePreview* preview = floater ? floater->getPreviewView() : nullptr;
            if (!preview)
            {
                fail("snapshot_floater: the snapshot floater or its preview is gone");
                return false;
            }
            preview->updateSnapshot(true, true, 0.f);
            // Not "Golden test: snapshot ", which run-local.sh and level2.py take for the run's snapshot.
            LL_INFOS("Golden") << "Golden test: the snapshot floater's preview asked for at frame "
                               << LLFrameTimer::getFrameCount() << LL_ENDL;
        }
        if (const LLSD& hud = sState.config["hud_text"]; hud.isMap())
        {
            // A beacon's hover text (no source object): at a fixed region point, so it holds still.
            LLViewerRegion* region = gAgent.getRegion();
            LLHUDText* text = region ? (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT) : nullptr;
            if (!text)
            {
                fail("hud_text: no region or no HUD text object");
                return false;
            }
            text->setFont(LLFontGL::getFontSansSerif());
            text->setColor(LLColor4::white);
            text->setDoFade(false);
            text->setZCompare(false);
            text->setString(hud["text"].asString());
            text->setPositionGlobal(region->getPosGlobalFromRegion(ll_vector3_from_sd(hud["region"])));
            LL_INFOS("Golden") << "Golden test: HUD text placed at region " << ll_vector3_from_sd(hud["region"]) << LL_ENDL;
        }
        return true;
    }

    /// Where the mouse is over the window, whose hover would highlight and show tooltips, or empty when it is outside.
    std::string mouseOverWindow()
    {
        LLCoordWindow mouse;
        if (gViewerWindow->getWindow()->getCursorPosition(&mouse) && mouse.mX >= 0 && mouse.mY >= 0
            && mouse.mX <= gViewerWindow->getWindowWidthRaw() && mouse.mY <= gViewerWindow->getWindowHeightRaw())
        {
            return llformat("the mouse is over the window at %d, %d; the run needs it outside", mouse.mX, mouse.mY);
        }
        return {};
    }

    /// Whether the selection holds a light renderSelections draws the radius sphere of (select_light).
    bool selectionHoldsLight()
    {
        struct FindLight : public LLSelectedObjectFunctor
        {
            bool found = false;
            bool apply(LLViewerObject* object) override
            {
                found = found || (object && object->mDrawable && object->mDrawable->isLight());
                return true;
            }
        } find;
        LLSelectMgr::getInstance()->getSelection()->applyToObjects(&find);
        return find.found;
    }

    /// What breaks the config's UI at the snapshot (show_ui): a floater not open at its rect, the selected light
    /// renderSelections no longer draws the radius sphere of, or the cull face that sphere left (select_light), or the
    /// mouse over the window. Empty when nothing does.
    std::string uiNotAsConfigured()
    {
        for (const LLSD& entry : llsd::inArray(sState.config["floaters"]))
        {
            const std::string name = entry["name"].asString();
            const LLSD& r = entry["rect"];
            const LLRect rect(r[0].asInteger(), r[1].asInteger(), r[2].asInteger(), r[3].asInteger());
            LLFloater* floater = LLFloaterReg::findInstance(name);
            if (!floater || !floater->getVisible() || floater->getRect() != rect)
            {
                std::ostringstream why;
                why << "the floater " << name << " is ";
                if (floater && floater->getVisible())
                {
                    why << "at " << floater->getRect() << ", not " << rect;
                }
                else
                {
                    why << "not open";
                }
                return why.str();
            }
        }
        if (sState.config["snapshot_floater"].isArray())
        {
            LLFloaterSnapshot* floater = LLFloaterSnapshot::findInstance();
            LLSnapshotLivePreview* preview = floater ? floater->getPreviewView() : nullptr;
            if (!floater || !floater->getVisible() || !preview || !preview->getSnapshotUpToDate()
                || !preview->getThumbnailImage())
            {
                return "the snapshot floater is not open with its snapshot and thumbnail";
            }
        }
        // The light's radius sphere is still drawn at the snapshot, and its inside's GL_FRONT is back to GL_BACK:
        // the image cannot tell the two cull faces, since gSphere covers the sphere twice.
        if (sState.config["select_light"].asBoolean())
        {
            if (!LLSelectMgr::sRenderLightRadius || !LLToolMgr::getInstance()->inEdit() || !selectionHoldsLight())
            {
                return llformat("select_light: renderSelections no longer draws the light's radius (RenderLightRadius %d, "
                                "an edit tool %d, a light selected %d)",
                                (S32)LLSelectMgr::sRenderLightRadius, (S32)LLToolMgr::getInstance()->inEdit(),
                                (S32)selectionHoldsLight());
            }
            if (gGL.getCullFace() != GL_BACK)
            {
                return llformat("select_light: the cull face is 0x%x at the snapshot, not GL_BACK", gGL.getCullFace());
            }
        }
        return mouseOverWindow();
    }

    /// The window's pixels per point of the config's window size: on macOS at RenderHiDPI 1 the backing's
    /// (getSystemUISize: 2 on a Retina display, 1 on a 1x one), else 1, where a point is a pixel.
    F32 windowPixelsPerPoint()
    {
#if LL_DARWIN
        return gHiDPISupport ? gViewerWindow->getWindow()->getSystemUISize() : 1.f;
#else
        return 1.f;
#endif
    }

    /// The window's content in pixels for the config's window, which is in points.
    std::pair<S32, S32> windowPixels()
    {
        const LLSD& window = sState.config["window"];
        const F32 scale = windowPixelsPerPoint();
        return { ll_round(F32(window[0].asInteger()) * scale), ll_round(F32(window[1].asInteger()) * scale) };
    }

    /// The window's content at the config's size, whatever size the window system gave it.
    /// LLViewerWindow::movieSize misses by the title bar's height on macOS, so the size is
    /// measured and the request corrected. True when it fits. The window's size is measured in
    /// pixels, while the request is in points (on macOS setSize sizes the window's frame in points),
    /// so a correction is the difference in pixels over the pixels per point.
    bool sizeWindow()
    {
        const LLSD& window = sState.config["window"];
        const auto [width, height] = windowPixels();
        const F32 scale = windowPixelsPerPoint();
        const S32 actual_width = gViewerWindow->getWindowWidthRaw();
        const S32 actual_height = gViewerWindow->getWindowHeightRaw();
        if (actual_width == width && actual_height == height)
        {
            return true;
        }
        if (sState.tries++ >= SIZING_TRIES)
        {
            fail(llformat("the window stays %dx%d, not %dx%d", actual_width, actual_height, width, height));
            return false;
        }
        sState.requestWidth = sState.tries == 1 ? window[0].asInteger()
                                                : sState.requestWidth + ll_round(F32(width - actual_width) / scale);
        sState.requestHeight = sState.tries == 1 ? window[1].asInteger()
                                                 : sState.requestHeight + ll_round(F32(height - actual_height) / scale);
        LLViewerWindow::movieSize(sState.requestWidth, sState.requestHeight);
        sState.frames = 0;
        return false;
    }

    /// The scene without UI, HUDs or the avatar in view.
    void startScene()
    {
        LLViewerRegion* region = gAgent.getRegion();
        const LLVector3 avatar = vector3(sState.config["avatar"]);
        LL_INFOS("Golden") << "Golden test: moving the avatar to " << avatar << LL_ENDL;
        gAgent.teleportViaLocation(region->getPosGlobalFromRegion(avatar));
        sState.phase = Phase::Moving;
        sState.frames = 0;
    }

    bool avatarArrived()
    {
        LLViewerRegion* region = gAgent.getRegion();
        const LLVector3 target = vector3(sState.config["avatar"]);
        if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || !region)
        {
            return false;
        }
        // Across the ground only: the avatar lands at its own height above the terrain.
        LLVector3 position = gAgent.getPositionAgent() - region->getOriginAgent();
        position.mV[VZ] = target.mV[VZ];
        return dist_vec(position, target) < ARRIVED_DISTANCE;
    }

    void setCamera()
    {
        LLViewerRegion* region = gAgent.getRegion();
        const LLSD& camera = sState.config["camera"];
        gAgentCamera.setFocusOnAvatar(false, false);
        // [Viewer Lab] The position is kept for the eye's wait below.
        sState.cameraEye = region->getPosGlobalFromRegion(vector3(camera["position"]));
        gAgentCamera.setCameraPosAndFocusGlobal(sState.cameraEye,
                                                region->getPosGlobalFromRegion(vector3(camera["focus"])), LLUUID::null);
        // At once: the transition runs on the wall clock, not on frames.
        gAgentCamera.stopCameraAnimation();
        // [Viewer Lab] The eye still moves there over the next frames (updateCamera's smoothing): notReady waits until
        // it is still, in a later tour stop at the position (ll_golden_eye_settled).
        sState.lastEye = LLViewerCamera::getInstance()->getOrigin();
        sState.eyeStillFrames = 0;
        sState.eyeSettled = false;
        LL_INFOS("Golden") << "Golden test: camera at " << vector3(camera["position"]) << " looking at "
                           << vector3(camera["focus"]) << LL_ENDL;
    }

    /// The back buffer, which holds the frame about to be presented, read through the viewer's
    /// render interface device, as RGB, since its alpha is not part of the picture;
    /// rows from the bottom, as GL has them.
    bool readFrame(LLPointer<LLImageRaw>& frame, std::string& why)
    {
        const S32 width = gViewerWindow->getWindowWidthRaw();
        const S32 height = gViewerWindow->getWindowHeightRaw();
        const auto [wanted_width, wanted_height] = windowPixels();
        if (width != wanted_width || height != wanted_height)
        {
            why = llformat("the window is %dx%d, not %dx%d", width, height, wanted_width, wanted_height);
            return false;
        }
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        if (!device || !device->surface())
        {
            why = "no render interface device for the window";
            return false;
        }
        // The surface follows the window (LLViewerWindow::reshape).
        const LL::RHI::TextureDesc& back = device->surface()->backBuffer()->desc();
        if (back.width != U32(width) || back.height != U32(height))
        {
            why = llformat("the device's surface is %ux%u, not the window's %dx%d", back.width, back.height, width, height);
            return false;
        }
        LL::RHI::Ref<LL::RHI::Readback> readback = device->encoder().readback(
            device->surface()->backBuffer(), 0, 0, { 0, 0, U32(width), U32(height) }, LL::RHI::PixelLayout());
        // Metal's readback is asynchronous (GL's is done at once), and its rows may have a pitch, as the Metal login
        // hook's readback reads them.
        if (readback.notNull() && !readback->ready())
        {
            readback->wait();
        }
        const size_t row = size_t(width) * 4;
        const size_t pitch = readback.notNull() && readback->rowPitch() ? readback->rowPitch() : row;
        if (readback.isNull() || pitch < row || readback->data().size() < pitch * size_t(height - 1) + row)
        {
            why = "the readback of the window failed";
            return false;
        }
        LLPointer<LLImageRaw> raw = new LLImageRaw(U16(width), U16(height), 3);
        const std::byte* rgba = readback->data().data();
        U8* rgb = raw->getData();
        for (size_t pixel = 0; pixel < size_t(width) * size_t(height); ++pixel)
        {
            memcpy(rgb + pixel * 3, rgba + (pixel / size_t(width)) * pitch + (pixel % size_t(width)) * 4, 3);
        }
        frame = raw;
        return true;
    }

    /// The frame about to be presented (readFrame), saved as a PNG.
    bool saveFrame(const std::string& path, std::string& why, LLPointer<LLImageRaw>& frame)
    {
        LLPointer<LLImageRaw> raw;
        if (!readFrame(raw, why))
        {
            return false;
        }
        LLPointer<LLImagePNG> png = new LLImagePNG();
        if (!png->encode(raw, 0.f) || !png->save(path))
        {
            why = "could not write " + path;
            return false;
        }
        LL_INFOS("Golden") << "Golden test: snapshot " << path << " (" << raw->getWidth() << "x" << raw->getHeight() << ")"
                           << LL_ENDL;
        frame = raw;
        return true;
    }

    /// Counts a partition's groups, and those occlusion culling hides from the world camera: the
    /// spatial groups of the objects in the scene, or the groups of the region's object cache.
    class CountGroups : public OctreeTraveler
    {
    public:
        S32 mGroups = 0;
        S32 mOccluded = 0;

        void visit(const OctreeNode* node) override
        {
            const LLOcclusionCullingGroup* group = (const LLOcclusionCullingGroup*)node->getListener(0);
            if (group)
            {
                ++mGroups;
                if (group->isOcclusionState(LLOcclusionCullingGroup::OCCLUDED))
                {
                    ++mOccluded;
                }
            }
        }
    };

    LLSD pair(S32 x, S32 y)
    {
        return llsd::array(x, y);
    }

    /// What the frame was rendered with: the GPU, graphics level and class, every Render
    /// setting and every setting in the feature table, the features the table allows and
    /// recommends, each shader's level (-1 when not loaded), the pipeline's own copies of its
    /// switches, the window and framebuffer, and under "measured" what the run itself saw.
    /// "system" holds what the references record but a comparison need not match: the macOS
    /// version and the driver.
    /// LLVertexBuffer's counts by their names in the render profile.
    struct DrawCount
    {
        const char* name;
        U64 LLVertexBuffer::DrawCounts::*field;
    };
    const DrawCount DRAW_COUNTS[] = {
        { "draws", &LLVertexBuffer::DrawCounts::draws },
        { "with_shader", &LLVertexBuffer::DrawCounts::drawsWithShader },
        { "with_storage", &LLVertexBuffer::DrawCounts::drawsWithStorage },
        { "with_program", &LLVertexBuffer::DrawCounts::drawsWithProgram },
        { "set_buffers", &LLVertexBuffer::DrawCounts::setBuffers },
        { "missing_attributes", &LLVertexBuffer::DrawCounts::missingAttributes },
        { "streams_beyond_buffer", &LLVertexBuffer::DrawCounts::streamsBeyondBuffer },
        { "indices_beyond_buffer", &LLVertexBuffer::DrawCounts::indicesBeyondBuffer },
        { "skipped", &LLVertexBuffer::DrawCounts::skipped },
        { "encoded", &LLVertexBuffer::DrawCounts::encoded },
        { "passless", &LLVertexBuffer::DrawCounts::passless },
        { "without_device", &LLVertexBuffer::DrawCounts::withoutDevice },
        { "without_program", &LLVertexBuffer::DrawCounts::withoutProgram },
        { "without_storage", &LLVertexBuffer::DrawCounts::withoutStorage },
        { "without_pipeline", &LLVertexBuffer::DrawCounts::withoutPipeline },
        { "passes_with_draws", &LLVertexBuffer::DrawCounts::passesWithDraws },
        { "format_changes", &LLVertexBuffer::DrawCounts::formatChanges },
        { "table_lookups", &LLVertexBuffer::DrawCounts::tableLookups },
        { "pipeline_requests", &LLVertexBuffer::DrawCounts::pipelineRequests },
        { "depth_requests", &LLVertexBuffer::DrawCounts::depthRequests },
        { "attribute_mask_mismatches", &LLVertexBuffer::DrawCounts::attributeMaskMismatches },
        { "depth_writes_in_read_only_pass", &LLVertexBuffer::DrawCounts::depthWritesInReadOnlyPass },
        { "draws_in_read_only_pass", &LLVertexBuffer::DrawCounts::drawsInReadOnlyPass },
        { "pipeline_sets", &LLVertexBuffer::DrawCounts::pipelineSets },
        { "depth_sets", &LLVertexBuffer::DrawCounts::depthSets },
        { "vertex_buffer_sets", &LLVertexBuffer::DrawCounts::vertexBufferSets },
        { "index_buffer_sets", &LLVertexBuffer::DrawCounts::indexBufferSets },
    };

    /// The counts whose mean and most per warm-up frame after the first the render profile also has: the device's
    /// requests, which the tables keep to the first frames.
    const DrawCount DRAW_REQUESTS[] = {
        { "pipeline_requests", &LLVertexBuffer::DrawCounts::pipelineRequests },
        { "depth_requests", &LLVertexBuffer::DrawCounts::depthRequests },
    };

    /// LLRender's dynamic state counts by their names in the render profile.
    struct DynamicStateCount
    {
        const char* name;
        U64 LLRender::DynamicStateCounts::*field;
    };
    const DynamicStateCount DYNAMIC_STATE_COUNTS[] = {
        { "sends", &LLRender::DynamicStateCounts::sends },
        { "pass_sets", &LLRender::DynamicStateCounts::passSets },
        { "pass_set_calls", &LLRender::DynamicStateCounts::passSetCalls },
        { "unclassified_enables", &LLRender::DynamicStateCounts::unclassifiedEnables },
    };

    /// The Metal encoder's type slot reads so far (MetalEncoderStats::typeSlotReads); 0 without a Metal
    /// device.
    U64 metalTypeSlotReads()
    {
#if LL_RHI_METAL
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        return device ? LL::RHI::metalEncoderStats(*device).typeSlotReads : 0;
#else
        return 0;
#endif
    }

    /// The Metal encoder's depth bias changes so far (MetalEncoderStats::depthBiasChanges); 0 without a
    /// Metal device.
    U64 metalDepthBiasChanges()
    {
#if LL_RHI_METAL
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        return device ? LL::RHI::metalEncoderStats(*device).depthBiasChanges : 0;
#else
        return 0;
#endif
    }

    /// The Metal device's compilations since it was made (MetalCompileStats), as measured.rhi.compile:
    /// the counts, the failed programs and the Safe math list, and the calling threads' waits in ms, which are
    /// information (validation, a warm OS cache). Empty without Metal.
    LLSD metalCompileMeasures(LL::RHI::Device& device)
    {
        LLSD compile;
#if LL_RHI_METAL
        const LL::RHI::MetalCompileStats stats = LL::RHI::metalCompileStats(device);
        compile["full"] = (S32)stats.full;
        compile["alpha_variants"] = (S32)stats.alphaVariants;
        compile["refused"] = (S32)stats.refused;
        compile["cached"] = (S32)stats.cached;
        compile["caller_translations"] = (S32)stats.callerTranslations;
        // The failed program jobs, which level 2 holds to 0, and the Safe math programs it
        // holds to ultra's GL render profile, read here rather than from single log lines
        compile["failed_programs"] = (S32)stats.failedPrograms;
        // The creations the test faults failed, which failed_programs leaves out, and the Pending answers of
        // the deferred ones; level 2 holds them to the faults' hits
        compile["faulted_programs"] = (S32)stats.faultedPrograms;
        compile["fault_pending"] = (S32)stats.faultPending;
        compile["safe_math_programs"] = LLSD::emptyArray();
        for (const std::string& name : LL::RHI::metalSafeMathPrograms())
        {
            compile["safe_math_programs"].append(name);
        }
        compile["ms"] = llsd::map("caller_translation", stats.callerTranslationUs / 1000.0, "program_wait",
                                  stats.programWaitUs / 1000.0, "pipeline_wait", stats.pipelineWaitUs / 1000.0,
                                  "point_wait", stats.pointWaitUs / 1000.0);
#endif
        return compile;
    }

    /// LLTexUnit's counts by their names in the render profile.
    struct TextureCount
    {
        const char* name;
        U64 LLTexUnit::BindCounts::*field;
    };
    const TextureCount TEXTURE_COUNTS[] = {
        { "calls", &LLTexUnit::BindCounts::calls },
        { "binds", &LLTexUnit::BindCounts::binds },
        { "skipped", &LLTexUnit::BindCounts::skipped },
        { "refused", &LLTexUnit::BindCounts::refused },
        { "nested", &LLTexUnit::BindCounts::nested },
        { "adopted_new_id", &LLTexUnit::BindCounts::adoptedNewId },
        { "adopted_after_raw", &LLTexUnit::BindCounts::adoptedAfterRaw },
        { "raw_binds", &LLTexUnit::BindCounts::rawBinds },
        { "fast_repeats", &LLTexUnit::BindCounts::fastRepeats },
        { "retold", &LLTexUnit::BindCounts::retold },
        { "sampling_sets", &LLTexUnit::BindCounts::samplingSets },
        { "sampling_untracked", &LLTexUnit::BindCounts::samplingUntracked },
        { "unknown_filter_parts", &LLTexUnit::BindCounts::unknownFilterParts },
        { "unknown_address_parts", &LLTexUnit::BindCounts::unknownAddressParts },
        { "unbinds_to_white", &LLTexUnit::BindCounts::unbindsToWhite },
        { "white_told", &LLTexUnit::BindCounts::whiteTold },
        { "beyond_channels", &LLTexUnit::BindCounts::beyondChannels },
        { "encoded", &LLTexUnit::BindCounts::encoded },
        { "encoded_null", &LLTexUnit::BindCounts::encodedNull },
        { "sampling_resends", &LLTexUnit::BindCounts::samplingResends },
        { "texture_reads", &LLTexUnit::BindCounts::textureReads },
    };

    /// At each warm-up frame's end: LLTexUnit's counts in the frame and the most of each in one frame.
    void noteFrameTextures()
    {
        const LLTexUnit::BindCounts now = LLTexUnit::bindCounts();
        ++sState.textureFrames;
        for (const TextureCount& count : TEXTURE_COUNTS)
        {
            U64& most = sState.texturesMost.*count.field;
            most = std::max(most, now.*count.field - sState.texturesAtFrame.*count.field);
        }
        sState.texturesAtFrame = now;
    }

    /// At each warm-up frame's end: LLVertexBuffer's counts in the frame, the most of each in one frame, and the same over
    /// the frames after the first; and the most of LLRender's dynamic state counts in one frame.
    void noteFrameDraws()
    {
        {
            const LLRender::DynamicStateCounts now = LLRender::dynamicStateCounts();
            for (const DynamicStateCount& count : DYNAMIC_STATE_COUNTS)
            {
                U64& most = sState.dynamicStateMost.*count.field;
                most = std::max(most, now.*count.field - sState.dynamicStateAtFrame.*count.field);
            }
            sState.dynamicStateAtFrame = now;
        }
        const LLVertexBuffer::DrawCounts now = LLVertexBuffer::drawCounts();
        const bool first = sState.drawFrames++ == 0;
        for (const DrawCount& count : DRAW_COUNTS)
        {
            const U64 frame = now.*count.field - sState.drawsAtFrame.*count.field;
            U64& most = sState.drawsMost.*count.field;
            most = std::max(most, frame);
            if (!first)
            {
                sState.drawsAfterFirst.*count.field += frame;
                U64& mostAfterFirst = sState.drawsMostAfterFirst.*count.field;
                mostAfterFirst = std::max(mostAfterFirst, frame);
            }
        }
        sState.drawsAtFrame = now;
        // scaleDown's draws ask the device for the copy program's pipeline once per run, so a first scaleDown after the
        // first warm-up frame shows in the requests the table makes; level2.py names these counts with that rule
        const U32 scale_downs = LLImageGL::sScaleDowns - sState.scaleDownsAtFrame;
        sState.scaleDownsAtFrame = LLImageGL::sScaleDowns;
        if (!first)
        {
            sState.scaleDownsAfterFirst += scale_downs;
            sState.scaleDownsMostAfterFirst = std::max(sState.scaleDownsMostAfterFirst, scale_downs);
        }
    }

    /// What every render profile measures from startup on: gDebugGL, the programs marked as needed at the login
    /// screen and the class the benchmark gave.
    void addStartupMeasures(LLSD& measured)
    {
        measured["debug_gl"] = gDebugGL;
        // The loaded programs marked as needed at the login screen (LLGLSLShader::mLoginScreen),
        // by name, which level2.py checks against its list.
        std::vector<std::string> login_screen;
        for (const LLGLSLShader* shader : LLGLSLShader::sInstances)
        {
            if (shader->mLoginScreen)
            {
                login_screen.push_back(shader->mName);
            }
        }
        std::sort(login_screen.begin(), login_screen.end());
        measured["login_screen_programs"] = LLSD::emptyArray();
        for (const std::string& name : login_screen)
        {
            measured["login_screen_programs"].append(name);
        }
        // Under measured, which level2.py never compares between runs.
        measured["benchmark"]["class"] = sState.benchmarkClass;
    }

    /// [Viewer Lab] The gated GL functions reached since the process began and their calls, by
    /// "<file>:<function>"; empty on GL. level2.py holds the names to scripts/metal/gated-gl.json on Metal.
    LLSD gatedGLCalls()
    {
        LLSD gated = LLSD::emptyMap();
        for (const auto& [name, calls] : LLGLGated::counts())
        {
            gated[name] = (LLSD::Integer)std::min<U64>(calls, (U64)S32_MAX);
        }
        return gated;
    }

    /// The render profile's parts that hold before login too: the GPU, graphics level, system, settings,
    /// features and programs.
    LLSD baseProfile()
    {
        LLSD profile;
        LLFeatureManager& features = LLFeatureManager::instance();
        profile["gpu"]["string"] = gGLManager.getRawGLString();
        profile["gpu"]["class"] = (S32)features.getGPUClass();
        profile["gpu"]["glsl_version"] = llformat("%d.%d", gGLManager.mGLSLVersionMajor, gGLManager.mGLSLVersionMinor);
        profile["gpu"]["vram_mb"] = (S32)gGLManager.mVRAM;
        profile["graphics_level"] = (S32)gSavedSettings.getU32("RenderQualityPerformance");
        profile["system"]["os"] = LLOSInfo::instance().getOSStringSimple();
        profile["system"]["gl_version"] = gGLManager.mGLVersionString;
        profile["system"]["driver"] = gGLManager.mDriverVersionVendorString;

        struct Collect : public LLControlGroup::ApplyFunctor
        {
            LLSD mValues = LLSD::emptyMap();
            void apply(const std::string& name, LLControlVariable* control) override
            {
                const LLSD& config = sState.config;
                if (LLStringUtil::startsWith(name, "Render") || LLFeatureManager::instance().getFeatures().count(name)
                    || config["settings"].has(name) || config["cached_settings"].has(name)
                    || (name == "GLTFEnabled" && config.has("gltf")))
                {
                    const LLSD typed = typedValue(control, control->getValue());
                    mValues[name] = typed.isUndefined() ? control->getValue() : typed;
                }
            }
        } collect;
        gSavedSettings.applyToAll(&collect);
        profile["settings"] = collect.mValues;

        profile["features"] = LLSD::emptyMap();
        for (const auto& [name, info] : features.getFeatures())
        {
            profile["features"][name] = llsd::map("available", info.mAvailable, "recommended", info.mRecommendedLevel);
        }

        // Sorted by name and level, so programs with the same name are numbered the same way in every run.
        std::vector<std::pair<std::string, S32>> shaders;
        for (const LLGLSLShader* shader : LLGLSLShader::sInstances)
        {
            shaders.emplace_back(shader->mName, shader->isComplete() ? shader->mShaderLevel : -1);
        }
        std::sort(shaders.begin(), shaders.end());
        profile["shaders"] = LLSD::emptyMap();
        for (const auto& [name, level] : shaders)
        {
            std::string key = name;
            for (S32 n = 2; profile["shaders"].has(key); ++n)
            {
                key = llformat("%s #%d", name.c_str(), n);
            }
            profile["shaders"][key] = level;
        }
        profile["measured"]["gl_gated"] = gatedGLCalls();
        // [Viewer Lab] The transfers since the process began whose GL operands were not the ones the
        // facades tracked, under gDebugGL on GL (0 otherwise); level2.py fails a debug_gl run on any.
        profile["measured"]["transfer_mismatches"] =
            (LLSD::Integer)std::min<U64>(LLGPUReadback::transferMismatches(), (U64)S32_MAX);
        return profile;
    }

    /// One glTF uniform buffer at the snapshot (gltf): whether its facade is allocated and dynamic, its size against
    /// the bytes the asset's upload computes now (expected), and how many floats of its contents, read back through the
    /// render interface, differ from them (beyond 1e-5 relative: the joints' products are computed in another translation
    /// unit). Asset::update uploads every buffer and nothing changes the asset after it in the frame, so an upload that
    /// did not reach the buffer shows here. GLTFSceneManager::render draws nothing today (upstream builds no batches), so
    /// the image cannot show the buffers.
    LLSD gltfBufferMeasure(const LL::GLTF::UniformBuffer& holder, const std::vector<F32>& expected)
    {
        const LLShaderUniformBuffer* facade = holder.facade();
        LLSD measure = llsd::map("allocated", facade != nullptr && facade->isAllocated(), "expected_size",
                                 (S32)(expected.size() * sizeof(F32)));
        LL::RHI::Buffer* buffer = facade ? facade->getRHIBuffer() : nullptr;
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        if (!buffer || !device)
        {
            return measure;
        }
        const LL::RHI::BufferDesc& desc = buffer->desc();
        measure["size"] = (S32)desc.size;
        measure["dynamic"] = desc.dynamic;
        if (desc.size != expected.size() * sizeof(F32))
        {
            return measure;
        }
        // Metal reads a buffer back too (an ordered GPU copy), so both backends read the contents and
        // level2.py requires differing_floats 0 on each.
        LL::RHI::Ref<LL::RHI::Readback> readback = device->encoder().readback(buffer, 0, desc.size);
        if (readback.isNull())
        {
            return measure;
        }
        if (!readback->ready())
        {
            readback->wait();
        }
        const std::span<const std::byte> data = readback->data();
        if (data.size() != desc.size)
        {
            return measure;
        }
        S32 differing = 0;
        for (size_t i = 0; i < expected.size(); ++i)
        {
            F32 value = 0.f;
            memcpy(&value, data.data() + i * sizeof(F32), sizeof(F32));
            differing += std::abs(value - expected[i]) <= 1e-5f * std::max(1.f, std::abs(expected[i])) ? 0 : 1;
        }
        measure["differing_floats"] = differing;
        return measure;
    }

    /// A matrix as glTF's uploads pack it: three rows of four floats (Asset::uploadTransforms, Skin::uploadMatrixPalette).
    static void packGLTFMatrix(const glm::mat4& matrix, std::vector<F32>& out)
    {
        const F32* m = glm::value_ptr(matrix);
        out.insert(out.end(), { m[0], m[1], m[2], m[12], m[4], m[5], m[6], m[13], m[8], m[9], m[10], m[14] });
    }

    /// The asset's uniform buffers (gltf): nodes, materials and each skin's joints (gltfBufferMeasure), each against
    /// what Asset::uploadTransforms, uploadMaterials and Skin::uploadMatrixPalette compute, packed as they pack it.
    LLSD gltfBufferMeasures(const LL::GLTF::Asset& asset)
    {
        const size_t max_joints = (size_t)LLSkinningUtil::getMaxGLTFJointCount();
        std::vector<F32> nodes;
        for (size_t i = 0; i < llmin(max_joints, asset.mNodes.size()); ++i)
        {
            packGLTFMatrix(asset.mNodes[i].mAssetMatrix, nodes);
        }
        const size_t material_size = sizeof(glm::vec4) * 12;
        const size_t material_count = llmin(asset.mMaterials.size(), (size_t)gGLManager.mMaxUniformBlockSize / material_size);
        std::vector<glm::vec4> md(material_count * 12);
        for (size_t i = 0; i < material_count * 12; i += 12)
        {
            const LL::GLTF::Material& material = asset.mMaterials[i / 12];
            material.mPbrMetallicRoughness.mBaseColorTexture.mTextureTransform.getPacked(&md[i + 0]);
            md[i + 1].g = (F32)material.mPbrMetallicRoughness.mBaseColorTexture.getTexCoord();
            material.mNormalTexture.mTextureTransform.getPacked(&md[i + 2]);
            md[i + 3].g = (F32)material.mNormalTexture.getTexCoord();
            material.mPbrMetallicRoughness.mMetallicRoughnessTexture.mTextureTransform.getPacked(&md[i + 4]);
            md[i + 5].g = (F32)material.mPbrMetallicRoughness.mMetallicRoughnessTexture.getTexCoord();
            material.mEmissiveTexture.mTextureTransform.getPacked(&md[i + 6]);
            md[i + 7].g = (F32)material.mEmissiveTexture.getTexCoord();
            material.mOcclusionTexture.mTextureTransform.getPacked(&md[i + 8]);
            md[i + 9].g = (F32)material.mOcclusionTexture.getTexCoord();
            const F32 min_alpha = material.mAlphaMode == LL::GLTF::Material::AlphaMode::MASK ? material.mAlphaCutoff : -1.0f;
            md[i + 10] = glm::vec4(material.mEmissiveFactor, 1.f);
            md[i + 11] = glm::vec4(0.f, material.mPbrMetallicRoughness.mRoughnessFactor,
                                   material.mPbrMetallicRoughness.mMetallicFactor, min_alpha);
        }
        std::vector<F32> materials;
        for (const glm::vec4& v : md)
        {
            materials.insert(materials.end(), { v.x, v.y, v.z, v.w });
        }
        LLSD measures = llsd::map("nodes", gltfBufferMeasure(asset.mNodesUBO, nodes), "materials",
                                  gltfBufferMeasure(asset.mMaterialsUBO, materials), "skins", LLSD::emptyArray());
        for (const LL::GLTF::Skin& skin : asset.mSkins)
        {
            std::vector<F32> joints;
            for (size_t i = 0; i < llmin(max_joints, skin.mJoints.size()); ++i)
            {
                packGLTFMatrix(asset.mNodes[skin.mJoints[i]].mAssetMatrix * skin.mInverseBindMatricesData[i], joints);
            }
            measures["skins"].append(gltfBufferMeasure(skin.mUBO, joints));
        }
        return measures;
    }

    /// The glTF run's measures at the snapshot (gltf): GLTFEnabled, the glTF program's variants, the loads, whether
    /// the first asset died, the host's asset (its parts, its uniform buffers (gltfBufferMeasures), the render batches
    /// GLTFSceneManager::render would draw, the images that have a texture without a GL one) and, with the raycast debug
    /// display, the warm-up frames whose debug ray hit it.
    LLSD gltfMeasures()
    {
        LLSD gltf;
        gltf["enabled"] = gSavedSettings.getBOOL("GLTFEnabled");
        S32 complete = 0;
        for (const LLGLSLShader& variant : gGLTFPBRMetallicRoughnessProgram.mGLTFVariants)
        {
            complete += variant.isComplete() ? 1 : 0;
        }
        gltf["program"] = llsd::map("variants", (S32)gGLTFPBRMetallicRoughnessProgram.mGLTFVariants.size(), "complete",
                                    complete, "can_use", gSavedSettings.getBOOL("RenderCanUseGLTFPBROpaqueShaders"));
        gltf["loads"] = sState.gltfLoads;
        gltf["first_asset_released"] = sState.gltfLoads.size() >= 2 && sState.gltfFirst.expired();
        LLViewerObject* host = gObjectList.findObject(sState.gltfHost);
        const std::shared_ptr<LL::GLTF::Asset> asset = host ? host->mGLTFAsset : nullptr;
        const auto& objects = LL::GLTFSceneManager::instance().mObjects;
        gltf["scene_manager_objects"] = (S32)objects.size();
        if (asset)
        {
            S32 primitives = 0;
            for (const LL::GLTF::Mesh& mesh : asset->mMeshes)
            {
                primitives += (S32)mesh.mPrimitives.size();
            }
            gltf["asset"] = llsd::map("nodes", (S32)asset->mNodes.size(), "meshes", (S32)asset->mMeshes.size(), "primitives",
                                      primitives, "materials", (S32)asset->mMaterials.size(), "skins",
                                      (S32)asset->mSkins.size(), "animations", (S32)asset->mAnimations.size(), "images",
                                      (S32)asset->mImages.size());
            gltf["uniform_buffers"] = gltfBufferMeasures(*asset);
            // The batches GLTFSceneManager::render(Asset&, U8) would draw: those with primitives and a vertex buffer.
            S32 drawable = 0;
            for (const LL::GLTF::RenderData& data : asset->mRenderData)
            {
                for (const auto& batches : data.mBatches)
                {
                    for (const LL::GLTF::RenderBatch& batch : batches)
                    {
                        drawable += !batch.mPrimitives.empty() && batch.mVertexBuffer.notNull() ? 1 : 0;
                    }
                }
            }
            gltf["drawable_batches"] = drawable;
            S32 without = 0;
            for (const LL::GLTF::Image& image : asset->mImages)
            {
                without += image.mTexture.notNull() && !image.mTexture->hasTexture() ? 1 : 0;
            }
            gltf["images_without_gl_texture"] = without;
        }
        if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_RAYCAST))
        {
            gltf["raycast"] = llsd::map("frames", (S32)sState.gltfRayFrames, "hits", (S32)sState.gltfRayHits, "node",
                                        gDebugRaycastGLTFNodeHit, "primitive", gDebugRaycastGLTFPrimitiveHit);
        }
        return gltf;
    }

    /// GLTFSceneManager::bindTexture's fallback binds (gltf), at the snapshot after the frame is saved and
    /// last in the render profile, because it binds textures. GLTFSceneManager::render draws nothing today (upstream
    /// builds no batches), so bindTexture never runs in the viewer; the probe calls GLTFSceneManager::bind for the
    /// host asset's first material, as render does for a batch, with the glTF program's opaque variant 0 bound (it samples
    /// all five texture slots). The asset has no images, so each slot takes bindTexture's fallback: the flat normal image
    /// for NORMAL, the white image for the others. Before the call each slot's channel is unbound and given the other
    /// image through LLTexUnit, so a slot whose fallback bind did not reach its channel still holds that one. After it,
    /// each channel is made GL's active unit (another unit first, so it reaches GL whatever gGL's record says) and GL's
    /// binding is read through gl_check_sampling, which also compares the bound texture's MAG_FILTER with LINEAR (the
    /// fallback branch sets no sampling, so the image keeps its own) and warns with the RHI tag on a difference, which
    /// fails the run. Then every channel is unbound through LLTexUnit, so GL and the units agree again, and the shader
    /// bound before is bound again. Per slot: its channel and what GL holds there (white, flat_normal or other; off GL
    /// what the encoder's tracker holds); an empty array when the probe cannot run.
    LLSD gltfBindTextureProbe()
    {
        LLSD slots = LLSD::emptyArray();
        LLViewerObject* host = gObjectList.findObject(sState.gltfHost);
        const std::shared_ptr<LL::GLTF::Asset> asset = host ? host->mGLTFAsset : nullptr;
        LLViewerFetchedTexture* white = LLViewerFetchedTexture::sWhiteImagep;
        LLViewerFetchedTexture* flat = LLViewerFetchedTexture::sFlatNormalImagep;
        if (!asset || asset->mMaterials.empty() || gGLTFPBRMetallicRoughnessProgram.mGLTFVariants.empty() || !white
            || !flat || !white->getGLTexture() || !flat->getGLTexture())
        {
            return slots;
        }
        LLGLSLShader* previous = LLGLSLShader::sCurBoundShaderPtr;
        gGLTFPBRMetallicRoughnessProgram.bind(U8(0));
        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        if (!shader)
        {
            return slots;
        }
        constexpr S32 SLOTS = (S32)LL::GLTF::TEXTURE_TYPE_COUNT;
        static const std::pair<S32, const char*> slot_maps[SLOTS] = {
            { LLShaderMgr::DIFFUSE_MAP, "base_color" },
            { LLShaderMgr::NORMAL_MAP, "normal" },
            { LLShaderMgr::METALLIC_ROUGHNESS_MAP, "metallic_roughness" },
            { LLShaderMgr::OCCLUSION_MAP, "occlusion" },
            { LLShaderMgr::EMISSIVE_MAP, "emissive" },
        };
        constexpr S32 NORMAL_SLOT = (S32)LL::GLTF::TextureType::NORMAL;
        S32 channels[SLOTS];
        // [Viewer Lab] Golden-only verification, like level 1's GL scenes, which stays: the hook reads
        // the GL name LLTexUnit's cache holds for each slot's texture only to ask gl_check_sampling, the GL backend's own
        // check, which unit samples which texture. No viewer path reads or passes these names. Off GL the check is
        // false, so there the slot holds the fallback whose device texture the encoder's tracker has on its channel
        // (LLTexUnit::getCurrRHITexture). That branch compares the texture only: it has no MAG_FILTER check,
        // because the facade's sampling of a texture is private to llrender.
        U32 white_name = 0;
        U32 flat_name = 0;
        for (S32 i = 0; i < SLOTS; ++i)
        {
            channels[i] = shader->getTextureChannel(slot_maps[i].first);
            if (channels[i] > -1)
            {
                LLTexUnit* unit = gGL.getTexUnit(channels[i]);
                unit->unbind(LLTexUnit::TT_TEXTURE);
                unit->bindTexture(LLTexUnit::TT_TEXTURE, (i == NORMAL_SLOT ? white : flat)->getGLTexture());
                (i == NORMAL_SLOT ? white_name : flat_name) = unit->getCurrTexture();
            }
        }
        LL::GLTFSceneManager& manager = LL::GLTFSceneManager::instance();
        for (S32& last : manager.mLastTexture)
        {
            last = -2; // as render does after binding the shader
        }
        manager.bind(*asset, asset->mMaterials[0]);
        LL::RHI::SamplerDesc linear;
        linear.magFilter = LL::RHI::MagFilter::Linear;
        for (S32 i = 0; i < SLOTS; ++i)
        {
            LLSD slot = llsd::map("slot", slot_maps[i].second, "channel", channels[i]);
            if (channels[i] > -1)
            {
                gGL.getTexUnit(channels[i] == 0 ? 1 : 0)->activate();
                gGL.getTexUnit(channels[i])->activate();
                const LL::RHI::Device* device = LL::RHI::viewerDevice();
                if (!device || device->backend() == LL::RHI::BackendType::GL)
                {
                    slot["holds"] = white_name && gl_check_sampling(LLTexUnit::TT_TEXTURE, white_name, linear, 4)
                                        ? "white"
                                    : flat_name && gl_check_sampling(LLTexUnit::TT_TEXTURE, flat_name, linear, 4)
                                        ? "flat_normal"
                                        : "other";
                }
                else
                {
                    const LL::RHI::Texture* held = gGL.getTexUnit(channels[i])->getCurrRHITexture();
                    slot["holds"] = held && held == white->getGLTexture()->getRHITexture()  ? "white"
                                    : held && held == flat->getGLTexture()->getRHITexture() ? "flat_normal"
                                                                                            : "other";
                }
            }
            slots.append(slot);
        }
        for (S32 i = 0; i < SLOTS; ++i)
        {
            if (channels[i] > -1)
            {
                gGL.getTexUnit(channels[i])->unbind(LLTexUnit::TT_TEXTURE);
            }
        }
        gGL.getTexUnit(0)->activate();
        LLGLSLShader::unbind();
        if (previous)
        {
            previous->bind();
        }
        return slots;
    }

    LLSD renderProfile()
    {
        LLSD profile = baseProfile();
        LLSD& pipeline = profile["pipeline"];
        pipeline["use_occlusion"] = LLPipeline::sUseOcclusion;
        pipeline["reflection_probes"] = LLPipeline::sReflectionProbesEnabled;
        pipeline["transparent_water"] = LLPipeline::sRenderTransparentWater;
        pipeline["glow"] = LLPipeline::sRenderGlow;
        pipeline["fsaa_type"] = (S32)LLPipeline::RenderFSAAType;
        pipeline["shadow_detail"] = LLPipeline::RenderShadowDetail;
        pipeline["ssao"] = LLPipeline::RenderDeferredSSAO;
        pipeline["depth_of_field"] = LLPipeline::RenderDepthOfField;
        pipeline["screen_space_reflections"] = LLPipeline::RenderScreenSpaceReflections;
        pipeline["mirrors"] = LLPipeline::RenderMirrors;
        pipeline["resolution_divisor"] = (S32)LLPipeline::RenderResolutionDivisor;

        LLSD& window = profile["window"];
        window["raw"] = pair(gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw());
        window["scaled"] = pair(gViewerWindow->getWindowWidthScaled(), gViewerWindow->getWindowHeightScaled());
        window["world_view"] = pair(gViewerWindow->getWorldViewWidthRaw(), gViewerWindow->getWorldViewHeightRaw());
        window["display_scale"] = llsd::array(gViewerWindow->getDisplayScale().mV[VX], gViewerWindow->getDisplayScale().mV[VY]);
        window["screen_buffer"] = pair((S32)gPipeline.mRT->screen.getWidth(), (S32)gPipeline.mRT->screen.getHeight());

        // The groups of every partition that occlusion culls (not terrain or water), as the world
        // camera last saw them, and those of the object cache: the viewer moves objects it cannot
        // see, occluded ones too, out of the scene into the cache after a while
        // (LLViewerRegion::killInvisibleObjects), and the cache culls its own groups.
        const S32 camera = LLViewerCamera::sCurCameraID;
        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
        CountGroups objects;
        CountGroups cache;
        LLSD by_partition = LLSD::emptyMap();
        LLSD visible = LLSD::emptyMap();
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            for (U32 type = 0; type < LLViewerRegion::NUM_PARTITIONS; ++type)
            {
                LLSpatialPartition* partition = region->getSpatialPartition(type);
                if (partition)
                {
                    CountGroups one;
                    one.traverse(partition->mOctree);
                    const std::string key = llformat("%u", type);
                    by_partition[key] = by_partition[key].asInteger() + one.mGroups;
                    // What the camera sees of the partition, which the sun's shadow frusta are fitted to.
                    if (gPipeline.hasRenderType(partition->mDrawableType))
                    {
                        LLVector3 min(65536.f, 65536.f, 65536.f);
                        LLVector3 max(-65536.f, -65536.f, -65536.f);
                        partition->getVisibleExtents(*LLViewerCamera::getInstance(), min, max);
                        if (min.mV[VX] <= max.mV[VX])
                        {
                            visible[key] = llsd::array(ll_sd_from_vector3(min), ll_sd_from_vector3(max));
                        }
                    }
                }
                if (partition && partition->mOcclusionEnabled)
                {
                    objects.traverse(partition->mOctree);
                }
            }
            if (LLVOCachePartition* partition = region->getVOCachePartition())
            {
                cache.traverse(partition->mOctree);
            }
        }
        LLViewerCamera::sCurCameraID = (LLViewerCamera::eCameraID)camera;
        LLSD& measured = profile["measured"];
        // Where the own avatar stands with the UI shown (show_ui): its name tag follows it.
        if (sState.config["show_ui"].asBoolean() && gAgent.getRegion())
        {
            const LLVector3 avatar = gAgent.getPositionAgent() - gAgent.getRegion()->getOriginAgent();
            measured["ui"]["avatar"] = llsd::array(avatar.mV[VX], avatar.mV[VY], avatar.mV[VZ]);
        }
        measured["spatial_groups"] = objects.mGroups;
        measured["occluded_groups"] = objects.mOccluded;
        measured["cache_groups"] = cache.mGroups;
        measured["occluded_cache_groups"] = cache.mOccluded;
        // The world camera's occlusion results since the start: read, 0 (occluded) and waited for.
        measured["occlusion_results"]["read"] = (LLSD::Integer)LLOcclusionCullingGroup::sWorldResultsRead;
        measured["occlusion_results"]["hidden"] = (LLSD::Integer)LLOcclusionCullingGroup::sWorldResultsHidden;
        measured["occlusion_results"]["waited"] = (LLSD::Integer)LLOcclusionCullingGroup::sWorldResultsWaited;
        measured["frames_to_ready"] = (S32)sState.readyAfter;
        measured["textures_missing"] = sState.unavailable;
        measured["textures_not_requested"] = sState.unrequested;
        measured["reflection_probes"] = gPipeline.mReflectionMapManager.probeState();
        // The sun's shadow frusta are fitted to what the camera sees (LLPipeline::getVisibleExtents).
        measured["groups_by_partition"] = by_partition;
        measured["visible_extents_by_partition"] = visible;
        // What functional alone draws: the particles alive at the snapshot and whether the
        // camera sees their partition, and the screen-space reflection copies over the warm-up (renderFinalize's, the
        // main view's). level2.py gives functional/overview floors on them.
        measured["particles"] = llsd::map("alive", LLViewerPartSim::sParticleCount2, "in_view",
                                          visible.has(llformat("%u", (U32)LLViewerRegion::PARTITION_PARTICLE)));
        measured["screen_space_reflections"] =
            llsd::map("copies", (S32)(gPipeline.mScreenSpaceReflectionCopies - sState.ssrCopiesAtWarmup));
        measured["terrain"] = sState.terrain;
        measured["shadow_extents"] = LLSD::emptyArray();
        for (const LLVector3* extents : gPipeline.mShadowExtents)
        {
            measured["shadow_extents"].append(llsd::array(ll_sd_from_vector3(extents[0]), ll_sd_from_vector3(extents[1])));
        }
        // The selection run (select): where the selected objects are, and the manipulator's state.
        if (!sState.selected.empty())
        {
            LLSD& selection = measured["selection"];
            selection["names"] = LLSD::emptyArray();
            selection["positions"] = LLSD::emptyMap();
            for (const auto& [name, id] : sState.selected)
            {
                selection["names"].append(name);
                if (LLViewerObject* object = gObjectList.findObject(id))
                {
                    selection["positions"][name] = ll_sd_from_vector3(object->getPositionRegion());
                }
            }
            LLSD& manip = measured["manip"];
            manip["tool"] = sState.config["tool"];
            LLManip* manipulator = selectionManip();
            manip["highlighted"] = manipulator ? manipPartName(manipulator->getHighlightedPart()) : std::string();
            manip["captured"] = manipulator && manipulator->hasMouseCapture();
            if (sState.dragPinned)
            {
                manip["drag"] = llsd::map("point", llsd::array(sState.dragPoint.mX, sState.dragPoint.mY), "probes",
                                          std::abs(sState.dragProbes));
            }
        }
        if (sState.config["monitor_check"].asBoolean())
        {
            measured["monitor"] = sState.monitor;
        }
        if (sState.config["back_buffer_copy_check"].asBoolean())
        {
            measured["back_buffer_copy"] = sState.backBufferCopy;
        }
        if (sState.config["rebake"].asBoolean())
        {
            measured["rebake"] = llsd::map("bake_frames", sState.bakeFrames, "renders", sState.bakeRenders,
                                           "preview_textures", sState.previewTextures);
        }
        if (sState.config.has("gltf"))
        {
            measured["gltf"] = gltfMeasures();
        }
        if (!sState.localLights.empty())
        {
            measured["local_lights"] = localLightsMeasures();
        }
        if (sState.cursorLocked)
        {
            measured["cursor"] = llsd::map("point", pair(sState.cursorPoint.mX, sState.cursorPoint.mY), "moved_frames",
                                           (S32)sState.cursorMoved, "hover_frames", (S32)sState.cursorHoverRay);
        }
        // The render device's frames over the warm-up against the main loop's, and its surface;
        // saveFrame has checked the device.
        if (LL::RHI::Device* device = LL::RHI::viewerDevice(); device && device->surface())
        {
            const LL::RHI::TextureDesc& back = device->surface()->backBuffer()->desc();
            measured["rhi"] = llsd::map("backend", device->backend() == LL::RHI::BackendType::GL ? "GL" : "Metal",
                                        "device_frames", (S32)(device->frameNumber() - sState.deviceFrameAtWarmup),
                                        "viewer_frames", (S32)(LLFrameTimer::getFrameCount() - sState.viewerFrameAtWarmup),
                                        "surface", pair((S32)back.width, (S32)back.height));
            // On Metal the native depth bias changes over the warm-up, which level2.py
            // floors per frame (the glow pool draws at -1/-1); not written on GL.
            if (device->backend() == LL::RHI::BackendType::Metal)
            {
                measured["rhi"]["depth_bias_changes"] = (S32)(metalDepthBiasChanges() - sState.depthBiasChangesAtWarmup);
                measured["rhi"]["compile"] = metalCompileMeasures(*device);
            }
            // The uniform blocks: the declared ones by name, the sweeps of
            // the mapped programs since the process began, and the ReflectionProbes block's buffer once updateUniforms
            // has allocated it. level2.py fails a world run, on GL and on Metal, unless
            // ReflectionProbes is declared, nothing was swept and the buffer is dynamic at the size of
            // LLReflectionMapManager::ReflectionProbeData.
            LLSD& blocks = measured["rhi"]["uniform_blocks"];
            blocks["declared"] = LLSD::emptyArray();
            const U32 declared = LLShaderUniformBuffer::declaredBlocks();
            for (U32 i = 0; i < LLGLSLShader::NUM_UNIFORM_BLOCKS; ++i)
            {
                if (declared & (1u << i))
                {
                    blocks["declared"].append(LLGLSLShader::uniformBlockName(LLGLSLShader::UniformBlock(i)));
                }
            }
            blocks["sweeps"] = (S32)LLShaderUniformBuffer::programSweeps();
            blocks["probes_data_size"] = (S32)sizeof(LLReflectionMapManager::ReflectionProbeData);
            if (const LL::RHI::Buffer* buffer = gPipeline.mReflectionMapManager.getProbesBuffer())
            {
                blocks["probes_buffer"] = llsd::map("dynamic", buffer->desc().dynamic, "size", (S32)buffer->desc().size);
            }
        }
        // LLVertexBuffer's draws over the warm-up up to the snapshot, per frame of the render device and the most in one
        // warm-up frame, and since the process began, and the device's requests per warm-up frame after the first;
        // level2.py fails a run without a test fault that skipped a draw or left one raw, or whose counts do not add up.
        {
            const LLVertexBuffer::DrawCounts now = LLVertexBuffer::drawCounts();
            const LLVertexBuffer::DrawCounts& start = sState.drawsAtWarmup;
            const LL::RHI::Device* device = LL::RHI::viewerDevice();
            const U64 frames = device ? device->frameNumber() - sState.deviceFrameAtWarmup : 0;
            LLSD& draws = measured["draws"];
            draws["frames"] = (S32)frames;
            for (const DrawCount& count : DRAW_COUNTS)
            {
                const U64 warmup = now.*count.field - start.*count.field;
                draws[count.name] = (S32)warmup;
                draws["per_frame"][count.name] = frames ? F64(warmup) / F64(frames) : 0.0;
                draws["most_in_a_frame"][count.name] = (S32)(sState.drawsMost.*count.field);
                draws["total"][count.name] = (S32)(now.*count.field);
            }
            // renderDeferredLighting's read-only depth sites over the warm-up: encoded draws and those in a
            // read-only pass, in LLPipeline::ReadOnlySite's order
            static const char* const READ_ONLY_SITES[LLPipeline::RO_SITE_COUNT] = { "blur", "soften", "lights", "probe_faces" };
            for (S32 site = 0; site < LLPipeline::RO_SITE_COUNT; ++site)
            {
                const LLPipeline::ReadOnlySiteCounts& at = sState.readOnlySitesAtWarmup[site];
                const LLPipeline::ReadOnlySiteCounts& end = LLPipeline::sReadOnlySites[site];
                draws["read_only_sites"][READ_ONLY_SITES[site]] =
                    llsd::map("encoded", (S32)(end.encoded - at.encoded), "read_only", (S32)(end.readOnly - at.readOnly));
            }
            const U32 after_first = sState.drawFrames > 1 ? sState.drawFrames - 1 : 0;
            LLSD& requests = draws["after_first_frame"];
            requests["frames"] = (S32)after_first;
            for (const DrawCount& count : DRAW_REQUESTS)
            {
                requests["per_frame"][count.name] =
                    after_first ? F64(sState.drawsAfterFirst.*count.field) / F64(after_first) : 0.0;
                requests["most_in_a_frame"][count.name] = (S32)(sState.drawsMostAfterFirst.*count.field);
            }
            requests["scale_downs"] = (S32)sState.scaleDownsAfterFirst;
            requests["scale_downs_most_in_a_frame"] = (S32)sState.scaleDownsMostAfterFirst;
        }
        // The draws per program over the warm-up, by shader name: level2.py floors them per warm-up frame
        // from features.json. The tally stops here.
        {
            LLVertexBuffer::sTallyPrograms = false;
            LLSD& programs = measured["programs"];
            programs["frames"] = measured["draws"]["frames"];
            programs["draws"] = LLSD::emptyMap();
            for (const auto& [name, draws] : LLVertexBuffer::programTally())
            {
                programs["draws"][name] = (S32)draws;
            }
        }
        // Whether the pipeline made SMAA's sample map: level2.py fails a scene that asks for it
        // without it, since the run would take the null-sample path and prove nothing about the map.
        measured["smaa_sample_map"] = gPipeline.mSMAASampleMap.notNull();
        // LLRender's dynamic state over the warm-up up to the snapshot, per frame of the render device and the most in one
        // warm-up frame, and since the process began: the values the facades and LLGLState gave the encoder in a
        // pass, the stack-pass begins that got the viewer's state again and their setter calls, and the enum changes with no
        // fixed path. level2.py fails a run whose stack passes got no state, or with an unclassified enum.
        {
            const LLRender::DynamicStateCounts& now = LLRender::dynamicStateCounts();
            const LLRender::DynamicStateCounts& start = sState.dynamicStateAtWarmup;
            const LL::RHI::Device* device = LL::RHI::viewerDevice();
            const U64 frames = device ? device->frameNumber() - sState.deviceFrameAtWarmup : 0;
            LLSD& dynamic = measured["dynamic_state"];
            dynamic["frames"] = (S32)frames;
            for (const DynamicStateCount& count : DYNAMIC_STATE_COUNTS)
            {
                const U64 warmup = now.*count.field - start.*count.field;
                dynamic[count.name] = (S32)warmup;
                dynamic["per_frame"][count.name] = frames ? F64(warmup) / F64(frames) : 0.0;
                dynamic["most_in_a_frame"][count.name] = (S32)(sState.dynamicStateMost.*count.field);
                dynamic["total"][count.name] = (S32)(now.*count.field);
            }
        }
        // LLTexUnit's bindings over the warm-up up to the snapshot, per frame of the render device and the most in one
        // warm-up frame, and since the process began, in measured.draws' layout, and at the snapshot the textures
        // the encoder's tracker holds whose sampling LLTexUnit does not know (in_use); level2.py prints them and fails a run
        // whose counts do not add up, that tells the encoder nothing, refuses a channel or has a bound texture whose
        // filter is unknown.
        {
            const LLTexUnit::BindCounts now = LLTexUnit::bindCounts();
            const LLTexUnit::BindCounts& start = sState.texturesAtWarmup;
            const LL::RHI::Device* device = LL::RHI::viewerDevice();
            const U64 frames = device ? device->frameNumber() - sState.deviceFrameAtWarmup : 0;
            LLSD& textures = measured["textures"];
            textures["frames"] = (S32)frames;
            textures["warmup_frames_noted"] = (S32)sState.textureFrames;
            for (const TextureCount& count : TEXTURE_COUNTS)
            {
                const U64 warmup = now.*count.field - start.*count.field;
                textures[count.name] = (S32)warmup;
                textures["per_frame"][count.name] = frames ? F64(warmup) / F64(frames) : 0.0;
                textures["most_in_a_frame"][count.name] = (S32)(sState.texturesMost.*count.field);
                textures["total"][count.name] = (S32)(now.*count.field);
            }
            U32 unknown_filter = 0;
            U32 unknown_address = 0;
            LLTexUnit::unknownSamplingInUse(unknown_filter, unknown_address);
            textures["in_use"]["filter"] = (S32)unknown_filter;
            textures["in_use"]["address"] = (S32)unknown_address;
            // On Metal, the sampler reads over the warm-up of a
            // type slot that was not the channel's current texture; information only, and not written on GL.
            if (device && device->backend() == LL::RHI::BackendType::Metal)
            {
                textures["type_slot_reads"] = (S32)(metalTypeSlotReads() - sState.typeSlotReadsAtWarmup);
            }
        }
        // The loaded shaders whose attribute mask, which LLVertexBuffer's draws give the device, is not their program's
        // reflected one; level2.py fails a run with any.
        {
            S32 mismatches = 0;
            for (const LLGLSLShader* shader : LLGLSLShader::sInstances)
            {
                const LL::RHI::Program* program = shader->getRHIProgram();
                if (program && shader->isReflectionReady() && shader->mAttributeMask != program->reflection().attributeMask)
                {
                    ++mismatches;
                }
            }
            measured["attribute_mask_mismatches"] = mismatches;
        }
        addStartupMeasures(measured);
        // LLImageGLThread's texture and media uploads as LLImageGL::initClass set them at startup, and LLImageGL's counts
        // since startup: what it sent to the render device and its syncs from the upload thread, swaps of a
        // name, the upload context's fences and their waits, and media's swaps of a deferred texture, which level2.py
        // checks against the scene.
        // The media phase's windows and faces (media); level2.py checks and --perf reads them.
        if (sState.mediaResult.isDefined())
        {
            measured["media"] = sState.mediaResult;
        }
        measured["upload_thread"] = llsd::map("textures", LLImageGLThread::sEnabledTextures,
                                              "media", LLImageGLThread::sEnabledMedia);
        measured["image_gl"] = imageGLMeasures();
        if (LLSD uploads = metalUploadMeasures(); uploads.isDefined())
        {
            measured["metal_uploads"] = uploads;
        }
        // [Viewer Lab] A tour's stop: which one, and the counts above when a later stop began, so that
        // level2.py floors a count by what the stop itself did (the anchor's are a fresh run's, from the start).
        if (sTour.active())
        {
            measured["tour"] = llsd::map("stop", S32(sTour.stop + 1), "stops", S32(sTour.stops.size()),
                                         "image_gl_at_begin", sTour.imageGLAtBegin,
                                         "metal_uploads_at_begin", sTour.metalUploadsAtBegin);
        }
        // The shader manager's level of each shader class, the avatar's highest level and the groups of
        // the load that failed at or after their own settle (bits by class), which level2.py --against
        // compares.
        static const std::pair<LLViewerShaderMgr::EShaderClass, const char*> shader_classes[] = {
            { LLViewerShaderMgr::SHADER_LIGHTING, "lighting" },
            { LLViewerShaderMgr::SHADER_OBJECT, "object" },
            { LLViewerShaderMgr::SHADER_AVATAR, "avatar" },
            { LLViewerShaderMgr::SHADER_ENVIRONMENT, "environment" },
            { LLViewerShaderMgr::SHADER_INTERFACE, "interface" },
            { LLViewerShaderMgr::SHADER_EFFECT, "effect" },
            { LLViewerShaderMgr::SHADER_WINDLIGHT, "windlight" },
            { LLViewerShaderMgr::SHADER_WATER, "water" },
            { LLViewerShaderMgr::SHADER_DEFERRED, "deferred" },
        };
        const LLViewerShaderMgr* shader_mgr = LLViewerShaderMgr::instance();
        LLSD& shader_groups = measured["shader_groups"];
        shader_groups = LLSD::emptyMap();
        for (const auto& [shader_class, name] : shader_classes)
        {
            shader_groups[name] = shader_mgr->mShaderLevel[shader_class];
        }
        shader_groups["avatar_max"] = shader_mgr->mMaxAvatarShaderLevel;
        shader_groups["failed"] = (S32)shader_mgr->failedGroups();
        // The test fault, when the config arms one: the creations of the program it failed, the
        // digest of the GLSL each program of that name in sInstances is built from (mSourceHash), sorted,
        // and the shader manager's publications of pending programs in this load, a retry's again (late),
        // and those LLGLSLShader::bind made since the process began (bound).
        const auto sources_of = [](const std::string& program)
        {
            std::vector<std::string> sources;
            for (const LLGLSLShader* shader : LLGLSLShader::sInstances)
            {
                if (shader->mName == program)
                {
                    sources.push_back(shader->mSourceHash.asString());
                }
            }
            std::sort(sources.begin(), sources.end());
            return sources;
        };
        const LL::RHI::ProgramFault& fault = sState.programFault;
        if (!fault.program.empty())
        {
            const std::vector<std::string> sources = sources_of(fault.program);
            LLSD& measured_fault = measured["program_fault"];
            measured_fault["program"] = fault.program;
            measured_fault["deferred"] = fault.deferred;
            measured_fault["times"] = (S32)fault.times;
            measured_fault["hits"] = (S32)LL::RHI::programFaultHits();
            measured_fault["late"] = (S32)shader_mgr->latePublications();
            measured_fault["bound"] = (S32)LLGLSLShader::bindPublications();
            measured_fault["sources"] = LLSD::emptyArray();
            for (const std::string& source : sources)
            {
                measured_fault["sources"].append(source);
            }
        }
#if LL_RHI_METAL
        // Metal's fault list, when the config arms one: each fault in the config's order with the creations
        // of its program it failed and its sources, as program_fault's.
        if (!sState.programFaults.empty())
        {
            LLSD& measured_faults = measured["program_faults"];
            measured_faults = LLSD::emptyArray();
            for (const LL::RHI::ProgramFault& listed : sState.programFaults)
            {
                LLSD entry;
                entry["program"] = listed.program;
                entry["deferred"] = listed.deferred;
                entry["times"] = (S32)listed.times;
                entry["hits"] = (S32)LL::RHI::metalProgramFaultHits(listed.program);
                entry["sources"] = LLSD::emptyArray();
                for (const std::string& source : sources_of(listed.program))
                {
                    entry["sources"].append(source);
                }
                measured_faults.append(entry);
            }
        }
#endif
        // The test delay, when the config arms one: the frames whose trigger settled, the publications those
        // made, the frame of the last (LLFrameTimer's), and those LLGLSLShader::bind made since the process began.
        if (const LL::RHI::ProgramDelay& delay = sState.programDelay; !delay.program.empty())
        {
            LLSD& measured_delay = measured["program_delay"];
            measured_delay["program"] = delay.program;
            measured_delay["frames"] = (S32)delay.frames;
            measured_delay["frame_settles"] = (S32)shader_mgr->frameSettles();
            measured_delay["frame_publications"] = (S32)shader_mgr->framePublications();
            measured_delay["last_frame"] = (S32)shader_mgr->lastSettleFrame();
            measured_delay["bound"] = (S32)LLGLSLShader::bindPublications();
        }
        if (sState.config.has("gltf"))
        {
            measured["gltf"]["bind_texture"] = gltfBindTextureProbe(); // last: it binds textures
        }
        return profile;
    }

    /// The render profile at the login screen (login_screen): what holds before login, the window and
    /// the frames shown there.
    LLSD loginProfile()
    {
        LLSD profile = baseProfile();
        LLSD& window = profile["window"];
        window["raw"] = pair(gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw());
        window["scaled"] = pair(gViewerWindow->getWindowWidthScaled(), gViewerWindow->getWindowHeightScaled());
        window["display_scale"] = llsd::array(gViewerWindow->getDisplayScale().mV[VX], gViewerWindow->getDisplayScale().mV[VY]);
        LLSD& measured = profile["measured"];
        measured["login_screen_frames"] = (S32)sState.frames;
        addStartupMeasures(measured);
        if (sState.phase == Phase::MetalLogin)
        {
            // The Metal login hook's profile only; the login screen run's profile does not have these. The back
            // buffer is the surface's on Metal and the raw window on GL; LLImageGL's swaps show the login page's media
            // texture reaching the frame through a deferred swap; geometry_shaders explains the GL-only programs.
            LL::RHI::Device* device = LL::RHI::viewerDevice();
            if (device && device->backend() == LL::RHI::BackendType::Metal && device->surface())
            {
                const LL::RHI::TextureDesc& back = device->surface()->backBuffer()->desc();
                window["back_buffer"] = pair((S32)back.width, (S32)back.height);
            }
            else
            {
                window["back_buffer"] = pair(gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw());
            }
            window["system_ui_size"] = gViewerWindow->getWindow()->getSystemUISize();
            window["ui_scale"] = llsd::array(LLUI::getScaleFactor().mV[VX], LLUI::getScaleFactor().mV[VY]);
            const LLImageGL::SyncCounts sync_counts = LLImageGL::syncCounts();
            measured["image_gl"] = llsd::map("deferred_swaps", (S32)sync_counts.deferredSwaps,
                                             "handle_swaps", (S32)sync_counts.handleSwaps,
                                             "swaps", (S32)sync_counts.swaps);
            measured["rhi"]["geometry_shaders"] = device && device->capabilities().geometryShaders;
        }
        return profile;
    }

    bool writeProfile(const std::string& path, const LLSD& profile, std::string& why)
    {
        llofstream file(path.c_str());
        file << boost::json::serialize(LlsdToJson(profile)) << "\n";
        file.close();
        if (file.fail())
        {
            why = "could not write " + path;
            return false;
        }
        LL_INFOS("Golden") << "Golden test: render profile " << path << LL_ENDL;
        return true;
    }

    bool writeRenderProfile(const std::string& path, std::string& why)
    {
        const bool login = sState.phase == Phase::LoginScreen || sState.phase == Phase::MetalLogin;
        sState.profile = login ? loginProfile() : renderProfile();
        return writeProfile(path, sState.profile, why);
    }

    // [Viewer Lab] Golden's transfer checks: after the snapshot, in the next main-loop idle, so inside a device
    // frame and outside display(), as LEAP's saveSnapshot runs. rawSnapshot draws a new frame with gSnapshot and skips the
    // hero probes; at the window's size its image must still equal the presented frame (level2.py fails the run
    // otherwise). simpleSnapshot's difference is only reported, and tests/golden/level2/transfers.py
    // holds raw-depth and raw-large against a base run.

    std::string md5Of(const U8* data, size_t bytes)
    {
        LLMD5 md5;
        md5.update(data, bytes);
        md5.finalize();
        char hex[33];
        md5.hex_digest(hex);
        return hex;
    }

    /// The scene monitor's check (monitor_check), at the snapshot, before anything else draws: its last capture
    /// target read back through the readback facade (LLGPUReadback) as RGB, the target's format. measured.monitor:
    /// the warm-up frames with a capture, the target's size and the MD5 of its pixels, an empty string when there is no
    /// target or the read failed. False after fail(): the monitor is off.
    bool monitorCheck()
    {
        if (!LLSceneMonitor::getInstance()->isEnabled())
        {
            fail("the scene monitor is off; monitor_check needs SceneLoadingMonitorEnabled");
            return false;
        }
        LLSD& result = sState.monitor;
        result = LLSD::emptyMap();
        result["captures"] = S32(sState.monitorCaptures);
        result["size"] = llsd::array(0, 0);
        result["checksum"] = std::string();
        const LLRenderTarget* target = LLSceneMonitor::getInstance()->getLastCaptureTarget();
        if (!target)
        {
            LL_WARNS("Golden") << "Golden test: the scene monitor has no capture" << LL_ENDL;
            return true;
        }
        const S32 width = S32(target->getWidth());
        const S32 height = S32(target->getHeight());
        std::vector<U8> pixels(size_t(width) * size_t(height) * 3);
        LL::RHI::Texture* texture = target->getRHITexture(0);
        gGL.getTexUnit(0)->bindTexture(LLTexUnit::TT_TEXTURE, texture);
        const bool read = texture && LLGPUReadback::readTexImage(texture, GL_TEXTURE_2D, 0, width, height, GL_RGB,
                                                                 GL_UNSIGNED_BYTE, pixels.data(), pixels.size());
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        result["size"] = llsd::array(width, height);
        result["checksum"] = read ? md5Of(pixels.data(), pixels.size()) : std::string();
        LL_INFOS("Golden") << "Golden test: scene monitor: " << sState.monitorCaptures << " captures over the warm-up, the last "
                           << width << "x" << height << " " << (read ? result["checksum"].asString() : "not read") << LL_ENDL;
        return true;
    }

    /// The back buffer copy's check (back_buffer_copy_check): the copy's pixels on GL, which no unit test fixture has
    /// a window for. In the snapshot's frame, after saveFrame, while the back buffer still holds the presented frame:
    /// a capture target of the window's size, made as LLSceneMonitor::getCaptureTarget makes its captures (GL_RGB, no
    /// depth), gets the back buffer through copyFromBackBuffer while a scratch target cleared to (1, 0, 1, 1) is bound, so
    /// a copy that reads the bound framebuffer instead of the window's shows magenta. The copy is read back through the
    /// device as readFrame reads the window, and its RGB bytes are compared with the presented frame's. Both targets are
    /// released before the transfer checks. measured.back_buffer_copy: result (the copy was read back), copied
    /// (copyFromBackBuffer's result), the differing bytes and the largest channel delta, -1 when not read; level2.py
    /// fails the run unless result, copied and 0 differing bytes.
    LLSD backBufferCopyCheck()
    {
        LLSD result;
        result["result"] = false;
        result["copied"] = false;
        result["differing_bytes"] = -1;
        result["max_delta"] = -1;
        const LLImageRaw* frame = sState.presented.get();
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        if (!frame || !frame->getData() || frame->getComponents() != 3 || !device)
        {
            LL_WARNS("Golden") << "Golden test: back buffer copy check without the presented frame or the device" << LL_ENDL;
            return result;
        }
        const S32 width = frame->getWidth();
        const S32 height = frame->getHeight();
        LLRenderTarget capture; // as the scene monitor's
        LLRenderTarget scratch; // bound during the copy
        if (!capture.allocate(U32(width), U32(height), GL_RGB) || !scratch.allocate(U32(width), U32(height), GL_RGBA))
        {
            LL_WARNS("Golden") << "Golden test: back buffer copy check: the targets could not be made" << LL_ENDL;
            capture.release();
            scratch.release();
            return result;
        }
        scratch.bindTarget();
        LLRenderTarget::setClearColor(1.f, 0.f, 1.f, 1.f);
        scratch.clear(GL_COLOR_BUFFER_BIT);
        LLRenderTarget::setClearColor(0.f, 0.f, 0.f, 0.f); // the colour the viewer's own clears leave
        gGL.getTexUnit(0)->bind(&capture);
        const bool copied = capture.copyFromBackBuffer();
        scratch.flush();
        LL::RHI::Texture* texture = capture.getRHITexture(0);
        LL::RHI::Ref<LL::RHI::Readback> readback;
        if (texture)
        {
            readback = device->encoder().readback(texture, 0, 0, { 0, 0, U32(width), U32(height) }, LL::RHI::PixelLayout());
        }
        if (readback.notNull() && !readback->ready())
        {
            readback->wait();
        }
        const size_t pixels = size_t(width) * size_t(height);
        const bool read = readback.notNull() && readback->data().size() == pixels * 4;
        S32 differing = 0;
        S32 max_delta = 0;
        if (read)
        {
            const U8* rgba = reinterpret_cast<const U8*>(readback->data().data());
            const U8* rgb = frame->getData();
            for (size_t pixel = 0; pixel < pixels; ++pixel)
            {
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    const S32 delta = std::abs(S32(rgba[pixel * 4 + channel]) - S32(rgb[pixel * 3 + channel]));
                    if (delta > 0)
                    {
                        ++differing;
                        max_delta = llmax(max_delta, delta);
                    }
                }
            }
        }
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        capture.release();
        scratch.release();
        result["result"] = read;
        result["copied"] = copied;
        result["differing_bytes"] = read ? differing : -1;
        result["max_delta"] = read ? max_delta : -1;
        LL_INFOS("Golden") << "Golden test: back buffer copy: " << width << "x" << height << ", "
                           << (copied ? "copied" : "not copied") << ", "
                           << (read ? llformat("%d of %llu bytes differ, max delta %d", differing,
                                               (unsigned long long)(pixels * 3), max_delta)
                                    : std::string("not read"))
                           << LL_ENDL;
        return result;
    }

    /// One snapshot check: the asked size, the size and MD5 of the pixel bytes, the PNG next to the snapshot and, with
    /// compare, the pixels that differ from the presented frame and the largest channel delta.
    LLSD snapshotCheck(const std::string& name, LLImageRaw* raw, bool taken, S32 width, S32 height, bool compare,
                       std::vector<std::string>& failures)
    {
        LLSD entry;
        entry["asked"] = llsd::array(width, height);
        entry["taken"] = taken;
        if (!taken || raw->isBufferInvalid() || !raw->getData() || raw->getWidth() < 1 || raw->getHeight() < 1)
        {
            failures.push_back(name + " failed");
            return entry;
        }
        const S32 components = raw->getComponents();
        const size_t pixels = size_t(raw->getWidth()) * size_t(raw->getHeight());
        entry["size"] = llsd::array(S32(raw->getWidth()), S32(raw->getHeight()));
        entry["components"] = components;
        entry["md5"] = md5Of(raw->getData(), pixels * size_t(components));
        const std::string config = sState.config["transfers"].asString();
        const std::string path = config.substr(0, config.size() - TRANSFERS_SUFFIX.size()) + "." + name + ".png";
        LLPointer<LLImagePNG> png = new LLImagePNG();
        if (!png->encode(raw, 0.f) || !png->save(path))
        {
            failures.push_back("could not write " + path);
        }
        else
        {
            entry["file"] = path;
        }
        const LLImageRaw* frame = sState.presented.get();
        if (compare && frame && frame->getWidth() == raw->getWidth() && frame->getHeight() == raw->getHeight()
            && frame->getComponents() == components)
        {
            S32 differing = 0;
            S32 most = 0;
            const U8* a = frame->getData();
            const U8* b = raw->getData();
            for (size_t pixel = 0; pixel < pixels; ++pixel)
            {
                S32 delta = 0;
                for (S32 c = 0; c < components; ++c)
                {
                    delta = llmax(delta, std::abs(S32(a[pixel * components + c]) - S32(b[pixel * components + c])));
                }
                differing += delta > 0 ? 1 : 0;
                most = llmax(most, delta);
            }
            entry["differing_pixels"] = differing;
            entry["max_channel_delta"] = most;
        }
        LL_INFOS("Golden") << "Golden test: transfer check " << name << " " << raw->getWidth() << "x" << raw->getHeight()
                           << " md5 " << entry["md5"].asString() << LL_ENDL;
        return entry;
    }

    /// readBackRaw of up to MAX_READBACK_TEXTURES textures in use, as the readiness check enumerates them: 2D with a
    /// texture on the current backend (LLImageGL::hasTexture: off GL it has no GL name), 1 to 4 components and at most
    /// MAX_READBACK_SIZE on a side at the current discard, sorted by id.
    LLSD textureChecks(LLSD& result, std::vector<std::string>& failures)
    {
        std::vector<LLViewerFetchedTexture*> qualifying;
        for (const LLPointer<LLViewerFetchedTexture>& entry : gTextureList)
        {
            LLViewerFetchedTexture* image = entry.get(); // the list's entries are const, the textures not
            if (image->getLastReferencedTimer()->getElapsedTimeF32() >= IN_USE_SECONDS || image->getDesiredDiscardLevel() < 0
                || image->isMissingAsset() || image->getDiscardLevel() < 0)
            {
                continue; // not in use, or no data
            }
            const LLImageGL* gl = image->getGLTexture();
            if (!gl || gl->getTexTarget() != GL_TEXTURE_2D || !gl->hasTexture() || gl->getComponents() < 1
                || gl->getComponents() > 4 || gl->getWidth() > MAX_READBACK_SIZE || gl->getHeight() > MAX_READBACK_SIZE)
            {
                continue;
            }
            qualifying.push_back(image);
        }
        std::sort(qualifying.begin(), qualifying.end(),
                  [](LLViewerFetchedTexture* a, LLViewerFetchedTexture* b) { return a->getID() < b->getID(); });
        result["qualifying_textures"] = S32(qualifying.size());
        if (qualifying.empty())
        {
            failures.push_back("no texture in use qualifies for readBackRaw");
        }
        LLSD textures = LLSD::emptyArray();
        S32 read = 0;
        for (size_t i = 0; i < qualifying.size() && i < MAX_READBACK_TEXTURES; ++i)
        {
            const LLImageGL* gl = qualifying[i]->getGLTexture();
            LLSD entry;
            entry["id"] = qualifying[i]->getID();
            entry["size"] = llsd::array(gl->getWidth(), gl->getHeight());
            entry["components"] = S32(gl->getComponents());
            entry["discard"] = gl->getDiscardLevel();
            LLPointer<LLImageRaw> raw = new LLImageRaw();
            // This thread's, which no other thread's readback moves
            const LLGPUReadback::Counts before = LLGPUReadback::threadCounts();
            const bool success = gl->readBackRaw(-1, raw, false);
            const LLGPUReadback::Counts after = LLGPUReadback::threadCounts();
            entry["success"] = success;
            // The readback facade's calls of this readBackRaw, which returns one bool for failures before, at and after
            // its read: level2.py holds a successful one to exactly one tracked or untracked call
            LLSD& facade = entry["facade"];
            facade["tracked"] = S32(after.tracked - before.tracked);
            facade["untracked"] = S32(after.untracked - before.untracked);
            facade["outside"] = S32(after.outside - before.outside);
            facade["refused"] = S32(after.refused - before.refused);
            if (success && raw->getData())
            {
                entry["md5"] = md5Of(raw->getData(), size_t(raw->getDataSize()));
                ++read;
            }
            textures.append(entry);
        }
        LL_INFOS("Golden") << "Golden test: transfer check readBackRaw read " << read << " of " << textures.size()
                           << " textures (" << qualifying.size() << " qualify)" << LL_ENDL;
        return textures;
    }

    /// LLGPUReadback's and LLGPUCopy's counts now, as <facade>.<count> under one point of the transfers file's counts:
    /// level2.py fails a run with refused or outside above 0 and prints the calls per warm-up frame.
    LLSD facadeCounts()
    {
        LLSD counts;
        const auto add = [&counts](const char* facade, const LLGPUReadback::Counts& c)
        {
            LLSD& entry = counts[facade];
            entry["tracked"] = (S32)c.tracked;
            entry["untracked"] = (S32)c.untracked;
            entry["outside"] = (S32)c.outside;
            entry["refused"] = (S32)c.refused;
        };
        add("readback", LLGPUReadback::counts());
        add("copy", LLGPUCopy::counts());
        return counts;
    }

    // [Viewer Lab] Golden's paint map check (paintmap_check): the bake of Develop > Terrain > Create Local Paintmap
    // (LLAdvancedTerrainCreateLocalPaintMap) into a texture of the check's own, which nothing draws, so the terrain and the
    // snapshot stay as they were; its levels read back through the readback facade, level 0 for the bake's draw and copy,
    // level 1 and the last level for its glGenerateMipmap.

    /// The paint map check's result, measured.paintmap: the bake's result, the levels read and their MD5s, an empty
    /// string for a level that could not be read.
    LLSD paintmapCheck()
    {
        LLSD result;
        result["result"] = false;
        result["levels"] = LLSD::emptyArray();
        result["checksums"] = LLSD::emptyArray();
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            LL_WARNS("Golden") << "Golden test: paint map check without a region" << LL_ENDL;
            return result;
        }
        // The menu item's size: TerrainPaintResolution as a power of two, at most RenderMaxTextureResolution.
        U16 dim = (U16)gSavedSettings.getU32("TerrainPaintResolution");
        const U32 max_resolution = gSavedSettings.getU32("RenderMaxTextureResolution");
        dim = llclamp(dim, 16, max_resolution);
        dim = 1 << U32(std::ceil(std::log2(dim)));
        LLPointer<LLImageRaw> image_raw = new LLImageRaw(dim, dim, 3);
        LLPointer<LLViewerTexture> tex = LLViewerTextureManager::getLocalTexture(image_raw.get(), true);
        const bool baked = LLTerrainPaintMap::bakeHeightNoiseIntoPBRPaintMapRGB(*region, *tex);
        result["result"] = baked;
        LLImageGL* image = tex->getGLTexture();
        // The last level of the device texture, its description's: the device texture has exactly the
        // levels LLImageGL describes, at most MAX_DISCARD_LEVEL below level 0, on both backends (GL's
        // GL_TEXTURE_MAX_LEVEL stops glGenerateMipmap there), so log2(dim) is no level on either;
        // without a device texture, log2(dim)
        const LL::RHI::Texture* device_texture = image ? image->getRHITexture() : nullptr;
        const U32 device_levels = device_texture ? device_texture->desc().mipLevels : 0;
        const S32 last = device_levels > 1 ? S32(device_levels) - 1 : S32(std::log2(dim));
        result["size"] = S32(dim);
        std::string line;
        for (const S32 level : { 0, 1, last })
        {
            const S32 size = llmax(S32(dim) >> level, 1);
            std::vector<U8> pixels(size_t(size) * size_t(size) * 3);
            gGL.getTexUnit(0)->bind(tex);
            const bool read = image && LLGPUReadback::readTexImage(image->getRHITexture(), GL_TEXTURE_2D, level, size, size,
                                                                   GL_RGB, GL_UNSIGNED_BYTE, pixels.data(), pixels.size());
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            const std::string md5 = read ? md5Of(pixels.data(), pixels.size()) : std::string();
            result["levels"].append(level);
            result["checksums"].append(md5);
            line += llformat(" level %d (%dx%d) %s", level, size, size, read ? md5.c_str() : "not read");
        }
        LL_INFOS("Golden") << "Golden test: paint map check: bake " << (baked ? "done" : "failed") << ", " << dim << "x"
                           << dim << ";" << line << LL_ENDL;
        return result;
    }

    // [Viewer Lab] Golden's own-avatar GPU times (avatar_gpu): after the snapshot and the checks,
    // so no compared pixel changes, LLPipeline::profileAvatar times the own avatar as auto-tune and the Performance
    // floater do, on whichever backend runs; tests/golden/level2/avatar-times.py sets GL's and Metal's side by side.

    const U32 AVATAR_PROFILES = 8;         ///< the whole-avatar profiles, each after the last one's result
    const U32 AVATAR_WAIT_IDLES = 300;     ///< the most idles one result may take before the run fails
    const S32 AVATAR_EMPTY_SPAN_TRIES = 5; ///< the empty span is read as readProfileQuery(5) reads: 5 tries, then waits

    // LLVOAvatar::mGPUProfilePending, which only LLPipeline (a friend) may read: true while the avatar's profile query
    // waits for its result (readProfileQuery's retries on the main loop's queue), when another profileAvatar would
    // begin the query again before its end. The hook reads it through the member pointer an explicit instantiation
    // names, where access checks do not apply ([temp.explicit]), so no Linden header changes for the test (the
    // sphere's flag is its only hook in Linden code); a renamed member fails the build.
    struct ProfilePendingTag
    {
        using type = bool LLVOAvatar::*;
        friend type profilePendingMember(ProfilePendingTag);
    };
    template <typename Tag, typename Tag::type Member>
    struct PrivateMember
    {
        friend typename Tag::type profilePendingMember(Tag) { return Member; }
    };
    template struct PrivateMember<ProfilePendingTag, &LLVOAvatar::mGPUProfilePending>;

    bool profilePending(const LLVOAvatar& avatar)
    {
        return avatar.*profilePendingMember(ProfilePendingTag());
    }

    /// Whether profileAvatar may run on the own avatar. Its llassert(!isTooSlow()) is compiled only with SHOW_ASSERT,
    /// and in world runs RenderAvatarFriendsOnly (level2.py's RUN_SETTINGS) makes isTooSlow() true for the own avatar,
    /// which is not its own friend. Nothing else of a profile reads it: generateImpostor ignores it for a profile, the
    /// own avatar is always drawn normally (getOverallAppearance), and the ART limit decides nothing for it here.
    bool mayProfile(LLVOAvatar* self)
    {
#ifdef SHOW_ASSERT
        return !self->isTooSlow(); // a build with asserts would stop in profileAvatar otherwise
#else
        return true;
#endif
    }

    /// LLPipeline::profileAvatar on the own avatar with gGoldenDrawUnloadedSelf set for the call only: the draws are
    /// inside it (generateImpostor), while the result of a whole-avatar profile comes in later idles.
    void profileOwnAvatar(LLVOAvatar* self, bool attachments)
    {
        gGoldenDrawUnloadedSelf = true;
        gPipeline.profileAvatar(self, attachments);
        gGoldenDrawUnloadedSelf = false;
    }

    /// The empty span: a timer query around profileAvatar's pass with no draw in it (the deferred
    /// screen bound and cleared before the begin, as profileAvatar does, and flushed before the end), so its time is
    /// the cost the whole-avatar times carry besides the avatar's draws. On Metal that is the G-buffer's clear load and
    /// store, which open and close the pass's one encoder inside the span (the clear is a load action, so the first
    /// draw, or here the pass's end, opens it), and the stamps' own cost; ending the query before the flush
    /// would measure the stamps alone. On GL the flush only unbinds the framebuffer; what the driver
    /// does there is its own, and GL's times are information.
    void beginEmptySpan()
    {
        sState.avatarEmptySpan = LLGPUQuery::create(GL_TIME_ELAPSED);
        LLGLSLShader* cur_shader = LLGLSLShader::sCurBoundShaderPtr;
        gPipeline.mRT->deferredScreen.bindTarget();
        gPipeline.mRT->deferredScreen.clear();
        const bool begun = sState.avatarEmptySpan.begin(GL_TIME_ELAPSED);
        gPipeline.mRT->deferredScreen.flush();
        if (begun)
        {
            sState.avatarEmptySpan.end(GL_TIME_ELAPSED);
        }
        else
        {
            sState.avatarEmptySpan.release(); // refused: no span, 0 ms
        }
        if (cur_shader)
        {
            cur_shader->bind();
        }
    }

    /// measured.avatar_gpu from the times read, written into the render profile, which the hook never collects again
    /// (renderProfile() would collect the settings again); then the run ends as the snapshot step
    /// would have ended it.
    void finishAvatarTimes(F32 empty_span_ms)
    {
        std::vector<F32> sorted = sState.avatarTimes;
        std::sort(sorted.begin(), sorted.end());
        const size_t n = sorted.size();
        const F32 median = n == 0 ? 0.f : (n % 2 ? sorted[n / 2] : 0.5f * (sorted[n / 2 - 1] + sorted[n / 2]));
        const F32 spread = n == 0 ? 0.f : sorted.back() - sorted.front();
        // The ART limit in force (0: none) and llvoavatar.cpp's updateTooSlow rule on the median: too slow when a limit
        // is on and the time reaches it.
        const U64 max_art_ns = LLPerfStats::renderAvatarMaxART_ns;
        const F32 max_art_ms = F32(max_art_ns / 1000000.0);
        LLSD result;
        result["median_ms"] = median;
        result["spread_ms"] = spread;
        result["times_ms"] = LLSD::emptyArray();
        for (const F32 time : sState.avatarTimes)
        {
            result["times_ms"].append(time);
        }
        result["attachments"] = S32(sState.avatarAttachmentTimes.size());
        result["attachment_ms"] = sState.avatarAttachmentTimes;
        result["max_art_ms"] = max_art_ms;
        result["too_slow"] = max_art_ns > 0 && median >= max_art_ms;
        result["empty_span_ms"] = empty_span_ms;
        result["timers"] = LLGPUQuery::supported(GL_TIME_ELAPSED);
        // No whole-avatar profile ran (no timer queries, or mayProfile false in a build with
        // asserts), so the zeros above are no result.
        result["profiled"] = n > 0;
        LL_INFOS("Golden") << "Golden test: own avatar GPU times: median " << median << " ms, spread " << spread
                           << " ms over " << n << ", empty span " << empty_span_ms << " ms, "
                           << sState.avatarAttachmentTimes.size() << " attachments, ART limit " << max_art_ms
                           << " ms, too slow " << result["too_slow"].asBoolean() << ", timers "
                           << result["timers"].asBoolean() << ", profiled " << result["profiled"].asBoolean() << LL_ENDL;
        sState.profile["measured"]["avatar_gpu"] = result;
        // The per-run gated GL check also sees a gated GL call reached only in the avatar's draws; only
        // this key is collected again (measured is not compared, so no compared key changes).
        sState.profile["measured"]["gl_gated"] = gatedGLCalls();
        std::string why;
        if (!writeProfile(sState.config["render_profile"].asString(), sState.profile, why))
        {
            fail(why);
            return;
        }
        finishScene();
    }

    /// One idle of the own avatar's timing; true when it is done (the run ended). Each whole-avatar profile waits until
    /// the last one's result was read; profileAvatar(self, true) reads each attachment's time before it returns.
    bool stepAvatarTimes()
    {
        if (sState.phase != Phase::Transfers)
        {
            return true; // the run ended meanwhile
        }
        if (!isAgentAvatarValid())
        {
            fail("the own avatar went away while the hook timed it");
            return true;
        }
        LLVOAvatar* self = gAgentAvatarp;
        // Each profileOwnAvatar waits while a profile is pending, the hook's own or one the floater or auto-tune began,
        // so profileAvatar never begins the query again before its end.
        if (profilePending(*self))
        {
            if (++sState.avatarWaitIdles > AVATAR_WAIT_IDLES)
            {
                fail(llformat("the own avatar's profile %u had no result after %u idles", sState.avatarProfiles,
                              AVATAR_WAIT_IDLES));
                return true;
            }
            return false;
        }
        sState.avatarWaitIdles = 0;
        if (sState.avatarTimes.size() < sState.avatarProfiles)
        {
            sState.avatarTimes.push_back(self->getGPURenderTime()); // the own avatar is never visually muted
        }
        if (sState.avatarProfiles < AVATAR_PROFILES && mayProfile(self))
        {
            profileOwnAvatar(self, false);
            ++sState.avatarProfiles;
            return false;
        }
        if (!sState.avatarAttachmentsTimed)
        {
            sState.avatarAttachmentsTimed = true;
            if (mayProfile(self))
            {
                profileOwnAvatar(self, true);
                for (const auto& [point, attachment] : self->mAttachmentPoints)
                {
                    for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
                    {
                        if (object)
                        {
                            sState.avatarAttachmentTimes.append(object->mGPURenderTime);
                        }
                    }
                }
            }
            beginEmptySpan();
            return false;
        }
        LLGPUQuery& span = sState.avatarEmptySpan;
        if (span && span.get(GL_QUERY_RESULT_AVAILABLE) != GL_TRUE
            && ++sState.avatarEmptySpanTries < AVATAR_EMPTY_SPAN_TRIES)
        {
            return false;
        }
        const F32 empty_span_ms = span ? F32(span.get(GL_QUERY_RESULT) / 1000000.0) : 0.f; // waits on the last try
        span.release();
        finishAvatarTimes(empty_span_ms);
        return true;
    }

    /// The own avatar's timing from the next idle on; it ends the run. Without timer queries profileAvatar does
    /// nothing, so the times stay empty and measured.avatar_gpu says timers false.
    void startAvatarTimes()
    {
        if (!LLGPUQuery::supported(GL_TIME_ELAPSED))
        {
            finishAvatarTimes(0.f);
            return;
        }
        doOnIdleRepeating(stepAvatarTimes);
    }

    /// The end after the snapshot (and the transfer checks): with paintmap_check the paint map check first, its result
    /// written into the render profile, and with avatar_gpu the own avatar's GPU times, which end the run; otherwise
    /// the run ends as the snapshot step would have ended it.
    void finishAfterChecks()
    {
        if (sState.config["paintmap_check"].asBoolean())
        {
            sState.profile["measured"]["paintmap"] = paintmapCheck();
            std::string why;
            if (!writeProfile(sState.config["render_profile"].asString(), sState.profile, why))
            {
                fail(why);
                return;
            }
        }
        if (sState.config["avatar_gpu"].asBoolean())
        {
            startAvatarTimes();
            return;
        }
        finishScene();
    }

    /// The transfer checks in their fixed order, written to the config's transfers file; then the run ends as the
    /// snapshot step would have ended it. A failed snapshot, or no texture to read back, fails the run.
    void runTransferChecks()
    {
        const auto [width, height] = windowPixels();
        std::vector<std::string> failures;
        LLSD result;
        LLSD& snapshots = result["snapshots"];
        // The counts before the first check, so each check's own readbacks are the difference to the point before it.
        sState.transferCounts["checks_start"] = facadeCounts();
        // (1) and (2): the window's back buffer, read row by row; (3) larger than the window, so through a scratch target.
        // Each check's own readbacks: the reads its snapshot marks as its own inside the check's scope, which neither
        // another thread's readback nor one of the frames the snapshot renders moves; level2.py holds them to one
        // tracked call per row read, one for simple.
        const auto ownReadbacks = [](const LLGPUReadback::CountScope& scope)
        {
            const LLGPUReadback::Counts& now = scope.counts();
            LLSD entry;
            entry["tracked"] = S32(now.tracked);
            entry["untracked"] = S32(now.untracked);
            entry["outside"] = S32(now.outside);
            entry["refused"] = S32(now.refused);
            return entry;
        };
        LLPointer<LLImageRaw> raw = new LLImageRaw();
        std::optional<LLGPUReadback::CountScope> own;
        own.emplace();
        bool taken = gViewerWindow->rawSnapshot(raw, width, height, true, false, false, false, false, false, false,
                                                LLSnapshotModel::SNAPSHOT_TYPE_COLOR);
        LLSD readbacks = ownReadbacks(*own);
        snapshots["raw"] = snapshotCheck("raw", raw, taken, width, height, true, failures);
        snapshots["raw"]["readbacks"] = readbacks;
        sState.transferCounts["raw"] = facadeCounts();
        raw = new LLImageRaw();
        own.emplace();
        taken = gViewerWindow->rawSnapshot(raw, width, height, true, false, false, false, false, false, false,
                                           LLSnapshotModel::SNAPSHOT_TYPE_DEPTH);
        readbacks = ownReadbacks(*own);
        snapshots["raw-depth"] = snapshotCheck("raw-depth", raw, taken, width, height, false, failures);
        snapshots["raw-depth"]["readbacks"] = readbacks;
        sState.transferCounts["raw-depth"] = facadeCounts();
        raw = new LLImageRaw();
        own.emplace();
        taken = gViewerWindow->rawSnapshot(raw, LARGE_SNAPSHOT_WIDTH, LARGE_SNAPSHOT_HEIGHT, true, false, false, false,
                                           false, false, false, LLSnapshotModel::SNAPSHOT_TYPE_COLOR);
        readbacks = ownReadbacks(*own);
        snapshots["raw-large"] = snapshotCheck("raw-large", raw, taken, LARGE_SNAPSHOT_WIDTH, LARGE_SNAPSHOT_HEIGHT, false,
                                               failures);
        snapshots["raw-large"]["readbacks"] = readbacks;
        sState.transferCounts["raw-large"] = facadeCounts();
        // (4) simpleSnapshot writes width * height * 3 bytes without sizing the image, as its one caller knows.
        raw = new LLImageRaw(U16(width), U16(height), 3);
        own.emplace();
        taken = gViewerWindow->simpleSnapshot(raw, width, height, 1);
        readbacks = ownReadbacks(*own);
        snapshots["simple"] = snapshotCheck("simple", raw, taken, width, height, true, failures);
        snapshots["simple"]["readbacks"] = readbacks;
        sState.transferCounts["simple"] = facadeCounts();
        // (5) readBackRaw of the scene's own textures, which no golden scene reaches otherwise.
        result["textures"] = textureChecks(result, failures);
        sState.transferCounts["textures"] = facadeCounts();
        result["counts"] = sState.transferCounts;
        sState.presented = nullptr;

        const std::string path = sState.config["transfers"].asString();
        llofstream file(path.c_str());
        file << boost::json::serialize(LlsdToJson(result)) << "\n";
        file.close();
        if (file.fail())
        {
            failures.push_back("could not write " + path);
        }
        else
        {
            LL_INFOS("Golden") << "Golden test: transfers " << path << LL_ENDL;
        }
        if (!failures.empty())
        {
            std::string why = "the transfer checks failed: " + failures[0];
            for (size_t i = 1; i < failures.size(); ++i)
            {
                why += "; " + failures[i];
            }
            fail(why);
            return;
        }
        finishAfterChecks();
    }

    /// The run's snapshot and render profile, then the transfer checks when level2.py asks for them, and the paint
    /// map check when the config asks for it, which end the run, or the end.
    /// The running stop of a tour: a fresh State with the launch's config and the stop's keys on top, its
    /// settings set, and Sizing on, so the window, the avatar's spot and the home, the camera, the environment lock, the
    /// terrain replay, the probes' restart and readiness, the warm-up and its counts come again as in a run's first
    /// scene. The fixed frame step stays on, so the test clock only runs forward; AFK is
    /// cleared. The first stop keeps the launch's counts, as a fresh run (no counts at its start).
    void beginStop()
    {
        const LLSD& stop = sTour.stops[sTour.stop];
        State fresh;
        fresh.config = sTour.launch;
        for (const auto& [key, value] : llsd::inMap(stop))
        {
            if (key != "name" && key != "settings")
            {
                fresh.config[key] = value;
            }
        }
        LLSD settings = sTour.launch["settings"].isMap() ? sTour.launch["settings"] : LLSD::emptyMap();
        for (const auto& [name, value] : llsd::inMap(stop["settings"]))
        {
            settings[name] = value;
        }
        fresh.config["settings"] = settings;
        fresh.benchmarkClass = sState.benchmarkClass;
        sState = std::move(fresh);
        sState.phase = Phase::Sizing;
        sState.frames = SIZING_FRAMES;
        tourMarker("tour stop " + tourStopName() + " begins");
        std::string why;
        for (const auto& [name, value] : llsd::inMap(stop["settings"]))
        {
            if (!setSetting(name, value, why))
            {
                fail("tour stop " + tourStopName() + ": " + why);
                return;
            }
        }
        gAgent.clearAFK();
        sTour.imageGLAtBegin = sTour.stop == 0 ? LLSD() : imageGLMeasures();
        sTour.metalUploadsAtBegin = sTour.stop == 0 ? LLSD() : metalUploadMeasures();
    }

    /// The tour's start, when the world is there: the baseline of every setting a stop sets, as the level has it
    /// after the login, then the first stop. False when the run failed.
    bool startTour()
    {
        sTour.baseline = LLSD::emptyMap();
        for (size_t i = 0; i < sTour.stops.size(); ++i)
        {
            for (const auto& [name, value] : llsd::inMap(sTour.stops[i]["settings"]))
            {
                LLControlVariable* control = gSavedSettings.getControl(name);
                if (!control)
                {
                    fail("tour: the stop setting " + name + " is not declared");
                    return false;
                }
                sTour.baseline[name] = typedValue(control, control->getValue());
            }
        }
        sTour.stop = 0;
        sTour.probes = probeStatus();
        sTour.anchorHome = LLGoldenHome();
        LL_INFOS("Golden") << "Golden test: tour of " << sTour.stops.size() << " stops, baseline " << sTour.baseline
                           << LL_ENDL;
        beginStop();
        return sState.phase != Phase::Done;
    }

    /// A scene's results are written: the end of the run, or in a tour the stop's unwind and the next stop.
    /// The unwind sets the stop's settings back to the baseline, and the audit then holds every setting a stop sets to
    /// it ("tour unwind" fails the run). The last stop has no unwind: nothing runs after it.
    void finishScene()
    {
        if (!sTour.active())
        {
            finish(sState.config["quit"].asBoolean());
            return;
        }
        if (sTour.stop + 1 < sTour.stops.size())
        {
            std::string why;
            for (const auto& [name, value] : llsd::inMap(sTour.stops[sTour.stop]["settings"]))
            {
                if (!setSetting(name, sTour.baseline[name], why))
                {
                    fail("tour unwind after stop " + tourStopName() + ": " + why);
                    return;
                }
            }
            for (const auto& [name, value] : llsd::inMap(sTour.baseline))
            {
                LLControlVariable* control = gSavedSettings.getControl(name);
                const LLSD now = typedValue(control, control->getValue());
                if (!llsd_equals(now, value))
                {
                    fail("tour unwind after stop " + tourStopName() + ": " + name + " is " + now.asString() +
                         ", not the baseline's " + value.asString());
                    return;
                }
            }
            // [Viewer Lab] The probe updates as a fresh run's scene starts them, running: a pause the stop left
            // ends here, since with the probes off at the stop nothing ended it.
            // The audit then holds the probes on or off as at the tour's start, and not paused.
            if (gPipeline.mReflectionMapManager.isPaused())
            {
                gPipeline.mReflectionMapManager.resume();
                LL_INFOS("Golden") << "Golden test: tour unwind after stop " << tourStopName()
                                   << ": the reflection probe updates were paused; resumed" << LL_ENDL;
            }
            const std::string probes = ll_golden_probes_unwind_problem(sTour.probes, probeStatus());
            if (!probes.empty())
            {
                fail("tour unwind after stop " + tourStopName() + ": " + probes);
                return;
            }
        }
        tourMarker("tour stop " + tourStopName() + " done");
        if (++sTour.stop < sTour.stops.size())
        {
            beginStop();
            return;
        }
        tourMarker("tour done");
        finish(sState.config["quit"].asBoolean());
    }

    void snapshotAndFinish()
    {
        std::string why;
        sState.transferCounts["snapshot"] = facadeCounts(); // the warm-up's end, before the snapshot
        if (!saveFrame(sState.config["snapshot"].asString(), why, sState.presented))
        {
            fail(why);
            return;
        }
        // The scene monitor's last capture is this frame's, until the transfer checks draw others.
        if (sState.config["monitor_check"].asBoolean() && !monitorCheck())
        {
            return;
        }
        // The back buffer still holds the presented frame, which the copy must equal.
        if (sState.config["back_buffer_copy_check"].asBoolean())
        {
            sState.backBufferCopy = backBufferCopyCheck();
        }
        if (!writeRenderProfile(sState.config["render_profile"].asString(), why))
        {
            fail(why);
            return;
        }
        // The transfer checks, when level2.py asks for them, in the next idle; they end the run.
        if (sState.config["transfers"].isString())
        {
            sState.phase = Phase::Transfers;
            doOnIdleOneTime(runTransferChecks);
            return;
        }
        sState.presented = nullptr;
        // The paint map check, outside display() as the menu item's bake, and the own avatar's GPU times, outside it as
        // auto-tune's profileAvatar, in the next idle.
        if (sState.config["paintmap_check"].asBoolean() || sState.config["avatar_gpu"].asBoolean())
        {
            sState.phase = Phase::Transfers;
            doOnIdleOneTime(finishAfterChecks);
            return;
        }
        finishScene();
    }

    // [Viewer Lab] The preview run (previews): after the warm-up, on the test clock, each step gets a fixed
    // number of frames, so that the world behind its floater is the same frame of the scene in every run; at the step's
    // last frame its preview must be there, or the run fails.

    /// The dynamic textures, as LLViewerDynamicTexture::updateAllInstances renders them: the class keeps its lists
    /// protected, so they are read through a derived name, of which nothing is ever made.
    struct DynamicTextureList : LLViewerDynamicTexture
    {
        static std::vector<LLViewerDynamicTexture*> all()
        {
            std::vector<LLViewerDynamicTexture*> textures;
            for (const instance_list_t& list : sInstances)
            {
                textures.insert(textures.end(), list.begin(), list.end());
            }
            return textures;
        }
    };

    /// The preview textures of one of the preview run's floaters, by the names its results give them, sorted.
    std::vector<std::pair<std::string, LLViewerDynamicTexture*>> previewTextures(const std::string& floater)
    {
        std::vector<std::pair<std::string, LLViewerDynamicTexture*>> textures;
        for (LLViewerDynamicTexture* texture : DynamicTextureList::all())
        {
            const S8 type = texture->getType();
            if (floater == "upload_image" && type == LLViewerDynamicTexture::LL_IMAGE_PREVIEW_AVATAR)
            {
                textures.emplace_back("avatar", texture);
            }
            else if (floater == "upload_image" && type == LLViewerDynamicTexture::LL_IMAGE_PREVIEW_SCULPTED)
            {
                textures.emplace_back("sculpted", texture);
            }
            else if (floater == "upload_model" && dynamic_cast<LLModelPreview*>(texture))
            {
                textures.emplace_back("model", texture);
            }
            else if (floater == "upload_anim_bvh" && type == LLViewerDynamicTexture::LL_PREVIEW_ANIMATION)
            {
                textures.emplace_back("animation", texture);
            }
        }
        std::sort(textures.begin(), textures.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        return textures;
    }

    /// The children the steps keep hidden (hide), hidden again for the next frame: the floater's
    /// own code shows one again when it updates (LLModelPreview::updateLodControls shows the file field of a LOD loaded
    /// from a file). Empty, or the first that was shown in the frame just drawn.
    std::string hidePreviewChildren()
    {
        std::string shown;
        for (const auto& [floater_name, child] : sState.previewHidden)
        {
            LLFloater* floater = sState.previewFloaters[floater_name].get();
            LLView* view = floater ? floater->findChildView(child) : nullptr;
            if (!view || view->getVisible())
            {
                if (shown.empty())
                {
                    shown = "the floater " + floater_name + (view ? " showed " : " lost ") + child + ", which the run hides";
                }
                if (view)
                {
                    view->setVisible(false);
                }
            }
        }
        return shown;
    }

    /// The step's action, at its first frame: its floater opened with its file at its rect, as a user's file choice
    /// would open it (no file picker), or the open one changed as a user would change it. False after a failure.
    bool startPreviewStep(const LLSD& step)
    {
        const std::string name = step["floater"].asString();
        LLFloater* floater = nullptr;
        if (step.has("file"))
        {
            const std::string file = step["file"].asString();
            if (name == "upload_model")
            {
                // LLFloaterModelPreview::showModelPreview's floater, given the file its picker would return.
                LLFloaterModelPreview* model = dynamic_cast<LLFloaterModelPreview*>(LLFloaterReg::getInstance(name));
                if (!model)
                {
                    fail("there is no floater " + name);
                    return false;
                }
                model->loadModel(LLModel::LOD_HIGH, file); // opens the floater
                floater = model;
            }
            else
            {
                // As upload_single_file opens it. The image preview's avatar would take its pose from motions that
                // follow the wall clock, and its eyes draw random numbers between the own avatar's: the avatars the
                // floater makes are held still from the start.
                const std::set<LLCharacter*> before(LLCharacter::sInstances.begin(), LLCharacter::sInstances.end());
                LLSD key;
                key["filename"] = file;
                floater = LLFloaterReg::showInstance(name, key, false);
                for (LLCharacter* character : LLCharacter::sInstances)
                {
                    LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
                    if (name == "upload_image" && !before.count(character) && avatar && avatar->isUIAvatar())
                    {
                        sState.previewPauses.push_back(avatar->requestPause());
                    }
                }
            }
            if (!floater)
            {
                fail("there is no floater " + name);
                return false;
            }
            const LLSD& r = step["rect"];
            const LLRect rect(r[0].asInteger(), r[1].asInteger(), r[2].asInteger(), r[3].asInteger());
            if (rect.getWidth() != floater->getRect().getWidth() || rect.getHeight() != floater->getRect().getHeight())
            {
                fail(llformat("the floater %s is %dx%d, not the rect's %dx%d", name.c_str(), floater->getRect().getWidth(),
                              floater->getRect().getHeight(), rect.getWidth(), rect.getHeight()));
                return false;
            }
            // As a user's move (openFloaters).
            floater->setShape(rect, true);
            sState.previewFloaters[name] = floater->getHandle();
            sState.previewRects[name] = rect;
            LL_INFOS("Golden") << "Golden test: preview floater " << name << " with " << file << " at " << floater->getRect()
                               << LL_ENDL;
        }
        floater = sState.previewFloaters[name].get();
        if (!floater || !floater->getVisible())
        {
            fail("the preview floater " + name + " is not open");
            return false;
        }
        if (step.has("combo"))
        {
            LLComboBox* combo = floater->findChild<LLComboBox>(step["combo"][0].asString());
            if (!combo || !combo->setCurrentByIndex(step["combo"][1].asInteger()))
            {
                fail("the floater " + name + " has no " + step["combo"][0].asString() + " with that item");
                return false;
            }
            combo->onCommit();
        }
        for (const LLSD& child : llsd::inArray(step["check"]))
        {
            LLCheckBoxCtrl* box = floater->findChild<LLCheckBoxCtrl>(child.asString());
            if (!box)
            {
                fail("the floater " + name + " has no check box " + child.asString());
                return false;
            }
            if (!box->get())
            {
                box->set(true);
                box->onCommit();
            }
        }
        for (const LLSD& child : llsd::inArray(step["click"]))
        {
            LLButton* button = floater->findChild<LLButton>(child.asString());
            if (!button)
            {
                fail("the floater " + name + " has no button " + child.asString());
                return false;
            }
            button->onCommit();
        }
        if (step.has("physics"))
        {
            // The physics from a file, as the physics tab's browse button loads it with the file its picker returns.
            static_cast<LLFloaterModelPreview*>(floater)->loadModel(LLModel::LOD_PHYSICS, step["physics"].asString());
        }
        // Children whose pixels would depend on the machine, as the model floater's file field with the file's absolute
        // path, kept hidden from now on.
        for (const LLSD& child : llsd::inArray(step["hide"]))
        {
            if (!floater->findChildView(child.asString()))
            {
                fail("the floater " + name + " has no " + child.asString() + " to hide");
                return false;
            }
            sState.previewHidden.emplace_back(name, child.asString());
            LL_INFOS("Golden") << "Golden test: " << child.asString() << " hidden in " << name << LL_ENDL;
        }
        hidePreviewChildren();
        LL_INFOS("Golden") << "Golden test: preview step " << step["name"].asString() << LL_ENDL;
        return true;
    }

    /// Why the step's floater, at its last frame, does not show what the step asked for yet, or empty: the floater not
    /// at its rect, a preview texture missing or not rendered since it last changed, the model loading or generating its
    /// levels of detail or, with physics, not showing a degenerate triangle, the animation not paused, the UI not as the
    /// config has it, or the assets not ready.
    std::string previewNotReady(const LLSD& step, LLFloater*& floater,
                                std::vector<std::pair<std::string, LLViewerDynamicTexture*>>& textures)
    {
        const std::string name = step["floater"].asString();
        floater = sState.previewFloaters[name].get();
        if (!floater || !floater->getVisible())
        {
            return "the floater " + name + " is not open";
        }
        if (floater->getRect() != sState.previewRects[name])
        {
            std::ostringstream why;
            why << "the floater " << name << " is at " << floater->getRect() << ", not " << sState.previewRects[name];
            return why.str();
        }
        textures = previewTextures(name);
        static const std::map<std::string, std::vector<std::string>> EXPECTED = {
            { "upload_image", { "avatar", "sculpted" } }, { "upload_model", { "model" } },
            { "upload_anim_bvh", { "animation" } }
        };
        std::vector<std::string> names;
        for (const auto& texture : textures)
        {
            names.push_back(texture.first);
        }
        if (names != EXPECTED.at(name))
        {
            return llformat("the floater %s has %d preview textures, not its own", name.c_str(), (S32)textures.size());
        }
        if (LLFloaterModelPreview* model = dynamic_cast<LLFloaterModelPreview*>(floater))
        {
            LLModelPreview* preview = dynamic_cast<LLModelPreview*>(textures[0].second);
            if (model->isModelLoading())
            {
                return "the model is still loading";
            }
            if (!preview->lodsReady())
            {
                return "the model's levels of detail are still being generated";
            }
            if (step.has("physics") && !preview->mHasDegenerate)
            {
                return "the model's physics shows no degenerate triangle";
            }
        }
        for (const auto& [texture_name, texture] : textures)
        {
            if (LLPreviewAnimation* animation = dynamic_cast<LLPreviewAnimation*>(texture))
            {
                // Renders every frame (LLFloaterBvhPreview::refresh), paused or not.
                if (!animation->getDummyAvatar()->areAnimationsPaused())
                {
                    return "the animation is not paused";
                }
            }
            else if (texture->needsRender())
            {
                return "the " + texture_name + " preview has not rendered its last change";
            }
        }
        std::string why = hidePreviewChildren();
        if (why.empty())
        {
            why = uiNotAsConfigured();
        }
        if (why.empty())
        {
            why = assetsNotReady();
        }
        return why;
    }

    /// An image saved next to the snapshot as <snapshot>.<suffix>.png, with its size and the MD5 of its pixel bytes.
    LLSD savePreviewImage(LLImageRaw* raw, const std::string& suffix, std::string& why)
    {
        const std::string snapshot = sState.config["snapshot"].asString();
        const std::string path = snapshot.substr(0, snapshot.size() - 4) + "." + suffix + ".png";
        LLSD entry;
        entry["size"] = llsd::array(S32(raw->getWidth()), S32(raw->getHeight()));
        entry["components"] = S32(raw->getComponents());
        entry["md5"] = md5Of(raw->getData(), size_t(raw->getWidth()) * size_t(raw->getHeight()) * size_t(raw->getComponents()));
        LLPointer<LLImagePNG> png = new LLImagePNG();
        if (!png->encode(raw, 0.f) || !png->save(path))
        {
            why = "could not write " + path;
            return entry;
        }
        entry["file"] = path;
        return entry;
    }

    /// The step's results at its last frame: the floater's part of the frame about to be presented, in the window's
    /// pixels, and readBackRaw of each of its preview textures. False with why.
    bool capturePreview(const LLSD& step, LLFloater* floater,
                        const std::vector<std::pair<std::string, LLViewerDynamicTexture*>>& textures, std::string& why)
    {
        const std::string name = step["name"].asString();
        LLPointer<LLImageRaw> frame;
        if (!readFrame(frame, why))
        {
            return false;
        }
        const LLRect ui = floater->calcScreenRect();
        const LLVector2& scale = LLUI::getScaleFactor();
        LLRect pixels(llfloor(F32(ui.mLeft) * scale.mV[VX]), llceil(F32(ui.mTop) * scale.mV[VY]),
                      llceil(F32(ui.mRight) * scale.mV[VX]), llfloor(F32(ui.mBottom) * scale.mV[VY]));
        pixels.intersectWith(LLRect(0, frame->getHeight(), frame->getWidth(), 0));
        if (pixels.isEmpty())
        {
            why = "the floater " + step["floater"].asString() + " is outside the window";
            return false;
        }
        const S32 width = pixels.getWidth();
        const S32 height = pixels.getHeight();
        LLPointer<LLImageRaw> crop = new LLImageRaw(U16(width), U16(height), 3);
        for (S32 row = 0; row < height; ++row)
        {
            memcpy(crop->getData() + size_t(row) * size_t(width) * 3,
                   frame->getData() + (size_t(pixels.mBottom + row) * size_t(frame->getWidth()) + size_t(pixels.mLeft)) * 3,
                   size_t(width) * 3);
        }
        LLSD result;
        result["name"] = name;
        result["floater"] = step["floater"];
        result["frame"] = S32(LLFrameTimer::getFrameCount() - sState.viewerFrameAtWarmup);
        result["crop"] = savePreviewImage(crop, name, why);
        result["crop"]["rect"] = llsd::array(pixels.mLeft, pixels.mTop, pixels.mRight, pixels.mBottom);
        if (!why.empty())
        {
            return false;
        }
        for (const auto& [texture_name, texture] : textures)
        {
            LLSD& entry = result["textures"][texture_name];
            const LLImageGL* gl = texture->getGLTexture();
            LLPointer<LLImageRaw> raw = new LLImageRaw();
            const bool success = gl && gl->readBackRaw(-1, raw, false) && raw->getData();
            entry["success"] = success;
            if (!success)
            {
                why = "readBackRaw of the " + texture_name + " preview failed";
                return false;
            }
            entry = savePreviewImage(raw, name + "." + texture_name, why);
            entry["success"] = true;
            if (!why.empty())
            {
                return false;
            }
        }
        if (LLModelPreview* model = textures.empty() ? nullptr : dynamic_cast<LLModelPreview*>(textures[0].second))
        {
            result["model"]["degenerate"] = model->mHasDegenerate;
            result["model"]["load_state"] = S32(model->getLoadState());
        }
        LL_INFOS("Golden") << "Golden test: preview " << name << " saved, " << width << "x" << height << " at " << pixels
                           << ", md5 " << result["crop"]["md5"].asString() << ", " << textures.size() << " textures read back"
                           << LL_ENDL;
        sState.previewResults.append(result);
        return true;
    }

    /// One frame of the preview run, at its end: the step's action at its first frame, its results at its last, and
    /// after the last step the results file, then the snapshot of that frame.
    void stepPreviews()
    {
        const LLSD& steps = sState.config["previews"];
        const LLSD& step = steps[sState.previewStep];
        if (sState.frames == 0 && !startPreviewStep(step))
        {
            return;
        }
        if (++sState.frames < U32(step["frames"].asInteger()))
        {
            hidePreviewChildren();
            return;
        }
        LLFloater* floater = nullptr;
        std::vector<std::pair<std::string, LLViewerDynamicTexture*>> textures;
        std::string why = previewNotReady(step, floater, textures);
        if (!why.empty())
        {
            fail("the preview step " + step["name"].asString() + " was not ready after its frames: " + why);
            return;
        }
        if (!capturePreview(step, floater, textures, why))
        {
            fail("the preview step " + step["name"].asString() + " failed: " + why);
            return;
        }
        sState.frames = 0;
        if (++sState.previewStep < steps.size())
        {
            return;
        }
        LLSD result;
        result["steps"] = sState.previewResults;
        result["decomposition_functional"] = LLConvexDecomposition::isFunctional();
        const std::string path = sState.config["previews_result"].asString();
        llofstream file(path.c_str());
        file << boost::json::serialize(LlsdToJson(result)) << "\n";
        file.close();
        if (file.fail())
        {
            fail("could not write " + path);
            return;
        }
        LL_INFOS("Golden") << "Golden test: previews " << path << LL_ENDL;
        snapshotAndFinish();
    }

    /// One window of the media phase as measured.media has it (media): its frames and seconds, the frame
    /// times' median and most, the frames over MEDIA_HITCH_US (and those at a frame with a media texture update or right
    /// after one), the media counts' changes over it (doMediaTexUpdate's calls and bytes, its and updateMediaImage's
    /// main-thread calls and microseconds; cost_us their microseconds together), the cost per update, per second and
    /// per frame, and per whole wall-clock second its microseconds, frames and cost (seconds_us, seconds_frames,
    /// seconds_cost_us), from which level2.py --perf takes medians.
    LLSD mediaWindow(const std::vector<MediaFrame>& frames, const LLViewerMediaImpl::TexUpdateCounts& at,
                     const LLViewerMediaImpl::TexUpdateCounts& now)
    {
        auto clamped = [](U64 value) { return (S32)llmin(value, U64(S32_MAX)); };
        U64 total_us = 0;
        U64 cost_us = 0;
        S32 over = 0;
        S32 over_at_updates = 0;
        std::vector<U32> times;
        LLSD seconds_us = LLSD::emptyArray();
        LLSD seconds_frames = LLSD::emptyArray();
        LLSD seconds_cost = LLSD::emptyArray();
        U64 second_us = 0;
        U64 second_cost = 0;
        S32 second_frames = 0;
        for (size_t i = 0; i < frames.size(); ++i)
        {
            const MediaFrame& frame = frames[i];
            total_us += frame.us;
            cost_us += frame.costUs;
            times.push_back(frame.us);
            if (frame.us > MEDIA_HITCH_US)
            {
                ++over;
                if (frame.updates > 0 || (i > 0 && frames[i - 1].updates > 0))
                {
                    ++over_at_updates;
                }
            }
            second_us += frame.us;
            second_cost += frame.costUs;
            ++second_frames;
            if (second_us >= 1000000)
            {
                seconds_us.append(clamped(second_us));
                seconds_frames.append(second_frames);
                seconds_cost.append(clamped(second_cost));
                second_us = 0;
                second_cost = 0;
                second_frames = 0;
            }
        }
        std::sort(times.begin(), times.end());
        const F64 seconds = F64(total_us) / 1e6;
        const U64 main_updates = now.mainUpdates - at.mainUpdates;
        const U64 update_us = now.mainUs - at.mainUs;
        LLSD window;
        window["frames"] = (S32)frames.size();
        window["seconds"] = seconds;
        window["median_frame_ms"] = times.empty() ? 0.0 : F64(times[times.size() / 2]) / 1000.0;
        window["max_frame_ms"] = times.empty() ? 0.0 : F64(times.back()) / 1000.0;
        window["over_33ms"] = over;
        window["over_33ms_at_updates"] = over_at_updates;
        window["updates"] = clamped(now.updates - at.updates);
        window["bytes"] = F64(now.bytes - at.bytes);
        window["main_updates"] = clamped(main_updates);
        window["update_us"] = clamped(update_us);
        window["image_calls"] = clamped(now.imageCalls - at.imageCalls);
        window["image_us"] = clamped(now.imageUs - at.imageUs);
        window["cost_us"] = clamped(cost_us);
        window["us_per_update"] = main_updates ? F64(update_us) / F64(main_updates) : 0.0;
        window["ms_per_second"] = seconds > 0.0 ? F64(cost_us) / 1000.0 / seconds : 0.0;
        window["ms_per_frame"] = frames.empty() ? 0.0 : F64(cost_us) / 1000.0 / F64(frames.size());
        window["seconds_us"] = seconds_us;
        window["seconds_frames"] = seconds_frames;
        window["seconds_cost_us"] = seconds_cost;
        return window;
    }

    /// The media phase's faces (media): each face's object, the region's root object nearest its host
    /// (findSceneObjectAt), gets a client-side LLMediaEntry on that face as shared media from the region would give it
    /// (LLVOVolume::syncMediaData, ignoring the agent: autoplay, loop, no auto scale, the config's size, the page's
    /// file:// url), so LLViewerMedia makes the face's impl and navigates it; the region and its objects are unchanged.
    /// A missing object, a face it does not have, a PBR face or no impl fails the run (false).
    bool attachMedia()
    {
        const LLSD& media = sState.config["media"];
        for (size_t i = 0; i < media["faces"].size(); ++i)
        {
            const LLSD& face = media["faces"][i];
            const std::string name = face["name"].asString();
            const S32 te = face["face"].asInteger();
            LLViewerObject* object = findSceneObjectAt(vector3(face["host"]));
            if (!object || object->getPCode() != LL_PCODE_VOLUME || te >= object->getNumTEs() || !object->getTE(te))
            {
                fail("media: " + name + ": no volume with face " + std::to_string(te) + " within FIND_DISTANCE of its host");
                return false;
            }
            if (object->getTE(te)->getGLTFMaterial())
            {
                fail("media: " + name + ": face " + std::to_string(te) + " has a PBR material, not a legacy face");
                return false;
            }
            LLSD entry;
            entry[LLMediaEntry::CURRENT_URL_KEY] = face["url"].asString();
            entry[LLMediaEntry::HOME_URL_KEY] = face["url"].asString();
            entry[LLMediaEntry::AUTO_PLAY_KEY] = true;
            entry[LLMediaEntry::AUTO_LOOP_KEY] = true;
            entry[LLMediaEntry::AUTO_SCALE_KEY] = false;
            entry[LLMediaEntry::WIDTH_PIXELS_KEY] = media["width"].asInteger();
            entry[LLMediaEntry::HEIGHT_PIXELS_KEY] = media["height"].asInteger();
            LLVOVolume* volume = static_cast<LLVOVolume*>(object);
            volume->syncMediaData(te, entry, false /*merge*/, true /*ignore_agent*/);
            viewer_media_t impl = volume->getMediaImpl(U8(te));
            if (impl.isNull())
            {
                fail("media: " + name + ": syncMediaData made no impl on face " + std::to_string(te));
                return false;
            }
            sState.mediaImpls.push_back(impl);
            // The face's side: its centre from the object's, in agent coordinates (a box's faces 1-4 are its sides).
            LLVector3 side;
            if (LLDrawable* drawable = object->mDrawable; drawable && te < drawable->getNumFaces() && drawable->getFace(te))
            {
                side = drawable->getFace(te)->getPositionAgent() - object->getPositionAgent();
            }
            LL_INFOS("Golden") << "Golden test: media " << name << " on face " << te << " of " << object->getID()
                               << " (its centre " << side << " from the object's): " << face["url"].asString() << LL_ENDL;
        }
        return true;
    }

    /// One frame of the media phase (media), at its swap: the frame's time and media counts, then the
    /// baseline window, the wait for every face's first update and the media window, then measured.media and the
    /// snapshot of the media window's last frame.
    void stepMedia()
    {
        const LLSD& media = sState.config["media"];
        const size_t window_frames = size_t(media["frames"].asInteger());
        const auto now = std::chrono::steady_clock::now();
        const LLViewerMediaImpl::TexUpdateCounts counts = LLViewerMediaImpl::texUpdateCounts();
        const U64 cost = counts.mainUs + counts.imageUs;
        MediaFrame frame;
        frame.us = U32(llclamp(std::chrono::duration_cast<std::chrono::microseconds>(now - sState.mediaLastSwap).count(),
                               (long long)0, (long long)U32_MAX));
        frame.costUs = U32(cost - sState.mediaCostAtSwap);
        frame.updates = U32(counts.updates - sState.mediaUpdatesAtSwap);
        sState.mediaLastSwap = now;
        sState.mediaCostAtSwap = cost;
        sState.mediaUpdatesAtSwap = counts.updates;
        if (sState.mediaStep == 0)
        {
            sState.mediaFrames.push_back(frame);
            if (sState.mediaFrames.size() < window_frames)
            {
                return;
            }
            sState.mediaResult["baseline"] = mediaWindow(sState.mediaFrames, sState.mediaCountsAt, counts);
            sState.mediaFrames.clear();
            if (!attachMedia())
            {
                return;
            }
            sState.mediaStep = 1;
            sState.frames = 0;
            sState.mediaWaitSince = LLTimer::getTotalSeconds();
            return;
        }
        if (sState.mediaStep == 1)
        {
            bool all = true;
            for (const viewer_media_t& impl : sState.mediaImpls)
            {
                all = all && impl->getTexUpdateCount() > 0;
            }
            if (!all)
            {
                if (ll_golden_wait_over(sState.frames, sState.mediaWaitSince, LLTimer::getTotalSeconds(), MEDIA_WAIT_FRAMES,
                                        MEDIA_WAIT_SECONDS))
                {
                    std::string which;
                    for (size_t i = 0; i < sState.mediaImpls.size(); ++i)
                    {
                        which += llformat("%s%s %llu updates, priority %s", i ? "; " : "",
                                          media["faces"][i]["name"].asString().c_str(),
                                          (unsigned long long)sState.mediaImpls[i]->getTexUpdateCount(),
                                          LLPluginClassMedia::priorityToString(sState.mediaImpls[i]->getPriority()));
                    }
                    fail("media: not every face's media texture updated within the wait: " + which);
                }
                return;
            }
            sState.mediaResult["wait_frames"] = (S32)sState.frames;
            sState.mediaResult["wait_seconds"] = F64(LLTimer::getTotalSeconds()) - sState.mediaWaitSince;
            sState.mediaCountsAt = counts;
            sState.mediaDeferredSwapsAt = LLImageGL::syncCounts().deferredSwaps;
            sState.mediaInPlaceUpdatesAt = LLImageGL::syncCounts().inPlaceUpdates;
            sState.mediaImplUpdatesAt.clear();
            for (const viewer_media_t& impl : sState.mediaImpls)
            {
                sState.mediaImplUpdatesAt.push_back(impl->getTexUpdateCount());
            }
            sState.mediaStep = 2;
            LL_INFOS("Golden") << "Golden test: every media face updated after " << sState.frames
                               << " frames; the media window of " << window_frames << " frames" << LL_ENDL;
            return;
        }
        sState.mediaFrames.push_back(frame);
        if (sState.mediaFrames.size() < window_frames)
        {
            return;
        }
        LLSD window = mediaWindow(sState.mediaFrames, sState.mediaCountsAt, counts);
        const F64 seconds = window["seconds"].asReal();
        sState.mediaResult["media"] = window;
        sState.mediaResult["deferred_swaps"] = (S32)(LLImageGL::syncCounts().deferredSwaps - sState.mediaDeferredSwapsAt);
        // [Viewer Lab] The window's main-thread updates made in place: on Metal all of window.main_updates,
        // on GL none (level2.py's media_failures)
        sState.mediaResult["in_place_updates"] =
            (S32)(LLImageGL::syncCounts().inPlaceUpdates - sState.mediaInPlaceUpdatesAt);
        sState.mediaResult["upload_thread"] = LLImageGLThread::sEnabledMedia;
        LLSD impls;
        for (size_t i = 0; i < sState.mediaImpls.size(); ++i)
        {
            viewer_media_t& impl = sState.mediaImpls[i];
            const U64 updates = impl->getTexUpdateCount() - sState.mediaImplUpdatesAt[i];
            LLPluginClassMedia* plugin = impl->getMediaPlugin();
            impls[media["faces"][i]["name"].asString()] =
                llsd::map("priority", LLPluginClassMedia::priorityToString(impl->getPriority()),
                          "updates", (S32)updates,
                          "updates_per_second", seconds > 0.0 ? F64(updates) / seconds : 0.0,
                          "url", impl->getCurrentMediaURL(),
                          "width", plugin ? plugin->getWidth() : 0,
                          "height", plugin ? plugin->getHeight() : 0,
                          "interest", impl->getInterest(),
                          "approximate_interest", impl->getApproximateTextureInterest());
        }
        sState.mediaResult["impls"] = impls;
        LL_INFOS("Golden") << "Golden test: the media window: " << window["ms_per_second"].asReal()
                           << " ms of media updates a second, " << window["updates"].asInteger() << " updates" << LL_ENDL;
        snapshotAndFinish();
    }
}

void LLGoldenTest::init()
{
    if (sState.phase == Phase::Unchecked)
    {
        readConfig();
        if (sState.phase == Phase::WaitingForWorld || sState.phase == Phase::LoginScreen
            || sState.phase == Phase::MetalLogin)
        {
            armProgramFault();
            armProgramDelay();
            declareCachedSettings();
            if (sState.phase != Phase::Done)
            {
                applyGraphics();
            }
            if (sState.phase == Phase::MetalLogin && sState.config["metal_login"]["autologin"].asBoolean())
            {
                // The autologin path of idle_startup (a credential from UserLoginInfo, so no connect box), before
                // STATE_LOGIN_SHOW reads it; the Metal UI dev mode must show the login screen anyway. The name
                // is made up and the secret empty: no account has them, and the world is off.
                gSavedSettings.getControl("UserLoginInfo")->setValue(llsd::array("Metal", "Autologin", ""), false);
                LL_INFOS("Golden") << "Golden test: metal login: autologin asked for (UserLoginInfo, unsaved)" << LL_ENDL;
            }
        }
    }
}

void LLGoldenTest::beforeNoise()
{
    if (sState.phase != Phase::Unchecked && sState.phase != Phase::Off)
    {
        ll_seed_rand(SEED);
    }
}

void LLGoldenTest::beforeUI()
{
    // The drag's mouse (drag): pinned at the drag's point in every frame from the drag's start, as the window
    // would report it, so that a mouse event from the window system (the window losing focus, the cursor leaving it)
    // never moves the drag. handleMouseMove takes the window's pixels and divides by the display scale.
    if (sState.dragPinned && sState.phase != Phase::Done && gViewerWindow)
    {
        const LLVector2& scale = gViewerWindow->getDisplayScale();
        gViewerWindow->handleMouseMove(gViewerWindow->getWindow(),
                                       LLCoordGL(ll_round(F32(sState.dragPoint.mX) * scale.mV[VX]),
                                                 ll_round(F32(sState.dragPoint.mY) * scale.mV[VY])),
                                       MASK_NONE);
    }
    // The config's mouse point (cursor), saved in every in-world frame before updateUI raycasts at it, as a
    // mouse move from the window system saves it, without the hover handling of handleMouseMove.
    if (sState.cursorLocked && gViewerWindow
        && (sState.phase == Phase::Sizing || sState.phase == Phase::Moving || sState.phase == Phase::Loading
            || sState.phase == Phase::Warming || sState.phase == Phase::Previews))
    {
        gViewerWindow->saveLastMouse(sState.cursorPoint);
    }
    // The config's projector and local lights, given back after this frame's network messages.
    if (sState.phase == Phase::Loading || sState.phase == Phase::Warming || sState.phase == Phase::Previews)
    {
        keepLocalLights();
    }
}

void LLGoldenTest::afterUI()
{
    // The config's mouse point with the raycast display (cursor). updateUI casts the debug ray at the saved
    // point, then, when the window system's cursor is inside the window (mMouseInWindow, updateMouseDelta), lets the
    // current tool handle a hover at the same point: LLToolPie::handleHover's pick ends in LLPickInfo::getSurfaceInfo,
    // whose cursorIntersect against the picked object alone leaves gDebugRaycastIntersection at the far end of its
    // 1024 m ray, so renderRaycast's axes at the hit point (llspatialpartition.cpp) are drawn out of view. The
    // display then depends on where the system's mouse is (a larger desktop can put the parked mouse inside the
    // window). The same raycast again, after the hover, gives the display updateUI's own result wherever
    // that mouse is; with it outside the window this changes nothing.
    if (!sState.cursorLocked || !gViewerWindow || !gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_RAYCAST)
        || !(sState.phase == Phase::Sizing || sState.phase == Phase::Moving || sState.phase == Phase::Loading
             || sState.phase == Phase::Warming || sState.phase == Phase::Previews))
    {
        return;
    }
    const LLVector4a after_ui = gDebugRaycastIntersection;
    // As LLViewerWindow::updateUI casts it.
    gDebugRaycastFaceHit = gDebugRaycastGLTFNodeHit = gDebugRaycastGLTFPrimitiveHit = -1;
    gDebugRaycastObject = gViewerWindow->cursorIntersect(-1, -1, 512.f, NULL, -1, false, false, true, false,
                                                         &gDebugRaycastFaceHit,
                                                         &gDebugRaycastGLTFNodeHit,
                                                         &gDebugRaycastGLTFPrimitiveHit,
                                                         &gDebugRaycastIntersection,
                                                         &gDebugRaycastTexCoord,
                                                         &gDebugRaycastNormal,
                                                         &gDebugRaycastTangent,
                                                         &gDebugRaycastStart,
                                                         &gDebugRaycastEnd);
    if (sState.phase == Phase::Warming && !after_ui.equals3(gDebugRaycastIntersection))
    {
        ++sState.cursorHoverRay;
    }
}

// Shows the config's login_page in the login panel's browser (login_screen). The grid's login page has the
// build's revision and the OS version in its address, and the local server's page shows the address, so a snapshot
// of it would change with every configure and OS update.
static bool isLoginPage(const std::string& url)
{
    const std::string& page = sState.loginPage;
    return url.compare(0, page.size(), page) == 0 && (url.size() == page.size() || url[page.size()] == '?');
}

static void showLoginPage()
{
    LLMediaCtrl* browser = gViewerWindow->getRootView()->findChild<LLMediaCtrl>("login_html");
    if (!browser)
    {
        fail("the login screen has no login_html browser");
        return;
    }
    browser->addObserver(&sLoginPageObserver);
    browser->navigateTo(sState.loginPage, "text/html");
    LL_INFOS("Golden") << "Golden test: showing " << sState.loginPage << " at the login screen" << LL_ENDL;
}

#if LL_RHI_METAL
// The Metal login hook (metal_login): the scenes at the login screen of the Metal UI dev
// mode, on Metal under the armed raw-GL trap, and their GL counterpart in the dev mode's injected refusal. Window
// changes go through the idle callbacks, outside display(), as the window system's own do; the checks run here, while
// the back buffer holds the frame about to be presented.
namespace
{
    constexpr U32 METAL_SETTLE_FRAMES = 10;       ///< frames a window change gets before its sizes are checked
    constexpr U32 METAL_SETTLE_LIMIT = 120;       ///< and at most, for the sizes to agree
    constexpr U32 METAL_PROGRAM_FRAMES = 30;      ///< the pending program is Ready within this many frames
    constexpr F32 METAL_RESIZE_POINTS = 64.f;     ///< the resize scene's growth on each side, in points
    constexpr size_t METAL_FRAME_COLOURS = 16;    ///< the drawn frame's distinct RGBA values, at least
    constexpr size_t METAL_ELEMENT_COLOURS = 2;   ///< and each element's, at its rect
    constexpr U32 METAL_GLYPH_STEADY_FRAMES = 10; ///< frames without an upload inside a pass before the glyphs
    constexpr U32 METAL_GLYPH_FRAMES = 5;         ///< and frames for the new glyphs' upload pass break after them
    constexpr F32 METAL_PAGE_SECONDS = 15.f;      ///< the resize scene's wait for the login page, at most (wall clock)
    constexpr F32 METAL_PAGE_STEADY_SECONDS = 0.5f; ///< and how long the media's pixels stay as they are before it ends
    constexpr F32 METAL_PAGE_NUDGE_SECONDS = 5.f; ///< and when it nudges the media to repaint once, if the page is not back
    /// MTLPixelFormatBGRA8Unorm (MTLPixelFormat.h:103), the layer's pixel format, as MetalSurfaceStats has it.
    constexpr U32 METAL_LAYER_PIXEL_FORMAT = 80;
    /// Latin-1 letters no login-screen widget shows, so their glyphs are not in the font's atlas yet: AE, O with
    /// stroke, A with ring, ae, o with stroke, a with ring, sharp s, Eth.
    const char* const METAL_GLYPH_TEXT = "ÆØÅæøåßÐ";
    const char* const METAL_PROGRAM_NAME = "Golden Metal Login Pending Program";
    /// The login screen's elements the drawn gate needs, by XUI name: the login panel in the login panel holder, its
    /// username field and its Log In button (panel_login.xml), and the menu bar (menu_login.xml) in the root view.
    /// The holder is where the panel is looked for, since the progress view has a panel_login of its own
    /// (panel_progress.xml).
    const char* const METAL_DRAWN_ELEMENTS[] = { "panel_login", "username_combo", "connect_btn", "Login Menu" };

    struct MetalLoginState
    {
        bool started = false;
        bool done = false;
        bool metal = false;      ///< the viewer's device is Metal's (else the dev mode's GL)
        U32 frame = 0;           ///< login-screen frames since the start; the first is 1
        U64 baseFrames = 0;      ///< the device's frame number at the start,
        U64 basePresents = 0;    ///< the surface's presents
        U64 baseSwaps = 0;       ///< and the window's swaps
        U64 skippedZero = 0;     ///< the presents without a drawable so far, all the zero scene's
        F32 uiSize = 1.f;        ///< getSystemUISize at the start: s without the test backing scale
        std::vector<std::pair<U32, std::string>> scenes; ///< (frame, name), by frame
        size_t next = 0;         ///< the next scene to start
        std::string scene;       ///< the scene in progress, or empty
        U32 step = 0;            ///< its step
        U32 stepFrame = 0;       ///< the frame the step began
        F32 windowFrame[4] = {}; ///< the window's frame (x, y, width, height) before the scene changed it
        F32 boundsWidth = 0.f;   ///< the view's bounds in points before the scene changed them
        F32 boundsHeight = 0.f;
        U64 skippedBefore = 0;   ///< presentsSkippedZero when the zero scene began
        U64 uploadBreaks = 0;    ///< the encoder's upload breaks when the glyphs scene's step began
        U64 passBreaks = 0;      ///< and its pass breaks
        U64 uploadPassBreaks = 0; ///< and its upload pass breaks
        LLTimer pageTimer;       ///< since the window was asked back in the resize scene (metalTrackPage)
        U32 pageFrames = 0;      ///< the frames of the wait for the login page, not counted in frame
        U64 pageSwaps = 0;       ///< LLImageGL's deferred swaps (the media's texture updates) when the window was asked back
        U64 pageLastSwaps = 0;   ///< at the previous frame's check
        U64 pageSwapsBefore = 0; ///< at the check before the media's pixels last changed
        F32 pageChanged = 0.f;   ///< when they last changed, on pageTimer
        U32 pageChanges = 0;     ///< how often they changed
        F32 pageNudged = -1.f;   ///< when the wait nudged the media to repaint, on pageTimer; negative before
        std::vector<U8> pagePixels; ///< the media's pixels as last seen
        std::vector<U8> backBuffer; ///< the drawn frame's back buffer, RGBA in GL's row order
        U32 backWidth = 0;
        U32 backHeight = 0;
        std::unique_ptr<LLGLSLShader> program; ///< the pending-program check's program
        U32 programFrame = 0;    ///< the frame it was made
        U32 programPending = 0;  ///< the frames it was seen pending
    };
    MetalLoginState sMetal;

    std::string metalCounts(LL::RHI::Device& device)
    {
        const LL::RHI::MetalSurfaceStats stats = LL::RHI::metalSurfaceStats(device);
        std::ostringstream text;
        text << "frames " << device.frameNumber() - sMetal.baseFrames << ", presents " << stats.presents - sMetal.basePresents
             << ", swaps " << display_window_swaps() - sMetal.baseSwaps << " since the start";
        if (sMetal.metal)
        {
            text << "; presents " << stats.presents << ", drawables presented " << stats.drawablesPresented
                 << ", presents without a drawable " << stats.presentsSkippedZero << ", drawable timeouts "
                 << stats.drawableTimeouts << ", drawable waits " << stats.drawableWaits << ", drawable signals "
                 << stats.drawableSignals << " since the device began";
            if (stats.presentsPaced > 0)
            {
                // Only with displaySync off, so the vsync-on scenes' lines do not change
                text << ", presents left in the back buffer for the window server " << stats.presentsPaced;
            }
            if (stats.finishedFlips + stats.finishedSkipped > 0)
            {
                // <Luminous> VSync off's finished path likewise
                text << ", finished frames flipped " << stats.finishedFlips << ", skipped " << stats.finishedSkipped
                     << ", presented " << stats.finishedPresented << ", held " << stats.finishedHeld;
            }
        }
        return text.str();
    }

    /// What breaks the counters, or empty: since the start the device's frames, the surface's presents (on
    /// Metal) and the window's swaps are equal; and on Metal no drawable timed out, each present presented a drawable,
    /// found a 0, left the frame in the back buffer for the window server (displaySync off), timed out, or (VSync
    /// off's finished path) flipped its frame into a finished frame or found none free, the present queue's drawables
    /// (finishedPresented, finishedTimeouts) aside,
    /// each presented drawable had one wait and one signal on the queue, and only the zero scene found a 0.
    std::string metalCountersWhy(LL::RHI::Device& device)
    {
        const LL::RHI::MetalSurfaceStats stats = LL::RHI::metalSurfaceStats(device);
        const U64 frames = device.frameNumber() - sMetal.baseFrames;
        const U64 presents = stats.presents - sMetal.basePresents;
        const U64 swaps = display_window_swaps() - sMetal.baseSwaps;
        const bool ok = frames == swaps
                        && (!sMetal.metal
                            || (presents == swaps && stats.drawableTimeouts == 0
                                && stats.presents == (stats.drawablesPresented - stats.finishedPresented)
                                                         + stats.presentsSkippedZero + stats.presentsPaced
                                                         + (stats.drawableTimeouts - stats.finishedTimeouts)
                                                         + stats.finishedFlips + stats.finishedSkipped
                                && stats.drawableWaits == stats.drawablesPresented
                                && stats.drawableSignals == stats.drawablesPresented
                                && (sMetal.scene == "zero" || stats.presentsSkippedZero == sMetal.skippedZero)));
        return ok ? std::string() : "the counters are off: " + metalCounts(device);
    }

    /// s for the window checks: 1 at RenderHiDPI 0; at RenderHiDPI 1 the test backing scale when it is on, else
    /// the system UI size at the start.
    F32 metalScale(bool override_on)
    {
        if (!gHiDPISupport)
        {
            return 1.f;
        }
        return override_on ? F32(sState.config["metal_login"]["scale"].asReal()) : sMetal.uiSize;
    }

    /// The view's bounds in points (the window's content view is the LLOpenGLView).
    CGSize metalBounds(LLWindow* window)
    {
        return getContentViewRect(window->getPlatformWindow()).size;
    }

    /// What keeps getSize, the drawable, the back buffer and the window from being the view's bounds (width x height
    /// points) x s, or contentsScale and getSystemUISize from being s, or empty.
    std::string metalSizesWhy(LL::RHI::Device& device, LLWindow* window, F32 s, F32 width, F32 height)
    {
        const CGSize bounds = metalBounds(window);
        LLCoordWindow size;
        window->getSize(&size);
        const LL::RHI::MetalSurfaceStats stats = LL::RHI::metalSurfaceStats(device);
        const LL::RHI::TextureDesc& back = device.surface()->backBuffer()->desc();
        const F32 ui = window->getSystemUISize();
        // getSize truncates the backing rect as here.
        const S32 w = S32(bounds.width * s);
        const S32 h = S32(bounds.height * s);
        const bool ok = F32(bounds.width) == width && F32(bounds.height) == height && size.mX == w && size.mY == h
                        && S32(stats.drawableWidth) == w && S32(stats.drawableHeight) == h && S32(back.width) == w
                        && S32(back.height) == h && gViewerWindow->getWindowWidthRaw() == w
                        && gViewerWindow->getWindowHeightRaw() == h && stats.contentsScale == s && ui == s;
        if (ok)
        {
            return {};
        }
        std::ostringstream why;
        why << "bounds " << bounds.width << "x" << bounds.height << " points (wanted " << width << "x" << height
            << "), s " << s << ": getSize " << size.mX << "x" << size.mY << ", drawable " << stats.drawableWidth << "x"
            << stats.drawableHeight << ", back buffer " << back.width << "x" << back.height << ", window "
            << gViewerWindow->getWindowWidthRaw() << "x" << gViewerWindow->getWindowHeightRaw() << ", contentsScale "
            << stats.contentsScale << ", system UI size " << ui;
        return why.str();
    }

    std::string metalSizes(LL::RHI::Device& device, LLWindow* window)
    {
        const CGSize bounds = metalBounds(window);
        const LL::RHI::MetalSurfaceStats stats = LL::RHI::metalSurfaceStats(device);
        LLCoordWindow size;
        window->getSize(&size);
        std::ostringstream text;
        text << "bounds " << bounds.width << "x" << bounds.height << " points, getSize " << size.mX << "x" << size.mY
             << ", drawable " << stats.drawableWidth << "x" << stats.drawableHeight << ", window "
             << gViewerWindow->getWindowWidthRaw() << "x" << gViewerWindow->getWindowHeightRaw() << ", contentsScale "
             << stats.contentsScale << ", system UI size " << window->getSystemUISize();
        return text.str();
    }

    /// The GL counterpart of metalSizesWhy, for the resize scene on GL, whose device has no drawable or layer: what
    /// keeps getSize and the window from being the view's bounds (width x height points) x s, or empty.
    std::string sizesOnGLWhy(LLWindow* window, F32 s, F32 width, F32 height)
    {
        const CGSize bounds = metalBounds(window);
        LLCoordWindow size;
        window->getSize(&size);
        const S32 w = S32(bounds.width * s);
        const S32 h = S32(bounds.height * s);
        const bool ok = F32(bounds.width) == width && F32(bounds.height) == height && size.mX == w && size.mY == h
                        && gViewerWindow->getWindowWidthRaw() == w && gViewerWindow->getWindowHeightRaw() == h;
        if (ok)
        {
            return {};
        }
        std::ostringstream why;
        why << "bounds " << bounds.width << "x" << bounds.height << " points (wanted " << width << "x" << height
            << "), s " << s << ": getSize " << size.mX << "x" << size.mY << ", window " << gViewerWindow->getWindowWidthRaw()
            << "x" << gViewerWindow->getWindowHeightRaw();
        return why.str();
    }

    std::string sizesOnGL(LLWindow* window)
    {
        const CGSize bounds = metalBounds(window);
        LLCoordWindow size;
        window->getSize(&size);
        std::ostringstream text;
        text << "bounds " << bounds.width << "x" << bounds.height << " points, getSize " << size.mX << "x" << size.mY
             << ", window " << gViewerWindow->getWindowWidthRaw() << "x" << gViewerWindow->getWindowHeightRaw();
        return text.str();
    }

    void metalStep(U32 step)
    {
        sMetal.step = step;
        sMetal.stepFrame = sMetal.frame;
    }

    /// True once the step has had METAL_SETTLE_FRAMES and the sizes agree with s and the bounds (on GL without the
    /// drawable and the layer); fails after METAL_SETTLE_LIMIT frames.
    bool metalSettled(LL::RHI::Device& device, LLWindow* window, F32 s, F32 width, F32 height)
    {
        const U32 frames = sMetal.frame - sMetal.stepFrame;
        if (frames < METAL_SETTLE_FRAMES)
        {
            return false;
        }
        const std::string why = sMetal.metal ? metalSizesWhy(device, window, s, width, height)
                                             : sizesOnGLWhy(window, s, width, height);
        if (why.empty())
        {
            return true;
        }
        if (frames >= METAL_SETTLE_LIMIT)
        {
            fail("metal login: " + sMetal.scene + llformat(" (step %u): after %u frames ", sMetal.step, frames) + why);
        }
        return false;
    }

    void metalLog(const std::string& line)
    {
        LL_INFOS("Golden") << "Golden test: metal login: " << line << LL_ENDL;
    }

    /// The window's frame size, from the next idle callback, outside display(), as the window system's own changes.
    void metalSetWindowSize(LLWindow* window, F32 width, F32 height)
    {
        void* platform = window->getPlatformWindow();
        doOnIdleOneTime([platform, width, height]() { setWindowSize(platform, S32(width), S32(height)); });
    }

    void metalSaveWindow(LLWindow* window)
    {
        getWindowSize(window->getPlatformWindow(), sMetal.windowFrame);
        const CGSize bounds = metalBounds(window);
        sMetal.boundsWidth = F32(bounds.width);
        sMetal.boundsHeight = F32(bounds.height);
    }

    /// The back buffer, RGBA in GL's row order (the bottom row first), through the viewer's device.
    bool metalReadBackBuffer(LL::RHI::Device& device, std::string& why)
    {
        LL::RHI::Texture* back = device.surface()->backBuffer();
        const U32 width = back->desc().width;
        const U32 height = back->desc().height;
        LL::RHI::Ref<LL::RHI::Readback> readback =
            device.encoder().readback(back, 0, 0, { 0, 0, width, height }, LL::RHI::PixelLayout());
        if (readback.isNull())
        {
            why = "the readback of the back buffer failed";
            return false;
        }
        readback->wait();
        const std::span<const std::byte> data = readback->data();
        const size_t row = size_t(width) * 4;
        const size_t pitch = readback->rowPitch() ? readback->rowPitch() : row;
        if (pitch < row || data.size() < pitch * (height - 1) + row)
        {
            why = llformat("the readback of the back buffer has %zu bytes for %ux%u", data.size(), width, height);
            return false;
        }
        sMetal.backBuffer.resize(row * height);
        for (U32 y = 0; y < height; ++y)
        {
            memcpy(sMetal.backBuffer.data() + y * row, data.data() + y * pitch, row);
        }
        sMetal.backWidth = width;
        sMetal.backHeight = height;
        return true;
    }

    size_t metalColours(U32 left, U32 bottom, U32 right, U32 top)
    {
        std::unordered_set<U32> colours;
        for (U32 y = bottom; y < top; ++y)
        {
            const U8* pixel = sMetal.backBuffer.data() + (size_t(y) * sMetal.backWidth + left) * 4;
            for (U32 x = left; x < right; ++x, pixel += 4)
            {
                colours.insert(U32(pixel[0]) | U32(pixel[1]) << 8 | U32(pixel[2]) << 16 | U32(pixel[3]) << 24);
            }
        }
        return colours.size();
    }

    /// The drawn gate's parts (2) and (3) on the back buffer: at least METAL_FRAME_COLOURS colours in all, and each
    /// of the named elements visible with at least METAL_ELEMENT_COLOURS at its rect, in raw window pixels (the UI's
    /// rect x the display scale, GL's row order). Empty when it holds.
    std::string metalDrawnWhy(std::ostringstream& info)
    {
        const size_t colours = metalColours(0, 0, sMetal.backWidth, sMetal.backHeight);
        info << sMetal.backWidth << "x" << sMetal.backHeight << " with " << colours << " colours";
        if (colours < METAL_FRAME_COLOURS)
        {
            return llformat("the frame has %zu colours, not %zu or more", colours, METAL_FRAME_COLOURS);
        }
        LLView* root = gViewerWindow->getRootView();
        LLView* holder = gViewerWindow->getLoginPanelHolder();
        LLView* panel = holder ? holder->findChild<LLView>(METAL_DRAWN_ELEMENTS[0], true) : nullptr;
        const LLVector2& scale = gViewerWindow->getDisplayScale();
        for (const char* name : METAL_DRAWN_ELEMENTS)
        {
            LLView* const scope = name == METAL_DRAWN_ELEMENTS[0] ? holder : name == METAL_DRAWN_ELEMENTS[3] ? root : panel;
            LLView* view = scope ? scope->findChild<LLView>(name, true) : nullptr;
            if (!view || !view->isInVisibleChain())
            {
                return std::string("the login screen's ") + name + (view ? " is not visible" : " is missing");
            }
            const LLRect rect = view->calcScreenRect();
            const S32 width = S32(sMetal.backWidth);
            const S32 height = S32(sMetal.backHeight);
            const S32 left = llclamp(ll_round(rect.mLeft * scale.mV[VX]), 0, width);
            const S32 right = llclamp(ll_round(rect.mRight * scale.mV[VX]), 0, width);
            const S32 bottom = llclamp(ll_round(rect.mBottom * scale.mV[VY]), 0, height);
            const S32 top = llclamp(ll_round(rect.mTop * scale.mV[VY]), 0, height);
            if (left >= right || bottom >= top)
            {
                return llformat("the login screen's %s is outside the frame (UI rect %d, %d, %d, %d)", name, rect.mLeft,
                                rect.mBottom, rect.mRight, rect.mTop);
            }
            const size_t element = metalColours(U32(left), U32(bottom), U32(right), U32(top));
            info << llformat("; %s %zu colours at %d, %d, %d, %d", name, element, left, bottom, right, top);
            if (element < METAL_ELEMENT_COLOURS)
            {
                return llformat("the login screen's %s has %zu colour at its rect %d, %d, %d, %d", name, element, left,
                                bottom, right, top);
            }
        }
        return {};
    }

    /// The resize scene's watch of the login page's media from the moment the window is asked back (step 2): each frame,
    /// once the plugin's size change is through (textureValid), its pixels (the segment its texture updates copy,
    /// getWidth x getHeight x the depth, as LLViewerMediaImpl::doMediaTexUpdate) against those seen last, and LLImageGL's
    /// deferred swaps, which are the media's texture updates. When the pixels changed since the check before, an update
    /// in this frame or later has them: the plugin writes its pixels before it sends the update, so an update that read
    /// the old ones is followed by the one for the new ones.
    void metalTrackPage()
    {
        LLMediaCtrl* browser = gViewerWindow->getRootView()->findChild<LLMediaCtrl>("login_html");
        LLPluginClassMedia* plugin = browser ? browser->getMediaPlugin() : nullptr;
        const U64 swaps = LLImageGL::syncCounts().deferredSwaps;
        const U8* bits = plugin && plugin->textureValid() ? plugin->getBitsData() : nullptr;
        if (bits)
        {
            const size_t size = size_t(plugin->getWidth()) * size_t(plugin->getHeight()) * size_t(llmax(plugin->getTextureDepth(), 0));
            if (sMetal.pagePixels.size() != size || memcmp(sMetal.pagePixels.data(), bits, size) != 0)
            {
                sMetal.pagePixels.assign(bits, bits + size);
                sMetal.pageChanged = F32(sMetal.pageTimer.getElapsedTimeF32());
                sMetal.pageSwapsBefore = sMetal.pageLastSwaps;
                ++sMetal.pageChanges;
            }
        }
        sMetal.pageLastSwaps = swaps;
    }

    /// Whether the resize scene's wait nudged the media to repaint (metalLoginPageBack), for its lines.
    std::string metalPageNudge()
    {
        return sMetal.pageNudged < 0.f ? std::string("no nudge") : llformat("nudged to repaint at %.1f s", sMetal.pageNudged);
    }

    /// The resize scene's last step, on GL and on Metal: the login page painted again at the size the window is back
    /// to. Each window size gives the login panel's browser a new size, and each size change of its media a new texture
    /// (LLViewerMediaImpl::updateMediaImage) and a new segment for its pixels, which LLPluginClassMedia::idle zeroes
    /// (llpluginclassmedia.cpp:240-247). When a paint of the old size reaches the CEF plugin after the change, the
    /// plugin marks the new segment dirty without copying into it (media_plugin_cef.cpp:196-209), so the texture first
    /// gets the zeros, and the page's paint at the new size comes later, on wall-clock time: about 2 s after the size
    /// change in the runs, on GL and on Metal, so a GL run (faster frames) took its snapshot frame before it and a
    /// Metal run after it, and the GL snapshot had the zeros where the page belongs. Done once the plugin's size change
    /// is through, its pixels are not all 0, a media texture update has come since they last changed (metalTrackPage),
    /// and they have stayed so for METAL_PAGE_STEADY_SECONDS; fails METAL_PAGE_SECONDS after the window was asked back.
    /// The viewer's own refresh for this (LLPanelLogin::draw calls LLPluginClassMedia::forceRenderRefresh 2 s after the
    /// media's size change, "the black screen issue"), which matches the paints that came 2.1 s after it; once in 11 GL
    /// waits no paint at the new size came within 15 s. So when the page is not back METAL_PAGE_NUDGE_SECONDS after the
    /// window was asked back, the wait makes that call once more, logs it, and waits for the rest of the time.
    /// metalLoginFrame does not count these frames.
    bool metalLoginPageBack()
    {
        if (sState.loginPage.empty())
        {
            metalLog("resize: no login page in the config to wait for");
            return true;
        }
        ++sMetal.pageFrames;
        metalTrackPage();
        LLMediaCtrl* browser = gViewerWindow->getRootView()->findChild<LLMediaCtrl>("login_html");
        LLPluginClassMedia* plugin = browser ? browser->getMediaPlugin() : nullptr;
        const F32 seconds = F32(sMetal.pageTimer.getElapsedTimeF32());
        const U64 swaps = sMetal.pageLastSwaps - sMetal.pageSwaps;
        const bool painted = std::any_of(sMetal.pagePixels.begin(), sMetal.pagePixels.end(), [](U8 byte) { return byte != 0; });
        std::string state;
        if (!plugin)
        {
            state = "the login panel's browser has no media plugin";
        }
        else if (!plugin->textureValid())
        {
            state = "the media's size change is not through";
        }
        else if (!painted)
        {
            state = llformat("the media's %dx%d pixels are all 0", plugin->getWidth(), plugin->getHeight());
        }
        else if (sMetal.pageLastSwaps == sMetal.pageSwapsBefore)
        {
            state = llformat("the media's %dx%d pixels are painted, but no media texture update has come since they changed",
                             plugin->getWidth(), plugin->getHeight());
        }
        else if (seconds - sMetal.pageChanged < METAL_PAGE_STEADY_SECONDS)
        {
            state = llformat("the media's %dx%d pixels changed %.2f s ago", plugin->getWidth(), plugin->getHeight(),
                             seconds - sMetal.pageChanged);
        }
        else
        {
            metalLog(llformat("resize: the login page again after %u frames, not counted, %.1f s after the window was asked back: "
                              "the media %dx%d painted %.1f s after it, %u pixel changes, %llu media texture updates, %s",
                              sMetal.pageFrames, seconds, plugin->getWidth(), plugin->getHeight(), sMetal.pageChanged, sMetal.pageChanges,
                              (unsigned long long)swaps, metalPageNudge().c_str()));
            return true;
        }
        if (seconds >= METAL_PAGE_SECONDS)
        {
            fail(llformat("metal login: resize: the login page was not painted again %.0f s after the window was asked back "
                          "(%u frames of the wait): ",
                          METAL_PAGE_SECONDS, sMetal.pageFrames) +
                 state +
                 llformat("; %u pixel changes, %llu media texture updates, %s", sMetal.pageChanges, (unsigned long long)swaps,
                          metalPageNudge().c_str()));
        }
        else if (plugin && sMetal.pageNudged < 0.f && seconds >= METAL_PAGE_NUDGE_SECONDS)
        {
            plugin->forceRenderRefresh();
            sMetal.pageNudged = seconds;
            metalLog(llformat("resize: the login page is not back %.1f s after the window was asked back (", seconds) + state +
                     "): nudged the media to repaint once (LLPluginClassMedia::forceRenderRefresh, as LLPanelLogin::draw "
                     "calls it 2 s after a size change)");
        }
        return false;
    }

    /// The drawn back buffer as the config's snapshot (RGB, as readFrame), and the render profile.
    bool metalSaveSnapshot(std::string& why)
    {
        const std::string path = sState.config["snapshot"].asString();
        LLPointer<LLImageRaw> raw = new LLImageRaw(U16(sMetal.backWidth), U16(sMetal.backHeight), 3);
        U8* rgb = raw->getData();
        for (size_t pixel = 0; pixel < size_t(sMetal.backWidth) * sMetal.backHeight; ++pixel)
        {
            memcpy(rgb + pixel * 3, sMetal.backBuffer.data() + pixel * 4, 3);
        }
        LLPointer<LLImagePNG> png = new LLImagePNG();
        if (!png->encode(raw, 0.f) || !png->save(path))
        {
            why = "could not write " + path;
            return false;
        }
        LL_INFOS("Golden") << "Golden test: snapshot " << path << " (" << sMetal.backWidth << "x" << sMetal.backHeight << ")"
                           << LL_ENDL;
        return writeRenderProfile(sState.config["render_profile"].asString(), why);
    }

    /// The drawn gate's part (1): the frame's drawable is the back buffer flipped, exactly.
    std::string metalDrawableWhy(LL::RHI::Device& device)
    {
        std::vector<U8> drawable;
        U32 width = 0;
        U32 height = 0;
        const bool read = LL::RHI::readMetalDrawable(device, drawable, width, height);
        LL::RHI::setMetalDrawableReadback(device, false);
        if (!read)
        {
            return "the drawable of the drawn frame could not be read";
        }
        if (width != sMetal.backWidth || height != sMetal.backHeight)
        {
            return llformat("the drawable is %ux%u, the back buffer %ux%u", width, height, sMetal.backWidth,
                            sMetal.backHeight);
        }
        const size_t row = size_t(width) * 4;
        size_t differ = 0;
        std::string first;
        for (U32 y = 0; y < height; ++y)
        {
            // The drawable's rows go from the top, the back buffer's from the bottom.
            const U8* top_down = drawable.data() + y * row;
            const U8* bottom_up = sMetal.backBuffer.data() + (height - 1 - y) * row;
            for (U32 x = 0; x < width; ++x)
            {
                if (memcmp(top_down + x * 4, bottom_up + x * 4, 4) != 0)
                {
                    if (differ++ == 0)
                    {
                        const U8* d = top_down + x * 4;
                        const U8* b = bottom_up + x * 4;
                        first = llformat("; first at drawable row %u, column %u: %u %u %u %u, back buffer %u %u %u %u", y,
                                         x, d[0], d[1], d[2], d[3], b[0], b[1], b[2], b[3]);
                    }
                }
            }
        }
        if (differ)
        {
            return llformat("the drawable differs from the back buffer flipped in %zu of %zu pixels", differ,
                            size_t(width) * height) + first;
        }
        return {};
    }

    /// The username field's line editor: the LLLineEditor child of the login panel's username_combo, found as
    /// metalDrawnWhy finds the combo, or null.
    LLLineEditor* metalUsernameEditor()
    {
        LLView* holder = gViewerWindow->getLoginPanelHolder();
        LLView* panel = holder ? holder->findChild<LLView>(METAL_DRAWN_ELEMENTS[0], true) : nullptr;
        LLView* combo = panel ? panel->findChild<LLView>(METAL_DRAWN_ELEMENTS[1], true) : nullptr;
        if (combo)
        {
            for (LLView* child : *combo->getChildList())
            {
                if (LLLineEditor* editor = dynamic_cast<LLLineEditor*>(child))
                {
                    return editor;
                }
            }
        }
        return nullptr;
    }

    /// The glyphs scene, one frame; true when it is done. New glyphs in the username field are first rasterized when
    /// the field draws them, inside the UI's pass, and their atlas upload breaks the pass on Metal. Step 1 waits
    /// METAL_GLYPH_STEADY_FRAMES frames, on Metal without an upload inside a pass; then the text is set (on GL that is
    /// all); step 2 wants an upload pass break within METAL_GLYPH_FRAMES frames: an upload inside a pass that closed
    /// the pass's open segment (MetalEncoderStats::uploadPassBreaks), which the steady frames had none of. The upload
    /// and pass breaks are logged too; a pass break alone can come from other splits (the present's flip).
    bool metalGlyphs(LL::RHI::Device& device)
    {
        const LL::RHI::MetalEncoderStats stats = LL::RHI::metalEncoderStats(device);
        const U32 frames = sMetal.frame - sMetal.stepFrame;
        if (sMetal.step == 0)
        {
            sMetal.uploadBreaks = stats.uploadBreaks;
            sMetal.passBreaks = stats.passBreaks;
            sMetal.uploadPassBreaks = stats.uploadPassBreaks;
            metalStep(1);
            return false;
        }
        if (sMetal.step == 1)
        {
            if (sMetal.metal && stats.uploadBreaks != sMetal.uploadBreaks)
            {
                fail(llformat("metal login: glyphs: %llu upload breaks inside a pass by frame %u of the %u frames before the new "
                              "glyphs (hook frame %u): the login screen is not steady",
                              (unsigned long long)(stats.uploadBreaks - sMetal.uploadBreaks), frames, METAL_GLYPH_STEADY_FRAMES,
                              sMetal.frame));
                return false;
            }
            if (frames < METAL_GLYPH_STEADY_FRAMES)
            {
                return false;
            }
            // With the cursor at 0 setText -> setCursor -> findPixelNearestPos measures no characters
            // (lllineeditor.cpp:455, 487-493), so no glyph is made here, outside the pass. Not LLComboBox::setTextEntry,
            // whose updateSelection completes the text from the prefix before the cursor (llcombobox.cpp:1101-1140).
            LLLineEditor* editor = metalUsernameEditor();
            if (!editor)
            {
                fail("metal login: glyphs: the username field's line editor is missing");
                return false;
            }
            if (editor->getCursor() != 0 || editor->hasSelection())
            {
                fail(llformat("metal login: glyphs: the username field's cursor is not at 0 (cursor %d, %s)", editor->getCursor(),
                              editor->hasSelection() ? "a selection" : "no selection"));
                return false;
            }
            editor->setText(LLStringExplicit(METAL_GLYPH_TEXT));
            if (!sMetal.metal)
            {
                metalLog("glyphs: set on GL");
                return true;
            }
            const LL::RHI::MetalEncoderStats set = LL::RHI::metalEncoderStats(device);
            sMetal.uploadBreaks = set.uploadBreaks;
            sMetal.passBreaks = set.passBreaks;
            sMetal.uploadPassBreaks = set.uploadPassBreaks;
            metalStep(2);
            return false;
        }
        const U64 closing = stats.uploadPassBreaks - sMetal.uploadPassBreaks;
        const U64 uploads = stats.uploadBreaks - sMetal.uploadBreaks;
        const U64 passes = stats.passBreaks - sMetal.passBreaks;
        if (closing > 0)
        {
            metalLog(llformat("glyphs: %llu upload pass breaks, %llu upload breaks and %llu pass breaks in %u frames after the "
                              "new glyphs, no upload inside a pass in the %u frames before",
                              (unsigned long long)closing, (unsigned long long)uploads, (unsigned long long)passes, frames,
                              METAL_GLYPH_STEADY_FRAMES));
            return true;
        }
        if (frames >= METAL_GLYPH_FRAMES)
        {
            fail(llformat("metal login: glyphs: no upload of the new glyphs broke the pass (already cached, uploaded outside "
                          "the pass, or the upload closed no segment): %llu upload pass breaks, %llu upload breaks and %llu "
                          "pass breaks in %u frames",
                          (unsigned long long)closing, (unsigned long long)uploads, (unsigned long long)passes, frames));
        }
        return false;
    }

    /// One frame of a scene; true when it is done. A failure calls fail().
    bool metalScene(LL::RHI::Device& device, LLWindow* window)
    {
        const std::string& scene = sMetal.scene;
        const bool metal_only = scene != "drawn" && scene != "glyphs" && scene != "resize"; // these three run on GL too
        if (metal_only && !sMetal.metal)
        {
            metalLog(scene + ": skipped on GL (the Metal part of the hook)");
            return true;
        }
        if (scene == "pending_program")
        {
            // One program outside setShaders, a copy of the UI program under a new name; the checks run every
            // frame (metalProgramFrame).
            sMetal.program = std::make_unique<LLGLSLShader>();
            LLGLSLShader& program = *sMetal.program;
            program.mName = METAL_PROGRAM_NAME;
            program.mShaderFiles = gUIProgram.mShaderFiles;
            program.mShaderLevel = gUIProgram.mShaderLevel;
            program.mFeatures = gUIProgram.mFeatures;
            program.mDefines = gUIProgram.mDefines;
            sMetal.programFrame = sMetal.frame;
            if (!program.createShader())
            {
                fail("metal login: pending_program: createShader failed for a copy of " + gUIProgram.mName);
            }
            return true;
        }
        if (scene == "drawn")
        {
            std::string why;
            if (sMetal.step == 0)
            {
                if (sMetal.metal)
                {
                    // The live layer is opaque, has no colorspace, so its frames are not colour matched,
                    // and is BGRA8Unorm (CAMetalLayer.h:46-47, 77, 118-122).
                    const LL::RHI::MetalSurfaceStats layer = LL::RHI::metalSurfaceStats(device);
                    if (!layer.opaque || layer.colorspace || layer.pixelFormat != METAL_LAYER_PIXEL_FORMAT)
                    {
                        fail(llformat("metal login: drawn: layer: opaque %d, colorspace %s, pixel format %u; wanted opaque, no "
                                      "colorspace (no colour matching), BGRA8Unorm (%u)",
                                      layer.opaque, layer.colorspace ? "set" : "nil", layer.pixelFormat,
                                      METAL_LAYER_PIXEL_FORMAT));
                        return false;
                    }
                    metalLog("drawn: layer: opaque, no colorspace (no colour matching), BGRA8Unorm");
                    LL::RHI::setMetalDrawableReadback(device, true); // a copy of this frame's drawable at its present
                }
                std::ostringstream info;
                if (!metalReadBackBuffer(device, why) || !(why = metalDrawnWhy(info)).empty())
                {
                    fail("metal login: drawn: " + why);
                    return false;
                }
                metalLog("drawn: the back buffer " + info.str());
                if (!metalSaveSnapshot(why))
                {
                    fail("metal login: drawn: " + why);
                    return false;
                }
                if (!sMetal.metal)
                {
                    metalLog("drawn: passed on GL, without the drawable");
                    return true;
                }
                metalStep(1);
                return false;
            }
            why = metalDrawableWhy(device);
            if (!why.empty())
            {
                fail("metal login: drawn: " + why);
                return false;
            }
            metalLog(llformat("drawn: passed: the drawable is the back buffer flipped, exactly (%ux%u)", sMetal.backWidth,
                              sMetal.backHeight));
            return true;
        }
        if (scene == "glyphs")
        {
            return metalGlyphs(device);
        }
        const F32 s = metalScale(false);
        if (scene == "resize")
        {
            switch (sMetal.step)
            {
            case 0:
                metalSaveWindow(window);
                metalSetWindowSize(window, sMetal.windowFrame[2] + METAL_RESIZE_POINTS,
                                   sMetal.windowFrame[3] + METAL_RESIZE_POINTS);
                metalStep(1);
                return false;
            case 1:
                if (!metalSettled(device, window, s, sMetal.boundsWidth + METAL_RESIZE_POINTS,
                                  sMetal.boundsHeight + METAL_RESIZE_POINTS))
                {
                    return false;
                }
                metalLog(sMetal.metal ? "resize: 64 points larger: " + metalSizes(device, window) + "; " + metalCounts(device)
                                      : "resize: 64 points larger (GL): " + sizesOnGL(window));
                metalSetWindowSize(window, sMetal.windowFrame[2], sMetal.windowFrame[3]);
                metalStep(2);
                // The login page's media from here on (metalTrackPage, metalLoginPageBack)
                sMetal.pageTimer.reset();
                sMetal.pageFrames = 0;
                sMetal.pageSwaps = sMetal.pageLastSwaps = sMetal.pageSwapsBefore = LLImageGL::syncCounts().deferredSwaps;
                sMetal.pageChanged = 0.f;
                sMetal.pageChanges = 0;
                sMetal.pageNudged = -1.f;
                sMetal.pagePixels.clear();
                metalTrackPage();
                return false;
            case 2:
                metalTrackPage();
                if (!metalSettled(device, window, s, sMetal.boundsWidth, sMetal.boundsHeight))
                {
                    return false;
                }
                metalLog(sMetal.metal ? "resize: back: " + metalSizes(device, window) + "; " + metalCounts(device)
                                      : "resize: back (GL): " + sizesOnGL(window));
                // Both runs of a pair take their snapshot with the login page painted at this size (metalLoginPageBack)
                metalStep(3);
                return false;
            default:
                return metalLoginPageBack();
            }
        }
        if (scene == "zero")
        {
            if (sMetal.step == 0)
            {
                // The content 0 points high: the frame as high as its title bar. An idle callback gives the size back
                // once a present has found the 0, so the window has it for one present.
                metalSaveWindow(window);
                sMetal.skippedBefore = LL::RHI::metalSurfaceStats(device).presentsSkippedZero;
                void* platform = window->getPlatformWindow();
                const F32 width = sMetal.windowFrame[2];
                const F32 height = sMetal.windowFrame[3];
                metalSetWindowSize(window, width, height - sMetal.boundsHeight);
                doOnIdleRepeating(
                    [platform, width, height]()
                    {
                        if (sState.phase != Phase::MetalLogin || sMetal.scene != "zero")
                        {
                            return true;
                        }
                        LL::RHI::Device* device = LL::RHI::viewerDevice();
                        if (!device || LL::RHI::metalSurfaceStats(*device).presentsSkippedZero == sMetal.skippedBefore)
                        {
                            return false;
                        }
                        setWindowSize(platform, S32(width), S32(height));
                        return true;
                    });
                metalStep(1);
                return false;
            }
            const U64 skipped = LL::RHI::metalSurfaceStats(device).presentsSkippedZero - sMetal.skippedBefore;
            if (skipped == 0)
            {
                if (sMetal.frame - sMetal.stepFrame >= METAL_SETTLE_LIMIT)
                {
                    fail("metal login: zero: no present found a 0 in the surface's size; " + metalSizes(device, window));
                }
                return false;
            }
            if (!metalSettled(device, window, s, sMetal.boundsWidth, sMetal.boundsHeight))
            {
                return false;
            }
            if (skipped != 1)
            {
                fail(llformat("metal login: zero: %llu presents found a 0 in the surface's size, not 1",
                              (unsigned long long)skipped));
                return false;
            }
            sMetal.skippedZero += skipped;
            metalLog("zero: one present without a drawable, then back: " + metalSizes(device, window) + "; "
                     + metalCounts(device));
            return true;
        }
        if (scene == "scale")
        {
            // The test backing scale override through the view, which then makes AppKit's calls on a screen change.
            void* view = window->getNativeView();
            const F32 scale = F32(sState.config["metal_login"]["scale"].asReal());
            switch (sMetal.step)
            {
            case 0:
                metalSaveWindow(window);
                doOnIdleOneTime([view, scale]() { setOpenGLViewTestBackingScale(view, scale); });
                metalStep(1);
                return false;
            case 1:
                if (!metalSettled(device, window, metalScale(true), sMetal.boundsWidth, sMetal.boundsHeight))
                {
                    return false;
                }
                metalLog(llformat("scale: the test backing scale %g at RenderHiDPI %d: ", scale, gHiDPISupport ? 1 : 0)
                         + metalSizes(device, window));
                doOnIdleOneTime([view]() { setOpenGLViewTestBackingScale(view, 0.f); });
                metalStep(2);
                return false;
            default:
                if (!metalSettled(device, window, s, sMetal.boundsWidth, sMetal.boundsHeight))
                {
                    return false;
                }
                metalLog("scale: the test backing scale off: " + metalSizes(device, window));
                return true;
            }
        }
        // gates: the window's GL functions without a context, under the armed trap, which stops the process at any
        // CGL or gl call; the settings go through gSavedSettings, so that their handlers run.
        const bool vsync = gSavedSettings.getBOOL("RenderVSyncEnable");
        const U32 target_fps = gSavedSettings.getU32("TargetFPS");
        const bool sync_before = LL::RHI::metalSurfaceStats(device).displaySync;
        gSavedSettings.setBOOL("RenderVSyncEnable", !vsync);
        const bool sync_toggled = LL::RHI::metalSurfaceStats(device).displaySync;
        gSavedSettings.setBOOL("RenderVSyncEnable", vsync);
        const bool sync_back = LL::RHI::metalSurfaceStats(device).displaySync;
        gSavedSettings.setU32("TargetFPS", target_fps); // the handler lowers it to the refresh rate when it turns VSync on
        const bool mult_gl = gSavedSettings.getBOOL("RenderAppleUseMultGL");
        gSavedSettings.setBOOL("RenderAppleUseMultGL", !mult_gl);
        gSavedSettings.setBOOL("RenderAppleUseMultGL", mult_gl);
        window->afterDialog();
        void* shared = window->createSharedContext();
        const std::string line = llformat("RenderVSyncEnable %d -> %d -> %d with displaySync %d -> %d -> %d, "
                                          "RenderAppleUseMultGL %d -> %d -> %d, afterDialog(), createSharedContext() %s",
                                          vsync, !vsync, vsync, sync_before, sync_toggled, sync_back, mult_gl, !mult_gl,
                                          mult_gl, shared ? "gave a context" : "null");
        if (shared)
        {
            window->destroySharedContext(shared);
        }
        if (sync_before != vsync || sync_toggled != !vsync || sync_back != vsync || shared)
        {
            fail("metal login: gates: " + line);
            return false;
        }
        // The world-off gate in setStartupState, which the login plugin's attempt never reaches (the login
        // panel refuses it first): asked for directly, it keeps STATE_LOGIN_WAIT and shows its notification, which is
        // cancelled so that the login screen stays as it was.
        LLStartUp::setStartupState(STATE_LOGIN_CLEANUP);
        const EStartupState after = LLStartUp::getStartupState();
        LLNotifications::instance().cancelByName("MetalUIDevLoginOff");
        if (after != STATE_LOGIN_WAIT)
        {
            fail("metal login: gates: setStartupState(STATE_LOGIN_CLEANUP) left STATE_LOGIN_WAIT for "
                 + LLStartUp::getStartupStateString());
            return false;
        }
        metalLog("gates: passed under the armed trap: " + line
                 + ", setStartupState(STATE_LOGIN_CLEANUP) refused in STATE_LOGIN_WAIT");
        return true;
    }

    /// The pending program's check, every frame from its creation: Ready (reflection and pipeline) within
    /// METAL_PROGRAM_FRAMES, and hasPendingPublications() true while it is pending.
    void metalProgramFrame()
    {
        LLGLSLShader& program = *sMetal.program;
        const U32 frames = sMetal.frame - sMetal.programFrame;
        if (!program.isComplete())
        {
            fail("metal login: pending_program: the program failed");
            return;
        }
        if (!program.isReflectionReady())
        {
            ++sMetal.programPending;
            if (!LLViewerShaderMgr::instance()->hasPendingPublications())
            {
                fail(llformat("metal login: pending_program: the program is pending %u frames after its creation, while "
                              "hasPendingPublications() is false",
                              frames));
                return;
            }
        }
        if (!program.isReflectionReady() || !program.isPipelineReady())
        {
            if (frames >= METAL_PROGRAM_FRAMES)
            {
                fail(llformat("metal login: pending_program: not Ready %u frames after its creation (%s)", frames,
                              program.isReflectionReady() ? "published, its pipeline not compiled" : "still pending"));
            }
            return;
        }
        metalLog(llformat("pending_program: Ready %u frames after its creation, pending at %u of the hook's frames", frames,
                          sMetal.programPending));
        program.unload();
        sMetal.program.reset();
    }

    /// The LLGLManager fields the viewer reads outside llgl.cpp, as one line for the comparison of the Metal runs with the
    /// GL run (the hook's own list, independent of the CGL store's). Not the raw strings, which only the CGL query
    /// sets; the extensions as their count and digest.
    void metalLogGLManager()
    {
        const LLGLManager& gl = gGLManager;
        std::string extensions;
        for (const std::string& name : gl.mGLExtensions)
        {
            extensions += name + " ";
        }
        const std::string digest = md5Of(reinterpret_cast<const U8*>(extensions.data()), extensions.size());
        std::ostringstream line;
        line << "mInited " << gl.mInited << ", mIsDisabled " << gl.mIsDisabled << ", mHasRequirements " << gl.mHasRequirements
             << ", mGLVendor '" << gl.mGLVendor << "', mGLVendorShort '" << gl.mGLVendorShort << "', mGLRenderer '"
             << gl.mGLRenderer << "', mGLVersion " << gl.mGLVersion << ", mGLVersionString '" << gl.mGLVersionString
             << "', mDriverVersionVendorString '" << gl.mDriverVersionVendorString << "', mGLSLVersionMajor "
             << gl.mGLSLVersionMajor << ", mGLSLVersionMinor " << gl.mGLSLVersionMinor << ", mVRAM " << gl.mVRAM
             << ", mIsApple " << gl.mIsApple << ", mIsIntel " << gl.mIsIntel << ", mIsNVIDIA " << gl.mIsNVIDIA << ", mIsAMD "
             << gl.mIsAMD << ", mIsMobileGF " << gl.mIsMobileGF << ", mHasCubeMapArray " << gl.mHasCubeMapArray
             << ", mHasAnisotropic " << gl.mHasAnisotropic
             << ", mHasDebugOutput " << gl.mHasDebugOutput << ", mHasNVXGpuMemoryInfo " << gl.mHasNVXGpuMemoryInfo
             << ", mHasAMDAssociations " << gl.mHasAMDAssociations << ", mMaxAnisotropy " << gl.mMaxAnisotropy
             << ", mNumTextureImageUnits " << gl.mNumTextureImageUnits << ", mMaxUniformBlockSize "
             << gl.mMaxUniformBlockSize << ", mGLMaxTextureSize " << gl.mGLMaxTextureSize << ", mGLMaxVertexRange "
             << gl.mGLMaxVertexRange << ", mGLMaxIndexRange " << gl.mGLMaxIndexRange << ", mMaxSamples " << gl.mMaxSamples
             << ", mMaxVaryingVectors " << gl.mMaxVaryingVectors << ", mAliasedLineRange " << gl.mAliasedLineRange.mV[VX] << " "
             << gl.mAliasedLineRange.mV[VY] << ", mDownScaleMethod " << gl.mDownScaleMethod << ", mGLExtensions "
             << gl.mGLExtensions.size() << " " << digest;
        LL_INFOS("Golden") << "Golden test: gl manager values: " << line.str() << LL_ENDL;
    }

    void metalStart(LL::RHI::Device& device, LLWindow* window)
    {
        const LL::RHI::MetalSurfaceStats stats = LL::RHI::metalSurfaceStats(device);
        sMetal.started = true;
        sMetal.metal = device.backend() == LL::RHI::BackendType::Metal;
        sMetal.baseFrames = device.frameNumber();
        sMetal.basePresents = stats.presents;
        sMetal.baseSwaps = display_window_swaps();
        sMetal.skippedZero = stats.presentsSkippedZero;
        sMetal.uiSize = window->getSystemUISize();
        for (const auto& [name, frame] : llsd::inMap(sState.config["metal_login"]["scenes"]))
        {
            sMetal.scenes.emplace_back(U32(frame.asInteger()), name);
        }
        std::stable_sort(sMetal.scenes.begin(), sMetal.scenes.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::ostringstream line;
        line << "start on " << (sMetal.metal ? "Metal" : "GL") << " at RenderHiDPI " << (gHiDPISupport ? 1 : 0)
             << ", window " << gViewerWindow->getWindowWidthRaw() << "x" << gViewerWindow->getWindowHeightRaw()
             << ", system UI size " << sMetal.uiSize;
        if (sMetal.metal)
        {
            line << ", " << metalSizes(device, window) << ", displaySync " << stats.displaySync;
        }
        line << ", " << sMetal.scenes.size() << " scenes in " << sState.config["metal_login"]["frames"].asInteger()
             << " frames";
        metalLog(line.str());
        metalLogGLManager();
        // No present has found a 0 before the zero scene, and s is 1 at RenderHiDPI 0.
        if (sMetal.metal && (sMetal.skippedZero != 0 || (!gHiDPISupport && sMetal.uiSize != 1.f)))
        {
            fail("metal login: at the start " + metalSizes(device, window) + "; " + metalCounts(device));
            return;
        }
        if (sMetal.metal)
        {
            const std::string why = metalSizesWhy(device, window, metalScale(false), F32(metalBounds(window).width),
                                                  F32(metalBounds(window).height));
            if (!why.empty())
            {
                fail("metal login: at the start " + why);
            }
        }
    }

    void metalFinish(LL::RHI::Device& device, LLWindow* window)
    {
        sMetal.done = true;
        // scaleDown runs on Metal too; the login screen does not reach it deterministically, so its counts are
        // information
        metalLog("end: " + metalCounts(device) + llformat("; scaleDown %u times, %u refused", LLImageGL::sScaleDowns,
                                                          LLImageGL::sScaleDownsRefused));
        LLCoordWindow size;
        window->getSize(&size);
        metalLog(llformat("window %dx%d, getSize %dx%d, system UI size %g", gViewerWindow->getWindowWidthRaw(),
                          gViewerWindow->getWindowHeightRaw(), size.mX, size.mY, window->getSystemUISize()));
        LL_INFOS("Golden") << "Golden test: metal login passed after " << sMetal.frame << " frames" << LL_ENDL;
        if (sState.config["quit"].asBoolean())
        {
            finish(true);
        }
    }
}

/// The Metal login hook's frame (metal_login), from beforeStartupSwap.
static void metalLoginFrame()
{
    const EStartupState state = LLStartUp::getStartupState();
    if (state < STATE_LOGIN_WAIT)
    {
        return;
    }
    if (state > STATE_LOGIN_WAIT)
    {
        fail("the viewer left the login screen during the Metal login hook");
        return;
    }
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    LLWindow* window = gViewerWindow->getWindow();
    if (!sMetal.started)
    {
        // As the login screen's run: the window at the config's size, and the config's login page when it has one.
        if (!sState.loginSized)
        {
            if (++sState.frames >= SIZING_FRAMES && sizeWindow())
            {
                sState.loginSized = true;
                sState.frames = 0;
                if (!sState.loginPage.empty())
                {
                    showLoginPage();
                }
            }
            return;
        }
        if (!sState.loginPage.empty() && !isLoginPage(sLoginPageObserver.loaded))
        {
            return;
        }
        if (!device || !device->surface() || !window->getPlatformWindow())
        {
            fail("metal login: no render interface device with a surface, or no window");
            return;
        }
        metalStart(*device, window);
        if (sState.phase != Phase::MetalLogin)
        {
            return;
        }
    }
    if (!sState.loginPage.empty() && !isLoginPage(sLoginPageObserver.loaded))
    {
        fail("the login panel's browser left the run's login page");
        return;
    }
    // The resize scene's wait for the login page runs on wall-clock time and is not counted, so the scenes start at the
    // config's frames after it and the hook passes after the config's frames (metalLoginPageBack)
    if (sMetal.scene != "resize" || sMetal.step != 3)
    {
        ++sMetal.frame;
    }
    if (sMetal.scene.empty() && sMetal.next < sMetal.scenes.size() && sMetal.scenes[sMetal.next].first <= sMetal.frame)
    {
        sMetal.scene = sMetal.scenes[sMetal.next++].second;
        metalStep(0);
    }
    if (!sMetal.scene.empty() && metalScene(*device, window))
    {
        sMetal.scene.clear();
    }
    if (sState.phase == Phase::MetalLogin && sMetal.program)
    {
        metalProgramFrame();
    }
    if (sState.phase != Phase::MetalLogin)
    {
        return;
    }
    const std::string why = metalCountersWhy(*device);
    if (!why.empty())
    {
        fail(llformat("metal login: frame %u: ", sMetal.frame) + why);
        return;
    }
    if (!sMetal.done && sMetal.scene.empty() && sMetal.next == sMetal.scenes.size() && !sMetal.program
        && sMetal.frame >= U32(sState.config["metal_login"]["frames"].asInteger()))
    {
        metalFinish(*device, window);
    }
}
#endif // LL_RHI_METAL

void LLGoldenTest::beforeStartupSwap()
{
#if LL_RHI_METAL
    if (sState.phase == Phase::MetalLogin)
    {
        metalLoginFrame();
        return;
    }
#endif
    if (sState.phase != Phase::LoginScreen)
    {
        return;
    }
    // After this frame's checks and readback, the golden login mode fills the back buffer with a
    // signal colour, so a later frame whose display_startup draws without clearing first shows it, and the login
    // screen's A/B fails. The clear colour goes back to (0, 0, 0, 0), which display_startup clears with at the login
    // screen (the A/B's base, at 0 px, proves it). Nothing of this runs in a normal start.
    struct SignalFill
    {
        ~SignalFill()
        {
            LLRenderTarget::setClearColor(1.f, 0.f, 1.f, 1.f);
            LLRenderTarget::clearCurrent(GL_COLOR_BUFFER_BIT);
            LLRenderTarget::setClearColor(0.f, 0.f, 0.f, 0.f);
        }
    } signal_fill;
    const EStartupState state = LLStartUp::getStartupState();
    if (state < STATE_LOGIN_WAIT)
    {
        return;
    }
    if (state > STATE_LOGIN_WAIT)
    {
        fail("the viewer left the login screen before the snapshot");
        return;
    }
    if (!sState.loginSized)
    {
        // As in-world, the window system gets SIZING_FRAMES to resize the window.
        if (++sState.frames >= SIZING_FRAMES && sizeWindow())
        {
            sState.loginSized = true;
            sState.frames = 0;
            LL_INFOS("Golden") << "Golden test: window " << gViewerWindow->getWindowWidthRaw() << "x"
                               << gViewerWindow->getWindowHeightRaw() << " at the login screen" << LL_ENDL;
            showLoginPage();
        }
        return;
    }
    // The frames count from the browser's last load of the run's login page.
    if (!isLoginPage(sLoginPageObserver.loaded))
    {
        if (sState.loginPageLoaded)
        {
            fail("the login panel's browser left the run's login page");
        }
        return;
    }
    sState.loginPageLoaded = true;
    if (sLoginPageObserver.loads != sState.loginPageLoads)
    {
        sState.loginPageLoads = sLoginPageObserver.loads;
        sState.frames = 0;
    }
    const LLSD& config = sState.config;
    if (++sState.frames < U32(config["login_screen"].asInteger()) || !onLocalGrid())
    {
        return;
    }
    // The mouse outside the window, as with the UI in-world: over the login panel its hover
    // would highlight a control or show a tooltip.
    std::string why = mouseOverWindow();
    if (!why.empty())
    {
        fail(why);
        return;
    }
    // The login screen's run has no transfer checks, so the frame saveFrame read is not kept.
    LLPointer<LLImageRaw> frame;
    if (!saveFrame(config["snapshot"].asString(), why, frame)
        || !writeRenderProfile(config["render_profile"].asString(), why))
    {
        fail(why);
        return;
    }
    LL_INFOS("Golden") << "Golden test: login screen after " << sState.frames << " frames; quitting without a login"
                       << LL_ENDL;
    finish(true);
}

void LLGoldenTest::beforeSwap()
{
    if (sState.phase == Phase::Sizing || sState.phase == Phase::Moving || sState.phase == Phase::Loading
        || sState.phase == Phase::Warming || sState.phase == Phase::Previews || sState.phase == Phase::Media)
    {
        keepUI();
        keepGLTF();
    }
    switch (sState.phase)
    {
    case Phase::Unchecked:
        // init() sets the graphics up before the pipeline; without it the run would not be the same.
        readConfig();
        if (sState.phase == Phase::WaitingForWorld || sState.phase == Phase::LoginScreen
            || sState.phase == Phase::MetalLogin)
        {
            fail("the hook was not set up at startup");
        }
        return;
    case Phase::LoginScreen:
    case Phase::MetalLogin:
        // swap() runs from STATE_PRECACHE on, after the login began.
        fail("the viewer left the login screen before the snapshot");
        return;
    case Phase::Off:
    case Phase::Transfers: // the checks run from the idle callback
    case Phase::Done:
        return;
    case Phase::Previews:
        stepPreviews();
        return;
    case Phase::Media:
        stepMedia();
        return;
    case Phase::WaitingForWorld:
        if (LLStartUp::getStartupState() == STATE_STARTED && gAgent.getRegion())
        {
            if (!onLocalGrid())
            {
                return;
            }
            keepUI();
            // [Viewer Lab] A tour's first stop sets Sizing itself.
            if (sTour.active())
            {
                startTour();
                return;
            }
            sState.phase = Phase::Sizing;
            sState.frames = SIZING_FRAMES;
        }
        return;
    case Phase::Sizing:
        // The glTF run's shaders load again before the scene is set up, so the reload is at the same point of every
        // run and never while the avatar moves or the scene loads.
        if (sState.config.has("gltf") && !sState.gltfOn)
        {
            // [Viewer Lab] The frames and a floor on the wall clock
            if (ll_golden_wait_over(sState.gltfWaitFrames, sState.gltfWaitSince, LLTimer::getTotalSeconds(),
                                    MOVE_TIMEOUT_FRAMES, GLTF_WAIT_SECONDS))
            {
                fail("gltf: GLTFEnabled is not on yet: the region's features did not come");
            }
            return;
        }
        if (++sState.frames >= SIZING_FRAMES && sizeWindow())
        {
            // Where the config wants the window (show_ui), once it has its size, which keeps it.
            if (const LLSD& origin = sState.config["window_origin"]; origin.isArray())
            {
                gViewerWindow->getWindow()->setPosition(LLCoordScreen(origin[0].asInteger(), origin[1].asInteger()));
                LLCoordScreen position;
                gViewerWindow->getWindow()->getPosition(&position);
                LL_INFOS("Golden") << "Golden test: window moved to " << origin[0].asInteger() << ", " << origin[1].asInteger()
                                   << " (content at " << position.mX << ", " << position.mY << ")" << LL_ENDL;
            }
            LL_INFOS("Golden") << "Golden test: window " << gViewerWindow->getWindowWidthRaw() << "x"
                               << gViewerWindow->getWindowHeightRaw() << LL_ENDL;
            startScene();
        }
        return;
    case Phase::Moving:
        if (avatarArrived())
        {
            // Home is the scene's avatar spot from now on (askForHomeOnSpot), and the test profile logs
            // in at home (LoginLocation), so later logins arrive out of the camera's view; without a home the account
            // arrives in the region's middle. A run with the UI (show_ui) has its avatar in view
            // for the name tag, so it leaves the home where the other scenes put it. It is asked for when
            // the probes start over (notReady, homeNotConfirmed).
            sState.setHome = !sState.config["show_ui"].asBoolean();
            setCamera();
            // The floaters open now, so the scene's readiness waits for their textures too (show_ui).
            if (!openFloaters())
            {
                return;
            }
            sState.phase = Phase::Loading;
            sState.frames = 0;
            sState.readyFrames = 0;
        }
        // [Viewer Lab] The frames and a floor on the wall clock: with vsync off 1800 frames are about 1 s
        else if (ll_golden_wait_over(sState.frames, sState.movingSince, LLTimer::getTotalSeconds(), MOVE_TIMEOUT_FRAMES,
                                     MOVE_TIMEOUT_SECONDS))
        {
            fail("the avatar did not arrive");
        }
        return;
    case Phase::Loading:
    {
        if (!sState.environmentLocked)
        {
            sState.environmentLocked = lockEnvironment();
        }
        // [Viewer Lab] Whether the eye has stopped moving to the test camera (notReady): still in the last
        // frame, as in a run outside a tour; in a later tour stop still and at the scene's camera position, which the first frame
        // after setCamera never is (ll_golden_eye_settled), or still for long enough anywhere, as the
        // viewer may keep the eye off the position.
        const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
        sState.eyeFromCamera = dist_vec(eye, gAgent.getPosAgentFromGlobal(sState.cameraEye));
        sState.eyeSettled = ll_golden_eye_settled(laterTourStop(), dist_vec(eye, sState.lastEye), sState.eyeFromCamera,
                                                  LLTimer::getTotalSeconds(), sState.eyeStillFrames,
                                                  sState.eyeStillSince);
        sState.lastEye = eye;
        const std::string why = notReady();
        sState.readyFrames = why.empty() ? sState.readyFrames + 1 : 0;
        sState.mostReady = llmax(sState.mostReady, sState.readyFrames);
        if (!why.empty())
        {
            sState.lastWhy = why;
        }
        if (sState.frames > 0 && sState.frames % NOT_READY_LOG_FRAMES == 0)
        {
            LL_INFOS("Golden") << "Golden test: not ready after " << sState.frames << " frames: " << sState.lastWhy
                               << " (ready at most " << sState.mostReady << " frames in a row)" << LL_ENDL;
        }
        if (sState.readyFrames >= READY_FRAMES)
        {
            // From here on every frame is the same: the test clock (running since the probes started
            // over) and a fixed seed.
            ll_seed_rand(SEED);
            // The auto-exposure starts over from this frame: its history from loading would change
            // every frame after.
            gPipeline.resetExposureHistory();
            sState.readyAfter = sState.frames;
            LL_INFOS("Golden") << "Golden test: ready after " << sState.frames << " frames (textures in use: "
                               << sState.unavailable << " missing, " << sState.unrequested << " not asked for, "
                               << sState.unrequestedBoosted << " of them boosted; settled sharper "
                               << sState.settledSharper << ", " << sState.settledSharperAbove0
                               << " above level 0); warming up on the test clock" << LL_ENDL;
            // The warm-up counts the render device's frames and the main loop's.
            LL::RHI::Device* device = LL::RHI::viewerDevice();
            if (!device)
            {
                fail("no render interface device");
                return;
            }
            sState.deviceFrameAtWarmup = device->frameNumber();
            sState.viewerFrameAtWarmup = LLFrameTimer::getFrameCount();
            sState.transferCounts = LLSD();
            sState.transferCounts["warmup_start"] = facadeCounts();
            sState.drawsAtWarmup = LLVertexBuffer::drawCounts();
            std::copy(std::begin(LLPipeline::sReadOnlySites), std::end(LLPipeline::sReadOnlySites),
                      std::begin(sState.readOnlySitesAtWarmup));
            sState.drawsAtFrame = sState.drawsAtWarmup;
            sState.ssrCopiesAtWarmup = gPipeline.mScreenSpaceReflectionCopies;
            sState.drawsMost = LLVertexBuffer::DrawCounts();
            sState.drawFrames = 0;
            sState.drawsAfterFirst = LLVertexBuffer::DrawCounts();
            sState.drawsMostAfterFirst = LLVertexBuffer::DrawCounts();
            sState.scaleDownsAtFrame = LLImageGL::sScaleDowns;
            sState.scaleDownsAfterFirst = 0;
            sState.scaleDownsMostAfterFirst = 0;
            sState.dynamicStateAtWarmup = LLRender::dynamicStateCounts();
            sState.dynamicStateAtFrame = sState.dynamicStateAtWarmup;
            sState.dynamicStateMost = LLRender::DynamicStateCounts();
            sState.texturesAtWarmup = LLTexUnit::bindCounts();
            sState.texturesAtFrame = sState.texturesAtWarmup;
            sState.typeSlotReadsAtWarmup = metalTypeSlotReads();
            sState.depthBiasChangesAtWarmup = metalDepthBiasChanges();
            LLVertexBuffer::clearProgramTally();
            LLVertexBuffer::sTallyPrograms = true;
            sState.texturesMost = LLTexUnit::BindCounts();
            sState.textureFrames = 0;
            sState.phase = Phase::Warming;
            sState.frames = 0;
            // render_debug_late's displays on before the warm-up's first frame, which draws them first.
            keepRenderDebug();
            // And the selected light and the snapshot preview.
            if (!startWarmupUI())
            {
                return;
            }
            // The own avatar's local rebake, whose bakes render in the warm-up's first frames.
            if (sState.config["rebake"].asBoolean())
            {
                if (!isAgentAvatarValid())
                {
                    fail("rebake: there is no own avatar");
                    return;
                }
                (void)DynamicTextureCounts::takeRenders();
                gAgentAvatarp->forceBakeAllTextures(true);
                LL_INFOS("Golden") << "Golden test: rebake asked for at frame " << LLFrameTimer::getFrameCount() << LL_ENDL;
            }
            // The selected objects' silhouettes are made anew in the warm-up's first frame, from the settled scene:
            // LLSelectMgr::updateSilhouettes keeps the ones it made when the objects were selected, while the scene
            // was loading, which differed from run to run.
            for (const auto& [name, id] : sState.selected)
            {
                if (LLViewerObject* object = gObjectList.findObject(id))
                {
                    object->setChanged(LLXform::SILHOUETTE);
                }
            }
            // The drag starts on the frame's manipulator, so the warm-up's first frame draws it dragged.
            if (sState.config["drag"].isString() && !startDrag())
            {
                return;
            }
            // The scene monitor's captures count from here.
            if (sState.config["monitor_check"].asBoolean())
            {
                sState.monitorLast = LLSceneMonitor::getInstance()->getLastCaptureTarget();
                sState.monitorCaptures = 0;
            }
            // The glTF run's first load, so the asset's first draws come in the warm-up's first frame.
            if (sState.config.has("gltf") && !loadGLTF())
            {
                return;
            }
        }
        // [Viewer Lab] The frames and a floor on the wall clock
        else if (ll_golden_wait_over(sState.frames, sState.loadingSince, LLTimer::getTotalSeconds(), LOAD_TIMEOUT_FRAMES,
                                     LOAD_TIMEOUT_SECONDS))
        {
            fail(llformat("the scene did not get ready: %s (ready at most %d frames in a row)", sState.lastWhy.c_str(),
                          sState.mostReady));
        }
        return;
    }
    case Phase::Warming:
    {
        noteFrameDraws();
        noteFrameTextures();
        // The mouse point this frame's updateUI raycast at, and still at the swap (cursor).
        if (sState.cursorLocked)
        {
            const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
            if (mouse.mX != sState.cursorPoint.mX || mouse.mY != sState.cursorPoint.mY)
            {
                if (sState.cursorMoved++ == 0)
                {
                    LL_WARNS("Golden") << "Golden test: the mouse point moved to " << mouse.mX << ", " << mouse.mY
                                       << " in warm-up frame " << sState.frames << LL_ENDL;
                }
            }
        }
        if (sState.config.has("gltf"))
        {
            // Whether this frame's debug ray hit the host's asset (LLViewerWindow::updateUI's cursorIntersect, which
            // GLTFSceneManager::renderDebug's own raycast repeats), then the second load.
            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_RAYCAST))
            {
                ++sState.gltfRayFrames;
                if (gDebugRaycastObject && gDebugRaycastObject->getID() == sState.gltfHost && gDebugRaycastGLTFNodeHit >= 0)
                {
                    ++sState.gltfRayHits;
                }
            }
            if (sState.frames + 1 == GLTF_RELOAD_FRAME && !loadGLTF())
            {
                return;
            }
        }
        // The frame's dynamic texture renders, after display's updateAllInstances (rebake).
        if (sState.config["rebake"].asBoolean())
        {
            if (const S32 renders = DynamicTextureCounts::takeRenders(); renders > 0)
            {
                ++sState.bakeFrames;
                sState.bakeRenders += renders;
            }
            sState.previewTextures = std::max(sState.previewTextures, DynamicTextureCounts::previews());
        }
        // The drag holds the mouse to the snapshot; level2.py fails a run whose manipulator lost it.
        if (sState.dragPinned && sState.dragProbes > 0)
        {
            LLManip* manip = selectionManip();
            if (!manip || !manip->hasMouseCapture())
            {
                LLMouseHandler* captor = gFocusMgr.getMouseCapture();
                LL_WARNS("Golden") << "Golden test: the drag lost the mouse in warm-up frame " << sState.frames << " to "
                                   << (captor ? captor->getName() : std::string("nothing")) << LL_ENDL;
                sState.dragProbes = -sState.dragProbes; // logged once; measured.manip.drag keeps the count's size
            }
        }
        // A capture swaps the scene monitor's two targets, so its last target changes once per capture.
        if (sState.config["monitor_check"].asBoolean())
        {
            if (const LLRenderTarget* last = LLSceneMonitor::getInstance()->getLastCaptureTarget(); last != sState.monitorLast)
            {
                ++sState.monitorCaptures;
                sState.monitorLast = last;
            }
        }
        if (++sState.frames < U32(sState.config["warmup_frames"].asInteger()))
        {
            return;
        }
        // Still ready, and nothing pushed textures down: an unfocused window can raise the bias.
        std::string why = notReady();
        LL_INFOS("Golden") << "Golden test: the window " << (gFocusMgr.getAppHasFocus() ? "has" : "does not have")
                           << " focus, discard bias " << LLViewerTexture::sDesiredDiscardBias << LL_ENDL;
        if (!why.empty())
        {
            fail("the scene was no longer ready after warming up: " + why);
            return;
        }
        if (sState.config["show_ui"].asBoolean())
        {
            why = uiNotAsConfigured();
            if (!why.empty())
            {
                fail("the UI is not as the config has it: " + why);
                return;
            }
        }
        // One frame of the render device per main-loop frame.
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        if (!device)
        {
            fail("no render interface device");
            return;
        }
        const U64 device_frames = device->frameNumber() - sState.deviceFrameAtWarmup;
        const U32 viewer_frames = LLFrameTimer::getFrameCount() - sState.viewerFrameAtWarmup;
        LL_INFOS("Golden") << "Golden test: the render device ran " << device_frames << " frames in " << viewer_frames
                           << " main loop frames" << LL_ENDL;
        if (device_frames != viewer_frames || device_frames == 0)
        {
            fail(llformat("the render device ran %llu frames in %u main loop frames", (unsigned long long)device_frames,
                          viewer_frames));
            return;
        }
        // The test fault failed a creation, and with times a whole number of shader loads' worth;
        // level2.py checks the number against the loads in the log.
        // Each fault of Metal's list is checked the same way.
        std::vector<std::pair<const LL::RHI::ProgramFault*, U32>> armed_faults;
        if (const LL::RHI::ProgramFault& fault = sState.programFault; !fault.program.empty())
        {
            armed_faults.emplace_back(&fault, LL::RHI::programFaultHits());
        }
#if LL_RHI_METAL
        for (const LL::RHI::ProgramFault& listed : sState.programFaults)
        {
            armed_faults.emplace_back(&listed, LL::RHI::metalProgramFaultHits(listed.program));
        }
#endif
        for (const auto& [fault, hits] : armed_faults)
        {
            if (hits == 0)
            {
                fail("the program fault on " + fault->program + " failed no creation of it");
                return;
            }
            if (fault->times > 0 && hits % fault->times != 0)
            {
                fail(llformat("the program fault on %s failed %u creations of it, not a multiple of times %u",
                              fault->program.c_str(), hits, fault->times));
                return;
            }
        }
        // The preview run's steps come first, and the snapshot is the last step's frame.
        if (sState.config["previews"].isArray())
        {
            LL_INFOS("Golden") << "Golden test: warmed up; " << sState.config["previews"].size() << " preview steps"
                               << LL_ENDL;
            sState.phase = Phase::Previews;
            sState.frames = 0;
            sState.previewStep = 0;
            return;
        }
        // The media phase's windows come before the snapshot; its first frame is timed from this swap.
        if (sState.config["media"].isMap())
        {
            LL_INFOS("Golden") << "Golden test: warmed up; the media phase's baseline window of "
                               << sState.config["media"]["frames"].asInteger() << " frames" << LL_ENDL;
            sState.phase = Phase::Media;
            sState.frames = 0;
            sState.mediaStep = 0;
            sState.mediaFrames.clear();
            sState.mediaCountsAt = LLViewerMediaImpl::texUpdateCounts();
            sState.mediaLastSwap = std::chrono::steady_clock::now();
            sState.mediaCostAtSwap = sState.mediaCountsAt.mainUs + sState.mediaCountsAt.imageUs;
            sState.mediaUpdatesAtSwap = sState.mediaCountsAt.updates;
            return;
        }
        snapshotAndFinish();
        return;
    }
    }
}
