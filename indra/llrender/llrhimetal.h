/**
 * @file llrhimetal.h
 * @brief The Metal backend's entry point and its build condition
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

#ifndef LL_LLRHIMETAL_H
#define LL_LLRHIMETAL_H

#include "llpreprocessor.h"
#include "llrhi.h"

#include <array>
#include <string>
#include <vector>

// LL_RHI_METAL is 1 where the Metal backend is built: Darwin, arm64 and an SDK with Metal 4
// (MTL4CommandQueue.h is in the macOS 26 SDK). Everywhere else it is 0, and the Metal files build
// nothing. The conditions stay nested, because `defined` in a macro body fails under -Wall
// -Werror (-Wexpansion-to-defined).
#if LL_DARWIN && defined(__arm64__)
#  if __has_include(<Metal/MTL4CommandQueue.h>)
#    define LL_RHI_METAL 1
#  endif
#endif
#ifndef LL_RHI_METAL
#  define LL_RHI_METAL 0
#endif

namespace LL::RHI
{

namespace MSL
{
    struct Program;
    struct Options;
    struct Result;
}

/// What the Mac offers the Metal backend.
struct MetalSupport
{
    bool macOS26 = false; ///< macOS 26.0 or newer
    bool device = false;  ///< MTLCreateSystemDefaultDevice() gives a device
    bool metal4 = false;  ///< the device supports MTLGPUFamilyMetal4
    bool apple7 = false;  ///< the device supports MTLGPUFamilyApple7
    std::string renderer; ///< the device's name; empty without a device
};

/// The Metal backend's one entry point, in llrhimetaldevice-objc.mm, built only where LL_RHI_METAL
/// is 1. Fills support from the one macOS 26 availability check. With create null it only asks
/// (chooseBackend), and makes no Metal 4 object. With create and full support it creates the
/// device, or returns null with its own WARN line (Device::create). Without full support it
/// returns null and leaves the WARN line to Device::create, which has the reason.
Ref<Device> metalBackend(MetalSupport& support, const DeviceCreateInfo* create);

/// The Metal backend's own debug setting: labels on textures and buffers, the binding site's checks
/// of residency, vertex ranges and bindings, the encoder tracker's draw checks (EncoderTracker), and Metal API
/// Validation and shader validation. An
/// explicit setMetalDebug wins, else LL_METAL_DEBUG=1 in the environment, else LL_DEBUG builds. A
/// device follows the value when it is made; the validation variables follow it once, at the first
/// metalBackend call. In llrhimetaldevice-objc.mm.
void setMetalDebug(bool on);
bool metalDebug();

/// Test runs only: LL_METAL_TEST_FAULT while the debug setting is on (the launchers' LL_METAL_DEBUG=1),
/// else empty. "access-revoked@N": frame N's commit feedback reports MTL4CommandQueueErrorAccessRevoked; "frame-hang@N":
/// frame N's commit waits for an event nothing signals, so the GPU never finishes it; both read by a Metal device when
/// it is made. "device-after-window" is the viewer's: its Metal device fails after the window in a normal start. In
/// llrhimetaldevice-objc.mm.
std::string metalTestFault();

/// [Viewer Lab] The main thread writes the failure lines the Metal device's threads hold (GPU errors, the
/// lost device, upload errors, failed programs), with the summaries of the lines counted past each kind's first ones,
/// as at the device's end; the viewer calls it before it stops for a device failure (fastQuit, so the device's own end
/// never comes). Nothing for a device that is not Metal's. In llrhimetaldevice-objc.mm.
void metalWriteFailureLines(Device& device);

/// What the Metal device holds, on one line for the log: MTLDevice's currentAllocatedSize, the releases waiting
/// for the GPU, and the residency set's allocations grouped largest first (MetalResidency::census), the
/// first top groups. Empty for a device that is not Metal's. In llrhimetaldevice-objc.mm.
std::string metalMemoryCensus(Device& device, size_t top);

/// Frame capture for development and tests (the viewer setting MetalFrameCapture asks for it): the next
/// frame a Metal device begins, from its first command buffer through its end's submit, goes to a GPU trace
/// document (a .gputrace folder) at path, which must not exist. The process needs MTL_CAPTURE_ENABLED=1 before
/// its first Metal or GL device; without it the device writes one WARN line and captures nothing. An empty path
/// takes back a request no device has taken. With AfterCurrentFrame the frame that begins after a device's current
/// frame ends is captured instead (the viewer: its first frame holds the whole start-up from the device on, and a
/// capture of it leaves no document). The device logs one INFO line when the
/// capture begins and one when Metal has written the document, or a WARN line when Metal left none at path. In
/// llrhimetaldevice-objc.mm.
enum class MetalFrameCaptureStart
{
    NextFrame,        ///< the next frame a device begins, which may be the one being recorded
    AfterCurrentFrame ///< the frame after the one a device is recording
};
void setMetalFrameCapture(std::string path, MetalFrameCaptureStart start = MetalFrameCaptureStart::NextFrame);

/// GLSL to MSL (llrhimsl.h), as MSL::translate does it.
using MetalTranslator = MSL::Result (*)(const MSL::Program&, const MSL::Options&);

/// The Metal backend's createProgram translates through this function; without one it fails every
/// program. The backend never names MSL::translate itself: whoever creates Metal programs registers
/// it (the tests, level 1, and the viewer's create_viewer_device on the Metal path only, which links
/// glslang and SPIRV-Cross into the viewer on Darwin arm64). In llrhimetalpipeline-objc.mm, built
/// only where LL_RHI_METAL is 1.
void setMetalTranslator(MetalTranslator translator);

#if LL_RHI_METAL
/// [Viewer Lab] What GL's glClear in LLRenderTarget::clear writes, read from the facade: ownGL while
/// LLRenderTarget sends a call of its stack (LLRenderTarget::makesOwnGL), the colour mask LLRender::setColorMask
/// keeps, and the depth write mask of the LLGLDepthTest scopes. MetalEncoder::clear asks it, so no Metal file
/// includes a facade header. The record follows only those two facades: a pipeline or a depth state set
/// directly on the interface inside a stack pass is not in it (GL's setPipeline writes gGL's mask beside the
/// depth state); no caller does that. In llrendertarget.cpp; GL never calls it.
struct MetalFacadeClear
{
    bool                ownGL = false;
    std::array<bool, 4> color{}; ///< red, green, blue, alpha
    bool                depthWrite = false;
};
MetalFacadeClear metalFacadeClear();

/// [Viewer Lab] The Metal surface's counters and its layer's state, for the tests and the golden hook. A
/// present() either presents a drawable, finds a 0 in the surface's size, leaves the frame in the back buffer (VSync
/// off: while the window server has a drawable queued, the window is hidden or the layer has just refused one) or
/// gets no drawable within the layer's timeout; once the frame's command buffer is sent, presents ==
/// drawablesPresented + presentsSkippedZero + presentsPaced + drawableTimeouts, and each presented drawable had one
/// wait and one signal on the queue.
struct MetalSurfaceStats
{
    U64  presents            = 0;     ///< present() calls
    U64  drawablesPresented  = 0;     ///< drawables drawn into and presented, at the submit that sent their flip pass
    U64  presentsSkippedZero = 0;     ///< presents without a drawable, because the surface's size has a 0
    U64  presentsPaced       = 0;     ///< presents left in the back buffer with VSync off (metalPresentPaced and
                                      ///< the two below)
    U64  presentsWindowHidden = 0;    ///< of them, while none of the window showed (its occlusion state)
    U64  presentsAfterTimeout = 0;    ///< of them, after a drawable timeout, before a presented handler or the retry
    U32  drawablesWithWindowServer = 0; ///< presented with VSync off, their presented handler not run yet
    U64  drawableTimeouts    = 0;     ///< presents whose nextDrawable gave nil (its 1 s timeout)
    U64  nextDrawableNs      = 0;     ///< the main thread's time in nextDrawable, in all
    U64  nextDrawableLongNs  = 0;     ///< the longest one nextDrawable call took
    U64  nextDrawableOver1ms = 0;     ///< nextDrawable calls that took more than 1 ms
    U64  drawableWaits       = 0;     ///< waitForDrawable: on the queue
    U64  drawableSignals     = 0;     ///< signalDrawable: on the queue
    U64  finishedFlips       = 0;     ///< VSync off presents on the finished path whose frame went into a finished
                                      ///< frame (MetalSurface::setPresentDirect)
    U64  finishedSkipped     = 0;     ///< the finished path's presents that found no finished frame free: not shown
    U64  finishedPresented   = 0;     ///< drawables the present queue filled from a finished frame (of
                                      ///< drawablesPresented)
    U64  finishedHeld        = 0;     ///< times a finished frame was not given to the window server when it could
                                      ///< have been, by metalPresentPaced or presentWithheld (a newer one goes later)
    U64  finishedTimeouts    = 0;     ///< of drawableTimeouts, the finished path's
    bool displaySync         = false; ///< the VSync setting (setVSync); off, present() paces by the window
                                      ///< server's drawables (metalPresentPaced)
    bool layerDisplaySync    = false; ///< the layer's displaySyncEnabled: on whatever the setting, off only with
                                      ///< LL_METAL_PRESENT_UNSYNCED and VSync off
    U32  drawableWidth       = 0;     ///< the layer's drawableSize
    U32  drawableHeight      = 0;
    F32  contentsScale       = 0.f; ///< the layer's contentsScale
    bool opaque              = false; ///< the layer's opaque
    bool colorspace          = false; ///< the layer's colorspace is non-nil, i.e. its frames are colour matched
    U32  pixelFormat         = 0;     ///< the layer's pixelFormat, an MTLPixelFormat value (BGRA8Unorm is 80)
};

/// The device's surface's counters; all 0 for a device that is not Metal's or has no surface. Main thread. In
/// llrhimetalsurface-objc.mm.
MetalSurfaceStats metalSurfaceStats(Device& device);

/// [Viewer Lab] The Metal encoder's pass breaks, for the tests and the golden hook: Metal 4 has no copy on a render
/// encoder, so work between draws of a pass ends the pass's open segment.
struct MetalEncoderStats
{
    /// splitPass calls inside a pass that closed an open segment: close() ended a native encoder or flushed the
    /// segment's clears. A split with nothing recorded to close is not one.
    U64 passBreaks = 0;
    /// uploadTexture calls inside a pass, whether or not a segment was open to close.
    U64 uploadBreaks = 0;
    /// uploadTexture calls inside a pass whose split closed an open segment: the upload itself broke the pass. Each
    /// is also one of passBreaks and one of uploadBreaks.
    U64 uploadPassBreaks = 0;
    /// The native render encoders that were sent at least one setDepthClipMode (GL_DEPTH_CLAMP), and the
    /// sends. Metal API Validation reports every send as unused, though a draw used it, so golden level 1 counts the
    /// reports against the sends; the encoders are information.
    U64 depthClipEncoders = 0;
    U64 depthClipSends = 0;
    /// The samplers a draw gave the texture of their own type's slot on a channel whose current texture was another
    /// (of another type, or none after a null tell). Information.
    U64 typeSlotReads = 0;
    /// The setDepthBias calls on a native render encoder, each a change of its bias (units, slope); the
    /// golden hook counts them over the warm-up. Information beside the clip sends.
    U64 depthBiasChanges = 0;
    /// The time queries' begins and ends that split the pass to put their stamp at a boundary
    /// (an open native encoder ended); each is also one of passBreaks. None for a begin or end with no encoder open.
    U64 timerSplits = 0;
};

/// The device's encoder's pass breaks; all 0 for a device that is not Metal's. Main thread. In
/// llrhimetalsurface-objc.mm.
MetalEncoderStats metalEncoderStats(Device& device);

/// [Viewer Lab] The upload thread's queue on Metal, for the tests and the golden hook.
struct MetalUploadCounts
{
    U64 submits = 0;             ///< the upload thread's submits that committed a command buffer to its queue
    U64 uploads = 0;             ///< the upload thread's uploadTexture calls encoded on its queue
    U64 deferredMips = 0;        ///< the upload thread's generateMipmaps calls, noted for the fence's waitForFence
    U64 signalledAtHandover = 0; ///< waitForFence on an upload fence that had signalled: no wait
    U64 queueWaits = 0;          ///< waitForFence on an upload fence that had not: the main queue waits on the GPU
    U64 mainThreadWaits = 0;     ///< Fence::wait() on an upload fence on the main thread (the swap makes none)
    U64 errors = 0;              ///< upload command buffers that failed (one WARN line each)
};

/// The device's upload counters; all 0 for a device that is not Metal's. Any thread. In llrhimetalresource-objc.mm.
MetalUploadCounts metalUploadCounts(Device& device);

/// [Viewer Lab] What a calling thread waited for: a program's libraries, a point variant, or a draw's render pipeline
/// state by its kind: a full state or a full state's variant without alpha.
enum class MetalWaitKind : U8
{
    Program,
    Point,
    Full,
    Variant,
};
inline constexpr size_t METAL_WAIT_KINDS = size_t(MetalWaitKind::Variant) + 1;

/// [Viewer Lab] One MetalWaitKind's waits that waited, in microseconds.
struct MetalWaits
{
    U64 count = 0;
    U64 totalUs = 0;
    U64 longestUs = 0;
};

/// [Viewer Lab] The Metal device's compilations since it was made, for the golden hook, the viewer's line 30 s after
/// STATE_STARTED and the tests. The counts are the render pipeline states sent to Metal's compiler, a full state per
/// program, attributes, primitive class, sample count and attachments; a state that later goes with its program stays
/// counted. The waits are the calling threads' time blocked on compilations, in microseconds: information, as Metal's
/// own caches decide them.
struct MetalCompileStats
{
    U64 full          = 0; ///< full states sent
    U64 alphaVariants = 0; ///< full states without alpha, for attachments that have none in GL
    U64 refused       = 0; ///< keys refused, each with its one WARN line
    U64 cached        = 0; ///< PipelineCache's pipelines now
    U64 callerTranslations  = 0; ///< translations on the calling thread: the login programs and wait()'s stolen jobs
    U64 callerTranslationUs = 0; ///< their time
    U64 programWaitUs       = 0; ///< Program::wait's wait for the libraries, after its stolen translations
    U64 pipelineWaitUs      = 0; ///< draws' waits for a pending pipeline state or alpha variant
    U64 pointWaitUs         = 0; ///< the point variants' waits
    U64 failedPrograms      = 0; ///< program jobs that failed, not cancelled
    /// Creations a test fault failed (takeProgramFault or takeMetalProgramFault), which failedPrograms never
    /// counts, and the Pending answers a deferred one gave before it read as Failed (one per such creation at most).
    U64 faultedPrograms     = 0;
    U64 faultPending        = 0;
    /// The waits above by what they waited for, each with its longest single wait.
    std::array<MetalWaits, METAL_WAIT_KINDS> waits{};
    U64 groupsSent        = 0; ///< program groups' libraries sent to Metal's compiler
    U64 groupsFromMain    = 0; ///< of those, sent from the main thread; the others from the pool's threads
    U64 groupsDone        = 0; ///< group library handlers that have run
    U64 groupLatencyUs    = 0; ///< their time from the send to the handler's start, in all
    U64 groupLatencyMaxUs = 0; ///< and the longest
    U32 groupHandlerQos   = 0; ///< the QoS classes the handlers ran at (qos_class_self), a bit each (metalQosText)
    U64 groupHash         = 0; ///< the sum of the sent groups' source digests (the same groups with or without stealing)
    U32 poolQos           = 0; ///< the translation threads' QoS class (qos_class_t)
    bool compileSlow      = false; ///< LL_METAL_COMPILE_SLOW: the pool at utility QoS and no stealing (the control run)
};

/// [Viewer Lab] A single wait this long or longer is a stall: the main thread pumps the network, and OpenSim
/// drops a client after 60 s without its packets, so the viewer writes its MetalCompileStall line at once.
inline constexpr U64 METAL_COMPILE_STALL_US = 10000000;

/// [Viewer Lab] Whether one wait in stats took thresholdUs or longer. In llrhimetalsurface-objc.mm.
bool metalCompileStalled(const MetalCompileStats& stats, U64 thresholdUs = METAL_COMPILE_STALL_US);

/// [Viewer Lab] The compile counts as text for the viewer's MetalCompileStall line: the waits by kind (count, total
/// and longest in ms), the caller's translations, the groups (sent, from the main thread, latency, the handlers' QoS,
/// the hash), the pool's QoS and the knob. In llrhimetalsurface-objc.mm.
std::string metalCompileStallText(const MetalCompileStats& stats);

/// [Viewer Lab] The name of a QoS class (qos_class_t's value), or the names of the classes in a mask of
/// MetalCompileStats::groupHandlerQos. In llrhimetalsurface-objc.mm.
std::string metalQosText(U32 qosClass);
std::string metalQosMaskText(U32 mask);
/// The mask bit of a QoS class (qos_class_t's value) in MetalCompileStats::groupHandlerQos.
U32 metalQosBit(U32 qosClass);

/// The device's compilations; all 0 for a device that is not Metal's. Main thread. In llrhimetalsurface-objc.mm.
MetalCompileStats metalCompileStats(Device& device);

/// [Viewer Lab] METAL_SAFE_MATH_PROGRAMS, for measured.rhi.compile, which golden level 2 reads. In
/// llrhimetalpipeline-objc.mm.
std::vector<std::string> metalSafeMathPrograms();

/// [Viewer Lab] Golden level 2's fault list on Metal: more test faults beside the shared ProgramFault (llrhi.h), which
/// GL keeps alone, so one run can fail several programs, each with its own count. Each entry works as ProgramFault
/// does: an exact name, times 0 fails every creation, times N fails N in a row, then one builds and the count starts
/// again. createMetalProgram asks takeProgramFault first, so a name in both is taken by the shared fault only. Main
/// thread only. In llrhimetalpipeline-objc.mm.
/// Arms the list (an empty one is off) and resets its hits and counts.
void setMetalProgramFaults(const std::vector<ProgramFault>& faults);
/// The creations the list's fault on the program has failed since the list was armed; 0 for a name not in the list.
U32 metalProgramFaultHits(const std::string& program);
/// For createMetalProgram, after takeProgramFault, as takeProgramFault counts: true, with deferred set from the entry,
/// when the list's fault on the program takes this creation.
bool takeMetalProgramFault(const std::string& program, bool& deferred);

/// [Viewer Lab] The samplers the device has made (one per distinct description, reused after); 0 for a
/// device that is not Metal's. For the tests and golden level 1. In llrhimetalsurface-objc.mm.
size_t metalSamplerCount(Device& device);

/// Tests and the golden hook only: on sets the layer's framebufferOnly NO, so its drawables can be
/// read, and asks for a copy of the next presented drawable, made in the command buffer of its flip pass; off sets it
/// YES again and drops the copy. framebufferOnly changes only what the CPU and copies may do with a drawable, not a
/// pixel (CAMetalLayer.h:79-87). Nothing for a device that is not Metal's or has no surface. Main thread.
void setMetalDrawableReadback(Device& device, bool on);
/// VSync off's present path (MetalSurface::setPresentDirect): false, the default, gives the window server the newest
/// frame the GPU has finished through a second queue; true presents each frame's own drawable from its command buffer
/// (LuminousMetalPresentDirect, for a comparison run). Nothing for a device that is not Metal's or has no surface.
/// Main thread.
void setMetalPresentDirect(Device& device, bool direct);
/// The last copy setMetalDrawableReadback asked for: waits for its command buffer (sent first when it is still being
/// recorded) and gives the drawable's pixels as RGBA8, rows from the top of the drawable down (its memory order), with
/// its size. False, with pixels empty, when there is no copy or its command buffer failed. Main thread.
bool readMetalDrawable(Device& device, std::vector<U8>& pixels, U32& width, U32& height);
#endif

} // namespace LL::RHI

#endif // LL_LLRHIMETAL_H
