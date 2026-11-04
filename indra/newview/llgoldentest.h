/**
 * @file llgoldentest.h
 * @brief Test hooks for golden-image tests of the viewer, behind --golden (level 2)
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

#ifndef LL_LLGOLDENTEST_H
#define LL_LLGOLDENTEST_H

// [Viewer Lab] Golden-image level 2 (tests/golden/README.md): with --golden <config>, the viewer
// sets a fixed scene up once it is in-world, waits a fixed number of frames, reads the presented
// frame back and quits. Without the flag nothing here does anything.

namespace LLGoldenTest
{
    /// Called once at startup, before the render pipeline is made: reads the config, sets the
    /// graphics level, GPU class and settings it locks, and seeds the main thread's random
    /// generator.
    void init();

    /// Called right before the pipeline makes its noise textures from the random generator, at
    /// startup and again whenever the shaders are reloaded: seeds it, so they are the same in
    /// every run.
    void beforeNoise();

    /// Called in every main-loop idle right before the window's UI update (LLViewerWindow::updateUI), which handles
    /// the mouse: pins the mouse at the drag's point from the drag's start (drag), and saves the config's mouse
    /// point from the first in-world frame (cursor). Nothing otherwise.
    void beforeUI();

    /// Called in every main-loop idle right after the window's UI update: with the config's mouse point and the
    /// raycast debug display, casts updateUI's debug ray again, so the tool's hover (with the system's mouse inside the
    /// window) does not move its hit point (cursor). Nothing otherwise.
    void afterUI();

    /// Called right before each swap of the window's buffers, while the back buffer holds the
    /// frame that is about to be presented.
    void beforeSwap();

    /// Called right before display_startup swaps the window's buffers, in the frames before login (the login
    /// screen's among them), while the back buffer holds the frame that is about to be presented.
    void beforeStartupSwap();
}

#endif // LL_LLGOLDENTEST_H
