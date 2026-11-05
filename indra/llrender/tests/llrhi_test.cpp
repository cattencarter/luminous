/**
 * @file llrhi_test.cpp
 * @brief Tests of the render hardware interface's shared code with a test backend
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

#include "../llrhi.h"
#include "../llmslinclude.h"

#if LL_DARWIN
#include "../llrhimetal.h"
#endif

#include "llglheaders.h"

#include "../test/lltut.h"
#include "tests/wrapllerrs.h"

#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

using namespace LL::RHI;

// The device the next Device::create() for GL gives, which a test sets; null when none is set.
static Ref<Device> sNextGLDevice;

// The GL backend is tested on its own (llrhigl_test.cpp); Device::create() reaches it here only
// through this stub.
Ref<Device> LL::RHI::createGLDevice(const DeviceCreateInfo&)
{
    return std::exchange(sNextGLDevice, nullptr);
}

#if LL_RHI_METAL
// What the Mac offers in the next metalBackend(), which a test sets, and how often it was asked.
static MetalSupport sMetalSupport;
static int sMetalBackendCalls = 0;

// The real entry point is in llrhimetaldevice-objc.mm and runs in INTEGRATION_TEST_llrhimetal;
// llrhi.cpp reaches it here only through this stub.
Ref<Device> LL::RHI::metalBackend(MetalSupport& support, const DeviceCreateInfo*)
{
    ++sMetalBackendCalls;
    support = sMetalSupport;
    return nullptr;
}
#endif

namespace
{
    // The test backend ---------------------------------------------------------------------------
    // It does what a real backend does with the shared code: validate before creating, log a
    // failed creation, hand native objects to the lifecycle when an object goes, and cache samplers.
    // Its "native objects" are counters.

    struct NativeCounter
    {
        std::atomic<int> created{ 0 };
        std::atomic<int> freed{ 0 };
    };

    /// The GPU's side of the lifecycle: records the frames the lifecycle waits for, and can time out.
    struct TestGpu
    {
        std::vector<U64> waits;
        bool finishes = true;
        U32 failingWaits = 0; ///< P3-01i S2: these next waits time out, then finishes decides again

        FrameLifecycle::WaitForFrame waitFunction()
        {
            return [this](U64 frame)
            {
                waits.push_back(frame);
                if (failingWaits > 0)
                {
                    --failingWaits;
                    return false;
                }
                return finishes;
            };
        }
    };

    class TestFence : public Fence
    {
    public:
        bool signaled() const override { return mSignaled.load(); }
        void wait() override { mSignaled = true; }
        void signal() { mSignaled = true; }

    private:
        std::atomic<bool> mSignaled{ false };
    };

    class TestBuffer : public Buffer
    {
    public:
        TestBuffer(FrameLifecycle& lifecycle, NativeCounter& native, const BufferDesc& desc)
        :   mLifecycle(lifecycle), mNative(native), mDesc(desc)
        {
            ++native.created;
        }

        ~TestBuffer() override
        {
            NativeCounter* native = &mNative;
            mLifecycle.release([native] { ++native->freed; });
        }

        const BufferDesc& desc() const override { return mDesc; }

    private:
        FrameLifecycle& mLifecycle;
        NativeCounter& mNative;
        BufferDesc mDesc;
    };

    class TestTexture : public Texture
    {
    public:
        TestTexture(FrameLifecycle& lifecycle, NativeCounter& native, const TextureDesc& desc)
        :   mLifecycle(lifecycle), mNative(native), mDesc(desc)
        {
            ++native.created;
        }

        ~TestTexture() override
        {
            NativeCounter* native = &mNative;
            std::vector<Ref<Fence>> uploads;
            if (mUpload.notNull())
            {
                uploads.push_back(mUpload);
            }
            mLifecycle.release([native] { ++native->freed; }, std::move(uploads));
        }

        const TextureDesc& desc() const override { return mDesc; }

        /// What an upload context does when it records an upload of this texture.
        void uploadedWith(const Ref<Fence>& fence) { mUpload = fence; }

    private:
        FrameLifecycle& mLifecycle;
        NativeCounter& mNative;
        TextureDesc mDesc;
        Ref<Fence> mUpload;
    };

    class TestSampler : public Sampler
    {
    public:
        TestSampler(FrameLifecycle& lifecycle, NativeCounter& native, const SamplerDesc& desc)
        :   mLifecycle(lifecycle), mNative(native), mDesc(desc)
        {
            ++native.created;
        }

        ~TestSampler() override
        {
            NativeCounter* native = &mNative;
            mLifecycle.release([native] { ++native->freed; });
        }

        const SamplerDesc& desc() const override { return mDesc; }

    private:
        FrameLifecycle& mLifecycle;
        NativeCounter& mNative;
        SamplerDesc mDesc;
    };

    class TestProgram : public Program
    {
    public:
        TestProgram(FrameLifecycle& lifecycle, NativeCounter& native, PipelineCache& pipelines,
                    const ProgramDesc& desc)
        :   mLifecycle(lifecycle), mNative(native), mPipelines(pipelines)
        {
            ++native.created;
            setDebugName(desc.name);
            mReflection.attributeMask = desc.attributeMask;
        }

        ~TestProgram() override
        {
            // As a real backend does when a program dies: its pipelines leave the cache.
            mPipelines.forgetProgram(id());
            NativeCounter* native = &mNative;
            mLifecycle.release([native] { ++native->freed; });
        }

        CompileStatus status() const override { return mStatus; }
        void wait() override { mStatus = CompileStatus::Ready; }
        const ProgramReflection& reflection() const override { return mReflection; }

        CompileStatus mStatus = CompileStatus::Pending;
        ProgramReflection mReflection;

    private:
        FrameLifecycle& mLifecycle;
        NativeCounter& mNative;
        PipelineCache& mPipelines;
    };

    class TestPipeline : public Pipeline
    {
    public:
        TestPipeline(FrameLifecycle& lifecycle, NativeCounter& native, const PipelineKey& key)
        :   mLifecycle(lifecycle), mNative(native), mKey(key)
        {
            ++native.created;
        }

        ~TestPipeline() override
        {
            NativeCounter* native = &mNative;
            mLifecycle.release([native] { ++native->freed; });
        }

        U64 programId() const override { return mKey.programId; }
        const PipelineDesc& desc() const override { return mKey.desc; }
        CompileStatus status() const override { return mStatus; }

        CompileStatus mStatus = CompileStatus::Ready;

    private:
        FrameLifecycle& mLifecycle;
        NativeCounter& mNative;
        PipelineKey mKey;
    };

    class TestDepthStencil : public DepthStencilState
    {
    public:
        explicit TestDepthStencil(const DepthStencilDesc& desc) : mDesc(desc) {}
        const DepthStencilDesc& desc() const override { return mDesc; }

    private:
        DepthStencilDesc mDesc;
    };

    class TestQuery : public Query
    {
    public:
        explicit TestQuery(QueryType type) : mType(type) {}
        QueryType type() const override { return mType; }
        bool ready() const override { return true; }
        U64 result(bool wait) override { return 0; }

    private:
        QueryType mType;
    };

    class TestReadback : public Readback
    {
    public:
        explicit TestReadback(size_t bytes) : mData(bytes) {}
        bool ready() const override { return true; }
        void wait() override {}
        std::span<const std::byte> data() const override { return mData; }
        U32 rowPitch() const override { return 0; }

    private:
        std::vector<std::byte> mData;
    };

    /// Records each call the tracker accepts, and each one it rejects with the reason, as a real
    /// backend logs and skips it with its checks on (decision 11).
    class TestEncoder : public Encoder
    {
    public:
        explicit TestEncoder(const Capabilities& caps) : tracker(caps) {}

        bool beginRenderPass(const RenderPassDesc& pass) override
        {
            return record("begin " + pass.name, tracker.beginRenderPass(pass));
        }
        void endRenderPass() override { close(tracker.endRenderPass()); }
        void clear(bool color, const std::array<F32, 4>& rgba, bool depth, F32 depthValue) override
        {
            record("clear", tracker.clear(color, depth));
        }
        void setPipeline(Pipeline* pipeline) override { record("pipeline", tracker.setPipeline(pipeline)); }
        void setDepthStencilState(DepthStencilState* state) override
        {
            record("depth state", tracker.setDepthStencilState(state));
        }
        void setViewport(S32 x, S32 y, S32 width, S32 height) override
        {
            record("viewport", tracker.setViewport(x, y, width, height));
        }
        void setScissor(bool enabled, S32 x, S32 y, S32 width, S32 height) override
        {
            record("scissor", tracker.setScissor(enabled, x, y, width, height));
        }
        void setCullMode(CullMode mode) override { record("cull", tracker.setCullMode(mode)); }
        void setFillMode(FillMode mode) override { record("fill", tracker.setFillMode(mode)); }
        void setDepthBias(F32 factor, F32 units, bool enableFill, bool enableLines) override
        {
            record("bias", tracker.setDepthBias(factor, units, enableFill, enableLines));
        }
        void setDepthClamp(bool enabled) override { record("clamp", tracker.setDepthClamp(enabled)); }
        void setPointSize(F32 size) override { record("point size", tracker.setPointSize(size)); }
        void setLineWidth(F32 width) override { record("line width", tracker.setLineWidth(width)); }
        void setUniform(Program* program, U32 uniformIndex, std::span<const std::byte> value,
                        U32 count) override
        {
            record("uniform", tracker.setUniform(program, uniformIndex, value.size(), count));
        }
        void setSamplerChannel(Program* program, U32 uniformIndex, U32 channel) override
        {
            record("channel", tracker.setSamplerChannel(program, uniformIndex));
        }
        void setVertexBuffer(Buffer* buffer, const VertexStreams& streams) override
        {
            record("vertices", tracker.setVertexBuffer(buffer, streams));
        }
        void setIndexBuffer(Buffer* buffer, IndexType type) override
        {
            record("indices", tracker.setIndexBuffer(buffer, type));
        }
        void setTexture(U32 channel, Texture* texture, const SamplerDesc& sampling) override
        {
            record("texture", tracker.setTexture(channel, texture));
        }
        void setUniformBuffer(U32 blockIndex, Buffer* buffer, U64 offset, U64 size) override
        {
            record("block", tracker.setUniformBuffer(blockIndex, buffer, offset, size));
        }
        void draw(PrimitiveType type, U32 firstVertex, U32 vertexCount) override
        {
            record("draw", tracker.draw(type));
        }
        void drawIndexed(PrimitiveType type, U32 firstIndex, U32 indexCount) override
        {
            record("draw indexed", tracker.drawIndexed(type, firstIndex, indexCount));
        }
        void updateBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data) override
        {
            record("update", tracker.updateBuffer(buffer, offset, data.size()));
        }
        void uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region,
                           const PixelLayout& layout, std::span<const std::byte> data) override
        {
            record("upload", tracker.uploadTexture(texture, level, layer, region, layout, data.size()));
        }
        void copyTexture(Texture* src, U32 srcLevel, U32 srcLayer, const Region& srcRegion,
                         Texture* dst, U32 dstLevel, U32 dstLayer, U32 dstX, U32 dstY) override
        {
            record("copy", tracker.copyTexture(src, srcLevel, srcLayer, srcRegion, dst, dstLevel,
                                               dstLayer, dstX, dstY));
        }
        void generateMipmaps(Texture* texture) override { record("mipmaps", tracker.generateMipmaps(texture)); }
        void waitForFence(Fence* fence) override {} // the tracker has no rule for a GPU-side wait (C47)
        Ref<Readback> readback(Texture* texture, U32 level, U32 layer, const Region& region,
                               const PixelLayout& layout) override
        {
            if (!record("readback", tracker.readback(texture, level, layer, region)))
            {
                return nullptr;
            }
            return new TestReadback(size_t(region.width) * region.height * layout.bytesPerPixel());
        }
        Ref<Readback> readback(Buffer* buffer, U64 offset, U64 size) override
        {
            if (!record("readback", tracker.readback(buffer, offset, size)))
            {
                return nullptr;
            }
            return new TestReadback(size);
        }
        bool beginQuery(Query* query) override { return record("begin query", tracker.beginQuery(query)); }
        void endQuery(Query* query) override { record("end query", tracker.endQuery(query)); }
        void pushDebugGroup(std::string_view name) override { record("push", tracker.pushDebugGroup()); }
        void popDebugGroup() override { record("pop", tracker.popDebugGroup()); }
        const EncoderTracker& state() const override { return tracker; }

        /// What a backend does with its native encoder when the tracker closes a pass or a frame:
        /// it closes them whether or not there is a warning.
        void close(const EncoderTracker::Closing& closing)
        {
            if (closing.popInPass > 0)
            {
                log.push_back("pop " + std::to_string(closing.popInPass));
            }
            if (closing.endPass)
            {
                log.push_back("end");
            }
            if (closing.popOutside > 0)
            {
                log.push_back("pop " + std::to_string(closing.popOutside) + " outside");
            }
            if (!closing.warning.empty())
            {
                errors.push_back("close: " + closing.warning);
            }
        }

        bool record(const std::string& call, const std::string& why)
        {
            if (why.empty())
            {
                log.push_back(call);
                return true;
            }
            errors.push_back(call + ": " + why);
            return false;
        }

        EncoderTracker tracker;
        std::vector<std::string> log;
        std::vector<std::string> errors;
    };

    /// The upload thread's context: uploads made before a submit share its fence, which the test
    /// signals when "the GPU" has finished them.
    class TestUploadContext : public UploadContext
    {
    public:
        void updateBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data) override {}
        void uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region,
                           const PixelLayout& layout, std::span<const std::byte> data) override;
        void generateMipmaps(Texture* texture) override {}
        Ref<Fence> submit() override
        {
            Ref<Fence> sent = mPending.notNull() ? mPending : Ref<Fence>(new TestFence);
            mPending = nullptr;
            return sent;
        }

        std::string lastError;

    private:
        Ref<Fence> mPending;
    };

    class TestSurface : public Surface
    {
    public:
        TestSurface(Ref<Texture> color, Ref<Texture> depth) : mColor(color), mDepth(depth) {}
        Texture* backBuffer() override { return mColor.get(); }
        Texture* backBufferDepth() override { return mDepth.get(); }
        void resize(U32 width, U32 height) override {}
        void setVSync(bool enabled) override {}
        void present() override {}

    private:
        Ref<Texture> mColor;
        Ref<Texture> mDepth;
    };

    TextureDesc texture2D(U32 width, U32 height, Format format = Format::RGBA8Unorm);

    /// Limits as Metal reports them on Apple Silicon (decision 10).
    Capabilities testCapabilities()
    {
        Capabilities caps;
        caps.maxTextureSize = 16384;
        caps.maxArrayLayers = 2048;
        caps.maxColorAttachments = 8;
        caps.maxSamples = 8;
        caps.maxSamplersPerStage = 16;
        caps.timerQueries = true;
        caps.cubeMapArrays = true;
        caps.bcCompression = true;
        caps.textureSwizzle = true;
        return caps;
    }

    class TestDevice : public Device
    {
    public:
        TestDevice(TestGpu& gpu, NativeCounter& native)
        :   mLifecycle(gpu.waitFunction()), mNative(native), mCaps(testCapabilities()), mEncoder(mCaps)
        {
            TextureDesc color = texture2D(1280, 720);
            color.usage = TEXTURE_SAMPLED | TEXTURE_RENDER_TARGET | TEXTURE_READBACK;
            TextureDesc depth = texture2D(1280, 720, Format::Depth32Float);
            depth.usage = TEXTURE_SAMPLED | TEXTURE_RENDER_TARGET;
            // The surface's textures count apart, so the tests count only what they make.
            mSurface = new TestSurface(new TestTexture(mLifecycle, mSurfaceNative, color),
                                       new TestTexture(mLifecycle, mSurfaceNative, depth));
        }

        ~TestDevice() override
        {
            // As a real backend shuts down: the GPU is idle (always, in the test), the caches let go
            // of their objects, and then everything released is freed.
            mEncoder.tracker = EncoderTracker();
            mSurface = nullptr;
            mPipelines.clear();
            mDepthStencils.clear();
            mSamplers.clear();
            mLifecycle.releaseAll();
        }

        BackendType backend() const override { return BackendType::GL; }
        const Capabilities& capabilities() const override { return mCaps; }

        Ref<Buffer> createBuffer(const BufferDesc& desc, std::span<const std::byte> initial,
                                 std::string_view debugName) override
        {
            const std::string why = validate(desc, initial.size());
            if (!why.empty())
            {
                warnCreateFailed("buffer", debugName, why);
                return nullptr;
            }
            Ref<Buffer> buffer = new TestBuffer(mLifecycle, mNative, desc);
            buffer->setDebugName(debugName);
            return buffer;
        }

        Ref<Texture> createTexture(const TextureDesc& desc, std::string_view debugName) override
        {
            const std::string why = validate(desc, mCaps);
            if (!why.empty())
            {
                warnCreateFailed("texture", debugName, why);
                return nullptr;
            }
            Ref<Texture> texture = new TestTexture(mLifecycle, mNative, desc);
            texture->setDebugName(debugName);
            return texture;
        }

        Ref<Sampler> getSampler(const SamplerDesc& desc) override
        {
            return mSamplers.get(desc, [this](const SamplerDesc& wanted) -> Ref<Sampler>
            {
                if (mSamplersFail)
                {
                    return nullptr;
                }
                ++mSamplersMade;
                return new TestSampler(mLifecycle, mNative, wanted);
            });
        }

        Ref<Program> createProgram(const ProgramDesc& desc) override
        {
            const std::string why = validate(desc, mCaps);
            if (!why.empty())
            {
                warnCreateFailed("program", desc.name, why);
                return nullptr;
            }
            return new TestProgram(mLifecycle, mNative, mPipelines, desc);
        }

        std::vector<Ref<Query>> createQueries(QueryType type, U32 count) override
        {
            const std::string why = validateQuery(type, mCaps);
            if (!why.empty())
            {
                warnCreateFailed("query", {}, why);
                return {};
            }
            std::vector<Ref<Query>> queries;
            for (U32 i = 0; i < count; ++i)
            {
                queries.push_back(new TestQuery(type));
            }
            return queries;
        }

        Ref<Pipeline> getPipeline(Program& program, const PipelineDesc& desc) override
        {
            const std::string why = validate(desc, mCaps);
            if (!why.empty())
            {
                warnCreateFailed("pipeline", program.debugName(), why);
                return nullptr;
            }
            return mPipelines.get({ program.id(), desc }, [this, &program](const PipelineKey& key) -> Ref<Pipeline>
            {
                ++mPipelinesMade;
                Ref<Pipeline> pipeline = new TestPipeline(mLifecycle, mNative, key);
                pipeline->setDebugName(program.debugName()); // as the GL and Metal devices name theirs (P2-R2)
                return pipeline;
            });
        }

        Ref<DepthStencilState> getDepthStencilState(const DepthStencilDesc& desc) override
        {
            return mDepthStencils.get(desc, [](const DepthStencilDesc& wanted) -> Ref<DepthStencilState>
            {
                return new TestDepthStencil(wanted);
            });
        }

        Surface* surface() override { return mSurface.get(); }
        Encoder& encoder() override { return mEncoder; }
        UploadContext& uploadContext() override { return mUploads; }

        void beginFrame() override {}
        void endFrame() override
        {
            // The tracker first, so what it lets go belongs to the frame that ends.
            mEncoder.close(mEncoder.tracker.endFrame());
            mLifecycle.endFrame();
        }
        U64 frameNumber() const override { return mLifecycle.currentFrame(); }
        U64 completedFrame() const override { return mLifecycle.completedFrame(); }
        void submitAndWait() override { mLifecycle.idle(); }

        FrameLifecycle mLifecycle;
        NativeCounter& mNative;
        Capabilities mCaps;
        DescCache<SamplerDesc, Sampler, SamplerDescHash> mSamplers;
        DescCache<DepthStencilDesc, DepthStencilState, DepthStencilDescHash> mDepthStencils;
        PipelineCache mPipelines;
        TestEncoder mEncoder;
        TestUploadContext mUploads;
        NativeCounter mSurfaceNative;
        Ref<TestSurface> mSurface;
        std::atomic<int> mPipelinesMade{ 0 };
        int mSamplersMade = 0;
        bool mSamplersFail = false;
    };

    TextureDesc texture2D(U32 width, U32 height, Format format)
    {
        TextureDesc desc;
        desc.format = format;
        desc.width = width;
        desc.height = height;
        return desc;
    }

    void TestUploadContext::uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region,
                                          const PixelLayout& layout, std::span<const std::byte> data)
    {
        lastError = validateUpload(texture->desc(), level, layer, region, layout, data.size());
        if (!lastError.empty())
        {
            return;
        }
        // The upload uses the texture until the GPU has run it, so the texture's deletion waits
        // for this submit's fence.
        if (mPending.isNull())
        {
            mPending = new TestFence;
        }
        static_cast<TestTexture*>(texture)->uploadedWith(mPending);
    }

    ProgramDesc program(const std::string& name, U32 attributeMask = 1)
    {
        ProgramDesc desc;
        desc.name = name;
        desc.attributeMask = attributeMask;
        desc.stages = { { ShaderStage::Vertex, { "void main() {}" } },
                        { ShaderStage::Fragment, { "void main() {}" } } };
        return desc;
    }

    PipelineDesc colorPipeline(Format color = Format::RGBA8Unorm)
    {
        PipelineDesc desc;
        desc.attributeMask = 1;
        desc.colorCount = 1;
        desc.colorFormats[0] = color;
        desc.depthFormat = Format::Depth32Float;
        return desc;
    }

    TextureDesc target(U32 width, U32 height, Format format = Format::RGBA8Unorm, U32 samples = 1)
    {
        TextureDesc desc = texture2D(width, height, format);
        desc.usage = TEXTURE_SAMPLED | TEXTURE_RENDER_TARGET;
        desc.samples = samples;
        return desc;
    }

    /// A pass with one color attachment and a Depth32Float depth, each new, of a square size, as
    /// colorPipeline() draws in with its defaults (P2-01b).
    RenderPassDesc colorDepthPass(Device& device, const std::string& name, U32 size,
                                  Format color = Format::RGBA8Unorm, U32 samples = 1)
    {
        RenderPassDesc pass;
        pass.name = name;
        pass.colorCount = 1;
        pass.color[0].texture = device.createTexture(target(size, size, color, samples), name);
        pass.depth.texture = device.createTexture(target(size, size, Format::Depth32Float, samples),
                                                  name + " depth");
        return pass;
    }

    template <class T>
    std::span<const std::byte> bytesOf(const T& value, size_t count = 1)
    {
        return { reinterpret_cast<const std::byte*>(&value), sizeof(T) * count };
    }

    BufferDesc vertexBuffer(U64 size)
    {
        BufferDesc desc;
        desc.size = size;
        return desc;
    }

    /// An object that releases another when its native objects are freed, as an object that owns
    /// another does.
    struct Chain
    {
        FrameLifecycle* lifecycle = nullptr;
        int freed = 0;
    };

    /// Declared right after a test's TestGpu and NativeCounter, so the viewer's device goes before
    /// they do, also when an ensure throws: its lifecycle waits through the TestGpu and its objects
    /// count into the NativeCounter, and no later test may find it in the accessor.
    struct ViewerDeviceGuard
    {
        ~ViewerDeviceGuard()
        {
            releaseViewerDevice();
            setViewerBackend(BackendType::GL);
            sNextGLDevice = nullptr;
        }
    };
}

namespace tut
{
    struct llrhi_data
    {
    };
    typedef test_group<llrhi_data> llrhi_group;
    typedef llrhi_group::object object;
    llrhi_group llrhi_test_group("LLRHI");

    // Objects ----------------------------------------------------------------------------------

    template<> template<>
    void object::test<1>()
    {
        set_test_name("object ids are unique, never 0, and an object goes with its last reference");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        std::set<U64> ids{ device->id() };
        for (int i = 0; i < 100; ++i)
        {
            Ref<Buffer> buffer = device->createBuffer(vertexBuffer(64), {}, "buffer");
            ensure("created", buffer.notNull());
            ensure("id is not 0", buffer->id() != 0);
            ensure("id is new", ids.insert(buffer->id()).second);
        }
        ensure_equals("each buffer went with its last reference", device->mLifecycle.pendingReleases(), 100u);
    }

    template<> template<>
    void object::test<2>()
    {
        set_test_name("the debug name is kept");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Texture> texture = device->createTexture(texture2D(256, 256), "UI atlas");
        ensure_equals(texture->debugName(), std::string("UI atlas"));
        texture->setDebugName("renamed");
        ensure_equals(texture->debugName(), std::string("renamed"));
    }

    // Frames (decision 2) ------------------------------------------------------------------------

    template<> template<>
    void object::test<3>()
    {
        set_test_name("at most three frames in flight, counting the one being recorded");
        TestGpu gpu;
        FrameLifecycle lifecycle(gpu.waitFunction());
        ensure_equals("first frame", lifecycle.currentFrame(), 1u);
        ensure_equals("nothing finished", lifecycle.completedFrame(), 0u);

        lifecycle.endFrame(); // recording 2; 1 in flight
        lifecycle.endFrame(); // recording 3; 1 and 2 in flight
        ensure("no wait for the first three frames", gpu.waits.empty());

        lifecycle.endFrame(); // recording 4 needs frame 1 done
        ensure_equals("one wait", gpu.waits.size(), 1u);
        ensure_equals("waited for frame 1", gpu.waits[0], 1u);
        ensure_equals("recording frame 4", lifecycle.currentFrame(), 4u);
        // The wait returned; the backend's own report may come later (the review's race).
        ensure_equals("the waited frame counts as finished", lifecycle.completedFrame(), 1u);

        lifecycle.endFrame();
        ensure_equals("then frame 2", gpu.waits.back(), 2u);
    }

    template<> template<>
    void object::test<4>()
    {
        set_test_name("no wait when the GPU has already reported the frame");
        TestGpu gpu;
        FrameLifecycle lifecycle(gpu.waitFunction());
        lifecycle.endFrame();
        lifecycle.endFrame();
        lifecycle.frameCompleted(1);
        lifecycle.endFrame();
        ensure("no wait", gpu.waits.empty());
        ensure_equals("recording frame 4", lifecycle.currentFrame(), 4u);
    }

    template<> template<>
    void object::test<5>()
    {
        set_test_name("a frame that does not finish within two waits makes the device lost: not finished, no frame "
                      "begun, nothing freed, and endFrame does nothing after; one wait that times out is not lost "
                      "(P3-01i S2)");
        {
            TestGpu gpu;
            gpu.finishes = false;
            FrameLifecycle lifecycle(gpu.waitFunction());
            int freed = 0;
            lifecycle.release([&freed] { ++freed; }); // released in frame 1
            lifecycle.endFrame();
            lifecycle.endFrame();
            ensure("not lost before a wait", !lifecycle.lost());
            lifecycle.endFrame(); // recording 4 needs frame 1
            ensure("lost", lifecycle.lost());
            ensure_equals("two waits", gpu.waits.size(), 2u);
            ensure_equals("both for frame 1", gpu.waits[0] + gpu.waits[1], 2u);
            ensure_equals("frame 1 not finished", lifecycle.completedFrame(), 0u);
            ensure_equals("no frame begun", lifecycle.currentFrame(), 3u);
            ensure_equals("nothing freed", freed, 0);
            ensure_equals("the release stays", lifecycle.pendingReleases(), 1u);
            lifecycle.endFrame();
            ensure_equals("then no wait", gpu.waits.size(), 2u);
            ensure_equals("and no frame", lifecycle.currentFrame(), 3u);
            lifecycle.releaseAll(); // the device's shutdown
            ensure_equals("freed at shutdown", freed, 1);
        }
        {
            TestGpu gpu;
            gpu.failingWaits = 1;
            FrameLifecycle lifecycle(gpu.waitFunction());
            lifecycle.endFrame();
            lifecycle.endFrame();
            lifecycle.endFrame();
            ensure("one wait timed out, the second saw the frame: not lost", !lifecycle.lost());
            ensure_equals("two waits", gpu.waits.size(), 2u);
            ensure_equals("frame 1 finished", lifecycle.completedFrame(), 1u);
            ensure_equals("recording frame 4", lifecycle.currentFrame(), 4u);
        }
    }

    template<> template<>
    void object::test<6>()
    {
        set_test_name("reports out of order keep the highest finished frame");
        TestGpu gpu;
        FrameLifecycle lifecycle(gpu.waitFunction());
        for (int i = 0; i < 5; ++i)
        {
            lifecycle.frameCompleted(lifecycle.currentFrame() - 1);
            lifecycle.endFrame();
        }
        ensure_equals("recording frame 6", lifecycle.currentFrame(), 6u);
        lifecycle.frameCompleted(5);
        lifecycle.frameCompleted(3);
        ensure_equals("still 5", lifecycle.completedFrame(), 5u);
    }

    // Deletion (decision 2) ----------------------------------------------------------------------

    template<> template<>
    void object::test<7>()
    {
        set_test_name("native objects go only when the GPU has finished their frame");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Buffer> buffer = device->createBuffer(vertexBuffer(1024), {}, "vertices");
        buffer = nullptr; // released while frame 1 is recorded
        ensure_equals("waiting", device->mLifecycle.pendingReleases(), 1u);
        ensure_equals("not freed in its own frame", device->mLifecycle.collect(), 0u);

        device->endFrame(); // recording 2, frame 1 not finished
        ensure_equals("not freed while frame 1 runs", native.freed.load(), 0);

        device->mLifecycle.frameCompleted(1);
        ensure_equals("freed once frame 1 finished", device->mLifecycle.collect(), 1u);
        ensure_equals("freed once", native.freed.load(), 1);
        ensure_equals("nothing left", device->mLifecycle.pendingReleases(), 0u);
    }

    template<> template<>
    void object::test<8>()
    {
        set_test_name("deletion also waits for the uploads that use the object");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<TestFence> upload = new TestFence;
        {
            Ref<Texture> texture = device->createTexture(texture2D(512, 512), "avatar bake");
            static_cast<TestTexture*>(texture.get())->uploadedWith(upload);
        }
        device->mLifecycle.frameCompleted(1);
        device->endFrame();
        ensure_equals("frame finished, upload not", native.freed.load(), 0);
        ensure_equals("still waiting", device->mLifecycle.pendingReleases(), 1u);

        upload->signal();
        ensure_equals("freed after the upload", device->mLifecycle.collect(), 1u);
        ensure_equals("freed once", native.freed.load(), 1);
    }

    template<> template<>
    void object::test<9>()
    {
        set_test_name("submitAndWait frees earlier frames' objects but not the current frame's");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        device->createBuffer(vertexBuffer(16), {}, "frame 1");
        device->endFrame();
        device->createBuffer(vertexBuffer(16), {}, "frame 2");
        device->submitAndWait();
        ensure_equals("frame 1 is finished", device->completedFrame(), 1u);
        ensure_equals("frame 1's buffer freed", native.freed.load(), 1);
        ensure_equals("frame 2's buffer waits, more of frame 2 may come",
                      device->mLifecycle.pendingReleases(), 1u);
    }

    template<> template<>
    void object::test<10>()
    {
        set_test_name("a freed object may release others, which wait for their own frame");
        TestGpu gpu;
        FrameLifecycle lifecycle(gpu.waitFunction());
        Chain chain;
        chain.lifecycle = &lifecycle;
        Chain* owner = &chain;
        lifecycle.release([owner]
        {
            ++owner->freed;
            owner->lifecycle->release([owner] { ++owner->freed; });
        });
        lifecycle.frameCompleted(1);
        lifecycle.endFrame(); // recording 2: the first release runs and makes the second
        ensure_equals("first freed", chain.freed, 1);
        ensure_equals("second waits for frame 2", lifecycle.pendingReleases(), 1u);
        lifecycle.frameCompleted(2);
        lifecycle.collect();
        ensure_equals("second freed", chain.freed, 2);
    }

    template<> template<>
    void object::test<11>()
    {
        set_test_name("releaseAll frees everything, also what freeing releases");
        TestGpu gpu;
        FrameLifecycle lifecycle(gpu.waitFunction());
        Chain chain;
        chain.lifecycle = &lifecycle;
        Chain* owner = &chain;
        lifecycle.release([owner]
        {
            ++owner->freed;
            owner->lifecycle->release([owner] { ++owner->freed; });
        });
        lifecycle.release([owner] { ++owner->freed; });
        ensure_equals("three ran", lifecycle.releaseAll(), 3u);
        ensure_equals("all freed", chain.freed, 3);
        ensure_equals("nothing left", lifecycle.pendingReleases(), 0u);
    }

    template<> template<>
    void object::test<12>()
    {
        set_test_name("the destructor frees nothing and reports what is left");
        TestGpu gpu;
        int freed = 0;
        int* count = &freed;
        CaptureLog log(LLError::LEVEL_WARN);
        {
            FrameLifecycle lifecycle(gpu.waitFunction());
            lifecycle.release([count] { ++*count; });
        }
        ensure_equals("not freed by the destructor", freed, 0);
        log.messageWith("releases were never freed");
    }

    template<> template<>
    void object::test<13>()
    {
        set_test_name("releases from several threads, while frames run and the GPU reports, "
                      "are each freed once");
        TestGpu gpu;
        FrameLifecycle lifecycle(gpu.waitFunction());
        std::atomic<int> freed{ 0 };
        std::atomic<int>* count = &freed;
        std::atomic<bool> releasing{ true };

        std::vector<std::thread> workers;
        for (int t = 0; t < 4; ++t)
        {
            workers.emplace_back([&lifecycle, count]
            {
                for (int i = 0; i < 2000; ++i)
                {
                    lifecycle.release([count] { ++*count; });
                }
            });
        }
        // Two GPU threads report finished frames, so the reports also race each other.
        std::vector<std::thread> reporters;
        for (int r = 0; r < 2; ++r)
        {
            reporters.emplace_back([&lifecycle, &releasing]
            {
                while (releasing.load())
                {
                    lifecycle.frameCompleted(lifecycle.currentFrame() - 1);
                }
            });
        }
        // The main thread runs frames meanwhile, and each endFrame() collects. It runs at least 50
        // frames before it waits for the workers, then goes on until everything is freed.
        int frames = 0;
        while (frames < 50 || (freed.load() < 8000 && frames < 100000))
        {
            lifecycle.endFrame();
            ++frames;
            if (frames == 50)
            {
                for (std::thread& worker : workers)
                {
                    worker.join();
                }
            }
        }
        releasing = false;
        for (std::thread& reporter : reporters)
        {
            reporter.join();
        }

        // In case the last frames still hold some: shutdown frees them.
        lifecycle.idle();
        lifecycle.releaseAll();
        ensure_equals("each freed exactly once", freed.load(), 8000);
        ensure_equals("nothing left", lifecycle.pendingReleases(), 0u);
    }

    // Validation and errors (decisions 10 and 11) ---------------------------------------------

    template<> template<>
    void object::test<14>()
    {
        set_test_name("buffer descriptions are checked");
        ensure("valid", validate(vertexBuffer(64), 64).empty());
        ensure("size 0", !validate(vertexBuffer(0), 0).empty());
        BufferDesc noUsage = vertexBuffer(64);
        noUsage.usage = 0;
        ensure("no usage", !validate(noUsage, 0).empty());
        BufferDesc unknownUsage = vertexBuffer(64);
        unknownUsage.usage = 1 << 7;
        ensure("unknown usage", !validate(unknownUsage, 0).empty());
        ensure_contains("initial data too large", validate(vertexBuffer(64), 65), "larger than the buffer");
    }

    template<> template<>
    void object::test<15>()
    {
        set_test_name("texture descriptions are checked against the limits");
        const Capabilities caps = testCapabilities();
        ensure("valid", validate(texture2D(1024, 512), caps).empty());
        ensure("largest", validate(texture2D(16384, 16384), caps).empty());
        // Metal stops the program with an assertion for 16385 (P0-05).
        ensure_contains("too wide", validate(texture2D(16385, 16), caps), "larger than the maximum");
        ensure_contains("too high", validate(texture2D(16, 16385), caps), "larger than the maximum");
        ensure_contains("zero width", validate(texture2D(0, 16), caps), "are 0");
        ensure_contains("zero height", validate(texture2D(16, 0), caps), "are 0");
        TextureDesc zero = texture2D(16, 16);
        zero.layers = 0;
        ensure_contains("zero layers", validate(zero, caps), "are 0");
        zero = texture2D(16, 16);
        zero.mipLevels = 0;
        ensure_contains("zero mips", validate(zero, caps), "are 0");
        zero = texture2D(16, 16);
        zero.samples = 0;
        ensure_contains("zero samples", validate(zero, caps), "are 0");
        TextureDesc usage = texture2D(16, 16);
        usage.usage = 0;
        ensure_contains("no usage", validate(usage, caps), "empty or unknown");
        usage.usage = TEXTURE_SAMPLED | (1 << 6);
        ensure_contains("unknown usage", validate(usage, caps), "empty or unknown");
        ensure("Count is not a format", !validate(texture2D(16, 16, Format::Count), caps).empty());
        ensure("unknown format", !validate(texture2D(16, 16, Format::Unknown), caps).empty());
        ensure_contains("BGRA8", validate(texture2D(16, 16, Format::BGRA8Unorm), caps), "drawables only");

        TextureDesc mips = texture2D(1024, 512);
        mips.mipLevels = 11;
        ensure("full chain", validate(mips, caps).empty());
        mips.mipLevels = 12;
        ensure_contains("too many mips", validate(mips, caps), "more than a full chain");

        TextureDesc cube = texture2D(256, 128);
        cube.type = TextureType::Cube;
        ensure_contains("cube not square", validate(cube, caps), "square");
        cube.height = 256;
        ensure("cube", validate(cube, caps).empty());
        cube.layers = 2;
        ensure_contains("layers on a cube", validate(cube, caps), "only cube map arrays");

        TextureDesc cubeArray = cube;
        cubeArray.type = TextureType::CubeArray;
        cubeArray.layers = 341; // 2046 faces
        ensure("cube array", validate(cubeArray, caps).empty());
        cubeArray.layers = 342; // 2052 faces
        ensure_contains("too many cubes", validate(cubeArray, caps), "array layers");
        Capabilities noArrays = caps;
        noArrays.cubeMapArrays = false;
        cubeArray.layers = 1;
        ensure_contains("no cube arrays", validate(cubeArray, noArrays), "not supported");
    }

    template<> template<>
    void object::test<16>()
    {
        set_test_name("multisampling, compression and swizzle are checked");
        const Capabilities caps = testCapabilities();
        TextureDesc msaa = texture2D(1920, 1080);
        msaa.usage = TEXTURE_RENDER_TARGET;
        msaa.samples = 4;
        ensure("4 samples", validate(msaa, caps).empty());
        msaa.samples = 3;
        ensure_contains("3 samples", validate(msaa, caps), "power of two");
        msaa.samples = 16;
        ensure_contains("16 samples", validate(msaa, caps), "power of two up to 8");
        msaa.samples = 4;
        msaa.mipLevels = 2;
        ensure_contains("samples with mips", validate(msaa, caps), "one mip level");
        msaa.mipLevels = 1;
        msaa.usage = TEXTURE_SAMPLED;
        ensure_contains("samples without a target", validate(msaa, caps), "render targets");

        TextureDesc bc = texture2D(512, 512, Format::BC3_RGBA);
        ensure("BC3", validate(bc, caps).empty());
        bc.usage = TEXTURE_SAMPLED | TEXTURE_RENDER_TARGET;
        ensure_contains("BC3 target", validate(bc, caps), "cannot be render targets");
        Capabilities noBC = caps;
        noBC.bcCompression = false;
        bc.usage = TEXTURE_SAMPLED;
        ensure_contains("no BC", validate(bc, noBC), "not supported");

        TextureDesc luminance = texture2D(64, 64, Format::R8Unorm);
        luminance.swizzle = { Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::One };
        ensure("swizzle", validate(luminance, caps).empty());
        Capabilities noSwizzle = caps;
        noSwizzle.textureSwizzle = false;
        ensure_contains("no swizzle", validate(luminance, noSwizzle), "swizzle is not supported");
    }

    template<> template<>
    void object::test<17>()
    {
        set_test_name("the facade's GL format must match the format");
        const Capabilities caps = testCapabilities();
        TextureDesc depth = texture2D(1024, 1024, Format::Depth32Float);
        depth.usage = TEXTURE_SAMPLED | TEXTURE_RENDER_TARGET;
        depth.glFormat = GL_DEPTH_COMPONENT24;
        ensure("24-bit depth is Depth32Float", validate(depth, caps).empty());
        depth.glFormat = GL_RGBA8;
        ensure_contains("RGBA8 is not depth", validate(depth, caps), "does not match");
    }

    template<> template<>
    void object::test<18>()
    {
        set_test_name("a failed creation returns null and logs the name and the reason");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        CaptureLog log(LLError::LEVEL_WARN);
        Ref<Texture> texture = device->createTexture(texture2D(16385, 16385), "huge snapshot");
        ensure("null", texture.isNull());
        log.messageWith("Could not create texture 'huge snapshot': 16385x16385 is larger");
        Ref<Buffer> buffer = device->createBuffer(vertexBuffer(0), {}, {});
        ensure("null buffer", buffer.isNull());
        log.messageWith("Could not create buffer 'unnamed': the size is 0");
        ensure_equals("nothing native made", native.created.load(), 0);
    }

    // Samplers (decision 4) ----------------------------------------------------------------------

    template<> template<>
    void object::test<19>()
    {
        set_test_name("samplers are cached by description, and a failure is not cached");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        SamplerDesc trilinear;
        trilinear.filter = Filter::Trilinear;
        Ref<Sampler> first = device->getSampler(trilinear);
        Ref<Sampler> second = device->getSampler(trilinear);
        ensure("same sampler", first.get() == second.get());
        ensure_equals("made once", device->mSamplersMade, 1);

        SamplerDesc shadow = trilinear;
        shadow.compare = true;
        ensure("another sampler", device->getSampler(shadow).get() != first.get());
        ensure_equals("two cached", device->mSamplers.size(), 2u);

        device->mSamplersFail = true;
        SamplerDesc clamp;
        clamp.addressU = AddressMode::ClampToEdge;
        ensure("failed", device->getSampler(clamp).isNull());
        ensure_equals("not cached", device->mSamplers.size(), 2u);
        device->mSamplersFail = false;
        Ref<Sampler> made = device->getSampler(clamp);
        ensure("made on the next call", made.notNull());
        ensure_equals("and cached", device->mSamplers.size(), 3u);
        ensure("the cached one", device->getSampler(clamp).get() == made.get());
    }

    template<> template<>
    void object::test<20>()
    {
        set_test_name("every field of a sampler description changes its hash");
        SamplerDescHash hash;
        const SamplerDesc base;
        std::set<size_t> hashes{ hash(base) };
        SamplerDesc changed = base;
        changed.filter = Filter::Anisotropic;
        ensure("filter", hashes.insert(hash(changed)).second);
        changed = base;
        changed.addressU = AddressMode::MirroredRepeat;
        ensure("address U", hashes.insert(hash(changed)).second);
        changed = base;
        changed.addressV = AddressMode::MirroredRepeat;
        ensure("address V", hashes.insert(hash(changed)).second);
        changed = base;
        changed.addressW = AddressMode::MirroredRepeat;
        ensure("address W", hashes.insert(hash(changed)).second);
        changed = base;
        changed.compare = true;
        ensure("compare", hashes.insert(hash(changed)).second);
        changed = base;
        changed.compareFunc = CompareFunc::Greater;
        ensure("compare function", hashes.insert(hash(changed)).second);
    }

    // The GL table (decision 1) ------------------------------------------------------------------

    template<> template<>
    void object::test<21>()
    {
        set_test_name("every format has a GL format and comes back from it");
        for (U16 value = U16(Format::Unknown) + 1; value < U16(Format::Count); ++value)
        {
            const Format format = Format(value);
            const U32 gl = toGLInternalFormat(format);
            if (format == Format::BGRA8Unorm)
            {
                ensure_equals("BGRA8 has no GL format", gl, 0u);
                continue;
            }
            ensure("format " + std::to_string(value) + " has a GL format", gl != 0);
            ensure("format " + std::to_string(value) + " comes back", formatFromGL(gl) == format);
        }
        ensure("Unknown has none", toGLInternalFormat(Format::Unknown) == 0);
        // LLRenderTarget's deferred normals with HDR (P2-03a)
        ensure("RGBA16", formatFromGL(GL_RGBA16) == Format::RGBA16Unorm);
        ensure("RGBA16's GL format", toGLInternalFormat(Format::RGBA16Unorm) == GL_RGBA16);
    }

    template<> template<>
    void object::test<22>()
    {
        set_test_name("GL formats without a format of their own are widened");
        ensure("RGB8", formatFromGL(GL_RGB8) == Format::RGBA8Unorm);
        ensure("RGB", formatFromGL(GL_RGB) == Format::RGBA8Unorm);
        ensure("RGBA", formatFromGL(GL_RGBA) == Format::RGBA8Unorm);
        ensure("SRGB_ALPHA", formatFromGL(GL_SRGB_ALPHA) == Format::RGBA8Unorm_sRGB);
        ensure("RGB16F", formatFromGL(GL_RGB16F) == Format::RGBA16Float);
        ensure("DEPTH_COMPONENT24", formatFromGL(GL_DEPTH_COMPONENT24) == Format::Depth32Float);
        ensure("DEPTH24_STENCIL8", formatFromGL(GL_DEPTH24_STENCIL8) == Format::Depth32Float_Stencil8);
        ensure("DEPTH_COMPONENT32", formatFromGL(GL_DEPTH_COMPONENT32) == Format::Depth32Float);
        ensure("RGB8's sized format", toGLInternalFormat(formatFromGL(GL_RGB8)) == GL_RGBA8);
        // GL's deprecated alpha and luminance formats and its generic compressed formats (P2-02a)
        ensure("ALPHA", formatFromGL(GL_ALPHA) == Format::R8Unorm);
        ensure("ALPHA8", formatFromGL(GL_ALPHA8) == Format::R8Unorm);
        ensure("LUMINANCE", formatFromGL(GL_LUMINANCE) == Format::R8Unorm);
        ensure("LUMINANCE8", formatFromGL(GL_LUMINANCE8) == Format::R8Unorm);
        ensure("LUMINANCE_ALPHA", formatFromGL(GL_LUMINANCE_ALPHA) == Format::RG8Unorm);
        ensure("LUMINANCE8_ALPHA8", formatFromGL(GL_LUMINANCE8_ALPHA8) == Format::RG8Unorm);
        ensure("COMPRESSED_RED", formatFromGL(GL_COMPRESSED_RED) == Format::R8Unorm);
        ensure("COMPRESSED_RG", formatFromGL(GL_COMPRESSED_RG) == Format::RG8Unorm);
        ensure("COMPRESSED_RGB", formatFromGL(GL_COMPRESSED_RGB) == Format::RGBA8Unorm);
        ensure("COMPRESSED_RGBA", formatFromGL(GL_COMPRESSED_RGBA) == Format::RGBA8Unorm);
        ensure("COMPRESSED_SRGB", formatFromGL(GL_COMPRESSED_SRGB) == Format::RGBA8Unorm_sRGB);
        ensure("COMPRESSED_SRGB_ALPHA", formatFromGL(GL_COMPRESSED_SRGB_ALPHA) == Format::RGBA8Unorm_sRGB);
        ensure("COMPRESSED_ALPHA", formatFromGL(GL_COMPRESSED_ALPHA) == Format::R8Unorm);
        ensure("COMPRESSED_LUMINANCE", formatFromGL(GL_COMPRESSED_LUMINANCE) == Format::R8Unorm);
        ensure("COMPRESSED_LUMINANCE_ALPHA", formatFromGL(GL_COMPRESSED_LUMINANCE_ALPHA) == Format::RG8Unorm);
        ensure("DXT3 has no format", formatFromGL(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT) == Format::Unknown);
        ensure("unknown", formatFromGL(GL_RGBA4) == Format::Unknown);
        ensure("0", formatFromGL(0) == Format::Unknown);
    }

    template<> template<>
    void object::test<23>()
    {
        set_test_name("pixel layouts go both ways with GL's format and type");
        const U32 formats[] = { GL_RED, GL_RG, GL_RGB, GL_RGBA, GL_BGRA, GL_DEPTH_COMPONENT };
        const U32 types[] = { GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT, GL_HALF_FLOAT, GL_FLOAT };
        for (U32 format : formats)
        {
            for (U32 type : types)
            {
                std::optional<PixelLayout> layout = pixelLayoutFromGL(format, type);
                ensure("known pair", layout.has_value());
                ensure_equals("format back", toGLPixelFormat(*layout), format);
                ensure_equals("type back", toGLPixelType(*layout), type);
            }
        }

        std::optional<PixelLayout> media = pixelLayoutFromGL(GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, true, 640);
        ensure("GStreamer's layout", media.has_value());
        ensure("swapped", media->swapBytes);
        ensure_equals("row length", media->rowLength, 640u);
        ensure_equals("packed type back", toGLPixelType(*media), U32(GL_UNSIGNED_INT_8_8_8_8_REV));
        ensure("packed type needs four components", !pixelLayoutFromGL(GL_RGB, GL_UNSIGNED_INT_8_8_8_8_REV));
        ensure("unknown format", !pixelLayoutFromGL(GL_STENCIL_INDEX, GL_UNSIGNED_BYTE));
        ensure("unknown type", !pixelLayoutFromGL(GL_RGBA, GL_BYTE));
    }

    template<> template<>
    void object::test<24>()
    {
        set_test_name("bytes per pixel, mip chains and format classes");
        ensure_equals("RGBA8", pixelLayoutFromGL(GL_RGBA, GL_UNSIGNED_BYTE).value().bytesPerPixel(), 4u);
        ensure_equals("RGB8", pixelLayoutFromGL(GL_RGB, GL_UNSIGNED_BYTE).value().bytesPerPixel(), 3u);
        ensure_equals("RG16F", pixelLayoutFromGL(GL_RG, GL_HALF_FLOAT).value().bytesPerPixel(), 4u);
        ensure_equals("RGBA32F", pixelLayoutFromGL(GL_RGBA, GL_FLOAT).value().bytesPerPixel(), 16u);
        ensure_equals("depth float",
                      pixelLayoutFromGL(GL_DEPTH_COMPONENT, GL_FLOAT).value().bytesPerPixel(), 4u);
        ensure_equals("packed",
                      pixelLayoutFromGL(GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV).value().bytesPerPixel(), 4u);

        ensure_equals("1x1", fullMipCount(1, 1), 1u);
        ensure_equals("2x1", fullMipCount(2, 1), 2u);
        ensure_equals("1024x512", fullMipCount(1024, 512), 11u);
        ensure_equals("1000x3", fullMipCount(1000, 3), 10u);
        ensure_equals("16384", fullMipCount(16384, 16384), 15u);
        ensure_equals("0x0", fullMipCount(0, 0), 0u);

        ensure("BC7 compressed", isCompressed(Format::BC7_RGBA_sRGB));
        ensure("RGBA8 not compressed", !isCompressed(Format::RGBA8Unorm));
        ensure("depth", isDepth(Format::Depth32Float) && isDepth(Format::Depth32Float_Stencil8));
        ensure("R32F not depth", !isDepth(Format::R32Float));
        ensure("stencil", hasStencil(Format::Depth32Float_Stencil8) && !hasStencil(Format::Depth32Float));
    }

    // Shutdown (decision 2) ----------------------------------------------------------------------

    template<> template<>
    void object::test<25>()
    {
        set_test_name("shutting a device down frees every native object, cached or still in use");
        TestGpu gpu;
        NativeCounter native;
        {
            Ref<TestDevice> device = new TestDevice(gpu, native);
            Ref<Buffer> kept = device->createBuffer(vertexBuffer(256), {}, "kept to the end");
            device->createTexture(texture2D(64, 64), "released at once");
            SamplerDesc sampling;
            device->getSampler(sampling);
            sampling.filter = Filter::Point;
            device->getSampler(sampling);
            Ref<TestFence> neverSignalled = new TestFence;
            Ref<Texture> uploading = device->createTexture(texture2D(128, 128), "uploading");
            static_cast<TestTexture*>(uploading.get())->uploadedWith(neverSignalled);
            uploading = nullptr;
            device->endFrame();
            kept = nullptr;
            ensure("made five", native.created.load() == 5);
            ensure("none freed before shutdown", native.freed.load() == 0);
        }
        // The GPU is idle at shutdown, so even an upload that never reported is done.
        ensure_equals("all freed", native.freed.load(), 5);
    }

    template<> template<>
    void object::test<26>()
    {
        set_test_name("a null upload fence does not hold deletion back");
        TestGpu gpu;
        FrameLifecycle lifecycle(gpu.waitFunction());
        int freed = 0;
        int* count = &freed;
        lifecycle.release([count] { ++*count; }, { Ref<Fence>() });
        lifecycle.frameCompleted(1);
        lifecycle.endFrame();
        ensure_equals("freed", freed, 1);
    }

    // Draw state, programs and pipelines (decisions 4, 6 and 8) ---------------------------------

    template<> template<>
    void object::test<27>()
    {
        set_test_name("primitive classes and index sizes");
        ensure("points", primitiveClass(PrimitiveType::Points) == PrimitiveClass::Point);
        ensure("lines", primitiveClass(PrimitiveType::Lines) == PrimitiveClass::Line);
        ensure("line strip", primitiveClass(PrimitiveType::LineStrip) == PrimitiveClass::Line);
        ensure("line loop", primitiveClass(PrimitiveType::LineLoop) == PrimitiveClass::Line);
        ensure("triangles", primitiveClass(PrimitiveType::Triangles) == PrimitiveClass::Triangle);
        ensure("strip", primitiveClass(PrimitiveType::TriangleStrip) == PrimitiveClass::Triangle);
        ensure("fan", primitiveClass(PrimitiveType::TriangleFan) == PrimitiveClass::Triangle);
        ensure_equals("U16", indexSize(IndexType::U16), 2u);
        ensure_equals("U32", indexSize(IndexType::U32), 4u);
    }

    template<> template<>
    void object::test<28>()
    {
        set_test_name("uniform sizes are tightly packed, matrices column-major");
        ensure_equals("float", uniformSize(UniformType::Float), 4u);
        ensure_equals("vec3", uniformSize(UniformType::Vec3), 12u);
        ensure_equals("ivec4", uniformSize(UniformType::IVec4), 16u);
        ensure_equals("mat2", uniformSize(UniformType::Mat2), 16u);
        ensure_equals("mat3", uniformSize(UniformType::Mat3), 36u);
        ensure_equals("mat3x4", uniformSize(UniformType::Mat3x4), 48u);
        ensure_equals("mat4", uniformSize(UniformType::Mat4), 64u);
        ensure_equals("sampler channel", uniformSize(UniformType::SamplerCube), 4u);
        ensure("shadow sampler", isSampler(UniformType::Sampler2DShadow));
        ensure("3D sampler", isSampler(UniformType::Sampler3D));
        ensure_equals("3D sampler channel", uniformSize(UniformType::Sampler3D), 4u);
        ensure("mat4 is not", !isSampler(UniformType::Mat4));
    }

    template<> template<>
    void object::test<29>()
    {
        set_test_name("uniform values are checked against the reflection");
        ProgramReflection reflection;
        reflection.uniforms = { { "modelview_matrix", UniformType::Mat4, 1 },
                                { "light_position", UniformType::Vec4, 8 },
                                { "diffuseMap", UniformType::Sampler2D, 1 } };
        ensure("matrix", validateUniform(reflection, 0, 64, 1).empty());
        ensure("eight lights", validateUniform(reflection, 1, 128, 8).empty());
        ensure("three lights", validateUniform(reflection, 1, 48, 3).empty());
        ensure_contains("nine lights", validateUniform(reflection, 1, 144, 9), "which has 8");
        ensure_contains("zero", validateUniform(reflection, 1, 0, 0), "which has 8");
        ensure_contains("wrong size", validateUniform(reflection, 0, 48, 1), "which takes 64");
        ensure_contains("sampler", validateUniform(reflection, 2, 4, 1), "set its channel");
        ensure_contains("no such uniform", validateUniform(reflection, 3, 4, 1), "does not exist");
    }

    template<> template<>
    void object::test<30>()
    {
        set_test_name("program descriptions are checked");
        const Capabilities caps = testCapabilities();
        ensure("valid", validate(program("objectSimpleProgram"), caps).empty());
        ProgramDesc noFragment = program("broken");
        noFragment.stages.pop_back();
        ensure_contains("no fragment stage", validate(noFragment, caps), "vertex and a fragment");
        ProgramDesc twice = program("broken");
        twice.stages.push_back(twice.stages[0]);
        ensure_contains("vertex twice", validate(twice, caps), "twice");
        ProgramDesc empty = program("broken");
        empty.stages[1].units.clear();
        ensure_contains("no source", validate(empty, caps), "no source");
        ProgramDesc geometry = program("normalDebugProgram");
        geometry.stages.push_back({ ShaderStage::Geometry, { "void main() {}" } });
        ensure_contains("geometry on Metal", validate(geometry, caps), "geometry shaders");
        Capabilities gl = caps;
        gl.geometryShaders = true;
        ensure("geometry on GL", validate(geometry, gl).empty());
        ensure_contains("unknown attribute", validate(program("broken", 1u << 14), caps), "unknown attributes");
    }

    template<> template<>
    void object::test<31>()
    {
        set_test_name("pipeline descriptions are checked");
        const Capabilities caps = testCapabilities();
        ensure("valid", validate(colorPipeline(), caps).empty());

        PipelineDesc shadow;
        shadow.colorCount = 0;
        shadow.depthFormat = Format::Depth32Float;
        ensure("depth only", validate(shadow, caps).empty());
        shadow.depthFormat = Format::Unknown;
        ensure_contains("nothing", validate(shadow, caps), "no attachments");

        PipelineDesc gbuffer = colorPipeline();
        gbuffer.colorCount = 4;
        gbuffer.colorFormats = { Format::RGBA8Unorm, Format::RGBA8Unorm, Format::RGB10A2Unorm,
                                 Format::RGBA16Float };
        ensure("G-buffer", validate(gbuffer, caps).empty());
        gbuffer.colorCount = 3;
        ensure_contains("format past the count", validate(gbuffer, caps), "not used but has a format");
        gbuffer.colorCount = 5;
        ensure_contains("missing format", validate(gbuffer, caps), "has no format");
        gbuffer.colorCount = 9;
        ensure_contains("too many", validate(gbuffer, caps), "more than the maximum 8");

        ensure_contains("depth as color", validate(colorPipeline(Format::Depth32Float), caps), "depth or compressed");
        ensure_contains("BC as color", validate(colorPipeline(Format::BC7_RGBA), caps), "depth or compressed");
        ensure("drawable", validate(colorPipeline(Format::BGRA8Unorm), caps).empty());

        PipelineDesc depth = colorPipeline();
        depth.depthFormat = Format::R32Float;
        ensure_contains("depth not depth", validate(depth, caps), "not a depth format");

        PipelineDesc msaa = colorPipeline();
        msaa.samples = 4;
        ensure("4 samples", validate(msaa, caps).empty());
        msaa.samples = 6;
        ensure_contains("6 samples", validate(msaa, caps), "power of two");
        msaa.samples = 0;
        ensure_contains("0 samples", validate(msaa, caps), "power of two");

        PipelineDesc mask = colorPipeline();
        mask.colorMask = 0x10;
        ensure_contains("mask", validate(mask, caps), "unknown bits");
        mask = colorPipeline();
        mask.attributeMask = 1u << 14;
        ensure_contains("attributes", validate(mask, caps), "unknown attributes");
    }

    template<> template<>
    void object::test<32>()
    {
        set_test_name("pipeline keys hash every field");
        PipelineKeyHash hash;
        const PipelineKey base{ 7, colorPipeline() };
        ensure("equal keys hash alike", hash(base) == hash(PipelineKey{ 7, colorPipeline() }));
        std::set<size_t> hashes{ hash(base) };
        auto differs = [&](const char* what, PipelineKey key)
        {
            ensure(std::string(what) + " differs", !(key == base));
            ensure(std::string(what) + " hashes differently", hashes.insert(hash(key)).second);
        };
        PipelineKey key = base;
        key.programId = 8;
        differs("program", key);
        key = base;
        key.desc.attributeMask = 3;
        differs("attributes", key);
        key = base;
        key.desc.primitiveClass = PrimitiveClass::Point;
        differs("primitive class", key);
        key = base;
        key.desc.samples = 4;
        differs("samples", key);
        key = base;
        key.desc.colorFormats[0] = Format::RGBA16Float;
        differs("color format", key);
        key = base;
        key.desc.depthFormat = Format::Depth32Float_Stencil8;
        differs("depth format", key);
        key = base;
        key.desc.blend.enabled = true;
        differs("blend on", key);
        key = base;
        key.desc.blend.dstColor = BlendFactor::OneMinusSourceAlpha;
        differs("blend factor", key);
        key = base;
        key.desc.blend.alphaOp = BlendOp::Max;
        differs("blend op", key);
        key = base;
        key.desc.colorMask = COLOR_MASK_R;
        differs("color mask", key);
        key = base;
        key.desc.colorCount = 2;
        key.desc.colorFormats[1] = Format::RGBA8Unorm;
        differs("color count", key);
        for (U32 i = 1; i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
        {
            key = base;
            key.desc.colorFormats[i] = Format::R8Unorm;
            differs(("color format " + std::to_string(i)).c_str(), key);
        }
        PipelineKey blended = base;
        blended.desc.blend.enabled = true;
        auto blendDiffers = [&](const char* what, PipelineKey changed)
        {
            ensure(std::string(what) + " differs", !(changed == blended));
            ensure(std::string(what) + " hashes differently", hash(changed) != hash(blended));
        };
        key = blended;
        key.desc.blend.srcColor = BlendFactor::SourceAlpha;
        blendDiffers("source color", key);
        key = blended;
        key.desc.blend.srcAlpha = BlendFactor::Zero;
        blendDiffers("source alpha", key);
        key = blended;
        key.desc.blend.dstAlpha = BlendFactor::One;
        blendDiffers("destination alpha", key);
        key = blended;
        key.desc.blend.colorOp = BlendOp::ReverseSubtract;
        blendDiffers("color op", key);

        DepthStencilDescHash depthHash;
        DepthStencilDesc test{ true, true, CompareFunc::LessEqual };
        DepthStencilDesc read{ true, false, CompareFunc::LessEqual };
        DepthStencilDesc greater{ true, true, CompareFunc::Greater };
        ensure("depth write", depthHash(test) != depthHash(read));
        ensure("depth func", depthHash(test) != depthHash(greater));
    }

    template<> template<>
    void object::test<33>()
    {
        set_test_name("pipelines are cached per program and description");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Program> simple = device->createProgram(program("objectSimpleProgram"));
        Ref<Program> alpha = device->createProgram(program("objectAlphaProgram"));
        ensure("programs made", simple.notNull() && alpha.notNull());

        Ref<Pipeline> first = device->getPipeline(*simple, colorPipeline());
        Ref<Pipeline> again = device->getPipeline(*simple, colorPipeline());
        ensure("same pipeline", first.get() == again.get());
        ensure_equals("holds the program's id", first->programId(), simple->id());
        ensure("other format", device->getPipeline(*simple, colorPipeline(Format::RGBA16Float)).get() != first.get());
        ensure("other program", device->getPipeline(*alpha, colorPipeline()).get() != first.get());
        ensure_equals("three made", device->mPipelinesMade.load(), 3);
        ensure_equals("three cached", device->mPipelines.size(), 3u);

        CaptureLog log(LLError::LEVEL_WARN);
        PipelineDesc broken = colorPipeline();
        broken.colorCount = 0;
        broken.depthFormat = Format::Unknown;
        ensure("invalid", device->getPipeline(*simple, broken).isNull());
        log.messageWith("Could not create pipeline 'objectSimpleProgram': the pipeline has no attachments");
        ensure_equals("not cached", device->mPipelines.size(), 3u);
    }

    template<> template<>
    void object::test<34>()
    {
        set_test_name("a program's pipelines go when the program dies, and wait for their frame");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Program> simple = device->createProgram(program("objectSimpleProgram"));
        Ref<Program> alpha = device->createProgram(program("objectAlphaProgram"));
        device->getPipeline(*simple, colorPipeline());
        device->getPipeline(*simple, colorPipeline(Format::RGBA16Float));
        Ref<Pipeline> held = device->getPipeline(*simple, colorPipeline(Format::RG16Float));
        device->getPipeline(*alpha, colorPipeline());
        ensure_equals("four cached", device->mPipelines.size(), 4u);
        const int made = native.created.load(); // 2 programs, 4 pipelines

        simple = nullptr; // the facade let go, as when shaders are reloaded
        ensure_equals("only the other program's pipeline is left", device->mPipelines.size(), 1u);
        ensure_equals("nothing freed during the frame", native.freed.load(), 0);
        device->mLifecycle.frameCompleted(1);
        device->endFrame();
        ensure_equals("the program and two pipelines freed", native.freed.load(), 3);
        ensure("a held pipeline still works", held->status() == CompileStatus::Ready);
        held = nullptr;
        device->endFrame();
        device->mLifecycle.frameCompleted(2);
        device->mLifecycle.collect();
        ensure_equals("the held one too", native.freed.load(), 4);
        ensure_equals("made", made, 6);
    }

    template<> template<>
    void object::test<35>()
    {
        set_test_name("threads asking for the same pipeline get one cached pipeline");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Program> simple = device->createProgram(program("objectSimpleProgram"));
        std::vector<Pipeline*> got(8, nullptr);
        std::vector<std::thread> threads;
        for (size_t t = 0; t < got.size(); ++t)
        {
            threads.emplace_back([&device, &simple, &got, t]
            {
                got[t] = device->getPipeline(*simple, colorPipeline()).get();
            });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        ensure_equals("one cached", device->mPipelines.size(), 1u);
        for (Pipeline* pipeline : got)
        {
            ensure("all got the cached one", pipeline == got[0]);
        }
        ensure("made at least once", device->mPipelinesMade.load() >= 1);
    }

    template<> template<>
    void object::test<36>()
    {
        set_test_name("depth-stencil states are cached, and programs report their status");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        DepthStencilDesc test{ true, true, CompareFunc::LessEqual };
        Ref<DepthStencilState> first = device->getDepthStencilState(test);
        ensure("same state", device->getDepthStencilState(test).get() == first.get());
        DepthStencilDesc read{ true, false, CompareFunc::LessEqual };
        ensure("another state", device->getDepthStencilState(read).get() != first.get());

        Ref<Program> simple = device->createProgram(program("objectSimpleProgram"));
        ensure("pending at first", simple->status() == CompileStatus::Pending);
        simple->wait();
        ensure("ready after the wait", simple->status() == CompileStatus::Ready);
        ensure_equals("named after the program", simple->debugName(), std::string("objectSimpleProgram"));
        CaptureLog log(LLError::LEVEL_WARN);
        ProgramDesc broken = program("brokenProgram");
        broken.stages.clear();
        ensure("invalid program", device->createProgram(broken).isNull());
        log.messageWith("Could not create program 'brokenProgram'");
    }

    // The viewer's device (P2-10) ----------------------------------------------------------------
    // The tests hold the device only as the pointer createViewerDevice() gives, so the accessor's
    // reference is the only one.

    template<> template<>
    void object::test<37>()
    {
        set_test_name("the viewer's device");
        TestGpu gpu;
        NativeCounter native;
        ViewerDeviceGuard guard;
        ensure("no device at first", viewerDevice() == nullptr);
        ensure("GL is the backend", viewerBackend() == BackendType::GL);
        ensure("a failed creation gives null", createViewerDevice(DeviceCreateInfo()) == nullptr);
        ensure("and keeps nothing", viewerDevice() == nullptr);

        sNextGLDevice = new TestDevice(gpu, native);
        Device* device = createViewerDevice(DeviceCreateInfo());
        ensure("created", device != nullptr);
        ensure("the accessor gives it", viewerDevice() == device);
        {
            CaptureLog log(LLError::LEVEL_WARN);
            ensure("a second creation gives the same device", createViewerDevice(DeviceCreateInfo()) == device);
            log.messageWith("The viewer's render device exists already");
        }

        // The release is seen through what the test owns: a texture counted in its NativeCounter,
        // whose native object the device frees when it goes.
        ensure("a texture", device->createTexture(texture2D(4, 4), "probe").notNull());
        ensure_equals("made", native.created.load(), 1);
        ensure_equals("not freed while the device lives", native.freed.load(), 0);
        ensure_equals("the accessor holds the only reference", viewerDevice()->getNumRefs(), 1);
        // A release hook runs before the device goes, while the accessor still gives it.
        static int hookRuns = 0;
        static bool hookSawDevice = false;
        hookRuns = 0;
        onViewerDeviceRelease([]()
        {
            ++hookRuns;
            hookSawDevice = viewerDevice() != nullptr;
        });
        releaseViewerDevice();
        ensure("no device after the release", viewerDevice() == nullptr);
        ensure_equals("the device went and freed the texture", native.freed.load(), 1);
        ensure_equals("the hook ran once", hookRuns, 1);
        ensure("the hook ran while the device was there", hookSawDevice);
        releaseViewerDevice();
        ensure_equals("no hook without a device", hookRuns, 1);
    }

    template<> template<>
    void object::test<38>()
    {
        set_test_name("one frame per ViewerFrame");
        TestGpu gpu;
        NativeCounter native;
        ViewerDeviceGuard guard;
        Device* device = nullptr;
        {
            ViewerFrame without; // no device: nothing begins
            sNextGLDevice = new TestDevice(gpu, native);
            device = createViewerDevice(DeviceCreateInfo());
            ensure("created", device != nullptr);
        }
        ensure_equals("a frame begun without a device ends nothing", device->frameNumber(), U64(1));
        for (U64 frame = 1; frame <= 5; ++frame)
        {
            ViewerFrame viewer_frame;
            ensure_equals("the frame being recorded", device->frameNumber(), frame);
        }
        ensure_equals("five frames later", device->frameNumber(), U64(6));
    }

    template<> template<>
    void object::test<39>()
    {
        set_test_name("the frame end lets go of the tracker's bindings and frees what earlier frames released");
        TestGpu gpu;
        NativeCounter native;
        ViewerDeviceGuard guard;
        sNextGLDevice = new TestDevice(gpu, native);
        TestDevice* device = static_cast<TestDevice*>(createViewerDevice(DeviceCreateInfo()));
        ensure("created", device != nullptr);
        {
            ViewerFrame frame;
            Ref<Texture> texture = device->createTexture(texture2D(4, 4), "bound");
            device->encoder().setTexture(0, texture.get(), {});
            texture = nullptr; // the facade let go
            ensure_equals("the tracker holds it", device->mEncoder.tracker.heldReferences(), 1u);
        }
        ensure_equals("let go at the frame end", device->mEncoder.tracker.heldReferences(), 0u);
        ensure_equals("released in frame 1", device->mLifecycle.pendingReleases(), 1u);
        ensure_equals("kept until the GPU has finished frame 1", native.freed.load(), 0);
        device->mLifecycle.frameCompleted(1);
        {
            ViewerFrame frame;
        }
        ensure_equals("freed at the next frame end", native.freed.load(), 1);
        ensure("no encoder errors", device->mEncoder.errors.empty());
    }

    template<> template<>
    void object::test<40>()
    {
        set_test_name("a frame ends only on its own device, and the backend changes only without a device");
        TestGpu gpu;
        NativeCounter native;
        ViewerDeviceGuard guard;
        CaptureLog log(LLError::LEVEL_WARN);
        sNextGLDevice = new TestDevice(gpu, native);
        Device* first = createViewerDevice(DeviceCreateInfo());
        ensure("first device", first != nullptr);
        const U64 firstId = first->id();
        Device* second = nullptr;
        {
            ViewerFrame frame;
            releaseViewerDevice(); // the first device is gone from here on
            sNextGLDevice = new TestDevice(gpu, native);
            second = createViewerDevice(DeviceCreateInfo());
            ensure("second device", second != nullptr && second->id() != firstId);
        }
        ensure_equals("the frame did not end on the new device", second->frameNumber(), U64(1));
        releaseViewerDevice();

        setViewerBackend(BackendType::Metal);
        ensure("Metal", viewerBackend() == BackendType::Metal);
        ensure("a GL device is refused while the backend is Metal", createViewerDevice(DeviceCreateInfo()) == nullptr);
        log.messageWith("asked for GL, but the viewer's backend is Metal");
        ensure("and nothing kept", viewerDevice() == nullptr);
        setViewerBackend(BackendType::GL);
        ensure("GL again", viewerBackend() == BackendType::GL);

        sNextGLDevice = new TestDevice(gpu, native);
        ensure("a device", createViewerDevice(DeviceCreateInfo()) != nullptr);
        setViewerBackend(BackendType::Metal);
        log.messageWith("cannot change while its render device exists");
        ensure("still GL", viewerBackend() == BackendType::GL);
    }

    template<> template<>
    void object::test<41>()
    {
        set_test_name("GL's alpha and luminance formats are R and RG layouts, and an upload takes GL's bytes at alignment 1");
        const std::optional<PixelLayout> alpha = pixelLayoutFromGL(GL_ALPHA, GL_UNSIGNED_BYTE);
        ensure("GL_ALPHA", alpha.has_value() && alpha->components == PixelLayout::Components::R);
        ensure_equals("GL_ALPHA: one byte", alpha->bytesPerPixel(), 1u);
        const std::optional<PixelLayout> luminance = pixelLayoutFromGL(GL_LUMINANCE, GL_UNSIGNED_BYTE);
        ensure("GL_LUMINANCE", luminance.has_value() && luminance->components == PixelLayout::Components::R);
        ensure_equals("GL_LUMINANCE: one byte", luminance->bytesPerPixel(), 1u);
        const std::optional<PixelLayout> luminanceAlpha = pixelLayoutFromGL(GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE);
        ensure("GL_LUMINANCE_ALPHA", luminanceAlpha.has_value() && luminanceAlpha->components == PixelLayout::Components::RG);
        ensure_equals("GL_LUMINANCE_ALPHA: two bytes", luminanceAlpha->bytesPerPixel(), 2u);

        // A 2x4 RGB8 region in rows of 6 bytes, as LLImageGL describes RGB8 (level 1's texture_rgb8): at alignment 1
        // GL reads (2 * (4 - 1) + 2) * 3 = 24 bytes, with no padding at the rows' ends.
        TextureDesc desc = texture2D(2, 4);
        desc.glFormat = GL_RGB8;
        const PixelLayout rgb = pixelLayoutFromGL(GL_RGB, GL_UNSIGNED_BYTE).value();
        ensure_equals("24 tight bytes", validateUpload(desc, 0, 0, { 0, 0, 2, 4 }, rgb, 24), std::string());
        ensure("23 refused", !validateUpload(desc, 0, 0, { 0, 0, 2, 4 }, rgb, 23).empty());
    }

    template<> template<>
    void object::test<42>()
    {
        set_test_name("golden level 2's test fault takes the named program's creations, times in a row per shader load");
        struct Off
        {
            ~Off() { setProgramFault({}); } // also when an ensure throws
        } off;

        // Off by default and when the name is empty: nothing is taken, and deferred is left alone.
        bool deferred = false;
        setProgramFault({});
        ensure("off: not taken", !takeProgramFault("faulty", deferred));
        ensure("off: an empty name is not taken", !takeProgramFault("", deferred));
        ensure_equals("off: no hits", programFaultHits(), 0u);
        setProgramFault({ "", true, 0 });
        ensure("an empty program is off", !takeProgramFault("", deferred) && !takeProgramFault("faulty", deferred));
        ensure("deferred untouched", !deferred);
        ensure_equals("an empty program: no hits", programFaultHits(), 0u);

        // times 2: two creations in a row are taken, the third builds and starts the count again, and the next load's
        // first two are taken again. Another name neither is taken nor touches the count, and deferred comes back.
        setProgramFault({ "faulty", false, 2 });
        deferred = true;
        ensure("times 2: the first is taken", takeProgramFault("faulty", deferred));
        ensure("inline", !deferred);
        ensure("another name is not taken", !takeProgramFault("other", deferred));
        ensure("nor a name it begins", !takeProgramFault("faulty #2", deferred) && !takeProgramFault("fault", deferred));
        ensure("times 2: the second is taken", takeProgramFault("faulty", deferred));
        ensure_equals("times 2: two hits", programFaultHits(), 2u);
        ensure("times 2: the third builds", !takeProgramFault("faulty", deferred));
        ensure_equals("times 2: still two hits", programFaultHits(), 2u);
        ensure("the next load: the first is taken again", takeProgramFault("faulty", deferred));
        ensure("the next load: the second too", takeProgramFault("faulty", deferred));
        ensure("the next load: the third builds", !takeProgramFault("faulty", deferred));
        ensure_equals("times 2 over two loads: four hits", programFaultHits(), 4u);

        // times 0: every creation is taken; deferred comes back through the out-parameter.
        setProgramFault({ "faulty", true, 0 });
        ensure_equals("armed again: the hits start over", programFaultHits(), 0u);
        for (U32 i = 1; i <= 5; ++i)
        {
            deferred = false;
            ensure("times 0: creation " + std::to_string(i) + " is taken", takeProgramFault("faulty", deferred));
            ensure("times 0: creation " + std::to_string(i) + " is deferred", deferred);
        }
        ensure_equals("times 0: five hits", programFaultHits(), 5u);

        // Arming again resets the count in a row as well as the hits.
        setProgramFault({ "faulty", false, 1 });
        ensure("times 1: the first is taken", takeProgramFault("faulty", deferred));
        setProgramFault({ "faulty", false, 1 });
        ensure_equals("armed again: no hits", programFaultHits(), 0u);
        ensure("armed again: the first is taken, not counted as the second", takeProgramFault("faulty", deferred));
        ensure("times 1: the second builds", !takeProgramFault("faulty", deferred));
        ensure_equals("times 1: one hit", programFaultHits(), 1u);

        // Off again.
        setProgramFault({});
        ensure("off again: not taken", !takeProgramFault("faulty", deferred));
        ensure_equals("off again: no hits", programFaultHits(), 0u);
    }

    template<> template<>
    void object::test<43>()
    {
        set_test_name("uniformTypeFromGL and toGLUniformType: the GL backend's uniform types, folded as it reflects them, "
                      "and each type's GL enum back (P2-04b)");
        const std::pair<U32, UniformType> table[] = {
            { GL_FLOAT, UniformType::Float },
            { GL_FLOAT_VEC2, UniformType::Vec2 },
            { GL_FLOAT_VEC3, UniformType::Vec3 },
            { GL_FLOAT_VEC4, UniformType::Vec4 },
            { GL_INT, UniformType::Int },
            { GL_BOOL, UniformType::Int },
            { GL_UNSIGNED_INT, UniformType::Int },
            { GL_INT_VEC2, UniformType::IVec2 },
            { GL_BOOL_VEC2, UniformType::IVec2 },
            { GL_UNSIGNED_INT_VEC2, UniformType::IVec2 },
            { GL_INT_VEC3, UniformType::IVec3 },
            { GL_BOOL_VEC3, UniformType::IVec3 },
            { GL_UNSIGNED_INT_VEC3, UniformType::IVec3 },
            { GL_INT_VEC4, UniformType::IVec4 },
            { GL_BOOL_VEC4, UniformType::IVec4 },
            { GL_UNSIGNED_INT_VEC4, UniformType::IVec4 },
            { GL_FLOAT_MAT2, UniformType::Mat2 },
            { GL_FLOAT_MAT3, UniformType::Mat3 },
            { GL_FLOAT_MAT3x4, UniformType::Mat3x4 },
            { GL_FLOAT_MAT4, UniformType::Mat4 },
            { GL_SAMPLER_2D, UniformType::Sampler2D },
            { GL_SAMPLER_2D_RECT, UniformType::Sampler2D },
            { GL_SAMPLER_2D_MULTISAMPLE, UniformType::Sampler2D },
            { GL_SAMPLER_2D_ARRAY, UniformType::Sampler2D },
            { GL_SAMPLER_3D, UniformType::Sampler2D },
            { GL_SAMPLER_2D_SHADOW, UniformType::Sampler2DShadow },
            { GL_SAMPLER_CUBE, UniformType::SamplerCube },
            { GL_SAMPLER_CUBE_MAP_ARRAY, UniformType::SamplerCubeArray },
        };
        for (const auto& [gl, type] : table)
        {
            const std::optional<UniformType> found = uniformTypeFromGL(gl);
            ensure(llformat("0x%x has a type", gl), found.has_value());
            ensure_equals(llformat("0x%x's type", gl), int(*found), int(type));
        }
        for (const U32 gl : { U32(GL_DOUBLE), U32(GL_FLOAT_MAT2x3), U32(GL_INT_SAMPLER_2D), U32(0) })
        {
            ensure(llformat("0x%x has no type", gl), !uniformTypeFromGL(gl).has_value());
        }
        for (int i = int(UniformType::Float); i <= int(UniformType::SamplerCubeArray); ++i)
        {
            const UniformType type = UniformType(i);
            const std::optional<UniformType> back = uniformTypeFromGL(toGLUniformType(type));
            ensure(llformat("type %d has a GL enum the table takes", i), back.has_value());
            ensure_equals(llformat("type %d comes back", i), int(*back), i);
        }
        ensure_equals("Sampler2D's GL enum", toGLUniformType(UniformType::Sampler2D), U32(GL_SAMPLER_2D));
        ensure_equals("Int's GL enum", toGLUniformType(UniformType::Int), U32(GL_INT));
        ensure_equals("IVec4's GL enum", toGLUniformType(UniformType::IVec4), U32(GL_INT_VEC4));
        ensure_equals("Mat3x4's GL enum", toGLUniformType(UniformType::Mat3x4), U32(GL_FLOAT_MAT3x4));
        // Sampler3D comes from the Metal translation only; GL's reflection keeps GL_SAMPLER_3D as Sampler2D.
        ensure_equals("Sampler3D's GL enum", toGLUniformType(UniformType::Sampler3D), U32(GL_SAMPLER_3D));
        ensure_equals("GL_SAMPLER_3D stays Sampler2D", int(*uniformTypeFromGL(GL_SAMPLER_3D)), int(UniformType::Sampler2D));
    }

    /// How often a text appears in what a CaptureLog caught.
    size_t occurrences(const CaptureLog& log, const std::string& what)
    {
        std::ostringstream caught;
        log.streamto(caught);
        const std::string text = caught.str();
        size_t count = 0;
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size()))
        {
            ++count;
        }
        return count;
    }

    template<> template<>
    void object::test<44>()
    {
        set_test_name("the startup check: each missing fact gives GL and one WARN with its reason, "
                      "all facts give Metal with the device's name, and GL asked for gives GL");
        PlatformFacts all;
        all.built = all.arm64 = all.metalDevice = all.macOS26 = all.metal4 = all.apple7 = true;
        all.gpuName = "Test GPU"; // P3-01f, M18: the CGL store's key

        struct Refusal
        {
            const char* name;
            std::function<void(PlatformFacts&)> change;
            const char* reason;
        };
        const Refusal refusals[] = {
            // A translated process is x86_64, so it is not arm64 either; Rosetta is named first.
            { "Rosetta", [](PlatformFacts& f) { f.translated = true; f.arm64 = false; },
              "the process runs translated by Rosetta; Metal needs a native arm64 process" },
            { "not arm64", [](PlatformFacts& f) { f.arm64 = false; },
              "the process is not arm64 (Metal needs Apple Silicon)" },
            { "not built", [](PlatformFacts& f) { f.built = false; },
              "the Metal backend is not built into this viewer (no Metal 4 SDK)" },
            { "before macOS 26", [](PlatformFacts& f) { f.macOS26 = false; }, "macOS is older than 26.0" },
            { "no device", [](PlatformFacts& f) { f.metalDevice = false; }, "there is no Metal device" },
            { "no Metal4", [](PlatformFacts& f) { f.metal4 = false; }, "the GPU lacks MTLGPUFamilyMetal4" },
            { "no Apple7", [](PlatformFacts& f) { f.apple7 = false; }, "the GPU lacks MTLGPUFamilyApple7" },
        };
        for (const Refusal& refusal : refusals)
        {
            const std::string name = refusal.name;
            PlatformFacts facts = all;
            refusal.change(facts);
            CaptureLog log(LLError::LEVEL_WARN);
            const BackendChoice choice = chooseBackend(BackendType::Metal, facts);
            ensure(name + ": GL", choice.type == BackendType::GL);
            ensure_equals(name + ": the reason", choice.reason, std::string(refusal.reason));
            ensure(name + ": no device name on GL", choice.gpuName.empty());
            ensure_equals(name + ": one WARN", occurrences(log, "WARNING"), size_t(1));
            ensure_equals(name + ": tagged RHI", occurrences(log, "WARNING #RHI#"), size_t(1));
            log.messageWith(std::string("Render backend: GL, because ") + refusal.reason);
        }

        {
            CaptureLog log(LLError::LEVEL_INFO);
            const BackendChoice choice = chooseBackend(BackendType::Metal, all);
            ensure("all facts: Metal", choice.type == BackendType::Metal);
            ensure("all facts: no reason", choice.reason.empty());
            ensure_equals("all facts: the device's name", choice.gpuName, std::string("Test GPU"));
            ensure_equals("all facts: no WARN", occurrences(log, "WARNING"), size_t(0));
            log.messageWith("Render backend: Metal");
        }
        const PlatformFacts none;
        const PlatformFacts* const asked[] = { &all, &none };
        for (const PlatformFacts* facts : asked)
        {
            CaptureLog log(LLError::LEVEL_INFO);
            const BackendChoice choice = chooseBackend(BackendType::GL, *facts);
            ensure("GL asked for: GL", choice.type == BackendType::GL);
            ensure("GL asked for: no reason", choice.reason.empty());
            ensure("GL asked for: no device name", choice.gpuName.empty());
            ensure_equals("GL asked for: no WARN", occurrences(log, "WARNING"), size_t(0));
            log.messageWith("Render backend: GL (Metal not asked for)");
        }
    }

    template<> template<>
    void object::test<45>()
    {
        set_test_name("chooseBackend gathers the facts only for Metal, and Device::create refuses a hand-made "
                      "Metal choice with one WARN");
        DeviceCreateInfo info;
        info.backend.type = BackendType::Metal;
#if LL_RHI_METAL
        sMetalSupport = MetalSupport();
        sMetalSupport.macOS26 = sMetalSupport.device = sMetalSupport.metal4 = true;
        sMetalSupport.renderer = "Stub GPU";
        {
            CaptureLog log(LLError::LEVEL_WARN);
            const int calls = sMetalBackendCalls;
            const BackendChoice choice = chooseBackend(BackendType::Metal);
            ensure_equals("Metal asked for: the backend is asked once", sMetalBackendCalls, calls + 1);
            ensure("no Apple7: GL", choice.type == BackendType::GL);
            ensure_equals("no Apple7: the reason", choice.reason, std::string("the GPU lacks MTLGPUFamilyApple7"));
            ensure_equals("no Apple7: one WARN", occurrences(log, "WARNING #RHI#"), size_t(1));
            ensure("no Apple7: no device name", choice.gpuName.empty());
        }
        sMetalSupport.apple7 = true;
        {
            const BackendChoice choice = chooseBackend(BackendType::Metal);
            ensure("full support: Metal", choice.type == BackendType::Metal && choice.reason.empty());
            ensure_equals("full support: the asked device's name (P3-01f, M18)", choice.gpuName, std::string("Stub GPU"));
            const int calls = sMetalBackendCalls;
            ensure("GL asked for: GL", chooseBackend(BackendType::GL).type == BackendType::GL);
            ensure_equals("GL asked for: the backend is not asked", sMetalBackendCalls, calls);
        }
        sMetalSupport = MetalSupport();
        {
            CaptureLog log(LLError::LEVEL_WARN);
            ensure("a hand-made Metal choice on an old macOS: null", Device::create(info).isNull());
            ensure_equals("one WARN", occurrences(log, "WARNING #RHI#"), size_t(1));
            log.messageWith("Could not create device 'Metal': macOS is older than 26.0");
        }
#else
        const BackendChoice choice = chooseBackend(BackendType::Metal);
        ensure("no Metal backend: GL", choice.type == BackendType::GL);
#if defined(__arm64__) || defined(__aarch64__)
        ensure_contains("and why", choice.reason, "not built into this viewer");
#else
        // Decision 13 names Rosetta and the architecture before the build: an x86_64 process on
        // Windows, Linux or an Intel Mac hears that it is not arm64, a translated one that it runs
        // under Rosetta.
        ensure("and why: Rosetta or not arm64, not " + choice.reason,
               choice.reason.find("Rosetta") != std::string::npos ||
                   choice.reason.find("not arm64") != std::string::npos);
#endif
        CaptureLog log(LLError::LEVEL_WARN);
        ensure("a hand-made Metal choice: null", Device::create(info).isNull());
        ensure_equals("one WARN", occurrences(log, "WARNING #RHI#"), size_t(1));
#endif
    }

    template<> template<>
    void object::test<46>()
    {
        set_test_name("hasAlphaInGLFormat is false exactly for the eight GL formats without alpha whose format has alpha (C4)");
        const U32 internalFormats[] = {
            GL_R8, GL_RG8, GL_RGBA8, GL_SRGB8_ALPHA8, GL_R16F, GL_RG16F, GL_RGBA16F, GL_RGBA16, GL_R32F, GL_RG32F,
            GL_RGBA32F, GL_RGB10_A2, GL_R11F_G11F_B10F, GL_R8UI, GL_R16UI, GL_R32UI, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
            GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT,
            GL_COMPRESSED_RG_RGTC2, GL_COMPRESSED_RGBA_BPTC_UNORM, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM,
            GL_DEPTH_COMPONENT32F, GL_DEPTH32F_STENCIL8, GL_RED, GL_RG, GL_RGB, GL_RGBA, GL_SRGB, GL_SRGB_ALPHA, GL_RGB8,
            GL_SRGB8, GL_RGB16F, GL_RGB32F, GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT32,
            GL_DEPTH24_STENCIL8, GL_ALPHA, GL_ALPHA8, GL_LUMINANCE, GL_LUMINANCE8, GL_LUMINANCE_ALPHA,
            GL_LUMINANCE8_ALPHA8, GL_COMPRESSED_RED, GL_COMPRESSED_RG, GL_COMPRESSED_RGB, GL_COMPRESSED_RGBA,
            GL_COMPRESSED_SRGB, GL_COMPRESSED_SRGB_ALPHA, GL_COMPRESSED_ALPHA, GL_COMPRESSED_LUMINANCE,
            GL_COMPRESSED_LUMINANCE_ALPHA,
        };
        const std::set<U32> withoutAlpha = { GL_RGB, GL_RGB8, GL_SRGB, GL_SRGB8, GL_COMPRESSED_RGB, GL_COMPRESSED_SRGB,
                                             GL_RGB16F, GL_RGB32F };
        const std::set<Format> alphaFormats = { Format::RGBA8Unorm, Format::RGBA8Unorm_sRGB, Format::RGBA16Float,
                                                Format::RGBA16Unorm, Format::RGBA32Float, Format::RGB10A2Unorm,
                                                Format::BC1_RGBA, Format::BC1_RGBA_sRGB, Format::BC3_RGBA,
                                                Format::BC3_RGBA_sRGB, Format::BC7_RGBA, Format::BC7_RGBA_sRGB };
        size_t falses = 0;
        for (U32 gl : internalFormats)
        {
            const Format format = formatFromGL(gl);
            ensure("GL format " + std::to_string(gl) + " is in the table", format != Format::Unknown);
            if (!hasAlphaInGLFormat(gl))
            {
                ++falses;
                ensure("GL format " + std::to_string(gl) + ": no alpha only where the format has alpha",
                       alphaFormats.count(format) == 1);
            }
            if (alphaFormats.count(format))
            {
                ensure_equals("GL format " + std::to_string(gl) + " keeps alpha", hasAlphaInGLFormat(gl),
                              withoutAlpha.count(gl) == 0);
            }
        }
        ensure_equals("eight formats without alpha", falses, size_t(8));
        ensure("0 has alpha", hasAlphaInGLFormat(0));
    }

    template<> template<>
    void object::test<47>()
    {
        set_test_name("the normal backend choice (P3-01h): --backend over GraphicsBackend, and a saved Metal whose last "
                      "session did not end normally gives GL once");
        struct Case
        {
            const char* name;
            const char* saved;
            const char* commandLine;
            bool marker;
            bool markerApplies;
            BackendType type;
            bool fellBack;
            bool setMarker;
            bool clearMarker;
            size_t warnings;
            const char* logged; ///< a text the log must have; nullptr: the log stays empty
        };
        const Case cases[] = {
            { "default", "OpenGL", "", false, true, BackendType::GL, false, false, false, 0, nullptr },
            { "saved Metal", "Metal", "", false, true, BackendType::Metal, false, true, false, 0, nullptr },
            { "saved metal in lower case", "metal", "", false, true, BackendType::Metal, false, true, false, 0, nullptr },
            { "saved Metal and the marker", "Metal", "", true, true, BackendType::GL, true, false, true, 1,
              "Render backend: GL, because the last Metal session did not end normally" },
            { "command-line metal ignores the marker", "Metal", "metal", true, true, BackendType::Metal, false, true,
              false, 0, nullptr },
            { "command-line METAL over saved OpenGL", "OpenGL", "METAL", false, true, BackendType::Metal, false, true,
              false, 0, nullptr },
            { "command-line opengl over saved Metal", "Metal", "opengl", false, true, BackendType::GL, false, false, false,
              0, nullptr },
            { "command-line opengl keeps the marker for the saved Metal", "Metal", "opengl", true, true, BackendType::GL,
              false, false, false, 0, nullptr },
            { "a stale marker under saved OpenGL is cleared", "OpenGL", "", true, true, BackendType::GL, false, false,
              true, 0, "Graphics backend: MetalSessionOpen cleared, since GraphicsBackend is not Metal" },
            { "unknown saved value", "Vulkan", "", false, true, BackendType::GL, false, false, false, 1,
              "Graphics backend: GraphicsBackend 'Vulkan' is neither OpenGL nor Metal; GL" },
            { "empty saved value", "", "", false, true, BackendType::GL, false, false, false, 1,
              "Graphics backend: GraphicsBackend '' is neither OpenGL nor Metal; GL" },
            { "unknown command line: the saved Metal decides", "Metal", "vulkan", false, true, BackendType::Metal, false,
              true, false, 1, "Graphics backend: --backend 'vulkan' is neither metal nor opengl; GraphicsBackend decides" },
            { "dev mode or second instance: saved Metal and the marker", "Metal", "", true, false, BackendType::Metal,
              false, false, false, 0, nullptr },
            { "dev mode or second instance: command-line metal", "OpenGL", "metal", false, false, BackendType::Metal,
              false, false, false, 0, nullptr },
        };
        for (const Case& c : cases)
        {
            const std::string name = c.name;
            BackendRequestFacts facts;
            facts.saved            = c.saved;
            facts.commandLine      = c.commandLine;
            facts.metalSessionOpen = c.marker;
            facts.markerApplies    = c.markerApplies;
            CaptureLog log(LLError::LEVEL_INFO);
            const BackendRequest request = resolveBackendRequest(facts);
            ensure(name + ": the backend", request.type == c.type);
            ensure_equals(name + ": fellBack", request.fellBack, c.fellBack);
            ensure_equals(name + ": setMarker", request.setMarker, c.setMarker);
            ensure_equals(name + ": clearMarker", request.clearMarker, c.clearMarker);
            ensure_equals(name + ": WARN lines", occurrences(log, "WARNING #RHI#"), c.warnings);
            if (c.logged)
            {
                log.messageWith(c.logged);
            }
            else
            {
                std::ostringstream caught;
                log.streamto(caught);
                ensure_equals(name + ": nothing logged", caught.str(), std::string());
            }
        }

        // At a normal quit only a session that set the marker and still runs on Metal clears it. After the in-process
        // GL fallback the marker stays, so the next start is the case "saved Metal and the marker" above (M-3).
        ensure("a Metal session clears its marker", clearsMetalSessionAtQuit(true, BackendType::Metal));
        ensure("a session that fell back to GL keeps it", !clearsMetalSessionAtQuit(true, BackendType::GL));
        ensure("a session that did not set it leaves it", !clearsMetalSessionAtQuit(false, BackendType::Metal));

        // The main window's title names the backend the viewer runs on (P3-UX1). This is the helper alone; that
        // initWindow calls it after the device, and so after the in-process GL fallback, the login screen runs' log
        // lines check (level2.py, metal_login_check).
        ensure_equals("title on Metal", windowTitleWithBackend("Second Life", BackendType::Metal),
                      std::string("Second Life (Metal)"));
        ensure_equals("title on GL", windowTitleWithBackend("Second Life", BackendType::GL),
                      std::string("Second Life (OpenGL)"));
        ensure_equals("a debug build's title keeps its tag",
                      windowTitleWithBackend("Second Life [DEBUG]", BackendType::Metal),
                      std::string("Second Life [DEBUG] (Metal)"));

        // The Preferences > Graphics switch (IN-6): its entry for the saved value, what it saves from the next start on,
        // and where it shows.
        const std::string none;
        ensure_equals("entry for Metal", graphicsBackendEntry("Metal"), std::string("Metal"));
        ensure_equals("entry for metal in lower case", graphicsBackendEntry("metal"), std::string("Metal"));
        ensure_equals("entry for OpenGL", graphicsBackendEntry("OpenGL"), std::string("OpenGL"));
        ensure_equals("entry for an unknown value", graphicsBackendEntry("Vulkan"), std::string("OpenGL"));
        ensure_equals("entry for an empty value", graphicsBackendEntry(""), std::string("OpenGL"));
        ensure_equals("the switch to Metal saves Metal", graphicsBackendToSave("OpenGL", "Metal"), std::string("Metal"));
        ensure_equals("the switch to OpenGL saves OpenGL", graphicsBackendToSave("Metal", "OpenGL"),
                      std::string("OpenGL"));
        ensure_equals("Metal over an unknown value", graphicsBackendToSave("Vulkan", "Metal"), std::string("Metal"));
        ensure_equals("an unchanged Metal keeps its spelling", graphicsBackendToSave("metal", "Metal"), none);
        ensure_equals("an unchanged OpenGL saves nothing", graphicsBackendToSave("OpenGL", "OpenGL"), none);
        ensure_equals("OpenGL over an unknown value saves nothing (it starts GL already)",
                      graphicsBackendToSave("Vulkan", "OpenGL"), none);
        ensure_equals("no entry saves nothing", graphicsBackendToSave("Metal", ""), none);
        for (const char* chosen : { "Metal", "OpenGL" })
        {
            BackendRequestFacts facts;
            facts.saved = graphicsBackendToSave(std::string(chosen) == "Metal" ? "OpenGL" : "Metal", chosen);
            CaptureLog log(LLError::LEVEL_INFO);
            ensure(std::string("the next start runs the saved ") + chosen,
                   resolveBackendRequest(facts).type ==
                       (std::string(chosen) == "Metal" ? BackendType::Metal : BackendType::GL));
        }
#if LL_RHI_METAL
        const bool built = true;
#else
        const bool built = false;
#endif
        ensure("no switch before macOS 26", !metalSelectable(15) && !metalSelectable(25));
        ensure_equals("the switch on macOS 26 and later where Metal is built", metalSelectable(26), built);
        ensure_equals("macOS 27 as 26", metalSelectable(27), metalSelectable(26));
    }

    template<> template<>
    void object::test<48>()
    {
        set_test_name("the load threads at the start (P3-03d): GL has Linden's two settings as they are, Metal also its "
                      "own two, on by default");
#if LL_RHI_METAL
        const bool built = true;
#else
        const bool built = false;
#endif
        // The defaults as settings.xml has them: Linden's off (featuretable_mac.txt's AppleGPU list), Metal's on.
        const UploadThreadSettings defaults;
        ensure("Linden's two are off by default", !defaults.renderGLTextures && !defaults.renderGLMedia);
        ensure("Metal's two are on by default", defaults.metalTextures && defaults.metalMedia);
        {
            CaptureLog log(LLError::LEVEL_INFO);
            const UploadThreads gl = uploadThreads(BackendType::GL, defaults);
            ensure("GL by default: no load thread", !gl.textures && !gl.media);
            std::ostringstream caught;
            log.streamto(caught);
            ensure_equals("GL logs nothing", caught.str(), std::string());
        }
        {
            CaptureLog log(LLError::LEVEL_INFO);
            const UploadThreads metal = uploadThreads(BackendType::Metal, defaults);
            ensure_equals("Metal by default: textures on the load thread where Metal is built", metal.textures, built);
            ensure_equals("Metal by default: media on the load thread where Metal is built", metal.media, built);
            if (built)
            {
                log.messageWith("Load threads on Metal (P3-03d): textures on (MetalMultiThreadedTextures 1, "
                                "RenderGLMultiThreadedTextures 0), media on (MetalMultiThreadedMedia 1, "
                                "RenderGLMultiThreadedMedia 0)");
            }
        }
        // Every combination of the four, each load on its own: GL follows Linden's setting alone; Metal is on when
        // Linden's or its own is (a hand-set RenderGLMultiThreaded* 1 holds on both, Metal's 0 turns Metal's off).
        for (U32 bits = 0; bits < 16; ++bits)
        {
            UploadThreadSettings settings;
            settings.renderGLTextures = (bits & 1) != 0;
            settings.renderGLMedia    = (bits & 2) != 0;
            settings.metalTextures    = (bits & 4) != 0;
            settings.metalMedia       = (bits & 8) != 0;
            const std::string name = "settings " + std::to_string(bits);
            CaptureLog log(LLError::LEVEL_INFO);
            const UploadThreads gl = uploadThreads(BackendType::GL, settings);
            ensure_equals(name + ": GL's textures are Linden's", gl.textures, settings.renderGLTextures);
            ensure_equals(name + ": GL's media are Linden's", gl.media, settings.renderGLMedia);
            const UploadThreads metal = uploadThreads(BackendType::Metal, settings);
            ensure_equals(name + ": Metal's textures", metal.textures,
                          settings.renderGLTextures || (built && settings.metalTextures));
            ensure_equals(name + ": Metal's media", metal.media,
                          settings.renderGLMedia || (built && settings.metalMedia));
            if (built)
            {
                ensure_equals(name + ": one line, from the Metal call", occurrences(log, "Load threads on Metal"),
                              size_t(1));
            }
        }
        {
            // Metal's switches off: the loads are off on Metal too, unless Linden's settings turn them on.
            UploadThreadSettings off;
            off.metalTextures = false;
            off.metalMedia    = false;
            CaptureLog log(LLError::LEVEL_INFO);
            const UploadThreads metal = uploadThreads(BackendType::Metal, off);
            ensure("Metal with its switches off: no load thread", !metal.textures && !metal.media);
            if (built)
            {
                log.messageWith("Load threads on Metal (P3-03d): textures off (MetalMultiThreadedTextures 0, "
                                "RenderGLMultiThreadedTextures 0), media off (MetalMultiThreadedMedia 0, "
                                "RenderGLMultiThreadedMedia 0)");
            }
        }
    }

    template<> template<>
    void object::test<49>()
    {
        set_test_name("idle(true) frees what was released before it, the current frame's included, and finishes only "
                      "the earlier frames; what is released after it waits for its frame or the next idle(true)");
        TestGpu gpu;
        FrameLifecycle lifecycle(gpu.waitFunction());
        int freed = 0;
        lifecycle.release([&freed] { ++freed; });
        lifecycle.endFrame(); // recording 2
        lifecycle.release([&freed] { ++freed; });
        lifecycle.idle(true);
        ensure_equals("frame 1 is finished", lifecycle.completedFrame(), 1u);
        ensure_equals("frame 2 is not: more of it may come", lifecycle.currentFrame(), 2u);
        ensure_equals("both releases freed", freed, 2);
        lifecycle.release([&freed] { ++freed; });
        lifecycle.collect();
        ensure_equals("a release after the wait waits", lifecycle.pendingReleases(), 1u);
        lifecycle.idle(true);
        ensure_equals("and goes at the next wait", freed, 3);
        lifecycle.release([&freed] { ++freed; });
        lifecycle.idle();
        ensure_equals("idle() alone keeps the current frame's", lifecycle.pendingReleases(), 1u);
        lifecycle.releaseAll();
    }

    // Render passes and the encoder (decisions 2, 3, 7, 9 and 11) -------------------------------
    // tut runs at most 50 tests in a group, so these are a group of their own.

    struct llrhi_encoder_data
    {
    };
    typedef test_group<llrhi_encoder_data> llrhi_encoder_group;
    typedef llrhi_encoder_group::object encoder_object;
    llrhi_encoder_group llrhi_encoder_test_group("LLRHI encoder");


    template<> template<>
    void encoder_object::test<1>()
    {
        set_test_name("render pass descriptions are checked");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Texture> albedo = device->createTexture(target(1280, 720), "albedo");
        Ref<Texture> normal = device->createTexture(target(1280, 720, Format::RGB10A2Unorm), "normal");
        Ref<Texture> depth = device->createTexture(target(1280, 720, Format::Depth32Float), "depth");
        Ref<Texture> smaller = device->createTexture(target(640, 360), "small");
        Ref<Texture> msaa = device->createTexture(target(1280, 720, Format::RGBA8Unorm, 4), "msaa");
        Ref<Texture> sampledOnly = device->createTexture(texture2D(1280, 720), "sampled only");

        RenderPassDesc pass;
        pass.name = "G-buffer";
        pass.colorCount = 2;
        pass.color[0].texture = albedo;
        pass.color[1].texture = normal;
        pass.depth.texture = depth;
        ensure("valid", validate(pass).empty());

        RenderPassDesc broken = pass;
        broken.colorCount = 3;
        ensure_contains("missing texture", validate(broken), "color attachment 2 has no texture");
        broken = pass;
        broken.color[2].texture = smaller;
        ensure_contains("unused slot", validate(broken), "not used but has a texture");
        broken = pass;
        broken.color[1].texture = smaller;
        ensure_contains("size", validate(broken), "is 640x360, the pass 1280x720");
        broken = pass;
        broken.color[1].texture = msaa;
        ensure_contains("samples", validate(broken), "has 4 samples, the pass 1");
        broken = pass;
        broken.color[1].texture = sampledOnly;
        ensure_contains("not a target", validate(broken), "'sampled only' is not a render target");
        broken = pass;
        broken.color[1].texture = depth;
        ensure_contains("depth as color", validate(broken), "depth or compressed");
        broken = pass;
        broken.depth.texture = albedo;
        ensure_contains("color as depth", validate(broken), "not a depth format");
        broken = pass;
        broken.color[0].level = 1;
        ensure_contains("level", validate(broken), "uses level 1 of 1");
        broken = pass;
        broken.depth.texture = nullptr;
        broken.depth.readOnly = true;
        ensure_contains("read-only without depth", validate(broken), "read-only depth needs");
        broken = pass;
        broken.depth.readOnly = true;
        broken.depth.load = LoadAction::Clear;
        ensure_contains("read-only cleared", validate(broken), "cannot be cleared");
        RenderPassDesc none;
        ensure_contains("nothing", validate(none), "no attachments");

        TextureDesc cubeDesc = target(256, 256);
        cubeDesc.type = TextureType::Cube;
        Ref<Texture> cube = device->createTexture(cubeDesc, "probe");
        RenderPassDesc face;
        face.colorCount = 1;
        face.color[0].texture = cube;
        face.color[0].layer = 5;
        ensure("last face", validate(face).empty());
        face.color[0].layer = 6;
        ensure_contains("no seventh face", validate(face), "uses layer 6 of 6");

        TextureDesc arrayDesc = cubeDesc;
        arrayDesc.type = TextureType::CubeArray;
        arrayDesc.layers = 2;
        RenderPassDesc arrayFace;
        arrayFace.colorCount = 1;
        arrayFace.color[0].texture = device->createTexture(arrayDesc, "probe array");
        arrayFace.color[0].layer = 11;
        ensure("second cube's last face", validate(arrayFace).empty());
        arrayFace.color[0].layer = 12;
        ensure_contains("past the second cube", validate(arrayFace), "uses layer 12 of 12");

        TextureDesc mipDesc = target(1024, 1024);
        mipDesc.mipLevels = 3;
        Ref<Texture> mips = device->createTexture(mipDesc, "mips");
        RenderPassDesc level;
        level.colorCount = 2;
        level.color[0].texture = mips;
        level.color[0].level = 2;
        level.color[1].texture = device->createTexture(target(256, 256), "quarter");
        ensure("level 2 is 256x256", validate(level).empty());
    }

    template<> template<>
    void encoder_object::test<2>()
    {
        set_test_name("a pipeline fits a pass only with the same attachments and samples");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        RenderPassDesc pass;
        pass.colorCount = 1;
        pass.color[0].texture = device->createTexture(target(64, 64), "color");
        pass.depth.texture = device->createTexture(target(64, 64, Format::Depth32Float), "depth");
        ensure("fits", checkPipelineForPass(colorPipeline(), pass).empty());
        ensure_contains("format", checkPipelineForPass(colorPipeline(Format::RGBA16Float), pass), "another format");
        ensure_contains("sRGB", checkPipelineForPass(colorPipeline(Format::RGBA8Unorm_sRGB), pass), "another format");
        PipelineDesc noDepth = colorPipeline();
        noDepth.depthFormat = Format::Unknown;
        ensure_contains("depth", checkPipelineForPass(noDepth, pass), "depth attachment has another format");
        PipelineDesc two = colorPipeline();
        two.colorCount = 2;
        two.colorFormats[1] = Format::RGBA8Unorm;
        ensure_contains("count", checkPipelineForPass(two, pass), "2 color attachments, the pass 1");
        PipelineDesc msaa = colorPipeline();
        msaa.samples = 4;
        ensure_contains("samples", checkPipelineForPass(msaa, pass), "4 samples, the pass 1");
    }

    template<> template<>
    void encoder_object::test<3>()
    {
        set_test_name("ranges and regions must lie inside the buffer or the texture");
        const BufferDesc buffer = vertexBuffer(1024);
        ensure("whole", validateRange(buffer, 0, 1024).empty());
        ensure("end", validateRange(buffer, 1000, 24).empty());
        ensure_contains("past the end", validateRange(buffer, 1000, 25), "beyond the buffer's 1024");
        ensure_contains("offset past", validateRange(buffer, 2048, 1), "beyond");
        ensure_contains("huge size", validateRange(buffer, 8, ~U64(0)), "beyond");
        ensure_contains("empty", validateRange(buffer, 0, 0), "empty");

        TextureDesc texture = texture2D(512, 256);
        texture.mipLevels = 3;
        ensure("whole level 0", validateRegion(texture, 0, 0, { 0, 0, 512, 256 }).empty());
        ensure("whole level 2", validateRegion(texture, 2, 0, { 0, 0, 128, 64 }).empty());
        ensure_contains("level 2 is smaller", validateRegion(texture, 2, 0, { 0, 0, 129, 64 }), "beyond level 2's 128x64");
        ensure_contains("level 3", validateRegion(texture, 3, 0, { 0, 0, 1, 1 }), "level 3 is beyond");
        ensure_contains("layer", validateRegion(texture, 0, 1, { 0, 0, 1, 1 }), "layer 1 is beyond");
        ensure_contains("offset", validateRegion(texture, 0, 0, { 500, 0, 13, 1 }), "beyond");
        ensure_contains("empty", validateRegion(texture, 0, 0, { 0, 0, 0, 1 }), "empty");
    }

    template<> template<>
    void encoder_object::test<4>()
    {
        set_test_name("a complete frame: a G-buffer pass with draws, then a light pass on its depth");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Encoder& encoder = device->encoder();

        Ref<Texture> albedo = device->createTexture(target(1280, 720), "albedo");
        Ref<Texture> normal = device->createTexture(target(1280, 720, Format::RGB10A2Unorm), "normal");
        Ref<Texture> depth = device->createTexture(target(1280, 720, Format::Depth32Float), "depth");
        Ref<Texture> diffuse = device->createTexture(texture2D(512, 512, Format::BC3_RGBA), "bark");
        BufferDesc vertexDesc = vertexBuffer(64 * 1024);
        Ref<Buffer> vertices = device->createBuffer(vertexDesc, {}, "tree");
        BufferDesc indexDesc = vertexBuffer(6 * 1024);
        indexDesc.usage = BUFFER_INDEX;
        Ref<Buffer> indices = device->createBuffer(indexDesc, {}, "tree indices");

        // The G-buffer program and its pipeline, as LLGLSLShader and LLRender would ask for them.
        Ref<Program> gbuffer = device->createProgram(program("deferredTreeProgram", 0x7));
        auto& reflection = static_cast<TestProgram*>(gbuffer.get())->mReflection;
        reflection.uniforms = { { "modelview_projection_matrix", UniformType::Mat4, 1 },
                                { "diffuseMap", UniformType::Sampler2D, 1 },
                                { "minimum_alpha", UniformType::Float, 1 } };
        gbuffer->wait();
        PipelineDesc gbufferPipeline;
        gbufferPipeline.attributeMask = 0x7;
        gbufferPipeline.colorCount = 2;
        gbufferPipeline.colorFormats[0] = Format::RGBA8Unorm;
        gbufferPipeline.colorFormats[1] = Format::RGB10A2Unorm;
        gbufferPipeline.depthFormat = Format::Depth32Float;
        Ref<Pipeline> trees = device->getPipeline(*gbuffer, gbufferPipeline);
        ensure("pipeline", trees.notNull());

        RenderPassDesc gpass;
        gpass.name = "G-buffer";
        gpass.colorCount = 2;
        gpass.color[0].texture = albedo;
        gpass.color[0].load = LoadAction::Clear;
        gpass.color[1].texture = normal;
        gpass.color[1].load = LoadAction::Clear;
        gpass.depth.texture = depth;
        gpass.depth.load = LoadAction::Clear;

        encoder.pushDebugGroup("frame");
        encoder.beginRenderPass(gpass);
        encoder.setPipeline(trees.get());
        encoder.setDepthStencilState(device->getDepthStencilState({ true, true, CompareFunc::LessEqual }).get());
        encoder.setViewport(0, 0, 1280, 720);
        encoder.setScissor(false, 0, 0, 1280, 720);
        encoder.setCullMode(CullMode::Back);
        const F32 matrix[16] = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                                 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f };
        encoder.setUniform(gbuffer.get(), 0, { reinterpret_cast<const std::byte*>(matrix), sizeof(matrix) });
        const F32 minimumAlpha = 0.5f;
        encoder.setUniform(gbuffer.get(), 2, bytesOf(minimumAlpha));
        encoder.setSamplerChannel(gbuffer.get(), 1, 0);
        SamplerDesc trilinear;
        trilinear.filter = Filter::Trilinear;
        encoder.setTexture(0, diffuse.get(), trilinear);
        VertexStreams streams;
        streams.attributeMask = 0x7;
        streams.offset = { 0, 16384, 32768 };
        encoder.setVertexBuffer(vertices.get(), streams);
        encoder.setIndexBuffer(indices.get(), IndexType::U16);
        encoder.pushDebugGroup("trees");
        encoder.drawIndexed(PrimitiveType::Triangles, 0, 3072);
        encoder.popDebugGroup();
        encoder.clear(false, { 0.f, 0.f, 0.f, 0.f }, true, 1.f); // the sky clears depth in the pass
        encoder.draw(PrimitiveType::TriangleStrip, 0, 4);
        encoder.endRenderPass();

        // The light pass draws into the back buffer and samples the depth it tests against.
        Ref<Program> light = device->createProgram(program("sunLightProgram", 0x1));
        light->wait();
        PipelineDesc lightPipeline = colorPipeline();
        Ref<Pipeline> sun = device->getPipeline(*light, lightPipeline);
        RenderPassDesc lpass;
        lpass.name = "sun light";
        lpass.colorCount = 1;
        lpass.color[0].texture = device->surface()->backBuffer();
        lpass.depth.texture = depth;
        lpass.depth.readOnly = true;
        encoder.beginRenderPass(lpass);
        encoder.setPipeline(sun.get());
        encoder.setDepthStencilState(device->getDepthStencilState({ true, false, CompareFunc::LessEqual }).get());
        encoder.setTexture(1, albedo.get(), SamplerDesc());
        encoder.setTexture(2, depth.get(), SamplerDesc());
        encoder.draw(PrimitiveType::TriangleStrip, 0, 4);
        encoder.endRenderPass();
        encoder.popDebugGroup();
        device->endFrame();

        auto& testEncoder = device->mEncoder;
        ensure_equals("no errors: " + (testEncoder.errors.empty() ? std::string() : testEncoder.errors[0]),
                      testEncoder.errors.size(), 0u);
        const std::vector<std::string> expected = {
            "push", "begin G-buffer", "pipeline", "depth state", "viewport", "scissor", "cull",
            "uniform", "uniform", "channel", "texture", "vertices", "indices", "push",
            "draw indexed", "pop", "clear", "draw", "end", "begin sun light", "pipeline",
            "depth state", "texture", "texture", "draw", "end", "pop" };
        ensure("the frame as recorded", testEncoder.log == expected);
        ensure("the frame's passes are let go", !testEncoder.tracker.inPass());
    }

    template<> template<>
    void encoder_object::test<5>()
    {
        set_test_name("draws that do not fit are rejected, and a rejected call changes nothing");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        Ref<Program> simple = device->createProgram(program("objectSimpleProgram", 0x3));
        PipelineDesc desc = colorPipeline();
        desc.attributeMask = 0x3;
        Ref<Pipeline> triangles = device->getPipeline(*simple, desc);
        desc.primitiveClass = PrimitiveClass::Line;
        Ref<Pipeline> lines = device->getPipeline(*simple, desc);
        Ref<Pipeline> hdr = device->getPipeline(*simple, [] { PipelineDesc d = colorPipeline(Format::RGBA16Float);
                                                                d.attributeMask = 0x3; return d; }());
        BufferDesc indexDesc = vertexBuffer(600);
        indexDesc.usage = BUFFER_INDEX;
        Ref<Buffer> indices = device->createBuffer(indexDesc, {}, "indices");
        Ref<Buffer> vertices = device->createBuffer(vertexBuffer(4096), {}, "vertices");

        encoder.draw(PrimitiveType::Triangles, 0, 3);
        ensure_contains("outside a pass", encoder.errors.back(), "no pass is open");

        RenderPassDesc pass;
        pass.name = "UI";
        pass.colorCount = 1;
        pass.color[0].texture = device->createTexture(target(64, 64), "color");
        pass.depth.texture = device->createTexture(target(64, 64, Format::Depth32Float), "depth");
        encoder.beginRenderPass(pass);
        encoder.beginRenderPass(pass);
        ensure_contains("pass in a pass", encoder.errors.back(), "pass 'UI' is still open");

        encoder.draw(PrimitiveType::Triangles, 0, 3);
        ensure_contains("no pipeline", encoder.errors.back(), "no pipeline is set");

        encoder.setPipeline(triangles.get());
        encoder.setPipeline(hdr.get());
        ensure_contains("wrong format", encoder.errors.back(), "another format");
        ensure("a rejected pipeline unbinds", encoder.tracker.pipeline() == nullptr);
        encoder.draw(PrimitiveType::Triangles, 0, 3);
        ensure_contains("so its draws are rejected too", encoder.errors.back(), "no pipeline is set");
        encoder.setPipeline(triangles.get());

        encoder.draw(PrimitiveType::Triangles, 0, 3);
        ensure_contains("no vertices", encoder.errors.back(), "no vertex buffer is bound");
        // The refusals name the pipeline and the buffer (P2-R2).
        ensure_equals("the pipeline is named after its program", triangles->debugName(), std::string("objectSimpleProgram"));
        ensure_contains("no vertices: the pipeline's name", encoder.errors.back(),
                        "no vertex buffer is bound for pipeline 'objectSimpleProgram'");
        VertexStreams one;
        one.attributeMask = 0x1;
        encoder.setVertexBuffer(vertices.get(), one);
        encoder.draw(PrimitiveType::Triangles, 0, 3);
        ensure_contains("missing attribute", encoder.errors.back(), "lacks attributes");
        ensure_contains("missing attribute: the buffer's and the pipeline's names", encoder.errors.back(),
                        "the vertex buffer 'vertices' lacks attributes pipeline 'objectSimpleProgram' reads");
        VertexStreams two;
        two.attributeMask = 0x3;
        two.offset = { 0, 2048 };
        encoder.setVertexBuffer(vertices.get(), two);
        encoder.draw(PrimitiveType::Lines, 0, 2);
        ensure_contains("lines with triangles", encoder.errors.back(), "a line draw with a triangle pipeline");
        encoder.setPipeline(lines.get());
        encoder.draw(PrimitiveType::LineStrip, 0, 2);

        encoder.setPipeline(triangles.get());
        encoder.drawIndexed(PrimitiveType::Triangles, 0, 3);
        ensure_contains("no indices", encoder.errors.back(), "no index buffer is bound");
        encoder.setIndexBuffer(vertices.get(), IndexType::U16);
        ensure_contains("not an index buffer", encoder.errors.back(), "is not an index buffer");
        encoder.setIndexBuffer(indices.get(), IndexType::U16);
        encoder.drawIndexed(PrimitiveType::Triangles, 0, 300);
        encoder.drawIndexed(PrimitiveType::Triangles, 297, 6);
        ensure_contains("index range", encoder.errors.back(), "indices 297 to 303 are beyond");
        VertexStreams pastEnd = two;
        pastEnd.offset[1] = 4096;
        encoder.setVertexBuffer(vertices.get(), pastEnd);
        ensure_contains("stream past the buffer", encoder.errors.back(), "starts beyond");
        encoder.draw(PrimitiveType::Triangles, 0, 3);
        ensure_contains("a rejected vertex buffer unbinds", encoder.errors.back(), "no vertex buffer is bound");
        encoder.setVertexBuffer(vertices.get(), two);
        encoder.endRenderPass();

        const size_t accepted = std::count(encoder.log.begin(), encoder.log.end(), std::string("draw")) +
                                std::count(encoder.log.begin(), encoder.log.end(), std::string("draw indexed"));
        ensure_equals("only the fitting draws ran", accepted, size_t(2));

        RenderPassDesc light = pass;
        light.depth.readOnly = true;
        encoder.beginRenderPass(light);
        encoder.setDepthStencilState(device->getDepthStencilState({ true, true, CompareFunc::LessEqual }).get());
        encoder.draw(PrimitiveType::Triangles, 0, 3);
        ensure_contains("writes to read-only depth", encoder.errors.back(), "read-only depth");
        encoder.endRenderPass();
        device->endFrame();
    }

    template<> template<>
    void encoder_object::test<6>()
    {
        set_test_name("debug groups, queries and passes must close before the frame ends");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        encoder.popDebugGroup();
        ensure_contains("pop without push", encoder.errors.back(), "no debug group is open");

        RenderPassDesc pass;
        pass.name = "shadow";
        pass.depth.texture = device->createTexture(target(2048, 2048, Format::Depth32Float), "shadow map");
        encoder.pushDebugGroup("outside");
        encoder.beginRenderPass(pass);
        encoder.popDebugGroup();
        ensure_contains("pop of a group opened outside", encoder.errors.back(), "opened outside pass 'shadow'");
        encoder.pushDebugGroup("inside");
        encoder.endRenderPass();
        ensure_contains("pass ends with a group open", encoder.errors.back(), "ended with 1 debug groups open");
        ensure("the pass ended anyway", !encoder.tracker.inPass());
        ensure("the backend popped the group and ended its native pass",
               encoder.log.size() >= 2 && encoder.log[encoder.log.size() - 2] == "pop 1" && encoder.log.back() == "end");
        encoder.popDebugGroup(); // closes "outside"

        Ref<Query> occlusion = device->createQuery(QueryType::SamplesPassed);
        ensure("query", encoder.beginQuery(occlusion.get()));
        ensure("a second of the same type", !encoder.beginQuery(device->createQuery(QueryType::SamplesPassed).get()));
        ensure("an any-samples query is the same kind",
               !encoder.beginQuery(device->createQuery(QueryType::AnySamplesPassed).get()));
        Ref<Query> timer = device->createQuery(QueryType::TimeElapsed);
        ensure("another type", encoder.beginQuery(timer.get()));
        Ref<Query> stray = new TestQuery(QueryType::AnySamplesPassed);
        encoder.endQuery(stray.get());
        ensure_contains("end of a query never begun", encoder.errors.back(), "not active");
        encoder.endQuery(occlusion.get());

        encoder.beginRenderPass(pass);
        device->endFrame();
        ensure_contains("frame ends in a pass", encoder.errors.back(), "the frame ended in pass 'shadow'");
        ensure("ended", !encoder.tracker.inPass());
        ensure_equals("and the native pass too", encoder.log.back(), std::string("end"));
        encoder.endQuery(timer.get());
        ensure_contains("queries end with the frame", encoder.errors.back(), "not active");

        encoder.errors.clear();
        encoder.pushDebugGroup("left open");
        device->endFrame();
        ensure_contains("frame ends with a group", encoder.errors.back(), "1 debug groups open");
        encoder.errors.clear();
        encoder.beginQuery(device->createQuery(QueryType::SamplesPassed).get());
        device->endFrame();
        ensure_contains("frame ends with a query", encoder.errors.back(), "1 queries active");
        encoder.errors.clear();
        device->endFrame();
        ensure("a clean frame", encoder.errors.empty());
    }

    template<> template<>
    void encoder_object::test<7>()
    {
        set_test_name("bindings last across frames; one nobody else holds goes when its frame ends");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        Ref<Texture> kept = device->createTexture(texture2D(256, 256), "kept");
        encoder.setTexture(1, kept.get(), SamplerDesc());
        {
            Ref<Texture> bark = device->createTexture(texture2D(256, 256), "bark");
            encoder.setTexture(0, bark.get(), SamplerDesc());
        } // the facade let go of it while it is bound
        ensure_equals("the bound texture lives through its frame", device->mLifecycle.pendingReleases(), 0u);

        RenderPassDesc pass;
        pass.colorCount = 1;
        pass.color[0].texture = device->createTexture(target(64, 64), "scratch target");
        encoder.beginRenderPass(pass);
        pass.color[0].texture = nullptr; // only the open pass holds the target now
        ensure_equals("the target lives while its pass is open", device->mLifecycle.pendingReleases(), 0u);
        encoder.endRenderPass();
        ensure_equals("and goes with its pass", device->mLifecycle.pendingReleases(), 1u);

        device->endFrame(); // frame 1 ends: the binding nobody else holds goes
        ensure_equals("bark released in frame 1", device->mLifecycle.pendingReleases(), 2u);
        ensure_equals("the kept texture is still bound", encoder.tracker.heldReferences(), 1u);
        ensure_equals("nothing freed before frame 1 is done", native.freed.load(), 0);
        device->mLifecycle.frameCompleted(1);
        device->mLifecycle.collect();
        ensure_equals("both freed once frame 1 is done", native.freed.load(), 2);

        device->endFrame();
        ensure_equals("bindings last across frames, as on GL", encoder.tracker.heldReferences(), 1u);
        encoder.setTexture(1, nullptr, SamplerDesc());
        ensure_equals("unbound", encoder.tracker.heldReferences(), 0u);
        ensure_equals("the facade still holds it", device->mLifecycle.pendingReleases(), 0u);
    }

    template<> template<>
    void encoder_object::test<8>()
    {
        set_test_name("a texture uploaded on the upload thread waits for the upload's fence");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Fence> fence;
        Ref<Texture> uploaded;
        std::thread uploadThread([&device, &fence, &uploaded]
        {
            // The upload thread may create and fill, but not draw (decision 3).
            uploaded = device->createTexture(texture2D(1024, 1024), "fetched texture");
            std::vector<std::byte> pixels(1024 * 1024 * 4);
            device->uploadContext().uploadTexture(uploaded.get(), 0, 0, { 0, 0, 1024, 1024 },
                                                  PixelLayout(), pixels);
            fence = device->uploadContext().submit();
        });
        uploadThread.join();
        ensure("a fence", fence.notNull());
        ensure("the upload is not done", !fence->signaled());

        uploaded = nullptr; // dropped before the upload finished
        device->endFrame();
        device->mLifecycle.frameCompleted(1);
        device->mLifecycle.collect();
        ensure_equals("not freed while the upload runs", native.freed.load(), 0);
        static_cast<TestFence*>(fence.get())->signal();
        device->mLifecycle.collect();
        ensure_equals("freed after the upload", native.freed.load(), 1);
        ensure("a submit with nothing uploaded still gives a fence", device->uploadContext().submit().notNull());
    }

    template<> template<>
    void encoder_object::test<9>()
    {
        set_test_name("updates, copies and readbacks are checked, and the back buffer can be read back");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        Ref<Buffer> buffer = device->createBuffer(vertexBuffer(256), {}, "dynamic");
        const std::vector<std::byte> data(64);
        encoder.updateBuffer(buffer.get(), 192, data);
        encoder.updateBuffer(buffer.get(), 193, data);
        ensure_contains("update past the end", encoder.errors.back(), "beyond the buffer's 256");

        Ref<Texture> atlas = device->createTexture(texture2D(512, 512), "font atlas");
        const std::vector<std::byte> glyphs(64 * 64 * 4);
        encoder.uploadTexture(atlas.get(), 0, 0, { 448, 448, 64, 64 }, PixelLayout(), glyphs);
        encoder.uploadTexture(atlas.get(), 0, 0, { 449, 448, 64, 64 }, PixelLayout(), glyphs);
        ensure_contains("upload past the edge", encoder.errors.back(), "beyond level 0's 512x512");

        Ref<Texture> other = device->createTexture(texture2D(128, 128), "copy target");
        encoder.copyTexture(atlas.get(), 0, 0, { 0, 0, 128, 128 }, other.get(), 0, 0, 0, 0);
        encoder.copyTexture(atlas.get(), 0, 0, { 0, 0, 128, 128 }, other.get(), 0, 0, 1, 0);
        ensure_contains("copy past the destination", encoder.errors.back(), "destination:");

        encoder.generateMipmaps(atlas.get());
        Ref<Texture> bc = device->createTexture(texture2D(64, 64, Format::BC1_RGBA), "compressed");
        encoder.generateMipmaps(bc.get());
        ensure_contains("mipmaps of BC", encoder.errors.back(), "compressed, depth or integer");

        ensure("no readback of an ordinary texture",
               encoder.readback(atlas.get(), 0, 0, { 0, 0, 1, 1 }, PixelLayout()).isNull());
        ensure_contains("why", encoder.errors.back(), "cannot be read back");
        Ref<Readback> snapshot = encoder.readback(device->surface()->backBuffer(), 0, 0, { 0, 0, 1280, 720 },
                                                  pixelLayoutFromGL(GL_RGB, GL_UNSIGNED_BYTE).value());
        ensure("the back buffer can be read back", snapshot.notNull());
        ensure_equals("in the asked layout", snapshot->data().size(), size_t(1280 * 720 * 3));
        ensure("a buffer readback", encoder.readback(buffer.get(), 0, 256).notNull());
        ensure("a buffer readback past the end", encoder.readback(buffer.get(), 128, 256).isNull());
        ensure_equals("six rejected", encoder.errors.size(), 6u);
    }

    template<> template<>
    void encoder_object::test<10>()
    {
        set_test_name("uploads need enough data for the region, and whole blocks when compressed");
        const TextureDesc rgba = texture2D(256, 256);
        const PixelLayout tight;
        ensure("exact", validateUpload(rgba, 0, 0, { 0, 0, 16, 16 }, tight, 16 * 16 * 4).empty());
        ensure_contains("short", validateUpload(rgba, 0, 0, { 0, 0, 16, 16 }, tight, 16 * 16 * 4 - 1),
                        "needs 1024");
        PixelLayout rows = tight;
        rows.rowLength = 256; // setSubImage from a wider image
        ensure("row length", validateUpload(rgba, 0, 0, { 0, 0, 16, 16 }, rows, (256 * 15 + 16) * 4).empty());
        ensure_contains("short with row length",
                        validateUpload(rgba, 0, 0, { 0, 0, 16, 16 }, rows, 16 * 16 * 4), "needs");
        rows.rowLength = 8;
        ensure_contains("row shorter than the region", validateUpload(rgba, 0, 0, { 0, 0, 16, 16 }, rows, 1 << 20),
                        "less than the width");
        const PixelLayout rgb = pixelLayoutFromGL(GL_RGB, GL_UNSIGNED_BYTE).value();
        ensure("RGB data", validateUpload(rgba, 0, 0, { 0, 0, 16, 16 }, rgb, 16 * 16 * 3).empty());
        ensure_contains("outside", validateUpload(rgba, 0, 0, { 250, 0, 16, 16 }, tight, 1 << 20), "beyond");

        TextureDesc bc = texture2D(64, 30, Format::BC3_RGBA);
        bc.mipLevels = 2;
        ensure("whole level", validateUpload(bc, 0, 0, { 0, 0, 64, 30 }, tight, 16 * 8 * 16).empty());
        ensure_contains("short BC", validateUpload(bc, 0, 0, { 0, 0, 64, 30 }, tight, 16 * 8 * 16 - 1), "needs");
        ensure("a block", validateUpload(bc, 0, 0, { 4, 8, 4, 4 }, tight, 16).empty());
        ensure_contains("off the blocks", validateUpload(bc, 0, 0, { 2, 0, 4, 4 }, tight, 16), "4x4 blocks");
        ensure_contains("not a whole block", validateUpload(bc, 0, 0, { 0, 0, 6, 4 }, tight, 32), "4x4 blocks");
        ensure("up to the edge", validateUpload(bc, 0, 0, { 60, 28, 4, 2 }, tight, 16).empty());
        ensure("level 1 is 32x15", validateUpload(bc, 1, 0, { 0, 0, 32, 15 }, tight, 8 * 4 * 16).empty());
        TextureDesc dxt1 = texture2D(16, 16, Format::BC1_RGBA);
        ensure("BC1 is 8 bytes a block", validateUpload(dxt1, 0, 0, { 0, 0, 16, 16 }, tight, 16 * 8).empty());

        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Texture> texture = device->createTexture(rgba, "fetched");
        std::vector<std::byte> few(10);
        device->uploadContext().uploadTexture(texture.get(), 0, 0, { 0, 0, 16, 16 }, tight, few);
        ensure_contains("the upload context checks too", device->mUploads.lastError, "needs 1024");
    }

    template<> template<>
    void encoder_object::test<11>()
    {
        set_test_name("pipelines that differ only where it does not matter share a key");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Program> simple = device->createProgram(program("objectSimpleProgram"));
        PipelineDesc opaque = colorPipeline();
        PipelineDesc leftover = opaque;
        leftover.blend.srcColor = BlendFactor::SourceAlpha; // a factor left over with blending off
        ensure("same pipeline without blending",
               device->getPipeline(*simple, opaque).get() == device->getPipeline(*simple, leftover).get());
        leftover.blend.enabled = true;
        ensure("another with blending",
               device->getPipeline(*simple, opaque).get() != device->getPipeline(*simple, leftover).get());

        PipelineDesc shadow;
        shadow.colorCount = 0;
        shadow.depthFormat = Format::Depth32Float;
        PipelineDesc masked = shadow;
        masked.colorMask = COLOR_MASK_NONE;
        ensure("no color mask without color",
               device->getPipeline(*simple, shadow).get() == device->getPipeline(*simple, masked).get());
        ensure("normalized", normalized(masked) == normalized(shadow));
        PipelineDesc blendedShadow = shadow;
        blendedShadow.blend.enabled = true;
        blendedShadow.blend.dstColor = BlendFactor::OneMinusSourceAlpha;
        ensure("no blend without color",
               device->getPipeline(*simple, shadow).get() == device->getPipeline(*simple, blendedShadow).get());
    }

    template<> template<>
    void encoder_object::test<12>()
    {
        set_test_name("clears need what they clear, and a pipeline that fails later stops its draws");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        encoder.clear(true, { 0.f, 0.f, 0.f, 1.f }, false, 1.f);
        ensure_contains("outside a pass", encoder.errors.back(), "no pass is open");

        RenderPassDesc shadow;
        shadow.name = "shadow";
        shadow.depth.texture = device->createTexture(target(512, 512, Format::Depth32Float), "shadow map");
        encoder.beginRenderPass(shadow);
        encoder.clear(false, {}, true, 1.f);
        encoder.clear(true, {}, false, 1.f);
        ensure_contains("no color", encoder.errors.back(), "no color to clear");
        encoder.endRenderPass();

        RenderPassDesc light;
        light.name = "light";
        light.colorCount = 1;
        light.color[0].texture = device->surface()->backBuffer();
        light.depth.texture = device->surface()->backBufferDepth();
        light.depth.readOnly = true;
        encoder.beginRenderPass(light);
        encoder.clear(true, {}, false, 1.f);
        encoder.clear(false, {}, true, 1.f);
        ensure_contains("read-only depth", encoder.errors.back(), "read-only depth");

        Ref<Program> simple = device->createProgram(program("objectSimpleProgram"));
        Ref<Pipeline> pipeline = device->getPipeline(*simple, colorPipeline());
        static_cast<TestPipeline*>(pipeline.get())->mStatus = CompileStatus::Pending;
        encoder.setPipeline(pipeline.get());
        VertexStreams streams;
        streams.attributeMask = 1;
        Ref<Buffer> vertices = device->createBuffer(vertexBuffer(1024), {}, "quad");
        encoder.setVertexBuffer(vertices.get(), streams);
        encoder.setDepthStencilState(device->getDepthStencilState({ true, false, CompareFunc::LessEqual }).get());
        encoder.draw(PrimitiveType::TriangleStrip, 0, 4); // still compiling: the backend waits
        static_cast<TestPipeline*>(pipeline.get())->mStatus = CompileStatus::Failed;
        encoder.draw(PrimitiveType::TriangleStrip, 0, 4);
        ensure_contains("failed since it was set", encoder.errors.back(), "failed to compile");
        encoder.setPipeline(pipeline.get());
        ensure_contains("and cannot be set", encoder.errors.back(), "failed to compile");
        encoder.endRenderPass();
        ensure_equals("one draw ran", std::count(encoder.log.begin(), encoder.log.end(), std::string("draw")), 1);
        ensure_equals("five rejected", encoder.errors.size(), 5u);
    }

    template<> template<>
    void encoder_object::test<13>()
    {
        set_test_name("uniform blocks and texture channels are checked");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        BufferDesc blockDesc = vertexBuffer(4096);
        blockDesc.usage = BUFFER_UNIFORM;
        Ref<Buffer> lights = device->createBuffer(blockDesc, {}, "lights");
        Ref<Buffer> vertices = device->createBuffer(vertexBuffer(4096), {}, "vertices");
        encoder.setUniformBuffer(1, lights.get(), 0, 1024);
        ensure("bound", encoder.errors.empty());
        encoder.setUniformBuffer(1, lights.get(), 3584, 1024);
        ensure_contains("past the end", encoder.errors.back(), "beyond the buffer's 4096");
        encoder.setUniformBuffer(2, vertices.get(), 0, 64);
        ensure_contains("not a uniform buffer", encoder.errors.back(), "not a uniform buffer");
        ensure_equals("the rejected blocks are unbound", encoder.tracker.heldReferences(), 0u);

        Ref<Texture> texture = device->createTexture(texture2D(16, 16), "texture");
        encoder.setTexture(15, texture.get(), SamplerDesc());
        encoder.setTexture(16, texture.get(), SamplerDesc());
        ensure_contains("channel 16", encoder.errors.back(), "beyond the 16 channels");
        ensure_equals("one held", encoder.tracker.heldReferences(), 1u);
    }

    template<> template<>
    void encoder_object::test<14>()
    {
        set_test_name("with checks off the tracker keeps bindings and passes but returns no reasons");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        EncoderTracker tracker(testCapabilities(), false);
        TextureDesc targetOnly = target(64, 64);
        targetOnly.usage = TEXTURE_RENDER_TARGET;
        Ref<Texture> notSampled = device->createTexture(targetOnly, "target only");
        RenderPassDesc broken; // no attachments
        ensure("no reason for a broken pass", tracker.beginRenderPass(broken).empty());
        ensure("in the pass", tracker.inPass());
        ensure("a second begin is still refused", !tracker.beginRenderPass(broken).empty());
        ensure("a draw without a pipeline is still refused", !tracker.draw(PrimitiveType::Triangles).empty());
        Ref<Program> simple = device->createProgram(program("objectSimpleProgram"));
        Ref<Pipeline> hdr = device->getPipeline(*simple, colorPipeline(Format::RGBA16Float));
        ensure("no format check", tracker.setPipeline(hdr.get()).empty());
        ensure("no vertex check", tracker.draw(PrimitiveType::Triangles).empty());
        static_cast<TestPipeline*>(hdr.get())->mStatus = CompileStatus::Failed;
        ensure("a failed pipeline is still refused", !tracker.draw(PrimitiveType::Triangles).empty());
        ensure("no sampling check", tracker.setTexture(0, notSampled.get()).empty());
        ensure("bindings kept", tracker.heldReferences() == 2); // the pipeline and the texture
        ensure("pass ended", tracker.endRenderPass().endPass);
        ensure("a draw outside a pass is still refused", !tracker.draw(PrimitiveType::Triangles).empty());
        tracker = EncoderTracker();
    }

    template<> template<>
    void encoder_object::test<15>()
    {
        set_test_name("frame end closes the pass's groups, the pass, then the groups outside it");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        RenderPassDesc pass;
        pass.name = "UI";
        pass.colorCount = 1;
        pass.color[0].texture = device->surface()->backBuffer();
        encoder.pushDebugGroup("frame");
        encoder.beginRenderPass(pass);
        encoder.pushDebugGroup("floaters");
        device->endFrame();
        ensure("closed in order", encoder.log.size() >= 3
               && encoder.log[encoder.log.size() - 3] == "pop 1"
               && encoder.log[encoder.log.size() - 2] == "end"
               && encoder.log.back() == "pop 1 outside");
        ensure_contains("and warned", encoder.errors.back(), "the frame ended in pass 'UI'");
        ensure("nothing open", !encoder.tracker.inPass());
        encoder.errors.clear();
        encoder.popDebugGroup();
        ensure_contains("the groups are gone", encoder.errors.back(), "no debug group is open");
    }

    template<> template<>
    void encoder_object::test<16>()
    {
        set_test_name("a released texture bound on two channels goes at frame end from both");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        {
            Ref<Texture> material = device->createTexture(texture2D(256, 256), "base color and emissive");
            encoder.setTexture(0, material.get(), SamplerDesc());
            encoder.setTexture(3, material.get(), SamplerDesc());
        }
        ensure_equals("bound twice", encoder.tracker.heldReferences(), 2u);
        device->endFrame();
        ensure_equals("dropped from both", encoder.tracker.heldReferences(), 0u);
        ensure_equals("released in frame 1", device->mLifecycle.pendingReleases(), 1u);

        Ref<Texture> kept = device->createTexture(texture2D(256, 256), "kept");
        encoder.setTexture(0, kept.get(), SamplerDesc());
        encoder.setTexture(3, kept.get(), SamplerDesc());
        device->endFrame();
        ensure_equals("one the facade holds stays on both", encoder.tracker.heldReferences(), 2u);
    }

    template<> template<>
    void encoder_object::test<17>()
    {
        set_test_name("queries need the capability for their kind");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        Ref<Query> metalPrimitives = new TestQuery(QueryType::PrimitivesGenerated);
        ensure("no primitive queries on Metal", !encoder.beginQuery(metalPrimitives.get()));
        ensure_contains("why", encoder.errors.back(), "primitive queries are not supported");
        Capabilities noTimers = testCapabilities();
        noTimers.timerQueries = false;
        EncoderTracker tracker(noTimers);
        Ref<Query> timer = new TestQuery(QueryType::TimeElapsed);
        ensure_contains("no timers", tracker.beginQuery(timer.get()), "timer queries are not supported");
        Capabilities gl = testCapabilities();
        gl.primitivesGeneratedQuery = true;
        EncoderTracker glTracker(gl);
        Ref<Query> primitives = new TestQuery(QueryType::PrimitivesGenerated);
        ensure("primitives on GL", glTracker.beginQuery(primitives.get()).empty());
        ensure("ended", glTracker.endQuery(primitives.get()).empty());
    }

    template<> template<>
    void encoder_object::test<18>()
    {
        set_test_name("A stack pass lets debug groups cross its boundaries and ends with the frame without a warning; "
                      "other passes keep their rules; a begin says whether it began");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        Ref<Texture> color = device->createTexture(target(64, 64), "stack color");
        Ref<Texture> depth = device->createTexture(target(64, 64, Format::Depth32Float), "stack depth");
        // A pass of LLRenderTarget's stack (P2-03b), with a colour and a depth when asked.
        auto stackPass = [&color, &depth](const std::string& name, bool withColor = true, bool withDepth = true)
        {
            RenderPassDesc pass;
            pass.name = name;
            pass.stack = true;
            if (withColor)
            {
                pass.colorCount = 1;
                pass.color[0].texture = color;
            }
            if (withDepth)
            {
                pass.depth.texture = depth;
            }
            return pass;
        };
        // What a pass's end or the frame's asked the backend to close, from the log's last entries.
        auto lastLogged = [&encoder](size_t count)
        {
            std::string out;
            for (size_t i = encoder.log.size() >= count ? encoder.log.size() - count : 0; i < encoder.log.size(); ++i)
            {
                out += (out.empty() ? "" : ", ") + encoder.log[i];
            }
            return out;
        };

        // (a) A stack pass still open at the frame's end ends with it, without a warning.
        ensure("(a) begun", encoder.beginRenderPass(stackPass("window")));
        device->endFrame();
        ensure_equals("(a) the backend ended it", encoder.log.back(), std::string("end"));
        ensure("(a) not open", !encoder.tracker.inPass());
        ensure("(a) no warning", encoder.errors.empty());

        // (b) A group pushed in a stack pass stays open across its end and is popped in the next stack pass.
        encoder.beginRenderPass(stackPass("P1"));
        encoder.pushDebugGroup("zone");
        encoder.endRenderPass();
        ensure_equals("(b) no pop when P1 ends", lastLogged(2), std::string("push, end"));
        ensure("(b) no warning when P1 ends", encoder.errors.empty());
        encoder.beginRenderPass(stackPass("P2"));
        encoder.popDebugGroup();
        ensure_equals("(b) the pop accepted in P2", encoder.log.back(), std::string("pop"));
        ensure("(b) no warning", encoder.errors.empty());
        encoder.endRenderPass();
        device->endFrame();
        ensure("(b) a clean frame", encoder.errors.empty());

        // (c) A group pushed before a stack pass is popped inside it; then none is open, and a group
        // pushed in it after that stays open across its end.
        encoder.pushDebugGroup("around a bind");
        encoder.beginRenderPass(stackPass("P"));
        encoder.popDebugGroup();
        ensure_equals("(c) the pop accepted", encoder.log.back(), std::string("pop"));
        ensure("(c) no warning", encoder.errors.empty());
        encoder.popDebugGroup();
        ensure_contains("(c) a second pop refused", encoder.errors.back(), "no debug group is open");
        encoder.errors.clear();
        encoder.pushDebugGroup("in P");
        encoder.endRenderPass();
        ensure_equals("(c) the pass ended without a pop", lastLogged(2), std::string("push, end"));
        ensure("(c) no warning when P ends", encoder.errors.empty());
        encoder.popDebugGroup();
        ensure_equals("(c) its group popped after P", encoder.log.back(), std::string("pop"));
        device->endFrame();
        ensure("(c) a clean frame", encoder.errors.empty());

        // (d) A group opened in a stack pass and open at the frame's end: the frame's warning, popped outside the pass.
        encoder.beginRenderPass(stackPass("P"));
        encoder.pushDebugGroup("left open");
        device->endFrame();
        ensure_equals("(d) the pass, then the group outside it", lastLogged(2), std::string("end, pop 1 outside"));
        ensure_equals("(d) one warning", encoder.errors.size(), size_t(1));
        ensure_contains("(d) the frame's", encoder.errors.back(), "the frame ended with 1 debug groups open");
        encoder.errors.clear();

        // (e) No pass in a stack pass, and no stack pass in another pass.
        RenderPassDesc ordinary;
        ordinary.name = "ordinary";
        ordinary.colorCount = 1;
        ordinary.color[0].texture = color;
        ensure("(e) the stack pass begun", encoder.beginRenderPass(stackPass("S")));
        ensure("(e) an ordinary pass refused", !encoder.beginRenderPass(ordinary));
        ensure_contains("(e) why", encoder.errors.back(), "pass 'S' is still open");
        ensure_equals("(e) with the recorded call", encoder.errors.back().substr(0, 15), std::string("begin ordinary:"));
        ensure("(e) the stack pass still open", encoder.tracker.inPass() && encoder.tracker.pass().name == "S");
        encoder.endRenderPass();
        ensure("(e) the ordinary pass begun", encoder.beginRenderPass(ordinary));
        ensure("(e) a stack pass refused", !encoder.beginRenderPass(stackPass("S")));
        ensure_contains("(e) why", encoder.errors.back(), "pass 'ordinary' is still open");
        ensure("(e) the ordinary pass still open", encoder.tracker.inPass() && !encoder.tracker.pass().stack);
        encoder.endRenderPass();
        encoder.errors.clear();

        // (f) A stack pass clears what it has, as any pass.
        encoder.beginRenderPass(stackPass("depth only", false, true));
        encoder.clear(true, { 0.f, 0.f, 0.f, 0.f }, false, 1.f);
        ensure_contains("(f) no colour", encoder.errors.back(), "pass 'depth only' has no color to clear");
        encoder.endRenderPass();
        encoder.beginRenderPass(stackPass("color only", true, false));
        encoder.clear(false, { 0.f, 0.f, 0.f, 0.f }, true, 1.f);
        ensure_contains("(f) no depth", encoder.errors.back(), "pass 'color only' has no depth to clear");
        encoder.clear(true, { 0.f, 0.f, 0.f, 0.f }, false, 1.f);
        ensure_equals("(f) its colour cleared", encoder.log.back(), std::string("clear"));
        encoder.endRenderPass();
        RenderPassDesc readOnly = stackPass("read-only depth");
        readOnly.depth.readOnly = true;
        encoder.beginRenderPass(readOnly);
        encoder.clear(false, { 0.f, 0.f, 0.f, 0.f }, true, 1.f);
        ensure_contains("(f) a read-only depth", encoder.errors.back(), "pass 'read-only depth' has a read-only depth");
        encoder.endRenderPass();
        ensure_equals("(f) three refused", encoder.errors.size(), size_t(3));
        encoder.errors.clear();

        // (g) A stack pass is validated as any pass.
        RenderPassDesc missing = stackPass("missing");
        missing.color[0].texture = nullptr;
        ensure("(g) refused", !encoder.beginRenderPass(missing));
        ensure_contains("(g) by validate()", encoder.errors.back(), "color attachment 0 has no texture");
        ensure_equals("(g) with the recorded call", encoder.errors.back().substr(0, 14), std::string("begin missing:"));
        ensure("(g) no pass open", !encoder.tracker.inPass());
        encoder.errors.clear();

        // (h) The tracker holds a stack pass's attachments while it is open, and not after.
        ensure_equals("(h) nothing held before", encoder.tracker.heldReferences(), size_t(0));
        encoder.beginRenderPass(stackPass("held"));
        ensure_equals("(h) its colour and depth", encoder.tracker.heldReferences(), size_t(2));
        encoder.endRenderPass();
        ensure_equals("(h) none after its end", encoder.tracker.heldReferences(), size_t(0));
        encoder.beginRenderPass(stackPass("held to the frame's end"));
        ensure_equals("(h) held again", encoder.tracker.heldReferences(), size_t(2));
        device->endFrame();
        ensure_equals("(h) none after the frame's end", encoder.tracker.heldReferences(), size_t(0));
        ensure("(h) no warning", encoder.errors.empty());

        // (i) test<6>'s sequence on a pass that is not a stack pass gives test<6>'s errors (P2-10's rules).
        RenderPassDesc shadow;
        shadow.name = "shadow";
        shadow.stack = false;
        shadow.depth.texture = depth;
        encoder.pushDebugGroup("outside");
        ensure("(i) begun", encoder.beginRenderPass(shadow));
        encoder.popDebugGroup();
        ensure_contains("(i) pop of a group opened outside", encoder.errors.back(), "opened outside pass 'shadow'");
        encoder.pushDebugGroup("inside");
        encoder.endRenderPass();
        ensure_contains("(i) pass ends with a group open", encoder.errors.back(), "ended with 1 debug groups open");
        ensure_equals("(i) the backend popped the group and ended its native pass", lastLogged(2), std::string("pop 1, end"));
        encoder.popDebugGroup(); // closes "outside"
        ensure_equals("(i) outside closed", encoder.log.back(), std::string("pop"));
        encoder.beginRenderPass(shadow);
        device->endFrame();
        ensure_contains("(i) frame ends in a pass", encoder.errors.back(), "the frame ended in pass 'shadow'");
        ensure_equals("(i) three warnings", encoder.errors.size(), size_t(3));
        encoder.errors.clear();

        // (j) The begin says whether the tracker accepted it: true above, false for (e)'s and (g)'s refusals, each with
        // the recorded reason; and a refused begin leaves the open pass as it was.
        ensure("(j) accepted", encoder.beginRenderPass(stackPass("first")));
        ensure("(j) refused while it is open", !encoder.beginRenderPass(stackPass("second")));
        ensure_contains("(j) why", encoder.errors.back(), "begin second: pass 'first' is still open");
        ensure_equals("(j) the first still open", encoder.tracker.pass().name, std::string("first"));
        encoder.endRenderPass();
        ensure("(j) refused by validate()", !encoder.beginRenderPass(missing));
        ensure_contains("(j) why", encoder.errors.back(), "begin missing: color attachment 0 has no texture");
        ensure_equals("(j) two refused", encoder.errors.size(), size_t(2));
        device->endFrame();
        ensure_equals("(j) a clean frame", encoder.errors.size(), size_t(2));
    }

    template<> template<>
    void encoder_object::test<19>()
    {
        set_test_name("A query of the device's is begun again and again: in one frame and the next, refused while it is "
                      "active, and across endRenderPass, an update inside a pass and a stack pass with no warning (P2-09)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        Ref<Query> samples = device->createQuery(QueryType::SamplesPassed);
        ensure("made", samples.notNull());

        // (a) Begun, ended and begun again in one frame, then in the next.
        ensure("(a) begun", encoder.beginQuery(samples.get()));
        encoder.endQuery(samples.get());
        ensure("(a) begun again", encoder.beginQuery(samples.get()));
        encoder.endQuery(samples.get());
        device->endFrame();
        ensure("(a) begun in the next frame", encoder.beginQuery(samples.get()));
        encoder.endQuery(samples.get());
        device->endFrame();
        ensure("(a) no warning", encoder.errors.empty());

        // (b) A begin of the query while it is active is refused, and the query stays active until its end.
        ensure("(b) begun", encoder.beginQuery(samples.get()));
        ensure("(b) refused while it is active", !encoder.beginQuery(samples.get()));
        ensure_contains("(b) why", encoder.errors.back(), "already active");
        ensure_equals("(b) still active once", encoder.tracker.activeQueries().size(), size_t(1));
        encoder.endQuery(samples.get());
        ensure_equals("(b) one warning", encoder.errors.size(), size_t(1));
        encoder.errors.clear();

        // (c) A samples query and a timer across pass breaks: a pass's end and the next pass's begin, an update inside a
        // pass, and a stack pass.
        Ref<Texture> color = device->createTexture(target(64, 64), "query color");
        Ref<Buffer> buffer = device->createBuffer(vertexBuffer(64), {}, "query buffer");
        RenderPassDesc pass;
        pass.name = "scene";
        pass.colorCount = 1;
        pass.color[0].texture = color;
        RenderPassDesc stack = pass;
        stack.name = "stack";
        stack.stack = true;
        Ref<Query> timer = device->createQuery(QueryType::TimeElapsed);
        ensure("(c) begun", encoder.beginQuery(samples.get()));
        ensure("(c) the timer begun", encoder.beginQuery(timer.get()));
        ensure("(c) the first pass", encoder.beginRenderPass(pass));
        const std::byte bytes[16] = {};
        encoder.updateBuffer(buffer.get(), 0, bytes);
        encoder.endRenderPass();
        ensure("(c) the second pass", encoder.beginRenderPass(pass));
        encoder.endRenderPass();
        ensure("(c) a stack pass", encoder.beginRenderPass(stack));
        encoder.endRenderPass();
        encoder.endQuery(samples.get());
        encoder.endQuery(timer.get());
        device->endFrame();
        ensure("(c) no warning: " + (encoder.errors.empty() ? std::string() : encoder.errors.back()), encoder.errors.empty());
    }

    template<> template<>
    void encoder_object::test<20>()
    {
        set_test_name("validateQuery gives the tracker's reasons for the four capability combinations, and createQueries makes "
                      "none of a kind the capabilities lack (P2-09)");
        const QueryType types[] = { QueryType::AnySamplesPassed, QueryType::SamplesPassed, QueryType::TimeElapsed,
                                    QueryType::PrimitivesGenerated };
        for (int combination = 0; combination < 4; ++combination)
        {
            Capabilities caps = testCapabilities();
            caps.timerQueries = (combination & 1) != 0;
            caps.primitivesGeneratedQuery = (combination & 2) != 0;
            const std::string at = llformat("timers %d, primitives %d, ", int(caps.timerQueries), int(caps.primitivesGeneratedQuery));
            for (QueryType type : types)
            {
                EncoderTracker tracker(caps);
                Ref<Query> query = new TestQuery(type);
                const std::string why = validateQuery(type, caps);
                ensure_equals(at + llformat("type %d: as the tracker", int(type)), why, tracker.beginQuery(query.get()));
                if (type == QueryType::TimeElapsed && !caps.timerQueries)
                {
                    ensure_contains(at + "no timers", why, "timer queries are not supported");
                }
                else if (type == QueryType::PrimitivesGenerated && !caps.primitivesGeneratedQuery)
                {
                    ensure_contains(at + "no primitives", why, "primitive queries are not supported");
                }
                else
                {
                    ensure_equals(at + llformat("type %d: supported", int(type)), why, std::string());
                }
            }
        }

        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native); // without primitive queries, as Metal
        ensure("no primitive queries", device->createQueries(QueryType::PrimitivesGenerated, 4).empty());
        ensure("nor one", device->createQuery(QueryType::PrimitivesGenerated).isNull());
        ensure_equals("samples queries", device->createQueries(QueryType::SamplesPassed, 4).size(), size_t(4));
        ensure("none asked, none made", device->createQueries(QueryType::AnySamplesPassed, 0).empty());
        device->mCaps.timerQueries = false;
        ensure("no timers without the capability", device->createQueries(QueryType::TimeElapsed, 1).empty());
    }

    template<> template<>
    void encoder_object::test<21>()
    {
        set_test_name("queryTypeFromGL gives the kind of the four GL query targets, and nothing for another (P2-09)");
        ensure("GL_ANY_SAMPLES_PASSED", queryTypeFromGL(GL_ANY_SAMPLES_PASSED) == QueryType::AnySamplesPassed);
        ensure("GL_SAMPLES_PASSED", queryTypeFromGL(GL_SAMPLES_PASSED) == QueryType::SamplesPassed);
        ensure("GL_TIME_ELAPSED", queryTypeFromGL(GL_TIME_ELAPSED) == QueryType::TimeElapsed);
        ensure("GL_PRIMITIVES_GENERATED", queryTypeFromGL(GL_PRIMITIVES_GENERATED) == QueryType::PrimitivesGenerated);
        ensure("GL_TIMESTAMP", !queryTypeFromGL(GL_TIMESTAMP).has_value());
        ensure("0", !queryTypeFromGL(0).has_value());
    }

    template<> template<>
    void encoder_object::test<22>()
    {
        set_test_name("With concurrentSampleQueries at least one sample and a sample count overlap in either order, while two "
                      "of one kind still do not; without it the two sample kinds do not overlap (P2-09)");
        // The reason a second begin is refused while the first is active, or an empty string.
        const auto second = [](const Capabilities& caps, QueryType first, QueryType then)
        {
            EncoderTracker tracker(caps);
            Ref<Query> a = new TestQuery(first);
            Ref<Query> b = new TestQuery(then);
            ensure("the first begins", tracker.beginQuery(a.get()).empty());
            return tracker.beginQuery(b.get());
        };
        Capabilities concurrent = testCapabilities();
        concurrent.concurrentSampleQueries = true;
        ensure_equals("concurrent: samples in at least one sample",
                      second(concurrent, QueryType::AnySamplesPassed, QueryType::SamplesPassed), std::string());
        ensure_equals("concurrent: at least one sample in samples",
                      second(concurrent, QueryType::SamplesPassed, QueryType::AnySamplesPassed), std::string());
        ensure_contains("concurrent: two of at least one sample",
                        second(concurrent, QueryType::AnySamplesPassed, QueryType::AnySamplesPassed), "already active");
        ensure_contains("concurrent: two sample counts", second(concurrent, QueryType::SamplesPassed, QueryType::SamplesPassed),
                        "already active");
        ensure_contains("concurrent: two timers", second(concurrent, QueryType::TimeElapsed, QueryType::TimeElapsed), "already active");
        const Capabilities shared = testCapabilities();
        ensure("the test capabilities have no concurrent sample queries", !shared.concurrentSampleQueries);
        ensure_contains("shared: samples in at least one sample", second(shared, QueryType::AnySamplesPassed, QueryType::SamplesPassed),
                        "already active");
        ensure_contains("shared: at least one sample in samples", second(shared, QueryType::SamplesPassed, QueryType::AnySamplesPassed),
                        "already active");
    }

    template<> template<>
    void encoder_object::test<23>()
    {
        set_test_name("state() is the encoder's tracker; passSerial counts the passes that began; passFormatsSerial is 0 "
                      "outside a pass, the same across passes of equal formats, and new for other formats, also when they "
                      "alternate (P2-01b)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        const Encoder& base = encoder;
        ensure("state() is the tracker", &base.state() == &encoder.tracker);
        const EncoderTracker& state = base.state();

        const RenderPassDesc a = colorDepthPass(*device, "A", 64);
        const RenderPassDesc a2 = colorDepthPass(*device, "A2", 32); // A's formats, other textures of another size
        RenderPassDesc stackA = a;
        stackA.name = "stack A";
        stackA.stack = true;
        const RenderPassDesc b = colorDepthPass(*device, "B", 64, Format::RGBA16Float);
        const RenderPassDesc msaa = colorDepthPass(*device, "MSAA", 64, Format::RGBA8Unorm, 4);
        RenderPassDesc missing = a;
        missing.name = "missing";
        missing.color[0].texture = nullptr;

        // (a) Before any pass.
        ensure_equals("(a) no pass yet", state.passSerial(), U64(0));
        ensure_equals("(a) 0 outside a pass", state.passFormatsSerial(), U64(0));
        ensure("(a) no formats outside a pass", state.passFormats() == PassFormats());

        // (b) A pass's serial and formats; a refused begin counts nothing.
        ensure("(b) A begins", encoder.beginRenderPass(a));
        ensure_equals("(b) one pass", state.passSerial(), U64(1));
        const U64 first = state.passFormatsSerial();
        ensure("(b) A has a formats serial", first != 0);
        ensure("(b) A's formats", state.passFormats() == passFormatsOf(a));
        ensure_equals("(b) asked again", state.passFormatsSerial(), first);
        ensure("(b) refused while A is open", !encoder.beginRenderPass(b));
        ensure_equals("(b) a refused begin is no pass", state.passSerial(), U64(1));
        ensure_equals("(b) A's serial stays", state.passFormatsSerial(), first);
        encoder.endRenderPass();
        ensure_equals("(b) 0 after the pass", state.passFormatsSerial(), U64(0));
        ensure("(b) no formats after the pass", state.passFormats() == PassFormats());
        ensure("(b) refused by validate()", !encoder.beginRenderPass(missing));
        ensure_equals("(b) an invalid pass is no pass", state.passSerial(), U64(1));
        ensure_equals("(b) and has no formats serial", state.passFormatsSerial(), U64(0));

        // (c) Equal formats keep the serial: other textures of another size, and a stack pass.
        encoder.beginRenderPass(a2);
        ensure_equals("(c) two passes", state.passSerial(), U64(2));
        ensure_equals("(c) A2 has A's serial", state.passFormatsSerial(), first);
        encoder.endRenderPass();
        encoder.beginRenderPass(stackA);
        ensure_equals("(c) three passes", state.passSerial(), U64(3));
        ensure_equals("(c) the stack pass has A's serial", state.passFormatsSerial(), first);
        encoder.endRenderPass();

        // (d) Other formats get a new serial, also when they alternate (B, A, B), and samples are formats.
        encoder.beginRenderPass(b);
        const U64 second = state.passFormatsSerial();
        ensure("(d) B's serial is new", second != 0 && second != first);
        ensure("(d) B's formats", state.passFormats() == passFormatsOf(b));
        encoder.endRenderPass();
        encoder.beginRenderPass(a);
        const U64 third = state.passFormatsSerial();
        ensure("(d) A after B is new", third != 0 && third != second && third != first);
        ensure("(d) A's formats again", state.passFormats() == passFormatsOf(a));
        encoder.endRenderPass();
        encoder.beginRenderPass(b);
        const U64 fourth = state.passFormatsSerial();
        ensure("(d) B after A is new", fourth != 0 && fourth != third && fourth != second);
        encoder.endRenderPass();
        encoder.beginRenderPass(msaa);
        const U64 fifth = state.passFormatsSerial();
        ensure("(d) 4 samples are other formats", fifth != 0 && fifth != fourth && fifth != third);
        ensure_equals("(d) the samples", state.passFormats().samples, U32(4));
        encoder.endRenderPass();

        // (e) A pass nobody asks about leaves the serial as it was: an A nobody asks about, then MSAA again.
        encoder.beginRenderPass(a);
        encoder.endRenderPass();
        encoder.beginRenderPass(msaa);
        ensure_equals("(e) MSAA's serial, as nobody asked about A", state.passFormatsSerial(), fifth);
        encoder.endRenderPass();
        ensure_equals("(e) every pass that began counts", state.passSerial(), U64(9));

        // (f) The same with checks off.
        EncoderTracker unchecked(testCapabilities(), false);
        ensure("(f) A begins", unchecked.beginRenderPass(a).empty());
        const U64 uncheckedA = unchecked.passFormatsSerial();
        ensure("(f) A has a formats serial", uncheckedA != 0);
        unchecked.endRenderPass();
        ensure("(f) A2 begins", unchecked.beginRenderPass(a2).empty());
        ensure_equals("(f) A2 has A's serial", unchecked.passFormatsSerial(), uncheckedA);
        unchecked.endRenderPass();
        ensure("(f) B begins", unchecked.beginRenderPass(b).empty());
        ensure("(f) B's serial is new", unchecked.passFormatsSerial() != uncheckedA);
        unchecked.endRenderPass();
        ensure_equals("(f) three passes", unchecked.passSerial(), U64(3));

        device->endFrame();
        ensure_equals("only the two refused begins warned", encoder.errors.size(), size_t(2));
    }

    template<> template<>
    void encoder_object::test<24>()
    {
        set_test_name("The draws' check of the pipeline against the pass, kept for a formats serial, gives every verdict "
                      "and reason that checkPipelineForPass and a tracker without that history give, and with checks "
                      "off rejects nothing (P2-01b)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        const RenderPassDesc a = colorDepthPass(*device, "A", 64);
        const RenderPassDesc a2 = colorDepthPass(*device, "A2", 32);
        RenderPassDesc stackA = a;
        stackA.name = "stack A";
        stackA.stack = true;
        const RenderPassDesc b = colorDepthPass(*device, "B", 64, Format::RGBA16Float);
        const RenderPassDesc msaa = colorDepthPass(*device, "MSAA", 64, Format::RGBA8Unorm, 4);
        Ref<Program> shader = device->createProgram(program("shader"));
        shader->wait();
        Ref<Pipeline> fitsA = device->getPipeline(*shader, colorPipeline());
        Ref<Pipeline> fitsB = device->getPipeline(*shader, colorPipeline(Format::RGBA16Float));
        ensure("pipelines", fitsA.notNull() && fitsB.notNull());
        Ref<Buffer> vertices = device->createBuffer(vertexBuffer(1024), {}, "vertices");
        VertexStreams streams;
        streams.attributeMask = 1;
        const std::string aInB = checkPipelineForPass(fitsA->desc(), b);
        const std::string aInMsaa = checkPipelineForPass(fitsA->desc(), msaa);
        const std::string bInA = checkPipelineForPass(fitsB->desc(), a);
        ensure_contains("A's pipeline does not fit B", aInB, "another format");
        ensure_contains("nor MSAA", aInMsaa, "1 samples, the pass 4");
        ensure_contains("B's does not fit A", bInA, "another format");

        for (const bool checks : { true, false })
        {
            const std::string at = checks ? "checks on: " : "checks off: ";
            // The reason with checks on; with them off nothing is rejected.
            const auto expected = [checks](const std::string& why) { return checks ? why : std::string(); };
            // The verdict of a tracker without the history: the pipeline set before the pass, then one draw.
            const auto uncached = [&](Pipeline* pipeline, const RenderPassDesc& pass, PrimitiveType type)
            {
                EncoderTracker fresh(testCapabilities(), checks);
                fresh.setVertexBuffer(vertices.get(), streams);
                fresh.setPipeline(pipeline);
                fresh.beginRenderPass(pass);
                return fresh.draw(type);
            };
            EncoderTracker tracker(testCapabilities(), checks);
            // One draw in the open pass, against the reason expected and the tracker without the history.
            const auto drawIn = [&](const std::string& what, const RenderPassDesc& pass, PrimitiveType type,
                                    const std::string& why)
            {
                const std::string got = tracker.draw(type);
                ensure_equals(at + what, got, why);
                ensure_equals(at + what + ", as without the history", got, uncached(tracker.pipeline(), pass, type));
            };
            ensure(at + "vertices", tracker.setVertexBuffer(vertices.get(), streams).empty());

            // (a) Set outside a pass, then a pass of other formats: rejected with checkPipelineForPass's reason at
            // every draw, and still bound.
            ensure(at + "(a) set outside a pass", tracker.setPipeline(fitsA.get()).empty());
            tracker.beginRenderPass(b);
            drawIn("(a) A's pipeline in B", b, PrimitiveType::Triangles, expected(aInB));
            drawIn("(a) again", b, PrimitiveType::Triangles, expected(aInB));
            ensure(at + "(a) still bound", tracker.pipeline() == fitsA.get());
            tracker.endRenderPass();

            // (b) A pass it fits: accepted, and the draw's other checks still run after the kept verdict.
            tracker.beginRenderPass(a);
            drawIn("(b) A's pipeline in A", a, PrimitiveType::Triangles, "");
            drawIn("(b) again", a, PrimitiveType::TriangleStrip, "");
            const std::string lines = tracker.draw(PrimitiveType::Lines);
            ensure_equals(at + "(b) a line draw, as without the history", lines,
                          uncached(fitsA.get(), a, PrimitiveType::Lines));
            ensure(at + "(b) a line draw: '" + lines + "'",
                   checks ? lines.find("draw with a") != std::string::npos : lines.empty());
            tracker.endRenderPass();

            // (c) The same pipeline bound, passes of other formats: the reasons without the kept verdict.
            tracker.beginRenderPass(b);
            drawIn("(c) B after A", b, PrimitiveType::Triangles, expected(aInB));
            tracker.endRenderPass();
            tracker.beginRenderPass(msaa);
            drawIn("(c) MSAA", msaa, PrimitiveType::Triangles, expected(aInMsaa));
            tracker.endRenderPass();

            // (d) Passes of equal formats keep the verdict: A, A2 and A's stack pass. A pipeline that failed
            // since is rejected all the same.
            tracker.beginRenderPass(a);
            drawIn("(d) A", a, PrimitiveType::Triangles, "");
            tracker.endRenderPass();
            tracker.beginRenderPass(a2);
            drawIn("(d) A2", a2, PrimitiveType::Triangles, "");
            tracker.endRenderPass();
            tracker.beginRenderPass(stackA);
            drawIn("(d) stack A", stackA, PrimitiveType::Triangles, "");
            static_cast<TestPipeline*>(fitsA.get())->mStatus = CompileStatus::Failed;
            ensure_equals(at + "(d) failed since", tracker.draw(PrimitiveType::Triangles),
                          std::string("the pipeline failed to compile"));
            static_cast<TestPipeline*>(fitsA.get())->mStatus = CompileStatus::Ready;
            drawIn("(d) ready again", stackA, PrimitiveType::Triangles, "");
            tracker.endRenderPass();

            // (e) Set in a pass it fits, then a pass of other formats.
            tracker.beginRenderPass(a2);
            ensure(at + "(e) set in A2", tracker.setPipeline(fitsA.get()).empty());
            drawIn("(e) A2", a2, PrimitiveType::Triangles, "");
            tracker.endRenderPass();
            tracker.beginRenderPass(b);
            drawIn("(e) B", b, PrimitiveType::Triangles, expected(aInB));

            // (f) Set in a pass it does not fit: refused and unbound with checks on, as before.
            ensure_equals(at + "(f) A's pipeline set in B", tracker.setPipeline(fitsA.get()), expected(aInB));
            ensure(at + "(f) bound only with checks off", (tracker.pipeline() == fitsA.get()) == !checks);
            drawIn("(f) the draw", b, PrimitiveType::Triangles,
                   checks ? std::string("no pipeline is set") : std::string());

            // (g) B's pipeline set in B; in A it is checked again, and in B again it fits.
            ensure(at + "(g) B's pipeline set in B", tracker.setPipeline(fitsB.get()).empty());
            drawIn("(g) B", b, PrimitiveType::Triangles, "");
            tracker.endRenderPass();
            tracker.beginRenderPass(a);
            drawIn("(g) B's pipeline in A", a, PrimitiveType::Triangles, expected(bInA));
            tracker.endRenderPass();
            tracker.beginRenderPass(b);
            drawIn("(g) B's pipeline in B again", b, PrimitiveType::Triangles, "");
            tracker.endRenderPass();

            // (h) Set outside a pass right after a pass of formats it does not fit, then a pass of those
            // formats: checked at the draw.
            ensure(at + "(h) set outside a pass", tracker.setPipeline(fitsA.get()).empty());
            tracker.beginRenderPass(b);
            drawIn("(h) A's pipeline in B", b, PrimitiveType::Triangles, expected(aInB));
            tracker.endRenderPass();
        }
    }

    template<> template<>
    void encoder_object::test<25>()
    {
        set_test_name("drawBindings and restoreDrawBindings: a round trip leaves every verdict and reason as it was, a "
                      "pipeline that does not fit the open pass is restored bound and rejected at the draw, and endFrame "
                      "after a restore drops what it drops without one (P2-01b)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        EncoderTracker& tracker = encoder.tracker;
        const EncoderTracker& state = encoder.state();
        const RenderPassDesc a = colorDepthPass(*device, "A", 64);
        const RenderPassDesc b = colorDepthPass(*device, "B", 64, Format::RGBA16Float);
        Ref<Program> shader = device->createProgram(program("shader", 0x3));
        shader->wait();
        PipelineDesc twoAttributes = colorPipeline();
        twoAttributes.attributeMask = 0x3;
        Ref<Pipeline> fitsA = device->getPipeline(*shader, twoAttributes);
        PipelineDesc blended = twoAttributes;
        blended.blend.enabled = true;
        Ref<Pipeline> ownA = device->getPipeline(*shader, blended);
        Ref<Pipeline> fitsB = device->getPipeline(*shader, colorPipeline(Format::RGBA16Float));
        ensure("pipelines", fitsA.notNull() && ownA.notNull() && fitsB.notNull());
        Ref<DepthStencilState> writes = device->getDepthStencilState({ true, true, CompareFunc::LessEqual });
        Ref<DepthStencilState> tests = device->getDepthStencilState({ true, false, CompareFunc::Less });
        Ref<Buffer> vertices = device->createBuffer(vertexBuffer(1024), {}, "interface vertices");
        Ref<Buffer> ownVertices = device->createBuffer(vertexBuffer(1024), {}, "own vertices");
        BufferDesc indexDesc = vertexBuffer(12);
        indexDesc.usage = BUFFER_INDEX;
        Ref<Buffer> indices = device->createBuffer(indexDesc, {}, "interface indices");
        Ref<Buffer> ownIndices = device->createBuffer(indexDesc, {}, "own indices");
        VertexStreams streams;
        streams.attributeMask = 0x3;
        streams.offset[1] = 512;
        VertexStreams ownStreams;
        ownStreams.attributeMask = 0x1;
        ownStreams.offset[0] = 256;

        // The verdicts of four draws: by class, and by the indices' range, which depends on the index type.
        const auto verdicts = [&tracker]()
        {
            return std::vector<std::string>{ tracker.draw(PrimitiveType::Triangles),
                                             tracker.draw(PrimitiveType::Lines),
                                             tracker.drawIndexed(PrimitiveType::Triangles, 0, 3),
                                             tracker.drawIndexed(PrimitiveType::Triangles, 0, 6) };
        };

        // (a) The interface's bindings in A and their verdicts; a round trip at once changes none.
        ensure("(a) A", encoder.beginRenderPass(a));
        encoder.setPipeline(fitsA.get());
        encoder.setDepthStencilState(writes.get());
        encoder.setVertexBuffer(vertices.get(), streams);
        encoder.setIndexBuffer(indices.get(), IndexType::U32);
        ensure("(a) bound", encoder.errors.empty());
        const std::vector<std::string> before = verdicts();
        ensure_equals("(a) a triangle draw", before[0], std::string());
        ensure_contains("(a) a line draw", before[1], "draw with a");
        ensure_equals("(a) three U32 indices", before[2], std::string());
        ensure_contains("(a) six U32 indices", before[3], "beyond index buffer 'interface indices'");
        const EncoderTracker::DrawBindings saved = tracker.drawBindings();
        ensure("(a) the pipeline", saved.pipeline.get() == fitsA.get());
        ensure("(a) the depth state", saved.depthStencil.get() == writes.get());
        ensure("(a) the vertices", saved.vertexBuffer.get() == vertices.get());
        ensure("(a) the streams", saved.streams.attributeMask == 0x3 && saved.streams.offset == streams.offset);
        ensure("(a) the indices", saved.indexBuffer.get() == indices.get() && saved.indexType == IndexType::U32);
        tracker.restoreDrawBindings(tracker.drawBindings());
        ensure("(a) a round trip changes no verdict", verdicts() == before);

        // (b) Other bindings, as LLVertexBuffer's draws make them, then the saved set back: every binding and
        // verdict as before.
        encoder.setPipeline(ownA.get());
        encoder.setDepthStencilState(tests.get());
        encoder.setVertexBuffer(ownVertices.get(), ownStreams);
        encoder.setIndexBuffer(ownIndices.get(), IndexType::U16);
        ensure("(b) other verdicts", verdicts() != before);
        tracker.restoreDrawBindings(saved);
        ensure("(b) the pipeline back", state.pipeline() == fitsA.get());
        ensure("(b) the depth state back", state.depthStencilState() == writes.get());
        ensure("(b) the vertices back", state.vertexBuffer() == vertices.get());
        ensure("(b) the streams back",
               state.streams().attributeMask == 0x3 && state.streams().offset == streams.offset);
        ensure("(b) the indices back", state.indexBuffer() == indices.get() && state.indexType() == IndexType::U32);
        ensure("(b) every verdict as before", verdicts() == before);
        encoder.endRenderPass();

        // (c) A pipeline that does not fit the open pass is restored bound, though the pipeline it replaces was
        // accepted for this pass's formats, and the draw rejects it with checkPipelineForPass's reason. Through
        // setPipeline it would be unbound.
        ensure("(c) B", encoder.beginRenderPass(b));
        encoder.setPipeline(fitsB.get());
        encoder.setVertexBuffer(vertices.get(), streams);
        ensure_equals("(c) B's pipeline draws in B", tracker.draw(PrimitiveType::Triangles), std::string());
        tracker.restoreDrawBindings(saved);
        ensure("(c) restored bound", state.pipeline() == fitsA.get());
        const std::string aInB = checkPipelineForPass(fitsA->desc(), b);
        ensure_contains("(c) A's pipeline does not fit B", aInB, "another format");
        ensure_equals("(c) rejected at the draw", tracker.draw(PrimitiveType::Triangles), aInB);
        ensure_equals("(c) and at the next", tracker.draw(PrimitiveType::Triangles), aInB);
        ensure("(c) still bound", state.pipeline() == fitsA.get());
        ensure("(c) no warning so far", encoder.errors.empty());
        encoder.setPipeline(fitsA.get());
        ensure_equals("(c) through setPipeline: refused", encoder.errors.back(), "pipeline: " + aInB);
        ensure("(c) and unbound", state.pipeline() == nullptr);
        encoder.endRenderPass();
        encoder.errors.clear();

        // (d) endFrame after a restore drops exactly what it drops without the round trip. The pipeline and the
        // depth state stay (the device's caches hold them), and so do the indices (the test holds them); the
        // vertices only the tracker holds go.
        struct Held
        {
            size_t before = 0;
            size_t after = 0;
            bool pipeline = false;
            bool depthStencil = false;
            bool vertices = false;
            bool indices = false;
            bool operator==(const Held&) const = default;
        };
        const auto frame = [&](bool roundTrip)
        {
            EncoderTracker local(testCapabilities());
            Ref<Buffer> only = device->createBuffer(vertexBuffer(1024), {}, "only the tracker's");
            local.beginRenderPass(a);
            local.setPipeline(fitsA.get());
            local.setDepthStencilState(writes.get());
            local.setVertexBuffer(only.get(), streams);
            local.setIndexBuffer(indices.get(), IndexType::U16);
            local.endRenderPass();
            if (roundTrip)
            {
                // Kept apart while other bindings are made, then moved back.
                EncoderTracker::DrawBindings apart = local.drawBindings();
                local.setPipeline(ownA.get());
                local.setDepthStencilState(tests.get());
                local.setVertexBuffer(ownVertices.get(), ownStreams);
                local.setIndexBuffer(ownIndices.get(), IndexType::U32);
                local.restoreDrawBindings(std::move(apart));
            }
            only = nullptr;
            Held held;
            held.before = local.heldReferences();
            local.endFrame();
            held.after = local.heldReferences();
            held.pipeline = local.pipeline() != nullptr;
            held.depthStencil = local.depthStencilState() != nullptr;
            held.vertices = local.vertexBuffer() != nullptr;
            held.indices = local.indexBuffer() != nullptr;
            return held;
        };
        const Held without = frame(false);
        const Held with = frame(true);
        ensure("(d) the vertices go, the rest stays",
               without.pipeline && without.depthStencil && !without.vertices && without.indices);
        ensure_equals("(d) four held before the end", without.before, size_t(4));
        ensure_equals("(d) three after", without.after, size_t(3));
        ensure("(d) the same after a round trip", with == without);

        device->endFrame();
        ensure("no warning", encoder.errors.empty());
    }

    template<> template<>
    void encoder_object::test<26>()
    {
        set_test_name("passFormatsOf and applyPassFormats give pipeline descriptions that validate and fit the pass, for 0 "
                      "to 8 colors, with and without depth, and with samples above 1 (P2-01b)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        const Format colors[PipelineDesc::MAX_COLOR_ATTACHMENTS] = {
            Format::RGBA8Unorm, Format::RGBA16Float, Format::RGB10A2Unorm, Format::R8Unorm,
            Format::RG16Float, Format::RGBA8Unorm_sRGB, Format::R32Float, Format::RG11B10Float };
        int cases = 0;
        for (const U32 samples : { 1u, 4u })
        {
            for (U32 count = 0; count <= PipelineDesc::MAX_COLOR_ATTACHMENTS; ++count)
            {
                for (const bool depth : { false, true })
                {
                    if (count == 0 && !depth)
                    {
                        continue; // not a pass
                    }
                    const std::string at = llformat("%u samples, %u colors, %s depth: ", samples, count,
                                                    depth ? "with" : "without");
                    RenderPassDesc pass;
                    pass.colorCount = count;
                    for (U32 i = 0; i < count; ++i)
                    {
                        pass.color[i].texture = device->createTexture(target(16, 16, colors[i], samples), "color");
                    }
                    if (depth)
                    {
                        pass.depth.texture = device->createTexture(target(16, 16, Format::Depth32Float, samples),
                                                                   "depth");
                    }
                    const std::string validPass = validate(pass);
                    ensure(at + "the pass is valid: " + validPass, validPass.empty());
                    const PassFormats formats = passFormatsOf(pass);
                    ensure_equals(at + "color count", formats.colorCount, count);
                    for (U32 i = 0; i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
                    {
                        ensure_equals(at + llformat("color %u", i), int(formats.colorFormats[i]),
                                      int(i < count ? colors[i] : Format::Unknown));
                    }
                    ensure_equals(at + "depth", int(formats.depthFormat),
                                  int(depth ? Format::Depth32Float : Format::Unknown));
                    ensure_equals(at + "samples", formats.samples, samples);
                    PipelineDesc pipeline;
                    pipeline.attributeMask = 1;
                    applyPassFormats(formats, pipeline);
                    const std::string validPipeline = validate(pipeline, device->capabilities());
                    ensure(at + "the pipeline is valid: " + validPipeline, validPipeline.empty());
                    const std::string fits = checkPipelineForPass(pipeline, pass);
                    ensure(at + "and fits the pass: " + fits, fits.empty());
                    ensure_equals(at + "the pipeline's samples", pipeline.samples, samples);
                    ++cases;
                }
            }
        }
        ensure_equals("cases", cases, 34);

        // A pass without a texture has no samples, and the pipeline gets 1.
        RenderPassDesc bare;
        bare.colorCount = 2;
        const PassFormats none = passFormatsOf(bare);
        ensure("no texture: unknown formats", none.colorCount == 2 && none.colorFormats[0] == Format::Unknown
                                              && none.colorFormats[1] == Format::Unknown
                                              && none.depthFormat == Format::Unknown);
        ensure_equals("no texture: no samples", none.samples, U32(0));
        PipelineDesc pipeline;
        pipeline.samples = 4;
        applyPassFormats(none, pipeline);
        ensure_equals("1 sample when the formats have none", pipeline.samples, U32(1));
        ensure_equals("the color count", pipeline.colorCount, U32(2));

        // The formats are all checkPipelineForPass reads: the size, the name, a stack pass and a read-only depth
        // change nothing, and neither does a texture past colorCount; a format or the samples do.
        const RenderPassDesc big = colorDepthPass(*device, "big", 64);
        RenderPassDesc other = colorDepthPass(*device, "small", 8);
        other.stack = true;
        other.depth.readOnly = true;
        other.color[1].texture = device->createTexture(target(8, 8), "past colorCount");
        ensure("equal formats", passFormatsOf(big) == passFormatsOf(other));
        ensure("another format",
               passFormatsOf(big) != passFormatsOf(colorDepthPass(*device, "half", 64, Format::RGBA16Float)));
        ensure("other samples",
               passFormatsOf(big) != passFormatsOf(colorDepthPass(*device, "msaa", 64, Format::RGBA8Unorm, 4)));
    }

    template<> template<>
    void encoder_object::test<27>()
    {
        set_test_name("compareFuncFromGL is the inverse of the GL backend's compareGL for the eight functions, and gives "
                      "nothing for another enum (P2-01b)");
        // compareGL (llrhigl.cpp), written out, because this test does not link the GL backend.
        const std::pair<CompareFunc, U32> compareGL[] = {
            { CompareFunc::Never, GL_NEVER },         { CompareFunc::AlwaysPass, GL_ALWAYS },
            { CompareFunc::Less, GL_LESS },           { CompareFunc::LessEqual, GL_LEQUAL },
            { CompareFunc::Equal, GL_EQUAL },         { CompareFunc::NotEqual, GL_NOTEQUAL },
            { CompareFunc::GreaterEqual, GL_GEQUAL }, { CompareFunc::Greater, GL_GREATER } };
        std::set<int> functions;
        for (const auto& [func, gl] : compareGL)
        {
            const std::optional<CompareFunc> back = compareFuncFromGL(gl);
            ensure(llformat("0x%x has a function", gl), back.has_value());
            ensure_equals(llformat("0x%x's function", gl), int(*back), int(func));
            functions.insert(int(func));
        }
        ensure_equals("all eight functions", functions.size(), size_t(8));
        for (const U32 gl : { U32(0), U32(GL_NEVER - 1), U32(GL_ALWAYS + 1), U32(GL_DEPTH_TEST), U32(GL_UNSIGNED_INT) })
        {
            ensure(llformat("0x%x has no function", gl), !compareFuncFromGL(gl).has_value());
        }
    }

    template<> template<>
    void encoder_object::test<28>()
    {
        set_test_name("indexTypeFromGL gives the type of GL_UNSIGNED_SHORT and GL_UNSIGNED_INT indices, and nothing for "
                      "another type (P2-01b)");
        ensure("GL_UNSIGNED_SHORT", indexTypeFromGL(GL_UNSIGNED_SHORT) == IndexType::U16);
        ensure("GL_UNSIGNED_INT", indexTypeFromGL(GL_UNSIGNED_INT) == IndexType::U32);
        ensure("GL_UNSIGNED_BYTE", !indexTypeFromGL(GL_UNSIGNED_BYTE).has_value());
        ensure("GL_INT", !indexTypeFromGL(GL_INT).has_value());
        ensure("0", !indexTypeFromGL(0).has_value());
    }

    template<> template<>
    void encoder_object::test<29>()
    {
        set_test_name("EncoderTracker::transfers counts the readbacks and copies it accepts, +1 each and +0 for a refused "
                      "one, with checks on and off (P2-08)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TextureDesc readable = texture2D(64, 64);
        readable.usage = TEXTURE_SAMPLED | TEXTURE_READBACK;
        Ref<Texture> source = device->createTexture(readable, "readable");
        Ref<Texture> sampled = device->createTexture(texture2D(64, 64), "sampled only");
        Ref<Texture> destination = device->createTexture(texture2D(32, 32), "destination");
        Ref<Buffer> buffer = device->createBuffer(vertexBuffer(256), {}, "buffer");
        for (bool checks : { true, false })
        {
            const std::string at = checks ? "checks on: " : "checks off: ";
            EncoderTracker tracker(testCapabilities(), checks);
            ensure_equals(at + "none at first", tracker.transfers(), U64(0));
            ensure(at + "a texture readback", tracker.readback(source.get(), 0, 0, { 0, 0, 64, 64 }).empty());
            ensure_equals(at + "a texture readback: +1", tracker.transfers(), U64(1));
            ensure(at + "a buffer readback", tracker.readback(buffer.get(), 0, 256).empty());
            ensure_equals(at + "a buffer readback: +1", tracker.transfers(), U64(2));
            ensure(at + "a copy",
                   tracker.copyTexture(source.get(), 0, 0, { 0, 0, 32, 32 }, destination.get(), 0, 0, 0, 0).empty());
            ensure_equals(at + "a copy: +1", tracker.transfers(), U64(3));
            const U64 accepted = tracker.transfers();
            // Refused with checks on only: with them off the tracker accepts and counts them.
            const bool noReadback = tracker.readback(sampled.get(), 0, 0, { 0, 0, 64, 64 }).empty();
            const bool outsideReadback = tracker.readback(source.get(), 0, 0, { 32, 32, 64, 64 }).empty();
            const bool outsideBuffer = tracker.readback(buffer.get(), 128, 256).empty();
            const bool outsideCopy =
                tracker.copyTexture(source.get(), 0, 0, { 0, 0, 64, 64 }, destination.get(), 0, 0, 0, 0).empty();
            const bool noTexture = tracker.copyTexture(nullptr, 0, 0, { 0, 0, 4, 4 }, destination.get(), 0, 0, 0, 0).empty();
            ensure_equals(at + "without TEXTURE_READBACK", noReadback, !checks);
            ensure_equals(at + "a readback outside the texture", outsideReadback, !checks);
            ensure_equals(at + "a buffer readback outside the buffer", outsideBuffer, !checks);
            ensure_equals(at + "a copy outside the destination", outsideCopy, !checks);
            ensure_equals(at + "a copy without a source", noTexture, !checks);
            ensure_equals(at + "the refused ones: +0, the accepted: +1 each", tracker.transfers(),
                          accepted + (checks ? 0 : 5));
            // Other calls do not count.
            tracker.uploadTexture(destination.get(), 0, 0, { 0, 0, 4, 4 }, PixelLayout(), 64);
            tracker.generateMipmaps(source.get());
            ensure_equals(at + "uploads and mipmaps: +0", tracker.transfers(), accepted + (checks ? 0 : 5));
        }
    }

    /// An upscaler of a description that needs sampled input and a render target output, for the tracker's rules.
    class TestUpscaler : public Upscaler
    {
    public:
        explicit TestUpscaler(const UpscalerDesc& desc) : mDesc(desc) {}
        const UpscalerDesc& desc() const override { return mDesc; }
        U8 inputUsage() const override { return TEXTURE_SAMPLED; }
        U8 outputUsage() const override { return TEXTURE_RENDER_TARGET; }

    private:
        const UpscalerDesc mDesc;
    };

    template<> template<>
    void encoder_object::test<30>()
    {
        set_test_name("Upscalers: validate(UpscalerDesc), the default device and encoder refuse, and EncoderTracker::upscale "
                      "takes the upscaler's textures outside passes and in a stack pass, refusing a compute pass and other "
                      "passes with checks on and off");
        Capabilities caps = testCapabilities();
        UpscalerDesc desc;
        desc.inputWidth = 640;
        desc.inputHeight = 360;
        desc.outputWidth = 1280;
        desc.outputHeight = 720;
        ensure("no spatial upscaler: refused", !validate(desc, caps).empty());
        caps.spatialUpscaler = true;
        ensure_equals("2x", validate(desc, caps), std::string());
        UpscalerDesc same = desc;
        same.outputWidth = 640;
        same.outputHeight = 360;
        ensure_equals("1x", validate(same, caps), std::string());
        UpscalerDesc smaller = desc;
        smaller.outputWidth = 320;
        ensure("an output narrower than the input", !validate(smaller, caps).empty());
        UpscalerDesc empty = desc;
        empty.inputHeight = 0;
        ensure("a size of 0", !validate(empty, caps).empty());
        UpscalerDesc large = desc;
        large.outputWidth = 20000;
        ensure("larger than the largest texture", !validate(large, caps).empty());
        for (Format format : { Format::Unknown, Format::BGRA8Unorm, Format::BC7_RGBA, Format::Depth32Float, Format::R32Uint })
        {
            UpscalerDesc wrong = desc;
            wrong.outputFormat = format;
            ensure("format " + std::to_string(U32(format)) + " refused", !validate(wrong, caps).empty());
        }
        UpscalerDesc half = desc;
        half.inputFormat = half.outputFormat = Format::RGBA16Float;
        half.color = UpscaleColor::HDR;
        ensure_equals("RGBA16Float, HDR", validate(half, caps), std::string());

        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        {
            CaptureLog log(LLError::LEVEL_WARN);
            ensure("the default createUpscaler gives none", device->createUpscaler(desc).isNull());
            log.messageWith("the spatial upscaler needs Metal");
        }
        Ref<Upscaler> upscaler = new TestUpscaler(desc);
        Ref<Texture> input = device->createTexture(texture2D(640, 360), "input");
        Ref<Texture> output = device->createTexture(target(1280, 720), "output");
        Ref<Texture> wrongSize = device->createTexture(target(1280, 640), "wrong size");
        Ref<Texture> wrongFormat = device->createTexture(target(1280, 720, Format::RGBA16Float), "wrong format");
        Ref<Texture> notTarget = device->createTexture(texture2D(1280, 720), "not a target");
        for (bool checks : { true, false })
        {
            const std::string at = checks ? "checks on: " : "checks off: ";
            EncoderTracker tracker(caps, checks);
            ensure_equals(at + "outside passes", tracker.upscale(upscaler.get(), input.get(), 640, 360, output.get()),
                          std::string());
            ensure_equals(at + "part of the input", tracker.upscale(upscaler.get(), input.get(), 320, 200, output.get()),
                          std::string());
            ensure_equals(at + "no upscaler", tracker.upscale(nullptr, input.get(), 640, 360, output.get()).empty(), !checks);
            ensure_equals(at + "the output of another size",
                          tracker.upscale(upscaler.get(), input.get(), 640, 360, wrongSize.get()).empty(), !checks);
            ensure_equals(at + "the output of another format",
                          tracker.upscale(upscaler.get(), input.get(), 640, 360, wrongFormat.get()).empty(), !checks);
            ensure_equals(at + "an output without the upscaler's usage",
                          tracker.upscale(upscaler.get(), input.get(), 640, 360, notTarget.get()).empty(), !checks);
            ensure_equals(at + "the input as the output",
                          tracker.upscale(upscaler.get(), input.get(), 640, 360, input.get()).empty(), !checks);
            ensure_equals(at + "more content than the input",
                          tracker.upscale(upscaler.get(), input.get(), 641, 360, output.get()).empty(), !checks);

            RenderPassDesc stack = colorDepthPass(*device, "stack", 64);
            stack.stack = true;
            ensure(at + "a stack pass begins", tracker.beginRenderPass(stack).empty());
            ensure_equals(at + "in a stack pass", tracker.upscale(upscaler.get(), input.get(), 640, 360, output.get()),
                          std::string());
            tracker.endRenderPass();
            RenderPassDesc ordinary = colorDepthPass(*device, "ordinary", 64);
            ensure(at + "an ordinary pass begins", tracker.beginRenderPass(ordinary).empty());
            ensure(at + "refused in an ordinary pass",
                   !tracker.upscale(upscaler.get(), input.get(), 640, 360, output.get()).empty());
            tracker.endRenderPass();
        }
        Capabilities compute = caps;
        compute.computePasses = true;
        compute.nativePrograms = true;
        for (bool checks : { true, false })
        {
            EncoderTracker tracker(compute, checks);
            ensure("a compute pass begins", tracker.beginComputePass({ "compute" }).empty());
            ensure(std::string(checks ? "checks on" : "checks off") + ": refused in a compute pass",
                   !tracker.upscale(upscaler.get(), input.get(), 640, 360, output.get()).empty());
            tracker.endComputePass();
        }
        {
            CaptureLog log(LLError::LEVEL_WARN);
            ensure("the default encoder refuses", !device->encoder().upscale(upscaler.get(), input.get(), 640, 360, output.get()));
            log.messageWith("upscale: the spatial upscaler needs Metal");
        }
    }

    /// A temporal upscaler whose inputs must be sampled and whose output is a render target, for the tracker's rules.
    class TestTemporalUpscaler : public TemporalUpscaler
    {
    public:
        explicit TestTemporalUpscaler(const TemporalUpscalerDesc& desc) : mDesc(desc) {}
        const TemporalUpscalerDesc& desc() const override { return mDesc; }
        U8 colorUsage() const override { return TEXTURE_SAMPLED; }
        U8 depthUsage() const override { return TEXTURE_SAMPLED; }
        U8 motionUsage() const override { return TEXTURE_SAMPLED; }
        U8 reactiveUsage() const override { return TEXTURE_SAMPLED; }
        U8 outputUsage() const override { return TEXTURE_RENDER_TARGET; }

    private:
        const TemporalUpscalerDesc mDesc;
    };

    template<> template<>
    void encoder_object::test<31>()
    {
        set_test_name("Temporal upscalers: validate(TemporalUpscalerDesc) with the device's scales, the default device and "
                      "encoder refuse, and EncoderTracker::upscaleTemporal takes the upscaler's textures outside passes and "
                      "in a stack pass, refusing other passes, a compute pass, a missing or extra mask, the output as an "
                      "input and a jitter past a texel");
        Capabilities caps = testCapabilities();
        TemporalUpscalerDesc desc;
        desc.reactiveFormat = Format::R8Unorm;
        desc.inputWidth = 640;
        desc.inputHeight = 360;
        desc.outputWidth = 1280;
        desc.outputHeight = 720;
        ensure("no temporal upscaler: refused", !validate(desc, caps).empty());
        caps.temporalUpscaler = true;
        caps.temporalMinScale = 1.f;
        caps.temporalMaxScale = 3.f;
        ensure_equals("2x", validate(desc, caps), std::string());
        TemporalUpscalerDesc quarter = desc;
        quarter.inputWidth = 320;
        quarter.inputHeight = 180;
        ensure("4x beyond the device's 3x", !validate(quarter, caps).empty());
        TemporalUpscalerDesc smaller = desc;
        smaller.outputWidth = 320;
        ensure("an output narrower than the input", !validate(smaller, caps).empty());
        TemporalUpscalerDesc depth = desc;
        depth.depthFormat = Format::R32Float;
        ensure("a depth format that is not Depth32Float", !validate(depth, caps).empty());
        TemporalUpscalerDesc motion = desc;
        motion.motionFormat = Format::RGBA16Float;
        ensure("a motion format of four channels", !validate(motion, caps).empty());
        TemporalUpscalerDesc reactive = desc;
        reactive.reactiveFormat = Format::RGBA8Unorm;
        ensure("a reactive format of four channels", !validate(reactive, caps).empty());
        TemporalUpscalerDesc unmasked = desc;
        unmasked.reactiveFormat = Format::Unknown;
        ensure_equals("no reactive mask", validate(unmasked, caps), std::string());

        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        {
            CaptureLog log(LLError::LEVEL_WARN);
            ensure("the default createTemporalUpscaler gives none", device->createTemporalUpscaler(desc).isNull());
            log.messageWith("the temporal upscaler needs Metal");
        }
        Ref<TemporalUpscaler> upscaler = new TestTemporalUpscaler(desc);
        Ref<Texture> color = device->createTexture(texture2D(640, 360), "colour");
        Ref<Texture> depthMap = device->createTexture(texture2D(640, 360, Format::Depth32Float), "depth");
        Ref<Texture> motionMap = device->createTexture(texture2D(640, 360, Format::RG16Float), "motion");
        Ref<Texture> mask = device->createTexture(texture2D(640, 360, Format::R8Unorm), "mask");
        Ref<Texture> output = device->createTexture(target(1280, 720), "output");
        Ref<Texture> wrongSize = device->createTexture(texture2D(320, 360, Format::RG16Float), "motion of another size");
        // A 1x upscaler, whose colour and output have the same size, format and usages, so a target can be both.
        TemporalUpscalerDesc same = desc;
        same.outputWidth = 640;
        same.outputHeight = 360;
        ensure_equals("1x", validate(same, caps), std::string());
        Ref<TemporalUpscaler> sameSize = new TestTemporalUpscaler(same);
        Ref<Texture> both = device->createTexture(target(640, 360), "colour and output");
        Ref<Texture> sameOutput = device->createTexture(target(640, 360), "1x output");
        Ref<TemporalUpscaler> maskless = new TestTemporalUpscaler(unmasked);
        TemporalUpscaleInputs inputs;
        inputs.color = color.get();
        inputs.depth = depthMap.get();
        inputs.motion = motionMap.get();
        inputs.reactive = mask.get();
        inputs.output = output.get();
        inputs.width = 640;
        inputs.height = 360;
        inputs.jitterX = 0.25f;
        inputs.jitterY = -0.4f;
        for (bool checks : { true, false })
        {
            const std::string at = checks ? "checks on: " : "checks off: ";
            EncoderTracker tracker(caps, checks);
            ensure_equals(at + "outside passes", tracker.upscaleTemporal(upscaler.get(), inputs), std::string());
            TemporalUpscaleInputs unmaskedInputs = inputs;
            unmaskedInputs.reactive = nullptr;
            ensure_equals(at + "no mask for an upscaler with one",
                          tracker.upscaleTemporal(upscaler.get(), unmaskedInputs).empty(), !checks);
            TemporalUpscaleInputs sized = inputs;
            sized.motion = wrongSize.get();
            ensure_equals(at + "motion of another size", tracker.upscaleTemporal(upscaler.get(), sized).empty(), !checks);
            TemporalUpscaleInputs far = inputs;
            far.jitterX = 1.5f;
            ensure_equals(at + "a jitter past a texel", tracker.upscaleTemporal(upscaler.get(), far).empty(), !checks);
            TemporalUpscaleInputs self = inputs;
            self.output = color.get();
            ensure_equals(at + "the colour as a 2x upscaler's output, of the input's size",
                          tracker.upscaleTemporal(upscaler.get(), self).empty(), !checks);
            // The 1x upscaler's checks of size, format and usage all pass a target that is colour and output, so only
            // the identity check refuses it.
            TemporalUpscaleInputs selfSame = inputs;
            selfSame.color = both.get();
            selfSame.output = both.get();
            const std::string selfWhy = tracker.upscaleTemporal(sameSize.get(), selfSame);
            ensure(at + "the colour as the output: " + selfWhy,
                   checks ? selfWhy.find("the output is one of the inputs") != std::string::npos : selfWhy.empty());
            TemporalUpscaleInputs ownSame = selfSame;
            ownSame.output = sameOutput.get();
            ensure_equals(at + "a 1x upscaler with an output of its own", tracker.upscaleTemporal(sameSize.get(), ownSame),
                          std::string());
            const std::string extraWhy = tracker.upscaleTemporal(maskless.get(), inputs);
            ensure(at + "a mask for an upscaler made without one: " + extraWhy,
                   checks ? extraWhy.find("a reactive mask for an upscaler made without one") != std::string::npos
                          : extraWhy.empty());
            ensure_equals(at + "no mask for an upscaler made without one",
                          tracker.upscaleTemporal(maskless.get(), unmaskedInputs), std::string());

            RenderPassDesc stack = colorDepthPass(*device, "stack", 64);
            stack.stack = true;
            ensure(at + "a stack pass begins", tracker.beginRenderPass(stack).empty());
            ensure_equals(at + "in a stack pass", tracker.upscaleTemporal(upscaler.get(), inputs), std::string());
            tracker.endRenderPass();
            RenderPassDesc ordinary = colorDepthPass(*device, "ordinary", 64);
            ensure(at + "an ordinary pass begins", tracker.beginRenderPass(ordinary).empty());
            ensure(at + "refused in an ordinary pass", !tracker.upscaleTemporal(upscaler.get(), inputs).empty());
            tracker.endRenderPass();
        }
        Capabilities compute = caps;
        compute.computePasses = true;
        compute.nativePrograms = true;
        for (bool checks : { true, false })
        {
            EncoderTracker tracker(compute, checks);
            ensure("a compute pass begins", tracker.beginComputePass({ "compute" }).empty());
            ensure(std::string(checks ? "checks on" : "checks off") + ": refused in a compute pass",
                   !tracker.upscaleTemporal(upscaler.get(), inputs).empty());
            tracker.endComputePass();
        }
        {
            CaptureLog log(LLError::LEVEL_WARN);
            ensure("the default encoder refuses", !device->encoder().upscaleTemporal(upscaler.get(), inputs));
            log.messageWith("upscaleTemporal: the temporal upscaler needs Metal");
        }
    }

    template<> template<>
    void encoder_object::test<32>()
    {
        set_test_name("with draw checks off the tracker skips the pipeline's formats and primitive class, the vertex and index "
                      "buffers' usage and the texture checks, and still refuses depth writes in a read-only pass, an "
                      "indexed draw without its index buffer or past its end, and what the other checks refuse");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        EncoderTracker tracker(testCapabilities(), true, false);
        RenderPassDesc pass = colorDepthPass(*device, "pass", 64);
        ensure("the pass begins", tracker.beginRenderPass(pass).empty());
        Ref<Program> simple = device->createProgram(program("objectSimpleProgram"));
        Ref<Pipeline> hdr = device->getPipeline(*simple, colorPipeline(Format::RGBA16Float));
        ensure("no format check when set", tracker.setPipeline(hdr.get()).empty());
        ensure("bound", tracker.pipeline() == hdr.get());
        ensure("no format or vertex buffer check at the draw", tracker.draw(PrimitiveType::Triangles).empty());
        ensure("no primitive class check", tracker.draw(PrimitiveType::Points).empty());
        BufferDesc indexDesc = vertexBuffer(64);
        indexDesc.usage = BUFFER_INDEX;
        Ref<Buffer> indices = device->createBuffer(indexDesc, {}, "indices");
        Ref<Buffer> vertices = device->createBuffer(vertexBuffer(64), {}, "vertices");
        VertexStreams beyond;
        beyond.attributeMask = 1;
        beyond.offset[0] = 128;
        ensure("no vertex usage or stream check", tracker.setVertexBuffer(indices.get(), beyond).empty());
        ensure("the vertex buffer bound", tracker.vertexBuffer() == indices.get());
        ensure_contains("an indexed draw without an index buffer is still refused",
                        tracker.drawIndexed(PrimitiveType::Triangles, 0, 3), "no index buffer is bound");
        ensure("no index usage check", tracker.setIndexBuffer(vertices.get(), IndexType::U16).empty());
        ensure_contains("indices past the buffer are still refused", tracker.drawIndexed(PrimitiveType::Triangles, 30, 4),
                        "are beyond index buffer 'vertices'");
        ensure("indices within the buffer", tracker.drawIndexed(PrimitiveType::Triangles, 0, 32).empty());
        TextureDesc targetOnly = target(64, 64);
        targetOnly.usage = TEXTURE_RENDER_TARGET;
        Ref<Texture> notSampled = device->createTexture(targetOnly, "target only");
        ensure("no sampling check", tracker.setTexture(0, notSampled.get()).empty());
        ensure("no channel range check", tracker.setTexture(1000, notSampled.get()).empty());
        ensure("an update past the buffer is still refused", !tracker.updateBuffer(vertices.get(), 60, 8).empty());
        ensure("the uniform buffer's usage is still checked", !tracker.setUniformBuffer(0, vertices.get(), 0, 16).empty());
        ensure("the pass ends", tracker.endRenderPass().endPass);
        RenderPassDesc readOnly = pass;
        readOnly.depth.readOnly = true;
        ensure("the read-only pass begins", tracker.beginRenderPass(readOnly).empty());
        ensure_contains("a clear of the read-only depth is still refused", tracker.clear(false, true), "read-only depth");
        Ref<DepthStencilState> writes = device->getDepthStencilState({ true, true, CompareFunc::LessEqual });
        tracker.setDepthStencilState(writes.get());
        ensure("set in the read-only pass", tracker.setPipeline(hdr.get()).empty());
        ensure_contains("depth writes in a read-only pass are still refused", tracker.draw(PrimitiveType::Triangles),
                        "has a read-only depth");
        tracker.endRenderPass();
        EncoderTracker checked(testCapabilities());
        ensure("the same pass begins with every check", checked.beginRenderPass(pass).empty());
        ensure("with draw checks the formats are checked", !checked.setPipeline(hdr.get()).empty());
        ensure("with draw checks the streams are checked", !checked.setVertexBuffer(vertices.get(), beyond).empty());
        ensure("with draw checks the sampling is checked", !checked.setTexture(0, notSampled.get()).empty());
        checked.endRenderPass();
        tracker = EncoderTracker();
        checked = EncoderTracker();
    }
}

// The tracker's record of a pass's dynamic state and the GL-enum tables for cull and fill (P2-05a) -------------------------
// A group of its own, so the tests merge beside other units' appended encoder tests without renumbering.

namespace
{
    typedef EncoderTracker::DynamicState DynamicState;

    std::string dynamicText(const DynamicState& d)
    {
        std::ostringstream out;
        out << "viewport ";
        if (d.viewport)
        {
            out << (*d.viewport)[0] << " " << (*d.viewport)[1] << " " << (*d.viewport)[2] << " " << (*d.viewport)[3];
        }
        else
        {
            out << "none";
        }
        out << ", scissor " << (d.scissorEnabled ? "on " : "off ") << d.scissor[0] << " " << d.scissor[1] << " "
            << d.scissor[2] << " " << d.scissor[3] << ", cull " << int(d.cull) << ", fill " << int(d.fill) << ", bias "
            << d.biasFactor << " " << d.biasUnits << (d.biasFill ? " fill" : "") << (d.biasLines ? " lines" : "")
            << ", clamp " << d.depthClamp << ", point " << d.pointSize << ", line " << d.lineWidth;
        return out.str();
    }

    void ensureDynamic(const std::string& msg, const DynamicState& actual, const DynamicState& expected)
    {
        tut::ensure(msg + ": " + dynamicText(actual) + ", not " + dynamicText(expected), actual == expected);
    }

    /// Each of the eight dynamic setters once on a tracker, with values that differ from a pass's defaults, and the record
    /// they make in a pass.
    const char* const DYNAMIC_SETTERS[] = { "setViewport", "setScissor",   "setCullMode",  "setFillMode",
                                            "setDepthBias", "setDepthClamp", "setPointSize", "setLineWidth" };

    std::string callDynamic(EncoderTracker& tracker, int setter)
    {
        switch (setter)
        {
        case 0: return tracker.setViewport(1, 2, 3, 4);
        case 1: return tracker.setScissor(true, 5, 6, 7, 8);
        case 2: return tracker.setCullMode(CullMode::Front);
        case 3: return tracker.setFillMode(FillMode::Lines);
        case 4: return tracker.setDepthBias(1.5f, 2.5f, true, true);
        case 5: return tracker.setDepthClamp(true);
        case 6: return tracker.setPointSize(3.f);
        default: return tracker.setLineWidth(0.75f);
        }
    }

    /// The record of all eight setters above, with the line width clamped to `low`-`high`.
    DynamicState allSet(F32 low, F32 high)
    {
        DynamicState d;
        d.viewport = std::array<S32, 4>{ 1, 2, 3, 4 };
        d.scissorEnabled = true;
        d.scissor = { 5, 6, 7, 8 };
        d.cull = CullMode::Front;
        d.fill = FillMode::Lines;
        d.biasFactor = 1.5f;
        d.biasUnits = 2.5f;
        d.biasFill = true;
        d.biasLines = true;
        d.depthClamp = true;
        d.pointSize = 3.f;
        d.lineWidth = llclamp(0.75f, low, high);
        return d;
    }

    Capabilities lineRange(F32 low, F32 high)
    {
        Capabilities caps;
        caps.lineWidthRange[0] = low;
        caps.lineWidthRange[1] = high;
        return caps;
    }
}

namespace tut
{
    struct llrhi_dynamic_data
    {
    };
    typedef test_group<llrhi_dynamic_data> llrhi_dynamic_group;
    typedef llrhi_dynamic_group::object dynamic_object;
    llrhi_dynamic_group llrhi_dynamic_test_group("LLRHI dynamic state");

    template<> template<>
    void dynamic_object::test<1>()
    {
        set_test_name("outside a pass each dynamic setter is refused with the GL backend's text, with checks on and off, "
                      "and the record keeps the defaults (P2-05a)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        const RenderPassDesc pass = colorDepthPass(*device, "pass", 16);
        for (bool checks : { true, false })
        {
            const std::string on = checks ? "checks on, " : "checks off, ";
            EncoderTracker tracker(lineRange(0.5f, 4.f), checks);
            ensureDynamic(on + "a new tracker has the defaults", tracker.dynamicState(), DynamicState());
            for (int setter = 0; setter < 8; ++setter)
            {
                const std::string at = on + DYNAMIC_SETTERS[setter];
                ensure_equals(at + " before any pass", callDynamic(tracker, setter),
                              std::string("dynamic state is set inside a pass"));
                ensureDynamic(at + " before any pass: no record", tracker.dynamicState(), DynamicState());
            }
            // After a pass, too.
            ensure_equals(on + "begin", tracker.beginRenderPass(pass), std::string());
            ensure_equals(on + "a setter in the pass", tracker.setPointSize(2.f), std::string());
            tracker.endRenderPass();
            for (int setter = 0; setter < 8; ++setter)
            {
                const std::string at = on + DYNAMIC_SETTERS[setter];
                ensure_equals(at + " after a pass", callDynamic(tracker, setter),
                              std::string("dynamic state is set inside a pass"));
                ensureDynamic(at + " after a pass: no record", tracker.dynamicState(), DynamicState());
            }
        }
    }

    template<> template<>
    void dynamic_object::test<2>()
    {
        set_test_name("in a pass each dynamic setter records GL's numbers as given and nothing else, the line width clamped "
                      "to the capabilities' range, with checks on and off (P2-05a)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        const RenderPassDesc pass = colorDepthPass(*device, "pass", 16);
        for (bool checks : { true, false })
        {
            const std::string on = checks ? "checks on, " : "checks off, ";
            for (const auto& [low, high] : { std::pair<F32, F32>(1.f, 1.f), std::pair<F32, F32>(0.5f, 4.f) })
            {
                const std::string in = on + llformat("lines %g-%g, ", low, high);
                EncoderTracker tracker(lineRange(low, high), checks);
                ensure_equals(in + "begin", tracker.beginRenderPass(pass), std::string());
                ensureDynamic(in + "the pass begins with the defaults", tracker.dynamicState(), DynamicState());
                // One field at a time, so each setter changes its own fields only.
                DynamicState expected;
                const DynamicState all = allSet(low, high);
                for (int setter = 0; setter < 8; ++setter)
                {
                    ensure_equals(in + DYNAMIC_SETTERS[setter], callDynamic(tracker, setter), std::string());
                    switch (setter)
                    {
                    case 0: expected.viewport = all.viewport; break;
                    case 1:
                        expected.scissorEnabled = all.scissorEnabled;
                        expected.scissor = all.scissor;
                        break;
                    case 2: expected.cull = all.cull; break;
                    case 3: expected.fill = all.fill; break;
                    case 4:
                        expected.biasFactor = all.biasFactor;
                        expected.biasUnits = all.biasUnits;
                        expected.biasFill = all.biasFill;
                        expected.biasLines = all.biasLines;
                        break;
                    case 5: expected.depthClamp = all.depthClamp; break;
                    case 6: expected.pointSize = all.pointSize; break;
                    default: expected.lineWidth = all.lineWidth; break;
                    }
                    ensureDynamic(in + DYNAMIC_SETTERS[setter] + " records", tracker.dynamicState(), expected);
                }
                // Values a pass would never see on Metal are still GL's numbers, recorded as given: a negative origin, an
                // empty or oversized box, a box set while the scissor is off, the never-set (0, 0, S32_MAX, S32_MAX), and
                // the separate enables of the bias.
                ensure_equals(in + "setViewport(-3, 5, 0, 7)", tracker.setViewport(-3, 5, 0, 7), std::string());
                expected.viewport = std::array<S32, 4>{ -3, 5, 0, 7 };
                ensure_equals(in + "setScissor off", tracker.setScissor(false, -1, -2, 100000, 0), std::string());
                expected.scissorEnabled = false;
                expected.scissor = { -1, -2, 100000, 0 };
                ensureDynamic(in + "as given", tracker.dynamicState(), expected);
                ensure_equals(in + "the never-set box", tracker.setScissor(true, 0, 0, S32_MAX, S32_MAX), std::string());
                expected.scissorEnabled = true;
                expected.scissor = { 0, 0, S32_MAX, S32_MAX };
                ensure_equals(in + "setCullMode(Off)", tracker.setCullMode(CullMode::Off), std::string());
                expected.cull = CullMode::Off;
                ensure_equals(in + "setCullMode(Back)", tracker.setCullMode(CullMode::Back), std::string());
                expected.cull = CullMode::Back;
                ensure_equals(in + "setFillMode(Fill)", tracker.setFillMode(FillMode::Fill), std::string());
                expected.fill = FillMode::Fill;
                ensure_equals(in + "setDepthBias(lines)", tracker.setDepthBias(-1.f, -4.f, false, true), std::string());
                expected.biasFactor = -1.f;
                expected.biasUnits = -4.f;
                expected.biasFill = false;
                expected.biasLines = true;
                ensure_equals(in + "setDepthClamp(false)", tracker.setDepthClamp(false), std::string());
                expected.depthClamp = false;
                ensure_equals(in + "setPointSize(0.5)", tracker.setPointSize(0.5f), std::string());
                expected.pointSize = 0.5f;
                ensureDynamic(in + "as given again", tracker.dynamicState(), expected);
                // The line width, clamped as LLRender::setLineWidth clamps it.
                for (const F32 width : { 0.25f, 0.5f, 2.f, 8.f, 1.f })
                {
                    ensure_equals(in + llformat("setLineWidth(%g)", width), tracker.setLineWidth(width), std::string());
                    ensure_equals(in + llformat("setLineWidth(%g) clamped", width), tracker.dynamicState().lineWidth,
                                  llclamp(width, low, high));
                }
                ensure_equals(in + "no other call is refused", tracker.endRenderPass().warning, std::string());
            }
        }
    }

    template<> template<>
    void dynamic_object::test<3>()
    {
        set_test_name("an accepted beginRenderPass, a stack pass's included, and endRenderPass and endFrame's end of a pass "
                      "put the defaults back; a refused begin keeps the open pass's record (P2-05a)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        const RenderPassDesc pass = colorDepthPass(*device, "pass", 16);
        RenderPassDesc stack = pass;
        stack.name = "stack";
        stack.stack = true;
        EncoderTracker tracker(lineRange(0.5f, 4.f));
        const DynamicState all = allSet(0.5f, 4.f);
        auto setAll = [&](const std::string& at)
        {
            for (int setter = 0; setter < 8; ++setter)
            {
                ensure_equals(at + DYNAMIC_SETTERS[setter], callDynamic(tracker, setter), std::string());
            }
            ensureDynamic(at + "all set", tracker.dynamicState(), all);
        };

        // endRenderPass.
        ensure_equals("begin", tracker.beginRenderPass(pass), std::string());
        setAll("pass: ");
        // A begin refused while the pass is open keeps the record, also one with an invalid description.
        ensure_contains("a begin in a pass", tracker.beginRenderPass(pass), "is still open");
        ensureDynamic("a refused begin keeps the record", tracker.dynamicState(), all);
        ensure_contains("a stack begin in a pass", tracker.beginRenderPass(stack), "is still open");
        ensureDynamic("a refused stack begin keeps the record", tracker.dynamicState(), all);
        tracker.endRenderPass();
        ensureDynamic("endRenderPass puts the defaults back", tracker.dynamicState(), DynamicState());
        ensure_contains("an end with no pass", tracker.endRenderPass().warning, "no pass is open");
        ensureDynamic("a refused end leaves the defaults", tracker.dynamicState(), DynamicState());
        RenderPassDesc invalid;
        invalid.name = "invalid";
        ensure("an invalid pass is refused", !tracker.beginRenderPass(invalid).empty());
        ensureDynamic("a refused begin with no pass leaves the defaults", tracker.dynamicState(), DynamicState());

        // An accepted begin, a stack pass's too, begins with the defaults.
        ensure_equals("a stack begin", tracker.beginRenderPass(stack), std::string());
        ensureDynamic("a stack pass begins with the defaults", tracker.dynamicState(), DynamicState());
        setAll("stack pass: ");
        // endFrame ends a stack pass with no warning and an ordinary one with one; both put the defaults back.
        EncoderTracker::Closing closing = tracker.endFrame();
        ensure("endFrame ends the stack pass", closing.endPass);
        ensure_equals("with no warning", closing.warning, std::string());
        ensureDynamic("endFrame's end of the stack pass puts the defaults back", tracker.dynamicState(), DynamicState());
        ensure_equals("begin again", tracker.beginRenderPass(pass), std::string());
        ensureDynamic("the next pass begins with the defaults", tracker.dynamicState(), DynamicState());
        setAll("pass again: ");
        closing = tracker.endFrame();
        ensure("endFrame ends the pass", closing.endPass);
        ensure_contains("with a warning", closing.warning, "the frame ended in pass 'pass'");
        ensureDynamic("endFrame's end of the pass puts the defaults back", tracker.dynamicState(), DynamicState());
        closing = tracker.endFrame();
        ensureDynamic("endFrame with no pass leaves the defaults", tracker.dynamicState(), DynamicState());
    }

    template<> template<>
    void dynamic_object::test<4>()
    {
        set_test_name("dynamicCalls counts every dynamic setter's call, accepted or refused, and nothing else (P2-05a)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        const RenderPassDesc pass = colorDepthPass(*device, "pass", 16);
        EncoderTracker tracker(lineRange(0.5f, 4.f));
        ensure_equals("none at first", tracker.dynamicCalls(), U64(0));
        U64 calls = 0;
        for (int setter = 0; setter < 8; ++setter)
        {
            callDynamic(tracker, setter);
            ensure_equals(std::string(DYNAMIC_SETTERS[setter]) + " refused", tracker.dynamicCalls(), ++calls);
        }
        ensure_equals("begin", tracker.beginRenderPass(pass), std::string());
        ensure_equals("a begin is not one", tracker.dynamicCalls(), calls);
        for (int setter = 0; setter < 8; ++setter)
        {
            callDynamic(tracker, setter);
            ensure_equals(std::string(DYNAMIC_SETTERS[setter]) + " accepted", tracker.dynamicCalls(), ++calls);
            callDynamic(tracker, setter);
            ensure_equals(std::string(DYNAMIC_SETTERS[setter]) + " with an equal value", tracker.dynamicCalls(), ++calls);
        }
        tracker.clear(true, true);
        tracker.setPipeline(nullptr);
        tracker.setDepthStencilState(nullptr);
        tracker.pushDebugGroup();
        tracker.popDebugGroup();
        ensure_equals("other calls are not", tracker.dynamicCalls(), calls);
        tracker.endRenderPass();
        tracker.endFrame();
        ensure_equals("nor an end or a frame's end", tracker.dynamicCalls(), calls);
    }

    template<> template<>
    void dynamic_object::test<5>()
    {
        set_test_name("cullModeFromGL and fillModeFromGL give the interface's value for GL_FRONT, GL_BACK, GL_FILL and GL_LINE, "
                      "and nothing for GL_FRONT_AND_BACK, GL_POINT, 0 or another enum (P2-05a)");
        ensure("GL_FRONT", cullModeFromGL(GL_FRONT) == CullMode::Front);
        ensure("GL_BACK", cullModeFromGL(GL_BACK) == CullMode::Back);
        for (const U32 gl : { U32(GL_FRONT_AND_BACK), U32(0), U32(GL_FILL), U32(GL_CULL_FACE), U32(GL_LEFT) })
        {
            ensure(llformat("0x%04x has no cull mode", gl), !cullModeFromGL(gl).has_value());
        }
        ensure("GL_FILL", fillModeFromGL(GL_FILL) == FillMode::Fill);
        ensure("GL_LINE", fillModeFromGL(GL_LINE) == FillMode::Lines);
        for (const U32 gl : { U32(GL_POINT), U32(0), U32(GL_FRONT_AND_BACK), U32(GL_LINES), U32(GL_FILL + 1) })
        {
            ensure(llformat("0x%04x has no fill mode", gl), !fillModeFromGL(gl).has_value());
        }
    }

    template<> template<>
    void dynamic_object::test<6>()
    {
        set_test_name("TestEncoder's dynamic setters run through the tracker: refused outside a pass with the reason, and "
                      "recorded in one, as Encoder::state() shows (P2-05a)");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        TestEncoder& encoder = device->mEncoder;
        encoder.setViewport(1, 2, 3, 4);
        encoder.setLineWidth(1.f);
        const std::vector<std::string> refused = { "viewport: dynamic state is set inside a pass",
                                                   "line width: dynamic state is set inside a pass" };
        ensure("refused outside a pass", encoder.errors == refused);
        ensure("nothing logged", encoder.log.empty());
        encoder.beginRenderPass(colorDepthPass(*device, "pass", 16));
        encoder.setViewport(1, 2, 3, 4);
        encoder.setScissor(true, 5, 6, 7, 8);
        encoder.setCullMode(CullMode::Front);
        encoder.setFillMode(FillMode::Lines);
        encoder.setDepthBias(1.5f, 2.5f, true, true);
        encoder.setDepthClamp(true);
        encoder.setPointSize(3.f);
        encoder.setLineWidth(0.75f);
        const std::vector<std::string> logged = { "begin pass", "viewport", "scissor", "cull", "fill",
                                                  "bias", "clamp", "point size", "line width" };
        ensure("recorded in the pass", encoder.log == logged);
        ensure("no new error", encoder.errors == refused);
        const Capabilities& caps = device->capabilities();
        ensureDynamic("state() shows the record", encoder.state().dynamicState(),
                      allSet(caps.lineWidthRange[0], caps.lineWidthRange[1]));
        encoder.endRenderPass();
        ensureDynamic("and the defaults after the pass", encoder.state().dynamicState(), DynamicState());
    }
}

// Samplers, their check and the texture's facade slot (P2-05b). tut runs at most 50 tests in a group, so these are a group of
// their own.

namespace tut
{
    struct llrhi_sampler_data
    {
    };
    typedef test_group<llrhi_sampler_data> llrhi_sampler_group;
    typedef llrhi_sampler_group::object sampler_object;
    llrhi_sampler_group llrhi_sampler_test_group("LLRHI samplers");

    template<> template<>
    void sampler_object::test<1>()
    {
        set_test_name("SamplerDesc (P2-05b): the defaults, == over every field, a hash that differs per field and packs -0 and "
                      "+0 alike, and the device's cache gives the cached sampler for an equal description and a new one for "
                      "each new field value");
        const SamplerDesc base;
        ensure("bilinear", base.filter == Filter::Bilinear);
        ensure("mipmapped", base.mipmapped);
        ensure_equals("no anisotropy", base.maxAnisotropy, 1.f);
        ensure("repeat on every axis", base.addressU == AddressMode::Repeat && base.addressV == AddressMode::Repeat
                                           && base.addressW == AddressMode::Repeat);
        ensure("not comparing", !base.compare);
        ensure("LessEqual, as pipeline.cpp sets GL_LEQUAL", base.compareFunc == CompareFunc::LessEqual);

        std::vector<std::pair<std::string, SamplerDesc>> fields;
        auto field = [&](const std::string& name, const std::function<void(SamplerDesc&)>& change)
        {
            SamplerDesc desc = base;
            change(desc);
            fields.emplace_back(name, desc);
        };
        field("filter", [](SamplerDesc& desc) { desc.filter = Filter::Anisotropic; });
        field("mipmapped", [](SamplerDesc& desc) { desc.mipmapped = false; });
        field("maxAnisotropy", [](SamplerDesc& desc) { desc.maxAnisotropy = 4.f; });
        field("addressU", [](SamplerDesc& desc) { desc.addressU = AddressMode::MirroredRepeat; });
        field("addressV", [](SamplerDesc& desc) { desc.addressV = AddressMode::MirroredRepeat; });
        field("addressW", [](SamplerDesc& desc) { desc.addressW = AddressMode::MirroredRepeat; });
        field("compare", [](SamplerDesc& desc) { desc.compare = true; });
        field("compareFunc", [](SamplerDesc& desc) { desc.compareFunc = CompareFunc::Greater; });
        SamplerDescHash hash;
        std::set<size_t> hashes{ hash(base) };
        for (const auto& [name, desc] : fields)
        {
            const SamplerDesc copy = desc;
            ensure(name + ": not equal to the defaults", !(desc == base));
            ensure(name + ": equal to its copy", desc == copy);
            ensure_equals(name + ": its copy's hash", hash(copy), hash(desc));
            ensure(name + ": a hash of its own", hashes.insert(hash(desc)).second);
        }
        SamplerDesc two = base;
        two.maxAnisotropy = 2.f;
        ensure("another anisotropy, another hash", hashes.insert(hash(two)).second);
        SamplerDesc plusZero = base;
        plusZero.maxAnisotropy = 0.f;
        SamplerDesc minusZero = base;
        minusZero.maxAnisotropy = -0.f;
        ensure("-0 and +0 are equal descriptions", plusZero == minusZero);
        ensure_equals("and hash alike", hash(plusZero), hash(minusZero));

        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        const Ref<Sampler> first = device->getSampler(base);
        ensure("a sampler", first.notNull());
        ensure("the cached one for an equal description", device->getSampler(SamplerDesc()).get() == first.get());
        int made = 1;
        for (const auto& [name, desc] : fields)
        {
            const Ref<Sampler> sampler = device->getSampler(desc);
            ensure(name + ": a new sampler", sampler.notNull() && sampler.get() != first.get());
            ensure(name + ": of its description", sampler->desc() == desc);
            ensure_equals(name + ": made", device->mSamplersMade, ++made);
            ensure(name + ": then cached", device->getSampler(desc).get() == sampler.get());
        }
        ensure_equals("one cached per description", device->mSamplers.size(), fields.size() + 1);
    }

    template<> template<>
    void sampler_object::test<2>()
    {
        set_test_name("validate(SamplerDesc, Capabilities) (P2-05b): the maximum anisotropy is refused when it is not finite, "
                      "below 1, above 1 without anisotropic filtering or above the device's maximum, and accepted otherwise, "
                      "1 also on a device without the capability, whose maximum GL reports as 0");
        Capabilities with = testCapabilities();
        with.anisotropicFiltering = true;
        with.maxAnisotropy = 16.f;
        Capabilities without = testCapabilities();
        without.anisotropicFiltering = false;
        without.maxAnisotropy = 0.f; // gGLManager.mMaxAnisotropy without the extension
        auto why = [](F32 anisotropy, const Capabilities& caps)
        {
            SamplerDesc desc;
            desc.maxAnisotropy = anisotropy;
            return validate(desc, caps);
        };
        auto refused = [&](const std::string& at, F32 anisotropy, const Capabilities& caps, const std::string& part)
        {
            const std::string reason = why(anisotropy, caps);
            ensure(at + ": refused with '" + part + "', reason '" + reason + "'", reason.find(part) != std::string::npos);
        };
        refused("NaN", std::numeric_limits<F32>::quiet_NaN(), with, "is not finite");
        refused("infinity", std::numeric_limits<F32>::infinity(), with, "is not finite");
        refused("minus infinity", -std::numeric_limits<F32>::infinity(), with, "is not finite"); // on the bits (P2-R1)
        refused("a NaN with the sign bit", -std::numeric_limits<F32>::quiet_NaN(), with, "is not finite");
        refused("0", 0.f, with, "is below 1");
        refused("0.5", 0.5f, with, "is below 1");
        refused("-2", -2.f, with, "is below 1");
        refused("17 above the device's 16", 17.f, with, "is above the device's 16");
        refused("2 without the capability", 2.f, without, "needs anisotropic filtering");
        refused("1.5 without the capability", 1.5f, without, "needs anisotropic filtering");
        ensure_equals("the defaults with the capability", validate(SamplerDesc(), with), std::string());
        ensure_equals("the defaults without it", validate(SamplerDesc(), without), std::string());
        ensure_equals("1 with the capability", why(1.f, with), std::string());
        ensure_equals("1 without it", why(1.f, without), std::string());
        ensure_equals("2.5", why(2.5f, with), std::string());
        ensure_equals("16, the device's maximum", why(16.f, with), std::string());
        SamplerDesc other;
        other.filter = Filter::Anisotropic;
        other.mipmapped = false;
        other.compare = true;
        other.addressU = AddressMode::ClampToEdge;
        ensure_equals("only the anisotropy is checked", validate(other, without), std::string());
    }

    template<> template<>
    void sampler_object::test<3>()
    {
        set_test_name("samplerForDraw (P2-05b's D8): Sampler2DShadow gives a comparing sampler with the binding's compare "
                      "function (LessEqual by default, another one kept), Sampler2D, SamplerCube and SamplerCubeArray give none "
                      "even when the binding compares, an integer format gives point filtering, and every other field is the "
                      "binding's");
        SamplerDesc bound;
        bound.filter = Filter::Anisotropic;
        bound.mipmapped = false;
        bound.maxAnisotropy = 8.f;
        bound.addressU = AddressMode::ClampToEdge;
        bound.addressV = AddressMode::MirroredRepeat;
        bound.addressW = AddressMode::Repeat;

        SamplerDesc expected = bound;
        expected.compare = true;
        ensure("Sampler2DShadow compares with LessEqual", samplerForDraw(bound, UniformType::Sampler2DShadow, Format::Depth32Float)
                                                             == expected);
        SamplerDesc greater = bound;
        greater.compareFunc = CompareFunc::Greater;
        expected.compareFunc = CompareFunc::Greater;
        ensure("Sampler2DShadow keeps another compare function",
               samplerForDraw(greater, UniformType::Sampler2DShadow, Format::Depth32Float) == expected);

        SamplerDesc comparing = bound;
        comparing.compare = true;
        for (const auto& [name, type] : { std::pair<std::string, UniformType>{ "Sampler2D", UniformType::Sampler2D },
                                          { "SamplerCube", UniformType::SamplerCube },
                                          { "SamplerCubeArray", UniformType::SamplerCubeArray } })
        {
            ensure(name + " does not compare", samplerForDraw(bound, type, Format::RGBA8Unorm) == bound);
            ensure(name + " does not compare for a comparing binding",
                   samplerForDraw(comparing, type, Format::Depth32Float) == bound);
        }

        SamplerDesc point = bound;
        point.filter = Filter::Point;
        point.magFilter = MagFilter::Nearest; // P2-11a: an integer format gets a nearest mag too
        for (const Format format : { Format::R8Uint, Format::R16Uint, Format::R32Uint })
        {
            ensure(llformat("integer format %u is point filtered", U32(format)),
                   samplerForDraw(bound, UniformType::Sampler2D, format) == point);
        }
        SamplerDesc pointShadow = point;
        pointShadow.compare = true;
        ensure("an integer shadow sampler: point and comparing",
               samplerForDraw(bound, UniformType::Sampler2DShadow, Format::R32Uint) == pointShadow);
        for (const Format format : { Format::RGBA8Unorm, Format::RGBA16Float, Format::Depth32Float })
        {
            ensure(llformat("format %u keeps the filter", U32(format)),
                   samplerForDraw(bound, UniformType::Sampler2D, format).filter == Filter::Anisotropic);
        }
    }

    template<> template<>
    void sampler_object::test<4>()
    {
        set_test_name("the texture's facade slot (P2-05b's D6): 0 for a new texture, set and read back, and every texture has "
                      "its own");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Texture> a = device->createTexture(texture2D(4, 4), "a");
        Ref<Texture> b = device->createTexture(texture2D(4, 4), "b");
        ensure("the textures", a.notNull() && b.notNull());
        ensure_equals("0 for a new texture", a->facadeState(), U64(0));
        ensure_equals("0 for the other", b->facadeState(), U64(0));
        const U64 state = 0xfedcba9876543210ull;
        a->setFacadeState(state);
        ensure_equals("read back", a->facadeState(), state);
        ensure_equals("not the other's", b->facadeState(), U64(0));
        b->setFacadeState(7);
        ensure_equals("the other's own", b->facadeState(), U64(7));
        ensure_equals("the first unchanged", a->facadeState(), state);
        a->setFacadeState(0);
        ensure_equals("back to 0", a->facadeState(), U64(0));
        ensure_equals("the other still its own", b->facadeState(), U64(7));
    }

    template<> template<>
    void sampler_object::test<5>()
    {
        set_test_name("the encoder's tracker reuses a channel's texture entry (P2-05b step 2b): an unbinding lets the texture "
                      "go at once and keeps the empty entry, the next binding of any channel takes it, so bind and unbind "
                      "cycles allocate no entry and never rehash; textures() and heldReferences() show exactly what they "
                      "showed before, also for refusals and the frame end's drop");
        TestGpu gpu;
        NativeCounter native;
        Ref<TestDevice> device = new TestDevice(gpu, native);
        Ref<Texture> a = device->createTexture(texture2D(4, 4), "reuse A");
        Ref<Texture> b = device->createTexture(texture2D(4, 4), "reuse B");
        TextureDesc targetOnly = texture2D(4, 4);
        targetOnly.usage = TEXTURE_RENDER_TARGET;
        Ref<Texture> unsampled = device->createTexture(targetOnly, "reuse unsampled");
        ensure("the textures", a.notNull() && b.notNull() && unsampled.notNull());
        EncoderTracker tracker(testCapabilities());
        const S32 refs = a->getNumRefs();
        auto bound = [&tracker](U32 channel) -> Texture*
        {
            const auto found = tracker.textures().find(channel);
            return found == tracker.textures().end() ? nullptr : found->second.get();
        };

        ensure_equals("A on 3", tracker.setTexture(3, a.get()), std::string());
        ensure("A shows on 3", bound(3) == a.get() && tracker.textures().size() == 1);
        ensure_equals("the tracker holds A", a->getNumRefs(), refs + 1);
        ensure_equals("no spare entry yet", tracker.spareTextureEntries(), size_t(0));
        ensure_equals("nothing on 3", tracker.setTexture(3, nullptr), std::string());
        ensure("3 shows nothing", bound(3) == nullptr && tracker.textures().empty());
        ensure_equals("A let go at the unbinding", a->getNumRefs(), refs);
        ensure_equals("held", tracker.heldReferences(), size_t(0));
        ensure_equals("the entry kept", tracker.spareTextureEntries(), size_t(1));
        ensure_equals("B on 5", tracker.setTexture(5, b.get()), std::string());
        ensure("B shows on 5 only", bound(5) == b.get() && bound(3) == nullptr && tracker.textures().size() == 1);
        ensure_equals("the kept entry taken", tracker.spareTextureEntries(), size_t(0));
        ensure_equals("A again on 5, replacing B", tracker.setTexture(5, a.get()), std::string());
        ensure("A shows on 5", bound(5) == a.get() && tracker.textures().size() == 1);
        ensure_equals("a replacement keeps its entry", tracker.spareTextureEntries(), size_t(0));

        // Cycles of a bind and an unbind on two channels: one entry each, reused, and the table never grows.
        ensure_equals("B on 6", tracker.setTexture(6, b.get()), std::string());
        const size_t buckets = tracker.textures().bucket_count();
        for (int i = 0; i < 100; ++i)
        {
            tracker.setTexture(5, nullptr);
            tracker.setTexture(6, nullptr);
            tracker.setTexture(i % 2 ? 6 : 5, a.get());
            tracker.setTexture(i % 2 ? 5 : 6, b.get());
        }
        ensure("after the cycles A and B on 5 and 6", bound(5) == b.get() && bound(6) == a.get());
        ensure_equals("no spare entry left over", tracker.spareTextureEntries(), size_t(0));
        ensure_equals("the table did not rehash", tracker.textures().bucket_count(), buckets);
        ensure_equals("held", tracker.heldReferences(), size_t(2));

        // A refusal lets the channel's binding go as before and keeps its entry.
        ensure("a texture that cannot be sampled is refused", !tracker.setTexture(5, unsampled.get()).empty());
        ensure("beyond the channels is refused", !tracker.setTexture(16, a.get()).empty());
        ensure("5 shows nothing after the refusal", bound(5) == nullptr && bound(16) == nullptr && bound(6) == a.get());
        ensure_equals("the refused channel's entry kept", tracker.spareTextureEntries(), size_t(1));

        // The frame end drops a binding nobody else holds and keeps its entry; a held one stays.
        Ref<Texture> lone = device->createTexture(texture2D(4, 4), "reuse lone");
        ensure_equals("lone on 7 takes the kept entry", tracker.setTexture(7, lone.get()), std::string());
        ensure_equals("taken", tracker.spareTextureEntries(), size_t(0));
        lone = nullptr;
        tracker.endFrame();
        ensure("lone dropped, A stays", bound(7) == nullptr && bound(6) == a.get() && tracker.textures().size() == 1);
        ensure_equals("the dropped binding's entry kept", tracker.spareTextureEntries(), size_t(1));
        ensure_equals("held after the frame end", tracker.heldReferences(), size_t(1));

        // A new tracker, as the GL encoder's reset() makes, starts empty.
        tracker = EncoderTracker(testCapabilities());
        ensure("a new tracker binds nothing", tracker.textures().empty() && tracker.spareTextureEntries() == 0);
        ensure_equals("and holds no reference", a->getNumRefs(), refs);
    }

    // [Viewer Lab] P2-11a's L1: SamplerDescHash keeps today's value for every description with the default magFilter.
    template<> template<>
    void sampler_object::test<6>()
    {
        set_test_name("SamplerDescHash and magFilter (P2-11a, L1): every description with the default magFilter packs as "
                      "before P2-11a (filter, U, V, W, compare, compareFunc, mips and anisotropy in today's bits); each "
                      "magFilter value gives its own hash and == sees it; validate refuses a value that is no MagFilter; "
                      "samplerForDraw gives an integer format Point and Nearest");
        auto today = [](const SamplerDesc& desc)
        {
            size_t packed = static_cast<size_t>(desc.filter);
            packed = (packed << 4) | static_cast<size_t>(desc.addressU);
            packed = (packed << 4) | static_cast<size_t>(desc.addressV);
            packed = (packed << 4) | static_cast<size_t>(desc.addressW);
            packed = (packed << 1) | (desc.compare ? 1 : 0);
            packed = (packed << 4) | static_cast<size_t>(desc.compareFunc);
            packed = (packed << 1) | (desc.mipmapped ? 1 : 0);
            const F32 anisotropy = desc.maxAnisotropy == 0.f ? 0.f : desc.maxAnisotropy;
            packed |= static_cast<size_t>(std::bit_cast<U32>(anisotropy)) << 32;
            return packed;
        };
        size_t checked = 0;
        for (U32 filter = 0; filter < 4; ++filter)
            for (U32 u = 0; u < 3; ++u)
                for (U32 v = 0; v < 3; ++v)
                    for (U32 w = 0; w < 3; ++w)
                        for (U32 compare = 0; compare < 2; ++compare)
                            for (U32 func = 0; func < 8; ++func)
                                for (U32 mips = 0; mips < 2; ++mips)
                                    for (F32 anisotropy : { 1.f, 2.f, 16.f })
                                    {
                                        SamplerDesc desc;
                                        desc.filter = Filter(filter);
                                        desc.addressU = AddressMode(u);
                                        desc.addressV = AddressMode(v);
                                        desc.addressW = AddressMode(w);
                                        desc.compare = compare != 0;
                                        desc.compareFunc = CompareFunc(func);
                                        desc.mipmapped = mips != 0;
                                        desc.maxAnisotropy = anisotropy;
                                        if (SamplerDescHash()(desc) != today(desc))
                                        {
                                            ensure_equals(llformat("filter %u U %u V %u W %u compare %u func %u mips %u "
                                                                   "anisotropy %.0f", filter, u, v, w, compare, func, mips,
                                                                   anisotropy), SamplerDescHash()(desc), today(desc));
                                        }
                                        ++checked;
                                    }
        ensure_equals("descriptions checked", checked, size_t(4 * 27 * 2 * 8 * 2 * 3));
        const SamplerDesc base;
        std::set<size_t> hashes;
        for (MagFilter mag : { MagFilter::FromFilter, MagFilter::Nearest, MagFilter::Linear })
        {
            SamplerDesc desc = base;
            desc.magFilter = mag;
            hashes.insert(SamplerDescHash()(desc));
            ensure("== sees magFilter", (desc == base) == (mag == MagFilter::FromFilter));
        }
        ensure_equals("each magFilter value gives its own hash", hashes.size(), size_t(3));
        const Capabilities caps = testCapabilities();
        SamplerDesc linear;
        linear.magFilter = MagFilter::Linear;
        ensure_equals("validate takes Linear", validate(linear, caps), std::string());
        SamplerDesc bad;
        bad.magFilter = MagFilter(3);
        const std::string why = validate(bad, caps);
        ensure("validate refuses MagFilter(3): " + why, why.find("mag filter 3") != std::string::npos);
        const SamplerDesc drawn = samplerForDraw(linear, UniformType::Sampler2D, Format::R8Uint);
        ensure("samplerForDraw: an integer format samples Point and Nearest",
               drawn.filter == Filter::Point && drawn.magFilter == MagFilter::Nearest);
        ensure("samplerForDraw: a float format keeps magFilter",
               samplerForDraw(linear, UniformType::Sampler2D, Format::RGBA8Unorm).magFilter == MagFilter::Linear);
    }
    // [Viewer Lab] P2-11a's L1 end

    // [Viewer Lab] P3-UX2 (the 16-sampler fix): the feature table's texture-unit lists on Metal.
    template<> template<>
    void sampler_object::test<7>()
    {
        set_test_name("featureTableTextureUnits, hardwareSettingCaps and cappedSetting (P3-UX2): a Metal device with 16 "
                      "samplers per stage is judged by 16 texture units whatever the system's GL reports, so "
                      "TexUnit16orLess caps RenderTerrainPBRDetail at -1 (a first start's default 0 becomes -1) and "
                      "TexUnit8orLess does not apply; fewer GL units add lists; other lists and names never cap; GL gets "
                      "no caps (Linden's behaviour)");
        // featuretable_mac.txt's lists as far as they name these settings
        const std::map<std::string, SettingCaps> table = {
            { "all", { { "RenderTerrainPBRDetail", 0.f }, { "RenderDeferredSSAO", 1.f } } },
            { "Ultra", { { "RenderTerrainPBRDetail", 0.f } } },
            { "Low", { { "RenderTerrainPBRDetail", -4.f } } },
            { "TexUnit8orLess", { { "RenderDeferredSSAO", 0.f } } },
            { "TexUnit16orLess", { { "RenderTerrainPBRDetail", -1.f } } },
            { "TexUnitorLess", { { "RenderDeferredSSAO", 0.f } } },
            { "TexUnit32orLessX", { { "RenderDeferredSSAO", 0.f } } },
        };
        Capabilities metal = testCapabilities();
        metal.backend = BackendType::Metal;
        ensure_equals("Metal's 16 samplers judge a GL of 32 units", featureTableTextureUnits(metal, 32), 16);
        ensure_equals("and a GL of 16", featureTableTextureUnits(metal, 16), 16);
        ensure_equals("a GL of 8 stays 8", featureTableTextureUnits(metal, 8), 8);
        for (S32 gl : { 16, 32 })
        {
            const SettingCaps caps = hardwareSettingCaps(metal, gl, table);
            ensure_equals(llformat("GL %d: one cap", gl), caps.size(), size_t(1));
            ensure_equals(llformat("GL %d: the default detail 0 capped at -1", gl),
                          cappedSetting(caps, "RenderTerrainPBRDetail", 0.f), -1.f);
            ensure_equals(llformat("GL %d: detail -4 stays", gl), cappedSetting(caps, "RenderTerrainPBRDetail", -4.f), -4.f);
            ensure_equals(llformat("GL %d: SSAO has no cap", gl), cappedSetting(caps, "RenderDeferredSSAO", 1.f), 1.f);
        }
        const SettingCaps eight = hardwareSettingCaps(metal, 8, table);
        ensure_equals("8 units: both lists", eight.size(), size_t(2));
        ensure_equals("8 units: SSAO off", cappedSetting(eight, "RenderDeferredSSAO", 1.f), 0.f);
        ensure_equals("8 units: detail -1", cappedSetting(eight, "RenderTerrainPBRDetail", 0.f), -1.f);
        Capabilities wide = metal;
        wide.maxSamplersPerStage = 32;
        ensure("32 samplers and a GL of 32: no caps", hardwareSettingCaps(wide, 32, table).empty());
        ensure_equals("32 samplers and a GL of 16: GL's units decide", hardwareSettingCaps(wide, 16, table).size(), size_t(1));
        Capabilities gl = testCapabilities();
        ensure("testCapabilities is GL", gl.backend == BackendType::GL);
        ensure_equals("GL: its own units", featureTableTextureUnits(gl, 32), 32);
        ensure("GL at 16 units: no caps", hardwareSettingCaps(gl, 16, table).empty());
        ensure("GL at 8 units: no caps", hardwareSettingCaps(gl, 8, table).empty());
        ensure_equals("GL: detail 0 unchanged", cappedSetting(hardwareSettingCaps(gl, 16, table), "RenderTerrainPBRDetail", 0.f),
                      0.f);
    }
    // [Viewer Lab] P3-UX2 end

    // Native programs, compute passes and the tracker's suspension of a stack pass ------------------------------------
    // No GPU: the tracker and the shared validation alone, with the test backend's objects.

    struct llrhi_native_data
    {
        TestGpu gpu;
        NativeCounter native;
        FrameLifecycle lifecycle{ gpu.waitFunction() };

        Ref<Texture> texture(const TextureDesc& desc, const std::string& name)
        {
            Ref<Texture> made = new TestTexture(lifecycle, native, desc);
            made->setDebugName(name);
            return made;
        }
        Ref<Texture> storage(const std::string& name, U32 mips = 1)
        {
            TextureDesc desc = texture2D(64, 64);
            desc.usage = TEXTURE_SAMPLED | TEXTURE_STORAGE;
            desc.mipLevels = mips;
            return texture(desc, name);
        }
        Ref<Buffer> buffer(U8 usage, const std::string& name)
        {
            BufferDesc desc;
            desc.size = 256;
            desc.usage = usage;
            Ref<Buffer> made = new TestBuffer(lifecycle, native, desc);
            made->setDebugName(name);
            return made;
        }
        ~llrhi_native_data() { lifecycle.releaseAll(); }
    };
    typedef test_group<llrhi_native_data> llrhi_native_group;
    typedef llrhi_native_group::object native_object;
    llrhi_native_group llrhi_native_test_group("LLRHI native");

    /// A compute pipeline of the test backend.
    class TestComputePipeline : public ComputePipeline
    {
    public:
        explicit TestComputePipeline(const ComputePipelineDesc& desc) : mDesc(desc) {}
        CompileStatus status() const override { return mStatus; }
        void wait() override {}
        U64 libraryId() const override { return 1; }
        const ComputePipelineDesc& desc() const override { return mDesc; }
        U32 executionWidth() const override { return 32; }
        U32 maxThreadsPerThreadgroup() const override { return 1024; }

        CompileStatus mStatus = CompileStatus::Ready;

    private:
        ComputePipelineDesc mDesc;
    };

    /// A pipeline of the test backend that says it is native.
    class TestNativePipeline : public TestPipeline
    {
    public:
        using TestPipeline::TestPipeline;
        bool native() const override { return true; }
    };

    Capabilities nativeCapabilities()
    {
        Capabilities caps = testCapabilities();
        caps.backend = BackendType::Metal;
        caps.computePasses = true;
        caps.nativePrograms = true;
        caps.textures3D = true;
        return caps;
    }

    template<> template<>
    void native_object::test<1>()
    {
        set_test_name("validate: storage textures and buffers and 3D textures need their capabilities; a 3D texture is one "
                      "sampled level of at most 2048 a side; textureBytes and rangeLevels");
        const Capabilities gl = testCapabilities();
        const Capabilities metal = nativeCapabilities();
        TextureDesc storage = texture2D(64, 64);
        storage.usage = TEXTURE_SAMPLED | TEXTURE_STORAGE;
        ensure_contains("GL refuses storage textures", validate(storage, gl), "need compute passes");
        ensure("Metal takes them", validate(storage, metal).empty());
        TextureDesc depthStorage = storage;
        depthStorage.format = Format::Depth32Float;
        ensure_contains("no depth storage", validate(depthStorage, metal), "storage textures are colour");
        TextureDesc msaaStorage = storage;
        msaaStorage.samples = 4;
        ensure_contains("no multisampled storage", validate(msaaStorage, metal), "single-sampled");
        TextureDesc volume = texture2D(48, 48, Format::RGBA16Float);
        volume.type = TextureType::Tex3D;
        volume.layers = 48;
        ensure_contains("GL refuses 3D textures", validate(volume, gl), "3D textures are not supported");
        ensure("Metal takes a 48^3 lookup table", validate(volume, metal).empty());
        TextureDesc mipped = volume;
        mipped.mipLevels = 2;
        ensure_contains("one level", validate(mipped, metal), "one mip level");
        TextureDesc target = volume;
        target.usage |= TEXTURE_RENDER_TARGET;
        ensure_contains("not a target", validate(target, metal), "neither render targets");
        TextureDesc deep = volume;
        deep.layers = 4096;
        ensure_contains("at most 2048", validate(deep, metal), "at most 2048");
        TextureDesc layered = texture2D(4, 4);
        layered.layers = 2;
        ensure_contains("a 2D texture has no layers", validate(layered, metal), "only cube map arrays and 3D textures");
        BufferDesc buffer;
        buffer.size = 64;
        buffer.usage = BUFFER_STORAGE;
        ensure_contains("a storage buffer without caps", validate(buffer, 0), "need compute passes");
        ensure_contains("a storage buffer on GL", validate(buffer, 0, &gl), "need compute passes");
        ensure("a storage buffer on Metal", validate(buffer, 0, &metal).empty());
        ensure_equals("a 48^3 RGBA16Float table", textureBytes(volume), U64(48) * 48 * 48 * 8);
        TextureDesc chain = texture2D(4, 4);
        chain.mipLevels = 3;
        ensure_equals("a 4x4 RGBA8 chain", textureBytes(chain), U64(16 + 4 + 1) * 4);
        ensure("all levels", rangeLevels({ 0, 0 }, 3) == std::make_pair(0u, 3u));
        ensure("one level", rangeLevels({ 1, 1 }, 3) == std::make_pair(1u, 2u));
        ensure("past the end is empty", rangeLevels({ 2, 2 }, 3).first == rangeLevels({ 2, 2 }, 3).second);
    }

    template<> template<>
    void native_object::test<2>()
    {
        set_test_name("a device and an encoder without native programs (GL) refuse each native call with a WARN line and "
                      "change nothing: the library fails, the pipelines and the pool are null, no compute pass begins");
        TestDevice device(gpu, native);
        CaptureLog log(LLError::LEVEL_WARN);
        Ref<NativeLibrary> library = device.createNativeLibrary({ "refused", "kernel void k() {}", {} });
        ensure("a library comes back", library.notNull());
        ensure("Failed", library->status() == CompileStatus::Failed);
        ensure("no compute pipeline", device.getComputePipeline(*library, { "k", {} }).isNull());
        ensure("no native pipeline", device.getNativePipeline(*library, { "v", "f" }, PipelineDesc()).isNull());
        ensure("no transient pool", device.createTransientPool({ "pool", {} }).isNull());
        Encoder& encoder = device.encoder();
        ensure("no compute pass", !encoder.beginComputePass({ "refused" }));
        encoder.dispatchThreads({ 1, 1, 1 }, { 1, 1, 1 });
        encoder.setNativeTexture(NativeStage::Compute, 0, nullptr);
        log.messageWith("createNativeLibrary 'refused': native programs need Metal");
        log.messageWith("beginComputePass: native programs and compute passes need Metal");
        log.messageWith("dispatchThreads: native programs and compute passes need Metal");
        log.messageWith("transient pools need Metal");
        ensure("the tracker saw nothing", !encoder.state().inComputePass());
    }

    template<> template<>
    void native_object::test<3>()
    {
        set_test_name("the tracker's compute pass: a dispatch needs a usable pipeline and a threadgroup the kernel takes; "
                      "transfers, render passes and queries are refused inside it; its end drops its bindings");
        EncoderTracker tracker(nativeCapabilities());
        Ref<ComputePipeline> kernel = new TestComputePipeline({ "k", { 8, 8, 1 } });
        Ref<Texture> texture = storage("t");
        Ref<Buffer> vertices = buffer(BUFFER_VERTEX, "v");
        bool barrier = false;
        ensure_contains("a dispatch outside", tracker.dispatch({ 1, 1, 1 }, { 8, 8, 1 }, barrier), "no compute pass");
        ensure_contains("a pipeline outside", tracker.setComputePipeline(kernel.get()), "no compute pass");
        ensure("begins", tracker.beginComputePass({ "c" }).empty());
        ensure_contains("twice", tracker.beginComputePass({ "d" }), "'c' is still open");
        ensure_contains("no pipeline", tracker.dispatch({ 1, 1, 1 }, { 8, 8, 1 }, barrier), "no compute pipeline");
        ensure("the pipeline", tracker.setComputePipeline(kernel.get()).empty());
        ensure_contains("the kernel's threadgroup", tracker.dispatch({ 64, 64, 1 }, { 16, 16, 1 }, barrier),
                        "made for threadgroups of 8x8x1");
        ensure_contains("empty", tracker.dispatch({ 0, 64, 1 }, { 8, 8, 1 }, barrier), "an empty dispatch");
        ensure_contains("too many threads", tracker.dispatch({ 64, 64, 1 }, { 64, 32, 1 }, barrier), "more than 1024");
        ensure("a dispatch", tracker.dispatch({ 64, 64, 1 }, { 8, 8, 1 }, barrier).empty());
        ensure_equals("counted", tracker.dispatches(), U64(1));
        ensure_contains("an update inside", tracker.updateBuffer(vertices.get(), 0, 4), "compute pass 'c' is open");
        ensure_contains("an upload inside",
                        tracker.uploadTexture(texture.get(), 0, 0, { 0, 0, 1, 1 }, PixelLayout(), 4), "is open");
        ensure_contains("a copy inside", tracker.copyTexture(texture.get(), 0, 0, { 0, 0, 1, 1 }, texture.get(), 0, 0, 1, 1),
                        "is open");
        ensure_contains("mipmaps inside", tracker.generateMipmaps(texture.get()), "is open");
        ensure_contains("a readback inside", tracker.readback(vertices.get(), 0, 4), "is open");
        ensure_contains("a render pass inside", tracker.beginRenderPass(RenderPassDesc()), "compute pass 'c' is still open");
        ensure_contains("threadgroup memory", tracker.setThreadgroupMemory(0, 100), "not a multiple of 16");
        ensure("threadgroup memory of 1 KB", tracker.setThreadgroupMemory(0, 1024).empty());
        ensure("a native texture", tracker.setNativeTexture(NativeStage::Compute, 0, texture.get(), Access::Write, {}).empty());
        ensure_contains("a fragment binding in a compute pass",
                        tracker.setNativeTexture(NativeStage::Fragment, 0, texture.get(), Access::Read, {}),
                        "no native render pass");
        ensure_contains("a slot past the table", tracker.setNativeSampler(NativeStage::Compute, 16), "past the 16");
        const size_t held = tracker.heldReferences();
        const EncoderTracker::Closing closing = tracker.endComputePass();
        ensure("the end", closing.endCompute && !closing.resumed && closing.warning.empty());
        ensure("its bindings and pipeline go", tracker.heldReferences() < held && tracker.computePipeline() == nullptr);
        ensure("none left", tracker.nativeTextures(NativeStage::Compute).empty());
        ensure("an update after", tracker.updateBuffer(vertices.get(), 0, 4).empty());
        ensure_contains("an end without a pass", tracker.endComputePass().warning, "no compute pass");
    }

    template<> template<>
    void native_object::test<4>()
    {
        set_test_name("native bindings: a written texture needs TEXTURE_STORAGE and covers one level; the same levels "
                      "written and bound again in a dispatch are refused, while reading level 0 and writing level 1 of "
                      "one texture is taken; a written buffer needs BUFFER_STORAGE; bytes are 1 byte to 64 KB");
        EncoderTracker tracker(nativeCapabilities());
        Ref<ComputePipeline> kernel = new TestComputePipeline({ "k", {} });
        TextureDesc sampledOnly = texture2D(64, 64);
        Ref<Texture> plain = texture(sampledOnly, "plain");
        Ref<Texture> chain = storage("chain", 3);
        Ref<Buffer> constants = buffer(BUFFER_UNIFORM, "constants");
        Ref<Buffer> counter = buffer(BUFFER_STORAGE, "counter");
        bool barrier = false;
        ensure("begins", tracker.beginComputePass({ "c" }).empty());
        tracker.setComputePipeline(kernel.get());
        ensure_contains("write without storage", tracker.setNativeTexture(NativeStage::Compute, 0, plain.get(), Access::Write, {}),
                        "without TEXTURE_STORAGE");
        ensure("a refused binding clears its slot", tracker.nativeTextures(NativeStage::Compute).empty());
        ensure_contains("write over all levels", tracker.setNativeTexture(NativeStage::Compute, 0, chain.get(), Access::Write, {}),
                        "over 3 levels");
        ensure_contains("levels outside", tracker.setNativeTexture(NativeStage::Compute, 0, chain.get(), Access::Read, { 3, 1 }),
                        "outside texture");
        ensure("read level 0", tracker.setNativeTexture(NativeStage::Compute, 0, chain.get(), Access::Read, { 0, 1 }).empty());
        ensure("write level 1", tracker.setNativeTexture(NativeStage::Compute, 1, chain.get(), Access::Write, { 1, 1 }).empty());
        ensure("a downsampler's bindings dispatch", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty());
        ensure("write level 0 too", tracker.setNativeTexture(NativeStage::Compute, 2, chain.get(), Access::Write, { 0, 1 }).empty());
        ensure_contains("the same level read and written", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier),
                        "over the same levels");
        ensure_contains("a written uniform buffer",
                        tracker.setNativeBuffer(NativeStage::Compute, 3, constants.get(), 0, Access::Write),
                        "without BUFFER_STORAGE");
        ensure("a read uniform buffer", tracker.setNativeBuffer(NativeStage::Compute, 3, constants.get(), 0, Access::Read).empty());
        ensure("a written storage buffer",
                        tracker.setNativeBuffer(NativeStage::Compute, 4, counter.get(), 0, Access::ReadWrite).empty());
        ensure_contains("an offset past the end", tracker.setNativeBuffer(NativeStage::Compute, 4, counter.get(), 256, Access::Read),
                        "past buffer");
        ensure_contains("no bytes", tracker.setNativeBytes(NativeStage::Compute, 0, 0), "1 to 65536");
        ensure_contains("too many bytes", tracker.setNativeBytes(NativeStage::Compute, 0, 65537), "1 to 65536");
        ensure("bytes", tracker.setNativeBytes(NativeStage::Compute, 3, 32).empty());
        ensure("the bytes take the buffer's slot", tracker.nativeBuffers(NativeStage::Compute).count(3) == 0);
        ensure("a buffer again", tracker.setNativeBuffer(NativeStage::Compute, 3, constants.get(), 0, Access::Read).empty());
        ensure_contains("refused bytes", tracker.setNativeBytes(NativeStage::Compute, 3, 0), "1 to 65536");
        ensure("refused bytes leave the slot empty", tracker.nativeBuffers(NativeStage::Compute).count(3) == 0);
        tracker.endComputePass();
    }

    template<> template<>
    void native_object::test<5>()
    {
        set_test_name("dispatches that bind what an earlier dispatch of the pass wrote get a barrier; unrelated ones none; a "
                      "barrier forgets what came before it, and a new pass starts with nothing written");
        EncoderTracker tracker(nativeCapabilities());
        Ref<ComputePipeline> kernel = new TestComputePipeline({ "k", {} });
        Ref<Texture> a = storage("a");
        Ref<Texture> b = storage("b");
        Ref<Texture> c = storage("c");
        Ref<Buffer> counts = buffer(BUFFER_STORAGE, "counts");
        bool barrier = true;
        tracker.beginComputePass({ "c" });
        tracker.setComputePipeline(kernel.get());
        tracker.setNativeTexture(NativeStage::Compute, 0, a.get(), Access::Write, {});
        ensure("the first dispatch", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && !barrier);
        tracker.setNativeTexture(NativeStage::Compute, 0, c.get(), Access::Write, {});
        ensure("an unrelated one", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && !barrier);
        tracker.setNativeTexture(NativeStage::Compute, 0, a.get(), Access::Read, {});
        tracker.setNativeTexture(NativeStage::Compute, 1, b.get(), Access::Write, {});
        ensure("reading a needs a barrier", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && barrier);
        tracker.setNativeTexture(NativeStage::Compute, 0, c.get(), Access::Read, {});
        tracker.setNativeTexture(NativeStage::Compute, 1, nullptr, Access::Read, {});
        ensure("c was written before the barrier", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && !barrier);
        tracker.setNativeTexture(NativeStage::Compute, 0, b.get(), Access::Read, {});
        ensure("b was written after it", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && barrier);
        tracker.setNativeTexture(NativeStage::Compute, 0, nullptr, Access::Read, {});
        tracker.setNativeBuffer(NativeStage::Compute, 1, counts.get(), 0, Access::ReadWrite);
        ensure("a buffer write", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && !barrier);
        ensure("the same buffer again", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && barrier);
        ensure_equals("three barriers", tracker.dispatchBarriers(), U64(3));
        tracker.endComputePass();
        tracker.beginComputePass({ "d" });
        tracker.setComputePipeline(kernel.get());
        tracker.setNativeTexture(NativeStage::Compute, 0, b.get(), Access::Read, {});
        ensure("a new pass: the queue barrier covers the last", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty()
                                                                 && !barrier);
        tracker.endComputePass();
    }

    template<> template<>
    void native_object::test<6>()
    {
        set_test_name("a compute pass and a native pass suspend an open stack pass, which goes on with its attachments "
                      "loaded and its dynamic state, pipeline and groups as they were; any other pass refuses them");
        EncoderTracker tracker(nativeCapabilities());
        Ref<Texture> color = texture(target(64, 64), "color");
        Ref<Texture> depth = texture(target(64, 64, Format::Depth32Float), "depth");
        Ref<Pipeline> pipeline = new TestPipeline(lifecycle, native, { 7, PipelineDesc() });
        PipelineDesc nativeDesc;
        nativeDesc.depthFormat = Format::Depth32Float;
        Ref<Pipeline> nativePipeline = new TestNativePipeline(lifecycle, native, { 8, nativeDesc });
        RenderPassDesc stack;
        stack.name = "stack";
        stack.stack = true;
        stack.colorCount = 1;
        stack.color[0].texture = color;
        stack.color[0].load = LoadAction::Clear;
        stack.depth.texture = depth;
        stack.depth.load = LoadAction::Clear;
        ensure("the stack pass", tracker.beginRenderPass(stack).empty());
        PipelineDesc color8;
        color8.depthFormat = Format::Depth32Float;
        Ref<Pipeline> drawn = new TestPipeline(lifecycle, native, { 9, color8 });
        ensure("its pipeline", tracker.setPipeline(drawn.get()).empty());
        tracker.setViewport(1, 2, 30, 40);
        tracker.pushDebugGroup();
        const U64 serial = tracker.passSerial();

        ensure("a compute pass in it", tracker.beginComputePass({ "compute" }).empty());
        ensure("the stack pass is set aside", !tracker.inPass() && tracker.suspendedPass() != nullptr);
        ensure_contains("dynamic state between", tracker.setScissor(true, 0, 0, 1, 1), "no pass is open");
        EncoderTracker::Closing closing = tracker.endComputePass();
        ensure("it goes on", closing.resumed && tracker.inPass() && tracker.pass().stack && !tracker.suspendedPass());
        ensure("its attachments load", tracker.pass().color[0].load == LoadAction::Load
                                       && tracker.pass().depth.load == LoadAction::Load);
        ensure("as a new pass", tracker.passSerial() == serial + 1);
        ensure("its viewport", tracker.dynamicState().viewport == std::optional<std::array<S32, 4>>({ 1, 2, 30, 40 }));
        ensure("its pipeline", tracker.pipeline() == drawn.get());

        RenderPassDesc nativePass = stack;
        nativePass.name = "native";
        nativePass.stack = false;
        nativePass.native = true;
        nativePass.color[0].load = LoadAction::Load;
        nativePass.depth.load = LoadAction::Load;
        ensure("a native pass in it", tracker.beginRenderPass(nativePass).empty());
        ensure("set aside again", tracker.suspendedPass() != nullptr && tracker.pass().native);
        ensure_contains("a translated pipeline in a native pass", tracker.setPipeline(pipeline.get()),
                        "translated program's pipeline in native pass");
        ensure("the native pipeline", tracker.setPipeline(nativePipeline.get()).empty());
        ensure("a fragment binding", tracker.setNativeSampler(NativeStage::Fragment, 0).empty());
        ensure_contains("a compute binding in a render pass", tracker.setNativeSampler(NativeStage::Compute, 0),
                        "no compute pass");
        ensure("a draw without vertices", tracker.draw(PrimitiveType::Triangles).empty());
        ensure_contains("a compute pass in a native pass", tracker.beginComputePass({ "nested" }),
                        "only a stack pass can be suspended");
        closing = tracker.endRenderPass();
        ensure("the native pass ends and the stack pass goes on", closing.endPass && closing.resumed && tracker.pass().stack);
        ensure("its pipeline again", tracker.pipeline() == drawn.get());
        ensure("the group opened before the native pass is still the stack pass's", tracker.popDebugGroup().empty());
        ensure_contains("a native pipeline in the stack pass", tracker.setPipeline(nativePipeline.get()),
                        "a native pipeline outside a native pass");
        tracker.endRenderPass();

        RenderPassDesc plain = stack;
        plain.stack = false;
        ensure("an ordinary pass", tracker.beginRenderPass(plain).empty());
        ensure_contains("no compute pass in it", tracker.beginComputePass({ "c" }), "only a stack pass can be suspended");
        ensure_contains("no native pass in it", tracker.beginRenderPass(nativePass), "is still open");
        tracker.endRenderPass();
    }

    template<> template<>
    void native_object::test<7>()
    {
        set_test_name("the frame's end inside a compute pass in a stack pass: the compute pass ends with a warning, then "
                      "the stack pass ends with the frame; inside a native pass in a stack pass: both end, one warning");
        EncoderTracker tracker(nativeCapabilities());
        Ref<Texture> color = texture(target(64, 64), "color");
        RenderPassDesc stack;
        stack.name = "stack";
        stack.stack = true;
        stack.colorCount = 1;
        stack.color[0].texture = color;
        tracker.beginRenderPass(stack);
        tracker.beginComputePass({ "late" });
        EncoderTracker::Closing closing = tracker.endFrame();
        ensure("the compute pass ends", closing.endCompute);
        ensure("and the stack pass", closing.endPass && !tracker.inPass() && !tracker.inComputePass());
        ensure_contains("one warning", closing.warning, "the frame ended in compute pass 'late'");
        ensure("nothing suspended", tracker.suspendedPass() == nullptr);

        tracker.beginRenderPass(stack);
        RenderPassDesc nativePass = stack;
        nativePass.name = "native";
        nativePass.stack = false;
        nativePass.native = true;
        tracker.beginRenderPass(nativePass);
        closing = tracker.endFrame();
        ensure("both end", closing.endPass && !closing.endCompute && !tracker.inPass() && !tracker.suspendedPass());
        ensure_contains("the native pass's warning", closing.warning, "the frame ended in pass 'native'");
        ensure_equals("nothing held", tracker.heldReferences(), size_t(0));
    }

    template<> template<>
    void native_object::test<8>()
    {
        set_test_name("the default transient pool makes each texture as its own on a device with compute passes; heapBytes "
                      "is what they take; a texture whose phases are backwards stays null in its place");
        class PoolDevice : public TestDevice
        {
        public:
            using TestDevice::TestDevice;
            const Capabilities& capabilities() const override { return mNative; }
            Capabilities mNative = nativeCapabilities();
        };
        Ref<PoolDevice> device = new PoolDevice(gpu, native);
        TransientPoolDesc desc;
        desc.name = "live";
        TransientTextureDesc half;
        half.desc = texture2D(32, 16, Format::RGBA16Float);
        half.desc.usage = TEXTURE_SAMPLED | TEXTURE_RENDER_TARGET;
        half.firstPhase = 1;
        half.lastPhase = 2;
        half.name = "half";
        TransientTextureDesc backwards = half;
        backwards.firstPhase = 3;
        backwards.lastPhase = 2;
        backwards.name = "backwards";
        desc.textures = { half, backwards };
        CaptureLog log(LLError::LEVEL_WARN);
        Ref<TransientPool> pool = device->createTransientPool(desc);
        ensure("a pool", pool.notNull());
        ensure_equals("two places", pool->textureCount(), U32(2));
        ensure("the first is made", pool->texture(0) != nullptr);
        ensure("the backwards one is not", pool->texture(1) == nullptr);
        ensure("past the end", pool->texture(2) == nullptr);
        log.messageWith("its last phase 2 is before its first 3");
        ensure_equals("dedicated bytes", pool->dedicatedBytes(), U64(32 * 16 * 8 * 2));
        ensure_equals("the heap is the textures for now", pool->heapBytes(), pool->dedicatedBytes());
    }

    template<> template<>
    void native_object::test<9>()
    {
        set_test_name("a dispatch that writes what an earlier dispatch of the pass read gets a barrier; reading it again "
                      "does not, and a barrier forgets the reads too");
        EncoderTracker tracker(nativeCapabilities());
        Ref<ComputePipeline> kernel = new TestComputePipeline({ "k", {} });
        Ref<Texture> a = storage("a");
        Ref<Texture> b = storage("b");
        Ref<Buffer> counts = buffer(BUFFER_STORAGE, "counts");
        bool barrier = true;
        tracker.beginComputePass({ "ping-pong" });
        tracker.setComputePipeline(kernel.get());
        tracker.setNativeTexture(NativeStage::Compute, 0, a.get(), Access::Read, {});
        tracker.setNativeTexture(NativeStage::Compute, 1, b.get(), Access::Write, {});
        ensure("a read into b", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && !barrier);
        tracker.setNativeTexture(NativeStage::Compute, 1, nullptr, Access::Read, {});
        ensure("a read again", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && !barrier);
        tracker.setNativeTexture(NativeStage::Compute, 0, a.get(), Access::Write, {});
        ensure("a written after it was read", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && barrier);
        tracker.setNativeTexture(NativeStage::Compute, 0, nullptr, Access::Read, {});
        tracker.setNativeBuffer(NativeStage::Compute, 0, counts.get(), 0, Access::Read);
        ensure("a buffer read", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && !barrier);
        tracker.setNativeBuffer(NativeStage::Compute, 0, counts.get(), 0, Access::Write);
        ensure("the buffer written after it", tracker.dispatch({ 1, 1, 1 }, { 1, 1, 1 }, barrier).empty() && barrier);
        ensure_equals("two barriers", tracker.dispatchBarriers(), U64(2));
        tracker.endComputePass();
    }

    template<> template<>
    void native_object::test<10>()
    {
        set_test_name("a 3D texture is neither copied from nor copied into, checks on or off");
        TextureDesc desc = texture2D(16, 16, Format::RGBA16Float);
        desc.type = TextureType::Tex3D;
        desc.layers = 16;
        Ref<Texture> volume = texture(desc, "volume");
        Ref<Texture> flat = texture(texture2D(16, 16, Format::RGBA16Float), "flat");
        for (bool checks : { true, false })
        {
            EncoderTracker tracker(nativeCapabilities(), checks);
            ensure_contains("from a 3D texture",
                            tracker.copyTexture(volume.get(), 0, 3, { 0, 0, 16, 16 }, flat.get(), 0, 0, 0, 0),
                            "a 3D texture is not copied");
            ensure_contains("into a 3D texture",
                            tracker.copyTexture(flat.get(), 0, 0, { 0, 0, 16, 16 }, volume.get(), 0, 3, 0, 0),
                            "a 3D texture is not copied");
            ensure_equals("no transfer", tracker.transfers(), U64(0));
        }
    }

    template<> template<>
    void native_object::test<11>()
    {
        set_test_name("MSL includes: one inside an #if is expanded where it stands and again later, so a header's guard "
                      "decides; files lists each file once; a file that includes itself is an error");
        const std::map<std::string, std::string> files = {
            { "main.metal", "#if X\n#include \"a.h\"\n#endif\n#include \"a.h\"\nbody\n" },
            { "a.h", "#ifndef A\n#define A\nA_TEXT\n#endif\n" },
            { "loop.metal", "#include \"b.h\"\n" },
            { "b.h", "#include \"loop.metal\"\n" },
        };
        const ::LL::MSL::ReadFile read = [&files](const std::string& path) -> std::optional<std::string>
        {
            const auto found = files.find(path);
            return found == files.end() ? std::nullopt : std::optional<std::string>(found->second);
        };
        const ::LL::MSL::IncludeResult result = ::LL::MSL::resolveIncludes("main.metal", read);
        ensure("no error", result.error.empty());
        size_t copies = 0;
        for (size_t at = result.text.find("A_TEXT"); at != std::string::npos; at = result.text.find("A_TEXT", at + 1))
        {
            ++copies;
        }
        ensure_equals("a.h at both includes", copies, size_t(2));
        ensure("the #if still holds the first", result.text.find("#if X\n#line 1 \"a.h\"") != std::string::npos);
        ensure_equals("each file once", result.files.size(), size_t(2));
        const ::LL::MSL::IncludeResult loop = ::LL::MSL::resolveIncludes("loop.metal", read);
        ensure_contains("a cycle", loop.error, "'loop.metal' includes itself");
        ensure("no text with an error", loop.text.empty());
    }

    template<> template<>
    void native_object::test<12>()
    {
        set_test_name("debug groups opened in a compute pass end with it, with a warning; one opened before it is not popped "
                      "inside it");
        EncoderTracker tracker(nativeCapabilities());
        ensure("an outer group", tracker.pushDebugGroup().empty());
        tracker.beginComputePass({ "c" });
        ensure_contains("the outer group inside", tracker.popDebugGroup(), "outside compute pass 'c'");
        tracker.pushDebugGroup();
        tracker.pushDebugGroup();
        ensure("one popped", tracker.popDebugGroup().empty());
        const EncoderTracker::Closing closing = tracker.endComputePass();
        ensure_equals("one left to pop", closing.popInPass, U32(1));
        ensure_contains("a warning", closing.warning, "ended with 1 debug groups open");
        ensure("the outer group pops after", tracker.popDebugGroup().empty());
        ensure_contains("and nothing more", tracker.popDebugGroup(), "no debug group is open");
        tracker.pushDebugGroup();
        tracker.beginComputePass({ "d" });
        tracker.pushDebugGroup();
        const EncoderTracker::Closing frame = tracker.endFrame();
        ensure_equals("the frame pops the compute pass's group", frame.popInPass, U32(1));
        ensure_equals("and the outer one", frame.popOutside, U32(1));
    }

    template<> template<>
    void native_object::test<13>()
    {
        set_test_name("nativePipelineKey: the functions and the normalized description; blend state that does not matter "
                      "gives the same key");
        PipelineDesc desc;
        desc.colorCount = 1;
        desc.colorFormats[0] = Format::RGBA16Float;
        PipelineDesc unblended = desc;
        unblended.blend.srcColor = BlendFactor::DestColor; // blending is off, so it does not count
        const NativeRenderFunctions copy{ "luminous_fullscreen_vs", "luminous_copy_fs" };
        ensure_equals("blend off: the same key", nativePipelineKey(copy, unblended), nativePipelineKey(copy, desc));
        ensure("another fragment function, another key",
               nativePipelineKey({ "luminous_fullscreen_vs", "other_fs" }, desc) != nativePipelineKey(copy, desc));
        PipelineDesc blended = desc;
        blended.blend.enabled = true;
        ensure("blending on, another key", nativePipelineKey(copy, blended) != nativePipelineKey(copy, desc));
        ensure_equals("it starts with the functions", nativePipelineKey(copy, desc).rfind("luminous_fullscreen_vs/luminous_copy_fs/", 0),
                      size_t(0));
    }
}
