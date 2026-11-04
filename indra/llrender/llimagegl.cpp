/**
 * @file llimagegl.cpp
 * @brief Generic GL image handler
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


// TODO: create 2 classes for images w/ and w/o discard levels?

#include "linden_common.h"

#include "llimagegl.h"

#include "llerror.h"
#include "llfasttimer.h"
#include "llimage.h"

#include "llmath.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrhi.h"
#include "llrhigl.h" // [Viewer Lab] GLNames
#include "llrendertarget.h"
#include "llvertexbuffer.h" // [Viewer Lab] scaleDown's screen triangle
#include "llwindow.h"
#include "llframetimer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <unordered_set>

extern LL_COMMON_API bool on_main_thread();

#if !LL_IMAGEGL_THREAD_CHECK
#define checkActiveThread()
#endif

//----------------------------------------------------------------------------
const F32 MIN_TEXTURE_LIFETIME = 10.f;
const F32 CONVERSION_SCRATCH_BUFFER_GL_VERSION = 3.29f;

// [Viewer Lab] LLImageGL's GL backend functions below do nothing when GL is not the backend chosen
// at startup, as llvertexbuffer.cpp's gl_is_backend.
static bool gl_is_backend()
{
    return LL::RHI::viewerBackend() == LL::RHI::BackendType::GL;
}

// [Viewer Lab] Whether the backend has swizzle masks, so GL's deprecated alpha and luminance formats
// become R8 or RG8 with a mask instead of converted pixels. Without the
// viewer's device (headless, llappearanceutility, the unit tests' fixture) the GL backend's own rule,
// the check textureSwizzle replaces. Any thread: capabilities() is fixed when the device is made,
// and viewerDevice() is atomic.
static bool texture_swizzle()
{
    if (LL::RHI::Device* device = LL::RHI::viewerDevice())
    {
        return device->capabilities().textureSwizzle;
    }
    return gGLManager.mGLVersion >= CONVERSION_SCRATCH_BUFFER_GL_VERSION;
}

// [Viewer Lab] Whether the backend has pixel buffer objects, for scaleDown's PBO path.
// Without the viewer's device GL's rule: GL has them (the GL backend's capability is true).
static bool pixel_buffer_objects()
{
    if (LL::RHI::Device* device = LL::RHI::viewerDevice())
    {
        return device->capabilities().pixelBufferObjects;
    }
    return true;
}

// [Viewer Lab] Whether the backend uploads on a context shared with the main one, so that LLImageGLThread's texture and
// media loads may run there. Without the viewer's device (headless, llappearanceutility, the unit tests'
// fixture) the GL backend's own rule, the check sharedContextUploads replaces, as texture_swizzle.
static bool shared_context_uploads()
{
    if (LL::RHI::Device* device = LL::RHI::viewerDevice())
    {
        return device->capabilities().sharedContextUploads;
    }
    return gGLManager.mGLVersion > 3.95f;
}

// [Viewer Lab] setManualImage's swizzle masks for GL's deprecated formats on the core profile
static const GLint ALPHA_MASK[] = { GL_ZERO, GL_ZERO, GL_ZERO, GL_RED };
static const GLint LUMINANCE_MASK[] = { GL_RED, GL_RED, GL_RED, GL_ONE };
static const GLint LUMINANCE_ALPHA_MASK[] = { GL_RED, GL_RED, GL_RED, GL_GREEN };

// [Viewer Lab] What setManualImage asks GL for: the formats after its conversion of GL's deprecated
// alpha and luminance formats and its compression, the swizzle mask it sets (null for none), the
// source format whose pixels it converts into RGBA (0 for none), whether it compresses, and whether
// compression was asked for a format that has no compressed format.
struct ManualImageFormat
{
    S32 intformat;
    U32 pixformat;
    const GLint* swizzle = nullptr;
    U32 converted = 0;
    bool compress = false;
    bool uncompressible = false;
};

// [Viewer Lab] setManualImage's format decision, moved without its side effects, so that
// setManualImage and LLImageGL::describeTexture use the same one and a texture's description never
// differs from what GL is asked for.
static ManualImageFormat manual_image_format(S32 intformat, U32 pixformat, U32 pixtype, bool allow_compression)
{
    const GLint* swizzle = nullptr;
    U32 converted = 0;
    if (LLRender::sGLCoreProfile)
    {
        if (texture_swizzle())
        {
            if (pixformat == GL_ALPHA)
            { //GL_ALPHA is deprecated, convert to RGBA
                swizzle = ALPHA_MASK;
                pixformat = GL_RED;
                intformat = GL_R8;
            }

            if (pixformat == GL_LUMINANCE)
            { //GL_LUMINANCE is deprecated, convert to GL_RGBA
                swizzle = LUMINANCE_MASK;
                pixformat = GL_RED;
                intformat = GL_R8;
            }

            if (pixformat == GL_LUMINANCE_ALPHA)
            { //GL_LUMINANCE_ALPHA is deprecated, convert to RGBA
                swizzle = LUMINANCE_ALPHA_MASK;
                pixformat = GL_RG;
                intformat = GL_RG8;
            }
        }
        else
        {
            if (pixformat == GL_ALPHA && pixtype == GL_UNSIGNED_BYTE)
            { //GL_ALPHA is deprecated, convert to RGBA
                converted = GL_ALPHA;
                pixformat = GL_RGBA;
                intformat = GL_RGBA8;
            }

            if (pixformat == GL_LUMINANCE_ALPHA && pixtype == GL_UNSIGNED_BYTE)
            { //GL_LUMINANCE_ALPHA is deprecated, convert to RGBA
                converted = GL_LUMINANCE_ALPHA;
                pixformat = GL_RGBA;
                intformat = GL_RGBA8;
            }

            if (pixformat == GL_LUMINANCE && pixtype == GL_UNSIGNED_BYTE)
            { //GL_LUMINANCE_ALPHA is deprecated, convert to RGB
                converted = GL_LUMINANCE;
                pixformat = GL_RGBA;
                intformat = GL_RGB8;
            }
        }
    }

    const bool compress = LLImageGL::sCompressTextures && allow_compression;
    bool uncompressible = false;
    if (compress)
    {
        switch (intformat)
        {
        case GL_RED:
        case GL_R8:
            intformat = GL_COMPRESSED_RED;
            break;
        case GL_RG:
        case GL_RG8:
            intformat = GL_COMPRESSED_RG;
            break;
        case GL_RGB:
        case GL_RGB8:
            intformat = GL_COMPRESSED_RGB;
            break;
        case GL_SRGB:
        case GL_SRGB8:
            intformat = GL_COMPRESSED_SRGB;
            break;
        case GL_RGBA:
        case GL_RGBA8:
            intformat = GL_COMPRESSED_RGBA;
            break;
        case GL_SRGB_ALPHA:
        case GL_SRGB8_ALPHA8:
            intformat = GL_COMPRESSED_SRGB_ALPHA;
            break;
        case GL_LUMINANCE:
        case GL_LUMINANCE8:
            intformat = GL_COMPRESSED_LUMINANCE;
            break;
        case GL_LUMINANCE_ALPHA:
        case GL_LUMINANCE8_ALPHA8:
            intformat = GL_COMPRESSED_LUMINANCE_ALPHA;
            break;
        case GL_ALPHA:
        case GL_ALPHA8:
            intformat = GL_COMPRESSED_ALPHA;
            break;
        default:
            uncompressible = true; // setManualImage logs it
            break;
        }
    }
    return { intformat, pixformat, swizzle, converted, compress, uncompressible };
}

// [Viewer Lab] isCompressed's formats, for LLImageGL::describeTexture too
static bool is_compressed_format(U32 format)
{
    // *NOTE: Not all compressed formats are included here.
    bool is_compressed = false;
    switch (format)
    {
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
        is_compressed = true;
        break;
    default:
        break;
    }
    return is_compressed;
}

// [Viewer Lab] The mask setManualImage sets, in the render interface's terms, and the identity when
// it sets none: on the core profile without swizzle masks the pixels are converted to RGBA, and on a
// compatibility profile GL keeps its own alpha and luminance formats, whose meaning glFormat carries
// (GL_ALPHA8, GL_LUMINANCE8). validate refuses a mask the backend does not have, so a described mask
// there would make such textures fail their creation, and a mask over GL_ALPHA8 would read
// (0, 0, 0, 0).
static LL::RHI::SwizzleRGBA rhi_swizzle(const ManualImageFormat& format)
{
    using LL::RHI::Swizzle;
    if (format.swizzle == ALPHA_MASK)
    {
        return { Swizzle::Zero, Swizzle::Zero, Swizzle::Zero, Swizzle::R };
    }
    if (format.swizzle == LUMINANCE_MASK)
    {
        return { Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::One };
    }
    if (format.swizzle == LUMINANCE_ALPHA_MASK)
    {
        return { Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::G };
    }
    return {};
}

//which power of 2 is i?
//assumes i is a power of 2 > 0
U32 wpo2(U32 i);


U32 LLImageGL::sFrameCount = 0;


// texture memory accounting (for macOS)
static LLMutex sTexMemMutex;
static std::unordered_map<U64, U64> sTextureAllocs; // [Viewer Lab] by texture_key
static U64 sTextureBytes = 0;

// [Viewer Lab] The GL names of LLImageGL's textures on the viewer's render device with their ids, from
// the name's creation until updateClass deletes it; queued once the name is in the deletion lists, so
// it goes there once
struct RHITextureName
{
    U64 id;
    bool queued;
};
static std::unordered_map<U32, RHITextureName> sRHITextureNames; // under sTexMemMutex

// [Viewer Lab] The accounting's key for a GL name: the id of the render interface's texture that owns
// it, or the raw name with the top bit; the caller holds sTexMemMutex
static U64 texture_key_locked(U32 texName)
{
    auto entry = sRHITextureNames.find(texName);
    if (entry != sRHITextureNames.end())
    {
        return entry->second.id;
    }
    return LLImageGLMemory::RAW_NAME_KEY | texName;
}

// [Viewer Lab] free_tex_image's accounting by key; the caller holds sTexMemMutex
static void free_tex_key_locked(U64 key)
{
    auto iter = sTextureAllocs.find(key);
    // sometimes a texName will be "freed" before allocated (e.g. first call to setManualImage for a given texName)
    if (iter != sTextureAllocs.end())
    {
        llassert(iter->second <= sTextureBytes); // sTextureBytes MUST NOT go below zero

        sTextureBytes -= iter->second;

        sTextureAllocs.erase(iter);
    }
}

// [Viewer Lab] True only while create_rhi_texture's one Device::createTexture call runs on this thread
static thread_local bool sCreatingOwnTexture = false;

// [Viewer Lab] While setImage runs on this thread: the texture of the viewer's render device whose levels it specifies,
// when that texture has no GL name (off GL). Unit 0's binding is not that texture there: createGLTexture's bind of the name
// 0 falls back to the default texture or leaves the unit as it was, and the upload thread has no device binding. So
// setManualImage's free_cur_tex_image and alloc_tex_image take its id as the key, as GL takes the bound name: each
// specified level at GL's size. Null otherwise, and always on GL, whose names are never 0.
static thread_local const LL::RHI::Texture* sSpecifiedTexture = nullptr;

// [Viewer Lab] A texture of the viewer's render device for LLImageGL, or null without the device
// (headless, llappearanceutility, the unit tests' fixture) or off the main thread, where LLImageGL
// keeps raw names: the branch on the device that LLGPUZone makes, never on the backend.
// Null too, with the device's WARN, when it refuses the description. Only createGLTexture
// passes upload_thread, for the texture it makes on the upload thread, which may call the device's
// create functions (llrhi.h's Device).
static LLPointer<LL::RHI::Texture> create_rhi_texture(const LL::RHI::TextureDesc& desc, bool upload_thread = false)
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || (!upload_thread && !on_main_thread()))
    {
        return nullptr;
    }
    // The GL backend takes only a name for exactly this call, also if it throws
    struct OwnTexture
    {
        OwnTexture() { sCreatingOwnTexture = true; }
        ~OwnTexture() { sCreatingOwnTexture = false; }
    } scope;
    return device->createTexture(desc);
}

// [Viewer Lab] The GL name LLImageGL keeps in mTexName for LLTexUnit, getTexName and the uploads, 0 off GL; the
// GL backend's state in llrender
static U32 gl_name(const LLPointer<LL::RHI::Texture>& texture)
{
    return LL::RHI::GLNames::of(texture.get());
}

// [Viewer Lab] Kept, never released, where the GL path deletes nothing, so GL keeps the name as
// before and no render interface object goes in static destruction
static void keep_unreleased(LLPointer<LL::RHI::Texture>& texture)
{
    texture->ref();
    texture = nullptr;
}

// [Viewer Lab] Off GL the device's textures have no GL name (all names 0), so the accounting's key for an image's own
// texture is its id; GL names are never 0, so GL keeps the name's key
static bool has_no_gl_name(const LLPointer<LL::RHI::Texture>& texture, U32 name)
{
    return name == 0 && texture.notNull() && gl_name(texture) == 0;
}

// [Viewer Lab] Off GL a cube face's texture of the viewer's render device, which createOwnTexture or shareTexture gave
// it, has no GL name: the face owns it as on GL it owns the shared name, and uploads to its layer. Never on GL, whose
// names are never 0
static bool owns_cube_texture(const LLPointer<LL::RHI::Texture>& texture, U32 name, LLTexUnit::eTextureType bind_target)
{
    return bind_target == LLTexUnit::TT_CUBE_MAP && has_no_gl_name(texture, name);
}

// [Viewer Lab] The accounting of size bytes under key, as alloc_tex_image books them
static void alloc_tex_key(U64 key, U64 size)
{
    sTexMemMutex.lock();
    llassert(sTextureAllocs.find(key) == sTextureAllocs.end());
    sTextureAllocs[key] = size;
    sTextureBytes += size;
    sTexMemMutex.unlock();
}

// [Viewer Lab] free_tex_image by key
static void free_tex_key(U64 key)
{
    sTexMemMutex.lock();
    free_tex_key_locked(key);
    sTexMemMutex.unlock();
}

// [Viewer Lab] scaleDown's free and allocation by the image's GL name, by its texture's id where it has none
static void free_own_tex_image(U32 name, const LLPointer<LL::RHI::Texture>& texture)
{
    if (has_no_gl_name(texture, name))
    {
        free_tex_key(texture->id());
    }
    else
    {
        LLImageGLMemory::free_tex_image(name);
    }
}

static void alloc_own_tex_image(U32 name, const LLPointer<LL::RHI::Texture>& texture, S32 width, S32 height, S32 intformat)
{
    if (has_no_gl_name(texture, name))
    {
        alloc_tex_key(texture->id(), LLImageGL::dataFormatBytes(intformat, width, height));
    }
    else
    {
        LLImageGLMemory::alloc_tex_image(width, height, intformat, 1);
    }
}

// [Viewer Lab] The entry of the texture with id from goes to the texture with id to, as the GL backend's adoption moves
// it for a GL name (adoptRHITextureName)
static void move_tex_key(U64 from, U64 to)
{
    sTexMemMutex.lock();
    auto alloc = sTextureAllocs.find(from);
    if (alloc != sTextureAllocs.end())
    {
        const U64 bytes = alloc->second;
        sTextureAllocs.erase(alloc);
        sTextureAllocs[to] = bytes;
    }
    sTexMemMutex.unlock();
}

// [Viewer Lab] Whether the texture owns the GL name: the registry has the name with the texture's id,
// not yet queued for deletion. A texture whose name syncTexName has deleted as a raw name owns it no
// more, also when GL hands the same name out again later
static bool owns_name(const LLPointer<LL::RHI::Texture>& texture, U32 name)
{
    if (texture.isNull() || name == 0 || gl_name(texture) != name)
    {
        return false;
    }
    sTexMemMutex.lock();
    auto entry = sRHITextureNames.find(name);
    const bool owned = entry != sRHITextureNames.end() && entry->second.id == texture->id() && !entry->second.queued;
    sTexMemMutex.unlock();
    return owned;
}

// [Viewer Lab]
U64 LLImageGLMemory::texture_key(U32 texName)
{
    sTexMemMutex.lock();
    const U64 key = texture_key_locked(texName);
    sTexMemMutex.unlock();
    return key;
}

// track a texture alloc on the currently bound texture.
// asserts that no currently tracked alloc exists
void LLImageGLMemory::alloc_tex_image(U32 width, U32 height, U32 intformat, U32 count)
{
    U32 texUnit = gGL.getCurrentTexUnitIndex();
    llassert(texUnit == 0); // allocations should always be done on tex unit 0
    U32 texName = gGL.getTexUnit(texUnit)->getCurrTexture();
    // [Viewer Lab] a bound texture without a GL name by its id; setImage's texture without one by its id
    const U64 boundId = sSpecifiedTexture ? sSpecifiedTexture->id()
                                          : texName == 0 ? gGL.getTexUnit(texUnit)->getCurrTextureId() : 0;
    U64 size = LLImageGL::dataFormatBytes(intformat, width, height);
    size *= count;

    llassert(size >= 0);

    sTexMemMutex.lock();
    const U64 key = boundId != 0 ? boundId : texture_key_locked(texName); // [Viewer Lab]

    // it is a precondition that no existing allocation exists for this texture
    llassert(sTextureAllocs.find(key) == sTextureAllocs.end()); // [Viewer Lab]

    sTextureAllocs[key] = size; // [Viewer Lab]
    sTextureBytes += size;

    sTexMemMutex.unlock();
}

// track texture free on given texName
void LLImageGLMemory::free_tex_image(U32 texName)
{
    sTexMemMutex.lock();
    free_tex_key_locked(texture_key_locked(texName)); // [Viewer Lab]
    sTexMemMutex.unlock();
}

// track texture free on given texNames
void LLImageGLMemory::free_tex_images(U32 count, const U32* texNames)
{
    for (U32 i = 0; i < count; ++i)
    {
        free_tex_image(texNames[i]);
    }
}

// track texture free on currently bound texture
void LLImageGLMemory::free_cur_tex_image()
{
    U32 texUnit = gGL.getCurrentTexUnitIndex();
    llassert(texUnit == 0); // frees should always be done on tex unit 0
    // [Viewer Lab] setImage's texture without a GL name by its id
    if (sSpecifiedTexture)
    {
        free_tex_key(sSpecifiedTexture->id());
        return;
    }
    U32 texName = gGL.getTexUnit(texUnit)->getCurrTexture();
    // [Viewer Lab] a bound texture without a GL name by its id
    if (texName == 0)
    {
        if (const U64 boundId = gGL.getTexUnit(texUnit)->getCurrTextureId())
        {
            free_tex_key(boundId);
            return;
        }
    }
    free_tex_image(texName);
}

using namespace LLImageGLMemory;

// static
U64 LLImageGL::getTextureBytesAllocated()
{
    return sTextureBytes;
}

//statics

U32 LLImageGL::sUniqueCount             = 0;
U32 LLImageGL::sBindCount               = 0;
U32 LLImageGL::sScaleDowns              = 0; // [Viewer Lab]
U32 LLImageGL::sScaleDownsRefused       = 0; // [Viewer Lab]
S32 LLImageGL::sCount                   = 0;

bool LLImageGL::sGlobalUseAnisotropic   = false;
F32 LLImageGL::sLastFrameTime           = 0.f;
LLImageGL* LLImageGL::sDefaultGLTexture = NULL ;
bool LLImageGL::sCompressTextures = false;
std::unordered_set<LLImageGL*> LLImageGL::sImageList;


bool LLImageGLThread::sEnabledTextures = false;
bool LLImageGLThread::sEnabledMedia = false;

//****************************************************************************************************
//The below for texture auditing use only
//****************************************************************************************************
//-----------------------
//debug use
S32 LLImageGL::sCurTexSizeBar = -1 ;
S32 LLImageGL::sCurTexPickSize = -1 ;
S32 LLImageGL::sMaxCategories = 1 ;

//optimization for when we don't need to calculate mIsMask
bool LLImageGL::sSkipAnalyzeAlpha;
U32  LLImageGL::sScratchPBO = 0;
U32  LLImageGL::sScratchPBOSize = 0;
U32* LLImageGL::sManualScratch = nullptr;


//------------------------
//****************************************************************************************************
//End for texture auditing use only
//****************************************************************************************************

//**************************************************************************************
//below are functions for debug use
//do not delete them even though they are not currently being used.

// [Viewer Lab] A GL backend function as a whole: GL's debug check of the bound texture's size, which returns at once
// when GL is not the backend
void LLImageGL::checkTexSize(bool forced) const
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:LLImageGL::checkTexSize");
        return;
    }
    if ((forced || gDebugGL) && mTarget == GL_TEXTURE_2D)
    {
        {
            //check viewport
            GLint vp[4] ;
            glGetIntegerv(GL_VIEWPORT, vp) ;
            llcallstacks << "viewport: " << vp[0] << " : " << vp[1] << " : " << vp[2] << " : " << vp[3] << llcallstacksendl ;
        }

        GLint texname;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texname);
        bool error = false;
        if (texname != mTexName)
        {
            LL_INFOS() << "Bound: " << texname << " Should bind: " << mTexName << " Default: " << LLImageGL::sDefaultGLTexture->mTexName << LL_ENDL; // [Viewer Lab]

            error = true;
            if (gDebugSession)
            {
                gFailLog << "Invalid texture bound!" << std::endl;
            }
            else
            {
                LL_ERRS() << "Invalid texture bound!" << LL_ENDL;
            }
        }
        stop_glerror() ;
        LLGLint x = 0, y = 0 ;
        glGetTexLevelParameteriv(mTarget, 0, GL_TEXTURE_WIDTH, (GLint*)&x);
        glGetTexLevelParameteriv(mTarget, 0, GL_TEXTURE_HEIGHT, (GLint*)&y) ;
        stop_glerror() ;
        llcallstacks << "w: " << x << " h: " << y << llcallstacksendl ;

        if(!x || !y)
        {
            return ;
        }
        if(x != (mWidth >> mCurrentDiscardLevel) || y != (mHeight >> mCurrentDiscardLevel))
        {
            error = true;
            if (gDebugSession)
            {
                gFailLog << "wrong texture size and discard level!" <<
                    mWidth << " Height: " << mHeight << " Current Level: " << (S32)mCurrentDiscardLevel << std::endl;
            }
            else
            {
                LL_ERRS() << "wrong texture size and discard level: width: " <<
                    mWidth << " Height: " << mHeight << " Current Level: " << (S32)mCurrentDiscardLevel << LL_ENDL ;
            }
        }

        if (error)
        {
            ll_fail("LLImageGL::checkTexSize failed.");
        }
    }
}
//end of debug functions
//**************************************************************************************

//----------------------------------------------------------------------------
bool is_little_endian()
{
    S32 a = 0x12345678;
    U8 *c = (U8*)(&a);

    return (*c == 0x78) ;
}

// [Viewer Lab] initClass's glGenBuffers of sScratchPBO, which only scaleDown uses, and scaleDown's when it has none
static void gl_gen_scratch_pbo(U32& name)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_gen_scratch_pbo");
        return;
    }
    glGenBuffers(1, &name);
}

// [Viewer Lab] cleanupClass's glDeleteBuffers of sScratchPBO
static void gl_delete_scratch_pbo(U32& name)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_delete_scratch_pbo");
        return;
    }
    glDeleteBuffers(1, &name);
}

// [Viewer Lab] allocateConversionBuffer's sManualScratch, the CPU buffer of setManualImage's conversion of GL's alpha
// and luminance formats on a GL without swizzle masks, allocated as before into the buffer pointer the caller gives
static void gl_allocate_conversion_buffer(U32*& scratch)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_allocate_conversion_buffer");
        return;
    }
    try
    {
        scratch = new U32[MAX_IMAGE_AREA];
    }
    catch (std::bad_alloc&)
    {
        LLError::LLUserWarningMsg::showOutOfMemory();
        LL_ERRS() << "Failed to allocate sManualScratch" << LL_ENDL;
    }
}

//static
void LLImageGL::initClass(LLWindow* window, S32 num_catagories, bool skip_analyze_alpha /* = false */, bool thread_texture_loads /* = false */, bool thread_media_updates /* = false */)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    sSkipAnalyzeAlpha = skip_analyze_alpha;

    if (sScratchPBO == 0)
    {
        gl_gen_scratch_pbo(sScratchPBO); // [Viewer Lab]
    }

    if (thread_texture_loads || thread_media_updates)
    {
        // [Viewer Lab] The thread only on GL, or on a backend whose upload context takes the thread's uploads:
        // Metal's own upload queue. The thread makes its shared GL context only
        // when GL is the backend (LLImageGLThread's constructor)
        if (gl_is_backend() || shared_context_uploads())
        {
            LLImageGLThread::createInstance(window);
            // [Viewer Lab] The device's sharedContextUploads, which is the version check at GL's effective version
            LLImageGLThread::sEnabledTextures = shared_context_uploads() ? thread_texture_loads : false;
            LLImageGLThread::sEnabledMedia    = shared_context_uploads() ? thread_media_updates : false;
        }
        else
        {
            LL_INFOS("RHI") << "RenderGLMultiThreadedTextures/RenderGLMultiThreadedMedia are on, but the render device has no "
                               "shared-context uploads: no texture or media load thread (P3-03a)"
                            << LL_ENDL;
        }
    }
}

