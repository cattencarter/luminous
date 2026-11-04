/**
 * @file llrhi.h
 * @brief Render hardware interface: the device, resources, pipelines, passes and the encoder
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Viewer Lab contributors
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

#ifndef LL_LLRHI_H
#define LL_LLRHI_H

#include "llpointer.h"
#include "llrefcount.h"
#include "stdtypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// The render hardware interface. The llrender facades (LLImageGL, LLVertexBuffer, LLRenderTarget,
// LLGLSLShader, LLRender) will own these objects and call them. The GL backend is a thin adapter
// over the GL code where it lies in llrender today; the Metal backend runs on Apple Silicon with
// macOS 26. The viewer has one device, which viewerDevice() gives; the facades call it each time
// they need the device and never keep a Ref to it.
//
// Names avoid the macros that platform headers define, which llglheaders.h brings in: X11 has
// None, Always, Bool, Status, True, False and Success, and Windows has near, far, ERROR, OPAQUE
// and TRANSPARENT.

namespace LL::RHI
{

enum class BackendType : U8
{
    GL,
    Metal,
};

/// The startup check, made before the window exists, because the window gives LLOpenGLView
/// either a GL context or a CAMetalLayer.
struct BackendChoice
{
    BackendType type = BackendType::GL;
    std::string reason; ///< why Metal was not chosen; empty when it was chosen or not asked for
    std::string gpuName; ///< the Metal device's name when Metal is chosen, from the check's own device; empty on GL
                         ///< (the CGL store's key needs no second device)
};

/// What the startup check knows about the process and the Mac.
struct PlatformFacts
{
    bool built = false;       ///< the Metal backend is built into this viewer (LL_RHI_METAL)
    bool arm64 = false;       ///< the process is arm64
    bool translated = false;  ///< the process runs translated by Rosetta
    bool macOS26 = false;     ///< macOS 26.0 or newer
    bool metalDevice = false; ///< there is a Metal device
    bool metal4 = false;      ///< the device supports MTLGPUFamilyMetal4
    bool apple7 = false;      ///< the device supports MTLGPUFamilyApple7
    std::string gpuName;      ///< the Metal device's name; empty without a device
};

/// Metal is chosen only when it is asked for, the process is arm64 (not Rosetta), the Metal
/// backend is built, macOS is 26 or newer, and the device supports MTLGPUFamilyMetal4 and
/// MTLGPUFamilyApple7. Otherwise GL with the reason. chooseBackend writes the line
/// where it is called: one WARN with the reason, or an INFO with the choice. The viewer's start
/// logs its backend through createViewerDevice()'s INFO line. The facts are gathered only when
/// Metal is asked for: for GL there is no sysctl and no Metal call.
BackendChoice chooseBackend(BackendType preferred);

/// The same check on given facts, which the tests inject.
BackendChoice chooseBackend(BackendType preferred, const PlatformFacts& facts);

/// The normal backend choice: what a start reads before it asks the startup check.
struct BackendRequestFacts
{
    std::string saved;             ///< GraphicsBackend: OpenGL or Metal, in any case; anything else is GL with a WARN
    std::string commandLine;       ///< --backend for this start only: metal or opengl, in any case; empty when not given
    bool metalSessionOpen = false; ///< MetalSessionOpen: the last session on Metal did not end normally
    bool markerApplies = true;     ///< false in the Metal UI dev mode and in a second instance: no marker is read or written
};

/// What the start does with the facts.
struct BackendRequest
{
    BackendType type = BackendType::GL; ///< Metal: ask the startup check for Metal; GL: no check at all
    bool fellBack = false;    ///< the saved Metal is refused because of the marker: GL now, GraphicsBackend becomes
                              ///< OpenGL and the start says so once
    bool setMarker = false;   ///< set and save MetalSessionOpen before the startup check makes its Metal device
    bool clearMarker = false; ///< clear and save MetalSessionOpen now (a fallback, or a stale marker under a saved choice
                              ///< that is not Metal)
};

/// A valid --backend wins over GraphicsBackend. A saved Metal with the marker set gives GL with fellBack and one WARN;
/// a command-line metal is explicit and ignores the marker. Unknown values give one WARN each; GL with no marker logs
/// nothing.
BackendRequest resolveBackendRequest(const BackendRequestFacts& facts);

/// At a normal quit: whether the session clears MetalSessionOpen. Only a session that set the marker and still runs on
/// Metal does; after the in-process GL fallback (the Metal device failed after the window) the marker stays, so the
/// next start is GL with the notice.
bool clearsMetalSessionAtQuit(bool sessionSetMarker, BackendType running);

/// The main window's title with the backend the viewer runs on after any GL fallback: "<title> (Metal)" or
/// "<title> (OpenGL)", e.g. "Second Life (Metal)". The viewer sets it on macOS only.
std::string windowTitleWithBackend(const std::string& title, BackendType running);

/// Whether Preferences > Graphics offers the backend switch: the Metal backend is built
/// (arm64 and an SDK with Metal 4) and macOS is 26 or newer (macOSMajor: the running macOS's major version, from
/// LLOSInfo). It asks no Metal device and makes no availability check (those stay in the Metal backend's files), so a
/// GL session may ask; the device's GPU families are checked at the start (chooseBackend), which still gives GL with
/// its reason.
bool metalSelectable(S32 macOSMajor);

/// The switch's entry for a saved GraphicsBackend: "Metal" when it names Metal in any case, otherwise "OpenGL", which is
/// what resolveBackendRequest starts for it.
std::string graphicsBackendEntry(const std::string& saved);

/// What the switch saves when Preferences applies: the chosen entry ("Metal" or "OpenGL", in any case), or empty when
/// the saved value already starts that backend (its spelling stays) or the entry is neither.
std::string graphicsBackendToSave(const std::string& saved, const std::string& chosen);

/// The settings the start reads for LLImageGLThread's loads: Linden's two, which GL and the feature table use
/// (featuretable_mac.txt turns both off on Apple GPUs), and Metal's own two, on by default, which only the Metal path
/// uses.
struct UploadThreadSettings
{
    bool renderGLTextures = false; ///< RenderGLMultiThreadedTextures
    bool renderGLMedia = false;    ///< RenderGLMultiThreadedMedia
    bool metalTextures = true;     ///< MetalMultiThreadedTextures
    bool metalMedia = true;        ///< MetalMultiThreadedMedia
};

/// Whether LLImageGLThread makes fetched textures (textures) and media updates (media): what LLImageGL::initClass gets.
struct UploadThreads
{
    bool textures = false;
    bool media = false;
};

/// The start's choice of the load threads (on by default on Metal, GL keeps Linden's default). On GL (and wherever the
/// Metal backend is not built) Linden's settings as they are. On Metal each load is on when Linden's setting is, as before, or Metal's own setting is; so a
/// hand-set RenderGLMultiThreaded* 1 still holds on both backends, and Metal's are the switch that turns the loads off
/// on Metal. Metal's default is a setting of its own, never a value of Linden's, so it is never saved into the
/// settings file GL and Linden's viewer share. running is the backend the viewer runs (GL after an in-process
/// fallback). On Metal one INFO line says the choice; on GL nothing is logged.
UploadThreads uploadThreads(BackendType running, const UploadThreadSettings& settings);

// ------------------------------------------------------------------------------------------------
// Formats
//
// The facades keep GL enums as their language; one table in llrhi.cpp translates them. There are
// no three-component formats: GL_RGB8 becomes RGBA8Unorm and GL_RGB16F RGBA16Float, and the Metal
// backend widens RGB pixel data on upload (GL uploads as today). GL_DEPTH_COMPONENT24 and 32 become
// Depth32Float, which is what Apple's GL stores too. GL_RGBA16 is RGBA16Unorm. The table
// grows with the formats LLImageGL and LLRenderTarget actually use, including GL's generic
// compressed formats.

enum class Format : U16
{
    Unknown = 0,
    R8Unorm,
    RG8Unorm,
    RGBA8Unorm,
    RGBA8Unorm_sRGB,
    BGRA8Unorm,         ///< drawables only
    R16Float,
    RG16Float,
    RGBA16Float,
    RGBA16Unorm,        ///< GL_RGBA16, LLRenderTarget's deferred normals with HDR
    R32Float,
    RG32Float,
    RGBA32Float,
    RGB10A2Unorm,
    RG11B10Float,
    R8Uint,
    R16Uint,
    R32Uint,
    BC1_RGBA,
    BC1_RGBA_sRGB,
    BC3_RGBA,
    BC3_RGBA_sRGB,
    BC5_RG,
    BC7_RGBA,
    BC7_RGBA_sRGB,
    Depth32Float,
    Depth32Float_Stencil8,

    Count ///< not a format: the number of values above, for tables and tests
};

bool isCompressed(Format format);
bool isDepth(Format format);
bool hasStencil(Format format);
bool isInteger(Format format);
/// The formats that store sRGB-encoded colour: RGBA8Unorm_sRGB and the BC sRGB formats.
bool isSRGB(Format format);
/// Bytes of one 4x4 block of a compressed format; 0 for the others.
U32 blockBytes(Format format);

/// Layout of pixel data in CPU memory, for uploads and readbacks: GL's format and type pair, and
/// GL_UNPACK_ROW_LENGTH as rowLength.
/// Rows are tightly packed at alignment 1: GL_UNPACK_ALIGNMENT 1, which LLRender::init sets on every
/// context it sets up (the main one and LLImageGLThread's). Rows come in GL's order and are never
/// flipped.
struct PixelLayout
{
    enum class Components : U8
    {
        R,
        RG,
        RGB,
        RGBA,
        BGRA,
        Depth,
    };
    enum class Type : U8
    {
        U8,
        U16,
        U32,
        F16,
        F32,
        U32_8_8_8_8_Rev, ///< GL_UNSIGNED_INT_8_8_8_8_REV, sent by the GStreamer plugin
    };

    Components components = Components::RGBA;
    Type type = Type::U8;
    bool swapBytes = false; ///< GL_UNPACK_SWAP_BYTES, which media plugins can ask for
    U32 rowLength = 0;      ///< pixels per source row; 0 means tightly packed (setSubImage uses it)

    bool operator==(const PixelLayout&) const = default;

    /// Bytes of one pixel in CPU memory.
    U32 bytesPerPixel() const;
};

/// Luminance and alpha textures are R8 with a swizzle, as in LLImageGL on the core profile.
enum class Swizzle : U8
{
    R,
    G,
    B,
    A,
    Zero,
    One,
};

struct SwizzleRGBA
{
    Swizzle r = Swizzle::R;
    Swizzle g = Swizzle::G;
    Swizzle b = Swizzle::B;
    Swizzle a = Swizzle::A;

    bool operator==(const SwizzleRGBA&) const = default;
    bool isIdentity() const { return *this == SwizzleRGBA(); }
};

// ------------------------------------------------------------------------------------------------
// Sampling state. The order follows llrender's own enums, so the facades translate by table.

/// LLRender::eCompareFunc without CF_DEFAULT, which the facade resolves ("AlwaysPass", because X11
/// defines Always).
enum class CompareFunc : U8
{
    Never = 0,
    AlwaysPass,
    Less,
    LessEqual,
    Equal,
    NotEqual,
    GreaterEqual,
    Greater,
};

/// LLTexUnit::eTextureFilterOptions. The mip part applies only with SamplerDesc::mipmapped; without it every filter
/// minifies from the base level alone.
enum class Filter : U8
{
    Point = 0,  ///< min nearest, mag nearest, mip nearest
    Bilinear,   ///< min linear, mag linear, mip nearest
    Trilinear,  ///< min linear, mag linear, mip linear
    Anisotropic ///< as Trilinear, with SamplerDesc::maxAnisotropy
};

/// GL_TEXTURE_MAG_FILTER apart from filter: FromFilter is filter's own (nearest for Point, else linear); the light function
/// and glTF set it alone
enum class MagFilter : U8
{
    FromFilter = 0,
    Nearest,
    Linear
};

/// LLTexUnit::eTextureAddressMode.
enum class AddressMode : U8
{
    Repeat = 0,
    MirroredRepeat,
    ClampToEdge,
};

/// The texture types the viewer uses. LLTexUnit's rectangle and multisample types have no user today. 3D textures
/// come through Tex3D only where the device has Capabilities::textures3D (native programs' lookup tables); GL keeps its
/// raw 3D textures. A translated program's sampler3D reads a channel's Tex3D slot (Encoder::setTexture).
enum class TextureType : U8
{
    Tex2D,
    Cube,
    CubeArray,
    /// TextureDesc::layers is the depth; one mip level, sampled only (no render target, no readback); an upload
    /// writes one slice, its layer being z. Bound natively, or on a channel for a translated program's sampler3D.
    Tex3D,
};

// ------------------------------------------------------------------------------------------------
// Draw state. The order follows llrender's own enums, so the facades translate by table.

/// LLRender::eGeomModes. Metal has no fans and no line loops; the Metal backend draws fans and
/// triangle strips as generated index lists in GL's provoking order (flat varyings from GL's last
/// vertex) and line loops as line strips. Plain lists are drawn as they are, so their flat varyings
/// follow Metal's first vertex.
enum class PrimitiveType : U8
{
    Triangles = 0,
    TriangleStrip,
    TriangleFan,
    Points,
    Lines,
    LineStrip,
    LineLoop,
};

/// The pipeline's topology class. Points get their own vertex-shader variant that
/// writes the point size.
enum class PrimitiveClass : U8
{
    Point,
    Line,
    Triangle,
};

PrimitiveClass primitiveClass(PrimitiveType type);

enum class IndexType : U8
{
    U16,
    U32,
};

/// Bytes of one index.
U32 indexSize(IndexType type);

/// LLRender::eBlendFactor, in the same order (it must match LLPartData's LL_PART_BF values).
enum class BlendFactor : U8
{
    One = 0,
    Zero,
    DestColor,
    SourceColor,
    OneMinusDestColor,
    OneMinusSourceColor,
    DestAlpha,
    SourceAlpha,
    OneMinusDestAlpha,
    OneMinusSourceAlpha,
};

enum class BlendOp : U8
{
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max,
};

enum ColorMask : U8
{
    COLOR_MASK_NONE = 0,
    COLOR_MASK_R = 1 << 0,
    COLOR_MASK_G = 1 << 1,
    COLOR_MASK_B = 1 << 2,
    COLOR_MASK_A = 1 << 3,
    COLOR_MASK_ALL = COLOR_MASK_R | COLOR_MASK_G | COLOR_MASK_B | COLOR_MASK_A,
};

/// Culling uses GL's front face, counter-clockwise; the Metal backend sets MTLWindingClockwise,
/// because its vertex shaders flip y. There is no front-face setter: the viewer never
/// calls glFrontFace. "Off" rather than None, which X11 defines.
enum class CullMode : U8
{
    Off,
    Front,
    Back,
};

/// glPolygonMode's GL_FILL and GL_LINE (wireframe, selection outlines).
enum class FillMode : U8
{
    Fill,
    Lines,
};

enum class LoadAction : U8
{
    Load,
    Clear,
    DontCare,
};

enum class StoreAction : U8
{
    Store,
    DontCare,
};

enum class ShaderStage : U8
{
    Vertex,
    Fragment,
    Geometry, ///< GL only (the normal-debug program), see Capabilities::geometryShaders
};

/// PrimitivesGenerated exists only on GL (Capabilities::primitivesGeneratedQuery).
enum class QueryType : U8
{
    AnySamplesPassed, ///< at least one sample; the GL backend asks GL for GL_SAMPLES_PASSED below GL 3.3
    SamplesPassed,
    TimeElapsed,
    PrimitivesGenerated,
};

// ------------------------------------------------------------------------------------------------
// Capabilities. Independent of GL versions. The facade LLGLManager stays the place
// the viewer asks; on Metal its fields get the values the system's GL reports on the same Mac, so
// code paths, shader defines and graphics levels stay the same.

struct Capabilities
{
    BackendType backend = BackendType::GL;

    // Limits. The backends check them before calling the native API, because Metal stops the
    // program with an assertion above them (see validate() below).
    U32 maxTextureSize = 0;            ///< 2D textures and cube faces
    U32 maxArrayLayers = 0;            ///< 2D layers, so a cube array needs 6 per cube
    U32 maxColorAttachments = 0;
    U32 maxSamples = 0;
    U32 maxSamplersPerStage = 0;       ///< 16 on Metal, also what mNumTextureImageUnits reports
    U32 maxTexturesPerStage = 0;
    U32 maxUniformBlockSize = 0;       ///< bytes
    U32 maxVaryingVectors = 0;         ///< 31; 32 crashes Metal's shader compiler service
    U32 maxVertexAttributes = 0;
    F32 lineWidthRange[2] = { 1.f, 1.f }; ///< 1-1 on Mac with both backends
    F32 pointSizeRange[2] = { 1.f, 1.f };
    F32 maxAnisotropy = 1.f;

    // Features
    bool cubeMapArrays = false;
    bool anisotropicFiltering = false;
    bool textureSwizzle = false;           ///< swizzle masks instead of LLImageGL's conversion buffer
    bool bcCompression = false;
    bool timerQueries = false;             ///< LLPipeline::profileAvatar and the GPU benchmark
    bool timerQueriesWithinPass = false;   ///< false on Metal, where a query mid-pass splits the pass
    /// gpu_benchmark: GL as timerQueries; Metal false, so its GPU class stays Darwin's class 3 until a GL and
    /// a Metal `--benchmark --against` run give the same class, though its timer queries work
    bool benchmarkTimers = false;
    /// The interface allows AnySamplesPassed and SamplesPassed to be active at once: the encoder's tracker does not
    /// refuse the overlap, and the backend decides. GL: from 3.3, where "at least one sample" has a GL target of its
    /// own, so GL accepts or refuses the overlap as it does today's raw calls; Apple's GL 4.1 refuses it in both
    /// orders with GL_INVALID_OPERATION ('LLRHI GL queries' test<1>'s p1 in llrhigl_test.cpp). Metal: false (one
    /// visibility result mode per render encoder)
    bool concurrentSampleQueries = false;
    bool primitivesGeneratedQuery = false; ///< GL only
    bool geometryShaders = false;          ///< GL only
    /// The device could run compute; it says nothing of the interface (computePasses does) and nothing reads it.
    bool compute = false;
    bool computePasses = false;  ///< Encoder's compute passes, dispatches and TEXTURE_STORAGE / BUFFER_STORAGE (Metal)
    bool nativePrograms = false; ///< Device::createNativeLibrary, native pipelines and native passes (Metal)
    bool textures3D = false;     ///< TextureType::Tex3D (Metal); GL keeps its raw 3D textures
    bool depthClamp = false;
    bool debugLabels = false;

    // Mechanisms the facades select today, as textureSwizzle and timerQueries above; their owners
    // ask these instead
    bool programBinaries = false;          ///< GL's program binary cache
    bool sharedContextUploads = false;     ///< LLImageGLThread's uploads: GL's shared context, Metal's queue
    bool pixelBufferObjects = false;       ///< LLImageGL's PBO paths
    /// Encoder::copyTexture between textures of different formats: GL's glCopyTexSubImage2D converts; Metal copies them
    /// as a draw, so LLImageGL::scaleDown asks before it changes anything. copyRefusal is
    /// the one rule over both flags
    bool copyBetweenFormats = false;
    /// With copyBetweenFormats: only between plain colour formats, neither sRGB, block-compressed, depth nor integer
    /// (Metal's copy as a draw); false on GL, which hands every pair to glCopyTexSubImage2D as before
    bool copyBetweenFormatsPlainColorOnly = false;

    // Workarounds for GL drivers in the GLSL LLShaderMgr assembles; false on Metal
    bool shaderOldSelect = false;          ///< OLD_SELECT: no mix(vec3, vec3, bvec3) on a Mac GPU other than Apple's
    bool shaderAMDCard = false;            ///< IS_AMD_CARD
    bool shaderIfChain = false;            ///< diffuseLookup() as an if chain; switches are unreliable on some NVIDIA drivers

    // Memory and identity
    U64 vramMB = 0;          ///< GL: kCGLRPVideoMemoryMegabytes; Metal: recommendedMaxWorkingSetSize
    bool unifiedMemory = false;
    bool isApple = false;    ///< keeps the GPU class cap at class 3 on Apple Silicon
    std::string vendor;
    std::string renderer;
    std::string driverVersion;
};

// [Viewer Lab] The feature table's texture-unit lists on Metal. Linden's
// featuretable_mac.txt has TexUnit8orLess and TexUnit16orLess, which LLFeatureManager applies when the device has that
// many texture units or fewer; TexUnit16orLess caps RenderTerrainPBRDetail at -1, because the PBR terrain at detail 0
// samples 17 textures in its fragment stage. Linden applies the lists to the graphics levels only, so a start that
// applies no level (a settings file that names the machine, as golden level 2's profile does) keeps a saved or default
// value above them. On GL that is Linden's behaviour and stays so; on Metal a program above the argument tables'
// samplers fails, and the viewer closes and starts OpenGL next time, so Metal caps the settings themselves.

/// Settings and the highest value each may have, by setting name.
using SettingCaps = std::map<std::string, F32>;

/// The texture units the feature table's TexUnit<n>orLess lists judge a device by: GL's GL_MAX_TEXTURE_IMAGE_UNITS
/// (gGLManager.mNumTextureImageUnits, from the system's GL on Metal too), and on Metal no more than the
/// samplers a stage binds (maxSamplersPerStage: 16), whatever the system's GL reports.
S32 featureTableTextureUnits(const Capabilities& caps, S32 systemTextureUnits);

/// The caps the feature table's texture-unit lists put on settings whatever the graphics level. `lists` holds the
/// table's lists by name; those named TexUnit<n>orLess apply when featureTableTextureUnits is n or less, and each caps
/// the settings it names at its value (the smallest when two name one). Only on Metal: on GL the result is empty, so
/// GL keeps Linden's behaviour.
SettingCaps hardwareSettingCaps(const Capabilities& caps, S32 systemTextureUnits, const std::map<std::string, SettingCaps>& lists);

/// `value` held to the setting's cap in `caps`, or unchanged when the setting has none.
F32 cappedSetting(const SettingCaps& caps, const std::string& name, F32 value);
// [Viewer Lab] end

// ------------------------------------------------------------------------------------------------
// Objects

/// Base of every interface object. Reference counted with LLPointer, as LL's code owns objects.
/// The id is fixed and never reused; it replaces the raw GL name as identity in hashes, caches
/// (for example the UI cache key) and memory accounting. When the count reaches zero, the
/// backend's destructor gives its native objects to FrameLifecycle::release, and they go when the
/// GPU has finished every frame and upload that used them.
class Object : public LLThreadSafeRefCount
{
public:
    U64 id() const { return mId; }
    const std::string& debugName() const { return mDebugName; }

    /// Becomes Metal's label: always for pipelines, passes and command buffers, for textures and
    /// buffers only with the Metal debug setting.
    virtual void setDebugName(std::string_view name) { mDebugName = name; }

    // A copy would share the id.
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

protected:
    Object();
    ~Object() override = default;

private:
    const U64 mId;
    std::string mDebugName;
};

template <class T>
using Ref = LLPointer<T>;

/// Signalled when the GPU has finished the work it guards. A fence that guards no work of the
/// backend is signalled at once, as the GL adapter's for LLImageGL's own uploads.
class Fence : public Object
{
public:
    virtual bool signaled() const = 0;
    virtual void wait() = 0;
};

// Buffers --------------------------------------------------------------------------------------

enum BufferUsage : U8
{
    BUFFER_VERTEX = 1 << 0,
    BUFFER_INDEX = 1 << 1,
    BUFFER_UNIFORM = 1 << 2,
    BUFFER_STAGING = 1 << 3,
    BUFFER_READBACK = 1 << 4,
    /// Read and written by native programs (setNativeBuffer with Access::Write); Capabilities::computePasses. Its
    /// memory is like any buffer's (Shared on Metal), so the CPU reads what a dispatch wrote once its frame finished.
    BUFFER_STORAGE = 1 << 5,
};

struct BufferDesc
{
    U64 size = 0;
    U8 usage = BUFFER_VERTEX;
    bool dynamic = false; ///< gets new memory at every update; the old goes when the GPU is done
};

/// Small buffers come from pooled, always-resident MTLBuffers on Metal. Contents change
/// through the encoder or the upload context, never on the object itself, so recording from
/// several threads can come later without changing the interface.
class Buffer : public Object
{
public:
    virtual const BufferDesc& desc() const = 0;
};

// Textures and samplers ------------------------------------------------------------------------

enum TextureUsage : U8
{
    TEXTURE_SAMPLED = 1 << 0,
    TEXTURE_RENDER_TARGET = 1 << 1,
    TEXTURE_READBACK = 1 << 2, ///< may be copied to the CPU
    /// Written by native programs (setNativeTexture with Access::Write); Capabilities::computePasses.
    TEXTURE_STORAGE = 1 << 3,
    /// A render target that lives only while its pass draws: never sampled, read back, copied or written by native
    /// programs, and its contents lost at every pass break (a segment's end and resume). On Metal memoryless storage,
    /// in tile memory, taking no memory of its own; with RENDER_TARGET only; Capabilities::computePasses (Metal).
    TEXTURE_MEMORYLESS = 1 << 4,
};

struct TextureDesc
{
    TextureType type = TextureType::Tex2D;
    Format format = Format::RGBA8Unorm;
    U32 width = 1;
    U32 height = 1;
    U32 layers = 1;     ///< cube arrays: cubes
    U32 mipLevels = 1;
    U32 samples = 1;    ///< render targets only
    U8 usage = TEXTURE_SAMPLED;
    SwizzleRGBA swizzle;
    /// The facade's own GL internal format, which the GL backend asks GL for, so GL's behavior does
    /// not change where several GL formats become one format (RGB8, 24-bit depth, unsized
    /// formats). 0 means toGLInternalFormat(format). Other backends read only whether it has alpha
    /// (hasAlphaInGLFormat), so a four-component upload to a texture GL keeps without alpha has
    /// alpha 1, as on GL.
    U32 glFormat = 0;

    bool operator==(const TextureDesc&) const = default;
};

/// The backend never reads sampling from the texture: the facade gives the sampling at each binding
/// (Encoder::setTexture), and the backend picks a sampler from it and the program's sampler type at
/// draw time. What a facade remembers of a texture's sampling between bindings, as GL
/// keeps it on the texture today, it keeps in facadeState(), which no backend reads.
class Texture : public Object
{
public:
    virtual const TextureDesc& desc() const = 0;

    /// Facade memory, which no backend reads: what a facade remembers of the texture between
    /// bindings. LLTexUnit packs into it, on the main thread only, the sampling it last applied to
    /// the texture, as GL keeps a texture's sampling on it today. 0 until a facade sets it.
    /// The atomic only keeps a stray read defined; a facade reads and writes it on one thread.
    U64 facadeState() const { return mFacadeState.load(std::memory_order_relaxed); }
    void setFacadeState(U64 state) { mFacadeState.store(state, std::memory_order_relaxed); }

private:
    std::atomic<U64> mFacadeState{ 0 };
};

/// Immutable; the device makes samplers from a description and reuses them.
struct SamplerDesc
{
    Filter filter = Filter::Bilinear;
    /// The minification uses the texture's mips (GL's *_MIPMAP_* filters, Metal's mipFilter);
    /// LLTexUnit's mip flag.
    bool mipmapped = true;
    F32 maxAnisotropy = 1.f; ///< GL_TEXTURE_MAX_ANISOTROPY; 1 is off
    MagFilter magFilter = MagFilter::FromFilter; ///< GL_TEXTURE_MAG_FILTER apart from filter
    /// GL applies addressU to S, addressV to T and addressW to R; LLTexUnit::setTextureAddressMode sets all three alike, as
    /// today, and setActiveTextureWrapST sets U and V.
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    bool compare = false; ///< sampler2DShadow gets a comparing sampler
    CompareFunc compareFunc = CompareFunc::LessEqual;

    bool operator==(const SamplerDesc&) const = default;
};

struct SamplerDescHash
{
    size_t operator()(const SamplerDesc& desc) const;
};

class Sampler : public Object
{
public:
    virtual const SamplerDesc& desc() const = 0;
};

struct Region
{
    U32 x = 0;
    U32 y = 0;
    U32 width = 0;
    U32 height = 0;
};

// Readbacks ------------------------------------------------------------------------------------

/// A copy of texture or buffer data to the CPU, recorded on the encoder with GL's format and type,
/// as in glReadPixels. Rows come in GL's order (row 0 is the bottom row in GL's terms), and the
/// Metal backend converts to the asked format, for example RGBA to RGB or depth to float. On GL the
/// data is read synchronously as today, so ready() is true at once. On Metal wait() ends the
/// current pass, sends the command buffer and waits, as glReadPixels waits on GL. Synchronous call
/// sites keep their behavior; asynchronous use comes per call site later.
/// On GL, inside LLImageGL::OwnGLTransfers, the facade makes today's GL call itself, into
/// its caller's memory: the GL backend only checks the readback, which comes back ready and without
/// data, and only checks a copy, with no GL call and no flush.
class Readback : public Object
{
public:
    virtual bool ready() const = 0;
    virtual void wait() = 0;
    virtual std::span<const std::byte> data() const = 0; ///< valid after ready() or wait()
    virtual U32 rowPitch() const = 0;
};

// Programs and reflection ----------------------------------------------------------------------

/// One stage as LLShaderMgr assembles it: the units in attach order, with the defines already
/// inserted. On Metal each program and stage becomes one glslang program, is translated to MSL,
/// and is kept in the disk cache with its reflection; locations are assigned for the whole
/// program, so the stages match.
struct StageSource
{
    ShaderStage stage = ShaderStage::Vertex;
    std::vector<std::string> units;
    /// Per unit, for the Metal translator's log and disk cache key: the file as the program names it
    /// (empty, or fewer entries than units: an internal program's unit), and the shader class the file was found in
    /// (0: not known, as for a shared object). The GL backend ignores both.
    std::vector<std::string> files;
    std::vector<S32> classes;
};

struct ProgramDesc
{
    std::string name;                  ///< LLGLSLShader::mName, used in logs and labels
    std::vector<StageSource> stages;
    /// LLVertexBuffer MAP_* bits the program reads: a hint the GL backend ignores; the program's mask
    /// is its reflection's. LLGLSLShader gives 0, because it is known only after the
    /// link.
    U32 attributeMask = 0;
    std::vector<std::string> reservedAttributes; ///< LLShaderMgr::mReservedAttribs, by index
    bool loginScreen = false;          ///< compiled first at startup
    /// LLGLSLShader::mShaderLevel for this attempt, lower after a fall-back; 0 when not known. The
    /// GL backend ignores it; on Metal it goes into the translation's classes, so a fall changes the disk cache key.
    S32 shaderLevel = 0;
    /// The uniform blocks the program may read, by slot (LLGLSLShader::UniformBlock): slot i is
    /// uniformBlocks[i], and Metal's buffer for it is 1 + i (llrhimsl.cpp). The names have static
    /// lifetime; empty: no block. The GL backend ignores it, as GL binds the blocks by name.
    std::span<const char* const> uniformBlocks;
};

enum class UniformType : U8
{
    Float,
    Vec2,
    Vec3,
    Vec4,
    Int,
    IVec2,
    IVec3,
    IVec4,
    Mat2,
    Mat3,
    Mat3x4,
    Mat4,
    Sampler2D,
    Sampler2DShadow,
    SamplerCube,
    SamplerCubeArray,
    /// A sampler3D, on Metal only (the translation's reflection); GL's reflection keeps GL_SAMPLER_3D as Sampler2D.
    /// It reads the channel's Tex3D slot.
    Sampler3D,
};

/// Bytes of one element as the setters take it: tightly packed, matrices column-major.
/// A sampler is its texture channel, one U32.
U32 uniformSize(UniformType type);
bool isSampler(UniformType type);

/// The reflection the facades see, the same for every backend: on GL from the driver as today
/// (mapUniforms), on Metal from SPIRV-Cross. Offsets, strides and slots are the
/// backend's own business and stay inside it.
struct UniformInfo
{
    std::string name;
    UniformType type = UniformType::Float;
    U32 arraySize = 1;
};

struct UniformBlockInfo
{
    std::string name;
    U32 size = 0; ///< bytes
};

struct ProgramReflection
{
    /// The default block's uniforms that have a location, in the backend's order (the driver's on
    /// GL), named without [0]; arraySize is the active size the backend reports. Samplers
    /// included; the index is the setter's index.
    std::vector<UniformInfo> uniforms;
    /// The program's blocks with name and size, in the backend's order (the driver's on GL); the
    /// index is not the encoder's slot (Encoder::setUniformBuffer).
    std::vector<UniformBlockInfo> blocks;
    U32 attributeMask = 0;
};

enum class CompileStatus : U8
{
    Pending, ///< translated or compiled in the background
    Ready,
    Failed,  ///< logged with the program name and the translator's log; LLShaderMgr falls back
};

/// A linked program. When it dies (also when shaders are reloaded), the backend tells the
/// PipelineCache, so the program's pipelines go too.
class Program : public Object
{
public:
    virtual CompileStatus status() const = 0;
    /// Blocks until Ready or Failed; afterwards reflection() is complete. On GL, the program adopted
    /// from LLGLSLShader is reflected from the driver here, through LLGLSLShader's GL
    /// reflector, the first time it is called after LLGLSLShader has linked it.
    virtual void wait() = 0;
    virtual const ProgramReflection& reflection() const = 0;
};

// Pipelines and depth state --------------------------------------------------------------------

/// Vertex data is non-interleaved, as LLVertexBuffer lays it out: each attribute is its own
/// stream in one buffer, with a fixed format and stride per attribute type (position float3 with
/// stride 16, the texture index as uint in position.w, colors ubyte4 normalized, joints ushort4
/// and so on). The vertex input therefore depends only on the attribute mask, which is what the
/// pipeline holds; the offsets, which grow with the vertex count, are given when the buffer is
/// bound. On Metal the streams use buffer slots attribute index + 16.
struct VertexStreams
{
    static constexpr U32 MAX_ATTRIBUTES = 14; ///< LLVertexBuffer::TYPE_MAX
    U32 attributeMask = 0;
    std::array<U32, MAX_ATTRIBUTES> offset{}; ///< byte offset of each attribute's stream
};

struct BlendState
{
    bool enabled = false;
    BlendFactor srcColor = BlendFactor::One;
    BlendFactor dstColor = BlendFactor::Zero;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::Zero;
    BlendOp colorOp = BlendOp::Add;
    BlendOp alphaOp = BlendOp::Add;

    bool operator==(const BlendState&) const = default;
};

/// Metal: a description in which every used color attachment blends has one unspecialized pipeline
/// per program, attribute mask, primitive class and sample count, specialized per attachment
/// formats, blend factors and color mask (0.02-0.04 ms each); one with an attachment whose blending
/// ends Disabled (blending off, or an integer format) has a full pipeline per attachment formats,
/// blend and color mask. The device caches on the program's id
/// together with this description (PipelineCache). The point variant of a program is part of the
/// key through the primitive class. Fill mode and depth clamp are dynamic state; a backend for
/// which they are pipeline state resolves them at draw time. Color formats past colorCount stay
/// Unknown, so equal pipelines have equal keys.
struct PipelineDesc
{
    static constexpr U32 MAX_COLOR_ATTACHMENTS = 8;

    U32 attributeMask = 0;
    PrimitiveClass primitiveClass = PrimitiveClass::Triangle;
    U32 samples = 1;
    U32 colorCount = 1;
    std::array<Format, MAX_COLOR_ATTACHMENTS> colorFormats{};
    Format depthFormat = Format::Unknown;
    BlendState blend;          ///< one for all attachments, as LLRender sets it
    U8 colorMask = COLOR_MASK_ALL;

    bool operator==(const PipelineDesc&) const = default;
};

struct PipelineKey
{
    U64 programId = 0;
    PipelineDesc desc;

    bool operator==(const PipelineKey&) const = default;
};

struct PipelineKeyHash
{
    size_t operator()(const PipelineKey& key) const;
};

/// Holds the program's id, not the program. A draw with a pipeline that is still compiling waits
/// for it.
class Pipeline : public Object
{
public:
    virtual U64 programId() const = 0;
    virtual const PipelineDesc& desc() const = 0;
    virtual CompileStatus status() const = 0; ///< pipelines compile in parallel
    /// A render pipeline of a native library (Device::getNativePipeline): programId() is the library's id, its
    /// bindings are the native ones (Encoder::setNative*), and it draws only in a native pass.
    virtual bool native() const { return false; }
};

/// No stencil operations: the viewer uses stencil only in dead code.
struct DepthStencilDesc
{
    bool depthTest = false;
    bool depthWrite = false;
    CompareFunc depthFunc = CompareFunc::LessEqual;

    bool operator==(const DepthStencilDesc&) const = default;
};

struct DepthStencilDescHash
{
    size_t operator()(const DepthStencilDesc& desc) const;
};

/// Immutable; the device makes them from a description and reuses them.
class DepthStencilState : public Object
{
public:
    virtual const DepthStencilDesc& desc() const = 0;
};

// Render passes ---------------------------------------------------------------------------------

struct ColorAttachment
{
    Ref<Texture> texture;
    U32 level = 0;
    U32 layer = 0;      ///< cube face, or cube * 6 + face in a cube array
    LoadAction load = LoadAction::Load;
    StoreAction store = StoreAction::Store;
    std::array<F32, 4> clear{ 0.f, 0.f, 0.f, 0.f };
};

struct DepthAttachment
{
    Ref<Texture> texture;
    U32 level = 0;
    U32 layer = 0;
    LoadAction load = LoadAction::Load;
    StoreAction store = StoreAction::Store;
    F32 clear = 1.f;
    /// The pass samples this depth while it tests against it, as the deferred lights do. Depth
    /// writes and clears must stay off (EncoderTracker checks it); sampling a depth attachment
    /// with writes off was measured to work on Metal 4, and an API that needs it, such as Vulkan,
    /// uses a read-only layout.
    bool readOnly = false;
};

/// The attachments' sizes at their levels must be equal, and so must their sample counts.
struct RenderPassDesc
{
    std::array<ColorAttachment, PipelineDesc::MAX_COLOR_ATTACHMENTS> color;
    U32 colorCount = 0;
    DepthAttachment depth;
    std::string name; ///< debug group and label; kept, because a pass break restarts the pass
    /// A pass of a facade's target stack (LLRenderTarget): the bound target's, or the
    /// window's back buffer when none is bound. GPU zones span LLRenderTarget's binds, so debug
    /// groups may begin before such a pass and end in it, or begin in it and end after it: the
    /// tracker asks no backend to close a group when a stack pass ends, and a backend whose native
    /// groups belong to an encoder keeps them open across its encoders (on Metal 4 the command
    /// buffer's groups). A stack pass still open when the frame ends ends with it, without a
    /// warning; the facade begins it again when it needs it.
    bool stack = false;
    /// A pass of native programs (Device::getNativePipeline), whose bindings are the native ones. Begun while a stack
    /// pass is open, it suspends that pass: the stack pass's recorded work is stored, and when the native pass ends,
    /// the stack pass goes on with its attachments loaded, its dynamic state and its draw bindings as they were. Any
    /// other pass still refuses a pass inside it.
    bool native = false;
};

// Queries --------------------------------------------------------------------------------------

/// Occlusion queries live in 256 KB visibility buffers on Metal 4 (at most 32,768 queries each,
/// because larger offsets read 0 without an error), with Accumulate in every pass and the buffer
/// zeroed on reuse. Time queries are timestamps at pass boundaries; a query that starts before the
/// first draw or ends after the last lies on the boundary, otherwise the pass is split. A query
/// may span the backend's own pass breaks; a backend whose native queries cannot do that sums
/// several of them. A result is ready when its frame's work has finished.
///
/// Device::createQueries makes queries, and the encoder begins each one again and again, as GL's
/// names are; a begin discards the previous result. ready() never waits. result(true) waits; on
/// Metal it sends the frame's work and waits, as Readback::wait does. A query is
/// made, begun, ended, read and released on the main thread only.
class Query : public Object
{
public:
    virtual QueryType type() const = 0;
    virtual bool ready() const = 0;
    /// Samples, nanoseconds or primitives. AnySamplesPassed's is non-zero when at least one sample
    /// passed: a boolean from GL 3.3, and below it a sample count, because the GL backend asks GL for
    /// GL_SAMPLES_PASSED there.
    virtual U64 result(bool wait) = 0;
};

// Native programs, compute and transient textures ----------------------------------------------
//
// Native programs are MSL written by hand (photo mode's own passes on Metal), compiled at run time from source. Their
// bindings are slots: the MSL index itself, with no texture channels and no reflection in between. Constants go to the
// frame's ring (setNativeBytes, setNativeConstants); textures, samplers and buffers are bound per slot. GL has none of
// it: Capabilities::computePasses and nativePrograms are false there, and the defaults below refuse with one WARN line.

/// The stage a native binding is for: a native render pipeline's vertex or fragment function, or a compute kernel.
enum class NativeStage : U8
{
    Vertex,
    Fragment,
    Compute,
};

/// How a native binding uses its resource. Write and ReadWrite need TEXTURE_STORAGE (BUFFER_STORAGE), and a written
/// texture binding covers exactly one mip level (TextureRange), which the backend binds as a view of that level.
enum class Access : U8
{
    Read,
    Write,
    ReadWrite,
};

/// Mip levels of a native texture binding: levelCount 0 is every level from baseLevel.
struct TextureRange
{
    U32 baseLevel = 0;
    U32 levelCount = 0;

    bool operator==(const TextureRange&) const = default;
};

/// The levels a range covers in a texture of mipLevels levels: [first, end); empty when baseLevel is past the end.
inline std::pair<U32, U32> rangeLevels(const TextureRange& range, U32 mipLevels)
{
    const U32 first = range.baseLevel;
    const U32 end = range.levelCount == 0 ? mipLevels : first + range.levelCount;
    return { first, first < mipLevels && end <= mipLevels && end > first ? end : first };
}

struct DispatchSize
{
    U32 x = 1;
    U32 y = 1;
    U32 z = 1;

    bool operator==(const DispatchSize&) const = default;
    U64 count() const { return static_cast<U64>(x) * y * z; } // not U64(x): OpenSSL defines a U64 macro
};

struct ComputePassDesc
{
    std::string name; ///< debug group and label
};

/// The native bindings' slots: the MSL index of a texture, sampler or buffer. Buffers from 16 belong to the vertex
/// streams of translated programs, so native buffers stop at 16; textures and samplers stop at the argument tables'.
inline constexpr U32 NATIVE_BUFFER_SLOTS = 16;
inline constexpr U32 NATIVE_TEXTURE_SLOTS = 16;
inline constexpr U32 NATIVE_SAMPLER_SLOTS = 16;
/// The largest setNativeBytes: one copy into the frame's ring.
inline constexpr U32 NATIVE_BYTES_MAX = 64 * 1024;
/// Threadgroup memory slots and the most threads a threadgroup may have on Apple GPUs.
inline constexpr U32 NATIVE_THREADGROUP_SLOTS = 31;
inline constexpr U32 NATIVE_THREADS_PER_GROUP_MAX = 1024;

/// MSL source with its includes resolved; LLNativeProgram reads the files.
struct NativeSource
{
    std::string name; ///< labels and logs
    std::string text;
    std::vector<std::pair<std::string, std::string>> defines; ///< preprocessor macros, name and value
};

/// One compiled MSL library. It compiles in the background, as a Program does, with the options of the translated
/// programs (MSL 4.0, the same math).
class NativeLibrary : public Object
{
public:
    virtual CompileStatus status() const = 0;
    /// Blocks until Ready or Failed.
    virtual void wait() = 0;
    /// The compiler's message: its errors when Failed, its warnings when Ready, empty while Pending.
    virtual std::string log() const = 0;
};

struct ComputePipelineDesc
{
    std::string function;
    /// The threadgroup size the kernel is compiled for (Metal's requiredThreadsPerThreadgroup); zeros let each
    /// dispatch choose.
    DispatchSize threadsPerThreadgroup{ 0, 0, 0 };

    bool operator==(const ComputePipelineDesc&) const = default;
};

class ComputePipeline : public Object
{
public:
    virtual CompileStatus status() const = 0;
    /// Blocks until Ready or Failed.
    virtual void wait() = 0;
    virtual U64 libraryId() const = 0;
    virtual const ComputePipelineDesc& desc() const = 0;
    /// threadExecutionWidth (32 on Apple GPUs) and the most threads one threadgroup may have; 0 until Ready.
    virtual U32 executionWidth() const = 0;
    virtual U32 maxThreadsPerThreadgroup() const = 0;
};

/// A native render pipeline's functions. The vertex function makes its own vertices (no vertex streams), so its
/// PipelineDesc has attributeMask 0 and its draws are draw(Triangles, 0, 3) without a vertex buffer.
struct NativeRenderFunctions
{
    std::string vertex;   ///< usually the common fullscreen triangle
    std::string fragment;

    bool operator==(const NativeRenderFunctions&) const = default;
};

/// A texture of a transient pool: its description and the phases of the frame in which it is used. Two textures
/// whose phases do not overlap may share memory once the pool has a heap; until then each is a texture of its own.
struct TransientTextureDesc
{
    TextureDesc desc;  ///< usage may include TEXTURE_STORAGE
    U32 firstPhase = 0;
    U32 lastPhase = 0; ///< inclusive
    std::string name;
};

struct TransientPoolDesc
{
    std::string name;
    std::vector<TransientTextureDesc> textures;
};

/// The textures of one pool, laid out once for their phases. A texture's contents are undefined at the start of its
/// first phase: its first use there must write it, never load it.
class TransientPool : public Object
{
public:
    /// Null past the pool's textures, or when the texture could not be made (one WARN line at creation).
    virtual Texture* texture(U32 index) const = 0;
    virtual U32 textureCount() const = 0;
    virtual const TransientPoolDesc& desc() const = 0;
    /// The memory the pool holds: with its textures as their own allocations, the sum of their sizes.
    virtual U64 heapBytes() const = 0;
    /// What the textures would take as their own allocations.
    virtual U64 dedicatedBytes() const = 0;
};

/// A description's bytes as one allocation: the format's texel or block bytes over every level and slice, without the
/// backend's alignment. For TransientPool's numbers and memory plans.
U64 textureBytes(const TextureDesc& desc);

// The encoder ----------------------------------------------------------------------------------

class EncoderTracker;

/// Records render passes, state, bindings, uniforms, draws, updates, copies, readbacks and
/// queries. The main thread records every frame. Everything that changes per draw lives here
/// rather than on shared objects, so recording from several threads later needs only an encoder
/// per thread.
///
/// Dynamic state is in GL's terms. On GL every setter goes to GL at once and is never
/// applied again at draw time, and enables go through LLGLState, so raw GL in code not ported yet
/// behaves as today. On Metal the backend applies it to the render encoder. On GL,
/// LLRender's facades and LLGLState make their own GL calls in GL backend functions and give the
/// same state inside LLRender::OwnGLState, which the GL backend only tracks.
///
/// Bindings (pipeline, depth state, buffers, textures) last until they are changed, across passes
/// and frames, as on GL: the facades skip binding what they think is still bound. Each backend sets
/// them again when a pass begins, as Metal must for every new native encoder; the GL backend does,
/// because raw GL between passes may have bound its own. On GL the passes of LLRenderTarget's
/// stack (RenderPassDesc::stack) are the exception: LLRenderTarget makes their GL calls itself, so
/// they apply no bindings when they begin, and a binding set while no pass is open reaches GL only
/// when an ordinary pass begins. Code that binds through the interface therefore begins the
/// stack's pass first (LLRenderTarget::beginStackPass). A binding whose object nobody else
/// holds any more is dropped at the end of the frame, as GL unbinds a deleted texture.
///
/// Dynamic state (viewport, scissor, cull, fill, depth bias, depth clamp, point size, line width)
/// is set inside a pass and lasts until the pass ends. Each pass begins with the defaults: the
/// viewport of the whole attachment, no scissor, no culling, filled polygons, no depth bias, no
/// depth clamp, and points and lines of 1 pixel. A pass is a scope for GL state on GL: what the
/// pass changed is put back when it ends, so raw GL after it sees the state it had before, as
/// after an LLGLEnable scope. The passes of LLRenderTarget's stack are the exception
/// on GL: the GL backend only tracks them, so they save, set and put back nothing, and the
/// viewer's GL state applies in them as before. On other backends they begin with the defaults
/// like any pass, and the facades set the viewer's dynamic state again when one begins.
/// The viewer's dynamic state in a stack pass therefore goes through the facades and
/// LLGLState: a direct setter of the encoder goes around LLRender's memory of it (all but
/// setLineWidth, which the GL backend gives LLRender), so GL keeps the value after the pass, but the
/// next stack pass on another backend begins with LLRender's values.
///
/// Raw GL inside a pass applies until the interface sets the same thing again, and draws through
/// the interface see it, as raw scopes around a draw expect today: dynamic state,
/// and also a program, buffers, textures or a framebuffer raw GL binds itself. Nothing is applied
/// again at draw time. Metal can only be chosen when no raw GL is left, so the difference
/// never shows there; the next pass begins with the interface's bindings again.
///
/// Every backend's encoder runs its calls through an EncoderTracker, which keeps a reference to
/// each bound object and checks the calls. A call the tracker rejects is logged as a WARN line with
/// the reason and skipped; a rejected binding is bound to null natively instead, as the tracker
/// does, so no draw uses the object that was there before.
class Encoder
{
public:
    virtual ~Encoder() = default;

    /// False, after the WARN line, when the pass was refused (a pass is still open, or the
    /// description is invalid) and none began. A facade that keeps a record of its passes records
    /// only one that began (LLRenderTarget).
    virtual bool beginRenderPass(const RenderPassDesc& pass) = 0;
    virtual void endRenderPass() = 0;

    /// glClear, also in the middle of a pass (the sky clears depth inside the G-buffer pass). The
    /// pass must have what is cleared, and a read-only depth is never cleared. As glClear, the
    /// scissor, the color mask and the depth write mask limit it.
    /// At the start of a pass it becomes the load action. Later the Metal backend ends the pass and
    /// starts a new one that clears the asked attachments and loads the others. A clear limited by
    /// the scissor or the color mask is a full-screen draw instead.
    /// On GL, LLRenderTarget::clear and clearCurrent clear the stack's pass through glClear with
    /// GL's own clear colour and only send the call here to be tracked; any other call clears with
    /// the values given, through glClearBufferfv, in a stack pass as in any pass.
    virtual void clear(bool color, const std::array<F32, 4>& rgba, bool depth, F32 depthValue) = 0;

    virtual void setPipeline(Pipeline* pipeline) = 0;
    virtual void setDepthStencilState(DepthStencilState* state) = 0;

    // Dynamic state
    virtual void setViewport(S32 x, S32 y, S32 width, S32 height) = 0; ///< GL's numbers, origin at row 0
    /// GL's numbers. Metal requires the rect inside the attachment, so the backend clips it (the
    /// UI adds 1 to its clip rects).
    virtual void setScissor(bool enabled, S32 x, S32 y, S32 width, S32 height) = 0;
    virtual void setCullMode(CullMode mode) = 0;
    virtual void setFillMode(FillMode mode) = 0;
    /// glPolygonOffset with GL's separate enables for fill and lines. Metal applies the bias
    /// (setDepthBias(units, factor, 0)) when the enable for the current fill mode is on.
    virtual void setDepthBias(F32 factor, F32 units, bool enableFill, bool enableLines) = 0;
    virtual void setDepthClamp(bool enabled) = 0; ///< GL_DEPTH_CLAMP, setDepthClipMode on Metal
    virtual void setPointSize(F32 size) = 0;      ///< glPointSize; a uniform in the point variant on Metal
    /// LLRender::setLineWidth, clamped to Capabilities::lineWidthRange: GL as today (Windows and
    /// Linux can draw wide lines), 1 pixel on Mac with both backends.
    virtual void setLineWidth(F32 width) = 0;

    // Uniforms. One setter per reflected uniform, by its index in the program's
    // reflection (validateUniform checks the value). Values are tightly packed, matrices
    // column-major; the facade transposes when a GL caller asked for transpose. GL calls
    // glUniform* as today. Metal writes a CPU copy of the program's default block per stage and
    // copies only changed blocks to the frame's ring buffer at draw time. LLGLSLShader keeps its
    // value cache. On GL, LLGLSLShader's own calls are only checked
    // (LLGLSLShader::makesOwnGLUniforms), because it makes today's glUniform* call itself.
    virtual void setUniform(Program* program, U32 uniformIndex, std::span<const std::byte> value,
                            U32 count = 1) = 0;
    /// uniform1i on a sampler uniform: on Metal it updates the table from texture channel to slot,
    /// so code in newview that fixes channels (texture0 = 0, texture1 = 1) works unchanged.
    virtual void setSamplerChannel(Program* program, U32 uniformIndex, U32 channel) = 0;

    // Bindings: on Metal only through the binding site, which also makes the resources resident
    // and, in debug and test builds, checks residency and fails with the resource's name.
    /// The streams' offsets are given here, not in the pipeline (on Metal they become the argument
    /// table's addresses).
    virtual void setVertexBuffer(Buffer* buffer, const VertexStreams& streams) = 0;
    virtual void setIndexBuffer(Buffer* buffer, IndexType type) = 0;
    /// LLTexUnit's channel, with the sampling the texture wants (LLImageGL's filter, address modes
    /// and anisotropy, which the facade remembers as today). The sampler is chosen at draw time
    /// from this and the program's sampler type. A channel the program samples but
    /// nothing is bound to reads a default texture of the sampler's type that returns
    /// (0, 0, 0, 1), as on GL. Null unbinds the channel.
    virtual void setTexture(U32 channel, Texture* texture, const SamplerDesc& sampling) = 0;
    /// blockIndex is a slot shared by every program, like GL's binding points. For LLGLSLShader's
    /// named blocks the slot is their UniformBlock, read by every program with the block of that
    /// name whatever its index in the program's reflection. On GL that is binding point
    /// LLGLSLShader::INTERFACE_BLOCK_BINDING + slot, which LLGLSLShader's programs bind while an
    /// LLShaderUniformBuffer exists for the block or the block is declared
    /// (LLShaderUniformBuffer::declareBlock). A block of the GL backend's own
    /// programs that is none of them is slot NUM_UNIFORM_BLOCKS + its index; Metal's translation
    /// refuses such blocks (llrhimsl.cpp).
    virtual void setUniformBuffer(U32 blockIndex, Buffer* buffer, U64 offset, U64 size) = 0;

    // Draws
    virtual void draw(PrimitiveType type, U32 firstVertex, U32 vertexCount) = 0;
    virtual void drawIndexed(PrimitiveType type, U32 firstIndex, U32 indexCount) = 0;

    // Updates: GL semantics. The data is copied at the call and applies to all later
    // draws; call sites stay as they are. In the middle of a render pass the backend ends the
    // pass, copies and goes on in a new pass that loads the attachments. When recording from
    // several threads comes, updates inside a pass are collected and run before the pass
    // instead, because Metal 4's suspended passes must not mix with copies.
    virtual void updateBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data) = 0;
    /// Rows are in GL's order: row 0 is the bottom row in GL's terms, and nothing is ever flipped.
    /// The Metal backend widens RGB data and handles swapped bytes.
    virtual void uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region,
                               const PixelLayout& layout, std::span<const std::byte> data) = 0;
    /// Marks the level and layer as never written again, as in a texture made without
    /// data, so the next write into part of it leaves GL's values of new storage around it (0, alpha 1 where the GL
    /// format keeps none), as GL's new name per media update does. For a texture updated in place instead of a new one.
    /// Nothing by default: GL's names are re-specified by GL itself; the Metal backend clears its written mark.
    virtual void forgetWritten(Texture* /*texture*/, U32 /*level*/, U32 /*layer*/) {}

    // Copies outside passes, with a pass break like updates. Blits when format, sample count and
    // size match, otherwise a draw. Rows are never flipped.
    // On GL, inside LLImageGL::OwnGLTransfers, the facade makes today's GL call itself, into
    // its caller's memory: the GL backend only checks the readback, which comes back ready and
    // without data, and only checks a copy, with no GL call and no flush.
    virtual void copyTexture(Texture* src, U32 srcLevel, U32 srcLayer, const Region& srcRegion,
                             Texture* dst, U32 dstLevel, U32 dstLayer, U32 dstX, U32 dstY) = 0;
    virtual void generateMipmaps(Texture* texture) = 0;

    /// A GPU-side wait: this encoder's later work runs after the work an upload
    /// context's fence guards, and the CPU does not wait. A null fence, or one that guards no work of
    /// the backend, is no wait. On GL it is keyed on the fence's type, not on LLImageGL's scope: the
    /// fence the upload context gives inside LLImageGL's scope makes no GL call here, as LLImageGL
    /// orders its own uploads (gl_sync_to_main_thread), and GL's own fence is glWaitSync. Metal's next
    /// submit waits on the upload queue's event, and it makes here the mips the upload thread
    /// asked for before the fence.
    virtual void waitForFence(Fence* fence) = 0;

    // Readbacks and queries
    virtual Ref<Readback> readback(Texture* texture, U32 level, U32 layer, const Region& region,
                                   const PixelLayout& layout) = 0;
    virtual Ref<Readback> readback(Buffer* buffer, U64 offset, U64 size) = 0;
    /// Begins a query of the device's; false, after the tracker's WARN line, when the tracker refuses it. On GL begin
    /// and end go to GL at once and do not flush LLRender's pending batch, so a query covers what reached GL, as
    /// today's raw calls do.
    virtual bool beginQuery(Query* query) = 0;
    virtual void endQuery(Query* query) = 0;

    // Debug groups: LL_PROFILE_GPU_ZONE through LLGPUZone on the main thread, popped only on the
    // device it was pushed on, in developer builds and builds with Tracy and free in release.
    // A group opened in a pass closes in it, except around stack passes
    // (RenderPassDesc::stack).
    virtual void pushDebugGroup(std::string_view name) = 0;
    virtual void popDebugGroup() = 0;

    // Compute passes (Capabilities::computePasses). Begun while a stack pass is open, a compute pass suspends it as a
    // native render pass does (RenderPassDesc::native): what the stack pass recorded is stored, and when the compute
    // pass ends, the stack pass goes on with its attachments loaded and its dynamic state and draw bindings as they
    // were. Any other open pass refuses one. Inside a compute pass only native bindings, dispatches and debug groups are
    // taken: updates, uploads, copies, readbacks and mipmaps are refused (record them before). Dispatches that read or
    // write what an earlier dispatch of the pass wrote get a barrier between them, and the pass's end makes what it
    // wrote visible to every later stage of the queue: there is no barrier call. A Write binding marks its level as
    // written, so a later pass that loads it keeps what the dispatch wrote. The defaults refuse with one WARN line (GL).
    virtual bool beginComputePass(const ComputePassDesc& pass);
    virtual void endComputePass();
    virtual void setComputePipeline(ComputePipeline* pipeline);
    /// [[threadgroup(index)]]'s bytes, a multiple of 16, for the following dispatches of the pass.
    virtual void setThreadgroupMemory(U32 index, U32 bytes);
    /// grid threads in all, in threadgroups of threadsPerGroup (the last ones partial); and whole threadgroups.
    virtual void dispatchThreads(DispatchSize grid, DispatchSize threadsPerGroup);
    virtual void dispatchThreadgroups(DispatchSize groups, DispatchSize threadsPerGroup);

    // Native bindings: the slot is the MSL index. Taken in a compute pass (stage Compute) or a native render pass
    // (Vertex, Fragment), and they last until that pass ends. A refused call leaves its slot empty. Writes from a
    // native draw (Write or ReadWrite in Vertex or Fragment) are marked written but get no barrier against other draws
    // of the same pass: a draw must not read what another draw of its pass writes. The defaults refuse with one WARN
    // line (GL).
    virtual void setNativeTexture(NativeStage stage, U32 slot, Texture* texture, Access access = Access::Read,
                                  TextureRange range = {});
    virtual void setNativeSampler(NativeStage stage, U32 slot, const SamplerDesc& sampling);
    /// A copy into the frame's ring, bound as the slot's buffer; at most NATIVE_BYTES_MAX.
    virtual void setNativeBytes(NativeStage stage, U32 slot, std::span<const std::byte> data);
    virtual void setNativeBuffer(NativeStage stage, U32 slot, Buffer* buffer, U64 offset, Access access = Access::Read);

    /// The frame enters a phase of a transient pool, for the pool's aliasing once it has a heap: nothing while its
    /// textures are their own allocations, and nothing by default.
    virtual void beginTransientPhase(TransientPool* /*pool*/, U32 /*phase*/) {}

    /// What this encoder's next draw is checked against, read only, so a facade chooses its
    /// pipeline and bindings against the encoder's own record: whether a pass is open (a stack
    /// pass or another), the pass's formats and what is bound. Every backend's encoder runs its
    /// calls through an EncoderTracker. On GL, between an LLVertexBuffer draw and the next
    /// interface call, it shows LLVertexBuffer's bindings (LLVertexBuffer::OwnGLDraws).
    virtual const EncoderTracker& state() const = 0;
};

