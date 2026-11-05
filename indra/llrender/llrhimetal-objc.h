/**
 * @file llrhimetal-objc.h
 * @brief The Metal backend's private Objective-C++ declarations
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

#ifndef LL_LLRHIMETAL_OBJC_H
#define LL_LLRHIMETAL_OBJC_H

// Private to the Metal .mm files (the backend's and its tests'), which include it before any other
// header. stdtypes.h's BOOL (S32) and Objective-C's BOOL (bool) cannot both exist in one file, so
// LL's headers see theirs under another name: a typedef, so nothing changes in their declarations,
// and BOOL is Objective-C's after them. This is the one place that does it.
#define BOOL LL_BOOL
#include "linden_common.h"

#include "llrhi.h"
#include "llrhimetal.h"
#include "llrhimsl.h" // its structs only: the backend calls nothing of llrhimsl.cpp
#undef BOOL

#if LL_RHI_METAL

#import <Metal/Metal.h>
#import <MetalFX/MTL4FXSpatialScaler.h>
#import <MetalFX/MTL4FXTemporalScaler.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/qos.h>

namespace LL::RHI
{

// Object::id() hides Objective-C's id inside the classes that derive from Object (MetalDevice and
// the resources), so the Metal files name Metal's objects through these.
typedef id<MTLDevice> IdMTLDevice API_AVAILABLE(macos(26.0));
typedef id<MTLTexture> IdMTLTexture API_AVAILABLE(macos(26.0));
typedef id<MTLBuffer> IdMTLBuffer API_AVAILABLE(macos(26.0));
typedef id<MTLAllocation> IdMTLAllocation API_AVAILABLE(macos(26.0));
typedef id<MTLResidencySet> IdMTLResidencySet API_AVAILABLE(macos(26.0));
typedef id<MTLSharedEvent> IdMTLSharedEvent API_AVAILABLE(macos(26.0));
typedef id<MTLDepthStencilState> IdMTLDepthStencilState API_AVAILABLE(macos(26.0));
typedef id<MTL4CommandQueue> IdMTL4CommandQueue API_AVAILABLE(macos(26.0));
typedef id<MTL4CommandAllocator> IdMTL4CommandAllocator API_AVAILABLE(macos(26.0));
typedef id<MTL4CommandBuffer> IdMTL4CommandBuffer API_AVAILABLE(macos(26.0));
typedef id<MTL4CounterHeap> IdMTL4CounterHeap API_AVAILABLE(macos(26.0)); // Timer queries
typedef id<MTL4CommandEncoder> IdMTL4CommandEncoder API_AVAILABLE(macos(26.0));
typedef id<MTL4RenderCommandEncoder> IdMTL4RenderCommandEncoder API_AVAILABLE(macos(26.0));
typedef id<MTL4ComputeCommandEncoder> IdMTL4ComputeCommandEncoder API_AVAILABLE(macos(26.0));
typedef id<MTL4CommitFeedback> IdMTL4CommitFeedback API_AVAILABLE(macos(26.0));
typedef id<MTLSamplerState> IdMTLSamplerState API_AVAILABLE(macos(26.0));
typedef id<MTLRenderPipelineState> IdMTLRenderPipelineState API_AVAILABLE(macos(26.0));
typedef id<MTLComputePipelineState> IdMTLComputePipelineState API_AVAILABLE(macos(26.0));
typedef id<MTLLibrary> IdMTLLibrary API_AVAILABLE(macos(26.0));
typedef id<MTL4Compiler> IdMTL4Compiler API_AVAILABLE(macos(26.0));
typedef id<MTL4ArgumentTable> IdMTL4ArgumentTable API_AVAILABLE(macos(26.0));
typedef id<CAMetalDrawable> IdCAMetalDrawable API_AVAILABLE(macos(26.0));
typedef id<MTLDrawable> IdMTLDrawable API_AVAILABLE(macos(26.0)); ///< a presented handler's argument
typedef id<MTLFence> IdMTLFence API_AVAILABLE(macos(26.0));
typedef id<MTL4FXSpatialScaler> IdMTL4FXSpatialScaler API_AVAILABLE(macos(26.0)); // MetalFX, weak-linked
typedef id<MTL4FXTemporalScaler> IdMTL4FXTemporalScaler API_AVAILABLE(macos(26.0)); // MetalFX, weak-linked
// The upload thread's Metal 3 queue
typedef id<MTLCommandQueue> IdMTLCommandQueue API_AVAILABLE(macos(26.0));
typedef id<MTLCommandBuffer> IdMTLCommandBuffer API_AVAILABLE(macos(26.0));
typedef id<MTLBlitCommandEncoder> IdMTLBlitCommandEncoder API_AVAILABLE(macos(26.0));

/// The time limit of every wait for the GPU.
inline constexpr U32 METAL_WAIT_MS = 5000;
/// The visibility result buffer's slots of 8 bytes (256 KB). Metal writes no visibility result at an offset of
/// 256 KB or more and reports nothing (a run-time probe: every slot from 32,769 on read 0 with the same draw), so the ring never goes past it (llrhi.h's Query contract).
inline constexpr U32 METAL_VISIBILITY_SLOTS = 1u << 15;
/// The timestamp heap's pairs, a begin's stamp and an end's; 4,096 entries of 8
/// bytes (32 KiB), made with the device. Metal API Validation refuses a counter heap of more entries ("entryCount must
/// not be more than 4096", a run-time probe), so not 16,384; a shader-profile frame's
/// synchronous reads free their pairs within the frame (takeTimerPair), and the other users begin a few per frame.
inline constexpr U32 METAL_TIMER_PAIRS = 1u << 11;

/// One WARN line for a call the Metal backend does not make yet; the caller gets null or nothing.
void metalNotPorted(const char* call, const char* owner);

/// Whether a commit feedback error loses the device by itself. In Metal 4's
/// MTL4CommandQueueErrorDomain: NotPermitted (2), DeviceRemoved (4) and AccessRevoked (5) (MTL4CommandQueue.h); in
/// MTLCommandBufferErrorDomain, as a fallback: AccessRevoked (4), NotPermitted (7) and DeviceRemoved (11)
/// (MTLCommandBuffer.h). Every other error, such as OutOfMemory or Internal, is counted only. The watchdog's Timeout
/// (metalErrorIsTimeout) is counted and loses the device only on a second one in a row.
API_AVAILABLE(macos(26.0)) bool metalErrorLosesDevice(NSError* error);
/// Whether the error is the system watchdog's Timeout (MTL4CommandQueueErrorDomain 1 or
/// MTLCommandBufferErrorDomain 2), which macOS recovers from once: the command buffer is dropped and later ones run.
API_AVAILABLE(macos(26.0)) bool metalErrorIsTimeout(NSError* error);
/// The error's domain, code and the code's name, for the feedback handler's WARN line.
API_AVAILABLE(macos(26.0)) std::string metalErrorName(NSError* error);

/// The text as an NSString, for Metal's labels.
NSString* metalString(std::string_view text);

/// Format's Metal formats, indexed by Format; Invalid for Unknown. The resource and the pipeline
/// files both use it, through metalPixelFormat.
inline constexpr MTLPixelFormat METAL_PIXEL_FORMATS[] = {
    MTLPixelFormatInvalid,             // Unknown
    MTLPixelFormatR8Unorm,             // R8Unorm
    MTLPixelFormatRG8Unorm,            // RG8Unorm
    MTLPixelFormatRGBA8Unorm,          // RGBA8Unorm
    MTLPixelFormatRGBA8Unorm_sRGB,     // RGBA8Unorm_sRGB
    MTLPixelFormatBGRA8Unorm,          // BGRA8Unorm
    MTLPixelFormatR16Float,            // R16Float
    MTLPixelFormatRG16Float,           // RG16Float
    MTLPixelFormatRGBA16Float,         // RGBA16Float
    MTLPixelFormatRGBA16Unorm,         // RGBA16Unorm
    MTLPixelFormatR32Float,            // R32Float
    MTLPixelFormatRG32Float,           // RG32Float
    MTLPixelFormatRGBA32Float,         // RGBA32Float
    MTLPixelFormatRGB10A2Unorm,        // RGB10A2Unorm
    MTLPixelFormatRG11B10Float,        // RG11B10Float
    MTLPixelFormatR8Uint,              // R8Uint
    MTLPixelFormatR16Uint,             // R16Uint
    MTLPixelFormatR32Uint,             // R32Uint
    MTLPixelFormatBC1_RGBA,            // BC1_RGBA
    MTLPixelFormatBC1_RGBA_sRGB,       // BC1_RGBA_sRGB
    MTLPixelFormatBC3_RGBA,            // BC3_RGBA
    MTLPixelFormatBC3_RGBA_sRGB,       // BC3_RGBA_sRGB
    MTLPixelFormatBC5_RGUnorm,         // BC5_RG
    MTLPixelFormatBC7_RGBAUnorm,       // BC7_RGBA
    MTLPixelFormatBC7_RGBAUnorm_sRGB,  // BC7_RGBA_sRGB
    MTLPixelFormatDepth32Float,        // Depth32Float
    MTLPixelFormatDepth32Float_Stencil8, // Depth32Float_Stencil8
};
static_assert(std::size(METAL_PIXEL_FORMATS) == size_t(Format::Count), "one Metal format for each Format");

/// The Metal format of a Format; Invalid for Unknown and for what is not a format.
constexpr MTLPixelFormat metalPixelFormat(Format format)
{
    return size_t(format) < std::size(METAL_PIXEL_FORMATS) ? METAL_PIXEL_FORMATS[size_t(format)] : MTLPixelFormatInvalid;
}

/// Where one uniform lives in a stage's default block: its byte offset, the strides of its
/// array elements and matrix columns, and its bytes; size 0 when the stage does not use it.
struct MetalUniformPlace
{
    U32 offset = 0;
    U32 arrayStride = 0;
    U32 matrixStride = 0;
    U32 size = 0;
};

/// One stage's part of a program's layout.
struct MetalStageLayout
{
    U32 blockSize = 0;                     ///< the default block's bytes, the size of its CPU copy
    std::vector<MetalUniformPlace> places; ///< by interface uniform index
    std::vector<U32> textureUniforms;      ///< by MSL texture index: the interface uniform index
    std::vector<UniformType> textureTypes; ///< by MSL texture index: the sampler's type, for samplerForDraw
    U32 uniformBlocks = 0; ///< the uniform blocks the stage reads: bit (buffer - 1), the block's slot
    std::vector<U32> blockSizes; ///< by slot: the bytes of the block the stage reads there
};

/// A program's default blocks and textures per stage, from the translation's reflection.
struct MetalProgramLayout
{
    MetalStageLayout vertex;
    MetalStageLayout fragment;
};

/// A program's entry points, as a render pipeline descriptor takes them.
struct API_AVAILABLE(macos(26.0)) MetalProgramFunctions
{
    MTL4FunctionDescriptor* vertex = nil;
    MTL4FunctionDescriptor* fragment = nil;
};

/// The Metal resources and objects. Their bodies are in their own files: the texture, the
/// buffer and the sampler in llrhimetalresource-objc.mm, the program and the pipeline in
/// llrhimetalpipeline-objc.mm and the surface in llrhimetalsurface-objc.mm.
class API_AVAILABLE(macos(26.0)) MetalTexture;
class API_AVAILABLE(macos(26.0)) MetalBuffer;
class API_AVAILABLE(macos(26.0)) MetalSampler;
class API_AVAILABLE(macos(26.0)) MetalProgram;
class API_AVAILABLE(macos(26.0)) MetalPipeline;
class API_AVAILABLE(macos(26.0)) MetalTimerQuery;
class API_AVAILABLE(macos(26.0)) MetalSurface;
class API_AVAILABLE(macos(26.0)) MetalDevice;

/// Whether a bound depth state writes depth. GL writes no depth while its depth test is off,
/// and a null binding is DepthStencilDesc(), with test and writes off.
inline bool metalWritesDepth(const DepthStencilDesc* state)
{
    return state && state->depthTest && state->depthWrite;
}

/// RHI's compare function as Metal's, for depth states and comparing samplers.
MTLCompareFunction metalCompareFunction(CompareFunc func);

/// The Metal description of a depth state: the compare function only with the test on,
/// writes as metalWritesDepth.
MTLDepthStencilDescriptor* metalDepthStencilDescriptor(const DepthStencilDesc& desc) API_AVAILABLE(macos(26.0));

/// The texture's Metal texture; nil for null. Every texture of a Metal device is a MetalTexture.
IdMTLTexture metalTexture(Texture* texture) API_AVAILABLE(macos(26.0));
/// The Metal texture's gpuResourceID, read once when the texture was made; zero for null. No Objective-C call.
MTLResourceID metalTextureID(Texture* texture) API_AVAILABLE(macos(26.0));
/// A view of the texture's levels [first, end) and every slice, made at the first ask and kept, and forgotten in the
/// encoder's shadows, with the texture; the texture itself for all its levels. Native bindings; main thread.
IdMTLTexture metalLevelView(Texture* texture, U32 first, U32 end) API_AVAILABLE(macos(26.0));
/// The slices of a description's level: 1 for 2D, 6 for a cube, 6 per cube of a cube array, the depth of a 3D
/// texture. The written marks keep one per level and slice.
U32 metalSlices(const TextureDesc& desc);

/// The buffer's Metal buffer; nil for null. Every buffer of a Metal device is a MetalBuffer.
IdMTLBuffer metalBuffer(Buffer* buffer) API_AVAILABLE(macos(26.0));

/// The sampler's Metal state; nil for null. Every sampler of a Metal device is a MetalSampler.
IdMTLSamplerState metalSampler(Sampler* sampler) API_AVAILABLE(macos(26.0));
/// The sampler state's gpuResourceID, read once when the sampler was made; zero for null. No Objective-C call.
MTLResourceID metalSamplerID(Sampler* sampler) API_AVAILABLE(macos(26.0));

/// The most samples an anisotropic Metal sampler takes: MTLSampler.h:130-135 has maxAnisotropy as a
/// whole number, Apple's documentation of it the range 1-16.
constexpr F32 METAL_MAX_ANISOTROPY = 16.f;

/// The Metal description of a sampler that createMetalSampler makes, with LLTexUnit::toGLSampling's
/// filters; desc.mipmapped is the flag in effect at the draw. anisotropic: the device has
/// anisotropic filtering, so maxAnisotropy applies to every filter, as gl_texture_filter sets it.
MTLSamplerDescriptor* metalSamplerDescriptor(const SamplerDesc& desc, bool anisotropic) API_AVAILABLE(macos(26.0));

/// The program's layout, its functions and the pipeline's state, with MetalProgram and
/// MetalPipeline in llrhimetalpipeline-objc.mm.
const MetalProgramLayout* metalProgramLayout(Program* program) API_AVAILABLE(macos(26.0));
MetalProgramFunctions metalProgramFunctions(Program* program) API_AVAILABLE(macos(26.0));
IdMTLRenderPipelineState metalPipelineState(Pipeline* pipeline) API_AVAILABLE(macos(26.0));
/// The pipeline's state for a pass whose color attachments in alphaOff (bit i for attachment i) have
/// no alpha in GL: a variant whose write mask has no alpha there, made at the first ask and waited for.
IdMTLRenderPipelineState metalPipelineState(Pipeline* pipeline, MetalDevice& device, U32 alphaOff)
    API_AVAILABLE(macos(26.0));
/// Tests: the pipeline's variants without alpha made so far; 0 for null.
size_t metalPipelineVariants(Pipeline* pipeline) API_AVAILABLE(macos(26.0));
/// A native pipeline's state (Pipeline::native), waited for while it compiles; nil for null, for a translated program's
/// pipeline and when it failed. In llrhimetalnative-objc.mm.
IdMTLRenderPipelineState metalNativePipelineState(Pipeline* pipeline) API_AVAILABLE(macos(26.0));
/// A compute pipeline's state, waited for while it compiles; nil for null and when it failed.
IdMTLComputePipelineState metalComputePipelineState(ComputePipeline* pipeline) API_AVAILABLE(macos(26.0));
/// The blend factors and operations of a color attachment, and a write mask, as the translated programs' pipelines
/// have them; for the native pipelines.
void metalSetBlend(MTL4RenderPipelineColorAttachmentDescriptor* attachment, const BlendState& blend)
    API_AVAILABLE(macos(26.0));
MTLColorWriteMask metalWriteMask(U8 mask);
/// Whether work that writes the texture's level and layer (a cube's face, a cube array's cube *
/// 6 + face) has been encoded: an upload, a copy into it, a generated mip level, a clear or a render segment with
/// it as an attachment. False for null and outside the texture. metalMarkWritten sets it.
bool metalWritten(Texture* texture, U32 level, U32 layer) API_AVAILABLE(macos(26.0));
void metalMarkWritten(Texture* texture, U32 level, U32 layer) API_AVAILABLE(macos(26.0));
/// The mark cleared, as in a new texture, so the next write into part of the level and
/// layer is a first write again (metalZeroAroundFirstWrite). Nothing for null and outside the texture.
void metalForgetWritten(Texture* texture, U32 level, U32 layer) API_AVAILABLE(macos(26.0));
/// The layout of the pipeline's program as the pipeline was made with it, so a draw finds the
/// program's blocks and textures through the bound pipeline alone; null for null.
const MetalProgramLayout* metalPipelineLayout(Pipeline* pipeline) API_AVAILABLE(macos(26.0));
/// The attributes the pipeline's vertex descriptor reads (metalVertexDescriptor's mask); 0 for null.
U32 metalPipelineAttributes(Pipeline* pipeline) API_AVAILABLE(macos(26.0));

/// How a point pipeline's vertex default block is made from the program's. The
/// point variant adds the uniform `point_size`, and a reflection is by offset, so the places of the
/// program's uniforms may move in the variant's block.
struct MetalPointBlock
{
    struct Copy
    {
        U32 from = 0; ///< the offset in the program's vertex block
        U32 to = 0;   ///< the offset in the variant's
        U32 size = 0;
    };
    U32 size = 0;             ///< the variant's vertex block
    U32 pointSize = 0;        ///< point_size's offset in it
    bool same = false;        ///< every place the program's vertex stage reads is the variant's: one memcpy
    std::vector<Copy> copies; ///< otherwise one per uniform the program's vertex stage reads, by name
};

/// The program's vertex places against the variant's by uniform name; empty when it worked,
/// otherwise why the variant does not fit (no `point_size`, or a uniform the variant lacks).
std::string metalPointBlockOf(const ProgramReflection& program, const MetalStageLayout& programVertex,
                              const ProgramReflection& variant, const MetalStageLayout& variantVertex,
                              MetalPointBlock& block);

/// The variant's vertex block for a point draw: the program's block moved by `block`, with
/// point_size the tracker's point size; the bytes no place covers are zeros.
void metalBuildPointBlock(const MetalPointBlock& block, std::span<const std::byte> program, F32 pointSize,
                          std::vector<std::byte>& out);

/// A point pipeline's block; null for null and for another class.
const MetalPointBlock* metalPipelinePointBlock(Pipeline* pipeline) API_AVAILABLE(macos(26.0));

/// The vertices 0 to count - 1 in the order a fan, strip or loop of count
/// vertices draws them as a list. A triangle has GL's provoking vertex, its last, first, as
/// a run-time probe measured the rotation (2, 0, 1): fan triangle i is (i + 2, 0, i + 1),
/// strip triangle i (i + 2, i, i + 1) for an even i and (i + 2, i + 1, i) for an odd one, which keeps
/// GL's alternating winding. A loop is the line strip 0 to count - 1, then 0; lines are not rotated.
/// Empty for the other types and for too few vertices to draw anything.
std::vector<U32> metalListOrder(PrimitiveType type, U32 count);
/// The length of metalListOrder's list, without making it: 0 when it is empty.
U32 metalListCount(PrimitiveType type, U32 count);

/// The Metal primitive of metalListOrder's list: triangles, and a line strip for a loop.
MTLPrimitiveType metalListPrimitive(PrimitiveType type);

/// A cached list of metalListOrder in a resident Shared buffer of its own, 16-bit up to 65535
/// vertices, else 32-bit (always a list, so Metal's primitive restart at 65535 never applies).
struct API_AVAILABLE(macos(26.0)) MetalIndexList
{
    IdMTLBuffer buffer = nil; ///< nil when the draw draws nothing
    U32 count = 0;
    IndexType type = IndexType::U16;
};

/// The MSL compiler's math. Both are set explicitly: mathMode's default depends on the language
/// version (MTLLibrary.h:329-332), and fastMathEnabled is deprecated (323-326). Chosen by a run-time
/// probe against GL in one process: Relaxed with metal::fast's
/// functions gave the fewest floats that are not bit-equal to GL's, and the same as GL for pow, exp2,
/// log2, sin, cos, inversesqrt, division, mix, clamp and fract. Relaxed keeps infs and NaNs (257-267),
/// so deferredUtil.glsl's isinf/isnan guard stays, which Fast folds away; Safe and Precise functions
/// differ from GL more.
inline constexpr MTLMathMode METAL_MATH_MODE = MTLMathModeRelaxed;
inline constexpr MTLMathFloatingPointFunctions METAL_MATH_FUNCTIONS = MTLMathFloatingPointFunctionsFast;

/// [Viewer Lab] Development knobs for the fast-math line, from the environment, read once:
/// LL_METAL_SAFE_MATH=all, or a comma-separated list of program names, compiles those programs with Safe math, and
/// with LL_METAL_PRECISE_MATH=1 also precise functions; a listed program is compiled alone, as a group's library has
/// one mode. LL_METAL_INVARIANT=1 makes every vertex stage's position invariant (MSL::Options) and compiles with
/// preserveInvariance. All off by default; MetalDevice::compiler logs them when one is set.
struct MetalMathKnobs
{
    bool safeAll = false;
    std::vector<std::string> safeNames;
    bool precise = false;
    bool invariant = false;

    bool safe(const std::string& program) const;
    bool any() const { return safeAll || !safeNames.empty() || invariant; }
    std::string describe() const;
};
const MetalMathKnobs& metalMathKnobs();

/// [Viewer Lab] A development knob for a control run, from the environment, read once: LL_METAL_COMPILE_SLOW=1
/// turns two speedups (the translation threads at QOS_CLASS_USER_INITIATED instead of QOS_CLASS_UTILITY, and a wait that
/// waits takes the rest of the pool's queue first) off, so a device compiles without them. Off by default; each new device
/// takes it (MetalDevice::setCompileSlow), and MetalDevice::compiler logs it when it is set.
bool metalCompileSlowKnob();

/// [Viewer Lab] The programs compiled with Safe math instead of METAL_MATH_MODE (the fast-math line along the panel's
/// edge in ultra/overview is fixed where the math mode lives). Each
/// is compiled alone, as a group's library has one mode; a name's skinned variant ("Skinned <name>",
/// make_rigged_variant) is compiled the same way. A bisect over the 52 programs ultra/overview draws (all but the
/// clouds' program, which Safe math loses, like the invariant position) found the one: with only it Safe, the line
/// (row 473, 105 columns brighter than the panel) is gone and the clouds stay. MetalDevice::compiler logs the list,
/// golden level 2 fails a Metal world run whose list names a program that ultra's GL render profile lacks (a renamed
/// program would bring the line back), and the shader check compiles each listed program with Safe math too. "HUD PBR
/// Alpha Shader" compiles the same pbralphaV/F files with IS_HUD (llviewershadermgr.cpp), so it goes with it; the list
/// is fitted to ultra/overview, and the GLTF alpha variants, which share the PBR lighting, are not in it.
inline constexpr std::array<std::string_view, 2> METAL_SAFE_MATH_PROGRAMS = { "Deferred PBR Alpha Shader",
                                                                             "HUD PBR Alpha Shader" };

/// Whether the program compiles with Safe math: a name of METAL_SAFE_MATH_PROGRAMS or its skinned variant, or a program
/// the development knob LL_METAL_SAFE_MATH names.
bool metalSafeMath(const std::string& program);

/// The options of every MSL compilation: MSL 4.0 and the two math constants above; `safe` (metalSafeMath) asks
/// for Safe math instead, with precise functions when the knob LL_METAL_PRECISE_MATH says so.
MTLCompileOptions* metalCompileOptions(bool safe = false) API_AVAILABLE(macos(26.0));

/// The key of a clear program for a pass's formats: the color count in bits 0-3, a depth
/// attachment in bit 4, and each color attachment i's output type in bits 8 + 2i: 0 float, 1 unsigned
/// integer, 2 signed integer (RHI's integer formats are all unsigned, llrhi.h:128-130, so 2 is not
/// reached today).
U32 metalClearKey(const PassFormats& formats);

/// The clear program for a key, in GLSL: the vertex stage makes one triangle over the whole
/// attachment from gl_VertexID, a front face as GL's; the fragment stage writes clear_color to each
/// color attachment, as uvec4(clear_color) for an integer one (glClearBufferuiv's GLuint(rgba[i]),
/// llrhigl.cpp), and clear_depth to gl_FragDepth with a depth attachment. Attachments of one type are
/// one output array, as the translator takes only one fragment output.
ProgramDesc metalClearProgramDesc(U32 key);

/// The mip program, in GLSL, one for every format (no rounding to the nearest half; a 16-bit float target converts
/// toward zero): GL's filter as the level 1
/// probe measured it (glGenerateMipmap on Apple's GL), a bilinear tap of the level above at each texel's centre: a 2x2 box on an even axis, and on an
/// odd one the two texels around the centre with its weights. GL makes its even levels from the unrounded
/// values of the level above, so 8-bit levels from 2 on may be one step from GL's (golden level 1's
/// probe_mips allows that one step; its scenes and its mip and texel edge probe are exact). The vertex stage is the clear program's
/// triangle; the fragment stage samples mip_source, a view of the level above, at gl_FragCoord.xy over the
/// level's size, with the device's bilinear sampler and no mips.
ProgramDesc metalMipProgramDesc();

/// The present program, in GLSL: the mip program's triangle, and a fragment stage that takes the back
/// buffer's texel present_source(x, H - 1 - y) by texelFetch at gl_FragCoord (the translator's GLSL has Metal's
/// upper-left origin, the back buffer's memory row 0 is GL's bottom row), black (alpha 1) beyond it.
ProgramDesc metalPresentProgramDesc();
/// The format copy program: the mip program's triangle, and a fragment stage that writes the texel of
/// copy_source at gl_FragCoord unfiltered (MetalEncoder::formatCopy).
ProgramDesc metalFormatCopyProgramDesc();

/// The program as the translator takes it: the stages' units in order, the reserved attributes and
/// the uniform blocks by slot (ProgramDesc::uniformBlocks).
MSL::Program metalProgramSource(const ProgramDesc& desc);

/// One library's source for a group of programs. Each stage's MSL is wrapped in a
/// namespace of its own, `v<i>` for member i's vertex stage and `f<i>` for its fragment stage, so
/// SPIRV-Cross's helpers and main0 exist once per namespace; the `#include` and `#pragma` lines and
/// `using namespace metal;` are lifted out, once each in the order first seen. `stages` holds per
/// member its vertex and fragment MSL. The functions are then `v<i>::main0` and `f<i>::main0`.
std::string metalGroupSource(std::span<const std::array<std::string, 2>> stages);

/// Why a stage does not fit Metal's argument tables, or empty: a texture or
/// sampler index from 16, as texture i has sampler i and the samplers stop at 16, or a block in a
/// buffer past 7. The streams' 16 + type stay below 31 by themselves.
std::string metalStageLimits(const MSL::StageResult& stage);

/// The interface's reflection and the backend's layout from a translation. The uniforms are the
/// vertex stage's default block by offset, then the fragment stage's new ones by offset, then the
/// fragment samplers by MSL index, then the samplers only the vertex stage has; only the uniforms a
/// stage reads, at the largest active array size of the stages, as GL reports them. The
/// blocks once per name, vertex then fragment; `blocks` are the program's by slot. Empty when it
/// worked; otherwise why the program fails.
std::string metalReflection(const MSL::Result& result, std::span<const char* const> blocks, ProgramReflection& reflection,
                            MetalProgramLayout& layout);

/// The vertex streams' buffer slots begin here; buffer 0 is the default block.
inline constexpr U32 METAL_FIRST_VERTEX_BUFFER = 16;

/// The vertex input of an attribute mask: attribute i in buffer 16 + i at offset 0,
/// one stream per attribute, with LLVertexBuffer's format and stride.
MTLVertexDescriptor* metalVertexDescriptor(U32 mask) API_AVAILABLE(macos(26.0));
/// The stride of an attribute type's stream, as the vertex descriptor has it; 0 past the types.
U32 metalVertexStride(U32 type);

/// Tests: a texture readback's Shared buffer; nil for null. Every readback of a Metal device's
/// encoder is a MetalReadback.
IdMTLBuffer metalBuffer(Readback* readback) API_AVAILABLE(macos(26.0));

/// Creates the Metal device, or returns null with a WARN line. Called by metalBackend with full
/// support only.
Ref<Device> createMetalDevice(const DeviceCreateInfo& info, const MetalSupport& support) API_AVAILABLE(macos(26.0));

/// The CAMetalLayer of DeviceCreateInfo::window: the layer itself (one without a view, the tests), or the
/// backing layer of the view (the viewer's LLOpenGLView), which the view's window displays first when AppKit has not
/// made it yet. Nil when there is none. Main thread. In llrhimetalsurface-objc.mm.
CAMetalLayer* metalLayerOf(void* window) API_AVAILABLE(macos(26.0));

class API_AVAILABLE(macos(26.0)) MetalDevice;

// The factories MetalDevice delegates to, each in its object's file. Each gives null after one WARN
// line.

/// Where bytes are for the GPU: a Shared buffer, the offset in it and the CPU's address of
/// the bytes there. A buffer's memory is its own buffer or a pool block; a frame's bytes are in the
/// frame slot's ring.
struct API_AVAILABLE(macos(26.0)) MetalAllocation
{
    IdMTLBuffer buffer = nil;
    U64 offset = 0;
    std::byte* cpu = nullptr;
};

/// Every usage is Shared, so the CPU's bytes are the GPU's (MTLDevice.h:683-689,
/// MTLResource.h:118), with the default cache mode, not write-combined; the buffers are bound by
/// their gpuAddress (MTLBuffer.h:121-124).
MTLResourceOptions metalBufferStorage(const BufferDesc& desc) API_AVAILABLE(macos(26.0));

/// The largest pool block; a dynamic buffer up to it lives in the pool, a larger
/// one gets a buffer of its own at each update.
inline constexpr U64 METAL_POOL_MAX_BLOCK = 64 * 1024;
inline bool metalPooled(const BufferDesc& desc)
{
    return desc.dynamic && desc.size <= METAL_POOL_MAX_BLOCK;
}

/// The buffer's current memory; empty for null. Every buffer of a Metal device is a MetalBuffer.
MetalAllocation metalAllocation(Buffer* buffer) API_AVAILABLE(macos(26.0));
/// The GPU address of the buffer's current memory (its MTLBuffer's gpuAddress plus the allocation's offset), kept with
/// the buffer from its creation and from each update that gives it new memory; 0 for null. No Objective-C call.
MTLGPUAddress metalAddress(Buffer* buffer) API_AVAILABLE(macos(26.0));

/// The indices drawIndexed's check and its fans, strips and loops read: a dynamic
/// buffer's current memory, which an update replaces at the call; a static index buffer's CPU copy,
/// written at creation and at each update, since its Shared memory is stale until the update's GPU
/// copy has run. The copy is kept for every static index buffer (always), since any
/// indexed fan, strip or loop reads it, with the debug setting or without.
std::span<const std::byte> metalIndexBytes(Buffer* buffer) API_AVAILABLE(macos(26.0));

/// Whether an index buffer was ever given a 16-bit 0xFFFF (at an even offset), at creation or at an
/// update, where its indices are written; no scan at a draw. False for null.
bool metalIndexHoldsRestart(Buffer* buffer) API_AVAILABLE(macos(26.0));
/// How many times the buffer has been written since it was made (metalUpdateBuffer counts each update); 0 for null.
U64 metalBufferWrites(Buffer* buffer) API_AVAILABLE(macos(26.0));

/// GL's semantics. A dynamic buffer gets new memory with the old bytes outside the
/// range and the new ones in it, also inside a pass, and a draw binds the memory current at the
/// draw. A static buffer gets the bytes in the frame's ring now and a GPU copy before its next use;
/// the caller has broken a pass first.
void metalUpdateBuffer(MetalDevice& device, Buffer* buffer, U64 offset, std::span<const std::byte> data)
    API_AVAILABLE(macos(26.0));

/// A Shared buffer with its data from creation, resident from creation.
Ref<Buffer> createMetalBuffer(MetalDevice& device, const BufferDesc& desc, std::span<const std::byte> initial,
                              std::string_view debugName) API_AVAILABLE(macos(26.0));
Ref<Texture> createMetalTexture(MetalDevice& device, const TextureDesc& desc, std::string_view debugName)
    API_AVAILABLE(macos(26.0));
/// Every sampler, comparing ones too; MetalDevice::getSampler caches what it makes.
Ref<Sampler> createMetalSampler(MetalDevice& device, const SamplerDesc& desc) API_AVAILABLE(macos(26.0));
/// Whether the device has MetalFX's spatial scaler for Metal 4 (Capabilities::spatialUpscaler). MetalFX is weak-linked:
/// false when it is not there.
bool metalSpatialUpscaler(IdMTLDevice device) API_AVAILABLE(macos(26.0));
/// MetalFX's spatial scaler for the description, made with the device's compiler, and the fence it waits on. Null
/// after a WARN line: validate's refusal, no scaler from MetalFX, or a texture usage it asks for that the backend's
/// textures cannot have.
Ref<Upscaler> createMetalUpscaler(MetalDevice& device, const UpscalerDesc& desc) API_AVAILABLE(macos(26.0));
/// The upscaler's MetalFX scaler and its fence; nil for null. Every upscaler of a Metal device is a MetalUpscaler.
IdMTL4FXSpatialScaler metalScaler(Upscaler* upscaler) API_AVAILABLE(macos(26.0));
IdMTLFence metalScalerFence(Upscaler* upscaler) API_AVAILABLE(macos(26.0));
/// Whether the device has MetalFX's temporal scaler for Metal 4 (Capabilities::temporalUpscaler), and Apple's range of
/// input scales for it (supportedInputContentMin/MaxScaleForDevice), logged once. False and 0 when MetalFX is not there.
bool metalTemporalUpscaler(IdMTLDevice device, F32& minScale, F32& maxScale) API_AVAILABLE(macos(26.0));
/// MetalFX's temporal scaler for the description, made with the device's compiler, the fence it waits on and the 1x1
/// exposure of 1 it is given. Null after a WARN line: validate's refusal, no scaler from MetalFX, or a texture usage it
/// asks for that the backend's textures cannot have (the reactive mask's only for a scaler made with one).
Ref<TemporalUpscaler> createMetalTemporalUpscaler(MetalDevice& device, const TemporalUpscalerDesc& desc)
    API_AVAILABLE(macos(26.0));
/// The temporal upscaler's MetalFX scaler, fence and exposure texture; nil for null.
IdMTL4FXTemporalScaler metalTemporalScaler(TemporalUpscaler* upscaler) API_AVAILABLE(macos(26.0));
IdMTLFence metalTemporalScalerFence(TemporalUpscaler* upscaler) API_AVAILABLE(macos(26.0));
IdMTLTexture metalTemporalScalerExposure(TemporalUpscaler* upscaler) API_AVAILABLE(macos(26.0));
/// llrhi.h's default texture of a type (2D, cube, cube array or a 1x1x1 3D), 1x1 RGBA8Unorm (0, 0, 0, 255) in every slice, Shared and
/// filled at creation, resident from creation. Null after a WARN line.
Ref<Texture> createMetalDefaultTexture(MetalDevice& device, TextureType type) API_AVAILABLE(macos(26.0));
/// The default texture of an unbound sampler2DShadow, a 1x1 Depth32Float (MSL's depth2d needs a depth
/// format), Private and never written: bindForDraw samples it with compare function Never, which never passes (Apple's
/// MTLCompareFunction.never; MTLSampler.h:207-210 says only that it is the default) and so gives 0 for every reference
/// whatever the texel holds, as GL gives 0 for a shadow sampler on an incomplete texture; test 86 shows it at run
/// time. Made without any pass or copy, so it can be made inside a pass. Resident from creation; null
/// after a WARN.
Ref<Texture> createMetalDefaultShadowTexture(MetalDevice& device) API_AVAILABLE(macos(26.0));
Ref<Program> createMetalProgram(MetalDevice& device, const ProgramDesc& desc) API_AVAILABLE(macos(26.0));
Ref<Pipeline> createMetalPipeline(MetalDevice& device, Program& program, const PipelineDesc& desc)
    API_AVAILABLE(macos(26.0));

/// The bytes go to a Shared staging buffer and are copied to the texture on a compute encoder of
/// their own, outside a pass (the encoder breaks one first). What is not ported gives one WARN
/// line and changes nothing.
void metalUploadTexture(MetalDevice& device, Texture* texture, U32 level, U32 layer, const Region& region,
                        const PixelLayout& layout, std::span<const std::byte> data) API_AVAILABLE(macos(26.0));
/// GL's storage for a texture made without data reads 0 where nothing has written it (the GL backend's
/// glTexImage2D with no pixels; Apple's GL zeroes it), and Metal's new texture has no defined contents (MTLDevice.h:
/// 703-707 promises only the allocation; API Validation shows magenta). So the first write of a level and slice that
/// nothing has written (the texture's written mark), an upload or a copy into part of it, writes zero texels into the rest: alpha 1
/// where GL keeps no alpha. The copies go on copies, the write's own encoder, outside the region, so no order
/// between them is needed (MTL4CommandEncoder.h:55-58). Colour formats without blocks; depth, BC and multisampled
/// textures are left as they are. Nothing when the level and slice is written or the region covers it. Call before the
/// write is marked.
void metalZeroAroundFirstWrite(MetalDevice& device, IdMTL4ComputeCommandEncoder copies, Texture* texture, U32 level,
                               U32 layer, const Region& region) API_AVAILABLE(macos(26.0));

/// What metalZeroAroundFirstWrite writes, apart from where its bytes and copies go, so the main thread's
/// encoder and the upload thread's queue write the same zeros byte for byte: the parts of the level and slice around
/// the region (the rows before and after it, whole, and its rows left and right of it; none overlaps it) and one
/// buffer of zero texels as large as the largest part, alpha 1 where GL keeps no alpha.
struct MetalZeroFill
{
    std::array<Region, 4> parts{};
    U64 largest = 0;       ///< the largest part's pixels
    U32 pixelBytes = 0;
    U32 componentSize = 0;
    bool alphaOne = false; ///< the fourth component is 1
    U32 one = 0;           ///< alpha 1 in the texel's type
    U64 bytes() const { return largest * pixelBytes; }
};
/// The zeros of the first write of a level and slice; none when metalZeroAroundFirstWrite writes nothing.
std::optional<MetalZeroFill> metalZeroFill(Texture* texture, U32 level, U32 layer, const Region& region)
    API_AVAILABLE(macos(26.0));
/// Writes fill.bytes() of zero texels to cpu.
void metalFillZeros(const MetalZeroFill& fill, std::byte* cpu);

/// A texture's storage mode and usage: always Private, so no memoryless storage
/// (MTLResource.h:75-87); SAMPLED is ShaderRead, RENDER_TARGET is RenderTarget and READBACK needs
/// nothing.
struct API_AVAILABLE(macos(26.0)) MetalTextureStorage
{
    MTLStorageMode mode = MTLStorageModePrivate;
    MTLTextureUsage usage = MTLTextureUsageUnknown;
};
MetalTextureStorage metalTextureStorage(const TextureDesc& desc) API_AVAILABLE(macos(26.0));

/// The layout whose pixels are the format's texels, the one layout a readback of the format
/// takes; none for the formats without one (RGB10A2Unorm, RG11B10Float, BGRA8Unorm and the
/// compressed formats).
std::optional<PixelLayout> metalReadbackLayout(Format format);

/// The one conversion table: the region's pixels of src in the layout, as the texture's texels
/// in tight rows (whole block rows for a compressed format) in dst. Gives dst's bytes per row, or 0
/// after one WARN line naming 'no unit yet' for a layout and format pair without a conversion.
U32 metalConvertUpload(const TextureDesc& desc, const PixelLayout& layout, std::span<const std::byte> src,
                       const Region& region, std::vector<std::byte>& dst);
/// What metalConvertUpload would write: its bytes per row and bytes in all, both 0 for every refusal it makes
/// (a pair without a conversion, an empty region, a source too small for the region). Each refusal has its WARN line,
/// except a compressed format's (a zero width, a source too small for the blocks), which has none.
struct MetalUploadSize
{
    U32 rowBytes = 0;
    U64 bytes = 0;
};
MetalUploadSize metalUploadSize(const TextureDesc& desc, const PixelLayout& layout, size_t srcSize, const Region& region);
/// metalConvertUpload's texels written straight into dst, which holds dstBytes, in one pass: rows copied for
/// U8 RGBA into RGBA texels, one 32-bit word per pixel for U8 BGRA into them and for alpha forced to 1, the general
/// loop for every other pair, byte for byte the same texels. Gives the bytes per row, or 0 for a refusal (its WARN line)
/// and when dstBytes is less than metalUploadSize's bytes (no line), and then writes nothing.
U32 metalConvertUploadInto(const TextureDesc& desc, const PixelLayout& layout, std::span<const std::byte> src,
                           const Region& region, std::byte* dst, U64 dstBytes);
/// Tests: the general loop alone, without the fast paths, into a zeroed dst; as metalConvertUpload otherwise.
U32 metalConvertUploadGeneral(const TextureDesc& desc, const PixelLayout& layout, std::span<const std::byte> src,
                              const Region& region, std::vector<std::byte>& dst);

/// The speed knobs, read from the environment when a device is made (metalSpeedKnobs), so the
/// each change can be measured against the old way. LL_METAL_BARRIER "full": the full queue barrier before
/// every encoder (the old way); "split": render passes let their vertex stages overlap the work before them
/// (MetalDevice::passBarrier). LL_METAL_DISCARD "0": every segment stores its attachments (the old way); "1": a
/// segment a full clear splits off stores nothing the clear covers (MetalEncoder::clear). Not set or empty: the
/// default, the initializer below. Any other value is not guessed at: the knob keeps its default and `unknown` names
/// it, for the device's WARN line (level2.py refuses such a value before a run).
struct MetalSpeedKnobs
{
    bool splitBarriers = false; ///< the default of LL_METAL_BARRIER: true split, false full
    bool discardStores = false; ///< the default of LL_METAL_DISCARD: true 1, false 0
    std::string unknown; ///< "LL_METAL_BARRIER=<value>" and the like, comma-separated; empty when both were understood
};

/// The knobs from the two variables' values (null: not set).
MetalSpeedKnobs metalParseSpeedKnobs(const char* barrier, const char* discard);

/// metalParseSpeedKnobs of the process's LL_METAL_BARRIER and LL_METAL_DISCARD.
MetalSpeedKnobs metalSpeedKnobs();

/// The stages a render pass's split barrier holds back until the queue's earlier work is done: all but
/// the geometry stages, as a complement, so a stage Metal adds later, rasterization, the depth test and the
/// attachments' loads and stores still wait (MTLStageAll is NSIntegerMax; MTLCommandEncoder.h:44-70).
API_AVAILABLE(macos(26.0)) inline MTLStages metalGeometryStages()
{
    return MTLStageVertex | MTLStageObject | MTLStageMesh;
}

API_AVAILABLE(macos(26.0)) inline MTLStages metalPassWaitingStages()
{
    return MTLStageAll & ~metalGeometryStages();
}

/// The GPU's busy time from the commit feedbacks' GPU intervals (MTL4CommitFeedback.h's GPUStartTime and
/// GPUEndTime, seconds). Time two command buffers overlap counts once. The sum is the exact union while the intervals
/// arrive in the order they began; a late one (it began before one added earlier) counts only past the latest end, so
/// its time before that end is lost unless an earlier interval covered it ({3,4} then {1,2} gives 1). `late` counts
/// them, so a window or total with late reports may be short. An interval without times or with
/// its end not after its start is left out.
struct MetalGpuBusy
{
    double busy = 0.0;      ///< seconds
    double lastEnd = 0.0;   ///< the latest end added
    double lastStart = 0.0; ///< the latest start added
    U64 commandBuffers = 0;
    U64 late = 0; ///< intervals added after one that began later

    void add(double start, double end)
    {
        if (start <= 0.0 || !(end > start))
        {
            return;
        }
        if (start < lastStart)
        {
            ++late;
        }
        busy += std::max(0.0, end - std::max(start, lastEnd));
        lastEnd = std::max(lastEnd, end);
        lastStart = std::max(lastStart, start);
        ++commandBuffers;
    }
};

/// One submission's flags, set by its feedback handler under FeedbackState's mutex.
struct MetalSubmission
{
    bool received = false;
    bool failed = false;
    /// The commit feedback's GPUEndTime (host seconds, CACurrentMediaTime's clock); 0 until it has come or with an
    /// error. Atomic: the present trace's presented handler reads it without the mutex (LL_METAL_PRESENT_TRACE).
    std::atomic<double> gpuEnd{ 0.0 };
};

/// What a readback learns when its copy's command buffer is sent: the slot event and
/// the serial it is signalled with, and the submission whose feedback reports it.
struct API_AVAILABLE(macos(26.0)) MetalReadbackStamp
{
    IdMTLSharedEvent event = nil;
    U64 serial = 0; ///< 0 until the command buffer with the copy is sent
    std::shared_ptr<MetalSubmission> submission;
};

/// One begin of a sample query on the device's visibility result buffer (llrhi.h's Query contract): the ring
/// slot it was given (-1: none, so it reads as visible), the stamp of the submit after its end (as a readback's),
/// and the slot's final value, kept here when the ring reuses the slot before anyone read it.
struct API_AVAILABLE(macos(26.0)) MetalQueryRun
{
    S64 slot = -1;
    IdMTLBuffer buffer = nil; ///< the visibility result buffer the slot is in
    std::shared_ptr<MetalReadbackStamp> stamp = std::make_shared<MetalReadbackStamp>();
    U64 kept = 0;           ///< the slot's final value, once the ring reused the slot (captured)
    bool captured = false;
    bool ended = false;     ///< endQuery recorded it
    bool abandoned = false; ///< the frame ended, or the encoder was reset, while it was active: no result
    bool uncounted = false; ///< one of its draws went to a pass without the buffer: no result
    U32 draws = 0;          ///< its draws that reached a pass with the buffer; none: no result (a refused draw counts none)
};

/// One begin of a time query on the device's timestamp heap (llrhi.h's Query contract):
/// the ring pair it was given (-1: none), whether each stamp was encoded, the stamps of the submits that carry them,
/// and the pair's ticks, kept here when the ring reuses the pair before anyone read it. The CPU's own record decides
/// whether the pair holds this run's stamps, never an invalidation of the heap.
struct API_AVAILABLE(macos(26.0)) MetalTimerRun
{
    S64 pair = -1;
    std::shared_ptr<MetalReadbackStamp> beginStamp = std::make_shared<MetalReadbackStamp>();
    std::shared_ptr<MetalReadbackStamp> endStamp = std::make_shared<MetalReadbackStamp>();
    std::array<U64, 2> kept = { 0, 0 }; ///< the pair's ticks, once the ring reused the pair (captured)
    bool captured = false;
    bool beginWritten = false; ///< the begin's stamp was encoded
    bool endWritten = false;   ///< the end's stamp was encoded
    bool ended = false;        ///< endQuery recorded it, or its begin was refused
    bool abandoned = false;    ///< the frame ended, or the encoder was reset, while it was active
};

/// The pass timer's pairs per frame slot: a heap of 4,096 entries (32 KiB), Metal's most, per slot, so a frame has
/// 2,048 encoders' stamps; an encoder past them is counted as left out.
inline constexpr U32 METAL_PASS_TIME_PAIRS = 1u << 11;

/// One encoder's pair in the pass timer: the frame it was begun in (0: not stamped) and the pair in that frame's heap.
struct MetalPassStamp
{
    U64 frame = 0;
    U32 pair = 0;
    bool wantsProgram = false; ///< the pass is named by its first program, which noteProgram has not had yet
};

/// The pass timer (Device::startPassTimes). For a number of whole frames, every native render and compute encoder,
/// and the device's encoders of copies, get a GPU timestamp at their begin and their end. A render encoder's are
/// written inside it with MTL4TimestampGranularityRelaxed (MTL4Counters.h:34-50): the begin's before any draw, so it
/// is taken before the vertex stage begins, the end's after the last draw's fragment stage
/// (MTL4RenderCommandEncoder.h:630-648). A compute encoder's are written on the command buffer right before the
/// encoder begins and right after it ends, each after the work before it is complete (MTL4CommandBuffer.h:162-174):
/// stamps inside a compute encoder do not enclose its work (a run-time probe: a 256 MB copy read 0 ms
/// between them at either granularity, and a dispatch's begin stamp came 4 to 11 ms after the stamp before the
/// encoder, while a render encoder's agreed with the command buffer's around it within 2 %). Each frame slot has a
/// Timestamp heap of its own, made at start() and let go through the lifecycle at the end. A frame's heap is resolved
/// on the CPU (resolveCounterRange:, MTL4Counters.h:81-92) at a later frame's end, once FrameLifecycle says the GPU has
/// finished it, so the main thread never waits for the GPU; then its entries are invalidated for the slot's next
/// frame. Per pass, by its name (with the attachments and the first program for the target stack's passes and for
/// passes without a name): the encoders, the GPU time from begin to end, and the time no earlier encoder of the frame
/// had covered. The table, sorted by time per frame, goes to the log (tag LuminousPassTimes) and to the file. Main
/// thread.
class API_AVAILABLE(macos(26.0)) MetalPassTimes
{
public:
    enum class Kind : U8
    {
        Render,  ///< a pass's render encoder, or one of the backend's own (mips, present, resolve, ...)
        Compute, ///< a compute pass's encoder
        Copies,  ///< the device's compute encoder of copies (uploads, readbacks)
    };

    /// Encoders are stamped now: the frames being timed are being recorded. The encoders ask this first, so the
    /// timer costs one test per encoder when it is off.
    bool recording() const { return mOpen != nullptr; }
    /// From start() until the table is written or stop().
    bool running() const { return mPhase != Phase::Off; }
    /// Arms the timer for frames whole frames, from the next frame on; the table goes to path. done runs on the main
    /// thread once the table is written. False after a WARN line when one is running or Metal gave no heap.
    bool start(IdMTLDevice device, U64 ticksPerSecond, U32 frames, std::string path, std::function<void()> done);
    /// Stops a running timer without a table; done does not run. The heaps go through lifecycle, as frames in flight
    /// may still write into them.
    void stop(FrameLifecycle& lifecycle);

    /// The stamp at a render encoder's begin, right after its barrier: a pair of the frame's heap, or none (frame 0)
    /// when not recording or the heap's pairs are used up (counted as left out). name is the pass's; render passes of
    /// the target stack and passes without a name also get their attachments, and their first program by noteProgram.
    MetalPassStamp beginRender(IdMTL4RenderCommandEncoder encoder, const RenderPassDesc& pass, U64 frame);
    /// The backend's own render encoders, by their label.
    MetalPassStamp beginOwnRender(IdMTL4RenderCommandEncoder encoder, NSString* label, U64 frame);
    /// The stamp before a compute encoder, on the command buffer it will be begun on, with no encoder open.
    MetalPassStamp beginCompute(IdMTL4CommandBuffer buffer, std::string_view name, Kind kind, U64 frame);
    /// The stamp at a render encoder's end, right before endEncoding; after a compute encoder's endEncoding, on its
    /// command buffer. Nothing for a stamp without a pair or of another frame than the one being recorded.
    void endRender(IdMTL4RenderCommandEncoder encoder, const MetalPassStamp& stamp);
    void endCompute(IdMTL4CommandBuffer buffer, const MetalPassStamp& stamp);
    /// The first pipeline state set on an encoder whose pass is named by its program (a stack or unnamed pass);
    /// once per encoder.
    void noteProgram(MetalPassStamp& stamp, NSString* label);

    /// What frameEnd did.
    enum class FrameEnd : U8
    {
        Nothing,
        Began,    ///< the timed frames begin with the frame now being recorded (firstFrame to lastFrame)
        Finished, ///< every timed frame has been resolved, and the frames' command buffers were counted (submitted)
    };
    /// At the device's frame end, once the next frame has begun: resolves the timed frames up to completed, begins
    /// recording an armed timer with current, stops recording after the last timed frame, and opens current's record.
    FrameEnd frameEnd(U64 current, U64 completed);
    U64 firstFrame() const { return mFirst; }
    U64 lastFrame() const { return mLast; }
    /// Whether frame is one of the timed frames (the commit feedback counts their GPU busy time).
    bool timed(U64 frame) const { return mPhase != Phase::Off && mFirst != 0 && frame >= mFirst && frame <= mLast; }
    /// A command buffer of a timed frame was committed.
    void countSubmit() { ++mSubmitted; }
    U64 submitted() const { return mSubmitted; }
    /// The frame ends since every timed frame was resolved.
    U32 framesSinceResolved() const { return mSinceResolved; }
    /// Writes the table with the timed frames' GPU busy time from the commit feedback (received: their command
    /// buffers reported), lets the heaps go through lifecycle, and returns done; the timer is off after it.
    std::function<void()> finish(const MetalGpuBusy& busy, U64 received, FrameLifecycle& lifecycle);

private:
    enum class Phase : U8
    {
        Off,
        Armed,     ///< begins with the next frame
        Recording, ///< the timed frames are being recorded
        Resolving, ///< every timed frame is recorded; waiting for the GPU and the commit feedback
    };
    struct Pair
    {
        U32 name = 0;
        U32 program = 0; ///< 0: none noted
        bool ended = false;
    };
    /// One frame slot's heap and the frame recorded in it.
    struct Record
    {
        IdMTL4CounterHeap heap = nil;
        U64 frame = 0; ///< 0: none, or resolved
        std::vector<Pair> pairs;
        U32 leftOut = 0;
    };
    /// A row of the table: one name (and program) over the timed frames, in ticks.
    struct Row
    {
        U64 encoders = 0;
        U64 ticks = 0;
        U64 ownTicks = 0;
    };

    MetalPassStamp begin(U32 name, U64 frame);
    U32 nameId(Kind kind, std::string_view name);
    void resolve(Record& record);
    void release(FrameLifecycle& lifecycle);
    std::string table(const MetalGpuBusy& busy, U64 received) const;

    Phase mPhase = Phase::Off;
    U64 mTicksPerSecond = 0;
    U32 mFrames = 0;
    U64 mFirst = 0;
    U64 mLast = 0;
    std::string mPath;
    std::function<void()> mDone;
    std::array<Record, FrameLifecycle::MAX_FRAMES_IN_FLIGHT> mRecords; ///< by frame slot
    Record* mOpen = nullptr; ///< the record of the frame being recorded while recording
    /// Names by id: the kind and the text; programs by id, 0 the empty one
    std::vector<std::pair<Kind, std::string>> mNames;
    std::unordered_map<std::string, U32> mNameIds;
    std::vector<std::string> mPrograms{ std::string() };
    std::unordered_map<std::string, U32> mProgramIds;
    std::map<std::pair<U32, U32>, Row> mRows; ///< by name and program
    U64 mResolvedFrames = 0;
    U64 mSubmitted = 0;
    U32 mSinceResolved = 0;
    U64 mEncoders = 0;
    std::array<U64, 3> mKindTicks{}; ///< by Kind
    U64 mOwnTicks = 0;
    U64 mSpanTicks = 0; ///< per frame, the first begin to the last end, summed
    U64 mLeftOut = 0;
    U64 mUnended = 0;    ///< pairs whose end was not written (Metal gave no encoder, or it was dropped at teardown)
    U64 mUnreadable = 0; ///< pairs with a 0 tick or an end before the begin
    U64 mLostFrames = 0; ///< timed frames the GPU had not finished when their slot came round again
};

/// A readback's copy from the texture to its Shared buffer: tight rows of the format's
/// texels in GL's order, never flipped.
struct API_AVAILABLE(macos(26.0)) MetalCopy
{
    IdMTLTexture texture = nil;
    U32 level = 0;
    U32 layer = 0;
    Region region;
    IdMTLBuffer buffer = nil;
    U32 bytesPerRow = 0;                      ///< the region's width times the texel's bytes
    MTLBlitOption options = MTLBlitOptionNone; ///< DepthFromDepthStencil for Depth32Float_Stencil8
    std::shared_ptr<MetalReadbackStamp> stamp;
};

/// A program's uniforms on the encoder, by program id: each stage's CPU copy of its default
/// block and each sampler uniform's texture channel, with GL's defaults (zeros, channel 0). A
/// stage's block goes to the frame's ring only when a setter changed its bytes or its copy there is
/// from another frame; the ring's memory lives for its frame slot.
struct MetalProgramState
{
    std::array<std::vector<std::byte>, 2> blocks; ///< the vertex stage's, then the fragment stage's
    std::vector<U32> channels;                    ///< by interface uniform index; channel 0 past its end
    std::array<bool, 2> dirty = { true, true };   ///< the block changed since its copy in the ring
    std::array<MTLGPUAddress, 2> address{};       ///< the block's copy in the ring
    U64 frame = 0;                                ///< the frame of the copies
    std::vector<std::byte> pointBlock; ///< the point variant's vertex block, made at each point draw
};

/// The argument tables' sizes (MTL4ArgumentTable.h:24-37): buffer 0 is the default block and the
/// streams are 16 + type, so 31 buffers; translate() gives each texture its sampler's
/// index, and the sampler slots stop at 16, so 16 textures are enough.
inline constexpr U32 METAL_TABLE_BUFFERS = 31;
inline constexpr U32 METAL_TABLE_TEXTURES = 16;
inline constexpr U32 METAL_TABLE_SAMPLERS = 16;

/// What bindForDraw last wrote in one stage's argument table, so it writes only the
/// places whose value changes; Metal snapshots the table at each draw (MTL4RenderCommandEncoder.h:613-622).
struct API_AVAILABLE(macos(26.0)) MetalTableShadow
{
    std::array<MTLGPUAddress, METAL_TABLE_BUFFERS> addresses{};
    std::array<MTLResourceID, METAL_TABLE_TEXTURES> textures{};
    std::array<MTLResourceID, METAL_TABLE_SAMPLERS> samplers{};
    std::array<bool, METAL_TABLE_BUFFERS> addressWritten{};
    std::array<bool, METAL_TABLE_TEXTURES> textureWritten{};
    std::array<bool, METAL_TABLE_SAMPLERS> samplerWritten{};
};

/// The main thread's encoder. A render pass is recorded as segments: beginRenderPass keeps the
/// pass's attachments and opens a segment without a native encoder; the first draw opens the
/// segment's native encoder (open()), and endRenderPass, a submit in the pass or a readback that a
/// draw must not reach closes it (close()), with the store actions it then knows.
class API_AVAILABLE(macos(26.0)) MetalEncoder final : public Encoder
{
public:
    /// The tracker's draw checks follow the Metal debug setting (metalDebug()) as it is when the encoder is made.
    MetalEncoder(MetalDevice& device, const Capabilities& caps);

    bool beginRenderPass(const RenderPassDesc& pass) override;
    void endRenderPass() override;
    void clear(bool color, const std::array<F32, 4>& rgba, bool depth, F32 depthValue) override;

    void setPipeline(Pipeline* pipeline) override;
    void setDepthStencilState(DepthStencilState* state) override;
    void setViewport(S32 x, S32 y, S32 width, S32 height) override;
    void setScissor(bool enabled, S32 x, S32 y, S32 width, S32 height) override;
    void setCullMode(CullMode mode) override;
    void setFillMode(FillMode mode) override;
    void setDepthBias(F32 factor, F32 units, bool enableFill, bool enableLines) override;
    void setDepthClamp(bool enabled) override;
    void setPointSize(F32 size) override;
    void setLineWidth(F32 width) override;

    void setUniform(Program* program, U32 uniformIndex, std::span<const std::byte> value, U32 count) override;
    void setSamplerChannel(Program* program, U32 uniformIndex, U32 channel) override;
    void setVertexBuffer(Buffer* buffer, const VertexStreams& streams) override;
    void setIndexBuffer(Buffer* buffer, IndexType type) override;
    void setTexture(U32 channel, Texture* texture, const SamplerDesc& sampling) override;
    void setUniformBuffer(U32 blockIndex, Buffer* buffer, U64 offset, U64 size) override;

    void draw(PrimitiveType type, U32 firstVertex, U32 vertexCount) override;
    void drawIndexed(PrimitiveType type, U32 firstIndex, U32 indexCount) override;

    void updateBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data) override;
    void uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region, const PixelLayout& layout,
                       std::span<const std::byte> data) override;
    void forgetWritten(Texture* texture, U32 level, U32 layer) override;
    void copyTexture(Texture* src, U32 srcLevel, U32 srcLayer, const Region& srcRegion, Texture* dst, U32 dstLevel,
                     U32 dstLayer, U32 dstX, U32 dstY) override;
    void generateMipmaps(Texture* texture) override;
    /// MetalFX's spatial scaler on the frame's command buffer, after a pass break as for a copy and an encoder of its
    /// own that orders it after the queue's earlier work (its comment says how).
    bool upscale(Upscaler* upscaler, Texture* input, U32 width, U32 height, Texture* output) override;
    /// MetalFX's temporal scaler on the frame's command buffer, ordered as upscale's spatial one.
    bool upscaleTemporal(TemporalUpscaler* upscaler, const TemporalUpscaleInputs& inputs) override;
    void waitForFence(Fence* fence) override;

    Ref<Readback> readback(Texture* texture, U32 level, U32 layer, const Region& region,
                           const PixelLayout& layout) override;
    Ref<Readback> readback(Buffer* buffer, U64 offset, U64 size) override;
    bool beginQuery(Query* query) override;
    void endQuery(Query* query) override;

    void pushDebugGroup(std::string_view name) override;
    void popDebugGroup() override;

    // Compute passes on a native compute encoder of their own, after the queue barrier, ending with a producer
    // barrier, so every later stage of the queue sees what they wrote; a stack pass is suspended around one
    // (splitPass first, then a new segment that loads). Native bindings through the binding site.
    bool beginComputePass(const ComputePassDesc& pass) override;
    void endComputePass() override;
    void setComputePipeline(ComputePipeline* pipeline) override;
    void setThreadgroupMemory(U32 index, U32 bytes) override;
    void dispatchThreads(DispatchSize grid, DispatchSize threadsPerGroup) override;
    void dispatchThreadgroups(DispatchSize groups, DispatchSize threadsPerGroup) override;
    void setNativeTexture(NativeStage stage, U32 slot, Texture* texture, Access access, TextureRange range) override;
    void setNativeSampler(NativeStage stage, U32 slot, const SamplerDesc& sampling) override;
    void setNativeBytes(NativeStage stage, U32 slot, std::span<const std::byte> data) override;
    void setNativeBuffer(NativeStage stage, U32 slot, Buffer* buffer, U64 offset, Access access) override;

    const EncoderTracker& state() const override { return mTracker; }

    /// Tests: the compute pass's native encoder while it is open; nil otherwise.
    IdMTL4ComputeCommandEncoder computeEncoder() const { return mCompute; }
    /// Tests: dispatches encoded, barriers put between dependent dispatches, and the compute encoders made.
    U64 nativeDispatches() const { return mNativeDispatches; }
    U64 dispatchBarriers() const { return mDispatchBarriers; }
    U64 computeEncoders() const { return mComputeEncoders; }

    /// The device's frame end: what is still open is closed, and the frame's passes let go.
    void endFrame();
    /// Before a submit, an upload or a static buffer's update inside a pass: what the open segment
    /// has recorded goes first, its readbacks' copies are encoded after it, and the pass goes on in a
    /// new segment that loads its attachments. True when it closed an open segment (a pass break).
    bool splitPass();
    /// At the device's end: the tracker and the segment let go of everything, with no WARN line.
    void reset();
    /// ~MetalDevice first: ends a native encoder that is open, with DontCare stores for every
    /// attachment (endEncoding needs them, MTL4RenderCommandEncoder.h:229-244) and its debug groups back on the
    /// command buffer. Unlike close(true), it begins no encoder for a segment that only clears.
    void closeForTeardown();
    /// The surface's present: the flip pass. What the open segment recorded goes first (splitPass: the
    /// pass's segment ends with color and depth stored, its readbacks' copies are encoded, and the pass goes on in a
    /// new segment that loads, so a facade's stack pass stays open as its record says), then a native encoder of its
    /// own on the drawable texture (DontCare, Store) draws the device's present program over it: drawable row r is
    /// back row H - 1 - r, by texelFetch, and black beyond the back buffer. The tracker's bindings are not touched.
    /// False after a WARN line, and nothing is drawn.
    bool presentDraw(IdMTLTexture drawable, Texture* back);

    /// Tests: the render pass descriptor of the last segment that was closed, with its final store
    /// actions; nil before one.
    MTL4RenderPassDescriptor* lastPassDescriptor() const { return mLastPass; }
    /// Tests: a stage's CPU copy of the program's default block; empty before a uniform of the
    /// program was set or the program drew.
    std::span<const std::byte> uniformBlock(Program* program, ShaderStage stage) const;
    /// Tests: the open segment's native encoder; nil while none is open.
    IdMTL4RenderCommandEncoder openEncoder() const { return mNative; }
    /// Tests: the label of the last native render encoder open() made, read back from it; empty before one.
    std::string lastEncoderLabel() const { return mLastEncoderLabel ? std::string(mLastEncoderLabel.UTF8String) : ""; }
    /// Tests: the stage blocks bindForDraw has copied to the frame's ring.
    U64 ringBlockCopies() const { return mRingBlockCopies; }
    /// Tests: the argument table places bindForDraw has written.
    U64 tableWrites() const { return mTableWrites; }
    /// Main thread, from the release of a texture or a mip level's view, while the native object still lives: the
    /// shadows forget the places that hold its MTLResourceID. Metal gives a released texture's ID to a later one (a
    /// probe saw the last mip view's ID on the next new texture), and a place written for the released texture is not
    /// a binding of the new one: under the capture layer, a skipped write sampled the released view.
    void forgetTexture(MTLResourceID texture);
    /// Tests: whether a stage's shadow holds the texture as written.
    bool shadowHolds(MTLResourceID texture) const;
    /// Tests: the texture places the two shadows hold as written.
    U32 shadowTextures() const;
    /// Tests: the draws encoded on a native encoder.
    U64 nativeDraws() const { return mNativeDraws; }
    /// The attachments a segment split off by a full clear left unstored (DontCare), which the clear
    /// overwrites before anything reads them; with LL_METAL_DISCARD only.
    U64 discardedStores() const { return mDiscardedStores; }
    /// Tests: the clears encoded as a draw of the device's clear program (clearDraw).
    U64 clearDraws() const { return mClearDraws; }
    /// Tests: the mip levels generateMipmaps has drawn with the device's mip program.
    U64 mipDraws() const { return mMipDraws; }
    /// Tests: the native render encoders open() has made.
    U64 renderEncoders() const { return mRenderEncoders; }
    /// Tests: the first loads of a never written attachment without alpha in GL made clears.
    U64 firstLoadClears() const { return mFirstLoadClears; }
    /// Tests: the alpha 1 draws after a copy into a render target without alpha in GL.
    U64 alphaCopyDraws() const { return mAlphaCopyDraws; }
    /// Tests: the flip passes presentDraw has drawn.
    U64 presentDraws() const { return mPresentDraws; }
    /// Tests: the copies between formats formatCopy has drawn.
    U64 formatCopyDraws() const { return mFormatCopyDraws; }
    /// Tests: the upscales encoded on the command buffer.
    U64 upscales() const { return mUpscales; }
    /// Tests and the golden hook: the pass breaks splitPass and uploadTexture have counted.
    const MetalEncoderStats& stats() const { return mStats; }
    /// Tests: the scissor rectangles sent to a segment's native encoder (setNativeScissor).
    U64 scissorSends() const { return mScissorSends; }
    /// Tests: the indexed draws whose indices bindForDraw checked (the Metal debug setting).
    U64 indexChecks() const { return mIndexChecks; }
    /// Tests: the encoder holds a state for the program id.
    bool hasProgramState(U64 programId) const { return mPrograms.count(programId) != 0; }
    /// Tests: the sampler description the last draw that sampled the channel chose (samplerForDraw and the
    /// mipmapped flag in effect); none before such a draw.
    std::optional<SamplerDesc> lastSamplerDesc(U32 channel) const
    {
        if (channel < RESOLVE_CHANNELS)
        {
            return mLastChannelSamplers[channel];
        }
        const auto found = mLastSamplers.find(channel);
        return found != mLastSamplers.end() ? std::optional<SamplerDesc>(found->second) : std::nullopt;
    }
    /// Tests: the remembered resolves (a sampler place's texture and sampler, a kept ID or GPU address, a uniform block's
    /// binding, a kept index list) that differed from a full one; the Metal debug setting compares them at every draw,
    /// and each difference wrote a WARN line.
    U64 resolveMismatches() const { return mResolveMismatches; }
    /// Tests: the places bindForDraw took from its remembered resolve instead of resolving them again.
    U64 resolveHits() const { return mResolveHits; }
    /// Tests: the indexed draws of a fan, strip or loop, or widened to 32 bits, that took their list from
    /// indexConversion's kept ones, and the lists it has made.
    U64 indexConversionHits() const { return mIndexConversionHits; }
    U64 indexConversionsMade() const { return mIndexConversionsMade; }

    /// ~MetalProgram: the program's state goes.
    void forgetProgram(U64 programId)
    {
        mPrograms.erase(programId);
        if (programId == mLastProgramId)
        {
            mLastProgramId = 0;
            mLastProgram = nullptr;
        }
    }
    /// A new texture from MetalDevice::createTexture, kept for zeroNewTargets when it is an uncompressed color cube or
    /// cube array, which is filled face by face and level by level after it is made and sampled before all of it is
    /// written (the reflection probes after a change of their count): every level and layer then reads 0 (alpha 1 where
    /// its GL format keeps none) before the frame's next encoder, as GL's new storage does, while Metal's new memory
    /// holds whatever it held before. Nothing from another thread than the recording thread.
    void zeroLater(Texture* texture);
    /// zeroLater has textures that zeroNewTargets has not cleared yet.
    bool waitingTargets() const { return !mNewTargets.empty(); }
    /// The clears of the cubes zeroLater was given: a render target's level in a render encoder of its own, all its
    /// layers at once, and another cube's unwritten layers by copies of zeros; each cleared layer is then written.
    /// Called before each of the frame's encoders begins, with no render encoder open (open(), openOwnEncoder,
    /// MetalDevice::openCopies), and by endFrame, so the list holds no texture past its frame. A texture nothing else
    /// holds any more, and a render target's level a write has already reached, is left as it is.
    void zeroNewTargets();
    /// Tests: the levels zeroNewTargets has cleared.
    U64 newTargetClears() const { return mNewTargetClears; }
    /// Tests: the depth clears of segments that only cleared (close()) that went into a later segment's load action,
    /// and those encoded in a pass of their own (settleDepthClears).
    U64 depthClearsCarried() const { return mDepthClearsCarried; }
    U64 depthClearsEncoded() const { return mDepthClearsEncoded; }

private:
    /// One recording thread: first in every Encoder call. Under the Metal debug
    /// setting a call from another thread than the one that made the device stops in LL_ERRS before the tracker or
    /// anything else changes; without the setting it checks nothing.
    void onRecordingThread(const char* call) const;
    bool accept(const char* call, const std::string& why);
    void closePass(const EncoderTracker::Closing& closing, const char* call);
    /// Makes the segment's native encoder when none is open: the segment's load actions, store
    /// actions left for close(), the front-facing winding and the argument tables. False after a
    /// WARN line. The dynamic state waits for the first draw or clear draw (applyDrawState, applyClearState):
    /// the mNative* copies start at Metal's defaults.
    bool open();
    /// Ends the segment's native encoder, first making one for a segment that only clears, except one whose clear
    /// carryDepthClear takes. last: the pass ends with it, so its store actions apply; otherwise color and depth are
    /// stored. True when a native encoder was ended (splitPass counts it as a pass break).
    bool close(bool last);
    /// A segment that records nothing and clears only its depth, which is stored, has loaded colours, and is a
    /// texture that nothing samples or writes as storage (the window's depth): its clear waits in mDepthClears for
    /// the next segment with that depth, and the segment needs no encoder. True when it was taken.
    bool carryDepthClear(bool last);
    /// open(): a waiting clear of the segment's depth becomes the segment's load action, Clear for Load, and is
    /// dropped for a segment that clears its depth or does not care what it holds.
    void takeDepthClear(RenderPassDesc& pass);
    /// Before a call that reads or writes a texture otherwise than as a pass's depth (a binding, a copy, a
    /// readback, an upload, mipmaps): its waiting clear, if any, is encoded first in a pass of its own, after the
    /// open segment's encoder has ended (splitPass). A clear that is still the load action of an open segment with no
    /// encoder yet is split off first, so it is among them. Null: every waiting clear, at the frame's end.
    void settleDepthClears(const Texture* texture);
    /// The open native encoder's store action for every attachment of the segment, which endEncoding needs
    /// (open() leaves them Unknown, MTL4RenderCommandEncoder.h:229-244): the pass's own when last, else Store;
    /// DontCare when the device goes (closeForTeardown).
    void setStoreActions(bool last, bool teardown);
    /// Ends the open native encoder: its debug groups leave it before endEncoding and go on in the command
    /// buffer.
    void endNative();
    /// A render encoder of its own for one draw outside the tracker's passes (the copies, the mips, the present):
    /// the frame's command buffer first, then the open debug groups leave it and go on in the encoder, which is
    /// labelled, counted and after the queue barrier. nil when Metal gave none; the groups are then on the command
    /// buffer again, and the caller writes the WARN line.
    IdMTL4RenderCommandEncoder openOwnEncoder(MTL4RenderPassDescriptor* pass, NSString* label);
    /// Ends openOwnEncoder's encoder: its debug groups go back on the command buffer.
    void closeOwnEncoder(IdMTL4RenderCommandEncoder encoder);
    bool scissorLimits() const;
    /// A clear the scissor or the color mask limits, as glClear, drawn on the open segment
    /// with the device's clear program over the whole attachment; the tracker's scissor limits it.
    /// colorMask: the channels it writes in every color attachment (0: none); depth: it writes
    /// depthValue. The tracker's pipeline and bindings stay, and the next draw gives the encoder the
    /// tracker's dynamic state again (applyDrawState). Nothing is drawn after a WARN line.
    void clearDraw(U8 colorMask, const std::array<F32, 4>& rgba, bool depth, F32 depthValue);
    /// Ends the open segment when readbacks wait on it: it is closed as a segment that is not the
    /// last, so color and depth are stored, then the copies are encoded.
    void breakForCopies();
    /// The program's state, made with GL's defaults at the first ask.
    MetalProgramState& programState(U64 programId, const MetalProgramLayout& layout);
    /// The one binding site, the one place that writes the argument tables: the stages'
    /// default blocks, the uniform blocks they read, the vertex streams, and each stage's
    /// textures and samplers. False after one WARN line, which begins with call. indexed: first and
    /// count are indices of the tracker's index buffer, whose residency and values are checked
    /// instead of the vertex range.
    bool bindForDraw(const char* call, U32 first, U32 count, bool indexed);
    /// bindForDraw's branch for a native pipeline: the vertex and fragment stages' native bindings (textures as views
    /// of their levels, samplers, the ring's bytes, buffers) on their slots; no vertex streams, no channels, no uniform
    /// blocks. False after one WARN line.
    bool bindNativeForDraw(const char* call);
    /// A dispatch's native bindings on the compute table, through the same binding site. False after one WARN line.
    bool bindForDispatch(const char* call);
    /// One stage's native bindings on table `table`: the tracker's textures and buffers, the encoder's samplers and bytes.
    bool bindNativeStage(const char* call, NativeStage stage, size_t table);
    /// Marks each level a Write or ReadWrite texture binding of the stage covers as written, in every slice, so a later
    /// pass that loads it keeps what was written (metalWritten).
    void markNativeWrites(NativeStage stage);
    /// A dispatch of either kind after the tracker accepted it: the pipeline state, the bindings, a barrier when asked.
    void encodeDispatch(const char* call, DispatchSize size, DispatchSize threadsPerGroup, bool groups, bool barrier);
    /// Ends the open compute encoder with the producer barrier.
    void closeCompute();
    /// Opens the compute pass's native encoder when none is open: the queue barrier, the compute table. False after
    /// one WARN line.
    bool openCompute();
    /// The clear draw's uniforms (clear_color, clear_depth) in the frame's ring on buffer 0 of the
    /// stages that read them, through the same binding site. The shadows' buffer 0 places are made
    /// unwritten, so the next draw writes its own block there again. False after one WARN line.
    bool bindForClear(Program& program, const std::array<F32, 4>& rgba, F32 depthValue);
    /// A mip level's draw on its own native encoder: the mip program's source, the view of the level
    /// above, and the device's bilinear sampler on the fragment stage through the same binding site, and
    /// the tables set on the encoder; the scratch is checked resident with the Metal debug setting. The
    /// tracker's bindings are not touched. False after one WARN line.
    bool bindForMip(Program& program, Texture* scratch, IdMTLTexture source, IdMTL4RenderCommandEncoder encoder);
    /// The flip pass's draw on its own native encoder: the present program's source, the back buffer, and
    /// the device's point sampler on the fragment stage through the same binding site, and the tables set on the
    /// encoder; the back buffer is checked resident with the Metal debug setting. The drawable is the encoder's
    /// attachment, bound through no table and resident through the layer's set on the queue, so it is not checked.
    /// The tracker's bindings are not touched. False after one WARN line.
    bool bindForPresent(Program& program, Texture* back, IdMTL4RenderCommandEncoder encoder);
    /// The format copy's draw on its own native encoder: the format copy program's source, the source's
    /// scratch, and the device's point sampler on the fragment stage through the same binding site, and the tables set
    /// on the encoder; the scratch is checked resident with the Metal debug setting. The tracker's bindings are not
    /// touched. False after one WARN line.
    bool bindForFormatCopy(Program& program, Texture* source, IdMTL4RenderCommandEncoder encoder);
    /// The argument tables, made at the first ask and set on the open native encoder. False after
    /// one WARN line, which begins with call.
    bool makeTables(const char* call);
    /// The draw's pipeline state on the open native encoder when it changed: the variant without alpha for the
    /// segment's attachments that have none in GL. False when the state failed.
    bool setNativePipeline();
    /// The tracker's pipeline waited for while it is being made (metalPipelineState), before the tracker judges a
    /// draw; nothing for a Ready one.
    void waitForPipeline();
    /// Leaves a native buffer slot without ring bytes (slots out of range are ignored).
    void clearNativeBytes(NativeStage stage, U32 slot);
    /// After a copy into a render target without alpha in GL, alpha 1 over the region on that level and
    /// layer, drawn with the clear program, mask alpha only, on an encoder of its own.
    void alphaAfterCopy(Texture* dst, U32 level, U32 layer, const Region& region);
    /// copyTexture between plain colour formats with one sample (copyRefusal): a blit of
    /// the region into a 2D scratch of the source's format with no swizzle, a draw of the format copy program into a 2D
    /// scratch of the destination's format, and a blit of that into the destination's level, layer and origin, with
    /// GL's zeros around a first write; each on an encoder of its own after the queue barrier. False after a WARN line.
    bool formatCopy(Texture* src, U32 srcLevel, U32 srcLayer, const Region& srcRegion, Texture* dst, U32 dstLevel,
                    U32 dstLayer, U32 dstX, U32 dstY);
    /// copyTexture's resolve of a whole multisampled colour texture into the level and layer of a
    /// one-sample texture of its format and size: an empty pass of its own that loads the source and stores it with
    /// MTLStoreActionMultisampleResolve. False when Metal gave no encoder (after a WARN line).
    bool resolveCopy(Texture* src, Texture* dst, U32 level, U32 layer);
    /// The open native encoder's viewport, cull mode, depth state, fill mode, depth bias (units and slope scale,
    /// no clamp) and depth clip mode, each set only when it differs from what the encoder has: Metal API
    /// Validation asserts on a redundant call and on a state that is set again before a draw used it.
    /// setNativeClip counts the encoders and the sends for the golden gate.
    void setNativeViewport(const MTLViewport& viewport);
    void setNativeCull(MTLCullMode mode);
    void setNativeDepth(IdMTLDepthStencilState state);
    void setNativeFill(MTLTriangleFillMode mode);
    void setNativeBias(float units, float slope);
    void setNativeClip(MTLDepthClipMode mode);
    /// The active sample query's visibility result mode and slot offset on the open native encoder right before
    /// a draw, or Disabled for a clear draw (GL's clear adds no samples) and with no query; only when it differs.
    /// Set at the draw, never at beginQuery or endQuery, as the other state: an end and the next begin with
    /// no draw between would leave a mode no draw used (a run-time probe).
    void setNativeVisibility(bool neutral);
    /// At the frame's end and at reset(), where the tracker drops it, the active sample query gets no end:
    /// it is abandoned (ready, reading as visible), and the next draw has no mode. The active time query likewise
    /// (ready, 0 ns).
    void abandonQuery();
    /// A time query's begin and end, after the tracker's accept: a pair of the ring and its stamps.
    void beginTimer(MetalTimerQuery& query);
    void endTimer(MetalTimerQuery& query);
    /// A stamp into the timestamp heap at a boundary of the pass: an open native encoder ends
    /// first (a split, counted in timerSplits), and with none open there is no split. False without the heap.
    bool writeTimestamp(U32 index);
    /// The tracker's scissor, clipped to the attachment, on the open native encoder right before a
    /// draw or a clear draw, and only when it differs from what the encoder has; setScissor only records it.
    /// Metal API Validation warns on a rectangle set again before a draw used it and on a redundant one.
    void setNativeScissor();
    /// The dynamic setters only record; right before each native draw the open encoder gets
    /// the tracker's state the draw uses, through the setNative* helpers, so validation never sees a state
    /// set again before a draw used it. Every draw: the viewport (the whole attachment when none was set),
    /// the depth state (with a depth attachment), the depth bias and the depth clip mode (GL_DEPTH_CLAMP),
    /// then the scissor. A triangle draw also gets the cull mode and the fill mode, and its bias when the
    /// enable for the fill mode is on (biasFill, biasLines); a point or line draw keeps the encoder's cull
    /// and fill modes, which do not apply to it, and gets no bias: GL's polygon offset applies to polygons only (glPolygonOffset).
    void applyDrawState(PrimitiveType type);
    /// A clear draw's state: the whole attachment, no culling, the given depth state
    /// (with a depth attachment), fill, no bias, and the tracker's scissor. The depth clip mode stays as
    /// it is: the clear's depth lies in [0, 1], where Clip and Clamp give the same. The next draw sets the
    /// tracker's state again.
    void applyClearState(IdMTLDepthStencilState depth);
    /// The sampler of a description and its gpuResourceID, from mSamplerIDs or else the device's getSampler; false
    /// when the device gave none (getSampler wrote the WARN line).
    bool samplerID(const SamplerDesc& desc, MTLResourceID& id);
    /// Every remembered resolve of a sampler place is forgotten: at the frame's end, where the tracker and
    /// dropUnheldSlots may let go of bindings, and at reset().
    void forgetResolves();
    /// mBlocks' buffers set again from the tracker's uniform buffers.
    void syncBlocks();
    /// drawIndexed's indices first..first + count of the index buffer as the draw reads them from the ring: a fan's,
    /// strip's or loop's list (metalListOrder), or a native primitive's indices as they are, each widened to 32 bits
    /// with `widen`. A list made before is kept (mIndexConversions) and given again while the buffer has had no update
    /// since; `source` is metalIndexBytes(indices), at least first + count indices. With the Metal debug setting a kept
    /// list is made again and compared, a difference counted in resolveMismatches with a WARN line.
    const std::vector<std::byte>& indexConversion(Buffer* indices, IndexType indexType, PrimitiveType type, U32 first,
                                                  U32 count, bool widen, std::span<const std::byte> source);
    /// With the Metal debug setting: a buffer's kept GPU address (metalAddress) against its allocation's; a difference
    /// is counted in resolveMismatches with a WARN line naming `what`.
    void checkAddress(const char* call, const std::string& what, MTLGPUAddress kept, const MetalAllocation& memory);
    /// The argument table writes, each only when the stage's shadow holds another value.
    void writeAddress(size_t stage, U32 index, MTLGPUAddress address);
    void writeTexture(size_t stage, U32 index, MTLResourceID texture);
    void writeSampler(size_t stage, U32 index, MTLResourceID sampler);
    /// At the frame's end, empties each type slot whose texture nobody but the slots and the tracker holds.
    void dropUnheldSlots();

    MetalDevice& mDevice;
    EncoderTracker mTracker;
    RenderPassDesc mSegment;   ///< the open segment: the pass's attachments and its load actions
    bool mSegmentWork = false; ///< the segment has drawn
    IdMTL4RenderCommandEncoder mNative = nil;    ///< the segment's native encoder while it is open
    MTL4RenderPassDescriptor* mOpenPass = nil;   ///< mNative's descriptor
    IdMTLRenderPipelineState mNativePipeline = nil; ///< the state mNative draws with
    U32 mNativeAlphaLess = 0;                       ///< mNative's attachments without alpha in GL (open())
    /// setNativePipeline's last lookup: the pipeline's id, the mask and the state, which serve the next draw of
    /// that pipeline with that mask.
    IdMTLRenderPipelineState mDrawState = nil;
    U64 mDrawPipelineId = 0;
    U32 mDrawAlphaLess = 0;
    MTLViewport mNativeViewport{};                  ///< mNative's viewport
    MTLCullMode mNativeCull = MTLCullModeNone;      ///< mNative's cull mode
    IdMTLDepthStencilState mNativeDepth = nil;      ///< mNative's depth state; nil without a depth attachment
    MTLScissorRect mNativeScissor{};                ///< mNative's scissor; the whole attachment when it opens
    MTLTriangleFillMode mNativeFill = MTLTriangleFillModeFill; ///< mNative's fill mode
    float mNativeBiasUnits = 0.f;                   ///< mNative's depth bias
    float mNativeBiasSlope = 0.f;                   ///< mNative's depth bias slope scale
    MTLDepthClipMode mNativeClip = MTLDepthClipModeClip; ///< mNative's depth clip mode
    bool mNativeClipSent = false;                   ///< mNative has had a setDepthClipMode
    MTLVisibilityResultMode mNativeVisibility = MTLVisibilityResultModeDisabled; ///< mNative's
    NSUInteger mNativeVisibilityOffset = 0;                                    ///< mNative's offset
    /// The sample query between beginQuery and endQuery and its mode (one at a time: concurrentSampleQueries is
    /// false, as a render encoder has one visibility result mode); held, so the frame's end can abandon it.
    std::shared_ptr<MetalQueryRun> mActiveRun;
    MTLVisibilityResultMode mActiveMode = MTLVisibilityResultModeDisabled;
    /// The time query between beginQuery and endQuery (one at a time, the tracker's class 1); held, so
    /// the frame's end can abandon it.
    std::shared_ptr<MetalTimerRun> mActiveTimer;
    MTL4RenderPassDescriptor* mLastPass = nil;
    NSString* mLastEncoderLabel = nil; ///< tests: the last native encoder's label
    std::vector<MetalCopy> mPendingCopies; ///< readbacks in the open segment, encoded when it breaks
    std::unordered_map<U64, MetalProgramState> mPrograms; ///< by program id; ids are never reused
    /// By channel, the last texture setTexture gave for each
    /// texture type with its sampling, as GL's per-target bindings, and the type it gave last. A sampler reads its
    /// own type's slot; a null tell empties the slot of the channel's last type.
    struct TypeSlot
    {
        Ref<Texture> texture;
        SamplerDesc sampling;
    };
    struct ChannelSlots
    {
        std::array<TypeSlot, size_t(TextureType::Tex3D) + 1> types{};
        TextureType last = TextureType::Tex2D;
    };
    std::unordered_map<U32, ChannelSlots> mTypeSlots;
    std::unordered_map<U32, SamplerDesc> mLastSamplers;  ///< by channel past RESOLVE_CHANNELS: what bindForDraw chose last (tests)
    /// bindForDraw's remembered resolve of each stage's sampler places. A place's texture and sampler follow from its
    /// channel, its sampler type and what setTexture gave the channel, so a place whose channel and type are the same
    /// as at its last resolve, and whose channel has had no setTexture that changed its slots or its binding since,
    /// takes the IDs found then. Channels from RESOLVE_CHANNELS on are resolved at every draw.
    static constexpr U32 RESOLVE_CHANNELS = 32;
    struct ResolvedPlace
    {
        U64 version = 0; ///< the channel's version at the resolve; 0: nothing remembered
        U32 channel = 0;
        UniformType type = UniformType::Sampler2D;
        MTLResourceID texture{};
        MTLResourceID sampler{};
        SamplerDesc desc;      ///< the sampler's description (lastSamplerDesc)
        bool slotRead = false; ///< the resolve counted a type slot read
    };
    std::array<std::array<ResolvedPlace, METAL_TABLE_SAMPLERS>, 2> mResolved{};
    /// By channel below RESOLVE_CHANNELS: a version that setTexture renews when it changes the channel's slots or
    /// binding; never 0. mNextVersion gives the new ones.
    std::array<U64, RESOLVE_CHANNELS> mChannelVersions{};
    U64 mNextVersion = 1;
    /// By channel below RESOLVE_CHANNELS: what bindForDraw chose last (tests).
    std::array<std::optional<SamplerDesc>, RESOLVE_CHANNELS> mLastChannelSamplers{};
    U64 mResolveMismatches = 0;
    U64 mResolveHits = 0;
    /// The samplers bindForDraw has asked the device for, with their gpuResourceIDs, so a resolve finds one without
    /// the device's lock and autorelease pool; the encoder holds them until the device goes.
    struct CachedSampler
    {
        Ref<Sampler> sampler;
        MTLResourceID id{};
    };
    std::unordered_map<SamplerDesc, CachedSampler, SamplerDescHash> mSamplerIDs;
    /// By uniform block slot: the buffer the tracker holds there and the range setUniformBuffer gave last (the
    /// tracker's and mBlockRanges' entries), kept in step at setUniformBuffer, the frame's end and reset(). The buffer
    /// is the tracker's, alive while it is here.
    struct BlockBinding
    {
        Buffer* buffer = nullptr;
        U64 offset = 0;
        U64 size = 0;
        bool ranged = false; ///< setUniformBuffer has given the slot a range
    };
    std::array<BlockBinding, 32> mBlocks{};
    /// programState's last answer, so a run of calls for one program finds its state without a lookup.
    U64 mLastProgramId = 0;
    MetalProgramState* mLastProgram = nullptr;
    /// indexConversion's lists, by the index buffer's id and writes, the range, the primitive, the index type and the
    /// widening; the least recently used goes past MAX_INDEX_CONVERSIONS, and a list of more than
    /// MAX_INDEX_CONVERSION_BYTES is made in mIndexScratch at each draw instead. reset() lets go of them.
    struct IndexConversion
    {
        U64 buffer = 0;
        U64 writes = 0;
        U32 first = 0;
        U32 count = 0;
        PrimitiveType type = PrimitiveType::Triangles;
        IndexType indexType = IndexType::U16;
        bool widen = false;
        U64 used = 0;
        std::vector<std::byte> bytes;
    };
    static constexpr size_t MAX_INDEX_CONVERSIONS = 64;
    static constexpr size_t MAX_INDEX_CONVERSION_BYTES = 1024 * 1024;
    std::vector<IndexConversion> mIndexConversions;
    std::vector<std::byte> mIndexScratch;
    U64 mIndexConversionUses = 0;
    U64 mIndexConversionHits = 0;
    U64 mIndexConversionsMade = 0;
    std::array<IdMTL4ArgumentTable, 3> mTables{};        ///< vertex, fragment, compute; made at the first draw
    std::array<MetalTableShadow, 3> mShadows{};          ///< what mTables hold; cleared with them
    /// The compute pass's native encoder and the state set on it.
    IdMTL4ComputeCommandEncoder mCompute = nil;
    IdMTLComputePipelineState mComputeState = nil;
    /// The pass timer's stamps of the open segment's encoder, the compute pass's and the backend's own render encoder.
    MetalPassStamp mNativeStamp;
    MetalPassStamp mComputeStamp;
    MetalPassStamp mOwnStamp;
    /// Native bindings the tracker does not hold, by NativeStage: samplers and the ring's bytes, per slot.
    struct NativeSlots
    {
        std::array<std::optional<SamplerDesc>, NATIVE_SAMPLER_SLOTS> samplers{};
        std::array<MTLGPUAddress, NATIVE_BUFFER_SLOTS> bytes{}; ///< 0: no bytes on the slot
    };
    std::array<NativeSlots, 3> mNativeSlots{};
    /// The compute pass's threadgroup memory by index, set again on an encoder opened after a submit split the pass.
    std::array<U32, NATIVE_THREADGROUP_SLOTS> mThreadgroupMemory{};
    U64 mNativeDispatches = 0;
    U64 mDispatchBarriers = 0;
    U64 mComputeEncoders = 0;
    /// By slot: the offset and size setUniformBuffer gave last, read only while the tracker holds
    /// the slot's buffer (GL's mBlockRanges).
    std::unordered_map<U32, std::pair<U64, U64>> mBlockRanges;
    U64 mRingBlockCopies = 0;
    U64 mTableWrites = 0;
    U64 mNativeDraws = 0;
    U64 mDiscardedStores = 0;
    /// The attachments the full clear being made covers, which close() leaves unstored when it ends the
    /// open native encoder (clear() sets them right before its split, close() resets them)
    bool mDiscardColor = false;
    bool mDiscardDepth = false;
    /// The open native encoder began with a split barrier, and its geometry stages' part has not been
    /// given (MetalDevice::vertexBarrier)
    bool mVertexWaitOpen = false;
    U64 mClearDraws = 0;
    U64 mMipDraws = 0;
    U64 mIndexChecks = 0;
    U64 mRenderEncoders = 0;
    U64 mFirstLoadClears = 0;
    std::vector<Ref<Texture>> mNewTargets; ///< zeroLater's targets not yet cleared
    U64 mNewTargetClears = 0;
    /// A depth clear carryDepthClear took, waiting for the next segment with that depth level and layer.
    struct DepthClear
    {
        Ref<Texture> texture;
        U32 level = 0;
        U32 layer = 0;
        F32 value = 1.f;
        std::string name; ///< the pass's, for the label of a pass of its own
    };
    std::vector<DepthClear> mDepthClears;
    U64 mDepthClearsCarried = 0;
    U64 mDepthClearsEncoded = 0;
    U64 mAlphaCopyDraws = 0;
    U64 mPresentDraws = 0;
    U64 mFormatCopyDraws = 0;
    U64 mUpscales = 0;
    U64 mScissorSends = 0;
    MetalEncoderStats mStats; ///< the pass breaks (llrhimetal.h)
    const std::thread::id mRecordingThread; ///< the thread that made the device
};

/// The lines that explain a Metal failure, held for the main thread and limited. LLError drops a line
/// from any thread that meets another thread's line (its try_to_lock, llcommon/llerror.cpp), and the commit feedback
/// handler, the upload queue's completion handlers and the program pool find their failures on their own threads, as
/// with the class lines (MetalCompileState::classLines). The device writes the held lines on the main
/// thread at the frame's end, after a program's wait(), after a feedback drain and when it goes; the viewer before it
/// stops for a device failure (metalWriteFailureLines). Each kind writes its first FIRST_LINES lines; after them its
/// lines are counted, and one line per SUMMARY_INTERVAL (and at the device's end) says how many and gives the last.
class MetalFailureLog
{
public:
    enum class Kind : U8
    {
        GpuError,   ///< a command buffer's commit feedback error that does not lose the device
        DeviceLost, ///< the error that loses it
        Upload,     ///< an upload command buffer's error
        Program,    ///< a program that failed (failJob)
        Drawable,   ///< the layer gave no drawable within its timeout (the main thread's own, limited alike)
        Count
    };
    static constexpr U64 FIRST_LINES = 10;
    static constexpr std::chrono::seconds SUMMARY_INTERVAL{ 60 };

    /// Any thread: the line is held, or counted once its kind has written its first lines.
    void add(Kind kind, std::string line);
    /// The main thread: the held lines as WARN lines, then a summary of each kind with lines counted since its last
    /// one, when SUMMARY_INTERVAL has passed since then, or always with `final`.
    void write(bool final = false);

private:
    struct Counts
    {
        U64 lines = 0;   ///< every line of the kind
        U64 counted = 0; ///< past its first lines, since its last summary
        std::string last;
        std::chrono::steady_clock::time_point summarized{};
    };
    std::mutex mMutex;
    std::vector<std::string> mHeld;
    std::array<Counts, size_t(Kind::Count)> mKinds{};
    /// add() has held or counted a line that write() has not written or summarized yet.
    std::atomic<bool> mPending{ false };
};

/// The upload queue's counters (MetalUploadCounts), which the upload thread, the main thread and the upload
/// command buffers' completion handlers update; a handler holds this and its event, never the device.
struct MetalUploadState
{
    std::thread::id recordingThread; ///< the thread that made the device, where a Fence::wait() is counted
    std::atomic<U64> submits{ 0 };
    std::atomic<U64> uploads{ 0 };
    std::atomic<U64> deferredMips{ 0 };
    std::atomic<U64> signalledAtHandover{ 0 };
    std::atomic<U64> queueWaits{ 0 };
    std::atomic<U64> mainThreadWaits{ 0 };
    std::atomic<U64> errors{ 0 };
    std::mutex mutex;
    std::condition_variable changed;
    U64 outstanding = 0; ///< committed upload command buffers whose completion handler has not run
    std::shared_ptr<MetalFailureLog> failures; ///< the device's, for the handlers' error lines
};

/// The one fence type of the upload context. It guards the upload queue's work up to value
/// on its event; value 0 guards nothing and is signalled at once (an empty submit, and every submit on the recording
/// thread, whose work is on the main encoder's command buffer). It carries the textures whose mips the upload thread
/// asked for since the last submit: MetalEncoder::waitForFence records them on the main encoder (so the upload thread
/// makes no program or pipeline).
class API_AVAILABLE(macos(26.0)) MetalUploadFence final : public Fence
{
public:
    MetalUploadFence(IdMTLSharedEvent event, U64 value, std::vector<Ref<Texture>> mips,
                     std::shared_ptr<MetalUploadState> state);

    /// The event's signaledValue has reached value.
    bool signaled() const override;
    /// The CPU waits until signaled(), at most METAL_UPLOAD_WAIT_MS (then one WARN line); counted when the caller is
    /// the recording thread (the swap never does it).
    void wait() override;

    IdMTLSharedEvent event() const { return mEvent; }
    U64 value() const { return mValue; }
    /// Main thread, at waitForFence: the textures whose mips are still to be made; the fence keeps none after.
    std::vector<Ref<Texture>> takeMips() { return std::exchange(mMips, {}); }
    MetalUploadState& state() const { return *mState; }

private:
    IdMTLSharedEvent mEvent;
    const U64 mValue;
    std::vector<Ref<Texture>> mMips;
    std::shared_ptr<MetalUploadState> mState;
};

/// The time limit of a CPU wait on an upload fence and of the device's end waiting for the upload queue.
inline constexpr U32 METAL_UPLOAD_WAIT_MS = 10000;

/// The upload thread's context. On the recording thread it does what the encoder
/// does, and submit() gives a fence of value 0. One other thread, the first that calls it (LLImageGLThread), is the
/// upload thread: its uploads go to a Metal 3 MTLCommandQueue of their own, made at its first upload, each on a blit
/// encoder of its own with a Shared staging buffer the command buffer holds; submit() commits them with one signal of
/// the queue's MTLSharedEvent and gives a fence of that value, whose completion handler counts a failed command
/// buffer with one WARN line and then sets the event from the CPU, so no queue waits forever. Its generateMipmaps notes
/// the texture for the next fence. A call from any further thread, and updateBuffer off the recording thread (no
/// caller: LLImageGL only uploads textures), gives one WARN line and does nothing.
class API_AVAILABLE(macos(26.0)) MetalUploadContext final : public UploadContext
{
public:
    explicit MetalUploadContext(MetalDevice& device);

    void updateBuffer(Buffer* buffer, U64 offset, std::span<const std::byte> data) override;
    void uploadTexture(Texture* texture, U32 level, U32 layer, const Region& region, const PixelLayout& layout,
                       std::span<const std::byte> data) override;
    void generateMipmaps(Texture* texture) override;
    Ref<Fence> submit() override;

    /// The counters, also for llrhimetal.h's metalUploadCounts.
    MetalUploadState& state() const { return *mState; }
    /// ~MetalDevice: waits for every committed upload command buffer's completion handler, at most
    /// METAL_UPLOAD_WAIT_MS, then one WARN line.
    void drain();
    /// ~MetalDevice, after drain(): lets go of what the upload thread recorded or noted and never submitted, the
    /// uncommitted command buffer and the textures noted for mipmaps, so their releases reach the device's lifecycle
    /// before its releaseAll().
    void dropPending();
    /// Tests: the upload queue's next command buffer begins with a wait until gate reaches value
    /// (encodeWaitForEvent:value:, MTLCommandBuffer.h:405-409), so its work cannot run before the test opens it.
    void gateNextCommands(IdMTLSharedEvent gate, U64 value);
    /// Tests: the upload queue, nil before the first upload on the upload thread.
    IdMTLCommandQueue queue() const { return mQueue; }

private:
    enum class Caller
    {
        Recording, ///< the thread that made the device
        Upload,    ///< the upload thread
        Other      ///< refused with one WARN line
    };
    /// Which thread calls; the first thread other than the recording thread becomes the upload thread.
    Caller caller(const char* call);
    /// Upload thread: the command buffer being recorded, begun on the queue (made at the first ask) when none is.
    /// Nil after a WARN line.
    IdMTLCommandBuffer commands();

    MetalDevice& mDevice;
    const std::thread::id mRecordingThread; ///< the thread that made the device
    std::atomic<std::thread::id> mUploadThread{}; ///< none until the first call from another thread
    std::shared_ptr<MetalUploadState> mState;
    // The upload thread's own
    IdMTLCommandQueue mQueue = nil;
    IdMTLSharedEvent mEvent = nil;
    U64 mValue = 0; ///< the value of the last submit's signal
    IdMTLCommandBuffer mCommands = nil;
    std::vector<Ref<Texture>> mMips; ///< noted since the last submit
    std::mutex mGateMutex;           ///< the test's gate, set on the main thread
    IdMTLSharedEvent mGate = nil;
    U64 mGateValue = 0;
};

/// The alignment of a frame ring's allocations, also of the fragment stage's block after the
/// vertex stage's in a draw's uniform bytes.
inline constexpr U64 METAL_RING_ALIGNMENT = 256;

/// One frame slot's bytes for its frame's commands (uniform blocks, upload staging, static
/// updates). Bump allocation at FRAGMENT_BLOCK_ALIGNMENT in 4 MiB Shared chunks, admitted when they
/// are made; every TRIM_REWINDS rewinds the slot keeps only as many as its fullest frame in that time
/// used (one at least), and the rest retire through the lifecycle, so a burst does not keep its chunks. An allocation over 1 MiB and up to
/// KEEP_MAX takes the smallest unused large staging buffer of the slot that holds it (best fit), else
/// a new one of whole MiB, which the slot keeps while its kept bytes stay within
/// KEEP_SLOT_MAX; one unused for TRIM_REWINDS rewinds of its slot retires through the lifecycle (about
/// 12 s at 60 fps with 3 slots). Every main-thread upload of that size takes this path, world textures
/// too, so a burst (a login or teleport) leaves up to KEEP_SLOT_MAX per slot for those 12 s.
/// A larger allocation, and one the cap leaves out, gets a Shared buffer of its own,
/// which retires with the frame. A kept buffer
/// is handed out again only after the rewind, which follows the GPU's end of the slot's previous frame,
/// so the GPU never reads it while the CPU writes it. MetalDevice::endFrame rewinds it once the
/// slot's previous frame has finished. Main thread.
class API_AVAILABLE(macos(26.0)) MetalRing
{
public:
    static constexpr U64 CHUNK_BYTES = 4 * 1024 * 1024;
    static constexpr U64 OWN_BUFFER_OVER = 1024 * 1024;
    static constexpr U64 ALIGNMENT = METAL_RING_ALIGNMENT;
    static constexpr U64 KEEP_MAX = 8 * 1024 * 1024;   ///< the largest allocation a slot keeps for reuse
    static constexpr U64 KEEP_ROUND = 1024 * 1024;     ///< a kept buffer's size in whole MiB
    static constexpr U32 TRIM_REWINDS = 240;           ///< the slot's rewinds a kept buffer may go unused
    static constexpr U64 KEEP_SLOT_MAX = 32 * 1024 * 1024; ///< the bytes a slot keeps at most

    /// Nil buffer after a WARN line.
    MetalAllocation allocate(MetalDevice& device, U64 size);
    /// The slot starts again; its kept buffers unused since the last rewind count one idle rewind more.
    void reset(MetalDevice& device);
    /// Tests: the chunk, nil past the last.
    IdMTLBuffer chunk(size_t index) const { return index < mChunks.size() ? mChunks[index] : nil; }
    /// Tests: the large staging buffers the slot keeps, their bytes, and those it has made.
    size_t keptBuffers() const { return mKept.size(); }
    U64 keptBytes() const { return mKeptBytes; }
    U64 keptMade() const { return mKeptMade; }

private:
    struct Kept
    {
        IdMTLBuffer buffer = nil;
        U64 length = 0;
        bool used = false; ///< handed out since the last rewind
        U32 idle = 0;      ///< rewinds without a use
    };
    std::vector<IdMTLBuffer> mChunks;
    size_t mChunk = 0;
    U64 mOffset = 0;
    size_t mChunksNeeded = 0; ///< the most chunks a frame used since the last trim
    U32 mRewinds = 0;         ///< rewinds since the last trim
    std::vector<Kept> mKept;
    U64 mKeptBytes = 0; ///< the lengths in mKept
    U64 mKeptMade = 0;
};

/// Dynamic buffers up to METAL_POOL_MAX_BLOCK in blocks of size classes 256 B to
/// 64 KiB (powers of two), carved from 4 MiB Shared chunks that are admitted once and always
/// resident. A free block comes back through FrameLifecycle::release, so it is handed out again only
/// after the GPU has finished the frame that let it go. Any thread.
class API_AVAILABLE(macos(26.0)) MetalBufferPool
{
public:
    static constexpr U64 CHUNK_BYTES = 4 * 1024 * 1024;
    static constexpr U64 MIN_BLOCK = 256;
    static constexpr size_t CLASSES = 9; ///< 256 B .. 64 KiB

    /// A block of at least size bytes; nil buffer after a WARN line.
    MetalAllocation acquire(MetalDevice& device, U64 size);
    /// The block of a buffer of that size is free again; through FrameLifecycle::release only.
    void giveBack(const MetalAllocation& block, U64 size);
    size_t chunks() const;

private:
    static size_t sizeClass(U64 size);

    struct SizeClass
    {
        std::vector<MetalAllocation> free;
        IdMTLBuffer chunk = nil; ///< the chunk blocks are carved from
        U64 next = CHUNK_BYTES;  ///< the next block's offset in it
    };

    mutable std::mutex mMutex;
    std::array<SizeClass, CLASSES> mClasses;
    size_t mChunks = 0;
};

/// The one residency site, in llrhimetalbinding-objc.mm: the only code that names
/// the residency set's selectors or MTLResidencySet (scripts/metal/residency-site.py). An allocation
/// is admitted when it is made and retired when it is released. Any thread: the upload thread
/// may create textures too (llrhi.h's Device contract), and the set documents no thread safety, so
/// it changes only under the mutex.
class API_AVAILABLE(macos(26.0)) MetalResidency
{
public:
    /// One set on the queue from the start (MTL4CommandQueue.h:318); false after a WARN line.
    bool attach(IdMTL4CommandQueue queue);
    /// Adds the allocation (MTLResidencySet.h:82), committed before the next submit; the name is
    /// for the WARN line when there is no allocation.
    void admit(IdMTLAllocation allocation, std::string_view name);
    /// Removes the allocation (MTLResidencySet.h:95), committed before the next submit.
    void retire(IdMTLAllocation allocation);
    /// Whether the set holds the allocation, committed or not (MTLResidencySet.h:111-115), so the
    /// commit before each submit is what makes it resident on the GPU.
    bool contains(IdMTLAllocation allocation) const;
    /// Whether a texture is resident as the GPU sees it: a view through the texture it views (its parentTexture,
    /// followed to the root), and a texture made in a heap through its heap; else the texture itself.
    bool containsTexture(IdMTLTexture texture) const;
    /// Commits the set when it changed (MTLResidencySet.h:136); before each submit.
    void commitIfChanged();
    U64 count() const;
    /// Tests: takes the allocation out of the set, as a forgotten admit would leave it.
    void dropForTest(IdMTLAllocation allocation);
    /// The surface's layer's own set on the queue as well, which keeps the layer's drawables resident and
    /// which the layer updates by itself (CAMetalLayer.h:160-171; MTL4CommandQueue.h:318). The layer's device must be
    /// set first: the set is the device's. False after a WARN line (a layer with no set).
    bool attachLayer(IdMTL4CommandQueue queue, CAMetalLayer* layer);
    /// The layer's set leaves the queue when the device ends (MTL4CommandQueue.h:333-339).
    void detachLayer(IdMTL4CommandQueue queue);
    /// The device's set and the layer's on a second queue too (the surface's present queue, which copies finished
    /// frames into drawables); false after a WARN line. detachSecond takes both off it again.
    bool attachSecond(IdMTL4CommandQueue queue, CAMetalLayer* layer);
    void detachSecond(IdMTL4CommandQueue queue, CAMetalLayer* layer);
    /// The allocations in the set, admitted and not yet retired (so released ones the lifecycle has not
    /// freed yet are counted), grouped by what they are -- a texture's size, format, levels and samples, a
    /// buffer's name -- largest first: each group's count and bytes (MTLAllocation's allocatedSize), the
    /// first top groups and the total. For the viewer's memory census (metalMemoryCensus).
    std::string census(size_t top, U64& totalBytes) const;
    /// Tests: the layer's set attachLayer recorded; nil before it and after detachLayer. It shows the
    /// bookkeeping only: a queue has no getter for its residency sets (MTL4CommandQueue.h:310-345), so
    /// scripts/metal/residency-site.py checks that the queue call comes before the record in both functions.
    IdMTLResidencySet layerSet() const;

private:
    struct CensusEntry
    {
        std::string group;
        U64 bytes = 0;
    };

    mutable std::mutex mMutex;
    IdMTLResidencySet mSet = nil;
    IdMTLResidencySet mLayerSet = nil; ///< the surface's layer's set while it is on the queue
    std::unordered_map<const void*, CensusEntry> mCensus; ///< each admitted allocation's group and size
    mutable std::unordered_map<std::string, U64> mAdmittedSince; ///< bytes admitted per group since the last census
    mutable std::unordered_map<std::string, U64> mRetiredSince;  ///< bytes retired per group since the last census
    bool mChanged = false;
};

/// The reason for an indexed draw's refusal, or empty: indices past the index buffer,
/// or, where the caller has the CPU copy of the indices, a value at or past vertexCount. For
/// drawIndexed; in llrhimetalbinding-objc.mm.
std::string metalCheckIndexRange(const Buffer& indices, IndexType type, U32 first, U32 count, U32 vertexCount,
                                 std::span<const std::byte> cpuIndices);

/// The whole vertices the fewest of the read streams holds (the binding's real vertex count, not a
/// stream-0 size rule); U32's maximum when the pipeline reads no stream, 0 without a vertex buffer. In
/// llrhimetalbinding-objc.mm.
U64 metalWholeVertices(Buffer* vertices, const VertexStreams& streams, U32 attributes) API_AVAILABLE(macos(26.0));

// Programs in the background -----------------------------------------------------------

/// One program's translation and compilation, in llrhimetalpipeline-objc.mm. The pool's threads
/// and the library handlers hold only the job and MetalCompileState, never a Ref, so a MetalProgram
/// is released on the main thread only.
struct API_AVAILABLE(macos(26.0)) MetalProgramJob;

/// What the library handlers report, after FeedbackState's pattern. The device owns it; each
/// handler holds a copy and counts itself down as its last step, so ~MetalDevice can wait for it.
struct MetalCompileState
{
    std::mutex mutex;
    std::condition_variable changed;
    U64 outstanding = 0;        ///< library handlers sent and not yet finished
    bool closing = false;       ///< ~MetalDevice has stopped waiting for the handlers
    U64 handlersAfterClose = 0; ///< handlers that finished after that, which are late
    U64 groupFallBacks = 0;     ///< groups whose library failed, so their members went alone
    U64 pointTranslations = 0;  ///< point variants translated (tests)
    /// Each translated program's line with its stages' files and classes, held for the main thread,
    /// which writes them at the frame's end and when the device goes (MetalDevice::writeClassLines). LLError drops a
    /// line from any thread, the main thread too, when another thread is logging at that moment (its try_to_lock),
    /// which lost 16 of 203 lines from the pool's threads in a start with a warm disk cache; writing them on one thread
    /// only lowers that contention (203 of 203 in two runs), so no check depends on these lines.
    std::vector<std::string> classLines;
    /// The device's failure lines, which the pool's threads and the handlers hold for the main thread.
    std::shared_ptr<MetalFailureLog> failures = std::make_shared<MetalFailureLog>();
    /// Program jobs that failed (failJob, not cancelled), for measured.rhi.compile, which
    /// golden level 2 holds to 0 in a Metal world run: a program that gives up leaves no class line and no WARN that
    /// level 2 could miss in a dropped log line.
    std::atomic<U64> failedPrograms{ 0 };
    /// Creations a test fault failed in createMetalProgram (never in failedPrograms), and the Pending
    /// answers MetalProgram::status() gave for the deferred ones, for measured.rhi.compile.
    std::atomic<U64> faultedPrograms{ 0 };
    std::atomic<U64> faultPendingAnswers{ 0 };
    /// The calling threads' time in nanoseconds, translating (login programs, stolen jobs) and waiting for a
    /// program's libraries, a pipeline state or a point variant (MetalCompileStats).
    std::atomic<U64> callerTranslationNs{ 0 };
    std::atomic<U64> programWaitNs{ 0 };
    std::atomic<U64> pipelineWaitNs{ 0 };
    std::atomic<U64> pointWaitNs{ 0 };
    /// The same waits by MetalWaitKind: how many waited, their nanoseconds and the longest single one.
    std::array<std::atomic<U64>, METAL_WAIT_KINDS> waitCount{};
    std::array<std::atomic<U64>, METAL_WAIT_KINDS> waitKindNs{};
    std::array<std::atomic<U64>, METAL_WAIT_KINDS> longestWaitNs{};
    /// The group libraries sent, those sent from the main thread, the handlers that have run, their time from
    /// the send to the handler's start (in all and the longest) and the QoS classes they ran at (metalQosBit).
    std::atomic<U64> groupsSent{ 0 };
    std::atomic<U64> groupsFromMain{ 0 };
    std::atomic<U64> groupsDone{ 0 };
    std::atomic<U64> groupLatencyNs{ 0 };
    std::atomic<U64> groupLatencyMaxNs{ 0 };
    std::atomic<U32> groupHandlerQos{ 0 };
};

/// A group of programs whose MSL is one library, in llrhimetalpipeline-objc.mm.
struct API_AVAILABLE(macos(26.0)) MetalProgramGroup;

/// Tests: a sent group's source as a digest, and its members.
struct MetalGroupRecord
{
    U64 number = 0; ///< the group's place in the order the groups were opened
    size_t digest = 0;
    U32 members = 0;
};

/// The device's groups. Ordinary programs join the open group in createProgram's order; it
/// closes at 16 members, at wait() on a member, at MetalDevice::endFrame and at the device's end.
/// The thread that translates the last member of a closed group sends its library.
class API_AVAILABLE(macos(26.0)) MetalProgramGroups
{
public:
    static constexpr size_t MAX_MEMBERS = 16;

    /// Main thread, in createProgram: the job joins the open group.
    void add(const std::shared_ptr<MetalProgramJob>& job);
    /// Closes the group if it is open, and sends it when its members are translated.
    void close(const std::shared_ptr<MetalProgramGroup>& group);
    /// MetalDevice::endFrame: closes the open group.
    void closeOpen();
    /// The group's members, for wait() to translate those still queued.
    std::vector<std::shared_ptr<MetalProgramJob>> members(const std::shared_ptr<MetalProgramGroup>& group);
    /// A member is translated, failed or cancelled; the last of a closed group sends it.
    void translated(MetalProgramJob& job);
    /// ~MetalDevice, before the pool stops: no group is sent from now on.
    void cancel();
    /// Tests: the sent groups by their number, the last MAX_RECORDS of them (shader reloads send
    /// groups for the device's whole life).
    std::vector<MetalGroupRecord> records() const;
    static constexpr size_t MAX_RECORDS = 256;
    /// The sum of every sent group's digest, in any order (MetalCompileStats::groupHash), kept beside the records.
    U64 digestSum() const;

private:
    /// Called with mMutex held, which it releases; sends the group when it is closed and translated.
    void submitIfTranslated(std::unique_lock<std::mutex>& lock, const std::shared_ptr<MetalProgramGroup>& group);
    void closeLocked(const std::shared_ptr<MetalProgramGroup>& group);

    mutable std::mutex mMutex;
    std::shared_ptr<MetalProgramGroup> mOpen;
    U64 mGroups = 0;
    bool mCancelled = false;
    std::vector<MetalGroupRecord> mRecords;
    U64 mDigestSum = 0;
};

/// The device's translation threads, started at the first createProgram: max(1, active
/// processors / 2) threads at QOS_CLASS_USER_INITIATED (QOS_CLASS_UTILITY with
/// LL_METAL_COMPILE_SLOW), which take the jobs from one queue in order.
class API_AVAILABLE(macos(26.0)) MetalProgramPool
{
public:
    ~MetalProgramPool() { stop(); }
    /// Starts the threads the first time. Main thread.
    void start();
    void push(std::shared_ptr<MetalProgramJob> job);
    /// Takes the job out of the queue; false when a thread has taken it already.
    bool steal(const MetalProgramJob* job);
    /// Takes the job at the queue's front out of it, also while paused, as wait() takes its own; null
    /// when the queue is empty.
    std::shared_ptr<MetalProgramJob> takeNext();
    /// Tests: the threads take nothing while paused.
    void pause(bool paused);
    /// Cancels what is queued and joins the threads, which do not start again.
    void stop();
    size_t threads() const { return mThreads.size(); }
    /// The QoS class the threads run at (qos_class_t's value), set before start(); the device's knob and the
    /// tests set it.
    void setQos(U32 qos) { mQos = qos; }
    U32 qos() const { return mQos; }
    /// The threads start() starts: max(1, NSProcessInfo's activeProcessorCount / 2).
    static size_t threadCount();

private:
    void run();

    std::mutex mMutex;
    std::condition_variable mChanged;
    std::deque<std::shared_ptr<MetalProgramJob>> mQueue;
    std::vector<std::thread> mThreads;
    U64 mSeq = 0; ///< the jobs' order in the queue
    U32 mQos = QOS_CLASS_USER_INITIATED; ///< read by each thread as it starts
    bool mPaused = false;
    bool mStopping = false;
};

/// One full render pipeline state, made by its compiler handler; in
/// llrhimetalpipeline-objc.mm. The handlers hold it and MetalCompileState only, never the device.
struct API_AVAILABLE(macos(26.0)) MetalPipelineHolder;

/// The device's pipeline states, which the MetalPipelines in PipelineCache share: one full state per
/// program, attribute mask (the program's attributes only), primitive class, sample count, attachment
/// formats, blend and color mask. The depth format is not pipeline state in Metal 4
/// (MTL4RenderPipeline.h:156-229), so no key has it. A refused key is remembered, so it gives one WARN
/// line. Main thread, as getPipeline.
class API_AVAILABLE(macos(26.0)) MetalPipelineStates
{
public:
    /// The full state of the description, sent to Metal's compiler when it is new.
    std::shared_ptr<MetalPipelineHolder> state(MetalDevice& device, Program& program, const PipelineDesc& desc,
                                                U32 attributes);
    /// True the first time the key is refused, so the caller writes its one WARN line.
    bool refuse(const PipelineKey& key);
    /// ~MetalProgram: the program's states and refusals go, beside PipelineCache::forgetProgram.
    void forgetProgram(U64 programId);
    /// ~MetalDevice, after the handlers have reported.
    void clear();
    size_t full() const;
    size_t refused() const;
    /// Tests: the color attachments below colorCount that the last full descriptor sent had
    /// with blending Disabled, one bit each (bit i for attachment i).
    U32 lastFullDisabled() const;
    /// MetalPipeline counts a variant without alpha it sends.
    void countVariant();
    /// The states sent since the device was made and the refusals; the rest of MetalCompileStats is the
    /// device's (metalCompileStats).
    MetalCompileStats stats() const;

private:
    mutable std::mutex mMutex;
    std::unordered_map<PipelineKey, std::shared_ptr<MetalPipelineHolder>, PipelineKeyHash> mFull;
    std::unordered_set<PipelineKey, PipelineKeyHash> mRefused;
    U32 mLastFullDisabled = 0;
    /// What was sent and refused since the device was made, kept when a program goes.
    MetalCompileStats mSent;
};

/// A window server that has taken none of the layer's drawables for this long no longer holds a
/// present back (metalPresentPaced): about six refreshes at 60 Hz, where it takes one every refresh.
inline constexpr U64 METAL_WINDOW_SERVER_STALE_NS = 100'000'000;

/// With VSync off, after nextDrawable gave nil (its 1 s timeout), present() asks again no sooner than this after it
/// gave up, unless a presented handler has run since the ask (MetalSurface::present).
inline constexpr U64 METAL_DRAWABLE_RETRY_NS = 3'000'000'000;

/// With VSync off, while the window server has stalled (drawables queued, no presented handler within the stale limit)
/// and the window counts as hidden (its occlusion state), present() asks for a drawable no more often than this, so an
/// occlusion state that stays wrong cannot hold the picture for good (MetalSurface::presentWithheld).
inline constexpr U64 METAL_HIDDEN_RETRY_NS = 10'000'000'000;

/// With VSync off, the finished frames a surface keeps for its present queue (MetalSurface::flipFinished): the frames
/// whose flip the main queue may still be running (FrameLifecycle::MAX_FRAMES_IN_FLIGHT - 1 besides the one being
/// recorded) and the newest one the GPU has finished. Each is a BGRA8Unorm texture of the drawable's size.
inline constexpr size_t METAL_FINISHED_FRAMES = 3;

/// Whether present() leaves the frame in the back buffer instead of asking for a drawable. Only with
/// VSync off (RenderVSyncEnable off, GL's swap interval 0), no drawable readback asked for and a layer with drawables
/// (drawableCount above 0): when the window server has every drawable of the layer but the one on screen (presented,
/// their presented handler not run yet), nextDrawable waits for its next composite (a probe: slow frames spent
/// 12.4-13.6 ms of 13.1-14.2 ms there), so the frame is left out and the next present gives the window server the
/// newest back buffer. Unlike GL's swap interval 0, where the compositor drops older frames and shows the newest, the
/// frames left out here are the newest. A drawable is shown only once the GPU has finished its frame, so how many
/// frames reach the screen is bounded by the drawables over that delay, not by the refresh. Not once neither the window server nor present() has given
/// progress for staleNs (sinceProgressNs; METAL_WINDOW_SERVER_STALE_NS, which only a test changes): then present()
/// asks as before, so a window server that stops taking drawables never leaves the window without presents.
bool metalPresentPaced(bool vsync, bool drawableReadback, U32 withWindowServer, U32 drawableCount,
                       U64 sinceProgressNs, U64 staleNs = METAL_WINDOW_SERVER_STALE_NS);

/// A development knob for a control run, from the environment, read once: LL_METAL_PRESENT_UNSYNCED=1 has
/// setVSync(false) turn the layer's displaySyncEnabled off too, so a drawable goes to the display without waiting for
/// its refresh, with the tearing that brings. Off by default: the layer stays display synced, and VSync off only paces
/// the presents.
bool metalPresentUnsyncedKnob();

/// A development knob for a timing run, from the environment, read once: LL_METAL_PRESENT_TRACE=1 has the surface
/// write a line a second (tag RHI, "Metal present trace") of its presents, the drawables it presented and when their
/// presented handlers saw them on screen. Off by default.
bool metalPresentTraceKnob();

/// The window's surface on Metal, in llrhimetalsurface-objc.mm. It owns the back buffer, an ordinary
/// RGBA8Unorm texture in GL's row order, with its Depth32Float depth, both made through the device and resident from
/// creation, and the CAMetalLayer it presents into. Main thread.
class API_AVAILABLE(macos(26.0)) MetalSurface final : public Surface
{
public:
    /// The layer is set up for the device (CAMetalLayer.h:60-171): device, BGRA8Unorm, framebufferOnly, opaque, no
    /// colorspace, display synced, three drawables with the 1 s timeout, and drawableSize width x height when both are
    /// above 0 (the
    /// window's backing size; the view keeps it after). width x height is the surface's size, 0 allowed; the back
    /// buffer is max(width, 1) x max(height, 1). ok() is false after a WARN line when a buffer could not be made.
    MetalSurface(MetalDevice& device, CAMetalLayer* layer, U32 width, U32 height);
    ~MetalSurface() override;

    bool ok() const { return mColor.notNull() && mDepth.notNull(); }

    Texture* backBuffer() override { return mColor.get(); }
    Texture* backBufferDepth() override { return mDepth.get(); }
    /// Records the size only; present() applies it after the flip pass, the frame boundary.
    void resize(U32 width, U32 height) override;
    /// Only kept for present(): the layer stays display synced (displaySyncEnabled), so the window server shows
    /// whole frames at the display's refresh; off, present() paces by the window server's drawables
    /// (metalPresentPaced) instead of waiting in nextDrawable. With metalPresentUnsyncedKnob the layer follows too.
    void setVSync(bool enabled) override;
    /// With a 0 in the size the frame is to have (the pending one, else the current one) no drawable is asked
    /// for; else the layer's next drawable, in its own autorelease pool, gets the flip pass, and the submit that sends
    /// it runs at once (MetalDevice::setPendingDrawable, then submitPending: the startup states
    /// present several times in one frame, and the layer has three drawables); then a pending resize is applied.
    /// With VSync off, no drawable is asked for while metalPresentPaced says the window server has the others,
    /// while none of the window shows (its occlusion state), or after nextDrawable gave nil, until a presented handler
    /// has run since that ask or METAL_DRAWABLE_RETRY_NS has passed (presentWithheld); each counts as a paced present.
    /// A drawable presented with VSync off gets a presented handler, which counts it back.
    void present() override;
    /// With VSync off, by default, the window server gets the newest frame the GPU has finished, as GL's compositor
    /// shows the newest swap: present() flips the back buffer into a finished frame (flipFinished), sent at once, and
    /// a second command queue copies the newest finished one into a drawable (presentFinished), at present() and
    /// after the frame lifecycle's wait (frameEnded), with the same pacing as above. A frame's drawable then waits
    /// for no frame in flight. direct true presents each frame's own drawable from its command buffer, behind the
    /// frames in flight, as before (LuminousMetalPresentDirect, for a comparison run). A drawable readback always
    /// presents the frame's own drawable.
    void setPresentDirect(bool direct);
    /// MetalDevice::endFrame, after FrameLifecycle::endFrame's wait (when a GPU-bound frame has just finished): on the
    /// finished path, the newest finished frame to the window server if it may take one.
    void frameEnded();

    CAMetalLayer* layer() const { return mLayer; }
    MetalSurfaceStats stats() const;
    /// Tests only: adds count drawables to those with the window server, as if presented with VSync
    /// off just now; no handler takes them back.
    void holdDrawablesForTest(U32 count);
    /// Tests only: the stale limit present() gives metalPresentPaced instead of
    /// METAL_WINDOW_SERVER_STALE_NS, so test 185 does not depend on 100 ms windows on a loaded machine.
    void setStaleLimitForTest(U64 staleNs);
    /// Tests only: as if nextDrawable had given nil agoNs ago, its ask begun then too.
    void refuseDrawableForTest(U64 agoNs);
    /// Tests only: the window counts as hidden (presentWithheld) while hidden is true.
    void setWindowHiddenForTest(bool hidden);
    /// MetalDevice::submit's counts for a drawable it sends.
    void countDrawableWait() { ++mStats.drawableWaits; }
    void countDrawableSignal() { ++mStats.drawableSignals; }
    void countDrawablePresented() { ++mStats.drawablesPresented; }
    /// setMetalDrawableReadback and readMetalDrawable (tests and the golden hook only).
    void setDrawableReadback(bool on);
    bool readDrawable(std::vector<U8>& pixels, U32& width, U32& height);

private:
    /// A back buffer and its depth of max(width, 1) x max(height, 1); the old ones are released through
    /// FrameLifecycle when their Refs drop. False after a WARN line, keeping the old ones.
    bool makeBuffers(U32 width, U32 height);
    /// The drawable's copy in the command buffer of its flip pass, for readDrawable.
    void copyDrawable(IdMTLTexture drawable);
    /// The copy's buffer leaves the residency set once the GPU has finished the frame.
    void dropCopy();
    /// Whether any of the layer's view's window shows (its occlusion state); true for a layer without a view.
    bool windowVisible() const;
    /// For a VSync off present that metalPresentPaced lets ask: true, counted, while the window server has stalled
    /// (stalled: drawables with it and no presented handler within the stale limit) and the window is hidden, except
    /// one ask every METAL_HIDDEN_RETRY_NS; or after a refused drawable (mRefusedNs) until a presented handler has run
    /// since that ask (progress, the window server's progressNs) or METAL_DRAWABLE_RETRY_NS has passed since it gave
    /// up. While the handlers run the occlusion state is not read, so a wrong one costs nothing then.
    /// now: steady_clock nanoseconds.
    bool presentWithheld(U64 now, U64 progress, bool stalled);
    /// LL_METAL_PRESENT_TRACE=1 only: writes the second's line once a second has passed since the last.
    void writePresentTrace(U64 now);
    /// The finished path is in use: VSync off and not setPresentDirect.
    bool finishedPath() const { return !mVSync && !mPresentDirect; }
    /// The layer's framebufferOnly: NO for a drawable readback and on the finished path (a copy's destination).
    void applyFramebufferOnly();
    /// The present queue, its event, allocators and residency sets, once; false after a WARN line, and then the
    /// direct path serves VSync off for the surface's life.
    bool makePresentQueue();
    /// One finished frame: the back buffer flipped into the drawable's layout at the end of a frame's command buffer.
    struct FinishedFrame
    {
        IdMTLTexture texture = nil; ///< BGRA8Unorm, width x height; resident from creation
        U32 width = 0;
        U32 height = 0;
        U64 flip = 0; ///< the flip's order (mFlips); 0: it holds no frame to show
        std::shared_ptr<MetalReadbackStamp> stamp; ///< the submit that sent the flip
        U64 copySerial = 0; ///< the present queue's event value once the copy reading it has run; 0: none pending
        double flipT = 0.0; ///< CACurrentMediaTime at its frame's present(), for the trace
    };
    /// Whether the GPU has finished the slot's flip (its submit's slot event).
    bool finishedOnGpu(const FinishedFrame& slot) const;
    /// Copies that have run leave their slots; finished frames that will never be shown (older than the newest
    /// finished one, or no newer than the last shown) let theirs go.
    void pollFinished();
    /// This frame's flip into a free finished frame, (re)made at the drawable's size, and its submit at once; false
    /// when none is free (the frame is not shown) or after a WARN line.
    bool flipFinished();
    /// The newest finished frame newer than the last shown, copied into the layer's next drawable on the present
    /// queue, with the direct path's pacing (metalPresentPaced, presentWithheld) and counts; true when a drawable was
    /// presented. justFlipped: instead the frame just flipped, before the GPU has finished it, and only while no older
    /// flip is still on the GPU (the copy then waits on the GPU for that frame's own work alone).
    bool presentFinished(U64 now, U64 progress, bool justFlipped = false);
    /// Its texture leaves the residency set through the frame lifecycle, and the slot is empty.
    void retireFinished(FinishedFrame& slot);
    /// Waits for the present queue's copies (METAL_WAIT_MS at most), then retires every finished frame; none of them
    /// is shown after.
    void dropFinished();
    /// The trace's record of one presented drawable.
    struct PresentTraceRecord
    {
        double presentT = 0.0; ///< CACurrentMediaTime at its present
        std::mutex mutex;
        std::shared_ptr<MetalSubmission> submission; ///< whose feedback gives the GPU's end of its frame
    };
    /// The presented handlers of a drawable about to be presented: VSync off's window server count (not vsync), and
    /// the trace's, whose record it gives (null without the trace).
    std::shared_ptr<PresentTraceRecord> watchDrawable(IdCAMetalDrawable drawable, bool vsync);

    MetalDevice& mDevice;
    CAMetalLayer* mLayer = nil;
    Ref<Texture> mColor;
    Ref<Texture> mDepth;
    U32 mWidth = 0;  ///< the surface's size, 0 allowed
    U32 mHeight = 0;
    bool mResizePending = false;
    U32 mPendingWidth = 0;
    U32 mPendingHeight = 0;
    MetalSurfaceStats mStats;
    bool mVSync = true; ///< setVSync's value; on until it is called, as the layer's displaySyncEnabled
    /// The drawables presented with VSync off whose presented handler has not run yet, and when
    /// the window server last took (showed or dropped) one or present() last gave it one, in steady_clock nanoseconds.
    /// The handlers hold this, never the surface (as the device's feedback state).
    struct WindowServerState
    {
        std::atomic<U32> drawables{ 0 };
        std::atomic<U64> progressNs{ 0 };
    };
    std::shared_ptr<WindowServerState> mWindowServer = std::make_shared<WindowServerState>();
    /// LL_METAL_PRESENT_TRACE=1 only (metalPresentTraceKnob): what the presented handlers saw since the last line,
    /// in milliseconds. The handlers hold this, never the surface.
    struct PresentTraceCounts
    {
        U32 handlers = 0;          ///< presented handlers run
        U32 shown = 0;             ///< of them, with a presentedTime (not dropped)
        U32 gpuKnown = 0;          ///< of the shown, whose commit feedback had come
        double toHandlerSum = 0.0; ///< present -> its handler
        double toHandlerMax = 0.0;
        double toGlassSum = 0.0;   ///< present -> presentedTime
        double toGlassMax = 0.0;
        double toGpuSum = 0.0;     ///< present -> its command buffer's GPU end
        double toGpuMax = 0.0;
        double gpuToGlassSum = 0.0; ///< GPU end -> presentedTime
        double gpuToGlassMax = 0.0;
        U32 gaps[4] = {};          ///< between shown ones: about 1, 2, 3, 4+ refreshes
    };
    struct PresentTraceState
    {
        std::mutex mutex;
        PresentTraceCounts counts;
        double lastGlass = 0.0;      ///< the last shown one's presentedTime, seconds
        double refresh = 1.0 / 60.0; ///< the screen's refresh interval, seconds (the main thread sets it)
    };
    std::shared_ptr<PresentTraceState> mTrace;
    /// The main thread's side of the trace's second.
    struct PresentTraceWindow
    {
        U64 beganNs = 0;
        U32 presents = 0;
        U32 drawables = 0;
        U32 held = 0;      ///< metalPresentPaced
        U32 staleAsks = 0; ///< the window server had all but one, and the stale limit let present() ask
        U32 withheld = 0;  ///< presentWithheld
        U32 queued[4] = {}; ///< drawables with the window server at each present(): 0, 1, 2, 3+
        U64 nextDrawableSumNs = 0;
        U64 nextDrawableMaxNs = 0;
    };
    PresentTraceWindow mTraceWindow;
    std::array<FinishedFrame, METAL_FINISHED_FRAMES> mFinished;
    U64 mFlips = 0;     ///< flips into finished frames so far
    U64 mShownFlip = 0; ///< the newest flip given to the window server, or made obsolete by a direct present
    IdMTL4CommandQueue mPresentQueue = nil;
    IdMTLSharedEvent mPresentEvent = nil; ///< signalled after each copy, with mPresentSerial
    U64 mPresentSerial = 0;
    std::array<IdMTL4CommandAllocator, METAL_FINISHED_FRAMES> mPresentAllocators{};
    std::array<U64, METAL_FINISHED_FRAMES> mAllocatorSerial{}; ///< the copy each allocator last recorded
    size_t mNextAllocator = 0;
    bool mPresentQueueFailed = false;
    bool mPresentDirect = false; ///< setPresentDirect's
    /// The finished frames' size: the drawable textures' for the layer's drawableSize layerWidth x layerHeight, which
    /// is the same unless a drawable showed otherwise (presentFinished).
    struct FinishedSize
    {
        U32 width = 0;
        U32 height = 0;
        U32 layerWidth = 0;
        U32 layerHeight = 0;
    };
    FinishedSize mFinishedSize;
    U64 mStaleNs = METAL_WINDOW_SERVER_STALE_NS; ///< setStaleLimitForTest's, else the constant
    U64 mRefusedAskNs = 0; ///< when the ask that gave nil began, steady_clock nanoseconds; 0 when none waits
    U64 mRefusedNs = 0;    ///< when it gave up; 0 when the last ask gave a drawable
    U64 mHiddenAskNs = 0;  ///< while stalled and hidden: when the retry clock began or the last ask went; 0 otherwise
    bool mHiddenForTest = false; ///< setWindowHiddenForTest's
    bool mCopyNext = false; ///< setDrawableReadback(true) asked for the next presented drawable
    IdMTLBuffer mCopyBuffer = nil;
    std::shared_ptr<MetalReadbackStamp> mCopyStamp;
    U32 mCopyWidth = 0;
    U32 mCopyHeight = 0;
};

/// The Metal device: one MTL4CommandQueue, and per frame slot one command allocator
/// and one shared event.
class API_AVAILABLE(macos(26.0)) MetalDevice final : public Device
{
public:
    /// What the commit feedback handlers report. A handler holds only this and its own
    /// submission, never the device.
    struct FeedbackState
    {
        std::mutex mutex;
        std::condition_variable changed;
        U64 outstanding = 0; ///< committed and not yet reported
        U64 received = 0;
        U64 gpuErrors = 0;
        U64 consecutiveTimeouts = 0; ///< command buffers in a row whose feedback was the watchdog's Timeout
        MetalGpuBusy gpu; ///< the command buffers without an error
        bool lost = false; ///< an error that loses the device arrived (metalErrorLosesDevice)
        /// The pass timer's frames (MetalPassTimes::timed), first to last, 0 for none: their command buffers' busy
        /// time without an error, and the command buffers reported, with an error too.
        U64 timedFirst = 0;
        U64 timedLast = 0;
        MetalGpuBusy timedGpu;
        U64 timedReceived = 0;
    };

    using Submission = MetalSubmission;

    MetalDevice(IdMTLDevice device, IdMTL4CommandQueue queue, dispatch_queue_t feedbackQueue,
                const MetalSupport& support);
    ~MetalDevice() override;

    BackendType backend() const override { return BackendType::Metal; }
    const Capabilities& capabilities() const override { return mCaps; }
    /// copyRefusal(mCaps, from, to), then for different formats formatCopyUnavailable(to).
    std::string copyRefusalBetween(Format from, Format to) override;

    Ref<Buffer> createBuffer(const BufferDesc& desc, std::span<const std::byte> initial,
                             std::string_view debugName) override;
    Ref<Texture> createTexture(const TextureDesc& desc, std::string_view debugName) override;
    Ref<Sampler> getSampler(const SamplerDesc& desc) override;
    /// The samplers getSampler has made and cached (reuse is counted by golden level 1 and the tests).
    size_t samplerCount() const { return mSamplers.size(); }
    Ref<Program> createProgram(const ProgramDesc& desc) override;
    std::vector<Ref<Query>> createQueries(QueryType type, U32 count) override;
    Ref<Pipeline> getPipeline(Program& program, const PipelineDesc& desc) override;
    Ref<DepthStencilState> getDepthStencilState(const DepthStencilDesc& desc) override;

    /// Native programs, in llrhimetalnative-objc.mm: libraries compiled from source with MTL4Compiler in the
    /// background, and their compute and render pipelines in caches of their own (their keys name the library, so
    /// they never meet the translated programs' ids in PipelineCache).
    Ref<NativeLibrary> createNativeLibrary(const NativeSource& source) override;
    Ref<ComputePipeline> getComputePipeline(NativeLibrary& library, const ComputePipelineDesc& desc) override;
    Ref<Pipeline> getNativePipeline(NativeLibrary& library, const NativeRenderFunctions& functions,
                                    const PipelineDesc& desc) override;
    /// ~MetalNativeLibrary: the library's pipelines leave the caches (the tracker or a facade may still hold them).
    void forgetNativeLibrary(U64 libraryId);
    /// Tests: the native pipelines cached, compute and render.
    size_t nativePipelineCount() const;
    /// MetalFX's spatial scaler (createMetalUpscaler), made with the device's compiler.
    Ref<Upscaler> createUpscaler(const UpscalerDesc& desc) override;
    /// MetalFX's temporal scaler (createMetalTemporalUpscaler), made with the device's compiler.
    Ref<TemporalUpscaler> createTemporalUpscaler(const TemporalUpscalerDesc& desc) override;

    Surface* surface() override;
    Encoder& encoder() override { return mEncoder; }
    UploadContext& uploadContext() override { return mUploads; }

    void beginFrame() override {}
    void endFrame() override;
    U64 frameNumber() const override { return mLifecycle.currentFrame(); }
    U64 completedFrame() const override { return mLifecycle.completedFrame(); }
    void submitAndWait() override;

    /// The pass timer (MetalPassTimes), on the timestamp heap's tick rate; false after a WARN line without one.
    bool startPassTimes(U32 frames, const std::string& path, std::function<void()> done) override;
    void stopPassTimes() override;
    MetalPassTimes& passTimes() { return mPassTimes; }

    // Sample queries on the visibility result buffer -------------------------------------------------------

    /// The visibility result buffer every render pass gets once it exists (MTL4RenderPass.h:96-100), made at the first
    /// createQueries of a sample kind; nil before, or when Metal gave none.
    IdMTLBuffer visibilityBuffer() const { return mVisibility; }
    /// A zeroed slot of the ring for run's begin, noted with the frame and run: -1 without the buffer, or when the next
    /// slot is still in a frame that has not finished (the ring is full: LL_ERRS under the Metal debug setting,
    /// otherwise a WARN line now and then). A reused slot's final value goes to its earlier run first, when someone can
    /// still read that run.
    S64 takeVisibilitySlot(const std::shared_ptr<MetalQueryRun>& run);
    /// A query's end: the next submit stamps it, as a readback's copy.
    void stampNextSubmit(std::shared_ptr<MetalReadbackStamp> stamp) { mStamps.push_back(std::move(stamp)); }
    /// What the sample queries did: begins, begins without a slot, results read, of them 0, reads that waited, and
    /// slot values kept for a late read. The time queries' likewise, with the reads that gave 0 ns
    /// because no valid pair holds the run's stamps (timerInvalid: never begun, refused, abandoned, without a pair, a
    /// failed command buffer, or t1 < t0), and the most sample and time begins in one frame, which
    /// the device logs in one INFO line when it ends.
    struct QueryStats
    {
        U64 begun = 0;
        U64 noSlot = 0;
        U64 read = 0;
        U64 zero = 0;
        U64 waited = 0;
        U64 kept = 0;
        U64 timeBegun = 0;
        U64 timeNoPair = 0;
        U64 timeRead = 0;
        U64 timeZero = 0;
        U64 timeWaited = 0;
        U64 timeKept = 0;
        U64 timerInvalid = 0;
        U64 frameSampleBegins = 0; ///< this frame's sample begins, with or without a slot
        U64 frameTimeBegins = 0;   ///< this frame's time begins, with or without a pair
        U64 peakSampleBegins = 0;
        U64 peakTimeBegins = 0;
    };
    QueryStats& queryStats() { return mQueryStats; }

    // Time queries on the timestamp heap ------------------------------------------------------------

    /// The Timestamp heap of METAL_TIMER_PAIRS pairs, made with the device, and the GPU's ticks per
    /// second (MTLDevice.h:1537-1541); nil and 0 when Metal gave none, and then timerQueries is false.
    IdMTL4CounterHeap timestampHeap() const { return mTimestamps; }
    U64 timestampFrequency() const { return mTimestampFrequency; }
    /// A pair of the ring for run's begin, noted with the frame and run: -1 without the heap, or when the next pair's
    /// earlier run is in a frame that has not finished and the submit after its end has not been signalled (the ring
    /// is full: a WARN line now and then, never LL_ERRS, as a timer only measures). A reused pair's ticks go to its
    /// earlier run first, when someone can still read that run.
    S64 takeTimerPair(const std::shared_ptr<MetalTimerRun>& run);
    /// The stamp at index on the command buffer being recorded, between encoders (MTL4CommandBuffer.h:162-174): the
    /// copies' encoder ends first. False without the heap.
    bool writeTimestamp(U32 index);
    /// The pair's two ticks, resolved on the CPU (MTL4Counters.h, resolveCounterRange:); the GPU has finished writing
    /// them (the caller waited for the submit's signal, MTL4Counters.h's note). False when Metal gave no data.
    bool resolveTimestamps(S64 pair, std::array<U64, 2>& ticks) const;

    // The backend's own -----------------------------------------------------------------------

    FrameLifecycle& lifecycle() { return mLifecycle; }
    /// The submission being recorded, begun on the frame's slot when first asked for.
    IdMTL4CommandBuffer commandBuffer();
    /// Sends what is recorded, also nothing, and stamps the readbacks whose copies it carries.
    void submit();
    /// wait()'s submit: the open segment ends, the copies waiting on it are encoded, and what
    /// is recorded is sent; the pass goes on in a new segment.
    void submitPending();
    /// LL_METAL_SPLIT_PASSES=1, for finding a pass that runs too long: what is recorded is sent before each encoder
    /// begins, and the next command buffer's label names the encoder, so a GPU error's line names the pass.
    void splitBeforeEncoder(NSString* name);
    /// Before MetalFX records its encoders: what is recorded is sent, so the scaler's work begins a command buffer
    /// of its own (nothing when nothing is recorded).
    void sendBeforeScaler();
    /// Encodes a readback's copy now, on the command buffer's open compute encoder, which begins
    /// with the queue barrier.
    void encodeCopy(const MetalCopy& copy);
    /// A buffer readback's copy from the source's GPU memory to its Shared buffer, on the same
    /// encoder of copies as encodeCopy's, so it follows the GPU copies of earlier static updates (the queue
    /// barrier) and is stamped by the submit that carries it.
    void encodeBufferCopy(IdMTLBuffer source, U64 offset, IdMTLBuffer destination, U64 size,
                          std::shared_ptr<MetalReadbackStamp> stamp);
    /// Ends the open compute encoder of copies; called before any other native command.
    void endCopies();
    /// The queue barrier at the start of each native render and compute encoder: the encoder's
    /// work waits for the queue's earlier work and sees what it wrote (MTL4CommandEncoder.h:70-72).
    /// The one place that issues it, so barriers() counts them.
    void queueBarrier(IdMTL4CommandEncoder encoder);
    /// A render pass's barrier at its encoder's begin (MetalEncoder::open). With split barriers
    /// (LL_METAL_BARRIER) only metalPassWaitingStages() wait for the queue's earlier work, so the pass's vertex work
    /// overlaps the work before it: a render pass writes only its attachments (no shader stores a buffer), every copy
    /// ends with a producer barrier before all later stages (endCopies), and a vertex stage that samples gets the
    /// rest of the barrier before its first draw (MetalEncoder::setNativePipeline). Otherwise queueBarrier. True when
    /// split. Counted in barriers().
    bool passBarrier(IdMTL4RenderCommandEncoder encoder);
    /// The geometry stages' part of a split barrier, before a draw whose vertex stage samples a texture
    /// an earlier pass may have written; once per encoder (the caller keeps track). Counted in vertexBarriers().
    void vertexBarrier(IdMTL4RenderCommandEncoder encoder);
    const MetalSpeedKnobs& speedKnobs() const { return mSpeed; }
    /// Tests: other knobs than the environment's for what is recorded from now on.
    void setSpeedKnobsForTest(const MetalSpeedKnobs& knobs) { mSpeed = knobs; }
    /// A static index buffer's CPU copy came (bytes > 0) or went (bytes < 0).
    void noteIndexCopy(S64 bytes);
    /// Ends the open compute encoder of copies and begins another, with the queue barrier,
    /// for an upload's copy; the caller ends it with endCopies() right after. Nil after a WARN line.
    /// The label names the encoder (the upscale's own encoder before MetalFX's work is one).
    IdMTL4ComputeCommandEncoder beginUploadCopies(NSString* label = @"Upload");
    /// Size bytes for this frame's commands in the frame slot's ring. Main thread. Nil buffer
    /// after a WARN line.
    MetalAllocation frameAllocation(U64 size);
    /// A buffer's memory: a pool block for a pooled buffer (metalPooled), otherwise a Shared
    /// buffer of its own, resident from creation and labelled with the Metal debug setting. Nil
    /// buffer after a WARN line.
    MetalAllocation newBufferMemory(const BufferDesc& desc, std::string_view debugName);
    /// Through FrameLifecycle::release, once the GPU has finished this frame, a pool block goes
    /// back to the pool and a buffer of its own leaves the residency set.
    void releaseBufferMemory(const BufferDesc& desc, const MetalAllocation& memory);
    /// A Shared buffer, resident from creation, labelled with the Metal debug setting; nil after a
    /// WARN line. The caller retires it.
    IdMTLBuffer newResidentBuffer(U64 length, std::string_view debugName);
    /// The Metal debug setting when the device was made: textures and buffers get labels.
    bool debugLabels() const { return mDebugLabels; }
    /// The Metal debug setting when the device was made: bindForDraw checks residency, vertex
    /// ranges and bindings.
    bool debugChecks() const { return mDebugChecks; }
    /// The one residency set.
    MetalResidency& residency() { return mResidency; }
    /// The texture a draw samples on a channel with nothing bound, of the sampler's type, which
    /// reads (0, 0, 0, 1) (llrhi.h:1004-1009); for a shadow sampler the never-written Depth32Float, which
    /// bindForDraw samples with compare function Never. Made at the first ask, kept until the device
    /// goes. Null after a WARN line.
    Texture* defaultTexture(UniformType type);
    /// Waits until the submission's feedback handler has run, at most 5000 ms; true when it
    /// ran and reported no error.
    bool waitForFeedback(const std::shared_ptr<Submission>& submission);
    /// A Shared buffer for a readback, resident from creation; nil after a WARN line.
    IdMTLBuffer newReadbackBuffer(U64 length);
    /// FrameLifecycle's wait: the frame's slot event reaches the frame's last serial, or the wait limit (5000 ms)
    /// passes; FrameLifecycle waits twice before the device is lost.
    bool waitForFrame(U64 frame);
    /// An error that loses the device came in a commit feedback, or a frame did not finish within two waits.
    bool deviceLost() const override;
    /// The commit feedback's GPU errors so far, without draining (gpuErrors() drains first).
    U64 gpuErrorCount() const override;
    /// Waits until every committed submission's feedback handler has run (5000 ms, then a WARN line).
    void drainFeedback();
    /// The Metal state for a binding; null binds DepthStencilDesc(), which is looked up once and then kept, so
    /// a draw with none bound takes no cache lock (every draw compares its state). Main thread.
    IdMTLDepthStencilState depthState(DepthStencilState* state);
    /// Debug groups: pushed on the open native render encoder while one is open, else on the
    /// command buffer, and popped from the one that holds them.
    void pushGroup(std::string_view name);
    void popGroups(U32 count);
    /// MetalEncoder::open() before a render encoder begins and endNative() before it ends: the open debug
    /// groups are popped from what holds them, so none crosses an encoder's begin or end.
    void endGroups();
    /// Then pushed again on the new encoder, or on the command buffer when encoder is nil (after
    /// endEncoding, or when Metal gave no encoder), which then holds them.
    void beginGroups(IdMTL4CommandEncoder encoder);
    /// Tests: the most debug groups the command buffer held while a native render encoder was open.
    U32 groupsOnCommandBufferWhileEncoding() const { return mBufferGroupsWhileEncoding; }
    /// Tests: a command buffer is being recorded (made and not yet sent).
    bool hasCommandBuffer() const { return mCommandBuffer != nil; }
    /// Notes the command buffer's groups while the encoder's native encoder is open (the hook above), or
    /// while one of the encoder's own render encoders is (encoding: the mips' and the alpha after a copy).
    void noteGroupsWhileEncoding(bool encoding = false);
    /// The device's MSL compiler, made at the first createProgram, which also writes the INFO line
    /// with the compiler's math once. Main thread, before the first job; the jobs send to it from
    /// the pool's threads (MTL4Compiler is NS_SWIFT_SENDABLE, MTL4Compiler.h:89).
    IdMTL4Compiler compiler();
    /// The pipelines of the device, which a program forgets when it dies.
    PipelineCache& pipelines() { return mPipelines; }
    /// The program of the clear draws for a pass with these formats, made through
    /// createProgram at the first ask, so the registered translator makes its MSL (GLSL is the only
    /// shader source), and waited for. One output per color attachment, uvec4 for an integer
    /// format, and gl_FragDepth with a depth attachment. Null when it failed: createProgram wrote the
    /// WARN line, and a failed program is made again at the next ask.
    Program* clearProgram(const PassFormats& formats);
    /// The device's own pipeline of a clear program, cached apart from the viewer's; null
    /// after a WARN line.
    Ref<Pipeline> clearPipeline(Program& program, const PipelineDesc& desc);
    /// Tests: the clear programs made so far, by key.
    size_t clearPrograms() const { return mClearPrograms.size(); }
    /// The mip program (one for every format), made through createProgram at the first ask as
    /// clearProgram, so the registered translator makes its MSL, and waited for. Null when it failed:
    /// createProgram wrote the WARN line, and a failed program is made again at the next ask.
    Program* mipProgram();
    /// The device's own pipeline of a mip program, cached apart from the viewer's; null after a
    /// WARN line.
    Ref<Pipeline> mipPipeline(Program& program, const PipelineDesc& desc);
    /// Tests: the mip programs made so far: 0 or 1.
    size_t mipPrograms() const { return mMipProgram.notNull() ? 1 : 0; }
    /// The present program (metalPresentProgramDesc), made through createProgram at the first ask as
    /// mipProgram, so the registered translator makes its MSL, and waited for. Null when it failed: createProgram
    /// wrote the WARN line, and a failed program is made again at the next ask.
    Program* presentProgram();
    /// The device's own pipeline of the present program, cached apart from the viewer's; null after a WARN line.
    Ref<Pipeline> presentPipeline(Program& program, const PipelineDesc& desc);
    /// The format copy program (metalFormatCopyProgramDesc), made through createProgram at the first ask as
    /// presentProgram, and waited for. Null when it failed: createProgram wrote the WARN line, and a failed program
    /// is made again at the next ask.
    Program* formatCopyProgram();
    /// The device's own pipeline of the format copy program for a destination's format, cached apart from the
    /// viewer's; null after a WARN line.
    Ref<Pipeline> formatCopyPipeline(Program& program, const PipelineDesc& desc);
    /// The pipeline description of the format copy into an attachment of format to (one colour attachment,
    /// no attributes), and empty when its program and its pipeline state are had (made and waited for at the first ask,
    /// then cached), otherwise the reason, after the WARN line createProgram or the pipeline wrote.
    static PipelineDesc formatCopyPipelineDesc(Format to);
    std::string formatCopyUnavailable(Format to);
    /// The surface on the layer, its residency set on the queue; false after a WARN line. Once, at creation.
    bool createSurface(CAMetalLayer* layer, U32 width, U32 height);
    /// The surface; null for a device made without a window.
    MetalSurface* metalSurface() { return mSurface.get(); }
    /// The drawable present() has drawn its flip pass into. The next submit, whichever sends the command buffer
    /// that holds the flip pass, waits for it on the queue before its commit, signals it after the commit and
    /// presents it (MTL4CommandQueue.h:277-308, MTLDrawable.h:29). Main thread.
    void setPendingDrawable(IdCAMetalDrawable drawable);
    /// The next submit waits on the queue until the upload queue's event reaches value, before its
    /// commit (MTL4CommandQueue.h:269-275), so the command buffer being recorded runs after the uploads. Main thread.
    void waitForUploads(IdMTLSharedEvent event, U64 value);
    /// The upload context's own (its counters and the tests' gate).
    MetalUploadContext& metalUploads() { return mUploads; }
    /// The list a non-indexed fan, strip or loop of count vertices draws, cached by (type,
    /// count); at most MAX_INDEX_LISTS, the least recently used leaves through FrameLifecycle::release.
    /// A nil buffer for a draw that draws nothing and after a WARN line.
    MetalIndexList generatedIndices(PrimitiveType type, U32 count);
    static constexpr size_t MAX_INDEX_LISTS = 64;
    /// Tests: the cached lists.
    size_t indexLists() const { return mIndexLists.size(); }
    /// The pipeline states the cache's pipelines share.
    MetalPipelineStates& pipelineStates() { return mPipelineStates; }
    /// The translation threads, started at the first createProgram.
    MetalProgramPool& programPool() { return mProgramPool; }
    /// The library handlers' state, which each job holds.
    std::shared_ptr<MetalCompileState> compileState() const { return mCompileState; }
    /// The groups the ordinary programs are compiled in.
    MetalProgramGroups& programGroups() { return mProgramGroups; }
    /// The test faults the next program takes with it (failNextCompile, sleepNextHandlerMs).
    bool takeCompileFault() { return std::exchange(mFailNextCompile, false); }
    U32 takeHandlerSleep() { return std::exchange(mSleepNextHandlerMs, 0u); }
    /// Counts a translation on the calling thread: the login programs and wait()'s stolen jobs.
    void countCallerTranslation() { ++mTranslatedOnCaller; }
    /// LL_METAL_COMPILE_SLOW (metalCompileSlowKnob), which turns the QoS and the stealing wait off.
    bool compileSlow() const { return mCompileSlow; }
    /// The knob for this device, before its first program (the constructor, and the tests): the QoS of
    /// the translation threads, and the stealing wait in MetalProgram::wait.
    void setCompileSlow(bool slow)
    {
        mCompileSlow = slow;
        mProgramPool.setQos(slow ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED);
    }

    // Tests -----------------------------------------------------------------------------------

    IdMTLDevice mtlDevice() const { return mDevice; }
    MetalEncoder& metalEncoder() { return mEncoder; }
    /// The next commit waits on the queue until the gate reaches value. The caller keeps the gate until then: Metal 4's
    /// queue does not retain it, and a released gate ends the wait.
    void gateNextCommit(IdMTLSharedEvent gate, U64 value);
    /// The next submission's feedback handler reports it failed, as a GPU error would, but
    /// counts no GPU error.
    void failNextFeedback() { mFailNextFeedback = true; }
    /// The next submission's feedback handler gets this error when Metal reports none, as a real error
    /// from Metal: logged, counted, failed, and the device lost when metalErrorLosesDevice says so or on a second
    /// watchdog Timeout in a row (metalErrorIsTimeout).
    void injectNextFeedbackError(NSError* error) { mInjectedError = error; }
    /// The time limit of submitAndWait's, drainFeedback's and waitForFrame's waits; METAL_WAIT_MS unless a test
    /// shortens it.
    void limitWaits(U32 ms) { mWaitLimitMs = ms; }
    /// The visibility ring's slots, before its buffer is made (METAL_VISIBILITY_SLOTS otherwise).
    void limitVisibilitySlots(U32 slots)
    {
        mVisibilitySlots = mVisibility ? mVisibilitySlots : std::clamp(slots, 1u, METAL_VISIBILITY_SLOTS);
    }
    /// The timer ring's pairs, before the first time query's begin (tests); the heap keeps its size.
    void limitTimerPairs(U32 pairs)
    {
        mTimerPairs = std::clamp(pairs, 1u, METAL_TIMER_PAIRS);
        mTimerUses.assign(mTimestampFrequency > 0 ? mTimerPairs : 0, TimerUse());
        mTimerNext = 0;
    }
    U64 submissions() const { return mSubmissions; }
    /// The ring chunk of the frame being recorded; nil past its last.
    IdMTLBuffer ringChunk(size_t index) const { return mRings[mLifecycle.currentFrame() % mRings.size()].chunk(index); }
    /// Tests: the large staging buffers all slots keep, the most bytes one slot keeps, and the buffers they
    /// have made.
    size_t ringKeptBuffers() const
    {
        size_t kept = 0;
        for (const MetalRing& ring : mRings)
        {
            kept += ring.keptBuffers();
        }
        return kept;
    }
    U64 ringKeptBytesMax() const
    {
        U64 most = 0;
        for (const MetalRing& ring : mRings)
        {
            if (ring.keptBytes() > most)
            {
                most = ring.keptBytes();
            }
        }
        return most;
    }
    U64 ringKeptMade() const
    {
        U64 made = 0;
        for (const MetalRing& ring : mRings)
        {
            made += ring.keptMade();
        }
        return made;
    }
    size_t poolChunks() const { return mPool.chunks(); }
    /// The compute encoders of copies begun so far, for readbacks and uploads.
    U64 copyEncodersBegun() const { return mCopyEncodersBegun; }
    /// The queue barriers queueBarrier() and passBarrier() have issued so far: one per native encoder.
    U64 barriers() const { return mBarriers; }
    /// Of barriers(), the split ones; the producer barriers endCopies issued with split barriers; the
    /// geometry stages' barriers before a sampling vertex stage (vertexBarrier); the render passes begun with
    /// passBarrier right after the copies' encoder and right after another render encoder (information: only the
    /// second kind can overlap).
    U64 splitBarriers() const { return mSplitBarriers; }
    U64 producerBarriers() const { return mProducerBarriers; }
    U64 vertexBarriers() const { return mVertexBarriers; }
    U64 passesAfterCopies() const { return mPassesAfterCopies; }
    U64 passesAfterPasses() const { return mPassesAfterPasses; }
    /// The bytes of the static index buffers' CPU copies now, and the most at once.
    U64 indexCopyBytes() const { return mIndexCopyBytes.load(); }
    U64 indexCopyPeak() const { return mIndexCopyPeak.load(); }
    /// The GPU's busy time and command buffers so far, after the handlers sent so far have run.
    MetalGpuBusy gpuBusy();
    /// Tests: the GPU time's window (10 s; zero gives a line at every endFrame).
    void setGpuTimeWindowForTest(std::chrono::steady_clock::duration window) { mGpuWindow = window; }
    /// The feedback handlers that have run, and the GPU errors they reported; both drain first.
    U64 feedbackReceived();
    U64 gpuErrors();
    std::shared_ptr<FeedbackState> feedbackState() const { return mFeedback; }
    std::shared_ptr<Submission> lastSubmission() const { return mLastSubmission; }
    /// The pool's threads take no job while paused; wait() still takes its own.
    void pausePrograms(bool paused) { mProgramPool.pause(paused); }
    /// The next program's vertex library gets MSL that does not compile, so Metal's handler fails it;
    /// with no program before it, the next new pipeline state's handler fails it.
    void failNextCompile() { mFailNextCompile = true; }
    /// The next program's first library handler, or the next new pipeline
    /// state's handler, sleeps this long before it reports.
    void sleepNextHandlerMs(U32 ms) { mSleepNextHandlerMs = ms; }
    /// The full pipeline states, and the refused keys.
    size_t fullPipelines() const { return mPipelineStates.full(); }
    size_t refusedPipelines() const { return mPipelineStates.refused(); }
    /// The attachments the last full pipeline state was sent with blending Disabled, one bit each.
    U32 lastFullDisabledAttachments() const { return mPipelineStates.lastFullDisabled(); }
    /// The translations on the calling thread so far.
    U64 translatedOnCaller() const { return mTranslatedOnCaller.load(); }
    /// The point variants translated so far.
    U64 pointTranslations() const
    {
        std::lock_guard<std::mutex> lock(mCompileState->mutex);
        return mCompileState->pointTranslations;
    }
    /// The groups sent so far, as digests of their source, by the groups' order.
    std::vector<MetalGroupRecord> groupSources() const { return mProgramGroups.records(); }
    /// The groups whose library failed, so their members were sent alone.
    U64 groupFallBacks() const
    {
        std::lock_guard<std::mutex> lock(mCompileState->mutex);
        return mCompileState->groupFallBacks;
    }

private:
    struct Slot
    {
        IdMTL4CommandAllocator allocator = nil;
        IdMTLSharedEvent event = nil;
        U64 frame = 0;      ///< the frame using the slot; 0 before the first
        U64 lastSerial = 0; ///< the serial of the frame's last signal
        U32 parts = 0;      ///< the frame's command buffers so far
    };

    Slot& slotOf(U64 frame) { return mSlots[frame % mSlots.size()]; }
    bool reached(const Slot& slot) const;
    /// Reports the frames whose last serial their slot event has reached, in order.
    void pollFrames();
    /// The class lines MetalCompileState holds, one INFO line each, on the main thread; a line already written
    /// in this session is not written again.
    void writeClassLines();
    /// The hashes of the class lines written: every change of graphics level rebuilt about a thousand
    /// programs with the lines they had before, so a long session's log grew by gigabytes of them.
    std::unordered_set<size_t> mWrittenClassLines;
public:
    /// The failure lines for the main thread (MetalFailureLog), and their writing there.
    MetalFailureLog& failureLog() { return *mCompileState->failures; }
    /// The same log for a feedback handler, which may outlive the device
    std::shared_ptr<MetalFailureLog> failureLogShared() { return mCompileState->failures; }
    void writeFailureLines(bool final = false) { mCompileState->failures->write(final); }
private:
    /// The open compute encoder of copies, begun with the queue barrier when none is open.
    IdMTL4ComputeCommandEncoder openCopies(NSString* label);
    /// At a frame's first command buffer, before it is made: a capture setMetalFrameCapture asked for
    /// begins, of the whole device, to a GPU trace document; one INFO line, or one WARN line when it cannot.
    void beginCapture();
    /// The capture this device began ends, and one INFO line says Metal wrote the document, or one WARN line
    /// that Metal left none at its path.
    void endCapture();
    /// At a frame's end, after its submit: the frame's capture ends, and a capture asked for after the current
    /// frame is one for the next frame.
    void captureAtFrameEnd();

    IdMTLDevice mDevice;
    IdMTL4CommandQueue mQueue;
    [[maybe_unused]] dispatch_queue_t mFeedbackQueue; ///< the queue descriptor only assigns it, so the device keeps it
    MetalResidency mResidency;
    std::shared_ptr<FeedbackState> mFeedback;
    std::shared_ptr<Submission> mLastSubmission;
    std::array<Slot, FrameLifecycle::MAX_FRAMES_IN_FLIGHT> mSlots;
    U64 mSerial = 0; ///< one monotonic serial for every signal
    /// A ring slot's last begin: its frame (0: none) and its run, whose final value the slot's reuse keeps
    struct VisibilityUse
    {
        U64 frame = 0;
        std::weak_ptr<MetalQueryRun> run;
    };
    IdMTLBuffer mVisibility = nil;              ///< the visibility result buffer (takeVisibilitySlot)
    std::vector<VisibilityUse> mVisibilityUses; ///< by slot
    U32 mVisibilitySlots = METAL_VISIBILITY_SLOTS;
    U32 mVisibilityNext = 0; ///< the ring's next slot
    /// A timer pair's last begin: its frame (0: none), its run, whose ticks the pair's reuse keeps, and
    /// its end's stamp, held here because a query's next begin drops the run: the stamp's serial is
    /// set only for an end that was written, and its signal frees the pair within the frame
    struct TimerUse
    {
        U64 frame = 0;
        std::weak_ptr<MetalTimerRun> run;
        std::shared_ptr<MetalReadbackStamp> endStamp;
    };
    std::vector<TimerUse> mTimerUses; ///< by pair
    U32 mTimerPairs = METAL_TIMER_PAIRS;
    U32 mTimerNext = 0; ///< the ring's next pair
    QueryStats mQueryStats;
    U64 mSubmissions = 0;
    U64 mCopyEncodersBegun = 0;
    U64 mBarriers = 0;
    U64 mSplitBarriers = 0;
    U64 mProducerBarriers = 0;
    U64 mVertexBarriers = 0;
    U64 mPassesAfterCopies = 0;
    U64 mPassesAfterPasses = 0;
    bool mCopiesSincePass = false; ///< the copies' encoder was begun since the last passBarrier
    MetalSpeedKnobs mSpeed;        ///< metalSpeedKnobs() when the device was made
    std::atomic<U64> mIndexCopyBytes{0}; ///< buffers are made and released on more than one thread
    std::atomic<U64> mIndexCopyPeak{0};
    /// One line of the GPU's busy time per frame: the window since the last line (about every 10 s of
    /// frames, from endFrame) or, total, the device's life (~MetalDevice, after drainFeedback).
    void logGpuTime(bool total);
    /// The GPU time's window and the frames so far
    U64 mGpuFrames = 0;
    U64 mGpuWindowFrames = 0;
    MetalGpuBusy mGpuWindowStart;
    std::chrono::steady_clock::time_point mGpuWindowBegan = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration mGpuWindow = std::chrono::seconds(10);
    /// At the frame's end, while the pass timer runs: its frame end, the commit feedback's window of its frames, and
    /// its table once their command buffers have reported (or 60 frame ends after the last was resolved).
    void passTimesAtFrameEnd();
    MetalPassTimes mPassTimes;
    MetalPassStamp mCopiesStamp; ///< the pass timer's stamp of the open encoder of copies
    IdMTL4CommandBuffer mCommandBuffer = nil;
    IdMTL4ComputeCommandEncoder mCopies = nil; ///< the open compute encoder of readback copies
    std::vector<std::shared_ptr<MetalReadbackStamp>> mStamps; ///< the readbacks of the command buffer
    std::vector<std::string> mGroups; ///< open debug groups, pushed again on each command buffer
    /// The open render encoder that holds mGroups; nil: the command buffer
    IdMTL4CommandEncoder mGroupEncoder = nil;
    U32 mBufferGroups = 0;              ///< the groups pushed on mCommandBuffer and not popped
    U32 mBufferGroupsWhileEncoding = 0; ///< tests: the most of them while a native render encoder was open
    bool mCapturing = false;            ///< this device began a frame capture, stopped after the frame's submit
    std::string mCapturePath;           ///< the document of the capture this device began
    U64 mCaptureFrame = 0;              ///< the frame of that capture
    IdMTLSharedEvent mGate = nil;
    U64 mGateValue = 0;
    bool mFailNextFeedback = false;
    NSError* mInjectedError = nil; ///< the next submission's feedback error (injectNextFeedbackError)
    /// Test runs only: LL_METAL_TEST_FAULT's access-revoked@N or frame-hang@N, read when the device was made
    /// with the debug setting (metalTestFault); the frame, 0 for none, and the fault, applied once
    U64 mTestFaultFrame = 0;
    bool mTestFaultHang = false;
    IdMTLSharedEvent mTestHangGate = nil; ///< frame-hang's gate, never signalled before ~MetalDevice
    U32 mWaitLimitMs;
    bool mDebugLabels = false; ///< metalDebug() when the device was made
    bool mDebugChecks = false; ///< metalDebug() when the device was made
    FrameLifecycle mLifecycle;
    // After mLifecycle, so their objects are gone before it; ~MetalDevice clears them first.
    std::array<MetalRing, FrameLifecycle::MAX_FRAMES_IN_FLIGHT> mRings; ///< by frame slot
    MetalBufferPool mPool;
    DescCache<SamplerDesc, Sampler, SamplerDescHash> mSamplers;
    std::array<Ref<Texture>, 5> mDefaultTextures; ///< 2D, cube, cube array, shadow, 3D
    PipelineCache mPipelines;
    MetalPipelineStates mPipelineStates; ///< cleared after mPipelines in ~MetalDevice
    /// The native pipelines by library and description (a key string that begins with the library's id); cleared
    /// with mPipelines in ~MetalDevice.
    mutable std::mutex mNativeMutex;
    std::unordered_map<std::string, Ref<ComputePipeline>> mComputePipelines;
    std::unordered_map<std::string, Ref<Pipeline>> mNativePipelines;
    /// The clear programs by key (metalClearKey) and their pipelines; cleared with mPipelines in
    /// ~MetalDevice, before mLifecycle.releaseAll().
    std::unordered_map<U32, Ref<Program>> mClearPrograms;
    PipelineCache mClearPipelines;
    /// The mip program (one for every format) and its pipelines; cleared with the clear programs.
    Ref<Program> mMipProgram;
    PipelineCache mMipPipelines;
    /// The present program and its pipelines; cleared with the mip programs.
    Ref<Program> mPresentProgram;
    PipelineCache mPresentPipelines;
    /// The format copy program and its pipelines; cleared with the present program.
    Ref<Program> mFormatCopyProgram;
    PipelineCache mFormatCopyPipelines;
    /// The window's surface; null without a window. Released in ~MetalDevice after the slot waits and before
    /// mLifecycle.releaseAll().
    Ref<MetalSurface> mSurface;
    /// Drawables with a recorded flip pass, waited for, signalled and presented by the next submit; dropped
    /// unpresented when the device ends with them.
    std::vector<IdCAMetalDrawable> mPendingDrawables;
    /// The upload queue's event and the highest value waitForFence asked for since the last submit,
    /// which the next submit waits for on the queue before its commit.
    IdMTLSharedEvent mUploadWait = nil;
    U64 mUploadWaitValue = 0;
    /// The cached lists by (type << 32 | count), each with its last use.
    struct CachedIndexList
    {
        U64 key = 0;
        U64 used = 0;
        MetalIndexList list;
    };
    std::vector<CachedIndexList> mIndexLists;
    U64 mIndexListUses = 0;
    IdMTL4Compiler mCompiler = nil; ///< made at the first createProgram
    std::shared_ptr<MetalCompileState> mCompileState;
    MetalProgramGroups mProgramGroups; ///< cancelled first in ~MetalDevice
    MetalProgramPool mProgramPool;     ///< stopped next in ~MetalDevice
    bool mFailNextCompile = false;
    bool mCompileSlow = false; ///< LL_METAL_COMPILE_SLOW
    U32 mSleepNextHandlerMs = 0;
    std::atomic<U64> mTranslatedOnCaller{ 0 };
    // Before mCaps, which says timerQueries when both are there.
    IdMTL4CounterHeap mTimestamps = nil;
    U64 mTimestampFrequency = 0;
    Capabilities mCaps;
    DescCache<DepthStencilDesc, DepthStencilState, DepthStencilDescHash> mDepthStencils;
    Ref<DepthStencilState> mDefaultDepth; ///< depthState(nullptr)'s, from mDepthStencils; let go with it
    MetalEncoder mEncoder;
    MetalUploadContext mUploads;
};

/// An occlusion query, AnySamplesPassed or SamplesPassed (llrhi.h's Query contract). Each begin makes a
/// MetalQueryRun on a new slot of the device's visibility result buffer, so a begin forgets the last result and a
/// facade that is never released holds no slot. ready() never waits: true once the submit after the end was signalled
/// and reported. result(true) sends what is recorded and waits, as Readback::wait. A query with no result (never
/// begun, refused, abandoned at the frame's end, without a slot, no draw reached a pass with the buffer, or its command
/// buffer failed) reads 1, so a missing result never hides an object; GL reads 0 for a query with no draw, so Metal
/// differs only in the safe direction. AnySamplesPassed reads 0 or 1 (llrhi.h), whatever non-zero Metal's Boolean mode
/// wrote, as the callers cut the result to 32 bits.
class API_AVAILABLE(macos(26.0)) MetalQuery final : public Query
{
public:
    MetalQuery(MetalDevice& device, QueryType type) : mDevice(device), mType(type) {}
    QueryType type() const override { return mType; }
    bool ready() const override;
    U64 result(bool wait) override;

    MetalDevice& mDevice;
    const QueryType mType;
    std::shared_ptr<MetalQueryRun> mRun; ///< the last begin's; null before the first
};

/// A time query (llrhi.h's Query contract) as two stamps in the device's timestamp heap at
/// pass boundaries. Each begin takes a new pair of the ring and forgets the last result. ready() never waits: true
/// once the submit after the end was signalled and reported, and at once for a run with no result. result(true) sends
/// what is recorded and waits, as Readback::wait. The result is (t1 - t0) * 1e9 / the tick rate in nanoseconds; a run
/// with no valid pair (never begun, still active, refused, abandoned, without a pair, a failed command buffer, a 0 tick
/// or t1 < t0) reads 0 ns and counts in QueryStats::timerInvalid, never LL_ERRS, as a timer only measures.
class API_AVAILABLE(macos(26.0)) MetalTimerQuery final : public Query
{
public:
    explicit MetalTimerQuery(MetalDevice& device) : mDevice(device) {}
    QueryType type() const override { return QueryType::TimeElapsed; }
    bool ready() const override;
    U64 result(bool wait) override;

    MetalDevice& mDevice;
    std::shared_ptr<MetalTimerRun> mRun; ///< the last begin's; null before the first
};

} // namespace LL::RHI

#endif // LL_RHI_METAL

#endif // LL_LLRHIMETAL_OBJC_H
