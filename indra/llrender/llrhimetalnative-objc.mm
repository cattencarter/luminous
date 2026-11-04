/**
 * @file llrhimetalnative-objc.mm
 * @brief The Metal backend's native programs: MSL libraries compiled from source, their compute and render pipelines
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

#include <condition_variable>
#include <functional>
#include <mutex>
#include <sstream>

// Native programs are MSL written by hand. A library is compiled from its source text with the device's MTL4Compiler
// in the background (MTL4Compiler.h:238), with the translated programs' options (metalCompileOptions) and the
// defines as preprocessor macros. Its pipelines are compiled in the background too, once the library is there
// (MTL4Compiler.h:270, 303). Every handler is counted in the device's MetalCompileState, so ~MetalDevice waits for it
// as for the translated programs' handlers. The handlers run on Metal's threads and log nothing: what failed is kept
// with the object and read on the main thread (NativeLibrary::log, a dispatch's or draw's WARN line).

namespace LL::RHI
{

namespace
{
    /// A handler is counted before it is sent; false once the device has stopped waiting (as the translated
    /// programs' countHandler).
    bool countNativeHandler(MetalCompileState& state)
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.closing)
        {
            return false;
        }
        ++state.outstanding;
        return true;
    }

    /// A handler's last step.
    void nativeHandlerDone(MetalCompileState& state)
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.handlersAfterClose += state.closing ? 1 : 0;
        --state.outstanding;
        state.changed.notify_all();
    }

    std::string nativeError(NSError* error)
    {
        return std::string("Metal: ") + (error ? error.localizedDescription.UTF8String : "no error given");
    }

    /// What a library's handler reports, and the pipelines that wait for it.
    struct API_AVAILABLE(macos(26.0)) NativeLibraryState
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<CompileStatus> status{ CompileStatus::Pending };
        IdMTLLibrary library = nil;
        std::string log;
        std::vector<std::function<void(IdMTLLibrary)>> waiting;
    };

    /// What a pipeline's handler reports: a compute or a render state.
    template <class State>
    struct API_AVAILABLE(macos(26.0)) NativePipelineState
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<CompileStatus> status{ CompileStatus::Pending };
        State state = nil;
        std::string log;

        void done(State made, const std::string& why)
        {
            std::lock_guard<std::mutex> lock(mutex);
            state = made;
            log = why;
            status.store(made ? CompileStatus::Ready : CompileStatus::Failed, std::memory_order_release);
            changed.notify_all();
        }

        State wait()
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [this]() { return status.load(std::memory_order_relaxed) != CompileStatus::Pending; });
            return state;
        }
    };

    struct API_AVAILABLE(macos(26.0)) ComputeState : NativePipelineState<IdMTLComputePipelineState>
    {
    };
    struct API_AVAILABLE(macos(26.0)) RenderState : NativePipelineState<IdMTLRenderPipelineState>
    {
    };

    /// A function the library has, by name; Metal stops the process on a pipeline whose function is missing, so it is
    /// asked first.
    bool hasFunction(IdMTLLibrary library, const std::string& name) API_AVAILABLE(macos(26.0))
    {
        return library && name.size() && [library.functionNames containsObject:metalString(name)];
    }

    MTL4LibraryFunctionDescriptor* functionDescriptor(IdMTLLibrary library, const std::string& name)
        API_AVAILABLE(macos(26.0))
    {
        MTL4LibraryFunctionDescriptor* descriptor = [MTL4LibraryFunctionDescriptor new]; // MTL4LibraryFunctionDescriptor.h
        descriptor.name = metalString(name);
        descriptor.library = library;
        return descriptor;
    }

    std::string sizeKey(const DispatchSize& size)
    {
        return std::to_string(size.x) + "x" + std::to_string(size.y) + "x" + std::to_string(size.z);
    }
}

// Libraries ---------------------------------------------------------------------------------------------------

class API_AVAILABLE(macos(26.0)) MetalNativeLibrary final : public NativeLibrary
{
public:
    MetalNativeLibrary(MetalDevice& device, std::shared_ptr<NativeLibraryState> state)
    :   mDevice(device), mState(std::move(state))
    {
    }

    ~MetalNativeLibrary() override
    {
        // The library's pipelines leave the device's caches; whoever holds one keeps it.
        mDevice.forgetNativeLibrary(id());
    }

    CompileStatus status() const override { return mState->status.load(std::memory_order_acquire); }

    void wait() override
    {
        std::unique_lock<std::mutex> lock(mState->mutex);
        mState->changed.wait(lock, [this]()
                             { return mState->status.load(std::memory_order_relaxed) != CompileStatus::Pending; });
    }

    std::string log() const override
    {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->log;
    }

    /// Runs then with the library once it is compiled (nil when it failed): now when it is, otherwise on the
    /// library's handler.
    void whenCompiled(std::function<void(IdMTLLibrary)> then)
    {
        IdMTLLibrary library = nil;
        {
            std::lock_guard<std::mutex> lock(mState->mutex);
            if (mState->status.load(std::memory_order_relaxed) == CompileStatus::Pending)
            {
                mState->waiting.push_back(std::move(then));
                return;
            }
            library = mState->library;
        }
        then(library);
    }

private:
    MetalDevice& mDevice;
    std::shared_ptr<NativeLibraryState> mState;
};

Ref<NativeLibrary> MetalDevice::createNativeLibrary(const NativeSource& source)
{
    auto state = std::make_shared<NativeLibraryState>();
    Ref<MetalNativeLibrary> library = new MetalNativeLibrary(*this, state);
    library->setDebugName(source.name);
    const auto fail = [&](const std::string& why)
    {
        LL_WARNS("RHI") << "createNativeLibrary '" << source.name << "': " << why << LL_ENDL;
        std::lock_guard<std::mutex> lock(state->mutex);
        state->log = why;
        state->status.store(CompileStatus::Failed, std::memory_order_release);
        state->changed.notify_all();
    };
    IdMTL4Compiler compiler = this->compiler();
    if (!compiler)
    {
        fail("Metal has no shader compiler"); // compiler() wrote its WARN line too
        return library;
    }
    if (source.text.empty())
    {
        fail("the source is empty");
        return library;
    }
    std::shared_ptr<MetalCompileState> counter = compileState();
    if (!countNativeHandler(*counter))
    {
        fail("the device is closing");
        return library;
    }
    @autoreleasepool
    {
        // The translated programs' options (MSL 4.0 and their math), with the defines as macros (MTLLibrary.h).
        MTLCompileOptions* options = metalCompileOptions(false);
        if (!source.defines.empty())
        {
            NSMutableDictionary<NSString*, NSObject*>* macros = [NSMutableDictionary new];
            for (const auto& [name, value] : source.defines)
            {
                macros[metalString(name)] = metalString(value);
            }
            options.preprocessorMacros = macros;
        }
        MTL4LibraryDescriptor* descriptor = [MTL4LibraryDescriptor new]; // MTL4LibraryDescriptor.h:26-32
        descriptor.source = metalString(source.text);
        descriptor.options = options;
        descriptor.name = metalString(source.name);
        __block std::shared_ptr<NativeLibraryState> held = state;
        [compiler newLibraryWithDescriptor:descriptor
                         completionHandler:^(IdMTLLibrary made, NSError* error)
        {
            std::vector<std::function<void(IdMTLLibrary)>> waiting;
            {
                std::lock_guard<std::mutex> lock(held->mutex);
                held->library = made;
                // Warnings come with a library that compiled; errors without one.
                held->log = error ? nativeError(error) : std::string();
                held->status.store(made ? CompileStatus::Ready : CompileStatus::Failed, std::memory_order_release);
                waiting.swap(held->waiting);
                held->changed.notify_all();
            }
            for (const std::function<void(IdMTLLibrary)>& then : waiting)
            {
                then(made);
            }
            held.reset();
            nativeHandlerDone(*counter);
        }];
    }
    return library;
}

// Compute pipelines -----------------------------------------------------------------------------------------

class API_AVAILABLE(macos(26.0)) MetalComputePipeline final : public ComputePipeline
{
public:
    MetalComputePipeline(FrameLifecycle& lifecycle, U64 libraryId, const ComputePipelineDesc& desc,
                         std::shared_ptr<ComputeState> state)
    :   mLifecycle(lifecycle), mLibraryId(libraryId), mDesc(desc), mState(std::move(state))
    {
    }

    ~MetalComputePipeline() override
    {
        // The state goes once the frames that may have dispatched with it have finished.
        std::shared_ptr<ComputeState> state = std::move(mState);
        mLifecycle.release([state]() mutable { state.reset(); });
    }

    CompileStatus status() const override { return mState->status.load(std::memory_order_acquire); }
    void wait() override { mState->wait(); }
    U64 libraryId() const override { return mLibraryId; }
    const ComputePipelineDesc& desc() const override { return mDesc; }
    U32 executionWidth() const override
    {
        return status() == CompileStatus::Ready ? U32(mState->state.threadExecutionWidth) : 0;
    }
    U32 maxThreadsPerThreadgroup() const override
    {
        return status() == CompileStatus::Ready ? U32(mState->state.maxTotalThreadsPerThreadgroup) : 0;
    }

    /// Waits while it compiles; nil when it failed.
    IdMTLComputePipelineState state() const { return mState->wait(); }
    std::string log() const
    {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->log;
    }

private:
    FrameLifecycle& mLifecycle;
    const U64 mLibraryId;
    const ComputePipelineDesc mDesc;
    std::shared_ptr<ComputeState> mState;
};

IdMTLComputePipelineState metalComputePipelineState(ComputePipeline* pipeline)
{
    return pipeline ? static_cast<MetalComputePipeline*>(pipeline)->state() : nil;
}

Ref<ComputePipeline> MetalDevice::getComputePipeline(NativeLibrary& library, const ComputePipelineDesc& desc)
{
    if (library.status() == CompileStatus::Failed)
    {
        LL_WARNS("RHI") << "getComputePipeline '" << desc.function << "': library '" << library.debugName()
                        << "' failed to compile" << LL_ENDL;
        return nullptr;
    }
    // The library's id first, so forgetNativeLibrary finds its pipelines, and a key apart from PipelineCache's.
    const std::string key = std::to_string(library.id()) + "/compute/" + desc.function + "/" +
                            sizeKey(desc.threadsPerThreadgroup);
    {
        std::lock_guard<std::mutex> lock(mNativeMutex);
        const auto found = mComputePipelines.find(key);
        if (found != mComputePipelines.end())
        {
            return found->second;
        }
    }
    auto state = std::make_shared<ComputeState>();
    Ref<ComputePipeline> pipeline = new MetalComputePipeline(mLifecycle, library.id(), desc, state);
    pipeline->setDebugName(desc.function);
    IdMTL4Compiler compiler = this->compiler();
    std::shared_ptr<MetalCompileState> counter = compileState();
    const std::string name = library.debugName();
    static_cast<MetalNativeLibrary&>(library).whenCompiled([compiler, counter, state, desc, name](IdMTLLibrary made)
    {
        if (!made || !compiler)
        {
            state->done(nil, "library '" + name + "' did not compile");
            return;
        }
        if (!hasFunction(made, desc.function))
        {
            state->done(nil, "library '" + name + "' has no function '" + desc.function + "'");
            return;
        }
        if (!countNativeHandler(*counter))
        {
            state->done(nil, "the device is closing");
            return;
        }
        @autoreleasepool // also on Metal's handler threads
        {
            MTL4ComputePipelineDescriptor* descriptor = [MTL4ComputePipelineDescriptor new]; // MTL4ComputePipeline.h
            descriptor.label = metalString(desc.function);
            descriptor.computeFunctionDescriptor = functionDescriptor(made, desc.function);
            if (desc.threadsPerThreadgroup.count() != 0)
            {
                descriptor.requiredThreadsPerThreadgroup =
                    MTLSizeMake(desc.threadsPerThreadgroup.x, desc.threadsPerThreadgroup.y, desc.threadsPerThreadgroup.z);
            }
            __block std::shared_ptr<ComputeState> held = state;
            [compiler newComputePipelineStateWithDescriptor:descriptor
                                        compilerTaskOptions:nil
                                          completionHandler:^(IdMTLComputePipelineState result, NSError* error)
            {
                held->done(result, result ? std::string() : nativeError(error));
                held.reset();
                nativeHandlerDone(*counter);
            }];
        }
    });
    std::lock_guard<std::mutex> lock(mNativeMutex);
    // When the same key was cached meanwhile, the first is kept, as PipelineCache does.
    return mComputePipelines.emplace(key, pipeline).first->second;
}

// Native render pipelines -------------------------------------------------------------------------------------

/// A render pipeline of a native library: its own class, so nothing takes it for a translated program's
/// MetalPipeline (the backend's accessors ask Pipeline::native first).
class API_AVAILABLE(macos(26.0)) MetalNativePipeline final : public Pipeline
{
public:
    MetalNativePipeline(FrameLifecycle& lifecycle, U64 libraryId, const PipelineDesc& desc,
                        const NativeRenderFunctions& functions, std::shared_ptr<RenderState> state)
    :   mLifecycle(lifecycle), mLibraryId(libraryId), mDesc(desc), mFunctions(functions), mState(std::move(state))
    {
    }

    ~MetalNativePipeline() override
    {
        std::shared_ptr<RenderState> state = std::move(mState);
        mLifecycle.release([state]() mutable { state.reset(); });
    }

    U64 programId() const override { return mLibraryId; }
    const PipelineDesc& desc() const override { return mDesc; }
    CompileStatus status() const override { return mState->status.load(std::memory_order_acquire); }
    bool native() const override { return true; }

    const NativeRenderFunctions& functions() const { return mFunctions; }
    /// Waits while it compiles; nil when it failed, after one WARN line on the main thread.
    IdMTLRenderPipelineState state()
    {
        IdMTLRenderPipelineState made = mState->wait();
        if (!made && !mWarned)
        {
            mWarned = true;
            std::lock_guard<std::mutex> lock(mState->mutex);
            LL_WARNS("RHI") << "native pipeline '" << mFunctions.vertex << "' + '" << mFunctions.fragment
                            << "' failed: " << mState->log << LL_ENDL;
        }
        return made;
    }

private:
    FrameLifecycle& mLifecycle;
    const U64 mLibraryId;
    const PipelineDesc mDesc;
    const NativeRenderFunctions mFunctions;
    std::shared_ptr<RenderState> mState;
    bool mWarned = false;
};

IdMTLRenderPipelineState metalNativePipelineState(Pipeline* pipeline)
{
    return pipeline && pipeline->native() ? static_cast<MetalNativePipeline*>(pipeline)->state() : nil;
}

Ref<Pipeline> MetalDevice::getNativePipeline(NativeLibrary& library, const NativeRenderFunctions& functions,
                                             const PipelineDesc& desc)
{
    const auto refuse = [&](const std::string& why) -> Ref<Pipeline>
    {
        LL_WARNS("RHI") << "getNativePipeline '" << functions.vertex << "' + '" << functions.fragment << "': " << why
                        << LL_ENDL;
        return nullptr;
    };
    if (library.status() == CompileStatus::Failed)
    {
        return refuse("library '" + library.debugName() + "' failed to compile");
    }
    if (desc.attributeMask != 0)
    {
        return refuse("a native vertex function reads no vertex streams, so attributeMask must be 0");
    }
    std::string why = validate(desc, mCaps);
    if (!why.empty())
    {
        return refuse(why);
    }
    if (desc.samples == 0 || ![mDevice supportsTextureSampleCount:desc.samples])
    {
        return refuse("the device has no " + std::to_string(desc.samples) + "-sample rendering");
    }
    for (U32 i = 0; i < std::min(desc.colorCount, PipelineDesc::MAX_COLOR_ATTACHMENTS); ++i)
    {
        if (metalPixelFormat(desc.colorFormats[i]) == MTLPixelFormatInvalid)
        {
            return refuse("color attachment " + std::to_string(i) + " has no Metal format");
        }
    }
    // The key: the library first (forgetNativeLibrary), then the functions and the normalized description, whose
    // depth format is kept so the tracker compares it with the pass's (Metal 4's state has none).
    const PipelineDesc key = normalized(desc);
    const std::string text = std::to_string(library.id()) + "/render/" + nativePipelineKey(functions, key);
    {
        std::lock_guard<std::mutex> lock(mNativeMutex);
        const auto found = mNativePipelines.find(text);
        if (found != mNativePipelines.end())
        {
            return found->second;
        }
    }
    auto state = std::make_shared<RenderState>();
    Ref<Pipeline> pipeline = new MetalNativePipeline(mLifecycle, library.id(), key, functions, state);
    pipeline->setDebugName(library.debugName() + ":" + functions.fragment);
    IdMTL4Compiler compiler = this->compiler();
    std::shared_ptr<MetalCompileState> counter = compileState();
    const std::string name = library.debugName();
    static_cast<MetalNativeLibrary&>(library).whenCompiled([compiler, counter, state, key, functions, name](
                                                               IdMTLLibrary made)
    {
        if (!made || !compiler)
        {
            state->done(nil, "library '" + name + "' did not compile");
            return;
        }
        for (const std::string& function : { functions.vertex, functions.fragment })
        {
            if (!hasFunction(made, function))
            {
                state->done(nil, "library '" + name + "' has no function '" + function + "'");
                return;
            }
        }
        if (!countNativeHandler(*counter))
        {
            state->done(nil, "the device is closing");
            return;
        }
        @autoreleasepool // also on Metal's handler threads
        {
            // A full state: the functions, no vertex descriptor (the vertex function makes its own vertices), the
            // samples, the topology and each attachment's format, write mask and blending (MTL4RenderPipeline.h).
            MTL4RenderPipelineDescriptor* descriptor = [MTL4RenderPipelineDescriptor new];
            descriptor.label = metalString(name + ":" + functions.fragment);
            descriptor.vertexFunctionDescriptor = functionDescriptor(made, functions.vertex);
            descriptor.fragmentFunctionDescriptor = functionDescriptor(made, functions.fragment);
            descriptor.rasterSampleCount = key.samples;
            switch (key.primitiveClass)
            {
            case PrimitiveClass::Point: descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint; break;
            case PrimitiveClass::Line:  descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
            default:                    descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; break;
            }
            const U32 count = std::min(key.colorCount, PipelineDesc::MAX_COLOR_ATTACHMENTS);
            for (U32 i = 0; i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
            {
                MTL4RenderPipelineColorAttachmentDescriptor* attachment = descriptor.colorAttachments[i];
                if (i >= count)
                {
                    attachment.pixelFormat = MTLPixelFormatInvalid;
                    continue;
                }
                attachment.pixelFormat = metalPixelFormat(key.colorFormats[i]);
                attachment.writeMask = metalWriteMask(key.colorMask);
                const bool blends = key.blend.enabled && !isInteger(key.colorFormats[i]);
                attachment.blendingState = blends ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
                metalSetBlend(attachment, blends ? key.blend : BlendState());
            }
            __block std::shared_ptr<RenderState> held = state;
            [compiler newRenderPipelineStateWithDescriptor:descriptor
                                       compilerTaskOptions:nil
                                         completionHandler:^(IdMTLRenderPipelineState result, NSError* error)
            {
                held->done(result, result ? std::string() : nativeError(error));
                held.reset();
                nativeHandlerDone(*counter);
            }];
        }
    });
    std::lock_guard<std::mutex> lock(mNativeMutex);
    return mNativePipelines.emplace(text, pipeline).first->second;
}

void MetalDevice::forgetNativeLibrary(U64 libraryId)
{
    // Released outside the lock: a pipeline's destructor hands its state to the lifecycle.
    std::vector<Ref<ComputePipeline>> computes;
    std::vector<Ref<Pipeline>> renders;
    const std::string prefix = std::to_string(libraryId) + "/";
    {
        std::lock_guard<std::mutex> lock(mNativeMutex);
        for (auto it = mComputePipelines.begin(); it != mComputePipelines.end();)
        {
            if (it->first.starts_with(prefix))
            {
                computes.push_back(std::move(it->second));
                it = mComputePipelines.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = mNativePipelines.begin(); it != mNativePipelines.end();)
        {
            if (it->first.starts_with(prefix))
            {
                renders.push_back(std::move(it->second));
                it = mNativePipelines.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

size_t MetalDevice::nativePipelineCount() const
{
    std::lock_guard<std::mutex> lock(mNativeMutex);
    return mComputePipelines.size() + mNativePipelines.size();
}

} // namespace LL::RHI

#endif // LL_RHI_METAL
