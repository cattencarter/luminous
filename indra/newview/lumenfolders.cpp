/**
 * @file lumenfolders.cpp
 * @brief Which top-level inventory folders Luminous uses, and making them.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Lumen Viewer Source Code
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

#include "lumenfolders.h"

#include "llinventorymodel.h"
#include "llviewerinventory.h"

#include <map>
#include <vector>

namespace LumenFolders
{
    const std::string FIRESTORM_FOLDER = "#Firestorm";
    const std::string LUMEN_FOLDER     = "#Lumen";
    const std::string LUMINOUS_FOLDER  = "#Luminous";
}

namespace
{
    bool               sDecided = false;
    const std::string* sShared  = &LumenFolders::FIRESTORM_FOLDER;

    // Callbacks waiting for a folder being made, by name
    std::map<std::string, std::vector<std::function<void(const LLUUID&)>>> sMaking;

    LLUUID childNamed(const LLUUID& parent, const std::string& name)
    {
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(parent, cats, items);
        if (cats)
        {
            for (const auto& cat : *cats)
            {
                if (cat && cat->getName() == name) return cat->getUUID();
            }
        }
        return LLUUID::null;
    }

    /**
     * How many things are in this root's AO, and whether that is known --
     * Lumen's own test. The AO engine makes an empty `#AO` by itself, so the
     * folder being there says nothing; animations inside it say somebody set
     * one up.
     */
    S32 aoItems(const LLUUID& root, bool& known)
    {
        known = true;
        const LLUUID ao = childNamed(root, "#AO");
        if (ao.isNull()) return 0;
        LLViewerInventoryCategory* top = gInventory.getCategory(ao);
        if (!top || top->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
        {
            known = false;
            return 0;
        }
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(ao, cats, items, LLInventoryModel::EXCLUDE_TRASH);
        for (const auto& cat : cats)
        {
            if (cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
            {
                known = false;
            }
        }
        return (S32)items.size();
    }

    /**
     * Once a session, Lumen's rules with `#Luminous` for `#Lumen`:
     *   - `#Firestorm`, shared, when the account has one -- unless only
     *     `#Luminous` holds a real AO (set up before Firestorm made its own
     *     empty one): that AO keeps being used, and moving it is the
     *     user's;
     *   - otherwise `#Luminous`.
     * Nothing is renamed or moved, whichever it is.
     */
    void decide()
    {
        if (sDecided || !gInventory.isInventoryUsable())
        {
            return;
        }
        sDecided = true;
        const LLUUID fs = gInventory.findCategoryByName(LumenFolders::FIRESTORM_FOLDER);
        const LLUUID lu = gInventory.findCategoryByName(LumenFolders::LUMINOUS_FOLDER);
        sShared = fs.notNull() ? &LumenFolders::FIRESTORM_FOLDER : &LumenFolders::LUMINOUS_FOLDER;
        if (fs.notNull() && lu.notNull())
        {
            bool fs_known = false, lu_known = false;
            const S32 fs_ao = aoItems(fs, fs_known);
            const S32 lu_ao = aoItems(lu, lu_known);
            if (fs_known && lu_known && lu_ao > 0 && fs_ao == 0)
            {
                sShared = &LumenFolders::LUMINOUS_FOLDER;
            }
            LL_INFOS("LumenFolders") << "AO items: " << LumenFolders::FIRESTORM_FOLDER << " " << fs_ao
                                     << (fs_known ? "" : " (not all loaded)") << ", " << LumenFolders::LUMINOUS_FOLDER
                                     << " " << lu_ao << (lu_known ? "" : " (not all loaded)") << LL_ENDL;
        }
        LL_INFOS("LumenFolders") << "Shared folder: " << *sShared << "; Luminous's own: "
                                 << LumenFolders::LUMINOUS_FOLDER << LL_ENDL;
    }
}

const std::string& LumenFolders::sharedRoot()
{
    decide();
    return *sShared;
}

bool LumenFolders::isSystemRoot(const std::string& name)
{
    return name == sharedRoot() || name == LUMINOUS_FOLDER;
}

void LumenFolders::ensureFolder(const std::string& name, std::function<void(const LLUUID&)> done)
{
    if (const LLUUID id = gInventory.findCategoryByName(name); id.notNull())
    {
        if (done)
        {
            done(id);
        }
        return;
    }
    auto& waiting = sMaking[name];
    waiting.push_back(done);
    if (waiting.size() > 1)
    {
        return; // already being made
    }
    LL_INFOS("LumenFolders") << "Making \"" << name << "\"." << LL_ENDL;
    gInventory.createNewCategory(gInventory.getRootFolderID(), LLFolderType::FT_NONE, name,
        [name](const LLUUID& id)
        {
            std::vector<std::function<void(const LLUUID&)>> calls;
            calls.swap(sMaking[name]);
            sMaking.erase(name);
            for (const auto& call : calls)
            {
                if (call && id.notNull())
                {
                    call(id);
                }
            }
        });
}

void LumenFolders::migrateRootFolder()
{
    if (!gInventory.isInventoryUsable())
    {
        // Saying so rather than returning quietly: deciding too early would
        // find neither folder and settle on the wrong one.
        LL_WARNS("LumenFolders") << "Asked to decide the shared folder before inventory was usable; "
                                    "the first lookup after it is will decide instead." << LL_ENDL;
        return;
    }
    decide();
}
