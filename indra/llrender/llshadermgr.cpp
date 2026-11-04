/**
 * @file llshadermgr.cpp
 * @brief Shader manager implementation.
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
#include "llshadermgr.h"
#include "llrender.h"
#include "llrhi.h" // [Viewer Lab]
#include "llfile.h"
#include "lldir.h"
#include "llsdutil.h"
#include "llsdserialize.h"
#include "hbxxh.h"
#include "llsdjson.h"

#include <boost/json/serialize.hpp>

#if LL_DARWIN
#include "OpenGL/OpenGL.h"
#endif

// Lots of STL stuff in here, using namespace std to keep things more readable
using std::vector;
using std::pair;
using std::make_pair;
using std::string;

LLShaderMgr * LLShaderMgr::sInstance = NULL;

LLShaderMgr::LLShaderMgr()
{
}


LLShaderMgr::~LLShaderMgr()
{
}

// static
LLShaderMgr * LLShaderMgr::instance()
{
    if(NULL == sInstance)
    {
        LL_ERRS("Shaders") << "LLShaderMgr should already have been instantiated by the application!" << LL_ENDL;
    }

    return sInstance;
}

bool LLShaderMgr::attachShaderFeatures(LLGLSLShader * shader)
{
    llassert_always(shader != NULL);
    LLShaderFeatures *features = & shader->mFeatures;

    if (!mCollecting && !mManifestFolder.empty() && !shader->mUsingBinaryProgram) // [Viewer Lab]
    {
        // [Viewer Lab] Shader manifest: the program's own files, which createShader() compiled and
        // attached just before this, come first. A later attempt after a failed link starts over.
        ManifestProgram& program = mManifestAttached[shader];
        if (program.mAttempts++ == 0)
        {
            program.mFirstLevel = shader->mShaderLevel;
        }
        program.mAttached.clear();
        for (const auto& [file, type] : shader->mShaderFiles)
        {
            const auto found = mManifestCompiled.find({ file, type });
            if (found != mManifestCompiled.end())
            {
                program.mAttached.push_back(found->second);
            }
            else
            {
                ManifestObject missing; // shows as a missing source
                missing.mFile = file;
                missing.mType = type;
                program.mAttached.push_back(missing);
            }
        }
    }

    if (features->attachNothing)
    {
        return true;
    }
    //////////////////////////////////////
    // Attach Vertex Shader Features First
    //////////////////////////////////////

    // NOTE order of shader object attaching is VERY IMPORTANT!!!
    if (features->calculatesAtmospherics || features->hasGamma || features->isDeferred)
    {
        if (!attachVertexObject(shader, "windlight/atmosphericsVarsV.glsl"))
        {
            return false;
        }
    }

    if (features->calculatesLighting || features->calculatesAtmospherics)
    {
        if (!attachVertexObject(shader, "windlight/atmosphericsHelpersV.glsl"))
        {
            return false;
        }
    }

    if (features->calculatesLighting)
    {
        if (features->isSpecular)
        {
            if (!attachVertexObject(shader, "lighting/lightFuncSpecularV.glsl"))
            {
                return false;
            }

            if (!features->isAlphaLighting)
            {
                if (!attachVertexObject(shader, "lighting/sumLightsSpecularV.glsl"))
                {
                    return false;
                }
            }

            if (!attachVertexObject(shader, "lighting/lightSpecularV.glsl"))
            {
                return false;
            }
        }
        else
        {
            if (!attachVertexObject(shader, "lighting/lightFuncV.glsl"))
            {
                return false;
            }

            if (!features->isAlphaLighting)
            {
                if (!attachVertexObject(shader, "lighting/sumLightsV.glsl"))
                {
                    return false;
                }
            }

            if (!attachVertexObject(shader, "lighting/lightV.glsl"))
            {
                return false;
            }
        }
    }

    // NOTE order of shader object attaching is VERY IMPORTANT!!!
    if (features->calculatesAtmospherics)
    {
        if (!attachVertexObject(shader, "environment/srgbF.glsl")) // NOTE -- "F" suffix is superfluous here, there is nothing fragment specific in srgbF
        {
            return false;
        }

        if (!attachVertexObject(shader, "windlight/atmosphericsFuncs.glsl")) {
            return false;
        }

        if (!attachVertexObject(shader, "windlight/atmosphericsV.glsl"))
        {
            return false;
        }
    }

    if (features->hasSkinning)
    {
        if (!attachVertexObject(shader, "avatar/avatarSkinV.glsl"))
        {
            return false;
        }
    }

    if (features->hasObjectSkinning)
    {
        shader->mRiggedVariant = shader;
        if (!attachVertexObject(shader, "avatar/objectSkinV.glsl"))
        {
            return false;
        }
    }

    if (!attachVertexObject(shader, "deferred/textureUtilV.glsl"))
    {
        return false;
    }

    ///////////////////////////////////////
    // Attach Fragment Shader Features Next
    ///////////////////////////////////////

    // NOTE order of shader object attaching is VERY IMPORTANT!!!

    if (!attachFragmentObject(shader, "deferred/globalF.glsl"))
    {
        return false;
    }

    if (features->hasSrgb || features->hasAtmospherics || features->calculatesAtmospherics || features->isDeferred)
    {
        if (!attachFragmentObject(shader, "environment/srgbF.glsl"))
        {
            return false;
        }
    }

    if(features->calculatesAtmospherics || features->hasGamma || features->isDeferred)
    {
        if (!attachFragmentObject(shader, "windlight/atmosphericsVarsF.glsl"))
        {
            return false;
        }
    }

    if (features->calculatesLighting || features->calculatesAtmospherics)
    {
        if (!attachFragmentObject(shader, "windlight/atmosphericsHelpersF.glsl"))
        {
            return false;
        }
    }

    // we want this BEFORE shadows and AO because those facilities use pos/norm access
    if (features->isDeferred || features->hasReflectionProbes)
    {
        if (!attachFragmentObject(shader, "deferred/deferredUtil.glsl"))
        {
            return false;
        }
    }

    if (features->hasFullGBuffer)
    {
        if (!attachFragmentObject(shader, "deferred/gbufferUtil.glsl"))
        {
            return false;
        }
    }

    if (features->hasScreenSpaceReflections || features->hasReflectionProbes)
    {
        if (!attachFragmentObject(shader, "deferred/screenSpaceReflUtil.glsl"))
        {
            return false;
        }
    }

    if (features->hasShadows)
    {
        if (!attachFragmentObject(shader, "deferred/shadowUtil.glsl"))
        {
            return false;
        }
    }

    if (features->hasReflectionProbes)
    {
        if (!attachFragmentObject(shader, "deferred/reflectionProbeF.glsl"))
        {
            return false;
        }
    }

    if (features->hasAmbientOcclusion)
    {
        if (!attachFragmentObject(shader, "deferred/aoUtil.glsl"))
        {
            return false;
        }
    }

    if (features->hasGamma || features->isDeferred)
    {
        if (!attachFragmentObject(shader, "windlight/gammaF.glsl"))
        {
            return false;
        }
    }

    if (features->hasAtmospherics || features->isDeferred)
    {
        if (!attachFragmentObject(shader, "windlight/atmosphericsFuncs.glsl")) {
            return false;
        }

        if (!attachFragmentObject(shader, "windlight/atmosphericsF.glsl"))
        {
            return false;
        }
    }

    if (features->isPBRTerrain)
    {
        if (!attachFragmentObject(shader, "deferred/pbrterrainUtilF.glsl"))
        {
            return false;
        }
    }

    if (features->hasTonemap)
    {
        if (!attachFragmentObject(shader, "deferred/tonemapUtilF.glsl"))
        {
            return false;
        }
        // <Luminous> Worn photo, which the Look's finish calls
        if (!attachFragmentObject(shader, "deferred/luminousWornF.glsl"))
        {
            return false;
        }
        // </Luminous>
    }

    // NOTE order of shader object attaching is VERY IMPORTANT!!!
    if (features->hasAtmospherics)
    {
        if (!attachFragmentObject(shader, "environment/waterFogF.glsl"))
        {
            return false;
        }
    }

    if (features->hasLighting)
    {
        if (features->mIndexedTextureChannels <= 1)
        {
            if (features->hasAlphaMask)
            {
                if (!attachFragmentObject(shader, "lighting/lightAlphaMaskNonIndexedF.glsl"))
                {
                    return false;
                }
            }
            else
            {
                if (!attachFragmentObject(shader, "lighting/lightNonIndexedF.glsl"))
                {
                    return false;
                }
            }
        }
        else
        {
            if (features->hasAlphaMask)
            {
                if (!attachFragmentObject(shader, "lighting/lightAlphaMaskF.glsl"))
                {
                    return false;
                }
            }
            else
            {
                if (!attachFragmentObject(shader, "lighting/lightF.glsl"))
                {
                    return false;
                }
            }
            shader->mFeatures.mIndexedTextureChannels = llmax(LLGLSLShader::sIndexedTextureChannels, 1);
        }
    }

    if (features->mIndexedTextureChannels <= 1)
    {
        if (!attachVertexObject(shader, "objects/nonindexedTextureV.glsl"))
        {
            return false;
        }
    }
    else
    {
        if (!attachVertexObject(shader, "objects/indexedTextureV.glsl"))
        {
            return false;
        }
    }

    return true;
}

//============================================================================
// Load Shader

static std::string get_shader_log(GLuint ret)
{
    std::string res;

    //get log length
    GLint length;
    glGetShaderiv(ret, GL_INFO_LOG_LENGTH, &length);
    if (length > 0)
    {
        //the log could be any size, so allocate appropriately
        GLchar* log = new GLchar[length];
        glGetShaderInfoLog(ret, length, &length, log);
        res = std::string((char *)log);
        delete[] log;
    }
    // <FS:LO> Fix intel GLSL compiler spitting out "No errors." instead of an empty string like others do when there are no errors, causing log spam.
    if(!strcmp(res.c_str(),"No errors.\n"))
    {
        res = "";
    }
    // </FS:LO>
    return res;
}

static std::string get_program_log(GLuint ret)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    std::string res;

    //get log length
    GLint length;
    glGetProgramiv(ret, GL_INFO_LOG_LENGTH, &length);
    if (length > 0)
    {
        //the log could be any size, so allocate appropriately
        GLchar* log = new GLchar[length];
        glGetProgramInfoLog(ret, length, &length, log);
        res = std::string((char*)log);
        delete[] log;
    }
    return res;
}

// get the info log for the given object, be it a shader or program object
// NOTE: ret MUST be a shader OR a program object
static std::string get_object_log(GLuint ret)
{
    if (glIsProgram(ret))
    {
        return get_program_log(ret);
    }
    else
    {
        llassert(glIsShader(ret));
        return get_shader_log(ret);
    }
}

//dump shader source for debugging
void LLShaderMgr::dumpShaderSource(U32 shader_code_count, GLchar** shader_code_text)
{
    char num_str[16]; // U32 = max 10 digits

    LL_SHADER_LOADING_WARNS() << "\n";

    for (U32 i = 0; i < shader_code_count; i++)
    {
        snprintf(num_str, sizeof(num_str), "%4d: ", i+1);
        std::string line_number(num_str);
        LL_CONT << line_number << shader_code_text[i];
    }
    LL_CONT << LL_ENDL;
}

void LLShaderMgr::dumpObjectLog(GLuint ret, bool warns, const std::string& filename)
{
    std::string log;
    log = get_object_log(ret);
    std::string fname = filename;
    if (filename.empty())
    {
        fname = "unknown shader file";
    }

    if (log.length() > 0)
    {
        LL_SHADER_LOADING_WARNS() << "Shader loading from " << fname << LL_ENDL;
        LL_SHADER_LOADING_WARNS() << "\n" << log << LL_ENDL;
    }
 }

// [Viewer Lab] LLShaderMgr's GL backend functions. Each holds today's GL calls
// and does nothing, or gives the neutral value, when GL is not the backend chosen at startup, as llimagegl.cpp's.
static bool gl_is_backend()
{
    return LL::RHI::viewerBackend() == LL::RHI::BackendType::GL;
}

// loadShaderFile's check for a GL error on entry.
static GLenum gl_get_error()
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llshadermgr:gl_get_error");
        return GL_NO_ERROR;
    }
    return glGetError();
}

// loadShaderFile's shader object from the assembled lines, with its GL error checks, the compile status, the log and
// source dump and the deletion on a failure. On another backend the lines count as compiled, with no GL object.
static bool gl_compile_shader(LLShaderMgr& mgr, GLenum type, GLuint shader_code_count, GLchar** shader_code_text,
                              const std::string& open_file_name, GLuint& ret)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llshadermgr:gl_compile_shader");
        ret = 0;
        return true;
    }
    GLenum error = GL_NO_ERROR;

    //create shader object
    ret = glCreateShader(type);

    error = glGetError();
    if (error != GL_NO_ERROR)
    {
        LL_WARNS("ShaderLoading") << "GL ERROR in glCreateShader: " << error << " for file: " << open_file_name << LL_ENDL;
        if (ret)
        {
            glDeleteShader(ret); //no longer need handle
            ret = 0;
        }
    }

    //load source
    if (ret)
    {
        LL_DEBUGS("ShaderLoading") << "glCreateShader done" << LL_ENDL;
        glShaderSource(ret, shader_code_count, (const GLchar**)shader_code_text, NULL);

        error = glGetError();
        if (error != GL_NO_ERROR)
        {
            LL_WARNS("ShaderLoading") << "GL ERROR in glShaderSource: " << error << " for file: " << open_file_name << LL_ENDL;
            glDeleteShader(ret); //no longer need handle
            ret = 0;
        }
    }

    //compile source
    if (ret)
    {
        LL_DEBUGS("ShaderLoading") << "glShaderSource done" << U32(ret) << LL_ENDL;
        glCompileShader(ret);

        error = glGetError();
        if (error != GL_NO_ERROR)
        {
            LL_WARNS("ShaderLoading") << "GL ERROR in glCompileShader: " << error << " for file: " << open_file_name << LL_ENDL;
            glDeleteShader(ret); //no longer need handle
            ret = 0;
        }
    }

    if (error == GL_NO_ERROR)
    {
        //check for errors
        LL_DEBUGS("ShaderLoading") << "glCompileShader done" << U32(ret) << LL_ENDL;
        GLint success = GL_TRUE;
        glGetShaderiv(ret, GL_COMPILE_STATUS, &success);

        error = glGetError();
        if (error != GL_NO_ERROR || success == GL_FALSE)
        {
            //an error occured, print log
            LL_WARNS("ShaderLoading") << "GLSL Compilation Error:" << LL_ENDL;
            mgr.dumpObjectLog(ret, true, open_file_name);
            mgr.dumpShaderSource(shader_code_count, shader_code_text);
            glDeleteShader(ret); //no longer need handle
            ret = 0;
        }
    }
    else
    {
        LL_DEBUGS("ShaderLoading") << "loadShaderFile() completed, ret: " << U32(ret) << LL_ENDL;
        ret = 0;
    }
    return ret != 0;
}

// [Viewer Lab]
//static
LLShaderMgr::ShaderWorkarounds LLShaderMgr::shaderWorkarounds()
{
    ShaderWorkarounds workarounds;
    if (LL::RHI::Device* device = LL::RHI::viewerDevice())
    {
        const LL::RHI::Capabilities& caps = device->capabilities();
        workarounds.oldSelect = caps.shaderOldSelect;
        workarounds.amdCard = caps.shaderAMDCard;
        workarounds.ifChain = caps.shaderIfChain;
        return workarounds;
    }
#if LL_DARWIN
    workarounds.oldSelect = !gGLManager.mIsApple;
#endif
    workarounds.amdCard = gGLManager.mIsAMD;
    workarounds.ifChain = gGLManager.mIsNVIDIA;
    return workarounds;
}

// [Viewer Lab]
LLShaderMgr::ShaderSource::ShaderSource(ShaderSource&& other) noexcept
{
    *this = std::move(other);
}

LLShaderMgr::ShaderSource& LLShaderMgr::ShaderSource::operator=(ShaderSource&& other) noexcept
{
    if (this != &other)
    {
        clear();
        mFile           = std::move(other.mFile);
        mType           = other.mType;
        mOpenFileName   = std::move(other.mOpenFileName);
        mRequestedClass = other.mRequestedClass;
        mFoundClass     = other.mFoundClass;
        mFound          = other.mFound;
        mCompiled       = other.mCompiled;
        mLines.swap(other.mLines);
        other.clear();
    }
    return *this;
}

LLShaderMgr::ShaderSource::~ShaderSource()
{
    clear();
}

// [Viewer Lab] Joined once and kept until clear(), so that the hash, the description and the object's text walk the lines
// once per createShader, not three times; a move clears both sides, so the moved-to source joins the moved lines when it is
// read. Joined again when the number of lines changed since, so a line added or removed without clear() is
// never left out of the hash (O(1) per call).
const std::string& LLShaderMgr::ShaderSource::text() const
{
    if (!mTextReady || mTextLines != mLines.size())
    {
        mText.clear();
        for (const GLchar* line : mLines)
        {
            mText += line;
        }
        mTextLines = mLines.size();
        mTextReady = true;
    }
    return mText;
}

void LLShaderMgr::ShaderSource::clear()
{
    for (GLchar* line : mLines)
    {
        free(line);
    }
    mLines.clear();
    mText.clear(); // [Viewer Lab]
    mTextReady = false;
    mTextLines = 0; // [Viewer Lab]
    mFile.clear();
    mType = 0;
    mOpenFileName.clear();
    mRequestedClass = 0;
    mFoundClass     = 0;
    mFound          = false;
    mCompiled       = false;
}

// [Viewer Lab] loadShaderFile's assembly of a file's lines, moved here as it was without the compile, so that
// assembleProgram can assemble the whole program before createShader's first GL call
bool LLShaderMgr::assembleShaderSource(const std::string& filename, S32 shader_level, GLenum type,
                                       std::map<std::string, std::string>* defines, S32 texture_index_channels, ShaderSource& source,
                                       bool warn)
{
    source.clear();
    source.mFile = filename;
    source.mType = type;

    //read in from file
    LLFILE* file = NULL;

    S32 try_gpu_class = shader_level;
    S32 gpu_class;

    std::string open_file_name;

#if 0  // WIP -- try to come up with a way to fallback to an error shader without needing debug stubs all over the place in the shader tree
    if (shader_level == -1)
    {
        // use "error" fallback
        if (type == GL_VERTEX_SHADER)
        {
            open_file_name = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders/errorV.glsl");
        }
        else
        {
            llassert(type == GL_FRAGMENT_SHADER);  // type must be vertex or fragment shader
            open_file_name = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders/errorF.glsl");
        }

        file = LLFile::fopen(open_file_name, "r");
    }
    else
#endif
    {
        //find the most relevant file
        for (gpu_class = try_gpu_class; gpu_class > 0; gpu_class--)
        {   //search from the current gpu class down to class 1 to find the most relevant shader
            std::stringstream fname;
            fname << getShaderDirPrefix();
            fname << gpu_class << gDirUtilp->getDirDelimiter() << filename;

            open_file_name = fname.str();

            /*
            Would be awesome, if we didn't have shaders that re-use files
            with different environments to say, add skinning, etc
            can't depend on cached version to have evaluate ifdefs identically...
            if we can define a deterministic hash for the shader based on
            all the inputs, maybe we can save some time here.
            if (mShaderObjects.count(filename) > 0)
            {
                return mShaderObjects[filename];
            }

            */

            LL_DEBUGS("ShaderLoading") << "Looking in " << open_file_name << LL_ENDL;
            file = LLFile::fopen(open_file_name, "r");      /* Flawfinder: ignore */
            if (file)
            {
                LL_DEBUGS("ShaderLoading") << "Loading file: " << open_file_name << " (Want class " << gpu_class << ")" << LL_ENDL;
                break; // done
            }
        }
    }

    if (file == NULL)
    {
        if (!warn) // [Viewer Lab] assembleProgram's look-ahead; loadShaderFile warns when it assembles the file itself
        {
            return false;
        }
        if (gDirUtilp->fileExists(open_file_name))
        {
            LL_WARNS("ShaderLoading") << "GLSL Shader file failed to open: " << open_file_name << LL_ENDL;
        }
        else
        {
            LL_WARNS("ShaderLoading") << "GLSL Shader file not found: " << open_file_name << LL_ENDL;
        }
        return 0;
    }

    //we can't have any lines longer than 1024 characters
    //or any shaders longer than 4096 lines... deal - DaveP
    GLchar buff[1024];
    GLchar *extra_code_text[1024];
    GLchar *shader_code_text[4096 + LL_ARRAY_SIZE(extra_code_text)] = { NULL };
    GLuint extra_code_count = 0, shader_code_count = 0;
    BOOST_STATIC_ASSERT(LL_ARRAY_SIZE(extra_code_text) < LL_ARRAY_SIZE(shader_code_text));


    S32 major_version = gGLManager.mGLSLVersionMajor;
    S32 minor_version = gGLManager.mGLSLVersionMinor;

    if (major_version == 1 && minor_version < 30)
    {
        llassert(false); // GL 3.1 or later required
    }
    else
    {
        if (major_version >= 4)
        {
            //set version to 400 or 420
            if (minor_version >= 20)
            {
                shader_code_text[shader_code_count++] = strdup("#version 420\n");
            }
            else
            {
                shader_code_text[shader_code_count++] = strdup("#version 400\n");
            }
        }
        else if (major_version == 3)
        {
            if (minor_version <= 29)
            {
                // OpenGL 3.2 had GLSL version 1.50.  anything after that the version numbers match.
                // https://www.khronos.org/opengl/wiki/Core_Language_(GLSL)#OpenGL_and_GLSL_versions
                shader_code_text[shader_code_count++] = strdup("#version 150\n");
            }
            else
            {
                shader_code_text[shader_code_count++] = strdup("#version 330\n");
            }
        }
        else
        {
            // OpenGL 3.2 had GLSL version 1.50.  anything after that the version numbers match.
            if (type == GL_GEOMETRY_SHADER || minor_version >= 50)
            {
                //set version to 1.50
                shader_code_text[shader_code_count++] = strdup("#version 150\n");
                //some implementations of GLSL 1.30 require integer precision be explicitly declared
                extra_code_text[extra_code_count++] = strdup("precision mediump int;\n");
                extra_code_text[extra_code_count++] = strdup("precision highp float;\n");
            }
            else
            {
                //set version to 1.40
                shader_code_text[shader_code_count++] = strdup("#version 140\n");
                //some implementations of GLSL 1.30 require integer precision be explicitly declared
                extra_code_text[extra_code_count++] = strdup("precision mediump int;\n");
                extra_code_text[extra_code_count++] = strdup("precision highp float;\n");
            }
        }
    }

    const ShaderWorkarounds workarounds = shaderWorkarounds(); // [Viewer Lab]

    if (type == GL_FRAGMENT_SHADER)
    {
        extra_code_text[extra_code_count++] = strdup("#define FRAGMENT_SHADER 1\n");
    }
    else
    {
        extra_code_text[extra_code_count++] = strdup("#define VERTEX_SHADER 1\n");
    }

    // Use alpha float to store bit flags
    // See: C++: addDeferredAttachment(), shader: frag_data[2]
    extra_code_text[extra_code_count++] = strdup("#define GBUFFER_FLAG_SKIP_ATMOS   0.0 \n"); // atmo kill
    extra_code_text[extra_code_count++] = strdup("#define GBUFFER_FLAG_HAS_ATMOS    0.34\n"); // bit 0
    extra_code_text[extra_code_count++] = strdup("#define GBUFFER_FLAG_HAS_PBR      0.67\n"); // bit 1
    extra_code_text[extra_code_count++] = strdup("#define GBUFFER_FLAG_HAS_HDRI      1.0\n");  // bit 2
    extra_code_text[extra_code_count++] = strdup("#define GET_GBUFFER_FLAG(data, flag)    (abs(data-flag)< 0.1)\n");

    if (defines)
    {
        for (auto iter = defines->begin(); iter != defines->end(); ++iter)
        {
            std::string define = "#define " + iter->first + " " + iter->second + "\n";
            extra_code_text[extra_code_count++] = (GLchar *) strdup(define.c_str());
        }
    }

    if (workarounds.amdCard) // [Viewer Lab]
    {
        extra_code_text[extra_code_count++] = strdup( "#define IS_AMD_CARD 1\n" );
    }

    if (texture_index_channels > 0 && type == GL_FRAGMENT_SHADER)
    {
        //use specified number of texture channels for indexed texture rendering

        /* prepend shader code that looks like this:

        uniform sampler2D tex0;
        uniform sampler2D tex1;
        uniform sampler2D tex2;
        .
        .
        .
        uniform sampler2D texN;

        flat in int vary_texture_index;

        vec4 ret = vec4(1,0,1,1);

        vec4 diffuseLookup(vec2 texcoord)
        {
            switch (vary_texture_index)
            {
                case 0: ret = texture(tex0, texcoord); break;
                case 1: ret = texture(tex1, texcoord); break;
                case 2: ret = texture(tex2, texcoord); break;
                .
                .
                .
                case N: return texture(texN, texcoord); break;
            }

            return ret;
        }
        */

        extra_code_text[extra_code_count++] = strdup("#define HAS_DIFFUSE_LOOKUP\n");

        //uniform declartion
        for (S32 i = 0; i < texture_index_channels; ++i)
        {
            std::string decl = llformat("uniform sampler2D tex%d;\n", i);
            extra_code_text[extra_code_count++] = strdup(decl.c_str());
        }

        if (texture_index_channels > 1)
        {
            extra_code_text[extra_code_count++] = strdup("flat in int vary_texture_index;\n");
        }

        extra_code_text[extra_code_count++] = strdup("vec4 diffuseLookup(vec2 texcoord)\n");
        extra_code_text[extra_code_count++] = strdup("{\n");


        if (texture_index_channels == 1)
        { //don't use flow control, that's silly
            extra_code_text[extra_code_count++] = strdup("return texture(tex0, texcoord);\n");
            extra_code_text[extra_code_count++] = strdup("}\n");
        }
        else if (major_version > 1 || minor_version >= 30)
        {  //switches are supported in GLSL 1.30 and later
            if (workarounds.ifChain) // [Viewer Lab]
            { //switches are unreliable on some NVIDIA drivers
                for (S32 i = 0; i < texture_index_channels; ++i)
                {
                    std::string if_string = llformat("\t%sif (vary_texture_index == %d) { return texture(tex%d, texcoord); }\n", i > 0 ? "else " : "", i, i);
                    extra_code_text[extra_code_count++] = strdup(if_string.c_str());
                }
                extra_code_text[extra_code_count++] = strdup("\treturn vec4(1,0,1,1);\n");
                extra_code_text[extra_code_count++] = strdup("}\n");
            }
            else
            {
                extra_code_text[extra_code_count++] = strdup("\tvec4 ret = vec4(1,0,1,1);\n");
                extra_code_text[extra_code_count++] = strdup("\tswitch (vary_texture_index)\n");
                extra_code_text[extra_code_count++] = strdup("\t{\n");

                //switch body
                for (S32 i = 0; i < texture_index_channels; ++i)
                {
                    std::string case_str = llformat("\t\tcase %d: return texture(tex%d, texcoord);\n", i, i);
                    extra_code_text[extra_code_count++] = strdup(case_str.c_str());
                }

                extra_code_text[extra_code_count++] = strdup("\t}\n");
                extra_code_text[extra_code_count++] = strdup("\treturn ret;\n");
                extra_code_text[extra_code_count++] = strdup("}\n");
            }
        }
        else
        { //should never get here.  Indexed texture rendering requires GLSL 1.30 or later
            // (for passing integers between vertex and fragment shaders)
            LL_ERRS() << "Indexed texture rendering requires GLSL 1.30 or later." << LL_ENDL;
        }
    }

    // Master definition can be found in deferredUtil.glsl
    extra_code_text[extra_code_count++] = strdup("struct GBufferInfo { vec4 albedo; vec4 specular; vec3 normal; vec4 emissive; float gbufferFlag; float envIntensity; };\n");

    //copy file into memory
    enum {
          flag_write_to_out_of_extra_block_area = 0x01
        , flag_extra_block_marker_was_found = 0x02
    };

    unsigned char flags = flag_write_to_out_of_extra_block_area;

    GLuint out_of_extra_block_counter = 0, start_shader_code = shader_code_count, file_lines_count = 0;

