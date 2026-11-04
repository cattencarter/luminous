/**
 * @file llrhi.cpp
 * @brief Render hardware interface: shared code and the table from GL's enums
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

#include "llrhi.h"

#if LL_DARWIN
#include "llrhimetal.h"

#include <sys/sysctl.h>
#endif
#ifndef LL_RHI_METAL
#define LL_RHI_METAL 0 // no Metal backend off Darwin
#endif

#include "llglheaders.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <iterator>
#include <sstream>

namespace LL::RHI
{

// Formats ----------------------------------------------------------------------------------------

bool isCompressed(Format format)
{
    switch (format)
    {
    case Format::BC1_RGBA:
    case Format::BC1_RGBA_sRGB:
    case Format::BC3_RGBA:
    case Format::BC3_RGBA_sRGB:
    case Format::BC5_RG:
    case Format::BC7_RGBA:
    case Format::BC7_RGBA_sRGB:
        return true;
    default:
        return false;
    }
}

bool isDepth(Format format)
{
    return format == Format::Depth32Float || format == Format::Depth32Float_Stencil8;
}

bool hasStencil(Format format)
{
    return format == Format::Depth32Float_Stencil8;
}

bool isInteger(Format format)
{
    return format == Format::R8Uint || format == Format::R16Uint || format == Format::R32Uint;
}

bool isSRGB(Format format)
{
    switch (format)
    {
    case Format::RGBA8Unorm_sRGB:
    case Format::BC1_RGBA_sRGB:
    case Format::BC3_RGBA_sRGB:
    case Format::BC7_RGBA_sRGB:
        return true;
    default:
        return false;
    }
}

U32 blockBytes(Format format)
{
    switch (format)
    {
    case Format::BC1_RGBA:
    case Format::BC1_RGBA_sRGB:
        return 8;
    case Format::BC3_RGBA:
    case Format::BC3_RGBA_sRGB:
    case Format::BC5_RG:
    case Format::BC7_RGBA:
    case Format::BC7_RGBA_sRGB:
        return 16;
    default:
        return 0;
    }
}

U32 PixelLayout::bytesPerPixel() const
{
    if (type == Type::U32_8_8_8_8_Rev)
    {
        return 4;
    }

    U32 count = 0;
    switch (components)
    {
    case Components::R:
    case Components::Depth:
        count = 1;
        break;
    case Components::RG:
        count = 2;
        break;
    case Components::RGB:
        count = 3;
        break;
    case Components::RGBA:
    case Components::BGRA:
        count = 4;
        break;
    }

    U32 size = 0;
    switch (type)
    {
    case Type::U8:
        size = 1;
        break;
    case Type::U16:
    case Type::F16:
        size = 2;
        break;
    case Type::U32:
    case Type::F32:
    case Type::U32_8_8_8_8_Rev:
        size = 4;
        break;
    }
    return count * size;
}

// Backends ---------------------------------------------------------------------------------------

namespace
{
    /// Why the facts rule Metal out, checked in a fixed order with Rosetta first; null when they
    /// allow it.
    const char* metalRefusal(const PlatformFacts& facts)
    {
        if (facts.translated)
        {
            return "the process runs translated by Rosetta; Metal needs a native arm64 process";
        }
        if (!facts.arm64)
        {
            return "the process is not arm64 (Metal needs Apple Silicon)";
        }
        if (!facts.built)
        {
            return "the Metal backend is not built into this viewer (no Metal 4 SDK)";
        }
        if (!facts.macOS26)
        {
            return "macOS is older than 26.0";
        }
        if (!facts.metalDevice)
        {
            return "there is no Metal device";
        }
        if (!facts.metal4)
        {
            return "the GPU lacks MTLGPUFamilyMetal4";
        }
        if (!facts.apple7)
        {
            return "the GPU lacks MTLGPUFamilyApple7";
        }
        return nullptr;
    }

#if LL_RHI_METAL
    PlatformFacts metalFacts(const MetalSupport& support)
    {
        PlatformFacts facts;
        facts.built = true;
        facts.arm64 = true; // LL_RHI_METAL is only 1 in an arm64 build
        facts.macOS26 = support.macOS26;
        facts.metalDevice = support.device;
        facts.metal4 = support.metal4;
        facts.apple7 = support.apple7;
        facts.gpuName     = support.renderer; // the device metalBackend asked
        return facts;
    }
#endif

    /// The facts of this process and Mac; asked only when Metal is.
    PlatformFacts platformFacts()
    {
        PlatformFacts facts;
#if defined(__arm64__) || defined(__aarch64__)
        facts.arm64 = true;
#endif
#if LL_DARWIN
        int translated = 0;
        size_t size = sizeof(translated);
        facts.translated = sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0 &&
                           translated == 1;
#endif
#if LL_RHI_METAL
        facts.built = true;
        if (facts.arm64 && !facts.translated)
        {
            MetalSupport support;
            metalBackend(support, nullptr);
            facts = metalFacts(support);
        }
#endif
        return facts;
    }
}

BackendChoice chooseBackend(BackendType preferred)
{
    return chooseBackend(preferred, preferred == BackendType::Metal ? platformFacts() : PlatformFacts());
}

BackendChoice chooseBackend(BackendType preferred, const PlatformFacts& facts)
{
    BackendChoice choice;
    if (preferred != BackendType::Metal)
    {
        LL_INFOS("RHI") << "Render backend: GL (Metal not asked for)" << LL_ENDL;
        return choice;
    }
    if (const char* reason = metalRefusal(facts))
    {
        choice.reason = reason;
        LL_WARNS("RHI") << "Render backend: GL, because " << reason << LL_ENDL;
        return choice;
    }
    choice.type = BackendType::Metal;
    choice.gpuName = facts.gpuName;
    LL_INFOS("RHI") << "Render backend: Metal" << LL_ENDL;
    return choice;
}

namespace
{
    enum class Asked
    {
        None,
        GL,
        Metal,
        Unknown,
    };

    Asked askedBackend(const std::string& value)
    {
        if (value.empty())
        {
            return Asked::None;
        }
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "metal")
        {
            return Asked::Metal;
        }
        return lower == "opengl" ? Asked::GL : Asked::Unknown;
    }
}

BackendRequest resolveBackendRequest(const BackendRequestFacts& facts)
{
    BackendRequest request;
    const Asked commandLine = askedBackend(facts.commandLine);
    const Asked saved       = askedBackend(facts.saved);
    const bool  marker      = facts.markerApplies && facts.metalSessionOpen;
    if (commandLine == Asked::Unknown)
    {
        LL_WARNS("RHI") << "Graphics backend: --backend '" << facts.commandLine
                        << "' is neither metal nor opengl; GraphicsBackend decides" << LL_ENDL;
    }
    if (commandLine == Asked::Metal || commandLine == Asked::GL)
    {
        request.type = commandLine == Asked::Metal ? BackendType::Metal : BackendType::GL;
    }
    else if (saved == Asked::Metal)
    {
        if (marker)
        {
            request.fellBack = true;
            LL_WARNS("RHI") << "Render backend: GL, because the last Metal session did not end normally" << LL_ENDL;
        }
        else
        {
            request.type = BackendType::Metal;
        }
    }
    else if (saved != Asked::GL)
    {
        LL_WARNS("RHI") << "Graphics backend: GraphicsBackend '" << facts.saved << "' is neither OpenGL nor Metal; GL"
                        << LL_ENDL;
    }
    request.setMarker = request.type == BackendType::Metal && facts.markerApplies;
    // A command-line opengl over a saved Metal keeps the marker, so the next start with the saved choice still falls
    // back; under a saved choice that is not Metal the marker is stale (a Metal session that saved OpenGL and crashed).
    request.clearMarker = marker && !request.setMarker && (request.fellBack || saved != Asked::Metal);
    if (request.clearMarker && !request.fellBack)
    {
        LL_INFOS("RHI") << "Graphics backend: MetalSessionOpen cleared, since GraphicsBackend is not Metal" << LL_ENDL;
    }
    return request;
}

bool clearsMetalSessionAtQuit(bool sessionSetMarker, BackendType running)
{
    return sessionSetMarker && running == BackendType::Metal;
}

std::string windowTitleWithBackend(const std::string& title, BackendType running)
{
    return title + (running == BackendType::Metal ? " (Metal)" : " (OpenGL)");
}

UploadThreads uploadThreads(BackendType running, const UploadThreadSettings& settings)
{
    UploadThreads threads;
    threads.textures = settings.renderGLTextures;
    threads.media    = settings.renderGLMedia;
    if (LL_RHI_METAL && running == BackendType::Metal)
    {
        threads.textures = threads.textures || settings.metalTextures;
        threads.media    = threads.media || settings.metalMedia;
        LL_INFOS("RHI") << "Load threads on Metal (P3-03d): textures " << (threads.textures ? "on" : "off")
                        << " (MetalMultiThreadedTextures " << settings.metalTextures << ", RenderGLMultiThreadedTextures "
                        << settings.renderGLTextures << "), media " << (threads.media ? "on" : "off")
                        << " (MetalMultiThreadedMedia " << settings.metalMedia << ", RenderGLMultiThreadedMedia "
                        << settings.renderGLMedia << ")" << LL_ENDL;
    }
    return threads;
}

// [Viewer Lab] The feature table's texture-unit lists on Metal
S32 featureTableTextureUnits(const Capabilities& caps, S32 systemTextureUnits)
{
    if (caps.backend == BackendType::Metal)
    {
        return std::min(systemTextureUnits, S32(caps.maxSamplersPerStage));
    }
    return systemTextureUnits;
}

SettingCaps hardwareSettingCaps(const Capabilities& caps, S32 systemTextureUnits, const std::map<std::string, SettingCaps>& lists)
{
    SettingCaps result;
    if (caps.backend != BackendType::Metal)
    {
        return result; // GL: Linden's behaviour, the lists mask the graphics levels only
    }
    const S32 units = featureTableTextureUnits(caps, systemTextureUnits);
    const std::string prefix = "TexUnit";
    const std::string suffix = "orLess";
    for (const auto& [name, settings] : lists)
    {
        if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0
            || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        {
            continue;
        }
        const std::string count = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        if (count.size() > 6 || count.find_first_not_of("0123456789") != std::string::npos || units > std::stoi(count))
        {
            continue;
        }
        for (const auto& [setting, value] : settings)
        {
            const auto found = result.find(setting);
            result[setting] = found == result.end() ? value : std::min(found->second, value);
        }
    }
    return result;
}

F32 cappedSetting(const SettingCaps& caps, const std::string& name, F32 value)
{
    const auto found = caps.find(name);
    return found == caps.end() ? value : std::min(value, found->second);
}
// [Viewer Lab] end

bool metalSelectable(S32 macOSMajor)
{
    // An arm64 process is never translated by Rosetta, so the build and the OS decide.
    return LL_RHI_METAL && macOSMajor >= 26;
}

std::string graphicsBackendEntry(const std::string& saved)
{
    return askedBackend(saved) == Asked::Metal ? "Metal" : "OpenGL";
}

std::string graphicsBackendToSave(const std::string& saved, const std::string& chosen)
{
    const Asked asked = askedBackend(chosen);
    if (asked != Asked::Metal && asked != Asked::GL)
    {
        return std::string();
    }
    const std::string entry = asked == Asked::Metal ? "Metal" : "OpenGL";
    return entry == graphicsBackendEntry(saved) ? std::string() : entry;
}

Ref<Device> Device::create(const DeviceCreateInfo& info)
{
    switch (info.backend.type)
    {
    case BackendType::GL:
        return createGLDevice(info);
    case BackendType::Metal:
#if LL_RHI_METAL
    {
        // The support is checked again, so a hand-made BackendChoice gives null and a WARN line.
        MetalSupport support;
        Ref<Device> device = metalBackend(support, &info);
        if (const char* reason = metalRefusal(metalFacts(support)))
        {
            warnCreateFailed("device", "Metal", reason);
        }
        return device;
    }
#else
        break;
#endif
    }
    warnCreateFailed("device", "Metal", "the Metal backend is not built yet");
    return nullptr;
}

// The viewer's device ----------------------------------------------------------------------------

namespace
{
    // One manual reference (ref() in createViewerDevice, unref() in releaseViewerDevice) rather
    // than a static Ref, so a process that never reaches LLViewerWindow::shutdownGL makes no GL
    // call in static destruction. Atomic, because the GL error checks also run on
    // LLImageGLThread.
    std::atomic<Device*> sViewerDevice{ nullptr };
    std::atomic<BackendType> sViewerBackend{ BackendType::GL };

    std::vector<std::function<void()>>& releaseHooks()
    {
        static std::vector<std::function<void()>> hooks;
        return hooks;
    }
}

void onViewerDeviceRelease(std::function<void()> then)
{
    if (then)
    {
        releaseHooks().push_back(std::move(then));
    }
}

Device* viewerDevice()
{
    return sViewerDevice.load();
}

Device* createViewerDevice(const DeviceCreateInfo& info)
{
    if (Device* device = sViewerDevice.load())
    {
        LL_WARNS("RHI") << "The viewer's render device exists already; there is one per process" << LL_ENDL;
        return device;
    }
    if (info.backend.type != viewerBackend())
    {
        LL_WARNS("RHI") << "The viewer's render device was asked for "
                        << (info.backend.type == BackendType::GL ? "GL" : "Metal")
                        << ", but the viewer's backend is "
                        << (viewerBackend() == BackendType::GL ? "GL" : "Metal") << LL_ENDL;
        return nullptr;
    }
    Ref<Device> device = Device::create(info);
    if (device.isNull())
    {
        return nullptr;
    }
    device->ref();
    sViewerDevice.store(device.get());
    LL_INFOS("RHI") << "The viewer's render device is created: "
                    << (info.backend.type == BackendType::GL ? "GL" : "Metal") << ", " << info.width << "x"
                    << info.height << LL_ENDL;
    return device.get();
}

void releaseViewerDevice()
{
    if (!sViewerDevice.load())
    {
        return;
    }
    // What still refers to the device without holding it goes first, while viewerDevice() still gives it.
    for (const std::function<void()>& hook : releaseHooks())
    {
        hook();
    }
    Device* device = sViewerDevice.exchange(nullptr);
    if (!device)
    {
        return;
    }
    if (device->getNumRefs() != 1)
    {
        LL_WARNS("RHI") << "The viewer's render device is still held " << device->getNumRefs() - 1
                        << " more times at its release, so it outlives the accessor" << LL_ENDL;
    }
    LL_INFOS("RHI") << "The viewer's render device is released" << LL_ENDL;
    device->unref();
}

BackendType viewerBackend()
{
    return sViewerBackend.load();
}

void setViewerBackend(BackendType type)
{
    if (sViewerDevice.load())
    {
        LL_WARNS("RHI") << "The viewer's backend cannot change while its render device exists" << LL_ENDL;
        return;
    }
    sViewerBackend.store(type);
}

ViewerFrame::ViewerFrame()
{
    if (Device* device = viewerDevice())
    {
        device->beginFrame();
        mDeviceId = device->id();
    }
}

ViewerFrame::~ViewerFrame()
{
    // Not on a device that replaced the one the frame began on.
    Device* device = viewerDevice();
    if (device && device->id() == mDeviceId)
    {
        device->endFrame();
    }
}

// Test support -----------------------------------------------------------------------------------

namespace
{
    // Golden level 2's test fault: the viewer's device makes a named program fail at its
    // publication, as a program the backend cannot compile, so that LLShaderMgr's fall-back can be
    // tried without a broken GLSL file; and the test delay, which keeps one Pending. One
    // object for both, so the library registers no further destructor at load. Used on the main
    // thread.
    struct ProgramTests
    {
        ProgramFault fault;
        U32 faultHits = 0;
        U32 faultsInRow = 0; ///< creations of the named program taken since one built
        ProgramDelay delay;
        U32 delaysInRow = 0; ///< creations of the named program delayed since one was not
    };
    ProgramTests sProgramTests;
}

void setProgramFault(const ProgramFault& fault)
{
    sProgramTests.fault = fault;
    sProgramTests.faultHits = 0;
    sProgramTests.faultsInRow = 0;
}

bool takeProgramFault(const std::string& program, bool& deferred)
{
    const ProgramFault& fault = sProgramTests.fault;
    if (fault.program.empty() || program != fault.program)
    {
        return false;
    }
    if (fault.times > 0 && sProgramTests.faultsInRow >= fault.times)
    {
        sProgramTests.faultsInRow = 0; // this creation builds, and the next load fails its first ones again
        return false;
    }
    ++sProgramTests.faultsInRow;
    ++sProgramTests.faultHits;
    deferred = fault.deferred;
    return true;
}

U32 programFaultHits()
{
    return sProgramTests.faultHits;
}

void setProgramDelay(const ProgramDelay& delay)
{
    sProgramTests.delay = delay;
    sProgramTests.delaysInRow = 0;
}

bool takeProgramDelay(const std::string& program, U32& frames)
{
    const ProgramDelay& delay = sProgramTests.delay;
    if (delay.program.empty() || program != delay.program)
    {
        return false;
    }
    if (delay.creations > 0 && sProgramTests.delaysInRow >= delay.creations)
    {
        sProgramTests.delaysInRow = 0; // this creation is not delayed, and the next load delays its first ones again
        return false;
    }
    ++sProgramTests.delaysInRow;
    frames = delay.frames;
    return true;
}

// Draw state -------------------------------------------------------------------------------------

PrimitiveClass primitiveClass(PrimitiveType type)
{
    switch (type)
    {
    case PrimitiveType::Points:
        return PrimitiveClass::Point;
    case PrimitiveType::Lines:
    case PrimitiveType::LineStrip:
    case PrimitiveType::LineLoop:
        return PrimitiveClass::Line;
    case PrimitiveType::Triangles:
    case PrimitiveType::TriangleStrip:
    case PrimitiveType::TriangleFan:
        break;
    }
    return PrimitiveClass::Triangle;
}

U32 indexSize(IndexType type)
{
    return type == IndexType::U16 ? 2 : 4;
}

// Objects ----------------------------------------------------------------------------------------

namespace
{
    // Starts at 1 so that 0 can mean "no object" in caches and hashes. 64 bits never wrap.
    std::atomic<U64> sNextObjectId{ 1 };
}

Object::Object()
:   mId(sNextObjectId.fetch_add(1))
{
}

namespace
{
    // boost::hash_combine's mixing, with the 64-bit constant.
    size_t hashCombine(size_t seed, size_t value)
    {
        return seed ^ (value + size_t(0x9e3779b97f4a7c15ull) + (seed << 6) + (seed >> 2));
    }
}

size_t SamplerDescHash::operator()(const SamplerDesc& desc) const
{
    // Every field but the anisotropy fits in a few bits, and the anisotropy's 32 bits go above them, so the packed value
    // is a perfect hash on a 64-bit size_t. -0 and +0 are equal descriptions, so they pack alike: made one on the bits,
    // which fast floating point (MSVC's /fp:fast) cannot fold away as it may a compare with 0.
    static_assert(sizeof(size_t) == 8, "the packed hash needs a 64-bit size_t");
    size_t packed = static_cast<size_t>(desc.filter);
    packed = (packed << 4) | static_cast<size_t>(desc.addressU);
    packed = (packed << 4) | static_cast<size_t>(desc.addressV);
    packed = (packed << 4) | static_cast<size_t>(desc.addressW);
    packed = (packed << 1) | (desc.compare ? 1 : 0);
    packed = (packed << 4) | static_cast<size_t>(desc.compareFunc);
    packed = (packed << 1) | (desc.mipmapped ? 1 : 0); // [Viewer Lab]
    packed |= static_cast<size_t>(desc.magFilter) << 20; // [Viewer Lab] FromFilter is 0, so every earlier value stays
    U32 anisotropy = std::bit_cast<U32>(desc.maxAnisotropy);
    if ((anisotropy & 0x7fffffffu) == 0)
    {
        anisotropy = 0;
    }
    packed |= static_cast<size_t>(anisotropy) << 32;
    return packed;
}

size_t PipelineKeyHash::operator()(const PipelineKey& key) const
{
    const PipelineDesc& desc = key.desc;
    size_t hash = std::hash<U64>()(key.programId);
    hash = hashCombine(hash, desc.attributeMask);
    hash = hashCombine(hash, static_cast<size_t>(desc.primitiveClass));
    hash = hashCombine(hash, desc.samples);
    hash = hashCombine(hash, desc.colorCount);
    for (Format format : desc.colorFormats)
    {
        hash = hashCombine(hash, static_cast<size_t>(format));
    }
    hash = hashCombine(hash, static_cast<size_t>(desc.depthFormat));
    const BlendState& blend = desc.blend;
    size_t packed = blend.enabled ? 1 : 0;
    packed = (packed << 4) | static_cast<size_t>(blend.srcColor);
    packed = (packed << 4) | static_cast<size_t>(blend.dstColor);
    packed = (packed << 4) | static_cast<size_t>(blend.srcAlpha);
    packed = (packed << 4) | static_cast<size_t>(blend.dstAlpha);
    packed = (packed << 3) | static_cast<size_t>(blend.colorOp);
    packed = (packed << 3) | static_cast<size_t>(blend.alphaOp);
    packed = (packed << 4) | desc.colorMask;
    return hashCombine(hash, packed);
}

size_t DepthStencilDescHash::operator()(const DepthStencilDesc& desc) const
{
    size_t packed = desc.depthTest ? 1 : 0;
    packed = (packed << 1) | (desc.depthWrite ? 1 : 0);
    packed = (packed << 4) | static_cast<size_t>(desc.depthFunc);
    return packed;
}

// Programs ---------------------------------------------------------------------------------------

U32 uniformSize(UniformType type)
{
    switch (type)
    {
    case UniformType::Float:
    case UniformType::Int:
        return 4;
    case UniformType::Vec2:
    case UniformType::IVec2:
        return 8;
    case UniformType::Vec3:
    case UniformType::IVec3:
        return 12;
    case UniformType::Vec4:
    case UniformType::IVec4:
    case UniformType::Mat2:
        return 16;
    case UniformType::Mat3:
        return 36;
    case UniformType::Mat3x4:
        return 48;
    case UniformType::Mat4:
        return 64;
    case UniformType::Sampler2D:
    case UniformType::Sampler2DShadow:
    case UniformType::SamplerCube:
    case UniformType::SamplerCubeArray:
    case UniformType::Sampler3D:
        return 4;
    }
    return 0;
}

bool isSampler(UniformType type)
{
    return type == UniformType::Sampler2D || type == UniformType::Sampler2DShadow
        || type == UniformType::SamplerCube || type == UniformType::SamplerCubeArray || type == UniformType::Sampler3D;
}

// Validation -------------------------------------------------------------------------------------

namespace
{
    // Builds the reason only when there is one, because textures are created often (media).
    template <class... Parts>
    std::string reason(const Parts&... parts)
    {
        std::ostringstream why;
        (why << ... << parts);
        return why.str();
    }

    std::string hexText(U32 value)
    {
        std::ostringstream text;
        text << "0x" << std::hex << value;
        return text.str();
    }
}

namespace
{
    /// The largest side of a 3D texture on Apple GPUs (Metal feature set tables).
    constexpr U32 MAX_TEXTURE_3D_SIZE = 2048;
}

std::string validate(const BufferDesc& desc, U64 initialBytes, const Capabilities* caps)
{
    constexpr U8 knownUsage = BUFFER_VERTEX | BUFFER_INDEX | BUFFER_UNIFORM | BUFFER_STAGING
                            | BUFFER_READBACK | BUFFER_STORAGE;
    if (desc.size == 0)
    {
        return reason("the size is 0");
    }
    if (desc.usage == 0 || (desc.usage & ~knownUsage) != 0)
    {
        return reason("the usage ", U32(desc.usage), " is empty or unknown");
    }
    if (initialBytes > desc.size)
    {
        return reason("the initial data (", initialBytes, " bytes) is larger than the buffer (",
                      desc.size, " bytes)");
    }
    if ((desc.usage & BUFFER_STORAGE) && !(caps && caps->computePasses))
    {
        return reason("storage buffers need compute passes, which the device does not have");
    }
    return {};
}

std::string validate(const TextureDesc& desc, const Capabilities& caps)
{
    constexpr U8 knownUsage = TEXTURE_SAMPLED | TEXTURE_RENDER_TARGET | TEXTURE_READBACK | TEXTURE_STORAGE | TEXTURE_MEMORYLESS;
    const bool cube = desc.type == TextureType::Cube || desc.type == TextureType::CubeArray;
    const bool volume = desc.type == TextureType::Tex3D;

    if (desc.format == Format::Unknown || desc.format >= Format::Count)
    {
        return reason("the format is unknown");
    }
    if (desc.format == Format::BGRA8Unorm)
    {
        return reason("BGRA8Unorm is for drawables only");
    }
    if (desc.glFormat != 0 && formatFromGL(desc.glFormat) != desc.format)
    {
        return reason("the GL format ", hexText(desc.glFormat), " does not match the format");
    }
    if (desc.usage == 0 || (desc.usage & ~knownUsage) != 0)
    {
        return reason("the usage ", U32(desc.usage), " is empty or unknown");
    }
    if (desc.width == 0 || desc.height == 0 || desc.layers == 0 || desc.mipLevels == 0
        || desc.samples == 0)
    {
        return reason("the width, height, layers, mip levels or samples are 0");
    }
    if (desc.width > caps.maxTextureSize || desc.height > caps.maxTextureSize)
    {
        return reason(desc.width, "x", desc.height, " is larger than the maximum size ",
                      caps.maxTextureSize);
    }
    if (cube && desc.width != desc.height)
    {
        return reason("cube faces must be square, not ", desc.width, "x", desc.height);
    }
    if ((desc.usage & TEXTURE_STORAGE) && !caps.computePasses)
    {
        return reason("storage textures need compute passes, which the device does not have");
    }
    if ((desc.usage & TEXTURE_STORAGE) && (isDepth(desc.format) || isCompressed(desc.format) || desc.samples > 1))
    {
        return reason("storage textures are colour, uncompressed and single-sampled");
    }
    if ((desc.usage & TEXTURE_MEMORYLESS) && !caps.computePasses)
    {
        return reason("memoryless textures need Metal");
    }
    if ((desc.usage & TEXTURE_MEMORYLESS) && (desc.usage != (TEXTURE_MEMORYLESS | TEXTURE_RENDER_TARGET) || desc.mipLevels != 1
                                              || desc.type != TextureType::Tex2D))
    {
        return reason("memoryless textures are 2D render targets of one level and nothing else");
    }
    if (volume)
    {
        // A lookup table's shape: sampled, one level, one sample; written by uploads, one slice at a time.
        if (!caps.textures3D)
        {
            return reason("3D textures are not supported");
        }
        if (desc.mipLevels != 1 || desc.samples != 1)
        {
            return reason("3D textures have one mip level and one sample");
        }
        if (desc.usage & (TEXTURE_RENDER_TARGET | TEXTURE_READBACK))
        {
            return reason("3D textures are neither render targets nor read back");
        }
        if (desc.layers > MAX_TEXTURE_3D_SIZE || desc.width > MAX_TEXTURE_3D_SIZE || desc.height > MAX_TEXTURE_3D_SIZE)
        {
            return reason("3D textures are at most ", MAX_TEXTURE_3D_SIZE, " on a side, not ", desc.width, "x",
                          desc.height, "x", desc.layers);
        }
        if (isCompressed(desc.format) || isDepth(desc.format))
        {
            return reason("3D textures cannot have a compressed or depth format");
        }
    }
    if (desc.type != TextureType::CubeArray && !volume && desc.layers != 1)
    {
        return reason("only cube map arrays and 3D textures have layers");
    }
    if (desc.type == TextureType::CubeArray && !caps.cubeMapArrays)
    {
        return reason("cube map arrays are not supported");
    }
    if (desc.type == TextureType::CubeArray && U64(desc.layers) * 6 > caps.maxArrayLayers)
    {
        return reason(desc.layers, " cubes need more than the maximum ", caps.maxArrayLayers,
                      " array layers");
    }
    if (desc.mipLevels > fullMipCount(desc.width, desc.height))
    {
        return reason(desc.mipLevels, " mip levels are more than a full chain for ", desc.width,
                      "x", desc.height);
    }
    if (desc.samples > 1
        && (desc.type != TextureType::Tex2D || desc.mipLevels != 1
            || !(desc.usage & TEXTURE_RENDER_TARGET)))
    {
        return reason("multisampled textures must be 2D render targets with one mip level");
    }
    if (desc.samples > 1
        && (desc.samples > caps.maxSamples || (desc.samples & (desc.samples - 1)) != 0))
    {
        return reason(desc.samples, " samples are not a power of two up to ", caps.maxSamples);
    }
    if (isCompressed(desc.format) && !caps.bcCompression)
    {
        return reason("BC compression is not supported");
    }
    if (isCompressed(desc.format) && (desc.usage & TEXTURE_RENDER_TARGET))
    {
        return reason("compressed formats cannot be render targets");
    }
    if (!desc.swizzle.isIdentity() && !caps.textureSwizzle)
    {
        return reason("texture swizzle is not supported");
    }
    return {};
}

std::string validate(const ProgramDesc& desc, const Capabilities& caps)
{
    bool vertex = false;
    bool fragment = false;
    bool geometry = false;
    for (const StageSource& stage : desc.stages)
    {
        bool& seen = stage.stage == ShaderStage::Vertex ? vertex
                   : stage.stage == ShaderStage::Fragment ? fragment : geometry;
        if (seen)
        {
            return reason("a stage appears twice");
        }
        seen = true;
        if (stage.units.empty())
        {
            return reason("a stage has no source");
        }
    }
    if (!vertex || !fragment)
    {
        return reason("a program needs a vertex and a fragment stage");
    }
    if (geometry && !caps.geometryShaders)
    {
        return reason("geometry shaders are not supported");
    }
    if ((desc.attributeMask >> VertexStreams::MAX_ATTRIBUTES) != 0)
    {
        return reason("the attribute mask has unknown attributes");
    }
    return {};
}

std::string validate(const PipelineDesc& desc, const Capabilities& caps)
{
    if (desc.colorCount > PipelineDesc::MAX_COLOR_ATTACHMENTS
        || desc.colorCount > caps.maxColorAttachments)
    {
        return reason(desc.colorCount, " color attachments are more than the maximum ",
                      std::min(PipelineDesc::MAX_COLOR_ATTACHMENTS, caps.maxColorAttachments));
    }
    if (desc.colorCount == 0 && desc.depthFormat == Format::Unknown)
    {
        return reason("the pipeline has no attachments");
    }
    for (U32 i = 0; i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
    {
        const Format format = desc.colorFormats[i];
        if (i >= desc.colorCount)
        {
            if (format != Format::Unknown)
            {
                return reason("color attachment ", i, " is not used but has a format");
            }
        }
        else if (format == Format::Unknown || format >= Format::Count)
        {
            return reason("color attachment ", i, " has no format");
        }
        else if (isDepth(format) || isCompressed(format))
        {
            return reason("color attachment ", i, " has a depth or compressed format");
        }
    }
    if (desc.depthFormat != Format::Unknown && !isDepth(desc.depthFormat))
    {
        return reason("the depth attachment's format is not a depth format");
    }
    if (desc.samples == 0 || (desc.samples & (desc.samples - 1)) != 0
        || (desc.samples > 1 && desc.samples > caps.maxSamples))
    {
        return reason(desc.samples, " samples are not a power of two up to ", caps.maxSamples);
    }
    if ((desc.attributeMask >> VertexStreams::MAX_ATTRIBUTES) != 0)
    {
        return reason("the attribute mask has unknown attributes");
    }
    if ((desc.colorMask & ~COLOR_MASK_ALL) != 0)
    {
        return reason("the color mask has unknown bits");
    }
    return {};
}

std::string validateUniform(const ProgramReflection& reflection, U32 uniformIndex, size_t bytes,
                            U32 count)
{
    if (uniformIndex >= reflection.uniforms.size())
    {
        return reason("uniform ", uniformIndex, " does not exist; the program has ",
                      reflection.uniforms.size());
    }
    const UniformInfo& uniform = reflection.uniforms[uniformIndex];
    if (isSampler(uniform.type))
    {
        return reason("'", uniform.name, "' is a sampler; set its channel instead");
    }
    if (count == 0 || count > uniform.arraySize)
    {
        return reason(count, " values for '", uniform.name, "', which has ", uniform.arraySize);
    }
    const size_t expected = size_t(uniformSize(uniform.type)) * count;
    if (bytes != expected)
    {
        return reason(bytes, " bytes for '", uniform.name, "', which takes ", expected);
    }
    return {};
}

namespace
{
    U64 layerCount(const TextureDesc& desc)
    {
        switch (desc.type)
        {
        case TextureType::Cube:
            return 6;
        case TextureType::CubeArray:
            return U64(desc.layers) * 6;
        case TextureType::Tex3D: // the depth's slices
        case TextureType::Tex2D:
            break;
        }
        return desc.layers;
    }

    U32 levelSize(U32 size, U32 level)
    {
        return level < 32 ? std::max(1u, size >> level) : 1u;
    }

    const char* className(PrimitiveClass type)
    {
        switch (type)
        {
        case PrimitiveClass::Point:
            return "point";
        case PrimitiveClass::Line:
            return "line";
        case PrimitiveClass::Triangle:
            break;
        }
        return "triangle";
    }

    // "color attachment 2" or "the depth attachment", built only for a reason.
    struct AttachmentName
    {
        S32 index; ///< -1 for the depth attachment
    };

    std::ostream& operator<<(std::ostream& out, AttachmentName name)
    {
        if (name.index < 0)
        {
            return out << "the depth attachment";
        }
        return out << "color attachment " << name.index;
    }

    // The checks every attachment of a pass shares.
    std::string checkAttachment(AttachmentName what, const Ref<Texture>& texture, U32 level,
                                U32 layer)
    {
        if (texture.isNull())
        {
            return reason(what, " has no texture");
        }
        const TextureDesc& desc = texture->desc();
        if (!(desc.usage & TEXTURE_RENDER_TARGET))
        {
            return reason(what, " '", texture->debugName(), "' is not a render target");
        }
        if (level >= desc.mipLevels)
        {
            return reason(what, " uses level ", level, " of ", desc.mipLevels);
        }
        if (layer >= layerCount(desc))
        {
            return reason(what, " uses layer ", layer, " of ", layerCount(desc));
        }
        return {};
    }

    // The size and samples a pass draws with, from its first attachment.
    struct PassShape
    {
        U32 width = 0;
        U32 height = 0;
        U32 samples = 0;
        bool known = false;
    };

    PassShape passShape(const RenderPassDesc& pass)
    {
        PassShape shape;
        auto take = [&shape](const Ref<Texture>& texture, U32 level)
        {
            if (!shape.known && texture.notNull())
            {
                shape.width = levelSize(texture->desc().width, level);
                shape.height = levelSize(texture->desc().height, level);
                shape.samples = texture->desc().samples;
                shape.known = true;
            }
        };
        for (U32 i = 0; i < pass.colorCount && i < pass.color.size(); ++i)
        {
            take(pass.color[i].texture, pass.color[i].level);
        }
        take(pass.depth.texture, pass.depth.level);
        return shape;
    }
}

std::string validate(const RenderPassDesc& desc)
{
    if (desc.colorCount > PipelineDesc::MAX_COLOR_ATTACHMENTS)
    {
        return reason(desc.colorCount, " color attachments are more than the maximum ",
                      PipelineDesc::MAX_COLOR_ATTACHMENTS);
    }
    if (desc.colorCount == 0 && desc.depth.texture.isNull())
    {
        return reason("the pass has no attachments");
    }
    const PassShape shape = passShape(desc);
    auto sameShape = [&shape](AttachmentName what, const Ref<Texture>& texture, U32 level)
    {
        const TextureDesc& td = texture->desc();
        if (levelSize(td.width, level) != shape.width || levelSize(td.height, level) != shape.height)
        {
            return reason(what, " is ", levelSize(td.width, level), "x", levelSize(td.height, level),
                          ", the pass ", shape.width, "x", shape.height);
        }
        if (td.samples != shape.samples)
        {
            return reason(what, " has ", td.samples, " samples, the pass ", shape.samples);
        }
        return std::string();
    };

    for (U32 i = 0; i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
    {
        const ColorAttachment& color = desc.color[i];
        const AttachmentName what{ S32(i) };
        if (i >= desc.colorCount)
        {
            if (color.texture.notNull())
            {
                return reason(what, " is not used but has a texture");
            }
            continue;
        }
        std::string why = checkAttachment(what, color.texture, color.level, color.layer);
        if (why.empty() && (isDepth(color.texture->desc().format)
                            || isCompressed(color.texture->desc().format)))
        {
            why = reason(what, " has a depth or compressed format");
        }
        if (why.empty())
        {
            why = sameShape(what, color.texture, color.level);
        }
        if (!why.empty())
        {
            return why;
        }
    }

    const DepthAttachment& depth = desc.depth;
    if (depth.texture.isNull())
    {
        return depth.readOnly ? reason("a read-only depth needs a depth attachment") : std::string();
    }
    if (depth.readOnly && depth.load == LoadAction::Clear)
    {
        return reason("a read-only depth cannot be cleared");
    }
    const AttachmentName what{ -1 };
    std::string why = checkAttachment(what, depth.texture, depth.level, depth.layer);
    if (why.empty() && !isDepth(depth.texture->desc().format))
    {
        why = reason("the depth attachment's format is not a depth format");
    }
    if (why.empty())
    {
        why = sameShape(what, depth.texture, depth.level);
    }
    return why;
}

std::string validate(const SamplerDesc& desc, const Capabilities& caps)
{
    const F32 anisotropy = desc.maxAnisotropy;
    // [Viewer Lab] On the bits: NaN and infinity have every exponent bit set, which fast floating point (MSVC's /fp:fast)
    // cannot change, as it may std::isfinite and the compares below
    if ((std::bit_cast<U32>(anisotropy) & 0x7f800000u) == 0x7f800000u)
    {
        return reason("the maximum anisotropy ", anisotropy, " is not finite");
    }
    if (anisotropy < 1.f)
    {
        return reason("the maximum anisotropy ", anisotropy, " is below 1");
    }
    if (anisotropy > 1.f && !caps.anisotropicFiltering)
    {
        return reason("the maximum anisotropy ", anisotropy, " needs anisotropic filtering, which is not supported");
    }
    if (anisotropy > std::max(1.f, caps.maxAnisotropy))
    {
        return reason("the maximum anisotropy ", anisotropy, " is above the device's ", std::max(1.f, caps.maxAnisotropy));
    }
    if (U8(desc.magFilter) > U8(MagFilter::Linear)) // [Viewer Lab]
    {
        return reason("the mag filter ", U32(desc.magFilter), " is not a MagFilter");
    }
    return {};
}

SamplerDesc samplerForDraw(const SamplerDesc& bound, UniformType samplerType, Format format)
{
    SamplerDesc sampler = bound;
    sampler.compare = samplerType == UniformType::Sampler2DShadow;
    if (isInteger(format))
    {
        sampler.filter = Filter::Point; // GL samples an integer texture with a linear filter as incomplete
        sampler.magFilter = MagFilter::Nearest; // [Viewer Lab] An explicit Linear never reaches an integer texture
    }
    return sampler;
}

std::string checkPipelineForPass(const PipelineDesc& pipeline, const RenderPassDesc& pass)
{
    if (pipeline.colorCount != pass.colorCount)
    {
        return reason("the pipeline has ", pipeline.colorCount, " color attachments, the pass ",
                      pass.colorCount);
    }
    for (U32 i = 0; i < pass.colorCount && i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
    {
        const Format format = pass.color[i].texture.notNull() ? pass.color[i].texture->desc().format
                                                              : Format::Unknown;
        if (pipeline.colorFormats[i] != format)
        {
            return reason("color attachment ", i, " has another format than the pipeline's");
        }
    }
    const Format depth = pass.depth.texture.notNull() ? pass.depth.texture->desc().format
                                                      : Format::Unknown;
    if (pipeline.depthFormat != depth)
    {
        return reason("the depth attachment has another format than the pipeline's");
    }
    const PassShape shape = passShape(pass);
    if (shape.known && pipeline.samples != shape.samples)
    {
        return reason("the pipeline has ", pipeline.samples, " samples, the pass ", shape.samples);
    }
    return {};
}

PassFormats passFormatsOf(const RenderPassDesc& pass)
{
    PassFormats formats;
    formats.colorCount = pass.colorCount;
    for (U32 i = 0; i < pass.colorCount && i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
    {
        formats.colorFormats[i] = pass.color[i].texture.notNull() ? pass.color[i].texture->desc().format
                                                                  : Format::Unknown;
    }
    formats.depthFormat = pass.depth.texture.notNull() ? pass.depth.texture->desc().format
                                                       : Format::Unknown;
    const PassShape shape = passShape(pass);
    formats.samples = shape.known ? shape.samples : 0;
    return formats;
}

void applyPassFormats(const PassFormats& formats, PipelineDesc& pipeline)
{
    pipeline.colorCount = formats.colorCount;
    pipeline.colorFormats = formats.colorFormats;
    pipeline.depthFormat = formats.depthFormat;
    pipeline.samples = formats.samples != 0 ? formats.samples : 1;
}

std::string validateRange(const BufferDesc& desc, U64 offset, U64 size)
{
    if (size == 0)
    {
        return reason("the range is empty");
    }
    if (offset > desc.size || size > desc.size - offset)
    {
        return reason("bytes ", offset, " to ", offset + size, " are beyond the buffer's ",
                      desc.size);
    }
    return {};
}

std::string validateRegion(const TextureDesc& desc, U32 level, U32 layer, const Region& region)
{
    if (level >= desc.mipLevels)
    {
        return reason("level ", level, " is beyond the texture's ", desc.mipLevels);
    }
    if (layer >= layerCount(desc))
    {
        return reason("layer ", layer, " is beyond the texture's ", layerCount(desc));
    }
    const U32 width = levelSize(desc.width, level);
    const U32 height = levelSize(desc.height, level);
    if (region.width == 0 || region.height == 0)
    {
        return reason("the region is empty");
    }
    if (region.x > width || region.width > width - region.x || region.y > height
        || region.height > height - region.y)
    {
        return reason("the region ", region.x, ",", region.y, " ", region.width, "x",
                      region.height, " is beyond level ", level, "'s ", width, "x", height);
    }
    return {};
}

std::string validateUpload(const TextureDesc& desc, U32 level, U32 layer, const Region& region,
                           const PixelLayout& layout, size_t bytes)
{
    std::string why = validateRegion(desc, level, layer, region);
    if (!why.empty())
    {
        return why;
    }
    U64 needed = 0;
    if (const U32 block = blockBytes(desc.format))
    {
        // Whole blocks: the region starts on a block and ends on one or at the level's edge.
        const U32 width = levelSize(desc.width, level);
        const U32 height = levelSize(desc.height, level);
        const bool alignedX = region.x % 4 == 0 && (region.width % 4 == 0 || region.x + region.width == width);
        const bool alignedY = region.y % 4 == 0 && (region.height % 4 == 0 || region.y + region.height == height);
        if (!alignedX || !alignedY)
        {
            return reason("the region is not on whole 4x4 blocks");
        }
        needed = U64((region.width + 3) / 4) * ((region.height + 3) / 4) * block;
    }
    else
    {
        const U32 rowLength = layout.rowLength != 0 ? layout.rowLength : region.width;
        if (rowLength < region.width)
        {
            return reason("the row length ", rowLength, " is less than the width ", region.width);
        }
        needed = (U64(rowLength) * (region.height - 1) + region.width) * layout.bytesPerPixel();
    }
    if (bytes < needed)
    {
        return reason(bytes, " bytes of data for a region that needs ", needed);
    }
    return {};
}

std::string validateMipmaps(const TextureDesc& desc)
{
    if (isCompressed(desc.format) || isDepth(desc.format) || isInteger(desc.format))
    {
        return reason("mipmaps cannot be generated for a compressed, depth or integer format");
    }
    return {};
}

std::string copyRefusal(const Capabilities& caps, Format from, Format to)
{
    if (from == to)
    {
        return {};
    }
    if (!caps.copyBetweenFormats)
    {
        return reason("the device copies only within a format");
    }
    const auto plain = [](Format format)
    {
        // BGRA8Unorm: validate refuses it as a texture, and no format copy is made into or from it
        return format != Format::Unknown && format < Format::Count && format != Format::BGRA8Unorm
               && !isCompressed(format) && !isDepth(format) && !isInteger(format) && !isSRGB(format);
    };
    if (caps.copyBetweenFormatsPlainColorOnly && (!plain(from) || !plain(to)))
    {
        return reason("the device copies between plain colour formats only, not sRGB, block-compressed, depth or "
                      "integer ones");
    }
    return {};
}

std::string Device::copyRefusalBetween(Format from, Format to)
{
    return copyRefusal(capabilities(), from, to);
}

std::string validateQuery(QueryType type, const Capabilities& caps)
{
    if (type == QueryType::TimeElapsed && !caps.timerQueries)
    {
        return reason("timer queries are not supported");
    }
    if (type == QueryType::PrimitivesGenerated && !caps.primitivesGeneratedQuery)
    {
        return reason("primitive queries are not supported");
    }
    return {};
}

PipelineDesc normalized(const PipelineDesc& desc)
{
    PipelineDesc key = desc;
    if (!key.blend.enabled || key.colorCount == 0)
    {
        key.blend = BlendState();
    }
    if (key.colorCount == 0)
    {
        key.colorMask = COLOR_MASK_ALL;
    }
    return key;
}

std::string nativePipelineKey(const NativeRenderFunctions& functions, const PipelineDesc& desc)
{
    const PipelineDesc key = normalized(desc);
    std::ostringstream text;
    text << functions.vertex << "/" << functions.fragment << "/" << key.samples << "/" << U32(key.primitiveClass) << "/"
         << key.colorCount << "/" << U32(key.depthFormat) << "/" << U32(key.colorMask) << "/" << key.blend.enabled
         << U32(key.blend.srcColor) << U32(key.blend.dstColor) << U32(key.blend.srcAlpha) << U32(key.blend.dstAlpha)
         << U32(key.blend.colorOp) << U32(key.blend.alphaOp);
    for (U32 i = 0; i < key.colorCount && i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
    {
        text << "/" << U32(key.colorFormats[i]);
    }
    return text.str();
}

U32 fullMipCount(U32 width, U32 height)
{
    U32 size = std::max(width, height);
    U32 levels = 0;
    while (size > 0)
    {
        ++levels;
        size >>= 1;
    }
    return levels;
}

void warnCreateFailed(std::string_view kind, std::string_view debugName, std::string_view reason)
{
    LL_WARNS("RHI") << "Could not create " << kind << " '"
                    << (debugName.empty() ? std::string_view("unnamed") : debugName)
                    << "': " << reason << LL_ENDL;
}

U64 textureBytes(const TextureDesc& desc)
{
    U32 texel = 0;
    switch (desc.format)
    {
    case Format::R8Unorm:
    case Format::R8Uint:
        texel = 1;
        break;
    case Format::RG8Unorm:
    case Format::R16Float:
    case Format::R16Uint:
        texel = 2;
        break;
    case Format::RGBA8Unorm:
    case Format::RGBA8Unorm_sRGB:
    case Format::BGRA8Unorm:
    case Format::RG16Float:
    case Format::R32Float:
    case Format::R32Uint:
    case Format::RGB10A2Unorm:
    case Format::RG11B10Float:
    case Format::Depth32Float:
        texel = 4;
        break;
    case Format::Depth32Float_Stencil8: // Apple stores the stencil apart; counted as 8 bytes
    case Format::RGBA16Float:
    case Format::RGBA16Unorm:
    case Format::RG32Float:
        texel = 8;
        break;
    case Format::RGBA32Float:
        texel = 16;
        break;
    default:
        break;
    }
    const U64 slices = desc.type == TextureType::Cube        ? 6
                       : desc.type == TextureType::CubeArray ? U64(desc.layers) * 6
                                                             : U64(desc.layers);
    U64 bytes = 0;
    for (U32 level = 0; level < desc.mipLevels; ++level)
    {
        const U64 width = std::max(1u, desc.width >> std::min(level, 31u));
        const U64 height = std::max(1u, desc.height >> std::min(level, 31u));
        if (isCompressed(desc.format))
        {
            bytes += ((width + 3) / 4) * ((height + 3) / 4) * blockBytes(desc.format);
        }
        else
        {
            bytes += width * height * texel;
        }
    }
    return bytes * slices * std::max(1u, desc.samples);
}

// Native programs, compute and transient pools: the defaults ------------------------------------------
//
// A backend without native programs (GL) overrides none of these, so each call is refused with one WARN line and
// changes nothing.

namespace
{
    void refuseNative(std::string_view call)
    {
        LL_WARNS("RHI") << call << ": native programs and compute passes need Metal" << LL_ENDL;
    }

    /// A library that never compiles, for a device without native programs.
    class RefusedNativeLibrary final : public NativeLibrary
    {
    public:
        CompileStatus status() const override { return CompileStatus::Failed; }
        void wait() override {}
        std::string log() const override { return "native programs need Metal"; }
    };

    /// The pool's textures as textures of their own: heapBytes is the sum of their sizes.
    class DedicatedTransientPool final : public TransientPool
    {
    public:
        DedicatedTransientPool(const TransientPoolDesc& desc, std::vector<Ref<Texture>> textures)
        :   mDesc(desc), mTextures(std::move(textures))
        {
            for (const TransientTextureDesc& texture : mDesc.textures)
            {
                mDedicated += textureBytes(texture.desc);
            }
        }

        Texture* texture(U32 index) const override
        {
            return index < mTextures.size() ? mTextures[index].get() : nullptr;
        }
        U32 textureCount() const override { return U32(mTextures.size()); }
        const TransientPoolDesc& desc() const override { return mDesc; }
        U64 heapBytes() const override { return mDedicated; }
        U64 dedicatedBytes() const override { return mDedicated; }

    private:
        const TransientPoolDesc mDesc;
        std::vector<Ref<Texture>> mTextures;
        U64 mDedicated = 0;
    };
}

Ref<NativeLibrary> Device::createNativeLibrary(const NativeSource& source)
{
    LL_WARNS("RHI") << "createNativeLibrary '" << source.name << "': native programs need Metal" << LL_ENDL;
    return new RefusedNativeLibrary();
}

Ref<ComputePipeline> Device::getComputePipeline(NativeLibrary& /*library*/, const ComputePipelineDesc& desc)
{
    LL_WARNS("RHI") << "getComputePipeline '" << desc.function << "': native programs need Metal" << LL_ENDL;
    return nullptr;
}

