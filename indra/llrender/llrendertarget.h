/**
 * @file llrendertarget.h
 * @brief Off screen render target abstraction.  Loose wrapper for GL_EXT_framebuffer_objects.
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

#ifndef LL_LLRENDERTARGET_H
#define LL_LLRENDERTARGET_H

// LLRenderTarget is unavailible on the mapserver since it uses FBOs.

#include "llgl.h"
#include "llrender.h"

// [Viewer Lab]
namespace LL::RHI
{
    class Texture;
    struct RenderPassDesc;
}

/*
 Wrapper around OpenGL framebuffer objects for use in render-to-texture

 SAMPLE USAGE:

    LLRenderTarget target;

    ...

    //allocate a 256x256 RGBA render target with depth buffer
    target.allocate(256,256,GL_RGBA,TRUE);

    //render to contents of offscreen buffer
    target.bindTarget();
    target.clear();
    ... <issue drawing commands> ...
    target.flush();

    ...

    //use target as a texture
    gGL.getTexUnit(INDEX)->bind(&target);
    ... <issue drawing commands> ...

*/

class LLRenderTarget
{
public:
    // Whether or not to use FBO implementation
    static bool sUseFBO;
    static U32 sBytesAllocated;
    static U32 sCurFBO;
    static U32 sCurResX;
    static U32 sCurResY;


    LLRenderTarget();
    ~LLRenderTarget();

    // [Viewer Lab] Defined in llrendertarget.cpp, where the render interface's texture is complete: std::vector<LLRenderTarget>::resize
    // copies elements (llreflectionmapmanager.cpp, llheroprobemanager.cpp). A copy shares the GL names and the textures.
    LLRenderTarget(const LLRenderTarget& other);
    LLRenderTarget& operator=(const LLRenderTarget& other);

    //allocate resources for rendering
    //must be called before use
    //multiple calls will release previously allocated resources
    // resX - width
    // resY - height
    // color_fmt - GL color format (e.g. GL_RGB)
    // depth - if true, allocate a depth buffer
    // usage - deprecated, should always be TT_TEXTURE
    bool allocate(U32 resx, U32 resy, U32 color_fmt, bool depth = false, LLTexUnit::eTextureType usage = LLTexUnit::TT_TEXTURE, LLTexUnit::eTextureMipGeneration generateMipMaps = LLTexUnit::TMG_NONE);

    //resize existing attachments to use new resolution and color format
    // CAUTION: if the GL runs out of memory attempting to resize, this render target will be undefined
    // DO NOT use for screen space buffers or for scratch space for an image that might be uploaded
    // DO use for render targets that resize often and aren't likely to ruin someone's day if they break
    void resize(U32 resx, U32 resy);

    //point this render target at a particular LLImageGL
    //   Intended usage:
    //      LLRenderTarget target;
    //      target.addColorAttachment(image);
    //      target.bindTarget();
    //      < issue GL calls>
    //      target.flush();
    //      target.releaseColorAttachment();
    //
    // attachment -- LLImageGL to render into
    // use_name -- optional texture name to target instead of attachment->getTexName()
    // NOTE: setColorAttachment and releaseColorAttachment cannot be used in conjuction with
    // addColorAttachment, allocateDepth, resize, etc.
    void setColorAttachment(LLImageGL* attachment, LLGLuint use_name = 0);

    // detach from current color attachment
    void releaseColorAttachment();

    //add color buffer attachment
    //limit of 4 color attachments per render target
    bool addColorAttachment(U32 color_fmt);

    //allocate a depth texture
    bool allocateDepth();

    //share depth buffer with provided render target
    void shareDepthBuffer(LLRenderTarget& target);

    // <Luminous> On a device with memoryless textures (Metal), the depth allocate() makes from now on lives only while
    // a pass draws into it (TEXTURE_MEMORYLESS): for a depth that is never sampled, read back or shared, and whose
    // contents nothing needs past a pass break. Elsewhere the depth is made as always.
    void setMemorylessDepth(bool memoryless) { mMemorylessDepth = memoryless; }

    //free any allocated resources
    //safe to call redundantly
    // asserts that this target is not currently bound or present in the RT stack
    void release();

    //bind target for rendering
    //applies appropriate viewport
    //  If an LLRenderTarget is currently bound, stores a reference to that LLRenderTarget
    //  and restores previous binding on flush() (maintains a stack of Render Targets)
    //  Asserts that this target is not currently bound in the stack
    void bindTarget();

    // [Viewer Lab] How the pass of a bound target uses its depth
    enum eDepthAccess
    {
        DEPTH_READ_WRITE,
        DEPTH_READ_ONLY
    };

