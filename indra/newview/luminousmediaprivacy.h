/**
 * @file luminousmediaprivacy.h
 * @brief Media in the world and privacy: the media filter for media on prims, and private media.
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
#ifndef LUMINOUS_MEDIA_PRIVACY_H
#define LUMINOUS_MEDIA_PRIVACY_H

#include <string>

class LLPluginClassMedia;
class LLPluginClassMediaOwner;
class LLViewerMediaImpl;

namespace LuminousMediaPrivacy
{
    /// Media in the world: the parcel's or on a prim, not a browser of the interface.
    bool inWorld(const LLViewerMediaImpl* impl);

    /// Media on a prim whose page waits instead of loading: the media filter (MediaEnableFilter) is on and the page's
    /// domain is not allowed in medialist.xml or for this session. An unknown domain is asked about once; the media
    /// loads when the domain is allowed. Called before the page's MIME probe and before the page itself. Every
    /// address that can reach a network waits, whatever its spelling (LuminousMediaURL); only an empty one, about:,
    /// file: and secondlife: do not.
    bool holdPrimMedia(LLViewerMediaImpl* impl, const std::string& url);

    /// Counts the media filter's changes that can let held media through (an answer in the alert, a change to
    /// medialist.xml, the filter switched): media held under an older count are asked about again.
    unsigned filterGeneration();
    /// medialist.xml or the filter's switch changed.
    void filterChanged();

    /// A new media source for this owner gets the private browser: LuminousPrivateMedia is on and the owner is media
    /// in the world.
    bool privateFor(LLPluginClassMediaOwner* owner);

    /// The private browser: no cookies, no WebGL, no graphics card, and a common language list.
    void applyPrivate(LLPluginClassMedia* source);
}

#endif // LUMINOUS_MEDIA_PRIVACY_H