void LLImageGL::allocateConversionBuffer()
{
    // [Viewer Lab] The device's textureSwizzle, which is the version check at GL's effective version
    if (!texture_swizzle())
    {
        gl_allocate_conversion_buffer(sManualScratch); // [Viewer Lab]
    }
}

//static
void LLImageGL::cleanupClass()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLImageGLThread::deleteSingleton();
    if (sScratchPBO != 0)
    {
        gl_delete_scratch_pbo(sScratchPBO); // [Viewer Lab]
        sScratchPBO = 0;
        sScratchPBOSize = 0;
    }

    delete[] sManualScratch;
}


//static
S32 LLImageGL::dataFormatBits(S32 dataformat)
{
    switch (dataformat)
    {
    case GL_COMPRESSED_RED:                         return 8;
    case GL_COMPRESSED_RG:                          return 16;
    case GL_COMPRESSED_RGB:                         return 24;
    case GL_COMPRESSED_SRGB:                        return 32;
    case GL_COMPRESSED_RGBA:                        return 32;
    case GL_COMPRESSED_SRGB_ALPHA:                  return 32;
    case GL_COMPRESSED_LUMINANCE:                   return 8;
    case GL_COMPRESSED_LUMINANCE_ALPHA:             return 16;
    case GL_COMPRESSED_ALPHA:                       return 8;
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:          return 4;
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:    return 4;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:          return 8;
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:    return 8;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:          return 8;
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:    return 8;
    case GL_LUMINANCE:                              return 8;
    case GL_LUMINANCE8:                             return 8;
    case GL_ALPHA:                                  return 8;
    case GL_ALPHA8:                                 return 8;
    case GL_RED:                                    return 8;
    case GL_R8:                                     return 8;
    case GL_COLOR_INDEX:                            return 8;
    case GL_LUMINANCE_ALPHA:                        return 16;
    case GL_LUMINANCE8_ALPHA8:                      return 16;
    case GL_RG:                                     return 16;
    case GL_RG8:                                    return 16;
    case GL_RGB:                                    return 24;
    case GL_SRGB:                                   return 24;
    case GL_RGB8:                                   return 24;
    case GL_R11F_G11F_B10F:                         return 32;
    case GL_RGBA:                                   return 32;
    case GL_RGBA8:                                  return 32;
    case GL_RGB10_A2:                               return 32;
    case GL_SRGB_ALPHA:                             return 32;
    case GL_BGRA:                                   return 32;      // Used for QuickTime media textures on the Mac
    case GL_DEPTH_COMPONENT:                        return 24;
    case GL_DEPTH_COMPONENT24:                      return 24;
    case GL_RGBA16:                                 return 64;
    case GL_R16F:                                   return 16;
    case GL_RG16F:                                  return 32;
    case GL_RGB16F:                                 return 48;
    case GL_RGBA16F:                                return 64;
    case GL_R32F:                                   return 32;
    case GL_RG32F:                                  return 64;
    case GL_RGB32F:                                 return 96;
    case GL_RGBA32F:                                return 128;
    default:
        LL_ERRS() << "LLImageGL::Unknown format: " << std::hex << dataformat << std::dec << LL_ENDL;
        return 0;
    }
}

//static
S64 LLImageGL::dataFormatBytes(S32 dataformat, S32 width, S32 height)
{
    switch (dataformat)
    {
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
        if (width < 4) width = 4;
        if (height < 4) height = 4;
        break;
    default:
        break;
    }
    S64 bytes (((S64)width * (S64)height * (S64)dataFormatBits(dataformat)+7)>>3);
    S64 aligned = (bytes+3)&~3;
    return aligned;
}

//static
S32 LLImageGL::dataFormatComponents(S32 dataformat)
{
    switch (dataformat)
    {
      case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:    return 3;
      case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT: return 3;
      case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:    return 4;
      case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT: return 4;
      case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:    return 4;
      case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT: return 4;
      case GL_LUMINANCE:                        return 1;
      case GL_ALPHA:                            return 1;
      case GL_RED:                              return 1;
      case GL_COLOR_INDEX:                      return 1;
      case GL_LUMINANCE_ALPHA:                  return 2;
      case GL_RG:                               return 2;
      case GL_RGB:                              return 3;
      case GL_SRGB:                             return 3;
      case GL_RGBA:                             return 4;
      case GL_SRGB_ALPHA:                       return 4;
      case GL_BGRA:                             return 4;       // Used for QuickTime media textures on the Mac
      default:
        LL_ERRS() << "LLImageGL::Unknown format: " << std::hex << dataformat << std::dec << LL_ENDL;
        return 0;
    }
}

//----------------------------------------------------------------------------

// static
void LLImageGL::updateStats(F32 current_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    sLastFrameTime = current_time;
}

//----------------------------------------------------------------------------

//static
void LLImageGL::destroyGL()
{
    for (S32 stage = 0; stage < gGLManager.mNumTextureImageUnits; stage++)
    {
        gGL.getTexUnit(stage)->unbind(LLTexUnit::TT_TEXTURE);
    }
}

//static
void LLImageGL::dirtyTexOptions()
{
    for (auto& glimage : sImageList)
    {
        glimage->mTexOptionsDirty = true;
        stop_glerror();
    }

}
//----------------------------------------------------------------------------

//for server side use only.
//static
bool LLImageGL::create(LLPointer<LLImageGL>& dest, bool usemipmaps)
{
    dest = new LLImageGL(usemipmaps);
    return true;
}

//for server side use only.
bool LLImageGL::create(LLPointer<LLImageGL>& dest, U32 width, U32 height, U8 components, bool usemipmaps)
{
    dest = new LLImageGL(width, height, components, usemipmaps);
    return true;
}

//for server side use only.
bool LLImageGL::create(LLPointer<LLImageGL>& dest, const LLImageRaw* imageraw, bool usemipmaps)
{
    dest = new LLImageGL(imageraw, usemipmaps);
    return true;
}

//----------------------------------------------------------------------------

LLImageGL::LLImageGL(bool usemipmaps/* = true*/, bool allow_compression/* = true*/)
:   mSaveData(0), mExternalTexture(false)
{
    init(usemipmaps, allow_compression);
    setSize(0, 0, 0);
    sImageList.insert(this);
    sCount++;
}

LLImageGL::LLImageGL(U32 width, U32 height, U8 components, bool usemipmaps/* = true*/, bool allow_compression/* = true*/)
:   mSaveData(0), mExternalTexture(false)
{
    llassert( components <= 4 );
    init(usemipmaps, allow_compression);
    setSize(width, height, components);
    sImageList.insert(this);
    sCount++;
}

LLImageGL::LLImageGL(const LLImageRaw* imageraw, bool usemipmaps/* = true*/, bool allow_compression/* = true*/)
:   mSaveData(0), mExternalTexture(false)
{
    init(usemipmaps, allow_compression);
    setSize(0, 0, 0);
    sImageList.insert(this);
    sCount++;

    createGLTexture(0, imageraw);
}


LLImageGL::~LLImageGL()
{
    if (!mExternalTexture && gGLManager.mInited)
    {
        LLImageGL::cleanup();
        sImageList.erase(this);
        freePickMask();
        sCount--;
    }
    // [Viewer Lab] Still here when GL is stopped or gone (cleanup deletes nothing while GL is
    // disabled, and nothing is deleted without mInited) or the texture is external: its name stays as
    // before
    if (mRHITexture.notNull())
    {
        keep_unreleased(mRHITexture);
    }
    // [Viewer Lab] A texture still parked for a swap that never came keeps its name, as an unswapped name
    if (mNewRHITexture.notNull())
    {
        keep_unreleased(mNewRHITexture);
    }
}

void LLImageGL::init(bool usemipmaps, bool allow_compression)
{
#if LL_IMAGEGL_THREAD_CHECK
    mActiveThread = LLThread::currentID();
#endif

    // keep these members in the same order as declared in llimagehl.h
    // so that it is obvious by visual inspection if we forgot to
    // init a field.

    mTextureMemory = S64Bytes(0);
    mLastBindTime = 0.f;

    mPickMask = NULL;
    mPickMaskWidth = 0;
    mPickMaskHeight = 0;
    mUseMipMaps = usemipmaps;
    mHasExplicitFormat = false;

    mIsMask = false;
    mNeedsAlphaAndPickMask = true ;
    mAlphaStride = 0 ;
    mAlphaOffset = 0 ;

    mGLTextureCreated = false ;
    mTexName = 0;
    mWidth = 0;
    mHeight = 0;
    mCurrentDiscardLevel = -1;

    mAllowCompression = allow_compression;

    mTarget = GL_TEXTURE_2D;
    mBindTarget = LLTexUnit::TT_TEXTURE;
    mHasMipMaps = false;
    mMipLevels = -1;

    mIsResident = 0;

    mComponents = 0;
    mMaxDiscardLevel = MAX_DISCARD_LEVEL;

    mTexOptionsDirty = true;
    mAddressMode = LLTexUnit::TAM_WRAP;
    mFilterOption = LLTexUnit::TFO_ANISOTROPIC;

    mFormatInternal = -1;
    mFormatPrimary = (LLGLenum) 0;
    mFormatType = GL_UNSIGNED_BYTE;
    mFormatSwapBytes = false;

#ifdef DEBUG_MISS
    mMissed = false;
#endif

    mCategory = -1;

    // Sometimes we have to post work for the main thread.
    mMainQueue = LL::WorkQueue::getInstance("mainloop");

    mTextureKey = 0; // [Viewer Lab] getTextureKey's, with mTexName's 0; declared last
}

void LLImageGL::cleanup()
{
    if (!gGLManager.mIsDisabled)
    {
        destroyGLTexture();
    }
    freePickMask();

    mSaveData = NULL; // deletes data
}

//----------------------------------------------------------------------------

//this function is used to check the size of a texture image.
//so dim should be a positive number
static bool check_power_of_two(S32 dim)
{
    if(dim < 0)
    {
        return false ;
    }
    if(!dim)//0 is a power-of-two number
    {
        return true ;
    }
    return !(dim & (dim - 1)) ;
}

//static
bool LLImageGL::checkSize(S32 width, S32 height)
{
    return check_power_of_two(width) && check_power_of_two(height);
}

bool LLImageGL::setSize(S32 width, S32 height, S32 ncomponents, S32 discard_level)
{
    if (width != mWidth || height != mHeight || ncomponents != mComponents)
    {
        // Check if dimensions are a power of two!
        if (!checkSize(width, height))
        {
            LL_WARNS() << llformat("Texture has non power of two dimension: %dx%d",width,height) << LL_ENDL;
            return false;
        }

        mWidth = width;
        mHeight = height;
        mComponents = ncomponents;
        if (ncomponents > 0)
        {
            mMaxDiscardLevel = 0;
            while (width > 1 && height > 1 && mMaxDiscardLevel < MAX_DISCARD_LEVEL)
            {
                mMaxDiscardLevel++;
                width >>= 1;
                height >>= 1;
            }

            if(discard_level > 0)
            {
                mMaxDiscardLevel = llmax(mMaxDiscardLevel, (S8)discard_level);
                // <FS:minerjr> [FIRE-35361] RenderMaxTextureResolution caps texture resolution lower than intended
                // 2K textures could set the mMaxDiscardLevel above MAX_DISCARD_LEVEL, which would
                // cause them to not be down-scaled so they would get stuck at 0 discard all the time.
                mMaxDiscardLevel = llmin(mMaxDiscardLevel, (S8)MAX_DISCARD_LEVEL);
                // </FS:minerjr> [FIRE-35361]
            }
        }
        else
        {
            mMaxDiscardLevel = MAX_DISCARD_LEVEL;
        }
    }

    return true;
}

//----------------------------------------------------------------------------

// virtual
void LLImageGL::dump()
{
    LL_INFOS() << "mMaxDiscardLevel " << S32(mMaxDiscardLevel)
            << " mLastBindTime " << mLastBindTime
            << " mTarget " << S32(mTarget)
            << " mBindTarget " << S32(mBindTarget)
            << " mUseMipMaps " << S32(mUseMipMaps)
            << " mHasMipMaps " << S32(mHasMipMaps)
            << " mCurrentDiscardLevel " << S32(mCurrentDiscardLevel)
            << " mFormatInternal " << S32(mFormatInternal)
            << " mFormatPrimary " << S32(mFormatPrimary)
            << " mFormatType " << S32(mFormatType)
            << " mFormatSwapBytes " << S32(mFormatSwapBytes)
            << " mHasExplicitFormat " << S32(mHasExplicitFormat)
#if DEBUG_MISS
            << " mMissed " << mMissed
#endif
            << LL_ENDL;

    LL_INFOS() << " mTextureMemory " << mTextureMemory
            << " mTexNames " << mTexName
            << " mIsResident " << S32(mIsResident)
            << LL_ENDL;
}

//----------------------------------------------------------------------------
void LLImageGL::forceUpdateBindStats(void) const
{
    mLastBindTime = sLastFrameTime;
}

bool LLImageGL::updateBindStats() const
{
    // [Viewer Lab] hasTexture(): off GL the image's own texture has no GL name; on GL it is mTexName != 0
    if (hasTexture())
    {
#ifdef DEBUG_MISS
        mMissed = ! getIsResident(true);
#endif
        sBindCount++;
        if (mLastBindTime != sLastFrameTime)
        {
            // we haven't accounted for this texture yet this frame
            sUniqueCount++;
            mLastBindTime = sLastFrameTime;

            return true ;
        }
    }
    return false ;
}

F32 LLImageGL::getTimePassedSinceLastBound()
{
    return sLastFrameTime - mLastBindTime ;
}

void LLImageGL::setExplicitFormat( LLGLint internal_format, LLGLenum primary_format, LLGLenum type_format, bool swap_bytes )
{
    // Note: must be called before createTexture()
    // Note: it's up to the caller to ensure that the format matches the number of components.
    mHasExplicitFormat = true;
    mFormatInternal = internal_format;
    mFormatPrimary = primary_format;
    if(type_format == 0)
        mFormatType = GL_UNSIGNED_BYTE;
    else
        mFormatType = type_format;
    mFormatSwapBytes = swap_bytes;

    calcAlphaChannelOffsetAndStride() ;
}

//----------------------------------------------------------------------------

// [Viewer Lab] LLImageGL's uploads and mipmaps through the viewer's render device: the encoder on the main
// thread and the upload context on any other, with the caller's bytes in their own layout. On GL the backend only tracks
// them inside LLImageGL's scope, and LLImageGL makes their GL calls itself in the GL backend functions below.

// [Viewer Lab] True on this thread while LLImageGL sends its own upload or mipmaps to the device; per thread, because
// the upload context runs on LLImageGLThread
static thread_local bool sMakesOwnGLUploads = false;

// [Viewer Lab] What LLImageGL has sent to the device, and the tests' log of it, appended under sTexMemMutex and null in
// the viewer
static std::atomic<U64> sDeviceUploads{ 0 };
static std::atomic<U64> sDeviceMipmaps{ 0 };
static std::atomic<U64> sDeviceUntracked{ 0 };
static std::atomic<U64> sDeviceRespecifications{ 0 };
static std::atomic<U64> sDeviceUploadContext{ 0 };
static std::atomic<std::vector<LLImageGL::DeviceCall>*> sDeviceCallLog{ nullptr };

// [Viewer Lab]
LLImageGL::OwnGLUploads::OwnGLUploads()
:   mPrevious(sMakesOwnGLUploads)
{
    sMakesOwnGLUploads = true;
}

// [Viewer Lab]
LLImageGL::OwnGLUploads::~OwnGLUploads()
{
    sMakesOwnGLUploads = mPrevious;
}

// [Viewer Lab]
// static
bool LLImageGL::makesOwnGLUploads()
{
    return sMakesOwnGLUploads;
}

// [Viewer Lab] True on this thread while LLGPUReadback or LLGPUCopy sends a readback or a copy whose GL call it makes
// itself; the GL backend then only tracks it. A scope of its own, not OwnGLUploads, which also makes uploads and mipmaps
// track-only
static thread_local bool sMakesOwnGLTransfers = false;

// [Viewer Lab]
LLImageGL::OwnGLTransfers::OwnGLTransfers()
:   mPrevious(sMakesOwnGLTransfers)
{
    sMakesOwnGLTransfers = true;
}

// [Viewer Lab]
LLImageGL::OwnGLTransfers::~OwnGLTransfers()
{
    sMakesOwnGLTransfers = mPrevious;
}

// [Viewer Lab]
// static
bool LLImageGL::makesOwnGLTransfers()
{
    return sMakesOwnGLTransfers;
}

// [Viewer Lab]
// static
LLImageGL::DeviceCounts LLImageGL::deviceCounts()
{
    DeviceCounts counts;
    counts.uploads = sDeviceUploads;
    counts.mipmaps = sDeviceMipmaps;
    counts.untracked = sDeviceUntracked;
    counts.respecifications = sDeviceRespecifications;
    counts.uploadContext = sDeviceUploadContext;
    return counts;
}

// [Viewer Lab]
// static
void LLImageGL::setDeviceCallLog(std::vector<DeviceCall>* log)
{
    sTexMemMutex.lock();
    sDeviceCallLog = log;
    sTexMemMutex.unlock();
}

// [Viewer Lab] Counts a call and logs it for the tests. The first call of each kind through each recorder is said once at
// INFO, as the viewer's evidence in its log, which never sees cleanupClass
static void note_device_call(const LLImageGL::DeviceCall& call)
{
    static const char* const FIRST[] = { "LLImageGL: first upload through the encoder",
                                         "LLImageGL: first upload through the upload context",
                                         "LLImageGL: first mipmaps through the encoder",
                                         "LLImageGL: first mipmaps through the upload context",
                                         "LLImageGL: first untracked re-specification",
                                         "LLImageGL: first re-specification" };
    static std::atomic<bool> said[std::size(FIRST)];
    size_t first = std::size(FIRST);
    switch (call.kind)
    {
    case 'u':
        ++sDeviceUploads;
        first = call.uploadContext ? 1 : 0;
        break;
    case 'm':
        ++sDeviceMipmaps;
        first = call.uploadContext ? 3 : 2;
        break;
    case 'x':
        ++sDeviceUntracked;
        first = 4;
        break;
    case 'r':
        ++sDeviceRespecifications;
        first = 5;
        break;
    default:
        break;
    }
    if (call.uploadContext)
    {
        ++sDeviceUploadContext;
    }
    if (first < std::size(FIRST) && !said[first].exchange(true))
    {
        LL_INFOS_ONCE("RHI") << FIRST[first] << LL_ENDL;
    }
    if (sDeviceCallLog.load())
    {
        sTexMemMutex.lock();
        if (std::vector<LLImageGL::DeviceCall>* log = sDeviceCallLog.load())
        {
            log->push_back(call);
        }
        sTexMemMutex.unlock();
    }
}

// [Viewer Lab] One recorder for uploads and mipmaps alike: the main thread's encoder, or the upload context on any other
// thread, chosen on the device and the thread as LLGPUZone and create_rhi_texture choose, never on the backend.
// Nothing without the viewer's device or without a texture. The call runs inside LLImageGL's scope, then it is
// counted; record carries an upload's level, region, layout and bytes
template <typename Call>
static void with_recorder(LL::RHI::Texture* texture, char kind, Call&& call, LLImageGL::DeviceCall record = {})
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || !texture)
    {
        return;
    }
    record.kind = kind;
    record.texture = texture->id();
    record.uploadContext = !on_main_thread();
    {
        LLImageGL::OwnGLUploads own;
        if (record.uploadContext)
        {
            call(device->uploadContext());
        }
        else
        {
            call(device->encoder());
        }
    }
    note_device_call(record);
}

// [Viewer Lab] The transfer operand check, with the GL backend's transfer functions below
static bool gl_check_transfer(LL::RHI::Device* device, const LL::RHI::Texture* source, bool depth,
                              const LL::RHI::Texture* destination);

// [Viewer Lab] Under gDebugGL, whether a tracked transfer's GL operands are the tracked ones
// (gl_check_transfer); a mismatch counts into mismatched, with one WARN line the first time the site mismatches.
static void check_transfer(std::atomic<U64>& mismatched, std::atomic<bool>& warned, const char* site,
                           LL::RHI::Device* device, const LL::RHI::Texture* source, bool depth,
                           const LL::RHI::Texture* destination)
{
    if (!gDebugGL || gl_check_transfer(device, source, depth, destination))
    {
        return;
    }
    ++mismatched;
    if (!warned.exchange(true))
    {
        LL_WARNS("RHI") << site << ": GL reads or writes other operands than the encoder tracked (R45; the site's first)"
                        << LL_ENDL;
    }
}

// [Viewer Lab] The transfer operand check's mismatches of the device uploads
static std::atomic<U64> sUploadMismatches{ 0 };

// [Viewer Lab] One level's upload of the bytes GL reads for it; nothing without data
static void device_upload(LL::RHI::Texture* texture, U32 level, U32 layer, const LL::RHI::Region& region,
                          const LL::RHI::PixelLayout& layout, const void* data, U64 bytes)
{
    if (!data)
    {
        return;
    }
    LLImageGL::DeviceCall record;
    record.level = level;
    record.layer = layer;
    record.x = region.x;
    record.y = region.y;
    record.width = region.width;
    record.height = region.height;
    record.rowLength = layout.rowLength;
    record.swapBytes = layout.swapBytes;
    record.bytes = bytes;
    const std::span<const std::byte> span(static_cast<const std::byte*>(data), size_t(bytes));
    with_recorder(texture, 'u',
                  [&](auto& recorder) { recorder.uploadTexture(texture, level, layer, region, layout, span); }, record);
    // The transfer operand check: the texture GL's upload after this writes, on the main thread's context
    if (gDebugGL && texture && on_main_thread())
    {
        static std::atomic<bool> sWarned{ false };
        check_transfer(sUploadMismatches, sWarned, "upload", LL::RHI::viewerDevice(), nullptr, false, texture);
    }
}

// [Viewer Lab]
static void device_mipmaps(LL::RHI::Texture* texture)
{
    with_recorder(texture, 'm', [texture](auto& recorder) { recorder.generateMipmaps(texture); });
}

// [Viewer Lab] Every level of a 2D texture of the viewer's device unwritten again on the main
// encoder, as in a texture made without data, before createGLTexture's in-place update writes it; a no-op on GL, which
// never gets here (no texture without a GL name). Nothing without the viewer's device
static void device_forget_written(LL::RHI::Texture* texture)
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || !texture)
    {
        return;
    }
    for (U32 level = 0; level < texture->desc().mipLevels; ++level)
    {
        device->encoder().forgetWritten(texture, level, 0);
    }
}

// [Viewer Lab] The layout of the caller's bytes in the facade's GL format and type, from the one table; none
// for a pair the table does not have, said once per pair, and then nothing is sent
static std::optional<LL::RHI::PixelLayout> upload_layout(U32 pixformat, U32 pixtype, bool swap, U32 row_length)
{
    std::optional<LL::RHI::PixelLayout> layout = LL::RHI::pixelLayoutFromGL(pixformat, pixtype, swap, row_length);
    if (!layout)
    {
        LL_WARNS_ONCE("RHI") << "LLImageGL sends no upload to the render interface for GL format 0x" << std::hex
                             << pixformat << " and type 0x" << pixtype << std::dec << ", which it has no layout for"
                             << LL_ENDL;
    }
    return layout;
}

// [Viewer Lab] The bytes GL reads for a region with this row length at GL_UNPACK_ALIGNMENT 1, as validateUpload counts
// them; dataFormatBytes rounds up to 4 bytes, so it serves DXT only
static U64 upload_bytes(const LL::RHI::PixelLayout& layout, U32 width, U32 height, U32 row_length)
{
    return (U64(row_length ? row_length : width) * (height - 1) + width) * layout.bytesPerPixel();
}