Ref<Pipeline> Device::getNativePipeline(NativeLibrary& /*library*/, const NativeRenderFunctions& functions,
                                        const PipelineDesc& /*desc*/)
{
    LL_WARNS("RHI") << "getNativePipeline '" << functions.fragment << "': native programs need Metal" << LL_ENDL;
    return nullptr;
}

Ref<TransientPool> Device::createTransientPool(const TransientPoolDesc& desc)
{
    if (!capabilities().computePasses)
    {
        LL_WARNS("RHI") << "createTransientPool '" << desc.name << "': transient pools need Metal" << LL_ENDL;
        return nullptr;
    }
    std::vector<Ref<Texture>> textures;
    textures.reserve(desc.textures.size());
    for (const TransientTextureDesc& texture : desc.textures)
    {
        // A failed texture stays null in its place, after createTexture's WARN line, so indices hold.
        std::string why;
        if (texture.lastPhase < texture.firstPhase)
        {
            why = reason("its last phase ", texture.lastPhase, " is before its first ", texture.firstPhase);
            warnCreateFailed("transient texture", texture.name, why);
            textures.emplace_back();
            continue;
        }
        textures.push_back(createTexture(texture.desc, texture.name));
    }
    Ref<TransientPool> pool = new DedicatedTransientPool(desc, std::move(textures));
    pool->setDebugName(desc.name);
    return pool;
}

