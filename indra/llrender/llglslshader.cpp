/**
 * @file llglslshader.cpp
 * @brief GLSL helper functions and state.
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
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

#include "llglslshader.h"

#include "llshadermgr.h"
#include "llfile.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llrendertarget.h"
#include "llrhi.h" // [Viewer Lab]

#include "hbxxh.h"
#include "llsdserialize.h"

// [Viewer Lab]
#include <atomic>
#include <mutex> // [Viewer Lab]
#include <set>
#include <span>
#include <tuple>
#include <unordered_set>

#if LL_DARWIN
#include "OpenGL/OpenGL.h"
#endif

 // Print-print list of shader included source files that are linked together via glAttachShader()
 // i.e. On macOS / OSX the AMD GLSL linker will display an error if a varying is left in an undefined state.
#define DEBUG_SHADER_INCLUDES 0

// Lots of STL stuff in here, using namespace std to keep things more readable
using std::vector;
using std::pair;
using std::make_pair;
using std::string;

GLuint LLGLSLShader::sCurBoundShader = 0;
LLGLSLShader* LLGLSLShader::sCurBoundShaderPtr = NULL;
S32 LLGLSLShader::sIndexedTextureChannels = 0;
U32 LLGLSLShader::sMaxGLTFMaterials = 0;
U32 LLGLSLShader::sMaxGLTFNodes = 0;
bool LLGLSLShader::sProfileEnabled = false;
bool LLGLSLShader::sCanProfile = true;
std::set<LLGLSLShader*> LLGLSLShader::sInstances;
LLGLSLShader::defines_map_t LLGLSLShader::sGlobalDefines;
U64 LLGLSLShader::sTotalTimeElapsed = 0;
U32 LLGLSLShader::sTotalTrianglesDrawn = 0;
U64 LLGLSLShader::sTotalSamplesDrawn = 0;
U32 LLGLSLShader::sTotalBinds = 0;
boost::json::value LLGLSLShader::sDefaultStats;

// [Viewer Lab] The shader class lines, held from the process start until writeClassLines(): one log write per
// program cost the GL startup 1.66 ms for 205 programs before STATE_STARTED (measured with a startup A/B), so publish keeps
// the name and the two levels, and the lines are written together once the startup is done.
namespace
{
struct ShaderClassLine
{
    std::string name;
    S32 level;
    S32 asked;
};
std::mutex sClassLinesMutex;
std::vector<ShaderClassLine> sClassLines;
bool sClassLinesHeld = true;
// The lines held or written in this session, so a program relinked at the class it had (every change of graphics
// level relinks all of them) does not log its line again.
std::unordered_set<std::string> sClassLinesSeen;

void log_shader_class(const std::string& name, S32 level, S32 asked)
{
    LL_INFOS("Shader") << "Program '" << name << "': shader class " << level << " (asked " << asked << ")" << LL_ENDL;
}
}

// [Viewer Lab]
//static
void LLGLSLShader::writeClassLines()
{
    std::vector<ShaderClassLine> lines;
    {
        std::lock_guard<std::mutex> lock(sClassLinesMutex);
        sClassLinesHeld = false;
        lines.swap(sClassLines);
    }
    for (const ShaderClassLine& line : lines)
    {
        log_shader_class(line.name, line.level, line.asked);
    }
}

//UI shader -- declared here so llui_libtest will link properly
LLGLSLShader    gUIProgram;
LLGLSLShader    gSolidColorProgram;

// NOTE: Keep gShaderConsts* and LLGLSLShader::ShaderConsts_e in sync!
const std::string gShaderConstsKey[LLGLSLShader::NUM_SHADER_CONSTS] =
{
      "LL_SHADER_CONST_CLOUD_MOON_DEPTH"
    , "LL_SHADER_CONST_STAR_DEPTH"
};

// NOTE: Keep gShaderConsts* and LLGLSLShader::ShaderConsts_e in sync!
const std::string gShaderConstsVal[LLGLSLShader::NUM_SHADER_CONSTS] =
{
      "0.99998" // SHADER_CONST_CLOUD_MOON_DEPTH // SL-14113
    , "0.99999" // SHADER_CONST_STAR_DEPTH       // SL-14113
};


bool shouldChange(const LLVector4& v1, const LLVector4& v2)
{
    return v1 != v2;
}

// [Viewer Lab] LLGLSLShader's GL backend functions. Each holds today's GL calls and
// does nothing, or gives the neutral value, when GL is not the backend chosen at startup, as llimagegl.cpp's.
static bool gl_is_backend()
{
    return LL::RHI::viewerBackend() == LL::RHI::BackendType::GL;
}

// createShader's program. On another backend there is no GL program, which is not a failure.
static bool gl_create_program(GLuint& program)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_create_program");
        program = 0;
        return true;
    }
    program = glCreateProgram();
    return program != 0;
}

// The attach of attachVertexObject, attachFragmentObject and attachObject.
static void gl_attach_shader(GLuint program, GLuint shader)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_attach_shader");
        return;
    }
    glAttachShader(program, shader);
}

// unloadInternal's deletion of the program and of every object attached to it, shared ones too.
static void gl_delete_program(GLuint program)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_delete_program");
        return;
    }
    GLuint obj[1024];
    GLsizei count = 0;
    glGetAttachedShaders(program, 1024, &count, obj);

    for (GLsizei i = 0; i < count; i++)
    {
        glDetachShader(program, obj[i]);
    }

    for (GLsizei i = 0; i < count; i++)
    {
        if (glIsShader(obj[i]))
        {
            glDeleteShader(obj[i]);
        }
    }

    glDeleteProgram(program);
}

// bind's and unbind's glUseProgram. On another backend bind only selects the shader: the program reaches the
// encoder in the pipeline at draw time.
static void gl_use_program(GLuint program)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_use_program");
        return;
    }
    glUseProgram(program);
}

// bind's enables of the program's vertex arrays, through LLVertexBuffer's GL backend function.
static void gl_setup_client_arrays(U32 mask)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_setup_client_arrays");
        return;
    }
    LLVertexBuffer::setupClientArrays(mask);
}

// mapAttributes' binding of the reserved attributes and the link. On another backend the backend linked the
// program, which is not a failure. No setter or lookup may run here or in the link: mMapping covers only mapUniforms, so
// their wait would publish the program again.
static bool gl_bind_attributes_and_link(LLGLSLShader& shader)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_bind_attributes_and_link");
        return true;
    }
    //before linking, make sure reserved attributes always have consistent locations
    for (U32 i = 0; i < LLShaderMgr::instance()->mReservedAttribs.size(); i++)
    {
        const char* name = LLShaderMgr::instance()->mReservedAttribs[i].c_str();
        glBindAttribLocation(shader.mProgramObject, i, (const GLchar*)name);
    }

    //link the program
    return shader.link();
}

// mapAttributes' location of a reserved attribute in the reflection's mask; the fallback, the attribute's index, on
// another backend.
static GLint gl_attrib_location(GLuint program, const char* name, GLint fallback)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_attrib_location");
        return fallback;
    }
    return glGetAttribLocation(program, (const GLchar*)name);
}

// The location of a uniform by name, for mapUniform, mapUniforms' texture names and getUniformLocation's gDebugGL check;
// the fallback, the uniform's index in the reflection, on another backend.
static GLint gl_uniform_location(GLuint program, const char* name, GLint fallback)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_uniform_location");
        return fallback;
    }
    return glGetUniformLocation(program, name);
}

// Set up block binding, in a way supported by Apple (rather than binding = 1 in .glsl).
// See slide 35 and more of https://docs.huihoo.com/apple/wwdc/2011/session_420__advances_in_opengl_for_mac_os_x_lion.pdf
static const char* const ubo_names[] =
{
    "ReflectionProbes", // UB_REFLECTION_PROBES
    "GLTFJoints",       // UB_GLTF_JOINTS
    "GLTFNodes",        // UB_GLTF_NODES
    "GLTFMaterials",    // UB_GLTF_MATERIALS
};

// [Viewer Lab]
// static
const char* LLGLSLShader::uniformBlockName(UniformBlock block)
{
    return block < NUM_UNIFORM_BLOCKS ? ubo_names[block] : nullptr;
}

// [Viewer Lab]
// static
std::span<const char* const> LLGLSLShader::uniformBlockNames()
{
    static_assert(LL_ARRAY_SIZE(ubo_names) == NUM_UNIFORM_BLOCKS);
    return ubo_names;
}

// The binding of the named blocks whose bit is in blocks, for mapUniforms (all four) and rebind_interface_block (the one
// whose claim began or ended): to their UB_* points, or, for a block in interface_mask, to the render interface's point for
// it; nothing on another backend. With all four blocks and mask 0, today's calls.
static void gl_bind_uniform_blocks(GLuint program, U32 blocks, U32 interface_mask)
{
    llassert(LL_ARRAY_SIZE(ubo_names) == LLGLSLShader::NUM_UNIFORM_BLOCKS);

    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_bind_uniform_blocks");
        return;
    }
    for (U32 i = 0; i < LLGLSLShader::NUM_UNIFORM_BLOCKS; ++i)
    {
        if (!(blocks & (1u << i)))
        {
            continue;
        }
        GLuint UBOBlockIndex = glGetUniformBlockIndex(program, ubo_names[i]);
        if (UBOBlockIndex != GL_INVALID_INDEX)
        {
            glUniformBlockBinding(program, UBOBlockIndex,
                                  (interface_mask & (1u << i)) ? LLGLSLShader::INTERFACE_BLOCK_BINDING + i : i);
        }
    }
}

// [Viewer Lab] The LLShaderUniformBuffers of each named block, main thread only: while a block has one or is declared,
// every program reads it at the render interface's point for it, otherwise at its UB_* point for raw GL. The
// switch stays: facade and interface share the GL context for good.
static U32 sInterfaceBlockBuffers[LLGLSLShader::NUM_UNIFORM_BLOCKS] = {};

// [Viewer Lab] The blocks LLShaderUniformBuffer::declareBlock declared, a bit each, for the rest of the process. Constant-
// initialized, so a declaration from a static initializer finds it; main thread or static initialization only.
static U32 sDeclaredBlocks = 0;

// [Viewer Lab] The sweeps rebind_interface_block made since the process began: calls that bound a block again in at least
// one mapped program, main thread only; the golden hook's measured.rhi.uniform_blocks
static U32 sProgramSweeps = 0;

// [Viewer Lab] The mask of the blocks that have an LLShaderUniformBuffer or are declared
static U32 interface_blocks()
{
    U32 mask = sDeclaredBlocks;
    for (U32 i = 0; i < LLGLSLShader::NUM_UNIFORM_BLOCKS; ++i)
    {
        if (sInterfaceBlockBuffers[i] > 0)
        {
            mask |= 1u << i;
        }
    }
    return mask;
}

// [Viewer Lab] When an undeclared block's first LLShaderUniformBuffer comes or its last goes, and at a late declaration: that
// one block bound again in every live program mapUniforms has mapped, with the viewer's device on the main thread; a program
// mapped later binds it in mapUniforms. No rebind without the device, so a facade that goes in static destruction makes no GL
// call.
static void rebind_interface_block(LLGLSLShader::UniformBlock block)
{
    if (!LL::RHI::viewerDevice() || !on_main_thread())
    {
        return;
    }
    const U32 mask = interface_blocks();
    bool swept = false;
    for (LLGLSLShader* shader : LLGLSLShader::sInstances)
    {
        if (shader->mProgramObject && shader->isReflectionReady())
        {
            gl_bind_uniform_blocks(shader->mProgramObject, 1u << block, mask);
            swept = true;
        }
    }
    sProgramSweeps += swept ? 1 : 0;
}

// The GL reflection of LLGLSLShader's program without the viewer's device; empty on another backend.
static void gl_reflect_uniforms(GLuint program, const std::string& label, LL::RHI::ProgramReflection& reflection)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_reflect_uniforms");
        return;
    }
    LLGLSLShader::reflectGLUniforms(program, label, reflection);
}

// The reflection's attribute mask of LLGLSLShader's program without the viewer's device; 0 on another backend.
static U32 gl_reflected_attribute_mask(GLuint program, const std::vector<std::string>& reserved)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_reflected_attribute_mask");
        return 0;
    }
    return LLGLSLShader::reflectGLAttributes(program, reserved);
}

// [Viewer Lab] The GL entry point each uniform setter calls today, which gl_uniform makes
enum class LLGLSLShader::GLUniformCall : U8
{
    Uniform1i,
    Uniform2i,
    Uniform1iv,
    Uniform4iv,
    Uniform4uiv,
    Uniform1f,
    Uniform2f,
    Uniform3f,
    Uniform4f,
    Uniform1fv,
    Uniform2fv,
    Uniform3fv,
    Uniform4fv,
    Matrix2fv,
    Matrix3fv,
    Matrix3x4fv,
    Matrix4fv,
};

// The uniform setters' and fastUniform1f's GL call, after their value cache: today's entry point, chosen by the setter (the
// index uniform4iv's glUniform1iv included), with the setter's location, count, transpose and values; nothing on another
// backend, where the encoder sets the value.
static void gl_uniform(LLGLSLShader::GLUniformCall call, GLint location, GLsizei count, GLboolean transpose, const void* values)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llglslshader:gl_uniform");
        return;
    }
    const GLint*   i = static_cast<const GLint*>(values);
    const GLuint*  u = static_cast<const GLuint*>(values);
    const GLfloat* f = static_cast<const GLfloat*>(values);
    using Call = LLGLSLShader::GLUniformCall;
    switch (call)
    {
    case Call::Uniform1i:   glUniform1i(location, i[0]); break;
    case Call::Uniform2i:   glUniform2i(location, i[0], i[1]); break;
    case Call::Uniform1iv:  glUniform1iv(location, count, i); break;
    case Call::Uniform4iv:  glUniform4iv(location, count, i); break;
    case Call::Uniform4uiv: glUniform4uiv(location, count, u); break;
    case Call::Uniform1f:   glUniform1f(location, f[0]); break;
    case Call::Uniform2f:   glUniform2f(location, f[0], f[1]); break;
    case Call::Uniform3f:   glUniform3f(location, f[0], f[1], f[2]); break;
    case Call::Uniform4f:   glUniform4f(location, f[0], f[1], f[2], f[3]); break;
    case Call::Uniform1fv:  glUniform1fv(location, count, f); break;
    case Call::Uniform2fv:  glUniform2fv(location, count, f); break;
    case Call::Uniform3fv:  glUniform3fv(location, count, f); break;
    case Call::Uniform4fv:  glUniform4fv(location, count, f); break;
    case Call::Matrix2fv:   glUniformMatrix2fv(location, count, transpose, f); break;
    case Call::Matrix3fv:   glUniformMatrix3fv(location, count, transpose, f); break;
    case Call::Matrix3x4fv: glUniformMatrix3x4fv(location, count, transpose, f); break;
    case Call::Matrix4fv:   glUniformMatrix4fv(location, count, transpose, f); break;
    }
}

// Whether a name is one of the samplers mapUniforms' diffuseMap rule searches for and compares.
static bool is_diffuse_rule_sampler(const char* name)
{
    for (const char* rule : { "diffuseMap", "specularMap", "bumpMap", "environmentMap", "altDiffuseMap" })
    {
        if (strcmp(name, rule) == 0)
        {
            return true;
        }
    }
    return false;
}

// The index of a uniform in a reflection, or -1.
static GLint reflected_index(const LL::RHI::ProgramReflection& reflection, const char* name)
{
    for (size_t i = 0; i < reflection.uniforms.size(); ++i)
    {
        if (reflection.uniforms[i].name == name)
        {
            return GLint(i);
        }
    }
    return -1;
}

//===============================
// LLGLSL Shader implementation
//===============================

//static
void LLGLSLShader::initProfile()
{
    sProfileEnabled = true;
    sTotalTimeElapsed = 0;
    sTotalTrianglesDrawn = 0;
    sTotalSamplesDrawn = 0;
    sTotalBinds = 0;

    for (auto ptr : sInstances)
    {
        ptr->clearStats();
    }
}


struct LLGLSLShaderCompareTimeElapsed
{
    bool operator()(const LLGLSLShader* const& lhs, const LLGLSLShader* const& rhs)
    {
        return lhs->mTimeElapsed < rhs->mTimeElapsed;
    }
};

//static
void LLGLSLShader::finishProfile(boost::json::value& statsv)
{
    sProfileEnabled = false;

    if (! statsv.is_null())
    {
        std::vector<LLGLSLShader*> sorted(sInstances.begin(), sInstances.end());
        std::sort(sorted.begin(), sorted.end(), LLGLSLShaderCompareTimeElapsed());

        auto& stats = statsv.as_object();
        auto shadersit = stats.emplace("shaders", boost::json::array_kind).first;
        auto& shaders = shadersit->value().as_array();
        bool unbound = false;
        for (auto ptr : sorted)
        {
            if (ptr->mBinds == 0)
            {
                unbound = true;
            }
            else
            {
                auto& shaderit = shaders.emplace_back(boost::json::object_kind);
                ptr->dumpStats(shaderit.as_object());
            }
        }

        constexpr float mega = 1'000'000.f;
        float totalTimeMs = sTotalTimeElapsed / mega;
        LL_INFOS() << "-----------------------------------" << LL_ENDL;
        LL_INFOS() << "Total rendering time: " << llformat("%.4f ms", totalTimeMs) << LL_ENDL;
        LL_INFOS() << "Total samples drawn: " << llformat("%.4f million", sTotalSamplesDrawn / mega) << LL_ENDL;
        LL_INFOS() << "Total triangles drawn: " << llformat("%.3f million", sTotalTrianglesDrawn / mega) << LL_ENDL;
        LL_INFOS() << "-----------------------------------" << LL_ENDL;
        auto totalsit = stats.emplace("totals", boost::json::object_kind).first;
        auto& totals = totalsit->value().as_object();
        totals.emplace("time", totalTimeMs / 1000.0);
        totals.emplace("binds", sTotalBinds);
        totals.emplace("samples", sTotalSamplesDrawn);
        totals.emplace("triangles", sTotalTrianglesDrawn);

        auto unusedit = stats.emplace("unused", boost::json::array_kind).first;
        auto& unused = unusedit->value().as_array();
        if (unbound)
        {
            LL_INFOS() << "The following shaders were unused: " << LL_ENDL;
            for (auto ptr : sorted)
            {
                if (ptr->mBinds == 0)
                {
                    LL_INFOS() << ptr->mName << LL_ENDL;
                    unused.emplace_back(ptr->mName);
                }
            }
        }
    }
}

void LLGLSLShader::clearStats()
{
    mTrianglesDrawn = 0;
    mTimeElapsed = 0;
    mSamplesDrawn = 0;
    mBinds = 0;
}

void LLGLSLShader::dumpStats(boost::json::object& stats)
{
    stats.emplace("name", mName);
    auto filesit = stats.emplace("files", boost::json::array_kind).first;
    auto& files = filesit->value().as_array();
    LL_INFOS() << "=============================================" << LL_ENDL;
    LL_INFOS() << mName << LL_ENDL;
    for (U32 i = 0; i < mShaderFiles.size(); ++i)
    {
        LL_INFOS() << mShaderFiles[i].first << LL_ENDL;
        files.emplace_back(mShaderFiles[i].first);
    }
    LL_INFOS() << "=============================================" << LL_ENDL;

    constexpr float  mega = 1'000'000.f;
    constexpr double giga = 1'000'000'000.0;
    F32 ms = mTimeElapsed / mega;
    F32 seconds = ms / 1000.f;

    F32 pct_tris = (F32)mTrianglesDrawn / (F32)sTotalTrianglesDrawn * 100.f;
    F32 tris_sec = (F32)(mTrianglesDrawn / mega);
    tris_sec /= seconds;

    F32 pct_samples = (F32)((F64)mSamplesDrawn / (F64)sTotalSamplesDrawn) * 100.f;
    F32 samples_sec = (F32)(mSamplesDrawn / giga);
    samples_sec /= seconds;

    F32 pct_binds = (F32)mBinds / (F32)sTotalBinds * 100.f;

    LL_INFOS() << "Triangles Drawn: " << mTrianglesDrawn << " " << llformat("(%.2f pct of total, %.3f million/sec)", pct_tris, tris_sec) << LL_ENDL;
    LL_INFOS() << "Binds: " << mBinds << " " << llformat("(%.2f pct of total)", pct_binds) << LL_ENDL;
    LL_INFOS() << "SamplesDrawn: " << mSamplesDrawn << " " << llformat("(%.2f pct of total, %.3f billion/sec)", pct_samples, samples_sec) << LL_ENDL;
    LL_INFOS() << "Time Elapsed: " << mTimeElapsed << " " << llformat("(%.2f pct of total, %.5f ms)\n", (F32)((F64)mTimeElapsed / (F64)sTotalTimeElapsed) * 100.f, ms) << LL_ENDL;
    stats.emplace("time", seconds);
    stats.emplace("binds", mBinds);
    stats.emplace("samples", mSamplesDrawn);
    stats.emplace("triangles", mTrianglesDrawn);
}

//static
void LLGLSLShader::startProfile()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    if (sProfileEnabled && sCurBoundShaderPtr)
    {
        sCurBoundShaderPtr->placeProfileQuery();
    }

}

//static
void LLGLSLShader::stopProfile()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (sProfileEnabled && sCurBoundShaderPtr)
    {
        sCurBoundShaderPtr->unbind();
    }
}

void LLGLSLShader::placeProfileQuery(bool for_runtime)
{
    if (sProfileEnabled || for_runtime)
    {
        // [Viewer Lab] Through LLGPUQuery, as made and begun today. The samples query exists whenever the device
        // does, so the three are not made again at every bind without timer queries.
        if (!mSamplesQuery)
        {
            mSamplesQuery = LLGPUQuery::create(GL_SAMPLES_PASSED);
            mTimerQuery = LLGPUQuery::create(GL_TIME_ELAPSED);
            mPrimitivesQuery = LLGPUQuery::create(GL_PRIMITIVES_GENERATED);
        }

        mTimerQuery.begin(GL_TIME_ELAPSED);

        if (!for_runtime)
        {
            mSamplesQuery.begin(GL_SAMPLES_PASSED);
            mPrimitivesQuery.begin(GL_PRIMITIVES_GENERATED);
        }
    }
}

bool LLGLSLShader::readProfileQuery(bool for_runtime, bool force_read)
{
    if ((sProfileEnabled || for_runtime) && sCanProfile)
    {
        // [Viewer Lab] Through LLGPUQuery, as ended and read today
        if (!mProfilePending)
        {
            mTimerQuery.end(GL_TIME_ELAPSED);
            if (!for_runtime)
            {
                mSamplesQuery.end(GL_SAMPLES_PASSED);
                mPrimitivesQuery.end(GL_PRIMITIVES_GENERATED);
            }
            mProfilePending = for_runtime;
        }

        if (mProfilePending && for_runtime && !force_read)
        {
            GLuint64 result = mTimerQuery.get(GL_QUERY_RESULT_AVAILABLE);

            if (result != GL_TRUE)
            {
                return false;
            }
        }

        GLuint64 time_elapsed = mTimerQuery.get(GL_QUERY_RESULT);
        mTimeElapsed += time_elapsed;
        mProfilePending = false;

        if (!for_runtime)
        {
            GLuint64 samples_passed = mSamplesQuery.get(GL_QUERY_RESULT);

            GLuint64 primitives_generated = mPrimitivesQuery.get(GL_QUERY_RESULT);
            sTotalTimeElapsed += time_elapsed;

            sTotalSamplesDrawn += samples_passed;
            mSamplesDrawn += samples_passed;

            U32 tri_count = (U32)primitives_generated / 3;

            mTrianglesDrawn += tri_count;
            sTotalTrianglesDrawn += tri_count;

            sTotalBinds++;
            mBinds++;
        }
    }

    return true;
}



// [Viewer Lab] The GL program the viewer's device adopts while an OwnGLProgramScope names it, on this thread only, as
// llimagegl.cpp's sCreatingOwnTexture
static thread_local U32 sOwnGLProgram = 0;

LLGLSLShader::OwnGLProgramScope::OwnGLProgramScope(U32 program)
:   mPrevious(sOwnGLProgram)
{
    sOwnGLProgram = program;
}

LLGLSLShader::OwnGLProgramScope::~OwnGLProgramScope()
{
    sOwnGLProgram = mPrevious; // [Viewer Lab] the previous program, not 0
}

// static
U32 LLGLSLShader::ownGLProgram()
{
    return sOwnGLProgram;
}

// [Viewer Lab] The viewer's device's program for createShader's GL program: none, which is no failure, without the
// device (headless, llappearanceutility, the tests' fixture) or off the main thread, where LLGLSLShader keeps only its GL
// program; the branch on the device that LLGPUZone makes, never on the backend. On GL the device adopts the GL program while
// the scope names it. False only when the device refuses the description, which is a failed glCreateProgram.
static bool create_rhi_program(const LL::RHI::ProgramDesc& desc, GLuint gl_program, LLPointer<LL::RHI::Program>& program, U64& device_id)
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || !on_main_thread())
    {
        return true;
    }
    {
        LLGLSLShader::OwnGLProgramScope scope(gl_program);
        program = device->createProgram(desc);
    }
    device_id = device->id();
    return program.notNull();
}

// [Viewer Lab] Lets go of the program on the main thread while the device that made it is the viewer's; otherwise it is
// kept, never released, so no render interface object goes in static destruction or to a device that is gone (as
// llimagegl.cpp's keep_unreleased)
static void release_rhi_program(LLPointer<LL::RHI::Program>& program, U64& device_id)
{
    if (program.notNull())
    {
        LL::RHI::Device* device = LL::RHI::viewerDevice();
        if (!on_main_thread() || !device || device->id() != device_id)
        {
            program->ref();
        }
        program = nullptr;
    }
    device_id = 0;
}

LLGLSLShader::LLGLSLShader()
    : mProgramObject(0),
    mAttributeMask(0),
    mTotalUniformSize(0),
    mActiveTextureChannels(0),
    mShaderLevel(0),
    mShaderGroup(SG_DEFAULT),
    mFeatures(),
    mUniformsDirty(false)
{

}

// [Viewer Lab] Here, where LL::RHI::Program is complete; a copy shares the program, as it shares mProgramObject
LLGLSLShader::LLGLSLShader(const LLGLSLShader&)            = default;
LLGLSLShader& LLGLSLShader::operator=(const LLGLSLShader&) = default;

LLGLSLShader::~LLGLSLShader()
{
    release_rhi_program(mRHIProgram, mRHIDevice); // [Viewer Lab] no GL call, as before
    setProgramState(ProgramState::Unloaded);      // [Viewer Lab] a pending shader is counted no more
    mTimerQuery.keep(); // [Viewer Lab] no GL call, as before
    mSamplesQuery.keep();
    mPrimitivesQuery.keep();
}

void LLGLSLShader::unload()
{
    mShaderFiles.clear();
    mDefines.clear();
    mFeatures = LLShaderFeatures();

    ++mGeneration; // [Viewer Lab] a build in progress is cancelled
    unloadInternal();
}

void LLGLSLShader::unloadInternal()
{
    sInstances.erase(this);

    stop_glerror();
    mAttribute.clear();
    mTexture.clear();
    mUniform.clear();

    // [Viewer Lab] The device forgets the program before GL deletes the name
    release_rhi_program(mRHIProgram, mRHIDevice);
    setProgramState(ProgramState::Unloaded);

    if (mProgramObject)
    {
        gl_delete_program(mProgramObject); // [Viewer Lab]

        mProgramObject = 0;
    }

    if (mTimerQuery)
    {
        mTimerQuery.release(); // [Viewer Lab]
    }

    if (mSamplesQuery)
    {
        mSamplesQuery.release(); // [Viewer Lab]
    }

    //hack to make apple not complain
    flush_glerror(); // [Viewer Lab]

    stop_glerror();
}

bool LLGLSLShader::createShader()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    unloadInternal();

    sInstances.insert(this);
    const U32 generation = ++mGeneration; // [Viewer Lab]
    // [Viewer Lab] The level asked for, kept over publish's retries at a lower level, for the class line
    mAskedLevel  = mFallingBack ? mAskedLevel : mShaderLevel;
    mFallingBack = false;

    //reloading, reset matrix hash values
    for (U32 i = 0; i < LLRender::NUM_MATRIX_MODES; ++i)
    {
        mMatHash[i] = 0xFFFFFFFF;
    }
    mLightHash = 0xFFFFFFFF;

    llassert_always(!mShaderFiles.empty());

#if LL_DARWIN
    if (LLShaderMgr::shaderWorkarounds().oldSelect) // [Viewer Lab]
    {
        // work-around missing mix(vec3,vec3,bvec3)
        mDefines["OLD_SELECT"] = "1";
    }
#endif

    // [Viewer Lab] The whole program before anything compiles: its GLSL goes
    // into hash(), and the own files compile from the lines assembled here. The viewer's device gets the description
    // right after the GL program is made.
    LLShaderMgr* mgr = LLShaderMgr::instance();

    std::vector<LLShaderMgr::ShaderSource>      sources;
    std::vector<std::pair<std::string, GLenum>> shared;
    const bool                                  assembled = mgr->assembleProgram(this, sources, shared);

    mSourceHash = mgr->programSourceHash(sources, shared);

    LL::RHI::ProgramDesc desc;
    const bool           described = assembled && mgr->describeProgram(this, sources, shared, desc);

    mShaderHash = hash();

    // Create program
    // [Viewer Lab] and the viewer's device's program for it, from a complete description
    if (!gl_create_program(mProgramObject) || (described && !create_rhi_program(desc, mProgramObject, mRHIProgram, mRHIDevice)))
    {
        // Shouldn't happen if shader related extensions, like ARB_vertex_shader, exist.
        LL_SHADER_LOADING_WARNS() << "Failed to create handle for shader: " << mName << LL_ENDL;
        unloadInternal();
        setProgramState(ProgramState::Failed); // [Viewer Lab]
        return false;
    }

    bool success = true;

    mUsingBinaryProgram =  LLShaderMgr::instance()->loadCachedProgramBinary(this);

    if (!mUsingBinaryProgram)
    {
        const S32 level = mShaderLevel; // [Viewer Lab]
#if DEBUG_SHADER_INCLUDES
        fprintf(stderr, "--- %s ---\n", mName.c_str());
#endif // DEBUG_SHADER_INCLUDES

        //compile new source
        vector< pair<string, GLenum> >::iterator fileIter = mShaderFiles.begin();
        for (; fileIter != mShaderFiles.end(); fileIter++)
        {
            // [Viewer Lab] The lines assembled above, and the level asked for, for the fall-back's log
            LLShaderMgr::ShaderSource& source = sources[fileIter - mShaderFiles.begin()];
            const S32                  asked  = mShaderLevel;

            GLuint shaderhandle = LLShaderMgr::instance()->loadShaderFile((*fileIter).first, mShaderLevel, (*fileIter).second, &mDefines,
                mFeatures.mIndexedTextureChannels, &source); // [Viewer Lab] source
            LL_DEBUGS("ShaderLoading") << "SHADER FILE: " << (*fileIter).first << " mShaderLevel=" << mShaderLevel << LL_ENDL;
            if (shaderhandle)
            {
                attachObject(shaderhandle);
            }
            else if (!source.mCompiled) // [Viewer Lab] compiled without a GL object when GL is not the backend
            {
                success = false;
            }
            if (mShaderLevel < asked && source.mCompiled) // [Viewer Lab] the fall-back, per program
            {
                LL_SHADER_LOADING_WARNS() << "Program '" << mName << "': " << (*fileIter).first << " compiled at shader level "
                                          << mShaderLevel << ", not " << asked << LL_ENDL;
            }
            else if (mShaderLevel < asked) // [Viewer Lab] a file that compiled at no level
            {
                LL_SHADER_LOADING_WARNS() << "Program '" << mName << "': " << (*fileIter).first
                                          << " did not compile at any shader level from " << asked << " down to 1" << LL_ENDL;
            }
        }
        if (mShaderLevel < level) // [Viewer Lab] the binary cache saves under what GL compiled
        {
            mSourceHash = mgr->programSourceHash(sources, shared);
            mShaderHash = hash();
        }
    }

    // Attach existing objects
    if (!LLShaderMgr::instance()->attachShaderFeatures(this))
    {
        unloadInternal();
        setProgramState(ProgramState::Failed); // [Viewer Lab]
        return false;
    }

    // [Viewer Lab] The publication: inline, unless the program is still pending on a backend that
    // compiles in the background, which is never so on GL
    mBuilt = success;
    setProgramState(ProgramState::Pending);
    if (mRHIProgram.notNull() && mRHIProgram->status() == LL::RHI::CompileStatus::Pending)
    {
        return true;
    }
    return publish(generation);
}

// [Viewer Lab] What createShader did after the attach, moved verbatim, with the two marked lines; on the main thread. A
// result of another generation, or for a program that is not pending, is dropped.
bool LLGLSLShader::publish(U32 generation)
{
    if (generation != mGeneration || mProgramState != ProgramState::Pending)
    {
        return false;
    }
    bool success = mBuilt && (mRHIProgram.isNull() || mRHIProgram->status() != LL::RHI::CompileStatus::Failed);

    // Map attributes and uniforms
    if (success)
    {
        success = mapAttributes();
    }
    if (success)
    {
        success = mapUniforms();
    }
    if (success) // [Viewer Lab] reflection ready
    {
        setProgramState(ProgramState::Published);
        // [Viewer Lab] The class the program linked at and the level asked, so a fall to a lower class shows in the log on
        // either backend; golden level 2 wants the line for every program of the render profile. Held
        // until writeClassLines(), then logged at once.
        std::unique_lock<std::mutex> lock(sClassLinesMutex);
        if (!sClassLinesSeen.insert(mName + "|" + std::to_string(mShaderLevel) + "|" + std::to_string(mAskedLevel)).second)
        {
            // already held or written in this session
        }
        else if (sClassLinesHeld)
        {
            sClassLines.push_back({ mName, mShaderLevel, mAskedLevel });
        }
        else
        {
            lock.unlock();
            log_shader_class(mName, mShaderLevel, mAskedLevel);
        }
    }
    if (!success)
    {
        LL_SHADER_LOADING_WARNS() << "Failed to link shader: " << mName << LL_ENDL;

        // Try again using a lower shader level;
        if (mShaderLevel > 0)
        {
            LL_SHADER_LOADING_WARNS() << "Failed to link using shader level " << mShaderLevel << " trying again using shader level " << (mShaderLevel - 1) << LL_ENDL;
            mShaderLevel--;
            mFallingBack = true; // [Viewer Lab] createShader keeps the level asked

            return createShader();
        }
        else
        {
            // Give up and unload shader.
            unloadInternal();
            setProgramState(ProgramState::Failed); // [Viewer Lab]
        }
    }
    else if (mFeatures.mIndexedTextureChannels > 0)
    { //override texture channels for indexed texture rendering
        llassert(mFeatures.mIndexedTextureChannels == LLGLSLShader::sIndexedTextureChannels); // these numbers must always match
        bind();
        S32 channel_count = mFeatures.mIndexedTextureChannels;

        for (S32 i = 0; i < channel_count; i++)
        {
            LLStaticHashedString uniName(llformat("tex%d", i));
            uniform1i(uniName, i);
        }

        //adjust any texture channels that might have been overwritten
        for (U32 i = 0; i < mTexture.size(); i++)
        {
            if (mTexture[i] > -1)
            {
                S32 new_tex = mTexture[i] + channel_count;
                uniform1i(i, new_tex);
                mTexture[i] = new_tex;
            }
        }

        // get the true number of active texture channels
        mActiveTextureChannels = channel_count;
        for (auto& tex : mTexture)
        {
            mActiveTextureChannels = llmax(mActiveTextureChannels, tex + 1);
        }

        // when indexed texture channels are used, enforce an upper limit of 16
        // this should act as a canary in the coal mine for adding textures
        // and breaking machines that are limited to 16 texture channels
        llassert(mActiveTextureChannels <= 16);
        unbind();
    }

    LL_DEBUGS("GLSLTextureChannels") << mName << " has " << mActiveTextureChannels << " active texture channels" << LL_ENDL;

    for (U32 i = 0; i < mTexture.size(); i++)
    {
        if (mTexture[i] > -1)
        {
            LL_DEBUGS("GLSLTextureChannels") << "Texture " << LLShaderMgr::instance()->mReservedUniforms[i] << " assigned to channel " << mTexture[i] << LL_ENDL;
        }
    }

#if LL_PROFILER_ENABLE_RENDER_DOC
    setLabel(mName.c_str());
#endif

    return success;
}

// [Viewer Lab] The shaders in Pending: on GL createShader's Pending and its inline publication are +1 and -1 in the same
// call. Main thread, as the publication.
U32 LLGLSLShader::sPendingPrograms = 0;

void LLGLSLShader::setProgramState(ProgramState state)
{
    if (mProgramState == ProgramState::Pending)
    {
        llassert(sPendingPrograms > 0);
        --sPendingPrograms;
    }
    if (state == ProgramState::Pending)
    {
        ++sPendingPrograms;
    }
    mProgramState.value = state;
}

// [Viewer Lab] The two stages, the generation and the manifest writer's wait
bool LLGLSLShader::isReflectionReady() const
{
    return mProgramState == ProgramState::Published;
}

bool LLGLSLShader::isPipelineReady() const
{
    return mRHIProgram.notNull() ? mRHIProgram->status() == LL::RHI::CompileStatus::Ready : isReflectionReady();
}

void LLGLSLShader::waitForReflection()
{
    // A publication that fails builds again at a lower level, which can be pending again; level 0 ends in Failed
    while (mProgramState == ProgramState::Pending && mRHIProgram.notNull())
    {
        mRHIProgram->wait();
        publish(mGeneration);
    }
    llassert(mProgramState != ProgramState::Pending);
}

void LLGLSLShader::waitForPipeline()
{
    if (mRHIProgram.notNull())
    {
        mRHIProgram->wait();
    }
}

U32 LLGLSLShader::getGeneration() const
{
    return mGeneration;
}

// static
void LLGLSLShader::finishBuilds()
{
    // A copy, because a publication that fails can build again at a lower level
    const std::vector<LLGLSLShader*> shaders(sInstances.begin(), sInstances.end());
    for (LLGLSLShader* shader : shaders)
    {
        shader->waitForReflection();
    }
}

#if DEBUG_SHADER_INCLUDES
void dumpAttachObject(const char* func_name, GLuint program_object, const std::string& object_path)
{
    GLchar* info_log;
    GLint      info_len_expect = 0;
    GLint      info_len_actual = 0;

    glGetShaderiv(program_object, GL_INFO_LOG_LENGTH, , &info_len_expect);
    fprintf(stderr, " * %-20s(), log size: %d, %s\n", func_name, info_len_expect, object_path.c_str());

    if (info_len_expect > 0)
    {
        fprintf(stderr, " ========== %s() ========== \n", func_name);
        info_log = new GLchar[info_len_expect];
        glGetProgramInfoLog(program_object, info_len_expect, &info_len_actual, info_log);
        fprintf(stderr, "%s\n", info_log);
        delete[] info_log;
    }
}
#endif // DEBUG_SHADER_INCLUDES

bool LLGLSLShader::attachVertexObject(std::string object_path)
{
    if (LLShaderMgr::instance()->mVertexShaderObjects.count(object_path) > 0)
    {
        stop_glerror();
        gl_attach_shader(mProgramObject, LLShaderMgr::instance()->mVertexShaderObjects[object_path]); // [Viewer Lab]
#if DEBUG_SHADER_INCLUDES
        dumpAttachObject("attachVertexObject", mProgramObject, object_path);
#endif // DEBUG_SHADER_INCLUDES
        stop_glerror();
        return true;
    }
    else
    {
        LL_SHADER_LOADING_WARNS() << "Attempting to attach shader object: '" << object_path << "' that hasn't been compiled." << LL_ENDL;
        return false;
    }
}

bool LLGLSLShader::attachFragmentObject(std::string object_path)
{
    if(mUsingBinaryProgram)
        return true;

    if (LLShaderMgr::instance()->mFragmentShaderObjects.count(object_path) > 0)
    {
        stop_glerror();
        gl_attach_shader(mProgramObject, LLShaderMgr::instance()->mFragmentShaderObjects[object_path]); // [Viewer Lab]
#if DEBUG_SHADER_INCLUDES
        dumpAttachObject("attachFragmentObject", mProgramObject, object_path);
#endif // DEBUG_SHADER_INCLUDES
        stop_glerror();
        return true;
    }
    else
    {
        LL_SHADER_LOADING_WARNS() << "Attempting to attach shader object: '" << object_path << "' that hasn't been compiled." << LL_ENDL;
        return false;
    }
}

void LLGLSLShader::attachObject(GLuint object)
{
    if(mUsingBinaryProgram)
        return;

    if (object != 0)
    {
        stop_glerror();
        gl_attach_shader(mProgramObject, object); // [Viewer Lab]
#if DEBUG_SHADER_INCLUDES
        std::string object_path("???");
        dumpAttachObject("attachObject", mProgramObject, object_path);
#endif // DEBUG_SHADER_INCLUDES
        stop_glerror();
    }
    else
    {
        LL_SHADER_LOADING_WARNS() << "Attempting to attach non existing shader object. " << LL_ENDL;
    }
}

void LLGLSLShader::attachObjects(GLuint* objects, S32 count)
{
    if(mUsingBinaryProgram)
        return;

    for (S32 i = 0; i < count; i++)
    {
        attachObject(objects[i]);
    }
}

// [Viewer Lab] The GL reflection of a linked GL program: mapUniforms' and mapUniform's
// read-only queries, for the GL backend and for LLGLSLShader without the viewer's device. A uniform whose location is -1
// (a block member, or GL's own) is left out, as mapUniform leaves it out. No glGetError, so a pending error stays.
// static
void LLGLSLShader::reflectGLUniforms(U32 program, const std::string& label, LL::RHI::ProgramReflection& reflection,
                                     std::vector<GLint>* locations, std::vector<U32>* types)
{
    reflection.uniforms.clear();
    reflection.blocks.clear();
    if (locations)
    {
        locations->clear();
    }
    if (types)
    {
        types->clear();
    }

    //get the number of active uniforms
    GLint activeCount = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &activeCount);

    for (S32 i = 0; i < activeCount; i++)
    {
        GLenum type = 0;
        GLsizei length = 0;
        GLint size = -1;
        char name[1024];        /* Flawfinder: ignore */
        name[0] = 0;

        glGetActiveUniform(program, i, 1024, &length, &size, &type, (GLchar*)name);
        const GLint location = glGetUniformLocation(program, name);
        if (location == -1)
        {
            continue;
        }

        const std::optional<LL::RHI::UniformType> reflected = LL::RHI::uniformTypeFromGL(type);
        if (!reflected || type == GL_SAMPLER_2D_ARRAY)
        {
            // LLGLSLShader's mapping of it would differ from today's: no texture channel for a sampler2DArray, and no
            // mapping at all for a type the interface does not have
            LL_WARNS("RHI") << "Program '" << label << "': uniform '" << name << "' has a GL type (0x" << std::hex << type
                            << std::dec << ") that the interface " << (reflected ? "reflects as Sampler2D" : "does not reflect")
                            << LL_ENDL;
            if (!reflected)
            {
                continue;
            }
        }

        //chop off "[0]" so we can always access the first element
        //of an array by the array name
        char* is_array = strstr(name, "[0]");
        if (is_array)
        {
            is_array[0] = 0;
        }

        // [Viewer Lab] Two arrays that LLGLSLShader would map differently by the name without [0] (the viewer's GLSL has
        // neither): an array of structs, whose cut name has no location by GL's rules, since more follows the [0], so
        // mapUniform would leave it out; and a sampler array with a name of the diffuseMap rule, which the rule's search
        // of cut names would now match
        const char* why = nullptr;
        if (is_array && is_array[3] != 0)
        {
            why = "has no location by its name without [0]";
        }
        else if (is_array && LL::RHI::isSampler(*reflected) && is_diffuse_rule_sampler(name))
        {
            why = "is a sampler array that the diffuseMap rule would match by its name without [0]";
        }
        if (why) // the name as GL gives it: the cut name, then what followed the cut
        {
            LL_WARNS("RHI") << "Program '" << label << "': uniform '" << name << "[" << (is_array + 1) << "' " << why << LL_ENDL;
        }

        reflection.uniforms.push_back({ name, *reflected, U32(llmax(size, 1)) });
        if (locations)
        {
            locations->push_back(location);
        }
        if (types)
        {
            types->push_back(type);
        }
    }

    GLint blocks = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
    for (GLint i = 0; i < blocks; ++i)
    {
        GLchar buffer[256] = {};
        GLsizei length = 0;
        glGetActiveUniformBlockName(program, GLuint(i), sizeof(buffer), &length, buffer);
        GLint size = 0;
        glGetActiveUniformBlockiv(program, GLuint(i), GL_UNIFORM_BLOCK_DATA_SIZE, &size);
        reflection.blocks.push_back({ std::string(buffer, length), U32(size) });
    }
}

