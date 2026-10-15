/**
 * @file lumenfolders.h
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
#ifndef LUMEN_FOLDERS_H
#define LUMEN_FOLDERS_H

#include <string>

#include <functional>

class LLUUID;

/**
 * Which top-level inventory folders Luminous uses.
 *
 * Lumen's rules (renaming somebody's `#Firestorm` breaks their AO in
 * Firestorm), with Luminous's own folder, a system folder, so that a user's
 * `#Firestorm` folder is never overwritten:
 *   - the AO, wearable favourites, the particle editor and the inventory
 *     protections use **`#Firestorm` when it exists**, shared with Firestorm,
 *     so switching viewers costs nothing -- never renamed, never moved;
 *   - otherwise they use **`#Luminous`** -- and keep using it if the
 *     account gets a `#Firestorm` later while its AO is set up here;
 *   - Luminous's own LSL bridge and its environments always live in
 *     **`#Luminous`**;
 *   - `#Lumen`, Lumen's folder, is never touched.
 * `#Luminous` is a virtual system folder as `#Firestorm` is: the system
 * folder icon, sorted with the system folders, not to be dragged.
 *
 * ROOT_FIRESTORM_FOLDER (llinventoryfunctions.h) expands to sharedRoot(), so
 * those files keep their upstream text.
 */
namespace LumenFolders
{
    extern const std::string FIRESTORM_FOLDER;   // "#Firestorm"
    extern const std::string LUMEN_FOLDER;       // "#Lumen" -- Lumen's, never touched
    extern const std::string LUMINOUS_FOLDER;    // "#Luminous" -- Luminous's own

    /**
     * `#Firestorm` or `#Luminous`, decided once per session the first time
     * it is asked after inventory is usable. Before that it answers
     * `#Firestorm`, upstream's own value.
     */
    const std::string& sharedRoot();

    /// A folder shown and kept as a system folder: the shared root, and
    /// `#Luminous` whichever the shared root is.
    bool isSystemRoot(const std::string& name);

    /**
     * The top-level folder of this name, made if there is none -- once, for
     * however many ask while it is being made (the AO and the bridge both
     * ask at login, and two requests would make two folders). The callback
     * may be empty.
     */
    void ensureFolder(const std::string& name, std::function<void(const LLUUID&)> done);

    /// Called at STATE_INVENTORY_CALLBACKS: makes the decision above.
    void migrateRootFolder();
}

#endif // LUMEN_FOLDERS_H
