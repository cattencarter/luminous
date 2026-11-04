/**
 * @file llnativeprogram.cpp
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

#include "linden_common.h"
#include "llnativeprogram.h"

#include "lldir.h"
#include "llfile.h"
#include "llmslinclude.h"
#include "llsdjson.h"
#include "llsdutil.h"

#include <fstream>
#include <set>
#include <sstream>

namespace
{
    std::string& rootOverride()
    {
        static std::string root;
        return root;
    }

    /// The programs holding a library, released all together before the viewer's device goes.
    std::set<LLNativeProgram*>& loaded()
    {
        static std::set<LLNativeProgram*> programs;
        return programs;
    }

    std::optional<std::string> readText(const std::string& path)
    {
        llifstream in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open())
        {
            return std::nullopt;
        }
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }
}

LLNativeProgram::~LLNativeProgram()
{
    release();
}

std::string LLNativeProgram::root()
{
    if (!rootOverride().empty())
    {
        return rootOverride();
    }
    return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders", "metal");
}

void LLNativeProgram::setRoot(const std::string& root)
{
    rootOverride() = root;
}

bool LLNativeProgram::readSource(const std::string& root, const std::string& manifest, const std::string& library,
                                 const Defines& defines, LL::RHI::NativeSource& out,
                                 std::map<std::string, LL::RHI::DispatchSize>* threads, std::string& error)
{
    const auto read = [&root](const std::string& path) { return readText(root + "/" + path); };
    const std::optional<std::string> text = read(manifest);
    if (!text)
    {
        error = "cannot read manifest '" + manifest + "'";
        return false;
    }
    LLSD parsed;
    try
    {
        parsed = LlsdFromJson(boost::json::parse(*text));
    }
    catch (const std::exception& e)
    {
        error = "manifest '" + manifest + "' is not JSON: " + e.what();
        return false;
    }
    LLSD entry;
    for (const LLSD& candidate : llsd::inArray(parsed["libraries"]))
    {
        if (candidate["name"].asString() == library)
        {
            entry = candidate;
            break;
        }
    }
    if (!entry.isMap())
    {
        error = "manifest '" + manifest + "' has no library '" + library + "'";
        return false;
    }

    out = LL::RHI::NativeSource();
    out.name = library;
    // Each file with its includes resolved, in the manifest's order; the shared headers keep include guards, so a
    // header two files include is compiled once.
    for (const LLSD& file : llsd::inArray(entry["files"]))
    {
        const LL::MSL::IncludeResult resolved = LL::MSL::resolveIncludes(file.asString(), read);
        if (!resolved.error.empty())
        {
            error = "library '" + library + "': " + resolved.error;
            return false;
        }
        out.text += resolved.text;
        out.text += "\n";
    }
    if (out.text.empty())
    {
        error = "library '" + library + "' has no files";
        return false;
    }
    // The manifest's defines, then the caller's, which replace one of the same name.
    for (const auto& [name, value] : llsd::inMap(entry["defines"]))
    {
        out.defines.emplace_back(name, value.asString());
    }
    for (const auto& [name, value] : defines)
    {
        auto same = std::find_if(out.defines.begin(), out.defines.end(),
                                 [&name](const auto& define) { return define.first == name; });
        if (same != out.defines.end())
        {
            same->second = value;
        }
        else
        {
            out.defines.emplace_back(name, value);
        }
    }
    if (threads)
    {
        threads->clear();
        for (const LLSD& compute : llsd::inArray(entry["compute"]))
        {
            LL::RHI::DispatchSize size{ 0, 0, 0 };
            const LLSD& given = compute["threadsPerThreadgroup"];
            if (given.isArray() && given.size() == 3)
            {
                size = { U32(given[0].asInteger()), U32(given[1].asInteger()), U32(given[2].asInteger()) };
            }
            (*threads)[compute["function"].asString()] = size;
        }
    }
    return true;
}

bool LLNativeProgram::load(const std::string& manifest, const std::string& library, const Defines& defines)
{
    release();
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || !device->capabilities().nativePrograms)
    {
        LL_WARNS("RHI") << "LLNativeProgram '" << library << "': native programs need Metal" << LL_ENDL;
        return false;
    }
    LL::RHI::NativeSource source;
    std::string error;
    if (!readSource(root(), manifest, library, defines, source, &mThreads, error))
    {
        LL_WARNS("RHI") << "LLNativeProgram: " << error << LL_ENDL;
        return false;
    }
    mName = library;
    mLibrary = device->createNativeLibrary(source);
    if (mLibrary.isNull())
    {
        return false;
    }
    // A library and its pipelines refer to the device without holding it, so they go before it does.
    static const bool hooked = []()
    {
        LL::RHI::onViewerDeviceRelease([]()
        {
            const std::set<LLNativeProgram*> programs = loaded();
            for (LLNativeProgram* program : programs)
            {
                program->release();
            }
        });
        return true;
    }();
    (void)hooked;
    loaded().insert(this);
    return true;
}

LL::RHI::CompileStatus LLNativeProgram::status() const
{
    return mLibrary.notNull() ? mLibrary->status() : LL::RHI::CompileStatus::Failed;
}

std::string LLNativeProgram::log() const
{
    return mLibrary.notNull() ? mLibrary->log() : std::string("not loaded");
}

LL::RHI::ComputePipeline* LLNativeProgram::compute(const std::string& function)
{
    if (mLibrary.isNull() || mLibrary->status() == LL::RHI::CompileStatus::Failed)
    {
        return nullptr;
    }
    auto found = mCompute.find(function);
    if (found == mCompute.end())
    {
        LL::RHI::ComputePipelineDesc desc;
        desc.function = function;
        const auto threads = mThreads.find(function);
        desc.threadsPerThreadgroup = threads != mThreads.end() ? threads->second : LL::RHI::DispatchSize{ 0, 0, 0 };
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        LL::RHI::Ref<LL::RHI::ComputePipeline> pipeline = device ? device->getComputePipeline(*mLibrary, desc) : nullptr;
        found = mCompute.emplace(function, pipeline).first; // a refusal is kept too, so it warns once
    }
    return found->second.get();
}

LL::RHI::Pipeline* LLNativeProgram::render(const std::string& vertex, const std::string& fragment,
                                           const LL::RHI::PipelineDesc& desc)
{
    if (mLibrary.isNull() || mLibrary->status() == LL::RHI::CompileStatus::Failed)
    {
        return nullptr;
    }
    const LL::RHI::PipelineDesc key = LL::RHI::normalized(desc);
    const std::string text = LL::RHI::nativePipelineKey({ vertex, fragment }, key);
    auto found = mRender.find(text);
    if (found == mRender.end())
    {
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        LL::RHI::Ref<LL::RHI::Pipeline> pipeline =
            device ? device->getNativePipeline(*mLibrary, { vertex, fragment }, key) : nullptr;
        found = mRender.emplace(text, pipeline).first;
    }
    return found->second.get();
}

void LLNativeProgram::release()
{
    mCompute.clear();
    mRender.clear();
    mThreads.clear();
    mLibrary = nullptr;
    mName.clear();
    loaded().erase(this);
}