/// A native program's constants (a struct of a group's shared header) through setNativeBytes.
template <class T>
void setNativeConstants(Encoder& encoder, NativeStage stage, U32 slot, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>, "native constants are copied as bytes");
    encoder.setNativeBytes(stage, slot, std::as_bytes(std::span<const T>(&value, 1)));
}

/// The upload thread's way in. It may create buffers, textures and samplers and fill
/// them, but not draw. submit() sends what it has recorded and returns a fence; a resource
/// uploaded here may be bound in a draw once that fence has signalled, or after the drawing
/// encoder's waitForFence on it. On Metal a texture whose mips the upload thread asked for
/// gets them only in that waitForFence, so it is bound after it. The backend then makes it
/// resident at the next pass boundary (and an API that needs it acquires it for the render queue).
/// On GL this is today's shared context with glFenceSync; on Metal 3 a queue of its own. LLImageGL's
/// own uploads keep that synchronization in llimagegl.cpp (gl_sync_to_main_thread), which orders the
/// main context after the upload before the swap makes the texture bindable, so the fence submit()
/// gives inside LLImageGL's scope is signalled at once. A resource released while an upload
/// uses it hands that upload's fence to the lifecycle.
class UploadContext
{
public:
    virtual ~UploadContext() = default;

    virtual void updateBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data) = 0;
    virtual void uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region,
                               const PixelLayout& layout, std::span<const std::byte> data) = 0;
    virtual void generateMipmaps(Texture* texture) = 0;
    virtual Ref<Fence> submit() = 0;
};

