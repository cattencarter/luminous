/**
 * @file llrendertarget.cpp
 * @brief LLRenderTarget implementation
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

#include "llrendertarget.h"
#include "llrender.h"
#include "llgl.h"
#include "llrhi.h" // [Viewer Lab]
#include "llrhigl.h" // [Viewer Lab] GLNames
#include "llthread.h" // [Viewer Lab] on_main_thread

// [Viewer Lab] metalFacadeClear's declaration, with llrhi.cpp's guard: off Darwin LL_RHI_METAL is 0
#if LL_DARWIN
#include "llrhimetal.h"
#endif
#ifndef LL_RHI_METAL
#define LL_RHI_METAL 0
#endif

LLRenderTarget* LLRenderTarget::sBoundTarget = NULL;
U32 LLRenderTarget::sBytesAllocated = 0;

// [Viewer Lab] LLRenderTarget's GL backend functions below do nothing when GL is not the backend chosen at startup,
// as llimagegl.cpp's and llvertexbuffer.cpp's gl_is_backend.
static bool gl_is_backend()
{
    return LL::RHI::viewerBackend() == LL::RHI::BackendType::GL;
}

static LL::RHI::Device* stack_device(); // [Viewer Lab] below, with the stack's passes

// [Viewer Lab] LLRenderTarget's GL backend functions: each makes the GL call LLRenderTarget makes where it calls it, and
// nothing when GL is not the backend. bindTarget and flush may use them, or change them when passes use the target's own
// framebuffer. The viewport and the scissor box are LLRender's facades.

// [Viewer Lab] glGenFramebuffers for the target's own framebuffer, in allocate and setColorAttachment
static void gl_gen_framebuffer(U32& framebuffer)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_gen_framebuffer");
        return;
    }
    glGenFramebuffers(1, (GLuint*) &framebuffer);
}

// [Viewer Lab] glBindFramebuffer on GL_FRAMEBUFFER, around the attachments and back to sCurFBO
static void gl_bind_framebuffer(U32 framebuffer)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_bind_framebuffer");
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
}

// [Viewer Lab] glBindFramebuffer on the caller's target (GL_READ_FRAMEBUFFER or GL_DRAW_FRAMEBUFFER), for
// copyFromBackBuffer's binds around its copy, as LLSceneMonitor made them
static void gl_bind_framebuffer_target(U32 target, U32 framebuffer)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_bind_framebuffer_target");
        return;
    }
    glBindFramebuffer(target, framebuffer);
}

// [Viewer Lab] glFramebufferTexture2D on GL_FRAMEBUFFER at level 0: an attachment, or none with texture 0
static void gl_framebuffer_texture_2d(U32 attachment, U32 target, U32 texture)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_framebuffer_texture_2d");
        return;
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, target, texture, 0);
}

// [Viewer Lab] release's glDeleteFramebuffers of the target's framebuffer
static void gl_delete_framebuffer(U32& framebuffer)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_delete_framebuffer");
        return;
    }
    glDeleteFramebuffers(1, (GLuint*) &framebuffer);
}

// [Viewer Lab] LLRenderTarget's own check that an attachment's storage was made: one glGetError
static bool gl_error_pending()
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_error_pending");
        return false;
    }
    return glGetError() != GL_NO_ERROR;
}

// [Viewer Lab] glCheckFramebufferStatus for LLRenderTarget::framebufferStatus; complete when GL is not the backend,
// whose passes check their attachments themselves
static U32 gl_framebuffer_status(U32 target)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_framebuffer_status");
        return GL_FRAMEBUFFER_COMPLETE;
    }
    return glCheckFramebufferStatus(target);
}

// [Viewer Lab] glDrawBuffer with the caller's buffer: bindTarget's GL_NONE for a target without colour and flush's GL_BACK
// when the stack returns to the window
static void gl_draw_buffer(U32 buffer)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_draw_buffer");
        return;
    }
    glDrawBuffer(buffer);
}

// [Viewer Lab] glReadBuffer with the caller's buffer: bindTarget's GL_COLOR_ATTACHMENT0 (GL_NONE without colour) and
// flush's GL_BACK at the window, which readbacks use
static void gl_read_buffer(U32 buffer)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_read_buffer");
        return;
    }
    glReadBuffer(buffer);
}

// [Viewer Lab] glDrawBuffers with the caller's colour attachments, in bindTarget and setDrawBuffers
static void gl_draw_buffers(S32 count, const U32* buffers)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_draw_buffers");
        return;
    }
    glDrawBuffers(count, buffers);
}

// [Viewer Lab] clear's and clearCurrent's glClear with the clear colour and depth GL has
static void gl_clear(U32 mask)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_clear");
        return;
    }
    glClear(mask);
}

// [Viewer Lab] setClearColor's glClearColor, which every later glClear uses
static void gl_clear_color(F32 red, F32 green, F32 blue, F32 alpha)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_clear_color");
        return;
    }
    glClearColor(red, green, blue, alpha);
}

// [Viewer Lab] flush's glGenerateMipmap for a TMG_AUTO target, on the texture of the unit bindTexture left active
static void gl_generate_mipmap(U32 target)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:gl_generate_mipmap");
        return;
    }
    glGenerateMipmap(target);
}

// [Viewer Lab]
U32 LLRenderTarget::framebufferStatus(U32 target)
{
    return gl_framebuffer_status(target);
}

void check_framebuffer_status()
{
    if (gDebugGL)
    {
        GLenum status = LLRenderTarget::framebufferStatus(GL_DRAW_FRAMEBUFFER); // [Viewer Lab] the facade
        switch (status)
        {
        case GL_FRAMEBUFFER_COMPLETE:
            break;
        default:
            LL_WARNS() << "check_framebuffer_status failed -- " << std::hex << status << LL_ENDL;
            ll_fail("check_framebuffer_status failed");
            break;
        }
    }
}

bool LLRenderTarget::sUseFBO = false;
U32 LLRenderTarget::sCurFBO = 0;


extern S32 gGLViewport[4];

U32 LLRenderTarget::sCurResX = 0;
U32 LLRenderTarget::sCurResY = 0;

LLRenderTarget::LLRenderTarget() :
    mResX(0),
    mResY(0),
    mFBO(0),
    mDepth(0),
    mUseDepth(false),
    mGenerateMipMaps(LLTexUnit::TMG_NONE), // <Luminous/> read by flush() before any allocate
    mMipLevels(0), // <Luminous/>
    mUsage(LLTexUnit::TT_TEXTURE)
{
}

LLRenderTarget::~LLRenderTarget()
{
    release();
}

// [Viewer Lab]
LLRenderTarget::LLRenderTarget(const LLRenderTarget& other) = default;

// [Viewer Lab]
LLRenderTarget& LLRenderTarget::operator=(const LLRenderTarget& other) = default;

void LLRenderTarget::resize(U32 resx, U32 resy)
{
    //for accounting, get the number of pixels added/subtracted
    S32 pix_diff = (resx*resy)-(mResX*mResY);

    mResX = resx;
    mResY = resy;

    llassert(mInternalFormat.size() == mTex.size());

    llassert(mRHITextures.size() == mTex.size()); // [Viewer Lab]

    for (U32 i = 0; i < mTex.size(); ++i)
    { //resize color attachments
        // [Viewer Lab] A TMG_AUTO target's first attachment with a texture of the viewer's device keeps the full chain
        // addColorAttachment gave it, at the new size: the description's levels and GL's level range, where
        // glGenerateMipmap then stops as with GL's default range
        const S32 max_level = i == 0 && mGenerateMipMaps == LLTexUnit::TMG_AUTO && mRHITextures[i].notNull()
                                      && mResX > 0 && mResY > 0
                                  ? S32(LL::RHI::fullMipCount(mResX, mResY)) - 1
                                  : -1;
        // [Viewer Lab] A texture of the viewer's device is a new one at the new size, which adopts the GL name with no GL
        // call; off GL it moves the accounting
        if (mRHITextures[i].notNull())
        {
            LLImageGL::respecifyEmptyTexture(mRHITextures[i], mTex[i], S32(mResX), S32(mResY), U32(max_level + 1));
        }
        if (mTex[i] != 0) // [Viewer Lab] a GL name, always on GL
        {
            gl_bind_raw_name(gGL.getTexUnit(0), mUsage, mTex[i], false); // [Viewer Lab] bindManual's GL
            if (max_level >= 0) // [Viewer Lab] createEmptyTexture's order: the range before the storage
            {
                LLImageGL::setLevelRange(mUsage, max_level);
            }
            LLImageGL::setManualImage(LLTexUnit::getInternalType(mUsage), 0, mInternalFormat[i], mResX, mResY, GL_RGBA, GL_UNSIGNED_BYTE, NULL, false);
        }
        sBytesAllocated += pix_diff*4;
    }

    if (ownsDepth()) // [Viewer Lab]
    {
        if (mRHIDepth.notNull()) // [Viewer Lab] as the colours above
        {
            LLImageGL::respecifyEmptyTexture(mRHIDepth, mDepth, S32(mResX), S32(mResY));
        }
        if (mDepth != 0) // [Viewer Lab]
        {
            gl_bind_raw_name(gGL.getTexUnit(0), mUsage, mDepth, false); // [Viewer Lab] bindManual's GL
            U32 internal_type = LLTexUnit::getInternalType(mUsage);
            LLImageGL::setManualImage(internal_type, 0, GL_DEPTH_COMPONENT24, mResX, mResY, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL, false);
        }

        sBytesAllocated += pix_diff*4;
    }
}

// [Viewer Lab] A raw attachment: a name from LLImageGL's pool, bound on texture unit 0, and its storage with no data.
// The path without the viewer's device (headless, the unit tests' fixture), off the main thread, while GL is disabled,
// for an attachment of no pixels, and where the device refuses the description. The GL backend's raw path.
static U32 raw_attachment(LLTexUnit::eTextureType usage, U32 intformat, U32 pixformat, U32 pixtype, U32 width, U32 height)
{
    // [Viewer Lab] On Metal an attachment of no pixels, a refused format or a call off the main thread gets no raw
    // name; the callers take 0 as no attachment, as mTex and mDepth are 0 there
    if (!gl_is_backend())
    {
        LL_GL_GATED("llrendertarget:raw_attachment");
        return 0;
    }
    U32 tex = 0;
    LLImageGL::generateTextures(1, &tex);
    gl_bind_raw_name(gGL.getTexUnit(0), usage, tex, false); // [Viewer Lab] bindManual's GL
    LLImageGL::setManualImage(LLTexUnit::getInternalType(usage), 0, S32(intformat), S32(width), S32(height), pixformat, pixtype,
                              NULL, false);
    return tex;
}

// [Viewer Lab] Colour and depth attachments are drawn into and sampled, and read back
static constexpr U8 ATTACHMENT_USAGE = LL::RHI::TEXTURE_SAMPLED | LL::RHI::TEXTURE_RENDER_TARGET | LL::RHI::TEXTURE_READBACK;

// [Viewer Lab] The GL name LLRenderTarget keeps in mTex and mDepth for getTexture, getDepth, the framebuffer attachments
// and the raw binds in llrender, 0 off GL; the GL backend's state in llrender
static U32 gl_name(const LLPointer<LL::RHI::Texture>& texture)
{
    return LL::RHI::GLNames::of(texture.get());
}

// [Viewer Lab] Kept, never released, where LLRenderTarget deletes nothing, so GL keeps the name and no render interface
// object goes in static destruction
static void keep_unreleased(LLPointer<LL::RHI::Texture>& texture)
{
    texture->ref();
    texture = nullptr;
}

// [Viewer Lab] An attachment: a texture of the viewer's render device with LLRenderTarget's GL calls (createEmptyTexture),
// or a raw name without the device, off the main thread, while GL is disabled, where the table or the device refuses the
// description (createEmptyTexture's own WARN tagged RHI), and for an attachment of no pixels, which no backend has
// storage for and which keeps its raw empty name without a WARN, as buffer parts of size 0 do: the branch is on whether
// the device made a texture, as LLGPUZone branches on the device, never on the backend. The texture
// is null on entry. With max_level 0 or more the texture has levels 0 to max_level, and GL gets that level range
// (createEmptyTexture's two glTexParameteri); the raw name keeps GL's default range.
static U32 make_attachment(LLPointer<LL::RHI::Texture>& texture, LLTexUnit::eTextureType usage, U32 intformat, U32 pixformat,
                           U32 pixtype, U32 width, U32 height, S32 max_level = -1, U8 extra_usage = 0)
{
    llassert(texture.isNull());
    if (width > 0 && height > 0)
    {
        // <Luminous> a memoryless attachment is a render target and nothing else
        const U8 texture_usage = (extra_usage & LL::RHI::TEXTURE_MEMORYLESS)
                                     ? U8(LL::RHI::TEXTURE_RENDER_TARGET | LL::RHI::TEXTURE_MEMORYLESS)
                                     : U8(ATTACHMENT_USAGE | extra_usage);
        texture = LLImageGL::createEmptyTexture(usage, S32(intformat), pixformat, pixtype, S32(width), S32(height),
                                                texture_usage, max_level);
    }
    if (texture.notNull())
    {
        return gl_name(texture);
    }
    return raw_attachment(usage, intformat, pixformat, pixtype, width, height);
}

// [Viewer Lab] Where LLRenderTarget deletes a name: through its texture of the viewer's render device when it has one,
// whose release puts the name into LLImageGL's deletion lists once; kept unreleased while GL is not initialised, where
// LLImageGL's lists take no name and survivors may be in static destruction; a raw name (made without the device, or the
// caller's left by setColorAttachment) directly
static void release_attachment(LLPointer<LL::RHI::Texture>& texture, const U32& name)
{
    if (texture.isNull())
    {
        LLImageGL::deleteTextures(1, &name);
    }
    else if (gGLManager.mInited)
    {
        if (name == 0 && !(texture->desc().usage & LL::RHI::TEXTURE_MEMORYLESS)) // <Luminous/> a memoryless one counted nothing
        {
            // [Viewer Lab] Without a GL name (GL is not the backend) no deletion list takes the bytes back, so they go here
            // by the texture's id, bound on unit 0 as createEmptyTexture's setManualImage counted them
            gGL.getTexUnit(0)->bindTexture(LLTexUnit::TT_TEXTURE, texture.get());
            LLImageGLMemory::free_cur_tex_image();
        }
        LLTexUnit::releaseTexture(texture.get()); // [Viewer Lab] the encoder lets go first, so the name goes now
        texture = nullptr;
    }
    else
    {
        keep_unreleased(texture);
    }
}

// [Viewer Lab] Which target's depth a sharer uses, for its passes: non-owning, so the owner's depth goes to LLImageGL's
// lists at the owner's release; never destroyed, so releases in static destruction can still erase.
static std::unordered_map<const LLRenderTarget*, const LLRenderTarget*>& shared_depth_owners()
{
    static auto* owners = new std::unordered_map<const LLRenderTarget*, const LLRenderTarget*>();
    return *owners;
}

// [Viewer Lab] The depth texture of the target whose depth a sharer uses, or null when it has no owner
static LL::RHI::Texture* shared_depth_of(const LLRenderTarget& target)
{
    const auto found = shared_depth_owners().find(&target);
    return found != shared_depth_owners().end() ? found->second->getRHIDepth() : nullptr;
}

// [Viewer Lab] At a target's release: its own link as a sharer, and every link that names it as the owner
static void forget_shared_depth(const LLRenderTarget& target)
{
    std::unordered_map<const LLRenderTarget*, const LLRenderTarget*>& owners = shared_depth_owners();
    owners.erase(&target);
    for (auto it = owners.begin(); it != owners.end();)
    {
        if (it->second == &target)
        {
            it = owners.erase(it);
        }
        else
        {
            ++it;
        }
    }
}


bool LLRenderTarget::allocate(U32 resx, U32 resy, U32 color_fmt, bool depth, LLTexUnit::eTextureType usage, LLTexUnit::eTextureMipGeneration generateMipMaps)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(usage == LLTexUnit::TT_TEXTURE);
    llassert(!isBoundInStack());

    if(mResX == resx && mResY == resy && mUsage == usage && depth == mUseDepth && mGenerateMipMaps == generateMipMaps)
    {
        return true;
    }
    resx = llmin(resx, (U32) gGLManager.mGLMaxTextureSize);
    resy = llmin(resy, (U32) gGLManager.mGLMaxTextureSize);

    release();

    mResX = resx;
    mResY = resy;

    mUsage = usage;
    mUseDepth = depth;

    mGenerateMipMaps = generateMipMaps;

    if (mGenerateMipMaps != LLTexUnit::TMG_NONE) {
        // Calculate the number of mip levels based upon resolution that we should have.
        mMipLevels = 1 + (U32)floor(log10((float)llmax(mResX, mResY)) / log10(2.0));
    }

    if (depth)
    {
        if (!allocateDepth())
        {
            LL_WARNS() << "Failed to allocate depth buffer for render target." << LL_ENDL;
            return false;
        }
    }

    gl_gen_framebuffer(mFBO); // [Viewer Lab]
    mPassOnly = mFBO == 0 && stack_device() != nullptr; // [Viewer Lab] GL always gives a name

    if (mDepth)
    {
        gl_bind_framebuffer(mFBO); // [Viewer Lab]

        gl_framebuffer_texture_2d(GL_DEPTH_ATTACHMENT, LLTexUnit::getInternalType(mUsage), mDepth); // [Viewer Lab]

        gl_bind_framebuffer(sCurFBO); // [Viewer Lab]
    }

    return addColorAttachment(color_fmt);
}

void LLRenderTarget::setColorAttachment(LLImageGL* img, LLGLuint use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    llassert(img != nullptr); // img must not be null
    llassert(sUseFBO); // FBO support must be enabled
    llassert(mDepth == 0); // depth buffers not supported with this mode
    llassert(mTex.empty()); // mTex must be empty with this mode (binding target should be done via LLImageGL)
    llassert(!isBoundInStack());

    if (mFBO == 0)
    {
        gl_gen_framebuffer(mFBO); // [Viewer Lab]
        mPassOnly = mFBO == 0 && stack_device() != nullptr; // [Viewer Lab] as in allocate
    }

    mResX = img->getWidth();
    mResY = img->getHeight();
    mUsage = img->getTarget();

    if (use_name == 0)
    {
        use_name = LL::RHI::GLNames::of(img); // [Viewer Lab]
    }

    mTex.push_back(use_name);
    mRHITextures.push_back(nullptr); // [Viewer Lab] the caller's image owns its texture

    gl_bind_framebuffer(mFBO); // [Viewer Lab]
    gl_framebuffer_texture_2d(GL_COLOR_ATTACHMENT0, LLTexUnit::getInternalType(mUsage), use_name); // [Viewer Lab]
        stop_glerror();

    check_framebuffer_status();

    gl_bind_framebuffer(sCurFBO); // [Viewer Lab]
}

void LLRenderTarget::releaseColorAttachment()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    llassert(!isBoundInStack());
    llassert(mTex.size() == 1); //cannot use releaseColorAttachment with LLRenderTarget managed color targets
    llassert(hasFramebuffer()); // [Viewer Lab] mFBO must be valid, or a pass-only target off GL

    gl_bind_framebuffer(mFBO); // [Viewer Lab]
    gl_framebuffer_texture_2d(GL_COLOR_ATTACHMENT0, LLTexUnit::getInternalType(mUsage), 0); // [Viewer Lab]
    gl_bind_framebuffer(sCurFBO); // [Viewer Lab]

    // [Viewer Lab] mTex.clear() deletes nothing, so a texture of this target's own is kept unreleased (only
    // setColorAttachment's null entry is expected here)
    for (LLPointer<LL::RHI::Texture>& texture : mRHITextures)
    {
        if (texture.notNull())
        {
            keep_unreleased(texture);
        }
    }
    mRHITextures.clear();

    mTex.clear();
}

bool LLRenderTarget::addColorAttachment(U32 color_fmt)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(!isBoundInStack());

    if (color_fmt == 0)
    {
        return true;
    }

    U32 offset = static_cast<U32>(mTex.size());

    if( offset >= 4 )
    {
        LL_WARNS() << "Too many color attachments" << LL_ENDL;
        llassert( offset < 4 );
        return false;
    }
    if( offset > 0 && !hasFramebuffer() ) // [Viewer Lab]
    {
        llassert( hasFramebuffer() ); // [Viewer Lab]
        return false;
    }

    // [Viewer Lab] A pending batch is drawn before the clearing, as bindManual's flush draws it
    gGL.flush();

    stop_glerror();

    U32 tex = 0; // [Viewer Lab]
    LLPointer<LL::RHI::Texture> texture; // [Viewer Lab]

    {
        clear_glerror();
        // [Viewer Lab] A TMG_AUTO target's first attachment, whose levels flush makes, has its full chain, so another
        // backend has the levels to make. GL's level range then ends at the chain's last level, where glGenerateMipmap
        // stops with GL's default range too, so GL makes the same levels; mMipLevels is not used, since upstream's
        // floating-point count is one short at some sizes (128, 1024)
        // <Luminous> A TMG_MANUAL target's first attachment has its full chain as well, written by compute passes
        // (TEXTURE_STORAGE), which only a device with them makes; nothing on OpenGL asks for TMG_MANUAL.
        const bool manual = offset == 0 && mGenerateMipMaps == LLTexUnit::TMG_MANUAL;
        const S32 max_level = offset == 0 && (mGenerateMipMaps == LLTexUnit::TMG_AUTO || manual)
                                  ? S32(LL::RHI::fullMipCount(mResX, mResY)) - 1
                                  : -1;
        const U8 extra_usage = manual ? U8(LL::RHI::TEXTURE_STORAGE) : U8(0);
        tex = make_attachment(texture, mUsage, color_fmt, GL_RGBA, GL_UNSIGNED_BYTE, mResX, mResY, max_level, extra_usage); // [Viewer Lab]
        // </Luminous>
        if (gl_error_pending()) // [Viewer Lab]
        {
            if (texture.notNull()) // [Viewer Lab] the name stays
            {
                keep_unreleased(texture);
            }
            LL_WARNS() << "Could not allocate color buffer for render target." << LL_ENDL;
            return false;
        }
    }

    sBytesAllocated += mResX*mResY*4;

    stop_glerror();


    if (offset == 0)
    { //use bilinear filtering on single texture render targets that aren't multisampled
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        stop_glerror();
    }
    else
    { //don't filter data attachments
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        stop_glerror();
    }

    if (mUsage != LLTexUnit::TT_RECT_TEXTURE)
    {
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_MIRROR);
        stop_glerror();
    }
    else
    {
        // ATI doesn't support mirrored repeat for rectangular textures.
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        stop_glerror();
    }

    if (mFBO)
    {
        gl_bind_framebuffer(mFBO); // [Viewer Lab]
        gl_framebuffer_texture_2d(GL_COLOR_ATTACHMENT0 + offset, LLTexUnit::getInternalType(mUsage), tex); // [Viewer Lab]

        check_framebuffer_status();

        gl_bind_framebuffer(sCurFBO); // [Viewer Lab]
    }

    mTex.push_back(tex);
    mRHITextures.push_back(texture); // [Viewer Lab]
    mInternalFormat.push_back(color_fmt);

    if (gDebugGL)
    { //bind and unbind to validate target
        bindTarget();
        flush();
    }


    return true;
}

bool LLRenderTarget::allocateDepth()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    gGL.flush(); // [Viewer Lab] as in addColorAttachment

    stop_glerror();
    clear_glerror();
    // <Luminous> memoryless when asked, on a device that has it (Metal's compute passes)
    LL::RHI::Device* device = stack_device();
    const bool memoryless = mMemorylessDepth && device && device->capabilities().computePasses;
    mDepth = make_attachment(mRHIDepth, mUsage, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, mResX, mResY, -1,
                             memoryless ? U8(LL::RHI::TEXTURE_MEMORYLESS) : U8(0)); // [Viewer Lab]
    if (!memoryless)
    {
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
    }
    // </Luminous>

    sBytesAllocated += mResX*mResY*4;

    if (gl_error_pending()) // [Viewer Lab]
    {
        LL_WARNS() << "Unable to allocate depth buffer for render target." << LL_ENDL;
        return false;
    }

    return true;
}

void LLRenderTarget::shareDepthBuffer(LLRenderTarget& target)
{
    llassert(!isBoundInStack());

    if (!hasFramebuffer() || !target.hasFramebuffer()) // [Viewer Lab]
    {
        LL_ERRS() << "Cannot share depth buffer between non FBO render targets." << LL_ENDL;
    }

    if (target.ownsDepth()) // [Viewer Lab]
    {
        LL_ERRS() << "Attempting to override existing depth buffer.  Detach existing buffer first." << LL_ENDL;
    }

    if (target.mUseDepth)
    {
        LL_ERRS() << "Attempting to override existing shared depth buffer. Detach existing buffer first." << LL_ENDL;
    }

    if (ownsDepth()) // [Viewer Lab]
    {
        gl_bind_framebuffer(target.mFBO); // [Viewer Lab]

        gl_framebuffer_texture_2d(GL_DEPTH_ATTACHMENT, LLTexUnit::getInternalType(mUsage), mDepth); // [Viewer Lab]

        check_framebuffer_status();

        gl_bind_framebuffer(sCurFBO); // [Viewer Lab]

        target.mUseDepth = true;
        shared_depth_owners()[&target] = this; // [Viewer Lab] the sharer's passes name this depth
    }
}

void LLRenderTarget::release()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(!isBoundInStack());
    llassert(mRHITextures.size() == mTex.size()); // [Viewer Lab]
    forget_shared_depth(*this); // [Viewer Lab]

    if (ownsDepth()) // [Viewer Lab]
    {
        release_attachment(mRHIDepth, mDepth); // [Viewer Lab]

        mDepth = 0;

        sBytesAllocated -= mResX*mResY*4;
    }
    // else if (mFBO)
    if (mFBO)
    {
        gl_bind_framebuffer(mFBO); // [Viewer Lab]

        if (mUseDepth)
        { //detach shared depth buffer
            gl_framebuffer_texture_2d(GL_DEPTH_ATTACHMENT, LLTexUnit::getInternalType(mUsage), 0); // [Viewer Lab]
            mUseDepth = false;
        }

        gl_bind_framebuffer(sCurFBO); // [Viewer Lab]
    }

    // Detach any extra color buffers (e.g. SRGB spec buffers)
    //
    if (hasFramebuffer() && (mTex.size() > 1)) // [Viewer Lab]
    {
        gl_bind_framebuffer(mFBO); // [Viewer Lab]
        size_t z;
        for (z = mTex.size() - 1; z >= 1; z--)
        {
            sBytesAllocated -= mResX*mResY*4;
            gl_framebuffer_texture_2d(U32(GL_COLOR_ATTACHMENT0 + z), LLTexUnit::getInternalType(mUsage), 0); // [Viewer Lab]
            release_attachment(mRHITextures[z], mTex[z]); // [Viewer Lab]
        }
        gl_bind_framebuffer(sCurFBO); // [Viewer Lab]
    }

    if (mFBO)
    {
        if (mFBO == sCurFBO)
        {
            sCurFBO = 0;
            gl_bind_framebuffer(0); // [Viewer Lab]
        }

        gl_delete_framebuffer(mFBO); // [Viewer Lab]
        mFBO = 0;
    }
    mPassOnly = false; // [Viewer Lab]

    if (mTex.size() > 0)
    {
        sBytesAllocated -= mResX*mResY*4;
        release_attachment(mRHITextures[0], mTex[0]); // [Viewer Lab]
    }

    mTex.clear();
    mRHITextures.clear(); // [Viewer Lab]
    mInternalFormat.clear();

    mResX = mResY = 0;
}

// [Viewer Lab] The stack's passes on the viewer's device ---------------------------------------------------------
//
// Every level of LLRenderTarget's stack is a render pass of the viewer's encoder (RenderPassDesc::stack), Load and Store:
// bindTarget ends the open pass and begins the target's, flush ends it and goes on in the previous target's pass or, at
// the bottom, the window's back buffer's, which a frame without one begins when something needs it (beginStackPass). On
// GL these calls run in an OwnGL scope, so the GL backend only tracks them, and LLRenderTarget's GL backend functions above
// make every GL call. Without the device, off the main thread, or for a level whose attachments are not all textures of
// the device, there is no pass: GL alone.

// [Viewer Lab] True while LLRenderTarget sends a call of its stack to the viewer's device
static bool sMakesOwnGL = false;

namespace
{
    // [Viewer Lab] The scope of one call of the stack to the device; it puts the previous value back, so a nested scope
    // never clears it early.
    struct OwnGL
    {
        const bool mPrevious = sMakesOwnGL;

        OwnGL() { sMakesOwnGL = true; }
        ~OwnGL() { sMakesOwnGL = mPrevious; }
    };

    // [Viewer Lab] The one pass the stack has open on the viewer's device, kept as it began: the device's id and frame
    // number, which tell that a frame's end, a release or a new device has closed it; the level, kept only for debugging
    // and not compared (null for the back buffer); the attachments as pointers the encoder's tracker holds while the pass
    // is open, and only read then; plain values, so static destruction has nothing to release.
    struct StackPass
    {
        U64 device = 0;
        U64 frame = 0;
        const LLRenderTarget* target = nullptr;
        bool backBuffer = false;
        U32 colorCount = 0;
        std::array<LL::RHI::Texture*, LL::RHI::PipelineDesc::MAX_COLOR_ATTACHMENTS> color{};
        LL::RHI::Texture* depth = nullptr;
        bool readOnly = false;
    };
}

static StackPass sStackPass;

// [Viewer Lab]
bool LLRenderTarget::makesOwnGL()
{
    return sMakesOwnGL;
}

#if LL_RHI_METAL
// [Viewer Lab] For the Metal encoder only (its clear and endRenderPass): OwnGL, and the masks GL's glClear in gl_clear
// writes with, as the facade keeps them; GL never calls it
LL::RHI::MetalFacadeClear LL::RHI::metalFacadeClear()
{
    MetalFacadeClear facade;
    facade.ownGL = LLRenderTarget::makesOwnGL();
    gGL.getColorMask(facade.color[0], facade.color[1], facade.color[2], facade.color[3]);
    GLboolean enabled = GL_FALSE;
    GLboolean write   = GL_FALSE;
    GLenum    func    = GL_LEQUAL;
    LLGLDepthTest::getState(enabled, write, func); // sWriteEnabled, with no GL
    facade.depthWrite = write != GL_FALSE;
    return facade;
}
#endif

// [Viewer Lab] The viewer's device for the stack's passes, on the main thread, as LLGPUZone has it; never a branch on the
// backend
static LL::RHI::Device* stack_device()
{
    LL::RHI::Device* device = LL::RHI::viewerDevice();
    return device && on_main_thread() ? device : nullptr;
}

// [Viewer Lab] Whether the stack's record names a pass still open on the device: one begun on it in this frame and not
// ended since
static bool stack_pass_open(LL::RHI::Device* device)
{
    return device && sStackPass.device == device->id() && sStackPass.frame == device->frameNumber();
}

// [Viewer Lab] The stack's bottom: the window's back buffer and its depth, Load and Store; false without a surface (level
// 1, devices made without a size)
static bool describe_back_buffer(LL::RHI::Device& device, LL::RHI::RenderPassDesc& pass)
{
    LL::RHI::Surface* surface = device.surface();
    if (!surface)
    {
        return false;
    }
    pass.colorCount = 1;
    pass.color[0].texture = surface->backBuffer();
    pass.depth.texture = surface->backBufferDepth();
    pass.name = "back buffer";
    pass.stack = true;
    return true;
}

// [Viewer Lab] Begins the pass of a level, a target's or the back buffer's (null), and keeps it only when the encoder began
// it: a refused pass has logged the tracker's WARN and is never recorded. The one place the stack's passes begin, where
// LLRender gives the begun pass the viewer's dynamic state again, for backends whose passes begin with the defaults.
static void begin_stack_pass(const LLRenderTarget* target)
{
    LL::RHI::Device* device = stack_device();
    if (!device)
    {
        return;
    }
    llassert(!stack_pass_open(device));
    LL::RHI::RenderPassDesc pass;
    if (!(target ? target->describePass(pass) : describe_back_buffer(*device, pass)))
    {
        return;
    }
    bool begun = false;
    {
        OwnGL own;
        begun = device->encoder().beginRenderPass(pass);
    }
    if (!begun)
    {
        return;
    }
    StackPass record;
    record.device = device->id();
    record.frame = device->frameNumber();
    record.target = target;
    record.backBuffer = !target;
    record.colorCount = pass.colorCount;
    for (U32 i = 0; i < pass.colorCount; ++i)
    {
        record.color[i] = pass.color[i].texture.get();
    }
    record.depth = pass.depth.texture.get();
    record.readOnly = pass.depth.readOnly;
    sStackPass = record;
    // [Viewer Lab] The viewer's dynamic state again, for backends whose passes begin with the defaults; on GL only tracked
    gGL.sendDynamicState(device->encoder());
}

// [Viewer Lab] Ends the stack's pass when one is open on the device, and forgets it
static void end_stack_pass()
{
    LL::RHI::Device* device = stack_device();
    if (stack_pass_open(device))
    {
        OwnGL own;
        device->encoder().endRenderPass();
    }
    sStackPass = StackPass();
}

// [Viewer Lab] The colour Encoder::clear clears with: GL's initial clear colour until setClearColor sets
// it; raw glClearColor callers keep GL's own, which gl_clear uses.
static std::array<F32, 4> sClearColor{ 0.f, 0.f, 0.f, 0.f };

// [Viewer Lab] clear's call to Encoder::clear in the stack's current pass, begun when none is open in this frame. A buffer
// the pass lacks was a no-op for glClear, so it is not asked; a read-only depth is asked, and the tracker refuses it. The
// viewer never calls glClearDepth, so the depth is 1.0.
static void clear_stack_pass(U32 mask)
{
    LL::RHI::Device* device = stack_device();
    if (!device || !LLRenderTarget::beginStackPass())
    {
        return;
    }
    const bool color = (mask & GL_COLOR_BUFFER_BIT) && sStackPass.colorCount > 0;
    const bool depth = (mask & GL_DEPTH_BUFFER_BIT) && sStackPass.depth;
    if (color || depth)
    {
        OwnGL own;
        device->encoder().clear(color, sClearColor, depth, 1.f);
    }
}

// [Viewer Lab] flush's mipmaps: Encoder::generateMipmaps outside any pass, only tracked on GL, then gl_generate_mipmap's
// glGenerateMipmap on the texture bindTexture left bound
static void generate_mipmaps(LL::RHI::Texture* texture)
{
    LL::RHI::Device* device = stack_device();
    if (device && texture)
    {
        OwnGL own;
        device->encoder().generateMipmaps(texture);
    }
    gl_generate_mipmap(GL_TEXTURE_2D);
}

// [Viewer Lab]
bool LLRenderTarget::beginStackPass()
{
    LL::RHI::Device* device = stack_device();
    if (device && !stack_pass_open(device))
    {
        begin_stack_pass(sBoundTarget);
    }
    return stack_pass_open(device);
}

// [Viewer Lab]
void LLRenderTarget::endStackPass()
{
    end_stack_pass();
}

// [Viewer Lab] The record copied, nothing described again and no map looked up
bool LLRenderTarget::currentPass(LL::RHI::RenderPassDesc& pass)
{
    if (!stack_pass_open(stack_device()))
    {
        return false;
    }
    pass = LL::RHI::RenderPassDesc();
    pass.colorCount = sStackPass.colorCount;
    for (U32 i = 0; i < sStackPass.colorCount; ++i)
    {
        pass.color[i].texture = sStackPass.color[i];
    }
    pass.depth.texture = sStackPass.depth;
    pass.depth.readOnly = sStackPass.readOnly;
    pass.name = sStackPass.backBuffer ? "back buffer" : "render target";
    pass.stack = true;
    return true;
}

// [Viewer Lab] Every colour attachment it draws into and the depth, own or shared, must be a texture of the device: a raw
// name (made without the device, of size 0, off the main thread, or setColorAttachment's caller's image) leaves the level
// passless. A read-only depth needs a depth texture, which validate() checks, so without one the pass is not read-only
bool LLRenderTarget::describePass(LL::RHI::RenderPassDesc& pass) const
{
    const U32 colors = llmin(U32(mTex.size()), mDrawCount);
    if (colors > LL::RHI::PipelineDesc::MAX_COLOR_ATTACHMENTS || mRHITextures.size() != mTex.size())
    {
        return false;
    }
    for (U32 i = 0; i < colors; ++i)
    {
        if (mRHITextures[i].isNull())
        {
            return false;
        }
        pass.color[i].texture = mRHITextures[i];
    }
    pass.colorCount = colors;
    if (ownsDepth()) // [Viewer Lab]
    {
        if (mRHIDepth.isNull())
        {
            return false;
        }
        pass.depth.texture = mRHIDepth;
    }
    else if (mUseDepth)
    {
        LL::RHI::Texture* shared = shared_depth_of(*this);
        if (!shared)
        {
            return false;
        }
        pass.depth.texture = shared;
    }
    pass.depth.readOnly = mDepthAccess == DEPTH_READ_ONLY && pass.depth.texture.notNull();
    if (colors == 0 && pass.depth.texture.isNull())
    {
        return false;
    }
    pass.name = "render target";
    pass.stack = true;
    return true;
}

// [Viewer Lab]
void LLRenderTarget::bindTarget()
{
    bindTarget(DEPTH_READ_WRITE);
}

void LLRenderTarget::bindTarget(eDepthAccess depth_access) // [Viewer Lab] bindTarget()'s body, with the pass's depth access
{
    LL_PROFILE_GPU_ZONE("bindTarget");
    llassert(mFBO || isComplete()); // [Viewer Lab] keyed on the attachments too, since mFBO stays 0 where GL is not the backend
    llassert(!isBoundInStack());
    llassert(depth_access == DEPTH_READ_WRITE || mDepth || mUseDepth); // [Viewer Lab] a read-only depth needs one

    // [Viewer Lab] The pass of the level below, a target's or the back buffer's, ends before the binding changes
    end_stack_pass();

    gl_bind_framebuffer(mFBO); // [Viewer Lab]
    sCurFBO = mFBO;

    //setup multiple render targets
    GLenum drawbuffers[] = {GL_COLOR_ATTACHMENT0,
                            GL_COLOR_ATTACHMENT1,
                            GL_COLOR_ATTACHMENT2,
                            GL_COLOR_ATTACHMENT3};

    if (mTex.empty())
    { //no color buffer to draw to
        gl_draw_buffer(GL_NONE); // [Viewer Lab]
        gl_read_buffer(GL_NONE); // [Viewer Lab]
    }
    else
    {
        gl_draw_buffers(static_cast<S32>(mTex.size()), drawbuffers); // [Viewer Lab]
        gl_read_buffer(GL_COLOR_ATTACHMENT0); // [Viewer Lab]
    }
    check_framebuffer_status();

    gGL.setViewport(0, 0, S32(mResX), S32(mResY)); // [Viewer Lab] LLRender's facade, no pass open
    sCurResX = mResX;
    sCurResY = mResY;

    mPreviousRT = sBoundTarget;
    sBoundTarget = this;

    // [Viewer Lab] The pass's depth access, and every colour drawn into, as the glDrawBuffers above says
    mDepthAccess = depth_access;
    mDrawCount = U32_MAX;
    begin_stack_pass(this); // [Viewer Lab] this target's pass, Load and Store; on GL only tracked
}

void LLRenderTarget::clear(U32 mask_in)
{
    LL_PROFILE_GPU_ZONE("clear");
    llassert(mFBO || isComplete()); // [Viewer Lab] keyed on the attachments too, since mFBO stays 0 where GL is not the backend
    U32 mask = GL_COLOR_BUFFER_BIT;
    if (mUseDepth)
    {
        mask |= GL_DEPTH_BUFFER_BIT;

    }
    // [Viewer Lab] Through Encoder::clear in the stack's pass; on GL only tracked, and gl_clear below makes the GL clear
    clear_stack_pass(mask & mask_in);
    if (hasFramebuffer()) // [Viewer Lab]
    {
        check_framebuffer_status();
        stop_glerror();
        gl_clear(mask & mask_in); // [Viewer Lab]
        stop_glerror();
    }
    else
    {
        LLGLEnable scissor(GL_SCISSOR_TEST);
        gGL.setScissor(0, 0, S32(mResX), S32(mResY)); // [Viewer Lab] LLRender's facade
        stop_glerror();
        gl_clear(mask & mask_in); // [Viewer Lab]
    }
}

// [Viewer Lab] Encoder::clear's colour, and GL's clear colour for glClear
void LLRenderTarget::setClearColor(F32 red, F32 green, F32 blue, F32 alpha)
{
    sClearColor = { red, green, blue, alpha };
    gl_clear_color(red, green, blue, alpha);
}

// [Viewer Lab] clear_stack_pass begins the current level's pass when none is open, the back buffer's when no target is
// bound; no GPU zone, as the raw glClear has none
void LLRenderTarget::clearCurrent(U32 mask)
{
    llassert(!(mask & GL_STENCIL_BUFFER_BIT));
    clear_stack_pass(mask);
    gl_clear(mask);
}

// [Viewer Lab] Exactly the raw glDrawBuffers(count, buffers) and no gGL.flush, since the raw call flushed nothing; with the
// device the stack's pass ends and begins again over the first count attachments
void LLRenderTarget::setDrawBuffers(S32 count, const U32* buffers)
{
    LLRenderTarget* target = sBoundTarget;
    llassert(target && count >= 0 && U32(count) <= target->mTex.size());
    for (S32 i = 0; i < count; ++i)
    {
        llassert(buffers[i] == U32(GL_COLOR_ATTACHMENT0 + i));
    }
    if (!target)
    {
        gl_draw_buffers(count, buffers);
        return;
    }
    end_stack_pass();
    gl_draw_buffers(count, buffers);
    target->mDrawCount = U32(llmax(count, 0));
    begin_stack_pass(target);
}

// [Viewer Lab] No GL call; no gGL.flush, as in setDrawBuffers; the pass keeps setDrawBuffers' attachments (mDrawCount)
void LLRenderTarget::setDepthAccess(eDepthAccess depth_access)
{
    llassert(sBoundTarget == this);
    llassert(depth_access == DEPTH_READ_WRITE || mDepth || mUseDepth); // [Viewer Lab] as bindTarget: a read-only depth needs one
    if (sBoundTarget != this || depth_access == mDepthAccess)
    {
        return;
    }
    end_stack_pass();
    mDepthAccess = depth_access;
    begin_stack_pass(this);
}

U32 LLRenderTarget::getTexture(U32 attachment) const
{
    if (attachment >= mTex.size())
    {
        LL_WARNS() << "Invalid attachment index " << attachment << " for size " << mTex.size() << LL_ENDL;
        llassert(false);
        return 0;
    }
    return mTex[attachment];
}

U32 LLRenderTarget::getNumTextures() const
{
    return static_cast<U32>(mTex.size());
}

// [Viewer Lab]
LL::RHI::Texture* LLRenderTarget::getRHITexture(U32 attachment) const
{
    return attachment < mRHITextures.size() ? mRHITextures[attachment].get() : nullptr;
}

// [Viewer Lab]
LL::RHI::Texture* LLRenderTarget::getRHIDepth() const
{
    return mRHIDepth.get();
}

// [Viewer Lab] The GL backend's raw-name bind for a raw attachment (no device, setColorAttachment's caller name, a
// refused description)
static bool bind_raw(LLTexUnit* unit, LLTexUnit::eTextureType usage, U32 name, bool has_mips)
{
    return gl_bind_raw_name(unit, usage, name, has_mips);
}

void LLRenderTarget::bindTexture(U32 index, S32 channel, LLTexUnit::eTextureFilterOptions filter_options)
{
    // [Viewer Lab] bindManual's GL by the attachment's device texture, or its raw name
    const bool mips = filter_options == LLTexUnit::TFO_TRILINEAR || filter_options == LLTexUnit::TFO_ANISOTROPIC;
    if (LL::RHI::Texture* texture = getRHITexture(index))
    {
        gGL.getTexUnit(channel)->bindTexture(mUsage, texture, mips);
    }
    else
    {
        bind_raw(gGL.getTexUnit(channel), mUsage, getTexture(index), mips);
    }
    gGL.getTexUnit(channel)->setTextureFilteringOption(filter_options);
}

// [Viewer Lab] LLSceneMonitor's five calls in their order: READ 0, the copy, READ 0, DRAW 0
// and FRAMEBUFFER sCurFBO as it was. Main thread; the caller has bound attachment 0 on the active unit, where GL copies.
// The copy goes through LLGPUCopy::fromFramebuffer with the device surface's back buffer as its source. Without a surface
// (no device, or a device made without a window) the destination is null too, so the copy counts untracked:
// fromFramebuffer reads the bound target's attachment for a null source (framebuffer_source), while GL reads
// framebuffer 0. No stack pass begins or ends here; on Metal the encoder's copy splits the open pass and the pass loads
// afterwards. The capture target has the world view's size and the back buffer the drawable's, so they can differ:
// MTL4ComputeCommandEncoder.h asks for a destination level of the source level's size, but under API Validation a region
// copy from a 64x48 back buffer into a 48x32 target reports nothing and copies the region's texels exactly (a test in
// llrhimetal_metal.mm), so a smaller target needs no branch on the sizes. A larger one (a snapshot above the window's size
// makes the world view larger than the window) copies the overlap, clamped below.
bool LLRenderTarget::copyFromBackBuffer()
{
    const U32 previous = sCurFBO;
    gl_bind_framebuffer_target(GL_READ_FRAMEBUFFER, 0); // the window's framebuffer
    LL::RHI::Device* device = stack_device();
    LL::RHI::Surface* surface = device ? device->surface() : nullptr;
    LL::RHI::Texture* source = surface ? surface->backBuffer() : nullptr;
    U32 width = mResX;
    U32 height = mResY;
    if (source)
    {
        // [Viewer Lab] The overlap with the back buffer: GL defined only the overlap; the target's pixels outside it
        // keep their contents
        width = llmin(width, source->desc().width);
        height = llmin(height, source->desc().height);
    }
    const bool copied = LLGPUCopy::fromFramebuffer(source ? getRHITexture(0) : nullptr, GL_TEXTURE_2D, 0, 0, 0, 0, 0, 0,
                                                   S32(width), S32(height), source);
    gl_bind_framebuffer_target(GL_READ_FRAMEBUFFER, 0);
    gl_bind_framebuffer_target(GL_DRAW_FRAMEBUFFER, 0);
    gl_bind_framebuffer(previous);
    return copied;
}

void LLRenderTarget::flush()
{
    LL_PROFILE_GPU_ZONE("rt flush");
    gGL.flush();
    llassert(mFBO || isComplete()); // [Viewer Lab] keyed on the attachments too, since mFBO stays 0 where GL is not the backend
    llassert(sCurFBO == mFBO);
    llassert(sBoundTarget == this);

    if (mGenerateMipMaps == LLTexUnit::TMG_AUTO)
    {
        LL_PROFILE_GPU_ZONE("rt generate mipmaps");
        // [Viewer Lab] The texture is bound in this target's pass, so a binding through the render interface reaches GL
        // there; the pass ends before the mipmaps, which lie outside passes; on GL generate_mipmaps'
        // call is only tracked and gl_generate_mipmap makes the GL call
        bindTexture(0, 0, LLTexUnit::TFO_TRILINEAR);
        end_stack_pass(); // [Viewer Lab]
        generate_mipmaps(getRHITexture(0)); // [Viewer Lab]
    }
    else // [Viewer Lab]
    {
        end_stack_pass(); // [Viewer Lab] this target's pass ends here
    }

    if (mPreviousRT)
    {
        // a bit hacky -- pop the RT stack back two frames and push
        // the previous frame back on to play nice with the GL state machine
        sBoundTarget = mPreviousRT->mPreviousRT;
        mPreviousRT->bindTarget(mPreviousRT->mDepthAccess); // [Viewer Lab] the level below goes on with its own depth access
    }
    else
    {
        sBoundTarget = nullptr;
        gl_bind_framebuffer(0); // [Viewer Lab]
        sCurFBO = 0;
        gGL.setViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]); // [Viewer Lab] LLRender's facade
        sCurResX = gGLViewport[2];
        sCurResY = gGLViewport[3];
        gl_read_buffer(GL_BACK); // [Viewer Lab]
        gl_draw_buffer(GL_BACK); // [Viewer Lab]
        begin_stack_pass(nullptr); // [Viewer Lab] the window's back buffer, the stack's bottom; none without a surface
    }
}

bool LLRenderTarget::isComplete() const
{
    return !mTex.empty() || ownsDepth(); // [Viewer Lab]
}

void LLRenderTarget::getViewport(S32* viewport)
{
    viewport[0] = 0;
    viewport[1] = 0;
    viewport[2] = mResX;
    viewport[3] = mResY;
}

bool LLRenderTarget::isBoundInStack() const
{
    LLRenderTarget* cur = sBoundTarget;
    while (cur && cur != this)
    {
        cur = cur->mPreviousRT;
    }

    return cur == this;
}

void LLRenderTarget::swapFBORefs(LLRenderTarget& other)
{
    // Must be initialized
    llassert(hasFramebuffer()); // [Viewer Lab] mFBO is 0 on a pass-only target where GL is not the backend
    llassert(other.hasFramebuffer()); // [Viewer Lab]

    // Must be unbound
    // *NOTE: mPreviousRT can be non-null even if this target is unbound - presumably for debugging purposes?
    // [Viewer Lab] A pass-only target has no name to compare; isBoundInStack below holds for it
    llassert(mPassOnly || sCurFBO != mFBO);
    llassert(other.mPassOnly || sCurFBO != other.mFBO);
    llassert(!isBoundInStack());
    llassert(!other.isBoundInStack());

    // Must be same type
    llassert(sUseFBO == other.sUseFBO);
    llassert(mResX == other.mResX);
    llassert(mResY == other.mResY);
    llassert(mInternalFormat == other.mInternalFormat);
    llassert(mTex.size() == other.mTex.size());
    llassert(mRHITextures.size() == other.mRHITextures.size()); // [Viewer Lab]
    llassert(mDepth == other.mDepth);
    llassert(mUseDepth == other.mUseDepth);
    llassert(mGenerateMipMaps == other.mGenerateMipMaps);
    llassert(mMipLevels == other.mMipLevels);
    llassert(mUsage == other.mUsage);

    std::swap(mFBO, other.mFBO);
    std::swap(mPassOnly, other.mPassOnly); // [Viewer Lab] false on GL
    std::swap(mTex, other.mTex);
    std::swap(mRHITextures, other.mRHITextures); // [Viewer Lab] the textures go with their names
}
