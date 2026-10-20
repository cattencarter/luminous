/**
 * @file luminousoptimise.h
 * @brief Settings that fit the Mac, and the Optimise window.
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
#ifndef LUMINOUS_OPTIMISE_H
#define LUMINOUS_OPTIMISE_H

#include "llfloater.h"

#include <map>
#include <vector>

/**
 * At the very first start (no earlier run in the settings), setUpForThisMac()
 * chooses the live view's resolution and which costly effects start live,
 * from the number of graphics cores and whether the display is Retina or
 * scaled, and says in a message what it chose. Nothing changes for a Mac
 * that has run Luminous before.
 *
 * The Optimise window measures, where the user stands, what each live effect
 * costs and what a lower preview resolution gives, and proposes the fewest
 * changes that reach LuminousOptimiseTargetFPS; nothing changes until Apply,
 * and Put back undoes it. It opens by itself once a session when the live
 * view has stayed under LuminousOptimiseOfferFPS for a minute
 * (LuminousOptimiseOffer), and from the Settings tab.
 */
namespace LuminousOptimise
{
    /// Registers Luminous.SetUpForThisMac and starts the slow-view watch.
    void init();
    /// At start-up, after the window is made: on the very first start, the
    /// settings for this Mac, and a message saying what they are.
    void setUpForThisMac();
    /// Measuring: llappviewer.cpp then leaves out the frame cap and the
    /// background yield, as for the benchmark.
    bool running();
}

class LuminousOptimiseFloater : public LLFloater
{
public:
    LuminousOptimiseFloater(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

private:
    struct Setting
    {
        std::string name;
        LLSD value;           // the value the step measures with
        LLSD before;          // the value it puts back, read as the step begins
    };
    struct Step
    {
        std::string label;              // what the result line names
        std::vector<Setting> settings;  // what the step changes, none for the baseline
        F32 settle = 1.5f;              // seconds before counting frames
        F32 ms = 0.f;                   // the frame time measured
        F32 err = 0.f;                  // how far the frame time measured may be off, from how much it swung
        F32 spread = 0.f;               // how much the half-second frame times swung, as a share of their mean
        bool baseline = false;          // the view as the user has it, nothing changed
        bool resolution = false;        // a lower preview resolution, not an effect
    };
    struct Change
    {
        std::string label;
        std::vector<std::pair<std::string, LLSD>> settings;
        F32 fps_after = 0.f;
    };

    void onMeasure();
    void onApply();
    void onPutBack();
    bool step();
    void beginStep();
    void endStep();
    void stop(const std::string& status);
    void propose();
    void showStatus(const std::string& text);
    bool cameraMoved() const;

    std::vector<Step> mSteps;
    size_t mStep = 0;
    bool mCounting = false;
    U32 mFrames = 0;
    std::vector<F32> mChunks;           // the frame time of each half second counted
    U32 mChunkFrames = 0;
    F32 mChunkStart = 0.f;
    LLTimer mTimer;
    LLVector3 mCameraOrigin;
    LLVector3 mCameraAt;
    std::vector<Change> mChanges;
    std::map<std::string, LLSD> mPutBack;
};

#endif // LUMINOUS_OPTIMISE_H