bool Encoder::beginComputePass(const ComputePassDesc& /*pass*/)
{
    refuseNative("beginComputePass");
    return false;
}

void Encoder::endComputePass()
{
    refuseNative("endComputePass");
}

void Encoder::setComputePipeline(ComputePipeline* /*pipeline*/)
{
    refuseNative("setComputePipeline");
}

void Encoder::setThreadgroupMemory(U32 /*index*/, U32 /*bytes*/)
{
    refuseNative("setThreadgroupMemory");
}

void Encoder::dispatchThreads(DispatchSize /*grid*/, DispatchSize /*threadsPerGroup*/)
{
    refuseNative("dispatchThreads");
}

void Encoder::dispatchThreadgroups(DispatchSize /*groups*/, DispatchSize /*threadsPerGroup*/)
{
    refuseNative("dispatchThreadgroups");
}

void Encoder::setNativeTexture(NativeStage /*stage*/, U32 /*slot*/, Texture* /*texture*/, Access /*access*/,
                               TextureRange /*range*/)
{
    refuseNative("setNativeTexture");
}

void Encoder::setNativeSampler(NativeStage /*stage*/, U32 /*slot*/, const SamplerDesc& /*sampling*/)
{
    refuseNative("setNativeSampler");
}

void Encoder::setNativeBytes(NativeStage /*stage*/, U32 /*slot*/, std::span<const std::byte> /*data*/)
{
    refuseNative("setNativeBytes");
}

