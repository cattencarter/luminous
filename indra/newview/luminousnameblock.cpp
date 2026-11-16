/**
 * @file luminousnameblock.cpp
 * @brief Hides and silences rezzed objects by name.
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

#include "luminousnameblock.h"

#include "fsassetblacklist.h"
#include "llagent.h"
#include "llavatarnamecache.h"
#include "llcallbacklist.h"
#include "llmutelist.h"
#include "llnotificationsutil.h"
#include "llselectmgr.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "message.h"

#include <algorithm>
#include <optional>

namespace
{
    // An unanswered request is asked again after this long, once.
    constexpr F64 REQUEST_TIMEOUT = 20.0;
    constexpr U8 MAX_TRIES = 2;
    // Objects created this long after the agent arrives in a region count as
    // newly rezzed rather than as the region's existing objects.
    constexpr F32 ARRIVAL_SETTLE = 20.f;
    // How often requests go out; sounding objects go at once.
    constexpr F32 SCAN_INTERVAL = 0.25f;
    // How often the whole queue is looked through for the nearest objects (or sooner when they are all asked for),
    // and how many of them are kept to be asked for in turn meanwhile.
    constexpr F32 NEAREST_INTERVAL = 3.f;
    constexpr size_t NEAREST_KEPT = 256;
    // A hidden temporary linkset is forgotten this long after it was hidden.
    constexpr F64 TEMPORARY_FORGET = 300.0;
    constexpr size_t MAX_KNOWN_NAMES = 200000;
    constexpr size_t MAX_HEARD = 4096;
}

class LuminousNameBlock::MuteObserver : public LLMuteListObserver
{
public:
    explicit MuteObserver(LuminousNameBlock* block) : mBlock(block) {}
    void onChange() override { mBlock->refreshNames(); }

private:
    LuminousNameBlock* mBlock;
};

LuminousNameBlock::LuminousNameBlock()
{
    gIdleCallbacks.addFunction(onIdle, this);

    mMuteObserver = std::make_unique<MuteObserver>(this);
    LLMuteList::getInstance()->addObserver(mMuteObserver.get());

    mBlacklistConnection = FSAssetBlacklist::getInstance()->setBlacklistChangedCallback(
        [this](const FSAssetBlacklist::changed_signal_data_t&, FSAssetBlacklist::eBlacklistOperation) { refreshNames(); });
    mRegionConnection = gAgent.addRegionChangedCallback([this]() { mArrivalTimer.reset(); });

    mArrivalTimer.reset();
    mRateTimer.reset();
    mScanTimer.reset();
    mExpireTimer.reset();
}

LuminousNameBlock::~LuminousNameBlock()
{
    gIdleCallbacks.deleteFunction(onIdle, this);
    if (LLMuteList::instanceExists())
    {
        LLMuteList::getInstance()->removeObserver(mMuteObserver.get());
    }
    mBlacklistConnection.disconnect();
    mRegionConnection.disconnect();
}

// static
std::string LuminousNameBlock::normalise(const std::string& name)
{
    std::string trimmed = name;
    LLStringUtil::trim(trimmed);
    return utf8str_tolower(trimmed);
}

// static
LLUUID LuminousNameBlock::entryID(const std::string& name, const LLUUID& owner)
{
    LLUUID id;
    id.generate("luminous-name-block:" + normalise(name) + (owner.notNull() ? ":" + owner.asString() : std::string()));
    return id;
}

bool LuminousNameBlock::isNameBlocked(const std::string& name, const LLUUID& owner) const
{
    if (mNames.empty())
    {
        return false;
    }
    const auto it = mNames.find(normalise(name));
    return it != mNames.end() && (it->second.count(LLUUID::null) || (owner.notNull() && it->second.count(owner)));
}

bool LuminousNameBlock::logging() const
{
    static LLCachedControl<bool> log(gSavedSettings, "LuminousNameBlockLog", false);
    return log;
}

void LuminousNameBlock::refreshNames()
{
    std::unordered_map<std::string, std::unordered_set<LLUUID>> names;
    for (const auto& [id, data] : FSAssetBlacklist::getInstance()->getBlacklistData())
    {
        if (data.type == LLAssetType::AT_OBJECT && (data.flags & FSAssetBlacklist::eBlacklistFlag::NAME))
        {
            std::string name = normalise(data.name);
            if (!name.empty())
            {
                names[name].insert(data.owner);
            }
        }
    }
    // The Block list's names, for anyone's objects; not the default names of
    // new objects, which a block by name meant for chat would otherwise hide
    // everywhere.
    if (LLMuteList::instanceExists())
    {
        for (const LLMute& mute : LLMuteList::getInstance()->getMutes())
        {
            if (mute.mType != LLMute::BY_NAME)
            {
                continue;
            }
            std::string name = normalise(mute.mName);
            // Firestorm's group chat mutes are kept as by-name entries "Group:<group id>": not object names.
            if (name.rfind("group:", 0) == 0 && LLUUID::validate(name.substr(6)))
            {
                continue;
            }
            if (!name.empty() && name != "object" && name != "primitive")
            {
                names[name].insert(LLUUID::null);
            }
        }
    }

    if (names == mNames)
    {
        return;
    }
    mNames.swap(names);

    LL_INFOS("LuminousNameBlock") << mNames.size() << " object name(s) blocked" << LL_ENDL;

    // Bring back what no blocked name matches any more.
    std::vector<std::pair<LLUUID, Where>> back;
    for (const auto& [id, where] : mHidden)
    {
        const auto known = mKnownNames.find(id);
        if (!active() || (known != mKnownNames.end() && !isNameBlocked(known->second.name, known->second.owner)))
        {
            back.emplace_back(id, where);
        }
    }
    for (const auto& [id, where] : back)
    {
        restore(id, where);
    }

    for (auto it = mSignatures.begin(); it != mSignatures.end();)
    {
        it = isNameBlocked(it->second, it->first.first) ? std::next(it) : mSignatures.erase(it);
    }

    if (!active())
    {
        mQueue.clear();
        mUrgent.clear();
        mAsked.clear();
        mTries.clear();
        mHeard.clear();
        mToKill.clear();
        return;
    }

    queueExistingObjects();
}

void LuminousNameBlock::queueExistingObjects()
{
    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLViewerObject* objectp = gObjectList.getObject(i);
        if (!canHide(objectp))
        {
            continue;
        }

        const LLUUID& id = objectp->getID();
        if (const auto known = mKnownNames.find(id); known != mKnownNames.end())
        {
            if (isNameBlocked(known->second.name, known->second.owner))
            {
                mToKill.insert(id);
            }
        }
        else if (!mAsked.count(id))
        {
            queue(id, PRIORITY_OTHER);
        }
    }
}

void LuminousNameBlock::addName(const std::string& name, const LLUUID& owner, const std::string& owner_name,
                                const std::string& region)
{
    std::string trimmed = name;
    LLStringUtil::trim(trimmed);
    if (trimmed.empty())
    {
        return;
    }

    FSAssetBlacklistData data;
    data.name = trimmed;
    data.region = region;
    data.type = LLAssetType::AT_OBJECT;
    data.flags = FSAssetBlacklist::eBlacklistFlag::NAME;
    data.date = LLDate((double)time_corrected());
    data.permanent = true;
    data.owner = owner;
    data.owner_name = owner_name;

    // The blacklist's change signal rereads the names.
    const LLUUID key = entryID(trimmed, owner);
    FSAssetBlacklist::getInstance()->addNewItemToBlacklistData(key, data, true);

    if (owner.notNull() && owner_name.empty())
    {
        LLAvatarNameCache::get(owner, [key](const LLUUID&, const LLAvatarName& av_name)
            {
                const auto entries = FSAssetBlacklist::getInstance()->getBlacklistData();
                if (const auto it = entries.find(key); it != entries.end() && it->second.owner_name.empty())
                {
                    FSAssetBlacklistData named = it->second;
                    named.owner_name = av_name.getUserName();
                    FSAssetBlacklist::getInstance()->addNewItemToBlacklistData(key, named, true);
                }
            });
    }
}

// static
void LuminousNameBlock::showAddNameDialog()
{
    LLNotificationsUtil::add("LuminousNameBlockAdd", LLSD(), LLSD(),
        [](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
            {
                return;
            }
            std::string region;
            if (LLViewerRegion* regionp = gAgent.getRegion())
            {
                region = regionp->getName();
            }
            LuminousNameBlock::getInstance()->addName(response["message"].asString(), LLUUID::null, std::string(), region);
        });
}

bool LuminousNameBlock::canHide(const LLViewerObject* objectp) const
{
    if (!objectp || objectp->isDead() || !objectp->getRegion())
    {
        return false;
    }
    // Only the region's objects, not the viewer's own (land, water, particles).
    const LLPCode pcode = objectp->getPCode();
    if (pcode != LL_PCODE_VOLUME && pcode != LL_PCODE_LEGACY_TREE && pcode != LL_PCODE_LEGACY_GRASS && pcode != LL_PCODE_TREE_NEW)
    {
        return false;
    }
    // Only a linkset's root: a child goes with it, an orphan waits for it,
    // and an attachment's root hangs off its avatar.
    if (objectp->isAvatar() || objectp->getParent() || objectp->isOrphaned() || objectp->isAttachment())
    {
        return false;
    }
    if (objectp->permYouOwner())
    {
        return false;
    }
    if (isAgentAvatarValid() && gAgentAvatarp->getRootEdit() == objectp)
    {
        return false;
    }
    return true;
}

bool LuminousNameBlock::isHiddenID(const LLUUID& id) const
{
    return mHidden.count(id) || mToKill.count(id) || mHiddenChildren.count(id);
}

void LuminousNameBlock::queue(const LLUUID& id, U8 priority)
{
    auto [it, inserted] = mQueue.try_emplace(id, priority);
    if (!inserted)
    {
        it->second = std::min(it->second, priority);
    }
    if (it->second == PRIORITY_SOUND)
    {
        mUrgent.insert(id);
    }
}

void LuminousNameBlock::dequeue(const LLUUID& id)
{
    mQueue.erase(id);
    mUrgent.erase(id);
}

void LuminousNameBlock::onObjectCreated(const LLUUID& id, bool from_cache)
{
    if (!active())
    {
        return;
    }

    // Back after the session's derender list was cleared (a teleport).
    if (mHidden.count(id) || mHiddenChildren.count(id))
    {
        mToKill.insert(id);
        if (mPresumed.count(id) && !mAsked.count(id))
        {
            queue(id, PRIORITY_SOUND);
        }
        return;
    }

    if (const auto known = mKnownNames.find(id); known != mKnownNames.end())
    {
        if (isNameBlocked(known->second.name, known->second.owner))
        {
            mToKill.insert(id);
        }
        return;
    }

    if (!mAsked.count(id))
    {
        const bool rezzed = !from_cache && mArrivalTimer.getElapsedTimeF32() > ARRIVAL_SETTLE;
        queue(id, rezzed ? PRIORITY_NEW : PRIORITY_OTHER);
    }
}

void LuminousNameBlock::onPropertiesFamily(LLMessageSystem* msg)
{
    LLUUID id;
    LLUUID owner;
    std::string name;
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, id);
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_OwnerID, owner);
    msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, name);
    if (id.notNull())
    {
        noteName(id, name, owner);
    }
}

void LuminousNameBlock::noteName(const LLUUID& id, const std::string& name, const LLUUID& owner)
{
    mAsked.erase(id);
    mTries.erase(id);
    dequeue(id);

    if (mKnownNames.size() >= MAX_KNOWN_NAMES)
    {
        for (auto it = mKnownNames.begin(); it != mKnownNames.end();)
        {
            it = mHidden.count(it->first) ? std::next(it) : mKnownNames.erase(it);
        }
    }
    mKnownNames[id] = Known{ name, owner };

    std::optional<signature_t> heard;
    if (const auto it = mHeard.find(id); it != mHeard.end())
    {
        heard = it->second;
        mHeard.erase(it);
    }

    if (!active())
    {
        return;
    }

    const bool blocked = isNameBlocked(name, owner);
    if (logging())
    {
        LL_INFOS("LuminousNameBlock") << "Name of " << id << ": \"" << name << "\", owner " << owner
                                      << (blocked ? " (blocked)" : "") << LL_ENDL;
    }

    if (mPresumed.count(id))
    {
        if (blocked)
        {
            mPresumed.erase(id);
        }
        else
        {
            if (heard)
            {
                mSignatures.erase(*heard);
                mBadSignatures.insert(*heard);
            }
            if (const auto it = mHidden.find(id); it != mHidden.end())
            {
                const Where where = it->second;
                restore(id, where);
            }
            LL_INFOS("LuminousNameBlock") << "Showing " << id << " again: its name \"" << name << "\" is not blocked" << LL_ENDL;
        }
        return;
    }

    if (!blocked || mHidden.count(id) || !canHide(gObjectList.findObject(id)))
    {
        return;
    }

    if (heard && !mBadSignatures.count(*heard))
    {
        mSignatures[*heard] = normalise(name);
    }
    mToKill.insert(id);
}

bool LuminousNameBlock::blockSound(const LLUUID& object_id, const LLUUID& owner_id, const LLUUID& sound_id,
                                   const LLUUID& parent_id, bool attached)
{
    if (isHiddenID(object_id) || (parent_id.notNull() && isHiddenID(parent_id)))
    {
        return true;
    }
    if (!active() || object_id.isNull())
    {
        return false;
    }

    // The linkset's root speaks for the prim.
    LLViewerObject* objectp = gObjectList.findObject(object_id);
    if (objectp && objectp->getParent() && !objectp->isAvatar())
    {
        LLViewerObject* rootp = objectp->getRootEdit();
        if (rootp && rootp != objectp)
        {
            if (isHiddenID(rootp->getID()))
            {
                return true;
            }
            objectp = rootp;
        }
    }
    else if (!objectp && parent_id.notNull())
    {
        objectp = gObjectList.findObject(parent_id);
    }

    if (!canHide(objectp))
    {
        return false;
    }
    const LLUUID id = objectp->getID();
    if (mKnownNames.count(id))
    {
        return false;
    }

    if (owner_id.notNull() && sound_id.notNull())
    {
        const signature_t signature{ owner_id, sound_id };
        if (mHeard.size() >= MAX_HEARD)
        {
            mHeard.clear();
        }
        mHeard[id] = signature;

        if (mSignatures.count(signature))
        {
            presume(objectp);
            // A looping attached sound stops with its object; if the prim
            // turns out to be a child of a linkset that stays, it plays on.
            return !attached;
        }
    }

    if (!mAsked.count(id))
    {
        queue(id, PRIORITY_SOUND);
    }
    return false;
}

void LuminousNameBlock::presume(LLViewerObject* objectp)
{
    const LLUUID& id = objectp->getID();
    if (!mHidden.count(id))
    {
        Where where;
        where.local_id = objectp->getLocalID();
        where.region_handle = objectp->getRegion()->getHandle();
        where.time = LLFrameTimer::getTotalSeconds();
        mHidden[id] = where;
    }
    mPresumed.insert(id);
    mToKill.insert(id);
    if (!mAsked.count(id))
    {
        queue(id, PRIORITY_SOUND);
    }
    if (logging())
    {
        LL_INFOS("LuminousNameBlock") << "Hiding " << id << " on its sound until its name comes" << LL_ENDL;
    }
}

void LuminousNameBlock::hideObject(LLViewerObject* objectp)
{
    const LLUUID id = objectp->getID();
    LLViewerRegion* regionp = objectp->getRegion();

    Where where;
    if (const auto it = mHidden.find(id); it != mHidden.end())
    {
        where = it->second;
    }
    where.local_id = objectp->getLocalID();
    where.region_handle = regionp->getHandle();
    where.time = LLFrameTimer::getTotalSeconds();
    where.temporary = objectp->flagTemporaryOnRez();
    for (LLViewerObject* childp : objectp->getChildren())
    {
        if (!childp || childp->isAvatar())
        {
            continue;
        }
        const LLUUID& child_id = childp->getID();
        if (std::none_of(where.children.begin(), where.children.end(), [&](const auto& c) { return c.first == child_id; }))
        {
            where.children.emplace_back(child_id, childp->getLocalID());
        }
    }
    for (const auto& [child_id, child_local_id] : where.children)
    {
        mHiddenChildren[child_id] = id;
        gObjectList.addDerenderedItem(child_id, false);
    }
    mHidden[id] = where;

    gObjectList.addDerenderedItem(id, false);
    gObjectList.killObject(objectp);
    LLSelectMgr::getInstance()->removeObjectFromSelections(id);
    if (LLViewerRegion::sVOCacheCullingEnabled)
    {
        regionp->killCacheEntry(where.local_id);
        for (const auto& [child_id, child_local_id] : where.children)
        {
            regionp->killCacheEntry(child_local_id);
        }
    }

    if (logging())
    {
        const auto known = mKnownNames.find(id);
        LL_INFOS("LuminousNameBlock") << "Hid " << id << " \"" << (known != mKnownNames.end() ? known->second.name : std::string())
                                      << "\" with " << where.children.size() << " child prim(s)" << LL_ENDL;
    }
}

void LuminousNameBlock::restore(const LLUUID& id, Where where)
{
    mHidden.erase(id);
    mPresumed.erase(id);
    mToKill.erase(id);
    gObjectList.removeDerenderedItem(id);
    for (const auto& [child_id, child_local_id] : where.children)
    {
        mHiddenChildren.erase(child_id);
        gObjectList.removeDerenderedItem(child_id);
    }

    if (gObjectList.findObject(id))
    {
        return;
    }
    if (LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(where.region_handle))
    {
        regionp->addCacheMissFull(where.local_id);
        for (const auto& [child_id, child_local_id] : where.children)
        {
            regionp->addCacheMissFull(child_local_id);
        }
    }
}

// static
void LuminousNameBlock::onIdle(void* user_data)
{
    static_cast<LuminousNameBlock*>(user_data)->idle();
}

void LuminousNameBlock::idle()
{
    if (!mToKill.empty())
    {
        const std::vector<LLUUID> ids(mToKill.begin(), mToKill.end());
        mToKill.clear();
        for (const LLUUID& id : ids)
        {
            LLViewerObject* objectp = gObjectList.findObject(id);
            if (mHiddenChildren.count(id))
            {
                // A prim of a hidden linkset, back on its own.
                if (objectp && !objectp->isDead())
                {
                    gObjectList.addDerenderedItem(id, false);
                    gObjectList.killObject(objectp);
                }
            }
            else if (canHide(objectp))
            {
                hideObject(objectp);
            }
            else if (objectp && !objectp->isDead() && mPresumed.count(id))
            {
                // Not a linkset to hide after all (a child, the agent's own).
                mHidden.erase(id);
                mPresumed.erase(id);
            }
        }
    }

    if (!active())
    {
        return;
    }

    static LLCachedControl<F32> rate_setting(gSavedSettings, "LuminousNameBlockRate", 20.f);
    const F32 rate = llclamp((F32)rate_setting, 1.f, 100.f);
    mBudget = llmin(mBudget + mRateTimer.getElapsedTimeAndResetF32() * rate, rate);

    if (mExpireTimer.getElapsedTimeF32() > 1.f)
    {
        mExpireTimer.reset();
        expire();
    }

    if (mBudget >= 1.f && !mUrgent.empty())
    {
        sendRequests((S32)mBudget, true);
    }
    if (mBudget >= 1.f && !mQueue.empty() && mScanTimer.getElapsedTimeF32() >= SCAN_INTERVAL)
    {
        mScanTimer.reset();
        sendRequests((S32)mBudget, false);
    }
}

void LuminousNameBlock::expire()
{
    const F64 now = LLFrameTimer::getTotalSeconds();

    std::vector<LLUUID> unanswered;
    for (auto it = mAsked.begin(); it != mAsked.end();)
    {
        if (now - it->second < REQUEST_TIMEOUT)
        {
            ++it;
            continue;
        }
        unanswered.push_back(it->first);
        it = mAsked.erase(it);
    }
    for (const LLUUID& id : unanswered)
    {
        const bool presumed = mPresumed.count(id) != 0;
        if (mTries[id] < MAX_TRIES && (presumed || canHide(gObjectList.findObject(id))))
        {
            queue(id, presumed ? PRIORITY_SOUND : PRIORITY_OTHER);
            continue;
        }
        mTries.erase(id);
        if (presumed)
        {
            // No name: show it again rather than hide it on a guess.
            if (const auto it = mHidden.find(id); it != mHidden.end())
            {
                const Where where = it->second;
                restore(id, where);
            }
        }
        else if (!mKnownNames.count(id))
        {
            // Not asked again.
            mKnownNames[id] = Known{};
        }
    }

    // Temporary linksets are gone from the region long before this.
    std::vector<LLUUID> forget;
    for (const auto& [id, where] : mHidden)
    {
        if (where.temporary && !mPresumed.count(id) && now - where.time > TEMPORARY_FORGET && !gObjectList.findObject(id))
        {
            forget.push_back(id);
        }
    }
    for (const LLUUID& id : forget)
    {
        const auto it = mHidden.find(id);
        for (const auto& [child_id, child_local_id] : it->second.children)
        {
            mHiddenChildren.erase(child_id);
            gObjectList.removeDerenderedItem(child_id);
        }
        gObjectList.removeDerenderedItem(id);
        mKnownNames.erase(id);
        mHidden.erase(it);
    }
}

void LuminousNameBlock::sendRequests(S32 count, bool urgent_only)
{
    // The queue's entries that can still be asked for, keyed by priority, then distance; others leave the queue.
    auto collect = [this](const std::vector<LLUUID>& ids)
    {
        std::vector<Pending> candidates;
        candidates.reserve(ids.size());
        const LLVector3d agent_pos = gAgent.getPositionGlobal();
        std::vector<std::pair<LLUUID, Where>> lost;

        for (const LLUUID& id : ids)
        {
            const auto queued = mQueue.find(id);
            if (queued == mQueue.end())
            {
                mUrgent.erase(id);
                continue;
            }

            LLHost host;
            F64 distance_sq = 0.0;
            if (mPresumed.count(id))
            {
                const auto where = mHidden.find(id);
                LLViewerRegion* regionp = where != mHidden.end() ? LLWorld::getInstance()->getRegionFromHandle(where->second.region_handle) : nullptr;
                if (!regionp)
                {
                    if (where != mHidden.end())
                    {
                        lost.emplace_back(id, where->second);
                    }
                    dequeue(id);
                    continue;
                }
                host = regionp->getHost();
            }
            else
            {
                LLViewerObject* objectp = gObjectList.findObject(id);
                if (!canHide(objectp))
                {
                    dequeue(id);
                    continue;
                }
                host = objectp->getRegion()->getHost();
                distance_sq = (objectp->getPositionGlobal() - agent_pos).magVecSquared();
            }

            // Priority first, then distance (at most a few million square metres).
            candidates.push_back({ queued->second * 1.0e12 + distance_sq, id, host });
        }

        // A presumed linkset whose region is gone is shown again.
        for (const auto& [id, where] : lost)
        {
            restore(id, where);
        }
        return candidates;
    };

    if (!gMessageSystem || count <= 0)
    {
        return;
    }

    std::vector<Pending> chosen;
    if (urgent_only)
    {
        std::vector<Pending> candidates = collect(std::vector<LLUUID>(mUrgent.begin(), mUrgent.end()));
        const size_t n = std::min<size_t>((size_t)count, candidates.size());
        std::partial_sort(candidates.begin(), candidates.begin() + n, candidates.end(),
                          [](const Pending& a, const Pending& b) { return a.key < b.key; });
        chosen.assign(candidates.begin(), candidates.begin() + n);
    }
    else
    {
        // The whole queue is looked through only now and then: its nearest objects are kept and asked for in turn.
        if (mNearestPos >= mNearest.size() || mNearestTimer.getElapsedTimeF32() >= NEAREST_INTERVAL)
        {
            std::vector<LLUUID> ids;
            ids.reserve(mQueue.size());
            for (const auto& [id, priority] : mQueue)
            {
                ids.push_back(id);
            }
            std::vector<Pending> candidates = collect(ids);
            const size_t kept = std::min(NEAREST_KEPT, candidates.size());
            std::partial_sort(candidates.begin(), candidates.begin() + kept, candidates.end(),
                              [](const Pending& a, const Pending& b) { return a.key < b.key; });
            candidates.resize(kept);
            mNearest.swap(candidates);
            mNearestPos = 0;
            mNearestTimer.reset();
        }
        while (chosen.size() < (size_t)count && mNearestPos < mNearest.size())
        {
            const Pending& c = mNearest[mNearestPos++];
            if (mQueue.count(c.id))
            {
                chosen.push_back(c);
            }
        }
    }

    const size_t n = chosen.size();
    if (n == 0)
    {
        return;
    }
    const F64 now = LLFrameTimer::getTotalSeconds();
    LLMessageSystem* msg = gMessageSystem;
    for (const Pending& c : chosen)
    {
        msg->newMessageFast(_PREHASH_RequestObjectPropertiesFamily);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_RequestFlags, 0);
        msg->addUUIDFast(_PREHASH_ObjectID, c.id);
        msg->sendReliable(c.host);

        dequeue(c.id);
        mAsked[c.id] = now;
        ++mTries[c.id];

        if (logging())
        {
            LLViewerObject* objectp = gObjectList.findObject(c.id);
            LL_INFOS("LuminousNameBlock") << "Asking " << c.id << " try " << (S32)mTries[c.id]
                                          << " pcode " << (S32)(objectp ? objectp->getPCode() : 0)
                                          << " temp " << (objectp && objectp->flagTemporaryOnRez())
                                          << " key " << c.key << LL_ENDL;
        }
    }
    mBudget -= (F32)n;

    if (logging())
    {
        LL_INFOS("LuminousNameBlock") << "Asked " << n << " name(s); " << mQueue.size() << " waiting, "
                                      << mHidden.size() << " hidden" << LL_ENDL;
    }
}
