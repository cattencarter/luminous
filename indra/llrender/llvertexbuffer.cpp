/**
 * @file llvertexbuffer.cpp
 * @brief LLVertexBuffer implementation
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

#include "linden_common.h"

#include "llfasttimer.h"
#include "llsys.h"
#include "llvertexbuffer.h"
// #include "llrender.h"
#include "llglheaders.h"
#include "llrender.h"
#include "llvector4a.h"
#include "llshadermgr.h"
#include "llglslshader.h"
#include "llmemory.h"
#include "llrhi.h"
#include "llrhigl.h" // [Viewer Lab] GLNames
#include "llthread.h"
#include "llrendertarget.h" // [Viewer Lab]
#include <glm/gtc/type_ptr.hpp>
#include <set> // [Viewer Lab]
#include <unordered_map> // [Viewer Lab]
#include <vector> // [Viewer Lab]

//Next Highest Power Of Two
//helper function, returns first number > v that is a power of 2, or v if v is already a power of 2
U32 nhpo2(U32 v)
{
    U32 r = 1;
    while (r < v) {
        r *= 2;
    }
    return r;
}

//which power of 2 is i?
//assumes i is a power of 2 > 0
U32 wpo2(U32 i)
{
    llassert(i > 0);
    llassert(nhpo2(i) == i);

    U32 r = 0;

    while (i >>= 1) ++r;

    return r;
}

struct CompareMappedRegion
{
    bool operator()(const LLVertexBuffer::MappedRegion& lhs, const LLVertexBuffer::MappedRegion& rhs)
    {
        return lhs.mStart < rhs.mStart;
    }
};

//============================================================================
// Pool of reusable VertexBuffer state

// batch calls to glGenBuffers
static GLuint gen_buffer()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;

    GLuint ret = 0;
    constexpr U32 pool_size = 4096;

    thread_local static GLuint sNamePool[pool_size];
    thread_local static U32 sIndex = 0;

    if (sIndex == 0)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("gen buffer");
        sIndex = pool_size;
#if !LL_DARWIN
        if (!gGLManager.mIsAMD)
        {
            glGenBuffers(pool_size, sNamePool);
        }
        else
#endif
        { // work around for AMD driver bug
            for (U32 i = 0; i < pool_size; ++i)
            {
                glGenBuffers(1, sNamePool + i);
            }
        }
    }

    ret = sNamePool[--sIndex];
    return ret;
}

static void delete_buffers(S32 count, GLuint* buffers)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    // wait a few frames before actually deleting the buffers to avoid
    // synchronization issues with the GPU
    static std::vector<GLuint> sFreeList[4];

    if (gGLManager.mInited)
    {
        U32 idx = LLImageGL::sFrameCount % 4;

        for (S32 i = 0; i < count; ++i)
        {
            sFreeList[idx].push_back(buffers[i]);
        }

        idx = (LLImageGL::sFrameCount + 3) % 4;

        if (!sFreeList[idx].empty())
        {
            glDeleteBuffers((GLsizei)sFreeList[idx].size(), sFreeList[idx].data());
            sFreeList[idx].resize(0);
        }
    }
}


// [Viewer Lab] The GL backend functions below do nothing when GL is not the backend chosen at
// startup, as llgl.cpp's gl_is_backend and llrender.cpp's drain_gl_errors.
static bool gl_is_backend()
{
    return LL::RHI::viewerBackend() == LL::RHI::BackendType::GL;
}

// [Viewer Lab] LLVertexBuffer's record of the buffer bound on a target
static U32& bound_gl_name(GLenum type)
{
    return type == GL_ELEMENT_ARRAY_BUFFER ? LLVertexBuffer::sGLRenderIndices : LLVertexBuffer::sGLRenderBuffer;
}

// [Viewer Lab] Binds a buffer when LLVertexBuffer's record names another one, as _unmapBuffer does
static void rebind_gl_buffer(GLenum type, GLuint name)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llvertexbuffer:rebind_gl_buffer");
        return;
    }
    U32& bound = bound_gl_name(type);
    if (bound != name)
    {
        glBindBuffer(type, name);
        bound = name;
    }
}

// [Viewer Lab] setBuffer's GL: the array buffer's bind with the attribute setup for the program's mask, the setup alone when
// only the mask changed, and the element buffer's bind, each when LLVertexBuffer's record of GL's bindings names another
// buffer or mask, with the record, in LLVertexBuffer's order. Only GL code reads that record.
static void gl_set_buffer(U32 vertices, U32 indices, U32 data_mask, const U32* offsets)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llvertexbuffer:gl_set_buffer");
        return;
    }
    if (LLVertexBuffer::sGLRenderBuffer != vertices)
    {
        glBindBuffer(GL_ARRAY_BUFFER, vertices);
        LLVertexBuffer::sGLRenderBuffer = vertices;

        LLVertexBuffer::setupVertexAttributes(data_mask, offsets);
    }
    else if (LLVertexBuffer::sLastMask != data_mask)
    {
        LLVertexBuffer::setupVertexAttributes(data_mask, offsets);
        LLVertexBuffer::sLastMask = data_mask;
    }

    if (indices != LLVertexBuffer::sGLRenderIndices)
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
        LLVertexBuffer::sGLRenderIndices = indices;
    }
}

// [Viewer Lab] unbind's GL: no array or element buffer bound, with the record
static void gl_unbind_buffers()
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llvertexbuffer:gl_unbind_buffers");
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    LLVertexBuffer::sGLRenderBuffer = 0;
    LLVertexBuffer::sGLRenderIndices = 0;
}

// [Viewer Lab] drawRange's and drawRangeFast's draw in GL's mode, so fans, line loops and strips draw as GL draws them
static void gl_draw_range_elements(U32 mode, U32 start, U32 end, U32 count, U32 type, size_t offset)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llvertexbuffer:gl_draw_range_elements");
        return;
    }
    glDrawRangeElements(LLVertexBuffer::sGLMode[mode], start, end, count, type, (GLvoid*)offset);
}

// [Viewer Lab] drawArrays' draw in GL's mode; true when it made the call, so drawArrays tells whether
// the draw was delivered without asking which backend runs
static bool gl_draw_arrays(U32 mode, U32 first, U32 count)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llvertexbuffer:gl_draw_arrays");
        return false;
    }
    glDrawArrays(LLVertexBuffer::sGLMode[mode], first, count);
    return true;
}

// [Viewer Lab] Whether LLVertexBuffer's record of GL's bindings names these buffers, as the draws assert; true when GL is not
// the backend. Unused where llassert is compiled out.
[[maybe_unused]] static bool gl_bound(U32 vertices, U32 indices)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llvertexbuffer:gl_bound");
        return true;
    }
    return vertices == LLVertexBuffer::sGLRenderBuffer && indices == LLVertexBuffer::sGLRenderIndices;
}

// [Viewer Lab] The census of the draws and setBuffer calls since the process began (LLVertexBuffer::DrawCounts): plain
// counts, since LLVertexBuffer binds and draws on the main thread only; no GL.
static LLVertexBuffer::DrawCounts sDrawCounts;
// [Viewer Lab] The draws per shader while LLVertexBuffer::sTallyPrograms is set, with the name taken at its first draw
// Keyed by the shader, so a tallied draw costs a hash of a pointer, not string compares; programTally()
// gives them by name.
struct ProgramTallyEntry
{
    std::string name;
    U64 draws = 0;
};
static std::unordered_map<const LLGLSLShader*, ProgramTallyEntry> sProgramTally;
bool LLVertexBuffer::sTallyPrograms = false;

// [Viewer Lab] One draw: with a shader bound, with a program of the viewer's current device, with the part's storage on the
// device (storage), and an indexed draw whose indices end past the part's
static void count_draw(bool storage, bool indices_beyond)
{
    ++sDrawCounts.draws;
    if (const LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr)
    {
        ++sDrawCounts.drawsWithShader;
        if (LLVertexBuffer::sTallyPrograms)
        {
            ProgramTallyEntry& entry = sProgramTally[shader];
            if (entry.draws++ == 0)
            {
                entry.name = shader->mName;
            }
        }
        const LL::RHI::Device* device = LL::RHI::viewerDevice();
        if (device && shader->getRHIProgram() && shader->getRHIDevice() == device->id())
        {
            ++sDrawCounts.drawsWithProgram;
        }
    }
    sDrawCounts.drawsWithStorage += storage ? 1 : 0;
    sDrawCounts.indicesBeyondBuffer += indices_beyond ? 1 : 0;
}

// [Viewer Lab]
//static
void LLVertexBuffer::noteSkippedDraw()
{
    count_draw(false, false);
    ++sDrawCounts.skipped;
}

// [Viewer Lab] A member so the tests reach it
//static
bool LLVertexBuffer::streamsBeyond(U32 mask, const U32* offsets, U32 size)
{
    for (U32 i = 0; i < TYPE_MAX; ++i)
    {
        if ((mask & (1u << i)) && offsets[i] >= size)
        {
            return true;
        }
    }
    return false;
}

// [Viewer Lab] The program last found pipeline ready by a draw, by id, which is never reused
static U64 sReadyProgram = 0;
// [Viewer Lab] The shaders and generations whose skipped draws have warned
static std::set<std::pair<const LLGLSLShader*, U32>> sSkipWarned;

// [Viewer Lab] Whether a draw goes on with the bound shader: not for a shader bind() refused (its record, no GL
// name), nor for one whose pipeline failed after its publication; a shader without an RHI program (hand-made, or made
// without the device) draws on GL alone. The draw waits for 'pipeline ready' only with an RHI program. No GL.
static bool shader_can_draw()
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    if (!shader)
    {
        return true;
    }
    if (shader->drawsRefused()) // [Viewer Lab] as LLRender::flush asks
    {
        if (sSkipWarned.emplace(shader, shader->getGeneration()).second)
        {
            LL_WARNS("Shader") << "Draws of program " << shader->mName << " are skipped: bind refused it" << LL_ENDL;
        }
        return false;
    }
    if (!shader->isComplete())
    {
        // Unloaded or rebuilt while still current, after bind() bound it complete: drawn on GL alone
        return true;
    }
    const LL::RHI::Program* program = shader->getRHIProgram();
    if (!program)
    {
        return true;
    }
    if (program->id() == sReadyProgram)
    {
        return true;
    }
    if (!shader->isPipelineReady())
    {
        shader->waitForPipeline();
        if (!shader->isPipelineReady())
        {
            if (sSkipWarned.emplace(shader, shader->getGeneration()).second)
            {
                LL_WARNS("Shader") << "Draws of program " << shader->mName << " are skipped: its pipeline failed" << LL_ENDL;
            }
            return false;
        }
    }
    sReadyProgram = program->id();
    return true;
}

// [Viewer Lab] True while LLVertexBuffer gives a draw to the viewer's encoder (LLVertexBuffer::OwnGLDraws); main
// thread, as LLRenderTarget's OwnGL
static bool sMakesOwnGLDraws = false;

namespace
{
    // [Viewer Lab] LLGLState's record of an enable, found without inserting into it as operator[] would, so gDebugGL's
    // checkStates sees the map unchanged; off, GL's default, when the record has none
    class GLStateRecord : public LLGLState
    {
    public:
        static bool isEnabled(LLGLenum state)
        {
            const auto it = sStateMap.find(state);
            return it != sStateMap.end() && it->second == GL_TRUE;
        }
    };

    // [Viewer Lab] A draw's compact pipeline key: describePipeline's inputs, with the program's id and the pass formats'
    // serial, so that equal keys describe equal pipelines within one tracker
    struct DrawKey
    {
        U64 program = 0;
        U64 formats = 0;
        U32 mask = 0;
        U8 primitiveClass = 0;
        bool blend = false;
        std::array<U8, 4> factors{};
        U8 colorMask = 0;

        bool operator==(const DrawKey&) const = default;
    };

    // [Viewer Lab] One way of the pipeline table: a key with the pass's formats by value in place of their serial, which
    // changes when formats alternate, and the device's answer, null too, so a description it refuses is asked for, and
    // warns, once
    struct PipelineWay
    {
        bool used = false;
        U64 program = 0;
        U32 mask = 0;
        U8 primitiveClass = 0;
        bool blend = false;
        std::array<U8, 4> factors{};
        U8 colorMask = 0;
        LL::RHI::PassFormats formats;
        LL::RHI::Pipeline* pipeline = nullptr;

        bool matches(const DrawKey& key, const LL::RHI::PassFormats& pass) const
        {
            return used && program == key.program && mask == key.mask && primitiveClass == key.primitiveClass
                   && blend == key.blend && factors == key.factors && colorMask == key.colorMask && formats == pass;
        }
    };

    // [Viewer Lab] The pipeline table's sets, of 2 ways each. A probe found at most 55 distinct keys in a frame of the
    // golden level 2 scenes, and no miss after the first frame.
    // 4096 sets: made once per record, no per-frame cost; 1024 missed ~6 a frame in overview-smaa-sample.
    constexpr U32 PIPELINE_SETS = 4096;
    // [Viewer Lab] The depth table's slots, [test][write][func]
    constexpr U32 DEPTH_SLOTS = 2 * 2 * 8;

    // [Viewer Lab] encodeDraw's records for the viewer's device: plain values and raw pointers into the device's caches,
    // never Refs. The device's PipelineCache keeps a pipeline until its program dies, and program ids are never
    // reused, so a way is matched only through a live shader's program id; depth states live as long as the device. A new
    // device id clears them all.
    struct EncodeRecord
    {
        U64 device = 0;
        U64 passSerial = ~U64(0);
        U64 formatsSerial = ~U64(0);
        LL::RHI::PassFormats formats;
        U64 formatsHash = 0;
        bool keyKnown = false;
        DrawKey key;
        LL::RHI::Pipeline* wanted = nullptr; ///< the last key's pipeline, null when the device refused its description
        std::vector<std::array<PipelineWay, 2>> pipelines; ///< PIPELINE_SETS sets, made at the first lookup
        std::array<LL::RHI::DepthStencilState*, DEPTH_SLOTS> depths{};
    };

    EncodeRecord sEncode;

    // [Viewer Lab] A 64-bit mix for the table's set
    U64 mix_bits(U64 x)
    {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    }

    // [Viewer Lab]
    U64 formats_hash(const LL::RHI::PassFormats& formats)
    {
        U64 hash = mix_bits(U64(formats.colorCount) ^ (U64(formats.depthFormat) << 8) ^ (U64(formats.samples) << 24));
        for (LL::RHI::Format format : formats.colorFormats)
        {
            hash = mix_bits(hash ^ U64(format));
        }
        return hash;
    }
}

// [Viewer Lab] The draw's key: the program's id and the bound shader's mask, the pass formats' serial, the mode's primitive
// class, LLGLState's GL_BLEND and LLRender's factors and colour mask
static DrawKey draw_key(U64 program, U32 mask, U32 mode, U64 formats)
{
    DrawKey key;
    key.program = program;
    key.mask = mask;
    key.formats = formats;
    key.primitiveClass = U8(LL::RHI::primitiveClass(LL::RHI::PrimitiveType(mode)));
    key.blend = GLStateRecord::isEnabled(GL_BLEND);
    LLRender::eBlendFactor factors[4];
    gGL.getBlendFunc(factors[0], factors[1], factors[2], factors[3]);
    for (U32 i = 0; i < 4; ++i)
    {
        key.factors[i] = U8(factors[i]);
    }
    bool r, g, b, a;
    gGL.getColorMask(r, g, b, a);
    key.colorMask = U8((r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
    return key;
}

// [Viewer Lab] The pipeline of a key from the 2-way table, which compares the whole key with the pass's formats, so a hash
// collision only misses. A hit in way 1 swaps the ways. A miss describes the pipeline, asks the device and keeps its
// answer, null too, in way 0, with the old way 0 moved to way 1.
static LL::RHI::Pipeline* find_pipeline(LL::RHI::Device& device, LL::RHI::Program& program, EncodeRecord& record,
                                        const DrawKey& key, U32 mode)
{
    if (record.pipelines.empty())
    {
        record.pipelines.resize(PIPELINE_SETS);
    }
    const U64 blend = U64(key.blend) | (U64(key.factors[0]) << 1) | (U64(key.factors[1]) << 5) | (U64(key.factors[2]) << 9)
                      | (U64(key.factors[3]) << 13) | (U64(key.colorMask) << 17);
    const U64 hash = mix_bits(key.program ^ mix_bits(U64(key.mask) | (U64(key.primitiveClass) << 32)) ^ record.formatsHash
                              ^ mix_bits(blend + 1));
    std::array<PipelineWay, 2>& set = record.pipelines[hash & (PIPELINE_SETS - 1)];
    if (set[0].matches(key, record.formats))
    {
        return set[0].pipeline;
    }
    if (set[1].matches(key, record.formats))
    {
        std::swap(set[0], set[1]);
        return set[0].pipeline;
    }
    LL::RHI::PipelineDesc desc;
    LLVertexBuffer::describePipeline(mode, key.mask, record.formats, desc);
    sDrawCounts.attributeMaskMismatches += key.mask != program.reflection().attributeMask ? 1 : 0;
    const LLPointer<LL::RHI::Pipeline> pipeline = device.getPipeline(program, desc);
    ++sDrawCounts.pipelineRequests;
    set[1] = set[0];
    set[0] = { true, key.program, key.mask, key.primitiveClass, key.blend, key.factors, key.colorMask, record.formats,
               pipeline.get() };
    return pipeline.get();
}

// [Viewer Lab] The encoder lets go of a part's storage as it is destroyed, so its pool name is freed at once: the
// tracker holds a bound buffer until it is rebound or the frame ends. Main thread, with the viewer's device; inside
// OwnGLDraws, so GL is untouched.
static void release_encoder_binding(const LL::RHI::Buffer* buffer, bool indices)
{
    if (!on_main_thread())
    {
        return;
    }
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device)
    {
        return;
    }
    LL::RHI::Encoder& encoder = device->encoder();
    const LL::RHI::EncoderTracker& state = encoder.state();
    if ((indices ? state.indexBuffer() : state.vertexBuffer()) != buffer)
    {
        return;
    }
    LLVertexBuffer::OwnGLDraws own;
    if (indices)
    {
        encoder.setIndexBuffer(nullptr, LL::RHI::IndexType::U16);
    }
    else
    {
        encoder.setVertexBuffer(nullptr, LL::RHI::VertexStreams());
    }
}


#define ANALYZE_VBO_POOL 0

// VBO Pool interface
class LLVBOPool
{
    public:
    virtual ~LLVBOPool() = default;
    // [Viewer Lab] GL names, their updates and the CPU copies as separate calls, for the render
    // interface's GL backend. allocate leaves the name 0 when the pool names a buffer at its
    // first update. update is GL's part of updating bytes [offset, offset + bytes) of a buffer of
    // size bytes, and may give the buffer a new name. replacesWhole says that an update gives the
    // buffer new memory, so it is updated whole. allocateCopy and freeCopy give the CPU copy
    // LLVertexBuffer maps, with the pool's own reuse; CPU only.
    virtual void allocate(GLenum type, U32 size, GLuint& name) = 0;
    virtual void free(GLenum type, U32 size, GLuint name) = 0;
    virtual void update(GLenum type, U32 size, GLuint& name, U32 offset, const U8* data, U32 bytes) = 0;
    virtual bool replacesWhole() const = 0;
    // [Viewer Lab] True when the parts' storage has no GL name, so a part with a CPU copy has storage
    // although its name is 0
    virtual bool storageHasNoGLName() const = 0;
    virtual U8* allocateCopy(GLenum type, U32 size) = 0;
    virtual void freeCopy(GLenum type, U32 size, U8* data) = 0;
    virtual U64 getVramBytesUsed() = 0;
};

// VBO Pool for Apple GPUs (as in M1/M2 etc, not Intel macs)
// Effectively disables VBO pooling
class LLAppleVBOPool final: public LLVBOPool
{
public:
    U64 mAllocated = 0;

    U64 getVramBytesUsed() override
    {
        return mAllocated;
    }

    void allocate(GLenum type, U32 size, GLuint& name) override // [Viewer Lab]
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        STOP_GLERROR;
        llassert(type == GL_ARRAY_BUFFER || type == GL_ELEMENT_ARRAY_BUFFER);
        llassert(name == 0); // non zero name indicates a gl name that wasn't freed
        llassert(size >= 2);  // any buffer size smaller than a single index is nonsensical

        mAllocated += size;

        { //allocate a new buffer
            LL_PROFILE_GPU_ZONE("vbo alloc");
            // ON OS X, we don't allocate a VBO until the last possible moment
            // in unmapBuffer
            STOP_GLERROR;
        }
    }

    void free(GLenum type, U32 size, GLuint name) override // [Viewer Lab]
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(type == GL_ARRAY_BUFFER || type == GL_ELEMENT_ARRAY_BUFFER);
        llassert(size >= 2);

        mAllocated -= size;
        STOP_GLERROR;
        if (name)
        {
            delete_buffers(1, &name);
        }
        STOP_GLERROR;
    }

    // [Viewer Lab] _unmapBuffer's Apple path: a whole update makes a new GL name with the whole
    // copy. A part, which only interface callers send, goes into the buffer.
    void update(GLenum type, U32 size, GLuint& name, U32 offset, const U8* data, U32 bytes) override
    {
        if (offset == 0 && bytes == size)
        {
            if (name)
            {
                delete_buffers(1, &name);
            }
            name = gen_buffer();
            glBindBuffer(type, name);
            bound_gl_name(type) = name;
            glBufferData(type, size, data, GL_STATIC_DRAW);
        }
        else
        {
            if (!name)
            {
                name = gen_buffer();
                glBindBuffer(type, name);
                bound_gl_name(type) = name;
                glBufferData(type, size, nullptr, GL_STATIC_DRAW);
            }
            else
            {
                rebind_gl_buffer(type, name);
            }
            glBufferSubData(type, offset, bytes, data);
        }
    }

    bool replacesWhole() const override { return true; } // [Viewer Lab]
    bool storageHasNoGLName() const override { return false; } // [Viewer Lab]

    // [Viewer Lab] The CPU copy, as allocate and free made it
    U8* allocateCopy(GLenum type, U32 size) override
    {
        return (U8*) ll_aligned_malloc_16(size);
    }

    void freeCopy(GLenum type, U32 size, U8* data) override // [Viewer Lab]
    {
        if (data)
        {
            ll_aligned_free_16(data);
        }
    }
};

// VBO Pool for GPUs that benefit from VBO pooling
class LLDefaultVBOPool final : public LLVBOPool
{
public:
    typedef std::chrono::steady_clock::time_point Time;
    struct Entry
    {
        GLuint mGLName; // [Viewer Lab] the CPU copy is in mVBOCopies and mIBOCopies
        Time mAge;
    };

    ~LLDefaultVBOPool() override
    {
        clear();
    }

    typedef std::unordered_map<U32, std::list<Entry>> Pool;

    Pool mVBOPool;
    Pool mIBOPool;

    // [Viewer Lab] The CPU copies LLVertexBuffer maps, reused by adjusted size as they were with the
    // names
    struct Copy
    {
        U8* mData;
        Time mAge;
    };
    typedef std::unordered_map<U32, std::list<Copy>> Copies;

    Copies mVBOCopies;
    Copies mIBOCopies;

    U32 mTouchCount = 0;

    U64 mDistributed = 0;
    U64 mAllocated = 0;
    U64 mReserved = 0;
    U32 mMisses = 0;
    U32 mHits = 0;

    U64 getVramBytesUsed() override
    {
        return mAllocated + mReserved;
    }

    // increase the size to some common value (e.g. a power of two) to increase hit rate
    void adjustSize(U32& size)
    {
        // size = nhpo2(size);  // (193/303)/580 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 66 percent. Hit rate: 77 percent

        //(245/276)/385 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 57 percent. Hit rate: 69 percent
        //(187/209)/397 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 112 percent. Hit rate: 76 percent
        U32 block_size = llmax(nhpo2(size) / 8, (U32) 16);
        size += block_size - (size % block_size);
    }

    void allocate(GLenum type, U32 size, GLuint& name) override // [Viewer Lab]
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(type == GL_ARRAY_BUFFER || type == GL_ELEMENT_ARRAY_BUFFER);
        llassert(name == 0); // non zero name indicates a gl name that wasn't freed
        llassert(size >= 2);  // any buffer size smaller than a single index is nonsensical

        mDistributed += size;
        adjustSize(size);
        mAllocated += size;

        auto& pool = type == GL_ELEMENT_ARRAY_BUFFER ? mIBOPool : mVBOPool;

        Pool::iterator iter = pool.find(size);
        if (iter == pool.end())
        { // cache miss, allocate a new buffer
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vbo pool miss");
            LL_PROFILE_GPU_ZONE("vbo alloc");

            mMisses++;
            name = gen_buffer();
            glBindBuffer(type, name);
            glBufferData(type, size, nullptr, GL_DYNAMIC_DRAW);
            if (type == GL_ELEMENT_ARRAY_BUFFER)
            {
                LLVertexBuffer::sGLRenderIndices = name;
            }
            else
            {
                LLVertexBuffer::sGLRenderBuffer = name;
            }
        }
        else
        {
            mHits++;
            llassert(mReserved >= size); // assert if accounting gets messed up
            mReserved -= size;

            std::list<Entry>& entries = iter->second;
            Entry& entry = entries.back();
            name = entry.mGLName;

            entries.pop_back();
            if (entries.empty())
            {
                pool.erase(iter);
            }
        }

        clean();
    }

    void free(GLenum type, U32 size, GLuint name) override // [Viewer Lab]
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(type == GL_ARRAY_BUFFER || type == GL_ELEMENT_ARRAY_BUFFER);
        llassert(size >= 2);
        llassert(name != 0);

        clean();

        llassert(mDistributed >= size);
        mDistributed -= size;
        adjustSize(size);
        llassert(mAllocated >= size);
        mAllocated -= size;
        mReserved += size;

        auto& pool = type == GL_ELEMENT_ARRAY_BUFFER ? mIBOPool : mVBOPool;

        Pool::iterator iter = pool.find(size);

        if (iter == pool.end())
        {
            std::list<Entry> newlist;
            newlist.push_front({ name, std::chrono::steady_clock::now() }); // [Viewer Lab]
            pool[size] = newlist;
        }
        else
        {
            iter->second.push_front({ name, std::chrono::steady_clock::now() }); // [Viewer Lab]
        }

    }

    // [Viewer Lab] flush_vbo's glBufferSubData in 64 KB blocks, after a bind when LLVertexBuffer's
    // record names another buffer
    void update(GLenum type, U32 size, GLuint& name, U32 offset, const U8* data, U32 bytes) override
    {
        rebind_gl_buffer(type, name);

        const U32 start = offset;
        const U32 end = offset + bytes - 1;
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("glBufferSubData");
        LL_PROFILE_ZONE_NUM(start);
        LL_PROFILE_ZONE_NUM(end);
        LL_PROFILE_ZONE_NUM(end-start);

        constexpr U32 block_size = 65536;

        for (U32 i = start; i <= end; i += block_size)
        {
            //LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("glBufferSubData block");
            //LL_PROFILE_GPU_ZONE("glBufferSubData");
            U32 tend = llmin(i + block_size, end);
            glBufferSubData(type, i, tend - i + 1, data + (i-start));
        }
    }

    bool replacesWhole() const override { return false; } // [Viewer Lab]
    bool storageHasNoGLName() const override { return false; } // [Viewer Lab]

    // [Viewer Lab] A miss mallocs the adjusted size, a hit hands back the oldest copy of that size,
    // as allocate hands back names
    U8* allocateCopy(GLenum type, U32 size) override
    {
        adjustSize(size);
        Copies& copies = type == GL_ELEMENT_ARRAY_BUFFER ? mIBOCopies : mVBOCopies;
        Copies::iterator iter = copies.find(size);
        if (iter == copies.end())
        {
            return (U8*) ll_aligned_malloc_16(size);
        }
        U8* data = iter->second.back().mData;
        iter->second.pop_back();
        if (iter->second.empty())
        {
            copies.erase(iter);
        }
        return data;
    }

    void freeCopy(GLenum type, U32 size, U8* data) override // [Viewer Lab]
    {
        adjustSize(size);
        (type == GL_ELEMENT_ARRAY_BUFFER ? mIBOCopies : mVBOCopies)[size].push_front({ data, std::chrono::steady_clock::now() });
    }

    // clean periodically (clean gets called for every alloc/free)
    void clean()
    {
        mTouchCount++;
        if (mTouchCount < 1024) // clean every 1k touches
        {
            return;
        }
        mTouchCount = 0;

        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;

        std::unordered_map<U32, std::list<Entry>>* pools[] = { &mVBOPool, &mIBOPool };

        using namespace std::chrono_literals;

        Time cutoff = std::chrono::steady_clock::now() - 5s;

        for (auto* pool : pools)
        {
            for (Pool::iterator iter = pool->begin(); iter != pool->end(); )
            {
                auto& entries = iter->second;

                while (!entries.empty() && entries.back().mAge < cutoff)
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vbo cache timeout");
                    auto& entry = entries.back();
                    delete_buffers(1, &entry.mGLName);
                    llassert(mReserved >= iter->first);
                    mReserved -= iter->first;
                    entries.pop_back();

                }

                if (entries.empty())
                {
                    iter = pool->erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
        }

        // [Viewer Lab] The CPU copies time out with the same cutoff
        Copies* copy_lists[] = { &mVBOCopies, &mIBOCopies };
        for (Copies* copies : copy_lists)
        {
            for (Copies::iterator iter = copies->begin(); iter != copies->end(); )
            {
                auto& entries = iter->second;
                while (!entries.empty() && entries.back().mAge < cutoff)
                {
                    ll_aligned_free_16(entries.back().mData);
                    entries.pop_back();
                }
                if (entries.empty())
                {
                    iter = copies->erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
        }

#if 0
        LL_INFOS() << llformat("(%d/%d)/%d MB (distributed/allocated)/total in VBO Pool. Overhead: %d percent. Hit rate: %d percent",
            mDistributed / 1000000,
            mAllocated / 1000000,
            (mAllocated + mReserved) / 1000000, // total bytes
            ((mAllocated+mReserved-mDistributed)*100)/llmax(mDistributed, (U64) 1), // overhead percent
            (mHits*100)/llmax(mMisses+mHits, (U32)1)) // hit rate percent
            << LL_ENDL;
#endif
    }

    void clear()
    {
        for (auto& entries : mIBOPool)
        {
            for (auto& entry : entries.second)
            {
                delete_buffers(1, &entry.mGLName);
            }
        }

        for (auto& entries : mVBOPool)
        {
            for (auto& entry : entries.second)
            {
                delete_buffers(1, &entry.mGLName);
            }
        }

        // [Viewer Lab]
        for (Copies* copies : { &mIBOCopies, &mVBOCopies })
        {
            for (auto& entries : *copies)
            {
                for (auto& copy : entries.second)
                {
                    ll_aligned_free_16(copy.mData);
                }
            }
            copies->clear();
        }

        mReserved = 0;

        mIBOPool.clear();
        mVBOPool.clear();
    }
};

// [Viewer Lab] The CPU copies LLVertexBuffer maps when GL is not the backend: the parts' storage is
// the render device's, so this pool names nothing, and gl_pool() keeps allocate, update and free from
// being called. An update gives the device's buffer the whole copy, and the bytes used are those of
// the live copies, as Apple's GL pool counts the sizes asked for
class LLDeviceCopyPool final : public LLVBOPool
{
public:
    U64 mAllocated = 0;

    U64 getVramBytesUsed() override
    {
        return mAllocated;
    }

    void allocate(GLenum type, U32 size, GLuint& name) override
    {
        llassert(false); // gl_pool() is null when GL is not the backend
        name = 0;
    }

    void free(GLenum type, U32 size, GLuint name) override
    {
        llassert(false);
    }

    void update(GLenum type, U32 size, GLuint& name, U32 offset, const U8* data, U32 bytes) override
    {
        llassert(false);
    }

    bool replacesWhole() const override { return true; }
    bool storageHasNoGLName() const override { return true; }

    U8* allocateCopy(GLenum type, U32 size) override
    {
        U8* data = (U8*) ll_aligned_malloc_16(size);
        if (data)
        {
            mAllocated += size;
        }
        return data;
    }

    void freeCopy(GLenum type, U32 size, U8* data) override
    {
        if (data)
        {
            llassert(mAllocated >= size);
            mAllocated -= size;
            ll_aligned_free_16(data);
        }
    }
};

static LLVBOPool* sVBOPool = nullptr;

// [Viewer Lab] The GL backend's VBO pool, or none when GL is not the backend
static LLVBOPool* gl_pool()
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llvertexbuffer:gl_pool");
        return nullptr;
    }
    return sVBOPool;
}

// [Viewer Lab] The GL storage of LLVertexBuffer's parts: the pool's GL calls, memory accounting and
// deletion delays. The facade and the render interface's GL backend both come here.
static bool gl_storage_allocate(GLenum type, U32 size, GLuint& name)
{
    LLVBOPool* pool = gl_pool();
    if (!pool)
    {
        return false;
    }
    pool->allocate(type, size, name);
    return true;
}

static void gl_storage_update(GLenum type, U32 size, GLuint& name, U32 offset, const U8* data, U32 bytes) // [Viewer Lab]
{
    if (LLVBOPool* pool = gl_pool())
    {
        pool->update(type, size, name, offset, data, bytes);
    }
}

static void gl_storage_free(GLenum type, U32 size, GLuint name) // [Viewer Lab]
{
    if (LLVBOPool* pool = gl_pool())
    {
        pool->free(type, size, name);
    }
}

// [Viewer Lab] The CPU copy LLVertexBuffer maps, with the pool's reuse; CPU only, so no backend
// test. Called only where the facade has tested sVBOPool.
static U8* allocate_cpu_copy(GLenum type, U32 size)
{
    return sVBOPool->allocateCopy(type, size);
}

static void free_cpu_copy(GLenum type, U32 size, U8* data) // [Viewer Lab]
{
    sVBOPool->freeCopy(type, size, data);
}

// [Viewer Lab] The GL backend's choice of VBO pool; the
// device's CPU copies when GL is not the backend
static LLVBOPool* create_vbo_pool()
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llvertexbuffer:create_vbo_pool");
        return new LLDeviceCopyPool();
    }
#if LL_DARWIN || LL_ARM64
    if (gGLManager.mIsApple)
    {
        LL_INFOS() << "VBO Pooling Disabled" << LL_ENDL;
        return new LLAppleVBOPool();
    }
    else
#endif
    {
        LL_INFOS() << "VBO Pooling Enabled" << LL_ENDL;
        return new LLDefaultVBOPool();
    }
}

// [Viewer Lab] The pool's answer to whether an update gives a buffer new memory, so LLVertexBuffer
// sends the whole CPU copy once per unmap instead of each mapped range. On GL
// sVBOPool is gl_pool(); the device's copy pool answers too
static bool whole_buffer_updates()
{
    return sVBOPool && sVBOPool->replacesWhole();
}

// [Viewer Lab] Apple's pool is the one that replaces a buffer whole, and it names a part at its first update
//static
bool LLVertexBuffer::bindsBeforeData()
{
    LLVBOPool* pool = gl_pool();
    return pool && !pool->replacesWhole();
}

// [Viewer Lab] A part's storage on the viewer's render device. Without the device
// (headless, llappearanceutility, the unit tests' fixture) or off the main thread the storage is the
// GL backend's pool (the branch LLGPUZone makes, on the device and never on the backend).
static LLPointer<LL::RHI::Buffer> create_rhi_storage(U8 usage, U32 size)
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || !on_main_thread())
    {
        return nullptr;
    }
    LL::RHI::BufferDesc desc;
    desc.size = size;
    desc.usage = usage;
    desc.dynamic = true;
    return device->createBuffer(desc);
}

// [Viewer Lab] The GL name that LLVertexBuffer's GL backend functions bind and draw with, 0 off GL; the
// GL backend's state in llrender
static U32 gl_name(const LLPointer<LL::RHI::Buffer>& buffer)
{
    return LL::RHI::GLNames::of(buffer.get());
}

// [Viewer Lab] Kept, never released, where LLVertexBuffer frees nothing (after cleanupClass, and
// createGLIndices' test of the GL name alone), so no render interface object dies in static
// destruction and the pool's count is unchanged
static void keep_unreleased(LLPointer<LL::RHI::Buffer>& buffer)
{
    buffer->ref();
    buffer = nullptr;
}

// [Viewer Lab] The display list entries that hold each texture of the device, by the texture's id, which let go of it at
// its owner's release. The entries of an id form an intrusive doubly-linked list through mTrackedPrev and mTrackedNext, and
// LLTrackedEntryTable (llvertexbuffer.h) maps the id to its list's first entry. Main thread only. Made once and never
// destroyed, so entries destroyed in static destruction still find it
using TrackedEntries = LLTrackedEntryTable<LLVertexBufferData>;

static TrackedEntries& tracked_entries()
{
    static auto* entries = new TrackedEntries();
    return *entries;
}

// [Viewer Lab] Only an entry made on the main thread is tracked; one made elsewhere keeps its texture until it goes. It
// becomes the first of its id's list
void LLVertexBufferData::track()
{
    llassert(mTrackedId == 0);
    if (mTexture.notNull() && on_main_thread())
    {
        const U64 id = mTexture->id();
        if (id != 0)
        {
            LLVertexBufferData*& first = tracked_entries().first(id);
            mTrackedPrev = nullptr;
            mTrackedNext = first;
            if (mTrackedNext)
            {
                mTrackedNext->mTrackedPrev = this;
            }
            first = this;
            mTrackedId = id;
        }
    }
}

// [Viewer Lab] Only the first of a list touches the table
void LLVertexBufferData::untrack()
{
    if (mTrackedId == 0)
    {
        return;
    }
    llassert(on_main_thread());
    if (mTrackedNext)
    {
        mTrackedNext->mTrackedPrev = mTrackedPrev;
    }
    if (mTrackedPrev)
    {
        mTrackedPrev->mTrackedNext = mTrackedNext;
    }
    else
    {
        tracked_entries().setFirst(mTrackedId, mTrackedNext);
    }
    mTrackedId = 0;
    mTrackedPrev = nullptr;
    mTrackedNext = nullptr;
}

// [Viewer Lab]
//static
void LLVertexBufferData::releaseTexture(U64 texture_id)
{
    llassert(on_main_thread());
    // Out of the table first, and the texture kept until the walk is done, so letting go destroys nothing during the walk
    LLVertexBufferData* entry = tracked_entries().take(texture_id);
    if (!entry)
    {
        return;
    }
    const LLPointer<LL::RHI::Texture> texture = entry->mTexture;
    while (entry)
    {
        LLVertexBufferData* next = entry->mTrackedNext;
        entry->mTrackedId = 0;
        entry->mTrackedPrev = nullptr;
        entry->mTrackedNext = nullptr;
        entry->mTexture = nullptr; // mTextureId and mTexName stay; draw binds the GL name raw
        entry = next;
    }
}

// [Viewer Lab]
//static
U32 LLVertexBufferData::trackedEntries(U64 texture_id)
{
    llassert(on_main_thread());
    U32 count = 0;
    for (const LLVertexBufferData* entry = tracked_entries().peek(texture_id); entry; entry = entry->mTrackedNext)
    {
        ++count;
    }
    return count;
}

// [Viewer Lab]
LLVertexBufferData::LLVertexBufferData()
    : mVB(nullptr)
    , mMode(0)
    , mCount(0)
    , mTextureId(0)
    , mTexName(0)
    , mProjection(glm::identity<glm::mat4>())
    , mModelView(glm::identity<glm::mat4>())
    , mTexture0(glm::identity<glm::mat4>())
{}

// [Viewer Lab]
LLVertexBufferData::LLVertexBufferData(LLVertexBuffer* buffer, U8 mode, U32 count, LL::RHI::Texture* texture, U64 texture_id, U32 tex_name, const glm::mat4& model_view, const glm::mat4& projection, const glm::mat4& texture0)
    : mVB(buffer)
    , mMode(mode)
    , mCount(count)
    , mTexture(texture)
    , mTextureId(texture_id)
    , mTexName(tex_name)
    , mProjection(model_view)
    , mModelView(projection)
    , mTexture0(texture0)
{
    track(); // [Viewer Lab]
}

// [Viewer Lab] Member-wise, and tracked as an entry of its own: mTrackedId is not copied
LLVertexBufferData::LLVertexBufferData(const LLVertexBufferData& other)
    : mVB(other.mVB)
    , mMode(other.mMode)
    , mCount(other.mCount)
    , mTexture(other.mTexture)
    , mTextureId(other.mTextureId)
    , mTexName(other.mTexName)
    , mProjection(other.mProjection)
    , mModelView(other.mModelView)
    , mTexture0(other.mTexture0)
{
    track();
}

// [Viewer Lab]
LLVertexBufferData& LLVertexBufferData::operator=(const LLVertexBufferData& other)
{
    if (this != &other)
    {
        untrack();
        mVB = other.mVB;
        mMode = other.mMode;
        mCount = other.mCount;
        mTexture = other.mTexture;
        mTextureId = other.mTextureId;
        mTexName = other.mTexName;
        mProjection = other.mProjection;
        mModelView = other.mModelView;
        mTexture0 = other.mTexture0;
        track();
    }
    return *this;
}

// [Viewer Lab]
LLVertexBufferData::~LLVertexBufferData()
{
    untrack();
}

// [Viewer Lab] The GL backend's raw-name bind for a list's raw GL name, a binding without a texture of the device (no
// device, a caller's raw name)
static void bind_raw(LLTexUnit* unit, U32 name)
{
    gl_bind_raw_name(unit, LLTexUnit::TT_TEXTURE, name, false);
}

void LLVertexBufferData::drawWithMatrix()
{
    if (!mVB)
    {
        llassert(false);
        // Not supposed to happen, check buffer generation
        return;
    }

    llassert(mTexture.isNull() || mTexture->id() == mTextureId); // [Viewer Lab]
    if (mTexture.notNull())
    {
        gGL.getTexUnit(0)->bindTexture(LLTexUnit::TT_TEXTURE, mTexture.get()); // [Viewer Lab] by the device's texture
    }
    else if (mTexName)
    {
        bind_raw(gGL.getTexUnit(0), mTexName); // [Viewer Lab]
    }
    else
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(mModelView));
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(mProjection));
    gGL.matrixMode(LLRender::MM_TEXTURE0);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(mTexture0));

    mVB->setBuffer();
    mVB->drawArrays(mMode, 0, mCount);

    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
}

void LLVertexBufferData::draw()
{
    if (!mVB)
    {
        llassert(false);
        // Not supposed to happen, check buffer generation
        return;
    }

    llassert(mTexture.isNull() || mTexture->id() == mTextureId); // [Viewer Lab]
    if (mTexture.notNull())
    {
        gGL.getTexUnit(0)->bindTexture(LLTexUnit::TT_TEXTURE, mTexture.get()); // [Viewer Lab] by the device's texture
    }
    else if (mTexName)
    {
        bind_raw(gGL.getTexUnit(0), mTexName); // [Viewer Lab]
    }
    else
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    mVB->setBuffer();
    mVB->drawArrays(mMode, 0, mCount);
}

//============================================================================

//static
U64 LLVertexBuffer::getBytesAllocated()
{
    return sVBOPool ? sVBOPool->getVramBytesUsed() : 0;
}

//============================================================================
//
//static
U32 LLVertexBuffer::sGLRenderBuffer = 0;
U32 LLVertexBuffer::sGLRenderIndices = 0;
U32 LLVertexBuffer::sLastMask = 0;
U32 LLVertexBuffer::sVertexCount = 0;


//NOTE: each component must be AT LEAST 4 bytes in size to avoid a performance penalty on AMD hardware
const U32 LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_MAX] =
{
    sizeof(LLVector4), // TYPE_VERTEX,
    sizeof(LLVector4), // TYPE_NORMAL,
    sizeof(LLVector2), // TYPE_TEXCOORD0,
    sizeof(LLVector2), // TYPE_TEXCOORD1,
    sizeof(LLVector2), // TYPE_TEXCOORD2,
    sizeof(LLVector2), // TYPE_TEXCOORD3,
    sizeof(LLColor4U), // TYPE_COLOR,
    sizeof(LLColor4U), // TYPE_EMISSIVE, only alpha is used currently
    sizeof(LLVector4), // TYPE_TANGENT,
    sizeof(F32),       // TYPE_WEIGHT,
    sizeof(LLVector4), // TYPE_WEIGHT4,
    sizeof(LLVector4), // TYPE_CLOTHWEIGHT,
    sizeof(U64),       // TYPE_JOINT,
    sizeof(LLVector4), // TYPE_TEXTURE_INDEX (actually exists as position.w), no extra data, but stride is 16 bytes
};

static const std::string vb_type_name[] =
{
    "TYPE_VERTEX",
    "TYPE_NORMAL",
    "TYPE_TEXCOORD0",
    "TYPE_TEXCOORD1",
    "TYPE_TEXCOORD2",
    "TYPE_TEXCOORD3",
    "TYPE_COLOR",
    "TYPE_EMISSIVE",
    "TYPE_TANGENT",
    "TYPE_WEIGHT",
    "TYPE_WEIGHT4",
    "TYPE_CLOTHWEIGHT",
    "TYPE_JOINT"
    "TYPE_TEXTURE_INDEX",
    "TYPE_MAX",
    "TYPE_INDEX",
};

const U32 LLVertexBuffer::sGLMode[LLRender::NUM_MODES] =
{
    GL_TRIANGLES,
    GL_TRIANGLE_STRIP,
    GL_TRIANGLE_FAN,
    GL_POINTS,
    GL_LINES,
    GL_LINE_STRIP,
    GL_LINE_LOOP,
};

//static
void LLVertexBuffer::setupClientArrays(U32 data_mask)
{
    if (sLastMask != data_mask)
    {
        for (U32 i = 0; i < TYPE_MAX; ++i)
        {
            S32 loc = i;

            U32 mask = 1 << i;

            if (sLastMask & (1 << i))
            { //was enabled
                if (!(data_mask & mask))
                { //needs to be disabled
                    glDisableVertexAttribArray(loc);
                }
            }
            else
            {   //was disabled
                if (data_mask & mask)
                { //needs to be enabled
                    glEnableVertexAttribArray(loc);
                }
            }
        }
    }

    sLastMask = data_mask;
}

//static
void LLVertexBuffer::drawArrays(U32 mode, const std::vector<LLVector3>& pos)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    gGL.begin(mode);
    for (auto& v : pos)
    {
        gGL.vertex3fv(v.mV);
    }
    gGL.end();
    gGL.flush();
}

//static
void LLVertexBuffer::drawElements(U32 mode, const LLVector4a* pos, const LLVector2* tc, U32 num_indices, const U16* indicesp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(LLGLSLShader::sCurBoundShaderPtr != NULL);

    // <FS:Beq> FIRE-29679 trap empty calls that cause crashes when rezzing in OpenSim.
    if(pos == nullptr || indicesp == nullptr )
    {
        LL_WARNS() << "Called drawElements with null pos or null indices" << LL_ENDL;
        return;
    }
    // </FS:Beq>

    // <FS:Ansariel> Crash fix due to invalid calls to drawElements by Drake Arconis
    if (num_indices == 0)
    {
        LL_WARNS() << "Called drawElements with 0 indices" << LL_ENDL;
        return;
    }
    // </FS:Ansariel>

    STOP_GLERROR;

    gGL.syncMatrices();

    unbind();

    gGL.begin(mode);

    if (tc != nullptr)
    {
        for (U32 i = 0; i < num_indices; ++i)
        {
            U16 idx = indicesp[i];
            gGL.texCoord2fv(tc[idx].mV);
            gGL.vertex3fv(pos[idx].getF32ptr());
        }
    }
    else
    {
        for (U32 i = 0; i < num_indices; ++i)
        {
            U16 idx = indicesp[i];
            gGL.vertex3fv(pos[idx].getF32ptr());
        }
    }
    gGL.end();
    gGL.flush();
}

bool LLVertexBuffer::validateRange(U32 start, U32 end, U32 count, U32 indices_offset) const
{
    if (!gDebugGL)
    {
        return true;
    }

    llassert(start < mNumVerts);
    llassert(end < mNumVerts);

    if (start >= mNumVerts ||
        end >= mNumVerts)
    {
        LL_ERRS() << "Bad vertex buffer draw range: [" << start << ", " << end << "] vs " << mNumVerts << LL_ENDL;
    }

    if (indices_offset >= mNumIndices ||
        indices_offset + count > mNumIndices)
    {
        LL_ERRS() << "Bad index buffer draw range: [" << indices_offset << ", " << indices_offset+count << "]" << LL_ENDL;
    }

    {
#if 0  // not a reliable test for VBOs that are not backed by a CPU buffer
        U16* idx = (U16*) mMappedIndexData+indices_offset;
        for (U32 i = 0; i < count; ++i)
        {
            llassert(idx[i] >= start);
            llassert(idx[i] <= end);

            if (idx[i] < start || idx[i] > end)
            {
                LL_ERRS() << "Index out of range: " << idx[i] << " not in [" << start << ", " << end << "]" << LL_ENDL;
            }
        }

        LLVector4a* v = (LLVector4a*)mMappedData;

        for (U32 i = start; i <= end; ++i)
        {
            if (!v[i].isFinite3())
            {
                LL_ERRS() << "Non-finite vertex position data detected." << LL_ENDL;
            }
        }

        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;

        if (shader && shader->mFeatures.mIndexedTextureChannels > 1)
        {
            LLVector4a* v = (LLVector4a*) mMappedData;

            for (U32 i = start; i < end; i++)
            {
                U32 idx = (U32) (v[i][3]+0.25f);
                if (idx >= (U32)shader->mFeatures.mIndexedTextureChannels)
                {
                    LL_ERRS() << "Bad texture index found in vertex data stream." << LL_ENDL;
                }
            }
        }
#endif
    }

    return true;
}

#if LL_PROFILER_ENABLE_RENDER_DOC
void LLVertexBuffer::setLabel(const char* label) {
    if (mRHIBuffer.notNull()) // [Viewer Lab] the RHI buffer's name, which the GL backend labels
    {
        mRHIBuffer->setDebugName(label);
    }
    else
    {
        LL_LABEL_OBJECT_GL(GL_BUFFER, mGLBuffer, (GLsizei)strlen(label), label); // [Viewer Lab] (GLsizei): C4267 under /WX
    }
}
#endif

void LLVertexBuffer::clone(LLVertexBuffer& target) const
{
    target.mTypeMask = mTypeMask;
    target.mIndicesType = mIndicesType;
    target.mIndicesStride = mIndicesStride;
    if (target.getNumVerts() != getNumVerts() ||
        target.getNumIndices() != getNumIndices())
    {
        target.allocateBuffer(getNumVerts(), getNumIndices());
    }
}

// [Viewer Lab] With LL_METAL_DEBUG=1 on Metal: an indexed draw's indices checked against GL's [start, end] and the vertex
// count, from the CPU copy. GL is given the range and Metal is not, so an index outside it can draw differently on the
// two. The first 200 such draws are logged in full, then a count every 1000.
static bool metal_index_check()
{
    static const bool on = [] {
        const char* debug = getenv("LL_METAL_DEBUG");
        return debug && std::string(debug) == "1" && LL::RHI::viewerBackend() == LL::RHI::BackendType::Metal;
    }();
    return on;
}

static void check_index_range(const U8* indices, U32 type, U32 num_verts, U32 mode, U32 start, U32 end, U32 count,
                              U32 offset)
{
    static U32 sLogged = 0;
    static U64 sOutside = 0;
    if (!indices || count == 0)
    {
        return;
    }
    U32 low = U32_MAX, high = 0, outside_range = 0, beyond_verts = 0;
    for (U32 i = 0; i < count; ++i)
    {
        const U32 index = type == GL_UNSIGNED_INT ? reinterpret_cast<const U32*>(indices)[offset + i]
                                                  : reinterpret_cast<const U16*>(indices)[offset + i];
        low = llmin(low, index);
        high = llmax(high, index);
        outside_range += (index < start || index > end) ? 1 : 0;
        beyond_verts += index >= num_verts ? 1 : 0;
    }
    if (outside_range == 0 && beyond_verts == 0)
    {
        return;
    }
    ++sOutside;
    if (sLogged < 200)
    {
        ++sLogged;
        LL_WARNS("RHI") << "index range: mode " << mode << ", indices " << offset << "+" << count << ", range " << start
                        << "-" << end << ", vertices " << num_verts << ", indices span " << low << "-" << high << ", "
                        << outside_range << " outside the range, " << beyond_verts << " beyond the vertices" << LL_ENDL;
    }
    else if (sOutside % 1000 == 0)
    {
        LL_WARNS("RHI") << "index range: " << sOutside << " draws with indices outside their range so far" << LL_ENDL;
    }
}

void LLVertexBuffer::drawRange(U32 mode, U32 start, U32 end, U32 count, U32 indices_offset) const
{
    llassert(validateRange(start, end, count, indices_offset));
    llassert(gl_bound(mGLBuffer, mGLIndices)); // [Viewer Lab] GL's record, in the GL backend
    // [Viewer Lab] The census, the skip and the encoder on the main thread only, with one check
    const bool main_thread = on_main_thread();
    if (main_thread)
    {
        count_draw(mRHIBuffer.notNull() && mRHIIndices.notNull(),
                   indicesBeyond(indices_offset, count, mIndicesStride, mIndicesSize));
        if (!shader_can_draw())
        {
            ++sDrawCounts.skipped;
            return;
        }
    }
    gGL.syncMatrices();
    STOP_GLERROR;
    if (main_thread) // [Viewer Lab]
    {
        if (metal_index_check())
        {
            check_index_range(mMappedIndexData, mIndicesType, mNumVerts, mode, start, end, count, indices_offset);
        }
        encodeDraw(mode, true, indices_offset, count);
    }
    gl_draw_range_elements(mode, start, end, count, mIndicesType, indices_offset * (size_t) mIndicesStride); // [Viewer Lab]
    STOP_GLERROR;
}

void LLVertexBuffer::drawRangeFast(U32 mode, U32 start, U32 end, U32 count, U32 indices_offset) const
{
    // [Viewer Lab] The census, the skip and the encoder on the main thread only, with one check
    if (on_main_thread())
    {
        count_draw(mRHIBuffer.notNull() && mRHIIndices.notNull(),
                   indicesBeyond(indices_offset, count, mIndicesStride, mIndicesSize));
        if (!shader_can_draw())
        {
            ++sDrawCounts.skipped;
            return;
        }
        if (metal_index_check())
        {
            check_index_range(mMappedIndexData, mIndicesType, mNumVerts, mode, start, end, count, indices_offset);
        }
        encodeDraw(mode, true, indices_offset, count);
    }
    gl_draw_range_elements(mode, start, end, count, mIndicesType, indices_offset * (size_t)mIndicesStride); // [Viewer Lab]
}


void LLVertexBuffer::draw(U32 mode, U32 count, U32 indices_offset) const
{
    drawRange(mode, 0, mNumVerts-1, count, indices_offset);
}


bool LLVertexBuffer::drawArrays(U32 mode, U32 first, U32 count) const
{
    llassert(first + count <= mNumVerts);
    llassert(gl_bound(mGLBuffer, mGLIndices)); // [Viewer Lab] GL's record, in the GL backend
    // [Viewer Lab] The census, the skip and the encoder on the main thread only, with one check
    const bool main_thread = on_main_thread();
    if (main_thread)
    {
        count_draw(mRHIBuffer.notNull(), false);
        if (!shader_can_draw())
        {
            ++sDrawCounts.skipped;
            return false;
        }
    }

    gGL.syncMatrices();
    STOP_GLERROR;
    bool encoded = false; // [Viewer Lab]
    if (main_thread) // [Viewer Lab]
    {
        encoded = encodeDraw(mode, false, first, count);
    }
    const bool made = gl_draw_arrays(mode, first, count); // [Viewer Lab]
    STOP_GLERROR;
    return encoded || made; // [Viewer Lab]
}

// [Viewer Lab] The steps of encoding a draw, in order. Every main-thread draw that is not skipped ends in exactly one of
// encoded, withoutDevice, withoutProgram, withoutStorage, passless and withoutPipeline. The tracker's verdict never
// decides whether GL draws, and every binding the tracker holds is compared at every draw, since the GL encoder puts the
// interface's bindings back before its calls and at the frame's end.
bool LLVertexBuffer::encodeDraw(U32 mode, bool indexed, U32 first, U32 count) const
{
    // (1) The preconditions: the viewer's device, asked every time with no reference kept, a program of that device
    // for the bound shader, and the part's storage on it. The draws call it on the main thread only.
    llassert(on_main_thread());
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device)
    {
        ++sDrawCounts.withoutDevice;
        return false;
    }
    const LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    LL::RHI::Program* program = shader ? shader->getRHIProgram() : nullptr;
    if (!program || shader->getRHIDevice() != device->id())
    {
        ++sDrawCounts.withoutProgram;
        return false;
    }
    if (mRHIBuffer.isNull() || (indexed && mRHIIndices.isNull()))
    {
        ++sDrawCounts.withoutStorage;
        return false;
    }

    // (2) The pass: the open one, an interface pass too, or else the stack's, begun now; a level without one stays raw.
    EncodeRecord& record = sEncode;
    if (record.device != device->id())
    {
        record = EncodeRecord();
        record.device = device->id();
    }
    LL::RHI::Encoder& encoder = device->encoder();
    const LL::RHI::EncoderTracker& state = encoder.state();
    if (!state.inPass() && !LLRenderTarget::beginStackPass())
    {
        ++sDrawCounts.passless;
        return false;
    }

    // (3) GL's calls stay LLVertexBuffer's own, and the interface keeps its bindings.
    OwnGLDraws own;

    // (4) The pass and its formats, read once per change.
    if (state.passSerial() != record.passSerial)
    {
        record.passSerial = state.passSerial();
        ++sDrawCounts.passesWithDraws;
    }
    const U64 formats = state.passFormatsSerial();
    if (formats != record.formatsSerial)
    {
        record.formatsSerial = formats;
        record.formats = state.passFormats();
        record.formatsHash = formats_hash(record.formats);
        ++sDrawCounts.formatChanges;
    }

    // (5) The pipeline, looked up when the key changes, and compared with the tracker's at every draw.
    const DrawKey key = draw_key(program->id(), shader->mAttributeMask, mode, formats);
    if (!record.keyKnown || !(key == record.key))
    {
        record.keyKnown = true;
        record.key = key;
        ++sDrawCounts.tableLookups;
        record.wanted = find_pipeline(*device, *program, record, key, mode);
    }
    LL::RHI::Pipeline* pipeline = record.wanted;
    if (!pipeline)
    {
        ++sDrawCounts.withoutPipeline;
        return false;
    }
    const LL::RHI::Pipeline* bound = state.pipeline();
    if (!bound || bound->id() != pipeline->id())
    {
        encoder.setPipeline(pipeline);
        ++sDrawCounts.pipelineSets;
    }

    // (6) The depth state, from the device's cache through the table, by pointer.
    LL::RHI::DepthStencilDesc depth;
    if (!describeDepth(depth))
    {
        ++sDrawCounts.withoutPipeline;
        return false;
    }
    LL::RHI::DepthStencilState*& slot =
        record.depths[(depth.depthTest ? 16 : 0) + (depth.depthWrite ? 8 : 0) + (U32(depth.depthFunc) & 7)];
    if (!slot)
    {
        slot = device->getDepthStencilState(depth).get();
        ++sDrawCounts.depthRequests;
    }
    sDrawCounts.depthWritesInReadOnlyPass += writesInReadOnlyPass(state.pass().depth.readOnly, depth.depthWrite) ? 1 : 0;
    sDrawCounts.drawsInReadOnlyPass += state.pass().depth.readOnly ? 1 : 0;
    if (state.depthStencilState() != slot)
    {
        encoder.setDepthStencilState(slot);
        ++sDrawCounts.depthSets;
    }

    // (7) The buffers, by the encoder's bound objects' ids, with the streams at their offsets.
    LL::RHI::VertexStreams streams;
    vertexStreams(key.mask, mOffsets, streams);
    const LL::RHI::Buffer* vertices = state.vertexBuffer();
    const LL::RHI::VertexStreams& boundStreams = state.streams();
    if (!vertices || vertices->id() != mRHIBuffer->id() || boundStreams.attributeMask != streams.attributeMask
        || boundStreams.offset != streams.offset)
    {
        encoder.setVertexBuffer(mRHIBuffer.get(), streams);
        ++sDrawCounts.vertexBufferSets;
    }
    if (indexed)
    {
        const LL::RHI::IndexType type = *LL::RHI::indexTypeFromGL(mIndicesType);
        const LL::RHI::Buffer* indices = state.indexBuffer();
        if (!indices || indices->id() != mRHIIndices->id() || state.indexType() != type)
        {
            encoder.setIndexBuffer(mRHIIndices.get(), type);
            ++sDrawCounts.indexBufferSets;
        }
    }

    // (8) The draw, recorded in the pass; GL's draw follows in the caller. Made when the tracker's draws() went up across
    // it.
    const U64 made = state.draws();
    if (indexed)
    {
        encoder.drawIndexed(LL::RHI::PrimitiveType(mode), first, count);
    }
    else
    {
        encoder.draw(LL::RHI::PrimitiveType(mode), first, count);
    }
    ++sDrawCounts.encoded;
    return state.draws() > made;
}

// [Viewer Lab]
//static
LLVertexBuffer::DrawCounts LLVertexBuffer::drawCounts()
{
    return sDrawCounts;
}

// [Viewer Lab] The tally by shader name; shaders of the same name add up.
//static
const std::map<std::string, U64>& LLVertexBuffer::programTally()
{
    static std::map<std::string, U64> by_name;
    by_name.clear();
    for (const auto& [shader, entry] : sProgramTally)
    {
        by_name[entry.name] += entry.draws;
    }
    return by_name;
}

// [Viewer Lab]
//static
void LLVertexBuffer::clearProgramTally()
{
    sProgramTally.clear();
}

// [Viewer Lab]
LLVertexBuffer::OwnGLDraws::OwnGLDraws()
:   mPrevious(sMakesOwnGLDraws)
{
    sMakesOwnGLDraws = true;
}

// [Viewer Lab]
LLVertexBuffer::OwnGLDraws::~OwnGLDraws()
{
    sMakesOwnGLDraws = mPrevious;
}

// [Viewer Lab]
//static
bool LLVertexBuffer::makesOwnGLDraws()
{
    return sMakesOwnGLDraws;
}

// [Viewer Lab] setupVertexAttributes' rules
//static
void LLVertexBuffer::vertexStreams(U32 data_mask, const U32* offsets, LL::RHI::VertexStreams& streams)
{
    streams.attributeMask = data_mask;
    for (U32 i = 0; i < TYPE_MAX; ++i)
    {
        streams.offset[i] = offsets[i];
    }
    if (data_mask & MAP_EMISSIVE)
    {
        streams.offset[TYPE_COLOR] = offsets[TYPE_EMISSIVE]; // with or without MAP_COLOR
    }
    streams.offset[TYPE_TEXTURE_INDEX] = offsets[TYPE_VERTEX] + 12;
}

// [Viewer Lab] The factors are eBlendFactor's order (llrhigl.cpp's static_assert)
//static
void LLVertexBuffer::describePipeline(U32 mode, U32 data_mask, const LL::RHI::PassFormats& pass, LL::RHI::PipelineDesc& pipeline)
{
    LL::RHI::PipelineDesc desc;
    desc.attributeMask = data_mask;
    desc.primitiveClass = LL::RHI::primitiveClass(LL::RHI::PrimitiveType(mode));
    LL::RHI::applyPassFormats(pass, desc);
    desc.blend.enabled = GLStateRecord::isEnabled(GL_BLEND);
    LLRender::eBlendFactor factors[4];
    gGL.getBlendFunc(factors[0], factors[1], factors[2], factors[3]);
    auto factor = [](LLRender::eBlendFactor value, LL::RHI::BlendFactor initial)
    {
        return value < LLRender::BF_UNDEF ? LL::RHI::BlendFactor(value) : initial;
    };
    desc.blend.srcColor = factor(factors[0], LL::RHI::BlendFactor::One);
    desc.blend.dstColor = factor(factors[1], LL::RHI::BlendFactor::Zero);
    desc.blend.srcAlpha = factor(factors[2], LL::RHI::BlendFactor::One);
    desc.blend.dstAlpha = factor(factors[3], LL::RHI::BlendFactor::Zero);
    desc.blend.colorOp = LL::RHI::BlendOp::Add;
    desc.blend.alphaOp = LL::RHI::BlendOp::Add;
    bool r, g, b, a;
    gGL.getColorMask(r, g, b, a);
    desc.colorMask = U8((r ? LL::RHI::COLOR_MASK_R : 0) | (g ? LL::RHI::COLOR_MASK_G : 0) | (b ? LL::RHI::COLOR_MASK_B : 0)
                        | (a ? LL::RHI::COLOR_MASK_A : 0));
    pipeline = LL::RHI::normalized(desc);
}

// [Viewer Lab]
//static
bool LLVertexBuffer::describeDepth(LL::RHI::DepthStencilDesc& depth)
{
    GLboolean enabled = GL_FALSE;
    GLboolean write = GL_FALSE;
    GLenum func = GL_LESS;
    LLGLDepthTest::getState(enabled, write, func);
    const std::optional<LL::RHI::CompareFunc> compare = LL::RHI::compareFuncFromGL(func);
    if (!compare)
    {
        return false;
    }
    depth.depthTest = enabled != GL_FALSE;
    depth.depthWrite = depth.depthTest && write != GL_FALSE;
    depth.depthFunc = *compare;
    return true;
}

//static
void LLVertexBuffer::initClass(LLWindow*) // [Viewer Lab] the window is not used
{
    llassert(sVBOPool == nullptr);
    sVBOPool = create_vbo_pool(); // [Viewer Lab]
}

// [Viewer Lab]
//static
U32 LLVertexBuffer::genGLBuffer()
{
    return gen_buffer();
}

// [Viewer Lab]
//static
void LLVertexBuffer::deleteGLBuffers(S32 count, U32* buffers)
{
    delete_buffers(count, buffers);
}

// [Viewer Lab]
//static
bool LLVertexBuffer::allocateGLStorage(U32 target, U32 size, U32& name)
{
    return gl_storage_allocate(target, size, name);
}

// [Viewer Lab]
//static
void LLVertexBuffer::updateGLStorage(U32 target, U32 size, U32& name, U32 offset, const U8* data, U32 bytes)
{
    gl_storage_update(target, size, name, offset, data, bytes);
}

// [Viewer Lab]
//static
void LLVertexBuffer::freeGLStorage(U32 target, U32 size, U32 name)
{
    gl_storage_free(target, size, name);
}

//static
void LLVertexBuffer::unbind()
{
    STOP_GLERROR;
    gl_unbind_buffers(); // [Viewer Lab] the binds and the record, in the GL backend
    STOP_GLERROR;
}

//static
void LLVertexBuffer::cleanupClass()
{
    unbind();

    delete sVBOPool;
    sVBOPool = nullptr;

}

//----------------------------------------------------------------------------

LLVertexBuffer::LLVertexBuffer(U32 typemask)
:   LLRefCount(),
    mTypeMask(typemask)
{
    //zero out offsets
    for (U32 i = 0; i < TYPE_MAX; i++)
    {
        mOffsets[i] = 0;
    }
}

// list of mapped buffers
// NOTE: must not be LLPointer<LLVertexBuffer> to avoid breaking non-ref-counted LLVertexBuffer instances
static std::vector<LLVertexBuffer*> sMappedBuffers;

//static
void LLVertexBuffer::flushBuffers()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    // must only be called from main thread
    for (auto& buffer : sMappedBuffers)
    {
        buffer->_unmapBuffer();
        buffer->mMapped = false;
    }

    sMappedBuffers.resize(0);
}

//static
U32 LLVertexBuffer::calcOffsets(const U32& typemask, U32* offsets, U32 num_vertices)
{
    U32 offset = 0;
    for (U32 i=0; i<TYPE_TEXTURE_INDEX; i++)
    {
        U32 mask = 1<<i;
        if (typemask & mask)
        {
            if (offsets && LLVertexBuffer::sTypeSize[i])
            {
                offsets[i] = offset;
                offset += LLVertexBuffer::sTypeSize[i]*num_vertices;
                offset = (offset + 0xF) & ~0xF;
            }
        }
    }

    offsets[TYPE_TEXTURE_INDEX] = offsets[TYPE_VERTEX] + 12;

    return offset;
}

//static
U32 LLVertexBuffer::calcVertexSize(const U32& typemask)
{
    U32 size = 0;
    for (U32 i = 0; i < TYPE_TEXTURE_INDEX; i++)
    {
        U32 mask = 1<<i;
        if (typemask & mask)
        {
            size += LLVertexBuffer::sTypeSize[i];
        }
    }

    return size;
}

// protected, use unref()
//virtual
LLVertexBuffer::~LLVertexBuffer()
{
    if (mMapped)
    { // is on the mapped buffer list but doesn't need to be flushed
        mMapped = false;
        unmapBuffer();
    }

    destroyGLBuffer();
    destroyGLIndices();

    if (mMappedData)
    {
        LL_ERRS() << "Failed to clear vertex buffer's vertices" << LL_ENDL;
    }
    if (mMappedIndexData)
    {
        LL_ERRS() << "Failed to clear vertex buffer's indices" << LL_ENDL;
    }
};

//----------------------------------------------------------------------------

void LLVertexBuffer::genBuffer(U32 size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(sVBOPool);

    if (sVBOPool)
    {
        llassert(mSize == 0);
        llassert(mGLBuffer == 0);
        llassert(mMappedData == nullptr);

        llassert(mRHIBuffer.isNull()); // [Viewer Lab]

        mSize = size;
        // [Viewer Lab] the CPU copy before the name, as the pool's lists expect, and the storage on
        // the viewer's device when there is one
        mMappedData = allocate_cpu_copy(GL_ARRAY_BUFFER, mSize);
        mRHIBuffer = create_rhi_storage(LL::RHI::BUFFER_VERTEX, mSize);
        if (mRHIBuffer)
        {
            mGLBuffer = gl_name(mRHIBuffer);
        }
        else
        {
            gl_storage_allocate(GL_ARRAY_BUFFER, mSize, mGLBuffer);
        }
    }
}

void LLVertexBuffer::genIndices(U32 size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(sVBOPool);

    if (sVBOPool)
    {
        llassert(mIndicesSize == 0);
        llassert(mGLIndices == 0);
        llassert(mMappedIndexData == nullptr);
        // [Viewer Lab] Only createGLIndices' test of mGLIndices alone gets here with storage, on
        // Apple's pool before the first unmap, where the pool keeps counting the old bytes and the
        // old copy is lost
        if (mRHIIndices)
        {
            keep_unreleased(mRHIIndices);
        }
        mIndicesSize = size;
        // [Viewer Lab] the CPU copy before the name, as the pool's lists expect, and the storage on
        // the viewer's device when there is one
        mMappedIndexData = allocate_cpu_copy(GL_ELEMENT_ARRAY_BUFFER, mIndicesSize);
        mRHIIndices = create_rhi_storage(LL::RHI::BUFFER_INDEX, mIndicesSize);
        if (mRHIIndices)
        {
            mGLIndices = gl_name(mRHIIndices);
        }
        else
        {
            gl_storage_allocate(GL_ELEMENT_ARRAY_BUFFER, mIndicesSize, mGLIndices);
        }
    }
}

bool LLVertexBuffer::createGLBuffer(U32 size)
{
    if (mGLBuffer || mMappedData)
    {
        destroyGLBuffer();
    }

    if (size == 0)
    {
        return true;
    }

    bool success = true;

    genBuffer(size);

    if (!mMappedData)
    {
        success = false;
    }
    return success;
}

bool LLVertexBuffer::createGLIndices(U32 size)
{
    // [Viewer Lab] Storage without a GL name has the CPU copy to show for it, so it goes before the
    // new size; GL's test is of the name alone
    if (mGLIndices || (mMappedIndexData && sVBOPool && sVBOPool->storageHasNoGLName()))
    {
        destroyGLIndices();
    }

    if (size == 0)
    {
        return true;
    }

    bool success = true;

    genIndices(size);

    if (!mMappedIndexData)
    {
        success = false;
    }
    return success;
}

void LLVertexBuffer::destroyGLBuffer()
{
    if (mGLBuffer || mMappedData)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        //llassert(sVBOPool);
        if (sVBOPool)
        {
            // [Viewer Lab] the storage on the device gives its name back to the pool as it goes,
            // before the copy, as the pool's own storage does
            if (mRHIBuffer)
            {
                release_encoder_binding(mRHIBuffer.get(), false); // [Viewer Lab]
                mRHIBuffer = nullptr;
            }
            else
            {
                gl_storage_free(GL_ARRAY_BUFFER, mSize, mGLBuffer);
            }
            if (mMappedData)
            {
                free_cpu_copy(GL_ARRAY_BUFFER, mSize, mMappedData); // [Viewer Lab]
            }
        }
        else if (mRHIBuffer) // [Viewer Lab] after cleanupClass nothing is freed
        {
            keep_unreleased(mRHIBuffer);
        }

        mSize = 0;
        mGLBuffer = 0;
        mMappedData = nullptr;
    }
}

void LLVertexBuffer::destroyGLIndices()
{
    if (mGLIndices || mMappedIndexData)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        //llassert(sVBOPool);
        if (sVBOPool)
        {
            // [Viewer Lab] the storage on the device gives its name back to the pool as it goes,
            // before the copy, as the pool's own storage does
            if (mRHIIndices)
            {
                release_encoder_binding(mRHIIndices.get(), true); // [Viewer Lab]
                mRHIIndices = nullptr;
            }
            else
            {
                gl_storage_free(GL_ELEMENT_ARRAY_BUFFER, mIndicesSize, mGLIndices);
            }
            if (mMappedIndexData)
            {
                free_cpu_copy(GL_ELEMENT_ARRAY_BUFFER, mIndicesSize, mMappedIndexData); // [Viewer Lab]
            }
        }
        else if (mRHIIndices) // [Viewer Lab] after cleanupClass nothing is freed
        {
            keep_unreleased(mRHIIndices);
        }

        mIndicesSize = 0;
        mGLIndices = 0;
        mMappedIndexData = nullptr;
    }
}

bool LLVertexBuffer::updateNumVerts(U32 nverts)
{
    llassert(nverts >= 0);

    bool success = true;

    U32 needed_size = calcOffsets(mTypeMask, mOffsets, nverts);

    if (needed_size != mSize)
    {
        success &= createGLBuffer(needed_size);
    }

    llassert(mSize == needed_size);
    mNumVerts = nverts;
    return success;
}

bool LLVertexBuffer::updateNumIndices(U32 nindices)
{
    llassert(nindices >= 0);

    bool success = true;

    U32 needed_size = sizeof(U16) * nindices;

    if (needed_size != mIndicesSize)
    {
        success &= createGLIndices(needed_size);
    }

    llassert(mIndicesSize == needed_size);
    mNumIndices = nindices;
    return success;
}

bool LLVertexBuffer::allocateBuffer(U32 nverts, U32 nindices)
{
    if (nverts < 0 || nindices < 0)
    {
        LL_ERRS() << "Bad vertex buffer allocation: " << nverts << " : " << nindices << LL_ENDL;
    }

    bool success = true;

    success &= updateNumVerts(nverts);
    success &= updateNumIndices(nindices);

    return success;
}

//----------------------------------------------------------------------------

// if no gap between region and given range exists, expand region to cover given range and return true
// otherwise return false
bool expand_region(LLVertexBuffer::MappedRegion& region, U32 start, U32 end)
{

    if (end < region.mStart ||
        start > region.mEnd)
    { //gap exists, do not merge
        return false;
    }

    region.mStart = llmin(region.mStart, start);
    region.mEnd = llmax(region.mEnd, end);

    return true;
}


// Map for data access
U8* LLVertexBuffer::mapVertexBuffer(LLVertexBuffer::AttributeType type, U32 index, S32 count)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    _mapBuffer();

    if (count == -1)
    {
        count = mNumVerts - index;
    }

    // [Viewer Lab] Region tracking is not needed when the backend replaces the whole buffer at
    // every update, as Apple silicon's GL does
    if (!whole_buffer_updates()) // [Viewer Lab]
    {
        U32 start = mOffsets[type] + sTypeSize[type] * index;
        U32 end = start + sTypeSize[type] * count-1;

        bool flagged = false;
        // flag region as mapped
        for (U32 i = 0; i < mMappedVertexRegions.size(); ++i)
        {
            MappedRegion& region = mMappedVertexRegions[i];
            if (expand_region(region, start, end))
            {
                flagged = true;
                break;
            }
        }

        if (!flagged)
        {
            //didn't expand an existing region, make a new one
            mMappedVertexRegions.push_back({ start, end });
        }
    }
    return mMappedData+mOffsets[type]+sTypeSize[type]*index;
}


U8* LLVertexBuffer::mapIndexBuffer(U32 index, S32 count)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    _mapBuffer();

    if (count == -1)
    {
        count = mNumIndices-index;
    }

    if (!whole_buffer_updates()) // [Viewer Lab]
    {
        U32 start = sizeof(U16) * index;
        U32 end = start + sizeof(U16) * count-1;

        bool flagged = false;
        // flag region as mapped
        for (U32 i = 0; i < mMappedIndexRegions.size(); ++i)
        {
            MappedRegion& region = mMappedIndexRegions[i];
            if (expand_region(region, start, end))
            {
                flagged = true;
                break;
            }
        }

        if (!flagged)
        {
            //didn't expand an existing region, make a new one
            mMappedIndexRegions.push_back({ start, end });
        }
    }

    return mMappedIndexData + sizeof(U16)*index;
}

// flush the given byte range
//  target -- "target" parameter for glBufferSubData
//  start -- first byte to copy
//  end -- last byte to copy (NOT last byte + 1)
//  data -- data to be flushed
//  dst -- mMappedData or mMappedIndexData
void LLVertexBuffer::flush_vbo(GLenum target, U32 start, U32 end, void* data, U8* dst)
{
    if (whole_buffer_updates()) // [Viewer Lab]
    {
        // on OS X, flush_vbo doesn't actually write to the GL buffer, so be sure to call
        // _mapBuffer to tag the buffer for flushing to GL
        _mapBuffer();
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb memcpy");
        STOP_GLERROR;
        // copy into mapped buffer
        memcpy(dst+start, data, end-start+1);
    }
    else
    {
        llassert(target == GL_ARRAY_BUFFER ? sGLRenderBuffer == mGLBuffer : sGLRenderIndices == mGLIndices);

        // skip mapped data and stream to GPU via glBufferSubData
        if (end != 0)
        {
            updateStorage(target, start, (const U8*) data, end - start + 1); // [Viewer Lab] the pool's glBufferSubData blocks
        }
    }
}

void LLVertexBuffer::unmapBuffer()
{
    flushBuffers();
}

void LLVertexBuffer::_mapBuffer()
{
    if (!mMapped)
    {
        mMapped = true;
        sMappedBuffers.push_back(this);
    }
}

void LLVertexBuffer::_unmapBuffer()
{
    STOP_GLERROR;
    if (!mMapped)
    {
        return;
    }

    struct SortMappedRegion
    {
        bool operator()(const MappedRegion& lhs, const MappedRegion& rhs)
        {
            return lhs.mStart < rhs.mStart;
        }
    };

    if (whole_buffer_updates()) // [Viewer Lab] the pool gives each part a new name with the whole copy
    {
        STOP_GLERROR;
        if (mMappedData)
        {
            updateStorage(GL_ARRAY_BUFFER, 0, mMappedData, mSize); // [Viewer Lab]
        }
        else
        {
            rebind_gl_buffer(GL_ARRAY_BUFFER, mGLBuffer); // [Viewer Lab]
        }
        STOP_GLERROR;

        if (mMappedIndexData)
        {
            updateStorage(GL_ELEMENT_ARRAY_BUFFER, 0, mMappedIndexData, mIndicesSize); // [Viewer Lab]
        }
        else
        {
            rebind_gl_buffer(GL_ELEMENT_ARRAY_BUFFER, mGLIndices); // [Viewer Lab]
        }
        STOP_GLERROR;
    }
    else
    {
        if (!mMappedVertexRegions.empty())
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("unmapBuffer - vertex");

            rebind_gl_buffer(GL_ARRAY_BUFFER, mGLBuffer); // [Viewer Lab]

            U32 start = 0;
            U32 end = 0;

            std::sort(mMappedVertexRegions.begin(), mMappedVertexRegions.end(), SortMappedRegion());

            for (U32 i = 0; i < mMappedVertexRegions.size(); ++i)
            {
                const MappedRegion& region = mMappedVertexRegions[i];
                if (region.mStart == end + 1)
                {
                    end = region.mEnd;
                }
                else
                {
                    flush_vbo(GL_ARRAY_BUFFER, start, end, (U8*)mMappedData + start, mMappedData);
                    start = region.mStart;
                    end = region.mEnd;
                }
            }

            flush_vbo(GL_ARRAY_BUFFER, start, end, (U8*)mMappedData + start, mMappedData);
            mMappedVertexRegions.clear();
        }

        if (!mMappedIndexRegions.empty())
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("unmapBuffer - index");

            rebind_gl_buffer(GL_ELEMENT_ARRAY_BUFFER, mGLIndices); // [Viewer Lab]
            U32 start = 0;
            U32 end = 0;

            std::sort(mMappedIndexRegions.begin(), mMappedIndexRegions.end(), SortMappedRegion());

            for (U32 i = 0; i < mMappedIndexRegions.size(); ++i)
            {
                const MappedRegion& region = mMappedIndexRegions[i];
                if (region.mStart == end + 1)
                {
                    end = region.mEnd;
                }
                else
                {
                    flush_vbo(GL_ELEMENT_ARRAY_BUFFER, start, end, (U8*)mMappedIndexData + start, mMappedIndexData);
                    start = region.mStart;
                    end = region.mEnd;
                }
            }

            flush_vbo(GL_ELEMENT_ARRAY_BUFFER, start, end, (U8*)mMappedIndexData + start, mMappedIndexData);
            mMappedIndexRegions.clear();
        }
    }
}

// [Viewer Lab] Through the viewer's render device when the part has storage there, otherwise the GL
// backend's pool
void LLVertexBuffer::updateStorage(U32 target, U32 offset, const U8* data, U32 bytes)
{
    const bool indices = target == GL_ELEMENT_ARRAY_BUFFER;
    LLPointer<LL::RHI::Buffer>& storage = indices ? mRHIIndices : mRHIBuffer;
    U32& name = indices ? mGLIndices : mGLBuffer;
    if (storage.isNull())
    {
        gl_storage_update(target, indices ? mIndicesSize : mSize, name, offset, data, bytes);
        return;
    }
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || !on_main_thread())
    {
        LL_WARNS_ONCE("RHI") << "LLVertexBuffer's storage on the render device is updated only on the main thread "
                             << "while the device exists; this update is skipped" << LL_ENDL;
        return;
    }
    if (bytes == 0)
    {
        return;
    }
    device->encoder().updateBuffer(storage.get(), offset,
                                   std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), bytes));
    name = gl_name(storage);
}

//----------------------------------------------------------------------------

template <class T,LLVertexBuffer::AttributeType type> struct VertexBufferStrider
{
    typedef LLStrider<T> strider_t;
    static bool get(LLVertexBuffer& vbo,
                    strider_t& strider,
                    S32 index, S32 count)
    {
        if (type == LLVertexBuffer::TYPE_INDEX)
        {
            U8* ptr = vbo.mapIndexBuffer(index, count);

            if (ptr == NULL)
            {
                LL_WARNS() << "mapIndexBuffer failed!" << LL_ENDL;
                return false;
            }

            strider = (T*)ptr;
            strider.setStride(0);
            return true;
        }
        else if (vbo.hasDataType(type))
        {
            U32 stride = LLVertexBuffer::sTypeSize[type];

            U8* ptr = vbo.mapVertexBuffer(type, index, count);

            if (ptr == NULL)
            {
                LL_WARNS() << "mapVertexBuffer failed!" << LL_ENDL;
                return false;
            }

            strider = (T*)ptr;
            strider.setStride(stride);
            return true;
        }
        else
        {
            LL_ERRS() << "VertexBufferStrider could not find valid vertex data." << LL_ENDL;
        }
        return false;
    }
};

bool LLVertexBuffer::getVertexStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_VERTEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getVertexStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_VERTEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getIndexStrider(LLStrider<U16>& strider, U32 index, S32 count)
{
    llassert(mIndicesStride == 2); // cannot access 32-bit indices with U16 strider
    llassert(mIndicesType == GL_UNSIGNED_SHORT);
    return VertexBufferStrider<U16,TYPE_INDEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord0Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD0>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord1Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD1>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord2Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD2>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getNormalStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_NORMAL>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getNormalStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a, TYPE_NORMAL>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTangentStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_TANGENT>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTangentStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_TANGENT>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getColorStrider(LLStrider<LLColor4U>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLColor4U,TYPE_COLOR>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getEmissiveStrider(LLStrider<LLColor4U>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLColor4U,TYPE_EMISSIVE>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getWeightStrider(LLStrider<F32>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<F32,TYPE_WEIGHT>::get(*this, strider, index, count);
}

// <FS:Ansariel> Vectorized Weight4Strider and ClothWeightStrider by Drake Arconis
//bool LLVertexBuffer::getWeight4Strider(LLStrider<LLVector4>& strider, U32 index, S32 count)
//{
//  return VertexBufferStrider<LLVector4,TYPE_WEIGHT4>::get(*this, strider, index, count);
//}
//
//bool LLVertexBuffer::getClothWeightStrider(LLStrider<LLVector4>& strider, U32 index, S32 count)
//{
//  return VertexBufferStrider<LLVector4,TYPE_CLOTHWEIGHT>::get(*this, strider, index, count);
//}
bool LLVertexBuffer::getWeight4Strider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_WEIGHT4>::get(*this, strider, index, count);
}

bool LLVertexBuffer::getClothWeightStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_CLOTHWEIGHT>::get(*this, strider, index, count);
}
// </FS:Ansariel>

//----------------------------------------------------------------------------


// Set for rendering
void LLVertexBuffer::setBuffer()
{
    STOP_GLERROR;

    if (mMapped)
    {
        LL_WARNS_ONCE() << "Missing call to unmapBuffer or flushBuffers" << LL_ENDL;
        _unmapBuffer();
    }

    // no data may be pending
    llassert(mMappedVertexRegions.empty());
    llassert(mMappedIndexRegions.empty());

    // a shader must be bound
    llassert(LLGLSLShader::sCurBoundShaderPtr);
    if (!LLGLSLShader::sCurBoundShaderPtr) // [Viewer Lab] no mask to read; without the check release builds would dereference null
    {
        LL_WARNS_ONCE("Shader") << "setBuffer with no shader bound: nothing is bound" << LL_ENDL;
        return;
    }

    U32 data_mask = LLGLSLShader::sCurBoundShaderPtr->mAttributeMask;

    // this Vertex Buffer must provide all necessary attributes for currently bound shader
    llassert_msg((data_mask & mTypeMask) == data_mask,
        "Attribute mask mismatch! mTypeMask should be a superset of data_mask.  data_mask: 0x"
                << std::hex << data_mask << " mTypeMask: 0x" << mTypeMask << " Missing: 0x" << (data_mask & ~mTypeMask) <<  std::dec);

    // [Viewer Lab] The census: the call, a part without one of the shader's attributes, and a part where the stream of one
    // starts at or past its vertices
    ++sDrawCounts.setBuffers;
    sDrawCounts.missingAttributes += lacksAttributes(data_mask, mTypeMask) ? 1 : 0;
    sDrawCounts.streamsBeyondBuffer += streamsBeyond(data_mask, mOffsets, mSize) ? 1 : 0;

    gl_set_buffer(mGLBuffer, mGLIndices, data_mask, mOffsets); // [Viewer Lab] the binds and the setup, in the GL backend

    STOP_GLERROR;
}

// [Viewer Lab] setBuffer's attribute setup for the bound program's mask (gl_set_buffer), with the
// offsets of the attributes' streams given, so the render interface's GL backend sets attributes
// up the same way (llrhigl.cpp).
//static
void LLVertexBuffer::setupVertexAttributes(U32 data_mask, const U32* offsets)
{
    STOP_GLERROR;
    U8* base = nullptr;

    if (data_mask & MAP_NORMAL)
    {
        AttributeType loc = TYPE_NORMAL;
        void* ptr = (void*)(base + offsets[TYPE_NORMAL]);
        glVertexAttribPointer(loc, 3, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[TYPE_NORMAL], ptr);
    }
    if (data_mask & MAP_TEXCOORD3)
    {
        AttributeType loc = TYPE_TEXCOORD3;
        void* ptr = (void*)(base + offsets[TYPE_TEXCOORD3]);
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[TYPE_TEXCOORD3], ptr);
    }
    if (data_mask & MAP_TEXCOORD2)
    {
        AttributeType loc = TYPE_TEXCOORD2;
        void* ptr = (void*)(base + offsets[TYPE_TEXCOORD2]);
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[TYPE_TEXCOORD2], ptr);
    }
    if (data_mask & MAP_TEXCOORD1)
    {
        AttributeType loc = TYPE_TEXCOORD1;
        void* ptr = (void*)(base + offsets[TYPE_TEXCOORD1]);
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[TYPE_TEXCOORD1], ptr);
    }
    if (data_mask & MAP_TANGENT)
    {
        AttributeType loc = TYPE_TANGENT;
        void* ptr = (void*)(base + offsets[TYPE_TANGENT]);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[TYPE_TANGENT], ptr);
    }
    if (data_mask & MAP_TEXCOORD0)
    {
        AttributeType loc = TYPE_TEXCOORD0;
        void* ptr = (void*)(base + offsets[TYPE_TEXCOORD0]);
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[TYPE_TEXCOORD0], ptr);
    }
    if (data_mask & MAP_COLOR)
    {
        AttributeType loc = TYPE_COLOR;
        //bind emissive instead of color pointer if emissive is present
        void* ptr = (data_mask & MAP_EMISSIVE) ? (void*)(base + offsets[TYPE_EMISSIVE]) : (void*)(base + offsets[TYPE_COLOR]);
        glVertexAttribPointer(loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, LLVertexBuffer::sTypeSize[TYPE_COLOR], ptr);
    }
    if (data_mask & MAP_EMISSIVE)
    {
        AttributeType loc = TYPE_EMISSIVE;
        void* ptr = (void*)(base + offsets[TYPE_EMISSIVE]);
        glVertexAttribPointer(loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, LLVertexBuffer::sTypeSize[TYPE_EMISSIVE], ptr);

        if (!(data_mask & MAP_COLOR))
        { //map emissive to color channel when color is not also being bound to avoid unnecessary shader swaps
            loc = TYPE_COLOR;
            glVertexAttribPointer(loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, LLVertexBuffer::sTypeSize[TYPE_EMISSIVE], ptr);
        }
    }
    if (data_mask & MAP_WEIGHT)
    {
        AttributeType loc = TYPE_WEIGHT;
        void* ptr = (void*)(base + offsets[TYPE_WEIGHT]);
        glVertexAttribPointer(loc, 1, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[TYPE_WEIGHT], ptr);
    }
    if (data_mask & MAP_WEIGHT4)
    {
        AttributeType loc = TYPE_WEIGHT4;
        void* ptr = (void*)(base + offsets[TYPE_WEIGHT4]);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[TYPE_WEIGHT4], ptr);
    }
    if (data_mask & MAP_JOINT)
    {
        AttributeType loc = TYPE_JOINT;
        void* ptr = (void*)(base + offsets[TYPE_JOINT]);
        glVertexAttribIPointer(loc, 4, GL_UNSIGNED_SHORT, LLVertexBuffer::sTypeSize[TYPE_JOINT], ptr);
    }
    if (data_mask & MAP_CLOTHWEIGHT)
    {
        AttributeType loc = TYPE_CLOTHWEIGHT;
        void* ptr = (void*)(base + offsets[TYPE_CLOTHWEIGHT]);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_TRUE, LLVertexBuffer::sTypeSize[TYPE_CLOTHWEIGHT], ptr);
    }
    if (data_mask & MAP_TEXTURE_INDEX)
    {
        AttributeType loc = TYPE_TEXTURE_INDEX;
        void* ptr = (void*)(base + offsets[TYPE_VERTEX] + 12);
        glVertexAttribIPointer(loc, 1, GL_UNSIGNED_INT, LLVertexBuffer::sTypeSize[TYPE_VERTEX], ptr);
    }
    if (data_mask & MAP_VERTEX)
    {
        AttributeType loc = TYPE_VERTEX;
        void* ptr = (void*)(base + offsets[TYPE_VERTEX]);
        glVertexAttribPointer(loc, 3, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[TYPE_VERTEX], ptr);
    }
    STOP_GLERROR;
}

void LLVertexBuffer::setPositionData(const LLVector4a* data)
{
    flush_vbo(GL_ARRAY_BUFFER, 0, sizeof(LLVector4a) * getNumVerts()-1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setTexCoord0Data(const LLVector2* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TEXCOORD0], mOffsets[TYPE_TEXCOORD0] + sTypeSize[TYPE_TEXCOORD0] * getNumVerts() - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord1Data(const LLVector2* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TEXCOORD1], mOffsets[TYPE_TEXCOORD1] + sTypeSize[TYPE_TEXCOORD1] * getNumVerts() - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setColorData(const LLColor4U* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_COLOR], mOffsets[TYPE_COLOR] + sTypeSize[TYPE_COLOR] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setNormalData(const LLVector4a* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_NORMAL], mOffsets[TYPE_NORMAL] + sTypeSize[TYPE_NORMAL] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setTangentData(const LLVector4a* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TANGENT], mOffsets[TYPE_TANGENT] + sTypeSize[TYPE_TANGENT] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setWeight4Data(const LLVector4a* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_WEIGHT4], mOffsets[TYPE_WEIGHT4] + sTypeSize[TYPE_WEIGHT4] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setJointData(const U64* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_JOINT], mOffsets[TYPE_JOINT] + sTypeSize[TYPE_JOINT] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setIndexData(const U16* data)
{
    flush_vbo(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(U16) * getNumIndices() - 1, (U8*) data, mMappedIndexData);
}

void LLVertexBuffer::setIndexData(const U32* data)
{
    if (mIndicesType != GL_UNSIGNED_INT)
    { // HACK -- vertex buffers are initialized as 16-bit indices, but can be switched to 32-bit indices
        mIndicesType = GL_UNSIGNED_INT;
        mIndicesStride = 4;
        mNumIndices /= 2;
    }
    flush_vbo(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(U32) * getNumIndices() - 1, (U8*)data, mMappedIndexData);
}

void LLVertexBuffer::setPositionData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, offset * sizeof(LLVector4a), (offset + count) * sizeof(LLVector4a) - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setNormalData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_NORMAL] + offset * sTypeSize[TYPE_NORMAL], mOffsets[TYPE_NORMAL] + (offset + count) * sTypeSize[TYPE_NORMAL] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord0Data(const LLVector2* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TEXCOORD0] + offset * sTypeSize[TYPE_TEXCOORD0], mOffsets[TYPE_TEXCOORD0] + (offset + count) * sTypeSize[TYPE_TEXCOORD0] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord1Data(const LLVector2* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TEXCOORD1] + offset * sTypeSize[TYPE_TEXCOORD1], mOffsets[TYPE_TEXCOORD1] + (offset + count) * sTypeSize[TYPE_TEXCOORD1] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setColorData(const LLColor4U* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_COLOR] + offset * sTypeSize[TYPE_COLOR], mOffsets[TYPE_COLOR] + (offset + count) * sTypeSize[TYPE_COLOR] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTangentData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TANGENT] + offset * sTypeSize[TYPE_TANGENT], mOffsets[TYPE_TANGENT] + (offset + count) * sTypeSize[TYPE_TANGENT] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setWeight4Data(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_WEIGHT4] + offset * sTypeSize[TYPE_WEIGHT4], mOffsets[TYPE_WEIGHT4] + (offset + count) * sTypeSize[TYPE_WEIGHT4] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setJointData(const U64* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_JOINT] + offset * sTypeSize[TYPE_JOINT], mOffsets[TYPE_JOINT] + (offset + count) * sTypeSize[TYPE_JOINT] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setIndexData(const U16* data, U32 offset, U32 count)
{
    flush_vbo(GL_ELEMENT_ARRAY_BUFFER, offset * sizeof(U16), (offset + count) * sizeof(U16) - 1, (U8*)data, mMappedIndexData);
}

void LLVertexBuffer::setIndexData(const U32* data, U32 offset, U32 count)
{
    if (mIndicesType != GL_UNSIGNED_INT)
    { // HACK -- vertex buffers are initialized as 16-bit indices, but can be switched to 32-bit indices
        mIndicesType = GL_UNSIGNED_INT;
        mIndicesStride = 4;
        mNumIndices /= 2;
    }
    flush_vbo(GL_ELEMENT_ARRAY_BUFFER, offset * sizeof(U32), (offset + count) * sizeof(U32) - 1, (U8*)data, mMappedIndexData);
}