// The surface ------------------------------------------------------------------------------------

/// The window's presentation: a CAMetalLayer or the GL context. The back buffer is an ordinary
/// RGBA8Unorm texture of the window's size with a depth texture, in GL's row order, so readbacks
/// and copies from it work like from any target. At present one pass draws it flipped into the
/// drawable; the layer is opaque, has no colorspace and an explicit drawableSize. Test builds read
/// the drawable back and compare it with the back buffer.
class Surface : public Object
{
public:
    virtual Texture* backBuffer() = 0;
    virtual Texture* backBufferDepth() = 0;
    virtual void resize(U32 width, U32 height) = 0; ///< backing pixels
    virtual void setVSync(bool enabled) = 0;        ///< displaySyncEnabled on Metal, at runtime
    virtual void present() = 0;
};

// The device -----------------------------------------------------------------------------------

/// What the window gives the device: on Mac the LLOpenGLView, which holds the GL context or the
/// CAMetalLayer. The display pointer is for platforms whose surfaces need one.
struct NativeWindow
{
    void* display = nullptr;
    void* window = nullptr;
};

struct DeviceCreateInfo
{
    BackendChoice backend;
    NativeWindow window;
    U32 width = 0;
    U32 height = 0;
};

/// Creates resources, reports capabilities and runs frames. At most FrameLifecycle::
/// MAX_FRAMES_IN_FLIGHT frames are in flight; the recording thread waits if it gets further ahead.
///
/// Threads: the main thread records every frame. The upload thread may also call the create
/// functions and getSampler.
///
/// Errors: creation that fails returns null and logs a WARN line with the object's name and the
/// reason, from validate() or the backend's own error text.
class Device : public Object
{
public:
    /// The backend factory: null, with a WARN line, when the backend cannot be created. The GL
    /// backend needs the window's current GL context and an initialized LLGLManager; while
    /// LLGLManager is not initialized, it first has the window make that context
    /// (LLWindow::createGLContext).
    static Ref<Device> create(const DeviceCreateInfo& info);

