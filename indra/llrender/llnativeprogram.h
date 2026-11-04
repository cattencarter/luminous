/**
 * @file llnativeprogram.h
 * @brief One library of hand-written MSL from a programs.json, with its pipelines
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

#ifndef LL_LLNATIVEPROGRAM_H
#define LL_LLNATIVEPROGRAM_H

#include "llrhi.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

/// One library of a programs.json under app_settings/shaders/metal (a group's manifest, such as
/// "luminous/dof/programs.json"): its files read with their includes resolved (llmslinclude.h), compiled by the viewer's
/// device in the background, and its compute and render pipelines kept. Only on a device with native programs (Metal):
/// elsewhere load() returns false without reading anything. Main thread. Every loaded program is released when the
/// viewer's device is (LL::RHI::onViewerDeviceRelease), before the device goes, so one held in a static is safe.
///
/// The manifest's format, which luminous/scripts/msl-check reads too:
///   { "libraries": [ { "name": "...", "files": ["luminous/<group>/x.metal", ...], "defines": { "NAME": "value" },
///       "compute": [ { "function": "...", "threadsPerThreadgroup": [x, y, z] } ],
///       "render": [ { "vertex": "...", "fragment": "...", "colorFormats": ["RGBA16Float"], "depthFormat": "",
///                     "blend": "none" } ] } ] }
/// Paths are relative to app_settings/shaders/metal. The render entries are what msl-check builds; the viewer's passes
/// give their own PipelineDesc to render().
class LLNativeProgram
{
public:
    using Defines = std::vector<std::pair<std::string, std::string>>;

    LLNativeProgram() = default;
    ~LLNativeProgram();
    LLNativeProgram(const LLNativeProgram&) = delete;
    LLNativeProgram& operator=(const LLNativeProgram&) = delete;

    /// Reads the manifest and the library's files, and starts the compilation with the manifest's defines and these
    /// (a name given here wins). Loading again releases what was loaded first. False after one WARN line: no device
    /// with native programs, a manifest or file that cannot be read, or a library the manifest does not have.
    bool load(const std::string& manifest, const std::string& library, const Defines& defines = {});

    /// Pending while it compiles, Failed when not loaded or when it failed (its log in log()).
    LL::RHI::CompileStatus status() const;
    bool ready() const { return status() == LL::RHI::CompileStatus::Ready; }
    std::string log() const;
    const std::string& name() const { return mName; }

    /// The kernel's pipeline, made at the first ask and kept, with the manifest's threadgroup size for it (zeros when
    /// the manifest names none). Pending until compiled; null when not loaded or failed.
    LL::RHI::ComputePipeline* compute(const std::string& function);
    /// A render pipeline of two of the library's functions for the given formats and blend (attributeMask 0), made at
    /// the first ask and kept. Pending until compiled; null when not loaded or failed.
    LL::RHI::Pipeline* render(const std::string& vertex, const std::string& fragment, const LL::RHI::PipelineDesc& desc);

    /// Lets go of the library and its pipelines.
    void release();

    /// The library's source as load() would compile it, without a device: the manifest read from root (a directory
    /// that holds the manifest's paths), each file's includes resolved, the files concatenated in order, and the
    /// defines. Also each compute entry's threadgroup size. False with the reason in error.
    static bool readSource(const std::string& root, const std::string& manifest, const std::string& library,
                           const Defines& defines, LL::RHI::NativeSource& out,
                           std::map<std::string, LL::RHI::DispatchSize>* threads, std::string& error);

    /// The directory of the native programs: app_settings/shaders/metal, or what setRoot gave (tests).
    static std::string root();
    static void setRoot(const std::string& root);

private:
    std::string mName;
    LL::RHI::Ref<LL::RHI::NativeLibrary> mLibrary;
    std::map<std::string, LL::RHI::DispatchSize> mThreads;
    std::map<std::string, LL::RHI::Ref<LL::RHI::ComputePipeline>> mCompute;
    std::map<std::string, LL::RHI::Ref<LL::RHI::Pipeline>> mRender;
};

#endif // LL_LLNATIVEPROGRAM_H
