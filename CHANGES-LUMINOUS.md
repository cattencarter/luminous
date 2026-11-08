# What Luminous changes in the source it builds on

Luminous is based on the source code of the **Phoenix Firestorm Viewer**,
released under the GNU Lesser General Public License version 2.1 — the same
licence as the work it is based on. The LGPL requires a
derivative to state what it changed in that work. This is that statement; it
names upstream files and features where Luminous changes them, which
implies no involvement of the upstream projects in Luminous.

Upstream baseline: the 7.2.5 release, `Firestorm_Release_7.2.5.81788`
(`4ac201fdc5`, on Linden Lab's 26.3). Edits to upstream files are marked by
comments in the code: `<Luminous>`, `<Lumen>`, or `[Viewer Lab]` for the
render device layer below. Some file and setting names keep an earlier
working name, `lumen`.

To see the changes exactly, rather than taking this file's word for it,
compare with the upstream commit Luminous is based on:

```sh
git fetch https://github.com/FirestormViewer/phoenix-firestorm.git tag Firestorm_Release_7.2.5.81788
git diff 4ac201fdc56127de27de34567286885f4e75d194 HEAD
```

Luminous is built for macOS only, on Apple silicon. The upstream Windows and
Linux files stay in the tree, renamed, so that merging from upstream stays
simple.

## Identity

| Where | Change |
|---|---|
| `indra/llcommon/indra_constants.h`, `indra/cmake/BuildVersion.cmake` | the application name, and Luminous's own version number (`LUMEN_VERSION`), which is what Second Life, the window title, About and Finder see; the upstream number is kept in `VIEWER_VERSION_FS.txt` |
| `indra/newview/llversioninfo.{h,cpp}`, `fsversionvalues.h.in` | the upstream version kept beside Luminous's, for Luminous's bug reports |
| `indra/llfilesystem/lldir_mac.cpp` | the macOS data directory, so Luminous never shares another viewer's settings |
| `indra/newview/CMakeLists.txt` | product name, bundle identifier, bundle name, copyright |
| `indra/newview/English.lproj/InfoPlist.strings`, `Firestorm.xib` | the names macOS shows in the menu bar and About panel |
| `indra/newview/viewer_manifest.py`, `fs_viewer_manifest.py`, `indra/lib/python/indra/util/llmanifest.py` | package as `Luminous.app`; release and symbol file names; the channel vendor base; Luminous's own resources packaged |
| `indra/newview/icons/`, `res/`, `luminous_icon.icns`, `skins/default/textures/windows/login_fs_logo.png` | Luminous's icon and logo |
| `indra/newview/installers/windows/*`, `res/viewerRes.rc` | the Windows installer and executable metadata |
| `indra/newview/llappviewer.cpp` | the window title; one session settings file instead of five login modes; the remote settings-defaults override (FSData) no longer loaded |
| `indra/newview/fsdata.cpp`, `fsgridhandler.cpp`, `llviewernetwork.cpp`, `llviewerhelp.cpp`, `llurlentry.cpp`, `llerror.cpp`, `lltransutil.cpp`, `llvelopack.cpp`, `app_settings/grids.xml` | no downloads from, links to, or support requests sent to the upstream project's servers and volunteers; download and support links go to needfulsl.com; only Second Life's main and beta grids |
| `indra/newview/skins/default/xui/*/strings.xml`, `notifications.xml`, `floater_about.xml`, `menu_viewer.xml` | the name, download and release-notes URLs; "Luminous by Needful" in About and the Help menu |

## Interface

| Where | Change |
|---|---|
| `indra/newview/skins/lumen/`, `skins/skins.xml`, `skins/default/colors.xml`, `textures/` | one skin of our own, teal rather than orange; the upstream skins are not shipped |
| `indra/newview/skins/default/html/en-us/lumen/`, `fspanellogin.{h,cpp}` | a local login start page instead of a web page |
| `indra/newview/app_settings/settings_lumen.xml` | the single session settings file that replaces the five login modes |
| `indra/newview/luminousfloater.{h,cpp}`, `skins/default/xui/en/floater_luminous.xml`, `llviewerfloaterreg.cpp`, `llstartup.cpp` | Luminous's own window, a sidebar on the right edge: a column of tabs (Scene, Tone, Colour, Effects, Render, Save / Load, Settings) that opens folded at every login, never at the login screen; each effect has one button cycling off, live, and only in the render; hints with pictures of an effect without and with it |
| `indra/newview/llscreenchannel.{h,cpp}` | the notification icons and notices stay left of the sidebar |
| `indra/newview/quickprefs.cpp`, `skins/default/xui/en/floater_phototools.xml` | Phototools keeps its environment, aids and camera tabs; its picture settings are in Luminous's window |
| `indra/newview/llfloaterpreference.cpp`, `skins/default/xui/en/floater_preferences*.xml`, `panel_preferences_*.xml` | Preferences hide the graphics controls Luminous's window sets (ambient occlusion, HDR and tonemapping, exposure, sharpening, glow, depth of field) |
| `indra/newview/llfloatersnapshot.cpp`, `skins/default/xui/en/notifications.xml` | the snapshot window says once that it does not show Luminous's effects, and points to Render |
| `indra/newview/llfloaterenvironmentadjust.{h,cpp}`, `skins/default/xui/en/floater_adjust_environment.xml` | Personal Lighting also sets haze distance and density, maximum cloud altitude, and the moon's scale and brightness |
| `indra/llui/lltooltip.cpp` | a new hint shows as soon as the pointer leaves the last one's area in either direction, not only in both |
| `indra/newview/llagentcamera.cpp` | the camera orbits round the avatar without turning her (`LuminousOrbitTurnsAvatar` brings the old way back) |
| `indra/newview/app_settings/settings.xml` | `FSTurnAvatarToSelectedObject` off by default; the Avatar Welcome Pack no longer opens by itself on the first login (`llstartup.cpp`, `llviewerwindow.cpp`) |
| `indra/newview/skins/default/toolbars.xml`, `xui/en/*` | default toolbar and wording changes |
| `indra/newview/luminousoptimise.{h,cpp}`, `llappviewer.cpp`, `floater_luminous_optimise.xml` | at the very first start, settings for the Mac: the graphics level from the chip's graphics cores and the backend (upstream starts every Apple GPU at Mid-High, without shadows, transparent water or ambient occlusion), SMAA, and the live view at Standard, one pixel per point, or smaller where the chip draws fewer pixels well (never MetalFX upscaling); and an Optimise window that measures where the user stands and proposes the fewest changes. On Metal with MetalFX upscaling off, where Preview resolution Half would draw fewer pixels than the live view does, it measures that first, at Half: where it reaches the aim it is proposed alone, where it helps but does not the effects are measured with it on, and where it does not help it is left out. With FXAA it proposes SMAA, with Preview resolution three quarters only where the live view is drawn larger than that |
| `indra/newview/featuretable_mac.txt` | no graphics level chooses FXAA, which keeps the live view at full size; SMAA instead |
| `indra/newview/luminousfloater.{h,cpp}`, `skins/default/xui/en/menu_viewer.xml`, `panel_preferences_graphics1.xml`, `app_settings/settings.xml` | the sidebar can be switched off under World > Photo and Video (Luminous Sidebar); Preferences > Graphics has a Luminous tab with the costlier effects, each Off, On or Only in renders, on the same settings as the sidebar's buttons |
| `indra/llui/llslider.{h,cpp}`, `llsliderctrl.h`, `indra/newview/luminousfloater.{h,cpp}` | Luminous's sliders: one that runs both ways about 0 (exposure, white balance, tint, split tone, distortion, clarity and the like) catches at 0 when a drag crosses it (and holds there until pulled a few pixels away; a drag from one side gives the values next to 0), the Look's contrast and saturation at 1; a double click (or Cmd-click) on any of them puts that one slider back to its default; Firestorm's own sliders are unchanged |

## Photo rendering

Luminous's rendering is always on. The costly effects are drawn only in a
rendered picture unless the user sets them live; where the upstream viewer has
its own, the live view shows that in their place.

| Where | Change |
|---|---|
| `indra/newview/luminousphotomode.{h,cpp}` | the effects' settings, their passes before exposure and tonemapping, which effects draw live and which only in a render, and the hooks below |
| `indra/newview/pipeline.{h,cpp}`, `llviewershadermgr.{h,cpp}`, `llviewercontrol.cpp` | where those passes, shaders and buffers are hooked in, and rebuilt when a setting changes (once for a whole style, preset or reset); HDR always on |
| `indra/newview/app_settings/settings.xml`, `settings_per_account.xml` | the `Luminous*` settings |
| `indra/newview/app_settings/shaders/class1/deferred/luminousVolumetricLightF.glsl` | volumetric light: single scattering along each view ray, wherever the sun shadow map says the sun reaches. The idea, and the shadow-map marching, are Black Dragon's (NiranV Dean) |
| `.../luminousProjectorBeamsF.glsl` | projector beams: a projector's light seen in the air from its apex, with its projected texture, Linden Lab's falloff and, for the two projectors that cast shadows, their spot shadow maps |
| `.../luminousSunShaft{Mask,Blur,s}F.glsl` | sun shafts, with stars kept out, after Kenny Mitchell's "Volumetric Light Scattering as a Post-Process" (GPU Gems 3) |
| `class1/deferred/shadowUtil.glsl`, `class2/deferred/sunLightF.glsl`, `sunLightSSAOF.glsl`, `class3/deferred/spotLightF.glsl` | soft shadows: sun and moon shadows sharp at contact and softer the further they fall (percentage-closer soft shadows, Randima Fernando), from the sun's real angular size times a Softness setting, through each cascade's own matrix; projector shadows get Black Dragon's 4x4 filter. Contact shadows: a short march toward the sun, moon or projector for what the shadow maps are too coarse to catch. The last cascade's fade no longer brightens what lies just inside the shadow distance |
| `class1/deferred/aoUtil.glsl`, `luminousGTAO{,Up}F.glsl`, `class3/deferred/softenLightF.glsl`, `pointLightF.glsl`, `multiPointLightF.glsl`, `spotLightF.glsl` | ambient occlusion by GTAO (Jimenez et al. 2016, after Intel's XeGTAO), at half size live; optionally on lamps and projectors too |
| `class3/deferred/screenSpaceReflUtil.glsl`, `reflectionProbeF.glsl` | enhanced reflections: screen-space reflections that keep to the surface they leave, blurred by roughness from the scene map's mip levels |
| `class1/deferred/tonemapUtilF.glsl`, `postDeferredTonemap.glsl`, `CASF.glsl` | the Look, inside the upstream `toneMap`: exposure, contrast, highlights, shadows, whites, blacks, three-way colour balance, HSL by colour range, saturation, vibrance, white balance (von Kries in LMS), black and white through a coloured filter, sepia, film grain after sharpening, a colour table, a dither on the eight-bit picture, and five more tonemappers -- AgX and AgX Punchy (Troy Sobotka's, as fitted by Benjamin Wrensch), Hable's filmic, Uchimura's Gran Turismo and Stachowiak's Tony McMapface. Under a legacy sky the Look grades but does not tonemap |
| `indra/newview/app_settings/luminous_luts/` | fourteen colour lookup tables (.cube) of Luminous's own, made by a script so no licence is borrowed; the user can load any .cube |
| `indra/newview/app_settings/luminous_tonemap/` | Tony McMapface's lookup table (Tomasz Stachowiak, MIT, licence beside it) |
| `.../luminousBloom{Down,Up}F.glsl` | bloom: half-size copies of the HDR frame, down and back up with Jorge Jimenez's filters, blended so that it spreads light rather than adding any |
| `.../luminousDiffusionF.glsl` | film halation, a red-orange glow outside the brightest edges, and bloom's wider glow |
| `.../luminousStreakF.glsl`, `luminousFlareBrightF.glsl`, `luminousFlareF.glsl`, `luminousFlareCompositeF.glsl` | anamorphic streaks in a colour of their own, and lens flare from the frame's own light: ghosts, halo, starburst and dirt, the sun's drawn |
| `.../luminousVignetteF.glsl`, `luminousDistortionF.glsl`, `luminousChromaticAberrationF.glsl`, `luminousClarityF.glsl` | vignette, barrel or pincushion distortion, chromatic aberration and clarity (local contrast), before tonemapping; `cofF.glsl` reads the depth where the distortion took each pixel from |
| `.../luminousDepthOfFieldF.glsl`, `luminousDoFTileF.glsl`, `class1/deferred/cofF.glsl` | depth of field as a lens blurs ("Enhance"): the upstream focus and lens settings, gathered at full resolution in HDR before tonemapping, so bright points open into discs (shaped by aperture blades) and the background never spreads onto what is in focus; after Dennis Gustafsson's "Bokeh depth of field in a single pass". A better depth of field is Black Dragon's idea |
| `indra/newview/lldrawpoolalpha.{h,cpp}`, `class2/deferred/alphaF.glsl`, `pbralphaF.glsl`, `class1/deferred/fullbrightF.glsl`, `class3/deferred/materialF.glsl` | worn alpha (hair, fur) in a render sorted pixel by pixel across the render's frames; worn rigged alpha writes its depth only where it covers |
| `class1/deferred/postDeferredNoDoFF.glsl`, `luminousphotomode.{h,cpp}`, `pipeline.cpp`, `panel_preferences_graphics1.xml`, `llfloaterpreference.{h,cpp}` | the 3D view renders at one pixel per point as Luminous ships (Preview resolution Standard: half each way on a Retina screen, full on a 1x screen, following the screen the window is on), or at full, three quarters or half the window's size, scaled up with Catmull-Rom while the interface stays sharp; renders are always at full size. Preview resolution is in the Luminous sidebar and in Preferences > Graphics > Hardware Settings |
| `indra/newview/lldrawpoolwater.cpp` | the water exclusion mask is not drawn when nothing excludes water (white is read in its place) |
| `indra/newview/luminousbenchmark.{h,cpp}` | a frame-time benchmark, run from a script |

## Drawing with Metal

On macOS 26 or later with Apple silicon, Luminous draws with Metal instead of
OpenGL. OpenGL stays: on macOS 15, and by choice in Preferences > Graphics from
the next start. After a Metal session that did not end normally, the next start
is on OpenGL.

| Where | Change |
|---|---|
| `indra/llrender/llrhi.{h,cpp}`, `llrhigl.{h,cpp}`, `llrhimetal*.{h,mm}`, `llrhimsl.{h,cpp}`, `llnativeprogram.{h,cpp}`, `llmslinclude.h` | a render device layer between the viewer and the graphics API, with an OpenGL and a Metal backend: textures, buffers, render targets, pipelines and frames; GLSL programs translated to Metal's shading language as they load |
| `indra/llrender/llrender.{h,cpp}`, `llimagegl.{h,cpp}`, `llglslshader.{h,cpp}`, `llvertexbuffer.{h,cpp}`, `llrendertarget.{h,cpp}`, `llshadermgr.{h,cpp}`, `llgl.{h,cpp}`, `llglstates.h`, `llgltexture.{h,cpp}`, `llcubemap*.{h,cpp}`, `llpostprocess.cpp`, `indra/llui/lllocalcliprect.cpp` | the upstream drawing code goes through that layer; on OpenGL it draws as before |
| `indra/llwindow/llwindowmacosx*.{h,cpp,mm}`, `llopenglview-objc.{h,mm}`, `llwindow.h`, and the new `llwindowmacosxcontext-objc.h`, `llopenglviewcontext-objc.h` | a Metal layer for the window; on Metal the graphics capabilities are read from a short-lived OpenGL context |
| `indra/llrender/llrhimetalsurface-objc.mm`, `llrhimetaldevice-objc.mm`, `llrhimetal*.h`, `indra/newview/llviewerwindow.cpp`, `app_settings/settings.xml` | the window's frames on Metal: whole frames at the display's refresh, with VSync on or off; with VSync off (the default) the newest finished frame is copied into the window on a queue of its own, so every frame reaches the screen without waiting behind the frames still being drawn, and the main loop never waits for the display |
| `indra/newview/pipeline.{h,cpp}`, `llviewershadermgr.{h,cpp}`, `lldrawpool*.cpp`, `llviewerdisplay.{h,cpp}`, `llviewerwindow.cpp`, `llreflectionmap*.{h,cpp}`, `llheroprobemanager.cpp`, `llspatialpartition.cpp`, `llvieweroctree.{h,cpp}`, `llface.cpp`, `llvovolume.cpp`, `llvoavatar*.{h,cpp}`, `llviewerjoint*.cpp`, `gltf/*`, `gltfscenemanager.cpp`, and other drawing code in `indra/newview` and `indra/llappearance` | the world, avatars, water, sky, reflection probes and the interface drawn through the layer |
| `app_settings/shaders/class1/deferred/SMAA.glsl`, `aoUtil.glsl`, `fullbrightF.glsl`, `class1/gltf/pbrmetallicroughnessV.glsl`, `class1/windlight/atmosphericsVarsV.glsl`, `class2/deferred/alphaF.glsl`, `pbralphaF.glsl`, `class3/deferred/fullbrightShinyF.glsl`, `materialF.glsl`, `screenSpaceReflUtil.glsl`, `softenLightF.glsl`, `class3/environment/waterF.glsl` | shaders written so that they translate to Metal as well; on Metal, worn alpha is sorted by a hash of position |
| `indra/newview/luminousmetal*.{h,cpp}`, `app_settings/shaders/metal/luminous/` | Luminous's photo effects drawn natively on Metal |
| `indra/llrender/llrhi.{h,cpp}`, `llrhimetal*.{h,mm}`, `indra/newview/luminousmetal.h`, `luminousmetaltone.cpp`, `luminousmetalair.cpp`, `luminousmetallens.cpp`, `luminousmetaldof.cpp`, `luminousphotomode.{h,cpp}`, `luminousmetalparams.h`, `luminousfloater.{h,cpp}`, `luminousoptimise.cpp`, `llfloaterpreference.{h,cpp}`, `llviewercontrol.cpp`, `llviewercamera.cpp`, `pipeline.{h,cpp}`, `skins/default/xui/en/panel_preferences_graphics1.xml`, `floater_luminous.xml`, `notifications.xml`, `app_settings/settings.xml`, `app_settings/shaders/metal/luminous/tone/`, `luminous/dof/`, `luminous/common/distortion.h`, `luminous/common/glitch.h`, `luminous/lens/finish.metal`, `class1/effects/glowExtractF.glsl`, `class1/interface/glowcombineF.glsl`, `class1/deferred/postDeferredF.glsl`, `dofCombineF.glsl`, `class3/deferred/screenSpaceReflUtil.glsl` | on Metal a live view rendered smaller can be scaled up with Apple's MetalFX temporal scaler, which builds detail from the frames before (the camera moved by a sub-pixel jitter, motion from depth and the camera, a mask for blended alpha nearby, a still and paused view held until the camera moves, the picture changes or a setting does; the upstream depth of field's blur, Enhance (photo mode's own depth of field), the object glow, the sun shafts and the lens flare's light made where the camera without the jitter saw the frame and averaged over the frames, so they do not flicker; the interface drawn over the world, the sun's shadow cascades and screen-space reflections kept off the jitter); "MetalFX upscaling" in Preferences > Graphics > Hardware Settings and on the sidebar's Settings tab under Preview resolution, off as Luminous ships; what is fixed to the screen is drawn on the scaled frame, so it keeps MetalFX on: live lens distortion, Glitch, Worn photo and the light leak by the present (with the film grain, and in that order), a picture shape's frame and Firestorm's capture frame in a pass of their own after it, and a live lens flare as above; the plain filter where it is off or while it settles. Where it would scale but cannot (refused at the window's size) the live view is drawn at full resolution, never the smaller view scaled the plain way, and a grey line under it says why. Switched on at full size (or at a quarter, beyond its range) it sets Preview resolution to Half, with a tip; at full size it rests, as a grey line under it says; while it scales, FXAA no longer keeps the live view at full size. Preferences' Cancel puts the Luminous tab, MetalFX upscaling and Preview resolution back. MetalFX is a system framework of macOS, linked weakly |
| `indra/newview/llappviewer.{h,cpp}`, `llstartup.cpp`, `llfeaturemanager.{h,cpp}`, `llfloaterpreference.{h,cpp}`, `skins/default/xui/en/panel_preferences_graphics1.xml`, `notifications.xml`, `strings.xml`, `app_settings/settings.xml`, `cmd_line.xml` | choosing the backend (Preferences > Graphics, "Graphics backend"; `--backend` for one start); the start on OpenGL after a Metal session that did not end normally; Help > About names the backend |
| `indra/third_party/glslang/`, `indra/third_party/spirv-cross/`, `indra/cmake/Glslang.cmake`, `SPIRVCross.cmake`, `Metal.cmake` | the Khronos Group's glslang and SPIRV-Cross, which translate the GLSL shaders for Metal, built from source; their licences are beside them |
| `indra/llrender/tests/`, `indra/newview/llgoldentest.{h,cpp}`, `llgoldenwait.{h,cpp}`, `luminous/scripts/msl-check*` | tests of the layer, and a picture test of the whole viewer that runs only when asked for |
| `indra/llrender/llrhimetal-objc.h`, `llrhimetalbinding-objc.mm`, `llrhimetaldevice-objc.mm`, `llrhimetalencoder-objc.mm` | mipmaps on M1 and M2 are drawn with the smoother filter again, as on newer chips: the mip program takes each level's size as a value instead of asking the view of the level above, which on those chips answers with the whole texture's size; `LL_METAL_MIPS` still chooses another way for a test |

## Renders

| Where | Change |
|---|---|
| `indra/newview/luminoushdrsnapshot.{h,cpp}`, `pipeline.cpp`, `llviewerwindow.cpp`, `.../luminousAverageF.glsl` | renders at the window's size, twice it or a custom size, in PNG, JPEG, BMP, 16-bit TIFF or OpenEXR (half float, scene-linear); Luminous's own TIFF and EXR writers, no library. 16 frames by default, each shifted by a fraction of a pixel (Halton 2,3), looking through another point of the lens and with the sun moved across its disc, averaged as light before tonemapping. A margin round the picture for the effects that look past its edges. Full detail loads textures and meshes at full detail for what the picture shows. The size and shadow maps are chosen to fit the memory there is, with a note on the picture when they are smaller |
| `indra/newview/llviewercamera.cpp` | the frames' sub-pixel and lens offsets |
| `indra/newview/luminousfloater.{h,cpp}`, `skins/default/xui/en/floater_luminous_picture.xml`, `floater_luminous_render.xml`, `llsnapshotlivepreview.{h,cpp}`, `llwebprofile.h`, `llfloaterflickr.cpp` | the render over the viewer: a still with progress and Stop, then the picture with Share (Download, Open in Preview, and sending on to inventory, the profile feed, Flickr or email) and Close |
| `indra/llwindow/llwindow.h`, `llwindowmacosx*.{h,cpp,mm}` | Stop and Esc reach a render that holds up the main loop; file panels answer at once while they are open (`llfilepicker*`); the viewer's focus is set to macOS's app activity twice a second, so a change missed while a render held the main loop does not leave it at the background's frame rate |
| `indra/llrender/llimagegl.{h,cpp}`, `pipeline.cpp`, `llviewerwindow.cpp` | what a render gives back is deleted between its frames, not after the render, and the HDR frame is kept between frames |
| `indra/newview/llviewertexture.cpp`, `llviewertexturelist.cpp` | Full detail's textures, for what the picture shows |
| `indra/llcharacter/llheadrotmotion.{h,cpp}` | the eyes are held still while a render takes |
| `indra/newview/llvoavatar.cpp`, `llviewerpartsim.cpp`, `lldrawpoolwlsky.cpp`, `llenvironment.cpp` | Pause scene: Linden Lab's freeze frame, plus avatars, animations, particles, the day, clouds, waves and stars held; the camera still moves |
| `indra/newview/luminousphotomode.cpp` | saved camera positions with their depth of field, per account; animation speed down to 0 |
| `indra/newview/llfilepicker.{h,cpp}`, `llfilepicker_mac.mm`, `luminousfloater.cpp`, `luminoushdrsnapshot.cpp` | a render's Download, and its save panel, ask in the render's own file type (PNG, JPEG, BMP, TIFF, EXR), so macOS no longer asks for a name ending in "." |
| `indra/newview/luminoushdrsnapshot.{h,cpp}`, `luminousfloater.cpp`, `skins/default/xui/en/floater_luminous.xml` | a custom render size beside Window size and Double: a width and height in pixels, up to the GPU's largest texture (16384) a side, the other side following the chosen shape or both free with the Free shape, fitted to memory as Double is, with what it needs shown before the render; a picture over 256 MB is cut, shown and sent on without a full-size copy, one too large to show can still be downloaded, and a size that can never fit on the Mac does not keep textures out of view lowered |
| `indra/newview/luminousfloater.cpp`, `luminoushdrsnapshot.{h,cpp}`, `floater_luminous_picture.xml`, `settings_per_account.xml`, `llstartup.cpp` | a render's Download asks for a folder and a name once, then saves at once as <name>_001, _002... in that folder; the picture says for a few seconds, over Share and Close, what was saved (or could not be), never over a file there; Save as... asks again; renders to disk use the same name and numbering once one is chosen; the name is kept per account (`LuminousRenderName`) |

## Styles, environments and presets

| Where | Change |
|---|---|
| `indra/newview/app_settings/luminous_styles/` | twenty styles: light, lens and look in one, optionally framing the avatar ("Frame me": face, half, low, tilted, behind, side low) |
| `indra/newview/app_settings/luminous_environments/` | ten skies made for photographing people, some mixed by the sun's height |
| `indra/newview/luminousphotomode.cpp`, `llenvironment.cpp` | the Scene tab's sky and sun as Personal Lighting sets them, the user's own PBR skies listed too; moving the sun keeps the reflection probes |
| `indra/newview/luminousphotomode.cpp`, `luminousfloater.cpp` | presets: every setting of the Scene, Tone, Colour and Effects tabs, and the sky and sun, saved by name in `user_settings/luminous_presets` |
| `indra/newview/llpresetsmanager.cpp`, `app_settings/graphic_preset_controls.xml` | graphics presets save what Preferences > Graphics shows, the Luminous tab's Off / On / Only in renders and Preview resolution and MetalFX upscaling included, and no longer the photo effects the sidebar sets (depth of field's f-number and lens, glow, exposure, tone mapping, sharpening, vignette, attached lights, the legacy-sky switch); loading an older preset leaves those as they are |

## Inventory and the LSL bridge

| Where | Change |
|---|---|
| `indra/newview/fs_resources/lumen_bridge.lsltxt` | a bridge script written fresh, implementing the same command set as the upstream bridge |
| `indra/newview/fslslbridge.{h,cpp}`, `lumenfolders.{h,cpp}`, `aoengine.cpp`, `llinventoryfunctions.h`, `llinventorybridge.cpp` | Luminous's own `#Luminous` system folder and "#Luminous LSL Bridge"; the AO and other features use an existing `#Firestorm` folder, untouched, where there is one; other viewers' bridges are taken off when worn |
| `indra/newview/llstartup.cpp`, `luminousphotomode.cpp` | the styles' skies and waters put in `#Luminous/#Environments` as environment items, once per account |
| `indra/newview/llpanelmaininventory.cpp` | the inventory's Recent and Worn tabs are built when first shown, not at login |
| `indra/newview/llinventorylistener.cpp` | an item's creator in the inventory event API |

## Network

What Luminous adds that talks to the network, besides Second Life itself:

| Where | Change |
|---|---|
| `indra/newview/luminousbugreport.{h,cpp}`, `luminousbugreportkey.h.in`, `skins/default/xui/en/floater_luminous_bug_report.xml` | Help > Report a Bug sends one report to needfulsl.com when Send is clicked: the text written, optionally the log and a picture, and the viewer's version, macOS and the avatar's name and region |
| `indra/newview/luminousupdatecheck.{h,cpp}` | once a session, at the login screen, one request for needfulsl.com's `version.json`; a newer version gives a notice once a day; a pre-release viewer is also told of a newer pre-release (the file's `prerelease`), a viewer on the release never |
| `indra/newview/llfloaterflickr.cpp`, `indra/newview/CMakeLists.txt` | Flickr with Luminous's own app key, read from a private file at configure time; only when the user signs in and sends |
| `indra/newview/fsfloaterprimfeed.cpp`, `fsprimfeedconnect.cpp`, `xui/en/*` | Primfeed hidden for now |
| `indra/newview/fsgridhandler.{h,cpp}` (upstream's), `llloginhandler.cpp`, `llstartup.cpp`, `app_settings/settings.xml` | a grid's `get_grid_info` is fetched from that grid only when the user names one Luminous does not know (Preferences > OpenSim, the login screen's grid row, `--grid`); no grid list is downloaded (`GridListDownload` off). A link (`secondlife:///app/login`, `hop://`, a start location) selects only a grid already in the list, and adding a grid by link (`gridmanager/addgrid`) takes a click in the viewer. Second Life's grids are told by their exact host (`lindenlab.com` or a name under it), not by a name that contains it |

## OpenSim

One Luminous serves Second Life and OpenSim grids: upstream's OpenSim support
is built in, and a Second Life session is as it was without it.

| Where | Change |
|---|---|
| `indra/CMakeLists.txt`, `autobuild.xml` | upstream's OpenSim support (`OPENSIM`) on by default and in macOS's ReleaseOS configuration |
| `indra/cmake/ViewerMiscLibs.cmake`, `Copy3rdPartyLibs.cmake`, `indra/newview/CMakeLists.txt`, `viewer_manifest.py`, `fs_viewer_manifest.py` | Vivox's voice client (SLVoice) is not included: a separate option, `LUMINOUS_VIVOX`, off; the app keeps its name, without upstream's "OS" tag |
| `indra/llfilesystem/lldir.cpp`, `lldir_mac.cpp` | one cache folder for both, as before (`Luminous_x64`) |
| `indra/newview/app_settings/grids.xml` | Second Life's grids only, with Linden Lab's own grid ids (and `grid_login_id`, from which Second Life's voice names Linden's STUN servers), SLURL bases and web profiles, as the viewer had them without OpenSim support; OpenSim grids are the user's own (`grids.user.xml`) |
| `indra/newview/fspanellogin.{h,cpp}`, `skins/default/xui/en/panel_fs_nui_login.xml`, `panel_fs_login.xml`, `app_settings/settings.xml` | the login screen: no grid row for a Second Life user (`FSOpenSimAlwaysForceShowGrid` off); "Other grids" shows it; it shows by itself when the chosen grid is not Second Life's; its "Add a grid" opens Preferences > OpenSim, the grid manager, not a web page |
| `indra/newview/llvoiceclient.cpp`, `skins/default/xui/en/notifications.xml` | voice: Vivox never runs in Second Life; without its client, a grid whose voice is Vivox has none, said once a session (`LuminousNoVivoxVoice`), and the voice of the region before stops there |
| `indra/newview/llvoavatar.cpp` | in Second Life avatars always count all eleven bakes |
| `indra/newview/llviewermessage.cpp`, `luminousphotomode.{h,cpp}` | OpenSim's "Setting updated" for each environment Luminous puts in `#Luminous/#Environments` is not shown in chat |
| `indra/newview/skins/default/xui/en/floater_preferences.xml`, `panel_preferences_opensim.xml` | the tab is named OpenSim |

## Build

| Where | Change |
|---|---|
| `indra/cmake/Variables.cmake` | macOS deploy target raised to 12.0 for Xcode 27; arm64 by default (override with `-DLUMEN_OSX_ARCH`) |
| `indra/cmake/00-Common.cmake` | the build machine's paths mapped to the checkout and the linker's debug map stripped, so no home folder is named in the binaries |
| `indra/newview/CMakeLists.txt` | Luminous's sources; the bug report and Flickr keys read at configure time from a private file, never kept in this repository |
| `autobuild.xml` | the licence and copyright text |
| `luminous/scripts/` | build and signing scripts |
| `.github/` | the upstream CI workflows are not carried |

## Fixes to upstream code

| Where | Change |
|---|---|
| `indra/newview/llviewerwindow.cpp` | a saved snapshot keeps its post-processing: Linden Lab's L$-balance change passed the balance flag where "no post-processing" belongs |
| `indra/newview/pipeline.cpp` | the upstream vignette and snapshot frame no longer read and write the same buffer, which they did with depth of field on and anti-aliasing off |
| `indra/newview/llvoicewebrtc.{h,cpp}`, `llappviewer.cpp` | a quit no longer hangs when voice's audio devices are locked; a quit that hangs anyway ends after 45 seconds, and a Metal session that ended its main loop normally first is marked ended, so the next start stays on Metal instead of falling back to OpenGL |
| `indra/newview/llappviewer.cpp` | no crash at exit after a disconnect and a camera reset |
| `indra/newview/llenvironment.cpp` | no crash showing the region's sky before the region has sent one |
| `indra/newview/skins/default/html/en-us/lumen/login.html` | the start page repaints after the window is resized |
| `indra/llrender/llrendertarget.cpp` | a render target's mip-map setting starts initialised, not undefined |
| `indra/newview/pipeline.cpp` | a projector shadow slot no projector holds has a 1 x 1 map read in its place, so its full-size map takes no memory |
| `app_settings/shaders/class2/deferred/sunLightF.glsl`, `sunLightSSAOF.glsl`, `indra/newview/pipeline.cpp` | the sun's shadow pass no longer reads a projector shadow map whose slot no projector holds, at every pixel; the picture is the same |
| `app_settings/shaders/class3/deferred/reflectionProbeF.glsl`, `indra/newview/llviewershadermgr.cpp`, `llviewercontrol.cpp` | a legacy surface that reads no reflection probe (a classic sky, no glossiness, no environment intensity) skips the search for the probes round it; the picture is the same |
| `indra/llimage/llimagepng.cpp` | room in PNG's encoding buffer for a picture that hardly compresses |
| `indra/newview/luminousenvironmentlistener.cpp` | an event API for the personal environment's sun, sky and water, of the same kind as Linden Lab's own, for test scripts started with `--leap`; values out of Linden Lab's ranges are refused |
| `indra/newview/llfilepicker.cpp`, `llfilepicker_mac.mm` | on macOS a save panel for any type no longer asks for a name ending in "."; a JPEG save panel keeps a .jpg name instead of making it .jpg.jpeg |
| `indra/newview/llfilepicker_mac.mm` | a macOS save panel given a full path opens in its folder with the file's name, and a proposed name with letters beyond ASCII keeps them |
| `indra/newview/llvoavatar.{h,cpp}`, `app_settings/settings.xml` | avatars near Max. # animated avatars no longer flicker between impostor and full: one drawn in full keeps its place until another covers 1.5 times its size on screen (`LuminousImpostorHysteresis`), a change waits until the ranking has asked for it for a second (`LuminousImpostorHoldSeconds`), and an impostor that goes out of view comes back as one (while its impostor is complete); `LuminousImpostorLog` logs each avatar's changes and render time |

## Other fixes

| Where | Change |
|---|---|
| `indra/newview/llviewermedia_streamingaudio.{h,cpp}` | streaming music reports why it failed and survives a wrong URL scheme |
| `indra/media_plugins/libvlc/media_plugin_libvlc.cpp`, `indra/newview/llviewermedia_streamingaudio.{h,cpp}` | a music stream's "now playing" reaches the toasts, nearby chat and the Stream Title window as the FMOD viewers show it: an internet radio's ICY title, or the artist and title an Ogg stream carries; a stream without one stays quiet; the title is kept through a reconnect and cleared when the stream stops, changes or gives up (the upstream VLC plugin reported a title only on VLC's title-change event, which a radio never sends) |
| `indra/llwebrtc/*` | the microphone is prepared only when the app starts recording, not when webrtc.lib connects a stream, so macOS does not ask for it before anybody speaks |
| `indra/llrender/llrhimetaldevice-objc.mm`, `llrhimetalencoder-objc.mm`, `llrhimetalsurface-objc.mm`, `llrhimetal-objc.h`, `indra/newview/llappviewer.cpp`, `app_settings/settings.xml` | with MetalFX upscaling, an open or save panel (bulk upload, model upload) no longer stops the graphics processor: after the frame had stood while the panel showed, the scaler's command buffer ran until the watchdog ended it and the device was lost; MetalFX's work now begins a command buffer of its own. A GPU error's line says what the command buffer waited for and when the GPU began it, the VSync-off present reports its own errors, `LL_METAL_SPLIT_PASSES=1` sends each encoder in its own command buffer to name a pass that hangs, and the developer settings LuminousTestPauseDisplay and LuminousTestStallMainThread stand the frame or the main thread for a test |
| `indra/llrender/llrhimetalsurface-objc.mm`, `llrhimetal-objc.h`, `llrhimetal.h`, `llrhimetaldevice-objc.mm` | with VSync off, the picture no longer freezes for about 4 s after an open or save panel (Download, upload, bulk upload), and with MetalFX upscaling on the viewer no longer closes afterwards ("The graphics processor stopped responding"): after a pause of over 250 ms in presenting, the first frames go to the screen once the graphics processor has finished them; MetalFX's command buffer of its own (the row above) stays |
| `indra/newview/luminousmetaltone.cpp`, `app_settings/shaders/metal/luminous/tone/temporal.metal`, `toneshared.h`, `programs.json` | with MetalFX upscaling, a paused, still scene no longer runs slower than a moving one: the check that keeps the held picture steady compares the frames in two passes and takes well under a millisecond of the graphics processor instead of several |
