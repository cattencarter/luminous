/**
 * @file luminousmediaprivacy.cpp
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

#include "llviewerprecompiledheaders.h"

#include "luminousmediaprivacy.h"
#include "luminousmediaurl.h"

#include "fscommon.h"
#include "llnotificationsutil.h"
#include "llpluginclassmedia.h"
#include "lltrans.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "llviewermedia.h"
#include "llviewerparcelmedia.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <vector>

namespace
{
    /// This session's answers by domain: "allow" or "deny" (Allow and Deny in the alert, not kept in medialist.xml).
    std::map<std::string, std::string> sSession;
    /// Media on prims waiting for their domain's answer, by the media's texture id.
    std::map<std::string, std::vector<LLUUID>> sWaiting;
    /// Domains to ask about, the first one's alert showing; one alert at a time.
    std::deque<std::string> sToAsk;
    /// Domains already reported as blocked in nearby chat this session.
    std::set<std::string> sReported;
    /// The media filter's changes that can let held media through (filterGeneration).
    unsigned sGeneration = 0;

    /// The media filter's action for the domain in medialist.xml: "allow", "deny", or empty when it is not listed.
    /// The whole address as listed, or the domain or one under it (LuminousMediaURL::domainMatches: "example.com"
    /// covers "www.example.com", not "notexample.com").
    std::string listedAction(const std::string& url, const std::string& domain)
    {
        const std::string trimmed = LuminousMediaURL::trimmed(url);
        const LLSD& list = LLViewerParcelMedia::getInstance()->mMediaFilterList;
        for (LLSD::array_const_iterator it = list.beginArray(); it != list.endArray(); ++it)
        {
            const std::string listed = (*it)["domain"].asString();
            if (url == listed || trimmed == listed || LuminousMediaURL::domainMatches(domain, listed))
            {
                return (*it)["action"].asString();
            }
        }
        return std::string();
    }

    /// The domain the media filter judges for this address: the host a browser or libcurl would connect to.
    std::string domainOf(const std::string& url)
    {
        return LuminousMediaURL::parse(url).host;
    }

    /// The impl for a waiting texture id, if it still exists and its page is still in this domain.
    LLViewerMediaImpl* stillWaiting(const LLUUID& id, const std::string& domain)
    {
        LLViewerMediaImpl* impl = LLViewerMedia::getInstance()->getMediaImplFromTextureID(id);
        return impl && domainOf(impl->getMediaURL()) == domain ? impl : nullptr;
    }

    void showNextAlert();

    void answered(const LLSD& notification, const LLSD& response)
    {
        const S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
        const std::string domain = notification["payload"]["domain"].asString();
        bool allowed = false;
        if (option == 0) // Allow, this session
        {
            sSession[domain] = "allow";
            allowed = true;
        }
        else if (option == 1) // Deny, this session
        {
            sSession[domain] = "deny";
        }
        else if (option == 2 || option == 3) // Blacklist, Whitelist: kept in medialist.xml
        {
            allowed = option == 3;
            LLViewerParcelMedia* filter = LLViewerParcelMedia::getInstance();
            LLSD entry;
            entry["domain"] = domain;
            entry["action"] = allowed ? "allow" : "deny";
            filter->mMediaFilterList.append(entry);
            filter->saveDomainFilterList();
            LLStringUtil::format_map_t args;
            args["[DOMAIN]"] = domain;
            FSCommon::report_to_nearby_chat(LLTrans::getString(allowed ? "MediaFilterMediaContentDomainAlwaysAllowed"
                                                                       : "MediaFilterMediaContentDomainAlwaysBlocked",
                                                               args));
        }
        else
        {
            sSession[domain] = "deny"; // the alert closed without an answer
        }

        ++sGeneration; // media held for this domain elsewhere, or under an older answer, are asked about again
        const std::vector<LLUUID> waiting = sWaiting[domain];
        sWaiting.erase(domain);
        LL_INFOS("MediaFilter") << "Media on prims from " << domain << (allowed ? " allowed" : " not allowed") << " ("
                                << waiting.size() << " waiting)" << LL_ENDL;
        if (allowed)
        {
            for (const LLUUID& id : waiting)
            {
                if (LLViewerMediaImpl* impl = stillWaiting(id, domain))
                {
                    impl->navigateInternal();
                }
            }
        }

        if (!sToAsk.empty() && sToAsk.front() == domain)
        {
            sToAsk.pop_front();
        }
        showNextAlert();
    }

    void showNextAlert()
    {
        while (!sToAsk.empty())
        {
            const std::string domain = sToAsk.front();
            // Only media that still exist and still show a page from this domain wait for it (a teleport away, or a
            // new address, leaves nothing to ask about).
            LLViewerMediaImpl* first = nullptr;
            const auto waiting = sWaiting.find(domain);
            if (waiting != sWaiting.end())
            {
                std::vector<LLUUID>& ids = waiting->second;
                ids.erase(std::remove_if(ids.begin(), ids.end(),
                                         [&domain](const LLUUID& id) { return !stillWaiting(id, domain); }),
                          ids.end());
                if (!ids.empty())
                {
                    first = stillWaiting(ids.front(), domain);
                }
                else
                {
                    sWaiting.erase(waiting);
                }
            }
            if (!first)
            {
                sToAsk.pop_front();
                continue;
            }
            // The page's address as the alert shows it: the host and the first part of the path only
            // (LuminousMediaURL::shown).
            const std::string url = LuminousMediaURL::shown(LuminousMediaURL::parse(first->getMediaURL()));
            LLSD args;
            args["MEDIADOMAIN"] = domain;
            args["MEDIAURL"] = url;
            LLSD payload;
            payload["domain"] = domain;
            LLNotificationsUtil::add("LuminousPrimMediaAlert", args, payload, &answered);
            return;
        }
    }
}

namespace LuminousMediaPrivacy
{
    bool inWorld(const LLViewerMediaImpl* impl)
    {
        return impl && !impl->getUsedInUI() && (impl->isParcelMedia() || impl->isMediaOnPrim());
    }

    bool holdPrimMedia(LLViewerMediaImpl* impl, const std::string& url)
    {
        if (!impl || impl->getUsedInUI() || impl->isParcelMedia() || !impl->isMediaOnPrim())
        {
            return false;
        }
        const LuminousMediaURL::Parts parts = LuminousMediaURL::parse(url);
        if (parts.local)
        {
            return false;
        }
        static LLCachedControl<bool> filter_on(gSavedSettings, "MediaEnableFilter", true);
        if (!filter_on)
        {
            return false;
        }
        const std::string& domain = parts.host;
        // medialist.xml first (Blacklist and Whitelist are kept), then this session's Allow or Deny.
        std::string action = listedAction(url, domain);
        if (action.empty())
        {
            const auto session = sSession.find(domain);
            if (session != sSession.end())
            {
                action = session->second;
            }
        }
        if (action == "allow")
        {
            return false;
        }
        if (action == "deny")
        {
            if (sReported.insert(domain).second)
            {
                LLStringUtil::format_map_t args;
                args["[DOMAIN]"] = domain;
                FSCommon::report_to_nearby_chat(LLTrans::getString("MediaFilterMediaContentBlocked", args));
            }
            return true;
        }
        const LLUUID id = impl->getMediaTextureID();
        for (auto& other : sWaiting) // a face whose address moved to another domain waits only for that one
        {
            if (other.first != domain)
            {
                other.second.erase(std::remove(other.second.begin(), other.second.end(), id), other.second.end());
            }
        }
        std::vector<LLUUID>& waiting = sWaiting[domain];
        if (std::find(waiting.begin(), waiting.end(), id) == waiting.end())
        {
            waiting.push_back(id);
        }
        if (std::find(sToAsk.begin(), sToAsk.end(), domain) == sToAsk.end())
        {
            sToAsk.push_back(domain);
            if (sToAsk.size() == 1)
            {
                showNextAlert();
            }
        }
        return true;
    }

    unsigned filterGeneration()
    {
        static bool listening = false;
        if (!listening)
        {
            listening = true;
            if (LLControlVariable* control = gSavedSettings.getControl("MediaEnableFilter"))
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { filterChanged(); });
            }
        }
        return sGeneration;
    }

    void filterChanged()
    {
        ++sGeneration;
    }

    bool privateFor(LLPluginClassMediaOwner* owner)
    {
        static LLCachedControl<bool> private_media(gSavedSettings, "LuminousPrivateMedia", false);
        return private_media && inWorld(dynamic_cast<LLViewerMediaImpl*>(owner));
    }

    void applyPrivate(LLPluginClassMedia* source)
    {
        if (!source)
        {
            return;
        }
        source->cookies_enabled(false);
        source->setWebGLEnabled(false);
        source->setGPUDisabled(true);
        source->setLanguageCode("en-us");
    }
}
