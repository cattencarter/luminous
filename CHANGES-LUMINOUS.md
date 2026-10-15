# What Luminous changes in the source it builds on

Luminous is based on the source code of the **Phoenix Firestorm Viewer**,
released under the GNU Lesser General Public License version 2.1 — the same
licence as the work it is based on. The LGPL requires a
derivative to state what it changed in that work. This is that statement; it
names upstream files and features where Luminous changes them, which
implies no involvement of the upstream projects in Luminous.

Upstream baseline: the master branch, last merged at `ccfb6e02c7` (the
7.2.5 pre-release, on Linden Lab's 26.3). Edits to upstream files are marked
by comments in the code. Some file and setting names keep an earlier working
name, `lumen`.

To see the changes exactly, rather than taking this file's word for it,
compare with the upstream commit Luminous is based on:

```sh
git fetch https://github.com/FirestormViewer/phoenix-firestorm.git master
git diff ccfb6e02c759176beb29271a47344a9ac429cbf6 HEAD
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
| `class1/deferred/postDeferredNoDoFF.glsl` | the live view can render at three quarters or half the window's size, scaled up with Catmull-Rom; renders are always at full size |
| `indra/newview/lldrawpoolwater.cpp` | the water exclusion mask is not drawn when nothing excludes water (white is read in its place) |
| `indra/newview/luminousbenchmark.{h,cpp}` | a frame-time benchmark, run from a script |

## Renders

| Where | Change |
|---|---|
| `indra/newview/luminoushdrsnapshot.{h,cpp}`, `pipeline.cpp`, `llviewerwindow.cpp`, `.../luminousAverageF.glsl` | renders at the window's size or twice it, in PNG, JPEG, BMP, 16-bit TIFF or OpenEXR (half float, scene-linear); Luminous's own TIFF and EXR writers, no library. 16 frames by default, each shifted by a fraction of a pixel (Halton 2,3), looking through another point of the lens and with the sun moved across its disc, averaged as light before tonemapping. A margin round the picture for the effects that look past its edges. Full detail loads textures and meshes at full detail for what the picture shows. The size and shadow maps are chosen to fit the memory there is, with a note on the picture when they are smaller |
| `indra/newview/llviewercamera.cpp` | the frames' sub-pixel and lens offsets |
| `indra/newview/luminousfloater.{h,cpp}`, `skins/default/xui/en/floater_luminous_picture.xml`, `floater_luminous_render.xml`, `llsnapshotlivepreview.{h,cpp}`, `llwebprofile.h`, `llfloaterflickr.cpp` | the render over the viewer: a still with progress and Stop, then the picture with Share (Download, Open in Preview, and sending on to inventory, the profile feed, Flickr or email) and Close |
| `indra/llwindow/llwindow.h`, `llwindowmacosx*.{h,cpp,mm}` | Stop and Esc reach a render that holds up the main loop; file panels answer at once while they are open (`llfilepicker*`) |
| `indra/llrender/llimagegl.{h,cpp}`, `pipeline.cpp`, `llviewerwindow.cpp` | what a render gives back is deleted between its frames, not after the render, and the HDR frame is kept between frames |
| `indra/newview/llviewertexture.cpp`, `llviewertexturelist.cpp` | Full detail's textures, for what the picture shows |
| `indra/llcharacter/llheadrotmotion.{h,cpp}` | the eyes are held still while a render takes |
| `indra/newview/llvoavatar.cpp`, `llviewerpartsim.cpp`, `lldrawpoolwlsky.cpp`, `llenvironment.cpp` | Pause scene: Linden Lab's freeze frame, plus avatars, animations, particles, the day, clouds, waves and stars held; the camera still moves |
| `indra/newview/luminousphotomode.cpp` | saved camera positions with their depth of field, per account; animation speed down to 0 |

## Styles, environments and presets

| Where | Change |
|---|---|
| `indra/newview/app_settings/luminous_styles/` | twenty styles: light, lens and look in one, optionally framing the avatar ("Frame me": face, half, low, tilted, behind, side low) |
| `indra/newview/app_settings/luminous_environments/` | ten skies made for photographing people, some mixed by the sun's height |
| `indra/newview/luminousphotomode.cpp`, `llenvironment.cpp` | the Scene tab's sky and sun as Personal Lighting sets them, the user's own PBR skies listed too; moving the sun keeps the reflection probes |
| `indra/newview/luminousphotomode.cpp`, `luminousfloater.cpp` | presets: every setting of the Scene, Tone, Colour and Effects tabs, and the sky and sun, saved by name in `user_settings/luminous_presets` |

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
| `indra/newview/luminousupdatecheck.{h,cpp}` | once a session, at the login screen, one request for needfulsl.com's `version.json`; a newer version gives a notice once a day |
| `indra/newview/llfloaterflickr.cpp`, `indra/newview/CMakeLists.txt` | Flickr with Luminous's own app key, read from a private file at configure time; only when the user signs in and sends |
| `indra/newview/fsfloaterprimfeed.cpp`, `fsprimfeedconnect.cpp`, `xui/en/*` | Primfeed hidden for now |

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
| `indra/newview/llvoicewebrtc.{h,cpp}`, `llappviewer.cpp` | a quit no longer hangs when voice's audio devices are locked; a quit that hangs anyway ends after 45 seconds |
| `indra/newview/llappviewer.cpp` | no crash at exit after a disconnect and a camera reset |
| `indra/newview/llenvironment.cpp` | no crash showing the region's sky before the region has sent one |
| `indra/newview/skins/default/html/en-us/lumen/login.html` | the start page repaints after the window is resized |
| `indra/llrender/llrendertarget.cpp` | a render target's mip-map setting starts initialised, not undefined |
| `indra/newview/pipeline.cpp` | a projector shadow slot no projector holds has a 1 x 1 map read in its place, so its full-size map takes no memory |
| `indra/llimage/llimagepng.cpp` | room in PNG's encoding buffer for a picture that hardly compresses |
| `indra/newview/luminousenvironmentlistener.cpp` | an event API for the personal environment's sun, sky and water, of the same kind as Linden Lab's own, for test scripts started with `--leap`; values out of Linden Lab's ranges are refused |

## Other fixes

| Where | Change |
|---|---|
| `indra/newview/llviewermedia_streamingaudio.{h,cpp}` | streaming music reports why it failed and survives a wrong URL scheme |
| `indra/llwebrtc/*` | the microphone is prepared only when the app starts recording, not when webrtc.lib connects a stream, so macOS does not ask for it before anybody speaks |
