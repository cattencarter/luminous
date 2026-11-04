/**
 * @file llrhigl.cpp
 * @brief The render hardware interface's GL backend
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

#include "llrhigl.h"

#include "llcubemap.h"
#include "llcubemaparray.h"
#include "llgl.h"
#include "llglheaders.h"
#include "llimagegl.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llvertexbuffer.h"
#include "llthread.h"
#include "llwindow.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>
#include <sstream>
#include <typeinfo>

namespace LL::RHI
{

// The interface's enums follow llrender's, so the facades translate by casting.
static_assert(U32(PrimitiveType::LineLoop) == LLRender::LINE_LOOP
              && U32(PrimitiveType::Points) == LLRender::POINTS, "PrimitiveType follows eGeomModes");
static_assert(U32(BlendFactor::OneMinusSourceAlpha) == LLRender::BF_ONE_MINUS_SOURCE_ALPHA
              && U32(BlendFactor::DestColor) == LLRender::BF_DEST_COLOR, "BlendFactor follows eBlendFactor");
static_assert(U32(CompareFunc::Greater) == LLRender::CF_GREATER
              && U32(CompareFunc::AlwaysPass) == LLRender::CF_ALWAYS, "CompareFunc follows eCompareFunc");
static_assert(U32(Filter::Anisotropic) == LLTexUnit::TFO_ANISOTROPIC
              && U32(Filter::Bilinear) == LLTexUnit::TFO_BILINEAR, "Filter follows eTextureFilterOptions");
static_assert(U32(AddressMode::ClampToEdge) == LLTexUnit::TAM_CLAMP
              && U32(AddressMode::MirroredRepeat) == LLTexUnit::TAM_MIRROR, "AddressMode follows eTextureAddressMode");
static_assert(VertexStreams::MAX_ATTRIBUTES == LLVertexBuffer::TYPE_MAX, "VertexStreams follows LLVertexBuffer's types");

namespace
{
    // GL errors from earlier code are not this call's; the backend clears them before it asks.
    std::string takeGLError()
    {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR)
        {
            return {};
        }
        while (glGetError() != GL_NO_ERROR)
        {
        }
        std::ostringstream text;
        text << "GL error 0x" << std::hex << error;
        return text.str();
    }

    LLTexUnit::eTextureType texUnitType(const TextureDesc& desc)
    {
        if (desc.samples > 1)
        {
            return LLTexUnit::TT_MULTISAMPLE_TEXTURE;
        }
        switch (desc.type)
        {
        case TextureType::Cube:
            return LLTexUnit::TT_CUBE_MAP;
        case TextureType::CubeArray:
            return LLTexUnit::TT_CUBE_MAP_ARRAY;
        case TextureType::Tex2D:
        case TextureType::Tex3D: // refused by validate on GL (no textures3D)
            break;
        }
        return LLTexUnit::TT_TEXTURE;
    }

    // A pixel format and type that GL accepts for allocating a format's storage without data.
    struct StorageLayout
    {
        GLenum format;
        GLenum type;
    };

    StorageLayout storageLayout(Format format)
    {
        switch (format)
        {
        case Format::R8Unorm:               return { GL_RED, GL_UNSIGNED_BYTE };
        case Format::RG8Unorm:              return { GL_RG, GL_UNSIGNED_BYTE };
        case Format::R16Float:              return { GL_RED, GL_HALF_FLOAT };
        case Format::RG16Float:             return { GL_RG, GL_HALF_FLOAT };
        case Format::RGBA16Float:           return { GL_RGBA, GL_HALF_FLOAT };
        case Format::RGBA16Unorm:           return { GL_RGBA, GL_UNSIGNED_SHORT };
        case Format::R32Float:              return { GL_RED, GL_FLOAT };
        case Format::RG32Float:             return { GL_RG, GL_FLOAT };
        case Format::RGBA32Float:           return { GL_RGBA, GL_FLOAT };
        case Format::RGB10A2Unorm:          return { GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV };
        case Format::RG11B10Float:          return { GL_RGB, GL_FLOAT };
        case Format::R8Uint:                return { GL_RED_INTEGER, GL_UNSIGNED_BYTE };
        case Format::R16Uint:               return { GL_RED_INTEGER, GL_UNSIGNED_SHORT };
        case Format::R32Uint:               return { GL_RED_INTEGER, GL_UNSIGNED_INT };
        case Format::Depth32Float:          return { GL_DEPTH_COMPONENT, GL_FLOAT };
        case Format::Depth32Float_Stencil8: return { GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV };
        default:                            return { GL_RGBA, GL_UNSIGNED_BYTE };
        }
    }

    GLint swizzleGL(Swizzle swizzle)
    {
        switch (swizzle)
        {
        case Swizzle::R:    return GL_RED;
        case Swizzle::G:    return GL_GREEN;
        case Swizzle::B:    return GL_BLUE;
        case Swizzle::A:    return GL_ALPHA;
        case Swizzle::Zero: return GL_ZERO;
        case Swizzle::One:  break;
        }
        return GL_ONE;
    }

    U32 levelSize(U32 size, U32 level)
    {
        return std::max(1u, size >> level);
    }

    /// Deletions of GL names let go of off the main thread, run on the main thread at a device's
    /// next frame end. LLImageGL's and LLVertexBuffer's delay lists belong to the main thread and
    /// have no lock, queries belong to the context that made them, and the thread that lets go may
    /// have no GL context at all.
    class MainThreadDeletes
    {
    public:
        static void run(std::function<void()> deletion)
        {
            if (on_main_thread())
            {
                deletion();
                return;
            }
            std::lock_guard<std::mutex> lock(sMutex);
            sPending.push_back(std::move(deletion));
        }

        /// On the main thread.
        static void flush()
        {
            std::vector<std::function<void()>> pending;
            {
                std::lock_guard<std::mutex> lock(sMutex);
                pending.swap(sPending);
            }
            for (std::function<void()>& deletion : pending)
            {
                deletion();
            }
        }

    private:
        inline static std::mutex sMutex;
        inline static std::vector<std::function<void()>> sPending;
    };

    /// Texture unit 0 for a texture's creation or upload, as LLImageGL binds there for its own.
    /// It binds on GL alone and puts back what GL had bound on the target, so LLTexUnit's cache
    /// and what raw GL or the encoder bound on channel 0 stay as they were. (The cache may even
    /// name a texture deleted since, which GL refuses to bind again.)
    class UnitZero
    {
    public:
        UnitZero(GLenum target, GLuint texture)
        :   mTarget(target)
        {
            gGL.flush();
            gGL.getTexUnit(0)->activate();
            glGetIntegerv(bindingQuery(target), &mPrevious);
            glBindTexture(target, texture);
        }
        ~UnitZero()
        {
            glBindTexture(mTarget, mPrevious);
        }

    private:
        static GLenum bindingQuery(GLenum target)
        {
            switch (target)
            {
            case GL_TEXTURE_CUBE_MAP:       return GL_TEXTURE_BINDING_CUBE_MAP;
            case GL_TEXTURE_CUBE_MAP_ARRAY: return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
            case GL_TEXTURE_2D_MULTISAMPLE: return GL_TEXTURE_BINDING_2D_MULTISAMPLE;
            default:                        return GL_TEXTURE_BINDING_2D;
            }
        }

        GLenum mTarget;
        GLint mPrevious = 0;
    };

    // Resources ----------------------------------------------------------------------------------

    /// A GL buffer from LLVertexBuffer's name pool. Created and filled through
    /// GL_COPY_WRITE_BUFFER, so the array and element bindings that LLVertexBuffer tracks stay as
    /// they are. A whole update of a dynamic one re-specifies its store (on GL_UNIFORM_BUFFER for a
    /// uniform buffer, whose generic binding no one tracks); a part and a static buffer are
    /// glBufferSubData; the name and every indexed binding stay (updateGLBuffer).
    class GLBuffer final : public Buffer
    {
    public:
        GLBuffer(const BufferDesc& desc, std::span<const std::byte> initial)
        :   mDesc(desc), mName(LLVertexBuffer::genGLBuffer())
        {
            glBindBuffer(GL_COPY_WRITE_BUFFER, mName);
            const bool whole = initial.size() == desc.size;
            glBufferData(GL_COPY_WRITE_BUFFER, desc.size, whole ? initial.data() : nullptr,
                         desc.dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
            if (!whole && !initial.empty())
            {
                glBufferSubData(GL_COPY_WRITE_BUFFER, 0, initial.size(), initial.data());
            }
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }

        ~GLBuffer() override
        {
            // As LLVertexBuffer deletes its own: a few frames later (GL as today).
            MainThreadDeletes::run([name = mName]() mutable { LLVertexBuffer::deleteGLBuffers(1, &name); });
        }

        const BufferDesc& desc() const override { return mDesc; }
        U32 name() const { return mName; } ///< [Viewer Lab] its GL name (GLNames::of)

    private:
        BufferDesc mDesc;
        U32 mName;
    };

    /// [Viewer Lab] Whether createBuffer makes a buffer of this description from LLVertexBuffer's pool (a GLPoolBuffer,
    /// below); updateBuffer casts only such a buffer
    bool poolDesc(const BufferDesc& desc)
    {
        return desc.dynamic && (desc.usage == BUFFER_VERTEX || desc.usage == BUFFER_INDEX);
    }

    /// GL's dynamic vertex and index buffers are LLVertexBuffer's: their GL name comes from the
    /// VBO pool LLVertexBuffer::initClass picked, and llvertexbuffer.cpp makes every GL call for
    /// them as LLVertexBuffer made them before. Unlike GLBuffer they use GL_ARRAY_BUFFER
    /// and GL_ELEMENT_ARRAY_BUFFER and keep LLVertexBuffer's binding record: the default pool binds
    /// a new buffer when it is created, and an update binds it when the record names another
    /// buffer. On Apple silicon's GL a buffer has no name until its first update and a new one
    /// after each whole update, so name() changes. The encoder binds its buffers only when
    /// they are set and when a pass begins, so a caller that creates or updates one inside a pass
    /// sets its buffers again before the next draw. desc() is as asked; the default pool's GL
    /// storage is larger (its adjusted size). Names go with LLVertexBuffer's delays
    /// (delete_buffers), never through FrameLifecycle.
    class GLPoolBuffer final : public Buffer
    {
    public:
        static Ref<Buffer> create(const BufferDesc& desc, std::span<const std::byte> initial)
        {
            const U32 target = desc.usage == BUFFER_INDEX ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
            U32 name = 0;
            if (!LLVertexBuffer::allocateGLStorage(target, U32(desc.size), name))
            {
                return nullptr;
            }
            Ref<GLPoolBuffer> buffer = new GLPoolBuffer(desc, target, name);
            if (!initial.empty())
            {
                buffer->update(0, initial);
            }
            return buffer;
        }

        ~GLPoolBuffer() override
        {
            MainThreadDeletes::run([target = mTarget, size = U32(mDesc.size), name = mName]
            {
                LLVertexBuffer::freeGLStorage(target, size, name);
            });
        }

        void update(U64 offset, std::span<const std::byte> data)
        {
            LLVertexBuffer::updateGLStorage(mTarget, U32(mDesc.size), mName, U32(offset),
                                            reinterpret_cast<const U8*>(data.data()), U32(data.size()));
        }

        /// In RenderDoc builds the name also labels the GL buffer, as LLVertexBuffer::setLabel
        /// labelled it before.
        void setDebugName(std::string_view name) override
        {
            Buffer::setDebugName(name);
#if LL_PROFILER_ENABLE_RENDER_DOC
            if (!name.empty())
            {
                LL_LABEL_OBJECT_GL(GL_BUFFER, mName, (GLsizei)name.size(), name.data());
            }
#endif
        }

        const BufferDesc& desc() const override { return mDesc; }
        U32 name() const { return mName; } ///< [Viewer Lab] its GL name (GLNames::of)

    private:
        GLPoolBuffer(const BufferDesc& desc, U32 target, U32 name)
        :   mDesc(desc), mTarget(target), mName(name)
        {
        }

        BufferDesc mDesc;
        U32 mTarget;
        U32 mName = 0;
    };

    /// A GL texture from LLImageGL's name pool, with storage for every level and the swizzle. It
    /// is made on texture unit 0, as LLImageGL makes its own, and GL's binding there is put back. Sampling is not
    /// stored here; the encoder sets it when it binds the texture.
    class GLTexture final : public Texture
    {
    public:
        /// The window's framebuffer, which GL draws to as framebuffer 0 and has no texture name.
        struct WindowFramebuffer {};
        GLTexture(const TextureDesc& desc, WindowFramebuffer)
        :   mDesc(desc), mWindow(true)
        {
        }

        /// LLImageGL's own texture: a name from LLImageGL's pool and nothing else, or the name of the texture
        /// LLImageGL re-specifies with another description, which this one replaces (LLImageGL::adoptRHITextureName).
        /// llimagegl.cpp
        /// makes every other GL call for it as LLImageGL made them before (the level range, setManualImage's
        /// swizzle mask and storage, glCompressedTexImage2D, the uploads and the mipmaps), so a level exists
        /// once LLImageGL has specified it, and desc() describes what LLImageGL asks GL for. LLImageGL's
        /// uploads and mipmaps still go through the encoder and the upload context, which only track them
        /// inside LLImageGL's scope (LLImageGL::makesOwnGLUploads). The name goes
        /// back to LLImageGL's deletion lists through LLImageGL::freeRHITextureName, which hands it over
        /// once even when LLImageGL has deleted it as a raw name first (syncTexName), and never through
        /// FrameLifecycle.
        struct ImageGLName {};
        GLTexture(const TextureDesc& desc, ImageGLName)
        :   mDesc(desc), mImageGL(true)
        {
            mName = LLImageGL::adoptRHITextureName(id());
            if (mName == 0)
            {
                mName = LLImageGL::allocateRHITextureName(id());
            }
        }

        /// A texture under a live GL name it does not own (GLNames::alias): no GL call, and the name is never deleted.
        struct BorrowedName {};
        GLTexture(const TextureDesc& desc, BorrowedName, U32 name)
        :   mDesc(desc), mName(name), mBorrowed(true)
        {
        }

        explicit GLTexture(const TextureDesc& desc)
        :   mDesc(desc)
        {
            LLImageGL::generateTextures(1, &mName);
            const LLTexUnit::eTextureType type = texUnitType(desc);
            const GLenum target = LLTexUnit::getInternalType(type);
            const GLenum internal = desc.glFormat != 0 ? desc.glFormat : toGLInternalFormat(desc.format);
            UnitZero unit(target, mName);

            if (desc.samples > 1)
            {
                glTexImage2DMultisample(target, desc.samples, internal, desc.width, desc.height, GL_TRUE);
            }
            else
            {
                const StorageLayout layout = storageLayout(desc.format);
                const U32 block = blockBytes(desc.format);
                for (U32 level = 0; level < desc.mipLevels; ++level)
                {
                    const U32 width = levelSize(desc.width, level);
                    const U32 height = levelSize(desc.height, level);
                    if (desc.type == TextureType::CubeArray)
                    {
                        allocate3D(target, level, internal, width, height, desc.layers * 6, layout, block);
                        continue;
                    }
                    const U32 faces = desc.type == TextureType::Cube ? 6 : 1;
                    for (U32 face = 0; face < faces; ++face)
                    {
                        const GLenum faceTarget = faces == 6 ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : target;
                        allocate2D(faceTarget, level, internal, width, height, layout, block);
                    }
                }
                glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, desc.mipLevels - 1);
                if (!desc.swizzle.isIdentity())
                {
                    const GLint swizzle[4] = { swizzleGL(desc.swizzle.r), swizzleGL(desc.swizzle.g),
                                               swizzleGL(desc.swizzle.b), swizzleGL(desc.swizzle.a) };
                    glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
                }
            }
        }

        ~GLTexture() override
        {
            // As LLImageGL deletes its own: a few frames later (GL as today).
            if (mImageGL)
            {
                MainThreadDeletes::run([name = mName, id = id()] { LLImageGL::freeRHITextureName(name, id); });
            }
            else if (mName != 0 && !mBorrowed)
            {
                MainThreadDeletes::run([name = mName] { LLImageGL::deleteTextures(1, &name); });
            }
        }

        const TextureDesc& desc() const override { return mDesc; }
        U32 name() const { return mName; } ///< [Viewer Lab] its GL name (GLNames::of)

        bool isWindow() const { return mWindow; }
        LLTexUnit::eTextureType unitType() const { return texUnitType(mDesc); }
        GLenum target() const { return LLTexUnit::getInternalType(unitType()); }
        GLenum internalFormat() const
        {
            return mDesc.glFormat != 0 ? mDesc.glFormat : toGLInternalFormat(mDesc.format);
        }
        /// The 2D target an upload or attachment of this layer uses: a cube's face, or the texture's.
        GLenum faceTarget(U32 layer) const
        {
            return mDesc.type == TextureType::Cube ? GLenum(GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer) : target();
        }
        void resize(U32 width, U32 height)
        {
            mDesc.width = width;
            mDesc.height = height;
        }

    private:
        static void allocate2D(GLenum target, U32 level, GLenum internal, U32 width, U32 height,
                               const StorageLayout& layout, U32 block)
        {
            if (block == 0)
            {
                glTexImage2D(target, level, internal, width, height, 0, layout.format, layout.type, nullptr);
                return;
            }
            const std::vector<U8> zeros(size_t((width + 3) / 4) * ((height + 3) / 4) * block);
            glCompressedTexImage2D(target, level, internal, width, height, 0, GLsizei(zeros.size()), zeros.data());
        }

        static void allocate3D(GLenum target, U32 level, GLenum internal, U32 width, U32 height, U32 depth,
                               const StorageLayout& layout, U32 block)
        {
            if (block == 0)
            {
                glTexImage3D(target, level, internal, width, height, depth, 0, layout.format, layout.type, nullptr);
                return;
            }
            const std::vector<U8> zeros(size_t((width + 3) / 4) * ((height + 3) / 4) * block * depth);
            glCompressedTexImage3D(target, level, internal, width, height, depth, 0, GLsizei(zeros.size()),
                                   zeros.data());
        }

        TextureDesc mDesc;
        U32 mName = 0;
        bool mWindow = false;
        bool mImageGL = false;
        bool mBorrowed = false;
    };

    GLTexture* asGLTexture(Texture* texture)
    {
        return static_cast<GLTexture*>(texture);
    }

    /// GL keeps sampling on the texture, as LLTexUnit sets it today, so a sampler is only its
    /// description; the encoder applies it when it binds a texture.
    class GLSampler final : public Sampler
    {
    public:
        explicit GLSampler(const SamplerDesc& desc) : mDesc(desc) {}
        const SamplerDesc& desc() const override { return mDesc; }

    private:
        SamplerDesc mDesc;
    };

    /// glFenceSync after the work it guards; the upload context returns one from submit().
    class GLFence final : public Fence
    {
    public:
        GLFence() : mSync(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0))
        {
            glFlush(); // so the fence is sent and can signal
        }
        ~GLFence() override
        {
            MainThreadDeletes::run([sync = mSync] { glDeleteSync(sync); });
        }

        bool signaled() const override
        {
            if (mSignaled)
            {
                return true;
            }
            mSignaled = glClientWaitSync(mSync, 0, 0) != GL_TIMEOUT_EXPIRED;
            return mSignaled;
        }

        void wait() override
        {
            while (!mSignaled)
            {
                const GLenum result = glClientWaitSync(mSync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
                if (result == GL_WAIT_FAILED)
                {
                    // Nothing more to wait for; the work's result is unknown.
                    LL_WARNS("RHI") << "Fence wait failed: " << takeGLError() << LL_ENDL;
                }
                mSignaled = result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED
                         || result == GL_WAIT_FAILED;
            }
        }

        /// The encoder's GPU-side wait: the current context's later commands wait for the fence, the CPU does not.
        void waitOnGPU() const { glWaitSync(mSync, 0, GL_TIMEOUT_IGNORED); }

    private:
        GLsync mSync;
        mutable bool mSignaled = false;
    };

    /// The fence the upload context gives inside LLImageGL's own scope. It guards no GL work of
    /// the adapter: LLImageGL makes the uploads itself, and its own fence, flushes and wait
    /// (gl_sync_to_main_thread in llimagegl.cpp) order the main context after the upload before the
    /// swap makes the texture bindable, so this fence makes no GL call and is signalled at once. Only
    /// LLImageGL::syncToMainThread submits inside the scope. Its debug name says so, for the tests.
    class ImageGLFence final : public Fence
    {
    public:
        ImageGLFence() { Object::setDebugName("LLImageGL's uploads"); }
        bool signaled() const override { return true; }
        void wait() override {}
    };

    /// GL reads synchronously, as glReadPixels does today, so the data is there at once.
    class GLReadback final : public Readback
    {
    public:
        GLReadback(std::vector<std::byte> data, U32 rowPitch) : mData(std::move(data)), mRowPitch(rowPitch) {}
        bool ready() const override { return true; }
        void wait() override {}
        std::span<const std::byte> data() const override { return mData; }
        U32 rowPitch() const override { return mRowPitch; }

    private:
        std::vector<std::byte> mData;
        U32 mRowPitch;
    };

    /// "At least one sample" is GL_ANY_SAMPLES_PASSED where GL has a target of its own for it
    /// (Capabilities::concurrentSampleQueries, from GL 3.3), otherwise GL_SAMPLES_PASSED, as
    /// LLOcclusionCullingGroup::doOcclusion chooses today.
    GLenum queryTarget(QueryType type, bool anySamplesTarget)
    {
        switch (type)
        {
        case QueryType::AnySamplesPassed:    return anySamplesTarget ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED;
        case QueryType::SamplesPassed:       return GL_SAMPLES_PASSED;
        case QueryType::TimeElapsed:         return GL_TIME_ELAPSED;
        case QueryType::PrimitivesGenerated: break;
        }
        return GL_PRIMITIVES_GENERATED;
    }

    /// A GL query name of GLDevice::createQueries, begun on its target again and again. GL fixes a query object's
    /// target at its first begin; this one has it from its creation. The reads are today's per call site: 32-bit for
    /// "at least one sample" and for the availability of samples, and 64-bit for the samples' result and for times and
    /// primitives when the device has timer queries (llgl.cpp loads glGetQueryObjectui64v only from GL 3.3), 32-bit
    /// below that.
    class GLQuery final : public Query
    {
    public:
        GLQuery(QueryType type, GLenum target, GLuint name, bool wide)
        :   mType(type), mTarget(target), mName(name), mWide(wide)
        {
        }

        ~GLQuery() override
        {
            MainThreadDeletes::run([name = mName] { glDeleteQueries(1, &name); });
        }

        QueryType type() const override { return mType; }
        bool ready() const override
        {
            if (mWide && (mType == QueryType::TimeElapsed || mType == QueryType::PrimitivesGenerated))
            {
                GLuint64 available = GL_FALSE;
                glGetQueryObjectui64v(mName, GL_QUERY_RESULT_AVAILABLE, &available);
                return available == GL_TRUE;
            }
            GLuint available = GL_FALSE;
            glGetQueryObjectuiv(mName, GL_QUERY_RESULT_AVAILABLE, &available);
            return available == GL_TRUE;
        }
        U64 result(bool wait) override
        {
            if (!wait && !ready())
            {
                return 0;
            }
            if (mType == QueryType::AnySamplesPassed || !mWide)
            {
                GLuint value = 0;
                glGetQueryObjectuiv(mName, GL_QUERY_RESULT, &value);
                return value;
            }
            GLuint64 value = 0;
            glGetQueryObjectui64v(mName, GL_QUERY_RESULT, &value);
            return value;
        }

        GLenum target() const { return mTarget; }
        GLuint name() const { return mName; }

    private:
        QueryType mType;
        GLenum mTarget;
        GLuint mName;
        bool mWide;
    };

    void updateGLBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data)
    {
        const BufferDesc& desc = buffer->desc();
        if (desc.dynamic && offset == 0 && data.size() == desc.size)
        {
            // [Viewer Lab] A new store at a whole update, as BufferDesc::dynamic promises and as the raw uniform-buffer
            // code calls it, so the update never waits for draws that still read the old store (a CGL probe: 1.0-2.4
            // us against 36-57 us for glBufferSubData into a buffer draws still read).
            const GLenum target = (desc.usage & BUFFER_UNIFORM) ? GL_UNIFORM_BUFFER : GL_COPY_WRITE_BUFFER;
            glBindBuffer(target, GLNames::of(buffer));
            glBufferData(target, desc.size, data.data(), GL_STREAM_DRAW);
            glBindBuffer(target, 0);
            return;
        }
        glBindBuffer(GL_COPY_WRITE_BUFFER, GLNames::of(buffer));
        glBufferSubData(GL_COPY_WRITE_BUFFER, offset, data.size(), data.data());
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }

    void uploadGLTexture(GLTexture* texture, U32 level, U32 layer, const Region& region,
                         const PixelLayout& layout, std::span<const std::byte> data)
    {
        const TextureDesc& desc = texture->desc();
        UnitZero unit(texture->target(), texture->name());
        if (const U32 block = blockBytes(desc.format))
        {
            const GLsizei size = GLsizei(U64((region.width + 3) / 4) * ((region.height + 3) / 4) * block);
            if (desc.type == TextureType::CubeArray)
            {
                glCompressedTexSubImage3D(texture->target(), level, region.x, region.y, layer, region.width,
                                          region.height, 1, texture->internalFormat(), size, data.data());
            }
            else
            {
                glCompressedTexSubImage2D(texture->faceTarget(layer), level, region.x, region.y, region.width,
                                          region.height, texture->internalFormat(), size, data.data());
            }
            return;
        }
        GLenum format = toGLPixelFormat(layout);
        if (isInteger(desc.format) && format == GL_RED)
        {
            format = GL_RED_INTEGER;
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, layout.rowLength);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, layout.swapBytes ? GL_TRUE : GL_FALSE);
        if (desc.type == TextureType::CubeArray)
        {
            glTexSubImage3D(texture->target(), level, region.x, region.y, layer, region.width, region.height, 1,
                            format, toGLPixelType(layout), data.data());
        }
        else
        {
            glTexSubImage2D(texture->faceTarget(layer), level, region.x, region.y, region.width, region.height,
                            format, toGLPixelType(layout), data.data());
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    }

    void generateGLMipmaps(GLTexture* texture)
    {
        UnitZero unit(texture->target(), texture->name());
        glGenerateMipmap(texture->target());
    }

    // Programs, pipelines and depth state ---------------------------------------------------------

    /// The GL programs the device has, by id, so the encoder finds a pipeline's program without the
    /// pipeline holding it. The device outlives its objects.
    struct ProgramRegistry
    {
        std::mutex mutex;
        std::unordered_map<U64, GLuint> names;

        GLuint find(U64 id)
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = names.find(id);
            return found != names.end() ? found->second : 0;
        }
    };

    GLenum shaderTypeGL(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex:   return GL_VERTEX_SHADER;
        case ShaderStage::Fragment: return GL_FRAGMENT_SHADER;
        case ShaderStage::Geometry: break;
        }
        return GL_GEOMETRY_SHADER;
    }

    const char* stageName(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex:   return "vertex";
        case ShaderStage::Fragment: return "fragment";
        case ShaderStage::Geometry: break;
        }
        return "geometry";
    }

    std::string shaderLog(GLuint shader)
    {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(std::max(length, 1), '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        log.resize(strlen(log.c_str()));
        return log;
    }

    std::string programLog(GLuint program)
    {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log(std::max(length, 1), '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());
        log.resize(strlen(log.c_str()));
        return log;
    }

    // The reflected types; a sampler of another kind (rectangle, 3D, multisample) is bound by its
    // channel like the others. Types the viewer does not use (doubles, other matrix shapes) are
    // left out of the reflection.
    bool isUnsignedGL(GLenum type)
    {
        return type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_VEC2 || type == GL_UNSIGNED_INT_VEC3
            || type == GL_UNSIGNED_INT_VEC4;
    }

    // The interface's uniform blocks bind from here, above LLGLSLShader's fixed binding points
    // (UB_REFLECTION_PROBES and the GLTF blocks), so raw GL's uniform buffers stay where they are.
    // Slot s is binding point BLOCK_BINDING_BASE + s, shared by every program.
    constexpr GLuint BLOCK_BINDING_BASE = LLGLSLShader::INTERFACE_BLOCK_BINDING;
    static_assert(BLOCK_BINDING_BASE >= LLGLSLShader::NUM_UNIFORM_BLOCKS, "above LLGLSLShader's blocks");

    /// A linked GL program, compiled from the stages' units with the reserved attributes bound
    /// first, as LLGLSLShader::mapAttributes binds them, and reflected by LLGLSLShader's GL
    /// reflector, as mapUniforms reads them. GL compiles at once, so the program is ready
    /// when it exists.
    class GLProgram final : public Program
    {
    public:
        /// LLGLSLShader's own program: LLGLSLShader and LLShaderMgr make every GL call for it as before (the
        /// binary cache, the shader objects, attaching, binding the attributes, linking, mapUniforms' block bindings),
        /// and LLGLSLShader deletes the name in unloadInternal. Its reflection is the driver's, read in wait() once
        /// LLGLSLShader has linked it.
        struct ShaderProgram {};

        /// Null with the reason when a stage does not compile or the program does not link.
        static Ref<Program> create(const ProgramDesc& desc, PipelineCache& pipelines,
                                   ProgramRegistry& registry, std::string& why)
        {
            const GLuint name = glCreateProgram();
            if (name == 0)
            {
                why = "glCreateProgram failed";
                return nullptr;
            }
            std::vector<GLuint> shaders;
            auto cleanUp = [&shaders, name]()
            {
                for (GLuint shader : shaders)
                {
                    glDetachShader(name, shader);
                    glDeleteShader(shader);
                }
            };
            for (const StageSource& stage : desc.stages)
            {
                for (size_t unit = 0; unit < stage.units.size(); ++unit)
                {
                    const GLuint shader = glCreateShader(shaderTypeGL(stage.stage));
                    const GLchar* text = stage.units[unit].c_str();
                    glShaderSource(shader, 1, &text, nullptr);
                    glCompileShader(shader);
                    GLint compiled = GL_FALSE;
                    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                    if (!compiled)
                    {
                        const std::string log = shaderLog(shader);
                        LL_WARNS("ShaderLoading") << "Program '" << desc.name << "', " << stageName(stage.stage)
                                                  << " unit " << unit << ":\n" << log << LL_ENDL;
                        why = std::string(stageName(stage.stage)) + " unit " + std::to_string(unit)
                            + " does not compile: " + log.substr(0, log.find('\n'));
                        glDeleteShader(shader); // never attached
                        cleanUp();
                        glDeleteProgram(name);
                        return nullptr;
                    }
                    glAttachShader(name, shader);
                    shaders.push_back(shader);
                }
            }
            for (size_t i = 0; i < desc.reservedAttributes.size(); ++i)
            {
                glBindAttribLocation(name, GLuint(i), desc.reservedAttributes[i].c_str());
            }
            glLinkProgram(name);
            GLint linked = GL_FALSE;
            glGetProgramiv(name, GL_LINK_STATUS, &linked);
            cleanUp();
            if (!linked)
            {
                const std::string log = programLog(name);
                LL_WARNS("ShaderLoading") << "Program '" << desc.name << "' does not link:\n" << log << LL_ENDL;
                why = "it does not link: " + log.substr(0, log.find('\n'));
                glDeleteProgram(name);
                return nullptr;
            }
            return new GLProgram(desc, name, pipelines, registry);
        }

        ~GLProgram() override
        {
            // Its pipelines leave the cache, and the encoder no longer finds it.
            mPipelines.forgetProgram(id());
            {
                std::lock_guard<std::mutex> lock(mRegistry.mutex);
                mRegistry.names.erase(id());
            }
            // As LLGLSLShader::unload deletes its program; GL keeps it while it is in use. LLGLSLShader's own
            // program is LLGLSLShader's to delete.
            if (mOwnsName)
            {
                MainThreadDeletes::run([name = mName] { glDeleteProgram(name); });
            }
        }

        /// Ready, as GL compiles at once, except while the test delay keeps LLGLSLShader's own program Pending.
        CompileStatus status() const override { return delaying() ? CompileStatus::Pending : CompileStatus::Ready; }

        /// LLGLSLShader's own program is reflected the first time after LLGLSLShader has linked it, with no block binding
        /// and no glGetError; on the main thread, as the adoption. A wait() while the test delay keeps it ends the
        /// delay at once, with no GL call: LLGLSLShader links the program in the publication that follows.
        void wait() override
        {
            if (delaying())
            {
                mDelayed.store(false);
                return;
            }
            if (!mOwnsName && !mReflected)
            {
                GLint linked = GL_FALSE;
                glGetProgramiv(mName, GL_LINK_STATUS, &linked);
                if (linked)
                {
                    fillReflection();
                    mReflected = true;
                }
            }
        }
        const ProgramReflection& reflection() const override { return mReflection; }
        U32 name() const { return mName; } ///< [Viewer Lab] its GL name (GLNames::of)

        /// The GL location of each reflected uniform, by its index in the reflection.
        GLint location(U32 uniformIndex) const
        {
            return uniformIndex < mLocations.size() ? mLocations[uniformIndex] : -1;
        }

        /// GL sets uint uniforms with glUniform*uiv; the reflection calls them Int.
        bool isUnsigned(U32 uniformIndex) const
        {
            return uniformIndex < mUnsigned.size() && mUnsigned[uniformIndex];
        }

    private:
        friend class GLDevice; // adopts LLGLSLShader's own program

        GLProgram(const ProgramDesc& desc, GLuint name, PipelineCache& pipelines, ProgramRegistry& registry)
        :   mName(name), mPipelines(pipelines), mRegistry(registry), mReservedAttributes(desc.reservedAttributes)
        {
            setDebugName(desc.name);
            reflect();
            std::lock_guard<std::mutex> lock(registry.mutex);
            registry.names[id()] = name;
        }

        /// Adopts LLGLSLShader's own program with no GL call: no block bindings and no glGetError, and no reflection until
        /// wait() after the link. With the device that takes the test delay (takeProgramDelay) it is Pending from
        /// the device's frameNumber() now for the delay's frames, or until wait() for 0, and logs nothing, since an RHI
        /// WARN fails every golden run; its id, registry entry, pipelines and name stay GLProgram's.
        GLProgram(const ProgramDesc& desc, GLuint name, PipelineCache& pipelines, ProgramRegistry& registry, ShaderProgram,
                  const Device* delayDevice, U32 delayFrames)
        :   mName(name), mOwnsName(false), mPipelines(pipelines), mRegistry(registry), mReservedAttributes(desc.reservedAttributes),
            mDelayDevice(delayDevice), mDelayUntil(delayDevice && delayFrames ? delayDevice->frameNumber() + delayFrames : 0),
            mDelayed(delayDevice != nullptr)
        {
            setDebugName(desc.name);
            std::lock_guard<std::mutex> lock(registry.mutex);
            registry.names[id()] = name;
        }

        /// The driver's reflection, through LLGLSLShader's GL reflector, with each uniform's location and whether GL sets it
        /// as unsigned.
        void fillReflection()
        {
            std::vector<U32> types;
            LLGLSLShader::reflectGLUniforms(mName, debugName(), mReflection, &mLocations, &types);
            mUnsigned.clear();
            for (U32 type : types)
            {
                mUnsigned.push_back(isUnsignedGL(type));
            }
            mReflection.attributeMask = LLGLSLShader::reflectGLAttributes(mName, mReservedAttributes)
                                      & ((1u << VertexStreams::MAX_ATTRIBUTES) - 1);
        }

        void reflect()
        {
            fillReflection();
            for (size_t i = 0; i < mReflection.blocks.size(); ++i)
            {
                // The block's binding point, which the encoder binds buffers to: one of LLGLSLShader's named blocks at
                // its slot, the UniformBlock, whatever its index here, and any other block above them.
                GLuint slot = LLGLSLShader::NUM_UNIFORM_BLOCKS + GLuint(i);
                for (U32 block = 0; block < LLGLSLShader::NUM_UNIFORM_BLOCKS; ++block)
                {
                    if (mReflection.blocks[i].name == LLGLSLShader::uniformBlockName(LLGLSLShader::UniformBlock(block)))
                    {
                        slot = block;
                        break;
                    }
                }
                glUniformBlockBinding(mName, GLuint(i), BLOCK_BINDING_BASE + slot);
            }
        }

        GLuint mName;
        bool mOwnsName = true; ///< false for LLGLSLShader's own program
        PipelineCache& mPipelines;
        ProgramRegistry& mRegistry;
        ProgramReflection mReflection;
        std::vector<GLint> mLocations;
        std::vector<bool> mUnsigned;
        std::vector<std::string> mReservedAttributes; ///< the description's, for the attribute mask
        bool mReflected = false; ///< LLGLSLShader's own program has been reflected after its link

        /// The test delay holds: armed, not ended by wait(), and for frames, before the frame that ends it.
        bool delaying() const
        {
            return mDelayed.load() && (mDelayUntil == 0 || mDelayDevice->frameNumber() < mDelayUntil);
        }

        const Device* mDelayDevice = nullptr; ///< the device that made it, for the test delay's frames
        U64 mDelayUntil = 0;                  ///< the frame that ends the delay; 0: the first wait()
        std::atomic<bool> mDelayed{ false };  ///< the test delay is armed, until wait(), on any thread
    };

    /// LLGLSLShader's own program under golden level 2's test fault (takeProgramFault): adopted with no GL call,
    /// as GLProgram adopts it, but failed; it logs nothing, since an RHI WARN fails every golden run. A deferred one is
    /// a background compile that fails at once: Pending to the first who asks, Failed from then on and after wait(). It
    /// has no registry entry and no pipelines (getPipeline refuses it), and LLGLSLShader deletes the name in
    /// unloadInternal.
    class FaultedProgram final : public Program
    {
    public:
        FaultedProgram(const ProgramDesc& desc, GLuint name, bool deferred)
        :   mName(name), mPending(deferred)
        {
            setDebugName(desc.name);
        }

        CompileStatus status() const override
        {
            if (mPending.exchange(false))
            {
                return CompileStatus::Pending;
            }
            return CompileStatus::Failed;
        }
        void wait() override { mPending.store(false); }
        const ProgramReflection& reflection() const override { return mReflection; }
        U32 name() const { return mName; } ///< [Viewer Lab] its GL name (GLNames::of)

    private:
        const GLuint mName;
        mutable std::atomic<bool> mPending; ///< deferred, until the first status() or wait(), on any thread
        ProgramReflection mReflection;
    };

    /// GL has no pipeline objects: the encoder applies the description through llrender's caches
    /// when the pipeline is set, so a pipeline is its key.
    class GLPipeline final : public Pipeline
    {
    public:
        explicit GLPipeline(const PipelineKey& key) : mKey(key) {}
        U64 programId() const override { return mKey.programId; }
        const PipelineDesc& desc() const override { return mKey.desc; }
        CompileStatus status() const override { return CompileStatus::Ready; }

    private:
        PipelineKey mKey;
    };

    class GLDepthStencilState final : public DepthStencilState
    {
    public:
        explicit GLDepthStencilState(const DepthStencilDesc& desc) : mDesc(desc) {}
        const DepthStencilDesc& desc() const override { return mDesc; }

    private:
        DepthStencilDesc mDesc;
    };

    // The encoder ------------------------------------------------------------------------------------

    /// Turns a GL state on or off through LLGLState's map and leaves it so, as interface setters do
    /// on GL: LLGLEnable scopes and RenderDebugGL's checks stay right. It is GL only,
    /// so it never reaches the viewer's encoder, which would call this backend again.
    class KeptGLState : public LLGLState
    {
    public:
        KeptGLState(LLGLenum state, bool enabled)
        :   LLGLState(state, enabled ? ENABLED_STATE : DISABLED_STATE, GLOnly())
        {
            mWasEnabled = mIsEnabled; // the destructor keeps the new state
        }

        /// What LLGLState's map says about a state.
        static bool isEnabled(LLGLenum state) { return sStateMap[state] == GL_TRUE; }
    };

    /// An LLGLEnable or LLGLDisable scope of this backend's own: the same GL and map, put back by
    /// ~LLGLState when it ends, and never sent to the viewer's encoder.
    class GLOnlyScope : public LLGLState
    {
    public:
        GLOnlyScope(LLGLenum state, bool enabled)
        :   LLGLState(state, enabled ? ENABLED_STATE : DISABLED_STATE, GLOnly())
        {
        }
    };

    GLenum compareGL(CompareFunc func)
    {
        switch (func)
        {
        case CompareFunc::Never:        return GL_NEVER;
        case CompareFunc::AlwaysPass:   return GL_ALWAYS;
        case CompareFunc::Less:         return GL_LESS;
        case CompareFunc::LessEqual:    return GL_LEQUAL;
        case CompareFunc::Equal:        return GL_EQUAL;
        case CompareFunc::NotEqual:     return GL_NOTEQUAL;
        case CompareFunc::GreaterEqual: return GL_GEQUAL;
        case CompareFunc::Greater:      break;
        }
        return GL_GREATER;
    }

    GLenum blendOpGL(BlendOp op)
    {
        switch (op)
        {
        case BlendOp::Add:             return GL_FUNC_ADD;
        case BlendOp::Subtract:        return GL_FUNC_SUBTRACT;
        case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case BlendOp::Min:             return GL_MIN;
        case BlendOp::Max:             break;
        }
        return GL_MAX;
    }

    /// Sets depth state through LLGLDepthTest and leaves it so: its destructor restores to the
    /// values it is given here, and its statics stay right for the scopes that follow.
    void keepDepthState(const DepthStencilDesc& desc)
    {
        const GLenum func = compareGL(desc.depthFunc);
        LLGLDepthTest depth(desc.depthTest, desc.depthWrite, func);
        depth.mPrevDepthEnabled = desc.depthTest;
        depth.mPrevWriteEnabled = desc.depthTest && desc.depthWrite;
        depth.mPrevDepthFunc = func;
    }

    /// Makes a program current for a uniform call and puts the previous one back, so uniforms can
    /// be set on a program that is not the bound pipeline's. LLGLSLShader::sCurBoundShader is the
    /// current program, as the encoder and LLGLSLShader::bind keep it.
    class ProgramForUniforms
    {
    public:
        explicit ProgramForUniforms(GLuint program)
        :   mProgram(program), mPrevious(LLGLSLShader::sCurBoundShader)
        {
            if (mPrevious != mProgram)
            {
                glUseProgram(mProgram);
            }
        }
        ~ProgramForUniforms()
        {
            if (mPrevious != mProgram)
            {
                glUseProgram(mPrevious);
            }
        }

    private:
        GLuint mProgram;
        GLuint mPrevious;
    };

    // LLRender::eBlendFactor's GL values, to read GL's blend function back.
    LLRender::eBlendFactor blendFactorFromGL(GLint factor)
    {
        const GLint factors[] = { GL_ONE, GL_ZERO, GL_DST_COLOR, GL_SRC_COLOR, GL_ONE_MINUS_DST_COLOR,
                                  GL_ONE_MINUS_SRC_COLOR, GL_DST_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_DST_ALPHA,
                                  GL_ONE_MINUS_SRC_ALPHA };
        for (U32 i = 0; i < std::size(factors); ++i)
        {
            if (factors[i] == factor)
            {
                return LLRender::eBlendFactor(i);
            }
        }
        return LLRender::BF_ONE;
    }

    /// The LLGLSLShader whose program of the viewer's device has this id, or null for the adapter's own programs: the
    /// shader an interface pipeline's program switch makes current, as LLGLSLShader::bind would. A scan of the
    /// loaded shaders, made only at an interface program switch.
    LLGLSLShader* owner_of(U64 programId)
    {
        for (LLGLSLShader* shader : LLGLSLShader::sInstances)
        {
            if (const Program* program = shader->getRHIProgram(); program && program->id() == programId)
            {
                return shader;
            }
        }
        return nullptr;
    }

    /// The GL state a pass can change, as it was when the pass began, and put back when it ends, so
    /// raw GL after the pass sees what it had. Enables come from LLGLState's map, the
    /// rest from GL; they go back through the same caches.
    class SavedGLState
    {
    public:
        void save()
        {
            for (size_t i = 0; i < std::size(ENABLES); ++i)
            {
                mEnabled[i] = KeptGLState::isEnabled(ENABLES[i]);
            }
            glGetIntegerv(GL_VIEWPORT, mViewport);
            glGetIntegerv(GL_SCISSOR_BOX, mScissor);
            glGetIntegerv(GL_BLEND_SRC_RGB, &mBlend[0]);
            glGetIntegerv(GL_BLEND_DST_RGB, &mBlend[1]);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &mBlend[2]);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &mBlend[3]);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, &mEquation[0]);
            glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &mEquation[1]);
            glGetBooleanv(GL_COLOR_WRITEMASK, mColorMask);
            glGetIntegerv(GL_CULL_FACE_MODE, &mCullFace);
            glGetIntegerv(GL_POLYGON_MODE, mPolygonMode);
            glGetFloatv(GL_POINT_SIZE, &mPointSize);
            glGetFloatv(GL_LINE_WIDTH, &mLineWidth);
            glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &mOffsetFactor);
            glGetFloatv(GL_POLYGON_OFFSET_UNITS, &mOffsetUnits);
            mDepthTest = glIsEnabled(GL_DEPTH_TEST);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &mDepthWrite);
            glGetIntegerv(GL_DEPTH_FUNC, &mDepthFunc);
            mProgram = LLGLSLShader::sCurBoundShader;
            mShader = LLGLSLShader::sCurBoundShaderPtr;
        }

        void restore()
        {
            // The program raw GL had, through LLGLSLShader when it was one, so sCurBoundShaderPtr is
            // right again for the code after the pass. A switch without a bind ends the current
            // shader's profile queries first, as bind does, before anything here changes.
            const bool rebind = mShader && mShader->mProgramObject == mProgram && mProgram != 0;
            const bool use = !rebind && LLGLSLShader::sCurBoundShader != mProgram;
            if (use && LLGLSLShader::sCurBoundShaderPtr)
            {
                LLGLSLShader::sCurBoundShaderPtr->readProfileQuery();
            }
            for (size_t i = 0; i < std::size(ENABLES); ++i)
            {
                KeptGLState state(ENABLES[i], mEnabled[i]);
            }
            glViewport(mViewport[0], mViewport[1], mViewport[2], mViewport[3]);
            glScissor(mScissor[0], mScissor[1], mScissor[2], mScissor[3]);
            // [Viewer Lab] GL's box and LLGLState's map behind LLRender::setScissor, also while the viewer's own pass is
            // registered and another GL device's interface pass ends
            gGL.invalidateScissor();
            gGL.blendFunc(blendFactorFromGL(mBlend[0]), blendFactorFromGL(mBlend[1]),
                          blendFactorFromGL(mBlend[2]), blendFactorFromGL(mBlend[3]));
            glBlendEquationSeparate(mEquation[0], mEquation[1]);
            gGL.setColorMask(mColorMask[0], mColorMask[1], mColorMask[2], mColorMask[3]);
            glCullFace(mCullFace);
            glPolygonMode(GL_FRONT_AND_BACK, mPolygonMode[0]);
            glPointSize(mPointSize);
            {
                // In LLRender's scope its facade sends nothing, so the pass's end never calls the encoder again.
                LLRender::OwnGLState own;
                gGL.setLineWidth(mLineWidth);
            }
            glPolygonOffset(mOffsetFactor, mOffsetUnits);
            LLGLDepthTest depth(mDepthTest, mDepthWrite, mDepthFunc);
            depth.mPrevDepthEnabled = mDepthTest;
            depth.mPrevWriteEnabled = mDepthWrite;
            depth.mPrevDepthFunc = mDepthFunc;
            if (rebind)
            {
                if (LLGLSLShader::sCurBoundShader != mProgram)
                {
                    mShader->bind();
                }
                LLGLSLShader::sCurBoundShaderPtr = mShader;
            }
            else if (use)
            {
                LLVertexBuffer::unbind();
                glUseProgram(mProgram);
                LLGLSLShader::sCurBoundShader = mProgram;
                LLGLSLShader::sCurBoundShaderPtr = mShader;
                // The saved shader's profile queries begin again, as bind places them after a switch: a refused shader,
                // whose program is 0, comes back here
                if (mShader)
                {
                    mShader->placeProfileQuery();
                }
            }
        }

    private:
        static constexpr LLGLenum ENABLES[] = { GL_BLEND, GL_SCISSOR_TEST, GL_CULL_FACE, GL_POLYGON_OFFSET_FILL,
                                                GL_POLYGON_OFFSET_LINE, GL_DEPTH_CLAMP };
        bool mEnabled[std::size(ENABLES)] = {};
        GLint mViewport[4] = {};
        GLint mScissor[4] = {};
        GLint mBlend[4] = {};
        GLint mEquation[2] = {};
        GLboolean mColorMask[4] = {};
        GLint mCullFace = GL_BACK;
        GLint mPolygonMode[2] = { GL_FILL, GL_FILL };
        GLfloat mPointSize = 1.f;
        GLfloat mLineWidth = 1.f;
        GLfloat mOffsetFactor = 0.f;
        GLfloat mOffsetUnits = 0.f;
        GLboolean mDepthTest = GL_FALSE;
        GLboolean mDepthWrite = GL_TRUE;
        GLint mDepthFunc = GL_LESS;
        GLuint mProgram = 0;
        LLGLSLShader* mShader = nullptr;
    };

    // The facades' switches, this backend's contract with llrender. The facades and the interface share one GL
    // context, so a facade that makes its own GL call sends
    // the same call to the viewer's device inside one of these scopes; this backend then only checks or tracks the call
    // and makes no GL call for it, and outside the scopes it makes the GL call itself. The names stay. Each scope puts
    // back the value it found, so scopes nest, except create_rhi_texture's (below). Three are statics for the main
    // thread only, the others thread_local:
    // - LLRenderTarget::makesOwnGL (llrendertarget.cpp's OwnGL): beginRenderPass, endRenderPass, clear and
    //   generateMipmaps of LLRenderTarget's stack. Main thread (a static, llrendertarget.cpp).
    // - LLVertexBuffer::OwnGLDraws: setPipeline, setDepthStencilState, setVertexBuffer, setIndexBuffer, draw
    //   and drawIndexed. Main thread (a static, llvertexbuffer.cpp).
    // - LLRender::OwnGLState: the eight dynamic setters, setViewport to setLineWidth. Main thread (a
    //   static, llrender.cpp).
    // - LLGLSLShader::OwnGLUniforms: setUniform and setSamplerChannel. thread_local.
    // - LLTexUnit::OwnGLBinds: setTexture. thread_local.
    // - LLImageGL::OwnGLUploads: the encoder's uploadTexture and generateMipmaps, and the upload
    //   context's uploadTexture, generateMipmaps and submit (an ImageGLFence then). thread_local, for the upload
    //   thread.
    // - LLImageGL::OwnGLTransfers: copyTexture and readback. thread_local.
    // - LLImageGL::makesOwnGLStorage (create_rhi_texture's scope): createTexture takes only a name.
    //   thread_local; it covers one createTexture call, which never comes back into it, so it clears on exit instead of
    //   nesting.
    // - LLGLSLShader::OwnGLProgramScope: createProgram adopts the named GL program. thread_local; it puts the
    //   previous program back.

    /// GL's encoder. Every setter goes to GL at once through llrender's caches and is never applied
    /// again at draw time; every call runs through the EncoderTracker; and passes,
    /// clears, state, uniforms, bindings, draws, copies, uploads, mipmaps, readbacks and updates of
    /// buffers outside LLVertexBuffer's pool flush llrender's batched immediate mode first,
    /// so draws keep their order with raw GL, unless LLRenderTarget or LLImageGL make the GL calls
    /// themselves (see LLRenderTarget's stack passes below). Query begins and ends do not flush: they go to GL
    /// at once, as today's raw calls do, so a query covers what reached GL. A pass is a
    /// scope for GL state: it saves what it can change when it begins, sets the interface's bindings
    /// again (raw GL between passes may have bound its own), and puts the saved state back when it
    /// ends. Dynamic state is set inside a pass. Passes use one framebuffer of the encoder's own, or
    /// the window's for the surface, and give LLRenderTarget its current framebuffer back.
    /// LLRenderTarget's stack passes are the exception (LLRenderTarget::makesOwnGL):
    /// LLRenderTarget's GL backend functions bind the target's framebuffer, draw and read buffers
    /// and viewport, clear and make mipmaps as before, so this backend only tracks their begin, end,
    /// clears and mipmaps, and it ends or clears no other pass for them. Such a pass is no scope for
    /// GL state: it saves, sets and puts back nothing and applies no bindings when it begins;
    /// dynamic state, bindings, draws, clears, copies and readbacks other code sends inside it go to
    /// GL at once as in any pass.
    /// LLVertexBuffer's draws are the other (LLVertexBuffer::makesOwnGLDraws): its GL
    /// backend functions bind and draw as before, so this backend only tracks the pipeline, depth
    /// state, buffers and draws it sends, with no flush and no GL. Those bindings are kept apart from
    /// the interface's: the interface's are copied at the first such binding and moved back into the
    /// tracker before any interface call that reads or changes them, before an interface pass
    /// begins and before the frame ends, so to the interface LLVertexBuffer's draws stay raw GL.
    /// LLRender's dynamic state is the third (LLRender::makesOwnGLState): its facades and
    /// LLGLState make their GL calls in GL backend functions, so inside LLRender::OwnGLState the
    /// dynamic setters only record the state in the tracker, with no flush and no GL. This backend's
    /// own LLGLState objects are GL only, and its own line-width calls run in that scope, so it never
    /// calls itself through LLGLState or LLRender. While LLRenderTarget's pass is open this backend
    /// registers its tracker with gGL (beginOwnPass/endOwnPass), so LLRender::setScissor records there
    /// without calling it, and each of its own scissor calls makes LLRender forget its box.
    class GLEncoder final : public Encoder
    {
    public:
        GLEncoder(const Capabilities& caps, ProgramRegistry& programs)
        :   mTracker(caps), mPrograms(programs)
        {
        }

        ~GLEncoder() override
        {
            reset();
            const GLuint framebuffers[] = { mFramebuffer, mReadFramebuffer, mDrawFramebuffer };
            for (GLuint framebuffer : framebuffers)
            {
                if (framebuffer)
                {
                    glDeleteFramebuffers(1, &framebuffer);
                }
            }
        }

        bool beginRenderPass(const RenderPassDesc& pass) override
        {
            ++mPassSerial; // [Viewer Lab] no bind record of an earlier pass is valid in this one (setUniformBuffer)
            if (LLRenderTarget::makesOwnGL())
            {
                // LLRenderTarget has bound the target, its buffers and its viewport; the framebuffer
                // is only kept for clearColorBuffer's draw-buffer index.
                if (!accept("beginRenderPass", mTracker.beginRenderPass(pass)))
                {
                    return false;
                }
                mOwnPass = true;
                gGL.beginOwnPass(mTracker); // [Viewer Lab] LLRender records its scissor straight in this pass
                mPassFramebuffer = LLRenderTarget::sCurFBO;
                return true;
            }
            gGL.flush();
            restoreInterfaceBindings();
            restoreInterfaceTextures(); // the interface's channels again, so applyBindings binds its textures
            if (!accept("beginRenderPass", mTracker.beginRenderPass(pass)))
            {
                return false;
            }
            mSaved.save();
            mPreviousFramebuffer = LLRenderTarget::sCurFBO;
            mPreviousWidth = LLRenderTarget::sCurResX;
            mPreviousHeight = LLRenderTarget::sCurResY;

            const GLTexture* first = pass.colorCount > 0 ? asGLTexture(pass.color[0].texture.get())
                                                         : asGLTexture(pass.depth.texture.get());
            const U32 level = pass.colorCount > 0 ? pass.color[0].level : pass.depth.level;
            const U32 width = std::max(1u, first->desc().width >> level);
            const U32 height = std::max(1u, first->desc().height >> level);
            mPassFramebuffer = first->isWindow() ? 0 : framebuffer(mFramebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, mPassFramebuffer);
            if (mPassFramebuffer != 0)
            {
                attach(pass);
            }
            LLRenderTarget::sCurFBO = mPassFramebuffer;
            LLRenderTarget::sCurResX = width;
            LLRenderTarget::sCurResY = height;
            mPassWidth = width;
            mPassHeight = height;
            setDefaults();
            loadActions(pass);
            applyBindings();
            return true;
        }

        void endRenderPass() override
        {
            if (LLRenderTarget::makesOwnGL())
            {
                if (mOwnPass)
                {
                    closePass(mTracker.endRenderPass(), "endRenderPass");
                }
                else
                {
                    refuseForeignPass("endRenderPass");
                }
                return;
            }
            gGL.flush();
            closePass(mTracker.endRenderPass(), "endRenderPass");
        }

        void clear(bool color, const std::array<F32, 4>& rgba, bool depth, F32 depthValue) override
        {
            if (LLRenderTarget::makesOwnGL())
            {
                // LLRenderTarget's gl_clear clears with GL's own clear colour.
                if (mOwnPass)
                {
                    accept("clear", mTracker.clear(color, depth));
                }
                else
                {
                    refuseForeignPass("clear");
                }
                return;
            }
            gGL.flush();
            if (!accept("clear", mTracker.clear(color, depth)))
            {
                return;
            }
            // As glClear today: the scissor and the masks apply. Color goes per attachment, since
            // glClear is undefined for integer attachments.
            if (color)
            {
                const RenderPassDesc& pass = mTracker.pass();
                for (U32 i = 0; i < pass.colorCount; ++i)
                {
                    clearColorBuffer(i, pass.color[i].texture.get(), rgba);
                }
            }
            if (depth)
            {
                // Not through glClearDepth, which would change raw GL's clears after it.
                glClearBufferfv(GL_DEPTH, 0, &depthValue);
            }
        }

        void setPipeline(Pipeline* pipeline) override
        {
            if (LLVertexBuffer::makesOwnGLDraws())
            {
                // LLVertexBuffer's program and state are set by its GL as today, so its pipeline is only tracked, with no
                // flush and no program lookup.
                keepInterfaceBindings();
                acceptOwnDraw("setPipeline", mTracker.setPipeline(pipeline));
                return;
            }
            gGL.flush();
            restoreInterfaceBindings();
            if (accept("setPipeline", mTracker.setPipeline(pipeline)) && pipeline
                && mPrograms.find(pipeline->programId()) == 0)
            {
                LL_WARNS("RHI") << "setPipeline: the pipeline's program is gone" << LL_ENDL;
                mTracker.setPipeline(nullptr);
            }
            if (mTracker.inPass())
            {
                applyPipeline();
            }
        }

        void setDepthStencilState(DepthStencilState* state) override
        {
            if (LLVertexBuffer::makesOwnGLDraws())
            {
                keepInterfaceBindings();
                acceptOwnDraw("setDepthStencilState", mTracker.setDepthStencilState(state));
                return;
            }
            gGL.flush();
            restoreInterfaceBindings();
            mTracker.setDepthStencilState(state);
            if (mTracker.inPass())
            {
                keepDepthState(state ? state->desc() : DepthStencilDesc());
            }
        }

        // Dynamic state, inside a pass. The tracker refuses it outside one and keeps the pass's record. Inside
        // LLRender::OwnGLState, LLRender's and LLGLState's GL backend functions have made the GL call, so a setter
        // only tracks it, with no flush and no GL.

        void setViewport(S32 x, S32 y, S32 width, S32 height) override
        {
            if (LLRender::makesOwnGLState())
            {
                accept("setViewport", mTracker.setViewport(x, y, width, height));
                return;
            }
            gGL.flush();
            if (!accept("setViewport", mTracker.setViewport(x, y, width, height)))
            {
                return;
            }
            glViewport(x, y, width, height);
        }

        void setScissor(bool enabled, S32 x, S32 y, S32 width, S32 height) override
        {
            // [Viewer Lab] The record, and outside OwnGLState GL's box and LLGLState's map, change behind LLRender::setScissor's
            // skip. In the own pass LLRender::setScissor makes the OwnGLState branch's tracker call and WARN itself, so a
            // change to that branch changes it too.
            gGL.invalidateScissor();
            if (LLRender::makesOwnGLState())
            {
                accept("setScissor", mTracker.setScissor(enabled, x, y, width, height));
                return;
            }
            gGL.flush();
            if (!accept("setScissor", mTracker.setScissor(enabled, x, y, width, height)))
            {
                return;
            }
            KeptGLState scissor(GL_SCISSOR_TEST, enabled);
            glScissor(x, y, width, height);
        }

        void setCullMode(CullMode mode) override
        {
            if (LLRender::makesOwnGLState())
            {
                accept("setCullMode", mTracker.setCullMode(mode));
                return;
            }
            gGL.flush();
            if (!accept("setCullMode", mTracker.setCullMode(mode)))
            {
                return;
            }
            KeptGLState cull(GL_CULL_FACE, mode != CullMode::Off);
            if (mode != CullMode::Off)
            {
                glCullFace(mode == CullMode::Front ? GL_FRONT : GL_BACK);
            }
        }

        void setFillMode(FillMode mode) override
        {
            if (LLRender::makesOwnGLState())
            {
                accept("setFillMode", mTracker.setFillMode(mode));
                return;
            }
            gGL.flush();
            if (!accept("setFillMode", mTracker.setFillMode(mode)))
            {
                return;
            }
            glPolygonMode(GL_FRONT_AND_BACK, mode == FillMode::Lines ? GL_LINE : GL_FILL);
        }

        void setDepthBias(F32 factor, F32 units, bool enableFill, bool enableLines) override
        {
            if (LLRender::makesOwnGLState())
            {
                accept("setDepthBias", mTracker.setDepthBias(factor, units, enableFill, enableLines));
                return;
            }
            gGL.flush();
            if (!accept("setDepthBias", mTracker.setDepthBias(factor, units, enableFill, enableLines)))
            {
                return;
            }
            KeptGLState fill(GL_POLYGON_OFFSET_FILL, enableFill);
            KeptGLState lines(GL_POLYGON_OFFSET_LINE, enableLines);
            glPolygonOffset(factor, units);
        }

        void setDepthClamp(bool enabled) override
        {
            if (LLRender::makesOwnGLState())
            {
                accept("setDepthClamp", mTracker.setDepthClamp(enabled));
                return;
            }
            gGL.flush();
            if (!accept("setDepthClamp", mTracker.setDepthClamp(enabled)))
            {
                return;
            }
            KeptGLState clamp(GL_DEPTH_CLAMP, enabled);
        }

        void setPointSize(F32 size) override
        {
            if (LLRender::makesOwnGLState())
            {
                accept("setPointSize", mTracker.setPointSize(size));
                return;
            }
            gGL.flush();
            if (!accept("setPointSize", mTracker.setPointSize(size)))
            {
                return;
            }
            glPointSize(size);
        }

        void setLineWidth(F32 width) override
        {
            if (LLRender::makesOwnGLState())
            {
                accept("setLineWidth", mTracker.setLineWidth(width));
                return;
            }
            gGL.flush();
            if (!accept("setLineWidth", mTracker.setLineWidth(width)))
            {
                return;
            }
            // LLRender's facade, which gives no send in this scope, so this backend is not called again.
            LLRender::OwnGLState own;
            gGL.setLineWidth(width);
        }

        // Uniforms are the program's own state, so they are set at once, in a pass or not.

        void setUniform(Program* program, U32 uniformIndex, std::span<const std::byte> value, U32 count) override
        {
            if (LLGLSLShader::makesOwnGLUniforms())
            {
                // LLGLSLShader makes today's glUniform* call itself, so its call is only checked; before any flush, so a
                // batch gGL holds is drawn when it would be today.
                accept("setUniform", mTracker.setUniform(program, uniformIndex, value.size(), count));
                return;
            }
            gGL.flush();
            if (!accept("setUniform", mTracker.setUniform(program, uniformIndex, value.size(), count)) || !program)
            {
                return;
            }
            const GLProgram* gl = static_cast<GLProgram*>(program);
            const GLint location = gl->location(uniformIndex);
            if (location < 0)
            {
                return; // the linker kept no place for it
            }
            ProgramForUniforms current(GLNames::of(program));
            const GLfloat* f = reinterpret_cast<const GLfloat*>(value.data());
            const GLint* i = reinterpret_cast<const GLint*>(value.data());
            const GLuint* u = reinterpret_cast<const GLuint*>(value.data());
            const bool isUnsigned = gl->isUnsigned(uniformIndex);
            switch (program->reflection().uniforms[uniformIndex].type)
            {
            case UniformType::Float:  glUniform1fv(location, count, f); break;
            case UniformType::Vec2:   glUniform2fv(location, count, f); break;
            case UniformType::Vec3:   glUniform3fv(location, count, f); break;
            case UniformType::Vec4:   glUniform4fv(location, count, f); break;
            case UniformType::Int:    isUnsigned ? glUniform1uiv(location, count, u) : glUniform1iv(location, count, i); break;
            case UniformType::IVec2:  isUnsigned ? glUniform2uiv(location, count, u) : glUniform2iv(location, count, i); break;
            case UniformType::IVec3:  isUnsigned ? glUniform3uiv(location, count, u) : glUniform3iv(location, count, i); break;
            case UniformType::IVec4:  isUnsigned ? glUniform4uiv(location, count, u) : glUniform4iv(location, count, i); break;
            case UniformType::Mat2:   glUniformMatrix2fv(location, count, GL_FALSE, f); break;
            case UniformType::Mat3:   glUniformMatrix3fv(location, count, GL_FALSE, f); break;
            case UniformType::Mat3x4: glUniformMatrix3x4fv(location, count, GL_FALSE, f); break;
            case UniformType::Mat4:   glUniformMatrix4fv(location, count, GL_FALSE, f); break;
            default:                  break; // samplers go through setSamplerChannel
            }
        }

        void setSamplerChannel(Program* program, U32 uniformIndex, U32 channel) override
        {
            if (LLGLSLShader::makesOwnGLUniforms())
            {
                // LLGLSLShader's uniform1i sets the channel itself, so its call is only checked, before any flush.
                accept("setSamplerChannel", mTracker.setSamplerChannel(program, uniformIndex));
                return;
            }
            gGL.flush();
            if (!accept("setSamplerChannel", mTracker.setSamplerChannel(program, uniformIndex)) || !program)
            {
                return;
            }
            const GLint location = static_cast<GLProgram*>(program)->location(uniformIndex);
            if (location >= 0)
            {
                ProgramForUniforms current(GLNames::of(program));
                glUniform1i(location, GLint(channel));
            }
        }

        // Bindings: kept by the tracker, set on GL now inside a pass and again when a pass begins.

        void setVertexBuffer(Buffer* buffer, const VertexStreams& streams) override
        {
            if (LLVertexBuffer::makesOwnGLDraws())
            {
                // LLVertexBuffer binds its buffers itself and keeps its own record of them.
                keepInterfaceBindings();
                acceptOwnDraw("setVertexBuffer", mTracker.setVertexBuffer(buffer, streams));
                return;
            }
            gGL.flush();
            restoreInterfaceBindings();
            accept("setVertexBuffer", mTracker.setVertexBuffer(buffer, streams));
            if (mTracker.inPass())
            {
                bindVertexBuffer();
            }
        }

        void setIndexBuffer(Buffer* buffer, IndexType type) override
        {
            if (LLVertexBuffer::makesOwnGLDraws())
            {
                keepInterfaceBindings();
                acceptOwnDraw("setIndexBuffer", mTracker.setIndexBuffer(buffer, type));
                return;
            }
            gGL.flush();
            restoreInterfaceBindings();
            accept("setIndexBuffer", mTracker.setIndexBuffer(buffer, type));
            if (mTracker.inPass())
            {
                bindIndexBuffer();
            }
        }

        void setTexture(U32 channel, Texture* texture, const SamplerDesc& sampling) override
        {
            if (LLTexUnit::makesOwnGLBinds())
            {
                // LLTexUnit's binding, which its GL backend functions have made as today: only tracked, with no flush, no
                // GL and no sampling kept, and the interface's binding of the channel kept apart. This backend
                // never reads the texture's facade state.
                keepInterfaceTexture(channel);
                accept("setTexture", mTracker.setTexture(channel, texture));
                return;
            }
            gGL.flush();
            restoreInterfaceTextures();
            const bool accepted = accept("setTexture", mTracker.setTexture(channel, texture));
            mSampling[channel] = sampling;
            if (mTracker.inPass())
            {
                bindTexture(channel, accepted ? texture : nullptr, sampling);
            }
        }

        void setUniformBuffer(U32 blockIndex, Buffer* buffer, U64 offset, U64 size) override
        {
            // [Viewer Lab] An unchanged binding records and binds nothing. Out of a pass the call would only record
            // the same entry; in a pass, GL has this buffer and range from a bind in this same pass (every in-pass change of
            // a slot goes through a bind that rewrites the record). Premise: within a pass, GL points 8 and up are this
            // encoder's alone (raw GL binds only 0-3; raw GL between passes may bind anything, so the record does not
            // survive a pass boundary); after a buffer update from the upload context the cache is off for the device (GL
            // makes another context's change visible only after a new attach). An unchanged binding that GL must bind
            // again in the pass does not go through the tracker again: the tracker holds this buffer from the slot's
            // last set, which it accepted with this range, and the same check would pass again and record the same
            // entry.
            bool unchanged = false;
            if (buffer)
            {
                const auto entry = mBlockRanges.find(blockIndex);
                const auto& held = mTracker.uniformBuffers();
                const auto tracked = held.find(blockIndex);
                unchanged = entry != mBlockRanges.end() && entry->second.offset == offset && entry->second.size == size &&
                            tracked != held.end() && tracked->second.get() == buffer;
                if (unchanged &&
                    (!mTracker.inPass() ||
                     (entry->second.boundPass == mPassSerial && entry->second.boundName != 0 &&
                      entry->second.boundName == GLNames::of(buffer) && !mUploadUpdates.load(std::memory_order_acquire))))
                {
                    return;
                }
            }
            gGL.flush();
            const bool accepted =
                unchanged || accept("setUniformBuffer", mTracker.setUniformBuffer(blockIndex, buffer, offset, size));
            GLBlock& block = mBlockRanges[blockIndex];
            block.offset = offset;
            block.size = size;
            if (mTracker.inPass())
            {
                bindUniformBuffer(blockIndex, accepted ? buffer : nullptr);
            }
        }

        void draw(PrimitiveType type, U32 firstVertex, U32 vertexCount) override
        {
            if (LLVertexBuffer::makesOwnGLDraws())
            {
                // LLVertexBuffer's GL draws it as today; the tracker checks it.
                if (acceptOwnDraw("draw", mTracker.draw(type)))
                {
                    mTracker.drawMade();
                }
                return;
            }
            gGL.flush();
            restoreInterfaceBindings();
            if (accept("draw", mTracker.draw(type)))
            {
                glDrawArrays(LLVertexBuffer::sGLMode[U32(type)], firstVertex, vertexCount);
                mTracker.drawMade();
            }
        }

        void drawIndexed(PrimitiveType type, U32 firstIndex, U32 indexCount) override
        {
            if (LLVertexBuffer::makesOwnGLDraws())
            {
                if (acceptOwnDraw("drawIndexed", mTracker.drawIndexed(type, firstIndex, indexCount)))
                {
                    mTracker.drawMade();
                }
                return;
            }
            gGL.flush();
            restoreInterfaceBindings();
            if (accept("drawIndexed", mTracker.drawIndexed(type, firstIndex, indexCount)))
            {
                const bool u16 = mTracker.indexType() == IndexType::U16;
                glDrawElements(LLVertexBuffer::sGLMode[U32(type)], indexCount,
                               u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                               reinterpret_cast<const void*>(uintptr_t(firstIndex) * (u16 ? 2 : 4)));
                mTracker.drawMade();
            }
        }

        void updateBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data) override
        {
            // LLVertexBuffer's updates never flushed immediate mode, and LLRender::flush itself
            // updates them: flushing here would draw pending vertices out of order and re-enter
            // LLRender::flush while LLVertexBuffer::flushBuffers walks its mapped buffers.
            // [Viewer Lab] Only a buffer that createBuffer makes from the pool can be one, so any other skips the cast
            // (a few hundred ns per uniform-buffer update at startup).
            GLPoolBuffer* pooled = buffer && poolDesc(buffer->desc()) ? dynamic_cast<GLPoolBuffer*>(buffer) : nullptr;
            if (!pooled)
            {
                gGL.flush();
            }
            if (accept("updateBuffer", mTracker.updateBuffer(buffer, offset, data.size())))
            {
                if (pooled)
                {
                    pooled->update(offset, data);
                }
                else
                {
                    updateGLBuffer(buffer, offset, data);
                }
            }
        }

        void uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region,
                           const PixelLayout& layout, std::span<const std::byte> data) override
        {
            if (LLImageGL::makesOwnGLUploads())
            {
                // LLImageGL specifies the level itself, on the texture it has bound, so the call is only checked;
                // before any flush, so a batch gGL holds is drawn when it would be today.
                accept("uploadTexture", mTracker.uploadTexture(texture, level, layer, region, layout, data.size()));
                return;
            }
            gGL.flush();
            if (accept("uploadTexture", mTracker.uploadTexture(texture, level, layer, region, layout, data.size())))
            {
                uploadGLTexture(asGLTexture(texture), level, layer, region, layout, data);
            }
        }

        void copyTexture(Texture* src, U32 srcLevel, U32 srcLayer, const Region& srcRegion,
                         Texture* dst, U32 dstLevel, U32 dstLayer, U32 dstX, U32 dstY) override
        {
            if (LLImageGL::makesOwnGLTransfers())
            {
                // LLGPUCopy copies with today's GL call itself, so the copy is only checked, with no GL call and no
                // flush.
                accept("copyTexture", mTracker.copyTexture(src, srcLevel, srcLayer, srcRegion, dst, dstLevel, dstLayer,
                                                          dstX, dstY));
                return;
            }
            gGL.flush();
            if (!accept("copyTexture", mTracker.copyTexture(src, srcLevel, srcLayer, srcRegion, dst, dstLevel,
                                                           dstLayer, dstX, dstY)))
            {
                return;
            }
            const GLTexture* from = asGLTexture(src);
            GLTexture* to = asGLTexture(dst);
            const bool depth = isDepth(from->desc().format);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, from->isWindow() ? 0 : framebuffer(mReadFramebuffer));
            if (!from->isWindow())
            {
                attachOne(GL_READ_FRAMEBUFFER, depth, from, srcLevel, srcLayer);
            }
            if (!depth)
            {
                glReadBuffer(from->isWindow() ? GL_BACK : GL_COLOR_ATTACHMENT0);
            }
            if (!to->isWindow() && from->desc().samples == 1 && to->desc().samples == 1)
            {
                // A texture copy, which Apple's GL orders with later work on the texture (a blit into
                // it is not, and mipmaps made right after missed it).
                UnitZero unit(to->target(), to->name());
                if (to->desc().type == TextureType::CubeArray)
                {
                    glCopyTexSubImage3D(to->target(), dstLevel, dstX, dstY, dstLayer, srcRegion.x, srcRegion.y,
                                        srcRegion.width, srcRegion.height);
                }
                else
                {
                    glCopyTexSubImage2D(to->faceTarget(dstLayer), dstLevel, dstX, dstY, srcRegion.x, srcRegion.y,
                                        srcRegion.width, srcRegion.height);
                }
            }
            else
            {
                // A resolve of a multisampled target, or a copy into the window: a blit, which the
                // scissor would clip, as copies on Metal are not.
                GLOnlyScope scissor(GL_SCISSOR_TEST, false);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to->isWindow() ? 0 : framebuffer(mDrawFramebuffer));
                if (!to->isWindow())
                {
                    attachOne(GL_DRAW_FRAMEBUFFER, depth, to, dstLevel, dstLayer);
                }
                glBlitFramebuffer(srcRegion.x, srcRegion.y, srcRegion.x + srcRegion.width,
                                  srcRegion.y + srcRegion.height, dstX, dstY, dstX + srcRegion.width,
                                  dstY + srcRegion.height, depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT,
                                  GL_NEAREST);
                if (!to->isWindow())
                {
                    detachOne(GL_DRAW_FRAMEBUFFER, depth);
                }
            }
            if (!from->isWindow())
            {
                detachOne(GL_READ_FRAMEBUFFER, depth);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
        }

        void generateMipmaps(Texture* texture) override
        {
            if (LLRenderTarget::makesOwnGL() || LLImageGL::makesOwnGLUploads())
            {
                // LLRenderTarget's or LLImageGL's gl_generate_mipmap makes them, on the texture it has bound.
                accept("generateMipmaps", mTracker.generateMipmaps(texture));
                return;
            }
            gGL.flush();
            if (accept("generateMipmaps", mTracker.generateMipmaps(texture)))
            {
                generateGLMipmaps(asGLTexture(texture));
            }
        }

        Ref<Readback> readback(Texture* texture, U32 level, U32 layer, const Region& region,
                               const PixelLayout& layout) override
        {
            if (LLImageGL::makesOwnGLTransfers())
            {
                // LLGPUReadback reads with today's GL call into its caller's memory and flushes nothing, so the readback
                // is only checked; it comes back ready and without data.
                if (!accept("readback", mTracker.readback(texture, level, layer, region)))
                {
                    return nullptr;
                }
                return new GLReadback({}, region.width * layout.bytesPerPixel());
            }
            gGL.flush();
            if (!accept("readback", mTracker.readback(texture, level, layer, region)))
            {
                return nullptr;
            }
            const GLTexture* from = asGLTexture(texture);
            const bool depth = isDepth(from->desc().format);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, from->isWindow() ? 0 : framebuffer(mReadFramebuffer));
            if (!from->isWindow())
            {
                attachOne(GL_READ_FRAMEBUFFER, depth, from, level, layer);
            }
            if (!depth)
            {
                // The read buffer matters for color only; depth reads leave it as it is.
                glReadBuffer(from->isWindow() ? GL_BACK : GL_COLOR_ATTACHMENT0);
            }
            GLenum format = toGLPixelFormat(layout);
            if (isInteger(from->desc().format) && format == GL_RED)
            {
                format = GL_RED_INTEGER;
            }
            const U32 rowPitch = region.width * layout.bytesPerPixel();
            std::vector<std::byte> data(size_t(rowPitch) * region.height);
            glReadPixels(region.x, region.y, region.width, region.height, format, toGLPixelType(layout), data.data());
            if (!from->isWindow())
            {
                detachOne(GL_READ_FRAMEBUFFER, depth);
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, LLRenderTarget::sCurFBO);
            return new GLReadback(std::move(data), rowPitch);
        }

        Ref<Readback> readback(Buffer* buffer, U64 offset, U64 size) override
        {
            gGL.flush();
            if (!accept("readback", mTracker.readback(buffer, offset, size)))
            {
                return nullptr;
            }
            std::vector<std::byte> data(size);
            glBindBuffer(GL_COPY_READ_BUFFER, GLNames::of(buffer));
            glGetBufferSubData(GL_COPY_READ_BUFFER, offset, size, data.data());
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
            return new GLReadback(std::move(data), U32(size));
        }

        // Begin and end go to GL at once and leave LLRender's pending batch alone, as today's raw calls
        // do: a query covers what reached GL.
        bool beginQuery(Query* query) override
        {
            if (!accept("beginQuery", mTracker.beginQuery(query)))
            {
                return false;
            }
            const GLQuery* gl = static_cast<GLQuery*>(query);
            glBeginQuery(gl->target(), gl->name());
            return true;
        }

        void endQuery(Query* query) override
        {
            // Held across the end: the tracker's erase may drop the last other reference, and GL ends the query before its
            // name is deleted, as at the frame end and at every call site today.
            const Ref<Query> hold = query;
            const GLenum target = query ? static_cast<GLQuery*>(query)->target() : GL_NONE;
            if (accept("endQuery", mTracker.endQuery(query)))
            {
                glEndQuery(target);
            }
        }

        // LL_PROFILE_GPU_ZONE's groups (LLGPUZone, in developer builds and builds with Tracy).
        // The GL backend sends them to GL on no platform (GL 4.1 on macOS has no
        // debug groups either); the tracker still checks their balance.
        void pushDebugGroup(std::string_view name) override { mTracker.pushDebugGroup(); }
        void popDebugGroup() override { accept("popDebugGroup", mTracker.popDebugGroup()); }

        // Keyed on the fence's type, not on LLImageGL::OwnGLUploads, whose scope is the upload thread's: the upload
        // context's fence inside that scope guards no GL work of this backend, since LLImageGL
        // orders its own uploads before the swap (gl_sync_to_main_thread), so it makes no GL call here; GL's own fence is
        // glWaitSync on this context. Neither changes the tracker's record.
        void waitForFence(Fence* fence) override
        {
            if (const GLFence* gl = dynamic_cast<const GLFence*>(fence))
            {
                gl->waitOnGPU();
            }
            else if (fence && !dynamic_cast<const ImageGLFence*>(fence))
            {
                LL_WARNS("RHI") << "waitForFence: not a fence of the GL backend; nothing waits" << LL_ENDL;
            }
        }

        /// The device's frame end: what is still open is closed, and the frame's passes let go.
        void endFrame()
        {
            gGL.flush();
            // The interface's bindings again, so the frame drops exactly what it drops without LLVertexBuffer's.
            restoreInterfaceBindings();
            // And the interface's textures, so its channels are judged as without LLTexUnit's bindings.
            restoreInterfaceTextures();
            // The tracker forgets queries still active; GL would keep them active for good, and
            // every later query on the target would fail.
            for (const Ref<Query>& query : mTracker.activeQueries())
            {
                glEndQuery(static_cast<GLQuery*>(query.get())->target());
            }
            closePass(mTracker.endFrame(), "endFrame");
        }

        /// [Viewer Lab] The upload context has updated a buffer: from now on setUniformBuffer binds again within a pass
        /// too, since GL makes another context's change visible only after a new attach. Any thread.
        void noteUploadUpdate() { mUploadUpdates.store(true, std::memory_order_release); }

        /// At the device's end: no binding is kept apart any more, and the tracker lets go of everything.
        void reset()
        {
            if (mOwnPass)
            {
                // [Viewer Lab] The device ends with its pass open. Only the main thread's gGL holds the registration, so the
                // last reference must go there (a held device already WARNs at releaseViewerDevice)
                llassert(on_main_thread());
                mOwnPass = false;
                gGL.endOwnPass();
            }
            mInterfaceBindings.reset();
            mInterfaceTextures.clear(); // [Viewer Lab]
            mTracker = EncoderTracker();
        }
        const EncoderTracker& state() const override { return mTracker; }

    private:
        /// At LLVertexBuffer's first binding since the interface's were last moved back: a copy of the interface's.
        void keepInterfaceBindings()
        {
            if (!mInterfaceBindings)
            {
                mInterfaceBindings = mTracker.drawBindings();
            }
        }

        /// The interface's bindings back in the tracker, as they were, with no GL: GL's state is LLVertexBuffer's raw GL,
        /// as today. Nothing when none are kept apart.
        void restoreInterfaceBindings()
        {
            if (mInterfaceBindings)
            {
                mTracker.restoreDrawBindings(std::move(*mInterfaceBindings));
                mInterfaceBindings.reset();
            }
        }

        /// At LLTexUnit's first binding of a channel the interface itself has set (mSampling has it) since the interface's
        /// textures were last moved back: a copy of the interface's binding of the channel, or of none. Channels
        /// the interface never set hold LLTexUnit's bindings alone, which stay across frames as the tracker keeps them.
        void keepInterfaceTexture(U32 channel)
        {
            if (mSampling.find(channel) == mSampling.end() || mInterfaceTextures.find(channel) != mInterfaceTextures.end())
            {
                return;
            }
            const auto& textures = mTracker.textures();
            const auto bound = textures.find(channel);
            mInterfaceTextures[channel] = bound != textures.end() ? bound->second : Ref<Texture>();
        }

        /// The interface's textures back in the tracker, as they were, with no GL: GL has LLTexUnit's bindings, as today.
        /// The tracker accepted each before, and its texture checks do not depend on the pass.
        void restoreInterfaceTextures()
        {
            for (const auto& entry : mInterfaceTextures)
            {
                accept("setTexture", mTracker.setTexture(entry.first, entry.second.get()));
            }
            mInterfaceTextures.clear();
        }

        bool accept(const char* call, const std::string& why)
        {
            if (why.empty())
            {
                return true;
            }
            LL_WARNS("RHI") << call << ": " << why << LL_ENDL;
            return false;
        }

        /// accept on LLVertexBuffer's OwnGLDraws path, where a refusal would repeat at every draw: a WARN once per call and
        /// reason category, so a refusal whose numbers or buffer name change from draw to draw warns once and the set stays
        /// as small as the tracker's reasons.
        bool acceptOwnDraw(const char* call, const std::string& why)
        {
            if (why.empty())
            {
                return true;
            }
            if (mOwnDrawWarnings.insert(std::string(call) + ": " + reasonCategory(why)).second)
            {
                LL_WARNS("RHI") << call << ": " << why << LL_ENDL;
            }
            return false;
        }
        std::set<std::string> mOwnDrawWarnings; ///< the reason categories acceptOwnDraw has warned of, per call

        /// A tracker reason's fixed part: each number (decimal or 0x hex) becomes '#', and each name quoted after a space
        /// becomes '?', as in "indices # to # are beyond index buffer '?'".
        static std::string reasonCategory(const std::string& why)
        {
            std::string category;
            for (size_t i = 0; i < why.size(); ++i)
            {
                const char c = why[i];
                if (c >= '0' && c <= '9')
                {
                    while (i + 1 < why.size() && (std::isxdigit(static_cast<unsigned char>(why[i + 1])) || why[i + 1] == 'x'))
                    {
                        ++i;
                    }
                    category += '#';
                }
                else if (c == '\'' && (i == 0 || why[i - 1] == ' '))
                {
                    const size_t close = why.find('\'', i + 1);
                    if (close == std::string::npos)
                    {
                        category += c;
                        continue;
                    }
                    category += "'?'";
                    i = close;
                }
                else
                {
                    category += c;
                }
            }
            return category;
        }

        /// LLRenderTarget never ends or clears a pass it did not begin.
        void refuseForeignPass(const char* call)
        {
            LL_WARNS("RHI") << call << ": "
                            << (mTracker.inPass() ? "pass '" + mTracker.pass().name + "' is not LLRenderTarget's"
                                                  : std::string("no pass is open"))
                            << LL_ENDL;
        }

        /// Sets the bound pipeline's program, blend and color mask, as LLGLSLShader::bind and
        /// LLRender's setters do.
        void applyPipeline()
        {
            Pipeline* pipeline = mTracker.pipeline();
            if (!pipeline)
            {
                return;
            }
            const GLuint program = mPrograms.find(pipeline->programId());
            if (program == 0)
            {
                // A shader reload let the program go; the draws in this pass are skipped.
                LL_WARNS("RHI") << "The bound pipeline's program is gone" << LL_ENDL;
                mTracker.setPipeline(nullptr);
                return;
            }
            const PipelineDesc& desc = pipeline->desc();
            LLGLSLShader* owner = nullptr;
            if (LLGLSLShader::sCurBoundShader != program)
            {
                // As LLGLSLShader::bind changes programs: the previous shader's profile queries end,
                // and LLVertexBuffer lets go of its buffers, so the interface's are bound again.
                if (LLGLSLShader::sCurBoundShaderPtr)
                {
                    LLGLSLShader::sCurBoundShaderPtr->readProfileQuery();
                }
                LLVertexBuffer::unbind();
                glUseProgram(program);
                LLGLSLShader::sCurBoundShader = program;
                // The shader whose program it is becomes the selected one, as in bind
                owner = owner_of(pipeline->programId());
                LLGLSLShader::sCurBoundShaderPtr = owner;
                bindVertexBuffer();
                bindIndexBuffer();
            }
            LLVertexBuffer::setupClientArrays(desc.attributeMask);
            KeptGLState blend(GL_BLEND, desc.blend.enabled);
            if (desc.blend.enabled)
            {
                gGL.blendFunc(LLRender::eBlendFactor(desc.blend.srcColor), LLRender::eBlendFactor(desc.blend.dstColor),
                              LLRender::eBlendFactor(desc.blend.srcAlpha), LLRender::eBlendFactor(desc.blend.dstAlpha));
            }
            glBlendEquationSeparate(blendOpGL(desc.blend.colorOp), blendOpGL(desc.blend.alphaOp));
            gGL.setColorMask((desc.colorMask & COLOR_MASK_R) != 0, (desc.colorMask & COLOR_MASK_G) != 0,
                             (desc.colorMask & COLOR_MASK_B) != 0, (desc.colorMask & COLOR_MASK_A) != 0);
            // The owner's profile queries begin, as bind places them after a switch, last, once every binding and state
            // above is set
            if (owner)
            {
                owner->placeProfileQuery();
            }
        }

        void bindVertexBuffer()
        {
            Buffer* buffer = mTracker.vertexBuffer();
            const U32 name = GLNames::of(buffer);
            glBindBuffer(GL_ARRAY_BUFFER, name);
            LLVertexBuffer::sGLRenderBuffer = name;
            if (name != 0)
            {
                LLVertexBuffer::setupVertexAttributes(mTracker.streams().attributeMask, mTracker.streams().offset.data());
            }
        }

        void bindIndexBuffer()
        {
            Buffer* buffer = mTracker.indexBuffer();
            const U32 name = GLNames::of(buffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, name);
            LLVertexBuffer::sGLRenderIndices = name;
        }

        void bindTexture(U32 channel, Texture* texture, const SamplerDesc& sampling)
        {
            LLTexUnit* unit = gGL.getTexUnit(channel);
            if (!unit || unit->getIndex() < 0)
            {
                return; // LLRender's stand-in for a unit it does not have
            }
            if (!texture)
            {
                // Nothing: reads (0, 0, 0, 1), as on GL. LLTexUnit::unbind may have left its white
                // texture bound with the unit's cache at 0, so bind 0 for sure.
                gl_bind_raw_name(unit, LLTexUnit::TT_TEXTURE, 0, false);
                unit->activate();
                glBindTexture(GL_TEXTURE_2D, 0);
                return;
            }
            GLTexture* gl = asGLTexture(texture);
            gl_bind_raw_name(unit, gl->unitType(), gl->name(), gl->desc().mipLevels > 1);
            // The raw-name bind keeps the flag when the texture was bound already, perhaps by raw GL
            // without it, and the filter below reads it.
            unit->setHasMipMaps(gl->desc().mipLevels > 1);
            if (gl->desc().samples > 1)
            {
                return; // multisampled textures have no sampling state
            }
            // Sampling as LLTexUnit sets it today: on the texture, one address mode for all axes.
            // The raw-name bind does not make the unit active when the texture is bound already, and the
            // sampling below works on the active unit.
            unit->activate();
            const bool integer = isInteger(gl->desc().format); // integer textures need point sampling
            // [Viewer Lab] LLTexUnit's GL for the sampling, with the description's mips and anisotropy, and no call that
            // reaches the encoder: the same calls as its two setters for every description with the
            // defaults, without their flushes, which had nothing to draw here.
            SamplerDesc effective = sampling;
            effective.filter = integer ? Filter::Point : sampling.filter;
            if (integer)
            {
                effective.magFilter = MagFilter::Nearest; // [Viewer Lab]
            }
            effective.mipmapped = sampling.mipmapped && gl->desc().mipLevels > 1;
            LLTexUnit::applyGLSampling(gl->unitType(), effective);
            glTexParameteri(gl->target(), GL_TEXTURE_COMPARE_MODE, sampling.compare ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
            if (sampling.compare)
            {
                glTexParameteri(gl->target(), GL_TEXTURE_COMPARE_FUNC, compareGL(sampling.compareFunc));
            }
        }

        void bindUniformBuffer(U32 blockIndex, Buffer* buffer)
        {
            GLBlock& block = mBlockRanges[blockIndex];
            if (buffer)
            {
                glBindBufferRange(GL_UNIFORM_BUFFER, BLOCK_BINDING_BASE + blockIndex, GLNames::of(buffer),
                                  block.offset, block.size);
                block.boundName = GLNames::of(buffer); // [Viewer Lab] what GL has on the slot in this pass
            }
            else
            {
                glBindBufferBase(GL_UNIFORM_BUFFER, BLOCK_BINDING_BASE + blockIndex, 0);
                block.boundName = 0;
            }
            block.boundPass = mPassSerial;
        }

        /// When a pass begins: the interface's bindings again, since raw GL between passes may have
        /// bound its own. Every channel and block the interface has set is set again, so one it set
        /// to nothing reads nothing.
        void applyBindings()
        {
            applyPipeline();
            DepthStencilState* depth = mTracker.depthStencilState();
            keepDepthState(depth ? depth->desc() : DepthStencilDesc());
            bindVertexBuffer();
            bindIndexBuffer();
            const auto& textures = mTracker.textures();
            for (const auto& entry : mSampling)
            {
                const auto bound = textures.find(entry.first);
                bindTexture(entry.first, bound != textures.end() ? bound->second.get() : nullptr, entry.second);
            }
            const auto& buffers = mTracker.uniformBuffers();
            for (const auto& entry : mBlockRanges)
            {
                const auto bound = buffers.find(entry.first);
                bindUniformBuffer(entry.first, bound != buffers.end() ? bound->second.get() : nullptr);
            }
        }

        /// A pass's dynamic state when it begins, as a Metal encoder starts (llrhi.h).
        void setDefaults()
        {
            glViewport(0, 0, mPassWidth, mPassHeight);
            KeptGLState scissor(GL_SCISSOR_TEST, false);
            // [Viewer Lab] LLGLState's map behind LLRender::setScissor, also while the viewer's own pass is registered and
            // another GL device begins an interface pass
            gGL.invalidateScissor();
            KeptGLState cull(GL_CULL_FACE, false);
            KeptGLState fill(GL_POLYGON_OFFSET_FILL, false);
            KeptGLState lines(GL_POLYGON_OFFSET_LINE, false);
            KeptGLState clamp(GL_DEPTH_CLAMP, false);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glPolygonOffset(0.f, 0.f);
            glPointSize(1.f);
            {
                LLRender::OwnGLState own; // no send
                gGL.setLineWidth(1.f);
            }
        }

        void clearColorBuffer(U32 index, const Texture* texture, const std::array<F32, 4>& rgba)
        {
            const GLint drawBuffer = mPassFramebuffer == 0 ? 0 : GLint(index);
            if (texture && isInteger(texture->desc().format))
            {
                const GLuint value[4] = { GLuint(rgba[0]), GLuint(rgba[1]), GLuint(rgba[2]), GLuint(rgba[3]) };
                glClearBufferuiv(GL_COLOR, drawBuffer, value);
            }
            else
            {
                glClearBufferfv(GL_COLOR, drawBuffer, rgba.data());
            }
        }

        GLuint framebuffer(GLuint& name)
        {
            if (name == 0)
            {
                glGenFramebuffers(1, &name);
            }
            return name;
        }

        static void attachOne(GLenum target, bool depth, const GLTexture* texture, U32 level, U32 layer)
        {
            const GLenum attachment = !depth ? GL_COLOR_ATTACHMENT0
                                    : hasStencil(texture->desc().format) ? GL_DEPTH_STENCIL_ATTACHMENT
                                                                         : GL_DEPTH_ATTACHMENT;
            if (texture->desc().type == TextureType::CubeArray)
            {
                glFramebufferTextureLayer(target, attachment, texture->name(), level, layer);
            }
            else
            {
                const GLenum face = texture->desc().samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE
                                                                : texture->faceTarget(layer);
                glFramebufferTexture2D(target, attachment, face, texture->name(), level);
            }
        }

        static void detachOne(GLenum target, bool depth)
        {
            if (depth)
            {
                glFramebufferTexture2D(target, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                glFramebufferTexture2D(target, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            }
            else
            {
                glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            }
        }

        static void attachTexture(GLenum attachment, const ColorAttachment* color, const DepthAttachment* depth)
        {
            const Ref<Texture>& ref = color ? color->texture : depth->texture;
            const U32 level = color ? color->level : depth->level;
            const U32 layer = color ? color->layer : depth->layer;
            const GLTexture* texture = asGLTexture(ref.get());
            if (!texture)
            {
                glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
                return;
            }
            if (texture->desc().type == TextureType::CubeArray)
            {
                glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, texture->name(), level, layer);
                return;
            }
            const GLenum face = texture->desc().samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : texture->faceTarget(layer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, face, texture->name(), level);
        }

        void attach(const RenderPassDesc& pass)
        {
            GLenum buffers[PipelineDesc::MAX_COLOR_ATTACHMENTS] = {};
            for (U32 i = 0; i < PipelineDesc::MAX_COLOR_ATTACHMENTS; ++i)
            {
                const bool used = i < pass.colorCount;
                if (used || i < mAttachedColors)
                {
                    attachTexture(GL_COLOR_ATTACHMENT0 + i, &pass.color[i], nullptr);
                }
                buffers[i] = used ? GLenum(GL_COLOR_ATTACHMENT0 + i) : GLenum(GL_NONE);
            }
            mAttachedColors = pass.colorCount;
            const bool stencil = pass.depth.texture.notNull() && hasStencil(pass.depth.texture->desc().format);
            glFramebufferTexture2D(GL_FRAMEBUFFER, stencil ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_TEXTURE_2D, 0, 0);
            attachTexture(stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, nullptr, &pass.depth);
            if (pass.colorCount == 0)
            {
                glDrawBuffer(GL_NONE);
                glReadBuffer(GL_NONE);
            }
            else
            {
                glDrawBuffers(pass.colorCount, buffers);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
            }
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE)
            {
                LL_WARNS("RHI") << "Pass '" << pass.name << "': the framebuffer is not complete (0x" << std::hex
                                << status << std::dec << ")" << LL_ENDL;
            }
        }

        // The load actions as clears with nothing masked, then the masks as they were.
        void loadActions(const RenderPassDesc& pass)
        {
            bool clearColor = false;
            for (U32 i = 0; i < pass.colorCount; ++i)
            {
                clearColor = clearColor || pass.color[i].load == LoadAction::Clear;
            }
            const bool clearDepth = pass.depth.texture.notNull() && pass.depth.load == LoadAction::Clear;
            if (!clearColor && !clearDepth)
            {
                return;
            }
            GLOnlyScope scissor(GL_SCISSOR_TEST, false);
            GLboolean mask[4];
            glGetBooleanv(GL_COLOR_WRITEMASK, mask);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            for (U32 i = 0; i < pass.colorCount; ++i)
            {
                if (pass.color[i].load != LoadAction::Clear)
                {
                    continue;
                }
                clearColorBuffer(i, pass.color[i].texture.get(), pass.color[i].clear);
            }
            glColorMask(mask[0], mask[1], mask[2], mask[3]);
            if (clearDepth)
            {
                LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);
                glClearBufferfv(GL_DEPTH, 0, &pass.depth.clear);
            }
        }

        void closePass(const EncoderTracker::Closing& closing, const char* call)
        {
            if (!closing.warning.empty())
            {
                LL_WARNS("RHI") << call << ": " << closing.warning << LL_ENDL;
            }
            if (!closing.endPass)
            {
                return;
            }
            if (mOwnPass)
            {
                // LLRenderTarget's pass saved and bound nothing, so nothing is let go of or put back.
                mOwnPass = false;
                gGL.endOwnPass(); // [Viewer Lab]
                return;
            }
            if (mPassFramebuffer != 0)
            {
                // Let go of the attachments, so GL holds no texture for a pass that has ended. Raw GL
                // in the pass may have bound another framebuffer, so bind the pass's first.
                glBindFramebuffer(GL_FRAMEBUFFER, mPassFramebuffer);
                for (U32 i = 0; i < mAttachedColors; ++i)
                {
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, 0, 0);
                }
                mAttachedColors = 0;
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            }
            // Back to the framebuffer LLRenderTarget had bound, as LLRenderTarget::flush returns.
            glBindFramebuffer(GL_FRAMEBUFFER, mPreviousFramebuffer);
            LLRenderTarget::sCurFBO = mPreviousFramebuffer;
            LLRenderTarget::sCurResX = mPreviousWidth;
            LLRenderTarget::sCurResY = mPreviousHeight;
            mSaved.restore(); // the viewport too
        }

        EncoderTracker mTracker;
        /// The interface's draw bindings while the tracker holds LLVertexBuffer's; empty otherwise.
        std::optional<EncoderTracker::DrawBindings> mInterfaceBindings;
        /// The interface's bindings of its own channels while the tracker holds LLTexUnit's there; empty otherwise.
        std::unordered_map<U32, Ref<Texture>> mInterfaceTextures;
        ProgramRegistry& mPrograms;
        SavedGLState mSaved;
        std::unordered_map<U32, SamplerDesc> mSampling;           ///< by channel, no references
        /// [Viewer Lab] A block's asked range, and what bindUniformBuffer last bound in GL on its slot and in which pass
        /// (setUniformBuffer's cache)
        struct GLBlock
        {
            U64 offset = 0;
            U64 size = 0;
            U32 boundName = 0;
            U64 boundPass = 0;
        };
        std::unordered_map<U32, GLBlock> mBlockRanges; ///< by block
        U64 mPassSerial = 0;                           ///< beginRenderPass counts it up
        std::atomic<bool> mUploadUpdates{ false };     ///< the upload context has updated a buffer
        GLuint mFramebuffer = 0;
        GLuint mReadFramebuffer = 0;
        GLuint mDrawFramebuffer = 0;
        GLuint mPassFramebuffer = 0;
        bool mOwnPass = false; ///< the open pass is LLRenderTarget's
        U32 mAttachedColors = 0;
        U32 mPassWidth = 0;
        U32 mPassHeight = 0;
        U32 mPreviousFramebuffer = 0;
        U32 mPreviousWidth = 0;
        U32 mPreviousHeight = 0;
    };

    /// The upload thread's context on GL: the thread's own shared GL context, as LLImageGLThread
    /// has today; submit() sends the work with a fence: a GLFence, or inside LLImageGL's
    /// scope an ImageGLFence, since LLImageGL synchronizes its own uploads.
    class GLUploadContext final : public UploadContext
    {
    public:
        explicit GLUploadContext(GLEncoder& encoder) : mEncoder(encoder) {}

        void updateBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data) override
        {
            if (dynamic_cast<GLPoolBuffer*>(buffer))
            {
                accept("updateBuffer",
                       "a dynamic vertex or index buffer is LLVertexBuffer's and is updated through the main thread's encoder");
                return;
            }
            const std::string why = buffer ? validateRange(buffer->desc(), offset, data.size()) : "no buffer";
            if (accept("updateBuffer", why))
            {
                mEncoder.noteUploadUpdate(); // [Viewer Lab] the encoder's bind cache off within a pass
                updateGLBuffer(buffer, offset, data);
            }
        }

        void uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region,
                           const PixelLayout& layout, std::span<const std::byte> data) override
        {
            const std::string why = texture ? validateUpload(texture->desc(), level, layer, region, layout, data.size())
                                            : "no texture";
            // Inside LLImageGL's scope LLImageGL specifies the level itself, so the call is only checked.
            if (accept("uploadTexture", why) && !LLImageGL::makesOwnGLUploads())
            {
                uploadGLTexture(asGLTexture(texture), level, layer, region, layout, data);
            }
        }

        void generateMipmaps(Texture* texture) override
        {
            const std::string why = texture ? validateMipmaps(texture->desc()) : "no texture";
            if (accept("generateMipmaps", why) && !LLImageGL::makesOwnGLUploads())
            {
                generateGLMipmaps(asGLTexture(texture));
            }
        }

        Ref<Fence> submit() override
        {
            if (LLImageGL::makesOwnGLUploads())
            {
                return new ImageGLFence;
            }
            return new GLFence;
        }

    private:
        static bool accept(const char* call, const std::string& why)
        {
            if (why.empty())
            {
                return true;
            }
            LL_WARNS("RHI") << "Upload " << call << ": " << why << LL_ENDL;
            return false;
        }

        GLEncoder& mEncoder; ///< the device's, whose bind cache a buffer update turns off within a pass
    };

    /// The window's framebuffer on GL. The window presents and sets the swap interval itself
    /// (LLWindow), so present() and setVSync() leave that to it.
    class GLSurface final : public Surface
    {
    public:
        GLSurface(U32 width, U32 height)
        {
            TextureDesc color;
            color.width = width;
            color.height = height;
            color.usage = TEXTURE_RENDER_TARGET | TEXTURE_READBACK;
            mColor = new GLTexture(color, GLTexture::WindowFramebuffer{});
            TextureDesc depth = color;
            depth.format = Format::Depth32Float;
            depth.usage = TEXTURE_RENDER_TARGET | TEXTURE_READBACK; // rawSnapshot reads the window's depth
            mDepth = new GLTexture(depth, GLTexture::WindowFramebuffer{});
        }

        Texture* backBuffer() override { return mColor.get(); }
        Texture* backBufferDepth() override { return mDepth.get(); }
        void resize(U32 width, U32 height) override
        {
            mColor->resize(width, height);
            mDepth->resize(width, height);
        }
        void setVSync(bool enabled) override {}
        void present() override {}

    private:
        Ref<GLTexture> mColor;
        Ref<GLTexture> mDepth;
    };

    // The device ---------------------------------------------------------------------------------

    class GLDevice final : public Device
    {
    public:
        // GL keeps its own order of work and deletes in-use objects late by itself, so a frame
        // has finished, as far as the lifecycle is concerned, once it is sent.
        explicit GLDevice(const DeviceCreateInfo& info)
        :   mLifecycle([](U64) { return true; }), mCaps(capabilitiesFromGL()), mEncoder(mCaps, mPrograms),
            mUploads(mEncoder)
        {
            // A window gives a surface; a device without one (tests, the golden-image program)
            // renders into textures only.
            if (info.width > 0 && info.height > 0)
            {
                mSurface = new GLSurface(info.width, info.height);
            }
        }

        ~GLDevice() override
        {
            mEncoder.reset();
            mSurface = nullptr;
            mPipelines.clear();
            mDepthStencils.clear();
            mSamplers.clear();
            mLifecycle.releaseAll();
            MainThreadDeletes::flush();
        }

        BackendType backend() const override { return BackendType::GL; }
        const Capabilities& capabilities() const override { return mCaps; }

        Ref<Buffer> createBuffer(const BufferDesc& desc, std::span<const std::byte> initial,
                                 std::string_view debugName) override
        {
            std::string why = validate(desc, initial.size());
            if (!why.empty())
            {
                warnCreateFailed("buffer", debugName, why);
                return nullptr;
            }
            // Dynamic vertex and index buffers are LLVertexBuffer's, from its VBO pool.
            // LLVertexBuffer never asked GL for errors when it allocated, so this path calls no
            // glGetError.
            if (poolDesc(desc))
            {
                if (!on_main_thread())
                {
                    warnCreateFailed("buffer", debugName,
                                     "dynamic vertex and index buffers are LLVertexBuffer's and are made on the main thread");
                    return nullptr;
                }
                if (desc.size > U32_MAX)
                {
                    warnCreateFailed("buffer", debugName, "too large for LLVertexBuffer's pool");
                    return nullptr;
                }
                Ref<Buffer> buffer = GLPoolBuffer::create(desc, initial);
                if (buffer.isNull())
                {
                    warnCreateFailed("buffer", debugName, "LLVertexBuffer has no buffer pool (LLVertexBuffer::initClass)");
                    return nullptr;
                }
                buffer->setDebugName(debugName);
                return buffer;
            }
            takeGLError();
            Ref<Buffer> buffer = new GLBuffer(desc, initial);
            why = takeGLError();
            if (!why.empty())
            {
                warnCreateFailed("buffer", debugName, why);
                return nullptr;
            }
            buffer->setDebugName(debugName);
            return buffer;
        }

        Ref<Texture> createTexture(const TextureDesc& desc, std::string_view debugName) override
        {
            std::string why = validate(desc, mCaps);
            if (!why.empty())
            {
                warnCreateFailed("texture", debugName, why);
                return nullptr;
            }
            // LLImageGL's own textures: LLImageGL makes their GL storage itself and never asked GL
            // for errors when it made a texture, so this path makes no GL call but the name pool's, and no
            // glGetError.
            if (LLImageGL::makesOwnGLStorage())
            {
                Ref<Texture> texture = new GLTexture(desc, GLTexture::ImageGLName{});
                texture->setDebugName(debugName);
                return texture;
            }
            takeGLError();
            Ref<Texture> texture = new GLTexture(desc);
            why = takeGLError();
            if (!why.empty())
            {
                warnCreateFailed("texture", debugName, why);
                return nullptr;
            }
            texture->setDebugName(debugName);
            return texture;
        }

        Ref<Sampler> getSampler(const SamplerDesc& desc) override
        {
            const std::string why = validate(desc, mCaps); // [Viewer Lab]
            if (!why.empty())
            {
                warnCreateFailed("sampler", {}, why);
                return nullptr;
            }
            return mSamplers.get(desc, [](const SamplerDesc& wanted) -> Ref<Sampler>
            {
                return new GLSampler(wanted);
            });
        }

        Ref<Program> createProgram(const ProgramDesc& desc) override
        {
            std::string why = validate(desc, mCaps);
            if (why.empty())
            {
                // LLGLSLShader's own program: LLGLSLShader makes, compiles and links it itself as before, so
                // this path makes no GL call and no glGetError.
                if (const GLuint own = LLGLSLShader::ownGLProgram())
                {
                    bool deferred = false;
                    if (takeProgramFault(desc.name, deferred)) // golden level 2's test fault
                    {
                        return new FaultedProgram(desc, own, deferred);
                    }
                    U32 frames = 0;
                    const bool delayed = takeProgramDelay(desc.name, frames); // the test delay, which the fault wins
                    return new GLProgram(desc, own, mPipelines, mPrograms, GLProgram::ShaderProgram{}, delayed ? this : nullptr,
                                         frames);
                }
                takeGLError();
                Ref<Program> program = GLProgram::create(desc, mPipelines, mPrograms, why);
                if (program.notNull())
                {
                    return program;
                }
            }
            warnCreateFailed("program", desc.name, why);
            return nullptr;
        }

        // One glGenQueries for the batch, and the queries in GL's order. Each one's target is fixed here, from the
        // rule that gives the tracker its concurrency.
        std::vector<Ref<Query>> createQueries(QueryType type, U32 count) override
        {
            if (!on_main_thread())
            {
                warnCreateFailed("query", {}, "queries belong to the main thread's context and are made there");
                return {};
            }
            const std::string why = validateQuery(type, mCaps);
            if (!why.empty())
            {
                warnCreateFailed("query", {}, why);
                return {};
            }
            std::vector<Ref<Query>> queries;
            if (count == 0)
            {
                return queries;
            }
            std::vector<GLuint> names(count);
            glGenQueries(GLsizei(count), names.data());
            const GLenum target = queryTarget(type, mCaps.concurrentSampleQueries);
            queries.reserve(count);
            for (GLuint name : names)
            {
                queries.push_back(new GLQuery(type, target, name, mCaps.timerQueries));
            }
            return queries;
        }

        Ref<Pipeline> getPipeline(Program& program, const PipelineDesc& desc) override
        {
            std::string why = validate(desc, mCaps);
            if (why.empty() && program.status() == CompileStatus::Failed)
            {
                why = "the program failed to compile";
            }
            if (!why.empty())
            {
                warnCreateFailed("pipeline", program.debugName(), why);
                return nullptr;
            }
            return mPipelines.get({ program.id(), desc }, [&program](const PipelineKey& key) -> Ref<Pipeline>
            {
                // [Viewer Lab] Named after its program, as Metal's pipelines are, for the tracker's refusals
                Ref<Pipeline> pipeline = new GLPipeline(key);
                pipeline->setDebugName(program.debugName());
                return pipeline;
            });
        }

        Ref<DepthStencilState> getDepthStencilState(const DepthStencilDesc& desc) override
        {
            return mDepthStencils.get(desc, [](const DepthStencilDesc& wanted) -> Ref<DepthStencilState>
            {
                return new GLDepthStencilState(wanted);
            });
        }

        Surface* surface() override { return mSurface.get(); }
        Encoder& encoder() override { return mEncoder; }
        UploadContext& uploadContext() override { return mUploads; }

        void beginFrame() override {}

        void endFrame() override
        {
            // The encoder first, so what it lets go belongs to the frame that ends.
            mEncoder.endFrame();
            mLifecycle.frameCompleted(mLifecycle.currentFrame());
            mLifecycle.endFrame();
            MainThreadDeletes::flush();
        }

        U64 frameNumber() const override { return mLifecycle.currentFrame(); }
        U64 completedFrame() const override { return mLifecycle.completedFrame(); }

        void submitAndWait() override
        {
            glFinish();
            mLifecycle.idle();
        }

        /// The GL program of a pipeline's program id, or 0 when the program is gone.
        GLuint programName(U64 programId) { return mPrograms.find(programId); }
        size_t pipelineCount() const { return mPipelines.size(); }

    private:
        FrameLifecycle mLifecycle;
        Capabilities mCaps;
        DescCache<SamplerDesc, Sampler, SamplerDescHash> mSamplers;
        DescCache<DepthStencilDesc, DepthStencilState, DepthStencilDescHash> mDepthStencils;
        ProgramRegistry mPrograms;
        PipelineCache mPipelines;
        GLEncoder mEncoder;
        GLUploadContext mUploads;
        Ref<GLSurface> mSurface;
    };

    S32 integerGL(GLenum name)
    {
        GLint value = 0;
        glGetIntegerv(name, &value);
        return value;
    }

    // While LLGLManager is not initialized, the window makes its GL context. The viewer has one
    // window, as LL_COORD_TYPE_WINDOW's conversions assume (llwindow.cpp); the tests and the golden level-1 program
    // have none, and then nothing is asked.
    bool makeWindowGLContext()
    {
        LLWindow::instance_snapshot windows;
        if (windows.begin() == windows.end())
        {
            return true;
        }
        return windows.begin()->createGLContext();
    }
}

Capabilities capabilitiesFromGL()
{
    // The GL version the facades' checks see: after LLFeatureManager's Intel 3.3 fallback, also
    // when the device is made before it.
    const F32 version = gGLManager.needsIntelGL33Fallback() ? llmin(gGLManager.mGLVersion, 3.33f) : gGLManager.mGLVersion;

    Capabilities caps;
    caps.backend = BackendType::GL;

    caps.maxTextureSize = gGLManager.mGLMaxTextureSize;
    caps.maxArrayLayers = integerGL(GL_MAX_ARRAY_TEXTURE_LAYERS);
    caps.maxColorAttachments = integerGL(GL_MAX_COLOR_ATTACHMENTS);
    caps.maxSamples = gGLManager.mMaxSamples;
    caps.maxSamplersPerStage = gGLManager.mNumTextureImageUnits;
    caps.maxTexturesPerStage = gGLManager.mNumTextureImageUnits;
    caps.maxUniformBlockSize = gGLManager.mMaxUniformBlockSize;
    caps.maxVaryingVectors = gGLManager.mMaxVaryingVectors;
    caps.maxVertexAttributes = integerGL(GL_MAX_VERTEX_ATTRIBS);
    caps.lineWidthRange[0] = gGLManager.mAliasedLineRange.mV[0];
    caps.lineWidthRange[1] = gGLManager.mAliasedLineRange.mV[1];
    glGetFloatv(GL_POINT_SIZE_RANGE, caps.pointSizeRange);
    caps.maxAnisotropy = gGLManager.mMaxAnisotropy;

    caps.cubeMapArrays = gGLManager.mHasCubeMapArray;
    caps.anisotropicFiltering = gGLManager.mHasAnisotropic;
    // LLImageGL::setManualImage's >= 3.29 after the fallback, and allocateConversionBuffer's
    // < CONVERSION_SCRATCH_BUFFER_GL_VERSION before it, which agree.
    caps.textureSwizzle = version >= 3.29f;
    caps.bcCompression = gGLManager.mGLExtensions.count("GL_EXT_texture_compression_s3tc") > 0;
    // LLPipeline::profileAvatar's < 3.25 and gpu_benchmark's < 3.3 agree for GL 3.2 and 3.3; the
    // midpoint does not depend on how 3.3 rounds as a float.
    caps.timerQueries = version >= 3.25f;
    caps.timerQueriesWithinPass = caps.timerQueries;
    caps.benchmarkTimers = caps.timerQueries; // gpu_benchmark asks this
    // LLOcclusionCullingGroup::doOcclusion's mGLVersion >= 3.3f: from GL 3.3 "at least one sample" has a GL target of
    // its own.
    caps.concurrentSampleQueries = version >= 3.3f;
    caps.primitivesGeneratedQuery = true;
    caps.geometryShaders = true;  // core since GL 3.2
    caps.compute = version >= 4.29f;
    caps.depthClamp = true;       // core since GL 3.2
    caps.debugLabels = gGLManager.mHasDebugOutput;

    // The checks the facades make today, at the version they see.
    caps.programBinaries = version >= 4.09f; // LLShaderMgr::initShaderCache
    caps.sharedContextUploads = version > 3.95f; // LLImageGL::initClass
    caps.pixelBufferObjects = true; // no version check guards LLImageGL's PBO paths
    caps.copyBetweenFormats = true; // glCopyTexSubImage2D converts

    // The GPU workarounds LLShaderMgr assembles into the GLSL today.
#if LL_DARWIN
    caps.shaderOldSelect = !gGLManager.mIsApple; // LLGLSLShader::createShader's and LLShaderMgr::loadShaderFile's !mIsApple
#endif
    caps.shaderAMDCard = gGLManager.mIsAMD;      // LLShaderMgr::loadShaderFile's mIsAMD
    caps.shaderIfChain = gGLManager.mIsNVIDIA;   // LLShaderMgr::loadShaderFile's mIsNVIDIA in diffuseLookup()

    caps.vramMB = gGLManager.mVRAM;
    caps.unifiedMemory = gGLManager.mIsApple;
    caps.isApple = gGLManager.mIsApple;
    caps.vendor = gGLManager.mGLVendor;
    caps.renderer = gGLManager.mGLRenderer;
    caps.driverVersion = gGLManager.mGLVersionString;
    return caps;
}

namespace
{
    /// The object as the GL class T when it is one, by its dynamic type (each GL class is final), else null: a load and a
    /// compare, as the virtual call it replaces.
    template <class T, class Base>
    const T* exactly(const Base* object)
    {
        return object && typeid(*object) == typeid(T) ? static_cast<const T*>(object) : nullptr;
    }
}

namespace GLNames
{
U32 of(const Buffer* buffer)
{
    if (const GLPoolBuffer* pooled = exactly<GLPoolBuffer>(buffer))
    {
        return pooled->name();
    }
    const GLBuffer* gl = exactly<GLBuffer>(buffer);
    return gl ? gl->name() : 0;
}

U32 of(const Texture* texture)
{
    const GLTexture* gl = exactly<GLTexture>(texture);
    return gl ? gl->name() : 0;
}

U32 of(const Program* program)
{
    if (const GLProgram* gl = exactly<GLProgram>(program))
    {
        return gl->name();
    }
    const FaultedProgram* faulted = exactly<FaultedProgram>(program);
    return faulted ? faulted->name() : 0;
}

U32 of(const LLImageGL* image)
{
    return image->getTexName();
}

U32 of(LLCubeMap* cube)
{
    return cube->getGLName();
}

U32 of(LLCubeMapArray* array)
{
    return array->getGLName();
}

U32 of(const LLGLSLShader& shader)
{
    return shader.mProgramObject;
}

void assign(LLGLSLShader& shader, U32 program)
{
    shader.mProgramObject = program;
}

U32 of(const LLImageGLHandle& handle)
{
    return handle.mName;
}

LLImageGLHandle handle(U32 name)
{
    return LLImageGLHandle(name);
}

Ref<Texture> alias(const TextureDesc& desc, U32 name)
{
    return new GLTexture(desc, GLTexture::BorrowedName{}, name);
}

void generate(S32 count, U32* names)
{
    LLImageGL::generateTextures(count, names);
}

void release(S32 count, const U32* names)
{
    LLImageGL::deleteTextures(count, names);
}

void specifyBound(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, U32 format, U32 type,
                  const void* pixels, bool allowCompression)
{
    LLImageGL::setManualImage(target, level, internalFormat, width, height, format, type, pixels, allowCompression);
}

bool bind(LLTexUnit* unit, U32 type, U32 name, bool hasMips)
{
    return gl_bind_raw_name(unit, LLTexUnit::eTextureType(type), name, hasMips);
}
} // namespace GLNames

Ref<Device> createGLDevice(const DeviceCreateInfo& info)
{
    if (!gGLManager.mInited && !makeWindowGLContext())
    {
        warnCreateFailed("device", "GL", "the window could not make its GL context");
        return nullptr;
    }
    if (!gGLManager.mInited)
    {
        warnCreateFailed("device", "GL", "LLGLManager is not initialized");
        return nullptr;
    }
    return new GLDevice(info);
}

} // namespace LL::RHI