#define TOUCH_SHADERS 0

#if TOUCH_SHADERS
    const char* marker = "// touched";
    bool touched = false;
#endif

    while(NULL != fgets((char *)buff, 1024, file)
          && shader_code_count < (LL_ARRAY_SIZE(shader_code_text) - LL_ARRAY_SIZE(extra_code_text)))
    {
        file_lines_count++;

        bool extra_block_area_found = NULL != strstr((const char*)buff, "[EXTRA_CODE_HERE]");

#if TOUCH_SHADERS
        if (NULL != strstr((const char*)buff, marker))
        {
            touched = true;
        }
#endif

        if(extra_block_area_found && !(flag_extra_block_marker_was_found & flags))
        {
            if(!(flag_write_to_out_of_extra_block_area & flags))
            {
                //shift
                for(GLuint to = start_shader_code, from = extra_code_count + start_shader_code;
                    from < shader_code_count; ++to, ++from)
                {
                    shader_code_text[to] = shader_code_text[from];
                }

                shader_code_count -= extra_code_count;
            }

            //copy extra code
            for(GLuint n = 0; n < extra_code_count
                && shader_code_count < (LL_ARRAY_SIZE(shader_code_text) - LL_ARRAY_SIZE(extra_code_text)); ++n)
            {
                shader_code_text[shader_code_count++] = extra_code_text[n];
            }

            extra_code_count = 0;

            flags &= ~flag_write_to_out_of_extra_block_area;
            flags |= flag_extra_block_marker_was_found;
        }
        else
        {
            shader_code_text[shader_code_count] = (GLchar *)strdup((char *)buff);

            if(flag_write_to_out_of_extra_block_area & flags)
            {
                shader_code_text[extra_code_count + start_shader_code + out_of_extra_block_counter]
                    = shader_code_text[shader_code_count];
                out_of_extra_block_counter++;

                if(out_of_extra_block_counter == extra_code_count)
                {
                    shader_code_count += extra_code_count;
                    flags &= ~flag_write_to_out_of_extra_block_area;
                }
            }

            ++shader_code_count;
        }
    } //while

    if(!(flag_extra_block_marker_was_found & flags))
    {
        for(GLuint n = start_shader_code; n < extra_code_count + start_shader_code; ++n)
        {
            shader_code_text[n] = extra_code_text[n - start_shader_code];
        }

        if (file_lines_count < extra_code_count)
        {
            shader_code_count += extra_code_count;
        }

        extra_code_count = 0;
    }

