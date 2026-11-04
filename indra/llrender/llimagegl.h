/**
 * @file llimagegl.h
 * @brief Object for managing images and their textures
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


#ifndef LL_LLIMAGEGL_H
#define LL_LLIMAGEGL_H

#include "llimage.h"

#include "llgltypes.h"
#include "llpointer.h"
#include "llrefcount.h"
#include "v2math.h"
#include "llunits.h"
#include "llthreadsafequeue.h"
#include "llrender.h"
#include "threadpool.h"
#include "workqueue.h"
#include <cstddef>
#include <span>
#include <unordered_set>

// [Viewer Lab]
namespace LL::RHI
{
    class Readback;
    class Texture;
    struct TextureDesc;
    enum class Format : U16; // [Viewer Lab]
}

class LLImageGL;
class LLImageGLHandle;
class LLVertexBuffer; // [Viewer Lab] scaleDown's screen triangle
// [Viewer Lab] The GL backend's name accessor (llrhigl.h), the one way to LLImageGL's GL name outside the class;
// also the GL name an LLImageGLHandle stands for, and the handle of a raw name
namespace LL::RHI::GLNames
{
    U32 of(const LLImageGL* image);
    U32 of(const LLImageGLHandle& handle);
    LLImageGLHandle handle(U32 name);
}

// [Viewer Lab] The texture createGLTexture made for its caller (tex_name, with defer_copy the new texture of the caller's
// later swap), as the caller gives it back to setSubImage, syncToMainThread and syncTexName (doMediaTexUpdate): an opaque
// handle that only LLImageGL and the GL backend's accessor (LL::RHI::GLNames) open. Inside llrender it is the texture's GL
// name, 1:1, the GL backend's state, so the GL calls are as before; off GL that name is 0, as for defer_copy's
// parked texture, which has none. The default handle is the name 0: none, the image's own texture.
class LLImageGLHandle
{
public:
    LLImageGLHandle() = default;
    bool operator==(const LLImageGLHandle& other) const = default;

private:
    friend class LLImageGL;
    friend U32 LL::RHI::GLNames::of(const LLImageGLHandle& handle);
    friend LLImageGLHandle LL::RHI::GLNames::handle(U32 name);
    explicit LLImageGLHandle(U32 name) : mName(name) {}

    U32 mName = 0;
};

#define LL_IMAGEGL_THREAD_CHECK 0 //set to 1 to enable thread debugging for ImageGL

class LLWindow;

#define BYTES_TO_MEGA_BYTES(x) ((x) >> 20)
#define MEGA_BYTES_TO_BYTES(x) ((x) << 20)

namespace LLImageGLMemory
{
    void alloc_tex_image(U32 width, U32 height, U32 intformat, U32 count);
    void free_tex_image(U32 texName);
    void free_tex_images(U32 count, const U32* texNames);
    void free_cur_tex_image();

    // [Viewer Lab] The accounting's key for a GL name: the render interface's 64-bit id of the texture that owns the
    // name, or the raw name with the top bit set, which no id reaches (ids count from 1). Any thread.
    constexpr U64 RAW_NAME_KEY = U64(1) << 63;
    U64 texture_key(U32 texName);
}

//============================================================================
class LLImageGL : public LLRefCount
{
    friend class LLTexUnit;
public:

    // call once per frame
    static void updateClass();
    // <Luminous> deletes every texture waiting to be deleted, now
    static void flushDeletedTextures();

    // Get an estimate of how many bytes have been allocated in vram for textures.
    // Does not include mipmaps.
    // NOTE: multiplying this number by two gives a good estimate for total
    // video memory usage based on testing in lagland against an NVIDIA GPU.
    static U64 getTextureBytesAllocated();

    // These 2 functions replace glGenTextures() and glDeleteTextures()
    static void generateTextures(S32 numTextures, U32 *textures);
    static void deleteTextures(S32 numTextures, const U32 *textures);
    // [Viewer Lab] The calling thread's generateTextures calls and names since it began, and the names its last call wrote,
    // in output order. A test aid; no GL.
    struct NameDraws
    {
        U64 calls = 0;
        U64 names = 0;
        std::vector<U32> lastBlock;
    };
    static NameDraws nameDraws();
    // [Viewer Lab] How many times a GL name waits in LLImageGL's deletion lists, from deleteTextures until updateClass
    // deletes it. Main thread; for the tests.
    static U32 pendingDeletions(U32 texName);

    // Size calculation
    static S32 dataFormatBits(S32 dataformat);
    static S64 dataFormatBytes(S32 dataformat, S32 width, S32 height);
    static S32 dataFormatComponents(S32 dataformat);

    bool updateBindStats() const ;
    F32 getTimePassedSinceLastBound();
    void forceUpdateBindStats(void) const;

    // needs to be called every frame
    static void updateStats(F32 current_time);

    // cleanup GL state
    static void destroyGL();
    static void dirtyTexOptions();

    static bool checkSize(S32 width, S32 height);

    //for server side use only.
    // Not currently necessary for LLImageGL, but required in some derived classes,
    // so include for compatability
    static bool create(LLPointer<LLImageGL>& dest, bool usemipmaps = true);
    static bool create(LLPointer<LLImageGL>& dest, U32 width, U32 height, U8 components, bool usemipmaps = true);
    static bool create(LLPointer<LLImageGL>& dest, const LLImageRaw* imageraw, bool usemipmaps = true);

public:
    LLImageGL(bool usemipmaps = true, bool allow_compression = true);
    LLImageGL(U32 width, U32 height, U8 components, bool usemipmaps = true, bool allow_compression = true);
    LLImageGL(const LLImageRaw* imageraw, bool usemipmaps = true, bool allow_compression = true);
    // [Viewer Lab] No constructor wraps a GL name made elsewhere: load_exr makes its image with createFloatImage, and
    // nothing wraps a raw GL name

protected:
    virtual ~LLImageGL();

    void analyzeAlpha(const void* data_in, U32 w, U32 h);
    void calcAlphaChannelOffsetAndStride();

public:
    virtual void dump();    // debugging info to LL_INFOS()

    bool setSize(S32 width, S32 height, S32 ncomponents, S32 discard_level = -1);
    void setComponents(S32 ncomponents) { mComponents = (S8)ncomponents ;}
    void setAllowCompression(bool allow) { mAllowCompression = allow; }

    static void setManualImage(U32 target, S32 miplevel, S32 intformat, S32 width, S32 height, U32 pixformat, U32 pixtype, const void *pixels, bool allow_compression = true);

    // [Viewer Lab] The render interface's description of a texture of this type and usage with these facade formats: the
    // GL format setManualImage gives GL (after its conversion of GL's alpha and luminance formats and its compression), or
    // the DXT format setImage gives glCompressedTexImage2D, as TextureDesc::glFormat, its format from the one table, and
    // the swizzle mask setManualImage sets. usage is LL::RHI::TextureUsage's bits. False for a type other than TT_TEXTURE
    // and TT_CUBE_MAP, or when the table has no format for it.
    static bool describeTexture(LL::RHI::TextureDesc& desc, LLTexUnit::eTextureType type, U8 usage, S32 intformat,
                                U32 pixformat, U32 pixtype, bool allow_compression, S32 width, S32 height, U32 mip_levels);

    // [Viewer Lab] A texture of the viewer's render device with storage and no data, made as LLImageGL makes its own: the
    // name from LLImageGL's pool, bound on texture unit 0 with bindManual, the level range when max_level is 0 or more, and
    // setManualImage with no pixels for level 0 of each face as the GL storage. It stays bound on unit 0, where the caller
    // sets its sampling, and letting go of it hands its name to LLImageGL's deletion lists. Null, with no GL call, without
    // the viewer's device, off the main thread, while GL is disabled, for a type other than TT_TEXTURE and TT_CUBE_MAP, and
    // for a description the table or the device refuses; the caller then keeps its raw path. For LLRenderTarget's
    // attachments; cube maps use createOwnTexture. Main thread. A caller that holds the result includes llrhi.h.
    static LLPointer<LL::RHI::Texture> createEmptyTexture(LLTexUnit::eTextureType type, S32 intformat, U32 pixformat,
                                                          U32 pixtype, S32 width, S32 height, U8 usage, S32 max_level = -1,
                                                          bool allow_compression = false);
    // [Viewer Lab] count textures of the viewer's render device described at the caller's size, formats and usage with
    // mip_levels levels (one by default), whose names come from one generateTextures(count) in the block order
    // (texture i gets the i-th name), and no other GL call; the caller binds each with bindTexture and gives it its data
    // level by level with specifyTexture, and allow_compression is what it then gives specifyTexture. False, with textures
    // empty and no GL call, without the viewer's device, off the main thread, while GL is disabled, for a type other than
    // TT_TEXTURE, and for a description the table or the device's capabilities refuse (one RHI WARN), such as more levels
    // than the size has. For the pipeline's textures without an image and the grid's levels.
    // Main thread. A caller that holds the results includes llrhi.h.
    static bool createTextureNames(std::vector<LLPointer<LL::RHI::Texture>>& textures, U32 count,
                                   LLTexUnit::eTextureType type, S32 intformat, U32 pixformat, U32 pixtype, S32 width,
                                   S32 height, U8 usage, bool allow_compression, U32 mip_levels = 1);
    // [Viewer Lab] setManualImage(GL_TEXTURE_2D, level, ...) with the caller's arguments on the texture the caller has bound
    // on GL's active unit; with pixels, on the main thread with the viewer's device, the level first goes to the device as
    // an upload (on GL only tracked). allow_compression's default is setManualImage's.
    static void specifyTexture(LL::RHI::Texture* texture, S32 level, S32 intformat, S32 width, S32 height, U32 pixformat,
                               U32 pixtype, const void* pixels, bool allow_compression = true);
    // [Viewer Lab] One texture of createTextureNames, bound on texture unit 0 with bindTexture (bindManual's GL, told to the
    // encoder), with level 0's data through specifyTexture and no level range: the generateTextures(1), bindManual on
    // unit 0 and setManualImage. It stays bound on unit 0, where the caller sets its sampling, as createEmptyTexture's.
    // Null, with no GL call, where createTextureNames is false. Main thread.
    static LLPointer<LL::RHI::Texture> createTextureWithData(LLTexUnit::eTextureType type, S32 intformat, U32 pixformat,
                                                             U32 pixtype, S32 width, S32 height, U8 usage,
                                                             const void* pixels, bool allow_compression = false);
    // [Viewer Lab] load_exr's HDRI image (llreflectionmapmanager.cpp): rgba's width x height floats as GL_RGB16F with a
    // full chain of mipmaps, clamped and trilinear, at any size, also one that is not a power of two (setSize refuses
    // those and leaves 0 x 0). With the viewer's device the image owns a texture of the device at that size on one name
    // from the pool; without it the raw name and setManualImage. Unit 0 is unbound after it. Main thread.
    static LLPointer<LLImageGL> createFloatImage(S32 width, S32 height, const F32* rgba);

    // [Viewer Lab] This image's name as a texture of the viewer's render device that the image owns: the one name
    // from LLImageGL's pool and no other GL call, so the caller makes the storage, the uploads and the options as before.
    // Described with the type, these facade formats, the usage and mip_levels at the image's full size; a
    // TT_CUBE_MAP_ARRAY texture is a cube map array of `layers` cubes. True when the texture is made. False, with a raw
    // name from the pool set as setTexName sets it, as before, without the viewer's device, off the main thread, for an
    // empty TT_CUBE_MAP_ARRAY (no layers), and for a description the table or the device refuses. For LLCubeMap and
    // LLCubeMapArray; main thread.
    bool createOwnTexture(LLTexUnit::eTextureType type, S32 intformat, U32 pixformat, U32 pixtype, U8 usage, U32 mip_levels,
                          U32 layers = 1, bool allow_compression = true);
    // [Viewer Lab] The GL name of `first`, and its texture of the viewer's render device when `first` owns the name, for an
    // image with no name of its own: LLCubeMap's other faces on the first face's cube, whose uploads go to their
    // layers.
    void shareTexture(const LLImageGL& first);

    // [Viewer Lab] An empty texture of the viewer's device at a new size with its old description, for LLRenderTarget's
    // resize: the new texture adopts the GL name with no GL call, and the caller's setManualImage specifies GL's as before;
    // off GL (no names) the accounting moves to the new texture at the new size. A width or height of 0 or above the
    // device's maxTextureSize keeps the old texture without asking the device. Gives the texture held afterwards, or null
    // when the device refuses the description (main thread). mip_levels above 0 replaces the description's level
    // count, for a TMG_AUTO target's full chain at the new size.
    static LL::RHI::Texture* respecifyEmptyTexture(LLPointer<LL::RHI::Texture>& texture, U32 name, S32 width, S32 height,
                                                   U32 mip_levels = 0);
    // [Viewer Lab] createEmptyTexture's level range, 0 to max_level, on the texture the caller has bound on unit 0 with
    // type: for LLRenderTarget's resize of a TMG_AUTO target, whose new size has its own chain. No GL call when GL is not
    // the viewer's backend.
    static void setLevelRange(LLTexUnit::eTextureType type, S32 max_level);

    // [Viewer Lab] For the render interface's GL backend (llrhigl.cpp): LLImageGL's own textures on the viewer's
    // device
    /// True on this thread while LLImageGL creates its texture through the device: the main thread, or the upload
    /// thread for createGLTexture. The GL backend then takes only a name. Per thread (thread_local); the scope covers
    /// create_rhi_texture's one createTexture call, which never comes back into it, so it clears on exit instead of
    /// nesting. A GL backend switch, its contract with llrender (llrhigl.cpp).
    static bool makesOwnGLStorage();
    /// A name from generateTextures, recorded with the texture's id for the accounting.
    static U32 allocateRHITextureName(U64 id);
    /// The name of the texture setImage re-specifies with another description, for the new texture made on this thread
    /// to replace it; the registry and the accounting move to the new id. 0 when no such adoption is pending.
    static U32 adoptRHITextureName(U64 id);
    /// Hands the name to the deletion lists, unless they have it already.
    static void freeRHITextureName(U32 name, U64 id);

    // [Viewer Lab] LLImageGL's uploads and mipmaps through the viewer's render device: the encoder on the main thread, the
    // upload context on any other
    /// True on this thread while LLImageGL sends its own upload or mipmaps to the device; the GL backend then only tracks
    /// the call, because LLImageGL makes its GL calls itself.
    static bool makesOwnGLUploads();
    /// That scope, for LLImageGL's own calls and the tests. It puts the previous value back, so it nests; per thread
    /// (thread_local), for the upload thread too. A GL backend switch, its contract with llrender (llrhigl.cpp).
    struct OwnGLUploads
    {
        OwnGLUploads();
        ~OwnGLUploads();
        OwnGLUploads(const OwnGLUploads&) = delete;
        OwnGLUploads& operator=(const OwnGLUploads&) = delete;

    private:
        const bool mPrevious;
    };
    // [Viewer Lab] Readbacks and copies through the viewer's render device
    /// True on this thread while LLGPUReadback or LLGPUCopy sends a readback or a copy whose GL call it makes itself; the
    /// GL backend then only tracks it.
    static bool makesOwnGLTransfers();
    /// That scope, for LLGPUReadback's and LLGPUCopy's own calls and the tests. It puts the previous value back, so it
    /// nests; per thread (thread_local). A GL backend switch, its contract with llrender (llrhigl.cpp).
    struct OwnGLTransfers
    {
        OwnGLTransfers();
        ~OwnGLTransfers();
        OwnGLTransfers(const OwnGLTransfers&) = delete;
        OwnGLTransfers& operator=(const OwnGLTransfers&) = delete;

    private:
        const bool mPrevious;
    };
    /// What LLImageGL has sent to the device since the process began, through either recorder: uploads, mipmaps, the
    /// calls of both through the upload context, setImage calls left untracked because the image's new description
    /// cannot be made or the device refuses it, and re-specifications into a new texture on the same name. Any thread;
    /// for level 1's per-scene lines and the tests.
    struct DeviceCounts
    {
        U64 uploads = 0;
        U64 mipmaps = 0;
        U64 untracked = 0;
        U64 respecifications = 0;
        U64 uploadContext = 0;
    };
    static DeviceCounts deviceCounts();
    /// One call in the tests' log: kind 'u' an upload, 'm' mipmaps, 'x' an untracked setImage, 'r' a re-specification;
    /// texture is the texture's id, for an 'r' the new texture's; 'f' the upload context's fence syncToMainThread
    /// submitted, with its debug name in name, only logged, never counted.
    struct DeviceCall
    {
        char kind = 0;
        U64 texture = 0;
        U32 level = 0;
        U32 layer = 0;
        U32 x = 0;
        U32 y = 0;
        U32 width = 0;
        U32 height = 0;
        U32 rowLength = 0;
        bool swapBytes = false;
        U64 bytes = 0;
        bool uploadContext = false;
        std::string name;
    };
    /// Tests only: every call is appended to the log until it is set to null again; null in the viewer.
    static void setDeviceCallLog(std::vector<DeviceCall>* log);
    /// Encoder::generateMipmaps, or UploadContext::generateMipmaps off the main thread, for a texture of the device, then
    /// glGenerateMipmap on the target the caller has bound, also without a texture.
    static void generateMipmaps(LL::RHI::Texture* texture, U32 target);

    bool createGLTexture() ;
    // [Viewer Lab] createGLTexture()'s empty texture as a texture of the viewer's render device that the image owns: the
    // same steps (GL disabled: a WARN and false; the previous name released as releaseTexName releases it), with the name
    // taken from one device texture described from the image as it is now (its formats, getWidth(0) x getHeight(0),
    // allow_compression && the image's own setting, which is what the caller then gives specifyOwnLevel) with the
    // caller's usage and mip_levels, through the one generateTextures(1). No other GL call: the caller binds the
    // image and specifies its levels with specifyOwnLevel. A description the table or the device refuses, an image that
    // is not a 2D texture, or no device: the raw name from the pool (true, getRHITexture() null), as
    // createOwnTexture's fallback. Off GL the image owns its texture without a GL name, and that counts as made. False
    // when neither a name nor an owned texture results. Main thread.
    bool createEmptyOwnTexture(U8 usage, U32 mip_levels, bool allow_compression);
    // [Viewer Lab] One level of the image createEmptyOwnTexture made, on the image the caller has bound on the active
    // unit: specifyTexture on its own texture of the viewer's render device (with pixels a tracked upload first), else
    // setManualImage(GL_TEXTURE_2D, ...) as before. Main thread.
    void specifyOwnLevel(S32 level, S32 intformat, S32 width, S32 height, U32 pixformat, U32 pixtype, const void* pixels,
                         bool allow_compression = true);
    bool createGLTexture(S32 discard_level, const LLImageRaw* imageraw, S32 usename = 0, bool to_create = true,
        S32 category = sMaxCategories-1, bool defer_copy = false, LLImageGLHandle* tex_name = nullptr); // [Viewer Lab] a handle
    bool createGLTexture(S32 discard_level, const U8* data, bool data_hasmips = false, S32 usename = 0, bool defer_copy = false, LLImageGLHandle* tex_name = nullptr);
    void setImage(const LLImageRaw* imageraw);
    bool setImage(const U8* data_in, bool data_hasmips = false, S32 usename = 0);
    // *TODO: This function may not work if the textures is compressed (i.e.
    // RenderCompressTextures is 0). Partial image updates do not work on
    // compressed textures.
    bool setSubImage(const LLImageRaw* imageraw, S32 x_pos, S32 y_pos, S32 width, S32 height, bool force_fast_update = false, LLImageGLHandle use_name = LLImageGLHandle()); // [Viewer Lab] a handle
    bool setSubImage(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height, bool force_fast_update = false, LLImageGLHandle use_name = LLImageGLHandle());
    bool setSubImageFromFrameBuffer(S32 fb_x, S32 fb_y, S32 x_pos, S32 y_pos, S32 width, S32 height);

    // wait for gl commands to finish on current thread and push
    // a lambda to main thread to swap mNewTexName and mTexName
    void syncToMainThread(LLImageGLHandle new_tex_name); // [Viewer Lab] a handle
    /// [Viewer Lab] What the swaps of a new name have done since the process began: syncToMainThread's calls
    /// (threadSyncs); every swap of a name, from syncTexName and from the upload thread's swap item (swaps); the swaps
    /// among those that also gave the image a new texture of the viewer's render device (handleSwaps); the upload
    /// context's fences that syncToMainThread submitted (fences) and the swap items that waited for theirs before the
    /// swap (fenceWaits); and the textures createGLTexture parked with defer_copy that syncTexName or syncToMainThread
    /// took for a media update's swap (deferredSwaps). Any thread; for golden level 2's render profile and the tests.
    /// Each one counter: also the syncs from the upload thread with the viewer's device whose submit() gave no fence
    /// (fencelessSyncs), the handle swaps the upload thread's swap items made (threadHandleSwaps) and the deferred
    /// textures syncToMainThread took (deferredThreadSwaps); fenceWaits is read before fences, so it is never above them,
    /// also while a sync runs. Information for golden level 2: the microseconds the main thread spent in createGLTexture
    /// with an image (mainCreateUs) and in the swap items' handover of the fence, the encoder's waitForFence with the
    /// mips the upload thread asked for (swapMipUs). deferredSwaps counts every media update that landed at its
    /// caller's swap point: off GL a main-thread update that createGLTexture
    /// made in the image's own texture, with no texture to swap in, counts there too, and apart in inPlaceUpdates (never
    /// on GL, whose names are never 0); swaps and handleSwaps do not move for it.
    struct SyncCounts
    {
        U64 threadSyncs = 0;
        U64 swaps = 0;
        U64 handleSwaps = 0;
        U64 fences = 0;
        U64 fenceWaits = 0;
        U64 deferredSwaps = 0;
        U64 fencelessSyncs = 0;
        U64 threadHandleSwaps = 0;
        U64 deferredThreadSwaps = 0;
        U64 mainCreateUs = 0;
        U64 swapMipUs = 0;
        U64 inPlaceUpdates = 0; // [Viewer Lab]
    };
    static SyncCounts syncCounts();

    // Read back a raw image for this discard level, if it exists
    bool readBackRaw(S32 discard_level, LLImageRaw* imageraw, bool compressed_ok) const;
    void destroyGLTexture();
    void forceToInvalidateGLTexture();

    void setExplicitFormat(LLGLint internal_format, LLGLenum primary_format, LLGLenum type_format = 0, bool swap_bytes = false);
    void setComponents(S8 ncomponents) { mComponents = ncomponents; }

    S32  getDiscardLevel() const        { return mCurrentDiscardLevel; }
    S32  getMaxDiscardLevel() const     { return mMaxDiscardLevel; }

    // override the current discard level
    // should only be used for local textures where you know exactly what you're doing
    void setDiscardLevel(S32 level) { mCurrentDiscardLevel = level; }

    S32  getCurrentWidth() const { return mWidth ;}
    S32  getCurrentHeight() const { return mHeight ;}
    S32  getWidth(S32 discard_level = -1) const;
    S32  getHeight(S32 discard_level = -1) const;
    U8   getComponents() const { return mComponents; }
    S64  getBytes(S32 discard_level = -1) const;
    S64  getMipBytes(S32 discard_level = -1) const;
    bool getBoundRecently() const;
    bool isJustBound() const;
    bool getHasExplicitFormat() const { return mHasExplicitFormat; }
    LLGLenum getPrimaryFormat() const { return mFormatPrimary; }
    LLGLenum getFormatType() const { return mFormatType; }

    bool getHasGLTexture() const { return mTexName != 0; }
    // [Viewer Lab] Whether the image holds a texture on the current backend: its GL name (on GL), or off GL its own
    // texture of the render device, which has no GL name. Callers that ask "is there a texture" use this,
    // not getHasGLTexture(), which is false for every texture off GL. On GL the two agree.
    bool hasTexture() const;
    // [Viewer Lab] The texture of the viewer's render device whose GL name getTexName() gives, or null when the name is a
    // raw GL name (made without the device, by a caller, or swapped in as a raw name by syncTexName).
    LL::RHI::Texture* getRHITexture() const;
    // [Viewer Lab] A 64-bit key for caches of what this image's texture draws (LLUIImage's display lists), in two regimes.
    // With a GL name (always on GL) equal keys mean the same GL name: set when the name changes, the id of the texture of
    // the viewer's render device that the image then owns for the name, or LLImageGLMemory::RAW_NAME_KEY | name for a raw
    // name; a re-specification in place keeps it, as it keeps the name. Without one, the id of the image's own texture
    // that has no GL name (off GL), which changes with that texture, also at a re-specification; else 0. A member
    // read: no lock and no GL. Main thread, as getTexName.
    U64 getTextureKey() const { return mTextureKey; }

    bool getIsAlphaMask() const;

    bool getIsResident(bool test_now = false); // not const

    void setTarget(const LLGLenum target, const LLTexUnit::eTextureType bind_target);

    LLTexUnit::eTextureType getTarget(void) const { return mBindTarget; }
    bool isGLTextureCreated(void) const { return mGLTextureCreated ; }
    void setGLTextureCreated (bool initialized) { mGLTextureCreated = initialized; }

    bool getUseMipMaps() const { return mUseMipMaps; }
    void setUseMipMaps(bool usemips) { mUseMipMaps = usemips; }
    void setHasMipMaps(bool hasmips) { mHasMipMaps = hasmips; }
    void updatePickMask(S32 width, S32 height, const U8* data_in);
// [RLVa:KB] - Checked: RLVa-2.2 (@setoverlay)
    bool getMask(const LLVector2 &tc) const;
// [/RLVa:KB]
//  bool getMask(const LLVector2 &tc);

    void checkTexSize(bool forced = false) const ;

    // Sets the addressing mode used to sample the texture
    //  (such as wrapping, mirrored wrapping, and clamp)
    // Note: this actually gets set the next time the texture is bound.
    void setAddressMode(LLTexUnit::eTextureAddressMode mode);
    LLTexUnit::eTextureAddressMode getAddressMode(void) const { return mAddressMode; }

    // Sets the filtering options used to sample the texture
    //  (such as point sampling, bilinear interpolation, mipmapping, and anisotropic filtering)
    // Note: this actually gets set the next time the texture is bound.
    void setFilteringOption(LLTexUnit::eTextureFilterOptions option);
    LLTexUnit::eTextureFilterOptions getFilteringOption(void) const { return mFilterOption; }

    LLGLenum getTexTarget()const { return mTarget; }

    void init(bool usemipmaps, bool allow_compression);
    virtual void cleanup(); // Clean up the LLImageGL so it can be reinitialized.  Be careful when using this in derived class destructors

    void setNeedsAlphaAndPickMask(bool need_mask);

#if LL_IMAGEGL_THREAD_CHECK
    // thread debugging
    std::thread::id mActiveThread;
    void checkActiveThread();
#endif

    // scale down to the desired discard level using GPU
    // returns true if texture was scaled down
    // desired discard will be clamped to max discard
    // if desired discard is less than or equal to current discard, no scaling will occur
    // only works for GL_TEXTURE_2D target
    // [Viewer Lab] screen_triangle is the caller's full-screen triangle with gCopyProgram and its target bound: drawn
    // through it, the draw reaches the viewer's encoder; without it GL draws as before and another backend returns false.
    // False with nothing changed also when the copy into the smaller texture cannot be made or the draw was not
    // delivered.
    bool scaleDown(S32 desired_discard, LLVertexBuffer* screen_triangle = nullptr);

public:
    // Various GL/Rendering options
    S64Bytes mTextureMemory;
    mutable F32  mLastBindTime; // last time this was bound, by discard level

private:
    // [Viewer Lab] The GL name is GL-backend state inside llrender, read outside the class through GLNames::of
    friend U32 LL::RHI::GLNames::of(const LLImageGL* image);
    LLGLuint getTexName() const { return mTexName; }

    U32 createPickMask(S32 pWidth, S32 pHeight);
    void freePickMask();
    bool isCompressed();

    LLPointer<LLImageRaw> mSaveData; // used for destroyGL/restoreGL
    LL::WorkQueue::weak_t mMainQueue;
    U8* mPickMask;  //downsampled bitmap approximation of alpha channel.  NULL if no alpha channel
    U16 mPickMaskWidth;
    U16 mPickMaskHeight;
    S8 mUseMipMaps;
    bool mHasExplicitFormat; // If false (default), GL format is f(mComponents)
    bool mAutoGenMips = false;

    bool mIsMask;
    bool mNeedsAlphaAndPickMask;
    S8   mAlphaStride ;
    S8   mAlphaOffset ;

    bool     mGLTextureCreated ;
    LLGLuint mTexName;
    U16      mWidth;
    U16      mHeight;
    S8       mCurrentDiscardLevel;

    bool mAllowCompression;

protected:
    LLGLenum mTarget;       // Normally GL_TEXTURE2D, sometimes something else (ex. cube maps)
    LLTexUnit::eTextureType mBindTarget;    // Normally TT_TEXTURE, sometimes something else (ex. cube maps)
    bool mHasMipMaps;
    S32 mMipLevels;

    LLGLboolean mIsResident;

    S8 mComponents;
    S8 mMaxDiscardLevel;

    bool    mTexOptionsDirty;
    LLTexUnit::eTextureAddressMode      mAddressMode;   // Defaults to TAM_WRAP
    LLTexUnit::eTextureFilterOptions    mFilterOption;  // Defaults to TFO_ANISOTROPIC

    LLGLint  mFormatInternal; // = GL internalformat
    LLGLenum mFormatPrimary;  // = GL format (pixel data format)
    LLGLenum mFormatType;
    bool     mFormatSwapBytes;// if true, use glPixelStorei(GL_UNPACK_SWAP_BYTES, 1)

    bool mExternalTexture;

    // STATICS
public:
    static std::unordered_set<LLImageGL*> sImageList;
    static S32 sCount;
    static U32 sFrameCount;
    static F32 sLastFrameTime;

    // Global memory statistics
    static U32 sBindCount;                  // Tracks number of texture binds for current frame
    static U32 sUniqueCount;                // Tracks number of unique texture binds for current frame
    static bool sGlobalUseAnisotropic;
    // [Viewer Lab] scaleDown's calls that scaled a texture down, and those that changed nothing past its early returns
    // because the copy could not be made, the draw was not delivered or the device refused the re-specification; main
    // thread, for the golden hook and the tests
    static U32 sScaleDowns;
    static U32 sScaleDownsRefused;
    static LLImageGL* sDefaultGLTexture ;
    static bool sAutomatedTest;
    static bool sCompressTextures;          //use GL texture compression
#if DEBUG_MISS
    bool mMissed; // Missed on last bind?
    bool getMissed() const { return mMissed; };
#else
    bool getMissed() const { return false; };
#endif

public:
    static void initClass(LLWindow* window, S32 num_catagories, bool skip_analyze_alpha = false, bool thread_texture_loads = false, bool thread_media_updates = false);
    static void allocateConversionBuffer();
    static void cleanupClass() ;

private:
    static S32 sMaxCategories;
    static bool sSkipAnalyzeAlpha;
    static U32 sScratchPBO;
    static U32 sScratchPBOSize;
    static U32* sManualScratch;

    //the flag to allow to call readBackRaw(...).
    //can be removed if we do not use that function at all.
    static bool sAllowReadBackRaw ;
//
//****************************************************************************************************
//The below for texture auditing use only
//****************************************************************************************************
private:
    S32 mCategory ;
public:
    void setCategory(S32 category) {mCategory = category;}
    S32  getCategory()const {return mCategory;}

    void setTexName(GLuint texName); // [Viewer Lab] defined in llimagegl.cpp

    //similar to setTexName, but will call deleteTextures on mTexName if mTexName is not 0 or texname
    void syncTexName(LLImageGLHandle texname); // [Viewer Lab] a handle

    //for debug use: show texture size distribution
    //----------------------------------------
    static S32 sCurTexSizeBar ;
    static S32 sCurTexPickSize ;

    static void setCurTexSizebar(S32 index, bool set_pick_size = true) ;
    static void resetCurTexSizebar();

//****************************************************************************************************
//End of definitions for texture auditing use only
//****************************************************************************************************

private:
    // [Viewer Lab]
    void dropReplacedRHITexture();
    void releaseTexName(U32 name);
    // [Viewer Lab] getTextureKey's update where mTexName may have changed from previous_name, or mRHITexture changed: with
    // a GL name nothing when it is still previous_name (a re-specification in place keeps the key), else the id of owner,
    // the texture of the viewer's render device the caller knows owns the new name, or the raw key without one; without a
    // GL name the id of the image's own texture that has none, else 0. No lock, no registry, no GL.
    void updateTextureKey(LLGLuint previous_name, const LL::RHI::Texture* owner);

    // [Viewer Lab] syncToMainThread with the texture of the viewer's render device createGLTexture has made on the
    // upload thread for the new name, or null, which it takes over: the swap item gives the image that texture as its
    // handle once submit()'s fence has signalled; and the swap itself on the main thread, which syncTexName makes with
    // the texture parked for the name, or none
    void syncToMainThread(LLGLuint new_tex_name, LLPointer<LL::RHI::Texture>&& new_texture);
    bool swapTexName(LLGLuint texname, const LLPointer<LL::RHI::Texture>& texture);

    // [Viewer Lab] The texture of the viewer's render device that createGLTexture made with defer_copy, parked in
    // mNewRHITexture until the caller's syncTexName or syncToMainThread takes it by its GL name: park keeps a still-parked
    // other texture unreleased and parks nothing for null; take gives the parked texture, clears it and counts it in
    // deferredSwaps when the name is its GL name, otherwise null; newRHITexture gives it, for setSubImage, when the name
    // is its nonzero GL name
    void parkNewRHITexture(const LLPointer<LL::RHI::Texture>& texture);
    LLPointer<LL::RHI::Texture> takeNewRHITexture(LLGLuint name);
    LL::RHI::Texture* newRHITexture(LLGLuint name) const;
    // [Viewer Lab] take's and newRHITexture's test: the name is the parked texture's GL name; off GL, where the textures
    // have no GL name and defer_copy gives the caller the name 0, the name 0 matches a parked texture without one
    bool parkedFor(LLGLuint name) const;

    // [Viewer Lab] setImage's body, with the texture of the viewer's render device createGLTexture has just made for the
    // call, or null; and the description createGLTexture gives a texture of the image as it is now
    bool setImage(const U8* data_in, bool data_hasmips, S32 usename, LL::RHI::Texture* texture);
    bool describeCurrentTexture(LL::RHI::TextureDesc& desc, U8 usage) const;
    // [Viewer Lab] scaleDown's re-specification of the image's own texture at desired_discard's size, right before its
    // glTexImage2D; the texture the rest of the call uses, or null
    LL::RHI::Texture* respecifyForScale(S32 desired_discard);

    // [Viewer Lab] The texture of the viewer's render device that owns mTexName, or null. mTexName is its GL name
    // (LL::RHI::GLNames::of), the GL backend's state. Off GL the image's own texture, which has no GL name; every write
    // of it is followed by updateTextureKey. After the existing members, so their offsets stay where code compiled
    // against the old header reads them; the class grows by one pointer.
    LLPointer<LL::RHI::Texture> mRHITexture;
    // [Viewer Lab] The texture createGLTexture made with defer_copy for its caller's later swap, or null. Only the thread
    // running the image's update touches it, until syncToMainThread or syncTexName takes it there.
    LLPointer<LL::RHI::Texture> mNewRHITexture;
    // [Viewer Lab] getTextureKey's value; written with mTexName or mRHITexture, on their thread. After the
    // existing members, as mRHITexture; the class grows by 8 bytes.
    U64 mTextureKey = 0;
    // [Viewer Lab] createGLTexture with defer_copy updated the image's own texture in place, which the caller's syncTexName
    // counts (takeNewRHITexture) and clears; destroyGLTexture and a parked texture clear it too. Main thread only, off GL
    // only. After the existing members, as mTextureKey.
    bool mInPlacePending = false;
};

// [Viewer Lab] Readbacks through the viewer's render device, in GL's terms: the caller's GL format and type, into the
// caller's memory, which must outlive wait(); rows in GL's order, never flipped. Each read
// goes to the encoder first, as a readback of the texture GL reads (LLRenderTarget's bound target's colour attachment 0 or
// depth, else the window's back buffer or depth; for readTexture the caller's texture), then GL makes the call in a
// GL backend function. On GL the data is read at once, so ready() is true and wait() copies nothing; another backend's
// readback carries the data, which wait() copies into the caller's memory. Without the viewer's device (headless,
// llappearanceutility, the tests), off the main thread, for a raw name, for GL_ALPHA, GL_LUMINANCE and GL_LUMINANCE_ALPHA
// (upload layouts only) or for a region outside the texture, only the GL call is made, and counted. An object holds the
// encoder's readback until it is destroyed, moved from or reused; it never holds the device or a texture. Copies take
// device textures as raw pointers valid for the call, or null for a raw name. Each read and copy returns whether it
// delivered: always on GL, which makes the call whatever the encoder saw; on another backend only when the encoder
// tracked it, and a readback that delivered nothing has an empty data(), as has another backend's before wait() has
// delivered it.
class LLGPUReadback
{
public:
    LLGPUReadback(); // defined where LL::RHI::Readback is complete, as the other special members
    ~LLGPUReadback();
    LLGPUReadback(const LLGPUReadback&) = delete;
    LLGPUReadback& operator=(const LLGPUReadback&) = delete;
    LLGPUReadback(LLGPUReadback&&) noexcept;
    LLGPUReadback& operator=(LLGPUReadback&&) noexcept;

    /// The current framebuffer's region, as glReadPixels reads it: GL_DEPTH_COMPONENT reads the depth.
    [[nodiscard]] bool readFramebuffer(S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* destination,
                                       size_t bytes);
    /// The whole level of the texture the caller has bound on target, as glGetTexImage, with the size the caller knows;
    /// texture is that texture of the device, or null for a raw name.
    [[nodiscard]] bool readTexture(LL::RHI::Texture* texture, U32 target, S32 level, S32 width, S32 height, U32 format,
                                   U32 type, void* destination, size_t bytes);
    /// glGetCompressedTexImage of the texture the caller has bound: GL only, counted untracked.
    void readCompressedTexture(U32 target, S32 level, void* destination);
    bool ready() const;
    void wait();
    std::span<const std::byte> data() const; ///< the caller's memory, as far as the readback fills it, once delivered
    size_t rowPitch() const; ///< the encoder's readback's row pitch, 0 without one; for tests
    /// Tests only: the encoder's readback this object holds, or null; and this object as another backend's tracked read
    /// leaves it, holding readback for the caller's memory, nothing delivered until wait().
    LL::RHI::Readback* readbackForTests() const;
    void adoptForTests(LL::RHI::Readback* readback, void* destination, size_t bytes, U32 row_bytes, U32 rows);
    /// wait()'s copy of another backend's data: rows rows of row_bytes each, pitch apart in data, packed into the
    /// destination's bytes, never past either; returns the bytes written.
    static size_t copyRows(std::byte* destination, size_t bytes, std::span<const std::byte> data, size_t pitch,
                           U32 row_bytes, U32 rows);

    /// readFramebuffer and readTexture, then wait(), for the synchronous call sites.
    [[nodiscard]] static bool readPixels(S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* destination,
                                         size_t bytes);
    [[nodiscard]] static bool readTexImage(LL::RHI::Texture* texture, U32 target, S32 level, S32 width, S32 height,
                                           U32 format, U32 type, void* destination, size_t bytes);
    /// rawSnapshot's reads of one tile: rows rows of the current framebuffer from row y up, width
    /// wide from x, read one row at a time in GL's order as glReadPixels reads them, row r into destination + r * stride,
    /// row_bytes of it; then it waits for them all, so another backend sends its work once for the tile, not once a row.
    /// Whether every row delivered.
    [[nodiscard]] static bool readFramebufferRows(S32 x, S32 y, S32 width, S32 rows, U32 format, U32 type,
                                                  void* destination, size_t stride, size_t row_bytes);
    /// glGetError once on GL; GL_NO_ERROR on another backend.
    static U32 takeError();
    /// Whether a target is read through its texture rather than with glReadPixels: renderMorphMasks' driver workaround,
    /// decided by a GL backend function.
    static bool targetsReadThroughTextures();
    /// That workaround's read: readTexImage of level 0 of the RGBA8 texture the caller has bound on
    /// GL_TEXTURE_2D, width x height as the caller knows it (texture is that texture of the device, or null for a raw
    /// name), into scratch's bytes, then takeError() into error. Only when it read without an error does it write the
    /// alpha of scratch's first pixels texels into alpha, one byte each. Returns whether it read: always on GL.
    [[nodiscard]] static bool readTextureAlpha(LL::RHI::Texture* texture, S32 width, S32 height, U8* scratch,
                                               size_t bytes, U8* alpha, size_t pixels, U32& error);

    /// The calls since the process began: tracked by the device's encoder, made by GL only (no device, off the main thread,
    /// a raw name, an upload-only or compressed format), made by GL only because the region is outside the texture, and
    /// refused by the encoder; and, under gDebugGL on GL, the tracked calls whose GL operands were not the tracked ones
    /// (the framebuffer and attachment GL reads, the texture bound on the destination's target). Any thread.
    struct Counts
    {
        U64 tracked = 0;
        U64 untracked = 0;
        U64 outside = 0;
        U64 refused = 0;
        U64 mismatched = 0;
    };
    static Counts counts();
    /// The operand mismatches of every transfer facade since the process began: the readbacks', LLGPUCopy's and the uploads'
    /// (LLImageGL's device uploads); the golden hook's measured.transfer_mismatches. Any thread.
    static U64 transferMismatches();
    /// counts() of the calling thread only, which another thread's reads do not move. The difference around a
    /// call holds every read the thread makes in it, also a frame's (renderMorphMasks', a sculpt's) that the call renders.
    static Counts threadCounts();

    /// A check's own readbacks: while a CountScope lives, the calls the same thread makes inside an
    /// OwnReads count into it, and no other call does. The check holds the scope around the call it checks, and that call
    /// holds an OwnReads around exactly its own reads, so a readback of a frame it renders in between does not count.
    /// Main thread or any other; the innermost scope counts.
    class CountScope
    {
    public:
        CountScope();
        ~CountScope();
        CountScope(const CountScope&) = delete;
        CountScope& operator=(const CountScope&) = delete;
        const Counts& counts() const { return mCounts; }

    private:
        Counts mCounts;
        Counts* mOuter;
    };
    /// Marks the calling thread's reads while it lives as its call's own, for a CountScope; nothing without one.
    class OwnReads
    {
    public:
        OwnReads();
        ~OwnReads();
        OwnReads(const OwnReads&) = delete;
        OwnReads& operator=(const OwnReads&) = delete;
    };

private:
    LLPointer<LL::RHI::Readback> mReadback;
    std::byte* mDestination = nullptr;
    size_t mBytes = 0;
    U32 mRowBytes = 0;
    U32 mRows = 0;
    bool mDelivered = false; ///< the caller's memory holds the read: at once on GL, after wait() elsewhere
};

// [Viewer Lab] GPU copies through the viewer's render device, in GL's terms: each copy goes to the
// encoder first, then GL makes the call, as LLGPUReadback's reads do, with the same fallbacks and counts.
class LLGPUCopy
{
public:
    /// glCopyTexSubImage2D from the framebuffer GL reads into the texture the caller has bound on target, or
    /// glCopyTexSubImage3D into layer (the cube map array's cube * 6 + face) when target is GL_TEXTURE_CUBE_MAP_ARRAY.
    /// texture is that texture of the device, or null for a raw name; source is the texture GL reads when the caller has
    /// made GL read something other than LLRenderTarget's current framebuffer, null for the current framebuffer.
    /// Whether it copied: always on GL; on another backend only when the encoder tracked the copy.
    [[nodiscard]] static bool fromFramebuffer(LL::RHI::Texture* texture, U32 target, S32 level, S32 layer, S32 dstX,
                                              S32 dstY, S32 x, S32 y, S32 width, S32 height,
                                              LL::RHI::Texture* source = nullptr);
    /// Whether a copy of a width x height region at the origin of LLRenderTarget's current framebuffer into level 0 of a
    /// texture of format (Unknown for a raw name) can be made, for a caller that must know before it changes anything
    /// (scaleDown): yes without the viewer's device on this thread, as on GL; otherwise when the framebuffer has a texture
    /// of the device, the region fits in it, and LL::RHI::copyRefusal takes its format and format (the one rule the
    /// Metal encoder's copyRefused asks too).
    [[nodiscard]] static bool canFromFramebuffer(LL::RHI::Format format, S32 width, S32 height);
    static LLGPUReadback::Counts counts();
};

class LLImageGLThread : public LLSimpleton<LLImageGLThread>, LL::ThreadPool
{
public:
    // follows gSavedSettings "RenderGLMultiThreadedTextures" ([Viewer Lab] on Metal also "MetalMultiThreadedTextures":
    // LLImageGL::initClass gets LL::RHI::uploadThreads' choice)
    static bool sEnabledTextures;
    // follows gSavedSettings "RenderGLMultiThreadedMedia" ([Viewer Lab] on Metal also "MetalMultiThreadedMedia")
    static bool sEnabledMedia;

    LLImageGLThread(LLWindow* window);

    // post a function to be executed on the LLImageGL background thread
    template <typename CALLABLE>
    bool post(CALLABLE&& func)
    {
        return getQueue().post(std::forward<CALLABLE>(func));
    }

    void run() override;

private:
    LLWindow* mWindow;
    void* mContext = nullptr;
    LLAtomicBool mFinished;
};

#endif // LL_LLIMAGEGL_H