// [Viewer Lab] mapAttributes' read-back as a mask: bit i for each reserved attribute the linked program reads.
// static
U32 LLGLSLShader::reflectGLAttributes(U32 program, const std::vector<std::string>& reserved)
{
    U32 mask = 0;
    for (U32 i = 0; i < reserved.size() && i < 32; i++)
    {
        if (glGetAttribLocation(program, (const GLchar*)reserved[i].c_str()) != -1)
        {
            mask |= 1u << i;
        }
    }
    return mask;
}

bool LLGLSLShader::mapAttributes()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    bool res = true;
    if (!mUsingBinaryProgram)
    {
        res = gl_bind_attributes_and_link(*this); // [Viewer Lab] the reserved attributes' bindings and the link
    }

    mAttribute.clear();
#if LL_RELEASE_WITH_DEBUG_INFO
    mAttribute.resize(LLShaderMgr::instance()->mReservedAttribs.size(), { -1, NULL });
#else
    mAttribute.resize(LLShaderMgr::instance()->mReservedAttribs.size(), -1);
#endif

    if (res)
    { //read back channel locations

        mAttributeMask = 0;

        // [Viewer Lab] The reflection's attribute mask: the program's of the viewer's device, which
        // GL reflects here for the first time, right after the link, or the GL reflection without the device
        U32 reflected = 0;
        if (mRHIProgram.notNull())
        {
            mRHIProgram->wait();
            reflected = mRHIProgram->reflection().attributeMask;
        }
        else
        {
            reflected = gl_reflected_attribute_mask(mProgramObject, LLShaderMgr::instance()->mReservedAttribs);
        }

        //read back reserved channels first
        for (U32 i = 0; i < LLShaderMgr::instance()->mReservedAttribs.size(); i++)
        {
            if ((reflected & (1u << i)) == 0) // [Viewer Lab]
            {
                continue;
            }
            const char* name = LLShaderMgr::instance()->mReservedAttribs[i].c_str();
            S32 index = gl_attrib_location(mProgramObject, name, S32(i)); // [Viewer Lab]
            if (index != -1)
            {
#if LL_RELEASE_WITH_DEBUG_INFO
                mAttribute[i] = { index, name };
#else
                mAttribute[i] = index;
#endif
                mAttributeMask |= 1 << i;
                LL_DEBUGS("ShaderUniform") << "Attribute " << name << " assigned to channel " << index << LL_ENDL;
            }
        }

        return true;
    }

    return false;
}

