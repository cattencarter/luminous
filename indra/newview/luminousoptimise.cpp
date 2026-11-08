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
#include "llfeaturemanager.h"
#include "llfloaterreg.h"
#include "llrhi.h"
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
#include "luminousmetal.h"
#include "luminousphotomode.h"

#include <algorithm>
#include <deque>

#if LL_DARWIN
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
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
    const F32 SCALER_SETTLE = 4.f; // MetalFX upscaling switched on: its scaler made, the targets at the new size and
                                   // its history filled
    const F32 SCALER_WAIT = 6.f; // and how much longer its first scaled frame may take before it counts as not scaling

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

    F32 targetMs()
    {
        return 1000.f / llmax(1.f, gSavedSettings.getF32("LuminousOptimiseTargetFPS"));
    }

    // MetalFX upscaling is measured first where it could be switched on and could make the live view quicker: on
    // Metal with MetalFX's temporal scaler, off (as Luminous ships), Half within its range, not refused at the
    // window's size, and the live view larger than Half now. At Standard on a Retina display the live view already
    // draws Half's pixels, so MetalFX could only add its own cost.
    bool metalFXToMeasure()
    {
        const F32 most = LuminousMetal::temporalMaxScale();
        return LuminousMetal::temporalSupported() && !gSavedSettings.getBOOL("LuminousMetalFXTemporal")
               && (most <= 0.f || most >= 2.f) && !LuminousMetal::temporalRefused()
               && LuminousPhotoMode::chosenLiveScale() > 0.51f;
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

    // The pixels the main display draws in its current mode: a scaled Retina mode's backing pixels with
    // HiDPI on, its points without. 0 x 0 when macOS does not say.
    void displayPixels(S32& width, S32& height)
    {
        width = height = 0;
#if LL_DARWIN
        if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGMainDisplayID()))
        {
            width = (S32)(gHiDPISupport ? CGDisplayModeGetPixelWidth(mode) : CGDisplayModeGetWidth(mode));
            height = (S32)(gHiDPISupport ? CGDisplayModeGetPixelHeight(mode) : CGDisplayModeGetHeight(mode));
            CGDisplayModeRelease(mode);
        }
#endif
    }

    // The scale Preview resolution Standard comes to on the main display: one pixel per point, so a half on a
    // Retina display with HiDPI on, and 1 on any other.
    F32 standardScaleOfDisplay()
    {
        F32 scale = 1.f;
#if LL_DARWIN
        if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGMainDisplayID()))
        {
            const size_t points = CGDisplayModeGetWidth(mode);
            const size_t pixels = CGDisplayModeGetPixelWidth(mode);
            if (gHiDPISupport && points > 0 && pixels > points)
            {
                scale = (F32)points / (F32)pixels;
            }
            CGDisplayModeRelease(mode);
        }