void Encoder::setNativeBuffer(NativeStage /*stage*/, U32 /*slot*/, Buffer* /*buffer*/, U64 /*offset*/,
                              Access /*access*/)
{
    refuseNative("setNativeBuffer");
}

// Frames and deletion ----------------------------------------------------------------------------

FrameLifecycle::FrameLifecycle(WaitForFrame waitForFrame)
:   mWaitForFrame(std::move(waitForFrame))
{
    llassert(mWaitForFrame);
}

FrameLifecycle::~FrameLifecycle()
{
    // The device frees everything with releaseAll() before it goes. What is left here would touch a
    // device that is half destroyed, so it is not freed, only reported.
    const size_t left = pendingReleases();
    if (left > 0)
    {
        LL_WARNS("RHI") << left << " releases were never freed; the device must call releaseAll()"
                        << " at shutdown" << LL_ENDL;
    }
}

void FrameLifecycle::endFrame()
{
    if (mLost.load())
    {
        return;
    }
    const U64 next = mCurrentFrame.load() + 1;
    if (next > MAX_FRAMES_IN_FLIGHT)
    {
        const U64 mustFinish = next - MAX_FRAMES_IN_FLIGHT;
        if (mCompletedFrame.load() < mustFinish)
        {
            // A frame unfinished after two of the backend's waits (10 s on Metal)
            // makes the device lost. Nothing is completed or collected then, because the GPU may still read what
            // the frames released; the viewer sees deviceLost() after the frame and stops. GL's wait is always true.
            if (!mWaitForFrame(mustFinish) && !mWaitForFrame(mustFinish))
            {
                LL_WARNS("RHI") << "Frame " << mustFinish << " did not finish on the GPU within two waits; the device"
                                << " is lost" << LL_ENDL;
                mLost.store(true);
                return;
            }
            // The wait can return before the backend's own report of the frame arrives.
            frameCompleted(mustFinish);
        }
    }
    mCurrentFrame.store(next);
    collect();
}

