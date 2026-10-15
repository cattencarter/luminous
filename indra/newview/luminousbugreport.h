/**
 * @file luminousbugreport.h
 * @brief Report a Bug: a report sent to Needful, with the viewer's logs and a picture.
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
#ifndef LUMINOUS_BUG_REPORT_H
#define LUMINOUS_BUG_REPORT_H

#include "llfloater.h"

/// Help > Report a Bug. What went wrong, in the user's words, with the
/// viewer log (ticked as it opens: bugs are hard to find without it), the
/// last session's log (ticked when that session crashed) and a picture of
/// the user's choosing, sent to Needful's bug report API
/// (LuminousBugReportURL) in one request when Send is clicked -- the
/// viewer's only call to it. The key comes from outside the repository at
/// build time (luminousbugreportkey.h.in).
class LuminousBugReportFloater : public LLFloater
{
public:
    LuminousBugReportFloater(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    /// The outcome of a send. Received: the window closes, its text cleared,
    /// and the server's thanks show as a tip. Not: what went wrong shows in
    /// the window, the text kept.
    void sent(bool success, const std::string& message);

private:
    void onChoosePicture();
    void onSend();
    void showStatus(const std::string& text);

    std::string mPicturePath;
    bool mSending = false;
};

#endif // LUMINOUS_BUG_REPORT_H
