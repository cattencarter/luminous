/**
 * @file luminoushdrsnapshot.h
 * @brief Snapshots with more than eight bits: the light as OpenEXR, the
 * finished picture as 16-bit TIFF.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
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
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */
#ifndef LUMINOUS_HDR_SNAPSHOT_H
#define LUMINOUS_HDR_SNAPSHOT_H

#include "stdtypes.h"

#include <string>

class LLRenderTarget;

/**
 * Every Second Life viewer saves snapshots as eight-bit colour read back after
 * tonemapping. These read the frame earlier, from LLPipeline::renderFinalize:
 *
 * - **OpenEXR**, half float, scene-linear Rec. 709: the light itself, before
 *   tonemapping, with Firestorm's exposure and the Look's exposure applied but
 *   none of its grading, so an editor gets every highlight at full range.
 * - **16-bit TIFF**: the finished picture -- tonemapped, with the Look -- at
 *   sixteen bits, so it can be edited hard without banding.
 *
 * Both are rendered through LLViewerWindow::rawSnapshot like any snapshot, at
 * twice the size when "smooth edges" is on and averaged down, since the
 * anti-aliasing passes run on the eight-bit picture after the point they are
 * read. Neither carries Firestorm's glow or sharpening, which also come after.
 * The writers are Luminous's own, uncompressed, with no library.
 *
 * - **PNG**, eight bits: the ordinary snapshot -- glow, sharpening and
 *   anti-aliasing included -- at any size the GPU can render, past the
 *   snapshot window's 7680. **JPEG** and **BMP** are the same picture,
 *   quicker to write at large sizes than PNG's compression: JPEG at
 *   quality 95, BMP uncompressed.
 */
class LLImageRaw;

namespace LuminousHDRSnapshot
{
    enum EFormat { EXR, TIFF16, PNG8, JPEG8, BMP8 };
    /// The finished eight-bit picture, read after anti-aliasing: PNG, JPEG, BMP.
    inline bool eightBit(EFormat format) { return format == PNG8 || format == JPEG8 || format == BMP8; }

    /// Called from LLPipeline::renderFinalize just before tonemapping. Does
    /// nothing unless a snapshot is being taken.
    void capture(LLRenderTarget* hdr_screen, LLRenderTarget* exposure_map);

    /// Takes the snapshot at the Render tab's size (chosenSize) and writes it
    /// to path. Returns false, with the reason in error, if it could not.
    bool takeAndSave(EFormat format, const std::string& path, std::string& error);
    /// The same render as a PNG, kept as an image instead of written to a
    /// file: for Second Life's own places (inventory, profile feed, ...).
    bool takePicture(LLPointer<LLImageRaw>& picture, std::string& error);
    /// The Render image button: renders and saves in the chosen format, and
    /// shows the picture in the render window, ready for the chosen place.
    void renderImage();
    /// The Preview button: four frames at the window's size, without the
    /// margin, Full detail's wait or the lens and the sun sampled across the
    /// frames -- every effect at full quality, in about a second -- shown
    /// over the viewer and not kept. Every animation stays held while it
    /// shows, so its Render button renders the same moment.
    void renderPreview();
    /// While a preview renders: takeAndSave and photo mode's sampling ask.
    bool previewing();
    /// The preview's overlay closed without Render: the animations go on.
    void endPreview();
    /// The name Download and renders to disk save pictures under once one
    /// has been chosen in a save panel (LuminousRenderName, per account):
    /// <name>_001, _002... in the folder (SnapshotBaseDir). Empty until
    /// then: renders to disk are named Luminous_ and the date and time, and
    /// Download asks.
    std::string chosenName();
    /// The next file for name in dir: <name>_<number>.<extension>, the
    /// number one above the highest of that name and extension there, 001
    /// in a folder without one. Never a file that is there.
    std::string nextNumbered(const std::string& dir, const std::string& name, const std::string& extension);
    /// A file picked in a save panel, taken as the folder and the name for
    /// the pictures after it: SnapshotBaseDir and LuminousRenderName set,
    /// the name without its extension or a number of its own at the end
    /// (_004). Returns the file to save to, the next numbered one there in
    /// the given extension.
    std::string chooseNameAndFolder(const std::string& picked, const std::string& extension);
    /// The shape a render from Render image will have: width over height.
    F32 pictureAspect();
    /// The shape chosen on the Render tab (LuminousSnapshotAspect, turned
    /// upright by LuminousSnapshotPortrait, or Free's own width and height),
    /// width over height; 0 for the window's own.
    F32 chosenAspect();
    /// The shape a Custom size follows: the chosen one, or the window's.
    F32 customAspect();
    /// Free's width and height as a render takes them
    /// (LuminousSnapshotWidth, LuminousSnapshotHeight): each side from
    /// SMALLEST_SIDE to largestSide(), and neither more than 20 times the
    /// other (the shorter raised).
    void freeSize(U32& width, U32& height);
    /// The picture's size at a scale of the window: the window's, or the
    /// largest centred part of it in the chosen shape, as rawSnapshot draws
    /// it (a narrower shape keeps the height, a wider one the width). At
    /// most the GPU's largest texture.
    void pictureSize(F32 scale, U32& width, U32& height);
    /// The size chosen on the Render tab, in pixels (LuminousSnapshotScale):
    /// the window's or twice it in the chosen shape (pictureSize), or with
    /// Custom (0) Free's width and height, or a long side of the user's own
    /// (LuminousSnapshotLongSide) in the chosen shape, the window's when none.
    void chosenSize(U32& width, U32& height);
    /// That size as a scale of the window's picture in the same shape: 1, 2,
    /// or what the custom size comes to.
    F32 chosenScale();
    /// The longest side a render can have: the GPU's largest texture, which
    /// its render targets share (16384 on Apple silicon, Metal and OpenGL).
    U32 largestSide();
    /// The shortest long side Custom takes.
    constexpr U32 SMALLEST_LONG_SIDE = 256;
    /// The smallest side of a Free size.
    constexpr U32 SMALLEST_SIDE = 64;
    /// The Render tab's Shape menu: Free, a width and height of the user's own.
    constexpr S32 SHAPE_FREE = 7;
    /// Whether a render at the Render tab's size fits beside the scene now,
    /// as takeAndSave reckons the least it takes at that size; need is that
    /// least, room what the viewer may still take, both in bytes. Where
    /// macOS's figures are unknown, room is 0 and the render counts as
    /// fitting.
    bool chosenSizeFits(U64& need, U64& room);
    /// During one of Luminous's renders in a shape other than the window's:
    /// rawSnapshot then draws into targets of the picture's size, as for a
    /// picture larger than the window.
    bool shapedRender();
    /// Metal, during one of Luminous's renders: true for a frame whose eight
    /// bits rawSnapshot need not read -- every frame of a TIFF or EXR, which
    /// capture() reads, and every frame but the last when the frames are
    /// averaged as light. rawSnapshot then waits for the frame instead of
    /// reading it. Always false on OpenGL.
    bool frameReadSkipped();

