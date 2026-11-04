/**
 * @file llrhimetaldebug_metal.mm
 * @brief The Metal debug setting's validation, without the launcher's variables (P3-01c)
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

#include "../llrhimetal-objc.h" // first: it settles BOOL

#include "../test/lltut.h"
#include "tests/wrapllerrs.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

#include <unistd.h>

void runMetalDebugTest()
{
#if LL_RHI_METAL
    using namespace LL::RHI;
    if (@available(macOS 26.0, *))
    {
        for (const char* name : { "MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION" })
        {
            tut::ensure(std::string(name) + " does not come from the launcher", getenv(name) == nullptr);
        }
        setMetalDebug(true);
        // Metal writes its banners on stderr, so stderr goes to a file while the device is made.
        fflush(stderr);
        FILE* captured = tmpfile();
        tut::ensure("a file for stderr", captured != nullptr);
        const int saved = dup(STDERR_FILENO);
        dup2(fileno(captured), STDERR_FILENO);
        Ref<Device> owner;
        std::string warnings;
        BackendChoice choice;
        {
            CaptureLog log(LLError::LEVEL_WARN);
            choice = chooseBackend(BackendType::Metal);
            if (choice.type == BackendType::Metal)
            {
                DeviceCreateInfo info;
                info.backend = choice;
                owner = Device::create(info);
            }
            std::ostringstream caught;
            log.streamto(caught);
            warnings = caught.str();
        }
        fflush(stderr);
        dup2(saved, STDERR_FILENO);
        close(saved);
        std::string text;
        rewind(captured);
        char buffer[4096];
        for (size_t count = 0; (count = fread(buffer, 1, sizeof(buffer), captured)) > 0;)
        {
            text.append(buffer, count);
        }
        fclose(captured);
        if (choice.type != BackendType::Metal)
        {
            LL_INFOS("RHI") << "No Metal device on this Mac: " << choice.reason << LL_ENDL;
            return;
        }
        tut::ensure("the device is made", owner.notNull());
        // The probe of P3-01c S2: setenv before the process's first device works here, where no GL
        // context exists, so the fallback's WARN line must not come.
        const std::string mtlClass =
            NSStringFromClass([static_cast<MetalDevice*>(owner.get())->mtlDevice() class]).UTF8String;
        tut::ensure("Metal API Validation is live: " + mtlClass, mtlClass.find("Debug") != std::string::npos);
        tut::ensure("shader validation is live: " + text, text.find("Metal GPU Validation Enabled") != std::string::npos);
        tut::ensure("no WARN line: " + warnings, warnings.empty());
        tut::ensure("the device has labels and checks", static_cast<MetalDevice*>(owner.get())->debugChecks());
        return;
    }
    LL_INFOS("RHI") << "No Metal device: macOS is older than 26" << LL_ENDL;
#else
    LL_INFOS("RHI") << "No Metal device: this build has no Metal backend" << LL_ENDL;
#endif
}
