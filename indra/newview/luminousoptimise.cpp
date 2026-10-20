/**
 * @file luminousoptimise.cpp
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

#include "llviewerprecompiledheaders.h"

#include "luminousoptimise.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "lltexteditor.h"
#include "lluictrl.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "luminousbenchmark.h"
#include "luminousphotomode.h"

#include <algorithm>
#include <deque>

#if LL_DARWIN
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#endif

extern bool gHiDPISupport;

namespace
{
    const F32 MEASURE = 3.f; // seconds of frames counted for each step
    const F32 MEASURE_LONGEST = 9.f; // a step whose frame time keeps swinging is counted up to this long
    const F32 CHUNK = 0.5f; // the count is also kept per half second, to see how much it swings
    const F32 STEADY = 0.08f; // half-second frame times within this share of their mean count as steady
    const F32 TOO_UNSTEADY = 0.3f; // the view as it is swinging more than this share is not judged
    const F32 SWAY_METRES = 2.f; // the camera may sway this far from where the measure began
    const F32 SWAY_COS = 0.9659f; // and turn up to 15 degrees

    bool sMeasuring = false;
    bool sOfferedThisSession = false;

    // The costly effects that can be kept for renders, by the setting that
    // switches each on and the one that keeps it for renders. Cheap ones
    // (vignette, grain, aberration, distortion) are left out: they cost too
    // little to be worth the half minute.
    struct Effect
    {
        const char* label;
        const char* on;
        const char* render_only;
    };
    const Effect EFFECTS[] =
    {
        { "Volumetric light",   "LuminousPhotoVolumetricLight", "LuminousPhotoVolumetricLightRenderOnly" },
        { "Projector beams",    "LuminousPhotoProjectorBeams",  "LuminousPhotoProjectorBeamsRenderOnly" },
        { "GTAO",               "LuminousPhotoAO",              "LuminousPhotoAORenderOnly" },
        { "Soft shadows",       "LuminousPhotoSoftShadows",     "LuminousPhotoSoftShadowsRenderOnly" },
        { "Contact shadows",    "LuminousPhotoContactShadows",  "LuminousPhotoContactShadowsRenderOnly" },
        { "Depth of field",     "RenderDepthOfField",           "LuminousPhotoDepthOfFieldRenderOnly" },
        { "Enhance (depth of field)", "LuminousPhotoDoF",       "LuminousPhotoDoFRenderOnly" },
        { "Bloom",              "LuminousPhotoBloom",           "LuminousPhotoBloomRenderOnly" },
        { "Sun shafts",         "LuminousPhotoSunShafts",       "LuminousPhotoSunShaftsRenderOnly" },
        { "Lens flare",         "LuminousPhotoLensFlare",       "LuminousPhotoLensFlareRenderOnly" },
        { "Clarity",            "LuminousPhotoClarity",         "LuminousPhotoClarityRenderOnly" },
        { "Halation",           "LuminousPhotoHalation",        "LuminousPhotoHalationRenderOnly" },
        { "Anamorphic streaks", "LuminousPhotoAnamorphic",      "LuminousPhotoAnamorphicRenderOnly" },
    };

    // On: a switch that is on, or an amount that is not 0.
    bool switchedOn(const char* setting)
    {
        LLControlVariable* control = gSavedSettings.getControl(setting);
        if (!control)
        {
            return false;
        }
        const LLSD value = control->getValue();
        return control->type() == TYPE_BOOLEAN ? value.asBoolean() : std::fabs(value.asReal()) > 0.005;
    }

    // Drawn in the live view: on, and not kept for renders.
    bool live(const Effect& effect)
    {
        if (!switchedOn(effect.on) || gSavedSettings.getBOOL(effect.render_only))
        {
            return false;
        }
        // Enhance is depth of field's own blur: live only while depth of
        // field itself is.
        if (std::string(effect.on) == "LuminousPhotoDoF")
        {
            return switchedOn("RenderDepthOfField") && !gSavedSettings.getBOOL("LuminousPhotoDepthOfFieldRenderOnly");
        }
        return true;
    }

    std::string scaleName(F32 scale)
    {
        return scale > 0.7f ? "three quarters" : "half";
    }

    // --- the Mac ---------------------------------------------------------

    std::string chipName()
    {
#if LL_DARWIN
        char name[128] = {};
        size_t size = sizeof(name) - 1;
        if (sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) == 0)
        {
            return std::string(name);
        }
#endif
        return std::string();
    }

    // Apple's GPU driver states its core count in the I/O registry.
    S32 graphicsCores()
    {
        S32 cores = 0;
#if LL_DARWIN
        io_iterator_t services = 0;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"), &services) != KERN_SUCCESS)
        {
            return 0;
        }
        io_object_t service = 0;
        while (cores == 0 && (service = IOIteratorNext(services)) != 0)
        {
            CFTypeRef value = IORegistryEntryCreateCFProperty(service, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0);
            if (value)
            {
                if (CFGetTypeID(value) == CFNumberGetTypeID())
                {
                    CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &cores);
                }
                CFRelease(value);
            }
            IOObjectRelease(service);
        }
        IOObjectRelease(services);
#endif
        return cores;
    }

    // Base chips have up to 10 graphics cores, Pro up to 20, Max and Ultra
    // more. The count, not the name, so a new chip falls into place.
    enum class Tier { Base, Pro, Max };

    Tier tierOf(S32 cores, const std::string& chip)
    {
        if (cores <= 0)
        {
            if (chip.find("Max") != std::string::npos || chip.find("Ultra") != std::string::npos)
            {
                return Tier::Max;
            }
            return chip.find("Pro") != std::string::npos ? Tier::Pro : Tier::Base;
        }
        return cores <= 10 ? Tier::Base : (cores <= 20 ? Tier::Pro : Tier::Max);
    }

    void setUp()
    {
        const std::string chip = chipName();
        const S32 cores = graphicsCores();
        const Tier tier = tierOf(cores, chip);
        const F32 backing = gViewerWindow && gViewerWindow->getWindow() ? gViewerWindow->getWindow()->getSystemUISize() : 1.f;
        // A Retina or scaled display draws backing-scale squared the pixels.
        const bool retina = gHiDPISupport && backing > 1.01f;

        std::vector<std::string> chosen;
        F32 live_scale = 1.f;
        if (retina && tier == Tier::Base)
        {
            live_scale = 0.f; // automatic: half each way on this display
            chosen.push_back("the live view at half resolution each way on this display");
        }
        else if (retina && tier == Tier::Pro)
        {
            live_scale = 0.75f;
            chosen.push_back("the live view at three quarters resolution on this display");
        }
        else
        {
            chosen.push_back("the live view at full resolution");
        }
        gSavedSettings.setF32("LuminousPhotoLiveScale", live_scale);
        if (tier == Tier::Base)
        {
            gSavedSettings.setBOOL("LuminousPhotoVolumetricLightRenderOnly", true);
            gSavedSettings.setBOOL("LuminousPhotoProjectorBeamsRenderOnly", true);
            chosen.push_back("volumetric light and projector beams in renders only: they show in every render, "
                             "not while you set up the shot");
        }
        else
        {
            chosen.push_back("volumetric light, projector beams and GTAO live, at low quality");
        }

        std::string mac = chip.empty() ? std::string("this Mac") : chip;
        if (cores > 0)
        {
            mac += llformat(", %d graphics cores", cores);
        }
        if (retina)
        {
            mac += ", a Retina or scaled display";
        }
        gSavedSettings.setString("LuminousSetUpFor", mac);
        LL_INFOS("LuminousOptimise") << "Set up for " << mac << ": live scale " << live_scale
            << (tier == Tier::Base ? ", volumetric light and projector beams in renders only" : "") << LL_ENDL;

        std::string message = "Luminous has set itself up for this Mac (" + mac + "):\n";
        for (const std::string& line : chosen)
        {
            message += "\n- " + line;
        }
        message += "\n\nThis only makes the live view, where you set up the shot, quicker. Your finished pictures "
                   "are not affected: renders always try for full resolution, and draw every effect at full quality."
                   "\n\nChange any of it in the sidebar; Settings > Optimise my settings measures what this Mac "
                   "can do where you are.";
        LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", message));
    }

    // --- the slow-view watch -----------------------------------------------

    // Once a session, when the live view has stayed slow for a minute while
    // the user is in the world with the window in front -- not at login, not
    // just after a teleport or a region crossing, not across a render's
    // pause -- the Optimise window opens with how slow it has been.
    struct Watch
    {
        bool started = false;
        F64 quiet_until = 0.0;
        U64 region = 0;
        LLTimer second;
        U32 frames = 0;
        F32 longest = 0.f;
        std::deque<F32> fps;
    };

    bool watch(Watch& w)
    {
        if (sOfferedThisSession)
        {
            return true; // once a session: the watch ends
        }
        if (LLStartUp::getStartupState() < STATE_STARTED)
        {
            return false;
        }
        const F64 now = LLTimer::getTotalSeconds();
        if (!w.started)
        {
            w.started = true;
            w.quiet_until = now + 120.0; // the region loads after login
            w.second.reset();
            w.frames = LLFrameTimer::getFrameCount();
        }
        w.longest = llmax(w.longest, (F32)gFrameIntervalSeconds.value());
        const F32 elapsed = w.second.getElapsedTimeF32();
        if (elapsed < 1.f)
        {
            return false;
        }
        const U32 frames = LLFrameTimer::getFrameCount() - w.frames;
        const F32 longest = w.longest;
        w.second.reset();
        w.frames = LLFrameTimer::getFrameCount();
        w.longest = 0.f;

        LLViewerRegion* region = gAgent.getRegion();
        const U64 handle = region ? region->getHandle() : 0;
        if (handle != w.region || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || gTeleportDisplay)
        {
            w.region = handle;
            w.quiet_until = now + 60.0; // a new region loads
            w.fps.clear();
            return false;
        }
        static LLCachedControl<bool> offer(gSavedSettings, "LuminousOptimiseOffer", true);
        const bool counted = offer() && region && now > w.quiet_until && gFocusMgr.getAppHasFocus()
            && gViewerWindow && !gViewerWindow->getWindow()->getMinimized()
            && !sMeasuring && !LuminousBenchmark::running()
            && longest < 1.f; // a render or a load stalls the frame: not the live view's pace
        if (!counted)
        {
            return false;
        }
        w.fps.push_back((F32)frames / elapsed);
        if (w.fps.size() > 60)
        {
            w.fps.pop_front();
        }
        if (w.fps.size() < 60)
        {
            return false;
        }
        std::vector<F32> sorted(w.fps.begin(), w.fps.end());
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const F32 median = sorted[sorted.size() / 2];
        static LLCachedControl<F32> slow(gSavedSettings, "LuminousOptimiseOfferFPS", 15.f);
        if (median >= slow())
        {
            return false;
        }
        sOfferedThisSession = true;
        LL_INFOS("LuminousOptimise") << "The live view has run at " << median << " fps for a minute: offering to optimise" << LL_ENDL;
        LLFloaterReg::showInstance("luminous_optimise", LLSD().with("slow_fps", median));
        return true;
    }
}

namespace LuminousOptimise
{
    bool running()
    {
        return sMeasuring;
    }

    void setUpForThisMac()
    {
        // Only a Mac that has never run Luminous: LastRunVersion is still
        // its default then. Anyone else keeps what they have.
        if (!gLastRunVersion.empty() && gLastRunVersion != "0.0.0")
        {
            return;
        }
        setUp();
    }

    void init()
    {
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.SetUpForThisMac",
            [](LLUICtrl*, const LLSD&) { setUp(); });
        auto w = std::make_shared<Watch>();
        doOnIdleRepeating([w]() { return watch(*w); });
    }
}

// --- the window -------------------------------------------------------------

LuminousOptimiseFloater::LuminousOptimiseFloater(const LLSD& key)
    : LLFloater(key)
{
}

bool LuminousOptimiseFloater::postBuild()
{
    getChild<LLButton>("measure_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMeasure(); });
    getChild<LLButton>("apply_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onApply(); });
    getChild<LLButton>("put_back_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPutBack(); });
    getChild<LLButton>("close_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    getChildView("apply_btn")->setEnabled(false);
    getChildView("put_back_btn")->setEnabled(false);
    return true;
}

void LuminousOptimiseFloater::onOpen(const LLSD& key)
{
    // Opened by hand, it is not offered again this session.
    sOfferedThisSession = true;
    const S32 target = ll_round(gSavedSettings.getF32("LuminousOptimiseTargetFPS"));
    std::string intro;
    if (key.has("slow_fps"))
    {
        intro = llformat("The live view has run at about %d fps here for the last minute. Luminous can measure what each "
                         "live effect costs where you are, and suggest what to change to reach %d fps.",
                         llmax(1, ll_round((F32)key["slow_fps"].asReal())), target);
    }
    else
    {
        intro = llformat("Luminous measures what each live effect costs where you are, and suggests what to change to "
                         "reach %d fps in the live view.", target);
    }
    getChild<LLUICtrl>("intro")->setValue(intro);
    if (!sMeasuring)
    {
        showStatus("");
    }
}

void LuminousOptimiseFloater::onClose(bool app_quitting)
{
    if (sMeasuring)
    {
        stop("");
    }
}

void LuminousOptimiseFloater::showStatus(const std::string& text)
{
    getChild<LLUICtrl>("status")->setValue(text);
}

// The camera may sway about where it stood when the measure began -- on a
// boat at anchor, say -- but not travel or turn away.
bool LuminousOptimiseFloater::cameraMoved() const
{
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    return (camera->getOrigin() - mCameraOrigin).length() > SWAY_METRES || camera->getAtAxis() * mCameraAt < SWAY_COS;
}

void LuminousOptimiseFloater::onMeasure()
{
    if (sMeasuring || LuminousBenchmark::running())
    {
        return;
    }
    if (LLStartUp::getStartupState() < STATE_STARTED || !gAgent.getRegion())
    {
        showStatus("Log in first: Luminous measures where you are.");
        return;
    }

    mSteps.clear();
    mChanges.clear();
    // The view as it is is measured again after every third change, so a
    // view that drifts -- a region still loading -- is measured against
    // where it has drifted to.
    Step baseline;
    baseline.label = "As you have it";
    baseline.settle = 2.f;
    baseline.baseline = true;
    mSteps.push_back(baseline);
    S32 since_baseline = 0;
    for (const Effect& effect : EFFECTS)
    {
        if (live(effect))
        {
            Step s;
            s.label = effect.label;
            s.settings.push_back({ effect.render_only, true, LLSD() });
            mSteps.push_back(s);
            if (++since_baseline == 3)
            {
                mSteps.push_back(baseline);
                since_baseline = 0;
            }
        }
    }
    // A lower preview resolution: the next step down from what is set.
    // FXAA keeps the live view at full resolution, so with FXAA the step is
    // SMAA and three quarters together, proposed as one change; the
    // shaders are made again for it, so it settles longer.
    if (gSavedSettings.getU32("RenderFSAAType") == 1)
    {
        Step s;
        s.label = "Anti-aliasing SMAA and preview resolution three quarters";
        s.settings.push_back({ "RenderFSAAType", LLSD::Integer(2), LLSD() });
        s.settings.push_back({ "LuminousPhotoLiveScale", 0.75, LLSD() });
        s.settle = 5.f;
        s.resolution = true;
        mSteps.push_back(s);
    }
    else
    {
        const F32 chosen = LuminousPhotoMode::chosenLiveScale();
        const F32 lower = chosen > 0.99f ? 0.75f : (chosen > 0.74f ? 0.5f : 0.f);
        if (lower > 0.f)
        {
            Step s;
            s.label = "Preview resolution " + scaleName(lower);
            s.settings.push_back({ "LuminousPhotoLiveScale", lower, LLSD() });
            s.settle = 3.f; // the targets are made again at the new size
            s.resolution = true;
            mSteps.push_back(s);
        }
    }
    if (!mSteps.back().baseline)
    {
        mSteps.push_back(baseline);
    }

    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    mCameraOrigin = camera->getOrigin();
    mCameraAt = camera->getAtAxis();
    sMeasuring = true;
    getChildView("measure_btn")->setEnabled(false);
    getChildView("apply_btn")->setEnabled(false);
    getChild<LLUICtrl>("results")->setValue("");
    mStep = 0;
    beginStep();
    LL_INFOS("LuminousOptimise") << "Measuring " << mSteps.size() << " steps" << LL_ENDL;

    LLHandle<LLFloater> handle = getHandle();
    doOnIdleRepeating([handle]()
    {
        LuminousOptimiseFloater* floater = static_cast<LuminousOptimiseFloater*>(handle.get());
        return !floater || floater->step();
    });
}

void LuminousOptimiseFloater::beginStep()
{
    Step& s = mSteps[mStep];
    for (Setting& setting : s.settings)
    {
        LLControlVariable* control = gSavedSettings.getControl(setting.name);
        setting.before = control->getValue();
        control->setValue(setting.value);
    }
    mCounting = false;
    mTimer.reset();
    std::string what = "the live view as it is";
    if (s.resolution)
    {
        what = s.label;
    }
    else if (!s.settings.empty())
    {
        what = s.label + " in renders only";
    }
    const S32 seconds = ll_round((F32)(mSteps.size() - mStep) * (MEASURE + 1.8f));
    showStatus(llformat("Measuring %d of %d: %s. Keep the camera where it is -- about %d s.", (S32)mStep + 1,
                        (S32)mSteps.size(), what.c_str(), seconds));
}

void LuminousOptimiseFloater::endStep()
{
    Step& s = mSteps[mStep];
    for (auto setting = s.settings.rbegin(); setting != s.settings.rend(); ++setting)
    {
        gSavedSettings.getControl(setting->name)->setValue(setting->before);
    }
}

void LuminousOptimiseFloater::stop(const std::string& status)
{
    if (mStep < mSteps.size())
    {
        endStep();
    }
    mStep = mSteps.size();
    sMeasuring = false;
    getChildView("measure_btn")->setEnabled(true);
    showStatus(status);
    LL_INFOS("LuminousOptimise") << "Stopped: " << status << LL_ENDL;
}

bool LuminousOptimiseFloater::step()
{
    if (!sMeasuring || mStep >= mSteps.size())
    {
        return true;
    }
    if (!gAgent.getRegion() || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        stop("Stopped: the region changed. Measure again once it has loaded.");
        return true;
    }
    if (cameraMoved())
    {
        stop("Stopped: the camera moved too far. A little sway is fine, as on a boat at anchor; keep it near "
             "one place and measure again.");
        return true;
    }
    Step& s = mSteps[mStep];
    const F32 t = mTimer.getElapsedTimeF32();
    if (!mCounting)
    {
        if (t >= s.settle)
        {
            mCounting = true;
            mFrames = LLFrameTimer::getFrameCount();
            mChunkFrames = mFrames;
            mChunkStart = 0.f;
            mChunks.clear();
            mTimer.reset();
        }
        return false;
    }
    // The frame time of each half second, to see how much it swings
    if (t - mChunkStart >= CHUNK)
    {
        const U32 now = LLFrameTimer::getFrameCount();
        mChunks.push_back(1000.f * (t - mChunkStart) / (F32)llmax(1U, now - mChunkFrames));
        mChunkFrames = now;
        mChunkStart = t;
    }
    if (t < MEASURE)
    {
        return false;
    }
    F32 mean = 0.f;
    for (F32 ms : mChunks)
    {
        mean += ms;
    }
    mean /= (F32)llmax((size_t)1, mChunks.size());
    F32 squares = 0.f;
    for (F32 ms : mChunks)
    {
        squares += (ms - mean) * (ms - mean);
    }
    const F32 deviation = mChunks.size() > 1 ? sqrtf(squares / (F32)(mChunks.size() - 1)) : 0.f;
    const F32 spread = deviation / llmax(mean, 0.1f);
    // A frame time that swings is counted longer, so that the swing
    // averages out
    if (spread > STEADY && t < MEASURE_LONGEST)
    {
        return false;
    }
    const U32 frames = llmax(1U, LLFrameTimer::getFrameCount() - mFrames);
    s.ms = 1000.f * t / (F32)frames;
    // Half seconds next to each other swing together, so the uncertainty
    // takes one independent sample for every two seconds counted
    s.err = deviation / sqrtf(llmax(1.f, t / 2.f));
    s.spread = spread;
    LL_INFOS("LuminousOptimise") << s.label << ": " << s.ms << " ms, swinging " << ll_round(spread * 100.f)
                                 << " %, counted " << t << " s" << LL_ENDL;
    endStep();
    if (++mStep < mSteps.size())
    {
        beginStep();
        return false;
    }
    sMeasuring = false;
    getChildView("measure_btn")->setEnabled(true);
    propose();
    return true;
}

void LuminousOptimiseFloater::propose()
{
    // The view as it is, measured between the changes: its mean is the
    // frame now, and each change is measured against it as it stood then,
    // interpolated between the measurements before and after.
    F32 base = 0.f;
    F32 lowest = 1e9f;
    F32 highest = 0.f;
    S32 baselines = 0;
    for (const Step& s : mSteps)
    {
        if (s.baseline)
        {
            base += s.ms;
            lowest = llmin(lowest, s.ms);
            highest = llmax(highest, s.ms);
            ++baselines;
        }
    }
    base /= (F32)llmax(baselines, 1);
    const F32 target_fps = llmax(1.f, gSavedSettings.getF32("LuminousOptimiseTargetFPS"));
    const F32 target_ms = 1000.f / target_fps;
    auto fps = [](F32 ms) { return ll_round(1000.f / llmax(ms, 1.f)); };
    // How much the frame time swung within a step: the view as it is, and any step
    F32 swing = 0.f;
    F32 any_swing = 0.f;
    for (const Step& s : mSteps)
    {
        any_swing = llmax(any_swing, s.spread);
        if (s.baseline)
        {
            swing = llmax(swing, s.spread);
        }
    }
    if (swing > TOO_UNSTEADY)
    {
        const std::string text = llformat("The frame rate swung too much while it measured -- by about %d%% within a "
                                          "few seconds -- as it does when much in view moves, a boat in waves or a "
                                          "crowd. Pause the scene (the pause button beside Reset) and measure again, "
                                          "or measure somewhere steadier.",
                                          ll_round(swing * 100.f));
        getChild<LLUICtrl>("results")->setValue(text);
        showStatus("Not measured: the frame rate swung too much.");
        LL_INFOS("LuminousOptimise") << text << LL_ENDL;
        return;
    }
    if (highest - lowest > base * 0.25f)
    {
        const std::string text = llformat("The live view kept changing while it measured, from %d to %d fps -- most "
                                          "likely the region is still loading, or much in view moves. Measure again "
                                          "once it has settled.",
                                          fps(highest), fps(lowest));
        getChild<LLUICtrl>("results")->setValue(text);
        showStatus("Not measured: the view kept changing.");
        LL_INFOS("LuminousOptimise") << text << LL_ENDL;
        return;
    }
    // A saving must stand above a twentieth of the frame, and above a
    // millisecond.
    const F32 noise = llmax(1.f, base * 0.05f);

    struct Saving { const Step* step; F32 ms; };
    std::vector<Saving> effects;
    const Step* scale = nullptr;
    F32 scale_ratio = 1.f;
    std::string costs;
    for (size_t i = 0; i < mSteps.size(); ++i)
    {
        const Step& s = mSteps[i];
        if (s.baseline)
        {
            continue;
        }
        size_t before = i;
        size_t after = i;
        while (before > 0 && !mSteps[before].baseline)
        {
            --before;
        }
        while (after + 1 < mSteps.size() && !mSteps[after].baseline)
        {
            ++after;
        }
        const F32 f = (F32)(i - before) / (F32)llmax((size_t)1, after - before);
        const F32 then = mSteps[before].ms + (mSteps[after].ms - mSteps[before].ms) * f;
        const F32 saved = then - s.ms;
        // A saving must also stand above twice what the swing leaves
        // uncertain, in the step and in the view as it is round it
        const F32 then_err = llmax(mSteps[before].err, mSteps[after].err);
        const F32 step_noise = llmax(noise, 2.f * sqrtf(s.err * s.err + then_err * then_err));
        if (s.resolution)
        {
            if (saved > step_noise)
            {
                scale = &s;
                scale_ratio = s.ms / llmax(then, 1.f);
            }
            continue;
        }
        costs += llformat("\n  %s: %.1f ms", s.label.c_str(), llmax(saved, 0.f));
        if (saved > step_noise)
        {
            effects.push_back({ &s, saved });
        }
        else if (saved > noise)
        {
            costs += " (no more than the swing here)";
        }
    }
    std::sort(effects.begin(), effects.end(), [](const Saving& a, const Saving& b) { return a.ms > b.ms; });

    std::string text = llformat("Now: about %d fps (%.1f ms a frame). Aim: %d fps.", fps(base), base, ll_round(target_fps));
    F32 predicted = base;
    if (base <= target_ms * 1.05f)
    {
        text += "\n\nThe live view already reaches it here: nothing needs to change.";
    }
    else
    {
        for (const Saving& saving : effects)
        {
            if (predicted <= target_ms)
            {
                break;
            }
            const S32 from = fps(predicted);
            predicted = llmax(predicted - saving.ms, 1.f);
            mChanges.push_back({ saving.step->label + " in renders only",
                                 { { saving.step->settings.front().name, saving.step->settings.front().value } },
                                 1000.f / predicted });
            text += llformat("\n- %s in renders only: %d -> %d fps", saving.step->label.c_str(), from, fps(predicted));
        }
        if (predicted > target_ms && scale && scale_ratio < 0.97f)
        {
            const S32 from = fps(predicted);
            predicted *= scale_ratio;
            Change change{ scale->label, {}, 1000.f / predicted };
            for (const Setting& setting : scale->settings)
            {
                change.settings.push_back({ setting.name, setting.value });
            }
            mChanges.push_back(change);
            text += llformat("\n- %s: %d -> %d fps (renders keep full resolution)", scale->label.c_str(), from, fps(predicted));
        }
        if (mChanges.empty())
        {
            text += "\n\nNo live effect costs enough here to make a difference, and a lower preview resolution "
                    "does not help: the rest of the frame is the region itself -- its objects and avatars.";
        }
        else if (predicted > target_ms)
        {
            text += llformat("\n\nThat comes to about %d fps; the rest of the frame is the region itself -- its "
                             "objects and avatars.", fps(predicted));
        }
        if (!mChanges.empty())
        {
            text += "\n\nApply makes these changes; Put back undoes them.";
        }
    }
    if (!costs.empty())
    {
        text += "\n\nWhat each live effect costs here:" + costs;
    }
    if (any_swing > STEADY)
    {
        text += "\n\nThe frame rate swung while it measured, so the steps that swung were counted longer, and "
                "only savings larger than the swing are suggested.";
    }
    getChild<LLUICtrl>("results")->setValue(text);
    getChildView("apply_btn")->setEnabled(!mChanges.empty());
    showStatus("Measured.");
    LL_INFOS("LuminousOptimise") << text << LL_ENDL;
}

void LuminousOptimiseFloater::onApply()
{
    for (const Change& change : mChanges)
    {
        for (const auto& [name, value] : change.settings)
        {
            LLControlVariable* control = gSavedSettings.getControl(name);
            if (!control)
            {
                continue;
            }
            if (!mPutBack.count(name))
            {
                mPutBack[name] = control->getValue();
            }
            control->setValue(value);
        }
    }
    if (!mChanges.empty())
    {
        showStatus("Applied. Put back undoes it.");
        getChildView("apply_btn")->setEnabled(false);
        getChildView("put_back_btn")->setEnabled(true);
    }
}

void LuminousOptimiseFloater::onPutBack()
{
    for (const auto& [setting, value] : mPutBack)
    {
        if (LLControlVariable* control = gSavedSettings.getControl(setting))
        {
            control->setValue(value);
        }
    }
    mPutBack.clear();
    getChildView("put_back_btn")->setEnabled(false);
    getChildView("apply_btn")->setEnabled(!mChanges.empty());
    showStatus("Put back as it was.");
}