#if TOUCH_SHADERS
    if (!touched)
    {
        fprintf(file, "\n%s\n", marker);
    }
#endif

    fclose(file);

    // [Viewer Lab] The lines go to the ShaderSource, which frees them
    source.mLines.assign(shader_code_text, shader_code_text + shader_code_count);
    source.mOpenFileName   = open_file_name;
    source.mRequestedClass = try_gpu_class;
    source.mFoundClass     = gpu_class;
    source.mFound          = true;
    return true;
}

GLuint LLShaderMgr::loadShaderFile(const std::string& filename, S32& shader_level, GLenum type, std::map<std::string, std::string>* defines,
                                   S32 texture_index_channels, ShaderSource* assembled) // [Viewer Lab] assembled
{
    // [Viewer Lab] Shader manifest: the class asked for before any fall-back to a lower one, and no
    // stale record of the file from an earlier compile with other defines.
    struct ManifestDepth
    {
        S32& mDepth;
        ManifestDepth(S32& depth) : mDepth(depth) { ++mDepth; }
        ~ManifestDepth() { --mDepth; }
    } manifest_depth(mManifestDepth);
    if (!mManifestFolder.empty() && mManifestDepth == 1)
    {
        mManifestRequestedClass = shader_level;
        mManifestCompiled.erase({ filename, type });
    }

// endsure work-around for missing GLSL funcs gets propogated to feature shader files (e.g. srgbF.glsl)
#if LL_DARWIN
    if (shaderWorkarounds().oldSelect && defines) // [Viewer Lab]
    {
        (*defines)["OLD_SELECT"] = "1";
    }
#endif

    GLenum error = GL_NO_ERROR;

    error = gl_get_error(); // [Viewer Lab]
    if (error != GL_NO_ERROR)
    {
        LL_SHADER_LOADING_WARNS() << "GL ERROR entering loadShaderFile(): " << error << " for file: " << filename << LL_ENDL;
    }

    if (filename.empty())
    {
        LL_WARNS("ShaderLoading") << "tried loading empty filename" << LL_ENDL;
        return 0;
    }

    // [Viewer Lab] The lines assembleProgram assembled for this file before createShader's first GL call while they are
    // still at the class asked for; otherwise, as after a fall-back to a lower class, they are assembled now into the
    // same ShaderSource, so that it ends up holding what GL compiled
    ShaderSource  local;
    ShaderSource& source = assembled ? *assembled : local;
    source.mCompiled     = false;
    if (!source.mFound || source.mRequestedClass != shader_level)
    {
        if (!assembleShaderSource(filename, shader_level, type, defines, texture_index_channels, source))
        {
            return 0;
        }
    }

    // [Viewer Lab] The shader object, in a GL backend function
    GLuint ret = 0;
    const bool compiled = gl_compile_shader(*this, type, (GLuint)source.mLines.size(), source.mLines.data(), source.mOpenFileName, ret);
    stop_glerror();

    //free memory
    if (compiled && !mManifestFolder.empty()) // [Viewer Lab]
    {
        recordCompiled(filename, type, source.mRequestedClass, source.mFoundClass, (U32)source.mLines.size(),
                       source.mLines.data()); // [Viewer Lab]
    }
    source.mCompiled = compiled; // [Viewer Lab] the ShaderSource frees the lines

    //successfully loaded, save results
    if (compiled) // [Viewer Lab]
    {
        // Add shader file to map
        if (type == GL_VERTEX_SHADER) {
            mVertexShaderObjects[filename] = ret;
        }
        else if (type == GL_FRAGMENT_SHADER) {
            mFragmentShaderObjects[filename] = ret;
        }
        // [Viewer Lab] What it was compiled from, beside the GL maps, for assembleProgram
        std::string       text             = source.text();
        const std::string digest           = HBXXH128::digest(text).asString();
        mObjectSources[{ filename, type }] = { std::move(text), digest };

        shader_level = source.mRequestedClass; // [Viewer Lab]
    }
    else
    {
        if (shader_level > 1)
        {
            shader_level--;
            return loadShaderFile(filename, shader_level, type, defines, texture_index_channels, assembled); // [Viewer Lab] assembled
        }
        LL_WARNS("ShaderLoading") << "Failed to load " << filename << LL_ENDL;
    }

    LL_DEBUGS("ShaderLoading") << "loadShaderFile() completed, ret: " << U32(ret) << LL_ENDL;
    return ret;
}

