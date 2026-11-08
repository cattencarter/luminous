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

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

/// The update message, when a new version is out. Once a session, as the
/// login screen shows, so that users have it before they log in: one GET of
/// a small JSON file on needfulsl.com (`LuminousUpdateURL`):
///
///     { "version": "0.9.0",
///       "url": "https://needfulsl.com/luminous/",
///       "notes": "What is new, a line or two.",
///       "prerelease": { "version": "0.9.1",
///                       "url": "https://needfulsl.com/luminous/download.php#pre-release",
///                       "notes": "What the pre-release changes." } }
///
/// `version` is the release; `prerelease`, optional, the newest pre-release.
/// When the release is newer than this one (`LUMEN_VERSION`), or this one is
/// newer than the release -- a pre-release itself -- and the pre-release is
/// newer still (`offerFor`), a window over the login screen with that
/// version's notes, Download -- its url (https only) in the browser -- and
/// Later; told again the next day the viewer starts while it is still older,
/// whichever was clicked (`LuminousUpdateSeen`, `LuminousUpdateToldDay`).
/// Viewers on the release are never told of a pre-release, and viewers that
/// read only the release's keys pass `prerelease` by.
/// An answer that comes after the login shows in the world instead. Nothing is
/// sent but the request itself; no answer, or a bad one, is only logged.
/// `LuminousUpdateCheck` off: no request at all.
namespace LuminousUpdateCheck
{
    void check();

    /// "1.2.3" as numbers: only a part's digits count, so the file's versions
    /// are digits and dots; a part without digits counts as 0.
    inline std::vector<int> versionParts(const std::string& version)
    {
        std::vector<int> parts;
        std::string part;
        for (char c : version + ".")
        {
            if (c == '.')
            {
                parts.push_back(part.empty() ? 0 : atoi(part.c_str()));
                part.clear();
            }
            else if (isdigit((unsigned char)c))
            {
                part += c;
            }
        }
        return parts;
    }

    /// Whether `offered` ("1.2.3") is a later version than `own`.
    inline bool isNewer(const std::string& offered, const std::string& own)
    {
        std::vector<int> a = versionParts(offered);
        std::vector<int> b = versionParts(own);
        const size_t n = std::max(a.size(), b.size());
        a.resize(n, 0);
        b.resize(n, 0);
        return a > b;
    }

    enum class Offer { None, Release, Prerelease };

    /// Which version a viewer of version `own` is told of: the release when
    /// it is newer; otherwise the pre-release (empty: none) when the viewer is
    /// newer than the release -- on the pre-release line -- and the
    /// pre-release newer than the viewer; otherwise none.
    inline Offer offerFor(const std::string& own, const std::string& release, const std::string& prerelease)
    {
        if (isNewer(release, own))
        {
            return Offer::Release;
        }
        if (!prerelease.empty() && isNewer(own, release) && isNewer(prerelease, own))
        {
            return Offer::Prerelease;
        }
        return Offer::None;
    }
}

#endif // LUMINOUS_UPDATE_CHECK_H