    // [Viewer Lab] bindTarget with the depth read-only: the pass samples the depth it tests against (the deferred lights,
    // renderDeferredLighting), so depth writes and depth clears must stay off, which the render interface refuses on every
    // backend (EncoderTracker). GL's calls are bindTarget()'s. The target keeps its access while bound, until
    // setDepthAccess changes it, also when a nested target's flush binds it again.
    void bindTarget(eDepthAccess depth_access);

    // [Viewer Lab] The depth access of the bound target's pass from here on, as bindTarget(eDepthAccess) sets it at the bind:
    // for a bind whose first part samples the depth it tests against and whose later part writes depth (renderDeferredLighting).
    // No GL call; with the viewer's device the stack's pass ends and begins again over the same attachments, loaded,
    // when the access changes. Asserts that this target is the bound one.
    void setDepthAccess(eDepthAccess depth_access);

    //clear render targer, clears depth buffer if present,
    //uses scissor rect if in copy-to-texture mode
    // asserts that this target is currently bound
    void clear(U32 mask = 0xFFFFFFFF);

    //get applied viewport
    void getViewport(S32* viewport);

    //get X resolution
    U32 getWidth() const { return mResX; }

    //get Y resolution
    U32 getHeight() const { return mResY; }

    LLTexUnit::eTextureType getUsage(void) const { return mUsage; }

    U32 getTexture(U32 attachment = 0) const;
    U32 getNumTextures() const;

    U32 getDepth(void) const { return mDepth; }

    // [Viewer Lab] The texture of the viewer's render device whose GL name getTexture(attachment) gives, or null when that name
    // is raw (made without the device, or the caller's through setColorAttachment) or there is no such attachment. Used by
    // the tests and the stack's passes.
    LL::RHI::Texture* getRHITexture(U32 attachment = 0) const;

    // [Viewer Lab] The same for getDepth(); null for a target that only shares another's depth.
    LL::RHI::Texture* getRHIDepth() const;

    void bindTexture(U32 index, S32 channel, LLTexUnit::eTextureFilterOptions filter_options = LLTexUnit::TFO_BILINEAR);

    // [Viewer Lab] Copies the window's back buffer from (0, 0) at this target's size into its colour attachment 0, which
    // the caller has bound on the active unit: GL reads framebuffer 0 (GL_READ_FRAMEBUFFER 0, glCopyTexSubImage2D), and
    // afterwards GL_READ_FRAMEBUFFER and GL_DRAW_FRAMEBUFFER are 0 and GL_FRAMEBUFFER is sCurFBO, as LLSceneMonitor's
    // calls left them. Through LLGPUCopy::fromFramebuffer with the viewer's device: the source is the device surface's
    // back buffer; without a surface the destination is null too (the copy counts untracked), because fromFramebuffer
    // would read the bound target's attachment for a null source while GL reads framebuffer 0. A target larger than the
    // surface's back buffer gets the overlap (GL defined only the overlap; the target's pixels outside it keep their
    // contents). No stack pass begins or ends here. Returns whether it copied, as fromFramebuffer reports it. Main thread.
    bool copyFromBackBuffer();

    //flush rendering operations
    //must be called when rendering is complete
    //should be used 1:1 with bindTarget
    // call bindTarget once, do all your rendering, call flush once
    // If an LLRenderTarget was bound when bindTarget was called, binds that RenderTarget for rendering (maintains RT stack)
    // asserts  that this target is currently bound
    void flush();

    //Returns TRUE if target is ready to be rendered into.
    //That is, if the target has been allocated with at least
    //one renderable attachment (i.e. color buffer, depth buffer).
    bool isComplete() const;

    // [Viewer Lab] Whether the target has a framebuffer: GL's own, or none where its passes are only the viewer's device's
    // (GL is not the backend). On GL it is mFBO != 0.
    bool hasFramebuffer() const { return mFBO != 0 || mPassOnly; }

    // [Viewer Lab] Whether the target has a depth of its own: a GL name, or a texture of the viewer's device without one
    // (GL is not the backend). On GL it is mDepth != 0, since the device's textures have GL names.
    bool ownsDepth() const { return mDepth != 0 || mRHIDepth.notNull(); }

    // Returns true if this RenderTarget is bound somewhere in the stack
    bool isBoundInStack() const;

    static LLRenderTarget* getCurrentBoundTarget() { return sBoundTarget; }

    // [Viewer Lab] For the render interface's GL backend (llrhigl.cpp), main thread: true while LLRenderTarget sends a
    // call of its stack (a pass's begin, end or clear, or a target's mipmaps) to the viewer's device; the GL backend
    // then only tracks the call, because LLRenderTarget's GL backend functions make the GL calls themselves. A static,
    // not thread_local; its scope puts the previous value back, so it nests. A GL backend switch, part of its contract
    // with llrender (llrhigl.cpp).
    static bool makesOwnGL();