// [Viewer Lab]
const LLShaderMgr::ObjectSource* LLShaderMgr::objectSource(const std::string& path, GLenum type) const
{
    // Only while the GL maps hold the file, which LLViewerShaderMgr::setShaders clears before every load
    const std::map<std::string, GLuint>* objects = type == GL_VERTEX_SHADER     ? &mVertexShaderObjects
                                                   : type == GL_FRAGMENT_SHADER ? &mFragmentShaderObjects
                                                                                : nullptr;
    if (!objects || objects->count(path) == 0)
    {
        return nullptr;
    }
    const auto found = mObjectSources.find({ path, type });
    return found != mObjectSources.end() ? &found->second : nullptr;
}

// [Viewer Lab]
bool LLShaderMgr::assembleProgram(LLGLSLShader* shader, std::vector<ShaderSource>& own, std::vector<std::pair<std::string, GLenum>>& shared)
{
    llassert_always(shader != NULL);
    // attachShaderFeatures' walk sets mRiggedVariant and mFeatures.mIndexedTextureChannels. Both come back byte for
    // byte, since hash() reads mFeatures' raw bytes.
    U8 features[sizeof(LLShaderFeatures)];
    memcpy(features, &shader->mFeatures, sizeof(LLShaderFeatures));
    LLGLSLShader* const rigged_variant = shader->mRiggedVariant;

    bool complete = true;
    own.clear();
    own.resize(shader->mShaderFiles.size());
    for (size_t i = 0; i < shader->mShaderFiles.size(); ++i)
    {
        const auto& [file, type] = shader->mShaderFiles[i];
        if (!assembleShaderSource(file, shader->mShaderLevel, type, &shader->mDefines, shader->mFeatures.mIndexedTextureChannels, own[i],
                                  false))
        {
            complete = false;
        }
    }

    shared.clear();
    mCollecting = &shared;
    attachShaderFeatures(shader);
    mCollecting = nullptr;

    memcpy(&shader->mFeatures, features, sizeof(LLShaderFeatures));
    shader->mRiggedVariant = rigged_variant;

    for (const auto& [path, type] : shared)
    {
        if (!objectSource(path, type))
        {
            complete = false;
        }
    }
    return complete;
}

// [Viewer Lab] Without the render interface.
LLUUID LLShaderMgr::programSourceHash(const std::vector<ShaderSource>& own, const std::vector<std::pair<std::string, GLenum>>& shared) const
{
    HBXXH128 hash_obj;
    for (const ShaderSource& unit : own)
    {
        hash_obj.update(&unit.mType, sizeof(GLenum));
        if (unit.mFound)
        {
            // [Viewer Lab] The joined text: the same bytes as per line, so the same digest (XXH3 streaming)
            hash_obj.update(unit.text());
        }
        else
        {
            hash_obj.update(unit.mFile);
        }
    }
    for (const auto& [path, type] : shared)
    {
        hash_obj.update(&type, sizeof(GLenum));
        const ObjectSource* object = objectSource(path, type);
        hash_obj.update(object ? object->mDigest : path);
    }
    return hash_obj.digest();
}

// [Viewer Lab]
bool LLShaderMgr::describeProgram(const LLGLSLShader* shader, const std::vector<ShaderSource>& own,
                                  const std::vector<std::pair<std::string, GLenum>>& shared, LL::RHI::ProgramDesc& desc)
{
    llassert_always(shader != NULL);
    const bool record = !mManifestFolder.empty();
    if (record)
    {
        mManifestStages.erase(shader);
    }

    desc                    = LL::RHI::ProgramDesc();
    desc.name               = shader->mName;
    desc.reservedAttributes = mReservedAttribs;
    desc.attributeMask      = 0; // known after the link, from the program's reflection
    desc.loginScreen        = shader->mLoginScreen;
    desc.uniformBlocks      = LLGLSLShader::uniformBlockNames();
    desc.shaderLevel        = shader->mShaderLevel; // the attempt's level
    const bool metal        = !gl_is_backend();     // the units' files and classes are Metal's

    for (const ShaderSource& unit : own)
    {
        if (!unit.mFound || !LL::RHI::shaderStageFromGL(unit.mType))
        {
            return false;
        }
    }
    for (const auto& [path, type] : shared)
    {
        if (!objectSource(path, type) || !LL::RHI::shaderStageFromGL(type))
        {
            return false;
        }
    }

    std::vector<std::pair<LL::RHI::ShaderStage, std::vector<std::string>>> digests;
    for (const LL::RHI::ShaderStage stage :
         { LL::RHI::ShaderStage::Vertex, LL::RHI::ShaderStage::Fragment, LL::RHI::ShaderStage::Geometry })
    {
        LL::RHI::StageSource source;
        source.stage = stage;
        std::vector<std::string> stage_digests;
        for (const ShaderSource& unit : own)
        {
            if (LL::RHI::shaderStageFromGL(unit.mType) == stage)
            {
                source.units.push_back(unit.text());
                if (metal) // the file and the class it was found in; GL ignores them
                {
                    source.files.push_back(unit.mFile);
                    source.classes.push_back(unit.mFoundClass);
                }
                if (record)
                {
                    stage_digests.push_back(HBXXH128::digest(source.units.back()).asString());
                }
            }
        }
        for (const auto& [path, type] : shared)
        {
            if (LL::RHI::shaderStageFromGL(type) == stage)
            {
                const ObjectSource* object = objectSource(path, type);
                source.units.push_back(object->mText);
                if (metal) // a shared object: its class is its own, not known here
                {
                    source.files.push_back(path);
                    source.classes.push_back(0);
                }
                if (record)
                {
                    stage_digests.push_back(object->mDigest);
                }
            }
        }
        if (!source.units.empty())
        {
            desc.stages.push_back(std::move(source));
            if (record)
            {
                digests.emplace_back(stage, std::move(stage_digests));
            }
        }
    }
    if (record)
    {
        mManifestStages[shader] = std::move(digests); // overwritten on each attempt, as mManifestAttached is
    }
    return true;
}