void FrameLifecycle::frameCompleted(U64 frame)
{
    // A frame that was just sent can finish before endFrame() starts the next, but never one that
    // has not been sent.
    llassert(frame <= mCurrentFrame.load());
    // Reports can arrive out of order from other threads; keep the highest.
    U64 completed = mCompletedFrame.load();
    while (frame > completed && !mCompletedFrame.compare_exchange_weak(completed, frame))
    {
    }
}

void FrameLifecycle::idle(bool freeReleased)
{
    if (freeReleased)
    {
        mWaits.fetch_add(1);
    }
    frameCompleted(mCurrentFrame.load() - 1);
    collect();
}

void FrameLifecycle::release(std::function<void()> freeNative, std::vector<Ref<Fence>> uploads)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mReleases.push_back({ mCurrentFrame.load(), mWaits.load(), std::move(freeNative), std::move(uploads) });
}

size_t FrameLifecycle::collect()
{
    std::vector<Release> finished;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const U64 completed = mCompletedFrame.load();
        const U64 waits = mWaits.load();
        auto split = std::stable_partition(mReleases.begin(), mReleases.end(),
                                           [completed, waits](const Release& release)
                                           { return release.frame > completed && release.waits >= waits; });
        std::move(split, mReleases.end(), std::back_inserter(finished));
        mReleases.erase(split, mReleases.end());
    }

    // Outside the lock: a backend's fence may take a lock of its own that another thread holds
    // while it releases an object, and freeing one object may release others.
    std::vector<Release> waiting;
    size_t freed = 0;
    for (Release& release : finished)
    {
        const bool uploaded = std::all_of(release.uploads.begin(), release.uploads.end(),
                                          [](const Ref<Fence>& fence)
                                          { return fence.isNull() || fence->signaled(); });
        if (uploaded)
        {
            release.freeNative();
            ++freed;
        }
        else
        {
            waiting.push_back(std::move(release));
        }
    }

    if (!waiting.empty())
    {
        std::lock_guard<std::mutex> lock(mMutex);
        std::move(waiting.begin(), waiting.end(), std::back_inserter(mReleases));
    }
    return freed;
}

size_t FrameLifecycle::releaseAll()
{
    size_t count = 0;
    // Freeing may release more, so repeat until nothing is left.
    while (true)
    {
        std::vector<Release> all;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            all.swap(mReleases);
        }
        if (all.empty())
        {
            return count;
        }
        for (Release& release : all)
        {
            release.freeNative();
        }
        count += all.size();
    }
}

size_t FrameLifecycle::pendingReleases() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mReleases.size();
}

// Pipelines --------------------------------------------------------------------------------------

Ref<Pipeline> PipelineCache::get(const PipelineKey& wanted, const Create& create)
{
    const PipelineKey key{ wanted.programId, normalized(wanted.desc) };
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto found = mPipelines.find(key);
        if (found != mPipelines.end())
        {
            return found->second;
        }
    }

    Ref<Pipeline> made = create(key);
    if (made.isNull())
    {
        return made;
    }

    Ref<Pipeline> cached;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        cached = mPipelines.emplace(key, made).first->second;
    }
    // When another thread cached the same pipeline first, ours is released here, outside the lock.
    return cached;
}

size_t PipelineCache::forgetProgram(U64 programId)
{
    std::vector<Ref<Pipeline>> leaving;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto it = mPipelines.begin(); it != mPipelines.end();)
        {
            if (it->first.programId == programId)
            {
                leaving.push_back(std::move(it->second));
                it = mPipelines.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    // Released outside the lock: a pipeline's destructor hands its native objects to the lifecycle.
    return leaving.size();
}

void PipelineCache::clear()
{
    std::unordered_map<PipelineKey, Ref<Pipeline>, PipelineKeyHash> leaving;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        leaving.swap(mPipelines);
    }
}

size_t PipelineCache::size() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mPipelines.size();
}

// The encoder's tracker -------------------------------------------------------------------------

namespace
{
    // Queries of one class cannot overlap: a Metal render encoder has one visibility-result mode,
    // and GL allows one active query per target. From GL 3.3 "at least one sample" has a target of
    // its own (Capabilities::concurrentSampleQueries), so there SamplesPassed is a class of its own:
    // the tracker allows the overlap and leaves it to GL, as today's raw calls do, and Apple's GL 4.1
    // refuses it (llrhigl_test's 'LLRHI GL queries' test<1>, p1). Otherwise SamplesPassed shares
    // AnySamplesPassed's class.
    int queryClass(QueryType type, bool concurrentSamples)
    {
        switch (type)
        {
        case QueryType::AnySamplesPassed:
            return 0;
        case QueryType::SamplesPassed:
            return concurrentSamples ? 3 : 0;
        case QueryType::TimeElapsed:
            return 1;
        case QueryType::PrimitivesGenerated:
            break;
        }
        return 2;
    }
}

EncoderTracker::EncoderTracker(const Capabilities& caps, bool checks)
:   mMaxChannels(caps.maxSamplersPerStage),
    mTimerQueries(caps.timerQueries),
    mPrimitivesQueries(caps.primitivesGeneratedQuery),
    mConcurrentSampleQueries(caps.concurrentSampleQueries),
    mChecks(checks),
    mLineWidthRange{ caps.lineWidthRange[0], caps.lineWidthRange[1] }
{
}

std::string EncoderTracker::beginRenderPass(const RenderPassDesc& pass)
{
    if (mInCompute)
    {
        return reason("compute pass '", mCompute.name, "' is still open");
    }
    // A native pass sets an open stack pass aside; any other pass in a pass is refused.
    const bool suspends = mInPass && pass.native && mPass.stack && !mSuspended;
    if (mInPass && !suspends)
    {
        return reason("pass '", mPass.name, "' is still open");
    }
    if (mChecks)
    {
        std::string why = validate(pass);
        if (!why.empty())
        {
            return why;
        }
    }
    if (suspends)
    {
        suspendStackPass();
    }
    mInPass = true;
    mPass = pass;
    mGroupsAtPassStart = mGroups;
    ++mPassSerial;
    mDynamic = DynamicState(); // every pass begins with the defaults
    return {};
}

U64 EncoderTracker::passFormatsSerial() const
{
    if (!mInPass)
    {
        return 0;
    }
    updatePassFormats();
    return mFormatsSerial;
}

const PassFormats& EncoderTracker::passFormats() const
{
    static const PassFormats none;
    if (!mInPass)
    {
        return none;
    }
    updatePassFormats();
    return mFormats;
}

void EncoderTracker::updatePassFormats() const
{
    if (mFormatsPassSerial == mPassSerial)
    {
        return;
    }
    mFormatsPassSerial = mPassSerial;
    const PassFormats formats = passFormatsOf(mPass);
    if (mFormatsSerial == 0 || formats != mFormats)
    {
        mFormats = formats;
        ++mFormatsSerial;
    }
}

EncoderTracker::Closing EncoderTracker::endRenderPass()
{
    Closing closing;
    if (!mInPass)
    {
        closing.warning = reason("no pass is open");
        return closing;
    }
    closing.endPass = true;
    // A stack pass delimits no debug group: its groups stay open across its end.
    if (!mPass.stack && mGroups > mGroupsAtPassStart)
    {
        closing.popInPass = mGroups - mGroupsAtPassStart;
        closing.warning = reason("pass '", mPass.name, "' ended with ", closing.popInPass,
                                 " debug groups open");
        mGroups = mGroupsAtPassStart;
    }
    // The attachments go with the pass; what nobody else holds is released in this frame.
    const bool native = mPass.native;
    mPass = RenderPassDesc();
    mInPass = false;
    mDynamic = DynamicState(); // the pass's dynamic state ends with it
    if (native)
    {
        closing.resumed = endNativeScope();
    }
    return closing;
}

void EncoderTracker::suspendStackPass()
{
    Suspended suspended;
    suspended.pass = mPass;
    suspended.dynamic = mDynamic;
    suspended.bindings = drawBindings();
    suspended.groupsAtPassStart = mGroupsAtPassStart;
    mSuspended = std::move(suspended);
    mPass = RenderPassDesc();
    mInPass = false;
    mDynamic = DynamicState();
}