    virtual BackendType backend() const = 0;
    virtual const Capabilities& capabilities() const = 0;
    /// Main thread. Empty when Encoder::copyTexture copies a one-sample texture of format from into one of format to
    /// on this device, otherwise the reason: copyRefusal(capabilities(), from, to), and on Metal also the format copy
    /// program and its pipeline for to, made and cached at the first ask. LLGPUCopy::
    /// canFromFramebuffer and the Metal encoder ask it before anything changes, so a copy that cannot be made is
    /// refused before the tracker and LLImageGL::scaleDown keeps the texture and its content.
    virtual std::string copyRefusalBetween(Format from, Format to);

    virtual Ref<Buffer> createBuffer(const BufferDesc& desc, std::span<const std::byte> initial = {},
                                     std::string_view debugName = {}) = 0;
    virtual Ref<Texture> createTexture(const TextureDesc& desc, std::string_view debugName = {}) = 0;
    virtual Ref<Sampler> getSampler(const SamplerDesc& desc) = 0; ///< cached
    virtual Ref<Program> createProgram(const ProgramDesc& desc) = 0; ///< returns at once; see status()
    /// Main thread. count queries of a kind, in one native allocation; empty, with a WARN line, off the main thread or
    /// for a kind the capabilities lack (validateQuery).
    virtual std::vector<Ref<Query>> createQueries(QueryType type, U32 count) = 0;
    /// The first of createQueries(type, 1), or null.
    Ref<Query> createQuery(QueryType type)
    {
        std::vector<Ref<Query>> queries = createQueries(type, 1);
        return queries.empty() ? Ref<Query>() : queries.front();
    }
    virtual Ref<Pipeline> getPipeline(Program& program, const PipelineDesc& desc) = 0; ///< cached
    virtual Ref<DepthStencilState> getDepthStencilState(const DepthStencilDesc& desc) = 0; ///< cached