// [Viewer Lab] The layer an upload names: a cube map face's index, 0 for other textures
static U32 upload_layer(U32 target, LLTexUnit::eTextureType bind)
{
    return bind == LLTexUnit::TT_CUBE_MAP ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
}

// [Viewer Lab] The adoption pending on this thread while respecify_rhi_texture makes its new texture: the GL name and
// the id of the texture that owns it, for LLImageGL::adoptRHITextureName
struct RHIAdoption
{
    U32 name = 0;
    U64 id = 0;
};
static thread_local RHIAdoption sRHIAdoption;

// [Viewer Lab] setImage re-specifies an owned 2D name with another description: a new texture of the viewer's device,
// described as the image now is, adopts the same GL name, so desc() follows (a new resource,
// a swapped handle). The registry and the accounting move from the old id to the new one with no GL call, and owned lets
// go of the old texture only once the new one owns the name, so the old one's release queues nothing. Gives the new
// texture; or null with an 'x', keeping owned, when the device refuses the description (its WARN) or gives another
// name.
static LL::RHI::Texture* respecify_rhi_texture(LLPointer<LL::RHI::Texture>& owned, U32 name,
                                               const LL::RHI::TextureDesc& desc)
{
    LLPointer<LL::RHI::Texture> texture;
    {
        // Pending for exactly this creation, also if it throws
        struct Adoption
        {
            Adoption(U32 name, U64 id) { sRHIAdoption = { name, id }; }
            ~Adoption() { sRHIAdoption = {}; }
        } adoption(name, owned->id());
        texture = create_rhi_texture(desc);
    }
    LLImageGL::DeviceCall call;
    if (texture.notNull() && gl_name(texture) == name)
    {
        if (name == 0)
        {
            move_tex_key(owned->id(), texture->id()); // [Viewer Lab] no GL name, so no adoption moves it
        }
        texture->setFacadeState(owned->facadeState()); // [Viewer Lab] LLTexUnit's memory of the sampling GL keeps
        owned = texture;
        call.kind = 'r';
        call.texture = texture->id();
        note_device_call(call);
        return texture.get();
    }
    call.kind = 'x';
    call.texture = owned->id();
    note_device_call(call);
    return nullptr;
}

// [Viewer Lab]
// static
LL::RHI::Texture* LLImageGL::respecifyEmptyTexture(LLPointer<LL::RHI::Texture>& texture, U32 name, S32 width, S32 height,
                                                   U32 mip_levels)
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (texture.isNull() || !device)
    {
        return texture.get();
    }
    // make_attachment's rule: no pixels, no texture of the device
    const U32 max_size = device->capabilities().maxTextureSize;
    if (width <= 0 || height <= 0 || U32(width) > max_size || U32(height) > max_size)
    {
        return texture.get();
    }
    LL::RHI::TextureDesc desc = texture->desc();
    desc.width = U32(width);
    desc.height = U32(height);
    if (mip_levels > 0) // [Viewer Lab]
    {
        desc.mipLevels = mip_levels;
    }
    LL::RHI::Texture* respecified = respecify_rhi_texture(texture, name, desc);
    if (respecified && name == 0)
    {
        // Off GL no setManualImage follows: the moved entry takes GL's size of the new level
        free_tex_key(respecified->id());
        const U32 glformat = desc.glFormat != 0 ? desc.glFormat : LL::RHI::toGLInternalFormat(desc.format);
        alloc_tex_key(respecified->id(), LLImageGL::dataFormatBytes(S32(glformat), width, height));
    }
    return respecified;
}

// [Viewer Lab] setImage's glCompressedTexImage2D of a DXT level, with the DXT format and dataFormatBytes' size as
// before
static void gl_compressed_tex_image_2d(U32 target, S32 level, U32 format, S32 width, S32 height, S32 size, const void* data)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_compressed_tex_image_2d");
        return;
    }
    glCompressedTexImage2D(target, level, format, width, height, 0, size, data);
}

// [Viewer Lab] glPixelStorei with the facade's GL_UNPACK_SWAP_BYTES and GL_UNPACK_ROW_LENGTH and the current values
static void gl_pixel_store_unpack(U32 pname, S32 value)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_pixel_store_unpack");
        return;
    }
    glPixelStorei(pname, value);
}

// [Viewer Lab] glGenerateMipmap on the target the caller has bound
static void gl_generate_mipmap(U32 target)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_generate_mipmap");
        return;
    }
    glGenerateMipmap(target);
}

// [Viewer Lab] setSubImage's glTexSubImage2D of the region in one call, with the row length the caller has set, after
// Encoder::uploadTexture has tracked the region
static void gl_tex_sub_image_2d(U32 target, S32 level, S32 x, S32 y, S32 w, S32 h, U32 pixformat, U32 pixtype,
                                const void* data)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_tex_sub_image_2d");
        return;
    }
    glTexSubImage2D(target, level, x, y, w, h, pixformat, pixtype, data);
}

// [Viewer Lab] The GL backend functions of LLImageGL's readbacks and copies (readBackRaw, setSubImageFromFrameBuffer,
// scaleDown): each makes the one GL call the function made where it calls it, with the GL enums from the call site,
// and nothing when GL is not the backend.

// [Viewer Lab] glGetTexLevelParameteriv of the texture the caller has bound, into a value that starts at 0 as before;
// the fallback when GL is not the backend
static S32 gl_tex_level_parameter(U32 target, S32 level, U32 pname, S32 fallback)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_tex_level_parameter");
        return fallback;
    }
    GLint value = 0;
    glGetTexLevelParameteriv(target, level, pname, &value);
    return value;
}

// [Viewer Lab] glGetError once; GL_NO_ERROR when GL is not the backend
static U32 gl_get_error()
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_get_error");
        return GL_NO_ERROR;
    }
    return glGetError();
}

// [Viewer Lab] glGetTexImage of the texture the caller has bound, with its GL format and type, into the caller's memory
// or the bound pixel-pack buffer; true when it made the call, false when GL is not the backend, so LLGPUReadback tells
// whether it delivered without asking which backend runs
static bool gl_get_tex_image(U32 target, S32 level, U32 format, U32 type, void* data)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_get_tex_image");
        return false;
    }
    glGetTexImage(target, level, format, type, data);
    return true;
}

// [Viewer Lab] glGetCompressedTexImage of the texture the caller has bound
static void gl_get_compressed_tex_image(U32 target, S32 level, void* data)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_get_compressed_tex_image");
        return;
    }
    glGetCompressedTexImage(target, level, data);
}

// [Viewer Lab] glCopyTexSubImage2D from the framebuffer GL reads into the texture the caller has bound; true when it made
// the call, false when GL is not the backend, so LLGPUCopy tells whether it delivered without asking which backend runs
static bool gl_copy_tex_sub_image_2d(U32 target, S32 level, S32 xoffset, S32 yoffset, S32 x, S32 y, S32 width,
                                     S32 height)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_copy_tex_sub_image_2d");
        return false;
    }
    glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
    return true;
}

// [Viewer Lab] scaleDown's glDrawArrays of the screen triangle its caller set, in the target its caller bound, when the
// caller gives no vertex buffer; true when it made the call, false when GL is not the backend, so scaleDown tells
// whether its draw was delivered without asking which backend runs
static bool gl_draw_arrays(U32 mode, S32 first, S32 count)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_draw_arrays");
        return false;
    }
    glDrawArrays(mode, first, count);
    return true;
}

#if LL_DARWIN
// [Viewer Lab] scaleDown's glFlush before the texture is re-specified, only on Darwin like its one call
static void gl_flush()
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_flush");
        return;
    }
    glFlush();
}
#endif

// [Viewer Lab] scaleDown's glBindBuffer of the pixel-pack and pixel-unpack buffers in its PBO path
static void gl_bind_buffer(U32 target, U32 name)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_bind_buffer");
        return;
    }
    glBindBuffer(target, name);
}

// [Viewer Lab] scaleDown's glBufferData without data when sScratchPBO grows
static void gl_buffer_data(U32 target, U64 size, U32 usage)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_buffer_data");
        return;
    }
    glBufferData(target, size, NULL, usage);
}

// [Viewer Lab] LLGPUReadback's glReadPixels with the caller's region, GL format and type, into the caller's memory, from
// the framebuffer GL reads; true when it made the call, false when GL is not the backend, so the facade tells whether it
// delivered without asking which backend runs
static bool gl_read_pixels(S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* data)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_read_pixels");
        return false;
    }
    glReadPixels(x, y, width, height, format, type, data);
    return true;
}

// [Viewer Lab] Whether GL reads and writes the operands a facade's encoder tracked,
// asked under gDebugGL after a tracked call. source (a read's or a copy's) against the framebuffer GL reads: framebuffer
// 0 is the window, whose GL_BACK is no attachment to ask, so it matches only the surface's back buffer or its depth;
// another framebuffer's read attachment (GL_READ_BUFFER's colour attachment, or the depth) must be a texture with the
// source's GL name at level 0. destination (glGetTexImage's texture, a copy's or an upload's) against the texture bound
// on the active unit for its type. The region was checked inside the tracked textures by record(). True when they
// match, and when GL is not the backend, where the tracker's operands are the ones the encoder uses.
static bool gl_check_transfer(LL::RHI::Device* device, const LL::RHI::Texture* source, bool depth,
                              const LL::RHI::Texture* destination)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_check_transfer");
        return true;
    }
    if (source)
    {
        GLint framebuffer = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
        if (framebuffer == 0)
        {
            LL::RHI::Surface* surface = device ? device->surface() : nullptr;
            if (!surface || source != (depth ? surface->backBufferDepth() : surface->backBuffer()))
            {
                return false;
            }
        }
        else
        {
            GLenum attachment = GL_DEPTH_ATTACHMENT;
            if (!depth)
            {
                GLint read_buffer = GL_NONE;
                glGetIntegerv(GL_READ_BUFFER, &read_buffer);
                GLint attachments = 0;
                glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &attachments);
                if (read_buffer < GLint(GL_COLOR_ATTACHMENT0) || read_buffer >= GLint(GL_COLOR_ATTACHMENT0) + attachments)
                {
                    return false;
                }
                attachment = GLenum(read_buffer);
            }
            GLint type = GL_NONE;
            glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                                  &type);
            if (type != GL_TEXTURE)
            {
                return false;
            }
            GLint name = 0;
            GLint level = -1;
            glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                                  &name);
            glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                                                  GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
            if (U32(name) != LL::RHI::GLNames::of(source) || level != 0)
            {
                return false;
            }
        }
    }
    if (destination)
    {
        const LL::RHI::TextureDesc& desc = destination->desc();
        GLenum binding = desc.samples > 1 ? GL_TEXTURE_BINDING_2D_MULTISAMPLE : GL_TEXTURE_BINDING_2D;
        if (desc.type == LL::RHI::TextureType::Cube)
        {
            binding = GL_TEXTURE_BINDING_CUBE_MAP;
        }
        else if (desc.type == LL::RHI::TextureType::CubeArray)
        {
            binding = GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
        }
        GLint bound = 0;
        glGetIntegerv(binding, &bound);
        if (bound == 0 || U32(bound) != LL::RHI::GLNames::of(destination))
        {
            return false;
        }
    }
    return true;
}

// [Viewer Lab] LLGPUCopy's glCopyTexSubImage3D from the framebuffer GL reads into a layer of the cube map array the caller
// has bound; true when it made the call, false when GL is not the backend, so the facade tells whether it delivered
// without asking which backend runs
static bool gl_copy_tex_sub_image_3d(U32 target, S32 level, S32 xoffset, S32 yoffset, S32 zoffset, S32 x, S32 y,
                                     S32 width, S32 height)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_copy_tex_sub_image_3d");
        return false;
    }
    glCopyTexSubImage3D(target, level, xoffset, yoffset, zoffset, x, y, width, height);
    return true;
}

// [Viewer Lab] A GL backend function as a whole: renderMorphMasks' driver workaround, for Intel's GL, which cannot
// glReadPixels the alpha of an RGBA framebuffer, so the target is read through its texture; false when GL is not the
// backend, the vendor choice as should_stagger_image_set
static bool gl_reads_targets_through_textures()
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_reads_targets_through_textures");
        return false;
    }
    return gGLManager.mIsIntel;
}

// [Viewer Lab]
// static
void LLImageGL::generateMipmaps(LL::RHI::Texture* texture, U32 target)
{
    device_mipmaps(texture);
    gl_generate_mipmap(target);
}

// [Viewer Lab] createGLTexture's description of the image's texture, from the image as it is now
bool LLImageGL::describeCurrentTexture(LL::RHI::TextureDesc& desc, U8 usage) const
{
    return describeTexture(desc, LLTexUnit::TT_TEXTURE, usage, mFormatInternal, mFormatPrimary, mFormatType,
                           mAllowCompression, getWidth(), getHeight(),
                           mUseMipMaps ? U32(mMaxDiscardLevel - mCurrentDiscardLevel + 1) : 1u);
}

// [Viewer Lab] scaleDown's re-specification right before its glTexImage2D: the image's own 2D texture on the main thread
// is described at desired_discard's size with createGLTexture's mip rule, and when that differs from its description a new
// texture of the viewer's device adopts the same GL name (respecify_rhi_texture, no GL call), so desc() follows. Gives the
// texture the rest of the call uses; null for a raw name, off the main thread, or for a description that cannot be made
// (counted 'x') or that the device refuses (respecify_rhi_texture's 'x')
LL::RHI::Texture* LLImageGL::respecifyForScale(S32 desired_discard)
{
    // [Viewer Lab] off GL the texture has no GL name to own
    LL::RHI::Texture* old = has_no_gl_name(mRHITexture, mTexName) ? mRHITexture.get() : getRHITexture();
    if (!old || !on_main_thread() || mTarget != GL_TEXTURE_2D || mBindTarget != LLTexUnit::TT_TEXTURE)
    {
        return nullptr;
    }
    LL::RHI::TextureDesc wanted;
    if (!describeTexture(wanted, LLTexUnit::TT_TEXTURE, old->desc().usage, mFormatInternal, mFormatPrimary, mFormatType,
                         mAllowCompression, getWidth(desired_discard), getHeight(desired_discard),
                         mUseMipMaps ? U32(mMaxDiscardLevel - desired_discard + 1) : 1u))
    {
        LLImageGL::DeviceCall untracked;
        untracked.kind = 'x';
        untracked.texture = old->id();
        note_device_call(untracked);
        return nullptr;
    }
    if (wanted == old->desc())
    {
        return old;
    }
    LL::RHI::Texture* respecified = respecify_rhi_texture(mRHITexture, mTexName, wanted);
    updateTextureKey(mTexName, nullptr); // [Viewer Lab] kept with a GL name; off GL the new texture's id
    return respecified;
}

void LLImageGL::setImage(const LLImageRaw* imageraw)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert((imageraw->getWidth() == getWidth(mCurrentDiscardLevel)) &&
             (imageraw->getHeight() == getHeight(mCurrentDiscardLevel)) &&
             (imageraw->getComponents() == getComponents()));
    const U8* rawdata = imageraw->getData();
    setImage(rawdata, false);
}

bool LLImageGL::setImage(const U8* data_in, bool data_hasmips /* = false */, S32 usename /* = 0 */)
{
    return setImage(data_in, data_hasmips, usename, nullptr); // [Viewer Lab]
}

// [Viewer Lab] setImage's body
bool LLImageGL::setImage(const U8* data_in, bool data_hasmips, S32 usename, LL::RHI::Texture* texture)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    const bool is_compressed = isCompressed();

    // [Viewer Lab] The texture of the viewer's render device whose levels the call specifies, decided once before any
    // upload: createGLTexture's new texture, described from the same inputs, or on the main thread the image's own when
    // the call specifies its name. An owned 2D texture whose description no longer matches the image (setSize grew it in
    // place, an explicit format changed, mips taken up after its creation) is re-specified on the same name: the image
    // gets a new texture that adopts the name, before any upload of the call, and the call's uploads go to it, never to
    // the old one it lets go of. A description that cannot be made, or that the device refuses, leaves the
    // call untracked and counted, so the tracker refuses nothing. rhi is assigned once, here, and setImage
    // releases nothing after it, so the pointer stays valid for the whole call. Each level then goes to it right before
    // its GL specification, at the same level and size: the caller's bytes in the facade's format and the type GL is
    // given, rows tightly packed at alignment 1 and never flipped.
    LL::RHI::Texture* rhi = texture;
    if (!rhi && on_main_thread() && (usename == 0 || LLGLuint(usename) == mTexName))
    {
        rhi = getRHITexture();
        if (rhi && mTarget == GL_TEXTURE_2D && mBindTarget == LLTexUnit::TT_TEXTURE)
        {
            LL::RHI::TextureDesc wanted;
            if (!describeCurrentTexture(wanted, rhi->desc().usage))
            {
                LLImageGL::DeviceCall untracked;
                untracked.kind = 'x';
                untracked.texture = rhi->id();
                note_device_call(untracked);
                rhi = nullptr;
            }
            else if (!(wanted == rhi->desc()))
            {
                rhi = respecify_rhi_texture(mRHITexture, mTexName, wanted);
                updateTextureKey(mTexName, nullptr); // [Viewer Lab] kept with a GL name; off GL the new id
            }
        }
    }
    // [Viewer Lab] Off GL, where rhi has no GL name, setManualImage books each level on rhi's id, also if a call throws;
    // never on GL, whose names are never 0
    struct SpecifiedTexture
    {
        explicit SpecifiedTexture(const LL::RHI::Texture* texture) : previous(sSpecifiedTexture)
        {
            sSpecifiedTexture = texture;
        }
        ~SpecifiedTexture() { sSpecifiedTexture = previous; }
        const LL::RHI::Texture* previous;
    } specified(rhi && LL::RHI::GLNames::of(rhi) == 0 ? rhi : nullptr);
    const U32 layer = upload_layer(mTarget, mBindTarget);
    auto upload = [&](S32 level, S32 w, S32 h, U32 pixtype, const void* data)
    {
        if (!rhi)
        {
            return;
        }
        if (const std::optional<LL::RHI::PixelLayout> layout = upload_layout(mFormatPrimary, pixtype, mFormatSwapBytes, 0))
        {
            device_upload(rhi, U32(level), layer, { 0, 0, U32(w), U32(h) }, *layout, data,
                          upload_bytes(*layout, U32(w), U32(h), 0));
        }
    };

    if (mUseMipMaps)
    {
        //set has mip maps to true before binding image so tex parameters get set properly
        gGL.getTexUnit(0)->unbind(mBindTarget);

        mHasMipMaps = true;
        mTexOptionsDirty = true;
        setFilteringOption(LLTexUnit::TFO_ANISOTROPIC);
    }
    else
    {
        mHasMipMaps = false;
    }

    gGL.getTexUnit(0)->bind(this, false, false, usename);

    if (data_in == nullptr)
    {
        S32 w = getWidth();
        S32 h = getHeight();
        LLImageGL::setManualImage(mTarget, 0, mFormatInternal, w, h,
            mFormatPrimary, mFormatType, (GLvoid*)data_in, mAllowCompression);
    }
    else if (mUseMipMaps)
    {
        if (data_hasmips)
        {
            // NOTE: data_in points to largest image; smaller images
            // are stored BEFORE the largest image
            for (S32 d=mCurrentDiscardLevel; d<=mMaxDiscardLevel; d++)
            {

                S32 w = getWidth(d);
                S32 h = getHeight(d);
                S32 gl_level = d-mCurrentDiscardLevel;

                mMipLevels = llmax(mMipLevels, gl_level);

                if (d > mCurrentDiscardLevel)
                {
                    data_in -= dataFormatBytes(mFormatPrimary, w, h); // see above comment
                }
                if (is_compressed)
                {
                    GLsizei tex_size = (GLsizei)dataFormatBytes(mFormatPrimary, w, h);
                    device_upload(rhi, U32(gl_level), layer, { 0, 0, U32(w), U32(h) }, LL::RHI::PixelLayout(), data_in,
                                  U64(tex_size)); // [Viewer Lab]
                    gl_compressed_tex_image_2d(mTarget, gl_level, mFormatPrimary, w, h, tex_size, (GLvoid *)data_in); // [Viewer Lab]
                    stop_glerror();
                }
                else
                {
                    if(mFormatSwapBytes)
                    {
                        gl_pixel_store_unpack(GL_UNPACK_SWAP_BYTES, 1); // [Viewer Lab]
                        stop_glerror();
                    }

                    upload(gl_level, w, h, GL_UNSIGNED_BYTE, data_in); // [Viewer Lab]
                    LLImageGL::setManualImage(mTarget, gl_level, mFormatInternal, w, h, mFormatPrimary, GL_UNSIGNED_BYTE, (GLvoid*)data_in, mAllowCompression);
                    if (gl_level == 0)
                    {
                        analyzeAlpha(data_in, w, h);
                    }
                    updatePickMask(w, h, data_in);

                    if(mFormatSwapBytes)
                    {
                        gl_pixel_store_unpack(GL_UNPACK_SWAP_BYTES, 0); // [Viewer Lab]
                        stop_glerror();
                    }

                    stop_glerror();
                }
                stop_glerror();
            }
        }
        else if (!is_compressed)
        {
            if (mAutoGenMips)
            {
                stop_glerror();
                {
                    if(mFormatSwapBytes)
                    {
                        gl_pixel_store_unpack(GL_UNPACK_SWAP_BYTES, 1); // [Viewer Lab]
                        stop_glerror();
                    }

                    S32 w = getWidth(mCurrentDiscardLevel);
                    S32 h = getHeight(mCurrentDiscardLevel);

                    mMipLevels = wpo2(llmax(w, h));

                    //use legacy mipmap generation mode (note: making this condional can cause rendering issues)
                    // -- but making it not conditional triggers deprecation warnings when core profile is enabled
                    //      (some rendering issues while core profile is enabled are acceptable at this point in time)
                    if (!LLRender::sGLCoreProfile)
                    {
                        glTexParameteri(mTarget, GL_GENERATE_MIPMAP, GL_TRUE);
                    }

                    upload(0, w, h, mFormatType, data_in); // [Viewer Lab]
                    LLImageGL::setManualImage(mTarget, 0, mFormatInternal,
                                 w, h,
                                 mFormatPrimary, mFormatType,
                                 data_in, mAllowCompression);
                    analyzeAlpha(data_in, w, h);
                    stop_glerror();

                    updatePickMask(w, h, data_in);

                    if(mFormatSwapBytes)
                    {
                        gl_pixel_store_unpack(GL_UNPACK_SWAP_BYTES, 0); // [Viewer Lab]
                        stop_glerror();
                    }

                    if (LLRender::sGLCoreProfile)
                    {
                        LL_PROFILE_GPU_ZONE("generate mip map");
                        LLImageGL::generateMipmaps(rhi, mTarget); // [Viewer Lab]
                    }
                    stop_glerror();
                }
            }
            else
            {
                // Create mips by hand
                // ~4x faster than gluBuild2DMipmaps
                S32 width = getWidth(mCurrentDiscardLevel);
                S32 height = getHeight(mCurrentDiscardLevel);
                S32 nummips = mMaxDiscardLevel - mCurrentDiscardLevel + 1;
                S32 w = width, h = height;


                const U8* new_data = 0;
                (void)new_data;

                const U8* prev_mip_data = 0;
                const U8* cur_mip_data = 0;
#ifdef SHOW_ASSERT
                S32 cur_mip_size = 0;
#endif
                mMipLevels = nummips;

                for (int m=0; m<nummips; m++)
                {
                    if (m==0)
                    {
                        cur_mip_data = data_in;
#ifdef SHOW_ASSERT
                        cur_mip_size = width * height * mComponents;
#endif
                    }
                    else
                    {
                        S32 bytes = w * h * mComponents;
#ifdef SHOW_ASSERT
                        llassert(prev_mip_data);
                        llassert(cur_mip_size == bytes*4);
#endif
                        U8* new_data = new(std::nothrow) U8[bytes];
                        if (!new_data)
                        {
                            stop_glerror();

                            if (prev_mip_data)
                            {
                                if (prev_mip_data != cur_mip_data)
                                    delete[] prev_mip_data;
                                prev_mip_data = nullptr;
                            }
                            if (cur_mip_data)
                            {
                                delete[] cur_mip_data;
                                cur_mip_data = nullptr;
                            }

                            mGLTextureCreated = false;
                            return false;
                        }
                        else
                        {

#ifdef SHOW_ASSERT
                            llassert(prev_mip_data);
                            llassert(cur_mip_size == bytes * 4);
#endif

                            LLImageBase::generateMip(prev_mip_data, new_data, w, h, mComponents);
                            cur_mip_data = new_data;
#ifdef SHOW_ASSERT
                            cur_mip_size = bytes;
#endif
                        }

                    }
                    llassert(w > 0 && h > 0 && cur_mip_data);
                    (void)cur_mip_data;
                    {
                        if(mFormatSwapBytes)
                        {
                            gl_pixel_store_unpack(GL_UNPACK_SWAP_BYTES, 1); // [Viewer Lab]
                            stop_glerror();
                        }

                        upload(m, w, h, mFormatType, cur_mip_data); // [Viewer Lab]
                        LLImageGL::setManualImage(mTarget, m, mFormatInternal, w, h, mFormatPrimary, mFormatType, cur_mip_data, mAllowCompression);
                        if (m == 0)
                        {
                            analyzeAlpha(data_in, w, h);
                        }
                        stop_glerror();
                        if (m == 0)
                        {
                            updatePickMask(w, h, cur_mip_data);
                        }

                        if(mFormatSwapBytes)
                        {
                            gl_pixel_store_unpack(GL_UNPACK_SWAP_BYTES, 0); // [Viewer Lab]
                            stop_glerror();
                        }
                    }
                    if (prev_mip_data && prev_mip_data != data_in)
                    {
                        delete[] prev_mip_data;
                    }
                    prev_mip_data = cur_mip_data;
                    w >>= 1;
                    h >>= 1;
                }
                if (prev_mip_data && prev_mip_data != data_in)
                {
                    delete[] prev_mip_data;
                    prev_mip_data = NULL;
                }
            }
        }
        else
        {
            LL_ERRS() << "Compressed Image has mipmaps but data does not (can not auto generate compressed mips)" << LL_ENDL;
        }
    }
    else
    {
        mMipLevels = 0;
        S32 w = getWidth();
        S32 h = getHeight();
        if (is_compressed)
        {
            GLsizei tex_size = (GLsizei)dataFormatBytes(mFormatPrimary, w, h);
            device_upload(rhi, 0, layer, { 0, 0, U32(w), U32(h) }, LL::RHI::PixelLayout(), data_in, U64(tex_size)); // [Viewer Lab]
            gl_compressed_tex_image_2d(mTarget, 0, mFormatPrimary, w, h, tex_size, (GLvoid *)data_in); // [Viewer Lab]
            stop_glerror();
        }
        else
        {
            if(mFormatSwapBytes)
            {
                gl_pixel_store_unpack(GL_UNPACK_SWAP_BYTES, 1); // [Viewer Lab]
                stop_glerror();
            }

            upload(0, w, h, mFormatType, data_in); // [Viewer Lab]
            LLImageGL::setManualImage(mTarget, 0, mFormatInternal, w, h,
                         mFormatPrimary, mFormatType, (GLvoid *)data_in, mAllowCompression);
            analyzeAlpha(data_in, w, h);

            updatePickMask(w, h, data_in);

            stop_glerror();

            if(mFormatSwapBytes)
            {
                gl_pixel_store_unpack(GL_UNPACK_SWAP_BYTES, 0); // [Viewer Lab]
                stop_glerror();
            }

        }
    }
    stop_glerror();
    mGLTextureCreated = true;
    return true;
}

