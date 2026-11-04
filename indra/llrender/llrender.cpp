 /**
 * @file llrender.cpp
 * @brief LLRender implementation
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

#include "linden_common.h"

#include "llrender.h"

#include "llvertexbuffer.h"
#include "llcubemap.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "llrendertarget.h"
#include "lltexture.h"
#include "llshadermgr.h"
#include "llrhi.h"
#include "llrhigl.h" // [Viewer Lab] GLNames
#include "llthread.h" // [Viewer Lab] on_main_thread
#include "hbxxh.h"
#include "glm/gtc/type_ptr.hpp"

#include <bit>
#include <sstream>

// [Viewer Lab] metalDebug's declaration, with llrhi.cpp's guard: off Darwin LL_RHI_METAL is 0
#if LL_DARWIN
#include "llrhimetal.h"
#endif
#ifndef LL_RHI_METAL
#define LL_RHI_METAL 0
#endif

#if LL_WINDOWS
extern void APIENTRY gl_debug_callback(GLenum source,
                                GLenum type,
                                GLuint id,
                                GLenum severity,
                                GLsizei length,
                                const GLchar* message,
                                GLvoid* userParam)
;
#endif

thread_local LLRender gGL;

// Handy copies of last good GL matrices
F32 gGLModelView[16];
F32 gGLLastModelView[16];
F32 gGLLastProjection[16];
F32 gGLProjection[16];

// transform from last frame's camera space to this frame's camera space (and inverse)
glm::mat4 gGLDeltaModelView;
glm::mat4 gGLInverseDeltaModelView;

S32 gGLViewport[4];


U32 LLRender::sUICalls = 0;
U32 LLRender::sUIVerts = 0;
U32 LLTexUnit::sWhiteTexture = 0;
bool LLRender::sGLCoreProfile = false;
bool LLRender::sNsightDebugSupport = false;
LLVector2 LLRender::sUIGLScaleFactor = LLVector2(1.f, 1.f);
bool LLRender::sClassicMode = false;

struct LLVBCache
{
    LLPointer<LLVertexBuffer> vb;
    std::chrono::steady_clock::time_point touched;
};

static std::unordered_map<U64, LLVBCache> sVBCache;
static thread_local std::list<LLVertexBufferData> *sBufferDataList = nullptr;

static const GLenum sGLTextureType[] =
{
    GL_TEXTURE_2D,
    GL_TEXTURE_RECTANGLE,
    GL_TEXTURE_CUBE_MAP,
    GL_TEXTURE_CUBE_MAP_ARRAY,
    GL_TEXTURE_2D_MULTISAMPLE,
    GL_TEXTURE_3D
};

static const GLint sGLAddressMode[] =
{
    GL_REPEAT,
    GL_MIRRORED_REPEAT,
    GL_CLAMP_TO_EDGE
};

// [Viewer Lab] The interface's sampling enums follow LLTexUnit's, so toGLSampling translates by casting
static_assert(U32(LL::RHI::Filter::Anisotropic) == LLTexUnit::TFO_ANISOTROPIC
              && U32(LL::RHI::Filter::Trilinear) == LLTexUnit::TFO_TRILINEAR
              && U32(LL::RHI::Filter::Bilinear) == LLTexUnit::TFO_BILINEAR
              && U32(LL::RHI::Filter::Point) == LLTexUnit::TFO_POINT, "Filter follows eTextureFilterOptions");
static_assert(U32(LL::RHI::AddressMode::ClampToEdge) == LLTexUnit::TAM_CLAMP
              && U32(LL::RHI::AddressMode::MirroredRepeat) == LLTexUnit::TAM_MIRROR
              && U32(LL::RHI::AddressMode::Repeat) == LLTexUnit::TAM_WRAP, "AddressMode follows eTextureAddressMode");

const U32 immediate_mask = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_TEXCOORD0;

static const GLenum sGLBlendFactor[] =
{
    GL_ONE,
    GL_ZERO,
    GL_DST_COLOR,
    GL_SRC_COLOR,
    GL_ONE_MINUS_DST_COLOR,
    GL_ONE_MINUS_SRC_COLOR,
    GL_DST_ALPHA,
    GL_SRC_ALPHA,
    GL_ONE_MINUS_DST_ALPHA,
    GL_ONE_MINUS_SRC_ALPHA,

    GL_ZERO // 'BF_UNDEF'
};

// [Viewer Lab] Whether GL is the backend chosen at startup, as llrendertarget.cpp's.
static bool gl_is_backend()
{
    return LL::RHI::viewerBackend() == LL::RHI::BackendType::GL;
}

// [Viewer Lab] The Metal backend's debug setting, which test runs set (LL_METAL_DEBUG=1), so a check there stops with
// LL_ERRS where a user's run warns; false where no Metal backend is built
static bool metal_debug()
{
#if LL_RHI_METAL
    return LL::RHI::metalDebug();
#else
    return false;
#endif
}

// [Viewer Lab] LLRender::setColorMask's glColorMask, moved here unchanged: the facade
// keeps its flush and cache, and nothing happens when GL is not the backend.
static void gl_color_mask(bool r, bool g, bool b, bool a)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_color_mask");
        return;
    }
    glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE, b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
}

// [Viewer Lab] LLRender::blendFunc's glBlendFunc, moved here unchanged: nothing when GL
// is not the backend.
static void gl_blend_func(GLenum sfactor, GLenum dfactor)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_blend_func");
        return;
    }
    glBlendFunc(sfactor, dfactor);
}

// [Viewer Lab] The four-factor LLRender::blendFunc's glBlendFuncSeparate, moved here unchanged:
// nothing when GL is not the backend.
static void gl_blend_func_separate(GLenum color_sfactor, GLenum color_dfactor, GLenum alpha_sfactor, GLenum alpha_dfactor)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_blend_func_separate");
        return;
    }
    glBlendFuncSeparate(color_sfactor, color_dfactor, alpha_sfactor, alpha_dfactor);
}

// [Viewer Lab] LLRender::setLineWidth's glLineWidth, moved here unchanged: nothing when
// GL is not the backend.
static void gl_line_width(F32 width)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_line_width");
        return;
    }
    glLineWidth(width);
}

// [Viewer Lab] LLRender::setViewport's glViewport, verbatim, as LLRenderTarget's and the raw callers':
// no flush, and nothing when GL is not the backend.
static void gl_viewport(S32 x, S32 y, S32 width, S32 height)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_viewport");
        return;
    }
    glViewport(x, y, width, height);
}

// [Viewer Lab] LLRender::setScissor's glScissor, verbatim; GL_SCISSOR_TEST stays LLGLState's:
// nothing when GL is not the backend.
static void gl_scissor(S32 x, S32 y, S32 width, S32 height)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_scissor");
        return;
    }
    glScissor(x, y, width, height);
}

// [Viewer Lab] The --debug-gl check of LLRender::setScissor's skip: GL's box against the known box the skip rests on.
// A difference is a GL write of the box that did not call invalidateScissor, one WARN line with the RHI tag (level 2's
// --debug-gl run fails on it). Reads GL state only, and nothing when GL is not the backend.
static void gl_check_scissor(const S32 known[4])
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_check_scissor");
        return;
    }
    GLint box[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_SCISSOR_BOX, box);
    if (box[0] != known[0] || box[1] != known[1] || box[2] != known[2] || box[3] != known[3])
    {
        LL_WARNS("RHI") << "setScissor: GL's box " << box[0] << " " << box[1] << " " << box[2] << " " << box[3]
                        << " is not the known box " << known[0] << " " << known[1] << " " << known[2] << " " << known[3]
                        << ": a GL write of the box did not call invalidateScissor" << LL_ENDL;
    }
}

// [Viewer Lab] LLRender::setCullFace's glCullFace, verbatim; GL_CULL_FACE stays LLGLState's:
// nothing when GL is not the backend.
static void gl_cull_face(GLenum face)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_cull_face");
        return;
    }
    glCullFace(face);
}

// [Viewer Lab] LLRender::setPolygonMode's glPolygonMode, verbatim: nothing when GL is not
// the backend.
static void gl_polygon_mode(GLenum face, GLenum mode)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_polygon_mode");
        return;
    }
    glPolygonMode(face, mode);
}

// [Viewer Lab] LLRender::setPolygonOffset's glPolygonOffset, verbatim; its enables for fill and lines stay LLGLState's:
// nothing when GL is not the backend.
static void gl_polygon_offset(F32 factor, F32 units)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_polygon_offset");
        return;
    }
    glPolygonOffset(factor, units);
}

// [Viewer Lab] LLRender::setPointSize's glPointSize, verbatim: nothing when GL is not the
// backend.
static void gl_point_size(F32 size)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_point_size");
        return;
    }
    glPointSize(size);
}

// [Viewer Lab] True while LLRender::OwnGLState lives; main thread only.
static bool sOwnGLState = false;

// [Viewer Lab] LLRender::dynamicStateCounts(); main thread only.
static LLRender::DynamicStateCounts sDynamicStateCounts;

// [Viewer Lab] The viewer's encoder while one of its passes is open, else null: off the main thread, inside OwnGLState (whose
// GL is already made), with no device, and outside a pass, where every backend refuses dynamic state with a WARN. The device is
// asked each time and never kept.
static LL::RHI::Encoder* encoder_in_pass()
{
    if (!on_main_thread() || sOwnGLState)
    {
        return nullptr;
    }
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device)
    {
        return nullptr;
    }
    LL::RHI::Encoder& encoder = device->encoder();
    return encoder.state().inPass() ? &encoder : nullptr;
}

namespace
{
    // [Viewer Lab] LLGLState's map, read with find() and never written, as the draw's GL_BLEND reader: an inserted key would
    // change what checkStates asks GL under gDebugGL.
    struct EnableMap : public LLGLState
    {
        static bool on(LLGLenum state)
        {
            const auto it = sStateMap.find(state);
            return it != sStateMap.end() && it->second == GL_TRUE;
        }
    };
}

// [Viewer Lab] LLTexUnit's GL in GL backend functions: each makes upstream's
// calls verbatim, and nothing when GL is not the viewer's backend. The callers keep every flush,
// activate, enable, stop_glerror and cache write where it was.
// [Viewer Lab] glActiveTexture of refreshState, activate and bindFast
static void gl_active_texture(S32 index)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_active_texture");
        return;
    }
    glActiveTexture(GL_TEXTURE0 + index);
}

// [Viewer Lab] glBindTexture of refreshState, bindFast, the binds, unbind and unbindFast
static void gl_bind_texture(LLTexUnit::eTextureType type, U32 name)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_bind_texture");
        return;
    }
    glBindTexture(sGLTextureType[type], name);
}

// [Viewer Lab] setTextureAddressModeFast's glTexParameteri calls on the active unit's texture, each axis with its
// own value (setTextureAddressModeFast gives all three alike)
static void gl_texture_address(LLTexUnit::eTextureType type, GLint wrap_s, GLint wrap_t, GLint wrap_r)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_texture_address");
        return;
    }
    glTexParameteri(sGLTextureType[type], GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(sGLTextureType[type], GL_TEXTURE_WRAP_T, wrap_t);
    if (type == LLTexUnit::TT_CUBE_MAP || type == LLTexUnit::TT_CUBE_MAP_ARRAY || type == LLTexUnit::TT_TEXTURE_3D)
    {
        glTexParameteri(sGLTextureType[type], GL_TEXTURE_WRAP_R, wrap_r);
    }
}

// [Viewer Lab] The raw glTexParameteri of pipeline.cpp and gltfscenemanager.cpp on the texture GL has bound for the type on
// GL's active unit, verbatim, behind the LLTexUnit::setActiveTexture* facades; nothing when GL is not the backend
static void gl_texture_mag_filter(LLTexUnit::eTextureType type, GLint value)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_texture_mag_filter");
        return;
    }
    glTexParameteri(sGLTextureType[type], GL_TEXTURE_MAG_FILTER, value);
}

static void gl_texture_min_filter(LLTexUnit::eTextureType type, GLint value)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_texture_min_filter");
        return;
    }
    glTexParameteri(sGLTextureType[type], GL_TEXTURE_MIN_FILTER, value);
}

static void gl_texture_wrap_st(LLTexUnit::eTextureType type, GLint wrap_s, GLint wrap_t)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_texture_wrap_st");
        return;
    }
    glTexParameteri(sGLTextureType[type], GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(sGLTextureType[type], GL_TEXTURE_WRAP_T, wrap_t);
}

// COMPARE_FUNC only with a function (func != 0), as unbindDeferredShader's GL_NONE sets the mode alone
static void gl_texture_compare(LLTexUnit::eTextureType type, GLint mode, GLint func)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_texture_compare");
        return;
    }
    glTexParameteri(sGLTextureType[type], GL_TEXTURE_COMPARE_MODE, mode);
    if (func != 0)
    {
        glTexParameteri(sGLTextureType[type], GL_TEXTURE_COMPARE_FUNC, func);
    }
}

// [Viewer Lab] Whether the backend filters anisotropically: the device's capability, and without the viewer's device
// (headless, llappearanceutility, the unit tests' fixture) gGLManager's flag, the check anisotropicFiltering replaces.
// Any thread: capabilities() is fixed when the device is made, and viewerDevice() is atomic.
static bool anisotropic_filtering()
{
    if (const LL::RHI::Device* device = LL::RHI::viewerDevice())
    {
        return device->capabilities().anisotropicFiltering;
    }
    return gGLManager.mHasAnisotropic;
}

// [Viewer Lab] setTextureFilteringOptionFast's MAG, MIN and MAX_ANISOTROPY calls on the active unit's texture, in upstream's
// order, from a sampling through toGLSampling, MAX_ANISOTROPY only with the anisotropy capability the caller gives;
// the GL adapter's interface bindings make the same calls through applyGLSampling
static void gl_texture_filter(LLTexUnit::eTextureType type, const LL::RHI::SamplerDesc& sampling, bool anisotropic)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_texture_filter");
        return;
    }
    const LLTexUnit::GLSampling gl = LLTexUnit::toGLSampling(sampling);
    glTexParameteri(sGLTextureType[type], GL_TEXTURE_MAG_FILTER, gl.mag);
    glTexParameteri(sGLTextureType[type], GL_TEXTURE_MIN_FILTER, gl.min);
    if (anisotropic)
    {
        glTexParameterf(sGLTextureType[type], GL_TEXTURE_MAX_ANISOTROPY, gl.anisotropy);
    }
}

// [Viewer Lab] The binding query of each texture type, in sGLTextureType's order, for gl_check_sampling
static const GLenum sGLTextureBinding[] =
{
    GL_TEXTURE_BINDING_2D,
    GL_TEXTURE_BINDING_RECTANGLE,
    GL_TEXTURE_BINDING_CUBE_MAP,
    GL_TEXTURE_BINDING_CUBE_MAP_ARRAY,
    GL_TEXTURE_BINDING_2D_MULTISAMPLE,
    GL_TEXTURE_BINDING_3D
};

// [Viewer Lab] The --debug-gl check of LLTexUnit's bindings (LLTexUnit's tell calls it under gDebugGL):
// when GL is the backend and the texture is bound for its type on GL's active unit, GL's MIN, MAG and, with the
// capability, MAX_ANISOTROPY (the filter part, bit 0 of known_parts) and WRAP_S, WRAP_T and, where gl_texture_address
// sets it, WRAP_R (the address part, bit 1) against toGLSampling(sampling), only for the parts known_parts marks; a
// difference is one WARN line with the RHI tag. Reads GL state only. True when it compared a part.
// MAG is compared also with the MAG part alone (bit 2), WRAP_T against addressV and WRAP_R against addressW.
bool gl_check_sampling(LLTexUnit::eTextureType type, U32 name, const LL::RHI::SamplerDesc& sampling, U8 known_parts)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_check_sampling");
        return false;
    }
    if (name == 0 || type >= LLTexUnit::TT_NONE || type == LLTexUnit::TT_MULTISAMPLE_TEXTURE || (known_parts & 7) == 0)
    {
        return false;
    }
    GLint bound = 0;
    glGetIntegerv(sGLTextureBinding[type], &bound);
    if (GLuint(bound) != name)
    {
        return false;
    }
    const GLenum target = sGLTextureType[type];
    const LLTexUnit::GLSampling given = LLTexUnit::toGLSampling(sampling);
    std::ostringstream differences;
    auto compare = [&](const char* what, GLenum parameter, GLint expected)
    {
        GLint value = 0;
        glGetTexParameteriv(target, parameter, &value);
        if (value != expected)
        {
            differences << " " << what << " given 0x" << std::hex << expected << ", GL 0x" << value << std::dec << ";";
        }
    };
    if (known_parts & 1)
    {
        compare("MIN_FILTER", GL_TEXTURE_MIN_FILTER, given.min);
    }
    if (known_parts & (1 | 4))
    {
        compare("MAG_FILTER", GL_TEXTURE_MAG_FILTER, given.mag);
    }
    if (known_parts & 1)
    {
        if (anisotropic_filtering())
        {
            GLfloat anisotropy = 0.f;
            glGetTexParameterfv(target, GL_TEXTURE_MAX_ANISOTROPY, &anisotropy);
            if (anisotropy != given.anisotropy)
            {
                differences << " MAX_ANISOTROPY given " << given.anisotropy << ", GL " << anisotropy << ";";
            }
        }
    }
    if (known_parts & 2)
    {
        compare("WRAP_S", GL_TEXTURE_WRAP_S, given.wrapS);
        compare("WRAP_T", GL_TEXTURE_WRAP_T, given.wrapT);
        if (type == LLTexUnit::TT_CUBE_MAP || type == LLTexUnit::TT_CUBE_MAP_ARRAY || type == LLTexUnit::TT_TEXTURE_3D)
        {
            compare("WRAP_R", GL_TEXTURE_WRAP_R, given.wrapR);
        }
    }
    const std::string found = differences.str();
    if (!found.empty())
    {
        GLint active = GL_TEXTURE0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        LL_WARNS("RHI") << "LLTexUnit's sampling of texture " << name << " on unit " << (active - GL_TEXTURE0)
                        << " differs from GL's:" << found << LL_ENDL;
    }
    return true;
}

// [Viewer Lab] debugTextureUnit's glGetIntegerv(GL_ACTIVE_TEXTURE) as a unit index; the unit's own index when GL is not
// the backend, so it warns only on GL
static S32 gl_active_texture_index(S32 own_index)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_active_texture_index");
        return own_index;
    }
    GLint activeTexture;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    return activeTexture - GL_TEXTURE0;
}

// [Viewer Lab] LLTexUnit's census of its bindings: written on the main thread only. Every write is on a path that
// has census_device()'s device, which only the main thread gets, so another thread's gGL (LLImageGLThread's) never reads
// or writes it, not even an add of 0; census_count keeps that rule for the counts every call makes
static LLTexUnit::BindCounts sBindCounts;

// [Viewer Lab] The viewer's device when LLTexUnit's records, census and tells run: on the main thread with the device,
// asked once per call with no reference kept, as LLVertexBuffer's draws ask; else null
static LL::RHI::Device* census_device()
{
    return on_main_thread() ? LL::RHI::viewerDevice() : nullptr;
}

// [Viewer Lab] One count of the census for a call with census_device()'s device; a call without it (another thread, or no
// device) leaves the census alone, so it is never written off the main thread
static void census_count(const LL::RHI::Device* device, U64& count)
{
    if (device)
    {
        ++count;
    }
}

// [Viewer Lab] The GL name of a texture of the viewer's device, 0 for one without (another backend's): the name bindTexture
// binds and keeps in mCurrTexture, and LLTexUnit's check that the name it binds is an image's or a target's texture.
// GL names stay the GL backend's state in llrender.
static U32 gl_name(const LL::RHI::Texture* texture)
{
    return LL::RHI::GLNames::of(texture);
}

// [Viewer Lab] The GL name LLTexUnit binds for an image, for the bind functions and bindFast:
// GL-backend state that stays inside llrender
static U32 gl_name(const LLImageGL* image)
{
    return LL::RHI::GLNames::of(image);
}

// [Viewer Lab] The GL backend's raw-name bind for a raw GL name, which has no texture of the device (no device, a
// caller's name, a refused description)
static bool bind_raw(LLTexUnit* unit, LLTexUnit::eTextureType type, U32 name, bool has_mips)
{
    return gl_bind_raw_name(unit, type, name, has_mips);
}

// [Viewer Lab] Whether the backend filters anisotropically, with the device a call has already asked for: its capability,
// else anisotropic_filtering()
static bool anisotropic_of(const LL::RHI::Device* device)
{
    return device ? device->capabilities().anisotropicFiltering : anisotropic_filtering();
}

// [Viewer Lab] The sampling the filter setters give GL for an option and a mip flag: the GL code's MAX_ANISOTROPY where GL
// sets it (with the capability, the maximum for anisotropic while LLImageGL::sGlobalUseAnisotropic is on), and 1 without
// the capability, where gGLManager.mMaxAnisotropy is 0 and the GL code sets nothing; the capability asked last, only for
// that case
static LL::RHI::SamplerDesc filter_sampling(LLTexUnit::eTextureFilterOptions option, bool mips, const LL::RHI::Device* device)
{
    LL::RHI::SamplerDesc sampling;
    sampling.filter = LL::RHI::Filter(option);
    sampling.mipmapped = mips;
    const bool anisotropic = option == LLTexUnit::TFO_ANISOTROPIC && LLImageGL::sGlobalUseAnisotropic
                             && anisotropic_of(device);
    sampling.maxAnisotropy = anisotropic ? gGLManager.mMaxAnisotropy : 1.f;
    return sampling;
}

// [Viewer Lab] The parts of a sampling LLTexUnit knows, in a texture's facade slot and its told records
static constexpr U8 FILTER_PART = 1;  // filter, mipmapped and maxAnisotropy
static constexpr U8 ADDRESS_PART = 2; // the address mode of every axis
static constexpr U8 MAG_PART = 4;     // magFilter alone, while the filter part is unknown

// [Viewer Lab] A sampling's known parts packed into 64 bits, the layout of LL::RHI::Texture's facade slot: bits 0-31
// maxAnisotropy's F32, 32-33 the filter, 34 mipmapped, 35 the filter part set, 36-37 the address mode, 38 the address
// part set; 0 means nothing set; 39-40 magFilter (with the filter part or the MAG part), 41 the MAG part set
// (only when the filter part is not), 42-43 addressV and 44-45 addressW (with the address part)
static U64 pack_sampling(const LL::RHI::SamplerDesc& sampling, U8 known)
{
    U64 packed = 0;
    if (known & FILTER_PART)
    {
        packed |= U64(std::bit_cast<U32>(sampling.maxAnisotropy));
        packed |= U64(sampling.filter) << 32;
        packed |= U64(sampling.mipmapped ? 1 : 0) << 34;
        packed |= U64(1) << 35;
        packed |= U64(sampling.magFilter) << 39; // [Viewer Lab]
    }
    else if (known & MAG_PART) // [Viewer Lab]
    {
        packed |= U64(sampling.magFilter) << 39;
        packed |= U64(1) << 41;
    }
    if (known & ADDRESS_PART)
    {
        packed |= U64(sampling.addressU) << 36;
        packed |= U64(1) << 38;
        packed |= U64(sampling.addressV) << 42; // [Viewer Lab]
        packed |= U64(sampling.addressW) << 44;
    }
    return packed;
}

// [Viewer Lab] A told record's sampling that is a texture's slot itself, with the filter and the address part set
static bool told_in_full(U64 told, U64 slot)
{
    constexpr U64 both = (U64(1) << 35) | (U64(1) << 38);
    return told == slot && (slot & both) == both;
}

// [Viewer Lab] The other way: the parts set, and SamplerDesc's defaults for the others
static LL::RHI::SamplerDesc unpack_sampling(U64 packed, U8& known)
{
    LL::RHI::SamplerDesc sampling;
    known = 0;
    if (packed & (U64(1) << 35))
    {
        sampling.maxAnisotropy = std::bit_cast<F32>(U32(packed));
        sampling.filter = LL::RHI::Filter((packed >> 32) & 3);
        sampling.mipmapped = ((packed >> 34) & 1) != 0;
        sampling.magFilter = LL::RHI::MagFilter((packed >> 39) & 3); // [Viewer Lab]
        known |= FILTER_PART;
    }
    else if (packed & (U64(1) << 41)) // [Viewer Lab]
    {
        sampling.magFilter = LL::RHI::MagFilter((packed >> 39) & 3);
        known |= MAG_PART;
    }
    if (packed & (U64(1) << 38))
    {
        sampling.addressU = LL::RHI::AddressMode((packed >> 36) & 3);
        sampling.addressV = LL::RHI::AddressMode((packed >> 42) & 3); // [Viewer Lab]
        sampling.addressW = LL::RHI::AddressMode((packed >> 44) & 3);
        known |= ADDRESS_PART;
    }
    return sampling;
}

// [Viewer Lab] The white image setWhiteImage gave, on the main thread: a never-destroyed holder, so no image is
// destroyed in static destruction (as llrendertarget.cpp's shared_depth_owners)
static LLPointer<LLImageGL>& white_image()
{
    static LLPointer<LLImageGL>* holder = new LLPointer<LLImageGL>();
    return *holder;
}

LLTexUnit::LLTexUnit(S32 index)
    : mCurrTexType(TT_NONE),
    mCurrTexture(0),
    mHasMipMaps(false),
    mIndex(index)
{
    llassert_always(index < (S32)LL_NUM_TEXTURE_LAYERS);
}

//static
U32 LLTexUnit::getInternalType(eTextureType type)
{
    return sGLTextureType[type];
}

// [Viewer Lab]
//static
LLTexUnit::BindCounts LLTexUnit::bindCounts()
{
    return sBindCounts;
}

// [Viewer Lab] The image's device texture when its GL name is the one bound, read on the main thread with the viewer's
// device only (mRHITexture is read only on the main thread), else null
//static
LL::RHI::Texture* LLTexUnit::textureOf(const LLImageGL* image, U32 name)
{
    return census_device() ? deviceTextureOf(image, name) : nullptr;
}

// [Viewer Lab] textureOf for a call that has the viewer's device from census_device(), which is asked only on the main
// thread, so no second check
//static
LL::RHI::Texture* LLTexUnit::deviceTextureOf(const LLImageGL* image, U32 name)
{
    if (!image)
    {
        return nullptr;
    }
    ++sBindCounts.textureReads;
    LL::RHI::Texture* texture = image->mRHITexture.get();
    return texture && gl_name(texture) == name ? texture : nullptr;
}

// [Viewer Lab] The id record of a binding
void LLTexUnit::recordBinding(U32 name, const LL::RHI::Texture* texture)
{
    mCurrTextureIdName = name;
    mCurrTextureId = texture ? texture->id() : 0;
}

// [Viewer Lab] The tests' log of the tells sent to the encoder; null in the viewer
static LLTexUnit::TellLog* sTellLog = nullptr;
U64 LLTexUnit::sTellShortcuts = 0; // [Viewer Lab]

//static
void LLTexUnit::setTellLog(TellLog* log)
{
    sTellLog = log;
}

// [Viewer Lab] The encoder gets the unit's binding inside OwnGLBinds, where the GL encoder only tracks it, when the device,
// the texture or the packed sampling differs from the unit's told record, and only below the device's channels.
// Under gDebugGL the known parts are checked against GL's state of the texture. The caller has asked for the
// device on the main thread.
LLTexUnit::Told LLTexUnit::tell(LL::RHI::Device* device, LL::RHI::Texture* texture, const LL::RHI::SamplerDesc& sampling,
                                U8 knownParts)
{
    if (U32(mIndex) >= device->capabilities().maxSamplersPerStage)
    {
        ++sBindCounts.beyondChannels;
        return Told::Beyond;
    }
    const U64 device_id = device->id();
    const U64 texture_id = texture ? texture->id() : 0;
    const U64 packed = pack_sampling(sampling, knownParts);
    if (mToldDevice == device_id && mToldTexture == texture_id && mToldSampling == packed)
    {
        return Told::Same;
    }
    {
        OwnGLBinds own;
        device->encoder().setTexture(U32(mIndex), texture, sampling);
    }
    mToldDevice = device_id;
    mToldTexture = texture_id;
    mToldSampling = packed;
    ++(texture ? sBindCounts.encoded : sBindCounts.encodedNull);
    if (sTellLog)
    {
        sTellLog->emplace_back(U32(mIndex), texture_id, sampling, knownParts);
    }
    if (gDebugGL && texture)
    {
        gl_check_sampling(mCurrTexType, gl_name(texture), sampling, knownParts);
    }
    return Told::Sent;
}

// [Viewer Lab] No texture of the device on the unit: an unbind, or a raw name GL binds
LLTexUnit::Told LLTexUnit::tellNothing(LL::RHI::Device* device)
{
    return tell(device, nullptr, LL::RHI::SamplerDesc(), 0);
}

// [Viewer Lab] A binding's tell once GL has bound it and applied its options: the texture with the sampling the facade
// knows for it, or nothing for a raw name. When the told record already has the texture with its slot, both
// parts set, the tell would find it the same: samplingOf reads such a slot alone and writes and counts nothing,
// pack_sampling gives back a slot it wrote, and the record's device passed the channel test, so neither is asked; an
// unbind to a told white and a repeated bind cost no sampling work
LLTexUnit::Told LLTexUnit::tellBinding(LL::RHI::Device* device, LL::RHI::Texture* texture, const LLImageGL* owner)
{
    if (!texture)
    {
        return tellNothing(device);
    }
    if (mToldTexture == texture->id() && told_in_full(mToldSampling, texture->facadeState()) && mToldDevice == device->id())
    {
        ++sTellShortcuts; // [Viewer Lab]
        return Told::Same;
    }
    U8 known = 0;
    const LL::RHI::SamplerDesc sampling = samplingOf(device, texture, owner, known);
    return tell(device, texture, sampling, known);
}

// [Viewer Lab] The sampling the facade knows for a texture: each part from the texture's slot; else from its owner
// image's options while they are applied (not dirty), written into the slot; else unknown, counted, left at
// SamplerDesc's default (Repeat is GL's initial wrap) and not marked known. The slot is read and written on the main
// thread only.
LL::RHI::SamplerDesc LLTexUnit::samplingOf(const LL::RHI::Device* device, LL::RHI::Texture* texture, const LLImageGL* owner,
                                           U8& knownParts)
{
    U8 known = 0;
    LL::RHI::SamplerDesc sampling = unpack_sampling(texture->facadeState(), known);
    // [Viewer Lab] A MAG part alone counts as no filter part; the owner's filter then keeps its value, and pack_sampling
    // carries it in the filter part
    if ((known & (FILTER_PART | ADDRESS_PART)) != (FILTER_PART | ADDRESS_PART) && owner && !owner->mTexOptionsDirty)
    {
        if (!(known & FILTER_PART))
        {
            const LL::RHI::SamplerDesc filter = filter_sampling(owner->mFilterOption, owner->mHasMipMaps, device);
            sampling.filter = filter.filter;
            sampling.mipmapped = filter.mipmapped;
            sampling.maxAnisotropy = filter.maxAnisotropy;
        }
        if (!(known & ADDRESS_PART))
        {
            const LL::RHI::AddressMode mode = LL::RHI::AddressMode(owner->mAddressMode);
            sampling.addressU = mode;
            sampling.addressV = mode;
            sampling.addressW = mode;
        }
        known = FILTER_PART | ADDRESS_PART;
        texture->setFacadeState(pack_sampling(sampling, known));
    }
    sBindCounts.unknownFilterParts += (known & FILTER_PART) ? 0 : 1;
    sBindCounts.unknownAddressParts += (known & ADDRESS_PART) ? 0 : 1;
    knownParts = known;
    return sampling;
}

// [Viewer Lab] The encoder tracker's texture on this unit's channel when its id is the given one, else null
LL::RHI::Texture* LLTexUnit::trackedTexture(LL::RHI::Device* device, U64 id) const
{
    if (id == 0)
    {
        return nullptr;
    }
    const auto& textures = device->encoder().state().textures();
    const auto bound = textures.find(U32(mIndex));
    return bound != textures.end() && bound->second->id() == id ? bound->second.get() : nullptr;
}

// [Viewer Lab] The texture a setter changes, as GL changes it: the device texture of the type bound on GL's active unit
// (gGL's index), found through that unit's record and the encoder's tracker; null when the active unit has another type
// or a raw name
//static
LL::RHI::Texture* LLTexUnit::activeBinding(LL::RHI::Device* device, eTextureType type)
{
    const LLTexUnit* active = gGL.getTexUnit(gGL.getCurrentTexUnitIndex());
    return active->mCurrTexType == type ? active->trackedTexture(device, active->effectiveId()) : nullptr;
}

// [Viewer Lab] After GL bound a name: the census and the id record
void LLTexUnit::noteBind(U32 name, const LL::RHI::Texture* texture)
{
    ++sBindCounts.binds;
    sBindCounts.rawBinds += texture ? 0 : 1;
    recordBinding(name, texture);
}

// [Viewer Lab] After GL's test skipped the binding, with no GL call and none of the bind's other work: a known texture
// whose id differs from the record's is adopted (the same GL name after a re-specification or a name GL reused,
// or a record a raw bind ended), the new texture taking the old one's slot when it has none; then the tell, as in every
// skip
void LLTexUnit::noteSkip(LL::RHI::Device* device, U32 name, LL::RHI::Texture* texture, const LLImageGL* owner)
{
    ++sBindCounts.skipped;
    if (!texture)
    {
        ++sBindCounts.rawBinds;
    }
    else if (const U64 effective = effectiveId(); effective != texture->id())
    {
        if (effective != 0)
        {
            ++sBindCounts.adoptedNewId;
            if (texture->facadeState() == 0)
            {
                if (const LL::RHI::Texture* old = trackedTexture(device, effective))
                {
                    texture->setFacadeState(old->facadeState());
                }
            }
        }
        else
        {
            ++sBindCounts.adoptedAfterRaw;
        }
        recordBinding(name, texture);
    }
    sBindCounts.retold += tellBinding(device, texture, owner) == Told::Sent ? 1 : 0;
}

// [Viewer Lab] Where unbind and unbindFast change the binding, after GL's call: the record ends, and the encoder gets the
// white image's texture for the 2D type when setWhiteImage gave one with sWhiteTexture's name, else nothing
void LLTexUnit::noteUnbind(eTextureType type)
{
    endRecord();
    LL::RHI::Device* device = census_device();
    if (!device)
    {
        return;
    }
    if (type == TT_TEXTURE)
    {
        ++sBindCounts.unbindsToWhite;
        const LLPointer<LLImageGL>& white = white_image();
        if (LL::RHI::Texture* texture = white.notNull() ? deviceTextureOf(white.get(), sWhiteTexture) : nullptr)
        {
            ++sBindCounts.whiteTold;
            tellBinding(device, texture, white.get());
            return;
        }
    }
    tellNothing(device);
}

// [Viewer Lab] A setter's address mode on GL's active unit's texture, and on the main thread with a texture its slot and
// the channels holding it
void LLTexUnit::applyAddressMode(eTextureAddressMode mode, eTextureType type, LL::RHI::Texture* texture, LL::RHI::Device* device)
{
    gl_texture_address(type, sGLAddressMode[mode], sGLAddressMode[mode], sGLAddressMode[mode]); // [Viewer Lab]
    if (!device)
    {
        return;
    }
    ++sBindCounts.samplingSets;
    if (!texture)
    {
        ++sBindCounts.samplingUntracked;
        return;
    }
    U8 known = 0;
    LL::RHI::SamplerDesc sampling = unpack_sampling(texture->facadeState(), known);
    sampling.addressU = sampling.addressV = sampling.addressW = LL::RHI::AddressMode(mode);
    texture->setFacadeState(pack_sampling(sampling, known | ADDRESS_PART));
    resend(device, texture, ADDRESS_PART, sampling);
}

// [Viewer Lab] A setter's filter on GL's active unit's texture with this unit's mip flag, and on the main thread with a
// texture its slot and the channels holding it
void LLTexUnit::applyFilteringOption(eTextureFilterOptions option, eTextureType type, LL::RHI::Texture* texture,
                                     LL::RHI::Device* device)
{
    const LL::RHI::SamplerDesc filter = filter_sampling(option, mHasMipMaps, device);
    gl_texture_filter(type, filter, anisotropic_of(device)); // [Viewer Lab]
    if (!device)
    {
        return;
    }
    ++sBindCounts.samplingSets;
    if (!texture)
    {
        ++sBindCounts.samplingUntracked;
        return;
    }
    U8 known = 0;
    LL::RHI::SamplerDesc sampling = unpack_sampling(texture->facadeState(), known);
    sampling.filter = filter.filter;
    sampling.mipmapped = filter.mipmapped;
    sampling.maxAnisotropy = filter.maxAnisotropy;
    // [Viewer Lab] gl_texture_filter wrote MAG from the filter, so an earlier MAG call is gone
    sampling.magFilter = LL::RHI::MagFilter::FromFilter;
    texture->setFacadeState(pack_sampling(sampling, U8((known & ~MAG_PART) | FILTER_PART)));
    resend(device, texture, FILTER_PART, sampling);
}

// [Viewer Lab] After a setter changed a texture's part: every unit that has told the encoder that texture tells it
// again with its told sampling's part replaced
//static
void LLTexUnit::resend(LL::RHI::Device* device, LL::RHI::Texture* texture, U8 part, const LL::RHI::SamplerDesc& sampling)
{
    const U64 device_id = device->id();
    const U64 texture_id = texture->id();
    U32 channels[LL_NUM_TEXTURE_LAYERS];
    U32 count = 0;
    for (const auto& [channel, bound] : device->encoder().state().textures())
    {
        if (bound.get() == texture && channel < LL_NUM_TEXTURE_LAYERS)
        {
            channels[count++] = channel;
        }
    }
    for (U32 i = 0; i < count; ++i)
    {
        LLTexUnit* unit = gGL.getTexUnit(channels[i]);
        if (unit->mToldDevice != device_id || unit->mToldTexture != texture_id)
        {
            continue; // the interface's binding of the channel
        }
        U8 known = 0;
        LL::RHI::SamplerDesc told = unpack_sampling(unit->mToldSampling, known);
        if (part & FILTER_PART)
        {
            told.filter = sampling.filter;
            told.mipmapped = sampling.mipmapped;
            told.maxAnisotropy = sampling.maxAnisotropy;
        }
        if (part & (FILTER_PART | MAG_PART)) // [Viewer Lab]
        {
            told.magFilter = sampling.magFilter;
        }
        if (part & ADDRESS_PART)
        {
            told.addressU = sampling.addressU;
            told.addressV = sampling.addressV;
            told.addressW = sampling.addressW;
        }
        sBindCounts.samplingResends += unit->tell(device, texture, told, known | part) == Told::Sent ? 1 : 0;
    }
}

void LLTexUnit::refreshState(void)
{
    // We set dirty to true so that the tex unit knows to ignore caching
    // and we reset the cached tex unit state

    gGL.flush();

    gl_active_texture(mIndex); // [Viewer Lab]

    if (mCurrTexType != TT_NONE)
    {
        gl_bind_texture(mCurrTexType, mCurrTexture); // [Viewer Lab]
    }
    else
    {
        gl_bind_texture(TT_TEXTURE, 0); // [Viewer Lab] GL_TEXTURE_2D
    }
}

void LLTexUnit::activate(void)
{
    if (mIndex < 0) return;

    if ((S32)gGL.mCurrTextureUnitIndex != mIndex || gGL.mDirty)
    {
        gGL.flush();
        gl_active_texture(mIndex); // [Viewer Lab]
        gGL.mCurrTextureUnitIndex = mIndex;
    }
}

void LLTexUnit::enable(eTextureType type)
{
    if (mIndex < 0) return;

    if ( (mCurrTexType != type || gGL.mDirty) && (type != TT_NONE) )
    {
        activate();
        if (mCurrTexType != TT_NONE && !gGL.mDirty)
        {
            disable(); // Force a disable of a previous texture type if it's enabled.
        }
        mCurrTexType = type;

        gGL.flush();
    }
}

void LLTexUnit::disable(void)
{
    if (mIndex < 0) return;

    if (mCurrTexType != TT_NONE)
    {
        unbind(mCurrTexType);
        mCurrTexType = TT_NONE;
    }
}

void LLTexUnit::bindFast(LLTexture* texture)
{
    LLImageGL* gl_tex = texture->getGLTexture();
    texture->setActive();
    gl_active_texture(mIndex); // [Viewer Lab]
    gGL.mCurrTextureUnitIndex = mIndex;
    mCurrTexture = gl_name(gl_tex); // [Viewer Lab]
    mCurrTexType = gl_tex->getTarget();
    LL::RHI::Device* device = mIndex >= 0 ? census_device() : nullptr; // [Viewer Lab]
    census_count(device, sBindCounts.calls);
    // [Viewer Lab] Missing: no GL name and no device texture without one, as the other bind functions test; on GL a
    // device texture always has a name, so the test is upstream's
    LL::RHI::Texture* rhi = device ? deviceTextureOf(gl_tex, mCurrTexture) : nullptr;
    const bool missing = !mCurrTexture && !rhi;
    if (missing) // [Viewer Lab]
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("MISSING TEXTURE");
        //if deleted, will re-generate it immediately
        texture->forceImmediateUpdate();
        gl_tex->forceUpdateBindStats();
        census_count(device, sBindCounts.nested); // [Viewer Lab]
        endRecord(); // [Viewer Lab] the binding ends, as mCurrTexture 0 ends GL's, also one without a GL name
        texture->bindDefaultImage(mIndex);
    }
    gl_bind_texture(gl_tex->getTarget(), mCurrTexture); // [Viewer Lab]
    // [Viewer Lab] The census and the record, as a bind; after a missing texture the binding is what the default image's
    // nested bind recorded, or nothing
    if (device && !missing)
    {
        noteBind(mCurrTexture, rhi);
    }
    else if (device)
    {
        rhi = trackedTexture(device, effectiveId());
    }
    mHasMipMaps = gl_tex->mHasMipMaps;
    if (gl_tex->mTexOptionsDirty)
    {
        gl_tex->mTexOptionsDirty = false;
        applyAddressMode(gl_tex->mAddressMode, gl_tex->getTarget(), rhi, device); // [Viewer Lab] as the Fast setter
        applyFilteringOption(gl_tex->mFilterOption, gl_tex->getTarget(), rhi, device); // [Viewer Lab] as the Fast setter
    }
    // [Viewer Lab] The tell after the GL calls: bindFast of what the unit has told already is a repeat; a missing texture
    // with no default bound leaves nothing on the unit
    if (device && !missing)
    {
        sBindCounts.fastRepeats += tellBinding(device, rhi, gl_tex) == Told::Same ? 1 : 0;
    }
    else if (device && mCurrTexture == 0 && effectiveId() == 0) // [Viewer Lab] nor a default without a GL name
    {
        endRecord();
        tellNothing(device);
    }
}

bool LLTexUnit::bind(LLTexture* texture, bool for_rendering, bool forceBind)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    stop_glerror();
    if (mIndex >= 0)
    {
        gGL.flush();

        LL::RHI::Device* device = census_device(); // [Viewer Lab]
        census_count(device, sBindCounts.calls);

        LLImageGL* gl_tex = NULL ;

        if (texture != NULL && (gl_tex = texture->getGLTexture()))
        {
            const U32 name = gl_name(gl_tex); // [Viewer Lab]
            LL::RHI::Texture* rhi = device ? deviceTextureOf(gl_tex, name) : nullptr; // [Viewer Lab]
            if (name != 0 || rhi) //if texture exists // [Viewer Lab] or its device texture without a GL name
            {
                //in audit, replace the selected texture by the default one.
                if (!sameBinding(name, rhi ? rhi->id() : 0) || forceBind) // [Viewer Lab] the GL test
                {
                    activate();
                    enable(gl_tex->getTarget());
                    mCurrTexture = name; // [Viewer Lab]
                    gl_bind_texture(gl_tex->getTarget(), mCurrTexture); // [Viewer Lab]
                    if (device)
                    {
                        noteBind(mCurrTexture, rhi); // [Viewer Lab]
                    }
                    if(gl_tex->updateBindStats())
                    {
                        texture->setActive() ;
                        texture->updateBindStatsForTester() ;
                    }
                    mHasMipMaps = gl_tex->mHasMipMaps;
                    if (gl_tex->mTexOptionsDirty)
                    {
                        gl_tex->mTexOptionsDirty = false;
                        setTextureAddressMode(gl_tex->mAddressMode, rhi); // [Viewer Lab] the bound texture's slot too
                        setTextureFilteringOption(gl_tex->mFilterOption, rhi); // [Viewer Lab]
                    }
                    if (device)
                    {
                        tellBinding(device, rhi, gl_tex); // [Viewer Lab]
                    }
                }
                else if (device)
                {
                    noteSkip(device, name, rhi, gl_tex); // [Viewer Lab] the same binding
                }
            }
            else
            {
                //if deleted, will re-generate it immediately
                texture->forceImmediateUpdate() ;

                gl_tex->forceUpdateBindStats() ;
                census_count(device, sBindCounts.nested); // [Viewer Lab]
                return texture->bindDefaultImage(mIndex);
            }
        }
        else
        {
            if (texture)
            {
                LL_DEBUGS() << "NULL LLTexUnit::bind GL image" << LL_ENDL;
            }
            else
            {
                LL_DEBUGS() << "NULL LLTexUnit::bind texture" << LL_ENDL;
            }
            census_count(device, sBindCounts.refused); // [Viewer Lab]
            return false;
        }
    }
    else
    { // mIndex < 0
        return false;
    }

    return true;
}

bool LLTexUnit::bind(LLImageGL* texture, bool for_rendering, bool forceBind, S32 usename)
{
    stop_glerror();
    if (mIndex < 0) return false;

    LL::RHI::Device* device = census_device(); // [Viewer Lab]
    census_count(device, sBindCounts.calls);

    U32 texname = usename ? usename : gl_name(texture); // [Viewer Lab]

    if(!texture)
    {
        LL_DEBUGS() << "NULL LLTexUnit::bind texture" << LL_ENDL;
        census_count(device, sBindCounts.refused); // [Viewer Lab]
        return false;
    }

    LL::RHI::Texture* rhi = device ? deviceTextureOf(texture, texname) : nullptr; // [Viewer Lab]
    if(!texname && !rhi) // [Viewer Lab] nor a device texture without a GL name
    {
        LLImageGL* fallback = LLImageGL::sDefaultGLTexture; // [Viewer Lab]
        if(fallback && (gl_name(fallback) || (device && deviceTextureOf(fallback, 0)))) // [Viewer Lab] or its device texture
        {
            census_count(device, sBindCounts.nested); // [Viewer Lab]
            return bind(LLImageGL::sDefaultGLTexture) ;
        }
        stop_glerror();
        census_count(device, sBindCounts.refused); // [Viewer Lab]
        return false ;
    }

    if (!sameBinding(texname, rhi ? rhi->id() : 0) || forceBind) // [Viewer Lab] the GL test
    {
        gGL.flush();
        stop_glerror();
        activate();
        stop_glerror();
        enable(texture->getTarget());
        stop_glerror();
        mCurrTexture = texname;
        gl_bind_texture(texture->getTarget(), mCurrTexture); // [Viewer Lab]
        if (device)
        {
            noteBind(mCurrTexture, rhi); // [Viewer Lab]
        }
        stop_glerror();
        texture->updateBindStats();
        mHasMipMaps = texture->mHasMipMaps;
        if (texture->mTexOptionsDirty)
        {
            stop_glerror();
            texture->mTexOptionsDirty = false;
            setTextureAddressMode(texture->mAddressMode, rhi); // [Viewer Lab] the bound texture's slot too
            setTextureFilteringOption(texture->mFilterOption, rhi); // [Viewer Lab]
            stop_glerror();
        }
        if (device)
        {
            tellBinding(device, rhi, texture); // [Viewer Lab]
        }
    }
    else if (device)
    {
        noteSkip(device, texname, rhi, texture); // [Viewer Lab]
    }

    stop_glerror();

    return true;
}

bool LLTexUnit::bind(LLCubeMap* cubeMap)
{
    if (mIndex < 0) return false;

    gGL.flush();

    LL::RHI::Device* device = census_device(); // [Viewer Lab]
    census_count(device, sBindCounts.calls);

    if (cubeMap == NULL)
    {
        LL_WARNS() << "NULL LLTexUnit::bind cubemap" << LL_ENDL;
        census_count(device, sBindCounts.refused); // [Viewer Lab]
        return false;
    }

    // [Viewer Lab] Face 0 holds the cube's GL name, and its device texture is the cube's
    LLImageGL* face = cubeMap->mImages[0];
    const U32 name = gl_name(face);
    LL::RHI::Texture* rhi = device ? deviceTextureOf(face, name) : nullptr;
    if (!sameBinding(name, rhi ? rhi->id() : 0)) // [Viewer Lab] the GL test
    {
        if (LLCubeMap::sUseCubeMaps)
        {
            activate();
            enable(LLTexUnit::TT_CUBE_MAP);
            mCurrTexture = name; // [Viewer Lab]
            gl_bind_texture(TT_CUBE_MAP, mCurrTexture); // [Viewer Lab] GL_TEXTURE_CUBE_MAP
            if (device)
            {
                noteBind(mCurrTexture, rhi); // [Viewer Lab]
            }
            mHasMipMaps = cubeMap->mImages[0]->mHasMipMaps;
            cubeMap->mImages[0]->updateBindStats();
            if (cubeMap->mImages[0]->mTexOptionsDirty)
            {
                cubeMap->mImages[0]->mTexOptionsDirty = false;
                setTextureAddressMode(cubeMap->mImages[0]->mAddressMode, rhi); // [Viewer Lab] the bound texture's slot too
                setTextureFilteringOption(cubeMap->mImages[0]->mFilterOption, rhi); // [Viewer Lab]
            }
            if (device)
            {
                tellBinding(device, rhi, face); // [Viewer Lab]
            }
            return true;
        }
        else
        {
            LL_WARNS() << "Using cube map without extension!" << LL_ENDL;
            census_count(device, sBindCounts.refused); // [Viewer Lab]
            return false;
        }
    }
    if (device)
    {
        noteSkip(device, name, rhi, face); // [Viewer Lab] the same binding
    }
    return true;
}

// LLRenderTarget is unavailible on the mapserver since it uses FBOs.
bool LLTexUnit::bind(LLRenderTarget* renderTarget, bool bindDepth)
{
    if (mIndex < 0) return false;

    gGL.flush();

    if (bindDepth)
    {
        // [Viewer Lab] On Metal the depth is the device's texture and the GL name is 0, so the check is on the texture: an
        // LL_ERRS under the Metal debug setting of the test runs, else a WARN once; GL's llassert below, on GL only
        if (!gl_is_backend() && !renderTarget->getRHIDepth())
        {
            if (metal_debug())
            {
                LL_ERRS("RHI") << "bind(target, true) on Metal: the target has no depth texture of the device" << LL_ENDL;
            }
            LL_WARNS_ONCE("RHI") << "bind(target, true) on Metal: the target has no depth texture of the device" << LL_ENDL;
        }
        llassert(!gl_is_backend() || renderTarget->getDepth()); // target MUST have a depth buffer attachment
    }

    // [Viewer Lab] bindManual's GL by the attachment's device texture; a raw attachment binds its raw name, as does a
    // sharer of another target's depth, whose name is 0
    if (LL::RHI::Texture* texture = bindDepth ? renderTarget->getRHIDepth() : renderTarget->getRHITexture(0))
    {
        bindTexture(renderTarget->getUsage(), texture);
    }
    else
    {
        LL::RHI::Device* device = census_device();
        const U32 name = bindDepth ? renderTarget->getDepth() : renderTarget->getTexture();
        const bool binds = mCurrTexture != name;
        bind_raw(this, renderTarget->getUsage(), name, false);
        if (device)
        {
            ++sBindCounts.calls;
            if (binds)
            {
                noteBind(name, nullptr);
                tellNothing(device);
            }
            else
            {
                noteSkip(device, name, nullptr, nullptr);
            }
        }
    }

    return true;
}

// [Viewer Lab] bindManual's GL for a texture of the device, by its GL name and sameBinding's test; on the main thread with the
// device the record and the tell. A null texture makes bindManual(type, 0)'s GL: name 0, a raw binding and
// the encoder told nothing, and false
bool LLTexUnit::bindTexture(eTextureType type, LL::RHI::Texture* texture, bool hasMips)
{
    if (mIndex < 0)
    {
        return false;
    }

    LL::RHI::Device* device = census_device();
    census_count(device, sBindCounts.calls);
    const U32 name = gl_name(texture);
    if (!sameBinding(name, device && texture ? texture->id() : 0))
    {
        gGL.flush();

        activate();
        enable(type);
        mCurrTexture = name;
        gl_bind_texture(type, name);
        mHasMipMaps = hasMips;
        if (device)
        {
            noteBind(name, texture);
            tellBinding(device, texture, nullptr);
        }
    }
    else if (device)
    {
        noteSkip(device, name, texture, nullptr);
    }
    return texture != nullptr;
}

// [Viewer Lab] bindManual's GL for an image's own GL name; its device texture (main thread) and its options for the
// sampling, or a raw bind of the name when it has none
bool LLTexUnit::bindTexture(eTextureType type, const LLImageGL* image, bool hasMips)
{
    if (mIndex < 0)
    {
        return false;
    }

    LL::RHI::Device* device = census_device();
    census_count(device, sBindCounts.calls);
    if (!image)
    {
        census_count(device, sBindCounts.refused);
        return false;
    }

    const U32 name = gl_name(image);
    LL::RHI::Texture* texture = device ? deviceTextureOf(image, name) : nullptr;
    if (!sameBinding(name, texture ? texture->id() : 0))
    {
        gGL.flush();

        activate();
        enable(type);
        mCurrTexture = name;
        gl_bind_texture(type, name);
        mHasMipMaps = hasMips;
        if (device)
        {
            noteBind(name, texture);
            tellBinding(device, texture, image);
        }
    }
    else if (device)
    {
        noteSkip(device, name, texture, image);
    }
    return true;
}

// [Viewer Lab] LLTexUnit::bindManual's body, with the unit as a parameter: the GL backend's raw-name bind
bool gl_bind_raw_name(LLTexUnit* unit, LLTexUnit::eTextureType type, U32 name, bool has_mips)
{
    if (unit->mIndex < 0)
    {
        return false;
    }

    if(unit->mCurrTexture != name)
    {
        gGL.flush();

        unit->activate();
        unit->enable(type);
        unit->mCurrTexture = name;
        gl_bind_texture(type, name); // [Viewer Lab] gated as the units' other binds; on GL the same call
        unit->mHasMipMaps = has_mips;
    }
    return true;
}

void LLTexUnit::unbind(eTextureType type)
{
    stop_glerror();

    if (mIndex < 0) return;

    //always flush and activate for consistency
    //   some code paths assume unbind always flushes and sets the active texture
    gGL.flush();
    activate();

    // Disabled caching of binding state.
    if (mCurrTexType == type)
    {
        mCurrTexture = 0;

        if (type == LLTexUnit::TT_TEXTURE)
        {
            gl_bind_texture(type, sWhiteTexture); // [Viewer Lab]
        }
        else
        {
            gl_bind_texture(type, 0); // [Viewer Lab]
        }
        noteUnbind(type); // [Viewer Lab]
        stop_glerror();
    }
}

void LLTexUnit::unbindFast(eTextureType type)
{
    activate();

    // Disabled caching of binding state.
    if (mCurrTexType == type)
    {
        mCurrTexture = 0;

        if (type == LLTexUnit::TT_TEXTURE)
        {
            gl_bind_texture(type, sWhiteTexture); // [Viewer Lab]
        }
        else
        {
            gl_bind_texture(type, 0); // [Viewer Lab]
        }
        noteUnbind(type); // [Viewer Lab]
    }
}

void LLTexUnit::setTextureAddressMode(eTextureAddressMode mode)
{
    if (mIndex < 0 || (mCurrTexture == 0 && effectiveId() == 0)) return; // [Viewer Lab] or a binding without a GL name

    gGL.flush();

    activate();

    setTextureAddressModeFast(mode, mCurrTexType);
}

// [Viewer Lab] The public setter for a texture the caller knows
void LLTexUnit::setTextureAddressMode(eTextureAddressMode mode, LL::RHI::Texture* texture)
{
    if (mIndex < 0 || (mCurrTexture == 0 && effectiveId() == 0)) return;

    gGL.flush();

    activate();

    applyAddressMode(mode, mCurrTexType, texture, census_device());
}

void LLTexUnit::setTextureAddressModeFast(eTextureAddressMode mode, eTextureType tex_type)
{
    LL::RHI::Device* device = census_device(); // [Viewer Lab]
    applyAddressMode(mode, tex_type, device ? activeBinding(device, tex_type) : nullptr, device); // [Viewer Lab]
}

void LLTexUnit::setTextureFilteringOption(LLTexUnit::eTextureFilterOptions option)
{
    if (mIndex < 0 || (mCurrTexture == 0 && effectiveId() == 0) || mCurrTexType == LLTexUnit::TT_MULTISAMPLE_TEXTURE) return; // [Viewer Lab]

    gGL.flush();

    setTextureFilteringOptionFast(option, mCurrTexType);
}

// [Viewer Lab] The public setter for a texture the caller knows
void LLTexUnit::setTextureFilteringOption(eTextureFilterOptions option, LL::RHI::Texture* texture)
{
    if (mIndex < 0 || (mCurrTexture == 0 && effectiveId() == 0) || mCurrTexType == LLTexUnit::TT_MULTISAMPLE_TEXTURE) return;

    gGL.flush();

    applyFilteringOption(option, mCurrTexType, texture, census_device());
}

void LLTexUnit::setTextureFilteringOptionFast(LLTexUnit::eTextureFilterOptions option, eTextureType tex_type)
{
    LL::RHI::Device* device = census_device(); // [Viewer Lab]
    applyFilteringOption(option, tex_type, device ? activeBinding(device, tex_type) : nullptr, device); // [Viewer Lab]
}

// [Viewer Lab] The mag filter a filter gives GL by itself
static LL::RHI::MagFilter effective_mag(LL::RHI::Filter filter)
{
    return filter == LL::RHI::Filter::Point ? LL::RHI::MagFilter::Nearest : LL::RHI::MagFilter::Linear;
}

// [Viewer Lab] A wrap value's address mode; false for a value sGLAddressMode does not hold
static bool address_mode_of(U32 wrap, LL::RHI::AddressMode& mode)
{
    for (U32 i = 0; i < 3; ++i)
    {
        if (U32(sGLAddressMode[i]) == wrap)
        {
            mode = LL::RHI::AddressMode(i);
            return true;
        }
    }
    return false;
}

// [Viewer Lab] The compare facade: allocateShadowBuffer's and unbindDeferredShader's compare calls, verbatim; no slot,
// no tell and no census (the GL adapter sets the compare mode from a draw's sampler itself)
//static
void LLTexUnit::setActiveTextureCompare(eTextureType type, U32 gl_mode, U32 gl_func)
{
    llassert(type < TT_NONE);
    gl_texture_compare(type, GLint(gl_mode), GLint(gl_func));
}

// [Viewer Lab] The MAG facade: the raw call; then, on the main thread with the device, the slot of the device
// texture gGL's active unit holds for the type: the filter part's magFilter, stored as FromFilter when it is the filter's
// own mag so the same GL keeps one sampling, else the MAG part, always explicit; any other value makes
// both unknown. The channels holding the texture are told again.
//static
void LLTexUnit::setActiveTextureMagFilter(eTextureType type, U32 gl_mag)
{
    llassert(type < TT_NONE);
    gl_texture_mag_filter(type, GLint(gl_mag));
    LL::RHI::Device* device = census_device();
    if (!device)
    {
        return;
    }
    ++sBindCounts.samplingSets;
    LL::RHI::Texture* texture = activeBinding(device, type);
    if (!texture)
    {
        ++sBindCounts.samplingUntracked;
        return;
    }
    if (type == TT_MULTISAMPLE_TEXTURE)
    {
        return; // GL only, as setTextureFilteringOption skips it
    }
    U8 known = 0;
    LL::RHI::SamplerDesc sampling = unpack_sampling(texture->facadeState(), known);
    if (gl_mag != GL_NEAREST && gl_mag != GL_LINEAR)
    {
        texture->setFacadeState(pack_sampling(sampling, U8(known & ~(FILTER_PART | MAG_PART))));
        return;
    }
    const LL::RHI::MagFilter mag = gl_mag == GL_NEAREST ? LL::RHI::MagFilter::Nearest : LL::RHI::MagFilter::Linear;
    if (known & FILTER_PART)
    {
        sampling.magFilter = mag == effective_mag(sampling.filter) ? LL::RHI::MagFilter::FromFilter : mag;
        texture->setFacadeState(pack_sampling(sampling, known));
        resend(device, texture, FILTER_PART, sampling);
        return;
    }
    sampling.magFilter = mag;
    texture->setFacadeState(pack_sampling(sampling, U8(known | MAG_PART)));
    resend(device, texture, MAG_PART, sampling);
}

// [Viewer Lab] The MIN facade: the raw call; then, as the MAG setter, the slot's filter and mip flag when
// the filter part is known and toGLSampling does not give this MIN already. A FromFilter magFilter is pinned to the old
// filter's mag first, so MIN never moves MAG in the slot, as in GL, and an explicit one equal to the new filter's mag
// becomes FromFilter. A value toGLSampling never gives makes the filter part unknown; maxAnisotropy stays.
//static
void LLTexUnit::setActiveTextureMinFilter(eTextureType type, U32 gl_min)
{
    llassert(type < TT_NONE);
    gl_texture_min_filter(type, GLint(gl_min));
    LL::RHI::Device* device = census_device();
    if (!device)
    {
        return;
    }
    ++sBindCounts.samplingSets;
    LL::RHI::Texture* texture = activeBinding(device, type);
    if (!texture)
    {
        ++sBindCounts.samplingUntracked;
        return;
    }
    if (type == TT_MULTISAMPLE_TEXTURE)
    {
        return;
    }
    U8 known = 0;
    LL::RHI::SamplerDesc sampling = unpack_sampling(texture->facadeState(), known);
    if (!(known & FILTER_PART))
    {
        return; // the filter part stays unknown, and a MAG part as it is
    }
    if (U32(toGLSampling(sampling).min) == gl_min)
    {
        return;
    }
    LL::RHI::Filter filter = LL::RHI::Filter::Point;
    bool mips = false;
    switch (gl_min)
    {
    case GL_NEAREST:
        break;
    case GL_LINEAR:
        filter = LL::RHI::Filter::Bilinear;
        break;
    case GL_NEAREST_MIPMAP_NEAREST:
        mips = true;
        break;
    case GL_LINEAR_MIPMAP_NEAREST:
        filter = LL::RHI::Filter::Bilinear;
        mips = true;
        break;
    case GL_LINEAR_MIPMAP_LINEAR:
        filter = LL::RHI::Filter::Trilinear;
        mips = true;
        break;
    default:
        texture->setFacadeState(pack_sampling(sampling, U8(known & ~FILTER_PART)));
        return;
    }
    if (sampling.magFilter == LL::RHI::MagFilter::FromFilter)
    {
        sampling.magFilter = effective_mag(sampling.filter);
    }
    sampling.filter = filter;
    sampling.mipmapped = mips;
    if (sampling.magFilter == effective_mag(filter))
    {
        sampling.magFilter = LL::RHI::MagFilter::FromFilter;
    }
    texture->setFacadeState(pack_sampling(sampling, known));
    resend(device, texture, FILTER_PART, sampling);
}

// [Viewer Lab] The wrap facade: the raw WRAP_S and WRAP_T, never WRAP_R; then, as the filter facades, the slot's U and V
// when both values are address modes. An unknown address part becomes known only for TT_TEXTURE, whose WRAP_R stays GL's initial
// REPEAT (gl_texture_address sets R only for cube and 3D types); a rectangle texture (CLAMP_TO_EDGE initially, REPEAT
// refused), cube and 3D textures keep it unknown. Any other value makes the address part unknown.
//static
void LLTexUnit::setActiveTextureWrapST(eTextureType type, U32 gl_wrap_s, U32 gl_wrap_t)
{
    llassert(type < TT_NONE);
    gl_texture_wrap_st(type, GLint(gl_wrap_s), GLint(gl_wrap_t));
    LL::RHI::Device* device = census_device();
    if (!device)
    {
        return;
    }
    ++sBindCounts.samplingSets;
    LL::RHI::Texture* texture = activeBinding(device, type);
    if (!texture)
    {
        ++sBindCounts.samplingUntracked;
        return;
    }
    if (type == TT_MULTISAMPLE_TEXTURE)
    {
        return;
    }
    U8 known = 0;
    LL::RHI::SamplerDesc sampling = unpack_sampling(texture->facadeState(), known);
    LL::RHI::AddressMode s = LL::RHI::AddressMode::Repeat;
    LL::RHI::AddressMode t = LL::RHI::AddressMode::Repeat;
    if (!address_mode_of(gl_wrap_s, s) || !address_mode_of(gl_wrap_t, t))
    {
        texture->setFacadeState(pack_sampling(sampling, U8(known & ~ADDRESS_PART)));
        return;
    }
    if (!(known & ADDRESS_PART))
    {
        if (type != TT_TEXTURE)
        {
            return;
        }
        sampling.addressW = LL::RHI::AddressMode::Repeat;
    }
    sampling.addressU = s;
    sampling.addressV = t;
    texture->setFacadeState(pack_sampling(sampling, U8(known | ADDRESS_PART)));
    resend(device, texture, ADDRESS_PART, sampling);
}

// [Viewer Lab]
LL::RHI::SamplerDesc LLTexUnit::filterSampling(eTextureFilterOptions option) const
{
    return filter_sampling(option, mHasMipMaps, census_device());
}

// [Viewer Lab]
//static
LLTexUnit::GLSampling LLTexUnit::toGLSampling(const LL::RHI::SamplerDesc& sampling)
{
    using LL::RHI::Filter;
    GLSampling gl;
    // [Viewer Lab] magFilter Nearest or Linear overrides the filter's own mag
    gl.mag = sampling.magFilter == LL::RHI::MagFilter::Nearest  ? GL_NEAREST
             : sampling.magFilter == LL::RHI::MagFilter::Linear ? GL_LINEAR
             : sampling.filter == Filter::Point                 ? GL_NEAREST
                                                                : GL_LINEAR;
    if (sampling.filter >= Filter::Trilinear && sampling.mipmapped)
    {
        gl.min = GL_LINEAR_MIPMAP_LINEAR;
    }
    else if (sampling.filter >= Filter::Bilinear)
    {
        gl.min = sampling.mipmapped ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
    }
    else
    {
        gl.min = sampling.mipmapped ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
    }
    gl.anisotropy = sampling.maxAnisotropy;
    gl.wrapS = sGLAddressMode[U32(sampling.addressU)]; // [Viewer Lab] each axis its own
    gl.wrapT = sGLAddressMode[U32(sampling.addressV)];
    gl.wrapR = sGLAddressMode[U32(sampling.addressW)];
    return gl;
}

// [Viewer Lab]
//static
void LLTexUnit::applyGLSampling(eTextureType type, const LL::RHI::SamplerDesc& sampling)
{
    gl_texture_filter(type, sampling, anisotropic_filtering());
    const GLSampling gl = toGLSampling(sampling); // [Viewer Lab] each axis its own
    gl_texture_address(type, gl.wrapS, gl.wrapT, gl.wrapR);
}

// [Viewer Lab] True on this thread while LLTexUnit gives the encoder its own binding
static thread_local bool sMakesOwnGLBinds = false;

LLTexUnit::OwnGLBinds::OwnGLBinds()
:   mPrevious(sMakesOwnGLBinds)
{
    sMakesOwnGLBinds = true;
}

LLTexUnit::OwnGLBinds::~OwnGLBinds()
{
    sMakesOwnGLBinds = mPrevious;
}

//static
bool LLTexUnit::makesOwnGLBinds()
{
    return sMakesOwnGLBinds;
}

// [Viewer Lab]
U64 LLTexUnit::getCurrTextureId() const
{
    return effectiveId();
}

// [Viewer Lab]
LL::RHI::Texture* LLTexUnit::getCurrRHITexture() const
{
    LL::RHI::Device* device = census_device();
    return device ? trackedTexture(device, effectiveId()) : nullptr;
}

// [Viewer Lab] The GL test for an image, for LLImageGL's setters: its GL name when it has one, as upstream; else its
// device texture's id against the record
bool LLTexUnit::isBound(const LLImageGL* image) const
{
    const U32 name = gl_name(image);
    if (name != 0)
    {
        return mCurrTexture == name;
    }
    const LL::RHI::Texture* texture = textureOf(image, 0);
    return sameBinding(0, texture ? texture->id() : 0);
}

// [Viewer Lab]
//static
void LLTexUnit::setWhiteImage(LLImageGL* image)
{
    llassert(on_main_thread());
    white_image() = image;
    sWhiteTexture = image ? gl_name(image) : 0;
}

// [Viewer Lab] Every unit whose told record has the texture tells nothing, inside OwnGLBinds as every tell, so the encoder's
// tracker lets go of it before its owner does
//static
void LLTexUnit::releaseTexture(const LL::RHI::Texture* texture)
{
    // [Viewer Lab] The display lists let go first, also without the device, so the owner's release queues the GL name in
    // the same frame as upstream
    if (texture && on_main_thread())
    {
        LLVertexBufferData::releaseTexture(texture->id());
    }
    LL::RHI::Device* device = texture ? census_device() : nullptr;
    if (!device)
    {
        return;
    }
    const U64 device_id = device->id();
    const U64 texture_id = texture->id();
    const U32 units = llmin(LL_NUM_TEXTURE_LAYERS, device->capabilities().maxSamplersPerStage);
    for (U32 index = 0; index < units; ++index)
    {
        LLTexUnit* unit = gGL.getTexUnit(index);
        if (unit->mToldDevice == device_id && unit->mToldTexture == texture_id)
        {
            unit->tellNothing(device);
        }
    }
}

// [Viewer Lab]
//static
void LLTexUnit::unknownSamplingInUse(U32& filter, U32& address)
{
    filter = 0;
    address = 0;
    LL::RHI::Device* device = census_device();
    if (!device)
    {
        return;
    }
    for (const auto& [channel, texture] : device->encoder().state().textures())
    {
        U8 known = 0;
        unpack_sampling(texture->facadeState(), known);
        filter += (known & FILTER_PART) ? 0 : 1;
        address += (known & ADDRESS_PART) ? 0 : 1;
    }
}

GLint LLTexUnit::getTextureSource(eTextureBlendSrc src)
{
    switch(src)
    {
        // All four cases should return the same value.
        case TBS_PREV_COLOR:
        case TBS_PREV_ALPHA:
        case TBS_ONE_MINUS_PREV_COLOR:
        case TBS_ONE_MINUS_PREV_ALPHA:
            return GL_PREVIOUS;

        // All four cases should return the same value.
        case TBS_TEX_COLOR:
        case TBS_TEX_ALPHA:
        case TBS_ONE_MINUS_TEX_COLOR:
        case TBS_ONE_MINUS_TEX_ALPHA:
            return GL_TEXTURE;

        // All four cases should return the same value.
        case TBS_VERT_COLOR:
        case TBS_VERT_ALPHA:
        case TBS_ONE_MINUS_VERT_COLOR:
        case TBS_ONE_MINUS_VERT_ALPHA:
            return GL_PRIMARY_COLOR;

        // All four cases should return the same value.
        case TBS_CONST_COLOR:
        case TBS_CONST_ALPHA:
        case TBS_ONE_MINUS_CONST_COLOR:
        case TBS_ONE_MINUS_CONST_ALPHA:
            return GL_CONSTANT;

        default:
            LL_WARNS() << "Unknown eTextureBlendSrc: " << src << ".  Using Vertex Color instead." << LL_ENDL;
            return GL_PRIMARY_COLOR;
    }
}

GLint LLTexUnit::getTextureSourceType(eTextureBlendSrc src, bool isAlpha)
{
    switch(src)
    {
        // All four cases should return the same value.
        case TBS_PREV_COLOR:
        case TBS_TEX_COLOR:
        case TBS_VERT_COLOR:
        case TBS_CONST_COLOR:
            return (isAlpha) ? GL_SRC_ALPHA: GL_SRC_COLOR;

        // All four cases should return the same value.
        case TBS_PREV_ALPHA:
        case TBS_TEX_ALPHA:
        case TBS_VERT_ALPHA:
        case TBS_CONST_ALPHA:
            return GL_SRC_ALPHA;

        // All four cases should return the same value.
        case TBS_ONE_MINUS_PREV_COLOR:
        case TBS_ONE_MINUS_TEX_COLOR:
        case TBS_ONE_MINUS_VERT_COLOR:
        case TBS_ONE_MINUS_CONST_COLOR:
            return (isAlpha) ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE_MINUS_SRC_COLOR;

        // All four cases should return the same value.
        case TBS_ONE_MINUS_PREV_ALPHA:
        case TBS_ONE_MINUS_TEX_ALPHA:
        case TBS_ONE_MINUS_VERT_ALPHA:
        case TBS_ONE_MINUS_CONST_ALPHA:
            return GL_ONE_MINUS_SRC_ALPHA;

        default:
            LL_WARNS() << "Unknown eTextureBlendSrc: " << src << ".  Using Source Color or Alpha instead." << LL_ENDL;
            return (isAlpha) ? GL_SRC_ALPHA: GL_SRC_COLOR;
    }
}

// Useful for debugging that you've manually assigned a texture operation to the correct
// texture unit based on the currently set active texture in opengl.
void LLTexUnit::debugTextureUnit(void)
{
    if (mIndex < 0) return;

    const S32 activeTexture = gl_active_texture_index(mIndex); // [Viewer Lab] the unit index, not the GL enum
    if (mIndex != activeTexture)
    {
        U32 set_unit = activeTexture;
        LL_WARNS() << "Incorrect Texture Unit!  Expected: " << set_unit << " Actual: " << mIndex << LL_ENDL;
    }
}

LLLightState::LLLightState(S32 index)
: mIndex(index),
  mEnabled(false),
  mConstantAtten(1.f),
  mLinearAtten(0.f),
  mQuadraticAtten(0.f),
  mSpotExponent(0.f),
  mSpotCutoff(180.f)
{
    if (mIndex == 0)
    {
        mDiffuse.set(1,1,1,1);
        mDiffuseB.set(0,0,0,0);
        mSpecular.set(1,1,1,1);
    }

    mSunIsPrimary = true;

    mAmbient.set(0,0,0,1);
    mPosition.set(0,0,1,0);
    mSpotDirection.set(0,0,-1);
}

void LLLightState::enable()
{
    mEnabled = true;
}

void LLLightState::disable()
{
    mEnabled = false;
}

void LLLightState::setDiffuse(const LLColor4& diffuse)
{
    if (mDiffuse != diffuse)
    {
        ++gGL.mLightHash;
        mDiffuse = diffuse;
    }
}

void LLLightState::setDiffuseB(const LLColor4& diffuse)
{
    if (mDiffuseB != diffuse)
    {
        ++gGL.mLightHash;
        mDiffuseB = diffuse;
    }
}

void LLLightState::setSunPrimary(bool v)
{
    if (mSunIsPrimary != v)
    {
        ++gGL.mLightHash;
        mSunIsPrimary = v;
    }
}

void LLLightState::setSize(F32 v)
{
    if (mSize != v)
    {
        ++gGL.mLightHash;
        mSize = v;
    }
}

void LLLightState::setFalloff(F32 v)
{
    if (mFalloff != v)
    {
        ++gGL.mLightHash;
        mFalloff = v;
    }
}

void LLLightState::setAmbient(const LLColor4& ambient)
{
    if (mAmbient != ambient)
    {
        ++gGL.mLightHash;
        mAmbient = ambient;
    }
}

void LLLightState::setSpecular(const LLColor4& specular)
{
    if (mSpecular != specular)
    {
        ++gGL.mLightHash;
        mSpecular = specular;
    }
}

void LLLightState::setPosition(const LLVector4& position)
{
    //always set position because modelview matrix may have changed
    ++gGL.mLightHash;
    mPosition = position;
    //transform position by current modelview matrix
    glm::vec4 pos(position);
    pos = gGL.getModelviewMatrix() * pos;
    mPosition.set(glm::value_ptr(pos));
}

void LLLightState::setConstantAttenuation(const F32& atten)
{
    if (mConstantAtten != atten)
    {
        mConstantAtten = atten;
        ++gGL.mLightHash;
    }
}

void LLLightState::setLinearAttenuation(const F32& atten)
{
    if (mLinearAtten != atten)
    {
        ++gGL.mLightHash;
        mLinearAtten = atten;
    }
}

void LLLightState::setQuadraticAttenuation(const F32& atten)
{
    if (mQuadraticAtten != atten)
    {
        ++gGL.mLightHash;
        mQuadraticAtten = atten;
    }
}

void LLLightState::setSpotExponent(const F32& exponent)
{
    if (mSpotExponent != exponent)
    {
        ++gGL.mLightHash;
        mSpotExponent = exponent;
    }
}

void LLLightState::setSpotCutoff(const F32& cutoff)
{
    if (mSpotCutoff != cutoff)
    {
        ++gGL.mLightHash;
        mSpotCutoff = cutoff;
    }
}

void LLLightState::setSpotDirection(const LLVector3& direction)
{
    //always set direction because modelview matrix may have changed
    ++gGL.mLightHash;

    //transform direction by current modelview matrix
    glm::vec3 dir(direction);
    const glm::mat3 mat(gGL.getModelviewMatrix());
    dir = mat * dir;

    mSpotDirection.set(glm::value_ptr(dir));
}

LLRender::LLRender()
  : mDirty(false),
    mCount(0),
    mMode(LLRender::TRIANGLES),
    mCurrTextureUnitIndex(0),
    mLineWidth(1.f), // <FS> Line width OGL core profile fix by Rye Mutt
    // <FS:Ansariel> Don't ignore OpenGL max line width
    mMaxLineWidthSmooth(1.f),
    mMaxLineWidthAliased(1.f)
    // </FS:Ansariel>
{
    for (U32 i = 0; i < LL_NUM_TEXTURE_LAYERS; i++)
    {
        mTexUnits[i].mIndex = i;
    }

    for (U32 i = 0; i < LL_NUM_LIGHT_UNITS; ++i)
    {
        mLightState[i].mIndex = i;
    }

    for (U32 i = 0; i < 4; i++)
    {
        mCurrColorMask[i] = true;
    }

    mCurrBlendColorSFactor = BF_UNDEF;
    mCurrBlendAlphaSFactor = BF_UNDEF;
    mCurrBlendColorDFactor = BF_UNDEF;
    mCurrBlendAlphaDFactor = BF_UNDEF;

    mMatrixMode = LLRender::MM_MODELVIEW;

    for (U32 i = 0; i < NUM_MATRIX_MODES; ++i)
    {
        for (U32 j = 0; j < LL_MATRIX_STACK_DEPTH; ++j)
        {
            mMatrix[i][j] = glm::identity<glm::mat4>();
        }
        mMatIdx[i] = 0;
        mMatHash[i] = 0;
        mCurMatHash[i] = 0xFFFFFFFF;
    }

    mLightHash = 0;
}

LLRender::~LLRender()
{
    shutdown();
}

// [Viewer Lab] LLRender::init's GL before the blend state, verbatim: the debug
// output callback where the driver has it (gl_debug_callback stays GL backend), and the pixel store alignment of 1 that
// uploads and readbacks rely on. Nothing when GL is not the backend.
static void gl_init_debug_output_and_pixel_store()
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_init_debug_output_and_pixel_store");
        return;
    }
#if LL_WINDOWS
    if (gGLManager.mHasDebugOutput && gDebugGL)
    { //setup debug output callback
        //glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_LOW_ARB, 0, NULL, GL_TRUE);
        glDebugMessageCallback((GLDEBUGPROC) gl_debug_callback, NULL);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    }
#endif

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

// [Viewer Lab] LLRender::init's GL after the blend state, verbatim: the cull face,
// seamless cube maps and the dummy vertex array object of the core profile. False, as init's return, when the driver has
// no vertex array objects; true and nothing when GL is not the backend.
static bool gl_init_cull_cube_map_and_vao()
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrender:gl_init_cull_cube_map_and_vao");
        return true;
    }
    glCullFace(GL_BACK);

    // necessary for reflection maps
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

#if LL_WINDOWS
    if (glGenVertexArrays == nullptr)
    {
        return false;
    }
#endif

    { //bind a dummy vertex array object so we're core profile compliant
        U32 ret;
        glGenVertexArrays(1, &ret);
        glBindVertexArray(ret);
    }
    return true;
}

bool LLRender::init(bool needs_vertex_buffer)
{
    invalidateScissor(); // [Viewer Lab] a context of its own may have another box
    gl_init_debug_output_and_pixel_store(); // [Viewer Lab]

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setAmbientLightColor(LLColor4::black);

    if (!gl_init_cull_cube_map_and_vao()) // [Viewer Lab]
    {
        return false;
    }

    if (needs_vertex_buffer)
    {
        initVertexBuffer();
    }

    // <FS:Ansariel> Don't ignore OpenGL max line width
    if (gl_is_backend()) // [Viewer Lab]
    {
        GLfloat range[2];
        glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, range);
        stop_glerror();
        mMaxLineWidthAliased = range[1];
        glGetFloatv(GL_SMOOTH_LINE_WIDTH_RANGE, range);
        stop_glerror();
        mMaxLineWidthSmooth = range[1];
    }
    else
    {
        // [Viewer Lab] The aliased range the startup's GL query found, for smooth lines too
        LL_GL_GATED("llrender:LLRender::init");
        mMaxLineWidthAliased = gGLManager.mAliasedLineRange[1];
        mMaxLineWidthSmooth  = gGLManager.mAliasedLineRange[1];
    }
    // </FS:Ansariel>

    return true;
}

void LLRender::initVertexBuffer()
{
    llassert_always(mBuffer.isNull()) ;
    stop_glerror();
    mBuffer = new LLVertexBuffer(immediate_mask);
    // <FS:Ansariel> Warn in case of allocation failure
    //mBuffer->allocateBuffer(4096, 0);
    if (!mBuffer->allocateBuffer(4096, 0))
    {
        // If this doesn't work, we're knee-deep in trouble!
        LL_WARNS() << "Failed to allocate Vertex Buffer for common rendering" << LL_ENDL;
    }
    mBuffer->getVertexStrider(mVerticesp);
    mBuffer->getTexCoord0Strider(mTexcoordsp);
    mBuffer->getColorStrider(mColorsp);
    stop_glerror();
}

void LLRender::resetVertexBuffer()
{
    mBuffer = NULL;
}

void LLRender::shutdown()
{
    resetVertexBuffer();
}

void LLRender::refreshState(void)
{
    mDirty = true;

    U32 active_unit = mCurrTextureUnitIndex;

    for (U32 i = 0; i < mTexUnits.size(); i++)
    {
        mTexUnits[i].refreshState();
    }

    mTexUnits[active_unit].activate();

    setColorMask(mCurrColorMask[0], mCurrColorMask[1], mCurrColorMask[2], mCurrColorMask[3]);

    flush();

    mDirty = false;
}

void LLRender::syncLightState()
{
    LLGLSLShader *shader = LLGLSLShader::sCurBoundShaderPtr;

    if (!shader)
    {
        return;
    }

    if (shader->mLightHash != mLightHash)
    {
        shader->mLightHash = mLightHash;

        LLVector4 position[LL_NUM_LIGHT_UNITS];
        LLVector3 direction[LL_NUM_LIGHT_UNITS];
        LLVector4 attenuation[LL_NUM_LIGHT_UNITS];
        LLVector3 diffuse[LL_NUM_LIGHT_UNITS];
        LLVector3 diffuse_b[LL_NUM_LIGHT_UNITS];
        bool      sun_primary[LL_NUM_LIGHT_UNITS];
        LLVector2 size[LL_NUM_LIGHT_UNITS];

        for (U32 i = 0; i < LL_NUM_LIGHT_UNITS; i++)
        {
            LLLightState *light = &mLightState[i];

            position[i]  = light->mPosition;
            direction[i] = light->mSpotDirection;
            attenuation[i].set(light->mLinearAtten, light->mQuadraticAtten, light->mSpecular.mV[2], light->mSpecular.mV[3]);
            diffuse[i].set(light->mDiffuse.mV);
            diffuse_b[i].set(light->mDiffuseB.mV);
            sun_primary[i] = light->mSunIsPrimary;
            size[i].set(light->mSize, light->mFalloff);
        }

        shader->uniform4fv(LLShaderMgr::LIGHT_POSITION, LL_NUM_LIGHT_UNITS, position[0].mV);
        shader->uniform3fv(LLShaderMgr::LIGHT_DIRECTION, LL_NUM_LIGHT_UNITS, direction[0].mV);
        shader->uniform4fv(LLShaderMgr::LIGHT_ATTENUATION, LL_NUM_LIGHT_UNITS, attenuation[0].mV);
        shader->uniform2fv(LLShaderMgr::LIGHT_DEFERRED_ATTENUATION, LL_NUM_LIGHT_UNITS, size[0].mV);
        shader->uniform3fv(LLShaderMgr::LIGHT_DIFFUSE, LL_NUM_LIGHT_UNITS, diffuse[0].mV);
        shader->uniform3fv(LLShaderMgr::LIGHT_AMBIENT, 1, mAmbientLightColor.mV);
        shader->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_primary[0] ? 1 : 0);

        if (sClassicMode)
        {
            shader->uniform3fv(LLShaderMgr::AMBIENT, 1, mAmbientLightColor.mV);
            shader->uniform3fv(LLShaderMgr::SUNLIGHT_COLOR, 1, diffuse[0].mV);
            shader->uniform3fv(LLShaderMgr::MOONLIGHT_COLOR, 1, diffuse_b[0].mV);
        }
    }
}

void LLRender::syncMatrices()
{
    STOP_GLERROR;
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

    static const U32 name[] =
    {
        LLShaderMgr::MODELVIEW_MATRIX,
        LLShaderMgr::PROJECTION_MATRIX,
        LLShaderMgr::TEXTURE_MATRIX0,
        LLShaderMgr::TEXTURE_MATRIX1,
        LLShaderMgr::TEXTURE_MATRIX2,
        LLShaderMgr::TEXTURE_MATRIX3,
    };

    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;

    static glm::mat4 cached_mvp;
    static glm::mat4 cached_inv_mdv;
    static U32 cached_mvp_mdv_hash = 0xFFFFFFFF;
    static U32 cached_mvp_proj_hash = 0xFFFFFFFF;

    static glm::mat4 cached_normal;
    static U32 cached_normal_hash = 0xFFFFFFFF;

    if (shader)
    {
        bool mvp_done = false;

        U32 i = MM_MODELVIEW;
        if (mMatHash[MM_MODELVIEW] != shader->mMatHash[MM_MODELVIEW])
        { //update modelview, normal, and MVP
            const glm::mat4& mat = mMatrix[MM_MODELVIEW][mMatIdx[MM_MODELVIEW]];

            // if MDV has changed, update the cached inverse as well
            if (cached_mvp_mdv_hash != mMatHash[MM_MODELVIEW])
            {
                cached_inv_mdv = glm::inverse(mat);
            }

            shader->uniformMatrix4fv(name[MM_MODELVIEW], 1, GL_FALSE, glm::value_ptr(mat));
            shader->mMatHash[MM_MODELVIEW] = mMatHash[MM_MODELVIEW];

            //update normal matrix
            S32 loc = shader->getUniformLocation(LLShaderMgr::NORMAL_MATRIX);
            if (loc > -1)
            {
                if (cached_normal_hash != mMatHash[i])
                {
                    cached_normal = glm::transpose(cached_inv_mdv);
                    cached_normal_hash = mMatHash[i];
                }

                auto norm = glm::value_ptr(cached_normal);

                F32 norm_mat[] =
                {
                    norm[0], norm[1], norm[2],
                    norm[4], norm[5], norm[6],
                    norm[8], norm[9], norm[10]
                };

                shader->uniformMatrix3fv(LLShaderMgr::NORMAL_MATRIX, 1, GL_FALSE, norm_mat);
            }

            if (shader->getUniformLocation(LLShaderMgr::INVERSE_MODELVIEW_MATRIX))
            {
                shader->uniformMatrix4fv(LLShaderMgr::INVERSE_MODELVIEW_MATRIX, 1, GL_FALSE, glm::value_ptr(cached_inv_mdv));
            }

            //update MVP matrix
            mvp_done = true;
            loc = shader->getUniformLocation(LLShaderMgr::MODELVIEW_PROJECTION_MATRIX);
            if (loc > -1)
            {
                U32 proj = MM_PROJECTION;

                if (cached_mvp_mdv_hash != mMatHash[i] || cached_mvp_proj_hash != mMatHash[MM_PROJECTION])
                {
                    cached_mvp = mat;
                    cached_mvp = mMatrix[proj][mMatIdx[proj]] * cached_mvp;
                    cached_mvp_mdv_hash = mMatHash[i];
                    cached_mvp_proj_hash = mMatHash[MM_PROJECTION];
                }

                shader->uniformMatrix4fv(LLShaderMgr::MODELVIEW_PROJECTION_MATRIX, 1, GL_FALSE, glm::value_ptr(cached_mvp));
            }
        }

        i = MM_PROJECTION;
        if (mMatHash[MM_PROJECTION] != shader->mMatHash[MM_PROJECTION])
        { //update projection matrix, normal, and MVP
            const glm::mat4& mat = mMatrix[MM_PROJECTION][mMatIdx[MM_PROJECTION]];

            // GZ: This was previously disabled seemingly due to a bug involving the deferred renderer's regular pushing and popping of mats.
            // We're reenabling this and cleaning up the code around that - that would've been the appropriate course initially.
            // Anything beyond the standard proj and inv proj mats are special cases.  Please setup special uniforms accordingly in the future.
            if (shader->getUniformLocation(LLShaderMgr::INVERSE_PROJECTION_MATRIX))
            {
                glm::mat4 inv_proj = glm::inverse(mat);
                shader->uniformMatrix4fv(LLShaderMgr::INVERSE_PROJECTION_MATRIX, 1, false, glm::value_ptr(inv_proj));
            }

            // Used by some full screen effects - such as full screen lights, glow, etc.
            if (shader->getUniformLocation(LLShaderMgr::IDENTITY_MATRIX))
            {
                shader->uniformMatrix4fv(LLShaderMgr::IDENTITY_MATRIX, 1, GL_FALSE, glm::value_ptr(glm::identity<glm::mat4>()));
            }

            shader->uniformMatrix4fv(name[MM_PROJECTION], 1, GL_FALSE, glm::value_ptr(mat));
            shader->mMatHash[MM_PROJECTION] = mMatHash[MM_PROJECTION];

            if (!mvp_done)
            {
                //update MVP matrix
                S32 loc = shader->getUniformLocation(LLShaderMgr::MODELVIEW_PROJECTION_MATRIX);
                if (loc > -1)
                {
                    if (cached_mvp_mdv_hash != mMatHash[MM_MODELVIEW] || cached_mvp_proj_hash != mMatHash[MM_PROJECTION])
                    {
                        U32 mdv = MM_MODELVIEW;
                        cached_mvp = mat;
                        cached_mvp *= mMatrix[mdv][mMatIdx[mdv]];
                        cached_mvp_mdv_hash = mMatHash[MM_MODELVIEW];
                        cached_mvp_proj_hash = mMatHash[MM_PROJECTION];
                    }

                    shader->uniformMatrix4fv(LLShaderMgr::MODELVIEW_PROJECTION_MATRIX, 1, GL_FALSE, glm::value_ptr(cached_mvp));
                }
            }
        }

        for (i = MM_TEXTURE0; i < NUM_MATRIX_MODES; ++i)
        {
            if (mMatHash[i] != shader->mMatHash[i])
            {
                shader->uniformMatrix4fv(name[i], 1, GL_FALSE, glm::value_ptr(mMatrix[i][mMatIdx[i]]));
                shader->mMatHash[i] = mMatHash[i];
            }
        }


        if (shader->mFeatures.hasLighting || shader->mFeatures.calculatesLighting || shader->mFeatures.calculatesAtmospherics)
        { //also sync light state
            syncLightState();
        }
    }
    STOP_GLERROR;
}

void LLRender::translatef(const GLfloat& x, const GLfloat& y, const GLfloat& z)
{
    flush();

    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] = glm::translate(mMatrix[mMatrixMode][mMatIdx[mMatrixMode]], glm::vec3(x, y, z));
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::scalef(const GLfloat& x, const GLfloat& y, const GLfloat& z)
{
    flush();

    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] = glm::scale(mMatrix[mMatrixMode][mMatIdx[mMatrixMode]], glm::vec3(x, y, z));
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::ortho(F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar)
{
    flush();

    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] *= glm::ortho(left, right, bottom, top, zNear, zFar);
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::rotatef(const GLfloat& a, const GLfloat& x, const GLfloat& y, const GLfloat& z)
{
    flush();

    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] = glm::rotate(mMatrix[mMatrixMode][mMatIdx[mMatrixMode]], glm::radians(a), glm::vec3(x,y,z));
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::pushMatrix()
{
    flush();

    {
        if (mMatIdx[mMatrixMode] < LL_MATRIX_STACK_DEPTH-1)
        {
            mMatrix[mMatrixMode][mMatIdx[mMatrixMode]+1] = mMatrix[mMatrixMode][mMatIdx[mMatrixMode]];
            ++mMatIdx[mMatrixMode];
        }
        else
        {
            LL_WARNS() << "Matrix stack overflow." << LL_ENDL;
        }
    }
}

void LLRender::popMatrix()
{
    flush();
    {
        if (mMatIdx[mMatrixMode] > 0)
        {
            --mMatIdx[mMatrixMode];
            mMatHash[mMatrixMode]++;
        }
        else
        {
            LL_WARNS() << "Matrix stack underflow." << LL_ENDL;
        }
    }
}

void LLRender::loadMatrix(const GLfloat* m)
{
    flush();
    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] = glm::make_mat4((GLfloat*) m);
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::multMatrix(const GLfloat* m)
{
    flush();
    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] *= glm::make_mat4(m);
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::matrixMode(eMatrixMode mode)
{
    if (mode == MM_TEXTURE)
    {
        U32 tex_index = gGL.getCurrentTexUnitIndex();
        // the shaders don't actually reference anything beyond texture_matrix0/1 outside of terrain rendering
        llassert(tex_index <= 3);
        mode = eMatrixMode(MM_TEXTURE0 + tex_index);
        if (mode > MM_TEXTURE3)
        {
            // getCurrentTexUnitIndex() can go as high as 32 (LL_NUM_TEXTURE_LAYERS)
            // Large value will result in a crash at mMatrix
            LL_WARNS_ONCE() << "Attempted to assign matrix mode out of bounds: " << mode << LL_ENDL;
            mode = MM_TEXTURE0;
        }
    }

    mMatrixMode = mode;
}

LLRender::eMatrixMode LLRender::getMatrixMode()
{
    if (mMatrixMode >= MM_TEXTURE0 && mMatrixMode <= MM_TEXTURE3)
    { //always return MM_TEXTURE if current matrix mode points at any texture matrix
        return MM_TEXTURE;
    }

    return mMatrixMode;
}


void LLRender::loadIdentity()
{
    flush();

    {
        llassert_always(mMatrixMode < NUM_MATRIX_MODES) ;

        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] = glm::identity<glm::mat4>();
        mMatHash[mMatrixMode]++;
    }
}

const glm::mat4& LLRender::getModelviewMatrix()
{
    return mMatrix[MM_MODELVIEW][mMatIdx[MM_MODELVIEW]];
}

const glm::mat4& LLRender::getProjectionMatrix()
{
    return mMatrix[MM_PROJECTION][mMatIdx[MM_PROJECTION]];
}

void LLRender::translateUI(F32 x, F32 y, F32 z)
{
    if (mUIOffset.empty())
    {
        LL_ERRS() << "Need to push a UI translation frame before offsetting" << LL_ENDL;
    }

    mUIOffset.back().add(LLVector4a(x, y, z));
}

void LLRender::scaleUI(F32 x, F32 y, F32 z)
{
    if (mUIScale.empty())
    {
        LL_ERRS() << "Need to push a UI transformation frame before scaling." << LL_ENDL;
    }

    mUIScale.back().mul(LLVector4a(x, y, z));
}

void LLRender::pushUIMatrix()
{
    if (mUIOffset.empty())
    {
        mUIOffset.emplace_back(0.f);
    }
    else
    {
        mUIOffset.push_back(mUIOffset.back());
    }

    if (mUIScale.empty())
    {
        mUIScale.emplace_back(1.f);
    }
    else
    {
        mUIScale.push_back(mUIScale.back());
    }
}

void LLRender::popUIMatrix()
{
    if (mUIOffset.empty())
    {
        LL_ERRS() << "UI offset stack blown." << LL_ENDL;
    }
    mUIOffset.pop_back();
    mUIScale.pop_back();
}

LLVector3 LLRender::getUITranslation()
{
    if (mUIOffset.empty())
    {
        return LLVector3::zero;
    }

    return LLVector3(mUIOffset.back().getF32ptr());
}

LLVector3 LLRender::getUIScale()
{
    if (mUIScale.empty())
    {
        return LLVector3::all_one;
    }

    return LLVector3(mUIScale.back().getF32ptr());
}


void LLRender::loadUIIdentity()
{
    if (mUIOffset.empty())
    {
        LL_ERRS() << "Need to push UI translation frame before clearing offset." << LL_ENDL;
    }

    mUIOffset.back().clear();
    mUIScale.back().splat(1);
}

void LLRender::setColorMask(bool writeColor, bool writeAlpha)
{
    setColorMask(writeColor, writeColor, writeColor, writeAlpha);
}

void LLRender::setColorMask(bool writeColorR, bool writeColorG, bool writeColorB, bool writeAlpha)
{
    flush();

    if (mCurrColorMask[0] != writeColorR ||
        mCurrColorMask[1] != writeColorG ||
        mCurrColorMask[2] != writeColorB ||
        mCurrColorMask[3] != writeAlpha)
    {
        mCurrColorMask[0] = writeColorR;
        mCurrColorMask[1] = writeColorG;
        mCurrColorMask[2] = writeColorB;
        mCurrColorMask[3] = writeAlpha;

        gl_color_mask(writeColorR, writeColorG, writeColorB, writeAlpha); // [Viewer Lab]
    }
}

void LLRender::setSceneBlendType(eBlendType type)
{
    switch (type)
    {
        case BT_ALPHA:
            blendFunc(BF_SOURCE_ALPHA, BF_ONE_MINUS_SOURCE_ALPHA);
            break;
        case BT_ADD:
            blendFunc(BF_ONE, BF_ONE);
            break;
        case BT_ADD_WITH_ALPHA:
            blendFunc(BF_SOURCE_ALPHA, BF_ONE);
            break;
        case BT_MULT:
            blendFunc(BF_DEST_COLOR, BF_ZERO);
            break;
        case BT_MULT_ALPHA:
            blendFunc(BF_DEST_ALPHA, BF_ZERO);
            break;
        case BT_MULT_X2:
            blendFunc(BF_DEST_COLOR, BF_SOURCE_COLOR);
            break;
        case BT_REPLACE:
            blendFunc(BF_ONE, BF_ZERO);
            break;
        default:
            LL_ERRS() << "Unknown Scene Blend Type: " << type << LL_ENDL;
            break;
    }
}

void LLRender::blendFunc(eBlendFactor sfactor, eBlendFactor dfactor)
{
    llassert(sfactor < BF_UNDEF);
    llassert(dfactor < BF_UNDEF);
    if (mCurrBlendColorSFactor != sfactor || mCurrBlendColorDFactor != dfactor ||
        mCurrBlendAlphaSFactor != sfactor || mCurrBlendAlphaDFactor != dfactor)
    {
        flush(); // [Viewer Lab] before the cache changes, so a batch it draws is described with GL's factors
        mCurrBlendColorSFactor = sfactor;
        mCurrBlendAlphaSFactor = sfactor;
        mCurrBlendColorDFactor = dfactor;
        mCurrBlendAlphaDFactor = dfactor;
        gl_blend_func(sGLBlendFactor[sfactor], sGLBlendFactor[dfactor]); // [Viewer Lab]
    }
}

void LLRender::blendFunc(eBlendFactor color_sfactor, eBlendFactor color_dfactor,
             eBlendFactor alpha_sfactor, eBlendFactor alpha_dfactor)
{
    llassert(color_sfactor < BF_UNDEF);
    llassert(color_dfactor < BF_UNDEF);
    llassert(alpha_sfactor < BF_UNDEF);
    llassert(alpha_dfactor < BF_UNDEF);

    if (mCurrBlendColorSFactor != color_sfactor || mCurrBlendColorDFactor != color_dfactor ||
        mCurrBlendAlphaSFactor != alpha_sfactor || mCurrBlendAlphaDFactor != alpha_dfactor)
    {
        flush(); // [Viewer Lab] before the cache changes, so a batch it draws is described with GL's factors
        mCurrBlendColorSFactor = color_sfactor;
        mCurrBlendAlphaSFactor = alpha_sfactor;
        mCurrBlendColorDFactor = color_dfactor;
        mCurrBlendAlphaDFactor = alpha_dfactor;

        gl_blend_func_separate(sGLBlendFactor[color_sfactor], sGLBlendFactor[color_dfactor], sGLBlendFactor[alpha_sfactor],
                               sGLBlendFactor[alpha_dfactor]); // [Viewer Lab]
    }
}

LLTexUnit* LLRender::getTexUnit(U32 index)
{
    if (index < mTexUnits.size())
    {
        return &mTexUnits[index];
    }
    else
    {
        LL_DEBUGS() << "Non-existing texture unit layer requested: " << index << LL_ENDL;
        return &mDummyTexUnit;
    }
}

LLLightState* LLRender::getLight(U32 index)
{
    if (index < mLightState.size())
    {
        return &mLightState[index];
    }

    return NULL;
}

void LLRender::setAmbientLightColor(const LLColor4& color)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    if (color != mAmbientLightColor)
    {
        ++mLightHash;
        mAmbientLightColor = color;
    }
}

// <FS> Line width OGL core profile fix by Rye Mutt
void LLRender::setLineWidth(F32 line_width)
{
    if (line_width > 1.f)
    {
        // [Viewer Lab] GL_LINE_SMOOTH as LLGLState has it rather than glIsEnabled; nothing enables it
        line_width = llmin(line_width, EnableMap::on(GL_LINE_SMOOTH) ? mMaxLineWidthSmooth : mMaxLineWidthAliased);
    }
    if (mLineWidth != line_width || mDirty)
    {
        if (mMode == LLRender::LINES || mMode == LLRender::LINE_STRIP)
        {
            flush();
        }
        mLineWidth = line_width;
        gl_line_width(line_width); // [Viewer Lab]
        if (LL::RHI::Encoder* encoder = encoder_in_pass()) // [Viewer Lab] the open pass gets it at once
        {
            OwnGLState own;
            ++sDynamicStateCounts.sends;
            encoder->setLineWidth(mLineWidth);
        }
    }
}
// </FS>

bool LLRender::verifyTexUnitActive(U32 unitToVerify)
{
    if (mCurrTextureUnitIndex == unitToVerify)
    {
        return true;
    }
    else
    {
        LL_WARNS() << "TexUnit currently active: " << mCurrTextureUnitIndex << " (expecting " << unitToVerify << ")" << LL_ENDL;
        return false;
    }
}

// [Viewer Lab] LLRender::clearErrors' GL, in a GL backend function of its own:
// nothing when GL is not the backend chosen at startup.
static void drain_gl_errors()
{
    if (LL::RHI::viewerBackend() != LL::RHI::BackendType::GL)
    {
        return;
    }
    while (glGetError())
    {
        //loop until no more error flags left
    }
}

void LLRender::clearErrors()
{
    drain_gl_errors(); // [Viewer Lab]
}

void LLRender::beginList(std::list<LLVertexBufferData> *list)
{
    if (sBufferDataList)
    {
        LL_ERRS() << "beginList called while another list is open." << LL_ENDL;
    }
    llassert(LLGLSLShader::sCurBoundShaderPtr == &gUIProgram);
    flush();
    sBufferDataList = list;
}

void LLRender::endList()
{
    if (sBufferDataList)
    {
        flush();
        sBufferDataList = nullptr;
    }
    else
    {
        llassert(false); // endList called without an open list
    }
}

void LLRender::begin(const GLuint& mode)
{
    if (mode != mMode)
    {
        if (mMode == LLRender::LINES ||
            mMode == LLRender::TRIANGLES ||
            mMode == LLRender::POINTS)
        {
            flush();
        }
        else if (mCount != 0)
        {
            LL_ERRS() << "gGL.begin() called redundantly." << LL_ENDL;
        }

        mMode = mode;
    }
}

void LLRender::end()
{
    if (mCount == 0)
    {
        return;
        //IMM_ERRS << "GL begin and end called with no vertices specified." << LL_ENDL;
    }

    if ((mMode != LLRender::LINES &&
        mMode != LLRender::TRIANGLES &&
        mMode != LLRender::POINTS) ||
        mCount > 2048)
    {
        flush();
    }
}

void LLRender::flush()
{
    STOP_GLERROR;
    if (mCount > 0)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
        llassert_always(LLGLSLShader::sCurBoundShaderPtr != nullptr);

        if (!mUIOffset.empty())
        {
            sUICalls++;
            sUIVerts += mCount;
        }

        //store mCount in a local variable to avoid re-entrance (drawArrays may call flush)
        U32 count = mCount;

        if (mMode == LLRender::TRIANGLES)
        {
            if (mCount%3 != 0)
            {
            count -= (mCount % 3);
            LL_WARNS() << "Incomplete triangle requested." << LL_ENDL;
            }
        }

        if (mMode == LLRender::LINES)
        {
            if (mCount%2 != 0)
            {
                count -= (mCount % 2);
                LL_WARNS() << "Incomplete line requested." << LL_ENDL;
            }
        }

        mCount = 0;

        if (mBuffer)
        {
            // [Viewer Lab] A batch for a shader bind() refused is dropped before a buffer is made of the shader's attribute
            // mask, which is 0 for a program that never linked, so genBuffer's position data had no copy to go to; the
            // batch's draw would be skipped (shader_can_draw, by the same drawsRefused), so it counts as a skipped draw, and
            // a list gets no entry. Every other batch flushes as upstream.
            if (LLGLSLShader::sCurBoundShaderPtr->drawsRefused())
            {
                LLVertexBuffer::noteSkippedDraw();
                resetStriders(count);
                return;
            }

            LLVertexBuffer *vb;

            U32 attribute_mask = LLGLSLShader::sCurBoundShaderPtr->mAttributeMask;

            if (sBufferDataList)
            {
                vb = genBuffer(attribute_mask, count);
                // [Viewer Lab] Unit 0's binding by the device's texture and its id, and its GL name as upstream, which
                // draw binds raw only while the entry holds no texture of the device, a GL-backend path that stays.
                LLTexUnit* unit = gGL.getTexUnit(0);
                LL::RHI::Texture* texture = unit->getCurrRHITexture();
                sBufferDataList->emplace_back(
                    vb,
                    mMode,
                    count,
                    texture,
                    unit->getCurrTextureId(),
                    unit->getCurrTexture(),
                    mMatrix[MM_MODELVIEW][mMatIdx[MM_MODELVIEW]],
                    mMatrix[MM_PROJECTION][mMatIdx[MM_PROJECTION]],
                    mMatrix[MM_TEXTURE0][mMatIdx[MM_TEXTURE0]]
                    );
            }
            else
            {
                vb = bufferfromCache(attribute_mask, count);
            }

            drawBuffer(vb, mMode, count);
        }
        else
        {
            // mBuffer is present in main thread and not present in an image thread
            LL_ERRS() << "A flush call from outside main rendering thread" << LL_ENDL;
        }

        resetStriders(count);
    }
}

LLVertexBuffer* LLRender::bufferfromCache(U32 attribute_mask, U32 count)
{
    LLVertexBuffer *vb = nullptr;
    HBXXH64 hash;

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb cache hash");

        hash.update((U8*)mVerticesp.get(), count * sizeof(LLVector4a));
        if (attribute_mask & LLVertexBuffer::MAP_TEXCOORD0)
        {
            hash.update((U8*)mTexcoordsp.get(), count * sizeof(LLVector2));
        }

        if (attribute_mask & LLVertexBuffer::MAP_COLOR)
        {
            hash.update((U8*)mColorsp.get(), count * sizeof(LLColor4U));
        }

        hash.finalize();
    }

    U64 vhash = hash.digest();

    // check the VB cache before making a new vertex buffer
    // This is a giant hack to deal with (mostly) our terrible UI rendering code
    // that was built on top of OpenGL immediate mode.  Huge performance wins
    // can be had by not uploading geometry to VRAM unless absolutely necessary.
    // Most of our usage of the "immediate mode" style draw calls is actually
    // sending the same geometry over and over again.
    // To leverage this, we maintain a running hash of the vertex stream being
    // built up before a flush, and then check that hash against a VB
    // cache just before creating a vertex buffer in VRAM
    std::unordered_map<U64, LLVBCache>::iterator cache = sVBCache.find(vhash);

    if (cache != sVBCache.end())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb cache hit");
        // cache hit, just use the cached buffer
        vb = cache->second.vb;
        cache->second.touched = std::chrono::steady_clock::now();
    }
    else
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb cache miss");
        vb = genBuffer(attribute_mask, count);

        sVBCache[vhash] = { vb , std::chrono::steady_clock::now() };

        static U32 miss_count = 0;
        miss_count++;
        if (miss_count > 1024)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb cache clean");
            miss_count = 0;
            auto now = std::chrono::steady_clock::now();

            using namespace std::chrono_literals;
            // every 1024 misses, clean the cache of any VBs that haven't been touched in the last second
            for (std::unordered_map<U64, LLVBCache>::iterator iter = sVBCache.begin(); iter != sVBCache.end(); )
            {
                if (now - iter->second.touched > 1s)
                {
                    iter = sVBCache.erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
        }
    }
    return vb;
}

LLVertexBuffer* LLRender::genBuffer(U32 attribute_mask, S32 count)
{
    LLVertexBuffer * vb = new LLVertexBuffer(attribute_mask);
    vb->allocateBuffer(count, 0);

    // Non-Apple path uses glBufferSubData inside setXxxData, so the VBO
    // must already be bound. On Apple, the VBO is lazily created in
    // _unmapBuffer (LLAppleVBOPool); calling setBuffer() here would bind
    // mGLBuffer == 0 and then setupVertexBuffer would issue
    // glVertexAttribIPointer with a non-null offset against no bound
    // GL_ARRAY_BUFFER -> GL_INVALID_OPERATION in core profile.
    if (LLVertexBuffer::bindsBeforeData()) // [Viewer Lab] as the GL backend's VBO pool needs
    {
        vb->setBuffer();
    }

    vb->setPositionData(mVerticesp.get());

    if (attribute_mask & LLVertexBuffer::MAP_TEXCOORD0)
    {
        vb->setTexCoord0Data(mTexcoordsp.get());
    }

    if (attribute_mask & LLVertexBuffer::MAP_COLOR)
    {
        vb->setColorData(mColorsp.get());
    }

#if LL_DARWIN
    // unmapBuffer creates the GL buffer, uploads, and leaves it bound,
    // drawBuffer's later setBuffer() then runs setupVertexBuffer against
    // a valid VBO.
    vb->unmapBuffer();
#endif
    vb->unbind();

    return vb;
}

void LLRender::drawBuffer(LLVertexBuffer* vb, U32 mode, S32 count)
{
    vb->setBuffer();
    vb->drawArrays(mode, 0, count);
}

void LLRender::resetStriders(S32 count)
{
    mVerticesp[0] = mVerticesp[count];
    mTexcoordsp[0] = mTexcoordsp[count];
    mColorsp[0] = mColorsp[count];

    mCount = 0;
}

void LLRender::vertex3f(const GLfloat& x, const GLfloat& y, const GLfloat& z)
{
    //the range of mVerticesp, mColorsp and mTexcoordsp is [0, 4095]
    if (mCount > 2048)
    { //break when buffer gets reasonably full to keep GL command buffers happy and avoid overflow below
        switch (mMode)
        {
            case LLRender::POINTS: flush(); break;
            case LLRender::TRIANGLES: if (mCount%3==0) flush(); break;
            case LLRender::LINES: if (mCount%2 == 0) flush(); break;
        }
    }

    if (mCount > 4094)
    {
    //  LL_WARNS() << "GL immediate mode overflow.  Some geometry not drawn." << LL_ENDL;
        return;
    }

    if (mUIOffset.empty())
    {
        mVerticesp[mCount].set(x,y,z);
    }
    else
    {
        LLVector4a vert(x, y, z);
        vert.add(mUIOffset.back());
        vert.mul(mUIScale.back());
        mVerticesp[mCount] = vert;
    }

    mCount++;
    mVerticesp[mCount] = mVerticesp[mCount-1];
    mColorsp[mCount] = mColorsp[mCount-1];
    mTexcoordsp[mCount] = mTexcoordsp[mCount-1];
}

void LLRender::vertexBatchPreTransformed(LLVector4a* verts, S32 vert_count)
{
    if (mCount + vert_count > 4094)
    {
        //  LL_WARNS() << "GL immediate mode overflow.  Some geometry not drawn." << LL_ENDL;
        return;
    }

    for (S32 i = 0; i < vert_count; i++)
    {
        mVerticesp[mCount] = verts[i];

        mCount++;
        mTexcoordsp[mCount] = mTexcoordsp[mCount-1];
        mColorsp[mCount] = mColorsp[mCount-1];
    }

    if( mCount > 0 ) // ND: Guard against crashes if mCount is zero, yes it can happen
        mVerticesp[mCount] = mVerticesp[mCount-1];
}

void LLRender::vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, S32 vert_count)
{
    if (mCount + vert_count > 4094)
    {
        //  LL_WARNS() << "GL immediate mode overflow.  Some geometry not drawn." << LL_ENDL;
        return;
    }

    for (S32 i = 0; i < vert_count; i++)
    {
        mVerticesp[mCount] = verts[i];
        mTexcoordsp[mCount] = uvs[i];

        mCount++;
        mColorsp[mCount] = mColorsp[mCount-1];
    }

    if (mCount > 0)
    {
        mVerticesp[mCount] = mVerticesp[mCount - 1];
        mTexcoordsp[mCount] = mTexcoordsp[mCount - 1];
    }
}

void LLRender::vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, LLColor4U* colors, S32 vert_count)
{
    if (mCount + vert_count > 4094)
    {
        //  LL_WARNS() << "GL immediate mode overflow.  Some geometry not drawn." << LL_ENDL;
        return;
    }

    for (S32 i = 0; i < vert_count; i++)
    {
        mVerticesp[mCount] = verts[i];
        mTexcoordsp[mCount] = uvs[i];
        mColorsp[mCount] = colors[i];

        mCount++;
    }

    if (mCount > 0)
    {
        mVerticesp[mCount] = mVerticesp[mCount - 1];
        mTexcoordsp[mCount] = mTexcoordsp[mCount - 1];
        mColorsp[mCount] = mColorsp[mCount - 1];
    }
}

void LLRender::vertex2i(const GLint& x, const GLint& y)
{
    vertex3f((GLfloat) x, (GLfloat) y, 0);
}

void LLRender::vertex2f(const GLfloat& x, const GLfloat& y)
{
    vertex3f(x,y,0);
}

void LLRender::vertex2fv(const GLfloat* v)
{
    vertex3f(v[0], v[1], 0);
}

void LLRender::vertex3fv(const GLfloat* v)
{
    vertex3f(v[0], v[1], v[2]);
}

void LLRender::texCoord2f(const GLfloat& x, const GLfloat& y)
{
    mTexcoordsp[mCount] = LLVector2(x,y);
}

void LLRender::texCoord2i(const GLint& x, const GLint& y)
{
    texCoord2f((GLfloat) x, (GLfloat) y);
}

void LLRender::texCoord2fv(const GLfloat* tc)
{
    texCoord2f(tc[0], tc[1]);
}

void LLRender::color4ub(const GLubyte& r, const GLubyte& g, const GLubyte& b, const GLubyte& a)
{
    if (!LLGLSLShader::sCurBoundShaderPtr || LLGLSLShader::sCurBoundShaderPtr->mAttributeMask & LLVertexBuffer::MAP_COLOR)
    {
        mColorsp[mCount] = LLColor4U(r,g,b,a);
    }
    else
    { //not using shaders or shader reads color from a uniform
        diffuseColor4ub(r,g,b,a);
    }
}
void LLRender::color4ubv(const GLubyte* c)
{
    color4ub(c[0], c[1], c[2], c[3]);
}

void LLRender::color4f(const GLfloat& r, const GLfloat& g, const GLfloat& b, const GLfloat& a)
{
    color4ub((GLubyte) (llclamp(r, 0.f, 1.f)*255),
        (GLubyte) (llclamp(g, 0.f, 1.f)*255),
        (GLubyte) (llclamp(b, 0.f, 1.f)*255),
        (GLubyte) (llclamp(a, 0.f, 1.f)*255));
}

void LLRender::color4fv(const GLfloat* c)
{
    color4f(c[0],c[1],c[2],c[3]);
}

void LLRender::color3f(const GLfloat& r, const GLfloat& g, const GLfloat& b)
{
    color4f(r,g,b,1);
}

void LLRender::color3fv(const GLfloat* c)
{
    color4f(c[0],c[1],c[2],1);
}

void LLRender::diffuseColor3f(F32 r, F32 g, F32 b)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, r,g,b,1.f);
    }
}

void LLRender::diffuseColor3fv(const F32* c)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, c[0], c[1], c[2], 1.f);
    }
}

void LLRender::diffuseColor4f(F32 r, F32 g, F32 b, F32 a)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, r,g,b,a);
    }
}

void LLRender::diffuseColor4fv(const F32* c)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4fv(LLShaderMgr::DIFFUSE_COLOR, 1, c);
    }
}

void LLRender::diffuseColor4ubv(const U8* c)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, c[0]/255.f, c[1]/255.f, c[2]/255.f, c[3]/255.f);
    }
}

void LLRender::diffuseColor4ub(U8 r, U8 g, U8 b, U8 a)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, r/255.f, g/255.f, b/255.f, a/255.f);
    }
}

// [Viewer Lab] The dynamic state facades: each makes its GL call in its GL backend function, with no flush and no
// equality skip, as the raw calls they replace have neither (raw GL elsewhere may have changed GL), then remembers the value.
// While one of the viewer's passes is open, the value goes to its encoder at once, inside OwnGLState, so on GL the encoder
// only records it; outside a pass the next stack pass gets it as it begins (sendDynamicState). Where the interface's tables
// have no value, nothing is sent. setScissor is the exception to 'no equality skip': inside the GL backend's own pass it
// records straight in the pass's tracker and skips a box GL and the record already have.

void LLRender::setViewport(S32 x, S32 y, S32 width, S32 height)
{
    gl_viewport(x, y, width, height);
    mViewport[0] = x;
    mViewport[1] = y;
    mViewport[2] = width;
    mViewport[3] = height;
    mViewportSet = true;
    if (LL::RHI::Encoder* encoder = encoder_in_pass())
    {
        OwnGLState own;
        ++sDynamicStateCounts.sends;
        encoder->setViewport(x, y, width, height);
    }
}

void LLRender::setWindowViewport(S32 x, S32 y, S32 width, S32 height)
{
    gGLViewport[0] = x;
    gGLViewport[1] = y;
    gGLViewport[2] = width;
    gGLViewport[3] = height;
    setViewport(x, y, width, height);
}

void LLRender::setScissor(S32 x, S32 y, S32 width, S32 height)
{
    // [Viewer Lab] In the GL backend's own pass, a box GL and the pass's record already have changes nothing: only this
    // facade and the GL backend write GL's box (the gl* ratchet), and the backend's writes, the pass's begin and end and every
    // scissor call through the encoder forget it (invalidateScissor)
    if (mScissorKnown && x == mScissor[0] && y == mScissor[1] && width == mScissor[2] && height == mScissor[3])
    {
        if (gDebugGL)
        {
            gl_check_scissor(mScissor); // [Viewer Lab] a writer that forgot invalidateScissor shows
        }
        return;
    }
    gl_scissor(x, y, width, height);
    mScissor[0] = x;
    mScissor[1] = y;
    mScissor[2] = width;
    mScissor[3] = height;
    mScissorKnown = false;
    if (mOwnPassTracker && !sOwnGLState)
    {
        // [Viewer Lab] The GL encoder's own pass, registered as it began: its setScissor only tracks inside OwnGLState, so
        // the tracker gets the box straight, with LLGLState's enable and the encoder's WARN (GLEncoder::setScissor and
        // GLEncoder::accept; a change there changes this too)
        if (mScissorTest < 0)
        {
            mScissorTest = EnableMap::on(GL_SCISSOR_TEST) ? 1 : 0;
        }
        ++sDynamicStateCounts.sends;
        const std::string why = mOwnPassTracker->setScissor(mScissorTest > 0, x, y, width, height);
        if (!why.empty())
        {
            LL_WARNS("RHI") << "setScissor: " << why << LL_ENDL;
            return;
        }
        // GL refuses a negative size and keeps its box, so such a box is never known
        mScissorKnown = width >= 0 && height >= 0;
        return;
    }
    if (LL::RHI::Encoder* encoder = encoder_in_pass())
    {
        OwnGLState own;
        ++sDynamicStateCounts.sends;
        encoder->setScissor(EnableMap::on(GL_SCISSOR_TEST), x, y, width, height);
    }
}

// [Viewer Lab] An enum the interface's tables lack still reaches GL as given; the WARN makes golden fail if the viewer ever uses
// one.
static void warn_without_interface_value(const char* facade, U32 value)
{
    LL_WARNS_ONCE("RHI") << facade << ": GL enum " << llformat("0x%04x", value) << " has no interface value" << LL_ENDL;
}

void LLRender::setCullFace(U32 face)
{
    gl_cull_face(face);
    mCullFace = face;
    const std::optional<LL::RHI::CullMode> mode = LL::RHI::cullModeFromGL(face);
    if (!mode)
    {
        warn_without_interface_value("setCullFace", face);
    }
    else if (LL::RHI::Encoder* encoder = encoder_in_pass())
    {
        OwnGLState own;
        ++sDynamicStateCounts.sends;
        encoder->setCullMode(EnableMap::on(GL_CULL_FACE) ? *mode : LL::RHI::CullMode::Off);
    }
}

void LLRender::setPolygonMode(U32 face, U32 mode)
{
    gl_polygon_mode(face, mode);
    if (face != GL_FRONT_AND_BACK)
    {
        // [Viewer Lab] GL's core profile refuses the call, so nothing is kept or sent
        warn_without_interface_value("setPolygonMode", face);
        return;
    }
    mPolygonMode = mode;
    const std::optional<LL::RHI::FillMode> fill = LL::RHI::fillModeFromGL(mode);
    if (!fill)
    {
        warn_without_interface_value("setPolygonMode", mode);
    }
    else if (LL::RHI::Encoder* encoder = encoder_in_pass())
    {
        OwnGLState own;
        ++sDynamicStateCounts.sends;
        encoder->setFillMode(*fill);
    }
}

void LLRender::setPolygonOffset(F32 factor, F32 units)
{
    gl_polygon_offset(factor, units);
    mPolygonOffset[0] = factor;
    mPolygonOffset[1] = units;
    if (LL::RHI::Encoder* encoder = encoder_in_pass())
    {
        OwnGLState own;
        ++sDynamicStateCounts.sends;
        encoder->setDepthBias(factor, units, EnableMap::on(GL_POLYGON_OFFSET_FILL), EnableMap::on(GL_POLYGON_OFFSET_LINE));
    }
}

void LLRender::setPointSize(F32 size)
{
    gl_point_size(size);
    mPointSize = size;
    if (LL::RHI::Encoder* encoder = encoder_in_pass())
    {
        OwnGLState own;
        ++sDynamicStateCounts.sends;
        encoder->setPointSize(size);
    }
}

// [Viewer Lab]
LLRender::OwnGLState::OwnGLState() : mPrevious(sOwnGLState)
{
    sOwnGLState = true;
}

// [Viewer Lab]
LLRender::OwnGLState::~OwnGLState()
{
    sOwnGLState = mPrevious;
}

// [Viewer Lab]
bool LLRender::makesOwnGLState()
{
    return sOwnGLState;
}

// [Viewer Lab]
void LLRender::beginOwnPass(LL::RHI::EncoderTracker& tracker)
{
    llassert(!mOwnPassTracker);
    mOwnPassTracker = &tracker;
    invalidateScissor();
}

// [Viewer Lab]
void LLRender::endOwnPass()
{
    mOwnPassTracker = nullptr;
    invalidateScissor();
}

// [Viewer Lab] Each GL enum the viewer toggles through LLGLState has one path: the five dynamic enables go to the
// encoder with their state's whole tuple; GL_BLEND is pipeline state, which each draw reads; GL_STENCIL_TEST (the
// interface has no stencil, and the two live uses only disable it), GL_MULTISAMPLE (only initClass disables it; the sample
// count is the pipeline's and the attachments') and GL_DITHER (only initClass writes it, as GL's default) stay GL only. Any
// other enum is counted, and golden fails a run with one.
void LLRender::sendEnable(U32 state, bool enabled)
{
    if (state == GL_SCISSOR_TEST)
    {
        // [Viewer Lab] LLGLState's map has a new value, also inside OwnGLState, where nothing reaches the record: setScissor
        // reads it again and records the next box, also the same one
        invalidateScissor();
    }
    if (!on_main_thread())
    {
        return;
    }
    switch (state)
    {
        case GL_SCISSOR_TEST:
        case GL_CULL_FACE:
        case GL_POLYGON_OFFSET_FILL:
        case GL_POLYGON_OFFSET_LINE:
        case GL_DEPTH_CLAMP:
            break;
        case GL_BLEND:
        case GL_STENCIL_TEST:
        case GL_MULTISAMPLE:
        case GL_DITHER:
            return;
        default:
            ++sDynamicStateCounts.unclassifiedEnables;
            return;
    }
    LL::RHI::Encoder* encoder = encoder_in_pass();
    if (!encoder)
    {
        return;
    }
    OwnGLState own;
    switch (state)
    {
        case GL_SCISSOR_TEST:
            ++sDynamicStateCounts.sends;
            encoder->setScissor(enabled, mScissor[0], mScissor[1], mScissor[2], mScissor[3]);
            break;
        case GL_CULL_FACE:
            if (!enabled)
            {
                ++sDynamicStateCounts.sends;
                encoder->setCullMode(LL::RHI::CullMode::Off);
            }
            else if (const std::optional<LL::RHI::CullMode> mode = LL::RHI::cullModeFromGL(mCullFace))
            {
                ++sDynamicStateCounts.sends;
                encoder->setCullMode(*mode);
            }
            break;
        case GL_POLYGON_OFFSET_FILL:
        case GL_POLYGON_OFFSET_LINE:
            ++sDynamicStateCounts.sends;
            encoder->setDepthBias(mPolygonOffset[0], mPolygonOffset[1], EnableMap::on(GL_POLYGON_OFFSET_FILL),
                                  EnableMap::on(GL_POLYGON_OFFSET_LINE));
            break;
        default: // GL_DEPTH_CLAMP, whose LLGLState is depth clamp's facade
            ++sDynamicStateCounts.sends;
            encoder->setDepthClamp(enabled);
            break;
    }
}

// [Viewer Lab] The tracker and every backend begin a pass with the defaults, and every send carries its state's whole tuple, so
// only what differs from them is given; the viewport always once set, since an attachment's size can be stale after a resize.
void LLRender::sendDynamicState(LL::RHI::Encoder& encoder)
{
    ++sDynamicStateCounts.passSets;
    OwnGLState own;
    U64 calls = 0;
    if (mViewportSet)
    {
        encoder.setViewport(mViewport[0], mViewport[1], mViewport[2], mViewport[3]);
        ++calls;
    }
    if (EnableMap::on(GL_SCISSOR_TEST))
    {
        encoder.setScissor(true, mScissor[0], mScissor[1], mScissor[2], mScissor[3]);
        ++calls;
    }
    if (EnableMap::on(GL_CULL_FACE))
    {
        if (const std::optional<LL::RHI::CullMode> mode = LL::RHI::cullModeFromGL(mCullFace))
        {
            encoder.setCullMode(*mode);
            ++calls;
        }
    }
    if (LL::RHI::fillModeFromGL(mPolygonMode) == LL::RHI::FillMode::Lines)
    {
        encoder.setFillMode(LL::RHI::FillMode::Lines);
        ++calls;
    }
    const bool biasFill = EnableMap::on(GL_POLYGON_OFFSET_FILL);
    const bool biasLines = EnableMap::on(GL_POLYGON_OFFSET_LINE);
    if (biasFill || biasLines)
    {
        encoder.setDepthBias(mPolygonOffset[0], mPolygonOffset[1], biasFill, biasLines);
        ++calls;
    }
    if (EnableMap::on(GL_DEPTH_CLAMP))
    {
        encoder.setDepthClamp(true);
        ++calls;
    }
    if (mPointSize != 1.f)
    {
        encoder.setPointSize(mPointSize);
        ++calls;
    }
    if (mLineWidth != 1.f)
    {
        encoder.setLineWidth(mLineWidth);
        ++calls;
    }
    sDynamicStateCounts.passSetCalls += calls;
}

// [Viewer Lab]
const LLRender::DynamicStateCounts& LLRender::dynamicStateCounts()
{
    return sDynamicStateCounts;
}

void LLRender::debugTexUnits(void)
{
    LL_INFOS("TextureUnit") << "Active TexUnit: " << mCurrTextureUnitIndex << LL_ENDL;
    std::string active_enabled = "false";
    for (U32 i = 0; i < mTexUnits.size(); i++)
    {
        if (getTexUnit(i)->mCurrTexType != LLTexUnit::TT_NONE)
        {
            if (i == mCurrTextureUnitIndex) active_enabled = "true";
            LL_INFOS("TextureUnit") << "TexUnit: " << i << " Enabled" << LL_ENDL;
            LL_INFOS("TextureUnit") << "Enabled As: " ;
            switch (getTexUnit(i)->mCurrTexType)
            {
                case LLTexUnit::TT_TEXTURE:
                    LL_CONT << "Texture 2D";
                    break;
                case LLTexUnit::TT_RECT_TEXTURE:
                    LL_CONT << "Texture Rectangle";
                    break;
                case LLTexUnit::TT_CUBE_MAP:
                    LL_CONT << "Cube Map";
                    break;
                default:
                    LL_CONT << "ARGH!!! NONE!";
                    break;
            }
            LL_CONT << ", Texture Bound: " << getTexUnit(i)->mCurrTexture << LL_ENDL;
        }
    }
    LL_INFOS("TextureUnit") << "Active TexUnit Enabled : " << active_enabled << LL_ENDL;
}

glm::mat4 get_current_modelview()
{
    return glm::make_mat4(gGLModelView);
}

glm::mat4 get_current_projection()
{
    return glm::make_mat4(gGLProjection);
}

glm::mat4 get_last_modelview()
{
    return glm::make_mat4(gGLLastModelView);
}

glm::mat4 get_last_projection()
{
    return glm::make_mat4(gGLLastProjection);
}

void copy_matrix(const glm::mat4& src, F32* dst)
{
    auto matp = glm::value_ptr(src);
    for (U32 i = 0; i < 16; i++)
    {
        dst[i] = matp[i];
    }
}

void set_current_modelview(const glm::mat4& mat)
{
    copy_matrix(mat, gGLModelView);
}

void set_current_projection(const glm::mat4& mat)
{
    copy_matrix(mat, gGLProjection);
}

void set_last_modelview(const glm::mat4& mat)
{
    copy_matrix(mat, gGLLastModelView);
}

void set_last_projection(const glm::mat4& mat)
{
    copy_matrix(mat, gGLLastProjection);
}

glm::vec3 mul_mat4_vec3(const glm::mat4& mat, const glm::vec3& vec)
{
#if 1 // SIMD path results in strange crashes. Fall back to scalar for now.
    const float w = vec[0] * mat[0][3] + vec[1] * mat[1][3] + vec[2] * mat[2][3] + mat[3][3];
    return glm::vec3(
       (vec[0] * mat[0][0] + vec[1] * mat[1][0] + vec[2] * mat[2][0] + mat[3][0]) / w,
       (vec[0] * mat[0][1] + vec[1] * mat[1][1] + vec[2] * mat[2][1] + mat[3][1]) / w,
       (vec[0] * mat[0][2] + vec[1] * mat[1][2] + vec[2] * mat[2][2] + mat[3][2]) / w
    );
#else
    LLVector4a x, y, z, s, t, p, q;

    x.splat(vec.x);
    y.splat(vec.y);
    z.splat(vec.z);

    s.splat<3>(mat[0].data);
    t.splat<3>(mat[1].data);
    p.splat<3>(mat[2].data);
    q.splat<3>(mat[3].data);

    s.mul(x);
    t.mul(y);
    p.mul(z);
    q.add(s);
    t.add(p);
    q.add(t);

    x.mul(mat[0].data);
    y.mul(mat[1].data);
    z.mul(mat[2].data);

    x.add(y);
    z.add(mat[3].data);
    LLVector4a res;
    res.load3(glm::value_ptr(vec));
    res.setAdd(x, z);
    res.div(q);
    return glm::make_vec3(res.getF32ptr());
#endif
}