bool LLShaderMgr::linkProgramObject(GLuint obj, bool suppress_errors)
{
    //check for errors
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_SHADER("glLinkProgram");
        glLinkProgram(obj);
    }

    GLint success = GL_TRUE;

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_SHADER("glsl check link status");
        glGetProgramiv(obj, GL_LINK_STATUS, &success);
        if (!suppress_errors && success == GL_FALSE)
        {
            //an error occured, print log
            LL_SHADER_LOADING_WARNS() << "GLSL Linker Error:" << LL_ENDL;
            dumpObjectLog(obj, true, "linker");
            return success;
        }
    }

    std::string log = get_program_log(obj);
    LLStringUtil::toLower(log);
    if (log.find("software") != std::string::npos)
    {
        LL_SHADER_LOADING_WARNS() << "GLSL Linker: Running in Software:" << LL_ENDL;
        success = GL_FALSE;
        suppress_errors = false;
    }
    return success;
}

bool LLShaderMgr::validateProgramObject(GLuint obj)
{
    //check program validity against current GL
    glValidateProgram(obj);
    GLint success = GL_TRUE;
    glGetProgramiv(obj, GL_LINK_STATUS, &success);
    if (success == GL_FALSE)
    {
        LL_SHADER_LOADING_WARNS() << "GLSL program not valid: " << LL_ENDL;
        dumpObjectLog(obj);
    }
    else
    {
        dumpObjectLog(obj, false);
    }

    return success;
}

// [Viewer Lab] Whether the backend keeps program binaries, which chooses the binary cache:
// the viewer's device's programBinaries, or without a device (headless, llappearanceutility, the tests' fixture) the GL
// rule it replaces, as llimagegl.cpp's texture_swizzle.
static bool program_binaries()
{
    if (LL::RHI::Device* device = LL::RHI::viewerDevice())
    {
        return device->capabilities().programBinaries;
    }
    return gGLManager.mGLVersion >= 4.09;
}

void LLShaderMgr::initShaderCache(bool enabled, const LLUUID& old_cache_version, const LLUUID& current_cache_version, bool second_instance)
{
    LL_PROFILE_ZONE_SCOPED;
    LL_INFOS("ShaderMgr") << "Initializing shader cache" << LL_ENDL;

    mShaderCacheEnabled = program_binaries() && enabled; // [Viewer Lab]

    if(!mShaderCacheEnabled || mShaderCacheVersion.notNull())
        return;

    mShaderCacheVersion = current_cache_version;

    mShaderCacheDir = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "shader_cache");
    LLFile::mkdir(mShaderCacheDir);

    {
        std::string meta_out_path = gDirUtilp->add(mShaderCacheDir, "shaderdata.llsd");
        if (gDirUtilp->fileExists(meta_out_path))
        {
            LL_PROFILE_ZONE_NAMED("shader_cache");
            LL_INFOS("ShaderMgr") << "Loading shader cache metadata" << LL_ENDL;

            llifstream instream(meta_out_path, std::ifstream::in | std::ifstream::binary);
            LLSD in_data;
            try
            {
                LLSDSerialize::fromBinary(in_data, instream, LLSDSerialize::SIZE_UNLIMITED);
            }
            catch( std::bad_alloc& )
            {
                // Try to get a bit more memory back before we try to clear the cache.
                in_data.clear();
                // Just in case it was somehow the cause, clear cache.
                clearShaderCache();
                // If user run out of memory this early in init,
                // we don't want to keep going just to crash again.
                // Notify user and close.
                LLError::LLUserWarningMsg::showOutOfMemory();
                LL_ERRS("ShaderMgr") << "Failed to parse shader cache metadata, potentially due to size. Purged cache." << LL_ENDL;
                return;
            }
            instream.close();

            if (old_cache_version == current_cache_version
                && in_data["version"].asUUID() == current_cache_version)
            {
                for (const auto& data_pair : llsd::inMap(in_data["shaders"]))
                {
                    ProgramBinaryData binary_info = ProgramBinaryData();
                    binary_info.mBinaryFormat = data_pair.second["binary_format"].asInteger();
                    binary_info.mBinaryLength = data_pair.second["binary_size"].asInteger();
                    binary_info.mLastUsedTime = (F32)data_pair.second["last_used"].asReal();
                    mShaderBinaryCache.insert_or_assign(LLUUID(data_pair.first), binary_info);
                }
            }
            else if (!second_instance)
            {
                LL_INFOS("ShaderMgr") << "Shader cache version mismatch detected. Purging." << LL_ENDL;
                clearShaderCache();
            }
            else
            {
                LL_INFOS("ShaderMgr") << "Shader cache version mismatch detected." << LL_ENDL;
            }
        }
    }
}

void LLShaderMgr::clearShaderCache()
{
    std::string shader_cache = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "shader_cache");
    LL_INFOS("ShaderMgr") << "Removing shader cache at " << shader_cache << LL_ENDL;
    const std::string mask = "*";
    gDirUtilp->deleteFilesInDir(shader_cache, mask);
    LLFile::rmdir(shader_cache);
    mShaderBinaryCache.clear();
}

void LLShaderMgr::persistShaderCacheMetadata()
{
    if(!mShaderCacheEnabled) return;
    if (mShaderCacheVersion.isNull())
    {
        LL_WARNS("ShaderMgr") << "Attempted to save shader cache with no version set" << LL_ENDL;
        return;
    }

    LL_INFOS("ShaderMgr") << "Persisting shader cache metadata to disk" << LL_ENDL;

    LLSD out;
    // Settings and shader cache get saved at different time, thus making
    // RenderShaderCacheVersion unreliable when running multiple viewer
    // instances, or for cases where viewer crashes before saving settings.
    // Dupplicate version to the cache itself.
    out["version"] = mShaderCacheVersion;
    out["shaders"] = LLSD::emptyMap();
    LLSD &shaders = out["shaders"];

    static const F32 LRU_TIME = (60.f * 60.f) * 24.f * 7.f; // 14 days
    const F32 current_time = (F32)LLTimer::getTotalSeconds();
    for (auto it = mShaderBinaryCache.begin(); it != mShaderBinaryCache.end();)
    {
        const ProgramBinaryData& shader_metadata = it->second;
        if ((shader_metadata.mLastUsedTime + LRU_TIME) < current_time)
        {
            std::string shader_path = gDirUtilp->add(mShaderCacheDir, it->first.asString() + ".shaderbin");
            LLFile::remove(shader_path);
            it = mShaderBinaryCache.erase(it);
        }
        else
        {
            LLSD data = LLSD::emptyMap();
            data["binary_format"] = LLSD::Integer(shader_metadata.mBinaryFormat);
            data["binary_size"] = LLSD::Integer(shader_metadata.mBinaryLength);
            data["last_used"] = LLSD::Real(shader_metadata.mLastUsedTime);
            shaders[it->first.asString()] = data;
            ++it;
        }
    }

    std::string meta_out_path = gDirUtilp->add(mShaderCacheDir, "shaderdata.llsd");
    llofstream outstream(meta_out_path, std::ios_base::out | std::ios_base::binary);
    if (!outstream.is_open())
    {
        LL_WARNS("ShaderMgr") << "Failed to open file. Unable to save shader cache to: " << mShaderCacheDir << LL_ENDL;
        return;
    }
    LLSDSerialize::toBinary(out, outstream);
    outstream.close();
}

bool LLShaderMgr::loadCachedProgramBinary(LLGLSLShader* shader)
{
    if (!mShaderCacheEnabled) return false;

    glProgramParameteri(shader->mProgramObject, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);

    auto binary_iter = mShaderBinaryCache.find(shader->mShaderHash);
    if (binary_iter != mShaderBinaryCache.end())
    {
        std::string in_path = gDirUtilp->add(mShaderCacheDir, shader->mShaderHash.asString() + ".shaderbin");
        auto& shader_info = binary_iter->second;
        if (shader_info.mBinaryLength > 0)
        {
            std::vector<U8> in_data;
            in_data.resize(shader_info.mBinaryLength);

            LLUniqueFile filep = LLFile::fopen(in_path, "rb");
            if (filep)
            {
                size_t result = fread(in_data.data(), sizeof(U8), in_data.size(), filep);
                filep.close();

                if (result == in_data.size())
                {
                    GLenum error = glGetError(); // Clear current error
                    glProgramBinary(shader->mProgramObject, shader_info.mBinaryFormat, in_data.data(), shader_info.mBinaryLength);

                    error = glGetError();
                    GLint success = GL_TRUE;
                    glGetProgramiv(shader->mProgramObject, GL_LINK_STATUS, &success);
                    if (error == GL_NO_ERROR && success == GL_TRUE)
                    {
                        binary_iter->second.mLastUsedTime = (F32)LLTimer::getTotalSeconds();
                        LL_INFOS() << "Loaded cached binary for shader: " << shader->mName << LL_ENDL;
                        return true;
                    }
                }
            }
        }
        //an error occured, normally we would print log but in this case it means the shader needs recompiling.
        LL_INFOS() << "Failed to load cached binary for shader: " << shader->mName << " falling back to compilation" << LL_ENDL;
        LLFile::remove(in_path);
        mShaderBinaryCache.erase(binary_iter);
    }
    return false;
}

bool LLShaderMgr::saveCachedProgramBinary(LLGLSLShader* shader)
{
    if (!mShaderCacheEnabled) return true;

    ProgramBinaryData binary_info = ProgramBinaryData();
    glGetProgramiv(shader->mProgramObject, GL_PROGRAM_BINARY_LENGTH, &binary_info.mBinaryLength);
    if (binary_info.mBinaryLength > 0)
    {
        std::vector<U8> program_binary;
        program_binary.resize(binary_info.mBinaryLength);

        GLenum error = glGetError(); // Clear current error
        glGetProgramBinary(shader->mProgramObject, static_cast<GLsizei>(program_binary.size() * sizeof(U8)), nullptr, &binary_info.mBinaryFormat, program_binary.data());
        error = glGetError();
        if (error == GL_NO_ERROR)
        {
            std::string out_path = gDirUtilp->add(mShaderCacheDir, shader->mShaderHash.asString() + ".shaderbin");
            LLUniqueFile outfile = LLFile::fopen(out_path, "wb");
            if (outfile)
            {
                fwrite(program_binary.data(), sizeof(U8), program_binary.size(), outfile);
                outfile.close();

                binary_info.mLastUsedTime = (F32)LLTimer::getTotalSeconds();

                mShaderBinaryCache.insert_or_assign(shader->mShaderHash, binary_info);
                return true;
            }
        }
    }
    return false;
}