    /// Registers the commit callbacks Luminous.SaveHDRSnapshot (EXR),
    /// Luminous.Save16BitSnapshot (TIFF) and Luminous.SaveLargeSnapshot
    /// (PNG). With a path as parameter they save there; without, next to
    /// Firestorm's own snapshots, or where a save dialog says.
    void init();

    /// While a snapshot waits for full textures (LuminousSnapshotFullTextures):
    /// textures that are not objects' (LLViewerFetchedTexture's own: avatars'
    /// and the like) ask here as they ask TextureLoadFullRes, which Luminous
    /// leaves alone -- Firestorm warns about it being on.
    bool wantsFullTextures();

    /// While a snapshot waits for full detail: how much larger an on-screen
    /// texture's screen area is taken to be (LLViewerTextureList), so that
    /// objects' textures load as finely as the picture needs; 1 otherwise.
    F32 fullDetailAreaScale();

    /// How much of a texture's screen area LLViewerTextureList counts. With
    /// HiDPI and LuminousTextureDetailByPoints, the area in the window's
    /// points, not its pixels: textures load at the level a display without
    /// HiDPI shows, a quarter of the memory on a Retina display. While a
    /// snapshot waits for full detail, on-screen textures as finely as the
    /// picture needs (fullDetailAreaScale).
    F32 textureAreaScale(bool on_screen);

    /// Whether textures out of view let go of the largest size they were
    /// ever wanted at: while the viewer holds more than
    /// LuminousTextureForgetShare of the Mac, measured twice a second.
    /// Second Life lets them go only once the discard bias passes 1.
    bool forgetOutOfView();

    /// Whether the viewer lacks room for a render at the Render tab's size:
    /// its footprint and the render's need past what a render may reach
    /// (what macOS could still give, less a margin), measured twice a
    /// second. LLViewerTexture::updateClass then lets textures out of view
    /// give way, the discard bias up to ROOM_BIAS and no further. Never
    /// short for a size that cannot fit beside the viewer even with the
    /// whole share of the Mac a render may reach: giving way would not make
    /// it fit, and the render then takes a size that does.
    bool roomShort();
    /// Whether the scene itself leaves no room for a render at the window's
    /// size (LuminousMemoryStepDown): macOS compresses and swaps rather than
    /// saying memory is low, so LLViewerTexture::updateClass counts this as
    /// low system memory -- the discard bias rising past the 1.9 at which
    /// textures on screen give way too. Room again with a gigabyte to spare;
    /// measured twice a second, not while a render waits or renders. After
    /// 20 s short, objects are drawn with half the user's detail
    /// (RenderVolumeLODFactor, not saved) until there has been room for a
    /// minute with 3 GB to spare.
    bool sceneShort();
    /// The discard bias room is kept with: below the 1.9 past which
    /// LLViewerTextureList biases textures on screen too, so only those out
    /// of view lose detail.
    constexpr F32 ROOM_BIAS = 1.85f;
}

#endif // LUMINOUS_HDR_SNAPSHOT_H