#endif
        return scale;
    }

    // Base chips have up to 10 graphics cores, Pro up to 20, Max up to 40 and
    // Ultra more. The count, not the name, so a new chip falls into place.
    enum class Tier { Base, Pro, Max, Ultra };

    Tier tierOf(S32 cores, const std::string& chip)
    {
        if (cores <= 0)
        {
            if (chip.find("Ultra") != std::string::npos)
            {
                return Tier::Ultra;
            }
            if (chip.find("Max") != std::string::npos)
            {
                return Tier::Max;
            }
            return chip.find("Pro") != std::string::npos ? Tier::Pro : Tier::Base;
        }
        return cores <= 10 ? Tier::Base : (cores <= 20 ? Tier::Pro : (cores <= 40 ? Tier::Max : Tier::Ultra));
    }

    // Second Life's graphics level for the tier. Second Life caps every Apple GPU at Mid-High
    // (LLFeatureManager::init: "high throughput, low capability"), which leaves shadows, transparent
    // water and ambient occlusion off, and with them soft shadows, volumetric light and GTAO. Metal
    // draws faster than OpenGL, so a Pro or Max chip gets a level more on Metal.
    U32 levelFor(Tier tier, bool metal)
    {
        switch (tier)
        {
        case Tier::Ultra: return 6;                // Ultra
        case Tier::Max:   return metal ? 6 : 5;    // Ultra, or High-Ultra (projectors' shadows too)
        case Tier::Pro:   return metal ? 5 : 4;    // High-Ultra, or High
        default:          return 4;                // High: sun shadows, transparent water, AO
        }
    }

    const char* levelName(U32 level)
    {
        return level >= 6 ? "Ultra" : (level == 5 ? "High-Ultra" : "High");
    }

    // The live view a tier draws well at its level: about 1920 x 1080 on a base chip, 2560 x 1440 on a Pro,
    // 3840 x 2160 on a Max; an Ultra draws all of them (0).
    F64 pixelBudget(Tier tier)
    {
        switch (tier)
        {
        case Tier::Base: return 1920.0 * 1080.0;
        case Tier::Pro:  return 2560.0 * 1440.0;
        case Tier::Max:  return 3840.0 * 2160.0;
        default:         return 0.0;
        }
    }

    // The main display's pixels (displayPixels), or the window's where macOS does not say.
    F64 displayPixelCount(S32& display_x, S32& display_y)
    {
        displayPixels(display_x, display_y);
        if (display_x <= 0 || display_y <= 0)
        {
            display_x = gViewerWindow ? gViewerWindow->getWindowWidthRaw() : 0;
            display_y = gViewerWindow ? gViewerWindow->getWindowHeightRaw() : 0;
        }
        return (F64)display_x * (F64)display_y;
    }

    // The live view made smaller where the display has more pixels than the tier's budget (15% over is let
    // through, for 1920 x 1200 and the like): three quarters each way, or half; else full.
    F32 budgetLiveScale(Tier tier, F64 pixels)
    {
        const F64 budget = pixelBudget(tier);
        if (budget > 0.0 && pixels > budget * 1.15)
        {
            return std::sqrt(budget / pixels) >= 0.75 ? 0.75f : 0.5f;
        }
        return 1.f;
    }

    // The Preview resolution the set-up chooses: Standard (0, one pixel per point, which follows the screen) unless
    // the tier's budget asks for fewer pixels than that, as on a large display at 1x.
    F32 setUpLiveScale(Tier tier, F64 pixels)
    {
        const F32 budget = budgetLiveScale(tier, pixels);
        return budget < standardScaleOfDisplay() - 0.01f ? budget : 0.f;
    }

    // tell: the message that says what was set; without it the settings are
    // only written (photo mode off, where none of them shows yet).
    void setUp(bool tell = true)
    {
        LuminousPhotoMode::HoldSettingTips quiet; // set for the user: no tips that answer a change of theirs
        const std::string chip = chipName();
        const S32 cores = graphicsCores();
        const Tier tier = tierOf(cores, chip);
        // What the display draws, not whether it is Retina: a 4K display at 1x is no Retina display, and a
        // base chip drew its full 3840 x 2160 at 8 frames a second.
        S32 display_x = 0;
        S32 display_y = 0;
        const F64 pixels = displayPixelCount(display_x, display_y);

        std::vector<std::string> chosen;
        const bool metal = LL::RHI::viewerBackend() == LL::RHI::BackendType::Metal;
        const U32 level = levelFor(tier, metal);
        LLFeatureManager::getInstance()->setGraphicsLevel(level, false);
        gSavedSettings.setU32("RenderQualityPerformance", level);
        // SMAA: FXAA keeps the live view at full size (chosenLiveScale). No level picks FXAA now.
        if (gSavedSettings.getU32("RenderFSAAType") == 1)
        {
            gSavedSettings.setU32("RenderFSAAType", 2);
        }
        chosen.push_back(std::string("graphics quality ") + levelName(level) + " on " + (metal ? "Metal" : "OpenGL")
                         + ": shadows, transparent water and ambient occlusion on, with SMAA anti-aliasing");

        // The live view at Standard, one pixel per point, or smaller where the display has more pixels than the
        // tier's budget even so. It pays for the level.
        const F32 live_scale = setUpLiveScale(tier, pixels);
        if (live_scale > 0.f)
        {
            chosen.push_back(std::string("the live view at ") + (live_scale > 0.7f ? "three quarters" : "half")
                             + llformat(" resolution each way: this display draws %d x %d pixels", display_x, display_y));
        }
        else
        {
            chosen.push_back("the live view at Standard resolution, one pixel per point");
        }
        gSavedSettings.setF32("LuminousPhotoLiveScale", live_scale);
        gSavedSettings.setBOOL("LuminousPhotoLiveScaleStandardChecked", true);
        if (tier == Tier::Base)
        {
            gSavedSettings.setBOOL("LuminousPhotoVolumetricLightRenderOnly", true);
            gSavedSettings.setBOOL("LuminousPhotoProjectorBeamsRenderOnly", true);
            chosen.push_back("volumetric light and projector beams in renders only: they show in every render, "
                             "not while you set up the shot");
        }
        else
        {
            // As the settings have it, which first start leaves as Luminous ships them.
            const bool gtao_live = !gSavedSettings.getBOOL("LuminousPhotoAORenderOnly");
            chosen.push_back(gtao_live ? "volumetric light, projector beams and GTAO live, at low quality"
                                       : "volumetric light and projector beams live, at low quality; GTAO in renders "
                                         "only: it shows in every render, not while you set up the shot");
        }

        std::string mac = chip.empty() ? std::string("this Mac") : chip;
        if (cores > 0)
        {
            mac += llformat(", %d graphics cores", cores);
        }
        if (display_x > 0 && display_y > 0)
        {
            mac += llformat(", a %d x %d display", display_x, display_y);
        }
        gSavedSettings.setString("LuminousSetUpFor", mac);
        LL_INFOS("LuminousOptimise") << "Set up for " << mac << ": graphics level " << level << " ("
            << (metal ? "Metal" : "OpenGL") << "), live scale " << live_scale
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
        if (tell)
        {
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", message));
        }
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
        if (sOfferedThisSession || !LuminousPhotoMode::isOn())
        {
            return true; // once a session, and never with photo mode off: the watch ends
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
            checkStandardLiveScale();
            return;
        }
        setUp(LuminousPhotoMode::isOn());
    }

    void checkStandardLiveScale()
    {
        if (gSavedSettings.getBOOL("LuminousPhotoLiveScaleStandardChecked"))
        {
            return;
        }
        gSavedSettings.setBOOL("LuminousPhotoLiveScaleStandardChecked", true);
        // A saved three quarters or half that the set-up chose for this Mac, when full was the default, goes to what
        // the set-up chooses now: Standard unless the tier's budget asks for fewer pixels. Any other value is the
        // user's own choice, or Optimise's that they applied, and stays.
        const F32 saved = gSavedSettings.getF32("LuminousPhotoLiveScale");
        if ((saved != 0.75f && saved != 0.5f) || gSavedSettings.getString("LuminousSetUpFor").empty())
        {
            return;
        }
        const Tier tier = tierOf(graphicsCores(), chipName());
        S32 display_x = 0;
        S32 display_y = 0;
        const F64 pixels = displayPixelCount(display_x, display_y);
        if (std::fabs(budgetLiveScale(tier, pixels) - saved) > 0.01f)
        {
            return;
        }
        const F32 now = setUpLiveScale(tier, pixels);
        LL_INFOS("LuminousOptimise") << "Preview resolution " << saved << " from the set-up for this Mac: now "
                                     << (now > 0.f ? llformat("%g", now) : std::string("Standard")) << LL_ENDL;
        LuminousPhotoMode::HoldSettingTips quiet;
        gSavedSettings.setF32("LuminousPhotoLiveScale", now);
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
    const bool metalfx = metalFXToMeasure();
    std::string intro;
    if (key.has("slow_fps"))
    {
        intro = llformat("The live view has run at about %d fps here for the last minute. Luminous can %s what each "
                         "live effect costs where you are, and suggest what to change to reach %d fps.",
                         llmax(1, ll_round((F32)key["slow_fps"].asReal())),
                         metalfx ? "try MetalFX upscaling and measure" : "measure", target);
    }
    else
    {
        intro = llformat("Luminous %s what each live effect costs where you are, and suggests what to change to "
                         "reach %d fps in the live view.",
                         metalfx ? "tries MetalFX upscaling and measures" : "measures", target);
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
    if (!LuminousPhotoMode::availableOrSay())
    {
        return; // it measures photo mode's effects
    }
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
    mKept.clear();
    mFirst = 0;
    mMetalFXWeighed = false;
    mMetalFXKept = false;
    mMeasureMetalFX = metalFXToMeasure();
    mSteps.push_back(baselineStep());
    if (mMeasureMetalFX)
    {
        // MetalFX upscaling first, at Preview resolution Half as its check box sets it, between two measures of the
        // view as it is; what comes after depends on what it gives (next, weighMetalFX).
        Step s;
        s.label = "MetalFX upscaling";
        s.settings.push_back({ "LuminousMetalFXTemporal", true, LLSD() });
        if (std::fabs(LuminousPhotoMode::settingLiveScale() - 0.5f) > 0.01f)
        {
            s.label += " and preview resolution half";
            s.settings.push_back({ "LuminousPhotoLiveScale", 0.5, LLSD() });
        }
        s.settle = SCALER_SETTLE;
        s.metalfx = true;
        s.scaler = true;
        mMetalFX = mSteps.size();
        mSteps.push_back(s);
        mSteps.push_back(baselineStep());
        mLater = laterSteps().size();
    }
    else
    {
        const std::vector<Step> later = laterSteps();
        mSteps.insert(mSteps.end(), later.begin(), later.end());
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
    LL_INFOS("LuminousOptimise") << "Measuring " << mSteps.size() << " steps"
                                 << (mMeasureMetalFX ? ", MetalFX upscaling first" : "") << LL_ENDL;

    LLHandle<LLFloater> handle = getHandle();
    doOnIdleRepeating([handle]()
    {
        LuminousOptimiseFloater* floater = static_cast<LuminousOptimiseFloater*>(handle.get());
        return !floater || floater->step();
    });
}

LuminousOptimiseFloater::Step LuminousOptimiseFloater::baselineStep()
{
    Step baseline;
    baseline.label = "As you have it";
    baseline.settle = 2.f;
    baseline.baseline = true;
    return baseline;
}

// The steps after the first measure of the view (and after MetalFX upscaling's, where it is measured), as the
// settings are when they begin: with MetalFX upscaling on where it is kept.
std::vector<LuminousOptimiseFloater::Step> LuminousOptimiseFloater::laterSteps() const
{
    std::vector<Step> steps;
    // The view as it is is measured again after every third change, so a
    // view that drifts -- a region still loading -- is measured against
    // where it has drifted to.
    S32 since_baseline = 0;
    for (const Effect& effect : EFFECTS)
    {
        if (live(effect))
        {
            Step s;
            s.label = effect.label;
            s.settings.push_back({ effect.render_only, true, LLSD() });
            steps.push_back(s);
            if (++since_baseline == 3)
            {
                steps.push_back(baselineStep());
                since_baseline = 0;
            }
        }
    }
    // A lower preview resolution: the next step down from what is set.
    // FXAA keeps the live view at full resolution (unless MetalFX temporal
    // smooths the edges in its place), so with FXAA the step is SMAA, under
    // which the live view comes to the Preview resolution setting itself
    // (settingLiveScale), proposed as one change. Three quarters is set with
    // it only where the setting comes to more than that (Full, or Standard
    // on a 1x screen); at or below it (Standard on a Retina screen, Three
    // quarters, Half) the setting is left as it is. The shaders are made
    // again for it, so it settles longer.
    if (LuminousPhotoMode::fxaaKeepsFullSize())
    {
        Step s;
        s.settings.push_back({ "RenderFSAAType", LLSD::Integer(2), LLSD() });
        const F32 setting = LuminousPhotoMode::settingLiveScale();
        if (setting > 0.76f)
        {
            s.label = "Anti-aliasing SMAA and preview resolution three quarters";
            s.settings.push_back({ "LuminousPhotoLiveScale", 0.75, LLSD() });
        }
        else if (gSavedSettings.getF32("LuminousPhotoLiveScale") <= 0.f)
        {
            s.label = "Anti-aliasing SMAA (preview resolution Standard)";
        }
        else
        {
            s.label = llformat("Anti-aliasing SMAA (preview resolution %d%%)", ll_round(setting * 100.f));
        }
        s.settle = 5.f;
        s.resolution = true;
        steps.push_back(s);
    }
    else if (!LuminousPhotoMode::temporalHeldAtFull())
    {
        // Not while MetalFX temporal holds the live view at full size (something it cannot scale shows live): a
        // lower preview resolution would change nothing until that goes.
        const F32 chosen = LuminousPhotoMode::chosenLiveScale();
        const F32 lower = chosen > 0.99f ? 0.75f : (chosen > 0.74f ? 0.5f : 0.f);
        if (lower > 0.f)
        {
            Step s;
            s.label = "Preview resolution " + scaleName(lower);
            s.settings.push_back({ "LuminousPhotoLiveScale", lower, LLSD() });
            s.settle = 3.f; // the targets are made again at the new size
            s.resolution = true;
            steps.push_back(s);
        }
    }
    if (!steps.empty() && !steps.back().baseline)
    {
        steps.push_back(baselineStep());
    }
    return steps;
}

void LuminousOptimiseFloater::beginStep()
{
    LuminousPhotoMode::HoldSettingTips quiet; // a measure, not the user's change
    Step& s = mSteps[mStep];
    for (Setting& setting : s.settings)
    {
        LLControlVariable* control = gSavedSettings.getControl(setting.name);
        setting.before = control->getValue();
        control->setValue(setting.value);
    }
    mCounting = false;
    mTimer.reset();
    std::string what = mMetalFXKept ? "the live view with MetalFX upscaling" : "the live view as it is";
    if (s.resolution || s.metalfx)
    {
        what = s.label;
    }
    else if (!s.settings.empty())
    {
        what = s.label + " in renders only";
    }
    // Until MetalFX upscaling is weighed, the steps after it are not yet in the list
    const size_t total = mSteps.size() + (mMeasureMetalFX && !mMetalFXWeighed ? mLater : 0);
    const S32 seconds = ll_round((F32)(total - mStep) * (MEASURE + 1.8f));
    showStatus(llformat("Measuring %d of %d: %s. Keep the camera where it is -- about %d s.", (S32)mStep + 1,
                        (S32)total, what.c_str(), seconds));
}

void LuminousOptimiseFloater::endStep()
{
    LuminousPhotoMode::HoldSettingTips quiet;
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
    putKeptBack();
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
        // With MetalFX upscaling on, the count begins with the frames its scaler scales: until then the live view
        // is scaled up the plain way. Where none comes, MetalFX upscaling does not scale here.
        if (t >= s.settle && s.scaler && !LuminousMetal::temporalFrame())
        {
            if (!LuminousMetal::temporalRefused() && t < s.settle + SCALER_WAIT)
            {
                return false;
            }
            if (s.metalfx)
            {
                LL_INFOS("LuminousOptimise") << s.label << ": MetalFX upscaling does not scale here" << LL_ENDL;
                s.unavailable = true;
                return next();
            }
        }
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
    return next();
}

// A step done: the next begins, or the measure ends. True when it has ended.
bool LuminousOptimiseFloater::next()
{
    endStep();
    ++mStep;
    if (mMeasureMetalFX && !mMetalFXWeighed)
    {
        if (mStep == mMetalFX && mSteps.front().ms <= targetMs() * 1.05f)
        {
            // The view as it is already reaches the aim: MetalFX upscaling is not needed, and not measured
            mMeasureMetalFX = false;
            mSteps.resize(mMetalFX);
            const std::vector<Step> later = laterSteps();
            mSteps.insert(mSteps.end(), later.begin(), later.end());
        }
        else if (mStep == mMetalFX + 1 && mSteps[mMetalFX].unavailable)
        {
            // It did not scale: the rest as without it, the view measured before it standing for the one after
            mMetalFXWeighed = true;
            mSteps.erase(mSteps.begin() + mStep);
            const std::vector<Step> later = laterSteps();
            mSteps.insert(mSteps.end(), later.begin(), later.end());
        }
        else if (mStep == mMetalFX + 2)
        {
            mMetalFXWeighed = true;
            weighMetalFX();
        }
    }
    if (mStep < mSteps.size())
    {
        beginStep();
        return false;
    }
    finish();
    return true;
}

// MetalFX upscaling measured between two measures of the view as it is. Where it reaches the aim, it is proposed
// alone and nothing more is measured; where it helps but does not, the rest is measured with it on, against the view
// with it; where it does not help (or the view swung or drifted too much to tell), the rest is measured as without
// it, and it is left out.
void LuminousOptimiseFloater::weighMetalFX()
{
    const Step& before = mSteps[mMetalFX - 1];
    const Step& after = mSteps[mMetalFX + 1];
    const Step& with = mSteps[mMetalFX];
    const F32 then = 0.5f * (before.ms + after.ms);
    const F32 then_err = llmax(before.err, after.err);
    const F32 step_noise = llmax(llmax(1.f, then * 0.05f), 2.f * sqrtf(with.err * with.err + then_err * then_err));
    const bool steady = llmax(before.spread, after.spread) <= TOO_UNSTEADY && std::fabs(after.ms - before.ms) <= then * 0.25f;
    const bool helps = steady && then - with.ms > step_noise;
    const bool alone = helps && with.ms <= targetMs() * 1.05f;
    LL_INFOS("LuminousOptimise") << "MetalFX upscaling: " << with.ms << " ms against " << then << " ms without"
                                 << (!steady ? ": the view too unsteady to tell, left out" : !helps ? ": left out"
                                     : alone ? ": reaches the aim alone" : ": kept on for the rest") << LL_ENDL;
    if (!helps)
    {
        const std::vector<Step> later = laterSteps();
        mSteps.insert(mSteps.end(), later.begin(), later.end());
        return;
    }
    mMetalFXKept = true;
    mFirst = mSteps.size();
    if (alone)
    {
        return;
    }
    const std::vector<Setting> settings = with.settings;
    LuminousPhotoMode::HoldSettingTips quiet;
    for (const Setting& setting : settings)
    {
        LLControlVariable* control = gSavedSettings.getControl(setting.name);
        mKept.push_back({ setting.name, setting.value, control->getValue() });
        control->setValue(setting.value);
    }
    Step kept = baselineStep();
    kept.label = "With MetalFX upscaling";
    kept.settle = SCALER_SETTLE;
    kept.scaler = true;
    mSteps.push_back(kept);
    const std::vector<Step> later = laterSteps();
    mSteps.insert(mSteps.end(), later.begin(), later.end());
}

void LuminousOptimiseFloater::putKeptBack()
{
    LuminousPhotoMode::HoldSettingTips quiet;
    for (auto setting = mKept.rbegin(); setting != mKept.rend(); ++setting)
    {
        gSavedSettings.getControl(setting->name)->setValue(setting->before);
    }
    mKept.clear();
}

void LuminousOptimiseFloater::finish()
{
    putKeptBack();
    sMeasuring = false;
    getChildView("measure_btn")->setEnabled(true);
    propose();
}

void LuminousOptimiseFloater::propose()
{
    // The view as it is (with MetalFX upscaling from mFirst, where it is kept), measured between the changes: its
    // mean is the frame now, and each change is measured against it as it stood then, interpolated between the
    // measurements before and after.
    F32 base = 0.f;
    F32 lowest = 1e9f;
    F32 highest = 0.f;
    S32 baselines = 0;
    for (size_t i = mFirst; i < mSteps.size(); ++i)
    {
        const Step& s = mSteps[i];
        if (s.baseline)
        {
            base += s.ms;
            lowest = llmin(lowest, s.ms);
            highest = llmax(highest, s.ms);
            ++baselines;
        }
    }
    // MetalFX upscaling reaching the aim alone ends the measure: the view with it is its own step then
    base = baselines > 0 ? base / (F32)baselines : (mMetalFXKept ? mSteps[mMetalFX].ms : 0.f);
    // The view as the user has it, before MetalFX upscaling
    F32 now = 0.f;
    S32 nows = 0;
    for (size_t i = 0; i < mFirst; ++i)
    {
        if (mSteps[i].baseline)
        {
            now += mSteps[i].ms;
            ++nows;
        }
    }
    now = nows > 0 ? now / (F32)nows : base;
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
    for (size_t i = mFirst; i < mSteps.size(); ++i)
    {
        const Step& s = mSteps[i];
        if (s.baseline || s.metalfx)
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

    std::string text = llformat("Now: about %d fps (%.1f ms a frame). Aim: %d fps.", fps(now), now, ll_round(target_fps));
    F32 predicted = base;
    // MetalFX upscaling reaches the aim by itself: nothing else is proposed
    const bool metalfx_alone = mMetalFXKept && base <= target_ms * 1.05f;
    if (!mMetalFXKept && base <= target_ms * 1.05f)
    {
        text += "\n\nThe live view already reaches it here: nothing needs to change.";
    }
    else
    {
        if (mMetalFXKept)
        {
            const Step& metalfx = mSteps[mMetalFX];
            Change change{ metalfx.label, {}, 1000.f / base };
            for (const Setting& setting : metalfx.settings)
            {
                change.settings.push_back({ setting.name, setting.value });
            }
            mChanges.push_back(change);
            text += llformat("\n- %s: %d -> %d fps (renders keep full resolution)", metalfx.label.c_str(), fps(now),
                             fps(base));
        }
        for (const Saving& saving : effects)
        {
            if (metalfx_alone || predicted <= target_ms)
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
        if (!metalfx_alone && predicted > target_ms && scale && scale_ratio < 0.97f)
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
        else if (metalfx_alone)
        {
            text += "\n\nThat reaches it here, so nothing else needs to change.";
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
    if (mMeasureMetalFX && mMetalFXWeighed && !mMetalFXKept && !mSteps[mMetalFX].unavailable)
    {
        const F32 without = 0.5f * (mSteps[mMetalFX - 1].ms + mSteps[mMetalFX + 1].ms);
        text += llformat("\n\nMetalFX upscaling does not help here (about %d fps with it, %d without), so it is left "
                         "out.", fps(mSteps[mMetalFX].ms), fps(without));
    }
    if (!costs.empty())
    {
        text += std::string("\n\nWhat each live effect costs here") + (mMetalFXKept ? ", with MetalFX upscaling:" : ":")
                + costs;
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
    LuminousPhotoMode::HoldSettingTips quiet; // the window says what it changed
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
    LuminousPhotoMode::HoldSettingTips quiet;
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