//virtual
void LLShaderMgr::initAttribsAndUniforms()
{
    //MUST match order of enum in LLVertexBuffer.h
    mReservedAttribs.push_back("position");
    mReservedAttribs.push_back("normal");
    mReservedAttribs.push_back("texcoord0");
    mReservedAttribs.push_back("texcoord1");
    mReservedAttribs.push_back("texcoord2");
    mReservedAttribs.push_back("texcoord3");
    mReservedAttribs.push_back("diffuse_color");
    mReservedAttribs.push_back("emissive");
    mReservedAttribs.push_back("tangent");
    mReservedAttribs.push_back("weight");
    mReservedAttribs.push_back("weight4");
    mReservedAttribs.push_back("clothing");
    mReservedAttribs.push_back("joint");
    mReservedAttribs.push_back("texture_index");

    //matrix state
    mReservedUniforms.push_back("modelview_matrix");
    mReservedUniforms.push_back("projection_matrix");
    mReservedUniforms.push_back("inv_proj");
    mReservedUniforms.push_back("modelview_projection_matrix");
    mReservedUniforms.push_back("inv_modelview");
    mReservedUniforms.push_back("identity_matrix");
    mReservedUniforms.push_back("normal_matrix");
    mReservedUniforms.push_back("texture_matrix0");
    mReservedUniforms.push_back("texture_matrix1");
    mReservedUniforms.push_back("texture_matrix2");
    mReservedUniforms.push_back("texture_matrix3");
    mReservedUniforms.push_back("object_plane_s");
    mReservedUniforms.push_back("object_plane_t");

    mReservedUniforms.push_back("texture_base_color_transform"); // (GLTF)
    mReservedUniforms.push_back("texture_normal_transform"); // (GLTF)
    mReservedUniforms.push_back("texture_metallic_roughness_transform"); // (GLTF)
    mReservedUniforms.push_back("texture_occlusion_transform"); // (GLTF)
    mReservedUniforms.push_back("texture_emissive_transform"); // (GLTF)
    mReservedUniforms.push_back("base_color_texcoord"); // (GLTF)
    mReservedUniforms.push_back("emissive_texcoord"); // (GLTF)
    mReservedUniforms.push_back("normal_texcoord"); // (GLTF)
    mReservedUniforms.push_back("metallic_roughness_texcoord"); // (GLTF)
    mReservedUniforms.push_back("occlusion_texcoord"); // (GLTF)
    mReservedUniforms.push_back("gltf_node_id"); // (GLTF)
    mReservedUniforms.push_back("gltf_material_id"); // (GLTF)

    mReservedUniforms.push_back("terrain_texture_transforms"); // (GLTF)

    llassert(mReservedUniforms.size() == LLShaderMgr::TERRAIN_TEXTURE_TRANSFORMS +1);

    mReservedUniforms.push_back("viewport");

    mReservedUniforms.push_back("light_position");
    mReservedUniforms.push_back("light_direction");
    mReservedUniforms.push_back("light_attenuation");
    mReservedUniforms.push_back("light_deferred_attenuation");
    mReservedUniforms.push_back("light_diffuse");
    mReservedUniforms.push_back("light_ambient");
    mReservedUniforms.push_back("light_count");
    mReservedUniforms.push_back("light");
    mReservedUniforms.push_back("light_col");
    mReservedUniforms.push_back("far_z");

    llassert(mReservedUniforms.size() == LLShaderMgr::MULTI_LIGHT_FAR_Z+1);

    //NOTE: MUST match order in eGLSLReservedUniforms
    mReservedUniforms.push_back("proj_mat");
    mReservedUniforms.push_back("proj_near");
    mReservedUniforms.push_back("proj_p");
    mReservedUniforms.push_back("proj_n");
    mReservedUniforms.push_back("proj_origin");
    mReservedUniforms.push_back("proj_range");
    mReservedUniforms.push_back("proj_ambiance");
    mReservedUniforms.push_back("proj_shadow_idx");
    mReservedUniforms.push_back("shadow_fade");
    mReservedUniforms.push_back("proj_focus");
    mReservedUniforms.push_back("proj_lod");
    mReservedUniforms.push_back("proj_ambient_lod");

    llassert(mReservedUniforms.size() == LLShaderMgr::PROJECTOR_AMBIENT_LOD+1);

    mReservedUniforms.push_back("color");
    mReservedUniforms.push_back("emissiveColor");
    mReservedUniforms.push_back("metallicFactor");
    mReservedUniforms.push_back("roughnessFactor");
    mReservedUniforms.push_back("mirror_flag");
    mReservedUniforms.push_back("clipPlane");
    mReservedUniforms.push_back("clipSign");

    mReservedUniforms.push_back("diffuseMap");
    mReservedUniforms.push_back("altDiffuseMap");
    mReservedUniforms.push_back("specularMap");
    mReservedUniforms.push_back("metallicRoughnessMap");
    mReservedUniforms.push_back("normalMap");
    mReservedUniforms.push_back("occlusionMap");
    mReservedUniforms.push_back("emissiveMap");
    mReservedUniforms.push_back("bumpMap");
    mReservedUniforms.push_back("bumpMap2");
    mReservedUniforms.push_back("environmentMap");
    mReservedUniforms.push_back("sceneMap");
    mReservedUniforms.push_back("sceneDepth");
    mReservedUniforms.push_back("reflectionProbes");
    mReservedUniforms.push_back("irradianceProbes");
    mReservedUniforms.push_back("heroProbes");
    mReservedUniforms.push_back("cloud_noise_texture");
    mReservedUniforms.push_back("cloud_noise_texture_next");
    mReservedUniforms.push_back("lightnorm");
    mReservedUniforms.push_back("sunlight_color");
    mReservedUniforms.push_back("ambient_color");
    mReservedUniforms.push_back("sky_hdr_scale");
    mReservedUniforms.push_back("sky_sunlight_scale");
    mReservedUniforms.push_back("sky_ambient_scale");
    mReservedUniforms.push_back("classic_mode");
    mReservedUniforms.push_back("blue_horizon");
    mReservedUniforms.push_back("blue_density");
    mReservedUniforms.push_back("haze_horizon");
    mReservedUniforms.push_back("haze_density");
    mReservedUniforms.push_back("cloud_shadow");
    mReservedUniforms.push_back("density_multiplier");
    mReservedUniforms.push_back("distance_multiplier");
    mReservedUniforms.push_back("max_y");
    mReservedUniforms.push_back("glow");
    mReservedUniforms.push_back("cloud_color");
    mReservedUniforms.push_back("cloud_pos_density1");
    mReservedUniforms.push_back("cloud_pos_density2");
    mReservedUniforms.push_back("cloud_scale");
    mReservedUniforms.push_back("gamma");
    mReservedUniforms.push_back("scene_light_strength");

    llassert(mReservedUniforms.size() == LLShaderMgr::SCENE_LIGHT_STRENGTH+1);

    mReservedUniforms.push_back("center");
    mReservedUniforms.push_back("size");
    mReservedUniforms.push_back("falloff");

    mReservedUniforms.push_back("box_center");
    mReservedUniforms.push_back("box_size");


    mReservedUniforms.push_back("minLuminance");
    mReservedUniforms.push_back("maxExtractAlpha");
    mReservedUniforms.push_back("lumWeights");
    mReservedUniforms.push_back("warmthWeights");
    mReservedUniforms.push_back("warmthAmount");
    mReservedUniforms.push_back("glowStrength");
    mReservedUniforms.push_back("glowDelta");
    mReservedUniforms.push_back("glowNoiseMap");

    llassert(mReservedUniforms.size() == LLShaderMgr::GLOW_NOISE_MAP+1);


    mReservedUniforms.push_back("minimum_alpha");
    mReservedUniforms.push_back("emissive_brightness");

    // Deferred
    mReservedUniforms.push_back("shadow_matrix");
    mReservedUniforms.push_back("env_mat");
    mReservedUniforms.push_back("shadow_clip");
    mReservedUniforms.push_back("sun_wash");
    mReservedUniforms.push_back("shadow_noise");
    mReservedUniforms.push_back("blur_size");
    mReservedUniforms.push_back("ssao_radius");
    mReservedUniforms.push_back("ssao_max_radius");
    mReservedUniforms.push_back("ssao_factor");
    mReservedUniforms.push_back("ssao_factor_inv");
    mReservedUniforms.push_back("ssao_effect_mat");
    mReservedUniforms.push_back("screen_res");
    mReservedUniforms.push_back("near_clip");
    mReservedUniforms.push_back("shadow_offset");
    mReservedUniforms.push_back("shadow_bias");
    mReservedUniforms.push_back("spot_shadow_bias");
    mReservedUniforms.push_back("spot_shadow_offset");
    mReservedUniforms.push_back("sun_dir");
    mReservedUniforms.push_back("moon_dir");
    mReservedUniforms.push_back("shadow_res");
    mReservedUniforms.push_back("proj_shadow_res");
    mReservedUniforms.push_back("depth_cutoff");
    mReservedUniforms.push_back("norm_cutoff");
    mReservedUniforms.push_back("shadow_target_width");

    llassert(mReservedUniforms.size() == LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH + 1);

    mReservedUniforms.push_back("iterationCount");
    mReservedUniforms.push_back("rayStep");
    mReservedUniforms.push_back("distanceBias");
    mReservedUniforms.push_back("depthRejectBias");
    mReservedUniforms.push_back("glossySampleCount");
    mReservedUniforms.push_back("noiseSine");
    mReservedUniforms.push_back("adaptiveStepMultiplier");

    mReservedUniforms.push_back("modelview_delta");
    mReservedUniforms.push_back("inv_modelview_delta");
    mReservedUniforms.push_back("cube_snapshot");

    mReservedUniforms.push_back("tc_scale");
    mReservedUniforms.push_back("rcp_screen_res");
    mReservedUniforms.push_back("rcp_frame_opt");
    mReservedUniforms.push_back("rcp_frame_opt2");

    mReservedUniforms.push_back("focal_distance");
    mReservedUniforms.push_back("blur_constant");
    mReservedUniforms.push_back("tan_pixel_angle");
    mReservedUniforms.push_back("magnification");
    mReservedUniforms.push_back("max_cof");
    mReservedUniforms.push_back("res_scale");
    mReservedUniforms.push_back("dof_width");
    mReservedUniforms.push_back("dof_height");

    mReservedUniforms.push_back("depthMap");
    mReservedUniforms.push_back("shadowMap0");
    mReservedUniforms.push_back("shadowMap1");
    mReservedUniforms.push_back("shadowMap2");
    mReservedUniforms.push_back("shadowMap3");
    mReservedUniforms.push_back("shadowMap4");
    mReservedUniforms.push_back("shadowMap5");

    llassert(mReservedUniforms.size() == LLShaderMgr::DEFERRED_SHADOW5+1);

    mReservedUniforms.push_back("positionMap");
    mReservedUniforms.push_back("diffuseRect");
    mReservedUniforms.push_back("specularRect");
    mReservedUniforms.push_back("emissiveRect");
    mReservedUniforms.push_back("exposureMap");
    mReservedUniforms.push_back("brdfLut");
    mReservedUniforms.push_back("noiseMap");
    mReservedUniforms.push_back("lightFunc");
    mReservedUniforms.push_back("lightMap");
    mReservedUniforms.push_back("bloomMap");
    mReservedUniforms.push_back("projectionMap");
    mReservedUniforms.push_back("norm_mat");

    mReservedUniforms.push_back("specular_color");
    mReservedUniforms.push_back("env_intensity");

    mReservedUniforms.push_back("matrixPalette");
    mReservedUniforms.push_back("translationPalette");

// <FS:CR> Import Vignette from Exodus
    mReservedUniforms.push_back("vignette");
// </FS:CR> Import Vignette from Exodus

    mReservedUniforms.push_back("screenTex");
    mReservedUniforms.push_back("screenDepth");
    mReservedUniforms.push_back("refTex");
    mReservedUniforms.push_back("exclusionTex");
    mReservedUniforms.push_back("eyeVec");
    mReservedUniforms.push_back("time");
    mReservedUniforms.push_back("waveDir1");
    mReservedUniforms.push_back("waveDir2");
    mReservedUniforms.push_back("lightDir");
    mReservedUniforms.push_back("specular");
    mReservedUniforms.push_back("lightExp");
    mReservedUniforms.push_back("waterFogColor");
    mReservedUniforms.push_back("waterFogColorLinear");
    mReservedUniforms.push_back("waterFogDensity");
    mReservedUniforms.push_back("waterFogKS");
    mReservedUniforms.push_back("refScale");
    mReservedUniforms.push_back("waterHeight");
    mReservedUniforms.push_back("waterPlane");
    mReservedUniforms.push_back("normScale");
    mReservedUniforms.push_back("fresnelScale");
    mReservedUniforms.push_back("fresnelOffset");
    mReservedUniforms.push_back("blurMultiplier");
    mReservedUniforms.push_back("sunAngle");
    mReservedUniforms.push_back("scaledAngle");
    mReservedUniforms.push_back("sunAngle2");

    mReservedUniforms.push_back("camPosLocal");
// [RLVa:KB] - @setsphere
    mReservedUniforms.push_back("rlvEffectMode");
    mReservedUniforms.push_back("rlvEffectParam1");
    mReservedUniforms.push_back("rlvEffectParam2");
    mReservedUniforms.push_back("rlvEffectParam3");
    mReservedUniforms.push_back("rlvEffectParam4");
    mReservedUniforms.push_back("rlvEffectParam5");
// [/RLV:KB]

    mReservedUniforms.push_back("gWindDir");
    mReservedUniforms.push_back("gSinWaveParams");
    mReservedUniforms.push_back("gGravity");

    mReservedUniforms.push_back("detail_0");
    mReservedUniforms.push_back("detail_1");
    mReservedUniforms.push_back("detail_2");
    mReservedUniforms.push_back("detail_3");

    mReservedUniforms.push_back("alpha_ramp");
    mReservedUniforms.push_back("paint_map");

    mReservedUniforms.push_back("detail_0_base_color");
    mReservedUniforms.push_back("detail_1_base_color");
    mReservedUniforms.push_back("detail_2_base_color");
    mReservedUniforms.push_back("detail_3_base_color");
    mReservedUniforms.push_back("detail_0_normal");
    mReservedUniforms.push_back("detail_1_normal");
    mReservedUniforms.push_back("detail_2_normal");
    mReservedUniforms.push_back("detail_3_normal");
    mReservedUniforms.push_back("detail_0_metallic_roughness");
    mReservedUniforms.push_back("detail_1_metallic_roughness");
    mReservedUniforms.push_back("detail_2_metallic_roughness");
    mReservedUniforms.push_back("detail_3_metallic_roughness");
    mReservedUniforms.push_back("detail_0_emissive");
    mReservedUniforms.push_back("detail_1_emissive");
    mReservedUniforms.push_back("detail_2_emissive");
    mReservedUniforms.push_back("detail_3_emissive");

    mReservedUniforms.push_back("baseColorFactors");
    mReservedUniforms.push_back("metallicFactors");
    mReservedUniforms.push_back("roughnessFactors");
    mReservedUniforms.push_back("emissiveColors");
    mReservedUniforms.push_back("minimum_alphas");

    mReservedUniforms.push_back("region_scale");

    mReservedUniforms.push_back("origin");
    mReservedUniforms.push_back("display_gamma");

    mReservedUniforms.push_back("inscatter");
    mReservedUniforms.push_back("sun_size");
    mReservedUniforms.push_back("fog_color");

    mReservedUniforms.push_back("transmittance_texture");
    mReservedUniforms.push_back("scattering_texture");
    mReservedUniforms.push_back("single_mie_scattering_texture");
    mReservedUniforms.push_back("irradiance_texture");
    mReservedUniforms.push_back("blend_factor");
    mReservedUniforms.push_back("moisture_level");
    mReservedUniforms.push_back("droplet_radius");
    mReservedUniforms.push_back("ice_level");
    mReservedUniforms.push_back("rainbow_map");
    mReservedUniforms.push_back("halo_map");
    mReservedUniforms.push_back("moon_brightness");
    mReservedUniforms.push_back("cloud_variance");
    mReservedUniforms.push_back("reflection_probe_ambiance");
    mReservedUniforms.push_back("max_probe_lod");
    mReservedUniforms.push_back("probe_strength");

    mReservedUniforms.push_back("sh_input_r");
    mReservedUniforms.push_back("sh_input_g");
    mReservedUniforms.push_back("sh_input_b");

    mReservedUniforms.push_back("sun_moon_glow_factor");
    mReservedUniforms.push_back("water_edge");
    mReservedUniforms.push_back("sun_up_factor");
    mReservedUniforms.push_back("moonlight_color");

    mReservedUniforms.push_back("debug_normal_draw_length");

    mReservedUniforms.push_back("edgesTex");
    mReservedUniforms.push_back("areaTex");
    mReservedUniforms.push_back("searchTex");
    mReservedUniforms.push_back("blendTex");
    // <FS:Beq> reserved uniforms for snapshot frame
    mReservedUniforms.push_back("border_color");
    mReservedUniforms.push_back("border_thickness");
    mReservedUniforms.push_back("frame_rect");
    // </FS:Beq>

    llassert(mReservedUniforms.size() == END_RESERVED_UNIFORMS);

    std::set<std::string> dupe_check;

    for (U32 i = 0; i < mReservedUniforms.size(); ++i)
    {
        if (dupe_check.find(mReservedUniforms[i]) != dupe_check.end())
        {
            LL_ERRS() << "Duplicate reserved uniform name found: " << mReservedUniforms[i] << LL_ENDL;
        }
        dupe_check.insert(mReservedUniforms[i]);
    }
}

