/**
 * @file llrhimetaldebug_test.cpp
 * @brief The Metal debug setting turns validation on by itself (P3-01c)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Viewer Lab contributors
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "../test/lltut.h"

// In llrhimetaldebug_metal.mm.
void runMetalDebugTest();

// No MTL_* from the launcher here (LL_ADD_INTEGRATION_TEST in llrender/CMakeLists.txt): the Metal
// debug setting must turn Metal API Validation and shader validation on itself (C14).

namespace tut
{
    struct rhimetaldebug_data
    {
    };
    typedef test_group<rhimetaldebug_data> rhimetaldebug_group;
    typedef rhimetaldebug_group::object object;
    rhimetaldebug_group rhimetaldebug_test_group("Metal debug setting");

    template<> template<>
    void object::test<1>()
    {
        set_test_name("setMetalDebug(true) before the first Metal device gives Metal API Validation and shader validation");
        runMetalDebugTest();
    }
}
