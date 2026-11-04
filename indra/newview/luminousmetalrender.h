/**
 * @file luminousmetalrender.h
 * @brief Photo mode on Metal: what renders read back, for LuminousHDRSnapshot.
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

#ifndef LUMINOUS_METAL_RENDER_H
#define LUMINOUS_METAL_RENDER_H

#include "stdtypes.h"

#include <string>
#include <vector>

// The render group's native passes (luminousmetalrender.cpp) and what LuminousHDRSnapshot asks of them
// on Metal. The passes are LuminousMetal::Pass::AverageFrames (the frames averaged as light, PassTargets:
// frame the HDR frame, other the float sum) and Pass::HDRCapture (one frame read back into the picture,
// PassTargets: frame the frame read; the exposure is the frame's PassContext::exposure). Nothing here
// does anything on OpenGL.
namespace LuminousMetalRender
{
    /// How the next HDRCapture reads its frame into the picture.
    struct Read
    {
        U32 cutX = 0;          ///< the part of the frame the picture is, in frame pixels, rows bottom first
        U32 cutY = 0;
        U32 cutWidth = 0;
        U32 cutHeight = 0;
        F32 renderWidth = 0.f; ///< the shape that part is cropped to, centred (reduce()'s render size)
        F32 renderHeight = 0.f;
        U32 width = 0;         ///< the picture
        U32 height = 0;
        bool light = false;    ///< OpenEXR: the light as half float, times scale and the metered exposure
        F32 scale = 1.f;       ///< for the light: RenderExposure and the Look's exposure
    };

    /// A render begins: what an earlier one left is let go.
    void beginRender();

    /// The next HDRCapture's read.
    void setRead(const Read& read);

    /// After the frames: waits for the reads, and gives the picture (RGB floats, top row first, the average of
    /// the frames read) and the metered exposure of the last frame read. False with a reason when a read
    /// failed or none was made.
    bool takePicture(std::vector<F32>& rgb, U32 width, U32 height, F32& metered, std::string& error);

    /// The render is done (or stopped): its textures and reads are let go.
    void endRender();

    /// Frames read so far in this render.
    S32 framesRead();

    /// The memory the reads of one render take above the frame's own targets, in bytes: the picture-sized
    /// texture the frame is read into, the reads in flight and the sum on the CPU. 0 for an eight-bit picture,
    /// which rawSnapshot reads.
    U64 readBytes(U32 width, U32 height, bool eight_bit, bool light);
}

#endif // LUMINOUS_METAL_RENDER_H