// [Viewer Lab] Shader manifest (llshadermgr.h)

namespace
{
    std::string stageName(GLenum type)
    {
        switch (type)
        {
        case GL_VERTEX_SHADER:
            return "vertex";
        case GL_FRAGMENT_SHADER:
            return "fragment";
        case GL_GEOMETRY_SHADER:
            return "geometry";
        default:
            return llformat("0x%x", type);
        }
    }

    // [Viewer Lab]
    std::string stageName(LL::RHI::ShaderStage stage)
    {
        switch (stage)
        {
            case LL::RHI::ShaderStage::Vertex:
                return "vertex";
            case LL::RHI::ShaderStage::Fragment:
                return "fragment";
            case LL::RHI::ShaderStage::Geometry:
                return "geometry";
        }
        return "?";
    }

    bool writeFile(const std::string& path, const std::string& text)
    {
        llofstream file(path.c_str(), std::ios::binary);
        file << text;
        file.close();
        return !file.fail();
    }
}

void LLShaderMgr::setManifestFolder(const std::string& folder)
{
    // Every load compiles every object again (LLViewerShaderMgr::setShaders).
    mManifestCompiled.clear();
    mManifestAttached.clear();
    mManifestStages.clear(); // [Viewer Lab]
    mManifestFolder = folder;
    if (!folder.empty())
    {
        LLFile::mkdir(folder);
        LLFile::mkdir(gDirUtilp->add(folder, "sources"));
    }
}

void LLShaderMgr::recordCompiled(const std::string& filename, GLenum type, S32 compiled_class, S32 used_class,
                                 U32 shader_code_count, GLchar** shader_code_text)
{
    std::string source;
    for (U32 i = 0; i < shader_code_count; ++i)
    {
        source += shader_code_text[i];
    }
    ManifestObject object;
    object.mFile = filename;
    object.mType = type;
    object.mRequestedClass = mManifestRequestedClass;
    object.mCompiledClass = compiled_class;
    object.mUsedClass = used_class;
    object.mSource = HBXXH128::digest(source.data(), source.size()).asString();
    const std::string path = gDirUtilp->add(gDirUtilp->add(mManifestFolder, "sources"), object.mSource + ".glsl");
    if (!LLFile::isfile(path) && !writeFile(path, source))
    {
        LL_WARNS("ShaderLoading") << "Shader manifest: could not write " << path << LL_ENDL;
    }
    mManifestCompiled[{ filename, type }] = object;
}

void LLShaderMgr::recordAttached(LLGLSLShader* shader, const std::string& object_path, GLenum type)
{
    if (mManifestFolder.empty() || shader->mUsingBinaryProgram)
    {
        return;
    }
    const auto found = mManifestCompiled.find({ object_path, type });
    if (found != mManifestCompiled.end())
    {
        mManifestAttached[shader].mAttached.push_back(found->second);
    }
    else
    {
        ManifestObject missing; // shows as a missing source
        missing.mFile = object_path;
        missing.mType = type;
        mManifestAttached[shader].mAttached.push_back(missing);
    }
}