U32 type_width_from_pixtype(U32 pixtype)
{
    U32 type_width = 0;
    switch (pixtype)
    {
    case GL_UNSIGNED_BYTE:
    case GL_BYTE:
    case GL_UNSIGNED_INT_8_8_8_8_REV:
        type_width = 1;
        break;
    case GL_UNSIGNED_SHORT:
    case GL_SHORT:
        type_width = 2;
        break;
    case GL_UNSIGNED_INT:
    case GL_INT:
    case GL_FLOAT:
        type_width = 4;
        break;
    default:
        LL_ERRS() << "Unknown type: " << pixtype << LL_ENDL;
    }
    return type_width;
}

// [Viewer Lab] A GL backend function as a whole, verbatim below its guard: the vendor choice between one glTexSubImage2D
// and sub_image_lines; false when GL is not the backend
bool should_stagger_image_set(bool compressed)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:should_stagger_image_set");
        return false;
    }
#if LL_DARWIN
    return !compressed && on_main_thread() && gGLManager.mIsAMD;
#else
    // glTexSubImage2D doesn't work with compressed textures on select tested Nvidia GPUs on Windows 10 -Cosmic,2023-03-08
    // Setting media textures off-thread seems faster when not using sub_image_lines (Nvidia/Windows 10) -Cosmic,2023-03-31
    return !compressed && on_main_thread() && !gGLManager.mIsIntel;
#endif
}

// Equivalent to calling glSetSubImage2D(target, miplevel, x_offset, y_offset, width, height, pixformat, pixtype, src), assuming the total width of the image is data_width
// However, instead there are multiple calls to glSetSubImage2D on smaller slices of the image
// [Viewer Lab] A GL backend function as a whole, verbatim below its guard: the driver workaround itself; nothing when GL
// is not the backend
void sub_image_lines(U32 target, S32 miplevel, S32 x_offset, S32 y_offset, S32 width, S32 height, U32 pixformat, U32 pixtype, const U8* src, S32 data_width)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:sub_image_lines");
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    LL_PROFILE_ZONE_NUM(width);
    LL_PROFILE_ZONE_NUM(height);

    U32 components = LLImageGL::dataFormatComponents(pixformat);
    U32 type_width = type_width_from_pixtype(pixtype);

    const U32 line_width = data_width * components * type_width;
    const U32 y_offset_end = y_offset + height;

    if (width == data_width && height % 32 == 0)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("subimage - batched lines");

        // full width, batch multiple lines at a time
        // set batch size based on width
        U32 batch_size = 32;

        if (width > 1024)
        {
            batch_size = 8;
        }
        else if (width > 512)
        {
            batch_size = 16;
        }

        // full width texture, do 32 lines at a time
        for (U32 y_pos = y_offset; y_pos < y_offset_end; y_pos += batch_size)
        {
            // If this keeps crashing, pass down data_size, looks like it is using
            // imageraw->getData(); for data, but goes way over allocated size limit
            glTexSubImage2D(target, miplevel, x_offset, y_pos, width, batch_size, pixformat, pixtype, src);
            src += line_width * batch_size;
        }
    }
    else
    {
        // partial width or strange height
        for (U32 y_pos = y_offset; y_pos < y_offset_end; y_pos += 1)
        {
            // If this keeps crashing, pass down data_size, looks like it is using
            // imageraw->getData(); for data, but goes way over allocated size limit
            glTexSubImage2D(target, miplevel, x_offset, y_pos, width, 1, pixformat, pixtype, src);
            src += line_width;
        }
    }
}

bool LLImageGL::setSubImage(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height, bool force_fast_update /* = false */, LLImageGLHandle use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!width || !height)
    {
        return true;
    }
    LLGLuint tex_name = use_name.mName != 0 ? use_name.mName : mTexName; // [Viewer Lab] the handle's GL name
    // [Viewer Lab] Off GL the image's own texture, which has no GL name, takes the region for any bind target, as a
    // cube face's does; never on GL, whose names are never 0. So does the texture that
    // createGLTexture parked without a GL name for defer_copy's caller, who passes its name 0
    if (0 == tex_name && !has_no_gl_name(mRHITexture, mTexName) && !newRHITexture(0))
    {
        // *TODO: Re-enable warning?  Ran into thread locking issues? DK 2011-02-18
        //LL_WARNS() << "Setting subimage on image without GL texture" << LL_ENDL;
        return false;
    }
    if (datap == NULL)
    {
        // *TODO: Re-enable warning?  Ran into thread locking issues? DK 2011-02-18
        //LL_WARNS() << "Setting subimage on image with NULL datap" << LL_ENDL;
        return false;
    }

    // HACK: allow the caller to explicitly force the fast path (i.e. using glTexSubImage2D here instead of calling setImage) even when updating the full texture.
    if (!force_fast_update && x_pos == 0 && y_pos == 0 && width == getWidth() && height == getHeight() && data_width == width && data_height == height)
    {
        setImage(datap, false, tex_name, newRHITexture(tex_name)); // [Viewer Lab] defer_copy's new texture
    }
    else
    {
        if (mUseMipMaps)
        {
            dump();
            LL_ERRS() << "setSubImage called with mipmapped image (not supported)" << LL_ENDL;
        }
        llassert_always(mCurrentDiscardLevel == 0);
        llassert_always(x_pos >= 0 && y_pos >= 0);

        if (((x_pos + width) > getWidth()) ||
            (y_pos + height) > getHeight())
        {
            dump();
            LL_ERRS() << "Subimage not wholly in target image!"
                   << " x_pos " << x_pos
                   << " y_pos " << y_pos
                   << " width " << width
                   << " height " << height
                   << " getWidth() " << getWidth()
                   << " getHeight() " << getHeight()
                   << LL_ENDL;
        }

        if ((x_pos + width) > data_width ||
            (y_pos + height) > data_height)
        {
            dump();
            LL_ERRS() << "Subimage not wholly in source image!"
                   << " x_pos " << x_pos
                   << " y_pos " << y_pos
                   << " width " << width
                   << " height " << height
                   << " source_width " << data_width
                   << " source_height " << data_height
                   << LL_ENDL;
        }


        gl_pixel_store_unpack(GL_UNPACK_ROW_LENGTH, data_width); // [Viewer Lab]
        stop_glerror();

        if(mFormatSwapBytes)
        {
            gl_pixel_store_unpack(GL_UNPACK_SWAP_BYTES, 1); // [Viewer Lab]
            stop_glerror();
        }

        const U8* sub_datap = datap + (y_pos * data_width + x_pos) * getComponents();
        // Update the GL texture
        bool res = gl_bind_raw_name(gGL.getTexUnit(0), mBindTarget, tex_name, false); // [Viewer Lab] bindManual's GL
        if (!res) LL_ERRS() << "LLImageGL::setSubImage(): bindTexture failed" << LL_ENDL;
        stop_glerror();

        // [Viewer Lab] The region to the texture of the viewer's render device that owns the name, before GL's copy: the
        // image's own on the main thread, or the one createGLTexture parked for the caller's name with defer_copy, on
        // either thread, through the encoder or the upload context. The caller's bytes from the region's first pixel
        // with GL's row length, at alignment 1 and never flipped. A partial update never re-specifies, so it needs no
        // description check; a caller's other raw name sends nothing. Off GL
        // defer_copy's caller passes the name 0, which is mTexName too: the texture parked without a GL name takes the
        // region, not the image's own; GL's names are never 0.
        LL::RHI::Texture* parked = tex_name == 0 ? newRHITexture(0) : nullptr;
        if (LL::RHI::Texture* rhi = parked                                                 ? parked
                                    : (on_main_thread() && LLGLuint(tex_name) == mTexName) ? getRHITexture()
                                                                                           : newRHITexture(tex_name))
        {
            if (const std::optional<LL::RHI::PixelLayout> layout =
                    upload_layout(mFormatPrimary, mFormatType, mFormatSwapBytes, U32(data_width)))
            {
                device_upload(rhi, 0, upload_layer(mTarget, mBindTarget), { U32(x_pos), U32(y_pos), U32(width), U32(height) },
                              *layout, sub_datap, upload_bytes(*layout, U32(width), U32(height), U32(data_width)));
            }
        }

        const bool use_sub_image = should_stagger_image_set(isCompressed());
        if (!use_sub_image)
        {
            // *TODO: Why does this work here, in setSubImage, but not in
            // setManualImage? Maybe because it only gets called with the
            // dimensions of the full image?  Or because the image is never
            // compressed?
            gl_tex_sub_image_2d(mTarget, 0, x_pos, y_pos, width, height, mFormatPrimary, mFormatType, sub_datap); // [Viewer Lab]
        }
        else
        {
            sub_image_lines(mTarget, 0, x_pos, y_pos, width, height, mFormatPrimary, mFormatType, sub_datap, data_width);
        }
        gGL.getTexUnit(0)->disable();
        stop_glerror();

        if(mFormatSwapBytes)
        {
            gl_pixel_store_unpack(GL_UNPACK_SWAP_BYTES, 0); // [Viewer Lab]
            stop_glerror();
        }

        gl_pixel_store_unpack(GL_UNPACK_ROW_LENGTH, 0); // [Viewer Lab]
        stop_glerror();
        mGLTextureCreated = true;
    }
    return true;
}

bool LLImageGL::setSubImage(const LLImageRaw* imageraw, S32 x_pos, S32 y_pos, S32 width, S32 height, bool force_fast_update /* = false */, LLImageGLHandle use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    return setSubImage(imageraw->getData(), imageraw->getWidth(), imageraw->getHeight(), x_pos, y_pos, width, height, force_fast_update, use_name);
}

// Copy sub image from frame buffer
bool LLImageGL::setSubImageFromFrameBuffer(S32 fb_x, S32 fb_y, S32 x_pos, S32 y_pos, S32 width, S32 height)
{
    if (gGL.getTexUnit(0)->bind(this, false, true))
    {
        // [Viewer Lab] Coverage marker, once per session
        static std::atomic<bool> sReached{ false };
        if (!sReached.exchange(true))
        {
            LL_INFOS("RHI") << "Transfer site reached: setSubImageFromFrameBuffer" << LL_ENDL;
        }
        // [Viewer Lab]; off GL a copy that did not reach the encoder copies nothing: false, as a failed bind
        if (!LLGPUCopy::fromFramebuffer(getRHITexture(), GL_TEXTURE_2D, 0, 0, fb_x, fb_y, x_pos, y_pos, width, height))
        {
            return false;
        }
        mGLTextureCreated = true;
        stop_glerror();
        return true;
    }
    else
    {
        return false;
    }
}

// [Viewer Lab] The calling thread's name draws, for nameDraws
static thread_local LLImageGL::NameDraws sNameDraws;
// [Viewer Lab] The name createTextureNames drew for the device texture it makes on this thread; allocateRHITextureName
// takes it once instead of a draw of its own
static thread_local U32 sPredrawnName = 0;

// static
void LLImageGL::generateTextures(S32 numTextures, U32 *textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    ++sNameDraws.calls; // [Viewer Lab]
    sNameDraws.names += U64(llmax(numTextures, 0));
    static constexpr U32 pool_size = 1024;
    static thread_local U32 name_pool[pool_size]; // pool of texture names
    static thread_local U32 name_count = 0; // number of available names in the pool

    if (name_count == 0)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("iglgt - reup pool");
        // pool is emtpy, refill it
        glGenTextures(pool_size, name_pool);
        name_count = pool_size;
    }

    if ((U32)numTextures <= name_count)
    {
        //copy teture names off the end of the pool
        memcpy(textures, name_pool + name_count - numTextures, sizeof(U32) * numTextures);
        name_count -= numTextures;
    }
    else
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("iglgt - pool miss");
        glGenTextures(numTextures, textures);
    }
    sNameDraws.lastBlock.assign(textures, textures + llmax(numTextures, 0)); // [Viewer Lab]
}

// [Viewer Lab]
// static
LLImageGL::NameDraws LLImageGL::nameDraws()
{
    return sNameDraws;
}

constexpr int DELETE_DELAY = 3; // number of frames to wait before deleting textures
static std::vector<U32> sFreeList[DELETE_DELAY+1];
// [Viewer Lab] The accounting's key of each name in sFreeList, taken when the name was queued
static std::vector<U64> sFreeKeys[DELETE_DELAY+1];

// [Viewer Lab] LLImageGL's GL backend functions: each makes the GL calls
// LLImageGL made where it calls them, and nothing when GL is not the backend.

// [Viewer Lab] updateClass's glDeleteTextures of the names whose delay has passed
static void gl_delete_textures(S32 count, const U32* names)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_delete_textures");
        return;
    }
    glDeleteTextures((GLsizei) count, names);
}

// [Viewer Lab] setManualImage's glTexImage2D, with the data or empty before sub_image_lines, and scaleDown's
// empty re-specification
static void gl_tex_image_2d(U32 target, S32 miplevel, S32 intformat, S32 width, S32 height, U32 pixformat, U32 pixtype,
                            const void* pixels)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_tex_image_2d");
        return;
    }
    glTexImage2D(target, miplevel, intformat, width, height, 0, pixformat, pixtype, pixels);
}

// [Viewer Lab] setManualImage's swizzle mask for GL's deprecated alpha and luminance formats. On
// GL_TEXTURE_2D whatever the target, as setManualImage always did (an upstream quirk
// kept on purpose)
static void gl_texture_swizzle_2d(const GLint* mask)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_texture_swizzle_2d");
        return;
    }
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, mask);
}

// [Viewer Lab] createGLTexture's level range on the texture it has just bound on unit 0
static void gl_texture_level_range(U32 target, S32 max_level)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_texture_level_range");
        return;
    }
    glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, max_level);
}

// [Viewer Lab]
// static
void LLImageGL::setLevelRange(LLTexUnit::eTextureType type, S32 max_level)
{
    gl_texture_level_range(LLTexUnit::getInternalType(type), max_level);
}

// [Viewer Lab] How many times a GL name waits in LLImageGL's deletion lists, from deleteTextures until updateClass
// deletes it. Main thread; for the tests.
// static
U32 LLImageGL::pendingDeletions(U32 texName)
{
    U32 count = 0;
    for (const std::vector<U32>& list : sFreeList)
    {
        count += (U32)std::count(list.begin(), list.end(), texName);
    }
    return count;
}

// static
void LLImageGL::updateClass()
{
    sFrameCount++;

    // wait a few frames before actually deleting the textures to avoid
    // synchronization issues with the GPU
    U32 idx = (sFrameCount+DELETE_DELAY) % (DELETE_DELAY+1);

    if (!sFreeList[idx].empty())
    {
        // [Viewer Lab] The accounting frees by the keys taken when the names were queued, and a name of
        // a render interface texture is forgotten as it goes, so a name GL hands out again starts raw
        sTexMemMutex.lock();
        for (size_t i = 0; i < sFreeKeys[idx].size(); ++i)
        {
            free_tex_key_locked(sFreeKeys[idx][i]);
            auto entry = sRHITextureNames.find(sFreeList[idx][i]);
            if (entry != sRHITextureNames.end() && entry->second.id == sFreeKeys[idx][i])
            {
                sRHITextureNames.erase(entry);
            }
        }
        sTexMemMutex.unlock();
        gl_delete_textures((S32) sFreeList[idx].size(), sFreeList[idx].data()); // [Viewer Lab]
        sFreeList[idx].resize(0);
        sFreeKeys[idx].resize(0); // [Viewer Lab]
    }
}

// <Luminous> updateClass deletes textures a few main-loop frames after
// they are given back. Luminous's accumulated renders draw many frames
// inside one main-loop frame, so nothing given back meanwhile would be
// deleted until the render ends: a render that resizes its targets for
// each frame, 16 frames at twice a 5K window, comes to some 120 GB that way.
// Called between such frames, after a read-back has waited for the GPU, so
// none of them is still in use.
// static
void LLImageGL::flushDeletedTextures()
{
    // [Viewer Lab] Each list as updateClass frees it: the accounting by the queued keys, the render interface's names
    // forgotten, the names deleted in the GL backend
    for (int idx = 0; idx <= DELETE_DELAY; ++idx)
    {
        if (!sFreeList[idx].empty())
        {
            sTexMemMutex.lock();
            for (size_t i = 0; i < sFreeKeys[idx].size(); ++i)
            {
                free_tex_key_locked(sFreeKeys[idx][i]);
                auto entry = sRHITextureNames.find(sFreeList[idx][i]);
                if (entry != sRHITextureNames.end() && entry->second.id == sFreeKeys[idx][i])
                {
                    sRHITextureNames.erase(entry);
                }
            }
            sTexMemMutex.unlock();
            gl_delete_textures((S32) sFreeList[idx].size(), sFreeList[idx].data());
            sFreeList[idx].resize(0);
            sFreeKeys[idx].resize(0);
        }
    }
}
// </Luminous>

// static
void LLImageGL::deleteTextures(S32 numTextures, const U32 *textures)
{
    if (gGLManager.mInited)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
        U32 idx = sFrameCount % (DELETE_DELAY+1);
        sTexMemMutex.lock(); // [Viewer Lab]
        for (S32 i = 0; i < numTextures; ++i)
        {
            // [Viewer Lab] A name of a render interface texture goes into the lists once; raw names as
            // before
            auto entry = sRHITextureNames.find(textures[i]);
            if (entry != sRHITextureNames.end())
            {
                if (entry->second.queued)
                {
                    continue;
                }
                entry->second.queued = true;
            }
            sFreeList[idx].push_back(textures[i]);
            sFreeKeys[idx].push_back(texture_key_locked(textures[i])); // [Viewer Lab]
        }
        sTexMemMutex.unlock(); // [Viewer Lab]
    }
}

// [Viewer Lab]
// static
bool LLImageGL::makesOwnGLStorage()
{
    return sCreatingOwnTexture;
}

// [Viewer Lab] The GL backend's name for LLImageGL's own texture: the same pool call as LLImageGL's
// raw names, recorded with the texture's id
// static
U32 LLImageGL::allocateRHITextureName(U64 id)
{
    U32 name = sPredrawnName; // [Viewer Lab] createTextureNames' name, taken once
    sPredrawnName = 0;
    if (!name)
    {
        generateTextures(1, &name);
    }
    sTexMemMutex.lock();
    sRHITextureNames[name] = { id, false };
    sTexMemMutex.unlock();
    return name;
}

// [Viewer Lab] The name of the texture setImage re-specifies, for the new texture that replaces it while
// respecify_rhi_texture makes it on this thread: the registry entry and the accounting move from the old texture's id
// to the new one, with no GL call, so the old texture's release finds its id gone and queues nothing. 0 when no
// adoption is pending, or the old texture no longer owns the name; the adoption is taken once.
// static
U32 LLImageGL::adoptRHITextureName(U64 id)
{
    const RHIAdoption adoption = sRHIAdoption;
    if (adoption.name == 0)
    {
        return 0;
    }
    sRHIAdoption = {};
    U32 name = 0;
    sTexMemMutex.lock();
    auto entry = sRHITextureNames.find(adoption.name);
    if (entry != sRHITextureNames.end() && entry->second.id == adoption.id && !entry->second.queued)
    {
        entry->second.id = id;
        auto alloc = sTextureAllocs.find(adoption.id);
        if (alloc != sTextureAllocs.end())
        {
            const U64 bytes = alloc->second;
            sTextureAllocs.erase(alloc);
            sTextureAllocs[id] = bytes;
        }
        name = adoption.name;
    }
    sTexMemMutex.unlock();
    return name;
}

// [Viewer Lab] The release of LLImageGL's own texture: its name goes into the deletion lists as
// deleteTextures puts it there, unless it is there already (syncTexName deleted it as a raw name) or
// GL has handed it out again since
// static
void LLImageGL::freeRHITextureName(U32 name, U64 id)
{
    if (gGLManager.mInited)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
        sTexMemMutex.lock();
        auto entry = sRHITextureNames.find(name);
        if (entry != sRHITextureNames.end() && entry->second.id == id && !entry->second.queued)
        {
            entry->second.queued = true;
            U32 idx = sFrameCount % (DELETE_DELAY+1);
            sFreeList[idx].push_back(name);
            sFreeKeys[idx].push_back(id);
        }
        sTexMemMutex.unlock();
    }
}