    // Native programs (Capabilities::nativePrograms). The defaults refuse with one WARN line, so a backend without them
    // (GL) needs no code: the library comes back Failed and the pipelines null. Main thread.
    /// Returns at once; the library compiles in the background (NativeLibrary::status).
    virtual Ref<NativeLibrary> createNativeLibrary(const NativeSource& source);
    /// Cached per library and description; returns at once, Pending until compiled. Null for a library that failed.
    virtual Ref<ComputePipeline> getComputePipeline(NativeLibrary& library, const ComputePipelineDesc& desc);
    /// A Pipeline for setPipeline and checkPipelineForPass in a native pass (Pipeline::native), cached per library,
    /// functions and normalized description, apart from the translated programs' pipelines; desc.attributeMask must be
    /// 0. Returns at once, Pending until compiled. Null for a library that failed.
    virtual Ref<Pipeline> getNativePipeline(NativeLibrary& library, const NativeRenderFunctions& functions,
                                            const PipelineDesc& desc);
    /// The pool's textures (TransientPool). The default makes each as a texture of its own through createTexture,
    /// on a device with computePasses; without it, it refuses with one WARN line.
    virtual Ref<TransientPool> createTransientPool(const TransientPoolDesc& desc);

    virtual Surface* surface() = 0;
    virtual Encoder& encoder() = 0;             ///< the main thread's; one per thread when that comes
    virtual UploadContext& uploadContext() = 0; ///< the upload thread's

    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;    ///< sends the frame and frees what earlier frames released
    virtual U64 frameNumber() const = 0;
    virtual U64 completedFrame() const = 0;

