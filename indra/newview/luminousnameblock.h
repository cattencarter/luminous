/**
 * @file luminousnameblock.h
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
#ifndef LUMINOUS_NAME_BLOCK_H
#define LUMINOUS_NAME_BLOCK_H

#include "llsingleton.h"
#include "llhost.h"
#include "lltimer.h"
#include "lluuid.h"

#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LLMessageSystem;
class LLViewerObject;

/// Hides and silences rezzed objects whose name is blocked, for one owner or
/// for anyone. A blocked name is an entry of the asset blacklist (type object,
/// flag NAME, with the owner or none), so it is listed, saved and removed in the
/// Asset Blacklist window with the rest, or a name in the Block list (block
/// object by name), which counts for anyone's objects unless it is the default
/// name of a new object ("Object", "Primitive").
///
/// Object updates carry no name. While at least one name is blocked (an
/// Asset Blacklist name, or a by-name entry in the Block list other than
/// Firestorm's group chat mutes), the viewer asks the region for the name of
/// each rezzed linkset it gets (RequestObjectPropertiesFamily, the request a
/// hover tip makes), at most LuminousNameBlockRate a second: linksets that
/// play a sound first, then linksets rezzed after the agent arrived, then the
/// rest, nearest first (the whole queue looked through every few seconds). An
/// object that does not answer twice is not asked again. With no name blocked
/// it asks nothing.
///
/// A matching linkset is derendered for the session (as Derender does), its
/// prims are not recreated, and sounds from any of its prims are dropped. The
/// owner and sound of each linkset hidden by name are remembered for the
/// session: a later linkset of unknown name that plays the same sound for the
/// same owner is hidden at once, before it is drawn or heard, and shown again
/// if its name turns out not to be blocked (that owner and sound then no
/// longer count). Linksets the agent owns, the one the agent sits on,
/// attachments, orphaned prims and avatars are never hidden. Removing a name
/// brings back the linksets it hid; temporary ones are forgotten five minutes
/// after they were hidden.
class LuminousNameBlock : public LLSingleton<LuminousNameBlock>
{
    LLSINGLETON(LuminousNameBlock);
    ~LuminousNameBlock();

public:
    /// Trimmed and lower-cased: names match whole, ignoring case.
    static std::string normalise(const std::string& name);
    /// The asset blacklist key of a name entry; owner null for anyone's objects.
    static LLUUID entryID(const std::string& name, const LLUUID& owner);

    /// Rereads the blocked names from the asset blacklist.
    void refreshNames();
    bool active() const { return !mNames.empty(); }
    bool isNameBlocked(const std::string& name, const LLUUID& owner) const;

    /// Blocks a name (saved), for owner's objects or (owner null) anyone's,
    /// and hides what matches. owner_name and region are shown in the Asset
    /// Blacklist window; an empty owner_name is looked up.
    void addName(const std::string& name, const LLUUID& owner, const std::string& owner_name, const std::string& region);
    /// Asks for a name to block.
    static void showAddNameDialog();

    /// An object's name and owner, from a reply or from the selection. Hides
    /// the object when its name is blocked for that owner.
    void noteName(const LLUUID& id, const std::string& name, const LLUUID& owner);

    /// From LLViewerObjectList, for each object it creates.
    void onObjectCreated(const LLUUID& id, bool from_cache);
    /// ObjectPropertiesFamily replies, ours and the hover tips'.
    void onPropertiesFamily(LLMessageSystem* msg);
    /// True when a sound from this prim must not play. parent_id is the
    /// sound's root prim when the message names one. An attached sound of a
    /// linkset hidden on its sound is left to stop with the linkset.
    bool blockSound(const LLUUID& object_id, const LLUUID& owner_id, const LLUUID& sound_id,
                    const LLUUID& parent_id, bool attached);

private:
    using signature_t = std::pair<LLUUID, LLUUID>; // owner, sound

    struct Where
    {
        U32 local_id{ 0 };
        U64 region_handle{ 0 };
        F64 time{ 0.0 };
        bool temporary{ false };
        std::vector<std::pair<LLUUID, U32>> children;
    };

    enum EPriority : U8
    {
        PRIORITY_SOUND = 0,
        PRIORITY_NEW = 1,
        PRIORITY_OTHER = 2
    };

    static void onIdle(void* user_data);
    void idle();
    void expire();
    void sendRequests(S32 count, bool urgent_only);
    void queue(const LLUUID& id, U8 priority);
    void dequeue(const LLUUID& id);
    void presume(LLViewerObject* objectp);
    void hideObject(LLViewerObject* objectp);
    void restore(const LLUUID& id, Where where);
    void queueExistingObjects();
    bool isHiddenID(const LLUUID& id) const;
    bool canHide(const LLViewerObject* objectp) const;
    bool logging() const;

    // Blocked names, each with the owners it applies to (null: anyone).
    std::unordered_map<std::string, std::unordered_set<LLUUID>> mNames;

    // Names and owners learned this session, by object; kept while no name is
    // blocked too.
    struct Known
    {
        std::string name;
        LLUUID owner;
    };
    std::unordered_map<LLUUID, Known> mKnownNames;
    // Objects waiting to be asked for (sounding ones also in mUrgent), and
    // asked for (the time asked), with the number of tries.
    std::unordered_map<LLUUID, U8> mQueue;
    std::unordered_set<LLUUID> mUrgent;
    std::unordered_map<LLUUID, F64> mAsked;
    std::unordered_map<LLUUID, U8> mTries;
    // The queue's nearest objects from its last full look (sendRequests), asked for in turn until the next.
    struct Pending
    {
        F64 key;
        LLUUID id;
        LLHost host;
    };
    std::vector<Pending> mNearest;
    size_t mNearestPos{ 0 };
    LLFrameTimer mNearestTimer;

    // Linksets hidden this session (by root), those hidden on their sound
    // alone whose name has not come yet, the prims of hidden linksets (to
    // their root), and roots to hide on the next idle.
    std::unordered_map<LLUUID, Where> mHidden;
    std::unordered_set<LLUUID> mPresumed;
    std::unordered_map<LLUUID, LLUUID> mHiddenChildren;
    std::unordered_set<LLUUID> mToKill;

    // The last sound heard from each linkset of unknown name; owner/sound
    // pairs of linksets hidden by name (to the name), and pairs that once
    // marked a linkset whose name was not blocked.
    std::unordered_map<LLUUID, signature_t> mHeard;
    std::map<signature_t, std::string> mSignatures;
    std::set<signature_t> mBadSignatures;

    F32 mBudget{ 0.f };
    LLFrameTimer mRateTimer;
    LLFrameTimer mScanTimer;
    LLFrameTimer mArrivalTimer;
    LLFrameTimer mExpireTimer;

    boost::signals2::connection mBlacklistConnection;
    boost::signals2::connection mRegionConnection;
    class MuteObserver;
    std::unique_ptr<MuteObserver> mMuteObserver;
};

#endif // LUMINOUS_NAME_BLOCK_H