// static
void LLImageGL::setManualImage(U32 target, S32 miplevel, S32 intformat, S32 width, S32 height, U32 pixformat, U32 pixtype, const void* pixels, bool allow_compression)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // [Viewer Lab] The formats, the mask and the conversion, decided by manual_image_format
    const ManualImageFormat format = manual_image_format(intformat, pixformat, pixtype, allow_compression);
    if (LLRender::sGLCoreProfile)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
        if (format.swizzle) // [Viewer Lab]
        {
            gl_texture_swizzle_2d(format.swizzle); // [Viewer Lab]
        }
        else
        {
            if (format.converted == GL_ALPHA) // [Viewer Lab]
            { //GL_ALPHA is deprecated, convert to RGBA
                if (pixels != nullptr)
                {
                    U32 pixel_count = (U32)(width * height);
                    for (U32 i = 0; i < pixel_count; i++)
                    {
                        U8* pix = (U8*)&sManualScratch[i];
                        pix[0] = pix[1] = pix[2] = 0;
                        pix[3] = ((U8*)pixels)[i];
                    }

                    pixels = sManualScratch;
                }
            }

            if (format.converted == GL_LUMINANCE_ALPHA) // [Viewer Lab]
            { //GL_LUMINANCE_ALPHA is deprecated, convert to RGBA
                if (pixels != nullptr)
                {
                    U32 pixel_count = (U32)(width * height);
                    for (U32 i = 0; i < pixel_count; i++)
                    {
                        U8 lum = ((U8*)pixels)[i * 2 + 0];
                        U8 alpha = ((U8*)pixels)[i * 2 + 1];

                        U8* pix = (U8*)&sManualScratch[i];
                        pix[0] = pix[1] = pix[2] = lum;
                        pix[3] = alpha;
                    }

                    pixels = sManualScratch;
                }
            }

            if (format.converted == GL_LUMINANCE) // [Viewer Lab]
            { //GL_LUMINANCE_ALPHA is deprecated, convert to RGB
                if (pixels != nullptr)
                {
                    U32 pixel_count = (U32)(width * height);
                    for (U32 i = 0; i < pixel_count; i++)
                    {
                        U8 lum = ((U8*)pixels)[i];

                        U8* pix = (U8*)&sManualScratch[i];
                        pix[0] = pix[1] = pix[2] = lum;
                        pix[3] = 255;
                    }

                    pixels = sManualScratch;
                }
            }
        }
    }

    if (format.uncompressible) // [Viewer Lab] manual_image_format's switch found no compressed format
    {
        LL_WARNS() << "Could not compress format: " << std::hex << format.intformat << std::dec << LL_ENDL; // [Viewer Lab]
    }
    intformat = format.intformat; // [Viewer Lab]
    pixformat = format.pixformat; // [Viewer Lab]
    const bool compress = format.compress; // [Viewer Lab]

    stop_glerror();
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("glTexImage2D");
        LL_PROFILE_ZONE_NUM(width);
        LL_PROFILE_ZONE_NUM(height);

        free_cur_tex_image();
        const bool use_sub_image = should_stagger_image_set(compress);
        if (!use_sub_image)
        {
            LL_PROFILE_ZONE_NAMED("glTexImage2D alloc + copy");
            gl_tex_image_2d(target, miplevel, intformat, width, height, pixformat, pixtype, pixels); // [Viewer Lab]
        }
        else
        {
            // break up calls to a manageable size for the GL command buffer
            {
                LL_PROFILE_ZONE_NAMED("glTexImage2D alloc");
                gl_tex_image_2d(target, miplevel, intformat, width, height, pixformat, pixtype, nullptr); // [Viewer Lab]
            }

            U8* src = (U8*)(pixels);
            if (src)
            {
                LL_PROFILE_ZONE_NAMED("glTexImage2D copy");
                sub_image_lines(target, miplevel, 0, 0, width, height, pixformat, pixtype, src, width);
            }
        }
        alloc_tex_image(width, height, intformat, 1);
    }
    stop_glerror();
}

// [Viewer Lab]
// static
bool LLImageGL::describeTexture(LL::RHI::TextureDesc& desc, LLTexUnit::eTextureType type, U8 usage, S32 intformat,
                                U32 pixformat, U32 pixtype, bool allow_compression, S32 width, S32 height, U32 mip_levels)
{
    desc = LL::RHI::TextureDesc();
    if (type == LLTexUnit::TT_TEXTURE)
    {
        desc.type = LL::RHI::TextureType::Tex2D;
    }
    else if (type == LLTexUnit::TT_CUBE_MAP)
    {
        desc.type = LL::RHI::TextureType::Cube;
    }
    else
    {
        return false;
    }
    desc.usage = usage;
    desc.width = U32(width);
    desc.height = U32(height);
    desc.mipLevels = mip_levels;
    if (is_compressed_format(pixformat))
    {
        // setImage gives glCompressedTexImage2D the DXT format itself
        desc.glFormat = pixformat;
    }
    else
    {
        const ManualImageFormat format = manual_image_format(intformat, pixformat, pixtype, allow_compression);
        desc.glFormat = U32(format.intformat);
        desc.swizzle = rhi_swizzle(format);
    }
    desc.format = LL::RHI::formatFromGL(desc.glFormat);
    return desc.format != LL::RHI::Format::Unknown;
}

//create an empty GL texture: just create a texture name
//the texture is assiciate with some image by calling glTexImage outside LLImageGL
bool LLImageGL::createGLTexture()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    checkActiveThread();
    dropReplacedRHITexture(); // [Viewer Lab]

    if (gGLManager.mIsDisabled)
    {
        LL_WARNS() << "Trying to create a texture while GL is disabled!" << LL_ENDL;
        return false;
    }

    mGLTextureCreated = false ; //do not save this texture when gl is destroyed.

    llassert(gGLManager.mInited);
    stop_glerror();

    const LLGLuint previous_name = mTexName; // [Viewer Lab]
    if(mTexName)
    {
        releaseTexName(mTexName); // [Viewer Lab]
        mTexName = 0;
    }


    LLImageGL::generateTextures(1, &mTexName);
    updateTextureKey(previous_name, nullptr); // [Viewer Lab] a raw pool name, or 0 when none was made
    stop_glerror();
    if (!mTexName)
    {
        LL_WARNS() << "LLImageGL::createGLTexture failed to make an empty texture" << LL_ENDL;
        return false;
    }

    return true ;
}

// [Viewer Lab] createGLTexture()'s body, with the name taken from a texture of the viewer's render device the image owns:
// create_rhi_texture's ImageGLName path makes only the generateTextures(1), so the name comes from the same pool and
// the caller's GL is as before. The raw name as createOwnTexture's fallback when no texture is made
bool LLImageGL::createEmptyOwnTexture(U8 usage, U32 mip_levels, bool allow_compression)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    checkActiveThread();
    dropReplacedRHITexture();

    if (gGLManager.mIsDisabled)
    {
        LL_WARNS() << "Trying to create a texture while GL is disabled!" << LL_ENDL;
        return false;
    }

    mGLTextureCreated = false ; //do not save this texture when gl is destroyed.

    llassert(gGLManager.mInited);
    stop_glerror();

    const LLGLuint previous_name = mTexName;
    if (mTexName)
    {
        releaseTexName(mTexName);
        mTexName = 0;
    }

    // Described as createGLTexture's data path describes a 2D image (the image's formats at its full size), with the
    // caller's usage, levels and compression; specifyOwnLevel uploads to a 2D texture only
    LL::RHI::TextureDesc desc;
    LLPointer<LL::RHI::Texture> texture;
    if (mTarget == GL_TEXTURE_2D && mBindTarget == LLTexUnit::TT_TEXTURE)
    {
        if (describeTexture(desc, LLTexUnit::TT_TEXTURE, usage, mFormatInternal, mFormatPrimary, mFormatType,
                            allow_compression && mAllowCompression, getWidth(0), getHeight(0), mip_levels))
        {
            texture = create_rhi_texture(desc);
        }
        else
        {
            LL_WARNS_ONCE("RHI") << "LLImageGL keeps a raw GL name for GL format 0x" << std::hex << desc.glFormat
                                 << std::dec << ", which the render interface has no format for" << LL_ENDL;
        }
    }
    if (texture.notNull())
    {
        mRHITexture = texture;
        mTexName = gl_name(texture);
        updateTextureKey(previous_name, texture.get());
    }
    else
    {
        LLImageGL::generateTextures(1, &mTexName);
        updateTextureKey(previous_name, nullptr); // a raw pool name, or 0 when none was made
    }
    stop_glerror();
    // Off GL the image's own texture has no GL name, and is made
    if (!mTexName && !has_no_gl_name(mRHITexture, mTexName))
    {
        LL_WARNS() << "LLImageGL::createEmptyOwnTexture failed to make an empty texture" << LL_ENDL;
        return false;
    }
    return true;
}

// [Viewer Lab] specifyTexture on the image's own texture of the viewer's render device (the device's upload, then GL's),
// else setManualImage alone as before
void LLImageGL::specifyOwnLevel(S32 level, S32 intformat, S32 width, S32 height, U32 pixformat, U32 pixtype,
                                const void* pixels, bool allow_compression)
{
    if (LL::RHI::Texture* texture = getRHITexture())
    {
        specifyTexture(texture, level, intformat, width, height, pixformat, pixtype, pixels, allow_compression);
    }
    else
    {
        setManualImage(GL_TEXTURE_2D, level, intformat, width, height, pixformat, pixtype, pixels, allow_compression);
    }
}

// [Viewer Lab] LLImageGL::syncCounts' mainCreateUs: the main thread's time in createGLTexture with an image
static std::atomic<U64> sMainCreateUs{ 0 };

namespace
{
    /// [Viewer Lab] Adds the time from its making to its end to sMainCreateUs when it is made on the main thread
    class MainCreateTime
    {
    public:
        MainCreateTime() : mOn(on_main_thread()), mStart(std::chrono::steady_clock::now()) {}
        ~MainCreateTime()
        {
            if (mOn)
            {
                sMainCreateUs += U64(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - mStart).count());
            }
        }

    private:
        const bool mOn;
        const std::chrono::steady_clock::time_point mStart;
    };
}

bool LLImageGL::createGLTexture(S32 discard_level, const LLImageRaw* imageraw, S32 usename/*=0*/, bool to_create, S32 category, bool defer_copy, LLImageGLHandle* tex_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    const MainCreateTime timed; // [Viewer Lab]
    checkActiveThread();

    if (gGLManager.mIsDisabled)
    {
        LL_WARNS() << "Trying to create a texture while GL is disabled!" << LL_ENDL;
        return false;
    }

    llassert(gGLManager.mInited);
    stop_glerror();

    if (!imageraw || imageraw->isBufferInvalid())
    {
        LL_WARNS() << "Trying to create a texture from invalid image data" << LL_ENDL;
        mGLTextureCreated = false;
        return false;
    }

    if (discard_level < 0)
    {
        llassert(mCurrentDiscardLevel >= 0);
        discard_level = mCurrentDiscardLevel;
    }
    discard_level = llmin(discard_level, MAX_DISCARD_LEVEL);

    // Actual image width/height = raw image width/height * 2^discard_level
    S32 raw_w = imageraw->getWidth() ;
    S32 raw_h = imageraw->getHeight() ;

    S32 w = raw_w << discard_level;
    S32 h = raw_h << discard_level;

    // setSize may call destroyGLTexture if the size does not match
    if (!setSize(w, h, imageraw->getComponents(), discard_level))
    {
        LL_WARNS() << "Trying to create a texture with incorrect dimensions!" << LL_ENDL;
        mGLTextureCreated = false;
        return false;
    }

    if (mHasExplicitFormat &&
        ((mFormatPrimary == GL_RGBA && mComponents < 4) ||
         (mFormatPrimary == GL_RGB  && mComponents < 3)))

    {
        LL_WARNS()  << "Incorrect format: " << std::hex << mFormatPrimary << " components: " << (U32)mComponents <<  LL_ENDL;
        mHasExplicitFormat = false;
    }

    if( !mHasExplicitFormat )
    {
        switch (mComponents)
        {
        case 1:
            // Use luminance alpha (for fonts)
            mFormatInternal = GL_LUMINANCE8;
            mFormatPrimary = GL_LUMINANCE;
            mFormatType = GL_UNSIGNED_BYTE;
            break;
        case 2:
            // Use luminance alpha (for fonts)
            mFormatInternal = GL_LUMINANCE8_ALPHA8;
            mFormatPrimary = GL_LUMINANCE_ALPHA;
            mFormatType = GL_UNSIGNED_BYTE;
            break;
        case 3:
            mFormatInternal = GL_RGB8;
            mFormatPrimary = GL_RGB;
            mFormatType = GL_UNSIGNED_BYTE;
            break;
        case 4:
            mFormatInternal = GL_RGBA8;
            mFormatPrimary = GL_RGBA;
            mFormatType = GL_UNSIGNED_BYTE;
            break;
        default:
            LL_ERRS() << "Bad number of components for texture: " << (U32)getComponents() << LL_ENDL;
        }

        calcAlphaChannelOffsetAndStride() ;
    }

    if(!to_create) //not create a gl texture
    {
        destroyGLTexture();
        mCurrentDiscardLevel = discard_level;
        mLastBindTime = sLastFrameTime;
        mGLTextureCreated = false;
        return true ;
    }

    setCategory(category);
    const U8* rawdata = imageraw->getData();
    return createGLTexture(discard_level, rawdata, false, usename, defer_copy, tex_name);
}

bool LLImageGL::createGLTexture(S32 discard_level, const U8* data_in, bool data_hasmips, S32 usename, bool defer_copy, LLImageGLHandle* tex_name)
// Call with void data, vmem is allocated but unitialized
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_PROFILE_GPU_ZONE("createGLTexture");
    checkActiveThread();

    bool main_thread = on_main_thread();

    if (defer_copy)
    {
        data_in = nullptr;
    }
    else
    {
        llassert(data_in);
    }

    stop_glerror();

    if (discard_level < 0)
    {
        llassert(mCurrentDiscardLevel >= 0);
        discard_level = mCurrentDiscardLevel;
    }
    discard_level = llclamp(discard_level, 0, (S32)mMaxDiscardLevel);
    discard_level = llmin(discard_level, MAX_DISCARD_LEVEL);

    // [Viewer Lab] Off GL a cube face keeps its cube's texture, which has no name, and draws none
    const bool own_cube = owns_cube_texture(mRHITexture, mTexName, mBindTarget);
    // [Viewer Lab] Off GL defer_copy keeps the image's own texture, which has no GL name, until the caller's swap, as GL
    // keeps its old name until then; on GL, whose names are never 0, the name test ends it
    if (main_thread && !own_cube && !(defer_copy && has_no_gl_name(mRHITexture, mTexName))) // [Viewer Lab]
    {
        dropReplacedRHITexture();
    }

    // [Viewer Lab] Off GL a media update (defer_copy) on the main thread writes the image's own texture in place
    // when nothing is parked and that texture is already what the update would make (the description of the new texture
    // below): the caller gets the name 0 with nothing parked, so setSubImage(0) writes the own texture (no parked one, the
    // main thread, 0 == mTexName) on the main encoder after the upload queue's barrier, which orders the copy after the
    // frames in flight that still sample the old texels (MTL4CommandEncoder.h:48-58), as every main-thread setSubImage;
    // and syncTexName(0) counts the update (takeNewRHITexture). Its levels are marked unwritten first, so a
    // partial region gets GL's zeros around it, as the new texture of each update did. A resize or a
    // format change has destroyed the own texture (setSize, updateMediaImage) or left another description, and
    // updateMediaImage's new placeholder is then written in place too. Never on GL, whose names are never 0, and
    // never on the media update thread, which keeps its new texture and swap
    if (defer_copy && main_thread && usename == 0 && !own_cube && mNewRHITexture.isNull()
        && has_no_gl_name(mRHITexture, mTexName) && mTarget == GL_TEXTURE_2D && mBindTarget == LLTexUnit::TT_TEXTURE)
    {
        LL::RHI::TextureDesc wanted;
        if (describeTexture(wanted, LLTexUnit::TT_TEXTURE, LL::RHI::TEXTURE_SAMPLED | LL::RHI::TEXTURE_READBACK,
                            mFormatInternal, mFormatPrimary, mFormatType, mAllowCompression, getWidth(discard_level),
                            getHeight(discard_level), mUseMipMaps ? U32(mMaxDiscardLevel - discard_level + 1) : 1u)
            && wanted == mRHITexture->desc())
        {
            if (tex_name != nullptr)
            {
                *tex_name = LLImageGLHandle(); // the name 0, as the new texture's off GL
            }
            if (mUseMipMaps)
            {
                mAutoGenMips = true;
            }
            mCurrentDiscardLevel = discard_level;
            device_forget_written(mRHITexture.get());
            // the unit as the new-texture path leaves it
            gGL.getTexUnit(0)->unbind(mBindTarget);
            mInPlacePending = true;
            mTextureMemory = (S64Bytes)getMipBytes(mCurrentDiscardLevel);
            mLastBindTime = sLastFrameTime;
            checkActiveThread();
            return true;
        }
    }

    if (main_thread // <--- always force creation of new_texname when not on main thread ...
        && !defer_copy // <--- ... or defer copy is set
        && mTexName != 0 && discard_level == mCurrentDiscardLevel)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("cglt - early setImage");
        // This will only be true if the size has not changed
        if (tex_name != nullptr)
        {
            *tex_name = LLImageGLHandle(mTexName); // [Viewer Lab] the name as a handle
        }
        return setImage(data_in, data_hasmips);
    }

    GLuint old_texname = mTexName;
    GLuint new_texname = 0;
    LLPointer<LL::RHI::Texture> new_texture; // [Viewer Lab]
    if (usename != 0 || own_cube) // [Viewer Lab] own_cube: setImage uploads to the face's texture
    {
        llassert(main_thread);
        new_texname = usename;
    }
    else
    {
        // [Viewer Lab] A texture of the viewer's render device for a 2D texture LLImageGL makes itself on
        // the main thread or the upload thread, described with the formats setImage gives setManualImage;
        // otherwise a raw name from the pool as before. Also on the upload thread and with defer_copy.
        // Sampled and read back
        if (mTarget == GL_TEXTURE_2D && mBindTarget == LLTexUnit::TT_TEXTURE)
        {
            LL::RHI::TextureDesc desc;
            if (describeTexture(desc, LLTexUnit::TT_TEXTURE, LL::RHI::TEXTURE_SAMPLED | LL::RHI::TEXTURE_READBACK,
                                mFormatInternal, mFormatPrimary,
                                mFormatType, mAllowCompression, getWidth(discard_level), getHeight(discard_level),
                                mUseMipMaps ? U32(mMaxDiscardLevel - discard_level + 1) : 1u))
            {
                new_texture = create_rhi_texture(desc, !main_thread);
            }
            else
            {
                LL_WARNS_ONCE("RHI") << "LLImageGL keeps a raw GL name for GL format 0x" << std::hex << desc.glFormat
                                     << std::dec << ", which the render interface has no format for" << LL_ENDL;
            }
        }
        if (new_texture.notNull())
        {
            new_texname = gl_name(new_texture);
        }
        else
        {
            LLImageGL::generateTextures(1, &new_texname);
        }
        {
            gGL.getTexUnit(0)->bind(this, false, false, new_texname);
            gl_texture_level_range(LLTexUnit::getInternalType(mBindTarget), mMaxDiscardLevel - discard_level); // [Viewer Lab]
        }
    }

    if (tex_name != nullptr)
    {
        *tex_name = LLImageGLHandle(new_texname); // [Viewer Lab] the name as a handle
    }

    if (mUseMipMaps)
    {
        mAutoGenMips = true;
    }

    mCurrentDiscardLevel = discard_level;

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("cglt - late setImage");
        if (!setImage(data_in, data_hasmips, new_texname, new_texture.get())) // [Viewer Lab] the texture to track
        {
            // [Viewer Lab] The new name stays, as the GL path leaves it
            if (new_texture.notNull())
            {
                keep_unreleased(new_texture);
            }
            return false;
        }
    }

    // Set texture options to our defaults.
    gGL.getTexUnit(0)->setHasMipMaps(mHasMipMaps);
    // [Viewer Lab] Off GL the new texture has no GL name, so the bind above left unit 0 on the default texture or an
    // earlier binding, never on it; the setters would write that texture's options. The new texture's slot stays empty, as
    // GL's raw bind of a new name leaves it, and its first bind takes this image's options (samplingOf, or bind's dirty
    // path). On GL, whose names are never 0, the calls are as before
    if (new_texture.isNull() || new_texname != 0)
    {
        gGL.getTexUnit(0)->setTextureAddressMode(mAddressMode);
        gGL.getTexUnit(0)->setTextureFilteringOption(mFilterOption);
    }

    // things will break if we don't unbind after creation
    gGL.getTexUnit(0)->unbind(mBindTarget);

    //if we're on the image loading thread, be sure to delete old_texname and update mTexName on the main thread
    if (!defer_copy)
    {
        if (!main_thread)
        {
            syncToMainThread(new_texname, std::move(new_texture)); // [Viewer Lab] the handle swap
        }
        else
        {
            //not on background thread, immediately set mTexName
            if (old_texname != 0 && old_texname != new_texname)
            {
                releaseTexName(old_texname); // [Viewer Lab]
            }
            mTexName = new_texname;
            if (new_texture.notNull()) // [Viewer Lab]
            {
                mRHITexture = new_texture;
            }
            updateTextureKey(old_texname, new_texture.get()); // [Viewer Lab]
        }
    }
    else
    {
        parkNewRHITexture(new_texture); // [Viewer Lab] the caller's syncTexName or syncToMainThread swaps it in
    }


    mTextureMemory = (S64Bytes)getMipBytes(mCurrentDiscardLevel);

    // mark this as bound at this point, so we don't throw it out immediately
    mLastBindTime = sLastFrameTime;

    checkActiveThread();
    return true;
}

// [Viewer Lab] LLRenderTarget's GL sequence for an attachment (generateTextures, bindManual on
// unit 0, setManualImage with no pixels), for a texture of the viewer's render device whose name comes
// from LLImageGL's pool, with no glGetError
// static
LLPointer<LL::RHI::Texture> LLImageGL::createEmptyTexture(LLTexUnit::eTextureType type, S32 intformat, U32 pixformat,
                                                          U32 pixtype, S32 width, S32 height, U8 usage, S32 max_level,
                                                          bool allow_compression)
{
    if (gGLManager.mIsDisabled)
    {
        return nullptr;
    }
    LL::RHI::TextureDesc desc;
    if (!describeTexture(desc, type, usage, intformat, pixformat, pixtype, allow_compression, width, height,
                         max_level >= 0 ? U32(max_level + 1) : 1u))
    {
        if (type == LLTexUnit::TT_TEXTURE || type == LLTexUnit::TT_CUBE_MAP)
        {
            LL_WARNS_ONCE("RHI") << "LLImageGL keeps a raw GL name for GL format 0x" << std::hex << desc.glFormat
                                 << std::dec << ", which the render interface has no format for" << LL_ENDL;
        }
        return nullptr;
    }
    LLPointer<LL::RHI::Texture> texture = create_rhi_texture(desc);
    if (texture.isNull())
    {
        return nullptr;
    }
    // <Luminous> A memoryless texture (Metal) is never bound to a unit, and has no image to set or memory to count
    if (usage & LL::RHI::TEXTURE_MEMORYLESS)
    {
        return texture;
    }
    // </Luminous>
    const U32 target = LLTexUnit::getInternalType(type);
    gGL.getTexUnit(0)->bindTexture(type, texture.get()); // [Viewer Lab] bindManual's GL for the texture's name
    if (max_level >= 0)
    {
        gl_texture_level_range(target, max_level);
    }
    for (U32 face = 0; face < (type == LLTexUnit::TT_CUBE_MAP ? 6u : 1u); ++face)
    {
        setManualImage(type == LLTexUnit::TT_CUBE_MAP ? U32(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face) : target, 0, intformat,
                       width, height, pixformat, pixtype, nullptr, allow_compression);
    }
    return texture;
}

// [Viewer Lab] The pipeline's textures without an image as textures of the viewer's render device: the description and
// the device's validation before any GL, then one generateTextures(count) in the block order and texture i made on
// the i-th name, which the GL backend takes through allocateRHITextureName
// static
bool LLImageGL::createTextureNames(std::vector<LLPointer<LL::RHI::Texture>>& textures, U32 count,
                                   LLTexUnit::eTextureType type, S32 intformat, U32 pixformat, U32 pixtype, S32 width,
                                   S32 height, U8 usage, bool allow_compression, U32 mip_levels)
{
    textures.clear();
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (gGLManager.mIsDisabled || !device || !on_main_thread() || type != LLTexUnit::TT_TEXTURE || count == 0)
    {
        return false;
    }
    LL::RHI::TextureDesc desc;
    std::string why;
    // [Viewer Lab] the caller's levels, validated with the rest below
    if (!describeTexture(desc, type, usage, intformat, pixformat, pixtype, allow_compression, width, height, mip_levels))
    {
        why = "the render interface has no format for it";
    }
    else
    {
        why = LL::RHI::validate(desc, device->capabilities());
    }
    if (!why.empty())
    {
        LL_WARNS_ONCE("RHI") << "LLImageGL refuses textures for GL format 0x" << std::hex << intformat << std::dec << " at "
                             << width << "x" << height << " and draws no GL names, so the caller keeps none: " << why
                             << LL_ENDL;
        return false;
    }
    // [Viewer Lab] only the GL backend draws names; off GL the textures have none
    const bool gl = gl_is_backend();
    if (!gl)
    {
        LL_GL_GATED("llimagegl:LLImageGL::createTextureNames");
    }
    std::vector<U32> names(count);
    if (gl)
    {
        generateTextures(S32(count), names.data());
    }
    textures.reserve(count);
    for (U32 i = 0; i < count; ++i)
    {
        // The GL backend takes names[i] for exactly this texture, and none is left behind if it throws
        struct Predrawn
        {
            explicit Predrawn(U32 name) { sPredrawnName = name; }
            ~Predrawn() { sPredrawnName = 0; }
        } predrawn(names[i]);
        LLPointer<LL::RHI::Texture> texture = create_rhi_texture(desc);
        if (texture.isNull())
        {
            // Cannot happen after the validation; the names no texture took go back, so none leaks
            llassert(texture.notNull());
            const U32 first = sPredrawnName == names[i] ? i : i + 1;
            if (gl && first < count)
            {
                deleteTextures(S32(count - first), names.data() + first);
            }
            textures.clear();
            return false;
        }
        textures.push_back(texture);
    }
    return true;
}