    // [Viewer Lab] Main thread: begins the pass of the stack's current level when none is open on the viewer's device in
    // this frame: the bound target's, loaded, or the window's back buffer's when no target is bound and the device has a
    // surface. True when a stack pass is open then. Code that records into the current pass calls it first, also before
    // binding through the encoder.
    static bool beginStackPass();

    // [Viewer Lab] Main thread: ends the stack's open pass without changing what is bound, for code that records passes of
    // its own (level 1's interface scenes); the next switch or beginStackPass begins it again.
    static void endStackPass();

    // [Viewer Lab] Main thread: the stack's pass open on the viewer's device in this frame, as it was when it began (its
    // textures, depth, read-only flag, Load and Store, stack), and true; false when none is open (no device, off the main
    // thread, ended by the frame or by endStackPass, no surface under no target, or a bound target whose attachments are
    // not all textures of the device). The textures are held by the encoder while the pass is open; the caller's
    // description holds its own references. For the tests, level 1 and code that records into the current pass.
    static bool currentPass(LL::RHI::RenderPassDesc& pass);

    // [Viewer Lab] This target's pass as the stack begins it: the colour attachments it draws into (all, or setDrawBuffers'
    // first ones) and its own or shared depth as textures of the viewer's device, the depth read-only when bindTarget made
    // it so, Load and Store, stack; false when one of them has no such texture.
    bool describePass(LL::RHI::RenderPassDesc& pass) const;

    // [Viewer Lab] The status GL gives the framebuffer bound on target (GL_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER or
    // GL_READ_FRAMEBUFFER): GL_FRAMEBUFFER_COMPLETE or GL's reason it is not, in GL's enums, the facades' language.
    // It always asks; check_framebuffer_status asks only with gDebugGL. GL_FRAMEBUFFER_COMPLETE when GL is not the
    // backend. For the owners of glCheckFramebufferStatus's call sites, such as pipeline.cpp's
    // validate_framebuffer_object.
    static U32 framebufferStatus(U32 target = GL_FRAMEBUFFER);

    // [Viewer Lab] glClearColor for every later clear of a target or of the window's back buffer, as glClearColor sets it,
    // for the owners of glClearColor's call sites.
    static void setClearColor(F32 red, F32 green, F32 blue, F32 alpha);

    // [Viewer Lab] glClear(mask) of the current target, the bound one or else the window's back buffer, through
    // Encoder::clear in the stack's pass; mask in GL's bits (colour and depth; the interface has no stencil); GL's clear
    // colour and depth, the scissor, colour mask and depth write mask apply as for glClear. For the owners of glClear's
    // call sites.
    static void clearCurrent(U32 mask);

    // [Viewer Lab] glDrawBuffers(count, buffers) for the bound target, in GL's enums: buffers are GL_COLOR_ATTACHMENT0 + i
    // in order (the interface's passes draw into their first attachments); the target draws into them until it is bound
    // again; with the viewer's device the stack's pass breaks into one over those attachments and the depth, loaded. For
    // the owners of glDrawBuffers' call sites, pipeline.cpp's impostors.
    static void setDrawBuffers(S32 count, const U32* buffers);

    // *HACK
    void swapFBORefs(LLRenderTarget& other);

    static LLRenderTarget* sBoundTarget;

protected:
    U32 mResX;
    U32 mResY;
    std::vector<U32> mTex;
    std::vector<U32> mInternalFormat;
    U32 mFBO;
    // [Viewer Lab] True where allocate or setColorAttachment made no framebuffer because GL is not the backend, and the
    // viewer's device has the target's passes; always false on GL
    bool mPassOnly = false;
    LLRenderTarget* mPreviousRT = nullptr;

    U32 mDepth;
    bool mUseDepth;
    bool mMemorylessDepth = false; // <Luminous/>
    LLTexUnit::eTextureMipGeneration mGenerateMipMaps;
    U32 mMipLevels;

    LLTexUnit::eTextureType mUsage;

    // [Viewer Lab] The textures of the viewer's render device behind mTex, one per entry and null where the name is raw, and
    // behind mDepth. mTex and mDepth are their GL names (LL::RHI::GLNames::of), the GL backend's state.
    std::vector<LLPointer<LL::RHI::Texture>> mRHITextures;
    LLPointer<LL::RHI::Texture> mRHIDepth;

    // [Viewer Lab] The bound pass's depth access, and how many of mTex it draws into (all until setDrawBuffers).
    // They hold only while the target is bound or below the bound target in the stack: bindTarget sets both, so the
    // default copies take them along and swapFBORefs, whose targets are unbound, leaves them.
    eDepthAccess mDepthAccess = DEPTH_READ_WRITE;
    U32 mDrawCount = U32_MAX;
};

#endif

