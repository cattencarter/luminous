/**
 * @file luminousenvironmentlistener.cpp
 * @brief A LEAP event API for the sun, so a test script can light a scene.
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

// An event API of the same kind as Linden Lab's own LLAgent, LLViewerControl
// and LLViewerWindow listeners: it opens nothing and listens to nothing outside
// the viewer. The only way to reach it from outside is a LEAP child that the
// viewer itself started because it was launched with --leap, which is how the
// test tooling drives a viewer -- settings, environment, camera, snapshots.
// Linden's 26.x has no such API for the environment; their Lua work adds one
// later.
//
// It does what the Personal Lighting window does, and nothing more: freeze the
// sky that is showing into the personal (local) environment and set its sun.
// Like that window, it never touches the region's or the parcel's settings.

#include "llviewerprecompiledheaders.h"

#include "lleventapi.h"
#include "llenvironment.h"
#include "llevents.h"
#include "llsdutil.h"
#include "llsettingssky.h"
#include "llsettingswater.h"
#include "llsettingsvo.h"
#include "llvirtualtrackball.h"

namespace
{
    class LuminousEnvironmentListener : public LLEventAPI
    {
    public:
        LuminousEnvironmentListener()
            : LLEventAPI("LuminousEnvironment",
                         "The personal environment's sky, sun and water, as Personal Lighting sets them")
        {
            add("setSun",
                "Freeze the sky that is showing into the personal environment, with its sun\n"
                "at [\"azimuth\"] and [\"elevation\"] in degrees, as Personal Lighting does.\n"
                "Azimuth is Personal Lighting's own. Reply on [\"reply\"] with [\"ok\"].",
                &LuminousEnvironmentListener::setSun,
                llsd::map("azimuth", LLSD::Real(), "elevation", LLSD::Real()));
            add("getSun",
                "Reply on [\"reply\"] with the showing sky's sun: [\"azimuth\"], [\"elevation\"] in degrees.",
                &LuminousEnvironmentListener::getSun,
                llsd::map("reply", LLSD()));
            add("reset",
                "Drop the personal environment; the region's or parcel's shows again.",
                &LuminousEnvironmentListener::clearSun);
            add("getSky",
                "Reply on [\"reply\"] with the showing sky's settings as [\"sky\"] (LLSD), the\n"
                "form Luminous's environment templates are kept in.",
                &LuminousEnvironmentListener::getSky,
                llsd::map("reply", LLSD()));
            add("getWater",
                "Reply on [\"reply\"] with [\"ok\"] and the showing water's settings as [\"water\"] (LLSD).",
                &LuminousEnvironmentListener::getWater,
                llsd::map("reply", LLSD()));
            add("setWater",
                "Show [\"water\"] (a water's settings, as getWater gives them) in the personal\n"
                "environment, keeping its sky. Checked first: a missing field, a wrong type or a\n"
                "value out of range changes nothing. Reply on [\"reply\"] with [\"ok\"], or\n"
                "[\"ok\"] false and [\"error\"].",
                &LuminousEnvironmentListener::setWater,
                llsd::map("water", LLSD(), "reply", LLSD()));
            add("setSky",
                "Show [\"sky\"] (a sky's settings, as getSky gives them) as the personal\n"
                "environment, as Personal Lighting does; with [\"azimuth\"] and [\"elevation\"]\n"
                "its sun there. Reply on [\"reply\"] with [\"ok\"].",
                &LuminousEnvironmentListener::setSky,
                llsd::map("sky", LLSD()));
        }

    private:
        void setSun(const LLSD& event) const
        {
            if (!environmentReady())
            {
                sendReply(llsd::map("ok", false, "error", "no environment yet (not logged in)"), event);
                return;
            }
            LLEnvironment& environment = LLEnvironment::instance();
            LLSettingsSky::ptr_t current = environment.getCurrentSky();
            if (!current)
            {
                sendReply(llsd::map("ok", false, "error", "no sky yet"), event);
                return;
            }

            // Personal Lighting's own conversion, onSunAzimElevChanged.
            F32 azimuth = (F32)event["azimuth"].asReal() * DEG_TO_RAD;
            F32 elevation = (F32)event["elevation"].asReal() * DEG_TO_RAD;
            if (is_approx_zero(elevation))
            {
                elevation = F_APPROXIMATELY_ZERO;
            }
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

            sendReply(llsd::map("ok", true), event);
        }

        void getSun(const LLSD& event) const
        {
            // before login getCurrentSky reads through no environment at all
            LLSettingsSky::ptr_t sky = environmentReady() ? LLEnvironment::instance().getCurrentSky() : LLSettingsSky::ptr_t();
            F32 azimuth = 0.f;
            F32 elevation = 0.f;
            if (sky)
            {
                LLVirtualTrackball::getAzimuthAndElevationDeg(sky->getSunRotation(), azimuth, elevation);
            }
            sendReply(llsd::map("azimuth", azimuth, "elevation", elevation), event);
        }

        void getSky(const LLSD& event) const
        {
            LLSettingsSky::ptr_t sky = environmentReady() ? LLEnvironment::instance().getCurrentSky() : LLSettingsSky::ptr_t();
            sendReply(llsd::map("sky", sky ? sky->getSettings() : LLSD()), event);
        }

        void setSky(const LLSD& event) const
        {
            if (!environmentReady())
            {
                sendReply(llsd::map("ok", false, "error", "no environment yet (not logged in)"), event);
                return;
            }
            LLSettingsSky::ptr_t sky = LLSettingsVOSky::buildSky(event["sky"]);
            if (!sky)
            {
                sendReply(llsd::map("ok", false, "error", "not a sky"), event);
                return;
            }
            if (event.has("azimuth") && event.has("elevation"))
            {
                sky->setSunRotation(sunRotation((F32)event["azimuth"].asReal(), (F32)event["elevation"].asReal()));
            }
            sky->update();
            LLEnvironment& environment = LLEnvironment::instance();
            environment.setEnvironment(LLEnvironment::ENV_LOCAL, sky);
            environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
            environment.updateEnvironment(LLEnvironment::TRANSITION_INSTANT);
            sendReply(llsd::map("ok", true), event);
        }

        // Personal Lighting's own conversion, onSunAzimElevChanged.
        static LLQuaternion sunRotation(F32 azimuth_deg, F32 elevation_deg)
        {
            const F32 azimuth = azimuth_deg * DEG_TO_RAD;
            F32 elevation = elevation_deg * DEG_TO_RAD;
            if (is_approx_zero(elevation))
            {
                elevation = F_APPROXIMATELY_ZERO;
            }
            LLQuaternion quat;
            quat.setAngleAxis(-elevation, 0, 1, 0);
            LLQuaternion az_quat;
            az_quat.setAngleAxis(F_TWO_PI - azimuth, 0, 0, 1);
            quat *= az_quat;
            return quat;
        }

        // Before login there is no environment to read or set.
        static bool environmentReady()
        {
            LLEnvironment& environment = LLEnvironment::instance();
            return environment.getCurrentEnvironmentInstance() && environment.getCurrentWater();
        }

        void getWater(const LLSD& event) const
        {
            if (!environmentReady())
            {
                sendReply(llsd::map("ok", false, "error", "no environment yet (not logged in)"), event);
                return;
            }
            sendReply(llsd::map("ok", true, "water", LLEnvironment::instance().getCurrentWater()->getSettings()), event);
        }

        // A field's value as its validator wants it, where JSON over LEAP
        // changed it: texture ids come as strings, and no texture (the null
        // id) as null; whole numbers come as integers.
        static void coerce(LLSD& settings, const std::string& name, LLSD::Type type)
        {
            if (!settings.has(name))
            {
                return;
            }
            LLSD& value = settings[name];
            if (type == LLSD::TypeUUID && (value.isUndefined() || (value.isString() && value.asString().empty())))
            {
                value = LLUUID::null;
            }
            else if (type == LLSD::TypeUUID && value.isString() && LLUUID::validate(value.asString()))
            {
                value = LLUUID(value.asString());
            }
            else if (type == LLSD::TypeReal && value.isInteger())
            {
                value = LLSD::Real(value.asInteger());
            }
            else if (type == LLSD::TypeInteger && value.isReal() && value.asReal() == (F64)(S32)value.asReal())
            {
                value = LLSD::Integer(value.asInteger());
            }
        }

        void setWater(const LLSD& event) const
        {
            if (!environmentReady())
            {
                sendReply(llsd::map("ok", false, "error", "no environment yet (not logged in)"), event);
                return;
            }
            if (!event["water"].isMap())
            {
                sendReply(llsd::map("ok", false, "error", "water must be a map of water settings, as getWater gives"), event);
                return;
            }
            LLSD settings = llsd_clone(event["water"]);
            if (!settings.has(LLSettingsBase::SETTING_TYPE))
            {
                settings[LLSettingsBase::SETTING_TYPE] = "water";
            }
            if (settings[LLSettingsBase::SETTING_TYPE].asString() != "water")
            {
                sendReply(llsd::map("ok", false, "error", "type is not water"), event);
                return;
            }
            LLSettingsWater::validation_list_t validations = LLSettingsWater::validationList();
            for (const auto& validator : validations)
            {
                coerce(settings, validator.getName(), validator.getType());
            }
            coerce(settings, LLSettingsWater::SETTING_TRANSPARENT_TEXTURE, LLSD::TypeUUID);
            coerce(settings, LLSettingsBase::SETTING_ID, LLSD::TypeUUID);
            coerce(settings, LLSettingsBase::SETTING_ASSETID, LLSD::TypeUUID);
            coerce(settings, LLSettingsBase::SETTING_HASH, LLSD::TypeInteger);
            coerce(settings, LLSettingsBase::SETTING_FLAGS, LLSD::TypeInteger);

            // Checked on a copy: Linden's validation clamps what is out of
            // range and strips what it has no validator for -- the
            // transparent texture among them -- and buildWater builds the
            // water whatever it finds. Out of range is refused here, so
            // nothing is shown that was not asked for.
            LLSD checked = llsd_clone(settings);
            LLSD results = LLSettingsWater::settingValidation(checked, validations);
            if (!results["success"].asBoolean())
            {
                std::string errors;
                for (const LLSD& error : llsd::inArray(results["errors"]))
                {
                    errors += (errors.empty() ? "" : " ") + error.asString();
                }
                sendReply(llsd::map("ok", false, "error", "invalid water: " + errors), event);
                return;
            }
            // The ranges Linden's water validators hold (llsettingswater.cpp),
            // checked here: their clamp asks is_approx_equal(F64, F64), which
            // compares only the low 32 bits of the two doubles, so a round
            // value past the top -- a fog density of 500, a fresnel scale of
            // 5 -- passes unclamped.
            struct Range { const char* name; F64 low; F64 high; S32 min_size; S32 max_size; };
            static const Range RANGES[] = {
                { "blur_multiplier",    -0.5,   0.5, 0, 0 },
                { "water_fog_color",     0.0,   1.0, 3, 4 },
                { "water_fog_density",   0.001, 100.0, 0, 0 },
                { "underwater_fog_mod",  0.0,  20.0, 0, 0 },
                { "fresnel_offset",      0.0,   1.0, 0, 0 },
                { "fresnel_scale",       0.0,   1.0, 0, 0 },
                { "normal_scale",        0.0,  10.0, 3, 3 },
                { "scale_above",         0.0,   3.0, 0, 0 },
                { "scale_below",         0.0,   3.0, 0, 0 },
                { "wave1_direction",   -20.0,  20.0, 2, 2 },
                { "wave2_direction",   -20.0,  20.0, 2, 2 },
            };
            for (const Range& range : RANGES)
            {
                if (!settings.has(range.name))
                {
                    continue;
                }
                const LLSD& value = settings[range.name];
                const std::string allowed = llformat("%s must be %s from %g to %g", range.name,
                                                     range.max_size ? "values" : "a value", range.low, range.high);
                if (range.max_size)
                {
                    const S32 size = value.isArray() ? (S32)value.size() : -1;
                    if (size < range.min_size || size > range.max_size)
                    {
                        sendReply(llsd::map("ok", false, "error",
                            llformat("%s must be a list of %d to %d numbers", range.name, range.min_size, range.max_size)), event);
                        return;
                    }
                    for (const LLSD& v : llsd::inArray(value))
                    {
                        // the fog colour's fourth value, if any, is its alpha: 1
                        if (!v.isReal() && !v.isInteger())
                        {
                            sendReply(llsd::map("ok", false, "error", allowed), event);
                            return;
                        }
                    }
                    for (S32 i = 0; i < size && i < 3; ++i)
                    {
                        if (value[i].asReal() < range.low || value[i].asReal() > range.high)
                        {
                            sendReply(llsd::map("ok", false, "error", allowed), event);
                            return;
                        }
                    }
                }
                else if (value.asReal() < range.low || value.asReal() > range.high)
                {
                    sendReply(llsd::map("ok", false, "error", allowed), event);
                    return;
                }
            }
            for (const auto& validator : validations)
            {
                const std::string name = validator.getName();
                if (settings.has(name) && !llsd_equals(settings[name], checked[name]))
                {
                    sendReply(llsd::map("ok", false, "error", name + " is out of range"), event);
                    return;
                }
            }
            if (settings.has(LLSettingsWater::SETTING_TRANSPARENT_TEXTURE))
            {
                if (!settings[LLSettingsWater::SETTING_TRANSPARENT_TEXTURE].isUUID())
                {
                    sendReply(llsd::map("ok", false, "error", "transparent_texture is not a texture id"), event);
                    return;
                }
                checked[LLSettingsWater::SETTING_TRANSPARENT_TEXTURE] = settings[LLSettingsWater::SETTING_TRANSPARENT_TEXTURE];
            }

            // The personal environment's water; its sky stays (or, if it
            // has none yet, is the sky showing, as Personal Lighting's water
            // takes it).
            LLSettingsWater::ptr_t water = std::make_shared<LLSettingsVOWater>(checked);
            water->update();
            LLEnvironment& environment = LLEnvironment::instance();
            environment.setEnvironment(LLEnvironment::ENV_LOCAL, water);
            environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
            environment.updateEnvironment(LLEnvironment::TRANSITION_INSTANT);
            sendReply(llsd::map("ok", true), event);
        }

        void clearSun(const LLSD& event) const
        {
            if (!environmentReady())
            {
                sendReply(llsd::map("ok", false, "error", "no environment yet (not logged in)"), event);
                return;
            }
            LLEnvironment& environment = LLEnvironment::instance();
            environment.clearEnvironment(LLEnvironment::ENV_LOCAL);
            environment.setSelectedEnvironment(LLEnvironment::ENV_PARCEL, LLEnvironment::TRANSITION_INSTANT);
            environment.updateEnvironment(LLEnvironment::TRANSITION_INSTANT);
            sendReply(llsd::map("ok", true), event);
        }
    };

    LuminousEnvironmentListener sLuminousEnvironmentListener;
}