// [Viewer Lab] setManualImage on the texture the caller has bound, after the tracked device upload of the level (the
// device first, then GL)
// static
void LLImageGL::specifyTexture(LL::RHI::Texture* texture, S32 level, S32 intformat, S32 width, S32 height, U32 pixformat,
                               U32 pixtype, const void* pixels, bool allow_compression)
{
    llassert(texture);
    llassert(!texture || !on_main_thread() || !LL::RHI::viewerDevice() ||
             gGL.getTexUnit(gGL.getCurrentTexUnitIndex())->getCurrTextureId() == texture->id());
    if (texture && pixels && on_main_thread() && LL::RHI::viewerDevice())
    {
        if (const std::optional<LL::RHI::PixelLayout> layout = upload_layout(pixformat, pixtype, false, 0))
        {
            device_upload(texture, U32(level), 0, { 0, 0, U32(width), U32(height) }, *layout, pixels,
                          upload_bytes(*layout, U32(width), U32(height), 0));
        }
    }
    setManualImage(GL_TEXTURE_2D, level, intformat, width, height, pixformat, pixtype, pixels, allow_compression);
}

// [Viewer Lab] The pipeline's GL sequence for a texture with data (generateTextures(1), bindManual on unit 0,
// setManualImage of level 0 with no level range), for a texture of the viewer's render device
// static
LLPointer<LL::RHI::Texture> LLImageGL::createTextureWithData(LLTexUnit::eTextureType type, S32 intformat, U32 pixformat,
                                                             U32 pixtype, S32 width, S32 height, U8 usage,
                                                             const void* pixels, bool allow_compression)
{
    std::vector<LLPointer<LL::RHI::Texture>> textures;
    if (!createTextureNames(textures, 1, type, intformat, pixformat, pixtype, width, height, usage, allow_compression))
    {
        return nullptr;
    }
    gGL.getTexUnit(0)->bindTexture(type, textures[0].get()); // [Viewer Lab] bindManual's GL for the texture's name
    specifyTexture(textures[0].get(), 0, intformat, width, height, pixformat, pixtype, pixels, allow_compression);
    return textures[0];
}

// [Viewer Lab] LLCubeMap's and LLCubeMapArray's texture of the viewer's render device: the ImageGLName variant makes
// only the generateTextures(1), so the name is drawn from the same pool and the caller's GL is as before. A cube map
// array is described as a cube, then given its type and cubes, since describeTexture describes 2D textures and cubes
// only. The raw name as before when no texture is made
bool LLImageGL::createOwnTexture(LLTexUnit::eTextureType type, S32 intformat, U32 pixformat, U32 pixtype, U8 usage,
                                 U32 mip_levels, U32 layers, bool allow_compression)
{
    LL::RHI::TextureDesc desc;
    LLPointer<LL::RHI::Texture> texture;
    if (describeTexture(desc, type == LLTexUnit::TT_CUBE_MAP_ARRAY ? LLTexUnit::TT_CUBE_MAP : type, usage, intformat,
                        pixformat, pixtype, allow_compression && mAllowCompression, getWidth(0), getHeight(0), mip_levels))
    {
        if (type == LLTexUnit::TT_CUBE_MAP_ARRAY)
        {
            desc.type = LL::RHI::TextureType::CubeArray;
            desc.layers = layers;
        }
        // An empty cube map array (no reflection probes, as in safe mode) is legal GL but not a valid texture of the
        // device: the raw name below, without asking the device, which would refuse it with a warning
        if (desc.layers > 0)
        {
            texture = create_rhi_texture(desc);
        }
    }
    else if (type == LLTexUnit::TT_TEXTURE || type == LLTexUnit::TT_CUBE_MAP || type == LLTexUnit::TT_CUBE_MAP_ARRAY)
    {
        LL_WARNS_ONCE("RHI") << "LLImageGL keeps a raw GL name for GL format 0x" << std::hex << desc.glFormat << std::dec
                             << ", which the render interface has no format for" << LL_ENDL;
    }
    if (texture.notNull())
    {
        const LLGLuint previous_name = mTexName; // [Viewer Lab]
        dropReplacedRHITexture();
        mRHITexture = texture;
        mTexName = gl_name(texture);
        updateTextureKey(previous_name, texture.get()); // [Viewer Lab]
        return true;
    }
    U32 name = 0;
    LLImageGL::generateTextures(1, &name);
    setTexName(name);
    return false;
}

// [Viewer Lab] load_exr's sequence, here so that a unit test runs it. The image takes setSize's state without its
// power-of-two check: LLImageGL(width, height, 4, true) left another size at 0 x 0, so createOwnTexture described a 1 x 1
// texture with the size's full chain, which the device refused with a WARN, and the image fell back to a raw name. A
// power-of-two size gets the same state and GL calls as before.
// static
LLPointer<LLImageGL> LLImageGL::createFloatImage(S32 width, S32 height, const F32* rgba)
{
    LLPointer<LLImageGL> image = new LLImageGL(true);
    image->mWidth           = width;
    image->mHeight          = height;
    image->mComponents      = 4;
    image->mMaxDiscardLevel = 0;
    for (S32 w = width, h = height; w > 1 && h > 1 && image->mMaxDiscardLevel < MAX_DISCARD_LEVEL; w >>= 1, h >>= 1)
    {
        image->mMaxDiscardLevel++;
    }

    // A texture of the viewer's render device on the one name from LLImageGL's pool, with no other GL call, and
    // the options; the level and its accounting through specifyTexture (setManualImage's GL), the mipmaps through
    // LLImageGL
    image->createOwnTexture(LLTexUnit::TT_TEXTURE, GL_RGB16F, GL_RGBA, GL_FLOAT, LL::RHI::TEXTURE_SAMPLED,
                            LL::RHI::fullMipCount(U32(width), U32(height)), 1, false);
    image->setAddressMode(LLTexUnit::TAM_CLAMP);
    image->setHasMipMaps(true);
    image->setUseMipMaps(true);
    image->setFilteringOption(LLTexUnit::TFO_TRILINEAR);

    gGL.getTexUnit(0)->bind(image);

    LL::RHI::Texture* texture = image->getRHITexture();
    if (texture)
    {
        specifyTexture(texture, 0, GL_RGB16F, width, height, GL_RGBA, GL_FLOAT, rgba, false);
    }
    else
    {
        setManualImage(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, GL_RGBA, GL_FLOAT, rgba, false);
    }

    generateMipmaps(texture, GL_TEXTURE_2D);

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    return image;
}

// [Viewer Lab] The member itself, not getTexName, so the shared name adds no transition token; the texture only while
// `first` owns the name
void LLImageGL::shareTexture(const LLImageGL& first)
{
    const LLGLuint previous_name = mTexName; // [Viewer Lab]
    mTexName = first.mTexName;
    if (owns_name(first.mRHITexture, first.mTexName) ||
        owns_cube_texture(first.mRHITexture, first.mTexName, first.mBindTarget)) // [Viewer Lab]
    {
        mRHITexture = first.mRHITexture;
    }
    else
    {
        mRHITexture = nullptr;
    }
    updateTextureKey(previous_name, mRHITexture.get()); // [Viewer Lab] first's texture while it owns the name
}

// [Viewer Lab] LLImageGL::syncCounts' counts
static std::atomic<U64> sThreadSyncs{ 0 };
static std::atomic<U64> sSwaps{ 0 };
static std::atomic<U64> sHandleSwaps{ 0 };
static std::atomic<U64> sFences{ 0 };
static std::atomic<U64> sFenceWaits{ 0 };
static std::atomic<U64> sDeferredSwaps{ 0 };
static std::atomic<U64> sFencelessSyncs{ 0 }; // [Viewer Lab]
static std::atomic<U64> sThreadHandleSwaps{ 0 }; // [Viewer Lab]
static std::atomic<U64> sDeferredThreadSwaps{ 0 }; // [Viewer Lab]
static std::atomic<U64> sSwapMipUs{ 0 }; // [Viewer Lab]
static std::atomic<U64> sInPlaceUpdates{ 0 }; // [Viewer Lab]

// [Viewer Lab]
// static
LLImageGL::SyncCounts LLImageGL::syncCounts()
{
    SyncCounts counts;
    counts.threadSyncs = sThreadSyncs;
    counts.swaps = sSwaps;
    counts.handleSwaps = sHandleSwaps;
    // [Viewer Lab] The waits before the fences: each wait follows its fence's count, so fenceWaits is never above fences,
    // also while a sync runs on the upload thread
    counts.fenceWaits = sFenceWaits;
    counts.fences = sFences;
    counts.deferredSwaps = sDeferredSwaps;
    counts.fencelessSyncs = sFencelessSyncs;
    counts.threadHandleSwaps = sThreadHandleSwaps;
    counts.deferredThreadSwaps = sDeferredThreadSwaps;
    counts.mainCreateUs = sMainCreateUs; // [Viewer Lab]
    counts.swapMipUs = sSwapMipUs;
    counts.inPlaceUpdates = sInPlaceUpdates; // [Viewer Lab]
    return counts;
}

// [Viewer Lab] syncToMainThread's GL synchronization of the upload thread, verbatim: on NVIDIA the upload thread waits
// on the CPU; elsewhere it posts GL's wait to the main queue, before the swap. The vendor's wait type is GL backend
// code, and the upload context's fence makes no GL call inside LLImageGL's scope.
// Nothing when GL is not the backend.
static void gl_sync_to_main_thread(const LL::WorkQueue::weak_t& main_queue)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llimagegl:gl_sync_to_main_thread");
        return;
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("cglt - sync");
        if (gGLManager.mIsNVIDIA)
        {
            // wait for texture upload to finish before notifying main thread
            // upload is complete
            auto sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            glClientWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
            glDeleteSync(sync);
        }
        else
        {
            // post a sync to the main thread (will execute before tex name swap lambda below)
            // glFlush calls here are partly superstitious and partly backed by observation
            // on AMD hardware
            glFlush();
            auto sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            LL::WorkQueue::postMaybe(
                main_queue,
                [=]()
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("cglt - wait sync");
                    {
                        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("glWaitSync");
                        glWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
                    }
                    {
                        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("glDeleteSync");
                        glDeleteSync(sync);
                    }
                });
        }
    }
}

// [Viewer Lab] The upload context's submit() inside LLImageGL's scope, whose fence the swap waits for, on the device
// and the thread as with_recorder chooses them, never on the backend, counted in fences. Null without the viewer's device
// or on the main thread.
static LLPointer<LL::RHI::Fence> device_submit()
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    if (!device || on_main_thread())
    {
        return nullptr;
    }
    LLImageGL::OwnGLUploads own;
    LLPointer<LL::RHI::Fence> fence = device->uploadContext().submit();
    if (fence.notNull())
    {
        ++sFences;
        // [Viewer Lab] For the tests' log only, which fence it was, by its debug name
        if (sDeviceCallLog.load())
        {
            LLImageGL::DeviceCall record;
            record.kind = 'f';
            record.uploadContext = true;
            record.name = fence->debugName();
            sTexMemMutex.lock();
            if (std::vector<LLImageGL::DeviceCall>* log = sDeviceCallLog.load())
            {
                log->push_back(record);
            }
            sTexMemMutex.unlock();
        }
    }
    return fence;
}

void LLImageGL::syncToMainThread(LLImageGLHandle handle)
{
    const LLGLuint new_tex_name = handle.mName; // [Viewer Lab] the handle's GL name
    // [Viewer Lab] The texture createGLTexture parked for the name with defer_copy, or a raw name as before;
    // the deferred textures taken here also counted apart
    LLPointer<LL::RHI::Texture> texture = takeNewRHITexture(new_tex_name);
    if (texture.notNull())
    {
        ++sDeferredThreadSwaps;
    }
    syncToMainThread(new_tex_name, std::move(texture));
}

// [Viewer Lab] syncToMainThread's body, with the texture of the viewer's render device that createGLTexture has made on
// this thread for new_tex_name, or null: submit()'s fence, then GL's own synchronization, then the swap item, which
// waits for the fence and gives the image the texture as its handle
void LLImageGL::syncToMainThread(LLGLuint new_tex_name, LLPointer<LL::RHI::Texture>&& new_texture)
{
    LL_PROFILE_ZONE_SCOPED;
    llassert(!on_main_thread());
    ++sThreadSyncs; // [Viewer Lab]

    LLPointer<LL::RHI::Fence> submitted = device_submit(); // [Viewer Lab]
    if (submitted.isNull() && LL::RHI::viewerDevice())
    {
        ++sFencelessSyncs; // [Viewer Lab] a sync with the device but no fence, which the golden tests fail
    }
    gl_sync_to_main_thread(mMainQueue); // [Viewer Lab]

    // [Viewer Lab] The fence and the texture go to the main thread with one reference each, taken over from this thread
    // before the item is posted and let go of only by the item, so this thread releases neither. An item that never
    // runs (the main queue closed at shutdown) leaks them, as it leaks the image's reference and the raw name, and
    // nothing is released in static destruction
    LL::RHI::Fence* fence = submitted.get();
    if (fence)
    {
        fence->ref();
    }
    submitted = nullptr;
    LL::RHI::Texture* texture = new_texture.get();
    if (texture)
    {
        texture->ref();
    }
    new_texture = nullptr;

    ref();
    LL::WorkQueue::postMaybe(
        mMainQueue,
        [=, this]()
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("cglt - delete callback");
            // [Viewer Lab] Bound only after the fence. The main thread's encoder waits (llrhi.h)
            // for it, so the main thread never waits on the CPU. On Metal the next command buffer waits on the GPU for
            // the upload queue, and the mips the thread asked for are made here; GL's waits for nothing for its own
            // ImageGLFence, whose wait() is empty, as before. Without the viewer's device, the CPU wait as before.
            // fenceWaits counts the handover; swap_mip_us times it
            if (fence)
            {
                const auto waited = std::chrono::steady_clock::now();
                if (LL::RHI::Device* device = LL::RHI::viewerDevice())
                {
                    device->encoder().waitForFence(fence);
                }
                else
                {
                    fence->wait();
                }
                sSwapMipUs += U64(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - waited).count());
                fence->unref();
                ++sFenceWaits;
            }
            LLPointer<LL::RHI::Texture> handle = texture;
            if (texture)
            {
                texture->unref();
            }
            // [Viewer Lab]; this item's handle swaps counted apart, and said once
            if (swapTexName(new_tex_name, handle) && ++sThreadHandleSwaps == 1)
            {
                LL_INFOS("RHI") << "LLImageGL: first handle swap from the upload thread" << LL_ENDL;
            }
            unref();
        });

    LL_PROFILER_GPU_COLLECT;
}


void LLImageGL::syncTexName(LLImageGLHandle handle)
{
    const LLGLuint texname = handle.mName; // [Viewer Lab] the handle's GL name
    // [Viewer Lab] The texture createGLTexture parked for the name with defer_copy, or a raw name as before
    swapTexName(texname, takeNewRHITexture(texname));
}

// [Viewer Lab] defer_copy's new texture waits here for the caller's swap. A texture still parked from a creation that
// was never swapped in keeps its name unreleased, as the GL path never deletes such a name
void LLImageGL::parkNewRHITexture(const LLPointer<LL::RHI::Texture>& texture)
{
    if (texture.isNull())
    {
        return;
    }
    if (mNewRHITexture.notNull())
    {
        keep_unreleased(mNewRHITexture);
    }
    mNewRHITexture = texture;
    mInPlacePending = false; // [Viewer Lab] the parked texture replaces what was written in place
}

// [Viewer Lab] Whether the parked texture is the one for the caller's name: its GL name, or off GL, where defer_copy
// gives the caller the name 0, a parked texture without one. GL's names are never 0, so GL tests the name as before
bool LLImageGL::parkedFor(LLGLuint name) const
{
    return name != 0 ? gl_name(mNewRHITexture) == name : has_no_gl_name(mNewRHITexture, name);
}

// [Viewer Lab] For a media update's swap: each texture it gives is counted. Off GL an update createGLTexture
// made in place landed at the caller's swap point as a swap did: counted in deferredSwaps, so "a media update landed"
// keeps its counter, and apart in inPlaceUpdates, with no texture to give. Only the main thread sets
// mInPlacePending, and only its syncTexName takes it; the upload thread's syncToMainThread finds it false
LLPointer<LL::RHI::Texture> LLImageGL::takeNewRHITexture(LLGLuint name)
{
    LLPointer<LL::RHI::Texture> texture;
    if (parkedFor(name)) // [Viewer Lab]
    {
        texture = std::move(mNewRHITexture);
        ++sDeferredSwaps;
    }
    if (mInPlacePending)
    {
        if (texture.isNull() && name == 0)
        {
            ++sDeferredSwaps;
            ++sInPlaceUpdates;
        }
        mInPlacePending = false;
    }
    return texture;
}

// [Viewer Lab]
LL::RHI::Texture* LLImageGL::newRHITexture(LLGLuint name) const
{
    return parkedFor(name) ? mNewRHITexture.get() : nullptr; // [Viewer Lab]
}

// [Viewer Lab] syncTexName's swap on the main thread, as before: the old name to the deletion lists, then the new name.
// With a texture of the viewer's render device whose name it is, the image gets that texture as its handle (a
// new resource, a swapped handle), and mTexName stays its GL mirror; the old texture's release then finds its name
// queued. swaps counts every swap of a name, handleSwaps those with a texture.
// Returns whether the image got the texture as its handle.
bool LLImageGL::swapTexName(LLGLuint texname, const LLPointer<LL::RHI::Texture>& texture)
{
    llassert(on_main_thread());
    if (texname != 0)
    {
        ++sSwaps; // [Viewer Lab]
        const LLGLuint previous_name = mTexName; // [Viewer Lab]
        if (mTexName != 0 && mTexName != texname)
        {
            // [Viewer Lab] The units' told records let go of the old texture first, so a later skip of its name, which GL
            // may hand out again, finds no foreign slot on a channel; CPU records only, no GL call
            if (owns_name(mRHITexture, mTexName))
            {
                LLTexUnit::releaseTexture(mRHITexture.get());
            }
            LLImageGL::deleteTextures(1, &mTexName);
        }
        mTexName = texname;
        if (texture.notNull() && gl_name(texture) == texname)
        {
            mRHITexture = texture;
            updateTextureKey(previous_name, texture.get()); // [Viewer Lab]
            // Said once: LL_INFOS_ONCE alone repeats the line at every hundredth swap
            if (++sHandleSwaps == 1)
            {
                LL_INFOS_ONCE("RHI") << "LLImageGL: first handle swap" << LL_ENDL;
            }
            return true;
        }
        updateTextureKey(previous_name, nullptr); // [Viewer Lab] a raw name
    }
    else if (has_no_gl_name(texture, texname))
    {
        // [Viewer Lab] Off GL defer_copy's swap of the name 0: the texture parked without a GL name replaces the image's
        // own, which goes as dropReplacedRHITexture lets one go off GL, the units' told records first and then its
        // accounting key; counted and said once as above. GL's names are never 0, so GL never gets here
        ++sSwaps;
        if (has_no_gl_name(mRHITexture, mTexName))
        {
            LLTexUnit::releaseTexture(mRHITexture.get());
            free_tex_key(mRHITexture->id());
        }
        mRHITexture = texture;
        updateTextureKey(mTexName, texture.get());
        if (++sHandleSwaps == 1)
        {
            LL_INFOS_ONCE("RHI") << "LLImageGL: first handle swap" << LL_ENDL;
        }
        return true;
    }
    return false;
}

// [Viewer Lab] setTexName deletes nothing, so a texture of the viewer's render device whose
// name it replaces is kept unreleased
void LLImageGL::setTexName(GLuint texName)
{
    const LLGLuint previous_name = mTexName; // [Viewer Lab]
    dropReplacedRHITexture();
    if (mRHITexture.notNull() && gl_name(mRHITexture) != texName)
    {
        keep_unreleased(mRHITexture);
    }
    mTexName = texName;
    updateTextureKey(previous_name, nullptr); // [Viewer Lab] a changed name is raw: no texture owns it by now
}

// [Viewer Lab] The two regimes, split by has_no_gl_name. With a GL name (always on GL) the key follows the name: it is
// recomputed only when the name changed, from the owner the caller passes, so a re-specification in place keeps it.
// Without one, it follows the image's own texture that has no GL name (off GL): the sites that replace
// mRHITexture and keep the name 0 call this too. A compare and at most one id(); no lock, no registry, no GL
void LLImageGL::updateTextureKey(LLGLuint previous_name, const LL::RHI::Texture* owner)
{
    if (mTexName != 0)
    {
        if (mTexName != previous_name)
        {
            mTextureKey = owner ? owner->id() : (LLImageGLMemory::RAW_NAME_KEY | mTexName);
        }
        return;
    }
    mTextureKey = has_no_gl_name(mRHITexture, mTexName) ? mRHITexture->id() : 0;
}

// [Viewer Lab]
bool LLImageGL::hasTexture() const
{
    // [Viewer Lab] A GL name, or off GL the own texture without one (getRHITexture's second case)
    return mTexName != 0 || getRHITexture() != nullptr;
}

LL::RHI::Texture* LLImageGL::getRHITexture() const
{
    // [Viewer Lab] Off GL the image's own texture without a GL name, for any bind target
    return (owns_name(mRHITexture, mTexName) || has_no_gl_name(mRHITexture, mTexName)) ? mRHITexture.get() : nullptr;
}

// [Viewer Lab] syncTexName has swapped a raw name in and deleted the texture's name already, so the
// texture owns no name of this image, also when GL has handed its old name out to the image again:
// letting go of it deletes nothing, since freeRHITextureName finds its name queued, or forgotten once
// updateClass has deleted it
void LLImageGL::dropReplacedRHITexture()
{
    if (mRHITexture.notNull() && !owns_name(mRHITexture, mTexName))
    {
        if (has_no_gl_name(mRHITexture, mTexName))
        {
            free_tex_key(mRHITexture->id()); // [Viewer Lab] off GL the facade lets go of it here
        }
        mRHITexture = nullptr;
        updateTextureKey(mTexName, nullptr); // [Viewer Lab] kept with a GL name; off GL 0
    }
}

// [Viewer Lab] Where LLImageGL deleted a name before: through its texture of the viewer's render device
// when that texture owns the name, so the name goes into the deletion lists once; any other name as a
// raw name, as before
void LLImageGL::releaseTexName(U32 name)
{
    if (owns_name(mRHITexture, name))
    {
        LLTexUnit::releaseTexture(mRHITexture.get()); // [Viewer Lab] the encoder lets go first, so the name goes now
        mRHITexture = nullptr;
    }
    else
    {
        LLImageGL::deleteTextures(1, &name);
    }
}