void LLGLSLShader::mapUniform(const LL::RHI::ProgramReflection& reflection, GLint index)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (index < 0 || index >= GLint(reflection.uniforms.size())) // [Viewer Lab] an index of the reflection
    {
        return;
    }

    char name[1024];        /* Flawfinder: ignore */
    name[0] = 0;

    // [Viewer Lab] The name, the GL type and the active size from the reflection
    const LL::RHI::UniformInfo& info = reflection.uniforms[index];
    LLStringUtil::copy(name, info.name.c_str(), sizeof(name));
    GLenum type = LL::RHI::toGLUniformType(info.type);
    GLint size = GLint(info.arraySize);
    if (size > 0)
    {
        switch (type)
        {
        case GL_FLOAT_VEC2: size *= 2; break;
        case GL_FLOAT_VEC3: size *= 3; break;
        case GL_FLOAT_VEC4: size *= 4; break;
        case GL_DOUBLE: size *= 2; break;
        case GL_DOUBLE_VEC2: size *= 2; break;
        case GL_DOUBLE_VEC3: size *= 6; break;
        case GL_DOUBLE_VEC4: size *= 8; break;
        case GL_INT_VEC2: size *= 2; break;
        case GL_INT_VEC3: size *= 3; break;
        case GL_INT_VEC4: size *= 4; break;
        case GL_UNSIGNED_INT_VEC2: size *= 2; break;
        case GL_UNSIGNED_INT_VEC3: size *= 3; break;
        case GL_UNSIGNED_INT_VEC4: size *= 4; break;
        case GL_BOOL_VEC2: size *= 2; break;
        case GL_BOOL_VEC3: size *= 3; break;
        case GL_BOOL_VEC4: size *= 4; break;
        case GL_FLOAT_MAT2: size *= 4; break;
        case GL_FLOAT_MAT3: size *= 9; break;
        case GL_FLOAT_MAT4: size *= 16; break;
        case GL_FLOAT_MAT2x3: size *= 6; break;
        case GL_FLOAT_MAT2x4: size *= 8; break;
        case GL_FLOAT_MAT3x2: size *= 6; break;
        case GL_FLOAT_MAT3x4: size *= 12; break;
        case GL_FLOAT_MAT4x2: size *= 8; break;
        case GL_FLOAT_MAT4x3: size *= 12; break;
        case GL_DOUBLE_MAT2: size *= 8; break;
        case GL_DOUBLE_MAT3: size *= 18; break;
        case GL_DOUBLE_MAT4: size *= 32; break;
        case GL_DOUBLE_MAT2x3: size *= 12; break;
        case GL_DOUBLE_MAT2x4: size *= 16; break;
        case GL_DOUBLE_MAT3x2: size *= 12; break;
        case GL_DOUBLE_MAT3x4: size *= 24; break;
        case GL_DOUBLE_MAT4x2: size *= 16; break;
        case GL_DOUBLE_MAT4x3: size *= 24; break;
        }
        mTotalUniformSize += size;
    }

    S32 location = gl_uniform_location(mProgramObject, name, index); // [Viewer Lab]
    if (location != -1)
    {
        //chop off "[0]" so we can always access the first element
        //of an array by the array name
        char* is_array = strstr(name, "[0]");
        if (is_array)
        {
            is_array[0] = 0;
        }

        LLStaticHashedString hashedName(name);
        mUniformMap[hashedName] = location;
        mReflectionIndex[location] = U32(index); // [Viewer Lab] for the encoder

        LL_DEBUGS("ShaderUniform") << "Uniform " << name << " is at location " << location << LL_ENDL;

        //find the index of this uniform
        for (S32 i = 0; i < (S32)LLShaderMgr::instance()->mReservedUniforms.size(); i++)
        {
            if ((mUniform[i] == -1)
                && (LLShaderMgr::instance()->mReservedUniforms[i] == name))
            {
                //found it
                mUniform[i] = location;
                mTexture[i] = mapUniformTextureChannel(location, type, size);
                if (mTexture[i] != -1)
                {
                    LL_DEBUGS("GLSLTextureChannels") << name << " assigned to texture channel " << mTexture[i] << LL_ENDL;
                }
                return;
            }
        }
    }
}

