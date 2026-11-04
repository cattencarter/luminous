/**
 * @file llglslshader.h
 * @brief GLSL shader wrappers
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#ifndef LL_LLGLSLSHADER_H
#define LL_LLGLSLSHADER_H

#include "llgl.h"
#include "llrender.h"
#include "llstaticstringtable.h"
#include <boost/json.hpp>
#include <span>
#include <unordered_map>

// [Viewer Lab]
namespace LL::RHI
{
    class Buffer; // [Viewer Lab]
    class Program;
    class Query; // [Viewer Lab]
    struct ProgramReflection;
}

class LLShaderFeatures
{
public:
    S32 mIndexedTextureChannels = 0;
    bool calculatesLighting = false;
    bool calculatesAtmospherics = false;
    bool hasLighting = false; // implies no transport (it's possible to have neither though)
    bool isAlphaLighting = false; // indicates lighting shaders need not be linked in (lighting performed directly in alpha shader to match deferred lighting functions)
    bool isSpecular = false;
    bool hasTransport = false; // implies no lighting (it's possible to have neither though)
    bool hasSkinning = false;
    bool hasObjectSkinning = false;
    bool mGLTF = false;
    bool hasAtmospherics = false;
    bool hasGamma = false;
    bool hasShadows = false;
    bool hasAmbientOcclusion = false;
    bool hasSrgb = false;
    bool isDeferred = false;
    bool hasFullGBuffer = false;
    bool hasScreenSpaceReflections = false;
    bool hasAlphaMask = false;
    bool hasReflectionProbes = false;
    bool attachNothing = false;
    bool hasHeroProbes = false;
    bool isPBRTerrain = false;
    bool hasTonemap = false;
};

// ============= Structure for caching shader uniforms ===============
class LLGLSLShader;

class LLShaderUniforms
{
public:

    template<typename T>
    struct UniformSetting
    {
        S32 mUniform{ 0 };
        T mValue{};
    };

    typedef UniformSetting<S32> IntSetting;
    typedef UniformSetting<F32> FloatSetting;
    typedef UniformSetting<LLVector4> VectorSetting;
    typedef UniformSetting<LLVector3> Vector3Setting;

    void clear()
    {
        mIntegers.resize(0);
        mFloats.resize(0);
        mVectors.resize(0);
        mVector3s.resize(0);
    }

    void uniform1i(S32 index, S32 value)
    {
        mIntegers.push_back({ index, value });
    }

    void uniform1f(S32 index, F32 value)
    {
        mFloats.push_back({ index, value });
    }

    void uniform4fv(S32 index, const LLVector4& value)
    {
        mVectors.push_back({ index, value });
    }

    void uniform4fv(S32 index, const F32* value)
    {
        mVectors.push_back({ index, LLVector4(value) });
    }

    void uniform3fv(S32 index, const LLVector3& value)
    {
        mVector3s.push_back({ index, value });
    }

    void uniform3fv(S32 index, const F32* value)
    {
        mVector3s.push_back({ index, LLVector3(value) });
    }

    void apply(LLGLSLShader* shader);


    std::vector<IntSetting> mIntegers;
    std::vector<FloatSetting> mFloats;
    std::vector<VectorSetting> mVectors;
    std::vector<Vector3Setting> mVector3s;
};

// [Viewer Lab] One query of the viewer's render device, in GL's terms: LLGLSLShader's profiling, LLVOAvatar's GPU
// timer, the occlusion queries and the GPU benchmark. As a GL name: made once, begun and ended on its target again and
// again, polled with get(GL_QUERY_RESULT_AVAILABLE) or waited for with get(GL_QUERY_RESULT), and released. Copies share the
// query, as copied names did; a move leaves the source empty. Main thread only: elsewhere each
// call warns and does nothing. Without the device (headless, llappearanceutility, the tests' fixture), or for a kind the
// device lacks, it is empty, as GL's name 0: begin is false, and end and get do nothing and give 0, with no GL call.
// release(), the destructor and the assignments let go of the query as LLGLSLShader lets go of its program: on the main
// thread while the device that made it is the viewer's, otherwise it is kept unreleased. keep() always keeps it, with no
// GL call, for a holder that goes without one (~LLGLSLShader). The branch is on the device and its capabilities, never on
// the backend.
class LLGPUQuery
{
public:
    LLGPUQuery(); // defined where LL::RHI::Query is complete, as the other special members
    ~LLGPUQuery();
    LLGPUQuery(const LLGPUQuery&);
    LLGPUQuery& operator=(const LLGPUQuery&);
    LLGPUQuery(LLGPUQuery&&) noexcept;
    LLGPUQuery& operator=(LLGPUQuery&&) noexcept;

    /// A query for GL_SAMPLES_PASSED, GL_ANY_SAMPLES_PASSED, GL_TIME_ELAPSED or GL_PRIMITIVES_GENERATED. Empty without the
    /// device and, with no WARN line, for a kind the device lacks; empty with a WARN line for another target.
    static LLGPUQuery create(U32 target);
    /// count of them in one allocation of the device's, appended to queries; none where create gives an empty one.
    static void create(U32 target, U32 count, std::vector<LLGPUQuery>& queries);
    /// Whether the device has the target's kind (LLPipeline::profileAvatar asks GL_TIME_ELAPSED); false without it.
    static bool supported(U32 target);

    explicit operator bool() const; ///< holds a query of the viewer's device, as a GL name != 0
    /// Begins it on the target it was made for: false when empty, with a WARN line for another target, or when the
    /// encoder refuses it (the encoder's WARN line). A refused begin of the query while it is active leaves it active.
    bool begin(U32 target);
    /// Ends it, when its begin was accepted; otherwise nothing, with no WARN line. A WARN line for another target.
    void end(U32 target);
    /// GL_QUERY_RESULT_AVAILABLE (GL_TRUE or GL_FALSE; never waits) or GL_QUERY_RESULT (waits); 0 when empty.
    U64 get(U32 pname) const;
    void release();
    void keep(); ///< drops it without releasing it: no GL call, and GL keeps the name
    U64 id() const; ///< the query's id, or 0

private:
    LLPointer<LL::RHI::Query> mQuery;
    U64 mDevice = 0;      ///< the id of the device that made it
    bool mActive = false; ///< begun with the encoder's accept, and not ended since
};

class LLGLSLShader
{
public:
    // NOTE: Keep gShaderConsts and LLGLSLShader::ShaderConsts_e in sync!
    enum eShaderConsts
    {
        SHADER_CONST_CLOUD_MOON_DEPTH
        , SHADER_CONST_STAR_DEPTH
        , NUM_SHADER_CONSTS
    };

    // enum primarily used to control application sky settings uniforms
    typedef enum
    {
        SG_DEFAULT = 0,  // not sky or water specific
        SG_SKY,  //
        SG_WATER,
        SG_ANY,
        SG_COUNT
    } eGroup;

    enum UniformBlock : GLuint
    {
        UB_REFLECTION_PROBES,   // "ReflectionProbes"
        UB_GLTF_JOINTS,         // "GLTFJoints"
        UB_GLTF_NODES,          // "GLTFNodes"
        UB_GLTF_MATERIALS,      // "GLTFMaterials"
        NUM_UNIFORM_BLOCKS
    };
    /// [Viewer Lab] GL's binding point of the render interface's block slot 0, above LLGLSLShader's fixed points: a block
    /// that an LLShaderUniformBuffer holds, or that is declared (declareBlock), is read at this + block
    static constexpr GLuint INTERFACE_BLOCK_BINDING = 8;
    static const char* uniformBlockName(UniformBlock block); ///< [Viewer Lab] its name in GLSL, for the GL backend; null past the four
    /// [Viewer Lab] the four names in UniformBlock order, with static lifetime: ProgramDesc::uniformBlocks
    static std::span<const char* const> uniformBlockNames();


    static std::set<LLGLSLShader*> sInstances;
    static bool sProfileEnabled;
    static bool sCanProfile;

    LLGLSLShader();
    // [Viewer Lab] A copy shares the profile queries, as copied names did. A destroyed copy keeps its reference
    // (~LLGLSLShader's keep()), so the other's unload then keeps the timer and samples names where it deleted them
    // before; no code copies a shader today.
    LLGLSLShader(const LLGLSLShader&);            // [Viewer Lab] defined where LL::RHI::Program is complete
    LLGLSLShader& operator=(const LLGLSLShader&); // [Viewer Lab]
    ~LLGLSLShader();

    static GLuint sCurBoundShader;
    static LLGLSLShader* sCurBoundShaderPtr;
    static S32 sIndexedTextureChannels;

    static U32 sMaxGLTFMaterials;
    static U32 sMaxGLTFNodes;

    static void initProfile();
    // [Viewer Lab] publish holds each linked program's line "Program '<name>': shader class N (asked M)"
    // until this writes the held lines, at STATE_STARTED or at the viewer's cleanup, whichever comes first, so the startup
    // pays no log write per program; from then on each link logs its line at once.
    static void writeClassLines();
    static void finishProfile(boost::json::value& stats=sDefaultStats);

    static void startProfile();
    static void stopProfile();

    // [Viewer Lab] createShader's, and the tests': names the GL program the viewer's device adopts during one
    // createProgram call on this thread. It puts the previous program back, so a nested scope never clears an outer
    // one's; per thread (thread_local). Kept as a GL backend switch, its contract with llrender (llrhigl.cpp).
    struct OwnGLProgramScope
    {
        explicit OwnGLProgramScope(U32 program);
        ~OwnGLProgramScope();
        OwnGLProgramScope(const OwnGLProgramScope&)            = delete;
        OwnGLProgramScope& operator=(const OwnGLProgramScope&) = delete;

    private:
        const U32 mPrevious;
    };
    static U32 ownGLProgram(); ///< [Viewer Lab] for the GL backend (llrhigl.cpp): that program, or 0

    // [Viewer Lab] The uniform setters through the viewer's render device: each makes today's GL call
    // itself and, when it sends a value, gives the encoder what GL takes for the reflected uniform.
    enum class GLUniformCall : U8; ///< [Viewer Lab] the GL entry point a setter calls today, defined in llglslshader.cpp
    /// [Viewer Lab] True on this thread while LLGLSLShader gives its own uniform call to the encoder; the GL backend then only
    /// checks the call, because LLGLSLShader makes today's GL call itself.
    static bool makesOwnGLUniforms();
    /// [Viewer Lab] That scope, for LLGLSLShader's own calls and the tests. It puts the previous value back, so it
    /// nests; per thread (thread_local). Kept as a GL backend switch, its contract with llrender (llrhigl.cpp).
    struct OwnGLUniforms
    {
        OwnGLUniforms();
        ~OwnGLUniforms();
        OwnGLUniforms(const OwnGLUniforms&)            = delete;
        OwnGLUniforms& operator=(const OwnGLUniforms&) = delete;

    private:
        const bool mPrevious;
    };
    /// [Viewer Lab] What the setters have given the encoder since the process began: values, sampler channels, calls that do
    /// not fit the reflected uniform and send nothing, and values limited to the reflected array size. Any thread; for level
    /// 1's per-scene lines and the tests.
    struct DeviceCounts
    {
        U64 uniforms = 0;
        U64 samplerChannels = 0;
        U64 refused = 0;
        U64 clamped = 0;
    };
    static DeviceCounts deviceCounts();
    /// [Viewer Lab] One call in the tests' log: kind 'u' a value, 's' a sampler channel, 'r' a refused call; program is the
    /// program's id, uniform its index in the reflection, count the values sent (the caller's for 'r'), bytes what the
    /// encoder got.
    struct DeviceCall
    {
        char kind = 0;
        U64 program = 0;
        U32 uniform = 0;
        U32 count = 0;
        U32 channel = 0;
        std::vector<U8> bytes;
    };
    /// [Viewer Lab] Tests only, on the main thread: every call is appended to the log until it is set to null again; null in
    /// the viewer.
    static void setDeviceCallLog(std::vector<DeviceCall>* log);

    // [Viewer Lab] The GL reflection of a linked GL program, in the interface's backend-neutral format: the
    // default block's uniforms with a location, in the driver's order, named without [0], with each one's location and GL
    // type when asked, and the blocks; and the reserved attributes the program reads. GL code only: the GL backend, and
    // LLGLSLShader without the viewer's device.
    static void reflectGLUniforms(U32 program, const std::string& label, LL::RHI::ProgramReflection& reflection,
                                  std::vector<GLint>* locations = nullptr, std::vector<U32>* types = nullptr);
    static U32  reflectGLAttributes(U32 program, const std::vector<std::string>& reserved); ///< [Viewer Lab]

    void unload();
    void clearStats();
    void dumpStats(boost::json::object& stats);

    // place query objects for profiling if profiling is enabled
    // if for_runtime is true, will place timer query only whether or not profiling is enabled
    void placeProfileQuery(bool for_runtime = false);

    // Readback query objects if profiling is enabled
    // If for_runtime is true, will readback timer query iff query is available
    // Will return false if a query is pending (try again later)
    // If force_read is true, will force an immediate readback (severe performance penalty)
    bool readProfileQuery(bool for_runtime = false, bool force_read = false);

    bool createShader();
    bool attachFragmentObject(std::string object);
    bool attachVertexObject(std::string object);
    void attachObject(GLuint object);
    void attachObjects(GLuint* objects = NULL, S32 count = 0);
    bool mapAttributes();
    bool mapUniforms();
    void mapUniform(const LL::RHI::ProgramReflection& reflection, GLint index); // [Viewer Lab] an index of the reflection
    void uniform1i(U32 index, GLint i);
    void uniform1f(U32 index, GLfloat v);
    void fastUniform1f(U32 index, GLfloat v);
    void uniform2f(U32 index, GLfloat x, GLfloat y);
    void uniform3f(U32 index, GLfloat x, GLfloat y, GLfloat z);
    void uniform4f(U32 index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void uniform1iv(U32 index, U32 count, const GLint* i);
    void uniform4iv(U32 index, U32 count, const GLint* i);
    void uniform1fv(U32 index, U32 count, const GLfloat* v);
    void uniform2fv(U32 index, U32 count, const GLfloat* v);
    void uniform3fv(U32 index, U32 count, const GLfloat* v);
    void uniform4fv(U32 index, U32 count, const GLfloat* v);
    void uniform4uiv(U32 index, U32 count, const GLuint* v);
    void uniform2i(const LLStaticHashedString& uniform, GLint i, GLint j);
    void uniformMatrix2fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v);
    void uniformMatrix3fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v);
    void uniformMatrix3x4fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v);
    void uniformMatrix4fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v);
    void uniform1i(const LLStaticHashedString& uniform, GLint i);
    void uniform1iv(const LLStaticHashedString& uniform, U32 count, const GLint* v);
    void uniform4iv(const LLStaticHashedString& uniform, U32 count, const GLint* v);
    void uniform1f(const LLStaticHashedString& uniform, GLfloat v);
    void uniform2f(const LLStaticHashedString& uniform, GLfloat x, GLfloat y);
    void uniform3f(const LLStaticHashedString& uniform, GLfloat x, GLfloat y, GLfloat z);
    void uniform4f(const LLStaticHashedString& uniform, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void uniform1fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v);
    void uniform2fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v);
    void uniform3fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v);
    void uniform4fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v);
    void uniform4uiv(const LLStaticHashedString& uniform, U32 count, const GLuint* v);
    void uniformMatrix4fv(const LLStaticHashedString& uniform, U32 count, GLboolean transpose, const GLfloat* v);
    void uniformMatrix3fv(const LLStaticHashedString& uniform, U32 count, GLboolean transpose, const GLfloat* v); // <Luminous/>

    void setMinimumAlpha(F32 minimum);

    //GLint getUniformLocation(const std::string& uniform);
    GLint getUniformLocation(const LLStaticHashedString& uniform);
    GLint getUniformLocation(U32 index);

    GLint getAttribLocation(U32 attrib);
    GLint mapUniformTextureChannel(GLint location, GLenum type, GLint size);

    void clearPermutations();
    void addPermutation(std::string name, std::string value);
    void addPermutations(const std::map<std::string, std::string>& defines)
    {
        mDefines.insert(defines.begin(), defines.end());
    }
    void removePermutation(std::string name);

    void addConstant(const LLGLSLShader::eShaderConsts shader_const);

    //enable/disable texture channel for specified uniform
    //if given texture uniform is active in the shader,
    //the corresponding channel will be active upon return
    //returns channel texture is enabled in from [0-MAX)
    S32 enableTexture(S32 uniform, LLTexUnit::eTextureType mode = LLTexUnit::TT_TEXTURE);
    S32 disableTexture(S32 uniform, LLTexUnit::eTextureType mode = LLTexUnit::TT_TEXTURE);

    // get the texture channel of the given uniform, or -1 if uniform is not used as a texture
    // [Viewer Lab] Valid once the reflection is ready (bind publishes first); -1 on a refused or unloaded shader, which has
    // no channels. The other channel functions wait for the reflection themselves.
    S32 getTextureChannel(S32 uniform) const;

    // bindTexture returns the texture unit we've bound the texture to.
    // You can reuse the return value to unbind a texture when required.
    S32 bindTexture(const std::string& uniform, LLTexture* texture, LLTexUnit::eTextureType mode = LLTexUnit::TT_TEXTURE);
    S32 bindTexture(S32 uniform, LLTexture* texture, LLTexUnit::eTextureType mode = LLTexUnit::TT_TEXTURE);
    S32 bindTexture(const std::string& uniform, LLRenderTarget* texture, bool depth = false, LLTexUnit::eTextureFilterOptions mode = LLTexUnit::TFO_BILINEAR);
    S32 bindTexture(S32 uniform, LLRenderTarget* texture, bool depth = false, LLTexUnit::eTextureFilterOptions mode = LLTexUnit::TFO_BILINEAR, U32 index = 0);
    S32 unbindTexture(const std::string& uniform, LLTexUnit::eTextureType mode = LLTexUnit::TT_TEXTURE);
    S32 unbindTexture(S32 uniform, LLTexUnit::eTextureType mode = LLTexUnit::TT_TEXTURE);

    bool link(bool suppress_errors = false);
    // [Viewer Lab] Waits only for 'reflection ready': a pending program is published first, before the flush, the program
    // switch and the uniform update, except while mapUniforms runs. A program that is not complete then is never bound:
    // GL gets program 0, the shader is the selected one (sCurBoundShaderPtr), its uniform update is left, and bind
    // records the refusal for this generation, with one 'Shader' WARN, so the draws skip it (isRefused()). The program
    // switches when the GL name differs, or, for a shader without one, when another shader is selected.
    void bind();
    //helper to conditionally bind mRiggedVariant instead of this
    // [Viewer Lab] A shader that is not complete and has no rigged variant is refused as bind() refuses it
    void bind(bool rigged);
    // [Viewer Lab] For callers that keep the pointer: the rigged variant, or, for a
    // shader that is not complete and has none (its group failed), the shader itself, whose bind refuses as bind(true)
    // does, so the rigged draws are skipped. A complete shader without one gives null, as reading mRiggedVariant did.
    LLGLSLShader* riggedVariant() { return (!mRiggedVariant && !isComplete()) ? this : mRiggedVariant; }

    // [Viewer Lab] made and not failed; on GL the same as mProgramObject != 0 in every state
    bool isComplete() const
    {
        return mProgramState == ProgramState::Pending || mProgramState == ProgramState::Published || mProgramObject != 0;
    }

    // [Viewer Lab] The two stages: reflection ready once the publication has mapped the program,
    // pipeline ready once the viewer's device has it compiled. On GL both hold when createShader returns.
    bool isReflectionReady() const;
    bool isPipelineReady() const;
    void waitForReflection(); ///< [Viewer Lab] publishes a program a backend still compiles, and asserts it is not pending
    void waitForPipeline();   ///< [Viewer Lab]

    // [Viewer Lab] True while bind() has refused this shader, at any generation, and not bound it complete since, so one
    // unloaded or rebuilt without success while current stays refused; what a draw skips by
    bool isRefused() const;
    // [Viewer Lab] What a draw is skipped by: not complete and refused. A shader unloaded or rebuilt while current after
    // bind() bound it complete, or complete again and not bound since its refusal, draws as today; shader_can_draw and
    // LLRender::flush ask this one
    bool drawsRefused() const { return !isComplete() && isRefused(); }
    // [Viewer Lab] The pending programs bind() has published since the process began, for the golden hook and the tests
    static U32 bindPublications();
    // [Viewer Lab] The shaders in Pending now, waiting for their publication: on GL none outside createShader, since it
    // publishes in the same call; for the settling and the tests
    static U32 pendingPrograms() { return sPendingPrograms; }

    // [Viewer Lab] On the main thread: createShader runs it inline; a backend's completion runs it for a Pending program.
    // The generation, raised by every createShader and unload(), drops a stale result.
    bool publish(U32 generation);
    U32  getGeneration() const;

    // [Viewer Lab] The viewer's device's program, null without the device
    LL::RHI::Program* getRHIProgram() const { return mRHIProgram.get(); }
    // [Viewer Lab] The id of the device that made that program, or 0; a program of an earlier device is kept unreleased
    U64 getRHIDevice() const { return mRHIDevice; }

    // [Viewer Lab] Publishes every pending program, for the manifest writer
    static void finishBuilds();

    LLUUID hash();

    // Unbinds any previously bound shader by explicitly binding no shader.
    static void unbind();

    U32 mMatHash[LLRender::NUM_MATRIX_MODES];
    U32 mLightHash;

    GLuint mProgramObject;
#if LL_RELEASE_WITH_DEBUG_INFO
    struct attr_name
    {
        GLint loc;
        const char* name;
        void operator = (GLint _loc) { loc = _loc; }
        operator GLint () { return loc; }
    };
    std::vector<attr_name> mAttribute; //lookup table of attribute enum to attribute channel
#else
    std::vector<GLint> mAttribute; //lookup table of attribute enum to attribute channel
#endif
    U32 mAttributeMask;  //mask of which reserved attributes are set (lines up with LLVertexBuffer::getTypeMask())
    std::vector<GLint> mUniform;   //lookup table of uniform enum to uniform location
    LLStaticStringTable<GLint> mUniformMap; //lookup map of uniform name to uniform location
    typedef std::unordered_map<GLint, LLVector4> uniform_value_map_t;
    uniform_value_map_t mValue; //lookup map of uniform location to last known value
    std::vector<GLint> mTexture;
    S32 mTotalUniformSize;
    S32 mActiveTextureChannels;
    S32 mShaderLevel;
    S32 mShaderGroup; // see LLGLSLShader::eGroup
    bool mUniformsDirty;
    LLShaderFeatures mFeatures;
    std::vector< std::pair< std::string, GLenum > > mShaderFiles;
    std::string mName;
    typedef std::map<std::string, std::string> defines_map_t; //NOTE: this must be an ordered map to maintain hash consistency
    defines_map_t mDefines;
    static defines_map_t sGlobalDefines;
    LLUUID mShaderHash;
    LLUUID mSourceHash; ///< [Viewer Lab] digest of the GLSL the program is built from, which hash() includes
    bool mUsingBinaryProgram = false;
    /// [Viewer Lab] needed at the login screen, compiled first at startup by a backend that compiles in the background;
    /// set by the program's owner
    bool mLoginScreen = false;

    //statistics for profiling shader performance
    bool mProfilePending = false;
    LLGPUQuery mTimerQuery; // [Viewer Lab]
    LLGPUQuery mSamplesQuery;
    LLGPUQuery mPrimitivesQuery;

    U64 mTimeElapsed;
    static U64 sTotalTimeElapsed;
    U32 mTrianglesDrawn;
    static U32 sTotalTrianglesDrawn;
    U64 mSamplesDrawn;
    static U64 sTotalSamplesDrawn;
    U32 mBinds;
    static U32 sTotalBinds;

    // this pointer should be set to whichever shader represents this shader's rigged variant
    LLGLSLShader* mRiggedVariant = nullptr;

    // variants for use by GLTF renderer
    // bit 0 = alpha mode blend (1) or opaque (0)
    // bit 1 = rigged (1) or static (0)
    // bit 2 = unlit (1) or lit (0)
    // bit 3 = single (0) or multi (1) uv coordinates
    struct GLTFVariant
    {
        constexpr static U8 ALPHA_BLEND = 1;
        constexpr static U8 RIGGED = 2;
        constexpr static U8 UNLIT = 4;
        constexpr static U8 MULTI_UV = 8;
    };

    constexpr static U8 NUM_GLTF_VARIANTS = 16;

    std::vector<LLGLSLShader> mGLTFVariants;

    //helper to bind GLTF variant
    // [Viewer Lab] A shader that is not complete and has no GLTF variants is refused as bind() refuses it
    void bind(U8 variant);

    // hacky flag used for optimization in LLDrawPoolAlpha
    bool mCanBindFast = false;

#if LL_PROFILER_ENABLE_RENDER_DOC
    void setLabel(const char* label);
#endif

private:
    void unloadInternal();

    // [Viewer Lab] The program's build
    enum class ProgramState : U8
    {
        Unloaded,
        Pending,   ///< built, waiting for the publication
        Published, ///< reflection ready
        Failed,
    };
    /// [Viewer Lab] mProgramState's value. The shader's copy and assignment are defaulted and copy it; a
    /// shader in Pending on either side would miscount sPendingPrograms, so neither may be.
    struct ProgramStateValue
    {
        ProgramState value = ProgramState::Unloaded;

        ProgramStateValue() = default;
        ProgramStateValue(const ProgramStateValue& other) : value(other.value)
        {
            llassert(other.value != ProgramState::Pending);
        }
        ProgramStateValue& operator=(const ProgramStateValue& other)
        {
            llassert(value != ProgramState::Pending && other.value != ProgramState::Pending);
            value = other.value;
            return *this;
        }
        operator ProgramState() const { return value; }
    };
    ProgramStateValue           mProgramState; ///< written only by setProgramState, and copied when not Pending
    /// [Viewer Lab] Every write of mProgramState, which keeps the count of shaders in Pending
    void setProgramState(ProgramState state);
    static U32 sPendingPrograms; ///< the shaders in Pending
    U32                         mGeneration   = 0;     ///< publish drops another generation's result
    bool                        mBuilt        = false; ///< createShader compiled and attached every unit
    S32                         mAskedLevel   = 0;     ///< the shader level asked for, before any fall-back
    bool                        mFallingBack  = false; ///< publish's retry at a lower level: createShader keeps mAskedLevel
    LLPointer<LL::RHI::Program> mRHIProgram;           ///< the viewer's device's program, adopting mProgramObject on GL
    U64                         mRHIDevice = 0;        ///< the id of the device that made it

    // [Viewer Lab] The reflection's lookups
    std::unordered_map<GLint, U32> mReflectionIndex; ///< a uniform's location -> its index in the reflection
    bool mMapping = false; ///< mapUniforms runs: bind()'s uniform update belongs to the publication
    void waitForUniforms() { if (!mMapping) waitForReflection(); } ///< reflection ready, except while mapUniforms runs
    /// The setters' GL call, today's, then with the viewer's device the value or sampler channel for the encoder
    void sendUniform(GLUniformCall call, GLint location, GLsizei count, GLboolean transpose, const void* values);

    // This must be static because finishProfile() is called at least once
    // within a __try block. If we default its stats parameter to a temporary
    // json::value, that temporary must be destroyed when the stack is
    // unwound, which __try forbids.
    static boost::json::value sDefaultStats;
};

// [Viewer Lab] A uniform buffer of the viewer's render device for one of LLGLSLShader's named blocks.
// bind() binds it to the block's slot, the UniformBlock, which GL reads at LLGLSLShader::INTERFACE_BLOCK_BINDING +
// block. While at least one exists for a block, every LLGLSLShader program reads that block there, whatever its index in
// the program's reflection; otherwise at its UB_* point, where raw GL's glBindBufferBase binds. So a block is never read
// both raw and through the facade, and the first allocate of an undeclared block and the release of its last buffer rebind
// that block in every mapped program. A declared block (declareBlock) is read at INTERFACE_BLOCK_BINDING + block by
// every program from its load, for the rest of the process, so its first allocate and last release rebind nothing. Declare
// a block only in the change that removed every raw bind of it, before the first shader load (a static initializer or a
// global's constructor); a later declaration costs one sweep of the mapped programs and one RHI WARN. Undeclared blocks
// keep the count and sweep. Updates have GL semantics: copied at the call, for all later draws. On GL a
// bind of an unchanged binding within one pass makes no GL call; within a pass points 8 and up are the GL encoder's alone.
// Main thread only; without the device (headless) allocate is false and the rest does nothing. Facades owned by
// singletons are released in an explicit cleanup: one that goes after its device releases nothing and makes no GL call.
class LLShaderUniformBuffer
{
public:
    LLShaderUniformBuffer(); // defined where LL::RHI::Buffer is complete, as the destructor
    ~LLShaderUniformBuffer();
    LLShaderUniformBuffer(const LLShaderUniformBuffer&)            = delete;
    LLShaderUniformBuffer& operator=(const LLShaderUniformBuffer&) = delete;

    /// Every LLGLSLShader program reads the block at LLGLSLShader::INTERFACE_BLOCK_BINDING + block from its load, buffer or
    /// not, for the rest of the process, and no allocate or release of it rebinds anything. Before the viewer's
    /// device exists it only sets a bit, so a static initializer may call it; with the device on the main thread, while
    /// mapped programs live and the block has no buffer, it binds the block again in them with one RHI WARN. Nothing for a
    /// block past the four or one already declared. Main thread or static initialization only.
    static void declareBlock(LLGLSLShader::UniformBlock block);
    /// For the tests, which share one process: no block declared any more, and each formerly declared block without a buffer
    /// bound again at its UB_* point in every mapped program.
    static void forgetDeclaredBlocksForTest();
    /// The declared blocks, bit UniformBlock each, and the sweeps since the process began: the times a block was bound
    /// again in the mapped programs, at an undeclared block's first allocate or last release or a late declaration. For
    /// the golden hook and the tests.
    static U32 declaredBlocks();
    static U32 programSweeps();

    /// A new buffer of size bytes for the block, its debug name the block's GLSL name unless one is given. False without
    /// the device, off the main thread, for size 0, for a block past the four or when the device refuses; false leaves the
    /// facade as it was. An allocated facade lets go of its old buffer and keeps its block's claim when the block is the
    /// same.
    bool allocate(LLGLSLShader::UniformBlock block, U32 size, bool dynamic, const char* name = nullptr);
    void update(U32 offset, const void* data, U32 size); ///< through the encoder; nothing for size 0
    /// To the block's slot, the stack's pass begun first (LLRenderTarget::beginStackPass): on GL at once in the pass, and
    /// when no pass can begin, as soon as an ordinary pass begins.
    void bind() const;
    /// As LLGLSLShader lets go of its program: kept unreleased after its device. Main thread only (an llassert): elsewhere
    /// the buffer is kept unreleased and the block keeps its claim.
    void release();
    bool isAllocated() const;
    LL::RHI::Buffer* getRHIBuffer() const; ///< for the tests

private:
    LLPointer<LL::RHI::Buffer> mBuffer;
    U64 mDevice = 0; ///< the id of the device that made it
    LLGLSLShader::UniformBlock mBlock = LLGLSLShader::NUM_UNIFORM_BLOCKS;
    U32 mSize = 0;
};

//UI shader (declared here so llui_libtest will link properly)
extern LLGLSLShader         gUIProgram;
//output vec4(color.rgb,color.a*tex0[tc0].a)
extern LLGLSLShader         gSolidColorProgram;
//Alpha mask shader (declared here so llappearance can access properly)
extern LLGLSLShader         gAlphaMaskProgram;

#if LL_PROFILER_ENABLE_RENDER_DOC
#define LL_SET_SHADER_LABEL(shader) shader.setLabel(#shader)
#else
#define LL_SET_SHADER_LABEL(shader, label)
#endif

#endif