bool EncoderTracker::endNativeScope()
{
    for (NativeStageBindings& stage : mNative)
    {
        stage = NativeStageBindings();
    }
    mWrittenSinceBarrier.clear();
    mReadSinceBarrier.clear();
    if (!mSuspended)
    {
        return false;
    }
    // The stack pass goes on in a new pass: what it had recorded was stored, so its attachments load.
    Suspended suspended = std::move(*mSuspended);
    mSuspended.reset();
    for (U32 i = 0; i < suspended.pass.colorCount && i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
    {
        suspended.pass.color[i].load = LoadAction::Load;
    }
    suspended.pass.depth.load = LoadAction::Load;
    mPass = std::move(suspended.pass);
    mInPass = true;
    ++mPassSerial;
    mDynamic = suspended.dynamic;
    restoreDrawBindings(std::move(suspended.bindings));
    mGroupsAtPassStart = suspended.groupsAtPassStart;
    return true;
}

std::string EncoderTracker::outsideCompute() const
{
    return mInCompute ? reason("compute pass '", mCompute.name, "' is open") : std::string();
}

std::string EncoderTracker::nativeStageOpen(NativeStage stage, U32 slot, U32 slots) const
{
    if (stage == NativeStage::Compute ? !mInCompute : !(mInPass && mPass.native))
    {
        return stage == NativeStage::Compute ? reason("no compute pass is open")
                                             : reason("no native render pass is open");
    }
    if (slot >= slots)
    {
        return reason("slot ", slot, " is past the ", slots, " native slots");
    }
    return {};
}

std::string EncoderTracker::beginComputePass(const ComputePassDesc& pass)
{
    if (mInCompute)
    {
        return reason("compute pass '", mCompute.name, "' is still open");
    }
    if (mInPass && (!mPass.stack || mSuspended))
    {
        return reason("pass '", mPass.name, "' is open, and only a stack pass can be suspended for a compute pass");
    }
    if (mInPass)
    {
        suspendStackPass();
    }
    mInCompute = true;
    mCompute = pass;
    mComputePipeline = nullptr;
    mGroupsAtComputeStart = mGroups;
    mWrittenSinceBarrier.clear();
    mReadSinceBarrier.clear();
    return {};
}

EncoderTracker::Closing EncoderTracker::endComputePass()
{
    Closing closing;
    if (!mInCompute)
    {
        closing.warning = reason("no compute pass is open");
        return closing;
    }
    closing.endCompute = true;
    // Debug groups opened in the compute pass end with it, as a render pass's do.
    if (mGroups > mGroupsAtComputeStart)
    {
        closing.popInPass = mGroups - mGroupsAtComputeStart;
        closing.warning = reason("compute pass '", mCompute.name, "' ended with ", closing.popInPass,
                                 " debug groups open");
        mGroups = mGroupsAtComputeStart;
    }
    mInCompute = false;
    mCompute = ComputePassDesc();
    mComputePipeline = nullptr;
    closing.resumed = endNativeScope();
    return closing;
}

std::string EncoderTracker::setComputePipeline(ComputePipeline* pipeline)
{
    if (!mInCompute)
    {
        return reason("no compute pass is open");
    }
    if (pipeline && pipeline->status() == CompileStatus::Failed)
    {
        mComputePipeline = nullptr;
        return reason("the compute pipeline failed to compile");
    }
    mComputePipeline = pipeline;
    return {};
}

std::string EncoderTracker::setThreadgroupMemory(U32 index, U32 bytes)
{
    if (!mInCompute)
    {
        return reason("no compute pass is open");
    }
    if (index >= NATIVE_THREADGROUP_SLOTS)
    {
        return reason("threadgroup memory ", index, " is past the ", NATIVE_THREADGROUP_SLOTS, " slots");
    }
    // Metal requires a multiple of 16 whatever the checks say.
    if ((bytes % 16) != 0)
    {
        return reason("threadgroup memory of ", bytes, " bytes is not a multiple of 16");
    }
    return {};
}

std::string EncoderTracker::dispatch(DispatchSize size, DispatchSize threadsPerGroup, bool& barrier)
{
    barrier = false;
    if (!mInCompute)
    {
        return reason("no compute pass is open");
    }
    if (mComputePipeline.isNull())
    {
        return reason("no compute pipeline is set");
    }
    if (mComputePipeline->status() == CompileStatus::Failed)
    {
        return reason("the compute pipeline failed to compile");
    }
    if (size.count() == 0 || threadsPerGroup.count() == 0)
    {
        return reason("an empty dispatch (", size.x, "x", size.y, "x", size.z, " of ", threadsPerGroup.x, "x",
                      threadsPerGroup.y, "x", threadsPerGroup.z, ")");
    }
    if (threadsPerGroup.count() > NATIVE_THREADS_PER_GROUP_MAX)
    {
        return reason(threadsPerGroup.count(), " threads per threadgroup are more than ", NATIVE_THREADS_PER_GROUP_MAX);
    }
    const NativeStageBindings& bound = mNative[size_t(NativeStage::Compute)];
    if (mChecks)
    {
        const DispatchSize required = mComputePipeline->desc().threadsPerThreadgroup;
        if (required.count() != 0 && required != threadsPerGroup)
        {
            return reason("compute pipeline '", mComputePipeline->desc().function, "' is made for threadgroups of ",
                          required.x, "x", required.y, "x", required.z);
        }
        // A texture's written levels are not bound in any other way in the same dispatch.
        for (const auto& [slot, write] : bound.textures)
        {
            if (write.access == Access::Read)
            {
                continue;
            }
            const auto written = rangeLevels(write.range, write.texture->desc().mipLevels);
            for (const auto& [other, binding] : bound.textures)
            {
                if (other == slot || binding.texture != write.texture)
                {
                    continue;
                }
                const auto levels = rangeLevels(binding.range, binding.texture->desc().mipLevels);
                if (levels.first < written.second && written.first < levels.second)
                {
                    return reason("texture '", write.texture->debugName(), "' is written at slot ", slot,
                                  " and bound again at slot ", other, " over the same levels");
                }
            }
        }
    }
    // A barrier when the dispatch binds anything an earlier dispatch of the pass wrote since the last one, or
    // writes anything an earlier dispatch read since then: dispatches of one pass may run at the same time.
    const auto listed = [](const std::vector<U64>& ids, U64 id)
    { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    const auto needs = [&](U64 id, Access access)
    { return listed(mWrittenSinceBarrier, id) || (access != Access::Read && listed(mReadSinceBarrier, id)); };
    for (const auto& [slot, binding] : bound.textures)
    {
        barrier = barrier || needs(binding.texture->id(), binding.access);
    }
    for (const auto& [slot, binding] : bound.buffers)
    {
        barrier = barrier || needs(binding.buffer->id(), binding.access);
    }
    if (barrier)
    {
        mWrittenSinceBarrier.clear();
        mReadSinceBarrier.clear();
        ++mDispatchBarriers;
    }
    const auto note = [&](U64 id, Access access)
    {
        std::vector<U64>& ids = access == Access::Read ? mReadSinceBarrier : mWrittenSinceBarrier;
        if (!listed(ids, id))
        {
            ids.push_back(id);
        }
    };
    for (const auto& [slot, binding] : bound.textures)
    {
        note(binding.texture->id(), binding.access);
    }
    for (const auto& [slot, binding] : bound.buffers)
    {
        note(binding.buffer->id(), binding.access);
    }
    ++mDispatches;
    return {};
}

std::string EncoderTracker::setNativeTexture(NativeStage stage, U32 slot, Texture* texture, Access access,
                                             TextureRange range)
{
    std::string why = nativeStageOpen(stage, slot, NATIVE_TEXTURE_SLOTS);
    if (why.empty() && texture && mChecks)
    {
        const TextureDesc& desc = texture->desc();
        const auto levels = rangeLevels(range, desc.mipLevels);
        if (levels.first == levels.second)
        {
            why = reason("levels from ", range.baseLevel, " (", range.levelCount, ") are outside texture '",
                         texture->debugName(), "''s ", desc.mipLevels);
        }
        else if (access != Access::Read && !(desc.usage & TEXTURE_STORAGE))
        {
            why = reason("texture '", texture->debugName(), "' is written without TEXTURE_STORAGE");
        }
        else if (access != Access::Read && levels.second - levels.first != 1)
        {
            why = reason("texture '", texture->debugName(), "' is written over ", levels.second - levels.first,
                         " levels; a written binding covers one");
        }
        else if (desc.samples > 1)
        {
            why = reason("texture '", texture->debugName(), "' is multisampled");
        }
    }
    if (!why.empty())
    {
        // A refused binding clears the slot, as a refused channel binding does.
        if (stage == NativeStage::Compute ? mInCompute : (mInPass && mPass.native))
        {
            mNative[size_t(stage)].textures.erase(slot);
        }
        return why;
    }
    if (!texture)
    {
        mNative[size_t(stage)].textures.erase(slot);
        return {};
    }
    mNative[size_t(stage)].textures[slot] = { texture, access, range };
    return {};
}

std::string EncoderTracker::setNativeSampler(NativeStage stage, U32 slot)
{
    return nativeStageOpen(stage, slot, NATIVE_SAMPLER_SLOTS);
}

std::string EncoderTracker::setNativeBytes(NativeStage stage, U32 slot, size_t bytes)
{
    std::string why = nativeStageOpen(stage, slot, NATIVE_BUFFER_SLOTS);
    if (why.empty() && (bytes == 0 || bytes > NATIVE_BYTES_MAX))
    {
        why = reason(bytes, " bytes of constants; 1 to ", NATIVE_BYTES_MAX, " are taken");
    }
    // The bytes take the slot; refused bytes leave it empty, as a refused buffer or texture does.
    const bool open = stage == NativeStage::Compute ? mInCompute : (mInPass && mPass.native);
    if (why.empty() || (open && slot < NATIVE_BUFFER_SLOTS))
    {
        mNative[size_t(stage)].buffers.erase(slot);
    }
    return why;
}

std::string EncoderTracker::setNativeBuffer(NativeStage stage, U32 slot, Buffer* buffer, U64 offset, Access access)
{
    std::string why = nativeStageOpen(stage, slot, NATIVE_BUFFER_SLOTS);
    if (why.empty() && buffer && mChecks)
    {
        if (offset >= buffer->desc().size)
        {
            why = reason("offset ", offset, " is past buffer '", buffer->debugName(), "''s ", buffer->desc().size,
                         " bytes");
        }
        else if (access != Access::Read && !(buffer->desc().usage & BUFFER_STORAGE))
        {
            why = reason("buffer '", buffer->debugName(), "' is written without BUFFER_STORAGE");
        }
    }
    const bool open = stage == NativeStage::Compute ? mInCompute : (mInPass && mPass.native);
    if (!why.empty() || !buffer)
    {
        if (open && slot < NATIVE_BUFFER_SLOTS)
        {
            mNative[size_t(stage)].buffers.erase(slot);
        }
        return why;
    }
    mNative[size_t(stage)].buffers[slot] = { buffer, offset, access };
    return {};
}

std::string EncoderTracker::clear(bool color, bool depth)
{
    if (!mInPass)
    {
        return reason("no pass is open");
    }
    if (!mChecks)
    {
        return {};
    }
    if (color && mPass.colorCount == 0)
    {
        return reason("pass '", mPass.name, "' has no color to clear");
    }
    if (depth && mPass.depth.texture.isNull())
    {
        return reason("pass '", mPass.name, "' has no depth to clear");
    }
    if (depth && mPass.depth.readOnly)
    {
        return reason("pass '", mPass.name, "' has a read-only depth");
    }
    return {};
}

std::string EncoderTracker::setPipeline(Pipeline* pipeline)
{
    std::string why;
    bool checked = false;
    if (pipeline)
    {
        // A failed pipeline has no native object to draw with, so this check stays on.
        if (pipeline->status() == CompileStatus::Failed)
        {
            why = reason("the pipeline failed to compile");
        }
        else if (mInPass && pipeline->native() != mPass.native)
        {
            // A native pipeline's bindings are the native slots, a translated one's the channels: neither draws in
            // the other's pass.
            why = pipeline->native() ? reason("a native pipeline outside a native pass")
                                     : reason("a translated program's pipeline in native pass '", mPass.name, "'");
        }
        else if (mChecks && mInPass)
        {
            why = checkPipelineForPass(pipeline->desc(), mPass);
            checked = true;
        }
    }
    mPipeline = why.empty() ? pipeline : nullptr;
    mFitsFormats = checked && why.empty() ? passFormatsSerial() : 0;
    return why;
}

std::string EncoderTracker::setDepthStencilState(DepthStencilState* state)
{
    mDepthStencil = state;
    return {};
}

std::string EncoderTracker::setVertexBuffer(Buffer* buffer, const VertexStreams& streams)
{
    std::string why;
    if (mChecks && buffer)
    {
        if (!(buffer->desc().usage & BUFFER_VERTEX))
        {
            why = reason("buffer '", buffer->debugName(), "' is not a vertex buffer");
        }
        else if ((streams.attributeMask >> VertexStreams::MAX_ATTRIBUTES) != 0)
        {
            why = reason("the streams have unknown attributes");
        }
        for (U32 i = 0; why.empty() && i < VertexStreams::MAX_ATTRIBUTES; ++i)
        {
            if ((streams.attributeMask & (1u << i)) && streams.offset[i] >= buffer->desc().size)
            {
                why = reason("attribute ", i, "'s stream starts beyond buffer '",
                             buffer->debugName(), "'");
            }
        }
    }
    const bool bound = why.empty() && buffer;
    mVertexBuffer = bound ? buffer : nullptr;
    mStreams = bound ? streams : VertexStreams();
    return why;
}

std::string EncoderTracker::setIndexBuffer(Buffer* buffer, IndexType type)
{
    std::string why;
    if (mChecks && buffer && !(buffer->desc().usage & BUFFER_INDEX))
    {
        why = reason("buffer '", buffer->debugName(), "' is not an index buffer");
    }
    mIndexBuffer = why.empty() ? buffer : nullptr;
    mIndexType = type;
    return why;
}

std::string EncoderTracker::setTexture(U32 channel, Texture* texture)
{
    std::string why;
    if (mChecks && mMaxChannels != 0 && channel >= mMaxChannels)
    {
        return reason("channel ", channel, " is beyond the ", mMaxChannels, " channels");
    }
    if (mChecks && texture && !(texture->desc().usage & TEXTURE_SAMPLED))
    {
        why = reason("texture '", texture->debugName(), "' cannot be sampled");
    }
    if (why.empty() && texture)
    {
        const auto bound = mTextures.find(channel);
        if (bound != mTextures.end())
        {
            bound->second = texture;
        }
        else if (!mSpareTextureEntries.empty())
        {
            // [Viewer Lab] An entry an unbinding left, so binding allocates nothing
            TextureMap::node_type entry = std::move(mSpareTextureEntries.back());
            mSpareTextureEntries.pop_back();
            entry.key() = channel;
            entry.mapped() = texture;
            mTextures.insert(std::move(entry));
        }
        else
        {
            mTextures.emplace(channel, texture);
        }
    }
    else
    {
        releaseTextureEntry(channel);
    }
    return why;
}

void EncoderTracker::releaseTextureEntry(U32 channel)
{
    // More spare entries than channels a stage samples would never be used again.
    constexpr size_t MAX_SPARE = 64;
    TextureMap::node_type entry = mTextures.extract(channel);
    if (entry.empty())
    {
        return;
    }
    entry.mapped() = nullptr;
    if (mSpareTextureEntries.size() < MAX_SPARE)
    {
        mSpareTextureEntries.push_back(std::move(entry));
    }
}

std::string EncoderTracker::setUniformBuffer(U32 blockIndex, Buffer* buffer, U64 offset, U64 size)
{
    std::string why;
    if (mChecks && buffer)
    {
        why = (buffer->desc().usage & BUFFER_UNIFORM)
            ? validateRange(buffer->desc(), offset, size)
            : reason("buffer '", buffer->debugName(), "' is not a uniform buffer");
    }
    if (why.empty() && buffer)
    {
        mUniformBuffers[blockIndex] = buffer;
    }
    else
    {
        mUniformBuffers.erase(blockIndex);
    }
    return why;
}

std::string EncoderTracker::setUniform(Program* program, U32 uniformIndex, size_t bytes, U32 count)
{
    if (!mChecks)
    {
        return {};
    }
    if (!program)
    {
        return reason("no program");
    }
    return validateUniform(program->reflection(), uniformIndex, bytes, count);
}

std::string EncoderTracker::setSamplerChannel(Program* program, U32 uniformIndex)
{
    if (!mChecks)
    {
        return {};
    }
    if (!program)
    {
        return reason("no program");
    }
    const ProgramReflection& reflection = program->reflection();
    if (uniformIndex >= reflection.uniforms.size())
    {
        return reason("uniform ", uniformIndex, " does not exist; the program has ",
                      reflection.uniforms.size());
    }
    if (!isSampler(reflection.uniforms[uniformIndex].type))
    {
        return reason("'", reflection.uniforms[uniformIndex].name, "' is not a sampler");
    }
    return {};
}

// The pass's dynamic state: recorded as given, never applied by the tracker.

std::string EncoderTracker::dynamicCall()
{
    ++mDynamicCalls;
    // Also with checks off: a native encoder sets dynamic state only in a pass. The text is the one
    // the GL backend logged before the record was the tracker's.
    if (!mInPass)
    {
        return reason("dynamic state is set inside a pass");
    }
    return {};
}

std::string EncoderTracker::setViewport(S32 x, S32 y, S32 width, S32 height)
{
    std::string why = dynamicCall();
    if (why.empty())
    {
        mDynamic.viewport = std::array<S32, 4>{ x, y, width, height };
    }
    return why;
}

std::string EncoderTracker::setScissor(bool enabled, S32 x, S32 y, S32 width, S32 height)
{
    std::string why = dynamicCall();
    if (why.empty())
    {
        mDynamic.scissorEnabled = enabled;
        mDynamic.scissor = { x, y, width, height };
    }
    return why;
}

std::string EncoderTracker::setCullMode(CullMode mode)
{
    std::string why = dynamicCall();
    if (why.empty())
    {
        mDynamic.cull = mode;
    }
    return why;
}

std::string EncoderTracker::setFillMode(FillMode mode)
{
    std::string why = dynamicCall();
    if (why.empty())
    {
        mDynamic.fill = mode;
    }
    return why;
}

std::string EncoderTracker::setDepthBias(F32 factor, F32 units, bool enableFill, bool enableLines)
{
    std::string why = dynamicCall();
    if (why.empty())
    {
        mDynamic.biasFactor = factor;
        mDynamic.biasUnits = units;
        mDynamic.biasFill = enableFill;
        mDynamic.biasLines = enableLines;
    }
    return why;
}

std::string EncoderTracker::setDepthClamp(bool enabled)
{
    std::string why = dynamicCall();
    if (why.empty())
    {
        mDynamic.depthClamp = enabled;
    }
    return why;
}

std::string EncoderTracker::setPointSize(F32 size)
{
    std::string why = dynamicCall();
    if (why.empty())
    {
        mDynamic.pointSize = size;
    }
    return why;
}

std::string EncoderTracker::setLineWidth(F32 width)
{
    std::string why = dynamicCall();
    if (why.empty())
    {
        // As LLRender::setLineWidth clamps it, to what the device can draw.
        mDynamic.lineWidth = llclamp(width, mLineWidthRange[0], mLineWidthRange[1]);
    }
    return why;
}

std::string EncoderTracker::checkDraw(PrimitiveType type) const
{
    // These stay on with checks off: without a pass or a usable pipeline the native encoder has
    // nothing to draw with.
    if (!mInPass)
    {
        return reason("no pass is open");
    }
    if (mPipeline.isNull())
    {
        return reason("no pipeline is set");
    }
    // A pipeline that was still compiling when it was set may have failed since.
    if (mPipeline->status() == CompileStatus::Failed)
    {
        return reason("the pipeline failed to compile");
    }
    if (mPipeline->native() != mPass.native)
    {
        return mPass.native ? reason("a translated program's pipeline in native pass '", mPass.name, "'")
                            : reason("a native pipeline outside a native pass");
    }
    if (!mChecks)
    {
        return {};
    }
    const PipelineDesc& pipeline = mPipeline->desc();
    // checkPipelineForPass reads only the pass's formats, and its reasons name no pass; the bound
    // pipeline's description never changes. So its verdict holds for every pass with the formats
    // serial it was given for.
    const U64 formats = passFormatsSerial();
    if (mFitsFormats == 0 || mFitsFormats != formats)
    {
        std::string why = checkPipelineForPass(pipeline, mPass);
        if (!why.empty())
        {
            return why;
        }
        mFitsFormats = formats;
    }
    if (primitiveClass(type) != pipeline.primitiveClass)
    {
        return reason("a ", className(primitiveClass(type)), " draw with a ",
                      className(pipeline.primitiveClass), " pipeline");
    }
    // The names in quotes, so that a refusal on LLVertexBuffer's draws stays one reason category.
    if (pipeline.attributeMask != 0 && mVertexBuffer.isNull())
    {
        return reason("no vertex buffer is bound for pipeline '", mPipeline->debugName(), "'");
    }
    if ((pipeline.attributeMask & ~mStreams.attributeMask) != 0)
    {
        return reason("the vertex buffer '", mVertexBuffer->debugName(), "' lacks attributes pipeline '",
                      mPipeline->debugName(), "' reads");
    }
    if (mPass.depth.readOnly && mDepthStencil.notNull() && mDepthStencil->desc().depthWrite)
    {
        return reason("depth writes are on, but pass '", mPass.name, "' has a read-only depth");
    }
    return {};
}

std::string EncoderTracker::draw(PrimitiveType type)
{
    return checkDraw(type);
}

std::string EncoderTracker::drawIndexed(PrimitiveType type, U32 firstIndex, U32 indexCount)
{
    std::string why = checkDraw(type);
    if (!why.empty() || !mChecks)
    {
        return why;
    }
    if (mIndexBuffer.isNull())
    {
        return reason("no index buffer is bound");
    }
    const U64 end = (U64(firstIndex) + indexCount) * indexSize(mIndexType);
    if (end > mIndexBuffer->desc().size)
    {
        return reason("indices ", firstIndex, " to ", U64(firstIndex) + indexCount,
                      " are beyond index buffer '", mIndexBuffer->debugName(), "'");
    }
    return {};
}

std::string EncoderTracker::updateBuffer(Buffer* buffer, U64 offset, U64 size)
{
    if (mInCompute)
    {
        return outsideCompute();
    }
    if (!mChecks)
    {
        return {};
    }
    return buffer ? validateRange(buffer->desc(), offset, size) : reason("no buffer");
}

std::string EncoderTracker::uploadTexture(Texture* texture, U32 level, U32 layer,
                                          const Region& region, const PixelLayout& layout,
                                          size_t bytes)
{
    if (mInCompute)
    {
        return outsideCompute();
    }
    if (!mChecks)
    {
        return {};
    }
    return texture ? validateUpload(texture->desc(), level, layer, region, layout, bytes)
                   : reason("no texture");
}

std::string EncoderTracker::copyTexture(Texture* src, U32 srcLevel, U32 srcLayer,
                                        const Region& srcRegion, Texture* dst, U32 dstLevel,
                                        U32 dstLayer, U32 dstX, U32 dstY)
{
    if (mInCompute)
    {
        return outsideCompute();
    }
    // A 3D texture is filled by uploads only: a copy's layer is not a slice of it.
    if ((src && src->desc().type == TextureType::Tex3D) || (dst && dst->desc().type == TextureType::Tex3D))
    {
        return reason("a 3D texture is not copied");
    }
    if (!mChecks)
    {
        acceptTransfer({ Transfer::Kind::Copy, src ? src->id() : 0, srcLevel, srcLayer, srcRegion, dst ? dst->id() : 0, dstLevel,
                         dstLayer, dstX, dstY });
        return {};
    }
    if (!src || !dst)
    {
        return reason("no texture");
    }
    std::string why = validateRegion(src->desc(), srcLevel, srcLayer, srcRegion);
    if (!why.empty())
    {
        return reason("source: ", why);
    }
    why = validateRegion(dst->desc(), dstLevel, dstLayer,
                         Region{ dstX, dstY, srcRegion.width, srcRegion.height });
    if (!why.empty())
    {
        return reason("destination: ", why);
    }
    acceptTransfer({ Transfer::Kind::Copy, src ? src->id() : 0, srcLevel, srcLayer, srcRegion, dst ? dst->id() : 0, dstLevel,
                     dstLayer, dstX, dstY });
    return why;
}

std::string EncoderTracker::generateMipmaps(Texture* texture)
{
    if (mInCompute)
    {
        return outsideCompute();
    }
    if (!mChecks)
    {
        return {};
    }
    if (!texture)
    {
        return reason("no texture");
    }
    std::string why = validateMipmaps(texture->desc());
    return why.empty() ? why : reason("texture '", texture->debugName(), "': ", why);
}

std::string EncoderTracker::readback(Texture* texture, U32 level, U32 layer, const Region& region)
{
    if (mInCompute)
    {
        return outsideCompute();
    }
    if (!mChecks)
    {
        acceptTransfer({ Transfer::Kind::TextureReadback, texture ? texture->id() : 0, level, layer, region });
        return {};
    }
    if (!texture)
    {
        return reason("no texture");
    }
    if (!(texture->desc().usage & TEXTURE_READBACK))
    {
        return reason("texture '", texture->debugName(), "' cannot be read back");
    }
    std::string why = validateRegion(texture->desc(), level, layer, region);
    if (why.empty())
    {
        acceptTransfer({ Transfer::Kind::TextureReadback, texture ? texture->id() : 0, level, layer, region });
    }
    return why;
}

std::string EncoderTracker::readback(Buffer* buffer, U64 offset, U64 size)
{
    if (mInCompute)
    {
        return outsideCompute();
    }
    if (!mChecks)
    {
        ++mTransfers;
        return {};
    }
    std::string why = buffer ? validateRange(buffer->desc(), offset, size) : reason("no buffer");
    if (why.empty())
    {
        ++mTransfers;
    }
    return why;
}

std::string EncoderTracker::beginQuery(Query* query)
{
    if (!query)
    {
        return reason("no query");
    }
    if (mInCompute)
    {
        return outsideCompute(); // a query begins in a render pass or between passes
    }
    if (mChecks && query->type() == QueryType::TimeElapsed && !mTimerQueries)
    {
        return reason("timer queries are not supported");
    }
    if (mChecks && query->type() == QueryType::PrimitivesGenerated && !mPrimitivesQueries)
    {
        return reason("primitive queries are not supported");
    }
    for (const Ref<Query>& active : mQueries)
    {
        if (queryClass(active->type(), mConcurrentSampleQueries) == queryClass(query->type(), mConcurrentSampleQueries))
        {
            return reason("a query of the same kind is already active");
        }
    }
    mQueries.push_back(query);
    return {};
}

std::string EncoderTracker::endQuery(Query* query)
{
    auto found = std::find_if(mQueries.begin(), mQueries.end(),
                              [query](const Ref<Query>& active) { return active.get() == query; });
    if (found == mQueries.end())
    {
        return reason("the query is not active");
    }
    mQueries.erase(found);
    return {};
}

std::string EncoderTracker::pushDebugGroup()
{
    ++mGroups;
    return {};
}

std::string EncoderTracker::popDebugGroup()
{
    if (mGroups == 0)
    {
        return reason("no debug group is open");
    }
    if (mInCompute && mGroups == mGroupsAtComputeStart)
    {
        return reason("the debug group was opened outside compute pass '", mCompute.name, "'");
    }
    if (mInPass && mGroups == mGroupsAtPassStart)
    {
        if (!mPass.stack)
        {
            return reason("the debug group was opened outside pass '", mPass.name, "'");
        }
        --mGroupsAtPassStart; // a stack pass's groups may have begun before it
    }
    --mGroups;
    return {};
}

EncoderTracker::Closing EncoderTracker::endFrame()
{
    Closing closing;
    if (mInCompute)
    {
        // The compute pass ends first; a stack pass it suspended goes on and ends with the frame below.
        const std::string name = mCompute.name;
        closing.popInPass += endComputePass().popInPass;
        closing.endCompute = true;
        closing.warning = reason("the frame ended in compute pass '", name, "'");
    }
    while (mInPass)
    {
        // A native pass that suspended a stack pass ends, and then the stack pass, which ends with the frame.
        const std::string name = mPass.name;
        const bool stack = mPass.stack;
        const Closing ended = endRenderPass();
        closing.endPass = true;
        closing.popInPass += ended.popInPass;
        if (!stack && closing.warning.empty())
        {
            closing.warning = reason("the frame ended in pass '", name, "'");
        }
    }
    mSuspended.reset();
    if (mGroups != 0)
    {
        if (closing.warning.empty())
        {
            closing.warning = reason("the frame ended with ", mGroups, " debug groups open");
        }
        closing.popOutside = mGroups;
        mGroups = 0;
    }
    if (!mQueries.empty() && closing.warning.empty())
    {
        closing.warning = reason("the frame ended with ", mQueries.size(), " queries active");
    }
    mQueries.clear();
    mGroupsAtPassStart = 0;
    mGroupsAtComputeStart = 0;

    // A binding whose object nobody but the tracker holds can never be bound again by a facade,
    // so it is dropped, as GL unbinds a deleted texture; the others stay, as on GL. An object can
    // be bound in several places, so the tracker's own references are counted first, and every
    // binding is judged before any is dropped.
    std::unordered_map<const LLThreadSafeRefCount*, S32> held;
    auto count = [&held](const auto& ref)
    {
        if (ref.notNull())
        {
            ++held[ref.get()];
        }
    };
    count(mPipeline);
    count(mDepthStencil);
    count(mVertexBuffer);
    count(mIndexBuffer);
    for (const auto& entry : mTextures)
    {
        count(entry.second);
    }
    for (const auto& entry : mUniformBuffers)
    {
        count(entry.second);
    }
    auto onlyHere = [&held](const auto& ref)
    {
        return ref.notNull() && ref->getNumRefs() == held[ref.get()];
    };
    const bool dropPipeline = onlyHere(mPipeline);
    const bool dropDepthStencil = onlyHere(mDepthStencil);
    const bool dropVertices = onlyHere(mVertexBuffer);
    const bool dropIndices = onlyHere(mIndexBuffer);
    std::vector<U32> dropTextures;
    for (const auto& entry : mTextures)
    {
        if (onlyHere(entry.second))
        {
            dropTextures.push_back(entry.first);
        }
    }
    std::vector<U32> dropBlocks;
    for (const auto& entry : mUniformBuffers)
    {
        if (onlyHere(entry.second))
        {
            dropBlocks.push_back(entry.first);
        }
    }

    if (dropPipeline)
    {
        mPipeline = nullptr;
    }
    if (dropDepthStencil)
    {
        mDepthStencil = nullptr;
    }
    if (dropVertices)
    {
        mVertexBuffer = nullptr;
        mStreams = VertexStreams();
    }
    if (dropIndices)
    {
        mIndexBuffer = nullptr;
    }
    for (U32 channel : dropTextures)
    {
        releaseTextureEntry(channel);
    }
    for (U32 block : dropBlocks)
    {
        mUniformBuffers.erase(block);
    }
    return closing;
}

EncoderTracker::DrawBindings EncoderTracker::drawBindings() const
{
    return { mPipeline, mDepthStencil, mVertexBuffer, mStreams, mIndexBuffer, mIndexType };
}

void EncoderTracker::restoreDrawBindings(DrawBindings bindings)
{
    mPipeline = std::move(bindings.pipeline);
    mDepthStencil = std::move(bindings.depthStencil);
    mVertexBuffer = std::move(bindings.vertexBuffer);
    mStreams = bindings.streams;
    mIndexBuffer = std::move(bindings.indexBuffer);
    mIndexType = bindings.indexType;
    mFitsFormats = 0;
}

size_t EncoderTracker::heldReferences() const
{
    size_t held = mTextures.size() + mUniformBuffers.size() + mQueries.size();
    held += mPipeline.notNull() + mDepthStencil.notNull() + mVertexBuffer.notNull()
          + mIndexBuffer.notNull();
    for (U32 i = 0; i < mPass.colorCount; ++i)
    {
        held += mPass.color[i].texture.notNull();
    }
    held += mPass.depth.texture.notNull();
    held += mComputePipeline.notNull();
    for (const NativeStageBindings& stage : mNative)
    {
        held += stage.textures.size() + stage.buffers.size();
    }
    if (mSuspended)
    {
        for (U32 i = 0; i < mSuspended->pass.colorCount; ++i)
        {
            held += mSuspended->pass.color[i].texture.notNull();
        }
        held += mSuspended->pass.depth.texture.notNull();
        const DrawBindings& bindings = mSuspended->bindings;
        held += bindings.pipeline.notNull() + bindings.depthStencil.notNull() + bindings.vertexBuffer.notNull()
              + bindings.indexBuffer.notNull();
    }
    return held;
}

// The table from GL's enums ----------------------------------------------------------------------

namespace
{
    struct FormatEntry
    {
        Format format;
        U32 gl; ///< the GL internal format the backend asks GL for
    };

    // One entry per format; toGLInternalFormat() uses it both ways.
    constexpr FormatEntry sFormats[] =
    {
        { Format::R8Unorm,               GL_R8 },
        { Format::RG8Unorm,              GL_RG8 },
        { Format::RGBA8Unorm,            GL_RGBA8 },
        { Format::RGBA8Unorm_sRGB,       GL_SRGB8_ALPHA8 },
        { Format::R16Float,              GL_R16F },
        { Format::RG16Float,             GL_RG16F },
        { Format::RGBA16Float,           GL_RGBA16F },
        { Format::RGBA16Unorm,           GL_RGBA16 },
        { Format::R32Float,              GL_R32F },
        { Format::RG32Float,             GL_RG32F },
        { Format::RGBA32Float,           GL_RGBA32F },
        { Format::RGB10A2Unorm,          GL_RGB10_A2 },
        { Format::RG11B10Float,          GL_R11F_G11F_B10F },
        { Format::R8Uint,                GL_R8UI },
        { Format::R16Uint,               GL_R16UI },
        { Format::R32Uint,               GL_R32UI },
        { Format::BC1_RGBA,              GL_COMPRESSED_RGBA_S3TC_DXT1_EXT },
        { Format::BC1_RGBA_sRGB,         GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT },
        { Format::BC3_RGBA,              GL_COMPRESSED_RGBA_S3TC_DXT5_EXT },
        { Format::BC3_RGBA_sRGB,         GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT },
        { Format::BC5_RG,                GL_COMPRESSED_RG_RGTC2 },
        { Format::BC7_RGBA,              GL_COMPRESSED_RGBA_BPTC_UNORM },
        { Format::BC7_RGBA_sRGB,         GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM },
        { Format::Depth32Float,          GL_DEPTH_COMPONENT32F },
        { Format::Depth32Float_Stencil8, GL_DEPTH32F_STENCIL8 },
    };

    // GL formats that have no format of their own: three components become four, unsized formats
    // become their usual sized format, and every depth format becomes 32-bit float. The GL backend
    // still asks GL for these (TextureDesc::glFormat). GL's deprecated alpha
    // and luminance formats become R8 or RG8, with the texture's swizzle as LLImageGL sets it, and
    // GL's generic compressed formats their uncompressed format.
    constexpr FormatEntry sWidenedFormats[] =
    {
        { Format::R8Unorm,               GL_RED },
        { Format::RG8Unorm,              GL_RG },
        { Format::RGBA8Unorm,            GL_RGB },
        { Format::RGBA8Unorm,            GL_RGBA },
        { Format::RGBA8Unorm_sRGB,       GL_SRGB },
        { Format::RGBA8Unorm_sRGB,       GL_SRGB_ALPHA },
        { Format::RGBA8Unorm,            GL_RGB8 },
        { Format::RGBA8Unorm_sRGB,       GL_SRGB8 },
        { Format::RGBA16Float,           GL_RGB16F },
        { Format::RGBA32Float,           GL_RGB32F },
        { Format::Depth32Float,          GL_DEPTH_COMPONENT },
        { Format::Depth32Float,          GL_DEPTH_COMPONENT24 },
        { Format::Depth32Float,          GL_DEPTH_COMPONENT32 },
        { Format::Depth32Float_Stencil8, GL_DEPTH24_STENCIL8 },
        { Format::R8Unorm,               GL_ALPHA },
        { Format::R8Unorm,               GL_ALPHA8 },
        { Format::R8Unorm,               GL_LUMINANCE },
        { Format::R8Unorm,               GL_LUMINANCE8 },
        { Format::RG8Unorm,              GL_LUMINANCE_ALPHA },
        { Format::RG8Unorm,              GL_LUMINANCE8_ALPHA8 },
        { Format::R8Unorm,               GL_COMPRESSED_RED },
        { Format::RG8Unorm,              GL_COMPRESSED_RG },
        { Format::RGBA8Unorm,            GL_COMPRESSED_RGB },
        { Format::RGBA8Unorm,            GL_COMPRESSED_RGBA },
        { Format::RGBA8Unorm_sRGB,       GL_COMPRESSED_SRGB },
        { Format::RGBA8Unorm_sRGB,       GL_COMPRESSED_SRGB_ALPHA },
        { Format::R8Unorm,               GL_COMPRESSED_ALPHA },
        { Format::R8Unorm,               GL_COMPRESSED_LUMINANCE },
        { Format::RG8Unorm,              GL_COMPRESSED_LUMINANCE_ALPHA },
    };
}

// Every format but Unknown and BGRA8Unorm (drawables only) has exactly one entry.
static_assert(std::size(sFormats) == size_t(Format::Count) - 2,
              "every format needs an entry in sFormats");

Format formatFromGL(U32 internalFormat)
{
    for (const FormatEntry& entry : sFormats)
    {
        if (entry.gl == internalFormat)
        {
            return entry.format;
        }
    }
    for (const FormatEntry& entry : sWidenedFormats)
    {
        if (entry.gl == internalFormat)
        {
            return entry.format;
        }
    }
    return Format::Unknown;
}

U32 toGLInternalFormat(Format format)
{
    for (const FormatEntry& entry : sFormats)
    {
        if (entry.format == format)
        {
            return entry.gl;
        }
    }
    return 0;
}

bool hasAlphaInGLFormat(U32 glFormat)
{
    switch (glFormat)
    {
    case GL_RGB:
    case GL_RGB8:
    case GL_SRGB:
    case GL_SRGB8:
    case GL_COMPRESSED_RGB:
    case GL_COMPRESSED_SRGB:
    case GL_RGB16F:
    case GL_RGB32F:
        return false;
    default:
        return true;
    }
}

std::optional<PixelLayout> pixelLayoutFromGL(U32 format, U32 type, bool swapBytes, U32 rowLength)
{
    PixelLayout layout;
    layout.swapBytes = swapBytes;
    layout.rowLength = rowLength;

    switch (format)
    {
    case GL_RED:             layout.components = PixelLayout::Components::R; break;
    case GL_RG:              layout.components = PixelLayout::Components::RG; break;
    // The facades' deprecated formats: their bytes are R and RG, which a backend with swizzle
    // masks uploads into R8 or RG8 (LLImageGL::describeTexture). Upload layouts only,
    // not for readbacks or a round trip to GL's format (llrhi.h).
    case GL_ALPHA:           layout.components = PixelLayout::Components::R; break;
    case GL_LUMINANCE:       layout.components = PixelLayout::Components::R; break;
    case GL_LUMINANCE_ALPHA: layout.components = PixelLayout::Components::RG; break;
    case GL_RGB:             layout.components = PixelLayout::Components::RGB; break;
    case GL_RGBA:            layout.components = PixelLayout::Components::RGBA; break;
    case GL_BGRA:            layout.components = PixelLayout::Components::BGRA; break;
    case GL_DEPTH_COMPONENT: layout.components = PixelLayout::Components::Depth; break;
    default:
        return std::nullopt;
    }

    switch (type)
    {
    case GL_UNSIGNED_BYTE:  layout.type = PixelLayout::Type::U8; break;
    case GL_UNSIGNED_SHORT: layout.type = PixelLayout::Type::U16; break;
    case GL_UNSIGNED_INT:   layout.type = PixelLayout::Type::U32; break;
    case GL_HALF_FLOAT:     layout.type = PixelLayout::Type::F16; break;
    case GL_FLOAT:          layout.type = PixelLayout::Type::F32; break;
    case GL_UNSIGNED_INT_8_8_8_8_REV:
        // A packed type: GL allows it with four components only.
        if (layout.components != PixelLayout::Components::RGBA
            && layout.components != PixelLayout::Components::BGRA)
        {
            return std::nullopt;
        }
        layout.type = PixelLayout::Type::U32_8_8_8_8_Rev;
        break;
    default:
        return std::nullopt;
    }
    return layout;
}

U32 toGLPixelFormat(const PixelLayout& layout)
{
    switch (layout.components)
    {
    case PixelLayout::Components::R:     return GL_RED;
    case PixelLayout::Components::RG:    return GL_RG;
    case PixelLayout::Components::RGB:   return GL_RGB;
    case PixelLayout::Components::RGBA:  return GL_RGBA;
    case PixelLayout::Components::BGRA:  return GL_BGRA;
    case PixelLayout::Components::Depth: return GL_DEPTH_COMPONENT;
    }
    return 0;
}

U32 toGLPixelType(const PixelLayout& layout)
{
    switch (layout.type)
    {
    case PixelLayout::Type::U8:              return GL_UNSIGNED_BYTE;
    case PixelLayout::Type::U16:             return GL_UNSIGNED_SHORT;
    case PixelLayout::Type::U32:             return GL_UNSIGNED_INT;
    case PixelLayout::Type::F16:             return GL_HALF_FLOAT;
    case PixelLayout::Type::F32:             return GL_FLOAT;
    case PixelLayout::Type::U32_8_8_8_8_Rev: return GL_UNSIGNED_INT_8_8_8_8_REV;
    }
    return 0;
}

std::optional<ShaderStage> shaderStageFromGL(U32 type)
{
    switch (type)
    {
        case GL_VERTEX_SHADER:
            return ShaderStage::Vertex;
        case GL_FRAGMENT_SHADER:
            return ShaderStage::Fragment;
        case GL_GEOMETRY_SHADER:
            return ShaderStage::Geometry;
        default:
            return std::nullopt;
    }
}

std::optional<QueryType> queryTypeFromGL(U32 target)
{
    switch (target)
    {
        case GL_ANY_SAMPLES_PASSED:
            return QueryType::AnySamplesPassed;
        case GL_SAMPLES_PASSED:
            return QueryType::SamplesPassed;
        case GL_TIME_ELAPSED:
            return QueryType::TimeElapsed;
        case GL_PRIMITIVES_GENERATED:
            return QueryType::PrimitivesGenerated;
        default:
            return std::nullopt;
    }
}

std::optional<UniformType> uniformTypeFromGL(U32 type)
{
    switch (type)
    {
        case GL_FLOAT:                  return UniformType::Float;
        case GL_FLOAT_VEC2:             return UniformType::Vec2;
        case GL_FLOAT_VEC3:             return UniformType::Vec3;
        case GL_FLOAT_VEC4:             return UniformType::Vec4;
        case GL_INT:
        case GL_BOOL:
        case GL_UNSIGNED_INT:           return UniformType::Int;
        case GL_INT_VEC2:
        case GL_BOOL_VEC2:
        case GL_UNSIGNED_INT_VEC2:      return UniformType::IVec2;
        case GL_INT_VEC3:
        case GL_BOOL_VEC3:
        case GL_UNSIGNED_INT_VEC3:      return UniformType::IVec3;
        case GL_INT_VEC4:
        case GL_BOOL_VEC4:
        case GL_UNSIGNED_INT_VEC4:      return UniformType::IVec4;
        case GL_FLOAT_MAT2:             return UniformType::Mat2;
        case GL_FLOAT_MAT3:             return UniformType::Mat3;
        case GL_FLOAT_MAT3x4:           return UniformType::Mat3x4;
        case GL_FLOAT_MAT4:             return UniformType::Mat4;
        case GL_SAMPLER_2D:
        case GL_SAMPLER_2D_RECT:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_3D:             return UniformType::Sampler2D;
        case GL_SAMPLER_2D_SHADOW:      return UniformType::Sampler2DShadow;
        case GL_SAMPLER_CUBE:           return UniformType::SamplerCube;
        case GL_SAMPLER_CUBE_MAP_ARRAY: return UniformType::SamplerCubeArray;
        default:                        return std::nullopt;
    }
}

U32 toGLUniformType(UniformType type)
{
    switch (type)
    {
        case UniformType::Float:            return GL_FLOAT;
        case UniformType::Vec2:             return GL_FLOAT_VEC2;
        case UniformType::Vec3:             return GL_FLOAT_VEC3;
        case UniformType::Vec4:             return GL_FLOAT_VEC4;
        case UniformType::Int:              return GL_INT;
        case UniformType::IVec2:            return GL_INT_VEC2;
        case UniformType::IVec3:            return GL_INT_VEC3;
        case UniformType::IVec4:            return GL_INT_VEC4;
        case UniformType::Mat2:             return GL_FLOAT_MAT2;
        case UniformType::Mat3:             return GL_FLOAT_MAT3;
        case UniformType::Mat3x4:           return GL_FLOAT_MAT3x4;
        case UniformType::Mat4:             return GL_FLOAT_MAT4;
        case UniformType::Sampler2D:        return GL_SAMPLER_2D;
        case UniformType::Sampler2DShadow:  return GL_SAMPLER_2D_SHADOW;
        case UniformType::SamplerCube:      return GL_SAMPLER_CUBE;
        case UniformType::SamplerCubeArray: return GL_SAMPLER_CUBE_MAP_ARRAY;
        case UniformType::Sampler3D:        return GL_SAMPLER_3D;
    }
    return 0;
}

std::optional<CompareFunc> compareFuncFromGL(U32 func)
{
    switch (func)
    {
        case GL_NEVER:    return CompareFunc::Never;
        case GL_LESS:     return CompareFunc::Less;
        case GL_EQUAL:    return CompareFunc::Equal;
        case GL_LEQUAL:   return CompareFunc::LessEqual;
        case GL_GREATER:  return CompareFunc::Greater;
        case GL_NOTEQUAL: return CompareFunc::NotEqual;
        case GL_GEQUAL:   return CompareFunc::GreaterEqual;
        case GL_ALWAYS:   return CompareFunc::AlwaysPass;
        default:          return std::nullopt;
    }
}

std::optional<IndexType> indexTypeFromGL(U32 type)
{
    switch (type)
    {
        case GL_UNSIGNED_SHORT: return IndexType::U16;
        case GL_UNSIGNED_INT:   return IndexType::U32;
        default:                return std::nullopt;
    }
}

std::optional<CullMode> cullModeFromGL(U32 face)
{
    switch (face)
    {
        case GL_FRONT: return CullMode::Front;
        case GL_BACK:  return CullMode::Back;
        default:       return std::nullopt;
    }
}

std::optional<FillMode> fillModeFromGL(U32 mode)
{
    switch (mode)
    {
        case GL_FILL: return FillMode::Fill;
        case GL_LINE: return FillMode::Lines;
        default:      return std::nullopt;
    }
}

} // namespace LL::RHI