    /// Sends what is recorded and waits for the GPU. Used by synchronous readbacks and by code that
    /// needs the GPU idle. What the current frame released stays until the frame has finished
    /// (FrameLifecycle::idle).
    virtual void submitAndWait() = 0;

    /// Main thread, after endFrame(): the device cannot go on. The GPU refused
    /// the process (AccessRevoked, NotPermitted, the system's timeout), or a frame did not finish within
    /// two of the backend's waits (FrameLifecycle::lost). The viewer then stops with the user's message.
    /// GL never loses its device.
    virtual bool deviceLost() const { return false; }
    /// The GPU errors the backend's commit feedback has reported so far, read without waiting;
    /// 0 on GL.
    virtual U64 gpuErrorCount() const { return 0; }
};

// ------------------------------------------------------------------------------------------------
// Shared by the backends. Backend-neutral, so the GL adapter and the Metal backend behave alike.

/// Checks a description against the device's limits before the backend calls the native API.
/// Returns an empty string when the description is valid, otherwise the
/// reason for the WARN line.
/// BUFFER_STORAGE only with caps that have computePasses; without caps it is refused.
std::string validate(const BufferDesc& desc, U64 initialBytes, const Capabilities* caps = nullptr);
std::string validate(const TextureDesc& desc, const Capabilities& caps);
std::string validate(const ProgramDesc& desc, const Capabilities& caps);
std::string validate(const PipelineDesc& desc, const Capabilities& caps);
std::string validate(const RenderPassDesc& desc);
/// A sampler's maximum anisotropy: finite, at least 1, at most the device's, and above 1 only with
/// anisotropic filtering.
std::string validate(const SamplerDesc& desc, const Capabilities& caps);

/// The sampler a draw uses for a binding: the binding's sampling, comparing
/// (with its compareFunc, LessEqual unless set) when the program's sampler type is
/// Sampler2DShadow and not comparing otherwise, and point filtered for an integer format, as the
/// GL adapter samples one. For the Metal backend at draw time; GL keeps sampling on the texture
/// and never calls it.
SamplerDesc samplerForDraw(const SamplerDesc& bound, UniformType samplerType, Format format);

/// Whether a pipeline can draw in a pass: the same color count and formats, depth format and
/// samples. Returns the reason, or an empty string.
std::string checkPipelineForPass(const PipelineDesc& pipeline, const RenderPassDesc& pass);

/// The pass's side of checkPipelineForPass, all it reads of a pass, so passes with equal formats
/// give a pipeline the same verdict and reason: Format::Unknown past colorCount and for an
/// attachment without a texture, and the samples of the first attachment, as the pass's size is,
/// or 0 when the pass has no texture.
struct PassFormats
{
    U32 colorCount = 0;
    std::array<Format, PipelineDesc::MAX_COLOR_ATTACHMENTS> colorFormats{};
    Format depthFormat = Format::Unknown;
    U32 samples = 0;

    bool operator==(const PassFormats&) const = default;
};

PassFormats passFormatsOf(const RenderPassDesc& pass);

/// Writes the formats into a pipeline's description, with 1 sample when they have none, so the
/// pipeline fits every valid pass with those formats.
void applyPassFormats(const PassFormats& formats, PipelineDesc& pipeline);

/// Ranges for updates, uploads, copies and readbacks: inside the buffer, or inside the texture's
/// level and layer. Returns the reason, or an empty string.
std::string validateRange(const BufferDesc& desc, U64 offset, U64 size);
std::string validateRegion(const TextureDesc& desc, U32 level, U32 layer, const Region& region);

/// An upload's region and data, for the encoder and the upload context alike: the region as
/// validateRegion() checks it, block-aligned for compressed formats (up to the level's edge), and
/// enough data for the region in the given layout, with GL's row length. Compressed data is whole
/// 4x4 blocks, and its layout is ignored.
std::string validateUpload(const TextureDesc& desc, U32 level, U32 layer, const Region& region,
                           const PixelLayout& layout, size_t bytes);

/// Mipmaps can be generated for formats that are filtered: not compressed, depth or integer.
std::string validateMipmaps(const TextureDesc& desc);

/// The format rule of Encoder::copyTexture between one-sample textures: empty when a
/// device with caps copies a texture of format from into one of format to, otherwise the reason. Equal formats always;
/// different formats only with caps.copyBetweenFormats, and with caps.copyBetweenFormatsPlainColorOnly only between
/// colour formats that are neither sRGB, block-compressed, depth nor integer. Unknown (a raw GL name) is never plain.
/// Device::copyRefusalBetween asks this one rule for LLGPUCopy::canFromFramebuffer and the Metal encoder, so
/// LLImageGL::scaleDown never re-specifies a texture whose copy the encoder then refuses (which would leave it at its
/// new size with no content). BGRA8Unorm is not plain: validate refuses it as a texture.
std::string copyRefusal(const Capabilities& caps, Format from, Format to);

/// Whether the capabilities have a kind of query: the reasons the tracker gives at a begin, for createQueries and the
/// facades alike.
std::string validateQuery(QueryType type, const Capabilities& caps);

/// The description the cache keys on: with blending off, or without color attachments, the blend
/// state does not matter, and without color attachments neither does the color mask, so they are
/// reset to the defaults and equal pipelines get equal keys.
PipelineDesc normalized(const PipelineDesc& desc);

/// A native render pipeline's key within its library: the two functions and the normalized description (its
/// depth format included). The device's cache puts the library's id before it; LLNativeProgram keys on it alone.
std::string nativePipelineKey(const NativeRenderFunctions& functions, const PipelineDesc& desc);

/// Checks a value for Encoder::setUniform against the program's reflection: the index, the count
/// against the array size, and the byte size against the type. Samplers go through
/// setSamplerChannel instead. Returns the reason, or an empty string.
std::string validateUniform(const ProgramReflection& reflection, U32 uniformIndex, size_t bytes,
                            U32 count);

/// Mip levels of a full chain down to 1x1.
U32 fullMipCount(U32 width, U32 height);

/// The WARN line for a creation that failed.
void warnCreateFailed(std::string_view kind, std::string_view debugName, std::string_view reason);

/// The GL backend's own factory, in llrhigl.cpp; Device::create() calls it.
Ref<Device> createGLDevice(const DeviceCreateInfo& info);

// The viewer's device --------------------------------------------------------------------------
//
// One device per process. LLViewerWindow creates it right after the window, which has its GL
// context by then or makes it when the GL backend asks (LLWindow::createGLContext), and
// releases it at the end of LLViewerWindow::shutdownGL, after every facade object is gone and
// while the context still exists. The accessor holds the only lasting reference, so the device is
// never destroyed in static destruction, when the context may be gone.

/// Any thread. The viewer's device; null before it is created, after it is released, and in
/// headless mode.
Device* viewerDevice();

/// Main thread. Creates the viewer's device through Device::create(). Returns it, or null when
/// the creation failed (Device::create() has logged the WARN line) or info asks for another
/// backend than viewerBackend(). When the device exists already, it is returned with a WARN line.
Device* createViewerDevice(const DeviceCreateInfo& info);

/// Main thread, after every facade object is gone: lets go of the viewer's device, which then
/// destroys its native objects. Nothing when there is none.
void releaseViewerDevice();

/// Main thread. Runs then in releaseViewerDevice before the device's last reference goes, so objects that hold
/// what the device made without a reference to it (LLNativeProgram's libraries and pipelines) let go first. Each
/// function runs at every release from then on.
void onViewerDeviceRelease(std::function<void()> then);

/// Any thread. The backend chosen at startup, which exists before the device does: GL until the
/// startup choice comes.
BackendType viewerBackend();

/// Main thread, before the device exists (refused with a WARN line afterwards). The startup
/// choice and the tests set it.
void setViewerBackend(BackendType type);

/// One main-loop frame of the viewer's device: the constructor calls beginFrame() and the
/// destructor endFrame(), also while an exception unwinds. Nothing without a device, and the
/// destructor ends the frame only on the device the constructor began it on.
class ViewerFrame
{
public:
    ViewerFrame();
    ~ViewerFrame();

    ViewerFrame(const ViewerFrame&) = delete;
    ViewerFrame& operator=(const ViewerFrame&) = delete;

private:
    U64 mDeviceId = 0; ///< the device's id(); ids start at 1 and are never reused
};

// Test support ---------------------------------------------------------------------------------
//
// Golden level 2's test fault: the viewer's device makes a named program fail at its
// publication, as a program the backend cannot compile, so that LLShaderMgr's fall-back can be
// tried without a broken GLSL file. The backends' createProgram asks for it when it makes
// LLGLSLShader's program; without a fault nothing changes. Main thread only.

/// The fault llgoldentest.cpp arms from the config's program_fault; off by default.
struct ProgramFault
{
    /// LLGLSLShader::mName; empty means off.
    std::string program;
    /// Still compiling when createShader asks, and failed from the next time anyone asks or waits.
    bool deferred = false;
    /// How many creations of the named program in a row fail before one builds; 0 means all of
    /// them. After a creation that builds, the count starts again, so each shader load fails its
    /// first `times` creations (a golden run can load the shaders twice).
    U32 times = 0;
};

/// Arms the fault and resets the hits and the count.
void setProgramFault(const ProgramFault& fault);

/// For the backends' createProgram. When the fault names the program (an exact match) and times
/// is 0 or fewer than times creations in a row have been taken, counts a hit, sets deferred and
/// returns true. When the fault names the program and times creations in a row have been taken,
/// starts the count again and returns false. Another name changes nothing.
bool takeProgramFault(const std::string& program, bool& deferred);

/// The creations the fault has failed since it was armed.
U32 programFaultHits();

/// The delay the tests and golden arm: the viewer's device keeps a named program of LLGLSLShader Pending, as
/// a backend that still compiles it, so that the publication's wait can be tried on GL; off by default. The fault wins
/// when both name the same creation, and that creation is not counted here.
struct ProgramDelay
{
    /// LLGLSLShader::mName; empty means off.
    std::string program;
    /// How many creations of the named program in a row are delayed before one is not; 0 means all of them. Counted
    /// as ProgramFault::times.
    U32 creations = 0;
    /// Pending until the device's frameNumber() has reached the creation's frame + frames; 0 means until the first
    /// wait(). wait() ends the delay at once.
    U32 frames = 0;
};

