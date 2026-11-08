/**
 * @file luminousupdatecheck.cpp
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

#include "llviewerprecompiledheaders.h"

#include "luminousupdatecheck.h"

#include "indra_constants.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llhttpconstants.h"
#include "llnotificationsutil.h"
#include "llsdjson.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llweb.h"
#include "llwindow.h"

namespace
{
    // Today on the Mac's own calendar, as "YYYY-MM-DD"
    std::string today()
    {
        const time_t now = time(nullptr);
        struct tm local {};
        localtime_r(&now, &local);
        char day[16] = {};
        strftime(day, sizeof(day), "%Y-%m-%d", &local);
        return day;
    }

    void checkCoro(const std::string& url)
    {
        LL_INFOS("LuminousUpdate") << "Asking " << url << " for the newest version" << LL_ENDL;
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("LuminousUpdateCheck", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
        options->setRetries(0); // one request; next session asks again
        options->setTimeout(30);
        options->setTransferTimeout(30);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");

        LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
        const LLCore::HttpStatus status =
            LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        if (!status)
        {
            LL_INFOS("LuminousUpdate") << "No answer: " << status.toString() << LL_ENDL;
            return;
        }
        const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        const std::string answer(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value json = boost::json::parse(answer, ec);
        const LLSD newest = ec ? LLSD() : LlsdFromJson(json);
        const std::string release = newest["version"].asString();
        if (release.empty())
        {
            LL_WARNS("LuminousUpdate") << "The answer names no version" << LL_ENDL;
            return;
        }
        const LLSD pre = newest["prerelease"];
        const std::string pre_version = pre.isMap() ? pre["version"].asString() : std::string();
        const LuminousUpdateCheck::Offer offer = LuminousUpdateCheck::offerFor(LUMEN_VERSION, release, pre_version);
        if (offer == LuminousUpdateCheck::Offer::None)
        {
            LL_INFOS("LuminousUpdate") << "Up to date: " << LUMEN_VERSION << ", the newest " << release
                                       << (pre_version.empty() ? std::string() : ", the newest pre-release " + pre_version) << LL_ENDL;
            return;
        }
        // The version told of, with its own link and notes
        const bool prerelease = offer == LuminousUpdateCheck::Offer::Prerelease;
        const LLSD offered = prerelease ? pre : newest;
        const std::string version = prerelease ? pre_version : release;
        // Once a day while this one is older, whichever button -- Download
        // too, so the message still comes the next day to a viewer that was
        // not updated; a newer version at once.
        if (gSavedSettings.getString("LuminousUpdateSeen") == version
            && gSavedSettings.getString("LuminousUpdateToldDay") == today())
        {
            LL_INFOS("LuminousUpdate") << "Luminous " << version << " is out; already told today" << LL_ENDL;
            return;
        }
        // Only a web page of https, in the browser, when Download is clicked
        std::string link = offered["url"].asString();
        if (link.rfind("https://", 0) != 0)
        {
            link = "https://needfulsl.com/luminous/";
        }
        std::string notes = offered["notes"].asString();
        LLStringUtil::trim(notes);
        if (notes.size() > 600)
        {
            notes = notes.substr(0, 600) + "...";
        }
        std::string message = "Luminous " + version + (prerelease ? " is out as a pre-release" : " is out")
                              + " (this is " + LUMEN_VERSION + ").";
        if (!notes.empty())
        {
            message += "\n\n" + notes;
        }
        LL_INFOS("LuminousUpdate") << "Luminous " << version << (prerelease ? " (pre-release)" : "") << " is out: telling" << LL_ENDL;
        gSavedSettings.setString("LuminousUpdateSeen", version);
        gSavedSettings.setString("LuminousUpdateToldDay", today());
        LLNotificationsUtil::add("LuminousUpdateAvailable", LLSD().with("MESSAGE", message), LLSD(),
            [link](const LLSD& notification, const LLSD& response)
            {
                // Download: the click is the consent, so no second question
                // as LLWeb::loadURLExternal asks
                if (LLNotificationsUtil::getSelectedOption(notification, response) == 0 && gViewerWindow
                    && !gSavedSettings.getBOOL("DisableExternalBrowser"))
                {
                    gViewerWindow->getWindow()->spawnWebBrowser(LLWeb::escapeURL(link), true);
                }
                return false;
            });
    }
}

namespace LuminousUpdateCheck
{
    void check()
    {
        static bool asked = false;
        if (asked || !gSavedSettings.getBOOL("LuminousUpdateCheck"))
        {
            return;
        }
        asked = true;
        const std::string url = gSavedSettings.getString("LuminousUpdateURL");
        if (url.rfind("https://", 0) != 0)
        {
            return;
        }
        LLCoros::instance().launch("LuminousUpdateCheck", [url]() { checkCoro(url); });
    }
}
