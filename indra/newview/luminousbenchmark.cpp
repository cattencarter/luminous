/**
 * @file luminousbenchmark.cpp
 * @brief A fixed benchmark at Blake Sea - Windlass, to compare Macs.
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

#include "luminousbenchmark.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "llcharacter.h"
#include "lldir.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llmotioncontroller.h"
#include "llnotificationsutil.h"
#include "llsettingssky.h"
#include "llsys.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "luminoushdrsnapshot.h"
#include "luminousphotomode.h"
#include "pipeline.h"

#include <ctime>
#include <memory>

namespace LuminousBenchmark
{
    namespace
    {
        // The place and the view: the steps down to the beach, the palms and
        // the sea beyond, the pose stand in the middle distance.
        const char* const REGION = "Blake Sea - Windlass";
        const LLVector3 CAMERA(130.f, 140.f, 30.f);
        const LLVector3 TARGET(130.f, 110.f, 24.f);
        const F32 SUN_AZIMUTH = 300.f;   // Personal Lighting's: from the west-north-west
        const F32 SUN_ELEVATION = 30.f;
        const F32 SETTLE = 20.f;         // seconds after each change: shaders, buffers, exposure
        const F32 MEASURE = 10.f;

        bool sRunning = false;

        struct Saved
        {
            std::map<std::string, bool> low_quality; // each effect's, by setting
            F32 animation_speed;
            LLSettingsSky::ptr_t local_sky;
        };

        struct Run
        {
            Saved saved;
            S32 phase = 0;
            LLTimer timer;
            U32 frames = 0;
            F32 results[2] = { 0.f, 0.f };
            U32 live_render_w = 0; // the live view, which may be smaller than the window
            U32 live_render_h = 0;
            F32 snapshot_seconds = 0.f;
        };

        void tell(const std::string& message)
        {
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
        }

        void setAnimationSpeed(F32 factor)
        {
            LLMotionController::setCurrentTimeFactor(factor);
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                character->setAnimTimeFactor(factor);
            }
        }

        // As the Personal Lighting window: the showing sky, frozen into the
        // personal environment, with the sun moved.
        void fixSun()
        {
            LLEnvironment& environment = LLEnvironment::instance();
            LLSettingsSky::ptr_t current = environment.getCurrentSky();
            if (!current)
            {
                return;
            }
            const F32 azimuth = SUN_AZIMUTH * DEG_TO_RAD;
            const F32 elevation = SUN_ELEVATION * DEG_TO_RAD;
            LLQuaternion quat;
            quat.setAngleAxis(-elevation, 0, 1, 0);
            LLQuaternion az_quat;
            az_quat.setAngleAxis(F_TWO_PI - azimuth, 0, 0, 1);
            quat *= az_quat;
            LLSettingsSky::ptr_t sky = current->buildClone();
            sky->setSunRotation(quat);
            sky->update();
            environment.setEnvironment(LLEnvironment::ENV_LOCAL, sky);
            environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
            environment.updateEnvironment(LLEnvironment::TRANSITION_INSTANT);
        }

        void placeCamera()
        {
            LLViewerRegion* region = gAgent.getRegion();
            const LLVector3d origin = region->getOriginGlobal();
            gAgentCamera.unlockView();
            gAgentCamera.setFocusOnAvatar(false, false);
            gAgentCamera.setCameraPosAndFocusGlobal(origin + LLVector3d(CAMERA), origin + LLVector3d(TARGET), LLUUID::null);
        }

        // Low quality in live preview, every effect's at once: the benchmark
        // measures the live view both ways.
        void setLowQualityLive(bool low)
        {
            for (LuminousPhotoMode::LiveQuality effect : LuminousPhotoMode::LIVE_QUALITIES)
            {
                gSavedSettings.setBOOL(LuminousPhotoMode::lowQualityLiveSetting(effect), low);
            }
        }

        void restore(const Saved& saved)
        {
            for (const auto& [setting, low] : saved.low_quality)
            {
                gSavedSettings.setBOOL(setting, low);
            }
            setAnimationSpeed(saved.animation_speed);
            LLEnvironment& environment = LLEnvironment::instance();
            if (saved.local_sky)
            {
                environment.setEnvironment(LLEnvironment::ENV_LOCAL, saved.local_sky);
                environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
            }
            else
            {
                environment.clearEnvironment(LLEnvironment::ENV_LOCAL);
                environment.setSelectedEnvironment(LLEnvironment::ENV_PARCEL, LLEnvironment::TRANSITION_INSTANT);
            }
            environment.updateEnvironment(LLEnvironment::TRANSITION_INSTANT);
            gAgentCamera.setFocusOnAvatar(true, true);
        }

        void report(const Run& run)
        {
            const S32 width = gViewerWindow->getWindowWidthRaw();
            const S32 height = gViewerWindow->getWindowHeightRaw();
            char stamp[32];
            time_t now = time(nullptr);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", localtime(&now));
            const std::string text = llformat(
                "Luminous benchmark, %s\n"
                "%s\n%s\nWindow %d x %d\n\n"
                "Live view, each effect as set: %.1f fps (rendering %u x %u)\n"
                "Live view, low quality effects: %.1f fps\n"
                "Render, 16 frames at the window's size: %.1f s\n",
                stamp, gGLManager.mGLRenderer.c_str(), gSysCPU.getCPUString().c_str(),
                width, height, run.results[0], run.live_render_w, run.live_render_h,
                run.results[1], run.snapshot_seconds);
            LL_INFOS("LuminousBenchmark") << text << LL_ENDL;

            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "luminous_benchmark.txt");
            LLFILE* file = LLFile::fopen(path, "a");
            if (file)
            {
                fputs((text + "\n").c_str(), file);
                fclose(file);
            }
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", text + "\nAlso kept in " + path));
        }

        // One step each frame: settle, count frames, move on.
        bool step(const std::shared_ptr<Run>& run)
        {
            const F32 t = run->timer.getElapsedTimeF32();
            auto measuring = [&](S32 index) -> bool
            {
                if (t < SETTLE)
                {
                    run->frames = LLFrameTimer::getFrameCount();
                    return false;
                }
                if (t < SETTLE + MEASURE)
                {
                    return false;
                }
                run->results[index] = (F32)(LLFrameTimer::getFrameCount() - run->frames) / (t - SETTLE);
                return true;
            };
            if (!gAgent.getRegion())
            {
                restore(run->saved);
                sRunning = false;
                tell("The benchmark stopped: no region.");
                return true;
            }
            switch (run->phase)
            {
                case 0: // the live view, as the user has it
                    if (measuring(0))
                    {
                        run->live_render_w = gPipeline.mMainRT.screen.getWidth();
                        run->live_render_h = gPipeline.mMainRT.screen.getHeight();
                        setLowQualityLive(true);
                        run->phase = 1;
                        run->timer.reset();
                    }
                    return false;
                case 1: // low quality effects in live preview
                    if (measuring(1))
                    {
                        setLowQualityLive(false);
                        run->phase = 2;
                        run->timer.reset();
                    }
                    return false;
                case 2: // a render, timed; saved to a scratch file and removed
                    if (t < 5.f)
                    {
                        return false;
                    }
                    {
                        const S32 frames = gSavedSettings.getU32("LuminousSnapshotFrames");
                        const U32 scale = gSavedSettings.getU32("LuminousSnapshotScale");
                        gSavedSettings.setU32("LuminousSnapshotFrames", 16);
                        gSavedSettings.setU32("LuminousSnapshotScale", 1);
                        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_TEMP, "luminous_benchmark.png");
                        std::string error;
                        LLTimer timer;
                        LuminousHDRSnapshot::takeAndSave(LuminousHDRSnapshot::PNG8, path, error);
                        run->snapshot_seconds = timer.getElapsedTimeF32();
                        LLFile::remove(path);
                        gSavedSettings.setU32("LuminousSnapshotFrames", frames);
                        gSavedSettings.setU32("LuminousSnapshotScale", scale);
                    }
                    restore(run->saved);
                    sRunning = false;
                    report(*run);
                    return true;
            }
            return true;
        }

        void start()
        {
            if (sRunning)
            {
                tell("The benchmark is already running.");
                return;
            }
            LLViewerRegion* region = gAgent.getRegion();
            if (!region || region->getName() != REGION)
            {
                tell(std::string("The benchmark runs in ") + REGION + ", the same view on every Mac: "
                     "go there first (secondlife:///app/teleport/Blake%20Sea%20-%20Windlass/130/130/25).");
                return;
            }
            sRunning = true;
            auto run = std::make_shared<Run>();
            for (LuminousPhotoMode::LiveQuality effect : LuminousPhotoMode::LIVE_QUALITIES)
            {
                const char* setting = LuminousPhotoMode::lowQualityLiveSetting(effect);
                run->saved.low_quality[setting] = gSavedSettings.getBOOL(setting);
            }
            run->saved.animation_speed = LLMotionController::getCurrentTimeFactor();
            run->saved.local_sky = LLEnvironment::instance().getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);

            setLowQualityLive(false);
            setAnimationSpeed(0.f);
            fixSun();
            placeCamera();
            tell("Benchmark: about a minute. Leave the viewer alone until the results show.");
            run->timer.reset();
            doOnIdleRepeating([run]() { return step(run); });
        }
    }

    bool running()
    {
        return sRunning;
    }

    void init()
    {
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.Benchmark",
            [](LLUICtrl*, const LLSD&) { start(); });
    }
}
