/**
 * @file llrhimetaldevice-objc.mm
 * @brief The Metal backend's device
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

#include "llrhimetal-objc.h" // first: it settles BOOL

#if LL_RHI_METAL

#include "llgl.h" // GL's sample count after the CGL query caps maxSamples
#include "llthread.h" // on_main_thread

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

// Nothing here is initialized dynamically at load time: no namespace-scope objects with
// constructors, no Objective-C classes and no +load, so the viewer's start on GL pays nothing for
// this file.

namespace LL::RHI
{

void metalNotPorted(const char* call, const char* owner)
{
    LL_WARNS("RHI") << call << ": not ported to Metal yet (" << owner << ")" << LL_ENDL;
}

bool metalErrorLosesDevice(NSError* error)
{
    if (!error)
    {
        return false;
    }
    // DeviceRemoved by number: both names are deprecated ("cannot occur on Apple Silicon"), but a report still counts.
    if ([error.domain isEqualToString:MTL4CommandQueueErrorDomain])
    {
        switch (error.code)
        {
        case MTL4CommandQueueErrorNotPermitted:
        case 4: // MTL4CommandQueueErrorDeviceRemoved
        case MTL4CommandQueueErrorAccessRevoked:
            return true;
        default:
            return false;
        }
    }
    if ([error.domain isEqualToString:MTLCommandBufferErrorDomain])
    {
        switch (error.code)
        {
        case MTLCommandBufferErrorAccessRevoked:
        case MTLCommandBufferErrorNotPermitted:
        case 11: // MTLCommandBufferErrorDeviceRemoved
            return true;
        default:
            return false;
        }
    }
    return false;
}

bool metalErrorIsTimeout(NSError* error)
{
    if (!error)
    {
        return false;
    }
    if ([error.domain isEqualToString:MTL4CommandQueueErrorDomain])
    {
        return error.code == MTL4CommandQueueErrorTimeout;
    }
    return [error.domain isEqualToString:MTLCommandBufferErrorDomain] && error.code == MTLCommandBufferErrorTimeout;
}

std::string metalErrorName(NSError* error)
{
    if (!error)
    {
        return "no error";
    }
    const char* name = "";
    if ([error.domain isEqualToString:MTL4CommandQueueErrorDomain])
    {
        static const char* const NAMES[] = { "None", "Timeout", "NotPermitted", "OutOfMemory", "DeviceRemoved",
                                             "AccessRevoked", "Internal" };
        name = error.code >= 0 && error.code < NSInteger(std::size(NAMES)) ? NAMES[error.code] : "";
    }
    else if ([error.domain isEqualToString:MTLCommandBufferErrorDomain])
    {
        static const char* const NAMES[] = { "None", "Internal", "Timeout", "PageFault", "AccessRevoked", "", "",
                                             "NotPermitted", "OutOfMemory", "InvalidResource", "Memoryless",
                                             "DeviceRemoved", "StackOverflow" };
        name = error.code >= 0 && error.code < NSInteger(std::size(NAMES)) ? NAMES[error.code] : "";
    }
    std::string text = std::string(error.domain.UTF8String) + " " + std::to_string(error.code);
    if (*name)
    {
        text += std::string(" (") + name + ")";
    }
    return text;
}

NSString* metalString(std::string_view text)
{
    return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
}

MTLCompareFunction metalCompareFunction(CompareFunc func)
{
    switch (func)
    {
    case CompareFunc::Never:        return MTLCompareFunctionNever;
    case CompareFunc::AlwaysPass:   return MTLCompareFunctionAlways;
    case CompareFunc::Less:         return MTLCompareFunctionLess;
    case CompareFunc::LessEqual:    return MTLCompareFunctionLessEqual;
    case CompareFunc::Equal:        return MTLCompareFunctionEqual;
    case CompareFunc::NotEqual:     return MTLCompareFunctionNotEqual;
    case CompareFunc::GreaterEqual: return MTLCompareFunctionGreaterEqual;
    case CompareFunc::Greater:      return MTLCompareFunctionGreater;
    }
    return MTLCompareFunctionAlways;
}

namespace
{
    /// The most samples a Metal texture takes on this device: the largest of 8, 4 and 2 the device supports
    /// (supportsTextureSampleCount:, MTLDevice.h:880-886), else 1.
    API_AVAILABLE(macos(26.0)) U32 metalTextureSamples(IdMTLDevice device)
    {
        for (const U32 samples : { 8u, 4u, 2u })
        {
            if ([device supportsTextureSampleCount:samples])
            {
                return samples;
            }
        }
        return 1;
    }

    /// Capabilities on Metal: the values the system's GL reports on the same Mac, which Metal handles all of. Apple's
    /// "Metal feature set tables", Apple7: 2D textures up to 16384 pixels on a side, 2048 array layers and 8 color render targets.
    /// The facade LLGLManager gets GL's values from the CGL query, not from here.
    /// A Timestamp heap of pairs pairs, labelled; nil, with a WARN line that ends with off, when Metal gives none.
    /// The device's one for the timer queries has METAL_TIMER_PAIRS pairs (4,096 entries, Metal's most; 32 KiB) and is
    /// made with the device, so the capabilities the encoder's tracker copies at its construction already say
    /// timerQueries; the pass timer makes one per frame slot while it runs.
    API_AVAILABLE(macos(26.0)) IdMTL4CounterHeap metalTimestampHeap(IdMTLDevice device, U32 pairs = METAL_TIMER_PAIRS,
                                                                    NSString* label = @"timer queries",
                                                                    const char* off = "timer queries are off")
    {
        MTL4CounterHeapDescriptor* descriptor = [MTL4CounterHeapDescriptor new];
        descriptor.type = MTL4CounterHeapTypeTimestamp;
        descriptor.count = NSUInteger(pairs) * 2;
        NSError* error = nil;
        IdMTL4CounterHeap heap = [device newCounterHeapWithDescriptor:descriptor error:&error];
        if (!heap)
        {
            LL_WARNS("RHI") << "Metal gave no timestamp heap of " << descriptor.count << " entries"
                            << (error ? std::string(": ") + error.localizedDescription.UTF8String : std::string())
                            << "; " << off << LL_ENDL;
            return nil;
        }
        heap.label = label;
        return heap;
    }

    /// The tick rate (MTLDevice.h:1537-1541), when the heap's resolved entries are the 8-byte
    /// MTL4TimestampHeapEntry that resolveTimestamps reads (MTLDevice.h:1526-1535); otherwise 0, and no timers.
    API_AVAILABLE(macos(26.0)) U64 metalTimestampFrequency(IdMTLDevice device, IdMTL4CounterHeap heap)
    {
        if (!heap || [device sizeOfCounterHeapEntry:MTL4CounterHeapTypeTimestamp] != sizeof(MTL4TimestampHeapEntry))
        {
            return 0;
        }
        return [device queryTimestampFrequency];
    }

    API_AVAILABLE(macos(26.0)) Capabilities metalCapabilities(IdMTLDevice device, const MetalSupport& support,
                                                              bool timers)
    {
        Capabilities caps;
        caps.backend = BackendType::Metal;

        caps.maxTextureSize = 16384;
        caps.maxArrayLayers = 2048;      // MTLTexture.h:186
        caps.maxColorAttachments = 8;
        // No more than GL reports after the CGL query (else GL's 4 on this Mac), so content chosen by the sample
        // count stays as on GL.
        const U32 cap = gGLManager.mMaxSamples > 0 ? U32(gGLManager.mMaxSamples) : 4u;
        caps.maxSamples = std::min(metalTextureSamples(device), cap);
        // The argument table's 16 samplers, as llrhi.h promises for Metal, so LLTexUnit's tell
        // keeps its record below them; the table's 16 textures likewise.
        caps.maxSamplersPerStage = METAL_TABLE_SAMPLERS;
        caps.maxTexturesPerStage = METAL_TABLE_TEXTURES;
        caps.maxUniformBlockSize = 65536; // GL's after LLGLManager::initGL's clamp
        caps.maxVaryingVectors = 31;      // 32 crashes Metal's shader compiler service (llrhi.h)
        caps.maxVertexAttributes = 16;
        caps.lineWidthRange[0] = caps.lineWidthRange[1] = 1.f;
        caps.pointSizeRange[0] = 1.f;
        caps.pointSizeRange[1] = 64.f;
        // Every Metal device filters anisotropically (MTLSampler.h:130-135).
        caps.maxAnisotropy = METAL_MAX_ANISOTROPY;

        caps.cubeMapArrays = true;       // MTLTexture.h:30-31
        caps.anisotropicFiltering = true;
        caps.textureSwizzle = true;      // MTLTexture.h:249-252
        caps.bcCompression = [device supportsBCTextureCompression]; // MTLDevice.h:598-602
        caps.timerQueries = timers;          // the timestamp heap and its tick rate
        caps.timerQueriesWithinPass = false; // a query mid-pass splits the pass
        caps.benchmarkTimers = timers;       // as GL's timerQueries; a GL/Metal benchmark pair agreed
        caps.concurrentSampleQueries = false; // one visibility result mode per render encoder
        caps.primitivesGeneratedQuery = false;
        caps.geometryShaders = false;
        caps.compute = true;
        caps.computePasses = true;  // MTL4ComputeCommandEncoder; the encoder's compute passes
        caps.nativePrograms = true; // MTL4Compiler from source (llrhimetalnative-objc.mm)
        caps.textures3D = true;     // MTLTextureType3D
        caps.spatialUpscaler = metalSpatialUpscaler(device); // MetalFX's MTL4FXSpatialScaler
        caps.temporalUpscaler = metalTemporalUpscaler(device, caps.temporalMinScale, caps.temporalMaxScale);
        caps.depthClamp = true;              // setDepthClipMode (MTL4RenderCommandEncoder.h:124-127)
        caps.debugLabels = true;

        caps.programBinaries = false;
        // LLImageGLThread's uploads go to the upload context's own Metal 3 queue (a GPU-side wait at the
        // swap); the thread makes no GL context off GL
        caps.sharedContextUploads = true;
        caps.pixelBufferObjects = false;
        // Between plain colour formats as a draw (MetalEncoder::formatCopy); sRGB, BC, depth and integer
        // formats only within their own (copyRefusal)
        caps.copyBetweenFormats = true;
        caps.copyBetweenFormatsPlainColorOnly = true;
        caps.shaderOldSelect = false;
        caps.shaderAMDCard = false;
        caps.shaderIfChain = false;

        caps.vramMB = [device recommendedMaxWorkingSetSize] / (1024 * 1024); // MTLDevice.h:517-523
        caps.unifiedMemory = [device hasUnifiedMemory];                      // MTLDevice.h:510-515
        caps.isApple = true;
        caps.vendor = "Apple";
        caps.renderer = support.renderer; // the device's name (metalBackend)
        // NSProcessInfo.h:227-230: human readable, not for parsing
        caps.driverVersion = [[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String];
        return caps;
    }
}

// Resources ---------------------------------------------------------------------------------------

class API_AVAILABLE(macos(26.0)) MetalDepthStencilState final : public DepthStencilState
{
public:
    MetalDepthStencilState(FrameLifecycle& lifecycle, const DepthStencilDesc& desc, IdMTLDepthStencilState state)
    :   mLifecycle(lifecycle), mDesc(desc), mState(state)
    {
    }

    ~MetalDepthStencilState() override
    {
        // Metal 4's command buffers do not keep what they use, so the state lives until its frames
        // have finished.
        IdMTLDepthStencilState state = mState;
        mLifecycle.release([state]() mutable { state = nil; });
    }

    const DepthStencilDesc& desc() const override { return mDesc; }
    IdMTLDepthStencilState state() const { return mState; }

private:
    FrameLifecycle& mLifecycle;
    const DepthStencilDesc mDesc;
    IdMTLDepthStencilState mState;
};

MTLDepthStencilDescriptor* metalDepthStencilDescriptor(const DepthStencilDesc& desc)
{
    // GL tests nothing and writes no depth while its depth test is off.
    MTLDepthStencilDescriptor* metal = [MTLDepthStencilDescriptor new];
    metal.depthCompareFunction = desc.depthTest ? metalCompareFunction(desc.depthFunc) : MTLCompareFunctionAlways;
    metal.depthWriteEnabled = metalWritesDepth(&desc);
    return metal;
}

// The device ------------------------------------------------------------------------------------

MetalDevice::MetalDevice(IdMTLDevice device, IdMTL4CommandQueue queue, dispatch_queue_t feedbackQueue,
                         const MetalSupport& support)
:   mDevice(device),
    mQueue(queue),
    mFeedbackQueue(feedbackQueue),
    mFeedback(std::make_shared<FeedbackState>()),
    mWaitLimitMs(METAL_WAIT_MS),
    mLifecycle([this](U64 frame) { return waitForFrame(frame); }),
    mCompileState(std::make_shared<MetalCompileState>()),
    mTimestamps(metalTimestampHeap(device)),
    mTimestampFrequency(metalTimestampFrequency(device, mTimestamps)),
    mCaps(metalCapabilities(device, support, mTimestampFrequency > 0)),
    mEncoder(*this, mCaps),
    mUploads(*this)
{
    // Labels and checks follow the setting as it is now, so setMetalDebug before
    // Device::create decides for the next device.
    mDebugLabels = mDebugChecks = metalDebug();
    mUploads.state().failures = mCompileState->failures; // the upload handlers' error lines
    // The control run's knob, read once per process, for this device's translation threads and waits
    setCompileSlow(metalCompileSlowKnob());
    // The speed knobs as the environment has them now; the line says which way a measurement ran.
    mSpeed = metalSpeedKnobs();
    LL_INFOS("RHI") << "Metal speed knobs: barriers " << (mSpeed.splitBarriers ? "split" : "full") << ", discard "
                    << (mSpeed.discardStores ? "on" : "off") << LL_ENDL;
    if (!mSpeed.unknown.empty())
    {
        LL_WARNS("RHI") << "Metal speed knob values not understood: " << mSpeed.unknown
                        << " (LL_METAL_BARRIER split or full, LL_METAL_DISCARD 1 or 0); their defaults are used"
                        << LL_ENDL;
    }
    // The timer ring, on the heap made above.
    mTimerUses.assign(mTimestampFrequency > 0 ? mTimerPairs : 0, TimerUse());
    // Per frame slot one command allocator and one shared event.
    for (Slot& slot : mSlots)
    {
        slot.allocator = [mDevice newCommandAllocator];
        slot.event = [mDevice newSharedEvent];
    }
    // Test runs only: the injected GPU fault (metalTestFault), applied by submit() at its frame.
    const std::string fault = metalTestFault();
    for (const auto& [prefix, hang] : { std::pair<const char*, bool>{ "access-revoked@", false },
                                        std::pair<const char*, bool>{ "frame-hang@", true } })
    {
        const size_t length = std::strlen(prefix);
        if (fault.compare(0, length, prefix) == 0)
        {
            mTestFaultFrame = std::strtoull(fault.c_str() + length, nullptr, 10);
            mTestFaultHang = hang;
        }
    }
    if (mTestFaultFrame > 0)
    {
        LL_WARNS("RHI") << "Metal test fault " << fault << " (LL_METAL_TEST_FAULT)" << LL_ENDL;
    }
}

MetalDevice::~MetalDevice()
{
    // Test runs only: an injected hang lets the queue go, so the teardown below does not wait on it.
    if (mTestHangGate)
    {
        mTestHangGate.signaledValue = 1;
    }
    // No job is left in the queue or on a thread, and every library handler has reported
    // before anything a handler could have used goes. No group is sent after this, also
    // not by a thread that translates a closed group's last member while the pool stops.
    mProgramGroups.cancel();
    mProgramPool.stop();
    {
        std::unique_lock<std::mutex> lock(mCompileState->mutex);
        const bool drained = mCompileState->changed.wait_for(lock, std::chrono::milliseconds(mWaitLimitMs),
                                                             [this]() { return mCompileState->outstanding == 0; });
        mCompileState->closing = true;
        const U64 outstanding = mCompileState->outstanding;
        lock.unlock();
        if (!drained)
        {
            LL_WARNS("RHI") << outstanding << " shader library handlers did not report within " << mWaitLimitMs
                            << " ms" << LL_ENDL;
        }
    }
    writeClassLines(); // the pool has stopped, so no line comes after these
    // The process's setDepthClipMode sends, which golden level 2 holds to the validation reports' pairs in the
    // viewer's stderr (every send is reported). No draw is encoded after this.
    LL_INFOS("RHI") << "metal depth clip sends: " << mEncoder.stats().depthClipSends << " (encoders "
                    << mEncoder.stats().depthClipEncoders << ")" << LL_ENDL;
    // The rings' load, once.
    const QueryStats& queries = mQueryStats;
    LL_INFOS("RHI") << "metal queries: sample begins " << queries.begun << " (most in one frame "
                    << std::max(queries.peakSampleBegins, queries.frameSampleBegins) << ", without a slot "
                    << queries.noSlot << "), time begins " << queries.timeBegun << " (most in one frame "
                    << std::max(queries.peakTimeBegins, queries.frameTimeBegins) << ", without a pair "
                    << queries.timeNoPair << "), times read " << queries.timeRead << " (0 ns " << queries.timeZero
                    << ", timer_invalid " << queries.timerInvalid << "), timer splits " << mEncoder.stats().timerSplits
                    << LL_ENDL;
    // A command buffer still being recorded was never sent, and ends unsent: an open native encoder ends
    // first, with its store actions and its debug groups back on the command buffer.
    if (mCommandBuffer)
    {
        mEncoder.closeForTeardown();
        endCopies();
        for (size_t i = 0; i < mGroups.size(); ++i)
        {
            [mCommandBuffer popDebugGroup];
        }
        mBufferGroups = 0;
        [mCommandBuffer endCommandBuffer];
        mCommandBuffer = nil;
    }
    // A drawable whose flip pass was recorded and never sent goes back to the layer unpresented.
    mPendingDrawables.clear();
    if (mCapturing)
    {
        // The frame being captured ends unsent with the device.
        endCapture();
    }
    // The upload queue's command buffers complete first, as a frame may wait for one.
    mUploads.drain();
    // What the upload thread noted and never submitted goes now, so the textures' releases are queued before the
    // lifecycle's releaseAll() below.
    mUploads.dropPending();
    for (const Slot& slot : mSlots)
    {
        if (!reached(slot) && ![slot.event waitUntilSignaledValue:slot.lastSerial timeoutMS:METAL_WAIT_MS])
        {
            LL_WARNS("RHI") << "Frame " << slot.frame << " did not finish on the GPU within " << METAL_WAIT_MS << " ms"
                            << LL_ENDL;
        }
    }
    drainFeedback();
    writeFailureLines(true); // every handler has run, and the counted lines get their summaries
    logGpuTime(true); // every handler has run
    // FrameLifecycle's shutdown: the bindings and the caches let go, then every release runs.
    mEncoder.reset();
    mDefaultDepth = nullptr;
    mDepthStencils.clear();
    mSamplers.clear();
    mDefaultTextures = {};
    mPipelines.clear();
    {
        // The native pipelines hand their states to the lifecycle as the translated ones do.
        std::lock_guard<std::mutex> lock(mNativeMutex);
        mComputePipelines.clear();
        mNativePipelines.clear();
    }
    // The clear draws' pipelines and programs queue their releases here, so they go before
    // releaseAll(); the programs' states leave mPipelineStates first.
    mClearPipelines.clear();
    mClearPrograms.clear();
    mMipPipelines.clear(); // as the clear programs
    mMipProgram = nullptr;
    mPresentPipelines.clear(); // as the mip programs
    mPresentProgram = nullptr;
    mFormatCopyPipelines.clear(); // as the present program
    mFormatCopyProgram = nullptr;
    // After the slot waits, the surface's back buffer and depth queue their releases for releaseAll(), and the
    // layer's set leaves the queue before the queue goes (MTL4CommandQueue.h:333-339).
    mSurface = nullptr;
    mResidency.detachLayer(mQueue);
    for (const CachedIndexList& cached : mIndexLists)
    {
        IdMTLBuffer buffer = cached.list.buffer;
        mLifecycle.release([this, buffer]() { mResidency.retire(buffer); });
    }
    mIndexLists.clear();
    mPipelineStates.clear();
    mCompiler = nil;
    mLifecycle.releaseAll();
}

Ref<Buffer> MetalDevice::createBuffer(const BufferDesc& desc, std::span<const std::byte> initial,
                                      std::string_view debugName)
{
    return createMetalBuffer(*this, desc, initial, debugName);
}

Ref<Texture> MetalDevice::createTexture(const TextureDesc& desc, std::string_view debugName)
{
    // Also called on the upload thread, which has no run loop to drain autoreleased objects.
    @autoreleasepool
    {
        Ref<Texture> texture = createMetalTexture(*this, desc, debugName);
        mEncoder.zeroLater(texture.get()); // a cube is cleared before the frame's next encoder
        return texture;
    }
}

Ref<Upscaler> MetalDevice::createUpscaler(const UpscalerDesc& desc)
{
    @autoreleasepool // as createTexture
    {
        return createMetalUpscaler(*this, desc);
    }
}

Ref<TemporalUpscaler> MetalDevice::createTemporalUpscaler(const TemporalUpscalerDesc& desc)
{
    @autoreleasepool // as createTexture
    {
        return createMetalTemporalUpscaler(*this, desc);
    }
}

Ref<Sampler> MetalDevice::getSampler(const SamplerDesc& desc)
{
    @autoreleasepool // as createTexture
    {
        return mSamplers.get(desc, [this](const SamplerDesc& key) { return createMetalSampler(*this, key); });
    }
}

Texture* MetalDevice::defaultTexture(UniformType type)
{
    size_t slot = 0;
    TextureType texture = TextureType::Tex2D;
    switch (type)
    {
    case UniformType::Sampler2D:        break;
    case UniformType::SamplerCube:      slot = 1; texture = TextureType::Cube; break;
    case UniformType::SamplerCubeArray: slot = 2; texture = TextureType::CubeArray; break;
    case UniformType::Sampler2DShadow:  slot = 3; break; // compared with Never
    case UniformType::Sampler3D:        slot = 4; texture = TextureType::Tex3D; break;
    default:                            return nullptr;
    }
    if (mDefaultTextures[slot].isNull())
    {
        mDefaultTextures[slot] = slot == 3 ? createMetalDefaultShadowTexture(*this) : createMetalDefaultTexture(*this, texture);
    }
    return mDefaultTextures[slot].get();
}

Ref<Program> MetalDevice::createProgram(const ProgramDesc& desc)
{
    return createMetalProgram(*this, desc);
}

U32 metalClearKey(const PassFormats& formats)
{
    U32 key = std::min(formats.colorCount, PipelineDesc::MAX_COLOR_ATTACHMENTS);
    if (formats.depthFormat != Format::Unknown)
    {
        key |= 1u << 4;
    }
    for (U32 i = 0; i < std::min(formats.colorCount, PipelineDesc::MAX_COLOR_ATTACHMENTS); ++i)
    {
        // Every integer format of RHI is unsigned (llrhi.h:128-130); a signed one would be 2.
        const U32 type = isInteger(formats.colorFormats[i]) ? 1 : 0;
        key |= type << (8 + 2 * i);
    }
    return key;
}

ProgramDesc metalClearProgramDesc(U32 key)
{
    const U32 count = key & 15;
    const bool depth = (key & (1u << 4)) != 0;
    const auto typeOf = [key](U32 i) { return (key >> (8 + 2 * i)) & 3; };
    bool oneType = true;
    for (U32 i = 1; i < count; ++i)
    {
        oneType = oneType && typeOf(i) == typeOf(0);
    }
    const char* const types[] = { "vec4", "uvec4", "ivec4" };
    std::string outputs;
    std::string writes;
    for (U32 i = 0; i < count; ++i)
    {
        const std::string type = types[std::min(typeOf(i), 2u)];
        const std::string value = typeOf(i) == 0 ? "clear_color" : type + "(clear_color)";
        if (oneType)
        {
            writes += "    frag_data[" + std::to_string(i) + "] = " + value + ";\n";
        }
        else
        {
            // One output each, which the translator refuses today (it takes one fragment output).
            outputs += "layout(location = " + std::to_string(i) + ") out " + type + " frag_data" + std::to_string(i) + ";\n";
            writes += "    frag_data" + std::to_string(i) + " = " + value + ";\n";
        }
    }
    if (oneType && count > 0)
    {
        outputs = std::string("out ") + types[std::min(typeOf(0), 2u)] + " frag_data[" + std::to_string(count) + "];\n";
    }
    if (depth)
    {
        writes += "    gl_FragDepth = clear_depth;\n";
    }
    ProgramDesc desc;
    desc.name = "clear program " + std::to_string(key);
    // (-1, -1), (3, -1), (-1, 3): counter-clockwise, a front face as GL's, over the whole
    // attachment, which the viewport is during the draw.
    const std::string vertex = "#version 400\n"
                               "void main()\n"
                               "{\n"
                               "    gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);\n"
                               "}\n";
    const std::string fragment = "#version 400\n"
                                 "uniform vec4 clear_color;\n"
                                 "uniform float clear_depth;\n" +
                                 outputs + "void main()\n{\n" + writes + "}\n";
    desc.stages = { { ShaderStage::Vertex, { vertex } }, { ShaderStage::Fragment, { fragment } } };
    return desc;
}

Program* MetalDevice::clearProgram(const PassFormats& formats)
{
    Ref<Program>& program = mClearPrograms[metalClearKey(formats)];
    if (program.isNull() || program->status() == CompileStatus::Failed)
    {
        // Made again after a failure, so a translator registered later is used.
        program = createProgram(metalClearProgramDesc(metalClearKey(formats)));
    }
    // The clear waits for its program as a draw waits for its pipeline.
    program->wait();
    return program->status() == CompileStatus::Ready ? program.get() : nullptr;
}

Ref<Pipeline> MetalDevice::clearPipeline(Program& program, const PipelineDesc& desc)
{
    return mClearPipelines.get({ program.id(), desc }, [this, &program](const PipelineKey& key)
    {
        return createMetalPipeline(*this, program, key.desc);
    });
}

ProgramDesc metalMipProgramDesc()
{
    ProgramDesc desc;
    desc.name = "mip program";
    const std::string vertex = "#version 400\n"
                               "void main()\n"
                               "{\n"
                               "    gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);\n"
                               "}\n";
    // The level's texel (x, y) is gl_FragCoord (x + 0.5, y + 0.5) on both APIs: the translator's GLSL has
    // Vulkan's upper-left origin, which is Metal's row y, and the level's row y as GL's lower-left origin is.
    // The filtered value is written as it is. A 16-bit float render target
    // converts it to a half toward zero, where GL's mips round to the nearest half (the probe's 1x8); that
    // half ULP is accepted, and no rounding is emulated in the shader.
    // mip_size is the level's size in texels (bindForMip), not asked of mip_source: mip_source is a view of one
    // level, and a view whose base level is above 0 can report its parent's level-0 size on GPUs before Apple9
    // (M1, M2), so the levels from 2 on would sample a corner of the level above. It is the value
    // max(textureSize(mip_source, 0) / 2, 1) gives where the view reports its own size, so the levels are the same.
    const std::string fragment = "#version 400\n"
                                 "uniform sampler2D mip_source;\n"
                                 "uniform vec2 mip_size;\n"
                                 "out vec4 frag_color;\n"
                                 "void main()\n"
                                 "{\n"
                                 "    frag_color = texture(mip_source, gl_FragCoord.xy / mip_size);\n"
                                 "}\n";
    desc.stages = { { ShaderStage::Vertex, { vertex } }, { ShaderStage::Fragment, { fragment } } };
    return desc;
}

Program* MetalDevice::mipProgram()
{
    Ref<Program>& program = mMipProgram;
    if (program.isNull() || program->status() == CompileStatus::Failed)
    {
        // Made again after a failure, so a translator registered later is used.
        program = createProgram(metalMipProgramDesc());
    }
    // The mips wait for their program as a draw waits for its pipeline.
    program->wait();
    return program->status() == CompileStatus::Ready ? program.get() : nullptr;
}

Ref<Pipeline> MetalDevice::mipPipeline(Program& program, const PipelineDesc& desc)
{
    return mMipPipelines.get({ program.id(), desc }, [this, &program](const PipelineKey& key)
    {
        return createMetalPipeline(*this, program, key.desc);
    });
}

ProgramDesc metalPresentProgramDesc()
{
    ProgramDesc desc;
    desc.name = "present program";
    // The mip program's triangle over the whole attachment.
    const std::string vertex = "#version 400\n"
                               "void main()\n"
                               "{\n"
                               "    gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);\n"
                               "}\n";
    // gl_FragCoord in the translator's GLSL is Metal's row, from the top (metalMipProgramDesc), and the back
    // buffer's memory row 0 is GL's bottom row, so drawable row r takes back buffer row H - 1 - r: exact,
    // with no filtering. Beyond the back buffer (a drawable larger than it) the drawable is black.
    const std::string fragment = "#version 400\n"
                                 "uniform sampler2D present_source;\n"
                                 "out vec4 frag_color;\n"
                                 "void main()\n"
                                 "{\n"
                                 "    ivec2 size = textureSize(present_source, 0);\n"
                                 "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
                                 "    frag_color = (p.x < size.x && p.y < size.y) ? texelFetch(present_source, ivec2(p.x, size.y - 1 - p.y), 0)\n"
                                 "                                               : vec4(0.0, 0.0, 0.0, 1.0);\n"
                                 "}\n";
    desc.stages = { { ShaderStage::Vertex, { vertex } }, { ShaderStage::Fragment, { fragment } } };
    return desc;
}

Program* MetalDevice::presentProgram()
{
    if (mPresentProgram.isNull() || mPresentProgram->status() == CompileStatus::Failed)
    {
        // Made again after a failure, so a translator registered later is used.
        mPresentProgram = createProgram(metalPresentProgramDesc());
    }
    // The flip pass waits for its program as a draw waits for its pipeline.
    mPresentProgram->wait();
    return mPresentProgram->status() == CompileStatus::Ready ? mPresentProgram.get() : nullptr;
}

Ref<Pipeline> MetalDevice::presentPipeline(Program& program, const PipelineDesc& desc)
{
    return mPresentPipelines.get({ program.id(), desc }, [this, &program](const PipelineKey& key)
    {
        return createMetalPipeline(*this, program, key.desc);
    });
}

ProgramDesc metalFormatCopyProgramDesc()
{
    ProgramDesc desc;
    desc.name = "format copy program";
    // The mip program's triangle over the whole attachment.
    const std::string vertex = "#version 400\n"
                               "void main()\n"
                               "{\n"
                               "    gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);\n"
                               "}\n";
    // copyTexture between formats. The source is a 2D scratch of the region in the source's
    // format with no swizzle (MetalEncoder::formatCopy), and the attachment a scratch of the region in the destination's
    // format, so texel (x, y) of the one is texel (x, y) of the other: gl_FragCoord (x + 0.5, y + 0.5) is the
    // attachment's texel (x, y) on both APIs (metalMipProgramDesc). The stored components go as they are, as
    // glCopyTexSubImage2D reads the framebuffer's: an R8 attachment keeps R, an RG8 one R and G, and a unorm
    // attachment clamps a float source to [0, 1].
    const std::string fragment = "#version 400\n"
                                 "uniform sampler2D copy_source;\n"
                                 "out vec4 frag_color;\n"
                                 "void main()\n"
                                 "{\n"
                                 "    frag_color = texelFetch(copy_source, ivec2(gl_FragCoord.xy), 0);\n"
                                 "}\n";
    desc.stages = { { ShaderStage::Vertex, { vertex } }, { ShaderStage::Fragment, { fragment } } };
    return desc;
}

Program* MetalDevice::formatCopyProgram()
{
    if (mFormatCopyProgram.isNull() || mFormatCopyProgram->status() == CompileStatus::Failed)
    {
        // Made again after a failure, so a translator registered later is used.
        mFormatCopyProgram = createProgram(metalFormatCopyProgramDesc());
    }
    // The copy waits for its program as a draw waits for its pipeline.
    mFormatCopyProgram->wait();
    return mFormatCopyProgram->status() == CompileStatus::Ready ? mFormatCopyProgram.get() : nullptr;
}

Ref<Pipeline> MetalDevice::formatCopyPipeline(Program& program, const PipelineDesc& desc)
{
    return mFormatCopyPipelines.get({ program.id(), desc }, [this, &program](const PipelineKey& key)
    {
        return createMetalPipeline(*this, program, key.desc);
    });
}

// static
PipelineDesc MetalDevice::formatCopyPipelineDesc(Format to)
{
    PassFormats formats;
    formats.colorCount = 1;
    formats.colorFormats[0] = to;
    PipelineDesc desc;
    applyPassFormats(formats, desc);
    desc.attributeMask = 0;
    return desc;
}

std::string MetalDevice::formatCopyUnavailable(Format to)
{
    Program* program = formatCopyProgram();
    if (!program)
    {
        return "the format copy program cannot be made";
    }
    // Waited for; nil after a WARN line, and the failed key stays refused in the cache
    if (!metalPipelineState(formatCopyPipeline(*program, formatCopyPipelineDesc(to)).get()))
    {
        return "the format copy pipeline for the destination's format cannot be made";
    }
    return {};
}

std::string MetalDevice::copyRefusalBetween(Format from, Format to)
{
    std::string why = copyRefusal(mCaps, from, to);
    if (why.empty() && from != to)
    {
        // A copy between formats is a draw, so LLGPUCopy::canFromFramebuffer refuses it when the
        // draw cannot be made, before scaleDown re-specifies the texture
        why = formatCopyUnavailable(to);
    }
    return why;
}

std::vector<Ref<Query>> MetalDevice::createQueries(QueryType type, U32 count)
{
    // The sample queries on the visibility result buffer (MetalQuery), the time queries on the timestamp heap
    // (MetalTimerQuery). Primitive queries are refused by the capabilities (primitivesGeneratedQuery is false),
    // and time queries too without the heap, as GL refuses what it lacks.
    if (!on_main_thread())
    {
        warnCreateFailed("query", {}, "queries are made, begun and read on the main thread (decision 14)");
        return {};
    }
    const std::string why = validateQuery(type, mCaps);
    if (!why.empty())
    {
        warnCreateFailed("query", {}, why);
        return {};
    }
    if (!mVisibility && count > 0 && type != QueryType::TimeElapsed)
    {
        // Always resident: admitted once and never retired before the device ends, as the buffer pool's chunks. Every
        // render pass opened from now on carries it (MetalEncoder::open). Without it the queries read as visible.
        mVisibility = newResidentBuffer(U64(mVisibilitySlots) * sizeof(U64), "visibility results");
        if (mVisibility)
        {
            mVisibilityUses.assign(mVisibilitySlots, VisibilityUse());
            std::memset(mVisibility.contents, 0, size_t(mVisibilitySlots) * sizeof(U64));
        }
    }
    std::vector<Ref<Query>> queries;
    queries.reserve(count);
    for (U32 i = 0; i < count; ++i)
    {
        if (type == QueryType::TimeElapsed)
        {
            queries.push_back(new MetalTimerQuery(*this)); // validateQuery: only with the heap
        }
        else
        {
            queries.push_back(new MetalQuery(*this, type));
        }
    }
    return queries;
}

S64 MetalDevice::takeVisibilitySlot(const std::shared_ptr<MetalQueryRun>& run)
{
    ++mQueryStats.frameSampleBegins; // the most in one frame, with or without a slot
    if (!mVisibility)
    {
        ++mQueryStats.noSlot;
        return -1;
    }
    const U32 slot = mVisibilityNext;
    VisibilityUse& use = mVisibilityUses[slot];
    // completedFrame() is always below the frame being recorded (FrameLifecycle::idle marks the one before it), so a
    // slot is never reused within its own frame, while the GPU may still write it.
    if (use.frame != 0 && use.frame > mLifecycle.completedFrame())
    {
        ++mQueryStats.noSlot;
        if (mDebugChecks)
        {
            LL_ERRS("RHI") << "The visibility result ring is full: " << mVisibilitySlots
                           << " sample queries begun in frames that have not finished (slot " << slot << ", frame "
                           << use.frame << ")" << LL_ENDL;
        }
        else if (mQueryStats.noSlot % 1000 == 1)
        {
            LL_WARNS("RHI") << "The visibility result ring is full (" << mVisibilitySlots
                            << " slots in unfinished frames); the query reads as visible (" << mQueryStats.noSlot
                            << " so far)" << LL_ENDL;
        }
        return -1;
    }
    U64* values = static_cast<U64*>(mVisibility.contents);
    if (std::shared_ptr<MetalQueryRun> earlier = use.run.lock(); earlier && !earlier->captured)
    {
        // U1: the earlier run's frame has finished, so this is its final value; it is kept for a late read (a group
        // outside the cull, a probe with the eye inside its radius), as GL keeps a name's result until its next begin.
        earlier->kept = values[slot];
        earlier->captured = true;
        ++mQueryStats.kept;
    }
    // Nothing recorded from here on is sent yet, so the GPU writes the slot only after this.
    values[slot] = 0;
    use.frame = mLifecycle.currentFrame();
    use.run = run;
    mVisibilityNext = (slot + 1) % mVisibilitySlots;
    ++mQueryStats.begun;
    return S64(slot);
}

S64 MetalDevice::takeTimerPair(const std::shared_ptr<MetalTimerRun>& run)
{
    ++mQueryStats.frameTimeBegins;
    if (!mTimestamps || mTimerUses.empty())
    {
        ++mQueryStats.timeNoPair;
        return -1;
    }
    const U32 pair = mTimerNext;
    TimerUse& use = mTimerUses[pair];
    std::shared_ptr<MetalTimerRun> earlier = use.run.lock();
    // A pair is free once its frame has finished, or once the submit after its earlier run's end was signalled: the GPU
    // has then written both stamps (a shader-profile frame reads each of its queries at once, result(true), and so
    // reuses pairs within the frame). The pair's own copy of the end's stamp decides, since the run is gone once its
    // query began again (LLGLSLShader's profile frame begins one query at every bind); only a
    // written end is stamped (endTimer). Never invalidated: the CPU knows which run a pair holds.
    const MetalReadbackStamp* end = use.endStamp.get();
    const bool endSignalled = end && end->serial && end->event.signaledValue >= end->serial;
    if (use.frame != 0 && use.frame > mLifecycle.completedFrame() && !endSignalled)
    {
        ++mQueryStats.timeNoPair;
        if (mQueryStats.timeNoPair % 1000 == 1)
        {
            LL_WARNS("RHI") << "The timer query ring is full (" << mTimerPairs
                            << " pairs in unfinished frames); the query reads 0 ns (" << mQueryStats.timeNoPair
                            << " so far)" << LL_ENDL;
        }
        return -1;
    }
    if (earlier && !earlier->captured && earlier->endWritten)
    {
        // As the visibility ring's U1: the earlier run's ticks are final, kept for a late read (an avatar's profile read
        // up to 5 frames later); a failed resolve keeps zeros, which read as 0 ns and timer_invalid.
        resolveTimestamps(S64(pair), earlier->kept);
        earlier->captured = true;
        ++mQueryStats.timeKept;
    }
    use.frame = mLifecycle.currentFrame();
    use.run = run;
    use.endStamp = run->endStamp;
    mTimerNext = (pair + 1) % mTimerPairs;
    ++mQueryStats.timeBegun;
    return S64(pair);
}

bool MetalDevice::writeTimestamp(U32 index)
{
    if (!mTimestamps)
    {
        return false;
    }
    // Between encoders (MTL4CommandBuffer.h:162-174): the copies' encoder ends first, as at a render encoder's begin;
    // the stamp comes after the command buffer's earlier work is complete.
    endCopies();
    [commandBuffer() writeTimestampIntoHeap:mTimestamps atIndex:index];
    return true;
}

namespace
{
    /// ticks.size() entries of the heap from first, resolved on the CPU (MTL4Counters.h:81-92); the GPU has finished
    /// writing them (the caller waited for a signal after them, MTL4Counters.h's note). False, with ticks unchanged,
    /// when Metal gave no data or too little.
    API_AVAILABLE(macos(26.0)) bool metalResolveTicks(IdMTL4CounterHeap heap, NSUInteger first, std::span<U64> ticks)
    {
        @autoreleasepool // resolveCounterRange: returns an autoreleased NSData
        {
            NSData* data = [heap resolveCounterRange:NSMakeRange(first, ticks.size())];
            if (!data || data.length < ticks.size() * sizeof(MTL4TimestampHeapEntry))
            {
                return false;
            }
            const MTL4TimestampHeapEntry* entries = static_cast<const MTL4TimestampHeapEntry*>(data.bytes);
            for (size_t i = 0; i < ticks.size(); ++i)
            {
                ticks[i] = entries[i].timestamp;
            }
            return true;
        }
    }
}

bool MetalDevice::resolveTimestamps(S64 pair, std::array<U64, 2>& ticks) const
{
    ticks = { 0, 0 };
    if (!mTimestamps || pair < 0)
    {
        return false;
    }
    std::array<U64, 2> resolved{};
    if (!metalResolveTicks(mTimestamps, NSUInteger(pair) * 2, resolved))
    {
        return false;
    }
    ticks = resolved;
    return true;
}

// The pass timer ------------------------------------------------------------------------------------

namespace
{
    /// An attachment's format, short, for the pass timer's table.
    const char* passFormatName(Format format)
    {
        switch (format)
        {
        case Format::R8Unorm:               return "R8";
        case Format::RG8Unorm:              return "RG8";
        case Format::RGBA8Unorm:            return "RGBA8";
        case Format::RGBA8Unorm_sRGB:       return "RGBA8 sRGB";
        case Format::BGRA8Unorm:            return "BGRA8";
        case Format::R16Float:              return "R16F";
        case Format::RG16Float:             return "RG16F";
        case Format::RGBA16Float:           return "RGBA16F";
        case Format::RGBA16Unorm:           return "RGBA16";
        case Format::R32Float:              return "R32F";
        case Format::RG32Float:             return "RG32F";
        case Format::RGBA32Float:           return "RGBA32F";
        case Format::RGB10A2Unorm:          return "RGB10A2";
        case Format::RG11B10Float:          return "RG11B10F";
        case Format::R8Uint:                return "R8U";
        case Format::R16Uint:               return "R16U";
        case Format::R32Uint:               return "R32U";
        case Format::Depth32Float:          return "D32F";
        case Format::Depth32Float_Stencil8: return "D32F S8";
        default:                            return "other";
        }
    }

    /// A render pass's attachments for the table: the first one's size at its level, then each format, depth last,
    /// as "[1920x1080 RGBA16F+D32F S8]".
    std::string passAttachments(const RenderPassDesc& pass)
    {
        std::string formats;
        U32 width = 0;
        U32 height = 0;
        auto add = [&formats, &width, &height](const Ref<Texture>& texture, U32 level)
        {
            if (texture.isNull())
            {
                return;
            }
            const TextureDesc& desc = texture->desc();
            if (width == 0)
            {
                width = std::max(1u, desc.width >> level);
                height = std::max(1u, desc.height >> level);
            }
            formats += formats.empty() ? "" : "+";
            formats += passFormatName(desc.format);
        };
        for (U32 i = 0; i < pass.colorCount; ++i)
        {
            add(pass.color[i].texture, pass.color[i].level);
        }
        add(pass.depth.texture, pass.depth.level);
        if (formats.empty())
        {
            return "[no attachments]";
        }
        return "[" + std::to_string(width) + "x" + std::to_string(height) + " " + formats + "]";
    }

    API_AVAILABLE(macos(26.0)) const char* passKindName(MetalPassTimes::Kind kind)
    {
        switch (kind)
        {
        case MetalPassTimes::Kind::Render:  return "render";
        case MetalPassTimes::Kind::Compute: return "compute";
        case MetalPassTimes::Kind::Copies:  return "copies";
        }
        return "";
    }

    /// The program ids of a render and a compute pass named by their first program that have none yet.
    constexpr U32 PASS_NO_DRAW = 1;
    constexpr U32 PASS_NO_DISPATCH = 2;
}

bool MetalPassTimes::start(IdMTLDevice device, U64 ticksPerSecond, U32 frames, std::string path,
                           std::function<void()> done)
{
    if (mPhase != Phase::Off)
    {
        LL_WARNS("LuminousPassTimes") << "The pass timer is running already" << LL_ENDL;
        return false;
    }
    if (ticksPerSecond == 0 || frames == 0)
    {
        LL_WARNS("LuminousPassTimes") << "The GPU gives no timestamps (no timestamp heap or tick rate); the pass timer "
                                      << "is off" << LL_ENDL;
        return false;
    }
    for (Record& record : mRecords)
    {
        record = Record();
        record.heap = metalTimestampHeap(device, METAL_PASS_TIME_PAIRS, @"pass times", "the pass timer is off");
        if (!record.heap)
        {
            // Nothing was stamped into the heaps made so far, so they go at once.
            for (Record& made : mRecords)
            {
                made = Record();
            }
            return false;
        }
    }
    mTicksPerSecond = ticksPerSecond;
    mFrames = frames;
    mFirst = 0;
    mLast = 0;
    mPath = std::move(path);
    mDone = std::move(done);
    mOpen = nullptr;
    mNames.clear();
    mNameIds.clear();
    mPrograms = { std::string(), std::string("(no draw)"), std::string("(no dispatch)") };
    mProgramIds.clear();
    mRows.clear();
    mResolvedFrames = 0;
    mSubmitted = 0;
    mSinceResolved = 0;
    mEncoders = 0;
    mKindTicks = {};
    mOwnTicks = 0;
    mSpanTicks = 0;
    mLeftOut = 0;
    mUnended = 0;
    mUnreadable = 0;
    mLostFrames = 0;
    mPhase = Phase::Armed;
    LL_INFOS("LuminousPassTimes") << "Timing every render and compute pass on the GPU over the next " << frames
                                  << " frames (" << METAL_PASS_TIME_PAIRS << " encoders a frame at most)" << LL_ENDL;
    return true;
}

void MetalPassTimes::stop(FrameLifecycle& lifecycle)
{
    if (mPhase == Phase::Off)
    {
        return;
    }
    release(lifecycle);
    mDone = nullptr;
    LL_INFOS("LuminousPassTimes") << "The pass timer stopped before its table" << LL_ENDL;
}

void MetalPassTimes::release(FrameLifecycle& lifecycle)
{
    // Metal 4's command buffers do not keep what they use, so each heap lives until the frames in flight have finished.
    for (Record& record : mRecords)
    {
        if (IdMTL4CounterHeap heap = record.heap)
        {
            lifecycle.release([heap]() mutable { heap = nil; });
        }
        record = Record();
    }
    mOpen = nullptr;
    mPhase = Phase::Off;
    mFirst = 0;
    mLast = 0;
}

U32 MetalPassTimes::nameId(Kind kind, std::string_view name)
{
    std::string key(1, char('0' + U8(kind)));
    key += name;
    const auto [found, added] = mNameIds.try_emplace(std::move(key), U32(mNames.size()));
    if (added)
    {
        mNames.emplace_back(kind, std::string(name));
    }
    return found->second;
}

MetalPassStamp MetalPassTimes::begin(U32 name, U64 frame)
{
    if (!mOpen || frame == 0 || mOpen->frame != frame)
    {
        return MetalPassStamp();
    }
    if (mOpen->pairs.size() >= METAL_PASS_TIME_PAIRS)
    {
        ++mOpen->leftOut;
        return MetalPassStamp();
    }
    MetalPassStamp stamp;
    stamp.frame = frame;
    stamp.pair = U32(mOpen->pairs.size());
    mOpen->pairs.push_back({ name, 0, false });
    return stamp;
}

MetalPassStamp MetalPassTimes::beginRender(IdMTL4RenderCommandEncoder encoder, const RenderPassDesc& pass, U64 frame)
{
    if (!mOpen)
    {
        return MetalPassStamp();
    }
    // The target stack's passes all carry the stack's name, and some passes none: those are told apart by their
    // attachments and their first program.
    const bool byProgram = pass.stack || pass.name.empty();
    std::string name = pass.name.empty() ? std::string("unnamed (render)") : pass.name;
    if (byProgram)
    {
        name += " " + passAttachments(pass);
    }
    MetalPassStamp stamp = begin(nameId(Kind::Render, name), frame);
    if (stamp.frame)
    {
        [encoder writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed
                                    afterStage:MTLRenderStageVertex
                                      intoHeap:mOpen->heap
                                       atIndex:NSUInteger(stamp.pair) * 2];
        if (byProgram)
        {
            mOpen->pairs.back().program = PASS_NO_DRAW;
            stamp.wantsProgram = true;
        }
    }
    return stamp;
}

MetalPassStamp MetalPassTimes::beginOwnRender(IdMTL4RenderCommandEncoder encoder, NSString* label, U64 frame)
{
    if (!mOpen)
    {
        return MetalPassStamp();
    }
    const std::string name = label.length ? std::string(label.UTF8String) : std::string("unnamed (render)");
    MetalPassStamp stamp = begin(nameId(Kind::Render, name), frame);
    if (stamp.frame)
    {
        [encoder writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed
                                    afterStage:MTLRenderStageVertex
                                      intoHeap:mOpen->heap
                                       atIndex:NSUInteger(stamp.pair) * 2];
    }
    return stamp;
}

MetalPassStamp MetalPassTimes::beginCompute(IdMTL4CommandBuffer buffer, std::string_view name, Kind kind, U64 frame)
{
    if (!mOpen)
    {
        return MetalPassStamp();
    }
    const bool unnamed = name.empty();
    const std::string text = !unnamed               ? std::string(name)
                             : kind == Kind::Copies ? std::string("unnamed (copies)")
                                                    : std::string("unnamed (compute)");
    MetalPassStamp stamp = begin(nameId(kind, text), frame);
    if (stamp.frame)
    {
        [buffer writeTimestampIntoHeap:mOpen->heap atIndex:NSUInteger(stamp.pair) * 2];
        if (unnamed && kind == Kind::Compute)
        {
            mOpen->pairs.back().program = PASS_NO_DISPATCH;
            stamp.wantsProgram = true;
        }
    }
    return stamp;
}

void MetalPassTimes::endRender(IdMTL4RenderCommandEncoder encoder, const MetalPassStamp& stamp)
{
    if (!mOpen || stamp.frame == 0 || stamp.frame != mOpen->frame || stamp.pair >= mOpen->pairs.size())
    {
        return;
    }
    [encoder writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed
                                afterStage:MTLRenderStageFragment
                                  intoHeap:mOpen->heap
                                   atIndex:NSUInteger(stamp.pair) * 2 + 1];
    mOpen->pairs[stamp.pair].ended = true;
}

void MetalPassTimes::endCompute(IdMTL4CommandBuffer buffer, const MetalPassStamp& stamp)
{
    if (!mOpen || stamp.frame == 0 || stamp.frame != mOpen->frame || stamp.pair >= mOpen->pairs.size())
    {
        return;
    }
    [buffer writeTimestampIntoHeap:mOpen->heap atIndex:NSUInteger(stamp.pair) * 2 + 1];
    mOpen->pairs[stamp.pair].ended = true;
}

void MetalPassTimes::noteProgram(MetalPassStamp& stamp, NSString* label)
{
    if (!stamp.wantsProgram)
    {
        return;
    }
    stamp.wantsProgram = false;
    if (!mOpen || stamp.frame != mOpen->frame || stamp.pair >= mOpen->pairs.size())
    {
        return;
    }
    const std::string program = label.length ? std::string(label.UTF8String) : std::string("(unlabelled program)");
    const auto [found, added] = mProgramIds.try_emplace(program, U32(mPrograms.size()));
    if (added)
    {
        mPrograms.push_back(program);
    }
    mOpen->pairs[stamp.pair].program = found->second;
}

void MetalPassTimes::resolve(Record& record)
{
    const size_t entries = record.pairs.size() * 2;
    std::vector<U64> ticks(entries, 0);
    if (entries > 0 && !metalResolveTicks(record.heap, 0, ticks))
    {
        ticks.assign(entries, 0); // every pair reads as unreadable
    }
    // In the order the encoders were begun, which is the queue's: a pass's own time is what no earlier
    // encoder of the frame covered, as MetalGpuBusy counts overlapping command buffers once.
    U64 latestEnd = 0;
    U64 firstBegin = 0;
    U64 lastEnd = 0;
    for (size_t i = 0; i < record.pairs.size(); ++i)
    {
        const Pair& pair = record.pairs[i];
        if (!pair.ended)
        {
            ++mUnended;
            continue;
        }
        const U64 begin = ticks[i * 2];
        const U64 end = ticks[i * 2 + 1];
        if (begin == 0 || end < begin)
        {
            ++mUnreadable;
            continue;
        }
        const U64 from = std::max(begin, latestEnd);
        const U64 own = end > from ? end - from : 0;
        Row& row = mRows[{ pair.name, pair.program }];
        ++row.encoders;
        row.ticks += end - begin;
        row.ownTicks += own;
        mKindTicks[size_t(mNames[pair.name].first)] += end - begin;
        mOwnTicks += own;
        ++mEncoders;
        latestEnd = std::max(latestEnd, end);
        firstBegin = firstBegin == 0 ? begin : std::min(firstBegin, begin);
        lastEnd = std::max(lastEnd, end);
    }
    mSpanTicks += lastEnd > firstBegin ? lastEnd - firstBegin : 0;
    mLeftOut += record.leftOut;
    ++mResolvedFrames;
    if (entries > 0)
    {
        // The slot's next frame reads 0 where it writes nothing; the GPU is done with the heap (MTL4Counters.h:94-102).
        [record.heap invalidateCounterRange:NSMakeRange(0, entries)];
    }
    record.frame = 0;
    record.pairs.clear();
    record.leftOut = 0;
}

MetalPassTimes::FrameEnd MetalPassTimes::frameEnd(U64 current, U64 completed)
{
    if (mPhase == Phase::Off)
    {
        return FrameEnd::Nothing;
    }
    // The timed frames the GPU has finished, from any slot.
    for (Record& record : mRecords)
    {
        if (record.frame != 0 && record.frame <= completed)
        {
            resolve(record);
        }
    }
    FrameEnd result = FrameEnd::Nothing;
    if (mPhase == Phase::Armed)
    {
        mFirst = current;
        mLast = current + mFrames - 1;
        mPhase = Phase::Recording;
        result = FrameEnd::Began;
    }
    mOpen = nullptr;
    if (mPhase == Phase::Recording && current > mLast)
    {
        mPhase = Phase::Resolving;
    }
    if (mPhase == Phase::Recording)
    {
        Record& record = mRecords[current % mRecords.size()];
        if (record.frame != 0)
        {
            // FrameLifecycle gave up waiting for the slot's earlier frame: its stamps are dropped.
            ++mLostFrames;
        }
        record.frame = current;
        record.pairs.clear();
        record.leftOut = 0;
        mOpen = &record;
        return result;
    }
    if (mPhase == Phase::Resolving)
    {
        const bool pending = std::any_of(mRecords.begin(), mRecords.end(),
                                         [](const Record& record) { return record.frame != 0; });
        if (!pending)
        {
            ++mSinceResolved;
            return FrameEnd::Finished;
        }
    }
    return result;
}

std::string MetalPassTimes::table(const MetalGpuBusy& busy, U64 received) const
{
    const U64 timedFrames = mLast >= mFirst && mFirst != 0 ? mLast - mFirst + 1 : 0;
    const double frames = double(std::max<U64>(mResolvedFrames, 1));
    auto ms = [this, frames](U64 ticks) { return double(ticks) * 1000.0 / double(mTicksPerSecond) / frames; };
    const double busyMs = busy.busy * 1000.0 / double(std::max<U64>(timedFrames, 1));
    U64 sumTicks = 0;
    for (U64 ticks : mKindTicks)
    {
        sumTicks += ticks;
    }

    std::vector<std::pair<std::pair<U32, U32>, Row>> rows(mRows.begin(), mRows.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ticks > b.second.ticks; });
    auto rowName = [this](const std::pair<U32, U32>& key)
    {
        std::string name = mNames[key.first].second;
        if (key.second != 0)
        {
            name += " " + mPrograms[key.second];
        }
        return name;
    };

    std::ostringstream out;
    char line[160];
    out << "Metal pass times over " << mResolvedFrames << " frames (" << mFirst << " to " << mLast
        << "), GPU milliseconds per frame\n";
    std::snprintf(line, sizeof(line), "GPU busy (commit feedback): %.3f ms, %llu command buffers reported of %llu\n",
                  busyMs, (unsigned long long)received, (unsigned long long)mSubmitted);
    out << line;
    std::snprintf(line, sizeof(line),
                  "Passes: sum %.3f ms (render %.3f, compute %.3f, copies %.3f), without overlap %.3f ms, first begin "
                  "to last end %.3f ms\n",
                  ms(sumTicks), ms(mKindTicks[size_t(Kind::Render)]), ms(mKindTicks[size_t(Kind::Compute)]),
                  ms(mKindTicks[size_t(Kind::Copies)]), ms(mOwnTicks), ms(mSpanTicks));
    out << line;
    std::snprintf(line, sizeof(line),
                  "Encoders: %.1f per frame timed; left out %llu (%u a frame at most), without an end %llu, unreadable "
                  "%llu; frames lost %llu\n",
                  double(mEncoders) / frames, (unsigned long long)mLeftOut, METAL_PASS_TIME_PAIRS,
                  (unsigned long long)mUnended, (unsigned long long)mUnreadable, (unsigned long long)mLostFrames);
    out << line;
    out << "      ms   own ms  % busy  per frame  kind     pass\n";
    for (const auto& [key, row] : rows)
    {
        const double rowMs = ms(row.ticks);
        std::snprintf(line, sizeof(line), "%8.3f %8.3f %7.1f %10.2f  %-8s ", rowMs, ms(row.ownTicks),
                      busyMs > 0.0 ? rowMs * 100.0 / busyMs : 0.0, double(row.encoders) / frames,
                      passKindName(mNames[key.first].first));
        out << line << rowName(key) << "\n";
    }
    // The passes without a name of their own, so they can be given one.
    std::vector<std::string> unnamed;
    for (const auto& [key, row] : rows)
    {
        if (mNames[key.first].second.rfind("unnamed (", 0) == 0)
        {
            std::snprintf(line, sizeof(line), "%8.3f ms, %.2f per frame: ", ms(row.ticks),
                          double(row.encoders) / frames);
            unnamed.push_back(line + rowName(key));
        }
    }
    out << "Passes without a name: " << (unnamed.empty() ? "none" : std::to_string(unnamed.size()));
    for (const std::string& entry : unnamed)
    {
        out << "\n  " << entry;
    }
    return out.str();
}

std::function<void()> MetalPassTimes::finish(const MetalGpuBusy& busy, U64 received, FrameLifecycle& lifecycle)
{
    const std::string text = table(busy, received);
    LL_INFOS("LuminousPassTimes") << text << LL_ENDL;
    if (!mPath.empty())
    {
        std::ofstream file(mPath, std::ios::out | std::ios::trunc);
        file << text << '\n';
        if (!file)
        {
            LL_WARNS("LuminousPassTimes") << "Could not write the pass times to " << mPath << LL_ENDL;
        }
    }
    std::function<void()> done = std::move(mDone);
    mDone = nullptr;
    release(lifecycle);
    return done;
}

bool MetalDevice::startPassTimes(U32 frames, const std::string& path, std::function<void()> done)
{
    return mPassTimes.start(mDevice, mTimestampFrequency, frames, path, std::move(done));
}

void MetalDevice::stopPassTimes()
{
    mPassTimes.stop(mLifecycle);
    std::lock_guard<std::mutex> lock(mFeedback->mutex);
    mFeedback->timedFirst = 0;
    mFeedback->timedLast = 0;
}

void MetalDevice::passTimesAtFrameEnd()
{
    const MetalPassTimes::FrameEnd end = mPassTimes.frameEnd(mLifecycle.currentFrame(), mLifecycle.completedFrame());
    if (end == MetalPassTimes::FrameEnd::Began)
    {
        std::lock_guard<std::mutex> lock(mFeedback->mutex);
        mFeedback->timedFirst = mPassTimes.firstFrame();
        mFeedback->timedLast = mPassTimes.lastFrame();
        mFeedback->timedGpu = MetalGpuBusy();
        mFeedback->timedReceived = 0;
        return;
    }
    if (end != MetalPassTimes::FrameEnd::Finished)
    {
        return;
    }
    MetalGpuBusy busy;
    U64 received = 0;
    {
        std::lock_guard<std::mutex> lock(mFeedback->mutex);
        busy = mFeedback->timedGpu;
        received = mFeedback->timedReceived;
    }
    // The handlers report soon after the GPU has finished; they are not waited for, and after 60 frame ends the
    // table is written with what has reported.
    if (received < mPassTimes.submitted() && mPassTimes.framesSinceResolved() < 60)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mFeedback->mutex);
        mFeedback->timedFirst = 0;
        mFeedback->timedLast = 0;
    }
    if (std::function<void()> done = mPassTimes.finish(busy, received, mLifecycle))
    {
        done();
    }
}

Ref<Pipeline> MetalDevice::getPipeline(Program& program, const PipelineDesc& desc)
{
    // A program still being made is waited for first.
    if (program.status() == CompileStatus::Pending)
    {
        program.wait();
    }
    // One pipeline per program and normalized description; a refused one is not cached.
    // The pipeline comes back at once, Pending until its shared state is made.
    return mPipelines.get({ program.id(), desc }, [this, &program](const PipelineKey& key)
    {
        return createMetalPipeline(*this, program, key.desc);
    });
}

IdMTL4Compiler MetalDevice::compiler()
{
    if (mCompiler)
    {
        return mCompiler;
    }
    // MTLDevice.h:1463; the label is MTL4Compiler.h:45-48's.
    MTL4CompilerDescriptor* descriptor = [MTL4CompilerDescriptor new];
    descriptor.label = @"Viewer Lab";
    NSError* error = nil;
    mCompiler = [mDevice newCompilerWithDescriptor:descriptor error:&error];
    if (!mCompiler)
    {
        LL_WARNS("RHI") << "No Metal shader compiler: "
                        << (error ? error.localizedDescription.UTF8String : "no error given") << LL_ENDL;
        return nil;
    }
    const char* mode = METAL_MATH_MODE == MTLMathModeSafe ? "Safe" : (METAL_MATH_MODE == MTLMathModeRelaxed ? "Relaxed" : "Fast");
    const char* functions = METAL_MATH_FUNCTIONS == MTLMathFloatingPointFunctionsFast ? "Fast" : "Precise";
    // The compiler's tasks run side by side on shouldMaximizeConcurrentCompilation's threads
    // (MTLDevice.h:1310-1321), and the jobs are translated on the pool's.
    LL_INFOS("RHI") << "Metal shaders: MSL 4.0, math mode " << mode << ", functions " << functions << " (P3-01b); "
                    << mDevice.maximumConcurrentCompilationTaskCount << " concurrent compilation tasks, "
                    << MetalProgramPool::threadCount() << " translation threads (P3-01d) at " << metalQosText(mProgramPool.qos())
                    << " QoS (P3-04c)" << LL_ENDL;
    // Golden level 2 reads this line and holds each name to ultra's GL render profile
    std::string safe;
    for (const std::string_view name : METAL_SAFE_MATH_PROGRAMS)
    {
        safe += (safe.empty() ? "'" : ", '") + std::string(name) + "'";
    }
    LL_INFOS("RHI") << "Metal safe math programs (P3-04a): " << safe << LL_ENDL;
    if (metalMathKnobs().any())
    {
        LL_INFOS("RHI") << "Metal math knobs (development, P3-04a S4): " << metalMathKnobs().describe() << LL_ENDL;
    }
    if (mCompileSlow)
    {
        LL_INFOS("RHI") << "Metal compile knob (development, P3-04c): LL_METAL_COMPILE_SLOW, the translation threads at "
                           "utility QoS and no wait takes the pool's queue"
                        << LL_ENDL;
    }
    return mCompiler;
}

Ref<DepthStencilState> MetalDevice::getDepthStencilState(const DepthStencilDesc& desc)
{
    return mDepthStencils.get(desc, [this](const DepthStencilDesc& key) -> Ref<DepthStencilState>
    {
        IdMTLDepthStencilState state = [mDevice newDepthStencilStateWithDescriptor:metalDepthStencilDescriptor(key)];
        if (!state)
        {
            warnCreateFailed("depth stencil state", {}, "Metal gave none");
            return nullptr;
        }
        return new MetalDepthStencilState(mLifecycle, key, state);
    });
}

Surface* MetalDevice::surface()
{
    // Null for a device made without a window, as GL's, with no WARN line.
    return mSurface.get();
}

bool MetalDevice::createSurface(CAMetalLayer* layer, U32 width, U32 height)
{
    Ref<MetalSurface> surface = new MetalSurface(*this, layer, width, height);
    if (!surface->ok())
    {
        return false; // createTexture wrote the WARN line
    }
    // The layer's own set on the queue, after the surface gave the layer its device.
    if (!mResidency.attachLayer(mQueue, layer))
    {
        return false;
    }
    mSurface = surface;
    return true;
}

void MetalDevice::waitForUploads(IdMTLSharedEvent event, U64 value)
{
    if (mUploadWait && mUploadWait != event)
    {
        // One upload queue and one event per device, so this does not happen.
        LL_WARNS("RHI") << "waitForUploads: a second upload event; the next submit waits for the first" << LL_ENDL;
        return;
    }
    mUploadWait = event;
    mUploadWaitValue = std::max(mUploadWaitValue, value);
}

void MetalDevice::setPendingDrawable(IdCAMetalDrawable drawable)
{
    // One present per swap, so one is pending at a time; a second present before a submit waits with the first.
    mPendingDrawables.push_back(drawable);
}

IdMTLDepthStencilState MetalDevice::depthState(DepthStencilState* state)
{
    // A null binding is DepthStencilDesc(), as on GL; a creation that failed is tried again at the next ask.
    if (!state)
    {
        if (mDefaultDepth.isNull())
        {
            mDefaultDepth = getDepthStencilState(DepthStencilDesc());
        }
        state = mDefaultDepth.get();
    }
    return state ? static_cast<MetalDepthStencilState*>(state)->state() : nil;
}

void MetalDevice::pushGroup(std::string_view name)
{
    endCopies();
    if (mGroupEncoder)
    {
        // While a render encoder is open, its groups are the encoder's (MTL4CommandEncoder.h:161-164).
        mGroups.emplace_back(name);
        [mGroupEncoder pushDebugGroup:metalString(name)];
        return;
    }
    IdMTL4CommandBuffer buffer = commandBuffer();
    mGroups.emplace_back(name);
    [buffer pushDebugGroup:metalString(name)];
    ++mBufferGroups;
    noteGroupsWhileEncoding();
}

void MetalDevice::popGroups(U32 count)
{
    endCopies();
    for (U32 i = 0; i < count && !mGroups.empty(); ++i)
    {
        if (mGroupEncoder)
        {
            [mGroupEncoder popDebugGroup];
        }
        else
        {
            [commandBuffer() popDebugGroup];
            --mBufferGroups;
        }
        mGroups.pop_back();
    }
}

void MetalDevice::endGroups()
{
    if (mGroupEncoder)
    {
        for (size_t i = 0; i < mGroups.size(); ++i)
        {
            [mGroupEncoder popDebugGroup];
        }
        mGroupEncoder = nil;
    }
    else if (mCommandBuffer)
    {
        for (size_t i = 0; i < mGroups.size(); ++i)
        {
            [mCommandBuffer popDebugGroup];
        }
        mBufferGroups = 0;
    }
}

void MetalDevice::beginGroups(IdMTL4CommandEncoder encoder)
{
    if (encoder)
    {
        for (const std::string& group : mGroups)
        {
            [encoder pushDebugGroup:metalString(group)];
        }
        mGroupEncoder = encoder;
    }
    else if (mCommandBuffer)
    {
        // Without a command buffer, commandBuffer() pushes them when it makes the next.
        for (const std::string& group : mGroups)
        {
            [mCommandBuffer pushDebugGroup:metalString(group)];
        }
        mBufferGroups = U32(mGroups.size());
    }
}

void MetalDevice::noteGroupsWhileEncoding(bool encoding)
{
    if (encoding || mEncoder.openEncoder())
    {
        mBufferGroupsWhileEncoding = std::max(mBufferGroupsWhileEncoding, mBufferGroups);
    }
}

bool MetalDevice::reached(const Slot& slot) const
{
    return slot.event.signaledValue >= slot.lastSerial;
}

IdMTL4CommandBuffer MetalDevice::commandBuffer()
{
    if (mCommandBuffer)
    {
        return mCommandBuffer;
    }
    const U64 frame = mLifecycle.currentFrame();
    Slot& slot = slotOf(frame);
    if (slot.frame != frame)
    {
        // The slot's allocator is reset only when its event has reached the slot's last serial;
        // FrameLifecycle has waited for that frame before this one began.
        if (!reached(slot) && ![slot.event waitUntilSignaledValue:slot.lastSerial timeoutMS:METAL_WAIT_MS])
        {
            LL_WARNS("RHI") << "Frame " << slot.frame << " did not finish on the GPU within " << METAL_WAIT_MS
                            << " ms; frame " << frame << " keeps its command allocator's memory" << LL_ENDL;
        }
        else
        {
            [slot.allocator reset];
        }
        slot.frame = frame;
        slot.parts = 0;
        // The frame's first command buffer, so a capture asked for begins before it is made
        // (MTLCaptureManager.h:94).
        beginCapture();
    }
    mCommandBuffer = [mDevice newCommandBuffer];
    [mCommandBuffer beginCommandBufferWithAllocator:slot.allocator];
    ++slot.parts;
    std::string label = "Frame " + std::to_string(frame);
    if (slot.parts > 1)
    {
        label += " part " + std::to_string(slot.parts);
    }
    mCommandBuffer.label = metalString(label);
    // Debug groups stay open across submissions (a command buffer per submission).
    for (const std::string& group : mGroups)
    {
        [mCommandBuffer pushDebugGroup:metalString(group)];
    }
    mBufferGroups = U32(mGroups.size());
    return mCommandBuffer;
}

void MetalDevice::submit()
{
    // Every way here closes the pass's native encoder first (endFrame, submitPending); debug only,
    // as FrameLifecycle's checks (llrhi.cpp:1199).
    llassert(!mEncoder.openEncoder());
    IdMTL4CommandBuffer buffer = commandBuffer();
    endCopies();
    for (size_t i = 0; i < mGroups.size(); ++i)
    {
        [buffer popDebugGroup];
    }
    mBufferGroups = 0;
    mResidency.commitIfChanged();
    [buffer endCommandBuffer];
    mCommandBuffer = nil;

    const U64 frame = mLifecycle.currentFrame();
    Slot& slot = slotOf(frame);
    // The handler holds the feedback state and its submission, never the device.
    std::shared_ptr<FeedbackState> feedback = mFeedback;
    std::shared_ptr<Submission> submission = std::make_shared<Submission>();
    NSString* label = buffer.label;
    const bool failForTest = mFailNextFeedback;
    mFailNextFeedback = false;
    NSError* injected = mInjectedError;
    mInjectedError = nil;
    if (mTestFaultFrame > 0 && frame >= mTestFaultFrame)
    {
        // Test runs only: frame N's fault, once. A hang gates the commit on an event nothing signals, so
        // the queue never runs it or anything after it.
        if (mTestFaultHang)
        {
            // Kept for the device's life: Metal 4's queue does not retain an event it waits on, and a released
            // one ends the wait. The queue also gives up a wait on an unsignalled event after about 4 s, under
            // METAL_WAIT_MS, so the hang's waits are 1 s each from here: both fail while the queue is held (run-time
            // probes). The 5 s limit itself is llrhi test<5>'s and test 165's.
            mTestHangGate = [mDevice newSharedEvent];
            mGate = mTestHangGate;
            mGateValue = 1;
            mWaitLimitMs = 1000;
        }
        else
        {
            injected = [NSError errorWithDomain:MTL4CommandQueueErrorDomain
                                           code:MTL4CommandQueueErrorAccessRevoked
                                       userInfo:@{ NSLocalizedDescriptionKey: @"injected (LL_METAL_TEST_FAULT)" }];
        }
        mTestFaultFrame = 0;
    }
    {
        std::lock_guard<std::mutex> lock(feedback->mutex);
        ++feedback->outstanding;
    }
    std::shared_ptr<MetalFailureLog> failures = mCompileState->failures; // as feedback
    // What the queue waits for before it runs this command buffer, named in its error line with when the GPU began
    // it: a Timeout on work that never began waited on one of these, one that began ran too long.
    IdMTLSharedEvent waitedUploads = mUploadWait;
    const U64 waitedUploadValue = mUploadWaitValue;
    const size_t waitedDrawables = mPendingDrawables.size();
    const bool waitedGate = mGate != nil;
    const double committedT = CACurrentMediaTime();
    MTL4CommitOptions* options = [MTL4CommitOptions new];
    [options addFeedbackHandler:^(IdMTL4CommitFeedback commitFeedback) {
        NSError* error = commitFeedback.error ? commitFeedback.error : injected;
        // The domain and code decide (Metal 4's AccessRevoked is 5, not MTLCommandBuffer's 4). A watchdog
        // Timeout is recoverable once on macOS (the command buffer is dropped, later ones run), so it is counted and
        // loses the device only when the command buffer before it timed out too (the serial feedback queue reports
        // in commit order).
        const bool timeout = metalErrorIsTimeout(error);
        bool lost = metalErrorLosesDevice(error);
        {
            std::lock_guard<std::mutex> lock(feedback->mutex);
            feedback->consecutiveTimeouts = timeout ? feedback->consecutiveTimeouts + 1 : 0;
            lost = lost || feedback->consecutiveTimeouts >= 2;
        }
        if (error)
        {
            // Held for the main thread, before submission and lost are stored, so a wait for them finds
            // the line held
            std::ostringstream line;
            line << "GPU error in frame " << frame << ", command buffer '" << (label ? label.UTF8String : "")
                 << "': " << metalErrorName(error)
                 << (lost ? ", the device is lost" : timeout ? ", counted (a second in a row loses the device)" : "")
                 << ": " << error.localizedDescription.UTF8String;
            char when[160];
            const double began = commitFeedback.GPUStartTime;
            const double ended = commitFeedback.GPUEndTime;
            if (began > 0.0)
            {
                snprintf(when, sizeof(when),
                         "; committed %.2f s ago, begun on the GPU %.2f s after it, ended %.2f s after it",
                         CACurrentMediaTime() - committedT, began - committedT,
                         ended > 0.0 ? ended - committedT : -1.0);
            }
            else
            {
                snprintf(when, sizeof(when), "; committed %.2f s ago, never begun on the GPU",
                         CACurrentMediaTime() - committedT);
            }
            line << when << "; it waited for: uploads "
                 << (waitedUploads ? std::to_string(waitedUploadValue) + " (signalled " +
                                         std::to_string(waitedUploads.signaledValue) + " now)"
                                   : std::string("none"))
                 << ", " << waitedDrawables << " drawable(s)" << (waitedGate ? ", the test gate" : "");
            failures->add(lost ? MetalFailureLog::Kind::DeviceLost : MetalFailureLog::Kind::GpuError, line.str());
        }
        {
            std::lock_guard<std::mutex> lock(feedback->mutex);
            submission->received = true;
            submission->failed = error != nil || failForTest;
            ++feedback->received;
            feedback->gpuErrors += error ? 1 : 0;
            if (!error)
            {
                feedback->gpu.add(commitFeedback.GPUStartTime, commitFeedback.GPUEndTime);
                submission->gpuEnd.store(commitFeedback.GPUEndTime, std::memory_order_relaxed);
                const double ran = commitFeedback.GPUEndTime - commitFeedback.GPUStartTime;
                if (commitFeedback.GPUStartTime > 0.0 && ran > 0.5)
                {
                    // Half the watchdog's limit: named, so a pass that grows towards a Timeout is found
                    LL_WARNS("RHI") << "Command buffer '" << (label ? label.UTF8String : "") << "' of frame " << frame
                                    << " ran " << ran << " s on the GPU" << LL_ENDL;
                }
            }
            if (feedback->timedFirst != 0 && frame >= feedback->timedFirst && frame <= feedback->timedLast)
            {
                // A command buffer of the pass timer's frames
                ++feedback->timedReceived;
                if (!error)
                {
                    feedback->timedGpu.add(commitFeedback.GPUStartTime, commitFeedback.GPUEndTime);
                }
            }
            feedback->lost = feedback->lost || lost;
            --feedback->outstanding;
        }
        feedback->changed.notify_all();
    }];
    if (mGate)
    {
        [mQueue waitForEvent:mGate value:mGateValue];
        mGate = nil;
    }
    if (mUploadWait)
    {
        // The uploads waitForFence asked for run before this command buffer, on the GPU; the CPU does not
        // wait. The wait holds for every later commit too (MTL4CommandQueue.h:269-275).
        [mQueue waitForEvent:mUploadWait value:mUploadWaitValue];
        mUploadWait = nil;
        mUploadWaitValue = 0;
    }
    // This command buffer holds the flip pass of every pending drawable (present() records it into the
    // command buffer being recorded, and any submit sends that one: endFrame's, or wait()'s and submitAndWait()'s
    // between present() and endFrame). The queue waits until the display no longer uses the drawable before it runs
    // a command buffer that targets it (MTL4CommandQueue.h:296-308) ...
    for (const IdCAMetalDrawable& drawable : mPendingDrawables)
    {
        [mQueue waitForDrawable:drawable];
        mSurface->countDrawableWait();
    }
    // Under the residency mutex: API validation checks the commit against the set (MetalResidency::commitToQueue).
    mResidency.commitToQueue(mQueue, buffer, options);
    if (mPassTimes.timed(frame))
    {
        mPassTimes.countSubmit();
    }
    // ... and signals it once after committing it, before its present (MTL4CommandQueue.h:277-294; MTLDrawable.h:29).
    for (const IdCAMetalDrawable& drawable : mPendingDrawables)
    {
        [mQueue signalDrawable:drawable];
        mSurface->countDrawableSignal();
        [drawable present];
        mSurface->countDrawablePresented();
    }
    mPendingDrawables.clear();
    slot.lastSerial = ++mSerial;
    // The signal follows all the queue's earlier work (MTL4CommandQueue.h:266), and the CPU reads
    // what the GPU wrote once it has been signalled (MTL4CommandBuffer.h:195-196).
    [mQueue signalEvent:slot.event value:slot.lastSerial];
    ++mSubmissions;
    mLastSubmission = submission;
    for (const std::shared_ptr<MetalReadbackStamp>& stamp : mStamps)
    {
        stamp->event = slot.event;
        stamp->serial = slot.lastSerial;
        stamp->submission = submission;
    }
    mStamps.clear();
}

void MetalDevice::splitBeforeEncoder(NSString* name)
{
    static const bool split = [] {
        const char* env = getenv("LL_METAL_SPLIT_PASSES");
        return env && std::strcmp(env, "1") == 0;
    }();
    if (!split || mEncoder.openEncoder())
    {
        return;
    }
    if (mCommandBuffer)
    {
        submit();
    }
    IdMTL4CommandBuffer buffer = commandBuffer();
    buffer.label = [NSString stringWithFormat:@"%@: %@", buffer.label, name ? name : @"?"];
}

void MetalDevice::sendBeforeScaler()
{
    // MetalFX's Metal 4 scalers begin a command buffer of their own; the work recorded before them goes in the one
    // before. After the frame has stood (an open or save panel shows), a stall on the GPU set off by the present
    // queue (METAL_PRESENT_GAP_NS) ran a scaler recorded after other work in the same command buffer into the
    // queue's watchdog, and the device was lost; at the start of a command buffer it runs as in any other frame, so
    // this submit stays as a second guard. The spatial scaler is sent the same way. One more submit in a frame
    // MetalFX scales.
    if (mCommandBuffer)
    {
        endCopies();
        submit();
    }
}

void MetalDevice::submitPending()
{
    // "wait() ends the pass": the open segment and its copies go with this submit. The next
    // command buffer of the frame begins on the same allocator once this one has ended
    // (MTL4CommandBuffer.h:55-66).
    mEncoder.splitPass();
    submit();
}

IdMTL4ComputeCommandEncoder MetalDevice::openCopies(NSString* label)
{
    if (mCopies && mEncoder.waitingTargets())
    {
        endCopies(); // a new target comes before copies that may write into or read it
    }
    if (!mCopies)
    {
        mEncoder.zeroNewTargets(); // before copies that may write into or read one
        splitBeforeEncoder(label);
        mCopiesStamp = mPassTimes.recording() ? mPassTimes.beginCompute(commandBuffer(), label ? label.UTF8String : "",
                                                                       MetalPassTimes::Kind::Copies,
                                                                       mLifecycle.currentFrame())
                                              : MetalPassStamp();
        // Metal 4 has no blit encoder; the compute encoder copies (MTL4CommandBuffer.h:130).
        mCopies = [commandBuffer() computeCommandEncoder];
        if (!mCopies)
        {
            LL_WARNS("RHI") << "Metal gave no compute encoder for " << label.UTF8String << LL_ENDL;
            return nil;
        }
        ++mCopyEncodersBegun;
        mCopiesSincePass = true; // the next render pass follows copies
        mCopies.label = label;
        // The copies wait for the queue's earlier work and see what it wrote.
        queueBarrier(mCopies);
    }
    return mCopies;
}

void MetalDevice::queueBarrier(IdMTL4CommandEncoder encoder)
{
    [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    ++mBarriers;
}

bool MetalDevice::passBarrier(IdMTL4RenderCommandEncoder encoder)
{
    (mCopiesSincePass ? mPassesAfterCopies : mPassesAfterPasses) += 1;
    mCopiesSincePass = false;
    if (!mSpeed.splitBarriers)
    {
        queueBarrier(encoder);
        return false;
    }
    // The consumer barrier waits for all of the queue's earlier stages, but holds back only the pass's
    // non-geometry stages (MTL4CommandEncoder.h:48-72): its attachments' loads, its fragments and what writes them wait
    // for every earlier read and write, and its vertex work starts while the earlier passes finish.
    [encoder barrierAfterQueueStages:MTLStageAll
                        beforeStages:metalPassWaitingStages()
                   visibilityOptions:MTL4VisibilityOptionDevice];
    ++mBarriers;
    ++mSplitBarriers;
    return true;
}

void MetalDevice::vertexBarrier(IdMTL4RenderCommandEncoder encoder)
{
    // The rest of the split barrier, as close to the draw that needs it as the header asks (MTL4CommandEncoder.h:
    // 54-55): a vertex stage that samples may read what an earlier pass wrote.
    [encoder barrierAfterQueueStages:MTLStageAll
                        beforeStages:metalGeometryStages()
                   visibilityOptions:MTL4VisibilityOptionDevice];
    ++mVertexBarriers;
}

void MetalDevice::noteIndexCopy(S64 bytes)
{
    if (bytes < 0)
    {
        mIndexCopyBytes.fetch_sub(U64(-bytes));
        return;
    }
    const U64 now = mIndexCopyBytes.fetch_add(U64(bytes)) + U64(bytes);
    U64 peak = mIndexCopyPeak.load();
    while (now > peak && !mIndexCopyPeak.compare_exchange_weak(peak, now))
    {
    }
}

MetalGpuBusy MetalDevice::gpuBusy()
{
    drainFeedback();
    std::lock_guard<std::mutex> lock(mFeedback->mutex);
    return mFeedback->gpu;
}

void MetalDevice::logGpuTime(bool total)
{
    MetalGpuBusy now;
    {
        // No drain: a window takes what has reported (the last frame's command buffers may follow in the next one);
        // the total comes after drainFeedback.
        std::lock_guard<std::mutex> lock(mFeedback->mutex);
        now = mFeedback->gpu;
    }
    const auto clock = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(clock - mGpuWindowBegan).count();
    const MetalGpuBusy& from = total ? MetalGpuBusy() : mGpuWindowStart;
    const U64 frames = total ? mGpuFrames : mGpuWindowFrames;
    const double busyMs = (now.busy - from.busy) * 1000.0;
    const U64 buffers = now.commandBuffers - from.commandBuffers;
    auto fixed = [](const char* format, double value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), format, value);
        return std::string(text);
    };
    // One line for A/B measurements: the GPU's busy time per frame, for the window or the total.
    LL_INFOS("RHI") << "Metal GPU time" << (total ? " (total)" : "") << ": " << frames << " frames"
                    << (total ? "" : fixed(" in %.1f s", seconds)) << ", GPU busy "
                    << fixed("%.3f", frames ? busyMs / double(frames) : 0.0) << " ms per frame ("
                    << fixed("%.1f", busyMs) << " ms, " << buffers << " command buffers, " << (now.late - from.late)
                    << " late); barriers "
                    << (mSpeed.splitBarriers ? "split" : "full") << ", discard " << (mSpeed.discardStores ? "on" : "off")
                    << LL_ENDL;
    if (total)
    {
        LL_INFOS("RHI") << "Metal speed counters: barriers " << mBarriers << " (split " << mSplitBarriers
                        << "), producer barriers " << mProducerBarriers << ", vertex barriers " << mVertexBarriers
                        << ", passes after copies " << mPassesAfterCopies << ", after passes " << mPassesAfterPasses
                        << ", discarded stores " << mEncoder.discardedStores() << ", static index copies "
                        << mIndexCopyBytes.load() << " bytes (most " << mIndexCopyPeak.load() << ")" << LL_ENDL;
        return;
    }
    mGpuWindowStart = now;
    mGpuWindowFrames = 0;
    mGpuWindowBegan = clock;
}

IdMTL4ComputeCommandEncoder MetalDevice::beginUploadCopies(NSString* label)
{
    // The queue barrier orders only the work of earlier encoders, not copies inside one
    // (MTL4CommandEncoder.h:55-58), so an upload never shares an encoder with the copies before it,
    // and a readback recorded before it reads the bytes from before it, as on GL.
    endCopies();
    return openCopies(label);
}

IdMTLBuffer MetalDevice::newResidentBuffer(U64 length, std::string_view debugName)
{
    IdMTLBuffer buffer = [mDevice newBufferWithLength:length options:metalBufferStorage(BufferDesc())];
    if (!buffer)
    {
        LL_WARNS("RHI") << "Metal gave no " << length << "-byte buffer for " << debugName << LL_ENDL;
        return nil;
    }
    if (mDebugLabels && !debugName.empty())
    {
        buffer.label = metalString(debugName);
    }
    // Resident from creation; the set is committed before the next submit.
    mResidency.admit(buffer, debugName);
    return buffer;
}

std::vector<U32> metalListOrder(PrimitiveType type, U32 count)
{
    std::vector<U32> order;
    switch (type)
    {
    case PrimitiveType::TriangleFan:
    case PrimitiveType::TriangleStrip:
        if (count < 3)
        {
            break;
        }
        order.reserve(size_t(count - 2) * 3);
        for (U32 i = 0; i + 2 < count; ++i)
        {
            // GL's triangle i is (0, i + 1, i + 2) in a fan, (i, i + 1, i + 2) or for an odd i
            // (i + 1, i, i + 2) in a strip; rotated (2, 0, 1) its provoking vertex comes first.
            const bool fan = type == PrimitiveType::TriangleFan;
            const bool odd = (i % 2) != 0;
            order.push_back(i + 2);
            order.push_back(fan ? 0 : (odd ? i + 1 : i));
            order.push_back(fan ? i + 1 : (odd ? i : i + 1));
        }
        break;
    case PrimitiveType::LineLoop:
        if (count < 2)
        {
            break;
        }
        order.reserve(size_t(count) + 1);
        for (U32 i = 0; i < count; ++i)
        {
            order.push_back(i);
        }
        order.push_back(0); // the closing line
        break;
    default:
        break;
    }
    return order;
}

U32 metalListCount(PrimitiveType type, U32 count)
{
    switch (type)
    {
    case PrimitiveType::TriangleFan:
    case PrimitiveType::TriangleStrip:
        return count < 3 ? 0 : (count - 2) * 3;
    case PrimitiveType::LineLoop:
        return count < 2 ? 0 : count + 1;
    default:
        return 0;
    }
}

MTLPrimitiveType metalListPrimitive(PrimitiveType type)
{
    return type == PrimitiveType::LineLoop ? MTLPrimitiveTypeLineStrip : MTLPrimitiveTypeTriangle;
}

MetalIndexList MetalDevice::generatedIndices(PrimitiveType type, U32 count)
{
    const U64 key = (U64(type) << 32) | count;
    ++mIndexListUses;
    for (CachedIndexList& cached : mIndexLists)
    {
        if (cached.key == key)
        {
            cached.used = mIndexListUses;
            return cached.list;
        }
    }
    const std::vector<U32> order = metalListOrder(type, count);
    if (order.empty())
    {
        return {};
    }
    MetalIndexList list;
    list.count = U32(order.size());
    list.type = count <= 65535 ? IndexType::U16 : IndexType::U32;
    const U32 size = indexSize(list.type);
    list.buffer = newResidentBuffer(U64(order.size()) * size, "index list of " + std::to_string(count) + " vertices");
    if (!list.buffer)
    {
        return {}; // newResidentBuffer wrote the WARN line
    }
    // Shared memory the GPU reads only from draws recorded after this.
    std::byte* bytes = static_cast<std::byte*>(list.buffer.contents);
    for (size_t i = 0; i < order.size(); ++i)
    {
        if (list.type == IndexType::U16)
        {
            const U16 index = U16(order[i]);
            std::memcpy(bytes + i * size, &index, size);
        }
        else
        {
            std::memcpy(bytes + i * size, &order[i], size);
        }
    }
    if (mIndexLists.size() >= MAX_INDEX_LISTS)
    {
        // The least recently used goes once the GPU has finished the frames that may read it.
        const auto oldest = std::min_element(mIndexLists.begin(), mIndexLists.end(),
                                             [](const CachedIndexList& a, const CachedIndexList& b)
                                             { return a.used < b.used; });
        IdMTLBuffer buffer = oldest->list.buffer;
        mLifecycle.release([this, buffer]() { mResidency.retire(buffer); });
        mIndexLists.erase(oldest);
    }
    mIndexLists.push_back({ key, mIndexListUses, list });
    return list;
}

MetalAllocation MetalDevice::frameAllocation(U64 size)
{
    return mRings[mLifecycle.currentFrame() % mRings.size()].allocate(*this, size);
}

MetalAllocation MetalDevice::newBufferMemory(const BufferDesc& desc, std::string_view debugName)
{
    if (metalPooled(desc))
    {
        return mPool.acquire(*this, desc.size);
    }
    IdMTLBuffer buffer = newResidentBuffer(desc.size, debugName);
    return { buffer, 0, buffer ? static_cast<std::byte*>(buffer.contents) : nullptr };
}

void MetalDevice::releaseBufferMemory(const BufferDesc& desc, const MetalAllocation& memory)
{
    if (!memory.buffer)
    {
        return;
    }
    // The memory is reused or leaves the set only once the GPU has finished the frames
    // that may read it.
    if (metalPooled(desc))
    {
        const U64 size = desc.size;
        mLifecycle.release([this, memory, size]() { mPool.giveBack(memory, size); });
    }
    else
    {
        IdMTLBuffer buffer = memory.buffer;
        mLifecycle.release([this, buffer]() { mResidency.retire(buffer); });
    }
}

// The frame slots' rings ----------------------------------------------------------------

MetalAllocation MetalRing::allocate(MetalDevice& device, U64 size)
{
    if (size > OWN_BUFFER_OVER && size <= KEEP_MAX)
    {
        // Large staging, such as a media frame's 4 MiB, from the slot's kept buffers, the smallest that holds
        // it, so a steady stream of uploads makes no buffer and changes no residency after its first frames
        Kept* best = nullptr;
        for (Kept& kept : mKept)
        {
            if (!kept.used && kept.length >= size && (!best || kept.length < best->length))
            {
                best = &kept;
            }
        }
        const U64 length = (size + KEEP_ROUND - 1) / KEEP_ROUND * KEEP_ROUND;
        if (!best && mKeptBytes + length <= KEEP_SLOT_MAX)
        {
            IdMTLBuffer buffer = device.newResidentBuffer(length, "Ring staging");
            if (!buffer)
            {
                return {};
            }
            Kept kept;
            kept.buffer = buffer;
            kept.length = length;
            mKept.push_back(kept);
            mKeptBytes += length;
            ++mKeptMade;
            best = &mKept.back();
        }
        if (best)
        {
            best->used = true;
            return { best->buffer, 0, static_cast<std::byte*>(best->buffer.contents) };
        }
        // The slot keeps KEEP_SLOT_MAX already (a burst): a buffer of its own, as below
    }
    if (size > OWN_BUFFER_OVER)
    {
        // Too large for a chunk, and not kept: a buffer of its own, which goes with the frame.
        IdMTLBuffer buffer = device.newResidentBuffer(size, "Ring overflow");
        if (!buffer)
        {
            return {};
        }
        MetalResidency* residency = &device.residency();
        device.lifecycle().release([residency, buffer]() { residency->retire(buffer); });
        return { buffer, 0, static_cast<std::byte*>(buffer.contents) };
    }
    U64 at = (mOffset + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
    while (mChunk >= mChunks.size() || at + size > CHUNK_BYTES)
    {
        if (mChunk < mChunks.size())
        {
            ++mChunk;
            at = 0;
            continue;
        }
        IdMTLBuffer chunk = device.newResidentBuffer(CHUNK_BYTES, "Frame ring");
        if (!chunk)
        {
            return {};
        }
        mChunks.push_back(chunk);
    }
    mOffset = at + size;
    IdMTLBuffer chunk = mChunks[mChunk];
    return { chunk, at, static_cast<std::byte*>(chunk.contents) + at };
}

void MetalRing::reset(MetalDevice& device)
{
    // The chunks the frame that ends here used; every TRIM_REWINDS rewinds those past the fullest frame's (one at
    // least) retire through the lifecycle, as a kept buffer does. The slot's previous frame has finished, and its
    // chunks serve no other slot.
    const size_t used = mChunk < mChunks.size() && (mChunk > 0 || mOffset > 0) ? mChunk + 1 : 0;
    mChunksNeeded = std::max(mChunksNeeded, used);
    if (++mRewinds >= TRIM_REWINDS)
    {
        const size_t keep = std::max<size_t>(mChunksNeeded, 1);
        MetalResidency* residency = &device.residency();
        while (mChunks.size() > keep)
        {
            IdMTLBuffer buffer = mChunks.back();
            mChunks.pop_back();
            device.lifecycle().release([residency, buffer]() { residency->retire(buffer); });
        }
        mChunksNeeded = 0;
        mRewinds = 0;
    }
    mChunk = 0;
    mOffset = 0;
    // A kept buffer unused for TRIM_REWINDS rewinds leaves the set through the lifecycle, as an overflow
    // buffer does with its frame (the GPU finished its last frame long ago)
    for (auto kept = mKept.begin(); kept != mKept.end();)
    {
        if (kept->used)
        {
            kept->used = false;
            kept->idle = 0;
            ++kept;
            continue;
        }
        if (++kept->idle < TRIM_REWINDS)
        {
            ++kept;
            continue;
        }
        MetalResidency* residency = &device.residency();
        IdMTLBuffer buffer = kept->buffer;
        device.lifecycle().release([residency, buffer]() { residency->retire(buffer); });
        mKeptBytes -= kept->length;
        kept = mKept.erase(kept);
    }
}

// The pool of small dynamic buffers ------------------------------------------

size_t MetalBufferPool::sizeClass(U64 size)
{
    size_t index = 0;
    while ((MIN_BLOCK << index) < size)
    {
        ++index;
    }
    return index;
}

MetalAllocation MetalBufferPool::acquire(MetalDevice& device, U64 size)
{
    const size_t index = sizeClass(size);
    llassert(index < CLASSES);
    const U64 block = MIN_BLOCK << index;
    std::lock_guard<std::mutex> lock(mMutex);
    SizeClass& one = mClasses[index];
    if (!one.free.empty())
    {
        const MetalAllocation reused = one.free.back();
        one.free.pop_back();
        return reused;
    }
    if (one.next + block > CHUNK_BYTES)
    {
        // Always resident: admitted once and never retired before the device ends.
        IdMTLBuffer chunk = device.newResidentBuffer(CHUNK_BYTES, "Buffer pool " + std::to_string(block) + " B");
        if (!chunk)
        {
            return {};
        }
        one.chunk = chunk;
        one.next = 0;
        ++mChunks;
    }
    const MetalAllocation carved{ one.chunk, one.next, static_cast<std::byte*>(one.chunk.contents) + one.next };
    one.next += block;
    return carved;
}

void MetalBufferPool::giveBack(const MetalAllocation& block, U64 size)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mClasses[sizeClass(size)].free.push_back(block);
}

size_t MetalBufferPool::chunks() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mChunks;
}

void MetalDevice::encodeCopy(const MetalCopy& copy)
{
    if (!openCopies(@"Readbacks"))
    {
        return;
    }
    // Never flipped. Row i of the buffer is the texture's row y + i, which is GL's row
    // y + i; rows are tight, and a depth and stencil texture gives its depth
    // (MTL4ComputeCommandEncoder.h:336-345, MTLBlitCommandEncoder.h:29-30).
    const Region& region = copy.region;
    [mCopies copyFromTexture:copy.texture
                 sourceSlice:copy.layer
                 sourceLevel:copy.level
                sourceOrigin:MTLOriginMake(region.x, region.y, 0)
                  sourceSize:MTLSizeMake(region.width, region.height, 1)
                    toBuffer:copy.buffer
           destinationOffset:0
      destinationBytesPerRow:copy.bytesPerRow
    destinationBytesPerImage:0
                     options:copy.options];
    mStamps.push_back(copy.stamp);
}

void MetalDevice::encodeBufferCopy(IdMTLBuffer source, U64 offset, IdMTLBuffer destination, U64 size,
                                   std::shared_ptr<MetalReadbackStamp> stamp)
{
    if (!openCopies(@"Readbacks"))
    {
        return;
    }
    [mCopies copyFromBuffer:source sourceOffset:offset toBuffer:destination destinationOffset:0 size:size];
    mStamps.push_back(std::move(stamp));
}

void MetalDevice::endCopies()
{
    if (mCopies)
    {
        if (mSpeed.splitBarriers)
        {
            // What the copies wrote (buffers, textures) comes before every stage of every later encoder
            // on the queue, also a split pass's vertex stage (MTL4CommandEncoder.h:74-93; a copy is a Blit stage,
            // MTL4ComputeCommandEncoder.h:39-40).
            [mCopies barrierAfterStages:MTLStageDispatch | MTLStageBlit
                      beforeQueueStages:MTLStageAll
                      visibilityOptions:MTL4VisibilityOptionDevice];
            ++mProducerBarriers;
        }
        [mCopies endEncoding];
        mCopies = nil;
        if (mCopiesStamp.frame)
        {
            mPassTimes.endCompute(mCommandBuffer, mCopiesStamp);
            mCopiesStamp = MetalPassStamp();
        }
    }
}

bool MetalDevice::waitForFeedback(const std::shared_ptr<Submission>& submission)
{
    std::unique_lock<std::mutex> lock(mFeedback->mutex);
    mFeedback->changed.wait_for(lock, std::chrono::milliseconds(METAL_WAIT_MS), [&submission]() { return submission->received; });
    return submission->received && !submission->failed;
}

IdMTLBuffer MetalDevice::newReadbackBuffer(U64 length)
{
    // Shared, so the CPU reads it (MTLResource.h:176).
    IdMTLBuffer buffer = [mDevice newBufferWithLength:length options:MTLResourceStorageModeShared];
    if (!buffer)
    {
        LL_WARNS("RHI") << "Metal gave no " << length << "-byte buffer for a readback" << LL_ENDL;
        return nil;
    }
    // Resident from creation; the set is committed before the next submit.
    mResidency.admit(buffer, "readback");
    return buffer;
}

bool MetalDevice::waitForFrame(U64 frame)
{
    const Slot& slot = slotOf(frame);
    if (slot.frame != frame)
    {
        // A slot that has gone on to a later frame finished this one first.
        return slot.frame > frame;
    }
    if ([slot.event waitUntilSignaledValue:slot.lastSerial timeoutMS:mWaitLimitMs])
    {
        return true;
    }
    LL_WARNS("RHI") << "Frame " << frame << " did not finish on the GPU within " << mWaitLimitMs << " ms" << LL_ENDL;
    return false;
}

bool MetalDevice::deviceLost() const
{
    if (mLifecycle.lost())
    {
        return true;
    }
    std::lock_guard<std::mutex> lock(mFeedback->mutex);
    return mFeedback->lost;
}

U64 MetalDevice::gpuErrorCount() const
{
    std::lock_guard<std::mutex> lock(mFeedback->mutex);
    return mFeedback->gpuErrors;
}

void MetalDevice::pollFrames()
{
    // Frames finish in order on the one queue, so the first unfinished one ends the walk.
    for (U64 frame = mLifecycle.completedFrame() + 1; frame <= mLifecycle.currentFrame(); ++frame)
    {
        const Slot& slot = slotOf(frame);
        if (slot.frame != frame || !reached(slot))
        {
            return;
        }
        mLifecycle.frameCompleted(frame);
    }
}

void MetalDevice::endFrame()
{
    // The encoder first, so what it lets go belongs to the frame that ends; always a submit;
    // finished frames come only from the slot events.
    mEncoder.endFrame();
    // The most query begins in one frame
    mQueryStats.peakSampleBegins = std::max(mQueryStats.peakSampleBegins, mQueryStats.frameSampleBegins);
    mQueryStats.peakTimeBegins = std::max(mQueryStats.peakTimeBegins, mQueryStats.frameTimeBegins);
    mQueryStats.frameSampleBegins = 0;
    mQueryStats.frameTimeBegins = 0;
    submit();
    // About every 10 s of frames, the window's GPU time
    ++mGpuFrames;
    ++mGpuWindowFrames;
    if (std::chrono::steady_clock::now() - mGpuWindowBegan >= mGpuWindow)
    {
        logGpuTime(false);
    }
    captureAtFrameEnd();
    pollFrames();
    mLifecycle.endFrame();
    writeFailureLines(); // also when the device is lost
    if (mLifecycle.lost())
    {
        // No frame began, so the ring of the frame just sent stays as it is; the GPU may still read it.
        return;
    }
    if (mSurface)
    {
        // <Luminous> Right after the wait, when a GPU-bound frame has just finished: VSync off's newest finished frame
        // to the window server (MetalSurface::frameEnded).
        mSurface->frameEnded();
    }
    // FrameLifecycle::endFrame has waited for the frame that used this slot before
    // (llrhi.cpp:1214-1232), so the new frame's ring starts again. Never in commandBuffer(): a draw's
    // uniform blocks and an upload's staging are allocated before the frame's first command buffer.
    mRings[mLifecycle.currentFrame() % mRings.size()].reset(*this);
    // The frame's end closes the open group, so its programs become Ready without a wait().
    mProgramGroups.closeOpen();
    writeClassLines();
    if (mPassTimes.running())
    {
        passTimesAtFrameEnd();
    }
}

void MetalFailureLog::add(Kind kind, std::string line)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mPending.store(true, std::memory_order_release);
    Counts& counts = mKinds[size_t(kind)];
    if (++counts.lines <= FIRST_LINES)
    {
        mHeld.push_back(std::move(line));
        if (counts.lines == FIRST_LINES)
        {
            counts.summarized = std::chrono::steady_clock::now(); // the first summary comes an interval after these
        }
        return;
    }
    ++counts.counted;
    counts.last = std::move(line);
}

void MetalFailureLog::write(bool final)
{
    static constexpr const char* KINDS[size_t(Kind::Count)] = { "GPU error", "lost device", "upload error",
                                                                "failed program", "drawable timeout" };
    // Nothing held and nothing counted since the last write: no lock, no clock.
    if (!mPending.load(std::memory_order_acquire))
    {
        return;
    }
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        lines.swap(mHeld);
        const auto now = std::chrono::steady_clock::now();
        bool counted = false;
        for (size_t kind = 0; kind < mKinds.size(); ++kind)
        {
            Counts& counts = mKinds[kind];
            if (counts.counted > 0 && (final || now - counts.summarized >= SUMMARY_INTERVAL))
            {
                lines.push_back(llformat("%llu more %s lines (%llu in all, the first %llu written); the last: ",
                                         (unsigned long long)counts.counted, KINDS[kind],
                                         (unsigned long long)counts.lines, (unsigned long long)FIRST_LINES) +
                                counts.last);
                counts.counted = 0;
                counts.last.clear();
                counts.summarized = now;
            }
            counted = counted || counts.counted > 0;
        }
        mPending.store(counted, std::memory_order_release);
    }
    for (const std::string& line : lines)
    {
        LL_WARNS("RHI") << line << LL_ENDL;
    }
}

void MetalDevice::writeClassLines()
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mCompileState->mutex);
        lines.swap(mCompileState->classLines);
    }
    for (const std::string& line : lines)
    {
        if (mWrittenClassLines.insert(std::hash<std::string>{}(line)).second)
        {
            LL_INFOS("MetalShaderClasses") << line << LL_ENDL;
        }
    }
}

void MetalDevice::submitAndWait()
{
    submitPending();
    const Slot& slot = slotOf(mLifecycle.currentFrame());
    const bool finished = [slot.event waitUntilSignaledValue:slot.lastSerial timeoutMS:mWaitLimitMs];
    drainFeedback();
    if (!finished)
    {
        // Metal 4's command buffers do not keep what they use, so what the frames let go stays
        // until pollFrames sees their slot events; FrameLifecycle::endFrame waits for them.
        LL_WARNS("RHI") << "submitAndWait: the GPU did not finish within " << mWaitLimitMs
                        << " ms; the released resources stay until it does" << LL_ENDL;
        return;
    }
    // Everything recorded has been sent and has finished, so what was let go before now is freed,
    // the current frame's included: a render's frames run inside one viewer frame and wait here.
    mLifecycle.idle(true);
    // The residency set keeps what it held until it is committed: what idle just freed would otherwise live on
    // until the next submit's commit, which in a render is the next frame's, after that frame has made its own.
    // The GPU has finished everything sent, so nothing it reads leaves the set before it is done.
    mResidency.commitIfChanged();
}

void MetalDevice::drainFeedback()
{
    // The event wait does not order the handlers: they can still be running after it returns.
    std::unique_lock<std::mutex> lock(mFeedback->mutex);
    const bool drained = mFeedback->changed.wait_for(lock, std::chrono::milliseconds(mWaitLimitMs),
                                                     [this]() { return mFeedback->outstanding == 0; });
    const U64 outstanding = mFeedback->outstanding;
    lock.unlock();
    if (!drained)
    {
        LL_WARNS("RHI") << outstanding << " commit feedback reports did not arrive within " << mWaitLimitMs
                        << " ms" << LL_ENDL;
    }
    writeFailureLines(); // the handlers that ran wrote no line themselves
}

void MetalDevice::gateNextCommit(IdMTLSharedEvent gate, U64 value)
{
    mGate = gate;
    mGateValue = value;
}

U64 MetalDevice::feedbackReceived()
{
    drainFeedback();
    std::lock_guard<std::mutex> lock(mFeedback->mutex);
    return mFeedback->received;
}

U64 MetalDevice::gpuErrors()
{
    drainFeedback();
    std::lock_guard<std::mutex> lock(mFeedback->mutex);
    return mFeedback->gpuErrors;
}

Ref<Device> createMetalDevice(const DeviceCreateInfo& info, const MetalSupport& support)
{
    IdMTLDevice device = MTLCreateSystemDefaultDevice();
    if (!device)
    {
        warnCreateFailed("device", "Metal", "MTLCreateSystemDefaultDevice gave no device");
        return nullptr;
    }
    // The compiler may use more of the CPU's threads (MTLDevice.h:1310-1314); it is the
    // device's, so set before the first compiler.
    device.shouldMaximizeConcurrentCompilation = YES;
    // The queue's label is read-only, so it comes from the descriptor, with a serial queue
    // for the commit feedback.
    MTL4CommandQueueDescriptor* queueDescriptor = [MTL4CommandQueueDescriptor new];
    queueDescriptor.label = @"Viewer Lab";
    dispatch_queue_t feedbackQueue = dispatch_queue_create("Viewer Lab Metal feedback", DISPATCH_QUEUE_SERIAL);
    queueDescriptor.feedbackQueue = feedbackQueue;
    // The property is assign (MTL4CommandQueue.h:86), yet Metal releases the queue once more than it
    // retains it: without this reference of Metal's own, the device's last release of the queue
    // trapped in _os_object_release under ~MetalDevice (run-time observation). At worst a
    // dispatch queue per device is kept.
    (void)(__bridge_retained void*)feedbackQueue;
    NSError* error = nil;
    IdMTL4CommandQueue queue = [device newMTL4CommandQueueWithDescriptor:queueDescriptor error:&error];
    if (!queue)
    {
        warnCreateFailed("device", "Metal", error ? error.description.UTF8String : "no command queue");
        return nullptr;
    }
    // One residency set on the queue from the start.
    Ref<Device> created = new MetalDevice(device, queue, feedbackQueue, support);
    if (!static_cast<MetalDevice*>(created.get())->residency().attach(queue))
    {
        return nullptr;
    }
    // A window gives the device its surface; a device without one renders into textures only, as GL's.
    if (info.window.window)
    {
        CAMetalLayer* layer = metalLayerOf(info.window.window);
        if (!layer)
        {
            warnCreateFailed("device", "Metal", "the window has no CAMetalLayer");
            return nullptr;
        }
        if (!static_cast<MetalDevice*>(created.get())->createSurface(layer, info.width, info.height))
        {
            warnCreateFailed("device", "Metal", "no surface on the window's CAMetalLayer");
            return nullptr;
        }
    }
    return created;
}

namespace
{
    /// setMetalDebug's value: -1 until it is called. Constant-initialized, so the GL start pays
    /// nothing for it.
    std::atomic<int> sMetalDebug{ -1 };
    bool sValidationAsked = false; ///< the first metalBackend call has asked for validation
    bool sValidationWarned = false;

    /// The capture asked for (CAPTURE_NONE, CAPTURE_NEXT_FRAME or CAPTURE_AFTER_FRAME). Constant-initialized,
    /// so a frame's first command buffer and a frame's end read only this, and the GL start pays nothing; the
    /// path and its mutex are function-local statics, made at the first ask.
    constexpr int CAPTURE_NONE = 0;
    constexpr int CAPTURE_NEXT_FRAME = 1;  ///< the next frame's first command buffer takes it
    constexpr int CAPTURE_AFTER_FRAME = 2; ///< a device's next frame end turns it into CAPTURE_NEXT_FRAME
    std::atomic<int> sCaptureAsked{ CAPTURE_NONE };

    std::mutex& captureMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::string& capturePath()
    {
        static std::string path;
        return path;
    }

    /// The path asked for, taken, so one device captures one frame; empty when none is asked for.
    std::string takeFrameCapture()
    {
        if (sCaptureAsked.load(std::memory_order_acquire) != CAPTURE_NEXT_FRAME)
        {
            return {};
        }
        std::lock_guard<std::mutex> lock(captureMutex());
        if (sCaptureAsked.load(std::memory_order_relaxed) != CAPTURE_NEXT_FRAME)
        {
            return {}; // asked again meanwhile for after the current frame
        }
        sCaptureAsked.store(CAPTURE_NONE, std::memory_order_release);
        std::string path;
        path.swap(capturePath());
        return path;
    }

    /// At a frame's end: a capture asked for after the current frame is now one for the next frame.
    void frameCaptureFrameEnded()
    {
        int after = CAPTURE_AFTER_FRAME;
        sCaptureAsked.compare_exchange_strong(after, CAPTURE_NEXT_FRAME, std::memory_order_acq_rel);
    }

    /// The entries of the folder at path; 0 when there is none (or path is a file).
    NSUInteger documentEntries(const std::string& path)
    {
        NSString* folder = metalString(path);
        BOOL isFolder = NO;
        if (![[NSFileManager defaultManager] fileExistsAtPath:folder isDirectory:&isFolder] || !isFolder)
        {
            return 0;
        }
        return [[NSFileManager defaultManager] contentsOfDirectoryAtPath:folder error:nil].count;
    }

    /// With the debug setting, Metal API Validation and shader validation, before this
    /// process's first MTLCreateSystemDefaultDevice. A run-time probe
    /// saw the variables work when set here before any Metal device, and not once a CGL context
    /// had made one; the launcher's variables (test.sh, CMake) work in both, and setenv leaves
    /// them. setenv is not safe while other threads read the environment. In the viewer setenv here
    /// does not make validation live; the launcher's variables do.
    void askForValidation()
    {
        if (sValidationAsked)
        {
            return;
        }
        sValidationAsked = true;
        if (!metalDebug())
        {
            return;
        }
        setenv("MTL_DEBUG_LAYER", "1", 0);
        setenv("MTL_DEBUG_LAYER_ERROR_MODE", "assert", 0);
        setenv("MTL_DEBUG_LAYER_WARNING_MODE", "assert", 0);
        setenv("MTL_SHADER_VALIDATION", "1", 0);
        setenv("MTL_SHADER_VALIDATION_REPORT_TO_STDERR", "1", 0);
    }

    /// One WARN line when the debug setting is on and the device is not Metal API Validation's
    /// wrapper class (MTLDebugDevice in the probe), as llrhimetal_metal.mm's deviceTest checks.
    API_AVAILABLE(macos(26.0)) void warnWithoutValidation(IdMTLDevice device)
    {
        if (sValidationWarned || !metalDebug())
        {
            return;
        }
        const char* name = NSStringFromClass([device class]).UTF8String;
        if (name && std::strstr(name, "Debug"))
        {
            return;
        }
        if (name && std::strstr(name, "Capture"))
        {
            // With MTL_CAPTURE_ENABLED the capture layer's class (CaptureMTLDevice in a probe) wraps the
            // device, so its class does not show the debug layer; Metal's banners still do (test.sh checks them).
            sValidationWarned = true;
            LL_INFOS("RHI") << "Metal debug setting: the device is the capture layer's (" << name
                            << "), so its class does not show whether Metal API Validation is live" << LL_ENDL;
            return;
        }
        sValidationWarned = true;
        LL_WARNS("RHI") << "Metal debug setting: Metal API Validation is not live (device class "
                        << (name ? name : "unknown")
                        << "); set MTL_DEBUG_LAYER and MTL_SHADER_VALIDATION before the process's first Metal or GL "
                           "device, as test.sh does"
                        << LL_ENDL;
    }
}

void MetalDevice::beginCapture()
{
    const std::string path = takeFrameCapture();
    if (path.empty())
    {
        return;
    }
    MTLCaptureManager* manager = [MTLCaptureManager sharedCaptureManager];
    // MTLCaptureManager.h:88; outside Xcode the process needs MTL_CAPTURE_ENABLED=1.
    if (![manager supportsDestination:MTLCaptureDestinationGPUTraceDocument])
    {
        LL_WARNS("RHI") << "Metal frame capture to " << path
                        << ": this process cannot write a GPU trace document; start it with MTL_CAPTURE_ENABLED=1"
                        << LL_ENDL;
        return;
    }
    // The device captures all its queues (MTLCaptureManager.h:47-55), to a document at outputURL (:58-62).
    MTLCaptureDescriptor* descriptor = [MTLCaptureDescriptor new];
    descriptor.captureObject = mDevice;
    descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
    descriptor.outputURL = [NSURL fileURLWithPath:metalString(path)];
    NSError* error = nil;
    if (![manager startCaptureWithDescriptor:descriptor error:&error])
    {
        LL_WARNS("RHI") << "Metal frame capture to " << path
                        << " did not begin: " << (error ? error.localizedDescription.UTF8String : "no reason given")
                        << LL_ENDL;
        return;
    }
    mCapturing = true;
    mCapturePath = path;
    mCaptureFrame = mLifecycle.currentFrame();
    LL_INFOS("RHI") << "Metal frame capture of frame " << mCaptureFrame << " began: " << path << LL_ENDL;
}

void MetalDevice::endCapture()
{
    // The captured frame ends with the submit before this (MTLCaptureManager.h:94, 105); Metal writes the
    // document in stopCapture, which a run-time probe saw finished when the call returns.
    [[MTLCaptureManager sharedCaptureManager] stopCapture];
    mCapturing = false;
    const Slot& slot = slotOf(mCaptureFrame);
    const U32 buffers = slot.frame == mCaptureFrame ? slot.parts : 0;
    if (const NSUInteger entries = documentEntries(mCapturePath))
    {
        LL_INFOS("RHI") << "Metal frame capture of frame " << mCaptureFrame << " written: " << mCapturePath << " ("
                        << entries << " entries, " << buffers << " command buffers)" << LL_ENDL;
    }
    else
    {
        LL_WARNS("RHI") << "Metal frame capture of frame " << mCaptureFrame << " (" << buffers
                        << " command buffers): Metal left no GPU trace document at " << mCapturePath << LL_ENDL;
    }
    mCapturePath.clear();
}

void MetalDevice::captureAtFrameEnd()
{
    if (mCapturing)
    {
        endCapture();
    }
    if (sCaptureAsked.load(std::memory_order_relaxed) == CAPTURE_AFTER_FRAME)
    {
        frameCaptureFrameEnded();
    }
}

void setMetalFrameCapture(std::string path, MetalFrameCaptureStart start)
{
    std::lock_guard<std::mutex> lock(captureMutex());
    capturePath() = std::move(path);
    sCaptureAsked.store(capturePath().empty()                                ? CAPTURE_NONE
                        : start == MetalFrameCaptureStart::AfterCurrentFrame ? CAPTURE_AFTER_FRAME
                                                                             : CAPTURE_NEXT_FRAME,
                        std::memory_order_release);
}

void setMetalDebug(bool on)
{
    sMetalDebug = on ? 1 : 0;
}

API_AVAILABLE(macos(26.0)) void metalWriteFailureLines(Device& device)
{
    if (device.backend() == BackendType::Metal)
    {
        // final: the viewer ends with fastQuit after this, so ~MetalDevice's summaries never come
        static_cast<MetalDevice&>(device).writeFailureLines(true);
    }
}

API_AVAILABLE(macos(26.0)) std::string metalMemoryCensus(Device& device, size_t top)
{
    if (device.backend() != BackendType::Metal)
    {
        return {};
    }
    MetalDevice& metal = static_cast<MetalDevice&>(device);
    U64 resident = 0;
    const std::string groups = metal.residency().census(top, resident);
    return "allocated " + std::to_string(U64(metal.mtlDevice().currentAllocatedSize) / (1024 * 1024)) + " MB, resident set "
           + std::to_string(resident / (1024 * 1024)) + " MB, releases waiting " + std::to_string(metal.lifecycle().pendingReleases())
           + ": " + groups;
}

std::string metalTestFault()
{
    const char* env = metalDebug() ? getenv("LL_METAL_TEST_FAULT") : nullptr;
    return env ? std::string(env) : std::string();
}

bool metalDebug()
{
    const int set = sMetalDebug;
    if (set >= 0)
    {
        return set == 1;
    }
    const char* env = getenv("LL_METAL_DEBUG");
    if (env && std::strcmp(env, "1") == 0)
    {
        return true;
    }
#ifdef LL_DEBUG
    return true;
#else
    return false;
#endif
}

MetalSpeedKnobs metalParseSpeedKnobs(const char* barrier, const char* discard)
{
    MetalSpeedKnobs knobs;
    // One knob: its new value, its old value, or (not set, empty) its default; anything else is named in `unknown`.
    auto parse = [&knobs](const char* name, const char* value, const char* newWay, const char* oldWay, bool& knob)
    {
        if (!value || !*value)
        {
            return;
        }
        if (std::strcmp(value, newWay) == 0 || std::strcmp(value, oldWay) == 0)
        {
            knob = std::strcmp(value, newWay) == 0;
            return;
        }
        knobs.unknown += (knobs.unknown.empty() ? "" : ", ") + std::string(name) + "=" + value;
    };
    parse("LL_METAL_BARRIER", barrier, "split", "full", knobs.splitBarriers);
    parse("LL_METAL_DISCARD", discard, "1", "0", knobs.discardStores);
    return knobs;
}

MetalSpeedKnobs metalSpeedKnobs()
{
    return metalParseSpeedKnobs(getenv("LL_METAL_BARRIER"), getenv("LL_METAL_DISCARD"));
}

Ref<Device> metalBackend(MetalSupport& support, const DeviceCreateInfo* create)
{
    support = MetalSupport();
    // The one availability check in the Metal files: everything the backend calls is macOS 26 API.
    if (@available(macOS 26.0, *))
    {
        support.macOS26 = true;
        askForValidation();
        IdMTLDevice device = MTLCreateSystemDefaultDevice();
        if (device)
        {
            warnWithoutValidation(device);
            support.device = true;
            support.metal4 = [device supportsFamily:MTLGPUFamilyMetal4];
            support.apple7 = [device supportsFamily:MTLGPUFamilyApple7];
            const char* name = device.name.UTF8String;
            support.renderer = name ? name : "";
        }
        if (create && support.device && support.metal4 && support.apple7)
        {
            return createMetalDevice(*create, support);
        }
    }
    return nullptr;
}

} // namespace LL::RHI

#endif // LL_RHI_METAL
