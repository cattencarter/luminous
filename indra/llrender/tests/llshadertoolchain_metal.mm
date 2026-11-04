/**
 * @file llshadertoolchain_metal.mm
 * @brief Metal compiles the shader tool chain's MSL, for llshadertoolchain_test.cpp
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

#include "../llrhimetal-objc.h" // first: it settles BOOL; the math constants (B6)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <string>

std::string compileWithMetal(const std::string& msl)
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
        {
            return "no Metal device";
        }
        MTLCompileOptions* options = [MTLCompileOptions new];
        if (@available(macOS 26.0, *))
        {
            options.languageVersion = MTLLanguageVersion4_0;
#if LL_RHI_METAL
            options.mathMode = LL::RHI::METAL_MATH_MODE; // as the backend compiles (B6)
            options.mathFloatingPointFunctions = LL::RHI::METAL_MATH_FUNCTIONS;
#endif
        }
        else
        {
            return "MSL 4.0 needs macOS 26";
        }
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()]
                                                      options:options
                                                        error:&error];
        if (!library)
        {
            return error ? std::string(error.localizedDescription.UTF8String) : std::string("no library");
        }
        return {};
    }
}
