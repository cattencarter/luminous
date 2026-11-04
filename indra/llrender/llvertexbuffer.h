/**
 * @file llvertexbuffer.h
 * @brief LLVertexBuffer wrapper for OpengGL vertex buffer objects
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

#ifndef LL_LLVERTEXBUFFER_H
#define LL_LLVERTEXBUFFER_H

#include "llgl.h"
#include "v2math.h"
#include "v3math.h"
#include "v4math.h"
#include "v4coloru.h"
#include "llstrider.h"
#include "llrender.h"
#include "lltrace.h"
#include <map>
#include <set>
#include <vector>
#include <list>
#include <glm/gtc/matrix_transform.hpp>

#define LL_MAX_VERTEX_ATTRIB_LOCATION 64

//============================================================================
// NOTES
// Threading:
//  All constructors should take an 'create' paramater which should only be
//  'true' when called from the main thread. Otherwise createGLBuffer() will
//  be called as soon as getVertexPointer(), etc is called (which MUST ONLY be
//  called from the main (i.e OpenGL) thread)


//============================================================================
// base class
class LLPrivateMemoryPool;
class LLVertexBuffer;

// [Viewer Lab]
namespace LL::RHI
{
    class Buffer;
    class Texture;
    struct VertexStreams;
    struct PipelineDesc;
    struct DepthStencilDesc;
    struct PassFormats;
}

// [Viewer Lab] The table behind the display list entries that hold each texture of the device (llvertexbuffer.cpp): it maps
// a texture's id to the first Entry of its intrusive list, with open addressing and linear probing. Tracking and
// untracking allocate nothing: the table is touched only when an entry is or becomes the first of its id, and it allocates
// only as it grows at half load. Here rather than in the .cpp so that a unit test reaches its probe, erase and grow paths
// directly.
template <class Entry>
class LLTrackedEntryTable
{
public:
    LLTrackedEntryTable() : mSlots(256), mShift(64 - 8) {}

    // The slot of the id's first entry, made with none if the id has no list
    Entry*& first(U64 id)
    {
        llassert(id != 0);
        size_t i = find(id);
        if (mSlots[i].mId == 0)
        {
            if ((mUsed + 1) * 2 > mSlots.size())
            {
                grow();
                i = find(id);
            }
            mSlots[i].mId = id;
            ++mUsed;
        }
        return mSlots[i].mFirst;
    }

    // The id's first entry, or null
    Entry* peek(U64 id) const
    {
        const Slot& slot = mSlots[find(id)];
        return slot.mId == id ? slot.mFirst : nullptr;
    }

    // The id's list starts with this entry; with none, the id goes
    void setFirst(U64 id, Entry* entry)
    {
        const size_t i = find(id);
        llassert(id != 0 && mSlots[i].mId == id);
        if (entry)
        {
            mSlots[i].mFirst = entry;
        }
        else
        {
            erase(i);
        }
    }

    // Takes the id out and returns its first entry, or null
    Entry* take(U64 id)
    {
        const size_t i = find(id);
        if (id == 0 || mSlots[i].mId != id)
        {
            return nullptr;
        }
        Entry* entry = mSlots[i].mFirst;
        erase(i);
        return entry;
    }

    // Tests only: the slots in use, the table's size, and an id's home slot
    size_t used() const { return mUsed; }
    size_t capacity() const { return mSlots.size(); }
    size_t home(U64 id) const { return size_t((id * 0x9E3779B97F4A7C15ull) >> mShift); }

private:
    struct Slot
    {
        U64 mId = 0; // 0: free
        Entry* mFirst = nullptr;
    };

    // The id's slot, or the free slot where it would go
    size_t find(U64 id) const
    {
        const size_t mask = mSlots.size() - 1;
        size_t i = home(id);
        while (mSlots[i].mId != id && mSlots[i].mId != 0)
        {
            i = (i + 1) & mask;
        }
        return i;
    }

    // Backward-shift deletion: the later slots of the probe run move up, so lookups need no markers
    void erase(size_t i)
    {
        const size_t mask = mSlots.size() - 1;
        for (size_t j = (i + 1) & mask; mSlots[j].mId != 0; j = (j + 1) & mask)
        {
            // j's id may fill the hole at i only if its home is not after i in the run
            if (((j - home(mSlots[j].mId)) & mask) >= ((j - i) & mask))
            {
                mSlots[i] = mSlots[j];
                i = j;
            }
        }
        mSlots[i] = Slot();
        --mUsed;
    }

    void grow()
    {
        std::vector<Slot> old(mSlots.size() * 2);
        old.swap(mSlots);
        --mShift;
        for (const Slot& slot : old)
        {
            if (slot.mId != 0)
            {
                mSlots[find(slot.mId)] = slot;
            }
        }
    }

    std::vector<Slot> mSlots; // a power of two, at most half used
    U32 mShift;               // 64 minus the power
    size_t mUsed = 0;
};


class LLVertexBufferData
{
public:
    // [Viewer Lab] Defined in llvertexbuffer.cpp, where LL::RHI::Texture is complete for mTexture
    LLVertexBufferData();
    LLVertexBufferData(LLVertexBuffer* buffer, U8 mode, U32 count, LL::RHI::Texture* texture, U64 texture_id, U32 tex_name, const glm::mat4& model_view, const glm::mat4& projection, const glm::mat4& texture0);
    LLVertexBufferData(const LLVertexBufferData& other);
    LLVertexBufferData& operator=(const LLVertexBufferData& other);
    ~LLVertexBufferData();
    void drawWithMatrix();
    void draw();
    LLPointer<LLVertexBuffer> mVB;
    U8 mMode;
    U32 mCount;
    // [Viewer Lab] Texture unit 0's binding at the flush by the device's texture and its id, which draw binds again
    // through LLTexUnit::bindTexture, and unit 0's GL name at the flush, as upstream. mTexName is bound
    // raw only while the entry holds no texture of the device: there was none at the flush, or the texture's owner has
    // released it since; a GL-backend path that stays
    LLPointer<LL::RHI::Texture> mTexture;
    U64 mTextureId;
    U32 mTexName;
    glm::mat4 mProjection;
    glm::mat4 mModelView;
    glm::mat4 mTexture0;

    // [Viewer Lab] Main thread: the entries made on the main thread that hold the texture with this id let go of it, as
    // its owner releases it (LLTexUnit::releaseTexture), so a list no longer keeps a released texture and its GL name
    // alive; their id and GL name stay
    static void releaseTexture(U64 texture_id);
    // [Viewer Lab] For tests, main thread: how many entries hold the texture with this id and let go of it at its release
    static U32 trackedEntries(U64 texture_id);

private:
    // [Viewer Lab] The release's registry: track adds an entry that holds a texture of the device on the main thread to
    // the list of the texture's id, untrack takes it out; neither allocates
    void track();
    void untrack();
    U64 mTrackedId = 0; // [Viewer Lab] the id the entry is tracked under, 0 when it is not; copies do not take it
    LLVertexBufferData* mTrackedPrev = nullptr; // [Viewer Lab] its neighbours in its id's list; copies do not take them
    LLVertexBufferData* mTrackedNext = nullptr;
};
typedef std::list<LLVertexBufferData> buffer_data_list_t;

class LLVertexBuffer final : public LLRefCount
{
public:
    struct MappedRegion
    {
        U32 mStart;
        U32 mEnd;
    };

    LLVertexBuffer(const LLVertexBuffer& rhs) = delete;
    const LLVertexBuffer& operator=(const LLVertexBuffer& rhs) = delete;

    static void initClass(LLWindow* window);
    static void cleanupClass();
    static void setupClientArrays(U32 data_mask);
    static void drawArrays(U32 mode, const std::vector<LLVector3>& pos);
    static void drawElements(U32 mode, const LLVector4a* pos, const LLVector2* tc, U32 num_indices, const U16* indicesp);

    static void unbind(); //unbind any bound vertex buffer

    // [Viewer Lab] Buffer names from the same pool, and deleted with the same delay, as the
    // vertex buffers' own, for the render interface's GL backend (llrhigl.cpp).
    static U32 genGLBuffer();
    static void deleteGLBuffers(S32 count, U32* buffers);
    // [Viewer Lab] The GL storage of LLVertexBuffer's buffers, from the VBO pool initClass picked,
    // with its GL calls, memory accounting and deletion delays, for the render interface's GL
    // backend (llrhigl.cpp). Main thread.
    static bool allocateGLStorage(U32 target, U32 size, U32& name);
    static void updateGLStorage(U32 target, U32 size, U32& name, U32 offset, const U8* data, U32 bytes);
    static void freeGLStorage(U32 target, U32 size, U32 name);
    // [Viewer Lab] Attribute pointers for the bound array buffer, as setBuffer sets them.
    static void setupVertexAttributes(U32 data_mask, const U32* offsets);

    // [Viewer Lab] The draws and setBuffer calls since the process began, main thread: the draws (draw counts
    // once, through drawRange), those with a shader bound, with the part's RHI storage (its indices too for an indexed
    // draw) and with a program of the viewer's current device, and those skipped because the bound shader was refused or
    // its pipeline failed; setBuffer's calls, those where the part lacks one of the shader's attributes, and those where
    // the stream of one of the shader's attributes starts at or past the part's vertices; and the indexed draws whose
    // indices end past the part's. For level 1's per-scene lines, golden level 2's render profile and the tests.
    // Each draw not skipped goes to the viewer's encoder, or stays raw GL counted by its reason: no device, no RHI program
    // of the device for the bound shader, no RHI storage for the part, no pass to record into (a passless level), or no
    // pipeline or depth state for its description. So the draws are skipped + encoded + the five reasons. Of the draws
    // that reach their pipeline: those in another pass than the draw before, with other pass formats, the pipeline table's
    // lookups (a change of the draw's key) and the device's pipeline and depth state requests (the tables' misses), the
    // descriptions whose mask is not the program's, the depth states that write in a pass with read-only depth, the draws
    // in a pass with read-only depth (golden level 2's floor on the passes that must be read-only), and the
    // encoder calls made: pipelines, depth states, vertex buffers and index buffers set.
    struct DrawCounts
    {
        U64 draws = 0;
        U64 drawsWithShader = 0;
        U64 drawsWithStorage = 0;
        U64 drawsWithProgram = 0;
        U64 setBuffers = 0;
        U64 missingAttributes = 0;
        U64 streamsBeyondBuffer = 0;
        U64 indicesBeyondBuffer = 0;
        U64 skipped = 0;
        U64 encoded = 0;
        U64 passless = 0;
        U64 withoutDevice = 0;
        U64 withoutProgram = 0;
        U64 withoutStorage = 0;
        U64 withoutPipeline = 0;
        U64 passesWithDraws = 0;
        U64 formatChanges = 0;
        U64 tableLookups = 0;
        U64 pipelineRequests = 0;
        U64 depthRequests = 0;
        U64 attributeMaskMismatches = 0;
        U64 depthWritesInReadOnlyPass = 0;
        U64 drawsInReadOnlyPass = 0;
        U64 pipelineSets = 0;
        U64 depthSets = 0;
        U64 vertexBufferSets = 0;
        U64 indexBufferSets = 0;
    };
    static DrawCounts drawCounts();
    // [Viewer Lab] While sTallyPrograms is set, every counted draw with a shader bound is tallied by the
    // shader's name (golden level 2's draws per program over the warm-up, the floors in features.json). Off unless the
    // golden hook sets it; main thread; no GL.
    static bool sTallyPrograms;
    static const std::map<std::string, U64>& programTally();
    static void clearProgramTally();
    // [Viewer Lab] One draw skipped before it reaches drawRange, counted as drawRange counts a skipped one (no part's
    // storage): LLRender::flush's batch for a shader bind() refused, which it drops before making a buffer; main thread.
    static void noteSkippedDraw();
    // [Viewer Lab] Whether a new part is bound before its data is set, as LLRender::genBuffer asks: with a VBO pool of the
    // GL backend that updates the bound GL buffer at each set (glBufferSubData), not with Apple's, which names the part and
    // sends its whole copy in _unmapBuffer, nor when GL is not the backend. The pool's choice, not a GPU flag.
    static bool bindsBeforeData();

    // [Viewer Lab] The census's predicates, named so the tests check them at their boundaries: a part
    // without one of the shader's attributes; the stream of an attribute of the mask starting at or past the part's
    // vertices, which the render interface's tracker refuses (the constructor zeroes the offsets of the attributes a part
    // does not have); an indexed draw whose indices end past the part's; a depth state that writes in a pass with read-only
    // depth.
    static bool lacksAttributes(U32 shader_mask, U32 type_mask) { return (shader_mask & type_mask) != shader_mask; }
    static bool streamsBeyond(U32 mask, const U32* offsets, U32 size);
    static bool indicesBeyond(U32 indices_offset, U32 count, U32 stride, U32 size)
    {
        return (static_cast<U64>(indices_offset) + count) * stride > size; // not U64(): OpenSSL's sha.h defines a U64 macro
    }
    static bool writesInReadOnlyPass(bool read_only, bool depth_write) { return read_only && depth_write; }

    // [Viewer Lab] True on the main thread while LLVertexBuffer gives a draw, its pipeline, depth state and buffers to the
    // viewer's encoder: the GL backend then only tracks them, and keeps them apart from the interface's, because
    // LLVertexBuffer's GL backend functions make the GL calls themselves.
    static bool makesOwnGLDraws();
    // [Viewer Lab] That scope. It puts the previous value back, as LLRenderTarget's does, so it nests; main thread only
    // (a static, not thread_local). A GL backend switch, part of its contract with llrender (llrhigl.cpp).
    struct OwnGLDraws
    {
        OwnGLDraws();
        ~OwnGLDraws();
        OwnGLDraws(const OwnGLDraws&)            = delete;
        OwnGLDraws& operator=(const OwnGLDraws&) = delete;

    private:
        const bool mPrevious;
    };

    // [Viewer Lab] What the render interface gets for a draw, from llrender's caches and with no GL:
    // - the streams of a shader's mask, as setupVertexAttributes sets the attributes up: the mask is the shader's, which
    //   setupClientArrays enables, the colour's stream is the emissive one whenever the mask has emissive, and the texture
    //   index is in the position's fourth component;
    static void vertexStreams(U32 data_mask, const U32* offsets, LL::RHI::VertexStreams& streams);
    // - the pipeline of a draw in a mode with a shader's mask in a pass of these formats: the primitive class, LLGLState's
    //   GL_BLEND, LLRender's blend factors (GL's initial One and Zero before the first blendFunc) and colour mask,
    //   normalized;
    static void describePipeline(U32 mode, U32 data_mask, const LL::RHI::PassFormats& pass, LL::RHI::PipelineDesc& pipeline);
    // - the depth state of LLGLDepthTest: no writes while the test is off, as GL; false for a function the table lacks.
    static bool describeDepth(LL::RHI::DepthStencilDesc& depth);

    //get the size of a vertex with the given typemask
    static U32 calcVertexSize(const U32& typemask);

    //get the size of a buffer with the given typemask and vertex count
    //fill offsets with the offset of each vertex component array into the buffer
    // indexed by the following enum
    static U32 calcOffsets(const U32& typemask, U32* offsets, U32 num_vertices);

    // flush any pending mapped buffers
    static void flushBuffers();

    //WARNING -- when updating these enums you MUST
    // 1 - update LLVertexBuffer::sTypeSize
    // 2 - update LLVertexBuffer::vb_type_name
    // 3 - add a strider accessor
    // 4 - modify LLVertexBuffer::setupVertexAttributes and vertexStreams
    // 6 - modify LLViewerShaderMgr::mReservedAttribs

    // clang-format off
    enum AttributeType {                      // Shader attribute name, set in LLShaderMgr::initAttribsAndUniforms()
        TYPE_VERTEX = 0,        //  "position"
        TYPE_NORMAL,            //  "normal"
        TYPE_TEXCOORD0,         //  "texcoord0"
        TYPE_TEXCOORD1,         //  "texcoord1"
        TYPE_TEXCOORD2,         //  "texcoord2"
        TYPE_TEXCOORD3,         //  "texcoord3"
        TYPE_COLOR,             //  "diffuse_color"
        TYPE_EMISSIVE,          //  "emissive"
        TYPE_TANGENT,           //  "tangent"
        TYPE_WEIGHT,            //  "weight"
        TYPE_WEIGHT4,           //  "weight4"
        TYPE_CLOTHWEIGHT,       //  "clothing"
        TYPE_JOINT,             //  "joint"
        TYPE_TEXTURE_INDEX,     //  "texture_index"
        TYPE_MAX,   // TYPE_MAX is the size/boundary marker for attributes that go in the vertex buffer
        TYPE_INDEX, // TYPE_INDEX is beyond _MAX because it lives in a separate (index) buffer
    };
    // clang-format on

    enum {
        MAP_VERTEX = (1<<TYPE_VERTEX),
        MAP_NORMAL = (1<<TYPE_NORMAL),
        MAP_TEXCOORD0 = (1<<TYPE_TEXCOORD0),
        MAP_TEXCOORD1 = (1<<TYPE_TEXCOORD1),
        MAP_TEXCOORD2 = (1<<TYPE_TEXCOORD2),
        MAP_TEXCOORD3 = (1<<TYPE_TEXCOORD3),
        MAP_COLOR = (1<<TYPE_COLOR),
        MAP_EMISSIVE = (1<<TYPE_EMISSIVE),
        MAP_TANGENT = (1<<TYPE_TANGENT),
        MAP_WEIGHT = (1<<TYPE_WEIGHT),
        MAP_WEIGHT4 = (1<<TYPE_WEIGHT4),
        MAP_CLOTHWEIGHT = (1<<TYPE_CLOTHWEIGHT),
        MAP_JOINT = (1<<TYPE_JOINT),
        MAP_TEXTURE_INDEX = (1<<TYPE_TEXTURE_INDEX),
    };

protected:
    friend class LLRender;

    ~LLVertexBuffer(); // use unref()

    void    genBuffer(U32 size);
    void    genIndices(U32 size);
    bool    createGLBuffer(U32 size);
    bool    createGLIndices(U32 size);
    void    destroyGLBuffer();
    void    destroyGLIndices();
    bool    updateNumVerts(U32 nverts);
    bool    updateNumIndices(U32 nindices);

public:
    LLVertexBuffer(U32 typemask);

    // allocate buffer
    bool    allocateBuffer(U32 nverts, U32 nindices);

    // map for data access (see also getFooStrider below)
    U8*     mapVertexBuffer(AttributeType type, U32 index, S32 count = -1);
    U8*     mapIndexBuffer(U32 index, S32 count = -1);

    // synonym for flushBuffers
    void    unmapBuffer();

    // set for rendering
    // assumes (and will assert on) the following:
    //      - this buffer has no pending unmapBuffer call
    //      - a shader is currently bound
    //      - This buffer has sufficient attributes within it to satisfy the needs of the currently bound shader
    void    setBuffer();

    // Only call each getVertexPointer, etc, once before calling unmapBuffer()
    // call unmapBuffer() after calls to getXXXStrider() before any calls to setBuffer()
    // example:
    //   vb->getVertexBuffer(verts);
    //   vb->getNormalStrider(norms);
    //   setVertsNorms(verts, norms);
    //   vb->unmapBuffer();
    bool getVertexStrider(LLStrider<LLVector3>& strider, U32 index=0, S32 count = -1);
    bool getVertexStrider(LLStrider<LLVector4a>& strider, U32 index=0, S32 count = -1);
    bool getIndexStrider(LLStrider<U16>& strider, U32 index=0, S32 count = -1);
    bool getTexCoord0Strider(LLStrider<LLVector2>& strider, U32 index=0, S32 count = -1);
    bool getTexCoord1Strider(LLStrider<LLVector2>& strider, U32 index=0, S32 count = -1);
    bool getTexCoord2Strider(LLStrider<LLVector2>& strider, U32 index=0, S32 count = -1);
    bool getNormalStrider(LLStrider<LLVector3>& strider, U32 index=0, S32 count = -1);
    bool getNormalStrider(LLStrider<LLVector4a>& strider, U32 index = 0, S32 count = -1);
    bool getTangentStrider(LLStrider<LLVector3>& strider, U32 index=0, S32 count = -1);
    bool getTangentStrider(LLStrider<LLVector4a>& strider, U32 index=0, S32 count = -1);
    bool getColorStrider(LLStrider<LLColor4U>& strider, U32 index=0, S32 count = -1);
    bool getEmissiveStrider(LLStrider<LLColor4U>& strider, U32 index=0, S32 count = -1);
    bool getWeightStrider(LLStrider<F32>& strider, U32 index=0, S32 count = -1);
    // <FS:Ansariel> Vectorized Weight4Strider and ClothWeightStrider by Drake Arconis
    //bool getWeight4Strider(LLStrider<LLVector4>& strider, U32 index=0, S32 count = -1);
    //bool getClothWeightStrider(LLStrider<LLVector4>& strider, U32 index=0, S32 count = -1);
    bool getWeight4Strider(LLStrider<LLVector4a>& strider, U32 index=0, S32 count = -1);
    bool getClothWeightStrider(LLStrider<LLVector4a>& strider, U32 index=0, S32 count = -1);
    // </FS:Ansariel>

    void setPositionData(const LLVector4a* data);
    void setNormalData(const LLVector4a* data);
    void setTangentData(const LLVector4a* data);
    void setWeight4Data(const LLVector4a* data);
    void setJointData(const U64* data);
    void setTexCoord0Data(const LLVector2* data);
    void setTexCoord1Data(const LLVector2* data);
    void setColorData(const LLColor4U* data);
    void setIndexData(const U16* data);
    void setIndexData(const U32* data);

    void setPositionData(const LLVector4a* data, U32 offset, U32 count);
    void setNormalData(const LLVector4a* data, U32 offset, U32 count);
    void setTangentData(const LLVector4a* data, U32 offset, U32 count);
    void setWeight4Data(const LLVector4a* data, U32 offset, U32 count);
    void setJointData(const U64* data, U32 offset, U32 count);
    void setTexCoord0Data(const LLVector2* data, U32 offset, U32 count);
    void setTexCoord1Data(const LLVector2* data, U32 offset, U32 count);
    void setColorData(const LLColor4U* data, U32 offset, U32 count);
    void setIndexData(const U16* data, U32 offset, U32 count);
    void setIndexData(const U32* data, U32 offset, U32 count);


    U32 getNumVerts() const                 { return mNumVerts; }
    U32 getNumIndices() const               { return mNumIndices; }

    U32 getTypeMask() const                 { return mTypeMask; }
    bool hasDataType(AttributeType type) const { return ((1 << type) & getTypeMask()); }
    U32 getSize() const                     { return mSize; }
    // [Viewer Lab] For the tests and the draws through the render interface. Update the buffers through LLVertexBuffer:
    // a whole update made on them directly gives a new GL name that mGLBuffer and mGLIndices do not follow.
    LL::RHI::Buffer* getRHIBuffer() const   { return mRHIBuffer.get(); }
    LL::RHI::Buffer* getRHIIndices() const  { return mRHIIndices.get(); }
    U32 getIndicesSize() const              { return mIndicesSize; }
    U8* getMappedData() const               { return mMappedData; }
    U8* getMappedIndices() const            { return mMappedIndexData; }
    U32 getOffset(AttributeType type) const { return mOffsets[type]; }

    // these functions assume (and assert on) the current VBO being bound
    // Detailed error checking can be enabled by setting gDebugGL to true
    void draw(U32 mode, U32 count, U32 indices_offset) const;
    // [Viewer Lab] Returns whether the draw was delivered: the viewer's encoder made it (its tracker's draws() went up) or
    // the GL draw was made; false for a draw skipped or left raw off GL. LLImageGL::scaleDown copies the draw's result
    // only when it was delivered.
    bool drawArrays(U32 mode, U32 offset, U32 count) const;
    void drawRange(U32 mode, U32 start, U32 end, U32 count, U32 indices_offset) const;

    // draw without syncing matrices.  If you're positive there have been no matrix
    // since the last call to syncMatrices, this is much faster than drawRange
    void drawRangeFast(U32 mode, U32 start, U32 end, U32 count, U32 indices_offset) const;

    //for debugging, validate data in given range is valid
    bool validateRange(U32 start, U32 end, U32 count, U32 offset) const;

    #if LL_PROFILER_ENABLE_RENDER_DOC
    void setLabel(const char* label);
    #endif

    void clone(LLVertexBuffer& target) const;

protected:
    U32     mGLBuffer = 0;      // GL VBO handle ([Viewer Lab] the RHI buffer's GL name when there is one, LL::RHI::GLNames::of)
    U32     mGLIndices = 0;     // GL IBO handle ([Viewer Lab] the RHI buffer's GL name when there is one, LL::RHI::GLNames::of)
    U32     mNumVerts = 0;      // Number of vertices allocated
    U32     mNumIndices = 0;    // Number of indices allocated
    U32     mIndicesType = GL_UNSIGNED_SHORT; // type of indices in index buffer
    U32     mIndicesStride = 2;     // size of each index in bytes
    U32     mOffsets[TYPE_MAX]; // byte offsets into mMappedData of each attribute

    U8* mMappedData = nullptr;  // pointer to currently mapped data (NULL if unmapped)
    U8* mMappedIndexData = nullptr; // pointer to currently mapped indices (NULL if unmapped)

    U32     mTypeMask = 0;      // bitmask of present vertex attributes

    U32     mSize = 0;          // size in bytes of mMappedData
    U32     mIndicesSize = 0;   // size in bytes of mMappedIndexData

    std::vector<MappedRegion> mMappedVertexRegions;  // list of mMappedData byte ranges that must be sent to GL
    std::vector<MappedRegion> mMappedIndexRegions;   // list of mMappedIndexData byte ranges that must be sent to GL

private:
    // DEPRECATED
    // These function signatures are deprecated, but for some reason
    // there are classes in an external package that depend on LLVertexBuffer

    // TODO: move these classes into viewer repository
    friend class LLNavShapeVBOManager;
    friend class LLNavMeshVBOManager;

    void flush_vbo(GLenum target, U32 start, U32 end, void* data, U8* dst);

    LLVertexBuffer(U32 typemask, U32 usage)
        : LLVertexBuffer(typemask)
    {}

    bool    allocateBuffer(S32 nverts, S32 nindices, bool create) { return allocateBuffer(nverts, nindices); }

    // actually unmap buffer
    void _unmapBuffer();

    // [Viewer Lab] GL's part of an update of bytes [offset, offset + bytes) of the vertices or the
    // indices
    void updateStorage(U32 target, U32 offset, const U8* data, U32 bytes);

    // add to set of mapped buffers
    void _mapBuffer();
    bool mMapped = false;

    // [Viewer Lab] A draw of this part with the bound shader, given to the viewer's encoder in the open pass before GL's
    // draw: the pipeline, depth state, buffers and draw, inside OwnGLDraws, so GL's calls stay LLVertexBuffer's own; or
    // counted by the reason it stays raw GL. Main thread; no GL. Returns whether the encoder made the draw, by its
    // tracker's draws() across the call.
    bool encodeDraw(U32 mode, bool indexed, U32 first, U32 count) const;

    // [Viewer Lab] The storage on the viewer's render device; null when the part was made
    // without it. After the existing members, so their offsets stay, but the class grows by two
    // pointers: code built against the old header, such as the prebuilt llphysicsextensions_tpv of
    // HAVOK_TPV builds, which allocates these objects, must be rebuilt against this one.
    LLPointer<LL::RHI::Buffer> mRHIBuffer;
    LLPointer<LL::RHI::Buffer> mRHIIndices;

public:

    static U64 getBytesAllocated();
    static const U32 sTypeSize[TYPE_MAX];
    static const U32 sGLMode[LLRender::NUM_MODES];
    static U32 sGLRenderBuffer;
    static U32 sGLRenderIndices;
    static U32 sLastMask;
    static U32 sVertexCount;
};

#if LL_PROFILER_ENABLE_RENDER_DOC
#define LL_LABEL_VERTEX_BUFFER(buf, name) buf->setLabel(name)
#else
#define LL_LABEL_VERTEX_BUFFER(buf, name)
#endif


#endif // LL_LLVERTEXBUFFER_H
