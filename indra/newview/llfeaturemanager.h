/**
 * @file llfeaturemanager.h
 * @brief The feature manager is responsible for determining what features are turned on/off in the app.
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#ifndef LL_LLFEATUREMANAGER_H
#define LL_LLFEATUREMANAGER_H

#include "stdtypes.h"

#include "llsingleton.h"
#include "llstring.h"
#include <map>
#include "llcoros.h"
#include "lleventcoro.h"

typedef enum EGPUClass
{
    GPU_CLASS_UNKNOWN = -1,
    GPU_CLASS_0 = 0,
    GPU_CLASS_1 = 1,
    GPU_CLASS_2 = 2,
    GPU_CLASS_3 = 3,
    GPU_CLASS_4 = 4,
    GPU_CLASS_5 = 5
} EGPUClass;


class LLFeatureInfo
{
public:
    LLFeatureInfo() : mValid(false), mAvailable(false), mRecommendedLevel(-1) {}
    LLFeatureInfo(const std::string& name, const bool available, const F32 level);

    bool isValid() const    { return mValid; };

public:
    bool        mValid;
    std::string mName;
    bool        mAvailable;
    F32         mRecommendedLevel;
};


class LLFeatureList
{
public:
    typedef std::map<std::string, LLFeatureInfo> feature_map_t;

    LLFeatureList(const std::string& name);
    virtual ~LLFeatureList();

    bool isFeatureAvailable(const std::string& name);
    F32 getRecommendedValue(const std::string& name);

    void setFeatureAvailable(const std::string& name, const bool available);
    void setRecommendedLevel(const std::string& name, const F32 level);

    bool loadFeatureList(LLFILE *fp);

    bool maskList(LLFeatureList &mask);

    void addFeature(const std::string& name, const bool available, const F32 level);

    feature_map_t& getFeatures()
    {
        return mFeatures;
    }

    void dump();
protected:
    std::string mName;
    feature_map_t   mFeatures;
};


class LLFeatureManager : public LLFeatureList, public LLSingleton<LLFeatureManager>
{
    LLSINGLETON(LLFeatureManager);
    ~LLFeatureManager() {cleanupFeatureTables();}

    // initialize this by loading feature table and gpu table
    void initSingleton() override;

public:

    void maskCurrentList(const std::string& name); // Mask the current feature list with the named list

    bool loadFeatureTables();

    EGPUClass getGPUClass()             { return mGPUClass; }
    // [Viewer Lab] Golden-image tests lock the class the benchmark would set (llgoldentest.cpp).
    void setGPUClass(EGPUClass gpu_class) { mGPUClass = gpu_class; }
    std::string& getGPUString()         { return mGPUString; }

    // get the measured GPU memory bandwidth in GB/sec
    // may return 0 of benchmark has not been run or failed to run
    F32 getGPUMemoryBandwidth() { return mGPUMemoryBandwidth; }
    bool isGPUSupported()               { return mGPUSupported; }
    F32 getExpectedGLVersion()          { return mExpectedGLVersion; }

    void cleanupFeatureTables();

    S32 getVersion() const              { return mTableVersion; }
    void setSafe(const bool safe)       { mSafe = safe; }
    bool isSafe() const                 { return mSafe; }
    bool skipProfiling() const          { return mSkipProfiling; }

    LLFeatureList *findMask(const std::string& name);
    bool maskFeatures(const std::string& name);

    // set the graphics to low, medium, high, or ultra.
    // skipFeatures forces skipping of mostly hardware settings
    // that we don't want to change when we change graphics
    // settings
    void setGraphicsLevel(U32 level, bool skipFeatures);

    // What 'level' values are valid to pass to setGraphicsLevel()?
    // 0 is the low end...
    U32 getMaxGraphicsLevel() const;
    bool isValidGraphicsLevel(U32 level) const;

    // setGraphicsLevel() levels have names.
    std::string getNameForGraphicsLevel(U32 level) const;
    // returns -1 for unrecognized name (hence S32 rather than U32)
    S32 getGraphicsLevelForName(const std::string& name) const;

    void applyBaseMasks();
    void applyRecommendedSettings();

    // apply the basic masks.  Also, skip one saved
    // in the skip list if true
    void applyFeatures(bool skipFeatures);

    LLSD getRecommendedSettingsMap();

    // [Viewer Lab] On Metal the feature table's TexUnit<n>orLess lists cap their
    // settings whatever the graphics level (LL::RHI::hardwareSettingCaps), because a program above the argument
    // tables' samplers fails there; on GL there are no such caps and nothing below changes anything.
    // The caps for the viewer's device now, worked out once per device; empty on GL and before the device exists.
    const std::map<std::string, F32>& hardwareCaps() const;
    // A value of a setting held to its cap (unchanged without one).
    F32 hardwareCapped(const std::string& name, F32 value) const;
    // At startup, after the window's graphics level (later levels, --graphicslevel and the golden hook, are masked by
    // the same lists) and before the pipeline builds shaders (LLViewerWindow): lowers each saved or default value
    // above its cap to the cap as an unsaved value (the settings file keeps the user's value), one INFO line each,
    // and from then on refuses a new value above the cap (the settings' validate signal, one WARN line each), so
    // Preferences, graphics presets and Debug Settings cannot raise it.
    void applyHardwareCaps();

protected:
    bool loadGPUClass();
    // [Viewer Lab] The texture units the TexUnit<n>orLess lists judge the device by
    // (LL::RHI::featureTableTextureUnits).
    S32 featureTableTextureUnits() const;

    bool parseFeatureTable(std::string filename);
    ///< @returns true is file parsed correctly, false if not

    void initBaseMask();

    std::map<std::string, LLFeatureList *> mMaskList;
    std::set<std::string> mSkippedFeatures;
    bool        mInited;
    S32         mTableVersion;
    bool        mSafe;                  // Reinitialize everything to the "safe" mask
    EGPUClass   mGPUClass;
    F32         mGPUMemoryBandwidth = 0.f;  // measured memory bandwidth of GPU in GB/second
    F32         mExpectedGLVersion;     //expected GL version according to gpu table
    std::string mGPUString;
    bool        mGPUSupported;
    bool        mSkipProfiling = false;
    std::set<std::string> mHardwareCapGuards; // [Viewer Lab] The settings whose validate signal refuses above the cap
    mutable std::map<std::string, F32> mHardwareCaps; // [Viewer Lab] hardwareCaps() for mHardwareCapsDevice
    mutable const void* mHardwareCapsDevice = nullptr;
};

inline
LLFeatureManager::LLFeatureManager()
:   LLFeatureList("default"),

    mInited(false),
    mTableVersion(0),
    mSafe(false),
    mGPUClass(GPU_CLASS_UNKNOWN),
    mExpectedGLVersion(0.f),
    mGPUSupported(false)
{
}

#endif // LL_LLFEATUREMANAGER_H