void LLGLSLShader::clearPermutations()
{
    mDefines.clear();
}

void LLGLSLShader::addPermutation(std::string name, std::string value)
{
    mDefines[name] = value;
}

void LLGLSLShader::addConstant(const LLGLSLShader::eShaderConsts shader_const)
{
    addPermutation(gShaderConstsKey[shader_const], gShaderConstsVal[shader_const]);
}

void LLGLSLShader::removePermutation(std::string name)
{
    mDefines.erase(name);
}

GLint LLGLSLShader::mapUniformTextureChannel(GLint location, GLenum type, GLint size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if ((type >= GL_SAMPLER_1D && type <= GL_SAMPLER_2D_RECT_SHADOW) ||
        type == GL_SAMPLER_2D_MULTISAMPLE ||
        type == GL_SAMPLER_CUBE_MAP_ARRAY)
    {   //this here is a texture
        GLint ret = mActiveTextureChannels;
        if (size == 1)
        {
            // [Viewer Lab] The same call through sendUniform, with no flush and no value cache, which with the viewer's
            // device also gives the encoder the sampler's channel (setSamplerChannel)
            const GLint channel = mActiveTextureChannels;
            sendUniform(GLUniformCall::Uniform1i, location, 1, GL_FALSE, &channel);
            mActiveTextureChannels++;
        }
        else
        {
            //is array of textures, make sequential after this texture
            GLint channel[16]; // <=== only support up to 16 texture channels
            llassert(size <= 16);
            size = llmin(size, 16);
            for (int i = 0; i < size; ++i)
            {
                channel[i] = mActiveTextureChannels++;
            }
            // [Viewer Lab] The same glUniform1iv through sendUniform; the interface takes no sampler array, so the encoder
            // gets nothing and one WARN per program and call says so (no GLSL of the viewer has one)
            sendUniform(GLUniformCall::Uniform1iv, location, size, GL_FALSE, channel);
        }

        return ret;
    }
    return -1;
}