bool LLImageGL::readBackRaw(S32 discard_level, LLImageRaw* imageraw, bool compressed_ok) const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }

    // [Viewer Lab] A texture on the current backend: off GL the own texture has no GL name
    if (!hasTexture() || discard_level < mCurrentDiscardLevel || discard_level > mMaxDiscardLevel )
    {
        return false;
    }

    S32 gl_discard = discard_level - mCurrentDiscardLevel;

    //explicitly unbind texture
    gGL.getTexUnit(0)->unbind(mBindTarget);
    llverify(gGL.getTexUnit(0)->bindTexture(mBindTarget, this)); // [Viewer Lab] binding by id

    // [Viewer Lab] The image's texture of the viewer's device, read once after the bind, and its description's values for
    // the level parameters when GL is not the backend; 0 without it
    LL::RHI::Texture* rhi = getRHITexture();
    const bool rhi_level = rhi && U32(gl_discard) < rhi->desc().mipLevels;
    const S32 rhi_width = rhi_level ? S32(llmax(rhi->desc().width >> gl_discard, 1u)) : 0;
    const S32 rhi_compressed = rhi_level && LL::RHI::isCompressed(rhi->desc().format) ? 1 : 0;

    //debug code, leave it there commented.
    //checkTexSize() ;

    LLGLint glwidth = gl_tex_level_parameter(mTarget, gl_discard, GL_TEXTURE_WIDTH, rhi_width); // [Viewer Lab]
    if (glwidth == 0)
    {
        // No mip data smaller than current discard level
        return false;
    }

    S32 width = getWidth(discard_level);
    S32 height = getHeight(discard_level);
    S32 ncomponents = getComponents();
    if (ncomponents == 0)
    {
        return false;
    }
    if(width < glwidth)
    {
        LL_WARNS() << "texture size is smaller than it should be." << LL_ENDL ;
        LL_WARNS() << "width: " << width << " glwidth: " << glwidth << " mWidth: " << mWidth <<
            " mCurrentDiscardLevel: " << (S32)mCurrentDiscardLevel << " discard_level: " << (S32)discard_level << LL_ENDL ;
        return false ;
    }

    if (width <= 0 || width > 2048 || height <= 0 || height > 2048 || ncomponents < 1 || ncomponents > 4)
    {
        LL_ERRS() << llformat("LLImageGL::readBackRaw: bogus params: %d x %d x %d",width,height,ncomponents) << LL_ENDL;
    }

    LLGLint is_compressed = 0;
    if (compressed_ok)
    {
        is_compressed = gl_tex_level_parameter(mTarget, gl_discard, GL_TEXTURE_COMPRESSED, rhi_compressed); // [Viewer Lab]
    }

    //-----------------------------------------------------------------------------------------------
    GLenum error ;
    while((error = gl_get_error()) != GL_NO_ERROR) // [Viewer Lab]
    {
        LL_WARNS() << "GL Error happens before reading back texture. Error code: " << error << LL_ENDL ;
    }
    //-----------------------------------------------------------------------------------------------

    LLImageDataLock lock(imageraw);

    if (is_compressed)
    {
        LLGLint glbytes = gl_tex_level_parameter(mTarget, gl_discard, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, 0); // [Viewer Lab]
        if(!imageraw->allocateDataSize(width, height, ncomponents, glbytes))
        {
            constexpr S64 MAX_GL_BYTES = 2048 * 2048;
            if (glbytes > 0 && glbytes <= MAX_GL_BYTES)
            {
                LLError::LLUserWarningMsg::showOutOfMemory();
                LL_ERRS() << "Memory allocation failed for reading back texture. Data size: " << glbytes << LL_ENDL;
            }
            else
            {
                LL_WARNS() << "Memory allocation failed for reading back texture. Data size is: " << glbytes << LL_ENDL;
                LL_WARNS() << "width: " << width << "height: " << height << "components: " << ncomponents << LL_ENDL;
            }
            return false ;
        }

        // [Viewer Lab] Coverage marker, once per session
        static std::atomic<bool> sReached{ false };
        if (!sReached.exchange(true))
        {
            LL_INFOS("RHI") << "Transfer site reached: readBackRaw compressed" << LL_ENDL;
        }
        LLGPUReadback readback; // [Viewer Lab] GL only, counted untracked
        readback.readCompressedTexture(mTarget, gl_discard, imageraw->getData());
        //stop_glerror();
    }
    else
    {
        if(!imageraw->allocateDataSize(width, height, ncomponents))
        {
            constexpr F32 MAX_IMAGE_SIZE = 2048 * 2048;
            F32 size = (F32)width * (F32)height * (F32)ncomponents;
            if (size > 0 && size <= MAX_IMAGE_SIZE)
            {
                LLError::LLUserWarningMsg::showOutOfMemory();
                LL_ERRS() << "Memory allocation failed for reading back texture. Data size: " << size << LL_ENDL;
            }
            else
            {
                LL_WARNS() << "Memory allocation failed for reading back texture." << LL_ENDL;
                LL_WARNS() << "width: " << width << "height: " << height << "components: " << ncomponents << LL_ENDL;
            }
            return false ;
        }

        // [Viewer Lab] Coverage marker, once per session
        static std::atomic<bool> sReached{ false };
        if (!sReached.exchange(true))
        {
            LL_INFOS("RHI") << "Transfer site reached: readBackRaw" << LL_ENDL;
        }
        // [Viewer Lab] Through the readback facade, with readBackRaw's own size, so a stale description is counted outside
        //; off GL a read that did not reach the encoder reads nothing: false, as after a GL error
        if (!LLGPUReadback::readTexImage(rhi, GL_TEXTURE_2D, gl_discard, width, height, mFormatPrimary, mFormatType,
                                         imageraw->getData(), size_t(imageraw->getDataSize())))
        {
            imageraw->deleteData();
            return false;
        }
        //stop_glerror();
    }

    //-----------------------------------------------------------------------------------------------
    if((error = gl_get_error()) != GL_NO_ERROR) // [Viewer Lab]
    {
        LL_WARNS() << "GL Error happens after reading back texture. Error code: " << error << LL_ENDL ;
        imageraw->deleteData() ;

        while((error = gl_get_error()) != GL_NO_ERROR) // [Viewer Lab]
        {
            LL_WARNS() << "GL Error happens after reading back texture. Error code: " << error << LL_ENDL ;
        }

        return false ;
    }
    //-----------------------------------------------------------------------------------------------

    return true ;
}

void LLImageGL::destroyGLTexture()
{
    checkActiveThread();
    // [Viewer Lab] Whether there was a texture on the current backend; off GL the own texture without a GL name, which
    // dropReplacedRHITexture frees. On GL it is mTexName != 0
    const bool had_texture = hasTexture();
    dropReplacedRHITexture(); // [Viewer Lab]
    mInPlacePending = false; // [Viewer Lab] no stale in-place update is counted

    if (had_texture) // [Viewer Lab]
    {
        if(mTextureMemory != S64Bytes(0))
        {
            mTextureMemory = (S64Bytes)0;
        }

        if (mTexName != 0) // [Viewer Lab] the GL name, never 0 on GL
        {
            const LLGLuint previous_name = mTexName; // [Viewer Lab]
            releaseTexName(mTexName); // [Viewer Lab]
            mTexName = 0;
            updateTextureKey(previous_name, nullptr); // [Viewer Lab] 0
        }
        mCurrentDiscardLevel = -1 ; //invalidate mCurrentDiscardLevel.
        mGLTextureCreated = false ;
    }
}

//force to invalidate the gl texture, most likely a sculpty texture
void LLImageGL::forceToInvalidateGLTexture()
{
    checkActiveThread();
    if (hasTexture()) // [Viewer Lab] any backend: off GL a texture has no GL name
    {
        destroyGLTexture();
    }
    else
    {
        mCurrentDiscardLevel = -1 ; //invalidate mCurrentDiscardLevel.
    }
}

//----------------------------------------------------------------------------

void LLImageGL::setAddressMode(LLTexUnit::eTextureAddressMode mode)
{
    if (mAddressMode != mode)
    {
        mTexOptionsDirty = true;
        mAddressMode = mode;
    }

    // [Viewer Lab] Bound now by LLTexUnit's GL test (its GL name), and the mode reaches this image's device texture
    // too, whatever the unit's record is
    LLTexUnit* unit = gGL.getTexUnit(gGL.getCurrentTexUnitIndex());
    if (unit->isBound(this))
    {
        unit->setTextureAddressMode(mode, LLTexUnit::textureOf(this, mTexName));
        mTexOptionsDirty = false;
    }
}

void LLImageGL::setFilteringOption(LLTexUnit::eTextureFilterOptions option)
{
    if (mFilterOption != option)
    {
        mTexOptionsDirty = true;
        mFilterOption = option;
    }

    // [Viewer Lab] As setAddressMode; an image without a GL name only with a device texture
    LLTexUnit* unit = gGL.getTexUnit(gGL.getCurrentTexUnitIndex());
    if ((mTexName != 0 || (on_main_thread() && mRHITexture.notNull())) && unit->isBound(this))
    {
        unit->setTextureFilteringOption(option, LLTexUnit::textureOf(this, mTexName));
        mTexOptionsDirty = false;
        stop_glerror();
    }
}

bool LLImageGL::getIsResident(bool test_now)
{
    if (test_now)
    {
        if (mTexName != 0)
        {
            glAreTexturesResident(1, (GLuint*)&mTexName, &mIsResident);
        }
        else
        {
            mIsResident = false;
        }
    }

    return mIsResident;
}

S32 LLImageGL::getHeight(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 height = mHeight >> discard_level;
    if (height < 1) height = 1;
    return height;
}

S32 LLImageGL::getWidth(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 width = mWidth >> discard_level;
    if (width < 1) width = 1;
    return width;
}

S64 LLImageGL::getBytes(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 w = mWidth>>discard_level;
    S32 h = mHeight>>discard_level;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    return dataFormatBytes(mFormatPrimary, w, h);
}

S64 LLImageGL::getMipBytes(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 w = mWidth>>discard_level;
    S32 h = mHeight>>discard_level;
    S64 res = dataFormatBytes(mFormatPrimary, w, h);
    if (mUseMipMaps)
    {
        while (w > 1 && h > 1)
        {
            w >>= 1; if (w == 0) w = 1;
            h >>= 1; if (h == 0) h = 1;
            res += dataFormatBytes(mFormatPrimary, w, h);
        }
    }
    return res;
}

bool LLImageGL::isJustBound() const
{
    return sLastFrameTime - mLastBindTime < 0.5f;
}

bool LLImageGL::getBoundRecently() const
{
    return (bool)(sLastFrameTime - mLastBindTime < MIN_TEXTURE_LIFETIME);
}

bool LLImageGL::getIsAlphaMask() const
{
    llassert_always(!sSkipAnalyzeAlpha);
    return mIsMask;
}

void LLImageGL::setTarget(const LLGLenum target, const LLTexUnit::eTextureType bind_target)
{
    mTarget = target;
    mBindTarget = bind_target;
}

const S8 INVALID_OFFSET = -99 ;
void LLImageGL::setNeedsAlphaAndPickMask(bool need_mask)
{
    if(mNeedsAlphaAndPickMask != need_mask)
    {
        mNeedsAlphaAndPickMask = need_mask;

        if(mNeedsAlphaAndPickMask)
        {
            mAlphaOffset = 0 ;
        }
        else //do not need alpha mask
        {
            mAlphaOffset = INVALID_OFFSET ;
            mIsMask = false;
        }
    }
}

void LLImageGL::calcAlphaChannelOffsetAndStride()
{
    if(mAlphaOffset == INVALID_OFFSET)//do not need alpha mask
    {
        return ;
    }

    mAlphaStride = -1 ;
    switch (mFormatPrimary)
    {
    case GL_LUMINANCE:
    case GL_ALPHA:
        mAlphaStride = 1;
        break;
    case GL_LUMINANCE_ALPHA:
        mAlphaStride = 2;
        break;
    case GL_RED:
    case GL_RGB:
    case GL_SRGB:
        mNeedsAlphaAndPickMask = false;
        mIsMask = false;
        return; //no alpha channel.
    case GL_RGBA:
    case GL_SRGB_ALPHA:
        mAlphaStride = 4;
        break;
    case GL_BGRA_EXT:
        mAlphaStride = 4;
        break;
    default:
        break;
    }

    mAlphaOffset = -1 ;
    if (mFormatType == GL_UNSIGNED_BYTE)
    {
        mAlphaOffset = mAlphaStride - 1 ;
    }
    else if(is_little_endian())
    {
        if (mFormatType == GL_UNSIGNED_INT_8_8_8_8)
        {
            mAlphaOffset = 0 ;
        }
        else if (mFormatType == GL_UNSIGNED_INT_8_8_8_8_REV)
        {
            mAlphaOffset = 3 ;
        }
    }
    else //big endian
    {
        if (mFormatType == GL_UNSIGNED_INT_8_8_8_8)
        {
            mAlphaOffset = 3 ;
        }
        else if (mFormatType == GL_UNSIGNED_INT_8_8_8_8_REV)
        {
            mAlphaOffset = 0 ;
        }
    }

    if( mAlphaStride < 1 || //unsupported format
        mAlphaOffset < 0 || //unsupported type
        (mFormatPrimary == GL_BGRA_EXT && mFormatType != GL_UNSIGNED_BYTE)) //unknown situation
    {
        LL_WARNS() << "Cannot analyze alpha for image with format type " << std::hex << mFormatType << std::dec << LL_ENDL;

        mNeedsAlphaAndPickMask = false ;
        mIsMask = false;
    }
}

void LLImageGL::analyzeAlpha(const void* data_in, U32 w, U32 h)
{
    if(!data_in || sSkipAnalyzeAlpha || !mNeedsAlphaAndPickMask)
    {
        return ;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    U32 length = w * h;
    U32 alphatotal = 0;

    U32 sample[16];
    memset(sample, 0, sizeof(U32)*16);

    // generate histogram of quantized alpha.
    // also add-in the histogram of a 2x2 box-sampled version.  The idea is
    // this will mid-skew the data (and thus increase the chances of not
    // being used as a mask) from high-frequency alpha maps which
    // suffer the worst from aliasing when used as alpha masks.
    if (w >= 2 && h >= 2)
    {
        llassert(w % 2 == 0);
        llassert(h % 2 == 0);
        const GLubyte* rowstart = ((const GLubyte*) data_in) + mAlphaOffset;
        for (U32 y = 0; y < h; y += 2)
        {
            const GLubyte* current = rowstart;
            for (U32 x = 0; x < w; x += 2)
            {
                const U32 s1 = current[0];
                alphatotal += s1;
                const U32 s2 = current[w * mAlphaStride];
                alphatotal += s2;
                current += mAlphaStride;
                const U32 s3 = current[0];
                alphatotal += s3;
                const U32 s4 = current[w * mAlphaStride];
                alphatotal += s4;
                current += mAlphaStride;

                ++sample[s1/16];
                ++sample[s2/16];
                ++sample[s3/16];
                ++sample[s4/16];

                const U32 asum = (s1+s2+s3+s4);
                alphatotal += asum;
                sample[asum/(16*4)] += 4;
            }

            rowstart += 2 * w * mAlphaStride;
        }
        length *= 2; // we sampled everything twice, essentially
    }
    else
    {
        const GLubyte* current = ((const GLubyte*) data_in) + mAlphaOffset;
        for (U32 i = 0; i < length; i++)
        {
            const U32 s1 = *current;
            alphatotal += s1;
            ++sample[s1/16];
            current += mAlphaStride;
        }
    }

    // if more than 1/16th of alpha samples are mid-range, this
    // shouldn't be treated as a 1-bit mask

    // also, if all of the alpha samples are clumped on one half
    // of the range (but not at an absolute extreme), then consider
    // this to be an intentional effect and don't treat as a mask.

    U32 midrangetotal = 0;
    for (U32 i = 2; i < 13; i++)
    {
        midrangetotal += sample[i];
    }
    U32 lowerhalftotal = 0;
    for (U32 i = 0; i < 8; i++)
    {
        lowerhalftotal += sample[i];
    }
    U32 upperhalftotal = 0;
    for (U32 i = 8; i < 16; i++)
    {
        upperhalftotal += sample[i];
    }

    if (midrangetotal > length/48 || // lots of midrange, or
        (lowerhalftotal == length && alphatotal != 0) || // all close to transparent but not all totally transparent, or
        (upperhalftotal == length && alphatotal != 255*length)) // all close to opaque but not all totally opaque
    {
        mIsMask = false; // not suitable for masking
    }
    else
    {
        mIsMask = true;
    }
}

//----------------------------------------------------------------------------
U32 LLImageGL::createPickMask(S32 pWidth, S32 pHeight)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    freePickMask();
    U32 pick_width = pWidth/2 + 1;
    U32 pick_height = pHeight/2 + 1;

    U32 size = pick_width * pick_height;
    size = (size + 7) / 8; // pixelcount-to-bits
    mPickMask = new U8[size];
    mPickMaskWidth = pick_width - 1;
    mPickMaskHeight = pick_height - 1;

    memset(mPickMask, 0, sizeof(U8) * size);

    return size;
}

//----------------------------------------------------------------------------
void LLImageGL::freePickMask()
{
    if (mPickMask != NULL)
    {
        delete [] mPickMask;
    }
    mPickMask = NULL;
    mPickMaskWidth = mPickMaskHeight = 0;
}

bool LLImageGL::isCompressed()
{
    llassert(mFormatPrimary != 0);
    return is_compressed_format(mFormatPrimary); // [Viewer Lab]
}

//----------------------------------------------------------------------------
void LLImageGL::updatePickMask(S32 width, S32 height, const U8* data_in)
{
    if(!mNeedsAlphaAndPickMask)
    {
        return ;
    }

    if (mFormatType != GL_UNSIGNED_BYTE ||
        ((mFormatPrimary != GL_RGBA)
      && (mFormatPrimary != GL_SRGB_ALPHA)))
    {
        //cannot generate a pick mask for this texture
        freePickMask();
        return;
    }


#ifdef SHOW_ASSERT
    const U32 pickSize = createPickMask(width, height);
#else // SHOW_ASSERT
    createPickMask(width, height);
#endif // SHOW_ASSERT

    U32 pick_bit = 0;

    for (S32 y = 0; y < height; y += 2)
    {
        for (S32 x = 0; x < width; x += 2)
        {
            U8 alpha = data_in[(y*width+x)*4+3];

            if (alpha > 32)
            {
                U32 pick_idx = pick_bit/8;
                U32 pick_offset = pick_bit%8;
                llassert(pick_idx < pickSize);

                mPickMask[pick_idx] |= 1 << pick_offset;
            }

            ++pick_bit;
        }
    }
}

//bool LLImageGL::getMask(const LLVector2 &tc)
// [RLVa:KB] - Checked: RLVa-2.2 (@setoverlay)
bool LLImageGL::getMask(const LLVector2 &tc) const
// [/RLVa:KB]
{
    bool res = true;

    if (mPickMask)
    {
        F32 u,v;
        if (LL_LIKELY(tc.isFinite()))
        {
            u = tc.mV[0] - floorf(tc.mV[0]);
            v = tc.mV[1] - floorf(tc.mV[1]);
        }
        else
        {
            LL_WARNS_ONCE("render") << "Ugh, non-finite u/v in mask pick" << LL_ENDL;
            u = v = 0.f;
            // removing assert per EXT-4388
            // llassert(false);
        }

        if (LL_UNLIKELY(u < 0.f || u > 1.f ||
                v < 0.f || v > 1.f))
        {
            LL_WARNS_ONCE("render") << "Ugh, u/v out of range in image mask pick" << LL_ENDL;
            u = v = 0.f;
            // removing assert per EXT-4388
            // llassert(false);
        }

        S32 x = llfloor(u * mPickMaskWidth);
        S32 y = llfloor(v * mPickMaskHeight);

        if (LL_UNLIKELY(x > mPickMaskWidth))
        {
            LL_WARNS_ONCE("render") << "Ooh, width overrun on pick mask read, that coulda been bad." << LL_ENDL;
            x = llmax((U16)0, mPickMaskWidth);
        }
        if (LL_UNLIKELY(y > mPickMaskHeight))
        {
            LL_WARNS_ONCE("render") << "Ooh, height overrun on pick mask read, that woulda been bad." << LL_ENDL;
            y = llmax((U16)0, mPickMaskHeight);
        }

        S32 idx = y*mPickMaskWidth+x;
        S32 offset = idx%8;

        res = (mPickMask[idx/8] & (1 << offset)) != 0;
    }

    return res;
}

void LLImageGL::setCurTexSizebar(S32 index, bool set_pick_size)
{
    sCurTexSizeBar = index ;

    if(set_pick_size)
    {
        sCurTexPickSize = (1 << index) ;
    }
    else
    {
        sCurTexPickSize = -1 ;
    }
}
void LLImageGL::resetCurTexSizebar()
{
    sCurTexSizeBar = -1 ;
    sCurTexPickSize = -1 ;
}

bool LLImageGL::scaleDown(S32 desired_discard, LLVertexBuffer* screen_triangle)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    if (mTarget != GL_TEXTURE_2D
        || mFormatInternal == -1 // not initialized
        )
    {
        return false;
    }

    desired_discard = llmin(desired_discard, mMaxDiscardLevel);

    if (desired_discard <= mCurrentDiscardLevel)
    {
        return false;
    }

    S32 mip = desired_discard - mCurrentDiscardLevel;

    S32 desired_width = getWidth(desired_discard);
    S32 desired_height = getHeight(desired_discard);

    if (gGLManager.mDownScaleMethod == 0 || !pixel_buffer_objects()) // [Viewer Lab] the PBO capability
    { // use an FBO to downscale the texture
        // [Viewer Lab] Before anything changes: whether the copy into the re-specified texture can be made.
        // Metal copies inside the bound target and between plain colour formats only, so BC and sRGB images and sizes
        // over the 1024 target stay as they are there; on GL it holds as before
        const LL::RHI::Texture* current = getRHITexture();
        if (!LLGPUCopy::canFromFramebuffer(current ? current->desc().format : LL::RHI::Format::Unknown, desired_width,
                                           desired_height))
        {
            ++sScaleDownsRefused;
            // One line for every format and size, so ONCE means once
            LL_WARNS_ONCE("LLImageGL") << "scaleDown: the copy from the bound target into the smaller texture cannot be "
                                       << "made (its format or size); the texture stays as it is (P3-08)" << LL_ENDL;
            return false;
        }
        gGL.setViewport(0, 0, desired_width, desired_height); // [Viewer Lab] the viewport through gGL

        // draw a full screen triangle
        if (gGL.getTexUnit(0)->bind(this, true, true))
        {
            // [Viewer Lab] Through the caller's vertex buffer, the viewer's normal draw (census, encoder, and on GL the same
            // glDrawArrays); without one GL's glDrawArrays as before. Not delivered (no triangle off GL, a draw skipped or
            // left raw off GL): false before the texture changes, since the copy would read the target's old image
            const bool delivered = screen_triangle ? screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3)
                                                   : gl_draw_arrays(GL_TRIANGLES, 0, 3); // [Viewer Lab]
            if (!delivered)
            {
                ++sScaleDownsRefused;
                if (screen_triangle)
                {
                    LL_WARNS_ONCE("LLImageGL") << "scaleDown: the screen triangle's draw was not delivered; the texture "
                                               << "stays as it is" << LL_ENDL;
                }
                else
                {
                    LL_WARNS_ONCE("LLImageGL") << "scaleDown needs the caller's screen triangle off GL" << LL_ENDL;
                }
                return false;
            }
            // [Viewer Lab] Coverage marker, once per session, after a delivered draw (m-3)
            static std::atomic<bool> sReached{ false };
            if (!sReached.exchange(true))
            {
                LL_INFOS("RHI") << "Transfer site reached: scaleDown FBO" << LL_ENDL;
            }

#if LL_DARWIN
            // On mac OS, flush before freeing, otherwise the texture may be
            // freed before Metal's 'lazy' evaluation/restoration behavior triggers.
            gl_flush(); // [Viewer Lab]
#endif

            // [Viewer Lab] Off GL the texture has no GL name, and its re-specification comes before the free, so a
            // refusal changes nothing (the accounting's entry moves to the new id with it); on GL after the free as
            // before (m-4)
            const bool named = !has_no_gl_name(mRHITexture, mTexName);
            LL::RHI::Texture* rhi = nullptr;
            if (!named)
            {
                rhi = respecifyForScale(desired_discard);
                if (!rhi)
                {
                    ++sScaleDownsRefused;
                    LL_WARNS_ONCE("LLImageGL") << "scaleDown: the device refused the re-specification; the texture stays "
                                               << "as it is" << LL_ENDL;
                    return false;
                }
            }
            free_own_tex_image(mTexName, mRHITexture); // [Viewer Lab]
            if (named)
            {
                rhi = respecifyForScale(desired_discard); // [Viewer Lab]
            }
            gl_tex_image_2d(mTarget, 0, mFormatInternal, desired_width, desired_height, mFormatPrimary, mFormatType, nullptr); // [Viewer Lab]
            if (!LLGPUCopy::fromFramebuffer(rhi, mTarget, 0, 0, 0, 0, 0, 0, desired_width, desired_height)) // [Viewer Lab]
            {
                // [Viewer Lab] Off GL nothing was copied; the image keeps its new size, as after GL's
                // failed copy, and the callers ignore scaleDown's result. canFromFramebuffer has refused the copies the
                // encoder refuses, so this is a copy refused for another reason
                LL_WARNS_ONCE("LLImageGL") << "Failed to copy texture for downscaling." << LL_ENDL;
            }
            alloc_own_tex_image(mTexName, mRHITexture, desired_width, desired_height, mFormatInternal); // [Viewer Lab]

            mTexOptionsDirty = true;

            if (mHasMipMaps)
            { // generate mipmaps if needed
                LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("scaleDown - glGenerateMipmap");
                gGL.getTexUnit(0)->bind(this);
                LLImageGL::generateMipmaps(rhi && LL::RHI::validateMipmaps(rhi->desc()).empty() ? rhi : nullptr, mTarget); // [Viewer Lab]
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            }
        }
        else
        {
            LL_WARNS_ONCE("LLImageGL") << "Failed to bind texture for downscaling." << LL_ENDL;
            return false;
        }
    }
    else
    { // use a PBO to downscale the texture
        // [Viewer Lab] Coverage marker, once per session
        static std::atomic<bool> sReached{ false };
        if (!sReached.exchange(true))
        {
            LL_INFOS("RHI") << "Transfer site reached: scaleDown PBO" << LL_ENDL;
        }
        U64 size = getBytes(desired_discard);
        llassert(size <= 2048 * 2048 * 4); // we shouldn't be using this method to downscale huge textures, but it'll work
        gGL.getTexUnit(0)->bind(this, false, true);

        if (sScratchPBO == 0)
        {
            gl_gen_scratch_pbo(sScratchPBO); // [Viewer Lab]
            sScratchPBOSize = 0;
        }

        gl_bind_buffer(GL_PIXEL_PACK_BUFFER, sScratchPBO); // [Viewer Lab]

        if (size > sScratchPBOSize)
        {
            gl_buffer_data(GL_PIXEL_PACK_BUFFER, size, GL_STREAM_COPY); // [Viewer Lab]
            sScratchPBOSize = (U32)size;
        }

        gl_get_tex_image(mTarget, mip, mFormatPrimary, mFormatType, nullptr); // [Viewer Lab]

        free_own_tex_image(mTexName, mRHITexture); // [Viewer Lab]

        gl_bind_buffer(GL_PIXEL_PACK_BUFFER, 0); // [Viewer Lab]

        gl_bind_buffer(GL_PIXEL_UNPACK_BUFFER, sScratchPBO); // [Viewer Lab]
        LL::RHI::Texture* rhi = respecifyForScale(desired_discard); // [Viewer Lab] no copy reaches the device
        gl_tex_image_2d(mTarget, 0, mFormatInternal, desired_width, desired_height, mFormatPrimary, mFormatType, nullptr); // [Viewer Lab]
        gl_bind_buffer(GL_PIXEL_UNPACK_BUFFER, 0); // [Viewer Lab]

        alloc_own_tex_image(mTexName, mRHITexture, desired_width, desired_height, mFormatInternal); // [Viewer Lab]

        if (mHasMipMaps)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("scaleDown - glGenerateMipmap");
            LLImageGL::generateMipmaps(rhi && LL::RHI::validateMipmaps(rhi->desc()).empty() ? rhi : nullptr, mTarget); // [Viewer Lab]
        }

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    mCurrentDiscardLevel = desired_discard;
    ++sScaleDowns; // [Viewer Lab]

    return true;
}


