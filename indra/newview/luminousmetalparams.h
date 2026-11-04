/**
 * @file luminousmetalparams.h
 * @brief Photo mode's parameters per group, as the translated passes compute them, for native passes.
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

#ifndef LUMINOUS_METAL_PARAMS_H
#define LUMINOUS_METAL_PARAMS_H

#include "stdtypes.h"

class LLViewerTexture;

#include "app_settings/shaders/metal/luminous/air/airshared.h"
#include "app_settings/shaders/metal/luminous/dof/dofshared.h"
#include "app_settings/shaders/metal/luminous/lens/lensshared.h"
#include "app_settings/shaders/metal/luminous/render/rendershared.h"
#include "app_settings/shaders/metal/luminous/screen/screenshared.h"
#include "app_settings/shaders/metal/luminous/shadow/shadowshared.h"
#include "app_settings/shaders/metal/luminous/tone/toneshared.h"

// Each fill function runs the computation the translated passes run, in luminousphotomode.cpp,
// so a native pass draws with the same numbers. Where a translated pass binds from the same
// struct (the Look, the lens, depth of field, GTAO, contact shadows, reflections, soft shadows),
// there is one computation for both.
namespace LuminousPhotoMode
{
    /// The Look's tonemap uniforms, grain and Worn photo in the tonemap and the sharpening
    /// passes, the light leak, and the picture frame's rect. tonemapType is the setting's; the
    /// translated tonemap falls back to Neutral when Tony McMapface's table is missing.
    void fillToneParams(LuminousToneParams& params);

    /// Every lens pass's values for a frame of width x height pixels.
    void fillLensParams(LuminousLensParams& params, U32 width, U32 height);

    /// The tiles' and the gather's values for a source of width x height and the largest blur.
    void fillDoFParams(LuminousDoFParams& params, U32 width, U32 height, F32 max_cof);

    /// GTAO, AO on lamps, contact shadows and enhanced reflections. halfWidth and halfHeight:
    /// the half-size GTAO's target, 0 when none.
    void fillScreenParams(LuminousScreenParams& params, U32 half_width = 0, U32 half_height = 0);

    /// The soft shadows' values.
    void fillShadowParams(LuminousShadowParams& params);

    /// Sun shafts, volumetric light and the projector beams' shared values, for a frame of
    /// width x height: the passes' own uniforms and those bindDeferredShader gives them.
    void fillAirParams(LuminousAirParams& params, U32 width, U32 height);

    /// The projector beams, nearest first, as addProjectorBeam computes them for the projection now
    /// current: up to max (and LuminousPhotoProjectorBeamsMax) projectors in view, each with its
    /// projected texture in textures (the white image when it has none; null when not even that).
    /// A projector whose range is off screen is left out. Returns how many were filled.
    U32 fillAirBeams(LuminousAirBeam* beams, LLViewerTexture** textures, U32 max);

    /// The render's frame and weight.
    void fillRenderParams(LuminousRenderParams& params);
}

#endif // LUMINOUS_METAL_PARAMS_H