/// Arms the delay and resets the count.
void setProgramDelay(const ProgramDelay& delay);

/// For the backends' createProgram, after takeProgramFault, as takeProgramFault counts: true with the frames when the
/// delay names the program and takes this creation.
bool takeProgramDelay(const std::string& program, U32& frames);

// Frames, caches and the encoder's tracking, shared by the backends ----------------------------

/// Frame numbers, the limit on frames in flight and the deletion list. Each device
/// has one. The backend tells it when the GPU has finished a frame and gives it a way to wait for
/// one: an MTLSharedEvent per frame on Metal, today's behavior or a fence per frame on GL.
///
/// At shutdown the device waits for the GPU, lets go of its caches, its surface and its encoder's
/// bindings (their objects release their native objects here too) and calls releaseAll() before
/// it destroys its own native objects; the destructor frees nothing, because the device is half
/// gone by then.
class FrameLifecycle
{
public:
    static constexpr U32 MAX_FRAMES_IN_FLIGHT = 3;

    /// Blocks until the GPU has finished the frame. Returns false when the frame did not finish
    /// within the backend's time limit. endFrame() waits a second time; two failures make the device
    /// lost, which stops the viewer with the user's message.
    using WaitForFrame = std::function<bool(U64 frame)>;

    explicit FrameLifecycle(WaitForFrame waitForFrame);
    ~FrameLifecycle();

    FrameLifecycle(const FrameLifecycle&) = delete;
    FrameLifecycle& operator=(const FrameLifecycle&) = delete;

    /// The frame being recorded; the first is 1.
    U64 currentFrame() const { return mCurrentFrame.load(); }
    /// The last frame the GPU has finished; 0 before the first.
    U64 completedFrame() const { return mCompletedFrame.load(); }

    /// Main thread, after the backend has sent the current frame: starts the next and frees what
    /// may be freed. When the next frame would have more than MAX_FRAMES_IN_FLIGHT frames in
    /// flight, counting itself, it first waits for the frame that must finish, twice at most. When
    /// neither wait sees it finish, the device is lost: the frame is not marked finished,
    /// no frame starts and nothing is freed, since the GPU may still read what the frames released.
    void endFrame();

    /// A frame did not finish within two waits; from then on endFrame() does nothing.
    bool lost() const { return mLost.load(); }

    /// Any thread: the GPU has finished this frame and every frame before it. A frame that was
    /// just sent may finish before endFrame() has started the next one.
    void frameCompleted(U64 frame);

    /// Main thread, after the backend has waited for all the work it has sent (submitAndWait),
    /// with a signal that covers every queue the frames use: every earlier frame has finished, and
    /// what they released is freed. The frame being recorded has not finished, because more of it
    /// may still be recorded and sent later. With freeReleased, what was released before this call
    /// is freed too, the current frame's included: everything recorded so far has been sent and has
    /// finished, and nothing recorded later can use an object that was released. A render of many
    /// GPU frames inside one viewer frame then frees what each of its frames let go.
    void idle(bool freeReleased = false);

    /// Any thread, typically an object's destructor: hands over the function that frees the
    /// native objects. It runs on the main thread, once the GPU has finished the frame being
    /// recorded now (or an idle(true) has come after the release) and every upload fence given
    /// has signalled.
    void release(std::function<void()> freeNative, std::vector<Ref<Fence>> uploads = {});

    /// Main thread: frees what may be freed. Returns how many releases ran.
    size_t collect();

    /// Main thread, only when the GPU is idle (shutdown): frees everything. Returns how many ran.
    size_t releaseAll();

    size_t pendingReleases() const;

private:
    struct Release
    {
        U64 frame;
        U64 waits; // mWaits when it was released
        std::function<void()> freeNative;
        std::vector<Ref<Fence>> uploads;
    };

    WaitForFrame mWaitForFrame;
    std::atomic<U64> mCurrentFrame{ 1 };
    std::atomic<U64> mCompletedFrame{ 0 };
    /// How many idle(true) calls have begun: a release made with fewer may be freed.
    std::atomic<U64> mWaits{ 0 };
    std::atomic<bool> mLost{ false };
    mutable std::mutex mMutex;
    std::vector<Release> mReleases;
};

/// The device's cache of immutable objects made from a description, such as samplers. Safe on the
/// main and the upload thread. A creation that fails is not cached, so the next call tries again.
/// Creation runs under the cache's lock, which suits quick creations such as samplers, not objects
/// that compile.
template <class Desc, class T, class Hash = std::hash<Desc>>
class DescCache
{
public:
    template <class Create>
    Ref<T> get(const Desc& desc, Create&& create)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto found = mObjects.find(desc);
        if (found != mObjects.end())
        {
            return found->second;
        }
        Ref<T> object = create(desc);
        if (object.notNull())
        {
            mObjects.emplace(desc, object);
        }
        return object;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mObjects.clear();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mObjects.size();
    }

private:
    mutable std::mutex mMutex;
    std::unordered_map<Desc, Ref<T>, Hash> mObjects;
};

/// The device's pipelines, keyed on the program's id and the normalized description.
/// It holds no reference to the program: the facade (LLGLSLShader) owns it, and the backend's
/// program calls forgetProgram() when it dies (also when shaders are reloaded), so its pipelines
/// leave the cache and go to the deletion list once nobody holds them. The caller holds the
/// program while it asks, so the program cannot die during create(). Safe on the main and the
/// upload thread. create() runs outside the lock, because setting a pipeline up can take a while;
/// when two threads create the same pipeline at once, the first one cached is kept and returned to
/// both.
class PipelineCache
{
public:
    using Create = std::function<Ref<Pipeline>(const PipelineKey& key)>;

    /// Null, and not cached, when create() fails.
    Ref<Pipeline> get(const PipelineKey& key, const Create& create);

    /// Returns how many pipelines left the cache. It looks at every entry, which is fine for how
    /// rarely programs die.
    size_t forgetProgram(U64 programId);

    void clear();
    size_t size() const;

private:
    mutable std::mutex mMutex;
    std::unordered_map<PipelineKey, Ref<Pipeline>, PipelineKeyHash> mPipelines;
};

/// What every backend's encoder does alike. The backend's encoder calls it
/// at each entry, before it records anything. It tracks whether a pass is open, what is bound and
/// the pass's dynamic state, and checks each call. It keeps a reference to every bound object until it is rebound or
/// unbound, or until the end of a frame in which nobody else holds it, and to the pass's
/// attachments while the pass is open, so a facade that releases an object does not free it under
/// the backend's recorded state. It records nothing itself and is used on the encoder's thread
/// only.
///
/// Each call returns the reason it is wrong, or an empty string. A call it rejects changes nothing,
/// so the backend can skip it, with one exception: a rejected binding clears that binding, and the
/// backend binds null natively, so no draw uses the object that was there before (and a draw that
/// needs a pipeline or vertex or index buffer is rejected).
///
/// With checks off (release builds) it skips the checks of descriptions, formats,
/// sizes, ranges and attributes, but still refuses what would break the native encoder: a pass in
/// a pass, a draw outside a pass or without a usable pipeline, dynamic state outside a pass, an
/// unbalanced debug group (stack passes delimit none), and a query that overlaps one of its kind.
class EncoderTracker
{
public:
    /// The limits come from the device's capabilities; a limit of 0 is not checked.
    explicit EncoderTracker(const Capabilities& caps = Capabilities(), bool checks = true);

    /// What the backend must close on its native encoders when a pass or the frame ends, warning
    /// or not, in this order: the groups opened in the pass, the pass, then the groups opened
    /// outside it (they belong to another native encoder).
    struct Closing
    {
        std::string warning;  ///< what was left open, or empty
        U32 popInPass = 0;
        bool endPass = false;
        U32 popOutside = 0;
        /// A compute pass ends (endComputePass, or endFrame with one open): first of all.
        bool endCompute = false;
        /// A stack pass that a compute or native pass suspended goes on: the backend continues it in a new segment
        /// that loads its attachments (pass() is that pass again).
        bool resumed = false;
    };

    std::string beginRenderPass(const RenderPassDesc& pass);
    Closing endRenderPass();
    std::string clear(bool color, bool depth);

    std::string setPipeline(Pipeline* pipeline);
    std::string setDepthStencilState(DepthStencilState* state);
    std::string setVertexBuffer(Buffer* buffer, const VertexStreams& streams);
    std::string setIndexBuffer(Buffer* buffer, IndexType type);
    std::string setTexture(U32 channel, Texture* texture);
    std::string setUniformBuffer(U32 blockIndex, Buffer* buffer, U64 offset, U64 size);
    std::string setUniform(Program* program, U32 uniformIndex, size_t bytes, U32 count);
    std::string setSamplerChannel(Program* program, U32 uniformIndex);

    std::string draw(PrimitiveType type);
    std::string drawIndexed(PrimitiveType type, U32 firstIndex, U32 indexCount);