//----------------------------------------------------------------------------
#if LL_IMAGEGL_THREAD_CHECK
void LLImageGL::checkActiveThread()
{
    llassert(mActiveThread == LLThread::currentID());
}
#endif

//----------------------------------------------------------------------------


// Manual Mip Generation
/*
        S32 width = getWidth(discard_level);
        S32 height = getHeight(discard_level);
        S32 w = width, h = height;
        S32 nummips = 1;
        while (w > 4 && h > 4)
        {
            w >>= 1; h >>= 1;
            nummips++;
        }
        stop_glerror();
        w = width, h = height;
        const U8* prev_mip_data = 0;
        const U8* cur_mip_data = 0;
        for (int m=0; m<nummips; m++)
        {
            if (m==0)
            {
                cur_mip_data = rawdata;
            }
            else
            {
                S32 bytes = w * h * mComponents;
                U8* new_data = new U8[bytes];
                LLImageBase::generateMip(prev_mip_data, new_data, w, h, mComponents);
                cur_mip_data = new_data;
            }
            llassert(w > 0 && h > 0 && cur_mip_data);
            U8 test = cur_mip_data[w*h*mComponents-1];
            {
                LLImageGL::setManualImage(mTarget, m, mFormatInternal, w, h, mFormatPrimary, mFormatType, cur_mip_data);
                stop_glerror();
            }
            if (prev_mip_data && prev_mip_data != rawdata)
            {
                delete prev_mip_data;
            }
            prev_mip_data = cur_mip_data;
            w >>= 1;
            h >>= 1;
        }
        if (prev_mip_data && prev_mip_data != rawdata)
        {
            delete prev_mip_data;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  nummips);
*/

// [Viewer Lab] LLGPUReadback and LLGPUCopy ------------------------------------------------

namespace
{
    // The calls of one facade since the process began, any thread.
    struct TransferCounts
    {
        std::atomic<U64> tracked{ 0 };
        std::atomic<U64> untracked{ 0 };
        std::atomic<U64> outside{ 0 };
        std::atomic<U64> refused{ 0 };
        std::atomic<U64> mismatched{ 0 }; ///< the transfer operand check's mismatches

        LLGPUReadback::Counts read() const
        {
            LLGPUReadback::Counts counts;
            counts.tracked = tracked.load();
            counts.untracked = untracked.load();
            counts.outside = outside.load();
            counts.refused = refused.load();
            counts.mismatched = mismatched.load();
            return counts;
        }
    };

    TransferCounts sReadbackCounts;
    TransferCounts sCopyCounts;

    // The readback facade's calls of this thread, as sReadbackCounts counts them.
    thread_local LLGPUReadback::Counts sThreadReadbackCounts;
    // This thread's innermost CountScope's counts, or null, and how many OwnReads it holds.
    thread_local LLGPUReadback::Counts* sScopeCounts = nullptr;
    thread_local U32 sOwnReads = 0;

    // One call of a facade: its count, and for the readback facade this thread's too, and its scope's when it is a call's
    // own read.
    void count_call(TransferCounts& counts, std::atomic<U64> TransferCounts::*total, U64 LLGPUReadback::Counts::*own)
    {
        ++(counts.*total);
        if (&counts == &sReadbackCounts)
        {
            ++(sThreadReadbackCounts.*own);
            if (sScopeCounts && sOwnReads)
            {
                ++(sScopeCounts->*own);
            }
        }
    }

    // A texture a transfer reads or writes, with the level, layer and region the encoder gets.
    struct TransferTexture
    {
        LL::RHI::Texture* texture;
        U32 level;
        U32 layer;
        S32 x;
        S32 y;
        S32 width;
        S32 height;
    };
}

// [Viewer Lab] The device a transfer is recorded on: the viewer's, on the main thread only; null otherwise
static LL::RHI::Device* transfer_device()
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    return device && on_main_thread() ? device : nullptr;
}

// [Viewer Lab] The texture GL reads from the current framebuffer: the bound LLRenderTarget's colour attachment 0 or depth,
// which bindTarget sets as GL's read buffer, else the window's back buffer or depth; null for a target whose attachment is
// a raw name
static LL::RHI::Texture* framebuffer_source(LL::RHI::Device* device, bool depth)
{
    if (LLRenderTarget* target = LLRenderTarget::getCurrentBoundTarget())
    {
        return depth ? target->getRHIDepth() : target->getRHITexture(0);
    }
    LL::RHI::Surface* surface = device->surface();
    if (!surface)
    {
        return nullptr;
    }
    return depth ? surface->backBufferDepth() : surface->backBuffer();
}

// [Viewer Lab] The readback layout of a GL format and type: the one table's, except GL_ALPHA, GL_LUMINANCE and
// GL_LUMINANCE_ALPHA, which are upload layouts only
static std::optional<LL::RHI::PixelLayout> readback_layout(U32 format, U32 type)
{
    if (format == GL_ALPHA || format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA)
    {
        return std::nullopt;
    }
    return LL::RHI::pixelLayoutFromGL(format, type);
}

// [Viewer Lab] The one record path of LLGPUReadback and LLGPUCopy: with the device on
// the main thread, textures of the device and a region inside each of them, send(encoder) makes the encoder's call, inside
// LLImageGL::OwnGLTransfers when the facade makes the GL call itself, and the call counts tracked when the tracker's
// transfers() went up across it and send gave something, refused otherwise; without them nothing is recorded and the call
// counts untracked or outside. Returns whether it was tracked. The GL call comes after it, in the caller.
template <typename Send>
static bool record(TransferCounts& counts, LL::RHI::Device* device, std::initializer_list<TransferTexture> textures,
                   bool layout, bool own_gl, Send&& send)
{
    if (!device || !layout)
    {
        count_call(counts, &TransferCounts::untracked, &LLGPUReadback::Counts::untracked);
        return false;
    }
    for (const TransferTexture& t : textures)
    {
        if (!t.texture)
        {
            count_call(counts, &TransferCounts::untracked, &LLGPUReadback::Counts::untracked);
            return false;
        }
    }
    for (const TransferTexture& t : textures)
    {
        if (t.x < 0 || t.y < 0 || t.width <= 0 || t.height <= 0
            || !LL::RHI::validateRegion(t.texture->desc(), t.level, t.layer,
                                        LL::RHI::Region{ U32(t.x), U32(t.y), U32(t.width), U32(t.height) })
                    .empty())
        {
            count_call(counts, &TransferCounts::outside, &LLGPUReadback::Counts::outside);
            return false;
        }
    }
    LL::RHI::Encoder& encoder = device->encoder();
    const U64 before = encoder.state().transfers();
    bool given = false;
    if (own_gl)
    {
        LLImageGL::OwnGLTransfers own;
        given = send(encoder);
    }
    else
    {
        given = send(encoder);
    }
    const bool tracked = given && encoder.state().transfers() > before;
    if (tracked)
    {
        count_call(counts, &TransferCounts::tracked, &LLGPUReadback::Counts::tracked);
    }
    else
    {
        count_call(counts, &TransferCounts::refused, &LLGPUReadback::Counts::refused);
    }
    return tracked;
}

// [Viewer Lab] Whether a facade's call delivered: its encoder tracked it, or the GL backend function made
// the call (on GL always, whatever record() found; false on any other backend). The facade combines the two reports and
// does not ask which backend runs. A readback that delivered nothing gets no rows, so its data() is empty
// and wait() copies nothing.
static bool transfer_delivered(bool tracked, bool made, U32* rows = nullptr)
{
    const bool delivered = tracked || made;
    if (!delivered && rows)
    {
        *rows = 0;
    }
    return delivered;
}

LLGPUReadback::LLGPUReadback() = default;

LLGPUReadback::~LLGPUReadback() = default;

LLGPUReadback::LLGPUReadback(LLGPUReadback&& other) noexcept
:   mReadback(std::move(other.mReadback)),
    mDestination(other.mDestination),
    mBytes(other.mBytes),
    mRowBytes(other.mRowBytes),
    mRows(other.mRows),
    mDelivered(other.mDelivered)
{
    other.mReadback = nullptr;
    other.mDestination = nullptr;
    other.mBytes = 0;
    other.mRowBytes = 0;
    other.mRows = 0;
    other.mDelivered = false;
}

LLGPUReadback& LLGPUReadback::operator=(LLGPUReadback&& other) noexcept
{
    if (this != &other)
    {
        mReadback = std::move(other.mReadback);
        other.mReadback = nullptr;
        mDestination = other.mDestination;
        mBytes = other.mBytes;
        mRowBytes = other.mRowBytes;
        mRows = other.mRows;
        mDelivered = other.mDelivered;
        other.mDestination = nullptr;
        other.mBytes = 0;
        other.mRowBytes = 0;
        other.mRows = 0;
        other.mDelivered = false;
    }
    return *this;
}

bool LLGPUReadback::readFramebuffer(S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* destination,
                                    size_t bytes)
{
    mReadback = nullptr;
    mDestination = static_cast<std::byte*>(destination);
    mBytes = bytes;
    const std::optional<LL::RHI::PixelLayout> layout = readback_layout(format, type);
    mRowBytes = layout && width > 0 ? U32(width) * layout->bytesPerPixel() : 0;
    mRows = height > 0 ? U32(height) : 0;
    LL::RHI::Device* device = transfer_device();
    LL::RHI::Texture* source = device ? framebuffer_source(device, format == GL_DEPTH_COMPONENT) : nullptr;
    const bool tracked = record(sReadbackCounts, device, { { source, 0, 0, x, y, width, height } }, layout.has_value(), true,
           [&](LL::RHI::Encoder& encoder)
           {
               mReadback = encoder.readback(source, 0, 0, LL::RHI::Region{ U32(x), U32(y), U32(width), U32(height) },
                                            *layout);
               return mReadback.notNull();
           });
    if (tracked)
    {
        static std::atomic<bool> sWarned{ false };
        check_transfer(sReadbackCounts.mismatched, sWarned, "LLGPUReadback::readFramebuffer", device, source,
                       format == GL_DEPTH_COMPONENT, nullptr); // [Viewer Lab] operand check
    }
    const bool made = gl_read_pixels(x, y, width, height, format, type, destination);
    mDelivered = made; // [Viewer Lab] at once on GL; wait() delivers another backend's read
    return transfer_delivered(tracked, made, &mRows);
}

bool LLGPUReadback::readTexture(LL::RHI::Texture* texture, U32 target, S32 level, S32 width, S32 height, U32 format,
                                U32 type, void* destination, size_t bytes)
{
    mReadback = nullptr;
    mDestination = static_cast<std::byte*>(destination);
    mBytes = bytes;
    const std::optional<LL::RHI::PixelLayout> layout = readback_layout(format, type);
    mRowBytes = layout && width > 0 ? U32(width) * layout->bytesPerPixel() : 0;
    mRows = height > 0 ? U32(height) : 0;
    // The whole level as the caller knows it, so a stale description shows as 'outside'.
    LL::RHI::Device* device = transfer_device();
    const bool tracked = record(sReadbackCounts, device, { { texture, U32(level), 0, 0, 0, width, height } },
                                layout.has_value(),
           true,
           [&](LL::RHI::Encoder& encoder)
           {
               mReadback = encoder.readback(texture, U32(level), 0, LL::RHI::Region{ 0, 0, U32(width), U32(height) },
                                            *layout);
               return mReadback.notNull();
           });
    if (tracked)
    {
        static std::atomic<bool> sWarned{ false };
        check_transfer(sReadbackCounts.mismatched, sWarned, "LLGPUReadback::readTexture", device, nullptr, false,
                       texture); // [Viewer Lab] operand check
    }
    const bool made = gl_get_tex_image(target, level, format, type, destination);
    mDelivered = made; // [Viewer Lab] at once on GL; wait() delivers another backend's read
    return transfer_delivered(tracked, made, &mRows);
}

void LLGPUReadback::readCompressedTexture(U32 target, S32 level, void* destination)
{
    mReadback = nullptr;
    mDestination = static_cast<std::byte*>(destination);
    mBytes = 0;
    mRowBytes = 0;
    mRows = 0;
    mDelivered = false;
    // The interface has no compressed readback.
    count_call(sReadbackCounts, &TransferCounts::untracked, &LLGPUReadback::Counts::untracked);
    gl_get_compressed_tex_image(target, level, destination);
}

bool LLGPUReadback::ready() const
{
    return mReadback.isNull() || mReadback->ready();
}

void LLGPUReadback::wait()
{
    if (mReadback.isNull())
    {
        return;
    }
    mReadback->wait();
    const std::span<const std::byte> data = mReadback->data();
    if (data.empty() || !mDestination)
    {
        // On GL the facade has read into the caller's memory already.
        return;
    }
    copyRows(mDestination, mBytes, data, mReadback->rowPitch(), mRowBytes, mRows);
    mDelivered = true;
}

// [Viewer Lab] wait()'s copy, apart for the tests
// static
size_t LLGPUReadback::copyRows(std::byte* destination, size_t bytes, std::span<const std::byte> data, size_t pitch,
                               U32 row_bytes, U32 rows)
{
    size_t written = 0;
    for (U32 row = 0; row < rows && written < bytes; ++row)
    {
        const size_t from = size_t(row) * pitch;
        if (from >= data.size())
        {
            break;
        }
        const size_t count = std::min({ size_t(row_bytes), data.size() - from, bytes - written });
        memcpy(destination + written, data.data() + from, count);
        written += count;
    }
    return written;
}

// [Viewer Lab] Tests only
LL::RHI::Readback* LLGPUReadback::readbackForTests() const
{
    return mReadback.get();
}

// [Viewer Lab] Tests only
void LLGPUReadback::adoptForTests(LL::RHI::Readback* readback, void* destination, size_t bytes, U32 row_bytes, U32 rows)
{
    mReadback = readback;
    mDestination = static_cast<std::byte*>(destination);
    mBytes = bytes;
    mRowBytes = row_bytes;
    mRows = rows;
    mDelivered = false;
}

std::span<const std::byte> LLGPUReadback::data() const
{
    return { mDestination, mDestination && mDelivered ? std::min(mBytes, size_t(mRows) * mRowBytes) : 0 };
}

size_t LLGPUReadback::rowPitch() const
{
    return mReadback.notNull() ? size_t(mReadback->rowPitch()) : 0;
}

// static
bool LLGPUReadback::readPixels(S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* destination, size_t bytes)
{
    LLGPUReadback readback;
    const bool delivered = readback.readFramebuffer(x, y, width, height, format, type, destination, bytes);
    readback.wait();
    return delivered;
}

// static
bool LLGPUReadback::readTexImage(LL::RHI::Texture* texture, U32 target, S32 level, S32 width, S32 height, U32 format,
                                 U32 type, void* destination, size_t bytes)
{
    LLGPUReadback readback;
    const bool delivered = readback.readTexture(texture, target, level, width, height, format, type, destination, bytes);
    readback.wait();
    return delivered;
}

// [Viewer Lab] One readback object a row, all recorded before the first wait: on GL each row's
// glReadPixels is made at once, as rawSnapshot made it, and the waits do nothing; on Metal the first wait sends the
// tile's copies in one command buffer and the others find theirs sent. The row objects are not pooled.
// static
bool LLGPUReadback::readFramebufferRows(S32 x, S32 y, S32 width, S32 rows, U32 format, U32 type, void* destination,
                                        size_t stride, size_t row_bytes)
{
    std::vector<LLGPUReadback> reads(rows > 0 ? size_t(rows) : 0);
    std::byte* const first = static_cast<std::byte*>(destination);
    bool delivered = true;
    for (size_t row = 0; row < reads.size(); ++row)
    {
        if (!reads[row].readFramebuffer(x, y + S32(row), width, 1, format, type, first + row * stride, row_bytes))
        {
            delivered = false;
        }
    }
    for (LLGPUReadback& read : reads)
    {
        read.wait();
    }
    return delivered;
}

// static
U32 LLGPUReadback::takeError()
{
    return gl_get_error();
}

// static
bool LLGPUReadback::targetsReadThroughTextures()
{
    return gl_reads_targets_through_textures();
}

// static
bool LLGPUReadback::readTextureAlpha(LL::RHI::Texture* texture, S32 width, S32 height, U8* scratch, size_t bytes,
                                     U8* alpha, size_t pixels, U32& error)
{
    const bool read = readTexImage(texture, LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, width, height, GL_RGBA,
                                   GL_UNSIGNED_BYTE, scratch, bytes);
    error = takeError();
    if (read && error == GL_NO_ERROR)
    {
        const size_t texels = std::min(pixels, bytes / 4); // never past scratch's bytes
        const U8* pixel = scratch;
        for (size_t i = 0; i < texels; ++i)
        {
            alpha[i] = pixel[3];
            pixel += 4;
        }
    }
    return read;
}

// static
LLGPUReadback::Counts LLGPUReadback::counts()
{
    return sReadbackCounts.read();
}

// static
U64 LLGPUReadback::transferMismatches()
{
    return sReadbackCounts.mismatched.load() + sCopyCounts.mismatched.load() + sUploadMismatches.load();
}

// static
LLGPUReadback::Counts LLGPUReadback::threadCounts()
{
    return sThreadReadbackCounts;
}

LLGPUReadback::CountScope::CountScope() : mOuter(sScopeCounts)
{
    sScopeCounts = &mCounts;
}

LLGPUReadback::CountScope::~CountScope()
{
    sScopeCounts = mOuter;
}

LLGPUReadback::OwnReads::OwnReads()
{
    ++sOwnReads;
}

LLGPUReadback::OwnReads::~OwnReads()
{
    --sOwnReads;
}

// static
bool LLGPUCopy::fromFramebuffer(LL::RHI::Texture* texture, U32 target, S32 level, S32 layer, S32 dstX, S32 dstY, S32 x,
                                S32 y, S32 width, S32 height, LL::RHI::Texture* source)
{
    LL::RHI::Device* device = transfer_device();
    LL::RHI::Texture* from = source ? source : (device ? framebuffer_source(device, false) : nullptr);
    const bool tracked = record(sCopyCounts, device,
           { { from, 0, 0, x, y, width, height }, { texture, U32(level), U32(layer), dstX, dstY, width, height } }, true,
           true,
           [&](LL::RHI::Encoder& encoder)
           {
               encoder.copyTexture(from, 0, 0, LL::RHI::Region{ U32(x), U32(y), U32(width), U32(height) }, texture,
                                   U32(level), U32(layer), U32(dstX), U32(dstY));
               return true;
           });
    if (tracked)
    {
        static std::atomic<bool> sWarned{ false };
        check_transfer(sCopyCounts.mismatched, sWarned, "LLGPUCopy::fromFramebuffer", device, from, false,
                       texture); // [Viewer Lab] operand check
    }
    bool made = false;
    if (target == GL_TEXTURE_CUBE_MAP_ARRAY)
    {
        made = gl_copy_tex_sub_image_3d(target, level, dstX, dstY, layer, x, y, width, height);
    }
    else
    {
        made = gl_copy_tex_sub_image_2d(target, level, dstX, dstY, x, y, width, height);
    }
    return transfer_delivered(tracked, made);
}

// [Viewer Lab] The conditions under which the encoder takes fromFramebuffer's copy of a width x height region at the origin
// into level 0 of a texture of format, asked before a caller changes anything: the bound framebuffer has a
// texture of the device, the region fits in it, and the device's copyRefusalBetween takes the pair of formats (GL takes
// every pair; Metal equal formats and plain colour ones, not sRGB, BC, depth or integer, and those only when its format
// copy program and pipeline can be had). Without the viewer's device on this thread, GL's default:
// yes. The same rules as record()'s region check and the Metal encoder's copyRefused, which asks the same rule, so
// scaleDown never re-specifies a texture the copy then leaves empty; a format of Unknown (a
// raw name) is only copied into where the device copies between any formats.
// static
bool LLGPUCopy::canFromFramebuffer(LL::RHI::Format format, S32 width, S32 height)
{
    LL::RHI::Device* device = transfer_device();
    if (!device)
    {
        return true;
    }
    const LL::RHI::Texture* from = framebuffer_source(device, false);
    if (!from || width <= 0 || height <= 0
        || !LL::RHI::validateRegion(from->desc(), 0, 0, LL::RHI::Region{ 0, 0, U32(width), U32(height) }).empty())
    {
        return false;
    }
    return device->copyRefusalBetween(from->desc().format, format).empty();
}

// static
LLGPUReadback::Counts LLGPUCopy::counts()
{
    return sCopyCounts.read();
}

LLImageGLThread::LLImageGLThread(LLWindow* window)
    // We want exactly one thread.
    : LL::ThreadPool("LLImageGL", 1)
    , mWindow(window)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    mFinished = false;

    // [Viewer Lab] A shared GL context only when GL is the backend: Metal's uploads go to the device's upload queue,
    // which needs no context, and a GL context there would be the GL dependency the Metal path is without
    mContext = gl_is_backend() ? mWindow->createSharedContext() : nullptr;

    // <FS:ND> If context creating is not supported (SDL1), mark texture thread disabled and exit
    if( !mContext && gl_is_backend() ) // [Viewer Lab] only GL needs the context
    {
        sEnabledTextures = false;
        mFinished = true;
        return;
    }
    // </FS:ND>

    LL::ThreadPool::start();
}

void LLImageGLThread::run()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // We must perform setup on this thread before actually servicing our
    // WorkQueue, likewise cleanup afterwards.
    // [Viewer Lab] The GL context only when the constructor made one; gGL is this thread's own (thread_local)
    if (mContext)
    {
        mWindow->makeContextCurrent(mContext);
    }
    gGL.init(false);
    LL_PROFILER_GPU_CONTEXT_NS("LLImageGL Context", 17);
    LL::ThreadPool::run();
    gGL.shutdown();
    if (mContext)
    {
        mWindow->destroySharedContext(mContext);
    }
}