bool LLGLSLShader::mapUniforms()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    bool res = true;

    mTotalUniformSize = 0;
    mActiveTextureChannels = 0;
    mUniform.clear();
    mUniformMap.clear();
    mReflectionIndex.clear(); // [Viewer Lab]
    mTexture.clear();
    mValue.clear();
    //initialize arrays
    mUniform.resize(LLShaderMgr::instance()->mReservedUniforms.size(), -1);
    mTexture.resize(LLShaderMgr::instance()->mReservedUniforms.size(), -1);

    mMapping = true; // [Viewer Lab] bind()'s uniform update belongs to the publication
    bind();

    // [Viewer Lab] The reflection in the interface's backend-neutral format: the program's of the
    // viewer's device, or the GL reflection without the device
    LL::RHI::ProgramReflection driver;
    if (mRHIProgram.isNull())
    {
        gl_reflect_uniforms(mProgramObject, mName, driver);
    }
    else
    {
        mRHIProgram->wait();
    }
    const LL::RHI::ProgramReflection& reflection = mRHIProgram.notNull() ? mRHIProgram->reflection() : driver;

    //get the number of active uniforms
    GLint activeCount = GLint(reflection.uniforms.size()); // [Viewer Lab]

    //........................................................................................................................................
    //........................................................................................

    /*
    EXPLANATION:
    This is part of code is temporary because as the final result the mapUniform() should be rewrited.
    But it's a huge a volume of work which is need to be a more carefully performed for avoid possible
    regression's (i.e. it should be formalized a separate ticket in JIRA).

    RESON:
    The reason of this code is that SL engine is very sensitive to fact that "diffuseMap" should be appear
    first as uniform parameter which is should get 0-"texture channel" index (see mapUniformTextureChannel() and mActiveTextureChannels)
    it influence to which is texture matrix will be updated during rendering.

    But, order of indexe's of uniform variables is not defined and GLSL compiler can change it as want
    , even if the "diffuseMap" will be appear and use first in shader code.

    As example where this situation appear see: "Deferred Material Shader 28/29/30/31"
    And tickets: MAINT-4165, MAINT-4839, MAINT-3568, MAINT-6437

    --- davep TODO -- pretty sure the entire block here is superstitious and that the uniform index has nothing to do with the texture channel
                texture channel should follow the uniform VALUE
    */


    // [Viewer Lab] The same queries on GL, the reflection's index on another backend
    S32 diffuseMap = gl_uniform_location(mProgramObject, "diffuseMap", reflected_index(reflection, "diffuseMap"));
    S32 specularMap = gl_uniform_location(mProgramObject, "specularMap", reflected_index(reflection, "specularMap"));
    S32 bumpMap = gl_uniform_location(mProgramObject, "bumpMap", reflected_index(reflection, "bumpMap"));
    S32 altDiffuseMap = gl_uniform_location(mProgramObject, "altDiffuseMap", reflected_index(reflection, "altDiffuseMap"));
    S32 environmentMap = gl_uniform_location(mProgramObject, "environmentMap", reflected_index(reflection, "environmentMap"));
    S32 reflectionMap = gl_uniform_location(mProgramObject, "reflectionMap", reflected_index(reflection, "reflectionMap"));

    std::set<S32> skip_index;

    if (-1 != diffuseMap && (-1 != specularMap || -1 != bumpMap || -1 != environmentMap || -1 != altDiffuseMap))
    {
        diffuseMap = altDiffuseMap = specularMap = bumpMap = environmentMap = -1;

        for (S32 i = 0; i < activeCount; i++)
        {
            const char* name = reflection.uniforms[i].name.c_str(); // [Viewer Lab] the reflection's

            if (-1 == diffuseMap && std::string(name) == "diffuseMap")
            {
                diffuseMap = i;
                continue;
            }

            if (-1 == specularMap && std::string(name) == "specularMap")
            {
                specularMap = i;
                continue;
            }

            if (-1 == bumpMap && std::string(name) == "bumpMap")
            {
                bumpMap = i;
                continue;
            }

            if (-1 == environmentMap && std::string(name) == "environmentMap")
            {
                environmentMap = i;
                continue;
            }

            if (-1 == reflectionMap && std::string(name) == "reflectionMap")
            {
                reflectionMap = i;
                continue;
            }

            if (-1 == altDiffuseMap && std::string(name) == "altDiffuseMap")
            {
                altDiffuseMap = i;
                continue;
            }
        }

        bool specularDiff = specularMap < diffuseMap && -1 != specularMap;
        bool bumpLessDiff = bumpMap < diffuseMap && -1 != bumpMap;
        bool envLessDiff = environmentMap < diffuseMap && -1 != environmentMap;
        bool refLessDiff = reflectionMap < diffuseMap && -1 != reflectionMap;

        if (specularDiff || bumpLessDiff || envLessDiff || refLessDiff)
        {
            mapUniform(reflection, diffuseMap); // [Viewer Lab]
            skip_index.insert(diffuseMap);

            if (-1 != specularMap) {
                mapUniform(reflection, specularMap); // [Viewer Lab]
                skip_index.insert(specularMap);
            }

            if (-1 != bumpMap) {
                mapUniform(reflection, bumpMap); // [Viewer Lab]
                skip_index.insert(bumpMap);
            }

            if (-1 != environmentMap) {
                mapUniform(reflection, environmentMap); // [Viewer Lab]
                skip_index.insert(environmentMap);
            }

            if (-1 != reflectionMap) {
                mapUniform(reflection, reflectionMap); // [Viewer Lab]
                skip_index.insert(reflectionMap);
            }
        }
    }

    //........................................................................................

    for (S32 i = 0; i < activeCount; i++)
    {
        //........................................................................................
        if (skip_index.end() != skip_index.find(i)) continue;
        //........................................................................................

        mapUniform(reflection, i); // [Viewer Lab]
    }
    //........................................................................................................................................

    gl_bind_uniform_blocks(mProgramObject, (1u << NUM_UNIFORM_BLOCKS) - 1, interface_blocks()); // [Viewer Lab]

    unbind();

    LL_DEBUGS("ShaderUniform") << "Total Uniform Size: " << mTotalUniformSize << LL_ENDL;
    mMapping = false; // [Viewer Lab]
    return res;
}


bool LLGLSLShader::link(bool suppress_errors)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    bool success = LLShaderMgr::instance()->linkProgramObject(mProgramObject, suppress_errors);

    if (!success && !suppress_errors)
    {
        LLShaderMgr::instance()->dumpObjectLog(mProgramObject, !success, mName);
    }

    if (success)
    {
        LLShaderMgr::instance()->saveCachedProgramBinary(this);
    }

    return success;
}

// [Viewer Lab] bind()'s refusal record: each shader bind() refused, with the generation it refused, so a draw
// knows a refused shader by the facade's own state and never by a GL name, the same on every backend. Main thread only,
// as bind. A shader is refused while it has a pair of any generation, so one unloaded or rebuilt without success while
// current stays refused; the generation keeps the WARN to one per generation. bind() erases a shader's pairs when it
// binds the shader complete; unbind() leaves them, since SavedGLState::restore can make a refused shader current again
// without a bind.
static std::set<std::pair<const LLGLSLShader*, U32>> sRefusedShaders;
// [Viewer Lab] The pending programs bind() has published since the process began
static U32 sBindPublications = 0;

// static
U32 LLGLSLShader::bindPublications()
{
    return sBindPublications;
}

bool LLGLSLShader::isRefused() const
{
    if (sRefusedShaders.empty())
    {
        return false;
    }
    const auto it = sRefusedShaders.lower_bound({ this, 0 });
    return it != sRefusedShaders.end() && it->first == this;
}

void LLGLSLShader::bind()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    // [Viewer Lab] Reflection ready first: a pending program is published before the flush,
    // the program switch, the uniform update and the loaders' uniform1i after bind, except while mapUniforms runs.
    const bool pending = !mMapping && mProgramState == ProgramState::Pending;
    waitForUniforms();
    if (pending && mProgramState != ProgramState::Pending)
    {
        ++sBindPublications;
    }

    // [Viewer Lab] A program that is not complete now is never bound: GL gets program 0, and the shader is the selected
    // one, so code that reads sCurBoundShaderPtr after bind still works, while its draws are skipped. On GL
    // this is exactly the old llassert_always(mProgramObject != 0).
    const bool refused = !isComplete();
    if (refused)
    {
        if (sRefusedShaders.emplace(this, mGeneration).second)
        {
            LL_WARNS("Shader") << "Program " << mName << " failed: not bound, and its draws are skipped" << LL_ENDL;
        }
    }
    else if (!sRefusedShaders.empty())
    {
        auto it = sRefusedShaders.lower_bound({ this, 0 });
        while (it != sRefusedShaders.end() && it->first == this)
        {
            it = sRefusedShaders.erase(it);
        }
    }

    gGL.flush();

    // [Viewer Lab] Or the pointer, for a shader without a GL program: a refused one, or any on another backend
    if (sCurBoundShader != mProgramObject || (mProgramObject == 0 && sCurBoundShaderPtr != this))  // Don't re-bind current shader
    {
        if (sCurBoundShaderPtr)
        {
            sCurBoundShaderPtr->readProfileQuery();
        }
        LLVertexBuffer::unbind();
        gl_use_program(mProgramObject); // [Viewer Lab]
        sCurBoundShader = mProgramObject;
        sCurBoundShaderPtr = this;
        placeProfileQuery();
        gl_setup_client_arrays(mAttributeMask); // [Viewer Lab]
    }

    if (refused) // [Viewer Lab] no uniform update for a program that is not there
    {
        return;
    }

    if (mUniformsDirty)
    {
        LLShaderMgr::instance()->updateShaderUniforms(this);
        mUniformsDirty = false;
    }

    llassert_always(sCurBoundShaderPtr != nullptr);
    llassert_always(sCurBoundShader == mProgramObject);
}

void LLGLSLShader::bind(U8 variant)
{
    // [Viewer Lab] A shader that is not complete and has no variants, because its loader never made them, is refused as
    // bind() refuses it, and its draws are skipped
    if (mGLTFVariants.size() != LLGLSLShader::NUM_GLTF_VARIANTS && !isComplete())
    {
        bind();
        return;
    }
    llassert_always(mGLTFVariants.size() == LLGLSLShader::NUM_GLTF_VARIANTS);
    llassert_always(variant < LLGLSLShader::NUM_GLTF_VARIANTS);
    mGLTFVariants[variant].bind();
}

void LLGLSLShader::bind(bool rigged)
{
    if (rigged)
    {
        // [Viewer Lab] As bind(U8): no rigged variant on a shader that is not complete is a refusal
        if (!mRiggedVariant && !isComplete())
        {
            bind();
            return;
        }
        llassert_always(mRiggedVariant);
        mRiggedVariant->bind();
    }
    else
    {
        bind();
    }
}

