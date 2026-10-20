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

    /// Takes the snapshot at the window's size, or twice it (LuminousSnapshotScale), and
    /// writes it to path. Returns false, with the reason in error, if it could
    /// not.
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
    /// The shape a render from Render image will have: width over height.
    F32 pictureAspect();
    /// The shape chosen on the Render tab (LuminousSnapshotAspect, turned
    /// upright by LuminousSnapshotPortrait), width over height; 0 for the
    /// window's own.
    F32 chosenAspect();
    /// The picture's size at a scale of the window: the window's, or the
    /// largest centred part of it in the chosen shape, as rawSnapshot draws
    /// it (a narrower shape keeps the height, a wider one the width). At
    /// most the GPU's largest texture.
    void pictureSize(F32 scale, U32& width, U32& height);
    /// During one of Luminous's renders in a shape other than the window's:
    /// rawSnapshot then draws into targets of the picture's size, as for a
    /// picture larger than the window.
    bool shapedRender();

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
}

#endif // LUMINOUS_HDR_SNAPSHOT_H