    std::string updateBuffer(Buffer* buffer, U64 offset, U64 size);
    std::string uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region,
                              const PixelLayout& layout, size_t bytes);
    std::string copyTexture(Texture* src, U32 srcLevel, U32 srcLayer, const Region& srcRegion,
                            Texture* dst, U32 dstLevel, U32 dstLayer, U32 dstX, U32 dstY);
    std::string generateMipmaps(Texture* texture);
    std::string readback(Texture* texture, U32 level, U32 layer, const Region& region);
    std::string readback(Buffer* buffer, U64 offset, U64 size);

    std::string beginQuery(Query* query);
    std::string endQuery(Query* query);

    std::string pushDebugGroup();
    std::string popDebugGroup();

    // Compute passes and native bindings (Encoder's). Refused, also with checks off: a compute pass inside a pass
    // other than a stack pass, a render pass inside a compute pass, transfers inside a compute pass, a dispatch outside
    // one or without a usable pipeline, and native bindings outside the pass of their stage.
    /// Begins a compute pass. An open stack pass is suspended (suspendedPass()); the backend stores what it recorded
    /// before it calls this.
    std::string beginComputePass(const ComputePassDesc& pass);
    /// Ends the compute pass and drops its pipeline and native bindings; resumed when a stack pass goes on.
    Closing endComputePass();
    std::string setComputePipeline(ComputePipeline* pipeline);
    std::string setThreadgroupMemory(U32 index, U32 bytes);
    /// A dispatch of threads (dispatchThreads) or of groups (dispatchThreadgroups), threadsPerGroup each. barrier is
    /// set when the dispatch binds what an earlier dispatch of the pass wrote since the last barrier: the backend puts
    /// a barrier between them. Counted in dispatches().
    std::string dispatch(DispatchSize size, DispatchSize threadsPerGroup, bool& barrier);
    std::string setNativeTexture(NativeStage stage, U32 slot, Texture* texture, Access access, TextureRange range);
    std::string setNativeSampler(NativeStage stage, U32 slot);
    std::string setNativeBytes(NativeStage stage, U32 slot, size_t bytes);
    std::string setNativeBuffer(NativeStage stage, U32 slot, Buffer* buffer, U64 offset, Access access);

    bool inComputePass() const { return mInCompute; }
    const ComputePassDesc& computePass() const { return mCompute; }
    ComputePipeline* computePipeline() const { return mComputePipeline.get(); }
    /// The stack pass a compute or native pass suspended; null when none is.
    const RenderPassDesc* suspendedPass() const { return mSuspended ? &mSuspended->pass : nullptr; }
    struct NativeTexture
    {
        Ref<Texture> texture;
        Access access = Access::Read;
        TextureRange range;
    };
    struct NativeBuffer
    {
        Ref<Buffer> buffer;
        U64 offset = 0;
        Access access = Access::Read;
    };
    /// The native bindings of a stage, by slot, as the open compute or native pass has them.
    const std::map<U32, NativeTexture>& nativeTextures(NativeStage stage) const { return mNative[size_t(stage)].textures; }
    const std::map<U32, NativeBuffer>& nativeBuffers(NativeStage stage) const { return mNative[size_t(stage)].buffers; }
    /// Accepted dispatches, and the barriers dispatch() asked for between them, since the tracker was made.
    U64 dispatches() const { return mDispatches; }
    U64 dispatchBarriers() const { return mDispatchBarriers; }

    /// Frame end: ends an open pass, query or debug group and warns about it, except a stack pass,
    /// which ends with the frame, and drops the bindings whose objects nobody but the tracker holds
    /// (also when one object is bound in several places). The backend calls it before
    /// FrameLifecycle::endFrame(), so what is released here belongs to the frame that ends.
    Closing endFrame();

    bool inPass() const { return mInPass; }
    const RenderPassDesc& pass() const { return mPass; }
    /// The passes that began (accepted beginRenderPass calls, stack passes too); 0 before the
    /// first.
    U64 passSerial() const { return mPassSerial; }
    /// 0 outside a pass. In a pass, a number that changes only when the pass's formats differ
    /// from those of the last pass they were asked for, so a run of passes with equal formats
    /// keeps one number, and equal numbers mean equal formats. A pass's formats are computed at
    /// the first ask in it, so a pass nobody asks about costs nothing.
    U64 passFormatsSerial() const;
    /// The open pass's formats (passFormatsOf); PassFormats() outside a pass.
    const PassFormats& passFormats() const;

    /// What is bound, for a backend that sets the bindings again when a pass begins.
    Pipeline* pipeline() const { return mPipeline.get(); }
    DepthStencilState* depthStencilState() const { return mDepthStencil.get(); }
    Buffer* vertexBuffer() const { return mVertexBuffer.get(); }
    const VertexStreams& streams() const { return mStreams; }
    Buffer* indexBuffer() const { return mIndexBuffer.get(); }
    IndexType indexType() const { return mIndexType; }
    const std::unordered_map<U32, Ref<Texture>>& textures() const { return mTextures; }
    const std::unordered_map<U32, Ref<Buffer>>& uniformBuffers() const { return mUniformBuffers; }
    /// Begun and not ended; endFrame() forgets them, so a backend ends them natively first.
    const std::vector<Ref<Query>>& activeQueries() const { return mQueries; }

    /// The four bindings a draw uses, for a backend that tracks draws it does not make itself: GL's
    /// LLVertexBuffer::OwnGLDraws, during which the GL backend keeps the interface's bindings apart. It stays
    /// with that scope, a GL backend switch.
    struct DrawBindings
    {
        Ref<Pipeline> pipeline;
        Ref<DepthStencilState> depthStencil;
        Ref<Buffer> vertexBuffer;
        VertexStreams streams;
        Ref<Buffer> indexBuffer;
        IndexType indexType = IndexType::U16;
    };
    DrawBindings drawBindings() const;
    /// Moves a set from drawBindings() back as it was, with no checks, because each binding was
    /// accepted when it was set; setPipeline would check the pipeline against the open pass again
    /// and could unbind it. The next draw checks the pipeline against the pass.
    void restoreDrawBindings(DrawBindings bindings);

    /// The open pass's dynamic state, as the encoder's setters gave it, in GL's numbers; outside a
    /// pass the defaults a pass begins with. It is what the encoder's
    /// next draw uses, read through Encoder::state(). A backend that ends and begins native encoders
    /// inside a pass sets it again from here. No backend applies it again at draw time, and the GL
    /// backend never reads it to make or skip a GL call.
    struct DynamicState
    {
        std::optional<std::array<S32, 4>> viewport; ///< none: the whole attachment
        bool scissorEnabled = false;
        std::array<S32, 4> scissor{};
        CullMode cull = CullMode::Off;
        FillMode fill = FillMode::Fill;
        F32 biasFactor = 0.f;
        F32 biasUnits = 0.f;
        bool biasFill = false;
        bool biasLines = false;
        bool depthClamp = false;
        F32 pointSize = 1.f;
        F32 lineWidth = 1.f;

        bool operator==(const DynamicState&) const = default;
    };

    /// Each records its values as given, the line width clamped to the capabilities'
    /// lineWidthRange, and checks nothing else. Outside a pass it refuses, also with checks off,
    /// because a native encoder needs a pass. An accepted beginRenderPass, endRenderPass and
    /// endFrame's end of a pass put the defaults back.
    std::string setViewport(S32 x, S32 y, S32 width, S32 height);
    std::string setScissor(bool enabled, S32 x, S32 y, S32 width, S32 height);
    std::string setCullMode(CullMode mode);
    std::string setFillMode(FillMode mode);
    std::string setDepthBias(F32 factor, F32 units, bool enableFill, bool enableLines);
    std::string setDepthClamp(bool enabled);
    std::string setPointSize(F32 size);
    std::string setLineWidth(F32 width);
    const DynamicState& dynamicState() const { return mDynamic; }
    /// The dynamic setters' calls, accepted or not.
    U64 dynamicCalls() const { return mDynamicCalls; }
    /// Readbacks and copies the tracker has accepted since it was made, checks on or off; a facade compares it before
    /// and after its call to see that the call reached the encoder.
    U64 transfers() const { return mTransfers; }
    /// Draws the backend reported made after the tracker accepted them (drawMade), checks on or off: on GL also those
    /// inside LLVertexBuffer::OwnGLDraws, whose GL call LLVertexBuffer makes. A facade compares it before and after its
    /// draw to see that the draw reached the encoder, as transfers() for copies.
    U64 draws() const { return mDraws; }
    /// The backend's report that a draw the tracker accepted reached its native encoder; a draw the backend drops after
    /// the tracker (a binding or a pipeline it cannot make) is not reported.
    void drawMade() { ++mDraws; }
    /// The backend's report that a copy the tracker accepted did not reach its native encoder (Metal: no encoder,
    /// scratch or binding for it): transfers() takes it back, so the facade reports the copy as not delivered.
    void transferDropped()
    {
        if (mTransfers > 0)
        {
            --mTransfers;
        }
    }
    /// The last texture readback or copy the tracker accepted, checks on or off: what a facade sent to the encoder, for
    /// tests. Ids are the textures' id(), 0 for none; a readback has no destination.
    struct Transfer
    {
        enum class Kind
        {
            None,
            Copy,
            TextureReadback
        };
        Kind kind = Kind::None;
        U64 source = 0;
        U32 sourceLevel = 0;
        U32 sourceLayer = 0;
        Region region;
        U64 destination = 0;
        U32 destinationLevel = 0;
        U32 destinationLayer = 0;
        U32 dstX = 0;
        U32 dstY = 0;
    };
    const Transfer& lastTransfer() const { return mLastTransfer; }

    /// Bound objects held, for tests.
    size_t heldReferences() const;
    /// Texture entries an unbinding left for the next binding of any channel, for tests.
    size_t spareTextureEntries() const { return mSpareTextureEntries.size(); }

private:
    typedef std::unordered_map<U32, Ref<Texture>> TextureMap;

    std::string checkDraw(PrimitiveType type) const;
    /// A channel's texture entry out of mTextures, its reference let go at once as erase does; the
    /// entry kept for the next binding, so a channel bound and unbound for every draw allocates
    /// nothing.
    void releaseTextureEntry(U32 channel);
    /// Computes the open pass's formats, once per pass.
    void updatePassFormats() const;
    /// Counts a dynamic setter's call and gives the reason it is refused outside a pass.
    std::string dynamicCall();
    /// The reason a transfer is refused inside a compute pass, or empty.
    std::string outsideCompute() const;
    /// Whether a native binding of the stage may be given now, or the reason not.
    std::string nativeStageOpen(NativeStage stage, U32 slot, U32 slots) const;
    /// The open stack pass is set aside for a compute or native pass.
    void suspendStackPass();
    /// The compute pass's or native pass's end: the native bindings go, and a suspended stack pass goes on.
    bool endNativeScope();

    U32 mMaxChannels = 0;
    bool mTimerQueries = false;
    bool mPrimitivesQueries = false;
    bool mConcurrentSampleQueries = false;
    bool mChecks = true;
    bool mInPass = false;
    RenderPassDesc mPass;
    U64 mPassSerial = 0;
    // Computed when asked for, also by the const reads and checkDraw.
    mutable U64 mFormatsPassSerial = 0; ///< the pass mFormats was computed for
    mutable U64 mFormatsSerial = 0;
    mutable PassFormats mFormats;
    /// The bound pipeline fits passes with this formats serial; 0 when that is not known.
    mutable U64 mFitsFormats = 0;
    U32 mGroupsAtPassStart = 0;
    U32 mGroupsAtComputeStart = 0; ///< debug groups open when the compute pass began
    U32 mGroups = 0;
    Ref<Pipeline> mPipeline;
    Ref<DepthStencilState> mDepthStencil;
    Ref<Buffer> mVertexBuffer;
    VertexStreams mStreams;
    Ref<Buffer> mIndexBuffer;
    IndexType mIndexType = IndexType::U16;
    std::unordered_map<U32, Ref<Texture>> mTextures;
    std::vector<TextureMap::node_type> mSpareTextureEntries; ///< empty entries, no references
    std::unordered_map<U32, Ref<Buffer>> mUniformBuffers;
    std::vector<Ref<Query>> mQueries; ///< begun and not ended
    // The pass's dynamic state
    F32 mLineWidthRange[2] = { 1.f, 1.f };
    DynamicState mDynamic;
    U64 mDynamicCalls = 0;
    U64 mTransfers = 0; ///< accepted readbacks and copies
    U64 mDraws = 0;     ///< draws the backend made
    // Compute and native passes
    bool mInCompute = false;
    ComputePassDesc mCompute;
    Ref<ComputePipeline> mComputePipeline;
    struct NativeStageBindings
    {
        std::map<U32, NativeTexture> textures;
        std::map<U32, NativeBuffer> buffers;
    };
    std::array<NativeStageBindings, 3> mNative; ///< by NativeStage
    /// Ids of what dispatches wrote since the last barrier between dispatches of the pass.
    std::vector<U64> mWrittenSinceBarrier;
    /// Ids of what dispatches only read since that barrier: a later dispatch that writes one waits for them.
    std::vector<U64> mReadSinceBarrier;
    U64 mDispatches = 0;
    U64 mDispatchBarriers = 0;
    /// A stack pass set aside by a compute or native pass, with what goes on with it.
    struct Suspended
    {
        RenderPassDesc pass;
        DynamicState dynamic;
        DrawBindings bindings;
        U32 groupsAtPassStart = 0;
    };
    std::optional<Suspended> mSuspended;
    Transfer mLastTransfer; ///< the last accepted texture readback or copy, for tests
    void acceptTransfer(const Transfer& transfer)
    {
        mLastTransfer = transfer;
        ++mTransfers;
    }
};

// ------------------------------------------------------------------------------------------------
// The table between the facades' GL enums and the interface

/// Format::Unknown for a GL format the table does not have.
Format formatFromGL(U32 internalFormat);
/// GL's sized internal format for a format; 0 for a format GL has none for (BGRA8Unorm, Unknown).
/// Where several GL formats become one format (RGB8, 24-bit depth), the GL backend keeps asking GL
/// for the facade's own GL format, so GL's behavior does not change.
U32 toGLInternalFormat(Format format);
/// Whether GL keeps alpha for a facade's GL internal format: false for the three-component formats
/// whose format has four (GL_RGB, GL_RGB8, GL_SRGB, GL_SRGB8, GL_COMPRESSED_RGB, GL_COMPRESSED_SRGB,
/// GL_RGB16F and GL_RGB32F), true for the others and for 0.
bool hasAlphaInGLFormat(U32 glFormat);
/// No value for a format and type pair the table does not have.
/// GL_ALPHA, GL_LUMINANCE and GL_LUMINANCE_ALPHA are upload layouts only: the caller's alpha and
/// luminance bytes as R or RG, uploaded into the R8 or RG8 storage with a swizzle mask that
/// LLImageGL::describeTexture describes. They are not valid for
/// readbacks or for a round trip to GL's format, where toGLPixelFormat gives GL_RED or GL_RG.
std::optional<PixelLayout> pixelLayoutFromGL(U32 format, U32 type, bool swapBytes = false,
                                             U32 rowLength = 0);
/// The other way, for the GL backend's uploads and readbacks.
U32 toGLPixelFormat(const PixelLayout& layout);
U32 toGLPixelType(const PixelLayout& layout);
/// The stage of a GL shader type (GL_VERTEX_SHADER, GL_FRAGMENT_SHADER or GL_GEOMETRY_SHADER), as
/// LLShaderMgr's units have it; no value for any other type.
std::optional<ShaderStage> shaderStageFromGL(U32 type);
/// The kind of a GL query target (GL_ANY_SAMPLES_PASSED, GL_SAMPLES_PASSED, GL_TIME_ELAPSED or
/// GL_PRIMITIVES_GENERATED), as the facades name it; no value for any other target.
std::optional<QueryType> queryTypeFromGL(U32 target);
/// The type of a GL uniform type, as the GL backend reflects it: bool and unsigned types
/// are Int and IVec*, and GL_SAMPLER_2D_RECT, GL_SAMPLER_2D_MULTISAMPLE, GL_SAMPLER_2D_ARRAY and
/// GL_SAMPLER_3D are Sampler2D; no value for any other type.
std::optional<UniformType> uniformTypeFromGL(U32 type);
/// The other way: each type's canonical GL enum, for the facades that keep GL's types.
U32 toGLUniformType(UniformType type);
/// The function of a GL depth or compare function (GL_NEVER to GL_ALWAYS), as LLGLDepthTest keeps
/// it; no value for any other enum.
std::optional<CompareFunc> compareFuncFromGL(U32 func);
/// The type of GL_UNSIGNED_SHORT or GL_UNSIGNED_INT indices, as LLVertexBuffer keeps it; no value
/// for any other type.
std::optional<IndexType> indexTypeFromGL(U32 type);
/// The cull mode of glCullFace's GL_FRONT or GL_BACK, as LLRender::setCullFace keeps it; no value
/// for any other face (GL_FRONT_AND_BACK). Off is GL_CULL_FACE's enable, which is LLGLState's.
std::optional<CullMode> cullModeFromGL(U32 face);
/// The fill mode of glPolygonMode's GL_FILL or GL_LINE, as LLRender::setPolygonMode keeps it; no
/// value for any other mode (GL_POINT).
std::optional<FillMode> fillModeFromGL(U32 mode);

} // namespace LL::RHI

#endif // LL_LLRHI_H