void LLGLSLShader::unbind(void)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    gGL.flush();
    LLVertexBuffer::unbind();

    if (sCurBoundShaderPtr)
    {
        sCurBoundShaderPtr->readProfileQuery();
    }

    gl_use_program(0); // [Viewer Lab]
    sCurBoundShader = 0;
    sCurBoundShaderPtr = NULL;
}

S32 LLGLSLShader::bindTexture(const std::string& uniform, LLTexture* texture, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 channel = 0;
    channel = getUniformLocation(uniform);

    return bindTexture(channel, texture, mode);
}

S32 LLGLSLShader::bindTexture(S32 uniform, LLTexture* texture, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] mTexture comes from the publication: reflection ready first

    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << uniform << LL_ENDL;
        llassert(false);
        return -1;
    }

    uniform = mTexture[uniform];

    if (uniform > -1)
    {
        gGL.getTexUnit(uniform)->bindFast(texture);
    }

    return uniform;
}

// [Viewer Lab] The GL backend's raw-name bind for a raw colour attachment (no device, setColorAttachment's caller name,
// a refused description)
static bool bind_raw(LLTexUnit* unit, LLTexUnit::eTextureType usage, U32 name, bool has_mips)
{
    return gl_bind_raw_name(unit, usage, name, has_mips);
}

S32 LLGLSLShader::bindTexture(S32 uniform, LLRenderTarget* texture, bool depth, LLTexUnit::eTextureFilterOptions mode, U32 index)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready

    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << uniform << LL_ENDL;
        llassert(false);
        return -1;
    }

    uniform = getTextureChannel(uniform);

    if (uniform > -1)
    {
        if (depth) {
            gGL.getTexUnit(uniform)->bind(texture, true);
        }
        else {
            bool has_mips = mode == LLTexUnit::TFO_TRILINEAR || mode == LLTexUnit::TFO_ANISOTROPIC;
            // [Viewer Lab] bindManual's GL by the attachment's device texture, or its raw name, so the filter below reaches
            // the texture LLTexUnit's facade remembers
            if (LL::RHI::Texture* rhi = texture->getRHITexture(index))
            {
                gGL.getTexUnit(uniform)->bindTexture(texture->getUsage(), rhi, has_mips);
            }
            else
            {
                bind_raw(gGL.getTexUnit(uniform), texture->getUsage(), texture->getTexture(index), has_mips);
            }
        }

        gGL.getTexUnit(uniform)->setTextureFilteringOption(mode);
    }

    return uniform;
}

S32 LLGLSLShader::bindTexture(const std::string& uniform, LLRenderTarget* texture, bool depth, LLTexUnit::eTextureFilterOptions mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 channel = 0;
    channel = getUniformLocation(uniform);

    return bindTexture(channel, texture, depth, mode);
}

S32 LLGLSLShader::unbindTexture(const std::string& uniform, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 channel = 0;
    channel = getUniformLocation(uniform);

    return unbindTexture(channel);
}

S32 LLGLSLShader::unbindTexture(S32 uniform, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready

    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << uniform << LL_ENDL;
        llassert(false);
        return -1;
    }

    uniform = mTexture[uniform];

    if (uniform > -1)
    {
        gGL.getTexUnit(uniform)->unbindFast(mode);
    }

    return uniform;
}

S32 LLGLSLShader::getTextureChannel(S32 uniform) const
{
    // [Viewer Lab] The other channel functions' range test: a refused or unloaded shader has
    // no channels, and bind() makes a refused one current, so callers that read a channel after a bind get -1, which
    // each of them handles, instead of a read past an empty mTexture. An index out of range on a complete shader is
    // still a programming error.
    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mTexture.size() << " index: " << uniform << LL_ENDL;
        llassert(!isComplete());
        return -1;
    }
    return mTexture[uniform];
}

S32 LLGLSLShader::enableTexture(S32 uniform, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready

    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << uniform << LL_ENDL;
        llassert(false);
        return -1;
    }


    S32 index = mTexture[uniform];
    if (index != -1)
    {
        gGL.getTexUnit(index)->activate();
        gGL.getTexUnit(index)->enable(mode);
    }
    return index;
}

S32 LLGLSLShader::disableTexture(S32 uniform, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready

    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << uniform << LL_ENDL;
        llassert(false);
        return -1;
    }

    S32 index = mTexture[uniform];
    if (index < 0)
    {
        // Invalid texture index - nothing to disable
        return index;
    }

    LLTexUnit* tex_unit = gGL.getTexUnit(index);
    if (!tex_unit)
    {
        // Invalid texture unit
        LL_WARNS_ONCE("Shader") << "Invalid texture unit at index: " << index << LL_ENDL;
        return index;
    }

    LLTexUnit::eTextureType curr_type = tex_unit->getCurrType();
    if (curr_type != LLTexUnit::TT_NONE)
    {
        if (gDebugGL && curr_type != mode)
        {
            if (gDebugSession)
            {
                gFailLog << "Texture channel " << index << " texture type corrupted. Expected: " << mode << ", Found: " << curr_type << std::endl;
                ll_fail("LLGLSLShader::disableTexture failed");
            }
            else
            {
                LL_ERRS() << "Texture channel " << index << " texture type corrupted. Expected: " << mode << ", Found: " << curr_type << LL_ENDL;
            }
        }
        tex_unit->disable();
    }

    return index;
}

// [Viewer Lab] The setters through the viewer's render device. Each setter makes today's GL call
// through gl_uniform; when it sends a value, the encoder gets what GL takes for the reflected uniform, inside LLGLSLShader's
// scope, where the GL backend only checks the call.

// True on this thread while LLGLSLShader gives its own uniform call to the encoder
static thread_local bool sMakesOwnGLUniforms = false;

// What the setters have given the encoder, and the tests' log of it, null in the viewer; the log on the main thread only
static std::atomic<U64> sDeviceUniformValues{ 0 };
static std::atomic<U64> sDeviceSamplerChannels{ 0 };
static std::atomic<U64> sDeviceRefused{ 0 };
static std::atomic<U64> sDeviceClamped{ 0 };
static std::vector<LLGLSLShader::DeviceCall>* sDeviceCallLog = nullptr;

LLGLSLShader::OwnGLUniforms::OwnGLUniforms()
:   mPrevious(sMakesOwnGLUniforms)
{
    sMakesOwnGLUniforms = true;
}

LLGLSLShader::OwnGLUniforms::~OwnGLUniforms()
{
    sMakesOwnGLUniforms = mPrevious;
}

// static
bool LLGLSLShader::makesOwnGLUniforms()
{
    return sMakesOwnGLUniforms;
}

// static
LLGLSLShader::DeviceCounts LLGLSLShader::deviceCounts()
{
    DeviceCounts counts;
    counts.uniforms = sDeviceUniformValues;
    counts.samplerChannels = sDeviceSamplerChannels;
    counts.refused = sDeviceRefused;
    counts.clamped = sDeviceClamped;
    return counts;
}

// static
void LLGLSLShader::setDeviceCallLog(std::vector<DeviceCall>* log)
{
    sDeviceCallLog = log;
}

// The reflected type a setter's GL call sets. The int and uint calls give Int and IVec*, as the reflection folds bool and
// uint into them.
static LL::RHI::UniformType uniform_shape(LLGLSLShader::GLUniformCall call)
{
    using Call = LLGLSLShader::GLUniformCall;
    using Type = LL::RHI::UniformType;
    switch (call)
    {
    case Call::Uniform1i:
    case Call::Uniform1iv:  return Type::Int;
    case Call::Uniform2i:   return Type::IVec2;
    case Call::Uniform4iv:
    case Call::Uniform4uiv: return Type::IVec4;
    case Call::Uniform1f:
    case Call::Uniform1fv:  return Type::Float;
    case Call::Uniform2f:
    case Call::Uniform2fv:  return Type::Vec2;
    case Call::Uniform3f:
    case Call::Uniform3fv:  return Type::Vec3;
    case Call::Uniform4f:
    case Call::Uniform4fv:  return Type::Vec4;
    case Call::Matrix2fv:   return Type::Mat2;
    case Call::Matrix3fv:   return Type::Mat3;
    case Call::Matrix3x4fv: return Type::Mat3x4;
    case Call::Matrix4fv:   return Type::Mat4;
    }
    return Type::Float;
}

// The call's name for the log, by GLUniformCall's order
static const char* uniform_call_name(LLGLSLShader::GLUniformCall call)
{
    static const char* const NAMES[] = { "uniform1i",  "uniform2i",  "uniform1iv", "uniform4iv",       "uniform4uiv",
                                         "uniform1f",  "uniform2f",  "uniform3f",  "uniform4f",        "uniform1fv",
                                         "uniform2fv", "uniform3fv", "uniform4fv", "uniformMatrix2fv", "uniformMatrix3fv",
                                         "uniformMatrix3x4fv", "uniformMatrix4fv" };
    return U8(call) < std::size(NAMES) ? NAMES[U8(call)] : "?";
}

// A transposed copy of count matrices of a reflected matrix type, which GL was asked to transpose: the caller's are row
// major, the encoder takes them column-major, out[c * R + r] = in[r * C + c] for C columns and R rows. Mat2 is 2 x 2, Mat3
// 3 x 3, Mat3x4 3 columns of 4 and Mat4 4 x 4. In a scratch buffer of this thread, valid until the next call.
static const F32* transposed_matrices(LL::RHI::UniformType type, U32 count, const F32* in)
{
    U32 columns = 4;
    U32 rows = 4;
    switch (type)
    {
    case LL::RHI::UniformType::Mat2:   columns = rows = 2; break;
    case LL::RHI::UniformType::Mat3:   columns = rows = 3; break;
    case LL::RHI::UniformType::Mat3x4: columns = 3; break;
    default:                           break;
    }
    const U32 elements = columns * rows;
    static thread_local std::vector<F32> scratch;
    scratch.resize(size_t(count) * elements);
    for (U32 m = 0; m < count; ++m)
    {
        const F32* from = in + size_t(m) * elements;
        F32* to = scratch.data() + size_t(m) * elements;
        for (U32 c = 0; c < columns; ++c)
        {
            for (U32 r = 0; r < rows; ++r)
            {
                to[c * rows + r] = from[r * columns + c];
            }
        }
    }
    return scratch.data();
}

// Counts a call and logs it for the tests. The first value and the first sampler channel through the encoder are said once
// per process at INFO, as the viewer's evidence in its log
static void note_uniform_call(char kind, U64 program, U32 uniform, U32 count, U32 channel, const void* bytes, size_t size)
{
    static const char* const FIRST[] = { "LLGLSLShader: first uniform through the encoder",
                                         "LLGLSLShader: first sampler channel through the encoder" };
    static std::atomic<bool> said[std::size(FIRST)];
    size_t first = std::size(FIRST);
    switch (kind)
    {
    case 'u':
        sDeviceUniformValues.fetch_add(1, std::memory_order_relaxed);
        first = 0;
        break;
    case 's':
        sDeviceSamplerChannels.fetch_add(1, std::memory_order_relaxed);
        first = 1;
        break;
    default:
        sDeviceRefused.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    if (first < std::size(FIRST) && !said[first].load(std::memory_order_relaxed) && !said[first].exchange(true))
    {
        LL_INFOS("RHI") << FIRST[first] << LL_ENDL;
    }
    if (sDeviceCallLog)
    {
        LLGLSLShader::DeviceCall call;
        call.kind = kind;
        call.program = program;
        call.uniform = uniform;
        call.count = count;
        call.channel = channel;
        const U8* from = static_cast<const U8*>(bytes);
        call.bytes.assign(from, from + size);
        sDeviceCallLog->push_back(std::move(call));
    }
}

// [Viewer Lab]
void LLGLSLShader::sendUniform(GLUniformCall call, GLint location, GLsizei count, GLboolean transpose, const void* values)
{
    gl_uniform(call, location, count, transpose, values);

    // The encoder, for the program of the viewer's device that made it, on the main thread, per reflected uniform. A count of
    // 0 sets nothing on GL and raises no error, so it sends nothing and is neither counted nor warned.
    if (location < 0 || count == 0 || mRHIProgram.isNull())
    {
        return;
    }
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || device->id() != mRHIDevice || !on_main_thread())
    {
        return;
    }
    const auto found = mReflectionIndex.find(location);
    if (found == mReflectionIndex.end())
    {
        return;
    }
    LL::RHI::Program* program = mRHIProgram.get();
    const U32 index = found->second;
    const LL::RHI::ProgramReflection& reflection = program->reflection();
    if (index >= reflection.uniforms.size())
    {
        return;
    }
    const LL::RHI::UniformInfo& info = reflection.uniforms[index];

    // uniform1i, and uniform1iv with one value, on a sampler: its texture channel, which GL takes when it is not negative
    const GLint* ints = static_cast<const GLint*>(values);
    if (LL::RHI::isSampler(info.type))
    {
        if ((call == GLUniformCall::Uniform1i || (call == GLUniformCall::Uniform1iv && count == 1)) && ints[0] >= 0)
        {
            {
                OwnGLUniforms own;
                device->encoder().setSamplerChannel(program, index, U32(ints[0]));
            }
            note_uniform_call('s', program->id(), index, 1, U32(ints[0]), nullptr, 0);
            return;
        }
    }
    // A value of the reflected type: at most the reflected array size, which the driver trims to the elements in use and
    // past which GL ignores the values; column-major when the caller asked GL to transpose
    else if (count > 0 && uniform_shape(call) == info.type)
    {
        const U32 sent = llmin(U32(count), info.arraySize);
        if (sent < U32(count))
        {
            sDeviceClamped.fetch_add(1, std::memory_order_relaxed);
        }
        const void* bytes = transpose ? transposed_matrices(info.type, sent, static_cast<const F32*>(values)) : values;
        const size_t size = size_t(LL::RHI::uniformSize(info.type)) * sent;
        {
            OwnGLUniforms own;
            device->encoder().setUniform(program, index, std::span<const std::byte>(static_cast<const std::byte*>(bytes), size),
                                         sent);
        }
        note_uniform_call('u', program->id(), index, sent, 0, bytes, size);
        return;
    }

    // Anything else, which GL refuses or the interface does not take (a sampler array's values), sends nothing: one WARN
    // per program, uniform and call, as a per-frame caller would repeat it
    note_uniform_call('r', program->id(), index, U32(count), 0, nullptr, 0);
    static std::set<std::tuple<U64, U32, U8>> warned;
    if (warned.insert({ program->id(), index, U8(call) }).second)
    {
        LL_WARNS("Shader") << "Program '" << mName << "': " << uniform_call_name(call) << " with count " << count
                           << " does not fit uniform '" << info.name << "'; the render device gets nothing" << LL_ENDL;
    }
}

