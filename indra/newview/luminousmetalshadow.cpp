/**
 * @file luminousmetalshadow.cpp
 * @brief Photo mode on Metal: shadow maps and soft shadows, the group's native passes.
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

#include "llviewerprecompiledheaders.h"

#include "luminousmetal.h"
#include "luminousmetalparams.h"

namespace LuminousMetal
{
    // The passes this group owns:
    //   none yet
    // Each is registered here as table.fn[size_t(Pass::X)] = function, and said ready with
    // setReady(Pass::X, true) once its native library has compiled. Until then the effect is off on
    // Metal. Soft shadows are hosted in shadowUtil.glsl and have no pass of their own yet; a native pass (a min/max map, a soft shadow term) gets a Pass value of its own. Parameters in shadow/shadowshared.h (fillShadowParams, a stub).
    void registerShadows(PassTable& table)
    {
        (void)table;
    }
}
