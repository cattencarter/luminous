/**
 * @file llrender.h
 * @brief LLRender definition
 *
 *  This class acts as a wrapper for OpenGL calls.
 *  The goal of this class is to minimize the number of api calls due to legacy rendering
 *  code, to define an interface for a multiple rendering API abstraction of the UI
 *  rendering, and to abstract out direct rendering calls in a way that is cleaner and easier to maintain.
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

#ifndef LL_LLGLRENDER_H
#define LL_LLGLRENDER_H

//#include "linden_common.h"

#include "v2math.h"
#include "v3math.h"
#include "v4coloru.h"
#include "v4math.h"
#include "llstrider.h"
#include "llpointer.h"
#include "llglheaders.h"
#include "llmatrix4a.h"
#include "glm/mat4x4.hpp"
#include <boost/align/aligned_allocator.hpp>

#include <array>
#include <list>
#include <tuple>
#include <vector>

class LLVertexBuffer;
class LLCubeMap;
class LLImageGL;
class LLRenderTarget;
class LLTexture;
class LLVertexBufferData;
namespace LL
{
    namespace RHI
    {
        class Device;
        class Encoder; // [Viewer Lab]
        class EncoderTracker; // [Viewer Lab]
        class Texture;
        struct SamplerDesc;
    }
}

#define LL_MATRIX_STACK_DEPTH 32

constexpr U32 LL_NUM_TEXTURE_LAYERS = 32;
constexpr U32 LL_NUM_LIGHT_UNITS = 8;

class LLTexUnit
{
    friend class LLRender;
    friend class LLImageGL; // [Viewer Lab] its sampling setters give their texture to the private setters
public:
    static U32 sWhiteTexture;

    typedef enum
    {
        TT_TEXTURE = 0,         // Standard 2D Texture
        TT_RECT_TEXTURE,        // Non power of 2 texture
        TT_CUBE_MAP,            // 6-sided cube map texture
        TT_CUBE_MAP_ARRAY,      // Array of cube maps
        TT_MULTISAMPLE_TEXTURE, // see GL_ARB_texture_multisample
        TT_TEXTURE_3D,          // standard 3D Texture
        TT_NONE,                // No texture type is currently enabled
    } eTextureType;

    typedef enum
    {
        TAM_WRAP = 0,           // Standard 2D Texture
        TAM_MIRROR,             // Non power of 2 texture
        TAM_CLAMP               // No texture type is currently enabled
    } eTextureAddressMode;

    typedef enum
    {   // Note: If mipmapping or anisotropic are not enabled or supported it should fall back gracefully
        TFO_POINT = 0,          // Equal to: min=point, mag=point, mip=none.
        TFO_BILINEAR,           // Equal to: min=linear, mag=linear, mip=point.
        TFO_TRILINEAR,          // Equal to: min=linear, mag=linear, mip=linear.
        TFO_ANISOTROPIC         // Equal to: min=anisotropic, max=anisotropic, mip=linear.
    } eTextureFilterOptions;

    typedef enum
    {
        TMG_NONE = 0,           // Mipmaps are not automatically generated for this texture.
        TMG_AUTO,               // Mipmaps are automatically generated for this texture.
        TMG_MANUAL              // Mipmaps are manually generated for this texture.
    } eTextureMipGeneration;

    typedef enum
    {
        TB_REPLACE = 0,
        TB_ADD,
        TB_MULT,
        TB_MULT_X2,
        TB_ALPHA_BLEND,
        TB_COMBINE          // Doesn't need to be set directly, setTexture___Blend() set TB_COMBINE automatically
    } eTextureBlendType;

    typedef enum
    {
        TBO_REPLACE = 0,            // Use Source 1
        TBO_MULT,                   // Multiply: ( Source1 * Source2 )
        TBO_MULT_X2,                // Multiply then scale by 2:  ( 2.0 * ( Source1 * Source2 ) )
        TBO_MULT_X4,                // Multiply then scale by 4:  ( 4.0 * ( Source1 * Source2 ) )
        TBO_ADD,                    // Add: ( Source1 + Source2 )
        TBO_ADD_SIGNED,             // Add then subtract 0.5: ( ( Source1 + Source2 ) - 0.5 )
        TBO_SUBTRACT,               // Subtract Source2 from Source1: ( Source1 - Source2 )
        TBO_LERP_VERT_ALPHA,        // Interpolate based on Vertex Alpha (VA): ( Source1 * VA + Source2 * (1-VA) )
        TBO_LERP_TEX_ALPHA,         // Interpolate based on Texture Alpha (TA): ( Source1 * TA + Source2 * (1-TA) )
        TBO_LERP_PREV_ALPHA,        // Interpolate based on Previous Alpha (PA): ( Source1 * PA + Source2 * (1-PA) )
        TBO_LERP_CONST_ALPHA        // Interpolate based on Const Alpha (CA): ( Source1 * CA + Source2 * (1-CA) )
    } eTextureBlendOp;

    typedef enum
    {
        TBS_PREV_COLOR = 0,         // Color from the previous texture stage
        TBS_PREV_ALPHA,
        TBS_ONE_MINUS_PREV_COLOR,
        TBS_ONE_MINUS_PREV_ALPHA,
        TBS_TEX_COLOR,              // Color from the texture bound to this stage
        TBS_TEX_ALPHA,
        TBS_ONE_MINUS_TEX_COLOR,
        TBS_ONE_MINUS_TEX_ALPHA,
        TBS_VERT_COLOR,             // The vertex color currently set
        TBS_VERT_ALPHA,
        TBS_ONE_MINUS_VERT_COLOR,
        TBS_ONE_MINUS_VERT_ALPHA,
        TBS_CONST_COLOR,            // The constant color value currently set
        TBS_CONST_ALPHA,
        TBS_ONE_MINUS_CONST_COLOR,
        TBS_ONE_MINUS_CONST_ALPHA
    } eTextureBlendSrc;

    typedef enum
    {
        TCS_LINEAR = 0,
        TCS_SRGB
    } eTextureColorSpace;

    LLTexUnit(S32 index = -1);

    // Refreshes renderer state of the texture unit to the cached values
    // Needed when the render context has changed and invalidated the current state
    void refreshState(void);

    // returns the index of this texture unit
    S32 getIndex(void) const { return mIndex; }

    // Sets this tex unit to be the currently active one
    void activate(void);

    // Enables this texture unit for the given texture type
    // (automatically disables any previously enabled texture type)
    void enable(eTextureType type);

    // Disables the current texture unit
    void disable(void);

    // Binds the LLImageGL to this texture unit
    // (automatically enables the unit for the LLImageGL's texture type)
    bool bind(LLImageGL* texture, bool for_rendering = false, bool forceBind = false, S32 usename = 0);
    bool bind(LLTexture* texture, bool for_rendering = false, bool forceBind = false);

    // bind implementation for inner loops
    // makes the following assumptions:
    //  - No need for gGL.flush()
    //  - texture is not null
    //  - gl_tex->getTexName() is not zero
    //  - This texture is not being bound redundantly
    //  - USE_SRGB_DECODE is disabled
    //  - mTexOptionsDirty is false
    //  -
    void bindFast(LLTexture* texture);

    // Binds a cubemap to this texture unit
    // (automatically enables the texture unit for cubemaps)
    bool bind(LLCubeMap* cubeMap);

    // Binds a render target to this texture unit
    // (automatically enables the texture unit for the RT's texture type)
    bool bind(LLRenderTarget * renderTarget, bool bindDepth = false);

    // Unbinds the currently bound texture of the given type
    // (only if there's a texture of the given type currently bound)
    void unbind(eTextureType type);

    // Fast but unsafe version of unbind
    void unbindFast(eTextureType type);

    // Sets the addressing mode used to sample the texture
    // Warning: this stays set for the bound texture forever,
    // make sure you want to permanently change the address mode  for the bound texture.
    void setTextureAddressMode(eTextureAddressMode mode);
    // MUST already be active and bound
    void setTextureAddressModeFast(eTextureAddressMode mode, eTextureType tex_type);

    // Sets the filtering options used to sample the texture
    // Warning: this stays set for the bound texture forever,
    // make sure you want to permanently change the filtering for the bound texture.
    void setTextureFilteringOption(LLTexUnit::eTextureFilterOptions option);
    // MUST already be active and bound
    void setTextureFilteringOptionFast(LLTexUnit::eTextureFilterOptions option, eTextureType tex_type);

    // [Viewer Lab] Facades for raw sampling calls: the raw glTexParameteri on the texture GL has bound for `type` on GL's
    // active unit, verbatim; no flush, no activate, no guard; never reads a unit's state. The filter and wrap setters also
    // set the facade slot of the device texture gGL's active unit holds for the type and tell the channels holding it again.
    static void setActiveTextureMagFilter(eTextureType type, U32 gl_mag);
    static void setActiveTextureMinFilter(eTextureType type, U32 gl_min);
    static void setActiveTextureWrapST(eTextureType type, U32 gl_wrap_s, U32 gl_wrap_t);
    // gl_func 0: COMPARE_FUNC is not set
    static void setActiveTextureCompare(eTextureType type, U32 gl_mode, U32 gl_func = 0);

    static U32 getInternalType(eTextureType type);

    U32 getCurrTexture(void) { return mCurrTexture; }

    eTextureType getCurrType(void) { return mCurrTexType; }

    void setHasMipMaps(bool hasMips) { mHasMipMaps = hasMips; }

    // [Viewer Lab] What LLTexUnit's bindings did on the main thread with the viewer's device since the process began:
    // plain counts written on the main thread only, as LLVertexBuffer::DrawCounts; a call on another thread
    // (LLImageGLThread's gGL) neither counts nor writes, so bindCounts() is read on the main thread with no lock. calls
    // are the entries of bind (all four), bindFast and bindTexture (both) of a unit, and bind(LLRenderTarget*)'s raw
    // binds; each ends in exactly one of binds (GL bound), skipped (the GL test said the same binding, adoptions
    // included), refused (no image, a cube map without cube maps) and nested (through bindDefaultImage or
    // sDefaultGLTexture's nested bind). adoptedNewId: a skip of the same GL name whose record had another id;
    // adoptedAfterRaw: a skip of the same GL name whose record had none (ended by unbind or a raw bind) while the
    // texture is known; rawBinds: GL binds and skips of a name with no texture of the device; fastRepeats: bindFast of
    // what the unit had told already; retold: a skip whose tell changed the unit's told record; samplingSets: the
    // setters' GL calls, samplingUntracked those whose texture the facade does not know (gGL's active unit's record and
    // the encoder's tracker, or the texture a bind or LLImageGL gives); unknownFilterParts and unknownAddressParts: the
    // parts of a tell's sampling neither the texture's slot nor its owner knew; unbindsToWhite: unbinds of the current
    // 2D type, whiteTold those that told the white image's texture; beyondChannels: tells refused at or above the
    // device's maxSamplersPerStage; encoded and encodedNull: the encoder calls with a texture and with none;
    // samplingResends: tells of a setter's changed sampling to the channels holding the texture; textureReads: the
    // reads of an image's device texture. For level 1, the golden hook and the tests.
    struct BindCounts
    {
        U64 calls = 0;
        U64 binds = 0;
        U64 skipped = 0;
        U64 refused = 0;
        U64 nested = 0;
        U64 adoptedNewId = 0;
        U64 adoptedAfterRaw = 0;
        U64 rawBinds = 0;
        U64 fastRepeats = 0;
        U64 retold = 0;
        U64 samplingSets = 0;
        U64 samplingUntracked = 0;
        U64 unknownFilterParts = 0;
        U64 unknownAddressParts = 0;
        U64 unbindsToWhite = 0;
        U64 whiteTold = 0;
        U64 beyondChannels = 0;
        U64 encoded = 0;
        U64 encodedNull = 0;
        U64 samplingResends = 0;
        U64 textureReads = 0;
    };
    static BindCounts bindCounts();

    // [Viewer Lab] True on this thread while LLTexUnit gives the encoder a binding it has made on GL itself: the GL
    // backend then only tracks it, with no flush and no GL, and keeps it apart from the interface's own channels.
    // Kept as a GL backend switch, its contract with llrender (llrhigl.cpp).
    static bool makesOwnGLBinds();
    // [Viewer Lab] That scope. It puts the previous value back, as LLGLSLShader's and LLVertexBuffer's do, so it nests;
    // per thread (thread_local).
    struct OwnGLBinds
    {
        OwnGLBinds();
        ~OwnGLBinds();
        OwnGLBinds(const OwnGLBinds&)            = delete;
        OwnGLBinds& operator=(const OwnGLBinds&) = delete;

    private:
        const bool mPrevious;
    };

    // [Viewer Lab] GL's values for a sampling: the one mapping from a SamplerDesc to GL's MIN_FILTER, MAG_FILTER,
    // MAX_ANISOTROPY and the wrap of every axis, the chain of setTextureFilteringOptionFast and
    // setTextureAddressModeFast. Pure: no GL call.
    struct GLSampling
    {
        S32 min;
        S32 mag;
        F32 anisotropy;
        S32 wrapS; // [Viewer Lab] each axis from its own address mode
        S32 wrapT;
        S32 wrapR;
    };
    static GLSampling toGLSampling(const LL::RHI::SamplerDesc& sampling);
    // [Viewer Lab] The GL adapter's sampling of an interface binding, on the texture bound on GL's active unit for the
    // type: the filter (with mips and anisotropy, MAX_ANISOTROPY only with the capability) and then the address mode, as
    // the two setters make them, with no flush and no encoder call. The GL backend only; nothing on another.
    static void applyGLSampling(eTextureType type, const LL::RHI::SamplerDesc& sampling);

    // [Viewer Lab] The binding interface by texture of the viewer's device. Both bindTexture forms make bindManual's GL
    // exactly (no flush when the GL test skips), decided by the GL name whenever the binding has one, and on the main
    // thread with the viewer's device give the encoder the binding by the texture's 64-bit id with the sampling the
    // facade remembers. No raw GL name passes through them; callers with only a raw name are llrender's, through
    // gl_bind_raw_name. The unit holds no reference: the encoder's tracker holds bound textures.
    // A texture of the device, its GL name from the texture; a null texture makes bindManual(type, 0)'s GL (texture 0 on
    // this unit, a raw binding of name 0, and the encoder told the unit holds nothing) and returns false.
    bool bindTexture(eTextureType type, LL::RHI::Texture* texture, bool hasMips = false);
    // An image's own GL name, its device texture (main thread) and its options for the sampling; with no device texture
    // a raw bind of its name. A null image binds nothing and returns false.
    bool bindTexture(eTextureType type, const LLImageGL* image, bool hasMips = false);
    // The binding's id while the record is valid (it was made on the main thread with the device and GL's binding is
    // still the one recorded); 0 after unbind, a raw bind or off the main thread.
    U64 getCurrTextureId() const;
    // The encoder's texture on this channel when its id is getCurrTextureId(); null off the main thread, without the
    // device, or raw. Valid while it stays bound; a caller that keeps it takes an LLPointer.
    LL::RHI::Texture* getCurrRHITexture() const;
    // Whether the image is bound on this unit by the GL test: its GL name when it has one, else its texture's id.
    bool isBound(const LLImageGL* image) const;
    // Main thread: the white image whose GL name is sWhiteTexture (null clears it); unbind(TT_TEXTURE) tells its device
    // texture while GL binds sWhiteTexture.
    static void setWhiteImage(LLImageGL* image);
    // Main thread: the encoder lets go of every binding LLTexUnit told it of a texture whose owner releases it now, so the
    // owner's release frees the texture and queues its GL name, also while a unit has it bound. No GL call: GL's
    // bindings and the units' GL records stay as they are (see release_encoder_binding). The display lists' entries let
    // go of it first, also without the device.
    static void releaseTexture(const LL::RHI::Texture* texture);
    // For the golden hook: of the textures the encoder's tracker holds, how many have no filter part and how many no
    // address part in their facade slot (main thread with the device; else both 0).
    static void unknownSamplingInUse(U32& filter, U32& address);
    // [Viewer Lab] Tests only, on the main thread: every tell sent to the encoder, as (channel, texture id or 0, the sampling
    // given, its known parts), is appended to the log until it is set to null again; null in the viewer
    using TellLog = std::vector<std::tuple<U32, U64, LL::RHI::SamplerDesc, U8>>;
    static void setTellLog(TellLog* log);

protected:
    friend class LLRender;

    S32                 mIndex;
    U32                 mCurrTexture;
    eTextureType        mCurrTexType;
    bool                mHasMipMaps;

    // [Viewer Lab] The binding's 64-bit id while mCurrTextureIdName == mCurrTexture (the id record), and what the unit has
    // told the encoder (the device's id, the texture's id and the packed sampling: the told record). Written on the main
    // thread with the viewer's device only.
    U64                 mCurrTextureId = 0;
    U32                 mCurrTextureIdName = 0;
    U64                 mToldDevice = 0;
    U64                 mToldTexture = 0;
    U64                 mToldSampling = 0;
    // [Viewer Lab] The tells tellBinding's shortcut found the same without asking for the sampling; main thread only.
    // Protected for the tests' probe, outside BindCounts.
    static U64          sTellShortcuts;

    void debugTextureUnit(void);
    GLint getTextureSource(eTextureBlendSrc src);
    GLint getTextureSourceType(eTextureBlendSrc src, bool isAlpha = false);

    // [Viewer Lab] The sampling setTextureFilteringOptionFast gives GL for a filter option: the option, the unit's mip flag
    // and the current anisotropy, 1 without the capability. Protected for the tests' probe.
    LL::RHI::SamplerDesc filterSampling(eTextureFilterOptions option) const;

private:
    // [Viewer Lab] LLTexUnit's records and tells. Each binding function asks for the viewer's device once (on the
    // main thread, else null) and gives it to the record, the census and the tell.
    // What a tell did: refused at or above the device's channels, nothing changed, or sent to the encoder.
    enum class Told : U8
    {
        Beyond,
        Same,
        Sent
    };
    // An image's device texture when its GL name is the one bound, on the main thread only (mRHITexture is read only
    // there), else null; deviceTextureOf for a call that has the device, so is on the main thread.
    static LL::RHI::Texture* textureOf(const LLImageGL* image, U32 name);
    static LL::RHI::Texture* deviceTextureOf(const LLImageGL* image, U32 name);
    U64 effectiveId() const { return mCurrTextureIdName == mCurrTexture ? mCurrTextureId : 0; }
    // The GL test: by the GL name whenever the binding has one, as upstream; by the id only for a binding without one.
    bool sameBinding(U32 name, U64 id) const
    {
        return name != 0 ? mCurrTexture == name : mCurrTexture == 0 && effectiveId() == id;
    }
    void recordBinding(U32 name, const LL::RHI::Texture* texture);
    void endRecord()
    {
        mCurrTextureId = 0;
        mCurrTextureIdName = 0;
    }
    Told tell(LL::RHI::Device* device, LL::RHI::Texture* texture, const LL::RHI::SamplerDesc& sampling, U8 knownParts);
    Told tellNothing(LL::RHI::Device* device);
    Told tellBinding(LL::RHI::Device* device, LL::RHI::Texture* texture, const LLImageGL* owner);
    LL::RHI::SamplerDesc samplingOf(const LL::RHI::Device* device, LL::RHI::Texture* texture, const LLImageGL* owner,
                                    U8& knownParts);
    LL::RHI::Texture* trackedTexture(LL::RHI::Device* device, U64 id) const;
    static LL::RHI::Texture* activeBinding(LL::RHI::Device* device, eTextureType type);
    void noteBind(U32 name, const LL::RHI::Texture* texture);
    void noteSkip(LL::RHI::Device* device, U32 name, LL::RHI::Texture* texture, const LLImageGL* owner);
    void noteUnbind(eTextureType type);
    void applyAddressMode(eTextureAddressMode mode, eTextureType type, LL::RHI::Texture* texture, LL::RHI::Device* device);
    void applyFilteringOption(eTextureFilterOptions option, eTextureType type, LL::RHI::Texture* texture,
                              LL::RHI::Device* device);
    static void resend(LL::RHI::Device* device, LL::RHI::Texture* texture, U8 part, const LL::RHI::SamplerDesc& sampling);
    // The public setters' guards, flush and activate, for a texture the caller knows: the bind paths' dirty options and
    // LLImageGL's setters.
    void setTextureAddressMode(eTextureAddressMode mode, LL::RHI::Texture* texture);
    void setTextureFilteringOption(eTextureFilterOptions option, LL::RHI::Texture* texture);
    // [Viewer Lab] The GL backend's raw-name bind keeps the unit's name and mips flag
    friend bool gl_bind_raw_name(LLTexUnit* unit, eTextureType type, U32 name, bool has_mips);
};

// [Viewer Lab] The GL backend's bind of a raw GL name on a unit, in llrender.cpp, with the GL and the unit's cache of the
// LLTexUnit::bindManual it replaces: false for the unit's stand-in (index below 0); else, when the unit's
// name differs, a flush, the unit made active and enabled for type, the name kept, glBindTexture and the mips flag, and
// nothing when it is the same name; true. No id record and no tell: a caller with the device does its own. For llrender's
// objects that can hold a raw name (the bind_raw helpers, the facades' raw paths) and the GL adapter (GLEncoder,
// GLNames::bind); viewer code binds textures of the device through bindTexture.
bool gl_bind_raw_name(LLTexUnit* unit, LLTexUnit::eTextureType type, U32 name, bool has_mips);

// [Viewer Lab] The --debug-gl check of LLTexUnit's binding, a GL backend function in llrender.cpp: when GL
// is the backend and name is bound for type on GL's active unit, compares the parts of sampling that known_parts marks
// (bit 0 the filter part: MIN, MAG and, with the capability, MAX_ANISOTROPY; bit 1 the address part: WRAP_S, WRAP_T and,
// for the cube and 3D types, WRAP_R) with the texture's GL state and warns with the RHI tag on a difference. Returns
// whether it compared a part. Reads GL state only; declared here for LLTexUnit and the tests.
bool gl_check_sampling(LLTexUnit::eTextureType type, U32 name, const LL::RHI::SamplerDesc& sampling, U8 known_parts);

class LLLightState
{
public:
    LLLightState(S32 index = -1);

    void enable();
    void disable();
    void setDiffuse(const LLColor4& diffuse);
    void setDiffuseB(const LLColor4& diffuse);
    void setAmbient(const LLColor4& ambient);
    void setSpecular(const LLColor4& specular);
    void setPosition(const LLVector4& position);
    void setConstantAttenuation(const F32& atten);
    void setLinearAttenuation(const F32& atten);
    void setQuadraticAttenuation(const F32& atten);
    void setSpotExponent(const F32& exponent);
    void setSpotCutoff(const F32& cutoff);
    void setSpotDirection(const LLVector3& direction);
    void setSunPrimary(bool v);
    void setSize(F32 size);
    void setFalloff(F32 falloff);

protected:
    friend class LLRender;

    S32 mIndex;
    bool mEnabled;
    LLColor4 mDiffuse;
    LLColor4 mDiffuseB;
    bool     mSunIsPrimary;
    LLColor4 mAmbient;
    LLColor4 mSpecular;
    LLVector4 mPosition;
    LLVector3 mSpotDirection;

    F32 mConstantAtten;
    F32 mLinearAtten;
    F32 mQuadraticAtten;

    F32 mSpotExponent;
    F32 mSpotCutoff;
    F32 mSize = 0.f;
    F32 mFalloff = 0.f;
};

class LLRender
{
    friend class LLTexUnit;
public:

    enum eTexIndex : U8
    {
        // Channels for material textures
        DIFFUSE_MAP            = 0,
        ALTERNATE_DIFFUSE_MAP  = 1,
        NORMAL_MAP             = 1,
        SPECULAR_MAP           = 2,
        // Channels for PBR textures
        BASECOLOR_MAP          = 3,
        METALLIC_ROUGHNESS_MAP = 4,
        GLTF_NORMAL_MAP        = 5,
        EMISSIVE_MAP           = 6,
        // Total number of channels
        NUM_TEXTURE_CHANNELS   = 7,
    };

    enum eVolumeTexIndex : U8
    {
        LIGHT_TEX = 0,
        SCULPT_TEX,
        NUM_VOLUME_TEXTURE_CHANNELS,
    };

    enum eGeomModes : U8
    {
        TRIANGLES = 0,
        TRIANGLE_STRIP,
        TRIANGLE_FAN,
        POINTS,
        LINES,
        LINE_STRIP,
        LINE_LOOP,
        NUM_MODES
    };

    enum eCompareFunc : U8
    {
        CF_NEVER = 0,
        CF_ALWAYS,
        CF_LESS,
        CF_LESS_EQUAL,
        CF_EQUAL,
        CF_NOT_EQUAL,
        CF_GREATER_EQUAL,
        CF_GREATER,
        CF_DEFAULT
    };

    enum eBlendType : U8
    {
        BT_ALPHA = 0,
        BT_ADD,
        BT_ADD_WITH_ALPHA,  // Additive blend modulated by the fragment's alpha.
        BT_MULT,
        BT_MULT_ALPHA,
        BT_MULT_X2,
        BT_REPLACE
    };

    // WARNING:  this MUST match the LL_PART_BF enum in LLPartData, so set values explicitly in case someone
    // decides to add more or reorder them
    enum eBlendFactor : U8
    {
        BF_ONE = 0,
        BF_ZERO = 1,
        BF_DEST_COLOR = 2,
        BF_SOURCE_COLOR = 3,
        BF_ONE_MINUS_DEST_COLOR = 4,
        BF_ONE_MINUS_SOURCE_COLOR = 5,
        BF_DEST_ALPHA = 6,
        BF_SOURCE_ALPHA = 7,
        BF_ONE_MINUS_DEST_ALPHA = 8,
        BF_ONE_MINUS_SOURCE_ALPHA = 9,
        BF_UNDEF
    };

    enum eMatrixMode : U8
    {
        MM_MODELVIEW = 0,
        MM_PROJECTION,
        MM_TEXTURE0,
        MM_TEXTURE1,
        MM_TEXTURE2,
        MM_TEXTURE3,
        NUM_MATRIX_MODES,
        MM_TEXTURE
    };

    LLRender();
    ~LLRender();
    bool init(bool needs_vertex_buffer);
    void initVertexBuffer();
    void resetVertexBuffer();
    void shutdown();

    // Refreshes renderer state to the cached values
    // Needed when the render context has changed and invalidated the current state
    void refreshState(void);

    void translatef(const GLfloat& x, const GLfloat& y, const GLfloat& z);
    void scalef(const GLfloat& x, const GLfloat& y, const GLfloat& z);
    void rotatef(const GLfloat& a, const GLfloat& x, const GLfloat& y, const GLfloat& z);
    void ortho(F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar);

    void pushMatrix();
    void popMatrix();
    void loadMatrix(const GLfloat* m);
    void loadIdentity();
    void multMatrix(const GLfloat* m);
    void matrixMode(eMatrixMode mode);
    eMatrixMode getMatrixMode();

    const glm::mat4& getModelviewMatrix();
    const glm::mat4& getProjectionMatrix();

    void syncMatrices();
    void syncLightState();

    void translateUI(F32 x, F32 y, F32 z);
    void scaleUI(F32 x, F32 y, F32 z);
    void pushUIMatrix();
    void popUIMatrix();
    void loadUIIdentity();
    LLVector3 getUITranslation();
    LLVector3 getUIScale();

    void flush();

    // if list is set, will store buffers in list for later use, if list isn't set, will use cache
    void beginList(std::list<LLVertexBufferData> *list);
    void endList();

    void begin(const GLuint& mode);
    void end();

    U8 getMode() const { return mMode; }

    void vertex2i(const GLint& x, const GLint& y);
    void vertex2f(const GLfloat& x, const GLfloat& y);
    void vertex3f(const GLfloat& x, const GLfloat& y, const GLfloat& z);
    void vertex2fv(const GLfloat* v);
    void vertex3fv(const GLfloat* v);

    void texCoord2i(const GLint& x, const GLint& y);
    void texCoord2f(const GLfloat& x, const GLfloat& y);
    void texCoord2fv(const GLfloat* tc);

    void color4ub(const GLubyte& r, const GLubyte& g, const GLubyte& b, const GLubyte& a);
    void color4f(const GLfloat& r, const GLfloat& g, const GLfloat& b, const GLfloat& a);
    void color4fv(const GLfloat* c);
    void color3f(const GLfloat& r, const GLfloat& g, const GLfloat& b);
    void color3fv(const GLfloat* c);
    void color4ubv(const GLubyte* c);

    void diffuseColor3f(F32 r, F32 g, F32 b);
    void diffuseColor3fv(const F32* c);
    void diffuseColor4f(F32 r, F32 g, F32 b, F32 a);
    void diffuseColor4fv(const F32* c);
    void diffuseColor4ubv(const U8* c);
    void diffuseColor4ub(U8 r, U8 g, U8 b, U8 a);

    void vertexBatchPreTransformed(LLVector4a* verts, S32 vert_count);
    void vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, S32 vert_count);
    void vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, LLColor4U*, S32 vert_count);

    void setColorMask(bool writeColor, bool writeAlpha);
    void setColorMask(bool writeColorR, bool writeColorG, bool writeColorB, bool writeAlpha);
    void setSceneBlendType(eBlendType type);

    // applies blend func to both color and alpha
    void blendFunc(eBlendFactor sfactor, eBlendFactor dfactor);
    // applies separate blend functions to color and alpha
    void blendFunc(eBlendFactor color_sfactor, eBlendFactor color_dfactor,
               eBlendFactor alpha_sfactor, eBlendFactor alpha_dfactor);

    // [Viewer Lab] The factors blendFunc last set (BF_UNDEF before the first call) and the color mask, read only and with no
    // GL, for the render interface's pipeline descriptions
    void getBlendFunc(eBlendFactor& colorS, eBlendFactor& colorD, eBlendFactor& alphaS, eBlendFactor& alphaD) const
    {
        colorS = mCurrBlendColorSFactor;
        colorD = mCurrBlendColorDFactor;
        alphaS = mCurrBlendAlphaSFactor;
        alphaD = mCurrBlendAlphaDFactor;
    }
    void getColorMask(bool& r, bool& g, bool& b, bool& a) const
    {
        r = mCurrColorMask[0];
        g = mCurrColorMask[1];
        b = mCurrColorMask[2];
        a = mCurrColorMask[3];
    }

    LLLightState* getLight(U32 index);
    void setAmbientLightColor(const LLColor4& color);
    // [Viewer Lab] The ambient light color setAmbientLightColor last set, read only and with no GL
    const LLColor4& getAmbientLightColor() const { return mAmbientLightColor; }

    void setLineWidth(F32 line_width); // <FS> Line width OGL core profile fix by Rye Mutt

    // [Viewer Lab] Dynamic state with GL's numbers and GL enums. Each setter makes the GL
    // call it is named after and, like that call, flushes nothing and skips nothing, except setScissor inside the GL backend's
    // own pass, which skips a box GL and the pass's record already have. The enables stay LLGLState's
    // (GL_SCISSOR_TEST, GL_CULL_FACE, GL_POLYGON_OFFSET_FILL and _LINE, GL_DEPTH_CLAMP), so depth clamp has no setter of its
    // own: its facade is LLGLEnable/LLGLState(GL_DEPTH_CLAMP), which the encoder gets as setDepthClamp.
    void setViewport(S32 x, S32 y, S32 width, S32 height);       // glViewport
    void setWindowViewport(S32 x, S32 y, S32 width, S32 height); // also gGLViewport, the viewport LLRenderTarget::flush returns to
    void setScissor(S32 x, S32 y, S32 width, S32 height);        // glScissor
    void setCullFace(U32 face);                                  // glCullFace
    void setPolygonMode(U32 face, U32 mode);                     // glPolygonMode
    void setPolygonOffset(F32 factor, F32 units);                // glPolygonOffset
    void setPointSize(F32 size);                                 // glPointSize, a value per draw
    F32 getLineWidth() const { return mLineWidth; }              // what setLineWidth gave GL
    U32 getCullFace() const { return mCullFace; }                // [Viewer Lab] what setCullFace gave GL

    // [Viewer Lab] While one lives, LLRender's and LLGLState's GL backend functions have made GL's calls for the
    // dynamic state, so the GL backend's dynamic setters only track, and the facades send nothing. It puts the previous
    // value back, so a nested scope never clears it early. Main thread only (a static, not thread_local). Kept as a GL
    // backend switch, its contract with llrender (llrhigl.cpp).
    struct OwnGLState
    {
        OwnGLState();
        ~OwnGLState();
        OwnGLState(const OwnGLState&) = delete;
        OwnGLState& operator=(const OwnGLState&) = delete;

        const bool mPrevious;
    };
    static bool makesOwnGLState();
    // [Viewer Lab] For the GL backend only (llrhigl.cpp), main thread: LLRenderTarget's stack pass has begun on the GL
    // encoder, whose dynamic setters only track it, and has ended (also with the frame or the device). In between,
    // setScissor records the box straight in the pass's tracker, as the encoder's setScissor does inside OwnGLState,
    // and skips a box GL and the record already have. Both forget the box. They stay with OwnGLState.
    void beginOwnPass(LL::RHI::EncoderTracker& tracker);
    void endOwnPass();
    const LL::RHI::EncoderTracker* ownPassTracker() const { return mOwnPassTracker; } // for the tests
    // [Viewer Lab] GL's scissor box, the open pass's record or LLGLState's GL_SCISSOR_TEST may no longer be what setScissor
    // last gave: its next call makes the GL call and records, also for the same box. sendEnable(GL_SCISSOR_TEST) and the GL
    // backend's own scissor calls and enables call it; raw glScissor, which the gl* ratchet keeps out of the viewer, must call
    // it too (tests).
    void invalidateScissor()
    {
        mScissorKnown = false;
        mScissorTest = -1;
    }

    // [Viewer Lab] For LLGLState only: an enable LLGLState changed, given to the viewer's encoder while one of its passes is
    // open, with the state's whole tuple from LLRender's remembered values. Main thread only.
    void sendEnable(U32 state, bool enabled);
    // [Viewer Lab] For LLRenderTarget's stack passes as they begin: the viewport once set, and every remembered value that
    // differs from the defaults a pass begins with. It neither flushes nor calls a facade, so it may run inside flush.
    void sendDynamicState(LL::RHI::Encoder& encoder);
    // [Viewer Lab] Main-thread totals since the process began, for the golden hook and the tests.
    struct DynamicStateCounts
    {
        U64 sends = 0;               ///< values given to an encoder in a pass by the facades and sendEnable
        U64 passSets = 0;            ///< stack-pass begins that got the viewer's dynamic state again
        U64 passSetCalls = 0;        ///< the setter calls those begins made
        U64 unclassifiedEnables = 0; ///< changes LLGLState made to an enum with no fixed path
    };
    static const DynamicStateCounts& dynamicStateCounts();

    LLTexUnit* getTexUnit(U32 index);

    U32 getCurrentTexUnitIndex(void) const { return mCurrTextureUnitIndex; }

    bool verifyTexUnitActive(U32 unitToVerify);

    void debugTexUnits(void);

    void clearErrors();

    struct Vertex
    {
        GLfloat v[3];
        GLubyte c[4];
        GLfloat uv[2];
    };

public:
    static U32 sUICalls;
    static U32 sUIVerts;
    static bool sGLCoreProfile;
    static bool sNsightDebugSupport;
    static LLVector2 sUIGLScaleFactor;
    static bool sClassicMode; // classic sky mode active

private:
    friend class LLLightState;

    LLVertexBuffer* bufferfromCache(U32 attribute_mask, U32 count);
    LLVertexBuffer* genBuffer(U32 attribute_mask, S32 count);
    void drawBuffer(LLVertexBuffer* vb, U32 mode, S32 count);
    void resetStriders(S32 count);

    eMatrixMode mMatrixMode;
    U32 mMatIdx[NUM_MATRIX_MODES];
    U32 mMatHash[NUM_MATRIX_MODES];
    glm::mat4 mMatrix[NUM_MATRIX_MODES][LL_MATRIX_STACK_DEPTH];
    U32 mCurMatHash[NUM_MATRIX_MODES];
    U32 mLightHash;
    LLColor4 mAmbientLightColor;

    bool            mDirty;
    U32             mCount;
    U32             mMode;
    U32             mCurrTextureUnitIndex;
    bool                mCurrColorMask[4];
    F32             mLineWidth; // <FS> Line width OGL core profile fix by Rye Mutt
    // <FS:Ansariel> Don't ignore OpenGL max line width
    F32             mMaxLineWidthSmooth;
    F32             mMaxLineWidthAliased;
    // </FS:Ansariel>

    // [Viewer Lab] What the dynamic state facades gave GL, starting as GL does. A box never set is larger than any
    // attachment, which the backend clips, as GL's initial box is the window's; the face is LLRender::init's.
    S32             mViewport[4] = { 0, 0, 0, 0 };
    bool            mViewportSet = false;
    S32             mScissor[4] = { 0, 0, S32_MAX, S32_MAX };
    // [Viewer Lab] The GL encoder's own pass while it is open (beginOwnPass), else null; true while GL's box and that pass's
    // record are mScissor, which setScissor then skips; LLGLState's GL_SCISSOR_TEST at the last record, -1 when not known.
    // Never a second copy of the box (test<10> finds it once in gGL)
    LL::RHI::EncoderTracker* mOwnPassTracker = nullptr;
    bool            mScissorKnown = false;
    S8              mScissorTest = -1;
    U32             mCullFace = GL_BACK;
    U32             mPolygonMode = GL_FILL;
    F32             mPolygonOffset[2] = { 0.f, 0.f };
    F32             mPointSize = 1.f;

    LLPointer<LLVertexBuffer>   mBuffer;
    LLStrider<LLVector4a>       mVerticesp;
    LLStrider<LLVector2>        mTexcoordsp;
    LLStrider<LLColor4U>        mColorsp;
    std::array<LLTexUnit, LL_NUM_TEXTURE_LAYERS> mTexUnits;
    LLTexUnit           mDummyTexUnit;
    std::array<LLLightState, LL_NUM_LIGHT_UNITS> mLightState;

    eBlendFactor mCurrBlendColorSFactor;
    eBlendFactor mCurrBlendColorDFactor;
    eBlendFactor mCurrBlendAlphaSFactor;
    eBlendFactor mCurrBlendAlphaDFactor;

    std::vector<LLVector4a, boost::alignment::aligned_allocator<LLVector4a, 16> > mUIOffset;
    std::vector<LLVector4a, boost::alignment::aligned_allocator<LLVector4a, 16> > mUIScale;
};

extern F32 gGLModelView[16];
extern F32 gGLLastModelView[16];
extern F32 gGLLastProjection[16];
extern F32 gGLProjection[16];
extern S32 gGLViewport[4];
extern glm::mat4 gGLDeltaModelView;
extern glm::mat4 gGLInverseDeltaModelView;

extern thread_local LLRender gGL;

// This rotation matrix moves the default OpenGL reference frame
// (-Z at, Y up) to Cory's favorite reference frame (X at, Z up)
const F32 OGL_TO_CFR_ROTATION[16] = {  0.f,  0.f, -1.f,  0.f,   // -Z becomes X
                                      -1.f,  0.f,  0.f,  0.f,   // -X becomes Y
                                       0.f,  1.f,  0.f,  0.f,   //  Y becomes Z
                                       0.f,  0.f,  0.f,  1.f };

glm::mat4 copy_matrix(F32* src);
glm::mat4 get_current_modelview();
glm::mat4 get_current_projection();
glm::mat4 get_last_modelview();
glm::mat4 get_last_projection();

void copy_matrix(const glm::mat4& src, F32* dst);
void set_current_modelview(const glm::mat4& mat);
void set_current_projection(const glm::mat4& mat);
void set_last_modelview(const glm::mat4& mat);
void set_last_projection(const glm::mat4& mat);

// glh compat
glm::vec3 mul_mat4_vec3(const glm::mat4& mat, const glm::vec3& vec);

#define LL_SHADER_LOADING_WARNS(...) LL_WARNS()

#endif