void LLGLSLShader::uniform1i(U32 index, GLint x)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);
    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            if (iter == mValue.end() || iter->second.mV[0] != x)
            {
                const GLint v[] = { x }; // [Viewer Lab]
                sendUniform(GLUniformCall::Uniform1i, mUniform[index], 1, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = LLVector4((F32)x, 0.f, 0.f, 0.f);
            }
        }
    }
}

void LLGLSLShader::uniform1f(U32 index, GLfloat x)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            if (iter == mValue.end() || iter->second.mV[0] != x)
            {
                const GLfloat v[] = { x }; // [Viewer Lab]
                sendUniform(GLUniformCall::Uniform1f, mUniform[index], 1, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = LLVector4(x, 0.f, 0.f, 0.f);
            }
        }
    }
}

void LLGLSLShader::fastUniform1f(U32 index, GLfloat x)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);
    llassert(isComplete()); // [Viewer Lab]
    llassert(mUniform.size() <= index);
    llassert(mUniform[index] >= 0);
    const GLfloat v[] = { x }; // [Viewer Lab]
    sendUniform(GLUniformCall::Uniform1f, mUniform[index], 1, GL_FALSE, v); // [Viewer Lab]
}

void LLGLSLShader::uniform2f(U32 index, GLfloat x, GLfloat y)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(x, y, 0.f, 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec))
            {
                const GLfloat v[] = { x, y }; // [Viewer Lab]
                sendUniform(GLUniformCall::Uniform2f, mUniform[index], 1, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform3f(U32 index, GLfloat x, GLfloat y, GLfloat z)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(x, y, z, 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec))
            {
                const GLfloat v[] = { x, y, z }; // [Viewer Lab]
                sendUniform(GLUniformCall::Uniform3f, mUniform[index], 1, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform4f(U32 index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(x, y, z, w);
            if (iter == mValue.end() || shouldChange(iter->second, vec))
            {
                const GLfloat v[] = { x, y, z, w }; // [Viewer Lab]
                sendUniform(GLUniformCall::Uniform4f, mUniform[index], 1, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform1iv(U32 index, U32 count, const GLint* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec((F32)v[0], 0.f, 0.f, 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                sendUniform(GLUniformCall::Uniform1iv, mUniform[index], count, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform4iv(U32 index, U32 count, const GLint* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                sendUniform(GLUniformCall::Uniform4iv, mUniform[index], count, GL_FALSE, v); // <FS:Beq/>  Apparent copy/paste error. [Viewer Lab] through sendUniform
                mValue[mUniform[index]] = vec;
            }
        }
    }
}


void LLGLSLShader::uniform1fv(U32 index, U32 count, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(v[0], 0.f, 0.f, 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                sendUniform(GLUniformCall::Uniform1fv, mUniform[index], count, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform2fv(U32 index, U32 count, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(v[0], v[1], 0.f, 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                sendUniform(GLUniformCall::Uniform2fv, mUniform[index], count, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform3fv(U32 index, U32 count, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(v[0], v[1], v[2], 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                sendUniform(GLUniformCall::Uniform3fv, mUniform[index], count, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform4fv(U32 index, U32 count, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(v[0], v[1], v[2], v[3]);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
                sendUniform(GLUniformCall::Uniform4fv, mUniform[index], count, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform4uiv(U32 index, U32 count, const GLuint* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
                sendUniform(GLUniformCall::Uniform4uiv, mUniform[index], count, GL_FALSE, v); // [Viewer Lab]
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniformMatrix2fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            sendUniform(GLUniformCall::Matrix2fv, mUniform[index], count, transpose, v); // [Viewer Lab]
        }
    }
}

void LLGLSLShader::uniformMatrix3fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            sendUniform(GLUniformCall::Matrix3fv, mUniform[index], count, transpose, v); // [Viewer Lab]
        }
    }
}

void LLGLSLShader::uniformMatrix3x4fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            sendUniform(GLUniformCall::Matrix3x4fv, mUniform[index], count, transpose, v); // [Viewer Lab]
        }
    }
}

void LLGLSLShader::uniformMatrix4fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready
    llassert(sCurBoundShaderPtr == this);

    if (isComplete()) // [Viewer Lab]
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            sendUniform(GLUniformCall::Matrix4fv, mUniform[index], count, transpose, v); // [Viewer Lab]
        }
    }
}

GLint LLGLSLShader::getUniformLocation(const LLStaticHashedString& uniform)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready

    GLint ret = -1;
    if (isComplete()) // [Viewer Lab]
    {
        LLStaticStringTable<GLint>::iterator iter = mUniformMap.find(uniform);
        if (iter != mUniformMap.end())
        {
            if (gDebugGL)
            {
                stop_glerror();
                if (iter->second != gl_uniform_location(mProgramObject, uniform.String().c_str(), iter->second)) // [Viewer Lab]
                {
                    LL_ERRS() << "Uniform does not match." << LL_ENDL;
                }
                stop_glerror();
            }
            ret = iter->second;
        }
    }

    return ret;
}

GLint LLGLSLShader::getUniformLocation(U32 index)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready

    GLint ret = -1;
    if (isComplete()) // [Viewer Lab]
    {
        if (index >= mUniform.size())
        {
            LL_WARNS_ONCE("Shader") << "Uniform index " << index << " out of bounds " << (S32)mUniform.size() << LL_ENDL;
            return ret;
        }
        return mUniform[index];
    }

    return ret;
}

GLint LLGLSLShader::getAttribLocation(U32 attrib)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    waitForUniforms(); // [Viewer Lab] reflection ready

    if (attrib < mAttribute.size())
    {
        return mAttribute[attrib];
    }
    else
    {
        return -1;
    }
}

void LLGLSLShader::uniform1i(const LLStaticHashedString& uniform, GLint v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec((F32)v, 0.f, 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            const GLint values[] = { v }; // [Viewer Lab]
            sendUniform(GLUniformCall::Uniform1i, location, 1, GL_FALSE, values); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform1iv(const LLStaticHashedString& uniform, U32 count, const GLint* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)v[0], 0.f, 0.f, 0.f);
        const auto& iter = mValue.find(location);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            sendUniform(GLUniformCall::Uniform1iv, location, count, GL_FALSE, v); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform4iv(const LLStaticHashedString& uniform, U32 count, const GLint* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
        const auto& iter = mValue.find(location);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            sendUniform(GLUniformCall::Uniform4iv, location, count, GL_FALSE, v); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform2i(const LLStaticHashedString& uniform, GLint i, GLint j)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec((F32)i, (F32)j, 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            const GLint values[] = { i, j }; // [Viewer Lab]
            sendUniform(GLUniformCall::Uniform2i, location, 1, GL_FALSE, values); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}


void LLGLSLShader::uniform1f(const LLStaticHashedString& uniform, GLfloat v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(v, 0.f, 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            const GLfloat values[] = { v }; // [Viewer Lab]
            sendUniform(GLUniformCall::Uniform1f, location, 1, GL_FALSE, values); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform2f(const LLStaticHashedString& uniform, GLfloat x, GLfloat y)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(x, y, 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            const GLfloat values[] = { x, y }; // [Viewer Lab]
            sendUniform(GLUniformCall::Uniform2f, location, 1, GL_FALSE, values); // [Viewer Lab]
            mValue[location] = vec;
        }
    }

}

void LLGLSLShader::uniform3f(const LLStaticHashedString& uniform, GLfloat x, GLfloat y, GLfloat z)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(x, y, z, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            const GLfloat values[] = { x, y, z }; // [Viewer Lab]
            sendUniform(GLUniformCall::Uniform3f, location, 1, GL_FALSE, values); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform4f(const LLStaticHashedString& uniform, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(x, y, z, w);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            const GLfloat values[] = { x, y, z, w }; // [Viewer Lab]
            sendUniform(GLUniformCall::Uniform4f, location, 1, GL_FALSE, values); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform1fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(v[0], 0.f, 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            sendUniform(GLUniformCall::Uniform1fv, location, count, GL_FALSE, v); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform2fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(v[0], v[1], 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            sendUniform(GLUniformCall::Uniform2fv, location, count, GL_FALSE, v); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform3fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(v[0], v[1], v[2], 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            sendUniform(GLUniformCall::Uniform3fv, location, count, GL_FALSE, v); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform4fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec(v);
        const auto& iter = mValue.find(location);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            sendUniform(GLUniformCall::Uniform4fv, location, count, GL_FALSE, v); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform4uiv(const LLStaticHashedString& uniform, U32 count, const GLuint* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
        const auto& iter = mValue.find(location);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            sendUniform(GLUniformCall::Uniform4uiv, location, count, GL_FALSE, v); // [Viewer Lab]
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniformMatrix4fv(const LLStaticHashedString& uniform, U32 count, GLboolean transpose, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        stop_glerror();
        sendUniform(GLUniformCall::Matrix4fv, location, count, transpose, v); // [Viewer Lab]
        stop_glerror();
    }
}

// <Luminous> by name, as uniformMatrix4fv: the encoder gets the matrices column-major when transpose is asked
void LLGLSLShader::uniformMatrix3fv(const LLStaticHashedString& uniform, U32 count, GLboolean transpose, const GLfloat* v)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        sendUniform(GLUniformCall::Matrix3fv, location, count, transpose, v);
    }
}
// </Luminous>

void LLGLSLShader::setMinimumAlpha(F32 minimum)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    gGL.flush();
    uniform1f(LLShaderMgr::MINIMUM_ALPHA, minimum);
}

void LLShaderUniforms::apply(LLGLSLShader* shader)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    for (auto& uniform : mIntegers)
    {
        shader->uniform1i(uniform.mUniform, uniform.mValue);
    }

    for (auto& uniform : mFloats)
    {
        shader->uniform1f(uniform.mUniform, uniform.mValue);
    }

    for (auto& uniform : mVectors)
    {
        shader->uniform4fv(uniform.mUniform, 1, uniform.mValue.mV);
    }

    for (auto& uniform : mVector3s)
    {
        shader->uniform3fv(uniform.mUniform, 1, uniform.mValue.mV);
    }
}

// [Viewer Lab] LLShaderUniformBuffer -----------------------------------------------------------------------------

// The viewer's device on the main thread while it is the one with that id, otherwise null.
static LL::RHI::Device* uniform_buffer_device(U64 device_id)
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    return device && on_main_thread() && device->id() == device_id ? device : nullptr;
}

// Lets go of a buffer as release_rhi_program lets go of a program: on the main thread while the device that made it is the
// viewer's; otherwise it is kept, never released, so no render interface object goes in static destruction or to a device
// that is gone.
static void release_rhi_buffer(LLPointer<LL::RHI::Buffer>& buffer, U64 device_id)
{
    if (buffer.notNull())
    {
        if (!uniform_buffer_device(device_id))
        {
            buffer->ref();
        }
        buffer = nullptr;
    }
}

// A block's claim, for an undeclared block: the first LLShaderUniformBuffer of a block binds it at the render interface's
// point in every live program, and the last one's release binds it back at its UB_* point. A declared block is read there
// from every program's load, so its claim rebinds nothing; the count stays, so a later forget is exact.
static void claim_interface_block(LLGLSLShader::UniformBlock block)
{
    if (++sInterfaceBlockBuffers[block] == 1 && !(sDeclaredBlocks & (1u << block)))
    {
        rebind_interface_block(block);
    }
}

static void drop_interface_block(LLGLSLShader::UniformBlock block)
{
    if (sInterfaceBlockBuffers[block] > 0 && --sInterfaceBlockBuffers[block] == 0 && !(sDeclaredBlocks & (1u << block)))
    {
        rebind_interface_block(block);
    }
}

LLShaderUniformBuffer::LLShaderUniformBuffer() = default;

LLShaderUniformBuffer::~LLShaderUniformBuffer()
{
    release();
}

// static
void LLShaderUniformBuffer::declareBlock(LLGLSLShader::UniformBlock block)
{
    if (block >= LLGLSLShader::NUM_UNIFORM_BLOCKS || (sDeclaredBlocks & (1u << block)))
    {
        return;
    }
    sDeclaredBlocks |= 1u << block;
    // The device first, so a declaration in static initialization touches neither LLGLSLShader::sInstances nor the log. A
    // block with a buffer is read at the render interface's point already.
    if (!LL::RHI::viewerDevice() || !on_main_thread() || sInterfaceBlockBuffers[block] > 0)
    {
        return;
    }
    for (LLGLSLShader* shader : LLGLSLShader::sInstances)
    {
        if (shader->mProgramObject && shader->isReflectionReady())
        {
            LL_WARNS_ONCE("RHI") << "uniform block " << LLGLSLShader::uniformBlockName(block)
                                 << " declared after programs were mapped" << LL_ENDL;
            rebind_interface_block(block);
            return;
        }
    }
}

// [Viewer Lab]
// static
U32 LLShaderUniformBuffer::declaredBlocks()
{
    return sDeclaredBlocks;
}

// [Viewer Lab]
// static
U32 LLShaderUniformBuffer::programSweeps()
{
    return sProgramSweeps;
}

// static
void LLShaderUniformBuffer::forgetDeclaredBlocksForTest()
{
    const U32 declared = sDeclaredBlocks;
    sDeclaredBlocks    = 0;
    for (U32 i = 0; i < LLGLSLShader::NUM_UNIFORM_BLOCKS; ++i)
    {
        if ((declared & (1u << i)) && sInterfaceBlockBuffers[i] == 0)
        {
            rebind_interface_block(LLGLSLShader::UniformBlock(i));
        }
    }
}

bool LLShaderUniformBuffer::allocate(LLGLSLShader::UniformBlock block, U32 size, bool dynamic, const char* name)
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || !on_main_thread() || size == 0 || block >= LLGLSLShader::NUM_UNIFORM_BLOCKS)
    {
        return false;
    }
    LL::RHI::BufferDesc desc;
    desc.size    = size;
    desc.usage   = LL::RHI::BUFFER_UNIFORM;
    desc.dynamic = dynamic;
    LLPointer<LL::RHI::Buffer> buffer = device->createBuffer(desc, {}, name ? name : LLGLSLShader::uniformBlockName(block));
    if (buffer.isNull())
    {
        return false; // as it was
    }

    // The new block is claimed before the old buffer goes, so a re-allocation for the same block rebinds nothing, and one
    // for another block never leaves a program between the two
    const bool                       allocated = mBuffer.notNull();
    const LLGLSLShader::UniformBlock previous  = mBlock;
    if (!allocated || previous != block)
    {
        claim_interface_block(block);
    }
    std::swap(mBuffer, buffer);
    release_rhi_buffer(buffer, mDevice);
    mBlock  = block;
    mSize   = size;
    mDevice = device->id();
    if (allocated && previous != block)
    {
        drop_interface_block(previous);
    }
    return true;
}

void LLShaderUniformBuffer::update(U32 offset, const void* data, U32 size)
{
    LL::RHI::Device* device = mBuffer.notNull() && size > 0 ? uniform_buffer_device(mDevice) : nullptr;
    if (device)
    {
        device->encoder().updateBuffer(mBuffer, offset, std::span<const std::byte>(static_cast<const std::byte*>(data), size));
    }
}

void LLShaderUniformBuffer::bind() const
{
    LL::RHI::Device* device = mBuffer.notNull() ? uniform_buffer_device(mDevice) : nullptr;
    if (device)
    {
        LLRenderTarget::beginStackPass();
        device->encoder().setUniformBuffer(mBlock, mBuffer, 0, mSize);
    }
}

void LLShaderUniformBuffer::release()
{
    if (mBuffer.isNull())
    {
        return;
    }
    // Main thread only. Elsewhere the programs cannot be bound again and the count is the main thread's, so the buffer is
    // kept unreleased and the block keeps its claim: every program goes on reading the block at one point (as
    // llimagegl.cpp's keep_unreleased keeps a texture).
    llassert(on_main_thread());
    release_rhi_buffer(mBuffer, mDevice);
    const LLGLSLShader::UniformBlock block = mBlock;
    mBlock  = LLGLSLShader::NUM_UNIFORM_BLOCKS;
    mSize   = 0;
    mDevice = 0;
    if (on_main_thread())
    {
        drop_interface_block(block);
    }
}

bool LLShaderUniformBuffer::isAllocated() const
{
    return mBuffer.notNull();
}

LL::RHI::Buffer* LLShaderUniformBuffer::getRHIBuffer() const
{
    return mBuffer.get();
}

// [Viewer Lab] LLGPUQuery -----------------------------------------------------------------------------------------

// On the main thread, true; elsewhere false, with a WARN line: queries belong to the main thread's context.
static bool gpu_query_on_main_thread(const char* call)
{
    if (on_main_thread())
    {
        return true;
    }
    LL_WARNS("RHI") << "LLGPUQuery::" << call << " off the main thread: queries are made and used on the main thread only"
                    << LL_ENDL;
    return false;
}

// The viewer's device while it is the one with that id, otherwise null (called each time, never kept).
static LL::RHI::Device* gpu_query_device(U64 device_id)
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    return device && device->id() == device_id ? device : nullptr;
}

// Whether a GL target is the query's kind; otherwise false, with a WARN line.
static bool gpu_query_target(const char* call, U32 target, LL::RHI::QueryType type)
{
    if (LL::RHI::queryTypeFromGL(target) == type)
    {
        return true;
    }
    LL_WARNS("RHI") << "LLGPUQuery::" << call << ": the target " << llformat("0x%04x", target) << " is not the query's"
                    << LL_ENDL;
    return false;
}

LLGPUQuery::LLGPUQuery() = default;

LLGPUQuery::~LLGPUQuery()
{
    release();
}

LLGPUQuery::LLGPUQuery(const LLGPUQuery&) = default; // shares the query, as a copied name did

LLGPUQuery& LLGPUQuery::operator=(const LLGPUQuery& other)
{
    if (this != &other)
    {
        release();
        mQuery  = other.mQuery;
        mDevice = other.mDevice;
        mActive = other.mActive;
    }
    return *this;
}

LLGPUQuery::LLGPUQuery(LLGPUQuery&& other) noexcept
:   mQuery(std::move(other.mQuery)), mDevice(other.mDevice), mActive(other.mActive)
{
    other.mDevice = 0;
    other.mActive = false;
}

LLGPUQuery& LLGPUQuery::operator=(LLGPUQuery&& other) noexcept
{
    if (this != &other)
    {
        release();
        mQuery  = std::move(other.mQuery);
        mDevice = other.mDevice;
        mActive = other.mActive;
        other.mDevice = 0;
        other.mActive = false;
    }
    return *this;
}

// static
LLGPUQuery LLGPUQuery::create(U32 target)
{
    std::vector<LLGPUQuery> queries;
    create(target, 1, queries);
    return queries.empty() ? LLGPUQuery() : std::move(queries.front());
}

// static
void LLGPUQuery::create(U32 target, U32 count, std::vector<LLGPUQuery>& queries)
{
    if (!gpu_query_on_main_thread("create"))
    {
        return;
    }
    const std::optional<LL::RHI::QueryType> type = LL::RHI::queryTypeFromGL(target);
    if (!type)
    {
        LL_WARNS("RHI") << "LLGPUQuery::create: no kind of query has the target " << llformat("0x%04x", target) << LL_ENDL;
        return;
    }
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || !LL::RHI::validateQuery(*type, device->capabilities()).empty())
    {
        return; // headless, or a kind the device lacks, which the profiling copes without
    }
    std::vector<LL::RHI::Ref<LL::RHI::Query>> made = device->createQueries(*type, count);
    queries.reserve(queries.size() + made.size());
    for (LL::RHI::Ref<LL::RHI::Query>& query : made)
    {
        LLGPUQuery facade;
        facade.mQuery  = std::move(query);
        facade.mDevice = device->id();
        queries.push_back(std::move(facade));
    }
}

// static
bool LLGPUQuery::supported(U32 target)
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    const std::optional<LL::RHI::QueryType> type = LL::RHI::queryTypeFromGL(target);
    return device && type && LL::RHI::validateQuery(*type, device->capabilities()).empty();
}