bool LLShaderMgr::attachVertexObject(LLGLSLShader* shader, const std::string& object_path)
{
    if (mCollecting) // [Viewer Lab] assembleProgram's walk collects the object and attaches nothing
    {
        mCollecting->emplace_back(object_path, GL_VERTEX_SHADER);
        return true;
    }
    const bool attached = shader->attachVertexObject(object_path);
    if (attached)
    {
        recordAttached(shader, object_path, GL_VERTEX_SHADER);
    }
    return attached;
}

bool LLShaderMgr::attachFragmentObject(LLGLSLShader* shader, const std::string& object_path)
{
    if (mCollecting) // [Viewer Lab] assembleProgram's walk collects the object and attaches nothing
    {
        mCollecting->emplace_back(object_path, GL_FRAGMENT_SHADER);
        return true;
    }
    const bool attached = shader->attachFragmentObject(object_path);
    if (attached)
    {
        recordAttached(shader, object_path, GL_FRAGMENT_SHADER);
    }
    return attached;
}

// [Viewer Lab] The stages describeProgram gave the render interface before the compile, against the digests of
// what GL compiled and attached (recordCompiled's, in attach order), for every complete program not from the binary
// cache whose units all compiled at the class they asked for. The others are counted as skipped.
void LLShaderMgr::checkManifestStages()
{
    U32 matched = 0;
    U32 skipped = 0;
    for (const LLGLSLShader* shader : LLGLSLShader::sInstances)
    {
        if (!shader->isComplete())
        {
            continue;
        }
        const auto attached  = mManifestAttached.find(shader);
        const auto described = mManifestStages.find(shader);
        bool       lower     = false;
        if (attached != mManifestAttached.end())
        {
            for (const ManifestObject& object : attached->second.mAttached)
            {
                lower = lower || object.mCompiledClass != object.mRequestedClass;
            }
        }
        if (shader->mUsingBinaryProgram || attached == mManifestAttached.end() || described == mManifestStages.end() || lower)
        {
            ++skipped;
            continue;
        }

        std::vector<std::pair<LL::RHI::ShaderStage, std::vector<std::string>>> compiled;
        for (const LL::RHI::ShaderStage stage :
             { LL::RHI::ShaderStage::Vertex, LL::RHI::ShaderStage::Fragment, LL::RHI::ShaderStage::Geometry })
        {
            std::vector<std::string> units;
            for (const ManifestObject& object : attached->second.mAttached)
            {
                if (LL::RHI::shaderStageFromGL(object.mType) == stage)
                {
                    units.push_back(object.mSource);
                }
            }
            if (!units.empty())
            {
                compiled.emplace_back(stage, std::move(units));
            }
        }
        if (compiled == described->second)
        {
            ++matched;
            continue;
        }

        // The first stage and unit that differ
        const auto& stages = described->second;
        size_t      index  = 0;
        size_t      unit   = 0;
        for (; index < stages.size() && index < compiled.size(); ++index)
        {
            if (stages[index].first != compiled[index].first)
            {
                break;
            }
            const auto& mine = stages[index].second;
            const auto& gl   = compiled[index].second;
            if (mine != gl)
            {
                unit = (size_t)(std::mismatch(mine.begin(), mine.end(), gl.begin(), gl.end()).first - mine.begin());
                break;
            }
        }
        const LL::RHI::ShaderStage stage = index < stages.size() ? stages[index].first : compiled[index].first;
        LL_WARNS("ShaderMgr") << "Shader manifest: the render interface's stages of '" << shader->mName
                              << "' differ from what GL compiled (" << stageName(stage) << " unit " << unit << ")" << LL_ENDL;
    }
    LL_INFOS("ShaderMgr") << "Shader manifest: the render interface's stages match what GL compiled for " << matched << " programs ("
                          << skipped << " skipped)" << LL_ENDL;
}

bool LLShaderMgr::writeManifest(const std::string& name, const LLSD& context)
{
    if (mManifestFolder.empty())
    {
        return false;
    }
    // [Viewer Lab] every program published, also those a backend compiles in the background
    LLGLSLShader::finishBuilds();
    checkManifestStages(); // [Viewer Lab]
    LLSD manifest = context;
    manifest["reserved_attributes"] = LLSD::emptyArray();
    for (const std::string& attribute : mReservedAttribs)
    {
        manifest["reserved_attributes"].append(attribute);
    }
    // By name, then by files and defines, so that two runs with the same settings write the same
    // manifest.
    std::vector<std::pair<std::string, LLGLSLShader*>> programs;
    for (LLGLSLShader* shader : LLGLSLShader::sInstances)
    {
        std::string key = shader->mName;
        for (const auto& [file, type] : shader->mShaderFiles)
        {
            key += "|" + file;
        }
        for (const auto& [define, value] : shader->mDefines)
        {
            key += "|" + define + "=" + value;
        }
        programs.emplace_back(key, shader);
    }
    std::sort(programs.begin(), programs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    manifest["programs"] = LLSD::emptyArray();
    for (const auto& [key, shader] : programs)
    {
        LLSD program;
        program["name"] = shader->mName;
        program["loaded"] = shader->isComplete();
        program["from_binary_cache"] = shader->mUsingBinaryProgram;
        program["shader_level"] = shader->mShaderLevel;
        program["indexed_texture_channels"] = shader->mFeatures.mIndexedTextureChannels;
        program["defines"] = LLSD::emptyMap();
        for (const auto& [define, value] : shader->mDefines)
        {
            program["defines"][define] = value;
        }
        program["files"] = LLSD::emptyArray();
        for (const auto& [file, type] : shader->mShaderFiles)
        {
            program["files"].append(llsd::map("file", file, "stage", stageName(type)));
        }
        program["attributes"] = LLSD::emptyArray();
        for (U32 i = 0; i < mReservedAttribs.size() && i < 32; ++i)
        {
            if (shader->mAttributeMask & (1U << i))
            {
                program["attributes"].append(mReservedAttribs[i]);
            }
        }
        // GL's active uniforms (mapUniforms), the facit for the Metal reflection's names, and the
        // texture channels of the reserved samplers.
        std::vector<std::string> uniforms;
        for (const auto& [name, location] : shader->mUniformMap)
        {
            uniforms.push_back(name.String());
        }
        std::sort(uniforms.begin(), uniforms.end());
        program["uniforms"] = LLSD::emptyArray();
        for (const std::string& uniform : uniforms)
        {
            program["uniforms"].append(uniform);
        }
        program["texture_channels"] = LLSD::emptyMap();
        for (U32 i = 0; i < shader->mTexture.size() && i < mReservedUniforms.size(); ++i)
        {
            if (shader->mTexture[i] >= 0)
            {
                program["texture_channels"][mReservedUniforms[i]] = shader->mTexture[i];
            }
        }
        program["active_texture_channels"] = shader->mActiveTextureChannels;
        // [Viewer Lab] GL's active size of each array among the active uniforms, by its name without
        // "[0]" (the shader check compares the Metal reflection's activeArraySize with it); block members included.
        program["uniform_sizes"] = LLSD::emptyMap();
        GLint active = 0;
        if (shader->mProgramObject)
        {
            glGetProgramiv(shader->mProgramObject, GL_ACTIVE_UNIFORMS, &active);
        }
        for (GLint index = 0; index < active; ++index)
        {
            GLchar uniform[256] = { 0 };
            GLint size = 0;
            GLenum type = 0;
            glGetActiveUniform(shader->mProgramObject, (GLuint)index, sizeof(uniform), nullptr, &size, &type, uniform);
            std::string name(uniform);
            if (name.size() > 3 && name.ends_with("[0]"))
            {
                program["uniform_sizes"][name.substr(0, name.size() - 3)] = size;
            }
        }
        // The uniform blocks GL reports, with their bindings (mapUniforms binds them by name) and
        // members, which mUniformMap does not hold.
        program["uniform_blocks"] = LLSD::emptyArray();
        GLint blocks = 0;
        if (shader->mProgramObject)
        {
            glGetProgramiv(shader->mProgramObject, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
        }
        for (GLint block = 0; block < blocks; ++block)
        {
            GLchar name[256] = { 0 };
            GLint binding = -1;
            GLint count = 0;
            GLint data_size = 0;
            glGetActiveUniformBlockName(shader->mProgramObject, block, sizeof(name), nullptr, name);
            glGetActiveUniformBlockiv(shader->mProgramObject, block, GL_UNIFORM_BLOCK_BINDING, &binding);
            glGetActiveUniformBlockiv(shader->mProgramObject, block, GL_UNIFORM_BLOCK_DATA_SIZE, &data_size); // [Viewer Lab]
            glGetActiveUniformBlockiv(shader->mProgramObject, block, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count);
            std::vector<GLint> indices(count > 0 ? count : 0);
            if (count > 0)
            {
                glGetActiveUniformBlockiv(shader->mProgramObject, block, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, indices.data());
            }
            std::vector<std::string> members;
            for (GLint index : indices)
            {
                GLchar member[256] = { 0 };
                glGetActiveUniformName(shader->mProgramObject, (GLuint)index, sizeof(member), nullptr, member);
                members.push_back(member);
            }
            std::sort(members.begin(), members.end());
            LLSD entry = llsd::map("name", std::string(name), "binding", binding, "size", data_size, "members",
                                   LLSD::emptyArray());
            for (const std::string& member : members)
            {
                entry["members"].append(member);
            }
            program["uniform_blocks"].append(entry);
        }
        program["attached"] = LLSD::emptyArray();
        const auto attached = mManifestAttached.find(shader);
        if (attached != mManifestAttached.end())
        {
            program["link_attempts"] = (S32)attached->second.mAttempts;
            program["first_shader_level"] = attached->second.mFirstLevel;
            for (const ManifestObject& object : attached->second.mAttached)
            {
                program["attached"].append(llsd::map("file", object.mFile, "stage", stageName(object.mType),
                                                     "requested_class", object.mRequestedClass,
                                                     "compiled_class", object.mCompiledClass,
                                                     "used_class", object.mUsedClass, "source", object.mSource));
            }
        }
        manifest["programs"].append(program);
    }
    const std::string path = gDirUtilp->add(mManifestFolder, name + ".json");
    if (!writeFile(path, boost::json::serialize(LlsdToJson(manifest)) + "\n"))
    {
        LL_WARNS("ShaderLoading") << "Shader manifest: could not write " << path << LL_ENDL;
        return false;
    }
    LL_INFOS("ShaderLoading") << "Shader manifest: " << manifest["programs"].size() << " programs in " << path << LL_ENDL;
    return true;
}
