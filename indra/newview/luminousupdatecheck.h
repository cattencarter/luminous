/**
 * @file luminousupdatecheck.h
 * @brief The update message: a notice when Needful has a newer Luminous.
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
#ifndef LUMINOUS_UPDATE_CHECK_H
#define LUMINOUS_UPDATE_CHECK_H

#include <string>

/// The update message, when a new version is out. Once a session, as the
/// login screen shows, so that users have it before they log in: one GET of
/// a small JSON file on needfulsl.com (`LuminousUpdateURL`):
///
///     { "version": "0.9.0",
///       "url": "https://needfulsl.com/luminous/",
///       "notes": "What is new, a line or two." }
///
/// When its version is newer than this one (`LUMEN_VERSION`), a window over
/// the login screen with the notes, Download -- the url (https only) in the
/// browser -- and Later; told again the next day the viewer starts while it
/// is still older, whichever was clicked (`LuminousUpdateSeen`,
/// `LuminousUpdateToldDay`).
/// An answer that comes after the login shows in the world instead. Nothing is
/// sent but the request itself; no answer, or a bad one, is only logged.
/// `LuminousUpdateCheck` off: no request at all.
namespace LuminousUpdateCheck
{
    void check();

    /// Whether `offered` ("1.2.3") is a later version than `own`.
    bool isNewer(const std::string& offered, const std::string& own);
}

#endif // LUMINOUS_UPDATE_CHECK_H