LLGPUQuery::operator bool() const
{
    return mQuery.notNull() && gpu_query_device(mDevice);
}

bool LLGPUQuery::begin(U32 target)
{
    if (!gpu_query_on_main_thread("begin"))
    {
        return false;
    }
    LL::RHI::Device* device = mQuery.notNull() ? gpu_query_device(mDevice) : nullptr;
    if (!device || !gpu_query_target("begin", target, mQuery->type()))
    {
        return false; // none, as GL's name 0, or another device's
    }
    // A refused begin of this very query leaves it active, as GL keeps a query after a refused glBeginQuery: its end still
    // ends it.
    const bool begun = device->encoder().beginQuery(mQuery.get());
    mActive = mActive || begun;
    return begun;
}

void LLGPUQuery::end(U32 target)
{
    if (!gpu_query_on_main_thread("end"))
    {
        return;
    }
    LL::RHI::Device* device = mQuery.notNull() ? gpu_query_device(mDevice) : nullptr;
    if (!device || !gpu_query_target("end", target, mQuery->type()) || !mActive)
    {
        return; // a query never begun, or one the encoder refused, has nothing to end
    }
    mActive = false;
    device->encoder().endQuery(mQuery.get());
}

U64 LLGPUQuery::get(U32 pname) const
{
    if (!gpu_query_on_main_thread("get"))
    {
        return 0;
    }
    if (mQuery.isNull() || !gpu_query_device(mDevice))
    {
        return 0; // never made, as GL's name 0: nothing to read
    }
    switch (pname)
    {
    case GL_QUERY_RESULT_AVAILABLE:
        return mQuery.get()->ready() ? GL_TRUE : GL_FALSE;
    case GL_QUERY_RESULT:
        return mQuery.get()->result(true);
    default:
        LL_WARNS("RHI") << "LLGPUQuery::get: no value " << llformat("0x%04x", pname) << LL_ENDL;
        return 0;
    }
}

void LLGPUQuery::release()
{
    if (mQuery.notNull())
    {
        if (!on_main_thread() || !gpu_query_device(mDevice))
        {
            mQuery->ref(); // kept unreleased, with no GL call off the main thread or after its device
        }
        mQuery = nullptr;
    }
    mDevice = 0;
    mActive = false;
}

void LLGPUQuery::keep()
{
    if (mQuery.notNull())
    {
        mQuery->ref(); // never released: GL keeps the name, as a raw name nobody deletes
        mQuery = nullptr;
    }
    mDevice = 0;
    mActive = false;
}

U64 LLGPUQuery::id() const
{
    return mQuery.notNull() ? mQuery->id() : 0;
}

LLUUID LLGLSLShader::hash()
{
    HBXXH128 hash_obj;
    hash_obj.update(mName);
    hash_obj.update(&mShaderGroup, sizeof(mShaderGroup));
    hash_obj.update(&mShaderLevel, sizeof(mShaderLevel));
    for (const auto& shdr_pair : mShaderFiles)
    {
        hash_obj.update(shdr_pair.first);
        hash_obj.update(&shdr_pair.second, sizeof(GLenum));
    }
    for (const auto& define_pair : mDefines)
    {
        hash_obj.update(define_pair.first);
        hash_obj.update(define_pair.second);

    }
    for (const auto& define_pair : LLGLSLShader::sGlobalDefines)
    {
        hash_obj.update(define_pair.first);
        hash_obj.update(define_pair.second);

    }
    hash_obj.update(&mFeatures, sizeof(LLShaderFeatures));
    hash_obj.update(mSourceHash.mData, UUID_BYTES); // [Viewer Lab] the GLSL the program is built from
    hash_obj.update(gGLManager.mGLVendor);
    hash_obj.update(gGLManager.mGLRenderer);
    hash_obj.update(gGLManager.mGLVersionString);
    return hash_obj.digest();
}

#if LL_PROFILER_ENABLE_RENDER_DOC
void LLGLSLShader::setLabel(const char* label) {
    if (mRHIProgram.notNull()) // [Viewer Lab]
    {
        mRHIProgram->setDebugName(label);
    }
    LL_LABEL_OBJECT_GL(GL_PROGRAM, mProgramObject, (GLsizei)strlen(label), label); // [Viewer Lab] (GLsizei): C4267 under /WX
}
#endif
